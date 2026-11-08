# 间接调用与 block dispatcher benchmark：2026-11-07

本文保留旧 per-site cache 基线。线程 L1 / VM 共享 L2 的实现与新一轮同输入结果见 [2026-11-08 报告](thread-dispatch-cache-2026-11-08.md)。

当前 SVM 的局部热命中 JMP 已与固定 FEX 对照接近；CALL + RET 单目标仍多约 20% cycles。最大的确定差距是局部缓存地址冲突：同槽目标使 CALL 路径接近 FEX 的 2 倍，JMP 达到约 2.25 倍。不能把这解释为每次都回 C++ dispatcher；本轮冲突主要在生成代码内探测全局 ready directory。

## 测量范围

- 红魔 NX729J，序列号 `bf1359ff`，CPU4。SVM 产品代码提交 `cbd06547a2d9e5259f65bc99a69e95c055aa8bcb`；本轮只增加 benchmark 工具和报告，没有修改 runtime。
- FEX 为既有固定 Android 对照 `a156752fae3a4f65fefb1ed95af980bcdfac43d7` 加 Android 平台补丁；实际 performance ELF SHA256 为 `8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175`。不是另一个本地 build/Bin/FEXLoader，也不宣称最新官方 FEX。
- FEX 保留默认 multiblock、TSO；AVX/AOT/object cache 沿用既有关闭配置。FEX CompileBlock 可以包含多个基本块，不能把名称与 SVM Block scope 等同；因此同时报告 SVM Block 和 Function。
- 首批 12 个 guest，每个在三个引擎各采固定 3 次，共 108 样本。交错轮换引擎顺序，保留全部样本，不选最快值。
- 每轮 4,194,304 次操作，预热 4096 次；再完整执行一轮相同 PC 的 READY/G/clock/kernel/clock/DONE/X，第二轮才计数。目标从实际 guest 指针表加载，始终经过同一条动态 CALL/JMP；direct 对照也保留指针读取、选择算术及可观察 checksum。
- 主结果为 guest host TID 上的 user cycles、instructions、branch-misses 同组计数；确认 TID 属于子进程且仅绑定 CPU4，所有样本无 multiplex。编译、启动、FEXServer 和第一轮不在 PMU 窗口内。SVM 全部 `interpreted_steps=0`、`wx_fallback_used=false`，正式样本禁用诊断计数器。

每个表项是 3 次的中位数，单位为每次 kernel 迭代，包含共同选择/校验工作。CALL 项包含一次 CALL 和对应 RET，JMP 项没有逐次 CALL/RET，二者不能直接按同一条分支比较。PMU 开关还覆盖少量 G 唤醒、clock、DONE 格式化和系统调用边界，不能将每一条计数指令都归为查表核心。

## cycles 结果

| 场景 | SVM Block cycles | SVM Function cycles | FEX cycles | Block/FEX | Function/FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| 直接 CALL + RET 对照 | 7.865 | 7.864 | 6.337 | 1.241× | 1.241× |
| 间接 CALL，1 目标 | 8.988 | 8.979 | 7.507 | 1.197× | 1.196× |
| 间接 CALL，4 目标 / 4 B 间距 | 9.135 | 9.048 | 7.504 | 1.217× | 1.206× |
| 间接 CALL，16 目标 / 4 B 间距 | 9.117 | 9.204 | 7.718 | 1.181× | 1.193× |
| 间接 CALL，4 目标 / 64 B 同槽 | 14.802 | 15.029 | 7.507 | 1.972× | 2.002× |
| 间接 CALL，16 目标 / 64 B 同槽 | 15.186 | 15.340 | 7.719 | 1.967× | 1.987× |
| 间接 CALL，64 目标固定排列 | 12.764 | 17.790 | 16.959 | 0.753× | 1.049× |
| 直接 JMP 对照 | 4.984 | 4.179 | 3.060 | 1.629× | 1.366× |
| 间接 JMP，1 目标 | 6.433 | 6.369 | 6.402 | 1.005× | 0.995× |
| 间接 JMP，4 目标 / 16 B 间距 | 6.582 | 6.489 | 6.577 | 1.001× | 0.987× |
| 间接 JMP，4 目标 / 64 B 同槽 | 14.756 | 14.913 | 6.576 | 2.244× | 2.268× |
| 间接 JMP，64 目标固定排列 | 13.832 | 18.398 | 16.823 | 0.822× | 1.094× |

## 指令数与冲突诊断

| 场景 | Block 指令/次 | Function 指令/次 | FEX 指令/次 |
| --- | ---: | ---: | ---: |
| 直接 CALL + RET 对照 | 36.002 | 36.002 | 28.000 |
| 间接 CALL，1 目标 | 43.002 | 43.002 | 34.000 |
| 间接 CALL，4 目标 / 4 B 间距 | 43.002 | 43.002 | 34.000 |
| 间接 CALL，16 目标 / 4 B 间距 | 43.002 | 43.002 | 34.000 |
| 间接 CALL，4 目标 / 64 B 同槽 | 60.252 | 60.252 | 34.000 |
| 间接 CALL，16 目标 / 64 B 同槽 | 63.627 | 63.627 | 34.000 |
| 间接 CALL，64 目标固定排列 | 54.281 | 54.281 | 34.000 |
| 直接 JMP 对照 | 22.002 | 17.002 | 14.000 |
| 间接 JMP，1 目标 | 29.002 | 28.002 | 32.000 |
| 间接 JMP，4 目标 / 16 B 间距 | 29.002 | 29.502 | 32.000 |
| 间接 JMP，4 目标 / 64 B 同槽 | 46.252 | 46.752 | 32.000 |
| 间接 JMP，64 目标固定排列 | 40.281 | 41.250 | 32.000 |

另外对选定场景各做一次独立 `--decode-timing` 诊断，数值不混入上表。目录探测包含冷目标和线性重探测，归一化分母为预热与两轮的总迭代次数；`cache_hits` 同时包括局部与目录 ready 命中，不能当作局部 hit rate。

| 场景 | Block 目录探测/次 | Function 目录探测/次 |
| --- | ---: | ---: |
| 间接 CALL，1 目标 | 0.00001 | 0.00001 |
| 间接 CALL，16 目标 / 4 B 间距 | 0.00002 | 0.00001 |
| 间接 CALL，4 目标 / 64 B 同槽 | 1.25038 | 1.25037 |
| 间接 CALL，16 目标 / 64 B 同槽 | 1.62557 | 1.62557 |
| 间接 CALL，64 目标固定排列 | 0.75198 | 0.75197 |
| 间接 JMP，4 目标 / 16 B 间距 | 0.00001 | 0.00001 |
| 间接 JMP，4 目标 / 64 B 同槽 | 1.25037 | 1.25037 |
| 间接 JMP，64 目标固定排列 | 0.75197 | 0.75197 |

同样的 16 个 CALL 目标，从 4 B 间距改为 64 B 间距后，Block 指令由约 43 增至 63.6，目录探测从接近零变为约 1.626 次/迭代；FEX 仍约 34 条指令。数据直接支持局部地址索引冲突，而不是额外编译或 C++ resolver 热调用。

64 目标固定排列不是所有随机工作负载的代表。该样例 Block 的 cycles 低于 FEX，而 Function 略高；Block 的分支误预测明显更少，且各进程布局之间有变化。不能因为一组固定排列获胜，就推导局部容量冲突没有成本；其 Block/Function 指令数仍高于 FEX，详细 branch-miss 和样本范围保留在 `records.json`。

## 共享 RET 的补充配对实验

主批次后，依据机制审阅固定补充两个场景，各引擎同样交错 3 次，共 18 样本，单独预登记于 `return-fanin/plan.json`。四个 caller trampoline 的入口在两个 ELF 中相同，均分散到局部槽 0/1/2/3；它们直接调用同一个 callee，唯一 guest RET 固定为 `0x401602`。只改变四个 continuation 的地址，使返回目标分散到槽 4/5/6/7，或全部冲突于槽 4。其他选择、校验、循环及协议相同，默认 multiblock 配置不变。

| 同一 callee 的四个返回目标 | Block cycles | Function cycles | FEX cycles | Block 指令/次 | Function 指令/次 | FEX 指令/次 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 返回地址分散四槽 | 10.146 | 10.209 | 10.580 | 48.002 | 47.502 | 48.000 |
| 返回地址冲突同槽 | 15.898 | 15.849 | 10.578 | 67.502 | 67.002 | 48.000 |

这是一次间接 JMP + 直接 CALL + 共享 RET 的复合操作，只对这两个配对用例相互比较，不与前面的 CALL/JMP 表合算。无冲突时 SVM cycles 略低于 FEX；只改变返回地址布局后，Block/Function 增加约 56.7%/55.2%，FEX 基本不变。冲突用例中 SVM 约为 FEX 的 1.50 倍。

独立计数诊断显示，两边 SVM scope 的目录探测/迭代均从接近零升至约 1.5005，增加约 19.5 条指令。因 incoming JMP 仍分散，该差额定位在共享 RET 的局部缓存失配和目录探测。这个结果支持先修索引；引入 FEX 式返回栈的收益还应按调用形态评估，不能把所有 CALL 路径差额直接等同于返回栈成本。

## 机制解释与改进方向

1. 优先修正 site cache 索引。当前 `index=(target>>2)&15`，只有 16 个 direct-mapped 槽，不是 16-way。16/32/64 B 对齐的目标分别只用到 4/2/1 槽；先考虑低成本混合高位，或小型组相联策略，再针对当前冲突用例验证收益与单态命中成本。依据：`backend/arm64/abi.h:29`、`emit_control.cpp:134`。
2. CALL/RET 单独处理。FEX 通过 shadow `(guest return PC, host continuation)` 和 BL/BLR/RET 利用返回预测；SVM guest RET 仍走普通动态查表。CALL 对照的差距与这一机制差异一致，但不能将全部额外指令归因给 RET，guest 栈和 CFG 边界也在总路径内。需要保持真实 guest 栈/Uniform 合同，将控制流意图交给后端，而不是再引入一套镜像 IR。依据：`frontend/x86/semantics_control.inc:313` 与固定 FEX `JIT/BranchOps.cpp:138`。
3. 保留目前 inline ready 路径。普通局部命中已经没有 C++ 回调，也不先将 PC 写回后再重读；双方查表核心都约七条 A64 指令。无冲突 JMP 的实测支持这一点，没有证据要求重写整套 dispatcher。
4. 直接 JMP 对照的 Block/Function 差异主要说明编译单元与静态边界不同。动态和静态出口会改变 CFG 形成及发码，不能简单相减就声称得到精确 dispatcher 周期；FEX_MULTIBLOCK=0 还会影响返回 continuation 资格，不是等价的基本块开关。

## 墙钟与局限

CPU4 min/max 始终为 614400/1171200 kHz，没有修改频率、系统调度策略或其他服务。各轮记录显示部分样本墙钟被调度拉长，cycles 与指令数仍较稳定。例如 Block 的 4 目标无冲突 CALL，三次约 9.134/9.135/9.144 cycles，但墙钟为 15.602/11.067/7.986 ns。因此本轮派发排序以 PMU cycles 为主，以下原始墙钟中位数只完整保留，不用来替代 CPU 派发成本。

| 场景 | Block ns | Function ns | FEX ns |
| --- | ---: | ---: | ---: |
| 直接 CALL + RET 对照 | 6.840 | 6.959 | 5.483 |
| 间接 CALL，1 目标 | 7.837 | 7.821 | 6.529 |
| 间接 CALL，4 目标 / 4 B 间距 | 11.067 | 7.877 | 6.549 |
| 间接 CALL，16 目标 / 4 B 间距 | 7.978 | 8.199 | 6.756 |
| 间接 CALL，4 目标 / 64 B 同槽 | 12.920 | 13.182 | 6.658 |
| 间接 CALL，16 目标 / 64 B 同槽 | 13.427 | 13.742 | 6.691 |
| 间接 CALL，64 目标固定排列 | 11.040 | 15.506 | 14.895 |
| 直接 JMP 对照 | 8.145 | 3.624 | 2.647 |
| 间接 JMP，1 目标 | 5.714 | 5.507 | 5.555 |
| 间接 JMP，4 目标 / 16 B 间距 | 5.729 | 6.779 | 5.759 |
| 间接 JMP，4 目标 / 64 B 同槽 | 12.804 | 13.161 | 9.562 |
| 间接 JMP，64 目标固定排列 | 13.372 | 16.719 | 14.556 |

这是固定小型循环的热态派发测试，不是应用总耗时、冷编译成本或全套 CoreMark/FEX 成绩。真实代码的函数对齐、target 分布、返回扇入和分支预测都会影响结果。

源码入口：`benchmarks/indirect_dispatch/build.py`、`build_returns.py`、`guest.S.in`、`pmu_region.cpp`、`run.py`。实际源码/ELF SHA、环境、全部样本、诊断及机制行号审阅保留于 `/Volumes/Data2/swiftvm-middle-results-relocated-20261025/indirect-dispatch-20261107/`；`plan.json`、`comparison.json`、`records.json` 和 `diagnostic-records.json` 为主要入口；共享 RET 的配对数据在 `return-fanin/`，合计 126 个正式 PMU 样本。
