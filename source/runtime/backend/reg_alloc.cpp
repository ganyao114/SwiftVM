#include "base/logging.h"
//
// Created by 甘尧 on 2023/10/13.
//

#include <algorithm>
#include <cstdio>
#if defined(__linux__)
#include <execinfo.h>
#include <unistd.h>
#endif
#include "reg_alloc.h"
#include "runtime/common/perf_stats.h"
#include "runtime/frontend/x86/sse42str_helper.h"
#include "runtime/frontend/x86/x87.h"
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

bool X86PinExtLevel3Requested() {
    return X86PinExtLevel() >= 3;
}

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

bool FixedGPRClassEnabled(const GPRSMask& pool,
                          const FeatureSet& features) {
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

bool IsFixedGPRHome(u32 code) {
    return code < 32 && (kX86FixedGPRHomes & (1u << code));
}

bool X86PinExtScratchOnlyEnabled(const GPRSMask& pool,
                                 const FeatureSet& features) {
    return X86PinExtLevel2Enabled(pool) && !ScratchXPoolEnabled(features) &&
           pool.Get(12) && pool.Get(13);
}

bool X86PinExtLevel3AluScratchEnabled(const GPRSMask& pool, ir::OpCode op) {
    return X86PinExtLevel3Enabled(pool) && pool.Get(10) &&
           (op == ir::OpCode::Add || op == ir::OpCode::Sub ||
            op == ir::OpCode::Or || op == ir::OpCode::Select ||
            op == ir::OpCode::SelectZero ||
            op == ir::OpCode::VecFCvtFloatToInt ||
            op == ir::OpCode::CallLambda || op == ir::OpCode::CallDynamic ||
            op == ir::OpCode::CallLocation);
}

static bool X86PinExtEnabled() {
    return X86PinExtLevel() >= 1;
}

bool ScratchPreciseRequested(const FeatureSet& features) {
    // Default ON after the merge A/B (L2 coremark spill 3.04B -> 20,
    // host_dynamic -1.44%; L3 modestly positive, Linux neutral). =0
    // restores the opcode-wide Add/Sub budget as the rollback.
    return features.scratch_precise;
}

static bool IsEncodedAddSubImmediate(s64 value) {
    return value >= 0 &&
           (value <= 0xfff ||
            ((static_cast<u64>(value) & 0xfff) == 0 &&
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
            return !IsEncodedAddSubImmediate(
                           right.GetLeft().imm.GetSigned())
                    ? 1
                    : 0;
        }
        return NarrowHostReadScratch(right.GetLeft());
    }
    u8 need = NarrowHostReadScratch(right.GetLeft()) +
              NarrowHostReadScratch(right.GetRight());
    if (right.GetRight().IsImm() &&
        (right.GetOp() == ir::OperandOp::LSL ||
         right.GetOp() == ir::OperandOp::LSR)) {
        return need;
    }
    return need + 1;
}

ScratchNeed PreciseAddSubScratchBudget(const ir::Inst& inst) {
    ASSERT(inst.GetOp() == ir::OpCode::Add || inst.GetOp() == ir::OpCode::Sub);
    ir::Flags requested{};
    bool branch_only = false;
    for (auto* pseudo : const_cast<ir::Inst&>(inst).GetPseudoOperations()) {
        if (pseudo->GetOp() == ir::OpCode::SaveFlags) {
            requested |= pseudo->GetArg<ir::Flags>(1);
        } else if (pseudo->GetOp() == ir::OpCode::BranchOnlyFlags) {
            requested |= pseudo->GetArg<ir::Flags>(1);
            branch_only = true;
        }
    }

    const auto right = inst.GetArg<ir::Operand>(1);
    const u8 operand_scratch =
            NarrowHostReadScratch(ir::DataClass{inst.GetArg<ir::Value>(0)}) +
            AddSubOperandScratch(right);
    const bool narrow_nzcv = ir::GetValueSizeByte(inst.ReturnType()) <= 2 &&
                             True(requested & ir::Flags::NZCV);
    if (narrow_nzcv) {
        // Current RA has no narrow Add/Sub destination tie: its result cannot
        // share either input, so the two emitter preservation arms are dead for
        // allocated code. The aligned right needs one register in the worst
        // operand form; EmitOperand's own materialization was counted above.
        u8 need = operand_scratch + 1;
        if (!branch_only) {
            // A preceding lazy producer can require MergeNZCV. AF is a
            // separate GetTmpX after the aligned operands are still leased.
            need += 1;
            if (True(requested & ir::Flags::AuxiliaryCarry)) {
                need += 1;
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
            // Preserve operand bits when the result reuses an input, then
            // compute AF after the arithmetic has produced its result.
            need += 2;
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

    u8 need = right.GetLeft().IsImm()
            ? 1
            : NarrowHostReadScratch(right.GetLeft());
    if (right.GetRight().IsImm() &&
        (right.GetOp() == ir::OperandOp::LSL ||
         right.GetOp() == ir::OperandOp::LSR)) {
        return need;
    }
    need += NarrowHostReadScratch(right.GetRight());
    return need + 1;
}

static ScratchNeed PreciseLogicalScratchBudget(const ir::Inst& inst) {
    u8 need = std::max<u8>(
            NarrowHostReadScratch(ir::DataClass{inst.GetArg<ir::Value>(0)}) +
                    LogicalOperandScratch(inst),
            1);
    for (auto* pseudo : const_cast<ir::Inst&>(inst).GetPseudoOperations()) {
        if (pseudo->GetOp() == ir::OpCode::SaveFlags ||
            pseudo->GetOp() == ir::OpCode::BranchOnlyFlags) {
            ++need;
            break;
        }
    }
    return {need, kDefaultScratchFPR};
}

static bool ExecProfileEnabled() {
    return GetSvmConfig().exec_prof;
}

u32 FixedGPRClobbers(ir::OpCode op, const FeatureSet& features,
                     bool scratch_only) {
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
            return exec_profile_clobbers |
                   (ScratchXPoolEnabled(features) ? ip0 : 0);
        // Host-call target and fixed-location materialization.
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::CallLocation:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::SetLocation:
        case ir::OpCode::CheckMemoryAlignment:
            return exec_profile_clobbers | x11;
        // Exact NaN cold veneers use x13 as their link register. A value live
        // across the originating FP opcode must therefore never occupy it.
        case ir::OpCode::VecFAddScalar32:
        case ir::OpCode::VecFSubScalar32:
        case ir::OpCode::VecFMulScalar32:
        case ir::OpCode::VecFDivScalar32:
        case ir::OpCode::VecFAddScalar64:
        case ir::OpCode::VecFSubScalar64:
        case ir::OpCode::VecFMulScalar64:
        case ir::OpCode::VecFDivScalar64:
        case ir::OpCode::VecFAdd:
        case ir::OpCode::VecFSub:
        case ir::OpCode::VecFMul:
        case ir::OpCode::VecFDiv:
        case ir::OpCode::VecFUnary:
            return exec_profile_clobbers | x13;
        default:
            return exec_profile_clobbers;
    }
}

u32 FixedGPRClobbers(const ir::Inst& inst, const FeatureSet& features,
                     bool scratch_only) {
    u32 fixed = FixedGPRClobbers(inst.GetOp(), features, scratch_only);
    if (inst.GetOp() != ir::OpCode::X87Op || !ScratchXPoolEnabled(features)) {
        return fixed;
    }
    const u64 command = inst.GetArg<ir::Imm>(1).Get();
    const auto action = static_cast<swift::x86::X87Action>(command & 0xFF);
    if (action == swift::x86::X87Action::StoreInt ||
        action == swift::x86::X87Action::Compare) {
        fixed |= (1u << 12) | (1u << 13);
    }
    return fixed;
}

GPRClassContract ClassifyGPRContract(const GPRSMask& pool,
                                     const ir::Inst& inst,
                                     const FeatureSet& features) {
    const u32 fixed = FixedGPRClobbers(inst, features);
    // x0-x17 are the ordinary AAPCS caller-clobbered GPRs used by generated
    // helper calls. x18 stays out: Apple reserves it and the Linux conditional
    // spill path treats it as reload-only capacity, never a helper value home.
    constexpr u32 kAAPCSCallerClobber = (1u << 18) - 1;
    const bool cold_continuation = (fixed & (1u << 13)) != 0;
    const auto need = ScratchBudget(inst, features);
    auto value_pool = pool;
    GPRClassContract result{
            .value_pool = static_cast<u8>(value_pool.GetClearCount()),
            .hot_instruction_scratch = need.gpr,
            .cold_edge_scratch = static_cast<u8>(cold_continuation ? 1 : 0),
            .call_clobber_mask = kAAPCSCallerClobber,
            .fixed_clobber_mask = fixed,
    };
    if (FixedGPRClassEnabled(pool, features)) {
        ASSERT_MSG((fixed & kX86FixedGPRHomes) == 0,
                   "fixed clobber mask {:#x} overlaps guest GPR class {:#x}",
                   fixed, kX86FixedGPRHomes);
    }
    return result;
}

// See reg_alloc.h for what this table is and who enforces it.
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
//  * VecFCvtPacked and VecFMulAdd hold five vector temporaries.
//  * The float arithmetic family needs no GPR at all: since the NaN fixup
//    became NEON (three vector temporaries, all lanes at once) their demand is
//    that three plus whatever the calling emitter is already holding -- two
//    for the 32-bit scalar shapes (merge target + the FPR-materialised right
//    operand), one for the 64-bit ones, none for the packed ones.
ScratchNeed ScratchBudget(ir::OpCode op, const FeatureSet& features) {
    switch (op) {
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
            return {static_cast<u8>(X86PinExtEnabled() ? 5 : 4),
                    kDefaultScratchFPR};
        // --- five GPRs ---------------------------------------------------
        case ir::OpCode::VecFCvtFloatToInt:
            return {5, kDefaultScratchFPR};
        // --- five FPRs ---------------------------------------------------
        case ir::OpCode::VecFAddScalar32:
        case ir::OpCode::VecFSubScalar32:
        case ir::OpCode::VecFMulScalar32:
        case ir::OpCode::VecFDivScalar32:
        case ir::OpCode::VecFCvtPacked:
        case ir::OpCode::VecFMulAdd:
            return {kDefaultScratchGPR, 5};
        // --- four FPRs ---------------------------------------------------
        case ir::OpCode::VecFAddScalar64:
        case ir::OpCode::VecFSubScalar64:
        case ir::OpCode::VecFMulScalar64:
        case ir::OpCode::VecFDivScalar64:
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

ScratchNeed ScratchBudget(const ir::Inst& inst, const FeatureSet& features) {
    auto need = ScratchBudget(inst.GetOp(), features);
    if (ScratchPreciseRequested(features)) {
        switch (inst.GetOp()) {
            case ir::OpCode::Add:
            case ir::OpCode::Sub:
                return PreciseAddSubScratchBudget(inst);
            case ir::OpCode::And:
            case ir::OpCode::Or:
            case ir::OpCode::Xor:
                return PreciseLogicalScratchBudget(inst);
            case ir::OpCode::AdvancePC:
                need.gpr = 1;
                break;
            case ir::OpCode::PublishSse42StrFlags:
                need.gpr = 1;
                break;
            case ir::OpCode::Sse42Str:
#if defined(__aarch64__)
                if (swift::x86::Sse42StrVectorHelperAddress(
                            u8(inst.GetArg<ir::Imm>(2).Get()))) {
                    return {0, 0};
                }
#endif
                break;
            case ir::OpCode::SetCarry:
            case ir::OpCode::SetOverflow:
                need.gpr = 2;
                break;
            case ir::OpCode::StoreUniform:
                // An MXCSR write can refresh the host floating-point control
                // register and leases three temporaries in EmitStoreUniform.
                need.gpr = inst.GetArg<ir::Uniform>(0).GetOffset() ==
                                   offsetof(swift::x86::ThreadContext64, mxcsr)
                        ? 3
                        : 0;
                break;
            case ir::OpCode::LoadImm:
            case ir::OpCode::LoadUniform:
            case ir::OpCode::GetHostGPR:
            case ir::OpCode::SetHostGPR:
            case ir::OpCode::ClearFlags:
            case ir::OpCode::AsrImm:
            case ir::OpCode::LslImm:
            case ir::OpCode::LsrImm:
            case ir::OpCode::RorImm:
            case ir::OpCode::RorValue:
            case ir::OpCode::ZeroExtend32:
            case ir::OpCode::ZeroExtend32To64:
            case ir::OpCode::ZeroExtend64:
                need.gpr = 0;
                break;
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
                if (features.sse_nan_coldpath) {
                    // The non-AFP fallback still needs its scalar result temp;
                    // a non-vector RHS adds one materialisation temp. The
                    // NaN-repair vectors belong to the fixed cold ABI.
                    need.fpr = static_cast<u8>(
                            1 + !ir::IsFloatValueType(
                                        inst.GetArg<ir::Value>(1).Type()));
                }
                break;
            case ir::OpCode::VecFAddScalar64:
            case ir::OpCode::VecFSubScalar64:
            case ir::OpCode::VecFMulScalar64:
            case ir::OpCode::VecFDivScalar64:
                if (features.sse_nan_coldpath) {
                    // The 64-bit fallback computes in result; only a scalar
                    // RHS that is not already an FPR needs a temporary.
                    need.fpr = static_cast<u8>(
                            !ir::IsFloatValueType(
                                     inst.GetArg<ir::Value>(1).Type()));
                }
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
    if (action == swift::x86::X87Action::LoadFloat &&
        format == swift::x86::X87Format::Float80) {
        need.gpr = 6;
    } else if (action == swift::x86::X87Action::Compare) {
        need.gpr = 6;
    } else if (action == swift::x86::X87Action::StoreInt) {
        need.gpr = 7;
    }
    return need;
}

RegAlloc::RegAlloc(u32 instr_size, const GPRSMask& gprs, const FPRSMask& fprs,
                   const FeatureSet& features, bool afp_nan)
        : alloc_result(instr_size), live_ends(instr_size, UINT32_MAX),
          coalesced_host_writes(instr_size),
          coalesced_host_reads(instr_size),
          width_chain_anchors(instr_size, UINT32_MAX),
          width_component_owners(instr_size),
          low32_copy_sources(instr_size, UINT32_MAX),
          const_address_cache_anchors(instr_size, UINT32_MAX),
          aes_chain_targets(instr_size, UINT16_MAX),
          pshufd_direct(instr_size),
          gprs(gprs), fprs(fprs), features(features) {
    // Trampolines 暴露跨 module 的最大 GPR 并集。XPOOL 关闭时在 unit 私有
    // allocator 里恢复旧保留集，保持默认发码不变，同时允许 module 快照分叉。
    if (!ScratchXPoolEnabled(features)) {
        for (u32 code : {11u, 12u, 13u, 16u, 17u}) {
            this->gprs.Mark(code);
        }
    }
    // A-class last_result home. XPOOL ON otherwise puts x12 in the value
    // pool; pin it out so later token emission can keep it live across the
    // dispatcher. XPOOL OFF already reserved it above.
    if (FlagsRegsEnabled()) {
        this->gprs.Mark(12);
    }
    // v11-v14 are an emitter-local NaN cold ABI, not a cross-unit ABI. Keep
    // their availability a per-unit FeatureSet decision: the trampoline mask
    // exposes the AFP-capable union, while an OFF or non-AFP unit restores the
    // four historical reservations here.
    if (!features.fpr_ipv_reclaim || !afp_nan) {
        for (u32 code = 11; code <= 14; ++code) {
            this->fprs.Mark(code);
        }
    }
}

void RegAlloc::ResetAllocations() {
    std::fill(alloc_result.begin(), alloc_result.end(), Map{});
    std::fill(live_ends.begin(), live_ends.end(), UINT32_MAX);
    spill_reloads.clear();
    spill_reload_region_count = 0;
    std::fill(coalesced_host_writes.begin(), coalesced_host_writes.end(), false);
    std::fill(coalesced_host_reads.begin(), coalesced_host_reads.end(), false);
    std::fill(width_chain_anchors.begin(), width_chain_anchors.end(), UINT32_MAX);
    std::fill(width_component_owners.begin(), width_component_owners.end(),
              WidthComponentOwner{});
    std::fill(low32_copy_sources.begin(), low32_copy_sources.end(),
              UINT32_MAX);
    std::fill(const_address_cache_anchors.begin(), const_address_cache_anchors.end(),
              UINT32_MAX);
    std::fill(aes_chain_targets.begin(), aes_chain_targets.end(), UINT16_MAX);
    std::fill(pshufd_direct.begin(), pshufd_direct.end(), false);
    current_ir = nullptr;
}

void RegAlloc::MapRegister(u32 id, ir::HostFPR fpr) {
    auto& map = alloc_result[id];
    map.type = FPR;
    map.slot = fpr.id;
    map.fixed_gpr = false;
}

void RegAlloc::MapRegister(u32 id, ir::HostGPR gpr) {
    auto& map = alloc_result[id];
    map.type = GPR;
    map.slot = gpr.id;
    map.fixed_gpr = false;
}

void RegAlloc::MapFixedRegister(u32 id, ir::HostGPR gpr) {
    ASSERT(IsFixedGPRHome(gpr.id));
    auto& map = alloc_result[id];
    map.type = GPR;
    map.slot = gpr.id;
    map.fixed_gpr = true;
}

void RegAlloc::MapMemSpill(u32 id, ir::SpillSlot slot) {
    auto& map = alloc_result[id];
    map.type = MEM;
    map.slot = slot.offset;
    map.fixed_gpr = false;
}

void RegAlloc::MapSpillReload(u32 value_id, u32 first_use, u32 last_use,
                              ir::HostGPR reg, bool owns_all_uses,
                              bool fault_backed) {
    value_id = ResolveId(value_id);
    ASSERT(alloc_result[value_id].type == MEM);
    ASSERT(first_use <= last_use);
    if (spill_reloads.size() < alloc_result.size()) {
        spill_reloads.resize(alloc_result.size());
    }
    const u32 region = spill_reload_region_count++;
    for (u32 id = first_use; id <= last_use; ++id) {
        ASSERT(id < alloc_result.size());
        auto& reloads = spill_reloads[id];
        ASSERT(std::none_of(reloads.begin(), reloads.end(), [value_id](const auto& reload) {
            return reload.value == value_id;
        }));
        reloads.push_back(SpillReload{
                .region = region,
                .value = value_id,
                .reg = reg.id,
                .owns_all_uses = owns_all_uses,
                .fault_backed = fault_backed,
        });
        alloc_result[id].dirty_gprs.Mark(reg.id);
    }
}

void RegAlloc::MapReference(u32 from, u32 to) {
    auto& map = alloc_result[to];
    map.type = REF;
    map.slot = from;
    map.fixed_gpr = false;
}

void RegAlloc::MarkHostWriteCoalesced(u32 id) {
    ASSERT(id < coalesced_host_writes.size());
    coalesced_host_writes[id] = true;
}

void RegAlloc::MarkHostReadCoalesced(u32 id) {
    ASSERT(id < coalesced_host_reads.size());
    coalesced_host_reads[id] = true;
}

void RegAlloc::MarkWidthChainCoalesced(u32 id, u32 anchor_id) {
    ASSERT(id < width_chain_anchors.size());
    ASSERT(anchor_id < width_chain_anchors.size());
    width_chain_anchors[id] = anchor_id;
}

void RegAlloc::MarkLow32CopyCoalesced(u32 id, u32 source_id) {
    ASSERT(id < low32_copy_sources.size());
    ASSERT(source_id < low32_copy_sources.size());
    low32_copy_sources[id] = source_id;
}

bool RegAlloc::FreezeWidthComponentOwner(u32 anchor_id, u16 target,
                                         bool high_zero) {
    ASSERT(anchor_id < width_component_owners.size());
    auto& owner = width_component_owners[anchor_id];
    if (owner.Valid()) {
        return owner.target == target && owner.high_zero == high_zero;
    }
    owner.target = target;
    owner.high_zero = high_zero;
    return true;
}

bool RegAlloc::CommitWidthComponentOwner(u32 anchor_id, u16 target) {
    ASSERT(anchor_id < width_component_owners.size());
    auto& owner = width_component_owners[anchor_id];
    if (!owner.Valid() || owner.target != target) {
        return false;
    }
    owner.committed = true;
    return true;
}

void RegAlloc::MarkConstAddressCached(u32 id, u32 anchor_id) {
    ASSERT(id < const_address_cache_anchors.size());
    ASSERT(anchor_id < const_address_cache_anchors.size());
    const_address_cache_anchors[id] = anchor_id;
}

void RegAlloc::MarkAesChainTied(u32 id, u16 target) {
    ASSERT(id < aes_chain_targets.size());
    aes_chain_targets[id] = target;
}

void RegAlloc::MarkPshufdDirect(u32 id) {
    ASSERT(id < pshufd_direct.size());
    pshufd_direct[id] = true;
}

bool RegAlloc::IsHostWriteCoalesced(u32 id) const {
    return id < coalesced_host_writes.size() && coalesced_host_writes[id];
}

bool RegAlloc::IsHostReadCoalesced(u32 id) const {
    return id < coalesced_host_reads.size() && coalesced_host_reads[id];
}

bool RegAlloc::IsWidthChainCoalesced(u32 id) const {
    return id < width_chain_anchors.size() && width_chain_anchors[id] != UINT32_MAX;
}

u32 RegAlloc::WidthChainAnchor(u32 id) const {
    ASSERT(IsWidthChainCoalesced(id));
    return width_chain_anchors[id];
}

bool RegAlloc::IsLow32CopyCoalesced(u32 id) const {
    return id < low32_copy_sources.size() &&
           low32_copy_sources[id] != UINT32_MAX;
}

u32 RegAlloc::Low32CopySource(u32 id) const {
    ASSERT(IsLow32CopyCoalesced(id));
    return low32_copy_sources[id];
}

bool RegAlloc::HasWidthComponentOwner(u32 anchor_id) const {
    return anchor_id < width_component_owners.size() &&
           width_component_owners[anchor_id].Valid();
}

bool RegAlloc::WidthComponentOwnerCommitted(u32 anchor_id) const {
    return HasWidthComponentOwner(anchor_id) &&
           width_component_owners[anchor_id].committed;
}

u16 RegAlloc::WidthComponentOwnerTarget(u32 anchor_id) const {
    ASSERT(HasWidthComponentOwner(anchor_id));
    return width_component_owners[anchor_id].target;
}

bool RegAlloc::WidthComponentOwnerHighZero(u32 anchor_id) const {
    ASSERT(HasWidthComponentOwner(anchor_id));
    return width_component_owners[anchor_id].high_zero;
}

RegAlloc::GPRCoalesceState RegAlloc::SaveGPRCoalesceState() const {
    return {alloc_result, coalesced_host_writes, coalesced_host_reads,
            width_chain_anchors, width_component_owners};
}

void RegAlloc::RestoreGPRCoalesceState(GPRCoalesceState state) {
    ASSERT(state.mappings.size() == alloc_result.size());
    alloc_result = std::move(state.mappings);
    coalesced_host_writes = std::move(state.host_writes);
    coalesced_host_reads = std::move(state.host_reads);
    width_chain_anchors = std::move(state.width_anchors);
    width_component_owners = std::move(state.width_owners);
}

bool RegAlloc::IsConstAddressCached(u32 id) const {
    return id < const_address_cache_anchors.size() &&
           const_address_cache_anchors[id] != UINT32_MAX;
}

u32 RegAlloc::ConstAddressCacheAnchor(u32 id) const {
    ASSERT(IsConstAddressCached(id));
    return const_address_cache_anchors[id];
}

bool RegAlloc::IsAesChainTied(u32 id) const {
    return id < aes_chain_targets.size() && aes_chain_targets[id] != UINT16_MAX;
}

u16 RegAlloc::AesChainTarget(u32 id) const {
    ASSERT(IsAesChainTied(id));
    return aes_chain_targets[id];
}

bool RegAlloc::IsPshufdDirect(u32 id) const {
    return id < pshufd_direct.size() && pshufd_direct[id];
}

bool RegAlloc::IsFixedGPR(u32 id) const {
    id = ResolveId(id);
    return id < alloc_result.size() && alloc_result[id].type == GPR &&
           alloc_result[id].fixed_gpr;
}

void RegAlloc::SetLiveEnd(u32 id, u32 end) {
    ASSERT(id < live_ends.size());
    live_ends[id] = end;
}

bool RegAlloc::ValueLiveAfter(const ir::Value& value,
                              u32 instruction_id) const {
    if (!value.Defined()) {
        return false;
    }
    const u32 id = ResolveId(value.Id());
    return id >= live_ends.size() || live_ends[id] == UINT32_MAX ||
           live_ends[id] > instruction_id;
}

void RegAlloc::SetActiveRegs(swift::u32 id, GPRSMask& gprs, FPRSMask& fprs) {
    auto& map = alloc_result[id];
    map.dirty_gprs = gprs;
    map.dirty_fprs = fprs;
}

void RegAlloc::PermutePlacementCheckGPRHomes() {
    constexpr u32 left = 14;
    constexpr u32 right = 15;
    const auto swap_mask_bits = [=](GPRSMask& mask) {
        const bool left_set = mask.Get(left);
        const bool right_set = mask.Get(right);
        if (left_set != right_set) {
            left_set ? mask.Clear(left) : mask.Mark(left);
            right_set ? mask.Clear(right) : mask.Mark(right);
        }
    };
    for (auto& map : alloc_result) {
        if (map.type == GPR) {
            if (map.slot == left) map.slot = right;
            else if (map.slot == right) map.slot = left;
        }
        swap_mask_bits(map.dirty_gprs);
    }
}

ir::HostFPR RegAlloc::ValueFPR(const ir::Value& value) { return ValueFPR(value.Id()); }

ir::HostGPR RegAlloc::ValueGPR(const ir::Value& value) {
    return ValueGPR(value.Id());
}

ir::SpillSlot RegAlloc::ValueMem(const ir::Value& value) { return ValueMem(value.Id()); }

ir::HostGPR RegAlloc::ValueGPR(u32 id) {
    id = ResolveId(id);
    ASSERT(alloc_result[id].type == GPR);
    return ir::HostGPR{alloc_result[id].slot};
}

ir::HostFPR RegAlloc::ValueFPR(u32 id) {
    id = ResolveId(id);
    ASSERT(alloc_result[id].type == FPR);
    return ir::HostFPR{alloc_result[id].slot};
}

ir::SpillSlot RegAlloc::ValueMem(u32 id) {
    id = ResolveId(id);
    ASSERT(alloc_result[id].type == MEM);
    return ir::SpillSlot{alloc_result[id].slot};
}

u32 RegAlloc::AllocationId(const ir::Value& value) const {
    return ResolveId(value.Id());
}

const RegAlloc::SpillReload* RegAlloc::SpillReloadAt(
        u32 value_id, u32 instruction_id) const {
    value_id = ResolveId(value_id);
    const auto* reloads = SpillReloadsAt(instruction_id);
    if (!reloads) {
        return nullptr;
    }
    const auto it = std::find_if(reloads->begin(), reloads->end(), [value_id](const auto& reload) {
        return reload.value == value_id;
    });
    return it == reloads->end() ? nullptr : &*it;
}

const RegAlloc::SpillReload* RegAlloc::SpillReloadAt(
        const ir::Value& value, u32 instruction_id) const {
    return SpillReloadAt(value.Id(), instruction_id);
}

bool RegAlloc::HasSpillReload(u32 value_id, u32 instruction_id) const {
    return SpillReloadAt(value_id, instruction_id) != nullptr;
}

const Vector<RegAlloc::SpillReload>* RegAlloc::SpillReloadsAt(
        u32 instruction_id) const {
    if (instruction_id >= spill_reloads.size() || spill_reloads[instruction_id].empty()) {
        return nullptr;
    }
    return &spill_reloads[instruction_id];
}

RegAlloc::Type RegAlloc::ValueType(const ir::Value& value) {
    const u32 vid = value.Id();
    if (vid >= alloc_result.size()) {
        auto* def = value.Def();
        SVM_DIAG_PRINT(Runtime,
                     "[OOB-ValueType] value.Id()=%u alloc_size=%zu def=%p op=%u\n",
                     vid, alloc_result.size(), (void*)def,
                     def ? (unsigned)def->GetOp() : 0xffffu);
#if defined(__linux__)
        void* bt[32];
        int n = backtrace(bt, 32);
        backtrace_symbols_fd(bt, n, STDERR_FILENO);
#endif
    }
    return alloc_result[ResolveId(vid)].type;
}

u32 RegAlloc::ResolveId(u32 id) const {
    while (true) {
        if (id >= alloc_result.size()) {
            SVM_DIAG_PRINT(Runtime,
                         "[OOB-ResolveId] id=%u alloc_size=%zu (read past end)\n",
                         id, alloc_result.size());
            return id;
        }
        if (alloc_result[id].type != REF) {
            break;
        }
        const u32 next = alloc_result[id].slot;
        if (next >= alloc_result.size()) {
            SVM_DIAG_PRINT(Runtime,
                         "[OOB-ResolveId] REF chain: id=%u slot=%u alloc_size=%zu\n",
                         id, next, alloc_result.size());
        }
        id = next;
    }
    return id;
}

const GPRSMask& RegAlloc::GetGprs() const { return gprs; }

const FPRSMask& RegAlloc::GetFprs() const { return fprs; }

u32 RegAlloc::SpillCount() const {
    return static_cast<u32>(std::count_if(
            alloc_result.begin(), alloc_result.end(),
            [](const Map& map) { return map.type == MEM; }));
}

GPRSMask RegAlloc::GetDirtyGPR() const {
    return alloc_result[current_ir->Id()].dirty_gprs;
}

FPRSMask RegAlloc::GetDirtyFPR() const {
    return alloc_result[current_ir->Id()].dirty_fprs;
}

void RegAlloc::SetCurrent(ir::Inst* inst) { current_ir = inst; }

}  // namespace swift::runtime::backend
