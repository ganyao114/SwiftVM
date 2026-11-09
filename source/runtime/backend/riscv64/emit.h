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

    void RecordGuestAccess(u32 first, unsigned access, unsigned bytes) {
        if (!options.fault_locations || !has_fault_location) return;
        output.faults.push_back({first, first + 4, guest_pc});
        auto& site = output.faults.back();
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
    u64 static_pc{};

private:
    struct Jump {
        u32 offset, target;
        bool local;
    };

    std::vector<u32> block_offsets, local_offsets;
    std::vector<Jump> jumps;
    void JumpTo(u32 target, bool local = false);
    void Prologue();
    void Restore();
    void Terminal(const StoredTerminal&);
    void FinishResult(OpContext&);
    void SaveProducerFlags(OpContext&, GPR raw);
};

u64 PackedFlagMask(u64 selected);
}  // namespace swift::runtime::backend::riscv64::detail
