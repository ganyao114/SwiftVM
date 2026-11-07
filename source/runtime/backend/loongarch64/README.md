# LA64 native backend

This backend consumes the current immutable `ir::Program`. It shares only the
mechanical CFG schedule/loop closure with other backends. Block and Function
allocation remain separate entry points; one fixed home belongs to each SSA
value, with eight callee-saved GPR homes (r23–r30), a consecutive pair for V128,
and aligned spill/auxiliary slots when needed. r22 is the Uniform base and r31
is the packed flags carrier. No interpreter value table or old lowering plan
exists.

`assembler` adapts the vendored Xbyak exact encoders to the sole output words
vector. It never allocates scratch registers, literal pools or executable memory.
Integer and memory temporaries are explicit; FP conversion/classification takes
its temporary FPR explicitly and comparisons name their FCC. The baseline emits
LA64 integer plus scalar F/D instructions. SIMD uses native scalar lane code;
crypto/x87 and wide numerical operations use normal typed C ABI helpers. LSX,
LASX and LBT are not required or advertised by this baseline. `host_features.h`
retains the kernel HWCAP decoder for future explicitly selected acceleration.

`emit_scalar`, `emit_flags`, `emit_operand`, `emit_control` and `emit_memory` own
those instruction families; `emit_vector` and `emit_float` own pair/lane emission;
`emit_atomic` owns aligned AMO/LL-SC and min/max selection. `emit_calls` stages the
real helper ABI. It never enters an IR interpreter. Misaligned atomic accesses
exit before accessing memory, while ordinary unaligned loads/stores use bytes.
CAS128 uses the platform atomic implementation, with the existing runtime backing
qualification before entering that implementation.

`runtime` binds byte offsets once from `GuestLayout`. Numeric controls use the
canonical IR control word; the backend knows no guest register layout or FPCR
bit encoding. `entry_elf.S` owns host callee-save/FP state and dispatch publication.
`patch` changes aligned link data, and `relocate` rebinds actual local address
literals through stable semantic HostSymbols. Default JIT keeps compact LI forms
and allocates no relocation records.

The exact instruction and FP-control definitions follow the
[LoongArch volume 1 manual](https://loongson.github.io/LoongArch-Documentation/LoongArch-Vol1-EN.html).
The encoder's Apache-2.0 license and upstream notices remain in
`runtime/externals/xbyak_loongarch64`.
