// The VEX horizontal / pairwise family: what
// source/runtime/frontend/x86/decoder_avx_hadd.cc must cover.
//
// Shared VERBATIM by the Rosetta reference generator (avx_hadd_rosetta_ref.c)
// and the differential case (avx_hadd_test.cpp).
//
// Following the second wave's convention, only the GENERATOR encodes from this
// table.  Every reference row carries the literal instruction BYTES that were
// executed and avx_hadd_test.cpp replays those, so a wrong field here can only
// lose coverage -- it cannot make the two sides agree on the same wrong
// instruction, which is how a differential passes vacuously.  This table's
// remaining jobs are (a) telling the generator what to emit and (b) letting the
// test assert that every mnemonic named here actually produced rows.
//
// Field meaning (all values UN-inverted):
//   map    : 1 = 0F, 2 = 0F38, 3 = 0F3A
//   pp     : 0 = none, 1 = 66, 2 = F3, 3 = F2
//   opcode : the opcode byte
//   lanes  : element width in bits -- documentation for the reader and the
//            width the test's independent SDM model uses
//   kind   : which pairwise operation the SDM defines, for that same model:
//            0 = float add     1 = float sub
//            2 = int add       3 = int sub
//            4 = int add, signed-saturating
//            5 = int sub, signed-saturating
//            6 = vpmaddubsw (unsigned x signed bytes, saturating pair sum)
//
// EVERY entry is emitted at BOTH VEX.L values and in BOTH operand shapes
// (register r/m and memory r/m).  The 128-bit forms are not an afterthought:
// the second wave found six opcodes whose 256-bit form worked while the
// VEX.128 twin killed the guest, and this family would fail the same way if
// only one width were covered.

#ifndef SVM_HADD
#define SVM_HADD(name, map, pp, opcode, lanes, kind)
#endif

// ---- floating point, 0F map ----------------------------------------------
// The F2 (packed single) and 66 (packed double) slots of 7C/7D.  The NP and F3
// slots are architecturally #UD and are deliberately not listed.
SVM_HADD(vhaddps, 1, 3, 0x7C, 32, 0)
SVM_HADD(vhaddpd, 1, 1, 0x7C, 64, 0)
SVM_HADD(vhsubps, 1, 3, 0x7D, 32, 1)
SVM_HADD(vhsubpd, 1, 1, 0x7D, 64, 1)

// ---- integer, 0F38 map ----------------------------------------------------
// 0F38 01..07 is a contiguous block; 0x04 is the odd one out (vpmaddubsw is
// not a horizontal add of its own operands but a pairwise multiply-accumulate
// ACROSS them), which is why it sits between vphaddsw and vphsubw.
SVM_HADD(vphaddw, 2, 1, 0x01, 16, 2)
SVM_HADD(vphaddd, 2, 1, 0x02, 32, 2)
SVM_HADD(vphaddsw, 2, 1, 0x03, 16, 4)
SVM_HADD(vpmaddubsw, 2, 1, 0x04, 16, 6)
SVM_HADD(vphsubw, 2, 1, 0x05, 16, 3)
SVM_HADD(vphsubd, 2, 1, 0x06, 32, 3)
SVM_HADD(vphsubsw, 2, 1, 0x07, 16, 5)
