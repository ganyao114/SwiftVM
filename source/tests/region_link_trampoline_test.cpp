#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "aarch64/macro-assembler-aarch64.h"
#include "runtime/common/svm_config.h"
#include "runtime/backend/arm64/fpcr_mode.h"
#include "runtime/backend/arm64/region_link_trampoline.h"
#include "runtime/backend/arm64/continuation_contract.h"
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
        state->rsb_pointer = &rsb.rsb_frames[rsb_stack_size];
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

TEST_CASE("region cycle reason trampoline publishes the pending reason",
           "[direct-link][trampoline][cycle]") {
#if defined(__aarch64__)
    std::vector<UniformMapDesc> descriptors;
    auto config = TestConfig(descriptors);
    TrampolinesArm64 runtime_trampolines{config, FeatureSet{}};
    LinkManager manager;
    CodeCache cache{config, 1u << 20, FeatureSet{}};
    auto* return_host = reinterpret_cast<void*>(runtime_trampolines.GetReturnHost());
    REQUIRE(cache.InitializeRegionTrampoline(
            manager, return_host, return_host, std::make_shared<RetirementControl>()));
    REQUIRE(cache.GetCycleReasonRegionTrampoline());

    auto code = cache.AllocCode(sizeof(u32));
    REQUIRE(code);
    const auto branch = EncodeB(
            static_cast<u8*>(cache.GetCycleReasonRegionTrampoline()) -
            code->exec_data);
    REQUIRE(branch);
    std::memcpy(code->rw_data, &*branch, sizeof(*branch));
    code->Flush();

    const std::array cases{
            std::pair{u64{0}, HaltReason::CodeMiss},
            std::pair{u64{1} << 63, HaltReason::Signal},
            std::pair{u64{1} << 62, HaltReason::Retirement},
            std::pair{(u64{1} << 62) | (u64{1} << 63) | 1, HaltReason::Retirement},
    };
    for (const auto& [request, expected] : cases) {
        TestState test_state{config.uniform_buffer_size};
        test_state.state->exit_request = request;
        REQUIRE(runtime_trampolines.GetRuntimeEntry()(test_state.state,
                                                       code->exec_data) == expected);
    }
#else
    SUCCEED("region trampoline execution requires an AArch64 host");
#endif
}

TEST_CASE("region exit trampolines merge flags before returning",
          "[direct-link][trampoline][cycle][return][flags]") {
#if defined(__aarch64__)
    std::vector<UniformMapDesc> descriptors;
    auto config = TestConfig(descriptors);
    TrampolinesArm64 runtime_trampolines{config, FeatureSet{}};
    LinkManager manager;
    CodeCache cache{config, 1u << 20, FeatureSet{}};
    auto* return_host = reinterpret_cast<void*>(runtime_trampolines.GetReturnHost());
    REQUIRE(cache.InitializeRegionTrampoline(
            manager, return_host, return_host, std::make_shared<RetirementControl>()));

    constexpr u64 packed_af = u64{1} << 26;
    constexpr u64 packed_zc = (u64{1} << 30) | (u64{1} << 29);
    constexpr u64 old_parity = 0xa5;
    constexpr u64 token_parity = 0x3c;
    for (const bool cycle : {false, true}) {
        for (const bool token : {false, true}) {
        auto code = cache.AllocCode(128);
        REQUIRE(code);
        MacroAssembler masm;
        masm.Mov(x26, packed_af | old_parity);
        masm.Mov(x12, token_parity);
        masm.Mov(x0, 1);
        masm.Cmp(x0, x0);
        if (!cycle) {
            masm.Mov(w11, static_cast<u32>(HaltReason::PageFatal));
            masm.Str(w11, MemOperand(x28, state_offset_halt_reason));
        }
        const u32 branch_offset =
                static_cast<u32>(masm.GetBuffer()->GetSizeInBytes());
        masm.dc32(*EncodeB(0));
        CopyAssembler(*code, 0, masm);

        auto* trampoline = static_cast<u8*>(cycle
                ? (token ? cache.GetCycleFlagsMergeTokenRegionTrampoline()
                         : cache.GetCycleFlagsMergeRegionTrampoline())
                : (token ? cache.GetReturnFlagsMergeTokenRegionTrampoline()
                         : cache.GetReturnFlagsMergeRegionTrampoline()));
        REQUIRE(trampoline);
        const auto branch = EncodeB(
                trampoline - (code->exec_data + branch_offset));
        REQUIRE(branch);
        std::memcpy(code->rw_data + branch_offset, &*branch, sizeof(*branch));
        code->Flush();

        TestState test_state{config.uniform_buffer_size};
        REQUIRE(runtime_trampolines.GetRuntimeEntry()(test_state.state,
                                                       code->exec_data) ==
                (cycle ? HaltReason::CodeMiss : HaltReason::PageFatal));
        const u64 parity = token ? token_parity : old_parity;
        REQUIRE(test_state.state->host_cpu_flags ==
                (packed_zc | packed_af | parity));
        }
    }
#else
    SUCCEED("region trampoline execution requires an AArch64 host");
#endif
}

TEST_CASE("region masked flags trampolines preserve unrequested bits",
          "[direct-link][trampoline][flags]") {
#if defined(__aarch64__)
    std::vector<UniformMapDesc> descriptors;
    auto config = TestConfig(descriptors);
    TrampolinesArm64 runtime_trampolines{config, FeatureSet{}};
    LinkManager manager;
    CodeCache cache{config, 1u << 20, FeatureSet{}};
    auto* return_host = reinterpret_cast<void*>(runtime_trampolines.GetReturnHost());
    REQUIRE(cache.InitializeRegionTrampoline(
            manager, return_host, return_host, std::make_shared<RetirementControl>()));

    constexpr u64 initial_nzcv = 0x9000'0000u;
    constexpr u64 packed_af = u64{1} << 26;
    constexpr u64 old_parity = 0xa5;
    constexpr u64 token_parity = 0x3c;
    const std::array masks{
            std::pair{u64{0xC000'0000u}, u64{0x5000'0000u}},
            std::pair{u64{0xA000'0000u}, u64{0x3000'0000u}},
    };
    for (const auto& [mask, expected_nzcv] : masks) {
        for (const bool token : {false, true}) {
            auto code = cache.AllocCode(128);
            REQUIRE(code);
            MacroAssembler masm;
            Label resume;
            masm.Mov(x26, initial_nzcv | packed_af | old_parity);
            masm.Mov(x12, token_parity);
            masm.Mov(x0, 1);
            masm.Cmp(x0, x0);
            masm.Adr(x17, &resume);
            const u32 branch_offset =
                    static_cast<u32>(masm.GetBuffer()->GetSizeInBytes());
            masm.dc32(*EncodeB(0));
            masm.Bind(&resume);
            masm.Mov(w11, static_cast<u32>(HaltReason::PageFatal));
            masm.Str(w11, MemOperand(x28, state_offset_halt_reason));
            masm.Ret();
            CopyAssembler(*code, 0, masm);

            const auto mask_index = static_cast<u8>(mask >> 28);
            auto* trampoline = static_cast<u8*>(
                    token ? cache.GetFlagsMaskMergeTokenRegionTrampoline(mask_index)
                          : cache.GetFlagsMaskMergeRegionTrampoline(mask_index));
            REQUIRE(trampoline);
            const auto branch = EncodeB(
                    trampoline - (code->exec_data + branch_offset));
            REQUIRE(branch);
            std::memcpy(code->rw_data + branch_offset, &*branch, sizeof(*branch));
            code->Flush();

            TestState test_state{config.uniform_buffer_size};
            REQUIRE(runtime_trampolines.GetRuntimeEntry()(test_state.state,
                                                           code->exec_data) ==
                    HaltReason::PageFatal);
            const u64 parity = token ? token_parity : old_parity;
            REQUIRE(test_state.state->host_cpu_flags ==
                    (expected_nzcv | packed_af | parity));
        }
    }
#else
    SUCCEED("region trampoline execution requires an AArch64 host");
#endif
}

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
                        manager, return_host, return_host, std::make_shared<RetirementControl>()));
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
                        cache.GetRegion().id,
                        {},
                        code->exec_data + kTarget2);
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
                        reinterpret_cast<uintptr_t>(code->exec_data + kTarget2));
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

TEST_CASE("call-kind region links preserve the host continuation",
          "[direct-link][trampoline][call]") {
#if defined(__aarch64__)
    std::vector<UniformMapDesc> descriptors;
    auto config = TestConfig(descriptors);
    config.global_opts = Optimizations::ReturnStackBuffer;
    TrampolinesArm64 runtime_trampolines{config, FeatureSet{}};
    LinkManager manager;
    CodeCache cache{config, 1u << 20, FeatureSet{}};
    auto* return_host = reinterpret_cast<void*>(runtime_trampolines.GetReturnHost());
    REQUIRE(cache.InitializeRegionTrampoline(
            manager, return_host, return_host, std::make_shared<RetirementControl>()));
    auto code = cache.AllocCode(256);
    REQUIRE(code);

    constexpr u64 guest_return = 0x12345678;
    MacroAssembler source;
    source.Mov(x14, guest_return);
    const u32 site_offset = source.GetBuffer()->GetSizeInBytes();
    source.dc32(*EncodeBL(0));
    const u32 continuation_offset = source.GetBuffer()->GetSizeInBytes();
    source.Mov(x16, reinterpret_cast<uintptr_t>(return_host));
    source.Br(x16);
    CopyAssembler(*code, 0, source);

    constexpr u32 target_offset = 128;
    MacroAssembler target;
    target.Stp(x14, x30, MemOperand(x25, -16, PreIndex));
    target.Mov(w11, static_cast<u32>(HaltReason::PageFatal));
    target.Str(w11, MemOperand(x28, state_offset_halt_reason));
    target.Ret();
    CopyAssembler(*code, target_offset, target);

    auto* site = code->exec_data + site_offset;
    const auto unlinked = EncodeBL(
            static_cast<u8*>(cache.GetRegionTrampoline()) - site);
    REQUIRE(unlinked);
    std::memcpy(code->rw_data + site_offset, &*unlinked, sizeof(*unlinked));
    code->Flush();

    const int owner_module{};
    const int owner_allocation{};
    const LinkSiteKey key{cache.GetRegion().id, code->offset + site_offset};
    REQUIRE(manager.RegisterSite(key,
                                 kGuestTarget,
                                 {&owner_module, &owner_allocation},
                                 nullptr,
                                 LinkSiteKind::Call));
    REQUIRE(manager.PublishTarget(kGuestTarget,
                                  code->exec_data + target_offset,
                                  cache.GetRegion().id,
                                  {},
                                  nullptr,
                                  nullptr,
                                  code->exec_data + target_offset) != 0);

    TestState state{config.uniform_buffer_size};
    auto* empty = state.state->rsb_pointer;
    auto entry = runtime_trampolines.GetRuntimeEntry();
    REQUIRE(entry(state.state, code->exec_data) == HaltReason::PageFatal);
    REQUIRE(state.state->rsb_pointer == empty - 1);
    REQUIRE(state.state->rsb_pointer->guest_location == guest_return);
    REQUIRE(state.state->rsb_pointer->dispatch_index ==
            reinterpret_cast<uintptr_t>(code->exec_data + continuation_offset));
    REQUIRE(manager.QuerySite(key)->state == LinkSiteState::Linked);
    REQUIRE(DecodeBranchTarget(site, LoadInsn(site)) ==
            reinterpret_cast<uintptr_t>(code->exec_data + target_offset));
#else
    SUCCEED("call-kind trampoline execution requires an AArch64 host");
#endif
}

TEST_CASE("region linker distinguishes absent dispatch from invalidated targets",
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
            const_cast<int*>(&dispatcher_stub),
            std::make_shared<RetirementControl>()));
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
    bool invalidated = true;
    REQUIRE_FALSE(manager.QueryTarget(kGuestTarget + 4, &invalidated));
    REQUIRE_FALSE(invalidated);
    REQUIRE(RegionLinkTrampolineSlow(context, state.state, site->exec_data) ==
            &outside_region_target);
    REQUIRE(manager.QuerySite(key)->state == LinkSiteState::Far);
    REQUIRE(manager.QuerySite(key)->target_generation == generation);
    // A cached Far record returns the target without attempting MarkFar or a
    // patch again. The instruction bytes remain untouched.
    REQUIRE(RegionLinkTrampolineSlow(context, state.state, site->exec_data) ==
            &outside_region_target);

    REQUIRE(manager.BeginTargetInvalidation(kGuestTarget).size() == 1);
    REQUIRE_FALSE(manager.QueryTarget(kGuestTarget, &invalidated));
    REQUIRE(invalidated);
    REQUIRE(RegionLinkTrampolineSlow(context, state.state, site->exec_data) ==
            &return_stub);
    REQUIRE(state.state->current_loc.Value() == kGuestTarget);
    REQUIRE(state.state->halt_reason == HaltReason::CodeMiss);
}

TEST_CASE("signal-invalidated cold links return host with call traversal intact", "[direct-link][trampoline][smc]") {
    for (const bool call : {false, true}) {
        std::vector<UniformMapDesc> descriptors;
        auto config = TestConfig(descriptors);
        LinkManager manager;
        CodeCache cache{config, 1u << 20, FeatureSet{}};
        alignas(4) int return_stub{}, dispatcher_stub{}, target_stub{};
        REQUIRE(cache.InitializeRegionTrampoline(
                manager, &return_stub, &dispatcher_stub, std::make_shared<RetirementControl>()));
        auto site = cache.AllocCode(16);
        REQUIRE(site);
        const int owner_module{}, owner_allocation{};
        const LinkSiteKey key{cache.GetRegion().id, site->offset};
        REQUIRE(manager.RegisterSite(key, kGuestTarget, {&owner_module, &owner_allocation},
                nullptr, call ? LinkSiteKind::Call : LinkSiteKind::Unconditional));
        TestState state{config.uniform_buffer_size};
        auto* context = cache.GetRegionLinkContext();
        const auto absent = RegionLinkTrampolineSlow(context, state.state, site->exec_data);
        REQUIRE(absent == ContinuationContract::EncodeTraversal(&dispatcher_stub,
                call ? ContinuationTraversal::CallMiss : ContinuationTraversal::Branch));
        (void)manager.PublishTarget(kGuestTarget, &target_stub, 0xfeed, {}, nullptr, nullptr, &target_stub);
        REQUIRE(manager.SignalInvalidateTarget(kGuestTarget).found);
        bool invalidated = false;
        REQUIRE_FALSE(manager.QueryTarget(kGuestTarget, &invalidated));
        REQUIRE(invalidated);
        const auto result = RegionLinkTrampolineSlow(context, state.state, site->exec_data);
        REQUIRE(result == ContinuationContract::EncodeTraversal(&return_stub,
                call ? ContinuationTraversal::CallMiss : ContinuationTraversal::Branch));
        REQUIRE(state.state->current_loc.Value() == kGuestTarget);
        REQUIRE(state.state->halt_reason == HaltReason::CodeMiss);
    }
}

TEST_CASE("closed cold links preserve traversal without patching or changing statistics",
          "[direct-link][trampoline][retirement]") {
    for (const bool sealed : {false, true}) {
        for (const bool call : {false, true}) {
            for (const auto link_state : {LinkSiteState::Unlinked, LinkSiteState::Linked,
                                          LinkSiteState::Far}) {
                std::vector<UniformMapDesc> descriptors;
                auto config = TestConfig(descriptors);
                LinkManager manager;
                CodeCache cache{config, 1u << 20, FeatureSet{}};
                auto control = std::make_shared<RetirementControl>();
                alignas(4) int return_stub{}, dispatcher_stub{};
                REQUIRE(cache.InitializeRegionTrampoline(
                        manager, &return_stub, &dispatcher_stub, control));
                REQUIRE(cache.InitializeRegionTrampoline(
                        manager, &return_stub, &dispatcher_stub, control));
                REQUIRE_FALSE(cache.InitializeRegionTrampoline(
                        manager, &return_stub, &dispatcher_stub,
                        std::make_shared<RetirementControl>()));
                auto site = cache.AllocCode(16);
                auto target = cache.AllocCode(16);
                REQUIRE(site);
                REQUIRE(target);
                const u32 original = 0x94000000;
                std::memcpy(site->rw_data, &original, sizeof(original));
                const int owner_module{}, owner_allocation{};
                const LinkSiteKey key{cache.GetRegion().id, site->offset};
                REQUIRE(manager.RegisterSite(key, kGuestTarget, {&owner_module, &owner_allocation},
                        nullptr, call ? LinkSiteKind::Call : LinkSiteKind::Unconditional));
                const auto generation = manager.PublishTarget(kGuestTarget, target->exec_data,
                        cache.GetRegion().id, {}, nullptr, nullptr, target->exec_data);
                REQUIRE(generation != 0);
                if (link_state == LinkSiteState::Linked)
                    REQUIRE(manager.MarkLinked(key, generation, [](const auto&) { return true; }));
                if (link_state == LinkSiteState::Far) REQUIRE(manager.MarkFar(key, generation));
                const auto before = manager.GetStats();
                const auto ticket = RetirementControl::RequestDrain(control);
                if (sealed) REQUIRE(control->SealPublications(ticket));
                TestState state{config.uniform_buffer_size};
                const auto result = RegionLinkTrampolineSlow(
                        cache.GetRegionLinkContext(), state.state, site->exec_data);
                REQUIRE(result == ContinuationContract::EncodeTraversal(&return_stub,
                        call ? ContinuationTraversal::CallMiss : ContinuationTraversal::Branch));
                REQUIRE(state.state->halt_reason == HaltReason::Retirement);
                REQUIRE(state.state->current_loc.Value() == kGuestTarget);
                REQUIRE(LoadInsn(site->rw_data) == original);
                REQUIRE(manager.QuerySite(key)->state == link_state);
                const auto after = manager.GetStats();
                REQUIRE(after.linker_calls == before.linker_calls);
                REQUIRE(after.linked == before.linked);
                REQUIRE(after.far == before.far);
                REQUIRE(control->WaitDrain(ticket) == RetirementControl::DrainStatus::Ready);
                REQUIRE(control->SealPublications(ticket));
                REQUIRE(control->Resume(ticket));
            }
        }
    }
}

TEST_CASE("closed generated cold links bypass a dispatcher holding old code",
          "[direct-link][trampoline][retirement][regression]") {
#if defined(__aarch64__)
    std::vector<UniformMapDesc> descriptors;
    const auto config = TestConfig(descriptors);
    TrampolinesArm64 trampolines{config, FeatureSet{}};
    LinkManager manager;
    CodeCache cache{config, 1u << 20, FeatureSet{}};
    auto old_target = cache.AllocCode(32);
    auto dispatcher = cache.AllocCode(32);
    REQUIRE(old_target);
    REQUIRE(dispatcher);
    EmitHaltTarget(*old_target, HaltReason::CallHost);
    MacroAssembler stale_dispatch;
    stale_dispatch.Mov(x16, reinterpret_cast<uintptr_t>(old_target->exec_data));
    stale_dispatch.Br(x16);
    CopyAssembler(*dispatcher, 0, stale_dispatch);
    dispatcher->Flush();
    auto control = std::make_shared<RetirementControl>();
    auto* return_host = reinterpret_cast<void*>(trampolines.GetReturnHost());
    REQUIRE(cache.InitializeRegionTrampoline(manager, return_host, dispatcher->exec_data, control));
    auto site = cache.AllocCode(16);
    REQUIRE(site);
    const auto unlinked = EncodeBL(static_cast<u8*>(cache.GetRegionTrampoline()) - site->exec_data);
    REQUIRE(unlinked);
    std::memcpy(site->rw_data, &*unlinked, sizeof(*unlinked));
    site->Flush();
    const int owner_module{}, owner_allocation{};
    const LinkSiteKey key{cache.GetRegion().id, site->offset};
    REQUIRE(manager.RegisterSite(key, kGuestTarget, {&owner_module, &owner_allocation}));
    REQUIRE(manager.PublishTarget(kGuestTarget, old_target->exec_data, cache.GetRegion().id) != 0);
    const auto ticket = RetirementControl::RequestDrain(control);
    TestState state{config.uniform_buffer_size};
    REQUIRE(trampolines.GetRuntimeEntry()(state.state, site->exec_data) == HaltReason::Retirement);
    REQUIRE(state.state->current_loc.Value() == kGuestTarget);
    REQUIRE(LoadInsn(site->rw_data) == *unlinked);
    REQUIRE(manager.GetStats().linker_calls == 0);
    REQUIRE(control->SealPublications(ticket));
    REQUIRE(control->Resume(ticket));
#else
    SUCCEED("native closed cold-link recovery requires AArch64");
#endif
}

TEST_CASE("invalidated cold targets bypass a dispatcher holding retired code", "[direct-link][trampoline][smc][regression]") {
#if defined(__aarch64__)
    std::vector<UniformMapDesc> descriptors;
    const auto config = TestConfig(descriptors);
    TrampolinesArm64 trampolines{config, FeatureSet{}};
    LinkManager manager;
    CodeCache cache{config, 1u << 20, FeatureSet{}};
    auto retired = cache.AllocCode(32);
    auto dispatcher = cache.AllocCode(32);
    REQUIRE(retired);
    REQUIRE(dispatcher);
    EmitHaltTarget(*retired, HaltReason::CallHost);
    MacroAssembler stale_dispatch;
    stale_dispatch.Mov(x16, reinterpret_cast<uintptr_t>(retired->exec_data));
    stale_dispatch.Br(x16);
    CopyAssembler(*dispatcher, 0, stale_dispatch);
    dispatcher->Flush();
    auto* return_host = reinterpret_cast<void*>(trampolines.GetReturnHost());
    REQUIRE(cache.InitializeRegionTrampoline(
            manager, return_host, dispatcher->exec_data, std::make_shared<RetirementControl>()));
    auto site = cache.AllocCode(16);
    REQUIRE(site);
    const auto unlinked = EncodeBL(static_cast<u8*>(cache.GetRegionTrampoline()) - site->exec_data);
    REQUIRE(unlinked);
    std::memcpy(site->rw_data, &*unlinked, sizeof(*unlinked));
    site->Flush();
    const int owner_module{}, owner_allocation{};
    const LinkSiteKey key{cache.GetRegion().id, site->offset};
    REQUIRE(manager.RegisterSite(key, kGuestTarget, {&owner_module, &owner_allocation}));
    (void)manager.PublishTarget(kGuestTarget, retired->exec_data, cache.GetRegion().id);
    REQUIRE(manager.SignalInvalidateTarget(kGuestTarget).found);
    TestState state{config.uniform_buffer_size};
    REQUIRE(trampolines.GetRuntimeEntry()(state.state, site->exec_data) == HaltReason::CodeMiss);
    REQUIRE(state.state->current_loc.Value() == kGuestTarget);
#else
    SUCCEED("native stale-dispatch recovery requires AArch64");
#endif
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
            manager, return_host, dispatcher->exec_data, std::make_shared<RetirementControl>()));
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
    const auto progress_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool progress_timeout = false;
    while (executions.load(std::memory_order_acquire) < thread_count && !failed.load()) {
        if (std::chrono::steady_clock::now() >= progress_deadline) {
            progress_timeout = true;
            break;
        }
        std::this_thread::yield();
    }
    const bool long_mode = GetSvmConfig().direct_link_stress_long;
    const unsigned iterations = GetSvmConfig().direct_link_stress_iters;
    for (unsigned iteration = 0; iteration < iterations && !failed.load() && !progress_timeout; ++iteration) {
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
        const auto before_resume = executions.load(std::memory_order_acquire);
        pause_entries.store(false, std::memory_order_release);
        // A yield doesn't guarantee any worker runs. Require actual generated
        // code execution between patch cycles instead of a timing assumption.
        while (executions.load(std::memory_order_acquire) == before_resume && !failed.load()) {
            if (std::chrono::steady_clock::now() >= progress_deadline) {
                progress_timeout = true;
                break;
            }
            std::this_thread::yield();
        }
    }
    stop.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    REQUIRE_FALSE(failed.load());
    REQUIRE_FALSE(progress_timeout);
    REQUIRE(executions.load() > iterations);
    for (const auto& token : tokens) tracker.UnregisterRuntime(token);
#else
    SUCCEED("direct-link execution stress requires an AArch64 host");
#endif
}

TEST_CASE("unsupported ARM64 host-call trampolines fail explicitly", "[trampoline][call-host]") {
#if defined(__aarch64__)
    std::vector<UniformMapDesc> descriptors;
    auto config = TestConfig(descriptors);
    TrampolinesArm64 trampolines{config, FeatureSet{}};
    HostFunction empty{.module = "test", .name = "void",
                       .signatures = {}, .addr = 0x7300,
                       .impl = reinterpret_cast<void*>(+[]() {})};
    HostFunction integer{.module = "test", .name = "integer",
                         .signatures = {ParamType::Uint64, ParamType::Uint64, ParamType::Uint64},
                         .addr = 0x7308,
                         .impl = reinterpret_cast<void*>(+[](u64 a, u64 b) { return a + b; })};
    HostFunction floating{.module = "test", .name = "floating",
                          .signatures = {ParamType::Float64, ParamType::Float64},
                          .addr = 0x7310,
                          .impl = reinterpret_cast<void*>(+[](double a) { return a; })};
    // Query only: no trampoline or host implementation is invoked.
    for (auto* function : {&empty, &integer, &floating}) {
        for (const auto frontend : {kX86, kX86_64, kArm64}) {
            CHECK_FALSE(trampolines.GetCallHost(function, frontend).has_value());
            CHECK_FALSE(trampolines.GetCallHost(function, frontend).has_value());
        }
    }
#else
    SUCCEED("host-call availability contract requires an AArch64 host");
#endif
}
