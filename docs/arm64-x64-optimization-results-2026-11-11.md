# ARM64 → x64 四项优化验收记录（2026-11-11）

候选版本实现了标量 F32/F64 加减乘、NEON 二元运算与 indexed FMUL 的 SSE2 快路径、NZCV 条件分支融合，以及访存直接使用分配寄存器。浮点快路径按指令提交 FPSR 的 IXC，特殊值和其他 FPCR 配置由 ARM 精确计算路径处理。分支融合保留 packed NZCV，访存恢复表记录实际访存指令的位置。

## 热执行与 JIT 编译

WSL `ProtonHM`，x64 Release/IPO，固定 CPU2。SwiftVM 基线为 `da0a692074dc00b56355bf160287b76f2fa5379c`，基线和候选均为 Function／pin 0。Dynarmic 使用未修改的 `9d4582339990d4eae53f1dc7160686920fc2075c`。三组轮换顺序，每组每项 31 个新进程，每进程取 3 次热执行的中位数。五个原始 ELF 逐一校验 SHA-256，并用 QEMU 核对输出。465 个计时进程的冷调用和热调用共校验 1,860 次输出，记录无解释回退或意外重编译。

| 负载 | 基线热执行 ms | 候选热执行 ms | Dynarmic 热执行 ms | 候选耗时变化 | 候选／Dynarmic | 基线 JIT μs | 候选 JIT μs |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 整数 | 39.500 | 29.408 | 13.042 | −25.5% | 2.26× | 908.63 | 895.29 |
| 访存 | 39.081 | 27.888 | 15.431 | −28.6% | 1.81× | 891.89 | 869.37 |
| 移位 | 42.460 | 30.815 | 16.230 | −27.4% | 1.90× | 942.28 | 929.16 |
| 浮点 | 19.634 | 10.399 | 4.456 | −47.0% | 2.33× | 943.79 | 930.06 |
| 综合计算 | 18.306 | 10.458 | 3.962 | −42.9% | 2.64× | 1901.84 | 2074.69 |

JIT 列取 SwiftVM 的 `compile_request_ns` 中位数，覆盖冷调用中的编译请求。进程初始化、冷调用总时间和热执行在原始样本中分别记录。逐轮候选／基线热耗时比的四分位区间为整数 0.734–0.754、访存 0.707–0.717、移位 0.722–0.730、浮点 0.523–0.533、综合计算 0.567–0.576。候选的综合计算编译请求中位数增加 172.85 μs（9.1%），其余四项下降 1.4%–2.5%。

## 生成代码与剩余瓶颈

硬件计数另跑 7 组交错样本，检查每个计数区间的 enabled time 与 running time 相等。表中的字节数取发布单元的代码大小之和。

| 负载 | 基线宿主指令数（百万） | 候选宿主指令数（百万） | Dynarmic 宿主指令数（百万） | 基线代码字节 | 候选代码字节 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 整数 | 564.006 | 444.006 | 276.001 | 488 | 456 |
| 访存 | 612.006 | 444.006 | 276.001 | 504 | 472 |
| 移位 | 600.006 | 480.006 | 312.001 | 504 | 472 |
| 浮点 | 446.005 | 65.005 | 28.001 | 652 | 908 |
| 综合计算 | 414.238 | 196.932 | 81.728 | 37,240 | 58,864 |
| 展开访存 | 202.505 | 139.505 | 66.001 | 840 | 728 |
| 向量矩阵 | 215.663 | 15.506 | 2.639 | 14,508 | 37,740 |

- 整数每轮从约 47 条宿主指令降至 37 条。融合后的终结分支直接测试 packed NZCV。现有测量报告四项优化的组合收益，单项贡献未做消融拆分。
- 向量矩阵热执行从 9.159 ms 降至 2.297 ms，纯标量矩阵从 7.989 ms 降至 4.553 ms。综合计算代码增加 21,624 字节（58.1%）。逐指令反汇编已定位增量，详见下方代码体积分解。
- 展开访存从 10.351 ms 降至 10.321 ms，宿主指令减少 31.1%，热执行基本持平。SHA 从 6.405 ms 降至 5.751 ms，排序从 1.386 ms 到 1.391 ms。P3a 的地址、来源与结果直达寄存器形态已验证，展开访存未测得稳定耗时改善。

### 代码体积与局部状态写回评估

综合计算中的矩阵编译单元包含 68 条 `ArmFPBinary`，代码从 14,652 增至 37,900 字节，其余单元合计减少 1,624 字节。按已发布二进制的指令地址逐条划分，这 68 条操作合计占 27,872 字节。

| 浮点操作内的发码范围 | 字节数 |
| --- | ---: |
| FPCR 检查、输入搬运及逐 lane 准入检查 | 11,212 |
| SSE 算术、MXCSR 保存恢复、IXC 提交及跳过慢路径 | 4,896 |
| 精确慢路径参数、活跃寄存器保存恢复与调用边界 | 11,764 |

本轮保留保守准入和逐指令 FPSR 提交，以维持现有数值与观察边界合同。综合负载的编译成本和代码体积增长作为这版实现的代价记录，原始发布单元、IR 和逐操作地址区间已归档。

P3b 的核查范围包括现有 `passes/rewrite.cpp` 的 uniform 读转发以及展开访存的发布 IR。现有规则保留可观察的 `StoreUniform`，访存循环中的 guest 访问具有 fault 观察边界。结合展开访存的耗时结果，第四批范围确定为 P3a，状态写回和 reservation 维护继续按现有恢复合同执行。

## 正确性与持久化

| 验证 | 结果 |
| --- | --- |
| ARM64 与 ARM32/Thumb 基础差分 | 4,224 次一致，覆盖 Block/Function、Known/Unknown/EarlyOff 和优化开关 |
| 原标量 FP 差分 | 27,648 次一致 |
| 新 ARM64 浮点二元与 indexed 差分 | 25,296 次一致，覆盖有效 lanes、源目的重叠、特殊值和 FPSR |
| 完整 CTest | 31 通过，7 跳过，0 失败。11 月 12 日重新构建后，完整 CTest 再次得到相同结果 |
| ARM64 与 ARM32 完整程序 | 每种 8 个配置通过，与 QEMU 输出一致。验证了 pin 0/2、磁盘缓存重载和 AOT |
| Thumb 完整程序 | 最终二进制 8 个配置通过，与 QEMU 输出一致。磁盘缓存重载和 AOT 均通过 |

57,168 次差分比较全部实际执行了 x64 原生代码。新浮点语料由 `tools/correctness/generate_arm64_fp_binary.py` 生成，参考结果由独立的 Unicorn oracle 提供。完整程序的磁盘缓存校验要求冷运行有写入、热重载命中且无重新编译；AOT 要求载入代码单元，且无回退或解释执行。

CTest 的跳过项为 `instruction_coverage_native`、`avx_coverage_native`、`x87_coverage_native`、`loongarch64_native_contract`、`arm64_native_contract`、`pipeline_native_contract` 和 `riscv64_native_contract`。x86 前端、浮点和解释器合约在已执行项目中。

## 11 月 12 日独立复核

WSL 访问已恢复。重新构建的比较 runner、完整程序 runner、AOT 工具和正确性 runner，其 SHA-256 均与此前验证的冻结二进制一致。直接读取原始结果后，确认 ARM64、ARM32、Thumb 的 24 组完整程序配置全部通过。

三种 ISA 的磁盘缓存冷运行各写入一次，重载分别命中 12、14、14 个代码单元，重载编译次数均为零。三种 AOT 运行各载入 6 个代码单元，并继续 JIT 编译剩余的 8、9、9 个单元，均无解释执行或 Function 回退。

使用相同五个 ELF 和冻结 runner，另跑 31 组完整交错样本，方法与首表一致。

| 负载 | 基线热执行 ms | 候选热执行 ms | Dynarmic 热执行 ms | 候选耗时变化 |
| --- | ---: | ---: | ---: | ---: |
| 整数 | 37.193 | 27.595 | 12.335 | −25.8% |
| 访存 | 40.091 | 28.597 | 15.743 | −28.7% |
| 移位 | 40.862 | 29.720 | 15.620 | −27.3% |
| 浮点 | 19.051 | 10.085 | 4.314 | −47.1% |
| 综合计算 | 17.532 | 9.954 | 3.725 | −43.2% |

本次综合计算的编译请求中位数从 1,762.06 增至 1,967.60 μs，增加 11.7%。其余四项的编译请求变化在 −0.8% 至 +1.3% 之间。两轮独立比较均确认综合负载的编译成本增长。

续跑后的第一次比较在整数负载中出现明显离散，候选／基线配对比的四分位区间为 0.698–0.879，当时 WSL 刚启动且存在系统后台活动。该轮完整保留于 `resume-bench/summary.json`，整数结论采用随后完整重跑的 `resume-steady-bench/summary.json`，其对应区间为 0.734–0.746。两轮均保存全部 31 组样本。

## 证据与复现

[证据摘要](evidence/arm64-x64-opt4-20261111/summary.json)已与 WSL 原始记录逐项核对，包含两轮稳定比较、一次高离散比较、JIT 时间、指令数、代码区间分解、正确性和持久化结果。[原始证据归档](evidence/arm64-x64-opt4-20261111/raw-evidence.tar.gz)保存逐进程计时、PMU 原始记录、差分比较记录、CTest、完整程序结果和复现脚本。完整差分 worker 输出继续保留在 `$HOME/.cache/SwiftVM/arm64-x64-opt4-20261111`。

[归档索引](evidence/arm64-x64-opt4-20261111/archive-index.json)记录归档 SHA-256 与成员数，归档内的 `manifest.json` 记录逐文件 SHA-256。

在原 WSL 环境中复用[基线 benchmark](dynarmic-benchmark-2026-11-11.md)留下的两个 runner 和固定 ELF 清单，即可复测综合负载的 JIT 时间和全部五项热执行。

```sh
ROOT=/mnt/d/Dev/SwiftVM
BUILD="$ROOT/build/host"
BASE="$HOME/.cache/SwiftVM/dynarmic-bench-20261111"
QEMU="$HOME/.cache/SwiftVM/wsl-arm-x64-20261111/qemu-packages/root/usr/bin/qemu-aarch64"
OUT=$(mktemp -d)

cmake --build "$BUILD" -j8
SVM_TEST_SCRATCH=/tmp ctest --test-dir "$BUILD" --output-on-failure \
  --output-junit "$OUT/ctest.xml"

python3 "$ROOT/tools/bench_dynarmic.py" \
  --swiftvm "$BUILD/svm_arm64_compare_bench" \
  --baseline-swiftvm "$BASE/runners/svm_arm64_compare_bench" \
  --dynarmic "$BASE/runners/dynarmic_arm64_compare_bench" \
  --dynarmic-source "$BASE/dynarmic" --qemu "$QEMU" \
  --reuse-guests "$BASE/final/summary.json" --scope function --pin-slots 0 \
  --cpu 2 --samples 31 --repeats 3 --out "$OUT/bench"
```

Thumb 可使用同一 ELF 和新的缓存目录复现持久化验证。

```sh
GUEST="$ROOT/build/wsl-arm-x64/compiled-programs/thumb"
"$(dirname "$QEMU")/qemu-arm" "$GUEST" > "$OUT/thumb.expected"
mkdir -m 700 "$OUT/thumb-cache"
for phase in cold reload; do
  "$BUILD/svm_rebuild_linux" --scope function --cache-dir "$OUT/thumb-cache" \
    "$GUEST" > "$OUT/thumb.$phase" 2> "$OUT/thumb.$phase.log"
  cmp "$OUT/thumb.expected" "$OUT/thumb.$phase"
done
"$BUILD/svm_aot" compile "$GUEST" -o "$OUT/thumb.aot" --scope function --max 16 --no-sweep
"$BUILD/svm_aot" run --aot "$OUT/thumb.aot" --scope function \
  > "$OUT/thumb.aot.out" 2> "$OUT/thumb.aot.log"
cmp "$OUT/thumb.expected" "$OUT/thumb.aot.out"
grep '"measurement_kind"' "$OUT"/thumb.*.log
```

核对 `cold` 的 `disk_cache_writes`、`reload` 的 `disk_cache_hits` 和零 `compile_attempts`，以及 AOT 的 `aot_units_loaded`。整程序的八配置为 Block/Function 各自的 JIT pin 0、JIT pin 2、mixed pin 0 和 interpreter pin 0。

四批实现的收尾验收与证据归档已完成。代码体积和编译成本的代价、展开访存的持平结果、七项 CTest 跳过均保留在记录中。
