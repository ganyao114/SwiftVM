#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/link_manager.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/cast_utils.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/block.h"
#include "translator/x86/cpu.h"

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

IntrusivePtr<Block> BuildConditionalSource(VAddr guest,
                                           VAddr then_target,
                                           VAddr else_target) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    const auto selector =
            block->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    const auto condition = block->TestNotZero(selector);
    block->SetTerminal(terminal::If{
            condition,
            terminal::LinkBlock{Location{then_target}},
            terminal::LinkBlock{Location{else_target}},
    });
    block->ReIdInstr();
    return block;
}

struct ProductionSite {
    u8* rx{};
    LinkSiteKey key{};
    LinkSiteRecord record{};
};

std::vector<ProductionSite> FindProductionSites(AddressSpace& space,
                                                const CodeRegion& region,
                                                u8* allocation,
                                                size_t scan_bytes = 256) {
    std::vector<ProductionSite> result;
    for (size_t offset = 0; offset < scan_bytes; offset += sizeof(u32)) {
        auto* site = allocation + offset;
        const LinkSiteKey key{
                region.id,
                static_cast<u32>(site - region.rx_base),
        };
        if (const auto record = space.GetLinkManager().QuerySite(key)) {
            result.push_back({site, key, *record});
        }
    }
    return result;
}

void SetSelector(Runtime& runtime, u64 selector) {
    auto uniform = runtime.GetUniformBuffer();
    REQUIRE(uniform.size() >= 16);
    std::memcpy(uniform.data() + 8, &selector, sizeof(selector));
}

std::atomic_bool g_arm_ring_fault{};
std::atomic_bool g_ring_fault_completed{};
std::atomic<u64> g_ring_b_entries{};
std::atomic<u8*> g_ring_fault_address{};
std::atomic<u64> g_conditional_selector{};

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

u64 ConditionalRingSelect() {
    return g_conditional_selector.fetch_add(1, std::memory_order_seq_cst) & 1u;
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

IntrusivePtr<Block> BuildConditionalRingHead(VAddr guest,
                                             VAddr then_target,
                                             VAddr else_target) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    const auto selector = block
                                  ->CallLambda(Lambda{Imm{static_cast<u64>(
                                          reinterpret_cast<uintptr_t>(
                                                  FptrCast(&ConditionalRingSelect)))}})
                                  .SetType(ValueType::U64);
    const auto condition = block->TestNotZero(selector);
    block->SetTerminal(terminal::If{
            condition,
            terminal::LinkBlock{Location{then_target}},
            terminal::LinkBlock{Location{else_target}},
    });
    block->ReIdInstr();
    return block;
}

IntrusivePtr<Block> BuildSelfEdgeConditional(VAddr guest, VAddr cold_target) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    (void)block
            ->CallLambda(Lambda{Imm{static_cast<u64>(reinterpret_cast<uintptr_t>(
                    FptrCast(&RingTargetProbe)))}})
            .SetType(ValueType::U64);
    const auto selector =
            block->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    const auto condition = block->TestNotZero(selector);
    block->SetTerminal(terminal::If{
            condition,
            terminal::LinkBlock{Location{guest}},
            terminal::LinkBlock{Location{cold_target}},
    });
    block->ReIdInstr();
    return block;
}

IntrusivePtr<Block> BuildEligibleBackedgeFlagsConditional(VAddr guest,
                                                          VAddr cold_target) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    const auto selector =
            block->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    const auto one = block->LoadImm(Imm{u64{1}}).SetType(ValueType::U64);
    const auto result = block->Sub(selector, Operand{one}).SetType(ValueType::U64);
    block->SaveFlags(result, Flags::All);
    const auto polarity = block->LoadImm(Imm{u8{1}}).SetType(ValueType::U8);
    block->StoreUniform(
            Uniform{offsetof(swift::x86::ThreadContext64, carry_inverted),
                    ValueType::U8},
            polarity);
    block->AdvancePC(Imm{u64{1}});
    const auto condition = block->LocalCondSet(Cond::EQ).SetType(ValueType::U8);
    block->SetTerminal(terminal::If{
            condition,
            terminal::LinkBlock{Location{guest}},
            terminal::LinkBlock{Location{cold_target}},
    });
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

std::string CurrentTestExecutable() {
#if defined(__APPLE__)
    u32 size{};
    (void)_NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    REQUIRE(_NSGetExecutablePath(path.data(), &size) == 0);
    path.resize(std::strlen(path.c_str()));
    return path;
#elif defined(__linux__)
    std::array<char, 4096> path{};
    const auto size = readlink("/proc/self/exe", path.data(), path.size() - 1);
    REQUIRE(size > 0);
    return std::string(path.data(), static_cast<size_t>(size));
#else
    return {};
#endif
}

int RunW81Child(bool flags_enabled) {
    const auto executable = CurrentTestExecutable();
    REQUIRE_FALSE(executable.empty());
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        setenv("SVM_DIRECT_LINK_P2_W81_CHILD", "1", 1);
        setenv("SVM_DIRECT_LINK_V2", "1", 1);
        setenv("SVM_BACKEDGE_LATCH", "1", 1);
        setenv("SVM_BACKEDGE_FLAGS", flags_enabled ? "1" : "0", 1);
        execl(executable.c_str(),
              executable.c_str(),
              "W81 self edge stays polled while only the cold conditional arm links",
              "--reporter",
              "compact",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    int status{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        }
        REQUIRE(waited == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    kill(child, SIGKILL);
    (void)waitpid(child, &status, 0);
    return 124;
}

}  // namespace

TEST_CASE("production conditional terminal links and delinks both arms independently",
          "[direct-link][production][conditional][smc]") {
#if defined(__aarch64__)
    ScopedEnvironment direct_link{"SVM_DIRECT_LINK_V2", "1"};
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};

    const size_t page_size = static_cast<size_t>(getpagesize());
    const size_t guest_size = 16 * page_size;
    void* guest_memory = mmap(nullptr,
                              guest_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON,
                              -1,
                              0);
    REQUIRE(guest_memory != MAP_FAILED);
    {
        const VAddr source_guest = page_size + 0x100;
        const VAddr then_guest = 5 * page_size + 0x100;
        const VAddr else_guest = 9 * page_size + 0x100;
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

        auto then_block = BuildTarget(then_guest, 0x1111);
        auto else_block = BuildTarget(else_guest, 0x2222);
        auto* then_code = TranslateIR(module, then_block);
        auto* else_code = TranslateIR(module, else_block);
        REQUIRE(then_code != nullptr);
        REQUIRE(else_code != nullptr);
        space.PushCodeCache(Location{then_guest}, then_code);
        space.PushCodeCache(Location{else_guest}, else_code);

        auto source = BuildConditionalSource(source_guest, then_guest, else_guest);
        auto* source_code = static_cast<u8*>(TranslateIR(module, source));
        REQUIRE(source_code != nullptr);
        space.PushCodeCache(Location{source_guest}, source_code);
        const auto region = module->GetCodeRegion(source_code);
        REQUIRE(region);
        auto sites = FindProductionSites(space, *region, source_code);
        REQUIRE(sites.size() == 2);
        const auto then_site = std::find_if(sites.begin(), sites.end(), [&](const auto& site) {
            return site.record.guest_target == then_guest;
        });
        const auto else_site = std::find_if(sites.begin(), sites.end(), [&](const auto& site) {
            return site.record.guest_target == else_guest;
        });
        REQUIRE(then_site != sites.end());
        REQUIRE(else_site != sites.end());
        REQUIRE(then_site->record.kind == LinkSiteKind::ConditionalThen);
        REQUIRE(else_site->record.kind == LinkSiteKind::ConditionalElse);
        REQUIRE(then_site->rx + sizeof(u32) == else_site->rx);
        const auto* trampoline = region->rx_base + region->trampoline_offset;
        REQUIRE(DecodeBranchTarget(then_site->rx, LoadInsn(then_site->rx)) ==
                reinterpret_cast<uintptr_t>(trampoline));
        REQUIRE(DecodeBranchTarget(else_site->rx, LoadInsn(else_site->rx)) ==
                reinterpret_cast<uintptr_t>(trampoline));

        Runtime runtime{&space};
        SetSelector(runtime, 1);
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(space.GetLinkManager().QuerySite(then_site->key)->state ==
                LinkSiteState::Linked);
        REQUIRE(space.GetLinkManager().QuerySite(else_site->key)->state ==
                LinkSiteState::Unlinked);
        REQUIRE(DecodeBranchTarget(then_site->rx, LoadInsn(then_site->rx)) ==
                reinterpret_cast<uintptr_t>(then_code));

        SetSelector(runtime, 0);
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(space.GetLinkManager().QuerySite(else_site->key)->state ==
                LinkSiteState::Linked);
        REQUIRE(DecodeBranchTarget(else_site->rx, LoadInsn(else_site->rx)) ==
                reinterpret_cast<uintptr_t>(else_code));

        // One target invalidation must restore only its incoming arm.
        space.InvalidateCodeRange(then_guest, then_guest + 1);
        REQUIRE(space.GetLinkManager().QuerySite(then_site->key)->state ==
                LinkSiteState::Unlinked);
        REQUIRE(space.GetLinkManager().QuerySite(else_site->key)->state ==
                LinkSiteState::Linked);
        REQUIRE(DecodeBranchTarget(then_site->rx, LoadInsn(then_site->rx)) ==
                reinterpret_cast<uintptr_t>(trampoline));

        // Re-publish then, independently re-link it, then invalidate the range
        // containing both targets in one transaction. Both sites must end BL.
        then_block = BuildTarget(then_guest, 0x3333);
        then_code = TranslateIR(module, then_block);
        REQUIRE(then_code != nullptr);
        space.PushCodeCache(Location{then_guest}, then_code);
        SetSelector(runtime, 1);
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(space.GetLinkManager().QuerySite(then_site->key)->state ==
                LinkSiteState::Linked);
        space.InvalidateCodeRange(then_guest, else_guest + 1);
        REQUIRE(space.GetLinkManager().QuerySite(then_site->key)->state ==
                LinkSiteState::Unlinked);
        REQUIRE(space.GetLinkManager().QuerySite(else_site->key)->state ==
                LinkSiteState::Unlinked);
        REQUIRE(DecodeBranchTarget(then_site->rx, LoadInsn(then_site->rx)) ==
                reinterpret_cast<uintptr_t>(trampoline));
        REQUIRE(DecodeBranchTarget(else_site->rx, LoadInsn(else_site->rx)) ==
                reinterpret_cast<uintptr_t>(trampoline));

        const auto stats = space.GetLinkManager().GetStats();
        REQUIRE(stats.sites_by_kind[static_cast<size_t>(LinkSiteKind::ConditionalThen)] == 1);
        REQUIRE(stats.sites_by_kind[static_cast<size_t>(LinkSiteKind::ConditionalElse)] == 1);
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("production conditional direct-link execution requires an AArch64 host");
#endif
}

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

TEST_CASE("SMC fault boundedly breaks a conditional two-arm production ring",
          "[direct-link][production][conditional][smc][signal-ring][stress]") {
#if defined(__aarch64__)
    ScopedEnvironment direct_link{"SVM_DIRECT_LINK_V2", "1"};
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    const bool same_thread_fault = GENERATE(false, true);
    DYNAMIC_SECTION("conditional ring faulting runtime thread=" << same_thread_fault) {
        const size_t page_size = static_cast<size_t>(getpagesize());
        const size_t guest_size = 8 * page_size;
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
            const VAddr guest_c = 5 * page_size + 0x100;
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
            // Keep QSBR enabled in both variants so the same test also covers
            // the production MT reclaim protocol. The ST variant means the
            // guest execution thread itself performs the synchronous write;
            // the MT variant uses a distinct writer thread.
            space.GetSmcTracker().EnableMultithreading();
            auto module = space.GetDefaultModule();
            auto block_a = BuildConditionalRingHead(guest_a, guest_b, guest_c);
            auto block_b = BuildRingBlock(guest_b, guest_a, true);
            auto block_c = BuildRingBlock(guest_c, guest_a, false);
            auto* code_a = TranslateIR(module, block_a);
            auto* code_b = TranslateIR(module, block_b);
            auto* code_c = TranslateIR(module, block_c);
            REQUIRE(code_a != nullptr);
            REQUIRE(code_b != nullptr);
            REQUIRE(code_c != nullptr);
            space.PushCodeCache(Location{guest_a}, code_a);
            space.PushCodeCache(Location{guest_b}, code_b);
            space.PushCodeCache(Location{guest_c}, code_c);

            g_conditional_selector.store(0, std::memory_order_seq_cst);
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
                    [&] { return space.GetLinkManager().GetStats().linked == 4; },
                    std::chrono::seconds(2)));
            const auto linked_stats = space.GetLinkManager().GetStats();
            REQUIRE(linked_stats.linked_by_kind[
                            static_cast<size_t>(LinkSiteKind::ConditionalThen)] == 1);
            REQUIRE(linked_stats.linked_by_kind[
                            static_cast<size_t>(LinkSiteKind::ConditionalElse)] == 1);

            if (same_thread_fault) {
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

            constexpr u64 kNewFingerprint = 0xC02D17u;
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
    SUCCEED("conditional production ring execution requires an AArch64 host");
#endif
}

TEST_CASE("W81 self edge stays polled while only the cold conditional arm links",
          "[direct-link][production][conditional][smc][w81]") {
#if defined(__aarch64__)
    if (!std::getenv("SVM_DIRECT_LINK_P2_W81_CHILD")) {
        REQUIRE(RunW81Child(false) == 0);
        REQUIRE(RunW81Child(true) == 0);
        return;
    }
    ScopedEnvironment direct_link{"SVM_DIRECT_LINK_V2", "1"};
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    ScopedEnvironment latch{"SVM_BACKEDGE_LATCH", "1"};
    const bool flags_enabled =
            std::strcmp(std::getenv("SVM_BACKEDGE_FLAGS"), "0") != 0;
    ScopedEnvironment flags{"SVM_BACKEDGE_FLAGS", flags_enabled ? "1" : "0"};
    DYNAMIC_SECTION("backedge flags=" << flags_enabled) {
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
            const VAddr self_guest = page_size + 0x100;
            const VAddr cold_guest = 3 * page_size + 0x100;
            Config config{
                    .loc_start = 0,
                    .loc_end = guest_size,
                    .enable_jit = true,
                    .enable_asm_interp = false,
                    .has_local_operation = false,
                    .backend_isa = kArm64,
                    .uniform_buffer_size = static_cast<u32>(
                            flags_enabled ? sizeof(swift::x86::ThreadContext64) : 64),
                    .global_opts = Optimizations::BlockLink,
                    .memory_base = guest_memory,
                    .guest_addr_mask = guest_size - 1,
            };
            AddressSpace space{config};
            space.GetSmcTracker().EnableMultithreading();
            auto module = space.GetDefaultModule();
            auto cold = BuildTarget(cold_guest, 0x811);
            auto self = flags_enabled
                    ? BuildEligibleBackedgeFlagsConditional(self_guest, cold_guest)
                    : BuildSelfEdgeConditional(self_guest, cold_guest);
            auto* cold_code = TranslateIR(module, cold);
            auto* self_code = static_cast<u8*>(TranslateIR(module, self));
            REQUIRE(cold_code != nullptr);
            REQUIRE(self_code != nullptr);
            space.PushCodeCache(Location{cold_guest}, cold_code);
            space.PushCodeCache(Location{self_guest}, self_code);

            const auto region = module->GetCodeRegion(self_code);
            REQUIRE(region);
            const auto sites = FindProductionSites(space, *region, self_code);
            REQUIRE(sites.size() == 1);
            REQUIRE(sites.front().record.guest_target == cold_guest);
            REQUIRE(sites.front().record.kind ==
                    (flags_enabled ? LinkSiteKind::BackedgeCold
                                   : LinkSiteKind::ConditionalElse));

            Runtime runtime{&space};
            SetSelector(runtime, 0);
            runtime.SetLocation(self_guest);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            REQUIRE(space.GetLinkManager().QuerySite(sites.front().key)->state ==
                    LinkSiteState::Linked);

            SetSelector(runtime, 1);
            g_ring_fault_completed.store(false, std::memory_order_seq_cst);
            g_ring_fault_address.store(
                    static_cast<u8*>(guest_memory) + self_guest,
                    std::memory_order_release);
            runtime.SetLocation(self_guest);
            const auto start = std::chrono::steady_clock::now();
            if (flags_enabled) {
                std::atomic_bool done{};
                std::atomic<u32> halt{};
                std::thread runner([&] {
                    halt.store(static_cast<u32>(runtime.Run()),
                               std::memory_order_release);
                    done.store(true, std::memory_order_release);
                });
                std::thread writer([&] {
                    Runtime writer_runtime{&space};
                    auto* address = static_cast<volatile u8*>(guest_memory) + self_guest;
                    const auto value = *address;
                    *address = static_cast<u8>(value ^ 1u);
                    g_ring_fault_completed.store(true, std::memory_order_seq_cst);
                });
                writer.join();
                REQUIRE(WaitUntil(
                        [&] { return done.load(std::memory_order_acquire); },
                        std::chrono::seconds(2)));
                runner.join();
                REQUIRE(halt.load(std::memory_order_acquire) ==
                        static_cast<u32>(HaltReason::CodeMiss));
            } else {
                g_arm_ring_fault.store(true, std::memory_order_seq_cst);
                REQUIRE(runtime.Run() == HaltReason::CodeMiss);
            }
            REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
            REQUIRE(g_ring_fault_completed.load(std::memory_order_seq_cst));
        }
        REQUIRE(munmap(guest_memory, guest_size) == 0);
    }
#else
    SUCCEED("W81 conditional direct-link execution requires an AArch64 host");
#endif
}
