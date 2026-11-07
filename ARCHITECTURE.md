# Compilation and execution

The build links the rebuilt pipeline directly. It does not invoke the retired
HIR, allocation plans, actions, certificates or legacy JIT services.

```text
x86 bytes -> decode / region formation -> neutral Uniform IR
          -> in-place common optimization
          -> Block or Function allocation -> direct ARM64 / RV64 encoding
          -> executable slabs / links / one active PC directory -> native execution
```

| Location | Responsibility |
| --- | --- |
| `source/runtime/ir/` | Sole Program/Builder owner, stable node IDs, 24B operations, 24B blocks and 8B operand words; O(1) Submit |
| `source/runtime/frontend/x86/` | Owned decoded instructions, bounded region formation and shared Semantics for IR emission or direct interpretation; Known-target selection with Unknown/EarlyOff paths |
| `source/runtime/ir/passes/` | Three walks over the same IR, flags demand, bounded rewrites, Uniform forwarding and DCE; one temporary u64/node table |
| `source/base/` | Typed formatting, error delivery and filtered logging; no per-node compile state |
| `source/runtime/ir/format.*` | Optional streaming IR diagnostics, separate from core storage |
| `source/runtime/backend/allocation_cfg.*` | Shared schedule/backedge mathematics, no node facts table |
| `source/runtime/backend/arm64/allocate.*`, `allocation_selection.cpp` | Distinct Block/Function lifetimes, pin/NZCV/PC choices in the original reverse walk, real registers/spills and one 16B Home/node |
| `source/runtime/backend/arm64/` | Typed Handle selection, exact controlled VIXL encoding, explicit scratch allocation, patching, helper ABI and actual fault/link metadata |
| `source/runtime/backend/riscv64/` | Independent RV64G allocation and Handle emission into the same word buffer, exact Biscuit encoding and explicit helper calls |
| `source/runtime/arch/x86/` | Sole guest state/flags definition, CPU model and portable x87/vector numerical semantics |
| `source/runtime/execution/` | VM/thread ownership, engine dispatch, compile service, guest memory, native mappings, slabs, directory/links, SMC and retirement |
| `source/runtime/execution/code_image.*`, `disk_cache.*`, `code_persistence.cpp` | Shared native unit format, optional persistent data lookup and VM source/SMC policy; published code stays owned by CodeHeap/CodeCache |
| `source/aot/` | Embedded-guest ELF container, offline candidate discovery, named SysV guest calls and explicit startup; uses ordinary compilation/publication |
| `source/runtime/platform/` | ELF loading, Linux syscalls/signals/safe access and host ARM64 bridge |
| `source/launcher/linux/` | Runner CLI and coarse diagnostics |
| `source/tests/` | Catch cases, independent numerical expectations, and explicit compile/native execution entries |
| `benchmarks/` | Fixed construction/compile workloads, budget diagnostics and guest input recipes |

GPR/XMM architecture accesses retain neutral LoadUniform/StoreUniform semantics.
The frontend owns logical versions; the backend owns pins and physical choices.
Block remains a lightweight allocation path. Function computes real RPO and
cross-block/loop lifetimes; failures and retries remain charged.

The frontend's 64B Instruction is independent of the legacy parser. One
Semantics<Sink> implements instruction ordering for both IrSink and DirectSink;
the interpreter creates no Program. VEX YMM operations use two V128 values and
Uniform accesses to their low and high halves. Backend dispatch selects typed
Handle methods whose implementations are grouped by responsibility. Its sole
Assembler borrows the output words and uses a controlled VIXL subset without
implicit scratch registers, literal pools, veneers or automatic extra code.

Packed SSE operations reuse the existing vector IR and direct encoders.
Restartable REP STOS emits a count guard, one TSO store and progress publication
after that store succeeds. Its forward branch uses fixups only for actual
edges; the join clears conditional PC/flags facts. It adds no analysis pass or
second representation of the program.

Software DAZ/FTZ controls are invariant within a native unit. Runtime root
boundaries select one of four lazily populated mode directories, so known
controls reach the encoder without per-operation reads. Unknown callers keep
the dynamic path. Each actual dynamic exit can own a fixed sixteen-entry
full-PC/entry array in its native DATA. The existing relocation owner resets
all entries to that site's callable directory fallback before retirement;
all mode variants participate in SMC invalidation. RW/RX aliases occupy one
reserved range and retire with one unmap.

Persistence is off by default. Opt-in emitters record semantic host-symbol
relocations at the actual address slots, without scanning machine code or
ordinary guest constants. AOT and disk cache save canonical words and the same
fault/link/entry records. Load binds the current process's addresses before
normal publication; source spans and source policy are checked before reuse.
An SMC event disables persistent reads/writes for the current guest lifetime.
See the [AOT/cache contract](docs/aot-code-cache-2026-11-07.md) for format,
compatibility, ownership and the current static ET_EXEC boundary.

CompileService consumes and resets the previous Program::Storage, retaining its
three vector capacities and the optimizer's one u64/node work array. Arm64Runtime
retains the actual Allocation homes/order and one CompileResult output. A
single-block Function with only an external exit can omit CFG construction;
internal edges and self-loops retain the Function graph algorithm. New IR
requests still mint full unit identities and immutable public metadata; native
fault/link owners borrow neither scratch buffer.
Successful retention is bounded and final release remains charged. Publish
can resolve ready static targets before its final cache sync, while the same
relocation rows retain unlink ownership. Single mappings keep cold transfers.

Numerical FP operations read the active FP environment without observing
arbitrary Uniform state or integer flags, or producing sticky exceptions/traps.
Their read-only effects and actual read-before-write rules allow safe pin
reuse. A Function conditional with two external PCs can use the existing
conditional Uniform store directly when the target accepts it; real internal
edges and unknown-target CFGs keep their ordinary representation.

The native ABI pins sixteen guest GPRs and sixteen XMMs. Supported hosts retain
hardware NZCV, raw PF in x1 and AF input xor in W29; packed and target-unknown
paths remain. Exact synchronous fault PCs/state, TSO, W^X, SMC unlinking and
safe retirement remain part of the implemented contracts.

Private immutable Function regions can publish complete external entries for
their nonempty source blocks. A neutral EntryPoint declaration carries the guest
PC; the backend emits any required frame/PC prologue and records its native
offset. Internal edges skip that prologue. Each callable PC uses the existing
directory and shares the unit's single NativeCode range owner; fault offsets and
the execution anchor retain the whole-unit base. Registration keeps the first
ready entry and resolves pending links independently for each PC and FP domain.
Formation stops at entries already ready in that domain. CALL remains a region
exit, with its continuation formed on the first actual request. Unknown targets,
Block scope and mutable-code boundaries retain their existing paths. Empty
budget frontiers and synthetic exits never become callable self-dispatch entries.

The x87 implementation lives in `arch/x86/x87*.cpp`, with true 80-bit values,
TOP/tags, control/status and FIP/FDP/FOP in the sole 864B GuestState. The selected
SoftFloat and Cephes dependencies use explicit local arithmetic state, including
rounding direction from the actual roundPack. On ARM64 this numerical closure is
built with general-regs-only and without instrumentation callouts. Its 144B
bridge owns GPR/NZCV preservation; vector registers and FP state stay untouched.
Same-block exact command windows and two immutable builder cursors reduce calls
without a second stack model. X87Execute distinguishes prior pending exceptions
from newly raised exceptions; metadata commits at the actual instruction's
successful boundary. Guest-memory accesses stay in ordinary IR; x87 memory
stores prepare their result and commit FSW/TOP/tags only after the store succeeds.
Unmasked exceptions stop at the runtime service boundary; full guest exception
delivery remains outside this checkpoint. FX/XSAVE state images use the shared
execution state service, including compacted XSAVEC and checked XRSTOR commits.

Batch37's adjacent consumer selections retain the original IR/Home and use one
elided-consumer ID. Signed loads, sole byte publications and unflagged U32
multiply/add have bounded eligibility and ordinary fallbacks. They add no IR,
node table or pass.

The current static-ELF subset does not cover every feature of the retired VM.
EVEX/AVX-512, complete PKU and guest kernel/compatibility modes remain deferred.
See the [runtime architecture](docs/runtime-architecture-plan-2026-11-05.md),
[runtime implementation](docs/runtime-architecture-implementation-2026-11-05.md),
[instruction coverage](docs/instruction-coverage-implementation-2026-11-05.md),
[latest compile results](docs/compile-cost-optimization-2026-11-06.md),
[historical post-push benchmark anchor](docs/post-push-benchmark-2026-11-05.md),
[Function entry reuse results](docs/function-entry-reuse-2026-11-05.md),
[owner/publication results](docs/compile-owner-and-publication-optimization-2026-11-05.md),
[FP and compile analysis](docs/fp-instruction-and-compile-cost-2026-11-05.md),
[optimization checkpoint](docs/parity-gap-optimization-2026-11-05.md),
[earlier checkpoint](docs/codegen-parity-checkpoint-2026-11-04.md),
[framework contract](docs/compile-framework-contract-2026-11-02.md) and
[implementation history](docs/codegen-parity-progress-2026-11-03.md).
