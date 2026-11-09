# 编译构造与资源所有权优化（2026-11-08）

后续已修复 ARM64 host FP 环境保存顺序及 fatal signal 的 guest FPSR 合并，[定点验证记录](evidence/fp-exception-fixes-20261108/summary.json)。本文保留 `ee0ae5c4` 检查点的原始性能数据，不代表这两项修复后的重测。

本轮已实现固定形状 IR 构造、消费点的局部读取复用、无 Value 输入的分配扫描省略、SMC 批量布防、L2 容量收敛以及故障元数据缓冲复用。没有新增 IR、分析表或正确性证明层；Block / Function 分配仍独立。

**当前没有达到所有用例 compile ≤2×FEX 的目标，也没有建立数量级提升。** 最终 x87 Block / Function 为 **2.557× / 2.283×FEX**。交替对照的 x87 墙钟中位数下降约 6.5% / 8.6%，但独立 PMU 显示其编译主机指令数略增，不能把墙钟变化全部归为 IR 出生成本降低。

## 1. 落地内容与职责

| 文件，相对 source/ | 工作 |
| --- | --- |
| `runtime/ir/builder.h/.cpp`、`builder_impl.h` | 四种薄模板入口 `EmitImm`、`EmitUniform`、`EmitUniformValue`、`EmitX87` 静态约束既有 schema；四个共享实现直接检查、编码真实参数。通用入口保留。两条路径共用 `StoreOperand`、`CommitAppend` 和 suffix rollback，编码与发布规则只有一份。 |
| `runtime/frontend/x86/build_ir.cpp`、`semantics_operands.inc`、`semantics_x87.inc` | 常量、Uniform、x87 命令使用固定形状入口；仍提交中性 GPR / 内存 IR。没有引入物理寄存器或软件查表描述。 |
| `runtime/ir/direct/interpreter.h` | 相同调用适配既有 direct IR 执行；没有复制前端指令语义。 |
| `runtime/ir/passes/rewrite.cpp` | 在实际消费位置复用已有 operand range、自然类型，限定局部代数 helper 的候选 opcode；不根据零优化计数跳过 pass。 |
| `runtime/backend/arm64/allocation_selection.cpp` | Nop / LoadImm / FaultLocation / EntryPoint 的 schema 无 Value 边，省去使用扫描；PC / NZCV / pin 处理及三段分配依赖保持。 |
| `runtime/execution/smc.h/.cpp` | `Arm(span<SourceSpan>)` 批量收集；已有页全部 armed 时跳过重复插入、排序和全表扫描。单个 `fully_armed` 汇总保留旧 unarmed 页和部分 mprotect 失败的重试行为；没有新增页索引。 |
| `runtime/execution/code_cache.cpp` | L2 从 1/8 占用率扩容改为 1/4；哈希、项布局、别名、pending、L1 ABI 保持。 |
| `runtime/execution/code_heap.h/.cpp` | 故障记录按稳定片段追加，首次至少 1 KiB，后续至少 4 KiB，大单元按实际数量。只给新片段 reserve，旧片段容量内追加，已发布 span 地址保持稳定。沿用现有 `fault_tables`，不增加池或所有权层。 |
| `runtime/execution/compile_service.h/.cpp`、`code_persistence.cpp` | 发布借入 fault span 并复制到 CodeHeap；之后清空后端临时 vector、保留容量，超过 4096 条则回收。持久化从已发布记录复制。代码、元数据和临时缓冲的实际释放仍计入原窗口。 |
| `runtime/frontend/timing.h`、两前端 `build_ir.cpp`、`execution/timing.h`、`runtime.*`、`dispatch.cpp`、launcher | 新增独立 `--compile-details`，细分 formation / IR birth 和 mapping / metadata / transfer / synchronize / registration / L1 准备。普通 `--decode-timing` 不增加这些读钟。 |

固定形状入口省去的是通用 Operand 数组与形状处理，保留 unit / node / 非 Void / view / literal / result / metadata / capacity 合同。Append 原本已单遍检查并编码，本轮没有所谓“删除 Append 的第二遍”。Rewrite 的异常安全流程也没有被绕开。

故障记录原先每个单元移走后端 vector，下一次编译重新申请，CodeHeap 退出时逐单元释放。现在后端复用临时容量，CodeHeap 拥有永久记录的稳定片段，`NativeCode` 继续只借用自己的真实范围。SMC / Close 先撤销读者再释放，部分 unmap 失败时保留记录，W^X、native 指令布局和产物格式保持。

提前在 liveness 遍历中读取 operand range 的尝试已撤回，`liveness.cpp` 与父版本逐字节相同。PMU 显示该尝试增加了优化阶段工作；消费点少一次读取并不意味着提前读取就更快。最终仍不把所有局部遍历修改统称为性能收益。

## 2. 正式 compile 对照

红魔 `bf1359ff`，CPU4；min/max 614400/1171200 kHz，全程未改频率或系统服务。固定 Android FEX `a156752f` 与相同原始输入，默认 multiblock / TSO 配置。每用例、每模式固定三轮，轮换顺序，全部保留；没有取最快值、按门槛重跑或混用候选。

下表单位 ms，中位数，只扣实际 byte parser。FEX CompileBlock 可包含多个基本块，Function 是主要结构对照，Block 仍作为独立分配层展示。

| 输入 | Block | Function | FEX | B/FEX | F/FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| CoreMark | 38.6982 | 48.4455 | 46.4466 | 0.833× | 1.043× |
| int | 1.9784 | 1.9016 | 1.6559 | 1.195× | 1.148× |
| fp | 2.4565 | 2.6276 | 1.6372 | 1.500× | 1.605× |
| mem | 2.3753 | 2.6026 | 1.5725 | 1.510× | 1.655× |
| branch | 1.8896 | 2.0475 | 1.5397 | 1.227× | 1.330× |
| call | 2.1247 | 2.2405 | 1.6985 | 1.251× | 1.319× |
| func_tests | 19.6912 | 30.6079 | 30.0960 | 0.654× | 1.017× |
| x87 | 0.7237 | 0.6459 | 0.2830 | 2.557× | 2.283× |

SVM 包括 host bind、fault service 和完整 native owner 回收。固定 FEX 探针的 logical work-buffer release 不包括最终 native arena / lookup cache 析构；不能把它的短释放时间当作完整回收成本，也没有通过挪动 SVM 工作来缩短计时。

FEX 的 CreateThread 已在 CompileBlock 探针前建立 compiler、LookupCache 和首个 16 MiB native arena；部分工作缓冲的首次申请仍在探针内。SVM 的整个 VM 构造、guest ELF / stack 建立也不在此窗口内，但按需 JIT 资源创建留在 compile 中。不能笼统声称任一方包含了全部初始化。

三轮仍存在明显墙钟波动，完整范围与逐条记录见[证据](evidence/compile-shape-optimization-20261108/README.md)。不能用某一轮或其他候选替换慢样本。

### 与稳定父版本交替运行

父 runner `c8ac4ba9…`，当前 `ee0ae5c4…`，每模式 / 每版本固定三次。单位 ms。

| 输入 | Block 父→当前 | 变化 | Function 父→当前 | 变化 |
| --- | ---: | ---: | ---: | ---: |
| FP | 2.6594→2.5948 | -2.43% | 2.8824→2.7902 | -3.20% |
| func_tests | 20.8436→19.9402 | -4.33% | 31.0888→30.7984 | -0.93% |
| x87 | 0.7512→0.7025 | -6.49% | 0.6580→0.6015 | -8.60% |

这些是固定三轮的描述性结果，不是显著性结论；小型 x87 的回收、fault service 等固定阶段影响很大，不能将整项变化算成某一处代码的收益。

## 3. CPU 工作量与代码质量

独立 instruction-clock 将原 CLOCK_MONOTONIC 读钟替换为 pinned per-thread user instructions。原 JSON 字段仍带 `_ns` 后缀，**本组所有计时数字实际都是 retired user instructions，绝非纳秒**。每次 read 要求 enabled=running；24 条观测通过 checksum、零解释回退及编译工作集合校验。计数包含探针包装开销，也影响代表 guest 发出的同类 host 时钟调用，只用于同协议诊断，不进入正式时间或 FEX 倍率。

下表取原字段 `compile_without_byte_decode_ns`，表示**扣除 byte parser 后的编译主机指令数**。

| 输入 / scope | 父版本指令数 | 当前指令数 | 变化 |
| --- | ---: | ---: | ---: |
| fp block | 3,104,618 | 3,056,819 | -1.54% |
| fp function | 4,009,186 | 3,956,017 | -1.33% |
| x87 block | 182,042 | 183,311 | +0.70% |
| x87 function | 173,154 | 174,148 | +0.57% |

因此扣除 byte parser 后，FP 编译主机指令量只有约 1%–2% 的下降，x87 略增。最终优化阶段也没有全面下降，不能把少了临时对象或函数调用直接等同于大幅 compile 收益。

L2 的实际容量在除 x87 外的样本中减半，并少一次 table allocation：小用例 512→256 slots，CoreMark 32768→16384，func_tests Block 16384→8192、Function 32768→16384。每槽 32 B，CoreMark 少 512 KiB 槽空间；代价是少量 L2 probe 增加。x87 仍为 32 slots，不能把它的变化归给 L2。

`llvm-size text` 为 2,936,664→2,905,324 B；Builder 名下 text 符号汇总为 203,736→191,708 B。它不是所有内联 helper 的完整归属统计，也不用于推算时间收益。

生成质量计数有明确边界：正式 compile 的 CoreMark Block 第 1、2 轮为 2049 units / 11728 guest instructions / 35333 IR nodes / 63234 words / 315584 native B；父版及当前第 0 轮为 2053 / 11761 / 35383 / 63345 / 315400。paired func_tests Block 中父版第 2 轮多一个单元（+13 guest instructions、+23 nodes、+43 words、+220 native B），当前三轮没有该额外单元。runtime CoreMark Block 也有跨轮变化。全部保留，不推测原因，不将工作集合变化当成质量提升；其余脚本比较没有发现非 L2 计数差异。

正式 whole-child user PMU cycles 的 SVM/FEX 中位数如下。CoreMark 为 100000 iterations，guest loop ≥10 秒、CRC d340 和正式校验通过；compile 的 20 次迭代仅检查 CRC4983。其余 elapsed 包括进程启动、JIT 与退出，不能用 cycles 倍率替代墙钟。

| 输入 | Block cycles/FEX | Function cycles/FEX |
| --- | ---: | ---: |
| CoreMark | 1.067× | 1.025× |
| int | 0.689× | 0.688× |
| fp | 1.188× | 1.192× |
| mem | 1.050× | 1.053× |
| branch | 0.981× | 0.983× |
| call | 1.053× | 1.052× |
| func_tests | 0.761× | 0.945× |
| x87 | 0.676× | 0.677× |

FP Block 整进程 elapsed 为 FEX 的 1.362×，cycles 为 1.188×；这些未达目标的项仍保留。x87 fixture 沿用 exit-only 验收，不能称为完整数值证明。

## 4. x87 的剩余差额

下表单位 µs，各列取其**总 compile 中位数对应的真实样本**，没有拼接各阶段中位数。

| 阶段 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| formation＋IR | 217.1 | 255.8 | 114.9 |
| IR 优化 | 58.8 | 57.1 | 43.7 |
| 分配＋编码 | 119.6 | 90.6 | 96.5 |
| 发布 | 102.5 | 86.0 | 20.2 |
| SVM native release／FEX logical release | 183.7 | 116.2 | 2.3 |
| 其他 | 42.0 | 40.2 | 5.4 |

SVM native release 与 FEX logical release 的端点不同。剩余差额需要继续分清首次 formation / IR 出生、发布固定工作与完整回收；本轮没有证明它们全部来自 CPU 指令量，也不能凭这些墙钟将差额全部归给分配器。

## 5. 保留的设计边界

- 可写源仍限制为单基本块。不能仅解除限制：原生写 fault、AtomicBridge 无信号 pending、REP / 多 store / x87 状态写入都需要完整 guest 指令提交与真实后继 PC 的恢复合同。当前没有半成品信号重入状态。
- CALL return continuation 尚未盲目加入 Function：可选后继解码失败不能否定已成功的根 CALL，需要先有局部失败退出边界。
- Uniform GPR / 内存 IR、未知后端分支、精确 FP sticky flags、Block / Function 分配继续保留；没有增加 MIR、计划 / actions / certificates 或逐 node 证明表。

## 6. 验证与保存

Host 五组构造 / direct / x86 / optimizer / ARM64 发码合同通过。修正了旧 x86 compare fixture：产品已有 signaling predicate bit4，fixture 仍只期待低四位；没有为本轮改动放宽产品语义。

Android 验证 FP 状态与链接 4 组、runtime native 8 组、persistence 4 组、AVX 4 组，以及 SourceAdmission / FaultMetadataLifetime 2 组。x64、RV64、LoongArch64 各用 QEMU system＋真实 Linux 内核验证相同两组 compile 合同、runtime native 8、persistence 4 和 x86 / ARM B/F 四次 smoke。旧 span 在输入销毁、自别名复制、多片及外层容器增长后仍稳定。这里不宣称实体跨架构性能或并发 guest 执行。

源码冻结 30 文件，测量后哈希匹配；liveness 额外核对与 HEAD 相同。统一使用实际 clang-format 22.1.1，检查真实格式化字节与 diff。Git 基线 `08143855`；本轮尚未提交或 push。

最终 runner SHA256：`ee0ae5c47f8988dd7d339fcbc0787e361aee27465316f4901dbbea52a0cdbac9`。最终观测为 72 compile＋72 runtime＋48 paired＋18 details＋24 instruction-clock；各中途候选完整保留在原结果根的 `pre-*`，未混入最终表。小型 JSON 证据见[索引](evidence/compile-shape-optimization-20261108/README.md)，完整脚本、源码包、二进制 gzip、stdout/stderr 和 kernel logs 留在 Data2。

手机私有目录已清理：移除 allocated bytes **251,753,472**，独立 df 空闲增加 **253,214,720 B**，剩余 0，无活跃进程、打开文件或清理错误。各 QEMU 已退出，派生 initrd 已删除；固定构建缓存保留，大临时数据继续放 Data2。
