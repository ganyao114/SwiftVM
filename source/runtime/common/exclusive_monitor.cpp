#include "exclusive_monitor.h"

#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace swift::runtime::exclusive {
namespace detail {
struct Key {
    BackingIdentity backing;
    std::uint64_t granule{};
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept {
        auto result = std::hash<std::uint64_t>{}(key.backing.domain);
        for (auto part : {key.backing.object, key.granule})
            result ^= std::hash<std::uint64_t>{}(part) + 0x9e3779b9u + (result << 6) + (result >> 2);
        return result;
    }
};
struct Watch {
    Key key;
    std::uint64_t version{};
    std::uint64_t tickets{};
    bool retired{};
};
struct RegistryState {
    explicit RegistryState(std::uint64_t limit) : version_limit(limit) {}
    std::mutex mutex;
    std::unordered_map<Key, std::shared_ptr<Watch>, KeyHash> watches;
    std::atomic<std::uint64_t> active{};
    const std::uint64_t version_limit;
};
} // namespace detail

namespace {
void Validate(Address address, Shape shape) {
    if ((shape.lane_bytes != 1 && shape.lane_bytes != 2 &&
         shape.lane_bytes != 4 && shape.lane_bytes != 8) ||
        (shape.pair && shape.lane_bytes != 4 && shape.lane_bytes != 8))
        throw std::invalid_argument("exclusive reservation requires scalar 1/2/4/8 or pair 4/8");
    const auto bytes = std::uint64_t(shape.lane_bytes) * (shape.pair ? 2 : 1);
    if (address.offset % bytes)
        throw std::invalid_argument("exclusive reservation requires full-width alignment");
}
} // namespace

Registry::Registry(std::uint64_t version_limit) {
    if (!version_limit) throw std::invalid_argument("exclusive version limit must be nonzero");
    state = std::make_shared<detail::RegistryState>(version_limit);
}

Token::~Token() { Clear(); }
void Token::Clear() noexcept {
    available = false;
    consuming = false;
    cancelled.store(true, std::memory_order_release);
    if (!watch) return;
    // Pin state across clearing the token's references.
    auto current = registry;
    std::lock_guard lock{current->mutex};
    if (--watch->tickets == 0 && !watch->retired) {
        const auto found = current->watches.find(watch->key);
        if (found != current->watches.end() && found->second == watch)
            current->watches.erase(found);
    }
    current->active.fetch_sub(1, std::memory_order_release);
    watch.reset();
    registry.reset();
}

void Registry::Register(Token& token, Address address, Shape shape) {
    // A failed replacement must not preserve an older reservation.
    token.Clear();
    Validate(address, shape);
    if (token.registration_nonce == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("exclusive token registration identity exhausted");
    ++token.registration_nonce;
    token.cancelled.store(false, std::memory_order_release);
    std::lock_guard lock{state->mutex};
    if (state->active.load(std::memory_order_relaxed) == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("exclusive ticket count exhausted");
    const detail::Key key{address.backing, address.offset & ~std::uint64_t{15}};
    auto found = state->watches.find(key);
    if (found == state->watches.end()) {
        auto watch = std::make_shared<detail::Watch>();
        watch->key = key;
        found = state->watches.emplace(key, std::move(watch)).first;
    }
    token.registry = state;
    token.watch = found->second;
    token.address = address;
    token.shape = shape;
    token.version = token.watch->version;
    token.available = true;
    ++token.watch->tickets;
    // Must precede caller's gate release. Post-admission writers load acquire.
    state->active.fetch_add(1, std::memory_order_release);
}

std::uint64_t Registry::ActiveTickets() const noexcept {
    return state->active.load(std::memory_order_acquire);
}

WriteResult Registry::InvalidateWrite(BackingIdentity backing, std::uint64_t offset,
                                      std::uint64_t length) {
    if (length && length - 1 > std::numeric_limits<std::uint64_t>::max() - offset)
        throw std::invalid_argument("exclusive write range wraps backing address");
    if (!ActiveTickets()) return WriteResult::NoActiveTickets;
    if (!length) return WriteResult::NoOverlap;
    const auto first = offset & ~std::uint64_t{15};
    const auto last = (offset + length - 1) & ~std::uint64_t{15};
    bool changed = false;
    std::lock_guard lock{state->mutex};
    // Scan only watched granules; a large copy does not walk its entire range.
    for (auto it = state->watches.begin(); it != state->watches.end();) {
        auto& watch = *it->second;
        if (watch.key.backing != backing || watch.key.granule < first || watch.key.granule > last) {
            ++it;
            continue;
        }
        changed = true;
        if (watch.version == state->version_limit) {
            watch.retired = true;
            it = state->watches.erase(it);
        } else {
            ++watch.version;
            ++it;
        }
    }
    return changed ? WriteResult::Invalidated : WriteResult::NoOverlap;
}

Consumption Token::BeginConsume(Address target, Shape access) {
    if (consuming) throw std::logic_error("nested exclusive consumption");
    // Even malformed/mismatched attempts consume the reservation.
    consuming = true;
    const bool was_available = std::exchange(available, false);
    if (!was_available) cancelled.store(true, std::memory_order_release);
    return Consumption{this, target, access};
}

Consumption::Consumption(Token* owner, Address target, Shape access)
    : token(owner), address(target), shape(access), registration_nonce(owner->registration_nonce) {}
Consumption::Consumption(Consumption&& other) noexcept
    : token(std::exchange(other.token, nullptr)), address(other.address),
      shape(other.shape), registration_nonce(other.registration_nonce) {}
Consumption::~Consumption() { Finish(); }
bool Consumption::Matches() const {
    if (!token || token->registration_nonce != registration_nonce || !token->watch ||
        token->cancelled.load(std::memory_order_acquire) ||
        token->address != address || token->shape != shape) return false;
    std::lock_guard lock{token->registry->mutex};
    return !token->watch->retired && token->watch->version == token->version &&
           !token->cancelled.load(std::memory_order_acquire);
}
void Consumption::Finish() noexcept {
    if (token && token->registration_nonce == registration_nonce) token->Clear();
    token = nullptr;
}

} // namespace swift::runtime::exclusive
