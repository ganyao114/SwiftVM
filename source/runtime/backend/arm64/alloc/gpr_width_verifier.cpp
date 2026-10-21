#include "gpr_width_verifier.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/arm64/alloc/fixed_gpr_constraints.h"
#include "runtime/ir/low32_copy.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"
#include "runtime/common/variant_util.h"
#include <algorithm>
#include <array>

namespace swift::runtime::backend::arm64 {
namespace {
struct PhysicalGPR { u16 slot; u16 GetCode() const { return slot; } };
class WidthAllocationView {
public:
    WidthAllocationView(const RegAlloc& a, const FeatureSet& f, const GPRSMask& u)
            : allocation(a), features(f), uniforms(u) {}
    void At(u32 id) { point = id; }
    u32 Point() const { return point; }
    const FeatureSet& GetFeatures() const { return features; }
    bool HasAllocation(ir::Value v) const { return allocation.ValueType(v) != RegAlloc::NONE; }
    PhysicalGPR X(ir::Value v) const {
        const auto loc = allocation.GPRLocationAt(v, point, RegAlloc::GPRView::WholeValue);
        return {static_cast<u16>(loc.kind == RegAlloc::GPR ? loc.slot : UINT16_MAX)};
    }
    bool SharesGPR(ir::Value a, ir::Value b) const {
        if (!a.Defined() || !b.Defined()) return false;
        if (allocation.PreservedGPRDefinitionAt(a.Id()) || allocation.PreservedGPRDefinitionAt(b.Id())) {
            const auto x = X(a), y = X(b);
            return x.slot < 31 && x.slot == y.slot;
        }
        return allocation.ValueType(a) == RegAlloc::GPR && allocation.ValueType(b) == RegAlloc::GPR &&
               allocation.ValueGPR(a).id == allocation.ValueGPR(b).id;
    }
    bool IsUniform(PhysicalGPR reg) const { return reg.slot < 31 && uniforms.Get(reg.slot); }
    GPRSMask DirtyGPR(u32 id) const { return allocation.DirtyGPR(id); }
    bool IsWidthChainCoalesced(u32 id) const { return allocation.IsWidthChainCoalesced(id); }
    u32 WidthChainAnchor(u32 id) const { return allocation.WidthChainAnchor(id); }
    bool IsLow32CopyCoalesced(u32 id) const { return allocation.IsLow32CopyCoalesced(id); }
    u32 Low32CopySource(u32 id) const { return allocation.Low32CopySource(id); }
    bool Low32SourceRecolored(ir::Value source, u32 bridge) const { return allocation.AllocationId(source) == bridge; }
    bool HasWidthComponentOwner(u32 id) const { return allocation.HasWidthComponentOwner(id); }
    bool WidthComponentOwnerCommitted(u32 id) const { return allocation.WidthComponentOwnerCommitted(id); }
    u16 WidthComponentOwnerTarget(u32 id) const { return allocation.WidthComponentOwnerTarget(id); }
    bool WidthComponentOwnerHighZero(u32 id) const { return allocation.WidthComponentOwnerHighZero(id); }
    bool IsHostWriteCoalesced(u32 id) const { return allocation.IsHostWriteCoalesced(id); }
private:
    const RegAlloc& allocation;
    const FeatureSet& features;
    const GPRSMask& uniforms;
    u32 point{};
};

ir::Value ResolveWidthChainBitCast(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<ir::Value>(0);
    }
    return value;
}

bool IsKnownWidthChainWWrite(ir::Value value, const WidthAllocationView& context) {
    value = ResolveWidthChainBitCast(value);
    if (!value.Defined()) {
        return false;
    }
    auto* def = value.Def();
    if (def->GetOp() == ir::OpCode::LoadImm) {
        // See HasKnownWWrite: a 32-bit-fitting constant is a W write.
        return (def->GetArg<ir::Imm>(0).Get() >> 32) == 0;
    }
    if (def->GetOp() == ir::OpCode::GetHostGPR) {
        return (context.GetFeatures().ra_width_chain ||
                context.GetFeatures().ra_width_chain_long) &&
               context.IsWidthChainCoalesced(value.Id()) &&
               context.WidthChainAnchor(value.Id()) == value.Id() &&
               ir::GetValueSizeByte(value.Type()) == sizeof(u32);
    }
    if (def->GetOp() == ir::OpCode::BitExtract &&
        ir::GetValueSizeByte(def->ReturnType()) == sizeof(u32) &&
        def->GetArg<ir::Imm>(1).Get() == 0 &&
        def->GetArg<ir::Imm>(2).Get() == 32) {
        return IsKnownWidthChainWWrite(def->GetArg<ir::Value>(0), context);
    }
    if (def->GetOp() == ir::OpCode::ZeroExtend32To64 &&
        ir::GetValueSizeByte(def->GetArg<ir::Value>(0).Type()) == sizeof(u32)) {
        return IsKnownWidthChainWWrite(def->GetArg<ir::Value>(0), context);
    }
    // See HasKnownWWrite: narrow unsigned loads materialize as W writes.
    if ((def->GetOp() == ir::OpCode::LoadMemory ||
         def->GetOp() == ir::OpCode::LoadMemoryTSO) &&
        (def->ReturnType() == ir::ValueType::U8 ||
         def->ReturnType() == ir::ValueType::U16)) {
        return true;
    }
    return ir::GetValueSizeByte(def->ReturnType()) == sizeof(u32);
}

bool TerminalUsesWidthChainValue(const ir::Terminal& terminal_value,
                                 ir::Inst* definition) {
    return VisitVariant<bool>(terminal_value, [&](const auto& edge) {
        using T = std::decay_t<decltype(edge)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            return ResolveWidthChainBitCast(edge.cond).Def() == definition ||
                   TerminalUsesWidthChainValue(edge.then_, definition) ||
                   TerminalUsesWidthChainValue(edge.else_, definition);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            if (ResolveWidthChainBitCast(edge.value).Def() == definition) {
                return true;
            }
            return std::any_of(edge.cases.begin(), edge.cases.end(),
                               [&](const auto& item) {
                                   return TerminalUsesWidthChainValue(item.then, definition);
                               });
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            return TerminalUsesWidthChainValue(edge.then_, definition) ||
                   TerminalUsesWidthChainValue(edge.else_, definition);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            return TerminalUsesWidthChainValue(edge.else_, definition);
        }
        return false;
    });
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

class WidthElisionVerifier {
public:
    WidthElisionVerifier(ir::Block* block, const RegAlloc& allocation, const FeatureSet& features,
                         const GPRSMask& uniforms, const BlockAnalysisIndex* analysis) : cur_block(block), context(allocation, features, uniforms),
                         allocation(allocation) {
        if (analysis) direct = analysis;
        else { owned_direct.Build(block); direct = &owned_direct; }
        const auto add_terminal = [&](ir::Value value) {
            value = ResolveWidthChainBitCast(value);
            if (value.Defined()) terminal_values.insert(value.Def());
        };
        const auto walk_terminal = [&](auto&& self, const ir::Terminal& tree) -> void {
            VisitVariant<void>(tree, [&](const auto& node) {
                using T = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<T, ir::terminal::If>) {
                    add_terminal(node.cond); self(self, node.then_); self(self, node.else_);
                } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                    add_terminal(node.value); for (const auto& item : node.cases) self(self, item.then);
                } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                    self(self, node.then_); self(self, node.else_);
                } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) self(self, node.else_);
            });
        };
        walk_terminal(walk_terminal, block->GetTerminal());
        for (auto& inst : block->GetInstList()) {
            const auto dirty = allocation.DirtyGPR(inst.Id());
            const auto fixed = FixedGPRClobbers(inst, features);
            u32 all = fixed;
            if (inst.GetOp() == ir::OpCode::SetHostGPR) all |= 1u << inst.GetArg<ir::Imm>(1).Get();
            const auto helper = HelperCallContract::Resolve(inst, features);
            for (u32 reg = 0; reg < 31; ++reg) {
                if (!dirty.Get(reg)) unavailable[reg].push_back(inst.Id());
                if (fixed & (1u << reg)) fixed_clobbers[reg].push_back(inst.Id());
                if ((all & (1u << reg)) || (helper && helper->ClobbersGPR(reg)))
                    clobbers[reg].push_back(inst.Id());
            }
        }
        for (auto& inst : block->GetInstList()) {
            if (!inst.HasValue() || inst.IsBitCastOperation()) continue;
            const auto value = ir::Value{&inst};
            const auto add = [&](u16 reg, u32 from, u32 to) {
                if (reg < 31) owners[reg].push_back({&inst, inst.Id(), LastUse(&inst), from, to});
            };
            if (const auto* preserved = allocation.PreservedGPRDefinitionAt(inst.Id())) {
                if (preserved->transfer) add(preserved->home, 0, preserved->transfer - 1);
                if (allocation.ValueType(value) == RegAlloc::GPR)
                    add(allocation.ValueGPR(value).id, preserved->transfer, UINT32_MAX);
            } else if (allocation.ValueType(value) == RegAlloc::GPR) {
                add(allocation.ValueGPR(value).id, 0, UINT32_MAX);
            }
        }
        for (u32 reg = 0; reg < 31; ++reg) {
            std::sort(owners[reg].begin(), owners[reg].end(), [](const auto& a, const auto& b) { return a.birth < b.birth; });
            u32 longest = 0;
            for (const auto& owner : owners[reg]) {
                longest = std::max(longest, owner.end);
                owner_prefix[reg].push_back(longest);
            }
        }
    }
    GPRWidthCertificates Verify();
private:
    bool VerifyHostRead(ir::Inst*) const;
    bool ReproveWidthChainBridge(ir::Inst*) const;
    bool ReproveLow32ViewOwnership(ir::Inst*, ir::Value) const;
    bool ReproveAdjacentLow32Copy(ir::Inst*, ir::Value) const;
    bool ReproveLow32Copy(ir::Inst*) const;
    std::span<const BlockAnalysisIndex::Use> Uses(ir::Inst* value) const { return direct->ResolvedUses(value); }
    u32 LastInstructionUse(ir::Inst* value) const {
        return direct->LastInstructionUse(value);
    }
    u32 LastUse(ir::Inst* value) const {
        auto end = LastInstructionUse(value);
        if (terminal_values.contains(value) && !cur_block->IsEmptyBlock())
            end = std::max<u32>(end, cur_block->GetInstList().back().Id());
        return end;
    }
    static bool HasPoint(const std::vector<u32>& ids, u32 begin, u32 end) {
        const auto first = std::lower_bound(ids.begin(), ids.end(), begin);
        return first != ids.end() && *first <= end;
    }
    struct Owner { ir::Inst* definition; u32 birth, end, view_begin, view_end; };
    std::span<const Owner> PossibleOwners(u16 reg, u32 begin, u32 end) const {
        const auto& entries = owners[reg];
        const size_t first = std::lower_bound(owner_prefix[reg].begin(), owner_prefix[reg].end(), begin) -
                             owner_prefix[reg].begin();
        const auto last = std::upper_bound(entries.begin(), entries.end(), end,
                [](u32 id, const Owner& owner) { return id < owner.birth; });
        return {entries.begin() + std::min<size_t>(first, last - entries.begin()), last};
    }
    ir::Block* cur_block;
    mutable WidthAllocationView context;
    const RegAlloc& allocation;
    BlockAnalysisIndex owned_direct;
    const BlockAnalysisIndex* direct{};
    std::unordered_set<ir::Inst*> terminal_values;
    std::array<std::vector<Owner>, 32> owners;
    std::array<std::vector<u32>, 32> owner_prefix;
    std::array<std::vector<u32>, 32> unavailable, fixed_clobbers, clobbers;
};

bool WidthElisionVerifier::ReproveWidthChainBridge(ir::Inst* inst) const {
    if (!inst || !context.IsWidthChainCoalesced(inst->Id())) {
        return false;
    }
    ir::Value source{};
    if (inst->GetOp() == ir::OpCode::BitExtract &&
        ir::GetValueSizeByte(inst->ReturnType()) == sizeof(u32) &&
        inst->GetArg<ir::Imm>(1).Get() == 0 &&
        inst->GetArg<ir::Imm>(2).Get() == 32) {
        source = ResolveWidthChainBitCast(inst->GetArg<ir::Value>(0));
    } else if (inst->GetOp() == ir::OpCode::ZeroExtend32To64 &&
               ir::GetValueSizeByte(inst->GetArg<ir::Value>(0).Type()) == sizeof(u32)) {
        source = ResolveWidthChainBitCast(inst->GetArg<ir::Value>(0));
    } else {
        return false;
    }
    // LONG 的 invariant 输入是 pinned X 的 W view。它只在唯一 U32
    // Add/Xor consumer 上省 direct 快照，不宣称 X[63:32] 已清零。
    bool long_u32_capture = false;
    if (context.GetFeatures().ra_width_chain_long && source.Defined() &&
        source.Def() && source.Def()->GetOp() == ir::OpCode::GetHostGPR &&
        ir::GetValueSizeByte(source.Type()) == sizeof(u32) && inst->GetUses() == 1) {
        u32 consumers = 0;
        for (const auto& use : Uses(inst)) {
            consumers += use.count;
            const auto& scan = *use.consumer;
            long_u32_capture |= (scan.GetOp() == ir::OpCode::Add || scan.GetOp() == ir::OpCode::Xor) &&
                                ir::GetValueSizeByte(scan.ReturnType()) == sizeof(u32);
        }
        long_u32_capture &= consumers == 1;
    }
    if (!source.Defined() ||
        !context.SharesGPR(source, ir::Value{inst})) {
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
        value = ResolveWidthChainBitCast(value);
        if (!value.Defined()) {
            return false;
        }
        return value.Id() == anchor ||
               (context.IsWidthChainCoalesced(value.Id()) &&
                context.WidthChainAnchor(value.Id()) == anchor);
    };
    const auto last_use = [&](ir::Inst* definition) { return LastUse(definition); };

    const u32 target = context.X(source).GetCode();
    ir::Inst* pinned_consumer = nullptr;
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
            auto value = ResolveWidthChainBitCast(scan.GetArg<ir::Value>(0));
            if (value.Def() && value.Def()->GetOp() == ir::OpCode::ZeroExtend32To64) {
                value = ResolveWidthChainBitCast(value.Def()->GetArg<ir::Value>(0));
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
    const u32 last_id = cur_block->GetInstList().begin() ==
                                cur_block->GetInstList().end()
            ? 0
            : std::prev(cur_block->GetInstList().end())->Id();
    if (target >= 31 || HasPoint(unavailable[target], inst->Id(), end) ||
        HasPoint(fixed_clobbers[target], inst->Id(), end) ||
        (last_id >= inst->Id() && last_id <= end && target == 11 && ScratchXPoolEnabled(context.GetFeatures())))
        return false;
    for (auto* scan : direct->Between(inst->Id() - (inst->Id() != 0), end + 1)) {
        if (scan->GetOp() == ir::OpCode::SetHostGPR && scan->GetArg<ir::Imm>(1).Get() == target &&
            !same_component(scan->GetArg<ir::Value>(0))) return false;
    }
    for (const auto& owner : PossibleOwners(target, inst->Id(), end)) {
        if (context.Point() < owner.view_begin || context.Point() > owner.view_end) continue;
        auto& other = *owner.definition;
        if (&other == inst || !other.HasValue() || other.IsBitCastOperation() ||
            same_component(ir::Value{&other}) ||
            !context.SharesGPR(ir::Value{&other}, source)) {
            continue;
        }
        if (other.Id() <= end && last_use(&other) >= inst->Id()) {
            auto inputs = other.GetValues();
            const bool exact_last_use_handoff =
                    other.Id() == end && std::any_of(
                            inputs.begin(), inputs.end(),
                            [&](ir::Value input) {
                                return ResolveWidthChainBitCast(input).Def() == inst;
                            });
            if (exact_last_use_handoff) {
                continue;
            }
            return false;
        }
    }
    return true;
}

bool WidthElisionVerifier::ReproveLow32ViewOwnership(ir::Inst* inst,
                                              ir::Value source) const {
    const u32 source_target = context.X(source).GetCode();
    bool has_consumer = false;
    u32 last_use = inst->Id();
    u32 source_last_use = inst->Id();
    source_last_use = std::max<u32>(inst->Id(), LastInstructionUse(source.Def()));
    for (const auto& use : direct->Uses(inst)) {
        auto& scan = *use.consumer;
        has_consumer = true;
        last_use = std::max<u32>(last_use, scan.Id());
        if (context.HasAllocation(ir::Value{&scan}) && context.SharesGPR(ir::Value{inst}, ir::Value{&scan})) return false;
        if (!context.HasAllocation(ir::Value{&scan}) && !ir::IsReadOnlyLow32Consumer(scan.GetOp())) return false;
    }
    if (!has_consumer ||
        (source_last_use < last_use && source_last_use != inst->Id())) {
        return false;
    }
    const u32 live_begin = context.Low32SourceRecolored(source, inst->Id())
            ? source.Id() + 1 : inst->Id();
    if (source_target >= 31 || HasPoint(clobbers[source_target], live_begin, last_use) ||
        HasPoint(unavailable[source_target], live_begin, last_use)) return false;
    return true;
}

bool WidthElisionVerifier::ReproveAdjacentLow32Copy(ir::Inst* inst,
                                              ir::Value source) const {
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
            context.IsUniform(context.X(source)) ||
            terminal_values.contains(source.Def())) {
            return false;
        }
        for (const auto& use : Uses(source.Def()))
            if (use.consumer->Id() > inst->Id()) return false;
    }

    return !context.IsWidthChainCoalesced(wrapper.Id()) &&
           context.DirtyGPR(wrapper.Id()).Get(source_target);
}

bool WidthElisionVerifier::ReproveLow32Copy(ir::Inst* inst) const {
    if (!inst || !context.IsLow32CopyCoalesced(inst->Id()) ||
        inst->GetOp() != ir::OpCode::BitExtract ||
        ir::GetValueSizeByte(inst->ReturnType()) != sizeof(u32) ||
        inst->GetArg<ir::Imm>(1).Get() != 0 ||
        inst->GetArg<ir::Imm>(2).Get() != 32) {
        return false;
    }
    auto source = ResolveWidthChainBitCast(inst->GetArg<ir::Value>(0));
    if (!source.Defined() || context.Low32CopySource(inst->Id()) != source.Id() ||
        !context.SharesGPR(source, ir::Value{inst})) {
        return false;
    }
    if (context.Low32SourceRecolored(source, inst->Id())) {
        const u32 target = context.X(source).GetCode();
        if (target >= 31 || HasPoint(clobbers[target], source.Id() + 1, inst->Id() - 1)) return false;
    }
    return ReproveLow32ViewOwnership(inst, source) ||
           ReproveAdjacentLow32Copy(inst, source);
}

bool WidthElisionVerifier::VerifyHostRead(ir::Inst* inst) const {
    if (inst->GetArg<ir::Imm>(1).Get() != 0 ||
        ir::GetValueSizeByte(inst->ReturnType()) != 4) return false;
    const auto target = inst->GetArg<ir::Imm>(0).Get();
    if (target >= 31 || context.X(ir::Value{inst}).GetCode() != target) return false;
    ir::Inst* publication = nullptr;
    bool blocked = false;
    for (auto* scan : direct->Instructions()) {
        if (scan == inst) break;
        if (scan->GetOp() == ir::OpCode::SetHostGPR && scan->GetArg<ir::Imm>(1).Get() == target) {
            publication = scan;
            blocked = false;
        } else if (publication && IsFixedGPRPublicationObserver(*scan, context.GetFeatures())) blocked = true;
    }
    if (!publication || blocked) return false;
    auto published = ResolveWidthChainBitCast(publication->GetArg<ir::Value>(0));
    const auto known_w_write = [&](auto&& self, ir::Value value) -> bool {
        value = ResolveWidthChainBitCast(value);
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
             (ir::GetValueSizeByte(published.Type()) == 4 && known_w_write(known_w_write, published)));
    if (published.Defined() && context.IsWidthChainCoalesced(published.Id())) {
        const auto anchor = context.WidthChainAnchor(published.Id());
        if (context.HasWidthComponentOwner(anchor)) {
            if (!context.WidthComponentOwnerCommitted(anchor) ||
                !context.IsHostWriteCoalesced(publication->Id()) ||
                context.WidthComponentOwnerTarget(anchor) != target) return false;
            high_zero = context.WidthComponentOwnerHighZero(anchor);
        }
    }
    if (!high_zero) return false;
    for (auto* scan : direct->Between(inst->Id(), LastUse(inst) + 1))
        if (scan->GetOp() == ir::OpCode::SetHostGPR && scan->GetArg<ir::Imm>(1).Get() == target) return false;
    return true;
}

GPRWidthCertificates WidthElisionVerifier::Verify() {
    GPRWidthCertificates proofs;
    for (auto& inst : cur_block->GetInstList()) {
        context.At(inst.Id());
        if (inst.GetOp() == ir::OpCode::GetHostGPR && allocation.IsHostReadCoalesced(inst.Id())) {
            ASSERT_MSG(VerifyHostRead(&inst), "invalid final GPR read elision at IR {}", inst.Id());
            proofs.reads.push_back(inst.Id());
        }
        if (context.IsWidthChainCoalesced(inst.Id()) &&
            (inst.GetOp() == ir::OpCode::BitExtract || inst.GetOp() == ir::OpCode::ZeroExtend32To64)) {
            ASSERT_MSG(ReproveWidthChainBridge(&inst), "invalid final width-chain bridge at IR {}", inst.Id());
            proofs.width.push_back(inst.Id());
        }
        if (context.IsLow32CopyCoalesced(inst.Id())) {
            ASSERT_MSG(ReproveLow32Copy(&inst), "invalid final low32 copy at IR {}", inst.Id());
            proofs.low32.push_back(inst.Id());
        }
    }
    return proofs;
}
}
GPRWidthCertificates VerifyGPRWidthElisions(ir::Block* block, const RegAlloc& allocation,
                                           const FeatureSet& features, const GPRSMask& uniforms,
                                           const BlockAnalysisIndex* analysis) {
    const bool needed = std::any_of(block->GetInstList().begin(), block->GetInstList().end(), [&](const ir::Inst& inst) {
        return (inst.GetOp() == ir::OpCode::GetHostGPR && allocation.IsHostReadCoalesced(inst.Id())) ||
               allocation.IsLow32CopyCoalesced(inst.Id()) ||
               (allocation.IsWidthChainCoalesced(inst.Id()) &&
                (inst.GetOp() == ir::OpCode::BitExtract || inst.GetOp() == ir::OpCode::ZeroExtend32To64));
    });
    if (!needed) return {};
    return WidthElisionVerifier{block, allocation, features, uniforms, analysis}.Verify();
}
}
