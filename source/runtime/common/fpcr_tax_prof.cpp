#include "runtime/common/fpcr_tax_prof.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <map>
#include <mutex>
#include <vector>

#include "runtime/common/perf_stats.h"

namespace swift::runtime {

namespace {

using Counter = std::atomic<unsigned long long>;

struct ProcessCounters {
    std::array<Counter, kFpcrTaxCounterCount> counters{};
};

struct ProcessTiming {
    std::mutex mutex{};
    u64 calls{};
    u64 dropped{};
    std::vector<FpcrTimingSample> samples{};
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

ProcessTiming& Timing() {
    static ProcessTiming timing{};
    return timing;
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
        u64 charged_events = values[i];
        if (i == static_cast<size_t>(FpcrTaxCounter::DirectHelper)) {
            const auto fp_free = values[static_cast<size_t>(
                    FpcrTaxCounter::DirectHelperFPFree)];
            charged_events = charged_events >= fp_free
                    ? charged_events - fp_free
                    : 0;
        }
        estimated += charged_events * static_instructions;
        std::fprintf(out,
                     "[svm-fpcr-tax-boundary] kind=%s events=%llu "
                     "charged_events=%llu "
                     "save_restore_static=%u cache_restore_static=%u "
                     "tax_static=%u tax_dynamic_est=%llu\n",
                     kCosts[i].name, PrintU64(values[i]), PrintU64(charged_events),
                     kCosts[i].save_restore,
                     kCosts[i].cache_restore,
                     kCosts[i].save_restore + kCosts[i].cache_restore,
                     PrintU64(charged_events * static_instructions));
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
    const auto direct_helpers =
            values[static_cast<size_t>(FpcrTaxCounter::DirectHelper)];
    const auto fp_free_helpers =
            values[static_cast<size_t>(FpcrTaxCounter::DirectHelperFPFree)];
    std::fprintf(out,
                 "[svm-fpcr-tax] tax_dynamic_est=%llu runtime_entry=%llu "
                 "runtime_return=%llu dispatcher_entry=%llu "
                 "dispatcher_per_runtime_entry=%.6f cache_lookups=%llu "
                 "cache_hits=%llu cache_misses=%llu rebuild_executed=%llu "
                 "cache_hit_pct=%.6f direct_helpers=%llu "
                 "fp_free_helpers=%llu fp_free_pct=%.6f\n",
                 PrintU64(estimated), PrintU64(entries), PrintU64(returns),
                 PrintU64(dispatch),
                 entries ? static_cast<double>(dispatch) /
                                   static_cast<double>(entries)
                         : 0.0,
                 PrintU64(lookups), PrintU64(hits), PrintU64(misses),
                 PrintU64(rebuilds),
                 lookups ? static_cast<double>(hits) * 100.0 /
                                   static_cast<double>(lookups)
                         : 0.0,
                 PrintU64(direct_helpers), PrintU64(fp_free_helpers),
                 direct_helpers ? static_cast<double>(fp_free_helpers) * 100.0 /
                                          static_cast<double>(direct_helpers)
                                : 0.0);
    std::fflush(out);
    if (close_out) std::fclose(out);
}

u64 TimerFrequency() {
#if defined(__aarch64__)
    u64 value{};
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(value));
    return value;
#else
    return 0;
#endif
}

VAddr ImageBase() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<const void*>(&FpcrTaxTimingEnabled), &info) &&
        info.dli_fbase) {
        return reinterpret_cast<VAddr>(info.dli_fbase);
    }
    return 0;
}

struct TimingDistribution {
    u64 minimum{};
    u64 p50{};
    u64 p90{};
    u64 p99{};
    u64 maximum{};
    double mean{};
};

TimingDistribution Distribution(std::vector<u64> values) {
    TimingDistribution out{};
    if (values.empty()) return out;
    std::sort(values.begin(), values.end());
    auto percentile = [&](u64 numerator) {
        return values[static_cast<size_t>(
                (values.size() - 1) * numerator / 100)];
    };
    long double sum = 0;
    for (u64 value : values) sum += value;
    out.minimum = values.front();
    out.p50 = percentile(50);
    out.p90 = percentile(90);
    out.p99 = percentile(99);
    out.maximum = values.back();
    out.mean = static_cast<double>(sum / values.size());
    return out;
}

void PrintDistribution(FILE* out,
                       const char* phase,
                       const TimingDistribution& dist,
                       size_t count,
                       u64 frequency) {
    const double ns_per_tick = frequency ? 1.0e9 / static_cast<double>(frequency) : 0.0;
    std::fprintf(out,
                 "[svm-fpcr-timing-dist] phase=%s samples=%zu "
                 "min_ticks=%llu p50_ticks=%llu p90_ticks=%llu p99_ticks=%llu "
                 "max_ticks=%llu mean_ticks=%.6f p50_ns=%.3f p99_ns=%.3f "
                 "mean_ns=%.3f\n",
                 phase, count, PrintU64(dist.minimum), PrintU64(dist.p50),
                 PrintU64(dist.p90), PrintU64(dist.p99), PrintU64(dist.maximum),
                 dist.mean, static_cast<double>(dist.p50) * ns_per_tick,
                 static_cast<double>(dist.p99) * ns_per_tick,
                 dist.mean * ns_per_tick);
}

void DumpTimingAtExit() {
    const char* destination = std::getenv("SVM_FPCR_TAX_TIMING");
    FILE* out = stderr;
    bool close_out = false;
    if (destination && *destination && std::strcmp(destination, "1") != 0 &&
        std::strcmp(destination, "stderr") != 0) {
        if (FILE* file = std::fopen(destination, "w")) {
            out = file;
            close_out = true;
        } else {
            std::fprintf(stderr,
                         "[svm-fpcr-timing] error=cannot_open path=%s; using stderr\n",
                         destination);
        }
    }

    auto& process = Timing();
    std::lock_guard lock{process.mutex};
    const u64 frequency = TimerFrequency();
    const VAddr image_base = ImageBase();
    std::fprintf(out,
                 "[svm-fpcr-timing] calls=%llu samples=%zu dropped=%llu "
                 "sample_period=%llu timer_hz=%llu unsafe_skip=%d "
                 "image_base=0x%llx\n",
                 PrintU64(process.calls), process.samples.size(),
                 PrintU64(process.dropped),
                 PrintU64(kFpcrTimingSampleMask + 1), PrintU64(frequency),
                 FpcrTaxSkipSwitchEnabled(),
                 static_cast<unsigned long long>(image_base));

    std::array<std::vector<u64>, 4> values;
    for (const auto& sample : process.samples) {
        if (!(sample.start <= sample.host_ready &&
              sample.host_ready <= sample.helper_done &&
              sample.helper_done <= sample.guest_ready)) {
            continue;
        }
        values[0].push_back(sample.host_ready - sample.start);
        values[1].push_back(sample.helper_done - sample.host_ready);
        values[2].push_back(sample.guest_ready - sample.helper_done);
        values[3].push_back(sample.guest_ready - sample.start);
    }
    constexpr std::array phases{"to_host", "helper", "to_guest", "total"};
    for (size_t i = 0; i < phases.size(); ++i) {
        PrintDistribution(out, phases[i], Distribution(values[i]),
                          values[i].size(), frequency);
    }

    std::map<std::pair<VAddr, bool>, std::vector<u64>> by_target;
    for (const auto& sample : process.samples) {
        if (sample.guest_ready >= sample.start) {
            by_target[{sample.target, sample.fpcr_transparent != 0}].push_back(
                    sample.guest_ready - sample.start);
        }
    }
    std::vector<std::pair<std::pair<VAddr, bool>, std::vector<u64>*>> ranked;
    ranked.reserve(by_target.size());
    for (auto& [target, samples] : by_target) {
        ranked.emplace_back(target, &samples);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (left.second->size() != right.second->size()) {
            return left.second->size() > right.second->size();
        }
        return left.first < right.first;
    });
    if (ranked.size() > 20) ranked.resize(20);
    const double ns_per_tick = frequency ? 1.0e9 / static_cast<double>(frequency) : 0.0;
    for (size_t rank = 0; rank < ranked.size(); ++rank) {
        const auto target = ranked[rank].first.first;
        const auto fp_free = ranked[rank].first.second;
        const auto dist = Distribution(*ranked[rank].second);
        std::fprintf(out,
                     "[svm-fpcr-timing-target] rank=%zu target=0x%llx "
                     "target_rva=0x%llx fp_free=%d samples=%zu "
                     "p50_ticks=%llu p99_ticks=%llu mean_ticks=%.6f "
                     "p50_ns=%.3f p99_ns=%.3f mean_ns=%.3f\n",
                     rank + 1,
                     static_cast<unsigned long long>(target),
                     static_cast<unsigned long long>(
                             image_base ? target - image_base : 0),
                     fp_free,
                     ranked[rank].second->size(), PrintU64(dist.p50),
                     PrintU64(dist.p99), dist.mean,
                     static_cast<double>(dist.p50) * ns_per_tick,
                     static_cast<double>(dist.p99) * ns_per_tick,
                     dist.mean * ns_per_tick);
    }

    std::map<std::pair<VAddr, u64>, std::vector<u64>> by_target_arg1;
    for (const auto& sample : process.samples) {
        if (sample.guest_ready >= sample.start) {
            by_target_arg1[{sample.target, sample.arg1}].push_back(
                    sample.guest_ready - sample.start);
        }
    }
    std::vector<std::pair<std::pair<VAddr, u64>, std::vector<u64>*>> arg_ranked;
    arg_ranked.reserve(by_target_arg1.size());
    for (auto& [key, samples] : by_target_arg1) {
        arg_ranked.emplace_back(key, &samples);
    }
    std::sort(arg_ranked.begin(), arg_ranked.end(), [](const auto& left,
                                                       const auto& right) {
        if (left.second->size() != right.second->size()) {
            return left.second->size() > right.second->size();
        }
        return left.first < right.first;
    });
    if (arg_ranked.size() > 20) arg_ranked.resize(20);
    for (size_t rank = 0; rank < arg_ranked.size(); ++rank) {
        const auto target = arg_ranked[rank].first.first;
        const auto arg1 = arg_ranked[rank].first.second;
        const auto dist = Distribution(*arg_ranked[rank].second);
        std::fprintf(out,
                     "[svm-fpcr-timing-target-arg1] rank=%zu target_rva=0x%llx "
                     "arg1=0x%llx samples=%zu p50_ticks=%llu p99_ticks=%llu "
                     "mean_ticks=%.6f\n",
                     rank + 1,
                     static_cast<unsigned long long>(
                             image_base ? target - image_base : 0),
                     static_cast<unsigned long long>(arg1),
                     arg_ranked[rank].second->size(), PrintU64(dist.p50),
                     PrintU64(dist.p99), dist.mean);
    }
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

bool FpcrTaxSkipSwitchEnabled() {
    static const bool enabled = [] {
        const char* value = PerfGetenv("SVM_FPCR_TAX_SKIP_SWITCH");
        const bool on = value && std::strcmp(value, "0") != 0;
        if (on) {
            std::fprintf(stderr,
                         "SVM_FPCR_TAX_SKIP_SWITCH: UNSAFE diagnostic; direct helpers "
                         "inherit guest FPCR\n");
        }
        return on;
    }();
    return enabled;
}

bool FpcrTaxTimingEnabled() {
    static const bool enabled = [] {
        const char* value = PerfGetenv("SVM_FPCR_TAX_TIMING");
        const bool on = value && std::strcmp(value, "0") != 0;
        if (on) {
            (void)Timing();
            std::atexit(DumpTimingAtExit);
        }
        return on;
    }();
    return enabled;
}

void FpcrTimingSubmit(const FpcrTimingBuffer& buffer) {
    if (!FpcrTaxTimingEnabled()) return;
    auto& process = Timing();
    std::lock_guard lock{process.mutex};
    process.calls += buffer.calls;
    process.dropped += buffer.dropped;
    const size_t count = std::min<size_t>(buffer.sample_count,
                                          buffer.samples.size());
    process.samples.insert(process.samples.end(), buffer.samples.begin(),
                           buffer.samples.begin() + count);
}

}  // namespace swift::runtime
