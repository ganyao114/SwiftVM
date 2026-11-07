# SwiftVM

This branch replaces the former middle pipeline with compact Uniform IR,
in-place optimization, separate Block/Function allocation and native ARM64,
RV64, x86-64 and LoongArch64 encoding. The runtime executes implemented x86-64
and AArch64 subsets from static Linux ELFs, with one guest thread. Hosts are
ARM64 Linux/Android and RV64, x86-64 or LoongArch64 Linux.

The former Android demo and Slang targets remain retired on this branch.
The RV64 backend now consumes the rebuilt IR directly; the former backend's
IR/HIR and persistence implementation remain in Git history and the frozen tree.
The current pipeline provides native AOT artifacts and an optional disk cache
using the same compilation, relocation and publication contracts as JIT.
Dynamic ELF, full ISA coverage and complete guest signals remain outside this
checkpoint.

## Build

Host contracts and code-generation tools require CMake 3.22+, a C++20 compiler
and a C compiler. Ninja is used below; diStorm, the controlled exact-encoding
VIXL subset, Biscuit, controlled Xbyak/Xbyak-LoongArch encoders, SoftFloat,
Cephes and fmt are included.
Catch2 is vendored separately for tests; `-DBUILD_TESTING=OFF` excludes the
test framework. `SVM_BUILD_IR_FORMAT` controls the optional IR text formatter
and defaults to the test setting.

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j4
ctest --test-dir build/host --output-on-failure
```

On the four supported Linux hosts this also builds `build/host/svm_rebuild_linux`. Native
checks require the matching host ISA; compiling RV words on ARM64 does not
execute them. CTest registers code-generation and native execution separately;
unsupported native entries are marked skipped (exit 77). When invoking a test
binary directly, select `--native` to execute its native cases; the default
runs construction/encoding contracts. Catch case names and tags can filter a
specific scenario. See the [RV64 backend](source/runtime/backend/riscv64/README.md)
for cross-building, execution coverage and current boundaries. The
[x64](source/runtime/backend/x64/README.md) and
[LoongArch64](source/runtime/backend/loongarch64/README.md) backends describe
their independent allocation, native ABI, encoding and numerical helper boundaries.

For Android, set `ANDROID_NDK` to the NDK installation. The measured build used
NDK 29.0.13846066, API28, Release/O3, static libc++ and ThinLTO.
`SVM_ENABLE_IPO` defaults to ON for supported Release/RelWithDebInfo builds;
the x87/Cephes numerical closure keeps its separate GPR-only compilation contract.

```sh
cmake -S . -B build/android -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
  -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release
cmake --build build/android --target \
  svm_rebuild_linux svm_arm64_compile_test svm_compile_pipeline_test -j4
```

Push the runner and a static guest ELF to the device, then select either scope:

```sh
./svm_rebuild_linux --scope block guest.elf [guest args...]
./svm_rebuild_linux --scope function guest.elf [guest args...]
./svm_rebuild_linux --engine interpreter guest.elf [guest args...]
./svm_rebuild_linux --engine mixed --hot-threshold 8 --scope function guest.elf [guest args...]
```

The default engine is JIT: each requested entry is compiled immediately.
Cold interpretation and hot promotion require explicit `--engine mixed`.
Both consumers use the same decoded instruction
contract and frontend semantics: the IR builder creates a Program, while the
independent IR direct interpreter executes the submitted operations immediately.
Mixed mode interprets the first N visits to a cold entry, then attempts JIT
compilation on the next visit (`--hot-threshold N`, default 8; 0 compiles
immediately). Ready entries run natively; cold exits return to interpretation.
Block and Function remain separate allocation scopes. An unsupported native
entry keeps interpreting until source invalidation permits a fresh attempt.

`--decode-timing` enables intrusive compile diagnostics and counted dispatch;
leave it off for ordinary runtime measurements. Requested Function cost includes
failed formation and actual Block retries.

## AOT and disk cache

Linux/Android also build `svm_aot`. Persistence accepts static x86-64 or AArch64
`ET_EXEC` guests; ordinary JIT retains its existing static PIE support.

```sh
./svm_rebuild_linux --cache-dir ./private-cache guest.elf
./svm_rebuild_linux --cache-dir ./private-cache guest.elf
./svm_aot compile guest.elf -o guest.aot --scope function
./svm_aot info guest.aot
./svm_aot run --aot guest.aot --scope function
./svm_aot call guest.aot function_name 1 2
```

The cache directory is created with mode `0700`. AOT carries the complete guest
ELF and runs after the original file is removed. Uncovered targets use the
ordinary JIT. Emission options such as scope, optimization and decode diagnostics
must match the artifact; incompatible explicit artifacts fail before execution.
For x86-64 guests, the C++ `AotGuestEnv` / `GuestFn` interface supports named calls, IFUNC resolution,
FP, aggregates and typed guest pointers. See the
[format, ownership and API documentation](docs/aot-code-cache-2026-11-07.md).

## Formatting

Use clang-format 21 with the repository style:

```sh
python3 scripts/format_runtime.py
python3 scripts/format_runtime.py --check
```

Set `CLANG_FORMAT` or pass `--clang-format` to select the executable. The helper
covers active C++ sources, tests and benchmarks, preserves include order and
macro tables, and leaves assembly and vendored formatting intact.
The [submission validation](docs/submission-validation-2026-11-06.md) records
the formatted-source identity and host/Android regression results.

## Current checkpoint

The [backend maintenance record](docs/backend-maintenance-2026-11-07.md) describes
the allocator/ABI/logging/test cleanup and its compile regression checks. It does
not replace the full FEX comparison below.

The runtime now separates guest architecture, execution ownership and platform
services. ELF entry assembly and private helper bridges live with each backend;
Linux host adapters own capability probing, signal-frame views and OS memory/cache
operations. Logging/errors and optional IR formatting use a small shared base
layer, independently of code-generation storage.

It supports JIT, direct interpretation and mixed execution, shared
x86 and AArch64 semantics, explicit code mappings/cache/link/SMC ownership, and expanded
user-mode scalar, SSE, AVX/AVX2, MMX and x87 forms. EVEX/AVX-512 and complete PKU
remain deferred. Source inventory is not full ISA/form coverage; see the
[coverage implementation and boundaries](docs/instruction-coverage-implementation-2026-11-05.md).

The AArch64 frontend ports the implemented `origin/master` visitor semantics
(reference `42e9c234`) onto the current uniform IR. Its 16-byte instruction
record stores a word, PC and selected decoder family; IR construction and direct
interpretation share `Semantics<Sink>`. Scalar/SP/ZR/NZCV, memory/control,
NEON/crypto and FP families are implemented; precise FP/QC operations use pure
architectural numerical helpers through declared relocations. Exclusive
LDXR/STXR reservation semantics, full ARMv8 extension coverage, complete guest
signals and AAPCS64 typed host-to-guest calls remain outside this checkpoint.
The LoongArch64 baseline requires LA64D; SIMD uses native scalar lanes and does
not require or advertise LSX/LASX/LBT.

ARM system-register coverage is NZCV, TPIDR_EL0/TPIDRRO_EL0, FPCR/FPSR and
DCZID_EL0. TPIDR variants retain the old shared storage model. DMB/DSB issue
barriers and ISB ends the block; guest DC/IC cache-maintenance instructions,
CLREX, counters, SVE/SME/MTE and authenticated branches are unsupported. Hints
retain the old no-op behavior. FPCR implements RMode/FZ/FZ16/DN; unsupported
controls are rejected at the VM's FPCR service boundary, and FPSR sticky/QC does not imply guest
FP exception delivery. BRK/HLT are rejected rather than exposed as host services.

The [architecture restoration checks](docs/evidence/multiarch-20261107/summary.json)
record physical RedMagic ARM64 tests, native x64/LoongArch64/RV64 execution
under real Linux kernels in QEMU, cache/AOT/fault/SMC checks and the compile
regression guard. The new host backends have functional validation; their
physical hardware performance has not been measured.

The latest full FEX compile comparison uses RedMagic NX729J CPU4 and fixed FEX
`a156752f`, with five fresh launches per arm/input and 200 total observations.
Only actual byte-parser calls are removed. Formation, IR construction,
optimization, allocation, encoding, failures/retries, publication and release
remain charged. Values below are medians in ms; ratios compare those medians.

| Workload | Block | Requested Function | Block / FEX | Function / FEX |
| --- | ---: | ---: | ---: | ---: |
| CoreMark | 41.9926 | 51.1897 | 0.88x | 1.07x |
| Integer | 1.8700 | 1.8465 | 1.20x | 1.18x |
| FP | 2.4207 | 2.4667 | 1.45x | 1.48x |
| Memory | 2.2868 | 2.2170 | 1.43x | 1.38x |
| Branch | 1.9250 | 1.9314 | 1.27x | 1.28x |
| Call | 1.8440 | 2.1307 | 1.13x | 1.30x |
| Function tests | 21.1883 | 33.1372 | 0.71x | 1.11x |
| x87 | 0.6425 | 0.5619 | 2.25x | 1.97x |

The strict 2x compile goal is still open for x87 Block. The latest changes reduce
CoreMark Block/Function compile by 9.4%/2.7% against the preceding coverage build;
Call Function increases 5.5%. See the [complete results, identities and remaining
costs](docs/compile-cost-optimization-2026-11-06.md). This round contains no new
full runtime/PMU comparison; the historical results below retain their original
source and binary identities and do not qualify the expanded instruction set.

See the [runtime design](docs/runtime-architecture-plan-2026-11-05.md),
[runtime implementation](docs/runtime-architecture-implementation-2026-11-05.md),
[architecture](ARCHITECTURE.md),
[historical post-push five-round benchmark anchor](docs/post-push-benchmark-2026-11-05.md),
[Function entry reuse and construction audit](docs/function-entry-reuse-2026-11-05.md),
[buffer reuse and publication](docs/compile-owner-and-publication-optimization-2026-11-05.md),
[FP optimization and compile cost analysis](docs/fp-instruction-and-compile-cost-2026-11-05.md),
[previous optimization results](docs/parity-gap-optimization-2026-11-05.md),
[earlier coverage completion](docs/benchmark-completion-2026-11-05.md),
[earlier Batch37 checkpoint](docs/codegen-parity-checkpoint-2026-11-04.md),
[codegen design](docs/codegen-parity-plan-2026-11-03.md),
[implementation history](docs/codegen-parity-progress-2026-11-03.md) and
[benchmarks](benchmarks/README.md).

Older `tools/` harnesses and historical documents retain their original
interfaces and results. Use the CMake targets above for this branch.
