#include "ra_shape_prof.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "runtime/common/perf_stats.h"
#include "runtime/common/svm_config.h"
#include "runtime/backend/reg_alloc.h"
#include "runtime/ir/block.h"

namespace swift::runtime {

namespace {

using Counter = std::atomic<unsigned long long>;
constexpr size_t kRegBins = 33;
constexpr size_t kSpillBins = backend::kMaxSpillSlots + 1;
constexpr size_t kTargetSlots = 512;

struct AtomicHelperCounters {
    Counter calls{};
    Counter snapshot_instructions{};
    Counter snapshot_code_bytes{};
    Counter snapshot_memory_bytes{};
};

struct TargetCounters {
    std::atomic<VAddr> target{};
    std::array<AtomicHelperCounters,
               static_cast<size_t>(RAShapeHelperABI::Count)>
            abi{};
};

struct RAShapeProcessCounters {
    Counter units{};
    Counter spill_units{};
    Counter flags_units{};
    std::array<Counter, kRegBins> gpr_pool{};
    std::array<Counter, kRegBins> fpr_pool{};
    std::array<Counter, kRegBins> max_live_gpr{};
    std::array<Counter, kRegBins> max_live_fpr{};
    std::array<Counter, kRegBins> scratch_gpr{};
    std::array<Counter, kRegBins> scratch_fpr{};
    std::array<Counter, kSpillBins> spill_high_water{};
    Counter spill_defs{};
    Counter spill_loads{};
    Counter spill_stores{};
    Counter consecutive_pair_fallbacks{};
    Counter host_bytes{};
    Counter fixed_affinity_reads{};
    Counter fixed_affinity_writes{};
    Counter fixed_hazards{};
    Counter fixed_evictions{};
    Counter fixed_copies_elided{};
    std::array<AtomicHelperCounters,
               static_cast<size_t>(RAShapeHelperABI::Count)>
            helpers{};
    Counter pf_producers{};
    Counter af_producers{};
    Counter pf_direct_consumers{};
    Counter af_direct_consumers{};
    Counter pf_materialize{};
    Counter af_materialize{};
    Counter pf_force_edge{};
    Counter af_force_edge{};
    Counter pf_force_helper{};
    Counter af_force_helper{};
    Counter pf_force_fault{};
    Counter af_force_fault{};
    Counter pf_force_other{};
    Counter af_force_other{};
    std::array<TargetCounters, kTargetSlots> targets{};
    Counter target_overflow{};
};

RAShapeProcessCounters& Counters() {
    static RAShapeProcessCounters counters{};
    return counters;
}

unsigned long long Load(const Counter& counter) {
    return counter.load(std::memory_order_relaxed);
}

void Add(Counter& counter, u64 value) {
    counter.fetch_add(value, std::memory_order_relaxed);
}

void Add(AtomicHelperCounters& out, const RAShapeHelperCounters& in) {
    Add(out.calls, in.calls);
    Add(out.snapshot_instructions, in.snapshot_instructions);
    Add(out.snapshot_code_bytes, in.snapshot_code_bytes);
    Add(out.snapshot_memory_bytes, in.snapshot_memory_bytes);
}

template <size_t N>
void DumpHistogram(FILE* out, const char* name, const std::array<Counter, N>& histogram) {
    std::fprintf(out, "[svm-ra-shape-hist] name=%s bins=", name);
    bool first = true;
    for (size_t i = 0; i < N; ++i) {
        const auto count = Load(histogram[i]);
        if (!count) continue;
        std::fprintf(out, "%s%zu:%llu", first ? "" : ",", i, count);
        first = false;
    }
    if (first) std::fprintf(out, "empty");
    std::fputc('\n', out);
}

const char* HelperABIName(RAShapeHelperABI abi) {
    switch (abi) {
        case RAShapeHelperABI::DirectAAPCS:
            return "direct_aapcs";
        case RAShapeHelperABI::DirectPreserveAll:
            return "direct_preserve_all";
        case RAShapeHelperABI::IndirectAAPCS:
            return "indirect_aapcs";
        case RAShapeHelperABI::Count:
            break;
    }
    return "unknown";
}

// Stable ELF/Mach-O-image-relative anchor for resolving the direct target
// deltas in a post-link disassembly.  This deliberately avoids dladdr/dlsym in
// the translator and therefore adds no loader dependency to swift_runtime.
extern "C" __attribute__((noinline, used)) void svm_ra_shape_prof_anchor() {}

void DumpAtExit() {
    const auto& config = GetSvmConfig();
    const char* destination = config.ra_shape_prof_is_set
            ? config.ra_shape_prof.c_str()
            : nullptr;
    FILE* out = stderr;
    bool close_out = false;
    if (destination && *destination && std::strcmp(destination, "1") != 0 &&
        std::strcmp(destination, "stderr") != 0) {
        if (FILE* file = std::fopen(destination, "w")) {
            out = file;
            close_out = true;
        } else {
            std::fprintf(stderr,
                         "[svm-ra-shape] error=cannot_open path=%s; using stderr\n",
                         destination);
        }
    }

    auto& counters = Counters();
    std::fprintf(out,
                 "[svm-ra-shape] units=%llu spill_units=%llu flags_units=%llu "
                 "spill_defs=%llu spill_loads=%llu spill_stores=%llu "
                 "pair_fallbacks=%llu host_bytes=%llu target_overflow=%llu\n",
                 Load(counters.units), Load(counters.spill_units),
                 Load(counters.flags_units), Load(counters.spill_defs),
                 Load(counters.spill_loads), Load(counters.spill_stores),
                 Load(counters.consecutive_pair_fallbacks),
                 Load(counters.host_bytes),
                 Load(counters.target_overflow));
    std::fprintf(out,
                 "[svm-ra-shape-fixed] affinity_reads=%llu affinity_writes=%llu "
                 "hazards=%llu evictions=%llu copies_elided=%llu\n",
                 Load(counters.fixed_affinity_reads),
                 Load(counters.fixed_affinity_writes),
                 Load(counters.fixed_hazards), Load(counters.fixed_evictions),
                 Load(counters.fixed_copies_elided));
    DumpHistogram(out, "gpr_pool", counters.gpr_pool);
    DumpHistogram(out, "fpr_pool", counters.fpr_pool);
    DumpHistogram(out, "max_live_gpr", counters.max_live_gpr);
    DumpHistogram(out, "max_live_fpr", counters.max_live_fpr);
    DumpHistogram(out, "scratch_gpr", counters.scratch_gpr);
    DumpHistogram(out, "scratch_fpr", counters.scratch_fpr);
    DumpHistogram(out, "spill_high_water", counters.spill_high_water);

    for (size_t i = 0; i < static_cast<size_t>(RAShapeHelperABI::Count); ++i) {
        const auto abi = static_cast<RAShapeHelperABI>(i);
        const auto& helper = counters.helpers[i];
        std::fprintf(out,
                     "[svm-ra-shape-helper] abi=%s calls=%llu "
                     "snapshot_instructions=%llu snapshot_code_bytes=%llu "
                     "snapshot_memory_bytes=%llu\n",
                     HelperABIName(abi), Load(helper.calls),
                     Load(helper.snapshot_instructions),
                     Load(helper.snapshot_code_bytes),
                     Load(helper.snapshot_memory_bytes));
    }

    std::fprintf(out,
                 "[svm-ra-shape-flags] pf_producers=%llu af_producers=%llu "
                 "pf_direct=%llu af_direct=%llu pf_materialize=%llu "
                 "af_materialize=%llu pf_force_edge=%llu af_force_edge=%llu "
                 "pf_force_helper=%llu af_force_helper=%llu "
                 "pf_force_fault=%llu af_force_fault=%llu "
                 "pf_force_other=%llu af_force_other=%llu\n",
                 Load(counters.pf_producers), Load(counters.af_producers),
                 Load(counters.pf_direct_consumers),
                 Load(counters.af_direct_consumers),
                 Load(counters.pf_materialize), Load(counters.af_materialize),
                 Load(counters.pf_force_edge), Load(counters.af_force_edge),
                 Load(counters.pf_force_helper), Load(counters.af_force_helper),
                 Load(counters.pf_force_fault), Load(counters.af_force_fault),
                 Load(counters.pf_force_other), Load(counters.af_force_other));

    const auto anchor = reinterpret_cast<VAddr>(&svm_ra_shape_prof_anchor);
    for (const auto& target : counters.targets) {
        const auto address = target.target.load(std::memory_order_relaxed);
        if (!address) continue;
        for (size_t i = 0; i < static_cast<size_t>(RAShapeHelperABI::Count); ++i) {
            const auto& helper = target.abi[i];
            if (!Load(helper.calls)) continue;
            std::fprintf(out,
                         "[svm-ra-shape-target] delta=%lld abi=%s calls=%llu "
                         "snapshot_instructions=%llu snapshot_code_bytes=%llu "
                         "snapshot_memory_bytes=%llu\n",
                         static_cast<long long>(static_cast<std::intptr_t>(address) -
                                                static_cast<std::intptr_t>(anchor)),
                         HelperABIName(static_cast<RAShapeHelperABI>(i)),
                         Load(helper.calls), Load(helper.snapshot_instructions),
                         Load(helper.snapshot_code_bytes),
                         Load(helper.snapshot_memory_bytes));
        }
    }
    std::fflush(out);
    if (close_out) std::fclose(out);
}

bool IsHelperBoundary(ir::OpCode op) {
    return op == ir::OpCode::CallLambda || op == ir::OpCode::CallLocation ||
           op == ir::OpCode::CallDynamic || op == ir::OpCode::X87Op;
}

bool IsFaultBoundary(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::StoreMemoryTSO:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::CompareAndSwap128:
        case ir::OpCode::CheckMemoryAlignment:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
            return true;
        default:
            return false;
    }
}

}  // namespace

bool RAShapeProfEnabled() {
    const auto& config = GetSvmConfig();
    const bool enabled = config.ra_shape_prof_is_set && config.ra_shape_prof != "0";
    if (enabled) {
        static const bool registered = [] {
            std::atexit(DumpAtExit);
            return true;
        }();
        (void)registered;
    }
    return enabled;
}

void RAShapeAnalyzeFlags(const ir::Block* block, RAShapeUnitCounters& unit) {
    if (!RAShapeProfEnabled()) return;

    bool pending_pf = false;
    bool pending_af = false;
    auto force = [&](bool observe_pf, bool observe_af, u64& pf, u64& af) {
        if (observe_pf && pending_pf) {
            ++pf;
            pending_pf = false;
        }
        if (observe_af && pending_af) {
            ++af;
            pending_af = false;
        }
    };

    for (const auto& inst : block->GetInstList()) {
        const auto op = inst.GetOp();
        if (op == ir::OpCode::SaveFlags) {
            const auto flags = inst.GetArg<ir::Flags>(1);
            if (True(flags & ir::Flags::Parity)) {
                ++unit.pf_producers;
                ++unit.pf_materialize;
                pending_pf = true;
            }
            if (True(flags & ir::Flags::AuxiliaryCarry)) {
                ++unit.af_producers;
                ++unit.af_materialize;
                pending_af = true;
            }
            continue;
        }
        if (op == ir::OpCode::LocalParitySet) {
            // This is the already-shipped branch-only source form: the PF
            // producer and its sole consumer remain local and no x26 write is
            // emitted. Count both ends so the producer denominator includes
            // the optimization that already succeeded.
            ++unit.pf_producers;
            ++unit.pf_direct_consumers;
            continue;
        }
        if (op == ir::OpCode::ClearFlags) {
            const auto flags = inst.GetArg<ir::Flags>(0);
            if (True(flags & ir::Flags::Parity)) pending_pf = false;
            if (True(flags & ir::Flags::AuxiliaryCarry)) pending_af = false;
            continue;
        }
        if (op == ir::OpCode::PublishFCmpFlags) {
            // Both compact and ordinary FCMP publication write PF's packed
            // source into x26 and replace AF with the architectural constant
            // zero.  A source-backed experiment could keep the packed FCMP
            // value live until an observation, so PF remains pending here.
            ++unit.pf_producers;
            ++unit.pf_materialize;
            pending_pf = true;
            pending_af = false;
            continue;
        }
        if (op == ir::OpCode::TestFlags || op == ir::OpCode::TestNotFlags) {
            const auto flags = inst.GetArg<ir::Flags>(0);
            if (True(flags & ir::Flags::Parity)) ++unit.pf_direct_consumers;
            if (True(flags & ir::Flags::AuxiliaryCarry)) ++unit.af_direct_consumers;
            // A source value may feed a local test without committing x26.
            // Keep it pending because a later edge/fault/helper may still
            // require the same value to become architectural.
            continue;
        }
        if (op == ir::OpCode::FCmpCondSet) {
            const auto cond = inst.GetArg<ir::Cond>(1);
            if (cond == ir::Cond::VS || cond == ir::Cond::VC) {
                ++unit.pf_direct_consumers;
            }
            continue;
        }
        if (op == ir::OpCode::GetFlags) {
            const auto flags = inst.GetArg<ir::Flags>(1);
            force(True(flags & ir::Flags::Parity),
                  True(flags & ir::Flags::AuxiliaryCarry),
                  unit.pf_force_other,
                  unit.af_force_other);
        } else if (IsHelperBoundary(op)) {
            force(true, true, unit.pf_force_helper, unit.af_force_helper);
        } else if (IsFaultBoundary(op)) {
            force(true, true, unit.pf_force_fault, unit.af_force_fault);
        }
    }
    // Any source still pending at a block terminal must be made architectural
    // before the edge, even when the successor is inside a function unit.
    force(true, true, unit.pf_force_edge, unit.af_force_edge);
}

void RAShapeRecordHelperTarget(VAddr target,
                               RAShapeHelperABI abi,
                               const RAShapeHelperCounters& counters) {
    if (!RAShapeProfEnabled() || !target) return;
    auto& process = Counters();
    size_t slot = (target >> 4) % kTargetSlots;
    for (size_t probe = 0; probe < kTargetSlots; ++probe) {
        auto& entry = process.targets[(slot + probe) % kTargetSlots];
        VAddr observed = entry.target.load(std::memory_order_relaxed);
        if (!observed) {
            VAddr expected = 0;
            if (entry.target.compare_exchange_strong(
                        expected, target, std::memory_order_relaxed)) {
                observed = target;
            } else {
                observed = expected;
            }
        }
        if (observed == target) {
            Add(entry.abi[static_cast<size_t>(abi)], counters);
            return;
        }
    }
    Add(process.target_overflow, 1);
}

void RAShapeSubmitUnit(const RAShapeUnitCounters& unit) {
    if (!RAShapeProfEnabled() || !unit.ra_valid) return;
    auto& out = Counters();
    Add(out.units, 1);
    if (unit.spill_defs) Add(out.spill_units, 1);
    if (unit.pf_producers || unit.af_producers) Add(out.flags_units, 1);
    Add(out.gpr_pool[std::min<size_t>(unit.gpr_pool, kRegBins - 1)], 1);
    Add(out.fpr_pool[std::min<size_t>(unit.fpr_pool, kRegBins - 1)], 1);
    Add(out.max_live_gpr[std::min<size_t>(unit.max_live_gpr, kRegBins - 1)], 1);
    Add(out.max_live_fpr[std::min<size_t>(unit.max_live_fpr, kRegBins - 1)], 1);
    Add(out.scratch_gpr[std::min<size_t>(unit.scratch_gpr, kRegBins - 1)], 1);
    Add(out.scratch_fpr[std::min<size_t>(unit.scratch_fpr, kRegBins - 1)], 1);
    Add(out.spill_high_water[
                std::min<size_t>(unit.spill_high_water, kSpillBins - 1)],
        1);
    Add(out.spill_defs, unit.spill_defs);
    Add(out.spill_loads, unit.spill_loads);
    Add(out.spill_stores, unit.spill_stores);
    Add(out.consecutive_pair_fallbacks, unit.consecutive_pair_fallbacks);
    Add(out.host_bytes, unit.host_bytes);
    Add(out.fixed_affinity_reads, unit.fixed_affinity_reads);
    Add(out.fixed_affinity_writes, unit.fixed_affinity_writes);
    Add(out.fixed_hazards, unit.fixed_hazards);
    Add(out.fixed_evictions, unit.fixed_evictions);
    Add(out.fixed_copies_elided, unit.fixed_copies_elided);
    for (size_t i = 0; i < static_cast<size_t>(RAShapeHelperABI::Count); ++i) {
        Add(out.helpers[i], unit.helpers[i]);
    }
    Add(out.pf_producers, unit.pf_producers);
    Add(out.af_producers, unit.af_producers);
    Add(out.pf_direct_consumers, unit.pf_direct_consumers);
    Add(out.af_direct_consumers, unit.af_direct_consumers);
    Add(out.pf_materialize, unit.pf_materialize);
    Add(out.af_materialize, unit.af_materialize);
    Add(out.pf_force_edge, unit.pf_force_edge);
    Add(out.af_force_edge, unit.af_force_edge);
    Add(out.pf_force_helper, unit.pf_force_helper);
    Add(out.af_force_helper, unit.af_force_helper);
    Add(out.pf_force_fault, unit.pf_force_fault);
    Add(out.af_force_fault, unit.af_force_fault);
    Add(out.pf_force_other, unit.pf_force_other);
    Add(out.af_force_other, unit.af_force_other);
}

}  // namespace swift::runtime
