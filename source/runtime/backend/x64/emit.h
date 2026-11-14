#pragma once
#ifndef XBYAK64
#define XBYAK64
#endif

#include "allocate.h"
#include "compile.h"
#include "runtime/backend/relocation.h"
#include "runtime/exit.h"

#include <array>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xbyak/xbyak.h>

namespace swift::runtime::backend::x64::detail {
using namespace ir;
using namespace Xbyak;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
template <OpCode Code> using Op = std::integral_constant<OpCode, Code>;

struct ArmNumericRequest;

constexpr bool SupportsFlagOutputs(OpCode opcode) noexcept {
    switch (opcode) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Adc:
        case OpCode::Sbb:
        case OpCode::Neg:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::AndNot:
        case OpCode::Not:
            return true;
        default:
            return false;
    }
}

constexpr bool IsPureScalarAluOrImmOp(OpCode opcode) noexcept {
    switch (opcode) {
        case OpCode::LoadImm:
        case OpCode::Zero:
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Mul:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::AndNot:
        case OpCode::LslImm:
        case OpCode::LsrImm:
        case OpCode::AsrImm:
        case OpCode::RorImm:
        case OpCode::ZeroExtend32:
        case OpCode::ZeroExtend32To64:
        case OpCode::ZeroExtend64:
        case OpCode::SignExtend:
        case OpCode::CondSet:
        case OpCode::LocalCondSet:
            return true;
        default:
            return false;
    }
}

constexpr bool PreservesCachedXmm0(OpCode opcode, bool vector_result) noexcept {
    switch (opcode) {
        case OpCode::Nop:
        case OpCode::SetLocation:
        case OpCode::AdvancePC:
        case OpCode::InvertCarry:
        case OpCode::SetCarry:
        case OpCode::ClearFlags:
        case OpCode::StoreUniform:
        case OpCode::StoreMemory:
        case OpCode::StoreMemoryTSO:
            return true;
        default:
            return !vector_result && IsPureScalarAluOrImmOp(opcode);
    }
}

struct Rejected {
    CompileError error;
    NodeId node;
    BlockId block;
};

struct OpContext {
    const Operation& operation;
    NodeId node;
    CellRange operands;
    std::array<CellView, 5> args{};
    unsigned count{}, width{};
    Reg64 destination{0};
    Xmm vector{0};
    bool vector_result{}, saved{};
    // An emitter may omit final normalization only when it already wrote the
    // complete scalar value, including the upper bits of narrow results.
    bool scalar_normalized{};

    CellView Arg(unsigned i) const { return args[i]; }
};

class Writer : public Xbyak::CodeGenerator {
public:
    Writer(const Program&, const Allocation&, CompileResult&, CompileOptions);
    void Run();
    ir::Type View(CellView) const;
    bool Immediate32(CellView, unsigned width, std::int32_t&) const;
    bool IsZeroScalarOperand(CellView, unsigned width) const;
    bool IsZeroIdentityCopy(OpContext&, CellView& other) const;
    bool TryEmitZeroIdentityCopy(OpContext&);
    void Read(CellView, const Reg64&, bool raw = false);
    bool ResidentScalar(CellView, unsigned width, Reg64&) const;
    bool MemoryScalarOperand(CellView, unsigned width, Xbyak::Address&) const;
    bool ResidentVector(CellView, Xmm&) const;
    bool MemoryVectorOperand(CellView, Xbyak::Address&) const;

    Reg64 ScalarDestination(OpContext& c) const {
        // Only leaf emitters that do not call helpers borrow the assigned home.
        // Spilled results keep the ordinary RAX path and final stack store.
        const auto& home = allocation.homes[c.node.value];
        if (!home.spilled && !c.vector_result) c.destination = Reg64(home.reg);
        return c.destination;
    }

    void ReadVector(CellView, const Xmm&);
    void LoadAvxVectorLow32(CellView, const Xmm&);
    void LoadAvxVectorLow64(CellView, const Xmm&);
    void WriteResult(OpContext&);
    void Mask(const Reg64&, unsigned);
    void Sign(const Reg64&, unsigned);
    void Expression(CellRange::Iterator&, CellRange::Iterator, const Reg64&, unsigned depth = 0);
    void Load(const Reg64&, const Xbyak::RegExp&, unsigned);
    void Store(const Reg64&, const Xbyak::RegExp&, unsigned);
    void Load(const Reg64&, const Reg64&, std::int32_t, unsigned);
    void Store(const Reg64&, const Reg64&, std::int32_t, unsigned);
    void HostAddress(const Reg64&, std::uintptr_t, HostSymbol);
    void Call(std::uintptr_t, HostSymbol);
    void CallRegister(const Reg64&);
    void LambdaCall(OpContext&);
    bool TryArmNumeric(OpContext&);
    void ArmFPValues(OpContext&);
    void MaterializeFPFlags();
    void CheckArmFPInput(const Xmm&, unsigned bits, unsigned lanes, unsigned mode, Label&);
    void PrepareCall();

    struct CallMasks {
        unsigned scalars{}, vectors{};
    };

    CallMasks LiveCallMasks() const;
    void PrepareCall(CallMasks);
    void FinishCall();
    void Exit(u64);
    void StaticExit(u64);
    void Restore();
    void PublishFlags();
    void MaterializeFlags();
    void SaveNativeFlags(bool canonical = false);
    void ImportPackedFlags();
    void ImportLinkedFlags();
    void ExportLinkedFlags();
    void EnsureNativeFlags(unsigned needed = 15);
    void PrepareFlags(OpContext&);
    void ClobberFlags(OpContext&);
    bool PreservesFlags(OpContext&) const;
    bool ExpressionPreservesFlags(CellRange::Iterator&, CellRange::Iterator) const;
    unsigned NativeCondition(ir::Condition);
    void PadFor32ByteBoundary(unsigned bytes);
    void NativeJump(unsigned, Label&);
    void BranchTo(BlockId, bool at_block_end = false);
    void PrepareEdge(BlockId);
    void ConditionalBranch(unsigned, BlockId, BlockId);

    bool Pinned() const { return options.pinned_uniform_slots != 0; }

    static unsigned PinnedRegister(unsigned slot) {
        constexpr unsigned registers[]{13, 14, 15, 5};
        return registers[slot];
    }

    int PinnedSlot(std::uint64_t offset, unsigned bytes) const;
    void PublishPinned();
    void ReloadPinned();
    void Condition(const Reg64&, ir::Condition);
    void PackedCondition(const Reg64&, ir::Condition);
    bool NativeFlagTest(OpContext&, bool inverse);
    void MergeFlags(const Reg64&, u64);
    void CaptureFlags(OpContext&);
    void Reject(CompileError);
    void Uniform(OpContext&);

    void RecordGuestAccess(u32 first, unsigned access, unsigned bytes) {
        if (!options.fault_locations || !has_fault_location) return;
        output.faults.push_back({first, static_cast<u32>(getSize()), guest_pc});
        auto& site = output.faults.back();
        site.recovery[0] = fault_committed_flags;
        site.recovery[4] = static_cast<std::uint8_t>(access);
        site.recovery[5] = static_cast<std::uint8_t>(bytes);
        if (options.local_flags && flags.dirty && !fault_committed_flags) {
            const auto saved = 15 & ~flags.native;
            site.recovery[0] =
                    2 | (flags.carry << 2) | (flags.saved_carry << 3) | (options.lahf << 4);
            site.recovery[1] = flags.native | (saved << 4);
            if (saved) {
                site.recovery[2] = allocation.flags_slot;
                site.recovery[3] = allocation.flags_slot >> 8;
                site.recovery[7] = allocation.flags_slot >> 16;
            }
        }
    }

    Xbyak::RegExp MemoryAddress(CellRange::Iterator&, CellRange::Iterator);
    void Memory(OpContext&);
    void Arithmetic(OpContext&);
    void Logical(OpContext&);
    void Shift(OpContext&);
    void BitField(OpContext&);
    void Count(OpContext&);
    void SelectValue(OpContext&);
    void Atomic(OpContext&);
    void Float(OpContext&);
    void AccumulateFloating(const Reg32&);
    void BeginArmFPRegion(u32 control, u32 status);
    void EndArmFPRegion();
    void PrepareArmFPRegion(OpContext&);

    bool ArmNativeFP() const { return options.arm_fpsr_offset != UINT32_MAX; }

    void HarvestArmFPStatus(bool saved = false, bool restore = true);
    void CheckArmFPResult(
            OpContext&, unsigned bits, unsigned lanes, Label& slow, bool only_nan = false);
    void EmitIndexedFPBroadcast(u32 rhs_node, unsigned bits, unsigned index);
    void EmitFPConstants();
    void EmitColdPaths();
    void Vector(OpContext&);
    void Crypto(OpContext&);
    void Query(OpContext&);
    void X87(OpContext&);
    void String(OpContext&);
    void VectorLanes(OpContext&);
    void ExtractLane(const Xmm&, unsigned, unsigned, const Reg64&, bool sign = false);
    void InsertLane(const Xmm&, unsigned, unsigned, const Reg64&);

    template <OpCode Code> void Handle(Op<Code>, OpContext&) {
        Reject(CompileError::UnsupportedOpcode);
    }

#define X64_OP(name) void Handle(Op<OpCode::name>, OpContext&);
#include "ops.inc"
#undef X64_OP

    const Program& source;
    const Allocation& allocation;
    CompileResult& output;
    CompileOptions options;
    NodeId current;
    BlockId block;
    u32 fallthrough_block{BlockId::invalid};
    u64 guest_pc{};
    bool has_fault_location{};
    bool static_pc_known{}, terminated{};
    u64 static_pc{};
    std::uint32_t frame_bytes{}, mxcsr_slot{};
    std::array<u32, 16> scalar_residents{}, vector_residents{};
    std::vector<Xbyak::Label> blocks, locals;
    CallMasks saved_calls;
    bool in_call{}, fault_committed_flags{};

    struct FlagsState {
        bool dirty{};
        std::uint8_t native{}, saved{};
        bool carry{}, saved_carry{}, floating{};

        FlagsState() = default;

        FlagsState(bool dirty, bool native, bool saved, bool carry, bool saved_carry)
                : dirty(dirty)
                , native(native ? 15 : 0)
                , saved(saved ? 15 : 0)
                , carry(carry)
                , saved_carry(saved_carry) {}
    } flags;

    bool defer_flags{};
    bool condition_saved{};
    unsigned flags_stack_bias{};
    unsigned stack_bias{};
    bool arm_fp_region{};
    u32 arm_fp_control{}, arm_fp_status{};

    struct FPCallKey {
        u32 control{}, status{}, format{}, scalars{}, vectors{};
        bool region{}, flags_saved{};
        friend bool operator==(const FPCallKey&, const FPCallKey&) = default;
    };

    struct FPCallHash {
        std::size_t operator()(const FPCallKey& key) const noexcept {
            std::size_t hash = key.control;
            for (auto value : {key.status,
                               key.format,
                               key.scalars,
                               key.vectors,
                               u32(key.region),
                               u32(key.flags_saved)})
                hash ^= value + std::size_t{0x9e3779b9} + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    struct FPColdPath {
        NodeId node;
        Label entry, resume;
        u32 stub{};
        FlagsState flags;
        std::uint8_t dest_reg{};
        std::int8_t fp_const{-1};
        u32 bcast_node{NodeId::invalid};
        std::uint8_t bcast_key{};
    };

    struct FPCallStub {
        FPCallKey key;
        Label entry;
    };

    struct EventColdPath {
        Label entry;
        FlagsState flags;
        u64 pc{};
    };

    std::vector<FPColdPath> cold_fp;
    std::vector<FPCallStub> fp_stubs;
    std::unordered_map<FPCallKey, u32, FPCallHash> fp_stub_index;
    std::vector<EventColdPath> cold_events;

    struct NumericColdPath {
        NodeId node;
        Label entry, resume;
        CallMasks masks;
        FlagsState flags;
        u64 pc{};
        std::uint8_t dest_reg{};
        std::int8_t fp_const{-1};
        u32 bcast_node{NodeId::invalid};
        std::uint8_t bcast_key{};
    };

    std::vector<NumericColdPath> cold_numeric;

    struct NumericCallKey {
        u64 encoded{};
        u32 control{}, status{}, scalars{}, vectors{};
        bool flags_saved{}, saved_carry{};
        friend bool operator==(const NumericCallKey&, const NumericCallKey&) = default;
    };

    struct NumericCallHash {
        std::size_t operator()(const NumericCallKey& key) const noexcept {
            std::size_t hash = key.encoded ^ (key.encoded >> 32);
            for (auto value : {key.control,
                               key.status,
                               key.scalars,
                               key.vectors,
                               u32(key.flags_saved),
                               u32(key.saved_carry)})
                hash ^= value + std::size_t{0x9e3779b9} + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

    struct NumericCallStub {
        NumericCallKey key;
        Label entry;
    };

    std::vector<NumericCallStub> numeric_stubs;
    std::unordered_map<NumericCallKey, u32, NumericCallHash> numeric_stub_index;
    std::array<Label, 4> fp_constants;
    std::array<bool, 4> fp_constants_used{};
    std::int8_t xmm15_fp_const{-1};
    bool kept_xmm15_fp_const{};
    u32 xmm0_node{NodeId::invalid};
    bool kept_xmm0{};
    u32 xmm1_bcast_node{NodeId::invalid};
    std::uint8_t xmm1_bcast_key{};
    bool kept_xmm1_bcast{};

    bool Linked() const { return options.dispatcher && options.exit_handler; }

private:
    using ExtraEntryList = std::vector<std::pair<unsigned, unsigned>>;

    void Prologue();
    void Terminal(const StoredTerminal&);
    void Dispatch(OpContext&);
    void PackFlags(const Reg64&, const Reg64&, u64 selected);

    // Core orchestration & terminal helpers (emit.cpp)
    bool PreservesScratchFpCaches(OpCode opcode) const;
    void ValidateCompileOptions();
    void EmitUnitEntry(u32 block_id, u64 pc, bool fp_guard);
    void ResetBlockState(u32 block_id);
    void EmitBlock(u32 block_id, ExtraEntryList& extra_entries);
    void EmitNode(u32 node_index, u32 block_id, ExtraEntryList& extra_entries);
    void UpdateScratchCaches(const OpContext& c, bool preserve_fp_caches);
    void WriteUniformBackedResult(OpContext& c, const Home& home);
    void LoadTerminalValue(const StoredTerminal& t, bool mask_rematerialized);
    void EmitBranchTerminal(const StoredTerminal& t);
    void EmitTerminalZeroOrBitBranch(const StoredTerminal& t,
                                     OpCode op,
                                     CellView first,
                                     CellRange::Iterator input,
                                     bool needs_flags);
    void EmitTerminalConditionBranch(const StoredTerminal& t,
                                     ir::Condition condition,
                                     bool needs_flags);
    void EmitTerminalFlagMaskBranch(const StoredTerminal& t,
                                    OpCode op,
                                    u64 mask_payload,
                                    bool needs_flags);
    void EmitReturnTerminal(const StoredTerminal& t);
    bool InterruptibleDispatch() const;
    void EmitDispatchTerminal();

    // Operand & home read helpers (emit_operand.cpp)
    void ReadValueOperand(CellView c, const Reg64& r, bool raw, bool narrow);
    void ReadVectorHomeAsScalar(const Home& h, const Reg64& r, bool narrow);
    void ReadScalarHome(u32 id, const Home& h, const Reg64& r, bool narrow);
    void ReadV128ValueToVector(u32 id, const Home& h, const Xmm& r);
    bool TryReadScalarHomeToVector(ir::Type natural, const Home& h, const Xmm& r);

    // Uniform & memory addressing helpers (emit_memory.cpp)
    void EmitLoadUniform(OpContext& c,
                         u64 off,
                         unsigned width,
                         unsigned bytes,
                         bool flags_access,
                         int pinned,
                         std::int64_t relative);
    void EmitStoreUniform(CellView u,
                          CellView value,
                          u64 off,
                          unsigned width,
                          bool flags_access,
                          int pinned,
                          std::int64_t relative);
    void UpdateUniformStoreState(CellView value, u64 off, unsigned width, bool flags_access);
    bool ConstantAddressDisp32(CellView imm_cell, bool negate, std::int32_t& out) const;
    bool IsPlainAddressValue(CellView c) const;
    Reg64 LoadAddressBaseReg(CellView c);
    Reg64 LoadAddressIndexReg(CellView c, bool uxtw, CellView base_cell, Reg64 base_reg);
    Xbyak::RegExp EmitFusedValueAddress(CellView root);
    bool TryEmitExpressionAddress(CellView first,
                                  CellRange::Iterator& at,
                                  CellRange::Iterator end,
                                  Xbyak::RegExp& out);
    void EmitMemoryLoad(OpContext& c);
    void EmitMemoryStore(OpContext& c);

    // Scalar arithmetic, logical & shift helpers (emit_scalar.cpp)
    void EmitCarryInput();
    bool TryEmitReusedRhsArithmetic(OpContext& c, const Reg64& destination, bool preserve_flags);
    bool TryEmitImmediateOrLeaArithmetic(OpContext& c,
                                         const Reg64& destination,
                                         bool can_lea,
                                         bool preserve_flags);
    bool TryEmitMemoryArithmetic(OpContext& c, const Reg64& destination);
    bool TryEmitImm64AndMask(OpContext& c, CellView left, const Reg64& destination);
    bool TryEmitImmediateLogical(OpContext& c, CellView left, const Reg64& destination);
    bool TryEmitBmiAndNot(OpContext& c,
                          CellView left,
                          CellRange::Iterator& at,
                          const Reg64& destination);
    bool TryEmitMemoryLogical(OpContext& c, CellView left, const Reg64& destination);
    bool TryEmitBmi2Rorx(OpContext& c, const Reg64& destination);

    // Flag state helpers (emit_flags.cpp)
    void SavePartialNativeFlags();
    void SaveFullNativeFlags(bool canonical);
    bool IsFlagNeutralOp(OpContext& c) const;
    void EmitConditionSlotSetcc(unsigned cc);

    // Vector helpers (emit_vector.cpp)
    void EmitVectorLaneOp(OpCode op, unsigned bits, bool uns);
    void EmitAvxVectorBinaryInsn(
            OpContext& c, const Xmm& dst, const Xmm& s1, const Xbyak::Operand& s2, unsigned n);
    bool TryEmitAvxVectorBinary(OpContext& c, const Xmm& dst, unsigned lane_bits);
    bool TryEmitSseVectorBitwiseOrAddSub(OpCode op, const Xmm& dst, unsigned lane_bits);
    bool TryEmitSseVectorCompareOrMinMax(OpContext& c, const Xmm& dst, unsigned lane_bits);
    void EmitVectorShift(OpContext& c, bool left, bool arithmetic, bool imm);
    void EmitByteImmVectorShift(OpContext& c, bool left);
    void EmitHardwareVectorShift(
            OpContext& c, unsigned lane_bits, bool left, bool arithmetic, bool imm);
    void EmitLaneLoopVectorShift(
            OpContext& c, unsigned lane_bits, bool left, bool arithmetic, bool imm);

    // Floating-point & ARM FP binary helpers (emit_float.cpp)
    void LoadAvxBroadcast2S(CellView rhs, unsigned index, const Xmm& dst);
    void EmitFloatHelperLoop(OpContext& c, unsigned bits, unsigned flags, bool fused, bool scalar);
    void EmitFloatRound(OpContext& c, unsigned bits, bool scalar);
    void EmitFloatCompareMask(OpContext& c, unsigned bits, bool scalar);
    void EmitFloatUnary(OpContext& c, unsigned bits, bool scalar);
    void EmitFloatMinMax(OpContext& c, unsigned bits, bool scalar);
    void EmitFloatArithmetic(OpCode op, unsigned bits, bool scalar);
    bool TryEmitAvxArmFPBinary(OpContext& c,
                               FPColdPath& path,
                               u64 format,
                               unsigned bits,
                               unsigned lanes,
                               unsigned kind);
    bool EmitAvxArmFPBinary128(
            OpContext& c, const Xmm& dst, u64 format, unsigned bits, unsigned kind);
    void EmitAvxArmFPBinary2S(OpContext& c, const Xmm& dst, u64 format, unsigned kind);
    bool EmitAvxArmFPBinaryScalar32(OpContext& c, const Xmm& dst, u64 format, unsigned kind);
    bool EmitAvxArmFPBinaryScalar64(OpContext& c, const Xmm& dst, u64 format, unsigned kind);
    void CheckSseArmFPBinaryCentralInputs(unsigned bits, unsigned lanes, Label& slow);
    void EmitSseArmFPBinary(OpContext& c,
                            FPColdPath& path,
                            u64 status,
                            u64 format,
                            unsigned bits,
                            unsigned lanes,
                            unsigned kind,
                            bool regional,
                            bool post_check);

    // Out-of-line cold paths & helper stubs (emit_cold.cpp)
    void EmitFPColdPaths(u32 tmp);
    void EmitFPCallStubs(u32 tmp);
    void EmitNumericColdPaths(u32 tmp);
    void EmitNumericCallStubs(u32 tmp);
    void EmitEventColdPaths();

    // ARM64 scalar/vector numeric fast paths (emit_arm_float.cpp)
    bool DecodeArmNumericRequest(OpContext& c, ArmNumericRequest& req);
    void ValidateTypedArmNumeric(OpContext& c, const ArmNumericRequest& req);
    void EmitAvxFmaInsn(const Xmm& dst,
                        const Xmm& lhs,
                        const Xbyak::Operand& rhs,
                        const ArmNumericRequest& req);
    bool EmitAvxArmFusedScalarRhs(CellView arg1,
                                  const Xmm& dst,
                                  const Xmm& lhs,
                                  const ArmNumericRequest& req);
    bool EmitAvxArmFusedVectorRhs(CellView arg1,
                                  const Xmm& dst,
                                  const Xmm& lhs,
                                  const ArmNumericRequest& req);
    bool TryEmitAvxArmFusedFP(OpContext& c,
                              NumericColdPath& path,
                              const ArmNumericRequest& req);
    void LoadSseArmNumericInputs(OpContext& c, Label& slow, const ArmNumericRequest& req);
    void EmitSseArmNumericCompare(const ArmNumericRequest& req);
    void EmitSseArmNumericVectorToInt(Label& slow, const ArmNumericRequest& req);
    void EmitSseArmNumericToInt(Label& slow, const ArmNumericRequest& req);
    void EmitSseArmNumericVectorFromInt(OpContext& c, Label& slow, const ArmNumericRequest& req);
    void EmitSseArmNumericFromInt(OpContext& c, Label& slow, const ArmNumericRequest& req);
    void EmitSseArmNumericValueOp(OpContext& c, Label& slow, const ArmNumericRequest& req);
    void EmitSseArmNumericFastPath(OpContext& c,
                                   NumericColdPath& path,
                                   const ArmNumericRequest& req);
    void EmitSseArmNumericBody(OpContext& c, Label& slow, const ArmNumericRequest& req);
};

inline Reg R(const Reg64& r, unsigned bits) {
    return bits <= 8 ? Reg(Reg8(r.getIdx(), r.getIdx() >= 4)) : r.changeBit(bits);
}

std::uint64_t PackedFlagMask(u64);
}  // namespace swift::runtime::backend::x64::detail
