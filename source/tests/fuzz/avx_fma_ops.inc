// FMA3 instruction table -- shared verbatim by avx_fma_rosetta_ref.c (the
// generator) and avx_fma_test.cpp (the differential).
//
// SVM_FMA(name, opcode, w, shape)
//   name    the assembler mnemonic, and the row key in the reference data
//   opcode  the 0F38 opcode byte; the whole family is VEX.66.0F38
//   w       VEX.W: 0 = single precision (ps/ss), 1 = double (pd/sd)
//   shape   P  packed: emitted at VEX.L = 0 and 1
//           S  scalar: VEX.LIG, so BOTH L values are emitted too -- an
//              implementation that routed L=1 to the packed path would then be
//              caught rather than untested
//
// The table says WHAT must be covered.  It does NOT say how each row is
// encoded: every reference row carries the literal instruction bytes the
// generator executed, and the test replays those, so a mistake in this table
// cannot make both sides agree on the wrong instruction.
//
// Opcode layout, which is why the handler's dispatch is arithmetic rather than
// a 60-entry table: high nibble 9/A/B selects 132/213/231, low nibble selects
// the operation (6 addsub, 7 subadd, 8/9 madd, A/B msub, C/D fnmadd, E/F
// fnmsub) with the low bit meaning "scalar".

// ---- 132: dst = op1 * op3 + op2 -------------------------------------------
SVM_FMA(vfmaddsub132ps, 0x96, 0, P)
SVM_FMA(vfmaddsub132pd, 0x96, 1, P)
SVM_FMA(vfmsubadd132ps, 0x97, 0, P)
SVM_FMA(vfmsubadd132pd, 0x97, 1, P)
SVM_FMA(vfmadd132ps, 0x98, 0, P)
SVM_FMA(vfmadd132pd, 0x98, 1, P)
SVM_FMA(vfmadd132ss, 0x99, 0, S)
SVM_FMA(vfmadd132sd, 0x99, 1, S)
SVM_FMA(vfmsub132ps, 0x9A, 0, P)
SVM_FMA(vfmsub132pd, 0x9A, 1, P)
SVM_FMA(vfmsub132ss, 0x9B, 0, S)
SVM_FMA(vfmsub132sd, 0x9B, 1, S)
SVM_FMA(vfnmadd132ps, 0x9C, 0, P)
SVM_FMA(vfnmadd132pd, 0x9C, 1, P)
SVM_FMA(vfnmadd132ss, 0x9D, 0, S)
SVM_FMA(vfnmadd132sd, 0x9D, 1, S)
SVM_FMA(vfnmsub132ps, 0x9E, 0, P)
SVM_FMA(vfnmsub132pd, 0x9E, 1, P)
SVM_FMA(vfnmsub132ss, 0x9F, 0, S)
SVM_FMA(vfnmsub132sd, 0x9F, 1, S)

// ---- 213: dst = op2 * op1 + op3 -------------------------------------------
SVM_FMA(vfmaddsub213ps, 0xA6, 0, P)
SVM_FMA(vfmaddsub213pd, 0xA6, 1, P)
SVM_FMA(vfmsubadd213ps, 0xA7, 0, P)
SVM_FMA(vfmsubadd213pd, 0xA7, 1, P)
SVM_FMA(vfmadd213ps, 0xA8, 0, P)
SVM_FMA(vfmadd213pd, 0xA8, 1, P)
SVM_FMA(vfmadd213ss, 0xA9, 0, S)
SVM_FMA(vfmadd213sd, 0xA9, 1, S)
SVM_FMA(vfmsub213ps, 0xAA, 0, P)
SVM_FMA(vfmsub213pd, 0xAA, 1, P)
SVM_FMA(vfmsub213ss, 0xAB, 0, S)
SVM_FMA(vfmsub213sd, 0xAB, 1, S)
SVM_FMA(vfnmadd213ps, 0xAC, 0, P)
SVM_FMA(vfnmadd213pd, 0xAC, 1, P)
SVM_FMA(vfnmadd213ss, 0xAD, 0, S)
SVM_FMA(vfnmadd213sd, 0xAD, 1, S)
SVM_FMA(vfnmsub213ps, 0xAE, 0, P)
SVM_FMA(vfnmsub213pd, 0xAE, 1, P)
SVM_FMA(vfnmsub213ss, 0xAF, 0, S)
SVM_FMA(vfnmsub213sd, 0xAF, 1, S)

// ---- 231: dst = op2 * op3 + op1 -------------------------------------------
SVM_FMA(vfmaddsub231ps, 0xB6, 0, P)
SVM_FMA(vfmaddsub231pd, 0xB6, 1, P)
SVM_FMA(vfmsubadd231ps, 0xB7, 0, P)
SVM_FMA(vfmsubadd231pd, 0xB7, 1, P)
SVM_FMA(vfmadd231ps, 0xB8, 0, P)
SVM_FMA(vfmadd231pd, 0xB8, 1, P)
SVM_FMA(vfmadd231ss, 0xB9, 0, S)
SVM_FMA(vfmadd231sd, 0xB9, 1, S)
SVM_FMA(vfmsub231ps, 0xBA, 0, P)
SVM_FMA(vfmsub231pd, 0xBA, 1, P)
SVM_FMA(vfmsub231ss, 0xBB, 0, S)
SVM_FMA(vfmsub231sd, 0xBB, 1, S)
SVM_FMA(vfnmadd231ps, 0xBC, 0, P)
SVM_FMA(vfnmadd231pd, 0xBC, 1, P)
SVM_FMA(vfnmadd231ss, 0xBD, 0, S)
SVM_FMA(vfnmadd231sd, 0xBD, 1, S)
SVM_FMA(vfnmsub231ps, 0xBE, 0, P)
SVM_FMA(vfnmsub231pd, 0xBE, 1, P)
SVM_FMA(vfnmsub231ss, 0xBF, 0, S)
SVM_FMA(vfnmsub231sd, 0xBF, 1, S)
