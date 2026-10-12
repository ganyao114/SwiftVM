#include "single_instruction_admission.h"

#include <span>
#include <unordered_map>
#include "interpreter.h"
#include "runtime/common/crc32.h"
#include "runtime/common/saturating_add_sub.h"
#include "runtime/common/saturating_multiply.h"
#include "runtime/common/saturating_shift.h"
#include "runtime/common/unsigned_estimate.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::interp {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
using A = ir::ArgType;
using Positions = std::unordered_map<const ir::Inst*, size_t>;

bool Scalar(T type) { return type >= T::U8 && type <= T::S64; }
bool Container(T type) { return type >= T::U8 && type <= T::V128; }
bool Lane(u64 bits) { return bits == 8 || bits == 16 || bits == 32 || bits == 64; }
u64 Imm(const ir::Inst& inst, int slot) { return inst.ArgAt(slot).Get<ir::Imm>().Get(); }
T View(const ir::Inst& inst, int slot) { return inst.ArgAt(slot).Get<ir::Value>().Type(); }
bool Canonical(const ir::Inst& inst, int slot, T type) {
    const auto value = inst.ArgAt(slot).Get<ir::Value>();
    return value.Type() == type && value.Def()->ReturnType() == type;
}

bool Value(ir::Value value, const Positions& prior, bool label = false) {
    const auto* definition = value.Def();
    if (!definition || !prior.contains(definition)) return false;
    // Only a label marker may refer to a non-data branch value.
    return label || (Container(value.Type()) && Container(definition->ReturnType()));
}

std::span<const A> Signature(O op) {
    switch (op) {
#define INST(name, ...) case O::name: return ir::meta::kArgs_##name;
#include "runtime/ir/ir.inc"
#undef INST
        default: return {};
    }
}

bool Shape(const ir::Inst& inst, const Positions& prior) {
    const auto args = Signature(inst.GetOp());
    int slot{};
    for (const auto expected : args) {
        // The unary logical Not is the only admitted optional-operand form.
        if (inst.GetOp() == O::Not && slot == 1 && inst.ArgAt(slot).IsVoid()) break;
        auto& arg = inst.ArgAt(slot);
        if (arg.GetType() != expected) return false;
        if (expected == A::Value) {
            if (!Value(arg.Get<ir::Value>(), prior, inst.GetOp() == O::BindLabel)) return false;
        } else if (expected == A::Imm) {
            if (!Scalar(arg.Get<ir::Imm>().GetType())) return false;
        } else if (expected == A::Lambda) {
            const auto lambda = arg.Get<ir::Lambda>();
            if (lambda.IsValue()) {
                if (!Value(lambda.GetValue(), prior) || !Scalar(lambda.GetValue().Type())) return false;
            } else if (!lambda.IsImm() || !Scalar(lambda.GetImm().GetType())) return false;
        } else if (expected == A::Cond) {
            if (static_cast<u8>(arg.Get<ir::Cond>()) > static_cast<u8>(ir::Cond::NV)) return false;
        } else if (expected == A::Flags) {
            if ((static_cast<u64>(arg.Get<ir::Flags>()) & ~static_cast<u64>(ir::Flags::All)) != 0) return false;
        } else if (expected == A::Operand) {
            const auto operation = arg.Get<ir::OperandOp>();
            if (operation.type != ir::OperandOp::None && operation.type != ir::OperandOp::Plus &&
                operation.type != ir::OperandOp::LSL && operation.type != ir::OperandOp::LSR &&
                operation.type != ir::OperandOp::PlusExt) return false;
            if (operation.shift_ext >= 64) return false;
            for (int side = 1; side <= 2; ++side) {
                auto& data = inst.ArgAt(slot + side);
                if (data.IsVoid()) { if (side == 1) return false; }
                else if (data.IsValue()) {
                    if (!Value(data.Get<ir::Value>(), prior) || !Scalar(data.Get<ir::Value>().Type())) return false;
                } else if (!data.IsImm() || !Scalar(data.Get<ir::Imm>().GetType())) return false;
            }
        } else if (expected != A::Uniform) {
            // Local and Params carry storage/linked data not admitted here.
            return false;
        }
        slot += ir::PhysicalSlots(expected);
    }
    for (; slot < ir::Inst::max_args; ++slot) if (!inst.ArgAt(slot).IsVoid()) return false;
    return true;
}

bool ScalarInputs(const ir::Inst& inst) {
    for (int slot = 0; slot < ir::Inst::max_args; ++slot)
        if (inst.ArgAt(slot).IsValue() && !Scalar(View(inst, slot))) return false;
    return true;
}

bool NativeOpcode(const ir::Inst& inst, size_t uniform_bytes) {
    const auto ret = inst.ReturnType();
    const bool scalar = Scalar(ret);
    // Deliberately positive list: unimplemented stubs, memory accesses,
    // optimizer/function-only operations and future additions fail closed.
    switch (inst.GetOp()) {
        case O::LoadUniform: case O::StoreUniform: {
            const auto uniform = inst.ArgAt(0).Get<ir::Uniform>();
            if (!Container(uniform.GetType())) return false;
            const auto type = inst.GetOp() == O::LoadUniform ? ret : View(inst, 1);
            if (type != uniform.GetType()) return false;
            const size_t size = ir::GetValueSizeByte(type), offset = uniform.GetOffset();
            return offset <= uniform_bytes && size <= uniform_bytes - offset;
        }
        case O::GetUniformAddress:
            return ret == T::U64 && Imm(inst, 0) < uniform_bytes;
        case O::CallLambda:
            // The frontend admission must establish fixed target identity,
            // exact metadata and context/word/PC provenance independently.
            return ret == T::U64 && inst.ArgAt(0).Get<ir::Lambda>().IsImm() &&
                   View(inst, 1) == T::U64 && View(inst, 2) == T::U64 && View(inst, 3) == T::U64;
        case O::Nop: case O::UniformBarrier: case O::PushRSB: case O::PopRSB:
        case O::SetLocation: case O::CallReturn: case O::BindLabel:
            return ret == T::VOID;
        case O::Goto: case O::NotGoto:
            return ScalarInputs(inst) && (ret == T::VOID || scalar);
        case O::GetLocation:
            return ret == T::U64;
        case O::LoadImm:
            return scalar;
        case O::Zero: case O::BitCast:
            return Container(ret);
        case O::Select: case O::CondSelect:
            return Container(ret) && (inst.GetOp() == O::CondSelect || Scalar(View(inst, 0)));
        case O::SelectZero:
            return Container(ret) && Scalar(View(inst, 0));
        case O::GetOperand:
            return scalar;
        case O::Add: case O::Sub: case O::Adc: case O::Sbb: case O::Mul:
        case O::MulSub: case O::Div: case O::And: case O::Or: case O::Xor:
        case O::Not: case O::AndNot: case O::Neg:
        case O::ZeroExtend32: case O::ZeroExtend32To64: case O::ZeroExtend64: case O::SignExtend:
        case O::LslImm: case O::LslValue: case O::LsrImm: case O::LsrValue:
        case O::AsrImm: case O::AsrValue: case O::RorImm: case O::RorValue:
            return scalar && ScalarInputs(inst);
        case O::SignedDiv64:
            return ret == T::U64 && View(inst, 0) == T::U64 && View(inst, 1) == T::U64;
        case O::MulHigh:
            return ret == T::U64 && ScalarInputs(inst) && Imm(inst, 2) <= 1;
        case O::CountLeadingZeros64: case O::CountTrailingZeros64:
            return ret == T::U64 && View(inst, 0) == T::U64;
        case O::CountLeadingZeros32: case O::CountTrailingZeros32:
            return ret == T::U32 && View(inst, 0) == T::U32;
        case O::PopCount:
            return ret == T::U64 && ScalarInputs(inst);
        case O::ByteSwap:
            return scalar && ScalarInputs(inst) && (Imm(inst, 1) == 16 || Imm(inst, 1) == 32 || Imm(inst, 1) == 64);
        case O::BitExtract: case O::BitClear:
            return scalar && ScalarInputs(inst) && Imm(inst, 1) < 64 && Imm(inst, 2) > 0 &&
                   Imm(inst, 2) <= 64 - Imm(inst, 1);
        case O::BitInsert:
            return scalar && ScalarInputs(inst) && Imm(inst, 2) < 64 && Imm(inst, 3) > 0 &&
                   Imm(inst, 3) <= 64 - Imm(inst, 2);
        case O::TestBit:
            return ret == T::U8 && ScalarInputs(inst) && Imm(inst, 1) < 64;
        case O::TestZero: case O::TestNotZero:
            return ret == T::U8 && ScalarInputs(inst);
        case O::CondSet: case O::LocalCondSet: case O::TestFlags: case O::TestNotFlags:
            return ret == T::U8;
        case O::GetFlags:
            return scalar && ScalarInputs(inst);
        case O::SaveFlags:
            return ret == T::VOID && ScalarInputs(inst);
        case O::ClearFlags: case O::InvertCarry:
            return ret == T::VOID;
        case O::SetCarry: case O::SetOverflow:
            return ret == T::VOID && ScalarInputs(inst);
        case O::Crc32:
            (void)ValidateIeeeCrc32(const_cast<ir::Inst*>(&inst));
            return true;
        case O::Crc32c:
            return ret == T::U32 && ScalarInputs(inst) && Lane(Imm(inst, 2));
        case O::VecXor: case O::VecAnd: case O::VecOr: case O::VecAndNot:
        case O::Vec4And: case O::Vec4Or:
        case O::VecAbsDiffSum8: case O::VecMadd16: case O::VecTableLookup8:
        case O::VecShuffle32Indexed:
        case O::VecAesEnc: case O::VecAesEncLast: case O::VecAesDec: case O::VecAesDecLast:
        case O::VecSha256Msg1: case O::VecSha256Msg2: case O::VecSha256Rnds2:
            return Container(ret);
        case O::VecAdd: case O::VecSub: case O::VecCmpEq: case O::VecCmpGt: case O::VecMul:
            return Container(ret) && Lane(Imm(inst, 2));
        case O::VecAvg:
            return Container(ret) && (Imm(inst, 2) == 8 || Imm(inst, 2) == 16);
        case O::VecMin: case O::VecMax:
            return Container(ret) && Lane(Imm(inst, 2)) && Imm(inst, 3) <= 1;
        case O::VecDot:
            return ret == T::V128 && Canonical(inst, 0, T::V128) && Canonical(inst, 1, T::V128) &&
                   Canonical(inst, 2, T::V128) && Imm(inst, 3) <= 1;
        case O::VecSatMulHigh:
            (void)ValidateSaturatingMultiply(const_cast<ir::Inst*>(&inst)); return true;
        case O::VecSatMulQC:
            (void)ValidateSaturatingMultiplyQC(const_cast<ir::Inst*>(&inst)); return true;
        case O::VecSatShiftImm:
            (void)ValidateSaturatingShift(const_cast<ir::Inst*>(&inst)); return true;
        case O::VecSatShiftQC:
            (void)ValidateSaturatingShiftQC(const_cast<ir::Inst*>(&inst)); return true;
        case O::VecUnsignedEstimate:
            (void)ValidateUnsignedEstimate(const_cast<ir::Inst*>(&inst)); return true;
        case O::VecMulHigh16:
            return Container(ret) && Imm(inst, 2) <= 1;
        case O::VecMulWiden:
            return Container(ret) && (Imm(inst, 2) == 8 || Imm(inst, 2) == 16 || Imm(inst, 2) == 32) && Imm(inst, 3) <= 1;
        case O::VecSatAdd: case O::VecSatSub:
            (void)ValidateSaturatingAddSub(const_cast<ir::Inst*>(&inst)); return true;
        case O::VecPack:
            return Container(ret) && (Imm(inst, 2) == 8 || Imm(inst, 2) == 16) && Imm(inst, 3) <= 1;
        case O::VecShiftLeft: case O::VecShiftRight: case O::VecShiftRightArithmetic:
            return Container(ret) && Scalar(View(inst, 1)) && Lane(Imm(inst, 2));
        case O::VecShiftLeftImm: case O::VecShiftRightImm: case O::VecShiftRightArithmeticImm:
            return Container(ret) && Lane(Imm(inst, 2));
        case O::VecByteShift:
            return Container(ret) && Imm(inst, 2) > 0 && Imm(inst, 2) < 16 && Imm(inst, 3) <= 1;
        case O::VecShuffle32:
            return Container(ret) && Imm(inst, 1) <= 255;
        case O::VecShuffle32TwoSrc:
            return Container(ret) && Imm(inst, 2) <= 255;
        case O::VecLoadConst:
            return ret == T::V128;
        case O::VecSharedZero:
            return Container(ret);
        case O::VecShuffle16:
            return Container(ret) && Imm(inst, 1) <= 255 && Imm(inst, 2) <= 1;
        case O::VecZip: case O::VecUnzip:
            return Container(ret) && Lane(Imm(inst, 2)) && Imm(inst, 3) <= 1;
        case O::VecDupPairs32:
            return Container(ret) && Imm(inst, 1) <= 1;
        case O::VecExtractBytes:
            return Container(ret) && Imm(inst, 2) > 0 && Imm(inst, 2) < 16;
        case O::VecDup64:
            return Container(ret) && Scalar(View(inst, 0));
        case O::VecExtract64:
            return scalar && Imm(inst, 1) <= 1;
        case O::VecExtract16:
            return scalar && Imm(inst, 1) <= 7;
        case O::VecInsert16:
            return Container(ret) && Scalar(View(inst, 1)) && Imm(inst, 2) <= 7;
        case O::VecMovMask:
            return scalar && (Imm(inst, 1) == 8 || Imm(inst, 1) == 32 || Imm(inst, 1) == 64);
        case O::VecAesKeygenAssist:
            return Container(ret) && Imm(inst, 1) <= 255;
        case O::VecPclMul:
            return Container(ret) && (Imm(inst, 2) & ~u64{0x11}) == 0;
        default:
            return false;
    }
}

bool Terminal(const ir::Terminal& terminal, const Positions& all, unsigned depth = 0) {
    if (depth > 64) return false;
    return VisitVariant<bool>(terminal, [&](const auto& term) {
        using U = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<U, ir::terminal::ReturnToDispatch> ||
                      std::is_same_v<U, ir::terminal::ReturnToHost> ||
                      std::is_same_v<U, ir::terminal::LinkBlock> ||
                      std::is_same_v<U, ir::terminal::LinkBlockFast> ||
                      std::is_same_v<U, ir::terminal::ExternalLinkBlock> ||
                      std::is_same_v<U, ir::terminal::PopRSBHint>) return true;
        else if constexpr (std::is_same_v<U, ir::terminal::If>)
            return Value(term.cond, all) && Scalar(term.cond.Type()) &&
                   Terminal(term.then_, all, depth + 1) && Terminal(term.else_, all, depth + 1);
        else if constexpr (std::is_same_v<U, ir::terminal::Condition>)
            return static_cast<u8>(term.cond) <= static_cast<u8>(ir::Cond::NV) &&
                   Terminal(term.then_, all, depth + 1) && Terminal(term.else_, all, depth + 1);
        else if constexpr (std::is_same_v<U, ir::terminal::CheckHalt>)
            return Terminal(term.else_, all, depth + 1);
        else return false; // Invalid and Switch cannot prove retirement here.
    });
}
}  // namespace

bool SingleInstructionAdmitted(const ir::Block& block, size_t uniform_bytes) {
    const auto& hoist = block.GetLoopHoistMetadata();
    if (!uniform_bytes || !block.HasTerminal() || hoist.prefix_end || !hoist.anchors.empty() ||
        hoist.gpr_count || hoist.const_count || block.HasDeadEdgeIntegerBranchProof() ||
        !const_cast<ir::Block&>(block).GetUniformCaptureRecipes().empty()) return false;
    try {
        Positions prior, targets;
        size_t ordinal{};
        for (const auto& inst : block.GetInstList()) {
            // Bound the private SSA stack and require dense, unique homes.
            if (ordinal >= 8192 || inst.Id() != ordinal || !Shape(inst, prior) ||
                !NativeOpcode(inst, uniform_bytes)) return false;
            if (inst.GetOp() == O::BindLabel) {
                const auto* branch = inst.ArgAt(0).Get<ir::Value>().Def();
                if ((branch->GetOp() != O::Goto && branch->GetOp() != O::NotGoto) ||
                    !targets.emplace(branch, ordinal).second) return false;
            }
            prior.emplace(&inst, ordinal++);
        }
        for (const auto& inst : block.GetInstList()) {
            if (inst.GetOp() == O::Goto || inst.GetOp() == O::NotGoto) {
                const auto target = targets.find(&inst);
                if (target == targets.end() || target->second <= prior.at(&inst)) return false;
            }
            // Producers eagerly fill pseudo homes. A pseudo from another
            // block must never index this private SSA buffer.
            if (inst.GetOp() == O::VecSatMulHigh || inst.GetOp() == O::VecSatShiftImm) {
                const auto pseudo_op = inst.GetOp() == O::VecSatMulHigh ? O::VecSatMulQC : O::VecSatShiftQC;
                for (const auto* pseudo : const_cast<ir::Inst&>(inst).GetPseudoOperations(pseudo_op))
                    if (!prior.contains(pseudo) || prior.at(pseudo) <= prior.at(&inst)) return false;
            }
        }
        return Terminal(block.GetTerminal(), prior);
    } catch (const std::exception&) {
        return false;
    }
}

bool Interpreter::ValidateSingle(const ir::Block& block, size_t uniform_bytes) {
    return SingleInstructionAdmitted(block, uniform_bytes);
}
}  // namespace swift::runtime::backend::interp

