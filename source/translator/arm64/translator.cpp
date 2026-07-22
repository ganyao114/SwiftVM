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
 * Guest memory model: the backend JIT (and interpreter) use guest virtual
 * addresses directly as host pointers (config.page_table / memory_base are
 * null), so the guest address space is identity-mapped into the host
 * process. The loader (translator/linux) is responsible for placing the
 * guest image / stack at their guest addresses with host mmap; instruction
 * fetch for the decoder therefore degrades to a plain memcpy.
 */
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

struct Arm64Instance::Impl final {
    Impl() {
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
                .global_opts = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove,
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
};

struct Arm64Core::Impl final {
    explicit Impl(Arm64Instance* instance) : instance(instance) {
        s_runtime = instance->impl->MakeRuntime();
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

Arm64Instance::Arm64Instance() { impl = std::make_unique<Impl>(); }

Arm64Instance* Arm64Instance::Make() { return new Arm64Instance(); }

void Arm64Instance::Destroy(Arm64Instance* instance) { delete instance; }

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
