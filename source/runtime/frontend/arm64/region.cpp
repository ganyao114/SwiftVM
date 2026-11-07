#include <algorithm>
#include <map>
#include <set>
#include "region.h"

namespace swift::arm64 {
DecodedFunction DecodeFunction(std::span<const BlockWindow> windows,
                               std::uint64_t entry,
                               DecodeLimits limits,
                               DecodeTiming* timing,
                               ReadyEntryLookup ready,
                               std::vector<std::uint64_t>* continuations) {
    using u64 = std::uint64_t;
    using u32 = std::uint32_t;
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
    const auto source = [&](u64 pc) -> std::span<const std::uint8_t> {
        const auto i = std::upper_bound(
                windows.begin(), windows.end(), pc, [](u64 p, const auto& w) { return p < w.pc; });
        if (i == windows.begin()) return {};
        const auto& w = *std::prev(i);
        return pc - w.pc < w.bytes.size() ? w.bytes.subspan(pc - w.pc)
                                          : std::span<const std::uint8_t>{};
    };

    struct Slice {
        u32 first{UINT32_MAX}, count{};
        u64 end{};
    };

    std::map<u64, Slice> slices;
    std::set<u64> pending;
    const auto queue = [&](u64 pc) {
        if ((pc & 3) || source(pc).size() < 4 || ((pc != entry) && ready.Contains(pc))) return;
        if (auto i = slices.upper_bound(pc); i != slices.begin()) {
            auto owner = std::prev(i);
            if (owner->first == pc) return;
            if (owner->second.first != UINT32_MAX && pc < owner->second.end) {
                if (slices.size() >= limits.max_blocks) {
                    ++out.budget_exits;
                    return;
                }
                const auto count = u32((pc - owner->first) / 4);
                auto tail = owner->second;
                tail.first += count;
                tail.count -= count;
                owner->second.count = count;
                owner->second.end = pc;
                slices.emplace(pc, tail);
                ++out.splits;
                return;
            }
        }
        if (slices.size() >= limits.max_blocks) {
            ++out.budget_exits;
            return;
        }
        slices.emplace(pc, Slice{});
        pending.insert(pc);
    };
    queue(entry);
    while (!pending.empty()) {
        const auto begin = *pending.begin();
        pending.erase(pending.begin());
        auto& slice = slices.at(begin);
        if (slice.first != UINT32_MAX) continue;
        slice.first = out.instructions.size();
        slice.end = begin;
        u64 pc = begin;
        while (true) {
            if (out.instructions.size() >= limits.max_instructions) {
                ++out.budget_exits;
                break;
            }
            if (pc != begin && (slices.contains(pc) || ready.Contains(pc))) break;
            auto bytes = source(pc);
            if (bytes.empty()) break;
            const auto decoded = DecodeOne(bytes, pc, timing);
            ++out.decode_calls;
            if (!decoded) {
                fail(decoded.error, pc);
                return out;
            }
            const auto& inst = decoded.instruction;
            out.instructions.push_back(inst);
            ++slice.count;
            out.decoded_bytes += 4;
            slice.end = pc + 4;
            if (inst.Boundary()) {
                u64 target{};
                if (inst.Flow() == FlowKind::Conditional) {
                    if (inst.RelativeTarget(target)) queue(target);
                    queue(pc + 4);
                } else if (inst.Flow() == FlowKind::Jump && inst.RelativeTarget(target))
                    queue(target);
                if (continuations &&
                    (inst.Flow() == FlowKind::Call || inst.Flow() == FlowKind::Service ||
                     inst.Flow() == FlowKind::Linear))
                    continuations->push_back(pc + 4);
                break;
            }
            pc += 4;
        }
    }
    for (auto& [pc, slice] : slices) {
        if (slice.first == UINT32_MAX) slice.first = out.instructions.size();
        if (pc == entry) out.entry_block = out.blocks.size();
        out.blocks.push_back({pc, slice.first, slice.count});
    }
    if (out.blocks.empty()) fail(DecodeError::InvalidInput, entry);
    return out;
}
}  // namespace swift::arm64
