# ARM64 编译流程维护性整改方案

日期：2026-10-28，已结合固定 FEX 源码的实际行为修订。本轮整改已实施并完成既定验收。本文保留两种时态：第 2 节是整改前的**基线审查**及原始理由；第 4 节和[逐文件职责文档](arm64-compile-pipeline-layout-2026-10-28.md)记录**实际实现**。第 3、5、6 节的归属规则、删除标准和验收条件继续约束后续修改。

后续性能工作统一见[根因、实施顺序与采用门](spill-and-compile-root-cause-plan-2026-10-28.md)：不降低生成质量，优先减少构造和跨阶段表示的真实工作，Block/Function 分别≤2×固定 FEX。本页的维护性验收没有普通 CoreMark 运行质量或该性能目标的通过资格，已退役的中间层也不重复计收益。

实施节奏按2026-10-29用户补充执行：重构期间允许生成质量暂时变化，按完整功能批次推进，只做构建及确有必要的受影响合同检查。下文完整质量、lifetime和两模式检查是阶段收口/最终采用要求，不是每次编辑或每个中间切片的前置条件；不在每片后反复跑质量、资源、PMU或FEX矩阵。历史已执行的验证记录不因此改写。

审查范围为 `source/runtime/backend/arm64/compile`，并追踪 `jit` 发码消费者、公共 allocator 和 prepared 结果的生命周期。最终版本 `layout-r7` 已构建全部16个Android目标，完成54项既定调用、151个扩充用例、33次Function对照及30项独立lifetime检查。在本轮验收范围内无新增失败或代码生成退步；22条历史断言失败逐项保持，具体证据、比较边界及原失败记录见[实施进度](arm64-compile-pipeline-consolidation-progress-2026-10-28.md)。此前“compile 成本达到 FEX 同数量级”的研究、测量及结果另行保留；本方案不恢复该历史性能目标，也不报告新的性能对比结论。

## 1. 整改目标与边界

整改起点的主要问题是最终决策的归属分散：一个选择在 allocation 状态、临时 action 包、最终 action、proof bits 和 emitter 条件判断中多次表达。新增一种指令形式时，往往需要同时更新这些表示、搬运代码、覆盖关系和封口逻辑。

本次整改以“跨指令、跨阶段决策只保存一次，普通操作直接依据 IR 和分配结果发码”为目标。目标 plan 只承载额外的跨节点选择、覆盖及其合同；固定 opcode、operand、mapping 和 feature 即可完成的局部编码，可以留在 emitter。文件整理应在数据流收敛之后进行，不能以减少文件数或 pass 名称代替实际删除重复工作。

必须保留：

- Block 与 Function 各自的分配算法、入口和验收；单块 Function 的局部 fast path 仍须满足既有准入条件。
- SSA、固定 home、宽度与高位清零、用途窗口、scratch、spill transport、helper clobber 等正确性合同。
- FPR admission 失败后的物理分配重试，以及失败 attempt 不泄露半完成结果的事务边界。
- Function 的跨块 incoming state、CFG、公共入口及 provenance 合同。
- prepared/encoder 借用结束后再释放 IR 的生命周期，以及 fault、SMC、恢复和实际代码发布边界。
- optional audit 的独立参考实现，不能改为读取 production plan 自证正确。

不把以下行为误判为重复 pass：

- `DescribeBlock` 已在共同遍历中描述多类分配输入，见 [registers/constraints.cpp](../source/runtime/backend/arm64/compile/registers/constraints.cpp#L157)。
- `PrepareValueLiveness` 的重复查询可命中同一个 `SemanticFacts` 缓存，见 [facts.cpp](../source/runtime/compile/analysis/facts.cpp#L308)。
- 封口输入的 SSA 检查可复用缓存；手工可变输入仍重新检查，见 [ssa.cpp](../source/runtime/compile/analysis/ssa.cpp#L311)。
- final fixed validation 默认复用 inputs 阶段的描述；不匹配或手工请求才重建，见 [compiler.cpp](../source/runtime/backend/arm64/compile/compiler.cpp#L74)。

历史链接用于回溯基线；本地源码链接指向当前文件。行号仅用于定位，维护时以函数名和实际数据消费者为准。

### 1.1 FEX 的实际流程与借鉴边界

本次对照本地 `/Volumes/Data2/swiftvm-middle-build-cache/fex-android/source`，HEAD 为 `a156752fae3a4f65fefb1ed95af980bcdfac43d7`。下列链接固定到该版本，不能套用其它版本的 pass 列表。

```text
取得/复用 buffer → decode → dispatch 建 IR、Finalize
  → x87 stack optimization、dead flag elimination（受 O0 开关控制）
  → RA：块内分析、分配、copy/spill/fill、局部合并及物理 operand 改写
  → PostRA 表示 → 同步 JIT 发码，其中仍有局部编码选择
  → 结束 IR 借用、disown buffer → 代码发布相关处理
```

- 默认优化列表见 [PassManager.cpp](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/PassManager.cpp#L69)，RA 在初始化时另行插入。`Run` 先执行整个 `Passes` 列表，包括 RA，再执行受 `ASSERTIONS_ENABLED` 控制的 validation 列表；不是每个 pass 后重新验证。插入顺序见 [Core.cpp](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/Core/Core.cpp#L428)。该版本没有独立的 IR Compact pass，不能写成 `ConstProp → RA → Compact`。
- 一个 RA pass 内仍有逐 CodeBlock 的逆序分析和正向分配；各块开始时重置可用寄存器，见 [RegisterAllocationPass.cpp](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp#L576)。SRA 的 guest-state 固定 home 不等于跨块任意 SSA 活跃区间分配，不能据此替换 SVM Function allocator。
- FEX 将部分选择直接表达为 typed IR，并在 RA 中写入物理结果。RA 会插入 copy/fill、删除或合并操作、将可分配 operand 改为物理寄存器表示，见 [同文件](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp#L652)。inline constant 等仍有非寄存器表示。
- RA 完成后清空 `PreferredReg`、`SSAToReg`、`SpillSlots`、`NextUses`、`Seen` 等工作集，再标记 `PostRA`，见 [同文件](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp#L747)。值得借鉴的是工作集有明确退出点；SVM 的最终结果仍需持有 emitter 实际消费的 Allocation 数据，不能照搬其清空名单。
- FEX 并未为所有机器编码预建 plan。访存操作到发码时仍根据 offset、scale 等产生 operand，scalar vector 操作仍根据物理寄存器重叠和 AFP/SVE 特性选择 copy、temporary、insert，见 [MemoryOps.cpp](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/Core/JIT/MemoryOps.cpp#L549)、[VectorOps.cpp](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/Core/JIT/VectorOps.cpp#L293)。

借鉴重点是操作语义、分配约束和阶段产物的明确归属。SVM 此次保留 RA 前封口的 source IR，不照搬 FEX 的破坏性 RA，也不为模仿 FEX 另建一套覆盖所有普通 IR 的机器计划。FEX 没采用的 SVM 优化及其证明，同样不能因此退役。

FEX 的同一个 RA pass 内存在逆序 kill/SRA 分析和正向分配、修正，两者有真实依赖。“一个 pass”不代表“一次遍历”。SVM 应按依赖和数据寿命合并工作，不能用 pass 名称或文件数推断可删除的扫描。

## 2. 基线审查：原数据链中的主要问题

以下保留整改前 WIP 的发现与原始重构理由，不作为迁移后的现状描述。历史源码链接固定到 `660b6ba14e3c8b6e6e21dc601fb66072b07b191f` 的对应文件；完整 WIP 审查基线另已封存，该 Git 提交不替代验收用的完整工作树。当前目录与接口见第 4 节及[逐文件职责文档](arm64-compile-pipeline-layout-2026-10-28.md)。

### 2.1 跨指令省略与物化决定由 emitter 拼装

审查基线中，`EmitZeroExtend32To64` 同时查询 residence 的零扩展省略标记、scalar tied-zero-extend、allocation width-chain 及其 proof、source low32-copy 及其 proof，再根据物理寄存器是否相同决定发什么。见 [translator_alu_scalar_bits.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/jit/translator_alu_scalar_bits.cpp#L428)。

应在最终物理映射确定后，统一跨指令省略和物化义务的来源，保存额外选择所需的理由与认证状态。emitter 不再组合多张跨指令状态表来判断是否还欠一次零扩展。普通路径依据 IR 语义、源宽度和最终 mapping，局部选择写 W、复制或扩展编码，不必为每个分支新增 plan row。

不能收敛成一个 `omit` 布尔值。基线的 [低 32 位合并路径](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/jit/translator_alu_scalar_bits.cpp#L449) 明确要求：coalesced BitExtract 后即使共享寄存器，仍可能必须 `Mov W,W` 清除高位。

迁移范围不能只覆盖 `EmitZeroExtend32To64`。`IsGPRZeroExtendElided` 还参与 [`Translate` 对 `TickIR` 的调用](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/jit/translator.cpp#L798)，影响 preservation/materialization 的执行节奏；[`CanConsumeForwardedWidthSpill`](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/jit/translator_alu_scalar_bits.cpp#L299) 也会重新组合相关状态。跨指令的静态结论应由这些消费者共同读取；是否存在 pending spill、当前是否需要 preservation 等动态事实仍在发码时判断。

`SVM_ZEXT_NO_ELIDE` 的现有含义也须保持：它绕过部分顶层省略选择，但最终仍可能经 `EmitZeroExtend32` 的既有路径省略操作，不能在重构中重新解释为“一律强制发出 Mov”。正常模式和诊断模式分别核对原有行为。

整改标准是收敛跨指令决定及其 provenance；保留 allocator 为计算物理位置所需的内部状态，也保留无需 SSA 遍历、用途窗口分析或分配修改的局部编码 helper。

### 2.2 验证结果经过同义容器再次投影

基线的 width 链路为：

```text
allocation 的最终物理状态
  → WidthContractCheck
  → WidthCertificates 的 width / low32 / reads 三个 ID vector
  → Finalize 三轮逐 ID 搬运
  → CertifyWidthElision / CertifyHostRead
  → UnitActions::proofs
```

证据为 [width_contracts.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/width_contracts.cpp#L520)、[compiler.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/compiler.cpp#L99) 和 [actions.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/actions.cpp#L150)。

最终 checker 必须保留，因为它验证实际 home、transfer owner、W-write、高位清零、clobber 和 terminal use。可以删除的是三个中转 ID vector、转发方法和搬运循环：checker 通过受限写口直接认证私有构建中的最终决策，整个 attempt 成功后才对外提供结果。

FPR 也有 `FPRContract.instructions → CertifyFPR → proof bits / shuffle controls` 的投影，见 [allocation_preparation.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/allocation_preparation.cpp#L203)。收敛时必须维持所有 block 的 FPR admission 先成功、再开始 GPR edits 的顺序。私有认证结果不能在失败时成为可发码结果。

### 2.3 内存选择被保存、排序、复制、再次排序

基线的主要链路为：

```text
MemorySelectionBuilder
  → MemorySelections::Finish：排序 transfers / addresses
  → StoreMemorySelections：逐条复制到 UnitActions
  → 局部 block_memory 继续供 PrepareMemory 查询
  → UnitActions::SealPayloads：再次排序
```

见 [memory_selections.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/memory_selections.cpp#L130)、[allocation_preparation.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/allocation_preparation.cpp#L431)、[lower.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/lower.cpp#L153) 和 [actions.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/actions.cpp#L291)。

应让最终 plan 的构建者从开始就拥有这些记录，block resolver 和 memory resolver 借用同一数据；最终只整理、封口一次。保留稀疏记录、非单调输入和重复身份检查，不能假设 `Inst::Id` 总是稠密位置或访问顺序。

若 `MemorySelections` 改为最终 owner 的 range/index 借用，须明确其有效期：构建期跨 block append 不能缓存会因 vector 扩容失效的元素指针；封口排序后，旧的位置索引不再有效。构建期借用在排序前结束，最终查询使用封口后的索引。必要的角色索引不拥有第二份 payload，也不以提前排序维持旧 view。

下游还有同一静态地址 recipe 的再次建表：`AddressAction` 已记录 EA/access/home/offset，`EmitGetOperand` 遇到 spilled EA 后又建立 `SpilledMemoryOperand` map，访问时再查表和选择编码。见 [translator_control.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/jit/translator_control.cpp#L589)、[translator_operand.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/jit/translator_operand.cpp#L10) 和 [translator_spilled_memory.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/jit/translator_spilled_memory.cpp#L22)。

建议对跨节点地址融合、writeback、load→extend、改变 home、提前 publication 或覆盖其它 IR 的访问，建立唯一的稀疏 memory override。该记录只保存原 IR 和 Allocation 尚未表达的新增决定及 witness；EA、access、update、publication 的角色索引指向同一记录。普通 access 继续以 sealed IR 的 Operand/type 为依据，不全量复制 Width/Class/Operand。逐步退役 emitter 内重复的静态 recipe 表。

spill/reload 的静态位置、slot、来源及 preserve 原因由分配结果确定；pending spill 的消费进度、当前 scratch lease、实际 host PC、veneer 和 fault offset 才是发射时事实。两者的职责与寿命见 3.5、3.7。

退役 `spilled_memory_operands` 时还须分离一项动态事实：原 map 只在 `EmitGetOperand` 确实省略了 spilled EA 的物化后加入记录，后续按 exact consumer 使用。静态 override 的存在不能替代“本次发码已经执行到并省略了该定义”。必要时保留只记录进度的轻量标记；recipe、home、offset 和 consumer 身份仍从唯一静态结果读取。`PendingScalarSpillValue` 的实际消费状态继续保留。

### 2.4 物理写回 witness 经不同结构反复转译

post-index 等内存选择同时输出 `MemoryAction`、`MemoryActions::Write` 和 `covered`；`lower.cpp` 再把写回转换为 `PhysicalGPRWrite`，最后借用 `EmissionCertificateInput` 交给 fixed verifier。见 [lower_memory.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/lower_memory.cpp#L250)、[lower.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/lower.cpp#L217) 和 [certificates.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/certificates.cpp#L11)。

应直接生成公共 `PhysicalGPRWrite`，或由同一 memory decision 提供只读投影视图，删除 `MemoryActions::Write` 和转换循环。提前写哪个 home、消耗哪个值、替代哪个后续使用，是必须保留的完整合同，不能压缩成一个 `PostIndex` 标记。

可以直接写入既有 draft 的 writes，不为消除转换另建一个 owning 容器。保留公共 witness 的完整 `u32` instruction/consuming-use 身份；不能将 block-local 的窄 row 编号当作全单元 ID，也不能通过结构重解释绕过字段合同。

`EmissionCertificateInput` 本身只是小型借用视图，可以保留；不要为了删除一个 view，又建立更大的 owning 容器。

### 2.5 coverage 和准备状态有多个管理者

各 selector 返回自己的 `covered`；`lower.cpp` 维护跨 selector 的 `flag_coverage`；`Store*` 再将覆盖 ID 写入最终 proof bits。见 [lower.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/lower.cpp#L148)。

应由同一构建者管理选择、覆盖请求、冲突及依赖。selector 提交类型化动作和覆盖意图，后续 selector 查询同一状态。保留 memory、flags、publication 等正交通道，不强制一条 IR 只有一个 action。

顺序依赖不能因合并遍历而消失。memory、branch、integer flags、narrowing 的覆盖优先级，以及 pinned-load/update 等 matcher 对物理状态的修改，需要先显式化。尤其保留“选择第一个可编码 update 后，窗口拒绝就回退，不另选后续候选”的既有规则，见 [memory_preparation.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/memory_preparation.cpp#L23)。

同一 coverage owner 不等于所有覆盖立即对所有 selector 可见。当前 integer-flags 与后续 narrowing 所见的覆盖集合有阶段差异；迁移时应明确查询通道和提交顺序，防止将已经记录、但原阶段尚不可见的覆盖提前用于排除候选。该顺序由集中编排表达，不继续复制多套 owning bitset 维持隐含协议。

基线的准备状态同时体现于 Allocation 的冻结/attempt、AllocationWriter 的编辑标记、UnitActions 的 building/ready，以及 UnitCertificate 的身份副本。`BeginTargetPlanning` 与 `AbortTargetPlanning` 还分别维护大量清空代码，见 [allocation_preparation.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/allocation_preparation.cpp#L48)。

由一个准备 owner 负责状态转换和 reset/abort。物理位置封口、验证成功、最终可发码是不同状态，应明确表达。基线中非法 SSA 返回 `Ready`、再由 certificate 拒绝的协议，应改为显式 rejected/retry/success，并保持原有拒绝与 fallback 行为。

### 2.6 prepared 所有权正确，但在消费者入口被拆散

基线中的 `Arm64PreparedUnit::Storage` 已持有 IR、Allocation、UnitActions，成功后才接管 IR；这是应加强的所有权边界，见 [compiler.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/compiler.cpp#L195)。

Function emitter 又将 prepared 拆成 `{function, allocation, actions}` 裸 view，随后重新组合准入条件，见 [function_code_object_emitter.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/jit/function_code_object_emitter.cpp#L15)。正常入口应直接借用现有 PreparedUnit，在同一个只读借用期间完成准入和发射。

不能直接删掉 UnitCertificate 的 CFG、入口及 provenance 镜像。其 generation 来自 `GPRLocationGeneration`，不是 IR/CFG 版本，见 [admission.cpp](https://github.com/ganyao114/SwiftVM/blob/660b6ba14e3c8b6e6e21dc601fb66072b07b191f/source/runtime/backend/arm64/compile/admission.cpp#L78)。先建立完整的封口、所有权和失效保证，再收敛重复快照。手工可变输入仍须等价失效检测。

`OptimizedUnit::TakeIR` 会释放分析缓存；最终 plan 不得借用已经销毁的 SemanticFacts。若需要保留某个不可变结构，必须明确将其所有权转入 prepared，不能只留下原缓存中的指针。

PreparedUnit 内的静态准入与 publication/SMC/retirement 的晚期有效性检查，是两个边界。前者保证编译结果与输入、物理映射和入口合同匹配；后者检查发布时仍可变化的代码、映射和运行时状态，不能因前者收敛而删除。

## 3. 收敛后的数据与阶段

### 3.1 保留三个明确的数据来源

| 数据 | 职责 | 生命周期及修改权限 |
| --- | --- | --- |
| SemanticFacts 与分配请求 | 封口 IR、用途、CFG，以及候选所需的分配约束 | 可跨物理重试复用；分析缓存不逃逸到 emission |
| Allocation | 物理位置、spill slot、静态 reload/rematerialize/preserve 结果、attempt | 只有分配编辑者可修改；最终物理验证基于此结果 |
| 稀疏 ARM64 plan | 原 IR 与 Allocation 之外的跨节点选择、覆盖、资源动作及认证状态 | 私有构建，成功后只读，由 PreparedUnit 持有；普通操作不额外建记录 |

当前实现沿用 `UnitActions` 作为唯一最终结果，原 `AllocationPreparation` 已直接改名为 `PlanBuilder`，两者的必要合同统一在 [plan.h](../source/runtime/backend/arm64/compile/plan.h)。这没有新增一层 owner。不要同时保留 UnitActions、UnitPlan 和另一套最终 CertificatePlan。候选可以是临时数据，但必须说明其保存的是尚未确定的条件；最终形式确定后，退役同义候选副本。

保留稀疏 payload 和必要的快速查询 bits，不把所有领域塞进一个巨大的每指令结构。仅有字段稀疏还不够：不能为所有普通 opcode 各建一条同义计划。构建者的受限写口用于维护真实不变量，不新增持有层、虚接口层或纯转发 DTO。

### 3.2 按依赖划分决策，保持 source IR 封口

```text
封口 IR / 共享 SemanticFacts
  → 描述会影响分配的目标约束及必要候选
  → AllocateBlock 或 AllocateFunction
  → FPR 物理 admission；失败则重试物理分配
  → 裁决依赖 mapping 的候选；纯物理合并可直接形成最终稀疏记录
  → 完成有明确原因的物理修正及静态 transport，关闭物理编辑
  → 验证最终 fixed / width / spill 等合同
  → 完成边、终结器及依赖最终 coverage 的剩余静态选择，封口为可发码结果
  → 在 PreparedUnit 只读借用内准入
  → emitter 消费 IR、分配结果和稀疏决定，完成局部编码
  → 记录实际偏移/fault metadata，进入独立的发布有效性检查
```

分配前不预言尚未确定的 home、spill 和 coalescing。影响分配的候选共享语义发现结果；只依赖最终物理关系的合并，可以在后 RA 阶段直接产生最终记录，不强制提前建立同义候选表。纯局部编码直接留在 emitter。物理重试保留 immutable inputs，清除本 attempt 的选择和认证结果。

本方案保持 source IR 在 RA 前封口。后续物理修正仅修改 Allocation 和目标结果，不暗中恢复 source IR 可写。若将来设计真正的物理 IR，必须单独设计表示迁移及原 IR 的退出边界；此次不新增 LoweredIR、MachinePlan、CertificatePlan 等并行最终表示。

最终 verifier 可以直接认证同一计划记录，但仍须根据 IR、实际分配和合同检查它；不能把已有 checked bit 当作正确性依据。optional audit 继续独立构建参考。

Function 所有相关物理修正稳定前，不能逐 block 提前封口并发码。不要为了“一次遍历”破坏全单元验证、跨块状态或多 region 的共存边界。

### 3.3 决策归属与新增记录的准入

| 事实或决定 | 归属 | 不应新增的同义表示 |
| --- | --- | --- |
| 操作语义、逻辑宽度、地址类型、直接 operand、可内联常量 | 原 IR、既有语义分析和目标规则 | 覆盖普通 IR 的完整 TargetOp DTO |
| operand/result register class、fixed/clobber、destructive 关系、分配预算 | 分配前目标约束 | 在后 RA 修正中重新发现同一硬约束 |
| physical home、spill slot、静态 reload/rematerialize/preserve | Allocation 的最终结果 | selector 与 emitter 各自重建的 transport 表 |
| 跨指令融合、省略、提前写回、覆盖、改变恢复窗口的选择 | 唯一稀疏目标记录及 witness | 候选、最终 action、proof-ID 中转等长期平行副本 |
| 固定 opcode、直接 operands、最终 mapping/feature 即可决定的编码、copy/temporary 序列 | emitter 局部 helper | 为普通编码分支逐个预存的 plan row |
| 当前 scratch lease、pending spill 消费进度、label、实际 host PC/fault/relocation offset | emission/runtime | 被提前伪装成确定值的静态分析字段 |

emitter 可以读取已封口 operand 和物理结果做局部展开，但不能重新执行跨节点 pattern、用途/观察窗口分析、修改 allocation，或重新决定覆盖关系。本地编码所需 scratch 预算仍须由同一资源规则覆盖，不能因为允许局部选择而绕过资源合同。

新增目标记录时，应说明：原 IR/Allocation 缺少什么决定；是否影响分配；谁消费、何时失效、失败如何回退；它将删除哪些旧表、转换或回扫。能稳定 O(1) 从既有来源获得的普通字段，不因“统一 plan”而再复制一份。

例如，已知零值是否需要复制到结果 home，可直接查询普通物理事实，不应再建立同义 action；由整条地址定义链和已发射写入历史推导的静态对齐，则属于跨节点分析，应在最后一次相关 coverage 和物理修正之后求解，只有会改变默认访存展开的结果进入已有稀疏 memory 记录。不能为迎合共同遍历而提前计算，随后再复制、修补或重算。

静态分析与晚期激活要分别看待。backedge flags 的实际导入仍依赖 code storage、ABI 和发码状态；该激活门保留在 emitter。若是否省略检查还受这种激活影响，准备结果必须准确表达两种有效行为，不能预先认定优化必然激活，也不能通过多加检查掩盖分析丢失导致的代码质量退步。透明 CFG 链中的静态入口要求，同样应先区分可预计算部分与必须晚期判定的导入条件。

### 3.4 物理编辑和失败分类

FEX 的 `TiedSource` 是尽力合并，失败后 JIT 可插 move，见 [RegisterAllocationPass.cpp](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp#L383)。guest-state 提前写回则涉及 RAW/WAW，必须阻止不安全合并或提前保存旧值，见 [同文件](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp#L608)。两者不能共用一个含糊的 admission 失败含义。

| 类型 | 正确处置及准备边界 |
| --- | --- |
| coalescing 偏好未满足 | 有资源预算的合法普通形式；不等于硬约束失败 |
| destructive operand 要求 | 受约束分配或明确 copy；具体编码可以局部展开 |
| 旧 guest 值可能提前被覆盖 | preserve、禁止该合并或重试；不能假定在消费点插 copy 就能救回旧值 |
| 早已能描述的 fixed/tie/clobber 条件 | 在分配请求中明确，减少后 RA 补救 |
| 真正依赖最终 home、spill、alias 的残余选择 | 后 RA 裁决；记录改动及其失效影响 |

实施前逐项登记现有 post-RA Allocation 编辑：调用者、修改的映射/窗口、必须后置的原因、受影响的证明及回退方式。保留仍有必要的修正，将本可提前声明的要求回收到分配约束。第一阶段只整理归属与失败原因，保持当前 FPR retry、指令选择和 fallback 行为；改变失败处置策略另行验证，不能混入机械迁移。

`Rejected` 返回值不取消现有诊断模式：例如非法 SSA 在普通模式下形成不可发码的拒绝结果，`SVM_VERIFY_PLAN=1` 下仍可直接抛出诊断异常。负例分别核对这两种行为，并在测试内恢复环境；不能为统一返回值删除严格检查。

### 3.5 静态资源结果与动态发射状态

FEX 在 RA 中插入带 slot/class/size 的 spill/fill，后端消费已确定的访问，见 [RegisterAllocationPass.cpp](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp#L718)。SVM 先沿现有 [SpillReload/GPRPreservationAction](../source/runtime/compile/allocate/allocation.h#L56) 收敛，不为此新增完整 MIR。

| 数据 | 唯一生产者及消费方式 |
| --- | --- |
| spill/fill 位置、slot、来源、宽度、preserve 原因 | Allocation/资源规划产生，emitter 执行，避免重新发现值身份与用途窗口 |
| opcode/feature 固有 clobber、临时寄存器需求 | 共用声明性资源规则，供分配预算和最终编码使用 |
| 依赖实际 MEM、consumer point、重复输入、最后用途的 transport | 最终分配和选择确定后形成，不能用分配前估计冒充 |
| 当前 scratch 占用、lease 获取释放、pending spill 已消费状态 | emission 维护；查询静态结果，不重新规划同一资源动作 |

验证的输出也遵循同一原则：flags-only spill 检查通过后直接写唯一私有结果，删除仅用于返回、再导入的 `EmissionCertificates` 容器。零存储证明的 pending ID 则具有真实阶段用途：收集时不能公开 proof 或关闭物理编辑，须保留到原最终认证点；可删除每块临时 vector 及拼接，不应为少一个容器提前执行 `CertifyZeroStore`。

### 3.6 合同建立、失效与最终消费

保留正确性义务，同时明确证明的有效期。已经在正确阶段建立、且依赖随后不可变的证明，可以通过同一 owner 和依赖身份复用；没有证明失效条件受控时，不能只凭 checked bit 放行。

| 合同 | 建立阶段/数据来源 | 依赖及失效修改 | 最终消费 |
| --- | --- | --- | --- |
| SSA、用途、CFG | 封口 IR 的 SemanticFacts | IR/CFG/成员身份改变；可变手工输入须重新核对 | 分配/目标准备及 prepared 绑定；封口缓存可复用 |
| 固定 home、提前写回、preservation | 分配约束及最终物理验证 | mapping、写回窗口、consumed use、helper clobber 改变 | 目标选择和 emitter 读取有效结果 |
| width、高位清零、物化省略 | 最后一次相关物理编辑和选择完成后验证 | home、owner、W-write、覆盖或用途窗口改变 | 跨指令省略的唯一决定 |
| FPR 合并、上位保留 | 当前 FPR admission；迁移期保持原事务顺序 | FPR mapping、操作语义、feature 或相关 transport 改变 | 认证结果；失败不得泄露半完成 GPR 计划 |
| scratch/spill | 共用规则、最终 mapping 与消费点 | pool、编码路径、MEM/last-use/transport 改变 | 静态需求及发射时实际 lease 检查 |
| 单元成员、入口、provenance | prepared 发码准入 | CFG/成员/入口状态或绑定变化；GPR generation 不是 IR epoch | 同一只读借用内消费；可变输入保留等价检测 |
| 实际 fault/relocation、发布有效性 | 发射及发布阶段 | host layout、guest code、映射、SMC/retirement 状态变化 | code object 和运行时发布/失效处理 |

实现上区分构造/分配算法维持的不变量、最终物理生产合同、封口后可复用证明、optional 独立 audit，以及发射时动态检查。复用声明性规则不等于让 verifier 直接相信 selector 的结论。

FEX 的 debug IRValidation 与其 RA 中实际执行的 hazard 限制属于不同机制。不能概括成“FEX 没检查”，也不能据其 assertions 开关关闭 SVM 的生产合同。

### 3.7 生命周期、重试与发布边界

| 数据 | 应存活到何时 |
| --- | --- |
| SemanticFacts、immutable allocation inputs | 全部物理重试结束；借用缓存的 view 在 TakeIR 前结束，除非明确转移所有权 |
| attempt 私有选择与认证结果 | attempt 失败即失效；复用容量不能保留旧决定 |
| 分配/选择的工作索引、候选及中转数据 | 各自最后一个真实消费者结束；不因与最终结果同属一个 owner 就全部保留到 emission |
| 最终 plan、Allocation、sealed IR | 全部 emitter、direct-link fallback 和多 region 借用结束 |
| label、实际 host offset、fault/relocation metadata | 完成发射后，必要元数据随 code object 进入发布 |
| workspace/cache 容量 | 按其实际 owner 保留或回收，与逻辑数据清空分开计账 |

FEX 在同步 JIT 返回后才 disown IR buffer，见 [Core.cpp](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/Core/Core.cpp#L806)。clear/reset/disown 不等于立即物理释放，也不能用其单次同步借用方式缩短 SVM 多 region prepared 的复合寿命。

逐项登记临时 inputs、候选、索引和证明中转的最后消费者，分别选择结束借用、清空逻辑内容、复用容量或显式转移所有权。保留在最终结果中的字段必须有下游消费者；不得仅因“以后可能用于验证”保留整套工作集。缓存容量可以复用，已退出作用域的 IR/分析指针和失败 attempt 的决定不能随之继续有效。

全单元发码准入只收敛静态编译结果的重复证明。代码发布时面对的 guest code、映射、并发重复编译和失效状态仍需晚期核对。FEX 自身也在 IR 生成后重查 lookup cache，并在 CompileBlock 中处理 invalidation 和后续发布，见 [Core.cpp](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/Core/Core.cpp#L785)。

## 4. 已实施的目录和接口

目录迁移已落地。以当前 [CMakeLists.txt](../source/runtime/backend/arm64/compile/CMakeLists.txt)为准，包含 52 个 C++ 源/头文件：12 个公开合同头、3 个必要的私有头和 37 个实现文件；构建清单本身另计。以下为实际组织，逐文件职责、真实拆分及退役对象见[详细布局文档](arm64-compile-pipeline-layout-2026-10-28.md)。

```text
source/runtime/backend/arm64/compile/
  compiler.h / compiler.cpp / pipeline.cpp
  plan.h / plan.cpp / plan_build.cpp / plan_identity.cpp
  target.h / target.cpp
  detail/compiler.h
  select/
    scalar.h / scalar.cpp
    vector.h / vector.cpp
    memory.h / memory.cpp / memory_updates.cpp
    flags.h / flags.cpp / consumers.cpp / integer_flags.cpp / predicates.cpp
    control.h / control.cpp / edges.cpp / edge_flags.cpp
  registers/
    constraints.h / constraints.cpp / fixed_constraints.cpp
    gpr_constraints.cpp / fpr_constraints.cpp
    state.h / state.cpp / versions.cpp / widths.cpp / residences.cpp
    publications.cpp / publication_copies.cpp
    publication_values.cpp / publication_windows.cpp
    resources.h / resources.cpp / scratch.cpp / spill_transport.cpp
    detail/describe.h / detail/write_successors.h
  verify/
    verify.h / verify.cpp / widths.cpp / fixed.cpp / fpr.cpp / audit.cpp
```

这些文件数不代表 pass 数、遍历次数或提速。真实依赖仍由 [pipeline.cpp](../source/runtime/backend/arm64/compile/pipeline.cpp)编排：共同描述分配约束，区分 Block/Function 分配，全部 FPR admission 成功后才开始 GPR edits，最终验证后封口。大算法按职责保留实现拆分，没有将所有逻辑拼入单个大文件。

公开合同按 compiler、plan、target、五类 selector、三类 register 职责和统一 verify 入口收敛。原 actions/admission/allocation_preparation、各域 inputs/preparation/actions/certificates 等旧公开路径已退役，不保留转发头或兼容别名。`PlanBuilder` 是原受限写口的直接改名；私有 `CompilerSession` 是原 `TargetPreparation` 的直接改名，负责同一编译会话的依赖顺序，不经新的 owner 或转发层。

实际归属同时改变了实现位置：零扩展的跨节点选择在 `select/scalar.cpp`，独立宽度复证在 `verify/widths.cpp`；W-write 事实在 `registers/state`，输入 builder 和 FPR/vector 证明工作集退出公开头；guest-state publication 在 `registers/publication*`，memory update/address/value 窗口在 `select/memory_updates.cpp`。普通局部编码、实际 lease、pending spill 与 host offset 继续由发码阶段处理。

`UnitCertificate` 的 CFG、入口及 provenance 深合同，以及 FPR 诊断查询的真实用途继续保留；生产 FPR admission 直接认证唯一最终结果，不再经诊断 instruction 表回灌。本轮目录、行为、代码生成和生命周期已按第 6 节完成验收；后续修改仍须执行相应检查。

## 5. 迁移次序和每步删除对象

本节保留原迁移顺序与删除验收标准，作为实施核对清单；提及旧对象不表示它仍存在于当前源码。已实施的目录与接口见第 4 节，逐轮质量结果见[实施进度](arm64-compile-pipeline-consolidation-progress-2026-10-28.md)。

### 先明确边界和删除清单

- 按 3.3 判定每类决定的归属，明确哪些普通操作无需目标记录。
- 按 3.4 登记 post-RA 物理编辑及失败类别；按 3.6 列出合同建立点、失效 writer 和最终消费者。
- 为每类中间数据登记 owner、首次建立、最后消费、跨 retry 是否复用、何时结束借用以及退役对象；将算法工作集与最终产物明确分开。
- 每个迁移单元列明将退役的旧表、搬运、排序和回扫，以及仍有用途的视图；不先按目录批量搬文件。

### 第一步：零扩展端到端试点

- 明确跨指令省略、物化义务、理由和认证状态的唯一来源；不为普通零扩展编码逐个建立 plan row。
- checker 直接认证私有结果，退役 width proof-ID 中转 vector 和对应搬运循环。
- `EmitZeroExtend32To64` 消费统一的跨指令结论，删除该决定的多表重选；局部写 W/复制/扩展编码依据 IR 与 mapping 完成。
- 同步迁移 `TickIR`、forwarded-width-spill 等实际消费者；不让旧 residence/helper 成为第二个最终决定来源，动态 pending/preservation 判断仍保留。
- allocator 计算和其它消费者仍需要的 width-chain/owner 状态继续保留；不能将它们一起误删。
- 验证必须 `Mov W,W`、可以省略、不同寄存器、spill、pinned home，以及 `SVM_ZEXT_NO_ELIDE` 的既有诊断行为；同时核对 transport 与物化时序。

### 第二步：memory 记录和 witness 收敛

- 让同一构建者持有 transfers/addresses，退役同类型复制和重复封口。
- 统一写回 witness，退役 `MemoryActions::Write → PhysicalGPRWrite` 的中间转换。
- 以需要 override 的 access 关联 EA/update/publication，随后退役 emitter 的重复静态地址 map；普通 access 不复制全部操作字段。
- 明确构建期 range/index 的退出点；保留“EA 已实际省略”的必要进度事实及 exact-consumer 检查，不复制静态 recipe。
- 统一静态 spill/reload/preserve 来源，保留 pending 消费进度和实际 scratch/fault 状态。
- 审查 emitter 中剩余的静态对齐分析；在最终 coverage 后计算跨节点结果，保留晚期 backedge 激活、unknown 回退、实际检查与 fault 路径，不为普通 access 建立全量 plan。
- 保持 matcher 优先级和首候选拒绝后回退规则。

### 第三步：coverage、领域提交和状态收敛

- selector 逐个接入同一 coverage 管理者，退役局部 covered 到最终 proof 的同义投影。
- 保持各阶段可见的覆盖集合与原 selector 优先级一致；一个 owner 不等于提前开放全部覆盖结果。
- 直接构造最终记录或移动完整 buffer，逐步删除纯复制 Store 方法。
- 只为影响分配的选择保留必要候选；纯物理合并直接产生最终稀疏记录，局部编码不加入统一 plan。
- 集中 reset/abort，明确 rejected/retry/success，保留 FPR admission 的事务性。
- 列出 selector 的依赖顺序，再决定哪些遍历能融合；不预先承诺整块只扫描一次。

### 第四步：prepared 入口及目录整理

- 正常 emitter 入口直接借用已有 PreparedUnit，减少裸 view 的拆装与重复准入。
- 在 IR/CFG/入口失效合同完整前保留深检查；证明所有相关修改点受控后，再收敛 certificate 镜像。
- 维持 source IR 封口和多 region 借用寿命；静态准入收敛不删除 publication/SMC/retirement 的晚期有效性检查。
- 最后迁移源文件，删除旧公开接口、旧类型、旧表和旧路径，更新包含关系及构建列表。

每一步都列明实际删除的表、转换和遍历。避免长期双轨：一个跨指令决定迁移完成后，其旧最终记录和旧 emitter 重分析应同时退役；合法局部编码分支继续保留。只移动文件、只新增统一入口而保留全部旧链路，不算完成。

## 6. 验收标准

### 6.1 维护性验收

- 每个跨阶段决定能指出唯一 owner、生产者、消费者、验证点和失效条件；普通操作不因统一 plan 而新增同义记录。
- emitter 不再做跨节点匹配、用途/观察窗口分析、allocation 修正或覆盖决策；允许基于确定参数做局部编码。
- 没有为同一结果保留临时 owning vector、最终 vector 和另一份 proof-ID 中转表。
- 临时工作集均有最后消费者和退出点；最终产物保留的字段有实际消费方，缓存容量复用不延长借用或 attempt 的逻辑有效期。
- spill/reload/preserve 的静态来源唯一，动态 lease 和实际代码偏移仍由 emission/runtime 管理。
- Block/Function 算法及全单元边界清晰，所有权和借用不会因减少层数而失效。
- source IR 在 RA 后仍封口，没有随整改引入另一套全量目标 IR、机器计划和证明镜像。
- 修改一种指令形式时，主要修改该领域的选择、合同和发射实现，不再沿四层增加同义字段。

### 6.2 行为与代码生成质量

行为验收以当前代码为参照，保持原指令选择优先级和失败行为。先覆盖零扩展高位、fixed-home/版本失效、FPR retry、scratch/spill、memory writeback、fault 恢复与 Function 入口等相关合同，再执行整合后的既定质量检查。重构中不混入新的融合规则、预算变化或指令选型扩张。

基线须包含整改开始时的完整工作树及既有 WIP，记录源码、构建配置、feature 和产物身份，不能仅以 HEAD 代表实际代码。前后比较使用相同配置；有源码变更时，不能直接把旧源码训练的 PGO profile 当作新的质量基线。此次结构整改可先使用一致的普通 Release 构建。

| 迁移领域 | 必须核对的行为 |
| --- | --- |
| 零扩展 | 高位确实清零、必要 `Mov W,W`、普通/诊断模式、pinned 与 spill 输入、`TickIR` 和 forwarded-spill 的物化/消费顺序 |
| memory | EA/access/update/publication 角色一致、非单调/稀疏及 foreign ID 拒绝、重复记录和首候选规则、写回 witness、pending spill、故障点与恢复状态 |
| coverage/状态 | 原 selector 优先级和阶段可见性、正交动作共存、abort 清理、失败 retry 不泄露旧 action/proof，成功 attempt 才可发码 |
| FPR/资源 | 所有 block 的 admission 先于 GPR edits、失败后原物理重试路径、scratch 预算与 lease、helper clobber 和 preserve |
| PreparedUnit | Block/Function 分别准入、单块 Function fast path、多 region 生命周期、direct-link fallback、可变输入拒绝及独立的晚期发布检查 |

既有完整质量检查继续执行，针对本次变化的合同用例作为补充；不能以新增少量单测替代原有检查。对基线已有失败逐条核对用例及原因，要求无新增失败、崩溃或静默跳过，不能仅比较失败总数。

检查清单须与实际 CMake 目标及测试用例清单核对，不能把历史脚本的调用数量当作完整覆盖。本次额外纳入 `shuffle-plan`、`region-plan`、FPR admission、普通零扩展，以及 copy/width-fact 中没有领域标签的具名用例；每个筛选记录实际执行的用例名与数量。独立 lifetime 组覆盖现有 15 个模式和两种 IR 设置，其结果单独记录。新增测试中的缺陷也须保留失败记录、修正后重跑，不能据生产子集通过就跳过失败筛选。

代码生成需要单独检查：固定 guest、配置和 feature，对照实际 guest PC/顺序、Block/Function 单元数量、IR fingerprint，以及既有 FP 代码缓冲区样本；Function 静默回退到 Block 不能算通过。稳定代码段保持字节及指令序列；涉及 host helper 绝对地址、relocation 或布局的差异须明确定位、解释，不能笼统忽略整个 host buffer，也不能据 fingerprint 相同就声称机器码完全一致。重点排查新增 move、spill/reload、物化和丢失的融合。

Android 验证使用用户连接的红魔手机，通过 adb 在独立结果目录运行；保留基线产物与原始输出。PMU 可用于后续性能归因，不能替代语义、实际生成代码及失败路径验证。

最终物理合同不能仅凭前置候选通过就放行；已经在正确阶段建立且依赖不再改变的证明，可以按 3.6 复用，不要求每层重新构建。同一证明的失效 writer 未被完整约束前，仍保留实际核对。独立 audit 不读取生产结果充当独立 oracle。现有测试若依赖准备接口，应迁移到明确的正常入口或可变输入测试入口，不绕过准入合同。

### 6.3 成本观察与完成判定

后续恢复测量时，按 Block/Function 分别观察决策匹配、复制和排序次数、证明复用/失效、post-RA 编辑及 retry 分类、目标 payload 大小。记录模型收敛的实际效果，不把少几张表或少几个 emitter 分支本身当作提速证据。

compile 成本变化需要在恢复性能工作后独立测量。文件数、pass 名称、表数量的减少只能证明结构变化，不能单独证明提速；生成代码、运行成本和编译成本的已有验收口径继续分别适用。

单项完成须同时满足：新来源已被全部实际消费者使用，旧最终表示/搬运/重分析已删除，拒绝和重试行为保持，相关合同及整体质量检查通过。整项完成还包含目录收敛、旧公开路径和构建引用退役；零扩展试点或一处借用简化通过，不代表整个方案完成。

## 7. 与现有方案的关系

- [编译成本优化方案](compile-cost-optimization-plan-2026-10-27.md) 保留已有研究、测量、停止条件及历史结果；本方案不恢复该历史性能目标。
- [中间流程重建方案](middle-pipeline-rebuild-plan-2026-10-21.md) 的共同 IR、Block/Function 区分及消费者合同继续适用。
- FEX 对照固定为 1.1 所列版本，借鉴其行为和结果归属；不要求抹平双方的分配范围、已有优化或验证合同差异。
- 本文定义 ARM64 后端的数据所有权、最终决策、维护性整改顺序及验收边界。文档修订本身不构成实施、测试或性能证据；生产变更及完成状态须由相应代码和验证记录证明。
