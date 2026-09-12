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
#include "runtime/common/svm_config.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/region_link_trampoline.h"
#include "runtime/backend/context.h"
#include "runtime/backend/link_manager.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/cast_utils.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/block.h"
#include "runtime/ir/hir_builder.h"
#include "translator/x86/cpu.h"

namespace {

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const char* value) : name_(name) {
        if (const char* old = swift::runtime::GetRawSvmConfigEnvForTest(name)) {
            old_ = old;
        }
        swift::runtime::SetSvmConfigEnvForTest(name, value, 1);
    }

    ~ScopedEnvironment() {
        if (old_) {
            swift::runtime::SetSvmConfigEnvForTest(name_.c_str(), old_->c_str(), 1);
        } else {
            swift::runtime::UnsetSvmConfigEnvForTest(name_.c_str());
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

IntrusivePtr<Block> BuildStaticForwardSource(VAddr guest, VAddr target) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    block->SetLocation(Lambda{Imm{target}});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

void* TranslateContinuationReturnSource(
        const std::shared_ptr<Module>& module,
        VAddr guest) {
    HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(Location{guest}, Location{guest + 1});
    const auto target = function
                                ->LoadUniform<TypedValue<ValueType::U64>>(
                                        Uniform{0, ValueType::U64})
                                .SetType(ValueType::U64);
    function->SetLocation(Lambda{target});
    function->EndBlock(terminal::PopRSBHint{});
    function->EndFunction();
    return TranslateIR(module, function);
}

void* TranslateIndirectCallSource(const std::shared_ptr<Module>& module,
                                  VAddr guest,
                                  VAddr return_guest) {
    HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(Location{guest},
                                            Location{return_guest + 1});
    const auto target = function
                                ->LoadUniform<TypedValue<ValueType::U64>>(
                                        Uniform{8, ValueType::U64})
                                .SetType(ValueType::U64);
    const auto return_value = function
                                      ->LoadImm(Imm{return_guest})
                                      .SetType(ValueType::U64);
    function->CallReturn(return_value,
                         Imm{return_guest},
                         Lambda{target});
    function->SetLocation(Lambda{target});
    builder.RegisterCallReturn(Location{return_guest});
    function->EndBlock(terminal::ReturnToDispatch{});
    builder.SetCurBlock(Location{return_guest});
    builder.AdvancePC(Imm{1});
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    return TranslateIR(module, function);
}

void* TranslateStaticCallSource(const std::shared_ptr<Module>& module,
                                VAddr guest,
                                VAddr target_guest,
                                VAddr return_guest) {
    HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(Location{guest}, Location{return_guest + 1});
    const auto return_value = function->LoadImm(Imm{return_guest}).SetType(ValueType::U64);
    function->CallReturn(return_value, Imm{return_guest}, Lambda{Imm{target_guest}});
    function->SetLocation(Lambda{Imm{target_guest}});
    builder.RegisterCallReturn(Location{return_guest});
    function->EndBlock(terminal::ReturnToDispatch{});
    builder.SetCurBlock(Location{return_guest});
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    return TranslateIR(module, function);
}

void* TranslateCallTarget(const std::shared_ptr<Module>& module,
                          VAddr guest,
                          u64 fingerprint) {
    HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(Location{guest}, Location{guest + 1});
    const auto value = function
                               ->LoadImm(Imm{fingerprint})
                               .SetType(ValueType::U64);
    function->StoreUniform(Uniform{0, ValueType::U64}, value);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    return TranslateIR(module, function);
}

void* TranslateReturningCallTarget(const std::shared_ptr<Module>& module,
                                   VAddr guest,
                                   u64 fingerprint) {
    HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(Location{guest}, Location{guest + 1});
    const auto value = function->LoadImm(Imm{fingerprint}).SetType(ValueType::U64);
    function->StoreUniform(Uniform{0, ValueType::U64}, value);
    const auto return_target =
            function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{16, ValueType::U64})
                    .SetType(ValueType::U64);
    function->SetLocation(Lambda{return_target});
    function->EndBlock(terminal::PopRSBHint{});
    function->EndFunction();
    return TranslateIR(module, function);
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

void* TranslateFlagsKillingTarget(const std::shared_ptr<Module>& module,
                                  VAddr guest,
                                  Flags saved_flags = Flags::NZCV) {
    HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(Location{guest}, Location{guest + 3});
    const auto left = function->LoadImm(Imm{u8{7}}).SetType(ValueType::U8);
    const auto right = function->LoadImm(Imm{u8{3}}).SetType(ValueType::U8);
    const auto result = function->Sub(left, Operand{right}).SetType(ValueType::U8);
    function->SaveFlags(result, saved_flags);
    function->AdvancePC(Imm{u64{1}});
    const auto condition = function->LocalCondSet(Cond::EQ).SetType(ValueType::U8);
    auto [then_block, else_block] = builder.If(terminal::If{
            condition,
            terminal::LinkBlock{Location{guest + 1}},
            terminal::LinkBlock{Location{guest + 2}},
    });
    builder.SetCurBlock(then_block);
    function->EndBlock(terminal::ReturnToHost{});
    builder.SetCurBlock(else_block);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    return TranslateIR(module, function);
}

void* TranslatePendingFlagsSource(const std::shared_ptr<Module>& module,
                                  VAddr guest,
                                  VAddr then_target,
                                  VAddr else_target) {
    HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(Location{guest}, Location{guest + 2});
    auto* body = builder.LinkBlock(
            terminal::LinkBlock{Location{guest + 1}});
    builder.SetCurBlock(body);
    const auto selector = function->LoadUniform(
            Uniform{8, ValueType::U8}).SetType(ValueType::U8);
    const auto one = function->LoadImm(Imm{u8{1}}).SetType(ValueType::U8);
    const auto result = function->Sub(selector, Operand{one}).SetType(ValueType::U8);
    function->SaveFlags(result, Flags::NZCV);
    function->AdvancePC(Imm{u64{1}});
    const auto condition = function->LocalCondSet(Cond::EQ).SetType(ValueType::U8);
    function->EndBlock(terminal::If{
            condition,
            terminal::LinkBlock{Location{then_target}},
            terminal::LinkBlock{Location{else_target}},
    });
    function->EndFunction();
    return TranslateIR(module, function);
}

void* TranslatePendingFlagsStaticSource(const std::shared_ptr<Module>& module,
                                        VAddr guest,
                                        VAddr target,
                                        Flags saved_flags = Flags::All,
                                        EdgeCarryPolarity carry_polarity =
                                                EdgeCarryPolarity::Unknown) {
    HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(Location{guest}, Location{guest + 2});
    auto* body = builder.LinkBlock(terminal::LinkBlock{Location{guest + 1}});
    builder.SetCurBlock(body);
    const auto value = function->LoadUniform(
            Uniform{8, ValueType::U8}).SetType(ValueType::U8);
    const auto one = function->LoadImm(Imm{u8{1}}).SetType(ValueType::U8);
    const auto result = function->Sub(value, Operand{one}).SetType(ValueType::U8);
    function->SaveFlags(result, saved_flags);
    if (True(saved_flags & Flags::Carry)) {
        if (carry_polarity == EdgeCarryPolarity::Direct) {
            function->InvertCarry();
        } else if (carry_polarity == EdgeCarryPolarity::Inverted) {
            const auto inverted = function->LoadImm(Imm{u8{1}})
                                          .SetType(ValueType::U8);
            function->StoreUniform(
                    Uniform{offsetof(swift::x86::ThreadContext64,
                                     carry_inverted),
                            ValueType::U8},
                    inverted);
        }
    }
    function->AdvancePC(Imm{u64{1}});
    function->SetLocation(Lambda{Imm{target}});
    function->EndBlock(terminal::ReturnToDispatch{});
    function->EndFunction();
    return TranslateIR(module, function);
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

u64 RingTargetCheck() {
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

IntrusivePtr<Block> BuildIndirectRingBlock(VAddr guest) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    const auto entries =
            block->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    const auto one = block->LoadImm(Imm{u64{1}}).SetType(ValueType::U64);
    const auto next = block->Add(entries, Operand{one}).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, next);
    const auto target =
            block->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    block->SetLocation(Lambda{target});
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

u64 ConditionalRingSelect() {
    return g_conditional_selector.fetch_add(1, std::memory_order_seq_cst) & 1u;
}

IntrusivePtr<Block> BuildRingBlock(VAddr guest, VAddr target, bool check_target) {
    IntrusivePtr<Block> block{new Block(0, Location{guest})};
    block->SetEndLocation(Location{guest + 1});
    if (check_target) {
        (void)block
                ->CallLambda(Lambda{Imm{static_cast<u64>(reinterpret_cast<uintptr_t>(
                        FptrCast(&RingTargetCheck)))}})
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
                    FptrCast(&RingTargetCheck)))}})
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

struct RegionBranchRun {
    HaltReason halt{};
    u64 selector{};
    u64 observed_flags{};
    u32 code_size{};
    u64 code_hash{};
    bool branch_precedes_merge{};
};

RegionBranchRun RunRegionBranchFunction(bool enabled,
                                        bool observe_before_overwrite = false,
                                        bool helper_after_producer = false,
                                        u8 initial_selector = 1,
                                        bool compatible_fallthrough = false,
                                        Flags source_flags = Flags::All,
                                        bool sse42_target = false) {
    constexpr VAddr source_guest = 0x2100;
    constexpr VAddr hot_guest = 0x2180;
    constexpr VAddr cold_guest = 0x2200;
    constexpr u32 selector_offset = 8;
    constexpr u32 result_offset = sizeof(swift::x86::ThreadContext64);

    FeatureSet features{};
    features.flags_region_branch = enabled;
    Config config{
            .loc_start = 0,
            .loc_end = 0x4000,
            .enable_jit = true,
            .enable_asm_interp = false,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = result_offset + sizeof(u64),
            .region_edges = true,
    };
    AddressSpace space{config};
    ModuleConfig module_config{.read_only = true};
    module_config.feature_overrides.Set(FeatureId::flags_region_branch, enabled);
    auto module = space.MapModule(source_guest, 0x2300, module_config);

    HIRBuilder builder{1, true, features};
    auto* function = builder.AppendFunction(Location{source_guest},
                                            Location{cold_guest + 1});
    if (observe_before_overwrite) {
        const auto token = function->LoadImm(Imm{u64{0}}).SetType(ValueType::U64);
        const auto incoming = function->GetFlags(token, Flags::All)
                                      .SetType(ValueType::U64);
        function->StoreUniform(Uniform{result_offset, ValueType::U64}, incoming);
        function->AdvancePC(Imm{u64{1}});
    }
    const auto selector = function->LoadUniform(
            Uniform{selector_offset, ValueType::U8}).SetType(ValueType::U8);
    const auto one = function->LoadImm(Imm{u8{1}}).SetType(ValueType::U8);
    const auto result = function->Sub(selector, Operand{one}).SetType(ValueType::U8);
    function->SaveFlags(result, source_flags);
    const auto polarity = function->LoadImm(Imm{u8{1}}).SetType(ValueType::U8);
    function->StoreUniform(
            Uniform{offsetof(swift::x86::ThreadContext64, carry_inverted),
                    ValueType::U8},
            polarity);
    if (helper_after_producer) {
        (void)function
                ->CallLambda(Lambda{Imm{static_cast<u64>(reinterpret_cast<uintptr_t>(
                        FptrCast(&RingTargetCheck)))}})
                .SetType(ValueType::U64);
    }
    function->AdvancePC(Imm{u64{1}});
    const auto condition = function->LocalCondSet(Cond::EQ).SetType(ValueType::U8);
    const auto branch_terminal = compatible_fallthrough
            ? terminal::If{
                      condition,
                      terminal::LinkBlock{Location{hot_guest}},
                      terminal::LinkBlock{Location{cold_guest}},
              }
            : terminal::If{
                      condition,
                      terminal::LinkBlock{Location{cold_guest}},
                      terminal::LinkBlock{Location{hot_guest}},
              };
    auto [else_block, then_block] = builder.If(branch_terminal);
    auto* hot = compatible_fallthrough ? then_block : else_block;
    auto* cold = compatible_fallthrough ? else_block : then_block;
    REQUIRE(hot != nullptr);
    REQUIRE(cold != nullptr);

    builder.SetCurBlock(hot);
    auto observe_target_flags = [&] {
        const auto hot_token = function->LoadImm(Imm{u64{0}}).SetType(ValueType::U64);
        const auto incoming = function->GetFlags(hot_token, Flags::All)
                                      .SetType(ValueType::U64);
        function->StoreUniform(Uniform{result_offset, ValueType::U64}, incoming);
        function->AdvancePC(Imm{u64{1}});
    };
    if (sse42_target) {
        const auto left = function->LoadUniform(Uniform{16, ValueType::V128});
        const auto right = function->LoadUniform(Uniform{32, ValueType::V128});
        const auto packed = function->Sse42Str(left, right, Imm{u64{0x02}})
                                    .SetType(ValueType::U64);
        if (observe_before_overwrite) {
            observe_target_flags();
        }
        function->PublishSse42StrFlags(packed, Flags::All);
    } else {
        if (observe_before_overwrite) {
            observe_target_flags();
        }
        const auto hot_left = function->LoadImm(Imm{u8{7}}).SetType(ValueType::U8);
        const auto hot_right = function->LoadImm(Imm{u8{3}}).SetType(ValueType::U8);
        const auto hot_result = function->Sub(hot_left, Operand{hot_right})
                                        .SetType(ValueType::U8);
        function->SaveFlags(hot_result, Flags::All);
    }
    const auto hot_polarity = function->LoadImm(Imm{u8{1}}).SetType(ValueType::U8);
    function->StoreUniform(
            Uniform{offsetof(swift::x86::ThreadContext64, carry_inverted),
                    ValueType::U8},
            hot_polarity);
    function->AdvancePC(Imm{u64{1}});
    if (sse42_target && !observe_before_overwrite) {
        const auto token = function->LoadImm(Imm{u64{0}}).SetType(ValueType::U64);
        const auto final_flags = function->GetFlags(token, Flags::All)
                                         .SetType(ValueType::U64);
        function->StoreUniform(Uniform{result_offset, ValueType::U64}, final_flags);
    }
    function->EndBlock(terminal::ReturnToHost{});

    builder.SetCurBlock(cold);
    const auto token = function->LoadImm(Imm{u64{0}}).SetType(ValueType::U64);
    const auto final_flags = function->GetFlags(token, Flags::All)
                                      .SetType(ValueType::U64);
    function->StoreUniform(Uniform{result_offset, ValueType::U64}, final_flags);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();

    const auto ir_function = function->GetFunction();
    auto* code = TranslateIR(module, function);
    REQUIRE(code != nullptr);
    Runtime runtime{&space};
    std::memcpy(runtime.GetUniformBuffer().data() + selector_offset,
                &initial_selector, sizeof(initial_selector));
    runtime.SetLocation(source_guest);
    const auto halt = runtime.Run();
    const u32 code_size = ir_function->GetJitCache().cache_size.get<u32>();
    u64 code_hash = 1469598103934665603ull;
    for (u32 index = 0; index < code_size; ++index) {
        code_hash ^= static_cast<const u8*>(code)[index];
        code_hash *= 1099511628211ull;
    }
    RegionBranchRun run{
            .halt = halt,
            .code_size = code_size,
            .code_hash = code_hash,
    };
    constexpr u32 kMrsNZCV = 0xD53B'4200u;
    constexpr u32 kMrsMask = 0xFFFF'FFE0u;
    constexpr u32 kBCond = 0x5400'0000u;
    constexpr u32 kBCondMask = 0xFF00'0010u;
    constexpr u32 kCompareBranch = 0x3400'0000u;
    constexpr u32 kCompareBranchMask = 0x7E00'0000u;
    std::vector<u32> instructions(code_size / sizeof(u32));
    std::memcpy(instructions.data(), code,
                instructions.size() * sizeof(u32));
    size_t first_branch = instructions.size();
    size_t first_merge = instructions.size();
    for (size_t index = 0; index < instructions.size(); ++index) {
        const bool branch =
                (instructions[index] & kBCondMask) == kBCond ||
                (instructions[index] & kCompareBranchMask) == kCompareBranch;
        const bool merge = (instructions[index] & kMrsMask) == kMrsNZCV;
        if (branch && first_branch == instructions.size()) {
            first_branch = index;
        }
        if (merge && first_merge == instructions.size()) {
            first_merge = index;
        }
    }
    run.branch_precedes_merge = first_branch < first_merge;
    u8 final_selector{};
    std::memcpy(&final_selector,
                runtime.GetUniformBuffer().data() + selector_offset,
                sizeof(final_selector));
    run.selector = final_selector;
    std::memcpy(&run.observed_flags,
                runtime.GetUniformBuffer().data() + result_offset,
                sizeof(run.observed_flags));
    return run;
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

bool ContainsInsn(const u8* begin, size_t size, u32 expected) {
    for (size_t offset = 0; offset + sizeof(u32) <= size; offset += sizeof(u32)) {
        if (LoadInsn(begin + offset) == expected) {
            return true;
        }
    }
    return false;
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

int RunW81Child(bool legacy_flags_enabled, bool loop_lazy_enabled = false) {
    const auto executable = CurrentTestExecutable();
    REQUIRE_FALSE(executable.empty());
    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        swift::runtime::SetSvmConfigEnvForTest("SVM_DIRECT_LINK_W81_CHILD", "1", 1);
        swift::runtime::SetSvmConfigEnvForTest("SVM_BACKEDGE_LATCH", "1", 1);
        swift::runtime::SetSvmConfigEnvForTest(
                "SVM_BACKEDGE_FLAGS", legacy_flags_enabled ? "1" : "0", 1);
        swift::runtime::SetSvmConfigEnvForTest(
                "SVM_FLAGS_LOOP_LAZY", loop_lazy_enabled ? "1" : "0", 1);
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

TEST_CASE("direct link keeps structural legacy fallbacks",
          "[direct-link][production][fallback]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    const bool cross_module = GENERATE(false, true);
    const bool static_forward = GENERATE(false, true);
    DYNAMIC_SECTION("shape=" << (static_forward ? "SetLocation" : "LinkBlock")
                             << " mode="
                             << (cross_module ? "cross-module target"
                                              : "BlockLink disabled")) {
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
            const VAddr source_guest = page_size + 0x100;
            const VAddr target_guest = 5 * page_size + 0x100;
            Config config{
                    .loc_start = 0,
                    .loc_end = guest_size,
                    .enable_jit = true,
                    .enable_asm_interp = false,
                    .has_local_operation = false,
                    .backend_isa = kArm64,
                    .uniform_buffer_size = 64,
                    .global_opts = cross_module ? Optimizations::BlockLink
                                                : Optimizations::None,
                    .memory_base = guest_memory,
                    .guest_addr_mask = guest_size - 1,
            };
            AddressSpace space{config};
            auto source_module = space.GetDefaultModule();
            auto target_module = source_module;
            if (cross_module) {
                target_module = space.MapModule(
                        target_guest,
                        target_guest + page_size,
                        ModuleConfig{.optimizations = Optimizations::BlockLink});
            }

            auto target = BuildTarget(target_guest, 0x1234'5678'9abc'def0ull);
            auto* target_code = TranslateIR(target_module, target);
            REQUIRE(target_code != nullptr);
            space.PushCodeCache(Location{target_guest}, target_code);

            auto source = static_forward
                    ? BuildStaticForwardSource(source_guest, target_guest)
                    : BuildSource(source_guest, target_guest);
            auto* source_code = static_cast<u8*>(TranslateIR(source_module, source));
            REQUIRE(source_code != nullptr);
            space.PushCodeCache(Location{source_guest}, source_code);
            REQUIRE(space.GetLinkManager().GetStats().sites == 0);

            const auto region = source_module->GetCodeRegion(source_code);
            REQUIRE(region);
            if (cross_module) {
                // A valid source trampoline does not override the module-
                // ownership rule: the exit remains a dispatcher leaf.
                REQUIRE(region->trampoline_offset !=
                        CodeRegion::kInvalidTrampolineOffset);
            } else {
                // Module-level BlockLink is the retained opt-out. It creates
                // neither link metadata nor a region trampoline.
                REQUIRE(region->trampoline_offset ==
                        CodeRegion::kInvalidTrampolineOffset);
            }

            Runtime runtime{&space};
            runtime.SetLocation(source_guest);
            REQUIRE(runtime.Run() == HaltReason::CallHost);
            u64 observed{};
            std::memcpy(&observed, runtime.GetUniformBuffer().data(), sizeof(observed));
            REQUIRE(observed == 0x1234'5678'9abc'def0ull);
        }
        REQUIRE(munmap(guest_memory, guest_size) == 0);
    }
#else
    SUCCEED("production direct-link fallback execution requires an AArch64 host");
#endif
}

TEST_CASE("direct SCC descending edge observes a pending interrupt",
          "[direct-link][production][signal][cycle]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    ScopedEnvironment latch{"SVM_BACKEDGE_LATCH", "1"};
    ScopedEnvironment flags{"SVM_BACKEDGE_FLAGS", "0"};
    const bool static_forward = GENERATE(false, true);
    DYNAMIC_SECTION("descending shape="
                    << (static_forward ? "SetLocation" : "LinkBlock")) {
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
            auto module = space.GetDefaultModule();
            auto block_a = BuildSource(guest_a, guest_b);
            auto block_b = static_forward
                    ? BuildStaticForwardSource(guest_b, guest_a)
                    : BuildSource(guest_b, guest_a);
            auto* code_a = static_cast<u8*>(TranslateIR(module, block_a));
            auto* code_b = static_cast<u8*>(TranslateIR(module, block_b));
            REQUIRE(code_a != nullptr);
            REQUIRE(code_b != nullptr);
            space.PushCodeCache(Location{guest_a}, code_a);
            space.PushCodeCache(Location{guest_b}, code_b);

            constexpr u32 kLdurWzrX28 = 0xb840039fu;
            constexpr u32 kInterruptPollLoad =
                    kLdurWzrX28 |
                    ((static_cast<u32>(state_offset_interrupt_poll) & 0x1ffu) << 12);
            const auto region_a = module->GetCodeRegion(code_a);
            REQUIRE(region_a);
            const auto sites_a = FindProductionSites(space, *region_a, code_a);
            const auto site_a = std::find_if(
                    sites_a.begin(), sites_a.end(), [&](const auto& site) {
                        return site.record.source_owner.allocation == code_a &&
                               site.record.guest_target == guest_b;
                    });
            REQUIRE(site_a != sites_a.end());
            REQUIRE_FALSE(ContainsInsn(
                    code_a,
                    static_cast<size_t>(site_a->rx - code_a),
                    kInterruptPollLoad));
            const auto region_b = module->GetCodeRegion(code_b);
            REQUIRE(region_b);
            const auto sites_b = FindProductionSites(space, *region_b, code_b);
            const auto site_b = std::find_if(
                    sites_b.begin(), sites_b.end(), [&](const auto& site) {
                        return site.record.source_owner.allocation == code_b &&
                               site.record.guest_target == guest_a;
                    });
            REQUIRE(site_b != sites_b.end());
            REQUIRE(ContainsInsn(
                    code_b,
                    static_cast<size_t>(site_b->rx - code_b),
                    kInterruptPollLoad));

            Runtime runtime{&space};
            runtime.SetLocation(guest_a);
            std::atomic_bool runner_done{};
            std::atomic<u32> runner_halt{};
            std::thread runner([&] {
                runner_halt.store(static_cast<u32>(runtime.Run()),
                                  std::memory_order_release);
                runner_done.store(true, std::memory_order_release);
            });

            REQUIRE(WaitUntil(
                    [&] {
                        return space.GetLinkManager().GetStats().linked ==
                               2;
                    },
                    std::chrono::seconds(2)));
            runtime.SignalInterrupt();
            const bool bounded = WaitUntil(
                    [&] { return runner_done.load(std::memory_order_acquire); },
                    std::chrono::seconds(2));
            if (!bounded) {
                space.InvalidateCodeRange(guest_a, guest_b + 1);
                REQUIRE(WaitUntil(
                        [&] { return runner_done.load(std::memory_order_acquire); },
                        std::chrono::seconds(2)));
            }
            runner.join();
            REQUIRE(bounded);
            REQUIRE(runner_halt.load(std::memory_order_acquire) ==
                    static_cast<u32>(HaltReason::Signal));
        }
        REQUIRE(munmap(guest_memory, guest_size) == 0);
    }
#else
    SUCCEED("direct SCC signal coverage requires an AArch64 host");
#endif
}

TEST_CASE("inline indirect L1 loop observes a pending interrupt",
          "[direct-link][production][signal][indirect-l1]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    constexpr VAddr guest = 0x2100;
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .enable_asm_interp = false,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = 16,
            .global_opts = Optimizations::BlockLink,
    };
    AddressSpace space{config};
    ModuleConfig module_config{.read_only = true};
    module_config.feature_overrides.Set(FeatureId::indirect_l1, true);
    auto module = space.MapModule(guest, guest + 0x100, module_config);
    REQUIRE(module != nullptr);
    auto block = BuildIndirectRingBlock(guest);
    auto* code = TranslateIR(module, block);
    REQUIRE(code != nullptr);
    space.PushCodeCache(Location{guest}, code);

    Runtime runtime{&space};
    SetSelector(runtime, guest);
    runtime.SetLocation(guest);
    auto uniform = runtime.GetUniformBuffer();
    auto* entries = reinterpret_cast<volatile u64*>(uniform.data());
    std::atomic_bool runner_done{};
    std::atomic<u32> runner_halt{};
    std::thread runner([&] {
        runner_halt.store(static_cast<u32>(runtime.Run()),
                          std::memory_order_release);
        runner_done.store(true, std::memory_order_release);
    });

    const bool entered = WaitUntil([entries] { return *entries > 32; },
                                   std::chrono::seconds(2));
    runtime.SignalInterrupt();
    const bool bounded = WaitUntil(
            [&] { return runner_done.load(std::memory_order_acquire); },
            std::chrono::seconds(2));
    if (!bounded) {
        space.InvalidateCodeRange(guest, guest + 1);
        REQUIRE(WaitUntil(
                [&] { return runner_done.load(std::memory_order_acquire); },
                std::chrono::seconds(2)));
    }
    runner.join();
    REQUIRE(bounded);
    REQUIRE(entered);
    REQUIRE(runner_halt.load(std::memory_order_acquire) ==
            static_cast<u32>(HaltReason::Signal));
#else
    SUCCEED("inline indirect L1 signal coverage requires an AArch64 host");
#endif
}

TEST_CASE("direct SCC cycle cover is inert with the latch disabled",
          "[direct-link][production][signal][cycle]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    ScopedEnvironment latch{"SVM_BACKEDGE_LATCH", "0"};
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
        const VAddr low = page_size + 0x100;
        const VAddr high = 3 * page_size + 0x100;
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
        auto block = BuildStaticForwardSource(high, low);
        auto* code = static_cast<u8*>(TranslateIR(space.GetDefaultModule(), block));
        REQUIRE(code != nullptr);
        constexpr u32 kExitRequestLdar = 0xc8dfff90u;
        REQUIRE_FALSE(ContainsInsn(code, 32, kExitRequestLdar));
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("direct SCC signal coverage requires an AArch64 host");
#endif
}

TEST_CASE("oversized direct-link allocation rejects before arena creation",
          "[direct-link][production][fallback][region]") {
#if defined(__aarch64__)
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 32,
            .enable_jit = true,
            .enable_asm_interp = false,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .uniform_buffer_size = 64,
            .global_opts = Optimizations::BlockLink,
    };
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    REQUIRE(module->PrepareDirectLinkRegion());
    constexpr u32 kJustOverHalfWindow = (1u << 26) + 4;
    const auto [cache_id, buffer] =
            module->AllocCodeCache(kJustOverHalfWindow, true);
    REQUIRE(cache_id == INVALID_CACHE_ID);
    REQUIRE(buffer.exec_data == nullptr);
#else
    SUCCEED("direct-link region sizing requires an AArch64 backend");
#endif
}

TEST_CASE("production conditional terminal links and delinks both arms independently",
          "[direct-link][production][conditional][smc]") {
#if defined(__aarch64__)
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

TEST_CASE("production direct links bypass a shared full flags merge",
          "[direct-link][production][conditional][flags][smc]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    ScopedEnvironment flags_regs{"SVM_FLAGS_REGS", "1"};

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
                .global_opts = Optimizations::BlockLink |
                               Optimizations::ReturnStackBuffer,
                .region_edges = true,
                .memory_base = guest_memory,
                .guest_addr_mask = guest_size - 1,
        };
        AddressSpace space{config};
        auto module = space.GetDefaultModule();

        auto* then_code = TranslateFlagsKillingTarget(module, then_guest);
        auto* else_code = TranslateFlagsKillingTarget(module, else_guest);
        REQUIRE(then_code != nullptr);
        REQUIRE(else_code != nullptr);
        space.PushCodeCache(Location{then_guest}, then_code);
        space.PushCodeCache(Location{else_guest}, else_code);
        const auto then_target = space.GetLinkManager().QueryTarget(then_guest);
        const auto else_target = space.GetLinkManager().QueryTarget(else_guest);
        REQUIRE(then_target);
        REQUIRE(else_target);
        REQUIRE(then_target->pending_flags_host_pc != nullptr);
        REQUIRE(else_target->pending_flags_host_pc != nullptr);
        REQUIRE(then_target->pending_flags_contract.overwrite_before_observe ==
                kEdgeNZCVMask);
        REQUIRE(else_target->pending_flags_contract.overwrite_before_observe ==
                kEdgeNZCVMask);

        auto* source_code = static_cast<u8*>(TranslatePendingFlagsSource(
                module, source_guest, then_guest, else_guest));
        REQUIRE(source_code != nullptr);
        space.PushCodeCache(Location{source_guest}, source_code);
        const auto region = module->GetCodeRegion(source_code);
        REQUIRE(region);
        const auto sites = FindProductionSites(
                space, *region, source_code, 512);
        REQUIRE(sites.size() == 2);
        const auto then_site = std::find_if(
                sites.begin(), sites.end(), [&](const auto& site) {
                    return site.record.guest_target == then_guest;
                });
        const auto else_site = std::find_if(
                sites.begin(), sites.end(), [&](const auto& site) {
                    return site.record.guest_target == else_guest;
                });
        REQUIRE(then_site != sites.end());
        REQUIRE(else_site != sites.end());
        REQUIRE(then_target->pending_flags_contract.Accepts(
                then_site->record.edge_flags));
        REQUIRE(else_target->pending_flags_contract.Accepts(
                else_site->record.edge_flags));
        REQUIRE(then_site->record.flags_bypass_offset != UINT32_MAX);
        REQUIRE(then_site->record.flags_bypass_offset ==
                else_site->record.flags_bypass_offset);
        REQUIRE(then_site->record.flags_bypass_instruction ==
                else_site->record.flags_bypass_instruction);

        auto* pending_trampoline =
                region->rx_base + region->pending_flags_trampoline_offset;
        REQUIRE(DecodeBranchTarget(then_site->rx, LoadInsn(then_site->rx)) ==
                reinterpret_cast<uintptr_t>(pending_trampoline));
        REQUIRE(DecodeBranchTarget(else_site->rx, LoadInsn(else_site->rx)) ==
                reinterpret_cast<uintptr_t>(pending_trampoline));
        auto* bypass =
                region->rx_base + then_site->record.flags_bypass_offset;
        REQUIRE(DecodeBranchTarget(bypass, LoadInsn(bypass)) ==
                reinterpret_cast<uintptr_t>(bypass + 2 * sizeof(u32)));
        auto* flags_merge_trampoline =
                region->rx_base + region->pending_flags_trampoline_offset +
                arm64::kFlagsMergeOffsetFromPending;
        REQUIRE(DecodeBranchTarget(bypass + sizeof(u32),
                                   LoadInsn(bypass + sizeof(u32))) ==
                reinterpret_cast<uintptr_t>(flags_merge_trampoline));

        Runtime runtime{&space};
        SetSelector(runtime, 1);
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(space.GetLinkManager().QuerySite(then_site->key)->state ==
                LinkSiteState::Linked);
        REQUIRE(space.GetLinkManager().QuerySite(else_site->key)->state ==
                LinkSiteState::Unlinked);
        REQUIRE(DecodeBranchTarget(then_site->rx, LoadInsn(then_site->rx)) ==
                reinterpret_cast<uintptr_t>(
                        then_target->pending_flags_host_pc));
        REQUIRE(DecodeBranchTarget(bypass, LoadInsn(bypass)) ==
                reinterpret_cast<uintptr_t>(bypass + 2 * sizeof(u32)));

        SetSelector(runtime, 0);
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(space.GetLinkManager().QuerySite(else_site->key)->state ==
                LinkSiteState::Linked);
        REQUIRE(DecodeBranchTarget(else_site->rx, LoadInsn(else_site->rx)) ==
                reinterpret_cast<uintptr_t>(
                        else_target->pending_flags_host_pc));
        REQUIRE(DecodeBranchTarget(bypass, LoadInsn(bypass)) ==
                reinterpret_cast<uintptr_t>(bypass + 2 * sizeof(u32)));

        space.InvalidateCodeRange(then_guest, then_guest + 1);
        REQUIRE(DecodeBranchTarget(bypass, LoadInsn(bypass)) ==
                reinterpret_cast<uintptr_t>(bypass + 2 * sizeof(u32)));
        REQUIRE(DecodeBranchTarget(then_site->rx, LoadInsn(then_site->rx)) ==
                reinterpret_cast<uintptr_t>(pending_trampoline));

        auto then_block = BuildTarget(then_guest, 0x3333);
        then_code = TranslateIR(module, then_block);
        REQUIRE(then_code != nullptr);
        space.PushCodeCache(Location{then_guest}, then_code);
        const auto incompatible_target =
                space.GetLinkManager().QueryTarget(then_guest);
        REQUIRE(incompatible_target);
        REQUIRE(incompatible_target->pending_flags_host_pc == nullptr);
        SetSelector(runtime, 1);
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(space.GetLinkManager().QuerySite(then_site->key)->state ==
                LinkSiteState::Linked);
        REQUIRE(LoadInsn(bypass) == then_site->record.flags_bypass_instruction);
        REQUIRE(DecodeBranchTarget(bypass + sizeof(u32),
                                   LoadInsn(bypass + sizeof(u32))) ==
                reinterpret_cast<uintptr_t>(flags_merge_trampoline));
        REQUIRE(DecodeBranchTarget(then_site->rx, LoadInsn(then_site->rx)) ==
                reinterpret_cast<uintptr_t>(then_code));
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("production flags bypass execution requires an AArch64 host");
#endif
}

TEST_CASE("static forwards register their flags bypass",
          "[direct-link][production][flags]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    ScopedEnvironment flags_regs{"SVM_FLAGS_REGS", "1"};

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
        const u32 flags_case = GENERATE(0u, 1u, 2u, 3u);
        CAPTURE(flags_case);
        ScopedEnvironment flags_cfinv{
                "SVM_FLAGS_CFINV", flags_case == 3 ? "0" : "1"};
        const VAddr source_guest = page_size + 0x100;
        const VAddr target_guest = 5 * page_size + 0x100;
        Config config{
                .loc_start = 0,
                .loc_end = guest_size,
                .enable_jit = true,
                .enable_asm_interp = false,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .uniform_buffer_size = 64,
                .global_opts = Optimizations::BlockLink |
                               Optimizations::ReturnStackBuffer,
                .arm64_features = flags_case == 3
                        ? Arm64Features::None
                        : Arm64Features::FlagM,
                .region_edges = true,
                .memory_base = guest_memory,
                .guest_addr_mask = guest_size - 1,
        };
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        auto target_flags = Flags::NZCV;
        auto source_flags = Flags::All;
        u32 expected_mask = kEdgeNZCVMask;
        if (flags_case == 1) {
            target_flags = Flags::NZ;
            source_flags = Flags::NZ;
            expected_mask = 0xC000'0000u;
        } else if (flags_case >= 2) {
            target_flags = Flags::Negate | Flags::Carry;
            source_flags = target_flags;
            expected_mask = 0xA000'0000u;
        }
        const auto expected_polarity = (expected_mask & kEdgeCarryMask) == 0
                ? EdgeCarryPolarity::Unknown
                : (flags_case == 3 ? EdgeCarryPolarity::Inverted
                                   : EdgeCarryPolarity::Direct);

        auto* target_code = TranslateFlagsKillingTarget(
                module, target_guest, target_flags);
        REQUIRE(target_code != nullptr);
        space.PushCodeCache(Location{target_guest}, target_code);
        const auto target = space.GetLinkManager().QueryTarget(target_guest);
        REQUIRE(target);
        REQUIRE(target->pending_flags_host_pc != nullptr);
        REQUIRE((target->call_pending_flags_host_pc != nullptr) ==
                (flags_case == 0));
        REQUIRE(target->pending_flags_contract.overwrite_before_observe ==
                expected_mask);

        auto* source_code = static_cast<u8*>(TranslatePendingFlagsStaticSource(
                module, source_guest, target_guest, source_flags,
                expected_polarity));
        REQUIRE(source_code != nullptr);
        space.PushCodeCache(Location{source_guest}, source_code);
        const auto region = module->GetCodeRegion(source_code);
        REQUIRE(region);
        const auto sites = FindProductionSites(space, *region, source_code, 512);
        REQUIRE(sites.size() == 1);
        const auto& site = sites.front();
        REQUIRE(site.record.guest_target == target_guest);
        REQUIRE(site.record.flags_bypass_offset != UINT32_MAX);
        REQUIRE(site.record.edge_flags.valid_nzcv_mask == expected_mask);
        REQUIRE(site.record.edge_flags.carry_polarity == expected_polarity);
        REQUIRE(target->pending_flags_contract.Accepts(site.record.edge_flags));
        auto* bypass = region->rx_base + site.record.flags_bypass_offset;
        if (flags_case == 0) {
            REQUIRE((site.record.flags_bypass_instruction & 0xFC00'0000u) ==
                    0x9400'0000u);
            REQUIRE((LoadInsn(bypass) & ~0x3E0u) == 0xB340'1C1Au);
        }

        Runtime runtime{&space};
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(space.GetLinkManager().QuerySite(site.key)->state ==
                LinkSiteState::Linked);
        REQUIRE(DecodeBranchTarget(site.rx, LoadInsn(site.rx)) ==
                reinterpret_cast<uintptr_t>(target->pending_flags_host_pc));
        if (flags_case == 1) {
            REQUIRE(DecodeBranchTarget(bypass, LoadInsn(bypass)) ==
                    reinterpret_cast<uintptr_t>(bypass + 3 * sizeof(u32)));
        } else if (flags_case >= 2) {
            const auto bypass_target = DecodeBranchTarget(
                    bypass, LoadInsn(bypass));
            REQUIRE(bypass_target);
            REQUIRE(*bypass_target >= reinterpret_cast<uintptr_t>(
                    bypass + 4 * sizeof(u32)));
            REQUIRE(*bypass_target <= reinterpret_cast<uintptr_t>(site.rx));
        }

        space.InvalidateCodeRange(target_guest, target_guest + 1);
        auto target_block = BuildTarget(target_guest, 0x1234);
        target_code = TranslateIR(module, target_block);
        REQUIRE(target_code != nullptr);
        space.PushCodeCache(Location{target_guest}, target_code);
        REQUIRE(space.GetLinkManager()
                        .QueryTarget(target_guest)
                        ->pending_flags_host_pc == nullptr);
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(LoadInsn(bypass) == site.record.flags_bypass_instruction);
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("production static flags bypass requires an AArch64 host");
#endif
}

TEST_CASE("canonical external roots stay inside one code object",
          "[function-entry][direct-link][production]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    ScopedEnvironment flags_regs{"SVM_FLAGS_REGS", "1"};

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
        const VAddr target_guest = 2 * page_size + 0x100;
        constexpr u64 kFingerprint = 0x63a4'51b2'97d8'e0full;
        Config config{
                .loc_start = 0,
                .loc_end = guest_size,
                .enable_jit = true,
                .enable_asm_interp = false,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .uniform_buffer_size = 64,
                .global_opts = Optimizations::BlockLink,
                .region_edges = true,
                .memory_base = guest_memory,
                .guest_addr_mask = guest_size - 1,
        };
        AddressSpace space{config};
        auto module = space.GetDefaultModule();

        HIRBuilder builder{2, true};
        auto* function = builder.AppendFunction(
                Location{source_guest}, Location{target_guest + 1});
        builder.AdvancePC(Imm{u64{1}});
        builder.ExternalLinkBlock(
                terminal::ExternalLinkBlock{Location{target_guest}});
        auto* target = function->CreateOrGetBlock(Location{target_guest});
        function->RegisterExternalEntryRoot(target);
        builder.SetCurBlock(target);
        const auto value = function
                                   ->LoadImm(Imm{kFingerprint})
                                   .SetType(ValueType::U64);
        function->StoreUniform(Uniform{0, ValueType::U64}, value);
        builder.AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();

        auto* source_code = static_cast<u8*>(TranslateIR(module, function));
        REQUIRE(source_code != nullptr);
        const auto source = space.GetLinkManager().QueryTarget(source_guest);
        const auto published_target =
                space.GetLinkManager().QueryTarget(target_guest);
        REQUIRE(source);
        REQUIRE(published_target);
        REQUIRE(source->target_owner == published_target->target_owner);
        REQUIRE(source->region_id == published_target->region_id);
        const auto region = module->GetCodeRegion(source_code);
        REQUIRE(region);
        REQUIRE(FindProductionSites(space, *region, source_code).empty());

        Runtime runtime{&space};
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        u64 observed{};
        const auto uniform = runtime.GetUniformBuffer();
        REQUIRE(uniform.size() >= sizeof(observed));
        std::memcpy(&observed, uniform.data(), sizeof(observed));
        REQUIRE(observed == kFingerprint);
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("canonical external root execution requires an AArch64 host");
#endif
}

TEST_CASE("terminal-only call returns enter through canonical links",
          "[function-entry][continuation][production]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    ScopedEnvironment flags_regs{"SVM_FLAGS_REGS", "1"};

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
        const VAddr return_guest = page_size + 0x180;
        const VAddr relay_guest = page_size + 0x1c0;
        const VAddr target_guest = 2 * page_size + 0x100;
        constexpr u64 kFingerprint = 0x4c51'9a27'd806'3be5ull;
        Config config{
                .loc_start = 0,
                .loc_end = guest_size,
                .enable_jit = true,
                .enable_asm_interp = false,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .uniform_buffer_size = 64,
                .global_opts = Optimizations::BlockLink |
                               Optimizations::ReturnStackBuffer,
                .region_edges = true,
                .memory_base = guest_memory,
                .guest_addr_mask = guest_size - 1,
        };
        AddressSpace space{config};
        auto module = space.GetDefaultModule();

        HIRBuilder builder{4, true};
        auto* function = builder.AppendFunction(
                Location{source_guest}, Location{target_guest + 1});
        const auto source_value = function
                                          ->LoadImm(Imm{u64{1}})
                                          .SetType(ValueType::U64);
        function->StoreUniform(Uniform{8, ValueType::U64}, source_value);
        builder.RegisterCallReturn(Location{return_guest});
        auto* connector = function->CreateOrGetBlock(Location{return_guest});
        builder.AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::ReturnToHost{});

        builder.SetCurBlock(connector);
        auto* relay = builder.LinkBlock(
                terminal::LinkBlock{Location{relay_guest}});
        builder.SetCurBlock(relay);
        auto* target = builder.LinkBlock(
                terminal::LinkBlock{Location{target_guest}});
        builder.SetCurBlock(target);
        const auto value = function
                                   ->LoadImm(Imm{kFingerprint})
                                   .SetType(ValueType::U64);
        function->StoreUniform(Uniform{0, ValueType::U64}, value);
        builder.AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();

        REQUIRE(TranslateIR(module, function) != nullptr);
        REQUIRE(space.GetCodeCache(Location{return_guest}) != nullptr);
        REQUIRE(space.GetCodeCache(Location{relay_guest}) != nullptr);
        REQUIRE_FALSE(space.GetLinkManager().QueryTarget(return_guest));
        REQUIRE_FALSE(space.GetLinkManager().QueryTarget(relay_guest));

        Runtime runtime{&space};
        runtime.SetLocation(return_guest);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        u64 observed{};
        const auto uniform = runtime.GetUniformBuffer();
        REQUIRE(uniform.size() >= sizeof(observed));
        std::memcpy(&observed, uniform.data(), sizeof(observed));
        REQUIRE(observed == kFingerprint);

        space.InvalidateCodeRange(return_guest, return_guest + 1);
        REQUIRE(space.GetCodeCache(Location{return_guest}) == nullptr);
        REQUIRE(space.GetCodeCache(Location{relay_guest}) == nullptr);
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("terminal-only call-return execution requires an AArch64 host");
#endif
}

TEST_CASE("fault-backed continuation rejects empty and mismatched frames",
          "[direct-link][continuation][fault]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    ScopedEnvironment flags_regs{"SVM_FLAGS_REGS", "1"};

    const bool mismatch = GENERATE(false, true);
    CAPTURE(mismatch);
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
        const VAddr source_guest = page_size + 0x180;
        const VAddr target_guest = mismatch ? 5 * page_size + 0x180 : 0;
        constexpr u64 kFingerprint = 0x1234'5678'9abc'def0ull;
        Config config{
                .loc_start = 0,
                .loc_end = guest_size,
                .enable_jit = true,
                .enable_asm_interp = false,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .uniform_buffer_size = 64,
                .global_opts = Optimizations::BlockLink |
                               Optimizations::ReturnStackBuffer,
                .region_edges = true,
                .memory_base = guest_memory,
                .guest_addr_mask = guest_size - 1,
        };
        AddressSpace space{config};
        auto module = space.GetDefaultModule();

        auto target_block = BuildTarget(target_guest, kFingerprint);
        auto* target_code = TranslateIR(module, target_block);
        REQUIRE(target_code != nullptr);
        space.PushCodeCache(Location{target_guest}, target_code);
        auto* source_code = TranslateContinuationReturnSource(module, source_guest);
        REQUIRE(source_code != nullptr);
        space.PushCodeCache(Location{source_guest}, source_code);

        Runtime runtime{&space};
        auto* empty = runtime.GetState()->rsb_pointer;
        REQUIRE(empty != nullptr);
        if (mismatch) {
            auto* frame = empty - 1;
            frame->guest_location = target_guest + 1;
            frame->dispatch_index = reinterpret_cast<u64>(target_code);
            runtime.GetState()->rsb_pointer = frame;
        }
        std::memcpy(runtime.GetUniformBuffer().data(),
                    &target_guest,
                    sizeof(target_guest));
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() ==
                (mismatch ? HaltReason::CallHost : HaltReason::CodeMiss));
        if (mismatch) {
            u64 result{};
            std::memcpy(&result, runtime.GetUniformBuffer().data(), sizeof(result));
            REQUIRE(result == kFingerprint);
        }
        REQUIRE(runtime.GetState()->rsb_pointer == empty);
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("fault-backed continuation recovery requires an AArch64 host");
#endif
}

TEST_CASE("call misses preserve host continuation across dispatch",
          "[direct-link][continuation][call-miss]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    ScopedEnvironment flags_regs{"SVM_FLAGS_REGS", "1"};

    const bool static_call = GENERATE(false, true);
    const bool outer_frame = GENERATE(false, true);
    CAPTURE(static_call, outer_frame);
    const size_t page_size = static_cast<size_t>(getpagesize());
    const size_t guest_size = 8 * page_size;
    void* guest_memory =
            mmap(nullptr, guest_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_memory != MAP_FAILED);
    {
        const VAddr source_guest = page_size + 0x140;
        const VAddr return_guest = source_guest + 1;
        const VAddr target_guest = 5 * page_size + 0x140;
        constexpr u64 kFingerprint = 0x3141'5926'5358'9793ull;
        Config config{
                .loc_start = 0,
                .loc_end = guest_size,
                .enable_jit = true,
                .enable_asm_interp = false,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .uniform_buffer_size = 64,
                .global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer,
                .region_edges = true,
                .memory_base = guest_memory,
                .guest_addr_mask = guest_size - 1,
        };
        AddressSpace space{config};
        auto module = space.GetDefaultModule();

        auto* source_code =
                static_call ? TranslateStaticCallSource(
                                      module, source_guest, target_guest, return_guest)
                            : TranslateIndirectCallSource(module, source_guest, return_guest);
        REQUIRE(source_code != nullptr);
        if (!static_call) {
            REQUIRE(space.GetCodeCache(return_guest) != nullptr);
            const auto return_entry = space.GetLinkManager().QueryTarget(return_guest);
            REQUIRE(return_entry);
            REQUIRE(return_entry->generation != 0);
        }

        Runtime runtime{&space};
        auto* empty = runtime.GetState()->rsb_pointer;
        REQUIRE(empty != nullptr);
        auto* expected = empty;
        if (outer_frame) {
            expected = empty - 1;
            expected->guest_location = return_guest + 0x100;
            expected->dispatch_index = 1;
            runtime.GetState()->rsb_pointer = expected;
        }
        std::memcpy(runtime.GetUniformBuffer().data() + 8, &target_guest, sizeof(target_guest));
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &return_guest, sizeof(return_guest));
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CodeMiss);
        REQUIRE(runtime.GetLocation() == target_guest);
        REQUIRE(runtime.GetState()->rsb_pointer == expected - 1);
        REQUIRE((expected - 1)->guest_location == return_guest);
        if (static_call) {
            REQUIRE((expected - 1)->dispatch_index != 0);
        } else {
            REQUIRE((expected - 1)->dispatch_index == 0);
        }

        auto* target_code = TranslateReturningCallTarget(module, target_guest, kFingerprint);
        REQUIRE(target_code != nullptr);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(runtime.GetState()->rsb_pointer == expected);
        u64 result{};
        std::memcpy(&result, runtime.GetUniformBuffer().data(), sizeof(result));
        REQUIRE(result == kFingerprint);

        space.InvalidateCodeRange(source_guest, source_guest + 1);
        REQUIRE(space.GetCodeCache(source_guest) == nullptr);
        REQUIRE(space.GetCodeCache(return_guest) == nullptr);
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("call-miss continuation requires an AArch64 host");
#endif
}

TEST_CASE("fault-backed indirect call preserves an invalidated target frame",
          "[direct-link][continuation][indirect-l1][fault]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    ScopedEnvironment flags_regs{"SVM_FLAGS_REGS", "1"};

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
        const VAddr source_guest = page_size + 0x180;
        const VAddr return_guest = source_guest + 1;
        const VAddr return_driver_guest = 2 * page_size + 0x180;
        const VAddr target_guest = 5 * page_size + 0x180;
        constexpr u64 kFingerprint = 0x1234'5678'9abc'def0ull;
        constexpr u64 kReturnFingerprint = 0x0ddc'0ffe'e15e'beefull;
        Config config{
                .loc_start = 0,
                .loc_end = guest_size,
                .enable_jit = true,
                .enable_asm_interp = false,
                .has_local_operation = false,
                .backend_isa = kArm64,
                .uniform_buffer_size = 64,
                .global_opts = Optimizations::BlockLink |
                               Optimizations::ReturnStackBuffer,
                .region_edges = true,
                .memory_base = guest_memory,
                .guest_addr_mask = guest_size - 1,
        };
        AddressSpace space{config};
        auto module = space.GetDefaultModule();

        REQUIRE(TranslateCallTarget(module, target_guest, kFingerprint) != nullptr);
        REQUIRE(TranslateIndirectCallSource(
                        module, source_guest, return_guest) != nullptr);
        REQUIRE(space.GetCallCodeCacheTable().Lookup(target_guest) != 0);
        REQUIRE(space.GetCallCodeCacheTable().Zero(target_guest));
        REQUIRE(space.GetCallCodeCacheTable().Lookup(target_guest) == 0);

        Runtime runtime{&space};
        auto* empty = runtime.GetState()->rsb_pointer;
        REQUIRE(empty != nullptr);
        std::memcpy(runtime.GetUniformBuffer().data() + 8,
                    &target_guest,
                    sizeof(target_guest));
        runtime.SetLocation(source_guest);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        REQUIRE(runtime.GetState()->rsb_pointer == empty - 1);
        REQUIRE((empty - 1)->guest_location == return_guest);
        REQUIRE((empty - 1)->dispatch_index == 0);
        u64 result{};
        std::memcpy(&result, runtime.GetUniformBuffer().data(), sizeof(result));
        REQUIRE(result == kFingerprint);

        space.InvalidateCodeRange(source_guest, source_guest + 1);
        REQUIRE(space.GetCodeCache(return_guest) == nullptr);
        REQUIRE(TranslateContinuationReturnSource(module, return_driver_guest) != nullptr);
        std::memcpy(runtime.GetUniformBuffer().data(), &return_guest, sizeof(return_guest));
        runtime.SetLocation(return_driver_guest);
        REQUIRE(runtime.Run() == HaltReason::CodeMiss);
        REQUIRE(runtime.GetLocation() == return_guest);
        REQUIRE(runtime.GetState()->rsb_pointer == empty);

        auto replacement = BuildTarget(return_guest, kReturnFingerprint);
        auto* replacement_code = TranslateIR(module, replacement);
        REQUIRE(replacement_code != nullptr);
        space.PushCodeCache(Location{return_guest}, replacement_code);
        REQUIRE(runtime.Run() == HaltReason::CallHost);
        std::memcpy(&result, runtime.GetUniformBuffer().data(), sizeof(result));
        REQUIRE(result == kReturnFingerprint);
    }
    REQUIRE(munmap(guest_memory, guest_size) == 0);
#else
    SUCCEED("fault-backed indirect-call recovery requires an AArch64 host");
#endif
}

TEST_CASE("production direct exit repeatedly delinks recompiles and relinks",
          "[direct-link][production][smc]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    const bool static_forward = GENERATE(false, true);
    CAPTURE(static_forward);

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

        auto source = static_forward
                ? BuildStaticForwardSource(source_guest, target_guest)
                : BuildSource(source_guest, target_guest);
        auto* source_code = TranslateIR(module, source);
        REQUIRE(source_code != nullptr);
        space.PushCodeCache(Location{source_guest}, source_code);

        const auto source_region = module->GetCodeRegion(static_cast<u8*>(source_code));
        REQUIRE(source_region);
        const auto trampoline = source_region->rx_base + source_region->trampoline_offset;
        const auto sites = FindProductionSites(
                space, *source_region, static_cast<u8*>(source_code));
        REQUIRE(sites.size() == 1);
        auto* site = sites.front().rx;
        REQUIRE(DecodeBranchTarget(site, LoadInsn(site)) ==
                reinterpret_cast<uintptr_t>(trampoline));
        const LinkSiteKey key = sites.front().key;
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
        std::array<RSBFrame, 2> held_rsb{};
        auto* held_empty = &held_rsb[1];
        auto* held_pointer = &held_rsb[0];
        auto held = space.GetSmcTracker().RegisterRuntime(
                held_l1, nullptr, nullptr, &held_pointer, held_empty);
        space.GetSmcTracker().EnableMultithreading();
        space.GetSmcTracker().BeginJit(held);
        space.InvalidateCodeRange(source_guest, source_guest + 1);
        const auto retiring = space.GetLinkManager().QuerySite(key);
        REQUIRE(retiring);
        REQUIRE(retiring->state == LinkSiteState::Retiring);
        space.GetSmcTracker().EndJit(held);
        REQUIRE_FALSE(space.GetLinkManager().QuerySite(key));
        space.GetSmcTracker().BeginJit(held);
        REQUIRE(held_pointer == held_empty);
        space.GetSmcTracker().EndJit(held);
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
    if (!GetSvmConfig().direct_link_w81_child) {
        REQUIRE(RunW81Child(false) == 0);
        REQUIRE(RunW81Child(true) == 0);
        REQUIRE(RunW81Child(false, true) == 0);
        return;
    }
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    ScopedEnvironment latch{"SVM_BACKEDGE_LATCH", "1"};
    const bool legacy_flags_enabled = GetSvmConfig().backedge_flags;
    const bool loop_lazy_enabled = GetSvmConfig().flags_loop_lazy;
    const bool flags_enabled = legacy_flags_enabled || loop_lazy_enabled;
    ScopedEnvironment flags{"SVM_BACKEDGE_FLAGS", legacy_flags_enabled ? "1" : "0"};
    ScopedEnvironment loop_lazy{"SVM_FLAGS_LOOP_LAZY", loop_lazy_enabled ? "1" : "0"};
    DYNAMIC_SECTION("backedge flags=" << legacy_flags_enabled
                                       << " loop lazy=" << loop_lazy_enabled) {
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

TEST_CASE("region branch flags materialize only on the observing exit",
          "[region][flags][production]") {
#if defined(__aarch64__)
    ScopedEnvironment disk_cache{"SVM_JIT_CACHE", ""};
    const auto off = RunRegionBranchFunction(false);
    const auto on = RunRegionBranchFunction(true);
    REQUIRE(off.halt == HaltReason::CallHost);
    REQUIRE(on.halt == HaltReason::CallHost);
    REQUIRE(off.selector == 1);
    REQUIRE(on.selector == 1);
    REQUIRE(on.observed_flags == off.observed_flags);
    REQUIRE(on.code_size > off.code_size);
    REQUIRE(on.code_hash != off.code_hash);

    // A flags observer at the hot target precedes the next full overwrite.
    // The region proof must reject that edge; execution remains identical.
    const auto observed_off = RunRegionBranchFunction(false, true, false, 2);
    const auto observed_on = RunRegionBranchFunction(true, true, false, 2);
    REQUIRE(observed_off.halt == HaltReason::CallHost);
    REQUIRE(observed_on.halt == HaltReason::CallHost);
    REQUIRE(observed_on.selector == observed_off.selector);
    REQUIRE(observed_on.observed_flags == observed_off.observed_flags);
    REQUIRE(observed_on.code_size == observed_off.code_size);
    REQUIRE(observed_on.code_hash == observed_off.code_hash);

    // The non-observing internal target overwrites all six flags before its
    // AdvancePC and therefore may take the deferred edge directly.
    const auto hot_off = RunRegionBranchFunction(false, false, false, 2);
    const auto hot_on = RunRegionBranchFunction(true, false, false, 2);
    REQUIRE(hot_off.halt == HaltReason::CallHost);
    REQUIRE(hot_on.halt == HaltReason::CallHost);
    REQUIRE(hot_on.selector == hot_off.selector);
    REQUIRE(hot_on.observed_flags == hot_off.observed_flags);
    REQUIRE(hot_on.branch_precedes_merge);

    const auto tail_off = RunRegionBranchFunction(false, false, false, 1, true);
    const auto tail_on = RunRegionBranchFunction(true, false, false, 1, true);
    REQUIRE(tail_off.halt == HaltReason::CallHost);
    REQUIRE(tail_on.halt == HaltReason::CallHost);
    REQUIRE(tail_on.selector == tail_off.selector);
    REQUIRE(tail_on.observed_flags == tail_off.observed_flags);
    REQUIRE(tail_on.branch_precedes_merge);
    REQUIRE(tail_on.code_size == hot_on.code_size);

    const auto partial_off = RunRegionBranchFunction(
            false, false, false, 2, true, Flags::NZ);
    const auto partial_on = RunRegionBranchFunction(
            true, false, false, 2, true, Flags::NZ);
    REQUIRE(partial_off.halt == HaltReason::CallHost);
    REQUIRE(partial_on.halt == HaltReason::CallHost);
    REQUIRE(partial_on.selector == partial_off.selector);
    REQUIRE(partial_on.observed_flags == partial_off.observed_flags);
    REQUIRE(partial_on.code_size <= partial_off.code_size);

    // Host calls after the final producer are committed-state boundaries.
    // They reject the recipe just like a faulting memory operation.
    const auto helper_off = RunRegionBranchFunction(false, false, true);
    const auto helper_on = RunRegionBranchFunction(true, false, true);
    REQUIRE(helper_off.halt == HaltReason::CallHost);
    REQUIRE(helper_on.halt == HaltReason::CallHost);
    REQUIRE(helper_on.selector == helper_off.selector);
    REQUIRE(helper_on.observed_flags == helper_off.observed_flags);
    REQUIRE(helper_on.code_size == helper_off.code_size);
    REQUIRE(helper_on.code_hash == helper_off.code_hash);

    const auto sse42_off = RunRegionBranchFunction(
            false, false, false, 2, true, Flags::All, true);
    const auto sse42_on = RunRegionBranchFunction(
            true, false, false, 2, true, Flags::All, true);
    REQUIRE(sse42_off.halt == HaltReason::CallHost);
    REQUIRE(sse42_on.halt == HaltReason::CallHost);
    REQUIRE(sse42_on.selector == sse42_off.selector);
    REQUIRE(sse42_on.observed_flags == sse42_off.observed_flags);
    REQUIRE(sse42_on.branch_precedes_merge);

    const auto sse42_observed_off = RunRegionBranchFunction(
            false, true, false, 2, true, Flags::All, true);
    const auto sse42_observed_on = RunRegionBranchFunction(
            true, true, false, 2, true, Flags::All, true);
    REQUIRE(sse42_observed_off.halt == HaltReason::CallHost);
    REQUIRE(sse42_observed_on.halt == HaltReason::CallHost);
    REQUIRE(sse42_observed_on.selector == sse42_observed_off.selector);
    REQUIRE(sse42_observed_on.observed_flags == sse42_observed_off.observed_flags);
    REQUIRE(sse42_observed_on.code_size == sse42_observed_off.code_size);
    REQUIRE(sse42_observed_on.code_hash == sse42_observed_off.code_hash);
#else
    SUCCEED("region branch-flags execution check requires an AArch64 host");
#endif
}
