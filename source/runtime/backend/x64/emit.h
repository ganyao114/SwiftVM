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
    bool ResidentScalar(CellView, unsigned width, Reg64&) const;

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
    void LambdaCall(OpContext&);
    bool TryArmNumeric(OpContext&);
    void ArmFPValues(OpContext&);
    void MaterializeFPFlags();
    void CheckArmFPInput(const Xmm&, unsigned bits, unsigned lanes, bool central, Label&);
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
    void NativeJump(unsigned, Label&);
    void BranchTo(BlockId);
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

    void MemoryAddress(CellRange::Iterator&, CellRange::Iterator, Reg64&, std::int32_t&);
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
    void CheckArmFPResult(OpContext&, unsigned bits, unsigned lanes, Label& slow, bool only_nan = false);
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
