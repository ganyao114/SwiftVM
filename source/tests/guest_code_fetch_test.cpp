#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>
#include <sys/mman.h>

#include "translator/linux/guest_memory.h"
#include "translator/arm64/translator.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/arm64/arm64_frontend.h"

namespace {
using swift::linux::GuestMemory;
using swift::runtime::CodeFetchResult;
using swift::u64;
using swift::VAddr;
constexpr auto page = GuestMemory::kHostPageSize;

// Scope: pinned, host-safe readable copies. These tests do not establish
// guest EXEC permission, coherent contents with writers, or JIT publication.
struct Memory {
    GuestMemory memory;
    std::vector<std::pair<VAddr, u64>> direct_allocations;
    explicit Memory(bool window) {
        if (window) REQUIRE(memory.ReserveWindow(20));
        else memory.EnableDirectMode();
    }
    ~Memory() {
        if (memory.Windowed()) {
            munmap(reinterpret_cast<void*>(memory.GetBias()), memory.WindowSize());
        } else {
            for (const auto& [base, size] : direct_allocations) memory.Unmap(base, size);
        }
    }
    VAddr Map(u64 size) {
        const auto base = memory.MapAnywhere(size);
        REQUIRE(base != 0);
        if (!memory.Windowed()) direct_allocations.emplace_back(base, size);
        return base;
    }
};

void FaultLeavesDestination(GuestMemory& memory, VAddr base, std::size_t size = 4) {
    std::array<unsigned char, 16> destination;
    destination.fill(0xa5);
    REQUIRE(size <= destination.size());
    REQUIRE(memory.ReadCode(destination.data(), base, size) == CodeFetchResult::Fault);
    REQUIRE(std::all_of(destination.begin(), destination.end(), [](auto byte) { return byte == 0xa5; }));
}

template<class Predicate> bool Poll(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}
}

TEST_CASE("GuestMemory code copies validate complete readable backing", "[memory][code-fetch]") {
    for (const bool window : {false, true}) {
        CAPTURE(window);
        Memory fixture(window);
        auto& memory = fixture.memory;
        const auto base = fixture.Map(3 * page);
        std::array<unsigned char, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
        memory.WriteBytes(base + 1, bytes);
        std::array<unsigned char, 6> destination;
        destination.fill(0xa5);
        REQUIRE(memory.ReadCode(destination.data() + 1, base + 1, 4) == CodeFetchResult::Success);
        REQUIRE(destination == std::array<unsigned char, 6>{0xa5, 1, 2, 3, 4, 0xa5});
        REQUIRE(memory.Protect(base, page, true, false, false));
        REQUIRE(memory.ReadCode(destination.data() + 1, base + 1, 4) == CodeFetchResult::Success);
        REQUIRE(memory.Protect(base + page, page, false, false, false));
        REQUIRE(memory.RangeIsMapped(base, 3 * page));
        FaultLeavesDestination(memory, base + page);
        FaultLeavesDestination(memory, base + page - 2);
        REQUIRE(memory.Protect(base + page, page, false, true, false));
        FaultLeavesDestination(memory, base + page);
        REQUIRE(memory.Protect(base + page, page, false, false, true));
        FaultLeavesDestination(memory, base + page);
        REQUIRE(memory.Protect(base + page, page, true, false, false));
        REQUIRE(memory.ReadCode(destination.data(), base + page - 2, 4) == CodeFetchResult::Success);
        memory.Unmap(base + page, page);
        REQUIRE_FALSE(memory.RangeIsMapped(base + page, page));
        FaultLeavesDestination(memory, base + page - 2);
        FaultLeavesDestination(memory, base + page);
        REQUIRE(memory.ReadCode(destination.data(), base + 2 * page, 4) == CodeFetchResult::Success);
        FaultLeavesDestination(memory, base + 3 * page - 2);
    }
}

TEST_CASE("GuestMemory replacement restores only explicit map read capability", "[memory][code-fetch]") {
    for (const bool window : {false, true}) {
        Memory fixture(window);
        auto& memory = fixture.memory;
        const auto base = fixture.Map(2 * page);
        REQUIRE(memory.Protect(base, page, false, false, false));
        // SMC's current R/RW overlay must not grant a revoked fetch capability.
        REQUIRE(mprotect(memory.ToHost(base), page, PROT_READ | PROT_WRITE) == 0);
        FaultLeavesDestination(memory, base);
        REQUIRE(memory.ReplaceMappedPages(base, page));
        std::uint32_t instruction = ~std::uint32_t{};
        REQUIRE(memory.ReadCode(&instruction, base, 4) == CodeFetchResult::Success);
        REQUIRE(instruction == 0);
        REQUIRE(memory.Protect(base, page, false, false, false));
        if (window) {
            REQUIRE(memory.MapFixed(base, page));
            REQUIRE(memory.ReadCode(&instruction, base, 4) == CodeFetchResult::Success);
        } else {
            // NOREPLACE failure retains both ownership and revoked capability.
            errno = 0;
            REQUIRE_FALSE(memory.MapFixed(base, page));
            REQUIRE(errno == EEXIST);
            REQUIRE(memory.RangeIsMapped(base, page));
            FaultLeavesDestination(memory, base);
        }
    }
}

TEST_CASE("GuestMemory code range arithmetic and address identity reject unsafe requests", "[memory][code-fetch]") {
    constexpr auto max = std::numeric_limits<VAddr>::max();
    Memory fixture(true);
    auto& memory = fixture.memory;
    REQUIRE(memory.MapFixed(memory.WindowSize() - page, page));
    REQUIRE(memory.MapFixed(GuestMemory::kNullGuardEnd, page));
    FaultLeavesDestination(memory, max - 1);
    FaultLeavesDestination(memory, memory.Mask() - 1);
    FaultLeavesDestination(memory, memory.WindowSize() + memory.Mask() - 1);
    std::uint32_t word = 1;
    REQUIRE(memory.ReadCode(&word, memory.WindowSize() + GuestMemory::kNullGuardEnd, 4) == CodeFetchResult::Success);
    REQUIRE(word == 0);
    REQUIRE(memory.MatchesCodeAddressSpace(memory.GetBias(), memory.Mask()));
    REQUIRE_FALSE(memory.MatchesCodeAddressSpace(memory.GetBias() + page, memory.Mask()));
    REQUIRE_FALSE(memory.MatchesCodeAddressSpace(memory.GetBias(), ~u64{}));
    const auto bias = memory.GetBias();
    memory.SetBias(0);
    REQUIRE(memory.GetBias() == bias);
    REQUIRE_FALSE(memory.ReserveWindow(21));
    REQUIRE_FALSE(memory.MapFixed(max - page + 1, page));
    REQUIRE_FALSE(memory.ReplaceMappedPages(GuestMemory::kNullGuardEnd, max));
    REQUIRE_FALSE(memory.Protect(GuestMemory::kNullGuardEnd, max, true, false, false));
    REQUIRE(memory.RangeIsMapped(GuestMemory::kNullGuardEnd, page));

    GuestMemory overflowing;
    overflowing.SetBias(max - 1);
    FaultLeavesDestination(overflowing, 0);  // base valid, last host byte overflows
    FaultLeavesDestination(overflowing, 4);  // host base overflows
    REQUIRE_FALSE(overflowing.MapFixed(page, page));
    REQUIRE_FALSE(overflowing.Protect(page, page, true, false, false));
    GuestMemory empty;
    FaultLeavesDestination(empty, 1);
    REQUIRE(empty.ReadCode(nullptr, 0, 0) == CodeFetchResult::Success);
    empty.SetBias(page);  // even an empty fetch seals provider identity
    REQUIRE(empty.GetBias() == 0);
}

TEST_CASE("GuestMemory Protect owns the complete rounded range", "[memory][code-fetch]") {
    for (const bool window : {false, true}) {
        Memory fixture(window);
        auto& memory = fixture.memory;
        const auto base = fixture.Map(2 * page);
        memory.Unmap(base + page, page);
        errno = 0;
        REQUIRE_FALSE(memory.Protect(base + page - 1, 2, false, false, false));
        REQUIRE(errno == ENOMEM);
        std::uint32_t word;
        REQUIRE(memory.ReadCode(&word, base, 4) == CodeFetchResult::Success);
        REQUIRE_FALSE(memory.Protect(base, 0, true, false, false));
        REQUIRE(memory.Protect(base + 1, 4, false, false, false));
        FaultLeavesDestination(memory, base);
        REQUIRE(memory.Protect(base, page, true, false, false));
        REQUIRE(memory.ReadCode(&word, base, 4) == CodeFetchResult::Success);
    }
}

TEST_CASE("GuestMemory failed host protection revokes fetch capability conservatively", "[memory][code-fetch]") {
    Memory fixture(false);
    auto& memory = fixture.memory;
    const auto base = fixture.Map(2 * page);
    // Deliberate physical-failure injection: remove the second backing page
    // outside GuestMemory, then mprotect sees a hole. Do not fetch that hole.
    REQUIRE(munmap(memory.ToHost(base + page), page) == 0);
    REQUIRE_FALSE(memory.Protect(base, 2 * page, true, false, false));
    REQUIRE(memory.RangeIsMapped(base, 2 * page));
    FaultLeavesDestination(memory, base);
    REQUIRE(memory.ReplaceMappedPages(base, 2 * page));
    std::uint32_t word = 1;
    REQUIRE(memory.ReadCode(&word, base, 4) == CodeFetchResult::Success);
    REQUIRE(word == 0);
}

TEST_CASE("GuestMemory image setup seals its actual address identity", "[memory][code-fetch]") {
    constexpr VAddr linked = 0x400000;
    for (const bool direct : {false, true}) {
        GuestMemory memory;
        if (direct) memory.EnableDirectMode();
        REQUIRE(memory.MapImageAnywhere(linked, 2 * page));
        const auto bias = memory.GetBias();
        const auto mask = memory.Mask();
        std::uint32_t word = 1;
        REQUIRE(memory.MatchesCodeAddressSpace(bias, mask));
        REQUIRE(memory.ReadCode(&word, linked, 4) == CodeFetchResult::Success);
        REQUIRE(word == 0);
        memory.SetBias(bias + page);
        memory.EnableDirectMode();
        REQUIRE_FALSE(memory.ReserveWindow(20));
        REQUIRE(memory.GetBias() == bias);
        REQUIRE(memory.Mask() == mask);
        REQUIRE(memory.Protect(linked, page, false, false, false));
        FaultLeavesDestination(memory, linked);
        REQUIRE(memory.ReplaceMappedPages(linked, page));
        REQUIRE(memory.ReadCode(&word, linked, 4) == CodeFetchResult::Success);
        memory.Unmap(linked, 2 * page);
        memory.SetBias(0);
        REQUIRE(memory.GetBias() == bias);  // immutable even after all Unmap
        if (memory.Windowed()) munmap(reinterpret_cast<void*>(bias), memory.WindowSize());
    }
    GuestMemory provider;
    REQUIRE_FALSE(provider.MatchesCodeAddressSpace(page, ~u64{}));
    provider.SetBias(page);  // failed identity validation did not expose it
    REQUIRE(provider.GetBias() == page);
    REQUIRE(provider.MatchesCodeAddressSpace(page, ~u64{}));
    provider.SetBias(2 * page);
    REQUIRE(provider.GetBias() == page);
}

TEST_CASE("GuestMemory fetch leases survive concurrent mapping and protection mutations", "[memory][code-fetch]") {
    for (const bool window : {false, true}) {
        CAPTURE(window);
        Memory fixture(window);
        auto& memory = fixture.memory;
        const auto base = fixture.Map(page);
        std::atomic<bool> stop{}, bad{};
        std::atomic<unsigned> phase{}, acknowledged{}, success{}, fault{};
        std::thread reader([&] {
            while (!stop.load()) {
                const auto observed_phase = phase.load();
                std::uint32_t word = 0xa5a5a5a5;
                const auto result = memory.ReadCode(&word, base + 1, 4);
                if (result == CodeFetchResult::Success) {
                    ++success;
                    if (word != 0) bad = true;
                } else if (result == CodeFetchResult::Fault) {
                    ++fault;
                    if (word != 0xa5a5a5a5) bad = true;
                } else bad = true;
                acknowledged.store(observed_phase);
            }
        });
        bool completed = true;
        auto observe = [&] {
            const auto current = ++phase;
            if (!Poll([&] { return acknowledged.load() == current; })) completed = false;
        };
        for (unsigned iteration = 0; iteration < 64 && completed; ++iteration) {
            if (!memory.Protect(base, page, false, false, false)) { completed = false; break; }
            observe();
            if (!memory.ReplaceMappedPages(base, page)) { completed = false; break; }
            observe();
            if (!memory.Protect(base, page, true, false, false)) { completed = false; break; }
            observe();
            memory.Unmap(base, page);
            observe();
            if (!memory.MapFixed(base, page)) { completed = false; break; }
            observe();
        }
        stop = true;
        reader.join();
        REQUIRE(completed);
        REQUIRE_FALSE(bad.load());
        REQUIRE(success.load() >= 64);
        REQUIRE(fault.load() >= 64);
    }
}

TEST_CASE("GuestMemory concurrent window first fit publishes disjoint mappings", "[memory][code-fetch]") {
    Memory fixture(true);
    auto& memory = fixture.memory;
    std::barrier start(3);
    std::array<std::array<VAddr, 32>, 2> addresses{};
    std::array<std::thread, 2> workers;
    for (unsigned index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            start.arrive_and_wait();
            for (auto& addr : addresses[index]) addr = memory.MapAnywhere(page);
        });
    }
    start.arrive_and_wait();
    for (auto& worker : workers) worker.join();
    std::vector<VAddr> sorted;
    for (const auto& batch : addresses) sorted.insert(sorted.end(), batch.begin(), batch.end());
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(sorted.front() != 0);
    REQUIRE(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());
    for (const auto addr : sorted) {
        std::uint32_t word = 1;
        REQUIRE(memory.ReadCode(&word, addr, 4) == CodeFetchResult::Success);
        REQUIRE(word == 0);
    }
}

TEST_CASE("ARM64 single decode delegates production code fetch and preserves fault PC", "[memory][code-fetch][arm64]") {
    using Instance = swift::translator::arm64::Arm64Instance;
    using Context = swift::arm64::ThreadContext64;
    for (const bool window : {false, true}) {
        Memory fixture(window);
        auto& memory = fixture.memory;
        const auto pc = fixture.Map(page);
        const std::uint32_t word = 0x91001420; // ADD x0,x1,#5
        memory.Write(pc, word);
        auto deleter = [](Instance* instance) { Instance::Destroy(instance); };
        std::unique_ptr<Instance, decltype(deleter)> instance{
            Instance::Make(reinterpret_cast<void*>(memory.GetBias()), memory.Windowed() ? memory.Mask() : 0, &memory), deleter};
        REQUIRE(instance);
        for (const bool readable : {true, false}) {
            REQUIRE(memory.Protect(pc, page, readable, false, false));
            swift::runtime::ir::Block block{0, pc};
            swift::runtime::ir::Assembler assembler{&block};
            swift::arm64::A64Decoder decoder{pc, instance->GetAddressSpace()->GetConfig().memory, &assembler};
            REQUIRE(decoder.DecodeSingle() == (readable ? CodeFetchResult::Success : CodeFetchResult::Fault));
            REQUIRE(block.GetGuestCodeDependencies().size() == std::size_t(readable));
            if (readable) {
                REQUIRE(block.GetGuestCodeDependencies()[0].start.Value() == pc);
                REQUIRE(block.GetGuestCodeDependencies()[0].end.Value() == pc + 4);
            }
            alignas(swift::runtime::backend::State) std::array<unsigned char, sizeof(swift::runtime::backend::State) + sizeof(Context)> storage{};
            auto* state = new (storage.data()) swift::runtime::backend::State{};
            Context initial{}; initial.r[0] = 91; initial.r[1] = 37; initial.r[20] = 113; initial.padding = 0x1289;
            std::memcpy(state->uniform_buffer_begin, &initial, sizeof(initial));
            swift::runtime::backend::interp::Interpreter interpreter{*state, &block};
            REQUIRE(interpreter.Run() == (readable ? swift::runtime::HaltReason::None : swift::runtime::HaltReason::CallHost));
            auto expected = initial;
            if (readable) expected.r[0] = 42;
            else expected.padding = static_cast<swift::u32>(swift::arm64::InterruptReason::PAGE_FATAL);
            REQUIRE(std::memcmp(state->uniform_buffer_begin, &expected, sizeof(expected)) == 0);
            REQUIRE(state->current_loc.Value() == pc + (readable ? 4 : 0));
            state->~State();
        }
    }
}
