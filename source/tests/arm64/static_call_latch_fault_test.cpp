#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "aot/aot_load.h"
#include "serialized_aot_fixture.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/code_serial.h"
#include "runtime/backend/context.h"
#include "runtime/backend/jit_cache.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/hir_builder.h"

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
unsigned checks{}, phase{};
u64 source_count{}, target_count{}, target_pc{}, call_target_pc{}, return_pc{}, callback_request{};
AddressSpace* active_space{};
Runtime* active_runtime{};
bool explicit_request{};
std::shared_ptr<RetirementControl> control;
RetirementControl::Ticket ticket;
RetirementControl::DrainStatus callback_status{};

void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
    ++checks;
}

class GuestWindow {
public:
    const size_t page_size = static_cast<size_t>(getpagesize());
    const size_t size = 8 * page_size;
    void* const data = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    GuestWindow() {
        Check(page_size >= 4096 && (size & (size - 1)) == 0, "power-of-two guest window");
        Check(data != MAP_FAILED, "map genuine SMC guest pages");
    }
    ~GuestWindow() { if (data != MAP_FAILED) munmap(data, size); }
};

struct FaultObservation {
    uintptr_t latch{};
    std::atomic<unsigned> count{};
    std::atomic<uintptr_t> address{}, pc{}, return_register{}, rsb_register{};
};
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<uintptr_t>::is_always_lock_free);

bool ObserveFault(void* opaque, ucontext_t* context, int signal, siginfo_t* info) {
    auto& observed = *static_cast<FaultObservation*>(opaque);
    if ((signal == SIGSEGV || signal == SIGBUS) &&
        reinterpret_cast<uintptr_t>(info->si_addr) == observed.latch) {
        observed.address.store(reinterpret_cast<uintptr_t>(info->si_addr), std::memory_order_relaxed);
        observed.pc.store(SignalHandler::GetContextPC(context), std::memory_order_relaxed);
        observed.return_register.store(SignalHandler::GetContextGPR(context, 14), std::memory_order_relaxed);
        observed.rsb_register.store(SignalHandler::GetContextGPR(context, 25), std::memory_order_relaxed);
        observed.count.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

class ObserverRegistration {
public:
    explicit ObserverRegistration(FaultObservation& observation) : observation{observation} {
        SignalHandler::RegisterHandler(&ObserveFault, &observation, -1);
    }
    ~ObserverRegistration() { SignalHandler::UnregisterHandler(&observation); }
private:
    FaultObservation& observation;
};

u64 SourceMarker() {
    ++source_count;
    if (phase == 1 && source_count == 1) {
        if (explicit_request) {
            ticket = RetirementControl::RequestDrain(control);
            callback_status = control->CheckDrain(ticket);
        } else {
            active_space->InvalidateCodeRange(call_target_pc, call_target_pc + 1);
        }
        callback_request = std::atomic_ref<u64>(active_runtime->GetState()->exit_request)
                                   .load(std::memory_order_acquire);
    }
    return source_count;
}

u64 TargetMarker() { return ++target_count; }

void SourceBody(HIRFunction* function) {
    const auto marker = reinterpret_cast<uintptr_t>(FptrCast(&SourceMarker));
    const auto count = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{u64{marker}}});
    function->StoreUniform(Uniform{0, ValueType::U64}, count);
    const auto left = function->LoadImm(Imm{~u64{0}}).SetType(ValueType::U64);
    const auto one = function->LoadImm(Imm{u64{1}}).SetType(ValueType::U64);
    const auto result = function->Add(left, Operand{one}).SetType(ValueType::U64);
    function->SaveFlags(result, Flags::All);
    function->StoreUniform(Uniform{40, ValueType::U64}, result);
}

u8* CompileTarget(const std::shared_ptr<Module>& module, bool rsb = false) {
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{target_pc}, Location{target_pc + 4});
    const auto marker = reinterpret_cast<uintptr_t>(FptrCast(&TargetMarker));
    const auto count = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{u64{marker}}});
    function->StoreUniform(Uniform{8, ValueType::U64}, count);
    if (rsb) {
        function->AdvancePC(Imm{u64{1}});
        auto* tail = builder.LinkBlock(terminal::LinkBlock{Location{target_pc + 2}});
        builder.SetCurBlock(tail);
        const auto resume = function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{16, ValueType::U64});
        function->SetLocation(Lambda{resume});
        function->EndBlock(terminal::PopRSBHint{});
    } else {
        function->EndBlock(terminal::ReturnToHost{});
    }
    function->EndFunction();
    auto* code = static_cast<u8*>(TranslateIR(module, function));
    Check(code != nullptr, "compile independent target function");
    return code;
}

u8* CompileSource(const std::shared_ptr<Module>& module, u64 source_pc, bool call, bool mixed, bool indirect = false, bool rsb = false, bool internal = false, bool published = false) {
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{source_pc}, Location{call ? return_pc + 4 : source_pc + 4});
    SourceBody(function);
    if (call) {
        const auto choice = mixed ? function->LoadUniform(Uniform{16, ValueType::U64}) : Value{};
        const auto ret = function->LoadImm(Imm{return_pc}).SetType(ValueType::U64);
        const auto destination = internal ? target_pc + 2 : target_pc;
        const auto target = indirect
                ? Lambda{function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{32, ValueType::U64})}
                : Lambda{Imm{destination}};
        function->CallReturn(ret, Imm{return_pc}, target);
        function->SetLocation(target);
        builder.RegisterCallReturn(Location{return_pc});
        if (mixed) {
            // ExternalLinkBlock restores the arm's constant after If publishes SetLocation.
            function->EndBlock(terminal::If{BOOL{choice}, terminal::ExternalLinkBlock{Location{target_pc}},
                                            terminal::LinkBlock{Location{target_pc}}});
        } else if (published) {
            function->EndBlock(terminal::ExternalLinkBlock{Location{target_pc}});
        } else {
            function->EndBlock(terminal::ReturnToDispatch{});
        }
        builder.SetCurBlock(Location{return_pc});
        if (rsb || internal) function->StoreUniform(Uniform{24, ValueType::U64},
                function->LoadImm(Imm{u64{0x51ab}}).SetType(ValueType::U64));
        function->AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::ReturnToHost{});
    } else {
        function->EndBlock(terminal::LinkBlock{Location{target_pc}});
    }
    function->EndFunction();
    auto* code = static_cast<u8*>(TranslateIR(module, function));
    Check(code != nullptr, "compile actual source function");
    return code;
}

u32 Instruction(const u8* address) {
    u32 instruction{};
    std::memcpy(&instruction, address, sizeof(instruction));
    return instruction;
}

const u8* BranchTarget(const u8* address) {
    const u32 instruction = Instruction(address);
    Check((instruction & 0xfc000000u) == 0x14000000u, "request arm ends with direct B");
    const s32 words = static_cast<s32>((instruction & 0x03ffffffu) << 6) >> 6;
    return address + static_cast<ptrdiff_t>(words) * 4;
}

std::vector<FaultEntry> PollEntries(const std::shared_ptr<Module>& module, u8* source, bool mixed, bool indirect = false) {
    FaultEntry allocation;
    std::vector<FaultEntry> entries;
    Check(module->LookupCodeAllocation(source, allocation, &entries), "actual source RX allocation");
    // LDUR WZR, [X28(State), #-40], with the signed offset taken from the public ABI.
    constexpr u32 poll_load = 0xb8400000u | ((static_cast<u32>(state_offset_interrupt_poll) & 511u) << 12)
                                             | (28u << 5) | 31u;
    std::erase_if(entries, [&](const FaultEntry& entry) {
        return entry.host_end - entry.host_start != 4 || Instruction(entry.host_start) != poll_load;
    });
    std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
        return left.host_start < right.host_start;
    });
    Check(entries.size() == (indirect ? 0u : mixed ? 2u : 1u), "actual per-arm poll catalog inventory");
    for (const auto& entry : entries) {
        Check(entry.owner_start == allocation.host_start && entry.guest_loc == call_target_pc &&
              entry.recovery_kind == FaultRecoveryKind::GuestFault &&
              entry.recovery >= allocation.host_start && entry.recovery < allocation.host_end,
              "poll catalog binds retained source allocation and selected target");
    }
    return entries;
}

void VerifyPollCodec(const FaultEntry& allocation, std::span<const FaultEntry> polls,
                     u64 source_pc, bool call, bool mixed) {
    SerialUnit input{};
    input.guest_start = source_pc;
    input.is_function = 1;
    input.code.assign(allocation.host_start, allocation.host_end);
    for (const auto& poll : polls) {
        input.fault_sites.push_back({poll.guest_loc,
                static_cast<u32>(poll.host_start - allocation.host_start),
                static_cast<u32>(poll.host_end - allocation.host_start),
                static_cast<u32>(poll.recovery - allocation.host_start),
                static_cast<u8>(poll.recovery_kind)});
    }
    BlobWriter writer;
    WriteUnit(writer, input);
    BlobReader reader{writer.Data().data(), writer.Size()};
    SerialUnit output{};
    Check(ReadUnit(reader, output) && reader.Empty(), "real poll metadata decodes completely");
    Check(output.code == input.code && output.fault_sites.size() == polls.size(),
          "real poll code and fault inventory survive codec");
    for (size_t index = 0; index < polls.size(); ++index) {
        const auto& site = output.fault_sites[index];
        const auto& before = input.fault_sites[index];
        Check(site.guest_start == before.guest_start && site.host_begin == before.host_begin &&
              site.host_end == before.host_end && site.recovery_offset == before.recovery_offset &&
              site.recovery_kind == before.recovery_kind, "codec preserves per-site recovery identity");
        Check(Instruction(output.code.data() + site.host_begin) == Instruction(polls[index].host_start) &&
              Instruction(output.code.data() + site.recovery_offset) == Instruction(polls[index].recovery),
              "codec preserves actual poll and recovery instructions");
    }
    if (call && !output.fault_sites.empty()) Check(Instruction(output.code.data() + output.fault_sites[0].recovery_offset) == 0xa9bf7f2eu,
                    "serialized Call recovery retains external frame wrapper");
    if (mixed) Check(output.fault_sites[0].recovery_offset + 4 == output.fault_sites[1].recovery_offset,
                     "codec preserves distinct mixed-arm recovery offsets");
}

void VerifyWarm(const std::shared_ptr<Module>& module, AddressSpace& space, u8* source, bool call, bool mixed, bool indirect = false, bool internal = false) {
    const auto sites = space.GetLinkManager().QuerySourceSites({module.get(), source});
    Check(sites.size() == (indirect ? 0u : mixed ? 2u : 1u), "actual static site inventory");
    unsigned calls{};
    for (const auto& site : sites) {
        calls += site.kind == LinkSiteKind::Call;
        if (internal) {
            Check(site.state == LinkSiteState::Unlinked, "internal entry has no call-entry publication and stays dispatcher-resolved");
            continue;
        }
        Check(site.state == LinkSiteState::Linked || site.state == LinkSiteState::Far, "each actual static arm warmed");
        const auto region = module->GetCodeRegion(site.site.region_id);
        Check(region && Instruction(region->rx_base + site.site.offset) != site.unlinked_instruction,
              "real RX static site patched");
    }
    Check(calls == (call && !indirect ? 1u : 0u), "actual Call continuation admission");
    std::printf("warm sites=%zu calls=%u mixed=%u\n", sites.size(), calls, mixed);
}

void Run(std::string_view mode) {
    const bool restored = mode.starts_with("aot-");
    if (restored) mode.remove_prefix(4);
    const bool disk_restored = mode.starts_with("disk-");
    if (disk_restored) mode.remove_prefix(5);
    explicit_request = mode.starts_with("request-");
    if (explicit_request) mode.remove_prefix(8);
    const bool mixed = mode.starts_with("mixed-");
    const bool indirect = mode == "indirect-call" || mode == "indirect-internal-call";
    const bool rsb = mode == "rsb-call";
    const bool internal = mode == "internal-call" || mode == "indirect-internal-call";
    const bool published = mode == "published-call";
    const bool call = mode != "branch";
    const bool selected_call = mode == "call" || mode == "mixed-call" || indirect || rsb || internal || published;
    Check(mode == "call" || mode == "branch" || mode == "mixed-call" || mode == "mixed-branch" || indirect || rsb || internal || published, "known latch mode");
    Check(SmcTracker::IsEnabled() && BackedgeLatchEnabled(), "real SMC and optional latch enabled");
    if (call) Check(FlagsRegsEnabled(), "Call fixture requires flags registers");
    GuestWindow guest;
    target_pc = guest.page_size + 0x100;
    call_target_pc = internal ? target_pc + 2 : target_pc;
    const u64 source_pc = 5 * guest.page_size + 0x100;
    return_pc = source_pc + 0x100;
    Config config{};
    config.loc_start = 0;
    config.loc_end = guest.size;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = 64;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = call;
    config.memory_base = guest.data;
    config.guest_addr_mask = guest.size - 1;
    auto space_owner = std::make_unique<AddressSpace>(config);
    auto& space = *space_owner;
    space.GetSmcTracker().EnableMultithreading();
    auto module = space.GetDefaultModule();
    Check(bool(module) && space.ExitLatchEnabled(), "map real latch-enabled module");
    u8* target{};
    u8* source{};
    if (disk_restored) {
        space.LoadJitCache();
        auto* cache = space.GetJitDiskCache();
        Check(cache && cache->Enabled(), "admit production disk cache for actual latch shape");
        const auto stage = std::string_view{std::getenv("STATIC_LATCH_DISK_PHASE")};
        if (stage == "write") {
            Check(cache->Stats().units_loaded.load() == 0, "disk writer starts without inherited latch units");
            CompileTarget(module, rsb || internal);
            CompileSource(module, source_pc, call, mixed, indirect, rsb, internal, published);
            Check(cache->Stats().units_compiled.load() == 2 && cache->Stats().reject_scan.load() == 0,
                  "production recorder captures both physical latch units");
            space.GetJitDiskCache()->Save();
            Check(cache->Stats().units_stored.load() == 2, "production writer stores both complete latch units");
            std::printf("disk_units phase=write loaded=0 compiled=2 stored=2\n");
            return;
        }
        Check(stage == "read" && cache->Stats().units_loaded.load() == 2 && cache->Stats().units_compiled.load() == 0 &&
              cache->Stats().reject_guest_bytes.load() == 0 && cache->Stats().reject_reloc.load() == 0,
              "fresh exec reader restores both complete physical latch units without compilation");
        const auto allocation = [&](u64 location) -> u8* {
            const auto node = module->GetNode(Location{location});
            Check(IsFunction(node), "disk revival retains actual function owner");
            return static_cast<u8*>(module->GetJitCache(GetFunction(node)->GetJitCache()));
        };
        target = allocation(target_pc);
        source = allocation(source_pc);
        std::printf("disk_units phase=read loaded=2 compiled=0 stored=0\n");
    } else if (restored) {
        GuestWindow original_guest;
        auto original_config = config;
        original_config.memory_base = original_guest.data;
        auto original = std::make_unique<AddressSpace>(original_config);
        original->SetSerializableCodeEmission();
        const auto original_module = original->GetDefaultModule();
        const auto* original_target = CompileTarget(original_module, rsb || internal);
        const auto* original_source = CompileSource(original_module, source_pc, call, mixed, indirect, rsb, internal, published);
        const std::array<u64, 2> locations{target_pc, source_pc};
        const auto image = swift::test::CaptureAotFunctions(*original, original_guest.data, original_guest.size, locations);
        Check(image.units.size() == 2, "capture both genuine latch source and target allocations");
        const auto blob = aot::EncodeInfoBlob(image);
        aot::AotImage decoded;
        std::string error;
        Check(aot::DecodeInfoBlob(blob.data(), blob.size(), decoded, error), "round trip full AOT latch recipes");
        decoded.host_isa = image.host_isa;
        decoded.code = image.code;
        aot::LoadReport report;
        const auto status = aot::InstallArtifact(space, decoded, guest.data, config.guest_addr_mask, report, error);
        if (status != aot::kAotOk) throw std::runtime_error("install AOT latch fixture: " + error);
        Check(report.units_installed == 2 && report.mapping, "install both actual AOT latch units");
        target = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, decoded.units.front().code_offset));
        source = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, decoded.units.back().code_offset));
        Check(target && source && target != original_target && source != original_source,
              "restored latch code has distinct RX identities while source allocations are still live");
        original.reset();
        Check(space.GetCodeCache(Location{source_pc}) && space.GetCodeCache(Location{target_pc}),
              "restored latch entries survive original AddressSpace destruction");
        std::printf("aot_latch units=2 distinct_rx=1 source_destroyed=1 mixed=%u request=%u\n", mixed, explicit_request);
    } else {
        target = CompileTarget(module, rsb || internal);
        source = CompileSource(module, source_pc, call, mixed, indirect, rsb, internal, published);
    }
    FaultEntry source_allocation, target_allocation;
    Check(module->LookupCodeAllocation(source, source_allocation) &&
          module->LookupCodeAllocation(target, target_allocation) &&
          source_allocation.host_start != target_allocation.host_start, "separate source and target allocation owners");
    Runtime runtime{&space};
    active_space = &space;
    active_runtime = &runtime;
    control = space.GetRetirementControl();
    auto* state = runtime.GetState();
    auto* empty = state->rsb_empty;
    FaultObservation observed;
    observed.latch = reinterpret_cast<uintptr_t>(reinterpret_cast<u8*>(state) + state_offset_interrupt_poll);
    ObserverRegistration registration{observed};
    for (unsigned warm = 0; warm < (mixed ? 4u : 2u); ++warm) {
        const u64 choice = (rsb || internal) ? return_pc : mixed ? warm % 2 : 1;
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &choice, sizeof(choice));
        if (indirect) std::memcpy(runtime.GetUniformBuffer().data() + 32, &call_target_pc, sizeof(call_target_pc));
        state->rsb_pointer = empty;
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == HaltReason::CallHost, "prewarm real source and target");
        if (rsb || internal) {
            u64 witness{};
            std::memcpy(&witness, runtime.GetUniformBuffer().data() + 24, sizeof(witness));
            Check(witness == 0x51ab && state->rsb_pointer == empty,
                  "RSB return reaches the guest return body and balances the prediction stack");
        }
    }
    Check(source_count == (mixed ? 4u : 2u) && target_count == (internal ? 0u : source_count) && observed.count.load() == 0,
          "all warm bodies executed without a fault");
    VerifyWarm(module, space, source, call, mixed, indirect, internal);
    const auto polls = PollEntries(module, source, mixed, indirect);
    source_count = target_count = 0;
    const u64 choice = (rsb || internal) ? return_pc : u64{selected_call};
    std::memcpy(runtime.GetUniformBuffer().data() + 16, &choice, sizeof(choice));
    if (indirect) std::memcpy(runtime.GetUniformBuffer().data() + 32, &call_target_pc, sizeof(call_target_pc));
    state->rsb_pointer = empty;
    state->host_cpu_flags = 0xa0000000u;
    phase = 1;
    runtime.SetLocation(source_pc);
    const auto halt = runtime.Run();
    const u64 returned_request = std::atomic_ref<u64>(state->exit_request).load(std::memory_order_acquire);
    const auto frame_count = empty - state->rsb_pointer;
    const auto fault_pc = reinterpret_cast<const u8*>(observed.pc.load());
    std::printf("latch=%.*s request=%u halt=%u pc=%llx source=%llu target=%llu faults=%u frame_count=%td "
                "request_bits=%llx fault_pc=%p x14=%llx x25=%llx\n",
                static_cast<int>(mode.size()), mode.data(), explicit_request, static_cast<unsigned>(halt),
                static_cast<unsigned long long>(runtime.GetLocation()), static_cast<unsigned long long>(source_count),
                static_cast<unsigned long long>(target_count), observed.count.load(), frame_count,
                static_cast<unsigned long long>(callback_request), fault_pc,
                static_cast<unsigned long long>(observed.return_register.load()),
                static_cast<unsigned long long>(observed.rsb_register.load()));
    Check(source_count == 1 && target_count == 0, "one completed source and no target effects");
    Check(halt == (explicit_request ? HaltReason::Retirement : HaltReason::CodeMiss) &&
          runtime.GetLocation() == call_target_pc, "publish selected target and correct exit reason");
    Check((state->host_cpu_flags & 0xf0000000u) == 0x60000000u, "preserve source NZCV on exit");
    u64 result = ~u64{0};
    std::memcpy(&result, runtime.GetUniformBuffer().data() + 40, sizeof(result));
    Check(result == 0, "preserve completed source arithmetic");
    if (explicit_request) {
        Check(observed.count.load() == 0 && (callback_request & kRetirementExitRequest), "explicit request skips physical fault arm");
        Check(returned_request == callback_request, "retirement request remains sticky until Resume");
        Check(callback_status == RetirementControl::DrainStatus::SelfActive &&
              control->CheckDrain(ticket) == RetirementControl::DrainStatus::Ready, "explicit request drains real execution");
    } else if (indirect) {
        Check(callback_request != 0 && (callback_request >> 62) == 0, "real low-bit SMC request reaches dynamic dispatch");
        Check(returned_request == 0, "completed SMC generation is acknowledged on return");
        Check(observed.count.load() == 0, "dynamic Call arm has no protected-latch poll and takes no latch fault");
        Check(!space.GetLinkManager().QueryTarget(target_pc) && space.GetLinkManager().QueryTarget(source_pc),
              "SMC detaches target while retaining executing source");
    } else {
        Check(callback_request != 0 && (callback_request >> 62) == 0, "real low-bit SMC request reaches poll load");
        Check(returned_request == 0, "completed SMC generation is acknowledged on return");
        Check(observed.count.load() == 1 && observed.address.load() == observed.latch, "one physical protected-latch fault");
        FaultEntry actual;
        Check(space.LookupFault(fault_pc, actual) && actual.owner_start == source_allocation.host_start &&
              actual.guest_loc == call_target_pc && actual.recovery, "actual signal PC resolves selected source-owned recovery");
        Check(fault_pc == polls[mixed && !selected_call ? 1 : 0].host_start, "physical fault selects correct static arm");
        if (selected_call) {
            Check(observed.return_register.load() == return_pc &&
                  observed.rsb_register.load() == reinterpret_cast<uintptr_t>(empty), "Call fault preserves live x14 and unpushed x25");
        }
        Check(!space.GetLinkManager().QueryTarget(call_target_pc) && space.GetLinkManager().QueryTarget(source_pc),
              "SMC detaches target while retaining executing source");
    }
    Check(state->rsb_pointer == (selected_call ? empty - 1 : empty), "single external call frame on physical poll recovery");
    if (selected_call) Check((empty - 1)->guest_location == return_pc && (empty - 1)->dispatch_index == 0,
                             "exact external return frame contents");
    constexpr u32 push_frame = 0xa9bf7f2eu; // STP X14, XZR, [X25, #-16]!
    if (call && !polls.empty()) {
        Check(Instruction(polls[0].recovery) == push_frame, "Call recovery starts with frame publication");
        Check(Instruction(polls[0].host_start - 8) == push_frame, "Call request has its own existing frame publication");
    }
    for (size_t index = 0; index < polls.size(); ++index) {
        Check(BranchTarget(polls[index].host_start - 4) == polls[index].recovery + (call && index == 0 ? 4 : 0),
              "request B skips only its Call fault frame wrapper");
    }
    if (mixed) Check(polls[0].recovery + 4 == polls[1].recovery && Instruction(polls[1].recovery) != push_frame,
                     "mixed Call and Branch recover independently through one common exit");
    VerifyPollCodec(source_allocation, polls, source_pc, call, mixed);
    if (explicit_request) {
        Check(control->SealPublications(ticket) && control->Resume(ticket), "release explicit request test gate");
        Check(std::atomic_ref<u64>(state->exit_request).load(std::memory_order_acquire) == 0,
              "Resume clears the acknowledged retirement request");
        ticket = {};
    }
    if (disk_restored) Check(space.GetJitDiskCache()->Stats().units_compiled.load() == 0,
          "all warm executions and the actual poll exit use restored code without compilation");
    control.reset();
    active_space = nullptr;
    active_runtime = nullptr;
}

void DiskChild(const char* executable, const char* mode, const char* stage, const char* directory) {
    const auto pid = fork();
    Check(pid >= 0, "fork independent disk latch phase");
    if (!pid) {
        setenv("STATIC_LATCH_DISK_PHASE", stage, 1);
        setenv("STATIC_LATCH_DISK_DIR", directory, 1);
        execl(executable, executable, mode, static_cast<char*>(nullptr));
        _exit(127);
    }
    int status{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result < 0) throw std::runtime_error("observe independent disk latch child");
        if (result == pid) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            throw std::runtime_error("independent disk latch child timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "independent disk latch phase passes full native assertions");
}

void DiskMatrix(const char* mode) {
    std::array<char, 4096> executable{};
    Check(readlink("/proc/self/exe", executable.data(), executable.size() - 1) > 0, "locate executable for fresh disk latch phases");
#if defined(__ANDROID__)
    char directory[] = "/data/local/tmp/swiftvm-disk-latch.XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-disk-latch.XXXXXX";
#endif
    Check(mkdtemp(directory) != nullptr, "allocate isolated production disk latch cache");
    const auto cleanup = ScopeExit([&] { std::filesystem::remove_all(directory); });
    DiskChild(executable.data(), mode, "write", directory);
    DiskChild(executable.data(), mode, "read", directory);
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected latch terminal mode");
        const char* stage = std::getenv("STATIC_LATCH_DISK_PHASE");
        if (stage) {
            const auto* directory = std::getenv("STATIC_LATCH_DISK_DIR");
            Check(directory && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0, "set isolated disk latch cache before configuration");
        }
        SmcTracker::SetEnabled(true);
        if (!stage && std::string_view{argv[1]}.starts_with("disk-")) DiskMatrix(argv[1]);
        else Run(argv[1]);
        if (stage) {
            std::printf("disk_phase=%s pid=%d checks=%u passed\n", stage, getpid(), checks);
            return 0;
        }
        std::printf("mode=%s checks=%u passed\n", argv[1], checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "static-latch failure: %s\n", error.what());
        return 1;
    }
}
