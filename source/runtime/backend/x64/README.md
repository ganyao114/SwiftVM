# x86-64 backend

This backend consumes the current uniform IR directly. `compile.cpp` chooses
`AllocateBlock` or `AllocateFunction`, then emits the original operations through
`Handle(Op<...>, OpContext&)`. The function allocator uses the shared CFG order and
loop liveness; there are no target actions, plans, or a second backend IR.

`allocate.*` owns scalar/XMM homes and actual spill offsets. Scalar SSA uses
R13–R15, RBP, R8 and R9; V128 SSA uses XMM2–XMM13. F32/F64 values retain their raw
bits in scalar homes. R12 holds the uniform pointer, RBX holds portable packed
flags, and the remaining GPRs plus XMM0/1/14/15 are explicit scratch registers.
Guest registers remain uniform loads/stores; this initial ABI has no guest GPR or
vector pin bank.

`emit_operand.cpp` reads homes and flat operand expressions. Scalar, memory,
flags, control, integer-vector, float, crypto and atomic handlers live in their
respective `emit_*.cpp` files. SSE2 is the baseline; runtime CPUID enables optional
SSSE3/SSE4, AES/PCLMUL/SHA forms. Widths without a matching instruction use actual
integer lane code. Complex numerical operations use specific numerical helpers;
no generated unit invokes the IR interpreter.

`emit_calls.cpp` owns host-call staging and the live caller-saved SSA boundary.
Only actual live R8/R9 and XMM homes are saved. The 320-byte helper area contains
192 bytes for twelve XMM homes, sixteen bytes for R8/R9, and numerical helper
operands/results. Helpers run with the caller's MXCSR; emitted arithmetic runs
with the canonical IR numeric control word, masked traps and private sticky
status. Standalone return restores the exact original caller MXCSR.

Xbyak is a pinned, licensed dependency with one controlled storage extension
(described in its `UPSTREAM`): the byte sink is `CompileResult.words`. No private
executable buffer or final code copy is created. The uint32 vector is a byte
carrier; only final padding is four-byte aligned. Fault, entry, link and
relocation offsets always count bytes, including unaligned MOVABS immediates.

`entry_elf.S` owns the Linux/ELF SysV root ABI and directory thunks. A root saves
callee state and MXCSR once; linked units reserve only their spill frame, release
it before a tail exit, and use the same runtime PC directory. Function external
entries have explicit frame wrappers. `runtime.*` binds the configurable guest
uniform layout; x86 and ARM guests use the same backend numeric-control contract.
OS signal/ucontext handling remains in `platform/linux/x64_host.cpp`.

`patch.*` updates aligned target/fallback pointer slots rather than instructions.
`relocate.*` patches only declared semantic MOVABS sites before publication.
`helpers.*` supplies checked wide division, x87 metadata, exact rounding/FMA and
CAS128 fallback. Native CMPXCHG16B uses no helper; its fault recipe records that
packed flags were published before borrowing RBX. A linked libatomic fallback
must pass the VM's existing access qualification before entering any lock.

Legacy local/Phi storage, host-register operands, old RSB calls and unused legacy
memory-copy/extend opcodes are explicitly rejected. Current frontends lower to the
uniform/SSA contract; unsupported legacy shapes are never treated as successful
native compilation.
