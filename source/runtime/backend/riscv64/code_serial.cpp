#include "runtime/backend/code_serial.h"

#include <array>
#include <cstring>
#include <optional>
#include <unordered_set>
#include <vector>
#include <fmt/format.h>

namespace swift::runtime::backend {

ScanResult ScanRiscvCodeUnit(std::span<const u8> code, const HostImageInfo& image,
                           u64 guest_window_size) {
    ScanResult result;
    const auto fail = [&](const char* reason) { result.reject_reason = reason; return result; };
    if (code.empty() || code.size() > UINT32_MAX || code.size() % 4)
        return fail("invalid RV64 code size");
    if (!image.size || (guest_window_size && image.base < guest_window_size))
        return fail("unknown or overlapping host image");
    const auto word = [&](u32 at) { u32 value; std::memcpy(&value, code.data() + at, 4); return value; };
    // RV64 emits fixed-width instructions. A dense byte map avoids one heap
    // allocation per instruction when validating large AOT/cache units.
    std::vector<u8> instructions(code.size() / 4);
    std::unordered_set<u32> literals, referenced;
    const auto executable = [&](u64 at) {
        return at % 4 == 0 && at < code.size() && instructions[at / 4];
    };
    for (u32 at = 0; at < code.size();) {
        const auto insn = word(at);
        if (insn == kRiscvHostPoolMagic) {
            if (at % 8 || code.size() - at < 8) return fail("misaligned RV64 literal pool");
            const u32 count = word(at + 4);
            if (!count || count > (code.size() - at - 8) / 8) return fail("truncated RV64 literal pool");
            for (u32 i = 0; i < count; ++i) {
                const u32 offset = at + 8 + i * 8;
                u64 value; std::memcpy(&value, code.data() + offset, 8);
                if (!image.Contains(value)) return fail("RV64 host literal is process-local or outside the host image");
                literals.insert(offset);
                result.relocs.push_back({offset, 2, 0, RelocKind::RiscvHostLiteral64,
                                        RelocUse::Unknown, value - image.base, value});
            }
            at += 8 + count * 8;
        } else {
            if ((insn & 3) != 3 || (insn & 0x1f) == 0x1f) return fail("unsupported RV64 instruction width");
            instructions[at / 4] = 1; at += 4;
        }
    }
    const auto inside = [&](s64 target) { return target >= 0 && executable(u64(target)); };
    std::array<std::optional<u64>, 32> constants;
    for (u32 at = 0; at < code.size(); at += 4) {
        if (!executable(at)) continue;
        constants[0] = 0;
        const u32 insn = word(at), opcode = insn & 0x7f, rd = (insn >> 7) & 31,
                  rs = (insn >> 15) & 31, kind = (insn >> 12) & 7;
        const s64 imm = s32(insn) >> 20;
        std::optional<u64> value;
        if (opcode == 0x17) {
            if (!executable(u64(at) + 4) || !rd) return fail("unpaired RV64 AUIPC");
            const u32 next = word(at + 4);
            if (((next >> 15) & 31) != rd || ((next >> 7) & 31) != rd)
                return fail("invalid RV64 PC-relative pair");
            const s64 target = s64(at) + s32(insn & 0xfffff000) + (s32(next) >> 20);
            if ((next & 0x707f) == 0x3003) {
                if (target < 0 || target > UINT32_MAX || !literals.contains(u32(target)))
                    return fail("RV64 PC-relative load is not a host literal");
                referenced.insert(u32(target));
            } else if ((next & 0x707f) != 0x13 || !inside(target))
                return fail("RV64 PC-relative address leaves executable code");
        } else if (opcode == 0x6f || opcode == 0x63) {
            s32 delta;
            if (opcode == 0x6f) {
                const u32 bits = ((insn >> 31) << 20) | (((insn >> 12) & 255) << 12) |
                                 (((insn >> 20) & 1) << 11) | (((insn >> 21) & 1023) << 1);
                delta = s32(bits << 11) >> 11;
            } else {
                const u32 bits = ((insn >> 31) << 12) | (((insn >> 7) & 1) << 11) |
                                 (((insn >> 25) & 63) << 5) | (((insn >> 8) & 15) << 1);
                delta = s32(bits << 19) >> 19;
            }
            if (!inside(s64(at) + delta)) return fail("RV64 branch leaves executable code");
            constants = {};
        } else if (opcode == 0x37) value = u64(s64(s32(insn & 0xfffff000)));
        else if ((opcode == 0x13 || opcode == 0x1b) && constants[rs]) {
            if (!kind) value = opcode == 0x1b ? u64(s64(s32(*constants[rs] + imm))) : *constants[rs] + imm;
            else if (opcode == 0x13 && kind == 1 && (insn >> 26) == 0) value = *constants[rs] << ((insn >> 20) & 63);
            else if (opcode == 0x13 && kind == 5 && (insn >> 26) == 0) value = *constants[rs] >> ((insn >> 20) & 63);
            else if (opcode == 0x13 && kind == 5 && (insn >> 26) == 0x10) value = u64(s64(*constants[rs]) >> ((insn >> 20) & 63));
            else if (opcode == 0x13 && kind == 6) value = *constants[rs] | u64(imm);
        }
        if (value && rd && image.Contains(*value)) {
            // LI can transiently form an image-looking prefix of an ordinary
            // 64-bit integer. Inspect the completed immediate chain, before
            // any consumer can use that register as an address.
            bool continues{};
            if (executable(u64(at) + 4)) {
                const auto next = word(at + 4), next_op = next & 0x7f, next_kind = (next >> 12) & 7;
                continues = ((next >> 7) & 31) == rd && ((next >> 15) & 31) == rd &&
                            ((next_op == 0x1b && !next_kind) ||
                             (next_op == 0x13 && (!next_kind || next_kind == 6 ||
                              (next_kind == 1 && (next >> 26) == 0) ||
                              (next_kind == 5 && ((next >> 26) == 0 || (next >> 26) == 0x10)))));
            }
            if (!continues) {
                result.reject_reason = fmt::format("untracked RV64 absolute host pointer {:#x} at {:#x}", *value, at);
                return result;
            }
        }
        // Stores and conditional branches have no destination register.
        if (opcode != 0x23 && opcode != 0x27 && opcode != 0x63 && rd) constants[rd] = value;
        if (opcode == 0x67) constants = {};
    }
    if (referenced.size() != literals.size()) return fail("unreferenced RV64 host literal");
    result.materializations = literals.size(); result.ok = true;
    return result;
}
} // namespace swift::runtime::backend
