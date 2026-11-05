# Compilation and execution

The build links the rebuilt pipeline directly. It does not invoke the retired
HIR, allocation plans, actions, certificates or legacy JIT services.

```text
x86 bytes -> decode / region formation -> neutral Uniform IR
          -> in-place common optimization
          -> Block or Function allocation -> direct ARM64 encoding
          -> executable slabs / links / one active PC directory -> native execution
```

| Location | Responsibility |
| --- | --- |
| `source/runtime/ir/` | Sole Program/Builder owner, stable node IDs, 24B operations, 24B blocks and 8B operand words; O(1) Submit |
| `source/runtime/frontend/x86/` | Decode buffer, bounded region formation and neutral IR emission; Known-target selection with Unknown/EarlyOff paths |
| `source/runtime/compile/` | Three walks over the same IR, flags demand, bounded rewrites, Uniform forwarding and DCE; one temporary u64/node table |
| `source/runtime/backend/arm64/allocate.*` | Distinct Block/Function lifetimes, pin affinity, real registers/spills and one 16B Home/node |
| `source/runtime/backend/arm64/compile.*`, `emit*`, `helper_calls.cpp` | Direct selection/encoding, raw PF/AF/native NZCV, helper ABI and actual fault/link metadata |
| `source/runtime/execution/` | Guest state/mappings, static ELF, code publication, directory/links, fault recovery, syscalls and retirement |
| `source/launcher/linux/` | Runner CLI and coarse diagnostics |
| `source/tests/rebuild/` | Construction, optimizer, allocation/encoding and native pipeline contracts |
| `benchmarks/` | Fixed construction/compile workloads, budget diagnostics and guest input recipes |

GPR/XMM architecture accesses retain neutral LoadUniform/StoreUniform semantics.
The frontend owns logical versions; the backend owns pins and physical choices.
Block remains a lightweight allocation path. Function computes real RPO and
cross-block/loop lifetimes; failures and retries remain charged.

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

Process reuses the previous Program's three vector capacities and one actual
CompileResult output. New IR requests still mint full unit identities and
immutable metadata; native fault/link owners borrow neither scratch buffer.
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

The implemented x87 subset uses the existing opaque `X87Op` contract, a tail
state in `GuestState` and `execution/x87.cpp`. The helper retains true 80-bit
values, TOP/tags, control and status; its small vendored SoftFloat subset uses
an explicit local environment, including rounding direction from the actual
roundPack. On ARM64 this private helper closure is built with general-regs-only
and without instrumentation callouts. Its 144B bridge owns GPR/NZCV preservation;
vector registers and FP state stay untouched. Same-block exact command windows
and two immutable builder cursors reduce calls without a second stack model.
Guest-memory accesses stay in ordinary IR so
loads can fault before the x87 stack changes. Unmasked x87 exceptions stop at
the runtime service boundary; full guest exception delivery and transactional
x87 memory stores are outside this subset.

Batch37's adjacent consumer selections retain the original IR/Home and use one
elided-consumer ID. Signed loads, sole byte publications and unflagged U32
multiply/add have bounded eligibility and ordinary fallbacks. They add no IR,
node table or pass.

The current static-ELF subset does not cover every feature of the retired VM.
See the [Function entry reuse results](docs/function-entry-reuse-2026-11-05.md),
[owner/publication results](docs/compile-owner-and-publication-optimization-2026-11-05.md),
[FP and compile analysis](docs/fp-instruction-and-compile-cost-2026-11-05.md),
[optimization checkpoint](docs/parity-gap-optimization-2026-11-05.md),
[earlier checkpoint](docs/codegen-parity-checkpoint-2026-11-04.md),
[framework contract](docs/compile-framework-contract-2026-11-02.md) and
[implementation history](docs/codegen-parity-progress-2026-11-03.md).
