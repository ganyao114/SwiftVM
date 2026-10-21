#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include "base/scope_exit.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/runtime.h"
#include "runtime/ir/hir_builder.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

extern "C" HaltReason SwiftRsbSeedInvoke(State*, void*, void*, u64, u64*);
asm(".text\n"
    ".global SwiftRsbSeedInvoke\n"
    ".type SwiftRsbSeedInvoke,%function\n"
    "SwiftRsbSeedInvoke:\n"
    "stp x25,x26,[sp,#-32]!\n"
    "stp x29,x30,[sp,#16]\n"
    "mov x25,x3\n"
    "mov x26,x4\n"
    "blr x2\n"
    "str x25,[x26]\n"
    "ldp x29,x30,[sp,#16]\n"
    "ldp x25,x26,[sp],#32\n"
    "ret\n"
    ".size SwiftRsbSeedInvoke,.-SwiftRsbSeedInvoke\n");

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view mode{argv[1]};
        if (mode != "body") return 2;
        const size_t size = 8 * static_cast<size_t>(getpagesize());
        void* guest = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (guest == MAP_FAILED) throw std::runtime_error("guest mmap");
        Config config{};
        config.loc_end = size;
        config.memory_base = guest;
        config.guest_addr_mask = size - 1;
        config.backend_isa = kArm64;
        config.enable_jit = true;
        config.uniform_buffer_size = 32;
        config.stack_alignment = 16;
        {
            AddressSpace space{config};
            auto module = space.GetDefaultModule();
            HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
            auto* function = builder.AppendFunction(Location{0x100}, Location{0x104});
            function->StoreUniform(Uniform{0, ValueType::U64},
                    function->LoadImm(Imm{u64{37}}).SetType(ValueType::U64));
            function->EndBlock(terminal::ReturnToHost{});
            function->EndFunction();
            void* entry = TranslateIR(module, function);
            if (!entry) throw std::runtime_error("compile native body");
            Runtime runtime{&space};
            runtime.SetLocation(0x100);
            auto* state = runtime.GetState();
            std::array<RSBFrame, 4> disabled_frames{};
            auto* const before = disabled_frames.data() + 2;
            state->rsb_pointer = state->rsb_empty = before;
            TranslateTable l1{8};
            auto& smc = space.GetSmcTracker();
            const auto token = smc.RegisterRuntime(l1);
            const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
            RetirementControl::Lease admission{space.GetRetirementControl(), RetirementControl::Kind::Execution};
            SmcTracker::ExecutionScope active{smc, token};
            constexpr u64 seed = 0x13579bdf2468ace0ull;
            u64 restored{};
            const auto reason = SwiftRsbSeedInvoke(state, entry,
                    reinterpret_cast<void*>(space.GetTrampolines().GetRuntimeEntry()), seed, &restored);
            u64 value{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
            std::printf("mode=%s halt=%u before=%p after=%p host_seed=%llx restored=%llx value=%llu\n", argv[1],
                    static_cast<unsigned>(reason), static_cast<void*>(before), static_cast<void*>(state->rsb_pointer),
                    static_cast<unsigned long long>(seed), static_cast<unsigned long long>(restored),
                    static_cast<unsigned long long>(value));
            if (reason != HaltReason::CallHost || restored != seed || value != 37)
                throw std::runtime_error("native halt/body or host callee-save contract");
            if (state->rsb_pointer != before) throw std::runtime_error("disabled RSB overwrote guest pointer with host x25");
            state->rsb_pointer = state->rsb_empty = nullptr;
            state->exit_request = 0;
            active.End();
        }
        munmap(guest, size);
        std::printf("mode=%s checks=4 passed\n", argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "failure: %s\n", error.what());
        return 1;
    }
}
