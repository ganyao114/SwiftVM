#pragma once
#ifndef XBYAK64
#define XBYAK64
#endif
#include <array>
#include <type_traits>
#include <unordered_map>
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
    void Read(CellView, const Reg64&, bool raw = false);

    Reg64 ScalarDestination(OpContext& c) const {
        // Only leaf emitters that do not call helpers borrow the assigned home.
        // Spilled results keep the ordinary RAX path and final stack store.
        const auto& home = allocation.homes[c.node.value];
        if (!home.spilled && !c.vector_result) c.destination = Reg64(home.reg);
        return c.destination;
    }

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
    void EnsureNativeFlags();
    void PrepareFlags(OpContext&);
    unsigned NativeCondition(ir::Condition);
    void NativeJump(unsigned, Label&);
    void BranchTo(BlockId);

    bool Pinned() const { return options.pinned_uniform_slots != 0; }

    static unsigned PinnedRegister(unsigned slot) {
        constexpr unsigned registers[]{13, 14, 15, 5};
        return registers[slot];
    }

    int PinnedSlot(std::uint64_t offset, unsigned bytes) const;
    void PublishPinned();
    void ReloadPinned();
    void Condition(const Reg64&, ir::Condition);
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
            site.recovery[0] = 2 | (flags.saved_carry << 3);
            site.recovery[1] = 0xf0;
            site.recovery[2] = allocation.flags_slot;
            site.recovery[3] = allocation.flags_slot >> 8;
            site.recovery[7] = allocation.flags_slot >> 16;
        }
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
    void BeginArmFPRegion(u32 control, u32 status);
    void EndArmFPRegion();
    void PrepareArmFPRegion(OpContext&);
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
        bool dirty{}, native{}, saved{}, carry{}, saved_carry{};
    } flags;

    bool defer_flags{}, merge_native_flags{};
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

    bool Linked() const { return options.dispatcher && options.exit_handler; }

private:
    void Prologue();
    void Terminal(const StoredTerminal&);
    void Dispatch(OpContext&);
    void PackFlags(const Reg64&, const Reg64&, u64 selected);
};

inline Reg R(const Reg64& r, unsigned bits) {
    return bits <= 8 ? Reg(Reg8(r.getIdx(), r.getIdx() >= 4)) : r.changeBit(bits);
}

std::uint64_t PackedFlagMask(u64);
}  // namespace swift::runtime::backend::x64::detail
