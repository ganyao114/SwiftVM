#include "cfg_uniform_dead_store_pc.h"
#include "translator.h"

#include <algorithm>
#include <limits>
#include <unordered_map>
#include <vector>

namespace swift::runtime::backend::x64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
bool Scalar(T type) { return type >= T::U8 && type <= T::S64; }
bool Tail(ir::Inst* inst, u32 first) {
    for (u32 slot = first; slot < ir::Inst::max_args; ++slot)
        if (!inst->ArgAt(slot).IsVoid()) return false;
    return true;
}
bool ScalarValue(ir::Inst* inst, u32 slot) {
    if (!inst->ArgAt(slot).IsValue()) return false;
    const auto value = inst->ArgAt(slot).Get<ir::Value>();
    return value.Defined() && Scalar(value.Type()) && Scalar(value.Def()->ReturnType());
}
bool PurePC(ir::Inst* inst) {
    if (inst->GetOp() != O::AdvancePC || inst->ReturnType() != T::VOID ||
        !inst->ArgAt(0).IsImm() || !Tail(inst,1)) return false;
    const auto imm = inst->GetArg<ir::Imm>(0);
    // Restrict this experiment to normal A64 decode-progress markers.
    return (imm.GetType() == T::U32 || imm.GetType() == T::U64) &&
           imm.Get() != 0 && (imm.Get() & 3) == 0;
}
bool PureNative(ir::Inst* inst) {
    if (inst->GetOp() == O::Nop) return inst->ReturnType() == T::VOID && Tail(inst,0);
    if (!Scalar(inst->ReturnType())) return false;
    switch (inst->GetOp()) {
        case O::LoadImm:
            return inst->ArgAt(0).IsImm() && Scalar(inst->GetArg<ir::Imm>(0).GetType()) && Tail(inst,1);
        case O::Add: case O::Sub: case O::And: case O::Or: case O::Xor: case O::AndNot: {
            if (!ScalarValue(inst,0) || !inst->ArgAt(1).IsOperand() ||
                !inst->ArgAt(3).IsVoid() || !Tail(inst,4)) return false;
            const auto op = inst->GetArg<ir::Operand>(1).GetOp();
            if (op.shift_ext || (op.type != ir::OperandOp::None && op.type != ir::OperandOp::Plus)) return false;
            return ScalarValue(inst,2) ||
                (inst->ArgAt(2).IsImm() && Scalar(inst->ArgAt(2).Get<ir::Imm>().GetType()));
        }
        case O::Neg: case O::ZeroExtend32: case O::ZeroExtend32To64:
        case O::ZeroExtend64: case O::SignExtend:
        case O::CountLeadingZeros32: case O::CountLeadingZeros64:
        case O::CountTrailingZeros32: case O::CountTrailingZeros64:
        case O::TestZero: case O::TestNotZero:
            return ScalarValue(inst,0) && Tail(inst,1);
        default: return false;
    }
}
bool MetadataFence(ir::Inst* inst, const backend::FunctionPlan& function) {
    return function.locations.contains(inst) || function.halt_checks.contains(inst) ||
        function.exits.contains(inst) || function.stops.contains(inst) ||
        function.targets.contains(inst) || function.synthetic_labels.contains(inst);
}
u32 Width(T type) {
    return type >= T::U8 && type <= T::V128 ? ir::GetValueSizeByte(type) : 0;
}
bool Range(u32 byte, u32 bytes, u32 extent) {
    return bytes && byte <= extent && bytes <= extent - byte &&
        u64{state_offset_uniform_buffer} + byte + bytes - 1 <= u64{std::numeric_limits<s32>::max()};
}
bool Overlap(u32 a, u32 bytes, u32 b) {
    return u64{a} < u64{b} + 8 && u64{b} < u64{a} + bytes;
}
struct Pending { ir::Inst* store{}; u32 pc_epoch{}; };
struct Snapshot { ir::Inst* store{}; ir::Uniform uniform; ir::Value value; bool crosses_pc{}; };
struct Transaction {
    const std::vector<Snapshot>& stores;
    size_t changed{};
    bool committed{};
    ~Transaction() {
        if (committed) return;
        for (size_t i = 0; i < changed; ++i) {
            const auto& old = stores[i]; old.store->DestroyArgs();
            old.store->SetInst(O::StoreUniform,old.uniform,old.value); old.store->SetReturn(T::VOID);
        }
    }
    void Rewrite() {
        for (const auto& old : stores) {
            old.store->DestroyArgs(); old.store->SetInst(O::Nop); old.store->SetReturn(T::VOID); ++changed;
        }
    }
};
} // namespace

CfgUniformDeadStorePcStats EliminateOverwrittenUniformStoresAcrossPC(
    backend::FunctionPlan& function, const Config& config, ExecutionMode mode,
    CfgUniformDeadStorePcOptions options) {
    CfgUniformDeadStorePcStats stats;
    if (!options.enabled) return stats;
    stats.attempted = true;
    const auto reject = [&](CfgUniformDeadStorePcRejection reason) {
        stats.accepted = false; stats.rejection = reason;
        stats.stores_removed = stats.removals_crossing_pc = 0; return stats;
    };
    if (mode != ExecutionMode::Ordinary || !config.enable_jit || config.backend_isa != kX86_64)
        return reject(CfgUniformDeadStorePcRejection::UnsupportedMode);
    auto* body = function.body.get();
    const auto graph = CfgRegisterPlan::ValidateGraph(body,function);
    if (!graph.Enabled()) return reject(CfgUniformDeadStorePcRejection::InvalidGraph);
    if (graph.Instructions().size() > options.max_instructions)
        return reject(CfgUniformDeadStorePcRejection::Budget);
    const auto extent = config.uniform_buffer_size;
    std::vector<Snapshot> overwritten;
    std::unordered_map<u32,Pending> pending;
    const auto fence = [&] { pending.clear(); ++stats.fences; };
    const auto kill = [&](u32 byte, u32 bytes, u32& count) {
        std::erase_if(pending,[&](const auto& candidate) {
            if (!Overlap(byte,bytes,candidate.first)) return false;
            ++count; return true;
        });
    };
    for (const auto& block : graph.Blocks()) {
        pending.clear(); u32 pc_epoch{};
        if (!block.reachable) continue;
        for (u32 pos = block.begin; pos < block.end; ++pos) {
            auto* inst = graph.Instructions()[pos];
            // No proof crosses metadata, labels/CFG edges or injected Phi
            // transport: BindLabel/branch are fences and blocks reset pending.
            if (MetadataFence(inst,function)) { fence(); continue; }
            if (PurePC(inst)) { ++pc_epoch; ++stats.pure_advance_pc_sites; continue; }
            if (inst->GetOp() == O::LoadUniform) {
                if (inst->ArgAt(0).GetType() != ir::ArgType::Uniform || !Tail(inst,1)) { fence(); continue; }
                const auto uniform = inst->GetArg<ir::Uniform>(0);
                const auto bytes = Width(uniform.GetType());
                if (!Range(uniform.GetOffset(),bytes,extent)) return reject(CfgUniformDeadStorePcRejection::InvalidAccess);
                kill(uniform.GetOffset(),bytes,stats.load_kills); continue;
            }
            if (inst->GetOp() == O::StoreUniform) {
                ++stats.stores_seen;
                if (inst->ArgAt(0).GetType() != ir::ArgType::Uniform || !inst->ArgAt(1).IsValue() ||
                    !Tail(inst,2) || inst->ReturnType() != T::VOID || inst->GetUses(false)) { fence(); continue; }
                const auto uniform = inst->GetArg<ir::Uniform>(0);
                const auto value = inst->GetArg<ir::Value>(1);
                const auto bytes = Width(value.Type());
                if (!value.Defined() || !Range(uniform.GetOffset(),bytes,extent)) return reject(CfgUniformDeadStorePcRejection::InvalidAccess);
                const bool eligible = uniform.GetType() == T::U64 && value.Type() == T::U64 && value.Def()->ReturnType() == T::U64;
                if (eligible) {
                    ++stats.candidates;
                    if (const auto old = pending.find(uniform.GetOffset()); old != pending.end()) {
                        auto* previous = old->second.store;
                        overwritten.push_back({previous,previous->GetArg<ir::Uniform>(0),previous->GetArg<ir::Value>(1),old->second.pc_epoch != pc_epoch});
                    }
                }
                kill(uniform.GetOffset(),bytes,stats.overlapping_store_kills);
                if (eligible) pending.emplace(uniform.GetOffset(),Pending{inst,pc_epoch});
                if (overwritten.size() > options.max_candidates || pending.size() > options.max_candidates)
                    return reject(CfgUniformDeadStorePcRejection::Budget);
                continue;
            }
            if (!PureNative(inst)) fence();
        }
    }
    Transaction transaction{overwritten};
    try {
        transaction.Rewrite();
        if (!CfgRegisterPlan::ValidateGraph(body,function).Enabled()) return reject(CfgUniformDeadStorePcRejection::InvalidResult);
        stats.accepted = true; stats.rejection = CfgUniformDeadStorePcRejection::None;
        stats.stores_removed = overwritten.size();
        stats.removals_crossing_pc = std::count_if(overwritten.begin(),overwritten.end(),[](const auto& old) { return old.crosses_pc; });
        transaction.committed = true; return stats;
    } catch (...) { return reject(CfgUniformDeadStorePcRejection::InvalidResult); }
}
} // namespace swift::runtime::backend::x64
