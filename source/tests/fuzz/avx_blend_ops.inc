// VEX blend / extract-insert-scalar / masked-move instruction table.
//
// Shared VERBATIM by the Rosetta reference generator (avx_blend_rosetta_ref.c)
// and the SwiftVM differential case (avx_blend_test.cpp).
//
// WHAT THIS TABLE IS AND IS NOT
// -----------------------------
// Only the GENERATOR encodes.  Every reference row carries the LITERAL
// instruction bytes it executed and the test replays those bytes, so a wrong
// field here cannot make the two sides assemble the same wrong instruction and
// pass vacuously -- the failure mode avx_fp_ops.inc had to guard against by
// hand-auditing an encoder on each side.  This table's remaining job is (a)
// naming what the generator must cover and (b) letting the test assert that
// every named mnemonic actually produced rows.
//
// Field meaning (all values UN-inverted):
//   shape  : how the generator builds the operands -- see the S_* list below
//   map    : 1 = 0F, 2 = 0F38, 3 = 0F3A
//   pp     : 0 = none, 1 = 66, 2 = F3, 3 = F2
//   w      : VEX.W
//   opcode : the opcode byte
//   aux    : shape-dependent -- the lane width in bits, or 0
//
// SHAPES
//   S_BLEND   dst = ymm0, VEX.vvvv = ymm1, r/m = ymm2 or [rdi+B], imm8 swept
//             over a set chosen per lane width.  Both VEX.L, both r/m shapes.
//   S_BLENDV  the /is4 form: as S_BLEND plus a selector register named by the
//             imm8's HIGH NIBBLE.  Swept over ymm1 / ymm2 / ymm3 as the
//             selector so an implementation that reads VEX.vvvv, ModRM.rm or a
//             fixed register instead of the /is4 nibble is caught, and over a
//             non-zero LOW nibble, which is architecturally ignored.
//   S_EXTRPS  vextractps: ModRM.reg is the XMM SOURCE and r/m the r32/m32
//             DESTINATION -- the reverse of every other shape here.  VEX.128
//             only.  Register rows pre-set rax to -1 so the 32-bit write's
//             zero-extension of bits 63:32 is measured.
//   S_INSPS   vinsertps: imm8 is three unrelated fields (source lane 7:6,
//             destination lane 5:4, zero mask 3:0) and all three are swept.
//             The MEMORY rows sweep 7:6 as well, where it must be IGNORED.
//             VEX.128 only.
//   S_MASKLD  vmaskmov* load: dst = ymm0, mask = VEX.vvvv, source = memory.
//             Also emitted with the mask register ALIASING the destination
//             (`vmaskmovps ymm0, ymm0, [rdi+B]`), which is both a legal
//             encoding and the case where VEX.vvvv's raw 1111b marker means a
//             genuine register 0 rather than "no operand".
//   S_MASKST  vmaskmov* store: destination = memory, mask = VEX.vvvv, source =
//             ModRM.reg.  The capture slot is pre-filled with 0xCC, so every
//             byte NOT written by the instruction is visible as 0xCC in the
//             reference -- which is what makes "masked-off elements are not
//             stored" a measured property rather than an assumed one.
//
// Every register-destination row runs against a POISONED ymm0 and reads all 32
// bytes back, so contract C3 (a VEX.128 write zeroes bits 255:128) is MEASURED
// on hardware for each one.

#ifndef SVM_BLEND
#define SVM_BLEND(name, shape, map, pp, w, opcode, aux)
#endif

// ---- imm8-controlled blends -----------------------------------------------
// The control is a decode-time constant, so what can go wrong is the BIT
// LAYOUT: which imm8 bit drives which lane of which 128-bit half.  The imm8
// sets in the generator therefore include values that differ between their
// nibbles, which is the only way a half-swap is visible.
SVM_BLEND(vblendps, S_BLEND, 3, 1, 0, 0x0C, 32)
SVM_BLEND(vblendpd, S_BLEND, 3, 1, 0, 0x0D, 64)

// ---- register-controlled blends (/is4) ------------------------------------
// Only each lane's MOST SIGNIFICANT bit selects; the rest of the selector lane
// must be ignored, which the `signs` and `random` input triples both exercise.
SVM_BLEND(vblendvps, S_BLENDV, 3, 1, 0, 0x4A, 32)
SVM_BLEND(vblendvpd, S_BLENDV, 3, 1, 0, 0x4B, 64)

// ---- scalar extract / insert ----------------------------------------------
SVM_BLEND(vextractps, S_EXTRPS, 3, 1, 0, 0x17, 32)
SVM_BLEND(vinsertps, S_INSPS, 3, 1, 0, 0x21, 32)

// ---- masked moves ---------------------------------------------------------
// Data path only -- the FAULT-SUPPRESSION half of these instructions cannot be
// expressed as a reference row (a row records a value, not the absence of a
// signal), so avx_blend_test.cpp asserts it directly against PROT_NONE pages.
SVM_BLEND(vmaskmovps, S_MASKLD, 2, 1, 0, 0x2C, 32)
SVM_BLEND(vmaskmovpd, S_MASKLD, 2, 1, 0, 0x2D, 64)
SVM_BLEND(vmaskmovps, S_MASKST, 2, 1, 0, 0x2E, 32)
SVM_BLEND(vmaskmovpd, S_MASKST, 2, 1, 0, 0x2F, 64)

#undef SVM_BLEND
