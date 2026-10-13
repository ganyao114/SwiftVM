#include "sparse_homes_plan.h"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::loongarch64 {
namespace {
using O = ir::OpCode;
using R = SparseHomeReason;

u32 Bytes(ir::ValueType type) {
    // GetValueSizeByte deliberately rejects VOID. BindLabel's self-reference
    // is metadata rather than a data value and has this return annotation.
    return type == ir::ValueType::VOID ? 0 : ir::GetValueSizeByte(type);
}

bool EarlyResult(ir::Inst* inst) {
    return inst->GetOp() == O::VecSatMulQC || inst->GetOp() == O::VecSatShiftQC ||
           inst->GetOp() == O::Div128Remainder || inst->GetOp() == O::CpuidUpper;
}

template<class F> void Operands(ir::Inst* inst, const F& visit) {
    // Operand's physical slots already contain its Value members. Lambda
    // addresses and Params lists need a separate walk, as in RegisterPlan.
    for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
        auto& arg = inst->ArgAt(slot);
        if (arg.IsValue()) visit(arg.Get<ir::Value>());
        else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) visit(arg.Get<ir::Lambda>().GetValue());
        else if (arg.IsParams()) for (const auto& parameter : arg.Get<ir::Params>())
            if (parameter.data.IsValue()) visit(parameter.data.value);
    }
}

bool MemoryWidth(ir::Inst* inst, bool store) {
    // A memory Operand occupies three physical slots; StoreUniform's first
    // argument occupies one. GetArg uses public rather than physical indices.
    const u32 stored_slot = inst->GetOp() == O::StoreUniform ? 1 : 3;
    if (store && !inst->ArgAt(stored_slot).IsValue()) return false;
    const auto type = store ? inst->GetArg<ir::Value>(1).Type() : inst->ReturnType();
    const u32 bytes = Bytes(type);
    return bytes && bytes <= SparseHomesPlan::kStride;
}

void ValidateType(ir::ValueType type) {
    if (type >= ir::ValueType::V256)
        throw std::invalid_argument("LoongArch sparse homes require legalized, valid 128-bit IR");
}

}  // namespace

bool SparseHomesPlan::MayCallTypedHelper(ir::Inst* inst) {
    if (!inst) throw std::invalid_argument("LoongArch sparse helper classifier needs an instruction");
    switch (inst->GetOp()) {
        // These emitters have a return-width guard before their native path.
        case O::LoadImm: case O::GetOperand:
        case O::Add: case O::Sub: case O::Adc: case O::Sbb: case O::Mul:
        case O::MulSub: case O::MulHigh: case O::Div:
        case O::And: case O::Or: case O::Xor: case O::Not: case O::AndNot: case O::Neg:
        case O::LslImm: case O::LsrImm: case O::AsrImm: case O::RorImm:
        case O::LslValue: case O::LsrValue: case O::AsrValue: case O::RorValue:
            return !ScalarAllocationPlan::Eligible(inst);
        case O::Zero: case O::BitCast: case O::GetResult:
        case O::Select: case O::SelectZero: case O::CondSelect:
            return Bytes(inst->ReturnType()) > kStride;
        case O::LoadUniform: case O::LoadMemory: case O::LoadMemoryTSO:
            return !MemoryWidth(inst, false);
        case O::StoreUniform: case O::StoreMemory: case O::StoreMemoryTSO:
            return !MemoryWidth(inst, true);
        // Audited unconditional native scalar/control emitters. Their calls
        // to context.Read/Write use assigned registers or existing spills.
        case O::SignedDiv64: case O::ZeroExtend32: case O::ZeroExtend32To64:
        case O::ZeroExtend64: case O::SignExtend:
        case O::TestZero: case O::TestNotZero: case O::TestBit:
        case O::BitExtract: case O::BitInsert: case O::BitClear: case O::ByteSwap:
        case O::CountLeadingZeros64: case O::CountTrailingZeros64:
        case O::CountLeadingZeros32: case O::CountTrailingZeros32:
        case O::PopCount: case O::LocalParitySet: case O::FCmpCondSet:
        case O::Crc32: case O::Crc32c:
        case O::CondSet: case O::LocalCondSet: case O::GetFlags:
        case O::SaveFlags: case O::BranchOnlyFlags: case O::TestFlags: case O::TestNotFlags:
        case O::ClearFlags: case O::SetCarry: case O::SetOverflow: case O::InvertCarry:
        case O::PublishFCmpFlags: case O::PublishSse42StrFlags:
        case O::Goto: case O::NotGoto: case O::BindLabel: case O::AddPhi:
        case O::AdvancePC: case O::SetLocation: case O::GetLocation:
        case O::Nop: case O::UniformBarrier: case O::XchgBarrier:
        case O::PushRSB: case O::PopRSB: case O::CallReturn:
        case O::GetUniformAddress: case O::MemoryBarrierTSO: case O::CheckMemoryAlignment:
            return false;
        // All vector/FP paths are deliberately conservative, including the
        // baseline raw-pair implementations. Scalar atomics retain their
        // complete typed inputs/results for alignment and feature fallbacks.
        // New opcodes inherit the same fail-closed behavior.
        default: return true;
    }
}

std::optional<u16> SparseHomesPlan::HomeId(ir::Inst* inst) const {
    const auto found = homes.find(inst);
    return found == homes.end() ? std::nullopt : std::optional{found->second};
}

u32 SparseHomesPlan::HomeOffset(ir::Inst* inst, u32 part) const {
    if (part > 1) throw std::out_of_range("invalid LoongArch SSA home half");
    const auto id = HomeId(inst);
    if (!id) throw std::runtime_error("missing LoongArch SSA home");
    return u32(*id) * kStride + part * sizeof(u64);
}

u32 SparseHomesPlan::Reasons(ir::Inst* inst) const {
    const auto found = reasons.find(inst);
    return found == reasons.end() ? 0 : found->second;
}

SparseHomesPlan SparseHomesPlan::Build(ir::Block* block, const ScalarAllocationPlan& allocation,
                                      const RegisterPlan::Exits& exits, const ir::Terminal* terminal) {
    if (!block) throw std::invalid_argument("LoongArch sparse homes need an IR block");
    SparseHomesPlan result;
    std::vector<ir::Inst*> instructions;
    LiveSet members;
    for (auto& inst : block->GetInstList()) {
        ValidateType(inst.ReturnType());
        instructions.push_back(&inst); members.insert(&inst);
    }
    const auto member = [&](ir::Inst* inst) {
        if (!inst || !members.contains(inst))
            throw std::invalid_argument("foreign LoongArch sparse-home instruction");
    };
    const auto require = [&](ir::Inst* inst, R reason) {
        member(inst); result.required.insert(inst); result.reasons[inst] |= u32(reason);
    };
    const auto value = [&](ir::Value input) {
        member(input.Def()); ValidateType(input.Type());
        // Wide views of scalar carriers still use their second home half.
        if (Bytes(input.Type()) > sizeof(u64)) require(input.Def(), R::RawContainer);
    };
    for (auto* inst : instructions) {
        Operands(inst, value);
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst->ArgAt(slot);
            if (arg.GetType() == ir::ArgType::Uniform) ValidateType(arg.Get<ir::Uniform>().GetType());
            else if (arg.GetType() == ir::ArgType::Local) ValidateType(arg.Get<ir::Local>().type);
            else if (arg.IsImm()) ValidateType(arg.Get<ir::Imm>().GetType());
        }
        if (allocation.Spilled(inst)) require(inst, R::Spill);
        if (ir::IsFloatValueType(inst->ReturnType())) require(inst, R::Vector);
        if (inst->GetOp() == O::AddPhi) require(inst, R::PhiDestination);
        if (EarlyResult(inst)) require(inst, R::ProducerPseudo);
        // Foreign pseudos must be rejected even if the lexical marker is
        // missing: typed producers can write their results immediately.
        if (!inst->IsPseudoOperation())
            for (auto* pseudo : inst->GetPseudoOperations()) member(pseudo);
        if (MayCallTypedHelper(inst)) {
            // Keep VOID ownhomes too. A typed helper's implementation may
            // write a raw result/pseudo independent of its return annotation.
            require(inst, R::HelperInstruction);
            Operands(inst, [&](ir::Value input) { require(input.Def(), R::HelperInput); });
            if (!inst->IsPseudoOperation())
                for (auto* pseudo : inst->GetPseudoOperations()) require(pseudo, R::ProducerPseudo);
        }
        if (inst->GetOp() == O::BitCast || inst->GetOp() == O::GetResult) {
            // CopyValue always preserves both halves, even with U8 return.
            require(inst, R::RawContainer);
            require(inst->GetArg<ir::Value>(0).Def(), R::RawContainer);
        }
        if ((inst->GetOp() == O::Select || inst->GetOp() == O::SelectZero || inst->GetOp() == O::CondSelect) &&
            ir::IsFloatValueType(inst->ReturnType())) {
            require(inst, R::RawContainer);
            require(inst->GetArg<ir::Value>(1).Def(), R::RawContainer);
            require(inst->GetArg<ir::Value>(2).Def(), R::RawContainer);
        }
    }
    // Terminals consume typed scalar registers directly. Visit all nested
    // exits to validate foreign/wide operands, without forcing register-only
    // predicates or switch values to acquire a home.
    const auto terminal_values = [&](const auto& recurse, const ir::Terminal& exit) -> void {
        VisitVariant<void>(exit, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) {
                value(term.cond); recurse(recurse, term.then_); recurse(recurse, term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                recurse(recurse, term.then_); recurse(recurse, term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                value(term.value);
                for (const auto& item : term.cases) recurse(recurse, item.then);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) recurse(recurse, term.else_);
        });
    };
    terminal_values(terminal_values, terminal ? *terminal : block->GetTerminal());
    for (const auto& [at, exit] : exits) { member(at); terminal_values(terminal_values, exit); }

    // UINT16_MAX remains an invalid/sentinel ID. Exactly 65535 homes fit,
    // with valid IDs [0,65534]; a 65536th home must reject compilation.
    if (result.required.size() > ir::Inst::invalid_id)
        throw std::overflow_error("too many LoongArch sparse SSA homes");
    u32 next{};
    for (auto* inst : instructions) if (result.required.contains(inst)) {
        result.homes.emplace(inst, static_cast<u16>(next++));
        const auto why = result.reasons.at(inst);
        result.stats.spill_homes += bool(why & u32(R::Spill));
        result.stats.vector_homes += bool(why & u32(R::Vector));
        result.stats.helper_homes += bool(why & (u32(R::HelperInstruction) | u32(R::HelperInput)));
        result.stats.pseudo_homes += bool(why & u32(R::ProducerPseudo));
        result.stats.raw_homes += bool(why & u32(R::RawContainer));
        result.stats.phi_homes += bool(why & u32(R::PhiDestination));
    }
    result.stats.instructions = instructions.size();
    result.stats.required_homes = next;
    result.stats.home_count = std::max(next, u32{1});
    result.stats.dense_bytes = u64(std::max(instructions.size(), size_t{1})) * kStride;
    result.stats.compact_bytes = u64(result.stats.home_count) * kStride;
    return result;
}

}  // namespace swift::runtime::backend::loongarch64
