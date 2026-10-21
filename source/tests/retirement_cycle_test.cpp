#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/hir_builder.h"

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
unsigned checks{};
std::atomic<u64> source_count{}, target_count{};
std::atomic_bool warming{};
Runtime* active_runtime{};

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    ++checks;
}

u64 SourceMarker() {
    const auto count = source_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (warming.load(std::memory_order_relaxed) && count >= 64) active_runtime->SignalInterrupt();
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

void AddMarker(HIRFunction* function, u64 (*callback)(), u32 offset) {
    const auto address = static_cast<u64>(reinterpret_cast<uintptr_t>(FptrCast(callback)));
    const auto value = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{address}});
    function->StoreUniform(Uniform{offset, ValueType::U64}, value);
}

u8* Compile(const std::shared_ptr<Module>& module, u64 guest, u64 (*callback)(), u64 next) {
    HIRBuilder builder{1, true, ResolveFeatureSet(ModuleConfig{})};
    auto* function = builder.AppendFunction(Location{guest}, Location{guest + 4});
    AddMarker(function, callback, guest == 0x1000 ? 0 : 8);
    function->EndBlock(terminal::LinkBlock{Location{next}});
    function->EndFunction();
    auto* code = static_cast<u8*>(TranslateIR(module, function));
    Check(code != nullptr, "compile cycle unit");
    return code;
}

void VerifyPatched(const std::shared_ptr<Module>& module, AddressSpace& space, u8* code) {
    const auto sites = space.GetLinkManager().QuerySourceSites({module.get(), code});
    Check(sites.size() == 1, "cycle has one real site");
    const auto& site = sites.front();
    Check(site.state == LinkSiteState::Linked || site.state == LinkSiteState::Far, "cycle site is warmed");
    const auto region = module->GetCodeRegion(site.site.region_id);
    Check(bool(region), "cycle site region is retained");
    u32 instruction{};
    std::memcpy(&instruction, region->rx_base + site.site.offset, sizeof(instruction));
    Check(instruction != site.unlinked_instruction, "cycle has patched RX bytes");
    std::printf("patched cold=%08x rx=%08x\n", site.unlinked_instruction, instruction);
}

void Cycle(const char* shape) {
    Check(!SmcTracker::IsEnabled(), "SMC disabled for retirement test");
    Check(!BackedgeLatchEnabled(), "optional backedge latch disabled for retirement test");
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = 0x4000;
    config.enable_jit = true;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::BlockLink;
    const bool region_shape = std::strcmp(shape, "region") == 0;
    const bool self_shape = std::strcmp(shape, "self") == 0;
    config.region_edges = region_shape;
#if defined(__riscv) && __riscv_xlen == 64
    config.backend_isa = kRiscv64;
#else
    config.backend_isa = kArm64;
#endif
    AddressSpace space{config};
    Runtime runtime{&space};
    active_runtime = &runtime;
    auto module = space.GetDefaultModule();
    u8* source{}, *target{};
    if (region_shape) {
        HIRBuilder builder{1, true, ResolveFeatureSet(ModuleConfig{})};
        auto* function = builder.AppendFunction(Location{0x1000}, Location{0x2004});
        auto* source_block = function->GetCurrentBlock();
        auto* target_block = function->AppendBlock(Location{0x2000});
        function->AddEdge(source_block, target_block);
        function->AddEdge(target_block, source_block);
        AddMarker(function, &SourceMarker, 0);
        function->EndBlock(terminal::LinkBlock{Location{0x2000}});
        function->SetCurBlock(target_block);
        AddMarker(function, &TargetMarker, 8);
        function->EndBlock(terminal::LinkBlock{Location{0x1000}});
        function->EndFunction();
        Check(TranslateIR(module, function) != nullptr, "compile region cycle");
    } else if (self_shape) {
        source = Compile(module, 0x1000, &SourceMarker, 0x1000);
    } else {
        target = Compile(module, 0x2000, &TargetMarker, 0x1000);
        source = Compile(module, 0x1000, &SourceMarker, 0x2000);
    }
    warming.store(true);
    runtime.SetLocation(0x1000);
    const auto warm = runtime.Run();
    if (warm != HaltReason::Signal)
        std::fprintf(stderr, "warm halt=%u source=%llu target=%llu\n",
                static_cast<unsigned>(warm), static_cast<unsigned long long>(source_count.load()),
                static_cast<unsigned long long>(target_count.load()));
    Check(warm == HaltReason::Signal, "warm cycle ends by explicit signal");
    if (!region_shape && !self_shape) {
        VerifyPatched(module, space, source);
        VerifyPatched(module, space, target);
    }
    warming.store(false);
    runtime.ClearInterrupt();
    runtime.SetLocation(0x1000);
    source_count.store(0);
    target_count.store(0);
    HaltReason halt{};
    std::atomic_bool returned{};
    std::thread worker{[&] {
        halt = runtime.Run();
        returned.store(true, std::memory_order_release);
    }};
    const bool started = Await([region_shape] {
        return source_count.load(std::memory_order_relaxed) >= 1000 &&
               (!region_shape || target_count.load(std::memory_order_relaxed) >= 1000);
    });
    auto control = space.GetRetirementControl();
    const auto ticket = RetirementControl::RequestDrain(control);
    const bool exited = Await([&] { return returned.load(std::memory_order_acquire); });
    // Failure cleanup is bounded; requiring exited below prevents rescue from
    // being mistaken for a successful retirement-request exit.
    if (!exited) runtime.SignalInterrupt();
    worker.join();
    Check(started, "warm patched cycle executed before closure");
    if (region_shape) Check(target_count.load(std::memory_order_relaxed) >= 1000,
            "region cycle traverses both blocks before closure");
    Check(exited, "retirement exits cycle without an extra interrupt");
    Check(halt == HaltReason::Retirement, "cycle reports retirement at host boundary");
    Check(control->CheckDrain(ticket) == RetirementControl::DrainStatus::Ready, "cycle execution lease drained");
    Check((runtime.GetState()->exit_request & kRetirementExitRequest) != 0, "retirement request remains sticky");
    Check(control->SealPublications(ticket), "cycle publication seal");
    Check(control->Resume(ticket), "cycle resume after full execution drain");
    Check((runtime.GetState()->exit_request & kRetirementExitRequest) == 0, "resume clears request");
    std::printf("shape=%s rescue_interrupt=0 halt=%u source=%llu target=%llu\n", shape,
            static_cast<unsigned>(halt), static_cast<unsigned long long>(source_count.load()),
            static_cast<unsigned long long>(target_count.load()));
    active_runtime = nullptr;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2 || (std::strcmp(argv[1], "two-unit") &&
                         std::strcmp(argv[1], "self") && std::strcmp(argv[1], "region")))
            throw std::runtime_error("expected cycle shape");
        SmcTracker::SetEnabled(false);
        Cycle(argv[1]);
        std::printf("mode=%s checks=%u passed\n", argv[1], checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "retirement-cycle failure: %s\n", error.what());
        return 1;
    }
}
