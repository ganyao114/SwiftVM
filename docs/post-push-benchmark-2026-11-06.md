# 已推送版本与 FEX 的全 benchmark 对比：2026-11-06

本轮测量 subject 为 `ebf557408e14aef165b28bd7f0f3ebfc0ae3e918`，源码依赖 digest 为 `8cfb6b743bf838545951d348413cd501e6e1a1612644095aadd43a0d3a020977`（host/Android 并集 282 项）。八项固定输入，每项 Block／Function／固定 FEX 各五次；普通运行 120 个样本、独立 compile 120 个样本，全部输出和计时记录有效。

既定门槛通过 **46/48**：每输入、每种 scope 分别要求运行时间和用户态 cycles ≤1.10× FEX，compile 扣字节解析后 ≤2× FEX。

- 未过：x87_bench／block／compile，2.464003× FEX。
- 未过：x87_bench／function／compile，2.098255× FEX。

## 测量口径

红魔 NX729J，serial `bf1359ff`，CPU4／`taskset 10`。频率 min/max 继续使用 614400／1171200 kHz，未修改系统服务或频率策略；这不是锁频。每轮前后采集策略和温度，全部 min/max 一致。

每个样本启动新 launcher，无 AOT/object-code cache，保留 OS page cache。普通运行和 compile 诊断独立启动；预先固定五轮 arm 顺序，保留全部样本，没有按门槛追加、替换或与旧结果混池。

CoreMark 使用原 Bionic ELF、100000 次迭代、CRC d340，所有十五次均正式验证且 guest loop timer ≥10 秒；该行时间为 loop timer。其它七项时间是 whole-child simpleperf elapsed，包含启动和冷 JIT。用户态 PMU 也包含同一 child 树的启动和 JIT；独立 FEXServer 在树外。不能把这些时间窗口混为统一总分，或把整段指令计数直接当作稳态生成代码质量。

Compile 只扣实际 byte-parser 调用，保留适配、形成、IR 出生、失败／Function 回退、优化、分配、发射、复制、链接、发布与当前释放窗口。SVM 计入最终 native owner 物理释放；固定 FEX probe 只含 compile root 内逻辑工作缓冲释放，未计等价 native arena 最终销毁。该边界差异继续明确保留，未扣除 SVM 成本或给 FEX 补猜时间。

固定 FEX 为 `a156752fae3a4f65fefb1ed95af980bcdfac43d7 + original Android patch`，沿用原 Config、AVX 关闭与 TSO 设置。普通／probe Loader SHA256 分别为 `8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175`／`d2e4ce32b6c650bb9f7ce46a91aff8a098431ad3f5f52f52fd746d30b3500995`。候选 runner SHA256 为 `83e10c8fa6cb94dab777ce2c07c8882e2d9634bbc3393728a70641f3f7961946`。这些旧 workload 不构成新增 AVX/AVX2 的性能验收。

func_tests 校验完整 stdout 和 exit101；整数等五项校验原生 checksum。x87 benchmark 延用 exit-only oracle，exit0 不证明完整数值或 ISA 正确性；独立覆盖测试不能替代这个说明。当前同一 runner 的完整功能回归见[提交验收](submission-validation-2026-11-06.md)。

## 运行时间

单位 ms，各五次中位数。比值为本 scope 中位数／同批 FEX 中位数，越小越好。

| 输入 | Block ms | Function ms | FEX ms | B/FEX | F/FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 16089.000 | 15623.000 | 15091.000 | 1.0661 | 1.0353 |
| int | 302.682 | 303.206 | 315.887 | 0.9582 | 0.9599 |
| fp | 249.003 | 265.375 | 265.458 | 0.9380 | 0.9997 |
| mem | 2156.053 | 2170.887 | 2050.576 | 1.0514 | 1.0587 |
| branch | 258.628 | 259.695 | 268.634 | 0.9628 | 0.9667 |
| call | 481.886 | 476.435 | 461.231 | 1.0448 | 1.0330 |
| func_tests | 107.621 | 121.383 | 139.998 | 0.7687 | 0.8670 |
| x87_bench | 592.537 | 590.647 | 854.879 | 0.6931 | 0.6909 |

CoreMark 中位迭代速率，Block／Function／FEX：6215.43／6400.82／6626.47 iterations/s。

## 用户态 PMU

先分别取五次计数中位数，再相除。cycles 是既有门槛，instructions 和 branch misses 用于解释差距，不增加新的合格条件。

| 输入 | cycles B/FEX | cycles F/FEX | instructions B/FEX | instructions F/FEX |
| --- | ---: | ---: | ---: | ---: |
| coremark | 1.0670 | 1.0352 | 1.0378 | 1.0099 |
| int | 0.9918 | 0.9918 | 1.2242 | 1.2658 |
| fp | 0.9827 | 1.0176 | 0.9903 | 1.0054 |
| mem | 1.0586 | 1.0674 | 1.3327 | 1.3661 |
| branch | 1.0020 | 1.0091 | 1.1377 | 1.1574 |
| call | 1.0774 | 1.0710 | 1.2406 | 1.2498 |
| func_tests | 0.7805 | 0.9692 | 0.7877 | 1.0399 |
| x87_bench | 0.6783 | 0.6767 | 1.3754 | 1.3725 |

| 输入 | Block branch misses | Function branch misses | FEX branch misses |
| --- | ---: | ---: | ---: |
| coremark | 34,676,248 | 19,958,831 | 19,276,110 |
| int | 375,217 | 375,551 | 424,028 |
| fp | 383,473 | 499,373 | 427,034 |
| mem | 382,987 | 382,859 | 426,410 |
| branch | 376,467 | 376,580 | 425,405 |
| call | 376,653 | 375,954 | 425,253 |
| func_tests | 600,208 | 844,572 | 809,102 |
| x87_bench | 517,247 | 514,649 | 658,037 |

## Compile 总成本：仅扣字节 parser

单位 ms，各五次中位数；原始五个数、min/max 均保存在 summary。

| 输入 | Block ms | Function ms | FEX ms | B/FEX | F/FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 42.436354 | 50.731396 | 47.514479 | 0.8931 | 1.0677 |
| int | 1.956767 | 1.824363 | 1.474687 | 1.3269 | 1.2371 |
| fp | 2.479478 | 2.469942 | 1.682760 | 1.4735 | 1.4678 |
| mem | 2.269159 | 2.251039 | 1.588073 | 1.4289 | 1.4175 |
| branch | 1.853710 | 1.875097 | 1.511198 | 1.2266 | 1.2408 |
| call | 1.974419 | 1.924049 | 1.576823 | 1.2522 | 1.2202 |
| func_tests | 20.922688 | 33.343956 | 30.193021 | 0.6930 | 1.1044 |
| x87_bench | 0.709171 | 0.603904 | 0.287813 | 2.4640 | 2.0983 |

### coremark：总成本中位样本的阶段分解

每列只取总 compile 成本排序的第三个真实样本，所有分项来自该同一样本，不把独立分项中位数相加。SVM“其它”是从总量扣已计时阶段的残差，不是独立测量的归因。

| 阶段 | Block ms | Function ms | FEX ms |
| --- | ---: | ---: | ---: |
| 形成＋IR 出生（扣字节） | 11.466978 | 17.050347 | 23.143229 |
| IR 优化 | 8.395010 | 11.501031 | 6.339271 |
| 分配＋编码 | 14.165938 | 17.264885 | 13.880156 |
| 复制／链接／发布 | 6.446641 | 4.018118 | 3.081927 |
| 当前释放窗口（owner 边界不同） | 0.415052 | 0.330521 | 0.351615 |
| 其它／残差 | 1.546735 | 0.566494 | 0.718281 |
| 总 compile 去字节 | 42.436354 | 50.731396 | 47.514479 |
| 所选 round（零基） | 4 | 1 | 2 |

### x87_bench：总成本中位样本的阶段分解

每列只取总 compile 成本排序的第三个真实样本，所有分项来自该同一样本，不把独立分项中位数相加。SVM“其它”是从总量扣已计时阶段的残差，不是独立测量的归因。

| 阶段 | Block ms | Function ms | FEX ms |
| --- | ---: | ---: | ---: |
| 形成＋IR 出生（扣字节） | 0.233545 | 0.282343 | 0.117135 |
| IR 优化 | 0.064688 | 0.087083 | 0.041094 |
| 分配＋编码 | 0.136563 | 0.079740 | 0.101875 |
| 复制／链接／发布 | 0.182709 | 0.063020 | 0.020104 |
| 当前释放窗口（owner 边界不同） | 0.069428 | 0.077344 | 0.002292 |
| 其它／残差 | 0.022238 | 0.014374 | 0.005313 |
| 总 compile 去字节 | 0.709171 | 0.603904 | 0.287813 |
| 所选 round（零基） | 0 | 4 | 1 |

## 当前生成资源

以下为 CoreMark20 compile 诊断中的实际请求集合。每项显示五轮的唯一值或范围；不是每条客体指令的成本，也不拿 Block 与 Function 的不同请求范围作直接优劣判定。

| 指标 | Block | Function |
| --- | ---: | ---: |
| compile_attempts | 2041 | 676 |
| fallbacks | 0 | 0 |
| emitted_guest_instructions | 12383 | 16449 |
| ir_nodes | 36575 | 50739 |
| operand_words | 65717 | 84868 |
| native_bytes | 388764 | 394016 |
| spill_slots | 25 | 33 |
| max_frame_bytes | 48 | 64 |
| fault_sites | 5072 | 6768 |
| static_links | 2878 | 944 |
| peak_slabs | 1 | 1 |
| peak_code_virtual_bytes | 2097152 | 2097152 |

八项负载的 Block／Function 共 16 组，IR 节点、operand words、native bytes、spill、frame、fault site 六项统计的取值集合，均与提交整理前的已测快照一致；对应比较保存在 resource-comparison-to-preformat.json。这只说明这些生成统计保持一致，不把计数一致扩大为完整机器码或语义等价证明。

## 历史与证据边界

此前完整 runtime/PMU 锚点是[11 月 5 日的 390cf73d](post-push-benchmark-2026-11-05.md)，此前 compile 优化结果是[提交整理前的源码快照](compile-cost-optimization-2026-11-06.md)。它们不与本批样本混合，也不能把跨批绝对时间变化直接归因于某次代码修改。

本轮原始目录：`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/post-push-benchmark-20261106`。小型汇总、身份、完整 runtime/compile 记录及阶段分解见 [evidence](evidence/post-push-benchmark-20261106/README.md)。当前源码、压缩保留源码和部署前后输入 SHA 均已核对；105 条有 live PID affinity，另外 15 条使用 wrapper CPU4 与子进程 affinity 继承记录，没有补造 live 状态。

手机独立 namespace 已检查无活动进程或打开文件后清理，剩余 0B，实际 df 释放 42278912 字节。所有本轮结果、候选压缩包和源码压缩包均写 Data2，复用固定构建缓存，没有新建系统盘构建场。
