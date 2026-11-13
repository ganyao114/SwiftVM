#pragma once

#include "allocate.h"
#include "compile.h"
#include "helpers.h"
#include "runtime/backend/allocation_cfg.h"
#include "runtime/ir/flag_effects.h"

#include <array>
#include <cstdint>
#include <span>

namespace swift::runtime::backend::x64::detail {
using namespace ir;
using u32 = std::uint32_t;

inline constexpr std::array<unsigned, 6> kPinRegisters{13, 14, 15, 5};
inline constexpr unsigned kRegisterBankSize = 12;

constexpr bool SupportedType(Type type) noexcept {
    return type == Type::Void || IntegerType(type) || type == Type::F32 || type == Type::F64 ||
           type == Type::V128;
}

constexpr bool UnsupportedStorage(OpCode opcode) noexcept {
    switch (opcode) {
        case OpCode::AddPhi:
        case OpCode::DefineLocal:
        case OpCode::LoadLocal:
        case OpCode::StoreLocal:
            return true;
        default:
            return false;
    }
}

constexpr bool IsArmFPValueOp(OpCode opcode) noexcept {
    switch (opcode) {
        case OpCode::ArmFPTernary:
        case OpCode::ArmFPUnary:
        case OpCode::ArmFPConvert:
        case OpCode::ArmFPCompare:
            return true;
        default:
            return false;
    }
}

constexpr bool UsesColdFPStub(OpCode opcode) noexcept {
    return opcode == OpCode::ArmFPBinary || opcode == OpCode::CallLambda || IsArmFPValueOp(opcode);
}

constexpr bool UsesArmFPCR(OpCode opcode) noexcept {
    return UsesColdFPStub(opcode) || opcode == OpCode::ArmFPBitUnary;
}

constexpr bool IsGuestSafepointOp(OpCode opcode) noexcept {
    return opcode == OpCode::GuestSafepoint || opcode == OpCode::GuestDispatchSafepoint;
}

constexpr bool AlwaysNeedsCall(OpCode opcode) noexcept {
    if (IsArmFPValueOp(opcode)) return true;
    switch (opcode) {
        case OpCode::CompareAndSwap128:
        case OpCode::CallLambda:
        case OpCode::ArmFPBinary:
        case OpCode::Cpuid:
        case OpCode::ReadSystem:
        case OpCode::ReadSystemPair:
        case OpCode::X87Op:
        case OpCode::X87Execute:
        case OpCode::Div128:
        case OpCode::VecFRoundInt:
        case OpCode::VecFMulAdd:
        case OpCode::Sse42Str:
        case OpCode::Sse42StrExplicit:
        case OpCode::VecAesEncFast:
        case OpCode::VecAesDecFast:
        case OpCode::VecAesEncLastFast:
        case OpCode::VecAesDecLastFast:
        case OpCode::VecAesImc:
        case OpCode::VecAesEnc:
        case OpCode::VecAesDec:
        case OpCode::VecAesEncLast:
        case OpCode::VecAesDecLast:
        case OpCode::VecAesKeygenAssist:
        case OpCode::VecPclMul:
        case OpCode::VecSha256Msg1:
        case OpCode::VecSha256Msg2:
        case OpCode::VecSha256Rnds2:
            return true;
        default:
            return false;
    }
}

constexpr bool ProducesNativeFlagsOp(OpCode opcode, bool bmi1) noexcept {
    switch (opcode) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Neg:
        case OpCode::Adc:
        case OpCode::Sbb:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
            return true;
        case OpCode::AndNot:
            return bmi1;
        default:
            return false;
    }
}

constexpr bool DestructiveInput(const Operation& operation) noexcept {
    switch (operation.opcode) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::Adc:
        case OpCode::Sbb:
        case OpCode::Neg:
        case OpCode::Mul:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::AndNot:
        case OpCode::LslImm:
        case OpCode::LslValue:
        case OpCode::LsrImm:
        case OpCode::LsrValue:
        case OpCode::AsrImm:
        case OpCode::AsrValue:
        case OpCode::RorImm:
        case OpCode::RorValue:
        case OpCode::ZeroExtend32:
        case OpCode::ZeroExtend32To64:
        case OpCode::ZeroExtend64:
        case OpCode::SignExtend:
            return operation.type == Type::U32 || operation.type == Type::U64;
        case OpCode::VecAnd:
        case OpCode::Vec4And:
        case OpCode::VecOr:
        case OpCode::Vec4Or:
        case OpCode::VecXor:
        case OpCode::VecAdd:
        case OpCode::Vec4Add:
        case OpCode::VecSub:
        case OpCode::Vec4Sub:
        case OpCode::VecShuffle32:
        case OpCode::VecZip:
        case OpCode::VecUnzip:
        case OpCode::VecExtend:
        case OpCode::VecShiftLeftImm:
        case OpCode::VecShiftRightImm:
        case OpCode::VecShiftRightArithmeticImm:
        case OpCode::BitCast:
            return operation.type == Type::V128;
        default:
            return false;
    }
}

// The private owner only borrows the original request and its actual result.
// Control flow is shared; register and auxiliary-slot policy stays here.
class Allocator {
public:
    Allocator(const Program& program,
              Scope scope,
              Allocation& result,
              unsigned pinned_slots,
              const CompileOptions* options)
            : program(program)
            , scope(scope)
            , result(result)
            , operations(program.Operations())
            , blocks(program.Blocks())
            , pinned_slots(pinned_slots)
            , options(options) {}

    void Run();

private:
    struct FlagSummary {
        unsigned char gen{}, kill{};
        bool queued{true};
    };

    struct FusedAddressState {
        CellView base{}, index{};
        bool has_base{}, has_index{}, index_uxtw{};
        unsigned scale{1}, fused_count{};
        std::int64_t disp{};
        std::array<u32, 5> fused{};
    };

    const Program& program;
    Scope scope;
    Allocation& result;
    std::span<const Operation> operations;
    std::span<const BlockRecord> blocks;
    backend::detail::AllocationCFG cfg;
    std::array<unsigned, 8> scalar_registers{};
    unsigned scalar_register_count{};
    unsigned pinned_slots{};
    const CompileOptions* options{};
    bool graph{};

    void ResetResult(CompileError error, NodeId node = {}, BlockId block = {});
    bool InitializeHomes();
    bool OperationNeedsCall(const Operation& operation, NodeId id) const;
    bool CanFoldTerminalCondition(const BlockRecord& body, u32 node) const;
    void ObserveFPCR(const Operation& operation, NodeId id);
    bool Reserve(u32 bytes, u32 alignment, u32 node, u32& offset);
    bool AllocatePayloads();

    // Register assignment helpers (allocate.cpp)
    void AssignRegisters();
    void ExpireDeadResidents(std::span<u32> bank, u32 position) const;
    bool TryRematerializeStoreUniformImm(u32 node, const BlockRecord& body);
    unsigned FindReusableInputSlot(CellView input,
                                   bool vector,
                                   u32 definition_position,
                                   std::span<const u32> bank) const;
    unsigned TryReuseOperandRegister(u32 node,
                                     bool vector,
                                     std::span<const u32> bank,
                                     bool& reused);
    unsigned TryReuseArmFPVectorInput(u32 node, std::span<const u32> bank, bool& reused) const;
    bool HasZeroSpillCost(u32 id) const;
    unsigned SelectEvictionVictim(u32 node, std::span<const u32> bank) const;
    bool SpillNode(u32 id, std::span<u32> residents);

    // Use & fusion selection passes (allocation_selection.cpp)
    Type ViewOf(CellView cell) const;
    bool Immediate32Cell(CellView cell, unsigned width, std::int32_t& immediate) const;
    bool IsZeroScalarCell(CellView cell, unsigned width) const;
    bool IsZeroIdentityCopyNode(u32 node) const;
    bool IsImmediateUniformStoreNode(u32 node) const;
    bool RecordUse(u32 definition, u32 at, u32 block = BlockId::invalid);
    bool ObserveUses();
    bool RecordAllUses();
    void MarkZeroImmediateRematerialization();
    bool HasSimpleScalarLoadAddress(u32 node) const;
    void TryFuseScalarLoadToVector(const BlockRecord& body, u32 node);
    bool IsPlainU64Value(CellView cell) const;
    bool IsUxtwSourceValue(CellView cell) const;
    bool ConstantDisp32(CellView cell, bool negate, std::int64_t& out) const;
    bool CanFuseAddressNode(u32 block_id, u32 node, u32 expected_pos) const;
    bool MatchUxtwAddressNode(u32 block_id, u32 node, u32 expected_pos, CellView& src) const;
    bool AddAddressLeaf(FusedAddressState& state, CellView cell) const;
    bool MatchAddressTree(u32 block_id,
                          FusedAddressState& state,
                          CellView cell,
                          u32 expected_pos,
                          unsigned depth) const;
    void TryFuseMemoryAddressing(u32 block_id, u32 node);

    // Flag liveness & native flag carrier analysis (allocation_selection.cpp)
    void AnalyzeFlags();
    unsigned ComputeNodeFlagWrites(const Operation& op, CellRange inputs) const;
    void ComputeBlockFlagSummaries(const UniformPolicy& uniforms,
                                   std::span<FlagSummary> facts,
                                   std::span<u32> offsets);
    void PropagateFlagLiveness(std::span<FlagSummary> facts,
                               std::span<const u32> offsets,
                               std::span<const u32> predecessors,
                               std::span<u32> queue);
    void AssignBlockFlagNeeds(u32 block_id);
    bool ComputeBlockNativeFlagOutput(u32 block_id) const;
    bool ExpressionPreservesNativeFlags(CellRange::Iterator& at, CellRange::Iterator end) const;
    bool InstructionPreservesNativeFlags(u32 node) const;

    // Uniform backing analysis (allocation_selection.cpp)
    void AnalyzeUniformBacking();
    bool IsOrdinaryUniformRange(UniformRange range) const;
    bool IsAdjacentToUniformStore(u32 producer_id, u32 store_id, UniformRange store_range) const;
    void TryBackStoreUniform(u32 at,
                             CellRange inputs,
                             u32 block_end,
                             u32 barrier,
                             const std::array<u32, 128>& next_write);
    void TryBackLoadUniform(u32 at,
                            CellRange inputs,
                            u32 block_end,
                            u32 barrier,
                            const std::array<u32, 128>& next_write);
    void RecordUniformWriteHazards(u32 at,
                                   CellRange inputs,
                                   const UniformPolicy& policy,
                                   u32& barrier,
                                   std::array<u32, 128>& next_write) const;
};
}  // namespace swift::runtime::backend::x64::detail
