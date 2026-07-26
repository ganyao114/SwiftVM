//
// Self-contained VEX (AVX/AVX2) instruction decoder.
//
// WHY THIS EXISTS
// ---------------
// The bundled distorm snapshot is AVX1-era and cannot carry AVX. Measured over
// 117 representative AVX/AVX2 encodings:
//
//   * 40 are returned as I_UNDEFINED — every AVX2-only form (vpbroadcast*,
//     vpermd/vpermq/vpermps/vpermpd, vperm2i128, vinserti128/vextracti128,
//     vpblendd, vpsllvd/vpsrlvd/vpsravd, the gather family), plus several
//     AVX1 forms (vextractf128, vroundps, vpextrb).
//   * 38 more decode but SILENTLY DROP VEX.L, reporting 128-bit XMM operands
//     for a 256-bit encoding with no error flag — indistinguishable at the API
//     level from the genuine 128-bit form (see II_V_66_0F_D7 and friends).
//
// That is 78 of 117 broken, so no amount of handler work on top of distorm can
// produce a complete AVX front end. VEX is a fixed-shape encoding, so decoding
// it directly is both smaller and exact. This decoder therefore owns every
// VEX-prefixed instruction end to end; distorm keeps everything else.
//
// A further benefit: distorm's operand list is a lossy view (it is what let the
// VPMOVMSKB source-register defect through). Handlers built on VexInsn read the
// architectural fields directly.
//
#pragma once

#include "runtime/common/types.h"

namespace swift::x86 {

using namespace swift::runtime;

// Opcode map selected by VEX.mmmmm.
enum class VexMap : u8 { Invalid = 0, Map0F = 1, Map0F38 = 2, Map0F3A = 3 };

// Mandatory-prefix slot selected by VEX.pp.
enum class VexPP : u8 { None = 0, P66 = 1, PF3 = 2, PF2 = 3 };

// How the r/m operand is expressed.
enum class VexRmKind : u8 {
    Register,  // mod == 11
    Memory,    // any other mod; base/index/disp below describe it
};

struct VexInsn {
    // --- prefix ---------------------------------------------------------
    bool valid{false};       // a well-formed VEX prefix was decoded
    bool three_byte{false};  // C4 (true) or C5 (false)
    VexMap map{VexMap::Invalid};
    VexPP pp{VexPP::None};
    bool w{false};  // VEX.W
    bool l{false};  // VEX.L: false = 128-bit, true = 256-bit

    // The register encoded in VEX.vvvv, already un-inverted. The encoding
    // marks "no such operand" with a raw field of 0b1111, which un-inverts to
    // register 0 and is otherwise indistinguishable from a genuine xmm0
    // source — always consult vvvv_valid before using vvvv.
    u8 vvvv{0};
    bool vvvv_valid{false};

    // --- opcode and ModRM ------------------------------------------------
    u8 opcode{0};
    u8 mod{0};
    u8 reg{0};  // ModRM.reg with VEX.R folded in (0..15)
    u8 rm{0};   // ModRM.rm with VEX.B folded in (0..15); register number only
                // when rm_kind == Register
    VexRmKind rm_kind{VexRmKind::Register};

    // --- memory operand (rm_kind == Memory) ------------------------------
    bool has_sib{false};
    u8 base{0};           // 0..15; valid unless base_none
    bool base_none{false};  // no base register (mod==00 with SIB base==101)
    u8 index{0};          // 0..15; valid unless index_none
    bool index_none{true};
    u8 scale{1};          // 1, 2, 4 or 8
    bool rip_relative{false};
    s32 displacement{0};

    // --- trailing immediate ---------------------------------------------
    bool has_imm8{false};
    u8 imm8{0};
    // is4: the /is4 byte of vblendvps/vpblendvb etc. encodes a register in its
    // high nibble. Same byte as imm8; split out for clarity.
    u8 is4_register{0};

    // --- bookkeeping -----------------------------------------------------
    u8 length{0};  // total instruction length in bytes, prefixes included

    [[nodiscard]] bool Is256() const { return l; }
    [[nodiscard]] bool RmIsRegister() const { return rm_kind == VexRmKind::Register; }
};

// Whether `bytes` begins (after any legacy segment / address-size override)
// with a VEX prefix. Cheap; used to route away from distorm before decoding.
[[nodiscard]] bool HasVexPrefix(const u8* bytes, size_t available);

// Decode one VEX instruction. `available` bounds the read so a truncated tail
// of a code page cannot be read past. Returns a VexInsn with valid=false when
// the bytes are not a VEX instruction, are truncated, or use an encoding this
// decoder does not model.
//
// Immediate presence is opcode-directed (see kImmediateForms in the .cc): a
// wrong answer there would mis-measure the instruction length and desynchronize
// the whole decode stream, so unknown (map, pp, opcode) triples decode with
// valid=false rather than guessing.
[[nodiscard]] VexInsn DecodeVexInsn(const u8* bytes, size_t available);

}  // namespace swift::x86
