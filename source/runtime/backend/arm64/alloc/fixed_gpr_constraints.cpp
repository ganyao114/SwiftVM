#include "fixed_gpr_constraints.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <unordered_set>

#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/ir/hir_builder.h"

namespace swift::runtime::backend::arm64 {

bool IsFixedGPRPublicationObserver(ir::OpCode op) {
    return GuestStateMap::MayFaultOrObserve(op) || op == ir::OpCode::GetUniformAddress ||
           op == ir::OpCode::UniformBarrier;
}

bool IsFixedGPRPublicationObserver(const ir::Inst& inst, const FeatureSet& features) {
    // Legacy publication coalescers do not yet carry a target-specific
    // physical helper barrier at every use of this predicate. Keep their
    // conservative call boundary; fixed residence proofs separately use the
    // precise per-register HelperCallContract through GuestStateMap.
    (void)features;
    return IsFixedGPRPublicationObserver(inst.GetOp());
}

namespace {

using TerminalValues = std::unordered_set<ir::Inst*>;

void CollectTerminalValues(const ir::Terminal& terminal, TerminalValues& values) {
    VisitVariant<void>(terminal, [&](const auto& edge) {
        using T = std::decay_t<decltype(edge)>;
        auto add = [&](ir::Value value) {
            if (value.Defined()) values.insert(value.Def());
        };
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            add(edge.cond);
            CollectTerminalValues(edge.then_, values);
            CollectTerminalValues(edge.else_, values);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            add(edge.value);
            for (const auto& item : edge.cases) CollectTerminalValues(item.then, values);
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            CollectTerminalValues(edge.then_, values);
            CollectTerminalValues(edge.else_, values);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            CollectTerminalValues(edge.else_, values);
        }
    });
}

ir::Flags SetFlags(ir::Inst* producer) {
    ir::Flags result{};
    for (const auto& pseudo : producer->GetPseudoOperations()) {
        if (pseudo->GetOp() == ir::OpCode::SaveFlags ||
            pseudo->GetOp() == ir::OpCode::BranchOnlyFlags) {
            result |= pseudo->GetArg<ir::Flags>(1);
        }
    }
    return result;
}

bool IsControlBoundary(const ir::Inst& inst) {
    return inst.GetOp() == ir::OpCode::Goto || inst.GetOp() == ir::OpCode::NotGoto ||
           inst.GetOp() == ir::OpCode::BindLabel;
}

void PrepareBlock(ir::Block* block, RegAlloc& allocation, const FeatureSet& features,
                  const TerminalValues& terminal_values) {
    const auto& skip = GetSvmConfig().skip_prep;
    if (block->IsEmptyBlock() || skip.find("fixedgpr") != std::string::npos) return;
    BlockAnalysisIndex uses;
    uses.Build(block);
    GuestStateMap state;
    state.Analyze(block, features);
    if (skip.find("transfers") == std::string::npos &&
        skip.find("fixedtransfers") == std::string::npos) {
        for (auto& publication : block->GetInstList()) {
            auto transfer = MatchFixedGPRTransfer(&publication, uses, state);
            if (!transfer || !allocation.GetGprs().Get(transfer->source) ||
                !allocation.GetGprs().Get(transfer->target) ||
                // Already alias-safe reads need no additional residence or
                // publication. Keep their existing allocation and dead stores.
                state.FixedHomeSurvives(transfer->source, transfer->read->Id(),
                                        transfer->last_use)) continue;
            bool unsafe = terminal_values.contains(transfer->read);
            for (auto* alias : transfer->aliases) unsafe |= terminal_values.contains(alias);
            for (auto& inst : block->GetInstList()) {
                if (transfer->read->Id() < inst.Id() && inst.Id() <= transfer->last_use &&
                    IsControlBoundary(inst)) unsafe = true;
            }
            if (unsafe) continue;
            auto plan = [&](ir::Inst* value) {
                const bool after = value->Id() > publication.Id();
                allocation.PlanFixedGPRDefinition(value->Id(), {
                        .home = after ? transfer->target : transfer->source,
                        .transferred_home = static_cast<u16>(after ? UINT16_MAX : transfer->target),
                        .end = after ? transfer->last_use : publication.Id(),
                        .transfer = after ? UINT32_MAX : publication.Id(),
                        .last_use = transfer->last_use,
                        .publication = publication.Id(),
                        .owner = transfer->read->Id(),
                        .width = sizeof(u64),
                        .elided = true,
                });
            };
            plan(transfer->read);
            for (auto* alias : transfer->aliases) plan(alias);
        }
    }
    for (auto& read : block->GetInstList()) {
        if (skip.find("fixedreads") != std::string::npos ||
            read.GetOp() != ir::OpCode::GetHostGPR ||
            allocation.HasFixedGPRDefinition(read.Id()) || terminal_values.contains(&read)) continue;
        const auto home = MatchNarrowFixedGPRRead(read, uses, state, SetFlags);
        if (!home || !allocation.GetGprs().Get(*home)) continue;
        auto* consumer = uses.Uses(&read).front().consumer;
        // Narrow NZCV lowering can shift an ordinary capture in place. Binding
        // that capture to guest state instead requires restoring the guest
        // home and may cost more than its removed copy. Keep those choices in
        // target preparation; preallocate exact U32 ALU views and store values.
        if (consumer->GetOp() != ir::OpCode::StoreMemory &&
            (SetFlags(consumer) != ir::Flags::None || consumer->GetUses() == 0 ||
             ir::GetValueSizeByte(read.ReturnType()) != sizeof(u32) ||
             !consumer->HasValue() || ir::GetValueSizeByte(consumer->ReturnType()) != sizeof(u32)))
            continue;
        // Extension chains may be published directly by width coalescing.
        // Leave those ownership decisions to that pass; the shared matcher is
        // still used for their later target-specific read elision.
        if (consumer->GetOp() == ir::OpCode::ZeroExtend32 ||
            consumer->GetOp() == ir::OpCode::SignExtend) continue;
        bool control = false;
        for (auto& inst : block->GetInstList()) {
            if (read.Id() < inst.Id() && inst.Id() < consumer->Id() &&
                IsControlBoundary(inst)) control = true;
        }
        if (control) continue;
        allocation.PlanFixedGPRDefinition(read.Id(), {
                .home = *home,
                .end = consumer->Id(),
                .last_use = consumer->Id(),
                .width = static_cast<u8>(ir::GetValueSizeByte(read.ReturnType())),
                .elided = true,
        });
    }
    if (!features.ra_coalesce || skip.find("fixedresults") != std::string::npos) return;
    uses.BuildWriteSuccessors([&](const ir::Inst& inst) { return state.MayFaultOrObserve(inst); },
                             [](const ir::Inst& inst) {
        const auto value = inst.GetArg<ir::Value>(0);
        return inst.GetArg<ir::Imm>(2).Get() == 0 &&
               ir::GetValueSizeByte(value.Type()) >= sizeof(u32);
    });
    auto& instructions = block->GetInstList();
    for (auto it = instructions.begin(); it != instructions.end(); ++it) {
        auto& producer = *it;
        const auto next = std::next(it);
        if (!producer.HasValue() || next == instructions.end() ||
            next->GetOp() != ir::OpCode::SetHostGPR ||
            next->GetArg<ir::Value>(0).Def() != &producer ||
            next->GetArg<ir::Imm>(2).Get() != 0 || producer.GetUses(false) != 1 ||
            terminal_values.contains(&producer) || uses.FollowingWrite(&*next)) continue;
        const auto op = producer.GetOp();
        if (op != ir::OpCode::LoadImm && op != ir::OpCode::LoadMemory &&
            op != ir::OpCode::Add && op != ir::OpCode::Sub && op != ir::OpCode::And) continue;
        const auto width = ir::GetValueSizeByte(producer.ReturnType());
        const auto home = next->GetArg<ir::Imm>(1).Get();
        // The SSA producer may compute U32 for a byte/halfword ADC/SBB.
        // Its publication selects a narrow view and must preserve guest high
        // bits; such a value cannot be produced directly with a W write.
        if (ir::GetValueSizeByte(next->GetArg<ir::Value>(0).Type()) != width ||
            (width != 4 && width != 8) || !IsFixedGPRHome(home) ||
            !allocation.GetGprs().Get(home)) continue;
        bool conflict = false;
        for (auto& other : instructions) {
            if (allocation.HasFixedGPRDefinition(other.Id()) &&
                allocation.FixedGPRHomeIntersects(ir::Value{&other}, home,
                                                 producer.Id(), next->Id(), other.Id())) {
                conflict = true;
                break;
            }
        }
        if (conflict) continue;
        allocation.PlanFixedGPRDefinition(producer.Id(), {
                .home = static_cast<u16>(home),
                .end = next->Id(),
                .last_use = next->Id(),
                .publication = next->Id(),
                .owner = producer.Id(),
                .width = static_cast<u8>(width),
        });
    }
}

}  // namespace

void PrepareFixedGPRConstraints(ir::Block* block, RegAlloc& allocation,
                                const FeatureSet& features) {
    TerminalValues terminal_values;
    CollectTerminalValues(block->GetTerminal(), terminal_values);
    PrepareBlock(block, allocation, features, terminal_values);
}

void PrepareFixedGPRConstraints(ir::HIRFunction* function, RegAlloc& allocation,
                                const FeatureSet& features) {
    TerminalValues terminal_values;
    for (auto& block : function->GetHIRBlocksRPO())
        CollectTerminalValues(block.GetBlock()->GetTerminal(), terminal_values);
    for (auto& block : function->GetHIRBlocksRPO())
        PrepareBlock(block.GetBlock(), allocation, features, terminal_values);
}

void VerifyFixedGPRConstraints(ir::Block* block, const RegAlloc& allocation,
                               const FeatureSet& features) {
    bool has_constraints = false;
    for (auto& inst : block->GetInstList()) has_constraints |= allocation.HasFixedGPRDefinition(inst.Id());
    if (!has_constraints) return;
    std::array<std::vector<u32>, 32> writers;
    for (auto& inst : block->GetInstList()) {
        u32 mask = FixedGPRClobbers(inst, features);
        if (inst.GetOp() == ir::OpCode::SetHostGPR) mask |= 1u << inst.GetArg<ir::Imm>(1).Get();
        if (const auto helper = HelperCallContract::Resolve(inst, features)) {
            for (u32 home = 0; home <= 9; ++home)
                if (helper->ClobbersGPR(home)) mask |= 1u << home;
        }
        if (inst.HasValue() && !inst.IsBitCastOperation() &&
            !allocation.IsFixedGPRDefinitionElided(inst.Id()) &&
            allocation.ValueType(ir::Value{&inst}) == RegAlloc::GPR) {
            const auto home = allocation.ValueGPR(ir::Value{&inst}).id;
            const bool read_alias = inst.GetOp() == ir::OpCode::GetHostGPR &&
                    inst.GetArg<ir::Imm>(0).Get() == home &&
                    (ir::GetValueSizeByte(inst.ReturnType()) == 8 ||
                     allocation.IsHostReadCoalesced(inst.Id()));
            if (!read_alias) mask |= 1u << home;
        }
        while (mask) {
            writers[std::countr_zero(mask)].push_back(inst.Id());
            mask &= mask - 1;
        }
    }
    auto check = [&](u32 value, u16 home, u32 after, u32 before) {
        const auto next = std::upper_bound(writers[home].begin(), writers[home].end(), after);
        ASSERT_MSG(next == writers[home].end() || *next >= before,
                   "fixed GPR value {} in x{} is overwritten by IR {} before use {}",
                   value, home, next == writers[home].end() ? UINT32_MAX : *next, before);
    };
    for (auto& inst : block->GetInstList()) {
        const auto* fixed = allocation.FixedGPRDefinitionAt(inst.Id());
        if (!fixed) continue;
        ASSERT(allocation.ValueType(ir::Value{&inst}) == RegAlloc::GPR);
        ASSERT(allocation.ValueGPR(ir::Value{&inst}).id == fixed->home);
        check(inst.Id(), fixed->home, inst.Id(), fixed->end);
        if (fixed->transferred_home != UINT16_MAX)
            check(inst.Id(), fixed->transferred_home, fixed->transfer, fixed->last_use);
    }
}

}  // namespace swift::runtime::backend::arm64
