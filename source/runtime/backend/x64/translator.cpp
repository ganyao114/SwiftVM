#include "translator.h"
#include "flags_usage.h"
#include "runtime/backend/riscv64/memory_protocol.h"
#include <algorithm>
#include <stdexcept>
#include "helpers.h"
#include "runtime/backend/interrupt_request.h"
#include "x87_helper.h"
#include "runtime/common/variant_util.h"
#include "runtime/common/crc32.h"
#include "runtime/common/saturating_multiply.h"
#include "runtime/common/saturating_shift.h"
#include "runtime/common/unsigned_estimate.h"
#include "runtime/common/saturating_add_sub.h"

namespace swift::runtime::backend::x64 {
using O = ir::OpCode;
using namespace Xbyak;

namespace {
HaltReason ObserveSingleEntry(State* state) noexcept {
    return ObserveHaltRequest(*state);
}

void ValidateSingleTerminal(const ir::Terminal& terminal) {
    VisitVariant<void>(terminal, [](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            throw std::runtime_error("x64 single instruction requires a defined terminal successor; Switch unsupported");
        } else if constexpr (std::is_same_v<T, ir::terminal::If> ||
                             std::is_same_v<T, ir::terminal::Condition>) {
            ValidateSingleTerminal(term.then_);
            ValidateSingleTerminal(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            ValidateSingleTerminal(term.else_);
        }
    });
}

bool ReadsConditionFlags(ir::Cond condition) {
    return condition != ir::Cond::AL && condition != ir::Cond::NV;
}

bool TerminalUsesGuestFlags(const ir::Terminal& terminal) {
    return VisitVariant<bool>(terminal, [](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            return ReadsConditionFlags(term.cond) || TerminalUsesGuestFlags(term.then_) ||
                   TerminalUsesGuestFlags(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
            return TerminalUsesGuestFlags(term.then_) || TerminalUsesGuestFlags(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            return TerminalUsesGuestFlags(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            for (const auto& item : term.cases) if (TerminalUsesGuestFlags(item.then)) return true;
        }
        return false;
    });
}

bool ProducerPublishesGuestFlags(const ir::Inst& inst) {
    const auto* producer = inst.GetArg<ir::Value>(0).Def();
    if (!producer) return false; // Constructor validation reports malformed IR.
    return ClassifyProducerFlags(producer->GetOp(), producer->ReturnType(), inst.GetArg<ir::Flags>(1)).published_mask != 0;
}

bool BodyUsesGuestFlags(ir::Block* body, const backend::FunctionPlan* plan) {
    for (auto& inst : body->GetInstList()) {
        // Match Translate's dispatch precedence: an exit or halt-check
        // replaces its marker opcode, so only the actual terminal can read
        // flags there. Calling a helper alone needs no resident word.
        if (plan) {
            if (auto exit = plan->exits.find(&inst); exit != plan->exits.end()) {
                if (TerminalUsesGuestFlags(exit->second)) return true;
                continue;
            }
            if (plan->halt_checks.contains(&inst)) continue;
        }
        switch (inst.GetOp()) {
            case O::Adc: case O::Sbb: case O::GetFlags:
            case O::PublishFCmpFlags: case O::PublishSse42StrFlags:
            case O::SetCarry: case O::SetOverflow: case O::InvertCarry:
                return true;
            case O::SaveFlags: case O::BranchOnlyFlags:
                if (ProducerPublishesGuestFlags(inst)) return true;
                break;
            case O::ClearFlags: case O::TestFlags: case O::TestNotFlags:
                if (True(inst.GetArg<ir::Flags>(0) & ir::Flags::All)) return true;
                break;
            case O::CondSet: case O::LocalCondSet: case O::CondSelect:
                if (ReadsConditionFlags(inst.GetArg<ir::Cond>(0))) return true;
                break;
            default: break;
        }
    }
    return TerminalUsesGuestFlags(body->GetTerminal());
}
} // namespace

CfgRegisterOptions Translator::AllocationBudget(ir::Block* block, const backend::FunctionPlan* plan,
                                                bool flags_residency, bool loop_weighted_costs, bool scalar_unary_phases) {
    CfgRegisterOptions budget;
    budget.loop_weighted_costs = loop_weighted_costs;
    budget.scalar_unary_phases = scalar_unary_phases;
    if (flags_residency && BodyUsesGuestFlags(block, plan)) budget.physical_registers = {5, 15};
    return budget;
}

HostFeatures HostFeatures::Detect() {
    __builtin_cpu_init();
    // GCC/Clang's FMA predicate includes OS support for saving AVX state.
    return {.sse41 = bool(__builtin_cpu_supports("sse4.1")), .fma = bool(__builtin_cpu_supports("fma")),
            .aes = bool(__builtin_cpu_supports("aes")), .pclmul = bool(__builtin_cpu_supports("pclmul")),
            .sse42 = bool(__builtin_cpu_supports("sse4.2")), .sha = bool(__builtin_cpu_supports("sha"))};
}

std::unique_ptr<Translator> Translator::PrepareFunction(backend::FunctionPlan& plan,
                       HostFeatures requested, const Config* config, bool scalar_cache,
                       bool cfg_allocation, bool flags_residency, FunctionPreparationOptions options) {
    auto* body = plan.body.get();
    if (!body) throw std::runtime_error("x64 preparation requires a function body");
    if ((options.uniform_forwarding || options.state_promotion || options.uniform_dead_store_cross_pc) && !config)
        throw std::runtime_error("x64 preparation requires explicit uniform extent");
    if (options.state_promotion && (!cfg_allocation || !options.uniform_forwarding))
        throw std::runtime_error("x64 state promotion requires CFG uniform forwarding");
    auto graph = CfgRegisterPlan::ValidateGraph(body, plan);
    if (!graph.Enabled()) throw std::runtime_error("x64 preparation requires valid CFG/SSA metadata");
    CfgStatePromoteStats promoted;
    CfgUniformForwardStats forwarded;
    std::vector<CfgUniformForwardChange> changes;
    std::optional<CfgRegisterPlan> allocation;
    if (options.state_promotion) {
        std::vector<u32> lanes;
        for (const auto& range : config->loop_gpr_uniform_ranges) {
            if (range.size != sizeof(u64)) { lanes.clear(); break; }
            lanes.push_back(range.offset);
        }
        promoted = PromoteCfgState(plan, config->uniform_buffer_size, lanes, graph,
                                   {.reject_dirty_loop_phis = true});
        if (!promoted.accepted || promoted.loads_replaced) {
            graph = CfgRegisterPlan::ValidateGraph(body, plan);
            if (!graph.Enabled()) throw std::runtime_error("x64 state promotion produced invalid SSA");
        }
    }
    CfgUniformDeadStorePcStats dead_stores_pc;
    if (options.uniform_dead_store_cross_pc) {
        dead_stores_pc = EliminateOverwrittenUniformStoresAcrossPC(plan, *config, ExecutionMode::Ordinary, {.enabled = true});
        if (dead_stores_pc.stores_removed) {
            graph = CfgRegisterPlan::ValidateGraph(body, plan);
            if (!graph.Enabled()) throw std::runtime_error("x64 cross-PC dead-store elimination produced invalid SSA");
        }
    }
    const bool requested_flags_residency=flags_residency;
    CfgHomeReloadPreparation home_reload{plan,options.scalar_home_reload_split,
        cfg_allocation&&scalar_cache&&!options.uniform_forwarding,
        {.allocation_budget=AllocationBudget(body,&plan,flags_residency,options.loop_weighted_costs,options.scalar_unary_phases),
         .scalar_rematerialization=options.scalar_rematerialization}};
    home_reload.Try();
    // This pass owns no cached authority. Its accepted standalone trial is
    // provisional until final flags selection and a fresh whole CFG/frame.
    if(home_reload.HasTrial()) {
        graph=CfgRegisterPlan::ValidateGraph(body,plan);
        if(!graph.Enabled())throw std::runtime_error("x64 home reload trial invalidated CFG");
        allocation.reset();
    }
    u32 home_reload_final_builds{},discarded_flags_builds{},discarded_flags_recolors{};
    bool home_reload_verified{};
    FlagsResidencyDecision flags_decision;
    if (options.profitable_flags_residency && cfg_allocation && flags_residency) {
        flags_decision = SelectFlagsForPreparation(body, plan,
            AllocationBudget(body, &plan, false, options.loop_weighted_costs, options.scalar_unary_phases),
            allocation, !options.uniform_forwarding, options.flags_selection_recolor);
        flags_residency = flags_decision.selected;
    }
    if(home_reload.HasTrial()) {
        ++home_reload_final_builds;
        auto final=CfgRegisterPlan::Build(body,plan,
            AllocationBudget(body,&plan,flags_residency,options.loop_weighted_costs,options.scalar_unary_phases));
        std::unordered_set<ir::Inst*> metadata;
        if(final.Enabled())for(auto* inst:final.Instructions())
            if(inst->GetOp()==O::SaveFlags||inst->GetOp()==O::BranchOnlyFlags) {
                auto* producer=inst->GetArg<ir::Value>(0).Def();
                if(ClassifyProducerFlags(producer->GetOp(),producer->ReturnType(),inst->GetArg<ir::Flags>(1)).needs_metadata)
                    metadata.insert(producer);
            }
        const auto frame=SparseFramePlan::Build(body,&plan,final,metadata,options.scalar_rematerialization);
        if(frame.Validate(body,&plan,final,metadata,options.scalar_rematerialization)&&home_reload.VerifyFinal(final,frame)) {
            allocation=std::move(final);home_reload_verified=true;
        } else {
            // Do not recolor a mutated trial at a different flags budget and
            // call that the original proof. Restore IR/uses/IDs first.
            discarded_flags_builds=flags_decision.allocation_builds;
            discarded_flags_recolors=flags_decision.allocation_recolors;
            allocation.reset();home_reload.Rollback();flags_residency=requested_flags_residency;
            graph=CfgRegisterPlan::ValidateGraph(body,plan);
            if(!graph.Enabled())throw std::runtime_error("x64 home reload rollback invalidated CFG");
            if(options.profitable_flags_residency&&cfg_allocation&&flags_residency) {
                flags_decision=SelectFlagsForPreparation(body,plan,
                    AllocationBudget(body,&plan,false,options.loop_weighted_costs,options.scalar_unary_phases),
                    allocation,true,options.flags_selection_recolor);
                flags_residency=flags_decision.selected;
            }
        }
    }
    if (options.uniform_forwarding) {
        if (options.capture_forward_changes) {
            u32 position{};
            for (auto& inst : body->GetInstList()) {
                if (inst.GetOp() == O::LoadUniform && inst.ArgAt(0).GetType() == ir::ArgType::Uniform)
                    changes.push_back({&inst, position, inst.GetArg<ir::Uniform>(0).GetOffset(), inst.ReturnType()});
                ++position;
            }
        }
        forwarded = ForwardForPreparation(plan, config->uniform_buffer_size, graph,
            {.require_scalar_source_register = cfg_allocation,
             .allocation_budget = AllocationBudget(body, &plan, flags_residency, options.loop_weighted_costs,
                                                   options.scalar_unary_phases)}, allocation);
        std::vector<CfgUniformForwardChange> kept_changes;
        for (auto change : changes) {
            if (change.load->GetOp() == O::BitExtract) {
                change.source = change.load->GetArg<ir::Value>(0);
                change.byte = change.load->GetArg<ir::Imm>(1).Get() / 8;
                change.bytes = change.load->GetArg<ir::Imm>(2).Get() / 8;
            } else if (change.load->GetOp() == O::BitCast) {
                change.source = change.load->GetArg<ir::Value>(0);
                change.bytes = ir::GetValueSizeByte(change.type);
            } else continue;
            kept_changes.push_back(change);
        }
        changes = std::move(kept_changes);
        if (options.capture_forward_changes && changes.size() != forwarded.loads_forwarded)
            throw std::runtime_error("x64 preparation forwarding capture count mismatch");
    }
    // With forwarding disabled the private selector retained its chosen plan
    // from this same immutable promoted body. With forwarding enabled every
    // policy plan was discarded before any rewrite, including zero candidates.
    // Every mutation has finished. Only internal read-only bookkeeping lies
    // between the final allocation and adoption; callers cannot cache it or
    // insert a callback here. The constructor scans this final IR normally.
    auto result = std::unique_ptr<Translator>(new Translator(PreparedTag{}, body, requested, config, &plan,
                    scalar_cache, ExecutionMode::Ordinary, cfg_allocation, flags_residency,
                    std::move(allocation), forwarded.allocation_rounds+home_reload.Statistics().trial.full_allocation_builds+home_reload_final_builds, options.scalar_rematerialization,
                    options.scalar_binary_immediates, options.loop_weighted_costs, options.scalar_unary_phases,
                    options.scalar_unary_coalescing, options.scalar_shift_immediates,
                    options.scalar_unary_mask_elision, options.native_cf_chains, options.vector_cfg_allocation,
                    std::move(options.vector_register_options)));
    // Keep the outer guard armed through constructor adoption/validation.
    if(home_reload_verified)home_reload.Commit();
    result->home_reload_split_stats=home_reload.Statistics();
    result->home_reload_split_stats.final_allocation_builds=home_reload_final_builds;
    result->home_reload_split_stats.discarded_flags_allocation_builds=discarded_flags_builds;
    result->home_reload_split_stats.discarded_flags_allocation_recolors=discarded_flags_recolors;
    result->promotion_stats = promoted;
    result->dead_store_pc_stats = dead_stores_pc;
    if (options.profitable_flags_residency && cfg_allocation) {
        result->flags_decision = flags_decision;
        result->preparation_stats.flags_policy_allocation_builds = flags_decision.allocation_builds+discarded_flags_builds;
        result->preparation_stats.flags_policy_allocation_recolors = flags_decision.allocation_recolors+discarded_flags_recolors;
    }
    result->forward_stats = forwarded;
    result->forward_changes = std::move(changes);
    return result;
}

Translator::Translator(ir::Block* block, HostFeatures requested, const Config* config,
                       const backend::FunctionPlan* plan, bool scalar_cache,
                       ExecutionMode mode, bool cfg_allocation, bool flags_residency, bool scalar_rematerialization,
                       bool scalar_binary_immediates, bool loop_weighted_costs, bool scalar_unary_phases,
                       bool scalar_unary_coalescing, bool scalar_shift_immediates,
                       bool scalar_unary_mask_elision, bool native_cf_chains, bool vector_cfg_allocation,
                       VectorRegisterOptions vector_register_options)
    : Translator(PreparedTag{}, block, requested, config, plan, scalar_cache, mode,
                 cfg_allocation, flags_residency, std::nullopt, 0, scalar_rematerialization, scalar_binary_immediates,
                 loop_weighted_costs, scalar_unary_phases, scalar_unary_coalescing, scalar_shift_immediates,
                 scalar_unary_mask_elision, native_cf_chains, vector_cfg_allocation,
                 std::move(vector_register_options)) {}

Translator::Translator(PreparedTag, ir::Block* block, HostFeatures requested, const Config* config,
                       const backend::FunctionPlan* plan, bool scalar_cache,
                       ExecutionMode mode, bool cfg_allocation, bool flags_residency,
                       std::optional<CfgRegisterPlan> prepared_allocation, u32 preparation_builds,
                       bool scalar_rematerialization, bool scalar_binary_immediates, bool loop_weighted_costs,
                       bool scalar_unary_phases, bool scalar_unary_coalescing, bool scalar_shift_immediates,
                       bool scalar_unary_mask_elision, bool native_cf_chains, bool vector_cfg_allocation,
                       VectorRegisterOptions vector_register_options)
    : CodeGenerator(4096, AutoGrow), block(block), function_plan(plan), execution_mode(mode),
      guest_access_coverage(mode == ExecutionMode::SingleInstruction
                                   ? GuestAccessCoverage::Precise : GuestAccessCoverage::Unsupported),
      guest_pc(block->GetStartLocation().Value()),
      flags_resident(flags_residency && mode == ExecutionMode::Ordinary && BodyUsesGuestFlags(block, plan)),
      scalar_binary_immediates(scalar_binary_immediates && mode == ExecutionMode::Ordinary),
      loop_weighted_costs(loop_weighted_costs && cfg_allocation && mode == ExecutionMode::Ordinary),
      scalar_unary_phases(scalar_unary_phases && cfg_allocation && mode == ExecutionMode::Ordinary),
      scalar_unary_coalescing(scalar_unary_coalescing && scalar_unary_phases && cfg_allocation &&
                              mode == ExecutionMode::Ordinary),
      scalar_shift_immediates(scalar_shift_immediates && mode == ExecutionMode::Ordinary),
      scalar_unary_mask_elision(scalar_unary_mask_elision && scalar_unary_coalescing &&
                                scalar_unary_phases && cfg_allocation && mode == ExecutionMode::Ordinary),
      native_cf_chains(native_cf_chains && mode == ExecutionMode::Ordinary),
      vector_cfg_allocation(vector_cfg_allocation && mode == ExecutionMode::Ordinary) {
    preparation_stats.full_allocation_builds = preparation_builds;
    flags_decision.selected = flags_resident;
    flags_decision.reason = flags_resident ? FlagsResidencyReason::Forced : FlagsResidencyReason::Disabled;
    if (execution_mode == ExecutionMode::SingleInstruction) {
        if (function_plan)
            throw std::runtime_error("x64 single instruction cannot use a FunctionPlan");
        for (const auto& inst : block->GetInstList())
            if (inst.GetOp() == O::AdvancePC)
                throw std::runtime_error("x64 single instruction cannot contain AdvancePC");
        ValidateSingleTerminal(block->GetTerminal());
    }
    ConfigureUniformBindings(config);
    const auto detected = HostFeatures::Detect();
    // A caller may disable features for portable code/tests, never enable a
    // feature the current CPU/OS cannot execute. Generated code is host-local.
    features = {.sse41 = requested.sse41 && detected.sse41, .fma = requested.fma && detected.fma,
                .aes = requested.aes && detected.aes, .pclmul = requested.pclmul && detected.pclmul,
                .sse42 = requested.sse42 && detected.sse42, .sha = requested.sha && detected.sha};
    std::vector<ir::Inst*> instructions;
    std::unordered_map<ir::Inst*, u32> last_use;
    bool local_control_flow = plan != nullptr;
    for (auto& inst : block->GetInstList()) {
        last_use[&inst] = instructions.size(); instructions.push_back(&inst);
        local_control_flow |= inst.GetOp() == O::Goto || inst.GetOp() == O::NotGoto || inst.GetOp() == O::BindLabel || inst.GetOp() == O::AddPhi;
        if (inst.GetOp() == O::SaveFlags || inst.GetOp() == O::BranchOnlyFlags) {
            auto* producer = inst.GetArg<ir::Value>(0).Def();
            if (!producer) throw std::runtime_error("x64 flags require a defined producer");
            if (ClassifyProducerFlags(producer->GetOp(), producer->ReturnType(), inst.GetArg<ir::Flags>(1)).needs_metadata)
                flag_producers.insert(producer);
        }
    }
    const auto use = [&](ir::Value value, u32 position) {
        if (auto found = last_use.find(value.Def()); found != last_use.end())
            found->second = std::max(found->second, position);
    };
    for (u32 position = 0; position < instructions.size(); ++position) {
        for (u32 index = 0; index < ir::Inst::max_args; ++index) {
            auto& arg = instructions[position]->ArgAt(index);
            if (arg.IsValue()) use(arg.Get<ir::Value>(), position);
            else if (arg.IsLambda()) {
                const auto lambda = arg.Get<ir::Lambda>();
                if (lambda.IsValue()) use(lambda.GetValue(), position);
            } else if (arg.IsParams()) {
                for (const auto& param : arg.Get<ir::Params>())
                    if (param.data.IsValue()) use(param.data.value, position);
            }
        }
    }
    const auto terminal_uses = [&](const auto& self, const ir::Terminal& terminal) -> void {
        VisitVariant<void>(terminal, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If> || std::is_same_v<T, ir::terminal::Condition>) {
                if constexpr (std::is_same_v<T, ir::terminal::If>) use(term.cond, instructions.size());
                self(self, term.then_); self(self, term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) self(self, term.else_);
            else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                use(term.value, instructions.size());
                for (const auto& item : term.cases) self(self, item.then);
            }
        });
    };
    terminal_uses(terminal_uses, block->GetTerminal());
    const auto reads_vector_registers = [](ir::Inst* inst) {
        const auto op = inst->GetOp();
        // The remaining 64-bit multiply lowering reads canonical homes.
        // Only packed multiply widths may consume or publish XMM2 values.
        if (op == O::VecMul && inst->GetArg<ir::Imm>(2).Get() == 64) return false;
        switch (op) {
            case O::Vec4And: case O::VecAnd: case O::Vec4Or: case O::VecOr: case O::VecXor: case O::VecAndNot:
            case O::Vec4Add: case O::VecAdd: case O::Vec4Sub: case O::VecSub:
            case O::VecFAdd: case O::VecFSub: case O::VecFMul: case O::VecFDiv:
            case O::VecSatShiftImm:
            case O::VecUnsignedEstimate:
            case O::VecSatAdd: case O::VecSatSub:
            case O::VecCmpEq: case O::VecCmpGt:
            case O::VecMul: case O::Vec4Mul:
                return true;
            default: return false;
        }
    };
    if (!local_control_flow) for (u32 position = 0; position + 1 < instructions.size(); ++position) {
        const auto producer = instructions[position]->GetOp();
        if ((reads_vector_registers(instructions[position]) || producer == O::LoadUniform) &&
            instructions[position]->ReturnType() == ir::ValueType::V128 && last_use[instructions[position]] == position + 1 &&
            reads_vector_registers(instructions[position + 1]))
            vector_forwarding.insert(instructions[position]);
    }
    // Local jumps can revisit definitions or skip a writer. Legacy paths
    // keep distinct homes; the CFG path may omit audited homes and share
    // scalar-only homes whose complete CFG lifetimes do not interfere.
    std::vector<std::vector<ir::Inst*>> early_definitions;
    std::unordered_map<ir::Inst*, u32> definition_positions;
    std::unordered_set<ir::Inst*> early_pseudos;
    for (auto* inst : instructions) {
        O producer_op;
        switch (inst->GetOp()) {
            case O::CpuidUpper: producer_op = O::Cpuid; break;
            case O::Div128Remainder: producer_op = O::Div128; break;
            case O::VecSatMulQC: producer_op = O::VecSatMulHigh; break;
            case O::VecSatShiftQC: producer_op = O::VecSatShiftImm; break;
            default: continue;
        }
        if (!inst->ArgAt(0).IsValue() || !inst->GetArg<ir::Value>(0).Defined() ||
            inst->GetArg<ir::Value>(0).Def()->GetOp() != producer_op)
            throw std::runtime_error("x64 pseudo requires its matching producer");
        auto* producer = inst->GetArg<ir::Value>(0).Def();
        if (inst->GetOp() == O::VecSatMulQC) ValidateSaturatingMultiplyQC(inst);
        else if (inst->GetOp() == O::VecSatShiftQC) ValidateSaturatingShiftQC(inst);
        else if (inst->ReturnType() != ir::ValueType::U64 || inst->GetArg<ir::Value>(0).Type() != ir::ValueType::U64 ||
                 producer->ReturnType() != ir::ValueType::U64)
            throw std::runtime_error("x64 pair-result pseudo requires U64 producer/input/result types");
        if (early_definitions.empty()) {
            early_definitions.resize(instructions.size());
            for (u32 index = 0; index < instructions.size(); ++index) definition_positions.emplace(instructions[index], index);
        }
        const auto found = definition_positions.find(producer);
        if (found == definition_positions.end()) throw std::runtime_error("x64 JIT requires block-local SSA inputs");
        if (found->second >= definition_positions.at(inst))
            throw std::runtime_error("x64 pseudo marker must follow its producer");
        early_definitions[found->second].push_back(inst);
        early_pseudos.insert(inst);
    }
    // Phi validation and stable CFG assignment do not need frame addresses.
    // Complete them first so omission decisions use the actual allocation.
    ConfigurePhis();
    if (cfg_allocation && execution_mode == ExecutionMode::Ordinary) {
        const backend::FunctionPlan empty_metadata;
        auto budget = AllocationBudget(block, function_plan, flags_resident, loop_weighted_costs, scalar_unary_phases);
        // Freshness follows from the private synchronous preparation path,
        // not MatchesBody/Validate alone (which do not detect changed args).
        if (prepared_allocation && prepared_allocation->Enabled() &&
            prepared_allocation->HasLiveness() && prepared_allocation->HasInterference() &&
            prepared_allocation->MatchesBody(block) && prepared_allocation->Validate() &&
            prepared_allocation->PhysicalRegisters() == budget.physical_registers &&
            prepared_allocation->LoopWeightedCostsEnabled() == budget.loop_weighted_costs &&
            prepared_allocation->ScalarUnaryPhasesEnabled() == budget.scalar_unary_phases) {
            cfg_register_plan = std::move(*prepared_allocation);
            preparation_stats.reused_final_allocation = true;
        } else {
            ++preparation_stats.full_allocation_builds;
            cfg_register_plan = CfgRegisterPlan::Build(block, function_plan ? *function_plan : empty_metadata,
                                                     std::move(budget));
        }
        if (!cfg_register_plan.Enabled() || !cfg_register_plan.Validate())
            throw std::runtime_error("x64 CFG allocation rejected invalid CFG/SSA metadata");
        sparse_frame_plan = SparseFramePlan::Build(block, function_plan, cfg_register_plan, flag_producers,
                                                  scalar_rematerialization);
        if (!sparse_frame_plan.Enabled())
            throw std::runtime_error("x64 sparse frame requires the validated CFG allocation");
    }
    // Shared topology-only fallback is built once when either independent
    // advanced plan needs it. Vector liveness/interference remain independent.
    CfgRegisterPlan auxiliary_graph;
    const auto* advanced_graph = &cfg_register_plan;
    if ((this->native_cf_chains || this->vector_cfg_allocation) && !advanced_graph->Enabled()) {
        const backend::FunctionPlan empty_metadata;
        auxiliary_graph = CfgRegisterPlan::ValidateGraph(block, function_plan ? *function_plan : empty_metadata);
        advanced_graph = &auxiliary_graph;
    }
    if (this->vector_cfg_allocation) {
        vector_register_plan = VectorRegisterPlan::Build(*advanced_graph, function_plan, std::move(vector_register_options));
        if (!vector_register_plan.Enabled()) {
            // Only an exhausted independent vector analysis may fall back.
            // Keep every legal scalar/flags/CF plan and the original homes/
            // legacy forwarding path. Invalid inputs still fail closed.
            if (vector_register_plan.Rejection() != VectorPlanRejection::AnalysisBudget)
                throw std::runtime_error("x64 vector CFG cache rejected invalid graph/register budget/analysis");
        } else if (!vector_register_plan.Validate()) {
            throw std::runtime_error("x64 vector CFG cache rejected inconsistent analysis");
        }
    }
    std::vector<std::pair<u32, u32>> live;
    std::vector<u32> available;
    u32 offset{};
    for (u32 position = 0; position < instructions.size(); ++position) {
        auto& inst = *instructions[position];
        if (inst.ReturnType() == ir::ValueType::V256)
            throw std::runtime_error("x64 JIT does not support V256");
        guest_instructions |= inst.GetOp() == O::AdvancePC;
        if (!local_control_flow) {
            std::erase_if(live, [&](const auto& interval) {
                if (interval.first >= position) return false;
                available.push_back(interval.second); return true;
            });
        }
        const auto allocate = [&](ir::Inst* definition) {
            u32 home;
            if (sparse_frame_plan.Enabled()) {
                const auto selected = sparse_frame_plan.Home(definition);
                if (!selected) return;
                home = *selected;
                offset = std::max(offset, home + 32);
            } else if (available.empty()) { home = offset; offset += 32; }
            else { home = available.back(); available.pop_back(); }
            homes.emplace(definition, home);
            live.emplace_back(std::max(position, last_use[definition]), home);
        };
        if (!early_pseudos.contains(&inst)) allocate(&inst);
        if (!early_definitions.empty()) for (auto* pseudo : early_definitions[position]) allocate(pseudo);
    }
    home_bytes = offset;
    local_begin = offset;
    for (auto* inst : instructions) {
        if (inst->GetOp() == O::DefineLocal) {
            const auto local = inst->GetArg<ir::Local>(0);
            locals.emplace(local.id, offset);
            offset += 32;
        }
    }
    local_bytes = offset - local_begin;
    phi_scratch = offset;
    offset += max_phi_moves * 16;
    // Dedicated helper-preservation slots follow all canonical/metadata,
    // local and Phi storage. They do not borrow guest State or SSA homes.
    if (!vector_register_plan.UsedRegisters().empty()) {
        offset = (offset + 15) & ~15u;
        vector_call_frame_begin = offset;
        vector_call_frame_bytes = vector_register_plan.UsedRegisters().size() * 16;
        offset += vector_call_frame_bytes;
    }
    // Six saved SysV registers leave RSP 8 mod 16. An 8-mod-16 frame restores
    // call alignment and reserves eight extra bytes for the host FP state.
    frame_size = ((offset + 15) & ~15u) + 24;
    if (frame_size > 8 * 1024 * 1024) throw std::runtime_error("x64 JIT frame exceeds stack limit");
    if (!cfg_register_plan.Enabled() && scalar_cache) {
        if (function_plan) {
            // ConfigurePhis has verified the CFG and every actual copy before
            // these constraints can authorize body-local function allocation.
            ScalarRegisterPlan::FunctionConstraints constraints;
            constraints.block = block;
            const auto exclude_copies = [&](const std::vector<PhiMove>& moves) {
                for (const auto& move : moves) {
                    constraints.home_only.insert(move.destination);
                    if (move.source.IsValue()) constraints.home_only.insert(move.source.value.Def());
                }
            };
            exclude_copies(entry_phis);
            for (const auto& [edge, moves] : taken_phis) { (void)edge; exclude_copies(moves); }
            for (const auto& [edge, moves] : fallthrough_phis) { (void)edge; exclude_copies(moves); }
            // Include unreachable Phi headers too: they still cannot acquire
            // slots, and their parameters must never become lexical SSA uses.
            for (auto& inst : block->GetInstList()) if (inst.GetOp() == O::AddPhi) {
                constraints.home_only.insert(&inst);
                for (const auto& param : inst.GetArg<ir::Params>(0))
                    if (param.data.IsValue()) constraints.home_only.insert(param.data.value.Def());
            }
            const auto exclude_terminal = [&](const auto& self, const ir::Terminal& terminal) -> void {
                VisitVariant<void>(terminal, [&](const auto& term) {
                    using T = std::decay_t<decltype(term)>;
                    if constexpr (std::is_same_v<T, ir::terminal::If>) {
                        constraints.home_only.insert(term.cond.Def()); self(self, term.then_); self(self, term.else_);
                    } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                        self(self, term.then_); self(self, term.else_);
                    } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                        constraints.home_only.insert(term.value.Def());
                        for (const auto& item : term.cases) self(self, item.then);
                    } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) self(self, term.else_);
                });
            };
            for (const auto& [marker, terminal] : function_plan->exits) {
                constraints.boundaries.insert(marker); exclude_terminal(exclude_terminal, terminal);
            }
            constraints.boundaries.insert(function_plan->halt_checks.begin(), function_plan->halt_checks.end());
            constraints.boundaries.insert(function_plan->stops.begin(), function_plan->stops.end());
            scalar_register_plan = ScalarRegisterPlan::BuildValidated(block, constraints, flags_resident);
        } else scalar_register_plan = ScalarRegisterPlan::Build(block, false, flags_resident);
        if (!scalar_register_plan.ValidateNonOverlapping())
            throw std::runtime_error("x64 scalar cache plan has overlapping allocations");
    }
    if (this->native_cf_chains) {
        // Analyze exactly once after all private preparation/allocation and
        // metadata-home decisions, with no callbacks or mutations afterward.
        // Do not remove snapshots: delayed producer consumers retain homes.
        carry_chain_plan = CarryChainPlan::Build(*advanced_graph, function_plan,
            uniform_extent_known ? std::optional<u64>{uniform_bytes} : std::nullopt);
    }
}

void Translator::ConfigureUniformBindings(const Config* config) {
    uniform_extent_known = config != nullptr;
    uniform_bytes = config ? config->uniform_buffer_size : 0;
    if (config) for (const auto& desc : config->buffers_static_alloc) {
        if (!desc.size || desc.size > (desc.is_float ? 16 : 8) || (desc.size & (desc.size - 1)))
            throw std::runtime_error("x64 JIT invalid uniform binding width (V256 unsupported)");
        if (desc.reg >= 32)
            throw std::runtime_error("x64 JIT invalid uniform binding register");
        if (desc.offset > config->uniform_buffer_size || desc.size > config->uniform_buffer_size - desc.offset ||
            u64(state_offset_uniform_buffer) + desc.offset + desc.size > INT32_MAX)
            throw std::runtime_error("x64 JIT uniform binding exceeds uniform buffer or displacement limit");
        for (const auto& [key, previous] : uniform_bindings)
            if ((previous.reg == desc.reg && previous.is_float == desc.is_float) ||
                (u64(desc.offset) < u64(previous.offset) + previous.size &&
                 u64(previous.offset) < u64(desc.offset) + desc.size))
                throw std::runtime_error("x64 JIT duplicate or overlapping uniform bindings");
        uniform_bindings.emplace(u32(desc.reg) | (u32(desc.is_float) << 16), desc);
    }
    // Host ops are optimizer IR with logical register bindings, not physical
    // x86 registers. Validate before emitting or publishing any native code.
    for (auto& inst : block->GetInstList()) {
        switch (inst.GetOp()) {
            case O::GetHostGPR: case O::GetHostFPR: case O::SetHostGPR: case O::SetHostFPR:
                (void)HostBinding(&inst); break;
            case O::X87Op:
                (void)ValidateX87RegisterOp(&inst, config ? config->uniform_buffer_size : UINT64_MAX);
                break;
            case O::Crc32:
                (void)ValidateIeeeCrc32(&inst);
                break;
            case O::VecCmpEq: case O::VecCmpGt: {
                // Validate the whole block, including skipped local branches,
                // before vector emission narrows the lane width for emission.
                if (!inst.ArgAt(2).IsImm())
                    throw std::runtime_error("x64 vector compare requires immediate lane width");
                const u64 width = inst.GetArg<ir::Imm>(2).Get();
                if (width != 8 && width != 16 && width != 32 && width != 64)
                    throw std::runtime_error("x64 vector compare lane width must be 8,16,32,64");
                break;
            }
            case O::VecMul: {
                if (!inst.ArgAt(2).IsImm())
                    throw std::runtime_error("x64 vector multiply requires immediate lane width");
                const u64 width = inst.GetArg<ir::Imm>(2).Get();
                if (width != 8 && width != 16 && width != 32 && width != 64)
                    throw std::runtime_error("x64 vector multiply lane width must be 8,16,32,64");
                break;
            }
            case O::VecSatShiftImm:
                (void)ValidateSaturatingShift(&inst);
                break;
            case O::VecUnsignedEstimate:
                (void)ValidateUnsignedEstimate(&inst);
                break;
            case O::VecSatAdd: case O::VecSatSub:
                (void)ValidateSaturatingAddSub(&inst);
                break;
            case O::VecSatShiftQC:
                (void)ValidateSaturatingShiftQC(&inst);
                break;
            default: break;
        }
    }
}

const UniformMapDesc& Translator::HostBinding(ir::Inst* inst) const {
    const auto op = inst->GetOp();
    const bool get = op == O::GetHostGPR || op == O::GetHostFPR;
    const bool fpr = op == O::GetHostFPR || op == O::SetHostFPR;
    const u64 reg = inst->GetArg<ir::Imm>(get ? 0 : 1).Get();
    const u64 offset = inst->GetArg<ir::Imm>(get ? 1 : 2).Get();
    const auto type = get ? inst->ReturnType() : inst->GetArg<ir::Value>(0).Type();
    if (type < ir::ValueType::U8 || type > ir::ValueType::V256 ||
        (!get && inst->GetArg<ir::Value>(0).Def()->ReturnType() == ir::ValueType::VOID))
        throw std::runtime_error("x64 JIT host-register IR has no defined value type");
    const u32 size = ir::GetValueSizeByte(type);
    if (reg >= 32)
        throw std::runtime_error("x64 JIT host-register IR has invalid binding register");
    const auto found = uniform_bindings.find(u32(reg) | (u32(fpr) << 16));
    if (found == uniform_bindings.end())
        throw std::runtime_error("x64 JIT host-register IR has no uniform binding");
    const auto& desc = found->second;
    if (!size || size > (fpr ? 16u : 8u) || size > desc.size || offset > desc.size - size ||
        (fpr && offset % size))
        throw std::runtime_error("x64 JIT host-register IR has invalid binding slice (V256 unsupported)");
    return desc;
}



u32 Translator::Home(ir::Inst* inst) const {
    const auto found = homes.find(inst);
    if (found == homes.end()) throw std::runtime_error("x64 JIT requires block-local SSA inputs");
    return found->second;
}

TransferRegion Translator::ClassifyTransfer(const Address& address, u32 bytes) const {
    const auto expression = address.getRegExp();
    const auto& base = expression.getBase();
    const auto& index = expression.getIndex();
    if (!base.getBit() || (base.getIdx() != 12 && base.getIdx() != 13)) return TransferRegion::OtherAddress;
    if (index.getBit()) return base.getIdx() == 13 ? TransferRegion::IndexedFrame : TransferRegion::OtherAddress;
    const auto displacement = static_cast<s64>(expression.getDisp());
    const auto within = [&](u32 begin, u32 length) {
        return displacement >= begin && u64(displacement - begin) <= length && bytes <= length - u64(displacement - begin);
    };
    if (base.getIdx() == 12) {
        if (within(state_offset_host_flags, sizeof(u64))) return TransferRegion::StateFlags;
        if (displacement >= state_offset_uniform_buffer) {
            const auto relative = u64(displacement - state_offset_uniform_buffer);
            if (!uniform_extent_known || (relative <= uniform_bytes && bytes <= uniform_bytes - relative))
                return TransferRegion::Uniform;
        }
        return TransferRegion::StateOther;
    }
    if (within(phi_scratch, max_phi_moves * 16)) return TransferRegion::PhiScratch;
    if (within(local_begin, local_bytes)) return TransferRegion::Local;
    // MemoryCopy's VOID instruction borrows its low home words for addresses,
    // not SSA values. Those words can share a recycled home with an old value.
    if (emitting_inst && (emitting_inst->GetOp() == O::MemoryCopy || emitting_inst->GetOp() == O::MemoryCopyTSO)) {
        const auto own_home = homes.find(emitting_inst);
        if (own_home != homes.end() && within(own_home->second, 16)) return TransferRegion::AddressScratch;
    }
    if (within(0, home_bytes)) {
        const auto slot_offset = u32(displacement) & 31;
        if (slot_offset < 16 && bytes <= 16 - slot_offset) return TransferRegion::CanonicalSSA;
        if (slot_offset >= 16 && slot_offset < 24 && bytes <= 24 - slot_offset) return TransferRegion::ProducerMetadata;
        if (slot_offset >= 24 && bytes <= 32 - slot_offset) return TransferRegion::AddressScratch;
    }
    return TransferRegion::FrameOther;
}

void Translator::ObserveTransfer(const Xbyak::Operand& operand, bool store, bool vector, u32 bits) {
    if (!operand.isMEM()) return;
    if (!bits) { ++statistics.unresolved_transfer_width_sites; return; }
    const u32 bytes = bits / 8;
    const auto region = ClassifyTransfer(static_cast<const Address&>(operand), bytes);
    auto& count = statistics.transfers[static_cast<unsigned>(region)];
    if (store) {
        if (vector) ++count.vector_store_sites; else ++count.scalar_store_sites;
        count.store_bytes += bytes;
    } else {
        if (vector) ++count.vector_load_sites; else ++count.scalar_load_sites;
        count.load_bytes += bytes;
    }
}

void Translator::CountPhiCopies(const std::vector<PhiMove>& moves) {
    for (const auto& move : moves) {
        if (move.source.IsImm()) ++statistics.phi_immediate_copy_sites;
        else ++statistics.phi_value_copy_sites;
    }
}

void Translator::Mask(const Reg64& reg, u32 bits) {
    if (bits == 8) movzx(reg.cvt32(), reg.cvt8());
    else if (bits == 16) movzx(reg.cvt32(), reg.cvt16());
    else if (bits == 32) mov(reg.cvt32(), reg.cvt32());
}

void Translator::SignExtend(const Reg64& reg, u32 bits) {
    if (bits == 8) movsx(reg, reg.cvt8());
    else if (bits == 16) movsx(reg, reg.cvt16());
    else if (bits == 32) movsxd(reg, reg.cvt32());
}

std::optional<unsigned> Translator::AllocatedScalarRegister(ir::Inst* producer) const noexcept {
    if (cfg_register_plan.Enabled()) return cfg_register_plan.Register(producer);
    if (const auto slot = scalar_register_plan.Slot(producer))
        return ScalarRegisterPlan::host_register_indices[*slot];
    return std::nullopt;
}

bool Translator::RequiresScalarHome(ir::Inst* producer) const noexcept {
    if (sparse_frame_plan.RematerializedValue(producer)) return false;
    return cfg_register_plan.Enabled() ? cfg_register_plan.RequiresCanonicalHome(producer)
                                       : scalar_register_plan.RequiresCanonicalHome(producer);
}

void Translator::Read(const Reg64& reg, ir::Value value) {
    if (value.Def()->ReturnType() == ir::ValueType::VOID || value.Type() == ir::ValueType::VOID)
        throw std::runtime_error("x64 JIT scalar input has no defined IR return type");
    if (const auto physical = AllocatedScalarRegister(value.Def())) {
        mov(reg, Reg64{static_cast<int>(*physical)});
        ++statistics.scalar_cache_read_sites;
    } else if (const auto constant = sparse_frame_plan.RematerializedValue(value.Def())) {
        // MOV preserves native flags. The plan has already truncated to the
        // producer width, matching Write; the consumer view mask follows.
        mov(reg, *constant);
        ++statistics.scalar_rematerialized_read_sites;
    } else mov(reg, qword[r13 + Home(value.Def())]);
    Mask(reg, ir::GetValueSizeByte(value.Type()) * 8);
}

void Translator::ReadVector(const Xmm& reg, ir::Value value) {
    if (value.Def()->ReturnType() == ir::ValueType::VOID || value.Type() == ir::ValueType::VOID)
        throw std::runtime_error("x64 JIT vector input has no defined IR return type");
    if (ir::GetValueSizeByte(value.Type()) > 16) throw std::runtime_error("x64 JIT does not support V256");
    if (vector_allocation_active) if (const auto physical = vector_register_plan.Register(value.Def())) {
        movdqa(reg, Xmm{static_cast<int>(*physical)});
        ++statistics.vector_cfg_cache_read_sites;
        return;
    }
    if (!vector_register_plan.Enabled() && cached_vector == value.Def()) { movdqa(reg, xmm2); ++statistics.vector_forward_read_sites; return; }
    movdqu(reg, ptr[r13 + Home(value.Def())]);
}

void Translator::Data(const Reg64& reg, const ir::DataClass& data) {
    if (data.IsValue()) Read(reg, data.value);
    else mov(reg, data.IsImm() ? data.imm.Get() : 0);
}

void Translator::Operand(const Reg64& reg, const ir::Operand& operand) {
    Data(reg, operand.GetLeft());
    if (operand.GetRight().Null() || operand.GetOp().type == ir::OperandOp::None) return;
    Data(rcx, operand.GetRight());
    switch (operand.GetOp().type) {
        case ir::OperandOp::Plus: add(reg, rcx); break;
        case ir::OperandOp::PlusExt: shl(rcx, operand.GetOp().shift_ext); add(reg, rcx); break;
        case ir::OperandOp::LSL: shl(reg, cl); break;
        case ir::OperandOp::LSR: shr(reg, cl); break;
        default: throw std::runtime_error("x64 JIT unsupported operand modifier");
    }
}

void Translator::Write(ir::Inst* inst, const Reg64& reg) {
    if (inst->ReturnType() == ir::ValueType::VOID) return;
    const auto size = ir::GetValueSizeByte(inst->ReturnType());
    if (!size) return;
    Mask(reg, size * 8);
    if (RequiresScalarHome(inst)) {
        mov(qword[r13 + Home(inst)], reg);
        // Full CFG runtime-use analysis distinguishes scalar spills from
        // raw containers. Metadata at +16 and explicit pair results at +8
        // remain owned by their producer emitters.
        if (cfg_register_plan.RequiresFullContainer(inst))
            mov(qword[r13 + Home(inst) + 8], 0);
        else ++statistics.scalar_high_word_store_omissions;
    }
    if (const auto physical = AllocatedScalarRegister(inst)) {
        const Reg64 cache_reg{static_cast<int>(*physical)};
        if (cache_reg.getIdx() != reg.getIdx()) {
            mov(cache_reg, reg);
            ++statistics.scalar_cache_write_sites;
        }
    }
}

void Translator::WriteVector(ir::Inst* inst, const Xmm& reg) {
    if (inst->ReturnType() == ir::ValueType::VOID) return;
    const auto size = ir::GetValueSizeByte(inst->ReturnType());
    if (size > 16) throw std::runtime_error("x64 JIT does not support V256");
    if (vector_forwarding.contains(inst) && !vector_register_plan.Enabled()) { movdqa(xmm2, reg); cached_vector = inst; ++statistics.vector_forward_write_sites; return; }
    if (vector_forwarding.contains(inst) && vector_register_plan.Enabled()) ++statistics.vector_cfg_legacy_home_retentions;
    // Generic vector operations and raw BitCast/GetResult aliases publish
    // their complete initialized 128-bit container, matching ARM64, RV64
    // and the interpreter. A narrow annotation selects access/consumer
    // widths; it does not insert a truncation into vector computation.
    movdqu(ptr[r13 + Home(inst)], reg);
    if (vector_allocation_active) if (const auto physical = vector_register_plan.Register(inst)) {
        if (reg.getIdx() != static_cast<int>(*physical)) {
            movdqa(Xmm{static_cast<int>(*physical)}, reg);
            ++statistics.vector_cfg_cache_write_sites;
        }
    }
}

void Translator::PublishPseudoResults(ir::Inst* inst, ir::OpCode op, u32 metadata_offset) {
    for (auto* pseudo : inst->GetPseudoOperations(op)) {
        mov(rax, qword[r13 + Home(inst) + metadata_offset]);
        Write(pseudo, rax);
    }
}

void Translator::Call(u64 target) { cached_vector = nullptr; mov(r11, target); call(r11); }
void Translator::SaveVectorCallRegisters() {
    if (!vector_allocation_active) return;
    const auto& registers = vector_register_plan.UsedRegisters();
    for (u32 slot = 0; slot < registers.size(); ++slot) {
        movdqu(ptr[r13 + vector_call_frame_begin + slot * 16], Xmm{static_cast<int>(registers[slot])});
        ++statistics.vector_cfg_helper_save_sites;
    }
}
void Translator::RestoreVectorCallRegisters() {
    if (!vector_allocation_active) return;
    const auto& registers = vector_register_plan.UsedRegisters();
    for (u32 slot = 0; slot < registers.size(); ++slot) {
        movdqu(Xmm{static_cast<int>(registers[slot])}, ptr[r13 + vector_call_frame_begin + slot * 16]);
        ++statistics.vector_cfg_helper_restore_sites;
    }
}
void Translator::Condition(ir::Cond condition) {
    if (condition == ir::Cond::AL || condition == ir::Cond::NV) { mov(eax, 1); return; }
    ReadGuestFlags(rax);
    const auto bit = [&](u32 shift) { shr(rax, shift); and_(eax, 1); };
    switch (condition) {
        case ir::Cond::EQ: case ir::Cond::NE: bit(30); break;
        case ir::Cond::CS: case ir::Cond::CC: bit(29); break;
        case ir::Cond::MI: case ir::Cond::PL: bit(31); break;
        case ir::Cond::VS: case ir::Cond::VC: bit(28); break;
        case ir::Cond::HI: case ir::Cond::LS:
            mov(rdx, rax); shr(rdx, 30); shr(rax, 29); xor_(eax, 1); or_(rax, rdx); and_(eax, 1); break;
        case ir::Cond::GE: case ir::Cond::LT: case ir::Cond::GT: case ir::Cond::LE:
            mov(rdx, rax); shr(rdx, 3); xor_(rax, rdx); shr(rax, 28); and_(eax, 1);
            if (condition == ir::Cond::GT || condition == ir::Cond::LE) {
                ReadGuestFlags(rdx); shr(rdx, 30); and_(edx, 1); or_(eax, edx);
            }
            break;
        default: xor_(eax, eax); return;
    }
    if (condition == ir::Cond::NE || condition == ir::Cond::CC || condition == ir::Cond::PL ||
        condition == ir::Cond::VC || condition == ir::Cond::HI || condition == ir::Cond::GE || condition == ir::Cond::GT)
        xor_(eax, 1);
}

void Translator::SaveProducerFlags(ir::Inst* producer, ir::Flags requested) {
    const auto usage = ClassifyProducerFlags(producer->GetOp(), producer->ReturnType(), requested);
    if (!usage.published_mask) return;
    const u32 bits = producer->ReturnType() == ir::ValueType::VOID ? 0 : ir::GetValueSizeByte(producer->ReturnType()) * 8;
    if (!bits || bits > 64) return;
    const auto op = producer->GetOp();
    const bool subtract = op == O::Sub || op == O::Sbb || op == O::Neg;
    const bool arithmetic = subtract || op == O::Add || op == O::Adc;
    const bool logical = op == O::And || op == O::Or || op == O::Xor || op == O::AndNot;
    const bool multiply = op == O::Mul && bits < 64;
    u64 mask{};
    const auto requested_bit = [&](ir::Flags flag) { return True(requested & flag); };
    const bool native_metadata = usage.needs_metadata;
    // Only real arithmetic snapshots contain width-correct SF/ZF. Narrow
    // multiply synthesizes C/V alone; NZ-only publications keep the result
    // path without adding a metadata load. Parity still publishes its byte.
    const bool native_nz = usage.nz_from_native;
    if (usage.needs_result) Read(rax, ir::Value{producer});
    xor_(r8d, r8d);
    if (native_metadata)
        mov(rdx, qword[r13 + Home(producer) + 16]);
    if (native_nz) {
        const u32 nz_mask = (requested_bit(ir::Flags::Negate) ? 0x80u : 0u) |
            (requested_bit(ir::Flags::Zero) ? 0x40u : 0u);
        if (nz_mask) {
            mov(ecx, edx); and_(ecx, nz_mask); shl(ecx, 24); or_(r8, rcx);
            mask |= u64{nz_mask} << 24;
        }
    } else {
        if (requested_bit(ir::Flags::Negate)) {
            mov(rcx, rax); shr(rcx, bits - 1); and_(ecx, 1); shl(rcx, 31); or_(r8, rcx); mask |= u64{1} << 31;
        }
        if (requested_bit(ir::Flags::Zero)) {
            test(rax, rax); sete(cl); movzx(ecx, cl); shl(rcx, 30); or_(r8, rcx); mask |= u64{1} << 30;
        }
    }
    if (arithmetic || logical || multiply) {
        if (requested_bit(ir::Flags::Carry)) {
            mask |= u64{1} << 29;
            if (!logical) { mov(rcx, rdx); if (subtract) xor_(ecx, 1); and_(ecx, 1); shl(rcx, 29); or_(r8, rcx); }
        }
        if (requested_bit(ir::Flags::Overflow)) {
            mask |= u64{1} << 28;
            if (!logical) { mov(rcx, rdx); shr(rcx, 11); and_(ecx, 1); shl(rcx, 28); or_(r8, rcx); }
        }
    }
    if (arithmetic && requested_bit(ir::Flags::AuxiliaryCarry)) {
        mov(rcx, rdx); shr(rcx, 4); and_(ecx, 1); shl(rcx, 26); or_(r8, rcx); mask |= u64{1} << 26;
    }
    if (requested_bit(ir::Flags::Parity)) { and_(eax, 255); or_(r8, rax); mask |= 255; }
    if (mask) {
        if (flags_resident && flags_residency_active) {
            // Merge the full packed word in place. A 32-bit destination or
            // sign-extended immediate mask would lose unused high State bits.
            mov(rcx, ~mask); and_(rbx, rcx); or_(rbx, r8);
            ++statistics.flags_resident_merge_sites;
        } else {
            ReadGuestFlags(rax); mov(rcx, ~mask); and_(rax, rcx); or_(rax, r8);
            WriteGuestFlags(rax);
        }
    }
}

void Translator::Return(HaltReason reason) {
    if (reason != HaltReason::None) mov(dword[r12 + state_offset_halt_reason], static_cast<u32>(reason));
    if (reason == HaltReason::None && execution_mode == ExecutionMode::SingleInstruction)
        jmp(retirement, T_NEAR);
    else jmp(epilogue, T_NEAR);
}

void Translator::PollInterrupt() {
    Label run, signal;
    mov(rax, qword[r12 + state_offset_exit_request]);
    test(rax, rax); je(run);
    bt(rax, 63); jc(signal);
    Return(HaltReason::CodeMiss);
    L(signal); Return(HaltReason::Signal);
    L(run);
}

void Translator::CheckHalt(bool poll_signal) {
    // Existing synchronous faults own the reason. Instruction-internal
    // stopping points recognize Signal only: low SMC bits require restartable
    // CFG entries and must not replay an already completed guest store.
    Label stop, run;
    cmp(dword[r12 + state_offset_halt_reason], 0); jne(stop, T_NEAR);
    // A helper's RAX result must be committed before the request load.
    if (poll_signal && execution_mode == ExecutionMode::Ordinary) {
        // Aligned x86 loads provide acquire ordering for the release request.
        mov(rax, qword[r12 + state_offset_exit_request]);
        bt(rax, 63); jnc(run);
        mov(dword[r12 + state_offset_halt_reason], static_cast<u32>(HaltReason::Signal));
    } else {
        jmp(run);
    }
    L(stop);
    // Only the stopped path executes this PC publication. The continuation
    // still owes its own publication and may retain an uncommitted RAX result.
    const bool dirty = location_dirty;
    PublishLocation();
    location_dirty = dirty;
    jmp(epilogue, T_NEAR);
    L(run);
}

void Translator::Translate() {
    flags_residency_active = false;
    vector_allocation_active = false;
    statistics = {}; statistics.frame_bytes = frame_size;
    statistics.vector_cfg_requested = vector_cfg_allocation;
    statistics.vector_cfg_plan_enabled = vector_register_plan.Enabled();
    statistics.vector_cfg_rejection_reason = static_cast<u32>(vector_register_plan.Rejection());
    const auto& vector_analysis = vector_register_plan.AnalysisStats();
    statistics.vector_cfg_budget_limit = static_cast<u32>(vector_analysis.exhausted);
    statistics.vector_cfg_analysis_nodes = vector_analysis.nodes;
    statistics.vector_cfg_analysis_work = vector_analysis.work;
    statistics.vector_cfg_pair_work = vector_analysis.pair_work;
    statistics.vector_cfg_interference_storage_entries = vector_analysis.interference_storage_entries;
    statistics.vector_cfg_fixed_point_passes = vector_analysis.fixed_point_passes;
    statistics.uniform_extent_known = uniform_extent_known;
    if (execution_mode == ExecutionMode::SingleInstruction) {
        // Observe before changing guest PC, frame/recovery ownership or phis.
        // Entry RSP is 8 mod 16; saving RDI also aligns this host call and
        // preserves State* across its caller-saved register clobbers.
        Label body;
        push(rdi);
        Call(reinterpret_cast<u64>(&ObserveSingleEntry));
        pop(rdi);
        test(eax, eax); jz(body, T_NEAR);
        ret();
        L(body);
    }
    // A signal can abandon a helper's own callee-saved register spill frame.
    // Save every SysV nonvolatile here, not just the block's register anchors.
    push(rbx); push(rbp); push(r12); push(r13); push(r14); push(r15);
    mov(r12, rdi);
    sub(rsp, frame_size);
    mov(r13, rsp);
    lea(rax, ptr[rip + recovery]);
    mov(qword[r12 + state_offset_riscv_recovery_pc], rax);
    mov(qword[r12 + state_offset_riscv_recovery_frame], r13);
    mov(qword[r12 + state_offset_riscv_memory_owned], 0);
    stmxcsr(dword[r13 + frame_size - 8]);
    mov(dword[r13 + frame_size - 4], 0x1f80);
    ldmxcsr(dword[r13 + frame_size - 4]);
    mov(rax, block->GetStartLocation().Value());
    mov(qword[r12 + state_offset_current_loc], rax);
    mov(qword[r12 + state_offset_prev_loc], rax);
    ReloadGuestFlags();
    flags_residency_active = flags_resident;
    vector_allocation_active = vector_register_plan.Enabled();
    EmitPhiMoves(entry_phis);
    if (execution_mode == ExecutionMode::Ordinary) {
        CheckHalt();
        PollInterrupt();
    }
    for (auto& inst : block->GetInstList()) {
        if (auto edge = fallthrough_phis.find(&inst); edge != fallthrough_phis.end()) EmitPhiMoves(edge->second);
        if (function_plan && function_plan->exits.contains(&inst)) {
            PublishLocation();
            Terminal(function_plan->exits.at(&inst));
        } else if (function_plan && function_plan->halt_checks.contains(&inst)) {
            CheckHalt();
        } else {
            emitting_inst = &inst;
            switch (inst.GetOp()) {
                case O::SaveFlags: case O::BranchOnlyFlags:
                case O::PublishFCmpFlags: case O::PublishSse42StrFlags:
                case O::ClearFlags: case O::SetCarry: case O::SetOverflow: case O::InvertCarry:
                    ++statistics.flags_materialization_ir_sites; break;
                default: break;
            }
            Emit(&inst); emitting_inst = nullptr;
        }
    }
    Terminal(block->GetTerminal());
    if (execution_mode == ExecutionMode::SingleInstruction) {
        // Every normal terminal selected and committed its successor first.
        // This boundary sees async Signal only after the complete unit; an
        // owner-reported synchronous reason always retains priority.
        L(retirement);
        cmp(dword[r12 + state_offset_halt_reason], 0); jne(epilogue, T_NEAR);
        mov(edx, static_cast<u32>(HaltReason::Step));
        mov(rax, qword[r12 + state_offset_exit_request]);
        Label completed;
        bt(rax, 63); jnc(completed, T_NEAR);
        mov(edx, static_cast<u32>(HaltReason::Signal));
        L(completed);
        mov(dword[r12 + state_offset_halt_reason], edx);
        jmp(epilogue, T_NEAR);
    }
    L(recovery);
    // A helper fault abandons the normal XMM restore sequence. Recovery and
    // exit only consume committed State/homes, never assigned vector cache.
    vector_allocation_active = false;
    recovery_offset = getSize();
    // A signal may have abandoned a helper's own RBX spill frame. Helpers
    // were entered with committed flags, so reload rather than trust RBX.
    ReloadGuestFlags();
    L(epilogue);
    FlushGuestFlags();
    // The shared exit commits the complete packed word once. EndMemory
    // observes State directly; no body instruction can consume RBX again.
    // Recovery reload above still precedes this commit if a helper fault
    // abandoned its own callee-saved register frame.
    flags_residency_active = false;
    // A fault can abandon a backward REP MOVSB with DF set. SysV requires
    // DF clear before calling C++ recovery helpers or returning to the host.
    cld();
    mov(rdi, r12); Call(reinterpret_cast<u64>(&EndMemory));
    ldmxcsr(dword[r13 + frame_size - 8]);
    mov(eax, dword[r12 + state_offset_halt_reason]);
    mov(qword[r12 + state_offset_riscv_recovery_pc], 0);
    mov(qword[r12 + state_offset_riscv_recovery_frame], 0);
    mov(rsp, r13);
    add(rsp, frame_size);
    pop(r15); pop(r14); pop(r13); pop(r12); pop(rbp); pop(rbx); ret();
    if (execution_mode == ExecutionMode::SingleInstruction) readyRE();
    else ready();
    statistics.emitted_code_bytes = getSize();
}

void Translator::PublishLocation() {
    if (!location_dirty) return;
    mov(rax, guest_pc);
    mov(qword[r12 + state_offset_current_loc], rax);
    location_dirty = false;
}

void Translator::MemoryAddress(ir::Inst* inst, u32 size) {
    PublishLocation();
    Operand(rax, inst->GetArg<ir::Operand>(0));
    and_(rax, qword[r12 + offsetof(State, guest_addr_mask)]);
    mov(qword[r13 + Home(inst) + 24], rax);
    mov(rdi, r12); mov(rsi, rax); mov(edx, size);
    Call(reinterpret_cast<u64>(&CheckAndBeginMemory));
    test(eax, eax); jz(epilogue, T_NEAR);
    mov(r14, qword[r13 + Home(inst) + 24]);
    add(r14, qword[r12 + state_offset_pt]);
}

void Translator::MemoryAddressValue(ir::Inst* inst, ir::Value value, u32 size) {
    PublishLocation();
    Read(rax, value);
    and_(rax, qword[r12 + offsetof(State, guest_addr_mask)]);
    mov(qword[r13 + Home(inst) + 24], rax);
    mov(rdi, r12); mov(rsi, rax); mov(edx, size);
    Call(reinterpret_cast<u64>(&CheckAndBeginMemory));
    test(eax, eax); jz(epilogue, T_NEAR);
    mov(r14, qword[r13 + Home(inst) + 24]);
    add(r14, qword[r12 + state_offset_pt]);
}

void Translator::EndMemoryAccess() {
    mov(qword[r12 + state_offset_riscv_memory_owned], 0);
    mov(r11, reinterpret_cast<u64>(&riscv64::anonymous_memory_readers));
    lock(); dec(dword[r11]);
}

void Translator::RecordGuestAccess(size_t begin, u32 size, GuestAccessKind kind) {
    if (execution_mode != ExecutionMode::SingleInstruction) return;
    const auto end = getSize();
    if (begin >= end || end > UINT32_MAX)
        throw std::runtime_error("x64 guest access range exceeds emitted code offsets");
    guest_accesses.push_back({static_cast<u32>(begin), static_cast<u32>(end), size, kind});
}

void Translator::MarkUntrackedGuestAccess() noexcept {
    if (execution_mode == ExecutionMode::SingleInstruction)
        guest_access_coverage = GuestAccessCoverage::Unsupported;
}

void Translator::Emit(ir::Inst* inst) {
    // Keep declarations and dispatch tied to the same IR inventory as ARM64.
    // A newly added IR instruction needs an emitter definition to link.
#define INST(name, ...) case ir::OpCode::name: Emit##name(inst); return;
    switch (inst->GetOp()) {
#include "runtime/ir/ir.inc"
        default: Unsupported(inst);
    }
#undef INST
}

void Translator::Unsupported(ir::Inst* inst) {
    throw std::runtime_error(std::string("x64 JIT unsupported opcode: ") + ir::GetIRMetaInfo(inst->GetOp()).name);
}

void Translator::Terminal(const ir::Terminal& terminal) {
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> || std::is_same_v<T, ir::terminal::LinkBlockFast> || std::is_same_v<T, ir::terminal::ExternalLinkBlock>) {
            if (execution_mode == ExecutionMode::SingleInstruction) CheckHalt(false);
            mov(rax, term.next.Value()); mov(qword[r12 + state_offset_current_loc], rax); Return(HaltReason::None);
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost>) {
            if (execution_mode == ExecutionMode::SingleInstruction) CheckHalt(false);
            Return(HaltReason::CallHost);
        } else if constexpr (std::is_same_v<T, ir::terminal::If> || std::is_same_v<T, ir::terminal::Condition>) {
            if constexpr (std::is_same_v<T, ir::terminal::If>) Read(rax, term.cond); else Condition(term.cond);
            Label alternative; test(rax, rax); jz(alternative, T_NEAR);
            Terminal(term.then_); L(alternative); Terminal(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            CheckHalt(); Terminal(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            for (const auto& item : term.cases) {
                Label next; Read(rax, term.value); mov(rdx, item.case_value.Get()); cmp(rax, rdx); jne(next, T_NEAR);
                Terminal(item.then); L(next);
            }
            Return(HaltReason::None);
        } else if constexpr (std::is_same_v<T, ir::terminal::Invalid>) {
            if (execution_mode == ExecutionMode::SingleInstruction) CheckHalt(false);
            Return(HaltReason::IllegalCode);
        } else {
            if (execution_mode == ExecutionMode::SingleInstruction) {
                CheckHalt(false);
                // Dynamic BR/BLR/RET targets were committed by SetLocation.
                // Never replace that runtime target with original PC + 4.
                PublishLocation();
            }
            Return(HaltReason::None);
        }
    });
}

}  // namespace swift::runtime::backend::x64
