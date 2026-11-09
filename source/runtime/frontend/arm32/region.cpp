#include <algorithm>
#include <map>
#include <set>
#include "region.h"

namespace swift::arm32 {
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
    const auto location = Location{entry};
    if (windows.empty() || !limits.max_blocks || !limits.max_instructions ||
        (location.Address() & (location.Thumb() ? 1u : 3u))) {
        fail(DecodeError::InvalidInput, entry);
        return out;
    }
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto& window = windows[i];
        if (window.bytes.empty() || window.pc > UINT32_MAX ||
            window.bytes.size() > (UINT64_C(1) << 32) - window.pc ||
            (i && windows[i - 1].pc + windows[i - 1].bytes.size() > window.pc)) {
            fail(DecodeError::InvalidInput, entry);
            return out;
        }
    }

    struct Slice {
        u32 first{UINT32_MAX}, count{};
    };

    std::map<u64, Slice> slices;
    std::set<u64> pending;
    const auto queue = [&](u64 pc) {
        const auto target = Location{pc};
        if ((target.Address() & (target.Thumb() ? 1u : 3u)) ||
            SourceBytes(windows, target.Address()).empty() || (pc != entry && ready.Contains(pc)) ||
            slices.contains(pc))
            return;
        if (slices.size() >= limits.max_blocks) {
            ++out.budget_exits;
            return;
        }
        // Variable width and IT progression prohibit computing an index from
        // a byte delta. Split only at an exact, already decoded Location.
        for (auto& [begin, slice] : slices) {
            if (!slice.count || slice.first == UINT32_MAX) continue;
            const auto& first = out.instructions[slice.first];
            const auto& last = out.instructions[slice.first + slice.count - 1];
            if (target.Address() < first.Address() || target.Address() > last.Address()) continue;
            for (u32 i = 1; i < slice.count; ++i) {
                if (out.instructions[slice.first + i].PC() != pc) continue;
                const auto tail = Slice{slice.first + i, slice.count - i};
                slice.count = i;
                slices.emplace(pc, tail);
                ++out.splits;
                return;
            }
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
        auto pc = begin;
        while (true) {
            if (out.instructions.size() >= limits.max_instructions) {
                ++out.budget_exits;
                break;
            }
            if (pc != begin && (slices.contains(pc) || ready.Contains(pc))) break;
            std::array<u8, 4> edge;
            const auto bytes = InstructionBytes(windows, Location{pc}.Address(), edge);
            if (bytes.empty()) break;
            const auto decoded = DecodeOne(bytes, pc, timing);
            ++out.decode_calls;
            if (!decoded) {
                if (slice.count) break;
                fail(decoded.error, pc);
                return out;
            }
            const auto instruction = decoded.instruction;
            out.instructions.push_back(instruction);
            ++slice.count;
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
    for (auto& [pc, slice] : slices) {
        if (slice.first == UINT32_MAX) slice.first = out.instructions.size();
        if (pc == entry) out.entry_block = out.blocks.size();
        out.blocks.push_back({pc, slice.first, slice.count});
    }
    if (out.blocks.empty()) fail(DecodeError::InvalidInput, entry);
    return out;
}
}  // namespace swift::arm32
