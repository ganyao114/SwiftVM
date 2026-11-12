# SwiftVM 与 Dynarmic：ARM64→x64 基准

2026-11-11，在 WSL 中比较同一份 ARM64 机器码。**Dynarmic 在本轮五类负载的热执行中全部领先；SwiftVM Function／pin 0 的耗时为其 2.55–4.61 倍。** 首次进程调用的结果不同，包含显著的一次性库初始化，需与热执行分别解读。

## 版本与配置

- SwiftVM：`da0a692074dc00b56355bf160287b76f2fa5379c` 的运行时源码，新增本轮测试工具；运行时实现未改动。
- Dynarmic：`https://github.com/yuzu-mirror/dynarmic.git`，固定 `9d4582339990d4eae53f1dc7160686920fc2075c`，项目版本 6.7.0；工作树干净，未打补丁。
- 环境：WSL Ubuntu x86-64，AMD Ryzen 9 7950X3D，固定 CPU2；两边均 GCC 13.3 Release／IPO。Guest 使用 Clang 18 `-O2`、关闭 FP contraction。后续反汇编确认综合程序包含 NEON；原命令中的关闭向量化选项位于 `-O2` 之前，被后者重新启用，不能将综合程序描述为纯标量。补测与归因见 `docs/dynarmic-gap-analysis-2026-11-11.md`。
- SwiftVM 测试 Block／Function × pin 0／2。Dynarmic 使用其正常块链接、全部安全优化、宿主特性检测；unsafe 优化关闭，周期计数关闭，默认 128 MiB 代码缓存，fastmem base=0、地址位数=64、禁止地址镜像。`IsReadOnlyMemory` 保持默认保守行为。
- 两个 runner 均放在 Linux 文件系统。Dynarmic 独立构建，使用其附带依赖，不与 SwiftVM 混用 fmt 或其他库。

## 测量方法与正确性

每项使用相同 ELF 和入口函数。微基准覆盖整数、访存、移位和双精度加法；综合程序包含 SHA256、插入排序、32 位整数混合及 FP 矩阵。整数／访存／移位各 12,000,000 次，FP 1,000,000 次。

两边都直接访问 guest==host 内存。计时前恢复全部可写 ELF 段、栈和寄存器；计时范围仅为 SwiftVM `VM::CallGuest` 或 Dynarmic `Jit::Run`。执行后核对全部输出字节。入口函数通过不可执行的地址 0 返回宿主；Dynarmic 的回调验证该地址的 `NoExecuteFault`，正常执行没有系统调用。

每个配置先丢弃一个进程的预热结果，再交错测量 31 个全新进程。每个进程记录一次首次调用、三次缓存代码调用；先取该进程三次热调用的中位数，再取 31 个进程的中位数。配置顺序按样本轮换并反转。配对分布以同一样本编号的 SwiftVM／Dynarmic 之比计算。

五份 ELF 的完整输出均与 QEMU 及独立 Python 预期一致。正式样本共 775 个进程、3,100 次函数执行全部通过，另有 25 个预热进程的 100 次执行。所有热调用都确认没有新增编译；无解释步骤、无回退，Dynarmic 数据访存回调计数为 0。综合结果的全部 56 字节均检查，SHA256 字段独立使用 Python hashlib 计算。

## 热执行结果

下表单位为 ms，中位数；最后一列为 SwiftVM Function／pin 0 除以 Dynarmic，越小越好。

| 负载 | Dynarmic | SwiftVM Block/pin0 | Block/pin2 | Function/pin0 | Function/pin2 | F/pin0 比值 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 整数 | 13.065 | 40.445 | 40.219 | 39.381 | 39.749 | 3.014× |
| 访存 | 15.768 | 40.832 | 40.399 | 40.182 | 40.050 | 2.548× |
| 移位 | 15.937 | 41.527 | 40.994 | 41.669 | 40.829 | 2.615× |
| FP | 4.490 | 19.869 | 19.765 | 19.569 | 19.513 | 4.358× |
| 综合计算 | 4.051 | 19.151 | 19.656 | 18.690 | 19.087 | 4.614× |

全部 20 组配置对比均为 31/31 对 Dynarmic 更快。Function／pin 0 配对比值的四分位区间依次为 `[3.004,3.058]`、`[2.540,2.573]`、`[2.600,2.638]`、`[4.332,4.441]`、`[4.550,4.696]`。完整精度、其他配置的分布和离群值保留在证据中。

Block／Function 和 pin 0／2 的变化远小于两种引擎之间的热执行差距。这批输入上，切换 scope 或 pin 配置不足以消除差距。

## 首次调用与初始化

下表仅列 Function／pin 0 与 Dynarmic，单位 ms。初始化包括引擎构造、映射和快照；首次调用包含懒编译和实际 guest 执行。每列独立取中位数，不能将列间相减当作配对统计。

| 负载 | SwiftVM 初始化 | Dynarmic 初始化 | SwiftVM 首次调用 | Dynarmic 首次调用 |
| --- | ---: | ---: | ---: | ---: |
| 整数 | 0.670 | 22.020 | 40.102 | 58.381 |
| 访存 | 0.709 | 22.317 | 41.043 | 61.051 |
| 移位 | 0.706 | 22.579 | 42.450 | 61.625 |
| FP | 0.682 | 22.924 | 20.513 | 51.266 |
| 综合计算 | 0.691 | 22.676 | 20.744 | 53.340 |

Dynarmic 的首次调用减去同进程热中位数，额外开销中位数约 45.3–49.4 ms。但其中包含进程级一次性工作，不能作为每次编译成本。

为核对这项现象，另做了 21 个进程的诊断：每个进程依次创建三个**全新** Dynarmic JIT，执行同一份 1,000 次整数循环，每个实例均编译自己的代码；输出全部校验，热调用没有新增取码。

| 同一进程中的新实例 | 初始化 ms | 首次执行 ms | 再次执行 μs | 编译期间取码次数 |
| --- | ---: | ---: | ---: | ---: |
| 第一个 | 20.396 | 43.554 | 1.270 | 16 |
| 第二个 | 20.459 | 0.082 | 1.220 | 16 |
| 第三个 | 14.564 | 0.073 | 1.230 | 16 |

后两个实例没有继承之前的机器码。这个结果确认首次进程初始化对冷调用影响很大；Dynarmic 源码的 `frontend/A64/decoder/a64.h` 也存在首次 `Decode` 时构造静态查找表的路径。查找表是合理的重点归因方向，诊断没有逐阶段计时，因此不把全部差异精确归于它。诊断的 126 次执行全部校验通过。

本轮对外比较的是首次调用总成本和热执行成本，没有修改 Dynarmic 加入编译计时。SwiftVM 原有 `compile_request_ns` 单独保留，不能直接与 Dynarmic 的冷／热差值比较。

## 对下一批优化的启示

1. FP 是优先候选：当前 A64 前端的 `TryNativeScalarFP` 仅对 AArch64 宿主放行，x64 继续走数值 helper。可以研究默认 FPCR 下的 x64 原生路径，并以现有 FP 边界差分保护 NaN、舍入及 FPSR 语义。
2. 整数／移位／访存循环仍有约 2.5–3.1 倍热执行差距。下一步应对照生成代码，检查死 flags 更新、guest state 提交和结果寄存器搬运；计时本身尚不能证明各项贡献。
3. 保留 SwiftVM 的低首次调用成本，同时分别记录进程首次初始化、后续新 JIT 的编译和缓存代码执行，避免优化方向被混合计时误导。

这是四项标量微基准及一项含编译器生成 NEON 的综合程序的 WSL 测量。本轮未锁定 CPU 频率、未采集 PMU；不代表完整 ISA、真实 Android 应用或一般系统调用性能。此处函数计时与既有报告的整进程 wall time 使用不同边界，不直接拼接比例。后续差距分析补充了 PMU 计数与采样。

## 复现

依赖 Linux/x64 的 GCC、CMake、Ninja、Boost 头文件、Clang/LLD 和 QEMU AArch64。Dynarmic 及依赖独立放在本机 cache；本轮 Boost 开发包仅解压到 cache，没有修改系统安装。

```sh
ROOT=/mnt/d/Dev/SwiftVM
WORK="$HOME/.cache/SwiftVM/dynarmic-bench-20261111"
git clone https://github.com/yuzu-mirror/dynarmic.git "$WORK/dynarmic"
git -C "$WORK/dynarmic" checkout 9d4582339990d4eae53f1dc7160686920fc2075c

cmake -S "$ROOT" -B "$ROOT/build/host" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSVM_ENABLE_IPO=ON \
  -DSVM_BUILD_ARM64_COMPARE_BENCHMARK=ON
cmake --build "$ROOT/build/host" --target svm_arm64_compare_bench -j8

cmake -S "$ROOT/benchmarks/dynarmic" -B "$WORK/runner-build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
  -DDYNARMIC_SOURCE_DIR="$WORK/dynarmic" \
  -DBOOST_ROOT="$WORK/deps/usr"
cmake --build "$WORK/runner-build" --target dynarmic_arm64_compare_bench -j8

mkdir -p "$WORK/runners"
cp "$ROOT/build/host/svm_arm64_compare_bench" "$WORK/runners/"
cp "$WORK/runner-build/dynarmic_arm64_compare_bench" "$WORK/runners/"
python3 "$ROOT/tools/bench_dynarmic.py" \
  --swiftvm "$WORK/runners/svm_arm64_compare_bench" \
  --dynarmic "$WORK/runners/dynarmic_arm64_compare_bench" \
  --dynarmic-source "$WORK/dynarmic" \
  --qemu /path/to/qemu-aarch64 \
  --out "$WORK/reproduce" --cpu 2 --samples 31 --repeats 3 \
  --iterations 12000000 --fp-iterations 1000000
```

使用系统 Boost 时可省略 `BOOST_ROOT`；本机路径指向已解压的 `libboost1.83-dev`。精简证据在 `docs/evidence/dynarmic-bench-20261111/summary.json`，绑定源码、runner、ELF、预期输出和原始记录哈希。全部样本保存在 `$WORK/final/summary.json`；冷实例诊断在 `$WORK/cold-instances-summary.json`，诊断源码与构建命令也由哈希记录。常规比较无需该补充诊断即可复现。
