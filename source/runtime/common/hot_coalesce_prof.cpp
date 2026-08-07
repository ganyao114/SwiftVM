#include "runtime/common/hot_coalesce_prof.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string_view>
#include <type_traits>
#include <vector>

#include "runtime/common/perf_stats.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/block.h"
#include "runtime/ir/ir_types.h"

namespace swift::runtime {

namespace {

using Counter = std::atomic<unsigned long long>;

struct ProcessSlot {
    HotCoalesceUnitStatic shape{};
    std::array<Counter, kHotCoalesceCounterCount> dynamic{};
};

struct ProcessCounters {
    std::array<ProcessSlot, kHotCoalesceMaxUnits> slots{};
    std::atomic<u32> next_slot{};
    Counter overflow{};
};

ProcessCounters& Counters() {
    static ProcessCounters counters{};
    return counters;
}

u64 Load(const Counter& counter) {
    return counter.load(std::memory_order_relaxed);
}

unsigned long long PrintU64(u64 value) {
    return static_cast<unsigned long long>(value);
}

u64 Dynamic(const ProcessSlot& slot, HotCoalesceCounter counter) {
    return Load(slot.dynamic[static_cast<u32>(counter)]);
}

double Percent(u64 value, u64 total) {
    return total ? static_cast<double>(value) * 100.0 / static_cast<double>(total) : 0.0;
}

struct AggregateBucket {
    VAddr guest_entry{};
    u32 versions{};
    u32 host_static_max{};
    u32 spill_static_max{};
    u32 spill_static_min{UINT32_MAX};
    u32 reload_static_max{};
    u32 writeback_static_max{};
    u32 move_static_max{};
    u32 nan_static_max{};
    u32 state_sequences_max{};
    u32 state_load_pairs_max{};
    u32 state_store_pairs_max{};
    u32 state_same_offset_max{};
    u32 state_saved_static_max{};
    u64 entries{};
    u64 host_dynamic{};
    u64 spill_reloads{};
    u64 spill_writebacks{};
    u64 move_dynamic{};
    u64 nan_dynamic{};
    u64 state_saved_dynamic{};
    u64 indirect_l1_hits{};
    u64 indirect_l1_misses{};
};

struct RankedBucket {
    size_t bucket{};
    u64 value{};
};

u64 HostDynamic(const ProcessSlot& slot) {
    return Dynamic(slot, HotCoalesceCounter::Entries) * slot.shape.host_instructions;
}

u64 MoveDynamic(const ProcessSlot& slot) {
    return Dynamic(slot, HotCoalesceCounter::Entries) * slot.shape.move_bridges;
}

u64 StateSavedDynamic(const ProcessSlot& slot) {
    return Dynamic(slot, HotCoalesceCounter::Entries) *
           slot.shape.uniform.saved_instructions;
}

std::vector<AggregateBucket> BuildBuckets(u32 count) {
    auto& process = Counters();
    std::map<VAddr, AggregateBucket> by_pc;
    for (u32 i = 0; i < count; ++i) {
        const auto& slot = process.slots[i];
        auto& bucket = by_pc[slot.shape.guest_entry];
        bucket.guest_entry = slot.shape.guest_entry;
        ++bucket.versions;
        bucket.host_static_max =
                std::max(bucket.host_static_max, slot.shape.host_instructions);
        const u32 spill_static =
                slot.shape.spill_reloads + slot.shape.spill_writebacks;
        bucket.spill_static_max = std::max(bucket.spill_static_max, spill_static);
        bucket.spill_static_min = std::min(bucket.spill_static_min, spill_static);
        bucket.reload_static_max =
                std::max(bucket.reload_static_max, slot.shape.spill_reloads);
        bucket.writeback_static_max =
                std::max(bucket.writeback_static_max, slot.shape.spill_writebacks);
        bucket.move_static_max =
                std::max(bucket.move_static_max, slot.shape.move_bridges);
        bucket.nan_static_max =
                std::max(bucket.nan_static_max, slot.shape.nan_guard_instructions);
        bucket.state_sequences_max =
                std::max(bucket.state_sequences_max, slot.shape.uniform.sequences);
        bucket.state_load_pairs_max =
                std::max(bucket.state_load_pairs_max, slot.shape.uniform.load_pairs);
        bucket.state_store_pairs_max =
                std::max(bucket.state_store_pairs_max, slot.shape.uniform.store_pairs);
        bucket.state_same_offset_max =
                std::max(bucket.state_same_offset_max, slot.shape.uniform.same_offset);
        bucket.state_saved_static_max = std::max(
                bucket.state_saved_static_max, slot.shape.uniform.saved_instructions);
        bucket.entries += Dynamic(slot, HotCoalesceCounter::Entries);
        bucket.host_dynamic += HostDynamic(slot);
        bucket.spill_reloads +=
                Dynamic(slot, HotCoalesceCounter::SpillReloads);
        bucket.spill_writebacks +=
                Dynamic(slot, HotCoalesceCounter::SpillWritebacks);
        bucket.move_dynamic += MoveDynamic(slot);
        bucket.nan_dynamic +=
                Dynamic(slot, HotCoalesceCounter::NaNGuardInstructions);
        bucket.state_saved_dynamic += StateSavedDynamic(slot);
        bucket.indirect_l1_hits +=
                Dynamic(slot, HotCoalesceCounter::IndirectL1Hit);
        bucket.indirect_l1_misses +=
                Dynamic(slot, HotCoalesceCounter::IndirectL1Miss);
    }
    std::vector<AggregateBucket> buckets;
    buckets.reserve(by_pc.size());
    for (auto& [pc, bucket] : by_pc) {
        (void)pc;
        buckets.push_back(bucket);
    }
    return buckets;
}

template <typename Value>
std::vector<RankedBucket> Rank(const std::vector<AggregateBucket>& buckets,
                               Value value) {
    std::vector<RankedBucket> ranked;
    ranked.reserve(buckets.size());
    for (size_t i = 0; i < buckets.size(); ++i) {
        const auto rank_value = value(buckets[i]);
        if (rank_value) {
            ranked.push_back({i, rank_value});
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        if (left.value != right.value) return left.value > right.value;
        return left.bucket < right.bucket;
    });
    if (ranked.size() > 20) ranked.resize(20);
    return ranked;
}

void DumpAtExit() {
    const auto& config = GetSvmConfig();
    const char* destination = config.ra_hot_coalesce_is_set
            ? config.ra_hot_coalesce.c_str()
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
                         "[svm-hot-coalesce] error=cannot_open path=%s; using stderr\n",
                         destination);
        }
    }

    auto& process = Counters();
    const u32 count = std::min(process.next_slot.load(std::memory_order_relaxed),
                               kHotCoalesceMaxUnits);
    const auto buckets = BuildBuckets(count);
    u64 executed_units = 0;
    for (const auto& bucket : buckets) {
        if (bucket.entries) ++executed_units;
    }
    u64 entries = 0;
    u64 host_dynamic = 0;
    u64 spill_reloads = 0;
    u64 spill_writebacks = 0;
    u64 move_dynamic = 0;
    u64 nan_dynamic = 0;
    u64 state_saved_dynamic = 0;
    u64 state_sequences = 0;
    u64 state_pairs = 0;
    u64 state_same_offset = 0;
    for (u32 i = 0; i < count; ++i) {
        const auto& slot = process.slots[i];
        const auto slot_entries = Dynamic(slot, HotCoalesceCounter::Entries);
        entries += slot_entries;
        host_dynamic += HostDynamic(slot);
        spill_reloads += Dynamic(slot, HotCoalesceCounter::SpillReloads);
        spill_writebacks += Dynamic(slot, HotCoalesceCounter::SpillWritebacks);
        move_dynamic += MoveDynamic(slot);
        nan_dynamic += Dynamic(slot, HotCoalesceCounter::NaNGuardInstructions);
        state_saved_dynamic += StateSavedDynamic(slot);
        state_sequences += slot.shape.uniform.sequences;
        state_pairs += slot.shape.uniform.load_pairs + slot.shape.uniform.store_pairs;
        state_same_offset += slot.shape.uniform.same_offset;
    }
    const u64 spill_dynamic = spill_reloads + spill_writebacks;
    std::fprintf(out,
                 "[svm-hot-coalesce] units=%zu versions=%u executed_units=%llu overflow=%llu "
                 "entries=%llu host_dynamic=%llu spill_reloads=%llu "
                 "spill_writebacks=%llu spill_dynamic=%llu spill_pct=%.6f "
                 "move_dynamic=%llu move_pct=%.6f nan_guard_dynamic=%llu "
                 "nan_pct=%.6f state_sequences=%llu state_pairs=%llu "
                 "state_same_offset=%llu state_saved_dynamic=%llu state_pct=%.6f\n",
                 buckets.size(), count, PrintU64(executed_units),
                 PrintU64(Load(process.overflow)), PrintU64(entries),
                 PrintU64(host_dynamic), PrintU64(spill_reloads),
                 PrintU64(spill_writebacks), PrintU64(spill_dynamic),
                 Percent(spill_dynamic, host_dynamic), PrintU64(move_dynamic),
                 Percent(move_dynamic, host_dynamic), PrintU64(nan_dynamic),
                 Percent(nan_dynamic, host_dynamic), PrintU64(state_sequences),
                 PrintU64(state_pairs), PrintU64(state_same_offset),
                 PrintU64(state_saved_dynamic),
                 Percent(state_saved_dynamic, host_dynamic));

    if (config.indirect_l1_prof) {
        u64 hits = 0;
        u64 misses = 0;
        for (const auto& bucket : buckets) {
            hits += bucket.indirect_l1_hits;
            misses += bucket.indirect_l1_misses;
        }
        std::fprintf(out,
                     "[svm-indirect-l1] units=%zu hits=%llu misses=%llu "
                     "total=%llu hit_pct=%.6f\n",
                     buckets.size(), PrintU64(hits), PrintU64(misses),
                     PrintU64(hits + misses), Percent(hits, hits + misses));
        for (const auto& bucket : buckets) {
            const u64 total =
                    bucket.indirect_l1_hits + bucket.indirect_l1_misses;
            if (!total) continue;
            std::fprintf(out,
                         "[svm-indirect-l1-pc] pc=0x%llx versions=%u "
                         "hits=%llu misses=%llu total=%llu hit_pct=%.6f\n",
                         static_cast<unsigned long long>(bucket.guest_entry),
                         bucket.versions, PrintU64(bucket.indirect_l1_hits),
                         PrintU64(bucket.indirect_l1_misses), PrintU64(total),
                         Percent(bucket.indirect_l1_hits, total));
        }
    }

    if (config.ra_hot_coalesce_all) {
        for (const auto& bucket : buckets) {
            std::fprintf(out,
                         "[svm-hot-all] pc=0x%llx versions=%u entries=%llu "
                         "host_static=%u move_static=%u nan_static=%u "
                         "spill_static=%u state_saved_static=%u\n",
                         static_cast<unsigned long long>(bucket.guest_entry),
                         bucket.versions, PrintU64(bucket.entries),
                         bucket.host_static_max, bucket.move_static_max,
                         bucket.nan_static_max, bucket.spill_static_max,
                         bucket.state_saved_static_max);
        }
    }

    const auto hot = Rank(buckets, [](const AggregateBucket& bucket) {
        return bucket.host_dynamic;
    });
    for (size_t rank = 0; rank < hot.size(); ++rank) {
        const auto& bucket = buckets[hot[rank].bucket];
        std::fprintf(out,
                     "[svm-hot-coalesce-hot] rank=%zu pc=0x%llx versions=%u entries=%llu "
                     "host_static=%u host_dynamic=%llu move_static=%u "
                     "move_dynamic=%llu move_pct=%.6f nan_static=%u "
                     "nan_dynamic=%llu nan_pct=%.6f\n",
                     rank + 1,
                     static_cast<unsigned long long>(bucket.guest_entry),
                     bucket.versions, PrintU64(bucket.entries),
                     bucket.host_static_max, PrintU64(bucket.host_dynamic),
                     bucket.move_static_max, PrintU64(bucket.move_dynamic),
                     Percent(bucket.move_dynamic, bucket.host_dynamic),
                     bucket.nan_static_max, PrintU64(bucket.nan_dynamic),
                     Percent(bucket.nan_dynamic, bucket.host_dynamic));
    }

    const auto spill = Rank(buckets, [](const AggregateBucket& bucket) {
        return bucket.spill_reloads + bucket.spill_writebacks;
    });
    for (size_t rank = 0; rank < spill.size(); ++rank) {
        const auto& bucket = buckets[spill[rank].bucket];
        std::fprintf(out,
                     "[svm-hot-coalesce-spill] rank=%zu pc=0x%llx versions=%u "
                     "entries=%llu spill_static=%u spill_static_min=%u "
                     "reload_static=%u writeback_static=%u "
                     "reload_dynamic=%llu writeback_dynamic=%llu spill_dynamic=%llu "
                     "host_dynamic=%llu spill_pct=%.6f\n",
                     rank + 1,
                     static_cast<unsigned long long>(bucket.guest_entry),
                     bucket.versions, PrintU64(bucket.entries),
                     bucket.spill_static_max,
                     bucket.spill_static_min, bucket.reload_static_max,
                     bucket.writeback_static_max,
                     PrintU64(bucket.spill_reloads),
                     PrintU64(bucket.spill_writebacks),
                     PrintU64(bucket.spill_reloads + bucket.spill_writebacks),
                     PrintU64(bucket.host_dynamic),
                     Percent(bucket.spill_reloads + bucket.spill_writebacks,
                             bucket.host_dynamic));
    }

    const auto state = Rank(buckets, [](const AggregateBucket& bucket) {
        return bucket.state_saved_dynamic;
    });
    for (size_t rank = 0; rank < state.size(); ++rank) {
        const auto& bucket = buckets[state[rank].bucket];
        std::fprintf(out,
                     "[svm-hot-coalesce-state] rank=%zu pc=0x%llx versions=%u "
                     "entries=%llu "
                     "sequences=%u load_pairs=%u store_pairs=%u same_offset=%u "
                     "saved_static=%u saved_dynamic=%llu host_dynamic=%llu "
                     "state_pct=%.6f\n",
                     rank + 1,
                     static_cast<unsigned long long>(bucket.guest_entry),
                     bucket.versions, PrintU64(bucket.entries),
                     bucket.state_sequences_max,
                     bucket.state_load_pairs_max, bucket.state_store_pairs_max,
                     bucket.state_same_offset_max, bucket.state_saved_static_max,
                     PrintU64(bucket.state_saved_dynamic),
                     PrintU64(bucket.host_dynamic),
                     Percent(bucket.state_saved_dynamic, bucket.host_dynamic));
    }

    // Layout view keeps individual versions instead of aggregating by guest
    // PC. This makes ON/OFF code-size shifts, alignment and resolved link
    // distances directly comparable even when a block was retranslated.
    std::map<VAddr, const ProcessSlot*> target_hosts;
    for (u32 i = 0; i < count; ++i) {
        const auto& slot = process.slots[i];
        if (!slot.shape.host_address) continue;
        auto [it, inserted] = target_hosts.emplace(slot.shape.guest_entry, &slot);
        if (!inserted &&
            Dynamic(slot, HotCoalesceCounter::Entries) >
                    Dynamic(*it->second, HotCoalesceCounter::Entries)) {
            it->second = &slot;
        }
    }
    std::vector<std::pair<u32, u64>> layout_rank;
    for (u32 i = 0; i < count; ++i) {
        const auto& slot = process.slots[i];
        const u64 slot_entries = Dynamic(slot, HotCoalesceCounter::Entries);
        if (slot_entries && slot.shape.host_address) {
            layout_rank.emplace_back(
                    i, slot_entries * std::max<u32>(slot.shape.host_instructions, 1));
        }
    }
    std::sort(layout_rank.begin(), layout_rank.end(), [](const auto& left,
                                                         const auto& right) {
        if (left.second != right.second) return left.second > right.second;
        return left.first < right.first;
    });
    if (layout_rank.size() > 20) layout_rank.resize(20);

    u64 link_edges = 0;
    u64 resolved_edges = 0;
    u64 self_edges = 0;
    u64 forward_edges = 0;
    u64 backward_edges = 0;
    u64 same_line_edges = 0;
    u64 same_page_edges = 0;
    u64 potential_dynamic = 0;
    for (u32 i = 0; i < count; ++i) {
        const auto& slot = process.slots[i];
        const auto entries_for_slot = Dynamic(slot, HotCoalesceCounter::Entries);
        if (!entries_for_slot) continue;
        for (u32 edge = 0; edge < slot.shape.link_target_count; ++edge) {
            ++link_edges;
            potential_dynamic += entries_for_slot;
            const VAddr target_pc = slot.shape.link_targets[edge];
            if (target_pc == slot.shape.guest_entry) ++self_edges;
            auto target = target_hosts.find(target_pc);
            if (target == target_hosts.end()) continue;
            ++resolved_edges;
            const VAddr target_host = target->second->shape.host_address;
            if (target_host >= slot.shape.host_address) {
                ++forward_edges;
            } else {
                ++backward_edges;
            }
            same_line_edges +=
                    (target_host >> 6) == (slot.shape.host_address >> 6);
            same_page_edges +=
                    (target_host >> 12) == (slot.shape.host_address >> 12);
        }
    }
    std::fprintf(out,
                 "[svm-hot-layout-summary] static_edges=%llu resolved_edges=%llu "
                 "self_edges=%llu forward_edges=%llu backward_edges=%llu "
                 "same_line_edges=%llu same_page_edges=%llu "
                 "potential_dynamic_edges=%llu\n",
                 PrintU64(link_edges), PrintU64(resolved_edges),
                 PrintU64(self_edges), PrintU64(forward_edges),
                 PrintU64(backward_edges), PrintU64(same_line_edges),
                 PrintU64(same_page_edges), PrintU64(potential_dynamic));

    if (config.ra_hot_coalesce_all) {
        // 输出完整的 source/target unit 身份，避免把全局块表命中误算成
        // 同一编译单元内的边。这里仅扩展诊断日志，不参与发码或分配。
        for (u32 i = 0; i < count; ++i) {
            const auto& slot = process.slots[i];
            const auto& shape = slot.shape;
            const u64 slot_entries = Dynamic(slot, HotCoalesceCounter::Entries);
            if (!shape.host_address) continue;
            const VAddr source_unit = shape.host_address - shape.host_offset;
            std::fprintf(out,
                         "[svm-direct-node] slot=%u unit=0x%llx pc=0x%llx "
                         "entries=%llu targets=%u overflow=%u\n",
                         i, static_cast<unsigned long long>(source_unit),
                         static_cast<unsigned long long>(shape.guest_entry),
                         PrintU64(slot_entries), shape.link_target_count,
                         shape.link_target_overflow);
            for (u32 edge = 0; edge < shape.link_target_count; ++edge) {
                const VAddr target_pc = shape.link_targets[edge];
                auto target = target_hosts.find(target_pc);
                if (target == target_hosts.end()) {
                    std::fprintf(out,
                                 "[svm-direct-edge] source_unit=0x%llx "
                                 "source=0x%llx target=0x%llx entries=%llu "
                                 "target_unit=unresolved same_unit=0\n",
                                 static_cast<unsigned long long>(source_unit),
                                 static_cast<unsigned long long>(shape.guest_entry),
                                 static_cast<unsigned long long>(target_pc),
                                 PrintU64(slot_entries));
                    continue;
                }
                const auto& target_shape = target->second->shape;
                const VAddr target_unit =
                        target_shape.host_address - target_shape.host_offset;
                std::fprintf(out,
                             "[svm-direct-edge] source_unit=0x%llx "
                             "source=0x%llx target=0x%llx entries=%llu "
                             "target_unit=0x%llx same_unit=%u\n",
                             static_cast<unsigned long long>(source_unit),
                             static_cast<unsigned long long>(shape.guest_entry),
                             static_cast<unsigned long long>(target_pc),
                             PrintU64(slot_entries),
                             static_cast<unsigned long long>(target_unit),
                             source_unit == target_unit ? 1u : 0u);
            }
        }
    }

    for (size_t rank = 0; rank < layout_rank.size(); ++rank) {
        const u32 slot_index = layout_rank[rank].first;
        const auto& slot = process.slots[slot_index];
        const auto& shape = slot.shape;
        std::fprintf(out,
                     "[svm-hot-layout] rank=%zu slot=%u pc=0x%llx host=0x%llx "
                     "host_offset=%u host_bytes=%u host_mod64=%llu "
                     "host_mod128=%llu host_mod4096=%llu entries=%llu "
                     "host_static=%u nan_static=%u targets=%u overflow=%u",
                     rank + 1, slot_index,
                     static_cast<unsigned long long>(shape.guest_entry),
                     static_cast<unsigned long long>(shape.host_address),
                     shape.host_offset, shape.host_bytes,
                     PrintU64(shape.host_address & 63),
                     PrintU64(shape.host_address & 127),
                     PrintU64(shape.host_address & 4095),
                     PrintU64(Dynamic(slot, HotCoalesceCounter::Entries)),
                     shape.host_instructions, shape.nan_guard_instructions,
                     shape.link_target_count, shape.link_target_overflow);
        for (u32 edge = 0; edge < shape.link_target_count; ++edge) {
            const VAddr target_pc = shape.link_targets[edge];
            auto target = target_hosts.find(target_pc);
            if (target == target_hosts.end()) {
                std::fprintf(out, " target%u_pc=0x%llx target%u_host=unresolved",
                             edge,
                             static_cast<unsigned long long>(target_pc), edge);
                continue;
            }
            const VAddr target_host = target->second->shape.host_address;
            const auto delta = static_cast<long long>(target_host) -
                               static_cast<long long>(shape.host_address);
            std::fprintf(out,
                         " target%u_pc=0x%llx target%u_host=0x%llx "
                         "target%u_delta=%lld",
                         edge, static_cast<unsigned long long>(target_pc), edge,
                         static_cast<unsigned long long>(target_host), edge,
                         delta);
        }
        std::fputc('\n', out);
    }

    std::fflush(out);
    if (close_out) std::fclose(out);
}

struct UniformAccess {
    const ir::Inst* inst{};
    bool store{};
    u32 offset{};
    u32 size{};
};

bool PairableSize(u32 size) {
    return size == 4 || size == 8 || size == 16;
}

}  // namespace

namespace {

bool RegisterDumpIfEnabled(bool enabled) {
    if (!enabled) return false;
    static const bool registered = [] {
        // Construct process storage before registering the dump. atexit runs
        // callbacks in reverse registration order, so counters remain live.
        (void)Counters();
        std::atexit(DumpAtExit);
        return true;
    }();
    (void)registered;
    return true;
}

}  // namespace

bool HotCoalesceProfEnabled() {
    const auto& config = GetSvmConfig();
    const bool enabled = config.ra_hot_coalesce_is_set &&
                         config.ra_hot_coalesce != "0";
    return RegisterDumpIfEnabled(enabled);
}

bool IndirectL1ProfEnabled() {
    return RegisterDumpIfEnabled(GetSvmConfig().indirect_l1_prof);
}

bool HotCounterStorageEnabled() {
    return HotCoalesceProfEnabled() || IndirectL1ProfEnabled();
}

u32 HotCoalesceRegisterUnit(VAddr guest_entry) {
    if (!HotCounterStorageEnabled()) return kHotCoalesceInvalidSlot;
    auto& process = Counters();
    const u32 slot = process.next_slot.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kHotCoalesceMaxUnits) {
        process.overflow.fetch_add(1, std::memory_order_relaxed);
        return kHotCoalesceInvalidSlot;
    }
    process.slots[slot].shape.guest_entry = guest_entry;
    return slot;
}

void HotCoalesceUpdateUnit(u32 slot, const HotCoalesceUnitStatic& counters) {
    if (slot == kHotCoalesceInvalidSlot || slot >= kHotCoalesceMaxUnits) return;
    Counters().slots[slot].shape = counters;
}

void HotCoalesceSetUnitHostBase(u32 slot, VAddr host_base) {
    if (slot == kHotCoalesceInvalidSlot || slot >= kHotCoalesceMaxUnits) return;
    auto& shape = Counters().slots[slot].shape;
    shape.host_address = host_base + shape.host_offset;
}

void HotCoalesceSubmitThread(std::span<const u64> counters) {
    if (!HotCounterStorageEnabled() || counters.empty()) return;
    auto& process = Counters();
    const u32 count = std::min(process.next_slot.load(std::memory_order_relaxed),
                               kHotCoalesceMaxUnits);
    const size_t wanted = static_cast<size_t>(count) * kHotCoalesceCounterCount;
    ASSERT(counters.size() >= wanted);
    for (u32 slot = 0; slot < count; ++slot) {
        for (u32 kind = 0; kind < kHotCoalesceCounterCount; ++kind) {
            const auto value = counters[static_cast<size_t>(slot) *
                                                kHotCoalesceCounterCount + kind];
            if (value) {
                process.slots[slot].dynamic[kind].fetch_add(
                        value, std::memory_order_relaxed);
            }
        }
    }
}

HotCoalesceUniformStats HotCoalesceAnalyzeUniformSequences(const ir::Block* block) {
    HotCoalesceUniformStats out{};
    if (!block) return out;

    UniformAccess previous{};
    bool have_previous = false;
    bool run_store = false;
    u32 run_size = 0;
    u32 run_next_offset = 0;
    u32 run_length = 0;
    auto flush_run = [&] {
        if (run_length >= 2) {
            ++out.sequences;
            const u32 pairs = run_length / 2;
            if (run_store) out.store_pairs += pairs;
            else out.load_pairs += pairs;
            out.saved_instructions += pairs;
        }
        run_length = 0;
    };

    for (const auto& inst : block->GetInstList()) {
        const auto op = inst.GetOp();
        if (op != ir::OpCode::LoadUniform && op != ir::OpCode::StoreUniform) {
            flush_run();
            have_previous = false;
            continue;
        }
        const auto uniform = inst.GetArg<ir::Uniform>(0);
        UniformAccess current{&inst,
                              op == ir::OpCode::StoreUniform,
                              uniform.GetOffset(),
                              ir::GetValueSizeByte(uniform.GetType())};

        if (have_previous && previous.offset == current.offset &&
            previous.size == current.size) {
            bool safe = true;
            if (!previous.store && current.store) {
                safe = current.inst->GetArg<ir::Value>(1).Def() == previous.inst;
            }
            if (safe) {
                ++out.same_offset;
                ++out.saved_instructions;
            }
        }

        const bool extend = run_length && current.store == run_store &&
                            current.size == run_size && PairableSize(current.size) &&
                            current.offset == run_next_offset;
        if (extend) {
            ++run_length;
            run_next_offset += current.size;
        } else {
            flush_run();
            run_store = current.store;
            run_size = current.size;
            run_next_offset = current.offset + current.size;
            run_length = PairableSize(current.size) ? 1 : 0;
        }
        previous = current;
        have_previous = true;
    }
    flush_run();
    return out;
}

void HotCoalesceAnalyzeLinkTargets(const ir::Block* block,
                                   HotCoalesceUnitStatic& out) {
    if (!block) return;
    auto add = [&](VAddr target) {
        if (out.link_target_count < out.link_targets.size()) {
            out.link_targets[out.link_target_count++] = target;
        } else if (out.link_target_overflow != UINT8_MAX) {
            ++out.link_target_overflow;
        }
    };
    std::function<void(const ir::Terminal&)> visit;
    visit = [&](const ir::Terminal& terminal) {
        VisitVariant<void>(terminal, [&](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                          std::is_same_v<T, ir::terminal::LinkBlockFast>) {
                add(item.next.Value());
            } else if constexpr (std::is_same_v<T, ir::terminal::If> ||
                                 std::is_same_v<T, ir::terminal::Condition>) {
                visit(item.then_);
                visit(item.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
                visit(item.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                for (const auto& item_case : item.cases) visit(item_case.then);
            }
        });
    };
    visit(block->GetTerminal());
}

bool HotCoalesceIsMoveBridge(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    const auto end = text.find_first_of(" \t");
    const auto mnemonic = text.substr(0, end);
    constexpr std::array<std::string_view, 17> kMoveMnemonics{{
            "mov", "fmov", "umov", "smov", "ins", "dup", "uxtb", "uxth",
            "uxtw", "sxtb", "sxth", "sxtw", "ubfx", "sbfx", "bfi", "bfxil",
            "extr",
    }};
    if (std::find(kMoveMnemonics.begin(), kMoveMnemonics.end(), mnemonic) !=
        kMoveMnemonics.end()) {
        return true;
    }
    if ((mnemonic == "lsl" || mnemonic == "lsr" || mnemonic == "asr") &&
        text.find("#0") != std::string_view::npos) {
        return true;
    }
    return false;
}

}  // namespace swift::runtime
