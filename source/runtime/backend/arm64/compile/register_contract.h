#pragma once

#include "runtime/backend/register_mask.h"
#include "runtime/common/svm_config.h"
#include "runtime/compile/allocate/scratch.h"
#include "runtime/compile/analysis/facts.h"
#include "runtime/ir/instr.h"

namespace swift::runtime::ir {
class Block;
}

namespace swift::runtime::backend {

// Raw XPOOL request and effective state. Pin level 3 forces the effective state
// on because pinning x6-x9 would otherwise leave only x14/x15 as value GPRs.
[[nodiscard]] bool ScratchXPoolRequested(const FeatureSet& features);
[[nodiscard]] bool ScratchXPoolAutoEnabled(const FeatureSet& features);
[[nodiscard]] bool X86PinExtLevel3Requested();

// True only for an actual level-2/3 x86 pool: the environment level alone is
// insufficient because swift_test and non-x86 frontends can construct an
// AddressSpace without the expected static descriptors while inheriting vars.
[[nodiscard]] bool X86PinExtLevel2Enabled(const GPRSMask& pool);
[[nodiscard]] bool X86PinExtLevel3Enabled(const GPRSMask& pool);
[[nodiscard]] bool X86PinExtScratchOnlyEnabled(const GPRSMask& pool, const FeatureSet& features);
// Level 3 can lease the bias-mode x10 reservation to pure high-pressure ALU
// lowering.
// It never enters the value pool and memory opcodes never see it as scratch.
[[nodiscard]] bool X86PinExtLevel3AluScratchEnabled(const GPRSMask& pool, ir::OpCode op);
[[nodiscard]] bool X86PinExtLevel3AluScratchEnabled(bool available, ir::OpCode op);

// W-gamma fixed-class gate.  The class is meaningful only with the complete
// sixteen-register level-3 architectural map; a sparse map must keep the
// established allocator unchanged instead of silently creating a hybrid ABI.
inline constexpr u32 kX86FixedGPRHomes = (0x3ffu << 0) | (0x1fu << 19) | (1u << 29);
[[nodiscard]] bool FixedGPRClassEnabled(const GPRSMask& pool, const FeatureSet& features);
[[nodiscard]] bool IsFixedGPRHome(u32 code);

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

using FlagsDemand = compile::FlagsDemand;

// Exact demand for `owner` by scanning one block. For call sites
// without a use index (standalone preparation, tests).
[[nodiscard]] FlagsDemand ScanFlagsDemand(const ir::Block& block, const ir::Inst& owner);

// Four independent GPR budgets.  `value_pool` is a count; the remaining
// fields are contracts for one emission site and never contribute registers
// to that pool.  Keeping fixed clobbers as a mask prevents the x12/x13 CAS and
// cold-continuation ABIs from being mistaken for ordinary scratch capacity.
struct GPRClassContract {
    u8 value_pool{};
    u8 hot_instruction_scratch{};
    u8 cold_edge_scratch{};
    u32 call_clobber_mask{};
    u32 fixed_clobber_mask{};
};

[[nodiscard]] GPRClassContract ClassifyGPRContract(const GPRSMask& pool,
                                                   const ir::Inst& inst,
                                                   const FeatureSet& features,
                                                   FlagsDemand demand);

// W52 opt-in scratch-pool contract. OFF preserves the historical global
// reservation byte-for-byte. ON lets the allocator use x11-x17 and protects
// only the registers an opcode explicitly clobbers.
[[nodiscard]] bool ScratchXPoolEnabled(const FeatureSet& features);

// Bit mask of architecturally fixed GPR clobbers for one opcode. These are
// direct SwiftVM uses (atomic status/value registers, host-call/terminal
// register, cold-path return register), not VIXL's dynamically leased scratch
// registers.
[[nodiscard]] u32 FixedGPRClobbers(ir::OpCode op,
                                   const FeatureSet& features,
                                   bool scratch_only = false);
[[nodiscard]] u32 FixedGPRClobbers(const ir::Inst& inst,
                                   const FeatureSet& features,
                                   bool scratch_only = false);
inline constexpr u32 kTerminalFixedGPRClobbers = 1u << 11;

// Ordinary emitters: the widest generic shape is a 3-GPR ALU sequence
// (Add/Sub/And/Or/Xor/Mul with a folded operand) and a 3-FPR vector sequence.
inline constexpr u8 kDefaultScratchGPR = 3;
inline constexpr u8 kDefaultScratchFPR = 3;

[[nodiscard]] u32 DefaultGPRValueReserve(const GPRSMask& pool, const FeatureSet& features);

// Every *use* of a spilled value reloads it into a scratch register
// (JitContext::SpillGPR/SpillFPR) on top of the emitting opcode's own budget,
// and a spilled destination needs one too. Reloads are memoized per
// (instruction, value), so an instruction's extra demand is exactly the number
// of distinct spilled values it names -- which the allocator counts directly
// rather than guessing at.

// Conservative budget by opcode, plus the precise instruction-level contract
// used whenever the caller has the full IR instruction. Total functions;
// never fail.
[[nodiscard]] compile::ScratchNeed ScratchBudget(ir::OpCode op, const FeatureSet& features);
[[nodiscard]] compile::ScratchNeed ScratchBudget(const ir::Inst& inst,
                                                 const FeatureSet& features,
                                                 FlagsDemand demand);
[[nodiscard]] u32 ScratchOnlyGPRCount(const GPRSMask& pool,
                                      ir::OpCode op,
                                      const FeatureSet& features);
[[nodiscard]] compile::ScratchNeed PreciseAddSubScratchBudget(const ir::Inst& inst,
                                                              FlagsDemand demand);
[[nodiscard]] bool ScratchPreciseRequested(const FeatureSet& features);

}  // namespace swift::runtime::backend

namespace swift::runtime::backend::arm64 {
[[nodiscard]] compile::ScratchRequirements DescribeInstructionScratch(const GPRSMask& pool,
                                                                      const FeatureSet& features,
                                                                      const ir::Inst& inst,
                                                                      FlagsDemand flags);
}  // namespace swift::runtime::backend::arm64
