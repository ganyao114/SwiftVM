// The mnemonics decoder_sse4.cc claims, in the spelling the reference
// generator prints.
//
// Shared by the Rosetta reference generator (sse4_rosetta_ref.c) and the
// SwiftVM differential case (sse4_test.cpp).  Only the generator encodes;
// every reference row carries the literal instruction BYTES it executed and
// the test replays those, so encoder drift between the two sides is
// structurally impossible.  This table's job is therefore only to (a) name
// what must be covered and (b) let the test assert that every named mnemonic
// actually produced rows -- a handler silently dropped from the generator
// would otherwise vanish from the run with no failure at all.
//
// The "v" spellings are the VEX twins, emitted with IDENTICAL operands so the
// test can require that the legacy form and the VEX form agree bit for bit on
// the low 128 bits while disagreeing exactly as specified on bits 255:128
// (legacy preserves, VEX zeroes).  That comparison is what makes the
// deliberate duplication between decoder_sse4.cc and decoder_avx*.cc safe.

#ifndef SVM_SSE4
#define SVM_SSE4(name)
#endif

// ---- SSE4.1 rounding ------------------------------------------------------
SVM_SSE4("roundps")
SVM_SSE4("roundpd")
SVM_SSE4("roundss")
SVM_SSE4("roundsd")
SVM_SSE4("roundps.mx")
SVM_SSE4("roundpd.mx")
SVM_SSE4("roundss.mx")
SVM_SSE4("roundsd.mx")
SVM_SSE4("vroundps")
SVM_SSE4("vroundpd")

// ---- SSE4.1 ptest ---------------------------------------------------------
SVM_SSE4("ptest")
SVM_SSE4("vptest")

// ---- SSE4.1 sign / zero extension -----------------------------------------
SVM_SSE4("pmovsxbw")
SVM_SSE4("pmovsxbd")
SVM_SSE4("pmovsxbq")
SVM_SSE4("pmovsxwd")
SVM_SSE4("pmovsxwq")
SVM_SSE4("pmovsxdq")
SVM_SSE4("pmovzxbw")
SVM_SSE4("pmovzxbd")
SVM_SSE4("pmovzxbq")
SVM_SSE4("pmovzxwd")
SVM_SSE4("pmovzxwq")
SVM_SSE4("pmovzxdq")
SVM_SSE4("vpmovsxbw")
SVM_SSE4("vpmovzxdq")

// ---- SSE4.1 blends --------------------------------------------------------
SVM_SSE4("blendvps")
SVM_SSE4("blendvpd")
SVM_SSE4("pblendvb")
SVM_SSE4("blendps")
SVM_SSE4("blendpd")
SVM_SSE4("pblendw")
SVM_SSE4("vblendvps")
SVM_SSE4("vpblendvb")
SVM_SSE4("vblendps")
SVM_SSE4("vpblendw")

// ---- SSE4.1 / SSE4.2 arithmetic and compare --------------------------------
SVM_SSE4("pmulld")
SVM_SSE4("pcmpeqq")
SVM_SSE4("pcmpgtq")
SVM_SSE4("packusdw")
SVM_SSE4("vpmulld")
SVM_SSE4("vpcmpeqq")
SVM_SSE4("vpcmpgtq")
SVM_SSE4("vpackusdw")

// ---- SSE4.1 insert / extract ------------------------------------------------
SVM_SSE4("insertps")
SVM_SSE4("extractps")
SVM_SSE4("pinsrb")
SVM_SSE4("pinsrd")
SVM_SSE4("pinsrq")
SVM_SSE4("pextrb")
SVM_SSE4("pextrd")
SVM_SSE4("pextrq")
SVM_SSE4("vinsertps")
SVM_SSE4("vpextrb")

// ---- SSSE3 -----------------------------------------------------------------
SVM_SSE4("phaddw")
SVM_SSE4("phaddd")
SVM_SSE4("phaddsw")
SVM_SSE4("phsubw")
SVM_SSE4("phsubd")
SVM_SSE4("phsubsw")
SVM_SSE4("psignb")
SVM_SSE4("psignw")
SVM_SSE4("psignd")
SVM_SSE4("pmulhrsw")
SVM_SSE4("pmaddubsw")
SVM_SSE4("pabsb")
SVM_SSE4("pabsw")
SVM_SSE4("pabsd")
SVM_SSE4("vphaddw")
SVM_SSE4("vpabsd")
SVM_SSE4("vpmaddubsw")

// ---- SSE4.1 min / max --------------------------------------------------------
SVM_SSE4("pminsb")
SVM_SSE4("pminsd")
SVM_SSE4("pminuw")
SVM_SSE4("pminud")
SVM_SSE4("pmaxsb")
SVM_SSE4("pmaxsd")
SVM_SSE4("pmaxuw")
SVM_SSE4("pmaxud")
SVM_SSE4("vpminsb")
SVM_SSE4("vpmaxud")

// ---- SSE3 horizontal float and add/subtract ----------------------------------
SVM_SSE4("haddpd")
SVM_SSE4("hsubpd")
SVM_SSE4("haddps")
SVM_SSE4("hsubps")
SVM_SSE4("addsubps")
SVM_SSE4("addsubpd")
SVM_SSE4("vhaddpd")

// ---- SSE4.1 dot product --------------------------------------------------------
SVM_SSE4("dpps")
SVM_SSE4("dppd")
SVM_SSE4("vdpps")

// ---- SSE4.1 odds and ends -------------------------------------------------------
SVM_SSE4("mpsadbw")
SVM_SSE4("phminposuw")
SVM_SSE4("movntdqa")

#undef SVM_SSE4
