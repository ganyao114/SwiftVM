#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

// Independent foundation. No current Runtime/JIT/interpreter path uses this
// ledger; it does not fix their existing TLS-based abandonment protocol.
namespace swift::runtime::memory {

struct FrameTag;
struct LeaseTag;
struct WriterTag;
template<class Tag> struct Handle {
    std::uint32_t slot{std::numeric_limits<std::uint32_t>::max()};
    std::uint64_t generation{};
    std::uint64_t domain{};
    explicit operator bool() const noexcept { return generation != 0; }
    bool operator==(const Handle&) const = default;
};
using FrameHandle = Handle<FrameTag>;
using LeaseHandle = Handle<LeaseTag>;
using WriterHandle = Handle<WriterTag>;

// nullptr selects the anonymous counter; otherwise the pointed-to private
// counter must be registered with, and outlive every use by, the adapter.
struct Endpoint {
    std::atomic<unsigned>* private_active{};
    std::atomic<unsigned>* private_pins{};
    std::uint64_t domain{};
};

class Admission {
public:
    virtual ~Admission() = default;
    virtual std::uint64_t DomainIdentity() const noexcept = 0;
    virtual void ValidateEndpoint(Endpoint) = 0;
    virtual void AttachLedger() = 0;
    virtual void DetachLedger() noexcept = 0;
    // Pins remain held while a live lease is suspended and uncounted.
    virtual void Pin(Endpoint) noexcept = 0;
    virtual void Unpin(Endpoint) noexcept = 0;
    // Strong guarantee: on throw, no reader contribution / gate ownership.
    virtual void Admit(Endpoint) = 0;
    virtual void AcquireGate() = 0;
    // May throw while the caller still owns the gate.
    virtual void Drain() = 0;
    virtual void Withdraw(Endpoint) noexcept = 0;
    virtual void ReleaseGate() noexcept = 0;
    // Recovery must not allocate or throw. May wait for another writer.
    virtual void Readmit(Endpoint) noexcept = 0;
};

class ProtocolError final : public std::exception {
public:
    const char* what() const noexcept override { return "memory admission timed out or counter saturated"; }
};

// Concrete testable adapter for the same publication / gate / drain shape as
// the existing memory protocol. It owns no guest bytes and invokes no callbacks.
class AdmissionProtocol final : public Admission {
public:
    class alignas(64) Participant final {
    public:
        explicit Participant(AdmissionProtocol&);
        ~Participant();
        Participant(const Participant&) = delete;
        Participant& operator=(const Participant&) = delete;
        Endpoint GetEndpoint() noexcept { return {&active, &pins, protocol.DomainIdentity()}; }
        unsigned Active() const noexcept { return active.load(std::memory_order_seq_cst); }
    private:
        friend class AdmissionProtocol;
        AdmissionProtocol& protocol;
        std::atomic<unsigned> active{};
        std::atomic<unsigned> pins{};
        Participant* next{};
    };

    AdmissionProtocol(std::atomic<unsigned>& gate, std::atomic<unsigned>& anonymous,
                      std::chrono::milliseconds timeout = std::chrono::seconds{5});
    ~AdmissionProtocol();
    AdmissionProtocol(const AdmissionProtocol&) = delete;
    AdmissionProtocol& operator=(const AdmissionProtocol&) = delete;
    std::uint64_t DomainIdentity() const noexcept override { return identity; }
    void ValidateEndpoint(Endpoint) override;
    void AttachLedger() override;
    void DetachLedger() noexcept override;
    void Pin(Endpoint) noexcept override;
    void Unpin(Endpoint) noexcept override;
    void Admit(Endpoint) override;
    void AcquireGate() override;
    void Drain() override;
    void Withdraw(Endpoint) noexcept override;
    void ReleaseGate() noexcept override;
    void Readmit(Endpoint) noexcept override;
private:
    void AdmitImpl(Endpoint, bool bounded);
    std::atomic<unsigned>& Counter(Endpoint) noexcept;
    std::atomic<unsigned>& gate;
    std::atomic<unsigned>& anonymous;
    const std::uint64_t identity;
    std::chrono::milliseconds timeout;
    std::mutex participants_mutex;
    Participant* participants{};
    std::mutex ledgers_mutex;
    std::vector<std::thread::id> attached_threads;
    std::atomic<unsigned> endpoint_pins{};
};

// One owner host thread. Handles contain no owning pointers. All state needed
// after abandoned helper frames stays in these stable records, not in a scope.
// Guest faults may abandon user code AFTER EnterWriter/Acquire returns. The
// ledger itself is neither async-signal-safe nor an arbitrary interruption API.
class LeaseLedger final {
public:
    explicit LeaseLedger(Admission&, std::uint64_t generation_limit = UINT64_MAX);
    ~LeaseLedger();
    LeaseLedger(const LeaseLedger&) = delete;
    LeaseLedger& operator=(const LeaseLedger&) = delete;
    FrameHandle OpenFrame(FrameHandle parent = {});
    void CloseFrame(FrameHandle); // refuses live descendants / resources
    LeaseHandle Acquire(FrameHandle, Endpoint = {});
    bool Release(LeaseHandle) noexcept; // stale/repeated handle is a no-op
    // Prepare stable storage and the entire suspension set before withdrawing
    // any reader. No gate operation until EnterWriter validates this handle.
    WriterHandle BeginWriter(FrameHandle);
    void EnterWriter(WriterHandle);
    bool FinishWriter(WriterHandle) noexcept;
    // Drops boundary resources and descendant frames, keeps boundary open,
    // restores surviving ancestor leases. No allocation; repeatable; not for
    // signal handlers. An unrelated writer is never released.
    bool RecoverFrame(FrameHandle) noexcept;
    bool IsLive(FrameHandle) const noexcept;
    bool IsLive(LeaseHandle) const noexcept;
    bool IsCounted(LeaseHandle) const noexcept;
    bool OwnsGate(WriterHandle) const noexcept;
    std::size_t LiveReaders() const noexcept;
private:
    struct FrameRecord {
        std::uint64_t generation{};
        bool live{}, retired{};
        FrameHandle parent;
    };
    struct LeaseRecord {
        std::uint64_t generation{};
        bool live{}, retired{}, counted{};
        FrameHandle frame;
        Endpoint endpoint;
        WriterHandle suspended_by;
    };
    enum class Phase { Prepared, Suspended, GateHeld, Ready };
    struct WriterRecord {
        std::uint64_t generation{};
        bool live{}, retired{}, owns_gate{};
        FrameHandle frame;
        Phase phase{Phase::Prepared};
        std::vector<LeaseHandle> suspended;
    };
    template<class Tag, class Record>
    Handle<Tag> Allocate(std::deque<Record>&);
    template<class Tag, class Record>
    Record* Find(std::deque<Record>&, Handle<Tag>) noexcept;
    template<class Tag, class Record>
    const Record* Find(const std::deque<Record>&, Handle<Tag>) const noexcept;
    void CheckOwner() const;
    void RequireOwner() const noexcept;
    bool Within(FrameHandle, FrameHandle) const noexcept;
    bool ReleaseImpl(LeaseHandle) noexcept;
    bool FinishImpl(WriterHandle) noexcept;
    Admission& admission;
    const std::uint64_t identity;
    const std::thread::id owner;
    const std::uint64_t generation_limit;
    std::deque<FrameRecord> frames;
    std::deque<LeaseRecord> leases;
    std::deque<WriterRecord> writers;
    WriterHandle pending_writer;
};

static_assert(std::atomic<unsigned>::is_always_lock_free);
} // namespace swift::runtime::memory
