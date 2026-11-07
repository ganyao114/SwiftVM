#pragma once

// Private declarations for one machine-code writer. Program, homes and
// output remain borrowed from Compile; no intermediate program is created.
#include "compile.h"
#include "abi.h"
#include "assembler.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace swift::runtime::backend::arm64::detail {
using namespace ir;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
template <OpCode Code> using Op = std::integral_constant<OpCode, Code>;

struct Rejected {
    CompileError error;
    NodeId node;
    BlockId block;
};

using namespace encoding;

class Writer {
public:
    Writer(const Program& source,
           const Allocation& allocation,
           CompileResult& output,
           CompileOptions options,
           bool long_forward_branches = false)
            : source(source)
            , allocation(allocation)
            , output(output)
            , options(options)
            , long_forward_branches(long_forward_branches)
            , masm(output.words, source.Blocks().size()) {}

    // False requests one fresh long-forward emission before publication.
    // Ordinary errors, including B26 overflow, retain their typed rejection.
    bool Run();

private:
    struct Cursor {
        CellRange::Iterator at, end;
        Writer& writer;

        CellView Take() {
            if (at == end) writer.Reject(CompileError::UnsupportedOperand);
            return *at++;
        }

        void Done() {
            if (at != end) writer.Reject(CompileError::UnsupportedOperand);
        }
    };

    struct OpContext {
        const Operation& operation;
        Cursor cursor;
        const Home& home;
        OperationMetadata metadata;
        u64 legacy_mask, flag_mask;
        unsigned destination, width;
        bool produces, vector, test_prefix, saved{};
    };

    void SaveResult(OpContext& ctx) {
        if (ctx.produces && ctx.home.spilled) {
            if (ctx.vector)
                VectorMemory(false, ctx.destination, 31, ctx.home.stack_offset / 16);
            else
                Memory(false, 64, ctx.destination, 31, ctx.home.stack_offset / 8);
            ctx.saved = true;
        }
    }

    void Dispatch(OpContext&);
    void HandleStringFamily(OpContext&);
    void HandleSse42FlagsFamily(OpContext&);

    void Handle(Op<OpCode::Sse42Str>, OpContext& ctx) { HandleStringFamily(ctx); }

    void Handle(Op<OpCode::Sse42StrExplicit>, OpContext& ctx) { HandleStringFamily(ctx); }

    void Handle(Op<OpCode::PublishSse42StrFlags>, OpContext& ctx) { HandleSse42FlagsFamily(ctx); }

    void HandleCryptoFamily(OpContext&);

    void Handle(Op<OpCode::VecAesEnc>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecAesEncLast>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecAesDec>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecAesDecLast>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecAesEncFast>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecAesEncLastFast>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecAesDecFast>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecAesDecLastFast>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecAesImc>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecAesKeygenAssist>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecPclMul>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecSha256Msg1>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecSha256Msg2>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void Handle(Op<OpCode::VecSha256Rnds2>, OpContext& ctx) { HandleCryptoFamily(ctx); }

    void HandleMemoryLoadFenceFamily(OpContext&);

    void Handle(Op<OpCode::MemoryLoadFence>, OpContext& ctx) { HandleMemoryLoadFenceFamily(ctx); }

    void Handle(Op<OpCode::CpuYield>, OpContext& ctx) { HandleMemoryLoadFenceFamily(ctx); }

    void HandleParallelBitsFamily(OpContext&);

    void Handle(Op<OpCode::ParallelBitDeposit>, OpContext& ctx) { HandleParallelBitsFamily(ctx); }

    void Handle(Op<OpCode::ParallelBitExtract>, OpContext& ctx) { HandleParallelBitsFamily(ctx); }

    void HandleReadSystemFamily(OpContext&);

    void Handle(Op<OpCode::ReadSystem>, OpContext& ctx) { HandleReadSystemFamily(ctx); }

    void Handle(Op<OpCode::ReadSystemPair>, OpContext& ctx) { HandleReadSystemFamily(ctx); }

    void Handle(Op<OpCode::X87Execute>, OpContext& ctx) { HandleX87OpFamily(ctx); }

    void Handle(Op<OpCode::X87CommitMetadata>, OpContext& ctx) {
        HandleX87CommitMetadataFamily(ctx);
    }

    void ConvertFloatToInteger(unsigned destination,
                               unsigned raw,
                               unsigned float_bits,
                               unsigned integer_bits,
                               bool truncate);
    void ConvertScalarFloat(unsigned destination, unsigned raw, unsigned source_bits);
    void HandleVecFMulAddFamily(OpContext&);

    void Handle(Op<OpCode::VecFMulAdd>, OpContext& ctx) { HandleVecFMulAddFamily(ctx); }

    void HandleCompareAndSwap128Family(OpContext&);

    void Handle(Op<OpCode::CompareAndSwap128>, OpContext& ctx) {
        HandleCompareAndSwap128Family(ctx);
    }

    template <OpCode Code> void Handle(Op<Code>, OpContext&) {
        Reject(CompileError::UnsupportedOpcode);
    }

    void HandleZeroFamily(OpContext&);
    void HandleLoadImmFamily(OpContext&);
    void HandleLoadMemoryFamily(OpContext&);
    void HandleStoreMemoryFamily(OpContext&);
    void HandleCompareAndSwapFamily(OpContext&);
    void HandleCheckMemoryAlignmentFamily(OpContext&);
    void HandleMemoryBarrierTSOFamily(OpContext&);
    void HandleMemoryStoreFenceFamily(OpContext&);
    void HandleUniformBarrierFamily(OpContext&);
    void HandleGetOperandFamily(OpContext&);
    void HandleBitCastFamily(OpContext&);
    void HandleZeroExtend32Family(OpContext&);
    void HandleAddFamily(OpContext&);
    void HandleNegFamily(OpContext&);
    void HandleAdcFamily(OpContext&);
    void HandleGetUniformAddressFamily(OpContext&);
    void HandleX87OpFamily(OpContext&);
    void HandleX87CommitMetadataFamily(OpContext&);
    void WriteX87Metadata(CellView state, CellView dp, u64 metadata, u64 pc, bool known_pc);
    void HandleDiv128Family(OpContext&);
    void HandleDiv128RemainderFamily(OpContext&);
    void HandleCpuidFamily(OpContext&);
    void HandleCpuidUpperFamily(OpContext&);
    void HandleMulFamily(OpContext&);
    void HandleMulHighFamily(OpContext&);
    void HandleBitExtractFamily(OpContext&);
    void HandleBitInsertFamily(OpContext&);
    void HandleCrc32cFamily(OpContext&);
    void HandleByteSwapFamily(OpContext&);
    void HandlePopCountFamily(OpContext&);
    void HandleCountLeadingZeros32Family(OpContext&);
    void HandleCondSetFamily(OpContext&);
    void HandleCondSelectFamily(OpContext&);
    void HandleGotoFamily(OpContext&);
    void HandleBindLabelFamily(OpContext&);
    void HandleSelectFamily(OpContext&);
    void HandleNotFamily(OpContext&);
    void HandleTestZeroFamily(OpContext&);
    void HandleTestBitFamily(OpContext&);
    void HandleLslImmFamily(OpContext&);
    void HandleLoadUniformFamily(OpContext&);
    void HandleStoreUniformFamily(OpContext&);
    void HandleStoreUniformSelectImmFamily(OpContext&);
    void HandleGetFlagsFamily(OpContext&);
    void HandleRestoreFlagsFamily(OpContext&);
    void HandleSaveFlagsFamily(OpContext&);
    void HandleClearFlagsFamily(OpContext&);
    void HandlePublishFCmpFlagsFamily(OpContext&);
    void HandleTestFlagsFamily(OpContext&);
    void HandleSetCarryFamily(OpContext&);
    void HandleInvertCarryFamily(OpContext&);
    void HandleLocalParitySetFamily(OpContext&);
    void HandleVecSharedZeroFamily(OpContext&);
    void HandleVecLoadConstFamily(OpContext&);
    void HandleVecDup64Family(OpContext&);
    void HandleVecFCvtIntToFloatFamily(OpContext&);
    void HandleVecFCvtFloatToIntFamily(OpContext&);
    void HandleVecFAddFamily(OpContext&);
    void HandleVecFAddScalar64Family(OpContext&);
    void HandleVecFCmpFamily(OpContext&);
    void HandleVecFCvtPackedFamily(OpContext&);
    void HandleVecFCmpMaskFamily(OpContext&);
    void HandleVecMinFamily(OpContext&);
    void HandleVecPackFamily(OpContext&);
    void HandleVecExtract64Family(OpContext&);
    void HandleVecMovMaskFamily(OpContext&);
    void HandleVecExtendFamily(OpContext&);
    void HandleVecXorFamily(OpContext&);
    void HandleVecZipFamily(OpContext&);
    void HandleVecSignedMulWidenLowFamily(OpContext&);
    void HandleVecMulWidenFamily(OpContext&);
    void HandleVecMadd16Family(OpContext&);
    void HandleVecShuffle32TwoSrcFamily(OpContext&);
    void HandleVecShuffle32Family(OpContext&);
    void HandleVecShiftLeftImmFamily(OpContext&);
    void HandleVecByteShiftFamily(OpContext&);

    void Handle(Op<OpCode::Zero>, OpContext& ctx) { HandleZeroFamily(ctx); }

    void Handle(Op<OpCode::LoadImm>, OpContext& ctx) { HandleLoadImmFamily(ctx); }

    void Handle(Op<OpCode::LoadMemory>, OpContext& ctx) { HandleLoadMemoryFamily(ctx); }

    void Handle(Op<OpCode::LoadMemoryTSO>, OpContext& ctx) { HandleLoadMemoryFamily(ctx); }

    void Handle(Op<OpCode::StoreMemory>, OpContext& ctx) { HandleStoreMemoryFamily(ctx); }

    void Handle(Op<OpCode::StoreMemoryTSO>, OpContext& ctx) { HandleStoreMemoryFamily(ctx); }

    void Handle(Op<OpCode::CompareAndSwap>, OpContext& ctx) { HandleCompareAndSwapFamily(ctx); }

    void Handle(Op<OpCode::AtomicExchange>, OpContext& ctx) { HandleCompareAndSwapFamily(ctx); }

    void Handle(Op<OpCode::AtomicFetchAdd>, OpContext& ctx) { HandleCompareAndSwapFamily(ctx); }

    void Handle(Op<OpCode::AtomicRMW>, OpContext& ctx) { HandleCompareAndSwapFamily(ctx); }

    void Handle(Op<OpCode::CheckMemoryAlignment>, OpContext& ctx) {
        HandleCheckMemoryAlignmentFamily(ctx);
    }

    void Handle(Op<OpCode::MemoryBarrierTSO>, OpContext& ctx) { HandleMemoryBarrierTSOFamily(ctx); }

    void Handle(Op<OpCode::MemoryStoreFence>, OpContext& ctx) { HandleMemoryStoreFenceFamily(ctx); }

    void Handle(Op<OpCode::UniformBarrier>, OpContext& ctx) { HandleUniformBarrierFamily(ctx); }

    void Handle(Op<OpCode::GetOperand>, OpContext& ctx) { HandleGetOperandFamily(ctx); }

    void Handle(Op<OpCode::BitCast>, OpContext& ctx) { HandleBitCastFamily(ctx); }

    void Handle(Op<OpCode::ZeroExtend32>, OpContext& ctx) { HandleZeroExtend32Family(ctx); }

    void Handle(Op<OpCode::ZeroExtend32To64>, OpContext& ctx) { HandleZeroExtend32Family(ctx); }

    void Handle(Op<OpCode::ZeroExtend64>, OpContext& ctx) { HandleZeroExtend32Family(ctx); }

    void Handle(Op<OpCode::SignExtend>, OpContext& ctx) { HandleZeroExtend32Family(ctx); }

    void Handle(Op<OpCode::Add>, OpContext& ctx) { HandleAddFamily(ctx); }

    void Handle(Op<OpCode::Sub>, OpContext& ctx) { HandleAddFamily(ctx); }

    void Handle(Op<OpCode::And>, OpContext& ctx) { HandleAddFamily(ctx); }

    void Handle(Op<OpCode::Or>, OpContext& ctx) { HandleAddFamily(ctx); }

    void Handle(Op<OpCode::Xor>, OpContext& ctx) { HandleAddFamily(ctx); }

    void Handle(Op<OpCode::AndNot>, OpContext& ctx) { HandleAddFamily(ctx); }

    void Handle(Op<OpCode::Neg>, OpContext& ctx) { HandleNegFamily(ctx); }

    void Handle(Op<OpCode::Adc>, OpContext& ctx) { HandleAdcFamily(ctx); }

    void Handle(Op<OpCode::Sbb>, OpContext& ctx) { HandleAdcFamily(ctx); }

    void Handle(Op<OpCode::GetUniformAddress>, OpContext& ctx) {
        HandleGetUniformAddressFamily(ctx);
    }

    void Handle(Op<OpCode::X87Op>, OpContext& ctx) { HandleX87OpFamily(ctx); }

    void Handle(Op<OpCode::Div128>, OpContext& ctx) { HandleDiv128Family(ctx); }

    void Handle(Op<OpCode::Div128Remainder>, OpContext& ctx) { HandleDiv128RemainderFamily(ctx); }

    void Handle(Op<OpCode::Cpuid>, OpContext& ctx) { HandleCpuidFamily(ctx); }

    void Handle(Op<OpCode::CpuidUpper>, OpContext& ctx) { HandleCpuidUpperFamily(ctx); }

    void Handle(Op<OpCode::Mul>, OpContext& ctx) { HandleMulFamily(ctx); }

    void Handle(Op<OpCode::MulHigh>, OpContext& ctx) { HandleMulHighFamily(ctx); }

    void Handle(Op<OpCode::BitExtract>, OpContext& ctx) { HandleBitExtractFamily(ctx); }

    void Handle(Op<OpCode::BitClear>, OpContext& ctx) { HandleBitExtractFamily(ctx); }

    void Handle(Op<OpCode::BitInsert>, OpContext& ctx) { HandleBitInsertFamily(ctx); }

    void Handle(Op<OpCode::Crc32c>, OpContext& ctx) { HandleCrc32cFamily(ctx); }

    void Handle(Op<OpCode::ByteSwap>, OpContext& ctx) { HandleByteSwapFamily(ctx); }

    void Handle(Op<OpCode::PopCount>, OpContext& ctx) { HandlePopCountFamily(ctx); }

    void Handle(Op<OpCode::CountLeadingZeros32>, OpContext& ctx) {
        HandleCountLeadingZeros32Family(ctx);
    }

    void Handle(Op<OpCode::CountLeadingZeros64>, OpContext& ctx) {
        HandleCountLeadingZeros32Family(ctx);
    }

    void Handle(Op<OpCode::CountTrailingZeros32>, OpContext& ctx) {
        HandleCountLeadingZeros32Family(ctx);
    }

    void Handle(Op<OpCode::CountTrailingZeros64>, OpContext& ctx) {
        HandleCountLeadingZeros32Family(ctx);
    }

    void Handle(Op<OpCode::CondSet>, OpContext& ctx) { HandleCondSetFamily(ctx); }

    void Handle(Op<OpCode::LocalCondSet>, OpContext& ctx) { HandleCondSetFamily(ctx); }

    void Handle(Op<OpCode::CondSelect>, OpContext& ctx) { HandleCondSelectFamily(ctx); }

    void Handle(Op<OpCode::Goto>, OpContext& ctx) { HandleGotoFamily(ctx); }

    void Handle(Op<OpCode::NotGoto>, OpContext& ctx) { HandleGotoFamily(ctx); }

    void Handle(Op<OpCode::BindLabel>, OpContext& ctx) { HandleBindLabelFamily(ctx); }

    void Handle(Op<OpCode::Select>, OpContext& ctx) { HandleSelectFamily(ctx); }

    void Handle(Op<OpCode::Not>, OpContext& ctx) { HandleNotFamily(ctx); }

    void Handle(Op<OpCode::TestZero>, OpContext& ctx) { HandleTestZeroFamily(ctx); }

    void Handle(Op<OpCode::TestNotZero>, OpContext& ctx) { HandleTestZeroFamily(ctx); }

    void Handle(Op<OpCode::TestBit>, OpContext& ctx) { HandleTestBitFamily(ctx); }

    void Handle(Op<OpCode::LslImm>, OpContext& ctx) { HandleLslImmFamily(ctx); }

    void Handle(Op<OpCode::LslValue>, OpContext& ctx) { HandleLslImmFamily(ctx); }

    void Handle(Op<OpCode::LsrImm>, OpContext& ctx) { HandleLslImmFamily(ctx); }

    void Handle(Op<OpCode::LsrValue>, OpContext& ctx) { HandleLslImmFamily(ctx); }

    void Handle(Op<OpCode::AsrImm>, OpContext& ctx) { HandleLslImmFamily(ctx); }

    void Handle(Op<OpCode::AsrValue>, OpContext& ctx) { HandleLslImmFamily(ctx); }

    void Handle(Op<OpCode::RorImm>, OpContext& ctx) { HandleLslImmFamily(ctx); }

    void Handle(Op<OpCode::RorValue>, OpContext& ctx) { HandleLslImmFamily(ctx); }

    void Handle(Op<OpCode::LoadUniform>, OpContext& ctx) { HandleLoadUniformFamily(ctx); }

    void Handle(Op<OpCode::StoreUniform>, OpContext& ctx) { HandleStoreUniformFamily(ctx); }

    void Handle(Op<OpCode::StoreUniformSelectImm>, OpContext& ctx) {
        HandleStoreUniformSelectImmFamily(ctx);
    }

    void Handle(Op<OpCode::GetFlags>, OpContext& ctx) { HandleGetFlagsFamily(ctx); }

    void Handle(Op<OpCode::RestoreFlags>, OpContext& ctx) { HandleRestoreFlagsFamily(ctx); }

    void Handle(Op<OpCode::SaveFlags>, OpContext& ctx) { HandleSaveFlagsFamily(ctx); }

    void Handle(Op<OpCode::ClearFlags>, OpContext& ctx) { HandleClearFlagsFamily(ctx); }

    void Handle(Op<OpCode::PublishFCmpFlags>, OpContext& ctx) { HandlePublishFCmpFlagsFamily(ctx); }

    void Handle(Op<OpCode::TestFlags>, OpContext& ctx) { HandleTestFlagsFamily(ctx); }

    void Handle(Op<OpCode::TestNotFlags>, OpContext& ctx) { HandleTestFlagsFamily(ctx); }

    void Handle(Op<OpCode::SetCarry>, OpContext& ctx) { HandleSetCarryFamily(ctx); }

    void Handle(Op<OpCode::SetOverflow>, OpContext& ctx) { HandleSetCarryFamily(ctx); }

    void Handle(Op<OpCode::InvertCarry>, OpContext& ctx) { HandleInvertCarryFamily(ctx); }

    void Handle(Op<OpCode::LocalParitySet>, OpContext& ctx) { HandleLocalParitySetFamily(ctx); }

    void Handle(Op<OpCode::VecSharedZero>, OpContext& ctx) { HandleVecSharedZeroFamily(ctx); }

    void Handle(Op<OpCode::VecLoadConst>, OpContext& ctx) { HandleVecLoadConstFamily(ctx); }

    void Handle(Op<OpCode::VecDup64>, OpContext& ctx) { HandleVecDup64Family(ctx); }

    void Handle(Op<OpCode::VecFCvtIntToFloat>, OpContext& ctx) {
        HandleVecFCvtIntToFloatFamily(ctx);
    }

    void Handle(Op<OpCode::VecFCvtFloatToInt>, OpContext& ctx) {
        HandleVecFCvtFloatToIntFamily(ctx);
    }

    void Handle(Op<OpCode::VecFAdd>, OpContext& ctx) { HandleVecFAddFamily(ctx); }

    void Handle(Op<OpCode::VecFSub>, OpContext& ctx) { HandleVecFAddFamily(ctx); }

    void Handle(Op<OpCode::VecFMul>, OpContext& ctx) { HandleVecFAddFamily(ctx); }

    void Handle(Op<OpCode::VecFDiv>, OpContext& ctx) { HandleVecFAddFamily(ctx); }

    void Handle(Op<OpCode::VecFAddScalar64>, OpContext& ctx) { HandleVecFAddScalar64Family(ctx); }

    void Handle(Op<OpCode::VecFSubScalar64>, OpContext& ctx) { HandleVecFAddScalar64Family(ctx); }

    void Handle(Op<OpCode::VecFMulScalar64>, OpContext& ctx) { HandleVecFAddScalar64Family(ctx); }

    void Handle(Op<OpCode::VecFDivScalar64>, OpContext& ctx) { HandleVecFAddScalar64Family(ctx); }

    void Handle(Op<OpCode::VecFCmp>, OpContext& ctx) { HandleVecFCmpFamily(ctx); }

    void Handle(Op<OpCode::VecFCvtPacked>, OpContext& ctx) { HandleVecFCvtPackedFamily(ctx); }

    void Handle(Op<OpCode::VecFCmpMask>, OpContext& ctx) { HandleVecFCmpMaskFamily(ctx); }

    void Handle(Op<OpCode::VecMin>, OpContext& ctx) { HandleVecMinFamily(ctx); }

    void Handle(Op<OpCode::VecPack>, OpContext& ctx) { HandleVecPackFamily(ctx); }

    void Handle(Op<OpCode::VecExtract64>, OpContext& ctx) { HandleVecExtract64Family(ctx); }

    void Handle(Op<OpCode::VecExtract16>, OpContext& ctx) { HandleVecExtract64Family(ctx); }

    void Handle(Op<OpCode::VecMovMask>, OpContext& ctx) { HandleVecMovMaskFamily(ctx); }

    void Handle(Op<OpCode::VecExtend>, OpContext& ctx) { HandleVecExtendFamily(ctx); }

    void Handle(Op<OpCode::VecXor>, OpContext& ctx) { HandleVecXorFamily(ctx); }

    void Handle(Op<OpCode::VecAnd>, OpContext& ctx) { HandleVecXorFamily(ctx); }

    void Handle(Op<OpCode::VecOr>, OpContext& ctx) { HandleVecXorFamily(ctx); }

    void Handle(Op<OpCode::VecAndNot>, OpContext& ctx) { HandleVecXorFamily(ctx); }

    void Handle(Op<OpCode::VecAdd>, OpContext& ctx) { HandleVecXorFamily(ctx); }

    void Handle(Op<OpCode::VecSub>, OpContext& ctx) { HandleVecXorFamily(ctx); }

    void Handle(Op<OpCode::VecMul>, OpContext& ctx) { HandleVecXorFamily(ctx); }

    void Handle(Op<OpCode::VecCmpEq>, OpContext& ctx) { HandleVecXorFamily(ctx); }

    void Handle(Op<OpCode::VecCmpGt>, OpContext& ctx) { HandleVecXorFamily(ctx); }

    void Handle(Op<OpCode::VecZip>, OpContext& ctx) { HandleVecZipFamily(ctx); }

    void Handle(Op<OpCode::VecUnzip>, OpContext& ctx) { HandleVecZipFamily(ctx); }

    void Handle(Op<OpCode::VecSignedMulWidenLow>, OpContext& ctx) {
        HandleVecSignedMulWidenLowFamily(ctx);
    }

    void Handle(Op<OpCode::VecMulWiden>, OpContext& ctx) { HandleVecMulWidenFamily(ctx); }

    void Handle(Op<OpCode::VecMadd16>, OpContext& ctx) { HandleVecMadd16Family(ctx); }

    void Handle(Op<OpCode::VecShuffle32TwoSrc>, OpContext& ctx) {
        HandleVecShuffle32TwoSrcFamily(ctx);
    }

    void Handle(Op<OpCode::VecShuffle32>, OpContext& ctx) { HandleVecShuffle32Family(ctx); }

    void Handle(Op<OpCode::VecShuffle16>, OpContext& ctx) { HandleVecShuffle32Family(ctx); }

    void Handle(Op<OpCode::VecShiftLeftImm>, OpContext& ctx) { HandleVecShiftLeftImmFamily(ctx); }

    void Handle(Op<OpCode::VecShiftRightImm>, OpContext& ctx) { HandleVecShiftLeftImmFamily(ctx); }

    void Handle(Op<OpCode::VecShiftRightArithmeticImm>, OpContext& ctx) {
        HandleVecShiftLeftImmFamily(ctx);
    }

    void Handle(Op<OpCode::VecByteShift>, OpContext& ctx) { HandleVecByteShiftFamily(ctx); }

    void Handle(Op<OpCode::VecExtractBytes>, OpContext& ctx) { HandleVecByteShiftFamily(ctx); }

    void HandleVecAvgFamily(OpContext&);

    void Handle(Op<OpCode::VecAvg>, OpContext& ctx) { HandleVecAvgFamily(ctx); }

    void HandleVecMaxFamily(OpContext&);

    void Handle(Op<OpCode::VecMax>, OpContext& ctx) { HandleVecMaxFamily(ctx); }

    void HandleVecSatAddFamily(OpContext&);

    void Handle(Op<OpCode::VecSatAdd>, OpContext& ctx) { HandleVecSatAddFamily(ctx); }

    void Handle(Op<OpCode::VecSatSub>, OpContext& ctx) { HandleVecSatAddFamily(ctx); }

    void HandleVecMulHigh16Family(OpContext&);

    void Handle(Op<OpCode::VecMulHigh16>, OpContext& ctx) { HandleVecMulHigh16Family(ctx); }

    void HandleVecAbsDiffSum8Family(OpContext&);

    void Handle(Op<OpCode::VecAbsDiffSum8>, OpContext& ctx) { HandleVecAbsDiffSum8Family(ctx); }

    void HandleVecShiftLeftFamily(OpContext&);

    void Handle(Op<OpCode::VecShiftLeft>, OpContext& ctx) { HandleVecShiftLeftFamily(ctx); }

    void Handle(Op<OpCode::VecShiftRight>, OpContext& ctx) { HandleVecShiftLeftFamily(ctx); }

    void Handle(Op<OpCode::VecShiftRightArithmetic>, OpContext& ctx) {
        HandleVecShiftLeftFamily(ctx);
    }

    void HandleVecDupPairs32Family(OpContext&);

    void Handle(Op<OpCode::VecDupPairs32>, OpContext& ctx) { HandleVecDupPairs32Family(ctx); }

    void HandleVecInsert16Family(OpContext&);

    void Handle(Op<OpCode::VecInsert16>, OpContext& ctx) { HandleVecInsert16Family(ctx); }

    void HandleVecTableLookup8Family(OpContext&);

    void Handle(Op<OpCode::VecTableLookup8>, OpContext& ctx) { HandleVecTableLookup8Family(ctx); }

    void HandleVecFMinMaxFamily(OpContext&);

    void Handle(Op<OpCode::VecFMinMax>, OpContext& ctx) { HandleVecFMinMaxFamily(ctx); }

    void HandleVecFUnaryFamily(OpContext&);

    void Handle(Op<OpCode::VecFUnary>, OpContext& ctx) { HandleVecFUnaryFamily(ctx); }

    void HandleVecFCvtScalarFamily(OpContext&);

    void Handle(Op<OpCode::VecFCvtScalar>, OpContext& ctx) { HandleVecFCvtScalarFamily(ctx); }

    void HandleVecFRoundIntFamily(OpContext&);

    void Handle(Op<OpCode::VecFRoundInt>, OpContext& ctx) { HandleVecFRoundIntFamily(ctx); }

    void Handle(Op<OpCode::VecFAddScalar32>, OpContext& ctx) { HandleVecFAddScalar64Family(ctx); }

    void Handle(Op<OpCode::VecFSubScalar32>, OpContext& ctx) { HandleVecFAddScalar64Family(ctx); }

    void Handle(Op<OpCode::VecFMulScalar32>, OpContext& ctx) { HandleVecFAddScalar64Family(ctx); }

    void Handle(Op<OpCode::VecFDivScalar32>, OpContext& ctx) { HandleVecFAddScalar64Family(ctx); }

    const Program& source;
    const Allocation& allocation;
    CompileResult& output;
    CompileOptions options;
    bool long_forward_branches{};
    Assembler masm;
    using LocalFixup = Assembler::LocalFixup;
    NodeId node{};
    BlockId block{};
    NodeId pending_save{};  // At most the adjacent legacy publication, never a node table.
    bool shadow_dirty{};
    bool packed_current{};       // Native mode: x1 has current NZCV and, when raw, AF.
    u64 nzcv_matches{};          // Host NZCV bits currently equal the authoritative packed x1.
    unsigned deferred_parity{};  // Zero means x1; otherwise this actual pin supplies its low byte.
    bool direct_carry{};         // Hardware C equals guest C until its deferred inversion.
    std::uint8_t fault_parity{},
            fault_carry{};  // Captured at the actual scalar memory instruction.
    unsigned terminal_condition{
            18};  // 0..15 neutral cond; 16/17 AF; 20/21 native C|Z; 18 materialized.
    u64 fault_location{};
    bool has_fault_location{};
    u32 unaligned_word{UINT32_MAX};  // Actual current-op recoverable fast instruction.
    NodeId elided_consumer{};        // Only the adjacent numeric consumer already emitted with its
                                     // producer.
    u64 static_pc{};
    bool static_pc_valid{};
    u64 alternate_pc{};
    bool alternate_pc_valid{};
    bool dispatch_emitted{};             // The actual final PC store emitted its dispatch.
    u32 dispatcher_literal{UINT32_MAX};  // Immutable DATA in this unit, never an outer link target.

    // One streaming recurrence in this Writer. Prefix operations are emitted
    // normally; these two actual sites acquire recipes only at the tail TEST.
    struct DeferredTest {
        NodeId snapshot{}, loaded{}, value{};
        u32 snapshot_offset{}, capture_offset{};
        std::array<std::size_t, 2> sites{};
        std::uint8_t site_count{}, bits{}, snapshot_register{}, value_register{};
        bool active{}, captured{};
    } deferred_test;

    bool deferred_test_used{};

    static constexpr u64 carry = FlagBits(Flag::Carry), overflow = FlagBits(Flag::Overflow),
                         zero = FlagBits(Flag::Zero), negative = FlagBits(Flag::Negate),
                         parity = FlagBits(Flag::Parity),
                         auxiliary = FlagBits(Flag::AuxiliaryCarry);
    static constexpr u64 all_flags = FlagBits(Flag::All);
    static constexpr u64 nzcv_flags = FlagBits(Flag::NZCV);

    bool Enabled() const { return options.flags_offset != UINT32_MAX; }

    bool NativeNZCV() const { return options.native_nzcv; }

    bool Linked() const { return options.dispatcher || options.exit_handler; }

    bool RawAF() const { return NativeNZCV() && Linked(); }

    bool Pinned() const { return options.pinned_uniform_offset != UINT32_MAX; }

    bool VectorPinned() const { return options.pinned_vector_offset != UINT32_MAX; }

    unsigned VectorTemporary(bool right = false) const {
        return (VectorPinned() ? 8u : 30u) + right;
    }

    bool CanDeferFlags() const {
        if (!options.local_flags || !RawAF() || !options.private_uniforms ||
            !options.fault_locations || !node)
            return false;
        const auto& operation = source.Operations()[node.value];
        const auto metadata = operation.Metadata();
        return (operation.opcode == OpCode::Add || operation.opcode == OpCode::Sub ||
                operation.opcode == OpCode::Neg) &&
               (operation.type == Type::U32 || operation.type == Type::U64) &&
               metadata.flag_outputs &&
               (metadata.profile == FlagProfile::ArithmeticRaw ||
                metadata.profile == FlagProfile::ArithmeticCanonicalCarry);
    }

    void CompleteParity() {
        if (deferred_parity) {
            Move(1, deferred_parity);
            deferred_parity = 0;
            packed_current = false;
        }
    }

    void CompleteCarry() {
        if (direct_carry) {
            masm.exact.cfinv();
            direct_carry = false;
            packed_current = false;
        }
    }

    void CompleteLocalFlags() {
        CompleteParity();
        CompleteCarry();
    }

    bool RcpcImm9() const {
        return options.rcpc_imm9 && options.fault_locations && options.recover_unaligned &&
               has_fault_location;
    }

    bool TestField(CellView field) const {
        const auto offset = field.Payload();
        return field.Kind() == OperandKind::Uniform && offset >= options.pinned_uniform_offset &&
               offset - options.pinned_uniform_offset < options.pinned_uniform_slots * 8u &&
               !((offset - options.pinned_uniform_offset) & 7u);
    }

    bool DeferTest(const Operation& operation, Cursor& cursor);
    void CompleteDeferredTest();

    void RequireFlags() const {
        if (!Enabled()) Reject(CompileError::UnsupportedFlags);
    }

    [[noreturn]] void Reject(CompileError error) const { throw Rejected{error, node, block}; }

    static unsigned Bits(Type type) { return IntegerType(type) ? TypeBits(type) : 0; }

    unsigned Width(Type type) const {
        const auto bits = Bits(type);
        if (!bits || bits > 64) Reject(CompileError::UnsupportedType);
        return bits;
    }

    void Move(unsigned to, unsigned from) {
        if (to != from) masm.exact.orr(X(to), v::xzr, Operand(X(from)));
    }

    void Immediate(unsigned reg, u64 bits) {
        const auto parts = bits <= UINT32_MAX ? 2u : 4u;
        unsigned zero{}, full{};
        for (unsigned part = 0; part < parts; ++part) {
            const auto word = static_cast<unsigned>((bits >> (part * 16)) & 0xffff);
            zero += word == 0;
            full += word == 0xffff;
        }
        const bool invert = full > zero;
        const auto seed = invert ? ~bits : bits;
        const auto first = std::min(parts - 1, static_cast<unsigned>(std::countr_zero(seed)) / 16);
        const auto target = R(reg, parts == 4 ? 64 : 32);
        if (invert)
            masm.exact.movn(target, (seed >> (first * 16)) & 0xffffu, first * 16);
        else
            masm.exact.movz(target, (seed >> (first * 16)) & 0xffffu, first * 16);
        for (unsigned part = 0; part < parts; ++part) {
            const auto word = static_cast<u32>((bits >> (part * 16)) & 0xffff);
            if (part != first && word != (invert ? 0xffffu : 0u))
                masm.exact.movk(target, word, part * 16);
        }
    }

    void Narrow(unsigned to, unsigned from, unsigned bits, bool sign = false) {
        if (bits == 64) {
            Move(to, from);
            return;
        }
        if (bits == 32 && !sign) {
            masm.exact.orr(W(to), v::wzr, Operand(W(from)));
            return;
        }
        if (sign)
            masm.exact.sbfx(X(to), X(from), 0, bits);
        else
            masm.exact.ubfx(X(to), X(from), 0, bits);
    }

    void Extract(unsigned to, unsigned from, unsigned first, unsigned count) {
        masm.exact.ubfx(X(to), X(from), first, count);
    }

    void Insert(unsigned to,
                unsigned from,
                unsigned first,
                unsigned count,
                unsigned native_width = 64) {
        masm.exact.bfi(R(to, native_width), R(from, native_width), first, count);
    }

    void ShiftLeft(unsigned to, unsigned from, unsigned amount) {
        if (!amount) {
            Move(to, from);
            return;
        }
        masm.exact.lsl(X(to), X(from), amount);
    }

    void StackAdjust(bool subtract);

    void Memory(bool load, unsigned bits, unsigned value, unsigned address, u32 scaled = 0) {
        if (scaled > 4095) Reject(CompileError::UnsupportedOperand);
        const MemOperand memory{Base(address), static_cast<std::int64_t>(scaled) * (bits / 8)};
        if (bits == 8) {
            if (load)
                masm.exact.ldrb(W(value), memory);
            else
                masm.exact.strb(W(value), memory);
        } else if (bits == 16) {
            if (load)
                masm.exact.ldrh(W(value), memory);
            else
                masm.exact.strh(W(value), memory);
        } else if (bits == 32 || bits == 64) {
            if (load)
                masm.exact.ldr(R(value, bits), memory);
            else
                masm.exact.str(R(value, bits), memory);
        } else
            Reject(CompileError::UnsupportedType);
    }

    void PCMemory(bool load);

    void VectorMemory(bool load, unsigned value, unsigned address, u32 scaled = 0) {
        if (load)
            masm.exact.ldr(Q(value), MemOperand(Base(address), scaled * 16));
        else
            masm.exact.str(Q(value), MemOperand(Base(address), scaled * 16));
    }

    void OrderedMemory(
            bool load, unsigned bits, unsigned value, unsigned address, std::int32_t offset = 0);

    void VectorMove(unsigned to, unsigned from) {
        if (to != from) masm.exact.orr(Lanes(to, 8), Lanes(from, 8), Lanes(from, 8));
    }

    void LoadMxcsrFlushControl();
    void FlushScalarDenormal(unsigned value,
                             unsigned control_bit,
                             unsigned lane = 0,
                             unsigned lane_bits = 64,
                             unsigned lane_count = 1);
    void TruncateDoubleToInteger(unsigned destination, unsigned raw, unsigned bits);
    unsigned VectorValue(CellView cell, unsigned scratch);
    void VectorUniform(bool load, CellView uniform, unsigned value);
    int VectorPinnedSlot(u32 offset, unsigned bytes) const;
    void VectorAlias(bool reload, u32 offset, unsigned bytes);

    unsigned RawValue(u32 id, unsigned scratch, bool physical = false) {
        if (id >= allocation.homes.size() || source.Operations()[id].type == Type::Void)
            Reject(CompileError::InvalidProgram);
        const auto& home = allocation.homes[id];
        unsigned reg = home.reg;
        if (home.spilled) {
            Memory(true, 64, scratch, 31, home.stack_offset / 8);
            reg = scratch;
        }
        if (home.narrow_pin && !physical) {
            Narrow(scratch, reg, home.CarrierBits());
            return scratch;
        }
        return reg;
    }

    Type View(CellView cell) const {
        if (cell.Kind() == OperandKind::Value) {
            if (cell.Payload() >= source.Operations().size()) Reject(CompileError::InvalidProgram);
            return cell.TypeView() == Type::Void ? source.Operations()[cell.Payload()].type
                                                 : cell.TypeView();
        }
        if (cell.Kind() == OperandKind::Immediate) return cell.TypeView();
        Reject(CompileError::UnsupportedOperand);
    }

    // limit/sign describe the logical read. native_width only says that the
    // consuming instruction itself ignores all bits above its W input.
    unsigned Read(CellView cell,
                  unsigned scratch,
                  unsigned limit = 64,
                  bool sign = false,
                  unsigned native_width = 64) {
        auto bits = std::min(Width(View(cell)), limit);
        unsigned reg = scratch;
        if (cell.Kind() == OperandKind::Value) {
            const auto id = static_cast<u32>(cell.Payload());
            reg = RawValue(id, scratch, true);
            const auto& producer = source.Operations()[id];
            const auto& home = allocation.homes[id];
            const auto narrow_pin = home.narrow_pin;
            const auto carrier_bits = home.CarrierBits();
            if (bits > carrier_bits) {
                bits = carrier_bits;
                sign = false;
            }
            if (native_width == 32 && bits >= 32) return reg;
            // Numeric producers already normalize to their natural width.
            // Raw transport is the exception; its narrow label is not a mask.
            if (!narrow_pin && !sign && Bits(producer.type) <= bits &&
                producer.opcode != OpCode::BitCast && producer.opcode != OpCode::GetResult &&
                producer.opcode != OpCode::GetFlags)
                return reg;
        } else {
            auto value = cell.Payload();
            if (bits < 64) {
                const auto mask = (u64{1} << bits) - 1;
                value &= mask;
                if (sign && (value & (u64{1} << (bits - 1)))) value |= ~mask;
            }
            Immediate(scratch, value);
            return scratch;
        }
        if (bits == 64) return reg;
        Narrow(scratch, reg, bits, sign);
        return scratch;
    }

    unsigned RawScalar(CellView cell, unsigned scratch, unsigned physical_bits = 0) {
        if (cell.Kind() != OperandKind::Value) Reject(CompileError::UnsupportedOperand);
        Width(View(cell));  // The integer view is valid but does not cut a raw container.
        const auto id = static_cast<u32>(cell.Payload());
        const auto value = RawValue(id, scratch, physical_bits != 0);
        // A consumer may cut its own low bits only when the real carrier
        // supplies all of them. Otherwise retain the borrowed value's zeros.
        if (physical_bits && allocation.homes[id].CarrierBits() < physical_bits) {
            Narrow(scratch, value, allocation.homes[id].CarrierBits());
            return scratch;
        }
        return value;
    }

    u64 IntegerImmediate(CellView cell) const {
        if (cell.Kind() != OperandKind::Immediate) Reject(CompileError::UnsupportedOperand);
        Width(cell.TypeView());
        return cell.Payload();
    }

    void Field(u64 first, u64 count) const {
        if (first >= 64 || count > 64 - first) Reject(CompileError::UnsupportedOperand);
    }

    unsigned Expression(Cursor& cursor,
                        unsigned first,
                        unsigned second,
                        unsigned native_width = 64);
    unsigned OrderedAddress(Cursor& cursor, std::int32_t& offset, bool store = false);
    bool PartialByteStore(const Operation& operation, const Home& home) const;
    void Uniform(bool load, CellView uniform, unsigned value);

    int PinnedSlot(u32 offset, unsigned size) const {
        const auto bytes = options.pinned_uniform_slots * 8u;
        if (!Pinned() || u64{offset} >= u64{options.pinned_uniform_offset} + bytes ||
            options.pinned_uniform_offset >= u64{offset} + size)
            return -1;
        if (offset < options.pinned_uniform_offset ||
            u64{offset} + size > u64{options.pinned_uniform_offset} + bytes ||
            ((offset - options.pinned_uniform_offset) & 7u) + size > 8)
            Reject(CompileError::UnsupportedOperand);
        return static_cast<int>(abi::PinnedUniformRegister(
                (offset - options.pinned_uniform_offset) / 8, options.pinned_uniform_slots));
    }

    void PublishPinned();
    bool ConstantPC(CellView value, u64& pc) const;
    void ObservePCStore(CellView uniform, CellView value);
    void FlagsMemory(bool load, unsigned value);
    void Shadow(bool load);
    void ContextMemory(bool load, unsigned value, int offset);
    void HelperFPEnvironment(bool guest);
    bool OverlapsShadow(CellView uniform) const;

    u64 Flags(CellView cell, bool validate = true) const {
        if (cell.Kind() != OperandKind::Flags || (validate && (cell.Payload() & ~all_flags)))
            Reject(CompileError::UnsupportedFlags);
        return cell.Payload();
    }

    unsigned Condition(CellView cell) const {
        if (cell.Kind() != OperandKind::Condition || cell.Payload() >= 16)
            Reject(CompileError::UnsupportedOperand);
        return static_cast<unsigned>(cell.Payload());
    }

    static u32 HostMask(u64 mask) {
        return (mask & carry ? 1u << 29 : 0) | (mask & overflow ? 1u << 28 : 0) |
               (mask & zero ? 1u << 30 : 0) | (mask & negative ? 1u << 31 : 0);
    }

    void PackNZCV();
    void UnpackRawFlags();
    void RestoreNZCV();
    void RestoreHelperNZCV();

    void SnapshotCompareNZCV() {
        if (RawAF())
            masm.exact.mrs(v::x30, v::NZCV);  // MRS x30,NZCV before reading scratch operands.
        else
            PackNZCV();
    }

    void RestoreCompareNZCV() {
        if (RawAF()) {
            masm.exact.msr(v::NZCV, v::x30);
            packed_current = false;
        } else
            RestoreNZCV();
    }

    void Rmif(unsigned value, unsigned rotation, u32 mask) {
        // The local mask is the instruction's four-bit NZCV nibble; VIXL's
        // StatusFlags instead names the architectural PSTATE bit positions.
        masm.exact.rmif(X(value), rotation, static_cast<v::StatusFlags>(mask << v::Flags_offset));
    }

    void PrepareNZCV(u64 mask) {
        const auto clobber = RawAF() ? allocation.homes[node.value].nzcv_clobber : 0u;
        const auto preserved = nzcv_flags & ~mask & ~clobber;
        if (NativeNZCV() && (mask & nzcv_flags) && preserved) {
            if (preserved & carry) CompleteCarry();
            masm.exact.mrs(v::x30, v::NZCV);  // MRS x30,NZCV: preserve the unwritten fields.
        }
    }

    void FinishNZCV(u64 mask, bool invert_carry) {
        if (!(mask & nzcv_flags)) return;
        const auto clobber = RawAF() ? allocation.homes[node.value].nzcv_clobber : 0u;
        if (mask & carry) {
            direct_carry = invert_carry && CanDeferFlags();
            if (invert_carry && !direct_carry) masm.exact.cfinv();  // CFINV.
        } else if (clobber & carry)
            direct_carry = false;
        if (const auto preserved = HostMask(nzcv_flags & ~mask & ~clobber) >> 28)
            Rmif(30, 28, preserved);
        packed_current = false;
    }

    void EnsureNZCV(unsigned condition);
    void ClearMask(u64 mask);
    void MergeNZCV(u64 mask, bool flip_carry = false);
    void Parity(unsigned to, unsigned value);
    void Test(u64 mask, unsigned destination, bool inverse);

    void Predicate(unsigned destination, unsigned value, bool zero) {
        if (NativeNZCV()) {
            masm.exact.clz(X(destination), X(value));  // CLZ Xd,Xn, including 64 for zero.
            Extract(destination, destination, 6, 1);
            if (!zero) masm.exact.eor(X(destination), X(destination), Operand(1));  // EOR Xd,Xd,#1.
            return;
        }
        nzcv_matches = 0;
        masm.exact.cmp(X(value), Operand(0));                   // CMP Xn,#0
        masm.exact.cset(X(destination), zero ? v::eq : v::ne);  // CSET EQ/NE
        RestoreNZCV();
    }

    static bool Logical(OpCode op) {
        return op == OpCode::And || op == OpCode::Or || op == OpCode::Xor || op == OpCode::AndNot;
    }

    static bool Arithmetic(OpCode op) {
        return op == OpCode::Add || op == OpCode::Sub || op == OpCode::Neg || op == OpCode::Adc ||
               op == OpCode::Sbb;
    }

    void Metadata(const Operation& op) const;
    // The only legacy domain accepted here is a fixed adjacent pair (or one
    // disjoint ClearFlags in between). No producer walk or demand table.
    u64 LegacyMask(NodeId owner, NodeId save) const;
    u64 CaptureLegacy(const Operation& op);
    void WriteParity(unsigned result, bool preserve_af);
    void ArithmeticFlags(unsigned result, u64 mask, bool canonical);
    void LogicalFlags(unsigned result,
                      unsigned width,
                      u64 mask,
                      bool clear_af,
                      bool nzcv_ready = false,
                      bool invert_carry = true);

    unsigned LaneSize(u64 bits, bool allow64 = true) const {
        if (bits == 8) return 0;
        if (bits == 16) return 1;
        if (bits == 32) return 2;
        if (bits == 64 && allow64) return 3;
        Reject(CompileError::UnsupportedOperand);
    }

    void Emit(const Operation& operation, Effect effects);
    void Branch(BlockId target);
    bool ShortBranch(Branch19 instruction, BlockId target);
    void TailJump(std::uintptr_t destination,
                  bool link = false,
                  u64 target_pc = 0,
                  u32 conditional_head = UINT32_MAX);
    void DynamicDispatch();
    void PublishPCAndJump(u64 target_pc);
    void Epilogue();
    unsigned PrepareTerminalCondition(unsigned condition, bool& restore, bool& flip);
    void RestoreTerminalCondition(bool restore, bool flip);
    void ConditionPC(unsigned condition, u64 taken, u64 fallthrough);
    void ConditionEdges(unsigned condition, const StoredTerminal& terminal, BlockId next);
    void Terminal(const StoredTerminal& terminal, BlockId next);
    void HandleReturn(const StoredTerminal& terminal);
    void HandleDispatch();
    void HandleJump(const StoredTerminal& terminal, BlockId next);
    void HandleBranch(const StoredTerminal& terminal, BlockId next);
    void HandleUnreachable();
};

}  // namespace swift::runtime::backend::arm64::detail
