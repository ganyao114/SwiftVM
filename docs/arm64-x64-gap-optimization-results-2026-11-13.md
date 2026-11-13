# ARM64 → x64 性能差距优化实施结果

日期为 2026-11-13。实施基于 `af7671f6b5563e94a30ded7b3992163e8f9558bc`，完成方案批次 0—5 和批次 6 的末次 RHS 复用。15 项负载中 11 项快于固定 Dynarmic，原有七项领先负载的配对耗时增幅均低于 2%。[实施方案](arm64-x64-gap-optimization-plan-2026-11-12.md)、[验收摘要](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/summary.json)

## 1. 实际实现

| 批次 | 落地内容 |
| --- | --- |
| 0 | 完整 Git 构建快照、冻结基线与候选、逐文件源码校验和补丁重放验证 |
| 1 | Known x64 的零高位标量 SIMD 写入统一为整 Q 的 V128 SSA 写入，复用现有 uniform 转发与写回消除 |
| 2 | LAHF／OF 快照按有效 16 位读取、更新和恢复，raw RFLAGS 保持 64 位访问 |
| 3 | native／saved NZCV 逐位掩码、部分生产者直接条件消费、混合来源保存与 fault 恢复 |
| 4 | 精确三元、一元、转换、比较及位操作 IR，显式 SSA 冷输入，稳定 FPCR 专门化与入口守卫，浮点 EFLAGS 条件载体 |
| 5 | AVX 独立特征检测、精确 NaN／MinNormal 合并检查、特征身份与 SSE4.1／SSE2 路径 |
| 6 | U32／U64 的 Add／And／Or／Xor 复用末次 RHS，结合现有 resident home 和 spill 规则减少搬运 |

标量 SIMD 审计覆盖加载、FMOV、FABS／FNEG、转换、标量 lane、shift、permutation 和 SHA1H。整 Q 写入通过现有 BitCast／MOVQ 清零高位，低于 64 位的输入先规范化宽度。插入与保留 lane 的既有语义继续通过相应写入入口表达。[semantics_registers.inc](../source/runtime/frontend/arm64/semantics_registers.inc)、[semantics_float.inc](../source/runtime/frontend/arm64/semantics_float.inc)

新增 `ArmFPTernary`、`ArmFPUnary`、`ArmFPConvert`、`ArmFPCompare` 和 `ArmFPBitUnary`，扩展 `ArmFPBinary` 支持标量除法。新 opcode 追加在原编号之后，参数保持现有五项上限。FPCR／FPSR 的精确范围进入原有 effects 分析。比较使用 Void 数据结果和固定 NZCV 写入，前端直接生成该操作，活跃性与后端共同识别固定写入。[opcodes.inc](../source/runtime/ir/opcodes.inc)、[uniform_effects.h](../source/runtime/ir/uniform_effects.h)、[liveness.cpp](../source/runtime/ir/passes/liveness.cpp)

冷叶子 `EvaluateFPValues` 接收原始 SSA 位模式、描述符、FPCR 和 FPSR 位置。实现把输入装入局部 GuestState，规范化描述符中的寄存器编号，复用已验证的 `ExecuteNumeric` 数学语义。不同 SSA home、相同 guest 寄存器字段以及 Rd／Rn 为 31 的情况都有定向回归。相同规范化描述符、状态偏移、活跃保存集合和 flags 策略共享调用 stub。未知或可变 FPCR 保留逐站点的精确失败出口。[fp_scalar.cpp](../source/runtime/arch/arm64/fp_scalar.cpp)、[emit_float.cpp](../source/runtime/backend/x64/emit_float.cpp)

浮点比较直接保留 COMIS／UCOMIS 的 CF／ZF／PF 载体，按 ARM 条件真值表生成分支。GE／LT、GT／LE、VS／VC 使用单个原生条件，其余条件捕获并组合所需谓词。在 fault、helper、整数 flags 合并和单元出口按需求规范化。pending MXCSR 的收割继续服从现有精确边界。[emit_flags.cpp](../source/runtime/backend/x64/emit_flags.cpp)、[emit_arm_float.cpp](../source/runtime/backend/x64/emit_arm_float.cpp)

## 2. 构建身份与源码校验

| 项目 | 身份 |
| --- | --- |
| Git 基线 | `af7671f6b5563e94a30ded7b3992163e8f9558bc` |
| 原冻结生产 CodeBuildId | `045cbc5e2551348c2c212350f19a834302969b0afbf7c7cbb58613e281d50d42` |
| 本轮重建基线 CodeBuildId | `b7fb23a60c03c53ed7a5bf54c1ccd799136f906b49f136c64c47f041d245494f` |
| 最终候选 CodeBuildId | `81131f17ec78743d92c1f4d2b284ace59476545517f65d101f9aef894ceae3e0` |
| 有效 x64 feature mask | 基线 511，候选 1023，新增独立 AVX 位 512 |
| 工具链 | GCC 13.3.0、CMake 3.28.3、Ninja 1.11.1 |

原工作区 `source/runtime/externals` 和 `source/runtime/frontend/arm32` 仍有目录读取故障。构建使用 Git 恢复的完整快照，再覆盖本轮修改。共核对 1,723 个可读跟踪文件，另外 311 个文件来自 Git 基线。比较先检查原始字节，再检查 CRLF→LF 等价性，差异为零。37 个修改文件的补丁已在独立基线文件集上通过 `git apply --check`，重放后逐文件散列与最终构建快照相同。[源码校验](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/source-verification.json)、[补丁重放校验](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/patch-verification.json)

运行时主表和完整程序冷编译使用本轮重建基线。N／2N／4N 规模对照沿用冻结的 `045…` 生产 scaling 二进制，其输入使用裸 Compile 与既有 FP region 合同。构建为 Release／IPO，保留链接所需的 `-latomic`。[构建及二进制清单](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/manifest.json)

## 3. 正确性验收

| 范围 | 最终结果 |
| --- | --- |
| CTest | 31 通过、7 跳过、0 失败 |
| ARM64／ARM32 基础 oracle 对照 | 4,224 次原生执行，全部一致 |
| 标量浮点 oracle 对照 | 27,648 次原生执行，数值、FPSR 与状态一致 |
| NEON／indexed 浮点 oracle 对照 | 25,296 次原生执行，全部一致 |
| x86 冻结基线全字段对照 | Block／Function 共 1,278 条结果一致 |
| 完整程序 | ARM64／ARM32／Thumb 各 8 组，共 24 组输出一致 |
| 磁盘缓存与 AOT | 三种 guest 均通过，暖缓存新增编译请求为零 |

新增定向回归覆盖部分 NZCV 的 14 种条件、全部旧值与部分写掩码，raw／LAHF、carry 极性、mixed fault recipe 和槽位高字节污染。浮点覆盖 32 FPCR 域、AH／可变控制、精确冷输入、F32／F64、FMA 特征组合、SSE2／SSE4.1／AVX、非对齐代码基址、NaN／MinNormal、FZ 除法以及浮点比较后的 fault。共享 stub 回归使用不同架构寄存器编码与相同显式输入集合，检查只生成一个数学叶子重定位，并验证 NaN、FZ、结果、FPSR 和 flags。[x64_compile_test.cpp](../source/tests/rebuild/x64_compile_test.cpp)、[runtime_engine_test.cpp](../source/tests/rebuild/runtime_engine_test.cpp)、[ir_direct_test.cpp](../source/tests/rebuild/ir_direct_test.cpp)、[arm64_frontend_test.cpp](../source/tests/rebuild/arm64_frontend_test.cpp)

x86 每种 scope 包含 624 个原生用例与 15 个既有未原生用例。oracle 分类仍包含每种 scope 的 100 个 alignment gap 和 406 个 x87 gap，全部 639 个用例另作 outcome、state、memory、service 与 fault 字段对照，差异为零。七个 CTest skip 对应当前 x64 宿主上的其他宿主 ISA 执行测试，其 portable compile 合约均通过。[ARM 摘要](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/final-correctness-summary.json)、[x86 对照](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/x86-block-raw-diff.json)、[CTest](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/ctest.xml)、[完整程序](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/final-programs-summary.json)

## 4. 热执行

环境为 Ryzen 9 7950X3D、WSL `ProtonHM`、CPU2。SwiftVM 使用 Function／pin0，Dynarmic 固定为 `9d4582339990d4eae53f1dc7160686920fc2075c`，沿用正常 block linking、safe optimizations、直接内存访问与关闭 cycle counting 的设置。基线、组合检查点、最终候选及 Dynarmic 按固定顺序轮转并反转，各执行 31 个新进程，每进程 3 次热执行。

| 负载 | 基线 ms | 候选 ms | Dynarmic ms | 配对候选／基线 [Q1, Q3] | 配对候选／Dynarmic |
| --- | ---: | ---: | ---: | --- | ---: |
| 整数 | 7.751 | 7.722 | 12.804 | 0.997 [0.990, 1.008] | 0.602 |
| 访存 | 13.546 | 13.534 | 16.225 | 1.000 [0.994, 1.005] | 0.834 |
| 移位 | 10.952 | 10.901 | 16.372 | 0.995 [0.981, 1.013] | 0.668 |
| 普通浮点 | 2.961 | 2.946 | 4.488 | 0.994 [0.985, 1.007] | 0.653 |
| 综合计算 | 4.769 | 4.413 | 4.092 | 0.925 [0.915, 0.936] | 1.078 |
| 浮点展开 ×8 | 1.281 | 1.281 | 2.569 | 1.000 [0.982, 1.015] | 0.499 |
| 向量矩阵 | 0.212 | 0.158 | 0.122 | 0.751 [0.734, 0.761] | 1.296 |
| 标量矩阵 | 1.622 | 0.339 | 0.431 | 0.209 [0.206, 0.211] | 0.790 |
| SHA | 3.468 | 3.425 | 3.091 | 0.982 [0.972, 1.004] | 1.107 |
| 排序 | 0.427 | 0.424 | 0.611 | 0.991 [0.982, 1.043] | 0.697 |
| cond | 67.208 | 33.940 | 37.047 | 0.507 [0.502, 0.511] | 0.922 |
| 分离比较与分支 | 21.709 | 21.642 | 24.487 | 1.001 [0.995, 1.006] | 0.882 |
| fpmix | 20.922 | 3.895 | 7.045 | 0.187 [0.186, 0.188] | 0.555 |
| 综合计算 FMA | 4.863 | 4.548 | 4.183 | 0.940 [0.924, 0.951] | 1.089 |
| 矩阵 FMA | 0.203 | 0.119 | 0.122 | 0.587 [0.580, 0.593] | 0.971 |

耗时列为各进程热执行中位数的中位数，配对比值按同轮进程计算。每次执行均校验输出、暖编译复用，以及零 fallback、解释执行和数据回调。组合检查点包含批次 1+2+3+5，最终候选再加入批次 4+6。在同一轮中，最终／检查点的配对比值为 fpmix 0.390、矩阵 FMA 0.728、综合计算 FMA 约 0.97、SHA 0.989。单项贡献以相应组合检查点为边界。[逐进程记录](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/runtime-measurements.json)、[检查点记录](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/checkpoint-measurements.json)

cond 确认组为 31 个新进程、每进程 9 次热执行。基线／候选／Dynarmic 分别为 69.357／34.866／38.668 ms，配对候选／基线为 0.506 [0.503, 0.511]，候选／Dynarmic 为 0.919 [0.914, 0.926]。[cond 确认组](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/cond9-measurements.json)

短矩阵确认组为 31 个新进程、每进程 99 次热执行。向量矩阵候选／Dynarmic 为 1.304 [1.289, 1.315]，矩阵 FMA 为 0.962 [0.953, 0.977]，与主表方向一致。[矩阵长窗口确认组](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/matrix99-measurements.json)

## 5. 冷编译与代码量

完整 ARM64 程序使用 31 组成对新进程，交替 AB／BA，CPU2。编译请求和发布单元均为 12。

| 阶段 | 基线 ms | 候选 ms | 配对候选／基线 |
| --- | ---: | ---: | ---: |
| 总编译时间 | 2.242 | 2.255 | 1.026 |
| frontend | 1.112 | 1.059 | 0.971 |
| optimize | 0.295 | 0.293 | 0.994 |
| backend | 0.654 | 0.728 | 1.108 |
| publication | 0.080 | 0.086 | 1.088 |
| 发布代码字节 | 35,932 | 42,724 | 1.189 |

总编译时间配对区间为 1.026 [0.991, 1.050]，最终中位数增加 2.6%，低于方案的 3% 复查线。backend 仍增加约 10.8%，发布代码量增加 18.9%，是本轮的主要编译成本。该程序和十五项 callable kernel 使用各自独立的编译输入。[冷编译原始记录](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/compile-cost.json)

首次完整候选测得总编译时间增加 4.3%，触发复查。数值冷路径的调用准备和失败退出存在重复发码，随后增加按规范化描述符共享的 stub，并省去稳定支持域中重复的失败出口。相同完整程序的代码量从 44,404 降至 42,724 字节。最终数据来自修正后的冻结二进制。[共享前记录](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/pre-sharing-compile-cost.json)

## 6. 规模与内存

以下为 11 组成对新进程，每组每规模 9 个序列，每序列 8 次构建、优化与编译请求。表内为同轮每请求时间比值的中位数。

| 输入 | N=256 | 2N=512 | 4N=1024 |
| --- | ---: | ---: | ---: |
| uniform | 1.003 | 1.034 | 1.025 |
| pressure | 1.026 | 1.050 | 1.046 |
| flags | 1.043 | 1.023 | 1.031 |
| FP | 0.989 | 0.991 | 1.007 |
| cold | 0.996 | 0.990 | 0.997 |
| CFG | 1.050 | 1.038 | 1.013 |

六类 4N 输入的增幅均低于 5%。`Home` 维持 28 字节，`Allocation` 维持 184 字节，`reuse_rhs` 使用原有 padding。18 组相同输入的 IR payload、operand words 与已知分配向量容量相同，保留原有优化遍历。[规模数据](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/scaling-cost.json)、[结构尺寸](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/home-size.json)

另以每输入 3 对新进程测量内存，单进程只运行一次构建、优化、编译及结果验证。4N 的最大进程 RSS 中位数为基线 5,392 KiB、候选 5,372 KiB。RSS 覆盖 loader、可执行页、IR、编译临时对象和验证代码，allocator 已知容量另行记录。该规模工具使用既有裸后端 FP region 输入，新增数值 IR 的图变化由实际负载单元记录补充。[内存与 IR 容量记录](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/memory-metadata.json)

### 新增数值 IR 的规模验证

另比较旧 `CallLambda(ExecuteNumeric)` 数值图与新 `ArmFPTernary` SSA 数值图，使用稳定 FPCR=0 的裸后端标量 FMA。有限输入与 sNaN 冷输入分别覆盖 N／2N／4N，每类 11 对新进程，每进程 7 个序列，每序列 4 次构建、优化及编译请求。执行验证同时检查数值、零高位、FPSR 和 NZCV。

| 输入 | N=256 | 2N=512 | 4N=1024 |
| --- | ---: | ---: | ---: |
| 数值 FMA 的配对编译时间比 | 0.731 | 0.711 | 0.689 |
| 数值 FMA 冷输入的配对编译时间比 | 0.733 | 0.729 | 0.703 |

4N 图的物理节点从 3,074 降至 2,052，代码从 299,552 降至 147,584 字节，数学叶子重定位从 1,024 个调用站点降至 2 个共享 stub。两个保存集合来自交替使用的向量 home。该输入检验新增数值 IR 与共享冷调用的规模行为，完整程序累计编译成本仍采用第 5 节的数据。[新增数值规模数据](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/numeric-scaling.json)

## 7. 发码、PMU 与剩余差距

cond 的部分生产者后直接使用 JA／JL。fpmix 的 UCOMISD 后直接使用 JA。向量结果普通路径为 VANDPD、VCMPEQ_UQPD、VPMOVMSKB、JRCXZ，保留精确 NaN／MinNormal 触发集合。标量矩阵 IR 中，LoadMemory U64 后构造 BitCast V128 并整 Q 写入。[热路径摘录](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/hot-paths.txt)

| 负载 | IR 节点基线／候选 | LoadUniform 基线／候选 | GPR MOV 基线／候选 | 真实 spill 值基线／候选 |
| --- | ---: | ---: | ---: | ---: |
| 标量矩阵 | 351／333 | 43／34 | 133／169 | 0／0 |
| fpmix | 88／67 | 9／9 | 76／33 | 0／0 |
| cond | 71／71 | 12／12 | 25／19 | 0／0 |
| 向量矩阵 | 1,114／1,093 | 69／62 | 495／549 | 0／0 |
| 综合计算 | 3,959／3,937 | 183／177 | 901／927 | 22／21 |
| 综合计算 FMA | 3,843／3,853 | 207／182 | 744／665 | 25／25 |
| SHA | 2,563／2,562 | 85／86 | 345／320 | 25／24 |
| 矩阵 FMA | 1,014／1,025 | 95／67 | 334／280 | 0／1 |

静态发码计数覆盖完整发布单元的入口、热区和冷区，排除链接数据与尾部 FP 常量。spill 按既有诊断 allocation 重放的 home 位置统计。标量矩阵的主要改善来自状态表示和依赖链，全单元 MOV 总数仍会受到冷路径与边界代码影响。[发码及分配统计](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/analysis.json)

PMU 使用主表每引擎、每负载前 7 个新进程的三次暖执行。全部满足 event enabled time 等于 running time。计数器在每次执行前 RESET，暖记录直接表示该次事件数，enabled／running 时间保持累计。下表为各引擎进程中位数的中位数之比。

| 剩余负载 | 候选／Dynarmic 周期 | 指令 | 分支 |
| --- | ---: | ---: | ---: |
| 向量矩阵 | 1.276 | 1.311 | 1.034 |
| 综合计算 | 1.075 | 1.297 | 1.234 |
| 综合计算 FMA | 1.092 | 1.295 | 1.242 |
| SHA | 1.103 | 1.379 | 1.145 |

向量矩阵仍有精确后查和状态搬运成本，周期与指令比值接近。综合计算、综合计算 FMA 和 SHA 仍分别保留 21、25、24 个真实 spill 值，指令数比值也继续偏高。现有数据支持继续审计 scratch 占用、调用保存集合和跨块状态写回，具体微架构停顿属于推断。数学冷叶子的真实动态调用频率仍需专项测量。[PMU 原始数据与反汇编](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/benchmark-raw.tar.gz)

SHA 本轮配对改善约 1.8%，尚未达到方案争取的额外 5%。扩大寄存器池与跨块 guest 状态保留按方案第 9 节作为后续独立批次。标量矩阵、cond、向量矩阵和矩阵 FMA 的本轮耗时目标均已达到。

## 8. 归档与复现

最终源码补丁可应用于上述 Git 基线。完整构建快照与可读工作区的等价性、最终工具散列、逐进程数据、输入与 oracle、测试日志、反汇编及构建脚本均已归档。组合检查点保留冻结二进制、测试日志和配对结果，最终源码由独立补丁重放验证。[源码补丁](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/implementation.patch)、[归档清单](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/manifest.json)

| 归档 | 内容 |
| --- | --- |
| [validation-raw.tar.gz](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/validation-raw.tar.gz) | 完整最终观察结果、输入、oracle、程序与缓存／AOT 验收 |
| [benchmark-raw.tar.gz](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/benchmark-raw.tar.gz) | 31 组热执行、cond／矩阵确认组、PMU、IR 与 native dump |
| [tools-build-and-checkpoints.tar.gz](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/tools-build-and-checkpoints.tar.gz) | 冻结工具、源码、构建与测试日志、规模原始数据和组合检查点 |
| [numeric-scaling-raw.tar.gz](evidence/arm64-x64-residual-20261112/gap-implementation-20261113/numeric-scaling-raw.tar.gz) | 新旧数值 IR 的规模探针源码、二进制、构建命令与逐序列数据 |

档案中的脚本保留实际执行路径。跨目录复现时，按清单映射源码、构建、corpus 与 oracle 根路径，并核对输入和二进制散列。运行性能与编译成本分别在构建和全量验证结束后独立测量。
