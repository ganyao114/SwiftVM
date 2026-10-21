#include <array>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <thread>

#include "base/scope_exit.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/fault_catalog.h"

namespace {
thread_local bool fail_next_allocation{};
thread_local int allocations_until_failure{-1};
thread_local bool signal_next_allocation{};
volatile std::sig_atomic_t in_signal{};
unsigned checks{};
}

void* operator new(std::size_t size) {
    if (in_signal) std::_Exit(91);
    if (std::exchange(fail_next_allocation, false) || allocations_until_failure == 0) {
        allocations_until_failure = -1;
        throw std::bad_alloc{};
    }
    if (allocations_until_failure > 0) --allocations_until_failure;
    if (std::exchange(signal_next_allocation, false)) std::raise(SIGUSR1);
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;

namespace {
void Check(bool condition, const char* detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}

FaultEntry Entry(u8* start, u8* end, u8* owner, u64 guest, u8* recovery = nullptr) {
    return {start, end, owner, recovery, guest, FaultRecoveryKind::GuestFault};
}

void Snapshots() {
    FaultCatalog catalog;
    std::array<u8, 256> code{};
    const int module{};
    auto* base = code.data();
    catalog.Add(&module, Entry(base, base + 64, base, 1));
    FaultEntry result;
    Check(catalog.Lookup(base, result) && result.guest_loc == 1, "whole allocation lookup failed");
    Check(!catalog.Lookup(base + 64, result), "fault ranges must be half-open");
    {
        FaultCatalog::Reader held{catalog};
        catalog.Add(&module, Entry(base, base + 16, base, 2, base + 56));
        catalog.Add(&module, Entry(base, base + 16, base, 3, base + 60));
        Check(held.Lookup(base, result) && result.guest_loc == 2 && result.recovery == base + 56,
              "equal-start recovery priority or original insertion order changed");
        catalog.Add(&module, Entry(base + 16, base + 48, base, 4, base + 56));
        Check(held.Lookup(base + 32, result) && result.guest_loc == 4,
              "specific containing start was not preferred");
        std::vector<FaultEntry> sites;
        Check(held.LookupAllocation(&module, base + 32, result, &sites) &&
                      result.host_end == base + 64 && sites.size() == 4,
              "whole allocation and recovery-site export diverged");
        catalog.Collect();
        Check(catalog.GetStats().retired_snapshots == 3, "live reader did not retain old snapshots");
        catalog.RemoveAllocation(&module, base);
        Check(!held.Lookup(base, result), "removed allocation remains discoverable");
        const auto retired = catalog.GetStats();
        Check(retired.retired_allocations == 1 && retired.retired_snapshots == 4,
              "live reader did not retain the unlinked directory node");
    }
    catalog.Collect();
    const auto empty = catalog.GetStats();
    Check(empty.allocations == 0 && empty.retired_snapshots == 0 &&
                  empty.retired_allocations == 0 && empty.readers == 0,
          "reader grace did not release metadata");

    catalog.Add(&module, Entry(base + 16, base + 48, base + 16, 5));
    catalog.Add(&module, Entry(base, base + 64, base, 6));
    FaultCatalog::Reader scope{catalog};
    Check(scope.LookupAllocation(&module, base + 32, result, nullptr) &&
                  result.owner_start == base + 16 && result.guest_loc == 5,
          "cross-owner overlap chose directory order instead of containing start");
    for (int stage = 0; stage < 3; ++stage) {
        for (const bool existing : {false, true}) {
            bool failed{};
            allocations_until_failure = stage;
            try {
                const auto entry = existing ? Entry(base + 24, base + 32, base + 16, 7)
                                            : Entry(base + 64, base + 96, base + 64, 7);
                catalog.Add(&module, entry);
            } catch (const std::bad_alloc&) { failed = true; }
            allocations_until_failure = -1;
            Check(failed && catalog.GetStats().allocations == 2 &&
                          !scope.Lookup(base + 80, result) &&
                          scope.Lookup(base + 28, result) && result.guest_loc == 5,
                  "failed snapshot copy/insert or directory allocation changed published entries");
        }
    }
    fail_next_allocation = true;
    catalog.RemoveAllocation(&module, base);
    catalog.RemoveModule(&module);
    Check(fail_next_allocation, "metadata removal unexpectedly allocated");
    fail_next_allocation = false;
}

FaultCatalog* signal_catalog{};
u8* signal_pc{};
volatile std::sig_atomic_t signals{};
volatile std::sig_atomic_t valid_signal{};

void LookupSignal(int) {
    in_signal = 1;
    FaultEntry result;
    valid_signal = signal_catalog->Lookup(signal_pc, result) && result.guest_loc == 11;
    signals = signals + 1;
    in_signal = 0;
}

void Signal() {
    FaultCatalog catalog;
    const int module{};
    std::array<u8, 64> code{};
    catalog.Add(&module, Entry(code.data(), code.data() + code.size(), code.data(), 11));
    signal_catalog = &catalog;
    signal_pc = code.data() + 8;
    struct sigaction action{}, previous{};
    action.sa_handler = LookupSignal;
    sigemptyset(&action.sa_mask);
    Check(sigaction(SIGUSR1, &action, &previous) == 0, "cannot install catalog signal probe");
    const auto restore = ScopeExit([&] { sigaction(SIGUSR1, &previous, nullptr); });
    // This allocation happens after Add takes writer_mutex. A lock-taking
    // signal reader would deadlock here; an allocating reader exits91.
    signal_next_allocation = true;
    catalog.Add(&module, Entry(code.data() + 16, code.data() + 32, code.data(), 12));
    signal_next_allocation = false;
    Check(signals == 1 && valid_signal == 1, "signal interrupted a writer without valid recovery metadata");
    Check(catalog.GetStats().readers == 0, "signal reader did not leave its grace scope");
}

void Concurrent() {
    FaultCatalog catalog;
    const int module{};
    std::array<u8, 64> code{};
    std::atomic<bool> start{}, stop{}, failed{};
    std::atomic<unsigned> ready{};
    std::array<std::atomic<unsigned>, 4> hits{};
    std::array<std::thread, 4> readers;
    catalog.Add(&module, Entry(code.data(), code.data() + 64, code.data(), 0));
    for (size_t index = 0; index < readers.size(); ++index) {
        readers[index] = std::thread([&, index] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            while (!stop.load(std::memory_order_acquire)) {
                FaultEntry result;
                if (catalog.Lookup(code.data() + 8, result)) {
                    if (result.host_start != code.data() || result.host_end != code.data() + 64 ||
                        result.owner_start != code.data() || result.guest_loc > 1999)
                        failed.store(true, std::memory_order_relaxed);
                    hits[index].fetch_add(1, std::memory_order_release);
                }
            }
        });
    }
    while (ready.load(std::memory_order_acquire) != readers.size()) std::this_thread::yield();
    start.store(true, std::memory_order_release);
    for (auto& count : hits)
        while (count.load(std::memory_order_acquire) == 0) std::this_thread::yield();
    catalog.RemoveAllocation(&module, code.data());
    for (u64 iteration = 0; iteration < 2000; ++iteration) {
        catalog.Add(&module, Entry(code.data(), code.data() + 64, code.data(), iteration));
        catalog.RemoveAllocation(&module, code.data());
    }
    stop.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();
    catalog.Collect();
    const auto final = catalog.GetStats();
    Check(!failed.load(), "concurrent lookup observed invalid metadata");
    for (auto& count : hits) Check(count.load() > 0, "one concurrent reader never obtained valid metadata");
    Check(final.readers == 0 && final.allocations == 0 && final.retired_snapshots == 0 &&
                  final.retired_allocations == 0,
          "concurrent publication/removal leaves metadata after grace");
}

void Integration() {
    const Config config{.loc_start = 0, .loc_end = 4096, .enable_jit = true,
#if defined(__riscv) && __riscv_xlen == 64
                        .backend_isa = kRiscv64};
#else
                        .backend_isa = kArm64};
#endif
    std::array<u8, 128> bytes{};
    std::shared_ptr<Module> retained;
    std::shared_ptr<FaultCatalog> catalog;
    FaultEntry result;
    {
        AddressSpace space{config};
        retained = space.MapModule(0x100, 0x200, ModuleConfig{.read_only = true});
        catalog = space.GetFaultCatalog();
        retained->AddFaultEntry(bytes.data(), bytes.data() + 64, 0x100, bytes.data(), bytes.data() + 56);
        space.UnmapModule(0x100, 0x200);
        Check(space.LookupFault(bytes.data() + 8, result) && result.guest_loc == 0x100,
              "routing removal lost live Module fault metadata");
        retained->RemoveFaultEntries(bytes.data());
        Check(!space.LookupFault(bytes.data() + 8, result), "rollback removal did not reach central catalog");
        retained->AddFaultEntry(bytes.data(), bytes.data() + 64, 0x100);
        auto writable = space.MapModule(0x300, 0x400, ModuleConfig{});
        const auto [cache_id, buffer] = writable->AllocCodeCache(32);
        Check(buffer.exec_data != nullptr, "cannot allocate actual code for ReclaimCode integration");
        writable->AddFaultEntry(buffer.exec_data, buffer.exec_data + buffer.size, 0x300);
        Check(space.LookupFault(buffer.exec_data, result), "allocated code metadata was not published");
        writable->ReclaimCode(buffer.exec_data);
        Check(!space.LookupFault(buffer.exec_data, result), "ReclaimCode did not reach central catalog");
        const auto [reused_id, reused] = writable->AllocCodeCache(32);
        Check(reused_id == cache_id && reused.exec_data == buffer.exec_data,
              "ReclaimCode did not return the actual allocation for reuse");
        space.GetDefaultModule()->AddFaultEntry(bytes.data() + 64, bytes.data() + 128, 0x300);
        Check(space.LookupFault(bytes.data() + 80, result) && result.guest_loc == 0x300,
              "default Module fault metadata was not published");
    }
    Check(!catalog->Lookup(bytes.data() + 8, result), "terminal shutdown leaves retained Module fault aliases");
    Check(!catalog->Lookup(bytes.data() + 80, result), "default Module metadata survives its destruction");
    retained.reset();
    Check(!catalog->Lookup(bytes.data() + 8, result) && !catalog->Lookup(bytes.data() + 80, result),
          "Module destruction did not remove central records independently of AddressSpace");
    catalog->Collect();
    Check(catalog->GetStats().allocations == 0, "Module teardown retains allocation metadata");
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view mode{argv[1]};
        if (mode == "snapshots") Snapshots();
        else if (mode == "signal") Signal();
        else if (mode == "concurrent") Concurrent();
        else if (mode == "integration") Integration();
        else return 2;
        std::cout << "mode=" << mode << " checks=" << checks << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        fail_next_allocation = false;
        allocations_until_failure = -1;
        signal_next_allocation = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
