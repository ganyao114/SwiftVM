#include "runtime/common/memory_lease.h"

#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string_view>
#if defined(__unix__)
#include <sys/wait.h>
#include <unistd.h>
#endif

// Deterministic allocation failures and a no-allocation recovery assertion.
namespace {
thread_local int fail_after = -1;
thread_local std::size_t allocation_count{};
}
void* operator new(std::size_t size) {
    ++allocation_count;
    if (fail_after == 0) throw std::bad_alloc{};
    if (fail_after > 0) --fail_after;
    if (auto* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
using namespace swift::runtime::memory;
using namespace std::chrono_literals;
std::size_t assertions{};
void Check(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
template<class Exception, class Function>
void Throws(Function&& function, const char* message) {
    bool threw{};
    try { function(); } catch (const Exception&) { threw = true; }
    Check(threw, message);
}
template<class Predicate>
bool Poll(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

struct Fixture {
    std::atomic<unsigned> gate{}, anonymous{};
    AdmissionProtocol protocol{gate, anonymous, 2s};
};
struct InjectedError final : std::exception {
    const char* what() const noexcept override { return "injected admission error"; }
};
struct FailingAdmission final : Admission {
    AdmissionProtocol& protocol;
    bool fail_admit{}, fail_gate{}, fail_drain{};
    unsigned admits{}, gates{}, drains{}, readmits{};
    explicit FailingAdmission(AdmissionProtocol& protocol_) : protocol(protocol_) {}
    std::uint64_t DomainIdentity() const noexcept override { return protocol.DomainIdentity(); }
    void ValidateEndpoint(Endpoint endpoint) override { protocol.ValidateEndpoint(endpoint); }
    void AttachLedger() override { protocol.AttachLedger(); }
    void DetachLedger() noexcept override { protocol.DetachLedger(); }
    void Pin(Endpoint endpoint) noexcept override { protocol.Pin(endpoint); }
    void Unpin(Endpoint endpoint) noexcept override { protocol.Unpin(endpoint); }
    void Admit(Endpoint endpoint) override {
        ++admits; if (fail_admit) throw InjectedError{}; protocol.Admit(endpoint);
    }
    void AcquireGate() override { ++gates; if (fail_gate) throw InjectedError{}; protocol.AcquireGate(); }
    void Drain() override { ++drains; if (fail_drain) throw InjectedError{}; protocol.Drain(); }
    void Withdraw(Endpoint endpoint) noexcept override { protocol.Withdraw(endpoint); }
    void ReleaseGate() noexcept override { protocol.ReleaseGate(); }
    void Readmit(Endpoint endpoint) noexcept override { ++readmits; protocol.Readmit(endpoint); }
};
struct Guard {
    LeaseLedger& ledger;
    LeaseHandle handle;
    Guard(LeaseLedger& ledger_, FrameHandle frame, Endpoint endpoint = {})
        : ledger(ledger_), handle(ledger.Acquire(frame, endpoint)) {}
    ~Guard() { ledger.Release(handle); }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
};

void OwnReadersAndPreparedMutation() {
    Fixture fixture;
    AdmissionProtocol::Participant first{fixture.protocol}, second{fixture.protocol};
    LeaseLedger ledger{fixture.protocol};
    const auto frame = ledger.OpenFrame();
    const auto a = ledger.Acquire(frame), b = ledger.Acquire(frame);
    const auto c = ledger.Acquire(frame, first.GetEndpoint()), d = ledger.Acquire(frame, second.GetEndpoint());
    const auto writer = ledger.BeginWriter(frame);
    Check(fixture.anonymous == 2 && first.Active() == 1 && second.Active() == 1, "prepare changed admission");
    Throws<std::logic_error>([&] { ledger.Acquire(frame); }, "prepared writer allowed acquire");
    Throws<std::logic_error>([&] { ledger.OpenFrame(frame); }, "prepared writer allowed child");
    Check(ledger.Release(b), "release during preparation failed");
    ledger.EnterWriter(writer);
    Check(ledger.OwnsGate(writer) && fixture.gate == 1, "writer did not own gate");
    Check(fixture.anonymous == 0 && first.Active() == 0 && second.Active() == 0, "writer waited on own readers");
    Check(!ledger.IsCounted(a) && ledger.IsLive(a), "suspension destroyed live handle");
    Check(ledger.Release(c), "suspended release failed");
    Check(ledger.FinishWriter(writer), "finish failed");
    Check(fixture.gate == 0 && fixture.anonymous == 1 && first.Active() == 0 && second.Active() == 1,
          "finish restored dead or omitted live readers");
    Check(!ledger.FinishWriter(writer) && !ledger.Release(b), "duplicate release changed counters");
    ledger.Release(a); ledger.Release(d); ledger.CloseFrame(frame);
}

// Intentionally leave every local handle behind. Recovery owns all resources.
void AbandonedHelper(LeaseLedger& ledger, FrameHandle frame, Endpoint endpoint) {
    ledger.Acquire(frame); ledger.Acquire(frame, endpoint);
    const auto writer = ledger.BeginWriter(frame);
    ledger.EnterWriter(writer);
}
void NestedFaultAndOuterDestructor() {
    Fixture fixture;
    AdmissionProtocol::Participant outer_private{fixture.protocol}, inner_private{fixture.protocol};
    LeaseLedger ledger{fixture.protocol};
    const auto outer = ledger.OpenFrame(), inner = ledger.OpenFrame(outer), child = ledger.OpenFrame(inner);
    {
        Guard anonymous{ledger, outer}, private_guard{ledger, outer, outer_private.GetEndpoint()};
        const auto abandoned_child = ledger.Acquire(child);
        AbandonedHelper(ledger, inner, inner_private.GetEndpoint());
        Check(fixture.gate == 1 && fixture.anonymous == 0, "abandoned writer not suspended");
        const auto before = allocation_count;
        fail_after = 0;
        const bool recovered = ledger.RecoverFrame(inner);
        fail_after = -1;
        Check(recovered && allocation_count == before, "recovery allocated");
        Check(fixture.gate == 0 && fixture.anonymous == 1 && outer_private.Active() == 1 && inner_private.Active() == 0,
              "inner recovery did not preserve outer guards");
        Check(!ledger.IsLive(child) && !ledger.IsLive(abandoned_child) && ledger.IsLive(inner), "recovery frame boundary wrong");
        Check(ledger.RecoverFrame(inner) && fixture.anonymous == 1, "repeat recovery released outer lease");
        Check(ledger.IsCounted(anonymous.handle) && ledger.IsCounted(private_guard.handle), "outer handles not resumed");
    }
    Check(fixture.anonymous == 0 && outer_private.Active() == 0, "outer RAII double-decremented");
    ledger.CloseFrame(inner); ledger.CloseFrame(outer);
}

void UnrelatedRecoveryAndFrameClose() {
    Fixture fixture;
    LeaseLedger ledger{fixture.protocol};
    const auto a = ledger.OpenFrame(), b = ledger.OpenFrame(), child = ledger.OpenFrame(a);
    Throws<std::logic_error>([&] { ledger.CloseFrame(a); }, "closed parent with live child");
    const auto left = ledger.Acquire(a), right = ledger.Acquire(b);
    const auto writer = ledger.BeginWriter(a); ledger.EnterWriter(writer);
    Throws<std::logic_error>([&] { ledger.CloseFrame(a); }, "closed frame with live resources");
    Check(ledger.RecoverFrame(b), "unrelated frame recovery failed");
    Check(fixture.gate == 1 && ledger.OwnsGate(writer) && ledger.IsLive(left) && !ledger.IsLive(right),
          "unrelated recovery released someone else's writer");
    ledger.FinishWriter(writer);
    Check(fixture.anonymous == 1, "unrelated abandoned reader was restored");
    ledger.Release(left); ledger.CloseFrame(child); ledger.CloseFrame(a); ledger.CloseFrame(b);
}

void StaleAndSaturatingGenerations() {
    Fixture fixture;
    FailingAdmission adapter{fixture.protocol};
    LeaseLedger ledger{adapter, 2};
    const auto f1 = ledger.OpenFrame(); ledger.CloseFrame(f1);
    const auto f2 = ledger.OpenFrame(); ledger.CloseFrame(f2);
    const auto f3 = ledger.OpenFrame();
    Check(f1.slot == f2.slot && f2.generation == 2 && f3.slot != f1.slot, "frame generation wrapped");
    const auto l1 = ledger.Acquire(f3); ledger.Release(l1);
    const auto l2 = ledger.Acquire(f3); ledger.Release(l2);
    const auto l3 = ledger.Acquire(f3);
    Check(l1.slot == l2.slot && l3.slot != l1.slot, "reader generation wrapped");
    Check(!ledger.Release(l1) && !ledger.RecoverFrame(f1) && fixture.anonymous == 1, "stale handle released new reader");
    const auto w1 = ledger.BeginWriter(f3); ledger.FinishWriter(w1);
    const auto w2 = ledger.BeginWriter(f3); ledger.FinishWriter(w2);
    const auto w3 = ledger.BeginWriter(f3);
    Check(w1.slot == w2.slot && w3.slot != w1.slot && !ledger.FinishWriter(w1), "writer generation wrapped");
    const auto gates = adapter.gates;
    Throws<std::invalid_argument>([&] { ledger.EnterWriter(w1); }, "stale writer accepted");
    Check(adapter.gates == gates && fixture.anonymous == 1, "stale writer touched gate/admission");
    ledger.EnterWriter(w3); ledger.FinishWriter(w3); ledger.Release(l3); ledger.CloseFrame(f3);
    Throws<std::invalid_argument>([&] { ledger.BeginWriter(f1); }, "stale writer frame accepted");
    Check(adapter.gates == gates + 1, "invalid frame attempted gate acquisition");
}

void AdmissionAndWriterExceptions() {
    Fixture fixture;
    FailingAdmission adapter{fixture.protocol};
    LeaseLedger ledger{adapter};
    const auto frame = ledger.OpenFrame();
    adapter.fail_admit = true;
    Throws<InjectedError>([&] { ledger.Acquire(frame); }, "admission failure missing");
    Check(ledger.LiveReaders() == 0 && fixture.anonymous == 0, "failed admission leaked reader");
    adapter.fail_admit = false;
    const auto reader = ledger.Acquire(frame);
    auto writer = ledger.BeginWriter(frame);
    adapter.fail_gate = true;
    Throws<InjectedError>([&] { ledger.EnterWriter(writer); }, "gate failure missing");
    Check(!ledger.OwnsGate(writer) && fixture.anonymous == 0 && fixture.gate == 0, "failed gate ownership inaccurate");
    Check(ledger.FinishWriter(writer) && fixture.anonymous == 1, "failed gate did not restore reader");
    adapter.fail_gate = false; adapter.fail_drain = true;
    writer = ledger.BeginWriter(frame);
    Throws<InjectedError>([&] { ledger.EnterWriter(writer); }, "drain failure missing");
    Check(ledger.OwnsGate(writer) && fixture.gate == 1, "drain exception lost writer ownership");
    ledger.RecoverFrame(frame);
    Check(fixture.gate == 0 && fixture.anonymous == 0 && !ledger.IsLive(reader), "drain failure recovery leaked resources");
    ledger.CloseFrame(frame);
}

void AllocationFailures() {
    Fixture fixture;
    FailingAdmission adapter{fixture.protocol};
    LeaseLedger ledger{adapter};
    const auto frame = ledger.OpenFrame();
    std::vector<LeaseHandle> handles; handles.reserve(600);
    unsigned pool_failures{};
    for (unsigned i = 0; i < 600; ++i) {
        const auto before = fixture.anonymous.load();
        const auto admits = adapter.admits;
        LeaseHandle handle;
        fail_after = 0;
        try { handle = ledger.Acquire(frame); }
        catch (const std::bad_alloc&) { ++pool_failures; }
        fail_after = -1;
        if (!handle) {
            Check(fixture.anonymous == before && adapter.admits == admits, "pool allocation failure admitted reader");
            handle = ledger.Acquire(frame);
        }
        handles.push_back(handle);
    }
    Check(pool_failures > 0, "did not exercise deque allocation failure");
    const auto before = fixture.anonymous.load();
    bool threw{};
    fail_after = 0;
    try { ledger.BeginWriter(frame); } catch (const std::bad_alloc&) { threw = true; }
    fail_after = -1;
    Check(threw && fixture.anonymous == before && fixture.gate == 0, "suspension allocation failure changed protocol");
    const auto extra = ledger.Acquire(frame); // pending writer must remain empty
    ledger.Release(extra);
    const auto writer = ledger.BeginWriter(frame); ledger.EnterWriter(writer);
    Check(fixture.anonymous == 0, "large suspension set omitted readers");
    const auto allocations = allocation_count;
    fail_after = 0;
    ledger.RecoverFrame(frame);
    fail_after = -1;
    Check(allocation_count == allocations && fixture.anonymous == 0 && fixture.gate == 0, "large recovery allocated/leaked");
    for (auto handle : handles) Check(!ledger.Release(handle), "recovered handle remained live");
    ledger.CloseFrame(frame);
}

void ForeignReadersDrain() {
    Fixture fixture;
    AdmissionProtocol::Participant own_private{fixture.protocol}, foreign_private{fixture.protocol};
    LeaseLedger ledger{fixture.protocol};
    const auto frame = ledger.OpenFrame();
    const auto own = ledger.Acquire(frame), own2 = ledger.Acquire(frame, own_private.GetEndpoint());
    std::atomic<bool> ready{};
    std::atomic<unsigned> failure{};
    std::jthread foreign{[&] {
        try {
            LeaseLedger other{fixture.protocol}; const auto other_frame = other.OpenFrame();
            const auto a = other.Acquire(other_frame), b = other.Acquire(other_frame, foreign_private.GetEndpoint());
            ready = true;
            if (!Poll([&] { return fixture.gate.load() == 1; })) failure = 1;
            else if (fixture.anonymous != 1 || own_private.Active() != 0 || foreign_private.Active() != 1) failure = 2;
            other.Release(a); other.Release(b); other.CloseFrame(other_frame);
        } catch (...) { failure = 3; ready = true; }
    }};
    Check(Poll([&] { return ready.load(); }), "foreign reader never admitted");
    const auto writer = ledger.BeginWriter(frame); ledger.EnterWriter(writer);
    foreign.join();
    Check(failure == 0 && fixture.anonymous == 0 && foreign_private.Active() == 0, "drain omitted foreign reader or own withdrawal");
    ledger.FinishWriter(writer); ledger.Release(own); ledger.Release(own2); ledger.CloseFrame(frame);
}

void CompetingWriterGateRace() {
    Fixture fixture;
    AdmissionProtocol::Participant private_reader{fixture.protocol};
    LeaseLedger ledger{fixture.protocol};
    const auto frame = ledger.OpenFrame();
    const auto a = ledger.Acquire(frame), b = ledger.Acquire(frame, private_reader.GetEndpoint());
    std::atomic<bool> gate_owned{}, drained{};
    std::atomic<unsigned> failure{};
    std::jthread competing{[&] {
        bool owns{};
        try {
            fixture.protocol.AcquireGate(); owns = true; gate_owned = true;
            fixture.protocol.Drain(); drained = true;
            if (fixture.anonymous != 0 || private_reader.Active() != 0) failure = 1;
        } catch (...) { failure = 2; gate_owned = true; }
        if (owns) fixture.protocol.ReleaseGate();
    }};
    Check(Poll([&] { return gate_owned.load(); }), "competing writer never acquired gate");
    const auto writer = ledger.BeginWriter(frame); ledger.EnterWriter(writer);
    competing.join();
    Check(drained && failure == 0 && ledger.OwnsGate(writer), "gate race waited for own retained reader");
    ledger.FinishWriter(writer);
    Check(fixture.anonymous == 1 && private_reader.Active() == 1, "gate race did not restore readers");
    ledger.Release(a); ledger.Release(b); ledger.CloseFrame(frame);
}

void ReaderAdmissionClosedByGate() {
    Fixture fixture;
    LeaseLedger ledger{fixture.protocol}; const auto frame = ledger.OpenFrame();
    const auto writer = ledger.BeginWriter(frame); ledger.EnterWriter(writer);
    std::atomic<bool> attempting{}, admitted{}, finished{};
    std::atomic<unsigned> failure{};
    std::jthread reader{[&] {
        try {
            LeaseLedger other{fixture.protocol}; const auto f = other.OpenFrame();
            attempting = true; const auto lease = other.Acquire(f); admitted = true;
            other.Release(lease); other.CloseFrame(f); finished = true;
        } catch (...) { failure = 1; attempting = true; }
    }};
    Check(Poll([&] { return attempting.load(); }), "reader never attempted admission");
    Check(!admitted && fixture.gate == 1, "reader crossed held writer gate");
    ledger.FinishWriter(writer); reader.join();
    Check(admitted && finished && failure == 0 && fixture.anonymous == 0, "reader did not resume after gate release");
    ledger.CloseFrame(frame);
}

void BoundedTimeoutsAndOwnership() {
    std::atomic<unsigned> gate{1}, anonymous{};
    AdmissionProtocol protocol{gate, anonymous, 20ms};
    LeaseLedger ledger{protocol}; const auto frame = ledger.OpenFrame();
    Throws<ProtocolError>([&] { ledger.Acquire(frame); }, "closed admission did not time out");
    Check(anonymous == 0 && ledger.LiveReaders() == 0 && gate == 1, "admission timeout changed other writer");
    auto writer = ledger.BeginWriter(frame);
    Throws<ProtocolError>([&] { ledger.EnterWriter(writer); }, "gate acquisition did not time out");
    Check(!ledger.OwnsGate(writer), "failed CAS recorded ownership");
    ledger.RecoverFrame(frame);
    Check(gate == 1, "recovery cleared unrelated gate owner");
    protocol.ReleaseGate();
    protocol.Admit({}); // model a foreign reader
    writer = ledger.BeginWriter(frame);
    Throws<ProtocolError>([&] { ledger.EnterWriter(writer); }, "drain did not time out");
    Check(ledger.OwnsGate(writer), "timed-out drain forgot acquired gate");
    ledger.RecoverFrame(frame);
    Check(gate == 0 && anonymous == 1, "drain recovery removed foreign reader");
    protocol.Withdraw({}); ledger.CloseFrame(frame);
}

void DestructorCleanupAndOwnerCheck() {
    Fixture fixture;
    AdmissionProtocol::Participant participant{fixture.protocol};
    {
        LeaseLedger ledger{fixture.protocol}; const auto frame = ledger.OpenFrame();
        ledger.Acquire(frame); ledger.Acquire(frame, participant.GetEndpoint());
        const auto writer = ledger.BeginWriter(frame); ledger.EnterWriter(writer);
        std::atomic<bool> rejected{};
        std::jthread other{[&] {
            try { ledger.OpenFrame(); } catch (const std::logic_error&) { rejected = true; }
        }};
        other.join(); Check(rejected, "foreign owner accepted mutation");
    }
    Check(fixture.gate == 0 && fixture.anonymous == 0 && participant.Active() == 0, "ledger destruction leaked gate/readers");
}

void DomainsAndDuplicateLedgers() {
    Fixture first, second;
    FailingAdmission decorator{first.protocol};
    AdmissionProtocol::Participant foreign{second.protocol};
    FrameHandle old_frame;
    LeaseHandle old_lease;
    WriterHandle old_writer;
    {
        LeaseLedger ledger{first.protocol}, other{second.protocol};
        const auto frame = ledger.OpenFrame(), other_frame = other.OpenFrame();
        const auto lease = ledger.Acquire(frame), other_lease = other.Acquire(other_frame);
        Check(frame.slot == other_frame.slot && frame.generation == other_frame.generation && frame.domain != other_frame.domain,
              "independent handles lacked ledger domain");
        Check(!ledger.Release(other_lease) && first.anonymous == 1 && second.anonymous == 1, "cross-ledger release accepted");
        Check(!ledger.RecoverFrame(other_frame), "cross-ledger recovery accepted");
        Throws<std::invalid_argument>([&] { ledger.Acquire(other_frame); }, "cross-ledger frame accepted");
        Throws<std::invalid_argument>([&] { ledger.Acquire(frame, foreign.GetEndpoint()); }, "foreign endpoint accepted");
        Throws<std::invalid_argument>([&] { first.protocol.Admit(foreign.GetEndpoint()); }, "adapter admitted foreign endpoint");
        Throws<std::logic_error>([&] { LeaseLedger duplicate{first.protocol}; }, "same-thread duplicate ledger accepted");
        Throws<std::logic_error>([&] { LeaseLedger duplicate{decorator}; }, "decorator hid duplicate admission domain");
        Throws<std::invalid_argument>([&] { AdmissionProtocol duplicate{first.gate, first.anonymous}; }, "duplicate gate registry accepted");
        std::atomic<unsigned> different_counter{};
        Throws<std::invalid_argument>([&] { AdmissionProtocol duplicate{first.gate, different_counter}; }, "shared gate with different registry accepted");
        Throws<std::invalid_argument>([&] { AdmissionProtocol alias{different_counter, different_counter}; }, "gate aliased its reader counter");
        Throws<std::invalid_argument>([&] { AdmissionProtocol alias{first.anonymous, different_counter}; }, "new gate aliased existing reader counter");
        Throws<std::invalid_argument>([&] { AdmissionProtocol alias{different_counter, first.gate}; }, "new reader counter aliased existing gate");
        const auto writer = ledger.BeginWriter(frame), other_writer = other.BeginWriter(other_frame);
        Check(!ledger.FinishWriter(other_writer), "cross-ledger writer finish accepted");
        Throws<std::invalid_argument>([&] { ledger.EnterWriter(other_writer); }, "cross-ledger writer enter accepted");
        ledger.FinishWriter(writer); other.FinishWriter(other_writer);
        old_frame = frame; old_lease = lease; old_writer = writer;
    }
    LeaseLedger replacement{decorator}; const auto frame = replacement.OpenFrame();
    const auto lease = replacement.Acquire(frame);
    Check(!replacement.Release(old_lease) && !replacement.RecoverFrame(old_frame) && !replacement.FinishWriter(old_writer),
          "destroyed ledger domain revived");
    Check(first.anonymous == 1 && replacement.IsCounted(lease), "stale ledger changed replacement");
    replacement.Release(lease); replacement.CloseFrame(frame);
}

void RepeatedReaderWriterRace() {
    Fixture fixture;
    AdmissionProtocol::Participant own_private{fixture.protocol}, racing_private{fixture.protocol};
    LeaseLedger ledger{fixture.protocol}; const auto frame = ledger.OpenFrame();
    const auto own = ledger.Acquire(frame), private_own = ledger.Acquire(frame, own_private.GetEndpoint());
    constexpr unsigned rounds = 3000;
    std::atomic<bool> start{}, in_writer{};
    std::atomic<unsigned> ready{}, in_readers{}, violations{}, completed_readers{};
    const auto read = [&](Endpoint endpoint) {
        try {
            LeaseLedger local{fixture.protocol}; const auto local_frame = local.OpenFrame();
            ready.fetch_add(1);
            if (!Poll([&] { return start.load(); })) { violations.fetch_add(1); return; }
            for (unsigned i = 0; i < rounds; ++i) {
                const auto lease = local.Acquire(local_frame, endpoint);
                in_readers.fetch_add(1);
                if (in_writer.load()) violations.fetch_add(1);
                std::this_thread::yield();
                if (in_writer.load()) violations.fetch_add(1);
                in_readers.fetch_sub(1); local.Release(lease);
            }
            local.CloseFrame(local_frame); completed_readers.fetch_add(1);
        } catch (...) { violations.fetch_add(1); }
    };
    std::jthread anonymous_reader{[&] { read({}); }};
    std::jthread private_reader{[&] { read(racing_private.GetEndpoint()); }};
    Check(Poll([&] { return ready.load() == 2; }), "race readers not ready");
    start = true;
    for (unsigned i = 0; i < rounds; ++i) {
        const auto writer = ledger.BeginWriter(frame); ledger.EnterWriter(writer);
        // A rejected reader may transiently publish a counter while gate=1;
        // only admitted byte-access sections must be excluded from the writer.
        if (ledger.IsCounted(own) || ledger.IsCounted(private_own) || own_private.Active() != 0 || in_readers != 0)
            violations.fetch_add(1);
        in_writer = true;
        std::this_thread::yield();
        if (in_readers != 0) violations.fetch_add(1);
        in_writer = false; ledger.FinishWriter(writer);
    }
    anonymous_reader.join(); private_reader.join();
    Check(violations == 0 && completed_readers == 2, "reader/writer overlap during actual admission race");
    Check(fixture.anonymous == 1 && own_private.Active() == 1 && racing_private.Active() == 0,
          "race leaked admission counters");
    ledger.Release(own); ledger.Release(private_own); ledger.CloseFrame(frame);
}

void NestedAdmissionFailsWithinBound() {
    std::atomic<unsigned> gate{}, anonymous{};
    AdmissionProtocol protocol{gate, anonymous, 40ms};
    AdmissionProtocol::Participant outer_private{protocol}, nested_private{protocol};
    LeaseLedger ledger{protocol}; const auto outer = ledger.OpenFrame(), inner = ledger.OpenFrame(outer);
    const auto a = ledger.Acquire(outer), b = ledger.Acquire(outer, outer_private.GetEndpoint());
    std::atomic<bool> owned{}, drained{};
    std::atomic<unsigned> failure{};
    std::jthread writer{[&] {
        bool acquired{};
        try {
            protocol.AcquireGate(); acquired = true; owned = true;
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            for (;;) {
                try { protocol.Drain(); drained = true; break; }
                catch (const ProtocolError&) {
                    if (std::chrono::steady_clock::now() >= deadline) { failure = 1; break; }
                }
            }
        } catch (...) { failure = 2; owned = true; }
        if (acquired) protocol.ReleaseGate();
    }};
    Check(Poll([&] { return owned.load(); }), "nested test writer not ready");
    const auto before = std::chrono::steady_clock::now();
    Throws<ProtocolError>([&] { ledger.Acquire(inner); }, "nested anonymous admission bypassed writer");
    Throws<ProtocolError>([&] { ledger.Acquire(inner, nested_private.GetEndpoint()); }, "nested private validation blocked/bypassed writer");
    Check(std::chrono::steady_clock::now() - before < 1s, "nested admission exceeded bounded wait");
    Check(ledger.IsCounted(a) && ledger.IsCounted(b) && anonymous == 1 && outer_private.Active() == 1 &&
          nested_private.Active() == 0 && !drained, "nested failure damaged outer reader");
    ledger.Release(a); ledger.Release(b); writer.join();
    Check(drained && failure == 0 && gate == 0 && anonymous == 0, "foreign writer failed after outer release");
    ledger.CloseFrame(inner); ledger.CloseFrame(outer);
}

void SaturatingPinsRejectWithoutWrap() {
#if defined(__unix__)
    const auto child = fork();
    Check(child >= 0, "pin saturation fork failed");
    if (child == 0) {
        std::set_terminate([] { std::_Exit(86); });
        Fixture fixture;
        AdmissionProtocol::Participant participant{fixture.protocol};
        auto endpoint = participant.GetEndpoint();
        endpoint.private_pins->store(std::numeric_limits<unsigned>::max());
        LeaseLedger ledger{fixture.protocol}; const auto frame = ledger.OpenFrame();
        ledger.Acquire(frame, endpoint);
        std::_Exit(87);
    }
    int status{};
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 86,
          "pin saturation wrapped instead of rejecting");
#else
    throw std::runtime_error("pin saturation test requires POSIX child processes");
#endif
}
}

int main() {
    struct Test { std::string_view name; void (*run)(); };
    constexpr Test tests[]{
        {"own readers and prepared mutation", OwnReadersAndPreparedMutation},
        {"nested fault and outer destructor", NestedFaultAndOuterDestructor},
        {"unrelated recovery and frame close", UnrelatedRecoveryAndFrameClose},
        {"stale and saturating generations", StaleAndSaturatingGenerations},
        {"admission and writer exceptions", AdmissionAndWriterExceptions},
        {"allocation failures", AllocationFailures},
        {"foreign reader drain", ForeignReadersDrain},
        {"competing writer gate race", CompetingWriterGateRace},
        {"reader admission closed by gate", ReaderAdmissionClosedByGate},
        {"bounded timeouts and ownership", BoundedTimeoutsAndOwnership},
        {"destructor cleanup and owner check", DestructorCleanupAndOwnerCheck},
        {"domains and duplicate ledgers", DomainsAndDuplicateLedgers},
        {"repeated reader writer race", RepeatedReaderWriterRace},
        {"nested admission bounded failure", NestedAdmissionFailsWithinBound},
        {"saturating pins reject without wrap", SaturatingPinsRejectWithoutWrap},
    };
    try {
        for (const auto& test : tests) { test.run(); std::cout << "PASS " << test.name << '\n'; }
        std::cout << std::size(tests) << " tests, " << assertions << " assertions passed\n";
    } catch (const std::exception& error) {
        fail_after = -1; std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
