#include "inputs.h"

#include <algorithm>
#include <bit>
#include <optional>
#include <type_traits>

#include "constraints.h"
#include "fixed_validation.h"
#include "fpr_inputs.h"
#include "gpr_inputs.h"
#include "register_contract.h"
#include "runtime/compile/allocate/writer.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/compile/workspace.h"
#include "runtime/ir/hir_builder.h"
#include "spill_inputs.h"

namespace swift::runtime::backend::arm64 {
namespace {
using namespace ir;

bool DirectlyFeedsImmediateShift(const Inst* inst, const Inst* next) {
    return next &&
           (next->GetOp() == OpCode::LslImm || next->GetOp() == OpCode::LsrImm ||
            next->GetOp() == OpCode::AsrImm) &&
           next->GetArg<Value>(0).Def() == inst;
}

bool DirectlyFeedsLogicalFlagDirect(const Inst* inst,
                                    const Inst* next,
                                    std::span<const compile::FlagsDemand> demands) {
    if (!next) return false;
    if (next->GetOp() != OpCode::Or || next->GetUses() != 0 ||
        next->GetArg<Value>(0).Def() != inst ||
        (next->Id() >= demands.size() || !demands[next->Id()].has_save)) {
        return false;
    }
    auto operand = next->GetArg<Operand>(1);
    return operand.IsImm() && operand.GetLeft().imm.Get() == 0;
}

Value NarrowLoadTieSource(const Inst* inst,
                          const Inst* next,
                          const FeatureSet& features,
                          std::span<const compile::FlagsDemand> demands) {
    if (features.shift_imm_fast) {
        if (inst->GetOp() == OpCode::BitExtract && inst->GetArg<Imm>(1).Get() == 0 &&
            inst->GetArg<Imm>(2).Get() == GetValueSizeByte(inst->ReturnType()) * 8 &&
            (DirectlyFeedsImmediateShift(inst, next) ||
             DirectlyFeedsLogicalFlagDirect(inst, next, demands))) {
            return ResolveBitCastSource(inst->GetArg<Value>(0));
        } else if (inst->GetOp() == OpCode::ZeroExtend32) {
            auto source = ResolveBitCastSource(inst->GetArg<Value>(0));
            if (source.Defined() && source.Def()->GetOp() == OpCode::LoadUniform &&
                GetValueSizeByte(source.Type()) <= 2 && DirectlyFeedsImmediateShift(inst, next)) {
                return source;
            }
            if (source.Defined() && source.Def()->GetOp() == OpCode::BitExtract &&
                source.Def()->GetArg<Imm>(1).Get() == 0 &&
                source.Def()->GetArg<Imm>(2).Get() == GetValueSizeByte(source.Type()) * 8 &&
                DirectlyFeedsImmediateShift(inst, next)) {
                auto input = source.Def()->GetArg<Value>(0);
                if (input.Def() && input.Def()->GetOp() == OpCode::GetHostGPR &&
                    input.Def()->GetArg<Imm>(0).Get() <= 9) {
                    return source;
                }
            }
        } else if (inst->GetOp() == OpCode::ZeroExtend32To64) {
            auto source = ResolveBitCastSource(inst->GetArg<Value>(0));
            // Only a 32-bit (W-write) shift makes the extension a no-op:
            // a 64-bit shift leaves live upper bits (e.g. the sign
            // extension of imul/cdq's high half) that the ZExt must clear.
            if (source.Defined() &&
                (source.Def()->GetOp() == OpCode::LslImm ||
                 source.Def()->GetOp() == OpCode::LsrImm ||
                 source.Def()->GetOp() == OpCode::AsrImm) &&
                GetValueSizeByte(source.Def()->ReturnType()) == 4) {
                return source;
            }
        }
    }
    if (!features.mem_narrow_fuse) {
        return {};
    }
    switch (inst->GetOp()) {
        case OpCode::SignExtend:
        case OpCode::ZeroExtend32: {
            auto source = ResolveBitCastSource(inst->GetArg<Value>(0));
            if (source.Defined() && source.Def()->GetOp() == OpCode::LoadMemory &&
                GetValueSizeByte(source.Type()) <= 2) {
                return source;
            }
            break;
        }
        case OpCode::ZeroExtend32To64: {
            auto source = ResolveBitCastSource(inst->GetArg<Value>(0));
            if (!source.Defined() || (source.Def()->GetOp() != OpCode::SignExtend &&
                                      source.Def()->GetOp() != OpCode::ZeroExtend32)) {
                break;
            }
            auto load = ResolveBitCastSource(source.Def()->GetArg<Value>(0));
            if (load.Defined() && load.Def()->GetOp() == OpCode::LoadMemory &&
                GetValueSizeByte(load.Type()) <= 2) {
                return source;
            }
            break;
        }
        default:
            break;
    }
    return {};
}

Value MemoryOperandTieSource(const Inst* inst, bool feeds_memory, const FeatureSet& features) {
    if ((!features.mem_narrow_fuse && !features.addr_ea_tie) ||
        inst->GetOp() != OpCode::GetOperand || inst->GetUses() != 1 || !feeds_memory) {
        return {};
    }
    const auto operand = inst->GetArg<Operand>(0);
    if (!operand.GetRight().Null() || !operand.GetLeft().IsValue()) {
        return {};
    }
    return operand.GetLeft().value;
}

Value IntWidthTieSource(const Inst* inst) {
    Value source{};
    if (inst->GetOp() == OpCode::BitExtract &&
        GetValueSizeByte(inst->ReturnType()) == sizeof(u32) && inst->GetArg<Imm>(1).Get() == 0 &&
        inst->GetArg<Imm>(2).Get() == 32) {
        source = inst->GetArg<Value>(0);
    } else if (inst->GetOp() == OpCode::ZeroExtend32To64 &&
               GetValueSizeByte(inst->GetArg<Value>(0).Type()) == sizeof(u32)) {
        source = inst->GetArg<Value>(0);
    } else if (inst->GetOp() == OpCode::ZeroExtend32) {
        // The byte/word frontend canonicalizes narrow results through
        // ZeroExtend32 before widening; without this link the chain
        // load -> ZeroExtend32 -> ZeroExtend32To64 materializes every
        // width view in its own register.
        source = inst->GetArg<Value>(0);
    }
    return HasKnownWWrite(source) ? source : Value{};
}

compile::AllocationInputs::PoolPolicy Policy(GPRSMask pool, const FeatureSet& features) {
    const bool compact = X86PinExtLevel3Enabled(pool);
    return {pool,
            {DefaultGPRValueReserve(pool, features), kDefaultScratchFPR, compact, compact},
            FixedGPRClassEnabled(pool, features),
            {},
            !GetSvmConfig().no_fuse_pin_writes && X86PinExtLevel2Enabled(pool),
            features.ra_coalesce && X86PinExtLevel2Enabled(pool)};
}

bool IsMemory(OpCode op) {
    return op == OpCode::LoadMemory || op == OpCode::StoreMemory || op == OpCode::LoadMemoryTSO ||
           op == OpCode::StoreMemoryTSO;
}

void DescribeBlock(const Block* block,
                   compile::AllocationInputs& input,
                   compile::SemanticFacts& facts,
                   std::span<const compile::FlagsDemand> demands,
                   std::unique_ptr<compile::FPRInputs>& fpr,
                   std::unique_ptr<compile::GPRInputs>& gpr,
                   std::unique_ptr<compile::WidthInputs>& width,
                   std::unique_ptr<compile::GPRWriteInputs>& writes,
                   std::unique_ptr<compile::SpillInputs>& spill,
                   FixedValidationBuilder& fixed_validation,
                   const FeatureSet& physical_features,
                   compile::CompileWorkspace& workspace) {
    if (block->GetInstList().empty()) return;
    // First raw-consumer classification joins the existing input visit. Only
    // one seen bit per local definition is needed, not reverse edges.
    auto& graph = facts.PrepareIndex(block);
    DescribeFPRInputs(block, facts, input.instruction_count,
                       input.fprs, input.features, graph, fpr);
    const auto nodes = graph.Instructions();
    std::optional<compile::InstructionRowScope> first_consumers;
    if (graph.HasMemoryInstructions()) first_consumers.emplace(workspace, nodes.size());
    fixed_validation.BeginBlock(block, graph);
    SpillInputBuilder spill_inputs{block, graph, spill};
    const bool scratch0 = X86PinExtScratchOnlyEnabled(input.pools[0].pool, input.features);
    const bool scratch1 = X86PinExtScratchOnlyEnabled(input.pools[1].pool, input.features);
    const bool fixed_publications =
            input.pools[0].fixed_publications || input.pools[1].fixed_publications;
    const bool gpr_coalescing = input.pools[0].gpr_coalescing || input.pools[1].gpr_coalescing;
    const bool width_enabled = input.features.ra_width_chain || input.features.ra_width_chain_long;
    std::optional<GPRInputBuilder> gpr_inputs;
    if ((fixed_publications || gpr_coalescing || width_enabled) &&
        graph.Summary().HasGPRInputCandidates())
        gpr_inputs.emplace(
                input,
                physical_features,
                graph,
                gpr_coalescing || width_enabled ? &facts.PrepareBlockUses(block) : nullptr,
                block,
                gpr,
                width,
                writes);
    for (size_t at = 0; at < nodes.size(); ++at) {
        const auto* inst = nodes[at];
        bool has_integer_operands = false;
        if (first_consumers || !inst->IsPseudoOperation())
            inst->ForEachValue([&](Value value) {
                if (!inst->IsPseudoOperation()) {
                    const auto source = ResolveBitCastSource(value);
                    has_integer_operands |= source.Defined() && !IsFloatValueType(source.Type());
                }
                if (!first_consumers) return;
                const auto* producer = value.Def();
                if (!producer || producer->Id() >= inst->Id()) return;
                const auto position = graph.InstructionPosition(producer);
                if (!position) return;
                auto& seen = (*first_consumers)[*position];
                if (seen != UINT32_MAX) return;
                seen = 0;
                if (!IsMemory(inst->GetOp())) return;
                input.memory_feeders.push_back(producer->Id());
                if (!producer->HasValue() || IsFloatValueType(producer->ReturnType())) return;
                const auto source = ResolveBitCastSource(
                        MemoryOperandTieSource(producer, true, input.features));
                if (source.Defined())
                    input.ties.push_back(
                            {producer->Id(), source.Id(), input.features.addr_ea_tie, false});
            });
        spill_inputs.Describe(*inst, at, has_integer_operands);
        fixed_validation.Describe(*inst);
        const auto* next = at + 1 < nodes.size() ? nodes[at + 1] : nullptr;
        DescribeScratchInput(
                input,
                *inst,
                inst->Id() < demands.size() ? demands[inst->Id()] : compile::FlagsDemand{});
        auto mask0 = FixedGPRClobbers(*inst, input.features, scratch0);
        auto mask1 =
                scratch0 == scratch1 ? mask0 : FixedGPRClobbers(*inst, input.features, scratch1);
        if (ScratchXPoolEnabled(input.features) && at + 1 == nodes.size()) {
            mask0 |= kTerminalFixedGPRClobbers;
            mask1 |= kTerminalFixedGPRClobbers;
        }
        if (mask0) input.clobbers.push_back({inst->Id(), mask0});
        if (mask0 != mask1) input.declared_clobber_deltas.push_back({inst->Id(), mask0 ^ mask1});
        const auto op = inst->GetOp();
        if (gpr_inputs) gpr_inputs->Describe(*inst, at);
        const bool full_gpr =
                op == OpCode::GetHostGPR && inst->IsGetHostRegOperation() &&
                GetValueSizeByte(inst->ReturnType()) == sizeof(u64);
        const bool full_fpr = op == OpCode::GetHostFPR && inst->GetArg<Imm>(1).Get() == 0 &&
                              IsFloatValueType(inst->ReturnType());
        if (full_gpr || full_fpr) {
            const auto home = inst->GetArg<Imm>(0).Get();
            ASSERT(home < 32);
            input.fixed_reads.push_back(
                    {inst->Id(),
                     static_cast<u16>(home),
                     full_fpr ? compile::RegisterClass::FPR : compile::RegisterClass::GPR,
                     full_fpr && input.features.sse_scalar_tie});
        }
        if (op == OpCode::SetHostGPR) {
            const auto source = ResolveBitCastSource(inst->GetArg<Value>(0));
            const auto home = inst->GetArg<Imm>(1).Get();
            ASSERT(home < 31);
            input.publications.push_back(
                    {inst->Id(),
                     static_cast<u16>(source.Defined() ? source.Id() : UINT16_MAX),
                     static_cast<u16>(home)});
        }
        if (input.function && op == OpCode::CallReturn) {
            const auto source = ResolveBitCastSource(inst->GetArg<Value>(0));
            if (source.Defined() && source.Id() < input.instruction_count &&
                !IsFloatValueType(source.Def()->ReturnType()))
                input.preferences.push_back({source.Id(), 14});
        }
        if (!inst->HasValue() || IsFloatValueType(inst->ReturnType())) continue;
        const auto add_tie = [&](Value source, bool allow_fixed, bool integer_width) {
            source = ResolveBitCastSource(source);
            if (source.Defined())
                input.ties.push_back({inst->Id(), source.Id(), allow_fixed, integer_width});
        };
        add_tie(NarrowLoadTieSource(inst, next, input.features, demands), false, false);
        if (input.features.ra_intwidth_tie) add_tie(IntWidthTieSource(inst), false, true);
    }
    if (gpr_inputs) gpr_inputs->Finish();
    spill_inputs.Finish();
}

template <class Unit>
compile::AllocationInputs Describe(Unit* unit,
                                   const compile::AllocationWriter& allocation,
                                   const FeatureSet& features,
                                   compile::SemanticFacts& facts,
                                   compile::CompileWorkspace& workspace) {
    if constexpr (std::is_same_v<std::remove_const_t<Unit>, Block>)
        ASSERT(facts.MatchesInput(unit, nullptr));
    else
        ASSERT(facts.MatchesInput(nullptr, unit));
    auto input = compile::AllocationInputs{unit,
                                           features,
                                           allocation.GetGprs(),
                                           allocation.GetFprs(),
                                           Policy(allocation.GetGprs(), features).budget};
    // Mandatory numbered facts are complete before any target input asks for
    // reverse uses, so flags, presence and liveness share the base visit.
    (void)facts.PrepareValueLiveness();
    input.declared_gprs = facts.GPRHomes();
    ASSERT(!(input.declared_gprs & (u32{1} << 31)));
    input.pools[0] = Policy(allocation.GetGprs(), features);
    auto declared = allocation.GetGprs();
    for (u32 bits = input.declared_gprs; bits; bits &= bits - 1)
        declared.Mark(std::countr_zero(bits));
    input.pools[1] = Policy(declared, features);
    for (auto& policy : input.pools) policy.width_component_sites = input.function ? 10 : 1;
    input.fixed_gpr_homes = kX86FixedGPRHomes;
    if (!allocation.FixedGPRConstraintsReady())
        input.fixed = BuildFixedGPRInputs(unit, declared, features, facts, workspace);
    const auto& demands = facts.PrepareFlags();
    std::unique_ptr<compile::FPRInputs> fpr;
    std::unique_ptr<compile::GPRInputs> gpr;
    std::unique_ptr<compile::WidthInputs> width;
    std::unique_ptr<compile::GPRWriteInputs> writes;
    std::unique_ptr<compile::SpillInputs> spill;
    FixedValidationBuilder fixed_validation{features};
    if constexpr (std::is_same_v<std::remove_const_t<Unit>, Block>)
        DescribeBlock(unit,
                      input,
                      facts,
                      demands,
                      fpr,
                      gpr,
                      width,
                      writes,
                      spill,
                      fixed_validation,
                      allocation.GetFeatures(),
                      workspace);
    else
        for (const auto* member : unit->GetHIRBlocks())
            DescribeBlock(member->GetBlock(),
                          input,
                          facts,
                          demands,
                          fpr,
                          gpr,
                          width,
                          writes,
                          spill,
                          fixed_validation,
                          allocation.GetFeatures(),
                          workspace);
    input.fixed_validation = fixed_validation.Finish();
    std::stable_sort(input.ties.begin(), input.ties.end(), [](const auto& a, const auto& b) {
        return a.value < b.value;
    });
    std::sort(input.preferences.begin(), input.preferences.end(), [](const auto& a, const auto& b) {
        return a.value < b.value;
    });
    std::sort(input.publications.begin(),
              input.publications.end(),
              [](const auto& a, const auto& b) { return a.instruction < b.instruction; });
    std::sort(input.memory_feeders.begin(), input.memory_feeders.end());
    if (fpr) {
        std::sort(fpr->ties.begin(), fpr->ties.end(), [](const auto& a, const auto& b) {
            return a.value < b.value;
        });
        std::sort(fpr->writes.begin(), fpr->writes.end(), [](const auto& a, const auto& b) {
            return a.begin < b.begin;
        });
        input.fpr = std::move(fpr);
    }
    std::sort(input.fixed_reads.begin(), input.fixed_reads.end(), [](const auto& a, const auto& b) {
        return a.value < b.value;
    });
    if (gpr && (!gpr->publications.empty() || !gpr->reads.empty() || !gpr->views.empty() ||
                !gpr->w_publications.empty() || width || writes)) {
        gpr->width = std::move(width);
        gpr->writes = std::move(writes);
        std::sort(gpr->consumer_rules.begin(),
                  gpr->consumer_rules.end(),
                  [](const auto& a, const auto& b) { return a.instruction < b.instruction; });
        std::sort(gpr->blocks.begin(), gpr->blocks.end(), [](const auto& a, const auto& b) {
            return a.block->GetInstList().front().Id() < b.block->GetInstList().front().Id();
        });
        input.gpr = std::move(gpr);
    }
    if (spill) {
        std::sort(spill->blocks.begin(), spill->blocks.end(), [](const auto& a, const auto& b) {
            return a.block->GetInstList().front().Id() < b.block->GetInstList().front().Id();
        });
        input.spill = std::move(spill);
    }
    std::sort(input.scratch.begin(), input.scratch.end(), [](const auto& a, const auto& b) {
        return a.instruction < b.instruction;
    });
    input.scratch.erase(
            std::unique(input.scratch.begin(),
                        input.scratch.end(),
                        [](const auto& a, const auto& b) { return a.profile == b.profile; }),
            input.scratch.end());
    return input;
}
}  // namespace

compile::AllocationInputs DescribeAllocationInputs(const ir::Block* block,
                                                   const compile::AllocationWriter& allocation,
                                                   const FeatureSet& features,
                                                   compile::SemanticFacts& facts,
                                                   compile::CompileWorkspace& workspace) {
    return Describe(block, allocation, features, facts, workspace);
}
compile::AllocationInputs DescribeAllocationInputs(const ir::HIRFunction* function,
                                                   const compile::AllocationWriter& allocation,
                                                   const FeatureSet& features,
                                                   compile::SemanticFacts& facts,
                                                   compile::CompileWorkspace& workspace) {
    return Describe(function, allocation, features, facts, workspace);
}
compile::AllocationInputs PrepareAllocationInputs(const ir::Block* block,
                                                  compile::AllocationWriter& allocation,
                                                  const FeatureSet& features) {
    allocation.SetAllocationUnit(block, nullptr);
    return Describe(block, allocation, features, allocation.PrepareFacts(), allocation.Workspace());
}
compile::AllocationInputs PrepareAllocationInputs(const ir::HIRFunction* function,
                                                  compile::AllocationWriter& allocation,
                                                  const FeatureSet& features) {
    allocation.SetAllocationUnit(nullptr, function);
    return Describe(
            function, allocation, features, allocation.PrepareFacts(), allocation.Workspace());
}
}  // namespace swift::runtime::backend::arm64
