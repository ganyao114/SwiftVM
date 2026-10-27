#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/retirement_control.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/common/backedge_control.h"
#include "runtime/ir/hir_builder.h"

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
using Kind = RetirementControl::Kind;
using DrainStatus = RetirementControl::DrainStatus;
using PublicationMode = RetirementControl::PublicationScope::Mode;
unsigned checks{};

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}

void Control() {
    auto control = std::make_shared<RetirementControl>();
    RetirementControl::Ticket ticket;
    {
        RetirementControl::Lease producer{control, Kind::Producer};
        Check(bool(producer), "initial producer");
        ticket = RetirementControl::RequestDrain(control);
        Check(control->IsClosed(), "close admission");
        Check(control->WaitDrain(ticket) == DrainStatus::SelfActive, "producer never waits for itself");
        Check(!control->SealPublications(ticket), "cannot seal while self-active");
        {
            RetirementControl::Lease nested{control, Kind::Producer};
            Check(bool(nested), "existing producer nesting survives close");
            RetirementControl::PublicationScope publication{nested};
            Check(bool(publication), "existing producer finishes before seal");
            RetirementControl::PublicationScope again{producer};
            Check(bool(again), "nested publication is recursive");
            RetirementControl::PublicationScope cold_link{nested, PublicationMode::OpenOnly};
            Check(!cold_link, "inherited producer cannot cold-link after close");
        }
        {
            RetirementControl::Lease execution{control, Kind::Execution};
            Check(!execution, "closed gate rejects new execution even inside producer");
        }
        std::atomic_bool denied{false};
        std::thread contender{[&] {
            RetirementControl::Lease other{control, Kind::Producer};
            denied.store(!other);
        }};
        contender.join();
        Check(denied.load(), "closed gate rejects unrelated producer");
    }
    Check(control->WaitDrain(ticket) == DrainStatus::Ready, "producer drained");
    Check(control->SealPublications(ticket), "seal after producer drain");
    Check(control->Resume(ticket), "reopen after total drain");
    Check(control->CheckDrain(ticket) == DrainStatus::Stale, "old ticket cannot control new generation");
    {
        RetirementControl::Lease execution{control, Kind::Execution};
        Check(bool(execution), "new generation execution");
        ticket = RetirementControl::RequestDrain(control);
        RetirementControl::Lease producer{control, Kind::Producer};
        Check(!producer, "old executor cannot create producer after close");
        Check(control->WaitDrain(ticket, true) == DrainStatus::SelfActive, "executor cannot self-wait on producers");
    }
    Check(control->SealPublications(ticket), "executor drained seal");
    Check(control->Resume(ticket), "executor drained reopen");
    ticket = RetirementControl::RequestDrain(control, true);
    Check(control->WaitDrain(ticket) == DrainStatus::Ready, "terminal drain ready");
    Check(control->SealPublications(ticket), "terminal seal");
    Check(!control->Resume(ticket), "terminal admission never revives");
}

void Concurrent() {
    auto control = std::make_shared<RetirementControl>();
    for (unsigned round = 0; round < 64; ++round) {
        std::atomic_bool ready{false}, leave{false}, valid{false}, drained{false};
        std::thread producer{[&] {
            RetirementControl::Lease active{control, Kind::Producer};
            ready.store(true);
            while (!leave.load()) std::this_thread::yield();
            RetirementControl::Lease inner{control, Kind::Producer};
            RetirementControl::PublicationScope commit{inner};
            valid.store(bool(active) && bool(inner) && bool(commit));
        }};
        while (!ready.load()) std::this_thread::yield();
        const auto ticket = RetirementControl::RequestDrain(control);
        const auto pending = control->CheckDrain(ticket);
        std::thread waiter{[&] { drained.store(control->WaitDrain(ticket) == DrainStatus::Ready); }};
        leave.store(true);
        producer.join();
        waiter.join();
        Check(pending == DrainStatus::Pending, "active producer remains counted");
        Check(valid.load() && drained.load(), "nested completion wakes drain");
        Check(control->SealPublications(ticket), "concurrent seal");
        Check(control->Resume(ticket), "concurrent next generation");
    }
}

void LinkPublication() {
    auto control = std::make_shared<RetirementControl>();
    for (unsigned round = 0; round < 64; ++round) {
        std::atomic_bool ready{false}, finish{false}, close_started{false}, committed{false};
        RetirementControl::Ticket ticket;
        std::thread linker{[&] {
            RetirementControl::Lease producer{control, Kind::Producer};
            RetirementControl::PublicationScope publication{producer, PublicationMode::OpenOnly};
            ready.store(true);
            while (!finish.load()) std::this_thread::yield();
            committed.store(bool(publication) && !control->IsClosed());
        }};
        while (!ready.load()) std::this_thread::yield();
        std::thread closer{[&] {
            close_started.store(true);
            ticket = RetirementControl::RequestDrain(control);
        }};
        while (!close_started.load()) std::this_thread::yield();
        const bool still_open = !control->IsClosed();
        finish.store(true);
        linker.join();
        closer.join();
        Check(still_open && committed.load(), "close waits for admitted cold mutation");
        Check(control->IsClosed(), "cold mutation precedes closure");
        Check(control->WaitDrain(ticket) == DrainStatus::Ready, "cold producer drained");
        Check(control->SealPublications(ticket), "cold publication sealed");
        {
            RetirementControl::Lease late{control, Kind::Producer};
            RetirementControl::PublicationScope publication{late, PublicationMode::OpenOnly};
            Check(!late && !publication, "sealed generation denies cold mutation");
        }
        Check(control->Resume(ticket), "cold test reopens next generation");
        {
            RetirementControl::Lease producer{control, Kind::Producer};
            ticket = RetirementControl::RequestDrain(control);
            RetirementControl::PublicationScope late{producer, PublicationMode::OpenOnly};
            RetirementControl::PublicationScope completion{producer};
            Check(!late && bool(completion), "close-first denies cold linking but permits producer completion");
        }
        Check(control->SealPublications(ticket), "close-first seal");
        Check(control->Resume(ticket), "close-first next generation");
    }
}

Config TestConfig() {
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = 0x4000;
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

thread_local AddressSpace* callback_space{};
thread_local Runtime* callback_runtime{};
thread_local RetirementControl::Ticket callback_ticket;
thread_local bool callback_failed{};

u64 CloseFromHelper() noexcept {
    try {
        auto control = callback_space->GetRetirementControl();
        callback_ticket = RetirementControl::RequestDrain(control);
        Check(control->WaitDrain(callback_ticket) == DrainStatus::SelfActive,
              "generated callback cannot wait for its own return");
        Check(callback_runtime->Run() == HaltReason::RuntimeBusy, "same Runtime still rejects reentry");
        RetirementControl::Lease producer{control, Kind::Producer};
        Check(!producer, "generated callback cannot revive compilation");
        return 17;
    } catch (...) {
        callback_failed = true;
        return 0;
    }
}

void RuntimeGate() {
    AddressSpace space{TestConfig()};
    Runtime runtime{&space};
    auto module = space.GetDefaultModule();
    HIRBuilder builder{1, true, ResolveFeatureSet(ModuleConfig{})};
    auto* function = builder.AppendFunction(Location{0x1000}, Location{0x1004});
    u64 (*callback)() = &CloseFromHelper;
    const auto address = static_cast<u64>(reinterpret_cast<uintptr_t>(FptrCast(callback)));
    auto value = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{address}});
    function->StoreUniform(Uniform{0, ValueType::U64}, value);
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    auto* code = TranslateIR(module, function);
    Check(code != nullptr, "compile draining callback");
    callback_space = &space;
    callback_runtime = &runtime;
    runtime.SetLocation(0x1000);
    Check(runtime.Run() == HaltReason::Retirement, "host boundary reports retirement instead of CodeMiss");
    Check(!callback_failed, "helper exception handling");
    u64 result{};
    std::memcpy(&result, runtime.GetUniformBuffer().data(), sizeof(result));
    Check(result == 17, "generated frame returned before drain");
    Check(space.GetRetirementControl()->WaitDrain(callback_ticket) == DrainStatus::Ready,
          "runtime admission released after generated return");
    Check(runtime.Run() == HaltReason::Retirement, "closed runtime never looks up code");
    Check(TranslateIR(module, function) == nullptr, "cached backend return rejects closed admission");
    Check(space.PushCodeCache(Location{0x2000}, code) == 0, "closed publication is rejected");
    Check(space.GetCodeCacheTable().Lookup(0x2000) == 0, "rejected alias was never visible");
    const auto allocation = module->AllocCodeCache(32);
    Check(allocation.first == INVALID_CACHE_ID, "closed allocation rejects before owner access");
    Check(space.GetRetirementControl()->SealPublications(callback_ticket), "callback generation sealed");
    Check(space.GetRetirementControl()->Resume(callback_ticket), "test resumes after synthetic drain");
    callback_space = nullptr;
    callback_runtime = nullptr;
    callback_ticket = {};
}

void RuntimeColdLink() {
    auto config = TestConfig();
    config.global_opts = Optimizations::BlockLink;
    AddressSpace space{config};
    Runtime runtime{&space};
    auto module = space.GetDefaultModule();
    Check(module->IsDirectLinkConfigured(), "production cold links enabled");
    HIRBuilder target_builder{1, true, ResolveFeatureSet(ModuleConfig{})};
    auto* target = target_builder.AppendFunction(Location{0x2000}, Location{0x2004});
    auto poison = target->LoadImm(ValueType::U64, Imm{0xbad});
    target->StoreUniform(Uniform{8, ValueType::U64}, poison);
    target->EndBlock(terminal::ReturnToHost{});
    target->EndFunction();
    Check(TranslateIR(module, target) != nullptr, "publish cold-link target");
    HIRBuilder source_builder{1, true, ResolveFeatureSet(ModuleConfig{})};
    auto* source = source_builder.AppendFunction(Location{0x1000}, Location{0x1004});
    u64 (*callback)() = &CloseFromHelper;
    const auto address = static_cast<u64>(reinterpret_cast<uintptr_t>(FptrCast(callback)));
    auto value = source->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{address}});
    source->StoreUniform(Uniform{0, ValueType::U64}, value);
    source->EndBlock(terminal::LinkBlock{Location{0x2000}});
    source->EndFunction();
    Check(TranslateIR(module, source) != nullptr, "publish cold-link source");
    const auto before = space.GetLinkManager().GetStats();
    Check(before.sites != 0, "source emitted a real cold site");
    callback_failed = false;
    callback_space = &space;
    callback_runtime = &runtime;
    runtime.SetLocation(0x1000);
    Check(runtime.Run() == HaltReason::Retirement, "closed cold link returns host");
    Check(!callback_failed, "cold callback completed");
    u64 observed{}, target_observed{};
    std::memcpy(&observed, runtime.GetUniformBuffer().data(), sizeof(observed));
    std::memcpy(&target_observed, runtime.GetUniformBuffer().data() + 8, sizeof(target_observed));
    Check(observed == 17 && target_observed == 0, "closed cold link commits source without traversing target");
    const auto after = space.GetLinkManager().GetStats();
    Check(after.linker_calls == before.linker_calls && after.linked == before.linked &&
                  after.far == before.far, "denied helper does not change link state or stats");
    Check(space.GetRetirementControl()->WaitDrain(callback_ticket) == DrainStatus::Ready,
          "generated cold frame finished before drain");
    Check(space.GetRetirementControl()->SealPublications(callback_ticket), "cold callback seal");
    Check(space.GetRetirementControl()->Resume(callback_ticket), "cold callback test resumes");
    callback_space = nullptr;
    callback_runtime = nullptr;
    callback_ticket = {};
}

void RetainedModule() {
    std::shared_ptr<Module> retained;
    {
        AddressSpace space{TestConfig()};
        retained = space.GetDefaultModule();
    }
    auto control = retained->GetRetirementControl();
    RetirementControl::Lease producer{control, Kind::Producer};
    Check(!producer, "module retains terminal control after address-space destruction");
    Check(retained->AllocCodeCache(32).first == INVALID_CACHE_ID,
          "stale module allocation never reads dead Config");
    Check(!retained->PrepareDirectLinkRegion(), "stale module region preparation denied");
    Check(retained->PublishLinkTarget(Location{0x1000}, nullptr, nullptr) == 0,
          "stale module publication never dereferences AddressSpace");
    IntrusivePtr<Block> block{new Block(Location{0x1000})};
    Check(!retained->Push(block.get()), "stale module cannot acquire a raw node");
    Check(TranslateIR(retained, block) == nullptr, "stale module backend compile denied");
}

void PersistenceOwner() {
    AddressSpace owner{TestConfig()};
    AddressSpace foreign{TestConfig()};
    JitDiskCache disk{owner};
    const auto record = [&](const std::shared_ptr<Module>& module) {
        disk.RecordUnit(module, 0x1000, false, nullptr, nullptr, 0, {}, {}, {});
    };
    record(foreign.GetDefaultModule());
    Check(disk.Stats().units_compiled.load() == 0, "foreign module cannot provide disk lease");
    record(owner.GetDefaultModule());
    Check(disk.Stats().units_compiled.load() == 1, "same-owner record is admitted");
    auto ticket = RetirementControl::RequestDrain(owner.GetRetirementControl());
    record(foreign.GetDefaultModule());
    record(owner.GetDefaultModule());
    Check(disk.Stats().units_compiled.load() == 1, "closed disk owner rejects every record");
    disk.Save();
    Check(owner.GetRetirementControl()->SealPublications(ticket), "public save does not revive a lease");
    Check(owner.GetRetirementControl()->Resume(ticket), "persistence ownership test resumes");
}

void ExitRequests() {
    auto control = std::make_shared<RetirementControl>();
    u64 first = kBackedgeSignalRequest | 7, second = 3, late = kBackedgeSignalRequest | 9;
    u64 removed{};
    RetirementControl::ExitRegistration first_registration{control, first};
    RetirementControl::ExitRegistration second_registration{control, second};
    RetirementControl::Ticket ticket;
    {
        RetirementControl::Lease execution{control, Kind::Execution};
        ticket = RetirementControl::RequestDrain(control);
        Check(first == (kBackedgeSignalRequest | kRetirementExitRequest | 7),
              "retirement preserves signal and SMC generation");
        Check(second == (kRetirementExitRequest | 3), "retirement reaches each registered exit word");
        Check(control->WaitDrain(ticket) == DrainStatus::SelfActive, "requested executor cannot self-wait");
        Check(!control->Resume(ticket), "active retirement cannot be cleared");
    }
    Check(control->WaitDrain(ticket) == DrainStatus::Ready, "requested execution drained");
    RetirementControl::ExitRegistration late_registration{control, late};
    Check(late == (kBackedgeSignalRequest | kRetirementExitRequest | 9), "late registration inherits close");
    { RetirementControl::ExitRegistration temporary{control, removed}; }
    removed = 0x123;
    Check(control->SealPublications(ticket), "exit registration seal");
    Check(control->Resume(ticket), "exit registration resume");
    Check(first == (kBackedgeSignalRequest | 7), "resume preserves independent signal and SMC");
    Check(second == 3, "resume only clears retirement");
    Check(late == (kBackedgeSignalRequest | 9), "late registration cleared by resume");
    Check(removed == 0x123, "unregistered exit word is not touched");
    AddressSpace space{TestConfig()};
    Runtime runtime{&space};
    auto runtime_control = space.GetRetirementControl();
    const auto runtime_ticket = RetirementControl::RequestDrain(runtime_control);
    Check((runtime.GetState()->exit_request & kRetirementExitRequest) != 0, "Runtime registers retirement request");
    runtime.SignalInterrupt();
    Check((runtime.GetState()->exit_request & (kBackedgeSignalRequest | kRetirementExitRequest)) ==
                  (kBackedgeSignalRequest | kRetirementExitRequest), "signal cannot replace retirement");
    runtime.ClearInterrupt();
    Check(runtime.GetState()->exit_request == kRetirementExitRequest, "interrupt clear preserves retirement");
    Check(runtime_control->SealPublications(runtime_ticket), "Runtime request seal");
    Check(runtime_control->Resume(runtime_ticket), "Runtime request resume");
    Check(runtime.GetState()->exit_request == 0, "Runtime request clears after drain");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected one retirement-control mode");
        SmcTracker::SetEnabled(false);
        if (std::strcmp(argv[1], "control") == 0) Control();
        else if (std::strcmp(argv[1], "concurrent") == 0) Concurrent();
        else if (std::strcmp(argv[1], "link-publication") == 0) LinkPublication();
        else if (std::strcmp(argv[1], "links") == 0) RuntimeColdLink();
        else if (std::strcmp(argv[1], "runtime") == 0) RuntimeGate();
        else if (std::strcmp(argv[1], "retained") == 0) RetainedModule();
        else if (std::strcmp(argv[1], "persistence") == 0) PersistenceOwner();
        else if (std::strcmp(argv[1], "requests") == 0) ExitRequests();
        else throw std::runtime_error("unknown retirement-control mode");
        std::printf("mode=%s checks=%u passed\n", argv[1], checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "retirement-control failure: %s\n", error.what());
        return 1;
    }
}
