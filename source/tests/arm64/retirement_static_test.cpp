#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>
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
constexpr u64 source_pc = 0x1000, first_pc = 0x2000, second_pc = 0x3000, return_pc = 0x1100;
unsigned checks{};
std::atomic<u64> source_count{}, target_count{}, target_while_closed{};
unsigned phase{};
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
    if (phase == 1 && count == 64) active_runtime->SignalInterrupt();
    if (phase == 2 && count == 1) {
        callback_ticket = RetirementControl::RequestDrain(control);
        callback_status = control->CheckDrain(callback_ticket);
    }
    return count;
}

u64 TargetMarker() {
    if (phase == 2) target_while_closed.fetch_add(1, std::memory_order_relaxed);
    return target_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

void WriteFlags(HIRFunction* function, bool source, bool pending_source = false) {
    const auto left = function->LoadImm(Imm{source ? ~u64{0} : u64{1}}).SetType(ValueType::U64);
    const auto one = function->LoadImm(Imm{u64{1}}).SetType(ValueType::U64);
    const auto result = function->Add(left, Operand{one}).SetType(ValueType::U64);
    function->SaveFlags(result, pending_source
            ? Flags::Carry | Flags::Overflow | Flags::Zero | Flags::Negate
            : Flags::All);
    function->StoreUniform(Uniform{40, ValueType::U64}, result);
}

Value Marker(HIRFunction* function, bool source, bool overwrite_first = false,
             bool pending_source = false) {
    if (overwrite_first) WriteFlags(function, false);
    const auto address = reinterpret_cast<uintptr_t>(FptrCast(source ? &SourceMarker : &TargetMarker));
    const auto count = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{u64{address}}});
    function->StoreUniform(Uniform{source ? 0u : 8u, ValueType::U64}, count);
    WriteFlags(function, source, pending_source);
    return count;
}

Terminal Branch(Value count, std::string_view mode) {
    const bool taken = mode.ends_with("taken");
    const u64 then_pc = taken ? second_pc : first_pc;
    const u64 else_pc = taken ? first_pc : second_pc;
    if (mode.find("condition") != std::string_view::npos) {
        return terminal::Condition{mode.ends_with("false") ? Cond::NE : Cond::EQ,
                terminal::LinkBlock{Location{then_pc}}, terminal::LinkBlock{Location{else_pc}}};
    }
    return terminal::If{BOOL{count}, terminal::LinkBlock{Location{then_pc}},
                        terminal::LinkBlock{Location{else_pc}}};
}

u8* Compile(const std::shared_ptr<Module>& module, u64 guest, std::string_view mode,
            bool source, bool call, bool pending) {
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{guest}, Location{source && call ? return_pc + 4 : guest + 4});
    if (!source && pending) {
        auto* entry = function->GetCurrentBlock();
        WriteFlags(function, false);
        function->AdvancePC(Imm{u64{1}});
        auto* body = function->AppendBlock(Location{guest + 1});
        function->AddEdge(entry, body);
        function->EndBlock(terminal::LinkBlock{Location{guest + 1}});
        function->SetCurBlock(body);
    }
    const auto count = Marker(function, source, false, source && pending && !call);
    if (!source && call) {
        function->EndBlock(terminal::ReturnToHost{});
    } else if (source && call) {
        const auto ret = function->LoadImm(Imm{return_pc}).SetType(ValueType::U64);
        function->CallReturn(ret, Imm{return_pc}, Lambda{Imm{first_pc}});
        function->SetLocation(Lambda{Imm{first_pc}});
        builder.RegisterCallReturn(Location{return_pc});
        function->EndBlock(terminal::ReturnToDispatch{});
        builder.SetCurBlock(Location{return_pc});
        function->AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::ReturnToHost{});
    } else if (source && (mode.starts_with("if") || mode.starts_with("condition"))) {
        const auto condition = function->And(count, Operand{Imm{u64{1}}}).SetType(ValueType::U64);
        function->EndBlock(Branch(condition, mode));
    } else if (mode == "dispatcher") {
        function->SetLocation(Lambda{Imm{source ? first_pc : source_pc}});
        function->EndBlock(terminal::ReturnToDispatch{});
    } else {
        function->EndBlock(terminal::LinkBlock{Location{source ? first_pc : source_pc}});
    }
    function->EndFunction();
    auto* code = static_cast<u8*>(TranslateIR(module, function));
    Check(code != nullptr, "compile static native unit");
    return code;
}

u8* CompileRegion(const std::shared_ptr<Module>& module, std::string_view mode) {
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    const bool conditional = mode.find("if-") != std::string_view::npos ||
                             mode.find("condition-") != std::string_view::npos;
    auto* function = builder.AppendFunction(Location{source_pc}, Location{second_pc + 4});
    auto* source = function->GetCurrentBlock();
    auto* first = function->AppendBlock(Location{first_pc});
    auto* second = conditional ? function->AppendBlock(Location{second_pc}) : nullptr;
    function->AddEdge(source, first);
    function->AddEdge(first, source);
    if (second) {
        function->AddEdge(source, second);
        function->AddEdge(second, source);
    }
    function->SetCurBlock(source);
    const auto count = Marker(function, true);
    if (conditional) {
        const auto condition = function->And(count, Operand{Imm{u64{1}}}).SetType(ValueType::U64);
        function->EndBlock(Branch(condition, mode));
    } else if (mode == "published") {
        function->EndBlock(terminal::ExternalLinkBlock{Location{first_pc}});
    } else {
        function->EndBlock(terminal::LinkBlock{Location{first_pc}});
    }
    function->SetCurBlock(first);
    (void)Marker(function, false);
    function->EndBlock(terminal::LinkBlock{Location{source_pc}});
    if (second) {
        function->SetCurBlock(second);
        (void)Marker(function, false, true);
        function->EndBlock(terminal::LinkBlock{Location{source_pc}});
    }
    function->EndFunction();
    auto* code = static_cast<u8*>(TranslateIR(module, function));
    Check(code != nullptr, "compile actual multi-block region");
    return code;
}

void VerifyPatch(const std::shared_ptr<Module>& module, AddressSpace& space, u8* code,
                 bool call, bool pending, bool two_arms) {
    const auto sites = space.GetLinkManager().QuerySourceSites({module.get(), code});
    Check(sites.size() == (two_arms ? 2u : 1u), "actual static source site inventory");
    unsigned warmed{};
    for (const auto& site : sites) {
        if (site.state != LinkSiteState::Linked && site.state != LinkSiteState::Far) continue;
        const auto region = module->GetCodeRegion(site.site.region_id);
        Check(bool(region), "retain real patch region");
        u32 instruction{};
        std::memcpy(&instruction, region->rx_base + site.site.offset, sizeof(instruction));
        Check(instruction != site.unlinked_instruction, "actual RX site is patched");
        if (call) Check(site.kind == LinkSiteKind::Call, "warm static call uses continuation traversal");
        if (pending) Check(site.pending_flags_compatible, "static transfer has actual pending-flags admission");
        ++warmed;
    }
    Check(warmed != 0, "a real static site was warmed");
    std::printf("warm_static sites=%zu warmed=%u call=%u pending=%u\n", sites.size(), warmed, call, pending);
}

void Run(std::string_view mode) {
    const bool call = mode == "call" || mode == "call-pending";
    const bool pending = mode.find("pending") != std::string_view::npos;
    const bool region = mode.starts_with("region") || mode == "published";
    const bool two_arms = mode.starts_with("if") || mode.starts_with("condition");
    Check(!SmcTracker::IsEnabled(), "SMC disabled");
    Check(!BackedgeLatchEnabled(), "optional latch disabled");
    if (call) Check(FlagsRegsEnabled(), "continuation requires flags registers");
    if (pending && !call) Check(FlagsRegsEnabled(), "conditional pending-flags fixture requires flags registers");
    Config config{};
    config.loc_start = source_pc;
    config.loc_end = 0x4000;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = 64;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::BlockLink;
    if (call || pending) config.global_opts |= Optimizations::ReturnStackBuffer;
    config.region_edges = region || pending;
    AddressSpace space{config};
    ModuleConfig module_config{};
    module_config.optimizations = config.global_opts;
    auto module = space.MapModule(source_pc, 0x4000, module_config);
    Check(bool(module), "map static test module");
    u8* source{};
    if (region) {
        source = CompileRegion(module, mode);
    } else {
        (void)Compile(module, first_pc, mode, false, call, pending);
        if (two_arms) (void)Compile(module, second_pc, mode, false, false, pending);
        source = Compile(module, source_pc, mode, true, call, pending);
    }
    Runtime runtime{&space};
    active_runtime = &runtime;
    control = space.GetRetirementControl();
    auto* empty = runtime.GetState()->rsb_pointer;
    phase = call ? 0 : 1;
    for (unsigned warm = 0; warm < (call ? 2u : 1u); ++warm) {
        runtime.GetState()->rsb_pointer = empty;
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == (call ? HaltReason::CallHost : HaltReason::Signal), "warm native boundary");
        runtime.ClearInterrupt();
    }
    Check(source_count.load() > 0 && target_count.load() > 0, "both warm bodies actually executed");
    if (!region) VerifyPatch(module, space, source, call, pending, two_arms);
    runtime.GetState()->rsb_pointer = empty;
    runtime.GetState()->host_cpu_flags = 0xa0000000u;
    source_count.store(0);
    target_count.store(0);
    phase = 2;
    runtime.SetLocation(source_pc);
    const auto halt = runtime.Run();
    const u64 expected_pc = mode.ends_with("taken") || mode.ends_with("false") ? second_pc : first_pc;
    std::printf("static=%.*s halt=%u pc=%llx source=%llu target=%llu while_closed=%llu rescue_interrupt=0\n",
            static_cast<int>(mode.size()), mode.data(), static_cast<unsigned>(halt),
            static_cast<unsigned long long>(runtime.GetLocation()),
            static_cast<unsigned long long>(source_count.load()),
            static_cast<unsigned long long>(target_count.load()),
            static_cast<unsigned long long>(target_while_closed.load()));
    Check(callback_status == RetirementControl::DrainStatus::SelfActive, "callback drain defers own lease");
    Check(source_count.load() == 1 && target_count.load() == 0 && target_while_closed.load() == 0,
          "closed callback prevents every target body");
    Check(halt == HaltReason::Retirement, "static request returns Retirement");
    Check(runtime.GetLocation() == expected_pc, "publish selected static target PC");
    Check((runtime.GetState()->host_cpu_flags & 0xf0000000u) == 0x60000000u, "preserve source arithmetic NZCV");
    u64 result = ~u64{0};
    std::memcpy(&result, runtime.GetUniformBuffer().data() + 40, sizeof(result));
    Check(result == 0, "publish live arithmetic result");
    Check(runtime.GetState()->rsb_pointer == (call ? empty - 1 : empty), "single correct frame consumption/publication");
    if (call) Check((empty - 1)->guest_location == return_pc && (empty - 1)->dispatch_index == 0,
                    "cancelled static call publishes external return frame");
    Check(control->CheckDrain(callback_ticket) == RetirementControl::DrainStatus::Ready, "execution drains");
    Check((runtime.GetState()->exit_request & kRetirementExitRequest) != 0, "request remains sticky");
    Check(runtime.Run() == HaltReason::Retirement && source_count.load() == 1 && target_count.load() == 0,
          "closed Run remains blocked");
    Check(control->SealPublications(callback_ticket), "seal after native drain");
    Check(control->Resume(callback_ticket), "reopen retained code");
    Check((runtime.GetState()->exit_request & kRetirementExitRequest) == 0, "resume clears request");
    active_runtime = nullptr;
    callback_ticket = {};
    control.reset();
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("expected static terminal mode");
        const std::string_view mode{argv[1]};
        if (mode != "direct" && mode != "dispatcher" && mode != "if" && mode != "condition" &&
            mode != "condition-false" && mode != "region" && mode != "published" &&
            mode != "region-if-taken" && mode != "region-if-fall" &&
            mode != "region-condition-taken" && mode != "region-condition-fall" &&
            mode != "if-pending" && mode != "condition-pending" && mode != "condition-pending-false" &&
            mode != "call" && mode != "call-pending") throw std::runtime_error("unknown static terminal mode");
        SmcTracker::SetEnabled(false);
        Run(mode);
        std::printf("mode=%s checks=%u passed\n", argv[1], checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "retirement-static failure: %s\n", error.what());
        return 1;
    }
}
