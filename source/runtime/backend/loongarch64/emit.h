#pragma once

// Private implementation surface for the one LA64 word writer. Handlers
// consume the original Program and its fixed homes; no lowered IR is built.
#include "allocate.h"
#include "compile.h"
#include "relocate.h"

#include <array>
#include <type_traits>
#include "assembler.h"

namespace swift::runtime::backend::loongarch64::detail {
using namespace ir;

using u32 = std::uint32_t;
using u64 = std::uint64_t;
template <OpCode Code> using Op = std::integral_constant<OpCode, Code>;

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
    Writer(const Program&, const Allocation&, CompileResult&, CompileOptions);
    void Run();

    // These methods are internal to this backend, shared by responsibility
    // files. Architectural control never enters the generic IR contract.
    Type View(CellView) const;
    void Read(CellView, GPR destination, bool raw = false);
    void ReadPair(CellView, GPR low, GPR high);
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
    void MergeFlags(GPR raw, u64 selected);
    void EmitPackedToLbt(GPR source_packed, u32 mask = 0x3f);
    void EmitExportLBTToPacked(GPR destination);
    void ImportPackedToLBT();
    void EnsureLBT();
    void NormalizeLBT();
    void EmitFlagTest(GPR destination, u64 selected, bool negate);
    void Expression(CellRange::Iterator&, CellRange::Iterator, GPR, unsigned depth = 0);
    void Reject(CompileError);

    void Dispatch(OpContext&);
#define LA_SCALAR_OP(name) void Handle(Op<OpCode::name>, OpContext&);
#include "scalar_ops.inc"
#undef LA_SCALAR_OP
#define LA_VECTOR_OP(name) void Handle(Op<OpCode::name>, OpContext&);
#include "vector_ops.inc"
#undef LA_VECTOR_OP

    void IntegerArithmetic(OpContext&);
    void LogicalArithmetic(OpContext&);
    void Shift(OpContext&);
    void Predicate(OpContext&);
    void BitField(OpContext&);
    void CountBits(OpContext&);

    void RecordGuestAccess(u32 first, unsigned access, unsigned bytes) {
        if (!options.fault_locations || !has_fault_location) return;
        output.faults.push_back({first, first + 4, guest_pc});
        auto& site = output.faults.back();
        const bool dirty_lbt = lbt_live && lbt_dirty_mask != 0;
        site.recovery[0] =
                static_cast<std::uint8_t>((dirty_lbt ? 1u : 0u) | (dirty_lbt && carry_xor ? 2u : 0u));
        site.recovery[1] = dirty_lbt ? static_cast<std::uint8_t>(lbt_dirty_mask) : 0u;
        site.recovery[4] = static_cast<std::uint8_t>(access);
        site.recovery[5] = static_cast<std::uint8_t>(bytes);
    }

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
    bool has_fault_location{};
    bool static_pc_known{}, terminated{};
    bool lbt_canonical_blocks{}, lbt_live{}, carry_xor{};
    u32 unit_lbt_used_mask{}, unit_lbt_written_mask{}, lbt_dirty_mask{};
    u64 static_pc{};

private:
    enum class JumpKind : std::uint8_t { Unconditional, Zero, NonZero };

    struct Jump {
        u32 offset, target;
        GPR reg{x0};
        JumpKind kind{JumpKind::Unconditional};
        bool local{};
    };

    std::vector<u32> block_offsets, local_offsets;
    std::vector<Jump> jumps;
    void JumpTo(u32 target, bool local = false);
    void BranchZero(GPR reg, u32 target, bool local = false);
    void BranchNonZero(GPR reg, u32 target, bool local = false);
    void Prologue();
    void Restore();
    void Terminal(const StoredTerminal&, u32 fallthrough_block);
    void FinishResult(OpContext&);
    void SaveProducerFlags(OpContext&, GPR raw);
};

u64 PackedFlagMask(u64 selected);
u32 LbtFlagMask(u64 selected);
}  // namespace swift::runtime::backend::loongarch64::detail
