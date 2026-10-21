//
// Created by 甘尧 on 2023/9/8.
//

#include <cmath>
#include "runtime/common/logging.h"
#include "runtime/ir/ir_meta.h"
#include "runtime/ir/ir_types.h"

namespace swift::runtime::ir {

// Signature tokens now live in meta::tokens (ir_meta.h); the runtime table below
// is a pure projection of the constexpr per-instruction facts generated there.
using namespace meta::tokens;

namespace {
// Every opcode must be assigned deliberately. Adding an opcode without an
// effect contract fails compilation, rather than silently treating it as pure.
constexpr auto BuildEffects() {
    using E = IREffect;
    std::array<E, static_cast<size_t>(OpCode::BASE_COUNT)> effects{};
    effects.fill(E::Unknown);
    const auto assign = [&](E effect, std::initializer_list<OpCode> ops) {
        for (auto op : ops) effects[static_cast<size_t>(op)] = effect;
    };
    assign(E::None, {
        OpCode::Void, OpCode::Nop, OpCode::Zero, OpCode::GetUniformAddress,
        OpCode::Select, OpCode::SelectZero, OpCode::Sse42Str, OpCode::AddPhi,
        OpCode::BitCast, OpCode::GetOperand, OpCode::GetResult, OpCode::LoadImm,
        OpCode::Add, OpCode::Sub, OpCode::Mul, OpCode::MulSub, OpCode::Div,
        OpCode::SignedDiv64, OpCode::Div128, OpCode::Div128Remainder,
        OpCode::Cpuid, OpCode::CpuidUpper,
        OpCode::ZeroExtend32, OpCode::ZeroExtend32To64, OpCode::ZeroExtend64,
        OpCode::SignExtend, OpCode::And, OpCode::Or, OpCode::Xor, OpCode::Not,
        OpCode::AndNot, OpCode::GetFlags,
        OpCode::LslImm, OpCode::LslValue, OpCode::LsrImm, OpCode::LsrValue,
        OpCode::AsrImm, OpCode::AsrValue, OpCode::RorImm, OpCode::RorValue,
        OpCode::ByteSwap, OpCode::CountLeadingZeros64, OpCode::CountTrailingZeros64,
        OpCode::CountLeadingZeros32, OpCode::CountTrailingZeros32,
        OpCode::BitExtract, OpCode::BitInsert, OpCode::BitClear,
        OpCode::TestBit, OpCode::TestZero, OpCode::TestNotZero,
        OpCode::Vec4Add, OpCode::Vec4Sub, OpCode::Vec4Mul, OpCode::Vec4And,
        OpCode::Vec4Or, OpCode::VecXor, OpCode::VecAnd, OpCode::VecOr, OpCode::VecAndNot,
        OpCode::VecAdd, OpCode::VecSub, OpCode::VecCmpEq, OpCode::VecCmpGt,
        OpCode::VecAvg, OpCode::VecMin, OpCode::VecMax, OpCode::VecMul,
        OpCode::VecMulHigh16, OpCode::VecMulWiden, OpCode::VecSatAdd, OpCode::VecSatSub,
        OpCode::VecPack, OpCode::VecAbsDiffSum8, OpCode::VecMadd16,
        OpCode::VecShiftLeft, OpCode::VecShiftRight, OpCode::VecShiftRightArithmetic,
        OpCode::VecShiftLeftImm, OpCode::VecShiftRightImm, OpCode::VecShiftRightArithmeticImm,
        OpCode::VecByteShift, OpCode::VecShuffle32, OpCode::VecShuffle32TwoSrc,
        OpCode::VecLoadConst, OpCode::VecShuffle32Indexed, OpCode::VecSharedZero,
        OpCode::VecShuffle16, OpCode::VecExtractBytes, OpCode::VecZip, OpCode::VecDupPairs32,
        OpCode::VecDup64, OpCode::VecExtract64, OpCode::VecExtract16, OpCode::VecInsert16,
        OpCode::VecMovMask, OpCode::VecTableLookup8,
        OpCode::VecAesEnc, OpCode::VecAesEncLast, OpCode::VecAesDec, OpCode::VecAesDecLast,
        OpCode::VecAesEncFast, OpCode::VecAesEncLastFast, OpCode::VecAesDecFast,
        OpCode::VecAesDecLastFast, OpCode::VecAesKeygenAssist, OpCode::VecPclMul,
        OpCode::VecSha256Msg1, OpCode::VecSha256Msg2, OpCode::VecSha256Rnds2,
        OpCode::VecUnzip, OpCode::MulHigh, OpCode::LocalParitySet,
        OpCode::Neg, OpCode::PopCount, OpCode::Crc32c,
    });
    assign(E::ReadFPEnvironment, {
        OpCode::VecFAddScalar32, OpCode::VecFSubScalar32, OpCode::VecFMulScalar32,
        OpCode::VecFDivScalar32, OpCode::VecFAddScalar64, OpCode::VecFSubScalar64,
        OpCode::VecFMulScalar64, OpCode::VecFDivScalar64, OpCode::VecFAdd,
        OpCode::VecFSub, OpCode::VecFMul, OpCode::VecFDiv, OpCode::VecFMinMax,
        OpCode::VecFUnary, OpCode::VecFCmp, OpCode::VecFCmpMask, OpCode::VecFCvtIntToFloat,
        OpCode::VecFCvtFloatToInt, OpCode::VecFCvtScalar, OpCode::VecFCvtPacked,
        OpCode::VecFMulAdd, OpCode::VecFRoundInt,
    });
    assign(E::ReadLocal, {OpCode::LoadLocal});
    assign(E::WriteLocal, {OpCode::DefineLocal, OpCode::StoreLocal});
    assign(E::ReadGuestState, {OpCode::LoadUniform, OpCode::GetHostGPR,
                             OpCode::GetHostFPR, OpCode::GetLocation});
    assign(E::WriteGuestState, {OpCode::StoreUniform, OpCode::SetHostGPR, OpCode::SetHostFPR,
                               OpCode::AdvancePC, OpCode::SetLocation, OpCode::PushRSB, OpCode::PopRSB});
    assign(E::ReadMemory | E::MayFault, {OpCode::LoadMemory, OpCode::LoadMemoryTSO});
    assign(E::WriteMemory | E::MayFault, {OpCode::StoreMemory, OpCode::StoreMemoryTSO});
    assign(E::ReadMemory | E::WriteMemory | E::MayFault | E::Barrier,
           {OpCode::CompareAndSwap, OpCode::CompareAndSwap128, OpCode::AtomicExchange,
            OpCode::AtomicFetchAdd, OpCode::AtomicRMW, OpCode::MemoryCopy, OpCode::MemoryCopyTSO});
    assign(E::MayFault, {OpCode::CheckMemoryAlignment});
    assign(E::Barrier, {OpCode::MemoryBarrierTSO, OpCode::UniformBarrier, OpCode::XchgBarrier,
                        OpCode::BranchOnlyEdges});
    assign(E::ControlFlow, {OpCode::Goto, OpCode::NotGoto, OpCode::BindLabel, OpCode::CallReturn});
    assign(E::ReadMemory | E::WriteMemory | E::ReadGuestState | E::WriteGuestState |
           E::MayFault | E::Call, {OpCode::CallLambda, OpCode::CallLocation, OpCode::CallDynamic,
                                 OpCode::X87Op});
    assign(E::ReadFlags, {OpCode::Adc, OpCode::Sbb, OpCode::CondSelect, OpCode::CondSet,
                         OpCode::LocalCondSet, OpCode::TestFlags, OpCode::TestNotFlags});
    assign(E::WriteFlags, {OpCode::SaveFlags, OpCode::BranchOnlyFlags, OpCode::ClearFlags,
                          OpCode::SetCarry, OpCode::SetOverflow, OpCode::PublishFCmpFlags,
                          OpCode::PublishSse42StrFlags});
    assign(E::ReadFlags | E::WriteFlags, {OpCode::InvertCarry});
    assign(E::ReadFPEnvironment | E::ReadFlags, {OpCode::FCmpCondSet});
    return effects;
}
constexpr auto kEffects = BuildEffects();
static_assert([] {
    for (auto effect : kEffects) if (effect == IREffect::Unknown) return false;
    return true;
}(), "every IR opcode requires an effect description");
}  // namespace

const std::array<IRMeta, static_cast<size_t>(OpCode::COUNT) + 1> ir_meta_infos{
        IRMeta{OpCode::Void, "Void", ArgType::Void, {}, IREffect::None},
#define INST(name, ret, ...) \
    IRMeta{OpCode::name, #name, meta::kRet_##name, \
           {meta::kArgs_##name.begin(), meta::kArgs_##name.end()}, kEffects[static_cast<size_t>(OpCode::name)]},
#include "ir.inc"
#undef INST
IRMeta{OpCode::BASE_COUNT, "BASE_COUNT", ArgType::Void, {}, IREffect::Unknown},
IRMeta{OpCode::COUNT, "COUNT", ArgType::Void, {}, IREffect::Unknown}};
static_assert(ir_meta_infos.size() == static_cast<size_t>(OpCode::COUNT) + 1,
              "ir_meta_infos must cover every opcode plus the COUNT sentinel");

u8 GetValueSizePow(ValueType type) {
    if (type >= ValueType::U8 && type <= ValueType::U64) {
        return (u32)type - (u32)ValueType::U8;
    } else if (type >= ValueType::S8 && type <= ValueType::S64) {
        return (u32)type - (u32)ValueType::S8;
    } else if (type >= ValueType::V8 && type <= ValueType::V256) {
        return (u32)type - (u32)ValueType::V8;
    } else {
        PANIC();
    }
}

ValueType GetIRValueType(u32 size_byte) {
    switch (size_byte) {
        case 1:
            return ValueType::U8;
        case 2:
            return ValueType::U16;
        case 4:
            return ValueType::U32;
        case 8:
            return ValueType::U64;
        default:
            return ValueType::VOID;
    }
}

ValueType GetVecIRValueType(u32 size_byte) {
    switch (size_byte) {
        case 1:
            return ValueType::V8;
        case 2:
            return ValueType::V16;
        case 4:
            return ValueType::V32;
        case 8:
            return ValueType::V64;
        case 16:
            return ValueType::V128;
        case 32:
            return ValueType::V256;
        default:
            return ValueType::VOID;
    }
}

ValueType GetSignedIRValueType(u32 size_byte) {
    switch (size_byte) {
        case 1:
            return ValueType::S8;
        case 2:
            return ValueType::S16;
        case 4:
            return ValueType::S32;
        case 8:
            return ValueType::S64;
        default:
            return ValueType::VOID;
    }
}

u8 GetValueSizeByte(ValueType type) { return std::pow(2u, GetValueSizePow(type)); }

bool IsFloatValueType(ValueType type) { return type >= ValueType::V8 && type <= ValueType::V256; }

bool IsSignValueType(ValueType type) { return type >= ValueType::S8 && type <= ValueType::S64; }

const char* ArgTypeString(ArgType arg_type) {
#define ENUM_CLASS ArgType
    switch (arg_type) { ARG_TYPE_ENUM(ENUM_TO_STRING_CASE) }
    return "Unk";
#undef ENUM_CLASS
}

const char* ValueTypeString(ValueType value_type) {
#define ENUM_CLASS ValueType
    switch (value_type) { VALUE_TYPE_ENUM(ENUM_TO_STRING_CASE) }
    return "Unk";
#undef ENUM_CLASS
}

}  // namespace swift::runtime::ir
