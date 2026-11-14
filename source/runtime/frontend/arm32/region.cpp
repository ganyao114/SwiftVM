#include <algorithm>
#include <array>
#include <vector>
#include "region.h"

namespace swift::arm32 {
namespace {
struct SliceEntry {
    u64 pc{};
    u32 first{UINT32_MAX};
    u32 count{};
};

class SliceDirectory {
public:
    std::size_t Size() const noexcept { return heap.empty() ? count : heap.size(); }

    SliceEntry* Data() noexcept { return heap.empty() ? inline_buf.data() : heap.data(); }

    const SliceEntry* Data() const noexcept {
        return heap.empty() ? inline_buf.data() : heap.data();
    }

    std::size_t LowerBound(u64 pc) const noexcept {
        const auto* begin = Data();
        const auto* end = begin + Size();
        return static_cast<std::size_t>(
                std::lower_bound(
                        begin, end, pc, [](const SliceEntry& e, u64 key) { return e.pc < key; }) -
                begin);
    }

    bool ContainsAt(std::size_t idx, u64 pc) const noexcept {
        return idx < Size() && Data()[idx].pc == pc;
    }

    bool Contains(u64 pc) const noexcept { return ContainsAt(LowerBound(pc), pc); }

    void InsertAt(std::size_t idx, SliceEntry entry) {
        if (heap.empty()) {
            if (count < inline_buf.size()) {
                std::move_backward(inline_buf.begin() + idx,
                                   inline_buf.begin() + count,
                                   inline_buf.begin() + count + 1);
                inline_buf[idx] = entry;
                ++count;
                return;
            }
            heap.reserve(inline_buf.size() * 2);
            heap.assign(inline_buf.begin(), inline_buf.begin() + count);
        }
        heap.insert(heap.begin() + static_cast<std::ptrdiff_t>(idx), entry);
    }

private:
    std::array<SliceEntry, 64> inline_buf{};
    std::size_t count{};
    std::vector<SliceEntry> heap;
};

class PendingQueue {
public:
    bool Empty() const noexcept { return heap.empty() ? count == 0 : heap.empty(); }

    void Push(u64 pc) {
        auto* begin = heap.empty() ? inline_buf.data() : heap.data();
        const std::size_t n = heap.empty() ? count : heap.size();
        const auto pos = static_cast<std::size_t>(
                std::lower_bound(begin, begin + n, pc, [](u64 a, u64 b) { return a > b; }) - begin);
        if (pos < n && begin[pos] == pc) return;
        if (heap.empty()) {
            if (count < inline_buf.size()) {
                std::move_backward(inline_buf.begin() + pos,
                                   inline_buf.begin() + count,
                                   inline_buf.begin() + count + 1);
                inline_buf[pos] = pc;
                ++count;
                return;
            }
            heap.reserve(inline_buf.size() * 2);
            heap.assign(inline_buf.begin(), inline_buf.begin() + count);
        }
        heap.insert(heap.begin() + static_cast<std::ptrdiff_t>(pos), pc);
    }

    u64 PopMin() noexcept {
        if (heap.empty()) return inline_buf[--count];
        const u64 pc = heap.back();
        heap.pop_back();
        return pc;
    }

private:
    std::array<u64, 64> inline_buf{};
    std::size_t count{};
    std::vector<u64> heap;
};

bool TrySplitSlice(SliceDirectory& slices,
                   std::size_t insert_idx,
                   u64 pc,
                   bool any_it_slice,
                   const std::vector<Instruction>& instructions) {
    const auto target = Location{pc};
    auto* data = slices.Data();
    if (!any_it_slice) {
        for (std::size_t k = insert_idx; k > 0 && (data[k - 1].pc >> 32) == (pc >> 32); --k) {
            auto& s = data[k - 1];
            if (!s.count || s.first == UINT32_MAX) continue;
            if (pc > instructions[s.first + s.count - 1].PC()) continue;
            u32 hit = UINT32_MAX;
            if (!target.Thumb()) {
                const u32 idx = static_cast<u32>((pc - s.pc) >> 2);
                if (idx > 0 && idx < s.count && instructions[s.first + idx].PC() == pc) hit = idx;
            } else {
                const auto begin = instructions.begin() + s.first + 1;
                const auto end = instructions.begin() + s.first + s.count;
                const auto it = std::lower_bound(
                        begin, end, pc, [](const Instruction& inst, u64 key) {
                            return inst.PC() < key;
                        });
                if (it != end && it->PC() == pc)
                    hit = static_cast<u32>(it - (instructions.begin() + s.first));
            }
            if (hit == UINT32_MAX) continue;
            const SliceEntry tail{pc, s.first + hit, s.count - hit};
            s.count = hit;
            slices.InsertAt(insert_idx, tail);
            return true;
        }
        return false;
    }
    const std::size_t total = slices.Size();
    for (std::size_t idx = 0; idx < total; ++idx) {
        auto& s = data[idx];
        if (!s.count || s.first == UINT32_MAX) continue;
        const auto& first = instructions[s.first];
        const auto& last = instructions[s.first + s.count - 1];
        if (target.Address() < first.Address() || target.Address() > last.Address()) continue;
        for (u32 i = 1; i < s.count; ++i) {
            if (instructions[s.first + i].PC() != pc) continue;
            const SliceEntry tail{pc, s.first + i, s.count - i};
            s.count = i;
            slices.InsertAt(insert_idx, tail);
            return true;
        }
    }
    return false;
}

bool ValidateDecodeInputs(std::span<const BlockWindow> windows, u64 entry, DecodeLimits limits) {
    const auto location = Location{entry};
    if (windows.empty() || !limits.max_blocks || !limits.max_instructions ||
        (location.Address() & (location.Thumb() ? 1u : 3u))) {
        return false;
    }
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto& window = windows[i];
        if (window.bytes.empty() || window.pc > UINT32_MAX ||
            window.bytes.size() > (UINT64_C(1) << 32) - window.pc ||
            (i && windows[i - 1].pc + windows[i - 1].bytes.size() > window.pc)) {
            return false;
        }
    }
    return true;
}
}  // namespace

std::span<const u8> SourceBytes(std::span<const BlockWindow> windows, u32 address) noexcept {
    const auto it = std::upper_bound(
            windows.begin(), windows.end(), address, [](u32 pc, const auto& window) {
                return pc < window.pc;
            });
    if (it == windows.begin()) return {};
    const auto& window = *std::prev(it);
    return address - window.pc < window.bytes.size() ? window.bytes.subspan(address - window.pc)
                                                     : std::span<const u8>{};
}

std::span<const u8> InstructionBytes(std::span<const BlockWindow> windows,
                                     u32 address,
                                     std::array<u8, 4>& edge) noexcept {
    const auto first = SourceBytes(windows, address);
    if (first.size() >= 4 || first.empty()) return first;
    auto count = first.size();
    std::copy(first.begin(), first.end(), edge.begin());
    // A Thumb32 fetch can cross adjacent source windows (including pages).
    if (u64(address) + count < (UINT64_C(1) << 32)) {
        const auto second = SourceBytes(windows, address + count);
        const auto extra = std::min(second.size(), edge.size() - count);
        std::copy_n(second.begin(), extra, edge.begin() + count);
        count += extra;
    }
    return {edge.data(), count};
}

DecodedFunction DecodeFunction(std::span<const BlockWindow> windows,
                               u64 entry,
                               DecodeLimits limits,
                               DecodeTiming* timing,
                               ReadyEntryLookup ready,
                               std::vector<u64>* continuations) {
    DecodedFunction out;
    const auto fail = [&](DecodeError error, u64 pc) {
        out.error = error;
        out.error_pc = pc;
        out.blocks.clear();
    };
    if (!ValidateDecodeInputs(windows, entry, limits)) {
        fail(DecodeError::InvalidInput, entry);
        return out;
    }

    SliceDirectory slices;
    PendingQueue pending;
    bool any_it_slice = false;
    const auto queue = [&](u64 pc) {
        const auto target = Location{pc};
        if ((target.Address() & (target.Thumb() ? 1u : 3u)) ||
            SourceBytes(windows, target.Address()).empty() || (pc != entry && ready.Contains(pc)))
            return;
        const auto idx = slices.LowerBound(pc);
        if (slices.ContainsAt(idx, pc)) return;
        if (slices.Size() >= limits.max_blocks) {
            ++out.budget_exits;
            return;
        }
        any_it_slice |= target.IT().IsInITBlock();
        if (TrySplitSlice(slices, idx, pc, any_it_slice, out.instructions)) {
            ++out.splits;
            return;
        }
        slices.InsertAt(idx, SliceEntry{pc, UINT32_MAX, 0});
        pending.Push(pc);
    };
    queue(entry);
    while (!pending.Empty()) {
        const auto begin = pending.PopMin();
        const auto slice_idx = slices.LowerBound(begin);
        if (slices.Data()[slice_idx].first != UINT32_MAX) continue;
        slices.Data()[slice_idx].first = static_cast<u32>(out.instructions.size());
        const bool in_it = Location{begin}.IT().IsInITBlock();
        std::size_t next_idx = slice_idx + 1;
        auto pc = begin;
        while (true) {
            if (out.instructions.size() >= limits.max_instructions) {
                ++out.budget_exits;
                break;
            }
            if (pc != begin) {
                bool hit_slice = false;
                if (!in_it && pc > begin) {
                    const auto* sdata = slices.Data();
                    const auto total = slices.Size();
                    while (next_idx < total && sdata[next_idx].pc < pc) ++next_idx;
                    hit_slice = next_idx < total && sdata[next_idx].pc == pc;
                } else {
                    hit_slice = slices.Contains(pc);
                }
                if (hit_slice || ready.Contains(pc)) break;
            }
            std::array<u8, 4> edge;
            const auto bytes = InstructionBytes(windows, Location{pc}.Address(), edge);
            if (bytes.empty()) break;
            const auto decoded = DecodeOne(bytes, pc, timing);
            ++out.decode_calls;
            if (!decoded) {
                if (slices.Data()[slice_idx].count) break;
                fail(decoded.error, pc);
                return out;
            }
            const auto instruction = decoded.instruction;
            out.instructions.push_back(instruction);
            ++slices.Data()[slice_idx].count;
            out.decoded_bytes += instruction.Length();
            if (instruction.Boundary()) {
                u64 target{};
                const auto flow = instruction.Flow();
                if (flow == FlowKind::Conditional) {
                    if (instruction.RelativeTarget(target)) queue(target);
                    queue(instruction.NextPC());
                } else if (flow == FlowKind::Jump && instruction.RelativeTarget(target))
                    queue(target);
                else if (instruction.Code() == Opcode::thumb16_IT)
                    queue(instruction.NextPC());
                if (continuations && (flow == FlowKind::Call || flow == FlowKind::Service ||
                                      flow == FlowKind::Linear))
                    continuations->push_back(instruction.NextPC());
                break;
            }
            pc = instruction.NextPC();
        }
    }
    const auto* sdata = slices.Data();
    const auto total = slices.Size();
    out.blocks.reserve(total);
    for (std::size_t i = 0; i < total; ++i) {
        const auto first = sdata[i].first == UINT32_MAX ? static_cast<u32>(out.instructions.size())
                                                        : sdata[i].first;
        if (sdata[i].pc == entry) out.entry_block = static_cast<u32>(out.blocks.size());
        out.blocks.push_back({sdata[i].pc, first, sdata[i].count});
    }
    if (out.blocks.empty()) fail(DecodeError::InvalidInput, entry);
    return out;
}
}  // namespace swift::arm32
