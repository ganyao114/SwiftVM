#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

// Registry foundation only: this does not implement guest exclusive accesses.
// The caller must admit ordinary writers and hold a writer gate + drain for
// Register and the entire BeginConsume/check/notify/physical-commit interval.
// Never call guest memory access or callbacks while a registry mutex is held.
namespace swift::runtime::exclusive {

// Canonical backing namespace/object, NOT a guest address-space or host VA.
// Different mappings of the same bytes must supply the same identity+offset.
// Canonical offset zero must preserve a 16-byte-aligned backing origin.
struct BackingIdentity {
    std::uint64_t domain{};
    std::uint64_t object{};
    bool operator==(const BackingIdentity&) const = default;
};
struct Address {
    BackingIdentity backing;
    std::uint64_t offset{};
    std::uint64_t mapping_generation{};
    bool operator==(const Address&) const = default;
};
struct Shape {
    std::uint8_t lane_bytes{};
    bool pair{};
    bool operator==(const Shape&) const = default;
};

namespace detail { struct RegistryState; struct Watch; }
class Registry;
class Consumption;

// Owned by one guest thread/State, not by host TLS. Only RequestCancel may be
// called concurrently. All other operations require exclusive owner access.
// Keep the token alive until signal/fault handlers can no longer reference it.
class Token final {
public:
    Token() = default;
    ~Token();
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
    Token(Token&&) = delete;
    Token& operator=(Token&&) = delete;

    // Signal-safe flag only; keeps the ticket counted until safe cleanup.
    void RequestCancel() noexcept { cancelled.store(true, std::memory_order_release); }
    // CLREX/context destruction; safe owner path, not a signal handler.
    void Clear() noexcept;
    // Safe recovery path after abandoned helper frames, including Consumption.
    void CancelFault() noexcept { Clear(); }
    [[nodiscard]] Consumption BeginConsume(Address address, Shape shape);

private:
    friend class Registry;
    friend class Consumption;
    std::shared_ptr<detail::RegistryState> registry;
    std::shared_ptr<detail::Watch> watch;
    Address address{};
    Shape shape{};
    std::uint64_t version{};
    // Never reused. Saturation rejects registration instead of permitting a
    // stale fault-abandoned scope to clear a replacement. No scope owns refs.
    std::uint64_t registration_nonce{};
    bool available{};
    bool consuming{};
    std::atomic<bool> cancelled{false};
};
static_assert(std::atomic<bool>::is_always_lock_free);

// Token is consumed even on mismatch. Matches is a validation, not a lock:
// caller must hold its memory gate through notification and physical commit.
// A fault can abandon this frame; Token::CancelFault is then mandatory. A
// normally returning Consumption must not outlive its owning Token.
class Consumption final {
public:
    ~Consumption();
    Consumption(const Consumption&) = delete;
    Consumption& operator=(const Consumption&) = delete;
    Consumption(Consumption&& other) noexcept;
    Consumption& operator=(Consumption&&) = delete;
    [[nodiscard]] bool Matches() const;
    void Finish() noexcept;
private:
    friend class Token;
    Consumption(Token* token, Address address, Shape shape);
    Token* token{};
    Address address{};
    Shape shape{};
    std::uint64_t registration_nonce{};
};

enum class WriteResult { NoActiveTickets, NoOverlap, Invalidated };

class Registry final {
public:
    // Production uses UINT64_MAX. A reduced limit enables deterministic wrap
    // testing via the same retirement path; zero is invalid.
    explicit Registry(std::uint64_t version_limit = std::numeric_limits<std::uint64_t>::max());
    // Clears/replaces old reservation, including one from another Registry.
    // Call only after a complete successful load snapshot under gate+drain.
    void Register(Token& token, Address address, Shape shape);
    // Notify BEFORE first possible physical mutation, including same-value,
    // partial-fault and conservatively failed CAS attempts. Caller holds an
    // admitted reader lease or the writer gate. No registry-side bytes access.
    [[nodiscard]] WriteResult InvalidateWrite(BackingIdentity backing,
                                             std::uint64_t offset,
                                             std::uint64_t length);
    [[nodiscard]] std::uint64_t ActiveTickets() const noexcept;
private:
    std::shared_ptr<detail::RegistryState> state;
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

} // namespace swift::runtime::exclusive
