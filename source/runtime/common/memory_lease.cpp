#include "memory_lease.h"

#include <stdexcept>
#include <algorithm>
#include <utility>

namespace swift::runtime::memory {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<std::uint64_t> next_identity{1};
std::uint64_t NewIdentity() {
    auto current = next_identity.load(std::memory_order_relaxed);
    for (;;) {
        if (current == UINT64_MAX) throw std::overflow_error("memory admission identity exhausted");
        if (next_identity.compare_exchange_weak(current, current + 1, std::memory_order_relaxed)) return current;
    }
}
void IncrementPin(std::atomic<unsigned>& value) noexcept {
    auto current = value.load();
    for (;;) {
        if (current == std::numeric_limits<unsigned>::max()) std::terminate();
        if (value.compare_exchange_weak(current, current + 1)) return;
    }
}
struct Binding { const void* gate; const void* anonymous; };
std::mutex bindings_mutex;
std::vector<Binding> bindings;
void Wait(bool bounded, Clock::time_point deadline) {
    if (bounded && Clock::now() >= deadline) throw ProtocolError{};
    std::this_thread::yield();
}
}

AdmissionProtocol::AdmissionProtocol(std::atomic<unsigned>& gate_, std::atomic<unsigned>& anonymous_,
                                    std::chrono::milliseconds timeout_)
    : gate(gate_), anonymous(anonymous_), identity(NewIdentity()), timeout(timeout_) {
    if (timeout <= std::chrono::milliseconds::zero()) throw std::invalid_argument("positive admission timeout required");
    if (&gate == &anonymous) throw std::invalid_argument("memory gate cannot alias reader counter");
    std::lock_guard lock{bindings_mutex};
    for (const auto& binding : bindings)
        if (binding.gate == &gate || binding.anonymous == &anonymous || binding.gate == &anonymous || binding.anonymous == &gate)
        throw std::invalid_argument("memory gate and readers require one admission domain");
    bindings.push_back({&gate, &anonymous});
}
AdmissionProtocol::~AdmissionProtocol() {
    if (participants || !attached_threads.empty() || endpoint_pins.load()) std::terminate();
    std::lock_guard lock{bindings_mutex};
    const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) { return binding.gate == &gate; });
    if (found == bindings.end()) std::terminate();
    bindings.erase(found);
}
void AdmissionProtocol::AttachLedger() {
    std::lock_guard lock{ledgers_mutex};
    const auto owner = std::this_thread::get_id();
    if (std::find(attached_threads.begin(), attached_threads.end(), owner) != attached_threads.end())
        throw std::logic_error("one host thread must share one ledger per admission domain");
    attached_threads.push_back(owner);
}
void AdmissionProtocol::DetachLedger() noexcept {
    std::lock_guard lock{ledgers_mutex};
    const auto found = std::find(attached_threads.begin(), attached_threads.end(), std::this_thread::get_id());
    if (found == attached_threads.end()) std::terminate();
    attached_threads.erase(found);
}
void AdmissionProtocol::ValidateEndpoint(Endpoint endpoint) {
    if (!endpoint.private_active && !endpoint.private_pins && !endpoint.domain) return;
    if (endpoint.domain != identity) throw std::invalid_argument("foreign memory endpoint domain");
    std::lock_guard lock{participants_mutex};
    for (auto* participant = participants; participant; participant = participant->next)
        if (&participant->active == endpoint.private_active && &participant->pins == endpoint.private_pins) return;
    throw std::invalid_argument("unregistered memory endpoint");
}
void AdmissionProtocol::Pin(Endpoint endpoint) noexcept {
    if (!!endpoint.private_active != !!endpoint.private_pins) std::terminate();
    if (endpoint.private_pins) IncrementPin(*endpoint.private_pins);
    IncrementPin(endpoint_pins);
}
void AdmissionProtocol::Unpin(Endpoint endpoint) noexcept {
    if (endpoint.private_pins && !endpoint.private_pins->fetch_sub(1)) std::terminate();
    if (!endpoint_pins.fetch_sub(1)) std::terminate();
}
AdmissionProtocol::Participant::Participant(AdmissionProtocol& protocol_) : protocol(protocol_) {
    std::lock_guard lock{protocol.participants_mutex};
    next = protocol.participants; protocol.participants = this;
}
AdmissionProtocol::Participant::~Participant() {
    std::lock_guard lock{protocol.participants_mutex};
    if (active.load(std::memory_order_seq_cst) || pins.load()) std::terminate();
    auto** link = &protocol.participants;
    while (*link != this) link = &(*link)->next;
    *link = next;
}
std::atomic<unsigned>& AdmissionProtocol::Counter(Endpoint endpoint) noexcept {
    return endpoint.private_active ? *endpoint.private_active : anonymous;
}
void AdmissionProtocol::AdmitImpl(Endpoint endpoint, bool bounded) {
    auto& counter = Counter(endpoint);
    const auto deadline = Clock::now() + timeout;
    for (;;) {
        auto count = counter.load(std::memory_order_seq_cst);
        for (;;) {
            if (count == std::numeric_limits<unsigned>::max()) {
                Wait(bounded, deadline); count = counter.load(std::memory_order_seq_cst); continue;
            }
            if (counter.compare_exchange_weak(count, count + 1, std::memory_order_seq_cst)) break;
            Wait(bounded, deadline);
        }
        if (!gate.load(std::memory_order_seq_cst)) return;
        counter.fetch_sub(1, std::memory_order_seq_cst);
        while (gate.load(std::memory_order_acquire)) Wait(bounded, deadline);
    }
}
void AdmissionProtocol::Admit(Endpoint endpoint) { ValidateEndpoint(endpoint); AdmitImpl(endpoint, true); }
void AdmissionProtocol::Readmit(Endpoint endpoint) noexcept { AdmitImpl(endpoint, false); }
void AdmissionProtocol::Withdraw(Endpoint endpoint) noexcept {
    if (!Counter(endpoint).fetch_sub(1, std::memory_order_seq_cst)) std::terminate();
}
void AdmissionProtocol::AcquireGate() {
    const auto deadline = Clock::now() + timeout;
    unsigned expected{};
    while (!gate.compare_exchange_weak(expected, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
        expected = 0; Wait(true, deadline);
    }
}
void AdmissionProtocol::Drain() {
    const auto deadline = Clock::now() + timeout;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    for (;;) {
        bool busy = anonymous.load(std::memory_order_seq_cst) != 0;
        {
            // Never hold the registry lock while waiting for a reader: a
            // nested bounded Admit must still be able to validate endpoints.
            std::lock_guard lock{participants_mutex};
            for (auto* participant = participants; participant; participant = participant->next)
                busy |= participant->active.load(std::memory_order_seq_cst) != 0;
        }
        if (!busy) return;
        Wait(true, deadline);
    }
}
void AdmissionProtocol::ReleaseGate() noexcept { gate.store(0, std::memory_order_release); }

LeaseLedger::LeaseLedger(Admission& admission_, std::uint64_t generation_limit_)
    : admission(admission_), identity(NewIdentity()), owner(std::this_thread::get_id()), generation_limit(generation_limit_) {
    if (!generation_limit) throw std::invalid_argument("positive generation limit required");
    admission.AttachLedger();
}
LeaseLedger::~LeaseLedger() {
    RequireOwner();
    // Destruction discards every lease before opening a held writer gate, so
    // it never readmits resources that are about to be destroyed.
    for (std::uint32_t i = 0; i < leases.size(); ++i)
        if (leases[i].live) ReleaseImpl({i, leases[i].generation, identity});
    if (pending_writer) FinishImpl(pending_writer);
    admission.DetachLedger();
}
void LeaseLedger::CheckOwner() const {
    if (std::this_thread::get_id() != owner) throw std::logic_error("memory ledger requires its owner thread");
}
void LeaseLedger::RequireOwner() const noexcept {
    if (std::this_thread::get_id() != owner) std::terminate();
}
template<class Tag, class Record>
Handle<Tag> LeaseLedger::Allocate(std::deque<Record>& records) {
    for (std::uint32_t i = 0; i < records.size(); ++i) {
        auto& record = records[i];
        if (record.live || record.retired) continue;
        if (record.generation == generation_limit) { record.retired = true; continue; }
        ++record.generation; record.live = true;
        return {i, record.generation, identity};
    }
    if (records.size() >= std::numeric_limits<std::uint32_t>::max()) throw std::length_error("memory ledger slot limit");
    records.emplace_back();
    records.back().generation = 1; records.back().live = true;
    return {static_cast<std::uint32_t>(records.size() - 1), 1, identity};
}
template<class Tag, class Record>
Record* LeaseLedger::Find(std::deque<Record>& records, Handle<Tag> handle) noexcept {
    if (handle.domain != identity || handle.slot >= records.size()) return nullptr;
    auto& record = records[handle.slot];
    return record.live && record.generation == handle.generation ? &record : nullptr;
}
template<class Tag, class Record>
const Record* LeaseLedger::Find(const std::deque<Record>& records, Handle<Tag> handle) const noexcept {
    if (handle.domain != identity || handle.slot >= records.size()) return nullptr;
    const auto& record = records[handle.slot];
    return record.live && record.generation == handle.generation ? &record : nullptr;
}
FrameHandle LeaseLedger::OpenFrame(FrameHandle parent) {
    CheckOwner();
    if (pending_writer) throw std::logic_error("cannot reenter a ledger with a pending writer");
    if (parent && !Find(frames, parent)) throw std::invalid_argument("invalid parent memory frame");
    auto handle = Allocate<FrameTag>(frames); frames[handle.slot].parent = parent;
    return handle;
}
bool LeaseLedger::Within(FrameHandle frame, FrameHandle boundary) const noexcept {
    // Recovery may retire descendants during this scan. Their immutable
    // generation/parent chain remains available until a subsequent allocation.
    for (std::size_t steps = 0; frame && steps < frames.size(); ++steps) {
        if (frame == boundary) return true;
        if (frame.domain != identity || frame.slot >= frames.size() || frames[frame.slot].generation != frame.generation) return false;
        frame = frames[frame.slot].parent;
    }
    return false;
}
void LeaseLedger::CloseFrame(FrameHandle frame) {
    CheckOwner();
    auto* record = Find(frames, frame);
    if (!record) throw std::invalid_argument("invalid memory frame");
    for (std::uint32_t i = 0; i < frames.size(); ++i)
        if (frames[i].live && i != frame.slot && Within({i, frames[i].generation, identity}, frame))
            throw std::logic_error("memory frame has live descendants");
    for (const auto& lease : leases) if (lease.live && lease.frame == frame)
        throw std::logic_error("memory frame has live readers");
    if (auto* writer = Find(writers, pending_writer); writer && writer->frame == frame)
        throw std::logic_error("memory frame has a live writer");
    record->live = false;
}
LeaseHandle LeaseLedger::Acquire(FrameHandle frame, Endpoint endpoint) {
    CheckOwner();
    if (!Find(frames, frame)) throw std::invalid_argument("invalid reader memory frame");
    if (pending_writer) throw std::logic_error("cannot admit a reader with a pending writer");
    admission.ValidateEndpoint(endpoint);
    auto handle = Allocate<LeaseTag>(leases);
    auto& record = leases[handle.slot];
    record.frame = frame; record.endpoint = endpoint; record.counted = false; record.suspended_by = {};
    admission.Pin(endpoint);
    try { admission.Admit(endpoint); record.counted = true; }
    catch (...) { admission.Unpin(endpoint); record.live = false; throw; }
    return handle;
}
bool LeaseLedger::ReleaseImpl(LeaseHandle handle) noexcept {
    auto* record = Find(leases, handle);
    if (!record) return false;
    if (record->counted) admission.Withdraw(record->endpoint);
    admission.Unpin(record->endpoint);
    record->counted = false; record->live = false; record->suspended_by = {};
    return true;
}
bool LeaseLedger::Release(LeaseHandle handle) noexcept { RequireOwner(); return ReleaseImpl(handle); }
WriterHandle LeaseLedger::BeginWriter(FrameHandle frame) {
    CheckOwner();
    if (!Find(frames, frame)) throw std::invalid_argument("invalid writer memory frame");
    if (pending_writer) throw std::logic_error("a memory writer is already pending");
    // Allocate the complete set before any admission is revoked. If either
    // allocation fails, no counter, gate or pending-writer state has changed.
    std::vector<LeaseHandle> suspended;
    suspended.reserve(LiveReaders());
    for (std::uint32_t i = 0; i < leases.size(); ++i)
        if (leases[i].live && leases[i].counted) suspended.push_back({i, leases[i].generation, identity});
    auto handle = Allocate<WriterTag>(writers);
    auto& record = writers[handle.slot];
    record.frame = frame; record.phase = Phase::Prepared; record.owns_gate = false;
    record.suspended.swap(suspended); pending_writer = handle;
    return handle;
}
void LeaseLedger::EnterWriter(WriterHandle handle) {
    CheckOwner();
    auto* record = Find(writers, handle);
    if (!record || handle != pending_writer || !Find(frames, record->frame))
        throw std::invalid_argument("invalid memory writer");
    if (record->phase == Phase::Ready) return;
    if (record->phase == Phase::Prepared) {
        for (const auto lease : record->suspended) if (auto* reader = Find(leases, lease)) {
            admission.Withdraw(reader->endpoint); reader->counted = false; reader->suspended_by = handle;
        }
        record->phase = Phase::Suspended;
    }
    if (!record->owns_gate) { admission.AcquireGate(); record->owns_gate = true; record->phase = Phase::GateHeld; }
    admission.Drain(); record->phase = Phase::Ready;
}
bool LeaseLedger::FinishImpl(WriterHandle handle) noexcept {
    auto* record = Find(writers, handle);
    if (!record || handle != pending_writer) return false;
    if (record->owns_gate) { admission.ReleaseGate(); record->owns_gate = false; }
    for (const auto lease : record->suspended) if (auto* reader = Find(leases, lease)) {
        if (reader->suspended_by != handle) continue;
        admission.Readmit(reader->endpoint); reader->counted = true; reader->suspended_by = {};
    }
    record->live = false; pending_writer = {};
    return true;
}
bool LeaseLedger::FinishWriter(WriterHandle handle) noexcept { RequireOwner(); return FinishImpl(handle); }
bool LeaseLedger::RecoverFrame(FrameHandle boundary) noexcept {
    RequireOwner();
    if (!Find(frames, boundary)) return false;
    // Discard abandoned records first; FinishImpl then restores only survivors.
    for (std::uint32_t i = 0; i < leases.size(); ++i)
        if (leases[i].live && Within(leases[i].frame, boundary)) ReleaseImpl({i, leases[i].generation, identity});
    if (auto* writer = Find(writers, pending_writer); writer && Within(writer->frame, boundary))
        FinishImpl(pending_writer);
    for (std::uint32_t i = 0; i < frames.size(); ++i)
        if (frames[i].live && i != boundary.slot && Within({i, frames[i].generation, identity}, boundary)) frames[i].live = false;
    return true;
}
bool LeaseLedger::IsLive(FrameHandle handle) const noexcept { RequireOwner(); return Find(frames, handle); }
bool LeaseLedger::IsLive(LeaseHandle handle) const noexcept { RequireOwner(); return Find(leases, handle); }
bool LeaseLedger::IsCounted(LeaseHandle handle) const noexcept {
    RequireOwner(); const auto* record = Find(leases, handle); return record && record->counted;
}
bool LeaseLedger::OwnsGate(WriterHandle handle) const noexcept {
    RequireOwner(); const auto* record = Find(writers, handle); return record && record->owns_gate;
}
std::size_t LeaseLedger::LiveReaders() const noexcept {
    RequireOwner(); std::size_t count{}; for (const auto& record : leases) count += record.live; return count;
}
} // namespace swift::runtime::memory
