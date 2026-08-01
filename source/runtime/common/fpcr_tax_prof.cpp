#include "runtime/common/fpcr_tax_prof.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime/common/perf_stats.h"

namespace swift::runtime {

namespace {

using Counter = std::atomic<unsigned long long>;

struct ProcessCounters {
    std::array<Counter, kFpcrTaxCounterCount> counters{};
};

struct Cost {
    const char* name;
    u32 save_restore;
    u32 cache_restore;
};

// Counts only instructions introduced by AFP FPCR ownership. The shared
// runtime-entry MOV state,x0 and ordinary helper plumbing are excluded.
constexpr std::array<Cost, kFpcrTaxBoundaryCount> kCosts{{
        {"runtime_entry", 3, 11},
        {"runtime_return", 2, 0},
        {"dispatcher_entry", 0, 0},
        {"asm_interpreter", 2, 5},
        {"call_host", 2, 5},
        {"direct_helper", 2, 5},
        {"memory_copy", 2, 5},
        {"store_mxcsr", 0, 5},
}};

static_assert(kCosts.size() == kFpcrTaxBoundaryCount);

ProcessCounters& Counters() {
    static ProcessCounters counters{};
    return counters;
}

unsigned long long PrintU64(u64 value) {
    return static_cast<unsigned long long>(value);
}

void DumpAtExit() {
    const char* destination = std::getenv("SVM_FPCR_TAX_PROF");
    FILE* out = stderr;
    bool close_out = false;
    if (destination && *destination && std::strcmp(destination, "1") != 0 &&
        std::strcmp(destination, "stderr") != 0) {
        if (FILE* file = std::fopen(destination, "w")) {
            out = file;
            close_out = true;
        } else {
            std::fprintf(stderr,
                         "[svm-fpcr-tax] error=cannot_open path=%s; using stderr\n",
                         destination);
        }
    }

    auto& process = Counters();
    std::array<u64, kFpcrTaxCounterCount> values{};
    u64 estimated = 0;
    for (size_t i = 0; i < values.size(); ++i) {
        values[i] = process.counters[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < kFpcrTaxBoundaryCount; ++i) {
        const auto static_instructions =
                static_cast<u64>(kCosts[i].save_restore + kCosts[i].cache_restore);
        estimated += values[i] * static_instructions;
        std::fprintf(out,
                     "[svm-fpcr-tax-boundary] kind=%s events=%llu "
                     "save_restore_static=%u cache_restore_static=%u "
                     "tax_static=%u tax_dynamic_est=%llu\n",
                     kCosts[i].name, PrintU64(values[i]), kCosts[i].save_restore,
                     kCosts[i].cache_restore,
                     kCosts[i].save_restore + kCosts[i].cache_restore,
                     PrintU64(values[i] * static_instructions));
    }

    const auto lookups =
            values[static_cast<size_t>(FpcrTaxCounter::CacheLookup)];
    const auto hits = values[static_cast<size_t>(FpcrTaxCounter::CacheHit)];
    const auto rebuilds =
            values[static_cast<size_t>(FpcrTaxCounter::RebuildExecuted)];
    const auto misses = lookups >= hits ? lookups - hits : 0;
    // A miss executes eleven more instructions than the five-instruction hit
    // path already charged to its boundary: nine MXCSR->FPCR operations, one
    // cache STP and one branch to the shared MSR.
    constexpr u64 kCacheMissExtraInstructions = 11;
    estimated += misses * kCacheMissExtraInstructions;

    const auto entries =
            values[static_cast<size_t>(FpcrTaxCounter::RuntimeEntry)];
    const auto returns =
            values[static_cast<size_t>(FpcrTaxCounter::RuntimeReturn)];
    const auto dispatch =
            values[static_cast<size_t>(FpcrTaxCounter::DispatcherEntry)];
    std::fprintf(out,
                 "[svm-fpcr-tax] tax_dynamic_est=%llu runtime_entry=%llu "
                 "runtime_return=%llu dispatcher_entry=%llu "
                 "dispatcher_per_runtime_entry=%.6f cache_lookups=%llu "
                 "cache_hits=%llu cache_misses=%llu rebuild_executed=%llu "
                 "cache_hit_pct=%.6f\n",
                 PrintU64(estimated), PrintU64(entries), PrintU64(returns),
                 PrintU64(dispatch),
                 entries ? static_cast<double>(dispatch) /
                                   static_cast<double>(entries)
                         : 0.0,
                 PrintU64(lookups), PrintU64(hits), PrintU64(misses),
                 PrintU64(rebuilds),
                 lookups ? static_cast<double>(hits) * 100.0 /
                                   static_cast<double>(lookups)
                         : 0.0);
    std::fflush(out);
    if (close_out) std::fclose(out);
}

}  // namespace

bool FpcrTaxProfEnabled() {
    static const bool enabled = [] {
        const char* value = PerfGetenv("SVM_FPCR_TAX_PROF");
        const bool on = value && std::strcmp(value, "0") != 0;
        if (on) {
            // Register after constructing storage: atexit callbacks run in
            // reverse order and must observe live process counters.
            (void)Counters();
            std::atexit(DumpAtExit);
        }
        return on;
    }();
    return enabled;
}

void FpcrTaxSubmit(std::span<const u64> counters) {
    if (!FpcrTaxProfEnabled() || counters.empty()) return;
    ASSERT(counters.size() == kFpcrTaxCounterCount);
    auto& process = Counters();
    for (size_t i = 0; i < counters.size(); ++i) {
        if (counters[i]) {
            process.counters[i].fetch_add(counters[i], std::memory_order_relaxed);
        }
    }
}

}  // namespace swift::runtime
