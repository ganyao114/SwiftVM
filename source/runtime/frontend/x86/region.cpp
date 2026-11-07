#include "region.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <map>
#include <memory_resource>

#include "instruction.h"

namespace swift::x86 {
namespace {
using u32 = std::uint32_t;
using u64 = std::uint64_t;

bool Valid(std::span<const BlockWindow> windows, DecodeLimits limits) {
    if (windows.empty() || !limits.max_instructions || !limits.max_blocks) return false;
    for (std::size_t i = 0; i < windows.size(); ++i) {
        const auto& window = windows[i];
        if (window.bytes.empty() || !window.bytes.data() ||
            window.bytes.size() > std::numeric_limits<u64>::max() - window.pc ||
            (i && windows[i - 1].pc + windows[i - 1].bytes.size() > window.pc))
            return false;
    }
    return true;
}

auto ReadableAt(std::span<const BlockWindow> windows, u64 pc) {
    auto next = std::upper_bound(
            windows.begin(), windows.end(), pc, [](u64 address, const BlockWindow& window) {
                return address < window.pc;
            });
    if (next == windows.begin()) return windows.end();
    const auto found = std::prev(next);
    return pc - found->pc < found->bytes.size() ? found : windows.end();
}

// This is the actual sparse block directory while discovery is open. Only its
// final slices are moved to the returned contiguous block list; no per-record
// address/index table or copy of decoded operands is produced.
struct Slice {
    u64 end{};
    u32 first{}, count{};
    bool pending{true};
};
}  // namespace

DecodedFunction DecodeFunction(std::span<const BlockWindow> readable,
                               u64 entry_pc,
                               DecodeLimits limits,
                               DecodeTiming* timing,
                               ReadyEntryLookup ready,
                               runtime::x86::GuestCpuModel model) {
    DecodedFunction out;
    if (!Valid(readable, limits) || ReadableAt(readable, entry_pc) == readable.end()) {
        out.error = DecodeError::InvalidInput;
        out.error_pc = entry_pc;
        return out;
    }
    // Only formation's original directory and worklist use this local arena.
    // The returned instruction/block vectors retain their own ordinary storage.
    alignas(std::max_align_t) std::array<std::byte, 2048> formation_storage;
    std::pmr::monotonic_buffer_resource formation_memory{
            formation_storage.data(), formation_storage.size(), std::pmr::new_delete_resource()};
    std::pmr::map<u64, Slice> directory{&formation_memory};
    std::pmr::vector<u64> pending{&formation_memory};
    out.instructions.reserve(std::min<u32>(limits.max_instructions, 32));
    pending.reserve(std::min<u32>(limits.max_blocks, 8));
    directory.emplace(entry_pc, Slice{entry_pc});
    pending.push_back(entry_pc);
    const auto fail = [&](DecodeError error, u64 pc) {
        out.error = error;
        out.error_pc = pc;
    };
    const auto queue = [&](u64 target) {
        if (ReadableAt(readable, target) == readable.end()) return true;
        if (target != entry_pc && ready.Contains(target)) return true;
        auto next = directory.lower_bound(target);
        if (next != directory.end() && next->first == target) return true;
        if (next != directory.begin()) {
            auto previous = std::prev(next);
            auto& slice = previous->second;
            if (target < slice.end) {
                const auto first = out.instructions.begin() + slice.first;
                const auto last = first + slice.count;
                const auto at = std::lower_bound(
                        first, last, target, [](const Instruction& instruction, u64 pc) {
                            return instruction.View().PC() < pc;
                        });
                if (at == last || at->View().PC() != target) {
                    fail(DecodeError::OverlappingTarget, target);
                    return false;
                }
                if (directory.size() == limits.max_blocks) {
                    ++out.budget_exits;
                    return true;
                }
                const auto offset = static_cast<u32>(at - first);
                directory.emplace_hint(next,
                                       target,
                                       Slice{slice.end,
                                             static_cast<u32>(slice.first + offset),
                                             static_cast<u32>(slice.count - offset),
                                             false});
                slice.count = offset;
                slice.end = target;
                ++out.splits;
                return true;
            }
        }
        if (directory.size() == limits.max_blocks ||
            out.instructions.size() == limits.max_instructions) {
            ++out.budget_exits;
            return true;
        }
        directory.emplace_hint(next, target, Slice{target});
        pending.push_back(target);
        return true;
    };

    while (!pending.empty()) {
        const u64 start = pending.back();
        pending.pop_back();
        const auto current = directory.find(start);
        auto& slice = current->second;
        if (!slice.pending) continue;
        slice.pending = false;
        slice.first = static_cast<u32>(out.instructions.size());
        const auto next = std::next(current);
        const bool has_next = next != directory.end();
        const u64 next_start = has_next ? next->first : 0;
        u64 pc = start;
        auto window = ReadableAt(readable, pc);
        for (;;) {
            // Another admitted entry owns that stream from here onward.
            if (has_next && pc == next_start) break;
            if (out.instructions.size() == limits.max_instructions) {
                ++out.budget_exits;
                break;
            }
            if (pc != entry_pc && ready.Contains(pc)) break;
            // A readable-span boundary is a real exit frontier. Do not fetch
            // across separate spans, even if their guest addresses touch.
            if (window == readable.end() || pc - window->pc >= window->bytes.size()) break;
            const auto bytes = window->bytes.subspan(static_cast<std::size_t>(pc - window->pc));
            ++out.decode_calls;
            const auto decoded = DecodeOne(bytes, pc, timing, model);
            if (!decoded) {
                fail(decoded.error, pc);
                return out;
            }
            const auto& instruction = decoded.instruction.View().Record();
            const u64 after = pc + instruction.size;
            if (has_next && after > next_start) {
                fail(DecodeError::OverlappingTarget, next_start);
                return out;
            }
            const auto flow = instruction.flow;
            out.instructions.push_back(decoded.instruction);
            ++slice.count;
            slice.end = after;
            out.decoded_bytes += instruction.size;
            // CPUID serializes the stream; LDMXCSR returns to the runtime to
            // install the new FP environment. REP STOS dispatches after one
            // restartable iteration. These frontiers admit no sequential edge.
            if (instruction.unit_boundary) {
                if ((flow == FlowKind::Conditional || flow == FlowKind::Jump) &&
                    instruction.ops[0].type == GuestOperandKind::Relative) {
                    if (!queue(after + instruction.immediate)) return out;
                    if (flow == FlowKind::Conditional && !queue(after)) return out;
                }
                break;
            }
            pc = after;
        }
    }
    out.blocks.reserve(directory.size());
    for (const auto& [pc, slice] : directory) {
        if (pc == entry_pc) out.entry_block = static_cast<u32>(out.blocks.size());
        out.blocks.push_back({pc, slice.first, slice.count});
    }
    return out;
}

}  // namespace swift::x86
