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
and a C compiler. Ninja is used below; diStorm and the required SoftFloat
ext80 subset are included.

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j4
ctest --test-dir build/host --output-on-failure
```

On ARM64 Linux this also builds `build/host/svm_rebuild_linux`. Native checks
depend on the host's ARM64/Linux facilities; host-only results do not replace
Android runtime qualification.

For Android, set `ANDROID_NDK` to the NDK installation. The measured build used
NDK 29.0.13846066, API28, Release/O3, static libc++ and no LTO.

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
```

`--decode-timing` enables intrusive compile diagnostics and counted dispatch;
leave it off for ordinary runtime measurements. Requested Function cost includes
failed formation and actual Block retries.

## Measured checkpoint

The November 5 UTC optimization checkpoint runs all eight fixed workloads in
both SVM scopes. The original Bionic/CoreMark ELF uses 100000 iterations on
RedMagic NX729J CPU4 against fixed FEX `a156752f`. All CoreMark runs exceeded
10 seconds and validated CRC `0xd340`; each table entry is a median of three.

| Metric | Block | Requested Function | FEX |
| --- | ---: | ---: | ---: |
| Runtime | 16.111 s | 15.633 s | 15.033 s |
| Runtime / FEX | 1.072x | 1.040x | 1x |
| User cycles / FEX | 1.070x | 1.039x | 1x |
| User instructions / FEX | 1.038x | 1.010x | 1x |
| Cold compile excluding byte parser | 44.906 ms | 51.447 ms | 48.025 ms |
| Compile / FEX | 0.935x | 1.071x | 1x |

Compile includes formation, IR construction, optimization, allocation, encoding,
failures/retries, publication and release; only byte-parser calls are removed.
All eight fixed inputs meet the 1.10x runtime/user-cycle limits in both scopes.
Fifteen of sixteen compile comparisons meet 2x: the original x87 Block median
is 2.109x. An independently planned seven-round paired check gives 1.966x;
it does not replace the original samples or establish stable compliance.
The original suite therefore retains 47/48 passing limits.
Function entry reuse reduced CoreMark repeated construction from 15,889 to
191 instruction occurrences and compile from 81.982 to 51.447 ms. Its runtime
increased from 15.282 to 15.633 s, so this is not a zero-cost quality change.
FP runtime is 0.955/0.947x FEX and FP instruction counts are 0.990/1.006x;
x87 runtime is 0.678/0.675x. CoreMark spill
slots are 25/31 and maximum frames 48/64 bytes for Block/Function.
Instruction counts still differ. The x87 guest has an exit-only oracle, with
separate finite precision/state contracts; this checkpoint covers these fixed
inputs rather than the entire x86 ISA or general application performance.

See the [Function entry reuse and construction audit](docs/function-entry-reuse-2026-11-05.md),
[buffer reuse and publication](docs/compile-owner-and-publication-optimization-2026-11-05.md),
[FP optimization and compile cost analysis](docs/fp-instruction-and-compile-cost-2026-11-05.md),
[previous optimization results](docs/parity-gap-optimization-2026-11-05.md),
[earlier coverage completion](docs/benchmark-completion-2026-11-05.md),
[earlier Batch37 checkpoint](docs/codegen-parity-checkpoint-2026-11-04.md),
[architecture](ARCHITECTURE.md), [design](docs/codegen-parity-plan-2026-11-03.md),
[implementation history](docs/codegen-parity-progress-2026-11-03.md) and
[benchmarks](benchmarks/README.md).

Older `tools/` harnesses and historical documents retain their original
interfaces and results. Use the CMake targets above for this branch.
