# SwiftVM

This branch replaces the former middle pipeline with compact Uniform IR,
in-place optimization, separate Block/Function allocation and direct ARM64
encoding. The runtime executes an implemented x86-64 subset from static Linux
ELFs on ARM64 Linux or Android, with one guest thread.

The former Android demo, Slang/AOT and RISC-V targets are retired on this branch;
their implementations remain in Git history and the frozen original workspace.
Dynamic ELF, full ISA coverage and complete guest signals remain outside this
checkpoint.

## Build

Host contracts and code-generation tools require CMake 3.22+, a C++20 compiler
and a C compiler. Ninja is used below; diStorm, the controlled exact-encoding
VIXL subset, SoftFloat and Cephes numerical dependencies are included.

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j4
ctest --test-dir build/host --output-on-failure
```

On ARM64 Linux this also builds `build/host/svm_rebuild_linux`. Native checks
depend on the host's ARM64/Linux facilities; host-only results do not replace
Android runtime qualification.

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

The runtime now separates guest architecture, execution ownership and platform
services. It supports JIT, direct interpretation and mixed execution, shared
x86 semantics, explicit code mappings/cache/link/SMC ownership, and expanded
user-mode scalar, SSE, AVX/AVX2, MMX and x87 forms. EVEX/AVX-512 and complete PKU
remain deferred. Source inventory is not full ISA/form coverage; see the
[coverage implementation and boundaries](docs/instruction-coverage-implementation-2026-11-05.md).

The latest compile measurement uses RedMagic NX729J CPU4 and fixed FEX
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
