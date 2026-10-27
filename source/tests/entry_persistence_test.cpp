#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <unistd.h>
#include "base/scope_exit.h"

#include "aot/aot_artifact.h"
#include "aot/aot_load.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/function_entry_contract.h"
#include "runtime/backend/jit_cache.h"
#include "runtime/backend/link_manager.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/hir_builder.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;

namespace {
unsigned checks{};
void Check(bool condition, const char* detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}

SerialUnit Fixture() {
    SerialUnit unit{};
    unit.guest_start = 0x100;
    unit.is_function = 1;
    unit.code.resize(64);
    SerialBlock block{0x100, 0x104, 0, 0x1234, 4, 8,
        {.overwrite_before_observe = kEdgeNZCVMask, .commits_before_fault = true}};
    block.continuation_code_offset = 12;
    block.pending_flags_continuation_code_offset = 16;
    unit.blocks.push_back(block);
    unit.blocks.push_back({.guest_start = 0x200, .guest_end = 0x204,
        .code_offset = UINT32_MAX, .entry_flags = SerialBlock::MetadataOnly});
    unit.blocks.push_back({.guest_start = 0x300, .guest_end = 0x304,
        .code_offset = UINT32_MAX, .entry_flags = SerialBlock::DependencyOnly});
    unit.fault_sites.push_back({0x100, 0, 64, UINT32_MAX, 0});
    return unit;
}

aot::AotImage Image(const SerialUnit& unit) {
    aot::AotImage image{};
    image.key.format_version = kCacheFormatVersion;
    image.guest.entry = unit.guest_start;
    image.code = unit.code;
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    aot::AotUnit record{};
    record.guest_start = unit.guest_start;
    record.is_function = unit.is_function;
    record.code_size = static_cast<u32>(unit.code.size());
    record.blocks = unit.blocks;
    record.fault_sites = unit.fault_sites;
    record.link_sites = unit.link_sites;
    record.region_branches = unit.region_branches;
    image.units.push_back(record);
    return image;
}

bool Disk(const SerialUnit& input, SerialUnit& output) {
    BlobWriter writer;
    WriteUnit(writer, input);
    BlobReader reader{writer.Data().data(), writer.Size()};
    return ReadUnit(reader, output) && reader.Empty();
}

bool Aot(const SerialUnit& input, aot::AotImage& output) {
    const auto blob = aot::EncodeInfoBlob(Image(input));
    std::string error;
    return aot::DecodeInfoBlob(blob.data(), blob.size(), output, error);
}

void Contract(const std::vector<SerialBlock>& blocks, const SerialUnit& input) {
    Check(blocks.size() == input.blocks.size(), "all published and dependency records survive codec");
    const auto& before = input.blocks.front();
    const auto& after = blocks.front();
    Check(after.code_offset == before.code_offset, "canonical entry survives codec");
    Check(after.direct_code_offset == before.direct_code_offset, "direct entry survives codec");
    Check(after.pending_flags_code_offset == before.pending_flags_code_offset, "pending entry survives codec");
    Check(after.continuation_code_offset == before.continuation_code_offset,
          "published-frame continuation entry survives codec");
    Check(after.pending_flags_continuation_code_offset == before.pending_flags_continuation_code_offset,
          "pending published-frame continuation entry survives codec");
    Check(after.pending_flags_contract == before.pending_flags_contract, "pending flags target contract survives codec");
    Check(blocks[1].entry_flags == SerialBlock::MetadataOnly && !blocks[1].IsPublished(),
          "metadata-only record keeps its non-published identity");
    Check(blocks[2].entry_flags == SerialBlock::DependencyOnly && !blocks[2].IsPublished(),
          "dependency-only record keeps its non-published identity");
    const auto restored = FunctionEntryContract::Restore(ir::Location{after.guest_start},
        ir::Location{after.guest_end}, {.canonical = after.code_offset,
            .direct_link = after.direct_code_offset, .pending_flags = after.pending_flags_code_offset,
            .continuation = after.continuation_code_offset,
            .pending_flags_continuation = after.pending_flags_continuation_code_offset},
        after.pending_flags_contract, after.IsLinkable());
    Check(restored.IsWellFormed(static_cast<u32>(input.code.size())), "restored complete target contract is well formed");
    Check(restored.Continuation().continuation == FunctionEntryContinuationRequirement::PublishedFrame &&
          restored.PendingFlagsContinuation().continuation == FunctionEntryContinuationRequirement::PublishedFrame,
          "both restored call entries require the published external frame");
    Check(restored.Canonical().flags == FunctionEntryFlagsRequirement::Canonical &&
          restored.Continuation().flags == FunctionEntryFlagsRequirement::Canonical &&
          restored.PendingFlags().flags == FunctionEntryFlagsRequirement::Pending &&
          restored.PendingFlagsContinuation().flags == FunctionEntryFlagsRequirement::Pending,
          "canonical and pending variants retain their distinct flags requirements");
}

void Reject() {
    Check(kCacheFormatVersion >= 35 && aot::kAotFormatVersion == 6,
          "entry-state artifacts require code format 35 and artifact schema 6");
    const auto input = Fixture();
    using Offset = u32 SerialBlock::*;
    const std::array<Offset, 5> offsets{&SerialBlock::code_offset, &SerialBlock::direct_code_offset,
        &SerialBlock::pending_flags_code_offset, &SerialBlock::continuation_code_offset,
        &SerialBlock::pending_flags_continuation_code_offset};
    const auto rejected = [&](const SerialUnit& bad) {
        SerialUnit disk;
        aot::AotImage artifact;
        Check(!Disk(bad, disk), "disk rejects malformed complete entry contract");
        Check(!Aot(bad, artifact), "AOT rejects malformed complete entry contract");
    };
    {
        auto ranged = input;
        ranged.fault_sites.push_back(
                {0x200, 20, 40, UINT32_MAX, static_cast<u8>(FaultRecoveryKind::GuestRange)});
        SerialUnit disk;
        aot::AotImage artifact;
        Check(Disk(ranged, disk) && disk.fault_sites.back().recovery_kind ==
                                            static_cast<u8>(FaultRecoveryKind::GuestRange),
              "disk codec preserves explicit committed-state block ranges");
        Check(Aot(ranged, artifact) && artifact.units.front().fault_sites.back().recovery_kind ==
                                               static_cast<u8>(FaultRecoveryKind::GuestRange),
              "AOT codec preserves explicit committed-state block ranges");
        auto bad = ranged;
        bad.fault_sites.back().recovery_kind = static_cast<u8>(FaultRecoveryKind::IndirectCallMiss);
        rejected(bad);
    }
    for (u8 kind = 0; kind < u8(SerialRegionBranchKind::Count); ++kind) {
        const auto type = static_cast<SerialRegionBranchKind>(kind);
        const bool masked = type == SerialRegionBranchKind::MergeMask || type == SerialRegionBranchKind::MergeMaskToken;
        for (u8 mask = masked ? 1 : 0; mask < (masked ? 15 : 1); ++mask) {
            auto valid = input;
            valid.region_branches.push_back({20, type, mask});
            const u32 placeholder = valid.region_branches.back().UnlinkedInstruction();
            std::memcpy(valid.code.data() + 20, &placeholder, 4);
            SerialUnit disk;
            aot::AotImage artifact;
            Check(Disk(valid, disk) && disk.region_branches.size() == 1 &&
                  disk.region_branches.front().code_offset == 20 && disk.region_branches.front().kind == type &&
                  disk.region_branches.front().mask == mask, "disk preserves every exact shared region ABI recipe");
            Check(Aot(valid, artifact) && artifact.units.front().region_branches.size() == 1 &&
                  artifact.units.front().region_branches.front().code_offset == 20 &&
                  artifact.units.front().region_branches.front().kind == type &&
                  artifact.units.front().region_branches.front().mask == mask,
                  "AOT preserves every exact shared region ABI recipe");
        }
    }
    for (const auto branch : {
            SerialRegionBranch{1, SerialRegionBranchKind::Return},
            SerialRegionBranch{64, SerialRegionBranchKind::Return},
            SerialRegionBranch{20, SerialRegionBranchKind::Count},
            SerialRegionBranch{20, SerialRegionBranchKind::Return, 1},
            SerialRegionBranch{20, SerialRegionBranchKind::MergeMask, 0},
            SerialRegionBranch{20, SerialRegionBranchKind::MergeMaskToken, 15}}) {
        auto bad = input;
        bad.region_branches.push_back(branch);
        rejected(bad);
    }
    {
        auto bad = input;
        bad.region_branches = {{20, SerialRegionBranchKind::Return}, {20, SerialRegionBranchKind::Return}};
        rejected(bad);
    }
    for (auto offset : offsets) for (const auto value : {u32{1}, u32{64}, u32{68}}) {
        auto bad = input;
        bad.blocks.front().*offset = value;
        rejected(bad);
    }
    for (unsigned fault = 0; fault < 6; ++fault) {
        auto bad = input;
        auto& block = bad.blocks.front();
        if (fault == 0) block.pending_flags_code_offset = UINT32_MAX;
        if (fault == 1) block.pending_flags_contract.commits_before_fault = false;
        if (fault == 2) block.entry_flags = 0;
        if (fault == 3) block.entry_flags |= 0x80;
        if (fault == 4) block.guest_end = block.guest_start - 1;
        if (fault == 5) bad.blocks[1].continuation_code_offset = 12;
        rejected(bad);
    }
    BlobWriter writer;
    WriteUnit(writer, input);
    for (size_t prefix = 0; prefix < writer.Size(); ++prefix) {
        BlobReader reader{writer.Data().data(), prefix};
        SerialUnit output;
        Check(!ReadUnit(reader, output), "every truncated disk record is rejected");
    }
    const auto blob = aot::EncodeInfoBlob(Image(input));
    for (size_t prefix = 0; prefix < blob.size(); ++prefix) {
        aot::AotImage output;
        std::string error;
        Check(!aot::DecodeInfoBlob(blob.data(), prefix, output, error), "every truncated AOT blob is rejected");
    }
    auto old = blob;
    const u64 previous = aot::kAotFormatVersion - 1;
    std::memcpy(old.data() + sizeof(aot::kAotMagic), &previous, sizeof(previous));
    aot::AotImage output;
    std::string error;
    Check(!aot::DecodeInfoBlob(old.data(), old.size(), output, error) &&
          error == "unsupported artifact format version", "old AOT entry schema is rejected before decoding");
}

void PendingOnly(bool artifact) {
    auto input = Fixture();
    input.blocks.front().continuation_code_offset = UINT32_MAX;
    if (artifact) {
        aot::AotImage output;
        Check(Aot(input, output), "AOT admits pending-only continuation without requiring canonical call entry");
        Contract(output.units.front().blocks, input);
    } else {
        SerialUnit output;
        Check(Disk(input, output), "disk admits pending-only continuation without requiring canonical call entry");
        Contract(output.blocks, input);
    }
}

void AotLinks() {
    auto input = Fixture();
    const auto pending = EdgeFlagsState::Pending(kEdgeNZCVMask, EdgeCarryPolarity::Unknown,
                                                 EdgeFlagsProducer::Restore);
    input.link_sites.push_back({40, 0x1000, static_cast<u8>(LinkSiteKind::Call),
        24, 32, 0x10000051u, 0, 28, pending, 0x94000000u});
    const u32 adr = 0x10000051u, branch = 0x14000000u, call = 0x94000000u;
    std::memcpy(input.code.data() + 24, &adr, 4);
    std::memcpy(input.code.data() + 28, &branch, 4);
    std::memcpy(input.code.data() + 40, &call, 4);
    aot::AotImage output;
    Check(Aot(input, output), "AOT decodes complete ARM physical Call and flags-bypass recipe");
    Check(output.units.size() == 1 && output.units.front().link_sites.size() == 1,
          "AOT preserves exact physical Call inventory");
    const auto& actual = output.units.front().link_sites.front();
    const auto& expected = input.link_sites.front();
    Check(actual.code_offset == expected.code_offset && actual.guest_target == expected.guest_target &&
          actual.kind == expected.kind && actual.unlinked_instruction == expected.unlinked_instruction,
          "AOT preserves cold Call location, target, kind and instruction");
    Check(actual.flags_bypass_offset == expected.flags_bypass_offset &&
          actual.flags_bypass_resume_offset == expected.flags_bypass_resume_offset &&
          actual.flags_bypass_instruction == expected.flags_bypass_instruction &&
          actual.flags_bypass_linked_instruction == expected.flags_bypass_linked_instruction &&
          actual.flags_merge_branch_offset == expected.flags_merge_branch_offset &&
          actual.edge_flags == expected.edge_flags, "AOT preserves the complete flags/frame bypass contract");
    for (unsigned fault = 0; fault < 8; ++fault) {
        auto malformed = input;
        auto& site = malformed.link_sites.front();
        if (fault == 0) site.code_offset = 64;
        if (fault == 1) site.kind = 255;
        if (fault == 2) site.flags_bypass_resume_offset = 36;
        if (fault == 3) site.flags_merge_branch_offset = 25;
        if (fault == 4) site.edge_flags = {};
        if (fault == 5) site.unlinked_instruction = 0x14000000u;
        if (fault == 6) malformed.link_sites.push_back(site);
        if (fault == 7) malformed.region_branches.push_back({28, SerialRegionBranchKind::MergeNZCV, 0});
        aot::AotImage rejected;
        Check(!Aot(malformed, rejected), "AOT rejects malformed or overlapping physical Call recipes");
    }
}

void Native(bool dependent = false) {
    using namespace swift::runtime::ir;
    const auto bytes = 8 * static_cast<size_t>(getpagesize());
    void* guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(guest != MAP_FAILED, "map real guest window for entry installation");
    const auto release = ScopeExit([&] { munmap(guest, bytes); });
    Config config{};
    config.loc_start = 0;
    config.loc_end = bytes;
    config.enable_jit = true;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
    config.memory_base = guest;
    config.guest_addr_mask = bytes - 1;
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
#if defined(__riscv)
    config.backend_isa = kRiscv64;
#else
    config.backend_isa = kArm64;
#endif
    std::string cache_directory;
    const std::array<const char*, 3> cache_env{
            "SVM_JIT_CACHE", "SVM_FLAGS_LOOP_LAZY", "SVM_BACKEDGE_FLAGS"};
    std::array<std::optional<std::string>, 3> saved_env;
    const auto restore_cache = ScopeExit([&] {
        if (cache_directory.empty()) return;
        for (size_t i = 0; i < cache_env.size(); ++i) {
            if (saved_env[i])
                SetSvmConfigEnvForTest(cache_env[i], saved_env[i]->c_str(), 1);
            else
                UnsetSvmConfigEnvForTest(cache_env[i]);
        }
        std::filesystem::remove_all(cache_directory);
    });
    if (dependent) {
#if defined(__ANDROID__)
        char directory[] = "/data/local/tmp/swiftvm-entry-state.XXXXXX";
#else
        char directory[] = "/tmp/swiftvm-entry-state.XXXXXX";
#endif
        Check(mkdtemp(directory) != nullptr, "create owned entry-state cache directory");
        cache_directory = directory;
        for (size_t i = 0; i < cache_env.size(); ++i)
            if (const auto* value = GetRawSvmConfigEnvForTest(cache_env[i])) saved_env[i] = value;
        SetSvmConfigEnvForTest(cache_env[0], cache_directory.c_str(), 1);
        SetSvmConfigEnvForTest(cache_env[1], "0", 1);
        SetSvmConfigEnvForTest(cache_env[2], "0", 1);
    }
    aot::AotImage image;
    {
        AddressSpace source{config};
        const auto module = source.GetDefaultModule();
        HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
        auto* function = builder.AppendFunction(Location{0x100}, Location{0x104});
        const auto left = function->LoadUniform(Uniform{8, ValueType::U64});
        const auto one = function->LoadImm(ValueType::U64, Imm{u64{1}});
        const auto sum = function->Add(ValueType::U64, left, Operand{one});
        function->SaveFlags(sum, Flags::All);
        function->StoreUniform(Uniform{0, ValueType::U64}, sum);
        function->AdvancePC(Imm{u64{1}});
        auto* tail = builder.LinkBlock(terminal::LinkBlock{Location{0x102}});
        builder.SetCurBlock(tail);
        // Exercise both an independent public entry and the old Function's
        // dependent body, which must survive persistence without publication.
        function->StoreUniform(Uniform{16, ValueType::U64},
                               dependent ? one : function->LoadImm(ValueType::U64, Imm{u64{1}}));
        function->AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        auto* entry = static_cast<u8*>(TranslateIR(module, function));
        FaultEntry allocation;
        std::vector<FaultEntry> faults;
        Check(entry && module->LookupCodeAllocation(entry, allocation, &faults),
              "capture actual compiled allocation and precise fault records");
        const auto target = source.GetLinkManager().QueryTarget(0x100);
        Check(target && target->host_pc == source.GetCodeCache(Location{0x100}) &&
              target->target_owner.allocation == allocation.host_start,
              "capture actual published function target contract");
        const auto canonical_entry = source.GetCodeCache(Location{0x100});
        const auto secondary_entry = source.GetCodeCache(Location{0x102});
        Check(module->GetJitCache(Location{0x100}) == canonical_entry,
              "module lookup returns the exact root canonical Function entry");
        Check(!module->GetJitCache(Location{0x101}) && !source.GetCodeCache(Location{0x101}) &&
                      !module->GetJitCache(Location{0x103}) &&
                      !source.GetCodeCache(Location{0x103}),
              "instruction interiors do not alias the Function allocation entry");
        if (config.backend_isa == kArm64 && !dependent) {
            const auto secondary_target = source.GetLinkManager().QueryTarget(0x102);
            Check(secondary_entry && secondary_entry != canonical_entry && secondary_target &&
                          secondary_target->host_pc == secondary_entry,
                  "address-space lookup returns the distinct published secondary Function entry");
            Check(source.GetCodeCacheTable().Zero(0x100) && source.GetCodeCacheTable().Zero(0x102),
                  "clear only the two canonical dispatch slots while retaining their owner");
            Check(!module->GetJitCache(Location{0x100}) && !source.GetCodeCache(Location{0x100}) &&
                          !module->GetJitCache(Location{0x102}) &&
                          !source.GetCodeCache(Location{0x102}),
                  "missing Function entries stay missing instead of resurrecting the allocation "
                  "start");
            source.PushCodeCache(Location{0x100}, canonical_entry);
            source.PushCodeCache(Location{0x102}, secondary_entry);
            Check(module->GetJitCache(Location{0x100}) == canonical_entry &&
                          source.GetCodeCache(Location{0x102}) == secondary_entry,
                  "republishing exact Function slots restores their distinct entries");
        } else if (dependent) {
            Check(!secondary_entry && !source.GetLinkManager().QueryTarget(0x102) &&
                          !module->GetJitCache(Location{0x102}) &&
                          !source.GetCallCodeCacheTable().Lookup(0x102),
                  "dependent Function body is absent from every external entry table");
        }
        const auto offset = [&](const void* pointer) -> u32 {
            if (!pointer) return UINT32_MAX;
            auto* address = static_cast<const u8*>(pointer);
            Check(address >= allocation.host_start && address < allocation.host_end,
                  "actual emitted entry belongs to the captured allocation");
            return static_cast<u32>(address - allocation.host_start);
        };
        SerialUnit unit{};
        unit.guest_start = 0x100;
        unit.is_function = 1;
        unit.code.assign(allocation.host_start, allocation.host_end);
        SerialBlock block{0x100, 0x102, offset(target->host_pc),
            HashBytes(static_cast<const u8*>(guest) + 0x100, 2, 0xCBF29CE484222325ull),
            offset(target->direct_host_pc), offset(target->pending_flags_host_pc), target->pending_flags_contract};
        block.continuation_code_offset = offset(target->call_host_pc);
        block.pending_flags_continuation_code_offset = offset(target->call_pending_flags_host_pc);
#if defined(__aarch64__)
        if (FlagsRegsEnabled() && (block.continuation_code_offset == UINT32_MAX ||
            block.pending_flags_code_offset == UINT32_MAX || block.pending_flags_continuation_code_offset == UINT32_MAX)) {
            throw std::runtime_error("emitted entry offsets: canonical=" + std::to_string(block.code_offset) +
                " direct=" + std::to_string(block.direct_code_offset) + " pending=" + std::to_string(block.pending_flags_code_offset) +
                " continuation=" + std::to_string(block.continuation_code_offset) +
                " pending_continuation=" + std::to_string(block.pending_flags_continuation_code_offset) +
                " overwrite=" + std::to_string(block.pending_flags_contract.overwrite_before_observe));
        }
        Check(!FlagsRegsEnabled() || (block.continuation_code_offset != UINT32_MAX &&
              block.pending_flags_code_offset != UINT32_MAX && block.pending_flags_continuation_code_offset != UINT32_MAX),
              "flags1 production emission supplies real pending and published-frame entries");
#endif
        Check(block.ValidEntryContract(unit.code.size()), "actual emitted entries satisfy persistent entry validation");
        unit.blocks.push_back(block);
        SerialBlock secondary{0x102, 0x104, UINT32_MAX,
            HashBytes(static_cast<const u8*>(guest) + 0x102, 2, 0xCBF29CE484222325ull)};
        if (auto* secondary_entry = source.GetCodeCache(Location{0x102})) {
            secondary.code_offset = offset(secondary_entry);
            const auto secondary_target = source.GetLinkManager().QueryTarget(0x102);
            if (secondary_target) {
                secondary.direct_code_offset = offset(secondary_target->direct_host_pc);
                secondary.pending_flags_code_offset = offset(secondary_target->pending_flags_host_pc);
                secondary.pending_flags_contract = secondary_target->pending_flags_contract;
                secondary.continuation_code_offset = offset(secondary_target->call_host_pc);
                secondary.pending_flags_continuation_code_offset = offset(secondary_target->call_pending_flags_host_pc);
            } else secondary.entry_flags = 0;
        } else secondary.entry_flags = SerialBlock::MetadataOnly;
        Check(secondary.ValidEntryContract(unit.code.size()), "retain actual secondary entry or dependency contract");
        unit.blocks.push_back(secondary);
        for (const auto& fault : faults) unit.fault_sites.push_back({fault.guest_loc,
            static_cast<u32>(fault.host_start - allocation.host_start),
            static_cast<u32>(fault.host_end - allocation.host_start),
            fault.recovery ? static_cast<u32>(fault.recovery - allocation.host_start) : UINT32_MAX,
            static_cast<u8>(fault.recovery_kind)});
        image = Image(unit);
        image.host_isa = config.backend_isa;
        image.key = {kCacheFormatVersion, ComputeBuildId(), ComputeConfigHash(config), ComputeEnvHash(),
                     aot::HashGuestImage(image.segments, image.guest.entry)};
        const auto cache = module->GetCodeCache(allocation.host_start);
        Check(cache && FreezeRegionBranches(*cache, allocation.host_start, image.code, image.units.front().region_branches),
              "freeze only exact known shared region branches from the actual code cache");
        image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
        const auto scan = ScanCodeUnit(image.code,
                                       GetHostImage(),
                                       bytes,
                                       {},
                                       config.backend_isa,
                                       image.units.front().region_branches);
        if (!scan.ok) throw std::runtime_error("actual five-entry relocation scan: " + scan.reject_reason);
        ++checks;
        image.units.front().relocs = scan.relocs;
        IntrusivePtr<Block> single{new Block(0, Location{0x200})};
        single->StoreUniform(Uniform{24, ValueType::U64},
                             single->LoadImm(ValueType::U64, Imm{u64{7}}));
        single->AdvancePC(Imm{u64{4}});
        single->SetTerminal(terminal::ReturnToHost{});
        Check(module->Push(single.get()), "publish independent Block ownership for exact lookup");
        const auto block_entry = TranslateIR(module, single);
        Check(block_entry && module->GetJitCache(Location{0x200}) == block_entry &&
                      source.GetCodeCache(Location{0x200}) == block_entry,
              "standalone Block retains its exact-start cache fallback");
        Check(!module->GetJitCache(Location{0x201}) && !source.GetCodeCache(Location{0x201}),
              "Block fallback does not publish an instruction interior");
        if (dependent) {
            const auto disk = source.GetJitDiskCache();
            Check(disk && disk->Enabled() && disk->Stats().units_compiled.load() == 2 &&
                          disk->Stats().reject_scan.load() == 0,
                  "production recorder captures the dependent Function and independent Block");
            disk->Save();
            Check(disk->Stats().units_stored.load() == 2, "save actual dependent Function code");
        }
    }
    if (dependent) {
        AddressSpace disk_destination{config};
        disk_destination.LoadJitCache();
        const auto disk = disk_destination.GetJitDiskCache();
        Check(disk && disk->Stats().units_loaded.load() == 2 &&
                      disk_destination.GetCodeCache(Location{0x100}) &&
                      !disk_destination.GetCodeCache(Location{0x102}) &&
                      !disk_destination.GetLinkManager().QueryTarget(0x102),
              (std::string{"fresh disk instance restores root and leaves dependent metadata "
                           "unexported: loaded="} +
               (disk ? std::to_string(disk->Stats().units_loaded.load()) : "disabled") +
               " header=" + (disk ? std::to_string(disk->Stats().reject_header.load()) : "-") +
               " relocation=" + (disk ? std::to_string(disk->Stats().reject_reloc.load()) : "-") +
               " guest=" + (disk ? std::to_string(disk->Stats().reject_guest_bytes.load()) : "-"))
                      .c_str());
        Runtime runtime{&disk_destination};
        const u64 input = 36;
        std::memcpy(runtime.GetUniformBuffer().data() + 8, &input, sizeof(input));
        runtime.SetLocation(0x100);
        Check(runtime.Run() == HaltReason::CallHost,
              "restored disk Function follows its retained internal value transfer");
        u64 answer{};
        std::memcpy(&answer, runtime.GetUniformBuffer().data(), sizeof(answer));
        Check(answer == 37, "disk restoration preserves independent arithmetic result");
        std::memcpy(&answer, runtime.GetUniformBuffer().data() + 16, sizeof(answer));
        Check(answer == 1, "disk restoration preserves nonlocal SSA into its internal body");
    }
    const auto blob = aot::EncodeInfoBlob(image);
    aot::AotImage restored;
    std::string error;
    Check(aot::DecodeInfoBlob(blob.data(), blob.size(), restored, error), "decode actual production entry metadata");
    restored.host_isa = image.host_isa;
    restored.code = image.code;
    AddressSpace destination{config};
    aot::LoadReport report;
    Check(aot::InstallArtifact(destination, restored, guest, config.guest_addr_mask, report, error) == aot::kAotOk,
          "install actual production code after source AddressSpace destruction");
    auto* base = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, 0));
    const auto& block = image.units.front().blocks.front();
    const auto pointer = [&](u32 offset) -> void* { return offset == UINT32_MAX ? nullptr : base + offset; };
    const auto module = destination.GetDefaultModule();
    const auto target = destination.GetLinkManager().QueryTarget(0x100);
    Check(target && target->target_owner.allocation == base &&
          target->host_pc == pointer(block.code_offset) && target->direct_host_pc == pointer(block.direct_code_offset) &&
          target->pending_flags_host_pc == pointer(block.pending_flags_code_offset) &&
          target->call_host_pc == pointer(block.continuation_code_offset) &&
          target->call_pending_flags_host_pc == pointer(block.pending_flags_continuation_code_offset) &&
          target->pending_flags_contract == block.pending_flags_contract,
          "installed target restores every actual entry and flags contract at its new allocation");
    Check(destination.GetCodeCache(Location{0x100}) == pointer(block.code_offset) &&
          reinterpret_cast<void*>(destination.GetCallCodeCacheTable().Lookup(0x100)) == pointer(block.continuation_code_offset) &&
          reinterpret_cast<void*>(destination.GetPendingCallCodeCacheTable().Lookup(0x100)) == pointer(block.pending_flags_continuation_code_offset),
          "canonical, call and pending-call tables publish the actual restored entries");
    const auto& secondary = image.units.front().blocks[1];
    Check(module->GetJitCache(Location{0x100}) == pointer(block.code_offset),
          "restored Module root lookup returns the canonical entry rather than the allocation");
    if (config.backend_isa == kArm64 && !dependent) {
        Check(secondary.IsPublished() &&
                      destination.GetCodeCache(Location{secondary.guest_start}) ==
                              pointer(secondary.code_offset),
              "restored address-space lookup preserves the exact secondary canonical entry");
        Check(destination.GetCodeCacheTable().Zero(0x100) &&
                      destination.GetCodeCacheTable().Zero(secondary.guest_start) &&
                      !module->GetJitCache(Location{0x100}) &&
                      !destination.GetCodeCache(Location{0x100}) &&
                      !destination.GetCodeCache(Location{secondary.guest_start}),
              "restored live Function ownership does not resurrect cleared canonical entries");
        destination.PushCodeCache(Location{0x100}, pointer(block.code_offset));
        destination.PushCodeCache(Location{secondary.guest_start}, pointer(secondary.code_offset));
    } else if (dependent) {
        Check(!secondary.IsPublished() && secondary.entry_flags == SerialBlock::MetadataOnly &&
                      !destination.GetCodeCache(Location{secondary.guest_start}) &&
                      !destination.GetLinkManager().QueryTarget(secondary.guest_start),
              "AOT restores dependent guest extent without fabricating an external entry");
    }
    Check(!module->GetJitCache(Location{0x101}) && !destination.GetCodeCache(Location{0x101}) &&
                  !module->GetJitCache(Location{0x103}) &&
                  !destination.GetCodeCache(Location{0x103}),
          "restored Function range does not publish instruction interiors");
    for (const u64 input : {u64{36}, ~u64{0}}) {
        Runtime runtime{&destination};
        std::memcpy(runtime.GetUniformBuffer().data() + 8, &input, sizeof(input));
        runtime.SetLocation(0x100);
        Check(runtime.Run() == HaltReason::CallHost, "execute genuinely restored native function");
        u64 value{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
        Check(value == input + 1, "actual restored flags-producing function computes the independent answer");
        if (dependent) {
            std::memcpy(&value, runtime.GetUniformBuffer().data() + 16, sizeof(value));
            Check(value == 1, "restored AOT internal body receives its original SSA constant");
            value = input + 1;
        }
#if defined(__aarch64__)
        Check((runtime.GetState()->host_cpu_flags & 0xf0000000u) == (value == 0 ? 0x60000000u : 0),
              "actual restored function preserves independent carry/zero/overflow/sign results");
#endif
    }
    auto malformed = restored;
    malformed.units.front().blocks.front().continuation_code_offset = malformed.units.front().code_size;
    AddressSpace rejected{config};
    aot::LoadReport rejection;
    Check(aot::InstallArtifact(rejected, malformed, guest, config.guest_addr_mask, rejection, error) == aot::kAotBadFormat &&
          !rejection.mapping && !rejection.units_installed && !rejected.GetCodeCache(Location{0x100}) &&
          !rejected.GetCallCodeCacheTable().Lookup(0x100) && !rejected.GetPendingCallCodeCacheTable().Lookup(0x100) &&
          !rejected.GetLinkManager().QueryTarget(0x100),
          "bad continuation entry is rejected before any native target or table publication");
    if (!restored.units.front().region_branches.empty()) {
        auto bad_branch = restored;
        const auto offset = bad_branch.units.front().region_branches.front().code_offset;
        const u32 changed = 0x1400'0001u;
        std::memcpy(bad_branch.code.data() + offset, &changed, 4);
        bad_branch.code_hash = HashBytes(bad_branch.code.data(), bad_branch.code.size(), 0xCBF29CE484222325ull);
        aot::LoadReport bad_report;
        Check(aot::InstallArtifact(rejected, bad_branch, guest, config.guest_addr_mask, bad_report, error) == aot::kAotBadFormat &&
              !bad_report.mapping && !bad_report.units_installed && !rejected.GetCodeCache(Location{0x100}),
              "changed shared branch instruction is rejected before native publication");
    }
    std::printf("native_entries canonical=%u direct=%u pending=%u continuation=%u pending_continuation=%u shared_branches=%zu\n",
        block.code_offset, block.direct_code_offset, block.pending_flags_code_offset,
        block.continuation_code_offset, block.pending_flags_continuation_code_offset,
        restored.units.front().region_branches.size());
    destination.InvalidateCodeRange(0x100, 0x104);
    Check(!destination.GetCodeCache(Location{0x100}) && !destination.GetCallCodeCacheTable().Lookup(0x100) &&
          !destination.GetPendingCallCodeCacheTable().Lookup(0x100) && !aot::ResolveCodeOffset(report.mapping, 0),
          "range retirement revokes every restored entry and installed allocation alias");
}

void NativeLinks() {
    using namespace swift::runtime::ir;
    const auto bytes = 8 * static_cast<size_t>(getpagesize());
    void* guest = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(guest != MAP_FAILED, "map linked AOT guest image");
    const auto release = ScopeExit([&] { munmap(guest, bytes); });
    Config config{};
    config.loc_start = 0;
    config.loc_end = bytes;
    config.enable_jit = true;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
    config.memory_base = guest;
    config.guest_addr_mask = bytes - 1;
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
#if defined(__riscv)
    config.backend_isa = kRiscv64;
#else
    config.backend_isa = kArm64;
#endif
    constexpr std::array<u64, 3> locations{0x100, 0x1100, 0x2100};
    aot::AotImage image;
    image.host_isa = config.backend_isa;
    image.guest.entry = locations.front();
    image.key = {kCacheFormatVersion, ComputeBuildId(), ComputeConfigHash(config), ComputeEnvHash(),
                 aot::HashGuestImage(image.segments, image.guest.entry)};
    {
        AddressSpace source{config};
        source.SetSerializableCodeEmission();
        const auto module = source.GetDefaultModule();
        for (const auto location : locations) {
            HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
            auto* function = builder.AppendFunction(Location{location}, Location{location + 4});
            const bool target = location == locations.front();
            function->StoreUniform(
                    Uniform{target ? u16{0} : u16{8}, ValueType::U64},
                    function->LoadImm(ValueType::U64, Imm{target ? u64{37} : location}));
            if (target)
                function->EndBlock(terminal::ReturnToHost{});
            else function->EndBlock(terminal::LinkBlock{Location{locations.front()}});
            function->EndFunction();
            auto* entry = static_cast<u8*>(TranslateIR(module, function));
            FaultEntry extent;
            Check(entry && module->LookupCodeAllocation(entry, extent), "compile genuine linked AOT unit");
            SerialUnit captured;
            std::string error;
            Check(CaptureSerialCode(*module, extent.host_start, u32(extent.host_end - extent.host_start),
                  GetHostImage(), bytes, captured, &error), error.c_str());
            captured.guest_start = location;
            captured.is_function = 1;
            const auto published = source.GetLinkManager().QueryTarget(location);
            Check(published.has_value(), "capture linked unit target contract");
            const auto offset = [&](const void* pointer) -> u32 {
                if (!pointer) return UINT32_MAX;
                const auto* address = static_cast<const u8*>(pointer);
                Check(address >= extent.host_start && address < extent.host_end, "linked unit entry belongs to allocation");
                return u32(address - extent.host_start);
            };
            SerialBlock block{location, location + 4, offset(published->host_pc),
                HashBytes(static_cast<const u8*>(guest) + location, 4, 0xCBF29CE484222325ull),
                offset(published->direct_host_pc), offset(published->pending_flags_host_pc), published->pending_flags_contract};
            block.continuation_code_offset = offset(published->call_host_pc);
            block.pending_flags_continuation_code_offset = offset(published->call_pending_flags_host_pc);
            captured.blocks.push_back(block);
            auto unit = Image(captured).units.front();
            unit.relocs = std::move(captured.relocs);
            unit.code_offset = u32(image.code.size());
            Check(target ? unit.link_sites.empty() : unit.link_sites.size() == 1 &&
                  unit.link_sites.front().kind == u8(LinkSiteKind::Unconditional), "capture two actual cross-unit Branch sources");
            image.code.insert(image.code.end(), captured.code.begin(), captured.code.end());
            image.units.push_back(std::move(unit));
        }
        source.GetCodeCacheTable().ForEachEntry([&](u32 index, size_t key, size_t) {
            image.dispatch_slots.emplace_back(u64(key), index);
        });
    }
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    const auto blob = aot::EncodeInfoBlob(image);
    aot::AotImage restored;
    std::string error;
    Check(aot::DecodeInfoBlob(blob.data(), blob.size(), restored, error), error.c_str());
    restored.host_isa = image.host_isa;
    restored.code = image.code;
    AddressSpace destination{config};
    aot::LoadReport report;
    Check(aot::InstallArtifact(destination, restored, guest, config.guest_addr_mask, report, error) == aot::kAotOk,
          error.c_str());
    Check(report.units_installed == locations.size(), "install all three actual linked units");
    const auto module = destination.GetDefaultModule();
    CodeCache* shared_cache{};
    for (const auto& unit : restored.units) {
        auto* allocation = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, unit.code_offset));
        auto* cache = module->GetCodeCache(allocation);
        Check(cache && (!shared_cache || cache == shared_cache), "linked AOT allocations reuse one real arena");
        shared_cache = cache;
    }
    for (const auto location : {locations[1], locations[2]}) {
        Runtime runtime{&destination};
        runtime.SetLocation(location);
        Check(runtime.Run() == HaltReason::CallHost, "execute each restored cross-unit Branch");
        u64 value{}, witness{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 8, 8);
        Check(value == 37 && witness == location, "restored linked source and target both execute independently");
    }
    std::printf("native_links units=3 sources=2 arenas=1 executions=2 result=37\n");
}
}

int main(int argc, char** argv) {
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "disk";
        const auto input = Fixture();
        if (mode == "disk") {
            SerialUnit output;
            Check(Disk(input, output), "decode disk entry fixture");
            Contract(output.blocks, input);
            PendingOnly(false);
        } else if (mode == "aot") {
            aot::AotImage output;
            Check(Aot(input, output) && output.units.size() == 1, "decode AOT entry fixture");
            Contract(output.units.front().blocks, input);
            PendingOnly(true);
        } else if (mode == "reject") {
            Reject();
        } else if (mode == "native") {
            Native();
#if defined(__aarch64__)
            Native(true);
#endif
        } else if (mode == "aot-links") {
            AotLinks();
        } else if (mode == "native-links") {
            NativeLinks();
        } else throw std::runtime_error("unknown entry persistence mode");
        std::printf("mode=%.*s checks=%u passed\n", static_cast<int>(mode.size()), mode.data(), checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "failure: %s\n", error.what());
        return 1;
    }
}
