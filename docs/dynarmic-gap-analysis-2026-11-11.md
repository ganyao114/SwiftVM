# ARM64→x64：SwiftVM 与 Dynarmic 的差距分解

本轮测量把差距收敛到三个方向：**FP 数值 helper 路径、NZCV 到条件分支的转换、剩余的状态读写与搬运**。整数微循环不是运行时调度或分支失误占主导；综合程序最大的独立分项差距是矩阵中的浮点路径。

## 方法与验证

沿用上一轮固定的 SwiftVM `da0a6920` 运行时、Dynarmic `9d458233` 和五份原始 ELF；WSL、CPU2、Release/IPO、直接内存访问、SwiftVM Function/pin0 与 Dynarmic 的配置保持一致。

另构建两个诊断 runner：只在原 benchmark 中加入 PMU 开关、公开的反汇编调用、SwiftVM 编译／发布观察回调。编译器和运行时源码没有修改，回调不改变 IR。普通 runner 和上一轮全部结果保留。

- 基线：每项 21 对新进程，每个进程一次冷执行、三次热执行。每进程先取热中位数，再取 21 个进程中位数。
- PMU：当前线程、仅用户态，固定组计数 cycles、retired instructions、branches、branch misses；全部记录的 enabled/running 时间相等，无复用缩放。
- 控制实验：16 份替代 guest，覆盖循环条件、8 次展开及四个计算分项；另有两份真正关闭向量化的 guest。每项 21 对进程，结果分别与独立 Python 预期和 QEMU 比对。
- 独立热点采样：每个引擎／原始负载 3 个进程，每进程 60 次热执行，250 μs CPU-clock IP 采样。采样有扰动，其耗时不用于性能表。
- 共 **966 个普通诊断进程、30 个采样进程、5,694 次结果校验通过的函数执行**；5,694 份 PMU 记录均有效，无热执行新增编译、无解释步数或回退；174,700 个 IP 样本，0 丢失。

## 1. 先区分代码数量与执行效率

下表使用本轮不启用采样的 PMU 基线。指令数包含完整热调用，微循环取整到每轮后与反汇编吻合；不把静态代码体积当作动态计数。

| 工作负载 | SwiftVM/Dynarmic 热耗时 ms | 动态指令数之比 | IPC：SwiftVM/Dynarmic | 周期数之比 |
| --- | ---: | ---: | ---: | ---: |
| 整数 | 39.626 / 13.057 | 2.043× | 3.096 / 4.598 | 3.035× |
| 访存 | 39.537 / 15.510 | 2.217× | 3.335 / 3.808 | 2.532× |
| 移位 | 39.992 / 15.270 | 1.923× | 3.159 / 4.328 | 2.635× |
| FP | 19.212 / 4.370 | 15.928× | 5.087 / 1.400 | 4.383× |
| 综合计算 | 18.134 / 4.001 | 5.069× | 5.006 / 4.470 | 4.525× |

例如整数周期差距可以精确按指标拆为：`2.043× 指令数量 × 1.485× 每指令周期 = 3.035× 周期`。这个等式不把两个因素误当作可相加的独立优化收益。

FP 的指令数增加远大于时间增加：软件计算执行很多可并行的整数指令，而 Dynarmic 的短加法循环存在浮点数据依赖。因此不能用指令数量比直接预测提速倍数。

## 2. 整数循环：主要多在 NZCV 与布尔分支

原整数 guest 热循环为 `ADDS; EOR; SUBS; B.NE`。SwiftVM 每轮退休 **47 条**宿主指令，Dynarmic **23 条**；PMU 对 12,000,000 轮分别记录约 564,005,518 与 276,000,582 条，固定调用开销只占尾数。

| 同一条热路径的指令分类 | SwiftVM | Dynarmic |
| --- | ---: | ---: |
| 算术主体及相关状态搬运 | 10 | 7 |
| 循环计数器更新及保存 | 4 | 6 |
| 捕获、打包、合并 NZCV，及 carry 反转 | 19 | 与下一项合计 8 |
| 读取条件、生成布尔值、再进行分支 | 12 | 包含在上述 8 中 |
| 事件检查 | 2 | 2 |
| 合计 | 47 | 23 |

SwiftVM 的路径为 `pushfq/pop → 按 ARM 位布局打包 → 合并 rbx → TestNotFlags 生成 Bool → 写入结果寄存器 → terminal 再 Mask/Test/Jcc`。Dynarmic 在这条路径用 `lahf/seto` 保存较接近宿主布局的 flags，再用 `sahf/Jcc` 消费。完整对照保存在 `docs/evidence/dynarmic-gap-20261111/integer-hot-loops.txt`。

这里有两个重要边界：

- `ADDS` 随后被 `SUBS` 覆盖的 flags **已经被消除**。将它改成普通 `ADD`，动态指令数完全不变。继续泛化地优化这处死 flags 不会缩小差距。
- `SUBS` 的输出在优化后仍是 `[flags=0xf, branch-only=false]`。退出、事件观察等语义需要保留；不能直接删除全部 NZCV 维护。

控制实验改用 `SUB + CBNZ`，只通过计数器值控制循环，不读取 NZCV。下表两边均执行同一份替代 guest；输出相同，但其最终 NZCV 不属于被比较的输出契约。

| Guest 形式 | SwiftVM ms | Dynarmic ms | SwiftVM/Dynarmic |
| --- | ---: | ---: | ---: |
| 空循环：SUBS + B.NE | 39.052 | 8.065 | 4.842× |
| 空循环：SUB + CBNZ | 10.766 | 8.013 | 1.344× |
| 整数：ADD 主体、SUBS + B.NE | 40.528 | 13.371 | 3.031× |
| 整数：ADD 主体、SUB + CBNZ | 11.698 | 10.709 | 1.092× |
| 访存：SUB + CBNZ | 13.715 | 13.645 | 1.005× |
| 移位：SUB + CBNZ | 13.727 | 14.083 | 0.975× |

这确认了这些短循环对 flags／条件链的高度敏感性，**不是一个可以直接套用到原程序的 29 ms 优化承诺**。替代 guest 同时改变了指令与依赖链，也改变了 flags 的观察需求。

相同算术工作展开 8 次再分支时，整数耗时为 9.606 / 8.064 ms；同时采用不读 flags 的计数循环则为 8.154 / 8.128 ms。摊薄每轮控制／状态提交开销后，纯整数主体的性能接近。

## 3. FP：软件数值计算和每次调用的 ABI／环境转换

原标量 `FADD` 循环每轮退休约 **446 / 28 条**宿主指令。SwiftVM 的 IR 是 `GetUniformAddress + LoadImm + CallLambda`；每次执行进入 `ExecuteNumeric → ExecuteScalarFP → EvaluateBinaryFP → SoftFloat`，重复做指令解码、输入处理、软件运算、FPSR 更新。

生成代码还在每次调用周围执行 phase 与 flags 提交、`stmxcsr`／`ldmxcsr`、参数准备、间接调用、恢复 guest MXCSR 和 flags。该微循环的栈预留为 320 字节，包含 helper 调用区；这不是每轮执行 40 次 spill 的含义。

独立采样中，SwiftVM 标量 FP 循环的样本分布为：

| 样本位置 | 比例 |
| --- | ---: |
| 数值 helper 与 SoftFloat 函数内部 | 51.90% |
| JIT 中的 helper 调用边界，包括环境切换／状态维护 | 23.56% |
| 其余生成代码，包括循环控制和参数物化 | 24.45% |
| 其他 | 约 0.09% |

约四分之三的样本落在 helper 闭包及其边界；不是只有 SoftFloat 中的一条运算需要优化。采样可能将停顿归到下一条指令，比例仅作为诊断，不当作互相独立的可删除时间。

将 FP 循环改成 `CBNZ` 后，SwiftVM 仍需 17.243 ms；8 次展开后仍需 17.366 ms，而 Dynarmic 展开后为 2.641 ms。与整数不同，摊薄循环分支远不足以解决 FP 的主体成本。

## 4. 综合程序：矩阵浮点是最大的分项

为了分辨来源，将原 C 程序的四段计算分别编译、执行，恢复相同初始内存，核对完整 56 字节结构：所选阶段的结果必须正确，其他字段必须保持零。

| 独立编译的阶段 | SwiftVM ms | Dynarmic ms | 耗时差 ms | 比值 |
| --- | ---: | ---: | ---: | ---: |
| SHA256 | 6.629 | 2.959 | 3.670 | 2.240× |
| 排序及其输入生成 | 1.546 | 0.596 | 0.950 | 2.594× |
| 整数混合 | 0.466 | 0.231 | 0.235 | 2.017× |
| 矩阵及 FP 初始化／归约 | 9.607 | 0.121 | 9.486 | 79.123× |

矩阵独立分项的差值约占这四个独立分项差值总和的 **66%**，SHA 约 **26%**，排序约 **7%**，混合约 **2%**。这只是优先级估计：分项重新编译会改变代码、地址和分配，不能把这些比例当作原程序的精确时间分区。

原始、未拆分程序的采样也支持 FP 优先：**42.49%** 样本在数值 helper／SoftFloat 内部，另有 **4.65%** 在 JIT 的 helper 调用边界，51.25% 在全部生成代码内，6.03% 在共享库／其他系统辅助代码。热点包括 `EvaluateBinaryFP`、`ExecuteVectorFPBinary`、`NumericIndexed`、SoftFloat 的乘法与舍入。

### 修正上一轮的向量化描述

上一轮命令把 `-fno-vectorize -fno-slp-vectorize` 放在 `-O2` 之前。Clang 18 的实际 driver 输出包含 `-vectorize-loops -vectorize-slp`，优化后 IR 有 66 处 vector FP 运算。反汇编直接确认 `FADD V?.2D`、`FMUL V?.2D` 和 indexed `FMUL ... V?.D[0]`。因此原综合程序不能称为纯标量；相同机器码的引擎对比仍然有效。

补充将禁用选项放在 `-O2` 后，确认 IR 和 A64 反汇编不再包含 vector FP 运算，且输出保持一致：

| 真正关闭向量化的控制 | SwiftVM ms | Dynarmic ms | 比值 |
| --- | ---: | ---: | ---: |
| 矩阵独立阶段 | 8.558 | 0.420 | 20.399× |
| 完整综合程序 | 17.412 | 4.280 | 4.069× |

这说明差距同时存在于标量和 NEON 路径。只实现标量 `FADD` 无法覆盖原矩阵热点，必须考虑 vector binary 与 indexed multiply。

## 5. 剩余状态搬运与可以降低优先级的方向

访存循环展开 8 次后，SwiftVM / Dynarmic 仍为 11.575 / 4.401 ms，约 2.63×；动态指令数为 202.505 / 66.001 百万。两边均为直接访存，没有数据回调。SwiftVM 的生成代码可见：

- 地址和数据经过 RDI／RAX 的固定临时寄存器，再搬回分配结果。
- guest store 后清除 reservation 状态。
- guest GPR 的 uniform 写回与读取，以及块内未消除的中间写回。

这批展开实验表明，摊薄 flags 成本之后仍有状态维护和搬运机会，不能因为未展开的 CBNZ 访存循环接近就认为访存已优化完毕。不过，指令数量减少也不能保证按比例提速；例如移位展开后 SwiftVM 的指令仍更多，耗时反而略低。

下面这些目前证据不支持作为首要方向：

- **分支预测**：整数／访存／移位的 SwiftVM 分支失误率仅约 0.0012%–0.0015%。
- **频繁回到 C++ 调度**：上述三个微基准有超过 99.95% 的样本在生成代码中；整数 Function 内的回边是直接本地分支。综合程序的原生 dispatcher／entry 样本约 0.21%。
- **整数寄存器 spill**：这三个 SwiftVM 微基准的编译结果均为 0 spill slots、0 frame bytes；Dynarmic 反而能看到少量栈临时值。不能把差距概括成 SwiftVM 寄存器不足。
- **继续删除第一个 ADDS 的 flags**：优化后的 IR 已经做完这件事。

## 建议顺序与验收

1. **x64 原生 ARM FP 常见路径**：按热点覆盖 scalar、vector binary、indexed multiply，先处理默认 FPCR 的 F32/F64 ADD/SUB/MUL，异常条件走保留的精确路径。验收同时检查 FPCR、FPSR、NaN、subnormal、舍入、upper lanes，不只比较普通数值。矩阵与 FP 展开循环是主要性能验收。
2. **条件消费与终结分支融合**：消除 `TestNotFlags → Bool 物化／归一化 → terminal 再读`；进一步评估 NZCV 的紧凑保存或按需物化。保留事件、退出和命名 snapshot 的正确观察，不照搬 CBNZ 替代 guest 的语义。
3. **状态提交与访存结果直达寄存器**：减少 RAX/RDI 中转，检查安全范围内的中间 uniform 写回和重复 reservation 清除；必须维护精确 fault 位置、恢复与 helper 状态可见性。用展开访存、SHA 和排序检验残余收益。

本轮只增加分析工具与证据，没有把替代 guest 的变化当作运行时优化提交，也没有据此承诺可以直接达到 Dynarmic 的数值。

## 证据与复现

精简证据：`docs/evidence/dynarmic-gap-20261111/summary.json`。包含 21 对样本的分布、PMU 中位数、受控 guest 的源码／ELF 哈希、runner 哈希、编译命令、热点符号与采样分类、原始证据索引。整数热路径另有可直接审阅的文本。之前的 benchmark 报告同步修正了向量化描述。

本机原始数据在 `/home/proton/.cache/SwiftVM/dynarmic-gap-20261111/`，`baseline-summary.json`、`ablations-summary.json`、`scalar-summary.json`、`profile-summary.json` 保留逐样本结果；`artifact-manifest.json` 绑定全部 IR、实际发布代码、反汇编、IP 与映射记录。

```sh
ROOT=/mnt/d/Dev/SwiftVM
WORK="$HOME/.cache/SwiftVM/dynarmic-gap-reproduce"
# 先按上一份报告构建普通 SwiftVM 与 Dynarmic runner；需要可用的用户态 PMU。
python3 "$ROOT/tools/analyze_dynarmic_gap.py" --out "$WORK" --stage build
python3 "$ROOT/tools/analyze_dynarmic_gap.py" --out "$WORK" --stage guests
python3 "$ROOT/tools/analyze_dynarmic_gap.py" --out "$WORK" --stage baseline --samples 21
python3 "$ROOT/tools/analyze_dynarmic_gap.py" --out "$WORK" --stage ablations --samples 21
python3 "$ROOT/tools/analyze_dynarmic_gap.py" --out "$WORK" --stage scalar --samples 21
python3 "$ROOT/tools/analyze_dynarmic_gap.py" --out "$WORK" --stage profile --samples 3
python3 "$ROOT/tools/summarize_dynarmic_gap.py" --work "$WORK"
```

默认从上一轮 cache 读取固定版本与原始 ELF；可用 `--original` 指定该目录。诊断输出要求空目录，避免把两次测量的发布记录混在一起。结果仍受 WSL、频率波动和有限工作负载限制；PMU 和采样只支持本轮输入的归因，不能外推为完整应用收益。
