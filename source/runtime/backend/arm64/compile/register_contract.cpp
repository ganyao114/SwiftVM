#include "register_contract.h"

#include <bit>

#include "aarch64/assembler-aarch64.h"
#include "base/logging.h"
#include "runtime/backend/context.h"
#include "runtime/frontend/x86/sse42str_helper.h"
#include "runtime/frontend/x86/x87.h"
#include "runtime/ir/block.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend {

static int X86PinExtLevel() {
    // Default level 2 after the flip A/B (bundle with XPOOL, coremark
    // 5/5 pairs positive, median 1.22). Level 3 stays opt-in only: its
    // measured coremark delta vs level 2 is -10.18%. =0 restores the
    // pre-W55 dynamic-only allocation as the rollback.
    return static_cast<int>(GetSvmConfig().x86_pin_ext);
}

bool ScratchXPoolRequested(const FeatureSet& features) {
    // Default ON after the flip A/B (bundled with PIN_EXT=2); =0
    // restores the old dynamic scratch pool as the rollback.
    return features.jit_scratch_xpool;
}

bool ScratchXPoolAutoEnabled(const FeatureSet& features) {
    return X86PinExtLevel() >= 3 && !ScratchXPoolRequested(features);
}

bool X86PinExtLevel3Requested() { return X86PinExtLevel() >= 3; }

bool ScratchXPoolEnabled(const FeatureSet& features) {
    return ScratchXPoolRequested(features) || X86PinExtLevel() >= 3;
}

bool X86PinExtLevel2Enabled(const GPRSMask& pool) {
    if (X86PinExtLevel() < 2) {
        return false;
    }
    for (u32 code = 0; code <= 5; ++code) {
        if (!pool.Get(code)) {
            return false;
        }
    }
    return true;
}

bool X86PinExtLevel3Enabled(const GPRSMask& pool) {
    if (X86PinExtLevel() < 3) {
        return false;
    }
    for (u32 code = 0; code <= 9; ++code) {
        if (!pool.Get(code)) {
            return false;
        }
    }
    return true;
}

bool FixedGPRClassEnabled(const GPRSMask& pool, const FeatureSet& features) {
    if (!features.ra_fixed_class || !X86PinExtLevel3Enabled(pool)) {
        return false;
    }
    for (u32 code = 0; code < 32; ++code) {
        if ((kX86FixedGPRHomes & (1u << code)) && !pool.Get(code)) {
            return false;
        }
    }
    return true;
}

bool IsFixedGPRHome(u32 code) { return code < 32 && (kX86FixedGPRHomes & (1u << code)); }

bool X86PinExtScratchOnlyEnabled(const GPRSMask& pool, const FeatureSet& features) {
    return X86PinExtLevel2Enabled(pool) && !ScratchXPoolEnabled(features) && pool.Get(12) &&
           pool.Get(13);
}

bool X86PinExtLevel3AluScratchEnabled(const GPRSMask& pool, ir::OpCode op) {
    return X86PinExtLevel3AluScratchEnabled(X86PinExtLevel3Enabled(pool) && pool.Get(10), op);
}

bool X86PinExtLevel3AluScratchEnabled(bool available, ir::OpCode op) {
    return available && (op == ir::OpCode::Add || op == ir::OpCode::Sub || op == ir::OpCode::Or ||
                         op == ir::OpCode::Select || op == ir::OpCode::SelectZero ||
                         op == ir::OpCode::VecFCvtFloatToInt || op == ir::OpCode::CallLambda ||
                         op == ir::OpCode::CallDynamic || op == ir::OpCode::CallLocation);
}

static bool X86PinExtEnabled() { return X86PinExtLevel() >= 1; }

bool ScratchPreciseRequested(const FeatureSet& features) {
    // Default ON after the merge A/B (L2 coremark spill 3.04B -> 20,
    // host_dynamic -1.44%; L3 modestly positive, Linux neutral). =0
    // restores the opcode-wide Add/Sub budget as the rollback.
    return features.scratch_precise;
}

static bool IsEncodedAddSubImmediate(s64 value) {
    return value >= 0 && (value <= 0xfff || ((static_cast<u64>(value) & 0xfff) == 0 &&
                                             (static_cast<u64>(value) >> 12) <= 0xfff));
}

static u8 NarrowHostReadScratch(const ir::DataClass& data) {
    if (!data.IsValue()) return 0;
    const auto value = data.value;
    return ir::GetValueSizeByte(value.Type()) <= 2 && value.Def() &&
                           value.Def()->IsGetHostRegOperation()
                   ? 1
                   : 0;
}

static u8 AddSubOperandScratch(const ir::Operand& right) {
    if (right.GetRight().Null()) {
        if (right.GetLeft().IsImm()) {
            return !IsEncodedAddSubImmediate(right.GetLeft().imm.GetSigned()) ? 1 : 0;
        }
        return NarrowHostReadScratch(right.GetLeft());
    }
    u8 need = NarrowHostReadScratch(right.GetLeft()) + NarrowHostReadScratch(right.GetRight());
    if (right.GetRight().IsImm() &&
        (right.GetOp() == ir::OperandOp::LSL || right.GetOp() == ir::OperandOp::LSR)) {
        return need;
    }
    return need + 1;
}

FlagsDemand ScanFlagsDemand(const ir::Block& block, const ir::Inst& owner) {
    FlagsDemand demand{};
    for (const auto& inst : block.GetInstList()) {
        const auto op = inst.GetOp();
        if (op != ir::OpCode::SaveFlags && op != ir::OpCode::BranchOnlyFlags) continue;
        if (!inst.ArgAt(0).IsValue() || inst.GetArg<ir::Value>(0).Def() != &owner) continue;
        demand.requested |= inst.GetArg<ir::Flags>(1);
        demand.pseudo_count++;
        demand.branch_only |= op == ir::OpCode::BranchOnlyFlags;
        demand.has_save |= op == ir::OpCode::SaveFlags;
    }
    return demand;
}

u32 ScratchOnlyGPRCount(const GPRSMask& pool, ir::OpCode op, const FeatureSet& features) {
    u32 count = X86PinExtLevel3AluScratchEnabled(pool, op) ? 1u : 0u;
    if (X86PinExtScratchOnlyEnabled(pool, features)) {
        const u32 fixed = FixedGPRClobbers(op, features, true);
        count += ((fixed & (1u << 12)) || GetSvmConfig().flags_regs ? 0u : 1u) +
                 ((fixed & (1u << 13)) ? 0u : 1u);
    }
    return count;
}

u32 DefaultGPRValueReserve(const GPRSMask& pool, const FeatureSet& features) {
    const u32 scratch_only = X86PinExtScratchOnlyEnabled(pool, features) ? 2u : 0u;
    return kDefaultScratchGPR > scratch_only ? kDefaultScratchGPR - scratch_only : 0u;
}

compile::ScratchNeed PreciseAddSubScratchBudget(const ir::Inst& inst, FlagsDemand demand) {
    ASSERT(inst.GetOp() == ir::OpCode::Add || inst.GetOp() == ir::OpCode::Sub);
    const auto requested = demand.requested;
    const bool branch_only = demand.branch_only;

    const auto right = inst.GetArg<ir::Operand>(1);
    const u8 operand_scratch = NarrowHostReadScratch(ir::DataClass{inst.GetArg<ir::Value>(0)}) +
                               AddSubOperandScratch(right);
    const u32 uses = inst.GetUses();
    const bool narrow_nzcv =
            ir::GetValueSizeByte(inst.ReturnType()) <= 2 && True(requested & ir::Flags::NZCV);
    if (narrow_nzcv) {
        // Sign alignment discards the upper input bits, so the emitter reads
        // the left and a plain register right without narrow extraction.
        const u8 right_scratch = right.GetRight().Null() && right.GetLeft().IsValue()
                                         ? 0
                                         : AddSubOperandScratch(right);
        // One lease covers materializing an aligned right or preserving it
        // when fixed-home publication aliases the result to that operand.
        u8 need = right_scratch + 1;
        if (!branch_only) {
            // MergeNZCV uses one shared lease. AF folds the operands in a
            // separate temporary; a used result can also publish directly to
            // the left home and must keep the original bit 4 until that fold.
            need += 1;
            if (True(requested & ir::Flags::AuxiliaryCarry)) {
                need += 1 + (uses != 0 ? 1 : 0);
            }
        }
        return {need, kDefaultScratchFPR};
    }

    u8 need = operand_scratch;
    if (!branch_only) {
        if (True(requested & ir::Flags::NZCV)) {
            ++need;
        }
        if (True(requested & ir::Flags::AuxiliaryCarry)) {
            // SaveAuxiliaryCarry itself leases one temporary after the
            // arithmetic. The narrow result path above separately prices the
            // operand-preservation lease required before that operation.
            need += 1;
        }
    }
    return {need, kDefaultScratchFPR};
}

static u8 LogicalOperandScratch(const ir::Inst& inst) {
    const auto right = inst.GetArg<ir::Operand>(1);
    if (right.GetRight().Null()) {
        if (!right.GetLeft().IsImm()) {
            return NarrowHostReadScratch(right.GetLeft());
        }
        return 1;
    }

    u8 need = right.GetLeft().IsImm() ? 1 : NarrowHostReadScratch(right.GetLeft());
    if (right.GetRight().IsImm() &&
        (right.GetOp() == ir::OperandOp::LSL || right.GetOp() == ir::OperandOp::LSR)) {
        return need;
    }
    need += NarrowHostReadScratch(right.GetRight());
    return need + 1;
}

static compile::ScratchNeed PreciseLogicalScratchBudget(const ir::Inst& inst, FlagsDemand demand) {
    u8 need = std::max<u8>(NarrowHostReadScratch(ir::DataClass{inst.GetArg<ir::Value>(0)}) +
                                   LogicalOperandScratch(inst),
                           1);
    if (demand.pseudo_count) ++need;
    return {need, kDefaultScratchFPR};
}

static bool ExecProfileEnabled() { return GetSvmConfig().exec_prof; }

u32 FixedGPRClobbers(ir::OpCode op, const FeatureSet& features, bool scratch_only) {
    constexpr u32 x11 = 1u << 11;
    constexpr u32 x12 = 1u << 12;
    constexpr u32 x13 = 1u << 13;
    constexpr u32 ip0 = 1u << 16;
    constexpr u32 ip1 = 1u << 17;
    // Execution counters are emitted at block entry and in terminals, outside
    // any one opcode's ordinary scratch lifetime. Under XPOOL, reserve VIXL's
    // ip0/ip1 across every live interval so the counters can use a stable
    // fixed pair instead of accumulating two dynamically-leased GPRs inside a
    // terminal. Profiling OFF keeps the allocator contract byte-for-byte
    // unchanged.
    const u32 exec_profile_clobbers =
            ScratchXPoolEnabled(features) && ExecProfileEnabled() ? ip0 | ip1 : 0;
    // Level 2 without XPOOL leases the otherwise globally-reserved x12/x13 as
    // scratch-only registers. It therefore needs the same fixed-clobber
    // exclusions even though those registers remain unavailable to linear
    // scan as value locations.
    if (!ScratchXPoolEnabled(features) && !scratch_only) {
        return exec_profile_clobbers;
    }
    switch (op) {
        // x11 is the exclusive-store status register; x12/x13 hold the
        // atomic value and unaligned fallback lock address.
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
            return exec_profile_clobbers | x11 | x12 | x13;
        // Pair CAS additionally holds the second observed half in x13.
        case ir::OpCode::CompareAndSwap128:
            return exec_profile_clobbers | x11 | x12 | x13;
        // X87 uses an instruction-specific scratch/fixed-clobber contract.
        case ir::OpCode::X87Op:
            // Level 2 without XPOOL takes the exact helper fallback instead
            // of the high-pressure inline lowering. Under XPOOL reserve ip0
            // as X87's dedicated VIXL immediate-synthesis register; the Inst
            // overload below adds x12/x13 only for arms that name that pair.
            return exec_profile_clobbers | (ScratchXPoolEnabled(features) ? ip0 : 0);
        // Host-call target and fixed-location materialization.
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::CallLocation:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::SetLocation:
        case ir::OpCode::CheckMemoryAlignment:
            return exec_profile_clobbers | x11;
        default:
            return exec_profile_clobbers;
    }
}

u32 FixedGPRClobbers(const ir::Inst& inst, const FeatureSet& features, bool scratch_only) {
    u32 fixed = FixedGPRClobbers(inst.GetOp(), features, scratch_only);
    if (inst.GetOp() == ir::OpCode::XchgBarrier && GetSvmConfig().static_regs) {
        // XCHG publishes the two guest values in program order. At the marker
        // only the first target has been overwritten; the second target still
        // contains its old value until the following SetHostGPR. Clobbering
        // only the first home preserves resident state for the second read.
        // The decoder stores the two parent guest-GPR indices in nibbles.
        const auto guest_home_mask = [](u32 guest) {
            const auto level = X86PinExtLevel();
            u32 home = UINT32_MAX;
            if (guest == 0 && level >= 1)
                home = 22;  // RAX
            else if (guest == 1 && level >= 1)
                home = 23;  // RCX
            else if (guest == 2 && level >= 1)
                home = 29;  // RDX
            else if (guest == 3)
                home = 20;  // RBX
            else if (guest == 4)
                home = 19;  // RSP
            else if (guest == 5)
                home = 21;  // RBP
            else if (level >= 2 && guest >= 6 && guest <= 12) {
                home = guest - 6;  // RSI..R12
            } else if (guest == 13) {
                home = level >= 3 ? 7 : (level >= 2 ? 8 : UINT32_MAX);
            } else if (guest == 14) {
                home = level >= 3 ? 8 : (level >= 2 ? 7 : UINT32_MAX);
            } else if (guest == 15 && level >= 3) {
                home = 9;  // R15
            }
            return home < 32 ? (1u << home) : 0u;
        };
        const u32 packed = inst.GetArg<ir::Imm>(0).Get();
        fixed |= guest_home_mask(packed & 0xFu);
    }
    if (inst.GetOp() != ir::OpCode::X87Op || !ScratchXPoolEnabled(features)) {
        return fixed;
    }
    const u64 command = inst.GetArg<ir::Imm>(1).Get();
    const auto action = static_cast<swift::x86::X87Action>(command & 0xFF);
    if (action == swift::x86::X87Action::StoreInt || action == swift::x86::X87Action::Compare) {
        fixed |= (1u << 12) | (1u << 13);
    }
    return fixed;
}

GPRClassContract ClassifyGPRContract(const GPRSMask& pool,
                                     const ir::Inst& inst,
                                     const FeatureSet& features,
                                     FlagsDemand demand) {
    const u32 fixed = FixedGPRClobbers(inst, features);
    // x0-x17 are the ordinary AAPCS caller-clobbered GPRs used by generated
    // helper calls. x18 stays out: Apple reserves it and the Linux conditional
    // spill path treats it as reload-only capacity, never a helper value home.
    constexpr u32 kAAPCSCallerClobber = (1u << 18) - 1;
    const bool cold_continuation = (fixed & (1u << 13)) != 0;
    const auto need = ScratchBudget(inst, features, demand);
    auto value_pool = pool;
    GPRClassContract result{
            .value_pool = static_cast<u8>(value_pool.GetClearCount()),
            .hot_instruction_scratch = need.gpr,
            .cold_edge_scratch = static_cast<u8>(cold_continuation ? 1 : 0),
            .call_clobber_mask = kAAPCSCallerClobber,
            .fixed_clobber_mask = fixed,
    };
    if (FixedGPRClassEnabled(pool, features) && inst.GetOp() != ir::OpCode::XchgBarrier) {
        ASSERT_MSG((fixed & kX86FixedGPRHomes) == 0,
                   "fixed clobber mask {:#x} overlaps guest GPR class {:#x}",
                   fixed,
                   kX86FixedGPRHomes);
    }
    return result;
}

// The common scratch contract and JitContext enforce these budgets.
//
// Only opcodes that exceed the default appear here. Every entry is a measured
// peak over the full corpus, cross-checked against the emitter source:
//
//  * X87Op holds eight dynamic emitter temporaries in its widest inline arms.
//    StoreInt/Compare additionally name fixed x12/x13, while every XPOOL arm
//    materializes VIXL immediates through fixed ip0. W52 incorrectly priced the
//    XPOOL shape as six and converted the two fixed users to extra dynamic
//    leases. The Inst overload below carries the narrower per-command peaks;
//    fixed registers are accounted separately and cannot hold a live value.
//  * VecFCvtFloatToInt open-codes the x86 "invalid conversion -> INT_MIN"
//    rule per lane and holds five GPRs across it.
//  * VecFCvtPacked holds five vector temporaries in its widest conversion.
//  * Native FP arithmetic has no payload-repair temporaries. Scalar merges
//    can need one vector plus one materialised non-vector right operand;
//    FMA can need one accumulator when result aliases a multiplicand.
compile::ScratchNeed ScratchBudget(ir::OpCode op, const FeatureSet& features) {
    switch (op) {
        case ir::OpCode::PopCount:
            return {0, 1};
        case ir::OpCode::Crc32c:
            // Three temporaries for the software fallback; the hardware
            // instruction needs none. Operand reloads are priced separately.
            return {3, 0};
        case ir::OpCode::CallLambda:
            // EmitHostCall leases only the shared NZCV merge temporary.
            // Argument, target and result reloads are charged separately by
            // the verifier; x11 is the fixed call target. Charging three
            // ordinary temporaries can reject a fully spilled call even
            // though its actual emitter fits the remaining dynamic pool.
            return {1, kDefaultScratchFPR};
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
            // Level 3's spill-aware accounting must use the measured emitter
            // peak, not the generic three-register default: operand/result
            // reloads replace ordinary value locations, while these emitters
            // themselves only hold MergeNZCV's shared temporary. The exclusive
            // status/value registers are fixed clobbers accounted separately
            // by the allocator.
            if (X86PinExtLevel() >= 3) {
                return {1, kDefaultScratchFPR};
            }
            return {kDefaultScratchGPR, kDefaultScratchFPR};
        // --- x87 GPRs ----------------------------------------------------
        case ir::OpCode::X87Op:
            if ((X86PinExtLevel() >= 2 && !ScratchXPoolEnabled(features)) ||
                X86PinExtLevel() >= 3) {
                // The reduced dynamic pool cannot cover the inline x87 peak.
                // The emitter deliberately selects its exact host-helper
                // fallback in this state; price that ordinary call shape.
                return {kDefaultScratchGPR, kDefaultScratchFPR};
            }
            // x12/x13 and XPOOL's dedicated VIXL ip0 are fixed clobbers, not
            // part of this dynamic count.
            return {8, kDefaultScratchFPR};
        // --- four GPRs ---------------------------------------------------
        // The 8/16-bit flag-setting path sign-aligns both operands to W[31]:
        // one save per RA-tied operand (left/right), one to materialize an
        // immediate or shifted-register right for the aligned form, and one
        // more inside SaveAuxiliaryCarry while those three are still leased.
        case ir::OpCode::Add:
        case ir::OpCode::Sub:
            // A narrow Add/Sub whose tied input aliases one of the extended
            // pinned GPRs needs one extra preservation temporary. OFF keeps
            // the historical reserve and byte-for-byte allocation; ON makes
            // the fifth register explicit in the scratch contract.
            return {static_cast<u8>(X86PinExtEnabled() ? 5 : 4), kDefaultScratchFPR};
        // --- five GPRs ---------------------------------------------------
        case ir::OpCode::VecFCvtFloatToInt:
            return {5, kDefaultScratchFPR};
        case ir::OpCode::VecFAdd:
        case ir::OpCode::VecFSub:
        case ir::OpCode::VecFMul:
        case ir::OpCode::VecFDiv:
            return {0, 0};
        case ir::OpCode::VecFMulAdd:
            return {0, 1};
        case ir::OpCode::VecFUnary:
            return {0, 3};
        case ir::OpCode::VecFAddScalar32:
        case ir::OpCode::VecFSubScalar32:
        case ir::OpCode::VecFMulScalar32:
        case ir::OpCode::VecFDivScalar32:
        case ir::OpCode::VecFAddScalar64:
        case ir::OpCode::VecFSubScalar64:
        case ir::OpCode::VecFMulScalar64:
        case ir::OpCode::VecFDivScalar64:
            return {0, 2};
        case ir::OpCode::VecFCvtPacked:
            return {kDefaultScratchGPR, 5};
        // AESKEYGENASSIST keeps the S-box output, zero key, TBL control and
        // RCON vector live together.  The other crypto ops stay at or below
        // the default three-vector scratch budget.
        case ir::OpCode::VecAesKeygenAssist:
            return {kDefaultScratchGPR, 4};
        case ir::OpCode::VecSha256Rnds2:
            // SHA256RNDS2 must retain both pre-round state vectors while
            // producing SHA256H and SHA256H2, plus the duplicated XMM0 key.
            return {kDefaultScratchGPR, 4};
        case ir::OpCode::Sse42Str:
            // Two implicit lengths plus scalar mask/index work, and four NEON
            // temporaries for equal-ordered's accumulator/row/validity/ones.
            return {4, 4};
        default:
            return {kDefaultScratchGPR, kDefaultScratchFPR};
    }
}

compile::ScratchNeed ScratchBudget(const ir::Inst& inst,
                                   const FeatureSet& features,
                                   FlagsDemand demand) {
    auto need = ScratchBudget(inst.GetOp(), features);
    if (ScratchPreciseRequested(features)) {
        switch (inst.GetOp()) {
            case ir::OpCode::Add:
            case ir::OpCode::Sub:
                return PreciseAddSubScratchBudget(inst, demand);
            case ir::OpCode::And:
            case ir::OpCode::Or:
            case ir::OpCode::Xor:
                return PreciseLogicalScratchBudget(inst, demand);
            case ir::OpCode::AdvancePC:
                // The FLAGS_REGS=0 path merges host NZCV with one explicit
                // shared temporary, while the scratch contract may also need
                // a VIXL temporary for an immediate materialisation. Count
                // both in the combined audit budget.
                need.gpr = 2;
                break;
            case ir::OpCode::PublishSse42StrFlags:
                need.gpr = 1;
                break;
            case ir::OpCode::Sse42Str:
#if defined(__aarch64__)
                if (swift::x86::Sse42StrVectorHelperAddress(u8(inst.GetArg<ir::Imm>(2).Get()))) {
                    return {0, 0};
                }
#endif
                break;
            case ir::OpCode::SetCarry:
            case ir::OpCode::SetOverflow:
                need.gpr = 2;
                break;
            case ir::OpCode::LoadUniform:
            case ir::OpCode::StoreUniform: {
                const bool store = inst.GetOp() == ir::OpCode::StoreUniform;
                const auto uniform = inst.GetArg<ir::Uniform>(0);
                const auto type = store ? inst.GetArg<ir::Value>(1).Type() : inst.ReturnType();
                const auto width = ir::GetValueSizeByte(type);
                const s64 offset = offsetof(State, uniform_buffer_begin) + uniform.GetOffset();
                // The same scaled/unscaled forms used by LoadStoreMacro.
                // Otherwise VIXL materializes the address in one temporary.
                using vixl::aarch64::Assembler;
                need.gpr = !width || width > 16 ||
                        (!Assembler::IsImmLSUnscaled(offset) &&
                         !Assembler::IsImmLSScaled(offset, std::countr_zero(u32{width})));
                // An MXCSR write can refresh the host floating-point control
                // register and leases three temporaries in EmitStoreUniform.
                if (store && uniform.GetOffset() == offsetof(swift::x86::ThreadContext64, mxcsr))
                    need.gpr += 3;
                break;
            }
            case ir::OpCode::LoadImm:
            case ir::OpCode::GetHostGPR:
            case ir::OpCode::SetHostGPR:
            case ir::OpCode::ClearFlags:
            case ir::OpCode::AsrImm:
            case ir::OpCode::LslImm:
            case ir::OpCode::LsrImm:
            case ir::OpCode::RorImm:
            case ir::OpCode::ZeroExtend32:
            case ir::OpCode::ZeroExtend32To64:
            case ir::OpCode::ZeroExtend64:
                need.gpr = 0;
                break;
            case ir::OpCode::RorValue: {
                const u32 bits = ir::GetScalarIntegerBits(inst.ReturnType());
                const u32 input = ir::GetScalarIntegerBits(inst.GetArg<ir::Value>(0).Type());
                // Staging a narrow/mixed-width input may overwrite a dying
                // count tied to the result; keep one explicit count copy.
                need.gpr = bits < 32 || input < bits;
                break;
            }
            default:
                break;
        }
    }
    if (features.fpr_scratch_precise) {
        switch (inst.GetOp()) {
            case ir::OpCode::VecFAddScalar32:
            case ir::OpCode::VecFSubScalar32:
            case ir::OpCode::VecFMulScalar32:
            case ir::OpCode::VecFDivScalar32:
            case ir::OpCode::VecFAddScalar64:
            case ir::OpCode::VecFSubScalar64:
            case ir::OpCode::VecFMulScalar64:
            case ir::OpCode::VecFDivScalar64:
                // Both widths can need a temporary to preserve scalar upper
                // lanes when result ties left; a GPR RHS needs another FPR.
                need.fpr = static_cast<u8>(
                        1 + !ir::IsFloatValueType(inst.GetArg<ir::Value>(1).Type()));
                break;
            case ir::OpCode::VecFUnary:
                if (inst.GetArg<ir::Imm>(3).Get() == 0)
                    need.fpr = static_cast<u8>(inst.GetArg<ir::Imm>(4).Get() != 0);
                break;
            case ir::OpCode::VecFCvtPacked: {
                const u32 kind = inst.GetArg<ir::Imm>(1).Get();
                // Kinds 2..5 hold invalid/compare/bound/indefinite plus wide.
                // Every other arm only leases the eagerly-created `wide`.
                need.fpr = static_cast<u8>(kind >= 2 && kind <= 5 ? 5 : 1);
                break;
            }
            default:
                break;
        }
    }
    if (inst.GetOp() != ir::OpCode::X87Op || !ScratchXPoolEnabled(features)) {
        return need;
    }
    const u64 command = inst.GetArg<ir::Imm>(1).Get();
    const auto action = static_cast<swift::x86::X87Action>(command & 0xFF);
    const auto format = static_cast<swift::x86::X87Format>((command >> 8) & 0xFF);
    if (action == swift::x86::X87Action::LoadFloat && format == swift::x86::X87Format::Float80) {
        need.gpr = 6;
    } else if (action == swift::x86::X87Action::Compare) {
        need.gpr = 6;
    } else if (action == swift::x86::X87Action::StoreInt) {
        need.gpr = 7;
    }
    return need;
}

}  // namespace swift::runtime::backend
