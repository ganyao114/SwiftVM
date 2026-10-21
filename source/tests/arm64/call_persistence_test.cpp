#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <elfio/elfio.hpp>

#include "aot/aot_call.h"
#include "aot/aot_compile.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/jit_cache.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/signal_handler.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/hir_builder.h"

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
unsigned checks{};
void Check(bool condition, const char* detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}
constexpr u64 source_pc = 0x100, return_pc = 0x180, target_pc = 0x1100;
constexpr u64 returned = 0x23456789abcd;
struct MissObservation {
    std::atomic<unsigned> count{};
    std::atomic<uintptr_t> pc{}, link_register{};
};
bool ObserveMiss(void* opaque, ucontext_t* context, int signal, siginfo_t*) {
    if (signal == SIGSEGV || signal == SIGBUS) {
        auto& observed = *static_cast<MissObservation*>(opaque);
        observed.pc.store(SignalHandler::GetContextPC(context), std::memory_order_relaxed);
        observed.link_register.store(SignalHandler::GetContextGPR(context, 30), std::memory_order_relaxed);
        observed.count.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

void CompileTarget(const std::shared_ptr<Module>& module) {
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{target_pc}, Location{target_pc + 4});
    const auto input = function->LoadUniform(Uniform{8, ValueType::U64}).SetType(ValueType::U64);
    const auto one = function->LoadImm(Imm{u64{1}}).SetType(ValueType::U64);
    const auto sum = function->Add(input, Operand{one}).SetType(ValueType::U64);
    function->SaveFlags(sum, Flags::All);
    function->StoreUniform(Uniform{0, ValueType::U64}, sum);
    function->AdvancePC(Imm{u64{1}});
    auto* tail = builder.LinkBlock(terminal::LinkBlock{Location{target_pc + 2}});
    builder.SetCurBlock(tail);
    const auto resume = function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{16, ValueType::U64});
    function->SetLocation(Lambda{resume});
    function->EndBlock(terminal::PopRSBHint{});
    function->EndFunction();
    Check(TranslateIR(module, function) != nullptr, "compile real flags-producing returning target");
}

void CompileSource(const std::shared_ptr<Module>& module, bool indirect) {
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{source_pc}, Location{return_pc + 1});
    function->StoreUniform(Uniform{24, ValueType::U64},
                          function->LoadImm(Imm{u64{0}}).SetType(ValueType::U64));
    const auto left = function->LoadImm(Imm{~u64{0}}).SetType(ValueType::U64);
    const auto one = function->LoadImm(Imm{u64{1}}).SetType(ValueType::U64);
    const auto sum = function->Add(left, Operand{one}).SetType(ValueType::U64);
    function->SaveFlags(sum, Flags::All);
    const auto resume = function->LoadImm(Imm{return_pc}).SetType(ValueType::U64);
    const auto target = indirect
            ? Lambda{function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{32, ValueType::U64})}
            : Lambda{Imm{target_pc}};
    function->CallReturn(resume, Imm{return_pc}, target);
    function->SetLocation(target);
    builder.RegisterCallReturn(Location{return_pc});
    function->EndBlock(terminal::ReturnToDispatch{});
    builder.SetCurBlock(Location{return_pc});
    function->StoreUniform(Uniform{24, ValueType::U64},
                          function->LoadImm(Imm{returned}).SetType(ValueType::U64));
    function->AdvancePC(Imm{u64{1}});
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    Check(TranslateIR(module, function) != nullptr, "compile real static or indirect Call and guest return body");
}

aot::AotImage CaptureCalls(AddressSpace& space, void* guest, size_t bytes) {
    const auto module = space.GetDefaultModule();
    aot::AotImage image;
    image.host_isa = kArm64;
    image.guest.entry = source_pc;
    image.key = {kCacheFormatVersion, ComputeBuildId(), ComputeConfigHash(space.GetConfig()), ComputeEnvHash(),
                 aot::HashGuestImage(image.segments, image.guest.entry)};
    for (const u64 pc : {source_pc, target_pc}) {
        const auto node = module->GetNode(Location{pc});
        Check(IsFunction(node), "capture production function owner");
        const auto function = GetFunction(node);
        auto* allocation = static_cast<u8*>(module->GetJitCache(function->GetJitCache()));
        FaultEntry extent;
        Check(module->LookupCodeAllocation(allocation, extent), "resolve whole production allocation");
        SerialUnit captured;
        std::string error;
        Check(CaptureSerialCode(*module, allocation, u32(extent.host_end - extent.host_start),
              GetHostImage(), bytes, captured, &error), error.c_str());
        aot::AotUnit unit;
        unit.guest_start = pc;
        unit.code_offset = u32(image.code.size());
        unit.code_size = u32(captured.code.size());
        unit.relocs = std::move(captured.relocs);
        unit.fault_sites = std::move(captured.fault_sites);
        unit.link_sites = std::move(captured.link_sites);
        unit.region_branches = std::move(captured.region_branches);
        const auto offset = [&](const void* pointer) -> u32 {
            if (!pointer) return UINT32_MAX;
            const auto* address = static_cast<const u8*>(pointer);
            Check(address >= allocation && address < extent.host_end, "captured entry belongs to its owner");
            return u32(address - allocation);
        };
        for (const auto& member : function->GetBlocks()) {
            const auto start = member.GetStartLocation().Value();
            const auto end = std::max(start + 1, member.GetEndLocation().Value());
            SerialBlock block{start, end, UINT32_MAX,
                HashBytes(static_cast<const u8*>(guest) + start, end - start, 0xCBF29CE484222325ull)};
            if (const auto target = space.GetLinkManager().QueryTarget(start)) {
                block.code_offset = offset(target->host_pc);
                block.direct_code_offset = offset(target->direct_host_pc);
                block.pending_flags_code_offset = offset(target->pending_flags_host_pc);
                block.pending_flags_contract = target->pending_flags_contract;
                block.continuation_code_offset = offset(target->call_host_pc);
                block.pending_flags_continuation_code_offset = offset(target->call_pending_flags_host_pc);
            } else if (auto* entry = space.GetCodeCache(Location{start})) {
                block.code_offset = offset(entry);
                block.entry_flags = 0;
            } else block.entry_flags = SerialBlock::MetadataOnly;
            Check(block.ValidEntryContract(unit.code_size), "capture all actual function entry contracts");
            unit.blocks.push_back(block);
        }
        image.code.insert(image.code.end(), captured.code.begin(), captured.code.end());
        image.units.push_back(std::move(unit));
    }
    space.GetCodeCacheTable().ForEachEntry([&](u32 index, size_t key, size_t) {
        image.dispatch_slots.emplace_back(u64(key), index);
    });
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    return image;
}

void AotCalls(bool indirect) {
    const auto bytes = 16 * static_cast<size_t>(getpagesize());
    void* source_guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(source_guest != MAP_FAILED && guest != MAP_FAILED, "map separate source and destination guest windows");
    const auto release = ScopeExit([&] { munmap(source_guest, bytes); munmap(guest, bytes); });
    std::memset(source_guest, 0xa5, bytes);
    std::memset(guest, 0xa5, bytes);
    Config config{};
    config.loc_start = 0;
    config.loc_end = bytes;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = 64;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    config.memory_base = source_guest;
    config.guest_addr_mask = bytes - 1;
    auto source = std::make_unique<AddressSpace>(config);
    source->SetSerializableCodeEmission();
    CompileTarget(source->GetDefaultModule());
    CompileSource(source->GetDefaultModule(), indirect);
    const auto image = CaptureCalls(*source, source_guest, bytes);
    const auto& source_unit = image.units.front();
    Check(indirect ? source_unit.link_sites.empty() : source_unit.link_sites.size() == 1 &&
          source_unit.link_sites.front().kind == u8(LinkSiteKind::Call),
          "production serializable emitter captures the actual static or indirect Call shape");
    Check(indirect || !FlagsRegsEnabled() || source_unit.link_sites.front().HasFlagsBypass(),
          "flags1 static Call carries its real physical flags-bypass recipe");
    const auto blob = aot::EncodeInfoBlob(image);
    aot::AotImage restored;
    std::string error;
    Check(aot::DecodeInfoBlob(blob.data(), blob.size(), restored, error), error.c_str());
    restored.host_isa = image.host_isa;
    restored.code = image.code;
    config.memory_base = guest;
    AddressSpace destination{config};
    aot::LoadReport report;
    Check(aot::InstallArtifact(destination, restored, guest, config.guest_addr_mask, report, error) == aot::kAotOk,
          error.c_str());
    Check(report.units_installed == 2 && report.mapping, "install exactly both captured Call units");
    for (const u64 pc : {source_pc, target_pc}) Check(destination.GetCodeCache(Location{pc}) &&
          destination.GetCodeCache(Location{pc}) != source->GetCodeCache(Location{pc}),
          "installed RX identity differs while both source allocations are still alive");
    source.reset();
    const auto module = destination.GetDefaultModule();
    auto* source_base = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, source_unit.code_offset));
    const auto target = destination.GetLinkManager().QueryTarget(target_pc);
    Check(target && target->call_host_pc && (!FlagsRegsEnabled() || target->call_pending_flags_host_pc),
          "AOT publishes actual physical Call and pending Call target entries");
    for (const auto& unit : restored.units) for (const auto& block : unit.blocks) {
        if (!block.IsPublished()) continue;
        auto* base = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, unit.code_offset));
        const auto pointer = [&](u32 offset) -> void* { return offset == UINT32_MAX ? nullptr : base + offset; };
        Check(destination.GetCodeCache(Location{block.guest_start}) == pointer(block.code_offset) &&
              reinterpret_cast<void*>(destination.GetCallCodeCacheTable().Lookup(block.guest_start)) == pointer(block.continuation_code_offset) &&
              reinterpret_cast<void*>(destination.GetPendingCallCodeCacheTable().Lookup(block.guest_start)) == pointer(block.pending_flags_continuation_code_offset),
              "installed canonical and both Call tables match serialized entries exactly");
    }
    for (unsigned iteration = 0; iteration < 4; ++iteration) {
        const u64 input = iteration & 1 ? ~u64{0} : u64{36};
        const bool miss = indirect && iteration >= 2;
        if (miss) Check(destination.GetCallCodeCacheTable().Zero(target_pc) &&
                        destination.GetPendingCallCodeCacheTable().Zero(target_pc), "remove both physical indirect targets");
        Runtime runtime{&destination};
        MissObservation observed;
        SignalHandler::RegisterHandler(&ObserveMiss, &observed, -1);
        const auto unregister = ScopeExit([&] { SignalHandler::UnregisterHandler(&observed); });
        const u64 resume = return_pc, target_address = target_pc;
        std::memcpy(runtime.GetUniformBuffer().data() + 8, &input, 8);
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &resume, 8);
        std::memcpy(runtime.GetUniformBuffer().data() + 32, &target_address, 8);
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == HaltReason::CallHost, "AOT Call returns after source AddressSpace destruction");
        u64 value{}, witness{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 24, 8);
        Check(value == input + 1 && witness == returned, "restored Call result and guest return body are independently correct");
        Check((runtime.GetState()->host_cpu_flags & 0xf0000000u) == (value == 0 ? 0x60000000u : 0),
              "restored physical Call preserves independent callee NZCV");
        Check(runtime.GetState()->rsb_empty && runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty,
              "restored Call balances its external frame");
        Check(observed.count.load() == (miss ? 1u : 0u), "restored Call takes exactly its intended native missing-entry faults");
        if (miss) {
            FaultEntry fault;
            Check(module->LookupFault(reinterpret_cast<const u8*>(observed.link_register.load() - 4), fault) &&
                  fault.recovery_kind == FaultRecoveryKind::IndirectCallMiss && fault.recovery && fault.guest_loc == source_pc,
                  "AOT preserves the exact actual IndirectCallMiss recipe");
            Check(destination.PushCallCodeCache(Location{target_pc}, target->call_host_pc) &&
                  destination.PushPendingCallCodeCache(Location{target_pc}, target->call_pending_flags_host_pc),
                  "restore both actual target entries after native fault");
        }
    }
    const auto sites = destination.GetLinkManager().QuerySourceSites({module.get(), source_base});
    Check(indirect ? sites.empty() : sites.size() == 1 && sites.front().kind == LinkSiteKind::Call &&
          sites.front().state == LinkSiteState::Linked, "restored static Call really patches its live source");
    destination.InvalidateCodeRange(target_pc, target_pc + 4);
    Check(!destination.GetCodeCache(Location{target_pc}) && !destination.GetCallCodeCacheTable().Lookup(target_pc) &&
          !destination.GetPendingCallCodeCacheTable().Lookup(target_pc) &&
          !aot::ResolveCodeOffset(report.mapping, restored.units.back().code_offset) &&
          aot::ResolveCodeOffset(report.mapping, source_unit.code_offset) == source_base,
          "target range retirement revokes every Call alias while retaining source ownership");
    const auto retired_sites = destination.GetLinkManager().QuerySourceSites({module.get(), source_base});
    Check(indirect || retired_sites.size() == 1 && retired_sites.front().state == LinkSiteState::Unlinked,
          "target retirement unlinks the restored physical Call");
    auto malformed = restored;
    malformed.units.back().relocs.push_back({.code_offset = malformed.units.back().code_size,
        .inst_count = 1, .kind = RelocKind::HostImageAbs64});
    {
        AddressSpace rejected{config};
        aot::LoadReport rejected_report;
        Check(aot::InstallArtifact(rejected, malformed, guest, config.guest_addr_mask, rejected_report, error) == aot::kAotRelocFailed &&
              !rejected_report.units_installed && !rejected_report.mapping && !rejected.GetCodeCache(Location{source_pc}) &&
              !rejected.GetCallCodeCacheTable().Lookup(source_pc) && !rejected.GetLinkManager().QueryTarget(source_pc),
              "bad later-unit relocation rejects before earlier Call publication");
    }
    std::printf("aot_call indirect=%u units=2 executions=4 faults=%u distinct_rx=1 source_destroyed=1 retired=1\n",
                indirect, indirect ? 2u : 0u);
}

void AotReject() {
    const auto bytes = 16 * static_cast<size_t>(getpagesize());
    void* source_guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    void* guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(source_guest != MAP_FAILED && guest != MAP_FAILED, "map separate source and destination guest windows");
    const auto release = ScopeExit([&] { munmap(source_guest, bytes); munmap(guest, bytes); });
    std::memset(source_guest, 0xa5, bytes);
    std::memset(guest, 0xa5, bytes);
    Config config{};
    config.loc_start = 0;
    config.loc_end = bytes;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = 64;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    config.memory_base = source_guest;
    config.guest_addr_mask = bytes - 1;
    auto source = std::make_unique<AddressSpace>(config);
    source->SetSerializableCodeEmission();
    CompileTarget(source->GetDefaultModule());
    CompileSource(source->GetDefaultModule(), false);
    const auto image = CaptureCalls(*source, source_guest, bytes);
    const auto blob = aot::EncodeInfoBlob(image);
    aot::AotImage pristine;
    std::string error;
    Check(aot::DecodeInfoBlob(blob.data(), blob.size(), pristine, error), error.c_str());
    pristine.host_isa = image.host_isa;
    pristine.code = image.code;
    source.reset();
    config.memory_base = guest;
    const auto entry_block = [](aot::AotImage& candidate, u64 pc) -> SerialBlock* {
        for (auto& unit : candidate.units) for (auto& block : unit.blocks)
            if (block.guest_start == pc && block.IsPublished()) return &block;
        return nullptr;
    };
    const auto expect_rejected = [&](aot::AotImage& candidate, const char* why) {
        AddressSpace destination{config};
        aot::LoadReport report;
        std::string reject_error;
        const auto status = aot::InstallArtifact(destination, candidate, guest, config.guest_addr_mask, report, reject_error);
        Check(status != aot::kAotOk && !report.units_installed && !report.mapping &&
              !destination.GetCodeCache(Location{source_pc}) && !destination.GetCodeCache(Location{target_pc}) &&
              !destination.GetCallCodeCacheTable().Lookup(target_pc) &&
              !destination.GetPendingCallCodeCacheTable().Lookup(target_pc) &&
              !destination.GetLinkManager().QueryTarget(source_pc) &&
              !destination.GetLinkManager().QueryTarget(target_pc), why);
    };
    unsigned rejected{};
    const auto mutate_target = [&](auto&& mutate, const char* why) {
        auto candidate = pristine;
        auto* block = entry_block(candidate, target_pc);
        Check(block != nullptr, "locate published target entry for contract mutation");
        mutate(*block, candidate);
        expect_rejected(candidate, why);
        ++rejected;
    };
    mutate_target([](SerialBlock& block, aot::AotImage& candidate) {
        block.continuation_code_offset = u32(candidate.units[1].code_size);
    }, "continuation offset at the code-size boundary rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.pending_flags_continuation_code_offset |= 1u;
    }, "misaligned pending continuation offset rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.pending_flags_contract.observed_nzcv_mask = 0x1f;
    }, "observed NZCV mask outside the NZCV nibble rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.pending_flags_contract.overwrite_before_observe = 0x08000000u;
    }, "overwrite mask outside host NZCV rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.pending_flags_contract.commits_before_fault = false;
    }, "pending entry without commit-before-fault rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.pending_flags_code_offset = UINT32_MAX;
    }, "pending continuation without a pending entry rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.code_offset = UINT32_MAX;
    }, "published entry without a canonical offset rejects before publication");
    mutate_target([](SerialBlock& block, aot::AotImage&) {
        block.entry_flags = u8(block.entry_flags | 0x80u);
    }, "unknown entry flag bits reject before publication");
    {
        auto candidate = pristine;
        Check(!candidate.units.front().link_sites.empty(), "static Call capture carries its serial site");
        candidate.units.front().link_sites.front().kind = 0xff;
        expect_rejected(candidate, "invalid serial Call site kind rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        auto* block = entry_block(candidate, target_pc);
        Check(block != nullptr, "locate published target entry for ownership mutation");
        candidate.units.front().blocks.push_back(*block);
        expect_rejected(candidate, "duplicate published Call entry ownership rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.host_isa = kRiscv64;
        expect_rejected(candidate, "foreign host ISA rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.key.format_version = kCacheFormatVersion - 1;
        expect_rejected(candidate, "stale cache format rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.key.build_id ^= 1;
        expect_rejected(candidate, "foreign build id rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.key.config_hash ^= 1;
        expect_rejected(candidate, "foreign runtime config rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.key.env_hash ^= 1;
        expect_rejected(candidate, "foreign environment hash rejects before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        candidate.guest.entry += 4;
        expect_rejected(candidate, "edited guest entry rejects before publication");
        ++rejected;
    }
    {
        auto* bytes_guest = static_cast<u8*>(guest);
        const u8 original = bytes_guest[source_pc];
        bytes_guest[source_pc] = u8(original ^ 0xffu);
        expect_rejected(pristine, "mutated guest bytes reject before publication");
        bytes_guest[source_pc] = original;
        ++rejected;
    }
    {
        AddressSpace destination{config};
        ModuleConfig module_config{};
        module_config.optimizations = Optimizations::ReturnStackBuffer;
        Check(destination.MapModule(0, bytes, module_config) != nullptr,
              "map a direct-link-disabled module over the destination window");
        aot::LoadReport report;
        std::string reject_error;
        const auto status = aot::InstallArtifact(destination, pristine, guest, config.guest_addr_mask, report, reject_error);
        Check(status != aot::kAotOk && !report.units_installed && !report.mapping &&
              !destination.GetCodeCache(Location{source_pc}) && !destination.GetCodeCache(Location{target_pc}) &&
              !destination.GetCallCodeCacheTable().Lookup(target_pc) &&
              !destination.GetPendingCallCodeCacheTable().Lookup(target_pc),
              "a module without direct-link configuration rejects serial Call metadata before publication");
        ++rejected;
    }
    {
        auto candidate = pristine;
        auto* block = entry_block(candidate, target_pc);
        Check(block != nullptr && block->pending_flags_contract.CanPublishPendingEntry(),
              "locate a pending-capable target contract for the compatibility case");
        block->pending_flags_contract.observed_nzcv_mask = 0xf;
        AddressSpace destination{config};
        aot::LoadReport report;
        Check(aot::InstallArtifact(destination, candidate, guest, config.guest_addr_mask, report, error) == aot::kAotOk &&
              report.units_installed == 2, error.c_str());
        const auto module = destination.GetDefaultModule();
        auto* source_base = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, candidate.units.front().code_offset));
        for (unsigned iteration = 0; iteration < 2; ++iteration) {
            Runtime runtime{&destination};
            const u64 input = 36, resume = return_pc;
            std::memcpy(runtime.GetUniformBuffer().data() + 8, &input, 8);
            std::memcpy(runtime.GetUniformBuffer().data() + 16, &resume, 8);
            runtime.SetLocation(source_pc);
            Check(runtime.Run() == HaltReason::CallHost, "contract-incompatible pending Call still executes correctly");
            u64 value{}, witness{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&witness, runtime.GetUniformBuffer().data() + 24, 8);
            Check(value == input + 1 && witness == returned &&
                  (runtime.GetState()->host_cpu_flags & 0xf0000000u) == 0,
                  "canonical fallback preserves Call result, return body and NZCV");
        }
        const auto sites = destination.GetLinkManager().QuerySourceSites({module.get(), source_base});
        Check(sites.size() == 1 && sites.front().state == LinkSiteState::Linked && !sites.front().pending_flags_compatible,
              "observing contract links canonically without pending admission");
    }
    {
        AddressSpace destination{config};
        aot::LoadReport report;
        Check(aot::InstallArtifact(destination, pristine, guest, config.guest_addr_mask, report, error) == aot::kAotOk &&
              report.units_installed == 2, "pristine artifact still installs after every rejection");
        Runtime runtime{&destination};
        const u64 input = 36, resume = return_pc;
        std::memcpy(runtime.GetUniformBuffer().data() + 8, &input, 8);
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &resume, 8);
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == HaltReason::CallHost, "pristine Call executes after the rejection matrix");
        u64 value{}, witness{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 24, 8);
        Check(value == input + 1 && witness == returned, "pristine result and return body stay correct");
    }
    std::printf("aot_reject cases=%u rejected=%u compatible=1 pristine=1\n", rejected + 2, rejected);
}

void AotProducer() {
#if defined(__ANDROID__)
    char directory[] = "/data/local/tmp/swiftvm-aot-call.XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-aot-call.XXXXXX";
#endif
    Check(mkdtemp(directory) != nullptr, "create owned real AOT producer fixture directory");
    const auto cleanup = ScopeExit([&] { std::filesystem::remove_all(directory); });
    const std::string guest_path = std::string{directory} + "/guest.elf";
    const std::string artifact_path = std::string{directory} + "/guest.aot";
    using namespace ELFIO;
    elfio guest;
    guest.create(ELFCLASS64, ELFDATA2LSB);
    guest.set_os_abi(ELFOSABI_LINUX);
    guest.set_type(ET_EXEC);
    guest.set_machine(EM_X86_64);
    guest.set_entry(0x401020);
    auto* text = guest.sections.add(".text");
    text->set_type(SHT_PROGBITS);
    text->set_flags(SHF_ALLOC | SHF_EXECINSTR);
    text->set_address(0x401000);
    text->set_addr_align(16);
    std::array<char, 64> code{};
    const unsigned char caller[] = {0xb8, 36, 0, 0, 0, 0xe8, 0xd6, 0xff, 0xff, 0xff, 0xc3};
    const unsigned char callee[] = {0x48, 0x83, 0xc0, 1, 0xc3};
    // Discovery visits the callee first, making the caller's edge cross a
    // real allocation boundary instead of absorbing the callee into its CFG.
    std::memcpy(code.data(), callee, sizeof(callee));
    std::memcpy(code.data() + 32, caller, sizeof(caller));
    text->set_data(code.data(), code.size());
    // ELFIO places a section-less PT_PHDR over the actual header table.
    // Turn that fixture descriptor into a read-only LOAD after layout so the
    // ordinary guest loader can find AT_PHDR in mapped guest memory.
    auto* headers = guest.segments.add();
    headers->set_type(PT_PHDR);
    headers->set_flags(PF_R);
    headers->set_virtual_address(0x400040);
    headers->set_physical_address(0x400040);
    headers->set_align(8);
    auto* load = guest.segments.add();
    load->set_type(PT_LOAD);
    load->set_flags(PF_R | PF_X);
    load->set_virtual_address(0x401000);
    load->set_physical_address(0x401000);
    load->set_align(0x1000);
    load->add_section_index(text->get_index(), 16);
    auto* strings = guest.sections.add(".strtab");
    strings->set_type(SHT_STRTAB);
    auto* table = guest.sections.add(".symtab");
    table->set_type(SHT_SYMTAB);
    table->set_link(strings->get_index());
    table->set_entry_size(guest.get_default_entry_size(SHT_SYMTAB));
    table->set_addr_align(8);
    table->set_info(1);
    string_section_accessor names{strings};
    symbol_section_accessor symbols{guest, table};
    symbols.add_symbol(names, "caller", 0x401020, sizeof(caller), STB_GLOBAL, STT_FUNC, 0, text->get_index());
    symbols.add_symbol(names, "callee", 0x401000, sizeof(callee), STB_GLOBAL, STT_FUNC, 0, text->get_index());
    Check(guest.save(guest_path), "write executable x86 guest with actual static Call and symbols");
    const int fixture = open(guest_path.c_str(), O_WRONLY | O_CLOEXEC);
    Check(fixture >= 0, "open own ELF fixture for header mapping descriptor");
    const u32 load_type = PT_LOAD;
    const bool mapped_headers = pwrite(fixture, &load_type, sizeof(load_type), guest.get_segments_offset()) == sizeof(load_type);
    const int closed = close(fixture);
    Check(mapped_headers && closed == 0, "map program header table through a valid read-only LOAD");
    aot::CompileOptions options{.guest_elf = guest_path, .out_path = artifact_path, .verbose = true,
                                .decode_budget = 64, .sweep = true};
    aot::AotStats stats;
    std::string error;
    Check(aot::CompileArtifact(options, stats, error), error.c_str());
    Check(stats.units_emitted >= 2 && !stats.fail_scan && !stats.fail_translate && !stats.symbols_stubbed,
          "production AOT compiler captures every real function without rejected scans or stubs");
    aot::AotGuestEnv env;
    Check(env.Init(guest_path, artifact_path, error), error.c_str());
    Check(env.load_report().units_installed == env.artifact().units.size(), "real artifact installs all producer units");
    size_t calls{};
    for (const auto& unit : env.artifact().units) for (const auto& site : unit.link_sites)
        calls += site.kind == u8(LinkSiteKind::Call);
    if (!calls) {
        std::string detail = "real producer has no physical Call recipes:";
        for (const auto& unit : env.artifact().units) {
            detail += " unit=" + std::to_string(unit.guest_start) + " function=" + std::to_string(unit.is_function) +
                      " blocks=" + std::to_string(unit.blocks.size()) + " links=" + std::to_string(unit.link_sites.size());
            for (const auto& site : unit.link_sites) detail += " kind=" + std::to_string(site.kind);
        }
        throw std::runtime_error(detail);
    }
    Check(calls > 0, "real production producer preserves physical static Call recipes");
    for (const char* name : {"caller", "callee"}) {
        bool mismatch{};
        Check(env.EntryOf(name, mismatch) && !mismatch, "artifact symbols resolve to exact published native entries");
    }
    auto caller_fn = env.Lookup<u64()>("caller");
    for (unsigned iteration = 0; iteration < 3; ++iteration) {
        const auto result = caller_fn.Try();
        Check(result.ok() && result.value() == 37, "actual compiled artifact executes static Call by rewritten symbol");
    }
    std::printf("aot_producer units=%u calls=%zu symbol_calls=3 result=37\n", stats.units_emitted, calls);
}

void Phase(std::string_view phase, bool indirect, const char* directory) {
    Check(FlagsRegsEnabled(), "production Call persistence requires flags1");
    Check(SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0, "set isolated production disk-cache directory");
    const auto bytes = 16 * static_cast<size_t>(getpagesize());
    void* guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(guest != MAP_FAILED, "map child guest image");
    const auto release = ScopeExit([&] { munmap(guest, bytes); });
    std::memset(guest, 0xa5, bytes);
    Config config{};
    config.loc_start = 0;
    config.loc_end = bytes;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = 64;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    config.memory_base = guest;
    config.guest_addr_mask = bytes - 1;
    {
        AddressSpace space{config};
        space.LoadJitCache();
        const auto module = space.GetDefaultModule();
        const auto disk = space.GetJitDiskCache();
        Check(disk && disk->Enabled(), "production disk cache admits complete flags1 Call contract");
        if (phase == "reject") {
            Check(disk->Stats().reject_header.load() == 1 && disk->Stats().units_loaded.load() == 0 &&
                  !space.GetCodeCache(Location{source_pc}) && !space.GetCodeCache(Location{target_pc}) &&
                  !space.GetCallCodeCacheTable().Lookup(target_pc) && !space.GetPendingCallCodeCacheTable().Lookup(target_pc),
                  "old flags1 cache format rejects every canonical and Call publication");
            std::printf("phase=reject pid=%d loaded=0 compiled=0 checks=%u\n", getpid(), checks);
            return;
        }
        if (phase == "write") {
            Check(disk->Stats().units_loaded.load() == 0, "writer starts without inherited units");
            CompileTarget(module);
            CompileSource(module, indirect);
            Check(disk->Stats().units_compiled.load() == 2 && disk->Stats().reject_scan.load() == 0,
                  "production recorder captures both real flags1 Call units without scanner rejection");
        } else {
            Check(phase == "read" && disk->Stats().units_loaded.load() == 2 &&
                  disk->Stats().units_compiled.load() == 0 && disk->Stats().reject_reloc.load() == 0 &&
                  disk->Stats().reject_guest_bytes.load() == 0,
                  "fresh process revives exactly two units without compiling or relocation fallback");
        }
        const auto target = space.GetLinkManager().QueryTarget(target_pc);
        Check(target && target->call_host_pc && target->pending_flags_host_pc &&
              target->call_pending_flags_host_pc &&
              target->call_host_pc == reinterpret_cast<void*>(space.GetCallCodeCacheTable().Lookup(target_pc)) &&
              target->call_pending_flags_host_pc == reinterpret_cast<void*>(space.GetPendingCallCodeCacheTable().Lookup(target_pc)),
              "actual target publishes canonical, pending and both physical Call entries");
        auto* source_entry = space.GetCodeCache(Location{source_pc});
        auto* target_entry = space.GetCodeCache(Location{target_pc});
        Check(source_entry && target_entry && space.GetCodeCache(Location{return_pc}),
              "source, target and guest return label are all present before native execution");
        const auto node = module->GetNode(Location{source_pc});
        Check(IsFunction(node), "source unit retains its real function owner");
        auto source_function = GetFunction(node);
        auto* source_allocation = static_cast<u8*>(module->GetJitCache(source_function->GetJitCache()));
        const auto sites = space.GetLinkManager().QuerySourceSites({module.get(), source_allocation});
        Check(indirect ? sites.empty() : sites.size() == 1 && sites.front().kind == LinkSiteKind::Call,
              "recorded source is the expected actual static-Call or indirect shape");
        if (!indirect && phase == "read") Check(sites.front().state == LinkSiteState::Unlinked,
              "fresh process restores static Call in cold unlinked state");
        unsigned iteration{};
        for (const auto input : {u64{36}, ~u64{0}, u64{36}, ~u64{0}}) {
            const bool forced_miss = indirect && iteration >= 2;
            if (forced_miss) Check(space.GetCallCodeCacheTable().Zero(target_pc) &&
                                   space.GetPendingCallCodeCacheTable().Zero(target_pc),
                                   "force actual canonical and pending Call target misses");
            Runtime runtime{&space};
            MissObservation observed;
            SignalHandler::RegisterHandler(&ObserveMiss, &observed, -1);
            const auto unregister = ScopeExit([&] { SignalHandler::UnregisterHandler(&observed); });
            const u64 resume = return_pc, target_address = target_pc;
            std::memcpy(runtime.GetUniformBuffer().data() + 8, &input, 8);
            std::memcpy(runtime.GetUniformBuffer().data() + 16, &resume, 8);
            std::memcpy(runtime.GetUniformBuffer().data() + 32, &target_address, 8);
            runtime.SetLocation(source_pc);
            Check(runtime.Run() == HaltReason::CallHost, "fresh/warm Call executes and returns to native host");
            u64 value{}, return_marker{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&return_marker, runtime.GetUniformBuffer().data() + 24, 8);
            Check(value == input + 1 && return_marker == returned,
                  "actual target result and guest return-body witness are both correct");
            if ((runtime.GetState()->host_cpu_flags & 0xf0000000u) != (value == 0 ? 0x60000000u : 0))
                throw std::runtime_error("Call NZCV mismatch: input=" + std::to_string(input) +
                    " result=" + std::to_string(value) + " flags=" + std::to_string(runtime.GetState()->host_cpu_flags));
            Check((runtime.GetState()->host_cpu_flags & 0xf0000000u) == (value == 0 ? 0x60000000u : 0),
                  "Call revival preserves independent target NZCV through physical return");
            Check(runtime.GetState()->rsb_empty && runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty,
                  "actual static/indirect Call balances its external continuation frame");
            Check(observed.count.load() == (forced_miss ? 1u : 0u),
                  "only explicitly missing indirect entries take one genuine native fault");
            if (forced_miss) {
                FaultEntry recovery;
                Check(module->LookupFault(reinterpret_cast<const u8*>(observed.link_register.load() - 4), recovery) &&
                      recovery.recovery_kind == FaultRecoveryKind::IndirectCallMiss && recovery.recovery &&
                      recovery.guest_loc == source_pc,
                      "actual fault uses the source's persisted precise IndirectCallMiss recovery recipe");
                Check(space.PushCallCodeCache(Location{target_pc}, target->call_host_pc) &&
                      space.PushPendingCallCodeCache(Location{target_pc}, target->call_pending_flags_host_pc),
                      "restore existing target entries after exercising their missing-entry fault path");
            }
            ++iteration;
        }
        Check(space.GetCodeCache(Location{source_pc}) == source_entry && space.GetCodeCache(Location{target_pc}) == target_entry,
              "execution retains restored code identities without recompiling");
        Check(disk->Stats().units_compiled.load() == (phase == "write" ? 2 : 0),
              "all cold and warm executions use the admitted code");
        if (phase == "write") {
            disk->Save();
            Check(disk->Stats().units_stored.load() == 2, "production Save writes both complete Call units");
        } else {
            space.InvalidateCodeRange(target_pc, target_pc + 4);
            Check(!space.GetCodeCache(Location{target_pc}) && !space.GetCallCodeCacheTable().Lookup(target_pc) &&
                  !space.GetPendingCallCodeCacheTable().Lookup(target_pc) && space.GetCodeCache(Location{source_pc}) == source_entry,
                  "target range retirement revokes all restored Call entries while preserving source ownership");
        }
        std::printf("phase=%.*s pid=%d loaded=%llu compiled=%llu checks=%u\n", static_cast<int>(phase.size()), phase.data(),
            getpid(), static_cast<unsigned long long>(disk->Stats().units_loaded.load()),
            static_cast<unsigned long long>(disk->Stats().units_compiled.load()), checks);
    }
}

void Child(const char* executable, const char* mode, const char* phase, const char* directory) {
    const auto pid = fork();
    Check(pid >= 0, "fork independent disk-cache phase");
    if (pid == 0) {
        setenv("CALL_CACHE_PHASE", phase, 1);
        setenv("CALL_CACHE_DIR", directory, 1);
        execl(executable, executable, mode, static_cast<char*>(nullptr));
        _exit(127);
    }
    int status{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
        const auto result = waitpid(pid, &status, WNOHANG);
        if (result < 0) throw std::runtime_error("observe child process status");
        if (result == pid) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            throw std::runtime_error("native disk-cache phase timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "independent writer/reader/rejection process passes full native oracle");
}

void Matrix(const char* mode) {
    std::array<char, 4096> executable{};
    const auto size = readlink("/proc/self/exe", executable.data(), executable.size() - 1);
    Check(size > 0, "locate current executable for fresh exec phases");
    std::array<char, 128> directory{};
#if defined(__ANDROID__)
    std::strcpy(directory.data(), "/data/local/tmp/swiftvm-flags1-call.XXXXXX");
#else
    std::strcpy(directory.data(), "/tmp/swiftvm-flags1-call.XXXXXX");
#endif
    Check(mkdtemp(directory.data()) != nullptr, "allocate private cache directory");
    const auto cleanup = ScopeExit([&] { std::filesystem::remove_all(directory.data()); });
    Child(executable.data(), mode, "write", directory.data());
    Child(executable.data(), mode, "read", directory.data());
    std::filesystem::path cache;
    for (const auto& entry : std::filesystem::directory_iterator(directory.data())) {
        if (entry.path().extension() == ".bin" && entry.path().filename().string().starts_with("svmjit-")) {
            Check(cache.empty(), "one exact cache file is produced");
            cache = entry.path();
        }
    }
    Check(!cache.empty(), "production writer created the persisted cache");
    const int fd = open(cache.c_str(), O_WRONLY | O_CLOEXEC);
    Check(fd >= 0, "open own cache fixture for old-version mutation");
    const u64 previous = kCacheFormatVersion - 1;
    const bool written = pwrite(fd, &previous, sizeof(previous), 8) == sizeof(previous);
    const int closed = close(fd);
    Check(written && closed == 0, "mutate only disk format to previous version");
    Child(executable.data(), mode, "reject", directory.data());
}
}

int main(int argc, char** argv) {
    try {
        const auto* mode = argc > 1 ? argv[1] : "static";
        if (std::string_view{mode} == "aot-static" || std::string_view{mode} == "aot-indirect" ||
            std::string_view{mode} == "aot-producer" || std::string_view{mode} == "aot-reject") {
            if (std::string_view{mode} == "aot-producer") AotProducer();
            else if (std::string_view{mode} == "aot-reject") AotReject();
            else AotCalls(std::string_view{mode} == "aot-indirect");
            std::printf("mode=%s checks=%u passed\n", mode, checks);
            return 0;
        }
        Check(std::string_view{mode} == "static" || std::string_view{mode} == "indirect", "known production Call mode");
        if (const char* phase = std::getenv("CALL_CACHE_PHASE")) {
            const auto directory = std::getenv("CALL_CACHE_DIR");
            Check(directory != nullptr, "child received isolated cache directory");
            Phase(phase, std::string_view{mode} == "indirect", directory);
        } else {
            Matrix(mode);
            std::printf("mode=%s checks=%u passed\n", mode, checks);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "failure: %s\n", error.what());
        return 1;
    }
}
