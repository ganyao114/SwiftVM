// The mnemonics decoder_avx_misc.cc claims.
//
// Shared by the Rosetta reference generator (avx_misc_rosetta_ref.c) and the
// SwiftVM differential case (avx_misc_test.cpp).  Only the generator encodes;
// every reference row carries the literal instruction BYTES it executed and
// the test replays those, so encoder drift between the two sides is
// structurally impossible.  This table's job is therefore only to (a) name
// what must be covered and (b) let the test assert that every named mnemonic
// actually produced rows -- a handler silently dropped from the generator
// would otherwise vanish from the run with no failure.
//
// The ".mx" entries are the same instructions with imm8 bit 2 set (round per
// MXCSR.RC); their rows are wrapped in the ldmxcsr pair that selects RC, and
// the test asserts that the four RC values do not all agree -- without that,
// an implementation which ignored MXCSR entirely would pass every row whose
// RC happens to be 0.

#ifndef SVM_MISC
#define SVM_MISC(name)
#endif

SVM_MISC("vroundps")
SVM_MISC("vroundpd")
SVM_MISC("vroundss")
SVM_MISC("vroundsd")
SVM_MISC("vroundps.mx")
SVM_MISC("vroundpd.mx")
SVM_MISC("vroundss.mx")
SVM_MISC("vroundsd.mx")
SVM_MISC("vdpps")
SVM_MISC("vdppd")
SVM_MISC("vpermilpd")

#undef SVM_MISC
