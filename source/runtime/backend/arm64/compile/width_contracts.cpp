#include <algorithm>
#include <bit>
#include "runtime/backend/arm64/compile/constraints.h"
#include "runtime/backend/arm64/compile/gpr_inputs.h"
#include "runtime/backend/arm64/compile/register_contract.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/terminal_uses.h"
#include "runtime/ir/low32_copy.h"
#include "width_contracts.h"

namespace swift::runtime::backend::arm64 {

namespace {
struct PhysicalGPR {
    u16 slot;
    u16 GetCode() const { return slot; }
};
class WidthAllocationView {
public:
    WidthAllocationView(compile::AllocationView a, const FeatureSet& f, const GPRSMask& u)
            : allocation(a), features(f), uniforms(u) {}
    void At(u32 id) { point = id; }
    u32 Point() const { return point; }
    const FeatureSet& GetFeatures() const { return features; }
    bool HasAllocation(ir::Value v) const {
        return allocation.ValueType(v) != compile::AllocationView::NONE;
    }
    PhysicalGPR X(ir::Value v) const {
        const auto loc =
                allocation.GPRLocationAt(v, point, compile::AllocationView::GPRView::WholeValue);
        return {static_cast<u16>(loc.kind == compile::AllocationView::GPR ? loc.slot : UINT16_MAX)};
    }
    bool SharesGPR(ir::Value a, ir::Value b) const {
        if (!a.Defined() || !b.Defined()) return false;
        if (allocation.PreservedGPRDefinitionAt(a.Id()) ||
            allocation.PreservedGPRDefinitionAt(b.Id())) {
            const auto x = X(a), y = X(b);
            return x.slot < 31 && x.slot == y.slot;
        }
        return allocation.ValueType(a) == compile::AllocationView::GPR &&
               allocation.ValueType(b) == compile::AllocationView::GPR &&
               allocation.ValueGPR(a).id == allocation.ValueGPR(b).id;
    }
    bool IsUniform(PhysicalGPR reg) const { return reg.slot < 31 && uniforms.Get(reg.slot); }
    GPRSMask DirtyGPR(u32 id) const { return allocation.DirtyGPR(id); }
    bool IsWidthChainCoalesced(u32 id) const { return allocation.IsWidthChainCoalesced(id); }
    u32 WidthChainAnchor(u32 id) const { return allocation.WidthChainAnchor(id); }
    bool IsLow32CopyCoalesced(u32 id) const { return allocation.IsLow32CopyCoalesced(id); }
    u32 Low32CopySource(u32 id) const { return allocation.Low32CopySource(id); }
    bool Low32SourceRecolored(ir::Value source, u32 bridge) const {
        return allocation.AllocationId(source) == bridge;
    }
    bool HasWidthComponentOwner(u32 id) const { return allocation.HasWidthComponentOwner(id); }
    bool WidthComponentOwnerCommitted(u32 id) const {
        return allocation.WidthComponentOwnerCommitted(id);
    }
    u16 WidthComponentOwnerTarget(u32 id) const { return allocation.WidthComponentOwnerTarget(id); }
    bool WidthComponentOwnerHighZero(u32 id) const {
        return allocation.WidthComponentOwnerHighZero(id);
    }
    bool IsHostWriteCoalesced(u32 id) const { return allocation.IsHostWriteCoalesced(id); }

private:
    compile::AllocationView allocation;
    const FeatureSet& features;
    const GPRSMask& uniforms;
    u32 point{};
};

bool IsKnownWidthChainWWrite(ir::Value value, const WidthAllocationView& context) {
    value = WWriteRoot(value, WWriteAliases::Resolved);
    if (!value.Defined()) {
        return false;
    }
    auto* def = value.Def();
    if (def->GetOp() == ir::OpCode::GetHostGPR) {
        return (context.GetFeatures().ra_width_chain ||
                context.GetFeatures().ra_width_chain_long) &&
               context.IsWidthChainCoalesced(value.Id()) &&
               context.WidthChainAnchor(value.Id()) == value.Id() &&
               ir::GetValueSizeByte(value.Type()) == sizeof(u32);
    }
    return HasKnownWWrite(value);
}

bool IsPinnedWViewProducer(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::Add:
        case ir::OpCode::Sub:
        case ir::OpCode::And:
        case ir::OpCode::AndNot:
        case ir::OpCode::Or:
        case ir::OpCode::Xor:
        case ir::OpCode::Mul:
            return true;
        default:
            return false;
    }
}

struct WidthContractQueries {
    bool needed{};
    u32 bridges{}, copies{};
};

WidthContractQueries CollectWidthContractQueries(const ir::Block* block,
                                                 compile::AllocationView allocation) {
    WidthContractQueries queries;
    for (const auto& inst : block->GetInstList()) {
        const auto op = inst.GetOp();
        const bool bridge = allocation.IsWidthChainCoalesced(inst.Id()) &&
                            (op == ir::OpCode::BitExtract || op == ir::OpCode::ZeroExtend32To64);
        const bool copy = allocation.IsLow32CopyCoalesced(inst.Id());
        queries.needed |=
                bridge || copy ||
                (op == ir::OpCode::GetHostGPR && allocation.IsHostReadCoalesced(inst.Id()));
        if ((!bridge && !(copy && op == ir::OpCode::BitExtract)) || !inst.ArgAt(0).IsValue())
            continue;
        const auto source = ir::ResolveBitCastSource(inst.GetArg<ir::Value>(0));
        if (!source.Defined() || source.Id() >= allocation.MapCount()) continue;
        const auto location = allocation.GPRLocationAt(
                source, inst.Id(), compile::AllocationView::GPRView::WholeValue);
        if (location.kind != compile::AllocationView::GPR || location.slot >= 31) continue;
        const u32 home = u32{1} << location.slot;
        if (bridge) queries.bridges |= home;
        if (copy) queries.copies |= home;
    }
    return queries;
}

class WidthContractCheck {
public:
    WidthContractCheck(const ir::Block* block,
                       compile::AllocationView allocation,
                       const FeatureSet& features,
                       const GPRSMask& uniforms,
                       const compile::BlockAnalysisIndex& analysis,
                       WidthContractQueries queries)
            : cur_block(block)
            , context(allocation, features, uniforms)
            , allocation(allocation)
            , direct(analysis)
            , terminal_values(block, compile::TerminalValueUses::Aliases::Resolved) {
        const u32 queried_homes = queries.bridges | queries.copies;
        if (!queried_homes) return;
        for (auto& inst : block->GetInstList()) {
            const auto dirty = allocation.DirtyGPR(inst.Id());
            const auto fixed = FixedGPRClobbers(inst, features);
            u32 all = fixed;
            if (inst.GetOp() == ir::OpCode::SetHostGPR) all |= 1u << inst.GetArg<ir::Imm>(1).Get();
            const auto helper = HelperCallContract::Resolve(inst, features);
            for (u32 remaining = queried_homes; remaining; remaining &= remaining - 1) {
                const u16 reg = std::countr_zero(remaining);
                const u32 mask = u32{1} << reg;
                if (!dirty.Get(reg))
                    points.push_back(PointKey(reg, PointKind::Unavailable, inst.Id()));
                if ((queries.bridges & fixed & mask) != 0)
                    points.push_back(PointKey(reg, PointKind::FixedClobber, inst.Id()));
                if ((queries.copies & mask) &&
                    ((all & mask) || (helper && helper->ClobbersGPR(reg))))
                    points.push_back(PointKey(reg, PointKind::Clobber, inst.Id()));
            }
            if (!queries.bridges || !inst.HasValue() || inst.IsBitCastOperation()) continue;
            const auto value = ir::Value{&inst};
            const auto add = [&](u16 reg, u32 transfer, bool before_transfer) {
                if (reg < 31 && (queries.bridges & (u32{1} << reg)))
                    owners.push_back({&inst, LastUse(&inst), 0, transfer, reg, before_transfer});
            };
            if (const auto* preserved = allocation.PreservedGPRDefinitionAt(inst.Id())) {
                if (preserved->transfer) add(preserved->home, preserved->transfer, true);
                if (allocation.ValueType(value) == compile::AllocationView::GPR)
                    add(allocation.ValueGPR(value).id, preserved->transfer, false);
            } else if (allocation.ValueType(value) == compile::AllocationView::GPR) {
                add(allocation.ValueGPR(value).id, 0, false);
            }
        }
        std::sort(points.begin(), points.end());
        std::sort(owners.begin(), owners.end(), [](const Owner& a, const Owner& b) {
            return a.home < b.home || (a.home == b.home && a.definition->Id() < b.definition->Id());
        });
        u16 previous = UINT16_MAX;
        u32 longest = 0;
        for (auto& owner : owners) {
            if (owner.home != previous) longest = 0;
            owner.prefix_end = longest = std::max(longest, owner.end);
            previous = owner.home;
        }
    }
    bool Verify(WidthCertificates* certificates);

private:
    bool VerifyHostRead(const ir::Inst*) const;
    bool ReproveWidthChainBridge(const ir::Inst*) const;
    bool ReproveLow32ViewOwnership(const ir::Inst*, ir::Value) const;
    bool ReproveAdjacentLow32Copy(const ir::Inst*, ir::Value) const;
    bool ReproveLow32Copy(const ir::Inst*) const;
    std::span<const compile::BlockAnalysisIndex::Use> Uses(const ir::Inst* value) const {
        return direct.ResolvedUses(value);
    }
    u32 LastInstructionUse(const ir::Inst* value) const { return direct.LastInstructionUse(value); }
    u32 LastUse(const ir::Inst* value) const {
        auto end = LastInstructionUse(value);
        if (terminal_values.Contains(value) && !cur_block->IsEmptyBlock())
            end = std::max<u32>(end, cur_block->GetInstList().back().Id());
        return end;
    }
    enum class PointKind : u8 { Unavailable, FixedClobber, Clobber };
    static u64 PointKey(u16 home, PointKind kind, u32 position) {
        return ((u64{home} * 3 + static_cast<u8>(kind)) << 32) | position;
    }
    bool HasPoint(u16 home, PointKind kind, u32 begin, u32 end) const {
        const auto first =
                std::lower_bound(points.begin(), points.end(), PointKey(home, kind, begin));
        return first != points.end() && *first <= PointKey(home, kind, end);
    }
    struct Owner {
        const ir::Inst* definition;
        u32 end, prefix_end, transfer;
        u16 home;
        bool before_transfer;
        [[nodiscard]] bool ActiveAt(u32 point) const {
            return before_transfer ? point < transfer : point >= transfer;
        }
    };
    std::span<const Owner> PossibleOwners(u16 reg, u32 begin, u32 end) const {
        const auto first_home = std::lower_bound(
                owners.begin(), owners.end(), reg, [](const Owner& owner, u16 home) {
                    return owner.home < home;
                });
        const auto last_home =
                std::upper_bound(first_home, owners.end(), reg, [](u16 home, const Owner& owner) {
                    return home < owner.home;
                });
        const auto first =
                std::lower_bound(first_home, last_home, begin, [](const Owner& owner, u32 point) {
                    return owner.prefix_end < point;
                });
        const auto last =
                std::upper_bound(first_home, last_home, end, [](u32 id, const Owner& owner) {
                    return id < owner.definition->Id();
                });
        return {std::min(first, last), last};
    }
    const ir::Block* cur_block;
    mutable WidthAllocationView context;
    compile::AllocationView allocation;
    const compile::BlockAnalysisIndex& direct;
    compile::TerminalValueUses terminal_values;
    std::vector<Owner> owners;
    // The full u32 instruction position is retained in each tagged key.
    std::vector<u64> points;
};

bool WidthContractCheck::ReproveWidthChainBridge(const ir::Inst* inst) const {
    if (!inst || !context.IsWidthChainCoalesced(inst->Id())) {
        return false;
    }
    ir::Value source{};
    if (inst->GetOp() == ir::OpCode::BitExtract &&
        ir::GetValueSizeByte(inst->ReturnType()) == sizeof(u32) &&
        inst->GetArg<ir::Imm>(1).Get() == 0 && inst->GetArg<ir::Imm>(2).Get() == 32) {
        source = ir::ResolveBitCastSource(inst->GetArg<ir::Value>(0));
    } else if (inst->GetOp() == ir::OpCode::ZeroExtend32To64 &&
               ir::GetValueSizeByte(inst->GetArg<ir::Value>(0).Type()) == sizeof(u32)) {
        source = ir::ResolveBitCastSource(inst->GetArg<ir::Value>(0));
    } else {
        return false;
    }
    // LONG 的 invariant 输入是 pinned X 的 W view。它只在唯一 U32
    // Add/Xor consumer 上省 direct 快照，不宣称 X[63:32] 已清零。
    bool long_u32_capture = false;
    if (context.GetFeatures().ra_width_chain_long && source.Defined() && source.Def() &&
        source.Def()->GetOp() == ir::OpCode::GetHostGPR &&
        ir::GetValueSizeByte(source.Type()) == sizeof(u32) && inst->GetUses() == 1) {
        u32 consumers = 0;
        for (const auto& use : Uses(inst)) {
            consumers += use.count;
            const auto& scan = *use.consumer;
            long_u32_capture |=
                    (scan.GetOp() == ir::OpCode::Add || scan.GetOp() == ir::OpCode::Xor) &&
                    ir::GetValueSizeByte(scan.ReturnType()) == sizeof(u32);
        }
        long_u32_capture &= consumers == 1;
    }
    if (!source.Defined() || !context.SharesGPR(source, ir::Value{inst})) {
        return false;
    }
    const u32 anchor = context.WidthChainAnchor(inst->Id());
    const u32 source_anchor = context.IsWidthChainCoalesced(source.Id())
                                      ? context.WidthChainAnchor(source.Id())
                                      : source.Id();
    if (anchor != source_anchor) {
        return false;
    }

    auto same_component = [&](ir::Value value) {
        value = ir::ResolveBitCastSource(value);
        if (!value.Defined()) {
            return false;
        }
        return value.Id() == anchor || (context.IsWidthChainCoalesced(value.Id()) &&
                                        context.WidthChainAnchor(value.Id()) == anchor);
    };
    const auto last_use = [&](const ir::Inst* definition) { return LastUse(definition); };

    const u32 target = context.X(source).GetCode();
    const ir::Inst* pinned_consumer = nullptr;
    u32 pinned_uses = 0;
    for (const auto& use : Uses(inst)) {
        pinned_uses += use.count;
        pinned_consumer = use.consumer;
    }
    bool pinned_w_handoff = pinned_uses == 1 && pinned_consumer &&
                            IsPinnedWViewProducer(pinned_consumer->GetOp()) &&
                            ir::GetValueSizeByte(pinned_consumer->ReturnType()) == sizeof(u32) &&
                            last_use(source.Def()) == inst->Id() &&
                            last_use(inst) == pinned_consumer->Id() &&
                            context.SharesGPR(ir::Value{pinned_consumer}, source);
    if (pinned_w_handoff) {
        bool published = false;
        for (auto& scan : cur_block->GetInstList()) {
            if (scan.Id() <= pinned_consumer->Id() || scan.GetOp() != ir::OpCode::SetHostGPR ||
                scan.GetArg<ir::Imm>(1).Get() != target || scan.GetArg<ir::Imm>(2).Get() != 0 ||
                !context.IsHostWriteCoalesced(scan.Id())) {
                continue;
            }
            auto value = ir::ResolveBitCastSource(scan.GetArg<ir::Value>(0));
            if (value.Def() && value.Def()->GetOp() == ir::OpCode::ZeroExtend32To64) {
                value = ir::ResolveBitCastSource(value.Def()->GetArg<ir::Value>(0));
            }
            published |= value.Def() == pinned_consumer;
        }
        pinned_w_handoff = published;
    }
    if (!IsKnownWidthChainWWrite(source, context) && !long_u32_capture && !pinned_w_handoff) {
        return false;
    }

    const u32 end = last_use(inst);
    if (context.HasWidthComponentOwner(anchor) &&
        (!context.WidthComponentOwnerCommitted(anchor) ||
         context.WidthComponentOwnerTarget(anchor) != target ||
         (!pinned_w_handoff && !context.WidthComponentOwnerHighZero(anchor)))) {
        return false;
    }
    const u32 last_id = cur_block->GetInstList().begin() == cur_block->GetInstList().end()
                                ? 0
                                : std::prev(cur_block->GetInstList().end())->Id();
    if (target >= 31 || HasPoint(target, PointKind::Unavailable, inst->Id(), end) ||
        HasPoint(target, PointKind::FixedClobber, inst->Id(), end) ||
        (last_id >= inst->Id() && last_id <= end && target == 11 &&
         ScratchXPoolEnabled(context.GetFeatures())))
        return false;
    for (auto* scan : direct.Between(inst->Id() - (inst->Id() != 0), end + 1)) {
        if (scan->GetOp() == ir::OpCode::SetHostGPR && scan->GetArg<ir::Imm>(1).Get() == target &&
            !same_component(scan->GetArg<ir::Value>(0)))
            return false;
    }
    for (const auto& owner : PossibleOwners(target, inst->Id(), end)) {
        if (!owner.ActiveAt(context.Point())) continue;
        auto& other = *owner.definition;
        if (&other == inst || !other.HasValue() || other.IsBitCastOperation() ||
            same_component(ir::Value{&other}) || !context.SharesGPR(ir::Value{&other}, source)) {
            continue;
        }
        if (other.Id() <= end && last_use(&other) >= inst->Id()) {
            auto inputs = other.GetValues();
            const bool exact_last_use_handoff =
                    other.Id() == end &&
                    std::any_of(inputs.begin(), inputs.end(), [&](ir::Value input) {
                        return ir::ResolveBitCastSource(input).Def() == inst;
                    });
            if (exact_last_use_handoff) {
                continue;
            }
            return false;
        }
    }
    return true;
}

bool WidthContractCheck::ReproveLow32ViewOwnership(const ir::Inst* inst, ir::Value source) const {
    const u32 source_target = context.X(source).GetCode();
    bool has_consumer = false;
    u32 last_use = inst->Id();
    u32 source_last_use = inst->Id();
    source_last_use = std::max<u32>(inst->Id(), LastInstructionUse(source.Def()));
    for (const auto& use : direct.Uses(inst)) {
        auto& scan = *use.consumer;
        has_consumer = true;
        last_use = std::max<u32>(last_use, scan.Id());
        if (context.HasAllocation(ir::Value{&scan}) &&
            context.SharesGPR(ir::Value{inst}, ir::Value{&scan}))
            return false;
        if (!context.HasAllocation(ir::Value{&scan}) && !ir::IsReadOnlyLow32Consumer(scan.GetOp()))
            return false;
    }
    if (!has_consumer || (source_last_use < last_use && source_last_use != inst->Id())) {
        return false;
    }
    const u32 live_begin =
            context.Low32SourceRecolored(source, inst->Id()) ? source.Id() + 1 : inst->Id();
    if (source_target >= 31 || HasPoint(source_target, PointKind::Clobber, live_begin, last_use) ||
        HasPoint(source_target, PointKind::Unavailable, live_begin, last_use))
        return false;
    return true;
}

bool WidthContractCheck::ReproveAdjacentLow32Copy(const ir::Inst* inst, ir::Value source) const {
    if (inst->GetUses() != 1) {
        return false;
    }
    const u32 source_target = context.X(source).GetCode();
    if (source_target >= 31 || !context.DirtyGPR(inst->Id()).Get(source_target)) {
        return false;
    }

    auto& list = cur_block->GetInstList();
    auto wrapper_it = std::next(list.iterator_to(*inst));
    if (wrapper_it == list.end()) {
        return false;
    }
    auto& wrapper = *wrapper_it;
    if (wrapper.GetOp() != ir::OpCode::ZeroExtend32To64 ||
        wrapper.GetArg<ir::Value>(0).Def() != inst) {
        return false;
    }

    if (context.X(ir::Value{&wrapper}).GetCode() == source_target) {
        // Recoloring may transfer a dead parent's register through the view
        // to its adjacent extension. That extension still owes a W write;
        // the original 64-bit value must have no remaining observers.
        if (source.Def()->GetOp() == ir::OpCode::GetHostGPR ||
            context.IsUniform(context.X(source)) || terminal_values.Contains(source.Def())) {
            return false;
        }
        for (const auto& use : Uses(source.Def()))
            if (use.consumer->Id() > inst->Id()) return false;
    }

    return !context.IsWidthChainCoalesced(wrapper.Id()) &&
           context.DirtyGPR(wrapper.Id()).Get(source_target);
}

bool WidthContractCheck::ReproveLow32Copy(const ir::Inst* inst) const {
    if (!inst || !context.IsLow32CopyCoalesced(inst->Id()) ||
        inst->GetOp() != ir::OpCode::BitExtract ||
        ir::GetValueSizeByte(inst->ReturnType()) != sizeof(u32) ||
        inst->GetArg<ir::Imm>(1).Get() != 0 || inst->GetArg<ir::Imm>(2).Get() != 32) {
        return false;
    }
    auto source = ir::ResolveBitCastSource(inst->GetArg<ir::Value>(0));
    if (!source.Defined() || context.Low32CopySource(inst->Id()) != source.Id() ||
        !context.SharesGPR(source, ir::Value{inst})) {
        return false;
    }
    if (context.Low32SourceRecolored(source, inst->Id())) {
        const u32 target = context.X(source).GetCode();
        if (target >= 31 || HasPoint(target, PointKind::Clobber, source.Id() + 1, inst->Id() - 1))
            return false;
    }
    return ReproveLow32ViewOwnership(inst, source) || ReproveAdjacentLow32Copy(inst, source);
}

bool WidthContractCheck::VerifyHostRead(const ir::Inst* inst) const {
    if (inst->GetArg<ir::Imm>(1).Get() != 0 || ir::GetValueSizeByte(inst->ReturnType()) != 4)
        return false;
    const auto target = inst->GetArg<ir::Imm>(0).Get();
    if (target >= 31 || context.X(ir::Value{inst}).GetCode() != target) return false;
    const ir::Inst* publication = nullptr;
    bool blocked = false;
    for (auto* scan : direct.Instructions()) {
        if (scan == inst) break;
        if (scan->GetOp() == ir::OpCode::SetHostGPR && scan->GetArg<ir::Imm>(1).Get() == target) {
            publication = scan;
            blocked = false;
        } else if (publication && IsFixedGPRPublicationObserver(*scan, context.GetFeatures()))
            blocked = true;
    }
    if (!publication || blocked) return false;
    const auto publication_width = ir::GetValueSizeByte(publication->GetArg<ir::Value>(0).Type());
    if (publication->GetArg<ir::Imm>(2).Get() != 0 ||
        (publication_width != sizeof(u32) && publication_width != sizeof(u64)))
        return false;
    auto published = ir::ResolveBitCastSource(publication->GetArg<ir::Value>(0));
    const auto known_w_write = [&](auto&& self, ir::Value value) -> bool {
        value = ir::ResolveBitCastSource(value);
        if (!value.Defined() || value.Def()->GetOp() == ir::OpCode::GetHostGPR) return false;
        auto* def = value.Def();
        if ((def->GetOp() == ir::OpCode::ZeroExtend32To64 &&
             ir::GetValueSizeByte(def->GetArg<ir::Value>(0).Type()) == 4) ||
            (def->GetOp() == ir::OpCode::BitExtract && def->ReturnType() == ir::ValueType::U32 &&
             def->GetArg<ir::Imm>(1).Get() == 0 && def->GetArg<ir::Imm>(2).Get() == 32))
            return self(self, def->GetArg<ir::Value>(0));
        return ir::GetValueSizeByte(def->ReturnType()) == 4;
    };
    bool high_zero = published.Defined() &&
                     ((published.Def()->GetOp() == ir::OpCode::ZeroExtend32To64 &&
                       ir::GetValueSizeByte(published.Def()->GetArg<ir::Value>(0).Type()) == 4) ||
                      (ir::GetValueSizeByte(published.Type()) == 4 &&
                       known_w_write(known_w_write, published)));
    if (published.Defined() && context.IsWidthChainCoalesced(published.Id())) {
        const auto anchor = context.WidthChainAnchor(published.Id());
        if (context.HasWidthComponentOwner(anchor)) {
            if (!context.WidthComponentOwnerCommitted(anchor) ||
                !context.IsHostWriteCoalesced(publication->Id()) ||
                context.WidthComponentOwnerTarget(anchor) != target)
                return false;
            high_zero = context.WidthComponentOwnerHighZero(anchor);
        }
    }
    if (!high_zero) return false;
    for (auto* scan : direct.Between(inst->Id(), LastUse(inst) + 1))
        if (scan->GetOp() == ir::OpCode::SetHostGPR && scan->GetArg<ir::Imm>(1).Get() == target)
            return false;
    return true;
}

bool WidthContractCheck::Verify(WidthCertificates* certificates) {
    for (auto& inst : cur_block->GetInstList()) {
        context.At(inst.Id());
        if (inst.GetOp() == ir::OpCode::GetHostGPR && allocation.IsHostReadCoalesced(inst.Id())) {
            const bool valid = VerifyHostRead(&inst);
            if (certificates) ASSERT_MSG(valid, "invalid final GPR read elision at IR {}", inst.Id());
            if (!valid) return false;
            if (certificates) certificates->reads.push_back(inst.Id());
        }
        if (context.IsWidthChainCoalesced(inst.Id()) &&
            (inst.GetOp() == ir::OpCode::BitExtract ||
             inst.GetOp() == ir::OpCode::ZeroExtend32To64)) {
            const bool valid = ReproveWidthChainBridge(&inst);
            if (certificates) ASSERT_MSG(valid, "invalid final width-chain bridge at IR {}", inst.Id());
            if (!valid) return false;
            if (certificates) certificates->width.push_back(inst.Id());
        }
        if (context.IsLow32CopyCoalesced(inst.Id())) {
            const bool valid = ReproveLow32Copy(&inst);
            if (certificates) ASSERT_MSG(valid, "invalid final low32 copy at IR {}", inst.Id());
            if (!valid) return false;
            if (certificates) certificates->low32.push_back(inst.Id());
        }
    }
    return true;
}
}  // namespace
WidthCertificates PrepareWidthCertificates(const ir::Block* block,
                                           compile::AllocationView allocation,
                                           const FeatureSet& features,
                                           const GPRSMask& uniforms,
                                           compile::BlockAnalysisIndex& analysis) {
    const auto queries = CollectWidthContractQueries(block, allocation);
    if (!queries.needed) return {};
    analysis.PrepareResolvedUses();
    WidthCertificates proofs;
    const bool valid =
            WidthContractCheck{block, allocation, features, uniforms, analysis, queries}.Verify(
                    &proofs);
    ASSERT(valid);
    return proofs;
}
bool CheckWidthContracts(const ir::Block* block,
                         compile::AllocationView allocation,
                         const FeatureSet& features,
                         const GPRSMask& uniforms,
                         const compile::BlockAnalysisIndex& analysis) {
    const auto queries = CollectWidthContractQueries(block, allocation);
    if (!queries.needed) return true;
    return WidthContractCheck{block, allocation, features, uniforms, analysis, queries}.Verify(nullptr);
}
}  // namespace swift::runtime::backend::arm64
