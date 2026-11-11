#pragma once
#ifndef XBYAK64
#define XBYAK64
#endif
#include <array>
#include <type_traits>
#include <xbyak/xbyak.h>
#include "allocate.h"
#include "compile.h"
#include "runtime/backend/relocation.h"
#include "runtime/exit.h"

namespace swift::runtime::backend::x64::detail {
using namespace ir;
using namespace Xbyak;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
template <OpCode Code> using Op = std::integral_constant<OpCode, Code>;

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

    CellView Arg(unsigned i) const { return args[i]; }
};

class Writer : public Xbyak::CodeGenerator {
public:
    Writer(const Program&, const Allocation&, CompileResult&, CompileOptions);
    void Run();
    ir::Type View(CellView) const;
    void Read(CellView, const Reg64&, bool raw = false);
    void ReadVector(CellView, const Xmm&);
    void WriteResult(OpContext&);
    void Mask(const Reg64&, unsigned);
    void Sign(const Reg64&, unsigned);
    void Expression(CellRange::Iterator&, CellRange::Iterator, const Reg64&, unsigned depth = 0);
    void Load(const Reg64&, const Reg64&, std::int32_t, unsigned);
    void Store(const Reg64&, const Reg64&, std::int32_t, unsigned);
    void HostAddress(const Reg64&, std::uintptr_t, HostSymbol);
    void Call(std::uintptr_t, HostSymbol);
    void CallRegister(const Reg64&);
    void PrepareCall();
    void FinishCall();
    void Exit(u64);
    void StaticExit(u64);
    void Restore();
    void PublishFlags();
    bool Pinned() const { return options.pinned_uniform_slots != 0; }
    static unsigned PinnedRegister(unsigned slot) {
        constexpr unsigned registers[]{13, 14, 15, 5};
        return registers[slot];
    }
    int PinnedSlot(std::uint64_t offset, unsigned bytes) const;
    void PublishPinned();
    void ReloadPinned();
    void Condition(const Reg64&, ir::Condition);
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
    }

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
    u64 guest_pc{};
    bool has_fault_location{};
    bool static_pc_known{}, terminated{};
    u64 static_pc{};
    std::uint32_t frame_bytes{}, mxcsr_slot{};
    std::array<u32, 16> scalar_residents{}, vector_residents{};
    std::vector<Xbyak::Label> blocks, locals;
    std::vector<unsigned> saved_scalar, saved_vectors;
    bool in_call{}, fault_committed_flags{};

    bool Linked() const { return options.dispatcher && options.exit_handler; }

private:
    void Prologue();
    void Terminal(const StoredTerminal&);
    void Dispatch(OpContext&);
    void PackFlags(const Reg64&, const Reg64&);
};

inline Reg R(const Reg64& r, unsigned bits) {
    return bits <= 8 ? Reg(Reg8(r.getIdx(), r.getIdx() >= 4)) : r.changeBit(bits);
}

std::uint64_t PackedFlagMask(u64);
}  // namespace swift::runtime::backend::x64::detail
