#include "zero_store_verifier.h"
#include <unordered_map>

namespace swift::runtime::backend::arm64 {
namespace {
class ZeroStoreVerifier {
public:
    ZeroStoreVerifier(const BlockAnalysisIndex& a, const RegAlloc& r) : analysis(a), allocation(r) {}
    std::vector<u32> Verify(std::vector<const ir::Inst*>* incoming) {
        std::vector<u32> verified;
        for (auto* inst : analysis.Instructions()) {
            const auto op = inst->GetOp();
            if (!inst->HasValue() || (op != ir::OpCode::LoadImm && !Extension(inst))) continue;
            if (op == ir::OpCode::LoadImm && inst->GetArg<ir::Imm>(0).Get() != 0) continue;
            if (allocation.ValueType(ir::Value{inst}) != RegAlloc::MEM && Zero(inst) && Stores(inst)) verified.push_back(inst->Id());
        }
        if (incoming) analysis.ForEachUsedDefinition([&](ir::Inst* inst) {
            if (!inst->HasValue()) return;
            const auto op = inst->GetOp();
            if (op != ir::OpCode::LoadImm && !Extension(inst)) return;
            if (op == ir::OpCode::LoadImm && inst->GetArg<ir::Imm>(0).Get() != 0) return;
            if (analysis.Contains(inst)) return;
            if (allocation.ValueType(ir::Value{inst}) != RegAlloc::MEM && Zero(inst) && Stores(inst)) incoming->push_back(inst);
        });
        return verified;
    }
private:
    enum class State { Visiting, Accepted, Rejected };
    static bool Extension(const ir::Inst* inst) {
        return inst->GetOp() == ir::OpCode::ZeroExtend32 || inst->GetOp() == ir::OpCode::ZeroExtend32To64;
    }
    bool Zero(ir::Inst* inst) {
        if (const auto found = zero_memo.find(inst); found != zero_memo.end()) return found->second == State::Accepted;
        zero_memo.emplace(inst, State::Visiting);
        bool accepted = inst->GetOp() == ir::OpCode::LoadImm && inst->GetArg<ir::Imm>(0).Get() == 0;
        if (Extension(inst)) {
            const auto source = inst->GetArg<ir::Value>(0);
            accepted = source.Defined() && !ir::IsFloatValueType(source.Type()) && Zero(source.Def());
        }
        zero_memo[inst] = accepted ? State::Accepted : State::Rejected;
        return accepted;
    }
    bool Stores(ir::Inst* inst) {
        if (const auto found = uses_memo.find(inst); found != uses_memo.end()) return found->second == State::Accepted;
        uses_memo.emplace(inst, State::Visiting);
        u32 compatible = 0;
        for (const auto& use : analysis.Uses(inst)) {
            auto* user = use.consumer;
            const auto op = user->GetOp();
            if ((op == ir::OpCode::StoreUniform || op == ir::OpCode::StoreMemory) && user->GetArg<ir::Value>(1).Def() == inst)
                ++compatible;
            else if (op == ir::OpCode::SetHostFPR && user->GetArg<ir::Value>(0).Def() == inst) ++compatible;
            else if (Extension(user) && user->GetArg<ir::Value>(0).Def() == inst &&
                     allocation.ValueType(ir::Value{user}) != RegAlloc::MEM && Zero(user) && Stores(user)) ++compatible;
        }
        // Count once per consumer. An address/data duplicate must reject.
        const bool accepted = compatible != 0 && compatible == inst->GetUses(false);
        uses_memo[inst] = accepted ? State::Accepted : State::Rejected;
        return accepted;
    }
    const BlockAnalysisIndex& analysis;
    const RegAlloc& allocation;
    std::unordered_map<ir::Inst*, State> zero_memo, uses_memo;
};
}
std::vector<u32> VerifyZeroStoreValues(const BlockAnalysisIndex& analysis, const RegAlloc& allocation,
                                     std::vector<const ir::Inst*>* incoming) {
    return ZeroStoreVerifier(analysis, allocation).Verify(incoming);
}
}
