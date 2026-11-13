# ARM64 → x64 残余优化实施结果

日期为 2026-11-12。实施基于 `ba00658d` 工作区，生产基线对应 `98e4d829`，诊断工具对应 `0297f745`。完成批次 0—7，并将实现边界回写方案。[实施方案](arm64-x64-residual-optimization-plan-2026-11-12.md)

## 1. 已落地的机制

| 批次 | 实际实现 |
| --- | --- |
| 0 | ASR 掩码后零位移归一化、AOT flush 域选择、可变 FPCR 精确回退、linked flags 请求校验、Bool 字节范围与编译失败清理。增加条件、分离比较、混合浮点与 FMA 基准 |
| 1 | ARM64 native 环境累计 pending MXCSR 状态，helper、FPSR 观察、退出与 fault 收割到 FPSR。忽略 DE 并清除全部低 6 位 |
| 2 | FZ=0 的 SSE4.1 整向量 NaN／MinNormal 后查，SSE2 按活跃 lane 检查，F32／F64 非活跃位清零，精确冷回退 |
| 3 | Known x64 的 14 种复合条件直接 CondSet，终结零测试与位测试直接分支，CBZ 可使用保留 flags 的 JRCXZ |
| 4 | 逐节点 flags 需求、SETcc 条件缓存、native fault recipe、循环入口约定、LAHF／OF 快照及 raw RFLAGS 回退、packed 权威条件路径 |
| 5 | ROR32、严格类型的恒等折叠、R32 逻辑运算、resident RHS、LEA 保留 flags、直接 V128 uniform 搬运、立即数存储和重复同值写消除 |
| 6 | 128 项 epoch 转发表，按字节处理重叠与 V256 pending store，稳定 uniform 槽支撑装载及相邻写回的计算结果 |
| 7 | 标量比较、标量／NEON／indexed FMA、整数转换、FDIV、FSQRT 和 F32／F64 精度转换的原生热路径与精确冷路径 |

flags 读取分类由 IR 共享函数提供，fault recipe 解码与镜像校验共享校验函数。需求分析在跨块 SSA 使用统计之后执行。内部标签合流清除路径上的条件缓存，回归测试主动污染条件槽来检验跳过捕获的路径。[flag_effects.h](../source/runtime/ir/flag_effects.h)、[allocate.cpp](../source/runtime/backend/x64/allocate.cpp)、[emit_control.cpp](../source/runtime/backend/x64/emit_control.cpp)

## 2. 正确性验收

| 项目 | 结果 |
| --- | --- |
| CTest | 31 通过、7 跳过、0 失败 |
| ARM64／ARM32 基础差分 | 4,224 次原生执行，逐项一致 |
| 标量浮点差分 | 27,648 次原生执行，数值与 FPSR 一致 |
| NEON／indexed 浮点差分 | 25,296 次原生执行，数值与 FPSR 一致 |
| x86 基线回归 | Block／Function 共 1,278 条完整结果字段一致 |
| 完整程序 | ARM64／ARM32／Thumb 各 8 组，共 24 组输出一致 |
| 磁盘缓存与 AOT | 三种 guest 冷写、暖命中与 AOT 执行通过，暖缓存编译请求为零 |
| FPCR 域 | 32 个组合的创建、往返与第二轮复用通过，第二轮新增编译为零 |

ARM 基础矩阵覆盖 Block／Function、Known／Unknown／EarlyOff 和优化开关。新增定向回归枚举全部 32 个 FPCR 域、SSE2／SSE4.1 后查、raw／LAHF flags、14 种条件乘 16 种 NZCV、ADC／SBB、helper 返回值、fault 前 pending FPSR、跨块条件 SSA、uniform 别名与真实 spill。[x64_compile_test.cpp](../source/tests/rebuild/x64_compile_test.cpp)、[runtime_engine_test.cpp](../source/tests/rebuild/runtime_engine_test.cpp)、[ir_optimize_test.cpp](../source/tests/rebuild/ir_optimize_test.cpp)

x86 每种 scope 有 624 个原生用例与 15 个既有未原生用例。oracle 分类仍包含每种 scope 的 100 个 alignment gap 与 406 个 x87 gap，本次对全部 639 个用例另作基线全字段比较，差异为零。七个 CTest skip 属于当前 x64 宿主上的其他宿主 ISA 执行测试，对应 portable compile 合约均通过。[验收摘要](evidence/arm64-x64-residual-20261112/implementation/summary.json)、[CTest 记录](evidence/arm64-x64-residual-20261112/implementation/ctest-final.xml)

## 3. 热执行

环境为 WSL `ProtonHM`、Ryzen 9 7950X3D、Release／IPO、CPU2。SwiftVM 使用 Function／pin0，参考为固定 Dynarmic `9d4582339990d4eae53f1dc7160686920fc2075c`。基线、候选与 Dynarmic 轮换并反转顺序，各 31 个新进程，每进程 3 次热执行取中位数。每次校验输出、暖编译复用和零 fallback／解释执行。

| 负载 | 基线 ms | 候选 ms | Dynarmic ms | 配对候选／基线中位数 [Q1, Q3] |
| --- | --- | --- | --- | --- |
| 整数 | 7.925 | 7.899 | 13.137 | 0.995 [0.983, 1.012] |
| 访存 | 18.394 | 13.028 | 15.577 | 0.712 [0.702, 0.717] |
| 移位 | 10.467 | 10.548 | 15.880 | 1.003 [0.993, 1.021] |
| 浮点 | 9.167 | 2.844 | 4.339 | 0.309 [0.305, 0.314] |
| 综合计算 | 8.140 | 4.746 | 4.054 | 0.576 [0.560, 0.599] |
| 浮点展开 ×8 | 1.550 | 1.266 | 2.577 | 0.814 [0.804, 0.828] |
| 向量矩阵 | 0.730 | 0.207 | 0.121 | 0.284 [0.280, 0.297] |
| 标量矩阵 | 2.419 | 1.563 | 0.415 | 0.644 [0.639, 0.649] |
| SHA | 6.128 | 3.384 | 3.051 | 0.552 [0.544, 0.565] |
| 排序 | 0.760 | 0.415 | 0.579 | 0.548 [0.527, 0.557] |
| 复合条件 | 258.682 | 65.262 | 35.894 | 0.253 [0.250, 0.255] |
| 分离比较与分支 | 61.520 | 20.370 | 22.924 | 0.331 [0.328, 0.333] |
| 混合浮点 | 135.538 | 20.386 | 6.908 | 0.151 [0.148, 0.155] |
| 综合计算 FMA | 18.421 | 4.526 | 3.912 | 0.246 [0.243, 0.248] |
| 矩阵 FMA | 11.248 | 0.186 | 0.112 | 0.017 [0.016, 0.017] |

表内耗时取进程中位数的中位数，配对比值按同轮进程计算，保留全部离散轮次。新增五种输入同时以 QEMU 和独立结果计算验证。FMA 变体使用 `-ffp-contract=on`，原有输入使用冻结 ELF。[原始配对记录](evidence/arm64-x64-residual-20261112/implementation/raw-evidence.tar.gz)

### x86 flags 策略

CoreMark 策略实验中，mixed native-flags 路径相对 `52f04abd` 生产基线仍有约 15% 成本，VM 对 x86 guest 选择 packed flags 和 packed 调度 ABI。ARM32／ARM64 使用 native flags，公共裸后端支持显式 local flags。

最终 31 组配对的 x86 CoreMark elapsed time 中位数为旧基线 145.367 ms、机制基线 343.088 ms、候选 141.635 ms。候选／旧基线为 0.973 [0.959, 0.985]，候选／机制基线为 0.414 [0.409, 0.417]。

该实验固定官方 CoreMark 源码，每进程先完整预热一次，再计时一次，使用 500 iterations、2K 数据和单线程。校验两次调用的全部 CRC。单次计时短于官方有效分数要求，原始 stdout 保留其时长检查提示，结果按暖循环 ABI 耗时比较报告。[策略实验与最终记录](evidence/arm64-x64-residual-20261112/implementation/raw-evidence.tar.gz)

## 4. uniform 转发与 spill

以下三组诊断 runner 使用相同当前实现，分别只替换缓存容量和 uniform 支撑开关。每格依次表示 `LoadUniform 数／真实栈 spill 值数／uniform 支撑的淘汰值数`。真实栈 spill 按 allocator 的实际位置统计。

| 负载 | 8 项，无 uniform 支撑 | 128 项，无 uniform 支撑 | 128 项，有 uniform 支撑 |
| --- | --- | --- | --- |
| 综合计算 | 435／11／0 | 183／57／0 | 183／22／35 |
| SHA | 191／12／0 | 85／39／0 | 85／25／14 |
| 向量矩阵 | 206／0／0 | 69／17／0 | 69／0／17 |
| 综合计算 FMA | 336／12／0 | 207／40／0 | 207／25／15 |
| 矩阵 FMA | 119／0／0 | 95／0／0 | 95／0／0 |

稳定性证明限定在单块生命期，检查后续重叠写与副作用屏障。计算值仅在其定义与写回相邻或中间只有至多两个无元数据 Nop 时使用提前提交，排除 pinned 与特殊状态槽。未声明 `private_state` 时，普通内存写构成别名屏障。U32 写入 U64 槽先保证高位清零。V256 尾部读取回归覆盖 portable 优化器的完整宽度。[allocate.cpp](../source/runtime/backend/x64/allocate.cpp)、[emit.cpp](../source/runtime/backend/x64/emit.cpp)、[rewrite.cpp](../source/runtime/ir/passes/rewrite.cpp)

## 5. 生成代码与 PMU

PMU 每引擎、每负载 7 个新进程，全部计数满足 `enabled_ns == running_ns`。CPU-clock IP 采样覆盖 8 个负载、2 个引擎、各 3 个进程，每进程 60 次热执行，丢失样本为零。

| 循环 | 基线指令／轮 | 候选指令／轮 |
| --- | --- | --- |
| 整数 | 13 | 12 |
| 访存 | 18 | 15 |
| 移位 | 14 | 13 |
| 浮点 | 33 | 25 |

热路径记录按普通无事件、回边取分支的实际路径计数。完整单元同时保留入口、冷区、常量池和 helper 收割，静态计数与热循环分别归档。[整数热路径](evidence/arm64-x64-residual-20261112/implementation/integer-hot-paths.txt)、[访存热路径](evidence/arm64-x64-residual-20261112/implementation/memory-hot-paths.txt)、[浮点热路径](evidence/arm64-x64-residual-20261112/implementation/fp-hot-paths.txt)

矩阵内层每轮有 64 条 packed 运算，普通路径共 1239 条宿主指令，平均 19.36 条／运算，热区 MXCSR 存取为零。排序比较分支路径为 SUB、事件指针装载、JRCXZ、JBE，共 4 条，原先的布尔链消除。SHA 全部发布单元和访存热循环均无 PUSHF／POPF。[矩阵热路径](evidence/arm64-x64-residual-20261112/implementation/matrix-hot-paths.txt)、[排序分支](evidence/arm64-x64-residual-20261112/implementation/sort-hot-paths.txt)

下表同时计入完整单元的入口、冷路径和常量池，MXCSR 指令数量按静态反汇编统计。

| 负载 | 基线代码字节 | 候选代码字节 | LDMXCSR 基线／候选 | STMXCSR 基线／候选 |
| --- | --- | --- | --- | --- |
| 浮点 | 1588 | 1520 | 6／2 | 5／1 |
| 向量矩阵 | 30868 | 41648 | 100／120 | 82／60 |
| SHA | 16444 | 13900 | 0／0 | 0／0 |
| 排序 | 3048 | 2944 | 0／0 | 0／0 |

向量矩阵的冷区与调用点保护增加发布代码量，热循环的区间切换已消除。PMU、IP 样本与冷热区二进制共同归档。

## 6. 编译成本

ARM64 完整程序使用 Function／JIT／pin0，31 组基线／候选交替冷运行。

| 阶段 | 基线 μs | 候选 μs | 变化 |
| --- | --- | --- | --- |
| Frontend | 1203.2 | 1104.2 | -8.2% |
| Optimize | 305.6 | 295.5 | -3.3% |
| Backend | 573.7 | 661.6 | +15.3% |
| Publication | 89.0 | 84.3 | -5.2% |
| 冷编译总计 | 2303.4 | 2237.2 | -2.9% |

两者均产生 12 次编译请求。发布代码量为 34,028 → 35,932 字节。总冷编译比值为 0.971，方案验收线为 1.03。Backend 变化单独保留，反映新增原生覆盖、flags 需求和 uniform 稳定性分析的成本。

规模 probe 各运行 3 个新进程，取每请求编译耗时中位数，N、2N、4N 使用原方案相同输入规模。

| 规模 probe | N μs，基线／候选 | 2N μs，基线／候选 | 4N μs，基线／候选 |
| --- | --- | --- | --- |
| uniform | 61.0／70.0 | 118.8／151.5 | 274.8／316.6 |
| pressure | 40.4／44.1 | 72.3／81.5 | 150.5／175.3 |
| flags | 122.8／138.1 | 270.3／306.0 | 549.1／619.2 |
| fp | 273.3／293.9 | 562.5／631.3 | 1076.9／1198.3 |
| cold | 267.4／295.9 | 532.6／548.3 | 1105.2／1168.1 |
| cfg | 40.2／46.2 | 77.3／94.1 | 158.4／175.7 |

N 为 256，2N 为 512，4N 为 1,024。4N 的候选／基线为 1.057—1.165，规模增长保持近线性。该组沿用裸编译接口，FP 与 cold probe 使用兼容区间路径，保留全部原始序列。

规模原始序列、真实 spill、发布字节、32 域首次创建和第二轮复用全部保留。[编译与规模数据](evidence/arm64-x64-residual-20261112/implementation/summary.json)

## 7. 实施调整与接口边界

- VM 通过 `arm_fpsr_offset` 显式启用 pending MXCSR 合同。裸后端默认调用保留原区间协议，原有 `region` 状态与 stub 分支承担兼容入口。
- FP 比较原生生成 NZCV nibble，前端通过 `LSL 28` 和完整 `RestoreFlags` 发布，后继使用 packed 条件消费。
- 浮点转整数在转换前检查 NaN、符号和范围，unsigned64 高半区与 FZ=1 的 F64→F32 精度转换由精确叶子处理，避免先执行原生转换污染 sticky 状态。
- LAHF 保存与恢复始终保护 RAX，context 区分 packed、raw RFLAGS、LAHF／OF 三种格式。CPUID 能力进入配置身份，缺少 LAHF 时选择 raw 快照。
- 编译单元的 FPCR 可能改变时，数值叶子按运行时 FPCR 执行。linked flags 的 private-state／fault-location 合同不满足时，编译返回可诊断错误。

候选 `CodeBuildId` 为 `045cbc5e2551348c2c212350f19a834302969b0afbf7c7cbb58613e281d50d42`。源代码指纹、宿主能力和数值域共同参与缓存身份。最终 runner、缓存和 AOT 采用同一候选构建身份，`NativeRunContext` 的字段布局沿用机制基线。[manifest](evidence/arm64-x64-residual-20261112/implementation/manifest.json)

## 8. 复现证据

实施证据独立放在 `implementation` 子目录，包含冻结 runner、完整配对记录、corpus／oracle／actual、CTest、整程序缓存与 AOT、PMU、IP 样本、JIT 二进制及反汇编、诊断变体源码和构建参数。`source.patch` 与新增源码可还原工作区实现，逐文件 SHA-256、构建身份和归档 SHA-256 可复核对应关系。[证据索引](evidence/arm64-x64-residual-20261112/implementation/archive-index.json)、[完整归档](evidence/arm64-x64-residual-20261112/implementation/raw-evidence.tar.gz)
