# ARM64 → x64 机制优化实施结果

日期为 2026-11-12。实施基于上一轮四项优化后的工作区，覆盖方案中的八个批次。保留原有修改，完成运行时、IR 优化、状态恢复、缓存与 AOT 的集成。方案已按实际实现更新。[实施方案](arm64-x64-mechanism-optimization-plan-2026-11-12.md)

## 1. 实际启用的机制

| 机制 | 已落地的行为 | 主要入口 |
| --- | --- | --- |
| 函数内 NZCV 驻留 | 同宽整数 producer 建立 EFLAGS 事实，追踪 carry 极性，部分写合并保存槽，条件直接使用 Jcc／SETcc／CMOVcc | [emit_flags.cpp](../source/runtime/backend/x64/emit_flags.cpp) |
| CFG 与观察边界 | 四位 flags 需求传播、规范化合流、保留 EFLAGS 的事件快路径、空 IR 标记保持 flags、按目标需求布置边转换 | [allocate.cpp](../source/runtime/backend/x64/allocate.cpp)、[emit.cpp](../source/runtime/backend/x64/emit.cpp) |
| 跨单元 flags ABI | static link 和 ready dispatcher 交换规范快照，helper／冷解析／退出发布完整 portable flags | [entry_elf.S](../source/runtime/backend/x64/entry_elf.S)、[dispatch.h](../source/runtime/execution/dispatch.h) |
| 精确故障恢复 | 每个真实访存记录 native／saved mask 与 carry 极性，signal 现场提供 RFLAGS、RSP，恢复未物化 NZCV 和 pinned 状态 | [recovery.cpp](../source/runtime/backend/x64/recovery.cpp)、[x64_host.cpp](../source/runtime/platform/linux/x64_host.cpp) |
| FPCR 专门化与浮点区间 | 全部 32 种受支持控制组合按需编译，入口校验 FPCR，直线区间共享 MXCSR、累计 IXC，状态观察前提交 | [emit_float.cpp](../source/runtime/backend/x64/emit_float.cpp)、[runtime.cpp](../source/runtime/execution/runtime.cpp) |
| 精确状态 effects | 共享 uniform 范围查询，精化 FPCR／FPSR、PC、flags 和 numeric control，八项缓存跟踪并删除局部完整覆盖的 store | [uniform_effects.h](../source/runtime/ir/uniform_effects.h)、[rewrite.cpp](../source/runtime/ir/passes/rewrite.cpp) |
| 指令约束与寄存器复用 | 双操作数 ALU／选定 SSE 操作交接末次使用的首输入 home，保留真实 spill 语义，省去已归一化 U32 的自搬运 | [allocate.cpp](../source/runtime/backend/x64/allocate.cpp)、[emit_operand.cpp](../source/runtime/backend/x64/emit_operand.cpp) |
| 冷区与共享调用 | FP 慢路径和事件退出延后发射，FP 站点按调用合同共享 stub，保存寄存器使用物理位图，局部 CALL／RET 处理栈偏移及对齐 | [emit_float.cpp](../source/runtime/backend/x64/emit_float.cpp)、[emit_calls.cpp](../source/runtime/backend/x64/emit_calls.cpp) |

x64 VM 编译在 private state 与 fault locations 合同成立时启用原生 flags。Known、Unknown、EarlyOff lowering，以及 ARM64、ARM32、x86 guest 使用一致的跨单元 ABI。裸后端的 `CompileOptions::local_flags` 保留显式选择入口。[runtime.cpp](../source/runtime/backend/x64/runtime.cpp)

raw RFLAGS 保存槽占 8 字节，使用 PUSHFQ／POP 与 PUSH／POPFQ。RBX 保留 PF 来源、AF、DF 和其他 portable 位。规范快照满足 `physical_CF = !logical_C`，其他 producer 的极性由发码事实跟踪。`NativeRunContext` 扩展至 1088 字节，新增字段位于 1072、1080，已有偏移保留。[emit_flags.cpp](../source/runtime/backend/x64/emit_flags.cpp)、[abi_layout.inc](../source/runtime/backend/x64/abi_layout.inc)

## 2. 正确性验收

| 验收 | 最终结果 |
| --- | --- |
| CTest | 31 通过、7 跳过、0 失败 |
| ARM64／ARM32 基础矩阵 | 4,224 次，全部原生执行并与 oracle 一致 |
| 标量浮点矩阵 | 27,648 次，全部原生执行并与 oracle 一致 |
| 二元／indexed NEON 浮点矩阵 | 25,296 次，全部原生执行并与 oracle 一致 |
| x86 基线状态回归 | Block／Function 共 1,278 次，状态、内存、service 与 fault 字段逐项一致 |
| 完整程序 | ARM64／ARM32／Thumb 各 8 组，合计 24 组输出一致 |
| 持久化 | 三种 guest 的冷写缓存、暖命中、AOT 执行均通过，暖缓存编译请求为零 |
| FPCR 域 | 32 个组合往返、特殊输入、非默认舍入、控制变化、磁盘与 AOT 多域恢复均通过 |

ARM 基础矩阵覆盖 Block／Function、Known／Unknown／EarlyOff、optimize 开关。浮点矩阵另有关闭优化的原生执行。完整程序覆盖 JIT、Mixed、Interpreter、pin0／pin2。JIT 与 AOT 检查输出、原生执行、fallback 和解释执行计数。[正确性摘要](evidence/arm64-x64-mechanisms-20261112/summary.json)

x86 每种 scope 包含 624 个原生用例和 15 个既有未原生执行用例。既有比较器将每种 scope 中 100 个结果标为 `oracle_alignment_gap`、406 个标为 `x87_gap`。此次另对全部 639 个用例的完整结果字段作基线比较，差异为零。原始分类、逐例比较和全字段比较摘要一起保留。[原始证据](evidence/arm64-x64-mechanisms-20261112/raw-evidence.tar.gz)

七个 skip 来自当前 x64 宿主无法执行的其他宿主 ISA 测试，名称为 `instruction_coverage_native`、`avx_coverage_native`、`x87_coverage_native`、`loongarch64_native_contract`、`arm64_native_contract`、`pipeline_native_contract`、`riscv64_native_contract`。对应 portable compile 合约均执行通过。[CTest 记录](evidence/arm64-x64-mechanisms-20261112/validated-ctest.xml)

新增定向回归覆盖 16 种 NZCV 输入和 16 个条件、carry 极性、部分更新、数值条件用途、helper、真实 fault、局部 label 合流、寄存器压力与结果后续 spill。恢复用例枚举 native／saved 来源组合。另补充 flags 字段窄写的保留位检查，以及空 marker 前后生成字节一致的检查。[x64_compile_test.cpp](../source/tests/rebuild/x64_compile_test.cpp)、[runtime_engine_test.cpp](../source/tests/rebuild/runtime_engine_test.cpp)、[runtime_persistence_test.cpp](../source/tests/rebuild/runtime_persistence_test.cpp)

## 3. 热执行与代码形态

环境为 WSL `ProtonHM`、Release／IPO、CPU2。基线为上一轮四项优化后的冻结 runner，参考为固定 Dynarmic `9d4582339990d4eae53f1dc7160686920fc2075c`。三个引擎轮换并反转执行顺序，每个负载各 31 个新进程，每进程 3 次热执行取中位数。SwiftVM 使用 Function／pin0，Dynarmic 使用其常规 block linking。输出与暖缓存复用在每次测量中校验。

| 负载 | 基线 ms | 候选 ms | Dynarmic ms | 耗时降低 | 候选／Dynarmic | 配对候选／基线中位数 [Q1, Q3] |
| --- | --- | --- | --- | --- | --- | --- |
| 整数 | 30.807 | 8.244 | 13.693 | 73.2% | 0.60× | 0.267 [0.266, 0.271] |
| 访存 | 29.992 | 19.238 | 16.476 | 35.9% | 1.17× | 0.640 [0.635, 0.645] |
| 移位 | 31.643 | 11.107 | 16.821 | 64.9% | 0.66× | 0.351 [0.347, 0.355] |
| 浮点 | 10.772 | 9.660 | 4.581 | 10.3% | 2.11× | 0.894 [0.885, 0.902] |
| 综合计算 | 11.316 | 8.602 | 4.262 | 24.0% | 2.02× | 0.762 [0.745, 0.772] |
| 浮点展开 ×8 | 9.371 | 1.615 | 2.692 | 82.8% | 0.60× | 0.171 [0.170, 0.174] |
| 向量矩阵 | 2.612 | 0.769 | 0.127 | 70.6% | 6.06× | 0.292 [0.289, 0.295] |
| 标量矩阵 | 5.188 | 2.595 | 0.442 | 50.0% | 5.87× | 0.498 [0.492, 0.506] |
| SHA | 6.561 | 6.572 | 3.248 | -0.2% | 2.02× | 0.999 [0.987, 1.009] |
| 排序 | 1.621 | 0.818 | 0.629 | 49.5% | 1.30× | 0.503 [0.498, 0.513] |

表中耗时和降低比例来自普通 runner。配对分布的原始 31 轮全部归档，出现大幅离散的轮次也保留。SHA 与矩阵另以 31 个新进程、每进程 9 次热执行追加复测。

| 追加负载 | 基线 ms | 候选 ms | Dynarmic ms | 配对中位数 [Q1, Q3] |
| --- | --- | --- | --- | --- |
| SHA | 6.536 | 6.490 | 3.201 | 0.998 [0.982, 1.008] |
| 向量矩阵 | 2.598 | 0.754 | 0.123 | 0.291 [0.286, 0.294] |
| 标量矩阵 | 5.086 | 2.543 | 0.436 | 0.498 [0.494, 0.505] |

扩展组使用每个配置 9 个新进程、每进程 3 次热执行。下表为候选／基线耗时中位数之比。

| 负载 | Block／pin0 | Block／pin2 | Function／pin2 |
| --- | --- | --- | --- |
| 整数 | 0.579 | 0.591 | 0.269 |
| 访存 | 0.645 | 0.654 | 0.632 |
| 移位 | 0.570 | 0.588 | 0.336 |
| 浮点 | 0.949 | 0.966 | 0.901 |
| 综合计算 | 0.802 | 0.798 | 0.768 |

PMU 每引擎、每负载 7 个新进程，检查每次计数的 `enabled_ns == running_ns`。前三个循环的 guest 迭代数均为 12,000,000。

| 循环 | 基线指令／轮 | 候选指令／轮 | Dynarmic 指令／轮 | 候选分支／轮 |
| --- | --- | --- | --- | --- |
| 整数 | 37.00 | 13.00 | 23.00 | 2.00 |
| 访存 | 37.00 | 18.00 | 23.00 | 2.00 |
| 移位 | 40.00 | 14.00 | 26.00 | 2.00 |

整数回边直接消费 producer 的 ZF，事件检查使用 MOV／JRCXZ。两个目标需要 flags 时，规范快照在分支前保存一次，Jcc 直接到达目标。空 Nop 与顺序标记保持 EFLAGS，已归一化的 U32 home 跳过冗余自搬运。反汇编、JIT 映射与逐进程 PMU 记录可定位这些代码形态。[原始证据](evidence/arm64-x64-mechanisms-20261112/raw-evidence.tar.gz)

CPU-clock IP 采样覆盖 8 个负载、2 个引擎，各 3 个进程、每进程 60 次热执行，合计 93,268 个样本，丢失数为零。分类摘要、进程映射与生成代码一起归档。

下表统计诊断 runner 实际发布的全部代码字节，以及这些单元反汇编中的静态 MXCSR 指令数量。冷热区和外部 entry wrapper 均计入。

| 负载 | 基线代码字节 | 候选代码字节 | LDMXCSR 基线／候选 | STMXCSR 基线／候选 |
| --- | --- | --- | --- | --- |
| 浮点展开 ×8 | 3,484 | 2,660 | 32／10 | 24／8 |
| 综合计算 | 58,864 | 50,772 | 276／100 | 206／82 |
| 向量矩阵 | 37,740 | 30,868 | 276／100 | 206／82 |
| 标量矩阵 | 8,956 | 10,356 | 72／46 | 53／37 |
| SHA | 18,292 | 16,444 | 0／0 | 0／0 |
| 排序 | 2,532 | 3,048 | 0／0 | 0／0 |

向量矩阵代码量降低 18.2%，标量矩阵增加 15.6%，排序增加 20.4%。原生 flags 的入口与冷出口、完整 FPCR 入口检查以及共享调用增加固定代码，单元大小与其实际热路径改善分别记录。

FP 和矩阵仍具有逐 lane 指数准入、SSA 搬运与必要的精确慢路径，表中保留其与 Dynarmic 的实际差距。SHA 的热耗时按追加复测解释。[完整测量摘要](evidence/arm64-x64-mechanisms-20261112/summary.json)

## 4. 编译成本与规模

ARM64 完整程序另作 31 组基线／候选交替冷运行，Function／JIT／pin0。两者均产生 12 次编译请求。

| 阶段，时间单位 μs | 基线 | 候选 | 变化 |
| --- | --- | --- | --- |
| compile 总量 | 2322.425 | 2388.150 | +2.8% |
| Frontend | 1273.855 | 1246.088 | -2.2% |
| Optimize | 275.821 | 319.406 | +15.8% |
| Backend | 528.885 | 597.763 | +13.0% |
| Publication | 104.032 | 97.926 | -5.9% |
| native bytes | 33,364 | 34,028 | +2.0% |

各阶段分别取中位数。`compile_request_ns` 还包括请求管理与实际域切换的计时。热执行收益、优化阶段增量和后端增量按各自指标报告。

compile 配对比值中位数为 1.029，Q1／Q3 为 0.997／1.064。最大单轮比值为 3.785，该轮完整保留在原始记录中。

独立 probe 使用 N＝256、512、1024，3 个新进程，每进程每个规模含 9 组、每组 8 次编译请求。每组执行生成代码并校验结果。

| 输入 | N μs | 2N μs | 4N μs | 4N／N |
| --- | --- | --- | --- | --- |
| uniform | 66.40 | 127.65 | 293.63 | 4.42× |
| pressure | 43.49 | 77.45 | 161.13 | 3.70× |
| flags | 133.44 | 297.64 | 611.67 | 4.58× |
| fp | 297.48 | 617.50 | 1161.30 | 3.90× |
| cold | 287.23 | 559.25 | 1157.74 | 4.03× |
| cfg | 42.12 | 82.84 | 167.21 | 3.97× |

uniform 组在三个规模分别消除 256／512／1024 个覆盖 store。pressure 组始终包含 14 个真实 SSA spill。FP 与 cold 两组始终各使用 2 个共享 stub。flags 与 CFG 组覆盖控制流和 flags 传播，规模数据同时保留原始 IR 节点数、块数、frame bytes 与代码字节。

### FPCR 版本与切换

另以 9 个新进程执行固定默认、固定非默认、两域交替和全部 32 个域的序列，每个序列执行两遍，每遍 32 次调用。第二遍切换特殊数值输入并检查数值、FPSR、保留位和编译复用。

| 序列 | 编译版本数 | 第二遍新增编译 | 代码字节 | 域目录数 | 切换次数 | 累计 reset μs |
| --- | --- | --- | --- | --- | --- | --- |
| default | 1 | 0 | 2064 | 1 | 0 | 0.000 |
| nondefault | 1 | 0 | 2064 | 2 | 1 | 0.022 |
| alternating | 2 | 0 | 4128 | 2 | 63 | 268.846 |
| all | 32 | 0 | 66048 | 32 | 63 | 296.795 |

非默认组的域目录数包含 VM 初建的空默认目录。全部 key 的驻留代码随实际触发的版本增长，第二遍新增编译数均为零。各序列在同一进程内依次执行，默认组首次调用包含 decoder 的进程级初始化，耗时按执行顺序解释。`switch_ns` 测量 L1 reset，后续 lookup 重新填充体现在调用耗时中。[域与规模摘要](evidence/arm64-x64-mechanisms-20261112/summary.json)

## 5. 实施调整与修复

- 快照采用 8 字节 raw RFLAGS，避开 RAX／AH 固定寄存器约束，并以基础 x64 指令完成往返。
- FPCR 一次覆盖 FZ16、RMode、FZ、DN 的 32 种受支持组合。128 个可空 cache 指针同时承载已有低两位 DAZ／FTZ key，目录按需创建。
- allocator 在现有遍历识别显式 FPCR store 和 opaque 控制写。此类单元的区间入口重新检查控制，失配时在当前 FP 指令执行前退出。
- 浮点区间在 flags producer 前提交累计状态，使随后的原生条件直接复用 EFLAGS。
- 持久格式升级为版本 5，旧格式 AOT 需要重新生成。默认 AOT 编译初始域，首次进入其他域时按需 JIT，多域镜像可按保存的域显式恢复。
- 批次 5—8 采用集成验收，最终性能归因采用集成对照与诊断证据。各项定向回归和合成规模输入分别核对对应机制。
- 修复了 round-minus 下 F32 双 lane 减法在未使用高 lane 产生负零的问题，运算后显式清除未使用结果。
- 修复 flags uniform 窄写的保留位问题，访问前发布当前载体，部分覆盖后从完整字段重建 RBX。packed 和 local flags 两种路径均有回归。

CTest 的第一次运行使用 DrvFS 当前目录作为 scratch，缓存权限检查失败。重新使用 Linux 文件系统中的 `SVM_TEST_SCRATCH` 后完成验收，失败记录与通过记录均归档。

## 6. 构建身份与复现

最终 `CodeBuildId` 为 `205fa36241fbda239874aca7b0c2f2c55ed90e484e79b506be7fa1c220686587`。基线与候选 runner 的完整 SHA-256、源码文件哈希和实际构建参数记录在清单中。

证据包包含最终源码差分与新增源文件、源码 SHA-256、构建配置和命令、基线与最终 runner、固定 ELF、逐轮测量、PMU、采样、JIT 代码与反汇编、测试输入／oracle／实际结果及比对记录。摘要和完整归档均有 SHA-256 清单。[摘要与清单](evidence/arm64-x64-mechanisms-20261112/manifest.json)、[原始证据包](evidence/arm64-x64-mechanisms-20261112/raw-evidence.tar.gz)

构建与测试命令在 WSL 中执行，`SVM_TEST_SCRATCH` 指向支持 Linux 权限的可写目录。

```sh
cmake --build /mnt/d/Dev/SwiftVM/build/host -j 8
SVM_TEST_SCRATCH=/tmp ctest --test-dir /mnt/d/Dev/SwiftVM/build/host --output-on-failure
```

`scripts/` 中保留实际使用的差分、程序验证、热执行、PMU、采样、编译成本与扩展组脚本。路径采用本次 WSL 工作目录，迁移环境时同步调整 ROOT、WORK 和既有 corpus 的路径。原工作目录为 `/home/proton/.cache/SwiftVM/arm64-x64-mechanisms-20261112/`。
