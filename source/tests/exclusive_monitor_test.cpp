#include "runtime/common/exclusive_monitor.h"

#include <array>
#include <iostream>
#include <limits>
#include <new>
#include <semaphore>
#include <stdexcept>
#include <thread>

#ifndef SWIFT_EXCLUSIVE_MONITOR_STANDALONE
#include <catch2/catch_test_macros.hpp>
#endif

namespace {
using namespace swift::runtime::exclusive;
constexpr BackingIdentity backing{17, 31};
constexpr Shape byte{1, false}, word{4, false}, pair{8, true};
Address At(std::uint64_t offset, std::uint64_t generation = 9) {
    return {backing, offset, generation};
}
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Exception, class Fn> void Throws(Fn fn, const char* message) {
    try { fn(); } catch (const Exception&) { return; }
    throw std::runtime_error(message);
}

// These tests drive actual write events, not a value-CAS model. Registry calls
// are the proposed caller hooks; no guest ISA/memory admission is implemented.
void TestABAAndSameValue() {
    Registry registry;
    Token token;
    std::array<unsigned char, 64> bytes{};
    bytes[4] = 42;
    const auto write = [&](unsigned char value) {
        Check(registry.InvalidateWrite(backing, 4, 1) == WriteResult::Invalidated,
              "watched write must notify before mutation");
        bytes[4] = value;
    };
    registry.Register(token, At(4), byte);
    write(99);
    write(42);
    Check(bytes[4] == 42, "ABA returned to original value");
    { auto attempt = token.BeginConsume(At(4), byte);
      Check(!attempt.Matches(), "ABA must not restore reservation"); }
    registry.Register(token, At(4), byte);
    write(42);
    { auto attempt = token.BeginConsume(At(4), byte);
      Check(!attempt.Matches(), "same-value store must invalidate"); }
    Check(registry.ActiveTickets() == 0, "failed attempts release tickets");
}

void TestGranulesAndCanonicalBacking() {
    Registry registry;
    Token left, right, other;
    registry.Register(left, At(0), pair);
    registry.Register(right, At(16, 27), pair); // another view/mapping generation
    const Address independent{{17, 32}, 0, 9};
    registry.Register(other, independent, word);
    Check(registry.InvalidateWrite(backing, 15, 2) == WriteResult::Invalidated,
          "partial byte range crossing granules must reach both reservations");
    auto l = left.BeginConsume(At(0), pair);
    auto r = right.BeginConsume(At(16, 27), pair);
    auto o = other.BeginConsume(independent, word);
    Check(!l.Matches() && !r.Matches() && o.Matches(), "canonical object isolates unrelated backing");
    l.Finish(); r.Finish(); o.Finish();

    // Distinct virtual mappings supply the SAME canonical backing identity.
    registry.Register(left, At(32, 1), word);
    registry.Register(right, At(32, 100), word);
    Registry other_view = registry;
    Check(other_view.InvalidateWrite(backing, 35, 1) == WriteResult::Invalidated,
          "write through any view invalidates canonical backing watches");
    auto first = left.BeginConsume(At(32, 1), word);
    auto second = right.BeginConsume(At(32, 100), word);
    Check(!first.Matches() && !second.Matches(), "mapping generation must not partition writes");
}

void TestUnrelatedWritesAndNoActivePath() {
    Registry registry;
    Check(registry.InvalidateWrite(backing, 0, 64) == WriteResult::NoActiveTickets,
          "inactive writer fast path");
    Token token;
    registry.Register(token, At(0), word);
    Check(registry.InvalidateWrite({18, 31}, 0, 1) == WriteResult::NoOverlap,
          "canonical backing namespace is explicit, not a host address");
    for (unsigned i = 0; i != 10000; ++i)
        Check(registry.InvalidateWrite(backing, 16 + i * 16, 1) == WriteResult::NoOverlap,
              "unrelated granules cannot break forward progress");
    auto attempt = token.BeginConsume(At(0), word);
    Check(attempt.Matches(), "unrelated write storm preserves reservation");
    attempt.Finish();
    Check(registry.InvalidateWrite(backing, 0, 1) == WriteResult::NoActiveTickets,
          "last ticket returns ordinary writers to inactive path");
}

void TestConsumeWindowAndCancellation() {
    Registry registry;
    Token token;
    registry.Register(token, At(0), word);
    auto attempt = token.BeginConsume(At(0), word);
    Check(attempt.Matches() && registry.ActiveTickets() == 1,
          "consumed token stays counted until check/commit scope finishes");
    std::binary_semaphore ready{0}, notify{0}, done{0};
    auto writer_result = WriteResult::NoActiveTickets;
    std::thread writer([&] {
        ready.release(); notify.acquire();
        writer_result = registry.InvalidateWrite(backing, 0, 1);
        done.release();
    });
    ready.acquire(); notify.release(); done.acquire(); writer.join();
    Check(writer_result == WriteResult::Invalidated, "consumed window cannot bypass active guard");
    Check(!attempt.Matches(), "writer between consume and gate/check invalidates");
    attempt.Finish();
    Check(registry.ActiveTickets() == 0, "consumption finish releases ticket");

    registry.Register(token, At(0), word);
    auto cancelled = token.BeginConsume(At(0), word);
    std::thread interrupt([&] { token.RequestCancel(); });
    interrupt.join();
    Check(!cancelled.Matches() && registry.ActiveTickets() == 1,
          "concurrent cancellation is atomic and deferred");
    token.CancelFault();
    Check(registry.ActiveTickets() == 0, "safe recovery reclaims abandoned scope ticket");
    registry.Register(token, At(16), word);
    cancelled.Finish(); // stale fault-abandoned scope must not clear replacement
    { auto replacement = token.BeginConsume(At(16), word);
      Check(replacement.Matches(), "stale consumption cannot cancel a new reservation"); }
    // Model a frame abandoned by native fault recovery: no scope destructor.
    registry.Register(token, At(0), word);
    auto* storage = ::operator new(sizeof(Consumption));
    auto* abandoned = new (storage) Consumption(token.BeginConsume(At(0), word));
    Check(abandoned->Matches(), "abandoned scope initially owns a counted attempt");
    token.RequestCancel(); token.CancelFault();
    ::operator delete(storage); // scope is intentionally never destructed
    Check(registry.ActiveTickets() == 0, "fault cleanup does not depend on scope destructor");
}

void TestWrapRetiresIdentity() {
    Registry registry{2};
    Token old, fresh, latest;
    registry.Register(old, At(0), word);
    Check(registry.InvalidateWrite(backing, 0, 1) == WriteResult::Invalidated, "version 1");
    Check(registry.InvalidateWrite(backing, 0, 1) == WriteResult::Invalidated, "version 2");
    registry.Register(fresh, At(0), word); // captured maximum version
    Check(registry.InvalidateWrite(backing, 0, 1) == WriteResult::Invalidated, "retire instead of wrap");
    registry.Register(latest, At(0), word); // distinct current identity
    auto a = old.BeginConsume(At(0), word);
    auto b = fresh.BeginConsume(At(0), word);
    auto c = latest.BeginConsume(At(0), word);
    Check(!a.Matches() && !b.Matches() && c.Matches(), "wrap never resurrects old identities");
    a.Finish(); b.Finish();
    Check(registry.ActiveTickets() == 1 && c.Matches(), "old ticket destruction cannot erase new watch");
    c.Finish();
    Check(registry.ActiveTickets() == 0, "retired and current tickets are reclaimed");
}

void TestReplaceClearAndLifetime() {
    Token token;
    Registry first, second;
    first.Register(token, At(0), word);
    second.Register(token, At(16), word);
    Check(first.ActiveTickets() == 0 && second.ActiveTickets() == 1, "replacement transfers registry ownership");
    token.Clear(); token.Clear();
    Check(second.ActiveTickets() == 0, "CLREX is idempotent");
    { auto attempt = token.BeginConsume(At(16), word);
      Check(!attempt.Matches(), "store after CLREX fails"); }
    {
        Registry temporary;
        temporary.Register(token, At(0), word);
    }
    { auto attempt = token.BeginConsume(At(0), word);
      Check(attempt.Matches(), "token pins registry lifetime"); }
    {
        Token temporary;
        first.Register(temporary, At(0), word);
    }
    Check(first.ActiveTickets() == 0, "State token destructor releases ticket");
}

void TestMismatchesAndRangeEdges() {
    Registry registry;
    Token token;
    for (const Shape access : {Shape{1, false}, Shape{2, false}, Shape{4, false},
                               Shape{8, false}, Shape{4, true}, Shape{8, true}}) {
        registry.Register(token, At(0), access);
        auto attempt = token.BeginConsume(At(0), access);
        Check(attempt.Matches(), "all six scalar/pair shapes support uncontended attempts");
    }
    for (const auto target : {At(4), At(0, 10), Address{{18, 31}, 0, 9}}) {
        registry.Register(token, At(0), word);
        { auto attempt = token.BeginConsume(target, word);
          Check(!attempt.Matches(), "address/backing/generation mismatch fails"); }
        { auto retry = token.BeginConsume(At(0), word);
          Check(!retry.Matches(), "failed attempt consumes reservation"); }
    }
    registry.Register(token, At(0), pair);
    { auto wrong_shape = token.BeginConsume(At(0), word);
      Check(!wrong_shape.Matches(), "matching width address alone is insufficient"); }
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    registry.Register(token, At(max - 15), pair);
    Check(registry.InvalidateWrite(backing, max, 1) == WriteResult::Invalidated,
          "final address granule does not overflow");
    token.Clear();
    registry.Register(token, At(0), word);
    Check(registry.InvalidateWrite(backing, 0, max) == WriteResult::Invalidated,
          "large copy range scans watches without range-sized iteration");
    Throws<std::invalid_argument>([&] { (void)registry.InvalidateWrite(backing, max, 2); }, "reject wrapped range");
    Throws<std::invalid_argument>([&] { registry.Register(token, At(1), pair); }, "reject unaligned pair");
    Check(registry.ActiveTickets() == 0, "failed replacement clears old reservation");
    Throws<std::invalid_argument>([&] { registry.Register(token, At(0), Shape{2, true}); }, "reject invalid pair shape");
    Throws<std::invalid_argument>([] { Registry invalid{0}; }, "reject zero version limit");
}

void TestSuccessfulCommitAndCompetitor() {
    Registry registry;
    Token winner, loser;
    unsigned memory = 7;
    registry.Register(winner, At(0), word);
    registry.Register(loser, At(0), word);
    {
        auto attempt = winner.BeginConsume(At(0), word);
        Check(attempt.Matches(), "first exclusive attempt can succeed");
        Check(registry.InvalidateWrite(backing, 0, 4) == WriteResult::Invalidated, "exclusive commit invalidates competitors");
        memory = 11;
    }
    { auto attempt = loser.BeginConsume(At(0), word);
      if (attempt.Matches()) memory = 13; }
    Check(memory == 11 && registry.ActiveTickets() == 0, "competing attempts cannot both commit");
    registry.Register(winner, At(0), word);
    { auto fresh = winner.BeginConsume(At(0), word);
      Check(fresh.Matches(), "fresh uncontended retry succeeds"); }
}
} // namespace

#ifdef SWIFT_EXCLUSIVE_MONITOR_STANDALONE
int main() {
    try {
        TestABAAndSameValue(); TestGranulesAndCanonicalBacking();
        TestUnrelatedWritesAndNoActivePath(); TestConsumeWindowAndCancellation();
        TestWrapRetiresIdentity(); TestReplaceClearAndLifetime();
        TestMismatchesAndRangeEdges(); TestSuccessfulCommitAndCompetitor();
        std::cout << "exclusive monitor: 8 deterministic scenarios PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
#else
TEST_CASE("Exclusive registry tracks ABA and same-value writes", "[exclusive][registry]") { TestABAAndSameValue(); }
TEST_CASE("Exclusive registry watches canonical backing granules", "[exclusive][registry]") { TestGranulesAndCanonicalBacking(); }
TEST_CASE("Exclusive registry preserves unrelated-write progress", "[exclusive][registry]") { TestUnrelatedWritesAndNoActivePath(); }
TEST_CASE("Exclusive consumed tickets and pending fault cancellation", "[exclusive][registry]") { TestConsumeWindowAndCancellation(); }
TEST_CASE("Exclusive version overflow retires identities", "[exclusive][registry]") { TestWrapRetiresIdentity(); }
TEST_CASE("Exclusive replacement clear and State lifetime", "[exclusive][registry]") { TestReplaceClearAndLifetime(); }
TEST_CASE("Exclusive shape mapping and range boundary validation", "[exclusive][registry]") { TestMismatchesAndRangeEdges(); }
TEST_CASE("Exclusive commit invalidates competing reservations", "[exclusive][registry]") { TestSuccessfulCommitAndCompetitor(); }
#endif
