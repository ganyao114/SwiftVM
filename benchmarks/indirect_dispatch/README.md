# Hot indirect dispatch

These freestanding x86-64 fixtures compare an identical guest ELF under SVM
Block, SVM Function, and the fixed Android FEX reference. The benchmark measures
the warmed path separately from process startup and JIT compilation.

`build.py` generates twelve direct/indirect CALL+RET and JMP cases. Every target
comes from a 1024-entry pointer table, and each result contributes to a checksum.
CALL target entries cover the retired sixteen-slot SVM site-cache indices or
deliberately collide there at a 64-byte stride. The same fixtures also compare
the current thread L1, whose byte-PC index matches the recorded FEX L1 index.
`build_returns.py` adds two paired cases with four
caller sites and one shared callee RET; incoming JMP targets stay distributed
while return addresses either spread across four indices or share one index.

`guest.S.in` owns the common loop, clock, decimal output, and two-round protocol.
The same guest PCs execute both rounds. After the initial 4096-iteration warmup,
each round performs:

1. Write `READY <host_tid>\n`; wait for byte `G` on stdin.
2. Read CLOCK_MONOTONIC, run the kernel, read the clock again.
3. Write `DONE <elapsed_ns> <checksum>\n`; wait for byte `X`.

Both rounds contain 4,194,304 iterations by default. Input `GXGX` runs the guest
without a controller. The manifest records actual target PCs, cache indices,
expected checksums, tool identities, assembly, and ELF hashes.

`pmu_region.cpp` is an Android native controller. It starts the child on CPU4,
executes the first round, confirms that the supplied TID belongs to the child,
and measures only the second round with a pinned hardware group containing
user cycles, instructions, and branch misses. The guest blocks before each
region; FEXServer and startup run outside the measured thread interval. Result
files record both guest clock time and PMU enabled/running time. The PMU window
includes the small G/clock/DONE boundary, while the guest clock excludes pipe
waiting and output formatting.

Build into a Data2 results directory:

```sh
python3 benchmarks/indirect_dispatch/build.py --out /Volumes/Data2/dispatch-results/guest
python3 benchmarks/indirect_dispatch/build_returns.py --out /Volumes/Data2/dispatch-results/return-fanin/guest
/Users/swift/Library/Android/sdk/ndk/29.0.13846066/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android28-clang++ \
    -O2 -std=c++20 -static-libstdc++ -Wall -Wextra -Werror \
    benchmarks/indirect_dispatch/pmu_region.cpp -o /Volumes/Data2/dispatch-results/pmu-region
```

Use the FEX binary and private service configuration described in
[`tools/fex-android/README.md`](../../tools/fex-android/README.md). Deploy the
controller, SVM runner, FEXLoader/FEXServer, configuration, and guest ELFs to an
owned device namespace. The controller invocation is:

```text
pmu-region CPU OUTPUT_PREFIX COMMAND [ARG...]
```

`OUTPUT_PREFIX.stderr` retains the launcher's complete log;
`OUTPUT_PREFIX.json` records counters and the guest result. Each sample needs a
new prefix. Root access is used for perf_event_open on the connected RedMagic.

`run.py --results DIRECTORY` consumes `plan.json`, `environment.json`, and
`guest/manifest.json` in a prepared result directory. Its plan fixes engine
order and repetition count before sampling. It rotates engines, retains every
observation, validates checksums/native execution/PMU scheduling, and writes raw
records plus medians. The completed reference plan and deployment identities
are linked from the [benchmark report](../../docs/indirect-dispatch-benchmark-2026-11-07.md).

CALL results include their RET. Direct controls expose common work and CFG
differences; subtraction is not an exact isolated lookup measurement. Report
cycles and instructions alongside wall time, since scheduling can lengthen
the latter. Counted-dispatch runs belong in a separate diagnostic batch.
