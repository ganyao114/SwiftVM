//
// Created by 甘尧 on 2024/6/21.
//

#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_set>
#if defined(__APPLE__) && defined(__aarch64__)
#include <sys/sysctl.h>
#elif defined(__aarch64__) && defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>

// Linux arm64 UAPI: arch/arm64/include/uapi/asm/hwcap.h.  Keep the fallback
// for builds against older kernel headers; these are bits 27 and 7 (not the
// unrelated PMULL/SVE2 bits 4 and 1).
#ifndef HWCAP_FLAGM
#define HWCAP_FLAGM (1UL << 27)
#endif
#ifndef HWCAP2_FLAGM2
#define HWCAP2_FLAGM2 (1UL << 7)
#endif
#ifndef HWCAP2_AFP
#define HWCAP2_AFP (1UL << 20)
#endif
#endif
#include "fmt/format.h"
#include "base/scope_exit.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/jit_code.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/common/perf_stats.h"
#include "runtime/frontend/x86/decoder.h"
#include "runtime/include/sruntime.h"
#include "translator.h"
#include "translator/function_stats.h"

namespace swift::translator::x86 {

using namespace swift::runtime;
using namespace swift::x86;

// Conservative static guest->host map. RBX/RSP/RBP are hot general, stack and
// frame registers; x19-x21 are AArch64 ABI callee-saved and do not overlap the
// runtime's x24-x28 state/cache/flags/RSB/page-table assignments.
// SVM_X86_PIN_EXT=1 extends the map with RAX/RCX in callee-saved x22/x23 and
// RDX in x29. Level 2 additionally keeps RSI/RDI/R8-R11 in caller-saved
// x0-x5; level 3 completes the GPR map with R12-R15 in x6-x9. EmitHostCall
// snapshots every caller-saved pin around each helper. Descriptors stay sorted
// by uniform offset so the trampoline can pair adjacent saves.
// The trampoline reserves them from linear scan, restores them on runtime
// entry and spills them on every host exit. Inline CallLambda helpers do not
// receive the uniform buffer and must preserve x19-x23 by the platform ABI;
// x29 is saved explicitly alongside LR.
// The legacy asm-interpreter uses x21 as `handle`; this x86 Config leaves that
// mutually-exclusive path disabled.
static UniformMapDesc arm64_backend_gpr_regs_map[] = {
        {offsetof(ThreadContext64, rbx), 8, 20, false},
        {offsetof(ThreadContext64, rsp), 8, 19, false},
        {offsetof(ThreadContext64, rbp), 8, 21, false},
};
static UniformMapDesc arm64_backend_gpr_regs_ext_map[] = {
        {offsetof(ThreadContext64, rax), 8, 22, false},
        {offsetof(ThreadContext64, rcx), 8, 23, false},
        {offsetof(ThreadContext64, rdx), 8, 29, false},
        {offsetof(ThreadContext64, rbx), 8, 20, false},
        {offsetof(ThreadContext64, rsp), 8, 19, false},
        {offsetof(ThreadContext64, rbp), 8, 21, false},
};
static UniformMapDesc arm64_backend_gpr_regs_ext2_map[] = {
        {offsetof(ThreadContext64, rax), 8, 22, false},
        {offsetof(ThreadContext64, rcx), 8, 23, false},
        {offsetof(ThreadContext64, rdx), 8, 29, false},
        {offsetof(ThreadContext64, rbx), 8, 20, false},
        {offsetof(ThreadContext64, rsp), 8, 19, false},
        {offsetof(ThreadContext64, rbp), 8, 21, false},
        {offsetof(ThreadContext64, rsi), 8, 0, false},
        {offsetof(ThreadContext64, rdi), 8, 1, false},
        {offsetof(ThreadContext64, r8), 8, 2, false},
        {offsetof(ThreadContext64, r9), 8, 3, false},
        {offsetof(ThreadContext64, r10), 8, 4, false},
        {offsetof(ThreadContext64, r11), 8, 5, false},
};
static UniformMapDesc arm64_backend_gpr_regs_ext3_map[] = {
        {offsetof(ThreadContext64, rax), 8, 22, false},
        {offsetof(ThreadContext64, rcx), 8, 23, false},
        {offsetof(ThreadContext64, rdx), 8, 29, false},
        {offsetof(ThreadContext64, rbx), 8, 20, false},
        {offsetof(ThreadContext64, rsp), 8, 19, false},
        {offsetof(ThreadContext64, rbp), 8, 21, false},
        {offsetof(ThreadContext64, rsi), 8, 0, false},
        {offsetof(ThreadContext64, rdi), 8, 1, false},
        {offsetof(ThreadContext64, r8), 8, 2, false},
        {offsetof(ThreadContext64, r9), 8, 3, false},
        {offsetof(ThreadContext64, r10), 8, 4, false},
        {offsetof(ThreadContext64, r11), 8, 5, false},
        {offsetof(ThreadContext64, r12), 8, 6, false},
        {offsetof(ThreadContext64, r13), 8, 7, false},
        {offsetof(ThreadContext64, r14), 8, 8, false},
        {offsetof(ThreadContext64, r15), 8, 9, false},
};

#define SVM_XMM_RESIDENT_DESC(i) \
    UniformMapDesc{offsetof(ThreadContext64, xmms) + (i) * sizeof(Xmm), 16, 16 + (i), true}
static constexpr std::array<UniformMapDesc, 8> arm64_backend_xmm_resident_map{{
        SVM_XMM_RESIDENT_DESC(0), SVM_XMM_RESIDENT_DESC(1),
        SVM_XMM_RESIDENT_DESC(2), SVM_XMM_RESIDENT_DESC(3),
        SVM_XMM_RESIDENT_DESC(4), SVM_XMM_RESIDENT_DESC(5),
        SVM_XMM_RESIDENT_DESC(6), SVM_XMM_RESIDENT_DESC(7),
}};
#undef SVM_XMM_RESIDENT_DESC

// This describes guest architectural state only; it does not pin any host
// FPRs.  UniformElimination uses it to include the U64 XmmLo/XmmHi views in
// SVM_XMM_UNIFORM_FWD's scope as well as direct V128 accesses.
static UniformRangeDesc x86_xmm_uniform_ranges[] = {
        {offsetof(ThreadContext64, xmms), sizeof(ThreadContext64::xmms)},
};

// Instruction-fetch memory interface for the x86 decoder. With guest
// address virtualization (memory_base), guest address G is backed by host
// memory at G + bias; the loader installs the bias via SetBias (0 =
// identity, the default for tests / non-loader embedders).
class MemoryImpl : public runtime::MemoryInterface {
public:
    void SetBias(u64 b) { bias = b; }
    // Bounded guest window (Config::guest_addr_mask): truncate before biasing
    // so a guest address can only ever name the embedder's window.
    void SetMask(u64 m) { mask = m ? m : UINT64_MAX; }
    bool Read(void* dest, size_t addr, size_t size) override {
        return std::memcpy(dest, reinterpret_cast<const void*>((addr & mask) + bias), size);
    }
    bool Write(void* src, size_t addr, size_t size) override {
        return std::memcpy(reinterpret_cast<void*>((addr & mask) + bias), src, size);
    }
    // Instruction fetch. This runs in *host* code, so a fault here is not
    // recoverable: runtime.cpp's HandleFault only rewrites faults whose host
    // pc lies inside a JIT buffer, and the decoder's is not. A wild guest
    // branch target (RET off a corrupted stack, `jmp rax` with garbage, an
    // encoding the decoder mis-sizes) would therefore kill the host process
    // instead of the guest. Validate against the embedder's guest-mapping
    // oracle and hand the decoder a nullptr -> ExitReason::PageFatal.
    // Embedders that installed no oracle (unit tests, fuzzers, identity
    // mappings) keep the raw bias add.
    void* GetPointer(void* src) override {
        const auto host = (reinterpret_cast<uintptr_t>(src) & mask) + bias;
        if (runtime::backend::SignalHandler::HasGuestMapProbe() &&
            !runtime::backend::SignalHandler::IsGuestAddressMapped(host)) {
            return nullptr;
        }
        return reinterpret_cast<void*>(host);
    }
    u64 bias{};
    u64 mask{UINT64_MAX};
};

static MemoryImpl memory_impl{};

static runtime::TsoMode TsoModeFromEnvironment() {
    const auto& value = runtime::GetSvmConfig().tso_mode;
    if (value == "relaxed" || value == "Relaxed") {
        return runtime::TsoMode::Relaxed;
    }
    if (value == "acqrel" || value == "AcqRel") {
        return runtime::TsoMode::AcqRel;
    }
    if (value == "hardware" || value == "Hardware") {
        return runtime::TsoMode::Hardware;
    }
    LOG_WARNING("Unknown SVM_TSO_MODE '{}'; using relaxed", value);
    return runtime::TsoMode::Relaxed;
}

static Arm64Features DetectArm64Features() {
    Arm64Features features = Arm64Features::None;

#if defined(__APPLE__) && defined(__aarch64__)
    auto sysctl_feature = [](const char* name) {
        int value = 0;
        size_t size = sizeof(value);
        return sysctlbyname(name, &value, &size, nullptr, 0) == 0 &&
               size == sizeof(value) && value != 0;
    };
    if (sysctl_feature("hw.optional.arm.FEAT_LRCPC")) {
        features |= Arm64Features::RCpc;
    }
    if (sysctl_feature("hw.optional.arm.FEAT_LRCPC2")) {
        features |= Arm64Features::RCpcImm;
    }
    if (sysctl_feature("hw.optional.arm.FEAT_AFP")) {
        features |= Arm64Features::AFP;
    }
    if (sysctl_feature("hw.optional.arm.FEAT_FlagM")) {
        features |= Arm64Features::FlagM;
    }
    if (sysctl_feature("hw.optional.arm.FEAT_FlagM2")) {
        features |= Arm64Features::AXFlag;
    }
#elif defined(__aarch64__) && defined(__linux__)
    // HWCAP is a process constant.  Cache both auxv reads so every Instance
    // receives the same feature bitmap, which is then part of ConfigHash.
    static const Arm64Features flag_features = [] {
        Arm64Features detected = Arm64Features::None;
        if ((getauxval(AT_HWCAP) & HWCAP_FLAGM) != 0) {
            detected |= Arm64Features::FlagM;
        }
        if ((getauxval(AT_HWCAP2) & HWCAP2_FLAGM2) != 0) {
            detected |= Arm64Features::AXFlag;
        }
        if ((getauxval(AT_HWCAP2) & HWCAP2_AFP) != 0) {
            detected |= Arm64Features::AFP;
        }
        return detected;
    }();
    features |= flag_features;
#endif

    // Diagnostic/bring-up override. The default remains the OS feature probe;
    // forcing an unsupported instruction will SIGILL, so this is intentionally
    // not a general user-facing mode switch.
    const auto& svm_config = runtime::GetSvmConfig();
    if (svm_config.arm64_lrcpc_override_is_set) {
        if (svm_config.arm64_lrcpc_override == "0") {
            features = static_cast<Arm64Features>(
                    static_cast<u32>(features) & ~static_cast<u32>(Arm64Features::RCpc));
        } else if (svm_config.arm64_lrcpc_override == "1") {
            features |= Arm64Features::RCpc;
        }
    }
    return features;
}

static bool SSEScalarInsertEnabled(Arm64Features features) {
#if defined(__APPLE__) && defined(__aarch64__)
    // Preserve W23 exactly: it was default-on on AFP-capable macOS hosts and
    // unavailable on Linux because the old Linux probe never published AFP.
    // Hardware capability is now platform-neutral, so policy must be kept
    // separate or merely detecting AFP would change Linux OFF codegen.
    return True(features & Arm64Features::AFP) &&
           runtime::GetSvmConfig().sse_scalar_insert;
#else
    (void) features;
    return false;
#endif
}

static bool SSEAFPNanEnabled(Arm64Features features) {
    const bool requested = runtime::GetSvmConfig().sse_afp_nan;
    if (!requested) {
        return false;
    }
    if (True(features & Arm64Features::AFP)) {
        return true;
    }
    static std::once_flag warning_once;
    std::call_once(warning_once, [] {
        LOG_WARNING("SVM_SSE_AFP_NAN defaults on but FEAT_AFP is unavailable; using guarded SSE path");
    });
    return false;
}

// WORKAROUND (runtime bug, ir/instr.h Inst::GetArg<Operand>): the x86
// frontend legitimately emits single-sided ir::Operand args (e.g.
// ir::Operand{left} for a RIP-relative lea / absolute address); the empty
// right side is stored as a Void arg (ir/args.cpp DataClass::ToArgClass),
// but Inst::GetArg<ir::Operand>() blindly calls ToDataClass() on it and
// PANICs with "Invalid arg type!". Until the runtime handles Void right
// sides, rewrite single-sided operands after decoding:
//   - left is a Value  -> right = Imm(0) (single-sided ops are always
//     OperandOp::Plus, so left + 0 == left; the arm64 backend's
//     EmitOperand handles this form);
//   - left is an Imm and the instruction is GetOperand -> rewrite the
//     whole instruction to LoadImm(imm) (GetOperand(#imm) == #imm);
//   - left is an Imm otherwise (absolute memory address) -> materialize
//     the immediate with a LoadImm in front of the instruction and use
//     that value as the left side with right = Imm(0).
static void FixupSingleSidedOperands(ir::Block* block) {
    bool inserted = false;
    for (auto& inst : block->GetInstList()) {
        for (int i = 0; i + 2 < ir::Inst::max_args; ++i) {
            if (!inst.ArgAt(i).IsOperand() || !inst.ArgAt(i + 2).IsVoid()) {
                continue;
            }
            auto& left_arg = inst.ArgAt(i + 1);
            if (left_arg.IsValue()) {
                inst.SetArg(i + 2, ir::Imm(u64(0)));
            } else if (left_arg.IsImm()) {
                auto imm = left_arg.Get<ir::Imm>();
                if (inst.GetOp() == ir::OpCode::GetOperand) {
                    inst.SetInst(ir::OpCode::LoadImm, imm);
                    // LoadImm takes a single arg; drop the two leftover
                    // slots of the old operand encoding.
                    inst.DestroyArg(1);
                    inst.DestroyArg(2);
                } else {
                    auto* li = new ir::Inst(ir::OpCode::LoadImm);
                    li->SetArgs(imm);
                    li->SetReturn(ir::ValueType::U64);
                    block->InsertBefore(li, &inst);
                    inst.SetArg(i + 1, ir::Value{li});
                    inst.SetArg(i + 2, ir::Imm(u64(0)));
                    inserted = true;
                }
            }
        }
    }
    if (inserted) {
        block->ReIdInstr();
    }
}

// WORKAROUND (runtime contract gap): the arm64 backend allows at most one
// pending SaveFlags / ClearFlags per flush window (flush points are
// AdvancePC and block end; see backend/arm64/jit/translator.cpp
// EmitSaveFlags: "ASSERT(flags_set == ir::Flags::None)"). The x86 frontend
// legitimately emits TWO SaveFlags in a row for narrow (8/16-bit) add/sub:
// one for PF/AF on the real result and one for NZCV on a bit-31-shifted
// result (frontend/x86/decoder.cc, the "Exact NZCV from the shifted op"
// sequence). Insert a zero-length AdvancePC (a pure flags-flush marker)
// right after the first SaveFlags/ClearFlags so the invariant holds.
//
// NB: Block::InsertAfter(x, anchor) is itself buggy and inserts *before*
// anchor (runtime/ir/block.cpp), so to land right AFTER the first
// SaveFlags we insert "after" its successor.
[[maybe_unused]] static void FixupConsecutiveFlagOps(ir::Block* block) {
    auto& list = block->GetInstList();
    bool inserted = false;
    ir::Inst* pending_save = nullptr;
    ir::Inst* pending_clear = nullptr;
    auto insert_flush_after = [&](ir::Inst* anchor) {
        auto next = std::next(list.iterator_to(*anchor));
        ASSERT(next != list.end());
        auto* flush = new ir::Inst(ir::OpCode::AdvancePC);
        flush->SetArgs(ir::Imm(u64(0)));
        block->InsertAfter(flush, &*next);  // buggy InsertAfter == insert before next
        inserted = true;
    };
    for (auto& inst : list) {
        switch (inst.GetOp()) {
            case ir::OpCode::AdvancePC:
                pending_save = nullptr;
                pending_clear = nullptr;
                break;
            case ir::OpCode::SaveFlags:
                if (pending_save) {
                    insert_flush_after(pending_save);
                }
                pending_save = &inst;
                break;
            case ir::OpCode::ClearFlags:
                if (pending_clear) {
                    insert_flush_after(pending_clear);
                }
                pending_clear = &inst;
                break;
            default:
                break;
        }
    }
    if (inserted) {
        block->ReIdInstr();
    }
}

// WORKAROUND (runtime bug, backend/arm64/jit/translator.cpp MergeNZCV +
// EmitAdvancePC): at every AdvancePC the backend OR-merges the host NZCV
// into its sticky flags register instead of *replacing* those bits, so a
// Z/N/C/V bit set by an older instruction sticks forever and poisons any
// later flags consumer that reloads from the sticky register (observed:
// `xor eax,eax` sets Z=1; a later `cmp`+`jne` reads the poisoned Z and
// takes the wrong branch). The only time the consumer sees correct flags
// is when no AdvancePC flushed between the producer and the consumer
// (host NZCV still dirty). The x86 frontend emits AdvancePC after every
// guest instruction, so delete an AdvancePC when it both closes a window
// with a pending SaveFlags and is immediately followed by a flags consumer
// (TestFlags/TestNotFlags/GetFlags/CondSelect) — this keeps the producer's host NZCV
// alive until the consumer. Cross-block flags (consumer before any
// producer in the next block) still read the poisoned sticky register —
// that needs the runtime fix (make MergeNZCV replace, not accumulate).
[[maybe_unused]] static void FixupFlagFlushBeforeConsumer(ir::Block* block) {
    auto& list = block->GetInstList();
    Vector<ir::Inst*> to_delete{};
    bool pending_save = false;
    for (auto it = list.begin(); it != list.end(); ++it) {
        const auto op = it->GetOp();
        if (op == ir::OpCode::SaveFlags) {
            pending_save = true;
            continue;
        }
        if (op != ir::OpCode::AdvancePC) {
            continue;
        }
        const bool had_save = pending_save;
        pending_save = false;
        if (!had_save) {
            continue;
        }
        // Look ahead: is the next flag-relevant op a consumer?
        for (auto look = std::next(it); look != list.end(); ++look) {
            const auto lop = look->GetOp();
            if (lop == ir::OpCode::TestFlags || lop == ir::OpCode::TestNotFlags ||
                lop == ir::OpCode::GetFlags || lop == ir::OpCode::CondSelect) {
                to_delete.push_back(&*it);
                break;
            }
            if (lop == ir::OpCode::SaveFlags || lop == ir::OpCode::ClearFlags ||
                lop == ir::OpCode::AdvancePC) {
                break;  // another producer / flush first: keep this AdvancePC
            }
        }
    }
    for (auto* inst : to_delete) {
        block->DestroyInst(inst);
    }
    if (!to_delete.empty()) {
        block->ReIdInstr();
    }
}

// Scratch uniform slot (see MaterializeTerminalCondUse below). It lives
// right past the ThreadContext64 in the uniform buffer, which is allocated
// with kScratchUniformSize extra bytes for exactly this purpose.
static constexpr u32 kScratchUniformOffset = (sizeof(ThreadContext64) + 7) & ~u32(7);
static constexpr u32 kScratchUniformSize = 16;

// WORKAROUND (runtime bug, ir/opts/register_alloc_pass.cpp
// LinearScanAllocator::CollectLiveIntervals(Block*)): liveness only scans
// *instruction* arguments, so a value whose only use is the block terminal
// (e.g. the condition of a terminal::If, produced by every x86 conditional
// jump) is treated as dead ("if (!end) continue") and gets no register
// allocation; the arm64 backend then asserts
// "alloc_result[id].type == GPR" when emitting the branch. Append a dummy
// StoreUniform of the terminal condition to a scratch uniform slot so the
// value has a real in-block use. The store is side-effect free wrt the
// guest (scratch memory only).
[[maybe_unused]] static void MaterializeTerminalCondUse(ir::Block* block) {
    auto terminal = block->GetTerminal();
    auto* if_term = boost::get<ir::terminal::If>(&terminal);
    if (!if_term) {
        // TODO: terminal::Switch has the same problem (its dispatch value
        // is also only referenced from the terminal).
        return;
    }
    ir::Value cond = if_term->cond;
    if (!cond.Def()) {
        return;
    }
    ir::Uniform scratch{kScratchUniformOffset, ir::ValueType::U8};
    block->AppendInst(ir::OpCode::StoreUniform, scratch, cond);
    block->ReIdInstr();
}

// WORKAROUND (runtime contract bug, ir/instr.cpp Inst::HasSideEffects +
// ir/opts/deadcode_elimination_pass.cpp): a CallLambda whose U64 result has
// no uses is classified as side-effect free, so DeadCodeRemove erases it —
// but the frontend's void-ish host helpers (x86 decoder RepMovs, decoder.cc)
// perform guest memory writes inside the host call. Observed: musl memcpy's
// `rep movsq` silently dropped under JIT (the IR interpreter has no DCE and
// is unaffected), leaving the destination unwritten. Pin every unused
// CallLambda with a dummy StoreUniform to the scratch uniform slot (same
// trick as MaterializeTerminalCondUse above) so the call survives DCE. The
// proper fix belongs to the runtime (treat CallLambda as side-effecting) or
// the frontend (return a value that is consumed, like RepStos does).
static void PinUnusedCallLambdas(ir::Block* block) {
    bool inserted = false;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() == ir::OpCode::CallLambda && inst.GetUses() == 0) {
            ir::Uniform scratch{kScratchUniformOffset, ir::ValueType::U64};
            block->AppendInst(ir::OpCode::StoreUniform, scratch, ir::Value{&inst});
            inserted = true;
        }
    }
    if (inserted) {
        block->ReIdInstr();
    }
}


// 这些开关由进程配置快照统一解析，逐 unit 路径不再扫描 environ。
// Blocks decoded per function-compile attempt before the region is closed and
// the remaining successors are left for on-demand compilation.  SVM_FUNC_LAZY=0
// restores eager whole-function decoding (the pre-2026-07-27 behaviour).
//
// Why a *region* rather than a whole function: measured on func_tests /
// real_busy (static glibc), 56% of the blocks inside eagerly compiled function
// units are never executed, and a short-lived guest spends ~60% of its wall
// clock translating.  Why this is safe at any budget: no IR value crosses a
// block boundary in function mode -- uniform and flag elimination are
// block-local and FlushFlags runs at every block end (measured: 0 cross-block
// operands over 17431 operands / 822 blocks on func_tests) -- so every decoded
// block already is a self-contained entry point.  TranslateIR has always
// relied on that, publishing every decoded block into the L2 dispatch table.
static size_t LazyFuncBudget() {
    return static_cast<size_t>(runtime::GetSvmConfig().func_lazy);
}

struct X86Instance::Impl final {
    // memory_base: guest->host bias (host addr = guest addr + bias), installed
    // by the linux loader; nullptr keeps the identity-mapped fast path.
    explicit Impl(void* memory_base, u64 guest_addr_mask) {
        memory_impl.SetBias(reinterpret_cast<uintptr_t>(memory_base));
        memory_impl.SetMask(guest_addr_mask);
        // Host helpers in the frontend (rep movs/stos, x87, xsave) dereference
        // raw guest pointers; they read the same bias and window mask from the
        // frontend-side globals.
        x86::SetGuestMemBias(reinterpret_cast<uintptr_t>(memory_base));
        x86::SetGuestAddrMask(guest_addr_mask);
        // SVM_ENABLE_JIT=0 forces the IR interpreter path (same switch as the
        // arm64 core; useful for cross-checking JIT results).
        const auto& svm_config = runtime::GetSvmConfig();
        const bool enable_jit = svm_config.enable_jit;
        // Default-on, with a diagnostic escape hatch for before/after
        // validation and field bisects.
        const bool enable_uniform_elim = svm_config.uniform_elim;
        // The interpreter's GetHostGPR/SetHostGPR handlers intentionally have
        // no host-register state, so never emit mapped ops in interpreter mode.
        // Default-on; keep an explicit escape hatch for diagnostics.
        const bool enable_static_regs =
                enable_jit && enable_uniform_elim &&
                svm_config.static_regs;
        // Default ON after the flip A/B (smallpt 5/5 pairs positive, median
        // 1.26, pixel-identical output); =0 selects the eager-store rollback.
        const bool enable_xmm_fault_sink =
                enable_jit && svm_config.xmm_fault_sink;
        // W55/W56/W60 opt-in extension of the scalar static map. Level 1 is the
        // byte-identical W55 map; level 2 adds six caller-saved pins; level 3
        // adds the remaining four. The backend makes XPOOL effective at level
        // 3 because the non-XPOOL value pool would contain only x14/x15.
        // Default level 2 after the flip A/B (bundle with XPOOL, coremark
        // 5/5 pairs positive, median 1.22); =0 restores the pre-W55 map as
        // the rollback. Level 3 stays opt-in only (measured -10.18% vs
        // level 2 on coremark).
        const int pin_ext_level = enable_static_regs
                ? static_cast<int>(svm_config.x86_pin_ext)
                : 0;
        const bool enable_pin_ext = pin_ext_level >= 1;
        const bool enable_pin_ext2 = pin_ext_level >= 2;
        const bool enable_pin_ext3 = pin_ext_level >= 3;
        std::span<UniformMapDesc> static_regs;
        if (enable_pin_ext3) {
            static_regs = arm64_backend_gpr_regs_ext3_map;
        } else if (enable_pin_ext2) {
            static_regs = arm64_backend_gpr_regs_ext2_map;
        } else if (enable_pin_ext) {
            static_regs = arm64_backend_gpr_regs_ext_map;
        } else if (enable_static_regs) {
            static_regs = arm64_backend_gpr_regs_map;
        }
        const bool enable_xmm_resident =
                enable_jit && enable_uniform_elim && svm_config.xmm_resident;
        if (enable_xmm_resident) {
            static_regs_storage.assign(static_regs.begin(), static_regs.end());
            static_regs_storage.insert(static_regs_storage.end(),
                                       arm64_backend_xmm_resident_map.begin(),
                                       arm64_backend_xmm_resident_map.end());
            static_regs = static_regs_storage;
        }
        const bool enable_block_link = svm_config.block_link;
        auto global_opts = Optimizations::ReturnStackBuffer | Optimizations::FlagElimination |
                           Optimizations::DeadCodeRemove | Optimizations::StaticCode |
                           Optimizations::ConstantFolding | Optimizations::FunctionBaseCompile;
        if (enable_block_link) {
            global_opts |= Optimizations::BlockLink;
        }
        if (enable_uniform_elim) {
            global_opts |= Optimizations::UniformElimination;
        }
        if (enable_xmm_fault_sink || enable_xmm_resident) {
            global_opts |= Optimizations::XmmFaultSink;
        }
        const Arm64Features arm64_features = DetectArm64Features();
        const bool sse_scalar_insert = SSEScalarInsertEnabled(arm64_features);
        const bool sse_afp_nan = SSEAFPNanEnabled(arm64_features);
        const bool mem_hostbase_fold = svm_config.mem_hostbase_fold;
        const bool induct_tie = svm_config.induct_tie;
        const bool region_edges = svm_config.region_edges;
        Config config{
                .loc_start = 0,
                .loc_end = 1ul << 49,
                .enable_jit = enable_jit,
                .has_local_operation = false,
                .backend_isa = swift::runtime::kArm64,
                .uniform_buffer_size = sizeof(ThreadContext64) + kScratchUniformSize,
                .buffers_static_alloc = static_regs,
                .xmm_uniform_ranges = x86_xmm_uniform_ranges,
                .static_program = false,
                // Block linking uses incoming-link-tracked 4-byte direct
                // branches for eligible same-module static exits. Empty or
                // structurally ineligible exits retain the dispatch-slot /
                // dispatcher fallback.
                // FunctionBaseCompile: whole-function decode + compile.
                //   Enabled by default; SVM_FUNC_BASE=0 is the escape hatch.
                //   function-level linear scan now handles terminal uses over an
                //   RPO numbering, and complex/failed functions fall back to
                //   block compilation.
                .global_opts = global_opts,
                .arm64_features = arm64_features,
                .sse_scalar_insert = sse_scalar_insert,
                .sse_afp_nan = sse_afp_nan,
                .mem_hostbase_fold = mem_hostbase_fold,
                .induct_tie = induct_tie,
                .region_edges = region_edges,
                .tso_mode = TsoModeFromEnvironment(),
                .stack_alignment = 16,
                .page_table = nullptr,
                .memory_base = memory_base,
                .guest_addr_mask = guest_addr_mask,
                .memory = &memory_impl,
        };
        // The x86 decoder mode is process-global because decoded IR is shared
        // by every Runtime in this Instance. Install it once before any block
        // can be decoded; the default remains Relaxed.
        x86::SetTsoMode(config.tso_mode);
        address_space = std::make_unique<backend::AddressSpace>(config);
    }

    [[nodiscard]] std::unique_ptr<runtime::Runtime> MakeRuntime() const {
        return std::make_unique<runtime::Runtime>(address_space.get());
    }

    [[nodiscard]] void* Translate(LocationDescriptor pc) const {
        // Coarse first-cut MT policy: only one thread may inspect/mutate the
        // frontend's function fallback sets or decode/publish IR at a time.
        // Already-published JIT code remains concurrently executable.
        std::lock_guard translate_guard(translate_mutex);
        PerfTranslationScope2 perf_detail;
        PerfScope perf_total{GetPerfStats().translate_ns};
        // Another thread may have published this exact location while we
        // waited for the coarse lock. Reuse it instead of compiling a duplicate
        // function whose Module::Push would fail and surface as IllegalCode.
        if (address_space->GetConfig().enable_jit) {
            PerfScope2 perf_lookup{GetPerfStats2().publish_lookup};
            if (auto* published = address_space->GetCodeCache(pc)) {
                return published;
            }
        }
        if (compile_observer) {
            compile_observer(compile_observer_ctx, pc);
        }
        PerfScope2 perf_module_lookup{GetPerfStats2().publish_lookup};
        auto module = address_space->GetModule(pc);
        perf_module_lookup.Stop();
        auto& m_config = module->GetModuleConfig();
        const auto features = backend::ResolveFeatureSet(m_config);
        // Function-level compilation is default-on when the optimization is
        // present; SVM_FUNC_BASE=0 is the explicit block-only escape hatch.
        auto func_base = m_config.HasOpt(runtime::Optimizations::FunctionBaseCompile) &&
                         runtime::GetSvmConfig().func_base &&
                         !function_compilation_disabled &&
                         !block_only_locations.contains(pc);

        // Function-level compilation: decode the whole function (all reachable
        // blocks up to ret / indirect jump / syscall) into an HIRFunction and
        // compile it as a single unit. Bypasses GetNodeOrCreate to avoid the
        // ir::Function identity conflict between the module's address-node map
        // and the HIRBuilder's internal Function object (TranslateIR pushes the
        // HIRBuilder's Function into the module).
        if (func_base) {
            // Best-effort: the whole-function path is a strict subset of what
            // block compilation handles. Any failure (an unsupported construct
            // trips an IR assert during decode/compile) falls back to the
            // known-good block path below instead of crashing the guest.
            void* func_code = nullptr;
            bool compiled = false;
            func_stats.Attempt();
            try {
            auto jit_guard = module->ModuleLockRead();
            PerfScope2 perf_ir_setup{GetPerfStats2().ir_setup};
            ir::HIRBuilder builder{1, true, features};
            auto* hir_func = builder.AppendFunction(pc);
            perf_ir_setup.Stop();

            // The current linear-scan allocator is intentionally conservative
            // and does not yet qualify very large libc CFGs (for example
            // _int_malloc at ~150 blocks). Keep those on the block compiler;
            // ordinary multi-block functions still take the function path.
            constexpr size_t kMaxFuncBlocks = 128;
            // JIT only.  The IR interpreter resolves the next guest location
            // through ir::Function::FindBlock, and HIRFunction::EndFunction
            // hands *every* HIR block to the ir::Function -- including the
            // undecoded ones.  A partial unit therefore gives the interpreter
            // an empty block for the successor, which executes nothing and
            // spins forever (found by run_helper_fault_tests.sh's
            // SVM_ENABLE_JIT=0 shapes, which hung).  The interpreter is a
            // cross-check path where translation cost does not matter, so it
            // keeps eager whole-function decoding.  The deeper fix -- not
            // adding undecoded blocks to the ir::Function at all -- would also
            // shrink SmcTracker::ClearDispatchSlots' walk, but it changes HIR
            // ownership semantics and is left for a separate change.
            const size_t lazy_budget =
                    decode_budget_override != 0
                            ? decode_budget_override
                            : (address_space->GetConfig().enable_jit
                                       ? (address_space->GetConfig().region_edges
                                                  ? 1024
                                                  : LazyFuncBudget())
                                       : kMaxFuncBlocks);
            const size_t decode_cap = std::min(lazy_budget, kMaxFuncBlocks);
            const bool lazy = lazy_budget < kMaxFuncBlocks;
            size_t decoded_count = 0;
            bool hit_block_cap = false;
            PerfScope perf_decode{GetPerfStats().decode_ns};
            while (decoded_count < decode_cap) {
                std::vector<LocationDescriptor> to_decode;
                for (auto& hb : hir_func->GetHIRBlockList()) {
                    auto* blk = hb.GetBlock();
                    // Undecoded block: no instructions AND no terminal. The
                    // synthetic entry block already has a LinkBlock terminal
                    // (and an INVALID start location) — never decode it.
                    if (blk->GetInstList().empty() && !blk->HasTerminal()) {
                        const auto addr = blk->GetStartLocation().Value();
                        // Successor that already has published code (an earlier
                        // region ended here, or it is another function entry).
                        // Re-decoding would emit a second copy of the same guest
                        // block; leaving it undecoded routes the edge through the
                        // L2 slot, which is already filled and branches straight
                        // to the existing code.  Without this, region growth
                        // duplicates work: at budget 4 on func_tests it compiled
                        // 1628 blocks where only 1147 are ever executed.
                        if (lazy && address_space->GetCodeCache(addr)) {
                            continue;
                        }
                        to_decode.push_back(addr);
                    }
                }
                if (to_decode.empty()) {
                    break;
                }
                for (auto addr : to_decode) {
                    if (decoded_count == decode_cap) {
                        hit_block_cap = true;
                        break;
                    }
                    builder.SetCurBlock(addr);
                    ir::Assembler assembler{&builder};
                    x86::X64Decoder decoder{
                            addr,
                            &memory_impl,
                            &assembler,
                            true,
                            address_space->GetConfig().arm64_features,
                            address_space->GetConfig().sse_afp_nan,
                            !address_space->GetConfig().memory_base &&
                                    !address_space->GetConfig().page_table,
                            features};
                    PerfScope2 perf_decode_detail{GetPerfStats2().decode_total};
                    decoder.Decode();
                    ++decoded_count;
                }
            }
            for (auto& hb : hir_func->GetHIRBlockList()) {
                auto* block = hb.GetBlock();
                if (block->GetInstList().empty() && !block->HasTerminal()) {
                    hit_block_cap = true;
                    break;
                }
            }
            PerfScope2 perf_ir_finalize{GetPerfStats2().ir_finalize};
            hir_func->EndFunction();
            perf_ir_finalize.Stop();
            perf_decode.Stop();
            PerfAdd(GetPerfStats().func_units, 1);
            PerfAdd(GetPerfStats().decoded_blocks, decoded_count);

            if (runtime::GetSvmConfig().dump_ir) {
                // stderr: survives the crash that aborts the guest before
                // buffered stdout would flush. EndFunction (ours or the
                // assembler's) clears block_list and fills the blocks vector,
                // so iterate GetHIRBlocks().
                fmt::print(stderr, "--- function {:#x} (decoded {} blocks) ---\n", pc,
                           decoded_count);
                for (auto* hb : hir_func->GetHIRBlocks()) {
                    if (hb) {
                        fmt::print(stderr, "{}\n", hb->GetBlock()->ToString());
                    }
                }
                fmt::print(stderr, "--- end function {:#x} ---\n", pc);
            }

            size_t decoded_blocks = 0;
            bool has_host_call = false;
            for (auto* hb : hir_func->GetHIRBlocks()) {
                if (!hb) {
                    continue;
                }
                auto* blk = hb->GetBlock();
                if (blk->GetInstList().empty()) {
                    continue;  // synthetic entry / undecoded successor
                }
                decoded_blocks++;
                has_host_call |= std::any_of(blk->GetInstList().begin(),
                                             blk->GetInstList().end(),
                                             [](const ir::Inst& inst) {
                                                 return inst.GetOp() == ir::OpCode::CallLambda;
                                             });
            }

            const bool allow_func_lambda = runtime::GetSvmConfig().func_lambda;
            if (has_host_call && !allow_func_lambda) {
                for (auto* hb : hir_func->GetHIRBlocks()) {
                    if (hb && hb != hir_func->GetEntryBlock()) {
                        block_only_locations.insert(hb->GetBlock()->GetStartLocation().Value());
                    }
                }
                throw std::runtime_error(
                        "function contains CallLambda; disabled by SVM_FUNC_LAMBDA=0");
            } else if (hit_block_cap && !lazy) {
                // Once a translation reaches the safety cap, entry at one of
                // its not-yet-discovered interior blocks cannot be identified
                // reliably as the same function. Keep the remainder of this
                // process on block compilation instead of repeatedly decoding
                // overlapping suffixes of the oversized CFG.
                function_compilation_disabled = true;
                for (auto* hb : hir_func->GetHIRBlocks()) {
                    if (hb && hb != hir_func->GetEntryBlock()) {
                        block_only_locations.insert(hb->GetBlock()->GetStartLocation().Value());
                    }
                }
                func_stats.BlockCap(pc, decoded_count);
            } else {
                perf_detail.Classify(static_cast<unsigned>(decoded_blocks));
                if (!module->GetAddressSpace().GetConfig().enable_jit) {
                    if (!backend::PublishIRFunction(module, hir_func)) {
                        throw std::runtime_error("failed to publish interpreted HIR function");
                    }
                    if (runtime::GetSvmConfig().dump_ir) {
                        fmt::print(stderr, "[func-compile] {:#x} interp-publish-ready\n", pc);
                    }
                    func_stats.Compiled(decoded_blocks);
                    if (runtime::GetSvmConfig().dump_ir) {
                        fmt::print(stderr, "[func-compile] {:#x} interp-return\n", pc);
                    }
                    compiled = true;
                } else {
                    func_code = backend::TranslateIR(module, hir_func);
                    if (!func_code) {
                        throw std::runtime_error("TranslateIR(HIRFunction) returned null");
                    }
                    func_stats.Compiled(decoded_blocks);
                    if (runtime::GetSvmConfig().dump_ir) {
                        fmt::print(stderr, "[func-compile] {:#x} jit-return\n", pc);
                    }
                    compiled = true;
                }
            }
            } catch (const std::exception& error) {
                // Unsupported construct in the whole-function path (e.g. a
                // branch the function-mode decode can't lower yet) — fall back
                // to block compilation. The builder is local and the module is
                // untouched until TranslateIR, so nothing leaks; the read lock
                // unwinds with the try scope.
                func_stats.Exception(pc, error.what());
                block_only_locations.insert(pc);
                compiled = false;
            }
            if (runtime::GetSvmConfig().dump_ir) {
                fmt::print(stderr, "[func-compile] {:#x} builder-destroyed\n", pc);
            }
            if (compiled) {
                return func_code;
            }
            // Complex / failed function: fall through to block compilation.
            func_base = false;
        }

        // SMC detachment takes the matching write lock. Cover node lookup /
        // creation as well as publication: otherwise invalidation could
        // detach this node immediately before this thread acquires the read
        // lock, after which it would compile and publish an orphaned node.
        auto module_compile_guard = module->ModuleLockRead();
        // Detect a freshly created node before GetNodeOrCreate: a fresh
        // ir::Block has an UNINITIALIZED jit_cache (runtime bug — it is
        // default-initialized garbage), so IsEmptyBlock()/IsJitCached() on
        // it are meaningless until we clear it.
        const bool fresh = backend::IsEmpty(module->GetNode(pc));
        auto node = module->GetNodeOrCreate(pc, func_base);
        auto code_cache = VisitVariant<void*>(
                node, [module, pc, fresh, &perf_detail, features](auto x) -> void* {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, IntrusivePtr<ir::Function>>) {
                // TODO: function-based compilation
                return nullptr;
            } else if constexpr (std::is_same_v<T, IntrusivePtr<ir::Block>>) {
                auto guard = x->LockWrite();
                if (fresh) {
                    // WORKAROUND (runtime bug): clear the uninitialized
                    // jit_cache so TranslateIR doesn't mistake the new block
                    // for a cached one.
                    std::memset(&x->GetJitCache(), 0, sizeof(backend::JitCache));
                    PerfScope2 perf_ir_setup{GetPerfStats2().ir_setup};
                    ir::Assembler assembler{x.get()};
                    x86::X64Decoder decoder{
                            pc,
                            &memory_impl,
                            &assembler,
                            true,
                            module->GetAddressSpace().GetConfig().arm64_features,
                            module->GetAddressSpace().GetConfig().sse_afp_nan,
                            !module->GetAddressSpace().GetConfig().memory_base &&
                                    !module->GetAddressSpace().GetConfig().page_table,
                            features};
                    perf_ir_setup.Stop();
                    PerfScope2 perf_decode_detail{GetPerfStats2().decode_total};
                    decoder.Decode();
                    perf_decode_detail.Stop();
                    PerfScope2 perf_ir_finalize{GetPerfStats2().ir_finalize};
                    // Root causes fixed in runtime (2026-07-22), workarounds
                    // retired:
                    //  - GetArg<Operand> tolerates a Void right side and the
                    //    backend materializes Imm left sides, so
                    //    FixupConsecutiveFlagOps / FixupFlagFlushBeforeConsumer
                    //    are no longer needed. FixupSingleSidedOperands is
                    //    kept as a harmless normalization pass.
                    //  - MergeNZCV uses replace semantics, so
                    //    FixupFlagFlushBeforeConsumer is retired.
                    //  - RegisterAllocPass accounts for terminal value uses,
                    //    so MaterializeTerminalCondUse is retired.
                    FixupSingleSidedOperands(x.get());
                    PinUnusedCallLambdas(x.get());
                    perf_ir_finalize.Stop();
                    if (runtime::GetSvmConfig().dump_ir) {
                        fmt::print("--- block {:#x} ---\n{}\n", pc, x->ToString());
                    }
                }
                if (!module->GetAddressSpace().GetConfig().enable_jit) {
                    // Interpreter path: leave the decoded block in the module;
                    // Runtime::Impl::Interpreter() picks it up by location.
                    return nullptr;
                }
                perf_detail.Classify(1);
                return backend::TranslateIR(module, x.get());
            } else {
                return nullptr;
            }
        });
        if (code_cache) {
            PerfScope2 perf_l2{GetPerfStats2().publish_l2};
            address_space->PushCodeCache(pc, code_cache);
        }
        return code_cache;
    }

    std::vector<UniformMapDesc> static_regs_storage{};
    std::unique_ptr<backend::AddressSpace> address_space{};
    mutable std::mutex translate_mutex;
    mutable FunctionCompileStats func_stats{"x86_64"};
    mutable std::unordered_set<LocationDescriptor> block_only_locations{};
    mutable bool function_compilation_disabled{};
    // AOT: in-process override of the function-mode decode budget (see
    // X86Instance::SetFunctionDecodeBudget). 0 = follow SVM_FUNC_LAZY.
    std::size_t decode_budget_override{};
    void (*compile_observer)(void*, uint64_t){nullptr};
    void* compile_observer_ctx{};
    // Interpreter wild-pointer guard: wired by the linux loader via
    // X86Instance::SetInterpRangeCheck; forwarded to State by X86Core::Impl.
    bool (*range_check_fn)(void*, u64, u64){nullptr};
    void* range_check_ctx{};
};

struct X86Core::Impl final {
    explicit Impl(X86Instance* instance) : instance(instance) {
        s_runtime = instance->impl->MakeRuntime();
        auto* cpu = GetCPUContext();
        cpu->x87_fcw = 0x037F;
        cpu->x87_fsw = 0;
        cpu->x87_ftw = 0xFFFF;
        // Wire the interpreter wild-pointer guard into the runtime State.
        auto* st = s_runtime->GetState();
        st->interp_range_check = instance->impl->range_check_fn;
        st->interp_range_check_ctx = instance->impl->range_check_ctx;
    }

    [[nodiscard]] void* Translate(LocationDescriptor pc) const {
        return instance->impl->Translate(pc);
    }

    [[nodiscard]] ExitReason Run() {
        // update backend location
        s_runtime->SetLocation(GetCPUContext()->pc.qword);
        const bool trace = runtime::GetSvmConfig().trace;
        auto hr = HaltReason::None;
        while (hr == HaltReason::None) {
            hr = s_runtime->Run();
            // update frontend location
            auto pc = s_runtime->GetLocation();
            GetCPUContext()->pc.qword = pc;
            if (trace) {
                fmt::print("[trace] halt={:#x} rip={:#x} rax={:#x} rbx={:#x} rcx={:#x} rdx={:#x}\n",
                           static_cast<u32>(hr),
                           pc,
                           GetCPUContext()->rax.qword,
                           GetCPUContext()->rbx.qword,
                           GetCPUContext()->rcx.qword,
                           GetCPUContext()->rdx.qword);
            }
            if (True(hr & runtime::HaltReason::CodeMiss)) {
                // No cache, do translate (in IR interpreter mode this only
                // decodes the block into the module and returns nullptr).
                const bool jit = instance->impl->address_space->GetConfig().enable_jit;
                auto* cache = Translate(pc);
                if (jit && !cache) {
                    return ExitReason::IllegalCode;
                }
                // Translation done, resume running the translated code
                hr = HaltReason::None;
                continue;
            }
            if (True(hr & runtime::HaltReason::CallHost)) {
                // The frontend stored the full context into the uniform
                // buffer, set rip to the instruction *after* the trapping
                // one and recorded why it stopped in ctx->interrupt.
                switch (GetCPUContext()->interrupt) {
                    case x86::InterruptReason::SVC:
                        // Guest `syscall`: rax = syscall number,
                        // rdi/rsi/rdx/r10/r8/r9 = args. The loader emulates
                        // the syscall, writes the result to rax and
                        // re-enters Run().
                        svc_num = GetCPUContext()->rax.qword;
                        return ExitReason::Syscall;
                    case x86::InterruptReason::HLT:
                        return ExitReason::None;
                    default:
                        // BRK / ILL_CODE / PAGE_FATAL / FALLBACK
                        return ExitReason::IllegalCode;
                }
            }
        }
        if (True(hr & HaltReason::PageFatal)) {
            return ExitReason::PageFatal;
        } else if (True(hr & HaltReason::Signal)) {
            return ExitReason::Signal;
        } else if (True(hr & HaltReason::IllegalCode)) {
            return ExitReason::IllegalCode;
        } else {
            return ExitReason::None;
        }
    }

    ExitReason Step() { return ExitReason::Step; }

    [[nodiscard]] ThreadContext64* GetCPUContext() const {
        return reinterpret_cast<ThreadContext64*>(s_runtime->GetUniformBuffer().data());
    }

    X86Instance* instance;
    std::unique_ptr<runtime::Runtime> s_runtime{};
    u64 svc_num{};
};

X86Instance::X86Instance(void* memory_base, u64 guest_addr_mask) {
    impl = std::make_unique<Impl>(memory_base, guest_addr_mask);
}

X86Instance* X86Instance::Make(void* memory_base, u64 guest_addr_mask) {
    return new X86Instance(memory_base, guest_addr_mask);
}

void X86Instance::Destroy(X86Instance* instance) {
    delete instance;
}

void X86Instance::InvalidateCodeRange(uint64_t start, uint64_t end) {
    impl->address_space->InvalidateCodeRange(start, end);
}

void X86Instance::SetInterpRangeCheck(bool (*fn)(void*, uint64_t, uint64_t), void* ctx) {
    impl->range_check_fn = fn;
    impl->range_check_ctx = ctx;
}

void X86Instance::PrepareForMultithreading() {
    std::lock_guard guard(impl->translate_mutex);
    auto& smc = impl->address_space->GetSmcTracker();
    if (!runtime::GetSvmConfig().smc_mt) {
        // Diagnostic fallback to the pre-QSBR behavior: MT continues, but
        // translated pages are unprotected and SMC detection is disabled.
        smc.DisableAndUnprotectAll();
    } else {
        smc.EnableMultithreading();
    }
}

// --- AOT pre-compilation (source/aot) -------------------------------------
// Exposed so the AOT compiler can drive the *existing* translation path from
// a symbol table. See translator.h.
void* X86Instance::CompileAt(uint64_t pc) { return impl->Translate(pc); }

void X86Instance::ResetFunctionModeLatch() {
    std::lock_guard guard(impl->translate_mutex);
    impl->function_compilation_disabled = false;
}

runtime::backend::AddressSpace* X86Instance::GetAddressSpace() {
    return impl->address_space.get();
}

void X86Instance::SetFunctionDecodeBudget(std::size_t blocks) {
    std::lock_guard guard(impl->translate_mutex);
    impl->decode_budget_override = blocks;
}

void X86Instance::SetCompileObserver(void (*fn)(void*, uint64_t), void* ctx) {
    std::lock_guard guard(impl->translate_mutex);
    impl->compile_observer = fn;
    impl->compile_observer_ctx = ctx;
}

X86Core::X86Core(X86Instance* instance) : instance(instance) {
    impl = std::make_unique<Impl>(instance);
}

X86Core* X86Core::Make(X86Instance* instance) { return new X86Core(instance); }

void X86Core::Destroy(X86Core* core) {
    delete core;
}

ExitReason X86Core::Run() { return impl->Run(); }

ExitReason X86Core::Step() { return impl->Step(); }

void X86Core::SignalInterrupt() { impl->s_runtime->SignalInterrupt(); }

void X86Core::ClearInterrupt() { impl->s_runtime->ClearInterrupt(); }

uint64_t X86Core::GetSyscallNumber() { return impl->svc_num; }

ThreadContext64& X86Core::GetContext() {
    auto uni_buffer = impl->s_runtime->GetUniformBuffer();
    auto ctx_ptr = reinterpret_cast<ThreadContext64*>(uni_buffer.data());
    return *ctx_ptr;
}

}  // namespace swift::translator::x86
