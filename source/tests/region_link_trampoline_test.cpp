#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "aarch64/macro-assembler-aarch64.h"
#include "runtime/common/svm_config.h"
#include "runtime/backend/arm64/fpcr_mode.h"
#include "runtime/backend/arm64/region_link_trampoline.h"
#include "runtime/backend/arm64/trampolines.h"
#include "runtime/backend/context.h"
#include "runtime/backend/link_manager.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/backend/translate_table.h"
#include "translator/x86/cpu.h"

namespace {

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::backend::arm64;
using namespace vixl::aarch64;

constexpr u64 kGuestTarget = 0x401000;

std::vector<UniformMapDesc> PinDescriptors(unsigned level) {
    std::vector<UniformMapDesc> result{
            {offsetof(x86::ThreadContext64, rbx), 8, 20, false},
            {offsetof(x86::ThreadContext64, rsp), 8, 19, false},
            {offsetof(x86::ThreadContext64, rbp), 8, 21, false},
    };
    if (level >= 1) {
        result.insert(result.begin(),
                      {{offsetof(x86::ThreadContext64, rax), 8, 22, false},
                       {offsetof(x86::ThreadContext64, rcx), 8, 23, false},
                       {offsetof(x86::ThreadContext64, rdx), 8, 29, false}});
    }
    if (level >= 2) {
        const std::array<u32, 6> offsets{
                offsetof(x86::ThreadContext64, rsi),
                offsetof(x86::ThreadContext64, rdi),
                offsetof(x86::ThreadContext64, r8),
                offsetof(x86::ThreadContext64, r9),
                offsetof(x86::ThreadContext64, r10),
                offsetof(x86::ThreadContext64, r11),
        };
        for (u32 i = 0; i < offsets.size(); ++i) {
            result.emplace_back(offsets[i], 8, i, false);
        }
    }
    if (level >= 3) {
        const std::array<u32, 4> offsets{
                offsetof(x86::ThreadContext64, r12),
                offsetof(x86::ThreadContext64, r13),
                offsetof(x86::ThreadContext64, r14),
                offsetof(x86::ThreadContext64, r15),
        };
        for (u32 i = 0; i < offsets.size(); ++i) {
            result.emplace_back(offsets[i], 8, 6 + i, false);
        }
    }
    std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.offset < rhs.offset;
    });
    return result;
}

Config TestConfig(std::span<UniformMapDesc> descriptors, bool afp_nan = false) {
    return Config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .enable_asm_interp = false,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = sizeof(x86::ThreadContext64),
            .buffers_static_alloc = descriptors,
            .arm64_features = afp_nan ? Arm64Features::AFP : Arm64Features::None,
            .sse_afp_nan = afp_nan,
    };
}

struct TestState {
    explicit TestState(size_t uniform_size)
            : storage((sizeof(State) + uniform_size + sizeof(u128) - 1) / sizeof(u128)) {
        std::memset(storage.data(), 0, storage.size() * sizeof(u128));
        state = reinterpret_cast<State*>(storage.data());
        state->l1_code_cache = l1.Data();
        state->l2_code_cache = l2.Data();
        state->interface = &profile;
        state->rsb_bottom = &rsb.rsb_frames.front();
        state->rsb_top = &rsb.rsb_frames[rsb_stack_size];
        state->rsb_pointer = state->rsb_top;
    }

    std::span<u8> Uniform(size_t size) {
        return {state->uniform_buffer_begin, size};
    }

    std::vector<u128> storage;
    State* state{};
    TranslateTable l1{8};
    TranslateTable l2{8};
    RuntimeProfileInterface profile{};
    RSBBuffer rsb{};
};

void CopyAssembler(CodeBuffer& buffer, size_t offset, MacroAssembler& masm) {
    masm.FinalizeCode();
    REQUIRE(offset + masm.GetBuffer()->GetSizeInBytes() <= buffer.size);
    std::memcpy(buffer.rw_data + offset,
                masm.GetBuffer()->GetStartAddress<u8*>(),
                masm.GetBuffer()->GetSizeInBytes());
}

void EmitFingerprintTarget(CodeBuffer& buffer, u64 fingerprint, u64* observed_lr = nullptr) {
    MacroAssembler masm;
    masm.Mov(x11, fingerprint);
    masm.Str(x11, MemOperand(x28, state_offset_current_loc));
    if (observed_lr) {
        masm.Mov(x11, reinterpret_cast<uintptr_t>(observed_lr));
        masm.Str(x30, MemOperand(x11));
    }
    masm.Mov(w11, static_cast<u32>(HaltReason::PageFatal));
    masm.Str(w11, MemOperand(x28, state_offset_halt_reason));
    masm.Ret();
    CopyAssembler(buffer, 0, masm);
    buffer.Flush();
}

void EmitHaltTarget(CodeBuffer& buffer, HaltReason reason) {
    MacroAssembler masm;
    masm.Mov(w11, static_cast<u32>(reason));
    masm.Str(w11, MemOperand(x28, state_offset_halt_reason));
    masm.Ret();
    CopyAssembler(buffer, 0, masm);
    buffer.Flush();
}

u32 LoadInsn(const void* address) {
    u32 value{};
    std::memcpy(&value, address, sizeof(value));
    return value;
}

}  // namespace

TEST_CASE("region trampoline preserves x30 and every static-pin configuration",
          "[direct-link][trampoline][x30]") {
#if defined(__aarch64__)
    for (unsigned level = 0; level <= 3; ++level) {
        for (const bool afp_nan : {false, true}) {
            DYNAMIC_SECTION("pin=" << level << " afp=" << afp_nan) {
                auto descriptors = PinDescriptors(level);
                auto config = TestConfig(descriptors, afp_nan);
                TrampolinesArm64 runtime_trampolines{config, FeatureSet{}};
                LinkManager manager;
                CodeCache cache{config, 1u << 20, FeatureSet{}};
                auto* return_host = reinterpret_cast<void*>(
                        runtime_trampolines.GetReturnHost());
                REQUIRE(cache.InitializeRegionTrampoline(
                        manager, return_host, return_host));
                REQUIRE(cache.GetRegion().trampoline_offset !=
                        CodeRegion::kInvalidTrampolineOffset);

                auto code = cache.AllocCode(256);
                REQUIRE(code);
                std::memset(code->rw_data, 0x1f, code->size);
                constexpr size_t kSite = 0;
                constexpr size_t kTarget1 = 64;
                constexpr size_t kTarget2 = 96;
                constexpr size_t kTarget3 = 128;
                const auto bl = EncodeBL(
                        static_cast<u8*>(cache.GetRegionTrampoline()) -
                        (code->exec_data + kSite));
                const auto b12 = EncodeB(static_cast<std::intptr_t>(kTarget2 - kTarget1));
                const auto b23 = EncodeB(static_cast<std::intptr_t>(kTarget3 - kTarget2));
                REQUIRE(bl);
                REQUIRE(b12);
                REQUIRE(b23);
                std::memcpy(code->rw_data + kSite, &*bl, sizeof(*bl));
                std::memcpy(code->rw_data + kTarget1, &*b12, sizeof(*b12));
                std::memcpy(code->rw_data + kTarget2, &*b23, sizeof(*b23));
                u64 observed_lr{};
                u64 observed_fpcr{};
                MacroAssembler final_target;
                final_target.Mov(x11, reinterpret_cast<uintptr_t>(&observed_lr));
                final_target.Str(x30, MemOperand(x11));
                if (afp_nan) {
                    final_target.Mrs(x12, FPCR);
                    final_target.Mov(x11, reinterpret_cast<uintptr_t>(&observed_fpcr));
                    final_target.Str(x12, MemOperand(x11));
                }
                final_target.Mov(w11, static_cast<u32>(HaltReason::PageFatal));
                final_target.Str(w11, MemOperand(x28, state_offset_halt_reason));
                final_target.Ret();
                CopyAssembler(*code, kTarget3, final_target);
                code->Flush();

                const int owner_module{};
                const int owner_allocation{};
                const LinkSiteKey key{cache.GetRegion().id,
                                      code->offset + static_cast<u32>(kSite)};
                REQUIRE(manager.RegisterSite(
                        key, kGuestTarget, {&owner_module, &owner_allocation}));
                const auto generation = manager.PublishTarget(
                        kGuestTarget,
                        code->exec_data + kTarget1,
                        cache.GetRegion().id);
                REQUIRE(generation != 0);

                TestState test_state{config.uniform_buffer_size};
                auto uniform = test_state.Uniform(config.uniform_buffer_size);
                for (size_t i = 0; i < uniform.size(); ++i) {
                    uniform[i] = static_cast<u8>(i * 37u + level * 11u + 3u);
                }
                if (afp_nan) {
                    constexpr u32 kMxcsr = 0x1f80u | (2u << 13);
                    std::memcpy(uniform.data() + offsetof(x86::ThreadContext64, mxcsr),
                                &kMxcsr,
                                sizeof(kMxcsr));
                }
                const std::vector<u8> expected(uniform.begin(), uniform.end());
                const u64 host_fpcr = ReadNativeFPCR();
                auto entry = runtime_trampolines.GetRuntimeEntry();

                REQUIRE(entry(test_state.state, code->exec_data) == HaltReason::PageFatal);
                REQUIRE(observed_lr == reinterpret_cast<uintptr_t>(return_host));
                REQUIRE(std::equal(uniform.begin(), uniform.end(), expected.begin()));
                REQUIRE(manager.QuerySite(key)->state == LinkSiteState::Linked);
                REQUIRE(DecodeBranchTarget(code->exec_data + kSite,
                                           LoadInsn(code->exec_data + kSite)) ==
                        reinterpret_cast<uintptr_t>(code->exec_data + kTarget1));
                if (afp_nan) {
                    constexpr u64 kExpectedGuestFPCR =
                            kSseAFPGuestFPCRBase | (u64{1} << 22);
                    REQUIRE(observed_fpcr == kExpectedGuestFPCR);
                    REQUIRE(ReadNativeFPCR() == host_fpcr);
                }

                observed_lr = 0;
                REQUIRE(entry(test_state.state, code->exec_data) == HaltReason::PageFatal);
                REQUIRE(observed_lr == reinterpret_cast<uintptr_t>(return_host));
                REQUIRE(std::equal(uniform.begin(), uniform.end(), expected.begin()));
            }
        }
    }
#else
    SUCCEED("region trampoline execution requires an AArch64 host");
#endif
}

TEST_CASE("direct-link patch epoch synchronizes each runtime before publication",
          "[direct-link][epoch]") {
    SmcTracker tracker{0};
    TranslateTable l1_a{8};
    TranslateTable l1_b{8};
    auto token_a = tracker.RegisterRuntime(l1_a);
    auto token_b = tracker.RegisterRuntime(l1_b);
    tracker.EnableMultithreading();

    REQUIRE(token_a->synced_patch_epoch.load() == tracker.GetCodePatchEpoch());
    tracker.BeginJit(token_a);
    REQUIRE(token_a->patch_sync_count.load() == 0);
    REQUIRE_FALSE(tracker.CanReclaimAtEpochForTest(2));
    tracker.EndJit(token_a);
    REQUIRE(tracker.CanReclaimAtEpochForTest(2));

    const auto patch_epoch = tracker.AdvanceCodePatchEpoch();
    REQUIRE(token_a->synced_patch_epoch.load() < patch_epoch);
    REQUIRE(token_b->synced_patch_epoch.load() < patch_epoch);
    tracker.BeginJit(token_a);
    REQUIRE(token_a->synced_patch_epoch.load() == patch_epoch);
    REQUIRE(token_a->patch_sync_count.load() == 1);
    REQUIRE(token_b->synced_patch_epoch.load() < patch_epoch);
    tracker.EndJit(token_a);
    tracker.BeginJit(token_a);
    REQUIRE(token_a->patch_sync_count.load() == 1);
    tracker.EndJit(token_a);
    tracker.BeginJit(token_b);
    REQUIRE(token_b->synced_patch_epoch.load() == patch_epoch);
    REQUIRE(token_b->patch_sync_count.load() == 1);
    tracker.EndJit(token_b);

    tracker.UnregisterRuntime(token_a);
    tracker.UnregisterRuntime(token_b);
}

TEST_CASE("region linker caches Far state and preserves dispatcher fallback",
          "[direct-link][trampoline][far]") {
    std::vector<UniformMapDesc> descriptors;
    auto config = TestConfig(descriptors);
    LinkManager manager;
    CodeCache cache{config, 1u << 20, FeatureSet{}};
    const int return_stub{};
    const int dispatcher_stub{};
    REQUIRE(cache.InitializeRegionTrampoline(
            manager,
            const_cast<int*>(&return_stub),
            const_cast<int*>(&dispatcher_stub)));
    auto site = cache.AllocCode(16);
    REQUIRE(site);
    const int owner_module{};
    const int owner_allocation{};
    const LinkSiteKey key{cache.GetRegion().id, site->offset};
    REQUIRE(manager.RegisterSite(key, kGuestTarget, {&owner_module, &owner_allocation}));

    const int outside_region_target{};
    const auto generation = manager.PublishTarget(
            kGuestTarget, const_cast<int*>(&outside_region_target), 0xfeed);
    TestState state{config.uniform_buffer_size};
    auto* context = cache.GetRegionLinkContext();
    REQUIRE(RegionLinkTrampolineSlow(context, state.state, site->exec_data) ==
            &outside_region_target);
    REQUIRE(manager.QuerySite(key)->state == LinkSiteState::Far);
    REQUIRE(manager.QuerySite(key)->target_generation == generation);
    // A cached Far record returns the target without attempting MarkFar or a
    // patch again. The instruction bytes remain untouched.
    REQUIRE(RegionLinkTrampolineSlow(context, state.state, site->exec_data) ==
            &outside_region_target);

    REQUIRE(manager.BeginTargetInvalidation(kGuestTarget).size() == 1);
    REQUIRE(RegionLinkTrampolineSlow(context, state.state, site->exec_data) ==
            &dispatcher_stub);
    REQUIRE(state.state->current_loc.Value() == kGuestTarget);
}

TEST_CASE("direct-link concurrent patch invalidation and mspace reuse stress",
          "[direct-link][stress]") {
#if defined(__aarch64__)
    std::vector<UniformMapDesc> descriptors;
    auto config = TestConfig(descriptors);
    TrampolinesArm64 runtime_trampolines{config, FeatureSet{}};
    LinkManager manager;
    CodeCache cache{config, 1u << 20, FeatureSet{}};
    auto* return_host = reinterpret_cast<void*>(runtime_trampolines.GetReturnHost());
    // The production dispatcher hardcodes the full L1/L2 table geometry. This
    // scratch harness uses tiny tables, so its preset dispatcher entry is a
    // faithful CodeMiss veneer instead of the production lookup body.
    auto dispatcher = cache.AllocCode(64);
    REQUIRE(dispatcher);
    EmitHaltTarget(*dispatcher, HaltReason::CodeMiss);
    REQUIRE(cache.InitializeRegionTrampoline(
            manager, return_host, dispatcher->exec_data));
    auto site = cache.AllocCode(64);
    REQUIRE(site);
    const auto bl = EncodeBL(
            static_cast<u8*>(cache.GetRegionTrampoline()) - site->exec_data);
    REQUIRE(bl);
    std::memcpy(site->rw_data, &*bl, sizeof(*bl));
    site->Flush();

    auto target = cache.AllocCode(64);
    REQUIRE(target);
    constexpr u64 kPoison = 0xdead'feed'bad0'c0deull;
    u64 fingerprint = 1;
    EmitFingerprintTarget(*target, fingerprint);
    const int owner_module{};
    const int owner_allocation{};
    const LinkSiteKey key{cache.GetRegion().id, site->offset};
    REQUIRE(manager.RegisterSite(key, kGuestTarget, {&owner_module, &owner_allocation}));
    REQUIRE(manager.PublishTarget(
                    kGuestTarget, target->exec_data, cache.GetRegion().id) != 0);

    SmcTracker tracker{0};
    tracker.EnableMultithreading();
    const unsigned thread_count = GetSvmConfig().direct_link_stress_threads;
    std::vector<std::unique_ptr<TestState>> states;
    std::vector<SmcTracker::RuntimeToken> tokens;
    std::vector<std::thread> workers;
    std::atomic_bool start{false};
    std::atomic_bool pause_entries{false};
    std::atomic_bool stop{false};
    std::atomic_bool failed{false};
    std::atomic<u64> executions{0};
    for (unsigned i = 0; i < thread_count; ++i) {
        states.emplace_back(std::make_unique<TestState>(config.uniform_buffer_size));
        tokens.push_back(tracker.RegisterRuntime(states.back()->l1));
    }
    for (unsigned i = 0; i < thread_count; ++i) {
        workers.emplace_back([&, i] {
            auto entry = runtime_trampolines.GetRuntimeEntry();
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (!stop.load(std::memory_order_acquire)) {
                while (pause_entries.load(std::memory_order_acquire) &&
                       !stop.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                tracker.BeginJit(tokens[i]);
                const auto halt = entry(states[i]->state, site->exec_data);
                const auto observed = states[i]->state->current_loc.Value();
                tracker.EndJit(tokens[i]);
                if (halt == HaltReason::PageFatal && observed == kPoison) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
                if (halt != HaltReason::PageFatal && halt != HaltReason::CodeMiss) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
                executions.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    start.store(true, std::memory_order_release);
    const bool long_mode = GetSvmConfig().direct_link_stress_long;
    const unsigned iterations = GetSvmConfig().direct_link_stress_iters;
    for (unsigned iteration = 0; iteration < iterations && !failed.load(); ++iteration) {
        const auto incoming = manager.BeginTargetInvalidation(kGuestTarget);
        REQUIRE(incoming.size() == 1);
        REQUIRE(PatchDirectBranch(cache.GetRegion(),
                                  site->exec_data,
                                  site->rw_data,
                                  *bl));
        const auto patch_epoch = tracker.AdvanceCodePatchEpoch();
        REQUIRE(tracker.GetCodePatchEpoch() == patch_epoch);
        pause_entries.store(true, std::memory_order_release);
        for (;;) {
            bool safe = true;
            for (const auto& token : tokens) {
                const auto active = token->active_epoch.load(std::memory_order_seq_cst);
                safe &= active == SmcTracker::kInactiveEpoch;
            }
            if (safe) break;
            std::this_thread::yield();
        }

        auto* old_exec = target->exec_data;
        REQUIRE(cache.FreeCode(old_exec));
        target = cache.AllocCode(64);
        REQUIRE(target);
        REQUIRE(target->exec_data == old_exec);
        EmitFingerprintTarget(*target, kPoison);
        for (unsigned spin = 0; spin < 32; ++spin) std::this_thread::yield();
        EmitFingerprintTarget(*target, ++fingerprint);
        REQUIRE(manager.PublishTarget(
                        kGuestTarget, target->exec_data, cache.GetRegion().id) != 0);
        pause_entries.store(false, std::memory_order_release);
    }
    stop.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    REQUIRE_FALSE(failed.load());
    REQUIRE(executions.load() > iterations);
    for (const auto& token : tokens) tracker.UnregisterRuntime(token);
#else
    SUCCEED("direct-link execution stress requires an AArch64 host");
#endif
}
