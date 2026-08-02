#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/link_manager.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/cast_utils.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/block.h"

namespace {

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const char* value) : name_(name) {
        if (const char* old = std::getenv(name)) {
            old_ = old;
        }
        setenv(name, value, 1);
    }

    ~ScopedEnvironment() {
        if (old_) {
            setenv(name_.c_str(), old_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

private:
    std::string name_;
    std::optional<std::string> old_;
};

IntrusivePtr<Block> BuildTarget(VAddr guest, u64 fingerprint) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    const auto value = block->LoadImm(Imm{fingerprint}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, value);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    return block;
}

IntrusivePtr<Block> BuildSource(VAddr guest, VAddr target) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    block->SetTerminal(terminal::LinkBlock{Location{target}});
    block->ReIdInstr();
    return block;
}

std::atomic_bool g_arm_ring_fault{};
std::atomic_bool g_ring_fault_completed{};
std::atomic<u64> g_ring_b_entries{};
std::atomic<u8*> g_ring_fault_address{};

u64 RingTargetProbe() {
    g_ring_b_entries.fetch_add(1, std::memory_order_seq_cst);
    if (g_arm_ring_fault.exchange(false, std::memory_order_seq_cst)) {
        auto* address = g_ring_fault_address.load(std::memory_order_acquire);
        const auto value = *static_cast<volatile u8*>(address);
        *static_cast<volatile u8*>(address) = static_cast<u8>(value ^ 1u);
        // The write returns only after the synchronous SMC handler completed
        // generation deactivate, BL restoration and cache maintenance.
        g_ring_fault_completed.store(true, std::memory_order_seq_cst);
    }
    return 0;
}

IntrusivePtr<Block> BuildRingBlock(VAddr guest, VAddr target, bool probe_target) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    if (probe_target) {
        (void)block
                ->CallLambda(Lambda{Imm{static_cast<u64>(reinterpret_cast<uintptr_t>(
                        FptrCast(&RingTargetProbe)))}})
                .SetType(ValueType::U64);
    }
    block->SetTerminal(terminal::LinkBlock{Location{target}});
    block->ReIdInstr();
    return block;
}

bool WaitUntil(const auto& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

u32 LoadInsn(const void* address) {
    u32 instruction{};
    std::memcpy(&instruction, address, sizeof(instruction));
    return instruction;
}

}  // namespace

TEST_CASE("production direct exit repeatedly delinks recompiles and relinks",
          "[direct-link][production][smc]") {
#if defined(__aarch64__)
    ScopedEnvironment direct_link{"SVM_DIRECT_LINK_V2", "1"};
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};

    const size_t page_size = static_cast<size_t>(getpagesize());
    const size_t guest_size = 4 * page_size;
    void* guest_memory = mmap(nullptr,
                              guest_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON,
                              -1,
                              0);
    REQUIRE(guest_memory != MAP_FAILED);

    {
        const VAddr source_guest = page_size + 0x100;
        const VAddr target_guest = 3 * page_size + 0x100;
        Config config{
                .loc_start = 0,
                .loc_end = guest_size,
                .enable_jit = true,
                .enable_asm_interp = false,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .uniform_buffer_size = 64,
                .global_opts = Optimizations::BlockLink,
                .memory_base = guest_memory,
                .guest_addr_mask = guest_size - 1,
        };
        AddressSpace space{config};
        auto module = space.GetDefaultModule();

        auto source = BuildSource(source_guest, target_guest);
        auto* source_code = TranslateIR(module, source);
        REQUIRE(source_code != nullptr);
        space.PushCodeCache(Location{source_guest}, source_code);

        const auto source_region = module->GetCodeRegion(static_cast<u8*>(source_code));
        REQUIRE(source_region);
        const auto trampoline = source_region->rx_base + source_region->trampoline_offset;
        // This source has no body/prologue instructions: its production
        // terminal is therefore exactly the first and only 4-byte leaf.
        auto* site = static_cast<u8*>(source_code);
        REQUIRE(DecodeBranchTarget(site, LoadInsn(site)) ==
                reinterpret_cast<uintptr_t>(trampoline));
        const LinkSiteKey key{
                source_region->id,
                static_cast<u32>(site - source_region->rx_base),
        };
        REQUIRE(space.GetLinkManager().QuerySite(key)->state == LinkSiteState::Unlinked);

        Runtime runtime{&space};
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CodeMiss);
        REQUIRE(runtime.GetLocation() == target_guest);

        constexpr unsigned kGenerations = 8;
        IntrusivePtr<Block> target;
        for (unsigned generation = 1; generation <= kGenerations; ++generation) {
            target = BuildTarget(target_guest, generation);
            auto* target_code = TranslateIR(module, target);
            REQUIRE(target_code != nullptr);
            space.PushCodeCache(Location{target_guest}, target_code);

            runtime.SetLocation(source_guest);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            u64 observed{};
            std::memcpy(&observed, runtime.GetUniformBuffer().data(), sizeof(observed));
            REQUIRE(observed == generation);
            REQUIRE(space.GetLinkManager().QuerySite(key)->state == LinkSiteState::Linked);
            REQUIRE(DecodeBranchTarget(site, LoadInsn(site)) ==
                    reinterpret_cast<uintptr_t>(target_code));

            space.InvalidateCodeRange(target_guest, target_guest + 1);
            REQUIRE(space.GetLinkManager().QuerySite(key)->state == LinkSiteState::Unlinked);
            REQUIRE(DecodeBranchTarget(site, LoadInsn(site)) ==
                    reinterpret_cast<uintptr_t>(trampoline));
        }

        const auto after_cycles = space.GetLinkManager().GetStats();
        REQUIRE(after_cycles.delinks == kGenerations);
        REQUIRE(after_cycles.linker_calls >= kGenerations + 1);
        REQUIRE(after_cycles.max_in_degree == 1);

        // Hold one execution epoch open so source invalidation proves the
        // two-phase owner lifecycle instead of reclaiming immediately.
        TranslateTable held_l1{8};
        auto held = space.GetSmcTracker().RegisterRuntime(held_l1);
        space.GetSmcTracker().EnableMultithreading();
        space.GetSmcTracker().BeginJit(held);
        space.InvalidateCodeRange(source_guest, source_guest + 1);
        const auto retiring = space.GetLinkManager().QuerySite(key);
        REQUIRE(retiring);
        REQUIRE(retiring->state == LinkSiteState::Retiring);
        space.GetSmcTracker().EndJit(held);
        REQUIRE_FALSE(space.GetLinkManager().QuerySite(key));
        space.GetSmcTracker().UnregisterRuntime(held);
    }

    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("production direct-link execution requires an AArch64 host");
#endif
}

TEST_CASE("SMC fault synchronously breaks a production direct-linked block ring",
          "[direct-link][production][smc][signal-ring]") {
#if defined(__aarch64__)
    ScopedEnvironment direct_link{"SVM_DIRECT_LINK_V2", "1"};
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    const bool same_thread_fault = GENERATE(false, true);
    DYNAMIC_SECTION("faulting runtime thread=" << same_thread_fault) {
        const size_t page_size = static_cast<size_t>(getpagesize());
        const size_t guest_size = 4 * page_size;
        void* guest_memory = mmap(nullptr,
                                  guest_size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANON,
                                  -1,
                                  0);
        REQUIRE(guest_memory != MAP_FAILED);
        {
            const VAddr guest_a = page_size + 0x100;
            const VAddr guest_b = 3 * page_size + 0x100;
            Config config{
                    .loc_start = 0,
                    .loc_end = guest_size,
                    .enable_jit = true,
                    .enable_asm_interp = false,
                    .has_local_operation = false,
                    .backend_isa = kArm64,
                    .uniform_buffer_size = 64,
                    .global_opts = Optimizations::BlockLink,
                    .memory_base = guest_memory,
                    .guest_addr_mask = guest_size - 1,
            };
            AddressSpace space{config};
            space.GetSmcTracker().EnableMultithreading();
            auto module = space.GetDefaultModule();
            auto block_a = BuildRingBlock(guest_a, guest_b, false);
            auto block_b = BuildRingBlock(guest_b, guest_a, true);
            auto* code_a = TranslateIR(module, block_a);
            auto* code_b = TranslateIR(module, block_b);
            REQUIRE(code_a != nullptr);
            REQUIRE(code_b != nullptr);
            space.PushCodeCache(Location{guest_a}, code_a);
            space.PushCodeCache(Location{guest_b}, code_b);

            g_ring_b_entries.store(0, std::memory_order_seq_cst);
            g_ring_fault_completed.store(false, std::memory_order_seq_cst);
            g_arm_ring_fault.store(false, std::memory_order_seq_cst);
            g_ring_fault_address.store(
                    static_cast<u8*>(guest_memory) + guest_b, std::memory_order_release);

            std::atomic_bool runner_done{};
            std::atomic<u32> runner_halt{};
            std::thread runner([&] {
                Runtime runtime{&space};
                runtime.SetLocation(guest_a);
                runner_halt.store(static_cast<u32>(runtime.Run()), std::memory_order_release);
                runner_done.store(true, std::memory_order_release);
            });

            REQUIRE(WaitUntil(
                    [&] { return space.GetLinkManager().GetStats().linked == 2; },
                    std::chrono::seconds(2)));

            if (same_thread_fault) {
                // The helper runs inside block B on the guest execution
                // thread; the next B entry performs the synchronous store.
                g_arm_ring_fault.store(true, std::memory_order_seq_cst);
            } else {
                std::thread writer([&] {
                    Runtime writer_runtime{&space};
                    auto* address = static_cast<volatile u8*>(guest_memory) + guest_b;
                    const auto value = *address;
                    *address = static_cast<u8>(value ^ 1u);
                    g_ring_fault_completed.store(true, std::memory_order_seq_cst);
                });
                writer.join();
            }

            REQUIRE(WaitUntil(
                    [] { return g_ring_fault_completed.load(std::memory_order_seq_cst); },
                    std::chrono::seconds(2)));
            const u64 entries_after_handler =
                    g_ring_b_entries.load(std::memory_order_seq_cst);
            const bool bounded = WaitUntil(
                    [&] { return runner_done.load(std::memory_order_acquire); },
                    std::chrono::seconds(2));
            if (!bounded) {
                // Keep a failing regression test joinable: the deferred path
                // is a rescue only, after the bounded-exit verdict is fixed.
                space.InvalidateCodeRange(guest_b, guest_b + 1);
                REQUIRE(WaitUntil(
                        [&] { return runner_done.load(std::memory_order_acquire); },
                        std::chrono::seconds(2)));
            }
            runner.join();
            REQUIRE(bounded);
            REQUIRE(runner_halt.load(std::memory_order_acquire) ==
                    static_cast<u32>(HaltReason::CodeMiss));
            REQUIRE(g_ring_b_entries.load(std::memory_order_seq_cst) ==
                    entries_after_handler);
            REQUIRE(space.GetLinkManager().QueryTarget(guest_b) == std::nullopt);

            // Deferred CloseWriteWindow has now detached old B. Republish a
            // different target and prove A cold-links to the new generation.
            constexpr u64 kNewFingerprint = 0xD1EEC7u;
            auto replacement_b = BuildTarget(guest_b, kNewFingerprint);
            auto* replacement_code = TranslateIR(module, replacement_b);
            REQUIRE(replacement_code != nullptr);
            space.PushCodeCache(Location{guest_b}, replacement_code);
            Runtime verify{&space};
            verify.SetLocation(guest_a);
            REQUIRE(verify.Run() == HaltReason::CallHost);
            u64 observed{};
            std::memcpy(&observed, verify.GetUniformBuffer().data(), sizeof(observed));
            REQUIRE(observed == kNewFingerprint);
        }
        REQUIRE(munmap(guest_memory, guest_size) == 0);
    }
#else
    SUCCEED("production direct-link ring execution requires an AArch64 host");
#endif
}
