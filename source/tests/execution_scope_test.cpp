#include <atomic>
#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <stdexcept>
#include <thread>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/ir/hir_builder.h"

namespace {
std::atomic_bool deny_new{false};
}

void* operator new(std::size_t size) {
    if (deny_new.load(std::memory_order_relaxed)) _Exit(92);
    if (auto* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;

unsigned checks{};

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}

constexpr u64 kFutureEpoch = SmcTracker::kInactiveEpoch - 1;
struct HostFailure {};

void Epochs() {
    for (bool multithreaded : {false, true}) {
        SmcTracker tracker{0};
        if (multithreaded) tracker.EnableMultithreading();
        TranslateTable l1{8};
        auto token = tracker.RegisterRuntime(l1);
        Check(tracker.CanReclaimAtEpochForTest(kFutureEpoch), "initially quiescent");
        {
            SmcTracker::ExecutionScope outer{tracker, token};
            const auto epoch = token->active_epoch.load();
            Check(epoch != SmcTracker::kInactiveEpoch, "single/MT entry is active");
            Check(!tracker.CanReclaimAtEpochForTest(kFutureEpoch), "active entry blocks grace");
            {
                SmcTracker::ExecutionScope inner{tracker, token};
                inner.End();
                inner.End();
            }
            Check(token->active_epoch.load() == epoch, "nested end preserves outer epoch");
            tracker.DisableAndUnprotectAll();
            Check(!tracker.CanReclaimAtEpochForTest(kFutureEpoch), "SMC disable preserves lifetime");
            (void)tracker.AdvanceCodePatchEpoch();
        }
        Check(tracker.CanReclaimAtEpochForTest(kFutureEpoch), "scope destruction leaves epoch");
        const auto syncs = token->patch_sync_count.load();
        try {
            SmcTracker::ExecutionScope throwing{tracker, token};
            Check(token->patch_sync_count.load() == syncs + 1, "single/MT patch synchronization");
            throw HostFailure{};
        } catch (const HostFailure&) {
        }
        Check(tracker.CanReclaimAtEpochForTest(kFutureEpoch), "host exception leaves epoch");
        {
            SmcTracker::ExecutionScope retry{tracker, token};
            Check(!tracker.CanReclaimAtEpochForTest(kFutureEpoch), "retry is protected");
            retry.End();
        }
        tracker.UnregisterRuntime(token);
    }
}

Config TestConfig() {
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = u64{1} << 48;
    config.enable_jit = true;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
#if defined(__riscv) && __riscv_xlen == 64
    config.backend_isa = kRiscv64;
#else
    config.backend_isa = kArm64;
#endif
    return config;
}

struct CallbackContext {
    AddressSpace* space;
    Runtime* outer;
    Runtime* inner;
    bool failed{};
};
thread_local CallbackContext* context{};

u64 InnerCallback() noexcept {
    try {
        Check(!context->space->GetSmcTracker().CanReclaimAtEpochForTest(kFutureEpoch),
              "nested generated frame remains protected");
        Check(context->outer->Run() == HaltReason::RuntimeBusy,
              "same Runtime recursion is rejected before state access");
        return 42;
    } catch (...) {
        context->failed = true;
        return 0;
    }
}

u64 OuterCallback() noexcept {
    try {
        auto& tracker = context->space->GetSmcTracker();
        Check(!tracker.CanReclaimAtEpochForTest(kFutureEpoch), "helper return address is protected");
        Check(context->outer->Run() == HaltReason::RuntimeBusy, "same Runtime reentry is rejected");
        HaltReason concurrent = HaltReason::None;
        // Only Run is called from the contender; mutable State stays with its owner.
        auto* outer = context->outer;
        std::thread contender{[&] { concurrent = outer->Run(); }};
        contender.join();
        Check(concurrent == HaltReason::RuntimeBusy, "concurrent Runtime entry is rejected");
        context->inner->SetLocation(0x1100);
        Check(context->inner->Run() == HaltReason::CallHost, "different Runtime nesting is allowed");
        Check(!tracker.CanReclaimAtEpochForTest(kFutureEpoch), "inner return preserves outer participant");
        return 84;
    } catch (...) {
        context->failed = true;
        return 0;
    }
}

void PublishCallback(AddressSpace& space, u64 guest, u64 (*callback)(), bool fault = false) {
    HIRBuilder builder{1, true, ResolveFeatureSet(ModuleConfig{})};
    auto* function = builder.AppendFunction(Location{guest}, Location{guest + 4});
    const auto address = static_cast<u64>(reinterpret_cast<uintptr_t>(FptrCast(callback)));
    auto value = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{address}});
    function->StoreUniform(Uniform{0, ValueType::U64}, value);
    if (fault) {
        auto pointer = function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{24, ValueType::U64});
        auto bad = function->LoadMemory<TypedValue<ValueType::U64>>(Operand{pointer});
        function->StoreUniform(Uniform{8, ValueType::U64}, bad);
    }
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    Check(TranslateIR(space.GetDefaultModule(), function) != nullptr, "compile callback fixture");
}

void RuntimeScopes() {
    struct FaultPage {
        const std::size_t size = static_cast<std::size_t>(getpagesize());
        void* const data = mmap(nullptr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
        ~FaultPage() { if (data != MAP_FAILED) munmap(data, size); }
    } page;
    Check(page.data != MAP_FAILED, "allocate protected fault page");
    AddressSpace space{TestConfig()};
    Runtime outer{&space};
    Runtime inner{&space};
    const auto pointer = reinterpret_cast<uintptr_t>(page.data);
    std::memcpy(outer.GetUniformBuffer().data() + 24, &pointer, sizeof(pointer));
    PublishCallback(space, 0x1000, OuterCallback);
    PublishCallback(space, 0x1100, InnerCallback);
    PublishCallback(space, 0x1200, OuterCallback, true);
    CallbackContext callbacks{&space, &outer, &inner};
    context = &callbacks;
    for (unsigned i = 0; i < 2; ++i) {
        outer.SetLocation(0x1000);
        Check(outer.Run() == HaltReason::CallHost, "normal generated return");
        Check(!callbacks.failed, "callbacks catch every helper exception");
        u64 value{};
        std::memcpy(&value, outer.GetUniformBuffer().data(), sizeof(value));
        Check(value == 84, "outer result survives nested callback");
        Check(space.GetSmcTracker().CanReclaimAtEpochForTest(kFutureEpoch), "all frames returned");
    }
    outer.SetLocation(0x1200);
    const auto fault_reason = outer.Run();
    if (fault_reason != HaltReason::PageFatal)
        std::fprintf(stderr, "nested fault reason=%#x location=%#llx\n",
                     static_cast<unsigned>(fault_reason),
                     static_cast<unsigned long long>(outer.GetLocation()));
    Check(fault_reason == HaltReason::PageFatal, "fault after nesting returns through outer boundary");
    Check(!callbacks.failed, "faulting callback completed normally");
    Check(space.GetSmcTracker().CanReclaimAtEpochForTest(kFutureEpoch), "fault epilogue leaves epoch");
    outer.SetLocation(0x1000);
    Check(outer.Run() == HaltReason::CallHost, "Runtime owner guard releases after fault");
    Check(!callbacks.failed, "post-fault callback completed normally");
    context = nullptr;
}

void RetiredCode() {
    AddressSpace space{TestConfig()};
    auto module = space.GetDefaultModule();
    HIRBuilder builder{1, true, ResolveFeatureSet(ModuleConfig{})};
    auto* function = builder.AppendFunction(Location{0x1300}, Location{0x1304});
    auto value = function->LoadImm<TypedValue<ValueType::U64>>(Imm{u64{17}});
    function->StoreUniform(Uniform{0, ValueType::U64}, value);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    auto* code = static_cast<u8*>(TranslateIR(module, function));
    Check(code != nullptr, "compile retirement fixture");
    module->AddFaultEntry(code, code + 4, 0x1300, code);
    TranslateTable l1{8};
    auto& tracker = space.GetSmcTracker();
    auto token = tracker.RegisterRuntime(l1);
    FaultEntry entry{};
    try {
        SmcTracker::ExecutionScope active{tracker, token};
        Check(tracker.RetireNode(space, nullptr, module, function->GetFunction()), "retire live code");
        Check(space.GetCodeCache(Location{0x1300}) == nullptr, "retired code loses dispatch visibility");
        Check(space.LookupFault(code, entry), "fault metadata survives active frame");
        deny_new.store(true, std::memory_order_relaxed);
        throw HostFailure{};
    } catch (const HostFailure&) {
        deny_new.store(false, std::memory_order_relaxed);
    }
    Check(tracker.CanReclaimAtEpochForTest(kFutureEpoch), "exception leaves executor inactive");
    Check(space.LookupFault(code, entry), "exception defers pending reclaim without allocation");
    {
        SmcTracker::ExecutionScope drain{tracker, token};
        drain.End();
    }
    Check(!space.LookupFault(code, entry), "fault metadata reclaims after last frame");
    tracker.UnregisterRuntime(token);
}

void ActiveUnregister() {
    const pid_t child = fork();
    Check(child >= 0, "fork unregister fixture");
    if (child == 0) {
        std::set_terminate([] { _Exit(90); });
        SmcTracker tracker{0};
        TranslateTable l1{8};
        auto token = tracker.RegisterRuntime(l1);
        SmcTracker::ExecutionScope active{tracker, token};
        tracker.UnregisterRuntime(token);
        _Exit(91);
    }
    int status{};
    Check(waitpid(child, &status, 0) == child, "join unregister fixture");
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 90, "active unregister cannot forge quiescence");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected one execution-scope mode");
        SmcTracker::SetEnabled(false);
        if (std::strcmp(argv[1], "epochs") == 0) Epochs();
        else if (std::strcmp(argv[1], "runtime") == 0) RuntimeScopes();
        else if (std::strcmp(argv[1], "retired") == 0) RetiredCode();
        else if (std::strcmp(argv[1], "unregister") == 0) ActiveUnregister();
        else throw std::runtime_error("unknown execution-scope mode");
        std::printf("mode=%s checks=%u passed\n", argv[1], checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "execution-scope failure: %s\n", error.what());
        return 1;
    }
}
