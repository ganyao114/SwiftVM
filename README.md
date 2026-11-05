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
10 seconds and validated CRC `0xd340`; each table entry is a median of five.

| Metric | Block | Requested Function | FEX |
| --- | ---: | ---: | ---: |
| Runtime | 16.119 s | 15.627 s | 15.047 s |
| Runtime / FEX | 1.071x | 1.039x | 1x |
| User cycles / FEX | 1.072x | 1.038x | 1x |
| User instructions / FEX | 1.038x | 1.010x | 1x |
| Cold compile excluding byte parser | 45.688 ms | 51.463 ms | 47.700 ms |
| Compile / FEX | 0.958x | 1.079x | 1x |

Compile includes formation, IR construction, optimization, allocation, encoding,
failures/retries, publication and release; only byte-parser calls are removed.
The post-push checkpoint measures committed subject `390cf73d` with five fresh
launches per arm/input: 120 runtime and 120 compile observations. All 48 fixed
input limits pass in this round (1.10x runtime/user-cycles and 2x compile);
worst median ratios are 1.0712x, 1.0771x and 1.9078x respectively.
The earlier three-round x87 Block result of 2.109x and its independent seven-round
confirmation remain archived. Short compile cases are near the limit; this
checkpoint does not establish an upper bound for other inputs or environments.
The same-source entry audit reduced CoreMark repeated construction from 15,889
to 191 instruction occurrences. Current Function compile is 51.463 ms versus
81.982 ms before entry reuse; runtime is 15.627 s versus 15.282 s at that earlier
checkpoint, so the change retains a runtime tradeoff.
FP runtime is 0.939/0.950x FEX and FP instruction counts are 0.990/1.006x;
x87 runtime is 0.645/0.641x. CoreMark spill
slots are 25/31 and maximum frames 48/64 bytes for Block/Function.
Instruction counts still differ. The x87 guest has an exit-only oracle, with
separate finite precision/state contracts; this checkpoint covers these fixed
inputs rather than the entire x86 ISA or general application performance.

See the [post-push five-round benchmark anchor](docs/post-push-benchmark-2026-11-05.md),
[Function entry reuse and construction audit](docs/function-entry-reuse-2026-11-05.md),
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
