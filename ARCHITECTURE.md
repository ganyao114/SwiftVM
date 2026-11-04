# Compilation and execution

The build links the rebuilt pipeline directly. It does not invoke the retired
HIR, allocation plans, actions, certificates or legacy JIT services.

```text
x86 bytes -> decode / region formation -> neutral Uniform IR
          -> in-place common optimization
          -> Block or Function allocation -> direct ARM64 encoding
          -> executable slabs / links / one PC directory -> native execution
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

The native ABI pins sixteen guest GPRs and sixteen XMMs. Supported hosts retain
hardware NZCV, raw PF in x1 and AF input xor in W29; packed and target-unknown
paths remain. Exact synchronous fault PCs/state, TSO, W^X, SMC unlinking and
safe retirement remain part of the implemented contracts.

Batch37's adjacent consumer selections retain the original IR/Home and use one
elided-consumer ID. Signed loads, sole byte publications and unflagged U32
multiply/add have bounded eligibility and ordinary fallbacks. They add no IR,
node table or pass.

The current static-ELF subset does not cover every feature of the retired VM.
See the [checkpoint](docs/codegen-parity-checkpoint-2026-11-04.md),
[framework contract](docs/compile-framework-contract-2026-11-02.md) and
[implementation history](docs/codegen-parity-progress-2026-11-03.md).
