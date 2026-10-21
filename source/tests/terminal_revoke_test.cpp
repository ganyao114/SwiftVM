#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include "runtime/backend/address_space.h"
#include "runtime/backend/fault_catalog.h"
#include "runtime/backend/link_manager.h"

namespace {
thread_local bool deny_allocations{};
volatile std::sig_atomic_t in_signal{};
void* fail_unprotect{};
unsigned failures_left{};
unsigned failures_seen{};
unsigned checks{};
}

void* operator new(std::size_t size) {
    if (in_signal) std::_Exit(91);
    if (deny_allocations) throw std::bad_alloc{};
    if (void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

extern "C" int __real_mprotect(void*, std::size_t, int);
extern "C" int __wrap_mprotect(void* pointer, std::size_t size, int prot) {
    if (pointer == fail_unprotect && (prot & PROT_WRITE) && failures_left) {
        --failures_left;
        ++failures_seen;
        errno = EACCES;
        return -1;
    }
    return __real_mprotect(pointer, size, prot);
}

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;

void Check(bool condition, const char* detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}

void Catalog() {
    FaultCatalog empty;
    {
        FaultCatalog::Reader held{empty};
        Check(!empty.SealAndRevoke(), "even an empty catalog preserves a held reader lifetime");
    }
    Check(empty.SealAndRevoke(), "empty catalog completes after its reader leaves");
    FaultCatalog catalog;
    std::array<u8, 256> code{};
    int first{}, second{};
    catalog.Add(&first, {code.data(), code.data() + 64, code.data(), code.data() + 60, 1});
    catalog.Add(&second, {code.data() + 64, code.data() + 128, code.data() + 64,
                         code.data() + 124, 2});
    {
        FaultCatalog::Reader held{catalog};
        FaultEntry copied;
        Check(held.Lookup(code.data(), copied) && copied.recovery == code.data() + 60,
              "pre-revocation recovery is real");
        deny_allocations = true;
        const bool ready = catalog.SealAndRevoke();
        deny_allocations = false;
        Check(!ready, "old reader prevents terminal metadata collection");
        const auto stats = catalog.GetStats();
        Check(stats.allocations == 0 && stats.retired_allocations == 2 &&
              stats.retired_snapshots == 2 && stats.readers == 1,
              "all owners revoked while old reader preserves storage");
        Check(!held.Lookup(code.data(), copied), "held reader cannot rediscover a revoked entry");
        for (unsigned i = 0; i < 64; ++i) {
            FaultCatalog::Reader late{catalog};
            Check(!late.Lookup(code.data() + 64, copied), "late reader cannot recover old code");
        }
        bool rejected{};
        try { catalog.Add(&first, {code.data(), code.data() + 64, code.data(), nullptr, 3}); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "terminal catalog rejects renewed publication");
        catalog.Collect();
        Check(catalog.GetStats().retired_allocations == 2, "collect does not wait under writer mutex");
    }
    deny_allocations = true;
    const bool ready = catalog.SealAndRevoke();
    deny_allocations = false;
    const auto stats = catalog.GetStats();
    Check(ready && stats.readers == 0 && stats.retired_snapshots == 0 &&
          stats.retired_allocations == 0, "reader exit permits allocation-free terminal retry");
}

LinkManager* signal_manager{};
volatile std::sig_atomic_t signal_count{};
void InvalidateSignal(int) {
    in_signal = 1;
    if (signal_manager->SignalInvalidateTarget(0x2000).found) std::_Exit(92);
    ++signal_count;
    in_signal = 0;
}

void Links(bool signals) {
    const auto page = static_cast<u32>(getpagesize());
    for (unsigned round = 0; round < 32; ++round) {
        MemMap memory{page, true};
        auto* rw = memory.GetMemory();
        auto* rx = static_cast<u8*>(memory.Map(page, 0, MemMap::ReadExe));
        const ISA isa =
#if defined(__riscv)
                kRiscv64;
#else
                kArm64;
#endif
        const u32 bl = isa == kRiscv64 ? *EncodeRiscvJal(8, 1) : *EncodeBL(8);
        *reinterpret_cast<u32*>(rw) = bl;
        CodeRegion region{.id = 1, .rw_base = rw, .rx_base = rx, .capacity = page, .isa = isa};
        LinkSignalPatchSite patch{.region = region, .rx_site = rx, .rw_site = rw, .unlinked_bl = bl};
        LinkManager manager;
        const LinkSourceOwner source{&manager, rx};
        const LinkSourceOwner target_only{&memory, rx + 32};
        Check(manager.RegisterSite({1, 0}, 0x2000, source, &patch), "register a physical signal site");
        Check(manager.PublishTarget(0x2000, rx + 16, 1, target_only) != 0 &&
              manager.PublishTarget(0x3000, rx + 32, 1, {}) != 0,
              "target-only and ownerless records are published");
        (void)manager.DetachSource(source);
        {
            LinkManager::SignalReader held{manager};
            Check(static_cast<bool>(held), "reader enters the same production signal gate");
            deny_allocations = true;
            const bool ready = manager.SealAndRevoke();
            deny_allocations = false;
            Check(!ready, "signal storage survives a held pre-revocation reader");
            Check(!manager.QueryTarget(0x2000) && !manager.QueryTarget(0x3000),
                  "all target generations become unavailable before grace");
            const auto stats = manager.GetStats();
            Check(stats.retiring == 1 && stats.signal_sites_retained == 1 &&
                  stats.signal_targets_retained == 2, "retiring sites and target-only storage survive grace");
            LinkManager::SignalReader late{manager};
            Check(!late, "late signal entry is refused after its reader increment");
            Check(!manager.SignalInvalidateTarget(0x2000).found, "late invalidation skips old addresses");
            Check(!manager.RegisterSite({1, 4}, 0x2000, source) &&
                  manager.PublishTarget(0x4000) == 0, "closed manager rejects new sites and targets");
        }
        deny_allocations = true;
        const bool ready = manager.SealAndRevoke();
        deny_allocations = false;
        const auto stats = manager.GetStats();
        Check(ready && stats.sites == 0 && stats.target_records == 0 && stats.outgoing_owners == 0 &&
              stats.incoming_targets == 0 && stats.signal_sites_retained == 0 &&
              stats.signal_targets_retained == 0 && stats.estimated_bytes == 0,
              "terminal retry clears every index including target-only and ownerless records");
        Check(memory.ReleaseAfterQuiescence().Complete(), "old patch addresses are actually unmapped");
        if (signals) {
            signal_manager = &manager;
            struct sigaction action{}, previous{};
            action.sa_handler = InvalidateSignal;
            sigemptyset(&action.sa_mask);
            Check(sigaction(SIGUSR2, &action, &previous) == 0, "install real late signal handler");
            for (unsigned i = 0; i < 16; ++i) raise(SIGUSR2);
            Check(sigaction(SIGUSR2, &previous, nullptr) == 0, "restore signal handler");
        } else {
            std::thread late{[&] {
                for (unsigned i = 0; i < 10000; ++i) {
                    if (manager.SignalInvalidateTarget(0x2000).found) std::_Exit(93);
                }
            }};
            late.join();
        }
        Check(manager.SealAndRevoke(), "terminal release is idempotent");
    }
    if (signals) Check(signal_count == 512, "all real signals ran against revoked, unmapped addresses");
}

void Smc(bool diagnostic) {
    const auto page = static_cast<size_t>(getpagesize());
    auto* guest = static_cast<u8*>(mmap(nullptr, page * 3, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Check(guest != MAP_FAILED, "create real three-page guest window");
    Config config{};
    config.loc_start = page;
    config.loc_end = page * 4;
    config.memory_base = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(guest) - page);
    config.enable_jit = true;
#if defined(__riscv)
    config.backend_isa = kRiscv64;
#else
    config.backend_isa = kArm64;
#endif
    {
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        auto* block = new ir::Block(ir::Location{page});
        ir::NodeRef external{block};
        block->SetEndLocation(ir::Location{page * 4});
        Check(module->Push(block), "publish tracked node");
        auto& tracker = space.GetSmcTracker();
        SmcTracker::SetEnabled(true);
        tracker.RegisterNode(module, block, page, page * 4);
        Check(tracker.HasProtectedPages(), "actual guest pages are read protected");
        const auto control = space.GetRetirementControl();
        auto ticket = RetirementControl::RequestDrain(control);
        Check(!tracker.ShutdownAfterQuiescence(space, ticket).admission_ready && tracker.HasProtectedPages(),
              "ordinary drain cannot unprotect or seal SMC");
        Check(control->SealPublications(ticket) && control->Resume(ticket), "ordinary drain reopens");
        {
            AddressSpace other{config};
            const auto other_control = other.GetRetirementControl();
            const auto other_ticket = RetirementControl::RequestDrain(other_control, true);
            Check(other_control->SealPublications(other_ticket), "seal another AddressSpace");
            Check(!tracker.ShutdownAfterQuiescence(other, other_ticket).admission_ready &&
                  tracker.HasProtectedPages(), "another owner's ready ticket cannot unprotect this tracker");
        }
        TranslateTable l1{3};
        auto token = tracker.RegisterRuntime(l1);
        u64 request{};
        auto attached = std::make_unique<RetirementControl::ExitRegistration>(control, request);
        {
            RetirementControl::Lease producer{control, RetirementControl::Kind::Producer};
            ticket = RetirementControl::RequestDrain(control, true);
            Check(!tracker.ShutdownAfterQuiescence(space, ticket).admission_ready &&
                  tracker.HasProtectedPages(), "active producer prevents terminal SMC mutation");
        }
        Check(control->SealPublications(ticket), "seal the terminal ticket");
        Check(!tracker.ShutdownAfterQuiescence(space, ticket).admission_ready &&
              tracker.HasProtectedPages(), "idle Runtime attachment prevents terminal SMC mutation");
        attached.reset();
        const auto pending = tracker.ShutdownAfterQuiescence(space, ticket);
        Check(pending.admission_ready && pending.runtimes_attached && tracker.HasProtectedPages(),
              "even a low-level idle SMC token blocks terminal mutation");
        tracker.UnregisterRuntime(token);
        fail_unprotect = guest + page;
        failures_left = 2;
        if (diagnostic) {
            tracker.DisableAndUnprotectAll();
        } else {
            deny_allocations = true;
            const auto failed = tracker.ShutdownAfterQuiescence(space, ticket);
            deny_allocations = false;
            Check(!failed.Complete() && failed.remaining_pages == 1 &&
                  failed.failed_page == page * 2 && failed.protect_error == EACCES,
                  "partial unprotect preserves the failed page and immediate errno");
        }
        Check(tracker.HasProtectedPages() && failures_seen == 1, "failed record survives the first attempt");
        guest[0] = 1;
        guest[page * 2] = 2;
        Check(guest[0] == 1 && guest[page * 2] == 2, "successful pages are already writable");
        deny_allocations = true;
        const auto persistent = tracker.ShutdownAfterQuiescence(space, ticket);
        deny_allocations = false;
        Check(!persistent.Complete() && persistent.remaining_pages == 1 && failures_seen == 2,
              "a repeated failure still retains ownership");
        deny_allocations = true;
        const auto ready = tracker.ShutdownAfterQuiescence(space, ticket);
        deny_allocations = false;
        Check(ready.Complete() && !tracker.HasProtectedPages(), "allocation-free retry clears the last page");
        guest[page] = 3;
        Check(guest[page] == 3, "last page is actually writable after retry");
        bool rejected{};
        try { (void)tracker.RegisterRuntime(l1); }
        catch (const std::runtime_error&) { rejected = true; }
        Check(rejected, "terminal SMC rejects late low-level Runtime registration");
        Check(tracker.ShutdownAfterQuiescence(space, ticket).Complete(), "SMC terminal retry is idempotent");
        fail_unprotect = nullptr;
    }
    Check(munmap(guest, page * 3) == 0, "release the guest window");
}

void Reclaim() {
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = 0x4000;
    config.enable_jit = true;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::BlockLink;
#if defined(__riscv)
    config.backend_isa = kRiscv64;
#else
    config.backend_isa = kArm64;
#endif
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    const auto [id, buffer] = module->AllocCodeCache(64);
    Check(id != INVALID_CACHE_ID, "allocate real executable code");
    auto* block = new ir::Block(ir::Location{0x1000});
    ir::NodeRef external{block};
    block->SetEndLocation(ir::Location{0x1004});
    auto& cache = block->GetJitCache();
    cache.jit_state = JitState::Cached;
    cache.cache_id = id;
    cache.offset_in = buffer.offset;
    cache.cache_size = buffer.size;
    Check(module->Push(block), "publish the retired IR owner");
    module->RetainCodeIR(buffer.exec_data, block);
    module->AddFaultEntry(buffer.exec_data, buffer.exec_data + 64, 0x1000);
    auto& manager = space.GetLinkManager();
    const LinkSourceOwner owner{module.get(), buffer.exec_data};
    const auto region = module->GetCodeRegion(buffer.exec_data);
    Check(region.has_value(), "real retired allocation has a code region");
    const auto instruction = region->isa == kRiscv64 ? EncodeRiscvJal(8, 1)
            : EncodeBL(region->rx_base + region->trampoline_offset - buffer.exec_data);
    Check(instruction.has_value(), "retired site has a valid cold branch");
    *reinterpret_cast<u32*>(buffer.rw_data) = *instruction;
    buffer.Flush();
    const LinkSignalPatchSite patch{.region = *region, .rx_site = buffer.exec_data,
                                   .rw_site = buffer.rw_data, .unlinked_bl = *instruction};
    Check(manager.RegisterSite({region->id, buffer.offset}, 0x1000, owner, &patch),
          "register actual allocation ownership");
    Check(manager.PublishTarget(0x1000, buffer.exec_data, region->id, owner) != 0,
          "publish the allocation target");
    space.PushCodeCache(ir::Location{0x1000}, buffer.exec_data);
    TranslateTable l1{3};
    auto& tracker = space.GetSmcTracker();
    const auto token = tracker.RegisterRuntime(l1);
    tracker.BeginJit(token);
    Check(tracker.RetireNode(space, &l1, module, block), "retire code while an old execution token is active");
    FaultEntry fault;
    Check(module->LookupFault(buffer.exec_data, fault) && manager.GetStats().retiring == 1,
          "active token really retains pending fault/link/code ownership");
    deny_allocations = true;
    tracker.EndJit(token);
    deny_allocations = false;
    Check(!module->LookupFault(buffer.exec_data, fault) && manager.QuerySourceSites(owner).empty() &&
          !manager.QueryTarget(0x1000), "allocation-free active reclaimer removes actual fault/link owners");
    Check(cache.jit_state.get<JitState>() == JitState::None && IsEmpty(module->GetNode(ir::Location{0x1000})),
          "external IR handle has no executable publication");
    const auto [reused_id, reused] = module->AllocCodeCache(64);
    Check(reused_id == id && reused.exec_data == buffer.exec_data,
          "retired allocation is actually reusable after the execution token leaves");
    tracker.UnregisterRuntime(token);
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view mode{argv[1]};
        if (mode == "catalog") Catalog();
        else if (mode == "links") Links(false);
        else if (mode == "signals") Links(true);
        else if (mode == "smc") Smc(false);
        else if (mode == "disable") Smc(true);
        else if (mode == "reclaim") Reclaim();
        else return 2;
        std::printf("mode=%s checks=%u passed\n", argv[1], checks);
        return 0;
    } catch (const std::exception& error) {
        deny_allocations = false;
        std::fprintf(stderr, "failure: %s\n", error.what());
        return 1;
    }
}
