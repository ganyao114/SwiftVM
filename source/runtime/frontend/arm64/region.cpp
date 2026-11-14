#include "region.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <iterator>
#include <vector>

namespace swift::arm64 {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

const BlockWindow* FindWindow(std::span<const BlockWindow> windows, u64 pc) noexcept {
    const auto i = std::upper_bound(
            windows.begin(), windows.end(), pc, [](u64 p, const BlockWindow& w) {
                return p < w.pc;
            });
    if (i == windows.begin()) return nullptr;
    const auto& w = *std::prev(i);
    return pc - w.pc < w.bytes.size() ? &w : nullptr;
}

struct SliceEntry {
    u64 pc{}, end{};
    u32 first{UINT32_MAX}, count{};
};

class SliceDirectory {
public:
    std::size_t size() const noexcept { return using_heap ? heap.size() : local_size; }

    SliceEntry* begin() noexcept { return using_heap ? heap.data() : local.data(); }
    SliceEntry* end() noexcept { return begin() + size(); }
    const SliceEntry* begin() const noexcept { return using_heap ? heap.data() : local.data(); }
    const SliceEntry* end() const noexcept { return begin() + size(); }

    SliceEntry* lower_bound(u64 pc) noexcept {
        return std::lower_bound(begin(), end(), pc, [](const SliceEntry& e, u64 addr) {
            return e.pc < addr;
        });
    }

    void insert_at(std::size_t index, SliceEntry entry) {
        if (!using_heap && local_size < local.size()) {
            for (std::size_t i = local_size; i > index; --i) local[i] = local[i - 1];
            local[index] = entry;
            ++local_size;
            return;
        }
        if (!using_heap) {
            heap.reserve(local.size() * 2);
            heap.assign(local.begin(), local.begin() + local_size);
            using_heap = true;
        }
        heap.insert(heap.begin() + static_cast<std::ptrdiff_t>(index), entry);
    }

private:
    std::array<SliceEntry, 64> local;
    std::size_t local_size{};
    std::vector<SliceEntry> heap;
    bool using_heap{};
};

// Sorted descending so popping the smallest pending PC is O(1) at the tail.
class PendingSet {
public:
    bool empty() const noexcept { return (using_heap ? heap.size() : local_size) == 0; }

    void insert(u64 pc) {
        auto* b = using_heap ? heap.data() : local.data();
        const std::size_t n = using_heap ? heap.size() : local_size;
        auto* it = std::lower_bound(b, b + n, pc, std::greater<u64>{});
        if (it != b + n && *it == pc) return;
        const auto idx = static_cast<std::size_t>(it - b);
        if (!using_heap && local_size < local.size()) {
            for (std::size_t i = local_size; i > idx; --i) local[i] = local[i - 1];
            local[idx] = pc;
            ++local_size;
            return;
        }
        if (!using_heap) {
            heap.reserve(local.size() * 2);
            heap.assign(local.begin(), local.begin() + local_size);
            using_heap = true;
        }
        heap.insert(heap.begin() + static_cast<std::ptrdiff_t>(idx), pc);
    }

    u64 pop_min() noexcept {
        if (using_heap) {
            const u64 v = heap.back();
            heap.pop_back();
            return v;
        }
        return local[--local_size];
    }

private:
    std::array<u64, 64> local;
    std::size_t local_size{};
    std::vector<u64> heap;
    bool using_heap{};
};
}  // namespace

DecodedFunction DecodeFunction(std::span<const BlockWindow> windows,
                               std::uint64_t entry,
                               DecodeLimits limits,
                               DecodeTiming* timing,
                               ReadyEntryLookup ready,
                               std::vector<std::uint64_t>* continuations) {
    DecodedFunction out;
    const auto fail = [&](DecodeError e, u64 pc) {
        out.error = e;
        out.error_pc = pc;
        out.blocks.clear();
    };
    if (windows.empty() || !limits.max_blocks || !limits.max_instructions || (entry & 3)) {
        fail(DecodeError::InvalidInput, entry);
        return out;
    }
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto& w = windows[i];
        if (w.bytes.empty() || w.pc > UINT64_MAX - w.bytes.size() ||
            (i && windows[i - 1].pc + windows[i - 1].bytes.size() > w.pc)) {
            fail(DecodeError::InvalidInput, w.pc);
            return out;
        }
    }

    SliceDirectory slices;
    PendingSet pending;
    const auto queue = [&](u64 pc) {
        if ((pc & 3) || ((pc != entry) && ready.Contains(pc))) return;
        const auto* w = FindWindow(windows, pc);
        if (!w || w->bytes.size() - (pc - w->pc) < 4) return;
        auto* next = slices.lower_bound(pc);
        const auto next_idx = static_cast<std::size_t>(next - slices.begin());
        if (next != slices.end() && next->pc == pc) return;
        if (next_idx != 0) {
            auto& owner = slices.begin()[next_idx - 1];
            if (owner.first != UINT32_MAX && pc < owner.end) {
                if (slices.size() >= limits.max_blocks) {
                    ++out.budget_exits;
                    return;
                }
                const auto count = u32((pc - owner.pc) / 4);
                const SliceEntry tail{pc, owner.end, owner.first + count, owner.count - count};
                owner.count = count;
                owner.end = pc;
                slices.insert_at(next_idx, tail);
                ++out.splits;
                return;
            }
        }
        if (slices.size() >= limits.max_blocks) {
            ++out.budget_exits;
            return;
        }
        slices.insert_at(next_idx, SliceEntry{pc});
        pending.insert(pc);
    };

    queue(entry);
    if (slices.size() == 0) {
        fail(DecodeError::InvalidInput, entry);
        return out;
    }
    out.instructions.reserve(std::min<std::size_t>(limits.max_instructions, 64));

    while (!pending.empty()) {
        const auto begin = pending.pop_min();
        auto* current = slices.lower_bound(begin);
        if (current == slices.end() || current->pc != begin || current->first != UINT32_MAX)
            continue;
        const auto current_idx = static_cast<std::size_t>(current - slices.begin());
        current->first = static_cast<u32>(out.instructions.size());
        current->end = begin;
        const bool has_next = current_idx + 1 < slices.size();
        const u64 next_start = has_next ? slices.begin()[current_idx + 1].pc : 0;
        u64 pc = begin;
        const BlockWindow* win = FindWindow(windows, pc);
        while (true) {
            if (out.instructions.size() >= limits.max_instructions) {
                ++out.budget_exits;
                break;
            }
            if (pc != begin && ((has_next && pc == next_start) || ready.Contains(pc))) break;
            if (!win || pc - win->pc >= win->bytes.size()) {
                if (win && win + 1 != windows.data() + windows.size() && (win + 1)->pc == pc)
                    ++win;
                else
                    break;
            }
            const auto bytes = win->bytes.subspan(static_cast<std::size_t>(pc - win->pc));
            const auto decoded = DecodeOne(bytes, pc, timing);
            ++out.decode_calls;
            if (!decoded) {
                fail(decoded.error, pc);
                return out;
            }
            const auto inst = decoded.instruction;
            out.instructions.push_back(inst);
            auto& active = slices.begin()[current_idx];
            ++active.count;
            out.decoded_bytes += 4;
            active.end = pc + 4;
            if (inst.Boundary()) {
                u64 target{};
                const auto flow = inst.Flow();
                if (flow == FlowKind::Conditional) {
                    if (inst.RelativeTarget(target)) queue(target);
                    queue(pc + 4);
                } else if (flow == FlowKind::Jump && inst.RelativeTarget(target))
                    queue(target);
                if (continuations &&
                    (flow == FlowKind::Call || flow == FlowKind::Service ||
                     flow == FlowKind::Linear))
                    continuations->push_back(pc + 4);
                break;
            }
            pc += 4;
        }
    }
    out.blocks.reserve(slices.size());
    for (const auto& slice : slices) {
        const u32 first =
                slice.first == UINT32_MAX ? static_cast<u32>(out.instructions.size()) : slice.first;
        if (slice.pc == entry) out.entry_block = static_cast<u32>(out.blocks.size());
        out.blocks.push_back({slice.pc, first, slice.count});
    }
    return out;
}
}  // namespace swift::arm64
