//
// Created by 甘尧 on 2024/6/21.
//

#include <memory>
#include "base/scope_exit.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/runtime.h"
#include "runtime/frontend/x86/decoder.h"
#include "runtime/include/sruntime.h"
#include "translator.h"

namespace swift::translator::x86 {

using namespace swift::runtime;
using namespace swift::x86;

static UniformMapDesc arm64_backend_regs_map[] = {
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

struct X86Instance::Impl final {
    Impl() {
        Config config{
                .loc_start = 0,
                .loc_end = 1ul << 49,
                .enable_jit = true,
                .has_local_operation = false,
                .backend_isa = swift::runtime::kArm64,
                .uniform_buffer_size = sizeof(ThreadContext64),
                .buffers_static_alloc = {arm64_backend_regs_map,
                                         sizeof(arm64_backend_regs_map) / sizeof(UniformMapDesc)},
                .static_program = false,
                .global_opts = Optimizations::ReturnStackBuffer | Optimizations::FlagElimination |
                               Optimizations::UniformElimination | Optimizations::DeadCodeRemove |
                               Optimizations::StaticCode | Optimizations::BlockLink |
                               Optimizations::ConstantFolding,
                .arm64_features = Arm64Features::None,
                .stack_alignment = 8,
                .page_table = nullptr,
                .memory_base = nullptr,
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
        auto node = module->GetNodeOrCreate(pc, func_base);
        auto code_cache = VisitVariant<void*>(node, [module, pc](auto x) -> auto {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, IntrusivePtr<ir::Function>>) {
                auto guard = x->LockWrite();
                // TODO
                PANIC();
                return nullptr;
            } else if constexpr (std::is_same_v<T, IntrusivePtr<ir::Block>>) {
                auto guard = x->LockWrite();
                if (x->IsEmptyBlock()) {
                    // Do
                    ir::Assembler assembler{x.get()};
                    x86::X64Decoder decoder{pc, &memory_impl, &assembler, true};
                    decoder.Decode();
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

    [[nodiscard]] ExitReason Run() const {
        x86::ToHost(s_runtime->GetState(), GetCPUContext());
        SCOPE_EXIT({ x86::FromHost(s_runtime->GetState(), GetCPUContext()); });
        auto hr = HaltReason::None;
        while (hr == HaltReason::None) {
            hr = s_runtime->Run();
            // update frontend location
            auto pc = s_runtime->GetLocation();
            GetCPUContext()->pc.qword = pc;
            if (True(hr & runtime::HaltReason::CodeMiss)) {
                // No cache, do translate
                if (!Translate(pc)) {
                    return ExitReason::IllegalCode;
                }
            }
        }
        if (hr == HaltReason::PageFatal) {
            return ExitReason::PageFatal;
        } else if (hr == HaltReason::Signal) {
            return ExitReason::Signal;
        } else if (hr == HaltReason::IllegalCode) {
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
