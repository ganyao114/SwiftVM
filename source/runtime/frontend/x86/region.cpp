#include "region.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

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
struct SliceEntry {
    u64 pc{}, end{};
    u32 first{}, count{};
    bool pending{true};
};

class SliceDirectory {
public:
    std::size_t size() const noexcept { return using_heap ? heap.size() : local_size; }

    SliceEntry* begin() noexcept { return using_heap ? heap.data() : local.data(); }
    SliceEntry* end() noexcept { return begin() + size(); }
    const SliceEntry* begin() const noexcept { return using_heap ? heap.data() : local.data(); }
    const SliceEntry* end() const noexcept { return begin() + size(); }

    SliceEntry* lower_bound(u64 pc) noexcept {
        return std::lower_bound(begin(), end(), pc, [](const SliceEntry& entry, u64 address) {
            return entry.pc < address;
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
}  // namespace

DecodedFunction DecodeFunction(std::span<const BlockWindow> readable,
                               u64 entry_pc,
                               DecodeLimits limits,
                               DecodeTiming* timing,
                               ReadyEntryLookup ready,
                               runtime::x86::GuestCpuModel model,
                               std::vector<runtime::SourceSpan>* source_spans,
                               std::vector<std::uint64_t>* continuations,
                               DecodedFunction* reusable_decoded) {
    if (source_spans) source_spans->clear();
    if (continuations) continuations->clear();
    DecodedFunction out;
    if (reusable_decoded) {
        out.instructions = std::move(reusable_decoded->instructions);
        out.blocks = std::move(reusable_decoded->blocks);
        out.instructions.clear();
        out.blocks.clear();
    }
    if (!Valid(readable, limits) || ReadableAt(readable, entry_pc) == readable.end()) {
        out.error = DecodeError::InvalidInput;
        out.error_pc = entry_pc;
        return out;
    }
    SliceDirectory directory;
    std::array<u64, 64> local_pending;
    std::size_t pending_size{};
    std::vector<u64> heap_pending;
    const auto push_pending = [&](u64 pc) {
        if (heap_pending.empty() && pending_size < local_pending.size()) {
            local_pending[pending_size++] = pc;
            return;
        }
        if (heap_pending.empty()) {
            heap_pending.reserve(local_pending.size() * 2);
            heap_pending.assign(local_pending.begin(), local_pending.begin() + pending_size);
        }
        heap_pending.push_back(pc);
    };
    const auto pop_pending = [&]() -> u64 {
        if (!heap_pending.empty()) {
            const u64 top = heap_pending.back();
            heap_pending.pop_back();
            return top;
        }
        return local_pending[--pending_size];
    };
    const auto has_pending = [&]() noexcept {
        return !heap_pending.empty() || pending_size != 0;
    };

    if (out.instructions.capacity() < std::min<u32>(limits.max_instructions, 32))
        out.instructions.reserve(std::min<u32>(limits.max_instructions, 32));
    directory.insert_at(0, SliceEntry{entry_pc, entry_pc});
    push_pending(entry_pc);
    const auto fail = [&](DecodeError error, u64 pc) {
        out.error = error;
        out.error_pc = pc;
    };
    const auto queue = [&](u64 target) {
        if (ReadableAt(readable, target) == readable.end()) return true;
        if (target != entry_pc && ready.Contains(target)) return true;
        auto* next = directory.lower_bound(target);
        const auto next_idx = static_cast<std::size_t>(next - directory.begin());
        if (next != directory.end() && next->pc == target) return true;
        if (next_idx != 0) {
            auto& slice = directory.begin()[next_idx - 1];
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
                const SliceEntry split_tail{target,
                                            slice.end,
                                            static_cast<u32>(slice.first + offset),
                                            static_cast<u32>(slice.count - offset),
                                            false};
                slice.count = offset;
                slice.end = target;
                directory.insert_at(next_idx, split_tail);
                ++out.splits;
                return true;
            }
        }
        if (directory.size() == limits.max_blocks ||
            out.instructions.size() == limits.max_instructions) {
            ++out.budget_exits;
            return true;
        }
        directory.insert_at(next_idx, SliceEntry{target, target});
        push_pending(target);
        return true;
    };

    while (has_pending()) {
        const u64 start = pop_pending();
        auto* current = directory.lower_bound(start);
        if (!current->pending) continue;
        current->pending = false;
        current->first = static_cast<u32>(out.instructions.size());
        const auto current_idx = static_cast<std::size_t>(current - directory.begin());
        const bool has_next = current_idx + 1 < directory.size();
        const u64 next_start = has_next ? directory.begin()[current_idx + 1].pc : 0;
        u64 pc = start;
        u64 span_end = start;
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
            ++directory.begin()[current_idx].count;
            directory.begin()[current_idx].end = after;
            span_end = after;
            out.decoded_bytes += instruction.size;
            // CPUID serializes the stream; LDMXCSR returns to the runtime to
            // install the new FP environment. REP STOS dispatches after one
            // restartable iteration. These frontiers admit no sequential edge.
            if (instruction.unit_boundary) {
                if (continuations && (flow == FlowKind::Call || instruction.opcode == Op::CPUID ||
                                      instruction.opcode == Op::LDMXCSR))
                    continuations->push_back(after);
                if ((flow == FlowKind::Conditional || flow == FlowKind::Jump) &&
                    instruction.ops[0].type == GuestOperandKind::Relative) {
                    if (!queue(after + instruction.immediate)) return out;
                    if (flow == FlowKind::Conditional && !queue(after)) return out;
                }
                break;
            }
            pc = after;
        }
        if (source_spans && span_end > start) source_spans->push_back({start, span_end - start});
    }
    if (out.blocks.capacity() < directory.size()) out.blocks.reserve(directory.size());
    for (const auto& entry : directory) {
        if (entry.pc == entry_pc) out.entry_block = static_cast<u32>(out.blocks.size());
        out.blocks.push_back({entry.pc, entry.first, entry.count});
    }
    return out;
}

}  // namespace swift::x86
