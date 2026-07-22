//
// Created by 甘尧 on 2024/6/21.
//

#include <cstdlib>
#include <cstring>
#include <memory>
#include "fmt/format.h"
#include "base/scope_exit.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/jit_code.h"
#include "runtime/backend/runtime.h"
#include "runtime/frontend/x86/decoder.h"
#include "runtime/include/sruntime.h"
#include "translator.h"

namespace swift::translator::x86 {

using namespace swift::runtime;
using namespace swift::x86;

// Static guest->host register map for the arm64 backend. Currently UNUSED:
// with this map enabled, guest rdi (and likely other statically mapped
// registers) reads back wrong after ReturnToHost (observed rdi == 0x100
// instead of 1 for `mov edi, 1` in the hello guest), i.e. the spill of
// statically allocated registers into the uniform buffer on the
// ReturnToHost path looks broken in the runtime backend. Re-enable only
// after that is fixed.
[[maybe_unused]] static UniformMapDesc arm64_backend_regs_map[] = {
        {offsetof(ThreadContext64, rdi), 8, 0, false},
        {offsetof(ThreadContext64, rsi), 8, 1, false},
        {offsetof(ThreadContext64, rdx), 8, 2, false},
        {offsetof(ThreadContext64, rcx), 8, 3, false},
        {offsetof(ThreadContext64, r8), 8, 4, false},
        {offsetof(ThreadContext64, r9), 8, 5, false},
        {offsetof(ThreadContext64, rax), 8, 6, false},
        {offsetof(ThreadContext64, rbx), 8, 19, false},
        {offsetof(ThreadContext64, rsp), 8, 20, false},
        {offsetof(ThreadContext64, rbp), 8, 21, false},
        {offsetof(ThreadContext64, r12), 8, 22, false},
        {offsetof(ThreadContext64, r13), 8, 23, false},

        {offsetof(ThreadContext64, xmm0), 16, 16, true},
        {offsetof(ThreadContext64, xmm1), 16, 17, true},
        {offsetof(ThreadContext64, xmm2), 16, 18, true},
        {offsetof(ThreadContext64, xmm3), 16, 19, true},
        {offsetof(ThreadContext64, xmm4), 16, 20, true},
        {offsetof(ThreadContext64, xmm5), 16, 21, true},
        {offsetof(ThreadContext64, xmm6), 16, 22, true},
        {offsetof(ThreadContext64, xmm7), 16, 23, true},
        {offsetof(ThreadContext64, xmm8), 16, 24, true},
        {offsetof(ThreadContext64, xmm9), 16, 25, true},
        {offsetof(ThreadContext64, xmm10), 16, 26, true},
        {offsetof(ThreadContext64, xmm11), 16, 27, true},
        {offsetof(ThreadContext64, xmm12), 16, 28, true},
        {offsetof(ThreadContext64, xmm13), 16, 29, true},
        {offsetof(ThreadContext64, xmm14), 16, 30, true},
        {offsetof(ThreadContext64, xmm15), 16, 31, true},
};

class MemoryImpl : public runtime::MemoryInterface {
public:
    bool Read(void* dest, size_t addr, size_t size) override {
        return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
    }
    bool Write(void* src, size_t addr, size_t size) override {
        return std::memcpy(reinterpret_cast<void*>(addr), src, size);
    }
    void* GetPointer(void* src) override { return src; }
};

static MemoryImpl memory_impl{};

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

struct X86Instance::Impl final {
    Impl() {
        // SVM_ENABLE_JIT=0 forces the IR interpreter path (same switch as the
        // arm64 core; useful for cross-checking JIT results).
        const char* jit_env = std::getenv("SVM_ENABLE_JIT");
        const bool enable_jit = jit_env ? std::strcmp(jit_env, "0") != 0 : true;
        Config config{
                .loc_start = 0,
                .loc_end = 1ul << 49,
                .enable_jit = enable_jit,
                .has_local_operation = false,
                .backend_isa = swift::runtime::kArm64,
                .uniform_buffer_size = sizeof(ThreadContext64) + kScratchUniformSize,
                // No static host-register allocation of guest registers:
                // the whole ThreadContext64 lives in the uniform buffer and
                // is loaded/stored via IR uniform accesses. (The
                // arm64_backend_regs_map static allocation above currently
                // loses guest register values across ReturnToHost — see
                // arm64_backend_regs_map comment.)
                .buffers_static_alloc = {},
                .static_program = false,
                // NOTE: Optimizations::BlockLink is intentionally left
                // out: the indirect-link path in the arm64 backend
                // (JitContext::Forward: "Ldr ip, [cache, ip, LSL 3]; Br ip")
                // jumps via the dispatch table even when the target block
                // has never been translated (empty slot -> br 0x0), so any
                // backward branch to a not-yet-compiled block crashes.
                .global_opts = Optimizations::ReturnStackBuffer | Optimizations::FlagElimination |
                               Optimizations::DeadCodeRemove |
                               Optimizations::StaticCode |
                               Optimizations::ConstantFolding,
                .arm64_features = Arm64Features::None,
                .stack_alignment = 16,
                .page_table = nullptr,
                .memory_base = nullptr,
                .memory = &memory_impl,
        };
        address_space = std::make_unique<backend::AddressSpace>(config);
    }

    [[nodiscard]] std::unique_ptr<runtime::Runtime> MakeRuntime() const {
        return std::make_unique<runtime::Runtime>(address_space.get());
    }

    [[nodiscard]] void* Translate(LocationDescriptor pc) const {
        auto module = address_space->GetModule(pc);
        auto& m_config = module->GetModuleConfig();
        auto func_base = m_config.HasOpt(runtime::Optimizations::FunctionBaseCompile);
        // Detect a freshly created node before GetNodeOrCreate: a fresh
        // ir::Block has an UNINITIALIZED jit_cache (runtime bug — it is
        // default-initialized garbage), so IsEmptyBlock()/IsJitCached() on
        // it are meaningless until we clear it.
        const bool fresh = backend::IsEmpty(module->GetNode(pc));
        auto node = module->GetNodeOrCreate(pc, func_base);
        auto code_cache = VisitVariant<void*>(node, [module, pc, fresh](auto x) -> void* {
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
                    auto jit_guard = module->ModuleLockRead();
                    ir::Assembler assembler{x.get()};
                    x86::X64Decoder decoder{pc, &memory_impl, &assembler, true};
                    decoder.Decode();
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
                    if (std::getenv("SVM_DUMP_IR")) {
                        fmt::print("--- block {:#x} ---\n{}\n", pc, x->ToString());
                    }
                }
                if (!module->GetAddressSpace().GetConfig().enable_jit) {
                    // Interpreter path: leave the decoded block in the module;
                    // Runtime::Impl::Interpreter() picks it up by location.
                    return nullptr;
                }
                return backend::TranslateIR(module, x.get());
            } else {
                return nullptr;
            }
        });
        if (code_cache) {
            address_space->PushCodeCache(pc, code_cache);
        }
        return code_cache;
    }

    std::unique_ptr<backend::AddressSpace> address_space{};
};

struct X86Core::Impl final {
    explicit Impl(X86Instance* instance) : instance(instance) {
        s_runtime = instance->impl->MakeRuntime();
    }

    [[nodiscard]] void* Translate(LocationDescriptor pc) const {
        return instance->impl->Translate(pc);
    }

    [[nodiscard]] ExitReason Run() {
        // update backend location
        s_runtime->SetLocation(GetCPUContext()->pc.qword);
        const bool trace = std::getenv("SVM_TRACE") != nullptr;
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

X86Instance::X86Instance() { impl = std::make_unique<Impl>(); }

X86Instance* X86Instance::Make() { return new X86Instance(); }

void X86Instance::Destroy(X86Instance* instance) {
    delete instance;
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
