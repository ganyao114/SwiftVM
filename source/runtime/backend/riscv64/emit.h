#pragma once

// Private implementation surface for the one RV64 word writer. Handlers
// consume the original Program and its fixed homes; no lowered IR is built.
#include "allocate.h"
#include "compile.h"
#include "relocate.h"

#include <array>
#include <type_traits>
#include <biscuit/assembler.hpp>

namespace swift::runtime::backend::riscv64::detail {
using namespace ir;
using namespace biscuit;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
template <OpCode Code> using Op = std::integral_constant<OpCode, Code>;

constexpr u32 kStandaloneSaveBytes = 112;

constexpr bool FlagProducer(OpCode code) {
    switch (code) {
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

struct Rejected {
    CompileError error;
    NodeId node;
    BlockId block;
};

struct ShortJumpOverflow {};

struct OpContext {
    const Operation& operation;
    std::array<CellView, 5> args{};
    unsigned count{};
    GPR destination{a4}, high{a5};
    NodeId node;
    unsigned width{};
    CellRange operands;
    bool saved{};

    CellView Arg(unsigned i) const { return args[i]; }
};

class Writer {
public:
    Writer(const Program&,
           const Allocation&,
           CompileResult&,
           CompileOptions,
           unsigned jump_mode = 2);
    void Run();

    [[nodiscard]] bool Linked() const noexcept {
        return options.dispatcher != 0 && options.exit_handler != 0;
    }

    [[nodiscard]] u32 ByteOffset() const noexcept {
        return static_cast<u32>(output.words.size() * 4);
    }

    // These methods are internal to this backend, shared by responsibility
    // files. Architectural control never enters the generic IR contract.
    Type View(CellView) const;
    void Read(CellView, GPR destination, bool raw = false);
    GPR ReadRef(CellView, GPR scratch, bool raw = false);
    GPR ReadWidth(CellView, GPR scratch, unsigned width);
    void ReadPair(CellView, GPR low, GPR high);
    void ReadExpr(CellRange::Iterator&, CellRange::Iterator, GPR destination, unsigned width);
    GPR ReadExprRef(CellRange::Iterator&, CellRange::Iterator, GPR scratch, unsigned width);
    void Mask(GPR, unsigned bits);
    void SignExtend(GPR, unsigned bits);
    void Load(GPR destination, GPR base, std::int32_t offset, unsigned bits);
    void Store(GPR source, GPR base, std::int32_t offset, unsigned bits);
    void FrameLoad(GPR, u32 offset);
    void FrameStore(GPR, u32 offset);
    void Address(GPR destination, GPR base, std::int64_t offset);
    void FrameDelta(std::int64_t);
    void Call(std::uintptr_t);
    void Call(std::uintptr_t, HostSymbol);
    void HostAddress(GPR, std::uintptr_t, HostSymbol, std::int64_t addend = 0);
    void CallRegister(GPR target);
    void Exit(u64 reason);
    void StaticExit(u64 pc);
    void Condition(GPR, ir::Condition);
    void MergeFlags(GPR raw, u64 selected, bool clean_input = false);
    void SaveProducerFlags(OpContext&, GPR raw, bool clean_input = false);
    void EmitArithmeticFlags(OpContext&,
                             GPR lhs,
                             GPR rhs,
                             GPR raw_result,
                             GPR carry,
                             bool subtract);
    void EmitLogicalFlags(OpContext&);
    void Expression(CellRange::Iterator&, CellRange::Iterator, GPR, unsigned depth = 0);
    void Reject(CompileError);

    void Dispatch(OpContext&);
#define RV_SCALAR_OP(name) void Handle(Op<OpCode::name>, OpContext&);
#include "scalar_ops.inc"
#undef RV_SCALAR_OP
#define RV_VECTOR_OP(name) void Handle(Op<OpCode::name>, OpContext&);
#include "vector_ops.inc"
#undef RV_VECTOR_OP

    void IntegerArithmetic(OpContext&);
    void LogicalArithmetic(OpContext&);
    void Shift(OpContext&);
    void Predicate(OpContext&);
    void BitField(OpContext&);
    void CountBits(OpContext&);

    void RecordGuestAccessAt(u32 first,
                             bool has_location,
                             u64 pc,
                             unsigned access,
                             unsigned bytes) {
        if (!options.fault_locations || !has_location) return;
        if (!output.faults.empty()) {
            auto& prev = output.faults.back();
            if (prev.last_offset == first && prev.guest_pc == pc &&
                prev.recovery[4] == static_cast<std::uint8_t>(access) &&
                prev.recovery[5] == static_cast<std::uint8_t>(bytes)) {
                prev.last_offset = first + 4;
                return;
            }
        }
        output.faults.push_back({first, first + 4, pc});
        auto& site = output.faults.back();
        site.recovery[4] = static_cast<std::uint8_t>(access);
        site.recovery[5] = static_cast<std::uint8_t>(bytes);
    }

    void RecordGuestAccess(u32 first, unsigned access, unsigned bytes) {
        RecordGuestAccessAt(first, has_fault_location, guest_pc, access, bytes);
    }

    struct ColdMemoryStub {
        u32 branch_offset{};
        u32 resume_offset{};
        u64 fault_pc{};
        std::int32_t disp{};
        std::uint8_t bits{};
        std::uint8_t base_reg{};
        std::uint8_t low_reg{};
        std::uint8_t high_reg{};
        bool store{};
        bool has_fault{};
    };

    void QueueColdMemoryStub(const ColdMemoryStub& stub);
    [[nodiscard]] bool NeedColdStubFlush(bool reserve_jump) const noexcept;
    void FlushColdStubs(bool jump_over);

    void Memory(OpContext&);
    void Uniform(OpContext&);
    void SelectValue(OpContext&);
    void Atomic(OpContext&);
    void X87(OpContext&);
    void CpuQuery(OpContext&);
    void String(OpContext&);

    const Program& source;
    const Allocation& allocation;
    CompileResult& output;
    CompileOptions options;
    Assembler as;
    NodeId current;
    BlockId block;
    u64 guest_pc{};
    unsigned jump_mode{2};
    bool has_fault_location{};
    bool static_pc_known{}, terminated{};
    u64 static_pc{};

private:
    enum class JumpKind : std::uint8_t {
        AuipcJalr,
        Jal,
        BranchEqZero,
        BranchNeZero,
    };

    struct Jump {
        u32 offset{};
        u32 target{};
        JumpKind kind{JumpKind::AuipcJalr};
        std::uint8_t reg{};
        bool local{};
    };

    std::vector<u32> block_offsets, local_offsets;
    std::vector<Jump> jumps;
    std::vector<ColdMemoryStub> cold_stubs;
    u32 cold_stub_bytes{};

    bool HasSecondaryEntry(u32 block_index) const;
    void InstallGuestRounding(GPR scratch0, GPR scratch1);
    void JumpTo(u32 target, bool local = false);
    void BranchZeroTo(GPR reg, bool nonzero, u32 target, bool local = false);
    void PatchJumps();
    void Prologue();
    void Restore();
    void Terminal(const StoredTerminal&, u32 next_block);
    void FinishResult(OpContext&);
};

u64 PackedFlagMask(u64 selected);
}  // namespace swift::runtime::backend::riscv64::detail
