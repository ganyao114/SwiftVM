//
// Created by 甘尧 on 2023/10/13.
//

#pragma once

#include <bit>
#include "base/common_funcs.h"
#include "runtime/common/types.h"
#include "runtime/common/ra_shape_prof.h"
#include "runtime/ir/block.h"
#include "runtime/ir/host_reg.h"

namespace swift::runtime::backend {

template<typename T = u32>
class RegisterMask {
public:

    explicit RegisterMask() : mask() {}

    explicit RegisterMask(T mask) : mask(mask) {}

    auto GetFirstMarked() {
        return std::countr_zero(mask);
    }

    auto GetFirstClear() {
        constexpr auto bit_count = static_cast<int>(sizeof(T) * 8);
        auto idx = std::countr_one(mask);
        // All bits marked → no clear register; countr_one returns the
        // bit-width, which is not a valid register index.
        return idx < bit_count ? idx : -1;
    }

    auto GetMarkedCount() {
        return std::popcount(mask);
    }

    auto GetClearCount() {
        return GetAllCount() - std::popcount(mask);
    }

    auto GetAllCount() {
        return sizeof(T) * 8;
    }

    [[nodiscard]] bool Null() const {
        return mask == 0;
    }

    [[nodiscard]] bool Get(u32 bit) const {
        return mask & (T(1) << bit);
    }

    bool operator==(const RegisterMask&) const = default;

    void Mark(u32 bit) {
        mask |= (T(1) << bit);
    }

    void Clear(u32 bit) {
        mask &= ~(T(1) << bit);
    }

    void Reset(T value) {
        mask = value;
    }

private:
    T mask;
};

using GPRSMask = RegisterMask<u32>;
using FPRSMask = RegisterMask<u32>;

// Raw XPOOL request and effective state. Pin level 3 forces the effective state
// on because pinning x6-x9 would otherwise leave only x14/x15 as value GPRs.
[[nodiscard]] bool ScratchXPoolRequested();
[[nodiscard]] bool ScratchXPoolAutoEnabled();
[[nodiscard]] bool X86PinExtLevel3Requested();

// True only for an actual level-2/3 x86 pool: the environment level alone is
// insufficient because swift_test and non-x86 frontends can construct an
// AddressSpace without the expected static descriptors while inheriting vars.
[[nodiscard]] bool X86PinExtLevel2Enabled(const GPRSMask& pool);
[[nodiscard]] bool X86PinExtLevel3Enabled(const GPRSMask& pool);
[[nodiscard]] bool X86PinExtScratchOnlyEnabled(const GPRSMask& pool);
// Level 3 can lease the bias-mode x10 reservation to pure high-pressure ALU
// lowering.
// It never enters the value pool and memory opcodes never see it as scratch.
[[nodiscard]] bool X86PinExtLevel3AluScratchEnabled(const GPRSMask& pool,
                                                    ir::OpCode op);

// Number of u64 spill slots reserved in State::spill_area (context.h).
// The linear-scan pass panics instead of handing out a slot beyond this:
// an out-of-range slot would silently overwrite the uniform buffer that
// follows the spill area. Kept in sync with State::spill_area by a
// static_assert in arm64/jit/jit_context.cpp.
//
// This is a budget for spills LIVE AT ONCE, not for spills performed. It used
// to be the latter -- the allocator never returned a slot -- which made a unit
// that spilled repeatedly abort even though it needed a handful of slots at any
// instant. With recycling (register_alloc_pass.cpp, ExpireOldIntervals) the
// measured high-water marks are: avx_real_x86_64 under SVM_AVX=1 fell from 50
// slots to 3 (57 spills either way), swift_test's saturation blocks from 55 to
// 28, and the block-mode x87 guests are unchanged at 8 and 21 -- those really
// do hold that many spilled values simultaneously.
//
// What can still reach 64: the ARM64 pools are 20 allocatable GPRs and 28 FPRs,
// less the scratch reserve (3/3 by default), so a compilation unit needs more
// than ~81 scalar values -- or ~57 V128 values, which take two slots each --
// live at one instruction. No guest in the corpus comes close; the closest is
// 21. Raising the ceiling is cheap if that ever changes: spill_area sits
// immediately before State's flexible uniform buffer, every uniform offset is
// derived from offsetof(State, uniform_buffer_begin) rather than hardcoded, and
// ThreadContext64 -- whose byte layout the FXSAVE path and contract C2 depend
// on -- lives inside that buffer and does not move. The cost is 8 bytes of
// per-thread State per slot; the JIT's scaled-immediate addressing has room to
// spare (32760 bytes for the u64 form, 65520 for the 16-byte one). It is left
// at 64 because the assert now fires only on a shape nothing produces, and it
// names the unit when it does.
static constexpr u32 kMaxSpillSlots = 64;

// --- Scratch-register budget -------------------------------------------
// While the JIT emits ONE IR instruction it may take host registers that hold
// no IR value (JitContext::GetTmpX / GetTmpV / ReserveTmpX). Those come from
// whatever the register allocator left unassigned at that instruction, and
// they are only recycled at the next instruction boundary (TickIR) -- so the
// number that matters is how many an emitter holds at once, not how many it
// uses in sequence.
//
// This table is the contract between the two halves:
//   * the linear scan (register_alloc_pass.cpp) keeps at least this many
//     registers unassigned at every instruction of a compilation unit, so the
//     emitter is guaranteed to find them;
//   * JitContext asserts that no emitter exceeds its declared budget, so an
//     emitter that grows past it fails loudly and by name instead of silently
//     stealing a live value (or panicking only under high pressure, which is
//     what used to happen).
//
// Values are the measured peaks over the whole test corpus (every guest under
// every gating configuration plus swift_test); see the report accompanying
// this change. Adding an opcode needs no table edit -- the default covers the
// ordinary emitter shapes -- and if a new emitter needs more, the assert in
// JitContext::GetTmpX names it on the first emission.
struct ScratchNeed {
    u8 gpr;
    u8 fpr;
};

// W52 opt-in scratch-pool contract. OFF preserves the historical global
// reservation byte-for-byte. ON lets the allocator use x11-x17 and protects
// only the registers an opcode explicitly clobbers.
[[nodiscard]] bool ScratchXPoolEnabled();

// Bit mask of architecturally fixed GPR clobbers for one opcode. These are
// direct SwiftVM uses (atomic status/value registers, host-call/terminal
// register, cold-path return register), not VIXL's dynamically leased scratch
// registers.
[[nodiscard]] u32 FixedGPRClobbers(ir::OpCode op, bool scratch_only = false);
[[nodiscard]] u32 FixedGPRClobbers(const ir::Inst& inst,
                                   bool scratch_only = false);
inline constexpr u32 kTerminalFixedGPRClobbers = 1u << 11;

// Ordinary emitters: the widest generic shape is a 3-GPR ALU sequence
// (Add/Sub/And/Or/Xor/Mul with a folded operand) and a 3-FPR vector sequence.
inline constexpr u8 kDefaultScratchGPR = 3;
inline constexpr u8 kDefaultScratchFPR = 3;

// Every *use* of a spilled value reloads it into a scratch register
// (JitContext::SpillGPR/SpillFPR) on top of the emitting opcode's own budget,
// and a spilled destination needs one too. Reloads are memoized per
// (instruction, value), so an instruction's extra demand is exactly the number
// of distinct spilled values it names -- which the allocator counts directly
// rather than guessing at.

// Conservative budget by opcode, plus the precise instruction-level contract
// used whenever the caller has the full IR instruction. Total functions;
// never fail.
[[nodiscard]] ScratchNeed ScratchBudget(ir::OpCode op);
[[nodiscard]] ScratchNeed ScratchBudget(const ir::Inst& inst);
[[nodiscard]] ScratchNeed PreciseAddSubScratchBudget(const ir::Inst& inst);
[[nodiscard]] bool ScratchPreciseRequested();

class RegAlloc : DeleteCopyAndMove {
public:

    explicit RegAlloc(u32 instr_size, const GPRSMask& gprs, const FPRSMask& fprs);

    enum Type : u16 {
        NONE,
        GPR,
        FPR,
        MEM,
        REF
    };

    struct Map {
        Type type{NONE};
        u16 slot{};
        GPRSMask dirty_gprs{0};
        FPRSMask dirty_fprs{0};

        bool operator==(const Map&) const = default;
    };

    [[nodiscard]] const GPRSMask& GetGprs() const;
    [[nodiscard]] const FPRSMask& GetFprs() const;

    void MapRegister(u32 id, ir::HostGPR gpr);
    void MapRegister(u32 id, ir::HostFPR fpr);
    void MapMemSpill(u32 id, ir::SpillSlot slot);
    void MapReference(u32 from, u32 to);
    void SetActiveRegs(u32 id, GPRSMask &gprs, FPRSMask &fprs);

    ir::HostGPR ValueGPR(const ir::Value &value);
    ir::HostFPR ValueFPR(const ir::Value &value);
    ir::SpillSlot ValueMem(const ir::Value &value);
    ir::HostGPR ValueGPR(u32 id);
    ir::HostFPR ValueFPR(u32 id);
    ir::SpillSlot ValueMem(u32 id);
    // Resolves REF (bitcast alias) entries, so the result is the underlying
    // GPR/FPR/MEM allocation rather than the alias itself.
    Type ValueType(const ir::Value &value);

    [[nodiscard]] GPRSMask GetDirtyGPR() const;
    [[nodiscard]] FPRSMask GetDirtyFPR() const;
    // Same, for an arbitrary instruction id: the allocation pass verifies its
    // own output against every instruction, not just the current one.
    [[nodiscard]] GPRSMask DirtyGPR(u32 id) const { return alloc_result[id].dirty_gprs; }
    [[nodiscard]] FPRSMask DirtyFPR(u32 id) const { return alloc_result[id].dirty_fprs; }
    [[nodiscard]] u32 MapCount() const { return static_cast<u32>(alloc_result.size()); }
    [[nodiscard]] const Map& Mapping(u32 id) const { return alloc_result[id]; }
    [[nodiscard]] u32 SpillCount() const;
    [[nodiscard]] RAShapeUnitCounters& RAShape() { return ra_shape; }
    [[nodiscard]] const RAShapeUnitCounters& RAShape() const { return ra_shape; }

    // Refines the pool for one compilation unit after a first allocation pass
    // has proved that the unit spills. The caller must ResetAllocations and
    // rerun the pass before emission, so no existing mapping can alias the
    // newly reserved register.
    void ReserveGPRForUnit(u32 code);

    void SetCurrent(ir::Inst *inst);

    // Drops every mapping so the linear scan can be re-run over the same unit
    // with a larger scratch reserve (see RegisterAllocPass::Run). Only the
    // allocation results are cleared; the pool masks are construction state.
    void ResetAllocations();

private:
    // Follows REF chains (MapReference) to the id holding the real
    // GPR/FPR/MEM allocation.
    [[nodiscard]] u32 ResolveId(u32 id) const;

    Vector<Map> alloc_result;
    u32 stack_size{};
    ir::Inst *current_ir{};
    GPRSMask gprs;
    const FPRSMask fprs;
    RAShapeUnitCounters ra_shape{};
};

}
