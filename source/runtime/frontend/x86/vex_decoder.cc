//
// Self-contained VEX decoder. See vex_decoder.h for why this bypasses distorm.
//

#include "vex_decoder.h"

namespace swift::x86 {

namespace {

// Legacy prefixes that may still precede a VEX prefix. 66/F2/F3/REX before VEX
// is #UD, so only segment and address-size overrides are accepted here.
constexpr bool IsAllowedPrefix(u8 b) {
    return b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65 ||
           b == 0x67;
}

// Skip the allowed legacy prefixes; returns the offset of the first
// non-prefix byte, or `available` if the input runs out.
constexpr size_t SkipPrefixes(const u8* bytes, size_t available) {
    size_t i = 0;
    // A sane encoding has at most a handful; cap it so a run of 0x67 bytes
    // cannot walk the whole page.
    while (i < available && i < 4 && IsAllowedPrefix(bytes[i])) {
        ++i;
    }
    return i;
}

// Whether a (map, pp, opcode) triple carries a trailing imm8.
//
// This has to be exact. The immediate is what makes the difference between an
// N and an N+1 byte instruction, so a wrong answer here does not merely
// mis-decode one instruction — it desynchronizes every instruction after it in
// the block. Unknown triples are therefore rejected by the caller rather than
// assumed to have no immediate.
enum class ImmForm : u8 {
    Unknown,  // not modelled: refuse to decode
    None,
    Imm8,
    Is4,  // imm8 whose high nibble names a register (vblendv*, vpblendvb)
};

constexpr ImmForm ImmediateForm(VexMap map, VexPP pp, u8 op) {
    switch (map) {
    case VexMap::Map0F:
        switch (op) {
        // Compare predicate.
        case 0xC2:  // vcmpps/pd/ss/sd
        case 0x70:  // vpshufd / vpshuflw / vpshufhw
        case 0xC6:  // vshufps/pd
        case 0xC4:  // vpinsrw
        case 0xC5:  // vpextrw
            return ImmForm::Imm8;
        // Shift-by-immediate groups: the immediate belongs to the /n form.
        case 0x71:
        case 0x72:
        case 0x73:
            return ImmForm::Imm8;
        default:
            break;
        }
        // Everything else on the 0F map that AVX uses is a plain 2/3-operand
        // form with no immediate. The opcode space here is well covered by
        // the SSE heritage, so treating the remainder as None is safe.
        return ImmForm::None;
    case VexMap::Map0F38:
        // The 0F38 map is uniformly immediate-free in AVX/AVX2 (arithmetic,
        // broadcasts, variable shifts, gathers, FMA).
        (void)pp;
        return ImmForm::None;
    case VexMap::Map0F3A:
        // The 0F3A map is uniformly imm8-carrying; the /is4 forms additionally
        // encode a register in the immediate's high nibble.
        switch (op) {
        case 0x4A:  // vblendvps
        case 0x4B:  // vblendvpd
        case 0x4C:  // vpblendvb
            return ImmForm::Is4;
        default:
            return ImmForm::Imm8;
        }
    case VexMap::Invalid:
    default:
        return ImmForm::Unknown;
    }
}

}  // namespace

bool HasVexPrefix(const u8* bytes, size_t available) {
    if (!bytes) {
        return false;
    }
    const size_t i = SkipPrefixes(bytes, available);
    if (i >= available) {
        return false;
    }
    const u8 b = bytes[i];
    if (b != 0xC4 && b != 0xC5) {
        return false;
    }
    // In 64-bit mode C4/C5 are unambiguously VEX (the LES/LDS forms they used
    // to encode are invalid), but a second byte is still required to exist.
    return i + 1 < available;
}

VexInsn DecodeVexInsn(const u8* bytes, size_t available) {
    VexInsn insn{};
    if (!bytes) {
        return insn;
    }
    size_t i = SkipPrefixes(bytes, available);
    // Need at least: prefix bytes + opcode + ModRM.
    if (i + 2 >= available) {
        return insn;
    }

    u8 rex_r = 0, rex_x = 0, rex_b = 0;
    if (bytes[i] == 0xC5) {
        // 2-byte form: [R vvvv L pp]. Implies map 0F, W = 0, X = B = 0.
        const u8 b1 = bytes[i + 1];
        insn.three_byte = false;
        insn.map = VexMap::Map0F;
        insn.w = false;
        rex_r = (b1 & 0x80) ? 0 : 1;
        const u8 raw_vvvv = static_cast<u8>((b1 >> 3) & 0xF);
        insn.vvvv = static_cast<u8>(~raw_vvvv & 0xF);
        insn.vvvv_valid = raw_vvvv != 0xF;
        insn.l = (b1 & 0x04) != 0;
        insn.pp = static_cast<VexPP>(b1 & 0x03);
        i += 2;
    } else if (bytes[i] == 0xC4) {
        if (i + 3 >= available) {
            return insn;
        }
        // 3-byte form: [R X B mmmmm][W vvvv L pp].
        const u8 b1 = bytes[i + 1];
        const u8 b2 = bytes[i + 2];
        insn.three_byte = true;
        const u8 mmmmm = static_cast<u8>(b1 & 0x1F);
        if (mmmmm < 1 || mmmmm > 3) {
            return insn;  // reserved / not modelled
        }
        insn.map = static_cast<VexMap>(mmmmm);
        rex_r = (b1 & 0x80) ? 0 : 1;
        rex_x = (b1 & 0x40) ? 0 : 1;
        rex_b = (b1 & 0x20) ? 0 : 1;
        insn.w = (b2 & 0x80) != 0;
        const u8 raw_vvvv = static_cast<u8>((b2 >> 3) & 0xF);
        insn.vvvv = static_cast<u8>(~raw_vvvv & 0xF);
        insn.vvvv_valid = raw_vvvv != 0xF;
        insn.l = (b2 & 0x04) != 0;
        insn.pp = static_cast<VexPP>(b2 & 0x03);
        i += 3;
    } else {
        return insn;
    }

    if (i >= available) {
        return insn;
    }
    insn.opcode = bytes[i++];

    const ImmForm imm_form = ImmediateForm(insn.map, insn.pp, insn.opcode);
    if (imm_form == ImmForm::Unknown) {
        return insn;  // refuse rather than mis-measure the length
    }

    if (i >= available) {
        return insn;
    }
    const u8 modrm = bytes[i++];
    insn.mod = static_cast<u8>((modrm >> 6) & 0x3);
    insn.reg = static_cast<u8>(((modrm >> 3) & 0x7) | (rex_r << 3));
    const u8 rm_field = static_cast<u8>(modrm & 0x7);

    if (insn.mod == 3) {
        insn.rm_kind = VexRmKind::Register;
        insn.rm = static_cast<u8>(rm_field | (rex_b << 3));
    } else {
        insn.rm_kind = VexRmKind::Memory;
        insn.rm = 0;
        if (rm_field == 4) {
            // SIB byte follows.
            if (i >= available) {
                return insn;
            }
            const u8 sib = bytes[i++];
            insn.has_sib = true;
            insn.scale = static_cast<u8>(1u << ((sib >> 6) & 0x3));
            const u8 index_field = static_cast<u8>((sib >> 3) & 0x7);
            const u8 base_field = static_cast<u8>(sib & 0x7);
            // index == 100b with X == 0 means "no index"; with X == 1 it is r12.
            if (index_field == 4 && rex_x == 0) {
                insn.index_none = true;
            } else {
                insn.index_none = false;
                insn.index = static_cast<u8>(index_field | (rex_x << 3));
            }
            if (base_field == 5 && insn.mod == 0) {
                insn.base_none = true;  // disp32-only (plus optional index)
            } else {
                insn.base_none = false;
                insn.base = static_cast<u8>(base_field | (rex_b << 3));
            }
        } else if (rm_field == 5 && insn.mod == 0) {
            // RIP-relative: disp32 from the END of the instruction, so the
            // caller must add `length` itself.
            insn.rip_relative = true;
            insn.base_none = true;
        } else {
            insn.base_none = false;
            insn.base = static_cast<u8>(rm_field | (rex_b << 3));
        }

        // Displacement.
        if (insn.rip_relative || (insn.mod == 0 && insn.base_none && insn.has_sib) ||
            insn.mod == 2) {
            if (i + 4 > available) {
                return insn;
            }
            insn.displacement = static_cast<s32>(static_cast<u32>(bytes[i]) |
                                                 (static_cast<u32>(bytes[i + 1]) << 8) |
                                                 (static_cast<u32>(bytes[i + 2]) << 16) |
                                                 (static_cast<u32>(bytes[i + 3]) << 24));
            i += 4;
        } else if (insn.mod == 1) {
            if (i >= available) {
                return insn;
            }
            insn.displacement = static_cast<s8>(bytes[i]);
            i += 1;
        }
    }

    if (imm_form != ImmForm::None) {
        if (i >= available) {
            return insn;
        }
        insn.has_imm8 = true;
        insn.imm8 = bytes[i++];
        if (imm_form == ImmForm::Is4) {
            insn.is4_register = static_cast<u8>((insn.imm8 >> 4) & 0xF);
        }
    }

    insn.length = static_cast<u8>(i);
    insn.valid = true;
    return insn;
}

}  // namespace swift::x86
