//
// x86 floating-point compare predicate -> IR relation set.
//
// WHY THIS FILE EXISTS
// --------------------
// SSE's CMPPS carries a 3-bit predicate (eq, lt, le, unord and their four
// negations); AVX's VCMPPS widened the imm8 to 5 bits and defined 32 named
// predicates.  The obvious "fix" is to let ir::OpCode::VecFCmpMask take the
// x86 imm8 straight through -- which would fossilize an x86 encoding inside a
// machine-independent IR, and leave the two backends decoding _CMP_NGE_UQ by
// number.  An ARM or RISC-V front end would have nothing to say about imm8=25.
//
// So VecFCmpMask's predicate is a RELATION SET instead: the four mutually
// exclusive outcomes of an IEEE 754 comparison, one bit each, and the lane's
// mask is all-ones exactly when the actual outcome is in the set.  Every FP
// compare in every ISA is a subset of {<, ==, >, unordered}, so the encoding
// is universal, total (all 16 subsets are meaningful) and self-describing.
// This file is where the x86-specific imm8 is translated into it, which is
// exactly where an ISA-specific encoding belongs: the front end.
//
// WHAT HAPPENED TO signalling-vs-quiet
// ------------------------------------
// AVX's 32 predicates are 16 relations x {signalling, quiet}: imm8 bit 4 is
// the toggle, and imm8 `i` and `i + 16` name the same relation.  Measured on
// hardware through Rosetta (vcmpps, ymm, 8 lanes covering <, ==, >, QNaN,
// SNaN, +-0, +-Inf and denormal): imm8 i and i+16 produced BIT-IDENTICAL
// result masks for all 16 values of i, while MXCSR's IE bit followed the
// signalling classification exactly -- with a QNaN operand and nothing else,
// the eight signalling predicates of 0..15 (1,2,5,6,9,10,13,14) set IE and the
// eight quiet ones did not, and 16..31 inverted that, matching the SDM's
// _OS/_OQ naming.
//
// The distinction is therefore ONLY an exception-flag distinction, and SwiftVM
// models no FP exception state at all -- MXCSR's exception flags are never
// updated by any arithmetic op, which is also why VUCOMISS and VCOMISS share a
// single handler.  Representing "signalling" in the IR would add a field no
// backend could act on.  It is deliberately dropped here, at the front end,
// where the reason is recorded, rather than carried into the IR as dead
// weight.  If MXCSR exception flags are ever modelled, they will be a
// cross-cutting change to every FP opcode and not a property of this one.
//
#pragma once

#include "runtime/common/types.h"

namespace swift::x86 {

using namespace swift::runtime;

// The IR relation-set bits.  Must agree with the comment on VecFCmpMask in
// runtime/ir/ir.inc and with both backends.
enum FpRelation : u32 {
    kFpLess = 1u << 0,
    kFpEqual = 1u << 1,
    kFpGreater = 1u << 2,
    kFpUnordered = 1u << 3,
};

// x86 imm8 (mod 16 -- bit 4 is the signalling toggle, see above) -> relation
// set.  Row order is the SDM's Table 3-1 / the _CMP_* constants in immintrin.h.
inline constexpr u32 kX86CmpPredicateToRelation[16] = {
        /*  0 EQ_OQ    */ kFpEqual,
        /*  1 LT_OS    */ kFpLess,
        /*  2 LE_OS    */ kFpLess | kFpEqual,
        /*  3 UNORD_Q  */ kFpUnordered,
        /*  4 NEQ_UQ   */ kFpLess | kFpGreater | kFpUnordered,
        /*  5 NLT_US   */ kFpEqual | kFpGreater | kFpUnordered,
        /*  6 NLE_US   */ kFpGreater | kFpUnordered,
        /*  7 ORD_Q    */ kFpLess | kFpEqual | kFpGreater,
        /*  8 EQ_UQ    */ kFpEqual | kFpUnordered,
        /*  9 NGE_US   */ kFpLess | kFpUnordered,
        /* 10 NGT_US   */ kFpLess | kFpEqual | kFpUnordered,
        /* 11 FALSE_OQ */ 0,
        /* 12 NEQ_OQ   */ kFpLess | kFpGreater,
        /* 13 GE_OS    */ kFpEqual | kFpGreater,
        /* 14 GT_OS    */ kFpGreater,
        /* 15 TRUE_UQ  */ kFpLess | kFpEqual | kFpGreater | kFpUnordered,
};

// The value VecFCmpMask's predicate immediate wants for an x86 compare whose
// imm8 is `imm`.  Accepts the full AVX range 0..31; the legacy SSE forms pass
// their 0..7 opcode-implied predicate, which is the same table's first eight
// rows and therefore cannot drift from the VEX path.
[[nodiscard]] inline constexpr u32 X86CmpPredicateToRelation(u32 imm) {
    return kX86CmpPredicateToRelation[imm & 15u];
}

}  // namespace swift::x86
