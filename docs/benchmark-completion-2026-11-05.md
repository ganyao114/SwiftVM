# 补齐 benchmark 覆盖及 FEX 对照 · 2026-11-05 UTC

此前未完成的 int、fp、mem、branch、call、func_tests、x87_bench 七项，现已在 SVM Block 和 requested Function 下完成原始输入的校验。CoreMark 同时通过。三方各项均采集三次运行和三次独立 compile，共 144 次有效观察。

本轮完成的是这些固定输入的覆盖与测量。运行质量仍有明显差距：FP 约为 FEX 的 1.80/1.86 倍，x87 约 2.42/2.40 倍，call 约 1.14/1.16 倍；x87 compile 约 2.84/2.08 倍，超过 2 倍目标。其余七项的两种 SVM scope compile 均在 2 倍内。

代码基于 `13ffa48f1d984cd7d600b5c93601301462b13570` **之后的未提交工作区**，不是旧提交原样重新测量。冻结源码为 118 个文件，digest `e1b33899baf9b85e104bc5703eccc389e89cf415f902df9552476c4cdcee769a`；测量前后逐文件 SHA 相同。SVM runner SHA `f159ce4a334f55c74d9937729a60f34c781cd2d0d333ab8490edfe4bb681e236`。原始工作树保持冻结，修改位于 `/Volumes/Data2/SwiftVM-submit-20261104`。

## 实现收口

| 文件 | 本轮负责的内容 |
| --- | --- |
| `frontend/x86/build_ir.cpp`、`decode.cpp`、`build_ir.h` | 补齐低/高半解包、packed PS/PD 四则、PS 比较、PACKUSWB、PMINUB、XMM 半搬、67 CALL，以及禁用 CET 时的精确 ENDBR/RDSSP 识别；发射既有中立 IR。REP STOS 形成真实可恢复边界。 |
| `backend/arm64/emit_vector.cpp` | 既有 VecF*、VecFCmpMask、VecPack、VecMin 的实际 packed 编码和独立 DAZ/FTZ 路径；解包及寄存器半搬复用 VecZip。 |
| `backend/arm64/compile.cpp`、`emit.h` | 既有 Goto/NotGoto/BindLabel 的真实 forward fixup、合流 PC/flags 事实清理、非末尾 StorePC 提交；接入既有 X87Op 消费者。 |
| `backend/arm64/helper_calls.cpp` | 复用 native helper 保存协议，连接 x87 C ABI，恢复客体 FP 环境和标志位。 |
| `execution/x87.cpp`、`ir/x87.h` | 真实 ext80、TOP/tag、控制字、状态字及有限命令协议。SoftFloat 显式局部环境，没有全局舍入状态。 |
| `execution/guest_state.h`、`runtime.cpp` | x87 状态追加到尾部、服务退出和布局连接；syscall R11 保留 DF。 |
| `externals/SoftFloat-3e/`、根 CMake | 只纳入 ext80/F64 必需的 22 个 C 实现及 8 个头文件，保留原版权和来源记录。 |
| 现有三组 rebuild 测试源 | 扩展现有前端/后端场景；增加有限的 REP 进度和 x87 精度/异常状态合同，不增测试平台。 |

表内路径均相对于 `source/runtime/`，测试位于 `source/tests/rebuild/`。GPR/XMM 仍为 LoadUniform/StoreUniform；IR 的 24B Operation/24B Block/8B Operand 和 16B Home 不变，没有新中间 IR 或分析 pass。GuestState 由 432B 变为 576B，原 GPR/PC/flags/FS/GS/XMM/MXCSR 偏移不变，尾部 144B 为 x87 状态。

REP STOS 每次执行先检查 RCX，完成真实 TSO store 后再提交 RDI/RCX/PC，保留 DF 和同步故障进度；forward fixup 只为实际边建立记录。x87 FLD m64 先做真实 IR load，再改变栈。当前 x87 未屏蔽异常由 runtime 明确停止，完整客体异常递送和 x87 内存存储事务仍在本子集之外。

ENDBR/RDSSP 只按当前未启用 CET 的合同识别精确形式；RDSSP 保留完整目的寄存器。Intel SDM 规定未启用 shadow stacks 时其行为为 NOP，见 [Intel 指令手册 RDSSPD/RDSSPQ](https://cdrdv2-public.intel.com/671110/325383-sdm-vol-2abcd.pdf)。真实 byte parser 仍被调用、仍按原边界计时。

## 验证和测量口径

最终冻结版本通过 Host 五组 CTest、Android 后端 264721 checks、native pipeline，以及八项 workload 的 Block/Function 校验。int/fp/mem/branch/call 沿用 scale=1 和 native checksum；func_tests 七行 stdout 一致、预期退出 101。CoreMark 100000 迭代均超过 10 秒，CRC 为 e714/1fd7/8e3a/d340；compile 单独取 20 迭代，final CRC 4983。

* x87 原始 benchmark 只规定 exit0，没有数值 stdout oracle；另以现有 pipeline 测试核实 ext80 的 `2 + 2^-52` 精度、PC53 舍入和未屏蔽空栈异常状态。其通过范围不能扩展为完整 x87 ISA 或异常模型等价。

红魔 NX729J/bf1359ff、CPU4/taskset10，每臂三次 fresh launcher、轮转顺序；未改频率和设备服务。min/max 一直为 614400/1171200 kHz，温度从 25.2°C 到 25.4°C。OS 页缓存保留，AOT/object cache 关闭，TSO 配置保留。PMU 为整个目标子进程的 user cycles/instructions/branch misses，含 cold JIT 和相同 wrapper；目标进程树外的 FEXServer 不计入。普通运行不启用 byte 时钟或诊断计数。

FEX 固定 revision `a156752fae3a4f65fefb1ed95af980bcdfac43d7` 和原 Android patch/config。普通 FEXLoader SHA `8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175`；独立 byte probe SHA `d2e4ce32b6c650bb9f7ce46a91aff8a098431ad3f5f52f52fd746d30b3500995`。双方 x87 路径均保留 ext80；本轮未开启 FEX reduced precision。

## 运行结果

单位 ms，均为三次独立中位数。CoreMark 使用内部循环 timer，其他项使用包含启动/JIT/wrapper 的 simpleperf elapsed，两种时间口径不混合汇总。

| 项目 | SVM Block | SVM Function | FEX | Block/FEX | Function/FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 16281.000 | 15659.000 | 15095.000 | 1.079× | 1.037× |
| int | 303.761 | 300.865 | 316.735 | 0.959× | 0.950× |
| fp | 472.660 | 488.856 | 262.446 | 1.801× | 1.863× |
| mem | 2145.252 | 2158.370 | 2049.272 | 1.047× | 1.053× |
| branch | 258.897 | 260.892 | 267.575 | 0.968× | 0.975× |
| call | 524.842 | 535.723 | 460.347 | 1.140× | 1.164× |
| func_tests | 110.685 | 149.548 | 139.598 | 0.793× | 1.071× |
| x87_bench * | 1979.379 | 1963.072 | 817.400 | 2.422× | 2.402× |

## Compile 结果

单位 ms，只扣真实 byte parser。形成、IR 构造、优化、分配、编码、失败/重试、发布和释放均计费；requested Function 包含全部实际 Block retry。独立侵入式诊断的成本口径与普通 PMU 分开；分项中位数不能直接相加。

| 项目 | SVM Block | SVM Function | FEX | Block/FEX | Function/FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 47.540 | 82.613 | 47.487 | 1.001× | 1.740× |
| int | 1.811 | 1.991 | 1.486 | 1.219× | 1.340× |
| fp | 2.427 | 2.681 | 1.663 | 1.460× | 1.612× |
| mem | 2.358 | 2.308 | 1.648 | 1.431× | 1.401× |
| branch | 1.652 | 1.996 | 1.634 | 1.011× | 1.222× |
| call | 1.821 | 1.946 | 1.561 | 1.166× | 1.247× |
| func_tests | 23.870 | 38.950 | 30.228 | 0.790× | 1.289× |
| x87_bench * | 0.802 | 0.589 | 0.283 | 2.835× | 2.081× |

CoreMark Function 的实际尝试由旧轮 722 Function + 24 Block retry 变为本轮 700 Function + 5 Block retry，25/42 spill slots 和 48/64B 最大 frame 保持原值。覆盖新形式减少了实际 retry；没有通过漏计失败或更改 scope 来降低 compile 数字。

## 尚存的性能成本

| 项目 | cycles / FEX（Block / Function） | instructions / FEX（Block / Function） |
| --- | ---: | ---: |
| coremark | 1.078 / 1.039 | 1.027 / 0.987 |
| int | 0.991 / 0.990 | 1.225 / 1.267 |
| fp | 2.125 / 2.197 | 3.254 / 3.271 |
| mem | 1.056 / 1.062 | 1.333 / 1.366 |
| branch | 1.002 / 1.010 | 1.138 / 1.158 |
| call | 1.198 / 1.219 | 1.277 / 1.287 |
| func_tests | 0.839 / 1.177 | 0.832 / 1.208 |
| x87_bench | 2.566 / 2.559 | 2.925 / 2.923 |

设备本轮 auxv 的 `AT_HWCAP2=0x26181`，AFP bit20 未置位，boot ID 前后一致。因此当前 runtime 选择软件 DAZ/FTZ 路径：`emit_vector.cpp:163` 的 packed 四则即使默认控制字未开启 DAZ/FTZ，也保留输入复制、控制读取、三次 TBZ 和结果复制；典型是八条编码，而固定 FEX 的 packed 算术消费者直接发对应 FOP。比较路径也有额外控制检查。这是确定的额外执行来源；本轮未测每项占比，不能把全部 FP 差距归到单一原因。独立 DAZ/FTZ 语义仍需保留。

x87 每条 X87Op 都经过 flags/pin 发布、FP 环境切换和 576B helper bank 保存/恢复；该 bank 是 native 寄存器保存区。实际 TOP/tag/物理栈状态按命令维护，inexact 时还用 toward-zero 再算一次以确定 C1。固定 FEX 则可用 x87 stack optimization 将 push/pop/FXCH 保持为 SSA 栈操作并延后同步。这里列的是源码可见结构成本，没有宣称已经测出其各自耗时。x87 compile 的绝对值为 0.802/0.589 ms 对 0.283 ms；这项仍未达到 2 倍目标。

## 证据与清理

[摘要和所有比值](evidence/benchmark-completion-20261105/summary.json)、[运行记录](evidence/benchmark-completion-20261105/runtime-records.json)、[compile 原始分项](evidence/benchmark-completion-20261105/compile-records.json)、[原始输出汇集](evidence/benchmark-completion-20261105/raw-output.jsonl)、[部署和源码身份收据](evidence/benchmark-completion-20261105/receipt.json)。原始 native golden、完整 smoke 结果、环境和能力值也保存在同目录。原七项输入和 golden 未改。

Data2 完整原始结果、脚本、冻结源码和唯一最终压缩 runner 位于 `/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/benchmark-completion-20261105/attempt8`。中途失败记录继续保留，已回收七个可再生成的旧 runner 压缩副本，共 15076090B。

手机私有目录已停止自己的 FEXServer，确认无活跃进程和打开文件后删除；allocated 回收 41149440B，df 实际增加 42393600B，目录剩余 0、无清理错误，见 [手机回执](evidence/benchmark-completion-20261105/phone-cleanup-receipt.json)。固定构建缓存继续复用，没有新提交或 push。
