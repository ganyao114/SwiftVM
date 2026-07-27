// The AVX2 gather family: eight opcodes, VEX.128 and VEX.256 for each.
//
// Shared VERBATIM by the Rosetta reference generator (avx_gather_rosetta_ref.c)
// and the SwiftVM differential case (avx_gather_test.cpp).  Only the GENERATOR
// encodes; every reference row carries the literal instruction BYTES it
// executed and the test replays those, so encoder drift between the two sides
// is structurally impossible.  This table's job is (a) naming what the
// generator must cover and (b) letting the test assert that every named
// mnemonic actually produced rows.
//
// Field meaning:
//   opcode : the byte on the 66.0F38 map -- 90/91 integer, 92/93 float
//   w      : VEX.W.  W selects the DESTINATION element size (0 = dword,
//            1 = qword); the opcode's low bit selects the INDEX size
//            (90/92 = dword indices, 91/93 = qword indices).
//   ebits  : destination element bits, i.e. 32 << w
//   ibits  : index element bits
//
// The number of elements a form gathers is VL / max(ebits, ibits), so the four
// combinations behave differently and all four are here:
//
//   dd  32/32   VEX.128 gathers 4, VEX.256 gathers 8
//   qd  32/64   VEX.128 gathers 2 into an XMM dst, VEX.256 gathers 4 -- the
//               destination is an XMM at BOTH widths and the bits above it are
//               zeroed, which is the easiest form in the family to get wrong
//   dq  64/32   the index operand is an XMM at BOTH widths (vm32x): a VEX.256
//               vpgatherdq reads its four dword indices from the LOW half of
//               the index register
//   qq  64/64   VEX.128 gathers 2, VEX.256 gathers 4
//
// vgatherdps/qps/dpd/qpd (92/93) move exactly the same bits as their integer
// twins; they are listed separately because a decoder that routed only 90/91
// would kill the guest on the float spellings, which is what a compiler
// actually emits for gathered float data.

#ifndef SVM_GATHER
#define SVM_GATHER(name, opcode, w, ebits, ibits)
#endif

SVM_GATHER(vpgatherdd, 0x90, 0, 32, 32)
SVM_GATHER(vpgatherqd, 0x91, 0, 32, 64)
SVM_GATHER(vpgatherdq, 0x90, 1, 64, 32)
SVM_GATHER(vpgatherqq, 0x91, 1, 64, 64)
SVM_GATHER(vgatherdps, 0x92, 0, 32, 32)
SVM_GATHER(vgatherqps, 0x93, 0, 32, 64)
SVM_GATHER(vgatherdpd, 0x92, 1, 64, 32)
SVM_GATHER(vgatherqpd, 0x93, 1, 64, 64)

#undef SVM_GATHER
