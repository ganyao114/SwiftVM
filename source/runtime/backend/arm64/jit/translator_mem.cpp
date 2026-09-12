#include "base/logging.h"
#include "translator.h"
#include "runtime/backend/gpr_coalescing_contract.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/arm64/fpcr_mode.h"
#include "runtime/common/svm_config.h"

namespace swift::runtime::backend::arm64 {

namespace {

ir::Value ResolveHostCoalesceBitCast(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<ir::Value>(0);
    }
    return value;
}

bool IsKnownHostWWrite(ir::Value value);

bool IsHostCoalesceProducer(ir::OpCode op, bool width_chain) {
    return IsGPRPublicationProducer(op, width_chain);
}

bool IsWidthChainHostWWrite(ir::Value value, u32 target,
                            const FeatureSet& features) {
    value = ResolveHostCoalesceBitCast(value);
    if (!value.Defined()) {
        return false;
    }
    auto* def = value.Def();
    if (def->GetOp() == ir::OpCode::GetHostGPR) {
        return features.ra_width_chain &&
               ir::GetValueSizeByte(def->ReturnType()) == sizeof(u32) &&
               def->GetArg<ir::Imm>(1).Get() == 0 &&
               def->GetArg<ir::Imm>(0).Get() != target;
    }
    return IsKnownHostWWrite(value);
}

bool IsHostScalarFPRBinaryProducer(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::VecFAddScalar32:
        case O::VecFSubScalar32:
        case O::VecFMulScalar32:
        case O::VecFDivScalar32:
        case O::VecFAddScalar64:
        case O::VecFSubScalar64:
        case O::VecFMulScalar64:
        case O::VecFDivScalar64:
            return true;
        default:
            return false;
    }
}

bool IsHostFPRCoalesceProducer(ir::OpCode op, bool scalar_insert,
                              bool scalar_tie) {
    using O = ir::OpCode;
    switch (op) {
        case O::LoadUniform:
        case O::LoadMemory:
        case O::VecAnd:
        case O::VecOr:
        case O::VecXor:
        case O::VecAdd:
        case O::VecSub:
        case O::VecCmpEq:
        case O::VecCmpGt:
        case O::VecMul:
        case O::VecShuffle32Indexed:
        case O::VecExtractBytes:
        case O::VecZip:
        case O::VecFAdd:
        case O::VecFSub:
        case O::VecFMul:
        case O::VecFDiv:
            return true;
        default:
            return IsHostScalarFPRBinaryProducer(op) &&
                   (!scalar_insert || scalar_tie);
    }
}

bool IsHostScalarUnaryProducer(const ir::Inst& inst,
                               bool scalar_tie) {
    if (!scalar_tie || inst.GetOp() != ir::OpCode::VecFUnary ||
        inst.GetArg<ir::Imm>(3).Get() != 0 ||
        inst.GetArg<ir::Imm>(4).Get() == 0) {
        return false;
    }
    const u32 lane_bits = inst.GetArg<ir::Imm>(2).Get();
    return lane_bits == 32 || lane_bits == 64;
}

bool IsHostCoalesceObserver(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::LoadMemory:
        case O::StoreMemory:
        case O::LoadMemoryTSO:
        case O::StoreMemoryTSO:
        case O::MemoryCopy:
        case O::MemoryCopyTSO:
        case O::CompareAndSwap:
        case O::CompareAndSwap128:
        case O::CheckMemoryAlignment:
        case O::AtomicExchange:
        case O::AtomicFetchAdd:
        case O::AtomicRMW:
        case O::CallLambda:
        case O::CallLocation:
        case O::CallDynamic:
        case O::X87Op:
        case O::Sse42Str:
        case O::GetUniformAddress:
        case O::UniformBarrier:
            return true;
        default:
            return false;
    }
}

bool IsKnownHostWWrite(ir::Value value) {
    value = ResolveHostCoalesceBitCast(value);
    if (!value.Defined()) {
        return false;
    }
    auto* def = value.Def();
    if (def->GetOp() == ir::OpCode::GetHostGPR) {
        return false;
    }
    if (def->GetOp() == ir::OpCode::BitExtract &&
        ir::GetValueSizeByte(def->ReturnType()) == sizeof(u32) &&
        def->GetArg<ir::Imm>(1).Get() == 0 &&
        def->GetArg<ir::Imm>(2).Get() == 32) {
        return IsKnownHostWWrite(def->GetArg<ir::Value>(0));
    }
    if (def->GetOp() == ir::OpCode::ZeroExtend32To64 &&
        ir::GetValueSizeByte(def->GetArg<ir::Value>(0).Type()) == sizeof(u32)) {
        return IsKnownHostWWrite(def->GetArg<ir::Value>(0));
    }
    return ir::GetValueSizeByte(def->ReturnType()) == sizeof(u32);
}

bool TerminalUsesHostCoalesceValue(const ir::Terminal& terminal_value,
                                   ir::Inst* definition) {
    return VisitVariant<bool>(terminal_value, [&](const auto& edge) {
        using T = std::decay_t<decltype(edge)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            return ResolveHostCoalesceBitCast(edge.cond).Def() == definition ||
                   TerminalUsesHostCoalesceValue(edge.then_, definition) ||
                   TerminalUsesHostCoalesceValue(edge.else_, definition);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            if (ResolveHostCoalesceBitCast(edge.value).Def() == definition) {
                return true;
            }
            return std::any_of(edge.cases.begin(), edge.cases.end(),
                               [&](const auto& item) {
                                   return TerminalUsesHostCoalesceValue(item.then, definition);
                               });
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            return TerminalUsesHostCoalesceValue(edge.then_, definition) ||
                   TerminalUsesHostCoalesceValue(edge.else_, definition);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            return TerminalUsesHostCoalesceValue(edge.else_, definition);
        }
        return false;
    });
}

}  // namespace

bool JitTranslator::ReproveCoalescedHostWrite(ir::Inst* inst) const {
    if (!inst || inst->GetOp() != ir::OpCode::SetHostGPR ||
        inst->GetArg<ir::Imm>(2).Get() != 0) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    auto stored = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(0));
    auto* wrapper = stored.Def();
    const bool width_wrapper = stored.Defined() &&
                               context.IsWidthChainCoalesced(stored.Id());
    const bool live_publish =
            context.GetFeatures().ra_coalesce_live && !width_wrapper;
    if (!wrapper || (!width_wrapper && !live_publish && wrapper->GetUses() != 1)) {
        return false;
    }
    auto produced = stored;
    bool zero_extend_chain = false;
    bool low32_copy = false;
    if (wrapper->GetOp() == ir::OpCode::ZeroExtend32To64) {
        produced = ResolveHostCoalesceBitCast(wrapper->GetArg<ir::Value>(0));
        zero_extend_chain = true;
        if (produced.Def() &&
            context.IsLow32CopyCoalesced(produced.Id()) &&
            produced.Def()->GetOp() == ir::OpCode::BitExtract &&
            produced.Def()->GetArg<ir::Imm>(1).Get() == 0 &&
            produced.Def()->GetArg<ir::Imm>(2).Get() == 32) {
            auto source = ResolveHostCoalesceBitCast(
                    produced.Def()->GetArg<ir::Value>(0));
            low32_copy = source.Defined() &&
                    context.Low32CopySource(produced.Id()) == source.Id() &&
                    context.SharesGPR(source, produced);
        }
        const bool width_root = produced.Defined() &&
                                context.IsWidthChainCoalesced(produced.Id()) &&
                                context.WidthChainAnchor(produced.Id()) == produced.Id();
        if (!produced.Def() ||
            (!width_root && !live_publish && produced.Def()->GetUses() != 1) ||
            (!low32_copy &&
             !IsWidthChainHostWWrite(produced, target, context.GetFeatures()))) {
            return false;
        }
    }
    auto* producer = produced.Def();
    if (!producer ||
        !IsHostCoalesceProducer(producer->GetOp(),
                                context.GetFeatures().ra_width_chain) ||
        (!low32_copy && context.X(produced).GetCode() != target) ||
        (zero_extend_chain && context.X(stored).GetCode() != target)) {
        return false;
    }
    const u32 width = ir::GetValueSizeByte(produced.Type());
    if ((width != sizeof(u32) && width != sizeof(u64)) ||
        (width == sizeof(u32) &&
         !low32_copy &&
         !IsWidthChainHostWWrite(produced, target, context.GetFeatures()))) {
        return false;
    }
    if (width_wrapper) {
        const u32 anchor = context.WidthChainAnchor(stored.Id());
        if (context.HasWidthComponentOwner(anchor)) {
            const bool actual_high_zero = zero_extend_chain ||
                    ir::GetValueSizeByte(produced.Type()) == sizeof(u32);
            if (!context.WidthComponentOwnerCommitted(anchor) ||
                context.WidthComponentOwnerTarget(anchor) != target ||
                context.WidthComponentOwnerHighZero(anchor) != actual_high_zero) {
                return false;
            }
        }
    }
    ir::Inst* component_source = nullptr;
    if (producer->GetOp() == ir::OpCode::SignExtend) {
        auto source = ResolveHostCoalesceBitCast(producer->GetArg<ir::Value>(0));
        auto* source_def = source.Def();
        if (source_def && context.IsWidthChainCoalesced(source.Id())) {
            if (source_def->GetOp() != ir::OpCode::LoadMemory ||
                ir::GetValueSizeByte(source.Type()) > sizeof(u16) ||
                source_def->GetUses() != 1 ||
                context.WidthChainAnchor(source.Id()) !=
                        context.WidthChainAnchor(produced.Id()) ||
                !context.SharesGPR(source, produced)) {
                return false;
            }
            component_source = source_def;
        }
    }
    const u32 proof_start = component_source ? component_source->Id()
                                             : producer->Id();

    auto last_use = [&](ir::Inst* definition) {
        u32 end = definition->Id();
        for (auto& scan : cur_block->GetInstList()) {
            for (auto use : scan.GetValues()) {
                if (ResolveHostCoalesceBitCast(use).Def() == definition) {
                    end = std::max<u32>(end, scan.Id());
                }
            }
        }
        if (TerminalUsesHostCoalesceValue(cur_block->GetTerminal(), definition) &&
            cur_block->GetInstList().begin() != cur_block->GetInstList().end()) {
            end = std::max<u32>(end, std::prev(cur_block->GetInstList().end())->Id());
        }
        return end;
    };

    for (auto input : producer->GetValues()) {
        auto root = ResolveHostCoalesceBitCast(input);
        if (low32_copy && root.Defined() &&
            context.Low32CopySource(produced.Id()) == root.Id()) {
            continue;
        }
        if (!root.Defined() || !context.SharesGPR(root, produced)) {
            continue;
        }
        if (last_use(root.Def()) > producer->Id()) {
            return false;
        }
    }

    u32 live_end = inst->Id();
    if (live_publish) {
        live_end = std::max<u32>(live_end, last_use(wrapper));
        if (zero_extend_chain) {
            live_end = std::max<u32>(live_end, last_use(producer));
        }
    }
    for (auto& other : cur_block->GetInstList()) {
        if (&other == component_source || &other == producer ||
            &other == wrapper || &other == inst ||
            !other.HasValue() || other.IsBitCastOperation() ||
            other.Id() > live_end) {
            continue;
        }
        const bool low32_copy_alias =
                context.IsLow32CopyCoalesced(other.Id()) &&
                other.GetOp() == ir::OpCode::BitExtract &&
                other.GetArg<ir::Imm>(1).Get() == 0 &&
                other.GetArg<ir::Imm>(2).Get() == 32 &&
                (context.Low32CopySource(other.Id()) == stored.Id() ||
                 context.Low32CopySource(other.Id()) == produced.Id());
        if (low32_copy_alias) {
            continue;
        }
        if (other.Id() > inst->Id() &&
            other.GetOp() == ir::OpCode::GetHostGPR &&
            other.GetArg<ir::Imm>(0).Get() == target) {
            continue;
        }
        ir::Value value{&other};
        const auto publication_value = low32_copy ? stored : produced;
        if (context.SharesGPR(value, publication_value) &&
            last_use(&other) > proof_start) {
            return false;
        }
    }

    bool after_start = false;
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() == proof_start) {
            after_start = true;
            continue;
        }
        if (!after_start || &scan == producer || &scan == wrapper) {
            continue;
        }
        if (&scan == inst) {
            break;
        }
        const bool flags_only = scan.GetOp() == ir::OpCode::SaveFlags;
        if ((!flags_only && IsHostCoalesceObserver(scan.GetOp())) ||
            (scan.GetOp() == ir::OpCode::GetHostGPR &&
             scan.GetArg<ir::Imm>(0).Get() == target) ||
            (scan.GetOp() == ir::OpCode::SetHostGPR &&
             scan.GetArg<ir::Imm>(1).Get() == target)) {
            return false;
        }
    }
    if (!live_publish) {
        for (auto& scan : cur_block->GetInstList()) {
            if (scan.Id() <= inst->Id()) {
                continue;
            }
            for (auto value : scan.GetValues()) {
                if (ResolveHostCoalesceBitCast(value).Def() == wrapper) {
                    if (width_wrapper) {
                        continue;
                    }
                    return false;
                }
            }
        }
        if (TerminalUsesHostCoalesceValue(cur_block->GetTerminal(), wrapper)) {
            return false;
        }
        return true;
    }
    bool after_store = false;
    for (auto& scan : cur_block->GetInstList()) {
        if (&scan == inst) {
            after_store = true;
            continue;
        }
        if (!after_store || scan.Id() > live_end) {
            continue;
        }
        if (scan.GetOp() == ir::OpCode::SetHostGPR &&
            scan.GetArg<ir::Imm>(1).Get() == target) {
            return false;
        }
        if (target <= 9 &&
            (scan.GetOp() == ir::OpCode::CallLambda ||
             scan.GetOp() == ir::OpCode::CallLocation ||
             scan.GetOp() == ir::OpCode::CallDynamic ||
             scan.GetOp() == ir::OpCode::X87Op ||
             scan.GetOp() == ir::OpCode::Sse42Str)) {
            return false;
        }
    }
    return true;
}

bool JitTranslator::ReproveCoalescedHostRead(ir::Inst* inst) const {
    if (!inst || inst->GetOp() != ir::OpCode::GetHostGPR ||
        inst->GetArg<ir::Imm>(1).Get() != 0 ||
        ir::GetValueSizeByte(inst->ReturnType()) != sizeof(u32)) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(0).Get();
    if (context.X(ir::Value{inst}).GetCode() != target) {
        return false;
    }
    ir::Inst* latest_store = nullptr;
    bool blocked = false;
    for (auto& scan : cur_block->GetInstList()) {
        if (&scan == inst) {
            break;
        }
        if (scan.GetOp() == ir::OpCode::SetHostGPR &&
            scan.GetArg<ir::Imm>(1).Get() == target) {
            latest_store = &scan;
            blocked = false;
            continue;
        }
        if (latest_store && IsHostCoalesceObserver(scan.GetOp())) {
            blocked = true;
        }
    }
    if (!latest_store || blocked) {
        return false;
    }
    auto published = ResolveHostCoalesceBitCast(latest_store->GetArg<ir::Value>(0));
    bool high_zero = published.Def() &&
            ((published.Def()->GetOp() == ir::OpCode::ZeroExtend32To64 &&
              ir::GetValueSizeByte(published.Def()->GetArg<ir::Value>(0).Type()) == sizeof(u32)) ||
             (ir::GetValueSizeByte(published.Type()) == sizeof(u32) &&
              IsKnownHostWWrite(published)));
    if (published.Defined() && context.IsWidthChainCoalesced(published.Id())) {
        const u32 anchor = context.WidthChainAnchor(published.Id());
        if (context.HasWidthComponentOwner(anchor)) {
            if (!context.WidthComponentOwnerCommitted(anchor) ||
                !context.IsHostWriteCoalesced(latest_store->Id()) ||
                context.WidthComponentOwnerTarget(anchor) != target) {
                return false;
            }
            high_zero = context.WidthComponentOwnerHighZero(anchor);
        }
    }
    if (!high_zero) {
        return false;
    }
    u32 read_end = inst->Id();
    for (auto& scan : cur_block->GetInstList()) {
        for (auto value : scan.GetValues()) {
            if (ResolveHostCoalesceBitCast(value).Def() == inst) {
                read_end = std::max<u32>(read_end, scan.Id());
            }
        }
    }
    if (TerminalUsesHostCoalesceValue(cur_block->GetTerminal(), inst) &&
        cur_block->GetInstList().begin() != cur_block->GetInstList().end()) {
        read_end = std::max<u32>(read_end, std::prev(cur_block->GetInstList().end())->Id());
    }
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() > inst->Id() && scan.Id() <= read_end &&
            scan.GetOp() == ir::OpCode::SetHostGPR &&
            scan.GetArg<ir::Imm>(1).Get() == target) {
            return false;
        }
    }
    return true;
}

bool JitTranslator::ReproveCoalescedHostFPRRead(ir::Inst* inst) const {
    if (!inst || inst->GetOp() != ir::OpCode::GetHostFPR ||
        inst->GetArg<ir::Imm>(1).Get() != 0 ||
        !ir::IsFloatValueType(inst->ReturnType())) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(0).Get();
    if (context.V(ir::Value{inst}).GetCode() != target) {
        return false;
    }
    u32 end = inst->Id();
    for (auto& scan : cur_block->GetInstList()) {
        for (auto value : scan.GetValues()) {
            if (ResolveHostCoalesceBitCast(value).Def() == inst) {
                end = std::max<u32>(end, scan.Id());
            }
        }
    }
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() <= inst->Id() || scan.Id() > end) continue;
        if (scan.GetOp() == ir::OpCode::SetHostFPR &&
            scan.GetArg<ir::Imm>(1).Get() == target) {
            return false;
        }
    }
    return true;
}

bool JitTranslator::ReproveAesChainTie(ir::Inst* inst) const {
    if (!context.GetFeatures().ra_aes_chain_tie || !inst ||
        (inst->GetOp() != ir::OpCode::VecAesEncFast &&
         inst->GetOp() != ir::OpCode::VecAesEncLastFast) ||
        !context.IsAesChainTied(inst->Id())) {
        return false;
    }
    const u32 target = context.AesChainTarget(inst->Id());
    auto data = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(0));
    auto key = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(1));
    auto zero = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(2));
    if (!data.Defined() || !data.Def() || target < 16 || target > 31 ||
        context.V(ir::Value{inst}).GetCode() != target ||
        context.V(data).GetCode() != target ||
        context.V(key).GetCode() == target ||
        context.V(zero).GetCode() == target) {
        return false;
    }

    u32 data_end = data.Def()->Id();
    ir::Inst* semantic_consumer = nullptr;
    u32 semantic_uses = 0;
    for (auto& scan : cur_block->GetInstList()) {
        for (auto value : scan.GetValues()) {
            auto root = ResolveHostCoalesceBitCast(value);
            if (root.Def() == data.Def()) {
                data_end = std::max<u32>(data_end, scan.Id());
            }
            if (!scan.IsBitCastOperation() && root.Def() == inst) {
                semantic_consumer = &scan;
                ++semantic_uses;
            }
        }
    }
    if (data_end != inst->Id() || semantic_uses != 1 || !semantic_consumer) {
        return false;
    }

    const bool predecessor_is_chain =
            (data.Def()->GetOp() == ir::OpCode::VecAesEncFast ||
             data.Def()->GetOp() == ir::OpCode::VecAesEncLastFast) &&
            context.IsAesChainTied(data.Id()) &&
            context.AesChainTarget(data.Id()) == target;
    const bool predecessor_is_home =
            data.Def()->GetOp() == ir::OpCode::GetHostFPR &&
            data.Def()->GetArg<ir::Imm>(0).Get() == target &&
            data.Def()->GetArg<ir::Imm>(1).Get() == 0 &&
            context.IsHostReadCoalesced(data.Id());
    if (!predecessor_is_chain && !predecessor_is_home) {
        return false;
    }

    const bool successor_is_chain =
            (semantic_consumer->GetOp() == ir::OpCode::VecAesEncFast ||
             semantic_consumer->GetOp() == ir::OpCode::VecAesEncLastFast) &&
            context.IsAesChainTied(semantic_consumer->Id()) &&
            context.AesChainTarget(semantic_consumer->Id()) == target &&
            ResolveHostCoalesceBitCast(
                    semantic_consumer->GetArg<ir::Value>(0)).Def() == inst;
    const bool successor_is_store =
            semantic_consumer->GetOp() == ir::OpCode::SetHostFPR &&
            semantic_consumer->GetArg<ir::Imm>(1).Get() == target &&
            semantic_consumer->GetArg<ir::Imm>(2).Get() == 0 &&
            context.IsHostWriteCoalesced(semantic_consumer->Id());
    // A single producer is deliberately outside this spike: the component
    // must own both the initial read and final publication, not just move one
    // of the two copies to the other side of the AES instruction.
    return (predecessor_is_home && successor_is_chain) ||
           (predecessor_is_chain && (successor_is_chain || successor_is_store));
}

bool JitTranslator::ReproveAesChainHostWrite(ir::Inst* inst) const {
    if (!inst || inst->GetOp() != ir::OpCode::SetHostFPR ||
        inst->GetArg<ir::Imm>(2).Get() != 0) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    auto produced = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(0));
    if (!produced.Defined() || !produced.Def() ||
        !context.IsAesChainTied(produced.Id()) ||
        context.AesChainTarget(produced.Id()) != target ||
        context.V(produced).GetCode() != target) {
        return false;
    }

    std::vector<ir::Inst*> reverse_chain;
    auto data = produced;
    while (data.Defined() && data.Def() && context.IsAesChainTied(data.Id())) {
        auto* node = data.Def();
        if (!ReproveAesChainTie(node)) {
            return false;
        }
        reverse_chain.push_back(node);
        data = ResolveHostCoalesceBitCast(node->GetArg<ir::Value>(0));
    }
    if (reverse_chain.size() < 2 || !data.Defined() || !data.Def() ||
        data.Def()->GetOp() != ir::OpCode::GetHostFPR ||
        data.Def()->GetArg<ir::Imm>(0).Get() != target ||
        data.Def()->GetArg<ir::Imm>(1).Get() != 0 ||
        !context.IsHostReadCoalesced(data.Id()) ||
        context.V(data).GetCode() != target) {
        return false;
    }
    std::reverse(reverse_chain.begin(), reverse_chain.end());
    auto is_chain_node = [&](const ir::Inst* candidate) {
        return std::find(reverse_chain.begin(), reverse_chain.end(), candidate) !=
               reverse_chain.end();
    };

    auto last_use = [&](ir::Inst* definition) {
        u32 end = definition->Id();
        for (auto& scan : cur_block->GetInstList()) {
            for (auto value : scan.GetValues()) {
                if (ResolveHostCoalesceBitCast(value).Def() == definition) {
                    end = std::max<u32>(end, scan.Id());
                }
            }
        }
        return end;
    };
    if (last_use(produced.Def()) != inst->Id()) {
        return false;
    }
    const u32 begin = reverse_chain.front()->Id();
    for (auto& other : cur_block->GetInstList()) {
        if (&other == inst || &other == data.Def() || is_chain_node(&other) ||
            !other.HasValue() || other.IsBitCastOperation() || other.Id() >= inst->Id()) {
            continue;
        }
        ir::Value value{&other};
        if (context.SharesFPR(value, produced) && last_use(&other) > begin) {
            return false;
        }
    }
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() <= begin || scan.Id() >= inst->Id() ||
            is_chain_node(&scan) || scan.IsBitCastOperation()) {
            continue;
        }
        if ((scan.GetOp() == ir::OpCode::GetHostFPR &&
             scan.GetArg<ir::Imm>(0).Get() == target) ||
            (scan.GetOp() == ir::OpCode::SetHostFPR &&
             scan.GetArg<ir::Imm>(1).Get() == target)) {
            return false;
        }
        const bool committed_home_safe =
                scan.GetOp() == ir::OpCode::LoadMemory ||
                scan.GetOp() == ir::OpCode::StoreMemory ||
                scan.GetOp() == ir::OpCode::SaveFlags;
        if (IsHostCoalesceObserver(scan.GetOp()) && !committed_home_safe) {
            return false;
        }
    }
    return true;
}

bool JitTranslator::ReproveCoalescedHostFPRWrite(ir::Inst* inst) const {
    if (!inst || inst->GetOp() != ir::OpCode::SetHostFPR ||
        inst->GetArg<ir::Imm>(2).Get() != 0) {
        return false;
    }
    const u32 target = inst->GetArg<ir::Imm>(1).Get();
    auto produced = ResolveHostCoalesceBitCast(inst->GetArg<ir::Value>(0));
    auto* producer = produced.Def();
    const bool scalar_unary = producer &&
            IsHostScalarUnaryProducer(*producer, sse_scalar_tie);
    if (producer && context.IsAesChainTied(produced.Id())) {
        return ReproveAesChainHostWrite(inst);
    }
    if (!producer || produced.Type() != ir::ValueType::V128 ||
        (!IsHostFPRCoalesceProducer(producer->GetOp(), sse_scalar_insert,
                                    sse_scalar_tie) &&
         !scalar_unary) ||
        context.V(produced).GetCode() != target) {
        return false;
    }

    auto last_use = [&](ir::Inst* definition) {
        u32 end = definition->Id();
        for (auto& scan : cur_block->GetInstList()) {
            for (auto use : scan.GetValues()) {
                if (ResolveHostCoalesceBitCast(use).Def() == definition) {
                    end = std::max<u32>(end, scan.Id());
                }
            }
        }
        return end;
    };
    const u32 produced_end = last_use(producer);
    if (produced_end < inst->Id()) {
        return false;
    }
    if (IsHostScalarFPRBinaryProducer(producer->GetOp())) {
        auto left = ResolveHostCoalesceBitCast(producer->GetArg<ir::Value>(0));
        if (sse_scalar_insert) {
            if (!left.Defined() || context.V(left).GetCode() != target ||
                !ReproveScalarFPRTie(producer)) {
                return false;
            }
        } else if (left.Defined() && context.SharesFPR(left, produced)) {
            if (!left.Def() ||
                left.Def()->GetOp() != ir::OpCode::GetHostFPR ||
                left.Def()->GetArg<ir::Imm>(0).Get() != target ||
                left.Def()->GetArg<ir::Imm>(1).Get() != 0 ||
                !context.IsHostReadCoalesced(left.Id()) ||
                last_use(left.Def()) != producer->Id()) {
                return false;
            }
        }
    } else if (scalar_unary) {
        auto merge = ResolveHostCoalesceBitCast(
                producer->GetArg<ir::Value>(1));
        if (!merge.Defined() || !merge.Def() ||
            merge.Def()->GetOp() != ir::OpCode::GetHostFPR ||
            merge.Def()->GetArg<ir::Imm>(0).Get() != target ||
            merge.Def()->GetArg<ir::Imm>(1).Get() != 0 ||
            !context.IsHostReadCoalesced(merge.Id()) ||
            context.V(merge).GetCode() != target ||
            last_use(merge.Def()) != producer->Id()) {
            return false;
        }
    }
    for (auto input : producer->GetValues()) {
        auto root = ResolveHostCoalesceBitCast(input);
        if (root.Defined() && context.SharesFPR(root, produced) &&
            last_use(root.Def()) > producer->Id()) {
            return false;
        }
    }
    auto is_in_place_successor = [&](ir::Inst& successor) {
        if (successor.Id() <= producer->Id() || successor.Id() != produced_end) {
            return false;
        }
        const auto successor_values = successor.GetValues();
        const bool consumes_produced = std::any_of(
                successor_values.begin(), successor_values.end(), [producer](ir::Value value) {
                    return ResolveHostCoalesceBitCast(value).Def() == producer;
                });
        if (!consumes_produced) {
            return false;
        }
        for (auto& scan : cur_block->GetInstList()) {
            if (scan.Id() <= successor.Id() || scan.GetOp() != ir::OpCode::SetHostFPR ||
                scan.GetArg<ir::Imm>(1).Get() != target || scan.GetArg<ir::Imm>(2).Get() != 0 ||
                !context.IsHostWriteCoalesced(scan.Id())) {
                continue;
            }
            if (ResolveHostCoalesceBitCast(scan.GetArg<ir::Value>(0)).Def() == &successor) {
                return ReproveCoalescedHostFPRWrite(&scan);
            }
        }
        return false;
    };
    for (auto& other : cur_block->GetInstList()) {
        if (&other == producer || &other == inst || !other.HasValue() ||
            other.IsBitCastOperation() || other.Id() > produced_end) {
            continue;
        }
        ir::Value value{&other};
        if (context.SharesFPR(value, produced) && last_use(&other) > producer->Id() &&
            !is_in_place_successor(other)) {
            return false;
        }
    }
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() <= inst->Id() || scan.Id() > produced_end) {
            continue;
        }
        if (scan.GetOp() == ir::OpCode::SetHostFPR &&
            scan.GetArg<ir::Imm>(1).Get() == target) {
            return false;
        }
    }
    bool after_producer = false;
    for (auto& scan : cur_block->GetInstList()) {
        if (&scan == producer) {
            after_producer = true;
            continue;
        }
        if (!after_producer) continue;
        if (&scan == inst) break;
        if (IsHostCoalesceObserver(scan.GetOp()) ||
            (scan.GetOp() == ir::OpCode::GetHostFPR &&
             scan.GetArg<ir::Imm>(0).Get() == target) ||
            (scan.GetOp() == ir::OpCode::SetHostFPR &&
             scan.GetArg<ir::Imm>(1).Get() == target)) {
            return false;
        }
    }
    return true;
}

bool JitTranslator::ReproveScalarFPRTie(ir::Inst* inst) const {
    if (!sse_scalar_tie || !sse_scalar_insert || !inst ||
        !IsHostScalarFPRBinaryProducer(inst->GetOp())) {
        return false;
    }
    const u32 target = context.V(ir::Value{inst}).GetCode();
    if (target < 16 || target > 31) {
        return false;
    }
    auto last_use = [&](ir::Inst* definition) {
        u32 end = definition->Id();
        for (auto& scan : cur_block->GetInstList()) {
            for (auto use : scan.GetValues()) {
                if (ResolveHostCoalesceBitCast(use).Def() == definition) {
                    end = std::max<u32>(end, scan.Id());
                }
            }
        }
        return end;
    };
    auto crosses_write = [&](u32 begin, u32 end, ir::Inst* owner) {
        for (auto& scan : cur_block->GetInstList()) {
            if (begin < scan.Id() && scan.Id() < end && scan.GetOp() == ir::OpCode::SetHostFPR &&
                scan.GetArg<ir::Imm>(1).Get() == target &&
                ResolveHostCoalesceBitCast(scan.GetArg<ir::Value>(0)).Def() != owner) {
                return true;
            }
        }
        return false;
    };
    const auto result_end = last_use(inst);
    if (crosses_write(inst->Id(), result_end, inst)) {
        return false;
    }
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.GetOp() == ir::OpCode::GetHostFPR &&
            context.IsHostReadCoalesced(scan.Id()) &&
            scan.GetArg<ir::Imm>(0).Get() == target &&
            scan.Id() < result_end && last_use(&scan) > inst->Id()) {
            return false;
        }
    }
    auto tie_source = [&](ir::Inst* node) -> ir::Value {
        if (IsHostScalarFPRBinaryProducer(node->GetOp())) {
            return node->GetArg<ir::Value>(0);
        }
        if (IsHostScalarUnaryProducer(*node, sse_scalar_tie)) {
            return node->GetArg<ir::Value>(1);
        }
        return {};
    };
    for (auto* node = inst;;) {
        auto left = ResolveHostCoalesceBitCast(tie_source(node));
        if (!left.Defined() || !left.Def() || context.V(left).GetCode() != target ||
            last_use(left.Def()) != node->Id() ||
            crosses_write(left.Id(), node->Id(), left.Def())) {
            return false;
        }
        if (left.Def()->GetOp() == ir::OpCode::GetHostFPR) {
            return left.Def()->GetArg<ir::Imm>(0).Get() == target &&
                   left.Def()->GetArg<ir::Imm>(1).Get() == 0 &&
                   context.IsHostReadCoalesced(left.Id());
        }
        if (!IsHostScalarFPRBinaryProducer(left.Def()->GetOp()) &&
            !IsHostScalarUnaryProducer(*left.Def(), sse_scalar_tie)) {
            for (auto& scan : cur_block->GetInstList()) {
                if (scan.Id() <= left.Id() || scan.Id() >= node->Id() ||
                    scan.GetOp() != ir::OpCode::SetHostFPR ||
                    scan.GetArg<ir::Imm>(1).Get() != target ||
                    !context.IsHostWriteCoalesced(scan.Id())) {
                    continue;
                }
                auto value = ResolveHostCoalesceBitCast(scan.GetArg<ir::Value>(0));
                if (value.Def() == left.Def()) {
                    return ReproveCoalescedHostFPRWrite(&scan);
                }
            }
            return false;
        }
        node = left.Def();
    }
}

#define __ masm.

namespace {

bool StructuredAddressModeEnabled(const FeatureSet& features) {
    return features.addrmode_struct;
}

void HostMemMove(void* dst, const void* src, size_t size) {
    std::memmove(dst, src, size);
}

struct TsoEmissionStats {
    const bool enabled{GetSvmConfig().tso_stats};
    std::atomic<u64> load_sites{};
    std::atomic<u64> store_sites{};
    std::atomic<u64> scalar_fast_sites{};
    std::atomic<u64> alignment_check_sites{};
    std::atomic<u64> dmb_instructions{};

    ~TsoEmissionStats() {
        if (enabled) {
            SVM_DIAG_PRINT(Codegen,
                         "SVM_TSO_STATS load_sites=%llu store_sites=%llu "
                         "scalar_fast_sites=%llu alignment_check_sites=%llu "
                         "dmb_instructions=%llu\n",
                         static_cast<unsigned long long>(load_sites.load()),
                         static_cast<unsigned long long>(store_sites.load()),
                         static_cast<unsigned long long>(scalar_fast_sites.load()),
                         static_cast<unsigned long long>(alignment_check_sites.load()),
                         static_cast<unsigned long long>(dmb_instructions.load()));
        }
    }

    void Increment(std::atomic<u64>& counter) {
        if (enabled) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

TsoEmissionStats tso_emission_stats;

}  // namespace

bool HostBaseFoldEligible(bool enabled,
                          bool use_memory_base,
                          u64 guest_addr_mask,
                          ir::ValueType type,
                          bool structured_guest_ea,
                          bool guest_add_form,
                          bool tso_or_atomic) {
    // The W39 structured operand has already computed the guest EA with W
    // arithmetic.  Only the bounded 4GB layout may use UXTW here: unbounded
    // and non-32-bit windows need their existing 64-bit bias/mask sequence.
    return enabled && use_memory_base && guest_addr_mask == UINT32_MAX &&
           type == ir::ValueType::V128 && structured_guest_ea && guest_add_form &&
           !tso_or_atomic;
}

void JitTranslator::LoadUnalignedAtomicLockAddress(const Register& lock) {
    __ Ldr(lock, MemOperand(state, state_offset_unaligned_atomic_lock));
}

void JitTranslator::AcquireUnalignedAtomicLock(const Register& lock,
                                               const Register& scratch) {
    // x12 is last_result when FLAGS_REGS is on. The lock sequence clobbers it.
    if (FlagsRegsEnabled()) {
        EmitSplitFlagsPublish();
    }
    Label retry;
    __ Bind(&retry);
    __ Ldaxr(scratch.W(), MemOperand(lock));
    __ Cbnz(scratch.W(), &retry);
    __ Mov(scratch.W(), 1);
    __ Stxr(ipw, scratch.W(), MemOperand(lock));
    __ Cbnz(ipw, &retry);
}

void JitTranslator::ReleaseUnalignedAtomicLock(const Register& lock) {
    __ Stlr(wzr, MemOperand(lock));
}

void JitTranslator::EmitBasicAtomicLoad(ir::ValueType type,
                                        const Register& result,
                                        const Register& address) {
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldrb(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldrh(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldr(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldr(result, MemOperand(address));
            break;
        default:
            PANIC("unsupported atomic load width");
    }
}

void JitTranslator::EmitBasicAtomicStore(ir::ValueType type,
                                         const Register& value,
                                         const Register& address) {
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Strb(value.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Strh(value.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Str(value.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Str(value, MemOperand(address));
            break;
        default:
            PANIC("unsupported atomic store width");
    }
}

void JitTranslator::EmitAtomicRMWValue(ir::AtomicRMWOp op,
                                       ir::ValueType type,
                                       const Register& output,
                                       const Register& old,
                                       ir::Value operand,
                                       ir::Value carry) {
    const bool wide = ir::GetValueSizeByte(type) == 8;
    const auto dst = wide ? output : output.W();
    const auto lhs = wide ? old : old.W();
    const Register rhs = context.R(operand);
    switch (op) {
        case ir::AtomicRMWOp::Add:
            __ Add(dst, lhs, rhs);
            break;
        case ir::AtomicRMWOp::Sub:
            __ Sub(dst, lhs, rhs);
            break;
        case ir::AtomicRMWOp::And:
            __ And(dst, lhs, rhs);
            break;
        case ir::AtomicRMWOp::Or:
            __ Orr(dst, lhs, rhs);
            break;
        case ir::AtomicRMWOp::Xor:
            __ Eor(dst, lhs, rhs);
            break;
        case ir::AtomicRMWOp::Neg:
            __ Neg(dst, lhs);
            break;
        case ir::AtomicRMWOp::AddCarry:
            __ Add(dst, lhs, rhs);
            __ Add(dst, dst, context.W(carry));
            break;
        case ir::AtomicRMWOp::SubBorrow:
            __ Sub(dst, lhs, rhs);
            __ Sub(dst, dst, context.W(carry));
            break;
    }
}

void JitTranslator::EmitGuestToHost(const Register& dst, const Register& guest_addr) {
    if (memory_state.window_uxtw) {
        // pt + zext32(guest): one instruction, same as the unbounded Add.
        __ Add(dst, pt, Operand(guest_addr.W(), UXTW));
        return;
    }
    if (memory_state.guest_addr_mask) {
        __ And(dst, guest_addr, memory_state.guest_addr_mask);
        __ Add(dst, dst, pt);
        return;
    }
    __ Add(dst, guest_addr, pt);
}

MemOperand JitTranslator::BiasMem(const Register& base, bool atomic) {
    if (memory_state.window_uxtw) {
        // Bounded 32-bit guest window: [pt, Wbase, UXTW] is the *same*
        // register-offset load the unbounded path emitted, with the
        // truncation folded into the addressing mode — zero extra cost.
        if (!atomic) {
            return MemOperand{pt, base.W(), UXTW};
        }
        __ Add(mem_scratch, pt, Operand(base.W(), UXTW));
        return MemOperand{mem_scratch};
    }
    if (memory_state.guest_addr_mask) {
        // Non-32-bit window: one extra `and` with a logical immediate.
        __ And(mem_scratch, base, memory_state.guest_addr_mask);
        if (!atomic) {
            return MemOperand{mem_scratch, pt};
        }
        __ Add(mem_scratch, mem_scratch, pt);
        return MemOperand{mem_scratch};
    }
    if (!atomic) {
        return MemOperand{base, pt};
    }
    // No register-offset form available: fold the bias into the reserved
    // scratch (mem_scratch is never allocated to a guest value, unlike a
    // GetTmpX register at a VOID instruction — see defines.h).
    __ Add(mem_scratch, base, pt);
    return MemOperand{mem_scratch};
}

MemOperand JitTranslator::BiasMem(const Register& base, s64 imm, bool atomic) {
    if (imm == 0) {
        return BiasMem(base, atomic);
    }
    if (memory_state.window_uxtw) {
        // 32-bit add wraps mod 2^32, so the displacement is applied *inside*
        // the window and the truncation is again free.
        if (imm > 0) {
            __ Add(mem_scratch.W(), base.W(), imm);
        } else {
            __ Sub(mem_scratch.W(), base.W(), -imm);
        }
        if (atomic) {
            __ Add(mem_scratch, pt, Operand(mem_scratch.W(), UXTW));
            return MemOperand{mem_scratch};
        }
        return MemOperand{pt, mem_scratch.W(), UXTW};
    }
    // [guest base + imm + pt]: fold the immediate into the reserved scratch.
    if (imm > 0) {
        __ Add(mem_scratch, base, imm);
    } else {
        __ Sub(mem_scratch, base, -imm);
    }
    if (memory_state.guest_addr_mask) {
        __ And(mem_scratch, mem_scratch, memory_state.guest_addr_mask);
    }
    if (atomic) {
        __ Add(mem_scratch, mem_scratch, pt);
        return MemOperand{mem_scratch};
    }
    return MemOperand{mem_scratch, pt};
}

void JitTranslator::EmitGetHostGPR(ir::Inst* inst) {
    if (context.IsHostReadCoalesced(inst->Id())) {
        ASSERT_MSG(ReproveCoalescedHostRead(inst),
                   "GetHostGPR coalescing proof diverged at IR {}", inst->Id());
        return;
    }
    if (pinned_gprs.fused_pin_gpr_reads.contains(inst)) {
        return;
    }
    if (guest_state_map.ValueFullyResident(inst)) {
        return;
    }
    auto offset = inst->GetArg<ir::Imm>(1).Get();
    auto reg_index = inst->GetArg<ir::Imm>(0).Get();
    const u32 value_size = ir::GetValueSizeByte(inst->ReturnType());
    const bool pin_ext_reg = IsFixedGPRHome(reg_index);
    if (offset == 0 && pin_ext_reg &&
        inst->GetUses() != 0 && value_size <= sizeof(u32)) {
        auto& list = cur_block->GetInstList();
        for (auto it = std::next(list.iterator_to(*inst)); it != list.end(); ++it) {
            if (it->GetOp() == ir::OpCode::SetHostGPR &&
                it->GetArg<ir::Imm>(1).Get() == reg_index) {
                break;  // the computed read must retain capture semantics
            }
            u32 named_uses = 0;
            for (auto used : it->GetValues()) {
                named_uses += used.Def() == inst;
            }
            if (named_uses == 0) {
                continue;
            }
            if (named_uses != inst->GetUses()) {
                break;
            }
            // Narrow And/Xor reads stay fused for the callee-saved pins
            // (22/23/29) below level 3 — the W56-proven shape. The x6-x9
            // caller-saved pins keep codex's conservative restriction.
            const bool direct_alu =
                    (it->GetOp() == ir::OpCode::And || it->GetOp() == ir::OpCode::Xor) &&
                    ir::GetValueSizeByte(it->ReturnType()) <= sizeof(u32) &&
                    (reg_index <= 5 || value_size == sizeof(u32) ||
                     (reg_index >= 22 && !backend::X86PinExtLevel3Requested()));
            const bool direct_u32_or =
                    it->GetOp() == ir::OpCode::Or &&
                    value_size == sizeof(u32) &&
                    ir::GetValueSizeByte(it->ReturnType()) == sizeof(u32);
            const bool direct_caller_pin_alu =
                    reg_index <= 9 &&
                    (it->GetOp() == ir::OpCode::Add || it->GetOp() == ir::OpCode::Sub) &&
                    ir::GetValueSizeByte(it->ReturnType()) <= sizeof(u32) &&
                    (reg_index <= 5 || value_size == sizeof(u32));
            const auto pseudo_flags = GetPseudoFlags(&*it);
            const bool direct_adjacent_narrow_flags =
                    reg_index >= 6 && reg_index <= 9 &&
                    (value_size == sizeof(u8) || value_size == sizeof(u16)) &&
                    it->Id() == inst->Id() + 1 &&
                    (it->GetOp() == ir::OpCode::Add ||
                     it->GetOp() == ir::OpCode::Sub) &&
                    ir::GetValueSizeByte(it->ReturnType()) == value_size &&
                    !pseudo_flags.Null() &&
                    True(pseudo_flags.set & ir::Flags::NZCV);
            const bool direct_callee_pin_sub =
                    reg_index >= 19 && it->GetOp() == ir::OpCode::Sub &&
                    ir::GetValueSizeByte(it->ReturnType()) <= sizeof(u32);
            const bool direct_extend =
                    (value_size == sizeof(u8) || value_size == sizeof(u16)) &&
                    it->GetOp() == ir::OpCode::ZeroExtend32 &&
                    it->GetArg<ir::Value>(0).Def() == inst;
            const bool direct_sign_extend =
                    reg_index >= 19 && it->GetOp() == ir::OpCode::SignExtend &&
                    it->GetArg<ir::Value>(0).Def() == inst;
            const bool direct_store =
                    named_uses == 1 && it->GetOp() == ir::OpCode::StoreMemory &&
                    it->GetArg<ir::Value>(1).Def() == inst;
            if (direct_alu || direct_u32_or || direct_caller_pin_alu ||
                direct_adjacent_narrow_flags ||
                direct_callee_pin_sub ||
                direct_extend || direct_sign_extend || direct_store) {
                pinned_gprs.fused_pin_gpr_reads.emplace(inst, static_cast<u16>(reg_index));
                return;
            }
            break;
        }
    }
    auto host_reg = XRegister(reg_index);
    auto ret_reg = context.X(ir::Value{inst});
    const auto bit_offset = offset * 8;
    const auto bit_width = value_size * 8;
    if (bit_offset == 0 && bit_width == 64) {
        if (host_reg != ret_reg) {
            __ Mov(ret_reg, host_reg);
        }
    } else if (bit_offset == 0 && bit_width == 32 && reg_index <= 9) {
        // A caller-saved-pin U32 GetHostGPR may be allocated directly to its static
        // W register. Reading W already supplies x86's required zero extension.
        if (host_reg.W() != ret_reg.W()) {
            __ Mov(ret_reg.W(), host_reg.W());
        }
    } else {
        __ Ubfx(ret_reg, host_reg, bit_offset, bit_width);
    }
}

void JitTranslator::EmitGetHostFPR(ir::Inst* inst) {
    if (context.IsHostReadCoalesced(inst->Id())) {
        ASSERT_MSG(ReproveCoalescedHostFPRRead(inst),
                   "GetHostFPR coalescing proof diverged at IR {}", inst->Id());
        return;
    }
    const u32 reg_index = inst->GetArg<ir::Imm>(0).Get();
    auto host_reg = VRegister::GetQRegFromCode(reg_index);
    const u32 offset = inst->GetArg<ir::Imm>(1).Get();
    const auto value_type = inst->ReturnType();
    const u32 size = ir::GetValueSizeByte(value_type);
    ASSERT_MSG(offset + size <= sizeof(u128) && offset % size == 0,
               "invalid fixed FPR read offset {} size {}", offset, size);

    if (!ir::IsFloatValueType(value_type)) {
        auto result = context.R(ir::Value{inst});
        const u32 lane = offset / size;
        switch (size) {
            case 1: __ Umov(result.W(), host_reg.V16B(), lane); break;
            case 2: __ Umov(result.W(), host_reg.V8H(), lane); break;
            case 4: __ Umov(result.W(), host_reg.V4S(), lane); break;
            case 8: __ Umov(result.X(), host_reg.V2D(), lane); break;
            default: PANIC("unsupported scalar fixed FPR read size {}", size);
        }
        return;
    }

    auto result = context.V(ir::Value{inst});
    if (size == sizeof(u128)) {
        ASSERT(offset == 0);
        if (host_reg != result) {
            __ Orr(result.V16B(), host_reg.V16B(), host_reg.V16B());
        }
        return;
    }
    const u32 lane = offset / size;
    switch (size) {
        case 1: __ Ins(result.V16B(), 0, host_reg.V16B(), lane); break;
        case 2: __ Ins(result.V8H(), 0, host_reg.V8H(), lane); break;
        case 4: __ Ins(result.V4S(), 0, host_reg.V4S(), lane); break;
        case 8: __ Ins(result.V2D(), 0, host_reg.V2D(), lane); break;
        default: PANIC("unsupported vector fixed FPR read size {}", size);
    }
}

void JitTranslator::EmitSetHostGPR(ir::Inst* inst) {
    if (auto direct = pinned_gprs.pinned_select_publications.find(inst);
        direct != pinned_gprs.pinned_select_publications.end()) {
        const auto reproved = MatchPinnedSelectPublication(inst);
        ASSERT_MSG(reproved && *reproved == direct->second,
                   "pinned SelectZero publication proof diverged at IR {}",
                   inst->Id());
        return;
    }
    if (auto view = pinned_gprs.pinned_gpr_publication_views.find(inst);
        view != pinned_gprs.pinned_gpr_publication_views.end()) {
        const auto reproved = MatchPinnedGPRPublicationView(inst);
        ASSERT_MSG(reproved && *reproved == view->second,
                   "pinned GPR publication view proof diverged at IR {}", inst->Id());
    }
    if (auto direct = pinned_gprs.spilled_gpr_publications.find(inst);
        direct != pinned_gprs.spilled_gpr_publications.end()) {
        const auto reproved = MatchSpilledGPRPublication(inst);
        ASSERT_MSG(reproved && *reproved == direct->second,
                   "spilled GPR publication proof diverged at IR {}",
                   inst->Id());
        return;
    }
    if (auto update = pinned_gprs.pinned_load_update_instructions.find(inst);
        update != pinned_gprs.pinned_load_update_instructions.end() &&
        inst == update->second.publication) {
        const auto reproved = MatchPinnedLoadUpdate(update->second.update);
        ASSERT_MSG(reproved && *reproved == update->second,
                   "pinned load update proof diverged at IR {}", inst->Id());
        return;
    }
    if (pinned_gprs.dead_pinned_gpr_writes.contains(inst)) {
        ASSERT_MSG(IsDeadPinnedGPRWrite(inst),
                   "dead pinned GPR write proof diverged at IR {}", inst->Id());
        return;
    }
    if (auto transfer = pinned_gprs.pinned_gpr_value_transfers.find(inst);
        transfer != pinned_gprs.pinned_gpr_value_transfers.end()) {
        const auto reproved = MatchPinnedGPRValueTransfer(inst);
        ASSERT_MSG(reproved && *reproved == transfer->second,
                   "pinned GPR value transfer proof diverged at IR {}",
                   inst->Id());
        __ Mov(XRegister(transfer->second.target),
               XRegister(transfer->second.source));
        return;
    }
    if (auto copy = pinned_gprs.pinned_gpr_copies.find(inst);
        copy != pinned_gprs.pinned_gpr_copies.end()) {
        const auto reproved = MatchPinnedGPRCopy(inst);
        ASSERT_MSG(reproved && reproved->read == copy->second.read &&
                           reproved->narrow_extend ==
                                   copy->second.narrow_extend &&
                           reproved->extend == copy->second.extend &&
                           reproved->signed_load == copy->second.signed_load &&
                           reproved->aliases == copy->second.aliases &&
                           reproved->transferred_uses ==
                                   copy->second.transferred_uses &&
                           reproved->source == copy->second.source &&
                           reproved->target == copy->second.target &&
                           reproved->width == copy->second.width &&
                           reproved->last_use == copy->second.last_use,
                   "pinned GPR copy proof diverged at IR {}", inst->Id());
        if (!copy->second.source) {
            return;
        }
        auto source = XRegister(*copy->second.source);
        auto target = XRegister(copy->second.target);
        if (copy->second.width == sizeof(u8)) {
            __ Uxtb(target.W(), source.W());
        } else if (copy->second.width == sizeof(u16)) {
            __ Uxth(target.W(), source.W());
        } else {
            __ Mov(target.W(), source.W());
        }
        return;
    }
    const auto published = inst->GetArg<ir::Value>(0);
    if (auto update = MatchBiasedMemoryUpdate(published.Def());
        update && update->publication == inst) {
        __ Sub(update->base,
               update->base,
               static_cast<u64>(-update->offset));
        return;
    }
    if (auto update = MatchPreIndexMemoryUpdate(published.Def());
        update && update->publication == inst) {
        return;
    }
    if (context.IsHostWriteCoalesced(inst->Id())) {
        if (ReproveCoalescedHostWrite(inst)) {
            return;
        }
        ASSERT_MSG(context.GetFeatures().ra_coalesce_live,
                   "SetHostGPR coalescing proof diverged at IR {}", inst->Id());
    }
    auto offset = inst->GetArg<ir::Imm>(2).Get();
    auto reg_index = inst->GetArg<ir::Imm>(1).Get();
    auto host_reg = XRegister(reg_index);
    auto value = inst->GetArg<ir::Value>(0);
    const bool fused_zext32 = value.Def() && fused_pin_zext32.contains(value.Def());
    const auto residence = guest_state_map.FixedHomeForUse(value, inst);
    auto value_reg = fused_zext32
            ? context.X(value.Def()->GetArg<ir::Value>(0))
            : (residence ? XRegister(residence->home) : context.X(value));
    const auto bit_offset = offset * 8;
    const auto bit_width = ir::GetValueSizeByte(value.Type()) * 8;
    ASSERT_MSG(bit_offset + bit_width <= 64,
               "invalid fixed GPR write offset {} width {}", bit_offset, bit_width);
    const bool pin_ext_reg = IsFixedGPRHome(reg_index);
    if (bit_offset == 0 && bit_width == 32 && pin_ext_reg) {
        const bool adjust_resident_home = residence &&
                value_reg.W() == host_reg.W() &&
                !residence->extension.KnownZeroAbove(32);
        if (value_reg.W() != host_reg.W() || adjust_resident_home) {
            __ Mov(host_reg.W(), value_reg.W());
        }
    } else if (bit_offset == 0 && bit_width == 64) {
        // x86-64 EAX/ECX/EDX writes reach here as a U64
        // ZeroExtend32To64 value so memory-backed StoreUniform can still
        // replace all eight context bytes. For the W55 pinned registers,
        // use the architectural W write: AArch64 clears bits [63:32]
        // naturally, exactly matching the x86 rule.
        const bool zext32 = fused_zext32 || (value.Def() &&
                value.Def()->GetOp() == ir::OpCode::ZeroExtend32To64);
        if (value_reg != host_reg) {
            if (pin_ext_reg && zext32) {
                __ Mov(host_reg.W(), value_reg.W());
            } else {
                __ Mov(host_reg, value_reg);
            }
        } else if (pin_ext_reg && zext32) {
            // A same-register U32->U64 publication still owes the
            // architectural upper-half zeroing: mov wN,wN clears xN[63:32].
            __ Mov(host_reg.W(), host_reg.W());
        }
    } else {
        // Low byte/word and AH/CH/DH (offset == 1) all lower to one BFI,
        // preserving every untouched bit of the pinned 64-bit parent.
        __ Bfi(host_reg, value_reg, bit_offset, bit_width);
    }
}

void JitTranslator::EmitSetHostFPR(ir::Inst* inst) {
    if (auto fusion = scalar_value_fpr_fusions.find(inst);
        fusion != scalar_value_fpr_fusions.end()) {
        ASSERT_MSG(ReproveScalarValueFPRFusion(inst, fusion->second),
                   "scalar FPR value fusion proof diverged at IR {}", inst->Id());
        const auto value = inst->GetArg<ir::Value>(0);
        auto target = VRegister::GetQRegFromCode(fusion->second.target);
        if (CanUseZeroStoreRegister(value)) {
            __ Eor(target.V16B(), target.V16B(), target.V16B());
        } else {
            __ Fmov(target.D(), context.X(value));
        }
        return;
    }
    if (context.IsHostWriteCoalesced(inst->Id())) {
        ASSERT_MSG(ReproveCoalescedHostFPRWrite(inst),
                   "SetHostFPR coalescing proof diverged at IR {}", inst->Id());
        return;
    }
    auto value = inst->GetArg<ir::Value>(0);
    const u32 reg_index = inst->GetArg<ir::Imm>(1).Get();
    const u32 offset = inst->GetArg<ir::Imm>(2).Get();
    const u32 size = ir::GetValueSizeByte(value.Type());
    auto host_reg = VRegister::GetQRegFromCode(reg_index);
    ASSERT_MSG(offset + size <= sizeof(u128) && offset % size == 0,
               "invalid fixed FPR write offset {} size {}", offset, size);

    if (!ir::IsFloatValueType(value.Type())) {
        const bool zero_gpr = CanUseZeroStoreRegister(value);
        Register source = zero_gpr
                ? (size == sizeof(u64) ? Register{xzr} : Register{wzr})
                : context.R(value);
        const u32 lane = offset / size;
        switch (size) {
            case 1: __ Ins(host_reg.V16B(), lane, source.W()); break;
            case 2: __ Ins(host_reg.V8H(), lane, source.W()); break;
            case 4: __ Ins(host_reg.V4S(), lane, source.W()); break;
            case 8: __ Ins(host_reg.V2D(), lane, source.X()); break;
            default: PANIC("unsupported scalar fixed FPR write size {}", size);
        }
        return;
    }

    auto source = context.V(value);
    if (size == sizeof(u128)) {
        ASSERT(offset == 0);
        if (source != host_reg) {
            __ Orr(host_reg.V16B(), source.V16B(), source.V16B());
        }
        return;
    }
    const u32 lane = offset / size;
    switch (size) {
        case 1: __ Ins(host_reg.V16B(), lane, source.V16B(), 0); break;
        case 2: __ Ins(host_reg.V8H(), lane, source.V8H(), 0); break;
        case 4: __ Ins(host_reg.V4S(), lane, source.V4S(), 0); break;
        case 8: __ Ins(host_reg.V2D(), lane, source.V2D(), 0); break;
        default: PANIC("unsupported vector fixed FPR write size {}", size);
    }
}

void JitTranslator::EmitLoadUniform(ir::Inst* inst) {
    auto uni = inst->GetArg<ir::Uniform>(0);
    s32 offset = offsetof(State, uniform_buffer_begin) + uni.GetOffset();
    auto reg = context.Get(inst);
    auto value_type = inst->ReturnType() == ir::ValueType::VOID ? uni.GetType() : inst->ReturnType();
    VisitVariant<void>(reg, [this, value_type, offset] (auto x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Register>) {
            switch (GetValueSizeByte(value_type)) {
                case 1:
                    __ Ldrb(x, MemOperand(state, offset));
                    break;
                case 2:
                    __ Ldrh(x, MemOperand(state, offset));
                    break;
                case 4:
                    __ Ldr(x.W(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Ldr(x, MemOperand(state, offset));
                    break;
            }
        } else if constexpr (std::is_same_v<T, VRegister>) {
            switch (GetValueSizeByte(value_type)) {
                case 1:
                    __ Ldr(x.B(), MemOperand(state, offset));
                    break;
                case 2:
                    __ Ldr(x.H(), MemOperand(state, offset));
                    break;
                case 4:
                    __ Ldr(x.S(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Ldr(x.D(), MemOperand(state, offset));
                    break;
                case 16:
                    __ Ldr(x.Q(), MemOperand(state, offset));
                    break;
            }
        } else {
            PANIC();
        }
    });
}

void JitTranslator::EmitGetUniformAddress(ir::Inst* inst) {
    const auto offset = inst->GetArg<ir::Imm>(0).Get();
    auto result = context.X(ir::Value{inst});
    __ Add(result, state, offsetof(State, uniform_buffer_begin) + offset);
}

void JitTranslator::EmitStoreUniform(ir::Inst* inst) {
    auto uni = inst->GetArg<ir::Uniform>(0);
    s32 offset = offsetof(State, uniform_buffer_begin) + uni.GetOffset();
    const auto value = inst->GetArg<ir::Value>(1);
    ObserveEdgeCarryPolarity(uni, value);
    auto value_type = value.Type();
    const bool zero_gpr = CanUseZeroStoreRegister(value);
    CPUReg reg = zero_gpr
            ? CPUReg{ir::GetValueSizeByte(value_type) == sizeof(u64)
                             ? Register{xzr}
                             : Register{wzr}}
            : context.Get(value);
    VisitVariant<void>(reg, [this, value_type, offset] (auto x) {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, Register>) {
            switch (GetValueSizeByte(value_type)) {
                case 1:
                    __ Strb(x, MemOperand(state, offset));
                    break;
                case 2:
                    __ Strh(x, MemOperand(state, offset));
                    break;
                case 4:
                    __ Str(x.W(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Str(x, MemOperand(state, offset));
                    break;
            }
        } else if constexpr (std::is_same_v<T, VRegister>) {
            switch (GetValueSizeByte(value_type)) {
                case 1:
                    __ Str(x.B(), MemOperand(state, offset));
                    break;
                case 2:
                    __ Str(x.H(), MemOperand(state, offset));
                    break;
                case 4:
                    __ Str(x.S(), MemOperand(state, offset));
                    break;
                case 8:
                    __ Str(x.D(), MemOperand(state, offset));
                    break;
                case 16:
                    __ Str(x.Q(), MemOperand(state, offset));
                    break;
            }
        } else {
            PANIC();
        }
    });
    if (sse_afp_nan &&
        uni.GetOffset() == offsetof(swift::x86::ThreadContext64, mxcsr)) {
        // StoreUniform has no save frame around it. Lease all three operands
        // from the allocator-visible scratch pool so XPOOL cannot place a
        // live guest value in a fixed ip register that this sync clobbers.
        const auto fpcr = context.GetTmpX();
        const auto mxcsr = context.GetTmpX();
        const auto bit = context.GetTmpX();
        EmitSseAFPRestoreGuestFPCRCached(
                masm,
                state,
                0,
                fpcr,
                mxcsr,
                bit);
    }
}

void JitTranslator::EmitLoadLocal(ir::Inst* inst) { PANIC("TODO"); }

void JitTranslator::EmitStoreLocal(ir::Inst* inst) { PANIC("TODO"); }

void JitTranslator::EmitLoadMemory(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = ir::Value{inst};
    auto type = inst->ReturnType();
    const auto pinned_value = ResolvePinnedGPRValue(value);
    auto value_w = [&] {
        return pinned_value ? pinned_value->W() : context.W(value);
    };
    auto value_x = [&] {
        return pinned_value ? *pinned_value : context.X(value);
    };
    ir::Inst* narrow_consumer = nullptr;
    bool direct_narrow_consumer = false;
    if (memory_state.mem_narrow_fuse && ir::GetValueSizeByte(type) <= 2 && inst->GetUses() == 1) {
        auto& list = cur_block->GetInstList();
        auto it = list.iterator_to(*inst);
        const auto adjacent = std::next(it);
        for (++it; it != list.end(); ++it) {
            bool uses_load = false;
            for (auto used : it->GetValues()) {
                uses_load |= used.Def() == inst;
            }
            if (!uses_load) {
                continue;
            }
            const bool extending =
                    (it->GetOp() == ir::OpCode::SignExtend ||
                     it->GetOp() == ir::OpCode::ZeroExtend32) &&
                    it->GetArg<ir::Value>(0).Def() == inst;
            const bool shared = extending &&
                    context.SharesGPR(value, ir::Value{it.operator->()});
            const bool pinned_extension = extending &&
                    fused_pin_sign_extends.contains(it.operator->());
            const bool direct = extending && it == adjacent && !pinned_value &&
                    !context.IsSpilled(ir::Value{it.operator->()});
            if (shared || pinned_extension || direct) {
                narrow_consumer = it.operator->();
                direct_narrow_consumer = direct && !shared && !pinned_extension;
            }
            break;
        }
    }
    // Keep the established computed-address path for generic Q loads and
    // do not consume the synthetic post-index produced by the generic address
    // peephole. A1 opens only the exact bounded W39 Plus form, for which the
    // SIMD register-offset encoding can carry pt + UXTW(guest EA) directly.
    // The ARM64 frontend lowers pair writeback into normal Add + two memory
    // operations, so folding writeback here would update after the first half.
    const bool q_access = type == ir::ValueType::V128;
    const bool structured_guest_ea =
            q_access && StructuredAddressModeEnabled(context.GetFeatures()) &&
            !operand.GetRight().Null();
    const bool fold_host_base =
            HostBaseFoldEligible(memory_state.mem_hostbase_fold,
                                 memory_state.use_memory_base,
                                 memory_state.guest_addr_mask,
                                 type,
                                 structured_guest_ea,
                                 operand.GetOp() == ir::OperandOp::Plus,
                                 false);
    auto load_update = pinned_gprs.pinned_load_updates.find(inst);
    auto vixl_operand = load_update != pinned_gprs.pinned_load_updates.end()
            ? MemOperand{XRegister(load_update->second.target),
                         load_update->second.offset,
                         PreIndex}
            : EmitMemOperand(operand,
                             type,
                             false,
                             q_access && !fold_host_base,
                             !q_access,
                             structured_guest_ea,
                             inst);
    if (load_update != pinned_gprs.pinned_load_updates.end()) {
        const auto reproved = MatchPinnedLoadUpdate(load_update->second.update);
        ASSERT_MSG(reproved && *reproved == load_update->second,
                   "pinned load update proof diverged at IR {}", inst->Id());
    }
    if (direct_narrow_consumer && vixl_operand.GetAddrMode() != Offset &&
        vixl_operand.GetBaseRegister().GetCode() ==
                context.R(ir::Value{narrow_consumer}).GetCode()) {
        narrow_consumer = nullptr;
        direct_narrow_consumer = false;
    }
    auto narrow_value_w = [&] {
        return direct_narrow_consumer ? context.W(ir::Value{narrow_consumer})
                                      : value_w();
    };
    auto narrow_value_x = [&] {
        return direct_narrow_consumer ? context.X(ir::Value{narrow_consumer})
                                      : value_x();
    };
    auto scalar_fpr = scalar_load_fpr_fusions.find(inst);
    if (scalar_fpr != scalar_load_fpr_fusions.end()) {
        ASSERT_MSG(ReproveScalarLoadFPRFusion(inst, scalar_fpr->second),
                   "scalar FPR load fusion proof diverged at IR {}", inst->Id());
        const auto target = VRegister::GetQRegFromCode(scalar_fpr->second.target);
        if (scalar_fpr->second.load_extension) {
            __ Ldr(target.S(), vixl_operand);
        } else {
            __ Ldr(target.D(), vixl_operand);
        }
        return;
    }
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            if (narrow_consumer && narrow_consumer->GetOp() == ir::OpCode::SignExtend) {
                if (ir::GetValueSizeByte(narrow_consumer->ReturnType()) == 8) {
                    __ Ldrsb(narrow_value_x(), vixl_operand);
                } else {
                    __ Ldrsb(narrow_value_w(), vixl_operand);
                }
            } else {
                // LDRB's W destination already performs ZeroExtend32.
                __ Ldrb(narrow_value_w(), vixl_operand);
            }
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            if (narrow_consumer && narrow_consumer->GetOp() == ir::OpCode::SignExtend) {
                if (ir::GetValueSizeByte(narrow_consumer->ReturnType()) == 8) {
                    __ Ldrsh(narrow_value_x(), vixl_operand);
                } else {
                    __ Ldrsh(narrow_value_w(), vixl_operand);
                }
            } else {
                // LDRH's W destination already performs ZeroExtend32.
                __ Ldrh(narrow_value_w(), vixl_operand);
            }
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldr(value_w(), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldr(value_x(), vixl_operand);
            break;
        case ir::ValueType::V8:
            __ Ldr(context.V(value).B(), vixl_operand);
            break;
        case ir::ValueType::V16:
            __ Ldr(context.V(value).H(), vixl_operand);
            break;
        case ir::ValueType::V32:
            __ Ldr(context.V(value).S(), vixl_operand);
            break;
        case ir::ValueType::V64:
            __ Ldr(context.V(value).D(), vixl_operand);
            break;
        case ir::ValueType::V128:
            __ Ldr(context.V(value).Q(), vixl_operand);
            break;
        default:
            PANIC("UnImplement!");
            break;
    }
    if (narrow_consumer) {
        disable_instructions.set(narrow_consumer->Id());
    }
}

void JitTranslator::EmitStoreMemory(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = inst->GetArg<ir::Value>(1);
    auto type = value.Type();
    // See EmitLoadMemory: A1 opens only the exact bounded W39 Plus form; all
    // other Q accesses keep address materialization and explicit writeback.
    const bool q_access = type == ir::ValueType::V128;
    const bool structured_guest_ea =
            q_access && StructuredAddressModeEnabled(context.GetFeatures()) &&
            !operand.GetRight().Null();
    const bool fold_host_base =
            HostBaseFoldEligible(memory_state.mem_hostbase_fold,
                                 memory_state.use_memory_base,
                                 memory_state.guest_addr_mask,
                                 type,
                                 structured_guest_ea,
                                 operand.GetOp() == ir::OperandOp::Plus,
                                 false);
    auto vixl_operand =
            EmitMemOperand(operand,
                           type,
                           false,
                           q_access && !fold_host_base,
                           !q_access,
                           structured_guest_ea,
                           inst);
    if (const auto target = resident_scalar_fpr_analysis.FindMemoryStore(inst)) {
        auto source = VRegister::GetQRegFromCode(*target);
        if (ir::GetValueSizeByte(type) == sizeof(u32)) {
            __ Str(source.S(), vixl_operand);
        } else {
            __ Str(source.D(), vixl_operand);
        }
        return;
    }
    const bool zero_gpr = CanUseZeroStoreRegister(value);
    const auto residence = guest_state_map.FixedHomeForUse(value, inst);
    const auto store_w = [&]() -> WRegister {
        if (zero_gpr) {
            return wzr;
        }
        if (residence && residence->width == ir::GetValueSizeByte(type)) {
            return WRegister(residence->home);
        }
        if (value.Def()) {
            if (auto it = memory_state.pinned_memory_values.find(value.Def());
                it != memory_state.pinned_memory_values.end()) {
                ASSERT_MSG(MatchPinnedMemoryValue(value.Def()) == it->second,
                           "pinned memory value proof drifted before store at IR {}",
                           inst->Id());
                return WRegister(it->second);
            }
            if (auto it = pinned_gprs.fused_pin_gpr_reads.find(value.Def());
                it != pinned_gprs.fused_pin_gpr_reads.end()) {
                return WRegister(it->second);
            }
        }
        return context.W(value);
    };
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Strb(store_w(), vixl_operand);
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Strh(store_w(), vixl_operand);
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Str(store_w(), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Str(zero_gpr ? xzr : context.X(value), vixl_operand);
            break;
        case ir::ValueType::V8:
            __ Str(context.V(value).B(), vixl_operand);
            break;
        case ir::ValueType::V16:
            __ Str(context.V(value).H(), vixl_operand);
            break;
        case ir::ValueType::V32:
            __ Str(context.V(value).S(), vixl_operand);
            break;
        case ir::ValueType::V64:
            __ Str(context.V(value).D(), vixl_operand);
            break;
        case ir::ValueType::V128:
            __ Str(context.V(value).Q(), vixl_operand);
            break;
        default:
            PANIC("UnImplement!");
            break;
    }
}

void JitTranslator::EmitLoadMemoryTSO(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = ir::Value{inst};
    auto type = inst->ReturnType();
    const bool q_access = type == ir::ValueType::V128;
    const bool scalar = type < ir::ValueType::V8 || type > ir::ValueType::V256;
    const bool supports_rcpc =
            True(context.GetConfig().arm64_features & Arm64Features::RCpc);
    const auto size = ir::GetValueSizeByte(type);
    const bool static_address =
            operand.GetRight().Null() && operand.GetLeft().IsImm();
    const auto static_bias = reinterpret_cast<uintptr_t>(
            context.GetConfig().memory_base ? context.GetConfig().memory_base
                                            : context.GetConfig().page_table);
    const bool statically_unaligned =
            static_address && size > 1 &&
            ((operand.GetLeft().imm.Get() + static_bias) & (size - 1)) != 0;
    const bool scalar_fast_path =
            scalar && supports_rcpc && !statically_unaligned;
    const bool check_alignment =
            scalar_fast_path && size > 1 && !static_address;
    tso_emission_stats.Increment(tso_emission_stats.load_sites);
    if (scalar_fast_path) {
        tso_emission_stats.Increment(tso_emission_stats.scalar_fast_sites);
    }
    if (check_alignment) {
        tso_emission_stats.Increment(tso_emission_stats.alignment_check_sites);
    }
    auto vixl_operand =
            EmitMemOperand(operand,
                           type,
                           false,
                           scalar_fast_path || q_access,
                           !scalar_fast_path && !q_access,
                           false,
                           inst);

    // FEAT_LRCPC's LDAPR is the scalar x86-TSO fast path. It requires a bare,
    // naturally aligned address, so materialize any offset and branch around
    // it for x86's permitted unaligned accesses. Byte accesses are naturally
    // aligned by definition. Hosts without LRCPC and vector accesses retain
    // the proven basic-load + dmb ishld half-barrier.
    if (scalar_fast_path) {
        Register address = vixl_operand.GetBaseRegister();
        if (!vixl_operand.IsImmediateOffset() || vixl_operand.GetOffset() != 0) {
            __ ComputeAddress(mem_scratch, vixl_operand);
            address = mem_scratch;
        }

        Label unaligned;
        Label done;
        if (check_alignment) {
            __ Tst(address, size - 1);
            __ B(&unaligned, ne);
        }
        {
            vixl::CPUFeaturesScope rcpc(&masm, vixl::CPUFeatures::kRCpc);
            switch (type) {
                case ir::ValueType::S8:
                case ir::ValueType::U8:
                    __ Ldaprb(context.W(value), MemOperand(address));
                    break;
                case ir::ValueType::S16:
                case ir::ValueType::U16:
                    __ Ldaprh(context.W(value), MemOperand(address));
                    break;
                case ir::ValueType::S32:
                case ir::ValueType::U32:
                    __ Ldapr(context.W(value), MemOperand(address));
                    break;
                case ir::ValueType::S64:
                case ir::ValueType::U64:
                    __ Ldapr(context.X(value), MemOperand(address));
                    break;
                default:
                    PANIC("unsupported scalar TSO load width");
            }
        }
        if (size == 1) {
            return;
        }
        if (!check_alignment) {
            return;
        }
        __ B(&done);
        __ Bind(&unaligned);
        switch (type) {
            case ir::ValueType::S16:
            case ir::ValueType::U16:
                __ Ldrh(context.W(value), MemOperand(address));
                break;
            case ir::ValueType::S32:
            case ir::ValueType::U32:
                __ Ldr(context.W(value), MemOperand(address));
                break;
            case ir::ValueType::S64:
            case ir::ValueType::U64:
                __ Ldr(context.X(value), MemOperand(address));
                break;
            default:
                PANIC("unsupported unaligned scalar TSO load width");
        }
        tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
        __ Dmb(InnerShareable, BarrierReads);
        __ Bind(&done);
        return;
    }

    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldrb(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldrh(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldr(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldr(context.X(value), vixl_operand);
            break;
        case ir::ValueType::V8:
            __ Ldr(context.V(value).B(), vixl_operand);
            break;
        case ir::ValueType::V16:
            __ Ldr(context.V(value).H(), vixl_operand);
            break;
        case ir::ValueType::V32:
            __ Ldr(context.V(value).S(), vixl_operand);
            break;
        case ir::ValueType::V64:
            __ Ldr(context.V(value).D(), vixl_operand);
            break;
        case ir::ValueType::V128:
            __ Ldr(context.V(value).Q(), vixl_operand);
            break;
        default:
            PANIC("UnImplement!");
            break;
    }
    // Acquire half: no later load/store may be observed before this one.
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierReads);
}

void JitTranslator::EmitStoreMemoryTSO(ir::Inst* inst) {
    auto operand = inst->GetArg<ir::Operand>(0);
    auto value = inst->GetArg<ir::Value>(1);
    auto type = value.Type();
    const bool q_access = type == ir::ValueType::V128;
    const bool scalar = type < ir::ValueType::V8 || type > ir::ValueType::V256;
    const bool supports_rcpc =
            True(context.GetConfig().arm64_features & Arm64Features::RCpc);
    const auto size = ir::GetValueSizeByte(type);
    const bool static_address =
            operand.GetRight().Null() && operand.GetLeft().IsImm();
    const auto static_bias = reinterpret_cast<uintptr_t>(
            context.GetConfig().memory_base ? context.GetConfig().memory_base
                                            : context.GetConfig().page_table);
    const bool statically_unaligned =
            static_address && size > 1 &&
            ((operand.GetLeft().imm.Get() + static_bias) & (size - 1)) != 0;
    const bool scalar_fast_path =
            scalar && supports_rcpc && !statically_unaligned;
    const bool check_alignment =
            scalar_fast_path && size > 1 && !static_address;
    tso_emission_stats.Increment(tso_emission_stats.store_sites);
    if (scalar_fast_path) {
        tso_emission_stats.Increment(tso_emission_stats.scalar_fast_sites);
    }
    if (check_alignment) {
        tso_emission_stats.Increment(tso_emission_stats.alignment_check_sites);
    }
    auto vixl_operand =
            EmitMemOperand(operand,
                           type,
                           false,
                           scalar_fast_path || q_access,
                           !scalar_fast_path && !q_access,
                           false,
                           inst);

    // Gate the complete scalar fast path with the same check as LDAPR. This
    // keeps non-LRCPC hosts on the previous dmb+str implementation and makes
    // SVM_ARM64_LRCPC=0 an exact A/B baseline.
    if (scalar_fast_path) {
        Register address = vixl_operand.GetBaseRegister();
        if (!vixl_operand.IsImmediateOffset() || vixl_operand.GetOffset() != 0) {
            __ ComputeAddress(mem_scratch, vixl_operand);
            address = mem_scratch;
        }

        Label unaligned;
        Label done;
        if (check_alignment) {
            __ Tst(address, size - 1);
            __ B(&unaligned, ne);
        }
        switch (type) {
            case ir::ValueType::S8:
            case ir::ValueType::U8:
                __ Stlrb(context.W(value), MemOperand(address));
                break;
            case ir::ValueType::S16:
            case ir::ValueType::U16:
                __ Stlrh(context.W(value), MemOperand(address));
                break;
            case ir::ValueType::S32:
            case ir::ValueType::U32:
                __ Stlr(context.W(value), MemOperand(address));
                break;
            case ir::ValueType::S64:
            case ir::ValueType::U64:
                __ Stlr(context.X(value), MemOperand(address));
                break;
            default:
                PANIC("unsupported scalar TSO store width");
        }
        if (size == 1) {
            return;
        }
        if (!check_alignment) {
            return;
        }
        __ B(&done);
        __ Bind(&unaligned);
        tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
        __ Dmb(InnerShareable, BarrierAll);
        switch (type) {
            case ir::ValueType::S16:
            case ir::ValueType::U16:
                __ Strh(context.W(value), MemOperand(address));
                break;
            case ir::ValueType::S32:
            case ir::ValueType::U32:
                __ Str(context.W(value), MemOperand(address));
                break;
            case ir::ValueType::S64:
            case ir::ValueType::U64:
                __ Str(context.X(value), MemOperand(address));
                break;
            default:
                PANIC("unsupported unaligned scalar TSO store width");
        }
        __ Bind(&done);
        return;
    }

    // Vector stores have no release form on the baseline used here.
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Strb(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Strh(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Str(context.W(value), vixl_operand);
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Str(context.X(value), vixl_operand);
            break;
        case ir::ValueType::V8:
            __ Str(context.V(value).B(), vixl_operand);
            break;
        case ir::ValueType::V16:
            __ Str(context.V(value).H(), vixl_operand);
            break;
        case ir::ValueType::V32:
            __ Str(context.V(value).S(), vixl_operand);
            break;
        case ir::ValueType::V64:
            __ Str(context.V(value).D(), vixl_operand);
            break;
        case ir::ValueType::V128:
            __ Str(context.V(value).Q(), vixl_operand);
            break;
        default:
            PANIC("UnImplement!");
            break;
    }
}

void JitTranslator::EmitMemoryCopy(ir::Inst* inst) {
    auto dst = inst->GetArg<ir::Lambda>(0);
    auto src = inst->GetArg<ir::Lambda>(1);
    const auto size = inst->GetArg<ir::Imm>(2).Get();

    if (size == 0) {
        return;
    }

    MergeNZCV(FlagsRegsAuditMergeCause::Helper,
              FlagsRegsAuditEdgeKind::Host);
    FlushFlags();

    // A memory copy is rare but needs true memmove overlap semantics. Call a
    // host helper after preserving every register the host ABI may clobber.
    // Saving all SIMD registers is deliberate: unlike a normal C++ caller,
    // this JIT can keep a live guest Q value in any V register, including the
    // ABI-volatile range.
    constexpr u32 kGprSaveBytes = 160;
    constexpr u32 kVRegSaveBytes = 32 * 16;
    constexpr u32 kSaveBytes = kGprSaveBytes + kVRegSaveBytes;
    static_assert((kSaveBytes % 16) == 0);

    auto saved_gpr_offset = [](u32 code) -> u32 {
        if (code <= 10) {
            return code * 8;
        }
        switch (code) {
            case 12: return 88;
            case 13: return 96;
            case 14: return 104;
            case 15: return 112;
            case 16: return 120;
            case 17: return 128;
            default: PANIC();
        }
    };
    auto load_lambda = [&](const ir::Lambda& lambda, const XRegister& target) {
        if (!lambda.IsValue()) {
            __ Mov(target, lambda.GetImm().Get());
            return;
        }
        auto value = lambda.GetValue();
        auto source = context.X(value);
        if (source.GetCode() <= 10 || (source.GetCode() >= 12 && source.GetCode() <= 17)) {
            __ Ldr(target, MemOperand(sp, saved_gpr_offset(source.GetCode())));
        } else {
            __ Mov(target, source);
        }
    };

    __ Sub(sp, sp, kSaveBytes);
    __ Stp(x0, x1, MemOperand(sp, 0));
    __ Stp(x2, x3, MemOperand(sp, 16));
    __ Stp(x4, x5, MemOperand(sp, 32));
    __ Stp(x6, x7, MemOperand(sp, 48));
    __ Stp(x8, x9, MemOperand(sp, 64));
    __ Stp(x10, x12, MemOperand(sp, 80));
    __ Stp(x13, x14, MemOperand(sp, 96));
    __ Stp(x15, x16, MemOperand(sp, 112));
    __ Str(x17, MemOperand(sp, 128));
    __ Stp(x29, x30, MemOperand(sp, 144));
    for (u32 i = 0; i < 32; ++i) {
        __ Str(VRegister::GetVRegFromCode(i).Q(), MemOperand(sp, kGprSaveBytes + i * 16));
    }

    load_lambda(dst, x0);
    load_lambda(src, x1);
    if (memory_state.use_memory_base) {
        EmitGuestToHost(x0, x0);
        EmitGuestToHost(x1, x1);
    }
    __ Mov(x2, size);
    if (sse_afp_nan) {
        __ Ldr(ip0,
               MemOperand(sp, kSaveBytes + kSseAFPHostFPCROffset));
        __ Msr(FPCR, ip0);
    }
    __ Mov(ip, reinterpret_cast<uintptr_t>(&HostMemMove));
    __ Blr(ip);
    if (sse_afp_nan) {
        EmitSseAFPRestoreGuestFPCRCached(
                masm,
                state,
                kSaveBytes,
                ip,
                ip0,
                ip1);
    }

    for (u32 i = 0; i < 32; ++i) {
        __ Ldr(VRegister::GetVRegFromCode(i).Q(), MemOperand(sp, kGprSaveBytes + i * 16));
    }
    __ Ldp(x0, x1, MemOperand(sp, 0));
    __ Ldp(x2, x3, MemOperand(sp, 16));
    __ Ldp(x4, x5, MemOperand(sp, 32));
    __ Ldp(x6, x7, MemOperand(sp, 48));
    __ Ldp(x8, x9, MemOperand(sp, 64));
    __ Ldp(x10, x12, MemOperand(sp, 80));
    __ Ldp(x13, x14, MemOperand(sp, 96));
    __ Ldp(x15, x16, MemOperand(sp, 112));
    __ Ldr(x17, MemOperand(sp, 128));
    __ Ldp(x29, x30, MemOperand(sp, 144));
    __ Add(sp, sp, kSaveBytes);
}

void JitTranslator::EmitMemoryCopyTSO(ir::Inst* inst) {
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
    EmitMemoryCopy(inst);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitMemoryBarrierTSO(ir::Inst* inst) {
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitCompareAndSwap(ir::Inst* inst) {
    // Args: (address, expected, desired); returns the old value.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    auto expected = inst->GetArg<ir::Value>(1);
    auto desired = inst->GetArg<ir::Value>(2);
    auto type = expected.Type();
    auto result = context.R(ir::Value{inst});

    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              flags_audit_block_edge);

    // Exclusive instructions take a base register only (no offset forms), so
    // under guest address virtualization the pt bias must be folded in
    // explicitly (reserved scratch: CAS is VOID-adjacent and GetTmpX cannot
    // be trusted here — see defines.h mem_scratch).
    if (memory_state.use_memory_base) {
        EmitGuestToHost(mem_scratch, address);
        address = mem_scratch;
    }

    Label aligned;
    Label retry;
    Label done;
    const bool lse = CanUseLSE();
    if (!lse) {
        tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
        __ Dmb(InnerShareable, BarrierAll);
    }

    if (ir::GetValueSizeByte(type) > 1) {
        __ Tst(address, ir::GetValueSizeByte(type) - 1);
        __ B(&aligned, eq);
        if (auto* fallback = GetUnalignedAtomicFallback(inst)) {
            Label fallback_return;
            __ Adr(atomic_pair_scratch, &fallback_return);
            __ B(fallback);
            __ Bind(&fallback_return);
        } else {
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
            LoadUnalignedAtomicLockAddress(atomic_pair_scratch);
            AcquireUnalignedAtomicLock(atomic_pair_scratch, result);
            EmitBasicAtomicLoad(type, result, address);
            Label cas_done;
            __ Cmp(result, context.R(expected, true));
            __ B(&cas_done, ne);
            EmitBasicAtomicStore(type, context.R(desired, true), address);
            __ Bind(&cas_done);
            ReleaseUnalignedAtomicLock(atomic_pair_scratch);
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
        }
        __ B(&done);
    }

    __ Bind(&aligned);
    if (lse) {
        EmitLSECompareAndSwap(type,
                              result,
                              context.R(expected, true),
                              context.R(desired, true),
                              address);
        __ Bind(&done);
        return;
    }
    __ Bind(&retry);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldaxrb(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldaxrh(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldaxr(result.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldaxr(result, MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
    __ Cmp(result, context.R(expected, true));
    __ B(&done, ne);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Stlxrb(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Stlxrh(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Stlxr(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Stlxr(ipw, context.X(desired), MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
    __ Cbnz(ipw, &retry);
    __ Bind(&done);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitCompareAndSwap128(ir::Inst* inst) {
    // Args: (address, expected_lo, expected_hi, desired_lo, desired_hi);
    // returns the observed pair as V128, low half in lane 0.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    const auto expected_lo = inst->GetArg<ir::Value>(1);
    const auto expected_hi = inst->GetArg<ir::Value>(2);
    const auto desired_lo = inst->GetArg<ir::Value>(3);
    const auto desired_hi = inst->GetArg<ir::Value>(4);
    const auto result = context.V(ir::Value{inst});

    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              flags_audit_block_edge);
    if (memory_state.use_memory_base) {
        EmitGuestToHost(mem_scratch, address);
        address = mem_scratch;
    }

    Label aligned;
    Label retry;
    Label fallback_no_store;
    Label aligned_observed;
    Label done;
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);

    // The locked frontend rejects misalignment before this instruction. The
    // no-LOCK form is architecturally legal when unaligned (confirmed under
    // Rosetta), so preserve it with a serialized basic pair load/store.
    __ Tst(address, 15);
    __ B(&aligned, eq);
    LoadUnalignedAtomicLockAddress(atomic_scratch);
    AcquireUnalignedAtomicLock(atomic_scratch, atomic_pair_scratch);
    __ Ldp(atomic_scratch, atomic_pair_scratch, MemOperand(address));
    __ Cmp(atomic_scratch, context.X(expected_lo));
    __ B(&fallback_no_store, ne);
    __ Cmp(atomic_pair_scratch, context.X(expected_hi));
    __ B(&fallback_no_store, ne);
    __ Stp(context.X(desired_lo), context.X(desired_hi), MemOperand(address));
    __ Bind(&fallback_no_store);
    __ Ins(result.V2D(), 0, atomic_scratch);
    __ Ins(result.V2D(), 1, atomic_pair_scratch);
    LoadUnalignedAtomicLockAddress(atomic_scratch);
    ReleaseUnalignedAtomicLock(atomic_scratch);
    __ B(&done);

    __ Bind(&aligned);
    __ Bind(&retry);
    __ Ldaxp(atomic_scratch, atomic_pair_scratch, MemOperand(address));
    __ Cmp(atomic_scratch, context.X(expected_lo));
    __ B(&aligned_observed, ne);
    __ Cmp(atomic_pair_scratch, context.X(expected_hi));
    __ B(&aligned_observed, ne);
    __ Stlxp(ipw,
             context.X(desired_lo),
             context.X(desired_hi),
             MemOperand(address));
    __ Cbnz(ipw, &retry);
    __ B(&aligned_observed);

    __ Bind(&aligned_observed);
    // Clear a still-live monitor after a compare mismatch. It is harmless
    // after a successful STLXP and keeps the exclusive state local to this op.
    __ Clrex();
    __ Ins(result.V2D(), 0, atomic_scratch);
    __ Ins(result.V2D(), 1, atomic_pair_scratch);

    __ Bind(&done);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitAtomicExchange(ir::Inst* inst) {
    // Args: (address, desired); returns the previous value. Memory XCHG is
    // implicitly locked on x86, so use an unconditional exclusive loop and
    // full barriers regardless of the configured ordinary-memory TSO mode.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    const auto desired = inst->GetArg<ir::Value>(1);
    const auto type = desired.Type();
    const auto result = context.R(ir::Value{inst});

    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              flags_audit_block_edge);
    if (memory_state.use_memory_base) {
        EmitGuestToHost(mem_scratch, address);
        address = mem_scratch;
    }

    Label aligned;
    Label retry;
    Label done;
    const bool lse = CanUseLSE();
    if (!lse) {
        tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
        __ Dmb(InnerShareable, BarrierAll);
    }

    if (ir::GetValueSizeByte(type) > 1) {
        __ Tst(address, ir::GetValueSizeByte(type) - 1);
        __ B(&aligned, eq);
        if (auto* fallback = GetUnalignedAtomicFallback(inst)) {
            Label fallback_return;
            __ Adr(atomic_pair_scratch, &fallback_return);
            __ B(fallback);
            __ Bind(&fallback_return);
        } else {
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
            LoadUnalignedAtomicLockAddress(atomic_pair_scratch);
            AcquireUnalignedAtomicLock(atomic_pair_scratch, result);
            EmitBasicAtomicLoad(type, result, address);
            EmitBasicAtomicStore(type, context.R(desired, true), address);
            ReleaseUnalignedAtomicLock(atomic_pair_scratch);
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
        }
        __ B(&done);
    }

    __ Bind(&aligned);
    if (lse) {
        EmitLSEExchange(type, result, context.R(desired, true), address);
        __ Bind(&done);
        return;
    }
    __ Bind(&retry);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldaxrb(result.W(), MemOperand(address));
            __ Stlxrb(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldaxrh(result.W(), MemOperand(address));
            __ Stlxrh(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldaxr(result.W(), MemOperand(address));
            __ Stlxr(ipw, context.W(desired), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldaxr(result, MemOperand(address));
            __ Stlxr(ipw, context.X(desired), MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
    __ Cbnz(ipw, &retry);
    __ Bind(&done);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitAtomicFetchAdd(ir::Inst* inst) {
    // Args: (address, addend); returns the previous value. Used by LOCK XADD.
    auto address = context.X(inst->GetArg<ir::Value>(0));
    const auto addend = inst->GetArg<ir::Value>(1);
    const auto type = addend.Type();
    const auto result = context.R(ir::Value{inst});

    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              flags_audit_block_edge);
    if (memory_state.use_memory_base) {
        EmitGuestToHost(mem_scratch, address);
        address = mem_scratch;
    }

    Label aligned;
    Label retry;
    Label done;
    const bool lse = CanUseLSE();
    if (!lse) {
        tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
        __ Dmb(InnerShareable, BarrierAll);
    }

    if (ir::GetValueSizeByte(type) > 1) {
        __ Tst(address, ir::GetValueSizeByte(type) - 1);
        __ B(&aligned, eq);
        if (auto* fallback = GetUnalignedAtomicFallback(inst)) {
            Label fallback_return;
            __ Adr(atomic_pair_scratch, &fallback_return);
            __ B(fallback);
            __ Bind(&fallback_return);
        } else {
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
            LoadUnalignedAtomicLockAddress(atomic_pair_scratch);
            AcquireUnalignedAtomicLock(atomic_pair_scratch, result);
            EmitBasicAtomicLoad(type, result, address);
            if (ir::GetValueSizeByte(type) == 8) {
                __ Add(atomic_scratch, result, context.X(addend));
            } else {
                __ Add(atomic_scratch.W(), result.W(), context.W(addend));
            }
            EmitBasicAtomicStore(type, atomic_scratch, address);
            ReleaseUnalignedAtomicLock(atomic_pair_scratch);
            if (lse) {
                tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
                __ Dmb(InnerShareable, BarrierAll);
            }
        }
        __ B(&done);
    }

    __ Bind(&aligned);
    if (lse) {
        EmitLSEFetchAdd(type, result, context.R(addend, true), address);
        __ Bind(&done);
        return;
    }
    __ Bind(&retry);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldaxrb(result.W(), MemOperand(address));
            __ Add(atomic_scratch.W(), result.W(), context.W(addend));
            __ Stlxrb(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldaxrh(result.W(), MemOperand(address));
            __ Add(atomic_scratch.W(), result.W(), context.W(addend));
            __ Stlxrh(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldaxr(result.W(), MemOperand(address));
            __ Add(atomic_scratch.W(), result.W(), context.W(addend));
            __ Stlxr(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldaxr(result, MemOperand(address));
            __ Add(atomic_scratch, result, context.X(addend));
            __ Stlxr(ipw, atomic_scratch, MemOperand(address));
            break;
        default:
            PANIC("UnImplement!");
    }
    __ Cbnz(ipw, &retry);
    __ Bind(&done);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitAtomicRMW(ir::Inst* inst) {
    // Args: (operation, address, operand, carry); returns the previous value.
    const auto op =
            static_cast<ir::AtomicRMWOp>(inst->GetArg<ir::Imm>(0).Get());
    auto address = context.X(inst->GetArg<ir::Value>(1));
    const auto operand = inst->GetArg<ir::Value>(2);
    const auto carry = inst->GetArg<ir::Value>(3);
    const auto type = operand.Type();
    const auto result = context.R(ir::Value{inst});

    MergeNZCV(FlagsRegsAuditMergeCause::PStateClobber,
              flags_audit_block_edge);
    if (memory_state.use_memory_base) {
        EmitGuestToHost(mem_scratch, address);
        address = mem_scratch;
    }

    Label aligned;
    Label retry;
    Label done;
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);

    if (ir::GetValueSizeByte(type) > 1) {
        __ Tst(address, ir::GetValueSizeByte(type) - 1);
        __ B(&aligned, eq);
        LoadUnalignedAtomicLockAddress(atomic_pair_scratch);
        AcquireUnalignedAtomicLock(atomic_pair_scratch, result);
        EmitBasicAtomicLoad(type, result, address);
        EmitAtomicRMWValue(op, type, atomic_scratch, result, operand, carry);
        EmitBasicAtomicStore(type, atomic_scratch, address);
        ReleaseUnalignedAtomicLock(atomic_pair_scratch);
        __ B(&done);
    }

    __ Bind(&aligned);
    __ Bind(&retry);
    switch (type) {
        case ir::ValueType::S8:
        case ir::ValueType::U8:
            __ Ldaxrb(result.W(), MemOperand(address));
            EmitAtomicRMWValue(op, type, atomic_scratch, result, operand, carry);
            __ Stlxrb(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S16:
        case ir::ValueType::U16:
            __ Ldaxrh(result.W(), MemOperand(address));
            EmitAtomicRMWValue(op, type, atomic_scratch, result, operand, carry);
            __ Stlxrh(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S32:
        case ir::ValueType::U32:
            __ Ldaxr(result.W(), MemOperand(address));
            EmitAtomicRMWValue(op, type, atomic_scratch, result, operand, carry);
            __ Stlxr(ipw, atomic_scratch.W(), MemOperand(address));
            break;
        case ir::ValueType::S64:
        case ir::ValueType::U64:
            __ Ldaxr(result, MemOperand(address));
            EmitAtomicRMWValue(op, type, atomic_scratch, result, operand, carry);
            __ Stlxr(ipw, atomic_scratch, MemOperand(address));
            break;
        default:
            PANIC("unsupported AtomicRMW width");
    }
    __ Cbnz(ipw, &retry);
    __ Bind(&done);
    tso_emission_stats.Increment(tso_emission_stats.dmb_instructions);
    __ Dmb(InnerShareable, BarrierAll);
}

void JitTranslator::EmitUniformBarrier(ir::Inst* inst) {
    // Compiler barrier only: uniform caching is not invalidated across this point.
}

}  // namespace swift::runtime::backend::arm64
