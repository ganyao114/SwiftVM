//
// Created by 甘尧 on 2024/2/23.
//

#include <cstdlib>
#include <cstring>
#include <memory>
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/jit_code.h"
#include "runtime/backend/runtime.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/include/sruntime.h"
#include "translator.h"

namespace swift::translator::arm64 {

using namespace swift::runtime;
using namespace swift::arm64;

/*
 * Guest memory model: with guest address virtualization (Config::memory_base)
 * the guest runs at its linked addresses while the host backing store sits at
 * guest + bias. The decoder's instruction fetch therefore applies the same
 * bias (installed by the linux loader via SetBias; 0 = identity, the default
 * for tests / non-loader embedders).
 */
class MemoryImpl : public runtime::MemoryInterface {
public:
    void SetBias(u64 b) { bias = b; }
    bool Read(void* dest, size_t addr, size_t size) override {
        return std::memcpy(dest, reinterpret_cast<const void*>(addr + bias), size);
    }
    bool Write(void* src, size_t addr, size_t size) override {
        return std::memcpy(reinterpret_cast<void*>(addr + bias), src, size);
    }
    void* GetPointer(void* src) override {
        return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(src) + bias);
    }
    u64 bias{};
};

static MemoryImpl memory_impl{};

struct Arm64Instance::Impl final {
    // memory_base: guest->host bias (host addr = guest addr + bias), installed
    // by the linux loader; nullptr keeps the identity-mapped fast path.
    explicit Impl(void* memory_base) {
        memory_impl.SetBias(reinterpret_cast<uintptr_t>(memory_base));
        // SVM_ENABLE_JIT=0 forces the IR interpreter path (bring-up aid
        // while the JIT is under development).
        const char* jit_env = std::getenv("SVM_ENABLE_JIT");
        const bool enable_jit = jit_env ? std::strcmp(jit_env, "0") != 0 : true;
        Config config{
                .loc_start = 0,
                .loc_end = 1ull << 48,
                .enable_jit = enable_jit,
                .has_local_operation = false,
                .backend_isa = swift::runtime::kArm64,
                .uniform_buffer_size = sizeof(ThreadContext64),
                // No static host-register allocation of guest registers:
                // the whole ThreadContext64 lives in the uniform buffer and
                // is loaded/stored via IR uniform accesses.
                .buffers_static_alloc = {},
                .static_program = false,
                // Block linking enabled: empty dispatch slots fall back to the
                // dispatcher safely (see JitContext::Forward).
                // ReturnStackBuffer: call/ret pairs skip the dispatcher on
                //   a correct RSB prediction (JitContext::EmitRSBPush/Pop).
                // DirectBlockLink: known targets branch via Mov+Br (SMC-safe,
                //   no backpatching); unknown targets fall back to the
                //   indirect dispatch table.
                // FunctionBaseCompile: whole-function decode + compile
                //   (TranslateIR(HIRFunction*) path). The function-level linear
                //   scan now accounts for terminal uses and runs over an RPO
                //   numbering (ComputeRPO/IdByRPO in TranslateIR), fixing the
                //   bad multi-block allocations. It is opt-in at runtime via
                //   SVM_FUNC_BASE=1 (see Translate()); complex functions fall
                //   back to block compilation.
                .global_opts = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove |
                               Optimizations::BlockLink | Optimizations::DirectBlockLink |
                               Optimizations::ReturnStackBuffer | Optimizations::FunctionBaseCompile,
                .arm64_features = Arm64Features::None,
                .stack_alignment = 16,
                .page_table = nullptr,
                .memory_base = memory_base,
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
        // Function-level compilation is opt-in: the opt must be set AND
        // SVM_FUNC_BASE=1. Default stays block compilation so the known-good
        // paths are untouched; simple functions opt into the whole-function
        // path for validation.
        const char* fb_env = std::getenv("SVM_FUNC_BASE");
        auto func_base = m_config.HasOpt(runtime::Optimizations::FunctionBaseCompile) &&
                         fb_env && std::strcmp(fb_env, "0") != 0;

        // Function-level compilation: decode the whole function (all
        // reachable blocks up to ret / indirect jump) into an HIRFunction
        // and compile it as a single unit.  Bypasses GetNodeOrCreate to
        // avoid the ir::Function identity conflict between the module's
        // address-node map and the HIRBuilder's internal Function object
        // (TranslateIR pushes the HIRBuilder's Function into the module).
        if (func_base) {
            // Best-effort: any failure in the whole-function path (an IR assert
            // during decode/compile of an unsupported construct) falls back to
            // the known-good block path below instead of crashing the guest.
            void* func_code = nullptr;
            bool compiled = false;
            try {
            auto jit_guard = module->ModuleLockRead();
            ir::HIRBuilder builder{};
            auto* hir_func = builder.AppendFunction(pc);
            ir::Assembler assembler{&builder};

            constexpr size_t kMaxFuncBlocks = 256;
            size_t decoded_count = 0;
            bool progress = true;
            bool function_ended = false;
            while (progress && decoded_count < kMaxFuncBlocks && !function_ended) {
                progress = false;
                std::vector<LocationDescriptor> to_decode;
                for (auto& hb : hir_func->GetHIRBlockList()) {
                    auto* blk = hb.GetBlock();
                    // An undecoded block has neither instructions nor a
                    // terminal. The synthetic entry block already carries a
                    // LinkBlock terminal (and an INVALID start location) — it
                    // must never be decoded, so require !HasTerminal().
                    if (blk->GetInstList().empty() && !blk->HasTerminal()) {
                        to_decode.push_back(blk->GetStartLocation().Value());
                    }
                }
                for (auto addr : to_decode) {
                    builder.SetCurBlock(addr);
                    arm64::A64Decoder decoder{addr, &memory_impl, &assembler};
                    decoder.Decode();
                    ++decoded_count;
                    progress = true;
                    // A function-ending terminal (ret / svc) calls EndFunction
                    // through the assembler and clears the current function —
                    // stop decoding; the graph is already finalized.
                    if (!builder.HasCurrentFunction()) {
                        function_ended = true;
                        break;
                    }
                }
            }
            // Finalize the function graph (predecessors/successors, blocks
            // vector) unless a ret/svc terminal already did. Do NOT call
            // builder.End(): it appends a spurious ReturnToDispatch terminal.
            if (!function_ended) {
                hir_func->EndFunction();
            }

            // Complexity gate (see the x86 driver for the full rationale): only
            // single-block, host-call-free functions take the whole-function
            // path; anything else falls back to block compilation below.
            bool too_complex = false;
            size_t decoded_blocks = 0;
            for (auto* hb : hir_func->GetHIRBlocks()) {
                if (!hb) {
                    continue;
                }
                auto* blk = hb->GetBlock();
                if (blk->GetInstList().empty()) {
                    continue;  // synthetic entry / undecoded successor
                }
                decoded_blocks++;
                for (auto& inst : blk->GetInstList()) {
                    if (inst.GetOp() == ir::OpCode::CallLambda) {
                        too_complex = true;
                        break;
                    }
                }
            }
            if (decoded_blocks > 1) {
                too_complex = true;
            }

            if (!too_complex) {
                if (!module->GetAddressSpace().GetConfig().enable_jit) {
                    return nullptr;
                }
                func_code = backend::TranslateIR(module, hir_func);
                if (func_code) {
                    address_space->PushCodeCache(pc, func_code);
                }
                compiled = true;
            }
            } catch (const std::exception&) {
                // Unsupported construct in the whole-function path — fall back
                // to block compilation (see the x86 driver for details).
                compiled = false;
            }
            if (compiled) {
                return func_code;
            }
            // Complex / failed function: fall through to block compilation.
        }

        // Block-level compilation path.
        const bool fresh = backend::IsEmpty(module->GetNode(pc));
        auto node = module->GetNodeOrCreate(pc, false);
        auto code_cache = VisitVariant<void*>(node, [module, pc, fresh](auto x) -> void* {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, IntrusivePtr<ir::Function>>) {
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
                    arm64::A64Decoder decoder{pc, &memory_impl, &assembler};
                    decoder.Decode();
                }
                if (!module->GetAddressSpace().GetConfig().enable_jit) {
                    // IR interpreter mode: the decoded block stays in the
                    // module, Runtime::Run() picks it up via Interpreter().
                    return nullptr;
                }
                return backend::TranslateIR(module, x);
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
    // Interpreter wild-pointer guard: wired by the linux loader via
    // Arm64Instance::SetInterpRangeCheck; forwarded to State by Arm64Core::Impl.
    bool (*range_check_fn)(void*, u64, u64){nullptr};
    void* range_check_ctx{};
};

struct Arm64Core::Impl final {
    explicit Impl(Arm64Instance* instance) : instance(instance) {
        s_runtime = instance->impl->MakeRuntime();
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
        s_runtime->SetLocation(GetCPUContext()->pc);
        auto hr = HaltReason::None;
        while (hr == HaltReason::None) {
            hr = s_runtime->Run();
            // update frontend location
            auto pc = s_runtime->GetLocation();
            GetCPUContext()->pc = pc;
            if (True(hr & runtime::HaltReason::CodeMiss)) {
                // No cache, do translate (in IR interpreter mode this only
                // decodes the block into the module).
                const bool jit = instance->impl->address_space->GetConfig().enable_jit;
                auto* cache = Translate(pc);
                if (jit && !cache) {
                    return ExitReason::IllegalCode;
                }
                hr = HaltReason::None;
                continue;
            }
            if (True(hr & runtime::HaltReason::CallHost)) {
                // Guest `svc #0`: the frontend stored the full context into
                // the uniform buffer; x8 = syscall number, x0-x5 = args,
                // pc = next instruction. The loader emulates the syscall,
                // writes the result to x0 and re-enters Run().
                svc_num = GetCPUContext()->r[8];
                return ExitReason::Syscall;
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

    Arm64Instance* instance{};
    std::shared_ptr<runtime::Runtime> s_runtime{};
    u64 svc_num{};
};

Arm64Instance::Arm64Instance(void* memory_base) { impl = std::make_unique<Impl>(memory_base); }

Arm64Instance* Arm64Instance::Make(void* memory_base) { return new Arm64Instance(memory_base); }

void Arm64Instance::Destroy(Arm64Instance* instance) { delete instance; }

void Arm64Instance::InvalidateCodeRange(uint64_t start, uint64_t end) {
    impl->address_space->InvalidateCodeRange(start, end);
}

void Arm64Instance::SetInterpRangeCheck(bool (*fn)(void*, uint64_t, uint64_t), void* ctx) {
    impl->range_check_fn = fn;
    impl->range_check_ctx = ctx;
}

Arm64Core::Arm64Core(Arm64Instance* instance) : instance(instance) {
    impl = std::make_unique<Impl>(instance);
}

Arm64Core* Arm64Core::Make(Arm64Instance* instance) { return new Arm64Core(instance); }

void Arm64Core::Destroy(Arm64Core* core) { delete core; }

ExitReason Arm64Core::Run() { return impl->Run(); }

ExitReason Arm64Core::Step() { return impl->Step(); }

void Arm64Core::SignalInterrupt() { impl->s_runtime->SignalInterrupt(); }

void Arm64Core::ClearInterrupt() { impl->s_runtime->ClearInterrupt(); }

uint64_t Arm64Core::GetSyscallNumber() { return impl->svc_num; }

ThreadContext64& Arm64Core::GetContext() {
    auto uni_buffer = impl->s_runtime->GetUniformBuffer();
    auto ctx_ptr = reinterpret_cast<ThreadContext64*>(uni_buffer.data());
    return *ctx_ptr;
}

}  // namespace swift::translator::arm64
