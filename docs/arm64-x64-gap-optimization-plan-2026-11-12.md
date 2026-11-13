# ARM64 → x64 性能差距优化实施方案

设计日期为 2026-11-12，实施更新为 2026-11-13。本文承接已完成的残余优化重构，以 `af7671f6b5563e94a30ded7b3992163e8f9558bc` 所归档的生产实现和最新差距分析为依据。本轮已落地批次 0—5 和批次 6 的末次 RHS 复用，实际收益与剩余边界见实施结果。[本轮实施结果](arm64-x64-gap-optimization-results-2026-11-13.md)、[上一轮实施结果](arm64-x64-residual-optimization-results-2026-11-12.md)、[差距分析摘要](evidence/arm64-x64-residual-20261112/gap-analysis-20261112-1846/summary.json)

本轮按标量 SIMD 状态表示、flags 快照宽度、部分 NZCV、浮点语义 IR、精确结果检查、整数数据搬运组织实现。AVX 精确后查先于新增数值 IR 接入共用入口。运行时优化始终保留 guest 状态、NZCV、FPCR、FPSR 和异常位置的精确语义。实施记录保留分阶段二进制、定向回归、逐进程性能数据与编译成本。

## 0. 实施边界

| 批次 | 实际落地 |
| --- | --- |
| 0 | 从 Git 基线恢复完整构建快照，覆盖本轮修改，并逐文件核对可读工作区源码。原工作区两个目录的读取故障仍需处理 |
| 1 | Known x64 的零高位标量写入统一为整 Q 的 V128 SSA 写入，覆盖加载、浮点、lane、shift、permutation 和 SHA1H |
| 2 | LAHF／OF 有效镜像按 16 位读取和更新，raw RFLAGS 按 64 位处理，fault 解码同步修正 |
| 3 | Writer 使用逐位 native／saved 掩码，部分生产者可直接满足条件需求，混合来源沿用现有 fault recipe |
| 4 | 新增精确数值与位操作 IR、FPCR 使用守卫、显式 SSA 冷输入、浮点比较载体，以及按模式共享的数值调用 stub |
| 5 | 独立 AVX 检测、精确 NaN／MinNormal 合并后查、缓存／AOT 特征身份与 SSE 回退 |
| 6 | U32／U64 的 Add／And／Or／Xor 可以复用末次使用的 RHS，Home 保持 28 字节。扩大寄存器池和跨块状态保留作为后续独立批次 |

实现沿用现有优化遍历和域缓存。浮点环境继续按上一轮 pending MXCSR 合同管理。逐批验证、最终验收、源码身份和运行时／编译成本集中归档。[实施结果](arm64-x64-gap-optimization-results-2026-11-13.md)

## 1. 基线与已确认的原因

### 1.1 最新性能差距

宿主为 Ryzen 9 7950X3D，WSL `ProtonHM`，CPU2。SwiftVM 使用 Release／IPO、Function／pin0。Dynarmic 固定为 `9d4582339990d4eae53f1dc7160686920fc2075c`，启用正常 block linking，使用 safe optimizations、直接内存访问，关闭 unsafe optimizations 和 cycle counting。每个引擎运行 31 个新进程，每进程 3 次热执行。表中的比值为逐组配对比值的中位数。[最新对比](evidence/arm64-x64-residual-20261112/rebenchmark-20261112-1756/comparison.json)

| 负载 | SwiftVM ms | Dynarmic ms | SwiftVM／Dynarmic | 已定位的主要成本 |
| --- | ---: | ---: | ---: | --- |
| 标量矩阵 | 1.620 | 0.426 | 3.80× | 标量拆分写入与后续整 Q 读取，丢失 SSA 转发 |
| fpmix | 21.318 | 6.929 | 2.97× | FABS 拆分写入、flags 快照宽度、比较结果往返与迟后数值降级 |
| cond | 64.969 | 35.845 | 1.82× | 部分 NZCV 写入后立即保存、合并并恢复物理 flags |
| 向量矩阵 | 0.198 | 0.115 | 1.73× | NaN／MinNormal 后查与 guest 状态搬运 |
| 矩阵 FMA | 0.193 | 0.116 | 1.65× | 精确后查与迟后数值降级 |
| 综合计算 FMA | 4.444 | 3.837 | 1.16× | 不透明调用 IR、状态搬运和额外指令 |
| 综合计算 | 4.740 | 4.114 | 1.16× | guest 状态搬运、GPR 搬运和部分真实 spill |
| SHA | 3.243 | 2.881 | 1.12× | guest 状态搬运、GPR 搬运和部分真实 spill |

整数、访存、移位、普通浮点、浮点展开 ×8、排序及分离比较已经领先，全部进入回归保护集合。

### 1.2 因果实验与解释边界

下表来自独立诊断组，每组 15 个进程、每进程 5 次热执行。不同诊断组的绝对耗时受运行时段影响，比较使用各组自己的 control。[实验原始索引与补丁](evidence/arm64-x64-residual-20261112/gap-analysis-20261112-1846/manifest.json)

| 实验 | control → 变体，ms | 对实施方案的约束 |
| --- | --- | --- |
| 标量写入改为单次 V128 写入 | 标量矩阵 1.577 → 0.436，同组 Dynarmic 0.420 | 优先规范化前端表示，复用现有 BitCast 与 uniform 转发 |
| 覆盖 FABS 等直接拆分写入点 | fpmix 20.812 → 10.700 | 全部零高半写入点都需要审计 |
| LAHF 快照读取改为 word 宽度 | fpmix 20.812 → 17.970 | 先修复同槽 word 写入、qword 读取的宽度失配 |
| 全部标量写入与 word 读取组合 | fpmix 20.812 → 10.072 | 收益存在重叠，使用组合变体重新测量 |
| 强制部分 NZCV 使用完整 native flags | cond 66.725 → 25.009，同组 Dynarmic 36.747 | 诊断放宽了未写位保留语义，生产实现必须逐位跟踪权威状态 |
| 省略精确结果后查 | 向量矩阵 0.203 → 0.127，矩阵 FMA 0.191 → 0.156 | 诊断放宽了浮点边界语义，生产实现需要保留 NaN 与 MinNormal 触发集合 |

标量矩阵的实际栈 spill 数为零。PMU 中该负载的指令数比为 1.715，周期比为 3.759，单次 V128 写入原型显著降低耗时，支持状态转发失败及相关依赖停顿的解释。具体微架构停顿仍属于推断。

cond 的分支数与 Dynarmic 基本相同，分支误预测也没有明显增加。现有数据把优化重点指向 flags 保存、合并和恢复。采样中的冷分支邻近 IP 可能受中断滞后影响，冷路径执行频率需要真实计数确认。

## 2. 批次顺序与工程目标

下列运行时目标用于安排投入和检查候选质量，属于设计目标。原型只验证了基准输出，完整架构等价性仍需按第 10 节验收。累计收益以实测为准。

| 批次 | 内容 | 主要文件 | 运行时目标 |
| --- | --- | --- | --- |
| 0 | 建立可重复构建与冻结基线 | 构建输出、已有 evidence | 候选与基线可从完整源码独立重建 |
| 1 | 统一零高半标量 SIMD 写入 | ARM64 `semantics_registers.inc`、`semantics_float.inc` | 标量矩阵降至当前基线的 40% 以内 |
| 2 | 统一 flags 快照访问宽度 | x64 `emit_flags.cpp`、相关恢复与出口 | 与批次 1 组合后 fpmix 降至基线的 60% 以内 |
| 3 | 部分 NZCV 的逐位 native／saved 状态 | `emit.h`、`emit_flags.cpp`、flags 分析和恢复代码 | cond 降至基线的 75% 以内，继续接近完整 native 诊断结果 |
| 4 | 提前建立精确浮点语义 IR，专门化 FPCR 并直接消费比较条件 | ARM64 前端、IR traits／effects、x64 数值发码 | 进一步降低 fpmix、综合计算 FMA 的状态往返和调用残留 |
| 5 | AVX 精确 NaN／MinNormal 合并检查 | 宿主特征检测、`emit_float.cpp` | 向量矩阵降至基线的 80% 以内，矩阵 FMA 降至 90% 以内 |
| 6 | 指令约束和整数数据搬运优化 | x64 `allocate.cpp`、相关整数 emitter | SHA／综合计算优先争取额外 5% 改善 |

批次 1、2 优先形成完整可验收结果。批次 4 的浮点比较载体复用批次 3 的状态边界处理。批次 5 在精确检查共用入口实现，使已有 `ArmFPBinary` 和新增精确数值操作同时受益。批次 6 根据前五批后的重新采样选择具体发码点。

## 3. 批次 0 — 构建基线与身份校验

当前生产基线的 CodeBuildId 为 `045cbc5e2551348c2c212350f19a834302969b0afbf7c7cbb58613e281d50d42`。最新复测复用了已验证的 Release 二进制。工作区 `source/runtime/externals` 与 `source/runtime/frontend/arm32` 的目录读取异常会阻断正常重建，Git 显示的 311 项删除需要结合目录读取状态处理。[源码与重建记录](evidence/arm64-x64-residual-20261112/rebenchmark-20261112-1756/manifest.json)

实施前从已推送提交及本轮明确修改的文件建立完整可读的构建输入，校验依赖树、ARM32 文件、源文件散列和编译选项。冻结基线与候选使用相同工具链、Release／IPO 选项和基准输入。若重建二进制与现有冻结版本的散列不同，先完成全字段结果及性能对照，再确立本轮编译成本基线。

每批保留提交、CodeBuildId、CPU feature mask、二进制散列、构建命令、逐进程数据和测试日志。诊断补丁单独归档，并注明其语义覆盖范围。

## 4. 批次 1 — 零高半标量 SIMD 写入

### 4.1 实现路径

在 Known x64 的 `WriteVRegister` 标量分支中，将低位值经过现有 `Reinterpret`／`Widen` 规范化后构造 `BitCast<V128>`，再发出一次 `StoreUniform V128`。现有 x64 U64→V128 路径使用 MOVQ，能够清零高 64 位。低于 64 位的输入先完成位宽归一化，保证 Q 寄存器其余位清零。

审计 `semantics_float.inc` 与相关标量加载、FMOV、转换路径，把“低位结果加全零高半”的写入统一导向该入口。保留真正的高半插入、保留 lane、双半拼接及非零高半写入各自的语义。偏移始终通过前端现有布局映射生成。

优化后的形态为 `LoadMemory U64 → BitCast V128 → StoreUniform V128`，后续同位置 `LoadUniform V128` 使用现有 uniform 转发取得 SSA 值。前端输出已经满足整 Q 表示，转发表继续使用现有重叠与失效规则。[标量原型](evidence/arm64-x64-residual-20261112/gap-analysis-20261112-1846/scalar-vector-write-prototype.patch)

### 4.2 验证与完成条件

- 枚举 8／16／32／64 位值、整数与浮点 raw view、负值和高位污染，逐位检查最终 128 位结果。
- 覆盖 scalar LDR／LDP、FMOV、FABS／FNEG、整数和精度转换，以及读写同一个 Q 寄存器的情况。
- 覆盖低半、全 Q、相邻 Q、V256 重叠读写、private-state 开关、分离 state layout、helper 和 fault 观察。
- 检查热循环 IR 和发码，标量生产者到后续 FP 消费者之间的双半写入与整 Q 回读应消失。最后可观察的状态写回保留在正确边界。

该批复用既有优化遍历。编译成本重点记录节点数、operand words、uniform forwarding 数量和发布代码量。

## 5. 批次 2 — flags 快照宽度和格式

### 5.1 实现路径

首先修正 `MaterializeFlags`。LAHF／OF 格式使用 `movzx edx, word [slot]`，raw RFLAGS 格式使用 qword 读取。继续审计 `ExportLinkedFlags`、carry canonicalization、部分位合并、命名快照、fault recipe 解码和 signal 恢复中的同槽访问。[宽度原型](evidence/arm64-x64-residual-20261112/gap-analysis-20261112-1846/flags-slot-width-prototype.patch)

快照格式明确区分 LAHF／OF 的有效 16 位与 raw RFLAGS 的有效 64 位。所有按位更新使用所属格式的有效宽度。栈帧槽位容量与 recovery offset 继续服从现有 ABI，读取器按格式解码有效位。命名快照按自身公开格式处理。

审计时同时检查访存宽度与指令的 EFLAGS clobber。例如 canonical carry 的 XOR 会覆盖物理 flags，需要在状态跟踪中立即反映。临时寄存器保护、stack bias 和外部出口都纳入同一次检查。

### 5.2 验证与完成条件

对 raw／LAHF 两种格式、16 种 NZCV、两种 carry 极性、部分写掩码、helper、linked exit、fault 和命名快照做全字段对照。在槽位无效高字节写入不同污染值，确认解码只依赖有效位。复测 fpmix，记录指令数、周期数与宽度修复的独立收益。

该批属于 emitter 的格式选择，编译时每处增加常量条件判断，IR 遍历数和逐节点状态尺寸保持现有规模。

## 6. 批次 3 — 部分 NZCV 的逐位状态

### 6.1 状态表示

把 `Writer::FlagsState` 当前的整体 `native／saved` 有效性扩展为逐位权威掩码。IR 使用的位编号为 C=1、V=2、Z=4、N=8，与架构 NZCV nibble 的物理存储位置分别转换。

| 状态 | 含义 |
| --- | --- |
| `native_mask` | 当前物理 EFLAGS 中可直接代表对应 guest 位的集合 |
| `saved_mask` | 当前快照中具有权威值的集合 |
| `packed_mask` | 当前 packed guest flags 中具有权威值的集合 |
| native／saved carry 极性 | 各载体中的 C 与 guest C 的对应关系 |
| 物理 flags 类型 | 整数 NZCV 映射或批次 4 的浮点比较映射 |

同一 guest 位在逻辑上选择一个权威来源，掩码的并集覆盖所有仍有可观察需求的位。允许留有过期副本，其有效性由掩码明确约束。条件字节缓存另行记录其定义及有效范围。

逐位状态放在 Writer 和必要的冷路径快照中。先复用 Home 已有的 `flag_reads／flag_writes／flags_need`，记录 `sizeof(Home)` 的变化。块出口与 fault 的完整需求继续参与现有 flags 分析。

实际实现用现有 `dirty=false` 表示 packed 全量权威状态。部分生产者前先建立旧值快照，本地状态由 native／saved 两个掩码覆盖全部 NZCV。该表示复用 RBX 和现有快照格式，Home 保持 28 字节。

### 6.2 部分生产者和消费者

1. 在部分写入 W 前，按后续需求确定旧值中的存活补集。补集中仍由 native 持有的位，先捕获到快照或 packed 来源。
2. 执行宿主算术后，把 W 交给 native 来源。未写位的权威来源继续保留，宿主额外改写的物理位标为无效。
3. 条件所需的位集合 D 完全包含于 `native_mask` 时，直接使用对应 Jcc／SETcc，并按 carry 极性选择条件。
4. D 跨多个来源时，按需形成条件或建立统一快照。完整恢复安排在确实要求完整物理状态的消费者处。
5. 宿主指令即将覆盖 EFLAGS 时，先捕获仍存活的 native 位，再与快照中的互补位合并。合并过程本身的 flags clobber 进入状态更新。

例如 C／Z 部分生产者的 W 为 `0x5`，HI／LS 消费者只需 `0x5`，可以直接消费。N／V 生产者的 W 为 `0xa`，GE／LT 消费者只需 `0xa`。GT／LE 同时需要 N／Z／V，必须验证完整需求覆盖情况。

`discard_omitted_flags=false` 的生产者逐位保留未写位。只有现有活跃性事实明确允许丢弃时，才允许把宿主额外产生的位用于简化后续状态。

### 6.3 边界、异常和跨块

- 第一版在 CFG 合流、普通跨单元入口和外部出口建立统一的现有快照格式，降低路径状态组合复杂度。已有 qualifying native loop entry 仅在入口假设能够验证时继续使用。
- 内部标签合流清理路径相关的条件缓存，防止跳过捕获的路径读取旧条件字节。
- helper、event、dispatch、guest flags 观察和退出均发布完整权威状态。pending FPSR 在现有精确边界收割。
- fault recipe 复用现有 native／saved 混合掩码、carry 极性与 image16 表示。若仍有 packed 位，先把该部分纳入可恢复快照，再发布 recipe。
- x86 packed flags 的 PF／AF／DF 基础值、裸编译 raw 默认格式和显式命名快照继续按各自合同合成。

### 6.4 验证与完成条件

定向枚举 14 条件 × 16 种旧 NZCV × 部分写掩码，构造宿主额外结果与旧未写位相反的输入。补充 ADC／SBB、carry 反相、重复部分写、跨块 SSA 条件、循环回边、helper 返回、条件槽污染、部分写后 fault／event／静态链接。

完成后 cond 热路径中的部分生产者与只读其新写位的分支之间，应省去新结果的 PUSHFQ、快照合并和 SAHF 往返。保留旧存活位所必需的捕获仍由需求决定。耗时与完整语义候选对照，强制 full-native 诊断数据仅用于估计潜在空间。

## 7. 批次 4 — 精确浮点语义 IR、FPCR 与比较条件

### 7.1 提前到前端建立数值操作

当前 x64 在 `TryArmNumeric` 发码时识别 `CallLambda(ExecuteNumeric)`。此前 liveness、uniform effects 和分配已经按不透明调用处理，指令字、PC、state 地址、helper_status 写入及无用返回值仍可能进入热路径。

Known x64 前端直接输出拥有 ARM 精确数值合同的操作，识别发生在共享优化器之前。普通调用路径继续承担尚未覆盖的编码。已有 `ArmFPBinary` 扩展合法 mode 以支持 FDIV，并逐一更新验证器、解释／direct 路径和后端处理。现有 `ArmScalarFP／ArmScalarFMA` 的环境合同先核对，能够完整表达所需语义的部分复用其算法和 emitter。

需要显式 FPCR／FPSR 与精确冷叶子的操作采用与 `ArmFPBinary` 一致的合同。拟增加的最小语义族如下。

| 操作族 | 显式输入与结果 | 控制及副作用 |
| --- | --- | --- |
| `ArmFPTernary` | n、m、addend 的 V128 SSA 值，V128 结果，FMA／indexed mode | FPCR 读取、FPSR 累积，活跃 lane 与零高位规则 |
| `ArmFPUnary` | 输入 SSA、结果 SSA，SQRT／精度转换／位操作 mode | 数值模式累积 FPSR，FABS／FNEG／FMOV 按模式声明实际 effects |
| `ArmFPConvert` | 输入 SSA、目标位宽和舍入 mode，整数或 V128 结果 | 饱和边界、FPCR 和 FPSR，输出类型由已验证 mode 决定 |
| `ArmFPCompare` | 两个输入 SSA、比较 mode，逻辑 NZCV 输出 | quiet／signaling 行为、FPCR、FPSR 和 flags 写需求 |

每族的 mode 解码和合法性验证共享一份定义。FPCR／FPSR offset 显式传入。建 IR 时使用统一 traits 设置实际 effects，`UniformEffectsFor` 返回精确范围。寄存器读取由 SSA 输入表达，目的寄存器写入通过正常 `StoreUniform` 完成，现有转发和写回消除即可处理。

例如三元操作的合同为 `ArmFPTernary(n, m, a, fpcr_offset, fpsr_offset, mode) → V128`。比较使用 Void 数据结果，由 opcode 固定声明完整 NZCV 写入，消费方直接使用 IR 条件。前端直接发出比较操作，固定 flags 写入参与共享活跃性与 x64 需求分析。新操作同时补齐 opcode traits、Builder／direct 验证、效果分析、正确性执行入口和各后端的能力分流。

FCCMP 的条件和未执行时的 NZCV 常量由现有条件 IR 表达，执行分支上的比较继续使用同一精确操作。读取旧 flags 的条件守卫保留完整 effects。

### 7.2 精确冷路径的输入合同

冷路径直接接收该操作的 SSA 输入、mode、FPCR 和原始操作数位模式，复用 `EvaluateBinaryFP／EvaluateFusedFP／EvaluateUnaryFP／EvaluatePrecisionFP／EvaluateIntegerFP` 等数学叶子，并共用现有 NaN 选择规则。比较需要的逻辑 NZCV 计算复用现有语义算法。

分配时把冷边所需的原始输入计入该操作的约束，优先从仍可恢复的 SSA home 读取。破坏性复用会覆盖原值时，在热运算前保留必要输入，FMA 的三个输入及符号控制尤其需要逐项验证。额外保存指令计入该批性能成本。冷调用只依赖显式参数，结果返回到同一个 SSA home，异常位按既有 pending FPSR 合同累积。冷调用的寄存器保存、flags 状态和 resume 标签继续服从调用 ABI。

对每种新增 native 运算检查投机执行后转入冷路径时的异常累积。只有宿主已产生的异常位属于该 guest 操作应有异常时，才可直接与精确叶子结果合并。其余输入在运算前准入检查中分流到精确路径，保证异常精度与 pending MXCSR 的批量管理兼容。

优化后的 uniform 值可能已经只保留在 SSA 中，冷路径从显式操作数获取这些值。由此可同时消除热路径的整个 GuestState 调用参数、helper_status 成功写入与无用零返回值。

实施采用 `EvaluateFPValues` 适配层，将显式 SSA 输入装入局部 GuestState，并统一重编号描述符的寄存器字段，复用已验证的 `ExecuteNumeric` 数学语义。输入寄存器编号、目的寄存器编号和实际 SSA home 可以独立变化。该层通过独立参数接收 FPCR 值与 FPSR 位置，返回独立结果。相同规范化模式、控制／状态位置、活跃保存集合和 flags 策略共享冷调用 stub。稳定且已验证的 FPCR 域省去重复的失败出口，可变或未知控制状态保留逐站点的精确退出位置。

### 7.3 FPCR 专门化的具体范围

本批选择语义操作级专门化。IR 操作自身始终表示运行时 FPCR 下的精确行为，x64 后端根据 `CompileOptions.arm_fpcr`、单位内写入分析和现有域身份选择实现。共享优化器继续复用当前最多三次有界遍历，单位中增加的语义操作通过精确 effects 暴露可消除的搬运。

| 控制事实 | 实现 |
| --- | --- |
| AH=0 | FABS／FNEG 的精确一元操作在稳定支持域内直接选择位运算，省去动态 AH 与 NaN 符号选择链 |
| RMode | 使用当前域的数值环境，转换操作区分指令固定舍入与 FPCR 舍入 |
| FZ／DN | 选择已有准入、结果检查和精确叶子策略，保持 IDC／UFC／IXC 与 NaN 规则 |
| FZ16 | 半精度数值操作使用精确实现，位操作按自身合同处理，域身份包含该位 |
| FPCR 可能改变 | 该单元采用动态精确路径，写入后的操作观察实际 FPCR |
| unsupported 控制位 | 根据真实 FPCR 走精确路径，并由入口守卫阻止支持域专门化代码误执行 |

特别修正入口守卫的触发条件。只包含 FABS／FNEG 等已专门化一元操作的单元也可能依赖 AH=0。分配阶段记录该单元是否实际使用 FPCR 假设，发码阶段据此生成守卫，替换当前依赖 `cold_fp_count` 的触发条件。

32 个域继续按实际请求惰性创建，缓存、AOT 与静态链接使用现有 fp_mode 域身份。新增 CPU 能力及语义合同进入构建／配置身份。裸编译的 offset、未知控制和 legacy FP region 路径分别验证。

### 7.4 浮点比较的物理 EFLAGS 载体

增加浮点比较载体类型，记录 COMIS／UCOMIS 的 CF、ZF、PF 含义。普通条件消费者可直接读取，完整 guest NZCV 在 flags 观察、部分整数生产者、helper、fault、合流及外部出口前规范化。首版 fault recipe 始终接收现有整数 NZCV 映射或快照格式。

根据现有比较语义，四类结果的 ARM NZCV 分别为 less=1000、equal=0110、greater=0010、unordered=0011。由此推导物理 flags 到 guest 位的映射为 `N=CF && !PF`、`Z=ZF && !PF`、`C=!CF || PF`、`V=PF`。条件映射如下，实施时通过四类结果及原生 oracle 验证。

| ARM 条件 | 物理 FP EFLAGS 上的表达式 |
| --- | --- |
| EQ／NE | `ZF && !PF` ／ `!ZF || PF` |
| CS／CC | `!CF || PF` ／ `CF && !PF` |
| MI／PL | `CF && !PF` ／ `!CF || PF` |
| VS／VC | `PF` ／ `!PF` |
| HI／LS | `(!CF && !ZF) || PF` ／ `(CF || ZF) && !PF` |
| GE／LT | `!CF` ／ `CF` |
| GT／LE | `!CF && !ZF` ／ `CF || ZF` |

fpmix 的 GT 可直接使用 JA。需要两个谓词的条件使用已有条件字节机制，组合之前捕获所需原始谓词并更新 clobber 状态。终结路径若先发布状态再跳转，先捕获条件，再完成规范化和出口发布。

COMIS 与 UCOMIS 的 signaling 差异、FZ 输入准入和 pending FPSR 累积沿用精确操作合同。PF 参与 unordered 判断，浮点载体通过专用转换进入逐位整数 NZCV 状态。

### 7.5 验证与完成条件

验证标量／NEON／indexed FMA、FDIV、FSQRT、整数与精度转换、FABS／FNEG／FMOV 和全部比较条件。包括 NaN payload／sign／sNaN、signed zero、U64 高半与饱和边界、所有舍入方向、32 FPCR 域、AH 等 unsupported 位以及单元内 FPCR 改写。

强制触发冷路径并污染已被 SSA 转发替代的 guest 状态槽，证明冷叶子使用原始显式输入。覆盖死结果仍累积 FPSR、冷路径后继续使用结果、fault 前状态、别名布局和源码级独立入口。热路径中应消除相应 helper 参数、成功状态写入和 NZCV nibble 往返。

## 8. 批次 5 — AVX 精确结果检查

### 8.1 发码方案

在 FZ=0 的既有 `CheckArmFPResult` 中增加 AVX 实现。对结果清除符号位后，以 unordered-or-equal quiet 谓词同时检查 NaN 和 MinNormal。`_CMP_EQ_UQ` 的编码为 `0x08`，unordered 输入或相等输入均产生真。以下为待微测试和性能验证的候选形态，使用 128 位 VEX 指令。[LLVM AVX intrinsic 定义](https://clang.llvm.org/doxygen/avxintrin_8h_source.html)

```asm
vandpd    xmm14, xmm0, [abs_mask]
vcmppd    xmm14, xmm14, [min_normal], 0x08
vpmovmskb ecx, xmm14
jrcxz     ready
jmp       cold
ready:
```

F32 使用对应的 VANDPS／VCMPPS。参与检查的非活跃 lane 先按已有规则清零。候选普通路径为四条检查指令，当前 SSE4.1 路径为十条，常量装载形式和布局变化一并计入最终发码统计。常量内存操作使用具有所需非对齐合同的形式，并在非 16 字节代码基址下验证，必要时使用 VMOVDQU 显式装载。

PMOVMSKB 与 JRCXZ 保留整数 EFLAGS，便于与批次 3 的 native 位共存。FZ=1 继续使用现有输入准入和精确回退。两种格式的触发集合都覆盖 NaN 和正负 MinNormal。

### 8.2 特征和数值环境

增加独立 AVX feature。检测要求 CPUID 的 AVX／OSXSAVE 能力及 XGETBV 确认 XCR0 的 SSE、YMM 状态启用，FMA 的选择另外检查其能力位。有效特征进入 ISA／缓存／AOT 配置身份。SSE4.1 与 SSE2 实现继续用于对应能力组合。[Intel 架构手册的 AVX 检测合同](https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf)

核对检查产生的 MXCSR 异常位，特别是 sNaN 与 subnormal 输入。现有 DE 忽略映射及低 6 位清理必须一致。使用 VEX.128 时记录实际向量宽度需求，按单元与调用边界处理可能需要的 VZEROUPPER，避免在每次检查中插入环境管理。

### 8.3 验证与完成条件

微测试先枚举 F32／F64 位模式中的 NaN、无穷、正负零、MinNormal 两侧值和非活跃 lane 污染，比较新旧检查的分支结果、原始结果位、NZCV 保留和 MXCSR 低位。随后运行完整浮点差分，覆盖舍入前 tiny 但舍入后得到 MinNormal 的结果及 FZ／DN 组合。

特征矩阵覆盖 AVX+FMA、AVX 且无 FMA、SSE4.1、SSE2 和 OS 未启用 AVX 状态。通过编译选项掩码选择已验证的真实宿主子集，分别验证 JIT、重定位、缓存和 AOT。最终性能使用完整精确候选测量，后查删除实验仅作为成本参考。

## 9. 批次 6 — 指令约束与整数数据搬运

前五批完成后重新统计 SHA／综合计算的 guest state loads、GPR moves、真实 spill、uniform-backed home 和活跃区间。现有 pin0 分配池包含六个标量 home 寄存器，向量 home 为 XMM2—13。逐类区分 architectural 写回、可转发回读、约束搬运和真实溢出。

实施顺序如下。

1. 给已定位的热点整数操作补充固定输入／输出、隐式 clobber、可复用末次输入和寄存器／内存 RHS 约束，整合进已有分配观察与发码入口。
2. 优先去掉约束引起的重复 MOV，允许末次使用的输入直接承接结果，复用已有 resident RHS 和稳定 uniform backing。
3. 针对仍有明显 spill 的热点审计 scratch 占用与 cold-call 保存集合。只有完成所有相关 handler、fault map 和调用边界核查后，才把可释放寄存器加入 home 池。
4. 若剩余主要成本来自跨块 guest 状态写回，再实现小范围保留状态。先覆盖 private state、无中途观察的区域，明确 helper／fault／event／dispatch 的写回和恢复映射，逐步扩展到跨块入口。

每项分别记录减少的 MOV／guest load／spill 数以及冷编译变化。只有真实热点收益明确的改动进入该批提交，较大的跨块状态保留可以形成后续独立实施批次。

## 10. 正确性和性能验收

### 10.1 架构等价性

每批先执行对应的定向回归，再运行上一轮基线套件。新增测试集中在现有用例未覆盖的具体回归风险。

| 范围 | 基线与本轮补充 |
| --- | --- |
| CTest 与 portable compile | 上轮 31 通过、7 个其他宿主执行 skip，继续核对 skip 原因与编译合同 |
| ARM 基础与浮点差分 | 上轮共 57,168 次原生 oracle 对照，数值、FPSR、NZCV 和完整状态逐字段检查 |
| x86 兼容 | Block／Function 共 1,278 个用例的完整结果字段对照，同时保留既有 oracle gap 分类 |
| 完整程序 | ARM64／ARM32／Thumb 的 24 组已有配置及受影响的新特征组合 |
| flags | 部分写保留、两种 carry 极性、raw／LAHF、条件缓存、CFG、fault、event、linked exit |
| 浮点 | 32 FPCR 域、unsupported／mutable FPCR、冷叶子 SSA 输入、NaN／tininess 边界、pending FPSR |
| state 与布局 | 高 lane 清零、部分别名、V256 overlap、分离布局、private-state 与 pin 配置 |
| 缓存与 AOT | 冷写、暖命中、不同特征／域隔离、旧身份失效、第二轮零新增编译 |

Dynarmic 用于性能参照。数值与 FPSR 的正确性继续由完整语义叶子及架构 oracle 验证，当前采样所见的较短比较序列尚未建立跨引擎的完整 FPSR 等价证明。

### 10.2 运行性能

- 每批使用上一批完整候选作为局部基线，同时保留本轮冻结生产基线和固定 Dynarmic。所有候选在构建结束后统一测量。
- 15 项负载各执行 31 个新进程、每进程 3 次热执行，校验输出、暖编译复用和 fallback／解释／数据回调计数。cond 另作每进程 9 次热执行的确认组。
- 三个引擎使用均衡轮换及反转顺序。两个变体使用交替 AB／BA，检查最终调度表的实际先后分布。
- 报告逐组配对比值中位数和四分位区间，短负载补充较长执行窗口。已经领先的七项负载沿用 2% 回归复查线。
- 对目标负载采集 7 组 PMU，核对 event enabled／running 时间。必要时补充采样，报告 lost samples、归类规则和中断滞后的归因限制。

工程目标未达到时，以发码、PMU 和冷路径计数检查原因，再决定调整或保留该优化，记录与设计目标的偏差。

## 11. 编译成本与交付

### 11.1 复杂度控制

| 批次 | 预计编译影响 | 实施约束 |
| --- | --- | --- |
| 1 | 节点与发码有望减少 | 复用既有 BitCast、StoreUniform 和转发遍历 |
| 2 | emitter 常量分支，成本接近持平 | 快照格式在编译时确定 |
| 3 | 增加固定大小掩码运算 | 复用 flags demand 与 CFG 数据，逐节点保持常数成本 |
| 4 | 前端语义分类增加，调用残留与分配工作减少 | 前端一次识别，精确 effects 进入既有优化遍历，32 域按需创建 |
| 5 | 每次检查发码有望减少 | CPU 能力统一检测，检查模板在编译时选择 |
| 6 | 约束分析有额外常数成本 | 先整合现有观察遍历，记录 home 尺寸与分配次数 |

本方案的 review、全量反汇编、差分和 PMU 采样都属于开发验收过程。生产编译增加的工作以固定掩码、共享语义分类和现有遍历中的约束处理为主。当前仍需实测确认批次 3、4、6 的常数成本。

### 11.2 成本测量与复查线

沿用上一轮完整程序冷编译总时间 3% 的复查线，以批次 0 重建并确认的本轮生产基线为参照，同时报告各批增量和最终累计变化。分开记录 frontend、optimize、backend、publication、编译请求数、IR 节点、operand words、工作内存与发布代码字节。

增加 N／2N／4N 的 uniform、flags、FP 和 cold-path 规模输入，与同一基线成对运行，报告每请求时间、每节点成本和峰值工作内存。某类 4N 成本增加超过 5% 时，分析新增遍历、状态尺寸、散列表与临时分配，并在结果文档说明保留或调整的依据。

上一轮完整程序冷编译总量下降 2.9%，backend 阶段增加 15.3%，4N probe 增加 5.7%—16.5%。本轮同时检查总量和分项，避免单一总量掩盖热点编译成本。[上一轮编译成本](arm64-x64-residual-optimization-results-2026-11-12.md)

### 11.3 提交和最终产物

按批次 1、2、3、4a 语义 IR 与冷路径、4b FPCR 与比较载体、5、6 组织可独立检查的提交。每批附定向回归和统一性能记录，文档随实际边界修订。新增 opcode、flags 状态合同、CPU 特征和 cache／AOT 身份在对应批次一起更新。

最终交付包含实施代码、必要回归测试、逐批与累计性能对照、冷编译及规模结果、关键热路径反汇编、源码与二进制身份，以及更新后的本方案和实施结果。尚未达到的目标与仍保留的精确慢路径逐项列明。

本轮实际验收按批次 1+2、3+5、4+6 的组合检查点及最终源码补丁归档。最终冷编译配对中位数增加 2.6%，发布代码量增加 18.9%。扩大寄存器池与跨块状态保留继续作为后续独立批次，SHA 额外 5% 的工程目标仍待推进。[实施结果与归档](arm64-x64-gap-optimization-results-2026-11-13.md)
