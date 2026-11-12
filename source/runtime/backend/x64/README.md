# x86-64 backend

This backend consumes the current uniform IR directly. `compile.cpp` chooses
`AllocateBlock` or `AllocateFunction`, then emits the original operations through
`Handle(Op<...>, OpContext&)`. The function allocator uses the shared CFG order and
loop liveness; there are no target actions, plans, or a second backend IR.

`allocate.*` owns scalar/XMM homes and actual spill offsets. Scalar SSA uses
R13–R15, RBP, R8 and R9; V128 SSA uses XMM2–XMM13. F32/F64 values retain their raw
bits in scalar homes. R12 holds the uniform pointer, RBX holds the portable base of
flags, and the remaining GPRs plus XMM0/1/14/15 are explicit scratch registers.
Guest registers use uniform loads/stores. Linked execution optionally pins the
first two uniform GPR slots in R13/R14 (default zero), removing those registers
from the SSA bank. Helpers publish/reload the pins and fault recovery restores
them; no vector pin bank is present.

`emit_operand.cpp` reads homes and flat operand expressions. Scalar, memory,
flags, control, integer-vector, float, crypto and atomic handlers live in their
respective `emit_*.cpp` files. SSE2 is the baseline; runtime CPUID enables optional
SSSE3/SSE4, AES/PCLMUL/SHA forms. Widths without a matching instruction use actual
integer lane code. Complex numerical operations use specific numerical helpers;
no generated unit invokes the IR interpreter.

Linked VM units retain NZCV in EFLAGS or an eight-byte raw RFLAGS frame slot.
RBX retains the portable base, including PF/AF/DF and reserved bits. Producers
record their carry polarity, and partial NZCV writes merge into a canonical
snapshot. Jcc, SETcc and scalar CMOVcc consume native conditions. Named snapshots,
opaque helpers and architectural observers materialize the portable carrier.
The flags live-in analysis uses bounded four-bit CFG propagation. Fault recipes
identify packed, published, native or frame-saved sources at the actual access.
Overlapping uniform accesses publish the current flags first. Partial writes
reload the complete portable carrier, preserving every untouched byte.

ARM scalar/NEON ADD/SUB/MUL and indexed multiply use `ArmFPBinary`, whose explicit
FPCR/FPSR offsets preserve the ARM state contract. All 32 supported FPCR profiles
have stable domain keys and compile on demand. External specialized entries guard
FPCR. Units with explicit control writes or opaque helpers revalidate it when a
region begins. Zero or central finite operands execute with SSE2 under the selected
rounding mode. Every active lane is qualified before execution, and other inputs
use exact ARM mathematical leaves. Inactive lanes are zero and cannot contribute
exceptions. Straight-line regions install MXCSR once, accumulate IXC, and commit
before state observers, faults, helpers, events, guest flags producers and control
edges.

FP qualification failures branch to the unit's deferred cold area. Call sites
snapshot their live-register masks and share a helper stub by numerical mode,
state offsets and save policy. The local CALL/RET stack bias preserves the SysV
alignment and the original frame slots. Guest memory accesses use resident
addresses, source homes and destination homes directly. Destructive integer and
SSE instructions can reuse a first input at its last use, preserving the original
input home when the result is subsequently spilled.

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
Native static links and ready dispatch exchange a canonical raw snapshot in the
context. Cold dispatch publishes complete portable flags. OS signal/ucontext
handling remains in `platform/linux/x64_host.cpp`.

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
