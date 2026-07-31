//
// Created by 甘尧 on 2023/10/13.
//

#include <algorithm>
#include "reg_alloc.h"
#include "runtime/common/perf_stats.h"

namespace swift::runtime::backend {

static int X86PinExtLevel() {
    static const int level = [] {
        const char* value = PerfGetenv("SVM_X86_PIN_EXT");
        return value ? std::max(0, std::atoi(value)) : 0;
    }();
    return level;
}

bool ScratchXPoolRequested() {
    static const bool requested = [] {
        const char* value = PerfGetenv("SVM_JIT_SCRATCH_XPOOL");
        return value && std::strcmp(value, "0") != 0;
    }();
    return requested;
}

bool ScratchXPoolAutoEnabled() {
    return X86PinExtLevel() >= 3 && !ScratchXPoolRequested();
}

bool X86PinExtLevel3Requested() {
    return X86PinExtLevel() >= 3;
}

bool ScratchXPoolEnabled() {
    return ScratchXPoolRequested() || X86PinExtLevel() >= 3;
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

bool X86PinExtScratchOnlyEnabled(const GPRSMask& pool) {
    return X86PinExtLevel2Enabled(pool) && !ScratchXPoolEnabled() &&
           pool.Get(12) && pool.Get(13);
}

bool X86PinExtLevel3AluScratchEnabled(const GPRSMask& pool, ir::OpCode op) {
    return X86PinExtLevel3Enabled(pool) && pool.Get(10) &&
           (op == ir::OpCode::Add || op == ir::OpCode::Sub ||
            op == ir::OpCode::VecFCvtFloatToInt ||
            op == ir::OpCode::CallLambda || op == ir::OpCode::CallDynamic ||
            op == ir::OpCode::CallLocation);
}

static bool X86PinExtEnabled() {
    return X86PinExtLevel() >= 1;
}

u32 FixedGPRClobbers(ir::OpCode op, bool scratch_only) {
    // Level 2 without XPOOL leases the otherwise globally-reserved x12/x13 as
    // scratch-only registers. It therefore needs the same fixed-clobber
    // exclusions even though those registers remain unavailable to linear
    // scan as value locations.
    if (!ScratchXPoolEnabled() && !scratch_only) {
        return 0;
    }
    constexpr u32 x11 = 1u << 11;
    constexpr u32 x12 = 1u << 12;
    constexpr u32 x13 = 1u << 13;
    switch (op) {
        // x11 is the exclusive-store status register; x12 holds the value
        // that must survive until the store-exclusive.
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
            return x11 | x12;
        // Pair CAS additionally holds the second observed half in x13.
        case ir::OpCode::CompareAndSwap128:
            return x11 | x12 | x13;
        // The OFF-XPOOL x87 lowering names x12/x13 directly.
        case ir::OpCode::X87Op:
            // Level 2 without XPOOL takes the exact helper fallback instead
            // of the high-pressure inline lowering, so this pair is available
            // as instruction-local scratch there.
            return 0;
        // Host-call target and fixed-location materialization.
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::CallLocation:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::SetLocation:
        case ir::OpCode::CheckMemoryAlignment:
            return x11;
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
            return x13;
        default:
            return 0;
    }
}

// See reg_alloc.h for what this table is and who enforces it.
//
// Only opcodes that exceed the default appear here. Every entry is a measured
// peak over the full corpus, cross-checked against the emitter source:
//
//  * X87Op historically declared eight: six emitter temporaries plus the
//    explicit ip0/ip1 exclusions required by VIXL's implicit pool. W52 ON
//    removes those exclusions and leases the two former x12/x13 work
//    registers, making the checked dynamic peak six. OFF retains eight so its
//    allocation and host bytes remain identical.
//  * VecFCvtFloatToInt open-codes the x86 "invalid conversion -> INT_MIN"
//    rule per lane and holds five GPRs across it.
//  * VecFCvtPacked and VecFMulAdd hold five vector temporaries.
//  * The float arithmetic family needs no GPR at all: since the NaN fixup
//    became NEON (three vector temporaries, all lanes at once) their demand is
//    that three plus whatever the calling emitter is already holding -- two
//    for the 32-bit scalar shapes (merge target + the FPR-materialised right
//    operand), one for the 64-bit ones, none for the packed ones.
ScratchNeed ScratchBudget(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::AtomicExchange:
            // Level 3's spill-aware accounting must use the measured emitter
            // peak, not the generic three-register default: address, desired
            // and result reloads replace ordinary value locations, while the
            // emitter itself only holds MergeNZCV's shared temporary. The
            // exclusive status/value registers are fixed clobbers accounted
            // separately by the allocator.
            if (X86PinExtLevel() >= 3) {
                return {1, kDefaultScratchFPR};
            }
            return {kDefaultScratchGPR, kDefaultScratchFPR};
        // --- x87 GPRs ----------------------------------------------------
        case ir::OpCode::X87Op:
            if ((X86PinExtLevel() >= 2 && !ScratchXPoolEnabled()) ||
                X86PinExtLevel() >= 3) {
                // The reduced dynamic pool cannot cover the inline x87 peak.
                // The emitter deliberately selects its exact host-helper
                // fallback in this state; price that ordinary call shape.
                return {kDefaultScratchGPR, kDefaultScratchFPR};
            }
            // OFF retains the historical accounting: two explicit ip0/ip1
            // exclusions plus six emitter temporaries. ON makes VIXL scratch
            // explicit and leases the former x12/x13 x87 work registers, so
            // the measured dynamic peak is six.
            return {static_cast<u8>(ScratchXPoolEnabled() ? 6 : 8),
                    kDefaultScratchFPR};
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

RegAlloc::RegAlloc(u32 instr_size, const GPRSMask& gprs, const FPRSMask& fprs)
        : alloc_result(instr_size), gprs(gprs), fprs(fprs) {}

void RegAlloc::ResetAllocations() {
    std::fill(alloc_result.begin(), alloc_result.end(), Map{});
    current_ir = nullptr;
}

void RegAlloc::MapRegister(u32 id, ir::HostFPR fpr) {
    auto& map = alloc_result[id];
    map.type = FPR;
    map.slot = fpr.id;
}

void RegAlloc::MapRegister(u32 id, ir::HostGPR gpr) {
    auto& map = alloc_result[id];
    map.type = GPR;
    map.slot = gpr.id;
}

void RegAlloc::MapMemSpill(u32 id, ir::SpillSlot slot) {
    auto& map = alloc_result[id];
    map.type = MEM;
    map.slot = slot.offset;
}

void RegAlloc::MapReference(u32 from, u32 to) {
    auto& map = alloc_result[to];
    map.type = REF;
    map.slot = from;
}

void RegAlloc::SetActiveRegs(swift::u32 id, GPRSMask& gprs, FPRSMask& fprs) {
    auto& map = alloc_result[id];
    map.dirty_gprs = gprs;
    map.dirty_fprs = fprs;
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

RegAlloc::Type RegAlloc::ValueType(const ir::Value& value) {
    return alloc_result[ResolveId(value.Id())].type;
}

u32 RegAlloc::ResolveId(u32 id) const {
    while (alloc_result[id].type == REF) {
        id = alloc_result[id].slot;
    }
    return id;
}

const GPRSMask& RegAlloc::GetGprs() const { return gprs; }

const FPRSMask& RegAlloc::GetFprs() const { return fprs; }

GPRSMask RegAlloc::GetDirtyGPR() const {
    return alloc_result[current_ir->Id()].dirty_gprs;
}

FPRSMask RegAlloc::GetDirtyFPR() const {
    return alloc_result[current_ir->Id()].dirty_fprs;
}

void RegAlloc::SetCurrent(ir::Inst* inst) { current_ir = inst; }

}  // namespace swift::runtime::backend
