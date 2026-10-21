#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#include "aot/aot_artifact.h"
#include "aot/aot_load.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/hir_builder.h"

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
namespace aot = swift::aot;
unsigned checks{};
constexpr u64 guest_pc = 0x100;
constexpr u64 expected = 0x123456789abcdef0ull;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}

class GuestWindow {
public:
    const size_t size = 8 * static_cast<size_t>(getpagesize());
    void* const data = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    GuestWindow() { Check(data != MAP_FAILED, "map guest bytes for actual artifact"); }
    ~GuestWindow() { if (data != MAP_FAILED) munmap(data, size); }
};

u8* Compile(const std::shared_ptr<Module>& module) {
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{guest_pc}, Location{guest_pc + 4});
    function->StoreUniform(Uniform{0, ValueType::U64},
                          function->LoadImm(Imm{expected}).SetType(ValueType::U64));
    function->EndBlock(terminal::ReturnToHost{});
    function->EndFunction();
    auto* entry = static_cast<u8*>(TranslateIR(module, function));
    Check(entry != nullptr, "compile current production artifact control");
    return entry;
}

void VerifyExecution(AddressSpace& space) {
    Runtime runtime{&space};
    runtime.SetLocation(guest_pc);
    Check(runtime.Run() == HaltReason::CallHost, "execute actual current artifact");
    u64 result{};
    std::memcpy(&result, runtime.GetUniformBuffer().data(), sizeof(result));
    Check(result == expected, "current artifact retains independently expected answer");
    Check(runtime.GetState()->rsb_empty && runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty,
          "current artifact balances external frame state");
}

void Run() {
    Check(kCacheFormatVersion == 31 && !FlagsRegsEnabled(), "test supported format31 flags0 contract");
    GuestWindow guest;
    Config config{};
    config.loc_start = 0;
    config.loc_end = guest.size;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::ReturnStackBuffer;
    config.region_edges = false;
    config.memory_base = guest.data;
    config.guest_addr_mask = guest.size - 1;
    aot::AotImage image{};
    image.host_isa = kArm64;
    image.guest.entry = guest_pc;
    image.key = {kCacheFormatVersion, ComputeBuildId(), ComputeConfigHash(config),
                 ComputeEnvHash(), aot::HashGuestImage(image.segments, image.guest.entry)};
    {
        AddressSpace source{config};
        auto module = source.GetDefaultModule();
        auto* entry = Compile(module);
        FaultEntry allocation;
        std::vector<FaultEntry> faults;
        Check(module->LookupCodeAllocation(entry, allocation, &faults), "capture real production allocation");
        Check(source.GetLinkManager().QuerySourceSites({module.get(), allocation.host_start}).empty(),
              "supported control has no production direct-link sites");
        aot::AotUnit unit{};
        unit.guest_start = guest_pc;
        unit.code_size = static_cast<u32>(allocation.host_end - allocation.host_start);
        image.code.assign(allocation.host_start, allocation.host_end);
        for (const auto& fault : faults) {
            Check(fault.owner_start == allocation.host_start, "actual AOT fault metadata retains allocation owner");
            unit.fault_sites.push_back({fault.guest_loc,
                    static_cast<u32>(fault.host_start - allocation.host_start),
                    static_cast<u32>(fault.host_end - allocation.host_start),
                    fault.recovery ? static_cast<u32>(fault.recovery - allocation.host_start) : UINT32_MAX,
                    static_cast<u8>(fault.recovery_kind)});
        }
        Check(unit.ValidFaultSites(), "actual AOT metadata includes the full allocation owner range");
        const auto scan = ScanCodeUnit(image.code, GetHostImage(), guest.size);
        Check(scan.ok, "production code satisfies supported AOT relocation scanner");
        unit.relocs = scan.relocs;
        unit.blocks.push_back({guest_pc, guest_pc + 4,
                              static_cast<u32>(entry - allocation.host_start),
                              HashBytes(static_cast<const u8*>(guest.data) + guest_pc, 4,
                                        0xCBF29CE484222325ull)});
        image.units.push_back(std::move(unit));
        image.code.resize((image.code.size() + 15) & ~size_t{15});
        image.stub_offset = static_cast<u32>(image.code.size());
        const u32 stub[2] = {aot::kAotStubInsn, 0};
        image.code.insert(image.code.end(), reinterpret_cast<const u8*>(stub),
                          reinterpret_cast<const u8*>(stub) + sizeof(stub));
        image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
        VerifyExecution(source);
    }
    const auto blob = aot::EncodeInfoBlob(image);
    aot::AotImage current{};
    std::string error;
    if (!aot::DecodeInfoBlob(blob.data(), blob.size(), current, error)) {
        throw std::runtime_error("decode valid current artifact metadata: " + error);
    }
    ++checks;
    current.code = image.code;
    Check(current.key == image.key && current.units.size() == 1, "current artifact key and actual unit round trip");
    Check(current.units[0].link_sites.empty(), "supported control has no AOT link metadata");
    AddressSpace destination{config};
    aot::LoadReport report{};
    Check(aot::InstallArtifact(destination, current, guest.data, config.guest_addr_mask, report, error) == aot::kAotOk,
          "format31 actual artifact installs through production loader");
    Check(report.units_installed == 1 && report.code_bytes == current.units[0].code_size,
          "current loader publishes the actual unit");
    auto* installed_entry = destination.GetCodeCache(Location{guest_pc});
    Check(installed_entry && installed_entry == aot::ResolveCodeOffset(current.units[0].code_offset +
                                                                      current.units[0].blocks[0].code_offset),
          "dispatch uses the installed artifact root entry");
    VerifyExecution(destination);
    Check(destination.GetCodeCache(Location{guest_pc}) == installed_entry,
          "actual execution preserves installed artifact entry identity");
    auto* entry_before = destination.GetCodeCache(Location{guest_pc});
    auto* offset_before = aot::ResolveCodeOffset(0);
    Check(entry_before && offset_before, "current dispatch and code-offset mapping are visible");
    auto old = current;
    old.key.format_version = 30;
    aot::LoadReport rejected{};
    error.clear();
    const int status = aot::InstallArtifact(destination, old, guest.data, config.guest_addr_mask, rejected, error);
    Check(status == aot::kAotKeyMismatch && error == "format version 30 != 31",
          "only format30 change produces the exact AOT compatibility rejection");
    Check(rejected.units_installed == 0 && rejected.dispatch_slots == 0 && rejected.code_bytes == 0,
          "old format publishes no code or dispatch assignments");
    Check(destination.GetCodeCache(Location{guest_pc}) == entry_before && aot::ResolveCodeOffset(0) == offset_before,
          "old format rejection preserves existing dispatch and code-offset identities");
    VerifyExecution(destination);
    std::printf("aot current=31 old=30 status=%d error=%s units=%u\n", status, error.c_str(), rejected.units_installed);
}
} // namespace

int main() {
    try {
        Run();
        std::printf("mode=cache-version checks=%u passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "static-latch-cache failure: %s\n", error.what());
        return 1;
    }
}
