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
and a C compiler. Ninja is used below; diStorm is included.

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

Batch37 used the same original Bionic/CoreMark ELF for 100000 iterations on
RedMagic NX729J CPU4 against fixed FEX `a156752f`. All runs exceeded 10 seconds
and validated CRC `0xd340`.

| Metric | Block | Requested Function | FEX |
| --- | ---: | ---: | ---: |
| Runtime | 16.302 s | 15.493 s | 15.072 s |
| Runtime / FEX | 1.082x | 1.028x | 1x |
| User cycles / FEX | 1.081x | 1.030x | 1x |
| User instructions / FEX | 1.027x | 0.987x | 1x |
| Cold compile excluding byte parser, median of 3 | 48.187 ms | 84.763 ms | 47.320 ms |
| Compile / FEX | 1.018x | 1.791x | 1x |

Compile includes formation, IR construction, optimization, allocation, encoding,
failures/retries, publication and release; only byte-parser calls are removed.
This fixed workload meets the 1.10x runtime/cycles and 2x compile limits. It does
not establish performance across all applications. Spill slots remain 25/42 and
maximum frames 48/64 bytes for Block/Function.

See the [checkpoint and evidence](docs/codegen-parity-checkpoint-2026-11-04.md),
[architecture](ARCHITECTURE.md), [design](docs/codegen-parity-plan-2026-11-03.md),
[implementation history](docs/codegen-parity-progress-2026-11-03.md) and
[benchmarks](benchmarks/README.md).

Older `tools/` harnesses and historical documents retain their original
interfaces and results. Use the CMake targets above for this branch.
