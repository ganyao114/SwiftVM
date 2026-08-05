#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/cast_utils.h"
#include "runtime/ir/hir_builder.h"

namespace {

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

std::atomic_bool g_region_fault_armed{};
std::atomic_bool g_region_fault_done{};
std::atomic<u64> g_region_loop_entries{};
std::atomic<u64> g_region_selector{};
std::atomic<u8*> g_region_fault_address{};

u64 RegionSelector() {
    return g_region_selector.fetch_add(1, std::memory_order_seq_cst) & 1u;
}

u64 RegionLoopProbe() {
    g_region_loop_entries.fetch_add(1, std::memory_order_seq_cst);
    if (g_region_fault_armed.exchange(false, std::memory_order_seq_cst)) {
        auto* address = g_region_fault_address.load(std::memory_order_acquire);
        const auto value = *static_cast<volatile u8*>(address);
        *static_cast<volatile u8*>(address) = static_cast<u8>(value ^ 1u);
        g_region_fault_done.store(true, std::memory_order_seq_cst);
    }
    return 0;
}

template <typename Predicate>
bool WaitRegion(Predicate&& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

struct RegionFunction {
    HIRBuilder builder{1, true, swift::runtime::FeatureSet{}};
    HIRFunction* function{};
};

std::unique_ptr<RegionFunction> BuildRegionLoop(VAddr a, VAddr b, VAddr c) {
    auto result = std::make_unique<RegionFunction>();
    result->function = result->builder.AppendFunction(Location{a}, Location{c + 1});
    auto* function = result->function;
    function->AdvancePC(Imm{u64{1}});
    const auto selector = function
                                  ->CallLambda(Lambda{Imm{static_cast<u64>(
                                          reinterpret_cast<uintptr_t>(
                                                  FptrCast(&RegionSelector)))}})
                                  .SetType(ValueType::U64);
    const auto condition = function->TestNotZero(selector);
    auto [else_block, then_block] = result->builder.If(terminal::If{
            condition,
            terminal::LinkBlock{Location{b}},
            terminal::LinkBlock{Location{c}},
    });

    result->builder.SetCurBlock(then_block);
    function->AdvancePC(Imm{u64{1}});
    (void)function
            ->CallLambda(Lambda{Imm{static_cast<u64>(reinterpret_cast<uintptr_t>(
                    FptrCast(&RegionLoopProbe)))}})
            .SetType(ValueType::U64);
    result->builder.LinkBlock(terminal::LinkBlock{Location{a}});

    result->builder.SetCurBlock(else_block);
    function->AdvancePC(Imm{u64{1}});
    result->builder.LinkBlock(terminal::LinkBlock{Location{a}});
    function->EndFunction();
    return result;
}

std::unique_ptr<RegionFunction> BuildMixedRegion(VAddr a,
                                                 VAddr internal,
                                                 VAddr external) {
    auto result = std::make_unique<RegionFunction>();
    result->function =
            result->builder.AppendFunction(Location{a}, Location{internal + 1});
    auto* function = result->function;
    function->AdvancePC(Imm{u64{1}});
    const auto selector = function
                                  ->CallLambda(Lambda{Imm{static_cast<u64>(
                                          reinterpret_cast<uintptr_t>(
                                                  FptrCast(&RegionSelector)))}})
                                  .SetType(ValueType::U64);
    const auto condition = function->TestNotZero(selector);
    auto [external_block, internal_block] = result->builder.If(terminal::If{
            condition,
            terminal::LinkBlock{Location{internal}},
            terminal::LinkBlock{Location{external}},
    });

    result->builder.SetCurBlock(internal_block);
    function->AdvancePC(Imm{u64{1}});
    result->builder.ReturnToHost();
    (void)external_block;
    function->EndFunction();
    return result;
}

IntrusivePtr<Block> BuildRegionReplacement(VAddr guest, u64 fingerprint) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    const auto value = block->LoadImm(Imm{fingerprint}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, value);
    block->SetTerminal(terminal::ReturnToHost{});
    block->ReIdInstr();
    return block;
}

}  // namespace

TEST_CASE("region local cycle exits boundedly on whole-function SMC invalidation",
          "[region-edges][production][smc]") {
#if defined(__aarch64__)
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
            const VAddr guest_b = page_size + 0x180;
            const VAddr guest_c = page_size + 0x200;
            Config config{
                    .loc_start = 0,
                    .loc_end = guest_size,
                    .enable_jit = true,
                    .enable_asm_interp = false,
                    .has_local_operation = false,
                    .backend_isa = kArm64,
                    .uniform_buffer_size = 64,
                    .global_opts = Optimizations::BlockLink |
                                   Optimizations::FunctionBaseCompile,
                    .region_edges = true,
                    .memory_base = guest_memory,
                    .guest_addr_mask = guest_size - 1,
            };
            AddressSpace space{config};
            space.GetSmcTracker().EnableMultithreading();
            auto module = space.GetDefaultModule();
            auto region = BuildRegionLoop(guest_a, guest_b, guest_c);
            REQUIRE(TranslateIR(module, region->function) != nullptr);

            // 三个 external entry 都发布，但 region 内边不登记 direct-link site。
            REQUIRE(space.GetCodeCache(guest_a) != nullptr);
            REQUIRE(space.GetCodeCache(guest_b) != nullptr);
            REQUIRE(space.GetCodeCache(guest_c) != nullptr);
            REQUIRE(space.GetLinkManager().GetStats().sites == 0);

            g_region_selector.store(0, std::memory_order_seq_cst);
            g_region_loop_entries.store(0, std::memory_order_seq_cst);
            g_region_fault_done.store(false, std::memory_order_seq_cst);
            g_region_fault_armed.store(same_thread_fault, std::memory_order_seq_cst);
            g_region_fault_address.store(
                    static_cast<u8*>(guest_memory) + guest_b,
                    std::memory_order_release);

            std::atomic_bool runner_done{};
            std::atomic<u32> runner_halt{};
            std::thread runner([&] {
                Runtime runtime{&space};
                runtime.SetLocation(guest_a);
                runner_halt.store(static_cast<u32>(runtime.Run()),
                                  std::memory_order_release);
                runner_done.store(true, std::memory_order_release);
            });

            if (!same_thread_fault) {
                REQUIRE(WaitRegion(
                        [] {
                            return g_region_loop_entries.load(
                                           std::memory_order_seq_cst) > 32;
                        },
                        std::chrono::seconds(2)));
                std::thread writer([&] {
                    Runtime writer_runtime{&space};
                    auto* address = static_cast<volatile u8*>(guest_memory) + guest_b;
                    const auto value = *address;
                    *address = static_cast<u8>(value ^ 1u);
                    g_region_fault_done.store(true, std::memory_order_seq_cst);
                });
                writer.join();
            }

            REQUIRE(WaitRegion(
                    [] { return g_region_fault_done.load(std::memory_order_seq_cst); },
                    std::chrono::seconds(2)));
            const u64 entries_after_fault =
                    g_region_loop_entries.load(std::memory_order_seq_cst);
            const bool bounded = WaitRegion(
                    [&] { return runner_done.load(std::memory_order_acquire); },
                    std::chrono::seconds(2));
            if (!bounded) {
                space.InvalidateCodeRange(guest_b, guest_b + 1);
                REQUIRE(WaitRegion(
                        [&] { return runner_done.load(std::memory_order_acquire); },
                        std::chrono::seconds(2)));
            }
            runner.join();
            REQUIRE(bounded);
            REQUIRE(runner_halt.load(std::memory_order_acquire) ==
                    static_cast<u32>(HaltReason::CodeMiss));
            REQUIRE(g_region_loop_entries.load(std::memory_order_seq_cst) ==
                    entries_after_fault);

            // 同一 function node 的任一页失效都清除三个 external entry。
            REQUIRE(space.GetCodeCache(guest_a) == nullptr);
            REQUIRE(space.GetCodeCache(guest_b) == nullptr);
            REQUIRE(space.GetCodeCache(guest_c) == nullptr);

            constexpr u64 kReplacement = 0xC1C1E5AFEull;
            auto replacement = BuildRegionReplacement(guest_a, kReplacement);
            auto* replacement_code = TranslateIR(module, replacement);
            REQUIRE(replacement_code != nullptr);
            space.PushCodeCache(Location{guest_a}, replacement_code);
            Runtime verify{&space};
            verify.SetLocation(guest_a);
            REQUIRE(verify.Run() == HaltReason::CallHost);
            u64 observed{};
            std::memcpy(&observed, verify.GetUniformBuffer().data(), sizeof(observed));
            REQUIRE(observed == kReplacement);
        }
        REQUIRE(munmap(guest_memory, guest_size) == 0);
    }
#else
    SUCCEED("region edge execution requires an AArch64 host");
#endif
}

TEST_CASE("region localizes only targets emitted in the same unit",
          "[region-edges][production][fallback]") {
#if defined(__aarch64__)
    const size_t page_size = static_cast<size_t>(getpagesize());
    const size_t guest_size = 2 * page_size;
    void* guest_memory = mmap(nullptr,
                              guest_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON,
                              -1,
                              0);
    REQUIRE(guest_memory != MAP_FAILED);
    {
        const VAddr guest_a = 0x100;
        const VAddr guest_internal = 0x180;
        const VAddr guest_external = page_size + 0x100;
        Config config{
                .loc_start = 0,
                .loc_end = guest_size,
                .enable_jit = true,
                .enable_asm_interp = false,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .uniform_buffer_size = 64,
                .global_opts = Optimizations::BlockLink |
                               Optimizations::FunctionBaseCompile,
                .region_edges = true,
                .memory_base = guest_memory,
                .guest_addr_mask = guest_size - 1,
        };
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        auto region = BuildMixedRegion(guest_a, guest_internal, guest_external);
        REQUIRE(TranslateIR(module, region->function) != nullptr);

        // 同 unit 的入口直接走本地标签；未发射的目标继续登记外部 direct-link site。
        REQUIRE(space.GetCodeCache(guest_a) != nullptr);
        REQUIRE(space.GetCodeCache(guest_internal) != nullptr);
        REQUIRE(space.GetCodeCache(guest_external) == nullptr);
        REQUIRE(space.GetLinkManager().GetStats().sites == 1);
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("region edge execution requires an AArch64 host");
#endif
}
