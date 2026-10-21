#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/backend/context.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/translate_table.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/hir_builder.h"

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
constexpr u64 source_pc = 0x1000, target_pc = 0x2000, return_pc = 0x1100;
unsigned checks{};
std::atomic<u64> source_count{}, target_count{};
std::atomic<unsigned> phase{};
Runtime* active_runtime{};
std::shared_ptr<RetirementControl> control;
RetirementControl::Ticket callback_ticket;
RetirementControl::DrainStatus callback_status{};

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    ++checks;
}

u64 SourceMarker() {
    const auto count = source_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (phase.load(std::memory_order_relaxed) == 1 && count >= 64)
        active_runtime->SignalInterrupt();
    if (phase.load(std::memory_order_relaxed) == 2 && count == 1) {
        callback_ticket = RetirementControl::RequestDrain(control);
        callback_status = control->CheckDrain(callback_ticket);
    }
    return count;
}

u64 TargetMarker() { return target_count.fetch_add(1, std::memory_order_relaxed) + 1; }

template<class Predicate> bool Await(Predicate ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

void WriteFlags(HIRFunction* function) {
    const auto left = function->LoadImm(Imm{u64{~u64{0}}}).SetType(ValueType::U64);
    const auto one = function->LoadImm(Imm{u64{1}}).SetType(ValueType::U64);
    const auto result = function->Add(left, Operand{one}).SetType(ValueType::U64);
    function->SaveFlags(result, Flags::All);
    function->StoreUniform(Uniform{40, ValueType::U64}, result);
}

void MarkerAndFlags(HIRFunction* function, bool source, bool pending_target) {
    if (pending_target) {
        auto* entry = function->GetCurrentBlock();
        WriteFlags(function);
        function->AdvancePC(Imm{u64{1}});
        auto* body = function->AppendBlock(Location{target_pc + 1});
        function->AddEdge(entry, body);
        function->EndBlock(terminal::LinkBlock{Location{target_pc + 1}});
        function->SetCurBlock(body);
    }
    const auto address = reinterpret_cast<uintptr_t>(FptrCast(source ? &SourceMarker : &TargetMarker));
    const auto count = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{u64{address}}});
    function->StoreUniform(Uniform{source ? 0u : 8u, ValueType::U64}, count);
    WriteFlags(function);
}

u8* Compile(const std::shared_ptr<Module>& module, u64 guest, bool source,
            bool cycle, bool rsb, bool continuation, bool call, bool pending_target = false) {
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{guest}, Location{call ? return_pc + 4 : guest + 4});
    const auto next = function->LoadUniform<TypedValue<ValueType::U64>>(
            Uniform{source ? 16u : 24u, ValueType::U64}).SetType(ValueType::U64);
    MarkerAndFlags(function, source, pending_target);
    if (!cycle && !source) {
        function->EndBlock(terminal::ReturnToHost{});
    } else if (call) {
        const auto return_value = function->LoadImm(Imm{return_pc}).SetType(ValueType::U64);
        function->CallReturn(return_value, Imm{return_pc}, Lambda{next});
        function->SetLocation(Lambda{next});
        builder.RegisterCallReturn(Location{return_pc});
        function->EndBlock(terminal::ReturnToDispatch{});
        builder.SetCurBlock(Location{return_pc});
        function->AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::ReturnToHost{});
    } else {
        if (rsb) function->PushRSB(Lambda{Imm{source ? target_pc : source_pc}});
        function->SetLocation(Lambda{next});
        function->EndBlock(rsb || continuation ? Terminal{terminal::PopRSBHint{}}
                                               : Terminal{terminal::ReturnToDispatch{}});
    }
    function->EndFunction();
    auto* code = static_cast<u8*>(TranslateIR(module, function));
    Check(code != nullptr, "compile dynamic unit");
    return code;
}

void SetTargets(Runtime& runtime) {
    const auto uniform = runtime.GetUniformBuffer();
    std::memcpy(uniform.data() + 16, &target_pc, sizeof(target_pc));
    std::memcpy(uniform.data() + 24, &source_pc, sizeof(source_pc));
}

void VerifyFlags(Runtime& runtime) {
    Check((runtime.GetState()->host_cpu_flags & 0xf0000000u) == 0x60000000u,
            "retirement preserves arithmetic NZCV");
    u64 result = ~u64{0};
    std::memcpy(&result, runtime.GetUniformBuffer().data() + 40, sizeof(result));
    Check(result == 0, "live arithmetic result remains published");
}

void FinishDrain(Runtime& runtime, const RetirementControl::Ticket& ticket) {
    Check(control->CheckDrain(ticket) == RetirementControl::DrainStatus::Ready,
            "execution lease drained");
    Check((runtime.GetState()->exit_request & kRetirementExitRequest) != 0, "retirement is sticky");
    Check(control->SealPublications(ticket), "seal after native return");
    Check(control->Resume(ticket), "resume after complete drain");
}

void VerifyWarmL1(Runtime& runtime, void* expected, bool call, bool pending) {
    auto* base = call ? (pending ? runtime.GetState()->pending_call_l1_code_cache
                               : runtime.GetState()->indirect_call_l1_code_cache)
                     : runtime.GetState()->indirect_l1_code_cache;
    Check(base != nullptr, "warm L1 exists");
    const auto& entry = static_cast<const TranslateEntry*>(base)[target_pc & L1_CODE_CACHE_HASH];
    Check(entry.key == target_pc && entry.value == reinterpret_cast<size_t>(expected),
            "actual warm L1 target identity");
    std::printf("warm_l1 call=%u pending=%u key=%llx value=%llx\n", call, pending,
            static_cast<unsigned long long>(entry.key), static_cast<unsigned long long>(entry.value));
}

void Run(const char* mode) {
    const bool rsb = std::strcmp(mode, "rsb") == 0 || std::strcmp(mode, "rsb-lean") == 0;
    const bool continuation = std::strcmp(mode, "continuation") == 0;
    const bool call = std::strcmp(mode, "call") == 0 || std::strcmp(mode, "call-pending") == 0;
    const bool pending = std::strcmp(mode, "call-pending") == 0;
    const bool cycle = !continuation && !call;
    Check(!SmcTracker::IsEnabled(), "SMC disabled");
    Check(!BackedgeLatchEnabled(), "optional latch disabled");
    if (continuation || call) Check(FlagsRegsEnabled(), "continuation ABI requires flags registers");
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = 0x4000;
    config.enable_jit = true;
    config.uniform_buffer_size = 64;
    config.stack_alignment = 16;
    config.backend_isa = kArm64;
    config.global_opts = Optimizations::BlockLink;
    if (rsb || continuation || call) config.global_opts |= Optimizations::ReturnStackBuffer;
    config.region_edges = continuation || pending;
    AddressSpace space{config};
    ModuleConfig module_config{};
    module_config.optimizations = config.global_opts;
    module_config.feature_overrides.Set(FeatureId::indirect_l1, !rsb);
    module_config.feature_overrides.Set(FeatureId::shadow_lean, std::strcmp(mode, "rsb-lean") == 0);
    auto module = space.MapModule(source_pc, 0x4000, module_config);
    Check(bool(module), "map dynamic test module");
    if (continuation || call) Check(module->IsDirectLinkConfigured() &&
            module->GetModuleConfig().HasOpt(Optimizations::ReturnStackBuffer) &&
            ResolveFeatureSet(module->GetModuleConfig()).indirect_l1,
            "continuation module ABI is configured");
    auto* target = Compile(module, target_pc, false, cycle, rsb, false, false, pending);
    Check(Compile(module, source_pc, true, cycle, rsb, continuation, call) != nullptr,
            "source unit is published");
    Runtime runtime{&space};
    active_runtime = &runtime;
    control = space.GetRetirementControl();
    SetTargets(runtime);
    auto* empty = runtime.GetState()->rsb_pointer;
    const auto prepare_frame = [&] {
        runtime.GetState()->rsb_pointer = empty;
        if (continuation) {
            auto* frame = empty - 1;
            frame->guest_location = target_pc;
            frame->dispatch_index = reinterpret_cast<u64>(target);
            runtime.GetState()->rsb_pointer = frame;
        }
    };
    phase.store(cycle ? 1 : 0);
    for (unsigned warm = 0; warm < (call ? 2u : 1u); ++warm) {
        prepare_frame();
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == (cycle ? HaltReason::Signal : HaltReason::CallHost),
                "warm native transfer reaches expected boundary");
        runtime.ClearInterrupt();
    }
    Check(source_count.load() > 0 && target_count.load() > 0, "warm native source and target execute");
    if (!rsb && !continuation) {
        auto expected = static_cast<void*>(target);
        if (call) {
            const auto record = space.GetLinkManager().QueryTarget(target_pc);
            Check(record.has_value(), "call target is published");
            expected = pending ? record->call_pending_flags_host_pc : record->call_host_pc;
            Check(expected != nullptr, "call ABI entry exists");
        }
        VerifyWarmL1(runtime, expected, call, pending);
    }
    if (cycle) {
        phase.store(0);
        source_count.store(0);
        target_count.store(0);
        prepare_frame();
        runtime.SetLocation(source_pc);
        std::atomic_bool returned{};
        HaltReason halt{};
        std::thread worker{[&] { halt = runtime.Run(); returned.store(true, std::memory_order_release); }};
        const bool started = Await([] { return source_count.load() >= 1000 && target_count.load() >= 1000; });
        const auto ticket = RetirementControl::RequestDrain(control);
        const bool exited = Await([&] { return returned.load(std::memory_order_acquire); });
        if (!exited) runtime.SignalInterrupt();
        worker.join();
        Check(started, "actual dynamic cycle executes before closure");
        Check(exited, "request exits without rescue interrupt");
        Check(halt == HaltReason::Retirement, "concurrent request returns Retirement");
        Check(runtime.GetLocation() == source_pc || runtime.GetLocation() == target_pc,
                "concurrent exit publishes a dynamic target PC");
        if (rsb) Check(runtime.GetState()->rsb_pointer == empty, "cycle pop is balanced on exit");
        VerifyFlags(runtime);
        FinishDrain(runtime, ticket);
    }
    prepare_frame();
    source_count.store(0);
    target_count.store(0);
    phase.store(2);
    runtime.SetLocation(source_pc);
    const auto halt = runtime.Run();
    Check(callback_status == RetirementControl::DrainStatus::SelfActive, "callback drain defers own executor");
    Check(source_count.load() == 1 && target_count.load() == 0, "closed callback prevents target body");
    Check(halt == HaltReason::Retirement, "callback returns Retirement");
    Check(runtime.GetLocation() == target_pc, "callback publishes the retained dynamic PC");
    if (rsb || continuation) Check(runtime.GetState()->rsb_pointer == empty, "return consumes exactly one frame");
    if (call) {
        Check(runtime.GetState()->rsb_pointer == empty - 1, "cancelled call publishes one external frame");
        Check((empty - 1)->guest_location == return_pc && (empty - 1)->dispatch_index == 0,
                "cancelled call frame retains guest return and null continuation");
    }
    VerifyFlags(runtime);
    FinishDrain(runtime, callback_ticket);
    std::printf("dynamic=%s rescue_interrupt=0 halt=%u pc=%llx source=%llu target=%llu\n", mode,
            static_cast<unsigned>(halt), static_cast<unsigned long long>(runtime.GetLocation()),
            static_cast<unsigned long long>(source_count.load()), static_cast<unsigned long long>(target_count.load()));
    active_runtime = nullptr;
    callback_ticket = {};
    control.reset();
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2 || (std::strcmp(argv[1], "l1") && std::strcmp(argv[1], "rsb") &&
            std::strcmp(argv[1], "rsb-lean") && std::strcmp(argv[1], "continuation") &&
            std::strcmp(argv[1], "call") && std::strcmp(argv[1], "call-pending")))
            throw std::runtime_error("expected dynamic transfer mode");
        SmcTracker::SetEnabled(false);
        Run(argv[1]);
        std::printf("mode=%s checks=%u passed\n", argv[1], checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "retirement-dynamic failure: %s\n", error.what());
        return 1;
    }
}
