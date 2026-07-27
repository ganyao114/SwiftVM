//
// Translation-pipeline phase counters.
//
// Off unless SVM_PROF is set in the environment: every probe is guarded by one
// process-constant bool, and every probe sits on a "once per compiled unit"
// path -- never on a per-executed-guest-block path -- so enabling it cannot
// perturb the thing being measured (guest execution time is derived as
// wall - translate).
//
// The counters are far more useful than wall clock for this pipeline: unit
// counts and emitted byte counts are exactly reproducible run to run, and the
// per-phase nanoseconds are only compared *within* one process against each
// other, never across differently loaded machines.
//
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace swift::runtime {

struct PerfStats {
    // Phases of one compiled unit, in pipeline order. Anything the unit spends
    // outside these -- builder setup and teardown, module lookup, the driver's
    // own scans -- shows up as translate_ns minus their sum, which is where the
    // pipeline's fixed per-unit overhead is visible.
    std::atomic<unsigned long long> decode_ns{0};     // frontend decode + HIR build
    std::atomic<unsigned long long> rpo_ns{0};        // ComputeRPO + IdByRPO (both)
    std::atomic<unsigned long long> opt_ns{0};        // PassPipeline
    std::atomic<unsigned long long> regalloc_ns{0};   // RegisterAllocPass
    std::atomic<unsigned long long> codegen_ns{0};    // JitTranslator + Flush
    std::atomic<unsigned long long> publish_ns{0};    // module push, L2 slots, SMC
    std::atomic<unsigned long long> ir_free_ns{0};    // releasing the unit's IR
    std::atomic<unsigned long long> translate_ns{0};  // whole Translate() call

    std::atomic<unsigned long long> func_units{0};
    std::atomic<unsigned long long> block_units{0};
    std::atomic<unsigned long long> decoded_blocks{0};
    std::atomic<unsigned long long> host_bytes{0};
    std::atomic<unsigned long long> ir_insts{0};
    std::atomic<unsigned long long> pool_bytes{0};  // bytes handed to the allocator
    // High-water mark of the ir::Inst arena: chunk bytes malloc'd by
    // Inst::operator new. Chunks are never returned, so this only grows, and it
    // grows only when the free list cannot satisfy an allocation -- i.e. it is
    // exactly "peak IR held live at once", independent of host load and of
    // anything else in the address space. Retaining a compiled unit's IR makes
    // it track total IR ever built instead.
    std::atomic<unsigned long long> ir_arena_bytes{0};
};

inline PerfStats& GetPerfStats() {
    static PerfStats stats{};
    return stats;
}

void PerfDumpAtExit();

inline bool PerfEnabled() {
    static const bool enabled = [] {
        if (std::getenv("SVM_PROF") == nullptr) {
            return false;
        }
        std::atexit(PerfDumpAtExit);
        return true;
    }();
    return enabled;
}

inline void PerfDumpAtExit() {
    auto& s = GetPerfStats();
    auto g = [](const std::atomic<unsigned long long>& a) { return a.load(std::memory_order_relaxed); };
    std::fprintf(stderr,
                 "[svm-prof] translate_ns=%llu decode_ns=%llu rpo_ns=%llu "
                 "opt_ns=%llu regalloc_ns=%llu codegen_ns=%llu publish_ns=%llu "
                 "ir_free_ns=%llu\n",
                 g(s.translate_ns), g(s.decode_ns), g(s.rpo_ns), g(s.opt_ns),
                 g(s.regalloc_ns), g(s.codegen_ns), g(s.publish_ns), g(s.ir_free_ns));
    std::fprintf(stderr,
                 "[svm-prof] func_units=%llu block_units=%llu decoded_blocks=%llu "
                 "host_bytes=%llu ir_insts=%llu pool_bytes=%llu\n",
                 g(s.func_units), g(s.block_units), g(s.decoded_blocks), g(s.host_bytes),
                 g(s.ir_insts), g(s.pool_bytes));
    // Separate line on purpose: run_func_fingerprint_tests.sh keys its totals
    // gate on a line that *starts* with "func_units=", so anything appended to
    // the line above would have to be stripped there as well.
    std::fprintf(stderr, "[svm-prof] ir_arena_bytes=%llu\n", g(s.ir_arena_bytes));
}

inline void PerfAdd(std::atomic<unsigned long long>& counter, unsigned long long v) {
    counter.fetch_add(v, std::memory_order_relaxed);
}

// SVM_PROF=2 additionally prints one line per compiled unit, which is what
// makes a codegen difference attributable: diff the two runs' unit lists and
// the changed guest addresses fall out.
inline bool PerfPerUnit() {
    static const bool on = [] {
        const char* e = std::getenv("SVM_PROF");
        return e && std::atoi(e) >= 2;
    }();
    return on;
}

// Adds the lifetime of the enclosing scope to `counter`. Construction cost is
// one steady_clock::now(); only instantiate on per-unit paths.
class PerfScope {
public:
    explicit PerfScope(std::atomic<unsigned long long>& counter)
            : counter(PerfEnabled() ? &counter : nullptr) {
        if (this->counter) {
            start = std::chrono::steady_clock::now();
        }
    }
    ~PerfScope() { Stop(); }

    void Stop() {
        if (!counter) {
            return;
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
        counter->fetch_add(static_cast<unsigned long long>(ns), std::memory_order_relaxed);
        counter = nullptr;
    }

    PerfScope(const PerfScope&) = delete;
    PerfScope& operator=(const PerfScope&) = delete;

private:
    std::atomic<unsigned long long>* counter;
    std::chrono::steady_clock::time_point start{};
};

}  // namespace swift::runtime
