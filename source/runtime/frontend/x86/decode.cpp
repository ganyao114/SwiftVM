#include "decode.h"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <limits>
#include <map>

#include "mnemonics.h"

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
            (i && windows[i - 1].pc + windows[i - 1].bytes.size() > window.pc)) return false;
    }
    return true;
}
auto ReadableAt(std::span<const BlockWindow> windows, u64 pc) {
    auto next = std::upper_bound(windows.begin(), windows.end(), pc,
            [](u64 address, const BlockWindow& window) { return address < window.pc; });
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

// Shared by the two byte-decoding entries and the emitter's prefix gate.
// The runtime advertises neither IBT nor a CET shadow-stack environment.
std::uint8_t DisabledCetHintSize(std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() < 4 || bytes[0] != 0xf3) return 0;
    std::size_t opcode = 1;
    if (bytes[1] >= 0x40 && bytes[1] <= 0x4f) {
        if (bytes[1] & 4) return 0; // REX.R must not turn the /1 extension into /9.
        opcode = 2;
    }
    if (bytes.size() < opcode + 3 || bytes[opcode] != 0x0f || bytes[opcode + 1] != 0x1e) return 0;
    const auto modrm = bytes[opcode + 2];
    if (opcode == 1 && (modrm == 0xfa || modrm == 0xfb)) return 4; // Exact ENDBR64/32.
    // RDSSPD/Q: /1, mod11, REX.W selects width and REX.B the high register.
    // Disabled shadow stacks make this a NOP: even a 32-bit form preserves
    // the complete destination carrier, rather than writing/zeroing EAX.
    return (modrm & 0xf8) == 0xc8 ? static_cast<std::uint8_t>(opcode + 3) : 0;
}

DecodedFunction DecodeFunction(std::span<const BlockWindow> readable, u64 entry_pc, DecodeLimits limits,
                                DecodeTiming* timing, ReadyEntryLookup ready) {
    DecodedFunction out;
    if (!Valid(readable, limits) || ReadableAt(readable, entry_pc) == readable.end()) {
        out.error = DecodeError::InvalidInput; out.error_pc = entry_pc; return out;
    }
    std::map<u64, Slice> directory;
    std::vector<u64> pending;
    directory.emplace(entry_pc, Slice{entry_pc});
    pending.push_back(entry_pc);
    const auto fail = [&](DecodeError error, u64 pc) {
        out.error = error; out.error_pc = pc;
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
                const auto at = std::lower_bound(first, last, target,
                        [](const _DInst& instruction, u64 pc) { return instruction.addr < pc; });
                if (at == last || at->addr != target) {
                    fail(DecodeError::OverlappingTarget, target); return false;
                }
                if (directory.size() == limits.max_blocks) { ++out.budget_exits; return true; }
                const auto offset = static_cast<u32>(at - first);
                directory.emplace_hint(next, target,
                        Slice{slice.end, static_cast<u32>(slice.first + offset),
                              static_cast<u32>(slice.count - offset), false});
                slice.count = offset; slice.end = target;
                ++out.splits;
                return true;
            }
        }
        if (directory.size() == limits.max_blocks || out.instructions.size() == limits.max_instructions) {
            ++out.budget_exits; return true;
        }
        directory.emplace_hint(next, target, Slice{target});
        pending.push_back(target);
        return true;
    };

    while (!pending.empty()) {
        const u64 start = pending.back(); pending.pop_back();
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
            if (out.instructions.size() == limits.max_instructions) { ++out.budget_exits; break; }
            if (pc != entry_pc && ready.Contains(pc)) break;
            // A readable-span boundary is a real exit frontier. Do not fetch
            // across separate spans, even if their guest addresses touch.
            if (window == readable.end() || pc - window->pc >= window->bytes.size()) break;
            const auto bytes = window->bytes.subspan(static_cast<std::size_t>(pc - window->pc));
            _CodeInfo code{};
            code.codeOffset = pc; code.code = bytes.data();
            code.codeLen = static_cast<int>(std::min<std::size_t>(bytes.size(), 15));
            code.dt = Decode64Bits;
            _DInst instruction{};
            unsigned used{};
            ++out.decode_calls;
            _DecodeResult parsed;
            if (timing) {
                const auto begin = std::chrono::steady_clock::now();
                parsed = distorm_decompose(&code, &instruction, 1, &used);
                const auto end = std::chrono::steady_clock::now();
                timing->nanoseconds += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
                ++timing->calls;
            } else parsed = distorm_decompose(&code, &instruction, 1, &used);
            if (bytes[0] == 0xf3) {
                if (const auto hint_size = DisabledCetHintSize(bytes)) {
                    instruction = {};
                    instruction.addr = pc; instruction.size = hint_size; instruction.opcode = I_NOP;
                    used = 1;
                }
            }
            if (!used) {
                fail(parsed == DECRES_INPUTERR ? DecodeError::InvalidEncoding : DecodeError::Truncated, pc);
                return out;
            }
            if (used != 1 || instruction.flags == FLAG_NOT_DECODABLE || !instruction.size || instruction.size > 15) {
                fail(DecodeError::InvalidEncoding, pc); return out;
            }
            if (instruction.size > bytes.size()) { fail(DecodeError::Truncated, pc); return out; }
            const u64 after = pc + instruction.size;
            if (has_next && after > next_start) { fail(DecodeError::OverlappingTarget, next_start); return out; }
            const auto flow = META_GET_FC(instruction.meta);
            out.instructions.push_back(instruction);
            ++slice.count; slice.end = after;
            out.decoded_bytes += instruction.size;
            // CPUID serializes the stream; LDMXCSR returns to the runtime to
            // install the new FP environment. REP STOS dispatches after one
            // restartable iteration. These frontiers admit no sequential edge.
            if (instruction.opcode == I_CPUID || instruction.opcode == I_LDMXCSR || instruction.opcode == I_STOS) break;
            if (flow != FC_NONE && flow != FC_CMOV) {
                if ((flow == FC_CND_BRANCH || flow == FC_UNC_BRANCH) && instruction.ops[0].type == O_PC) {
                    if (!queue(after + instruction.imm.addr)) return out;
                    if (flow == FC_CND_BRANCH && !queue(after)) return out;
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
