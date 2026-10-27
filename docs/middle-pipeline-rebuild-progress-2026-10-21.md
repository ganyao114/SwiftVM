# 中间流水线重构执行记录

目标与完成标准沿用 [重构方案](middle-pipeline-rebuild-plan-2026-10-21.md) 的完整 R0–R7 范围。本记录跟踪实际实现与验证，不以首批清理替代完整重构。起点为 `2804a51b`；测试按改动批次集中执行，不向仓库加入临时 probe 或仅服务单次观察的测试脚本。

## 结构重构验收总览（public-audit-v2）

本节保存结构重构的 public-audit-v2 签收检查点。后续 compile 优化和 guard-r6
代码量对齐的最新结果见 [编译与代码质量记录](compile-codegen-performance-2026-10-26.md)，
最终源码摘要为 `778c3f9c…`、cache 版本为 51；下述历史测量保留各自原始身份。

截至2026-10-26 public-audit-v2（481路径源摘要 `6049ce55…`、cache42），核心目录/接口、旧分配与持久plan退役、Block/Function独立边界、单owner发射回退、Params/terminal封口及公共独立audit均已交付。R2E未交付且无实验残留，不作为新的缺项。 本轮R0–R7核心重构与文末列明的有限验收已完成，最终人工记录为 `r7-public-audit-v2-final-stage-verdict.json`。

当前153项原生资格全部达到预期终态；main仍保留同一11失败分项/66失败断言，不能写全套零失败。RV85闭合37/254有限范围。真实audit接线与API测试分开报告，详见末尾public-audit-v2记录。

当前V2四份普通堆流已独立签收：public requested完整流、同事件编译存储与Inst backing、publication/runtime及退出保留分别核算，未分配归属U为0；固定帧、CodeCache逻辑映射及参数化递归边界另列，不冒充全进程RSS或无界栈最大值。当前V2六组216主点、36冷服务点和6个服务寿命已全部完成，独立核验与人工阶段签收通过，12组每轮指令斜率均在1%门内；不宣称整体提速或追平FEX。旧V5 Core36＋6无效数据单独保留。限定生产内部栈关系、实际caller和命名库边界已交付，系统盘/设备清理及最终存储封口完成。以下分批记录和旧阶段表保留各自当时状态，其中“待接续”不自动成为当前新增义务。

## 分批实现记录（保留当时状态）

- 旧 backend/reg_alloc 三个文件已退役。公共 AllocationWriter 显式借用物理结果和 OptimizedUnit 的唯一 SemanticFacts，ARM64 AllocationPreparation 另借用独立动作并处理目标保留集/证明；生产准备对象在返回结果前销毁。测试 AllocationFixture 独立持有 facts/物理结果/actions，没有生产便利 owning 构造或兼容别名。公共 writer、控制器和局部事务均不 include ARM64 头文件，预制约束仍待接续。
- 原 ARM64 BlockAnalysisIndex、PseudoFlagsPlan 与 UnitFacts 入口已迁到 compile/analysis/{block_index,pseudo_flags,semantic_facts}。显式 PrepareUses 复用基础 consumer 图；原 AllocationFacts 类已删除；普通分配与 target 共享一个 SemanticFacts，别名消费者改为显式 PrepareResolvedUses，const 查询不再构图。最终持有边界已接入 OptimizedUnit；必需摘要和剩余基础扫描继续接续。
- `runtime/compile/pipeline.{h,cpp}` 的 OptimizeBlock/OptimizeFunction 消费 owning CompileUnit，返回封口的 owning OptimizedUnit；Function 的 guest 范围、RPO/编号、提升和整组 region 合并已从 ARM64 compiler 移入公共调度。删除旧 `PassPipeline`、`PassEntry`、动态注册表、线程缓存及 raw-pointer void Optimize 接口。保留原有优化开关、依赖顺序和 PerfStats 接入。
- `runtime/compile/optimized_unit.{h,cpp}` 独立稳定存储拥有封口 IR、不可变 FeatureSet 快照和唯一 SemanticFacts，移动不使已借用索引失效。最后一个准备 writer 退出后才能 TakeIR；交接将 IR/pool/实际父对象/anchors 转给 prepared 结果，同时释放全部 facts 缓存。
- `runtime/compile/unit.{h,cpp}` 从构造时持有 HIR pool、实际 Function/独立 Block 与 loop-hoist anchors，覆盖尚未封口的准备阶段；删除默认构造和带输入参数的 SealIR。准备 writer 与封口后只读查询分开，移动单元转移存储与阶段职责；消费者结束后解除封口和解绑元数据，最后释放存储。Block 继续只保存 anchors 视图。
- HIRFunction 在构建和发布后持续持有持久 Function 的 intrusive 引用，Module 持有独立引用；Function 继续直接删除其 Block，不给这些 Block 混入 intrusive refcount。CompileUnit 另持有实际 canonical 父对象；准备阶段的成员所有权由 compilation 计数约束，整组合并先校验再转移，并同步更新各单元引用。
- `backend/arm64/compile/compiler.{h,cpp}` 统一生产 Block、最终 HIRBlock、Function 和多 region 的准备；必要契约通过后返回 owning Arm64PreparedUnit，稳定存储拥有 IR、anchors、分配及 target 结果，对外只读。runtime 的重复准备结构与流程已删除；函数 code-object emitter 只接受 prepared 结果，旧公开 FunctionRegionEmission 裸配对已退役。物理结果已转为公共 AllocationView，target 动作由独立 UnitActions 提供；可变准备 builder、最终公共事实和完整轻量 actions 模型继续接续。
- 新建 `runtime/compile/analysis/facts.{h,cpp}`，合并重复的 GPR/width-chain use-end 收集与 BitCast 来源解析，补齐 loop-hoist 隐式根。块内 use-end 使用局部稠密表，跨块来源使用排序稀疏表，避免为每块保存全函数大小的数组。FPR 的专用语义仍独立。
- OptimizedUnit 持有唯一 `SemanticFacts`，准备 writer、普通分配、scratch/FPR 重试和 target 借用同一份 use-end、flags、CFG、spill positions 与 consumer 图；AllocationState 只拥有策略/workspace。每份 facts 只允许一个活动分配 writer，封口后禁止换成其他单元，Function 显式拒绝非成员 Block。没有 flags pseudo 时不分配全单元 demand 数组；spill-use 仍仅在淘汰确实遇到压力时建立。Function 提升在最终编号和物理分配前完成，不再用 Apply/Revert 切换 IR 或双份分配结果。
- Function CFG 活跃性事实迁入 `compile/analysis/cfg.cpp`，区间消费者合并到 `compile/allocate/intervals.cpp`；旧 register_alloc_cfg_liveness.cpp 和过渡 function_liveness.cpp 均退役。固定点结果在一轮准备中至多建立一次。单块 Function 先检查合成入口、入口来源、本块定义归属与先定义后使用；合格时省去 CFG 固定点，自回边与隐式根仍保留。资格检查不额外建立索引。
- `compile/analysis/liveness.{h,cpp}` 的 ValueLiveness 统一持有 Block/Function 的基础 use-end、host-register 写入与 Function 局部性，跨 scratch/FPR 重试复用；首个分配消费者将活跃性与 flags 合并收集。仅 flags 的调用不构建 ValueLiveness，consumer/CFG/spill 仍按需；分配中的三套前扫描和重复 scalar-tie FPR 写表已删除。
- 新增公共 AllocationInputs 与 ARM64 输入准备入口：静态 clobber、默认预算、GPR tie、调用偏好和发表来源在扫描前生成，由请求 const 持有并跨物理重试复用。208 处分配调用与 14 处手工扫描构造已显式接入，没有保留 FeatureSet-only 重载；旧 gpr_ties.cpp、const NextInst 惰性表、逐 attempt 的 call-ABI 向量及 FeatureSet 副本已退役。fixed/preservation、FPR 和逐指令 scratch 规则仍待接续。
- 分配公共入口改为 `compile::AllocateBlock/AllocateFunction`；生产与测试调用方直接接入，删除旧 RegisterAllocPass 及 HIRBuilder 批量包装。重试与 reserve 阶梯统一编译在 `compile/allocate/allocation.cpp`，测试支持复用同一实现；扫描器、区间、基础事实/CFG、spill 和 scratch 分列源文件，不再通过巨型头文件或 `.inc` 导出实现。
- ARM64 固定输入、调用偏好、GPR ties/width/home/copy、FPR 合并和 spill reload 规则迁入 `backend/arm64/compile` 家族文件；公共分配源文件不包含 ARM64/VIXL 头，也不直接判断 x86 固定池模式，改用后端给出的预算描述。必要 fixed-GPR 验证仍无条件执行。旧合并家族的 DropCensus 计数随实现退役，`ir/opts` 目录已全部删除。物理字段与只读查询已移入公共 Allocation，公共 AllocationWriter 借用 Allocation，ARM64 AllocationPreparation 另外借用 UnitActions；旧 RegAlloc 已删除，生产 compiler 在返回结果前销毁准备对象；后端家族继续通过内部 LinearScan 接缝协作；预制约束的输入/结果分离尚未完成；Bind/replay 已由 R4 退役。
- `compile/allocate/transaction.{h,cpp}` 的 AllocationEdit 替代 width-chain 每块复制整单元 mapping/read/write/owner 表的备份。真实修改才记录，块内编号使用按需局部索引，块外 owner 使用稀疏条目；拒绝和异常退出恢复 mapping、alias、owner、dirty masks、被修改的非 preserved 位置及发表/consumer 条目，并使派生范围缓存失效。带 capture actions 的组件明确拒绝 recolor，保留其 backing 与普通发表；R4 的 narrow-preserved 修复直接约束 consumer 视图，不重写 capture actions。
- target 声明借用 UnitFacts 的同一份只读 use 图，不再深拷贝到 GPRPlanBuilder/PendingGPRBlock；BlockAnalysisIndex 禁止复制。两种 observer/full-write 策略的 write-successor 投影移入 `backend/arm64/compile/write_successors.h`，分别由调用方持有；发表结果仍复制轻量 flags 存储。ARM64 compile 源文件注册移入自己的 CMakeLists.txt。删除旧 Bind/early/narrow/clause 临时普查与 SVM_BINDCENSUS 入口；生产 Bind 已在 R4 删除，fixed 必需检查与显式 FPR 正确性重试保留。
- RegAlloc 不再保存 emitter 的当前指令游标；TickIR 直接用编号读取 live masks。JitContext 的所有构造、成员和 GetAllocation 查询均使用公共 AllocationView，ARM64 动作通过独立 const UnitActions 提供；spill/helper/host-byte 等发射计数由每个 encoder 按需保存。metrics 改读相应 encoder，生产准备结果已接续封口 IR；准备 writer 已退出结果存储；预制输入、通用编辑接口、公共 facts 和 emitter 内旧准备扫描仍待后续阶段。
- 优化 pass 的 `CompileWorkspaceScope` 复用线程工作区和 Function 删除队列，嵌套编译使用独立存储，大容量在作用域退出时回收。分配控制器的请求状态拥有 workspace 并借用同一 facts；reserve/FPR 重试复用其存储，初次分配线程退出后仍可继续 admission，目标准备完成后释放请求状态。Function owner 表和队列只在首次确实收集 DCE 时建立；公共 OptimizeBlock/OptimizeFunction/OptimizeFunctions 已显式传入同一 CompileWorkspace&；独立 pass 的十五个便利重载与十四处隐式 CompileWorkspaceScope 获取已退役；测试只通过资源夹具调用真实 RewriteContext，家族 helper 边界与完整私有成本继续收口。
- `Inst::ArgAt(int) const` 返回 `const Arg&`，Arg 禁止浅拷贝；Block/HIRBlock 的指令列表统一返回 const 元素，指令链节点私有，Params 查询返回不可变 View，const Lambda 返回 const Value/Imm 引用。Value::SetType 已删除，Value 定义、anchor/capture 和 HIRUse 的指令指针已改为 const；HIR value 表、定义身份和 use 链也已关闭可写查询。指令写入口校验真实 Block 归属与封口状态，HIR CFG writer 通过 EditBlock 校验归属与阶段。AddressNode 范围/种类/链节点及持久成员查询只读；CompileUnit 与 ARM64 prepared 结果已持有 HIR 存储并约束成员所有权，最终事实视图及其他调用方仍需接续。
- builder 迁入 `compile/builder/ir_builder.{h,cpp}`，guest-state 字节转发、helper/分支失效与 mapped load/store 单列为 `guest_state.cpp`；所有内部调用方已改用 `compile::IrBuilder`，删除原 `frontend/ir_assembler.{h,cpp}`，不留旧类名别名或转发文件。未迁移调用暂用新类中的保守 fresh-node 构建入口。
- 新建 `compile/rules/value_rules.{h,cpp}`，提供三态 FlagsIntent、BuilderCapabilities、共享整数立即数/扩展/截取规则及同类型 cast 别名建议。ARM64 编码谓词位于 `backend/arm64/compile/target.{h,cpp}`，逻辑立即数查询使用现有 VIXL 编码器；删除 builder、常量 pass 和 narrow flags 的重复谓词。
- 原常量 pass 迁为 `compile/passes/values.cpp` 的 Block/Function 显式入口，删除未使用的 HIRBuilder 包装及 `ConstFoldingPass` 旧文件。生产 ARM64 调度显式传目标能力；RISC-V 使用目标无关规则。扩展/截取的常量求值由 builder 和残余 pass 共用，旧 builder/常量 pass 的 census 探针随实现退役。
- 新 builder 提供结果类型和 FlagsIntent 在先的构建接口；显式调用可返回已有同类型 Value，保持原定义的类型。x86 访存、NarrowTo/Extend、MOVZX/MOVSX、LEA 和立即数转换，以及 ARM64 常量 helper 已接入。所有原 append/SetType 调用已迁到创建时显式定型，helper 保留原 traits。仍依赖参数推断结果类型的旧入口保守进入残余 pass；Unknown flags 不据此转成无输出。
- 新建 `compile/passes/passes.h` 与 `dce.cpp` 的 RewriteContext，共享操作数改写、HIR use 注销/注册、删除通知和 flags 变化通知。独立 Block 用合法逆序回收，不建 Function owner 表；Function 用一次播种的跨块队列回收定义，先查成员资格再访问可能重复入队的指针。DCE 保留 fault、提升锚点和仍需使用的 capture 根。
- flags、整数宽度与 DCE 迁入 `compile/passes/{flags,widths,dce}.cpp`；value pass 归为 `values.cpp`，统一使用 `passes.h`。删除旧 flags/width/DCE/carry-region 类、源文件及未调用的 HIRBuilder 批量包装。flags 逆扫在判断 writer 前先删除已死纯 reader，删除立即更新 uses；value/width 改写后排空 DCE，并将 flags 变化送回同一上下文。
- Function flags 的纯 transfer、成员/terminal 需求和固定点已归 analysis/flag_flow，flags.cpp 保留改写调度。摘要与清理的两张局部 label 哈希已合为一个惰性有序快照缓冲，carry 屏障通过 epoch 丢弃旧快照；独立 Block 不建立 Function 表。IR 相关删除或掩码变化后仍处理必要整组 RPO 摘要，按受影响块进一步缩小范围及完整私有成本尚未完成。
- builder 原地折叠，以及 uniform 转发、host 映射、FPR 最终映射与 sink carrier 改写均同步 HIR uses，避免旧链继续指向已删除节点。CSE 同时覆盖 Lambda 与 Params，保留每条边的类型视图及重复参数数量。
- state 家族迁入 `compile/passes/{state,state_dse,state_map,state_sink}.cpp`，分别负责前向转发、反向死存储删除、静态 FPR 映射收尾和 capture/fault sink。生产调度向它们传同一 RewriteContext；删除旧 uniform elimination/store-sink 四个源文件和类、忽略参数的 Function 包装以及 uniform census。DCE 每次收集前刷新隐式根，已释放的根会重新触发回收。
- `GuestStateLayout`、静态绑定与范围描述迁入 `compile/analysis/guest_state.h`，RegisterMask 移入不依赖分配准备器的 `backend/register_mask.h`；全部内部调用方已改名，AddressSpace 不再通过 pass 头导出布局。
- `compile/builder/instruction.cpp` 接入 x86/AArch64 的单条 guest 指令作用域；显式 typed 候选在终结 HIR 前提交，使用原始 append 位置，异常退出清空待处理候选。作用域校验已声明的 flags 输出；Unknown flags 继续保守，不因当前没有 pseudo uses 就视为无输出；旧 SetType 已退役。x86 原生 ADD/ADC/SUB/SBB、carry 归一化及窄位宽对齐算术已显式传类型和输出掩码；其余原 SetType 家族已显式传结果类型，旧类型推断入口与完整 flags 契约仍待接续。
- 循环提升迁入 `compile/passes/loop_hoist.{h,cpp}`。候选先证明 unit 内 guest state 无重叠写入或未知重入，再按局部 use-end、跨块 CFG 活跃性、固定读取、clobber 与 scratch 预算检查新增活跃范围；只有真实跨块 GPR 才建立临时 CFG 需求。每块继续限制一个 GPR 锚点，宽比较常量保留既有形状。目标预算由 ARM64 `compile/target.cpp` 提供，scratch-only 和默认 reserve 查询与 RA 共用。
- 生产 Function 只维护一份公共物理分配结果；提升在最终 IdByRPO 前完成。删除旧 LoopInvariantHoistRecipe、original_order、Apply/Revert、baseline/hoisted 双分配、hoist-census 及两个旧源文件。准备段已迁入 compiler 并接续封口与持有权；最终分配输入/结果解耦、公共 facts 与完整轻量 actions 仍待接续。
- 删除 Dataflow、旧 VReg 分配器、ConstAddressCandidate、旧 CFG/ReId pass 及对应构建项。原 CFG 测试转向生产 `ComputeRPO/IdByRPO`，保留 irreducible CFG 的终止性和块覆盖检查。
- 删除未使用的 elision 查询、`MatchesFrozenIR` 包装及重复验证分支；生产调用使用物理实例与 allocation generation/attempt 身份检查，必要宽度证明继续生成。
- 扩展现有 hoist 行为测试：recipe 销毁和 CompileUnit 移动后，通过 Block、单块 Function 快速路径和通用 Function 分配检查锚点到块尾的活跃性，并检查元数据解绑。没有新增测试脚本或测试源文件。

- `analysis/uses.h` 为基础摘要、CFG、块内资格与 Function 区间共享 terminal/loop-hoist anchor 的隐式使用遍历。CFG 延长已补齐 anchors；嵌套 Lambda/Params 原本由 GetValues 覆盖，现直接 ForEachValue，避免临时列表。单元 scratch demand 与块内 pseudo demand 在一次 flags 扫描中收集，保持不同的跨块语义；已有 consumer 图时直接从图派生 use-end。

- `backend/arm64/alloc` 的最后八个文件和空目录已退役。必要 fixed/width/spill/zero-store 证明归 certificates 与 width_contracts，纯 effects 归 effects，可选类型/位置/支配审计归 audit；删除 owned width graph 和 nullable fallback。compiler 的 Finalize 已集中物理编辑封口、整单元语义绑定、必要证明、可选审计与最终动作冻结；普通 attempt 的基础 fixed/scratch 检查和独立 FPR retry 继续保留。

- consumer 行 map、terminal-condition 哈希表和 MemorySelections 的每块哈希索引已删除。memory builder 与 consumer 顺序借用同一块内行工作区；memory Finish 后只交接连续决策和必要的 load-publication 名单，不再保留可变索引或 publication 链接。公共优化、分配 spill 游标和 target 已在后续批次统一借用外层工作区；其余候选和完整容量度量继续接续。

- 公共 Optimize、AllocationWriter、scratch/FPR 重试和 ARM64 target 已统一借用外层 CompileWorkspace；AllocationState 内嵌存储和 RewriteContext 私有完整 scope 已移除。Function DCE 用 RewriteQueueScope 管理短期队列借用，Block 不申请队列，RV 原有准备入口也显式接入。结果交接后工作区可重用；容量度量、其余候选和异步交接验证继续推进。

- 驻留准备已直接持有最终四字节动作，旧 GPRResidenceFacts 的三份哈希集合和一份 result-home map 退役；块身份绑定的结果显式移动到 UnitActions。准备对象及两种每块结果各缩小 112 bytes，launcher text 增长 408 bytes，私有 vector 插入/容量与完整成本仍待验收。

- pinned select/view 候选现直接借用共享使用表，transfer 的两份指针列表已合为一个按需工作区队列，MEM 发表直接返回最终选择。三个私有 final 会话去掉无用虚析构基类并显式禁止复制/移动；最终 .text 相邻减少 152 bytes。300 层 alias 与大编号/冲突/晚期拒绝恢复已验证，新增借用管理和完整成本继续验收。

- CopyCandidate 已退役 aliases/transferred_uses 两份 owning 列表，recolor 仅传递规范语义 root ID，definitions/component 指针副本和整组 proof-writes 复制退役。修正 Function 外块 MEM producer 的迭代器归属检查顺序；双块与混合 alias 回归通过。候选由 96 降至 40 bytes，.text 相邻增加 120 bytes，必要 sites/公共事务暂存及完整成本仍待验收。

- 五类发表选择已共享在 dead-write/version 访问中播种的候选序列，既有索引缓冲分阶段复用；零 pseudo 的 scratch 查询用精确 use 计数跳过。主要 owner 大小保持，.text 相邻增加 3216 bytes。最新红魔三方 PMU 三十六主点/六辅助点有效，指令斜率持平，周期/耗时斜率中位数约增百分之一；私有容量、完整成本和正式 FEX 门继续开放。

- 公共 recolor 的物理 root 与旧 home 已合为单份八字节记录，独立 OldHome vector 退役；仅实际释放 GPR/reload 时重建活跃事件。单次调用快照与整次事务 first-write 日志保持不同职责；连续 recolor、重复 REF、MEM-only fixed class 切换的提交/回滚已验证。RecolorGPR 自身帧 416→384 bytes，launcher .text 增加 3368 bytes，完整成本继续开放。

- 共享整数位查询已扩到 carry/borrow 加减、Neg、AndNot 与同宽 32/64 位立即数移位；worklist 与查询共用 transfer 范围，私有 TransfersBits 退役。decode/pass 实际折叠、独立输入域与跨算术/移位反馈已验证；Block 两处真实代码变化和 .text/递归帧增长已记录。没有新增逐值缓存，完整单元语义宽度及总体成本继续开放。

- helper 状态/fault/reentry/uniform traits 已由公共五字节记录解析，IR effects 与目标契约共享；Lambda 十一字节、目标契约二十字节保持。前向 Declared 与循环 NoReentry 策略明确区分并共用范围校验，CallLocation 在 decode/残余转发中的失效遗漏已修复。三组新 IR 用例通过，.text 增 56 bytes、自身帧增长与完整成本仍开放。

- 二十三种固定默认结果已归 opcode 元数据，构造/空结果改写先定型，typed builder 默认入口共用规则；通用 Value 推导继续迁移。冷报告保留越界拒绝，有效构造固定帧退役；.text 增长、局部 spill 与全部成本另列。

- 最新 v8 在 fixed-result-v4 源码上构建十四个 RV64 目标，十九项新用户态通过；十三项用户态/五项 guest 内核按完整相同 ELF 和原环境复用 v6，独立强 fence TSO 另新执行，不称一次新三十七项 CTest。

- 结果类型来源已归既有 ir.inc 的五种声明规则，Block/HIR/typed builder 在节点创建前定型。八处参数 setter 推导、缺省 VOID sentinel、Inst::Create 和无类型 helper 包装退役；真实无结果调用保留 effects/使用而不创建 HIRValue。构造/参数改写及 Block/Function 的解释器/JIT 结果已集中验证，完整类型/SSA/宽度与动态成本仍开放。

- 基础结果/类型视图与数据可用性现归共享 IR 原语，封口检查与 SSA/use-end 合流，验证用 Values 快照退役。红魔十二项原生/十组负载新执行与 RV64 修正验证闭合；launcher .text 增加 4880 bytes，完整类型/宽度/CFG 和动态成本继续开放。

- fault sink 的整块 original 指针副本与全指令位置表已收为直接链遍历和配方稀疏位置，状态 DSE/sink 四处只读 Values 快照退役。原始顺序、提前观察及缺失边界已验证；.text 增加 1776 bytes，pending/flush 私有缓冲和完整成本继续接续。

- fault sink 的 selected/victim/remaining 三份临时容器已退役；pending 三态仍占 32 bytes，coverage 复用，pending/published 按实际需求准备。独立字节 oracle 与限定 C++ 请求计量通过，.text 相邻减 1072 bytes；外层帧增 32 bytes、完整动态成本及异常退出继续接续。

- fault sink 脱链节点的持有状态已覆盖扩容、重连与部分压缩；删除通知失败后只交回仍持有节点供销毁。快/慢模式各完成 41 次真实 C++ 分配失败注入，归属/uses/清理无新请求通过；PendingStore 仍为 32 bytes，两处自身帧保持，.text 相邻增 760 bytes。其余改写故障矩阵、capture 载荷与完整成本继续接续。

- capture 的 latest/published 两份逐字节表、dirty/slot 中转已退役，改为完整 V128 候选与唯一请求范围的有效字节证明；各记录 24 bytes。修复部分 flush 将旧窄写留到新宽写之后的真实字节错误，并补齐非对齐 uniform 地址的精确 scratch 预算。快/慢 IR 各新增三例通过，限定 span 分配比较通过；launcher .text 增 6224 bytes，其中三份新槽排序符号共 6088 bytes，排序与完整成本继续清理。

- CaptureSlot 及重复 Value/offset 随后已退役，候选仅借用单个 8-byte store 指针，在已有访问中维护 offset 顺序，同槽替换/交叠区间失效，边界无需排序。三份 6088-byte 排序特化消失，launcher .text 相邻减 4616 bytes；capture 自身帧增 32 bytes、完整私有/动态成本继续计量。本批集中六项 native/四组普通 Block/Function 负载，不追认旧主套件或 fixed-class/call 结果为新 ELF 验证。

- coverage 随后收为全部 pending 的包围区间，配方 carrier 索引复用 PublishedCapture 尾部填充；位置表只由实际 carrier 的非精确边界查询触发，并使用稳定非 StoreUniform 顺序。已有发表证明短路 consumer 扫描，terminal 跳过不再读取的短期事实。延迟建表/失败与偏移 64000 的分配比较通过；.text 相邻增 216 bytes，sink/flush 自身帧增 64/16 bytes，完整成本仍开放。

## 阶段状态（历史汇总，截至 block-emission-owner-v5）

本表按此前批次累积，保留早期尚未接续的描述及V5时点的R7结果，不代表public-audit-v2当前待办。最新结论以上方总览与末尾当前批记录为准，原表不整体改写。

| 阶段 | 当时记录的状态与剩余工作 |
| --- | --- |
| R0 | 首批死代码、Android 工具链、失败集合和探索性 PMU 已落实；固定 FEX 的配方/补丁归 tools/fex-android，设备/主机入口归 tools/redmagic，独立目标退出与同组原始有效时间已验证。原始基线 Block 的 low32 失败和 profiler 零退出保留。r1-flags-demand-storage-v2 与最新 r5-publication-roster-v1 各自以相邻冻结 SVM 为参照，完成三十六个三方主点、六个冷服务辅助点及单列服务聚合，均通过采样协议；最新指令斜率近乎持平，周期/耗时斜率中位数约增百分之一，不能宣称稳定提速。这些是各自相邻比较，不是起点累计对照。完整 guest 能力、参照适用性、其他代表负载及完整矩阵未验收 |
| R1 | CompileUnit/OptimizedUnit/facts/AllocationView/Writer 的归属已分开；活跃性、flags、presence、声明 home 共享准备，直接/resolved 边按需。两种 flags 结果现直接写各自 owner，临时归档集合与二次移交退役，SemanticFacts 保持 624 bytes、基础入口自身帧 640→400。外层 workspace 独占借用、九类容量过程与批量初始化已计量；捕获宽度/末次位置按需稀疏缓存，FunctionCFG 结构共享。完整宽度、其他 CFG/候选/暂存和总体成本仍待完成，交接及并发验收继续接续 ；有界位查询现支持算术及八类窄/混合宽度移位，变量只查询必要低计数位并共享改写传播范围，真实消费者已验证，仍非完整单元/CFG 宽度摘要 |
| R2 | typed builder/残余规则、value/width worklist、DCE/flags 通知、跨块回收与 typed guest 提交已接入，SetType/uniform restamp 退役。helper 状态解析和写范围策略共享，CallLocation 双路径失效修复已验证。一百九十一项结果来源现归唯一 schema，所有构造入口先取类型；八处 setter 推导、缺省 VOID sentinel、Inst::Create 与无类型 helper 包装已退役。两种无结果 helper 与六种工厂/解释器/JIT 已验证，低层显式契约和冷越界拒绝保留；完整类型/宽度/CFG、有序候选、guest-state/fault/观察契约与成本仍开放；基础类型枚举/规范定义/数据角色与隐式根检查现共享，封口 SSA 和 liveness 同次枚举，临时 Values 快照退役；完整参数种类/可选后缀/内部槽现已归唯一 schema，CheckArguments 与 SSA/use-end 同次访问；两套 HIR use 遍历和 Call 特判已退役。构造前 C++ 种类/物理容量及动态先检查已接入；元数据改为 constinit span，IRMeta 48 bytes、表 9312 bytes，旧 18396-byte 初始化消失。具名签名结果现已贯通共享创建入口，动态只校验一次；相对上一批 108 创建主体 code 减少 17752、launcher .text 减少 26520 bytes，自身 SP 保持；单个动态包装及完整分区成本分别记录；十四项标量基础整数域/selector/计数宽度现已归共享参数检查，解释器入口合并旧宽度预扫，ARM64 零宽位域与宽结果 ByteSwap16 已补齐；新 helper 自身 16-byte 帧及 launcher text +3088 bytes 等成本单列；完整位宽/值域/转换、CFG/Phi 与成本仍待接续；fault sink 的整块指针副本、全指令位置表和四处状态只读快照已退役，配方稀疏位置/缺失边界与直接链遍历已验证，selected/victim/remaining 也已退役为三态和稳定压缩，coverage/pending 容量复用及限定 C++ 请求回归已验证；脱链持有/回收的限定分配失败矩阵已通过；capture/published 字节表随后退役为完整槽与唯一请求字节掩码，部分 flush 的较早重叠依赖已闭合并经修复前失败/修复后 oracle 验证；CaptureSlot 与排序特化随后退役为有序的借用 store 指针，既有配方/字节及分配失败回归通过；coverage 随后已收为全部 pending 包围区间，carrier 索引复用尾部填充，实际 carrier 非精确查询才建稳定非 store 位置表，发表证明短路 consumer 遍历；延迟建表/失败及大 span/高 offset 回归通过。候选查询/移动、稀疏哈希与必要 observer、其余改写故障矩阵及完整成本仍开放；同宽整数谓词已贯通 ARM64 三处 W/X 选择，S64 高三十二位丢失已由修复前失败/修复后 Block 与合格单块 Function 2000 断言验证；本地 ScalarInteger 退役，相邻 .text −144 bytes；完整逻辑读取/raw 运输与转换继续接续；四类整数转换现共享输入视图/32 位上限与专用位宽查询，本地 IntegerWidth 和四份解释器执行逻辑退役；Block/合格单块 Function、优化开关、全部整数源/结果类型及原始来源模式已验证。launcher text 相邻 −528 bytes，ZeroExtend64 自身帧 +16 bytes 等成本单列，其他窄算术/移位/比较及完整成本继续接续 ；八类移位现统一数值契约、低计数位分析与三后端共享主体，ARM64 四个旧立即数主体从巨型文件删除；55296 程序/2605056 次 oracle 检查通过（含 halt/translation），launcher 相邻 .text −5384 bytes，自身栈增长单列，完整 CFG/成本继续接续；整数谓词/条件读取与本批 FlagFlow 拆分、两张局部 label 哈希退役已完成范围内验证，完整语义/CFG 与成本仍开放；十五个隐式 pass 重载及十四处工作区获取随后已退役，测试 99 处显式上下文迁移，内联/空入口及完整成本继续接续 |
| R2E | 可选实验尚未开展，不阻塞核心迁移 |
| R2H | 分配前压力决策、一次性提升、owning anchors、双分配/顺序备份/Revert 退役已落地，compiler 已接续其存储和封口；代表性负载的指令与 spill 质量门通过。固定源码 FEX 的整体对照仍属于 R0，最终公共 facts/分配结果仍由 R1/R3 接续 |
| R3 | 公共分配消费显式 GPR/FPR/fixed/preservation/spill/scratch 输入；普通 attempt 基础 fixed/scratch 与目标编辑后的 final fixed 证明分别保留，compiler 统一独立 FPR retry 和冻结。声明 home 与 FPR 候选扫描已收敛；宽度复证的一百六十份私有 vector 已收为两份候选 home 缓冲，fixed/width 的终结哈希现归公共策略化短期查询，只读 Check 不再创建三组证书输出。必要/可选入口自身固定帧现为 288/240 bytes，检查对象增至 160 bytes、fixed Block 自身帧增十六字节及新查询构造成本另计。完整私有容量、复用、其他接缝与总体成本仍开放；精确 uniform load/store 按实际地址复用 VIXL 编码谓词计入地址临时量，真实非对齐 Block/Function JIT 已验证，预算函数自身帧增十六字节，完整 scratch/fault 矩阵仍开放 |
| R4 | writer/width-chain 局部 recolor、narrow-preserved 后缀、Bind/replay/声明备份/undo 退役已实现；Block、合格单块 Function、通用 Function 与跨线程 FPR retry 验证通过，SVM 本地指令与 spill 质量门通过。公共 AllocationView 已接入；预制输入与通用编辑接口、其余调用方和固定源码 FEX 对照仍属于其他未完成阶段 ；公共 recolor 的并行 roots/OldHome 列表已收为八字节调用快照，无释放路径跳过 mask 事件重建；first-write 日志保持独立，连续编辑及回滚已验证，排序特化和完整成本仍开放 |
| R5/R6 | 旧 alloc/plan 容器已退役，准备归 compiler、静态结果归 UnitActions，emitter 只接收 prepared 结果并保留真实编码状态。consumer/memory/flags/原子/边准备、共享工作区、块摘要和普通 terminal 目标复用已接入；共享 host-call 目标冻结与 flags1 的 SSE/CallLambda 持久化验收已接入，PF 后缀两组 vector 已退役；终结位置静态选择也已归准备层，恢复标签按实际分支寄存器创建，延迟消费保留 spill backing；其余 pinned/条件融合/共享 CFG、其他调用方和总体收益未验收；region 双结果 helper 重绑定及 Function Div128 的磁盘/AOT 恢复已接续，Call source 退役夹具的越界和过期返回预期已修正，完整并发矩阵仍开放；条件准备现共用唯一 roster/位置查询，未进入生产的 ScalarFPRLiveness 类/文件/构建项已退役。窄整数 flags 已直接生成最终动作，三种中转类型及 map/set/memo 退役，短期证明按需借用行工作区；首次初始化、递归深度与总体成本仍开放 ；五类发表选择已共享块局部候选，索引阶段和精确 pseudo 查询边界已验证；新增私有候选容量、.text 增长及约百分之一的周期/耗时斜率变化继续计入待验收成本；GuestStateMap 五文件随后退役，物理 register_* 准备移出 jit，const 惰性建表与 65 个 vector 头退役；不同视图、公共逻辑与完整动态成本继续接续 |
| R7 | 累计退役118路径；旧 Assembler/RegAlloc、ir/opts、ARM64 alloc、Bind replay、持久子plan及emitter补分析已清理。最新block-emission-owner-v5（df9d69f7…、cache42）交付两种Block单一可重建发射owner与完整flags观察回归，Function分配独立；R2E未交付且无残留。当前152项不同原生资格为149精确复用＋3项新执行；main同一11失败分项/66失败断言，成功断言增308未归因，79条固定非计时记录一致。RA_1BLK=0、BOUND_DECODE=0及state-sink-failure fast0/1已覆盖。RV84为19项新执行＋13用户态/5真实guest内核精确复用，强TSO220321新执行通过；37项为全仓254项中的有限范围。剩余任务限定为合格堆采样及五组共存容量/保留、完整调用链边界、当前六负载216点PMU与人工质量/FEX阶段判定；不以这些局部通过代签全目标 |

## 构建与验证记录

使用当前连接的红魔 NX729J（`bf1359ff`）。设备已有 FEX Android launcher、FEXServer 和 `coremark-O2-x86_64`。主机基线源码通过 `git archive 2804a51b` 导出到 `/private/tmp/swiftvm-middle-baseline-2804a51b`，与候选工作树独立构建，避免后续编辑污染基线。

本机可用 NDK 的兼容性检查结果：NDK r27 的 libc++ 缺少当前源码使用的 `std::atomic_ref`；NDK r29-beta3/Clang 21 支持该接口，但仓库 fmt 9.1.1 的 consteval 处理不兼容。基线与候选统一使用 `-DFMT_CONSTEVAL=constexpr` 构建，不修改两边的 fmt 源码。Boost 使用本机 1.86.0 头文件，两边一致。

```sh
cmake -S <基线源码或候选工作树> -B <对应构建目录> -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/Users/swift/Library/Android/sdk/ndk/29.0.13846066/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBoost_NO_BOOST_CMAKE=ON -DBoost_INCLUDE_DIR=/opt/homebrew/include \
  -DCMAKE_CXX_FLAGS=-DFMT_CONSTEVAL=constexpr
cmake --build <对应构建目录> --target swift_test swift_ir_test svm_translator_linux -j 3
```

基线构建目录为 `/private/tmp/swiftvm-middle-baseline-build`，候选为 `/private/tmp/swiftvm-middle-candidate-build`。设备部署目录为 `/data/local/tmp/svm-middle-r1`，主机原始结果目录为 `/private/tmp/swiftvm-middle-results`。

| 验证 | 状态与证据 |
| --- | --- |
| Android 基线构建 | `swift_test` 与 `svm_translator_linux` 已构建完成；构建日志在基线目录 `build.log` |
| Android 首批候选构建 | `swift_test` 与 `svm_translator_linux` 已构建完成；构建日志在候选目录 `build.log` |
| 实际全套件基线 | 607 个注册用例；696 条含 section 的 JUnit 记录，12 条失败记录归并为 9 个失败用例，67 个失败断言，0 个 error |
| 首批候选与基线回归 | 两边各完整执行一次。候选同为 696 条 JUnit 记录、12 条失败记录、67 个失败断言；归一化两边源码根路径后，失败名称、消息与断言正文逐项一致，无新增/消失记录。新增覆盖的 hoist 生命周期与生产 CFG 行为均通过 |
| PMU 三方对照 | 完成一组 2000/12000 次三方采样，六次 CRC 全部正确。统一 root、CPU 4、PMU 进程树范围；本轮独立 FEXServer 预启动，成本不在计数范围内，采样结束已停止该服务。详情见下表 |

基线产物 SHA-256：

- `swift_test`：`713fe1d7a5861abb063a64ec56145fa2f667366ee4fa0b23b9568416d4f4ca6e`。
- `svm_translator_linux`：`a38c2db8d75c75b66bdc926fb1fb21ae16828050789a53d23c7baf6b53652116`。

首批候选产物 SHA-256（对应下方三方 PMU 采样）：

- `swift_test`：`2d6b9ca25ecb1f4444b92b17df2009cf16a927b533d66a5fe5478758d653ee7a`。
- `svm_translator_linux`：`c5e0bec823e013b27657eb07004aea2ac5fbca4f9a631ede1b3a353b1756cd3e`。

当前 FEXServer 的版本输出为 `FEX-Emu ()`，不能从该输出证明其源码提交。后续实测应记录设备产物哈希，将其与方案中用于源码研究的 FEX 提交区分；重新构建固定源码参照仍属于 R0 的未完成项。

## Facts 与工作区批次验证

本批集中运行候选完整套件一次，以及基线和候选的独立 `swift_ir_test` 各一次；没有逐次修改后重复运行套件。Android 三个目标均构建完成，最终构建日志为候选目录中的 `build-r1-qualified-fix.log`；随后构建 dry-run 显示没有待构建项。

| 验证 | 结果 |
| --- | --- |
| 基线 IR 契约 | 11 个用例、264 个断言，全部通过 |
| 候选 IR 契约 | 14 个用例、293 个断言，全部通过；新增跨块 Lambda/Params、terminal/anchor use-end、缓存视图稳定性、单块输入资格、工作区嵌套与清零覆盖；既有高 fanout 测试补充 300 个 flags pseudo |
| 候选完整套件 | 696 条 JUnit 记录、12 条失败记录、67 个失败断言、0 个 error；归一化源码根路径后，失败名称、类型、消息及断言正文与固定基线逐项一致 |
| 分配行为与成本边界 | 单块快速路径与通用路径的完整 mapping 对照通过；Block/单块 Function/通用 Function 的提升锚点活跃性通过；既有压力测试分别确认未启用淘汰时不建 spill-use、需要淘汰时仅建立一份 |
| 静态清理 | `git diff --check` 通过；旧 CFG-use-end、两份 GPR/width use-end、旧 flags 构建与 attempt 自持游标名称的源码引用已清零；未新增 probe 或测试脚本 |

本批产物 SHA-256：

- `swift_test`：`b8425e8a6354ccafc3f6c159913f91f8daea49844e6c756c56810e822ea67282`。
- `swift_ir_test`：`e7bb47843fb91d9b0152b44f6f739455c9734c9178acc06a47a79d25815c1a6b`。
- `svm_translator_linux`：`e1295a336afe5694626f4884bff9af5a19efd6769648f3cc1af8c3812851b5a7`。

原始结果为主机结果目录中的 `facts-suite.xml`、`facts-ir-suite.xml`、`baseline-ir-suite.xml` 及对应设备运行日志。本批不重复三方 PMU；下方首批数据对应先前产物，不能视为本批产物的性能结论。普通路径的完整初始化字节、容量增长统计以及最终 facts/封口所有权验收仍待后续阶段落实。

## Builder 与共享规则批次验证

Android 的 `swift_test`、`swift_ir_test`、`svm_translator_linux` 构建完成，最终日志为 `build-r2-builder-typed.log`，共 316 个构建步骤；随后 dry-run 无待构建项。本批 IR 契约套件与完整套件各在红魔执行一次。

| 验证 | 结果 |
| --- | --- |
| IR 契约 | 17 个用例、347 个断言全部通过；覆盖确定结果类型前后两种折叠路径、符号扩展及截取结果、原定义类型保持、重复访存的效果、flags 未知/声明与目标能力缺失时的保守行为 |
| 完整套件 | 696 条 JUnit 记录、12 条失败记录、67 个失败断言、0 个 error；归一化源码根路径后，所有失败记录与固定基线逐项一致，无新增或消失记录 |
| RISC-V 调用方 | 此次修改的 22 个 RISC-V 测试源文件使用同一 NDK 的 C++ 编译参数通过 `-fsyntax-only`；这仅验证 API 迁移与编译，不代表 RISC-V 机器码执行验证 |
| 清理 | 旧 Assembler 类/头文件和 ConstFoldingPass 的源码引用清零；ARM64 add/sub、logical 编码谓词仅保留新 target 实现；`git diff --check` 通过；没有新增 probe 或测试脚本 |

本批产物 SHA-256：

- `swift_test`：`51d0e7f53578157d1650515e8a62ba1f885b5617c33a7ba40187d599c8fbf5e3`。
- `swift_ir_test`：`11093e524eb37f64ba45bd2cedee8dad5592a0b318b1120b114763ed1b567dd3`。
- `svm_translator_linux`：`d0814aa6e6a160c9816ace90c4cbe42a20b6c2cdc9bad652391364d3dc3893e2`。

同一 guest 的两点 PMU 以「基线 2000、候选 2000、候选 12000、基线 12000」顺序各采一次，四次 CRC 均正确。采集条件沿用 root、CPU 4、`taskset 10`、`simpleperf --cpu 4` 及空 `SVM_JIT_CACHE`。每点前后记录 governor、当前/最大频率和三个命名温度传感器；governor 均为 `walt`，当前/最大频率均为 1171200 kHz，`cpuss-0` 为 30.1–31.6°C、`cpu-1-0` 为 29.7–32.0°C、`cpu-1-6` 为 27.7–30.1°C。这些名称按设备 sysfs 原样记录，不推定为 CPU 4 的独占温度。

| 指标 | 固定基线 | 当前累计候选 |
| --- | ---: | ---: |
| instructions，2000 次 | 5,835,187,446 | 5,423,415,418 |
| instructions，12000 次 | 17,162,752,305 | 16,468,911,153 |
| 指令斜率，kins/iter | 1132.7565 | 1104.5496 |
| 周期斜率，kcycles/iter | 342.9224 | 336.5958 |
| 启动/编译相关指令截距，B | 3.569674 | 3.214316 |
| 稳态 IPC | 3.3032 | 3.2815 |

本组指令斜率变化约 `-2.49%`，周期斜率约 `-1.84%`。比较包含起点以来全部已实现改动，不单独归因于 typed builder，也不以一组采样替代后续正式质量门。此批没有重新运行 FEX；下方三方组仍是独立历史参照，不能将两组混作同条件的三方测量。PMU 固定工作量同样不用于报告正式 CoreMark 分数。

原始结果在主机结果目录的 `builder-suite.xml`、`builder-ir-suite.xml`、`r2-{baseline,builder}-{2000,12000}.log` 和 `r2-pmu-summary.json`，后者包含每点前后的原始环境记录。

## 共享改写与删除通知批次验证

Android 三个目标均构建完成，生产改动日志为 `build-r2-rewrite-owners.log`。完整套件执行一次后，仅修改 carry 正例的测试输入，并通过 `build-r2-rewrite-fixture-final.log` 中的两个步骤重新编译该测试源文件和链接测试程序；生产源码未在此补验中改变。最终 dry-run 无待构建项。

| 验证 | 结果 |
| --- | --- |
| IR 契约 | 22 个用例、379 个断言全部通过；新增跨 Lambda/重复 Params 的 HIR use 与类型视图检查、构建期/state 转发的 use 同步、DCE 隐式根与 fault 保持、Block/Function 死 flags reader 回收、跳过块内写操作的 label 路径检查 |
| 完整套件首轮 | 696 条 JUnit 记录、13 条失败记录、68 个失败断言、0 个 error；除 carry 正例外，其余失败记录与固定基线逐项一致 |
| carry 正例补验 | 旧正例没有消费 LocalCondSet/SBB 的结果，新 DCE 因而可以正确回收 reader。改为把结果写入 uniform 后，`*carry*` 的 20 个用例、906 个断言全部通过；测试仍检查真正有观察者的 carry 依赖，不放宽期望 |
| 联合回归结论 | 用补验结果替换完整首轮中的相应用例后，剩余失败名称、类型、消息和正文与固定基线完全一致；没有为这次测试输入修正再跑第二遍完整套件。两个测试产物哈希和联合比较结果保存在 `r2c-verification-summary.json` |
| RISC-V 调用方 | 改动涉及的 22 个测试源文件再次通过同一 NDK 的 `-fsyntax-only` 检查；执行验证仍待对应后续阶段 |
| 静态清理 | 旧 flags/width/DCE/carry-region 的类调用、include 和构建引用清零；旧 `ir/opts` 当前剩 17 个文件；`git diff --check` 通过，没有新增 probe 或测试脚本 |

本批最终产物 SHA-256：

- `swift_test`：`6449a5d216c80b59d1c9760024828619ead3f01cd6843358c2e3ab8f028004ab`；完整首轮产物为 `31069dd7ede2221061fc8d8aed8a51cf94afbb86ac950fd802768f7e466053af`，二者仅测试输入修正的构建不同。
- `swift_ir_test`：`9b584683b10705791204149da5c28857624e2e983c35218968aee0efcd2faec9`。
- `svm_translator_linux`：`94ce18787b7a3fc7441a88475ca8338aacb3840cc5d1e1cda7eb3178c834ba7c`。

一次两点 PMU 直接比较上一批 builder 候选与本批 rewrite 候选，顺序为 builder 2000、rewrite 2000、rewrite 12000、builder 12000，四次 CRC 均正确。保持同一 guest、root、CPU 4、`taskset 10`、`simpleperf --cpu 4` 与空 `SVM_JIT_CACHE`。governor 均为 `walt`，最大频率为 1171200 kHz；大多数点前后当前频率也是 1171200 kHz，但 rewrite 12000 点结束后的读数为 940800 kHz，不把周期差异视为固定频率下的结论。三个命名温度传感器在 28.5–32.0°C 范围内，原始逐点记录随结果保存。

| 指标 | 上一批 builder | 本批 rewrite |
| --- | ---: | ---: |
| instructions，2000 次 | 5,422,210,316 | 5,375,798,808 |
| instructions，12000 次 | 16,466,275,694 | 15,864,088,106 |
| 指令斜率，kins/iter | 1104.4065 | 1048.8289 |
| 周期斜率，kcycles/iter | 343.3916 | 328.5138 |
| 启动/编译相关指令截距，B | 3.213397 | 3.278141 |

本组相对上一批的指令斜率变化约 `-5.03%`，周期斜率约 `-4.33%`，启动/编译相关指令截距约 `+2.01%`。收益和新增准备成本均继续跟踪；不能从单组数据归因某个子项，也不将它替代后续重复采样和代表性负载质量门。此批未重跑 FEX，不将其与早前三方组混为同条件测量。

原始 JUnit/运行日志以 `rewrite-` 开头，PMU 日志为 `r2c-{builder,rewrite}-{2000,12000}.log`，摘要为 `r2c-pmu-summary.json`，均保存在前述主机结果目录。

## State、guest 提交与工作区批次验证

Android 的 `swift_test`、`swift_ir_test` 和 `svm_translator_linux` 已构建完成，最终日志为 `build-r2-state-layout-fixed.log`，dry-run 无待构建项。移除旧 state 头的间接依赖后，为 ARM64 发射器和 region 发射器补上了直接使用的 HIR 定义头。本批只运行 IR 契约和完整套件各一次，未新增测试文件、临时 probe 或测试脚本。

| 验证 | 结果 |
| --- | --- |
| IR 契约 | 24 个用例、397 个断言全部通过；覆盖提交前后候选状态、原始立即数距离、已声明/未知 flags、HIR 终结前提交、异常退出、工作区删除队列的嵌套隔离与清零，以及同一改写上下文释放隐式根后的回收 |
| 完整套件 | 696 条含 section 的 JUnit 记录、12 条失败记录、67 个失败断言、0 个 error；归一化源码根目录后，失败名称、类型、消息与断言正文逐项等于固定基线，无新增或消失记录 |
| RISC-V 调用方 | 22 个已迁移测试源文件均通过同一 NDK 的 `-fsyntax-only`；未进行 RISC-V 机器码执行验证 |
| 静态清理 | 旧 UniformInfo、UniformEliminationPass 和 UniformStoreSinkPass 的定义/调用及四个旧构建项已清零；state 改写不直接调用原始 DestroyInst/EraseInst；`git diff --check` 通过 |

产物 SHA-256：

- `swift_test`：`11bd6ccdf37d3c64a4473082511048211f28f9a5a6391e79007c91bdf79fc408`。
- `swift_ir_test`：`ea6419e5bdb4f4b4cf8eb8da8af8280e4e2b67076ce25c2b6c1bd11746ffc74d`。
- `svm_translator_linux`：`cfd78ef4bc85727f1a9d4dcd54070469db453997f6bd2a4271e37daf8bfeb6a6`。

一组两点 PMU 比较上一批 rewrite 候选（SVM 哈希 `94ce18787b7a3fc7441a88475ca8338aacb3840cc5d1e1cda7eb3178c834ba7c`）与本批 state 候选。顺序为 rewrite 2000、state 2000、state 12000、rewrite 12000，沿用相同 guest、root、CPU 4、`taskset 10`、`simpleperf stat --cpu 4` 与空 `SVM_JIT_CACHE`，四次 CRC 均正确。

| 指标 | 上一批 rewrite | 本批 state |
| --- | ---: | ---: |
| instructions，2000 次 | 5,375,941,318 | 5,383,031,503 |
| instructions，12000 次 | 15,863,723,450 | 15,871,883,678 |
| 指令斜率，千条/次 | 1048.7782 | 1048.8852 |
| 周期斜率，千周期/次 | 325.9073 | 333.0217 |
| 启动/编译相关指令截距，十亿条 | 3.278385 | 3.285261 |

相对上一批，本组指令斜率约 `+0.0102%`、周期斜率约 `+2.18%`、启动/编译相关指令截距约 `+0.21%`。本批主要改变组织和构建期处理，单组数据不能证明稳定收益或退化，也不替代正式质量门。governor 均为 `walt`，最大频率为 1171200 kHz；rewrite 2000 点结束时当前频率为 940800 kHz，其余点前后读数为 1171200 kHz，不能当作固定频率比较。三个命名温度传感器读数为 28.5–32.0°C；采样前电池报告 AC 供电、100%、状态码 5、电池温度 25.9°C，均按设备原始含义记录。本组未重跑 FEX，不与早期三方数据混用；短工作量结果只用于 CRC 和 PMU 比较，不报告正式 CoreMark 分数。

结果为 `state-{ir-suite,suite}.xml`、`state-run.log`、`r2-state-verification-summary.json`、`r2-state-riscv-syntax.log`、`r2-state-{rewrite,state}-{2000,12000}.log` 与 `r2-state-pmu-summary.json`，均保存在前述主机结果目录；逐点条件另存于 `r2-state-pmu-conditions.json`。

## R2H：分配前提升与双分配退役

本批将提升放在残余 pass 之后、最终 IdByRPO 之前。FunctionRegionAllocation 只持有一个 RegAlloc，普通 scratch/FPR 正确性重试继续在同一 IR 上运行。旧 LoopInvariantHoistRecipe 及两个旧文件已删除，生产路径不再保存原始指令顺序或执行提升后的第二次分配/Revert；没有留下新旧决策比较分支。准备编排暂仍由 backend/runtime.cpp 调用，向 compiler 的完整迁移属于后续 R1/R3/R6，不能把本批记为全部中间流程完成。

压力决策按稳定节点与块内序号建立临时 use-end。只有存在真实跨块 GPR 才计算 CFG live-in/live-out，包含透过循环到后继块的使用；固定 host 读取在跨越写入/clobber 时计入动态压力。ARM64 提供实际 pool、固定 clobber、helper ABI 与 scratch 预算，预算基础查询与分配器共用。每个候选延长至整环前后，新增占用不能扩大任一点的压力超额；候选拒绝不改 IR。GPR 的只读证明覆盖整个 unit 的重叠 uniform 写、helper 写入范围、未知重入与观察屏障，不依赖旧 pass 只查 StoreUniform 的假设。

Android 三个目标均已构建完成，最终构建日志为 `build-r2h-cfg-final.log`，后续 dry-run 无待构建项。红魔 IR 契约与完整套件各执行一次；性能补验期间没有改动候选源码或重跑完整套件。

| 验证 | 结果 |
| --- | --- |
| IR 契约 | 24 个用例、397 个断言全部通过 |
| 完整套件 | 699 条含 section 的 JUnit 记录，新增 3 个提升行为用例；仍为 12 条失败记录、67 个失败断言、0 个 error。源码根归一化后，失败名称、类型、消息与正文与固定基线逐项一致；此次失败位置也未变化 |
| 提升行为 | 低压力接受、受限 pool/饱和窗口拒绝且 IR 顺序与 metadata 不变；未知 helper、重入、重叠/不相交 helper effects；多块 Function 透传值的 CFG 压力；pass 返回后、CompileUnit 移动后及 Block/两种 Function RA 入口的 anchors 存活性，均通过 |
| 静态清理 | LoopInvariantHoistRecipe、生产 original_order、双 RegAlloc、提升 Apply/Revert 与 hoist-census 已删除；`git diff --check` 通过。未新增测试文件、测试脚本或临时 probe |

产物 SHA-256：

- `swift_test`：`af5ad16c7c9d8d476076662666321e797d818c58a83774ce25f88d9795168e47`。
- `swift_ir_test`：`737297adecb8b84fad54dc9895d2026d59106bfb96cae523f60a03ae676cd3a0`。
- `svm_translator_linux`：`bd2ef738bac609566b85fc06067e2b962eb6633c43c0b2c5b7df232abf5b5448`。

代码质量对照以上一批 state 候选（SVM 哈希 `cfd78ef4bc85727f1a9d4dcd54070469db453997f6bd2a4271e37daf8bfeb6a6`）为参照，均禁用磁盘 JIT cache。CoreMark 和现有 benchmark 的整数、FP/vector、访存、分支、调用五类负载各交错采样三轮；奇数轮按旧低、新低、新高、旧高执行，偶数轮反转两版顺序。CoreMark 固定 2000/12000 次，其他负载先用 scale 1/2，共 72 点。root、CPU 4、`taskset 10`、`simpleperf stat --cpu 4 -e cpu-cycles,instructions` 保持一致，逐点记录频率和温度。

benchmark guest 哈希为 `df60aa3f57d40ed3eb0b44d124f2cb8c9d15899f90021b6d20e94be179240a6d`。使用已有 `bench_suite_native.c`，以同一 NDK、`-O2 -ffp-contract=off` 编译 Android 原生 oracle，并在手机取得校验值；scale 1 的五项值与仓库 golden 一致。原生 oracle 哈希为 `e13a6fa23dd66df9caaed0de7376867e4d288ddace811278e12a27be7ad85961`。全部 PMU 点均验证退出码和对应参数的校验值，原生程序只提供正确性参照。

| 负载 | 指令斜率中位数变化 | 周期斜率中位数变化 |
| --- | ---: | ---: |
| CoreMark，2000/12000 | -0.0071% | -0.81% |
| 整数，scale 1/2 | +0.0693% | -1.28% |
| FP/vector，scale 1/2 | -0.0459% | +0.67% |
| 访存，scale 1/2 | +0.0295% | -0.08% |
| 分支，scale 1/2 | -0.0118% | -0.48% |
| 调用，scale 1/2 | +0.0015% | -2.08% |

短整数采样首轮出现 `+2.3649%` 的指令斜率离群值，原始数据完整保留。只对整数负载扩大到 scale 4/8，以相同候选和采集条件补充三轮交错采样，共 12 点，并先在手机取得这两个输入的原生校验值。补测指令斜率中位数约 `+0.0081%`，三轮范围为 `-0.5639%～+0.0406%`，周期斜率中位数约 `+0.27%`。这说明扩大工作量后未复现稳定退化，不能据此断言原始离群值的具体成因。以长工作量整数补验和其他五类三轮结果判断，稳态指令退化低于 1% 的本批 SVM 质量门通过。

静态 spill/代码质量使用已有 `SVM_PROF=2` 与 `SVM_RA_SHAPE_PROF=1` 另行观察，不混入 PMU 点：CoreMark 两版的 432 个发表单元及每单元 PC/IR 数量相同，spill defs 均为 333、load/store 均为 99/73、host bytes 均为 287224；五类 benchmark 合并运行的 22 个单元及 PC/IR 数量相同，spill defs 均为 162、load/store 均为 245/150、host bytes 均为 19908。相同单元集合上的 spill 总量没有增长，符合 2% 质量门。

另用相同的细粒度 profiling 开关观察准备工作，旧版本实际触发 6 次提升比较，collect_live 调用从 488 降至新版本的 482。两边 allocation scope 调用均为 443，spill 总量相同，但该重 profiling 组有一个发表单元 PC/IR 变体不同（`0x25fb00/40` 与 `0x25fbbc/25`），因此只将少 6 次收集记为与结构删除相符的观察，不作为独立的耗时或指令成本归因；前述质量门使用单元集合匹配的独立组。重 profiling 的计时也不与无 profiling 的 PMU 混用。

原始 72 点中 governor 均为 `walt`，最大频率为 1171200 kHz，当前频率读数覆盖 940800–1171200 kHz，三个命名温度传感器合计为 28.1–34.0°C；补验的逐点条件也完整保存。周期结果不视为固定频率下的收益；两点截距仍含启动和其他成本，不能替代后续编译成本归因。此次未重跑 FEX，固定源码 Android FEX 构建和整体三方正式验收仍属于 R0 的剩余项。

原始结果和摘要保存在 `/private/tmp/swiftvm-middle-results`：`hoist-{ir-suite,suite}.xml`、`r2h-verification-summary.json`、`r2h-shape-summary.json`、`r2h-pmu-{conditions,summary}.json`、`r2h-int-large-{conditions,summary}.json`、`r2h-preparation-summary.json` 和 `r2h-quality-gate.json`；84 份 PMU 原始日志及 profiling 日志一并保留。

同一目录另存源码 overlay 与 manifest，包含相对 `2804a51b` 的 136 个现存修改/新增源码文件和 27 个删除路径，覆盖未跟踪的新目录；不包含构建产物或测试脚本。`r2h-source-manifest.json` 的源码内容摘要为 `c8570ef6b836e71d9b9cba3b6ea099f777868c6fed6d5b8f6d6a08ae40964492`，`r2h-source-overlay.tar.gz` 的 SHA-256 为 `c78dd1a2f46b6f066f2165364c99c6ea5c5229d4469ef51431bafff6a0cab54b`。该 overlay 配合固定基线源码、此文工具链参数和依赖版本保留当前候选的源码对应关系；仓库没有新增归档或 probe。

### R3 公共分配核心与 ARM64 家族拆分

本批保留 Block、合格单块 Function 和通用 Function 的独立区间算法，不用单一 Function 扫描替代便宜的 Block 路径。`compile/allocate` 的 `internal.h` 只保留内部数据与声明，实现在 `allocation.cpp`、`linear_scan.cpp`、`intervals.cpp`、`function_liveness.cpp`、`spill.cpp` 和 `scratch.cpp`。ARM64 源文件按固定寄存器、GPR ties、width/home/copy、FPR 和 reload 家族分开；公共层消费预算，必要约束由后端核验。查询参数槽统一返回 `const Arg&`，写入通过 setter；这不代表 Value 定义和块列表等其他可变逃逸已经封口。

| 验证 | 结果 |
| --- | --- |
| Android 构建 | 同一 NDK/Release 参数下 swift_test、swift_ir_test 和 Linux guest launcher 均通过 |
| IR 契约 | 24 个用例、398 个断言全部通过，含可变 Inst 的 ArgAt 仍返回只读引用的编译期约束 |
| 完整套件 | 699 条含 section 的记录，仍为 12 条失败记录、67 个失败断言、0 个 error；按源码根和相同源码行块归一化，失败名称、类型、消息与正文与固定基线逐项相同，无新增失败 |
| 静态质量 | 与上一批 svm_hoist 对比，CoreMark 的 432 个单元和 benchmark all 的 22 个单元逐单元 PC/IR 数量相同；spill/load/store 分别为 333/99/73 与 162/245/150，host bytes 分别为 287224 和 19908，两版完全相同 |
| 清理 | RegisterAllocPass、旧分配 `.inc`、旧 coalescing/reload 文件与整个 ir/opts 目录退役；未加入临时 probe 或测试脚本 |

产物 SHA-256：swift_test 为 `94b66b0ae1b33e0f5e245fe6a78c562f7d2ab2525a26e85e5f99fcb0ecd7573d`，swift_ir_test 为 `58ca08faaccb18c04e01070c3f85732232e5d3eeff27a94f58361e76d6e38734`，svm_translator_linux 为 `2b252f040ee85abe8420d4676a18ddf5f5a5ac5d1a7a1665d584426cf1ef9997`。参照仍为上一批不可变的 svm_hoist，哈希见 R2H 记录。

本批集中采集三轮交错 PMU：CoreMark 2000/12000 次，以及现有五类 benchmark 合并负载的 scale 1/2，共 24 点。每轮交错两版与两种工作量，奇偶轮反转版本顺序；使用同一 guest、空磁盘 JIT cache、root、CPU 4、taskset 10 与 simpleperf 的 cpu-cycles/instructions，全部退出码和参数对应校验值正确。

| 负载 | 指令斜率变化中位数（三轮范围） | 周期斜率变化中位数（三轮范围） |
| --- | ---: | ---: |
| CoreMark | -0.1176%（-0.1339%～+0.0244%） | -0.69%（-0.80%～-0.47%） |
| 五类 benchmark 合并 | -0.0057%（-0.0317%～+0.0127%） | -0.28%（-6.13%～+3.08%） |

这些数据支持此次行为保持拆分未产生可见的稳态指令或 spill 退化，不将 benchmark 合并结果解释为每个家族的独立性能结论。governor 均为 walt，最大频率 1171200 kHz，前后当前频率为 1056000–1171200 kHz；cpuss-0/cpu-1-0 为 28.5–32.4°C，cpu-1-6 为 28.1–30.1°C。周期数据不作为固定频率下的收益结论。固定源码 Android FEX 的正式三方对照、完整阶段只读约束与 R3 最终结果模型仍未完成。

原始结果保存在 `/private/tmp/swiftvm-middle-results`：allocate-{ir-suite,suite}.xml、运行日志、r3-verification-summary.json、r3-shape-summary.json、r3-pmu-{conditions,summary}.json，以及 24 份 PMU 原始日志和两份静态 profile；设备结果保留在原 results 目录。

本批源码另存为同目录的 r3-source-overlay.tar.gz 和 r3-source-manifest.json，包含相对起点的 182 个现存修改/新增源文件和 38 个删除路径。源码摘要为 `8ac6a9b4d7125d07ac159ef81a060c9dff670b09d50a2a78bf422270e27eb811`，归档 SHA-256 为 `e7660185be0510b48c740056304537c3a24db8c985e0949e600375f9e7186d49`；与上方产物对应，后续编辑不覆盖此参照。

### R3 局部编辑与 target use 图复用

此批维持原有 width-chain 的候选、接受阈值与完整 owner/scratch 验证，改变备份与图的持有方式。AllocationEdit 不保存整个 target 声明世界，也不修改 IR、pool、编号、spill recipe 或 target actions；支持的非 preserved 位置变更同时记录相关发表和 consumer 关系。preserved-prefix 还关联 capture actions，当前明确拒绝这类编辑，其闭包与 R4 的驻留事务一起处理。

块内日志的编号表在第一次真实修改时建立，长度限定为当前块的编号窗口；窗口外值按需保存。既有分配用例补充提交、主动拒绝、异常退出与窗口外值，验证 4096 项物理表中修改 4 个值只保存 4 项，局部索引为 34 项（136 个初始化字节），无修改时两种索引均不建立；只修改窗口外值时，局部索引仍为零、稀疏条目为一。另覆盖 fixed 位置、发表条目、consumer 选中状态、额外 use 数及已建立的范围缓存恢复。这里的计数只覆盖该日志接缝，不替代 15.4 全部准备状态的成本验收。

UnitFacts 继续持有已建立的 use 图；GPR 声明和后续 target 家族借用只读索引，类型禁止复制，独立 write-successor 投影通过不同观察谓词生成。现有 64 种 write-search 组合同时验证两个投影互不覆盖。最终公共 facts 归属、显式按需准备与完整只读 IR 仍未完成。

| 验证 | 结果 |
| --- | --- |
| Android 构建 | 三个既有目标均通过，最终增量检查为 no work to do |
| IR 契约 | 24 个用例、398 个断言全部通过 |
| 完整套件 | 705 条含 section 的记录，12 条既有失败记录、67 个失败断言、0 个 error；失败名称、类型、消息及正文与前一批逐项相同，并沿用前一批与固定基线的等价对照 |
| 静态质量 | 配对 CoreMark 的 432 个单元逐单元 PC/IR/host bytes 相同，spill/load/store 均为 333/99/73、总 host bytes 为 287224；五类 benchmark 的 22 个单元逐项相同，162/245/150 与 19908 bytes 均不变 |
| 清理 | 整单元 GPRCoalesceState/Save/Restore、target use 图复制与旧 SVM_BINDCENSUS 诊断清零；未新增测试文件、测试脚本或临时 probe；git diff --check 通过 |

第一次候选 CoreMark profile 比先前参照少一个单元 `0x25fb00/40`，对应 188 host bytes，不能直接作为同单元集合上的完整比较。该原始组保留为 r3-compact-use-{core-shape.log,shape-summary.json}；只补充一组相邻的两版 CoreMark profile，得到上表逐单元匹配的 432 个单元。未据此断言首次差异的具体成因，也未重跑完整正确性套件来寻找相同 profile。

产物 SHA-256：swift_test 为 `bc169b7d675708123acbc9487a7597c49f033686aba6112910b39e69a3ca336b`，swift_ir_test 为 `3a0e83dd7aa73dd7cca883ae7c05fb76091aa29b77cc04cb012b2ab7d5b3ab74`，svm_translator_linux 为 `2178276c89bdc56b5bef9628a1203774f4874cf88fcf720422a5a1e5fd4ad867`。参照为本节之前的 svm_allocate（`2b252f…ef9997`）。

初版逐项哈希日志已通过正确性和匹配单元的静态质量检查，但三轮 CoreMark 观察未显示编译成本降低，指令截距中位数反而为 +0.5583%；该数据保留在 r3-edit-* 结果中。最终改用块内局部索引，并删除 target 图复制后，再集中采集三轮 CoreMark 2000/12000 次交错 PMU，共 12 点，全部退出码与 CRC 正确：

| 指标 | 候选相对参照的中位数变化（三轮范围） |
| --- | ---: |
| instructions 斜率 | -0.0028%（-0.0099%～+0.1215%） |
| instructions 截距 | -1.3244%（-1.7802%～-1.2940%） |
| cpu-cycles 斜率 | +0.59%（-0.68%～+0.68%） |
| cpu-cycles 截距 | -2.75%（-3.36%～-0.20%） |

root、CPU 4、taskset 10、空 JIT cache 与 simpleperf 事件保持相同。governor 均为 walt，最大频率均为 1171200 kHz，前后当前频率读数为 614400–1171200 kHz；cpuss-0 为 29.3–32.0°C、cpu-1-0 为 29.7–32.0°C、cpu-1-6 为 28.1–30.1°C。指令截距同时包含启动等其他成本，只报告整体观察，不能全归因于分配器或拆分为两项规则的收益；周期数据不视为固定频率结论。该批 SVM 局部正确性、指令与 spill 质量门通过，固定源码 Android FEX 的正式对照仍未完成，也不据此宣布完整 R3/R0–R7 完成。

原始数据继续放在 `/private/tmp/swiftvm-middle-results`：compact-use-{ir-suite,suite}.xml、r3-compact-use-verification-summary.json、r3-shared-paired-shape-summary.json、r3-shared-pmu-{conditions,summary}.json、r3-shared-quality-gate.json，以及对应运行/profile/12 点 PMU 日志。初版 r3-edit-* 的二进制哈希、源码 overlay 与结果同样保留。

最终源码 r3-compact-use-source-overlay.tar.gz/manifest.json 包含相对起点的 196 个现存修改/新增源码文件和 38 个删除路径，源码摘要为 `a3c2a96a39cda038126af67d1f98362383a99de5b6c1011ef99faf8f7acef624`，归档 SHA-256 为 `721ccdba9861bb7c82dcfb88895587f8e5e32ee3de635dfaabac5e5a514ed1ca`，对应上述最终产物。

### R3 分配结果的发射只读边界

删除 RegAlloc 的 current_ir、SetCurrent、GetDirtyGPR/FPR；它只提供带指令编号的 DirtyGPR/FPR 查询。JitContext 已有 cur_inst 和 scratch/live-mask 状态，不需要在冻结分配中再存一个游标。三个构造入口及 GetAllocation 统一接收/返回 const RegAlloc 引用，现有 coalescing 契约编译期检查阻止重新导出可变引用。

原来写入 RegAlloc.RAShape 的发射 spill、helper、host-byte 与 pair-fallback 计数迁到各 JitContext 按需建立的 accumulator，以分配阶段的统计为初值。不同 encoder 的发射事实分别保存，不借用分配结果作为可变状态；提交时机继续沿用代码 finalization 的既有协议，不将它解释为新的发布或回收协议。所有现有 metrics 改读实际 encoder。

| 验证 | 结果 |
| --- | --- |
| Android 构建 | 三个目标通过，最终增量检查无待编译项 |
| 完整套件 | 705 条含 section 的记录，12 条既有失败记录、67 个失败断言、0 个 error；失败名称、类型、消息与正文与上一批逐项相同 |
| profiling 接续 | 两版分别运行现有 FPR metrics，长度限定为 2/8/32：每版 621 个断言通过，9 组 bytes/spill/reload/store 相同；这些 FPR 样例的 spill 数均为零，不把它们当作非零 spill 覆盖 |
| 非零 spill 与代码质量 | 五类 benchmark 的 22 个单元逐单元 PC/IR/host bytes 相同，spill/load/store 仍为 162/245/150，host bytes 为 19908，全部校验值相同；覆盖移出分配结果后的实际非零计数 |
| 静态接缝 | emitter 不再有 SetCurrent/GetDirty 游标调用或通过 RegAlloc.RAShape 写计数的调用；源码 diff 检查通过，无新增测试文件、脚本或 probe |

本批未重复独立 IR 契约或 PMU，前面的 PMU 只对应此前候选，不作为此二进制的性能结论。没有据此宣布完整 R3、只读 IR 或 R6 完成。

产物 SHA-256：swift_test 为 `85d9b2bb80190c71ea840bde08581027defcd8cb41c334069430a37a98e02957`，swift_ir_test 为 `1bda334dda52d02ab4de7377f60f7e15c409c85ff8819e965b27a7c3e9b35690`（本批仅构建），svm_translator_linux 为 `4fa25056545d14c4dfb0074313e6cb358347a79befe529ebe60b104eb3366cd1`。原始结果为 `/private/tmp/swiftvm-middle-results` 中 const-suite.xml、r3-const-verification-summary.json、const-{reference-,}fpr-metrics 的 XML/日志、r3-const-fpr-metrics-summary.json 及 r3-const-{bench-shape.log,shape-summary.json}。

同目录的 r3-const-source-overlay.tar.gz/manifest.json 包含相对起点的 199 个现存修改/新增源码文件和 38 个删除路径。源码摘要为 `936cf3db1efdd798cc6499fe49c7c69984f31fb8b2392486a4f1a46ba2bc79e8`，归档 SHA-256 为 `89479e6d663ee9eb7c58332c2b292cae306afa0bfb5e24a38e988a29c46d2874`，对应此批最终产物。

## R4 局部驻留与 Bind 退役

### 实现与责任边界

| 文件 | 本批责任 |
| --- | --- |
| `compile/allocate/controller.h`、`allocation.cpp` | 请求状态直接持有 AllocationFacts/CompileWorkspace，区间资格只判断一次；reserve 与 FPR contract 重试共用输入。目标端只返回 Ready/RetryWithoutFPRCoalescing，控制器执行有界重分配，成功 admission 后释放请求状态 |
| `compile/allocate/coalesce.cpp`、`transaction.{h,cpp}` | 对闭合的 GPR/MEM、width/low32/REF 组件换家；移除失效 reload，按其他活跃值、剩余 reload、保留池与 target clobber 重建释放寄存器的局部 mask；拒绝/异常恢复关联条目，成功提交推进统一分配版本 |
| `compile/allocate/scratch.{h,cpp}` | 分配扫描和 recolor 共用纯查询检查，计入去重 reload、结果和 terminal headroom；预算描述由后端提供 |
| `backend/arm64/compile/publications.cpp` | 先保存 MEM 匹配候选，再检查 guest-state window、固定 home 的实际使用、helper/clobber、隐式根与跨块边界；补齐 producer/extend 的实际 fixed residence，提交前检查 scratch 和所有必需 fixed/width 证明 |
| `backend/reg_alloc_gpr_locations.cpp` | preserved root 的 transfer 点及其后使用 displaced 副本；忽略指向已失效原 home 的低位/consumer 请求，保留指向其他复制 home 的显式选择 |
| `backend/arm64/alloc/target_allocation_planner.cpp` | FPR contract 在 GPR target 编辑前检查；必要契约完成后冻结。删除生产 Bind，不再保存声明快照或通过闭包重放分配 |

删除 BindDeclaredGPRHomes、BindTargetDefinition、Save/RestoreTargetDeclarations、TargetDeclarations、ReplayAllocation、allocation_replay，以及只服务旧流程的 fixed/publication undo、plan_bind 与 target_replay 计时桶。FPR contract retry 使用独立计数与计时；必需 fixed/width 验证仍开启。Abort 丢弃当前物理结果及其约束，重新准备需要重新分配输入，不恢复半份 target 声明。

局部编辑保守拒绝带 capture 的组件、未闭合 alias/width 链、已归属其他 publication 的组件和超出本块的生命周期；这些候选保持原位置与普通发表。narrow-preserved 修复使用已有 preservation action/backing，不扩展 transfer/end 来跨越 clobber。它允许 publication 从已规范化的 displaced 副本写回 W home，因此 CFG 测试同时检查入口宽度事实、真实翻译成功和第二块的 W 发表，不再限定必须生成 self-move。

### 正确性验证

最终 Android 三目标构建通过；已有测试文件中增加局部事务拒绝/回滚/提交、共享 reload 与其他活跃 owner、narrow-preserved 和三种分配入口的回归。FPR retry 用例先在工作线程分配，线程退出后在调用线程完成 contract 检查、重试和发射，检查 IR 编号与共享 use 图保持有效。没有新增测试文件、脚本或生产 probe。

| 验证 | 最终结果 |
| --- | --- |
| 完整套件 | 709 条含 section 的记录；12 条既有失败记录、67 个失败断言、0 个 error，失败名称/类型/消息/正文与 svm_const 对应基线逐项相同 |
| 独立 IR 契约 | 24 个用例、398 个断言通过 |
| GPR/FPR/region 审计 | SVM_VERIFY_PLAN=1，77 条记录、1438 个断言通过，含 Block、合格单块 Function、通用 Function、跨线程 FPR retry 和外部入口宽度事实 |
| 生产 guest 审计 | 默认与 SVM_BOUND_DECODE=0 分别运行 CoreMark、五类 benchmark，四组退出码和校验值正确 |
| 静态清理 | 生产 Bind、声明备份、重放闭包及其计时引用清零；源码 diff 检查通过 |

### Spill 与机器码

默认 CoreMark 的第三组相邻采样匹配全部可见 Function 单元的 PC/IR 数量：425 条 profiler 记录，汇总计数为 432 个发射单元。汇总包含未逐项列出的 standalone/helper 发射，未将这些单元宣称为已逐项匹配。五类 benchmark 在两种 decode 配置下均匹配全部 22 个可见单元。

| 配置/负载 | spill defs，前→后 | load/store，前→后 | host bytes，前→后 |
| --- | ---: | ---: | ---: |
| 默认 CoreMark，配对组 | 333→330 | 99/73→102/73 | 287224→287268 |
| 默认五类 benchmark | 162→161 | 245/150→245/150 | 19908→19908 |
| 关闭绑定的五类 benchmark | 162→161 | 245/150→245/150 | 19916→19916 |

spill 统计现在读取最终 mapping；其中的减少不能全部解释为运行时 spill 指令减少，实际 load/store 另列。相同可见单元组的 spill 总量未增长。CoreMark 早期默认组及关闭绑定组存在 0x25fb00/40、0x25fbbc/25 或未出现该单元的差别；地址均位于 guest libc 的 __dtoa。计时输出变化是可能来源，尚未独立归因。全部原始组保留；关闭绑定的 CoreMark 未得到严格配对组，只报告其汇总 spill 333→330，不据此声称该组通过逐单元 spill 门。

### 红魔 PMU 对照

SVM 基线固定为 svm_const，本批固定为 svm_recolor_final。使用同一红魔 NX729J、root、CPU 4、taskset 10、simpleperf stat --cpu 4 的 cpu-cycles/instructions 与空 SVM_JIT_CACHE。审计/profile 与 PMU 分开。三轮交错版本及两种工作量，第二轮反转版本顺序，共 36 点；所有退出码及输入对应校验值正确。

| 负载 | 稳态指令斜率变化，中位数 | 三轮范围 | 启动相关指令截距变化，中位数 |
| --- | ---: | ---: | ---: |
| 默认 CoreMark，2000/12000 | +0.0095% | +0.0073%～+0.0225% | -0.6429% |
| 五类 benchmark，scale 1/2 | -0.0059% | -0.2037%～+0.2039% | 不作稳定收益解释 |
| 关闭绑定的 CoreMark，2000/12000 | -0.0048% | -0.1154%～+0.0086% | -0.6150% |

三组指令斜率均在 1% 门内。CoreMark 截距包含 loader、dispatcher、运行时及编译等成本；benchmark 的截距波动较大，不用于归因编译收益。周期斜率变化中位数分别为 -1.1541%、-0.9351%、+0.8260%，不作为固定频率结论。governor 均为 walt，最大频率 1171200 kHz，前后当前频率为 729600～1171200 kHz；cpuss-0、cpu-1-0、cpu-1-6 分别为 28.5～33.2°C、28.5～32.8°C、27.7～30.9°C，名称按设备 sysfs 原样记录。低工作量 CoreMark 未达到官方计分时长门槛，本处只计算相同工作量的 PMU 两点指标。

本批没有重跑 FEX；固定源码 Android FEX 参照仍在 R0 待办。本批通过的是 SVM 局部正确性、指令斜率与已配对负载的 spill 质量门，完整 R0–R7 尚未完成。

### 产物与接续

最终产物 SHA-256：

- swift_test：fc6d712495004560ad6b76ae4762846c6936ad94fdb8bdc4dffd50a4c9faf209。
- swift_ir_test：c3b5c721918b39de89ef50cdf99da1427e80fd863c39c33243c47a27f2c8d6ef。
- SVM：43a8f7a1d4af83471896ac14b9eff3985174606ee5ec1992890cd98a26f22302；对照 svm_const 为 4fa25056545d14c4dfb0074313e6cb358347a79befe529ebe60b104eb3366cd1。

原始文件位于 /private/tmp/swiftvm-middle-results：r4-final-{suite,ir,audit}.xml、r4-final-{verification-summary,quality-gate,pmu-conditions,pmu-summary,shape-summary,core-matched-groups}.json，以及相应 profile/guest audit/36 点 PMU 日志。CoreMark guest 哈希 d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba，benchmark guest 哈希 df60aa3f57d40ed3eb0b44d124f2cb8c9d15899f90021b6d20e94be179240a6d，后者沿用已有 Android native oracle 校验值。

r4-final-source-overlay.tar.gz 与 r4-final-source-manifest.json 包含相对 2804a51b 的 207 个现存修改/新增源码和 38 个删除路径；源码摘要 ba021605e580d9077c296cad480ed1ed45f64ac184c1add14df8429f3715bc28，归档 SHA-256 为 0456843777597992cf81d9be22da0614013966827815e5d284da07a80a901981。早期带旧断言、尚未通过的 r4-no-bind-* 结果保留为排错记录，不作为最终验证结果。

接续 R1/R3 的完整 IR 只读封口、AllocationView 与约束输入/结果分离，再推进 R5/R6 的候选/actions/edges 和 emitter 接口清场。RegAlloc 的 CompleteGPRAllocation 目前仅转发到公共控制器，兼容入口仍待 R7 删除；UnitFacts/元数据的最终归属、旧构建家族、摘要/通知范围等未完成项继续保留。

## R1 只读参数与容器接续

本批收紧既有查询接口，不以 const 修饰代替完整 IR 封口：

- const Block::GetInstList 返回 const InstList，pseudo 收集返回 const Inst 指针；删除为了旧查询保留的 mutable 指令列表和参数数组。
- Params 的 const iterator 返回 const Param；IR 参数查询返回只含读取能力的 Params::View，复制视图仍不能 Push/Destroy 或修改原参数节点。Arg 禁止浅拷贝，保留构造/移动，以阻止将只读参数槽复制成可写的共享列表；写入继续通过 SetArg/RewriteContext。
- const Lambda::GetValue/GetImm 返回 const 引用，address 不再 mutable。Arg 的 const 查询共用保留 const 属性的 Storage 实现，移除原 const_cast 转发。
- 解释器和 RISC-V 的 scalar 写回按 const Inst 读取 SSA 编号；RISC-V cache/snapshot/last-use 使用 const 定义指针，未用 const_cast 绕过新查询接口。
- 删除无消费者的 VirRegID/SetVirReg/vir_reg，以及 uniform 等价检查中始终比较该空字段的断言；保留其 opcode、编号、类型、uses 和参数等语义检查。Android DWARF 中 Inst 从 88 字节变为 86 字节，arena 仍按 96 字节槽分配，未据此声称内存占用下降。

编译期契约覆盖 const 列表/元素、pseudo、Lambda、Params::View 和 Arg 不可拷贝。现有 HIR Lambda/Params 重写与 uses/CSE 用例继续验证实际数据关系，没有新增测试文件、脚本或 probe。

| 验证 | 结果 |
| --- | --- |
| Android 构建 | 三目标通过，最终日志 build-r1-const-arguments.log |
| 完整套件 | 709 条含 section 的记录；12 条既有失败记录、67 个失败断言、0 个 error；与 R4 最终失败名称/类型/消息/正文逐项相同 |
| IR 契约 | 24 个用例、398 个断言通过 |
| 五类 benchmark | 对照 svm_recolor_final，22 个单元的 PC/IR/host bytes、校验值及全部 shape 相同；spill/load/store 为 161/245/150，host bytes 为 19908 |
| CoreMark | 2000 次退出码正确，CRC 0x4983 |
| 清理 | VirReg、旧 mutable 容器/地址和 Arg const_cast 查询引用清零；源码 diff 检查通过 |

本批未追加 PMU，R4 的 PMU 不作为此新二进制的性能数据。输出为 /private/tmp/swiftvm-middle-results 中 r1-const-{suite,ir}.xml、r1-const-verification-summary.json、r1-const-shape-summary.json 和对应日志。

最终 SHA-256：swift_test 为 c0b9f2bc87e40fd5d1e1c3b059e536f641e31e71a8c6ac0f486ee88c344890fd，swift_ir_test 为 685f60dfb8072736c8d87b3d7e2628a97b163c70bbf46a7474a5e125cf4c8beb，SVM 为 9005fac512a315484ab32a9a1ae4591de74424004395cf432abdc5b8c951a976。r1-const-source-overlay.tar.gz 与 r1-const-source-manifest.json 包含 214 个现存修改/新增源码和 38 个删除路径，源码摘要为 743ec4ff92e3057c7fdd0abde48b9fca4e14d434d09a3dc262e95f464031f701，归档 SHA-256 为 53acab91d08c6fdd7511cbb5f3dd2598f3d68a579d067d10a8d5992c7fb55a85。

此只读容器批次尚留 Value::Def/旧 SetType 等入口；后续 typed 构建批次已删除 SetType。Value::Def、HIR 与 metadata 的传递访问、写入阶段约束和最终 AllocationView 仍需接续，完整 R0–R7 尚未完成。

## R1/R2 创建时定型与旧类型写入退役

本批删除 `Value::SetType`，将原先三千余处 append 后定型的调用改为创建时传入结果类型。变量上的定型按语义处理：结果类型移到对应 producer 的构建点，消费者读取视图使用 `SetCastType`，实际宽度转换保留显式节点。没有新增构建结果包装类来继续导出旧 setter。

| 文件或目录 | 本批职责与变化 |
| --- | --- |
| `ir/{block.h,hir_builder.h}` | Block、HIRBlock、HIRFunction、HIRBuilder 接受运行时结果类型；在参数登记、链接与 HIR uses 建立前设置类型，覆盖 fast/slow 构建路径 |
| `ir/{args.h,instr.h}` | 删除 Value 的 canonical 类型写入接口；保留 use-edge 视图。本批结束时 Value::Def 尚未关闭，后续定义只读批次已接续 |
| `compile/builder/ir_builder.h` | typed 构建通过 `AppendRawTyped` 一次初始化；CallHost、traits、uniform effects 和 FP-free helper 增加显式返回类型入口，保留原 helper 契约 |
| `compile/builder/{guest_state,instruction,ir_builder}.cpp` | 删除 `UniformFlushRestamps`、restamp 容器、析构补写及每次 append/commit/terminal 的反复定型；uniform alias/extract/GetHost 在创建时定型 |
| x86/AArch64 frontend 与现有测试调用方 | 迁移旧定型调用，覆盖标量、SIMD、helper、地址和存储视图；继续使用共享规则与 guest 提交边界 |
| `aot/aot_load.cpp` | 补上 ScopeExit 的直接 include，避免依赖其他模块的传递 include |

Unknown flags 继续保守，不因当前未看到 pseudo uses 就视为无输出；已声明的输出掩码与追加校验继续有效。尚未显式给出结果类型的推断式构建入口、完整 flags/guest 提交契约、残余工作通知与队列仍需接续。本批只关闭 SetType 这条写入路径；Value::Def、HIR/metadata 的传递访问、阶段约束与最终 AllocationView 尚未完成。

| 验证 | 最终结果 |
| --- | --- |
| Android 主目标 | `swift_test`、`swift_ir_test`、`svm_translator_linux` 构建通过，日志 `build-r1-typed-final.log`；共享调用方构建后主目标 dry-run 无待构建项 |
| 共享调用方 | `swift_riscv_backend_test`、`swift_riscv_emit`、`swift_entry_persistence_test`、`swift_address_space_shutdown_test` 构建通过，日志 `build-r1-typed-shared-callers-final.log`；RV64 执行验证仍需对应架构环境 |
| 完整套件 | 709 条含 section 的记录，12 条既有失败记录、67 个失败断言、0 个 error；忽略源码行号移动后，失败表达式、展开值、类型和上下文与 const-IR 参照相同 |
| IR 契约 | 默认与 `SVM_IR_FAST=0` 各 24 个用例、398 个断言通过 |
| 已有 entry 持久化测试 | disk、aot、reject、native、aot-links、native-links 六模式，共 950 次检查通过 |
| 真实 guest audit | 默认和 `SVM_BOUND_DECODE=0` 的 CoreMark、五类 benchmark 校验值与退出码正确，`SVM_VERIFY_PLAN=1` 通过 |
| CoreMark 静态质量 | 两种配置在 2000 次工作量各匹配 425 条可见 Function 记录的 PC/IR/host bytes；汇总 432 个发射单元，spill/load/store 均为 330/102/73。默认 287268 bytes，关闭绑定 287272 bytes，分别与参照相同 |
| 五类 benchmark 静态质量 | 两种配置各 22 个单元的 PC/IR/host bytes 与参照完全相同；全部校验值相同，spill/load/store 均为 161/245/150，默认 19908 bytes、关闭绑定 19916 bytes |
| 清理 | 生产与测试调用方的 Value::SetType、uniform restamp 引用清零；未新增测试脚本、测试源文件或 probe；源码 diff 检查通过 |

CoreMark 的逐项匹配覆盖 profiler 可见的 425 条 Function 记录；432 的汇总还包含未逐项列出的 standalone/helper 发射。独立 shape 对照基于 2000 次工作量，不将所有 PMU 点宣称为逐单元匹配。完整套件仍含上述既有失败，不能称为全部通过。

在红魔 NX729J（bf1359ff）完成 36 点 PMU：三组负载各按两版本、两工作量、三轮交错采集，第二轮交换版本先后顺序。保持 root、CPU 4、`taskset 10`、同一 simpleperf、空 `SVM_JIT_CACHE`，采样时关闭 profiling/audit；36 点退出码与 CRC/校验值全部正确，未出现 affinity 警告。

| 对照组 | 指令斜率变化中位数 | 三轮范围 | 启动/编译相关指令截距变化中位数 | 周期斜率变化中位数 |
| --- | ---: | --- | ---: | ---: |
| 默认 CoreMark，2000/12000 | +0.0123% | -0.1153%～+0.0241% | -1.1526% | -0.4571% |
| 五类 benchmark，scale 1/2 | -0.0048% | -0.0144%～-0.0033% | -0.6884% | -0.8345% |
| 关闭 bound decode 的 CoreMark，2000/12000 | -0.0039% | -0.8822%～+0.0348% | -1.0752% | +0.8643% |

所有指令斜率变化均在 1% 门内。CoreMark 指令截距包含 loader、运行时及编译工作，变化比较涵盖本批全部构建/定型/restamp 改动，不归因单个子项。benchmark 周期截距与总时间截距波动较大，不用于证明稳定的编译耗时收益。governor 均为 walt，最大频率 1171200 kHz，前后当前频率 940800～1171200 kHz；cpuss-0、cpu-1-0、cpu-1-6 分别为 29.3～32.4°C、30.5～32.4°C、28.5～30.9°C。采样前电池记录为 AC 供电、100%、状态码 5、26.2°C；传感器含义按设备原样记录。周期结果不作为固定频率比较，短工作量 CoreMark 不报告正式分数。

SVM 参照为 `svm_const_ir`，SHA-256 `9005fac512a315484ab32a9a1ae4591de74424004395cf432abdc5b8c951a976`；候选 `svm_typed_ir_final` 为 `111fd221445770f20393749e83dd8ca15ce858b7bbc4539db5f937036bca7513`。最终 swift_test 为 `29f0edb022f4b18fce6a78b88e3a590035646fc18cbb670ddd82458b794a856a`，swift_ir_test 为 `b1a5194e19b20a6e8cf6bba1bd17266aa7f9aa886677731399399578f86b6581`。

CoreMark guest 哈希沿用 `d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba`，benchmark 为 `df60aa3f57d40ed3eb0b44d124f2cb8c9d15899f90021b6d20e94be179240a6d`，simpleperf 为 `30b6cd23fb8d819c835487f2fe6c3c416324488c7ac26c194ee7ba200f15b575`。本批未重跑 FEX；已核对设备 FEX 使用 Android `/system/bin/linker64`，固定源码及 Android 构建路径仍待确认，正式三方基线继续归 R0。

原始数据位于 `/private/tmp/swiftvm-middle-results`：`r1-typed-final-{suite,ir,ir-slow}.xml`、`r1-typed-final-{verification-summary,entry-summary,shape-summary,pmu-points,pmu-summary,quality-gate}.json`，以及对应 guest/entry/PMU 日志。早期 `r1-typed-v1-*` 是本批收尾前的记录，最终结论使用 final 产物，slow 路径以独立 IR 套件的 24/398 结果为准。

相对 2804a51b 的最终源码归档 `r1-typed-final-source-overlay.tar.gz` 包含 250 个现存修改/新增源码及 38 个删除路径，源码摘要 `c0d7557d1cb8aeb2f83013dc70532af9b3a2dacdb8c9ea265eda0ee608d011b6`，归档 SHA-256 `e8ab2ce428c4bf605fbdeac5c17ac8a595fcf752d7dd75fc96dbe4658e110bbd`；详细哈希见同名前缀的 source-manifest.json。完整 R0–R7 仍在推进，继续关闭只读 IR 的定义指针与 HIR/metadata 逃逸，并迁出最终 Allocation/AllocationView。

## R1 定义指针与 metadata 指令引用只读接续

Value 的定义读取现在返回 `const Inst*`，构造、TypedValue 和 WithDefinition 同时接受 const 定义身份。Value 的 Use/UnUse 改为私有 bookkeeping，只允许指令构建、terminal 构建与 HIR teardown 登记引用；计数字段为私有 mutable，不向读取者导出写入能力。新增编译期契约检查，确认读取 Value 不能调用定义的 SetReturn，也不能直接登记或撤销 use。

| 文件或目录 | 本批变化 |
| --- | --- |
| `ir/{args.h,args.cpp,instr.h}` | const 定义身份贯通 Value 与 TypedValue；语义 setter 保持可写对象接口；use 计数由内部维护 |
| `ir/{block.h,block.cpp}` | capture 的 segment/boundary/latest_store 和 hoist prefix 指向 const Inst；anchor 为 `span<const Inst* const>`，复制 metadata 也不能改写 anchor 数组或 producer；删除和插入位置以 const 身份定位，由可写 Block 操作列表 |
| `ir/{hir_builder.h,hir_builder.cpp}` | HIRUse 的 consumer 为 const Inst；UseInst/UnUseInst 和删除接受 const 身份；terminal uses 释放移入拥有写入职责的 HIRFunction 私有操作 |
| `compile/{unit.h,unit.cpp}` | DCE 队列与 owning anchor 存储使用 const 指令身份，继续复用 workspace，不复制 IR |
| `compile/passes` | 单个 Change 入口向写入 callback 提供 `Inst&`，统一维护 HIR uses、flags 通知及回收；其余查询、根与替换索引使用 const 指令。可写能力恢复集中在此入口，读侧没有添加 const_cast |
| `compile/{allocate,analysis}`、后端 `alloc/compile/jit/interp` | 分配事实、家族计划、区间与发射的指令参数/容器改为 const；解释器使用 const 指令迭代器。RV64 flatten/wide 保留自己创建的可写副本，原 IR 与发表的查询引用为 const |
| 既有测试 | 稀疏编号、malformed IR、Phi 回边改用明确的改写作用域；HIR Phi 替换同步 uses。保留原测试语义与独立 Block/Function 路径，未新增测试文件或脚本 |

移除无运行时消费者的 `ValueAllocated` 类型及 `HIRValue::allocated` 字段，同时删除 fast/slow HIR 对照中始终比较默认 NONE 的断言；opcode、类型、编号、block 与 uses 校验继续保留。Android DWARF 的 HIRValue 从 40 字节变为 36 字节。ObjectPool 的 Storage 按对象类型布局分配；下述 profile 的 pool_bytes 容量请求统计均减少 24576 字节，不把该计数解释为进程 RSS 或总峰值内存。

本批并未完成整个 IR 封口：HIR CFG/record owner 指针的传递访问、此前保留的构建句柄、单元阶段与真正的 owner/借用寿命约束仍需关闭；分配和准备入口仍有可写 Block/HIRFunction 参数。allocation generation 继续只表示物理结果身份，没有冒充 IR 修改检测；最终 AllocationView、R5 actions 与其余 R0–R7 工作继续保留。

| 验证 | 结果 |
| --- | --- |
| Android 六目标 | swift_test、swift_ir_test、svm_translator_linux、swift_riscv_backend_test、swift_entry_persistence_test、swift_address_space_shutdown_test 构建通过；最终日志 `build-r1-const-def-verified.log`，后续 dry-run 无待构建项 |
| 完整套件 | 709 条含 section 的记录；12 条既有失败记录、67 个失败断言、0 个 error。忽略源码行号移动后，失败内容与 typed-final 逐项相同 |
| IR 契约 | 默认和 SVM_IR_FAST=0 各 24 个用例、398 个断言通过；包含定义写入/use bookkeeping/metadata 的编译期只读约束 |
| 既有 entry 测试 | disk、aot、reject、native、aot-links、native-links 六模式、950 次检查通过 |
| 额外 RV64 源码 | 8 个未进入当前构建目标的测试文件，用既有 Android ARM64 backend-test 参数执行 -fsyntax-only 均通过；此项不覆盖 __riscv 条件分支与原生 RV64 执行 |
| 默认及关闭绑定 CoreMark | 各自与 typed-final 相邻对照；两种配置均匹配全部 425 条可见 Function 的 PC/IR/host bytes。汇总 432 个发射单元，spill/load/store 为 330/102/73；host bytes 分别为 287268、287272，CRC 正确 |
| 默认及关闭绑定 benchmark | 每种配置 22 个单元的 PC/IR/host bytes、全部 shape 与校验值相同；spill/load/store 为 161/245/150，host bytes 为 19908、19916 |
| guest audit | 上述 guest 以 SVM_VERIFY_PLAN=1 通过，退出码正确 |
| 对象池容量计数 | 默认 benchmark 559664→535088 bytes，关闭绑定 578864→554288；两种 CoreMark 配置均为 685128→660552 |
| 清理 | ValueAllocated、公开 Value use bookkeeping 调用及测试通过 Def 直接修改节点的引用清零；源码 diff 检查通过 |

CoreMark 的逐项质量对照范围是 profiler 可见的 425 条 Function；432 的汇总还包括未逐项列出的 helper/standalone 发射。完整套件仍有既有失败，不能称为全部通过。本批没有重新采集 PMU，前一批 typed-final 的 36 点数据不作为此新二进制的性能数据。

本批 SVM `svm_readonly_def_v1` SHA-256 为 `2e97776abc78aa9c57320c7fa6783611671b3e3d435ca3bdcf42323eed753dcc`；swift_test 为 `e60fc528d2320482e49f746e7dec7988e673a7f54d132ad0fd0ac6f9b8f9f9ff`，swift_ir_test 为 `1df3180c81844e5175e0bc4bd5115d4934f7a5b6a7c38c3fb04cb38a0f0c5621`，entry 测试为 `7f83c91cdfd233fc19b049860962b9b1644d6467f3d501317aec06b28d2b8fa5`。静态参照是前一批 `svm_typed_ir_final`（111fd221445770f20393749e83dd8ca15ce858b7bbc4539db5f937036bca7513）。

结果位于 `/private/tmp/swiftvm-middle-results`：`r1-readonly-def-v1-{verification-summary,entry-summary,shape-summary,quality-gate,run-commands}.json`、suite/ir/ir-slow JUnit、对应 guest/entry 日志、`r1-readonly-def-syntax-summary.json` 和两份 layout 日志。源码归档 `r1-readonly-def-v1-source-overlay.tar.gz` 相对 2804a51b 包含 337 个现存修改/新增源码和 38 个删除路径；源码摘要 `f5d040cfbf33bfd55b6169f6ca66abef8277b4fe45dbdf0b9cdcb9051b05a79d`，归档 SHA-256 `0eb5552e3b42d5f5d6a81607979b59e0cb1d4acd84b356395929d5e5f19ffaab`，细目见 source-manifest.json。

## R1 HIR 查询与分配输入只读接续

HIRFunction/HIRBlock 的 const 查询现在返回只读块、值记录、边、RPO、前后继和 call-return 引用，const HIRValue 也不再导出可写的 owner。块和 value 指针集合使用 `span<const T* const>`，直接借用现有存储；没有为查询复制整组对象，也没有添加另一套 IR 节点或在读取端恢复可写指针。

| 文件或目录 | 本批变化 |
| --- | --- |
| `ir/hir_builder.{h,cpp}` | 补齐 const 查询与只读 span；Edge 端点和 HIRValue owner 改为私有字段并提供对应访问；DataContext 的计数查询改为 const；删除无消费者的 dominance/frontier/backedge 存储、DfsHIRBlock、DOMINATES 标记清理循环及两个未实现的 merge/split 接口 |
| `ir/block.{h,cpp}` | IsEmptyBlock/IsJitCached 改为 const 查询；缓存状态使用既有 BitField::get 读取 |
| `compile/{allocate,analysis}` | AllocateBlock/AllocateFunction、facts、区间、活跃性和扫描器的实际 IR 参数改为 const；模板仍分别选择 Block 与 Function 路径，合格单块 Function 的入口证明保持原有要求 |
| `backend/arm64/{alloc,compile}`、`backend/reg_alloc*` | 目标约束、家族计划、验证及物理事务读取 const IR；RegAlloc 保存 const 单元身份；region preparation 继续使用原有证书存储，读取新 span 时按范围捕获和比较 |
| `backend/arm64/jit` 的分析器、`backend/function_entry_contract.*` | use index、GuestStateMap、scalar/carry/div128 分析与入口契约接受 const 输入；它们自身的派生事实仍可构建，不修改 IR |
| `tests/ir_contract_test.cpp`、既有调用方 | 加入 HIR 容器元素、边端点、value owner 与 use 链的编译期只读约束；迁移既有读取调用，未新增测试文件、脚本或 probe |

HIRBlock 的 Android DWARF 大小由 200 字节降为 160 字节。本批仍保留构建和改写期间所需的可写接口；尚未禁止此前取得的构建别名跨过封口点，也未完成 PreparedUnit 对真正 IR owner/借用寿命的约束。const 输入接续不能视为 R1 全部完成，AllocationView、R5/R6 和固定源码 FEX 参照继续在未完成范围内。

本批在红魔 NX729J 上集中验证一次；相邻参照是上一批固定产物 `svm_readonly_def_v1`。

| 验证 | 结果 |
| --- | --- |
| Android 六目标 | 主测试、IR 契约、SVM launcher、RV64 backend、entry persistence 和 address-space shutdown 构建通过；日志 `build-r1-hir-const-verified.log`，后续 dry-run 无待构建项 |
| 完整套件 | 709 条含 section 的记录；12 条既有失败记录、67 个失败断言、0 个 error。源码行号移动归一化后，失败消息、类型和正文与上一批一致，测试记录名称集合相同 |
| IR 契约 | 默认及 SVM_IR_FAST=0 各 24 个用例、398 个断言通过，含新增 HIR 编译期约束 |
| entry 六模式 | disk、aot、reject、native、aot-links、native-links 共 950 次检查通过 |
| 额外 RV64 源码 | 8 个未进入当前目标的既有测试源文件，用 Android ARM64 参数执行 -fsyntax-only 均通过；不覆盖 __riscv 条件分支或 RV64 原生执行 |
| CoreMark 默认/关闭绑定 | 两种配置均与相邻参照逐项匹配 425 条可见 Function 的 PC/IR/host bytes，完整 shape 与 CRC 相同；汇总 432 个发射单元，spill/load/store 为 330/102/73，host bytes 为 287268/287272 |
| benchmark 默认/关闭绑定 | 每种配置的 22 个单元、全部 shape 与五项校验值相同；spill/load/store 为 161/245/150，host bytes 为 19908/19916 |
| guest audit | 上述八次相邻对照均启用 SVM_VERIFY_PLAN=1，退出码均为 0 |
| 对象池容量计数 | CoreMark 两种配置均为 660552→641352 bytes；默认 benchmark 为 535088→524528，关闭绑定为 554288→543728 |

pool_bytes 表示对象池容量请求计数，不是进程 RSS。CoreMark 的逐项比较只覆盖 profiler 可见的 425 条 Function，汇总 432 还含未逐项列出的 helper/standalone。完整套件仍有既有失败。本批未重采 PMU，历史采样不作为本批新产物的性能结果。

SVM `svm_hir_const_v1` SHA-256 为 `15e78bb147da1740a0d9f529bc0fe669048112b38facad0f9569953ae8a3b1dd`；swift_test 为 `2fa87ffbbde8b7099d1e1cba1ab2eddd620a5626ddc9b3354fbf9b0be2990d03`，swift_ir_test 为 `3ce94b02cd33d0f94c3255b659428810b44d6a5cb0f45c75b8736477aa804ab9`，entry 为 `67dd8c8146980c168e0340ccc48780fde243ba14dcb24d67b3a5f87aaca454c5`。主机与设备上的这四份产物哈希一致，参照 SVM 哈希也已核对。

原始数据位于 `/private/tmp/swiftvm-middle-results`，前缀 `r1-hir-const-v1-`，包括 verification/entry/shape/syntax/layout/quality-gate 摘要、完整命令、JUnit 与运行日志。源码归档 `r1-hir-const-v1-source-overlay.tar.gz` 相对 2804a51b 包含 344 个现存修改/新增源码和 38 个删除路径；源码摘要 `8db4274d7931c0925dea4dcac7654940b1dae874dbcd8508a1b883ad6cd924bf`，归档 SHA-256 `cf0a928b660feafd997cc94824ad419dff5ca03f059b3487aeeb0cc6ed5d4988`。运行后已逐文件核对源码摘要，完整产物身份见 source-manifest.json。

## R1 指令写入边界与封口作用域接续

Block/HIRBlock 的指令列表现在对可写调用方也只提供只读查询，Inst 的链节点转为私有。构建时取得的 Inst 指针仍受所属 Block 的共享封口状态约束：改变 opcode、操作数、返回类型、编号和 use 登记均检查写入权限。挂入、摘下、移动及删除由 Block 操作；RewriteContext 在注销 HIR uses 前核对实际指令归属，Function 改写还通过块序号与 roster 中的实际指针校验所属单元，不新建成员哈希表。

| 文件或目录 | 本批工作 |
| --- | --- |
| `ir/instr.{h,cpp}`、`ir/args.cpp` | 指令保存所属 Block 状态的引用；写入口检查封口，use 登记改为内部职责。新输入在修改 operand slot 前验证，拒绝使用已封口定义时保留原参数与计数。Validate 改为只读查询 |
| `ir/block.{h,cpp}` | 列表和 capture recipes 统一只读；增加校验归属的写入口与块内移动，删除 GetBeginInst。capture recipes 通过显式取出/提交复用容量；terminal、编号和元数据写接口检查封口 |
| `ir/hir_builder.{h,cpp}` | HIRValue 的定义身份不可变，use 链及登记方法私有，值表/记录查询统一返回只读视图。内部 FindValueRecord 同时核对编号和定义指针，拒绝外来同编号定义；构建、CFG 和编号操作接入受控写入检查 |
| `compile/unit.{h,cpp}` | SealIR 借用 Block/Function，在块上设置共享状态，不逐指令扫描封口；移动操作转移封口职责。析构先解除封口，再解绑自有 metadata；外层 IR owner 必须覆盖该作用域 |
| `compile/passes/{passes.h,dce.cpp,flags.cpp,state*.cpp,loop_hoist.cpp}` | 读取使用 const 身份；改写先验证归属。提升通过 Block 的块内移动接口重排。sink 先确定 carrier 的待提交值，重新挂回后再提交 operand 改写，避免在失去 Block 归属时写入 |
| `backend/runtime.cpp` | ARM64 Function、普通 Block、独立 HIRBlock 在物理分配前封口。Function 的 IR 回收移到 prepared/emitter/分配作用域结束后；普通 Block 的回收作用域也晚于所有消费者与 CompileUnit 析构 |
| `translator/x86/translator.cpp` | 删除无调用者的 FixupConsecutiveFlagOps、FixupFlagFlushBeforeConsumer 及其旧 workaround 注释 |
| 既有测试 | 新增一项永久 IR 契约用例，覆盖 Block/Function 的旧写句柄、错误归属、失败后参数和 uses 不变、CompileUnit 移动及解除封口；稀疏编号与 malformed IR 夹具改用显式 RewriteContext。没有新增测试文件、脚本或 probe |

新增归属检查在整套验证中识别出 XMM sink 的两处失败：原实现对已摘链的 carrier 直接 SetArg，再重新挂回。修正后的提交顺序保留检查；17 个 fault/XMM 用例、175 个断言定向通过后，再完成最终 v2 整套验证。没有通过放宽归属检查继续消费 detached IR。

当前边界覆盖受控的指令和 Block 写接口。HIR CFG 容器、前后继/边的可写别名、AddressNode 范围及完整 PreparedUnit 所有权尚未全部收口，其他调用方也尚未全部要求封口输入；不能把本批等同于 R1 或 R6 完成。Function 的 canonical 合并/发布交接仍在 runtime，后续需要接入最终 compiler/PreparedUnit 模型。

| 验证 | 最终 v2 结果 |
| --- | --- |
| Android 构建 | 前述六目标及既有 swift_hir_lifetime_test 均通过；主构建 `build-r1-write-boundary-verified.log`，sink 修正后增量构建 `build-r1-write-boundary-v2.log`，最终七目标 dry-run 无待构建项 |
| 完整套件 | 709 条含 section 记录；12 条既有失败、67 个失败断言、0 个 error。源码行号移动归一化后，失败类型、消息与正文及测试名称集合均与 hir-const-v1 相同 |
| IR 契约 | 默认及 SVM_IR_FAST=0 各 25 个用例、448 个断言通过 |
| HIR 生命周期 | unfinished、published、construction、finish-retry 在 fast=0/1 的八种组合均通过，含既有分配失败与 finish 重试覆盖 |
| entry 六模式 | disk、aot、reject、native、aot-links、native-links 共 950 次检查通过 |
| 额外 RV64 源码 | 八个既有源文件的 Android ARM64 参数语法检查通过；pure_scalar_flags 的 malformed store 改用显式改写入口。此项不覆盖 __riscv 分支或 RV64 原生执行 |
| guest audit 与代码形状 | 默认及关闭绑定的 CoreMark、benchmark 八次相邻运行均以 SVM_VERIFY_PLAN=1 正常退出，CRC/校验值和全部 shape 相同；可见单元的 PC/IR/host bytes 逐项匹配 |
| CoreMark 本轮范围 | 默认配对为 424 条可见 Function、汇总 431 个发射单元，host bytes 为 287080；关闭绑定为 425/432、287272 bytes。两种配置的 spill/load/store 均为 330/102/73 |
| benchmark 本轮范围 | 两种配置各 22 个单元，spill/load/store 为 161/245/150，host bytes 为 19908/19916，五项校验值相同 |
| 容量与布局 | Inst 为 86→94 字节，arena slot 仍为 96；Block 为 240→256 字节。相邻 profile 的 pool_bytes 和 ir_arena_bytes 均相同，不把它们当作总内存无增长的证据 |

CoreMark 的可见单元数按本轮配对记录，不沿用先前批次的固定数量；汇总还包括未逐项列出的 helper/standalone。布局证据来自 v1 的 DWARF，v2 仅修改 state_sink.cpp，IR 源文件/头和 ABI 保持相同。八份额外 RV64 语法结果同样复用该批接口验证。完整套件保留既有失败，未称为全部通过。

在红魔上另做 16 点相邻 SVM PMU 对照：默认 CoreMark 2000/12000 三轮交错采样，关闭绑定一轮。固定 root、CPU 4、相同 guest 与 simpleperf，空 SVM_JIT_CACHE；16 点均正常退出，CRC 为 0x4983/0xd340，无 affinity 警告。

| PMU 指标，相对 hir-const-v1 | 默认三轮 | 关闭绑定一轮 |
| --- | --- | --- |
| 稳态指令斜率变化 | 中位 +0.00499%，范围 -0.01466%～+0.12744% | -0.00955% |
| 启动/编译相关指令截距变化 | 中位 +0.89589%，范围 +0.42876%～+0.95317% | +0.96263% |
| 周期斜率变化 | 中位 +0.53015%，范围 +0.01563%～+0.77199% | +0.26954% |
| wall-time 斜率变化 | 中位 +0.10625%，范围 -0.40937%～+0.17348% | +0.51717% |

新增写入检查带来的启动/编译相关指令成本约增加 0.9%，后续继续跟踪。截距包含启动与运行时成本，也可能受自适应编译影响，不单独解释为编译耗时。governor 为 walt，当前频率读数为 940800/1171200 kHz，最大频率为 1171200 kHz；cpuss-0、cpu-1-0、cpu-1-6 分别为 29.7–33.2、30.9–33.6、28.9–31.6°C。传感器按 sysfs 名称记录，不推定为 CPU 4 的独占温度。

本轮未采 benchmark PMU、未重跑 FEX，也未完成固定源码 FEX 参照。simpleperf 日志未显式报告逐事件 running ratio，本轮保留原始统计，不额外作 IPC 结论；固定工作量不足 CoreMark 正式计分时长，只用于 CRC 与 PMU。新增开销、采样局限和正式 R0 三方协议均继续保留，不能用本组数据宣告完整性能验收完成。

最终 SVM `svm_write_boundary_v2` SHA-256 为 `b5646edf79177762ac4b028da6a3bfed3eff59d92baae8960c0aa5dbd98ca8a3`；swift_test 为 `4e8d3bb03f28c23e4d4e07e5b1522385d17edc7036019a863167a70deea185de`，swift_ir_test 为 `0a1ff09c4ff34edba1122e66719465d58a1c09a519519145ee87c98b458bac1c`，entry 为 `675b28425d653a5be5ba82a964df9b0f2a99598b5fafc9178c84ca2e5f19e2ee`。上述产物及 lifetime 测试已核对主机/设备哈希，PMU 参照 SVM 为 `15e78bb147da1740a0d9f529bc0fe669048112b38facad0f9569953ae8a3b1dd`。

结果位于 `/private/tmp/swiftvm-middle-results`，最终前缀 `r1-write-boundary-v2-`：verification/entry/lifetime/shape/layout/quality-gate 摘要、完整命令、pmu-points/pmu-summary、JUnit 与运行日志。源码归档相对 2804a51b 包含 344 个现存修改/新增源码和 38 个删除路径，源码摘要 `6d9a1cf8f0d3748792c9b638a89e44182a52aa530921f02cce516dc3a780c520`，归档 SHA-256 `7839c35dcd420aaf5b61c6286b4e0fe5103ce67a728034c9bccb5bc4e5d98889`；运行后逐文件摘要核对通过。

## R1 HIR CFG 只读查询与显式 writer 接续

本批对应 `r1-cfg-boundary-v1`，邻接参照为已经验证的 `svm_write_boundary_v2`。本批关闭 HIR CFG 的可写查询出口，继续保留现有 IR/HIR 模型及 Block、合格单块 Function、通用 Function 的区分。

| 文件或接缝 | 当前职责与变化 |
| --- | --- |
| `ir/hir_builder.{h,cpp}` | 成员表、构建列表、RPO、前驱/后继、call-return 和边端点统一只读；链节点私有。EditBlock 检查 Function 归属和实际 Block 的封口状态，decode/pass 显式申请 writer；AddEdge/RemoveEdge 在写列表前完成两端检查。封口循环仍在 HIRFunction 内部，不经公开查询返回可写容器 |
| `ir/function.{h,cpp}` | const EntryBlock/FindBlock 返回 const Block；两个 FindBlock 重载共用相同查找实现，不通过只读侧 const_cast 获取写权限。持久 Function 的其他可写入口仍须继续收口 |
| `compile/unit.cpp`、`compile/passes` | CompileUnit 调用内部封口操作；DCE、flags、values、widths 和提升分别保留语义写入与只读分析，写入方通过所属 Function 获取 writer |
| `backend/arm64/jit` | JitContext、JitTranslator、函数对象发射与其分析帮助函数接收 const IR；不因读 CFG 而取得可写 Block/Function |
| `backend/riscv64/jit` | HIR FunctionPlan 与克隆输入只读；范围准备和发布仍通过实际 Function 的 writer 更新自身数据。此项不表示 RV64 已完成 PreparedUnit 迁移 |
| `translator/x86/function_*` | frontier 和 decode 队列保存 const HIRBlock 身份；构建通过 HIRBuilder/SetCurBlock/EditBlock 进行 |
| 现有测试支持与契约用例 | 畸形 roster/RPO/边测试通过测试目标内部 HIRTestSupport 构造，生产查询保持只读；原有阶段用例补充外来边拒绝无副作用、单独 Block 封口、Function 封口与保留 writer 的检查；没有新增 probe 或测试脚本 |

全树检查确认没有消费者后，删除 DataContext 虚基类、HIRBlock 恒返回零的三个 count 接口、未使用的 Edge 条件标记、HIRFunction::GetMemPool 和 HIRBuilder::GetHIRFunctions。独立 HIRBlock 发布的 begin/abort/commit 增加封口检查；HIR 追加指令在分配节点前检查目标 Block 可写。封口仍依赖外层拥有 IR，CompileUnit 只拥有提升存储和封口职责；当前不能宣称完整 R1 已完成。

本批在红魔 NX729J（`bf1359ff`）上集中验证，编译参数沿用前述 NDK 29/API 28/Release 配置：

| 验证 | 实际结果 |
| --- | --- |
| Android 构建 | 7 个目标构建完成，最终增量检查无待执行任务，git diff --check 通过 |
| 主套件 | 709 条记录；与 write-boundary-v2 的用例名集合、失败类型、断言及展开内容一致，仅忽略源代码行号。仍为原有 12 条失败记录、67 个失败断言、0 errors；本次报告断言总数为 220892，不作为固定覆盖率要求 |
| IR 契约 | 默认和 SVM_IR_FAST=0 各 25 cases/464 assertions，全部通过；新增断言进入原有契约用例，没有增加一次性测试入口 |
| HIR 生命周期 | unfinished/published/construction/finish-retry × 两种 IR_FAST，共 8 组合全部通过；construction 每模式注入 5 次失败，finish-retry 每模式 2 次 |
| 入口持久化 | disk/aot/reject/native/aot-links/native-links 共 950 checks，全部通过 |
| RV 兼容性 | ARM64 构建包含 RV 后端目标；另 8 个 RV 测试源文件按相同 Android 参数完成语法检查。ARM 手机没有执行 RV64 原生测试；shutdown 本批仅构建 |
| Guest 对照 | CoreMark 与 bench 在默认和 SVM_BOUND_DECODE=0 下共 8 次运行均成功，SVM_VERIFY_PLAN=1；CoreMark CRC=0x4983，bench 的 5 个 checksum 一致，共同 PC 的逐次 IR 数/host 字节数一致，spill/load/store 未增加 |

CoreMark 的完整单元集合本批并不相同，不能把整份形态日志写为相等。默认参照为 424 条可见记录、候选为 425，候选多出 `pc=0x25fb00 ir=40 host=188`；关闭 bound decode 后两侧各 425，参照出现 `0x25fbbc/25/104`、候选出现 `0x25fb00/40/188`。guest 符号和反汇编表明两个地址均在 __dtoa 的十进制舍入分支，结合时长输出判断来自计时文本路径变化；旧版 write-boundary-v2 日志已有同一 `0x25fb00/40/188` 记录。共同 PC 无 IR 数或 host 长度变化；两模式 CoreMark 的 spill/load/store 均为 `330/102/73`，bench 为 `161/245/150`。bench 完整形态日志在两种模式下分别相等，host bytes 为 `19908/19916`。

DWARF 定义记录显示 HIRBlock `160→152`、HIRFunction `264→256`、HIR Edge `56→48` 字节；Block 仍为 256，Inst 仍为 94，Inst arena 槽仍为 96。实际 pool capacity：默认和 bound-off CoreMark 均从 `641352→632320`，bench 分别为 `524528→519360`、`543728→538560`；CoreMark 的工作集合有上述差异，不能将整个容量变化逐字节归因于单一字段。Inst arena 总量仍为 CoreMark 458752、bench 262144。

本批另集中采集 12 个 PMU 点：默认两轮交错顺序，bound-off 一轮，每轮两版本各执行 2000/12000 次；所有退出码、CRC 正确，未出现 affinity 警告。

| 候选相对 write-boundary-v2 | 默认两轮中位数（范围） | bound-off 一轮 |
| --- | ---: | ---: |
| instructions 稳态斜率 | −0.0482%（−0.1040%～+0.0076%） | +0.0172% |
| 启动/编译相关 instructions 截距 | +0.2518%（−0.0040%～+0.5076%） | −0.3888% |
| cpu-cycles 稳态斜率 | +0.8604%（+0.1945%～+1.5264%） | −1.9088% |
| wall time 稳态斜率 | +1.8314%（+1.1705%～+2.4924%） | −2.1497% |

逐点条件为 CPU4、taskset 10、governor=walt，观察到当前频率 1056000～1171200 kHz、上限 1171200 kHz；cpuss-0 为 29.3～33.2°C、cpu1-0 为 30.5～33.2°C、cpu1-6 为 28.5～30.9°C，这些是具名传感器，不能视为独占 CPU4 的温度。当前样本与频率波动只支持未见明显指令成本退化，不构成正式性能收益结论。截距含启动、编译和两点非线性，不能视为纯编译器时间；原始 stat 没有逐事件 running 比例，不新增 IPC 结论。CoreMark 保留低于 10 秒的时长提示，仅使用固定工作量及正确 CRC，不报告正式分数。FEX 本批未重新采样；固定源码 Android FEX 构建仍是 R0 待办。

源码快照包含相对 HEAD 的 356 个现存修改/新增源文件和 38 个已删除路径；source digest 为 `8adf1ae1d8c237c71928df726a017e3602e68569a86cae2af0c1928299af09a5`，overlay archive SHA-256 为 `4b2d9ec8b4d7513c86e7937e14cb65ef30f54fe8720805467c5f740a49f330e5`。候选 launcher `svm_cfg_boundary_v1` 的 SHA-256 为 `46b670ed2f6d5e4624b5f307ad9dfe18c77a3c7712fb4a4d004805cad2d0de4a`，邻接参照 `svm_write_boundary_v2` 为 `b5646edf79177762ac4b028da6a3bfed3eff59d92baae8960c0aa5dbd98ca8a3`。验证结束后源码、主机快照及设备二进制哈希均已复核。

全部原始日志、JUnit、命令、设备条件、布局与汇总保存在 `/private/tmp/swiftvm-middle-results/r1-cfg-boundary-v1-*`，设备原始结果仍在 `/data/local/tmp/svm-middle-r1/results`。源码 overlay 与 manifest、test/shape executions、verification/entry/lifetime/syntax/shape/layout/pmu summaries、quality-gate 和 device-hashes 分别记录身份与判定。AddressNode 范围和持久 Function 别名、完整 PreparedUnit owner/阶段/借用寿命、R2/R3/R5/R6/R7 剩余范围继续接续。

## R1 范围、持久成员与 Function 实际寿命接续

本批最终验证版本为 `r1-range-owner-v2`，邻接参照为 `svm_cfg_boundary_v1`。本批完成范围与持久成员写边界和 Function 实际引用寿命，完整 PreparedUnit/HIR pool 所有权仍未完成。

| 文件或接缝 | 当前职责与变化 |
| --- | --- |
| `ir/block.h`、`ir/function.{h,cpp}` | AddressNode 的 location、node_size、种类和 intrusive hooks 私有；种类只由派生构造确定，AddressNode 禁止复制，避免切片后的假身份。SetEndLocation 经实际 Block/Function 的封口检查；Function 成员表只读，添加、移除、转移与批量回收检查阶段及实际 Block 状态，转移/回收在修改前完成所需检查 |
| `runtime/externals/utils/util_intrusive_red_black_tree.hpp` | 修正 const find/lower_bound/upper_bound/nfind 及 key/existing 查询返回可写 iterator 的出口，提供相应非 const 重载，size 改为 const；不能只给 GetBlocks 加 const 而保留底层可写迭代器 |
| `ir/hir_builder.{h,cpp}` | HIRFunction 的原始持有指针改为 IntrusivePtr，构建失败时正常析构，发布后也不放弃自身引用。MarkFunctionPublished 只记录发布状态，替代旧 ReleaseFunctionOwnership；Function 直接删除其子 Block 的职责不变。封口拒绝未 EndFunction 的 HIR，并同时封住持久 Function；仅内部使用的 value 登记/回收接口改为 private |
| `backend/runtime.cpp` | ARM64 多 region 先全部优化与最终编号，合并持久 Block 所有权，再全部封口、分别分配和发射。普通 Block 的 guest 范围计算也移到分配和封口前；SMC 发布只消费已经准备好的范围 |
| `backend/module.cpp`、`smc_tracker.cpp`、`common/address_hash_map.h` | 通过种类和范围 getter 查询身份；关闭/退役读取 const 成员，publication cache 的清除与锁操作独立于语义 IR 可写性 |
| 现有测试支持与契约用例 | 畸形地址身份经测试内部 fixture 构造；原有阶段用例覆盖通过 AddressNode 基类写范围、成员转移/删除拒绝、未完成 HIR 拒绝封口，以及模块先结束而 HIR 仍持有有效 Function；发布寿命用例改为真实 intrusive 持有者 |

全树确认无人使用后删除 HIRBlock::CreateInst、HIR spill_stack 和 Function v_stack。没有新增源文件、临时 probe 或测试脚本。持久 Function 的引用利用已有 refcounter，不为函数拥有的 Block 新增 intrusive 引用，也不为此增加逐指令状态。

新增范围检查在首版 range-owner-v1 暴露了 W81 回归：普通 Block 在 SMC 发布期间才补写 guest end，此时语义 IR 已封口。首版主套件为 68 个失败断言；发现后停止后续设备项目，保留首版源码、二进制和失败日志。将同一范围计算移至分配前，先跑 W81 定向用例，再完成最终整批验证。最终版与首版只相差 runtime.cpp；IR 源文件/头文件及额外 RV 语法输入一致，相关哈希已复核。

红魔 NX729J（`bf1359ff`）上的最终结果：

| 验证 | 实际结果 |
| --- | --- |
| Android 构建 | 7 个目标构建通过，最终增量检查无任务；git diff --check 通过 |
| 主套件 | 709 条记录，与 cfg-boundary-v1 的用例名集合和失败语义一致，仅忽略源码行号；仍为原有 12 条失败记录、67 个失败断言、0 errors。本次报告断言总数 221052，不作为固定覆盖率要求 |
| IR 契约 | 默认和 SVM_IR_FAST=0 各 25 cases/486 assertions，全部通过；沿用原有用例扩充边界检查 |
| W81 定向验证 | 1 case/73 assertions 通过，主套件中也无该回归 |
| HIR 生命周期 | unfinished/published/construction/finish-retry × 两种 IR_FAST，共 8 组合通过；各模式 checks 与邻接参照一致，construction 每模式注入 5 次失败、finish-retry 每模式 2 次 |
| 入口持久化 | disk/aot/reject/native/aot-links/native-links 共 950 checks 通过 |
| 关闭回收 | owners/aot/symbols-child-node/resource-cycles 共 133 checks 通过，含实际安装、子入口清除和 6 轮资源复用；不代表整个 shutdown 模式集合均重新执行 |
| RV 兼容性 | RV 后端目标完成 Android/ARM64 构建；额外 8 源文件在最终头文件下通过语法检查，证据为 range-owner-v1-syntax-summary，最终版相关输入哈希相同。ARM 手机未执行 RV64 原生用例 |
| Guest 对照 | CoreMark 与 bench 在默认/bound-off 下共 8 次运行成功，启用 SVM_VERIFY_PLAN=1；CoreMark CRC=0x4983、bench 的 5 个 checksum 正确且一致，共同 PC 的 IR 数/host 长度与 spill/load/store 均无退化 |

默认 CoreMark 参照 424 条可见记录、候选 425，多出的仍是已经归因的 __dtoa 舍入路径 `0x25fb00/40/188`；本次不把默认 CoreMark 的整份形态日志写为相等。bound-off CoreMark 两侧各 425，完整形态日志相等；bench 两模式各 22，也分别完全相等。两模式 CoreMark 的 spill/load/store 均为 `330/102/73`；bench 为 `161/245/150`，host bytes 分别 `19908/19916`。

数据布局与邻接参照相同：AddressNode 64、Block 256、Function 136、HIRBlock 152、HIRFunction 256、HIR Edge 48、Inst 94 字节，Inst arena 槽仍为 96。退役字段被对齐空间吸收，本批不宣称对象尺寸减少。实际 pool capacity 也相同：CoreMark 632320，bench 默认 519360、bound-off 538560；Inst arena 总量仍为 CoreMark 458752、bench 262144。

本批集中采集 12 个 PMU 点：默认两轮交错采样，bound-off 一轮，每轮两版本各执行 2000/12000 次；全部退出码、CRC 正确，无 affinity 警告。

| 候选相对 cfg-boundary-v1 | 默认两轮中位数（范围） | bound-off 一轮 |
| --- | ---: | ---: |
| instructions 稳态斜率 | −0.0087%（−0.0109%～−0.0066%） | −0.1187% |
| 启动/编译相关 instructions 截距 | +0.5413%（+0.3271%～+0.7555%） | +0.7926% |
| cpu-cycles 稳态斜率 | −0.0591%（−0.2459%～+0.1277%） | +1.1671% |
| wall time 稳态斜率 | −0.1174%（−0.8987%～+0.6638%） | +0.7766% |

CPU4、taskset 10、governor=walt；逐点当前频率 1056000～1171200 kHz，上限 1171200 kHz。具名温度 cpuss-0 为 30.1～34.0°C、cpu1-0 为 30.9～33.2°C、cpu1-6 为 28.9～31.3°C，不将其视为独占 CPU4 温度。本批未见明显稳态指令退化，启动/编译相关截距有小幅增加；这组样本不构成性能收益结论。截距含启动、编译和非线性，不能当作纯编译耗时；原始 stat 无逐事件 running 比例，不新增 IPC 结论。保留 CoreMark 低于 10 秒的提示，只使用固定工作量和正确 CRC，不报告正式分数。固定源码 Android FEX 对照仍待补齐，本批未重新采样 FEX。

最终快照相对 HEAD 包含 360 个现存修改/新增源文件、38 个已删除路径。source digest 为 `b7767ed10cf5e35f6f3e7b29ae6e85dc8ec05cc6d8a1461016879a7d6f099e13`，源码 overlay archive SHA-256 为 `badb464335984ab0e10aadd18366d93a6939b90c6e7c65e67ef33206a724f7b9`。候选 `svm_range_owner_v2` 的 SHA-256 为 `34de56195892e0bac206f7c25daeab4d269753609740f955f427e818c54aaef8`，参照 `svm_cfg_boundary_v1` 为 `46b670ed2f6d5e4624b5f307ad9dfe18c77a3c7712fb4a4d004805cad2d0de4a`。验证后源码、主机产物、设备二进制和 guest 哈希均已复核。

原始日志、JUnit、命令、设备条件、布局和汇总保存在 `/private/tmp/swiftvm-middle-results/r1-range-owner-v2-*`，设备原始结果在 `/data/local/tmp/svm-middle-r1/results`；首版失败证据保留在 range-owner-v1 的对应文件。CompileUnit 仍借用 HIR pool 槽位，HIR 的 Function 强引用不等同于完整 PreparedUnit 存储持有权；这部分与 R2/R3/R5/R6/R7 的剩余工作继续接续。

## R1 HIR 存储会话与实际父对象持有权

本批验证版本为 `r1-storage-owner-v1`，邻接参照为 `svm_range_owner_v2`。已封口的 CompileUnit 不再借用 builder 的 HIR pool；这项交接覆盖至分配和发射结束，完整 PreparedUnit/compiler API 与未封口阶段的持有/借用约束仍需接续。

| 文件或接缝 | 当前职责与变化 |
| --- | --- |
| `ir/hir_storage.{h,cpp}` | 新增 8 字节 HIRPoolLease 接口，按 pool 会话引用计数；builder 和封口结果各自持有会话。线程缓存每线程一个控制对象、至多一个空闲 pool，源线程退出后最后一个消费者仍可安全回收；不为每个单元新增 shared_ptr 控制块 |
| `ir/hir_pools.h`、`common/object_pool.h` | 将 pool 布局从 hir_builder.h 移到内部头文件；析构显式先销毁 HIRFunction 再销毁 HIRBlock，避免前者读取已回收的 synthetic entry。新增仅用于析构的无分配 DestroyContents，缓存容量整理遇到 bad_alloc 时放弃缓存并直接回收 |
| `compile/unit.{h,cpp}` | SealIR 同时接续 pool 存储和实际 canonical Function 引用；独立 Block 接口接收 IntrusivePtr，删除 raw Block* 封口入口。移动结果转移存储、父对象和封口责任；释放时先解除封口/提升视图，再释放 pool 和实际父对象 |
| `ir/block.h`、`function.cpp` | Block 记录非 owning 实际父 Function；添加、移除、转移和析构维护该身份。TakeBlocksFrom 在任何修改前拒绝重复地址，只转移不相交的真实成员，不交换各 HIR pool 的占位对象 |
| `ir/hir_builder.{h,cpp}` | 非主入口、无指令且无 terminal 的占位块继续由 HIR pool 持有；真实块及主入口转交持久 Function。HIR 封口检查真实成员父对象；builder 禁止复制，析构先清空自身 roster 再放下会话 |
| 现有生命周期与契约测试 | 沿用 swift_hir_lifetime_test 增加 retained、thread-exit、region-owner 模式，覆盖 builder 先退出、存储不提前复用、源线程退出后实际分配/发射、跨 pool 合并后其他 region 先释放、独立 Block 最后引用，以及缓存维护失败回收。低层原始 stack pool 不允许承载逃逸 CompileUnit |

其中 thread-exit 在生产线程结束后，实际调用 AllocateFunction、CompleteARM64UnitForEmission 与 JitTranslator 并检查非空代码；存活对象不是只检查一个悬空指针值。region-owner 分别持有两个 builder/pool，合并实际成员后先结束 builder、再释放第一个结果，验证剩余结果和 canonical 父对象仍存活。大量 slab/free-list 比较用于寿命见证，不把其累计 checks 当作覆盖率指标。没有新增临时 probe 或测试脚本。

红魔 NX729J（`bf1359ff`）上的结果：

| 验证 | 实际结果 |
| --- | --- |
| Android 构建 | 9 个目标通过，最终增量检查无任务；额外 8 个 RV 源文件在最终头文件下通过语法检查，ARM 手机未执行 RV64 原生用例；git diff --check 通过 |
| 主套件 | 709 条记录，用例名集合和失败语义与 range-owner-v2 一致，只忽略源码行号；保留原有 12 条失败记录、67 个失败断言、0 errors。本次断言总数 220933，不作为固定覆盖率要求 |
| IR 契约 | 默认与 SVM_IR_FAST=0 各 25 cases/486 assertions 全部通过；W81 定向 1 case/112 assertions 通过 |
| HIR 生命周期 | 原有 4 模式和新增 3 模式 × 两种 IR_FAST，共 14 组合通过；retained 每模式注入 1 次缓存维护失败，construction 每模式 5 次、finish-retry 每模式 2 次 |
| 持久化与回收 | 入口 6 模式/950 checks、选取的 shutdown 4 模式/133 checks、调用持久化 6 模式/233 顶层 checks 均通过；调用子进程还有独立 phase 断言，不将它们重复计入顶层总数 |
| 静态 latch | 新纳入的 call/internal-call/aot-internal-call/disk-internal-call 均失败于旧的固定指令位置断言。为区分来源，另从原始 HEAD 构建对应目标；4 模式的失败、halt/PC/单帧/fault 见证与候选一致，只归一化进程号和 host 地址。该集合没有判为通过；disk 子进程失败也如实保留 |
| Guest 对照 | CoreMark/bench 在默认与 bound-off 共 8 次运行退出成功，启用 SVM_VERIFY_PLAN=1；CRC=0x4983、bench 的 5 个 checksum 正确且一致。bench 两模式完整形态分别一致；CoreMark 有下述块集合变化，不能判为逐 PC 完全一致 |

CoreMark 默认可见记录为参照 424、候选 425，bound-off 为参照 425、候选 424。两个共同 PC 的逐次记录发生变化：matrix_test 的 `0x21ab30` 有两次从 `233/600`（IR/host bytes）变为 `192/396`；__dtoa 的 `0x25f61f` 从 `1091/5820` 变为 `1098/5844`。另有计时文本路径的 `0x25fb00` 集合变化。为追踪这些差异，仅补充两次使用现有 SVM_DUMP_IR/SVM_DUMP_IR_POST 的诊断运行，没有写入探针代码。

诊断显示 matrix_test 两次重编译少纳入 `0x21b868` 尾块，__dtoa 的集合在 `0x2601f2` 与 `0x25fbbc` 间变化；这些差异在 decode 后已经出现。旧持久 Function 包含无代码占位成员，非 CodeLifetime 路径的 ClearDispatchSlots 退役时遍历全部成员，会同时清除该地址在其他已发布 allocation 中的分派槽；本批占位块留在 HIR 后，该成员枚举行为随之改变。这条源码路径解释了额外解码/重编译集合变化，不能把它写成对同一 IR 的生成质量收益。其他共同 PC 逐次 IR 数和 host 长度一致；两模式 CoreMark 的 spill/load/store 仍为 `330/102/73`，bench 仍为 `161/245/150`。CoreMark 的 shape host bytes 默认 `287080→286788`、bound-off `287272→286700`；bench 为 `19908/19916`。后续 R6 仍应把发布入口与依赖/成员所有权的退役范围明确分离。

数据布局中 AddressNode 64、Block 256、Function 136、HIRBlock 152、HIRFunction 256、Edge 48、Inst 94 字节及 Inst arena 槽 96 字节均未增加；通过利用 Block 既有对齐空间容纳父对象身份。CompileUnit 因存储和实际父引用从 `40→56` 字节，HIRPoolLease 从 `16→8` 字节。实际 pool capacity 与参照相同：CoreMark 632320，bench 默认 519360、bound-off 538560；Inst arena 为 CoreMark 458752、bench 262144。单元级控制对象增量如实计入，不宣称全部内存减少。

本批集中采集 12 个 PMU 点：默认两轮交错顺序、bound-off 一轮，每轮两版本各执行 2000/12000 次；退出码、CRC 全部正确，无 affinity 警告。

| 候选相对 range-owner-v2 | 默认两轮中位数（范围） | bound-off 一轮 |
| --- | ---: | ---: |
| instructions 稳态斜率 | −0.0168%（−0.0213%～−0.0124%） | +0.1254% |
| 启动/编译相关 instructions 截距 | −0.0305%（−0.0464%～−0.0146%） | −0.1791% |
| cpu-cycles 稳态斜率 | −0.9576%（−1.1852%～−0.7299%） | −1.5563% |
| wall time 稳态斜率 | −1.8081%（−2.8125%～−0.8038%） | −3.3063% |

CPU4、taskset 10、governor=walt；逐点当前频率与上限均为 1171200 kHz。具名传感器 cpuss-0 为 30.5～33.2°C、cpu1-0 为 31.3～33.2°C、cpu1-6 为 29.3～31.3°C，不视为独占 CPU4 温度。样本未见明显稳态指令成本退化，不构成正式收益结论；截距包含启动、编译和非线性，原始 stat 无逐事件 running 比例，不新增 IPC 结论。CoreMark 低于 10 秒的提示仍保留，只用固定工作量和正确 CRC。固定源码 Android FEX 构建仍待补齐，本批没有重新采样 FEX。

源码快照相对 HEAD 含 365 个现存修改/新增源文件、38 个已删除路径，source digest 为 `ab713ad7df1e1750bba0ef27c954fb57402bce21e6124cd7a18bff895ec2c003`，overlay archive SHA-256 为 `4061c6917af22ae56bd8268dc6c7bb72a3887c017c6914569d6773b5cc98b9a9`。候选 launcher `svm_storage_owner_v1` SHA-256 为 `9628774ef68aff815798fc1f09de3facc473e6031b4cb5592e3387d0d2bd67b3`；参照 `svm_range_owner_v2` 为 `34de56195892e0bac206f7c25daeab4d269753609740f955f427e818c54aaef8`。静态 latch 原始 HEAD 参照为 `b56124a4794b0e3f29620ea401bddcb4a4d64b0a1d2ff5d3661bc647fd03f07a`。验证后源码、9 个主机二进制、8 个设备候选二进制及 guest/tool 哈希均已复核。

命令、退出码、原始日志、JUnit、形态/布局/寿命/PMU 汇总和哈希证据保存在 `/private/tmp/swiftvm-middle-results/r1-storage-owner-v1-*`；设备原始结果在 `/data/local/tmp/svm-middle-r1/results`。完整 R1/R0–R7 尚未完成，不能将封口结果持有存储等同于整个准备模型完成。

## R1 多 region 共享父对象的封口覆盖

本批版本 `r1-parent-seal-v1` 接续 storage-owner-v1，只处理封口覆盖和对应析构边界。原实现将持久 Function 的只读状态保存为 bool；合并后，secondary HIR 的 header 已无成员，主 region 先结束会解除 canonical 父对象的约束，即使 secondary 的实际 Block 仍在分配/发射作用域中。

先向现有 ir_contract_test 加入永久契约用例，仅构建并执行该用例，在修复前真实复现 `release_primary_first=true` 时 `owner->IsIRReadOnly()` 为 false；red 二进制和 JUnit 已保留。随后完成以下修改：

- 持久 Function 用一个原子 region 计数代替 bool。每个封口 HIR 同时约束自己的 header 和实际父对象，最后一个消费者退出后才恢复父对象可写；移动 CompileUnit 不复制计数责任。
- 局部 HIRBlock 封口也持有实际父对象的结构约束，禁止期间通过父对象补写范围、增加成员或批量回收。
- HIRFunction 析构只检查自身 synthetic entry 的封口是否解除；另一个 region 可以继续封住共享父对象。不能为了回收已经结束的 wrapper，要求仍在使用的 canonical Function 整体可写。
- 现有生命周期 region-owner 模式在 builder 结束后，将主结果交给另一线程释放，再验证剩余结果的实际父对象仍只读且有效。契约用例覆盖两种释放顺序和最后释放后恢复结构修改。

红魔 NX729J 上集中完成直接相关验证：3 个 Android 目标构建通过、最终 Ninja 无任务；IR 默认和 IR_FAST=0 各 26 cases/517 assertions 全部通过；retained/thread-exit/region-owner × 两种 IR_FAST 共 6 组合通过。默认 CoreMark 与 bench 的 SVM_VERIFY_PLAN 运行退出、CRC/checksum 正确；bench 22 条记录及完整 shape 与 storage-owner-v1 一致，CoreMark 所有共同 PC 的逐次 IR/host 长度一致，只有已识别的 __dtoa `0x25fb00/15/92` 计时文本分支本次未执行（425→424 条记录）。spill/load/store 仍为 CoreMark `330/102/73`、bench `161/245/150`。

Function 仍为 136 字节，CompileUnit 56、HIRPoolLease 8 字节。本批没有重复整套主回归、额外 RV 测试矩阵、静态 latch 集合或 PMU，不把上批性能数字移用于新二进制。git diff --check 通过；源码和 3 个主机/设备产物哈希均复核。原子计数用于共享父对象的消费者释放，完整准备阶段与借用 API 仍待迁移。

快照包含 365 个现存修改/新增源文件和 38 个删除路径，source digest 为 `86100f60a5549adc93b2c66e26a9e2e89499bafd98c4eca0e903154bae1519a9`，archive SHA-256 为 `11d8039626176adb399e3a4ea2c2061a02fdfb44e4556551f9df7ba33dc84502`。launcher `svm_parent_seal_v1` SHA-256 为 `8a2f34d5a87b7205816b1feaa91ec72369ef7791e1637f3a41d6e74e497e7a28`。构建、red/green JUnit、运行命令、寿命见证、guest 对照、DWARF 布局与哈希保存在 `/private/tmp/swiftvm-middle-results/r1-parent-seal-*`。完整 R0–R7 仍在执行。

## R1/R6 准备入口、持有权与 owning ARM64 结果

本批版本为 `r1-compiler-owner-v1`，邻接 launcher 参照为 `svm_parent_seal_v1`。准备持有权已提前到 CompileUnit 构造，ARM64 的四类生产路径已接入新 compiler，并删除 runtime 的重复准备结构；最终 Allocation、公共 facts 和统一 actions 仍需接续。

| 文件或接缝 | 本批职责与变化 |
| --- | --- |
| `compile/unit.{h,cpp}` | Block/最终 HIRBlock/Function 强类型构造即接续存储和实际父对象，删除默认构造及带输入的 SealIR。准备阶段通过 EditBlock/EditFunction 修改自身输入，封口后只读；提升 metadata 校验所属单元。PrepareFunctionGuestRanges 从 runtime 移入公共层，解释执行发布仍复用它 |
| `ir/function.{h,cpp}`、`ir/instr.h` | 准备期间也禁止无协调的添加、移除、转移或批量回收成员；Block 利用 instruction owner 的空隙记录已绑定状态。准备计数与只读消费者计数分开，不把可变优化准备误判为只读 IR |
| `CompileUnit::MergeFunctions` | 验证整组重复身份、地址冲突和准备阶段，所有检查通过后才转移成员；intrusive 转移阶段无分配，再同步各单元的 canonical 父引用和阶段责任。失败不会先移走前几个 region，不引入新的通用回滚框架 |
| `backend/arm64/compile/compiler.{h,cpp}` | 新增 PrepareBlock、PrepareFinalBlock、PrepareFunction、PrepareFunctions；统一语义准备、最终编号、封口、分配、必要 target 契约和 admission，拒绝时不返回 ready 结果。最终 HIRBlock 保留单独的编号/局部值契约，不重跑普通 Block 优化 |
| `Arm64PreparedUnit` | 仅成功工厂可构造，稳定存储拥有 CompileUnit、RegAlloc 和 target 结果；只导出 const 查询。不可复制，移动不搬动分配器和其内部视图；分配结果先析构，随后才解除 IR/anchors/pool 持有权 |
| `backend/runtime.cpp` | 删除 PreparedFunctionRegion、FunctionRegionAllocation、PrepareFunctionRegion、AllocateFunctionRegion、重复支持检查和 ResolveBackendFeatures；约 256 行准备逻辑迁出。保留执行模式选择、prepared 编码、布局回退、发布与消费者结束后的 IR 回收 |
| `jit/function_code_object_emitter.{h,cpp}` | 只接收 owning prepared 结果，删除公开的 FunctionRegionEmission 裸配对；内部保留 const Function/RegAlloc 视图，仍在建立 encoder 和写出第一字节前检查全部 region。移动 owning 结果保持这些内部对象地址稳定，使用者仍须保持其持有者存活至消费结束 |
| 既有契约与测试支持 | code-object 用例改走真实 compiler；未完成第二个 region 时准备入口拒绝，保留范围 writer 在封口时即拒绝。损坏 pair 事实的负向用例经测试内部显式 fixture 取得 mutable allocation，继续证明必要契约失配时零字节拒绝；生产接口没有增加可写出口 |

现有 swift_hir_lifetime_test 新增 preparing 与 prepared 模式，分别验证：源 builder/线程退出后仍可完成未封口输入的编号、封口、分配和发射；以及 compiler 的 ready 结果移动后，在源 builder/线程均结束的条件下通过真实 FunctionCodeObjectEmitter 发射非空代码。原有跨 pool 合并、另一线程先释放主 region、独立 Block、构建/收尾失败和缓存维护失败用例继续运行。没有新增临时 probe 或测试脚本。

本批先完成代码与调用方迁移，再集中验证。第一次构建暴露 target.h 缺少直接 Block 声明，修正头文件依赖后，9 个 Android 目标全部构建通过，最终 Ninja 无任务；额外 8 个 RV 源文件按最终输入通过语法检查。ARM 手机未运行 RV64 原生测试。

红魔 NX729J（`bf1359ff`）上的结果：

| 验证 | 实际结果 |
| --- | --- |
| 主套件 | 709 条记录，仍为既有 12 条失败记录、67 个失败断言、0 errors；与最近完整 storage-owner-v1 主套件相比，失败语义完全一致，仅忽略源码行号。本次总断言 221243，不视为固定覆盖率要求 |
| 用例名称变化 | 两项按新接口改写为“全部 region 完成后才能取得 ready 对象”及“prepared region 拒绝保留的范围 writer”；旧的对应名称退役。不是在 case-name 集合不同的情况下声称集合完全相同 |
| IR 契约 | 默认/IR_FAST=0 各 27 cases、540 assertions，全部通过；覆盖重复绑定、foreign metadata、整组合并冲突保持原状、重复单元拒绝、阶段 writer 和两种释放顺序 |
| HIR 生命周期 | 9 模式 × 两种 IR_FAST，共 18 组合通过；retained 各注入 1 次缓存维护失败，construction 各 5 次、finish-retry 各 2 次，原有失败注入继续有效 |
| 持久化与回收 | 入口 6 模式/950 checks、选取的关闭回收 4 模式/133 checks、调用持久化 6 模式/233 顶层 checks 均通过；不把子进程 phase checks 重复计入顶层总数 |
| 静态 latch | 本批选取的 4 个 call 模式仍触发既有固定位置断言；与原始 HEAD 参照的失败及运行时见证一致，仅归一化 host 地址与进程号。该集合没有判为通过 |
| Guest 对照 | CoreMark/bench 默认与 bound-off 共 8 次运行成功，SVM_VERIFY_PLAN=1；CRC=0x4983，5 个 bench checksum 正确且一致。四组的完整单元 IR 数/host 长度及全部 shape 记录分别一致，没有新增共同 PC 差异 |

CoreMark 可见记录默认两侧各 425，bound-off 两侧各 424；对应 shape host bytes 为 `286788/286700`，spill/load/store 均为 `330/102/73`。bench 两模式各 22 条记录，host bytes 为 `19908/19916`，spill/load/store 为 `161/245/150`。实际 HIR pool capacity 仍为 CoreMark 632320、bench 默认 519360/关闭 bound decode 538560；Inst arena 为 458752/262144。

对象布局代价单独记录：Function 因新增准备所有权计数从 `136→144` 字节；CompileUnit 利用 union/标记仍为 56 字节，Block 仍为 256、Inst 94/槽 96、HIRBlock 152、HIRFunction 256、IR Edge 48、HIRPoolLease 8 字节。Arm64PreparedUnit 的公开持有对象为 8 字节，但内部稳定存储确实需要一次堆分配；不能把它写成每单元仅有 8 字节成本。HIR pool/arena 数字不包含所有持久对象及 allocator 开销，本批不声称总体内存无增加。

集中采集 12 个 PMU 点：默认两轮交错顺序、bound-off 一轮，两版本各执行 2000/12000 次；退出码、CRC 全部正确，无 affinity 警告。

| 候选相对 parent-seal-v1 | 默认两轮中位数（范围） | bound-off 一轮 |
| --- | ---: | ---: |
| instructions 稳态斜率 | +0.0162%（+0.0035%～+0.0289%） | +0.0231% |
| 启动/编译相关 instructions 截距 | +0.0109%（−0.0082%～+0.0301%） | +0.0414% |
| cpu-cycles 稳态斜率 | +0.9197%（+0.5512%～+1.2882%） | +1.2978% |
| wall time 稳态斜率 | +1.7032%（+1.6879%～+1.7185%） | +1.3049% |

CPU4、taskset 10、governor=walt；逐点当前频率和上限均为 1171200 kHz。具名传感器 cpuss-0 为 29.7～33.2°C、cpu1-0 为 30.5～33.2°C、cpu1-6 为 28.9～31.3°C，不视为独占 CPU4 温度。未见明显指令成本退化，周期与时长斜率存在上述小幅增加；这组诊断样本不构成性能收益结论。两点截距含启动、编译和非线性，不是纯编译器耗时；细分 codegen 计时现由 compiler 完成部分和 runtime 编码部分分别累计。原始 stat 无逐事件 running 比例，不新增 IPC 结论；CoreMark 保留低于 10 秒的提示，不报告正式分数。固定源码 Android FEX 构建仍待补齐，本批未重新采样 FEX。

最终快照含相对 HEAD 的 368 个现存修改/新增源文件、38 个删除路径，source digest 为 `3d18b9518a6526427fbdb1038358618d1e79d3817ff4a2ac8ec5f7bdc679d8fb`，archive SHA-256 为 `522f1f68ae69869583a3813e5ac375cd166a50deb8cb3d8e2815b6aadf0c0a1c`。候选 `svm_compiler_owner_v1` SHA-256 为 `b889f97cfd40951635584b75b5556f1256ce442acc1ec1fd3d7d700741b793fb`；参照 `svm_parent_seal_v1` 为 `8a2f34d5a87b7205816b1feaa91ec72369ef7791e1637f3a41d6e74e497e7a28`。源文件、9 个主机产物、8 个设备候选、参照、guest 与工具哈希均已复核；git diff --check 通过。

构建、命令、JUnit、原始日志、设备条件、布局、寿命/持久化/形态/PMU 汇总和哈希在 `/private/tmp/swiftvm-middle-results/r1-compiler-owner-v1-*`，设备原始结果仍在 `/data/local/tmp/svm-middle-r1/results`。这完成了准备结果持有权与 ARM64 compiler 接缝，不能据此宣称 R1/R3/R5/R6 或完整 R0–R7 全部完成。

## R3/R5 物理 AllocationView 与独立 ARM64 动作

本批将物理数据与目标动作分开，并直接迁移生产发射接口，没有保留旧 JitContext 构造重载或复制一份 mapping。

- `compile/allocate/allocation.h` 拥有 mapping、live masks、reload、fixed/transfer/preservation 位置及公共类型；`AllocationView` 是 `const Allocation&`。47 个现有只读实现迁入 `locations.cpp`，该文件没有 ARM64 plan、encoder 或惰性分析缓存。
- `backend/arm64/compile/actions.{h,cpp}` 独立保存块动作和证明。width、low32、host read、zero store、publication elision 共用稠密字节标记，删除独立 publication 哈希集合；recolor 是否已产生不可再修改的证明仍为常数时间查询。块动作内部仍是旧 GPRBlockPlan 子 plan/map/shared_ptr，本批没有将其包装宣称为最终轻量 payload。
- `UnitActions::MatchesAllocation` 同时检查实际物理结果地址、冻结状态、generation 和 attempt。相同 IR 的两个分配可以有相同代际和物理位置，仍不能交换动作。本批在已有 admission 用例中加入这一反例，要求拒绝且输出零字节。
- `register_contract.{h,cpp}` 接管旧 RegAlloc 文件中的寄存器池、fixed home/clobber、scratch 和 spill 容量规则。RegAlloc 仍是可变准备 builder，继承唯一的物理存储并持有分析/区间缓存、事务、请求状态和 UnitActions；冻结输入、预制约束和 builder 的最终退役继续接续。
- Arm64PreparedUnit 分别提供 const Allocation 和 const UnitActions，JitContext 只消费这两个结果；runtime、函数 code-object emitter 和现有测试的构造均已接入。FrozenGPRPlan 也分别借用物理视图与动作，prepared 的移动继续保持地址稳定。
- 旧的 EditPreparedAllocation 测试逃生口已删除。缺失 pair facts 的既有反例改用测试目标中的定向动作替换夹具；生产 StoreGPRPlan 增加准备阶段及未冻结检查，不再通过生产写入口破坏已冻结动作。没有新增测试源文件或临时 probe/脚本。

三个旧文件的文本行数为 reg_alloc.h `509→115`、reg_alloc.cpp `1106→287`、reg_alloc_gpr_locations.cpp `445→288`。这些数值包含迁出的实现，不能当作相同行数的死代码删除量。

集中验证使用红魔 NX729J（`bf1359ff`）及既有 NDK 29/API 28/Release 参数：

- 9 个 Android 目标构建通过，最终 Ninja 无任务；额外 8 个 RV 源文件 syntax-only 全部通过。RV 原生执行没有在 ARM64 手机上进行。
- 主回归 709 条 case 记录，12 个失败用例、67 个失败断言、0 errors；case 名称的 multiset 和失败内容均与 compiler-owner-v1 一致，只归一化源码 .cpp/.h 行号。JUnit 总断言 221069 仅记录本批运行，不作为跨批覆盖率相等的依据。
- IR 默认和 IR_FAST=0 各 27 cases/540 assertions 通过。
- retained、thread-exit、region-owner、preparing、prepared × 两种 IR_FAST，共 10 个持有权组合通过。entry 6 模式 950 checks、call 的 static/indirect/aot-static/aot-indirect 4 模式 154 个顶层 checks、shutdown 的 owners/resource-cycles 2 模式 105 checks 通过；call 子进程 phase 检查单独保留在日志。
- 上述共 25 次执行。没有重复完整的 lifetime/shutdown 矩阵或 static-latch 失败集合；此前已与原始 HEAD 核对的 static-latch 失败不据此标为修复。

两版相邻 launcher 以 SVM_VERIFY_PLAN=1，集中运行默认/bound-off 的 CoreMark 与 bench，共 8 次，退出、CRC 和 5 项 bench checksum 全部正确。默认 CoreMark 的 424 条完整有序 PC/IR/host 记录与完整 shape 相同，shape host bytes 均为 286696；两种 bench 的 22 条完整记录和 shape 也相同，host bytes 为 19908/19916。

bound-off CoreMark 参照 425 条、候选 424 条，候选本次没有执行既有的 __dtoa `0x25fb00/15/92` 计时文本分支；所有共同 PC 的逐次 IR 数和代码长度一致。shape host bytes 为 `286792→286700`，差额正好是该分支的 92 字节，不能归因为相同 IR 的生成质量收益。两模式 CoreMark spill defs/loads/stores 均为 `330/102/73`，bench 均为 `161/245/150`。

HIR pool bytes 在两版间一致：CoreMark 632320，bench 默认 519360、bound-off 538560；Inst arena 分别为 458752、262144。

本批只采一轮默认 CoreMark 的四点交错 PMU：参照 2000、候选 2000、候选 12000、参照 12000；全部 CRC、退出码正确，没有 affinity 警告。

| 候选相对 compiler-owner-v1 | 稳态斜率变化 | 两点截距变化 |
| --- | ---: | ---: |
| instructions | -0.023444% | -0.201614% |
| cpu-cycles | -2.483052% | +2.714161% |
| wall time | -1.862482% | +1.867914% |

CPU4、taskset 10、governor=walt；逐点当前频率 1056000～1171200 kHz，上限 1171200 kHz。具名传感器 cpuss-0 为 29.3～32.8°C、cpu1-0 为 30.9～32.4°C、cpu1-6 为 28.9～30.9°C，不当作独占 CPU4 温度。单轮诊断与频率变化不支持正式收益结论；两点截距包含启动、编译和非线性，不是纯编译器耗时。原始 stat 未提供逐事件 running 比例，不新增 IPC 结论；保留 CoreMark 低于 10 秒的提示，不报告正式分数。本批未重新采样 FEX，固定源码 Android FEX 构建仍待补齐。

DWARF 显示 RegAlloc `3304→3280` 字节；新公共 Allocation 为 816 字节、UnitActions 为 88 字节，均已包含在上述 RegAlloc 大小内，不能相加重复计算。FrozenGPRPlan `304→312` 字节，增加独立动作借用引用。AddressNode 64、Block 256、Inst 94（arena slot 96）、Function 144、HIRBlock 152、HIRFunction 256、CompileUnit 56、HIRPoolLease 8、Arm64PreparedUnit wrapper 8 字节均未变化；wrapper 仍有独立稳定 heap storage。另一个名为 Allocation 的 40 字节类型来自 fault_catalog.h，与公共物理结果无关，布局报告按声明文件区分。

快照包含 375 个现存修改/新增源文件和 38 个删除路径，source digest 为 `c1b52532dea20df2ff29d44fc32bed386f7d5e71ee65e7418a9d8c7c65978e81`，archive SHA-256 为 `7643f9258ba2bb35d8406c42d48f65de2b1387c370ca8bd6a59ba1db8297a0ce`。launcher `svm_allocation_view_v1` SHA-256 为 `6e68badd56111a5762c7f11fb53d3024d89e5acdb9adb1390ab24c70105ad320`；相邻参照 `svm_compiler_owner_v1` 为 `b889f97cfd40951635584b75b5556f1256ce442acc1ec1fd3d7d700741b793fb`。

构建、源码/二进制身份、命令、JUnit、寿命/持久化/形态/PMU 汇总及具名设备条件保存在 `/private/tmp/swiftvm-middle-results/r3-allocation-view-v1-*`；设备原始日志仍在 `/data/local/tmp/svm-middle-r1/results`。主机 9 个产物、设备 8 个候选、参照/guest/tool、源码和 archive 已复核。该批完成物理视图与动作接口分离，完整 R1/R3/R5/R6/R7 和 R0–R7 仍在执行。

## R3 分配输出与准备 writer 的寿命分离（2026-10-22）

本批不再让 prepared 结果持有 RegAlloc。输出由独立的 Allocation 和 UnitActions 拥有，writer、扫描缓存和控制器状态在进入发射前结束；单元 IR 与提升 anchors 仍由 CompileUnit 接续持有。

- `AllocationTypes` 只定义一次物理类型；Allocation 拥有唯一数据表，AllocationView 保存一个指向它的指针，所有查询在该只读视图上实现。视图复制不复制数据，也不取得准备写权限；旧的 const Allocation 引用别名已被真实借用值取代。
- RegAlloc 继承只读查询能力并借用外部 Allocation/UnitActions，写操作直接修改这唯一的输出。每个结果只接受一个活动 writer，冻结输出拒绝重新打开；writer 析构释放自己的分析、事务和范围缓存。普通分配夹具保留一个拥有输出的便利构造，但生产 ARM64 只使用借用构造。
- Arm64PreparedUnit 的 Storage 只包含 CompileUnit、optional Allocation、optional UnitActions。Block、最终 HIRBlock、Function 的 writer 均为准备方法中的栈对象，返回结果前已经销毁。多 region 先全部优化、合并和封口，然后逐 region 分配、完成目标动作、释放 writer；全部成功后才交给 code-object emitter。
- FrozenGPRPlan、JitContext 和 code-object 内部 region 持有轻量物理视图，UnitActions 以底层 Allocation 的实际地址校验身份。writer 栈地址被后续 region 复用不会使两个结果拥有相同身份，移动 prepared wrapper 也不搬移数据。
- 删除 RegAlloc::CompleteGPRAllocation、公共 CompleteAllocation/PrepareAllocationTargets 及 `backend/arm64/alloc/gpr_allocation_plan.cpp` 和 CMake 条目；141 处普通调用与 1 处解引用调用直接接入 ARM64 compiler。AllocationPreparationResult 也移出公共分配头。目标 admission 和 FPR retry 循环归 compiler，公共 AllocationController 只提供 RetryWithoutFPRCoalescing 和 Finish，不再接受 Module 或反向启动后端。
- 在现有 region_edges_test 中增加一个真实生命周期用例：writer 销毁后继续验证映射并发射代码，同时检查重复 writer 与冻结后重新打开均被拒绝。没有新增测试源文件、临时 probe 或测试脚本。

相邻参照为 allocation-view-v1，本批集中构建和验证仍使用红魔 NX729J（`bf1359ff`）：

- 9 个 Android 目标一次构建通过，最终 Ninja 无任务；额外 8 个 RV 源文件 syntax-only 全部通过。RV 原生执行未在该 ARM64 设备进行。
- 主回归 710 条 case 记录，比参照新增上述生命周期用例，无删除或重命名。仍为同一组 12 个失败用例、67 个失败断言、0 errors；只归一化源码 .cpp/.h 行号后，失败内容完全一致。JUnit 总断言 221130 仅作为本批记录。
- IR 默认和 IR_FAST=0 各 27 cases/540 assertions 通过。
- lifetime 全部 9 模式 × 两种 IR_FAST，共 18 组合通过；construction 每模式注入 5 次、finish-retry 2 次、retained 的 cache-maintenance 1 次失败均通过。entry 6 模式 950 checks、call 6 模式 233 个顶层 checks、shutdown 的 owners/resource-cycles 2 模式 105 checks 通过。以上共 35 次执行；未重复 static-latch 已知失败集合或完整 shutdown 矩阵。

为分开核对 Function 与普通 Block，使用默认、SVM_BOUND_DECODE=0、SVM_FUNC_BASE=0 三种配置，各运行两版本的 CoreMark 与 bench，共 12 次。后一个开关经源码路径核对：function_enabled 为 false，跳过 Function 路径并以 Block 节点进入 CompileBlock。SVM_VERIFY_PLAN 全程开启，所有退出、CoreMark CRC 与 5 项 bench checksum 正确。

| 配置/负载 | 本批对照结果 |
| --- | --- |
| 默认 Function CoreMark | 参照 424、候选 425 条记录；全部共同 PC 的逐次 IR 数/host 长度一致。候选多执行既有 __dtoa `0x25fb00/15/92` 分支，shape host bytes `286696→286788` |
| bound-off Function CoreMark | 两版 425 条完整有序 PC/IR/host 记录与完整 shape 均相同，shape host bytes 286792 |
| 两种 Function bench | 两版均 22 条完整记录及相同 shape，host bytes 为 19908/19916 |
| 普通 Block CoreMark 首轮 | shape units `2417→2418`、host bytes `206104→206172`，spill 统计相同；该路径原有 SVM_PROF 未输出逐 PC 记录，不能把空数组相等视为验证通过，也不能从该轮直接定位 68 字节差额 |
| 普通 Block bench 首轮 | 完整 shape 相同，units 101、host bytes 16468；逐 PC 记录同样缺失，另行补齐下述 host dump |

Block 路径原有 block_units 计数也未接入，日志中该项为零，不能作为实际编译了零个 Block 的证据。本批使用已有 SVM_VIXL_HOST_DUMP 补充两版本的 Block CoreMark/bench，共 4 次。CoreMark 分别 2413/2415 条 host 记录，所有共同 PC 的逐次 host 长度一致；候选额外 7 个地址、缺少 5 个地址，均由 guest ELF 的 llvm-nm 起始地址和符号大小核对，位于 __dtoa 或 __d2b_D2A 内。该补采启用逐字节日志，时间及文本格式化分支会变化，只作结构诊断。Block bench 两版 101 条完整有序 PC/size 记录、完整 shape 和 checksum 相同。

未开启 host dump 的 Function/Block spill 统计分别核对：两模式 Function CoreMark defs/loads/stores 为 `330/102/73`，Function bench 为 `161/245/150`；普通 Block CoreMark 为 `771/360/261`，Block bench 为 `188/262/162`，两版本均相同。Block 的 host dump 补采也保持相同 spill 统计。上述 CoreMark roster 差异不作为相同 IR 的生成质量变化，不使用日志开启时的耗时与 PMU 混比。

两版 HIR pool/Inst arena 在对应模式相同：Function CoreMark `632320/458752`，Function bench 默认 `519360/262144`、bound-off `538560/262144`；普通 Block CoreMark 为 `0/196608`，Block bench `0/65536`。这些是各自池/arena 的统计，不包含全部分析缓存和 allocator 开销。

默认 Function 与普通 Block 各采一轮四点交错 PMU：参照 2000、候选 2000、候选 12000、参照 12000，共 8 点。profiling、audit、host dump 均未在这些 PMU 命令中开启；全部退出、CRC 正确，无 affinity 警告。

| 模式/指标 | 稳态斜率变化 | 两点截距变化 |
| --- | ---: | ---: |
| Function instructions | +0.128023% | +0.230018% |
| Function cpu-cycles | +0.820258% | -0.480941% |
| Function wall time | -1.813960% | +2.844897% |
| Block instructions | +0.057225% | +0.062782% |
| Block cpu-cycles | -0.540215% | +2.826353% |
| Block wall time | -0.264638% | +0.056925% |

CPU4、taskset 10、governor=walt；逐点当前频率 1056000～1171200 kHz，上限 1171200 kHz。具名温度 cpuss-0 为 29.3～32.8°C、cpu1-0 为 30.1～32.8°C、cpu1-6 为 28.9～30.9°C，不视为独占 CPU4 温度。这是单轮诊断，不能据此确认收益或把小幅指令增加归因为某项实现；两点截距含启动、编译和非线性，不是纯编译时间。原始 stat 无逐事件 running 比例，不增加 IPC 结论；保留 CoreMark 低于 10 秒的提示，不报告正式分数。FEX 本批未重新采样，固定源码 Android FEX 构建仍待补齐。

DWARF 按声明文件核对：compiler.cpp 中实际 prepared Storage 为 `3344→976` 字节，生产栈上 RegAlloc writer 为 2408 字节，发射阶段不再保留它。Allocation 816、AllocationView 8、UnitActions 88、FrozenGPRPlan 312、wrapper 8 字节；旧 AllocationView 为引用别名，没有独立 class 布局。便利 OwnedResults 为 904 字节，只用于独立分配客户。不能将 Storage 的下降比例解释为总峰值内存下降：准备期仍有 writer 栈、容器 backing 与其他暂存，ABI/代码缓存布局也没有据此改变。AddressNode 64、Block 256、Inst 94/slot 96、Function 144、HIRBlock 152、HIRFunction 256、CompileUnit 56、HIRPoolLease 8 均未变化。

源码快照包含 374 个现存修改/新增源文件、39 个删除路径；source digest 为 `1e81911b25320d02cb47b140cb1c820185817853bbbeb6cf2d243b0439624fd8`，archive SHA-256 为 `a5b8c7c5f781d5d62d3133df90f9229d5e5f6ff6b53202bc2cf1baf2ce523ebd`。候选 launcher `svm_result_owner_v1` SHA-256 为 `662fbe7316bdf945ba8b62ef3a24aacecbb17a59f2c1b96512ed5e6d87545098`，相邻参照 `svm_allocation_view_v1` 为 `6e68badd56111a5762c7f11fb53d3024d89e5acdb9adb1390ab24c70105ad320`。

源码、9 个主机产物、8 个设备候选、参照/guest/tool 均已复核；设备运行前后哈希一致。构建、命令、JUnit、注入失败见证、普通/host-dump 形态、guest 符号归属、PMU 条件、布局和摘要在 `/private/tmp/swiftvm-middle-results/r3-result-owner-v1-*`，设备原始日志在 `/data/local/tmp/svm-middle-r1/results`。本批完成生产结果与准备状态寿命分离、公共反向 target 回调退役，预制约束、通用编辑接口、公共 facts、完整轻量 actions 及其余 R0–R7 继续执行。

## R3 公共 writer 与 ARM64 准备接口分离（2026-10-22）

本批接续 result-owner-v1，删除旧 RegAlloc 的最后三个文件及所有 include/CMake 条目。公共层不再借助 ARM64 容器实现可变分配接口，也没有增加旧类别名或转发头。

| 位置 | 当前职责 |
| --- | --- |
| compile/allocate/writer.h、writer.cpp | AllocationWriter 借用唯一 Allocation，处理映射、dirty masks、spill/reload、局部编辑关联与写入寿命；固定 home 掩码由目标输入，构造不调整调用方寄存器池 |
| compile/allocate/writer_locations.cpp | fixed/consumer home、residence/preservation 区间、冲突缓存、编辑/冻结与物理 attempt 放弃；不读取 UnitActions |
| backend/arm64/compile/allocation_preparation.h、cpp | ARM64 AllocationPreparation 借用公共 writer 与 UnitActions；处理 XPOOL/flags/AFP 保留策略、target begin/end/abort、证明和动作提交 |
| compile/analysis/block_index.h、pseudo_flags.h、pseudo_flags.cpp、semantic_facts.h | 原纯 IR consumer/pseudo 索引及显式 PrepareUses，供 fixed 约束和 target 准备复用；旧 ARM64 对应文件退役 |
| tests/support/allocation_fixture.h | 测试夹具持有一份实际物理结果与动作；销毁准备对象后才销毁输出。生产不再提供便利 owning 构造 |

发表省略位迁入物理 Map 的现有 padding，与原 coalesced-host-write 标记分别保存；通用只读查询合并这两者，局部事务仍能判断原始物理 coalescing。稀疏发表 ID 列表只在准备 writer 内用于重置。必要省略证明提交后，公共位置编辑屏障关闭，不再向 UnitActions 查询是否存在证明。

动作的 Ready/查询按实际物理结果、frozen、generation/attempt 校验，构建期查询由明确 building 阶段限定。公共重置无需回调目标容器；新的请求即使是跳过普通扫描的空 Block，也立即使旧动作失效。ARM64 compiler 根据 pending 输入组织完成/重试，已完成请求不会因失效查询而被意外重新规划。

本批集中构建时修正了旧头文件隐式带入的策略/IR 声明，以及测试夹具的 include 路径与命名空间。最终 9 个 Android 目标全部构建通过，Ninja 无剩余任务；额外 8 个 RV 源文件 syntax-only 通过。没有在 ARM64 手机上执行 RV 原生测试，也没有新增 probe 或单次测试脚本。

红魔 NX729J（bf1359ff）的 35 次核心运行结果：

- 主套件仍为 710 个用例，12 个失败用例、67 个失败断言、0 errors，与相邻参照的失败语义一致；比较仅归一化源码行号。5 个用例名称中的 RegAlloc 改为 AllocationFixture，映射已单列记录，没有删除行为覆盖。
- IR default/IR_FAST=0 各通过 28 个用例、547 个断言；新增公共 writer 独立运行分配、保留调用方池及销毁后读取冻结结果的真实契约用例。
- lifetime 的 9 模式 × 两种 IR 模式共 18 次通过；construction/finish-retry/retained 各模式仍分别见证 5/2/1 次注入失败。
- entry 6 模式共 950 checks、call 6 模式共 233 个顶层 checks、shutdown 的 owners/resource-cycles 两个选定模式共 105 checks 通过；子进程 phase checks 未混入顶层总数。
- 静态 latch 目标构建通过，本批未重新运行其既有 4 模式失败集合，不能报告为修复或通过。

集中完成 12 次 guest 对照：默认 Function、SVM_BOUND_DECODE=0 的 Function、SVM_FUNC_BASE=0 的普通 Block，各运行 CoreMark/bench 与两版本。前一开关只关闭 bound decode；后一开关才走普通 Block。Block 四次对照直接启用已有 SVM_VIXL_HOST_DUMP，取得 PC/长度，不再额外重跑一组诊断。

Function 的所有公共 PC 均保持相同的有序 IR/host 大小。默认 CoreMark 参照 425、候选 424 条 unit 记录，仅少一次 __dtoa 内的 0x25fb00（IR 15、host 92），汇总 host bytes 286788→286696；bound-off 的 425 条完整记录及形态相同，host bytes 286792。bench 两配置各有 22 条完整记录，host bytes 分别 19908/19916，形态及五个校验值全部相同。

普通 Block 使用 host dump 的 CoreMark 记录为 2413→2416，所有公共 PC 的长度一致；候选多 5 个、少 2 个 PC，均经相同 guest ELF 的符号范围核对落在 __d2b_D2A。记录长度总和与汇总 host bytes 分别相符，为 205584→206308，差 724 来自这组实际运行 roster；不据此认定相同 IR 的生成质量改变。bench 的 101 条有序 PC/长度、16468 字节及完整形态一致。host hash/bytes 含本机地址，未用完整字节相等充当长度比较。普通 Block 的 [svm-unit] 记录仍未接线，未把空记录列表解释为零 Block 或质量相同。

两版本的 spill defs/loads/stores 一致：Function CoreMark 330/102/73、bench 161/245/150；普通 Block CoreMark 771/360/261、bench 188/262/162。HIR pool/Inst arena 也分别一致：Function CoreMark 632320/458752、bench 默认 519360/262144、bound-off 538560/262144；普通 Block CoreMark 0/196608、bench 0/65536。所有 guest CRC 与 bench 五个校验值正确。bench 输出会把日志接在 case 标签后，解析未假定 [svm-unit]/[svm-host] 总在行首。

默认 Function 与普通 Block 各采一轮四点交错 PMU，共 8 点，顺序为参照 2000、候选 2000、候选 12000、参照 12000。未在 PMU 命令启用 profiling、audit 或 host dump；所有退出、CRC 正确，无 affinity 警告。相对相邻参照的两点拟合变化为：

| 路径/指标 | 斜率变化 | 截距变化 |
| --- | ---: | ---: |
| Function instructions | -0.146548% | +0.213030% |
| Function cpu-cycles | -0.094090% | +0.062823% |
| Function wall | +1.072779% | -0.453976% |
| Block instructions | -0.005578% | +0.135665% |
| Block cpu-cycles | +0.810345% | -2.598284% |
| Block wall | +0.835152% | -3.831609% |

CPU4、taskset 10、governor=walt，上限为 1171200 kHz；逐点前后读取的当前频率为 614400～1171200 kHz，并未锁频。thermal_zone31 的 cpuss-0 为 29.7～32.8°C，zone35 的 cpu-1-0 为 30.5～33.2°C，zone41 的 cpu-1-6 为 28.9～30.9°C，不作为独占 CPU4 温度。单轮诊断不能确认收益，截距包含启动、编译和非线性，不能当作纯编译耗时。原始 stat 无逐事件 running 比例，不新增 IPC 结论；CoreMark 保留低于 10 秒的提示，不报告正式分数。本批未重新采样 FEX，固定源码 Android FEX 构建仍待补齐。

DWARF 按声明文件确认：公共 AllocationWriter 2424 字节，ARM64 AllocationPreparation 2432 字节；相邻旧 writer 为 2408 字节。Prepared Storage 976、Allocation 816、AllocationView 8、Map 16、UnitActions 88 字节保持不变。这是职责和依赖拆分，不是总峰值内存下降的证明；准备阶段还持有 writer、容器 backing 和新增稀疏发表 ID 缓冲。

源码快照包含 391 个现存修改/新增源文件、46 个删除路径，source digest 为 dfa31996f8d10631d41afe7b897a978600fab02af8ff05fb13ec76035a65fd2a，archive SHA-256 为 7a0db8a1316c0d4ce57c3731c981f63fd034481abb9c1104bdcb2f928fada19e。候选 svm_common_writer_v1 SHA-256 为 5717779b77e9d4c0bc24f804ddf3e0ddc4c3cfa82363a1f690fb911b903d5608；相邻参照 svm_result_owner_v1 为 662fbe7316bdf945ba8b62ef3a24aacecbb17a59f2c1b96512ed5e6d87545098。

源码/archive、9 个主机/构建产物、8 个设备候选及参照/guest/tool 已复核，设备运行前后哈希一致。构建、命令、JUnit、重命名映射、寿命/持久化/形态/符号/PMU/布局与哈希保存在 /private/tmp/swiftvm-middle-results/r3-common-writer-v1-*，设备原始日志在 /data/local/tmp/svm-middle-r1/results。

本批完成公共 writer/局部事务的类型边界和旧 RegAlloc 退役。AllocationFacts 与 SemanticFacts 尚未统一，ResolvedUses 仍有 const 惰性构图；预制约束、目标家族脱离 LinearScan、OptimizedUnit、完整轻量 actions 及其余 R0–R7 均继续执行，不能把本批当作全部完成。

## R1 共享 SemanticFacts 与显式派生索引（2026-10-22）

本批接续 common-writer-v1，删除独立 AllocationFacts 类，普通分配、target 和物理重试共享一个 SemanticFacts。writer 在准备作用域内拥有它，AllocationState 借用同一对象并拥有 workspace/策略；Finish 释放请求状态，生产 writer 在返回 prepared 结果前释放事实缓存。此处尚未完成最终 CompileUnit/OptimizedUnit 接缝。

| 文件 | 本批职责与变化 |
| --- | --- |
| compile/analysis/semantic_facts.h、facts.cpp | 同一所有者管理 use-end、flags、CFG、spill positions 和 consumer 图；基础摘要不强制建立完整索引。每块 payload 地址独立，Function 在发布 inline 摘要前预留完整成员容量 |
| compile/analysis/facts.h | 保留 FlagsDemand、BlockUseEnds 等基础类型；原 owning AllocationFacts 类退役，没有别名 |
| compile/analysis/block_index.h | 新增显式 PrepareResolvedUses；ResolvedUses/LastInstructionUse 的 const 查询不补建索引，未准备时拒绝。补充别名索引不改变直接 uses 的 span |
| compile/analysis/uses.h | terminal 与提升锚点的共同遍历，不混合调用方的 use 策略 |
| compile/analysis/cfg.cpp | CFG 活跃性固定点和跨块 use-end；从 allocation/function_liveness.cpp 分离。补齐提升 anchors，使用 ForEachValue 直接访问嵌套参数 |
| compile/allocate/controller.h、writer.cpp | 分配 state 借用单一 facts；仅切换语义输入时更换所有者，scratch/FPR 物理重试不重新构图 |
| ARM64 准备家族 | 在 FPR、memory、width 或具体发表证明需要别名使用时显式准备，算法继续读取 const 图；旧 const 证明入口改为明确的 PrepareCoalescedHostWriteProof |

flags 仍保留两种有不同含义的表示：单元 scratch demand 可以包含跨块使用，块内 packed pseudo demand 只包含该块内且 owner 属于该块的 flags；它们在同一次 IR 扫描中收集，后者在构图成功后移动到共享图。没有 flags pseudo 时不分配全单元 demand 数组。基础 use-end 若先于 consumer 需求建立，不预建整图；已有图时用同一批边派生摘要。spill positions 继续只在压力真正需要时构建，游标仍归 attempt。

GetValues 原本已经访问 Lambda/Params，本批没有把它误记为旧的嵌套使用缺陷；替换为 ForEachValue 的目的是避免临时列表。CFG 的真实遗漏是 loop-hoist anchors，现与普通 terminal 一起进入共同隐式 use 遍历。FPR 的 last-instruction-use 与 GPR/CFG 的隐式根策略仍分别保留，未因统一所有者而强行合并语义。

集中构建修正了两处 const 匹配入口调用准备 API 的问题。首轮主套件出现 1 个新增 error：旧 transfer 夹具在同一准备器内切换两个独立 Block，却没有登记新输入。保留 SemanticFacts 的单元边界检查，夹具显式 SetAllocationUnit 后重建主套件，最终冻结版本为 shared-facts-v2；v1 的失败及单独通过的新 IR 契约日志均保留。v2 只比 v1 改动这一个 ARM64 测试源文件；8 个 RV 源文件的语法检查沿用已通过的 v1 结果，依赖源码未变，未重复检查。

最终 9 个 Android 目标全部构建通过，Ninja 无任务。红魔 NX729J（bf1359ff）上完成最终版本的 35 次核心运行：

- 主套件仍为 710 个用例，12 个失败用例、67 个失败断言、0 errors；失败语义与 common-writer-v1 一致，仅归一化源码行号，没有新增/删除/重命名用例。
- IR default/IR_FAST=0 各通过 29 个用例、594 个断言。新增单一 facts 的两种构建顺序、派生索引不破坏已借用 span、跨 7 块容器扩展、flags 跨块与块内需求区别的契约；扩展现有 CFG/索引用例，验证 anchors、嵌套独占使用、未准备 const 查询拒绝及不提前建立 spill 索引。
- lifetime 的 9 模式 × 两种 IR 模式共 18 次通过，construction/finish-retry/retained 在每种 IR 模式仍分别见证 5/2/1 次注入失败。entry 6 模式共 950 checks、call 6 模式共 233 个顶层 checks、shutdown owners/resource-cycles 两模式共 105 checks 通过。
- RV 原生执行和静态 latch 的既有 4 模式失败集合本批均未运行；不报告为通过。没有新增 probe 或单次测试脚本。

集中完成默认 Function、SVM_BOUND_DECODE=0 的 Function、SVM_FUNC_BASE=0 的普通 Block × CoreMark/bench × 两版本，共 12 次 guest 对照。Block 四次直接使用已有 host dump，不再额外重复诊断；日志按实际标记解析，兼容 bench case 标签与标记位于同一行。

Function 所有公共 PC 的有序 IR/host 大小一致；默认 CoreMark 425→424 条记录，仅少 __dtoa 内 0x25fb00 的 IR 15/host 92，汇总 host bytes 286788→286696。bound-off 的 425 条完整记录、286792 字节及形态一致。bench 两配置的 22 条完整记录、19908/19916 字节及五个校验值一致。

普通 Block CoreMark 两版各 2416 条 host 记录，公共 PC 长度全部一致；候选多 0x253be8/0x253d39、少 0x253cac/0x253d57，均经相同 ELF 的符号范围核对属于 __d2b_D2A，记录长度总和为 206128→206272，差 144 来自实际运行 roster。不将它视为相同 IR 的生成退化。bench 的 101 条有序 PC/长度、16468 字节及完整形态一致。Block 的 [svm-unit] 仍未接线，未用空记录证明零 Block，也未把含 native 地址的完整 host bytes/hash 当作可比较内容。

两版本 spill defs/loads/stores 一致：Function CoreMark 330/102/73、bench 161/245/150；Block CoreMark 771/360/261、bench 188/262/162。HIR pool/Inst arena 一致：Function CoreMark 632320/458752，bench 默认 519360/262144、bound-off 538560/262144；Block CoreMark 0/196608、bench 0/65536。全部 CoreMark CRC 与 bench 五个校验值正确。

默认 Function 与普通 Block 各采一轮四点交错 PMU，共 8 点，顺序仍为参照 2000、候选 2000、候选 12000、参照 12000。命令未开启 profiling/audit/host dump；退出、CRC 均正确，没有 affinity 警告。相对相邻参照的两点拟合变化如下：

| 路径/指标 | 斜率变化 | 截距变化 |
| --- | ---: | ---: |
| Function instructions | +0.139427% | -0.282669% |
| Function cpu-cycles | +0.644760% | +0.163247% |
| Function wall | -0.598271% | +1.693260% |
| Block instructions | -0.097053% | +0.048288% |
| Block cpu-cycles | -0.252540% | -0.173945% |
| Block wall | +1.221261% | -3.520010% |

CPU4、taskset 10、governor=walt；逐点前后读取的当前频率/上限均为 1171200 kHz，不代表锁频。具名传感器 cpuss-0 为 30.1～32.8°C、cpu-1-0 为 30.9～33.2°C、cpu-1-6 为 29.3～30.9°C，不作为独占 CPU4 温度。单轮诊断不确认收益或小幅变化的归因；截距包含启动、编译与非线性，不是纯编译耗时。原始 stat 没有逐事件 running 比例，不新增 IPC 结论；保留 CoreMark 低于 10 秒提示，不报告正式分数。本批未重采 FEX，固定源码 Android FEX 构建仍待补齐。

DWARF 按声明文件核对：AllocationState 从 520→144 字节，旧 AllocationFacts 为 384 字节、旧独立 SemanticFacts 缓存为 40 字节，新合并 SemanticFacts 为 576 字节；BlockRecord 64→112，BlockAnalysisIndex 保持 232。分配 state 与 facts 两个固定对象合计从 560→720 字节，增加 160，不能把 state 变小宣传为总内存下降。writer/ARM64 preparation 仍为 2424/2432、prepared Storage 976、Allocation 816、AllocationView 8 字节；索引 backing 与容器容量仍需计入实际峰值。

源码快照包含 393 个现存修改/新增源文件、46 个删除路径，source digest 为 8baecfcf18f7c01873cc250116cf127d808710dac29fab1fc690dc4e328a3967，archive SHA-256 为 4ba348d25bbabbd3334da77ddd4d0f4e1fa280a1576b775cacd2c1a14275d8b3。候选 svm_shared_facts_v2 SHA-256 为 22cf1d8855023c2b75576d8ca6d5ac81dfc9a19f923831a4dd86b0282911a15d，相邻参照 svm_common_writer_v1 为 5717779b77e9d4c0bc24f804ddf3e0ddc4c3cfa82363a1f690fb911b903d5608。

源码/archive、9 个主机/构建产物、8 个设备候选、参照/guest/tool 的哈希均复核，设备运行前后相同。原始构建、命令、JUnit、寿命/持久化/形态/符号/PMU/布局及身份记录保存在 /private/tmp/swiftvm-middle-results/r1-shared-facts-v2-*；初轮诊断在 r1-shared-facts-v1-*，设备原始日志在 /data/local/tmp/svm-middle-r1/results。

本批完成两套 facts 所有者统一、const 惰性构图退役及隐式根遍历共享；最终 CompileUnit/OptimizedUnit 归属、必需摘要/基础扫描收口、预制约束、完整轻量 actions 与其余 R0–R7 继续执行。

## R1 owning OptimizedUnit 与 typed 公共调度（2026-10-22）

本批接续 shared-facts-v2，将事实的所有者从准备 writer 上移到真实的优化结果。公共调度消费 owning CompileUnit，返回封口的 OptimizedUnit；ARM64 消费该结果完成物理分配和目标准备，在最后一个 writer 退出后移交 IR，分析缓存不进入 emission。没有保留旧 raw-pointer void Optimize 包装，也没有在 writer 中保留便利 owning 构造。

| 文件 | 本批职责与变化 |
| --- | --- |
| compile/optimized_unit.h、optimized_unit.cpp | 移动专属结果；稳定存储持有封口 CompileUnit、不可变 FeatureSet 快照、decoded-block 计数及唯一 SemanticFacts。FromFinalIR 拒绝未封口输入；TakeIR 拒绝仍有活动 writer 的交接，成功后释放 facts 并转移 IR/pool/实际父对象/anchors |
| compile/pipeline.h、pipeline.cpp | CompileOptions 和 owning OptimizeBlock/OptimizeFunction/OptimizeFunctions。Function 的范围、RPO/临时编号、残余 pass、提升及最终编号进入公共流程；整组优化和合并、封口全部结束后才开始目标分配。能力与提升预算由调用方传入，公共层不 include ARM64 |
| compile/analysis/semantic_facts.h、facts.cpp | 已封口输入只允许复用同一身份；Function 检查实际成员，拒绝借用外部 Block。facts 增加独占 writer 借用检查；未封口的显式新输入仍可重置事实，物理重试不更换所有者 |
| compile/allocate/writer.h、writer.cpp、controller.h | writer 显式借用调用方的 SemanticFacts 和 Allocation，不创建或释放 facts；请求状态只拥有 workspace/策略。两份不同物理结果也不能同时借用同一 facts 进行准备 |
| backend/arm64/compile/compiler.h、compiler.cpp | Block、Function、多 region 和最终 HIRBlock 接入优化结果。writer 在目标准备作用域内销毁，随后 TakeIR 交接；prepared 存储只接续 CompileUnit、Allocation、UnitActions。最终 HIRBlock 保留既有容量校验，不重新运行普通 Block pass |
| tests/support/allocation_fixture.h 与现有 IR/region 测试 | 测试输出独立持有 facts/物理结果/actions。新增配置快照、移动后 facts/索引地址稳定、未封口输入拒绝、借用期间禁止移交、跨物理结果重复借用拒绝及 Function 非成员拒绝；交接后仍验证封口与物理结果 |

首轮 9 个 Android 目标全部构建成功，没有编译错误；最终 Ninja 无任务。8 个额外 RV 源文件重新完成语法检查，均通过。冻结产物后在红魔 NX729J（bf1359ff）集中完成 35 次核心运行：

- 主套件 710 个用例，仍为 12 个失败用例、67 个失败断言、0 errors；有序用例名与失败类型、消息和文本均与 shared-facts-v2 一致。失败文本只归一化 cpp/h 行号，JUnit 的 binary 派生 classname 前缀单独排除；没有新增、删除或重命名主用例。原始总断言数为 221097→221095，未以断言总数相同作为等价依据。
- IR default/IR_FAST=0 各通过 30 个用例、609 个断言。新增 optimized ownership 行为用例，扩展已有共享 facts 用例的非成员拒绝；没有新建测试源文件或脚本。
- lifetime 9 模式 × 两种 IR 模式共 18 次通过；construction/finish-retry/retained 每种 IR 模式的注入失败分别为 5/2/1。entry 6 模式共 950 checks、call 6 模式共 233 个顶层 checks，以及 shutdown owners/resource-cycles 两模式共 105 checks 通过。
- 静态 latch 的既有 4 模式 fixture 失败和 RV 原生执行本批未重跑，不报告为通过；shutdown 只覆盖上述两模式。

同批完成默认 Function、SVM_BOUND_DECODE=0 的 Function、SVM_FUNC_BASE=0 的普通 Block × CoreMark/bench × 两版本，共 12 次 guest 对照。Block 四次使用已有 host dump；解析时保留 case 标签与标记同一行的记录，并将 host hash 与 guest checksum 分开处理。

| 路径/负载 | 相邻参照/候选记录数 | 代码大小与形态结果 |
| --- | ---: | --- |
| Function 默认 CoreMark | 425/425 | 完整有序 IR/host 大小及全部 shape 一致；286788 host bytes |
| Function bound-off CoreMark | 424/424 | 完整有序 IR/host 大小及全部 shape 一致；286700 host bytes |
| Function 默认/bound-off bench | 各 22/22 | 完整记录及 shape 一致；19908/19916 host bytes |
| 普通 Block CoreMark | 2412/2416 | 公共 PC 的有序长度一致；总 host bytes 205572→205872，差异来自记录 roster |
| 普通 Block bench | 101/101 | 完整有序 PC/长度及全部 shape 一致；16468 host bytes |

Block CoreMark 候选多出 0x25f929/28、0x25faf8/92、0x25fb00/88、0x25fb10/72、0x25fbbc/112，少 0x25f966/92；相同 ELF 的 llvm-nm 证明六个 PC 都在 __dtoa 的 0x25f2d0～0x2602e7 范围。差 300 字节不用于判断相同 IR 的生成退化。Block 的 [svm-unit]/block_units 仍未接线，不把空记录当作等价证明，也不比较含 native 地址的完整机器码 hash。

两版本 spill defs/loads/stores 一致：Function CoreMark 330/102/73、bench 161/245/150，Block CoreMark 771/360/261、bench 188/262/162。HIR pool/Inst arena 一致：Function CoreMark 632320/458752，bench 默认 519360/262144、bound-off 538560/262144；Block CoreMark 0/196608、bench 0/65536。全部 CoreMark CRC 和每次 bench 的五个校验值正确。

默认 Function 与普通 Block 各采一轮四点交错 PMU，共 8 点，顺序为参照 2000、候选 2000、候选 12000、参照 12000。命令未开启 profiling/audit/host dump；退出和 CRC 均正确，没有 affinity 警告。相对相邻参照的两点拟合变化如下：

| 路径/指标 | 斜率变化 | 截距变化 |
| --- | ---: | ---: |
| Function instructions | -0.007495% | +0.147137% |
| Function cpu-cycles | +0.511733% | -0.163571% |
| Function wall | +0.041842% | -1.205866% |
| Block instructions | -0.010133% | +0.108454% |
| Block cpu-cycles | +0.010589% | +5.595365% |
| Block wall | +0.942577% | +2.658777% |

CPU4、taskset 10、governor=walt；逐点前后当前频率/上限均为 1171200 kHz，不代表锁频。具名传感器 cpuss-0 为 30.1～32.8°C、cpu-1-0 为 30.9～33.6°C、cpu-1-6 为 29.3～30.9°C，不视为 CPU4 独占温度。单轮诊断不确认收益或小幅变化的归因；截距包含启动、编译和非线性，不是纯编译耗时。stat 没有逐事件 running 比例，不新增 IPC 结论；低于 10 秒的 CoreMark 提示保留，不报告正式分数。FEX 本批未重采，固定源码 Android 构建仍待补齐。

DWARF 按声明文件核对：OptimizedUnit 句柄 8 字节，新稳定 Storage 720 字节，内含 CompileUnit 56、SemanticFacts 584、FeatureSet 69 和计数/填充；旧独立 facts 为 576，新增借用检查后增加 8。prepared Storage 976→984，AllocationState 144、writer/ARM64 preparation 2424/2432、Allocation 816、AllocationView 8、BlockRecord 112、BlockAnalysisIndex 232 均不变。持有权上移引入了新的存储边界，不据此声称内存或分配次数下降；分析缓存仍在准备结束后、发射前释放，动态 backing 和实际峰值另行计算。

源码快照含 395 个现存修改/新增源文件、46 个删除路径；source digest 为 dc2399d40b4320a4461d077d41316b4c9dffdf6942447745d388b85026e914b8，archive SHA-256 为 b9224c0d6c6635f3bf263d357715b24d5af76cc7aeb0122a814602173ad6a887。候选 svm_optimized_unit_v1 SHA-256 为 b84c24c37b4960ec9df95829ed5f173090fa35f2cf11b265f3c0abf8a6d65945；相邻参照 svm_shared_facts_v2 为 22cf1d8855023c2b75576d8ca6d5ac81dfc9a19f923831a4dd86b0282911a15d。

源码/archive、9 个主机/构建产物及设备候选/参照/guest/tool 的哈希均已核对，设备运行前后 12 项相同。构建、命令、JUnit、寿命/持久化、形态、符号定位、PMU、DWARF 和身份记录保存在 /private/tmp/swiftvm-middle-results/r1-optimized-unit-v1-*；设备原始日志在 /data/local/tmp/svm-middle-r1/results。仓库没有新增临时 probe 或单次测试脚本。

本批完成 OptimizedUnit 的真实持有权和 typed 公共调度接缝；必需摘要、剩余基础扫描、显式 workspace、预制约束、完整轻量 actions、其他后端调用方及其余 R0–R7 继续执行。

## R1 基础活跃性输入与跨 attempt 复用（2026-10-22）

本批接续 optimized-unit-v1，将普通 Block、单块 Function 和多块 Function 的三套区间前扫描收拢到 SemanticFacts 的 ValueLiveness。分配只读取同一份基础使用末端、host-register 写入与 Function 局部性；scratch/FPR 重试不重建。区间、fixed mapping 和物理证明仍归分配阶段，CFG 固定点、consumer 与 spill next-use 继续按需。

| 文件 | 本批职责与退役项 |
| --- | --- |
| compile/analysis/liveness.h、liveness.cpp | 只读 ValueLiveness、紧凑 HostRegisterWrites 和查询；区分 Block 的实际物理读与 Function 的当前操作数/保守 HIR use-end，块尾直接查询已有 IR 元数据。host 写位置按寄存器/指令排序，不建立逐寄存器树或重复 FPR 写表 |
| compile/analysis/facts.cpp、semantic_facts.h | PrepareValueLiveness 首次合并收集当前 use-end、host 写、Function 局部性和两种 flags demand；PrepareFlags 单独触发时保留轻路径。已有 flags 时只补活跃性，已有 ValueLiveness 时直接复用；所有发布后的查询只读 |
| compile/allocate/intervals.cpp、linear_scan.cpp、internal.h | Block/Function 各自生成区间，单块 Function 的资格检查和专用活跃集保留；删除三套操作数/terminal 前扫描、每次 attempt 的 Function 局部性向量及 scalar-tie FPR 写表。过渡 function_liveness.cpp 与 CMake 条目退役 |
| backend/arm64/compile/fixed_registers.cpp、fpr_coalesce.cpp | fixed capture 和 scalar FPR 直接读取共享 host 写位置；保留 fixed capture 的右端包含和 scalar FPR 的严格中间写入判断，删除两套 write-map/flat-list 重载 |
| tests/ir_contract_test.cpp | 扩展跨块嵌套参数/terminal/anchors 和 owning 结果复用检查；新增 flags/活跃性两种准备顺序、虚拟 BitCast 与逻辑使用区别、capture 写边界与 FPR 寄存器分组的真实契约 |

基础扫描不把语义不同的使用强行合并：独立 Block 不因虚拟 BitCast 本身延长物理读取，GPR/width 的逻辑 use-end 仍保留该边；Function 保留既有可达操作数和 HIR use-list 上界。Function 局部性观察所有成员中的使用，实际区间和 host 写只观察 RPO 可达块；被丢出 RPO 的使用不拉长可达定义。terminal/提升 anchors 由共享隐式 use 遍历接入。保守 HIR 上界只为高于当前操作数末端的条目保存稀疏覆盖，不再为通用 Function 保存三套整单元临时末端数组。

构建首轮在 CMake 生成阶段失败：误将 CMakeLists.txt 交给 C++ formatter，导致路径被拆开。已从相邻冻结源码恢复原格式，仅应用新增/退役 sources，格式化范围限定为 C++。第二轮生产源文件构建通过，新 IR 夹具遗漏 GetHostGPR 的 offset 参数，补齐后第三轮 9 个 Android 目标全部构建完成；最终 Ninja 无任务。8 个额外 RV 源文件语法检查全部通过。构建修正没有放宽生产契约。

冻结版本在红魔 NX729J（bf1359ff）集中完成 35 次核心运行：

- 主套件 710 个用例，仍为 12 个失败用例、67 个失败断言、0 errors；有序用例名、classname 后缀及全部失败类型/消息/文本与 optimized-unit-v1 一致，仅归一化 binary 派生前缀和 cpp/h 行号。候选原始断言总数 221308，没有新增/删除/重命名主用例。
- IR default/IR_FAST=0 各通过 31 个用例、656 个断言；新增上述基础活跃性契约，补充既有跨块和持有权检查。
- lifetime 9 模式 × 两种 IR 模式共 18 次通过；construction/finish-retry/retained 的注入失败按每种 IR 模式分别为 5/2/1。entry 6 模式共 950 checks、call 6 模式共 233 个顶层 checks，以及 shutdown owners/resource-cycles 两模式共 105 checks 通过。
- 本批未运行 RV 原生代码、静态 latch 的既有 4 模式 fixture 失败或完整 shutdown 矩阵，不将其报告为通过；没有新增 probe、测试源文件或单次测试脚本。

同批完成默认 Function、SVM_BOUND_DECODE=0 的 Function、SVM_FUNC_BASE=0 的普通 Block × CoreMark/bench × 两版本，共 12 次 guest 对照。全部共同 PC 的有序 IR/host 大小（Function）或 host 长度（Block）一致。bench 的 Function 两配置均为 22 条完整记录、19908/19916 字节；Block 为 101 条、16468 字节，完整有序记录及全部 shape 一致，五个校验值正确。

CoreMark 的运行 roster 差异经同一 ELF 的符号范围核对：默认 Function 424→425，仅多 __dtoa 内 0x25fb00 的 IR 15/host 92，host bytes 286696→286788；bound-off 425→424，仅少同一记录，286792→286700。普通 Block 2415→2412，候选多 0x253cac/36、0x253d57/120，少 0x253be8/40、0x253c2b/252、0x253c8e/72、0x253d39/260、0x253d42/256，七个 PC 均在 __d2b_D2A 的 0x253b00～0x253e42 范围；host bytes 206296→205572，差 -724 来自 roster。全部 CoreMark CRC 正确，不用这些总量差异推断相同 IR 的代码质量变化。

spill defs/loads/stores 均一致：Function CoreMark 330/102/73、bench 161/245/150；Block CoreMark 771/360/261、bench 188/262/162。HIR pool/Inst arena 均一致：Function CoreMark 632320/458752，bench 默认 519360/262144、bound-off 538560/262144；Block CoreMark 0/196608、bench 0/65536。Block 四次直接使用已有 host dump，没有额外重跑；未把空 [svm-unit] 记录或带 native 地址的 host hash 当作等价证据。

默认 Function 与普通 Block 各采一轮四点交错 PMU，共 8 点，顺序为参照 2000、候选 2000、候选 12000、参照 12000；没有开启 profiling/audit/host dump，退出、CRC 均正确，没有 affinity 警告。

| 路径/指标 | 斜率变化 | 截距变化 |
| --- | ---: | ---: |
| Function instructions | +0.119462% | -0.566595% |
| Function cpu-cycles | -1.108420% | +0.084642% |
| Function wall | -2.321066% | +0.382758% |
| Block instructions | +0.178972% | -1.144582% |
| Block cpu-cycles | +1.050002% | -4.034493% |
| Block wall | -0.782595% | +0.728597% |

CPU4、taskset 10、governor=walt，逐点前后当前频率/上限均为 1171200 kHz，不代表锁频；cpuss-0 为 30.9～33.6°C、cpu-1-0 为 30.9～33.2°C、cpu-1-6 为 29.7～32.0°C，均为具名传感器读数。单轮诊断不确认收益或小幅变化的归因；截距含启动、编译和非线性，不是纯编译耗时。stat 无逐事件 running 比例，不新增 IPC 结论；保留 CoreMark 低于 10 秒提示，不报告正式分数。本批未重采 FEX，固定源码 Android 构建仍待补齐。

DWARF 按声明文件核对：新 ValueLiveness 320 字节，其中两份 HostRegisterWrites 各 56；SemanticFacts 584→592，OptimizedUnit Storage 720→728，LinearScan 2112→2072。AllocationState 144、writer/ARM64 preparation 2424/2432、prepared Storage 984、Allocation 816、AllocationView 8、CompileUnit 56 不变。缓存延长到跨 attempt 准备作用域后释放，不进入 emission；不将扫描器固定对象变小当作总内存下降，新增缓存、动态 backing、临时表退役和实际峰值需要一起计算。

源码快照含 396 个现存修改/新增源文件、46 个相对起点的删除路径；相邻快照中的过渡 function_liveness.cpp 另已退役。source digest 为 0cc4c4416ea8322cd30771985388e285c1d68f59e95ca61e180475f12f95d940，archive SHA-256 为 25110effd5b39e846d369f4dbc3a723c6c324a8943699a7efe73a1500fbbcb72。svm_base_liveness_v1 SHA-256 为 6dad9979129253773d46fb2179d64b88ff1fe8f0ed02fd5a01d3d2b10d6bfa7d；相邻参照 svm_optimized_unit_v1 为 b84c24c37b4960ec9df95829ed5f173090fa35f2cf11b265f3c0abf8a6d65945。

构建、命令、JUnit、寿命/持久化、形态/符号、PMU、DWARF 与身份记录在 /private/tmp/swiftvm-middle-results/r1-base-liveness-v1-*；设备原始日志在 /data/local/tmp/svm-middle-r1/results。设备运行前后 12 项哈希一致，源码/archive 与主机/构建产物身份一并核对。

本批完成基础活跃性从物理 attempt 中分离及首个分配消费者的 flags 合并扫描；必需 use-count/presence/语义宽度摘要、剩余 target 扫描、显式 workspace、预制约束、完整轻量 actions 及其余 R0–R7 仍待接续。

## R3 显式 AllocationInputs 与目标候选预制（2026-10-22）

本批接续 base-liveness-v1，让公共扫描器显式消费移动专属 AllocationInputs；原 FeatureSet-only 入口已移除。ARM64 在扫描前生成默认预算、fixed-class/静态 clobber、稀疏 GPR tie、调用返回偏好、发表来源和首个局部 consumer 的 memory 摘要。请求持有 const 输入，scratch/FPR 重试复用；实际 mapping、preservation 和动态 clobber 冲突继续由物理 attempt 判断。

| 文件 | 本批职责与退役项 |
| --- | --- |
| compile/allocate/inputs.h、inputs.cpp | 配置、Block/Function 身份和编号容量、池策略及紧凑表；校验输入身份/实际池，提供只读范围查询。没有 target callback 或可变分配结果 |
| backend/arm64/compile/inputs.h、inputs.cpp | PrepareAllocationInputs 显式开始新请求；DescribeAllocationInputs 为已绑定输入生成描述，保留冲突夹具的现有 mapping。候选匹配借用 SemanticFacts 的直接 consumer/flags 图，保留 memory/narrow/integer-width 的优先顺序 |
| compile/allocate/ties.cpp | 根据预制输入执行 GPR tie/偏好、active set 更新、home 转交与 clobber 相交；将静态 clobber 与本 attempt 的发表/固定定义叠加，动态证明仍在这里执行 |
| controller.h、allocation.cpp、linear_scan.cpp | 请求拥有 const AllocationInputs，FeatureSet 从输入借用；移除目标 DescribeAllocationBudget 回调、逐 attempt 的 call-ABI 向量、const NextInst 惰性表及 GPR 模式匹配入口 |
| backend/arm64/compile/fixed_registers.cpp | 保留尚未数据化的 fixed/preservation、必要核验和物理收尾；GPR 候选与静态 clobber 已迁出。旧 gpr_ties.cpp 及 CMake 条目退役，没有转发文件或兼容别名 |
| compiler、现有调用方与测试支持 | 208 处分配调用显式提供输入；14 处手工扫描使用单独持有的描述，避免临时对象借用悬空或重置已注入的物理冲突。公共 IR 契约使用目标无关输入，ARM64 契约使用真实 AllocationPreparation |

首次 guest-home 声明会扩大部分调用方的保留 GPR 集合；旧代码在首次扫描前后、reserve/FPR 重试时可能因此得到不同策略。本批显式描述声明前/后的两个实际池，保存各自默认预算和 fixed-class，并为静态 clobber 保存基值及差量。调用方原来的首次尝试与重试行为因此继续保留；这是两个小型策略描述，不是双份 Allocation 或 IR。任意其他池、错单元或编号容量变化会被拒绝。

首轮编译发现两处无效 ID 常量名称的作用域错误，修正后 9 个 Android 目标全部构建成功。v1 先跑 IR 时，新 ARM64 夹具使用了没有 ARM64 home 掩码的公共 writer，必要 fixed-home 检查拒绝它，主套件和 guest/PMU 均未运行。仅将该夹具改为真实 AllocationPreparation 并补充显式 include，最终冻结为 inputs-v2，生产检查没有放宽。源码清单确认 v2 相比 v1 只改动 ir_contract_test.cpp；8 个已通过的 RV 语法检查据此沿用 v1，未重复运行。v2 完成构建，最终 Ninja 无任务。

最终版本在红魔 NX729J（bf1359ff）集中完成 35 次核心运行：

- 主套件 710 个用例，仍为 12 个失败用例、67 个失败断言、0 errors；有序名称、classname 后缀和失败类型/消息/文本与 base-liveness-v1 一致，只归一化 binary 派生前缀和 cpp/h 行号。原始断言数 220807，没有新增/删除/重命名主用例。
- IR default/IR_FAST=0 各通过 32 个用例、671 个断言；新增输入配置快照、池声明前后策略、错池/错单元拒绝、移动后的表视图及输入移交后实际驱动 GPR tie 的契约。
- lifetime 9 模式 × 两种 IR 模式共 18 次通过，construction/finish-retry/retained 每种模式的注入失败仍为 5/2/1；entry 6 模式共 950 checks、call 6 模式共 233 个顶层 checks、shutdown owners/resource-cycles 两模式共 105 checks 通过。
- RV 原生执行、静态 latch 的既有 4 模式 fixture 失败和完整 shutdown 矩阵本批未运行，不报告为通过；仓库没有新增 probe、测试源文件或单次测试脚本。

同批完成默认 Function、SVM_BOUND_DECODE=0 的 Function、SVM_FUNC_BASE=0 的普通 Block × CoreMark/bench × 两版本，共 12 次 guest 对照。Function 默认 CoreMark 的 424 条有序 IR/host 大小及全部 shape 一致，286696 字节；bound-off 的 425 条及全部 shape 一致，286792 字节。bench 两个 Function 配置均为 22 条、19908/19916 字节；Block 为 101 条、16468 字节，完整有序记录及全部 shape 一致，五个校验值正确。

普通 Block CoreMark 2416→2417 条，公共 PC 的有序 host 长度全部一致；host bytes 206308→206104。候选多 0x253cac/36、0x253d57/120、0x25f951/168、0x25f963/92、0x25f966/92、0x25f971/28；少 0x253c2b/252、0x253c8e/72、0x253d42/256、0x25fb00/88、0x25fb10/72。相同 guest ELF 证明 0x253xxx 的五个 PC 属于 __d2b_D2A，0x25fxxx 的六个 PC 属于 __dtoa；差 -204 来自运行 roster，不作为相同 IR 代码质量变化。全部 CoreMark CRC 正确；Block 使用已有 host dump，未额外重跑，未用空 [svm-unit] 或带 native 地址的 hash 证明等价。

spill defs/loads/stores 两版一致：Function CoreMark 330/102/73、bench 161/245/150；Block CoreMark 771/360/261、bench 188/262/162。HIR pool/Inst arena 一致：Function CoreMark 632320/458752，bench 默认 519360/262144、bound-off 538560/262144；Block CoreMark 0/196608、bench 0/65536。

默认 Function 与普通 Block 各采一轮四点交错 PMU，共 8 点，顺序为参照 2000、候选 2000、候选 12000、参照 12000。未启用 profiling/audit/host dump；退出和 CRC 均正确，没有 affinity 警告。

| 路径/指标 | 斜率变化 | 截距变化 |
| --- | ---: | ---: |
| Function instructions | -0.007043% | -0.038511% |
| Function cpu-cycles | -1.230821% | +1.942071% |
| Function wall | -1.794577% | +2.724878% |
| Block instructions | -0.008806% | +0.357495% |
| Block cpu-cycles | -1.869188% | +4.291727% |
| Block wall | -3.713529% | +9.851885% |

CPU4、taskset 10、governor=walt，上限均为 1171200 kHz；Block 候选 12000 点结束后当前频率为 1056000，其余逐点前后读取为 1171200，不按锁频比较。cpuss-0 为 29.7～34.0°C、cpu-1-0 为 30.9～33.6°C、cpu-1-6 为 29.3～31.3°C；这些不是 CPU4 独占温度。单轮及频率变化不足以确认收益或归因；截距包含启动、编译和非线性，不能解释为纯编译时间。stat 没有逐事件 running 比例，不新增 IPC；CoreMark 低于 10 秒提示保留，不报告正式分数。本批未重采 FEX，固定源码 Android 构建仍待补齐。

DWARF 按声明文件核对：AllocationInputs 288 字节，PoolPolicy 20，AllocationBudget 12；请求 AllocationState 144→360，LinearScan 2072→1968，二者固定对象合计增加 112 字节。SemanticFacts 592、ValueLiveness 320、OptimizedUnit Storage 728、writer/ARM64 preparation 2424/2432、prepared Storage 984、Allocation 816、AllocationView 8、CompileUnit 56 均不变。新增稀疏表与退役的逐 attempt 数组需要计入动态 backing 和峰值；不以扫描器固定对象变小声称总内存或分配次数下降。

最终源码快照含 400 个现存修改/新增源文件、46 个相对起点的删除路径，相邻快照的 gpr_ties.cpp 另已退役；source digest 为 7440390fdcef4ed8d13e102ac366ddf06203709e4d0c0f7585a93adb40d10ebb，archive SHA-256 为 1c2270feab8f2cff9331bc9a47e79590eaa1bc91bdd3329b30203b29a16998e9。svm_inputs_v2 SHA-256 为 87433d31316924767119600473e5f74eba93c871188fed8e78a9e6f6611e2d55，相邻参照 svm_base_liveness_v1 为 6dad9979129253773d46fb2179d64b88ff1fe8f0ed02fd5a01d3d2b10d6bfa7d。v1 与 v2 launcher 相同，只有 IR 测试产物改变。

源码/archive、9 个主机/构建产物及设备候选/参照/guest/tool 身份均核对；设备前后 12 项哈希一致。原始构建、命令、JUnit、寿命/持久化、形态/符号、PMU、布局与身份记录在 /private/tmp/swiftvm-middle-results/r3-inputs-v2-*；首轮诊断在 r3-inputs-v1-*，设备日志在 /data/local/tmp/svm-middle-r1/results。

本批完成静态目标输入的显式交接与 GPR 模式匹配迁出扫描器；fixed/preservation、FPR、逐指令 scratch 及其他目标家族仍有 LinearScan 接缝，R3 尚未完成。R1 必需摘要/剩余扫描/workspace、R2 worklist/提交契约、R5/R6 轻量 actions 及其余 R0–R7 继续执行。

## R3 初始 fixed/preservation 输入与紧凑 home 索引（2026-10-22）

本批接续 inputs-v2，把初始 fixed/read/transfer/capture 的生产从可变分配 writer 中移出。ARM64 读取最终编号 IR、声明后的寄存器池和共享 SemanticFacts，生成请求拥有的稀疏 FixedGPRInput；普通 GPR/MEM mapping 不再是初始生产依赖。Block 和 Function 保持各自入口，Function 仍按 RPO 收集约束，并保留跨块完整 use-end 与原有候选/发表提交顺序。

| 文件 | 本批职责与退役项 |
| --- | --- |
| compile/allocate/constraints.h、constraints.cpp | 唯一公共 FixedGPRDefinition/FixedGPRInput，以及单一稀疏缓冲的 FixedGPRHomeIndex；前缀最大 end 与区间合存，初始生产和物理编辑共用同一查询算法 |
| backend/arm64/compile/constraints.h、constraints.cpp | IR-only InitialConstraints 收集器；初始 read/transfer、窄读、capture、地址别名及安全结果 home。语义使用先记录视图宽度再解 BitCast；普通参数使用 ForEachValue，terminal/hoist-anchor 使用共用 VisitImplicitUses |
| backend/arm64/compile/constraints_verify.cpp | 原必要物理检查迁入独立文件，检查 GPR/MEM 结果、helper/clobber、target writes、preserved 形状与 transferred-use 见证；逻辑未改变，仅格式与相邻字符串排版变化，不受可选 audit 控制 |
| ARM64 inputs.cpp 与公共 allocate/ties.cpp | 前者在新请求准备稀疏输入；后者安装一次并标记 ready。物理重试保留已有降级、替换或移除，不重新覆盖为原始声明；扫描器不再调用目标初始分析函数 |
| allocate/writer_locations.cpp、writer.h | home 冲突使用公共紧凑索引，退役 32 份区间 vector 与 32 份 prefix vector；开始新请求清除旧声明、发表、consumer home、映射和请求状态，正确性重试直接重置物理 attempt 并保留当前声明 |
| 既有契约和夹具 | 新增物理重试/新请求隔离及重叠、transfer、两个忽略 owner、清除后重建的边界覆盖；窄 preserved 后缀通过实际输入表建模。旧 alloc/fixed_gpr_constraints.{h,cpp}、全部 include/CMake 条目已删除，无兼容头或别名 |

home residence 使用 [begin,end)，写窗口按原有顺序判断：residence.begin <= write.end 且 residence.end > write.begin 才冲突；零长写点、终点读后覆盖以及两个忽略值均有既有套件中的行为检查。稀疏输入按首次插入顺序交给公共核心，避免排序改变原发表 owner 的首次登记行为。尚依赖最终 mapping、压力和动态 clobber 的候选继续留在物理阶段。

v1 完成九目标构建和八个 RV 语法检查，IR 两模式均通过；主套件退出 95（72 个失败断言、23 errors），随即停止后续运行并保存源码/二进制和日志。新增问题来自旧夹具在完成分配后重复 SetAllocationUnit，以及在输入准备前预设 ready 来禁用初始约束。现有夹具已迁移：完成阶段直接消费已绑定请求，禁用预分配通过显式空 fixed 表表达；测试支持允许传入 owning AllocationInputs，原断言保留。v2 首轮发现新测试调用方缺少 inputs.h 显式 include，补齐后九目标构建通过，最终 Ninja 无任务。v2 相比 v1 只改动 11 个测试/支持文件，仅主套件二进制改变；生产源码及其余八个产物相同。

最终版本在红魔 NX729J（bf1359ff）完成 33 次新运行，另沿用 v1 中相同 IR 二进制已通过的两次运行，共覆盖 35 项核心场景，没有为未变更的 IR 产物重复测试：

- 主套件 711 个用例、221265 个断言，仍为 12 个失败用例、67 个失败断言、0 errors。新增一个 home-window 用例，其余有序名称、classname 后缀与失败类型/消息/文本和 inputs-v2 一致；只归一化二进制前缀和 cpp/h 行号。
- IR default/IR_FAST=0 各 33 个用例、694 个断言通过；新增输入安装前无物理声明、安装后实际 home、物理重试不恢复已移除约束、另一约束继续有效，以及新请求清除旧映射/重试状态的契约。
- lifetime 9 模式 × 两种 IR 模式通过，construction/finish-retry/retained 的注入失败仍为每种模式 5/2/1；entry 六模式 950 checks、call 六模式 233 个顶层 checks、shutdown owners/resource-cycles 两模式 105 checks 通过。
- 共享测试头改动后八个 RV 语法检查重新执行并全部通过。RV 原生执行、静态 latch 的既有四模式 fixture 失败、完整 shutdown 矩阵本批未运行，不报告为通过；没有新增 probe、测试源文件或单次测试脚本。

同批执行默认 Function、SVM_BOUND_DECODE=0 的 Function、SVM_FUNC_BASE=0 的普通 Block × CoreMark/bench × 两版本，共 12 次 guest 对照。默认 Function CoreMark 两版均 425 条完整有序记录、286788 host bytes；bound-off 均 425 条、286792 字节，全部 shape 一致。bench 的两种 Function 均 22 条、19908/19916 字节；普通 Block 均 101 条、16468 字节，完整记录与 shape 一致，五个校验值正确。

普通 Block CoreMark 为 2415→2412 条、205944→205572 host bytes；全部公共 PC 的有序长度一致，没有候选新增 PC。候选少 0x253be8/40、0x253bf1/68、0x253d40/264；相同 guest ELF 的符号表证明它们均在 __d2b_D2A（起点 0x253b00、size 0x342）内。差 -372 来自运行 roster，不能作为相同 IR 的代码质量收益。Block 使用计划内已有 host dump，不用空 [svm-unit] 或包含 native 地址的 hash 证明等价；所有 CoreMark CRC 正确。

spill defs/loads/stores 两版一致：Function CoreMark 330/102/73、bench 161/245/150；Block CoreMark 771/360/261、bench 188/262/162。HIR pool/Inst arena 一致：Function CoreMark 632320/458752，bench 默认 519360/262144、bound-off 538560/262144；Block CoreMark 0/196608、bench 0/65536。

Function 和 Block 各采一轮四点交错 PMU，共八点，顺序为参照 2000、候选 2000、候选 12000、参照 12000；未启用 profiling/audit/host dump。退出和 CRC 均正确，没有 affinity 警告。

| 路径/指标 | 斜率变化 | 截距变化 |
| --- | ---: | ---: |
| Function instructions | -0.014644% | -0.456189% |
| Function cpu-cycles | +0.613163% | -1.471469% |
| Function wall | +0.421188% | -1.574963% |
| Block instructions | +0.061598% | -0.970768% |
| Block cpu-cycles | -1.576457% | +0.883888% |
| Block wall | -1.186143% | +0.705115% |

CPU4、taskset 10、governor=walt，上限均为 1171200 kHz；Function 参照 2000 点开始前为 940800，其余逐点前后为 1171200，不是锁频测量。cpuss-0 为 30.5～34.4°C、cpu-1-0 为 31.6～34.0°C、cpu-1-6 为 30.1～32.0°C；具名传感器不是 CPU4 独占温度。单轮及频率变化不足以确认收益、退化或归因。截距含启动、编译和非线性，不是纯编译时间；stat 没有逐事件 running 比例，不新增 IPC，保留 CoreMark 低于十秒提示，不报告正式分数。本批未重采 FEX，固定源码 Android 构建仍待补齐。

DWARF 按声明文件核对：AllocationWriter 2424→912 字节，ARM64 AllocationPreparation 2432→920；AllocationInputs 288→312，AllocationState 360→384，LinearScan 1968 不变。公共 FixedGPRDefinition 32、FixedGPRInput 36、FixedGPRHomeIndex 24、Interval 20。SemanticFacts 592、ValueLiveness 320、OptimizedUnit Storage 728、prepared Storage 984、Allocation 816、AllocationView 8、CompileUnit 56 均不变。writer 固定对象减少 1512 字节，但新的稀疏声明在请求期间保留，构建器另有临时 value→entry 索引；每个 home 区间记录从旧 interval/prefix 的 12+4 变为 20 字节。动态 backing、容量、分配次数与真实峰值尚未一起测量，不能据此宣称总内存减少。

最终源码快照含 403 个现存修改/新增源文件、48 个相对起点删除路径；本批退役旧 fixed_gpr_constraints 两文件。source digest 为 dfe6e604c239ef9d0c56b2338ea3c92c1a0b944b1e83d22a72a98e26cd24346e，archive SHA-256 为 be9fae128991813029bf95d6386e59971ce0b184307cbb693b5d239a294a4600。svm_fixed_inputs_v2 SHA-256 为 c83d42bb43ced9738b729a6a8efab08b833523e5b03f566c674a0c62cd6b7150；相邻参照 svm_inputs_v2 为 87433d31316924767119600473e5f74eba93c871188fed8e78a9e6f6611e2d55。

源码/archive 全体成员、九个主机/构建产物与设备候选/参照/guest/tool 身份均核对；设备前后十二项哈希一致。原始构建、命令、JUnit、寿命/持久化、形态/符号、PMU、布局和身份记录位于 /private/tmp/swiftvm-middle-results/r3-fixed-inputs-v2-*；首轮失败版本独立保留为 r3-fixed-inputs-v1-*。两个沿用 IR 结果的命令和日志仍明确指向 v1，未改写为并未执行的 v2 命令。设备日志保留在 /data/local/tmp/svm-middle-r1/results。

本批完成初始 fixed/preservation 的单向输入和公共 home 索引，未完成后续物理 fixed、FPR、逐指令 scratch 与完整目标家族分离；R1 必需摘要/剩余扫描/workspace、R2 worklist/提交契约、R5/R6 轻量 actions、固定源码 FEX 及其余 R0–R7 继续保留在范围内。

## R3 FPR 语义输入与公共物理执行（2026-10-22）

本批接续 fixed-inputs-v2，把 destructive FPR tie、scalar merge、resident publication 和 AES 链的语义准备从 LinearScan/物理 coalescer 移到 ARM64 输入构建。公共核心消费请求持有的 const FPRInputs，只检查实际 FPR home、存活冲突、已合并 read 和 active interval，并提交 mapping/发表标记。旧 compile/fpr_coalesce.cpp 已删除，没有转发文件或旧 helper 别名。

| 文件 | 本批职责与退役项 |
| --- | --- |
| compile/allocate/fpr_inputs.h、fpr_inputs.cpp | 可选 FPRInputs 及只读查询；共享平铺 ties、写窗口、单块 operand use-end、publication、链节点及动态输入冲突 ID。普通 GPR 输入保持 nullptr，请求只增加一个 owning 指针，不常驻七个 vector 容器 |
| backend/arm64/compile/fpr_inputs.h、fpr_inputs.cpp | 在实际 mapping 产生前匹配 AES/shuffle/scalar tie，保留 scalar insert 两模式的不同 merge 条件；提前证明普通发表与 AES fault-safe 窗口，observer/块边界截断未来写集合并压缩为区间 |
| compile/allocate/fpr.cpp | 通用 FPR tie 与 publication 执行；检查实际 home、merge 和重叠存活值，成功后才转交 active interval、写 mapping、AES tied 与发表省略标记。没有 ARM64 头、opcode 匹配、普通 IR 参数扫描或逐 attempt 的 use-end 构建 |
| ARM64 inputs.cpp、公共 AllocationInputs | 复用 SemanticFacts 的共享 use 图，按需生成可选表并在移交时变为 const；Block/Function 各自保持原遍历/发表顺序。物理重试借用原表，禁用 FPR coalescing 的正确性重试跳过其消费 |
| LinearScan 与 fixed_registers.cpp | 删除 ScalarInsertTieSource、CollectResidentFPRWrites、scalar_tie_future_writes；保留实际固定 read 的活跃结束表，固定读取别名及最终目标契约仍有待收敛职责 |
| allocation_rules.h 与现有测试支持 | 删除八个旧 FPR helper 的公开声明，直接调用公共物理操作。已有 conflict 夹具继续在注入 mapping 后生成纯描述，没有重置其冲突；IR 契约增加模式、observer 和空家族输入覆盖 |

原 AES 每个链节点的全块语义消费者扫描改为显式准备并查询共享 ResolvedUses；没有另建一份反向图。FPR operand end 保留旧语义：普通参数边包含 BitCast 传递边，不混入 terminal/hoist 隐式使用，不能与物理活跃性或完整 BlockUseEnds 混用。静态窗口失败的普通 publication 仍保留：如果物理结果已在目标 home，原路径允许只省略发表，无需 recolor；实际重放保持这一优先顺序。

首次构建发现两个 GPR helper 的参数被错误扩展，以及新文件缺少 ResolveBitCastSource 的显式声明 include；等构建终止后修正。第二次构建九个 Android 目标通过，最终 Ninja 无任务；八个 RV 语法检查全部通过。之后保持源码冻结，未新增 probe、测试源文件或单次测试脚本。

红魔 NX729J（bf1359ff）集中执行 35 次核心运行，均符合预期退出状态：

- 主套件 711 个用例、221128 个断言，仍为 12 个失败用例、67 个失败断言、0 errors；全部有序名称、classname 后缀及失败类型/消息/文本与 fixed-inputs-v2 一致，只归一化 binary 前缀与 cpp/h 行号。
- IR default/IR_FAST=0 各 34 个用例、713 个断言通过。新契约在封口输入上确认 scalar insert 两模式的 tie source、observer 截断未来写窗口、移动移交后的表仍有效，以及最终 home/发表省略确由该输入驱动；普通 GPR 输入无 FPR 表。
- lifetime 九模式 × 两种 IR 模式通过；construction/finish-retry/retained 每种模式的注入失败仍为 5/2/1。entry 六模式 950 checks、call 六模式 233 个顶层 checks、shutdown owners/resource-cycles 两模式 105 checks 通过。
- RV 原生执行、静态 latch 的既有四模式 fixture 失败、完整 shutdown 矩阵本批未运行，不报告为通过。

同批完成默认 Function、bound-off Function、普通 Block × CoreMark/bench × 两版本共 12 次 guest 对照。默认 Function CoreMark 两版均 425 条完整有序记录、286788 host bytes，全部 shape 一致。bench 两种 Function 均 22 条、19908/19916 字节，Block 均 101 条、16468 字节，完整有序记录和 shape 一致，五个校验值正确。

bound-off Function CoreMark 为 424→425 条、286700→286792 字节，所有公共 PC 的完整记录一致；仅候选新增 0x25fb00（15 IR、92 host bytes）。相同 guest ELF 证明该 PC 在 __dtoa（起点 0x25f2d0、size 0x1017）内。Block CoreMark 为 2415→2416 条、205884→206128 字节，公共 PC 的有序 host 长度全部一致；候选增加 0x253c2b/252、0x253d30/36、0x253d42/256，缺少 0x253be8/40、0x253d39/260，均在 __d2b_D2A（起点 0x253b00、size 0x342）内。两项均属运行 roster 差异，不能把 host 总量的变化解释为同一 IR 的代码质量变化。Block 采用计划内已有 host dump，未用空 unit 记录或带 native 地址的 hash 证明等价；所有 CoreMark CRC 正确。

spill defs/loads/stores 两版一致：Function CoreMark 330/102/73、bench 161/245/150；Block CoreMark 771/360/261、bench 188/262/162。HIR pool/Inst arena 一致：Function CoreMark 632320/458752，bench 默认 519360/262144、bound-off 538560/262144；Block CoreMark 0/196608、bench 0/65536。

Function 与 Block 各采一轮四点交错 PMU，共八点，顺序为参照 2000、候选 2000、候选 12000、参照 12000；未启用 profiling/audit/host dump。退出和 CRC 正确，没有 affinity 警告。

| 路径/指标 | 斜率变化 | 截距变化 |
| --- | ---: | ---: |
| Function instructions | -0.159556% | +0.010561% |
| Function cpu-cycles | -1.509487% | +1.259336% |
| Function wall | -1.717320% | -0.750221% |
| Block instructions | +0.030879% | -0.635790% |
| Block cpu-cycles | +1.321209% | -0.651328% |
| Block wall | +1.927414% | -2.189942% |

CPU4、taskset 10、governor=walt，上限均为 1171200 kHz；Block 候选 12000 点结束后当前频率为 940800，其余逐点前后为 1171200，不是锁频测量。cpuss-0 为 30.5～34.0°C、cpu-1-0 为 31.3～34.0°C、cpu-1-6 为 30.1～31.6°C；具名传感器不是 CPU4 独占温度。单轮、运行 roster 和频率变化不能确认收益、退化或归因。截距含启动、编译及非线性，不是纯编译时间；stat 无逐事件 running 比例，不新增 IPC；保留 CoreMark 低于十秒提示，不报告正式分数。本批未重采 FEX，固定源码 Android 构建仍待补齐。

DWARF 按声明文件核对：AllocationInputs 312→320 字节，AllocationState 384→392，LinearScan 1968→1944；可选 FPRInputs 固定头 176，Tie 6、WriteRange 8、ValueEnd 4、Node 8、Publication 20、BlockRange 16。SemanticFacts 592、ValueLiveness 320、writer/ARM64 preparation 912/920、OptimizedUnit Storage 728、prepared Storage 984、Allocation 816、AllocationView 8、CompileUnit 56 均不变。逐 attempt 的密集未来写数组和每次 FPR block 的 unit-size use-end 数组已退役，但请求保留新增稀疏输入，并可能更早准备共享 resolved graph；容器容量、动态 backing、分配次数和真实峰值未一起测量，不宣称总内存下降。

最终源码快照含 407 个现存修改/新增源文件、48 个相对起点删除路径；相邻快照的 compile/fpr_coalesce.cpp 另已退役。source digest 为 e4a69944ab4cc10e830b14e265d247b524a34c0036a3fe9a06b28715f11329e7，archive SHA-256 为 725d30cf531db56b5b34c74d3d3658550877ced1100718b574fd12a4f0aedb1b。svm_fpr_inputs_v1 SHA-256 为 6eaa218b51e417392b007128afa3aebd49741449d4cca02e0acdc9f343c1606a；参照 svm_fixed_inputs_v2 为 c83d42bb43ced9738b729a6a8efab08b833523e5b03f566c674a0c62cd6b7150。

源码/archive 全体成员、九个主机/构建产物与设备候选/参照/guest/tool 身份均已核对；设备运行前后十二项哈希一致。构建、命令、JUnit、寿命/持久化、形态/符号、PMU、布局和身份记录位于 /private/tmp/swiftvm-middle-results/r3-fpr-inputs-v1-*，设备原始日志在 /data/local/tmp/svm-middle-r1/results。

本批完成 FPR 组合候选和通用物理执行的分离；固定读取别名、后续 GPR 物理家族、逐指令 scratch、旧 alloc 中的最终 FPR 契约与动作仍未全部收敛。R1 必需摘要/剩余扫描/workspace、R2 worklist/提交契约、R5/R6 轻量 actions、固定源码 FEX 及其余 R0–R7 继续保留在范围内。

## R3 固定读取输入与死物理 affinity 退役

最终批次为 r3-fixed-reads-v2，相邻参照为 r3-fpr-inputs-v1。完整 GPR/FPR 读取的语义分类已并入 ARM64 inputs.cpp 的既有 DescribeBlock 访问，公共 AllocationInputs 保存按值编号排序的 FixedRegisterRead；每行 6 字节，包含值、home、寄存器类和局部读取末端跟踪标记。GPR 复用 IsGetHostRegOperation 的零偏移条件，且宽度必须为 u64；FPR 要求零偏移和浮点返回类型。公共 allocate/fixed.cpp 按单调游标消费这些记录，保留跨 guest-home 写入、preservation、owner/end 和禁用 FPR coalescing 重试的物理检查。Block 与 Function 保持独立的区间、use-end 与 owner 语义，没有把两条分配路径合并。

必要 precolor 安装、single-home 压力降级、读取约束最终化与冲突查询也迁入公共 fixed.cpp，不包含 ARM64 头文件或 opcode 识别。原 backend/arm64/compile/fixed_registers.cpp 已删除：目标 scratch 主体迁入 compile/scratch.cpp；GPR coalescing、必要 fixed 验证与 spill-region 收尾暂归 allocation_finish.cpp。四个公共 fixed 方法、三个目标收尾方法及 DescribeInstructionScratch 共八个主体经归一化空白/相邻字符串检查均未改动。allocation_finish.cpp 仍是目标方法接缝，scratch 仍是公共声明由目标实现；后续继续收敛，不能据此宣布 R3 完成。

本批直接删除 RecipeFixedGPRAffinities、TryAllocateFixedGPR 和仅为其服务的 TryAllocatePreferred，以及两份逐 attempt 密集数组、32 个范围容器、五个专属计数及 `[svm-ra-shape-fixed]` footer，没有将死逻辑重新包装成输入家族。删除依据限定于生产的新建结果和清除 coalesced writes 后的正确性重试：fixed clobber 在区间 admission 前建立；affinity 要求来源在自身 SetHostGPR 发表处结束，Function/CFG use-end 只会延长该边界；已预着色来源不进入普通区间，其余来源必与自身发表的 inclusive clobber 相交，在任何 mapping 提交前被拒绝。起点 HEAD 2804a51b6c0477756ca081bec5ba744d152d81d8 已采用相同规则。保留 fixed-class 池、初始 fixed/preservation、实际读取 admission 与必要验证/重试。

启用 SVM_X86_PIN_EXT=3、SVM_RA_FIXED_CLASS=1 的四个真实参照运行覆盖了该策略；affinity writes 均为零，reads/hazards/evictions/copies_elided 分别为：Function CoreMark 10267/2642/56/10267，Function bench 359/207/11/359，Block CoreMark 7453/1883/62/7453，Block bench 368/160/13/368。运行数据只支持源码证明，不替代上述适用范围。原始五字段记录保留在参照日志，候选不再输出这条诊断。

v1 最终复核发现 GPR 读取分类遗漏旧 IsGetHostRegOperation 的零偏移守卫，v2 补回该条件后重新冻结、构建并验证；v2 只改变 ARM64 compile/inputs.cpp，其他源文件与 v1 完全一致。v1 的两次 ADB 连接启动失败没有启动测试程序，原始失败记录与已执行日志均保留；不计为通过，也未通过修改生产条件绕过。v1 测量不充作 v2 PMU。v2 的九个目标首次构建成功，最终 Ninja 返回 no work；八个 RV 语法检查和 DWARF 布局显式复用 v1，理由是 RV 源码、全部声明头与工具链参数逐字未变，不报告为 v2 新执行的测量。

红魔 NX729J（bf1359ff）运行最终 v2 的 36 个核心场景，退出状态全部符合预期：主套件 712 用例、221157 断言，仍有既有的 12 个失败用例/67 个失败断言、0 errors；新增 fixed-read capture 用例通过，原有有序用例和失败类型/消息/文本与相邻参照一致，只归一化 binary 前缀和源行号。IR default/IR_FAST=0 各 34 用例、721 断言通过。PIN_EXT=3 的现有两项定向契约共 39 断言通过；覆盖池策略和读取跨 home 写入后的 capture。lifetime 九模式 × 两种 IR 模式通过，construction/finish-retry/retained 注入失败各为 5/2/1；entry 六模式共 950 checks，call 六模式 233 顶层 checks，shutdown owners/resource-cycles 两模式共 105 checks 通过。RV 原生运行、静态 latch 既有四模式 fixture 失败和完整 shutdown 矩阵本批未重跑。

形态对照共十组：六组普通策略及四组 fixed-class。本次新跑十个候选，复用十个不可变参照的静态记录；参照的真实命令、日志仍指向 r3-fixed-reads-v1 和 svm_fpr_inputs_v1，没有改写为 v2 或伪造重跑。所有公共 PC 的有序 IR/host 记录一致；普通 Block 使用已有 host dump 的 PC/长度，未用空 unit 记录或含 native 地址的代码 hash 证明等价。所有 CoreMark CRC 和 bench 五个校验值正确。原始 shape 每组均因 footer 退役而不同；只排除明确退役的 `[svm-ra-shape-fixed]` 后，默认 Function CoreMark 和五组 bench 的完整 shape 相同，其余四组 CoreMark 有运行 roster 变化。

| 路径 | 有序 guest 记录数：参照→候选 | 全局 RA host bytes：参照→候选 |
| --- | ---: | ---: |
| 默认 Function CoreMark | 425→425 | 286788→286788 |
| bound-off Function CoreMark | 424→425 | 286700→286792 |
| Block CoreMark | 2415→2412 | 205944→205572 |
| fixed-class Function CoreMark | 425→424 | 286352→286260 |
| fixed-class Block CoreMark | 2412→2418 | 207308→208120 |
| 默认 Function bench | 22→22 | 19908→19908 |
| bound-off Function bench | 22→22 | 19916→19916 |
| Block bench | 101→101 | 16468→16468 |
| fixed-class Function bench | 22→22 | 20080→20080 |
| fixed-class Block bench | 101→101 | 16492→16492 |

Function 的 `[svm-unit]` 记录与全局 RA 统计范围不同；例如默认 CoreMark 的 425 条记录自身 host 长度之和为 284168，全局 RA 则为 286788，不能混用。bound-off 仅候选新增 0x25fb00（15 IR、92 bytes），fixed-class Function 仅候选缺少同一记录。普通 Block 候选缺少 0x253be8/40、0x253bf1/68、0x253d40/264；fixed-class Block 候选新增 0x253c2b/252、0x253c8e/72、0x253d42/256、0x25f929/28、0x25faf8/92、0x25fbbc/112。同一 CoreMark ELF 的 llvm-nm -n -S 逐项确认这些地址都在 __d2b_D2A（0x253b00，size 0x342）或 __dtoa（0x25f2d0，size 0x1017）内。未出现公共 PC 长度变化，不能把 roster 导致的总量变化解释为代码质量收益或退化。

十组 spill defs/loads/stores 均一致：普通 Function CoreMark 330/102/73、bench 161/245/150，普通 Block CoreMark 771/360/261、bench 188/262/162；fixed-class Function 为 775/301/225、229/259/172，fixed-class Block 为 1636/740/560、190/264/164。pool/arena 也逐组一致：Function CoreMark 632320/458752，bench 默认 519360/262144、bound-off 538560/262144；Block CoreMark 0/196608、bench 0/65536，fixed-class 不改变这些值。

最终 v2 重新采集 Function/Block 各一轮四点 PMU，共八点，逐模式顺序为参照 2000、候选 2000、候选 12000、参照 12000。SVM_JIT_CACHE 为空，关闭 profiling/audit/host dump，使用 simpleperf stat --cpu 4 -e cpu-cycles,instructions taskset 10；退出、CRC 正确，没有 affinity 警告。

| 路径/指标 | 斜率变化 | 截距变化 |
| --- | ---: | ---: |
| Function instructions | +0.016740% | +0.051755% |
| Function cpu-cycles | +0.711147% | -1.388963% |
| Function wall | +1.336133% | -2.655406% |
| Block instructions | -0.032043% | +0.214094% |
| Block cpu-cycles | -0.410412% | +3.653097% |
| Block wall | +0.446727% | +0.389666% |

逐点前后 governor=walt，CPU4 当前/最高频率均为 1171200 kHz；只是边界读数，不证明全程锁频。cpuss-0 为 30.5～34.4°C，cpu-1-0 为 30.1～34.0°C，cpu-1-6 为 29.7～31.6°C，传感器不是 CPU4 独占温度。单轮数据只作诊断，不确认性能收益/退化或归因；截距含启动、编译及非线性，不是纯编译时间；raw stat 无逐事件 running 比例，不新增 IPC。保留 CoreMark 低于十秒提示，不报告正式分数。本批未重采 FEX；固定源码的 Android FEX 与正式多轮三方 PMU 仍待补齐。

按声明路径复用 v1 的 DWARF：AllocationInputs 320→344、AllocationState 392→416、LinearScan 1944→1112、Allocation 816→776、prepared Storage 984→944 字节；FixedRegisterRead 为 6。writer/preparation 912/920、View 8、CompileUnit 56、SemanticFacts 592、ValueLiveness 320、OptimizedUnit Storage 728、PoolPolicy 20、Budget 12 不变。删除的两份数组每 attempt 原逻辑初始化量为 6×编号容量字节，并退役 32 个范围容器；新增请求保留 6×读取行数的逻辑数据与容器头。未测全部动态 capacity、分配次数和真实峰值，不宣称总内存下降。

最终源码快照为 409 个现存修改/新增源文件、48 个相对 HEAD 删除路径，另有相邻 overlay 的 fixed_registers.cpp 退役。source digest 为 213154704044e9e54ff226badd3dd0ab61738584ad4e9bdf65275546a4ce44ba，archive SHA-256 为 22f2058d5be5578b02bc94a6d244a1508d3cb2417cfd707c9071b2a3a63b920a。svm_fixed_reads_v2 SHA-256 为 585e50760e85c5d077e18c517d9e86a306851af734b46cc01af9857d554ebca4，相邻参照 svm_fpr_inputs_v1 为 6eaa218b51e417392b007128afa3aebd49741449d4cca02e0acdc9f343c1606a。全体源码/archive 成员、九个主机/构建二进制、八个设备候选及参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致。

命令、原始日志、JUnit、静态形态/ELF 符号、PMU、复用布局/语法检查及身份记录位于 /private/tmp/swiftvm-middle-results/r3-fixed-reads-v2-*；v1 及相邻参照证据保留原名。设备日志位于 /data/local/tmp/svm-middle-r1/results。没有新增临时 probe、测试脚本或兼容入口。余下 GPR 物理家族、逐指令 scratch、最终 FPR 契约/动作、R1 摘要与 workspace、R2 worklist/提交契约、R5/R6 轻量 actions/调用方和固定源码 FEX 均继续属于完整 R0–R7 目标。

## R3 逐指令 scratch 输入与预算重扫退役

本批为 r3-scratch-inputs-v1，相邻参照为 r3-fixed-reads-v2。ARM64 compile/scratch.cpp 现在只生成目标数据：DescribeInstructionScratch 改为 ARM64 命名空间下的纯函数，输入为池、FeatureSet、指令和已准备 flags demand，没有 writer 或物理 mapping 依赖。DescribeScratchInput 在 inputs.cpp 既有 DescribeBlock 访问中收集两种池阶段的需求，并累积最大 GPR/FPR 预算及 reload 上界；交接前按编号排序、合并相邻相同 profile。公共 AllocationInputs 持有这些变化点，ScratchFor 只读查询指定指令/池，不构图、扫描 IR 或调用目标函数。

公共无 spill 验证、带 spill 的逐指令检查和 reserve 升级均消费这张表。Block/Function 两份 CollectUnitBudget 重扫、公共头中的目标预算声明、LinearScan::ScratchOnlyGPRs、DemandFor 及独立 flags 引用已删除。升级最大需求仍按实际池选择，保留首次 guest-home 声明前后池的区别；Block/Function 的区间和 terminal 协议保持分开。去重 reload、Add/Sub 的旧 overlap 规则、必要 fixed 验证及正确性 reserve ladder 均保留。

spill-region 的 GPR/FPR 基础预算也改为消费同一请求输入，跨辅助函数传递 AllocationInputs，不再重新查询 ScratchBudget 或传递 flags demand。其 scratch-only 容量仍使用原有 Inst 级 clobber 规则，这与公共检查的 opcode 级容量有差别，未作错误合并。publication 局部 recolor 仍调用 ARM64 纯描述函数，旧冻结 plan 中的 scratch/emission 元数据尚未统一；allocation_finish.cpp、其余目标家族和最终 FPR 契约/动作仍待接续，不能将本批认定为完整 R3/R5。

仅修改相邻版本的 16 个现有源文件，没有新增源文件、兼容入口、probe 或测试脚本。现有 IR ownership 用例改为接收目标显式生成的输入；新增一项契约在八个 GPR 的池中声明更宽的 scratch 需求，验证移动请求后的表地址/查询有效、越界拒绝，并确认实际分配发生 spill 且每条指令具备所声明的 headroom。它验证数据确实驱动分配，没有为测试添加生产分支。

九个 Android 目标首次构建成功，最终 Ninja 为 no work；八个 RV 语法检查本批实际重新执行并通过。红魔 bf1359ff 的 36 个核心场景全部符合预期退出状态：主套件 712 用例、221182 断言，仍为 12 个失败用例、67 个失败断言、0 errors；有序用例、classname 后缀和全部失败类型/消息/文本与相邻版本一致，只归一化 binary 前缀与 cpp/cc/h 行号。总断言比参照多 25 个成功断言，未将汇总计数报告为完全相等。IR default/IR_FAST=0 各 35 用例、736 断言通过，PIN_EXT=3 定向契约仍为两用例/39 断言通过。lifetime 九模式 × 两种 IR 模式通过，逐模式检查数及注入失败与参照相同；entry 六模式 950 checks、call 六模式 233 顶层 checks（子进程阶段分别核对并归一化 PID）、shutdown owners/resource-cycles 两模式 105 checks 通过。本批未重跑 RV 原生、静态 latch 四个既有失败模式或完整 shutdown 矩阵。

本次新跑十个候选 guest 点，复用相邻不可变二进制的十个静态参照记录；真实参照命令/日志仍指向 fixed-reads-v2 的 candidate 运行，未改写历史文件名。两版诊断 schema 相同，没有忽略 shape 字段。默认和 bound-off Function CoreMark、fixed-class Function CoreMark 及五组 bench 的全部有序记录、完整 shape 相等。普通和 fixed-class Block CoreMark 的公共 PC 有序 host 长度均相等，差别只在运行 roster；十组 spill defs/loads/stores、HIR pool/Inst arena 全部相等，CoreMark CRC 与 bench 五个校验值正确。

| 路径 | guest 记录数：参照→候选 | 全局 RA host bytes：参照→候选 |
| --- | ---: | ---: |
| 默认 Function CoreMark | 425→425 | 286788→286788 |
| bound-off Function CoreMark | 425→425 | 286792→286792 |
| fixed-class Function CoreMark | 424→424 | 286260→286260 |
| Block CoreMark | 2412→2416 | 205572→206164 |
| fixed-class Block CoreMark | 2418→2413 | 208120→207320 |

五组 bench 的记录数/全局字节依次为 22/19908、22/19916、101/16468、22/20080、101/16492，均与参照相同。Function guest 记录的长度之和与全局 RA 统计范围不同，仍分别保留。普通 Block 候选增加 0x253c2b/252、0x253c8e/72、0x253d42/256、0x25f929/28、0x25faf8/92、0x25fb00/88、0x25fb10/72、0x25fbbc/112，缺少 0x25f951/168、0x25f963/92、0x25f966/92、0x25f971/28。fixed-class Block 候选增加 0x25fb00/88、0x25fb10/72，缺少 0x253c2b/252、0x253c8e/72、0x253d42/256 及同样四个 0x25f951～0x25f971 记录。同一 guest ELF 的符号表逐项确认差别均在 __d2b_D2A 或 __dtoa，不能把全局 host 总量变化视为相同 IR 的代码质量变化。

本批对最终版本重新采集八个 PMU 点，Function/Block 各一轮四点，顺序为参照 2000、候选 2000、候选 12000、参照 12000。使用 simpleperf stat --cpu 4 -e cpu-cycles,instructions taskset 10，SVM_JIT_CACHE 为空，关闭 profiling/audit/host dump；所有退出/CRC 正确，没有 affinity 警告。

| 路径/指标 | 斜率变化 | 截距变化 |
| --- | ---: | ---: |
| Function instructions | +0.001634% | +1.650021% |
| Function cpu-cycles | +0.157420% | +0.727019% |
| Function wall | +0.455346% | +0.754508% |
| Block instructions | +0.002366% | +1.884127% |
| Block cpu-cycles | +0.648472% | +1.305443% |
| Block wall | +0.585507% | +2.179942% |

governor=walt，逐点最高频率均为 1171200 kHz；当前频率边界读数包括 940800 和 1171200，未锁频。cpuss-0 为 30.5～34.0°C、cpu-1-0 为 30.5～33.6°C、cpu-1-6 为 29.3～31.6°C。单轮测量只作诊断；指令截距有所增加，后续关注输入生成成本，但这些数据不能单独确认收益/退化或归因。截距包含启动、编译及非线性，不是纯编译时间；没有逐事件 running 比例，不新增 IPC；保留低于十秒的 CoreMark 提示，不报告正式分数。固定源码的 Android FEX 和正式多轮三方 PMU 仍待补齐，本批未重采 FEX。

本批候选 DWARF 实测 AllocationInputs 344→376、AllocationState 416→448、LinearScan 1112→1104 字节；PoolPolicy 仍为 20，ScratchNeed 2、ScratchProfile 6、ScratchSite 8。参照布局明确复用 fixed-reads-v1 的候选 DWARF：fixed-reads-v2 仅改变 ARM64 inputs.cpp 的读取谓词，声明头/工具链相同。其他已记录的结果/owner 布局未变。变化点逻辑数据为每行 8 字节，但生成时先收集逐指令记录，压缩后 vector capacity 不保证释放；新增请求容器、动态 backing、分配次数和峰值未整体测量，不报告总内存下降。

最终 source digest 为 d1955b32a8f5dcad2bdb643efa84366c05bfc59a5a218ba326dbfa1b2820212a，archive SHA-256 为 d4afec172c5b9fa60d1f80a7d32630b218d0395d33483ebc2682f7a3c53e23c0，快照仍为 409 个现存修改/新增源文件、48 个相对 HEAD 删除路径。svm_scratch_inputs_v1 SHA-256 为 22bda8272a8fdcd19f25f7aa77c83f1c8ec517c2317f2bf0ec364b8ae9374d44，参照 svm_fixed_reads_v2 为 585e50760e85c5d077e18c517d9e86a306851af734b46cc01af9857d554ebca4。源码/archive 全体成员、九个主机/构建产物、八个设备候选及参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致。

构建、精确命令、JUnit、生命周期/持久化、RV 语法、形态/ELF 符号、PMU、布局和身份记录位于 /private/tmp/swiftvm-middle-results/r3-scratch-inputs-v1-*；复用参照保持原路径，设备日志仍在 /data/local/tmp/svm-middle-r1/results。R1 必需摘要/workspace、R2 worklist/提交契约、剩余 GPR/FPR 目标家族、R5/R6 轻量 actions/调用方、R7 清理及完整 R0–R7 验收继续保留在范围内。

## R3 固定 GPR 发表候选与公共 home 准入

本批最终为 r3-gpr-publications-v3，相邻参照为 r3-scratch-inputs-v1。原 gpr_copy.cpp 的 FusePinnedWriteChains 已退役：ARM64 gpr_inputs.{h,cpp} 只做封口 IR 的语义匹配，公共 allocate/fixed.cpp 的 ReuseFixedPublications 只做实际 fixed-home 冲突检查和 mapping 提交。AllocationInputs 持有有序 FixedGPRPublication，记录块、发表值、发表点、home 和可选 producer；请求移交后保持 const，所有物理重试复用，没有 target callback、writer 或 emission 对象。

inputs.cpp 在既有 DescribeBlock 访问中收集 ZeroExtend32To64 的稀疏位置及完整交换标记，再于块末处理候选。存在完整交换时不执行候选窗口检查；没有另加整块筛选。gpr_inputs.cpp 保留原始重复 SSA/pseudo 边的计数、全局 GetUses 与块内计数相等、首个真实 consumer、source/target 覆写和观察窗口、level-2 多使用 capture 例外等语义。顺序保持为原成员遍历顺序和块内扩展顺序，Block/Function 没有合并。旧 no_fuse_pin_writes 和实际 level-2 池条件分别快照到两个池策略，声明后按真实池选择。

公共核心先检查发表值的既有 fixed 声明和窗口冲突，通过才写入该值；随后才检查和提交可选 producer，不能在发表值被拒后单独重映射 producer。调用次序仍在 spill-region 准备之后、其余 target coalescing 之前。CoalescingValueUsesStayInBlock 的原主体也已迁回公共 fixed.cpp，经归一化空白确认未变；CoalescingQueries 的 directly_feeds_memory 从未被读取，四个生产处均传 nullptr，字段及空实参已删除。剩余 GPR 读写、width/low32、最终 FPR 契约/动作仍有目标物理实现和回调，未把首个家族完成等同于完整 R3。

相邻版本共变动 15 个源文件，其中新增两个目标纯输入文件，没有新增测试源文件、脚本、probe 或兼容入口。现有 IR 契约增加四种形态：正常复用、清空输入后取消复用、target 覆写后继续使用旧 capture、较晚出现的完整交换拒绝整块候选；同时检查输入移动后的有效性。初始 fixed 输入在该用例中明确清空，另外关闭 guest-access/width 家族，以确认实际映射由这一输入驱动。

v1 构建因新文件遗漏 ResolveBitCastSource 的直接声明头而失败，补齐 facts.h 后 v2 九目标构建成功。v2 的新契约夹具错误地向 PolicyFor 传入 ARM64 preparation 补充保留寄存器之前的原始池，严格身份检查拒绝；其余原有 IR 用例未新增失败。v3 仅将夹具改为查询 writer 的实际池，生产检查没有放宽。v3 九目标构建成功，只有 IR 契约二进制改变，其余八个构建产物与 v2 逐字相同；最终 Ninja 为 no work。v2 已实际通过的八个 RV 语法检查明确复用：v3 仅改变 ir_contract_test.cpp，生产/RV 源码、声明头及工具链参数不变。两个早期问题及原始失败记录保留，不计为最终通过。

红魔 bf1359ff 上最终 v3 的 36 个核心场景全部符合预期退出状态。主套件 712 用例、221085 断言，仍为 12 个失败用例、67 个失败断言、0 errors；有序用例、classname 后缀及全部失败类型/消息/文本与相邻版本一致，只归一化 binary 前缀和 cpp/cc/h 行号。两次主套件随机种子分别为 785489973、2800038291，本轮总成功断言比参照少 97，不报告汇总计数完全相等，也不据此归因。IR default/IR_FAST=0 各 36 用例、759 断言通过；PIN_EXT=3 定向契约仍为两用例/39 断言通过。lifetime 九模式 × 两种 IR 模式的逐模式检查数及注入失败与参照相同；entry 六模式 950 checks、call 六模式 233 顶层 checks（子进程阶段单独归一化 PID 核对）、shutdown owners/resource-cycles 两模式 105 checks 通过。RV 原生、静态 latch 四个既有失败模式及完整 shutdown 矩阵本批未重跑。

本次新跑十个候选 guest 点，复用相邻不可变参照的十个静态记录；真实参照命令/日志仍指向 scratch-inputs-v1 的 candidate 运行，没有改写历史名称。两版诊断 schema 相同，不忽略任何 shape 字段。十组公共 PC 的有序 IR/host 或 Block host 长度均相等，spill defs/loads/stores 和 HIR pool/Inst arena 均相等，CoreMark CRC 与 bench 五个校验值正确。默认/bound-off Function CoreMark 及五组 bench 的完整有序记录和完整 shape 相等，另外三组 CoreMark 有运行 roster 差别。

| 路径 | guest 记录数：参照→候选 | 全局 RA host bytes：参照→候选 |
| --- | ---: | ---: |
| 默认 Function CoreMark | 425→425 | 286788→286788 |
| bound-off Function CoreMark | 425→425 | 286792→286792 |
| Block CoreMark | 2416→2416 | 206164→206128 |
| fixed-class Function CoreMark | 424→425 | 286260→286352 |
| fixed-class Block CoreMark | 2413→2418 | 207320→207920 |

五组 bench 的记录数/全局字节依次为 22/19908、22/19916、101/16468、22/20080、101/16492，均与参照相同。普通 Block 候选增加 0x253d30/36，缺少 0x253c8e/72；fixed-class Function 增加 0x25fb00（15 IR、92 bytes）；fixed-class Block 增加 0x253be8/48、0x253bf1/68、0x253d40/264、0x25f951/168、0x25f963/92、0x25f966/92、0x25f971/28，缺少 0x25fb00/88、0x25fb10/72。同一 CoreMark ELF 的符号表逐项确认都位于 __d2b_D2A（0x253b00，size 0x342）或 __dtoa（0x25f2d0，size 0x1017），不将全局总量变化解释为相同 IR 的代码质量变化。Function guest 记录的长度之和与全局 RA 统计范围仍分开保留；Block 没有用空 unit 表或含 native 地址的 hash 证明等价。

最终版本重新采集八个 PMU 点，Function/Block 各一轮四点，顺序为参照 2000、候选 2000、候选 12000、参照 12000。使用 simpleperf stat --cpu 4 -e cpu-cycles,instructions taskset 10，SVM_JIT_CACHE 为空，关闭 profiling/audit/host dump；退出和 CRC 全部正确，没有 affinity 警告。

| 路径/指标 | 斜率变化 | 截距变化 |
| --- | ---: | ---: |
| Function instructions | +0.121689% | +0.003797% |
| Function cpu-cycles | -0.506591% | -0.067694% |
| Function wall | -2.124004% | +1.254692% |
| Block instructions | +0.075965% | -0.300157% |
| Block cpu-cycles | +0.876225% | -1.688160% |
| Block wall | +0.752050% | +0.096325% |

governor=walt，最高频率均为 1171200 kHz；逐点当前频率边界读数包括 940800、1056000 和 1171200，未锁频。cpuss-0 为 30.5～33.2°C、cpu-1-0 为 31.3～34.0°C、cpu-1-6 为 29.7～31.3°C。单轮、运行 roster 和频率波动不能确认收益/退化或归因；截距含启动、编译和非线性，不是纯编译时间；raw stat 无逐事件 running 比例，不新增 IPC。保留 CoreMark 低于十秒提示，不报告正式分数。固定源码 Android FEX 和正式多轮三方 PMU 仍待补齐，本批未重采 FEX。

候选 DWARF 实测 FixedGPRPublication 为 16 字节、AllocationInputs 376→400、AllocationState 448→472；PoolPolicy 仍为 20，LinearScan 仍为 1104，CoalescingQueries 32→24。其余已记录结果/owner 布局不变。参照复用 scratch-inputs-v1 的实测布局，另外在同一不可变参照二进制中补测 CoalescingQueries。请求增加稀疏候选和容器头，表行含封口块借用；没有测全部动态 capacity、分配次数或真实峰值，不报告总内存下降。

最终 source digest 为 0bc9088e5b729298ea22bb2f53aa7af4a2cfac1df47dece005a9c0d95c8076bf，archive SHA-256 为 8e681fd8cdc50c0d8061e7b3366f8699617af063a2108939b74c6dbf2732ea25；快照为 411 个现存修改/新增源文件、48 个相对 HEAD 删除路径。svm_gpr_publications_v3 SHA-256 为 43b8a7136ea53667c2a9fe1d64a1a7cdfb0fc25f780df033e7183e67c0c57cd0，相邻参照 svm_scratch_inputs_v1 为 22bda8272a8fdcd19f25f7aa77c83f1c8ec517c2317f2bf0ec364b8ae9374d44。源码/archive 全体成员、九个主机/构建二进制、八个设备候选及参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致。

构建、精确命令、JUnit、生命周期/持久化、RV 复用依据、边界源码复核、形态/ELF 符号、PMU、布局及身份记录位于 /private/tmp/swiftvm-middle-results/r3-gpr-publications-v3-*；v1 构建失败和 v2 夹具失败保持原名。设备日志位于 /data/local/tmp/svm-middle-r1/results。余下 GPR/FPR 目标家族、R1 必需摘要/workspace、R2 worklist/提交契约、R5/R6 轻量 actions/调用方、R7 清理与完整 R0–R7 验收继续推进。

## R3 窄 GPR 读取、low32 copy 与可选 GPR 输入

本批最终为 r3-gpr-reads-copies-v3，相邻参照为 r3-gpr-publications-v3。固定发表、窄 GPR 读取复用和 low32 copy 统一由可选 const GPRInputs 持有；AllocationInputs 不再内嵌固定发表 vector，空家族不保留 payload。新增公共 allocate/gpr_inputs.{h,cpp} 保存及查询稀疏记录，allocate/gpr.cpp 负责物理执行；原目标 gpr_copy.cpp、两个旧 free-function 声明及对应 target 回调生产处/CMake 项已删除。目标 gpr_home.cpp 也不再实现窄读取 matcher。两种分配入口和原成员遍历顺序保持独立。

ARM64 GPRInputBuilder 在既有输入访问中维护每个 home 的最近发表与 observer 状态，收集低位视图、邻接 wrapper、消费规则和 clobber，再处理固定发表的稀疏工作表。读取后的覆写检查使用本块原始 SetHostGPR 列表，保留不可达块中的写入，并且不会把下一块的写入算进本块虚拟 use-end。copy clobber 保留原 inclusive 区间，合并默认固定写掩码、guest 发表和 HelperCallContract 的精确掩码；没有用另一种 scratch-only 策略替换它。observer、clobber 与读取的固定 class 标记沿用实际 Allocation 的 FeatureSet，分配策略沿用请求 FeatureSet，两个实际池阶段分别表达。

公共 copy 仍依次尝试 live source view、final-view transfer、final-source recolor、邻接 wrapper。实际 GPR/MEM、home、fixed/alias、live end、第三方重叠、mask 与 scratch 检查留在公共层，拒绝时恢复先前 mask，通过后才提交 alias。读取的 width-owner 是否提交、发表是否省略、owner home 和 high-zero 仍按实际结果判断；静态 high_zero=false 不会提前丢弃可由已提交 owner 证明的候选。公共实现没有 ARM64 include、opcode/helper/observer 匹配或 target callback。

copy 消费者直接借用唯一 BlockAnalysisIndex 的原始 use span：源码核对 GetValues 本身即由 ForEachValue 收集，同一图覆盖 BitCast、Lambda/Params 和重复边。消费规则只保存 site 标记，没有复制第二份 consumer 图。邻接 wrapper 使用 u32 sentinel，位置 65535 仍可表达；所有块/指针借用在请求结束时释放，不进入 emission。width-root/W-write 两个纯谓词的原主体迁入目标输入文件，经归一化空白确认未变；IsPinnedCoalesceTarget/Producer/Observer 的单纯转发别名改用规范谓词并删除。剩余 GPR 写入、pinned W-view、width 和最终 FPR 契约/动作继续接续，未将本批视为完整 R3。

相邻版本变动 17 个现有/新增源文件，新增三个公共源/头文件，并退役旧 overlay 的 gpr_copy.cpp；没有新增测试源文件、脚本或 probe。现有 IR 契约增加正常读取/清空读取输入/覆写后 capture、正常 low32 copy/清空 copy 输入五种形态，检查真实复用标记随候选输入变化、请求移动后 payload 地址有效，以及普通无候选输入保持为空。原固定发表契约改为访问可选 const payload；测试修改候选时复制自己的输入表，不引入生产可变别名。

v1 构建暴露 SetActiveRegs 只复制 mask 却要求可变引用的问题，接口改为按值接收两个小 mask；v2 清理聚合头后暴露 HIRFunction 的隐式定义依赖，改为直接包含 hir_builder.h。两次失败都发生在设备测试开始之前，日志和源码快照保留。最终 v3 九个 Android 目标构建成功，最终 Ninja 为 no work；八个 RV 语法检查本批实际重新执行并通过，没有借用旧头文件结果。

红魔 bf1359ff 上最终 v3 的 36 个核心场景全部符合预期退出状态。主套件 712 用例、221119 断言，仍为既有 12 个失败用例、67 个失败断言、0 errors；有序用例、classname 后缀和失败类型/消息/文本与相邻参照一致，只归一化 binary 前缀及 cpp/cc/h 行号。随机种子为 817971020，参照为 785489973，成功断言数多 34，不报告汇总计数完全相等，也不据此归因。IR default/IR_FAST=0 各 37 用例、779 断言通过，PIN_EXT=3 定向契约两用例/39 断言通过。lifetime 九模式 × 两种 IR 模式的检查数/注入失败逐项与参照相同；entry 六模式 950 checks、call 六模式 233 顶层 checks（子进程阶段归一化 PID 单独核对）、shutdown owners/resource-cycles 两模式 105 checks 通过。RV 原生、静态 latch 四个既有失败模式及完整 shutdown 矩阵本批未重跑。

本次新跑十个候选 guest 点，复用相邻不可变参照的十个静态形态记录，保留原始 publications-v3 candidate 命令和日志路径，没有改写历史名称。十组公共 PC 的有序 IR/host 或 Block host 长度全部相等；spill defs/loads/stores、HIR pool/Inst arena 均相等，CoreMark CRC 和 bench 五个校验值正确。五组 bench 的完整有序记录和完整 shape 相同，五组 CoreMark 有运行 roster 差异；两版 schema 相同，没有排除 shape 字段。

| 路径 | guest 记录数：参照→候选 | 全局 RA host bytes：参照→候选 |
| --- | ---: | ---: |
| 默认 Function CoreMark | 425→424 | 286788→286696 |
| bound-off Function CoreMark | 425→424 | 286792→286700 |
| Block CoreMark | 2416→2416 | 206128→206164 |
| fixed-class Function CoreMark | 425→424 | 286352→286260 |
| fixed-class Block CoreMark | 2418→2412 | 207920→207308 |

五组 bench 的记录数/全局字节依次为 22/19908、22/19916、101/16468、22/20080、101/16492。三组 Function CoreMark 的候选均缺少 0x25fb00（15 IR、92 bytes）；普通 Block 增加 0x253c8e/72，缺少 0x253d30/36；fixed-class Block 缺少 0x253be8/48、0x253bf1/68、0x253d40/264、0x25f929/28、0x25faf8/92、0x25fbbc/112。同一 guest ELF 的 llvm-nm -n -S 逐项确认均在 __d2b_D2A（0x253b00，size 0x342）或 __dtoa（0x25f2d0，size 0x1017）内，不能把总量变化当作同一 IR 的代码质量变化。Function guest 记录之和与全局 RA 范围分开；Block 使用已有 host dump 的 PC/长度，不依赖空 unit 表或带 native 地址的 hash。

最终版本新采八个 PMU 点，Function/Block 各一轮四点，顺序为参照 2000、候选 2000、候选 12000、参照 12000。使用 simpleperf stat --cpu 4 -e cpu-cycles,instructions taskset 10，SVM_JIT_CACHE 为空，关闭 profiling/audit/host dump；退出和 CRC 正确，无 affinity 警告。

| 路径/指标 | 斜率变化 | 截距变化 |
| --- | ---: | ---: |
| Function instructions | +0.142537% | -0.094024% |
| Function cpu-cycles | -0.436579% | +0.016140% |
| Function wall | -2.106102% | +2.423261% |
| Block instructions | -0.006340% | +0.186047% |
| Block cpu-cycles | -0.583629% | +0.041827% |
| Block wall | +0.325925% | -4.002759% |

governor=walt，逐点当前/最高频率边界读数均为 1171200 kHz，但不证明全程锁频。cpuss-0 为 30.5～33.2°C、cpu-1-0 为 30.9～32.8°C、cpu-1-6 为 29.3～30.9°C。单轮、运行 roster 和边界条件不能确认收益/退化或归因；截距含启动、编译和非线性，不是纯编译时间；缺少逐事件 running 比例，不新增 IPC。保留 CoreMark 低于十秒提示，不报告正式分数。本批未重采 FEX，固定源码 Android FEX 和正式多轮三方 PMU 仍待补齐。

候选 DWARF 按声明文件核对：AllocationInputs 400→392、AllocationState 472→464 字节，PoolPolicy 20→24，LinearScan 仍为 1104；可选 GPRInputs 头为 144，ReadReuse/Low32Copy 各 16、GPR BlockRange 32、Clobber 8、ConsumerRule 4。GPRInputBuilder 对象布局为 768，不等于编译器生成的完整栈帧或调用峰值。其余已记录结果/owner 布局不变；参照使用 publications-v3 的实测 DWARF。请求头缩小同时增加可选表及稀疏记录，动态 capacity、分配次数与真实峰值未整体测量，不报告总内存下降。

最终 source digest 为 316f7ec1a818d66ca1caa8364d773934385679e4f86747670b01bd176131d358，archive SHA-256 为 4cd05937419690cc750c7e55fd241a88ee806e3b063798902ad7631a0de84417；快照含 413 个现存修改/新增源文件、48 个相对 HEAD 删除路径，另有相邻 overlay 的 gpr_copy.cpp 退役。svm_gpr_reads_copies_v3 SHA-256 为 30f2440b875f597485cada069e43c8c3120b7875685162cd9e1a234b9ca328ef，参照 svm_gpr_publications_v3 为 43b8a7136ea53667c2a9fe1d64a1a7cdfb0fc25f780df033e7183e67c0c57cd0。全部源码/archive 成员、九个主机/构建产物、八个设备候选及参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致。

构建、精确命令、JUnit、生命周期/持久化、RV 语法、边界源码复核、形态/ELF 符号、PMU、布局与身份记录位于 /private/tmp/swiftvm-middle-results/r3-gpr-reads-copies-v3-*；两个早期构建失败保持 v1/v2 原名。设备日志位于 /data/local/tmp/svm-middle-r1/results。继续处理 GPR 写入/pinned W-view/width、最终 FPR 契约/动作、R1 必需摘要/workspace、R2 worklist/提交契约、R5/R6 轻量 actions/调用方、R7 清理及完整 R0–R7 验收。

## R3 pinned W-view 与 width 家族输入化及公共物理执行

本批最终为 r3-gpr-widths-v2，相邻参照为 r3-gpr-reads-copies-v3。pinned W-view、width owner 冻结/事务验证、LONG 链和 width bridge 的物理实现迁入公共 allocate/width.cpp；新增 width_inputs.h 保存可选语义 witness。旧目标 gpr_widths.cpp、对应构建项/free-function 声明和 gpr_home.cpp 内的 pinned W-view 实现已删除。gpr_home.cpp 仍保留 GPR 写入物理家族及 baseline/事务编排，allocation_finish.cpp 的目标收尾也尚在，未将本批视为完整 R3。

low32 copy 的独立输入记录改为共享 ValueView，不保留旧类型/字段别名。原 low32 四策略继续只消费 Low32 role；width 消费 WidthBridge role，pinned W-view 借用同一低位视图。known-W、U32 capture、recolor 许可和实际 FeatureSet 对应的两种池 fixed-class 标记在目标输入阶段准备，保留来源 Value 的宽度语义；指针进入公共层后只用于相同值 ID 的物理查询。WidthInputs 为可选 const owner，空发表家族不分配；BlockRange 同时指明各稀疏表的本块范围。

GPRInputBuilder 在既有目标指令访问中收集 owner/high-zero、ALU/U32 consumer、W-view 发表和 raw width 发表 witness，块末处理长链稀疏步骤。W-alpha 后置保护仍保留全部 raw SetHostGPR，含 partial 与非 fixed home 写入；没有用仅完整发表的候选替代它。普通原始发表来源范围复用 AllocationInputs.publications，公共层不重新匹配 SetHostGPR/BitExtract/ZeroExtend 或 helper opcode。

LONG 的前驱输入保留所有可能的先前步骤。公共层先按实际 produced/stored GPR 与 home 过滤，随后才在过滤后的步骤序列中判断相邻关系、闭合/安全间隙及最少 32 步；没有把原始语义相邻关系冒充物理相邻关系。producer 的 low-extract/last-use 资格提前准备，source 是否实际 GPR 留在执行时检查。LONG bridge 标记改为排序去重的稀疏 ID 集合，删除原来每个块分配整个 unit 指令数量的 u8 数组；并未删除所有逐 unit 分配，owner 事务验证的 committed-store 表仍在。

需要这些家族时显式准备唯一 BlockAnalysisIndex 的 resolved-use 视图，W-view 使用 occurrence 总数和 ALU site 规则。BitCast 本身的直接消费边、其后解析到相同 root 的消费边及重复输入次数均保留，等价于旧实现对每个原始 operand 做 ResolveBitCastSource；没有复制第二份 use 图。bridge/consumer 的实际 home、source 最后使用、第三方存活重叠、foreign raw 发表、scratch、dirty masks 和旧 fixed/ordinary mapping 回滚继续留在公共分配阶段。公共 width.cpp 不含 ARM64 include、opcode matcher 或 target callback。

baseline 写入/W-view/读取、baseline LONG、AllocationEdit、owner 冻结、写入/W-view、LONG/width、新省略点门槛和整块 owner 验证/提交保持原顺序；region 门槛仍为 10、Block 为 1。guest-access 编排继续使用 GetHIRBlocks，独立 width 家族继续使用 GetHIRBlocksRPO，Block 入口单独处理。事务验证保留原先对全部 committed owner 的检查范围，没有借迁移改变其语义。

本批相邻变动 16 个现存/新增源文件，新增两个公共源/头文件并退役一个旧 overlay 文件；源码快照为 414 个现存修改/新增源文件、48 个相对 HEAD 删除路径。没有新增测试文件、测试脚本或 probe。现有 IR 契约增加 width views 输入驱动物理决定的用例，检查正常候选/清除 WidthBridge role 后的实际标记、anchor、移动请求后的 payload 地址及无发表时可选 WidthInputs 为空；原读取/copy/固定发表契约采用测试自己的深拷贝修改 const 输入。

v1 九个 Android 目标构建和八个 RV 语法检查通过，但首个 IR 场景发现新的 pinned-W matcher 在筛选 opcode 前对 void 返回类型求宽度，产生一项 error；后续 35 个场景未执行，未作通过记录。v2 改为先筛选原 ALU whitelist，再检查 U32 宽度。v1 源码、构建产物、JUnit 和设备日志保留原名；最终 v2 重新构建九个目标，八个 RV 语法检查实际重跑通过，最终 Ninja 为 no work。此后没有修改源码。

红魔 bf1359ff 上 v2 的 36 个核心场景全部符合预期退出状态。IR default/IR_FAST=0 各 38 用例、803 断言通过；PIN_EXT=3 定向契约两用例/39 断言通过。主套件仍为 712 用例、12 个失败用例、67 个失败断言、0 errors；有序用例、classname 后缀和失败类型/消息/文本与参照相同，只归一化 binary 前缀与 cpp/cc/h 行号。候选/参照种子为 2948248232/817971020，总断言 220932/221119，少 187；不报告汇总数相等，也不据此归因。lifetime 九模式 × 两种 IR 模式的检查数/注入失败逐项相同；entry 950、call 233、shutdown 两模式 105 项检查相同。RV 原生执行、static-latch 四个既有失败和完整 shutdown 矩阵不在本次执行范围。

十组候选形态运行均为本批实测，十组参照复用相邻版本当时的候选日志，保留实际命令、文件名及复用依据；PMU 没有复用。Function 默认/bound-decode-off/fixed-class 的 CoreMark 三组和五组 bench，共八组完整有序记录/原始 shape 相同；两组 Block CoreMark 的 roster 有差异，按每个共同 PC 分组比较的出现记录及大小/IR 签名相同，spill 计数、pool/arena、CRC/checksum 均相同。两组在格式化区间内的共同 PC 出现顺序也有变化；去掉这两个已验证符号区间后，十组完整有序记录均相同。差异经相同 guest ELF 的 llvm-nm 实测，只落在 __d2b_D2A（0x253b00，大小 0x342）和 __dtoa（0x25f2d0，大小 0x1017）内部；不称作全 raw shape 相同，也不将全局 host_bytes 差异归为优化收益。

| 两组有 roster 差异的形态 | 记录数：参照 → 候选 | 全局 host_bytes：参照 → 候选 |
| --- | --- | --- |
| Block CoreMark | 2416 → 2417 | 206164 → 206104 |
| fixed-class Block CoreMark | 2412 → 2416 | 207308 → 207900 |

其余八组的记录数/host_bytes 均相等：Function CoreMark 默认 424/286696、bound-decode-off 424/286700、fixed-class 424/286260；bench 默认 22/19908、bound-decode-off 22/19916、Block 101/16468、fixed-class Function 22/20080、fixed-class Block 101/16492。两组完整 roster 的新增/缺少 PC 和原始日志保存在 shape-summary.json，未丢弃格式化路径差异。

重新采集 Function 与 SVM_FUNC_BASE=0 的实际 Block 两模式各四个 PMU 点，顺序均为 reference-2000、candidate-2000、candidate-12000、reference-12000。simpleperf 的 instructions/cpu-cycles、taskset 10、CPU4、完整命令/UTC 起止/退出码及每点前后六项频率/温度条件均保留；采集不带审计或 host dump。

| 单轮两点拟合：候选相对参照 | instructions 斜率 / 截距 | cycles 斜率 / 截距 | wall 斜率 / 截距 |
| --- | --- | --- | --- |
| Function | −0.143389% / +0.371718% | −2.325653% / +6.355793% | −0.507039% / +4.432091% |
| Block | −0.011542% / +0.218544% | −0.908158% / +3.816331% | −0.153362% / +1.652038% |

这些是单轮诊断，不能确定收益或因果，Function cycles 截距上升也需正式多轮比较验证；截距包含运行时工作，不是纯编译成本。边界 governor 均为 walt、current/max 均为 1171200 kHz，仅为边界观察而非锁频证据；cpuss-0 30.1–33.6°C、cpu1-0 31.3–33.2°C、cpu1-6 29.3–31.3°C。原始输出没有逐事件有效运行比例，不计算 IPC。两个工作量 CRC 均为 0x4983/0xd340，保留短于 10 秒的时长提示，不作为正式 CoreMark 分数。固定源码 FEX 和正式三方多轮 PMU 尚未完成。

DWARF 按声明文件核对：AllocationInputs 392、AllocationState 464、PoolPolicy 24、LinearScan 1104 字节不变；GPRInputs 144→176、GPR BlockRange 32→56、共享 ValueView 16、ReadReuse 16、WPublication 6、WidthInputs 104、width Publication 24、Step 20、Clobber 8、ConsumerRule 4。GPRInputBuilder 对象布局 768→792，不等于完整编译器栈帧/峰值。Allocation 776、writer 912、ARM64 preparation 920、prepared Storage 944、View 8、CompileUnit 56、SemanticFacts 592、ValueLiveness 320、optimized Storage 728 均不变。稀疏标记减少与可选表增加同时存在；未整体测量 capacity、分配次数与峰值，不报告总内存下降。

最终 source digest 为 97d6295fb9d9cd52c8da3666f14cd7af5c8263e7d9201438c8ca4bd79e22b52f，archive SHA-256 为 1b5b3438e8ec8b14d8a5dafb8343a5da44f287fb51ca0014a92c37f76e449a59；digest 的精确定义写入 manifest。svm_gpr_widths_v2 SHA-256 为 27be713f8f4d24f7e84674707e363df63940e4ec1bfa66620db72c20e29bc606，相邻参照 svm_gpr_reads_copies_v3 为 30f2440b875f597485cada069e43c8c3120b7875685162cd9e1a234b9ca328ef。全部源码/archive 成员、九个主机/构建二进制、八个设备候选及参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致。

构建、精确命令、JUnit、生命周期/持久化、RV 语法、源码边界复核、形态/ELF 符号、PMU、布局和身份记录位于 /private/tmp/swiftvm-middle-results/r3-gpr-widths-v2-*；设备日志仍位于 /data/local/tmp/svm-middle-r1/results。quality-gate 的本批检查全部通过，full_goal_complete=false、formal_three_way_performance_accepted=false。继续接续 GPR 写入/目标编排与收尾、最终 FPR 契约/动作、R1 必需摘要/workspace、R2 worklist/提交契约、R5/R6 轻量 actions/调用方、R7 清理和完整 R0–R7 验收。

## R3 GPR 写入与完整 coalescing 编排公共化

本批为 r3-gpr-writes-v1，相邻参照为 r3-gpr-widths-v2。新增公共 allocate/gpr_write_inputs.h 与 gpr_writes.cpp，ARM64 GPRInputBuilder 在原目标输入访问中收集 raw capture、候选发表和 site 规则，块末处理稀疏发表表。公共分配器消费可选 const GPRWriteInputs，负责实际映射、保留/owner、冲突、component、scratch 与提交。原 gpr_home.cpp、allocation_rules.h、GuestGPRMappedTo/HasGuestGPRTargetConflict 公开 helper 和最后一套 CoalescingQueries 已删除，没有保留兼容入口。

原 target FinishTargetCoalescing 改为公共 FinishCoalescing，GPR baseline/width、LONG、low32、FPR 与固定读取最终化的完整顺序归公共 allocate/coalesce.cpp。原该路径下的 AllocationEdit::RecolorGPR 主体逐字节迁至 recolor.cpp，保持窗口、scratch 和拒绝恢复算法不变；新旧文件没有同时实现同一个原语。target allocation_finish.cpp 仍定义 VerifyTargetConstraints、BuildSpillReloads 两项公共扫描器方法，这是剩余迁移接缝，未把完整 R3 或全部 allocator 收敛宣布为完成。spill_reload.h 只保留 spill-region 的两项类型化声明，cpp 直接包含 writer/HIR 定义，不再借用旧聚合头和其 using namespace。

Write 记录分别保存 baseline 与 width 模式的静态准入：tentative root、最后/持续发表、内层 wrapper 的最后/持续使用、producer whitelist、known-W/宽度以及原 home capture 限制。live-end 使用 u32，保留虚拟端点，不缩为 u16。目标准备同时保留两种模式；width 扩展不能抢占 baseline 已证明的写入/读取，公共层仍从完整 baseline 开始整块事务。

producer 到发表之间的语义窗口只在目标准备检查，保留原 IR 顺序、wrapper 跳过、SaveFlags 的 observer 豁免，以及同 home Get/SetHostGPR 拒绝。持续发表后的窗口仍拒绝所有同 home 原始 store，含 partial store；只对原低 home 范围检查 helper clobber。helper 的该项语义沿用请求 FeatureSet，observer 与 fixed-class 标记沿用实际 writer FeatureSet，两种实际池分别快照。没有将这两个 FeatureSet 来源混为一份。

capture 表记录全部 raw GetHostGPR，独立于返回宽度、offset 和窄读取省略资格。公共层据此保留发表后读取不作为第三方写者的原例外，以及 width root 不得置换任何 baseline 同 home read marker 的限制。实际 GPR/pool、preserved/elided fixed 定义、owner 目标、现有同 home 映射、第三方区间、未结束输入与 component 冲突继续在分配阶段检查。

SignExtend 的窄 LoadMemory 只提前记录语义 witness，实际映射为 GPR 后才加入 component。component 使用 u32 值 ID，保留可来自本块之外的有效前驱；其余节点按原扫描顺序借用共享 WidthBridge 视图扩张到不动点，不另扫 opcode 构建第二份组件语义。宽度窗口的原始发表沿用通用 publications 范围，observer 沿用稀疏 site 标记，fixed clobber 沿用实际分配掩码；scratch 拒绝仍恢复全部临时 dirty masks，然后按原顺序执行 owner commit、producer/wrapper 映射和 width 标记。

公共 writes 只对原始 operand 做通用 ForEachValue/ResolveBitCastSource 身份解析，以检查实际 home 和存活结束；没有引入另一份 operand/consumer 图。公共 coalesce.cpp 和 gpr_writes.cpp 不含 ARM64 include、opcode matcher、helper/observer 识别或目标 callback。省略点识别改为准备好的 WidthEmissionSite 标记，Function/Block 门槛 10/1 显式放进目标生成的 PoolPolicy；两种单元及原成员遍历顺序保持独立。

相邻变动 17 个现存/新增源文件，新增四个源/头路径（含 recolor.cpp 原体迁移），退役两条旧 overlay 路径。快照为 416 个现存修改/新增源文件、48 个相对 HEAD 删除路径。没有新增测试文件、测试脚本或 probe。现有 IR 契约补充正常写入、清除写入输入、插入 observer 三种形态，验证真实发表省略/目标 home 随输入变化，语义 observer 在准备阶段已排除候选，以及移动请求后 payload 地址稳定。

九个 Android 目标首轮构建成功，八个 RV 语法检查本批实际重跑通过。冻结源码后只执行一轮最终核心回归，没有失败重试；最终 Ninja 为 no work。红魔 bf1359ff 上 36 个核心场景全部符合预期退出状态。IR default/IR_FAST=0 各 39 用例、823 断言通过；PIN_EXT=3 定向契约两用例/39 断言通过。主套件仍为 712 用例、12 个失败用例、67 个失败断言、0 errors；有序用例、classname 后缀和失败类型/消息/文本与参照相同，只归一化 binary 前缀及 cpp/cc/h 行号。候选/参照种子为 2859163480/2948248232，断言总数为 220894/220932，少 38；不报告汇总数相同或据此归因。lifetime 九模式 × 两种 IR 模式的检查数/注入失败逐项相同；entry 950、call 233、shutdown 两模式 105 项相同。RV 原生执行、static-latch 四个既有失败和完整 shutdown 矩阵不在本次执行范围。

十组候选形态均为本批运行，十组参照复用相邻版本的实际候选日志，保留真实命令/文件名及复用依据；PMU 没有复用。默认 Function CoreMark 及五组 bench，共六组完整有序记录/原始 shape 相同。其余四组所有共同 PC 按每个 PC 分组的有序大小/IR 签名均相同；去掉经相同 ELF 实测的 __d2b_D2A（0x253b00，大小 0x342）和 __dtoa（0x25f2d0，大小 0x1017）后，十组完整有序记录相同。四组新增/缺少 PC 全在这两个符号内部；格式化区间内的共同 PC 出现顺序也可能变化，不报告全 raw shape 相同。十组 spill unit/def/load/store、pair fallback、target overflow、pool/arena、CRC/checksum 均相同。

| 有 roster 差异的形态 | 记录数：参照 → 候选 | 全局 host_bytes：参照 → 候选 |
| --- | --- | --- |
| bound-decode-off Function CoreMark | 424 → 425 | 286700 → 286792 |
| Block CoreMark | 2417 → 2416 | 206104 → 205872 |
| fixed-class Function CoreMark | 424 → 425 | 286260 → 286352 |
| fixed-class Block CoreMark | 2416 → 2412 | 207900 → 207308 |

默认 Function CoreMark 为 424/286696；五组 bench 的记录数/host_bytes 分别为默认 22/19908、bound-decode-off 22/19916、Block 101/16468、fixed-class Function 22/20080、fixed-class Block 101/16492。四组完整 PC 差异、形态日志与精确符号范围均保留，不把全局 host_bytes 变化算作优化收益。

重新采集 Function 与 SVM_FUNC_BASE=0 的实际 Block 各四个 PMU 点，顺序均为 reference-2000、candidate-2000、candidate-12000、reference-12000。simpleperf instructions/cpu-cycles、CPU4/taskset 10、完整命令/UTC 起止/退出状态、每点前后六项频率/温度条件及原始输出均保留，采集不带 audit/host dump。

| 单轮两点拟合：候选相对参照 | instructions 斜率 / 截距 | cycles 斜率 / 截距 | wall 斜率 / 截距 |
| --- | --- | --- | --- |
| Function | +0.008420% / +0.766815% | +0.539067% / +0.194773% | +2.504762% / −2.925608% |
| Block | +0.257901% / −1.479978% | +0.371789% / −3.406690% | +1.449373% / −6.175890% |

这仍是单轮诊断，不能确定收益、退化或因果；拟合截距包含运行时工作，不是纯编译时间。边界 governor 均为 walt、current/max 均为 1171200 kHz，只是观察而非锁频证据；cpuss-0 30.5–33.2°C、cpu1-0 30.5–33.2°C、cpu1-6 29.3–30.9°C。缺少逐事件有效运行比例，不计算 IPC；CRC 均正确为 0x4983/0xd340，保留短于 10 秒的提示，不作为正式 CoreMark 分数。固定源码 Android FEX 与正式三方多轮比较仍未完成。

DWARF 按声明文件核对：AllocationInputs 392、PoolPolicy 24、AllocationState 464、LinearScan 1104 字节不变；新增 PoolPolicy 字段落在原布局空间内。GPRInputs 176→184、GPR BlockRange 56→72；可选 GPRWriteInputs 48、Write 32、Capture 4，GPRInputBuilder 对象布局 792→856。其余 WidthInputs 104、共享 ValueView/ReadReuse 各 16、width Publication 24、Step 20、WPublication 6、Clobber 8、ConsumerRule 4，以及已记录物理结果/owner 布局不变。临时对象 sizeof 不等于完整编译器栈帧或峰值；新增可选记录与减少重复扫描同时存在，没有整体测量 capacity、分配次数和峰值，不报告总内存下降。

source digest 为 62046b2ec061eab45b0fd509f5dbb0b0273b00a3ff24be3c5b2df731f73fcaa7，archive SHA-256 为 d9245e44bc0a3691a93632a48c5c646284476b46026dc5488590ad825315235e。svm_gpr_writes_v1 SHA-256 为 86683be31929ef02d9b70283e99a05ba4a713bf06ee802e6ab7cf3a77cd77993，相邻参照 svm_gpr_widths_v2 为 27be713f8f4d24f7e84674707e363df63940e4ec1bfa66620db72c20e29bc606。全部源码/archive 成员、九个主机/构建二进制、八个设备候选和参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致，recolor 原主体的逐字节比较通过。

构建、完整命令、JUnit、生命周期/持久化、RV 语法、源码边界复核、形态/精确符号、PMU、布局和身份记录位于 /private/tmp/swiftvm-middle-results/r3-gpr-writes-v1-*，设备日志位于 /data/local/tmp/svm-middle-r1/results。quality-gate 的本批检查全部通过，full_goal_complete=false、formal_three_way_performance_accepted=false。继续处理 target fixed/spill 收尾、最终 FPR/emission 契约与动作、R1 必需摘要/workspace、R2 worklist/提交契约、R5/R6 轻量 actions/调用方、R7 清理及完整 R0–R7 验收。

## R3 spill-region 输入化与公共物理执行

本批最终为 r3-spill-regions-v2，相邻参照为 r3-gpr-writes-v1。新增公共 allocate/spill_inputs.{h,cpp} 与 spill_regions.cpp，目标 spill_inputs.{h,cpp} 在既有 DescribeBlock 访问中准备语义规则。原 target spill_reload.{h,cpp}、RecipeSpillReloadRegions、两段 OOB-RECIPE 临时诊断及其诊断专属 FindInstBlock/InRPO 扫描已删除。BuildSpillReloads 现由公共层定义，调用点仍在 assignment 收尾和 coalescing 之前；target allocation_finish.cpp 只剩 VerifyTargetConstraints 一项公共扫描器的目标实现，尚未将完整 R3 宣布完成。

可选 const SpillInputs 保存 transfer/consumer/handoff、GetOperand/LoadMemory ownership site 标记、local-control-flow 分段和块范围。site 只存指令 ID 与标记，segment 用 u32 位置/count 借用唯一 BlockAnalysisIndex 的指令序列，公共查询不补建分析，不另存 operand/use 图或一份指针段。没有 non-pseudo scalar operand 的块在任意物理分配下都没有候选 positions，不保留 payload。target 的四个原语义谓词主体经归一化空白比较保持一致；BindLabel/Goto/NotGoto 继续切段并从段中排除。

实际 MEM、allocation alias、寄存器空闲、已有 reload 和 dirty masks 仍由公共物理阶段判断。每段的临时 positions/count/transfer/handoff 映射是实际 allocation ID 的投影，不能在 decode 时按语义 ID 固定下来，也不作为另一份语义 consumer 图保存。保留原去重位置与重复 occurrence 两种口径，以及 source/raw allocation ID 各自的查询表达式；物理投影仍跳过 pseudo 指令，而 GetUses(false) 的全局计数包含 pseudo uses，不能替换成默认 GetUses() 或局部去重次数。

执行顺序保持原实际 value-ID 升序，再依次尝试 LoadMemory ownership pair、definition transfer、最长重复 reload 区间。输入/结果两寄存器仍先选 result 再选 input，并保留 forbidden bit、owns-all 和 fault-backed 参数。DefinitionTransfer/global use-count、segment membership、全宽/浮点过滤和实际 scratch 检查均保留；MapSpillReload 的提交顺序不变。

terminal 使用按相同 If/Switch/Condition/CheckHalt 子集和原块范围投影实际 allocation alias，排序去重后每块使用一次。源码核对 MapSpillReload 只修改 reload 元数据和 dirty masks，不改变 allocation alias，因此本阶段内该投影保持有效。Function 仍按 GetHIRBlocksRPO 遍历，Block 单独处理；没有把块内 ownership 证明扩为整函数证明，也没有预先选择哪些值最终会溢出。

spill 的旧 ScratchOnlyGPRs 使用指令实例级 FixedGPRClobbers，普通 instruction scratch 使用 opcode 级计数；X87 等指令可能不同。本批在同一目标 scratch 描述阶段为两种实际池分别准备 spill_outside_value_pool，保留两种计数；普通 scratch 字段及其 checker 不变。Spill headroom 从同一压缩 profile 读取实例级 outside 数量，Add/Sub overlap/default headroom 复用已有字段，比较仍是严格 available > required。拒绝与寄存器选择继续读此前 region 已更新的 dirty/reload 状态。

相邻变动 16 个现存/新增源文件，新增五个源/头路径、退役两个旧 overlay 路径；快照为 419 个现存修改/新增源文件、48 个相对 HEAD 删除路径。没有新增测试文件、测试脚本或 probe。原 spill_forwarding_test.cpp 补充一个三形态行为用例：手动确定 MEM 结果后，正常输入建立真实 owned reload，清除输入取消它，local control flow 阻止跨段 ownership；请求移动后 payload 地址稳定。测试入口只存在于现有 RegisterAllocTestSupport，没有生产测试 hook；原无实际作用的 Proposal-only 注释一并移除。

v1 构建暴露新头缺少 Inst 前置声明、目标输入实现缺少 facts.h 直接依赖，第三项 Describe 声明错误由此级联；构建日志和源码快照保留原名，设备测试尚未开始。v2 补齐直接依赖后九个 Android 目标构建成功，八个 RV 语法检查本批实际重跑通过。冻结源码后执行一轮最终核心回归，没有设备失败重试，最终 Ninja 为 no work。

红魔 bf1359ff 上 36 个核心场景全部符合预期退出状态。主套件现为 713 用例、221141 断言，新增的 spill 输入行为用例通过；剔除且仅剔除这一明确新增用例后，原有 712 个有序用例、classname 后缀和失败类型/消息/文本与参照相同，只归一化 binary 前缀及 cpp/cc/h 行号。仍为 12 个失败用例、67 个失败断言、0 errors。候选/参照种子为 2688697402/2859163480，参照总断言 220894，本批多 247；存在新用例且种子不同，不报告汇总数相等或据此归因。IR default/IR_FAST=0 各 39 用例、823 断言通过；PIN_EXT=3 定向契约两用例/39 断言通过。lifetime 九模式 × 两种 IR 模式的检查数/注入失败逐项相同，entry 950、call 233、shutdown 两模式 105 项相同。RV 原生执行、static-latch 四个既有失败和完整 shutdown 矩阵不在本次执行范围。

十组候选形态均为本批运行，十组参照复用相邻版本实际候选日志，保留真实命令/文件名及复用依据；PMU 没有复用。默认 Function CoreMark 与五组 bench，共六组完整有序记录/原始 shape 相同。十组共同 PC 的出现记录和大小/IR 签名相同，移除格式化两个符号区间后完整有序记录也相同；四组 roster 差异全部位于同一 guest ELF 实测的 __d2b_D2A（0x253b00，大小 0x342）与 __dtoa（0x25f2d0，大小 0x1017）内部。十组 spill unit/def/load/store、pair fallback、target overflow、pool/arena、CRC/checksum 均相同；不将全局 host_bytes 变化当作优化收益。

| 有 roster 差异的形态 | 记录数：参照 → 候选 | 全局 host_bytes：参照 → 候选 |
| --- | --- | --- |
| bound-decode-off Function CoreMark | 425 → 424 | 286792 → 286700 |
| Block CoreMark | 2416 → 2416 | 205872 → 206272 |
| fixed-class Function CoreMark | 425 → 424 | 286352 → 286260 |
| fixed-class Block CoreMark | 2412 → 2413 | 207308 → 207320 |

默认 Function CoreMark 为 424/286696；五组 bench 的记录数/host_bytes 分别为默认 22/19908、bound-decode-off 22/19916、Block 101/16468、fixed-class Function 22/20080、fixed-class Block 101/16492。四组详细新增/缺少 PC 保留在 shape-summary.json，不称全部 raw shape 相同。

重新采集 Function 与 SVM_FUNC_BASE=0 的实际 Block 各四个 PMU 点，顺序均为 reference-2000、candidate-2000、candidate-12000、reference-12000。simpleperf instructions/cpu-cycles、CPU4/taskset 10、完整命令/UTC 起止/退出状态及每点前后六项频率/温度条件均保留；PMU 不带 audit/host dump。

| 单轮两点拟合：候选相对参照 | instructions 斜率 / 截距 | cycles 斜率 / 截距 | wall 斜率 / 截距 |
| --- | --- | --- | --- |
| Function | −0.008062% / +1.150516% | +0.583432% / −0.566582% | +0.532894% / +0.001222% |
| Block | +0.008741% / +1.214609% | +0.220088% / +0.186929% | +1.082556% / +0.720801% |

这些仍为单轮诊断，不证明收益、退化或因果；截距包含运行时工作，不能解释为纯编译成本。边界 governor 均为 walt、current/max 均为 1171200 kHz，是边界观察而非锁频证据；cpuss-0 30.5–32.8°C、cpu1-0 30.5–33.2°C、cpu1-6 29.3–30.9°C。没有逐事件有效运行比例，不计算 IPC；CRC 为 0x4983/0xd340，保留短于 10 秒的提示，不作为正式 CoreMark 分数。固定源码 Android FEX 与正式三方多轮比较仍未完成。

DWARF 按声明文件核对：AllocationInputs 392→400、AllocationState 464→472 字节；ScratchRequirements 20→24、ScratchProfile 6→8、ScratchSite 8→10。可选 SpillInputs 72、spill Site 4、Segment 8、BlockRange 24，SpillInputBuilder 对象布局 80；参考新增 scratch 类型的布局从固定 SHA 的参照可执行文件重新读取，来源已核对。PoolPolicy 24、LinearScan 1104、GPRInputs 184、GPR BlockRange 72、GPRInputBuilder 856 及其余已记录结果/owner 布局不变。新增记录与去掉逐 attempt 指针段、减少 terminal 重复投影同时存在；对象 sizeof 不等于完整栈帧/峰值，capacity、分配次数和真实峰值未整体测量，不报告总内存下降。

最终 source digest 为 48b896340505cbae4181b289e09d173d6f0da664c07a34b80bfaf0bff05dfdfa，archive SHA-256 为 ffd48d01f79e98dfb92145d6d07f15bb5b3def4357f83b30cfa023c0fabbbf62。svm_spill_regions_v2 SHA-256 为 ec2916b74965aae6256ab468b0b3c876b154c90c0497f82fe24a9b70ce0d1158，相邻参照 svm_gpr_writes_v1 为 86683be31929ef02d9b70283e99a05ba4a713bf06ee802e6ab7cf3a77cd77993。全部源码/archive 成员、九个主机/构建二进制、八个设备候选及参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致。

构建、完整命令、JUnit、生命周期/持久化、RV 语法、语义谓词/源码边界复核、形态/精确符号、PMU、布局和身份记录位于 /private/tmp/swiftvm-middle-results/r3-spill-regions-v2-*，设备日志位于 /data/local/tmp/svm-middle-r1/results。quality-gate 的本批检查全部通过，full_goal_complete=false、formal_three_way_performance_accepted=false。继续处理最后的 target fixed 验证接缝、最终 FPR/emission 契约与动作、R1 必需摘要/workspace、R2 worklist/提交契约、R5/R6 轻量 actions/调用方、R7 清理及完整 R0–R7 验收。

## R3 fixed 验证输入化与公共物理检查

本批最终为 r3-fixed-validation-v3，相邻参照为 r3-spill-regions-v2。新增公共 allocate/fixed_validation.{h,cpp}、fixed_verify.cpp 和目标 compile/fixed_validation.{h,cpp}。旧 target constraints_verify.cpp、最后的 allocation_finish.cpp、VerifyTargetConstraints 及旧 Verify/CheckFixedGPRConstraints API 已删除；目标目录不再定义公共 LinearScan 方法，没有保留转发头或别名。初始 fixed 安装、物理 coalescing、spill-region 和必要 fixed 检查均已回到公共层；最终 FPR/emission 契约及动作仍待收敛，完整 R3 尚未完成。

目标 FixedValidationBuilder 在既有 DescribeBlock 指令访问中记录默认 FixedGPRClobbers、helper 低十个 home 的 clobber、host read/write、width alias 与窄常量标记；默认 clobber 不替换为两种池的 scratch-only clobber。Site 只保存掩码/编号/标记，Step 保存透明链逐边原始来源和类型视图，BlockRange 借用 SemanticFacts 已有的唯一 BlockAnalysisIndex。临时去重集合只在 builder 中存在；可达外部 extension/BitCast 前驱也会描述，不另建完整 consumer 图。FindInstruction 对正常密集编号直接索引，对手工稀疏编号作只读查找。

公共检查只接收 AllocationView、const 描述和 PhysicalGPRWrite，检查实际 home、GPR/MEM、未省略写入、覆写区间及 preservation。保留 (after,before) 开区间、consumed-use 提前写回的唯一见证断言、首个 fail_kind=1/2/3、preserved 形状与位宽条件。透明链遇到待验证 value 立即停止，按该次 raw operand 的类型视图验证宽度；不先 ResolveBitCastSource 或展开到最终根。BitCast 只能作为中间节点，SetHostGPR 只能作为首节点，原 extension 白名单保持不变。GetOp 只用于失败消息，公共层不匹配目标 opcode/helper。

必要检查仍在普通分配收尾、scratch 验证之前执行，也接入手工 Freeze、publication recolor 提交前拒绝以及可选审计。没有将必要契约移动到可关闭的 audit 下。普通路径借用同一请求的描述；由于既有测试/手工调用可能提供不同 FeatureSet，FixedValidationDescription 只有在完整 FeatureSet 和影响 clobber 的 x86_pin_ext/static_regs/exec_prof/XPOOL 配置身份均匹配时复用。否则在准备期使用调用方提供的现有索引显式生成短期描述；const 查询不补建，描述不进入 emission。

相邻变动 27 个现存/新增源文件，新增五个源/头路径、退役两个旧 overlay 路径；快照为 422 个现存修改/新增源文件，48 个相对 HEAD 删除路径。没有新增测试文件、脚本或 probe。原 IR 契约文件补充一个真实行为用例：准备并分配后检查透明 extension、带 U8 operand 视图的覆写拒绝、显式写入覆写、错误 home、preservation 拒绝和不同 feature 描述。其余手工与审计用例接入显式描述和已有索引，没有生产测试 hook。

v1 构建因 verifier 缺少 BlockAnalysisIndex 的直接 include 停止；v2 构建又暴露测试依赖旧聚合头间接提供 GuestStateMap，最终为三个实际使用者补齐直接 include。两版源码快照和日志保留，均未启动设备测试。v3 九个 Android 目标构建成功，仅运行一轮最终设备回归，最终 Ninja 为 no work。八个 RV 语法命令实际在 v2 执行通过；v3 只改上述三个非 RV 测试的直接 include，生产及 RV 源/头内容不变，因此保留并标明这份证据的实际 v2 命令与日志，没有冒称重跑。

红魔 bf1359ff 上 36 个核心场景全部符合预期退出状态。主套件 713 用例、221005 断言，仍为 12 个失败用例、67 个失败断言、0 errors；全部 713 个有序用例、classname 后缀及失败类型/消息/文本与相邻参照一致，只归一化 binary 前缀和 cpp/cc/h 行号。候选/参照种子为 4248226490/2688697402，总断言比参照少 136；种子不同，不报告断言汇总相等，也不据此归因。IR default/IR_FAST=0 各 40 用例、842 断言通过，相比原 39 用例新增且仅新增上述通过用例。PIN_EXT=3 定向契约两用例/39 断言通过。lifetime 九模式 × 两种 IR 模式逐项检查/注入计数一致，entry 950、call 233、shutdown 两模式 105 项一致。RV 原生、static-latch 四个既有失败及完整 shutdown 矩阵未在本批执行。

十组候选形态均为本批运行，十组静态参照复用相邻版本的真实候选日志并保留命令/路径及依据；PMU 没有复用。八组完整有序记录及原始 shape 相同，另两组仅为 fixed-class CoreMark 的格式化路径 roster 变化：Function 424→425，Block 2413→2416，差异均在同一 guest ELF 的 __d2b_D2A（0x253b00，大小 0x342）和 __dtoa（0x25f2d0，大小 0x1017）。十组共同 PC 的有序签名均相同，排除这两个精确符号区间后的完整有序记录也相同；pool/IR arena、CoreMark CRC 和五项 bench checksum 相同。不能将这两组报告为完整 shape 相等。

候选记录数/全局 host_bytes：默认 Function core 424/286696、bench 22/19908；bound-decode=0 的 core 424/286700、bench 22/19916；实际 Block core 2416/206272、bench 101/16468；fixed-class Function core 425/286352、bench 22/20080；fixed-class Block core 2416/207700、bench 101/16492。SVM_BOUND_DECODE=0 仍为 Function，实际 Block 由 SVM_FUNC_BASE=0 选择；Block 比较 host PC/size，不比较含地址字节和 hash。

新采八个 PMU 点，CPU4/taskset 10，每模式顺序为参照2k、候选2k、候选12k、参照12k，未开启 audit/prof/host dump。两点 slope/intercept 相对差：Function instructions −0.036692%/+1.153633%，cycles −0.717460%/+4.430317%，wall +0.419482%/+2.249764%；Block instructions −0.035732%/+1.382579%，cycles −0.468740%/+2.654901%，wall −0.341342%/+1.971208%。单轮诊断不支持收益或因果结论；未取得每事件 running ratio，不报告 IPC。前后观测 governor=walt，current 为 1056000 或 1171200 kHz、max=1171200，cpuss-0 29.7–34.4°C、cpu1-0 30.9–34.0°C、cpu1-6 28.9–31.6°C；没有据此宣称锁频。CoreMark 短运行警告保留。

Android DWARF sizeof：AllocationInputs 400→408、AllocationState 472→480、LinearScan 1104 不变；FixedValidationInputs 80、Site 8、Step 24、BlockRange 16，builder 56、临时描述 16；PhysicalGPRWrite 16 与旧目标 POD 相同。AllocationWriter 912、ARM64 preparation 920、prepared/optimized Storage 944/728 不变。新增语义 payload 与移除逐次 definition-pointer 索引同时存在；sizeof 不等于完整栈帧、capacity 或峰值，未测总内存变化。

最终 source digest 为 2743bf9bd58462be6330868df7ac6d0f8d390e3963f393fccaf1b31c7ad035a1，archive SHA-256 为 989d47b8179d1b3f44ce6bb40098ef930348217c1a88c977ccc11f3bb9f2979c。svm_fixed_validation_v3 SHA-256 为 d363732bbfc2fae3b3a13484765d1ae73f1e90d8b71c2a618a20f65ebc7f4af5，相邻 svm_spill_regions_v2 为 ec2916b74965aae6256ab468b0b3c876b154c90c0497f82fe24a9b70ce0d1158。全部源码/archive 成员、九个主机/构建二进制、八个设备候选与参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致。

完整构建、命令、JUnit、shape/PMU、布局、源码边界及身份结果位于 /private/tmp/swiftvm-middle-results/r3-fixed-validation-v3-*，设备原始日志位于 /data/local/tmp/svm-middle-r1/results。quality-gate 本批 20 项检查通过，full_goal_complete=false、formal_three_way_performance_accepted=false。继续接续最终 FPR/emission 契约与动作、R1 必需摘要/workspace、R2 worklist/类型/提交契约、R5/R6 轻量 actions/调用方及 R7 清理。固定源码 Android FEX 和完整 R0–R7 验收仍未完成。

## R3 FPR 最终契约与统一动作证明

本批为 r3-fpr-certificates-v1，相邻参照为 r3-fixed-validation-v3。原 alloc/fpr_allocation_plan.{h,cpp} 已删除，五项最终判定归目标 compile/fpr_contracts.{h,cpp}；FPRAllocationPlan、BuildFPRAllocationPlan、GPRBlockPlan::fpr、每块 FPR shared_ptr 及 actions/controls 两份稠密向量均已退役。生产 emitter 的十处证明/控制值读取改为 UnitActions，必要拒绝和 FPR contract retry 保留。scalar_fpr_publication 等其余动作家族及冻结 scratch/emission 元数据尚未统一，没有宣布完整 R3 或 R5 完成。

FPRContract 准备入口只读 AllocationView，显式接收调用方已有 BlockAnalysisIndex；有实际选择后才显式请求 resolved uses。原 optional analysis、owned index 和 CompactUses 已删除，手工测试显式准备各自的索引，不保留 const 查询补图路径。五个 Compute 主体和 SelectPshufdDirect 的规则体均与相邻源码作空白归一化比较相同；完整 FeatureSet、scalar_insert/scalar_tie、AES 前后继、observer/committed-home-safe、live owner、shuffle alias/spill 和 exact-consumer gate 均保留。物理视图在整个 FPR admission 中不变，故原每个 home 的 Owners 全块遍历合并为一次所有 home 的同序投影；这不预先决定 decode 阶段的实际 FPR owner。

临时 FPRContract 只保存有动作的四字节 instruction-ID/action/control 记录和拒绝位置；它不进入 emission。所有块先完成必要检查，任一块失败都在 BeginTargetPlanning 和安装证书前返回原重试原因。通过后将证明写入 UnitActions：原低四位证明保留，FPR 的六类证明占后六位，统一稠密表从 u8 扩为 u16。PSHUFD 控制值使用按需稀疏四字节记录，目标准备期间可查，封口时按 ID 排序并检查唯一性；冻结后用 lower_bound 查询。UnitActions 仍以 allocation 实例、generation、attempt 和 frozen 状态拒绝旧结果；Begin/Abort 清除证明及控制表。冻结动作中没有 FPR 子 plan、独立图或 IR 指针 payload。

相邻变动 16 个现存/新增源文件，新增两个 compile 路径、退役两个旧 tracked 文件；快照仍为 422 个现存修改/新增源文件，相对 HEAD 删除路径增为 50。没有新增测试文件、脚本或 probe。既有 shuffle 用例保留全部规范 control、exact-consumer、别名/spill、stale/rebuild/reset 与顺序独立检查；原共享/内部补图对照改为两个显式索引。成功路径新增 UnitActions 构建中、冻结后的实际 control/证明读取及 reset 后拒绝检查；合并后的记录比较不保留重复的 actions/controls 同义断言。FPR 活跃旧值拒绝及 Block/Function 三种重试路径继续由原用例验证。

首次构建即通过九个 Android 目标；八个 RV 语法命令本批实际重跑通过。冻结源码后红魔 bf1359ff 上运行一轮 36 个核心场景，全部符合预期退出状态，最终 Ninja 为 no work。主套件仍为 713 个有序用例，221258 断言、12 个失败用例、67 个失败断言、0 errors；全部用例 classname 后缀和失败类型/消息/文本与参照一致，仅归一化 binary 前缀及 cpp/cc/h 行号。候选/参照种子 4151797716/4248226490，断言总数 +253；本批既改变断言又使用不同种子，不能只归因于某一项。IR default/IR_FAST=0 各 40 用例/842 断言通过，PIN_EXT=3 定向契约两用例/39 断言通过。lifetime 九模式 × 两种 IR 模式、entry 950、call 233、shutdown 两模式 105 项逐项一致。RV 原生、static-latch 四个既有失败及完整 shutdown 矩阵未在本批运行。

十组候选 shape 为本批运行，静态参照复用相邻实际候选日志并保留命令/路径和依据。七组完整有序记录及原始 shape 相同；默认 Function core、默认 Block core、fixed-class Block core 的 roster 差异均在同一 guest ELF 已确认的 __d2b_D2A（0x253b00，大小 0x342）与 __dtoa（0x25f2d0，大小 0x1017）。十组每个共同 PC 的 occurrence 签名均相同，排除这两个精确符号区间后完整有序记录相同；fixed-class Block core 的共同 PC 全局首次出现顺序也在格式化路径内改变，因此该组不能报告完整 common-PC 子序列相同。pool/IR arena、CoreMark CRC 和五个 bench checksum 一致，没有为匹配参照而重跑。

候选记录数/全局 host_bytes：默认 Function core 425/286788、bench 22/19908；bound-decode=0 的 core 424/286700、bench 22/19916；实际 Block core 2415/206260、bench 101/16468；fixed-class Function core 425/286352、bench 22/20080；fixed-class Block core 2418/207920、bench 101/16492。Block 仍按 host PC/size 比较，不比较含地址的机器码 hash/bytes。

新采八个 PMU 点，CPU4/taskset 10、每模式参照2k/候选2k/候选12k/参照12k，未复用 PMU，也未开启 audit/prof/host dump。两点 slope/intercept 相对差：Function instructions +0.012526%/−0.015840%，cycles −0.472175%/+0.728861%，wall +0.400857%/+0.535427%；Block instructions +0.003629%/−0.281814%，cycles +0.535337%/−3.797868%，wall −0.468401%/−1.073705%。单轮仅作诊断，不报告收益或因果结论；缺少每事件 running ratio，不报告 IPC。前后观测 governor=walt、current/max=1171200 kHz，cpuss-0 29.7–34.4°C、cpu1-0 30.5–34.0°C、cpu1-6 28.9–32.0°C；不是锁频证明，CoreMark 短运行警告保留。

Android DWARF sizeof：旧 FPRAllocationPlan 80 已消失，临时 FPRContract 40、Instruction 4；builder 1496→848，原内嵌独立 index 已删除。UnitActions 88→112，统一证明元素由一字节改为两字节，稀疏 ShuffleControl 4；GPRBlockPlan 432→416，prepared Storage 944→968、optimized Storage 728 不变，AllocationPreparation 920 不变。移除每块 shared_ptr/稠密向量与增加统一表/稀疏 payload 同时发生，sizeof 不等于完整栈帧或峰值，未测容量/分配次数/总内存变化，不据此报告总内存下降。

source digest 为 5ad54b2b6578f23a5df76763f987427d1a1ed055fd03ae89629d373860ee2763，archive SHA-256 为 1afab2f0d9820e34da1e38da32a690e1b6465d0470481e397be4f9dc6e689e02。svm_fpr_certificates_v1 SHA-256 为 86e8724e1f0f2c6fae38c5b2956c14aa0dd952c7587610939dfe8207bf259b63，相邻 svm_fixed_validation_v3 为 d363732bbfc2fae3b3a13484765d1ae73f1e90d8b71c2a618a20f65ebc7f4af5。源码/archive 成员、九个主机/构建二进制、八个设备候选和参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致。

完整结果位于 /private/tmp/swiftvm-middle-results/r3-fpr-certificates-v1-*，设备原始日志位于 /data/local/tmp/svm-middle-r1/results。quality-gate 的 23 项本批检查通过，full_goal_complete=false、formal_three_way_performance_accepted=false。继续处理其余 emission/scratch 契约与 scalar-FPR 等动作家族、R1 必需摘要/workspace、R2 worklist/类型/提交契约、R5/R6 轻量 actions 与调用方、R7 清理和完整适用矩阵；可复现 Android FEX 与正式 R0–R7 验收仍未完成。

## R5 标量 FPR 发表动作与统一覆盖

本批为 r5-scalar-fpr-actions-v1，相邻参照为 r3-fpr-certificates-v1。原 alloc/scalar_fpr_publication_plan.{h,cpp}、jit/translator_fpr_publication.cpp、ScalarFPRPublicationPlan、GPRBlockPlan::scalar_fpr 与 ScalarPublicationPlan 查询已删除。实现归 compile/lower_vector.{h,cpp}，不再保留每块 shared_ptr、两个指针 map 或发射覆盖指针列表。其余 residence/copy/memory 等家族及 R3 emission/scratch 契约仍待接续，完整 R5 和 R0–R7 未完成。

LowerScalarFPRPublications 接收只读 AllocationView 和调用方已有的 BlockAnalysisIndex，有候选后显式请求 resolved uses；原 optional analysis 和内部 owned index 已删除。IsFusionBarrier、PublicationShape 与 load/owner/barrier 准入段在相邻源码上作空白归一化比较相同。候选仍严格要求相邻 low/high SetHostFPR、目标 v16–v31、U64 零值、高半偏移与完整宽度；load32 的 ZeroExtend64 和各节点单一 raw use、指令顺序、同 home 观察与外部 FPR owner 拒绝规则保留。raw GetUses(false)、DirectUseCount 与 zero_uses 的口径不变，仍按实际块内证明，未扩展为函数范围的 owner 证明。

临时 ScalarFPRActions 只包含稀疏 ScalarFPRAction 和 covered ID 两份列表，不进入 emission。冻结 payload 仅四字节：u16 instruction、u8 target、Load32/Load64/Value kind；low/high/zero/extension 的指针见证和临时 zero-use map 只在准备阶段存在。普通指令没有该 payload。UnitActions 保存单元级稀疏列表，封口排序并检查 ID 唯一性；构建时与冻结后均可查询，Begin/Abort 清空，reset/generation/attempt 失配拒绝旧动作。

覆盖使用既有 u16 证明表的 bit10，原低四位和 FPR 后六位不变，没有新建一份常驻覆盖数组。load 组合覆盖 low/high、可选 extension 和已无其他使用的零值；value 组合保留 low，覆盖 high 及可省略的零值。target 的 flag_coverage 仍在 memory coverage 之后、integer/bit 规则之前消费同一组 ID。emitter 的最后有效指令选择与主发射循环都改用统一 InstructionDisabled，合并旧动态 bitmap 和 UnitActions 覆盖；未以删除专属导入为由漏掉其中一条路径。实际 LDR S/D、FMOV/EOR 编码主体不变。

原测试支持中的 ScalarPublicationCounts 私有子 plan 查询已删除，已有 isolated metrics 用例改从公开动作接口计数；IsInstructionSuppressed 跟随统一覆盖查询，没有新增生产测试 hook。两个手工调用方显式准备独立索引。既有九种 barrier/live-lane/shape 场景及 function/cycle 字节稳定性用例继续验证。原 scalar_publication_plan_test.cpp 新增一个四形态行为用例，覆盖 Load64、Load32+extension、observer 后保留 load 并选择 value 发表、零值有额外使用时必须继续发射，以及构建中/冻结后的动作读取和 reset 后失效；没有新增测试文件、测试脚本或 probe。

相邻变动 17 个现存/新增源文件，新增两个 lower_vector 路径、退役三个 tracked 文件。快照为 421 个现存修改/新增源文件，53 个相对 HEAD 删除路径。首次构建通过九个 Android 目标，八个 RV 语法检查本批实际执行通过；冻结源码后红魔 bf1359ff 上仅执行一轮 36 个核心场景，全部符合预期退出状态，最终 Ninja 为 no work。

主套件现为 714 用例、221175 断言、12 个失败用例、67 个失败断言、0 errors。剔除且仅剔除新增四形态用例后，原有 713 个有序用例、classname 后缀与失败类型/消息/文本和参照一致，只归一化 binary 前缀与 cpp/cc/h 行号；新增用例通过。候选/参照种子为 3667509351/4151797716，总断言 −83；既修改断言又存在种子差异，不报告断言总数相等或据此单一归因。IR default/IR_FAST=0 各 40 用例/842 断言、PIN_EXT=3 定向契约两用例/39 断言通过。lifetime 九模式 × 两种 IR 模式、entry 950、call 233、shutdown 两模式 105 项逐项一致。RV 原生、static-latch 四个既有失败与完整 shutdown 矩阵未在本批执行。

十组候选形态为本批运行，静态参照复用相邻真实候选日志并保留其命令/路径及依据，PMU 没有复用。六组完整有序记录和原始 shape 相同。默认 Function core、默认 Block core、fixed-class Function/Block core 四组 roster 差异都位于同一 guest ELF 已确认的 __d2b_D2A（0x253b00，大小 0x342）和 __dtoa（0x25f2d0，大小 0x1017）。十组每个共同 PC 的 occurrence 签名及排除这两个精确符号区间后的完整有序记录相同；默认 Block core 的共同 PC 全局首次出现顺序也在格式化路径内改变，不能称其完整 common-PC 子序列相等。pool/IR arena、CoreMark CRC 与五项 bench checksum 一致，没有为对齐参照重跑。

候选记录数/全局 host_bytes：默认 Function core 424/286696、bench 22/19908；bound-decode=0 的 core 424/286700、bench 22/19916；实际 Block core 2416/205872、bench 101/16468；fixed-class Function core 424/286260、bench 22/20080；fixed-class Block core 2415/207692、bench 101/16492。Block 仍比较 host PC/size，不比较含地址的机器码 hash/bytes。

新采八个 PMU 点，CPU4/taskset 10，每模式参照2k、候选2k、候选12k、参照12k，未开启 audit/prof/host dump。两点 slope/intercept 相对差：Function instructions +0.012042%/+0.108437%，cycles +0.227069%/+0.279113%，wall −0.388427%/−0.170528%；Block instructions +0.018818%/+0.208102%，cycles +1.252991%/−0.089759%，wall −0.511151%/+1.850829%。单轮只作诊断，不报告收益或因果结论；缺少每事件 running ratio，不报告 IPC。前后观测 governor=walt、current/max=1171200 kHz，cpuss-0 30.5–32.8°C、cpu1-0 30.9–33.2°C、cpu1-6 28.9–30.9°C；不构成锁频证明，CoreMark 短运行警告保留。

Android DWARF sizeof：旧 ScalarFPRPublicationPlan 72 和其公开 40 字节 publication 类型已消失；临时 ScalarFPRActions 48，冻结 ScalarFPRAction 4；40 字节 Publication 仅在 lower_vector.cpp 内部作为临时见证。UnitActions 112→136、GPRBlockPlan 416→400，prepared Storage 968→992，optimized Storage 728、AllocationPreparation 920 不变。统一证明元素仍为 u16。移除每块 map/shared_ptr/覆盖指针列表与增加单元稀疏向量同时发生，sizeof 不等于全部容量、分配次数、完整栈帧或峰值，未测总内存变化，不宣称总内存下降。

source digest 为 08fe0f3cbceb79b223a93c433cb8164ffb9f096f4c87c3128b093353ddf44940，archive SHA-256 为 f468eb3f923405cd1b4238b3e30a7d10ad204d0102d361edb23168816494b31a。svm_scalar_fpr_actions_v1 SHA-256 为 9260afabe2434d60de52b6c6be9fc432619c8b09ea80a96493c87e152e65c55a，相邻 svm_fpr_certificates_v1 为 86e8724e1f0f2c6fae38c5b2956c14aa0dd952c7587610939dfe8207bf259b63。源码/archive 成员、九个主机/构建二进制、八个设备候选与参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致。

完整结果位于 /private/tmp/swiftvm-middle-results/r5-scalar-fpr-actions-v1-*，设备原始日志位于 /data/local/tmp/svm-middle-r1/results。quality-gate 的 23 项本批检查通过，full_goal_complete=false、formal_three_way_performance_accepted=false。后续仍覆盖 R1 必需摘要/workspace、R2 worklist/类型/提交契约、R3 剩余 emission/scratch 契约、R5/R6 其余轻量动作与调用方、R7 清理/完整适用矩阵；可复现 Android FEX 与正式 R0–R7 验收继续保留在范围内。

## R5 Boolean Select 与 PF token 统一动作

本批为 r5-predicate-token-actions-v1，相邻参照为 r5-scalar-fpr-actions-v1。两个家族一起完成后统一构建/验证，没有逐文件重复设备测试。旧 alloc/boolean_select_plan.{h,cpp}、flags_token_plan.{h,cpp}、两个子 plan/API、GPRBlockPlan 中的两个 shared_ptr 字段及 emitter 的 Select 专属覆盖导入已删除。选择实现分别归 compile/lower_predicates.{h,cpp} 与 lower_flags.{h,cpp}，结果归 UnitActions。其余 narrow-bit、integer flags、consumer/memory/edge 等动作与 R3 emission/scratch 契约仍待接续，完整 R5/R0–R7 未完成。

LowerSelects 保留 IsImmediate、IsAdjustedBoolean 原主体，最大递归深度、U8 真1/假0、CondSet sole raw use、块成员身份、pseudo-use/NZCV defer 安全窗口和 shared-constant accumulator 不变；准入与覆盖段与相邻源码作归一化比较相同，只有输出表示变化。四字节 SelectAction 保存编号、boolean 与可选 condition 的紧凑编码，coverage 只保存 u16 ID；不再保留动作指针 pair 或覆盖指针列表。target flag_coverage 仍在原有 integer coverage 之后、后续 narrow-bit 之前消费覆盖，emitter 的 terminal-body 与主循环已统一读取 UnitActions 标记，不需要额外导入 bitmap。

LowerFlagsTokens 接收只读 AllocationView、显式 publication span 和已准备的 PseudoFlagsPlan。生产 span 保持此前发表 map 的访问顺序；不再隐式扫描整块寻找候选，或按每次查询重扫 pseudo 来推导 saved parity。SavesParity 与 branch-only parity 的区别保留；OR/AND funnel capture 的静态需求仍读取同一摘要。Shares/Wrapper 原物理判定、候选过滤、每 home 最后发表、默认 FixedGPRClobbers、suffix owners、preservation 与 capture 点规则均核对原逻辑，未将块内证明扩展为函数范围证明。

八字节 FlagsTokenAction 保存 u32 homes 掩码、u16 producer 和 u16 point。point=UINT16_MAX 对应 IR 的无效编号，表达原非 preserved 路径的整块保留；preserved 路径继续只接受明确计划的点，不用整块标记替代它。准备结果排序并合并相同 producer/point 的 home 掩码，删除旧按块稠密 homes、point_homes map、point_dependent/first_id 状态与子 plan。实际 token 有效性、发表与物化继续由 emitter 管理，不进入冻结资格记录。

两个 payload 都按单元存储。函数准备按 RPO 安装块，构建期查询允许未排序记录，封口再全局按编号排序并检查唯一性；冻结后用 lower_bound 查询。Select coverage 复用已有 u16 证明表 bit10，没有新增覆盖数组。Begin/Abort 清空两个 payload，query 按同一 allocation 实例、generation、attempt 与 frozen 状态拒绝失效动作。编号仅在所属 owning unit 内解释，旧临时 ActionFor(pointer) 的同 ID 外部指针拒绝 API 已删除；正式边界仍验证 unit/block/Allocation 的身份，没有保留指针查询兼容包装。

既有 Boolean 场景继续覆盖 16 种 flags/外部常量/使用次数/递归深度、独立 select 顺序、stale index 与 defer gate；新增真实 UnitActions 的 allocation-instance、冻结与 reset 失效检查。顺序独立用例的汇总也改为编号，避免在新接口下对离开作用域的 block 指针读取 ID。PF 既有用例继续覆盖同/异 home 多发表、offset、普通/窄 wrapper、suffix 覆写、默认 fixed clobber、saved/branch-only parity 与具体 preserved capture 点，新增构建/冻结/reset 查询；删除的 legacy parity fallback 不再用同义重复断言保活。已有隐藏 token metrics 改为只测显式摘要路径并报告 retentions/median_ns，Boolean metrics 改读公开动作接口；本批未执行这些隐藏指标循环。

原 flags_token_plan_test.cpp 新增一个两块函数行为用例，使用真实 lowering，故意逆序安装两块的 Select/PF 结果，检查封口排序、不同 condition、统一覆盖、整块保留、错误 home、reset 和 abort 后拒绝。没有新增测试文件、脚本、probe 或生产测试 hook。相邻变动 21 个现存/新增源文件，新增四个 lower 文件，退役四个 tracked 文件；快照 421 个现存修改/新增源文件，57 个相对 HEAD 删除路径。

首次构建通过九个 Android 目标，八个 RV 语法命令本批实际执行通过。冻结源码后红魔 bf1359ff 上仅运行一轮 36 个核心场景，全部符合预期退出状态，最终 Ninja 为 no work。主套件 715 用例、221137 断言，12 个失败用例、67 个失败断言、0 errors；剔除且仅剔除新增两块行为用例后，原有 714 个有序用例、classname 后缀及失败类型/消息/文本与参照一致，只归一化 binary 前缀和 cpp/cc/h 行号。新增用例通过。候选/参照种子为 3135606271/3667509351，总断言 −38；同时有测试断言调整和随机种子变化，不报告总断言相等或据此单一归因。

IR default/IR_FAST=0 各 40 用例/842 断言，PIN_EXT=3 定向契约两用例/39 断言通过；lifetime 九模式 × 两种 IR 模式逐项检查与注入计数、entry 950、call 233、shutdown 两模式 105 项一致。RV 原生、static-latch 四个既有失败和完整 shutdown 矩阵未在本批执行，九目标构建不能替代这些原生执行范围。

十组候选 shape 为本批运行，静态参照复用相邻版本真实候选日志并保留命令/路径与依据；PMU 没有复用。七组完整有序记录及原始 shape 相同。默认 Function core、默认 Block core、fixed-class Block core 三组 roster 差异只在同一 guest ELF 的 __d2b_D2A（0x253b00，大小 0x342）与 __dtoa（0x25f2d0，大小 0x1017）。十组每个共同 PC 的 occurrence 签名、排除这两个精确符号范围后的完整有序记录相同；两个 Block core 的共同 PC 全局首次出现顺序也在格式化路径内变化，不能报告它们完整 common-PC 子序列相等。pool/IR arena、CoreMark CRC 与五项 bench checksum 一致，没有为了对齐参照重跑。

候选记录数/全局 host_bytes：默认 Function core 425/286788、bench 22/19908；bound-decode=0 的 core 424/286700、bench 22/19916；实际 Block core 2417/206104、bench 101/16468；fixed-class Function core 424/286260、bench 22/20080；fixed-class Block core 2418/207916、bench 101/16492。Block 比较 host PC/size，不比较含地址的机器码 hash/bytes。

新采八个 PMU 点，CPU4/taskset10，每模式参照2k、候选2k、候选12k、参照12k，未开启 audit/prof/host dump。两点 slope/intercept 相对差：Function instructions +0.013166%/+0.045565%，cycles +4.141476%/−2.336320%，wall +3.827457%/−2.090978%；Block instructions −0.145390%/+0.260933%，cycles −0.152499%/−0.640361%，wall +3.521620%/−4.166643%。这轮的 cycles/wall 增幅原样保留，不能视为正式无退化证据，也不将变化归因于频率。单轮数据与缺少每事件 running ratio 仍不足以报告收益、因果结论或 IPC；正式性能验收继续待定。

前后观测 governor=walt，current 为 940800、1056000 或 1171200 kHz、max=1171200；cpuss-0 30.1–32.8°C、cpu1-0 30.5–32.4°C、cpu1-6 28.9–30.9°C。该观测不是锁频证明，CoreMark 短运行警告保留，没有为改善数字增加采样轮次。

Android DWARF sizeof：旧 BooleanSelectPlan 48、BooleanSelectAction 3 和 FlagsTokenPlan 64 已消失；新 SelectAction 4、临时 SelectActions 48，FlagsTokenAction 8、临时 FlagsTokenActions 24。新 SelectAction 已含 instruction ID，不能直接把旧三字节字段类型当作原完整 pointer/action pair 大小比较。UnitActions 136→184、GPRBlockPlan 400→368，prepared Storage 992→1040；optimized Storage 728、AllocationPreparation 920 不变。移除每块 shared_ptr、指针列表、稠密数组/map 与增加单元稀疏向量同时发生；sizeof 不等于 capacity、分配次数、完整栈帧或峰值，未测总内存变化，不据此报告总内存下降。

source digest 为 747202e06d6e2403540ee455412f729ae195316d700521c3a91107947a509aa7，archive SHA-256 为 938675058f3eee7a407f5e813cd28c7a5948fa96d302ba9bd800513d83d6376c。svm_predicate_token_actions_v1 SHA-256 为 e57d435b27f6dc2b5805db235d301e829222cb9cf6f165a6953b043fc19c7136，相邻 svm_scalar_fpr_actions_v1 为 9260afabe2434d60de52b6c6be9fc432619c8b09ea80a96493c87e152e65c55a。源码/archive 成员、九个主机/构建二进制、八个设备候选与参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致。

完整结果位于 /private/tmp/swiftvm-middle-results/r5-predicate-token-actions-v1-*，设备原始日志位于 /data/local/tmp/svm-middle-r1/results。quality-gate 的 26 项本批结构/正确性/身份检查通过，full_goal_complete=false、formal_three_way_performance_accepted=false；其中 PMU 检查只验证采集有效，不代表接受性能变化。R1 必需摘要/workspace、R2 worklist/类型/提交契约、R3 剩余 emission/scratch 契约、R5/R6 其余动作/调用方、R7 清理/完整适用矩阵，以及可复现 Android FEX 与正式 R0–R7 验收继续保留在范围内。

## R5 窄位与 Self-XOR 统一动作

本批最终为 r5-scalar-bit-actions-v2，相邻参照为 r5-predicate-token-actions-v1。narrow-bit 与标量 Self-XOR/输入省略作为一批完成后集中验证。旧 alloc/narrow_bit_plan.{h,cpp}、jit/scalar_copy_analysis.{h,cpp}、NarrowBitPlan、ScalarCopyAnalysis、GPRBlockPlan::narrow_bits 及 ScalarPreparationPlan::copies 均已删除；实现归 compile/lower_scalar.{h,cpp}。原持久的七个 narrow map、tied-extension set、两份 copy 指针 set 与 narrow shared_ptr 已退役；resident/raw-carry 聚合及其余家族尚在，完整 R5/R0–R7 未完成。

三个窄位 matcher 的准入段、ProducesZeroUpper32 白名单、IsNarrowLoadZeroExtended 链检查、tied zero-extend 的实际 GPR 同 home/spill/preserved/width-chain 条件，以及 Self-XOR Equivalent/References 主体均与相邻源码作归一化核对。顺序仍为 extension、mask、shift-mask、tied-extension 四段，保留原 blocked/discarded/flagged 与 earlier coverage 的优先级，未把多次扫描或家族播种收敛误报为完成。两份短期 pointer membership set 只表达早先 extension-shift 和 masked-shift 对后续规则的排除，不进入 emission，也不恢复反向 map。

LowerNarrowBits 只读 AllocationView，输出四字节 NarrowAction：编号、kind 和宽度/offset/high-zero 的紧凑参数。原三个公开 source/witness 结构变为 cpp 内部短期匹配结果，移除未再使用的 Value 副本与比较操作；冻结结果不保存 source 指针、Value 或另一份编号到指令索引。NarrowAction::Source 只沿当前指令已证明的两条 raw operand 边读取来源，extension 后的 LSR 为三条；不跳过中间 view，不 ResolveBitCastSource，不重建图或重匹配 opcode，保持原 Value 的 type view。实际寄存器仍在同一发射点通过冻结物理视图读取，因此 preserved/use-point 语义不被提前压成一个全局 home。

LowerScalarCopies 显式消费 SemanticFacts 已有的唯一块索引，保持深度8的类型/BitCast/BitExtract 等价与逐 occurrence 引用计数。只有属于本块的 input definition 会输出 CopyInputDiscarded；Self-XOR 对外部相同输入仍可成立，但不能将外部定义的编号误作用到当前块的同 ID 节点。手工用例在调用前显式编号并构建索引，生产入口没有新增隐式 ReId/分析构建。

四类 SelfXor、CopyInputDiscarded、BitInputCovered、TiedZeroExtend 标记写入已有 u16 证明表的 bit11–14；表元素宽度没有继续扩大。窄位参数保存在 UnitActions 的单元稀疏向量，准备期 ScalarActions 只含动作与标记两份列表；排序、重复标记合并和动作 ID 唯一性在准备/封口完成。Begin/Abort 清空 payload，查询继续按同一 allocation 实例、generation、attempt 与 frozen 状态拒绝失效结果。

省略仍在原 EmitBitExtract、EmitLsr/Asr、EmitZeroExtend、EmitXor/ClearFlags 的位置消费。它们不写入 InstructionCovered，不绕过指令入口的 spill、scratch 或 metadata 处理；zext_no_elide 对 ZeroExtend32To64 tied 省略的原条件仍在。没有借这次存储变化统一提前跳过指令。Self-XOR 结果及逻辑 flags 的原编码主体、窄位 UXT/UBFX 和 Mask 的普通编码主体保留。

既有 lower-coverage、narrow-memory native 编码、Self-XOR zero/flags、跨块保留、独立候选顺序与 barrier/spill/覆盖拒绝用例改读公开动作结果，不保留旧 map/analysis 兼容查询。原 lowering_coverage_test.cpp 新增一个真实动作生命周期用例，同时建立 DeferredExtend/ExtractShift/Mask/ShiftMask、Self-XOR 与 tied extension；确认 U64 definition 上的 U32 operand view 保留、各省略位不变成主循环覆盖、冻结查询与 reset 失效。原 Self-XOR 用例还补充外部 extract 与本块 shadow 编号相同的情形，确认只标记本块结果。没有新增测试文件、测试脚本、probe 或生产测试 hook。

相邻变动 19 个现存/新增源文件，新增 lower_scalar 两个路径、退役四个 tracked 文件；快照 420 个现存修改/新增源文件、61 个相对 HEAD 删除路径。v1 编译在测试阶段发现替换范围错误：scalar_publication_plan_test.cpp 的 resident 分支也使用 analysis 变量名，被机械替换成 copy 调用。v2 将 resident/raw-carry 整段逐字节恢复为相邻基线，生产源码完全不变；失败快照和日志保留。v1 没有启动设备测试。

v2 九个 Android 目标构建成功，冻结后红魔 bf1359ff 上仅运行一轮 36 个核心场景，全部符合预期退出状态，最终 Ninja 为 no work。八个 RV 语法命令实际在 v1 执行通过；v2 只修复上述非 RV 测试段，生产及 RV 源/头保持相同，因而明确复用并保留实际 v1 命令/日志，而不冒称重跑。

主套件现为 716 用例、221333 断言，12 个失败用例、67 个失败断言、0 errors。剔除且仅剔除新增动作生命周期用例后，原有 715 个有序用例、classname 后缀和失败类型/消息/文本与参照一致，只归一化 binary 前缀和 cpp/cc/h 行号；新增用例通过。候选/参照种子 796746156/3135606271，总断言 +196；既有断言修改和种子变化同时存在，不报告汇总数相等或作单一归因。IR default/IR_FAST=0 各 40 用例/842 断言、PIN_EXT=3 定向契约两用例/39 断言通过。lifetime 九模式 × 两种 IR 模式逐项检查/注入计数、entry 950、call 233、shutdown 两模式 105 项一致。RV 原生、static-latch 四个既有失败和完整 shutdown 矩阵未在本批执行。

十组候选形态为本批运行，静态参照复用相邻版本真实候选日志并保留路径、命令和依据；PMU 没有复用。七组完整有序记录与原始 shape 相同。bound-decode=0 core、默认 Block core、fixed-class Block core 的 roster 差异均在同一 guest ELF 的 __d2b_D2A（0x253b00，大小 0x342）与 __dtoa（0x25f2d0，大小 0x1017）。十组每个共同 PC 的 occurrence 签名、排除这两个精确符号范围后的完整有序记录一致；两个 Block core 的共同 PC 全局首次出现顺序也在格式化路径内变化，不能报告其完整 common-PC 子序列相等。pool/IR arena、CoreMark CRC 与五项 bench checksum 一致，没有为了对齐参照重跑。

候选记录数/全局 host_bytes：默认 Function core 425/286788、bench 22/19908；bound-decode=0 core 425/286792、bench 22/19916；实际 Block core 2416/206164、bench 101/16468；fixed-class Function core 424/286260、bench 22/20080；fixed-class Block core 2415/207692、bench 101/16492。Block 仍比较 host PC/size，不比较含地址的机器码 hash/bytes。

新采八个 PMU 点，CPU4/taskset10，每模式参照2k、候选2k、候选12k、参照12k，未开启 audit/prof/host dump。两点 slope/intercept 相对差：Function instructions −0.038332%/+0.140139%，cycles −0.565094%/+2.244707%，wall +0.606150%/−1.291718%；Block instructions +0.066009%/−0.522343%，cycles +1.609968%/−0.653906%，wall +2.311791%/−3.742935%。单轮只作诊断，不据此宣称收益、无退化或因果结论；未取得每事件 running ratio，不报告 IPC。

前后观测 governor=walt，current 为 940800、1056000 或 1171200 kHz、max=1171200，cpuss-0 29.3–33.2°C、cpu1-0 30.9–33.2°C、cpu1-6 28.9–30.9°C；不构成锁频证明，也不将性能变化归因于这些观测。CoreMark 短运行警告保留，未增加轮次以改善数字，正式性能验收继续待定。

Android DWARF sizeof：旧 NarrowBitPlan 208、ScalarCopyAnalysis 80 已消失；ScalarPreparationPlan 320→240，NarrowAction 4、临时 ScalarActions 48、Flag 4；NarrowBitBuilder 232→152，内部 ExtractExtensionMatch 24。UnitActions 208、GPRBlockPlan 352，分别相对参照 +24/−16；prepared Storage 1040→1064、optimized Storage 728 和 AllocationPreparation 920 不变。移除持久 map/set/shared_ptr 与增加单元稀疏向量同时发生；对象 sizeof 不等于完整栈帧、capacity、分配次数或峰值，未测总内存变化，不报告总内存下降。

最终 source digest 为 287e384826263b19855c8344b8e06295598497ac45db42106ffc810fd61a7402，archive SHA-256 为 aeae3d42426866462e70f762cea0a28d7bb2eabf3c43ab0ca08cd82081d70e33。svm_scalar_bit_actions_v2 SHA-256 为 92cb21b9d47afcb36447daea5c750744a34f2ced672a2adbdac7a195f2d6b597，相邻 svm_predicate_token_actions_v1 为 e57d435b27f6dc2b5805db235d301e829222cb9cf6f165a6953b043fc19c7136。源码/archive 成员、九个主机/构建二进制、八个设备候选与参照/tool/两个 guest 已核对，十二项设备哈希运行前后一致。

完整结果位于 /private/tmp/swiftvm-middle-results/r5-scalar-bit-actions-v2-*，设备原始日志位于 /data/local/tmp/svm-middle-r1/results。quality-gate 的 29 项本批检查通过，full_goal_complete=false、formal_three_way_performance_accepted=false；PMU 检查只确认采集有效。R1 必需摘要/workspace/候选扫描收敛、R2 worklist/类型/提交契约、R3 剩余 emission/scratch 契约、R5/R6 其余动作/调用方、R7 清理/完整适用矩阵，以及可复现 Android FEX 与正式 R0–R7 验收继续保留在范围内。

## R5 resident-FPR 与 raw-carry 统一动作

本批为 r5-resident-carry-actions-v1，相邻参照为 r5-scalar-bit-actions-v2。resident-FPR 与 raw-carry 作为一批迁入 compile/lower_vector.cpp、compile/lower_flags.cpp 后集中验证一次。旧 jit/resident_scalar_fpr_analysis.{h,cpp}、jit/raw_carry_branch_analysis.{h,cpp}、alloc/scalar_preparation_plan.h 五个文件及其构建项、类型、查询与导入调用均已退役。ScalarPreparationPlan 聚合和每块 shared_ptr 删除，剩余六个持久 pointer map/set 不再保存到 prepared 结果；完整 R5/R0–R7 未完成。

resident conversion/store 并入既有四字节 ScalarFPRAction，新增 Conversion/ConversionExport/Store 三类，覆盖复用 bit10。Load/Value/Conversion/Store 查询按种类隔离，避免新增类型被旧 Load 查询误认。准备期仍按 conversion、resident read store、extract store 的优先级处理，保留 zero-high、bitcast/raw-use 类型、单用、目标覆写、opaque barrier 和首个匹配语义；精确 conversion use 与后缀窗口复用已有 BlockAnalysisIndex。只新增对 conversion 属于本块的检查，避免外块同编号 producer 生成本块动作。后续 integer flags/narrow 的 coverage 仍在原来的依赖位置读取这些 ID，没有提前改变规则优先级。

LowerCarryConditions 显式接收唯一块索引，不再保留无索引时反复全块 SoleUser 扫描的第二路径。TestFlags 的 consumer/terminal 准入、形状与顺序、CFINV/FLAGS_REGS/FlagM 配置以及 producer→SaveFlags 和 TestFlags→最终 consumer 两段 NZCV/guest-observation 窗口保持。八字节 CarryAction 只保存 TestFlags/combine/invert ID 及 packed/raw 条件；不保留未消费的 predicate/other 指针。反转省略使用 u16 证明表最后一位 bit15，在原 EmitInvertCarry 中消费，没有并入主循环 Covered，原 pre-instruction transport 和 pending host-C 记账时机保留。发射局部条件表改用 prepared unit 中唯一的指令 ID，实际 raw_carry_pending 仍为发射状态；RecordLocalCondition 的其他入口继续按 ConsumerPlan 检查准入。

现有 resident/raw-carry 用例接续新接口，补充 action 种类隔离、冻结后查询、ResetAllocations 失效以及反转省略不属于主循环覆盖的断言；没有新增 probe 或测试脚本。raw-carry 原双路径对照改为共享索引与独立构建索引结果相等，失效/未准入 consumer 仍拒绝动作。

九个 Android 目标首轮构建退出 0，再次 Ninja 报 no work to do；八个 RISC-V 相关语法编译单元均对本批冻结源码新执行并通过。红魔 bf1359ff 上 36 个核心场景集中执行一次，退出码均符合既有预期。主套件 716 个用例、221166 条断言，12 个失败用例/67 条失败断言/零 error；与相邻参照的全部有序用例、classname 与失败类型/消息/正文相同，没有排除上一批新加入的类型视图用例。本批断言数少 167，seed 分别为候选 1150348342、参照 796746156；用例接口/断言调整和不同随机种子均存在，不把总数变化归于单一原因。IR 两种模式各 40/842，PIN3 contracts 为 2/39；寿命九种模式×fast 两种的计数与注入位置相同，entry/call/shutdown 分别 950/233/105。构建九目标不等于执行九目标；RV 原生、已知失败的 static latch 和完整 shutdown 矩阵本批未执行。

静态对照执行十个新候选，参照复用相邻不可变二进制的十份候选原始日志，保留原命令与时间，PMU 数据另行采集。七组完整有序记录与 raw shape 相同。core 的记录数为 425→424、全局 host bytes 为 286788→286696；block-core 为 2416→2412、206164→205572；fixedclass-block-core 为 2415→2415、207692→207688。这三组 roster 差异全部落在精确 ELF 范围 __dtoa（0x25f2d0，size 0x1017）与 __d2b_D2A（0x253b00，size 0x342）中；全部共同 PC 的每次出现签名、格式化范围以外的完整有序记录相同。两个 Block core 的共同 PC 全局顺序在格式化函数内也不同，不声称这三组全流相等，不重跑筛选 roster。其余候选记录数/全局 host bytes：bench 22/19908，boundoff-core 425/286792，boundoff-bench 22/19916，block-bench 101/16468，fixedclass-core 424/286260，fixedclass-bench 22/20080，fixedclass-block-bench 101/16492。各组 pool/arena 指标及 guest CRC/五个 checksum 与参照相同。

PMU 在 CPU4 上对两种模式各执行新的一组 ref2k/cand2k/cand12k/ref12k，工具、guest、绑核和无 prof/audit/host dump 参数一致。八点 counters 与 CRC 有效。候选相对参照的斜率/截距变化如下；截距含启动和 guest 行为，不是独立编译耗时。

| 模式 | instructions 斜率/截距 | cycles 斜率/截距 | wall 斜率/截距 |
| --- | ---: | ---: | ---: |
| Function | −0.145588% / +0.130995% | −2.468260% / +3.389707% | −2.103679% / +3.540308% |
| Block | +0.005072% / +0.538915% | −2.167705% / −0.380941% | −1.876096% / −2.962130% |

各点前后均记录 walt、current 940800/1171200 kHz、max 1171200 kHz 和温度；cpuss 29.3–33.2°C、cpu1-0 30.1–32.8°C、cpu1-6 28.5–30.9°C。读数不证明锁频。单组数据只作诊断，不认定收益、无回归或因果，不在缺少各 event running-time ratio 时推导 IPC；低于 CoreMark 计分时长的警告保留，不引用正式分数。正式三方性能验收继续待固定可复现的 Android FEX 源码及完整协议。

DWARF 确认 ScalarPreparationPlan（旧 240）、ResidentScalarFPRAnalysis（旧 120）和 RawCarryBranchAnalysis（旧 120）从候选消失。ScalarFPRAction 仍为 4 字节，CarryAction 为 8，准备期 ScalarFPRActions 为 48、CarryActions 为 24、ResidentLowering 为 56。UnitActions 208→232，GPRBlockPlan 352→336，prepared Storage 1064→1088；optimized Storage 728、Allocation 776、AllocationWriter 912、AllocationPreparation 920 和 ARM64 JitTranslator 3320 不变。统一 carry 向量使每个 UnitActions 对象增长 24 字节，不能把每块旧对象删除直接等同于总内存下降；sizeof 也不包含动态容量、总 frame 或峰值内存。

冻结清单包含相对 HEAD 的 415 条新增/修改 source 路径与 66 条删除；相邻批次变更 24 条、退役 5 条。源码摘要为 26728552def70d0812afa2469dcb09cf8e0988ba81d2ff64f5eaf277328b4c6e，overlay 归档 SHA-256 为 4ae892c88d358e798e95d824df77533b5dcf1f3dca6afa2969d3183d8ce9da80。svm_resident_carry_actions_v1 SHA-256 为 d0ddb475d8a2eec20dc65619e4771bbda270fb50df940926c8233b6edb65ba64，相邻 svm_scalar_bit_actions_v2 为 92cb21b9d47afcb36447daea5c750744a34f2ced672a2adbdac7a195f2d6b597。源码/archive 成员、九个主机/构建二进制、设备八候选/参照/tool/两 guest 的十二项哈希核对，设备前后一致。

结果保存在 /private/tmp/swiftvm-middle-results/r5-resident-carry-actions-v1-*，设备原始日志仍在 /data/local/tmp/svm-middle-r1/results。quality-gate 的 29 项本批检查通过，batch_complete=true、full_goal_complete=false、formal_three_way_performance_accepted=false。构建退出状态来自实际工具完成结果，Ninja 最后一条进度为 [148/149]，不以进度计数作为成败条件。R1 必需摘要/workspace/候选扫描、R2 worklist/类型/guest 提交/effects、R3 剩余 emission/scratch 契约、R5/R6 memory/consumer/integer flags/branch/edges/其余发表和调用方、R7 清理与完整适用矩阵及正式 R0–R7 验收继续在范围内。

## R5 memory 统一动作与 preservation 只读索引

本批最终为 r5-memory-actions-v3，相邻参照为 r5-resident-carry-actions-v1。窄加载、RCpc offset、post-index writeback 和 GetOperand 别名覆盖一起迁入 compile/lower_memory.{h,cpp} 后集中上机验证。alloc/memory_lowering_plan.{h,cpp}、MemoryLoweringPlan 及三个旧 payload 类型、GPRBlockPlan::memory、每块 shared_ptr、三个持久 map、address_aliases/covered 指针列表与 emitter 导入均已退役。其余 preindex/biased/pinned load update、spilled EA、consumer/integer flags/branch/edges/发表仍在，完整 R5/R0–R7 未完成。

每个访问最多保存一条八字节 MemoryAction；narrow 和 post-index 可共存，offset 与 post-index 按原条件互斥。offset 只在原 simm9 检查通过后缩为 s16；direct narrow 目标来自相同 WholeValue 查询，原条件已保证为 GPR 且在访问/extension 两点同 home，因而发射直接使用记录的寄存器号，不再保存 extension 指针。X/W 查询在该条件下只有只读 GPR 查询，没有 spill 或 scratch 副作用。普通/共享/固定 extension 保留原目标路径。OffsetBase 只读取 memory→EA→base 的封口 operand 边，保留原始 use 类型视图，不重新匹配或构建图。写回的 update/owner/base 以八字节 Write 仅交给准备期 residence 验证，不进入 UnitActions；别名只记录必要覆盖，不保留无发射用途的单独列表。覆盖复用 bit10，integer flags 等后续规则仍在原依赖位置读取。

LowerMemory 显式借用唯一 BlockAnalysisIndex，不再提供隐式自建图的第二路径；原 lazy resolved-use 触发、三节点 update 候选窗口、raw/resolved use 计数、观察/覆写窗口、固定发表相邻条件、RCpc/biased/Q、home overlap 和 spill/preservation 条件保持。现有独立调用测试显式构建自己的索引，与共享索引比较完整动作和验证 witness。

preservation 冲突查询原本依赖 const writer 内的惰性缓存，本批将同一缓存提为公共 compile/allocate/preservation_index.{h,cpp} 的 GPRPreservationIndex，writer 仍是唯一所有者。索引不可复制/移动，构造与失效仅对 writer 开放；lowering 接收 AllocationView 与该索引，并断言二者指向同一物理结果。索引不进入 prepared 结果，没有新增全图副本或回调；区间排序、合并与查询主体经字段/所有者重命名和空白规范化后与旧主体一致。原五处失效全部保留：writer_locations 三处、recolor 提交一处、transaction 撤销一处。Freeze 查询仍复用这一索引，构建时机不变。

v1 首轮构建失败，原因是索引提取时漏掉 recolor 和 transaction 对旧 dirty 字段的直接引用；v2 补齐引用后，编译又发现新 MemoryAction.flags 字段与 ARM64 既有 flags 宏冲突，成员访问被替换为 x26。v3 将该字段改为 bits。两次失败的冻结源码、归档、构建日志、状态及各八个通过的语法编译结果保留在各自前缀下；v1/v2 均未运行设备测试、shape 或 PMU。最终 v3 九目标构建退出 0，后续同目标 Ninja 为 no work to do；八个 RISC-V 相关语法编译单元在 v3 冻结源码上新执行并通过。没有新增 probe 或测试脚本。

红魔 bf1359ff 上 v3 的 36 个核心场景集中执行一次，退出码均符合预期。主套件 717 个用例、221385 条断言，12 个失败用例/67 条失败断言/零 error。新增 memory actions preserve sealed offset views and allocation identity 通过，覆盖 EA base 的 U64 use 视图/原 U32 定义、动作种类隔离、冻结后查询与分配 reset 失效；其余 716 个有序用例的 classname 和失败类型/消息/正文与相邻参照相同。本批仅在比较中排除这一新通过用例，没有排除此前两批加入的用例。断言数增加 219，seed 分别为候选 1929497287、参照 1150348342；接口/断言调整和随机种子差异均存在，不将总数变化归为单一原因。IR 两种模式各 40/842，PIN3 contracts 2/39；寿命九模式×fast 两种的计数与注入位置相同，entry/call/shutdown 分别 950/233/105。RV 原生、static latch 和完整 shutdown 矩阵本批未执行。

静态对照执行十个新候选，参照为相邻不可变二进制的十份候选原始日志，保留实际原命令/时间，PMU 不复用。七组完整有序记录和 raw shape 相同。core 为 424→425 条、全局 host bytes 286696→286788；block-core 为 2412→2417、205572→206104；fixedclass-block-core 为 2415→2416、207688→207900。三组 roster 差异全部在精确 ELF 范围 __dtoa（0x25f2d0，size 0x1017）及 __d2b_D2A（0x253b00，size 0x342）内，全部共同 PC 的出现签名、格式化范围外完整有序记录相同；fixedclass-block-core 的共同 PC 全局顺序在格式化范围内也不同，不声称该三组全流一致，不重跑筛选 roster。其余候选记录数/全局 host bytes：bench 22/19908，boundoff-core 425/286792，boundoff-bench 22/19916，block-bench 101/16468，fixedclass-core 424/286260，fixedclass-bench 22/20080，fixedclass-block-bench 101/16492。各组 pool/arena 与 guest CRC/五个 checksum 均与参照相同。

CPU4 两种模式各采集一组新的 ref2k/cand2k/cand12k/ref12k，八点 counters/CRC 有效，完整命令及 UTC 时间保留。斜率/截距相对变化如下；截距含启动和 guest 行为，不等于独立编译耗时。

| 模式 | instructions 斜率/截距 | cycles 斜率/截距 | wall 斜率/截距 |
| --- | ---: | ---: | ---: |
| Function | −0.008128% / +0.061978% | −1.122047% / +4.642187% | −1.791893% / +5.028695% |
| Block | −0.043371% / +0.277621% | −0.161642% / −2.057177% | −0.501451% / −0.503849% |

各点前后均记录 walt、current 940800/1056000/1171200 kHz、max 1171200 kHz 和温度；cpuss 29.3–33.2°C、cpu1-0 30.1–33.2°C、cpu1-6 28.5–30.9°C。这些读数不证明锁频。单组只作诊断，不认定收益、无回归或因果；缺少各 event running-time ratio 时不推导 IPC，短运行警告保留，不引用正式 CoreMark 分数。可复现 Android FEX 与正式三方质量门仍待完成。

DWARF 确认旧 MemoryLoweringPlan（120）及旧 narrow/writeback/offset payload（16/32/24）从候选消失。MemoryAction 为 8 字节，准备期 MemoryActions 为 72、Write 为 8。GPRPreservationIndex 为 784，其中三十二个原有区间向量继续只存一份，新增物理结果所有者引用；AllocationWriter 912→920，AllocationPreparation 920→928。UnitActions 232→256，GPRBlockPlan 336→320，prepared Storage 1088→1112；optimized Storage 728 与 Allocation 776 不变。新向量头和索引所有者引用增加单元/准备对象大小，不能将旧子 plan 删除直接视为总内存下降；sizeof 不包含动态容量、稠密证明表首次启用的成本、总 frame 或峰值内存。

冻结清单包含相对 HEAD 的 417 条新增/修改 source 路径和 68 条删除；相邻参照变更 24 条、退役 2 条。源码摘要为 90db4f62d8005b6d2e1416ea973521130e5f980e783a00897eee643a8fb24b0d，overlay 归档 SHA-256 为 0309d2f9f31004fe107cad5c28c7fa1fa6de8f37b0bc0dc5703a0df2b2ca0934。svm_memory_actions_v3 SHA-256 为 7e8d2ae546736856d72466e357d69facab1a9d4a2857eb9b776e5bea0a735451，相邻 svm_resident_carry_actions_v1 为 d0ddb475d8a2eec20dc65619e4771bbda270fb50df940926c8233b6edb65ba64。源码/archive 成员、九个主机/构建二进制及设备八候选/参照/tool/两 guest 的十二项哈希核对，设备前后一致。

结果保存在 /private/tmp/swiftvm-middle-results/r5-memory-actions-v3-*，设备原始日志仍在 /data/local/tmp/svm-middle-r1/results。quality-gate 的 34 项本批检查通过，batch_complete=true、full_goal_complete=false、formal_three_way_performance_accepted=false。R1 必需摘要/workspace/候选扫描、R2 类型/worklist/guest 提交/effects、R3 剩余 final emission/scratch、R5/R6 其余家族与调用方、R7 清理/完整适用矩阵，以及正式 R0–R7 验收继续在范围内。

## R5 consumer 与 PSTATE 统一动作

本批最终为 r5-consumer-actions-v2，相邻参照为 r5-memory-actions-v3。alloc/consumer_plan.{h,cpp}、pstate_facts.{h,cpp} 四个文件及其构建项、旧类型与调用接口已退役。实现分别归 compile/lower_consumers.{h,cpp} 和 lower_flags.{h,cpp}；GPRBlockPlan、RegionBlockCertificate 中的 consumer shared_ptr、旧持久 map/set、反转指针列表及未消费的 SoleConsumer 图均已删除。完整 R5/R0–R7 仍未完成。

每个相关指令最多保存一条八字节 ConsumerAction，合并 boolean/local/terminal 消费、pair secondary、FCmp compact/raw reader、反转窗口、compound-clear、SSE string demand、PSTATE mask 与 incoming-zero 接收边。related、properties 和 auxiliary 按互斥 opcode 含义解释，编号与链接容量在提交时检查；UnitActions 冻结前排序/查重，冻结后按编号查询。不扩宽已用满的 u16 证明表，也不把 consumer 省略并入主循环 Covered；原 transport、副作用和省略消费时机保持。ALU split 可选审计单独保存十六字节 AluSplitAction，无审计需求时不为普通指令保留该 payload。

LowerConsumers 显式借用唯一 BlockAnalysisIndex，保留 pair 的类型、pseudo multiplicity、先后顺序、foreign use 和反向 owner 约束。无 secondary 的 pair 显式使用无效 related 值，不能与无 pair 混同。FCmp 跨块来源的 IncomingPacked 写在接收 reader/publication 上；即使外块 producer 与本块定义具有相同数字 ID，也不会抹掉本块 raw/compact 资格。incoming zero 先沿原验证器取得根，再只为允许的接收 opcode/参数 slot 留记录；发射核验该封口边的真实 Def，不能仅以数字 ID 认定同一个值。

FCmp 的反转列表改为 producer 编号与窗口奇偶性。准备扫描仍统计窗口中的全部 InvertCarry，偶数窗口也保留起点；查询时只在当前块真实 pending-C 指令位于窗口内时扣除这一项，块间 pending 状态仍重置。PstateEligibility 为准备期 Test 向量和 first-advance 前缀：Test 保留真实 Inst 身份，拒绝外块同 ID 节点；原扫描和 clobber 规则保持。consumer mask 写入动作后不保留前缀或 Test 向量，narrow integer flags 借用同一准备结果，不另外补图。

多结果发射不再需要正向 Inst 指针。AllocationView 增加显式 ID/ValueType 的位置与 value-home 查询，JitContext 增加结果写入及 spill 的 ID 入口；Value 包装保留原始类型与“是否当前定义”的指针身份判断。Div128/Cpuid 的 pair secondary 使用已验证的编号和 U64。四个关键主体在显式 Value→ID/type/definition 替换、注释/空白规范化后与相邻参照相同，证据只覆盖这四个主体。region admission 用同一 UnitActions 实例和 consumer revision 替代旧子 plan 指针身份，并继续校验 allocation/generation/attempt、IR/config/pinned；测试中的替换/恢复入口只在 test support，生产结果保持只读。

v1 的生产目标已编译，主测试编译因遗漏 swift::u32 和 GPRSMask/FPRSMask 命名空间而失败，没有运行设备测试、shape 或 PMU。修正后第二次按变更范围格式化还改动另外三个文件的换行：translator_terminal.cpp、alu_split_plan_test.cpp、scalar_copy_analysis_test.cpp。初始 v2 冻结脚本的“仅一个文件变化”断言失败，随后仍启动了构建 83605；该构建退出 0，期间没有源码修改。核对四个文件后确认除命名空间限定外仅有空白变化，重新生成最终清单/归档，再执行权威冻结九目标 Ninja，退出 0、no work to do。此顺序保留在 first-attempt-delta 和 build-executions，不能声称首次 v2 编译发生在成功冻结之后。八个 RV 相关语法单元在 v1 新执行并通过；v2 的生产/RV 头文件与 RV 源码均未变，故精确复用原命令、时间和日志，没有宣称 v2 新跑八次语法编译。没有新增 probe 或测试脚本。

红魔 bf1359ff 上 36 个核心场景集中执行一次，退出码符合预期。主套件 718 个用例、221376 条断言，12 个失败用例/67 条失败断言/零 error；新增 consumer actions keep inversion windows and receiving-edge ownership 通过，覆盖 0–3 次反转和每个 pending 位置、同 ID 外块 PSTATE/FCmp/zero 来源、冻结查询及 allocation reset。仅排除这一新通过用例后，其余 717 个用例的有序名称/classname 与失败类型/消息/正文与参照相同。断言数 −9；候选 seed 为 2792561715，参照为 1929497287，接口/断言调整与 seed 差异均存在，不归因于单一因素。IR 两模式各 40/842、PIN3 contracts 2/39；寿命九模式×fast 两种的计数/注入点、entry/call/shutdown 的 950/233/105 均相同。RV 原生、static latch 和完整 shutdown 矩阵本批未执行。

十个静态场景只新执行候选；十份相邻参照日志沿用不可变二进制与相同 guest 的原始命令/时间，PMU 不复用。六组完整有序记录和 raw shape 相同；另外四组如下，表中 host bytes 为完整日志汇总，包含 guest 输出函数的编译差异。

| 场景 | 记录数：参照→候选 | 全局 host bytes：参照→候选 |
| --- | ---: | ---: |
| Function core | 425→424 | 286788→286696 |
| boundoff core（仍是 Function） | 425→424 | 286792→286700 |
| Block core | 2417→2415 | 206104→206260 |
| fixed-class Block core | 2416→2414 | 207900→207616 |

四组 roster 差异全部在精确 ELF 范围 __dtoa（0x25f2d0，size 0x1017）和 __d2b_D2A（0x253b00，size 0x342）内；全部共同 PC 的逐次出现签名相同，范围外完整有序记录相同。两个 Block core 场景在这些范围内连共同 PC 的全局次序也不同，不声称四组全流一致，不重跑筛选 roster。其余候选记录数/host bytes：bench 22/19908、boundoff-bench 22/19916、block-bench 101/16468、fixedclass-core 424/286260、fixedclass-bench 22/20080、fixedclass-block-bench 101/16492。十组 pool/arena 与 guest CRC/五个 checksum 均相同。

CPU4 分别采集 Function 与实际 SVM_FUNC_BASE=0 Block 的新 ref2k/cand2k/cand12k/ref12k，八点的两个 counters 与 CRC 均有效。斜率/截距的候选相对变化如下；截距包含启动和 guest 行为，不是独立编译耗时。

| 模式 | instructions 斜率/截距 | cycles 斜率/截距 | wall 斜率/截距 |
| --- | ---: | ---: | ---: |
| Function | −0.137192% / +0.450493% | −1.173841% / +1.159871% | −0.025624% / −1.426374% |
| Block | +0.050403% / −0.187556% | +1.030608% / +0.917426% | +0.536990% / +1.399348% |

逐点前后记录 walt、current/max 均为 1171200 kHz，温度 cpuss 29.3–32.4°C、cpu1-0 29.7–32.4°C、cpu1-6 28.9–30.1°C。读数不证明锁频，单组不认定收益、无回归或因果；缺少各 event running-time ratio 时不推导 IPC。短运行警告保留，不引用正式 CoreMark 分数；固定源码 Android FEX 和正式三方质量门仍待完成。

DWARF 确认旧 ConsumerPlan（416）、PstateFacts（48）和 BooleanConsumerAction/PairResultAction/FCmpAction（各 16）消失。ConsumerAction 为 8、AluSplitAction 为 16、AluSplitFacts 仍为 12；准备期 ConsumerActions 为 96、PstateEligibility 为 32、Test 为 16、ConsumerBuilder 为 128。UnitActions 256→312、GPRBlockPlan 320→304、RegionBlockCertificate 112→96、RegionPreparationPlan 520→536、prepared Storage 1112→1168；optimized Storage 728、AllocationWriter 920、AllocationPreparation 928、两后端 JitContext/JitTranslator 均不变。单元向量头和 region 身份字段增加大小，不能把旧容器删除直接视为总内存下降；sizeof 不含动态容量、总 frame 或峰值。准备期仍有一个临时行 map，R1 工作区与候选收敛继续保留。

冻结清单包含相对 HEAD 的 415 条新增/修改 source 路径和 72 条删除；相邻参照变更 40 条、退役 4 条。源码摘要为 13445a81205edb266fa9ea9e39b0950a9ffcaa2698ed33ea0d4b783dc4341b35，overlay SHA-256 为 f00b9f6596d9f333186a08290bbe455c2303dd09a307abb92b46990b4d9637f7。svm_consumer_actions_v2 SHA-256 为 23ad069e15113b5ad199d1ef6a8ab683b3b81405050e88bb9fe654a851b016b6；参照 svm_memory_actions_v3 为 7e8d2ae546736856d72466e357d69facab1a9d4a2857eb9b776e5bea0a735451。归档成员、源码、九个主机/构建二进制和设备十二项身份核对，设备前后一致。hash-after 的时间字段明确为首次命令响应的观察时间，不伪作启动时间。

证据保存在 /private/tmp/swiftvm-middle-results/r5-consumer-actions-v2-*，设备原始日志仍在 /data/local/tmp/svm-middle-r1/results。quality-gate 的 33 项本批检查通过，batch_complete=true、full_goal_complete=false、formal_three_way_performance_accepted=false。接续 R1 必需摘要/候选/workspace、R2 type/worklist/guest commit/effects、R3 emission/scratch、R5/R6 余下动作与调用方、R7 清理/完整矩阵和正式 R0–R7 验收。

## R5 窄整数 flags 与 dead-edge branch 统一动作

本批最终为 r5-integer-actions-v2，相邻参照为 r5-consumer-actions-v2。alloc/integer_branch_plan.{h,cpp}、narrow_integer_flags_plan.{h,cpp} 与 jit/translator_narrow_integer_flags.cpp 共五个文件及其构建项、旧类型和 API 已退役。窄 carry、窄比较和 low-extract 接收边归 compile/lower_integer_flags.{h,cpp}；dead-edge branch 归 lower_predicates.{h,cpp}。三个持久 map、两组 covered/discarded 集合、carry dead-input 指针列表和两个每块 shared_ptr 不再进入冻结结果；完整 R5/R0–R7 未完成。

窄 flags 每个相关指令只保留一条八字节 IntegerFlagsAction。种类区分 Input/InputLow、CarryLeft/CarryRight、CompareImmediate/CompareValue；u32 argument 在互斥种类中表示接收者编号或比较立即数，options 表示比较宽度与是否沿 extract source 边取值。输入记录精确接收者，并核验真实参数边及所属块；carry/compare 按已证明的有界封口边读取原始 ValueType，保留 X/W 与窄视图，不保存 Value、正向 Inst 指针或另一份 use 图。旧 comparison_inputs 临时 map 也改为借用 input 动作 span。匹配过程仍有准备期私有候选 map/set 和 carry dead-input 向量，交接后释放，后续按 R1 继续收敛，不能声称所有临时容器已清除。

dead-edge branch 每块最多一条十二字节 IntegerBranchAction，保存 producer/condition 编号、raw condition、所需 flags、可选 CBZ home/宽度与窄立即数。立即数在原编码资格通过后缩为 u32；编号转换前核验 producer、condition、discarded 和加载均是当前块真实节点。NZCV producer 后窗口与 CBZ publication→terminal 窗口、physical home、dirty/preservation、fault/helper 和窄宽度条件保留。查询先核验当前块成员身份，再按 condition 编号查找；emitter 每次 ImportGPRBlock 只借用一次冻结动作指针，避免每个消费点重复查询。branch 的 home 冲突缓存仍只读借用 AllocationPreparation，独立只读索引与未消费上下文参数的清理继续接续 R1/R7。

原已导入动态 bitmap 的 carry/test、discarded 和立即数加载改用已有 bit10；主循环与辅助扫描继续共用 InstructionCovered。input extract 仍在 EmitBitExtract 原位置读取动作后省略，未提前移入主循环 Covered，保留原 transport。旧 integer flags/branch 的覆盖导入已删除，条件记账保留，原 backedge 状态断言也保留；动态 bitmap 的剩余写入用于 backedge polarity 两个实际节点。静态准备仍在 Backedge/PFAF 之前，不改变这些晚期家族的需求或覆盖准入。PSTATE 与唯一 BlockAnalysisIndex 显式共享，narrow flags 只读 AllocationView。

v1 首轮九目标构建失败于两处：候选类型的机械重命名误将 HasDeadEdgeIntegerBranchProof 改为不存在的 HasEdgeCandidateProof；新增测试尝试通过只读指令列表调用 SetId。修复前已确认构建进程终止，保留 v1 冻结源码/归档、构建日志、状态及八个通过的语法编译结果；v1 没有执行设备场景、shape 或 PMU。v2 恢复正确查询名，并以另一个真实 BranchFixture 验证同编号外块，未放宽 IR 只读边界。只对这两个文件的变更范围格式化，再生成最终清单/归档，九目标构建退出 0；后续同目标 Ninja 退出 0、no work to do，期间无源码编辑。

八个 RV 相关语法单元在 v1 新执行并通过；v2 生产/RV 头文件和 RV 源码均逐字节不变，精确复用原命令、时间和日志，不宣称 v2 新跑八次语法编译。v2 的规则审计只按完整候选类型标识符改名，保留 API 调用名，再规范注释/空白；14 处 unchanged-body/compare 前后分段检查通过。输入投影和紧凑转换不在这些“主体未变”的声明内，另由现有发射/运行时用例和新接收边回归验证。没有新增 probe 或测试脚本。

红魔 bf1359ff 上 36 个核心场景集中执行一次，退出码符合预期。主套件 719 个用例、221345 条断言，12 个失败用例/67 条失败断言/零 error。新增 integer flags actions preserve sealed input views and receiving identity 通过，覆盖原始 U64 use/U8 定义、InputLow、同编号外块值、动作种类隔离与 reset 失效；原 branch 生命周期用例补充冻结查询、外块同编号拒绝与 reset。仅排除这一新通过用例，其余 718 个有序名称/classname 与失败类型/消息/正文与相邻参照相同。断言数 −31，候选 seed 2689585684、参照 2792561715；接口/断言调整与 seed 差异并存，不归于单一原因。IR 两模式各 40/842、PIN3 contracts 2/39；寿命九模式×fast 两种的检查数/各模式注入次数相同，entry/call/shutdown 为 950/233/105。RV 原生、static latch 和完整 shutdown 矩阵本批未执行。

十个静态候选新执行，十份参照复用相邻不可变候选的原日志、命令和时间，PMU 不复用。七组完整有序记录和 raw shape 相同。其余三组如下，host bytes 为完整日志汇总，包含输出格式化路径的单元增减。

| 场景 | 记录数：参照→候选 | 全局 host bytes：参照→候选 |
| --- | ---: | ---: |
| Function core | 424→425 | 286696→286788 |
| Block core | 2415→2414 | 206260→205872 |
| fixed-class Block core | 2414→2415 | 207616→207628 |

三组 roster 差异全部在精确 ELF 范围 __dtoa（0x25f2d0，size 0x1017）和 __d2b_D2A（0x253b00，size 0x342）内；全部共同 PC 的逐次出现签名与范围外完整有序记录相同。两个 Block 场景在这些范围内的共同 PC 全局顺序也不同，不声称三组全流一致，不重跑筛选 roster。其余候选记录数/host bytes：bench 22/19908、boundoff-core 424/286700（仍是 Function）、boundoff-bench 22/19916、block-bench 101/16468、fixedclass-core 424/286260、fixedclass-bench 22/20080、fixedclass-block-bench 101/16492。十组 pool/arena 与 CRC/五个 checksum 均与参照相同。

CPU4 两模式各新采集 ref2k/cand2k/cand12k/ref12k，八点 counters/CRC 有效。候选相对斜率/截距变化如下，截距包含启动和 guest 行为，不是独立编译耗时。

| 模式 | instructions 斜率/截距 | cycles 斜率/截距 | wall 斜率/截距 |
| --- | ---: | ---: | ---: |
| Function | +0.003338% / +0.121452% | +0.135724% / +0.491664% | −1.066766% / +1.325086% |
| Block | −0.074091% / +0.376054% | −4.386248% / +7.204032% | −4.951261% / +7.932960% |

每点前后记录 walt、current/max 均为 1171200 kHz；cpuss 29.3–32.4°C、cpu1-0 29.7–32.4°C、cpu1-6 28.5–30.1°C。读数不证明锁频，单组只作诊断，不认定收益、无回归或因果；缺少各 event running-time ratio 时不推导 IPC，短运行警告保留，不引用正式 CoreMark 分数。可复现 Android FEX 源码/构建与正式三方质量门仍待完成。

DWARF 确认旧 IntegerBranchPlan（120）、DeadEdgeIntegerBranch（72）、DeadNarrowImmediateBranch（40）、IntegerFlagPreparationPlan（112）、NarrowIntegerFlagsPlan（88）、NarrowFlagsInput（24）和旧 carry/compare payload（56/40）消失。新 flags/branch 动作为 8/12 字节，准备期输出为 48/40；IntegerFlagsInputs 112→104，私有 builder 152、NarrowCandidates 88 和 carry/compare 候选 56/40 保留原算法的临时容器。UnitActions 312→360、GPRBlockPlan 304→272、prepared Storage 1168→1216，ARM64 JitTranslator 3320→3328（新增当前块动作指针）；RV JitTranslator 1440、两后端 JitContext 1384/936、optimized Storage 728、writer/preparation 920/928 与 region 结构不变。向量头和 emitter 缓存增加对象大小，不能将旧子 plan 删除直接视为总内存下降；sizeof 不含动态容量、总 frame 或峰值。

冻结清单含相对 HEAD 的 413 条新增/修改 source 路径和 77 条删除；相邻变更 23 条、退役 5 条。源码摘要为 1db1d48f54d605cc531723c8fe9e33d7ca80c39ca480b99bf902ce4e4c44c32c，overlay SHA-256 为 e770a9158cad8959714483a769c08c8414294b7931d2697deac6b5e771f5fb83。svm_integer_actions_v2 SHA-256 为 0db7cbc9f2186014436689db10900ccadd483011a7af28aa99e8289c362a4308；参照 svm_consumer_actions_v2 为 23ad069e15113b5ad199d1ef6a8ab683b3b81405050e88bb9fe654a851b016b6。源码、归档成员、九个主机/构建二进制、设备十二项身份核对，设备前后一致。

证据在 /private/tmp/swiftvm-middle-results/r5-integer-actions-v2-*，设备日志在 /data/local/tmp/svm-middle-r1/results。quality-gate 的 34 项本批检查通过，batch_complete=true、full_goal_complete=false、formal_three_way_performance_accepted=false。R1 摘要/候选/workspace、R2 type/worklist/guest commit/effects、R3 剩余 emission/scratch/只读输入边界、R5/R6 余下 GPR/pinned、pseudo demand、flags-only spill、memory update 与 region/edge 家族和调用方、R7 清理/完整矩阵及正式 R0–R7 验收继续在范围内。

## R5 GPR 发表与 memory 选择、旧 coordinator 退役

本批最终为 r5-gpr-memory-actions-v3，相邻参照为 r5-integer-actions-v2。GPRBlockPlan 的 publications、addresses、memory_values、load_updates、load_update_instructions、preindex_updates、biased_updates 七个直接指针 map 及四类旧 payload 已退役；原 GPRPlanBuilder 的准备期 recipes、FrozenGPRPlan、pseudo/flags-only spill 与 region/backedge 结果仍在范围内，不能把本批当成完整 R3/R5/R7。

alloc/target_allocation_planner.{h,cpp} 两个文件、ARMTargetPlanner 类和 CompleteARM64TargetPlanning 包装已删除。私有会话归 compile/internal.h 的 TargetPreparation，块声明/单元调度归 lower.cpp，coalesced host-write、pinned load/value/address、preindex/biased update 和 publication-only zero-extend 规则归 residences.cpp；GPR 发表选择归 publications.cpp。公开的 CompleteARM64UnitForEmission、ARM64UnitReadyForEmission、ARM64StandaloneHIRCapacity 声明和主体归 compiler.{h,cpp}，调用方使用真实公开头。compiler 在重试循环内创建短期会话，必要 FPR contract retry 与公共 Finish 保留；lower.cpp 目前仍承接部分必要契约/冻结编排，最终分层继续接续。

发表直接生成八字节 PublicationAction：kind、target/source home、width/bit-offset、clear/adjust/adopts-spill/fused-source 为紧凑字段，biased decrement 只有原 1–256 资格通过后才缩为 u16。普通值或已证明的 fused source 沿一/两条封口边读取原始 ValueType，不保存 Value 或正向 Inst 指针。select/spilled/load-update/dead-write、transfer/copy、biased/preindex、coalesced 和普通值的优先级保持；发表省略与 PF 候选仍按原指针顺序消费，UnitActions 最后才按编号排序。

load、update-add、preindex、biased 和 pinned-store value 合用八字节 MemoryTransferAction；角色分离的 related 编号保留 load→update、update→publication/access、extract→store 关联，旧 load 双向 map 的首次插入行为保持。地址使用十六字节 AddressAction，偏移保留完整 s64。实际 target/home、dirty/preservation、全局/局部 use、宽度、helper/fault、观察/覆写与邻接窗口继续在准备期证明；临时节点候选仅在调用栈内保留。

准备期 MemorySelections 只拥有两份紧凑行向量、一份按 u16 ID 的稀疏 slot 索引及当前块身份。slot 区分 transfer/address/load-publication，并保存仅供准备使用的 publication 链接；不建立第二份 use 图，不分配 B×unit-size 稠密表，不将索引/链接交给 prepared 结果。行存储私有，Record/LinkPublication 维护索引与首次选择优先，Reset 清空逻辑状态。v1 审查发现线性 Record/查询会令大量候选重复扫描，v2 改为该共享索引，以平均常数查询替代这条新引入的平方级路径；没有将此项源代码分析伪作设备性能实测。容量复用和显式 workspace 仍待 R1。

省略保持在原 EmitAdd/EmitSub/EmitBitExtract/EmitGetOperand/SetHostGPR 方法内，未提前并入主循环 Covered，也没有扩宽 u16 证明表。动态 spilled-memory 状态仍持有实际临时寄存器/偏移，接收者改为当前块编号，保持每块清空；查找须匹配实际 access。pinned value 还核验 store 的真实参数边及当前块身份，拒绝同编号外块值。Decode 可以播种形状、立即数与宽度候选，最终 home、省略、写回和保留区间选择仍属于 pass 后封口/分配阶段。

v1 九目标构建失败于 region_preparation_plan.cpp：它依赖旧 GPRBlockPlan 间接包含 VIXL 寄存器声明；v2 的首次显式 include 又选用了此版仓库不存在的 registers-aarch64.h。核对实际目录后，v3 改为存在的 operands-aarch64.h。两次失败均确认进程终止后才编辑，保留各自冻结源码/归档、构建日志和状态；v1/v2 均没有执行设备场景、shape 或 PMU。v3 冻结后九目标构建退出 0，后续同目标 Ninja 退出 0、no work to do，期间无源码编辑。

v1/v2 各新执行八个 RV 相关语法单元并通过。v3 仅改动一个 ARM64 cpp include 路径，头文件和 RV 源码逐字节不变，精确复用 v2 的实际命令、时间和日志，没有宣称 v3 新跑八次。24 处具名规则、准入主体/返回前片段和发表 emitter 的显式表示替换对照通过；初次审计误匹配调用点与候选返回名，已改为带声明的函数签名和显式类型替换，first-read 结果另存。索引、编排、紧凑转换与作用域检查不在“主体未变”声明内，另由现有运行/发射用例和新增回归约束。没有新增 probe 或测试脚本。

红魔 bf1359ff 上 36 个核心场景集中执行一次，退出码符合预期。主套件 720 个用例、221602 条断言，12 个失败用例/67 条失败断言/零 error。新增 publication and memory selections preserve views offsets and scoped receivers 通过，覆盖原始 use 类型与不同定义类型、fused/普通发表、s64 大偏移、角色与接收边、外块同 ID、首次选择、load/publication 反向关联、frame reset 和 allocation reset。仅排除这一新通过用例，其余 719 个用例的有序名称/classname 与失败类型/消息/正文与参照相同。断言数 +257，候选 seed 3791318210、参照 2689585684；接口/断言调整与 seed 差异并存，不将总数变化归于单一因素。IR 两模式各 40/842，PIN3 contracts 2/39；寿命十八模式组合的检查数/注入次数、entry/call/shutdown 的 950/233/105 相同。RV 原生、static latch 和完整 shutdown 矩阵本批未执行。

静态对照新执行十个候选，十份参照来自相邻不可变候选的原始日志，保留命令与时间，PMU 不复用。七组完整有序记录和 raw shape 相同，另外三组如下；host bytes 为完整日志汇总，包含浮点输出路径的单元变化。

| 场景 | 记录数：参照→候选 | 全局 host bytes：参照→候选 |
| --- | ---: | ---: |
| Block core | 2414→2412 | 205872→205572 |
| fixed-class Function core | 424→425 | 286260→286352 |
| fixed-class Block core | 2415→2412 | 207628→207308 |

三组 roster 差异全部位于精确 ELF 范围 __d2b_D2A（0x253b00，size 0x342）和 __dtoa（0x25f2d0，size 0x1017）；全部共同 PC 的逐次出现签名、范围外完整有序记录相同。仅 fixedclass-block-core 在这些范围内还改变共同 PC 的全局次序，不声称三组全流一致，不重跑筛选 roster。其余候选记录数/host bytes：core 425/286788、bench 22/19908、boundoff-core 424/286700（仍是 Function）、boundoff-bench 22/19916、block-bench 101/16468、fixedclass-bench 22/20080、fixedclass-block-bench 101/16492。十组 pool/IR arena 与 CRC/五个 checksum 相同；这些池统计不覆盖所有 metadata 容器。

CPU4 两模式各新采集 ref2k/cand2k/cand12k/ref12k，八点 counters/CRC 有效。候选相对斜率/截距变化如下，截距包含启动和 guest 行为，不是独立编译耗时。

| 模式 | instructions 斜率/截距 | cycles 斜率/截距 | wall 斜率/截距 |
| --- | ---: | ---: | ---: |
| Function | +0.118691% / +0.037969% | +1.421989% / −1.408112% | +1.172814% / −2.946389% |
| Block | +0.111695% / −0.635463% | +2.273139% / −3.187669% | +2.080878% / −0.015863% |

每点前后记录 walt、current 940800/1171200 kHz、max 1171200 kHz；cpuss 29.3–32.8°C、cpu1-0 30.1–31.3°C、cpu1-6 28.5–30.1°C。这些读数不证明锁频，单组不认定收益、无回归或因果；缺少各 event running-time ratio 时不推导 IPC。短运行警告保留，不引用正式 CoreMark 分数；当前斜率增加作为诊断如实记录，正式三方质量门仍需固定 Android FEX 源码/构建并按完整协议验收。

DWARF 确认旧四类 payload 与 ARMTargetPlanner 消失。PublicationAction/MemoryTransferAction/AddressAction 为 8/8/16 字节，旧发表/load-update/memory-update/address payload 为 32/40/40/24。MemorySelections 为 96、Slot 为 6，私有 store/load/source 候选为 24/40/8。GPRBlockPlan 272→104，UnitActions 360→432，prepared Storage 1216→1288；准备会话 ARMTargetPlanner 2264→TargetPreparation 2384，PendingGPRBlock 208→328；动态 SpilledMemoryOperand 32→24。FrozenGPRPlan 312、optimized Storage 728、writer/preparation 920/928、两后端 JitContext 1384/936、JitTranslator 3328/1440 与 region 结构不变。单元向量头、准备会话和待交接 frame 增大，不能将持久 map 删除直接视为总内存下降；sizeof 不含动态容量、总 frame 或峰值。

冻结清单含相对 HEAD 的 414 条新增/修改 source 路径和 79 条删除；相邻变更 36 条、退役 2 条。源码摘要为 46990968cfb62d0311b55948bf3d0efd6555f2535aa0f8ca6dba28c873f5546e，overlay SHA-256 为 5c247134a2abf1e64f7b7728076c9be9ef07ca1e5c78f6acfe0b6d6fbe57abcd。svm_gpr_memory_actions_v3 SHA-256 为 46c43c3c17f89232e3e20e9338d7320ded3b2cee8e5f3aa31eb40d71366f4290；参照 svm_integer_actions_v2 为 0db7cbc9f2186014436689db10900ccadd483011a7af28aa99e8289c362a4308。源码、归档成员、九个主机/构建二进制及设备十二项身份核对，设备前后一致。

证据在 /private/tmp/swiftvm-middle-results/r5-gpr-memory-actions-v3-*，设备原始日志在 /data/local/tmp/svm-middle-r1/results。quality-gate 的 39 项本批检查通过，batch_complete=true、full_goal_complete=false、formal_three_way_performance_accepted=false。后续继续必需摘要/候选/workspace、type/worklist/guest commit/effects、FrozenGPRPlan 与剩余 emission/scratch 契约、pseudo/flags-only spill、region/edge/backedge/PFAF、剩余 pinned 准备与调用方，以及 R7 清理/完整矩阵和正式 R0–R7 验收。

## R5 pseudo demand 与 flags-only spill 统一结果

本批最终版本为 r5-flags-actions-v2，相邻参照为 r5-gpr-memory-actions-v3。GPRBlockPlan 的 pseudo_flags、flags_only_spills 两个拥有数据的成员已经删除，旧 flags_only_spill_plan.h 已退役；当前旧块结果只保留 pinned、region、backedge_flags 三个 shared_ptr。这完成本家族结果迁移，FrozenGPRPlan、region/edge 与其他阶段目标继续推进。

公共 analysis/pseudo_flags.h 提供十六字节 PseudoFlagsView，统一解释已有 packed word，保留 set/clear、BranchOnly 和单独的 SaveFlags parity 位。准备规则直接读取共享 BlockAnalysisIndex，不再通过 GPRPlanBuilder::CopyPseudoFlags 复制。冻结时按 active block 的 ID span 追加到 UnitActions 的共同 backing，原块表中增加八字节 offset/first/last；空块不追加 words，总 span 不超过 MapCount，保留完整 u16 ID 边界，没有 B×unit 稠密矩阵。ImportGPRBlock 绑定一次视图，普通算术发射仍按 ID 常数时间读取。视图是借用，生命周期由 prepared unit 和原 admission 契约保证；它不保留 consumer 图，也不单独检测代际。

flags-only spill 的单一 flags consumer、原始重复边计数、producer 类型和最终 MEM residence 条件保持原样。GPRPlanVerificationInput 不再夹带逐块可变输出指针，GPREmissionCertificates 显式汇总 width/low32/read 与四字节 consumer/producer ID 对，随后交给 UnitActions。关闭可选 audit 时仍执行 PrepareGPREmissionCertificates，正常发射不能丢掉这些结果。封口时排序去重，查询核对真实 consumer 所属 block；pending spill、required backing、FPR 与既有 transport 时机保持。该资格依赖最终 mapping，不能移到 decode。decode 只能播种 presence/关联，最终 pseudo demand 在重写与编号后由共享事实汇总。

TargetPreparation 已移除两个只为旧块 flags 容器服务的成员；旧 ReleasePseudoFlags 也删除。isolated encoder 的测试辅助接口返回带有数据所有权的 flags，由调用方保留到编码结束，没有给生产 translator 添加兼容拥有者。原压力回归现在直接断言 UnitActions 的 spill 资格，不再通过 requires 条件静默跳过。

### 构建与正确性

v1 首轮构建退出 1：两个测试辅助接口仍按旧块表/旧 pseudo 容器访问，新用例另有 Operand/寄存器 mask 类型限定错误。该轮八个语法单元通过，但没有设备执行。构建失败后因完成状态判断错误复制过本地二进制，已在 v1-binaries.json 标为无效，未部署或用于验证；失败源码归档、构建日志和状态保留。修正辅助调用方和类型限定、删除最后的 ReleasePseudoFlags 后冻结 v2。v2 九个 Android 目标全部构建成功，同一 Ninja 目标的最终确认退出 0/no work；八个 RISC-V 语法单元重新执行并通过。

红魔 NX729J（bf1359ff、CPU4）执行 36 个场景。主套件 721 cases、221739 assertions、12 个失败用例、67 个失败断言、0 errors；候选 seed=361525892，参照 seed=3791318210。仅排除新增且通过的 prepared flags own block slices and scoped spill certificates 后，原有 720 个用例的完整顺序、类名后缀归一后的失败类型、消息与文本一致；断言总数增量 137，不将变化全部归因于新增用例。新增用例覆盖语义索引销毁后的持有权、反向追加的稀疏块 ID span、SaveFlags/BranchOnly parity 区别、同 ID 外部块、reset/rebuild/abort 和 u16 边界。原 flags-only spilled XOR 用例继续验证真实分配、存活用途和 native 结果。

IR 默认/slow 各 40 cases、842 assertions；PIN_EXT=3 契约 2 cases、39 assertions；18 个 fast/slow 寿命模式及 entry/call/shutdown 的原检查计数、注入次数和输出均一致。构建了 RISC-V 与 static-latch 目标，但本批没有执行 RISC-V native、static-latch 或完整 shutdown 故障矩阵，不能据此声称 R7 已完成。

### 静态记录与对象布局

十个新候选静态场景全部退出 0；十份相邻参照复用其原候选的不可变日志，命令、原时间戳和二进制身份均保留。五组 bench 完整有序记录与完整 RA shape 相同。五组 CoreMark 的完整记录和 aggregate shape 不同，差异如下；host bytes 为主 RA shape 汇总，单独记录逐 PC host 合计，两个口径不混用。

| 场景 | 编译记录数：参照→候选 | RA host bytes：参照→候选 |
| --- | ---: | ---: |
| Function core | 425→424 | 286788→286696 |
| bound-decode OFF core（Function） | 424→425 | 286700→286792 |
| Block core | 2412→2417 | 205572→206104 |
| fixed-class Function core | 425→424 | 286352→286260 |
| fixed-class Block core | 2412→2415 | 207308→207688 |

所有 roster 变化都在 __d2b_D2A（0x253b00，size 0x342）与 __dtoa（0x25f2d0，size 0x1017）精确 ELF 范围内。全部共有 PC 的逐次出现签名及范围外完整有序记录一致；Block core 在格式化范围内还有共有 PC 的全局顺序变化。不声称五组完整流一致，不重跑以筛选相同 roster。十组 pool/IR arena 统计和 CRC/五个 checksum 一致。首次解析误要求短跑出现长跑验证语句，first-read 文件保留；按原短跑契约核对 CRC 4983，并保留 10 秒警告/Errors detected 原文，没有重跑。池统计不覆盖全部 metadata 堆。

DWARF 显示 GPRBlockPlan 104→48、UnitActions 432→480、prepared Storage 1288→1336、TargetPreparation 2384→2360、ARM64 JitTranslator 3328→3344 字节；优化结果 Storage 728、FrozenGPRPlan 312、ARM/RV JitContext 1384/936、RV JitTranslator 1440 不变。块表 mapped value 从 shared_ptr 的 16 字节变为 BlockActions 24 字节，原 flags-only use 的 pointer/u32 记录由 16 字节改为四字节 ID 对，PseudoFlagsView 为十六字节。数据容器容量、节点分配、准备峰值和完整 frame 尚未计量，不把这些 sizeof 相加当成总内存或峰值收益。

### PMU 与证据

两模式各新采集 ref2k/cand2k/cand12k/ref12k，八点 cycles/instructions/wall 和 CRC 有效；2000/12000 次 CRC 分别为 4983/d340。候选相对斜率/截距变化如下，截距仍包括启动和 guest 行为。

| 模式 | instructions 斜率/截距 | cycles 斜率/截距 | wall 斜率/截距 |
| --- | ---: | ---: | ---: |
| Function | +0.932364% / −3.388048% | +2.731903% / −5.697421% | +0.050326% / −2.662301% |
| Block | +0.037509% / −0.294990% | +2.051439% / −3.396896% | +2.247189% / −3.444848% |

每种模式仅一轮括号采样，只作诊断，不接受为正式收益、无退化或因果结论。walt，CPU4 current 940800/1171200、max 1171200；cpuss 28.9–32.8℃、cpu1-0 30.1–32.8℃、cpu1-6 28.1–30.9℃。这些读数不证明锁频；缺少 per-event running ratio，不推断 IPC；截距不是独立编译耗时。短跑警告完整保留，没有官方 CoreMark 分数。

候选 launcher SHA256=c45ace7e6a55d98050fad5d87d0d43be1ae71b4096b842b8592570dee06b822e；相邻参照=46c43c3c17f89232e3e20e9338d7320ded3b2cee8e5f3aa31eb40d71366f4290。设备前后十二项二进制/工具/guest 身份相同。冻结源码相对 HEAD 包含 414 个修改/新增路径、80 个删除路径，相邻批次改变 21 个源文件并退役一个头文件；source digest=076e1855e969204d0f954620031ab52337b936d31f13a9a7d8949764d6e0812f，archive SHA256=8e1983ceff77335e9d0d06999e021f78a6e88a801c445b767a9db33634937712。构建与设备验证后逐文件检查无漂移；完整采集命令、执行结果、日志、源码归档与布局证据在 /private/tmp/swiftvm-middle-results/r5-flags-actions-v2-*，设备原始日志在 /data/local/tmp/svm-middle-r1/results。

本批 quality-gate 的 35 项检查通过，batch_complete=true，full_goal_complete=false，formal_three_way_performance_accepted=false。继续 R1 必需摘要/候选/workspace、R2 type/worklist/guest commit/effects、R3 FrozenGPRPlan 与剩余 emission/scratch/spill 契约、R5/R6 region/edge/backedge/PFAF 和剩余 pinned/callers，以及 R7 完整清理/矩阵和可复现 Android FEX 三方验收。

## R3 发射事实与审计拆分、FrozenGPRPlan 退役

本批最终为 r3-emission-facts-v4，相邻参照为 r5-flags-actions-v2。FrozenGPRPlan 类、frozen_gpr_plan.h、GPRPlanBuilder::Freeze 与 GPRPlanDraft 已退役，没有旧头、转发类或别名。正常发射不再持有全部 operand 的 32 字节快照、逐块 residence 指针 map/set、physical-write vector 或独立 scratch owner；GPRBlockPlan 现在只剩 region/backedge 两个 shared_ptr，整个旧目录的退役仍未完成。

### 文件职责与必要契约

- compile/residences.h 仅声明准备期 GPRResidenceFacts 和 GPRBlockPreparation。准备规则仍用真实节点身份完成物理准入，最终只将四字节 residence 记录交给 UnitActions；零/符号扩展、省略 GetHostGPR 与 select home 保持原发射位置和原始类型视图。
- compile/scratch.h/.cpp 同时容纳既有分配输入描述与最终发射准备，二者共用原预算/clobber/lease 规则。LowerScratch 显式借用共享块 use 图，不再另扫 SaveFlags 并维护 owner-demand 线性查找表。SaveFlags(None) 仍贡献 presence/count；Function 普通分配仍保留跨块 mapping，块局部发射需求不使用单元级 flags 摘要代替。
- UnitActions 保存一次单元 scratch、每指令二十字节 scratch 记录及只针对实际 MEM 输入的十六字节证书。后者保存 consumer/producer ID、精确 producer 身份和 last-use，避免发射时重新搜索 Lambda/链式 Params。重复边计数、外部同 ID 拒绝与 required backing/FPR/transport 时机保持；原始 use 类型继续由封口 IR 的真实边提供，没有持久 Value/VIXL 对象或 consumer 图。
- JitContext 在导入 block 时借用二十四字节 InstructionScratchView，顺序运输按 cursor O(1)，跳过/非单调编号按 ID 二分，foreign block 被拒绝。密集 scratch 已有序时线性封口，仅乱序时排序；没有扩宽 u16 证明表，也不为每块创建 unit 大小的数组。
- gpr_plan_verifier 的输入只借用 block/index 与短期 target-write span。PrepareGPREmissionCertificates 和完整 audit 都在所有 target 编辑结束后执行包含 target writes 的最终 fixed 检查，再生成 width/low32/read/spill 证书。完整类型/位置审计直接访问定义值和封口参数边，包括 Lambda/Params，不保留全 operand 表。公共最终化仍有基础 fixed 检查，后续须凭完整覆盖与代际证明合并重复工作，不能跳过最终 target-write 契约。
- RegionBlockCertificate 用原块结果的身份指针替代 FrozenGPRPlan shared_ptr，先验证 owning UnitActions 的实例/代际再比较；不形成 region 与块结果的强引用循环。测试中的旧快照接口已接入真实准备提交，准备期字段不再冒充拥有物理结果的发射对象。

原 scratch.cpp 的分配输入描述曾在组装文件时被覆盖，在首轮构建前已从相邻冻结归档原样恢复。本批又逐函数重新核对 DescribeInstructionScratch、DescribeScratchInput、SpillScratchOnlyGPRs 及六项既有 scratch/手动准备规则，全部一致；恢复和最终源码证据保留。

### 构建与正确性

v1 构建失败：flags 宏把新字段访问改写为 x26，memory 测试另缺少完整准备期类型。v2 将字段改为 bits，但测试 include 放在文件末尾，使用处仍不可见。两轮各自八个 RISC-V 语法单元均通过，未复制或部署失败构建产物，设备执行为零。v3 移动 include 后九个目标构建成功并已部署，但没有执行任何 native/static/PMU 场景；运行前成本审查发现密集 scratch 无条件排序，v4 只在 ARM64 actions.cpp 增加已排序判断。v4 九目标构建成功，最终同目标 Ninja 退出 0/no work。v3/v4 准确复用 v2 的八份语法记录：所有 runtime 头和 RV 源文件逐字节相同，保留原命令、时间戳与日志，不声称又执行了新语法检查。

红魔 NX729J（bf1359ff、CPU4）只对最终 v4 执行一组 36 个场景。主套件 722 cases、221791 assertions、12 个失败用例、67 个失败断言、0 errors，seed=97400554；相邻参照 seed=361525892。按 case-renames.json 中四个明确重命名映射，并仅排除新增且通过的 prepared scratch keeps local demand and exact repeated spill inputs 后，原有 721 个用例的顺序、类名后缀归一后的失败类型/消息/文本一致。四个旧快照用例现验证真实 UnitActions/AllocationView 的类型视图、借用/代际、确定性和 foreign 边界。断言总数变化 +52，同时存在用例重写与不同 seed，不全部归因于新增用例。

新增用例用独立手动准备扫描作参照，覆盖零掩码 SaveFlags、本块无 save 而其他块有 save、Lambda/Params 的原始 U8/U64 边与重复次数、存活用途、跨块用途、外部同 ID 冲突和索引销毁/reset 后的查询。原 native flags-only XOR、Div128、pressure、持久化与寿命用例继续使用生产结果。

IR 默认/slow 各 40 cases、842 assertions；PIN_EXT=3 契约 2 cases、39 assertions；18 个 fast/slow 寿命模式及 entry/call/shutdown 的检查计数、注入次数和输出一致。RISC-V、static-latch 和其他目标已构建，但本批没有执行 RISC-V native、static-latch 或完整 shutdown 故障矩阵，不能据此声明完整 R7 通过。

### 静态记录与布局

十个候选静态场景新执行，十份相邻参照复用其不可变候选日志，保留原命令/时间戳。五组 bench 与 bound-decode OFF 的 Function core 完整有序记录、完整 RA shape 一致。其余四组有差异，主 RA shape 的 host bytes 与逐 PC 合计分别保存，口径不混用。

| 场景 | 编译记录数：参照→候选 | RA host bytes：参照→候选 |
| --- | ---: | ---: |
| Function core | 424→425 | 286696→286788 |
| Block core | 2417→2416 | 206104→206128 |
| fixed-class Function core | 424→425 | 286260→286352 |
| fixed-class Block core | 2415→2413 | 207688→207320 |

所有 roster 差异只在 __d2b_D2A（0x253b00，size 0x342）和 __dtoa（0x25f2d0，size 0x1017）的精确 ELF 范围内。所有共有 PC 的逐次出现签名、范围外完整有序记录相同；Block core 在这些范围内还有共有 PC 的全局顺序差异。不声称四组完整流相同，不重跑筛选 roster。十组 pool/IR arena 与 CRC/五个 checksum 一致；CoreMark 2000 次的 CRC 为 4983，短跑警告和 Errors detected 原文保留，没有官方分数。池统计不覆盖全部 metadata 堆。

DWARF 已找不到 FrozenGPRPlan/OperandPlan。GPRBlockPlan 48→32、RegionBlockCertificate 96→88、TargetPreparation 2360→2352；UnitActions 480→560、块表 mapped value 24→32、prepared Storage 1336→1416 字节。residence/ScratchAction/SpillInputAction 分别为 4/20/16 字节，InstructionScratchView 为 24 字节；原 scratch 单条记录为 24 字节，原 OperandPlan 每个定义/参数都用 32 字节。GPRResidenceFacts 144 和 GPRBlockPreparation 176 仅在准备期存在，PendingGPRBlock 328、RegionPreparationPlan 536、优化结果 Storage 728、AllocationPreparation 928、ARM/RV JitContext 1384/936、ARM/RV JitTranslator 3344/1440 不变。向量容量、map 节点、整单元/峰值和完整 frame 未计量，不将 sizeof 相加宣称总内存或峰值收益。

### PMU 与证据

两模式各新采集 ref2k/cand2k/cand12k/ref12k，八点 cycles/instructions/wall 和 CRC 有效；2000/12000 次 CRC 分别为 4983/d340。候选相对斜率/截距变化如下。

| 模式 | instructions 斜率/截距 | cycles 斜率/截距 | wall 斜率/截距 |
| --- | ---: | ---: | ---: |
| Function | +0.004140% / +1.041118% | +0.385769% / +0.473665% | +1.484399% / −0.638680% |
| Block | −0.052636% / +1.700753% | +0.090833% / +0.585616% | +0.498128% / +0.364927% |

每模式仅一轮括号采样，只作诊断，不接受为正式收益、无退化或因果结论。采样读数为 walt、CPU4 current/max 均为 1171200；cpuss 28.9–32.8℃、cpu1-0 30.5–32.4℃、cpu1-6 28.9–30.5℃。读数一致不证明锁频；缺少 per-event running ratio，不推断 IPC；截距含启动与 guest 行为，不是独立编译耗时。八份短跑警告完整保留。

候选 launcher SHA256=1e01be20508b15905a60c7899e01e252b3eef9fc0e46c5882180bc1710b1ba2d，相邻参照=c45ace7e6a55d98050fad5d87d0d43be1ae71b4096b842b8592570dee06b822e；设备前后十二项二进制/工具/guest 身份一致。源码相对 HEAD 为 415 个修改/新增路径、81 个删除路径，相邻批次改变 36 个源文件并退役一个头文件。source digest=57e5a65959d0f33f8c1b4aa3b4576a76b4589281e6b1b362617ba8c98145075d，archive SHA256=1250ea71986e874d71afc89fc14628c25ebbd572d6f8e7498f23a909ff2629b1。构建及设备验证后逐文件无漂移。

证据在 /private/tmp/swiftvm-middle-results/r3-emission-facts-v4-*，设备原始日志在 /data/local/tmp/svm-middle-r1/results；失败/被替代版本的源码归档、状态和日志均保留。quality-gate 的 42 项本批检查通过，batch_complete=true、full_goal_complete=false、formal_three_way_performance_accepted=false。继续完整 R1/R2/R3/R5/R6/R7/R0，尤其是剩余工作区/扫描、type/worklist/guest commit、最终契约编排、region/edge/backedge/PFAF、旧 pinned 与调用方，以及完整矩阵和可复现 Android FEX 三方验收。

## R6 单元边动作与入口证书、GPRBlockPlan 退役

本批最终为 r6-unit-edges-v3，相邻参照为 r3-emission-facts-v4。已删除 GPRBlockPlan 及 region_preparation_plan、backedge_flags_plan、region_pfaf_plan、edge_flags_analysis 共九个旧头/实现文件；对应 CMake 项和调用方已迁移，没有旧头转发、兼容别名或 RetainBlock 包装。块级与 Function 级分配入口及活跃性规则继续分别保留。

### 文件与所有权

| 文件 | 本批职责 |
| --- | --- |
| compile/admission.h/.cpp | 唯一必要 UnitCertificate：配置/scratch、actions 实例与 consumer revision、物理 allocation/generation/attempt、完整 IR/CFG 和入口 provenance。先验证 CFG 闭合归属，再做 canonical/edge 分析；前驱和后继名单连续存放。证书不属于可选 audit |
| compile/edges.h/.cpp | 最终 EdgeActions、BackedgeAction、PFAFAction、AtomicActions 与排序查询；共享纯 terminal visitor。边、cycle cuts、canonical entries 不再用持久 map/set 或第二份 internal-location 名单 |
| compile/edge_flags.h/.cpp | 原 flags edge/acceptance、透明性和 token overwrite 规则；目标 fault/helper 谓词显式传入，纯规则不含当前机器 flags 状态 |
| compile/lower_edges.h/.cpp | 必要证书捕获之后准备 region、回边、PFAF、backward terminal candidates 与可选 CFG 报告；保留 RPO 首次插入、重复 guest location 的身份语义、原 DFS 顺序及 mask/fault/helper 窗口。两个候选 flags-dead 目标使用栈上 span |
| compile/lower_atomics.h/.cpp | 按最终 mapping、原宽度/result-alias/address-bias 规则准备未对齐原子 fallback。四字节指令动作直接索引十二字节 key/count cohort；临时计数 map 留在准备期 |
| compile/actions、allocation_preparation | UnitActions 独占证书及各动作表，十二字节 BlockActions 仅保存局部范围、scratch 和 backedge 索引。DeclareBlock/StoreEdges/StoreAtomics/StoreBackedge 显式提交，不为每块创建 shared_ptr |
| arm64/jit | 借用 prepared unit 中的动作与证书，保留当前 NZCV/PF/AF/token、VIXL 标签、helper 冷路径、poll 和恢复编码。动态 Module 可链接性仍在实际发射时查询 |

backedge 的选中指令使用单元 ID，省略 polarity load/store 使用独立位，保留完整 u16 指令编号域。PFAFOperand 保留完整 u64 immediate、kind/value/offset 比较顺序及原覆写规则，producer 抑制仍检查本块真实身份。入口证书以稳定块表记录地址识别当前注册；过期证书先被 owning actions/物理代际检查拒绝。需要跨重置检验旧证书的测试保存测试自有值快照，不继续使用失效借用指针。

可选 density/hot-coalesce 的 CFG 报告不再由 emitter 重扫 terminal：准备期保存原始目标顺序、重复项和顶层 CheckHalt 的 external 标记。纯 terminal visitor 仍供必要动态发射规则使用。IsStrictInternalAdvancePC 的条件性 flags audit 尾部扫描尚未迁移，其他候选/CFG/workspace 合并继续属于 R1；本批不把所有 emitter 分析视为完成。

### 构建、修正与正确性

v1 九目标构建失败于原子 lowering/test 缺少寄存器定义头，以及诊断测试仍从已拆分的 AtomicActions 读取 options；没有部署或设备执行。v2 修正后九目标构建成功，八个 RISC-V 语法单元全部通过，执行 36 个真机场景。主套件出现新增 error 并最终中止：原子 lifecycle 夹具把第二块 ID 平移为整个单元的连续编号，却仍按单块最大容量分配数组。JUnit 明确记录 id < alloc_result.size() 检查失败；最终错误日志和 v2 的全部执行结果保留。

v3 仅将该夹具容量改为两块合计，所有 runtime 源文件/头和八个 RV 语法输入与 v2 逐字节相同。九目标构建成功，最终同目标 Ninja 退出 0/no work；语法检查准确复用 v2 的命令、时间戳与日志。九份最终二进制中，只有 swift_test 与 v2 不同，其余八份 SHA-256 相同。为减少重复设备测试，v3 只新执行主套件和 fixedclass-contracts，另外 34 个成功命令按二进制完全一致复用；原始命令、版本名、时间戳与日志不改写成新运行。

最终 36 个验证命令均按预期退出。主套件 722 cases、221529 assertions、12 个失败用例、67 个失败断言、0 errors，seed=873119422；相邻参照为 722/221791、12/67/0，seed=97400554。全部 722 个用例的顺序、归一后类名以及 failure/error 类型、消息、正文一致，没有排除用例或隐藏新增失败。本批存在夹具/API 迁移和不同 seed，断言总数变化不作单一原因归因。IR 默认/slow 各 40 cases、842 assertions；fixedclass 为 2/39；18 个 lifetime 模式及 entry、call、shutdown 的 checks/injected_failures 与参照一致。新增的 AtomicFor 空指针/外部同 ID 拒绝检查在既有 lifecycle 用例内通过。

本批构建但未新增执行 RISC-V native、独立 static latch 和完整 shutdown/fault 矩阵；这些不能由九目标构建或当前 36 条命令代替，继续在 R7 范围内。

### 静态对比与可选报告

十组静态场景中，五个 bench 加 boundoff Function CoreMark 的全部有序记录与完整 RA shape 相同。其余四组差异只在固定 ELF 的 __d2b_D2A（0x253b00，size 0x342）及 __dtoa（0x25f2d0，size 0x1017）范围；所有共同 PC 的逐次签名和范围外完整顺序均相同。普通和 fixedclass Block 的共同 PC 全局顺序在这两个范围内也有变化，完整差异已保存，没有补跑以匹配 roster。

| 场景 | 记录数，参照→候选 | RA host_bytes，参照→候选 | per-PC host_bytes 合计，参照→候选 |
| --- | ---: | ---: | ---: |
| Function CoreMark | 425→424 | 286788→286696 | 284168→284076 |
| Block CoreMark | 2416→2415 | 206128→206260 | 206128→206260 |
| fixedclass Function CoreMark | 425→424 | 286352→286260 | 283600→283508 |
| fixedclass Block CoreMark | 2413→2417 | 207320→208024 | 207320→208024 |

Block 路径的既有 profiler 仍报告 block_units=0；本批不以该计数证明路径。实际模式由显式 SVM_FUNC_BASE=0、无 Function unit/counter 和非空 standalone host dump 共同确认；boundoff 场景仍为 Function。

十组 pool_bytes、ir_arena、CoreMark 2000 的 0x4983 CRC 与五个 bench checksum 均一致。RA 单元总量和逐 PC 合计分别保留，不混为同一统计口径。另新运行参照/候选各一次 density+hot-coalesce bench：完整有序 CFG 报告均为 496 行，其中 287 行边记录；报告全文、unit 记录、完整 RA shape 和五个 checksum 相同，实际覆盖新的准备期报告路径。

### 布局与 PMU

最终 Android DWARF：BlockActions 32→12、PendingGPRBlock 328→312、ARM64 JitTranslator 3344→3320 字节。新 EdgeBlockAction/BackedgeAction/PFAFAction/PFAFOperand/AtomicAction/AtomicCohort 分别为 32/32/40/16/4/12 字节；UnitCertificate 为 312、BlockCertificate 为 88。UnitActions 固定大小由 560 增至 824，prepared Storage 从 1416 增至 1680，新增动作容器的固定成本如实保留；不能仅凭块记录缩小宣称总内存下降。旧 RegionPreparationPlan 为 536 字节，旧 GPRBlockPlan 及 region/backedge/PFAF 类型在最终 DWARF 中不存在。TargetPreparation 2352、AllocationPreparation 928、ARM64/RV JitContext 1384/936 和 RV JitTranslator 1440 不变。

红魔 NX729J/bf1359ff、CPU4/taskset 10；Function 与实际 SVM_FUNC_BASE=0 的 Block 各一组 ref-2000/candidate-2000/candidate-12000/ref-12000，simpleperf 同时记录 instructions/cpu-cycles。八点退出 0，CRC 分别为 0x4983/0xd340。两点斜率/截距相对变化如下：

| 路径 | instructions 斜率/截距 | cycles 斜率/截距 | wall 斜率/截距 |
| --- | ---: | ---: | ---: |
| Function | +0.014297% / −0.206741% | +0.954515% / −2.755021% | +0.162740% / −1.852856% |
| Block | −0.121636% / +0.353770% | −0.557864% / −1.790897% | −0.233202% / −1.688713% |

每点前后读取 governor/current/max 和三路温度：walt，current/max 均 1171200 kHz，cpuss 29.7–32.0°C、cpu1-0 29.7–33.2°C、cpu1-6 29.3–30.5°C。这只是读数，没有另行证明频率被锁定。每路径只有一个交错括号；本轮仅作诊断，不认定收益、无回归或因果，不报告未验证的有效 IPC/事件运行比例，也不将截距当作独立编译时间。负载低于 CoreMark 正式计分时长，不引用为正式分数；固定、可复现 Android FEX 的三方正式验收仍未完成。

候选 launcher SHA-256=64d4ad68a0960ad5b7db4d34589b42b5e34bc5cb531c8e6ea2b3c77ed8f40af8；参照=1e01be20508b15905a60c7899e01e252b3eef9fc0e46c5882180bc1710b1ba2d。最终设备前后十二项二进制/工具/guest 哈希稳定。冻结源码相对 HEAD 包含 419 个修改/新增路径、90 个删除路径，相邻批次改变 53 个源文件并退役九个；source digest=a2ad75580981b10ce5ede4ba41bdbf9038f2f4f9f48ae3f286550f4b979efce3，archive SHA-256=16726d7a817a89c1535c5183bece36f07100b024a96f41e20cd6477f6201e263。完整证据在 /private/tmp/swiftvm-middle-results/r6-unit-edges-v3-*，失败尝试另保留 v1/v2；没有在仓库新增临时 probe 或测试脚本。

本批 quality-gate 的 44 项检查通过，batch_complete=true，full_goal_complete=false，formal_three_way_performance_accepted=false。本批完成不等于全部重构完成。继续 R1 摘要/候选/workspace/剩余扫描、R2 type/worklist/guest commit/effects、R3 最终只读契约及 fixed 检查编排、R5/R6 剩余 pinned/条件融合/RISC-V/AOT，以及 R7 完整清理/矩阵和 R0 可复现 FEX 三方验收。

## R5 驻留准备与发表选择收敛、旧 pinned 家族退役

本批为 r5-residence-prep-v1，相邻参照为 r6-unit-edges-v3。已删除 pinned_gpr_allocation 的头/实现和 copy、dead_write、narrow_read、publication、select、spilled_publication、transfer 共九个旧文件，迁移 CMake 与生产/测试调用方。GPRPlanBuilder、Recipes、GetRecipes 和旧头均已退役，没有旧类别名或转发包装。旧 alloc 目录现在仅剩 target_analysis_traits、gpr_plan_verifier、gpr_width_verifier、zero_store_verifier 四组头/实现，共八个文件；必要验证尚未整体退役。

### 文件职责和临时存储

| 文件 | 当前职责 |
| --- | --- |
| compile/residence_preparation.h/.cpp | 块局部 ResidencePreparation；只保存后续需要的二十四字节 PublicationSelection、稀疏索引、dead roster 与必要的驻留/写入输入。选择和索引容器跨块复用容量，FinishConstraints 后只把必要输入移给 PendingGPRBlock |
| compile/residence_reads.cpp | MatchNarrowFixedGPRRead、MatchFixedGPRTransfer 等纯匹配，以及窄读取/transfer 的物理 home 提交；共享唯一块 use 图和 guest-state 存活/观察事实 |
| compile/publication_copies.cpp | copy、扩展和低位 ALU alias 的候选闭合及提交；匹配得到的 aliases/transferred_uses 在提交后释放，不复制到子 plan |
| compile/publication_values.cpp | select 发表、低位 publication view 和 MEM 来源发表资格及物理事务；select 不再维护 producer/publication 两份重复 recipe map，publication_views 这个提交后没有读者的 map 直接删除 |
| compile/publication_windows.cpp | 下一写入驱动的 dead publication，以及统一 recolor 的 home/clobber/observer 窗口、必要 scratch/fixed/width 检查和事务回滚 |
| compile/publications.cpp | 只消费显式建立的选择索引和已有 memory selections，按原优先级投影最终八字节 PublicationAction；保留普通 Value、biased/preindex 与 coalesced fallback |

原有六份 recipe map 和一份 dead-write unordered_set 被紧凑选择表、稀疏索引与排序后的真实节点名单替代。候选内部的 alias/consumer 图只活到物理提交，不进入 frozen actions。GuestStateMap::BuildValueVersions 同步借用排序后的忽略名单，查询继续使用实际节点身份，不以同 ID 代替真实归属。必要 GPRResidenceFacts 和最终 target-write 输入仍按原生命周期保留。

Prepare 不再隐式调用 BeginGPRLocationAnalysis 重置物理代际；生产的编辑生命周期由 owning target preparation 明确开启，独立物理夹具使用其已有可变分配状态。IndexPublications 是显式准备边界，const PublicationFor 查询不排序或分配，并拒绝尚未建立索引的调用。记录保持插入顺序，稀疏索引按发表节点和家族优先级排序。

物理写入没有前移：仍等全部编辑结束，在 FinishConstraints 中按原 copy/publication、select/producer、spill/publication 的指针顺序提交，并保留 map::emplace 的首次插入语义。最终发表优先级继续是 select、spill、load publication、dead write、transfer、copy，然后既有 memory/coalesced/Value 分支；source/target/width 等投影字段不变。匹配、recolor 及回滚规则的十三项逐函数 token 核对通过。

### 集中验证与布局

冻结一次源码后，九个 Android 目标首次构建即成功，最终相同目标 Ninja 退出 0/no work；八个 RISC-V 语法单元全部新执行并通过。没有失败构建或补丁轮次，编译期间没有修改源文件。红魔 NX729J/bf1359ff、CPU4 只执行一组 36 个 native 验证命令，全部按预期退出。

主套件为 722 cases、221764 assertions、12 个失败用例、67 个失败断言、0 errors，seed=743017579；相邻版本为 722/221529、12/67/0，seed=873119422。全部 722 个用例的顺序、归一后的类名以及 failure/error 类型、消息和正文完全一致，没有排除用例或新增失败。断言总数变化存在 API 断言迁移与不同 seed，不作单一原因归因。IR 默认/slow 各 40 cases、842 assertions；fixedclass 为 2/39；18 个 lifetime 模式以及 entry/call/shutdown 的 checks/injected_failures 与参照一致。

既有 transfer 用例补充了实际契约检查：索引建立前拒绝查询，索引中 source/target/类型正确，跨块重用后拒绝旧节点，Prepare 不隐式重置 allocation generation。旧的 recipe 容器数量断言已迁为这些行为及原有 location/生成代码检查，没有新增临时 probe 或测试脚本。

最终 Android DWARF：旧 GPRPlanBuilder 为 568 字节，ResidencePreparation 为 448 字节，TargetPreparation 2352→2232。旧 Recipes 固定容器为 184 字节；旧 Copy/Select/View/Spill 候选分别为 96/56/48/24 字节，原来有些还复制保存 alias vectors；现在提交后只保留二十四字节选择行和四字节稀疏索引。以上是固定布局证据，没有据此宣称整个编译的峰值内存或耗时已下降。UnitActions 824、prepared Storage 1680、PendingGPRBlock 312、AllocationPreparation 928、ARM64/RV JitContext 1384/936、ARM64/RV JitTranslator 3320/1440 均不变；旧 GPRPlanBuilder 类型在最终 DWARF 中不存在。

本批未新增执行独立 RISC-V native、static-call latch 和完整 shutdown/fault 矩阵，九目标构建不能代替这些 R7 验收。

### 静态记录和 PMU

十组静态场景中，默认 Function CoreMark 和五个 bench 的全部有序记录及完整 RA shape 相同。其余差异仅位于 __d2b_D2A（0x253b00，size 0x342）及 __dtoa（0x25f2d0，size 0x1017）；共同 PC 的逐次签名和范围外完整顺序全部相同。两组 Block 在这些格式化范围内的共同 PC 全局顺序也有变化，原始完整差异已保存，没有补跑匹配 roster。

| 场景 | 记录数，参照→候选 | RA host_bytes，参照→候选 | per-PC host_bytes 合计，参照→候选 |
| --- | ---: | ---: | ---: |
| boundoff Function CoreMark | 425→424 | 286792→286700 | 284172→284080 |
| Block CoreMark | 2415→2413 | 206260→205992 | 206260→205992 |
| fixedclass Function CoreMark | 424→425 | 286260→286352 | 283508→283600 |
| fixedclass Block CoreMark | 2417→2419 | 208024→207988 | 208024→207988 |

十组 pool_bytes、ir_arena、2000 迭代的 0x4983 CRC 与五个 bench checksum 全部一致。RA 总量与逐 PC 合计分开保存。boundoff 仍为 Function；实际 Block 由显式 SVM_FUNC_BASE=0、无 Function unit/counter 以及非空 standalone host dump 确认，未将既有 block_units=0 计数当作路径证明。

Function 和实际 Block 各一组 ref-2000/candidate-2000/candidate-12000/ref-12000，八个 PMU 点均退出 0，CRC 为 0x4983/0xd340。单轮两点斜率/截距的相对变化如下：

| 路径 | instructions 斜率/截距 | cycles 斜率/截距 | wall 斜率/截距 |
| --- | ---: | ---: | ---: |
| Function | −0.116699% / +0.072105% | +0.319758% / −0.093255% | +1.971521% / −0.877062% |
| Block | +0.109690% / −0.584561% | +1.961618% / −4.170150% | +1.935211% / −2.867172% |

逐点前后读数：governor=walt，max=1171200 kHz，current 出现 940800/1171200 kHz；cpuss 29.3–32.4°C、cpu1-0 30.1–32.8°C、cpu1-6 28.9–30.5°C。当前频率并未始终相同，本批没有锁频或有效 IPC/事件运行比例证明。因此这些数字只用于诊断，不认定收益、无回归或因果，不把截距当作独立编译时间；不因读数波动追加补跑。负载低于 CoreMark 正式计分时长，不引用正式分数；固定、可复现 Android FEX 的三方正式验收仍未完成。

候选 launcher SHA-256=2ed382c12e24579c21080d6299e17c0bc8b2dbe2760d11717413ecbf4a723430，参照=64d4ad68a0960ad5b7db4d34589b42b5e34bc5cb531c8e6ea2b3c77ed8f40af8。设备前后十二项二进制/工具/guest 哈希稳定。冻结源码相对 HEAD 为 416 个修改/新增路径、99 个删除路径；相邻批次修改/新增 16 个源文件，退役九个。source digest=6a067e8c7dcd39dbb0112d4e5ce32e4b091388794ca24be4a492414071d641d2，archive SHA-256=f5c8487d88fddf0742bb451c5be95c3a3694d4b8948296757e2f541c4732a15a。证据位于 /private/tmp/swiftvm-middle-results/r5-residence-prep-v1-*，设备原始日志在 /data/local/tmp/svm-middle-r1/results。

本批 quality-gate 的 45 项检查通过，batch_complete=true，full_goal_complete=false，formal_three_way_performance_accepted=false。本批未完成整个 R1/R3/R5/R6 或正式 R0–R7 验收。继续候选/窗口与 workspace 合并、必要 fixed/FPR/scratch/spill/helper 契约和最终编排、剩余条件融合/RISC-V/AOT，以及完整矩阵与可复现 FEX 对照。

## R3 最终必要证明、可选审计与冻结编排；旧 alloc 全部退役

相邻版本为 r5-residence-prep-v1。本批把旧 alloc 的最后四组八个源/头文件全部删除，并移除空目录；不保留转发头、旧 API 或兼容包装。新增文件及职责如下，方案目录树和迁移表已同步。

| 文件 | 实际职责 |
| --- | --- |
| `compile/certificates.h` | 三十二字节显式 block/index/target-write 输入，以及按值返回的必要证明结果；物理输入只读借用 AllocationView |
| `compile/certificates.cpp` | 全部目标编辑后的 fixed 检查、必要宽度证明编排、flags-only spill 生产与 zero-store 本地/入边资格；必要失败阻止冻结 |
| `compile/width_contracts.h` | WidthCertificates 和最终准备/局部 recolor 复核入口，调用方必须提供唯一共享索引 |
| `compile/width_contracts.cpp` | 保留 width/low32/host-read 重证明规则；删除自带 BlockAnalysisIndex、可空/default index 与 fallback 构图，只读物理结果 |
| `compile/effects.h` | host NZCV/helper/观察窗口、zero-store consumer 与 spill consumer 分类的纯规则声明和唯一 opcode 分类表 |
| `compile/effects.cpp` | 原 target effects 规则实现，不持有物理分配、动作或 emitter；手工编码路径直接调用同一分类函数 |
| `compile/audit.h` | 可选整单元类型/位置/preservation/支配/SSA/clobber 审计入口 |
| `compile/audit.cpp` | 保留原 owner、DOM、raw Lambda/Params/terminal 与 clobber 复核；不生产必要证书，不隐式准备分析图 |

compiler.cpp 的 TargetPreparation::Finalize 已统一完成关闭物理编辑、绑定整单元匹配的 fixed 语义描述、执行必要证书、按开关执行独立 audit、提交证明、准备边/原子/backedge 动作和最终冻结。FeatureSet 是准备对象的 const 快照。正常路径复用预制 fixed 描述；手工请求的策略不匹配时，仅为整个单元重建一次描述，各块使用实际共享索引，不增大每块输入记录。lower.cpp 保留早期独立 FPR retry、目标编辑与块内 lowering，最后调用 Finalize。

此前当前待办将“最终 target-write fixed 检查与公共最终化的基础 fixed 检查”视为重复，这个判断已修正：公共 LinearScan::Verify 在普通物理 attempt 中、scratch reserve 决策和目标编辑之前验证基础 fixed；最终检查在全部编辑之后额外包含实际 target writes。公共 AllocationController::Finish 只清理准备状态。两次检查覆盖不同阶段，本批全部保留，公共 allocate 源文件相对相邻版本逐字节不变。

必要 fixed/width/spill 生产现在只有一条无条件路径，先于 verify_plan 判断；audit 不再重复产生证书。zero-store 的扩展链、重复 raw consumer、本地/入边资格与循环拒绝规则原样接续。十二项逐函数规则核对通过；局部 recolor 仍用相同宽度复核，不允许用普通发射回退替代已经改变物理 mapping 的必要证明。

### 构建修正与实际测试范围

三次源码冻结各有独立清单和归档；构建期间没有编辑运行时源码。v1 首次构建失败：lower.cpp 缺少原间接导入的 HIR 定义，translator_operand.cpp 的 include 排序使 arg 寄存器宏提前污染 Boost 头路径。补齐直接 include 并将寄存器宏头置于独立末尾分组后，v2 九个 Android 目标构建成功。v1 没有拷贝或部署测试产物，也没有设备执行。

v2 在红魔 NX729J/bf1359ff、CPU4 执行一组 36 个 native 命令，其中 35 个通过，主套件退出 84：保留原 67 个失败断言，同时新增 17 个错误。原因是新 SealTargetEdits 用目标编辑开始时的 allocation attempt 检查最终结果，而 AllocationEdit::Commit 会在合法局部事务提交后递增 attempt。v3 仅修正 allocation_preparation.cpp：先验证结果身份及 location generation，关闭物理编辑，再记录最终 attempt。必要证明仍严格核对该最终版本；ResetAllocations 的旧 session 不会因此被放行。

v3 九个目标重新构建成功，最终 Ninja 退出 0/no work；只重跑受影响的主套件一次，退出恢复为 67。722 个用例的名称、顺序、失败类型/消息/正文与相邻版本逐项一致，无新增 error，无排除或改名；仍为 12 个失败用例、67 个失败断言。本次断言总数为 221494，相邻版本为 221764，随机种子相关的总数变化不归因于本次修改。

其余 35 个成功 native 验证保留真实 v2 产物身份，没有声称在 v3 上重新执行。快/慢 IR 各 40 cases/842 assertions，fixed-class 两个用例/39 assertions，十八项 lifetime 和十四项 entry/shutdown/call 的模式、检查数及注入结果均与相邻版本一致。v3 对已经在 v2 通过封口检查的路径，只写回相同的最终 attempt；变更的正是原来被错误拒绝的已提交事务。原 v1 的八个 RV 语法单元均新执行成功；v2/v3 仅修改 ARM64 cpp，全部相关头和 RV 源字节一致，因此复用原命令、时间和退出记录。本批未执行 RV native、独立 static-latch fault 和完整 shutdown 矩阵；构建目标数量不等于设备运行范围。没有新增测试文件、probe 或测试脚本。

现有“GPR verifier rejects stale views and invalid physical residences”用例已接续独立的必要证明与可选 audit，覆盖正常冻结结果通过、ResetAllocations 后旧结果拒绝，以及 preparation 阶段身份；原宽度用例改为显式提供共享索引。十七个实际 recolor/publication/fault 场景在 v3 主套件中恢复通过。

### 生成代码与固定布局

v3 只新执行十组静态场景，相邻版本的十组日志按不可变记录复用。八组完整有序 PC/IR 长度/host 长度记录及全部 RA shape 相等：默认 Function CoreMark、关闭 bound-decode 的 Function CoreMark、fixed-class Function CoreMark，以及五种配置的 bench。Block 使用 PC/host 长度记录，以下两组保留原始差异：

| 场景 | 相邻/本批记录数 | 相邻/本批 RA host bytes | 相邻/本批逐 PC host bytes |
| --- | --- | --- | --- |
| Block CoreMark | 2413 / 2418 | 205992 / 206176 | 205992 / 206176 |
| fixed-class Block CoreMark | 2419 / 2415 | 207988 / 207688 | 207988 / 207688 |

两组新增/缺失 PC 全在精确符号区间 __d2b_D2A（0x253b00，长度 0x342）或 __dtoa（0x25f2d0，长度 0x1017）。全部共有 PC 的各次生成长度记录相等，区间外完整有序记录相等；fixed-class Block 的共有 PC 全局顺序在该区间内有变化，未宣称两组完整顺序或 shape 相等，也没有重跑以凑相同 roster。十组的 pool_bytes、ir_arena_bytes、CRC 与五项 benchmark checksum 均一致。关闭 bound decode 仍是 Function；Block 由 SVM_FUNC_BASE=0、func_units=0、无 function-unit 记录及实际 standalone host 记录共同确认。

最终 v3 Android DWARF：WidthElisionVerifier 4168 字节，WidthContractCheck 3936 字节；减少的是旧 owned 分析图的固定布局。证明输入 32 字节、WidthCertificates 72 字节、EmissionCertificates 96 字节均保持原大小。TargetPreparation 2232、PendingGPRBlock 312、AllocationPreparation 928、UnitActions 824、prepared Storage 1680、OptimizedUnit Storage 728、ARM64/RV JitContext 1384/936、ARM64/RV JitTranslator 3320/1440 均与相邻版本相等。这里只是固定布局证据，没有证明整体峰值内存下降。

### PMU 诊断与证据

v3 和相邻版本各在实际 Function/Block 模式执行一组 2000/12000 bracket，共八次，顺序均为 reference 2000、candidate 2000、candidate 12000、reference 12000。simpleperf 采 cpu-cycles/instructions，CPU4/taskset 10，空 SVM_JIT_CACHE；每点前后都记录 governor/current/max 和三个温度传感器。八次退出 0，2000 CRC 为 4983、12000 CRC 为 d340。

| 模式 | 指令数 slope / intercept 变化 | cycles slope / intercept 变化 | wall slope / intercept 变化 |
| --- | --- | --- | --- |
| Function | −0.005119% / +0.321044% | −0.864978% / +1.765873% | −1.692691% / +1.390925% |
| Block | +0.036222% / −0.166369% | +0.969180% / −2.196318% | +1.532364% / −3.883344% |

全部采样读数为 walt，current/max 均为 1171200 kHz；cpuss 29.7–32.4°C、cpu1-0 29.3–32.4°C、cpu1-6 28.5–30.5°C。采样一致不能证明全程锁频，单组 bracket 也不能证明因果收益或性能无回退；intercept 不是独立编译耗时，不用短时 CoreMark 输出作为正式分数。最终 fixed 的语义输入绑定计入 plan_verify，总体作用域仍包含这项工作，verify_fixed 子桶现在只计最终物理检查，不能把子桶变化直接当作减少了相同口径的工作。正式可复现 FEX 三方验收仍未完成。

最终 v3 源清单 digest：f5302d0542e0770a9cb14d80508457dbcf7ec7e59270543f8e2660c67415bf10；归档 SHA-256：efe75a41e1fc240f349f505f1095cccc2797ef64205599bec7c93a34b98140c1。416 个 source 路径、107 个已删除路径，相对相邻版本 32 个源码路径变化、8 个文件退役。最终 launcher svm_final_certificates_v3 SHA-256 为 4a05e92b506b5d2f2c1cc9fdc0d516ab78456230d83d165ecf3606d0be55a036，相邻 svm_residence_prep_v1 为 2ed382c12e24579c21080d6299e17c0bc8b2dbe2760d11717413ecbf4a723430。设备前后十二份 hash 均一致并匹配不可变候选、相邻 launcher、工具及 guest。

原始清单/归档、三轮 build、syntax、format、规则核对、v2 rejection、v2/v3 native 记录及 JUnit、十组静态日志和逐 PC 差异、八次 PMU 条件与日志、DWARF、integrity 和 quality-gate 均保存在 /private/tmp/swiftvm-middle-results，以 r3-final-certificates-v1/v2/v3 为前缀，未在仓库保留观察脚本。quality-gate 的 53 项检查通过，batch_complete=true，full_goal_complete=false，formal_three_way_performance_accepted=false。R1/R2 摘要、候选/workspace 与契约、其余 R3/R5/R6/R7 和可复现 Android FEX 仍按完整范围继续。

## R1 块内选择索引共享工作区与 memory 交接收敛

相邻版本为 r3-final-certificates-v3。本批删除 consumer 的 std::map 动作行、terminal-condition 哈希表，以及 MemorySelections 的每块哈希节点；通过显式借用的工作区组织选择，结果只交接连续记录。没有恢复旧 alloc 目录，没有保留旧可变 MemorySelections API、转发包装、临时 probe 或测试脚本。

新增 compile/memory_selections.{h,cpp}，下表路径相对于 source/runtime；方案目录树、逐文件职责和当前待办已同步。

| 文件 | 实际职责 |
| --- | --- |
| `compile/unit.h`、`compile/unit.cpp` | CompileWorkspace 增加可复用的 u32 行列。InstructionRowScope 拒绝重叠借用，第一次插入才按实际块内指令数初始化，结束借用后清空逻辑状态；外层作用域按既有 64K entries 上限回收超大容量，嵌套准备使用独立工作区 |
| `compile/analysis/block_index.h` | InstructionPosition 复用原 Contains 的 ID/真实指针校验与稀疏有序查询，返回实际块内位置；Contains 直接消费该结果。已有 dense-use 存储的 InstructionSlot 语义不变，不按 Function 中较大的 ID 偏移扩大行表 |
| `backend/arm64/compile/memory_selections.h` | 区分可变 MemorySelectionBuilder 和阶段内只读 MemorySelections；后者不持有 workspace、哈希节点或 publication 链接 |
| `backend/arm64/compile/memory_selections.cpp` | builder 用共享行列索引八字节候选 slot，保留首次角色、payload 和 publication 选择。Finish 才排序 transfer/address、保留必要的 u16 load-publication 名单并释放行列借用与临时链接；只读结果在连续记录上二分查询 |
| `backend/arm64/compile/lower_memory.{h,cpp}` | 只负责 LowerMemory 的窄加载、offset、post-index、覆盖与 write witness；选择容器实现已移出，lowering 规则主体逐 token 一致 |
| `backend/arm64/compile/lower_consumers.{h,cpp}` | 显式接收 CompileWorkspace，连续动作与行号分别拥有；terminal 的 seen/NZCV 合取状态与行号共用 u32 暂存，不额外分配哈希节点。Finish 按 ID 排序后交接动作，不把工作区带进 ConsumerActions |
| `backend/arm64/compile/compiler.cpp`、`internal.h`、`lower.cpp` | CompleteARM64Allocation 持有目标工作区作用域并显式传入 TargetPreparation。声明每块 memory 选择，Finish 后才能交给 PendingGPRBlock；后续 consumer 顺序借用同一行列。独立 FPR retry 的每次目标准备复用该作用域 |

memory 的 physical write/publication 准备顺序没有前移，仍在全部目标编辑完成后按原顺序提交约束，再整理只读选择。首次插入优先、LoadUpdate 对 related publication 角色的碰撞策略、重复 publication 链接保留第一个的规则均保留。结果查询现在直接核对唯一语义索引中的真实节点，不以同 ID 或 GetBlock 字段代替归属。

consumer 仍按原顺序生成 pair、compound-clear、local condition、PSTATE 与反向 boolean/FCmp/string 决定；最终排序与旧 map 的 ID 顺序相同。一个条件在多个 terminal 出现时，所有 occurrence 都必须保持先前 NZCV，嵌套 canonical-PSTATE reload 的任一失败仍使合取失效。foreign terminal 定义不会进入本块的局部决策。动作引用不跨越下一次 Touch 的 vector 增长。八个规则主体核对通过，包括完整 BuildPairResults、BuildFCmpAction 和 LowerMemory。

### 集中构建与设备验证

源码一次冻结，九个 Android 目标首次构建成功，最终相同目标 Ninja 退出 0/no work；八个 RV 语法单元全部新执行并通过。构建期间没有修改源码，没有失败构建或补丁轮次。红魔 NX729J/bf1359ff、CPU4/taskset 10 只执行一组 36 个 native 验证命令，全部按预期退出，没有重跑。

主套件仍为 722 cases、12 个失败用例、67 个失败断言、0 error，退出 67；所有用例的名称、顺序、失败类型/消息/正文与相邻版本逐项一致。当前总断言为 221795，相邻主套件为 221494；除现有 memory 用例增加的六个断言外，不将随机种子相关的总数变化归因于本次修改。快/慢 IR 各 40 cases/842 assertions，fixed-class 两个用例/39 assertions；十八项 lifetime 和十四项 entry/shutdown/call 的模式、检查数与注入结果均相等。参考证据沿用相邻版本记录的真实产物身份：suite 为 v3，其余为该批已经说明的 v2 成功记录；本批 36 项则全部新执行。

现有 memory 选择/类型视图用例改为显式 builder→Finish 交接，并补上工作区重用到另一个块后，旧只读结果仍保留首选 offset、正确识别 load publication、拒绝外块同 ID 节点的断言。原 consumer 重复构建、foreign/stale 输入、canonical-PSTATE reload 与终端条件用例全部接续生产工作区入口。没有新增测试源文件或专用观察脚本。本批仍未执行 RV native、独立 static-latch fault 及完整 shutdown 矩阵，不把九目标构建当作这些矩阵已经运行。

### 生成记录与布局取舍

十组静态场景只新执行候选一次，相邻十份日志核对 SHA 并复用。七组完整有序 PC/IR 长度/host 长度记录及全部 RA shape 一致：默认 Function CoreMark、关闭 bound-decode 的 Function CoreMark，以及五种配置的 bench。Block 记录使用 PC/host 长度。另三组的原始差异如下：

| 场景 | 相邻/本批记录数 | 相邻/本批 RA host bytes | 相邻/本批逐 PC host bytes |
| --- | --- | --- | --- |
| Block CoreMark | 2418 / 2413 | 206176 / 205992 | 206176 / 205992 |
| fixed-class Function CoreMark | 425 / 424 | 286352 / 286260 | 283600 / 283508 |
| fixed-class Block CoreMark | 2415 / 2415 | 207688 / 207692 | 207688 / 207692 |

全部新增/缺失 PC 均在 __d2b_D2A（0x253b00，长度 0x342）或 __dtoa（0x25f2d0，长度 0x1017）精确范围内，共有 PC 的每次生成长度完全一致，区间外完整顺序一致。fixed-class Block 在范围内的共有 PC 全局顺序不同；未据此宣称三组完整记录或 shape 相等，没有重跑以凑相同名单。十组 memory 字段、CRC 和五项 benchmark checksum 全部正确且相等；Function/Block 继续按实际编译模式核验，关闭 bound decode 不等于 Block。

最终 Android DWARF 给出以下固定布局，既记录缩小也记录增长：

| 类型 | 相邻/本批字节 |
| --- | --- |
| MemorySelections | 96 / 80 |
| ConsumerBuilder | 128 / 120 |
| PendingGPRBlock | 312 / 296 |
| CompileWorkspace | 56 / 80 |
| TargetPreparation | 2232 / 2288 |
| 新 MemorySelectionBuilder / InstructionRowScope | 144 / 16 |

UnitActions 824、prepared Storage 1680、OptimizedUnit Storage 728、AllocationPreparation 928、ARM64/RV JitContext 1384/936、ARM64/RV JitTranslator 3320/1440 均保持。每块保留结果缩小，不代表整体峰值内存下降；共享列还需要首次使用时初始化四字节×块内指令数。完整容量增长、分配次数与初始化总量计量仍属 R1 后续目标。

### PMU 与证据范围

候选与相邻版本在实际 Function/Block 各执行一个 2000/12000 bracket，共八次。每种模式的顺序为 reference 2000、candidate 2000、candidate 12000、reference 12000；simpleperf 采 cpu-cycles/instructions，CPU4/taskset 10，空 SVM_JIT_CACHE，每点前后记录 governor/current/max 与三个命名温度。所有退出为 0，2000 CRC 为 4983、12000 为 d340。

| 模式 | 指令数 slope / intercept 变化 | cycles slope / intercept 变化 | wall slope / intercept 变化 |
| --- | --- | --- | --- |
| Function | −0.124902% / +0.496534% | −2.823584% / +1.744334% | −2.172680% / −0.663860% |
| Block | +0.139305% / −1.029044% | +0.317795% / +3.586356% | −0.301574% / +6.656428% |

全部快照为 walt，current/max 均为 1171200 kHz；cpuss 29.3–32.4°C、cpu1-0 30.1–32.8°C、cpu1-6 28.5–30.5°C。读数一致不能证明全程锁频，单 bracket 不能证明因果收益、性能无回退或正式三方验收；intercept 不是独立编译成本，短时 CoreMark 不作为正式分数。

源码 digest 为 6a0bdeaa181cb7031a7af1dc6e6f1478d275f7ed281235ee0ccd015affbd2b0e；归档 SHA-256 为 9b506b3b12372c3a331ed428bf191855ecc01a7344de2a4c821c9053c9339fd2。418 个 source 路径、107 个已删除路径，相对相邻版本 20 个源码路径变化，仅新增 memory_selections 两个运行时文件。launcher svm_selection_workspace_v1 SHA-256 为 b451cfde008ec6ce6c2003501c7941e6bac9ea26a3357895902acb4e39801aec；相邻 svm_final_certificates_v3 为 4a05e92b506b5d2f2c1cc9fdc0d516ab78456230d83d165ecf3606d0be55a036。设备前后十二份 hash 稳定并匹配不可变产物、工具及 guest。

清单/归档、build/syntax/format、规则核对、36 项 native 及 JUnit、十份静态日志与完整差异、八次 PMU 条件和日志、DWARF、integrity、quality-gate 保存在 /private/tmp/swiftvm-middle-results，以 r1-selection-workspace-v1 为前缀。48 项质量检查通过，batch_complete=true，full_goal_complete=false，formal_three_way_performance_accepted=false。

本批只统一 memory/consumer 的块内行暂存。公共 AllocationState 仍持有独立 workspace，rewrite/spill 与其余候选的跨入口共享、必需 use/presence/语义宽度摘要、候选播种和窗口扫描继续按 R1/R2/R3/R5/R6 推进；完整 R7 矩阵与可复现 Android FEX 仍未完成。

## R1 优化、分配与 target 共用调用方工作区

相邻版本为 r1-selection-workspace-v1。本批移除公共 AllocationState 内嵌的 CompileWorkspace、RewriteContext 内部的完整 CompileWorkspaceScope，以及 CompleteARM64Allocation 单独申请的 target scope；外层准备入口成为实际所有者。全部构造调用方、独立 pass 入口和 RV 现有准备路径均接续显式借用，没有新增无 workspace 的兼容重载、可空 fallback 或隐藏重建。

| 文件 | 实际职责与交接 |
| --- | --- |
| `compile/unit.{h,cpp}` | CompileWorkspace 禁止复制；新增 RewriteQueueScope，互斥借用 Function 删除队列，正常/异常退出均清空逻辑引用并释放占用。外层线程/嵌套 scope 和既有行列借用、容量回收规则继续保留 |
| `compile/pipeline.{h,cpp}` | OptimizeBlock/OptimizeFunction/OptimizeFunctions 显式接收 CompileWorkspace&，向残余 pass 的同一 RewriteContext 传递；结果不借用工作区。Block/Function 调度、提升、编号、合并与封口顺序不变 |
| `compile/passes/passes.h`、`dce.cpp` | RewriteContext 只借用调用方工作区，Function 首次 DCE 播种时才取得队列 scope。Block 继续反向删除，不申请队列；保留 owners 检查、重复项处理、raw uses、隐式根及逐步回收顺序 |
| `compile/passes/{flags,values,widths,state,state_map,state_sink}.cpp` | 共享 RewriteContext 的生产路径直接使用外层工作区；独立运行一个 pass 的入口持有自己的完整 scope。规则主体不变，没有为共享流水线重复申请作用域 |
| `compile/allocate/writer.{h,cpp}` | 构造必须显式接收 CompileWorkspace&，只在准备期借用；Workspace 查询不出现在 AllocationView 或物理结果中。调用方必须覆盖全部分配及目标重试的寿命 |
| `compile/allocate/controller.h`、`allocation.cpp` | AllocationState 只持有请求输入、策略和 facts 借用，不再拥有工作区。RunAttempts 从同一 writer 取工作区，scratch/FPR 重试继续复用；Finish 释放请求状态，外层继续持有 facts/workspace |
| `backend/arm64/compile/{compiler,allocation_preparation}.{h,cpp}` | PrepareBlock、PrepareFinalBlock、PrepareFunction、PrepareFunctions 各拥有一个外层 scope，覆盖该入口实际执行的优化、分配、重试和 target 准备。已编号 HIRBlock 仍不重跑普通 Block 优化；多 region 顺序共享暂存，owning 结果独立持有 IR/分配/actions |
| `backend/riscv64/jit/compile.cpp` | 原有 Block/单 Function 编译入口各持有 workspace，逐块 Prepare 显式借用；保留 RV 原有 flags/架构 admission 与本地分配策略，不强行接入 ARM64 分配路径 |
| `tests/support/{allocation_fixture.h,register_alloc_test_support.cpp}` | fixture 独立持有工作区，生命周期覆盖 writer；直接 LinearScan 测试也复用该 writer 的工作区，不再每次另造一份。其余测试构造显式提供实际作用域内的所有者 |

DCE、六类残余 pass、普通分配 attempt 和最终 Finalize 的十九项定义主体核对通过。提取锚定在返回类型及函数定义，记录定义签名，不把调用点后的代码块当作函数；队列访问和 workspace 来源只作明确的局部归一化。没有合并 Block/Function 分配，没有改变 scratch reserve/evict 顺序或独立 FPR retry，也没有改变必要 fixed/width/spill 证明的执行顺序。

### 构建与集中验证

v1 首次九目标构建失败：compiler.h 的新参数缺少 CompileWorkspace 前置声明。十个 RV 语法单元在该轮全部通过，其中除既有八个外，补查本批修改、未由这组 Android 目标覆盖的 native_structure/native_wide 两个源。v1 未拷贝、部署或执行设备产物。

等待构建真正退出后，v2 仅补一条 ARM64 compiler.h 前置声明，重新冻结并成功构建九个目标；最终 Ninja 退出 0/no work。编译期间没有修改源码。十个 RV 语法记录保留真实 v1 命令、时间和退出结果：源码、其余头、类型定义和选项均不变，新增声明不改变原成功单元已能解析的定义或调用契约；未声称这十项在 v2 新执行。

红魔 NX729J/bf1359ff、CPU4/taskset 10 只执行一组 36 个 v2 native 命令，全部按预期退出，没有设备重跑。主套件仍为 722 cases、12 个失败用例、67 个失败断言、0 error，退出 67；所有用例名称、顺序和失败类型/消息/正文与相邻版本逐项一致，无排除或改名。总断言 221697，相邻为 221795，随机种子相关的总数变化不视为此次修改的行为结论。

快/慢 IR 各 40 cases/847 assertions，比相邻各增加五个断言；fixed-class 两个用例/39 assertions。新增断言位于既有所有权/工作区用例，检查 writer 确实借用调用方对象、队列拒绝重叠借用、同一工作区在两个队列作用域间清空逻辑状态、异常退出后可重新借用。冻结物理结果与 actions 的用例还让正常准备使用的工作区先于后续查询/发射销毁，保证结果不依赖它。十八项 lifetime 和十四项 entry/shutdown/call 的模式、检查数和注入结果均与相邻版本相等。

没有新增测试源文件、临时 probe 或测试脚本。本批仍未运行 RV native、独立 static-latch fault、完整 shutdown 或异步跨工作线程交接矩阵；语法检查和 Android 构建不替代这些验收。

### 固定布局与生成记录

最终 Android DWARF 同时记录减少和新增借用成本：

| 类型 | 相邻/本批字节 |
| --- | --- |
| AllocationState | 504 / 424 |
| RewriteContext | 336 / 248 |
| AllocationWriter | 920 / 928 |
| AllocationPreparation | 928 / 936 |
| CompileWorkspace | 80 / 80 |
| CompileWorkspaceScope | 96 / 96 |
| 新 RewriteQueueScope | 8 |

TargetPreparation 2288、PendingGPRBlock 296、UnitActions 824、prepared Storage 1680、OptimizedUnit Storage 728、ARM64/RV JitContext 1384/936、ARM64/RV JitTranslator 3320/1440 均保持。这里是固定布局证据，不能直接相加为整体峰值内存收益；动态容量增长、分配次数与初始化总量尚需按 R1 计量。

静态十组只新执行 v2 候选，相邻日志核对 SHA 后复用。七组完整有序 PC/IR 长度/host 长度记录与全部 RA shape 一致：默认 Function CoreMark、关闭 bound-decode 的 Function CoreMark，以及五种配置的 bench。Block 使用 PC/host 长度记录。其余三组完整差异保留如下：

| 场景 | 相邻/本批记录数 | 相邻/本批 RA host bytes | 相邻/本批逐 PC host bytes |
| --- | --- | --- | --- |
| Block CoreMark | 2413 / 2413 | 205992 / 205584 | 205992 / 205584 |
| fixed-class Function CoreMark | 424 / 425 | 286260 / 286352 | 283508 / 283600 |
| fixed-class Block CoreMark | 2415 / 2419 | 207692 / 208008 | 207692 / 208008 |

全部新增/缺失 PC 均在精确符号 __d2b_D2A（0x253b00，长度 0x342）或 __dtoa（0x25f2d0，长度 0x1017）内。共有 PC 的各次生成长度与共有顺序相等，区间外完整顺序相等；没有宣称这三组完整名单或 shape 相等，也没有重跑以凑相同名单。十组 memory 字段、CRC 和五项 bench checksum 全部相等且正确。关闭 bound decode 仍按实际 Function 模式记录，Block 继续用显式环境、计数及 standalone host 记录共同核验。

### PMU 诊断与证据

相邻/候选在实际 Function、Block 各执行一个 2000/12000 bracket，共八次；每种模式顺序为 reference 2000、candidate 2000、candidate 12000、reference 12000。simpleperf 采 cpu-cycles/instructions，CPU4/taskset 10，空 SVM_JIT_CACHE，每点前后记录 governor/current/max 与三个命名温度。八次退出 0，2000 CRC 为 4983，12000 为 d340。

| 模式 | 指令数 slope / intercept 变化 | cycles slope / intercept 变化 | wall slope / intercept 变化 |
| --- | --- | --- | --- |
| Function | +0.005185% / −0.023675% | +0.760355% / −1.453289% | +1.268522% / −1.749829% |
| Block | −0.031056% / −0.002117% | +0.816516% / −1.565949% | +1.020871% / −2.199070% |

全部快照为 walt，current/max 均为 1171200 kHz；cpuss 30.1–32.8°C、cpu1-0 30.5–32.8°C、cpu1-6 29.3–30.5°C。单组 bracket 与频率快照不足以证明因果收益、性能无回退或全程锁频；intercept 不是独立编译耗时，短时 CoreMark 不作为正式分数，也未完成可复现 FEX 三方验收。

v2 源码 digest 为 dc0e9dcf551ef91638576b582c96acaa8ff0b93b2c0e154463f79bb035fb9211；归档 SHA-256 为 4ba86212a439b02f7c4f2c2c190958ebd5c723e9acdb2257f5eb0dba5519564b。418 个 source 路径、107 个已删除路径，相对相邻版本 41 个源码路径变化，无新增 source/test 文件。launcher svm_shared_workspace_v2 SHA-256 为 208279407eb6f59765774bb026ae5b7766c44dfc46df73171ebb490cf79ddc03；相邻 svm_selection_workspace_v1 为 b451cfde008ec6ce6c2003501c7941e6bac9ea26a3357895902acb4e39801aec。设备前后十二份 hash 稳定并匹配不可变产物、工具和 guest。

两轮清单/归档、build/syntax/format、定义主体核对、36 项 native 及 JUnit、十组静态日志和完整差异、八次 PMU 条件与日志、DWARF、integrity、quality-gate 保存在 /private/tmp/swiftvm-middle-results，以 r1-shared-workspace-v1/v2 为前缀。64 项质量检查通过，batch_complete=true，full_goal_complete=false，formal_three_way_performance_accepted=false。

方案中 controller “拥有 workspace、支持跨线程交接”的旧描述已改为实际借用关系；异步交接仍保留为需要接续独占所有者并验证的要求。公共优化/分配/target 的现有同步路径已共享工作区，但其余候选容器、容量/初始化计量、必需 use/presence/语义宽度摘要与 affected-value worklist 继续推进；R3/R5/R6 其余契约/扫描、完整 R7 与可复现 Android FEX 均未完成。

## R1 封口摘要、consumer 候选与 GPR 使用查询

本批相邻版本为 r1-shared-workspace-v2，当前通过验证的版本为 r1-summary-candidates-v2。基础摘要从 consumer 图中独立出来；consumer 的多个规则遍历复用一次候选播种；固定 GPR 发表复用已有直接边查询。这是 R1 的一批交付，完整 R0–R7 仍未完成。

### 文件组织与实际行为

| 文件 | 本批职责 |
| --- | --- |
| `compile/analysis/block_summary.h`（新增） | 八字节 BlockSummary 保存指令数与 memory/FPR/vector-constant/consumer/select/single-use/GPR 输入候选 presence。精确 use-count 使用 Inst 现有计数，不再复制一份；没有添加尚无消费者的语义宽度字段 |
| `compile/analysis/consumer_roles.h`（新增） | 共享纯 IR opcode 的 consumer 角色分类。生产入口 presence 与局部规则候选分开：And/Or 单独出现仍不改变旧生产入口准入；控制、PSTATE 事件不能因没有动作行而丢失 |
| `compile/analysis/{semantic_facts.h,facts.cpp}` | 摘要和既有 flags/活跃性基础遍历合并。FindSummary 为只读查询；直接准备 SemanticFacts 可先取 flags/摘要、后补活跃性。Function 摘要覆盖全部存在的实际成员（包括空 IR 块），原 flags 与物理区间的可达性规则不变；存储预留完整成员数，借出的 inline 摘要地址稳定 |
| `compile/optimized_unit.cpp` | 最终编号、封口后的 owning 结果构造时显式准备基础 facts/摘要；返回时已存在 ValueLiveness，但不因此建立 consumer 或 spill 图。所有权及 Block/Function 分流不变 |
| `compile/analysis/block_index.h` | SemanticFacts 通过私有入口传入本块已准备摘要，建图时不重复分类；独立 Build 仍从 IR 得出摘要。校验摘要指令数和实际 roster 一致；直接边、重复边、BitCast 派生与指针身份规则不变 |
| `backend/arm64/compile/lower_consumers.cpp` | 一次 forward 播种四字节位置/角色候选，pair producer/secondary、compound-clear、local condition、可选 ALU audit 与 reverse event 共用。禁用审计时不播种其专属角色；无单使用值时排除 local-condition 角色。FCmp 窗口、PSTATE 必要前缀和真实指令邻接仍检查完整 IR |
| `backend/arm64/compile/inputs.cpp` | 基础 facts 在 fixed/目标输入之前准备；无保守 GPR opcode 候选则不创建其 builder。逐指令 scratch/helper/clobber/fixed 描述保留；删除无条件 resolved-use 准备 |
| `backend/arm64/compile/gpr_inputs.cpp` | 固定发表的首个 zext 使用、source/zext raw 边数和覆盖后使用查询借用直接索引，替代重复全块 operand 遍历。observer 与后续写入窗口保留，source-home/target-home 资格不变 |
| `compile/allocate/width.cpp` | bridge 与 pinned W-view 通过已有物理/局部性初筛后，在真正 resolved-use 证明处显式准备共享图。宽度消费者此前借用 GPR 描述顺带构图的副作用，迁移时一并消除；未放松最终 width 证明 |
| `tests/{ir_contract_test,block_analysis_index_test,cycle_exit_plan_test}.cpp` | 扩充既有用例中的摘要独立性、稀疏编号、真实 IR 尾/后续控制、覆盖指令参数读取边界和结果快照检查；没有新增测试文件或脚本 |

两个新头文件接入 compile/CMakeLists.txt。本批相对相邻版共十四个 source 路径变化，新增两份生产头文件，无本批新增删除路径；累计快照为 420 个 source 路径、107 个已删除路径。未保留临时 probe 或测试脚本。

### 语义边界与剩余工作

- `Last` 按候选在完整 IR 中的位置判断。尾部 AdvancePC 等非 consumer 指令虽不进入候选表，仍阻止前面的 Boolean 被误判为最后一条；Goto/BindLabel 与所有原 PSTATE 事件仍参与反向状态传播。
- 固定发表继续比较全局 GetUses 和本块 raw direct-use 边数；重复参数、pseudo uses、Lambda/Params 与跨块使用拒绝保持原语义，不改成 terminal/anchor 活跃性计数。
- zext 的覆盖后使用判断为严格大于覆盖点，因为该写入先读参数再替换 home；source 的捕获判断包含覆盖指令自身的参数读取。现有输入契约用例增加重复 zext 写与 source 作为覆盖参数两种边界，分别核验候选/源复用结果。
- 八项定义主体核对通过：两项宽度选择只归一化显式 PrepareResolvedUses 和局部可变借用；W-write/root 谓词、块逻辑 use-end、基础准备入口等规则不变。候选循环、摘要构造和 GPR 索引算法是有意修改，不能把这些算法说成仅文件搬迁或完整主体等价。
- 摘要不建图，尚不等于所有直接图都按需。必要 fixed/residence 和其他调用方仍会请求图；完整语义宽度、共享 CFG/其余候选、受影响 value worklist、工作区增长/初始化计量与异步跨线程所有权交接仍待完成。consumer 候选数组本批仍为 builder 私有暂存，没有宣称已迁入 workspace。
- 基础 facts 从后续分配移到封口结果构造，改变了阶段计时边界。必须检查总编译成本，不能把 RA 子桶变小算作实际节省。

### 构建与集中正确性验证

v1 九目标构建在新 next-write 查询的泛型 lambda 中报 GetArg 依赖模板错误；该轮未拷贝、部署或执行设备产物。等待进程退出后，v2 仅把内部谓词参数明确为 const Inst*，重新冻结源码。九目标构建与最终 Ninja 无待构建检查均退出 0。v1 实际执行的十个 RV 语法单元全部退出 0；v2 仅修改 ARM64 gpr_inputs.cpp 的该参数，因此复用原执行记录并保留来源，不伪记为十次新执行。

全部设备执行在红魔 NX729J/bf1359ff 上，CPU4/taskset 10，空 SVM_JIT_CACHE。合并修改后集中执行一次：

| 验证 | 结果 |
| --- | --- |
| 36 次 native | 退出状态全部符合预期：主套件 67，其余 35 次为 0 |
| 主套件 | 722 个用例、221956 条断言；原有 12 个失败用例/67 条失败断言、0 error；全部有序用例及失败内容与相邻版本一致，没有排除或改名 |
| IR fast/slow | 各 40 个用例、874 条断言，均通过；相邻为各 847 条，本批增加 27 条有意义的契约断言 |
| fixed-class | 2 个用例、39 条断言，均通过 |
| 寿命与持久化 | 18 组寿命、14 组入口/关闭/调用 marker 的模式与 checks 值逐项一致 |

主套件成功断言总数含随机输入，不能把总数变化等同于新覆盖；用例签名比较只归一化源码行号及二进制 classname 前缀，保留全部 failure/error 类型、消息和内容。本轮仍未补齐完整 RV native、独立 static-latch fault、完整 shutdown 和异步交接矩阵。

### 十组静态输出

五组 bench 的完整有序记录、全部 shape 及内存字段相同，五个 checksum 均正确。CoreMark 五组 CRC 均为 4983；所有共同 PC 的逐次记录相同，内存字段相同。差异只在 __d2b_D2A（0x253b00，大小 0x342）与 __dtoa（0x25f2d0，大小 0x1017）的精确区间内，区间外完整序列一致。

| CoreMark 组 | 相邻/候选记录数 | RA host bytes | per-PC host bytes | 共同记录的全局次序 |
| --- | --- | --- | --- | --- |
| 默认 Function | 424 / 425 | 286696 / 286788 | 284076 / 284168 | 相同 |
| bound-decode off Function | 424 / 425 | 286700 / 286792 | 284080 / 284172 | 相同 |
| 实际 Block | 2413 / 2417 | 205584 / 206104 | 205584 / 206104 | formatter 内有变化 |
| fixed-class Function | 425 / 425 | 286352 / 286352 | 283600 / 283600 | formatter 内有变化 |
| fixed-class Block | 2419 / 2416 | 208008 / 207608 | 208008 / 207608 | formatter 内有变化 |

固定类 Function 的完整 shape 相同，但有序记录不同；不能仅因字节和 shape 相同就宣称全部记录相同。三组的共同记录全局次序变化均保留在原始差异中，未通过重跑凑 roster。Function 默认、关闭 bounded decode 仍分别验证为 Function；Block 由 SVM_FUNC_BASE=0、func_units=0 和实际 host 记录证明，未依赖旧 block_units 计数。

### 暂存布局与成本

DWARF 对冻结的相邻/候选产物测得以下对象大小，单位字节：

| 对象 | 相邻 | 候选 |
| --- | ---: | ---: |
| BlockSummary | 无独立对象 | 8 |
| SemanticFacts::BlockRecord | 112 | 120 |
| SemanticFacts | 592 | 624 |
| BlockAnalysisIndex | 232 | 240 |
| ConsumerBuilder | 120 | 208 |
| OptimizedUnit::Storage | 728 | 760 |

ConsumerBuilder 新候选元素为 4 字节，inline 容量 16；其容器增加的暂存已计入上表。TargetPreparation 2288、PendingGPRBlock 296、AllocationPreparation 936、AllocationWriter 928、AllocationState 424、UnitActions 824 和 Prepared Storage 1680 均保持原大小。静态 pool_bytes/ir_arena_bytes 相同不代表峰值暂存相同；本批没有用扫描减少掩盖对象增长，也不宣称峰值内存下降。候选暂存继续列入工作区收敛和计量要求。

### PMU 诊断与证据

实际 Function/Block 各采一个 2000/12000 bracket，共八次。每组顺序为 reference 2000、candidate 2000、candidate 12000、reference 12000；simpleperf 读取 cpu-cycles/instructions，每点前后记录 governor/current/max 与三个命名温度。八次退出 0，2000 CRC 4983、12000 CRC d340。

| 模式 | 指令数 slope / intercept 变化 | cycles slope / intercept 变化 | wall slope / intercept 变化 |
| --- | --- | --- | --- |
| Function | −0.017577% / −0.344545% | −1.857675% / +3.444347% | −3.590817% / +5.488728% |
| Block | −0.063596% / −0.533000% | +3.197861% / −0.730385% | +5.062893% / −4.100830% |

所有频率快照均为 walt、current/max 1171200 kHz；cpuss 29.7–33.6°C、cpu1-0 30.5–33.2°C、cpu1-6 28.9–30.9°C。Block 的正向 cycles/wall 波动如实保留，单组 bracket 不足以作收益或性能无回退结论。intercept 含启动、guest 运行、打印等成本，不是独立编译耗时；频率快照不证明全程锁频，不作官方 CoreMark 分数。可复现 Android FEX 和正式三方性能验收仍未通过。

v2 源码 digest：55bc5cb212b5e087c82fc7b176f0cf91fcb60710a5b3ff5c34668c672cbd2e64；归档 SHA-256：e0d571c405af5981e087fd3b4eedc1e985f2165a991770297787d07197aaab5e。launcher svm_summary_candidates_v2 SHA-256：6eb8f68bc0e1ea16ca7a88390a8cb1d3abb16cf4e8f6bd804d0c04a0b767d780。设备前后十二份 hash 稳定并匹配冻结产物、相邻 launcher、simpleperf 和两个 guest。

源码清单/归档、build/syntax/format、八项定义核对、native/JUnit、十组静态原始记录与次序差异、八次 PMU 条件、DWARF 和质量门集中保存在 /private/tmp/swiftvm-middle-results，以 r1-summary-candidates-v1/v2 为前缀；证据只留在外部目录，不向仓库添加诊断脚本。方案已同步新增文件职责、实际事实准备时机、直接图/语义宽度的未完成范围与总成本计时要求。本批 61 项质量检查通过，batch_complete=true，full_goal_complete=false，formal_three_way_performance_accepted=false。

## R1 指令索引与直接使用图分离

本批相邻版本为 r1-summary-candidates-v2，最终验证版本为 r1-demand-uses-v4。编号 roster 不再顺带构建全部直接 consumer 边；准备期间显式升级同一个对象，所有 const 查询继续只读。纯普通块已在现有用例中走过输入描述、普通分配、完整 target 准备与冻结，确认无分析需求时直接图仍未建立。完整 R1 以及 R0–R7 均未据此认定完成。

### 数据与文件职责

| 文件 | 实际改动 |
| --- | --- |
| `compile/analysis/block_index.h` | 将 roster/presence/成员身份建立与直接边建立分开。PrepareUses 显式填充边表且幂等；Uses/ForEachUsedDefinition/ResolvedUses 在直接边未就位时拒绝。PrepareResolvedUses 先接续准备直接边，再解析实际本地及传入 BitCast。独立 Build 仍是明确请求完整索引的构造入口；生产 SemanticFacts 使用轻 roster 入口 |
| `compile/analysis/{semantic_facts.h,facts.cpp}` | 新增 PrepareIndex/FindIndex；PrepareUses 在同一稳定对象上补齐直接边，FindUses 只返回已就位的使用图。BlockRecord 的成员名由 consumers 改为 index。逻辑块 use-end 只在边已就位时借用，否则保持原有 IR 使用遍历；不为 use-end 强制构图 |
| `backend/arm64/compile/{inputs,gpr_inputs}.cpp` 及 `gpr_inputs.h` | 输入描述先取 roster，memory feeder 查询在 memory presence 下申请边表；固定发表通过 source 类型筛选后才申请直接边。只读输入内容、首消费者次序及 raw/pseudo/重复边资格保持不变 |
| `backend/arm64/compile/{constraints.h,constraints.cpp,residence_reads.cpp}` | narrow-read 和 transfer matcher 在通过 opcode/类型/home 初筛后显式准备直接边；preservation 和地址 alias 在实际消费证明点申请。初始 fixed 入口只取 roster，PrepareEarlyResultHomes 的只读窗口查询不要求图。准备 matcher 不再声明成 const 索引查询 |
| `backend/arm64/compile/{residence_preparation.h,residence_preparation.cpp,publication_copies.cpp,publication_values.cpp,publication_windows.cpp,residences.cpp,internal.h}` | ResidencePreparation 起始只借用 roster。copy/select/publication-view、memory-source/address/value 等候选分别在实际证明处申请边表；两个会申请分析的私有 memory matcher 改为非 const。已有 resolved-use 证明继续接续其前置直接边；recolor 的 scratch/fixed/width 事务规则不变 |
| `backend/arm64/compile/{lower.cpp,fpr_contracts.h,fpr_contracts.cpp,lower_flags.h,lower_flags.cpp,lower_integer_flags.h,lower_integer_flags.cpp,lower_vector.h,lower_vector.cpp}` | 初始 FPR 准备只取 roster；Select/consumer/branch 家族、pshufd、PF funnel/retention、窄整数 flags 和 scalar conversion-store 的实际候选显式请求所需边。FPR/resolved certificates 的已有候选门继续保留，const 基础查询不补建分析 |
| `backend/arm64/compile/{scratch.h,scratch.cpp}` | 精确 raw 与排除 pseudo 的计数不同才查询本块 pseudo edges，保留 SaveFlags(None) 的存在/计数；本地 MEM 参数运输在物理筛选后请求直接图。范围与真实 Def、重复使用与 foreign-ID 拒绝规则保留 |
| `backend/arm64/compile/{certificates.h,certificates.cpp}` | flags-only spill 先确认实际 MEM 位置，再证明 raw sole consumer。zero-store 在现有 IR 遍历中筛选本地/外块零来源，Stores 证明才触发图；incoming 根复用调用方输出做本次去重，不增加候选容器、不为枚举任意定义预建图。Zero/Stores 的接受、全局使用计数及循环拒绝规则保持不变 |
| `backend/arm64/compile/compiler.cpp` | 可选独立 audit 前显式准备完整参考使用图/别名图；与普通候选路径分开计量。必要 fixed、width、flags-only spill 证书及最终冻结顺序保留 |
| `compile/allocate/{gpr.cpp,gpr_writes.cpp,width.cpp,spill_regions.cpp}` | 只需按候选位置访问 IR 的路径取 PrepareIndex，spill 段只查 FindIndex；有真实 reverse-use 需求的 low32 family 保留显式 PrepareUses。width 证明继续在实际候选点 PrepareResolvedUses，重试共用同一事实对象 |
| `tests/{ir_contract_test.cpp,zero_store_verifier_test.cpp,support/register_alloc_test_support.cpp}` | 原用例补充轻索引/边表升级、未准备查询拒绝、完整普通准备不构图、外来零值的重复边去重及无候选不构图；测试辅助准备入口提供可变分析借用，没有新增测试文件或脚本 |

本批相对相邻版为 37 个 source 路径修改，无新增或本批新增退役路径；累计 source 快照仍为 420 个路径、107 个已删除路径。直接边的 dense/sparse/compact 路径、compact 溢出、外来 Def、重复参数与 pseudo/Lambda/Params 计数保留。图的准备不改变既有指令 roster、summary、逻辑 use-end 或已借出的直接 use span；resolved 仍保存独立派生存储。

二十五项定义/回调主体核对通过，记录实际定义签名与 hash：只归一化显式准备调用、可变准备访问及 roster provider 变化。原 direct-edge ForEachValue 回调与新 roster 回调逐项一致，只把同一节点的引用地址改成 roster 中的指针。zero-store 的 incoming 枚举、scratch 的 pseudo-count guard、spill 物理筛选前移和 readiness 断言是有意的控制变化，不冒称这些整段算法完全未变。

### 构建与验证轮次

- v1 九目标构建失败，原因是两个仍为 const 的私有 memory matcher 不能调用可变准备入口，另有扩展测试遗漏 AddressSpace 命名空间。进程真正退出后再修正；该轮无设备执行。十个 RV 语法单元实际执行均退出 0。
- v2 修正两个私有 matcher 和测试命名空间，九目标构建及最终无待构建检查通过。首次 IR 入口检查失败：我扩展原 caller-pool 用例时改变了 FeatureSet 来源，破坏了该用例的原条件；立即停止其余 native、静态和 PMU。
- v3 只修改测试，恢复原默认 features/调用方池场景，完整 target 场景独立使用模块 features、真实池和 facts。九目标通过。IR 检查中完整普通块“不建图”断言已通过，但末尾重复 FreezeGPRLocations 触发冻结后拒绝；再次停止后续验证。
- v4 只把重复冻结改成检查已冻结状态。九目标构建退出 0，最终 Ninja 无待构建。其余八份冻结二进制与 v3 完全相同，明确按 hash 复用不可变字节；IR 产物为本轮新构建。十个 RV 语法结果保留 v1 的实际执行身份及后续私有 ARM64/test-only 修改依据，未写成新执行。

两次失败的 IR gate、日志和 XML 均保留；未跳过、删除、改名或放松原断言。最终版本才完整执行一轮 36 次 native、十组静态和八次 PMU；主套件没有为早期测试场景问题反复运行。

| 最终验证 | 结果 |
| --- | --- |
| 36 次 native | 主套件退出 67，其余 35 次退出 0，全部符合预期 |
| 主套件 | 722 个用例、221735 条断言；原有 12 个失败用例/67 条失败断言、0 error；全部有序用例及 failure/error 内容与相邻版一致 |
| IR fast/slow | 各 40 个用例、900 条断言，均通过；相邻各 874，本批增加 26 条契约断言 |
| fixed-class | 2 个用例、39 条断言，均通过 |
| 寿命/持久化 | 18 组寿命及 14 组入口/关闭/调用 marker 与 checks 值一致 |

用例对照只归一化源码行号和二进制 classname 前缀；主套件成功断言含随机输入，不把总数变化当作新增覆盖。未完成完整 RV native、独立 static-latch fault、完整 shutdown 或异步跨线程交接矩阵。

### 静态代码与暂存布局

十组 CRC/checksum 全部正确。五组 bench 的完整有序记录和 shape 相同；其余差异只在 __d2b_D2A（0x253b00，大小 0x342）与 __dtoa（0x25f2d0，大小 0x1017）精确区间内。全部共同 PC 的每次记录、共同记录全局次序、区间外完整序列及 pool_bytes/ir_arena_bytes 相同，没有重跑凑 roster。

| CoreMark 组 | 相邻/候选记录数 | RA host bytes | per-PC host bytes |
| --- | --- | --- | --- |
| 默认 Function | 425 / 424 | 286788 / 286696 | 284168 / 284076 |
| bound-decode off Function | 425 / 424 | 286792 / 286700 | 284172 / 284080 |
| 实际 Block | 2417 / 2412 | 206104 / 205572 | 206104 / 205572 |
| fixed-class Function | 425 / 424 | 286352 / 286260 | 283600 / 283508 |
| fixed-class Block | 2416 / 2416 | 207608 / 207700 | 207608 / 207700 |

Block 由实际配置和记录核验，不借用旧 block_units 计数来推断；关闭 bounded decode 仍是 Function。Function 的 RA 总字节与逐 PC 和分别保留，不混作同一指标。

DWARF 测得 BlockAnalysisIndex 仍为 240 字节，新增 UsesReady 标志使用既有布局余量；SemanticFacts/BlockRecord 为 624/120，ConsumerBuilder 208，TargetPreparation 2288，PendingGPRBlock 296，AllocationPreparation 936，AllocationWriter 928，AllocationState 424，UnitActions 824，Prepared/Optimized Storage 为 1680/760，均与相邻版相同。ZeroStoreProof 仍为 96 字节。incoming 去重直接使用调用方输出，没有引入私有候选容器；这些固定布局与静态 pool 字段不证明峰值内存已降低。

### 红魔 PMU 诊断

红魔 NX729J/bf1359ff，CPU4/taskset 10，空 SVM_JIT_CACHE；实际 Function/Block 各采一个 2000/12000 bracket，顺序 reference 2000、candidate 2000、candidate 12000、reference 12000，共八次。全部退出 0，2000 CRC 4983、12000 CRC d340；每点前后采集 governor/current/max 和三个命名温度。

| 模式 | 指令数 slope / intercept 变化 | cycles slope / intercept 变化 | wall slope / intercept 变化 |
| --- | --- | --- | --- |
| Function | −0.262819% / +0.819267% | −1.318340% / +2.380919% | +1.056921% / +0.059335% |
| Block | −0.072123% / +0.167520% | +1.516814% / −1.561445% | +2.393922% / −1.992439% |

governor 均为 walt，max 均为 1171200 kHz，current 快照为 940800 或 1171200 kHz，不能按恒频处理。cpuss 29.3–33.2°C、cpu1-0 30.9–32.8°C、cpu1-6 28.9–30.9°C。本组仅为诊断：没有因果收益、性能无回退或全程锁频结论；intercept 含启动/guest/打印等成本，不是独立编译耗时，也不是正式 CoreMark 分数或三方性能验收。

v4 源码 digest：2c8ac637433d2642f210182bf852f8b62392a18714a1b61159b736a0aa48208e；归档 SHA-256：ebe208256bebb4584cd1619ad88963a0fad0f9cc796e771b69436a796bd7bc6d；launcher svm_demand_uses_v4 SHA-256：8587fcf17f3beab57daf81ccbf60b1655ab2301265a0dfdae75a0b3d9e341ee5。设备前后十二份 hash 与冻结产物、相邻 launcher、工具及 guest 一致。

四轮源快照/归档、构建/语法/格式记录、早期失败 gate、最终 native/JUnit、十组静态原始记录、八次 PMU 条件、定义核对、DWARF 和质量门保存在 /private/tmp/swiftvm-middle-results，以 r1-demand-uses-v1/v2/v3/v4 为前缀。仓库中没有新增 probe 或测试脚本。本批 90 项质量检查通过，batch_complete=true，full_goal_complete=false，formal_three_way_performance_accepted=false。

本批推进了真实按需构图，但 memory feeder 仍以 presence 触发，low32 family 仍以 view 范围触发；这些较粗的申请点需继续细化并计量，不能把本批普通块零构图覆盖当成所有输入的成本验收。完整语义宽度、共享 CFG/其余候选、工作区容量/初始化/峰值、异步交接、R2/R3/R5/R6 剩余规则与完整 R7、可复现 Android FEX 均继续施工。

## R1 首消费者合扫与 consumer 候选工作区

本批相邻版本为 r1-demand-uses-v4，最终版本为 r1-candidate-inputs-v2。memory-feeder 不再借完整直接图查询首个使用，low32 不再因存在 view 范围就建图；consumer 候选从 builder 私有小向量迁入外层工作区。各修改合并后集中验证，没有逐文件上机。

### 文件与实际职责

| 文件 | 本批行为 |
| --- | --- |
| `backend/arm64/compile/inputs.cpp` | 在既有 DescribeBlock 访问中，用同一次 ForEachValue 计算 spill 非 pseudo 整数参数类别和本地定义的首个 raw consumer。memory presence 只触发共享行缓冲，不触发图；每个真实本地定义只记录首次 Id 更大的使用。重复参数只记一次；pseudo 与 BitCast 仍是原始使用，incoming/同 ID foreign 指针不被当成本地。GetOperand 的 memory tie 在首个访存使用到来时生成，最终仍按 value 稳定排序 |
| `backend/arm64/compile/inputs.h` | DescribeAllocationInputs 继续接收 const AllocationWriter，新增显式 CompileWorkspace&；facts/workspace 可准备，物理 mapping 只读。两个 PrepareAllocationInputs 包装从实际 writer 传同一工作区，不新增私有 scope 或兼容重载 |
| `backend/arm64/compile/{spill_inputs.h,spill_inputs.cpp}` | Describe 接收共享扫描产生的整数参数布尔属性；保留 control 早返回、分段、site flags 与空 payload 规则，不再重复遍历 operands |
| `compile/allocate/gpr.cpp` | low32 family 起始只取 roster；LiveView/Transfer 经过既有物理、活跃区间、clobber 等初筛后，在第一次 raw-use 证明处 PrepareUses。FinalView/Recolor/重写和回滚规则保持不变；没有有效 Low32 角色的非空 view 范围不再构图 |
| `backend/arm64/compile/{gpr_inputs.h,gpr_inputs.cpp}` | 记录块内是否实际出现 SetHostGPR；没有发表则不运行固定发表的 reverse-use 查询。原匹配器只有遇到 SetHostGPR 才能成功，此筛选不改变接受集合。W-write、width-root、宽度/observer/使用次数判定保持原规则 |
| `compile/analysis/consumer_roles.h` | 四字节 ConsumerCandidate 的位置/角色声明从 ARM64 builder 私有类型收口为公共纯 IR 暂存元素；opcode 角色分类不变 |
| `compile/unit.{h,cpp}` | CompileWorkspace 持有 consumer_candidates；ConsumerCandidateScope 拒绝该缓冲的重叠借用，构造/销毁清空逻辑状态，异常退出同样释放占用。外层 Reset 检查借用结束并按原 64K entries 上限回收极大容量；候选、行缓冲、删除队列各有独立占用状态 |
| `backend/arm64/compile/lower_consumers.cpp` | 原有一次候选播种及全部顺序/反向访问借用共享候选缓冲，移除私有 StackVector<Candidate,16>。动作、Last、terminal occurrence 合取、PSTATE 与 pair/FCmp 资格及冻结结果不变 |
| `tests/ir_contract_test.cpp` | 扩充原输入、low32、workspace 用例：完整逐定义参考搜索对照首消费者，覆盖重复访存边、先非访存使用、SaveFlags(None)、BitCast；保留原 input-removal 场景，新增保留 view_count 但移除 Low32 角色的拒绝场景；覆盖候选缓冲重叠/嵌套、容量保留、重置与异常清理 |
| `tests/support/register_alloc_test_support.cpp` | 十四个物理冲突 fixture 的 DescribeAllocationInputs 调用显式传同一 writer 工作区，继续只读物理 mapping |

本批修改十三个 source 路径，无新增 source/test/script 或本批新增退役路径；累计快照仍为 420 个 source 路径、107 个已删除路径。没有保留 probe 或无意义的测试脚本。十五项定义主体核对通过，记录实际定义签名及 hash；只归一化工作区/候选访问、显式构图位置和“无 SetHostGPR”筛选记账。首消费者合扫与 spill 参数交接是有意的算法变化，以独立完整参考与现有契约验证，不能写成整段算法完全没变。

### 构建与集中验证

v1 九目标构建失败：新扫描从 const AllocationWriter 调用了可变 Workspace。等待构建真正退出后，v2 通过显式参数传工作区，保持物理视图只读，并迁移所有调用；未使用 const_cast 或改成物理可写描述。v1 无设备执行。v1 新 workspace/候选类型下的十个 RV 语法单元实际执行均退出 0；v2 增加的 ARM64 描述参数不被这些 RV 单元或其 inline support 调用，保留原执行身份及复用依据，没有伪记为新执行。

v2 九目标构建退出 0，最终 Ninja 无待构建；之后才冻结九份二进制、部署八份设备产物。最终一轮验证如下：

| 验证 | 结果 |
| --- | --- |
| 36 次 native | 主套件退出 67，其余 35 次退出 0，全部符合预期 |
| 主套件 | 722 个用例、221888 条断言；原有 12 个失败用例/67 条失败断言、0 error；全部有序用例及 failure/error 内容与相邻版一致 |
| IR fast/slow | 各 40 个用例、949 条断言，均通过；相邻各 900，本批增加 49 条契约断言 |
| fixed-class | 2 个用例、39 条断言，均通过 |
| 寿命/持久化 | 18 组寿命及 14 组入口/关闭/调用 marker 的模式与 checks 值一致 |

主套件成功断言含随机输入，不把总数变化等同于新覆盖；只归一化源码行号和二进制 classname 前缀，未排除、改名或放松旧用例。完整 RV native、独立 static-latch fault、完整 shutdown 和异步跨线程交接矩阵仍未补齐。

### 静态输出与布局

十组 CRC/checksum 正确。五组 bench 加 fixed-class Function CoreMark 的完整有序记录、全部 shape 相同，共六组；四组差异仅在 __d2b_D2A（0x253b00，大小 0x342）与 __dtoa（0x25f2d0，大小 0x1017）精确区间内。全部共同 PC 的逐次记录和全局次序、区间外完整序列以及 pool_bytes/ir_arena_bytes 相同，没有重跑凑 roster。

| CoreMark 组 | 相邻/候选记录数 | RA host bytes | per-PC host bytes |
| --- | --- | --- | --- |
| 默认 Function | 424 / 425 | 286696 / 286788 | 284076 / 284168 |
| bound-decode off Function | 424 / 425 | 286700 / 286792 | 284080 / 284172 |
| 实际 Block | 2412 / 2417 | 205572 / 206104 | 205572 / 206104 |
| fixed-class Function | 424 / 424 | 286260 / 286260 | 283508 / 283508 |
| fixed-class Block | 2416 / 2413 | 207700 / 207320 | 207700 / 207320 |

实际 Block/Function 分别通过配置与代码记录验证；关闭 bounded decode 仍为 Function。Function 的 RA 总量和逐 PC 字节和分别保留。

DWARF 对冻结产物测得对象大小，单位字节：

| 对象 | 相邻 | 候选 |
| --- | ---: | ---: |
| CompileWorkspace | 80 | 104 |
| CompileWorkspaceScope | 96 | 120 |
| ConsumerBuilder | 208 | 128 |
| ConsumerCandidateScope | 无 | 8 |
| Candidate / ConsumerCandidate 元素 | 4 | 4 |
| GPRInputBuilder | 856 | 856 |

InstructionRowScope 16、RewriteContext 248、SemanticFacts 624、BlockAnalysisIndex 240、AllocationWriter 928、AllocationPreparation 936、AllocationState 424、TargetPreparation 2288、PendingGPRBlock 296、UnitActions 824、Prepared/Optimized Storage 1680/760 均保持相邻大小。新候选向量容量归调用方长期暂存，旧 inline 小向量从每个 builder 移除；固定布局、测试中的容量保留与静态 pool 字段不等于完整增长次数/清零字节或峰值计量，不能据此宣称峰值内存下降。

### PMU 诊断与后续要求

红魔 NX729J/bf1359ff，CPU4/taskset 10，空 SVM_JIT_CACHE；实际 Function/Block 各采一个 2000/12000 bracket，顺序 reference 2000、candidate 2000、candidate 12000、reference 12000，共八次。全部退出 0，2000 CRC 4983、12000 CRC d340；每点前后记录 governor/current/max 和三个命名温度。

| 模式 | 指令数 slope / intercept 变化 | cycles slope / intercept 变化 | wall slope / intercept 变化 |
| --- | --- | --- | --- |
| Function | −0.004709% / −0.029956% | −0.073882% / +0.561340% | −0.231693% / +0.919563% |
| Block | −0.000095% / −0.231510% | +1.174731% / −2.874859% | +0.374640% / −2.035535% |

governor 均为 walt，max 1171200 kHz，current 快照为 1056000 或 1171200 kHz；cpuss 29.3–33.2°C、cpu1-0 30.1–33.6°C、cpu1-6 28.9–31.3°C。频率快照与单组 bracket 仅作诊断，不证明锁频、因果收益或性能无回退；intercept 含启动/guest/打印等成本，不是独立编译耗时，不引用正式 CoreMark 分数或三方验收结论。

v2 源码 digest：4abecc17d5933e463db663c271b91ba05ca2c8023f60bdba822d36161251f850；归档 SHA-256：37dc13e929700f9d777bebd226a5b837590ca8ec79eca40ea8a604a4e7ed9038；launcher svm_candidate_inputs_v2 SHA-256：24111a44599307773b207f1a1f425428ddb18942100f9be4a13f68c2b96cf6c3。设备前后十二份 hash 与冻结产物、相邻 launcher、工具及 guest 一致。源码归档、构建/语法/格式、完整 native/JUnit、十组静态、八次 PMU、定义核对、DWARF 和质量门保存在 /private/tmp/swiftvm-middle-results，以 r1-candidate-inputs-v1/v2 为前缀。本批 91 项质量检查通过，batch_complete=true，full_goal_complete=false，formal_three_way_performance_accepted=false。

语义宽度仍待落地。本轮核对确认，SSA 的值位宽/已知零位与 ARM64 实际 W 写入不能共用一个真假判断：U32 GetHostGPR 语义值不超过 32 位，却不证明原物理 X 高半部为零；现有 BitCast、width owner 和来源规则仍由目标层证明。没有为“补摘要”新增无消费者字段，也没有用物理 W-write 谓词冒充完整语义宽度事实。

完整语义宽度、共享 CFG/其余候选、其余 GPR/publication/flags 暂存迁移、增长/初始化/峰值计量、异步交接，以及 R2/R3/R5/R6 剩余规则、完整 R7 和可复现 Android FEX 继续施工。当前批次不等于完整 R1 或 R0–R7 完成。

## R1 纯整数位事实与 decode/pass 共用折叠

本批以 r1-candidate-inputs-v2 为相邻基线，落地 `r1-integer-bits-v2`。它推进 R1/R2 的共享值分析，不代表完整语义宽度摘要、R1 或 R0–R7 验收完成。

| 文件 | 实际职责与变化 |
| --- | --- |
| `compile/analysis/value_bits.h` | 新增 IntegerValueBits：只描述当前整数 Value 视图内的已知零/一位与符号复制范围；视图外不作承诺，与物理 W/X/home/alias 证明分开 |
| `compile/analysis/value_bits.cpp` | 新增无堆分配的纯查询、宽度转移；覆盖常量、BitCast、扩展/提取和简单 And/Or/Xor。总访问预算 32，遍历中的非法输入、循环或耗尽时返回 Width()==0，不凭残余部分事实折叠 |
| `compile/rules/value_rules.{h,cpp}` | FoldWidthValue 消费上述事实，只有完整结果确定且无 flags 输出时返回立即数；删除 FoldWidthConstant，无兼容包装。literal 按 operand view 读取原位模式的行为与原常量规则一致 |
| `compile/builder/ir_builder.cpp`、`compile/passes/values.cpp` | decode 的 FoldExtend 和残余 FoldIntegerImmediates 共用新入口，支持非 literal 来源的常量结果；保留类型、pseudo/flags guard、use 和改写所有权；不直接删除来源效果 |
| `compile/CMakeLists.txt`、`tests/ir_contract_test.cpp` | 注册两个新源文件；在现有契约用例中补数学参考、有限域、预算、加宽视图、fault 与 Phi 准入覆盖，无新测试文件或脚本 |

非 literal 的加宽 Value/BitCast 视图保留已有位的事实，不把新增高位认定为零。GetHostGPR U32 本身不能证明物理 X 高半部；ARM64 的 W-write、width owner、CFG 入口和 publication/alias 证书继续由原目标层判定，本批未删除或放宽这些证明。查询也不创建 consumer 图、SemanticFacts 缓存或逐值稠密表。尚未覆盖通用算术/移位和 CFG 宽度传播；预算只限制单次查询，不能据此认定总体查询成本或完整宽度工作已验收。

源码冻结摘要 `06efac8ca27f470a83c17ff9278308aecd76da1aedee0c78ea56307366e62871`，归档 SHA-256 `a122c1493aa4ac555442c5c54a505b3af2153385d8416f2a237e60d66c9d4b4b`；八个 source 路径有变，其中两个为新 analysis 文件，累计 422 个 source 路径、107 个删除路径。launcher `svm_integer_bits_v2` SHA-256 `77a95991555faee675c7dceb2880f27716369b6ad187f7cfd7c07569d938bad6`。v1 构建因整数条件表达式窄化与测试 const Value 调用失败，退出 1 且未部署；修正后的 v2 九目标构建退出 0，最终 Ninja 无待构建工作。十个 RV 源单元的成功语法检查保留 v1 原执行身份；v2 只改 analysis 的 .cpp 和 IR 测试，不重新运行相同语法检查。

红魔 bf1359ff 上集中执行 36 项 native 检查一次，退出码全部符合预期。IR 快/慢各 40 cases、5558 assertions，无 failure/error；固定寄存器输入 2 cases、39 assertions 通过。完整套件 722 cases、221511 assertions，仍为既有 12 个失败 case、67 个失败断言、0 error；全部有序 case 的 failure/error 签名与相邻基线一致，仅归一化源码行号和 suite 名，不排除失败项。18 项寿命与 14 项持久化/资源/调用检查的通过标记均一致。240 组 literal 转换使用逐位数学参考，另外遍历全部 256 个 byte 输入验证已知位与符号复制范围；也覆盖预算耗尽、未知高位、保留 fault load 和折叠不能隐藏非法 Phi。

十组静态运行均退出 0，2000 工作量 CRC 均为 0x4983，五组 bench 的五个 checksum 均正确；Function、bound decode 关闭、实际 Block 与固定寄存器池模式分别核对。五组 bench 的完整记录/shape 与相邻基线相同，十组 pool/IR arena 占用均相同。原始记录、共同 PC 的所有出现次序、增加/缺失入口及完整 shape 均保留。

| Core 配置 | 入口记录数，相邻→候选 | RA host bytes，相邻→候选 | 逐 PC host bytes，相邻→候选 |
| --- | ---: | ---: | ---: |
| 默认 Function | 425→424 | 286788→286688 | 284168→284068 |
| bound decode 关闭的 Function | 425→424 | 286792→286692 | 284172→284072 |
| 实际 Block | 2417→2414 | 206104→205872 | 206104→205872 |
| 固定寄存器 Function | 424→424 | 286260→286252 | 283508→283500 |
| 固定寄存器 Block | 2413→2418 | 207320→207916 | 207320→207916 |

三个 Function 配置在共同 PC 0x21d3c0 都出现真实变化：IR 383→379，默认/bound-off host 1404→1396，固定池 host 1384→1376。该入口位于同哈希 guest 的 out_vformat 模板函数内；两版现有诊断的该区域 decode/前置 pass dump 一致。另做各一次既有诊断开关运行确认输入，CRC 正确；诊断没有输出最终 FoldValues 后的完整 IR，因此不把它描述为逐节点最终改写证明。固定池 Function 的入口次序完全相同，shape 仅 host_bytes 减少 8，spill、helper capture 与压力直方图不变。其余共同入口不变；新增/缺失入口均位于原已记录的 __d2b_D2A 和 __dtoa 精确符号区间。没有把上述真实变化过滤掉，也没有为匹配入口集合重跑静态负载。

八次 PMU 为每种模式各一组 `reference-2000 / candidate-2000 / candidate-12000 / reference-12000`；参考也是相邻冻结的 SVM。每点前后记录 CPU4 governor/current/max 与三个温度节点。两点 CRC 分别为 0x4983、0xd340，均正确。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | +0.258796% / +0.364742% | +1.159215% / -1.771918% | -1.917307% / +2.101819% |
| 实际 Block | +0.025558% / +0.769472% | -1.285806% / +2.283760% | -1.507763% / +3.633814% |

采集前后 governor 均为 walt，current/max 读数均为 1171200 kHz；采样读数不证明整段锁频。cpuss 29.3–33.2°C、cpu1-0 30.1–33.6°C、cpu1-6 28.9–30.9°C。这是单组诊断，不是多轮中位数、性能提升、无回退或正式三方验收；截距含启动与运行时成本，不等于纯编译。新增查询的总体成本仍需按原质量门归因，不能以代码缩减掩盖正向指令变化。

DWARF 实测 IntegerValueBits 为 24 字节、查询状态对象 IntegerValueAnalysis 为 8 字节；20 个已有公共/目标类型的布局均与相邻版一致，包括 SemanticFacts 624、BlockRecord 120、BlockAnalysisIndex 240、CompileWorkspace 104、AllocationWriter 928、TargetPreparation 2288。它们不新增逐值缓存；不把固定对象大小当作堆分配或峰值成本测量。27 项既有规则定义/物理宽度、fixed、effects、flags、DCE 文件核对通过；改动范围内的宽度折叠本来就改变算法，不声称整段等价未改。

冻结源码、九个二进制、构建/语法/格式化、JUnit、十组静态、两份诊断、八次 PMU 条件、DWARF、完整差异和质量检查保存在 /private/tmp/swiftvm-middle-results，以 r1-integer-bits-v1/v2 为前缀。仓库中未新增临时 probe 或测试脚本。51 项质量检查通过；质量门只记录本批结构和正确性完成，full_goal_complete=false，formal_three_way_performance_accepted=false。

完整单元语义宽度摘要、共享 CFG/其余候选、其余 GPR/publication/flags 工作区、增长/初始化/峰值与异步交接，以及 R2/R3/R5/R6 剩余规则、完整 R7 和可复现 Android FEX 继续施工。

## R2 可变值工作队列与退出轮询契约

本批以 r1-integer-bits-v2 为相邻基线，最终冻结为 `r2-value-worklist-v19`。完成受影响 value/width 的队列接续，退役旧 ConsumerIndex，并修正本批代码布局变化暴露的非自环恢复 PC，以及扩展退休检查发现的轮询发布、环覆盖问题。结构与本批正确性检查通过；性能仍未验收，完整 R1/R2/R6/R7 与 R0–R7 目标继续开放。

| 文件 | 实际职责与变化 |
| --- | --- |
| `compile/passes/value_worklist.{h,cpp}` | 新增可变 IR epoch 的节点身份、简化/传播原因位及去重队列。最终编号前工作，不插入节点；删除时先移除身份索引，再释放节点，过期队列项不解引用已释放对象 |
| `compile/unit.{h,cpp}` | 调用方工作区保存 ValueRow、ValueLookup、pending queue、Block links/uses 五组可复用数组。ValueQueueScope 拒绝重叠借用，正常和异常退出均清理；每组超过 64K 元素容量时不继续保留 |
| `compile/passes/passes.h`、`dce.cpp` | RewriteContext 统一通知参数改写、输入 use 变化和删除。队列存活期间仍维护 HIR uses、隐式 roots、fault/effects 与 DCE；不通过可选简化预算截断必要清理 |
| `compile/passes/values.cpp` | 原全量 immediate folding 改为单候选规则；保持原短距离、单 use、类型和 flags 准入。常量 CSE 仍是一次有序扫描，保留八指令窗口、label 边界、typed Lambda/Params 和 vector 规则，尚未迁为受影响窗口算法 |
| `compile/passes/widths.cpp` | 原三轮固定扫描改为单候选扩展、掩码和提取规则；查询可变 uses，按原始块内位置维护 128 指令窗口。IsU32Consumer、DirectSource、ReplaceSingleUse 原规则正文保持，不加入更宽的消费准入 |
| `compile/pipeline.cpp` | value/width 工作队列覆盖后续 DCE/flags 的反馈，收敛后才释放借用、最终编号和封口；优化结果不持有队列指针 |
| `backend/riscv64/jit/compile.cpp` | 接入 Values 队列及 DCE/flags 反馈；保留 PureScalarFlags 的保守准入，并在初始 value folding 后、DCE 前固定其结果，未扩大 RV scalar flags 支持范围 |
| `arm64/compile/gpr_inputs.cpp`、`residences.cpp`、`width_contracts.cpp` | 以 SetHostGPR 的原始 operand view 判断实际发表宽度，再解析 BitCast。部分字节/半字发表不充作完整 home 更新；实际 U32 的物理 W-write 证明仍独立检查。输入、事务与最终读证明分别守住各自边界 |
| `arm64/jit/translator_region.cpp`、`translator_terminal.cpp`、`translator_cold_paths.cpp`、`translator.h` | 每个 fault-only poll 显式记录已完成边的实际目标 PC。共用退出 stub 只解析 recovery offset，不再用源块地址覆盖 guest PC；保留各 Call 故障站点的单次帧发表 |
| `arm64/jit/jit_context.cpp`、`backend/retirement_control.cpp` | latch 关闭时三处请求轮询读取完整的 64 位 acquire word；latch 开启时退休发布在写请求位后 Arm，包括关闭期间的新 owner 注册。Resume 仍只清除退休位并在整个 request word 为空时解除保护 |
| `arm64/compile/edges.cpp`、`arm64/jit/translator_terminal.cpp` | 把 ExternalLinkBlock 的明确目标纳入环分析。同 Function 的静态 dispatcher 回跳也保留有序 cycle cut，不因目标属于 region 而漏掉轮询；普通前向链仍允许延后到实际轮询点 |
| `backend/code_serial.h`、已有入口/缓存测试 | 代码兼容版本升为 32，AOT schema 仍为 6。实际 AOT 安装器分别拒绝格式 30、31，且不发布旧代码、不破坏已有有效 mapping |

Block 只在实际 use 查询/传播需要时建立可更新的反向边，保留 operand slot、重复 use、Lambda/Params 和编辑后的 unlink/relink。Function 复用既有 HIRUseList，不另建全函数 use 图；未注册的手工 HIR 才采用稳定 roster 的保守查询。队列只播种可达 RPO 的候选，但保存不可达节点身份以安全接续全 Function DCE。初始播种按前向规则顺序处理，后续修改只排入受影响节点及可传播的 bit-transfer 链。

可选简化总预算为 `8 * roster_count + 32`。耗尽后停止继续折叠并清空 pending 状态，use 维护、DCE、flags 清理和封口照常完成。它是终止与成本上界的一部分，不代表已经测得整个编译过程的线性成本。decode 仍使用前批无分配的纯值规则；本批队列属于 decode 之后的可变优化 epoch，不把 CFG、反向 uses 或物理寄存器证明塞进 decoder。

CRC 问题的最终根因是非自环的 fault-poll 恢复记录。某次四字节代码布局变化触发 RCpc 自适应退出后，已完成的 `0x21ad40 → 0x21ad4f` 边被恢复到源块，重复执行了矩阵行更新。外部布局/开关隔离与 trace 将问题定位到恢复 PC；取消 coalescing 或填 NOP 只能改变触发条件。最终保留 coalescing，修正每站点目标并用现有 IR 契约测试覆盖两块非自环、真实受保护页故障、清除中断后的再次运行及双方计数。临时布局实验头、两个 SVM_PLACEMENT 配置项及其专属 parser 用例均已退役，没有把 padding 或按 guest PC 的例外留下。

扩展退休矩阵还确认：只读取 request 低字节会漏掉 bit 63 的 Signal 和 bit 62 的退休请求；只写退休位而不保护 latch 页也会漏掉 fault-only poll。另一个 published 环因 ExternalLinkBlock 未进入环图而完全没有 poll。这三处生产修正分别接入完整 word 读取、发布后 Arm 和实际边覆盖。测试按已有有界退出语义验证：本批两节点场景可以在前向边退出，也可以完成一个选定 target 后在 cycle cut/有限 callee 的主机返回处退出；不得再次执行 source，退出 PC、NZCV、结果及返回帧必须对应最后实际完成的路径。新增 `region-dispatcher` 模式验证同 Function 静态回跳。有限 callee 显式 SetLocation；完成调用检查 source allocation 内的真实 continuation，取消调用仍检查 null external frame。

早期把一个手工入边 SSA 用例解释为全局 fixed-home 冲突的归因已撤回。其 host dump 实际显示跨 dispatcher 返回仍假定 X0 保存虚拟值；该用例及实验性全局 home 查询均未保留。本批保留的是上述原始发表宽度契约。Function 非局部 SSA 穿过 dispatcher、导出内部入口和中断恢复时的准入/保存协议仍是 R7 待交付项，不能据本批环退出修正宣称该问题已解决。

源码摘要 `20bd027df77b79a5ca3715ac1d25ed751c0cb2dae30692e3930c8b7445ad19ff`，source overlay 归档 SHA-256 `5c4e9f393aed0b041b9a7fbf62ee5b4501c53c1d463f87b70ddf6f685715696d`；累计 426 个 source 路径、109 个删除路径。相邻树比较为两个新文件、34 个已有文件修改及两个退役文件：`ir/consumer_index.h`、`arm64/jit/placement_experiment.h`。manifest 的 new_since_adjacent 表示新进入 overlay 的路径，实际新增/修改/退役分类另存 source-comparison.json，不能混读为全部新建文件。launcher `svm_value_worklist_v19` SHA-256 `971405a2de980ebe8ab70dd952b49ae264cf74f2d944a6f2cffa805b6eddfdde`。14 个 Android 目标构建退出 0，最终 Ninja 无待构建工作；十份 RV 语法检查复用 v12 的原执行身份，最终 RV 后端目标也构建通过，红魔不作为 RV native 验收设备。

最终候选在红魔 bf1359ff 上集中执行 125 项 native 检查，均有实际终止结果并符合预期；不把前期失败/超时的候选计为通过。IR 快/慢各 45 cases、5674 assertions、0 failure/error，固定寄存器输入 2 cases、39 assertions 通过。原 consumer index 用例更新为可变 use 身份/编辑契约，另增加五个工作队列、宽度/发表与非自环故障恢复用例。

公共套件为 721 cases、222224 assertions、11 个失败 case、66 个失败断言、0 error；全部共同用例的有序 failure/error 签名与相邻基线一致，只归一化源码行号。相邻版被退役的 probe parser 用例原本在手机上于 `fd >= 0` 失败，删除它使既有失败断言从 67 降至 66，并非修复了这项环境失败。过程中旧 169 配置字段数量断言还曾与该删除互相抵消，已删除这项过时硬编码，保留配置表与字段清单一致性检查；因此退出码相同不能替代逐用例核对。其余 18 项 HIR 寿命、14 项持久化/资源/调用通过标记全部与基线一致。追加的 89 项由 54 项 JIT/AOT/disk 静态 latch 场景、一项旧缓存拒绝检查及 34 项退休控制/循环/static/dynamic 场景组成，均有通过标记；循环退出无 rescue interrupt。旧格式安装器检查共 35 项，通过并保留现有 live entry/mapping。

十组静态负载均退出 0；五种 Core 配置的 2000 工作量 CRC 均为 0x4983，五种 bench 配置各自五个 checksum 均正确。Function、bound decode 关闭、实际 Block 与固定寄存器配置均核对实际模式。十组 pool/IR arena 占用与相邻版相同；这不包含工作队列新增堆容量和峰值的完整测量。

| Core 配置 | 有序入口记录数，相邻→候选 | RA host bytes，相邻→候选 | 逐记录 host bytes，相邻→候选 |
| --- | ---: | ---: | ---: |
| 默认 Function | 424→425 | 286688→288660 | 284068→286036 |
| bound decode 关闭的 Function | 424→424 | 286692→288572 | 284072→285948 |
| 实际 Block | 2414→2427 | 205872→207844 | 205872→207844 |
| 固定寄存器 Function | 424→425 | 286252→288412 | 283500→285652 |
| 固定寄存器 Block | 2418→2425 | 207916→209384 | 207916→209384 |

这些记录包含同一 PC 的多次编译，不是去重后的入口集合。默认 Function 的 spill defs/loads/stores 为 330/102/73→321/90/61；Block 为 771/360/261→735/307/208，target_overflow 均为 0。三个 Function bench 的代码各增加 20 字节，两个 Block bench 各增加 8 字节。共同 PC 的实际变化、新增/缺失入口及每个 PC 的全部出现次序均保留；默认 Function 有 193 个共同 PC 变化，新增 0x21aff2、0x21ad7d 等矩阵区域记录，不能将全部差异归为格式化函数。Block bench 的 0x400e21 经离线反汇编确认从直接写 w29 的 ANDS 变为临时 w9 结果、w12 捕获和 w29 的单独零写，共增加两条指令；其余体积与运行周期差异继续归因。

DWARF 选取结果复用定义未变的 v12 原产物：CompileWorkspace 104→224、CompileWorkspaceScope 120→240、RewriteContext 248→256；其余 19 个相邻既有类型布局不变。新增 ValueRow/ValueLookup/ValueLinks/ValueUse 分别为 24/16/8/20 字节，ValueQueueScope 为 8、ValueWorklist 为 64 字节。另测得已有 PendingExitPollFault 为 24、SvmConfig 为 840，不能把新测量的已有类型当作本批新类型。固定对象大小和数组保留上限均不替代增长次数、初始化流量及峰值成本验收。11 个选定的 CSE/宽度 matcher、必要 DCE、物理 W-write 与 RV scalar flags 规则正文核对一致；worklist 调度与新增发表准入属于真实算法变化。

PMU 为每种模式各一组 `reference-2000 / candidate-2000 / candidate-12000 / reference-12000`，两点 CRC 分别为 0x4983、0xd340，均正确；参考为相邻冻结 SVM。每点前后记录 CPU4 governor/current/max 和三个温度节点。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | -0.290124% / -1.975413% | +3.603831% / -2.747520% | +2.832057% / -0.844154% |
| 实际 Block | -0.132458% / -2.973482% | +5.391874% / -3.360244% | +5.135998% / -1.216674% |

governor 均为 walt，current/max 快照均为 2188800 kHz；cpuss 48.9–53.6°C、cpu1-0 50.8–56.7°C、cpu1-6 45.7–54.0°C。不能据快照认定全程锁频，也不能据温度把正向 cycles 差异直接排除。单组诊断中的指令减少不构成性能提升或无回退结论；截距含启动和运行时，不能解释为纯编译成本。事件运行比未给出，不报告 IPC。当前代码体积增长及周期斜率上升均作为未解决的性能事项保留，正式多轮/三方质量门尚未通过。

冻结源码、14 个二进制、构建/格式化/语法记录、JUnit、125 项实际执行、十组静态完整次序、八个 PMU 点及条件、布局、规则正文和输入完整性证据保存在 /private/tmp/swiftvm-middle-results，以 r2-value-worklist-v19 为前缀；前期失败和诊断也保留原身份。验证前后 13 个部署二进制、相邻 launcher、simpleperf 与两个 guest 共 17 个 SHA-256 均一致，源码未偏离冻结快照。外部按 PC/开关修改的诊断二进制已清理，仓库未新增临时 probe 或测试脚本。full_goal_complete=false，formal_three_way_performance_accepted=false。

## R2 flags 与完整数据发表的关联

本批相邻基线为 r2-value-worklist-v19，最终冻结为 `r2-flags-value-coupling-v5`。修正的是 value folding 的收益判断：仍需产生 flags 的 U32 生产者已经计算出了结果，将它的完整 GPR 发表扩展边折成另一个常量，可能反而增加结果捕获和独立寄存器写。纯值数学规则及物理 W-write 证明未改；本批不把提交前的 flags 观察问题宣称为已解决。

| 文件 | 实际变化 |
| --- | --- |
| `compile/passes/values.cpp` | 先运行共享 FoldWidthValue；只有确实得到常量时才查询单一 consumer。若 ZeroExtend32To64 的 U32 输入仍带 live pseudo，唯一 consumer 是 offset=0、原始输入 view 为 U64 的 SetHostGPR，则保留扩展边，让 flags 和完整发表共用生产者。部分发表及其他 consumer 继续按原规则处理 |
| `compile/optimized_unit.cpp` | 在现有 dump_ir 与 dump_ir_post 同时开启时输出封口后的 final IR，Function 按可达 RPO 输出。此前 post-uniform/post-elim 输出早于最后的 value/width 反馈，不能作为最终 IR 的证据。默认关闭，不增加新开关或临时 probe |
| `tests/ir_contract_test.cpp` | 增加已有 native 契约测试中的一项：Block/Function × 是否观察 flags × U32/U64 投影返回类型 × U16/U64 原始发表 view，共 16 组，每组执行两个动态输入。核对完整/部分 home、PF/ZF/SF/CF/OF，并在实际 level-2 寄存器池及 coalescing 准入成立时检查共享的单条 ANDS 零写 |

真实 frontend 的 ZeroExtend32To64 可以保留 U32 返回类型，而发表 operand 使用 U64 view；本批同时覆盖两种投影返回类型，没有通过改写共享生产者的类型来取得零扩展。是否保留扩展边属于可变 IR 的收益判断；decoder 的无分配纯规则不查询反向 uses、pseudo 或物理寄存器池。

新增测试按真实 guest 提交序列在观察前放置 AdvancePC。早期 v3 在部分 U16 发表场景中暴露了独立问题：ClearFlags(CF) 尚在 pending 窗口，TestFlags(CF) 在 AdvancePC 前读到了旧 packed carry。该场景不满足本批完整发表 gate，不能归因为本次 gate 的错误，也不能据此断言该 IR 序列不受支持；提交前观察仍是 R2 待修正契约。v4 的共享指令检查失败则来自手工 fixture 仅映射 x22，未启用实际 level-2 coalescing 池。v5 使用完整的 12 项 ABI 映射并检查实际池策略，保留原 flags 期望值；没有通过关闭观察或删除失败断言过关。v2 的缺失完整 State 头导致的构建失败、v3/v4 失败及其诊断均保留原身份，不计入最终通过结果。

源码摘要 `e731d19e9030a9bd930a524135c80ebcdaa2f8bb7fce23def5d9a4ef1ab38928`，source overlay SHA-256 `b749470273fd85c29f6fdf5e1cdf158882bede9c309c655ac19adfbd51c26ed3`。426 个 source 路径、109 个删除路径，相邻版仅修改上述三个已有源文件，没有新增源文件。14 个 Android 目标构建退出 0，最终 Ninja 无待构建工作。launcher `svm_flags_value_coupling_v5` SHA-256 为 `7a3a88e1dbd9c57f30fad9aa1de685d6ff1412d7e6202bba2d6342d1f6840fa3`；v4/v5 只调整 native fixture，launcher 与其余非 IR 测试二进制复用 v3 的相同字节。布局定义未变，沿用上一批布局测量的执行身份，不把复用记作新测量。

最终候选在红魔 bf1359ff 上实际执行 125 项原生检查，退出结果均符合预期。IR 快/慢各 46 cases、5866 assertions、0 failure/error，新增一项上述测试；固定寄存器输入仍为 2 cases、39 assertions。主 JUnit 为 721 cases、221855 assertions、11 个失败 case、66 个失败断言、0 error，共同用例顺序及 failure/error 签名与相邻版一致，121 项其他 native 通过标记一致。这里不声称全部测试零失败，也不声称断言总数相等。

主 JUnit 相邻版为 222224 assertions，本批少 369 次。为核查差额，两版以相同 rng-seed=2895199340 各补跑一次现有 XML reporter，均以既有 66 失败断言结束；626 个顶层用例的顺序和成功/失败结果一致，122 个嵌套 section 的计数一致。两次总数分别为 221548、221692，同一候选的总数也不同于前次 JUnit 运行。XML reporter 不输出全部顶层用例的断言数，因此补跑未完整定位原始差额；不能把差额直接归为被删除的测试、某一条代码体积变化或随机种子。原始输出与此限制一并保留，不继续为凑齐断言总数反复跑整套。

十组静态负载均实际退出 0；五种 Core 的 CRC 均为 0x4983，五种 bench 的各五个 checksum 均正确，并核对实际 Function/Block、bound-decode 和 fixed-class 配置。相邻静态原始日志复用 v19 的已完成运行，重新核对日志 SHA 与全部有序记录，没有重跑相邻静态负载。十组 pool/IR arena 记录相同，不代表新增工作队列的堆峰值已验收。

| Core 配置 | 有序记录数，相邻→候选 | RA host bytes，相邻→候选 | 逐记录 host bytes，相邻→候选 |
| --- | ---: | ---: | ---: |
| 默认 Function | 425→425 | 288660→286132 | 286036→283508 |
| bound decode 关闭的 Function | 424→425 | 288572→286136 | 285948→283512 |
| 实际 Block | 2427→2416 | 207844→205368 | 207844→205368 |
| 固定寄存器 Function | 425→424 | 288412→285712 | 285652→282952 |
| 固定寄存器 Block | 2425→2416 | 209384→206744 | 209384→206744 |

三个 Function bench 分别为 19928→19896、19936→19904、20100→20068 字节；两个 Block bench 为 16476→16468、16500→16492 字节。Block bench 的 0x400e21 从 68 降为 60 字节，离线反汇编确认 `ands w9,wzr,wzr; mov w12,w9; mov w29,#0` 已恢复为 `ands w29,wzr,wzr`。默认 Function 与 Block 的 spill defs/loads/stores 分别保持 321/90/61 和 735/307/208，target_overflow 仍为 0。默认 Function 有 142 个共同 PC 记录变化；Block 也存在新增/缺失和重复编译记录，全部有序出现及具体差异已保留，不能以只比较去重 PC 或单个 bench 替代完整静态结果。

PMU 仍为 Function/Block 各一个 `reference-2000 / candidate-2000 / candidate-12000 / reference-12000` 诊断组，参考为本轮重新执行的 v19 二进制；八点 CRC 均正确，逐点保存前后 CPU4 governor/current/max 与三个温度节点。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | -0.046272% / +0.253899% | -1.800891% / -0.389562% | -1.395760% / -1.412082% |
| 实际 Block | -0.079923% / +0.217473% | -2.727737% / +2.579062% | -3.243471% / +4.222281% |

governor 为 walt，current 快照 940800–1171200 kHz、max 为 1171200 kHz；cpuss 30.5–33.6°C、cpu1-0 30.9–34.4°C、cpu1-6 29.7–32.0°C。本轮条件与上一批 2188800 kHz 快照不同，不能将两批相对变化叠加为累计收益。单组负向 cycles 斜率只说明这次采样的结果，不构成多轮无回退或三方性能验收；截距含启动/运行时，不能充作纯编译成本，未给事件运行比也不报告 IPC。

冻结源码、14 个二进制、构建记录、125 项原生执行及两个补充 XML 执行、十组完整静态次序、八点 PMU/条件、反汇编与输入完整性证据均在 /private/tmp/swiftvm-middle-results，以 r2-flags-value-coupling-v5 为前缀。首次 hash 快照位于第一项 IR triage 之后、剩余 124 项和静态/PMU 之前；不得写成早于全部 125 项。最终补充 XML 后再次取快照，17 个设备二进制/工具/guest 输入的 SHA 均与首次一致，全部主机冻结二进制与源码也一致。没有保留临时 probe 或增加测试脚本；完整目标、提交前 flags 观察、未归因的断言差额及正式性能质量门继续开放。

## R2 共享 effects 与 flags 观察契约

本批相邻基线为 r2-flags-value-coupling-v5，最终冻结为 `r2-flag-observers-v4`。公共 flags 位语义及 helper 写范围查询已接入，修正上一批明确保留的提交前 ClearFlags 观察问题，并补齐部分提交和融合后的实际读取位置。完整 R2 的 guest-state/观察传播与其他阶段仍未完成。

| 文件 | 实际职责与变化 |
| --- | --- |
| `compile/analysis/effects.{h,cpp}` | 新增共享 ConditionFlags、InstructionFlagAccess 和 HelperUniformWrites。区分 guest flags 读取与必然覆盖，AL/NV 无条件条件不需要 flags，未知条件保守回退。GetFlags 按实际完整 packed read 处理，FCmpCondSet 的具名关系不当作 guest flags 读取；未知 helper/flags writer 不能作为必然覆盖来杀死前序需求 |
| `compile/builder/guest_state.cpp`、`compile/passes/state.cpp` | 删除两份 helper opcode/范围校验，改用同一查询。缺失表、空 ranges 与非零 count、越界和溢出均回到原全失效路径。范围元数据仍只描述写入，不用它推断 helper 没有读取或删除前序 store |
| `compile/passes/flags.cpp` | Function 的反向 transfer 摘要和残余读取需求复用 FlagAccess；InvertCarry 明确读/写 C。标签与 CFG 传播、carry 保护、逐 writer 的删除/缩窄规则仍由 pass 管理；未知 effects 保守保留需求 |
| `arm64/compile/lower_integer_flags.cpp`、`arm64/jit/translator_alu_scalar.cpp`、`translator_select.cpp` | 删除目标/发射器重复的条件掩码表。目标窗口仍单独保护 BranchOnlyFlags 的原始 PSTATE 消费，它不是公共 guest flags 读取 |
| `ir/ir_meta.cpp` | 将 GetFlags 从纯值表移至 ReadFlags，与其实际读取完整状态字及公共查询一致；不复制一份逐指令静态 effects 缓存 |
| `arm64/jit/translator.{h,cpp}`、`translator_flags.cpp` | PrepareFlagRead 在实际读取所需的 pending clear 位时先合并旧 lazy 定义，再施加 clear；不暗中依赖 AdvancePC。部分 ClearFlags 移除所有被清位的 pending NZCV 需求，不能在后续合并时复活旧值。PrepareCarryInput 只在 pending mask 真正覆盖 C 时沿用物理 carry，否则先保存其他 lazy 位并恢复 canonical carry |
| `translator_alu_scalar.cpp`、`translator_flags.cpp`、`translator_select.cpp` | ADC/SBB/InvertCarry 与窄 carry 融合使用统一 carry 准备；TestNotFlags、CondSet 快捷路径检查实际所需位的覆盖。直接 Select 融合也调用 PrepareFlagRead，覆盖原 CondSet 被省略后不再执行其读取准备的情况；删除 TestFlags 中已不可达的第二个 FLAGS_REGS carry 分支 |
| `backend/code_serial.h`、已有持久化/旧缓存测试 | code format 升为 33，AOT schema 保持 6。生产安装器实际拒绝 30、31、32；每个被拒旧版本的三项拒绝/映射检查及三项再次执行检查均通过，总检查从 35 增至 41 |

三个条件表及两份 helper 范围规则已合并为实际共用实现，没有留下旧 API 转发别名。查询不分配 consumer/CFG 图，不新增逐指令 effects 容器；公共工作区、物理分配和 prepared 结果的数据成员未改。这些事实不能替代尚未交付的堆增长、初始化流量和峰值测量。现阶段 InstructionFlagAccess 对 helper 仍采用保守读取，细化已知 helper 的 guest-state/观察 effects 及其成本属于后续工作，不能据此宣称整个 effects 已统一。

新增现有 ir_contract_test.cpp 中的一项 native 回归，覆盖 Block/Function × 优化开/关 × 独立 clear/已有 producer × 读取前有/无 AdvancePC 共 16 组，每组运行全一及符号边界两个动态输入。读取包括 TestFlags、TestNotFlags、完整 GetFlags、CondSet、CondSelect、CondSet→Select 融合、ADC、SBB、InvertCarry、窄 carry add，以及没有最终 reader/AdvancePC 的直接主机返回状态。每次读取前重新产生/清除相关位，避免只验证第一个 reader 强制发表后的其他读取。

v1 的首项完整 IR triage 在 producer + AdvancePC + ADC 场景失败：dirty NZCV 仍含其他位，却不包含已被清除的 C，ADC 继续消费了旧物理 C。v2 构建通过，但静态审查发现 TestNotFlags、CondSet 和窄 carry 融合的同类准入问题，未部署/执行 v2 矩阵，合并修正后进入 v3。v3 的新选择用例再次发现原 CondSet 被融合覆盖后，Select 绕过了其 pending clear 准备。v4 将准备放在实际消费者处；全部原始断言保留，没有把提交前读取改成必须先 AdvancePC，也没有通过关闭融合过关。前期失败/仅构建版本不计为通过。

源码摘要 `1b15fe28ece55e3b6152980c26df640281fadc5d5fecb7d2c0b03b27b94c179f`，source overlay SHA-256 `9260d31bef4a7f2f6270640f5e4f48d8ae1bb96354ced629b81db4e58a2d8eec`。429 个 source 路径、109 个删除路径，相邻树实际新增 effects 两文件、修改 15 个已有文件，无新增删除路径；本批首次进入 overlay 的 ir_meta.cpp 是已有文件，不能算作新增源文件。launcher `svm_flag_observers_v4` SHA-256 `22971ca95700a5fc9b38a09625da695a23cac2c0398d772de6e9e384f5ff0f8a`。14 个 Android 目标及最终 Ninja 无待构建工作均退出 0；13 个 ARM64 适用二进制部署到红魔。RV 后端目标构建通过，仍不以 ARM64 手机替代 RV native 验收。

最终候选执行 125 项既有 native 矩阵，另执行一次仅选择新增 pending 用例的 FLAGS_REGS=0 回归。IR 快/慢各 47 cases、6338 assertions、0 failure/error；packed 回归 1 case、472 assertions、0 failure/error。主 JUnit 为 721 cases、221747 assertions、11 个既有失败 case、66 个失败断言、0 error；共同用例顺序及 failure/error 签名与相邻版一致。120 项其他 native 通过标记一致，cache-version 一项按增加的旧格式拒绝检查由 35 变为 41，三个实际旧格式拒绝结果均保存。循环退出均未使用 rescue interrupt。

主 JUnit 相邻版为 221855 assertions，不声称成功断言总数完全一致；上一批未完整归因的总数变化限制继续保留。本批未为凑齐总数再次执行整个套件。packed 回归首次命令在主机 ADB listener 启动阶段失败，未执行设备测试；随后用关闭 shell glob 的直接命令正确选择 [pending] 并实际退出 0。两次启动身份分别保存，主机启动失败不记为 native 用例失败或通过。

十组静态负载均实际退出 0；五种 Core 的 CRC 均为 0x4983，五种 bench 的各五个 checksum 均正确。相邻静态日志复用已完成的 v5 运行，重新核对 SHA、全部有序记录与实际模式；本轮 PMU 的相邻二进制另行实际运行。十组 pool/IR arena 与相邻记录一致。

| Core 配置 | 有序记录数，相邻→候选 | RA host bytes，相邻→候选 | 逐记录 host bytes，相邻→候选 |
| --- | ---: | ---: | ---: |
| 默认 Function | 425→424 | 286132→286072 | 283508→283448 |
| bound decode 关闭的 Function | 425→424 | 286136→286076 | 283512→283452 |
| 实际 Block | 2416→2416 | 205368→205116 | 205368→205116 |
| 固定寄存器 Function | 424→424 | 285712→285744 | 282952→282984 |
| 固定寄存器 Block | 2416→2419 | 206744→206836 | 206744→206836 |

五种 bench 的完整大小/IR 次序与 RA shape 均一致；host bytes 依次保持 19896、19904、16468、20068、16492。Core 大小记录的差异均在既有两个格式化函数范围，范围外的有序大小记录相同；这不等于已证明所有机器码字节相同。Function 的 0x25f61f 两次出现各增加 16 字节，默认及 bound-off 另少一条 0x25fb00、92 字节的记录，因此净值为 -60；固定寄存器 Function 没有这条缺失记录，净增 32。Block 的新/缺失记录及全部重复出现均保留，不能把 -252 或固定寄存器 Block 的 +92 直接解释为同一 guest 编译体的缩减/增长。五种 Core 的 spill defs/loads/stores 均与各自相邻版相同，target_overflow 均为 0。

PMU 仍为每种模式各一个 `reference-2000 / candidate-2000 / candidate-12000 / reference-12000` 诊断组，参考为 v5。八点 CRC 均正确；CPU4 绑核、缓存、事件和逐点前后条件记录协议保持。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | +0.015926% / +1.046908% | -0.751281% / +0.702872% | -2.066069% / +2.786322% |
| 实际 Block | +0.027520% / +0.280888% | +1.992728% / -2.909355% | +2.270115% / -5.515227% |

governor 为 walt，current 快照 940800–1171200 kHz、max 为 1171200 kHz；cpuss 30.5–34.0°C、cpu1-0 31.3–34.0°C、cpu1-6 29.3–31.6°C。单组不能建立收益或无回退结论，也不能凭温度排除 Block 的正向 cycles/wall 差异。指令截距增加及 Block 周期变化作为未解决性能事项保留；截距包含启动/运行时，不归因为纯编译成本或某个共享查询。现有静态 profile 的阶段时间可复查，但不替代增长/初始化/峰值计量。未给事件运行比，不报告 IPC；正式多轮三方性能质量门继续开放。

原始日志、JUnit、125 项矩阵及一项 packed 执行、冻结源码/14 个二进制、十组静态完整次序、八点 PMU/条件及完整性证据在 /private/tmp/swiftvm-middle-results，以 r2-flag-observers-v4 为前缀。首个 17 项 SHA 快照早于本批全部 native/静态/PMU；最终快照与其一致，13 个部署二进制均匹配主机冻结字节。429 个源路径未偏离冻结快照；选定的 value-bits、纯规则、worklist、values/widths、必要 DCE 与三份物理 GPR 宽度/发表证明源文件均与相邻冻结版相同。没有新增临时 probe 或测试脚本，full_goal_complete=false，formal_three_way_performance_accepted=false。

## R1 value 候选轻路径与 helper flags 需求

本批相邻基线为 r2-flag-observers-v4，最终冻结为 `r1-value-presence-v3`。生产 value/width 入口在没有实际规则候选时跳过 ValueWorklist 身份表与借用，同时保留常量共享和外层必要清理；flags 需求复用已有实例级 helper effects。它关闭了一个不必要的暂存构建入口，不代表完整 R1/R2 成本或性能验收已经完成。

| 文件 | 实际变化 |
| --- | --- |
| `compile/passes/value_worklist.{h,cpp}` | BeginValueCleanup 统一筛选与启动。Block 检查本块，Function 复用既有可达 RPO，找到首个候选即停止资格扫描；有候选才建立原完整 roster/queue，保持不可达身份和删除安全。队列与入口共用同一 IsCandidate，未扩大优化准入 |
| `compile/pipeline.cpp`、`passes/values.cpp`、`passes/widths.cpp`、`riscv64/jit/compile.cpp` | 四个生产入口接入统一启动。无候选时不构造 ValueWorklist/ValueQueueScope；Values 模式仍执行一次有序 CSE，Widths-only 不额外做 CSE；DCE、flags 反馈与最终编号/封口照常完成。RV 的 PureScalarFlags 仍在初始值清理后、DCE 前固定准入 |
| `compile/analysis/effects.cpp` | 读取 Inst::GetEffects 已有的直接 helper 细化，不重复解析 traits。不观察 guest state 且无 fault/reentry 的 helper 不制造 flags 读取；可能观察、fault/reentry、未知与间接 helper 仍保守。可能写入不作为必然覆盖，memory/call 副作用保持，不能借 flags 优化删除调用 |
| `arm64/jit/translator_alu_scalar.cpp`、`translator_select.cpp` | 每个 CondSet/直接 Select 只求一次条件 mask；各时点的 PendingNZCVCovers 仍分别查询当前动态状态，不缓存合并前后的覆盖结果 |
| `tests/ir_contract_test.cpp`、`helper_effect_codegen_test.cpp` | 既有 CSE 用例占住不用的 value 存储，继续验证 Block 的四种 operand view 与 Function 的 typed Lambda/重复 Params、真实 HIR uses；新增六种 helper 观察契约的 Block/Function 反向 liveness 回归。原生 resident helper 用例扩展为 PF/AF/NZCV 及 pending partial clear，与不调用 helper 的结果比较 |

无候选路径不会 reserve、填充或排序 value_rows/value_lookup，也不播种队列或建立 value_links/value_uses。上述借用测试在同一 workspace 的 value 存储不可再借用时仍完成真实 CSE 与 use 更新，覆盖生产入口的必要边界；直接构造 ValueWorklist 的显式身份/编辑查询契约没有改成缺失 roster 的静默查询。CSE 前的资格扫描仍是独立 IR 访问，有候选路径可能多访问一个前缀；本批没有接入 decode presence，也没有据此宣称所有候选访问、整个编译的初始化/分配或栈成本为零。增长次数、初始化流量、保留容量及峰值计量仍需继续交付。

helper liveness 新例分别验证精确无观察、可能 fault、可能 reentry、只读 guest state、间接目标和默认未知六种情形；只有无观察情形的旧 flags writer 被后续完整写覆盖后删除，六种调用本身都因 memory/call 副作用保留。原生 helper 用例继续独立验证实际物理保存，不用公共语义查询替代目标 clobber/ABI 契约。

v1/v2 的初始 IR 检查分别发现新夹具误用了“仅支持直接地址的 traits 构造器”及“CallLambda 固定 Value 槽中传入空 Params”。后一项在快模式未报错，由 SVM_IR_FAST=0 检查路径正确拒绝。v3 改为合法的间接构造器及省略可选 CallLambda 参数，保留全部优化期望；两个失败候选未跑后续完整矩阵，不计为通过。三个版本的生产代码一致，v2/v3 只改 IR 测试，未通过修改生产校验绕过失败。

源码摘要 `84ae66f5564640f898ded6b13b04b92a3c307dc4df4a00a3468b28861cd5c5c0`，overlay SHA-256 `b4094456056e58363efe4bd0fbd48bbdcce3d81952e24e9f8f21deb45e23a6ca`。429 个 source 路径、109 个删除路径，相邻树修改 11 个已有文件，无新增/退役文件。launcher `svm_value_presence_v3` SHA-256 `07179a7239918e6c0b90ccdeb0ab4489712248b67637cf045d7930028674df23`。14 个 Android 目标及最终 Ninja 无待构建工作均退出 0。最后部署复制了经 SHA 确认相同的 12 个设备二进制，只重新上传 IR 测试；这是字节复用，不是测试结果复用。RV 后端构建通过，RV native 仍开放；code format 33、AOT schema 6 未改变。

最终执行 125 项完整 native 矩阵，全部实际终止且退出符合预期；另运行一次 FLAGS_REGS=0 的 helper-effects 组，3 cases、37 assertions、0 failure/error。IR 快模式及 SVM_IR_FAST=0 检查模式各 48 cases、6362 assertions、0 failure/error；固定寄存器仍为 2 cases、39 assertions。主 JUnit 为 721 cases、221913 assertions、11 个既有失败 case、66 个失败断言、0 error，共同用例顺序及 failure/error 签名与相邻版一致，121 项其他 native 标记一致，循环未使用 rescue interrupt。主套件成功断言总数变化未完整归因，未为凑数追加整套重复运行。

十组静态负载均退出 0，五种 Core 的 CRC 为 0x4983，五种 bench 的各五个 checksum 正确，实际模式均核对。相邻静态日志复用已完成的 v4 执行，重新核对 SHA 和完整次序；十组 pool/IR arena 相同。五种 bench 的完整大小/IR 次序及 RA shape 均一致，host bytes 保持 19896、19904、16468、20068、16492。

| Core 配置 | 有序记录数，相邻→候选 | RA host bytes，相邻→候选 | 逐记录 host bytes，相邻→候选 |
| --- | ---: | ---: | ---: |
| 默认 Function | 424→425 | 286072→286164 | 283448→283540 |
| bound decode 关闭的 Function | 424→425 | 286076→286168 | 283452→283544 |
| 实际 Block | 2416→2416 | 205116→205368 | 205116→205368 |
| 固定寄存器 Function | 424→424 | 285744→285744 | 282984→282984 |
| 固定寄存器 Block | 2419→2418 | 206836→206764 | 206836→206764 |

Core 所有共同 PC 的大小/IR 记录均相同，差异只在已知格式化函数中的新增/缺失记录，范围外完整次序相同。默认及 bound-off 多出 0x25fb00 的 92 字节记录；两个 Block 配置保留具体新增/缺失与重复出现。它们不能直接解释成同一编译体增长或算法收益，也不能从大小相同推断机器码字节全部相同。五种 Core 的 spill defs/loads/stores 均保持，target_overflow 为 0。

PMU 为每种模式各一个 `reference-2000 / candidate-2000 / candidate-12000 / reference-12000` 诊断组，参考为本轮实际运行的 v4；八点 CRC 正确，逐点前后条件均保存。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | -0.018919% / +0.121216% | +0.455386% / -0.429236% | +0.399506% / -0.511943% |
| 实际 Block | -0.037144% / +0.266187% | -0.895853% / +3.746344% | -0.695758% / +2.538849% |

governor 为 walt，current 快照 844800–1171200 kHz、max 为 1171200 kHz；cpuss 30.5–33.6°C、cpu1-0 31.6–34.0°C、cpu1-6 30.1–31.3°C。单组不能证明性能收益或无回退；正向指令截距也不能据此归因于编译器或资格预扫。避免了特定暂存工作不等于已证明全负载成本降低，正式多轮三方性能与完整 R1 成本门继续开放。截距含启动/运行时，未给事件运行比，不报告 IPC。

冻结源码、14 个二进制、构建/部署、125 项矩阵、一项 packed helper 组、十组静态完整记录与八点 PMU/条件、源码/输入完整性证据在 /private/tmp/swiftvm-middle-results，以 r1-value-presence-v3 为前缀。17 项设备 SHA 在全部测试前后相同，13 个部署二进制与主机冻结字节一致，源码未偏离冻结快照。值/CSE 和宽度 matcher 正文、原候选谓词及 RV scalar-flags admission 正文核对相同；必要 DCE、纯值位规则、三份物理 GPR 证明及缓存格式源文件也与相邻冻结版相同。没有新增临时 probe 或测试脚本，full_goal_complete=false，formal_three_way_performance_accepted=false。

## R1 工作区存储边界与请求计量

本批以 r1-value-presence-v3 为相邻基线，最终冻结为 r1-workspace-storage-v3。工作区从 IR 所有权文件拆出，并修正 caller 自持工作区在借用结束后仍保留超限容量的问题。存储请求计量已接入现有 SVM_PROF2；结构与已检查行为通过，完整 R1 成本和正式性能门继续开放。

| 文件 | 实际职责与变化 |
| --- | --- |
| compile/unit.{h,cpp} | 只保留 CompileUnit、HIR/实际父对象与 anchors 所有权、合并/guest ranges/封口 |
| compile/workspace.{h,cpp} | 管理九类 vector、独占借用、线程缓存与嵌套隔离。ValueQueueScope、RewriteQueueScope、ConsumerCandidateScope、InstructionRowScope 退出时清空逻辑状态，并释放 capacity 超过 64K entries 的缓冲；caller 自持工作区同样执行 |
| compile/allocate/writer.cpp | writer 退出时释放超限 spill cursor；各 attempt 仍只清空游标，保留跨 scratch/FPR 重试的容量复用。其余分配与最终检查行为保持 |
| common/perf_storage.{h,cpp}、perf_stats.h | 无状态 allocator 记录成功的请求/释放次数、请求/释放字节、当前请求字节及逻辑峰值；各类和总量分别累计，总峰值包含扩容时新旧缓冲重叠 |
| 接口与调用方 | 按实际借用直接包含 workspace，只有引用声明的 pipeline 使用前置声明。SpillCursor 只导出 span；没有在 unit.h 添加兼容包含来维持旧间接依赖 |
| tests/ir_contract_test.cpp | 扩展 caller 自持缓冲正常/异常回收与顺序跨线程交接；新增无候选冷路径请求比较、大型真实 Params use 图回收及小块复用。沿用既有目标，没有新增脚本或 probe |

容量上限以元素数计算，各缓冲元素大小不同，不能写成统一 64KB。四类 scope 在借用结束时回收，spill 等待 writer 结束，外层 Reset 继续最终清理。大图用例有 1024 次合法 CallDynamic，每次一个目标 Value 和 64 个重复 Params，共 66560 条真实 use 边。worklist 结束后超限 use 缓冲的请求存活字节回到借用前值；后续小块仍查询到正确的唯一使用者，再次复用不产生新请求。该证明不依赖线程工作区最后析构。

无候选比较使用同一份有效 IR 和两份冷工作区：显式构造既有 ValueWorklist 查询路径产生 rows/lookup 各一次请求，生产 BeginValueCleanup 两者均为零，分别覆盖 Block 与已完成 RPO/编号的 Function。这只证明特定暂存路径，不代表整个编译零分配或资格预扫免费。跨线程用例在第一个借用结束后转移唯一所有者，另一线程复用并释放；没有并发共享活动借用，也不据此认定全部 prepared 异步交接已经验收。

计量开关在首次存储使用（包括快照）时固定，需在编译前设置 SVM_PROF2，防止配置重载或跨线程释放使申请/释放落在开关两侧。默认路径没有计数器原子更新或逐指令计时，仍有分配/释放入口的调用和判断成本。编译静止时读取快照；请求字节不等于初始化流量、malloc 实际块大小或 RSS，各类峰值之和也不等于同时峰值。初始化/清零、保留容量过程、其余暂存与代表性收益仍待交付。Android DWARF 对相邻和候选均给出 CompileWorkspace=224、CompileWorkspaceScope=240、ValueWorklist=64 bytes；布局相同不能替代运行期成本测量。

v1 编译发现 pipeline.h 依赖旧间接声明，补上前置声明后 v2 的 14 个目标通过。检查新 Function 用例时补齐 RPO/编号，再冻结 v3；只有 IR 测试二进制改变，生产字节与 v2 相同。v1/v2 未执行 native/静态/PMU，不计为通过候选。最终部署复制经 SHA 确认相同的 12 个 v2 设备文件并上传新 IR 测试，全部执行均为 v3 实际运行。

源码摘要 2a92ae70c7b2735a3696ba4d2021268174c6a4b34720e08fc0080cb07913ff4a，overlay SHA-256 2e7e212eeeafa69d1f8637cf406aefd6c05aaa7c9d3ce04138a5c5f204b1335f。433 个 source 路径、109 个删除路径，相邻树新增 4 个、修改 48 个已有文件，无新增退役文件。launcher svm_workspace_storage_v3 SHA-256 105a41b4eb8151d7de3609beb7323c89ace949b7cb5bf0b3853a4f96ab56ce30。14 个目标构建与最终 Ninja 无待构建工作均退出 0；13 个部署文件匹配主机冻结字节，17 项设备输入在全部执行前后相同。RV 构建通过，RV native 仍开放；code format 33、AOT schema 6 不变。

125 项完整 native 矩阵实际终止并符合预期退出。IR 快模式 50 cases、6382 assertions；SVM_IR_FAST=0 且 SVM_PROF2=1 为 50 cases、6401 assertions，均无 failure/error。多出的 19 个断言验证实际请求/释放与冷路径计量；检查套件计 105 次请求、105 次释放，请求/释放各 6352436 bytes，逻辑峰值 3981724 bytes，输出时 live=0。固定寄存器为 2 cases、39 assertions。主 JUnit 为 721 cases、222257 assertions、11 个既有失败 case、66 个失败断言、0 error；共同用例顺序与失败签名保持，121 项 native 标记相同，循环未使用 rescue interrupt。成功断言总数变化仍未完整归因，未为凑数重复整套 XML 审计。

十组静态负载均退出 0，五种 Core CRC 均为 0x4983，五种 bench 的各五个 checksum 正确，实际模式核对通过。相邻静态日志复用已完成的 v3 执行并重核 SHA 与完整次序；候选额外开启 SVM_PROF2，因此不比较两组阶段时间。五组 bench 的完整大小/IR 次序与 RA shape 相同，host bytes 保持 19896、19904、16468、20068、16492。默认与固定寄存器 Function Core 的大小记录/RA shape 也相同；其余 Core 所有共同 PC 大小/IR 相同，仅在格式化函数范围缺少记录，范围外完整次序保持，不能解释为算法缩减或机器码字节完全相同。

| Core 配置 | 有序记录，相邻→候选 | RA host bytes，相邻→候选 |
| --- | ---: | ---: |
| 默认 Function | 425→425 | 286164→286164 |
| bound-off Function | 425→424 | 286168→286076 |
| 实际 Block | 2416→2413 | 205368→204828 |
| 固定寄存器 Function | 424→424 | 285744→285744 |
| 固定寄存器 Block | 2418→2415 | 206764→206476 |

下表是候选进程的实际容器请求，包含首次申请及增长，不包含其余 IR/facts/actions 分配。九类缓冲均配对释放，输出时 live=0；固定寄存器配置的对应总量与默认配置相同。退出时释放完整不能替代 scope 内保留容量的过程记录。

| 负载 | 请求次数 | 累计请求 bytes | 逻辑总峰值 bytes |
| --- | ---: | ---: | ---: |
| Function Core | 61 | 265376 | 194964 |
| Function bench | 33 | 66908 | 62032 |
| bound-off Function Core | 61 | 266584 | 195860 |
| bound-off Function bench | 33 | 67868 | 62992 |
| Block Core | 52 | 140620 | 93748 |
| Block bench | 46 | 72964 | 38864 |

PMU 每种模式各一个 reference-2000 / candidate-2000 / candidate-12000 / reference-12000 诊断组，实际参考为相邻版；关闭 profiling、清空 SVM_JIT_CACHE、绑 CPU4，八点 CRC 正确，前后条件均保存。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | +0.005476% / -0.112511% | +0.863266% / -2.269684% | +0.503123% / -1.839156% |
| 实际 Block | +0.030058% / -0.503876% | +1.509554% / -0.745630% | +2.084072% / -2.516015% |

governor 为 walt，current/max 快照均为 1171200 kHz，仍不能证明锁频；cpuss 31.3–34.0°C、cpu1-0 31.6–33.6°C、cpu1-6 30.5–32.0°C。正向 cycles/wall 变化未归因，单组不建立收益或无回退。截距含启动/运行时，不等同纯编译；缺少事件运行比，不报告 IPC。

证据位于 /private/tmp/swiftvm-middle-results，以 r1-workspace-storage-v3 为前缀，包括源码/14 个二进制、JUnit、完整形状、九类存储、DWARF 与八点 PMU。19 项结构/已检查行为门通过；40 个调用方仅变更包含，pipeline 仅另加前置声明，unit.cpp 的 IR 主体、writer 其余行为、纯规则、worklist/effects、三份 GPR 证明及缓存格式核对保持。没有新增临时 probe 或测试脚本；full_goal_complete=false，full_r1_cost_accepted=false，formal_three_way_performance_accepted=false。

## R6 发射前块摘要与审计收敛

本批相邻基线为 r1-workspace-storage-v3，最终冻结 r6-emission-summaries-v3。三处准备扫描移出 emitter，末条主体定位改为按实际编码状态从尾部查找；相关接口与夹具同步改成显式摘要输入。它完成本组准备边界清理，不代表全部 R1/R5/R6 或正式性能验收。

| 文件 | 实际变化 |
| --- | --- |
| compile/analysis/block_summary.h | 在既有八字节摘要的 presence 位中记录 PushRSB，与基础事实同次收集 |
| arm64/compile/actions.h、allocation_preparation.{h,cpp}、lower.cpp | DeclareBlock 必须显式接收既有只读 BlockSummary，按值保存 return-address-push 标记。十二字节 BlockActions 不增长，不隐式准备 facts/图，不保留摘要借用 |
| arm64/compile/edges.{h,cpp}、lower_edges.cpp | 三十二字节 EdgeBlockAction 的原空位保存首个严格尾段 AdvancePC 编号和可选 gap guest 指令数。复用已有 raw targets、排序 membership 与目标 helper 谓词，按需单次扫描；ForBlock 保留 inactive 重复 location 的实际身份和 active 首个 canonical 身份 |
| arm64/compile/admission.{h,cpp} | 快照加入已有 flags_regs_audit 特征，准备/发射的诊断需求漂移会被拒绝；必要准入不受可选审计控制 |
| arm64/jit/translator.{h,cpp}、translator_region.cpp | 删除 IsStrictInternalAdvancePC 及仅为它存在的观察包装。移除 PushRSB、gap AdvancePC 全块计数；发射只读取冻结标记。末条主体从尾部按实际 coverage/动态 suppression 查询，并跳过 CallReturn 元数据 |
| 既有 fixture 与测试 | 所有 DeclareBlock 调用方显式提供真实摘要，保留多块/稀疏 spans 与析构后查询覆盖；扩展 presence 正反例，并增加 edge-audit 的十一种输入、region/audit 各开关组合 |

严格尾段保持原始 terminal 判据：收集结果恰好两项（重复目标仍计两项）且均为 internal；CheckHalt、Switch 和 ExternalLinkBlock 继续使用原 visitor 语义。准备期遇到 TargetMayFaultOrObserve 就重置起点，得到最后观察点之后的首个 AdvancePC。发射按该唯一编号在实际 IR 顺序中切换状态，不能使用编号 >= 阈值；即使首个 marker 被覆盖，也先更新尾段状态再跳过编码。实际 dirty/requested NZCV 仍在发射时判断，不把静态资格写成运行时 flags 状态。

新例包含 memory、未知 helper、精确无 guest-state/fault/reentry 的 helper、可能 fault helper，以及重复 internal、external、三目标、单目标、嵌套 CheckHalt。Void ID 被反向交换但仍唯一，用于拒绝“编号单调”的错误假设。gap 指令数保留原来对所有 IR AdvancePC 的计数，不按覆盖过滤；关闭相关审计不新增 IR 扫描。末条主体查询依赖 encoder 的动态 suppression，仍属于编码状态查询，未创建新的持久 plan。

v1/v2 编译拒绝了新夹具错误的 helper 枚举与私有编辑调用；v3 使用既有 HelperGuestStateEffect::None 和 RewriteContext，生产源码三个版本相同。前两版未部署或执行测试，未通过放宽生产访问边界修复夹具。

源码摘要 8f2c995b6baa7e96b2a31d54f8c658225c9054fa41a254daefe7d50c842212f5，overlay SHA-256 a499e4c296752b509dba0094bd6dc2a34de157fa2fd9e2304869fe4e24a17fee。433 个 source 路径、109 个删除路径，相邻修改 20 个已有文件，无新增/退役文件。launcher svm_emission_summaries_v3 SHA-256 19b89459400db7e767dddd9fde5d71342f52153c8315470ee7070d03067fd0ca。14 个目标及最终 Ninja 无待构建工作退出 0，13 个部署文件匹配主机冻结字节；17 项输入在全部执行前和补充 packed 运行后的最终快照一致，较早的中间快照单独保留。RV 构建通过，RV native 仍开放；code format 33、AOT schema 6 保持。

125 项 native 矩阵均实际终止并符合预期退出。主 JUnit 为 722 cases、221904 assertions、11 个既有失败 case、66 个失败断言、0 error；新增一例通过，共同顺序/失败签名一致，121 项 native 标记一致，循环未用 rescue interrupt。IR 快模式为 50 cases、6384 assertions；SVM_IR_FAST=0 且 SVM_PROF2=1 为 50 cases、6403 assertions，均无 failure/error。固定寄存器为 2 cases、39 assertions。成功断言总数变化未完整归因，未为凑数重复全套审计。

另运行正常与 SVM_FLAGS_REGS=0 的定向诊断组，各 3 cases、249 assertions、0 failure/error，同时开启 density/hot-coalesce-all/flags-regs-audit。它们覆盖新的摘要例和既有循环、原子开关诊断前后机器码相同的断言；不由跨进程大小相同推断字节相同。DWARF 对相邻和候选均给出 BlockSummary=8、BlockActions=12、EdgeBlockAction=32 bytes；未据此认定整个编译无额外成本。

十组普通静态负载均退出 0、CRC/checksum 正确，实际模式核对。相邻日志复用已完成执行并重核 SHA 与完整次序，十组 pool/IR arena 相同。五组 bench 的完整大小/IR 次序和 RA shape 保持，host bytes 仍为 19896、19904、16468、20068、16492。默认 Function Core 也完整保持；其余 Core 所有共同 PC 的大小/IR 相同，差异仅为格式化函数范围内新增/缺失记录，范围外完整次序保持，不能归因为算法增长或缩减。

| Core 配置 | 有序记录，相邻→候选 | RA host bytes，相邻→候选 |
| --- | ---: | ---: |
| 默认 Function | 425→425 | 286164→286164 |
| bound-off Function | 424→425 | 286076→286168 |
| 实际 Block | 2413→2417 | 204828→205348 |
| 固定寄存器 Function | 424→425 | 285744→285836 |
| 固定寄存器 Block | 2415→2416 | 206476→206456 |

又实际运行两版正常/packed 的完整 Core 审计，共四点，CRC 均为 0x4983。两种模式均为 4572→4571 条 gap 记录，仅参考版多出 formatter 的 0x25fb00（两个 guest 指令）；共同记录以及格式化范围外完整次序相同。全局 audit 聚合总数随这条覆盖变化，不能声称全量输出相同。默认 flags 寄存器路径没有 AdvancePC 合并行；packed 记录的 RegionInternal 为 471 个 PC、523 个静态合并点，两版相同。四指令严格合并机会的 merge_saved_static 均为零，因此本轮没有覆盖该模型的非零实际机会，也不把零相等当成收益证明。

PMU 仍为每种模式一个 reference-2000 / candidate-2000 / candidate-12000 / reference-12000 诊断组，实际运行相邻参考；关闭 profiling、清空缓存、绑 CPU4，八点 CRC 正确且前后条件保存。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | +0.001220% / -0.111560% | +0.629388% / -0.822390% | +1.182328% / -1.052202% |
| 实际 Block | -0.020098% / +0.028420% | +2.809807% / -4.196985% | +1.610545% / -0.210363% |

walt governor；current/max 快照均 1171200 kHz，不能证明连续锁频；cpuss 31.6–34.4°C、cpu1-0 32.0–34.4°C、cpu1-6 30.9–32.0°C。正向 cycles/wall 变化未归因，单组不建立收益或无回退；截距非纯编译，缺少事件运行比，不报告 IPC。

证据位于 /private/tmp/swiftvm-middle-results，以 r6-emission-summaries-v3 为前缀，包含完整源码/二进制、125 项矩阵、两个定向组、四点完整审计、十组静态次序和八点 PMU。21 项结构/已检查行为门通过，纯规则/worklist/DCE/effects、三份 GPR 证明、workspace/存储计量和缓存格式源文件与相邻版保持。没有新增临时 probe 或测试脚本；full_goal_complete=false，batch_performance_accepted=false，formal_three_way_performance_accepted=false。

## R1 工作区闲置容量与分配借用

本批接续 r6-emission-summaries-v3，最终冻结 r1-workspace-retention-v2。它补齐九类既有 workspace 的闲置容量过程与三个显式批量初始化点，并拒绝不同分配输出意外重叠借用同一工作区；完整 R1 成本和异步 prepared 交接仍开放。

| 文件 | 实际职责与变化 |
| --- | --- |
| compile/workspace.{h,cpp} | 分配期独占守卫；四类 scope、spill 游标和 Reset 的保留/重新借用/析构配对；沿用逐容器 64K entries 上限 |
| compile/allocate/writer.cpp | 在污染结果/facts 标记前取得工作区借用，析构时释放；冻结结果继续独立存活 |
| common/perf_storage.{h,cpp}、perf_stats.h | 既有 SVM_PROF2 下增加 retention_events、retained_bytes、独立 peak_retained_bytes，以及两类显式初始化逻辑字节 |
| compile/passes/value_worklist.cpp | 在 value links 的实际 resize 初始化点记录两个 UINT32_MAX 字段，不改变容器 allocator 的构造/搬迁路径 |
| tests/ir_contract_test.cpp | 在既有用例内扩展独占、异常后复用、跨线程析构、同时闲置与实际 size 初始化断言；修正旧夹具的 writer 寿命 |

Allocation 与 SemanticFacts 不同也不能重叠使用同一 workspace；此处是同步借用守卫，不是互斥锁。分配重试仍处于同一 writer 借用内；前一 writer 析构后可以顺序复用，冻结 AllocationView 保持有效。新状态利用原有尾部 padding：候选 DWARF 仍为 CompileWorkspace=224、CompileWorkspaceScope=240、ValueWorklist=64 bytes。

保留字节指借用结束后仍由 vector 持有的实际 capacity × 元素大小，重新借用时移出闲置统计，析构时在释放成员缓冲前配对扣除。重复 Reset 不重复增加，进程峰值独立累计，不把各类峰值相加。统计快照要求编译静止；它不是 RSS 或分配器实际块大小。关闭 profiling 时，作用域保留入口直接跳过计量，不遍历九类缓冲或做计数原子更新。

初始化只覆盖 spill cursor 的零填充、instruction rows 的 UINT32_MAX 填充，以及 value links 的两个 UINT32_MAX 字段，按实际 size 计算。其余 payload 构造、扩容复制和硬件流量未计入；没有给 allocator 增加 construct hook，避免改变 STL 的 trivial relocation。现有 allocator 仍无每缓冲计量对象。

v1 首次快模式 IR 捕获一个既有夹具在冻结第一份分配后仍保留 writer，又复用同一 workspace 创建后续 writer。v2 让首个 writer 在下一次准备前退出，保留原断言并验证析构后冻结视图；生产入口原本已顺序组织。v1 只运行这一项 IR，余下 124 项及静态/PMU 未执行；v2 生产二进制与 v1 相同，仅 IR 夹具改变，复制十二个相同设备文件并重新上传 IR，随后完整执行新矩阵，未复用 v1 测试结果。

源码摘要 b445270ac38ad339ec59ec8fe9fc70047c36ef42bf540ae3ef951fdfc68f812e，overlay SHA-256 32805c05842fb0e6fe417417ea02f082f835eb8f774d51dce09fba708f2218ed。433 个 source 路径、109 个删除路径，相邻修改八个已有文件，无新增/退役源文件。launcher svm_workspace_retention_v2 SHA-256 0dbe8f919a24f02966884333aeedee1bc34adf7034920374885bc95839e46198。十四个目标与最终 Ninja 无待构建工作均退出 0；十三个部署文件匹配冻结主机字节，全部执行前后十七项输入 SHA 一致。RV 仅构建，native 仍开放；code format 33、AOT schema 6 保持。

125 项 native 均实际终止且符合预期退出。主 JUnit 为 722 cases、221998 assertions、11 个既有失败 case、66 个失败断言、0 error；共同顺序/失败签名与 121 项 native 标记保持，三个循环退出未使用 rescue interrupt。IR 快模式为 50 cases、6387 assertions；SVM_IR_FAST=0 且 SVM_PROF2=1 为 50 cases、6423 assertions，均无 failure/error。固定寄存器为 2 cases、39 assertions。主测试成功断言总量的变化仍未完整归因，未为凑数重复测试。

既有 IR 用例实际检查八个独立 workspace 同时保留容量，主线程在 barrier 之间读取进程总量，释放后先 join 再断言；另检查顺序跨线程析构、超过上限的 use edges 回收和小图复用。checked IR 最终为 113 次申请/113 次释放、6385204 请求/释放字节、live=retained=0、请求峰值 3981724、闲置峰值 49584、457 次保留事件、零初始化 80 字节、模式初始化 276084 字节。新增八个同时闲置的 4096 字节缓冲解释相邻版多出的八次申请和 32768 字节；该夹具覆盖独立工作区的并发统计，不代表 prepared 结果异步交接已验收。

十组静态负载均退出 0、CRC/checksum 正确且实际模式核对。相邻日志复用已执行的 r6 候选，重核 SHA 和完整次序。九类缓冲原有六项申请/释放/当前/峰值字段在十组中均与相邻版一致，pool/IR arena 也一致；所有进程九类缓冲最终 live/retained 为零，累计可加字段匹配总量。新指标如下，单元格按 Core / bench 顺序，单位均为字节。

| 配置 | 同时闲置峰值 | 显式零初始化 | 显式模式初始化 |
| --- | ---: | ---: | ---: |
| 默认 Function | 186032 / 61356 | 137408 / 33976 | 532364 / 34072 |
| bound-off Function | 186928 / 62316 | 137408 / 33992 | 532532 / 34088 |
| 实际 Block | 90176 / 34008 | 40288 / 16452 | 810220 / 68256 |
| 固定寄存器 Function | 186032 / 61356 | 151348 / 34628 | 531964 / 34072 |
| 固定寄存器 Block | 90176 / 34008 | 61388 / 18380 | 812820 / 68256 |

五组 bench 的完整大小/IR 次序及 RA shape 保持，host bytes 为 19896、19904、16468、20068、16492。Core 的共同 PC 大小/IR 均相同，完整覆盖存在下表差异；完整记录仍保留重复出现，没有用按 PC 去重掩盖差异。

| Core 配置 | 有序记录，相邻→候选 | RA host bytes，相邻→候选 |
| --- | ---: | ---: |
| 默认 Function | 425→424 | 286164→286072 |
| bound-off Function | 425→425 | 286168→286168 |
| 实际 Block | 2417→2416 | 205348→205280 |
| 固定寄存器 Function | 425→425 | 285836→285836 |
| 固定寄存器 Block | 2416→2418 | 206456→207080 |

差异位于 ELF 符号 __d2b_D2A、__dtoa、__vfprintf 范围，三个范围外完整次序相同。本轮 Block 新出现的 0x25cedc、0x25ceed、0x25d00a、0x25d37f 均由同一 SHA 的 guest 符号表定位到 __vfprintf；原有两符号范围不能覆盖它们，分析记录保留了该扩展依据。不能把不同覆盖下的全局大小差异解释为同一编译体增长/缩减，也不能从大小相等推断全部机器码字节相同。

关闭 profiling 的 PMU 为 Function/实际 Block 各一个 reference-2000 / candidate-2000 / candidate-12000 / reference-12000 诊断组，实际运行相邻参考，清空缓存、绑 CPU4，八点 CRC 正确，逐点前后条件保存。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | -0.009270% / +0.057844% | -0.541803% / +1.040674% | -0.343288% / +1.405022% |
| 实际 Block | -0.005947% / +0.126317% | +0.698717% / -1.410814% | +0.585955% / -0.799506% |

walt governor，current 快照 614400–1171200 kHz、max 1171200 kHz；cpuss 31.3–34.0°C、cpu1-0 31.3–33.2°C、cpu1-6 30.1–31.6°C。单组正负波动不建立收益或无回退，快照不证明连续锁频；截距包含启动/运行时，未提供事件运行比，不报告 IPC。此处计量和布局验证不替代完整 R1 成本及正式多轮三方性能门。

证据以 r1-workspace-retention-v2 为前缀保存在 /private/tmp/swiftvm-middle-results，包含源码、二进制、构建/部署、125 项矩阵、十一份存储闭合记录、十组静态完整次序、guest 符号依据及八点 PMU/条件。纯值规则、DCE/effects、三份 GPR 证明、lower_edges 和缓存格式源文件与相邻版保持。没有新增临时 probe 或测试脚本；full_goal_complete=false、full_r1_cost_accepted=false、formal_three_way_performance_accepted=false。

## R1/R3 声明 home 与 FPR 候选扫描收敛

本批接续 r1-workspace-retention-v2，最终采用 r3-input-scan-fusion-v3。整单元声明 home 不再单独遍历，FPR 的两次前向筛选合为一次局部投影；两种模式的活跃性、必要 fixed/scratch 检查与独立 FPR retry 保持。

| 文件 | 实际变化 |
| --- | --- |
| compile/analysis/facts.cpp、semantic_facts.h | 随 flags/presence 基础访问累计 GetHostGPR/SetHostGPR home 掩码。Block 取本块，Function 只取 RPO 可执行成员；原始窄视图仍声明 home，输入重置清零，const 查询要求摘要已准备 |
| arm64/compile/inputs.cpp | 从共享事实取得掩码形成两种池，删除整单元 homes 扫描；ARM64 对 home 31 的拒绝仍在目标层 |
| arm64/compile/fpr_inputs.{h,cpp} | 一个紧凑前向循环同时收集 destructive tie 与 publication。反向 observer 写窗口、实际 publication 的 value ends、两种 scalar insert merge 及 AES 语义/观察窗口仍独立按需完成 |
| compile/workspace.h | 收紧无状态 allocator 的注释，明确其不增加每缓冲 allocator/header 状态；闲置容量仍使用工作区内已记录的状态位 |
| tests/ir_contract_test.cpp | 扩展既有基础事实用例，检查不同宽度 home、断开 Function 成员不扩大掩码、先摘要后活跃性及新输入清除旧声明；保留原有 FPR observer、scalar insert 和物理分配断言 |

新掩码利用 SemanticFacts 既有 padding，不增加独立对象。相邻与最终候选 DWARF 均为 SemanticFacts=624、ValueLiveness=320、CompileWorkspace=224 bytes。它是最终 IR 的声明摘要，不是物理 allocation mapping；不能以 decode 期间尚未完成失效处理的掩码替代。

v1 构建通过后复核，把最初放入 ValueLiveness 的掩码移到 SemanticFacts；v1 未部署或执行。v2 将 FPR 流式接入通用逐指令遍历，十四目标、125 项新 native、两个 FPR 组、十组静态和八点 PMU 均终止并完成核对，但十二种孤立编译中位数全部增加约 0.026%～11.967%，原因未证明。复核发现新增逐指令跨文件调用可直接消除，因此 v3 撤掉该临时 builder，保留一个 FPR 内部紧凑投影。此版本选择不构成对 v2 时间差的完整归因，也不保留 builder 兼容入口。

最终源码摘要 6513b6213a7c6a58664414fc98ec5073625176aaf18a96e1be7636797547728a，overlay SHA-256 e82588171dad70abac1cd3c0aa29ed61e2368f596689f81ca9ac3e38f43bc33e。433 个 source 路径、109 个删除路径，相邻修改七个已有文件，无新增/退役文件；相对 v2 仅修改 FPR 投影及其调用的三个文件。launcher svm_input_scan_fusion_v3 SHA-256 d27de5d558cf8246cdccb0983e956792a44bc57f3d16001a87bf66120ac1517e。十四个目标及最终 Ninja 无待构建工作均退出 0，十三个文件重新部署匹配；候选、相邻 launcher/主测试、simpleperf 和两个 guest 共十八项 SHA 在全部执行前后保持。RV 仅构建；code format 33、AOT schema 6 未变。

遵守集中验证的范围，v3 实际重跑四套相关测试；v2 完整 125 项的 121 个 native 标记和循环无 rescue 记录按旧版证据单独保留，没有计作 v3 新执行。v3 主 JUnit 为 722 cases、221818 assertions、11 个既有失败 case、66 个失败断言、0 error；IR 快模式为 50 cases、6394 assertions，SVM_IR_FAST=0 且 SVM_PROF2=1 为 50 cases、6430 assertions，均无 failure/error。固定寄存器为 2 cases、39 assertions。四套共同 case 顺序/失败签名与相邻版一致。成功断言总量变化仍未完整归因，未为凑数重复整套测试。

另使用已有隐藏 FPR 编译基准作 reference / candidate / candidate / reference 四点对照，逐点采集 PMU 与前后条件。每个进程为 1 case、828 assertions、无 failure/error；三种 SIMD/scalar/AES 形状各取长度 2、8、32、128，每种编译 35 次，计时舍去前五次再取三十次中位数。四个进程的十二条完整代码 hash、大小与 spill/reload/store 记录相同；每个进程总共编译 420 次。

| FPR 四点诊断指标 | 参考两进程均值 | 候选两进程均值 | 变化 |
| --- | ---: | ---: | ---: |
| instructions | 912450779 | 912028004.5 | -0.046334% |
| cpu-cycles | 351178469 | 351383992.5 | +0.058524% |
| wall seconds | 0.351009 | 0.355619 | +1.313357% |

每种形状两次计时中位数的均值变化范围为 -8.266484%～+3.138236%，该均值是诊断口径，不是正式多轮中位数。PMU 总量含启动、夹具和运行时工作，不能写成输入阶段的纯成本。walt governor，current 快照 940800–1171200 kHz、max 为 1171200 kHz；cpuss 31.3–33.6°C、cpu1-0 30.9–33.2°C、cpu1-6 30.5–31.3°C。只有一组 ABBA，时长和周期信号仍未完整归因，正式性能验收保持开放。

十组静态负载均退出 0、CRC/checksum 正确且实际模式已核对。相邻静态日志复用已完成的 workspace-retention-v2 执行，重核 SHA 与完整次序。十组 pool/IR arena、九类工作区原有六项请求/释放/峰值数据相同；十份生产日志及 checked IR 的九类缓冲退出均 live=retained=0，可加字段与总量匹配。五组 bench 的完整大小/IR 次序和 RA shape 均保持，host bytes 为 19896、19904、16468、20068、16492。

| Core 配置 | 有序记录，相邻→候选 | RA host bytes，相邻→候选 |
| --- | ---: | ---: |
| 默认 Function | 424→424 | 286072→286072 |
| bound-off Function | 425→424 | 286168→286076 |
| 实际 Block | 2416→2412 | 205280→204816 |
| 固定寄存器 Function | 425→424 | 285836→285744 |
| 固定寄存器 Block | 2418→2415 | 207080→206884 |

共同 PC 大小/IR 相同，增减仅位于同一 guest 符号表确认的 __d2b_D2A、__dtoa、__vfprintf 范围，范围外完整次序相同；完整记录保留重复出现。大小差不能归因为同一编译体增长/缩减，大小相等也不证明所有机器码字节相同。FPR 基准的实际 hash 对照另列，未以大小比较替代。

Core PMU 实际运行相邻参考，Function/实际 Block 各一个 reference-2000 / candidate-2000 / candidate-12000 / reference-12000 组，关闭 profiling、清空缓存、绑 CPU4，八点 CRC 正确且前后条件保存。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | -0.006471% / -0.018073% | -0.801097% / -0.214393% | -1.106547% / +0.107430% |
| 实际 Block | +0.034505% / -0.348412% | +0.616264% / -0.555896% | -0.376122% / +1.898416% |

walt governor，current/max 快照均为 1171200 kHz；cpuss 30.9–33.6°C、cpu1-0 31.3–33.2°C、cpu1-6 30.5–31.6°C。快照不证明连续锁频，单组不能建立收益或无回退；截距含启动/运行时，事件运行比缺失，不报告 IPC。

证据以 r3-input-scan-fusion-v3 为前缀保存在 /private/tmp/swiftvm-middle-results，v1/v2 的构建、完整矩阵及被替代的计量另存。二十二项结构及本轮已检查行为门通过；纯规则/DCE/effects、三份 GPR 证明、边准备、fixed 验证、compiler 编排和缓存格式与相邻版保持。没有新增临时 probe 或测试脚本；full_goal_complete=false、batch_performance_accepted=false、formal_three_way_performance_accepted=false。完整语义宽度/CFG、其余目标候选与 FPR/scratch/helper 契约、Function 非局部 SSA 和正式三方验收继续接续。

## R1 捕获寄存器原始使用摘要

本批接续 r3-input-scan-fusion-v3，冻结 r1-register-reads-v1。ARM64 初始约束不再无条件构建 VersionUseEnds 哈希表；只在出现实际尚未绑定的合法捕获候选时显式准备同一单元的语义需求缓存。物理活跃性表及必要 fixed/scratch/FPR 重试规则保持。

| 文件 | 实际职责 |
| --- | --- |
| compile/analysis/register_reads.{h,cpp} | 共享只读 RegisterReadUses：只为 GetHostGPR 声明保留最大原始使用宽度与末次位置。先播种声明，再访问全部原始参数和隐式根；八字节稀疏行、内联八项，排序查询 |
| compile/analysis/semantic_facts.h、facts.cpp | 唯一 owning 缓存与显式 Prepare/const Find 接口；同一输入复用、新输入清空，不改变 ValueLiveness 或 consumer 图的准备 |
| arm64/compile/constraints.cpp | 删除 VersionUseEnd/VersionUseEnds/CollectVersionUseEnds。DeclareCaptureVersions 在既有 skip、已有约束、home/池检查后申请缓存，保留宽度、偏移、版本和保存判据 |
| compile/CMakeLists.txt | 接入两个实际源文件，无旧收集器兼容条目 |
| tests/ir_contract_test.cpp | 在既有用例内使用独立完整哈希参考对照，覆盖 typed Lambda/Params、BitCast 原始宽度、跨块 terminal、断开块、未使用声明、稀疏/非单调 ID、重试复用与新输入丢弃 |

Block 只汇总本块；Function 汇总整个 RPO，再为各实际捕获查询，仍排除断开的成员。原始 Value 的宽度在解析 BitCast 前计算，包括 pseudo 与虚拟别名的实际参数；terminal 和提升锚点使用实际块尾位置。它不等于 Block 的物理 UseEnd，后者继续省略虚拟 BitCast 读取，不能用这次共享改动把两种含义合并。seen 位保留“没有使用”与“编号零处存在使用”的区别，查询不会构图或分配。

独立参考保留每个原始根的完整哈希需求，再选择捕获声明，与生产的只存声明、二分查询路径分离。新例检查 U16 参数、U32 Lambda 目标、跨块一字节条件及 U32 捕获被 U64 原始视图使用时的八字节需求。手工编号包含 65000、65535、0、5、900，确认不要求单调或把最高 ID 扩为稠密列。已有 GPR publication 的 U8/U16/U32/U64 BitCast 宽度与物理分配测试保持。

这不是零存储改动：DWARF 显示 SemanticFacts 从 624 到 632 bytes，用于一个 owning 指针；RegisterReadUses 对象为 88 bytes、每行 8 bytes，超过八条内联声明仍有 vector 增长。ValueLiveness=320、CompileWorkspace=224 bytes 保持。普通无实际候选路径不创建表，具体触发由现有目标策略决定；九类 workspace 指标不包含该缓存的对象和溢出存储，不能据此声称全部编译内存不增长或完整 R1 成本已经验收。

源码摘要 1e8d13d478919f7209064c08852b7067b982587646a33acd4bc46c70cb2885ec，overlay SHA-256 ad98f3601fcba4a12bc583e46e5cc4f1c937a10bb72efa379e684f436c6654f0。435 个 source 路径、109 个删除路径，相邻新增两个、修改五个已有文件，无新退役路径。launcher svm_register_reads_v1 SHA-256 39f5d24f8227ce91a2c21d546472e7fcb17eb9f1af8355d5908034228ff2aecb。十四个目标及最终 Ninja 无待构建工作退出 0；十三个重新部署文件匹配冻结主机字节，十七项输入在所有执行前后相同。RV 仅构建，native 仍开放；code format 33、AOT schema 6 未变。

125 项 native 均新执行、实际终止并符合预期退出。主 JUnit 为 722 cases、222190 assertions、11 个既有失败 case、66 个失败断言、0 error；IR 快模式为 50 cases、6461 assertions，SVM_IR_FAST=0 且 SVM_PROF2=1 为 50 cases、6497 assertions，均无 failure/error。固定寄存器为 2 cases、39 assertions。四套与相邻 v3 的共同 case 顺序/失败签名保持；相邻 v3 仅重跑四套，其余 121 个 native 标记本轮与最近完整的 input-scan-fusion-v2 矩阵比较并一致，未把旧版执行计作本轮测试。三个循环均未使用 rescue interrupt。主测试成功断言总量差异仍未完整归因，未为凑数重复测试。

十组静态负载均退出 0、CRC/checksum 正确且实际模式核对。相邻日志复用 v3 已执行候选，重新验证 SHA 和完整次序。五组 bench 完整大小/IR 次序及 RA shape 保持，host bytes 为 19896、19904、16468、20068、16492。十组 pool/IR arena 和九类工作区原有六项字段相同；checked IR 及十份生产日志的 live/retained 均归零、累计字段与总量一致。checked IR 为 113 次申请/释放、6385204 请求/释放字节、逻辑峰值 3981724、闲置峰值 49584，显式初始化量保持；这些只覆盖工作区，不覆盖本批新 owning 表。

| Core 配置 | 有序记录，相邻→候选 | RA host bytes，相邻→候选 |
| --- | ---: | ---: |
| 默认 Function | 424→425 | 286072→286164 |
| bound-off Function | 424→425 | 286076→286168 |
| 实际 Block | 2412→2417 | 204816→205208 |
| 固定寄存器 Function | 424→424 | 285744→285744 |
| 固定寄存器 Block | 2415→2412 | 206884→206156 |

共同 PC 的大小/IR 相同，增减仅位于已由相同 guest 符号表确认的 __d2b_D2A、__dtoa、__vfprintf 范围，范围外完整次序保持；记录保留重复出现。不同覆盖下的聚合 host bytes 不能解释为同一编译体大小变化，大小相等也不证明全部机器码字节相同。

关闭 profiling 的 PMU 为 Function/实际 Block 各一个 reference-2000 / candidate-2000 / candidate-12000 / reference-12000 组，参考实际运行相邻 v3，清空缓存、绑 CPU4，八点 CRC 正确，逐点前后条件保存。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | -0.006237% / -0.684146% | -1.118703% / +1.368174% | -0.853452% / +0.844521% |
| 实际 Block | -0.005395% / -0.512028% | +1.923682% / -3.060273% | +1.277652% / -0.903170% |

walt governor，current 快照 614400–1171200 kHz、max 1171200 kHz；cpuss 31.6–33.6°C、cpu1-0 31.6–33.6°C、cpu1-6 30.9–32.0°C。快照不证明连续锁频，截距含启动/运行时；缺少事件运行比，不报告 IPC。单组指令截距下降不能证明整体收益，Block 正向 cycles/wall 差异继续待归因，正式多轮性能门保持开放。

证据以 r1-register-reads-v1 为前缀保存在 /private/tmp/swiftvm-middle-results，包含完整源码/二进制、125 项终止记录、四套 XML、完整静态次序、DWARF、存储闭合、PMU/条件和完整性记录。二十三项结构与已检查行为门通过，物理 liveness、纯规则/DCE/effects、三份 GPR 证明、fixed 验证、compiler 重试、FPR 输入、边准备、workspace 与缓存格式源文件与相邻版保持。没有新增临时 probe 或测试脚本；full_goal_complete=false、full_r1_cost_accepted=false、formal_three_way_performance_accepted=false。完整单元宽度推断、共享 CFG/其他候选、其余分配/lowering 与 Function 非局部 SSA，以及可复现 Android FEX 和正式三方对照继续接续。

## R6 Terminal 使用遍历与回边目标复用

本批接续 r1-register-reads-v1，冻结 r6-terminal-scans-v1。普通边准备共用一份 terminal 目标缓冲，fixed/width 证明共用原始 terminal 值遍历，没有新增持久 plan、目录或字段。

| 文件 | 实际变化 |
| --- | --- |
| compile/analysis/uses.h | 提取 VisitTerminalValues，按实际树次序访问 If/Switch 的原始 Value，递归 Condition/CheckHalt；VisitImplicitUses 随后追加提升锚点 |
| arm64/compile/constraints.cpp | terminal 集合改用公共 visitor，仍保存原始 Def，不混入提升锚点或 width 证明的别名规则 |
| arm64/compile/width_contracts.cpp | 删除本地 terminal 递归；公共 visitor 访问后仍由既有 ResolveWidthChainBitCast 解析，其他物理检查保持 |
| arm64/compile/lower_edges.cpp | 删除每块单独申请目标 vector 的回边准备；每单元复用一个缓冲，active 循环图/回边候选消费同一次收集，inactive 仍逐实际块收集 |
| 既有 IR/cycle 测试 | 检查嵌套 terminal 的原始 Def、类型和重复访问次序、terminal 值不含提升锚点，以及二十四种 shape/region/links 的完整回边序列 |

active 路径先使用原始目标顺序和重复计数构图、判断恰好两个内部目标的严格资格，再排序/去重生成回边候选。回边仍按 RPO 块次序逐块追加，不做跨块全局去重；重复 location 的 canonical/实际块身份保持。可选 CFG 报告仍单独收集原始目标，保持 location 次序和每块重复序列，不为普通路径增加报告缓存。HIR successor、raw terminal 边和 flags pass 的未知出口仍有不同语义，未强行合图。

公共 visitor 只统一数据 Value 访问。fixed 保留原始 Def，width 单独解析别名，implicit wrapper 另加 anchors；独立 GPR audit 保留完整参考遍历。两份证明源文件除新增 include 和 terminal 遍历片段外，归一化内容一致，记录在 unchanged-policy-check.json；共享 visitor 的行为由明确预期的嵌套用例检查。

源码摘要 ddd5081fbe51ca4d0d2ee12fb594576e169bc04c09d037a00921d7e731c111b7，overlay SHA-256 6ada8fedba87efc7e9f8da63c72b5c952f0a435ddaa4b890a234108b192c4875。435 个 source 路径、109 个删除路径，相邻修改六个已有文件，无新增/退役路径。launcher svm_terminal_scans_v1 SHA-256 b616ac679ff53f658f497f7cc77e1fa9507a67dc28e613c80a7c3df9111fa627。十四目标和最终 Ninja 无待构建工作退出 0，十三个设备文件匹配冻结主机字节，全部执行前后十七项 SHA 相同。RV 只构建；code format 33、AOT schema 6 不变。相邻/候选 DWARF 均为 BlockSummary=8、BlockActions=12、EdgeBlockAction=32、CompileWorkspace=224 bytes。

本批新执行四套相关测试及二十六个 retirement cycle/static/dynamic 模式，共三十项，全部终止并符合预期退出；上一批完整 125 项单独保留，没有计作本批新执行。主 JUnit 为 722 cases、221996 assertions、11 个既有失败 case、66 个失败断言、0 error；IR 快模式为 50 cases、6470 assertions，SVM_IR_FAST=0 且 SVM_PROF2=1 为 50 cases、6506 assertions，均无 failure/error。固定寄存器为 2 cases、39 assertions。共同顺序/失败签名和二十六个 native 标记保持，三个循环未使用 rescue interrupt。成功断言总量变化未完整归因，未为凑数重复测试。

正常与 SVM_FLAGS_REGS=0 的诊断各 3 cases、249 assertions、无 failure/error，均启用 density/hot-coalesce-all/flags-regs-audit。主测试实际输出的二十四条 cycle 机器码与相邻版完整顺序、全部字节相同，只证明该二十四组。新 CheckHalt/Condition/Switch/If 例保留同一条件的重复访问及 selector 的 U16 原始视图；terminal 集合不包含提升 anchor，隐式使用继续包含它。

十组静态负载均退出 0、CRC/checksum 正确且实际模式核对。相邻日志复用已执行的 register-reads-v1 候选并重核 SHA/完整次序。五组 bench 完整大小/IR 次序和 RA shape 保持，host bytes 为 19896、19904、16468、20068、16492。十组 pool/IR arena、九类 workspace 原有六项计量保持；checked IR 及十份生产日志的 live/retained 均归零、累计字段闭合。checked IR 仍为 113 次申请/释放、6385204 请求/释放字节、逻辑峰值 3981724、闲置峰值 49584。这不度量普通局部 target vector 的全部成本。

| Core 配置 | 有序记录，相邻→候选 | RA host bytes，相邻→候选 |
| --- | ---: | ---: |
| 默认 Function | 425→424 | 286164→286072 |
| bound-off Function | 425→425 | 286168→286168 |
| 实际 Block | 2417→2412 | 205208→204816 |
| 固定寄存器 Function | 424→424 | 285744→285744 |
| 固定寄存器 Block | 2412→2414 | 206156→206464 |

共同 PC 的大小/IR 相同，增减仅位于同一 guest 符号表确认的 __d2b_D2A、__dtoa、__vfprintf，范围外完整次序保持，重复记录未丢弃。不同覆盖下的聚合大小不能归因于算法，大小相等也不证明其他机器码字节全部相同。

关闭 profiling 的 Core PMU 实际运行相邻参考，Function/实际 Block 各一组 reference-2000 / candidate-2000 / candidate-12000 / reference-12000；清空缓存、绑 CPU4，八点 CRC 正确且逐点前后条件保存。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | +0.143768% / -0.218671% | -1.283909% / +2.359943% | -3.407782% / +5.367128% |
| 实际 Block | +0.003674% / -0.134702% | -1.764519% / +1.434845% | -1.222886% / -1.211404% |

walt governor，current 快照 614400–1171200 kHz、max 1171200 kHz；cpuss 31.3–34.0°C、cpu1-0 31.6–34.0°C、cpu1-6 30.5–32.0°C。单组正负变化不证明收益或无回退，快照不证明连续锁频，截距含启动/运行时；缺少事件运行比，不报告 IPC。减少普通目标重收集没有被预记为完整编译成本收益。

证据以 r6-terminal-scans-v1 为前缀保存在 /private/tmp/swiftvm-middle-results，二十四项结构与本轮已检查行为门通过。物理 liveness、纯规则/DCE/effects、其他 GPR 证明、独立 audit、两阶段 fixed 检查、compiler/FPR retry、workspace 和缓存格式与相邻源码保持。没有新增临时 probe 或测试脚本；full_goal_complete=false、batch_performance_accepted=false、formal_three_way_performance_accepted=false。其余共享 CFG、pinned/条件融合、guest-state/Function 非局部 SSA 和正式三方验收继续接续。

## R7 Function 根入口查询与持久化边界

本批相邻版为 `r6-terminal-scans-v1`，交付版为 `r7-exact-entry-lookup-v2`。生产修改仅在 `module.cpp` 的 `GetJitCache(Location)`；覆盖扩展在现有 `entry_persistence_test.cpp` 的 Native 路径，没有新增源文件、probe 或测试脚本。

Module 节点表按起始地址精确查询，Function 的次级块入口由 AddressSpace 的 L2 表提供。此前关于“范围内任意地址会回退到函数首入口”的判断有误，v1 的新增断言也错误要求 Module 直接返回没有独立节点的次级入口。v1 仅运行了首项 `entry-native`，该断言失败后停止；其余矩阵、静态运行和 PMU 均未执行，不计入通过结果。

实际修复针对 ARM64 Function 根节点：其 JitCache 代表整段代码分配，起点可能是要求已有帧的调用续接序言。按 location 查询现在读取已发布的 canonical 槽，槽缺失时返回缺失，不能从分配起点重新得到入口。按 JitCache 查询整段分配的接口、独立 Block 回退及 RISC-V 子块查询保持。本批正常生成的 canonical 偏移为 4、continuation 偏移为 0，覆盖了两者实际不同的情况。

现有 Native 测试补入十三个检查点：根 canonical 查询、实际次级入口发表、未发表指令内部地址、保留所有者时清除/恢复两个 dispatch 槽、独立 Block 查询，以及源 AddressSpace 销毁后 AOT 重装的入口与清除行为。恢复后的 CodeLifetime 仍为 Live，清除槽也不会重新返回整段分配；重新发表后实际执行结果保持。次级入口通过 AddressSpace 检查，不再给 Module 施加错误的范围查询约定。

源码清单 435 项、既有删除 109 项；相邻版只修改上述两项，未新增或退役文件。源码摘要 `6bf3dbc4cce283e43ab0177da71b723880db2a9f183ae7bc4cbcb413488c60f6`，overlay 归档 SHA-256 为 `e40fd4cea7686e1fcc35b4d69ad57970045d528018139dbf542b33df9546fecd`。十四个 Android 目标构建完成，最终 Ninja 无待构建项；十三个 ARM64 运行产物部署至红魔 NX729J `bf1359ff`，RISC-V 专属目标仅构建。launcher SHA-256 为 `0b32e31ab5fa34ecf8870a6d121d2eb75dfea64e4c7cd289ca4ad757d7b87464`。候选十三项、相邻 launcher、simpleperf 与两个 guest，共十七项在执行前后均匹配冻结哈希；全部源码在本批结束时仍与冻结清单一致。

v2 集中执行一次完整的 125 项矩阵，均按预期终止。主套件 722 用例、222200 断言，仍为既有 11 个失败用例、66 个失败断言、0 error；相邻版本的用例身份、顺序及失败签名保持，断言总数差异尚未逐项定位。IR 两种模式为 50/6470 与 50/6506，fixed 为 2/39，均通过。121 项原生结果标记中，120 项与 `r1-register-reads-v1` 的完整矩阵记录一致；entry-native 因新增覆盖从 28 增至 41，不能称全部原始标记相同。三个循环退出测试 rescue_interrupt 均为 0。

静态候选十点实际运行；参考使用相邻十点缓存日志，校验 SHA 与保留重复项的完整顺序，不记作重跑。五组基准完整代码大小、IR 记录与分配 shape 保持，RA host bytes 分别为 19896、19904、16468、20068、16492，全部答案正确。Core 的公共 PC 大小/IR 保持，名单差异仅在冻结 guest ELF 的 `__d2b_D2A`、`__dtoa`、`__vfprintf` 范围；这些范围之外的完整次序保持，未声称整个 Core shape 或全部机器码逐字节相同。

| Core 模式 | 记录数，相邻→候选 | RA host bytes，相邻→候选 |
| --- | ---: | ---: |
| 默认 Function | 424→424 | 286072→286072 |
| bound-decode 关闭 Function | 425→424 | 286168→286076 |
| 实际 Block | 2412→2419 | 204816→205488 |
| fixed-class Function | 424→425 | 285744→285836 |
| fixed-class Block | 2414→2415 | 206464→206536 |

关闭 profiling 的 PMU 按 Function/实际 Block 分开，各执行一组 reference-2000 / candidate-2000 / candidate-12000 / reference-12000，八点 CRC 正确，逐点前后条件保存。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | -0.000176% / -0.025254% | -0.405000% / -0.057886% | -0.292247% / -0.447363% |
| 实际 Block | -0.092849% / +0.770276% | +0.838777% / +0.645498% | +1.918205% / -3.917741% |

walt governor，current/max 快照均为 1171200 kHz；cpuss 33.2–35.6°C、cpu1-0 33.6–35.2°C、cpu1-6 32.4–34.0°C。快照不证明连续锁频；单组数据不确认收益或无回退，截距含启动与运行时，缺少事件运行比，不报告 IPC。

证据以 `r7-exact-entry-lookup-v2` 为前缀保存在 `/private/tmp/swiftvm-middle-results`，十八项结构与已检查行为门通过。其余分配、优化、emitter、workspace、必要证明与缓存格式源文件均与相邻版一致。`full_goal_complete=false`、`batch_performance_accepted=false`、`nonlocal_ssa_entry_and_resume_protocol_accepted=false`。本批只修正入口查询边界，Function 非局部 SSA 穿过 dispatcher、外部入口和中断的保存/重建协议仍待完成；共享 CFG、其余候选与可复现 Android FEX 的正式三方验收继续接续。

## R1 Function CFG 结构共享与连续宽度行

本批交付 `r1-shared-function-cfg-v2`，相邻版为 `r7-exact-entry-lookup-v2`。新增 `compile/analysis/cfg.h` 的只读 FunctionCFG，由 SemanticFacts 唯一拥有；`cfg.cpp` 显式准备它，const Find 只返回已建数据，新输入清除缓存。结构保存现有可执行 RPO、HIR 前驱/出边及入口属性，保留次序、重复边和非 RPO 邻居的 MissingBlock 标记；不建立第二套 DFS，不根据 terminal 补造 HIR 边。

CFG use-end 不再建立自己的块列表/哈希编号，固定点直接读取共享索引。ARM64 GuestStateMap 的 Function 分析显式借用同一个 SemanticFacts，入口/出口宽度改用按 RPO 索引的连续行，删除这部分根集合与宽度哈希表。活跃性仍忽略非 RPO 出边，宽度传播遇到非 RPO 前驱仍降为未知；函数入口、外部 root、无入边与 call-return 的播种规则保持。宽度转移、helper clobber、partial write、候选触发及普通值收集/活跃性递推经源码范围比较保持。terminal 派生的循环图与独立 audit 没有改为使用这份 HIR 投影，其余 guest-state 与目标物理视图也未合并。

扩展现有 IR/pinned 用例验证：需要前不建图、两个宽度视图与活跃性复用同一结构、不可达块排除、新输入清除、独立 Block 不建 Function 图、非 RPO 前驱阻断零扩展/符号扩展结论、重复回边保留及原有交汇/外部入口结果。现有 fixture 的 tail 直接设置 terminal，没有对应 HIR 出边；测试明确保留这一区别。v1 构建后在上机前据此校准预期，v2 只改这一测试文件，生产源码与 v1 一致；v1 没有设备执行，不记作失败或通过的一轮验收。

本批共 436 项源码、既有删除 109 项，相邻版新增一个头文件、修改十一项，没有新增 probe 或测试脚本。源码摘要 `cdb128445d5d694168fc8928897de6525adac1d05f8ac8c0ca83d94a92207ffc`，overlay SHA-256 为 `bbd1acf78f8bbc8c0ce8baea6f3ad70ebc109dda0e80aec117ecf8a1b489517b`。十四目标构建、最终 Ninja 无待构建项，十三个 ARM64 产物部署；launcher SHA-256 为 `348885235d2e44ad754920e6d9063edb6596585b1958c1ba04b164b79cab1aa7`。红魔 `bf1359ff` 上执行前后的十七项输入均匹配冻结哈希，结束时源码保持冻结版本。

集中执行 26 项相关原生检查：四套 XML、十八项 HIR/准备寿命、entry-native 与三个循环退出。均按预期终止，22 项文本标记与相邻版保持，三个 rescue_interrupt 为 0。主套件 722 用例、222233 断言，既有 11 个失败用例、66 个失败断言、0 error；用例身份、顺序与失败签名保持，未将总断言数变化都归因于新增检查。IR 两模式为 50/6483 与 50/6519，各新增十三个断言，fixed 为 2/39，均通过。本批没有重跑完整 125 项；相邻批的完整矩阵作为已存证据保留。

DWARF 实测如下，不以单个对象缩小代替总体成本验收。

| 对象 | 相邻版 | 本批 |
| --- | ---: | ---: |
| SemanticFacts | 632 | 640 |
| GuestStateMap | 1544 | 1536 |
| FunctionCFG | 无 | 72 |
| FunctionCFG::BlockInfo | 无 | 32 |
| ValueLiveness | 320 | 320 |
| CompileWorkspace | 224 | 224 |

FunctionCFG 的三个动态数组与构造期临时 HIR 索引不包含在 72 字节内，也未进入九类 workspace 计数。静态十点的九类缓冲原有六项分配/释放/峰值字段保持；三个 Core 日志的完整 retention/初始化行并非全部相同，其余七点完整行相同。IR 的完整十一字段保持：113 次申请/释放、6385204 字节、峰值 3981724、闲置峰值 49584、zero/pattern 初始化 80/276084。十一份日志均在退出时 live/retained 为 0，加总关系成立。这些结果不覆盖新增 CFG backing 的完整动态成本。

静态候选十点新运行，参考为经 SHA 和完整次序校验的相邻缓存日志。五组基准的代码大小、IR 与分配 shape 保持，RA host bytes 仍为 19896、19904、16468、20068、16492，全部答案正确。Core 公共 PC 大小/IR 保持，差异在同一冻结 ELF 的三个已识别格式化符号范围内，范围之外的完整次序保持，不声称整个 Core 的 shape/机器码一致。

| Core 模式 | 记录数，相邻→本批 | RA host bytes，相邻→本批 |
| --- | ---: | ---: |
| 默认 Function | 424→425 | 286072→286164 |
| bound-decode 关闭 Function | 424→424 | 286076→286076 |
| 实际 Block | 2419→2414 | 205488→205292 |
| fixed-class Function | 425→424 | 285836→285744 |
| fixed-class Block | 2415→2415 | 206536→206536 |

关闭 profiling 的八点 PMU 仍按 Function/实际 Block 各一组相邻参考包围候选的两点斜率/截距计算，CRC 与逐点前后条件完整。

| 模式 | instructions 斜率/截距变化 | cycles 斜率/截距变化 | wall 斜率/截距变化 |
| --- | ---: | ---: | ---: |
| Function | +0.008578% / -0.123341% | +1.220748% / -3.541211% | +2.541555% / -6.130228% |
| 实际 Block | -0.048520% / +0.391123% | -1.011446% / +4.031624% | -0.064478% / +3.621633% |

walt，current/max 快照均为 1171200 kHz；cpuss 32.0–35.2°C、cpu1-0 32.8–34.0°C、cpu1-6 31.3–32.8°C。单组变化未归因为共享 CFG 的收益/回退；快照不证明锁频，截距不是纯编译时间，缺少事件运行比，不报告 IPC。

证据以 `r1-shared-function-cfg-v2` 为前缀保存在 `/private/tmp/swiftvm-middle-results`，二十二项结构与已检查行为门通过。分配核心、纯规则/pass、必要证明、emitter、compiler 重试、入口发布与缓存格式源文件保持。`full_goal_complete=false`、`full_r1_cost_accepted=false`、`batch_performance_accepted=false`。完整宽度/guest-state、其他 CFG 视图、其余候选/暂存、Function 非局部 SSA 入口/恢复和固定源码 Android FEX 三方验收继续接续。

## R7 Module 范围查询清场

本批 `r7-dead-range-api-v1` 的相邻版为 `r1-shared-function-cfg-v2`。全仓引用检查确认 Module 的 `GetNodes/GetRangeNodes` 没有调用；两者只服务于同一个已闲置的 AddressHashMap 范围扫描。删除这三个方法、AddressNodeRefs/Nodes 两个专属别名、只被范围扫描读取的 addr_end 字段及无用 optional include，不留转发包装。范围大小仍由构造参数计算，精确 Get/Put/Remove、桶边界检查、引用释放和失效路径保持；Function 的既有 Overlap 代码没有在本批修改。

只有 module.h、module.cpp、address_hash_map.h 三个既有文件变化，未增加测试或 probe。源码对照在明确删去上述死代码后，其余方法实现与相邻版一致；这不是全部机器码逐字节一致的声明。源码清单 437 项、既有删除 109 项，摘要 `d5d0cb913d1f36d44aa2e4500b386e6aca9ae7fe2c9bb271a620d3deeb6f9c50`，overlay SHA-256 为 `573a3e6d7ca6cf630337fd6feea36cee939190aa7ef6f0e51dbe6567e04784a6`。清单新增路径 module.h 原先与起点相同；本批没有新建源文件。

十四目标构建完成、最终 Ninja 无待构建项；只部署本批所需的 entry/shutdown 两个原生程序和留存的 launcher，三个设备文件在八项检查前后均与冻结哈希匹配。launcher SHA-256 为 `323e1f5026567766ec1dfa38a7d256692b75d19944fe8c605789a9e4fde4209c`。其余十一个产物在主机冻结并与实际构建输出核对，未记作上机运行。

红魔上运行六项入口持久化/恢复检查和 owners/resource-cycles 两项关闭寿命检查，八项均通过，结果标记与具名完整参考 `r7-exact-entry-lookup-v2` 相同：41、26、26、826、12、32、20、85 个检查点。该参考不是相邻版的全新重跑。本批没有重跑 XML 主套件、静态基准或 PMU，也没有新增测试脚本。

DWARF 显示 AddressHashMap 从 80 减至 72 字节、Module 从 944 减至 936 字节。Module 内嵌该表，这两个八字节变化是同一处，不能相加；未据此推导总体内存或执行性能收益。

证据以 `r7-dead-range-api-v1` 为前缀保存在 `/private/tmp/swiftvm-middle-results`，十二项结构与已检查行为门通过。分配、优化、共享 CFG、必要证明、入口发表和 emitter 源码保持。`full_goal_complete=false`、`batch_performance_accepted=false`。Function 非局部 SSA 的 dispatcher/外部入口/中断恢复协议、其他分析与候选清理、固定源码 Android FEX 及正式三方质量门仍未完成。

## R7 SSA 必要准入与 CFG 引用闭合

本批 `r7-ssa-admission-v2` 的相邻源/PMU 版为 `r7-dead-range-api-v1`。必要检查从可选 audit 中独立出来：定义必须属于输入、编号有效且唯一、同块使用晚于定义，跨块定义必须支配使用。原始 Value 在 BitCast 来源折叠之前接受检查，覆盖嵌套参数、terminal 和提升锚点，不依赖 HIR use-list 已登记；保留此前未登记 HIR 使用的保守活跃性路径。VOID 指令编号无需单调，但仍须唯一。ARM64 本来不支持的 Phi/local IR 没有因此开放；RISC-V 的 Phi 边规则未套用此检查。

封口输入在已有基础 facts 访问中同时记录 SSA 检查，结果归 SemanticFacts，并跨物理 retry 复用；普通 Block 不创建 FunctionCFG。只有实际非局部值使用才借用共享 CFG，按定义块去重使用对，通过避开定义块的入口可达性检查支配，不保留二次方支配矩阵。declared entry、外部入口及没有入边且不是已知 call-return 的组件为根；合成入口未进入 RPO 时，其后继仍是 Function 入口。逻辑 call-return 目标单独投影，参与该支配查询，但本批没有把它加进活跃性固定点，也没有证明调用 ABI 已保存普通 SSA 驻留。

共享 FunctionCFGClosed 以完整成员身份排序查找，再读取已归属的邻居；同时比较原始边与缓存 predecessor/successor 行的顺序和重复项。捕获 UnitCertificate 的 RPO/CFG 快照前先检查闭合性，修正原先先读外部邻居 order_id 再确认归属的顺序。既有 region 契约用不可读页充当不透明的外部邻居，确认拒绝时不会读该页。封口 SSA 结果写入必要证书，缺少结果时 owning Prepare 或独立发射准入拒绝；非法手工输入不会继续执行目标分析。可选 audit 仍是独立参考检查。

新增 `analysis/ssa.h/.cpp`，修改十个生产文件与两个既有测试文件；未添加 probe 或测试脚本。源码清单 439 项、既有删除 109 项，摘要 `5cf458f629fdd8438acd5465271509081d7d057daf5e513ab99370e1cb57bad0`，overlay SHA-256 为 `e4152f9d7d8cd30ff76227e0586addb493ed5c2f26e4d07e75ff5fcffa73fa41`。v1 因新增夹具的 RewriteContext 构造参数与 Catch 断言语法编译失败，没有部署或执行；v2 只修正夹具，两版生产代码相同。

十四目标构建完成、最终 Ninja 无待构建项；冻结十四产物，部署十三个 ARM 原生产物。launcher SHA-256 为 `f5517e6ccee387620107fed9ed7ef0fc77f9a27c038c82227ad7bfef22ff196d`。候选十三产物、相邻 launcher、simpleperf 与两个 guest 共十七项在执行前后哈希一致。RISC-V 目标仅构建，未记作 native 运行。

红魔上 125 项 fresh native 均符合预期，121 项结果标记与具名完整参考 `r7-exact-entry-lookup-v2` 一致，三个环退出的 rescue_interrupt 均为零。XML 比较使用最近四套件参考 `r1-shared-function-cfg-v2`：主套件 722 case、222211 assertions，保留既有 11 个失败 case、66 个失败断言、零 error；共享 case 的失败签名与顺序一致，不声称动态断言总数不变。IR/慢模式为 53 case、6567/6603 assertions，固定契约 2 case/39 assertions，均通过。新增三 case 覆盖分支汇合/循环/外部根/terminal，及关闭可选 audit 时必要检查仍拒绝外部或前向定义、合法 Function 实际生成机器码；没有把新增覆盖记为旧 case 数相同。

十份 fresh 静态日志与缓存的 `r1-shared-function-cfg-v2` 比较，参考日志 SHA 与完整有序、保留重复的记录均复核。相邻删除 API 版没有重跑静态日志，因此不称本组是相邻版 fresh 静态对跑。五组 bench 的完整代码大小/IR/RA shape 一致：19896、19904、16468、20068、16492 bytes。CoreMark 共有 PC 的 IR/大小一致，有序名单变化只在已识别的 __d2b_D2A、__dtoa、__vfprintf 范围，范围外顺序一致；整次 CoreMark 的代码与分配 shape 仍有差异：

| CoreMark 模式 | 单元数，参考 → 候选 | RA host bytes，参考 → 候选 |
| --- | ---: | ---: |
| Function | 425 → 424 | 286164 → 286072 |
| bound-decode off Function | 424 → 425 | 286076 → 286168 |
| 实际 Block | 2414 → 2416 | 205292 → 205404 |
| fixed-class Function | 424 → 425 | 285744 → 285836 |
| fixed-class Block | 2415 → 2417 | 206536 → 206548 |

DWARF 比较相邻冻结产物：SemanticFacts 640、ValueLiveness 320、CompileWorkspace 224、FunctionCFG 72、BlockInfo 32、GuestStateMap 1536、UnitCertificate 312 bytes 均保持。新增结果位与 return_block 使用原 padding。十份静态日志的九类工作区旧六字段均相同；完整十一字段在五份 CoreMark 格式化工作量差异日志不同，五份 bench 相同。IR 慢模式完整计数相同：113 次申请/释放、6385204 bytes、逻辑峰值 3981724、闲置容量峰值 49584、零初始化 80、模式初始化 276084；十一份日志均归零闭合且总量可加。SSA 的位图、非局部使用对与可达性暂存不在现有九类计量内，固定布局相同不代表本批全成本为零。

PMU 每模式仅一组 reference-2k / candidate-2k / candidate-12k / reference-12k，reference 为相邻冻结 launcher。CRC 均正确。相对斜率/截距为：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | -0.005340% / +0.343478% | +0.425500% / -0.629311% | +1.642888% / -0.902645% |
| 实际 Block | -0.003014% / +0.223742% | -2.592827% / +4.034772% | -1.083887% / -2.335616% |

walt，current/max 快照为 1171200 kHz；cpuss 31.3–34.0°C、cpu1-0 31.3–33.2°C、cpu1-6 30.5–31.6°C。单组结果不作收益结论，快照不证明锁频，截距不等于纯编译时间，缺少事件运行比，不报告 IPC。

证据以 `r7-ssa-admission-v2` 为前缀保存在 `/private/tmp/swiftvm-middle-results`，二十三项结构与已检查行为门通过。该必要证明不处理块内 Goto/BindLabel 路径支配，也不解决普通 SSA 穿过 dispatcher、导出 canonical 入口、调用和中断恢复时的值可用性；不会用全局拒绝有效 Function 或关闭 Function 分配绕过这些工作。完整 R1 成本、R7 activation/恢复和固定源码 Android FEX 三方验收仍未完成，性能与完整目标质量门保持未通过。

## R1/R7 call-return 活跃性与独立审计

本批 `r1-call-return-flow-v1` 的相邻版为 `r7-ssa-admission-v2`。此前 ComputeRPO 与 SSA 支配已处理独立的 GetCallReturnBlock 关系，活跃性固定点却只合并普通出边。返回块在调用源之前时，原始 use-end 可能小于调用尾，导致 caller 的物理 live mask 提前释放值。现在固定点另行合并共享 CFG 的 return_block live-in，不把返回关系补成普通边，不改变原始出边顺序或重复项。

可选 audit 从原始 HIR 独立建立返回前驱；合成入口未列入 RPO 时，其直接后继仍作为入口根。审计继续使用自己的支配固定点与原始使用边，不消费生产 SSA 检查的支配结果。逻辑调用/返回可达性与 guest-call ABI 保存仍是两件事；本批只修复前者在活跃性/审计中的缺口。

新增的一个既有 IR 测试构造两个 call-return 源，共用一个没有普通前驱的返回块，并让第二个调用源位于返回块之后。它验证基础 use-end 确实不足、共享图保持普通出边为空、返回关系精确、CFG use-end 延长到调用尾；打开可选 audit 的实际 Function 准备能通过，最终 caller live mask 保留该 GPR。这个合成用例不执行 guest callee，不作为实际调用保存协议完成的证据。

仅修改 analysis/cfg.cpp、arm64/compile/audit.cpp、ir_contract_test.cpp 三个既有文件，没有新增源文件、probe 或测试脚本。源码清单 439 项、既有删除 109 项，摘要 `8bea4457379167e75c3815046e6c223d51a5b56db47f79a14b1c4a34e4be5197`，overlay SHA-256 为 `522bc458e80e1544f408a879e6bd25bb8e9c9845dfcfab4df9ab3c2624206a89`。十四目标构建完成，最终 Ninja 无待构建项；十四产物在主机冻结，实际只部署本组需要的六个原生程序，六项设备哈希在执行前后与冻结输入一致。launcher 仅在主机留存，SHA-256 为 `957c1b0293056a517dfb4b56835cfdc87c883393bba82a656fe5030879c4bfb3`，未记作本批设备执行。

为避免反复跑刚完成的全矩阵，本批仅 fresh 执行四套 XML、六项调用持久化、四项静态/动态 call 退出与三个环退出，共十七项，均符合预期。十三项结果标记与相邻完整批一致，三个 rescue_interrupt 均为零。主套件 722 case、221837 assertions，仍为既有 11 个失败 case、66 个失败断言、零 error；共享 case 的失败签名和顺序一致。IR/慢模式 54 case、6586/6622 assertions，固定契约 2 case/39 assertions，均通过。慢模式九类工作区完整计数与相邻版相同且归零闭合；这只覆盖既有计量范围。

本批没有重跑完整 125 项、静态 shape 或 PMU，不沿用上批采样作为本批 fresh 性能证据。十三项结构与已检查行为门通过，证据以 `r1-call-return-flow-v1` 为前缀保存在 `/private/tmp/swiftvm-middle-results`。guest-call 实际驻留保存、dispatcher/外部入口/中断恢复的 activation 协议、块内 label CFG 和完整性能验收仍未完成。

## R7 Function 入口状态隔离与 canonical 重建

本批 `r7-entry-state-v6` 的相邻源码与 PMU 参考为 `r1-call-return-flow-v1`。此前必要 SSA 支配只证明定义能够沿逻辑控制流到达使用，尚不能保证 dispatcher、调用返回或中断恢复拥有旧单元中的物理值。现在显式区分可独立进入的 body 与依赖单元状态的内部 body，保留 Function 分配，不全局拒绝合法非局部 SSA。

FunctionCFG 继续只保存结构。按需 FunctionValueFlow 独立持有 CFG use-end 和入口值依赖；SSA 检查将跨块使用按定义块去重分组，复用 generation/worklist，先验证支配，再沿普通前驱和独立 call-return 源反向标记 relay，在定义块停止。无需第二次完整 IR 扫描，不建二次方支配矩阵。普通 Block 和没有对应需求的路径不创建该 payload。目标 Finalize 将该位图与已经计算的入口宽度合并为 requires_unit_state，并只为普通原始边冻结状态传递 PC 对；逻辑 call-return 不视作 ABI 保存。入口宽度存在即保守标记，尚未根据最终优化选择收紧。

这些普通边必须保留单元内状态传递，region_edges 与 BlockLink 关闭时仍在同一单元跳转，循环退出观察也包含这些边。可选 audit 不参与正确性路由。内部 body 不发布 L2、call/link 执行指针，guest 范围保留为 MetadataOnly，代码 fault 范围与全部 guest 依赖仍保留。外部进入或退出后恢复走已有 CodeMiss，前端按当前 canonical guest state 重新 decode 当前 PC。重新编译不重跑原根，也不承诺恢复任意捕获 SSA。

静态或间接 guest call 的返回 PC 依赖单元状态时，禁用指向旧 body 的 continuation，只发布单个 external frame，host continuation 为空；callee 返回后 canonical dispatch，按需独立编译。PublishExternalFrame 的新重载接收实际 guest-return 寄存器，不要求其落在 x14，不覆盖动态目标。重新加载使用既有 spill 临时寄存器预算，未新增随意保留的 scratch。

真实 disk/AOT round trip 暴露了另一处既有接缝：ARM64 磁盘 loader 将粗粒度 guest-code 范围也按单指令 recovery site 验证，导致保存的 Function 被拒。新增 GuestRange recovery kind，集中 ValidSerialFaultSite 的边界、对齐与种类检查；粗范围和精确 GuestFault 分别验证，IndirectCallMiss/ExternalContinuation 始终只允许一条四字节指令。原生 Module 的既有粗记录保持 GuestFault，序列化粗范围才使用 GuestRange；signal handler 对其走已有一般恢复分支。缓存 CodeFormat 从 33 升至 34，AOT schema 仍为 6，沿用 MetadataOnly/DependencyOnly wire 格式。AOT 为未导出的独立入口安排额外编译，ARM64 也保留 guest-code 依赖。

必要 entry_state_valid 证明入口分类和传递约束已冻结，缺少时 emission 拒绝；它不代表任意故障和调用保存协议已经完成。新增两个既有 IR fixture，覆盖：

- region 开/关 × BlockLink 开/关下的 relay 传递、内部入口不导出、外部 CodeMiss 后真实独立 Block 编译、环退出与 canonical 恢复。向前边允许已有延迟观察；用实际环验证有界退出，三个环退出用例均无需 rescue。
- 静态与间接真实 callee 执行、内部返回 body 不导出、第一次返回 CodeMiss、external frame 平衡，以及独立返回块编译后的热调用。检查输出采用 canonical 状态，不误用旧 SSA。

既有 entry-native 现在分别验证局部常量 secondary 可独立发布、非局部常量 secondary 保持私有，并实际销毁源 AddressSpace、创建新实例从磁盘加载执行；另做 AOT 保存/加载/安装/销毁后的根执行。guest 范围 MetadataOnly 与真实 nonlocal constant 均被检查。entry-reject 增加合法 GuestRange 的 disk/AOT codec 检查，同时确认宽跨度调用 miss 仍被拒绝。

本批修改二十九个既有源文件，没有新增源文件、probe 或测试脚本。清单因补入七个原先未列出的既有文件增至 446 项，既有删除仍为 109 项；不能把清单新增项称为新增源文件。源码摘要 `965777624547b0ad44fefdef69ddb03384ac18ec003dbe9ba7c92e0b6d283008`，overlay SHA-256 为 `805c63c1ca16d5860bc2aeb8bc285444e9128647f2f9e629230f4a2f9b5f7844`。十四目标构建完成、最终 Ninja 无待构建项；十四产物冻结，十三候选部署，RV 仅构建。launcher SHA-256 为 `796c78b3ef8771b80782198beafeb3551676748063ece8eaccf3b328f434fc98`。候选、相邻参考 launcher、simpleperf 与两个 guest 共十七项设备输入，执行前、矩阵后及补充诊断后哈希均一致。

草稿失败单独保留，不将未运行矩阵计作完成：v1 因 EdgeBlockAction 变成四十字节构建失败，压缩布尔字段后保持三十二字节；v2 只构建，未部署执行。v3 只运行 IR，新增 fixture 缺少 Module::Push/PushCodeCache；v4 只运行 IR，fixture 错误要求向前边即时 poll，改为真实环并遵守已有观察契约。v5 的 IR/慢模式通过，entry-native 失败，随后一次既有缓存统计诊断定位上述 loader 拒绝；该版共四次原生进程调用，其余 122 项未运行。生产实现 v3–v5 相同；v6 才加入 GuestRange 修复。各草稿原始日志、构建、部署、执行和哈希记录分别留存。

最终版在红魔 fresh 执行完整 125 项，全部符合预期退出码。四套 XML 与相邻 call-return 版比较：主套件 722 case、222089 assertions，仍为既有 11 个失败 case、66 个失败断言、零 error；共享 case 的失败签名和顺序相同。IR 及慢模式均为 56 case，6733/6769 assertions；固定契约 2 case/39 assertions，均通过。相较最近完整批 `r7-ssa-admission-v2` 的 121 项标记，119 项相同，entry-native 的 checks 41→85、entry-reject 826→830 是明确新增覆盖；原始 all_markers_equal 保留 false，另记 as_designed，不覆盖观测值。

十份 fresh 静态候选日志使用 `r7-ssa-admission-v2` 的缓存参考，逐份验证 SHA 和完整有序、保留重复项的记录。相邻 call-return 版没有重跑静态日志，不能称其为本批静态参考。十组 CRC/checksum 和实际模式均正确；五组 bench 的完整代码大小、IR、RA shape 相同，分别为 19896、19904、16468、20068、16492 bytes。CoreMark 共有 PC 的 IR/大小相同，但本批 Function 新增了两个格式化符号范围外的根：

| CoreMark 模式 | 单元数，参考 → 候选 | RA host bytes，参考 → 候选 |
| --- | ---: | ---: |
| Function | 424 → 426 | 286072 → 286744 |
| bound-decode off Function | 425 → 426 | 286168 → 286748 |
| 实际 Block | 2416 → 2414 | 205404 → 205292 |
| fixed-class Function | 425 → 427 | 285836 → 286508 |
| fixed-class Block | 2417 → 2415 | 206548 → 206476 |

三个 Function 模式均新增 `0x22625d`、`0x22613e`，IR 分别为 169、48，host 合计 672 bytes；Block 的名单差异仍仅位于三个已识别格式化符号。冻结 guest SHA 与静态测试一致，ELF 将两个 PC 定位到 Scudo 的 populateFreeListAndPopBlocks，分别为条件 jb/je 后的落空路径，不是 syscall 后继。额外一次既有 IR dump 诊断退出零且 CRC 正确，确认两块先存在于 Function `0x225e60`，随后又各自从该 PC 独立 decode。该诊断改变了 profiling/格式化工作量，只用于结构归因，不混入静态有序名单或计时。没有动态 dispatch trace，尚不能区分每次重新进入的具体源头，或逐块确定是 raw SSA 还是保守入口宽度导致私有；672 bytes 是实际成本，不能称为格式化噪声或成本已验收。

DWARF 对比相邻冻结产物：SemanticFacts 640→624、FunctionCFG 72、BlockInfo 32、ValueLiveness 320、CompileWorkspace 224、GuestStateMap 1536、UnitCertificate 312 bytes；新增按需 FunctionValueFlow 为 64 bytes，最多六十四块的入口位图可内嵌。EdgeActions 160→184，UnitActions 824→848；后者包含同一份内嵌 EdgeActions，不能将两个二十四字节相加。owner 减少十六字节不等于净节省；Flow 堆对象、use-end/更大入口位图、状态边向量和 SSA 临时缓冲仍有成本。

十一份 workspace 日志均归零闭合且总量可加。十份静态的旧六字段与各自参考相同；完整十一字段在五份 CoreMark 工作量变化日志不同，五份 bench 相同。IR 慢模式全部十一字段与相邻 call-return 版相同：113 次申请/释放、6385204 bytes、逻辑峰值 3981724、闲置容量峰值 49584、零初始化 80、模式初始化 276084。上述 Flow/SSA/状态边的分配并不在既有九类 workspace 计量中，不据此认定完整成本相同。

PMU 相邻参考为实际冻结的 call-return launcher，每模式仅一组 reference-2k / candidate-2k / candidate-12k / reference-12k；八个点 CRC 均正确，相对斜率/截距为：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | +0.004861% / +0.391374% | -0.828767% / +1.030333% | -0.586982% / -0.472898% |
| 实际 Block | -0.000195% / +0.041086% | -2.686462% / +1.510418% | -2.154781% / +1.810541% |

walt，current/max 快照为 1171200 kHz；cpuss 31.3–34.0°C、cpu1-0 32.4–34.0°C、cpu1-6 30.5–32.0°C。单组不作收益结论，快照不证明锁频，截距不是纯编译时间；未提供事件运行比，不报告 IPC。

二十三项结构与已检查行为门通过，证据以 `r7-entry-state-v6` 为前缀保存在 `/private/tmp/swiftvm-middle-results`。当前交付是可执行的入口隔离/canonical 重建子集；块内 label 路径、完整 guest-call 保存、任意 fault 状态恢复、真实重入的完整归因与成本、RV native 及固定源码 Android FEX 正式对照均未完成。完整 R1、R7、性能和全目标门保持未通过。

## R7 前向局部分支的 SSA 必要证明

本批 `r7-local-control-v3` 接续 `r7-entry-state-v6`，补上文本顺序检查无法证明的路径：前向 Goto/NotGoto 可跳过一个定义，再从 BindLabel 后读取它；定义块即使支配后继 HIR 块，也不能保证该定义实际执行。

SSAValidation 仅在出现局部分支时创建 preparation-only LocalFlow，沿既有 raw-use 访问记录定义/使用所在片段；同一块开始时显式切换会话，terminal 与提升锚点继续按块尾使用处理。非局部使用另要求定义支配其来源块的局部出口，然后才进入已有 HIR 支配/入口依赖检查。查询按定义片段排序去重，共用可达性 worklist/generation，暂存线性，不建二次方支配矩阵，不增加第二遍参数扫描，也不为局部证明创建 FunctionCFG。封口输入复用结果，各物理 attempt 不重复验证。

BindLabel 作为控制 token 的绑定单独检查，不作为物化数据读取。未绑定、重复绑定、绑定普通值、跨块绑定和把 Goto token 当数据均拒绝；后向 label 保留原有拒绝边界，没有在缺少局部循环活跃性协议时扩大支持。两条条件路径均保守保留，没有新增常量条件或一般路径敏感推理。LocalFlow 随必要检查结束销毁，不进入持久 facts 或发射动作。

在既有 IR 测试中增加三个 case：Block/Function × Goto/NotGoto，验证前置定义、guarded 区内使用与合法嵌套路径通过，join、terminal、提升锚点、后继 HIR 使用和内部嵌套跳过定义被拒绝；同时比较可变显式重查与封口基础 facts 的结果。audit 关闭时，真实 PrepareBlock/PrepareFunction 仍拒绝非法绑定和被跳过的定义。第三个 case 真正执行两种外层分支的全部内外条件组合，检查 guarded 输出与跨 HIR 块的合法非局部值；region_edges 关闭，结果正确。

仅修改 facts.cpp、ssa.h、ssa.cpp、ir_contract_test.cpp 四个既有文件；没有新增源文件、probe 或测试脚本，缓存/发射契约未改。清单仍为 446 项、既有删除 109 项，源码摘要 `7fcf2bca89ec85e4ce22775307bb38fbf72ba94c344d07fea9748a7eb72116c4`，overlay SHA-256 为 `e79186ef1e3614d1099a062a5f345f84eb7d21bc04185fff20ad9616bc0ffc6f`。十四目标完成且最终 Ninja 无待构建项，十四产物冻结、七个候选程序部署；七候选及两个 guest 的九项哈希在执行前后与冻结输入一致。launcher SHA-256 为 `3dac9f575a6e574472bbc9186e965d9d0b97ec0a0b4dea4e317fce8e23fb987d`。RV 仅构建。

两次 fixture 修正分别留存：v1 构建失败，新增 case 把只读 HIRBlock 指针传给 owning CompileUnit，改用 EditBlock；零部署、零执行。v2 构建和七项部署完成，仅 fresh 执行 IR，首个新增 raw standalone-HIR 场景因未设置 Block 编号容量失败；补充既有 ReIdInstr 前置步骤，其他 33 项及四份静态负载未运行。v2 的九项设备哈希保持，日志留存。v1/v2/v3 三个生产文件完全相同，两个修改均只在 fixture。

最终版只集中 fresh 执行 34 项：四套 XML、十八项寿命、六项调用持久化、三项 entry disk/AOT/native、三个环退出；全部符合预期。三十项标记与相邻版相同，三个 rescue_interrupt 均为零。主套件 722 case、221937 assertions，既有 11 个失败 case、66 个失败断言、零 error；共享 case 的失败签名与顺序相同。IR/慢模式均为 59 case，6915/6951 assertions；固定契约 2 case/39 assertions，均通过。新增三个 case 在两种模式明确记为 as_designed，保留原始新增名单，不伪称覆盖名单未变。本批没有重跑完整 125 项。

四份 fresh 静态负载分别为默认 Function 和实际 Block 的 CoreMark、bench；参考是相邻批缓存日志，SHA、完整有序且保留重复项的记录均复核。四份 CRC/checksum 与实际模式正确。Function CoreMark 的完整记录、IR、代码大小与 RA shape 相同：426 单元、286744 RA host bytes。两份 bench 完整记录和 shape 相同，分别为 19896、16468 bytes。Block CoreMark 2414→2419 单元、205292→205488 RA host bytes；共有 PC 的 IR/大小不变，名单差异仅在已识别的三个格式化符号内，范围外有序记录相同。没有 fresh 运行 bound-decode off、fixed-class 静态或 PMU。

DWARF 比较：SemanticFacts 624、ValueLiveness 320、CompileWorkspace 224、FunctionCFG 72、BlockInfo 32、GuestStateMap 1536、UnitCertificate 312、FunctionValueFlow 64、UnitActions 848、EdgeActions 184 bytes 均不变；短期 SSAValidation 从 96 增至 104 bytes，新增 LocalFlow 为 32 bytes。普通直线输入不分配 LocalFlow，但检查对象仍增加八字节指针；有局部分支时的块记录、定义/使用、branch/label、目标及查询/可达性缓冲另有堆与初始化成本，尚未完整计量。

四份静态日志的九类工作区旧六字段均相同；完整十一字段只有 Block CoreMark 因工作量名单变化而不同，其他三份相同。IR 慢模式十一字段与相邻版相同：113 次申请/释放、6385204 bytes、逻辑峰值 3981724、闲置容量峰值 49584、零初始化 80、模式初始化 276084；五份日志均归零闭合且总量可加。LocalFlow 不在这九类计量内，不将这些相同计数视作新增验证成本为零。

二十一项结构与已检查行为门通过，证据前缀为 `/private/tmp/swiftvm-middle-results/r7-local-control-v3`。本批完成前向局部路径必要证明；后向 label/局部循环活跃性、完整类型/Phi 规则、任意 fault 重建、完整 guest-call 保存及正式性能验收仍未完成。前批两个 Scudo 独立根的 672 bytes 成本仍在，没有用本批 Function 等价结果消除该问题；完整 R1/R7 与全目标门继续保持未通过。

## R1 稀疏 Function 活跃性与显式工作区借用

本批 `r1-sparse-cfg-flow-v1` 接续 `r7-local-control-v3`，退役 CFG 活跃性的每块 definitions/uses/live-in/live-out 四组 unordered_set、固定点循环中的集合复制，以及最终 live-out 全集合汇总。结构图继续归 cfg.h/cpp，FunctionValueFlow 的类型和数据流实现独立到 analysis/value_flow.h/cpp；SemanticFacts 仍为唯一 owning owner，没有复制图或新增物理结果容器。

PrepareCFGUseEnds 现在显式接收调用方 CompileWorkspace。沿可执行 RPO 访问非 pseudo 参数，按旧规则解析 BitCast，收集当前 use-end 与在本块定义前出现的值需求；terminal 和提升锚点仍作为隐式根。同一 InstructionRowScope 记录编号定义所在块，后续候选选择复用该缓冲。需求按 value/block 排序去重，再向普通前驱与独立 call-return 源传播；先把值延长至前驱块尾，再在定义块停止，保留定义块自身的 live-out 以及反向逻辑返回边。世代标记/worklist 只保留线性暂存，不建立块×值矩阵。

没有上行暴露需求时，不为本次活跃性查询创建 FunctionCFG；普通 Block 直接返回空结果，缓存查询在借用工作区之前返回。输出为独立 owning 向量，不保留行缓冲 span。定义行在查询结束归还，超限回收复用既有规则。需求对、返回源和可达性缓冲仍在本次调用局部，尚未全部迁入 CompileWorkspace，也不与 SSA 的 raw-use 入口位图合并语义。

两个新增既有 IR case 使用独立的 raw-HIR 稠密固定点作为参考，比较完整 use-end 向量。覆盖八类 CFG × 有/无 BitCast 别名，共十六组，包括 diamond、自然循环、irreducible、重复边、自环、不可达定义、嵌套/重复 Params、pseudo-only 使用、提升锚点和逻辑返回；小型分析输入还覆盖局部先使用后定义，不将这些输入视为可执行 SSA。另验证本地使用的多块 Function 不建共享图、行缓冲释放后可复用、其他用户持有行时缓存查询不重借用或覆盖，以及普通 Block 不占用行。已有反向 call-return 用例继续检查物理 caller live mask，独立 audit 保持原实现。

修改七个既有文件，新增两个实际 analysis 源文件，已登记构建项；没有新增 probe 或测试脚本。源码清单 448 项、既有删除 109 项，摘要 `7da3a4a975654f5f29e23c32d99b053ae70dd3cb54ca68e4c17f1bfc44b6912b`，overlay SHA-256 为 `e06933d09941107889308fd5132b998cc3882c2d2360c111e58ec2fff84e7408`。十四目标及最终无待构建项检查一次完成，十四产物冻结、十三候选部署；launcher SHA-256 为 `b7446c9d66522bfec6d8991cce007106544a377f8aa826625b4a99b31618b11c`。候选、相邻参考 launcher、simpleperf 与两个 guest 共十七项设备输入，执行前后哈希一致。RV 仅构建，没有重跑 native。

本批通用 Function 区间构建改变，合并后集中 fresh 执行完整 125 项，全部符合预期。与相邻版四套 XML 比较：主套件 722 case、222076 assertions，仍为既有 11 个失败 case、66 个失败断言、零 error，共享 case 的失败签名和顺序一致。IR/慢模式 61 case、6970/7006 assertions，固定契约 2 case/39 assertions，均通过。两个新增 case 在两种 IR 模式明确记为 as_designed；121 项标记与最近完整批 `r7-entry-state-v6` 相同，与相邻版的三十项交集也逐项一致，三个 rescue_interrupt 均为零。

十份 fresh 静态候选使用 `r7-entry-state-v6` 的完整缓存参考，复核 SHA 与保留重复项的完整有序记录；相邻版只重跑过其中四种配置，因此不将该参考称为相邻版 fresh 对跑。十份 CRC/checksum、实际模式正确，五组 bench 的完整代码大小、IR、RA shape 相同，依次为 19896、19904、16468、20068、16492 bytes。CoreMark 的共有 PC 的 IR/大小相同，名单变化仅在已识别的三个格式化符号内，范围外有序记录一致：

| CoreMark 模式 | 单元数，参考 → 候选 | RA host bytes，参考 → 候选 |
| --- | ---: | ---: |
| Function | 426 → 427 | 286744 → 286836 |
| bound-decode off Function | 426 → 427 | 286748 → 286840 |
| 实际 Block | 2414 → 2417 | 205292 → 205348 |
| fixed-class Function | 427 → 427 | 286508 → 286508 |
| fixed-class Block | 2415 → 2412 | 206476 → 206492 |

fixed-class Function 的完整记录与 shape 相同。前批两个 Scudo 根与其 672 bytes 成本仍在，本批没有消除该成本。DWARF 比较相邻冻结产物的十项固定布局均不变：SemanticFacts 624、ValueLiveness 320、CompileWorkspace 224、FunctionCFG 72、BlockInfo 32、GuestStateMap 1536、UnitCertificate 312、FunctionValueFlow 64、UnitActions 848、EdgeActions 184 bytes。

十一份工作区日志均归零闭合且总量可加。复用的定义行现在进入既有 instruction_rows 请求与初始化统计；旧四组哈希分配及仍局部持有的需求/返回源/可达性暂存不在旧九类计量内，不能把新增观测字节当作净堆成本，也不能宣称总体成本为零。完整代码名单相同的三份 Function bench、fixed-class CoreMark 和 IR 慢模式均只有 instruction_rows 这一类计数变化；两份 Block bench 的完整十一字段相同。部分总量如下：

| 工作量 | 请求次数 | 请求 bytes | 逻辑峰值 bytes | 闲置容量峰值 bytes | 模式初始化 bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| fixed-class Function CoreMark | 61→60 | 265376→271004 | 194964→201184 | 186032→191840 | 533216→897184 |
| 默认 Function bench | 33→34 | 66908→70760 | 62032→66088 | 61356→65096 | 34072→55132 |
| 实际 Block bench | 46→46 | 72964→72964 | 38864→38864 | 34008→34008 | 68256→68256 |
| IR 慢模式，含新增覆盖 | 113→135 | 6385204→6387324 | 3981724→3981780 | 49584→49640 | 276084→278648 |

其他 CoreMark 日志含格式化工作量变化，不能将其所有计数差额都归于定义行。定义表复用避免增加 workspace 字段，但增加了全编号范围的填充和部分保留容量；进一步收紧这部分成本仍待接续。

PMU 使用相邻冻结的 `svm_local_control_v3` 为参考，每模式一组 reference-2k / candidate-2k / candidate-12k / reference-12k，八个点 CRC 均正确。相对斜率/截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | -0.113460% / -0.806027% | -0.012479% / -1.015149% | +1.725011% / -2.820343% |
| 实际 Block | +0.031940% / -0.144987% | +1.804621% / -1.709189% | +0.971893% / +1.492533% |

walt，current/max 快照 1171200 kHz；cpuss 31.6–34.4°C、cpu1-0 32.4–34.4°C、cpu1-6 30.5–32.0°C。单组不作为性能收益或无退化证明；快照不证明锁频，截距不是纯编译时间，缺少事件运行比，不报告 IPC。完整 R1 成本、正式性能/FEX 三方和全目标门保持未通过。

二十四项结构与已检查行为门通过，证据前缀为 `/private/tmp/swiftvm-middle-results/r1-sparse-cfg-flow-v1`。当前交付退役生产集合固定点并建立显式暂存借用；其余 facts/width、候选扫描、完整初始化/搬迁成本及先前 R7/R0 未完成事项仍按原目标推进。

## R1 删除 CFG 定义行表与追加工作区成本

本批 `r1-cfg-owner-storage-v1` 接续 `r1-sparse-cfg-flow-v1`，收回上一批新增的全编号定义行初始化和保留容量。PrepareCFGUseEnds 不再借用 InstructionRowScope；workspace 参数、无调用转发及相应依赖均删除，稀疏需求传播仍保留。

当前指令号与块数量域均为 u16。准备期间，既有 u32 use-end 元素的高半部保存定义块的 RPO 序号加一，低半部保存端点；零标签表示尚无定义。定义仍按实际文本顺序记录，因而保留局部先使用后定义、不可达定义和未注册 HIR 等输入的原有分析行为，不依赖额外的“合法 SSA 快路径”。更新端点不改变高半部；传播结束统一清除标签，再设 ready、返回最终向量。每次未就绪准备都先清空私有缓冲，失败后重新准备不会复用旧标签。静态类型约束和 RPO 数量检查防止编号域扩大后静默截断。

输出缓冲本来就需要分配和初始化。本批删除独立定义表及其工作区活动，但仍有输出零填充、末尾去标签扫描和局部需求/返回源/可达性暂存；没有宣称这些成本消失，也没有恢复生产哈希集合或旧固定点。

既有十六组 raw-HIR 固定点参考继续逐元素比较完整端点向量；选择行现在从首次准备前就保持借用，验证 fresh 与缓存查询均不覆盖它。慢模式额外检查 instruction_rows 的请求、模式初始化和保留事件均不因查询变化。原“释放工作区行”的 case 改名为“不触碰选择行”，覆盖范围明确记录；新增最大指令数量 case 构造 65535 条指令，检查跨块与本地端点达到 65533/65534，并与独立参考完整比较。该边界在两种 IR 模式通过。

仅修改六个既有文件，没有新增源文件、probe 或测试脚本。源码清单仍为 448 项、既有删除 109 项，摘要 `a17c172aa174c34968aa95fc7190f9c5e9f6b5a58df771e1365933c1a74ce471`，overlay SHA-256 为 `3b56ecdd98c56c0e511b1472b985575448b967bd4e9bfab050f3a0c4e3331b77`。十四目标及最终无待构建项检查一次完成，十四产物冻结、实际部署七个候选；launcher SHA-256 为 `7f426f8b7927bdb3112b330bfbba63951060bca113e165d0cbc2221844b13b59`。七候选、相邻参考、simpleperf 和两个 guest 共十一项设备输入在执行前后哈希一致。RV 仅构建。

前批刚完成通用传播算法的完整矩阵，本批集中 fresh 执行四套 XML、十八项寿命、六项调用持久化、三项 entry disk/AOT/native 和三个环退出，共三十四项，均符合预期；不重复宣称 fresh 完成 125 项。三十项标记与相邻版一致，三个 rescue_interrupt 为零。主套件 722 case、221865 assertions，仍为既有 11 个失败 case、66 个失败断言、零 error，共享 case 的失败签名和顺序相同。IR/慢模式均为 62 case，6976/7060 assertions；固定契约 2 case/39 assertions，均通过。新增边界和 case 改名各自列明，不将原始新增/退役名单覆盖成“没有覆盖变化”。

十份 fresh 静态候选使用相邻完整缓存参考，SHA 与完整有序、保留重复项的记录均复核。CRC/checksum、实际模式均正确；五组 bench 的完整记录/IR/代码大小/RA shape 一致，分别为 19896、19904、16468、20068、16492 bytes。默认 Function、bound-decode off Function 和实际 Block CoreMark 也保持完整记录与 shape：

| CoreMark 模式 | 单元数，参考 → 候选 | RA host bytes，参考 → 候选 |
| --- | ---: | ---: |
| Function | 427 → 427 | 286836 → 286836 |
| bound-decode off Function | 427 → 427 | 286840 → 286840 |
| 实际 Block | 2417 → 2417 | 205348 → 205348 |
| fixed-class Function | 427 → 426 | 286508 → 286416 |
| fixed-class Block | 2412 → 2416 | 206492 → 206456 |

两种 fixed-class CoreMark 的共有 PC 的 IR/大小相同，名单差异仅在三个已识别格式化符号内，范围外顺序一致。十项 DWARF 固定布局均与相邻版相同：SemanticFacts 624、ValueLiveness 320、CompileWorkspace 224、FunctionCFG 72、BlockInfo 32、GuestStateMap 1536、UnitCertificate 312、FunctionValueFlow 64、UnitActions 848、EdgeActions 184 bytes。

十一份工作区日志均归零闭合且总量可加。五组 bench 的九类工作区完整十一字段均恢复到引入定义表之前的 `r7-entry-state-v6` 水平；相较紧邻的定义表版本，默认/无 bound-decode Function CoreMark、三份 Function bench 和 IR 慢模式只有 instruction_rows 发生变化。部分总量如下：

| 工作量 | 请求次数 | 请求 bytes | 逻辑峰值 bytes | 闲置容量峰值 bytes | 模式初始化 bytes |
| --- | ---: | ---: | ---: | ---: | ---: |
| 默认 Function CoreMark | 60→61 | 271004→265376 | 201184→194964 | 191840→186032 | 898028→533736 |
| 默认 Function bench | 34→33 | 70760→66908 | 66088→62032 | 65096→61356 | 55132→34072 |
| 实际 Block bench | 46→46 | 72964→72964 | 38864→38864 | 34008→34008 | 68256→68256 |
| IR 慢模式，含修订覆盖 | 135→130 | 6387324→6385272 | 3981780→3981724 | 49640→49584 | 278648→276152 |

请求次数不保证单调下降；这些计数仅表示既有九类缓冲，不能作为完整 compiler 堆占用或初始化/搬迁流量。两份 fixed-class CoreMark 还存在格式化工作量差异，不把其全部计数变化归因于表删除。

PMU 每模式仅一组 reference-2k / candidate-2k / candidate-12k / reference-12k，相邻冻结版为参考，八个点 CRC 正确。相对斜率/截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | +0.005495% / -0.101838% | -0.987198% / +0.298350% | -1.809838% / +1.713517% |
| 实际 Block | +0.001500% / -0.026952% | +0.056122% / +0.233874% | +0.037268% / +0.531554% |

walt，current/max 快照 1171200 kHz；cpuss 30.5–34.0°C、cpu1-0 30.9–34.0°C、cpu1-6 30.1–31.6°C。单组不证明收益或无退化，快照不证明锁频，截距不是纯编译时间，缺少事件运行比，不报告 IPC。

二十四项结构与已检查行为门通过，证据前缀为 `/private/tmp/swiftvm-middle-results/r1-cfg-owner-storage-v1`。定义行的追加工作区成本已经清理；剩余局部传播与输出缓冲的完整成本、其他 facts/width/候选工作，以及 R7/R0 的未完成协议与正式三方验收，继续保留原范围，完整目标未完成。

## R3 物理冲突索引的显式准备与只读 lowering

本批 `r3-physical-index-inputs-v1` 接续 `r1-cfg-owner-storage-v1`。此前 preservation 与 fixed-home 的 const 冲突查询会按 dirty 状态分配、排序或重建索引；Freeze 还无条件通过两次空查询触发建表。现在只有非 const writer 的 PrepareGPRPreservations/PrepareFixedGPRHomes 执行构建，Find 只返回已经准备的结果，借出的 Locations/Ready/Conflicts 均只读。无消费者的冻结结果不再预热索引。

新增公共 `allocate/home_index.{h,cpp}`，以 Allocation 身份包装原有紧凑 FixedGPRHomeIndex。LowerIntegerBranch 只借用 AllocationView、已准备的 const home 索引及语义块索引，拒绝未就绪或其他物理结果的索引；不再接收 AllocationPreparation 或未使用的 GuestStateMap。publication/residence 的阶段内调用及初始 fixed 输入收集器也显式准备后查询，旧方法名和兼容包装均删除。memory lowering 检查 preservation 索引已准备且属于同一结果。

新请求、中止编辑、fixed 声明变化、recolor 和事务撤销的五处失效保持；其中修改 fixed 声明时，仅新增或原声明为 preserved 才使 preservation 索引失效，普通 mapping 重置不改变 fixed 声明。prefix/transfer 区间端点、两个忽略 owner 和普通映射覆写检查保持。普通 attempt 的 fixed/scratch 检查、目标编辑完成后的含 target-write 最终检查及独立 FPR retry 均保留，相关生产文件与相邻冻结版一致。

新增两个 IR case：以原始 FixedGPRDefinition 线性扫描为独立参考，比较 home 与 preservation 的边界和忽略 owner 查询，并覆盖显式准备、声明变化、事务内查询、撤销、新输入；另一 case 验证 Freeze 不建立未使用索引。分支 case 明确改名，删除原未参与算法的 foreign GuestStateMap 比较，替换为 foreign physical-index 拒绝，同时保留跨 fixture 决策一致性及 stale block analysis 拒绝。新增与改名分别记录，原始 XML 新增/退役名单未被覆盖成“无变化”。

本批修改 22 个既有源文件，仅新增上述两个生产源文件；没有 source probe 或测试脚本。源码清单 450 项、既有删除 109 项，摘要 `7858662fb9a982aa0b7ac18af1c5b2abad8e42cbb4b7bcba9ab679c3ba727d50`，overlay SHA-256 为 `e75b28db0948c84c65368d3f177c65421de2a548fc13121b6c2a3d0c88a290f6`。十四目标构建及最终无待构建项检查通过，十四产物冻结、十三个 ARM64 候选部署，RV 仅构建；launcher SHA-256 为 `8890c5fad0014caae6a6b8a204a9b30b7be23f6684b6a0c82da20fdb511b0d34`。十三候选、相邻 launcher、simpleperf 和两份 guest 共十七项输入在设备执行前后哈希一致。部署准备有一次 adb shell 引用形式导致本机沙箱无法访问 daemon，换为既有调用形式后成功；该失败未运行设备测试，没有源码失败草稿。

这一批集中 fresh 执行完整 125 项设备矩阵，退出均符合预期；121 项标记与最近完整参考 `r1-sparse-cfg-flow-v1` 一致，相邻版已有的三十项也逐项一致，三个环的 rescue_interrupt 均为零。主套件 722 case、222029 assertions，仍为既有 11 个失败 case、66 个失败断言、零 error，共享 case 的失败签名与顺序相同。IR 快/慢模式均为 64 case，8306/8390 assertions，全部通过；固定契约 2 case、39 assertions，通过。没有把既有主套件失败记为全部测试通过。

十组 fresh 静态候选与相邻缓存参考比较，复核参考 SHA 和保留重复项的完整有序记录；CRC/checksum 和实际模式均正确。五组 bench 的完整记录、IR、代码大小与 RA shape 相同，依次为 19896、19904、16468、20068、16492 bytes。CoreMark 的共有 PC 均保持 IR/大小，名单变化仅在三个已有格式化符号内，范围外有序记录相同：

| CoreMark 模式 | 单元数，参考 → 候选 | RA host bytes，参考 → 候选 |
| --- | ---: | ---: |
| Function | 427 → 427 | 286836 → 286836 |
| bound-decode off Function | 427 → 426 | 286840 → 286748 |
| 实际 Block | 2417 → 2416 | 205348 → 205200 |
| fixed-class Function | 426 → 427 | 286416 → 286508 |
| fixed-class Block | 2416 → 2415 | 206456 → 206388 |

DWARF 显示 AllocationWriter 928→936、AllocationPreparation 936→944 bytes；后者内嵌前者，只增加同一份八字节身份引用。新 GPRHomeIndex 为 40 bytes，替代原 24-byte 紧凑索引及 dirty/padding 所占的 32 bytes。GPRPreservationIndex 仍为 784、底层 FixedGPRHomeIndex 仍为 24 bytes；此前十项公共布局均不变，包括 SemanticFacts 624、CompileWorkspace 224、UnitActions 848、EdgeActions 184。未把独立索引大小与包含它的 writer 大小重复相加。

十一份工作区日志均归零闭合、总量可加。十份静态日志的六项基础字段均与相邻版相同，默认 Function CoreMark、五组 bench 和 IR 慢模式的完整十一字段相同；其他四份 CoreMark 只在 retention_events/pattern_init_bytes 随格式化名单变化。IR 慢模式仍为 130 次请求、6385272 请求 bytes、3981724 逻辑峰值、49584 闲置容量峰值、80 零初始化和 276152 模式初始化 bytes。两种物理索引的堆容量仍在九类工作区计量之外；删除无消费者预热的收益尚未完成完整分配/初始化成本归因。

PMU 每模式只执行一组 reference-2k / candidate-2k / candidate-12k / reference-12k，参考为相邻冻结 launcher，八个点 CRC 正确。相对斜率/截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | -0.002492% / -0.073911% | +0.342813% / -0.698715% | +1.887342% / -2.806577% |
| 实际 Block | +0.044880% / -0.564540% | -0.083691% / +0.356604% | +0.196018% / -0.651500% |

walt，current/max 快照均为 1171200 kHz；cpuss 31.3–33.6°C、cpu1-0 31.6–34.0°C、cpu1-6 30.1–32.0°C。单组不证明收益或无退化，快照不证明锁频，截距不等于纯编译时间，缺少事件运行比，不报告 IPC。

二十七项结构与已检查行为门通过，证据前缀 `/private/tmp/swiftvm-middle-results/r3-physical-index-inputs-v1`。这一交付完成上述索引的显式准备和 branch 的只读物理输入边界；memory 仍按保守家族触发 preservation 准备，算法内仍显式准备共享 resolved uses。其余候选驱动、索引容量成本与完整 fixed/FPR/scratch/spill/helper 契约继续施工，完整 R1/R3 和总目标均未完成。

## R1 preservation 窗口的紧凑存储

本批最终实现为 `r1-compact-preservations-v2`，相邻参照为 `r3-physical-index-inputs-v1`。准备 writer 原来内嵌三十二个 preservation vector，即使没有窗口也占有这些容器头；现在只保留一个连续缓冲，按 home 查询合并后的区间。公开准备/只读接口、物理身份、五处失效、回滚及全部调用方不变，普通 attempt 的 fixed/scratch 检查、含 target-write 的最终 fixed 检查与独立 FPR retry 也未修改。

准备按物理声明的编号顺序访问。对每个非空 preserved 前缀，只与该 home 最后一个窗口比较；编号递增保证更早的不相交窗口不会重新相交。嵌套或相接时更新最大 end，否则追加新窗口。所有 home 扫描完成后，只排序最终窗口，不再先保留全部原始前缀、排序后复制合并。每个 home 的最终 end 严格递增，查询以 home/end 找到首个 end 晚于 query begin 的窗口，再检查其 begin 不晚于 inclusive consumer end。两个端点仍为 u32，空前缀和非 preserved 声明仍被排除。

新增一个独立 raw-declaration case：三十二个 home 的声明交错排列，覆盖嵌套、相接、不相交、空前缀和普通 fixed 声明；以逐声明线性扫描计算期望，不调用合并实现。查询包括零宽、跨空隙和 UINT32_MAX 端点，共增加 1729 个断言。上一批的显式准备、事务内查询、回滚、新输入与无消费者 Freeze 用例继续保留。

首稿 `r1-compact-preservations-v1` 采用先收集再合并，十四目标构建通过，七候选部署、三十四项 native 退出均符合预期，三十项标记一致；IR 两模式均为 65 case、10035/10119 assertions，主套件仍为既有 11/66 失败。代码检查发现该方式会保留合并前容量，因此在静态/PMU 前退役。v1 没有 static 或 PMU 执行，其源码、产物、三十四份日志及四套 XML 独立保留；最终 v2 只改 collection 实现，未改已经运行的 oracle。未将 v1 或相邻完整矩阵当作 fresh v2 结果。

最终仅修改三个既有源文件：preservation_index.h、preservation_index.cpp 和 ir_contract_test.cpp，没有新增源文件、probe 或测试脚本。源码清单仍为 450 项、既有删除 109 项，摘要 `69abe34b9f5280cf1b922c884169e12cc51dadf1ff643eb8680ee7e8c5d813d9`，overlay SHA-256 为 `afd636dc0a706550bf96076656a875f606fee624cdaf170a51a4a848e65c4e57`。最终十四目标构建及无待构建项检查通过，十四产物冻结、六个候选部署，RV 仅构建；launcher SHA-256 为 `f4acd42e96cf4dcf4a31f30eeba64897b540f832bc9cbeb06d75472cd1f6d662`。最终六候选、相邻 launcher、simpleperf 和两份 guest 共十项输入在执行前后哈希一致；v1 的十一项输入也单独核对一致。

v2 集中 fresh 执行四套 XML、三项 entry disk/AOT/native、静态/间接调用和三个环退出，共十二项，均符合预期；八项实际输出标记与相邻版一致，三个 rescue_interrupt 均为零。主套件 722 case、222161 assertions，仍只有既有 11 个失败 case、66 个失败断言、零 error，共享 case 的失败签名与顺序相同。IR 两模式均为 65 case，10035/10119 assertions，全部通过；固定契约 2 case、39 assertions，通过。XML 保留新增 oracle 的原始名单，未把 coverage 差异改成“无变化”。本批没有重跑完整 125 项。

十组 fresh 静态候选与相邻缓存参考比较，复核 SHA、完整有序记录与重复项，CRC/checksum 和实际模式均正确。五组 bench 的全部记录、IR、代码大小与 RA shape 一致，依次为 19896、19904、16468、20068、16492 bytes。CoreMark 的共有 PC 的 IR/大小不变，名单变化仅在三个已有格式化符号内，范围外有序记录相同：

| CoreMark 模式 | 单元数，参考 → 候选 | RA host bytes，参考 → 候选 |
| --- | ---: | ---: |
| Function | 427 → 427 | 286836 → 286836 |
| bound-decode off Function | 426 → 427 | 286748 → 286840 |
| 实际 Block | 2416 → 2413 | 205200 → 204972 |
| fixed-class Function | 427 → 426 | 286508 → 286416 |
| fixed-class Block | 2415 → 2416 | 206388 → 206860 |

DWARF 确认 preservation 索引 784→40、AllocationWriter 936→192、AllocationPreparation 944→200 bytes；后两者包含同一个索引，只计算一次 744-byte 固定空间减少。Range 记录实测十二字节，含两个完整 u32 端点与 u16 home，相较旧 pair 的八字节增加四字节。Prepare 的三十二项 u32 末窗口表只在收集期使用，逻辑 128 bytes 且显式填充 UINT32_MAX，不保留在 writer 中；没有将它当作已测量的实际 stack frame。其余十二项公共布局不变，包括 SemanticFacts 624、CompileWorkspace 224、UnitActions 848、EdgeActions 184、GPRHomeIndex 40 和 FixedGPRHomeIndex 24。

十一份工作区日志归零闭合且总量可加。十份静态日志的六项基础字段相同；默认 Function CoreMark、五组 bench 和 IR 慢模式的完整十一字段也与相邻版相同。IR 慢模式仍为 130 次请求、6385272 请求 bytes、3981724 逻辑峰值、49584 闲置容量峰值、80 零初始化和 276152 模式初始化 bytes。物理索引的堆容量、Range 扩容搬迁和局部末窗口表初始化均未进入九类计量，不据此宣称完整堆成本或初始化量下降。

PMU 每模式只执行一组 reference-2k / candidate-2k / candidate-12k / reference-12k，八个点 CRC 正确，实际参考为相邻冻结 launcher。相对斜率/截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | -0.002957% / -0.004656% | +0.078905% / +0.778245% | +1.288088% / -1.483227% |
| 实际 Block | +0.006216% / -0.212754% | -0.447257% / +1.709862% | -0.192762% / +1.836589% |

walt，current/max 快照 1171200 kHz；cpuss 30.9–34.0°C、cpu1-0 31.6–33.6°C、cpu1-6 30.5–31.6°C。单组不证明收益或无退化，快照不证明锁频，截距不是纯编译时间；缺少事件运行比，不报告 IPC。

二十七项结构与已检查行为门通过，证据前缀 `/private/tmp/swiftvm-middle-results/r1-compact-preservations-v2`。本批收回 preparation 对象中的三十一份多余容器头，并避免保留合并前前缀数所要求的容量；记录增宽、实际 vector 容量/扩容、局部表初始化和全 compiler 峰值仍需完整归因。memory 保守触发、其余候选/工作区和完整 R1/R3、R7/R0 验收继续按原范围推进，总目标未完成。

## R3 memory 准备与只读 lowering

本批 `r3-memory-inputs-v1` 接续 `r1-compact-preservations-v2`，拆开 memory 家族的可变分析准备和只读决策。此前普通成员/Between 查询也通过统一 lambda 请求 resolved uses，preservation 在外层遇到任意 memory 块就准备；现在按实际候选分别声明需求，不在只读 matcher 内构图。

新增 `memory_preparation.{h,cpp}`，借用公共 writer 和唯一块索引，一次按块访问组织准备。非 memory 指令先内联排除，避免为每条普通 IR 进行跨文件调用。`lower_memory.{h,cpp}` 保留只读物理筛选、使用/观察/发表证明和紧凑动作生成；参数只有只读物理视图、const 语义索引及已有选择/驻留事实，没有 writer 或构图入口。单访问的 MemoryCandidates 在首个实际候选处才初始化，包含原始 Value 类型视图；没有跨访问的候选容器、第二份图或额外全块预扫。旧公共可变 LowerMemory 入口和 eager preservation 参数直接退役，现有十二处测试调用迁移到 PrepareMemory，没有兼容包装。

首个通过原三指令扫描、位置与立即数条件的 post-index 候选才准备 preservation。若其窗口冲突，则撤销该候选，保持原来不再寻找后续更新的优先级；仅剩余候选准备 resolved uses。窄加载独立请求直接 uses，所以 post-index 被拒绝后仍可继续融合。roster 成员/Between、地址别名和 RCpc offset 不为本家族构图。索引由同一 writer 生产，外部不再注入可能属于其他物理结果的 preservation 借用。

新增一个八场景 case，使用轻 roster 和未准备的索引启动；额外放入一个无关 BitCast，确保 direct-ready 与 resolved-ready 可以区分。检查实际动作、构图状态、preservation 就绪状态及重复准备的输出：

| 冷索引 fixture | 直接 uses 就绪 | resolved uses 就绪 | preservation 就绪 |
| --- | --- | --- | --- |
| 普通 U32 memory | 否 | 否 | 否 |
| GetOperand 地址别名省略 | 否 | 否 | 否 |
| RCpc offset | 否 | 否 | 否 |
| 窄加载 | 是 | 否 | 否 |
| post-index 物理映射拒绝 | 否 | 否 | 否 |
| post-index preservation 冲突 | 否 | 否 | 是 |
| post-index 通过 | 是 | 是 | 是 |
| preservation 阻止写回，窄加载继续 | 是 | 否 | 是 |

上述为本家族冷输入的契约，不表示整个生产单元的其他消费者也没有准备图。更细的 owner/consumer 初筛仍可继续收紧；没有宣称全部 R1 分析都达到最小成本。

从相邻冻结源码逐段提取并规范化比较，地址别名/offset 条件、首个三指令物理匹配、post-index 的 use/observer/alias/pinned-publication 证明和窄加载证明均保持；变化只涉及显式输入及从整块循环转成单访问返回。第五项检查确认只读 matcher 不再包含任何准备调用。这些文本比较只支持所抽取规则体，完整控制流仍由设备差分验证。普通/最终两阶段 fixed 检查、FPR retry、五处索引失效、回滚和冻结 action 布局的生产文件均未改动。

仅修改五个既有源文件并新增上述两个生产文件，没有 probe 或测试脚本。源码清单为 452 项、既有删除 109 项，摘要 `6880a72de2c5e9e59d502f179bec7285e9d876a11023b39f995d8a175516966c`，overlay SHA-256 为 `65e3e69f74334c5fe59a00bec33c1faae724ec67b9fe57c4bf8775c7e62b0594`。十四目标及最终无待构建项检查一次完成，十四产物冻结、十三个候选部署，RV 仅构建；launcher SHA-256 为 `0df316933000241fe8d6702fdad7f8a813c75f4260c362606cdadde4826a1148`。十三候选、相邻 launcher、simpleperf 与两份 guest 共十七项设备输入在执行前后哈希一致，无失败草稿或额外逐文件测试。

本批集中 fresh 执行完整 125 项，退出码均符合预期；121 项标记与最近完整参考 `r3-physical-index-inputs-v1` 一致，相邻版已覆盖的八项也逐项一致，三个环的 rescue_interrupt 均为零。主套件 723 case、222427 assertions，新增八场景 case 实际运行且通过；仍为既有 11 个失败 case、66 个失败断言、零 error，共享 case 的失败签名和顺序相同。IR 两模式仍为 65 case、10035/10119 assertions，固定契约 2 case、39 assertions，均通过。XML 中保留新增 case 的原始覆盖差异。

十份 fresh 静态候选与相邻完整缓存参考比较，复核 SHA、完整有序记录及重复项，实际模式和 CRC/checksum 正确。五组 bench 的完整记录、IR、代码大小和 RA shape 相同，依次为 19896、19904、16468、20068、16492 bytes。CoreMark 的共有 PC 保持 IR/大小，名单变化仅在三个已有格式化符号内，范围外有序记录一致：

| CoreMark 模式 | 单元数，参考 → 候选 | RA host bytes，参考 → 候选 |
| --- | ---: | ---: |
| Function | 427 → 427 | 286836 → 286836 |
| bound-decode off Function | 427 → 426 | 286840 → 286748 |
| 实际 Block | 2413 → 2418 | 204972 → 205592 |
| fixed-class Function | 426 → 426 | 286416 → 286416 |
| fixed-class Block | 2416 → 2419 | 206860 → 207184 |

十九项既有 DWARF 布局保持，包括 SemanticFacts 624、CompileWorkspace 224、AllocationWriter 192、AllocationPreparation 200、UnitActions 848、EdgeActions 184、MemoryActions 72、MemoryAction 8 和 TargetPreparation 2280 bytes。新 MemoryCandidates 为 64 bytes，PostIndex 为 24 bytes；后者包含在前者的 optional 中，不能把两者相加当作新增常驻空间。候选的外层 optional 另有状态与对齐，这些 sizeof 不代表实际 stack frame、初始化或搬迁成本；没有向 prepared 结果增加该存储。

十一份工作区日志均归零闭合且总量可加。十份静态日志的六项基础字段全部与相邻版相同；默认/fixed-class Function CoreMark、五组 bench 和 IR 慢模式的完整十一字段也相同。IR 慢模式仍为 130 次请求、6385272 请求 bytes、3981724 逻辑峰值、49584 闲置容量峰值、80 零初始化和 276152 模式初始化 bytes。semantic graph、物理索引及局部候选初始化/复制并未进入九类计量，不能用这些计数证明完整堆成本下降。

PMU 每模式仅一组 reference-2k / candidate-2k / candidate-12k / reference-12k，参考为相邻冻结 launcher，八个点 CRC 正确。相对斜率/截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | +0.002896% / -0.058250% | -1.732273% / +6.137595% | -1.765916% / +6.097912% |
| 实际 Block | +0.040249% / -0.257898% | -2.127952% / +2.369342% | -2.628796% / +5.058375% |

walt，current/max 快照为 1171200 kHz；cpuss 31.3–33.2°C、cpu1-0 31.3–34.0°C、cpu1-6 30.5–31.6°C。cycles/wall 的正向截距变化原样保留，单组无法归因或作为无退化证明；快照不证明锁频，截距包含启动/运行时，缺少事件运行比，不报告 IPC，也不宣称性能提升。

三十项结构与已检查行为门通过，证据前缀 `/private/tmp/swiftvm-middle-results/r3-memory-inputs-v1`。本批完成 memory 家族的显式候选准备与只读 lowering 边界，继续其他家族、owner 初筛、全量初始化/搬迁及完整 fixed/FPR/scratch/spill/helper 契约，完整 R1/R3 和总目标仍未完成。

## R3 vector 候选准备与只读 lowering

参考相邻已验收的 `r3-memory-inputs-v1`，本批继续可选标量 FPR 家族，将可变准备入口收拢到 `vector_preparation.{h,cpp}` 的 PrepareVector；`lower_vector.{h,cpp}` 只读 const BlockAnalysisIndex 和候选/owner span。必要 FPR 合约、SelectPshufdDirect、独立 FPR retry、普通 attempt fixed/scratch 检查及全部目标编辑后的最终 fixed 检查保持独立，相关实现与相邻冻结版哈希一致。

一次顺序投影核对块与索引的实际指针序列，筛选 low/high publication、zero-high 转换、resident read 和 extract-store 候选。低 64 位写入存在性并入既有八字节 BlockSummary 的 presence mask，删除独立 any_of 资格预扫。consumer 和 vector 在各自独占的 LoweringCandidateScope 中顺序借用同一工作区缓冲：位置和角色合为四字节 LoweringCandidate，没有候选不借用，不把临时 span 交给结果。旧 ConsumerCandidate/Scope 源码别名退役，CompileWorkspace 仍为 224 bytes、九类缓冲，未新增常驻 vector 头。

publication 先检查真实形状、使用数与 barrier/目标覆写窗口，仅实际 load 候选的 home 进入 owner 初筛；首个匹配的物理浮点 owner 才请求 resolved uses。旧三十二份 owner vector 合为单个临时稀疏缓冲，记录按 home 排序；原 Owner 与新 ScalarFPROwner 都为十二字节，新增 home 使用原有 padding。外部使用仍能独立否决融合，即使 owner 的 birth 在 publication 之后，也不能按出生窗口提前丢弃。resident 只有选中的两用转换请求直接 uses；read-store/extract-store 和单用转换不构图。转换、read-store、extract-store 的执行顺序、首个动作优先、覆盖排序及 zero-use 合并保持。两组输出同时计算，lower.cpp 仍在原 publication 与 resident 两个覆盖点分别存入 UnitActions；没有提前改变覆盖顺序。

同时修正真实的 raw Value 宽度漏洞。旧代码只要求块内存在任意一次低 64 位写入，就可能将另一处 U64 load 的 U32 视图融合成 LDR D；high-zero 的 U32 视图也不能当作完整高半清零，ZeroExtend64 的 U16 输入视图不能直接融合为 LDR S。现在每对 low/high 写入分别证明完整 64 位 lane 覆盖，Load32 另检查 extension 的原始输入为 U32。实际边类型是语义依据，不能强制便捷构造得到的 extension producer 元数据为 U64：其元数据可保留 U32，而返回的 U64 视图仍合法。未增加全局类型重写或放宽必要契约。

新增三项永久测试，位于现有测试文件：十七种冷图/匹配 home/未来外部 owner/转换使用组合；consumer→vector→consumer 的缓冲角色隔离与结果寿命；三种窄边负例及两种正常 Load64/Load32 正例的实际分配、动作、反汇编和红魔原生执行。执行检查完整 128 位 lane，确认局部写入保留原高位，另一个正常 publication 不受影响。七项隔离规则体比较保持，证据只覆盖各自的 barrier、bitcast/self-XOR/conversion、store 窗口、lookup/coverage 和 owner 否决谓词，不作为整个控制流等价证明；新增 raw-edge 宽度守卫属于有意的语义修正。

保留完整尝试记录：v1 在新增夹具的 ranges/intrusive-list 约束处编译失败，没有上机；改为 iterator 重载并消费 nodiscard 返回值。v2 完成 125 项 native，新三项测试及 121 个标记均正确，但新增的 producer 返回类型限制导致既有正常 Load32 融合测试多一个失败断言，因此没有继续静态或 PMU。v3 删除该多余限制，并让正常 native 正例覆盖便捷构造接口，随后才统一复验；没有逐文件或逐断言反复运行测试。

最终冻结 `r3-vector-inputs-v3`：454 个 source 路径、109 个删除路径，相对相邻版十四处修改和两个生产文件新增，没有 probe、源内测试脚本或旧接口包装。source digest 为 `452af16c085093bdb58fa9ccbfe6d33f8140b07379995627a8ad3a4ab3aeb044`，overlay SHA-256 为 `89d40714731e8b4f9b1a5318c084aa188d7080347f2b6cda4f1bc611402d2eb2`，launcher 为 `c0b4ee6249fd2c557eef3cda146831d9270bfe132902d390154ce0087fc06db0`。十四个 Android 目标构建通过，最终构建 no work；十四份产物冻结，十三份部署，前后十七项设备输入哈希一致。

v3 重新运行 125 项 native，退出码全部符合预期。主测试 723→726 cases，222330 次断言，仍为已知的十一例、六十六个失败断言、零 error；共享 case 的顺序和失败签名一致，新三例均运行通过。IR 快/慢各六十五例、10035/10119 断言，fixed-class 两例/39 断言通过；121 个 mode 标记与相邻版一致，三项 cycle 的 rescue_interrupt 均为零。记录的 raw coverage 字段保留新增 case 的真实差异，由单独的 declared coverage gate 核对，未将它改写成无新增。

十组静态比较均核验 guest CRC/checksum、真实 Function/Block 模式、参考日志 SHA 和保留重复项的完整记录顺序。五组 bench 的 IR、完整形状和有序记录相同，host bytes 仍为 19896、19904、16468、20068、16492。CoreMark：

| 模式 | 实际记录数：相邻→本批 | RA host bytes：相邻→本批 | 结果边界 |
| --- | ---: | ---: | --- |
| 默认 Function | 427→427 | 286836→286836 | 完整顺序与形状相同 |
| bound-off Function | 426→426 | 286748→286748 | 完整顺序与形状相同 |
| 实际 Block | 2418→2415 | 205592→205128 | 公共 PC 发码相同，记录差异仅在三个已核验 formatter |
| fixed-class Function | 426→426 | 286416→286416 | 完整顺序与形状相同 |
| fixed-class Block | 2419→2416 | 207184→206456 | 公共 PC 发码相同，记录差异仅在同三个 formatter |

这些 formatter 路径差异不归因为本批减少了 guest 主体发码。二十二种既有布局保持，包括 SemanticFacts 624、CompileWorkspace 224、UnitActions 848、AllocationWriter 192、AllocationPreparation 200 和 TargetPreparation 2280 bytes。LoweringCandidate 仍为四字节，ScalarFPRActions 为四十八字节；VectorActions 的九十六字节包含两份原有输出，不能再重复计入同一输出成本。临时 owner 从三十二份容器头收拢为一个，不据此推断实际 stack frame 或总动态占用。

工作区计数标签明确从 `consumer_candidates` 迁为 `lowering_candidates`，ordinal 和九类数量保持。比较工具显式归一化该标签，同时保存双方原始行/名称及日志哈希；真实新增 vector 借用不能被解释为纯重命名。十一份日志闭合、可加；十份静态日志的六项请求/释放/当前/峰值基础字段全部相同。共享候选的 retention events 实际增加：默认/bound-off/fixed-class Function CoreMark 各 +57，Function bench 各 +8，Block/fixed-class Block CoreMark 分别 +45/+44，Block bench 各 +7。Block formatter roster 差异另改变部分缓冲的保留事件和逻辑填充：默认/fixed-class Block 的总 pattern_init_bytes 分别为 811600→810448、813288→809412，不能将这部分归因为优化主体初始化下降。IR 慢模式归一化后的十一项字段全部相同，仍为 130 次请求、6385272 请求 bytes、3981724 逻辑峰值、49584 闲置容量峰值、80 零初始化与 276152 模式初始化 bytes。semantic graph 和临时 owner 的堆容量、追加/搬迁及栈初始化未进入九类计数，完整动态成本仍未验收。

PMU 每模式只做一组 reference-2k / candidate-2k / candidate-12k / reference-12k，参考为相邻冻结 launcher，八个点 CRC 正确。相对斜率/截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | -0.012240% / -0.026793% | -1.238771% / +3.300408% | -0.758141% / +2.029729% |
| 实际 Block | -0.020469% / -0.261068% | -1.014180% / +0.732902% | -0.884429% / +0.681950% |

walt，current 快照为 1056000–1171200 kHz、max 为 1171200 kHz；cpuss 30.9–33.6°C、cpu1-0 31.6–34.0°C、cpu1-6 30.1–32.0°C。频率并未锁定；cycles/wall 截距的增加原样保留，一组采样不构成收益或无退化证明。缺少事件运行比，不报告 IPC；截距包含启动/运行时，不能解释为纯编译时间。

三十一项结构与已检查行为门通过，证据前缀 `/private/tmp/swiftvm-middle-results/r3-vector-inputs-v3`。本批完成可选 scalar FPR 家族的候选准备/只读 lowering 边界及上述宽度修正；必要 FPR、其他 fixed/scratch/spill/helper 契约、其余候选暂存、完整动态成本和正式 FEX 三方对比继续接续。完整 R1/R3 和总重构目标仍未完成。

## R3 必要 FPR 契约的候选准备与只读复证

相邻参考为 `r3-vector-inputs-v3`。本批将必要 FPR 证明的可变入口移入 `fpr_preparation.{h,cpp}`，`fpr_contracts.{h,cpp}` 只读 AllocationView、const BlockAnalysisIndex 和准备好的候选/owner span。目标 controller 的独立 FPR contract retry 仍在 GPR 目标编辑与 actions 构造之前；普通 attempt fixed/scratch 检查、最终 target-write 验证及可选 scalar FPR 规则保持，相关源文件哈希与相邻版一致。没有将必要证明降为可选优化。

PrepareFPRContract 在一次块访问中核对实际 IR/索引指针序列，并只投影已选物理 read/write/AES/PSHUFD 及实际 scalar/destructive-shuffle tie。选中的非法绑定仍进入原复证，不会因类型或 offset 预筛而消失；没有实际证明候选时，不借用候选或状态缓冲。PSHUFD 证明只准备直接 uses，read/write/scalar/AES 以及识别的 destructive shuffle 才请求 resolved uses。SelectPshufdDirect 的原逐 use、组件完整性和 disabled/retry 规则保持；其写接口收窄为公共 AllocationWriter，不再依赖 ARM64 AllocationPreparation。

必要入口不能对着其他块的旧索引返回有效的空证明。本批在图准备和复证之前验证 roster；不匹配时返回 valid=false 和实际拒绝节点，已有 stale-index 测试相应改名并检查拒绝。重建匹配索引、清除旧选择和恢复选择继续验证。此项属于必要身份检查的修正，不把过期索引的原有空结果保留为成功契约。

owner 只投影实际证明目标 home，记录为四字节 position/home；按 home、原始块内位置排序，保留原逐 home 的 IR 遍历顺序，避免递归证明次序改变。旧三十二份 owner vector 及隐式 Owners 建表退役。判定仍包含所有实际映射到 FPR 的值，不能只按 IR 的浮点类型筛选：新增整型 IR/FPR owner 冲突用例确保不漏检。

原五类四状态缓存按 ID 范围分配五字节 State 数组，现改为首次 Cached 调用才借用已有 InstructionRowScope，以实际块位置索引，在一个 u32 的十个位中编码 Read、AesTie、AesWrite、Write、ScalarTie 状态。未见状态复用 UINT32_MAX 填充；Visiting 仍拒绝，递归更新保留其他 kind 的位。没有为稀疏 ID 间隙扩表，不创建第十类工作区或新的常驻缓存。PSHUFD-only 证明不借用递归行缓存。临时状态和候选在返回/拒绝时释放借用，结果只交接原稀疏证书。

七项隔离比较通过：五个核心复证主体、Build 的全部资格/首拒绝/前缀条件、PSHUFD 物理选择主体保持，仅替换了相同顺序的 owner 运输与候选循环来源。它们不证明候选调度、位编码或身份修正的整体等价性；两个新增永久 case 分别覆盖十九种冷图/实际绑定/非法选择/AES 组合，以及稠密/接近 u16 上界的稀疏编号下递归组件成功与循环拒绝，并在证明之间执行真正的 consumer/vector lowering 检查工作区交接。原 stale-index case 的改名及增删关系保留在 raw XML 比较中，未伪装成无覆盖变化。

v1 在新增稀疏编号夹具处编译失败：公开 IR roster 为只读，不能直接改号；改为既有 RewriteContext 的编辑入口，生产实现未变，没有为此上机或新增脚本。最终 `r3-fpr-contract-inputs-v2` 冻结 456 个 source 路径、109 个删除路径，相对相邻版六处修改、两个生产文件新增，没有 probe、源内测试脚本或旧签名包装。source digest：`bd25896672e5a4e86d3fe1be81ef9d167a9a66332f18e3fa2c902b99c67b4174`；overlay SHA-256：`eb2b9ae9c0497bcc2b9f7d2444ab68f58616dd32ea414977193bc116bbf9be3c`；launcher：`3e5484f7b7589f82c613e3c69db247471af072f7ee3d5dd6ae5473bdf85b0b8a`。

十四个 Android 目标构建通过，最终构建 no work；十四份产物冻结、十三份部署到红魔 NX729J，CPU4/taskset 10。一次集中运行 125 项 native，退出码全部符合预期；主测试 726→728 cases、222804 次断言，仍为既有十一例/六十六个失败断言、零 error。两个新增 case 和改名后的 stale case 均运行通过，共享用例顺序与失败签名保持。IR 快/慢各六十五例、10035/10119 断言，fixed-class 两例/39 断言通过；121 个 mode 标记与相邻版一致，三项 cycle 的 rescue_interrupt 为零。前后十七项设备输入哈希一致。

十组静态比较验证真实模式、guest CRC/checksum、参考日志 SHA 及包含重复项的有序记录。五组 bench 的完整 IR/形状/顺序相同，host bytes 仍为 19896、19904、16468、20068、16492。CoreMark 所有公共 PC 发码一致，非 formatter 的有序记录完全相同：

| 模式 | 实际记录数：相邻→本批 | RA host bytes：相邻→本批 |
| --- | ---: | ---: |
| 默认 Function | 427→426 | 286836→286744 |
| bound-off Function | 426→427 | 286748→286840 |
| 实际 Block | 2415→2417 | 205128→205348 |
| fixed-class Function | 426→426 | 286416→286416 |
| fixed-class Block | 2416→2419 | 206456→206836 |

记录差异全部位于已核验的三个 formatter，不归因为本批增减了 guest 主体代码。二十七种既有布局保持，包含 CompileWorkspace 224、UnitActions 848、AllocationWriter 192、AllocationPreparation 200、TargetPreparation 2280、FPRContract 40 及其四字节 Instruction。新 FPRProofOwner 为四字节；FPRContractBuilder 的 sizeof 从 848 降为 96 bytes，但准备入口另有临时 owner vector/候选 scope，不能把该对象差值当作实际 frame 或总堆栈节省。

十一份九类工作区日志均闭合、可加，buffer 标签仍为 lowering_candidates，没有口径重命名。旧私有五字节状态数组和 owner 堆容量从未计入九类统计，新 u32 行及其初始化现在进入统计；不能直接把两个口径相减得到总成本。五组 bench 各真实增加一次候选扩容请求、128 请求 bytes、64 bytes 闲置容量峰值；candidate 自身扩容同时峰值 96→192 bytes。Function/bound-off/fixed-class Function 的总请求次数分别为 33→34，实际 Block 两模式为 46→47。bench 总 pattern_init_bytes：

| 模式 | 相邻→本批 |
| --- | ---: |
| 默认 Function | 34072→42016 |
| bound-off Function | 34088→42040 |
| 实际 Block | 68256→78740 |
| fixed-class Function | 34072→42016 |
| fixed-class Block | 68256→78740 |

五份 CoreMark 的六项请求/释放/当前/峰值基础字段相同，新增保留事件和逻辑填充原样记录；其中 formatter roster 变化也会影响这些值，不能全部归为 FPR cache 的净初始化成本。IR 慢模式完整十一字段相同：130 次请求、6385272 请求 bytes、3981724 逻辑峰值、49584 闲置容量峰值、80 零初始化及 276152 模式初始化 bytes。语义图、临时 owner、容器搬迁与旧私有缓存的全口径堆成本仍需接续；本批未通过完整 R1 成本验收。

PMU 每模式只做一组 reference-2k / candidate-2k / candidate-12k / reference-12k，参考为相邻冻结 launcher，八点 CRC 正确。相对斜率/截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | +0.019205% / -0.076420% | +0.994133% / -1.563363% | +0.304417% / -0.327085% |
| 实际 Block | -0.116209% / +0.588755% | -0.676802% / -0.336378% | +0.909216% / -2.936409% |

walt，current/max 快照均为 1171200 kHz，不能据此证明锁频；cpuss 30.5–33.2°C、cpu1-0 30.9–33.2°C、cpu1-6 29.3–31.3°C。Function cycles/wall 斜率、Block instructions 截距与 wall 斜率的正向变化均保留，不将单组混合结果作为收益或无退化证明；缺少事件运行比，不报告 IPC，截距不解释为纯编译时间。

三十六项结构与已检查行为门通过，证据前缀 `/private/tmp/swiftvm-middle-results/r3-fpr-contract-inputs-v2`。质量门初稿误指不存在的 controller.cpp，已核对真实 controller.h 后更正，未改源码或重跑 native。本批收口必要 FPR 候选准备/只读复证及其临时存储，保留完整物理复证和 retry；其余 fixed/scratch/spill/helper 接缝、其他候选扫描/暂存、全量成本、共享调用方和正式 FEX 三方对照继续推进，完整 R1/R3 和总目标仍未完成。

## R3 scratch range 与精确 MEM 运输

相邻参考为 `r3-fpr-contract-inputs-v2`。本批将必要 scratch 的可变入口收拢为 `scratch_preparation.{h,cpp}` 的 PrepareScratch，原公开的可变 LowerScratch 签名退役。const lowerer 消费 AllocationView、const BlockAnalysisIndex 和工作区；实际块访问中核对完整 roster，不新增资格预扫。八字节 BlockSummary 的既有 presence mask 增加 pseudo-consumer 位，与原 GetUses(false) != GetUses() 触发条件一致；只有该条件才显式准备直接图，不请求 resolved 图。必要预算、两种池最大需求、helper/fixed clobber、leases、Div128 局部 uniform/barrier 和手动 encoder 准备规则保持。

MEM 输入不再为末次使用查询反向图。一次 ForEachValue 访问保留 Params/Lambda 的每个原始 occurrence，以真正属于本块的 Def 在既有 u32 行缓冲中计数，并在每个 consumer 结束时仅清理触及的计数。GetUses(false) 是含 pseudo/外部使用的总数；当前 consumer 的精确计数与之相等，才等价于原来的“唯一 consumer 且本地 edge count 等于总使用数”关系。重复类型视图不改写，其他本地/外部使用仍拒绝 last-use。没有本地 MEM 参数时不借用行缓冲；foreign/colliding-ID 的无效 sentinel 及冻结时的完整 key 冲突检查保留。SaveFlags(None) 仍贡献 presence/count，外块保存不进入本块 scratch 需求，不能改用单元 flags 合计。

旧逐指令 ScratchAction 退役，必要事实改为同块内连续编号、全部事实相同的二十字节 ScratchRange，first/last 复用原 padding，不扩宽记录。编号间隙和块边界不合并，不以缺失记录作为默认配方。读视图同时核对区间两端，cursor 支持正常顺序及跨 range 快路，跳过/反向访问使用二分；全单元封口检查范围不重叠。已准入的正常发射若缺少 scratch 记录直接拒绝，不能静默回落到扫描；未准入的公共手动 encoder 接口保留独立准备。MEM 运输先去重再交接，最终 key 冲突/去重规则主体不变。

两个新增永久 case 在既有 spill_forwarding_test.cpp 中覆盖十四种普通/重复 Params/外部使用/零掩码保存/branch-only/foreign ID/整型 MEM 组合，包含 301 次总引用及多个 consumer 之间的计数清理；反向图作为独立关系 oracle，手动准备作为预算/事实 oracle。另检查连续/稀疏 ID 的压缩与逐指令等价、顺序/跳转/反向 cursor、缺失 range、foreign 节点及两个相邻块的独立边界。八个预算/手动准备主体及冻结输入冲突主体的九项隔离比较通过；它们不替代新 range/count/scheduling 路径的原生与 oracle 检查。

在既有 SVM_PROF2 中加入永久的 `[svm-scratch-storage]` 九字段统计：准备块数、指令数、range/input 数、临时 vector 容量字节和，以及封口单元数、冻结 range/input 数、冻结容量字节和。只在准备/封口边界累加，未在生成代码的执行路径插桩；独立于九类 workspace 请求计量。容量是边界处 vector payload capacity 的累计和，不是 malloc 请求和、usable size、RSS、同时存活或峰值。旧参照版本没有这些 scratch 容量字段；20×指令数只是本候选准备流的逐指令记录反事实，不能当作实测参照堆占用。进程级 PerfStats2 为这些永久计数增长 72 bytes，不增加每个 prepared unit 的字段。

v1 构建、125 项 native 和十组静态检查通过，但计量发现轻度压缩时 std::vector 几何增长会使临时容量超过旧的每指令预留。未继续 PMU；v2 将 range 扩容上限约束为实际块指令数，并补永久容量断言，随后才集中复验。该限制约束结束时容量，不能据此证明扩容重叠峰值或请求次数下降。两次尝试均没有逐文件/逐断言循环测试，也没有新增临时 probe 或源内测试脚本。

最终 `r3-scratch-ranges-v2` 冻结 458 个 source 路径、109 个删除路径，相对相邻版十二处修改及两个生产文件新增。source digest 为 `1e2c5f6384506d9bc33aad0545b49d21250961b560f1692219e835f95768bc31`，overlay SHA-256 为 `dcd054eeae9c3cd766e0d6f265b24fde85abcc8994d6fb3699d3f0d61cbec1af`，launcher 为 `0067050850341115a6084a3351aad287a65eef311066aa586c262c62246430ed`。十四个 Android 目标构建通过，最终 no work；十四份产物冻结、十三份部署，前后十七项设备输入哈希一致。物理分配、普通/最终 fixed 检查、FPR 必要复证与独立 retry 的源实现保持。

v2 重新运行 125 项 native，退出码全部符合预期；主测试 728→730 cases、225050 次断言，仍为既有十一例/六十六个失败断言、零 error，新两例均运行通过，共享用例顺序及失败签名相同。IR 快/慢各六十五例、10035/10119 断言，fixed-class 两例/39 断言通过；121 个 mode 标记与相邻版一致，三项 cycle 的 rescue_interrupt 为零。raw coverage 保留真实新增，由单独 declared coverage gate 核对，没有改写成无变化。

十组静态比较均核验真实模式、guest CRC/checksum、参考日志 SHA 和包含重复项的记录顺序。五组 bench 的完整 IR/形状/顺序相同，host bytes 仍为 19896、19904、16468、20068、16492。CoreMark 公共 PC 发码及非 formatter 的有序记录保持：

| 模式 | 实际记录数：相邻→本批 | RA host bytes：相邻→本批 |
| --- | ---: | ---: |
| 默认 Function | 426→427 | 286744→286836 |
| bound-off Function | 427→427 | 286840→286840 |
| 实际 Block | 2417→2419 | 205348→205488 |
| fixed-class Function | 426→426 | 286416→286416 |
| fixed-class Block | 2419→2416 | 206836→206584 |

记录差异仅在已核验的三个 formatter，不归为主体发码收益。三十五种既有布局保持：包括 SemanticFacts 624、CompileWorkspace 224、BlockSummary 8、BlockAnalysisIndex 240、UnitActions 848、AllocationWriter 192、AllocationPreparation 200、TargetPreparation 2280、InstructionScratchFacts 16、视图 24 和临时 ScratchActions 56 bytes。ScratchRange 与退役 ScratchAction 都为二十字节。唯一列出的布局增长为进程级 PerfStats2：1480608→1480680。

新统计的五组 bench 如下；临时/冻结容量列包含 range 和 MEM input 两个 vector 的 payload capacity 和：

| 模式 | 指令→range | MEM inputs | 临时容量 bytes | 冻结容量 bytes |
| --- | ---: | ---: | ---: | ---: |
| 默认 Function | 5292→3506 | 598 | 93096 | 114304 |
| bound-off Function | 5294→3508 | 598 | 93096 | 114304 |
| 实际 Block | 5533→4458 | 464 | 110964 | 136584 |
| fixed-class Function | 5292→3506 | 661 | 94408 | 114336 |
| fixed-class Block | 5533→4458 | 468 | 111028 | 136648 |

这些是同一候选准备流中约 33.7%（Function bench）/19.4%（Block bench）的记录数减少，不是总内存或性能提升百分比。v1→v2 的临时容量：默认 Function bench 118096→93096，Block bench 136584→110964；默认 Function CoreMark 2016484→1568564，IR 慢模式 39260→26740。所有十一份 scratch 日志的 payload/capacity 界限闭合；旧版没有同口径容量，完整旧堆成本与重分配重叠仍未验收。

十一份九类 workspace 日志闭合、可加；十份静态日志的六项请求/释放/当前/峰值基础字段均保持，IR 慢模式完整十一字段也相同：130 次请求、6385272 请求 bytes、3981724 逻辑峰值、49584 闲置容量峰值、80 零初始化及 276152 模式初始化 bytes。MEM occurrence 表真实增加行借用/填充，不能因其替代未计量图而抹平：五组 bench 的总 pattern_init_bytes 分别为 42016→50772、42040→50804、78740→87292、42016→52848、78740→88568；这些日志的差异仅在 instruction_rows 与 total。CoreMark 另有 formatter roster 差异，不能全部归因。语义图/索引、全部请求次数、复制与峰值的全口径成本继续接续。

PMU 仅对最终 v2 做每模式一组 reference-2k / candidate-2k / candidate-12k / reference-12k，参考为相邻冻结 launcher，八点 CRC 正确。相对斜率/截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | -0.097826% / +0.908382% | +0.488596% / +0.180737% | +1.519493% / -1.899562% |
| 实际 Block | +0.004801% / +0.528355% | +0.094864% / +0.699093% | -0.252174% / +0.279112% |

walt，current/max 快照均为 1171200 kHz，不能证明锁频；cpuss 30.9–34.0°C、cpu1-0 31.6–34.4°C、cpu1-6 30.9–32.4°C。instructions 截距、cycles 等正向变化保留；单组混合数据不构成收益或无退化证明。缺少事件运行比，不报告 IPC，截距不解释为纯编译时间。

三十九项结构与已检查行为门通过，证据前缀 `/private/tmp/swiftvm-middle-results/r3-scratch-ranges-v2`。本批完成 scratch 必要事实的 range 运输与显式准备边界，以及 MEM 输入的精确无图计数；必要预算和身份/冲突拒绝未丢失。完整初始化/请求/峰值成本、其余 fixed/spill/helper 契约与调用方、剩余候选/事实及正式 FEX 三方对照继续推进，完整 R1/R3 和总目标仍未完成。

## R3 fixed 复证与 spill ownership 暂存收敛

相邻参考为 `r3-scratch-ranges-v2`。本批只修改公共 allocate/fixed_verify.cpp、spill_regions.cpp 及两个既有测试文件，没有新增源文件、公开 API、计数器、probe 或测试脚本。普通 attempt 的必要 fixed/scratch 检查、目标编辑后的最终 fixed 检查及独立 FPR retry 保持各自位置；helper ABI、预算、目标准备和 emitter 源文件不变。

fixed 复证将三十二个 writer vector 收为一个按 home/完整 u32 instruction 排序的 u64 键缓冲，只收集本块实际 prefix/transferred home 所查询的写入。消费见证的 unordered_map 退役，改为借用 PhysicalGPRWrite 输入行、排序后查询；全部目标输入保留 home 检查；带 consumed-use 的行还保留时序、实际 instruction/consumer 与重复消费 key 检查，无关 home 的重复 key 也拒绝。保持两个区间端点、原透明链、原始边宽度和首个 fail_kind。普通写入键仍支持 u32，不因 IR ID 通常为十六位就截断 target input。单条 writer 由四字节变为八字节，减少容器头不等于减少总 payload、请求或栈帧；这些动态成本尚未完整计量。

spill ownership 将 positions、use_counts、transferable_uses、handoff_uses 四棵树及 prepared-values 线性查找收为一棵有序映射。私有 SpillValueUses 同时保存去重的 consumer 位置、完整 u32 occurrence、计数 presence、资格和已处理位；presence 独立于数值零，缺少计数不能给出完整所有权。非 pseudo 指令只做一次 ForEachValue，原始 typed Params/Lambda 的每次出现仍计数，同一 consumer 的位置只记一次。原始 allocation ID 和去 BitCast 的 allocation ID 分别投影，尚未绑定的别名不强行归并；count-only 行跳过选择。升序选值及 ownership pair → definition transfer → repeated reload 的顺序保持，严格 headroom 与 terminal 根条件不变。SpillReloadDemand 也由 GetValues 临时集合改为同一 raw visitor。仍有私有树节点/位置 payload、资格查询与定义查找，不能据此认定完整 R1/R3 已完成。

新增两个永久 case：主测试覆盖九种重复引用、外部/pseudo/terminal 使用、原始/已绑定别名、300 个 Params 加 Lambda、局部分段及两个 MEM 值竞争的场景；检查完整/部分 ownership、相同 region 与不同寄存器选择。IR case 覆盖连续/稀疏编号、多个 home、转移后缀与端点、完整 u32 instruction/consumed-value、正确/错误消费见证，以及无关 home 的重复 key。十项隔离主体比较通过，包括透明链/宽度、placement/preservation/fail-kind、terminal/headroom/选寄存器、公共两种检查及 Block/Function 调用边界；不替代新索引与合并投影的实际测试。

v1 的十一项集中 native 中，主测试、新 spill case、fixed-class 和七项入口/调用/循环场景通过；新增 IR case 在稀疏编号分支暴露夹具错误：手动修改 ID 后仍按原 dense 容量创建 Allocation。IR 快/慢各一处 error，慢 XML 还保留一处受越界写影响的旧用例名称损坏；原始日志、XML 和失败 coverage 均保留，不将其改写为通过。v2 只将这个夹具容量改为实际末条 ID+1，两个生产文件不变。最终十四份产物中十三份 SHA 与 v1 完全相同，只有 IR test 改变；v1 前后十项设备输入哈希一致，因此仅重跑 IR 快/慢，九项成功 native 按最终二进制哈希严格复用，保留其实际 v1 命令和文件名。这是选定的十一项验证，不是新跑完整 125 项矩阵。

最终 `r3-physical-contract-temporaries-v2` 冻结 458 个 source、109 个删除路径，相对相邻版四个既有文件修改、零新增/退役。source digest 为 `9c5d00e65cde1bbcfabf9fc0eb9733c2e8246464a68419509844878b77fc83f3`，overlay 为 `3526fe4189c69d3671f7e75015b9bce6615fee507f47e4b75a35610187a7a82a`，launcher 为 `f2cc7e21c81b10becb9beae533e23e603d00517dcd4f9e9dda3433eb230bfddb`。十四目标构建并冻结，最终 no work；六份所需产物部署，最终前后十项输入哈希一致。最终新日志/XML 二十二份，另保留十一份实际 v1 的复用 native 日志/XML；设备与主机原始证据均留存。

最终选定 native 退出码全部符合预期：主测试 730→731 cases、225095 次断言，仍为既有十一例/六十六个失败断言、零 error；新增 spill case 通过。IR 快/慢各 65→66 cases、10085/10169 次断言，零失败/零 error；新增 fixed case 两种编号均通过。fixed-class 两例/39 断言通过，七项 mode 标记与相邻版相同，三项 cycle 的 rescue_interrupt 为零。所有共享用例顺序及失败签名保持，原始 coverage_as_designed 因真实新增而为 false，由单独 declared coverage gate 核对新增各一例，没有消掉真实新增或沿用 v1 损坏名称。

十组静态检查均验证实际模式、CRC/checksum 和参考日志 SHA/重复项顺序。五组 bench 的完整形状、有序记录和代码量相同，host bytes 仍为 19896、19904、16468、20068、16492。CoreMark 公共 PC 发码相同，非 formatter 的有序记录相同：

| 模式 | 实际记录数：相邻→本批 | RA host bytes：相邻→本批 |
| --- | ---: | ---: |
| 默认 Function | 427→426 | 286836→286744 |
| bound-off Function | 427→427 | 286840→286840 |
| 实际 Block | 2419→2416 | 205488→205200 |
| fixed-class Function | 426→427 | 286416→286508 |
| fixed-class Block | 2416→2413 | 206584→206168 |

记录增减仅在三个已核验 formatter，不作为本批主体发码收益。三十九种既有布局保持，包含 SemanticFacts 624、Workspace 224、UnitActions 848、AllocationWriter 192、AllocationPreparation 200、TargetPreparation 2280、FixedValidationInputs 80、SpillInputs 72、ScratchRange 20 和进程 PerfStats2 1480680 bytes。新私有 SpillValueUses 为三十二字节；不含树节点、position vector payload、分配头、搬迁或同时存活峰值，不是每单元净节省。

十一份九类 workspace 日志均闭合且可加；十组静态的六项请求/释放/当前/峰值基础字段保持，五组 bench 全部十一字段及 IR 慢模式也保持。IR 慢模式仍为 130 次请求、6385272 请求 bytes、3981724 逻辑峰值、49584 闲置容量峰值、80 零初始化及 276152 模式初始化 bytes。CoreMark 另有 formatter roster 引起的事件/填充计数变化，不将其归为私有暂存收益。既有 scratch 九字段统计的十一份输出也闭合，五组 bench 和 IR 的记录/容量保持；它们及九类 workspace 计数都不覆盖本批私有容器的全部动态堆成本。

PMU 只在最终 v2 做 Function/实际 Block 各一组 reference-2k / candidate-2k / candidate-12k / reference-12k，参考为相邻冻结 launcher，八点 CRC 正确。相对斜率/截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | +0.000334% / -1.766232% | +2.412318% / -5.778562% | +2.111495% / -5.850882% |
| 实际 Block | -0.046193% / -1.453863% | +0.030436% / +4.431852% | +0.112099% / +2.962840% |

walt，current/max 快照均为 1171200 kHz；cpuss 30.5–34.0°C、cpu1-0 31.6–34.0°C、cpu1-6 30.1–32.0°C。指令截距降低与 Function 周期斜率约 +2.41% 等正向变化一并保留。没有完成周期变化归因，单组结果不构成收益或无退化证明；快照不证明锁频，缺少事件运行比，不报告 IPC，截距不是纯编译时间。正式性能与三方质量门仍为 false。

二十八项已检查结构/行为门通过，证据前缀 `/private/tmp/swiftvm-middle-results/r3-physical-contract-temporaries-v2`。本批收拢私有 proof/ownership 暂存，保留全部必要检查层次；并未完成剩余 fixed/spill/helper 调用方、普通路径资格查询、完整动态成本或正式 FEX 三方比较，完整 R1/R3 及总目标继续接续。

## R1 prepared 结果的工作区隔离与线程交接验收

本批 `r1-delayed-prepared-handoff-v2` 以 `r3-physical-contract-temporaries-v2` 为参照，只修改 hir_lifetime_test.cpp、ir_contract_test.cpp 及测试注册 CMake；全部生产源码保持。删除原有隐藏的 IR DCE chain scaling probe 及仅供它使用的 chrono/iostream include，原长链 DCE、跨块回收等正确性 case 保留。没有新增临时 probe、测试脚本、配置项或公开编译 API。

在既有独立寿命程序中增加 prepared-blocks 与 prepared-regions，CMake 各注册 fast 0/1，共四项。前者用生产 PrepareBlock 保留两个独立 owning 结果；后者用 PrepareFunctions 保留两个合并 region，第二个含跨块 SSA，secondary header 不拥有 canonical raw Blocks。各结果先实际发射并保存不同的机器码，合并 region 另保存完整 code object。随后仍在同一创建线程上复用同一 TLS workspace，覆盖并释放 65537 行的 instruction rows 与候选缓冲，再编译和销毁一个 256 级 Add 的第三个单元，重新发射原结果并逐字节对照。

创建线程结束（其 TLS 工作区已析构）后，将各 owning 结果逐个移入接收线程的新 vector，销毁原容器；再次检查 admission、单元及合并 code object 的机器码相同。释放第一个结果后，最后一个独立 Block/secondary region 仍独立有效并发出原字节；最终检查真实 Block/Function slab 对象全部回收。测试保持持有者覆盖每次 emission，不从裸视图反向取得 owner。

v1 的 24 项集中 native 全部通过，但四份新诊断只有人工覆盖的两种缓冲请求。检查发现默认 ModuleConfig 未开启残余 pass，第三次编译没有实际触发 value epoch 扩容，这不足以验证所需场景。v2 仅加强该新夹具：用显式 mapped Module 启用 ConstantFolding、DeadCodeRemove、FlagElimination，并固定 int_imm_fold；在第三次编译前后，断言 value rows、lookup、queue 的实际 allocated_bytes 增长。生产实现与所有旧寿命函数主体不变，原 v1 日志/计量保留，没有将“程序通过”直接当作该覆盖完成。

最终四种新场景均通过。各份 workspace 日志包含九类及 total，15 次请求/15 次释放、536820 请求/释放 bytes、live=retained=0，逻辑峰值 524460、闲置容量峰值 11344、九次保留事件、模式初始化 262148 bytes；value rows/lookup/queue 分别有 2/2/9 次实际请求，三种扩容断言均执行通过。计数与字节比较联合证明该夹具的复用/增长路径，故意覆盖和扩容的成本属于测试输入，不是生产内存收益；九类口径也不覆盖全部 IR、结果及私有堆存储。

最终 source 冻结 458 个路径、109 个删除路径，相对相邻版三个既有测试/注册文件修改、零新增/退役文件。source digest 为 `61e1141a43af8addb1d6cf5e34f92e3c6dc200afb83e93f2f9707785e450bf47`，overlay 为 `88005c3c4e2d2843206bd769222ca3a0a0552ed65951de951d965eab03eb173a`。十四个 Android 目标构建并冻结，最终 no work。相邻版十二份二进制（含 launcher 和主测试）逐字节相同，变化仅为 IR 与 HIR 寿命程序；launcher SHA 仍为 `f2cc7e21c81b10becb9beae533e23e603d00517dcd4f9e9dda3433eb230bfddb`。v2 相对 v1 仅寿命程序变化，部署这一份，前后三项实际输入哈希相同。

验证按受影响程序集中进行：v1 首次跑四种新增场景、十八种既有寿命场景及 IR 快/慢，共 24 项；夹具加强后只重跑 22 项寿命场景，IR 两份结果按完全相同的最终二进制 SHA 复用，保留其实际 v1 命令、XML 和文件名。最终选定的 24 项全部符合退出码；十八项既有 mode/检查计数与参考一致，新增四项的 fast、mode、零 injected_failures 和 passed 标记齐全。IR 快/慢仍各 66 cases、10085/10169 断言，零失败/error，顺序、签名及完整 coverage 均相同。最终有二十二份新日志和四份精确复用的 IR 日志/XML，没有把它们写成新跑完整 125 项。

旧寿命路径十个隔离主体保持；IR 文件的唯一主体删除是隐藏计时 probe，已从最终 IR 二进制的注册字符串中消失，现有长链/跨块 DCE case 仍在。四个新 CTest 注册均由生成的测试清单确认。全部生产源和 release/main 二进制不变，因此未重复主套件、十组静态或 PMU；相邻版已有的十一例/六十六个主测试失败断言、Function 周期斜率约 +2.41% 的未归因变化及正式 FEX 缺口仍保留，不能用这批寿命验证替代其处理。

二十四项已检查结构/行为门通过，证据前缀 `/private/tmp/swiftvm-middle-results/r1-delayed-prepared-handoff-v2`。本批闭合所列 prepared 独立 Block/合并 region 的顺序跨线程延迟发射边界。新模式比较实际 emission 字节，没有执行或发布这些 guest 代码；优化中输入/活动 facts 的 worker 交接、多个编译 worker 并发准备同一 Module、取消/失败及适用的发布/运行矩阵继续接续。完整初始化/搬迁/峰值成本、其余 R1–R7 和正式 FEX 三方验收仍未完成。

## R2 标量常量共享的有界存储

本批 `r2-scalar-constant-window-v1` 接续 `r1-delayed-prepared-handoff-v2`，只修改 values.cpp、ir_contract_test.cpp、hir_lifetime_test.cpp 和既有 CMake 测试注册，零新增/退役源文件。退役标量常量整段 unordered_map 及私有 ImmKey/ScalarImmEntry；没有新增临时 probe、测试脚本、统计字段、配置或公开 API。

标量 CSE 改用私有 ScalarImmWindow：八个定义指针及游标/有效期，首个启用的 LoadImm 才初始化。每条实际 IR 指令推进一次，包括 pseudo 和向量节点；先查询再覆盖最老槽，距离恰为八仍可复用，距离九不再复用。完整 ReturnType 与立即数位值共同匹配，重复定义被替换时不安装新锚点、不延长原定义寿命；窗口过期后撤销，下个常量可重新激活。距离不使用稀疏指令 ID。

Goto、NotGoto、BindLabel 清除标量窗口、向量表和零值锚点。replacement 继续跨边界存活，保留先前已支配替换的传递；向量常量及 VecSharedZero 仍按块共享，scalar const_cse 关闭不关闭其规则。replacement 为空时跳过原始参数访问；一旦有替换，原 Value/Lambda/Params 视图与 use 更新主体保持。ShareConstants 不另借用或建立 value 身份表、图或 worklist；它仍是每块一次有序扫描，资格预扫和受影响 CSE 窗口驱动尚未完成。

永久 IR case 使用独立的完整键映射与绝对位置参考，不复制旋转窗口实现；分别覆盖普通 Block、稀疏 ID Block、已登记的两块 Function，const_cse 各开/关，共六条轨迹。涵盖八种常量填满窗口后复用最老项、距离 7/8/9、重复不滚动锚点、过期重启、相同位值的不同类型、混合 Lambda/Params 重复引用、pseudo 配对、三种控制边界、远距向量/零值、256 个不同键及 Function 跨块导出。逐项核对原始引用和类型、完整 use 次数及 HIR use 列表，期间占用 ValueQueueScope，证明该路径不另借 value 存储。

既有独立寿命程序增加 constant-window 模式，fast 0/1 各注册一项。对 256 个不同 U64 常量分别关闭/开启 scalar CSE，只在 ShareConstants 阶段启用已有分配失败钩子；两种输入均无异常、钩子未消耗，256 个定义均保留。此处证明所列输入的 C++ 堆请求为零，不覆盖重复替换表、向量表或整个编译器，也不测量全部初始化和查询流量。

最终 source 冻结 458 个路径、109 个删除路径，四个既有文件修改。source digest 为 `4ca6c7fb1199729bdee0d1db8292dddee97d53730f7336cbd0bd971335845455`，overlay 为 `3fc365523f5f7e7da88f982f26fa709165f664500ed7b6549a7da092ca5d20c4`。十四个 Android 目标集中构建并冻结，最终 no work；部署十三份，launcher SHA 为 `826a3df0188d0f664d8b65772de8c1e6c9115e59c5cbec89c26620246633b86a`，前后十七项设备输入哈希相同。静态和 PMU 参照 physical-contract-temporaries-v2，其 release 源码及 launcher 与相邻 handoff-v2 相同；参考 launcher 为 `f2cc7e21c81b10becb9beae533e23e603d00517dcd4f9e9dda3433eb230bfddb`。

红魔 NX729J 集中运行 131 项，全部符合预期退出码：原 125 项、四个既有 prepared handoff 模式及两个新窗口模式。本批全部为新运行。主套件 731 cases、225091 断言、十一例/六十六个已知失败断言、零 error；参考为 225095 断言，共享 case 顺序及全部失败签名相同。成功断言总数少四个的原始差异保留，不归因为 CSE，也不重跑以抹平数字。IR 快/慢各 67 cases、15513/15597 断言，零失败/error，各增加一个参考对照 case；fixedclass 为 2 cases/39 断言通过。121 个旧模式标记一致，三个 cycle 的 rescue_interrupt 为零；四个 handoff 标记与参考一致，两个新模式均零 injected_failures 并 passed。

十组静态实际模式、输出/CRC、参考日志哈希及顺序全部核对。五组 bench 的完整有序记录和 shape 相同，host bytes 依次为 19896、19904、16468、20068、16492。CoreMark 的共同 PC 字节及格式化函数以外的完整有序记录不变；差异只在已核实的三种 formatter 符号中：

| CoreMark 模式 | 记录数，参考 → 候选 | RA host bytes，参考 → 候选 |
| --- | ---: | ---: |
| Function | 426 → 427 | 286744 → 286836 |
| Function bound-off | 427 → 427 | 286840 → 286840 |
| 实际 Block | 2416 → 2418 | 205200 → 205420 |
| Function fixed-class | 427 → 426 | 286508 → 286416 |
| 实际 Block fixed-class | 2413 → 2414 | 206168 → 206464 |

这些动态编译 roster 变化不记为生成代码收益。十一份既有 workspace 记录请求/释放与分项合计闭合，基准和 IR 全字段相同；十组静态的六项基础字段相同，CoreMark 其余暂存总量随所列 roster 变化，不能声称全部动态总量不变。十一份 scratch 容量/覆盖检查通过。

九处独立规则/改写主体保持，包括整数纯规则调用、向量分支及原始参数重定向主体；这不替代新窗口的参考验证。DWARF 中四十种既有有效布局不变，旧 ImmKey/ScalarImmEntry 各 16 bytes 已消失，新私有 ScalarImmWindow 为 72 bytes；没有新增 per-unit payload。实际 launcher 反汇编显示 DedupConstants 自身静态入口栈帧从 416 增至 432 bytes，增加 16 bytes。该数不是传递调用峰值，也没有覆盖全部堆/栈成本，不能据此宣称总内存下降。

同机 CPU 4 下，Function 和实际 Block 各交错三轮 reference-2000、candidate-2000、candidate-12000、reference-12000，共 24 点，全部退出零且 CRC 正确；每点保留前后六项设备条件。下表采用“各版本三轮系数中位数的相对变化”，每格为斜率 / 截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | +0.013628% / -0.333459% | -0.374163% / -0.020458% | -0.503367% / +0.001324% |
| 实际 Block | -0.024557% / -0.099010% | -0.382035% / +0.781634% | -1.312590% / +1.284586% |

配对轮次相对变化的中位数与上述口径分别保存，不能混用。以下为三轮配对变化的完整观察范围，不是置信区间：

| 模式/指标 | 斜率变化范围 | 截距变化范围 |
| --- | ---: | ---: |
| Function instructions | -0.023891% ～ +0.143191% | -0.471094% ～ -0.299528% |
| Function cycles | -1.197088% ～ +0.383959% | -1.967522% ～ +1.557399% |
| Function wall | -1.520988% ～ -0.383519% | -1.736877% ～ +2.612735% |
| Block instructions | -0.041045% ～ +0.006516% | -0.194672% ～ -0.099010% |
| Block cycles | -0.811052% ～ -0.290207% | +0.211591% ～ +1.197353% |
| Block wall | -1.871752% ～ +0.203969% | +0.470716% ～ +2.395219% |

walt，current 940800–1171200、max 1171200 kHz；cpuss 31.3–34.8°C、cpu1-0 32.0–35.2°C、cpu1-6 30.5–32.8°C。未锁频，缺少事件运行比，不报告 IPC。指令截距在各配对轮次均下降，但它混合启动、编译、运行时及 formatter 成本，不能全部归因于 CSE。Block cycles/wall 截距的正向代价完整保留；先前物理暂存批次 Function 周期斜率约 +2.41% 的单组变化仍未归因，本批不同改动的三轮结果不能追溯解释它。仅本次观察的指令斜率退化均低于 1%；正式性能及固定 FEX 三方质量门仍为 false。

三十三项已检查结构/行为门通过，九十四项原始日志哈希核对，证据前缀 `/private/tmp/swiftvm-middle-results/r2-scalar-constant-window-v1`。本批删除标量整段哈希及无替换时的参数访问，保留原八指令限制和必要一致性工作；其余类型推断、受影响窗口驱动、state/helper effects、完整初始化/动态成本及 R1–R7 验收继续接续，完整总目标未完成。

## R3 spill ownership 的查询与定义定位

本批 `r3-spill-query-windows-v1` 接续 `r2-scalar-constant-window-v1`，只修改公共 allocate/spill_regions.cpp 和既有 spill_forwarding_test.cpp。没有新增文件、公开接口、配置、统计字段、临时 probe 或测试脚本；目标输入、必要验证、重试、映射及冻结结果的布局保持。

BuildSegment 不再对每条非 pseudo 指令查询 consumer flags，首个实际标量 MEM occurrence 才查询一次并供该指令后续 raw 引用共用。没有 MEM 参数、只有寄存器参数或只有浮点 MEM 参数时不查该 flags 表；原始/去 BitCast 的 allocation ID 仍独立，consumer 位置仍去重，完整 u32 occurrence、Params/Lambda 类型引用和 pseudo 排除规则保持。

每个候选值的本段定义只定位一次。借用唯一 BlockAnalysisIndex 已验证的 DenseInstructionIDs，在当前段范围内直接按偏移查找；段外定义返回缺失，不能因为同属一块就越过控制边界。稀疏/手工编号保留原指令序列查找，不另建索引或重建指针段。ownership pair、definition transfer 及重复 reload 共用该位置，pair 直接借用当前映射行，退役它重复查找 input 行的操作。选值升序、pair → definition transfer → repeated reload、严格 ID 邻接及 scratch headroom 均保持。

terminal 投影改用既有 VisitTerminalValues，退役本文件按值复制 variant 的递归访问。共享访问器按 const 引用遍历 If/Switch/Condition/CheckHalt；本文件继续选择 raw terminal Value 的物理 alias、排序去重，不引入 hoist anchors 或额外隐式根。临时有序映射、位置 payload 和 terminal ID 缓冲仍存在，不能把移除递归复制记成完整阶段零堆成本。

既有 occurrence/alias case 从九种加强为十二种，增加删除已编号无效节点产生的稀疏间隙、正偏移控制段及嵌套 terminal；分别核对实际稠密属性与最终 ownership。新增一个永久 case，用 dense/sparse 各六种输入验证 GetOperand→LoadMemory pair：普通与前缀段可接续；物理相邻但 ID 不相邻、定义在前一控制段均拒绝 pair；额外输入使用保留不拥有全部 uses 的普通 reload，terminal 使用的结果也保留普通重复 reload。稀疏夹具删除 AdvancePC 后保留原有效 capacity，不以越界手工编号构造场景。

最终 source 冻结 458 个路径、109 个删除路径，相邻版仅两个既有文件修改。source digest 为 `76fdb732c1d593826897c9f67af5515d33712d2764acd793370c4aa16eddad38`，overlay 为 `57b08fa5f316d196d76a163492828468a948d747c6bd45bb058c319ddf4efe48`。十四个 Android 目标一次集中构建并冻结，最终 no work；部署十三份，前后十七项设备输入哈希相同。候选 launcher 为 `7ff9549ef7a7f24f95b3806f734e2894c4a301256e0f9a4cbf78d180776f9eff`，参考为 `826a3df0188d0f664d8b65772de8c1e6c9115e59c5cbec89c26620246633b86a`。

验证使用包含主套件的集中十九项相关 native，不重跑全部 131 个模式。主套件 732 cases、225206 断言，参考 731/225091；只增加所列 pair case，共享 case 顺序、全部失败签名及十一例/六十六个已知失败断言保持，零 error。IR 快/慢仍各 67 cases、15513/15597 断言，fixedclass 2 cases/39 断言，均零失败/error。十五个旧 mode 标记一致，三个 cycle 的 rescue_interrupt 为零；所选入口、disk/AOT、调用、关闭、准备结果和 retirement 路径均符合预期退出码。该子集不替代完整故障/寿命/并发矩阵。

十组静态模式、输出/CRC、参考身份及有序记录全部核对。五组 bench 的完整记录和 shape 相同，host bytes 仍为 19896、19904、16468、20068、16492。CoreMark 共同 PC 字节和 formatter 外的完整有序记录不变；变化只落在已核实的三种 formatter 符号中：

| CoreMark 模式 | 记录数，参考 → 候选 | RA host bytes，参考 → 候选 |
| --- | ---: | ---: |
| Function | 427 → 427 | 286836 → 286836 |
| Function bound-off | 427 → 426 | 286840 → 286748 |
| 实际 Block | 2418 → 2415 | 205420 → 205128 |
| Function fixed-class | 426 → 426 | 286416 → 286416 |
| 实际 Block fixed-class | 2414 → 2413 | 206464 → 206168 |

这些 roster 差异不作为代码缩减收益。十一份 workspace 请求/释放及分项合计闭合，十组静态的六项基础字段相同，bench 与 IR 全字段相同；其余 CoreMark 字段随所列 formatter roster 变化。十一份 scratch 容量/覆盖检查通过。workspace 口径仍未覆盖全部私有堆、初始化及重分配峰值。

SpillReloadDemand、HasHeadroom、FindRegionRegister 和 SpillValueUses 四处隔离主体保持，选值顺序保持；这些检查不替代已改变查询的行为测试。实际双方主测试 SHA 绑定的 DWARF 中四十一种有效布局完全相同，SpillValueUses 仍 32、BlockAnalysisIndex 240、CompileWorkspace 224、PerfStats2 1480680 bytes。launcher 反汇编中，BuildSpillReloads 外层自身栈帧仍为 48 bytes；包含内联 BuildSegment 的块处理函数从 416 增至 480 bytes，增加 64 bytes，其静态指令字节从 3844 变为 3828。此为指定函数自身成本，不代表传递/同时调用峰值或完整编译器内存收益；栈帧增加明确保留。

同机 CPU 4 下，Function 和实际 Block 各三轮交错 reference-2000、candidate-2000、candidate-12000、reference-12000，共 24 点，全部 CRC 正确，每点保存前后六项设备条件。下表为各版本三轮系数中位数的相对变化，每格为斜率 / 截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | +0.022127% / -0.395990% | +1.563603% / -2.021462% | +2.578200% / -3.551132% |
| 实际 Block | -0.021370% / -0.218992% | +0.837797% / -1.003421% | +1.141182% / -2.169914% |

配对轮次相对变化的中位数另外保存在摘要，不能与上表口径混用。以下为配对变化的完整观察范围，不是置信区间：

| 模式/指标 | 斜率变化范围 | 截距变化范围 |
| --- | ---: | ---: |
| Function instructions | +0.007701% ～ +0.258538% | -0.939677% ～ -0.363147% |
| Function cycles | -0.440312% ～ +1.563603% | -2.021462% ～ +2.328039% |
| Function wall | -2.082289% ～ +2.683148% | -3.997774% ～ +2.921171% |
| Block instructions | -0.046524% ～ -0.014347% | -0.373589% ～ -0.218992% |
| Block cycles | -1.140443% ～ +1.482109% | -3.584768% ～ +1.150299% |
| Block wall | -0.879945% ～ +1.449505% | -3.431021% ～ +0.378615% |

walt，current 1056000–1171200、max 1171200 kHz；cpuss 31.3–36.7°C、cpu1-0 32.0–36.0°C、cpu1-6 30.9–33.6°C。未锁频，缺少事件运行比，不报告 IPC。指令截距降低与周期/耗时斜率正向变化一并保留，未完成后者归因；截距混合启动、编译、运行时和 formatter 成本，不记为纯编译器收益。先前约 +2.41% 的 Function 周期观察也未由本批解释。仅本次观察的指令斜率退化低于 1%；正式性能及固定 FEX 三方质量门仍为 false，没有追加采样以平滑结果。

三十三项已检查结构/行为门通过，九十六项原始哈希核对涉及五十五个不同路径，证据前缀 `/private/tmp/swiftvm-middle-results/r3-spill-query-windows-v1`。本批退役重复定义/行查询与独立 terminal 递归，按实际 MEM 输入门控 flags；私有容器完整成本、候选与 helper/fixed 接缝、剩余 R1–R7、正式 FEX 比较继续接续，完整总目标未完成。

## R1 已封口优化输入与活动 facts 的线程交接

本批 `r1-active-facts-handoff-v2` 接续 `r3-spill-query-windows-v1`，只加强既有 hir_lifetime_test.cpp 及 CMake 注册。新增 optimized-block/optimized-function，各注册 fast 0/1，共四项；没有新增源文件、临时 probe、测试脚本、生产接口或统计字段。辅助 CaptureInstructions 改为接受 const HIRFunction，包括捕获辅助函数在内的十个既有主体保持。

生产线程通过公共 OptimizeBlock/OptimizeFunction 生成 owning OptimizedUnit，Function 输入含两个执行块和跨块 SSA。生产线程预建基础活跃性、Function CFG 与各块直接 uses，保存真实 facts/索引地址，然后结束 builder、局部引用及 TLS workspace。接收线程移动该优化输入，检查同一 facts 和全部缓存地址，借用自己的 workspace，通过既有 AllocationPreparation、公共 AllocateBlock/AllocateFunction 及 ARM64 完成/准入链实际分配；分配后再次核对缓存身份。

writer 析构后先实际发射，再通过 TakeIR 释放整个活动 facts 存储；接收线程内再次发射，字节相同。接收线程及其 TLS 工作区也退出后，主线程从稳定的 IR/Allocation/UnitActions owner 再检查 admission 和完整机器码；最后释放 actions、allocation、IR，并检查原始指令复用及 Block/Function slab 全部回收。结果 owner 在测试外层稳定持有，不从只读裸视图反向取得所有权。

此处覆盖优化完成后至分配/发射的顺序独占 worker 交接，实际发射采用现有公开组件组成的链。新模式没有发布或执行 guest 代码，没有同时运行两个编译 worker，也没有转移活动改写/writer 作用域或 workspace 对象；不把这批证据扩为并发编译或新的异步生产 API 验收。

v1 首轮构建发现测试夹具的两处 Operand 名称歧义，以及 CaptureInstructions 未接受只读 HIRFunction。v2 仅修正测试作用域的名字引用与辅助函数 const 参数，生产源码始终不变；保留 v1 source/构建日志，v1 没有部署或设备运行。v2 集中构建、冻结十四个 Android 目标，最终 no work，只有寿命程序变化，其余十三份二进制逐字节相同，故未重跑主套件、静态配置或 PMU。

最终 source 冻结 458 个路径、109 个删除路径，相邻版仅两个既有测试/注册文件修改，344 个非测试 source 路径及删除清单相同。source digest 为 `d5a3c9e95b3749f225ea6df2fef653300e8961de0b1746652b657d633b716b04`，overlay 为 `17f046fb881a349c19ee56183bb191ce4cb4af43ce706e6b47aa1a6f10789f67`。新寿命程序 SHA 为 `773e7e874593f971826b74e01b9b6f611f5021de68a22afde4ad0ddc65c782f5`；只部署这一份，前后三项实际设备输入哈希相同。launcher 仍为 `7ff9549ef7a7f24f95b3806f734e2894c4a301256e0f9a4cbf78d180776f9eff`。

集中运行二十八个寿命模式，全部符合预期退出码。四种新场景检查数分别为 Block 123492、Function 123494，fast 0/1 相同，均零 injected_failures 并 passed；二十四个旧 mode/检查数/注入数与最近可用的逐项参考一致，参考批次及日志哈希显式保留。四个新 CTest 注册由生成清单确认。新模式使用默认 profiling 设置，没有 workspace-storage 分项日志；这批验证寿命，不推断分配量或峰值改善。

IR 快/慢两项按完全相同的最终二进制 SHA 精确复用相邻版结果，各 67 cases、15513/15597 断言、零失败/error；保留真实命令、参考 XML 的原程序名、文件名及逐字节内容。最终选定三十项结果，其中二十八项新运行、两项复用；归档含二十八份新日志及四份复用的 IR 日志/XML，没有记成三十次新运行。

二十四项已检查结构/行为门通过，证据前缀 `/private/tmp/swiftvm-middle-results/r1-active-facts-handoff-v2`。本批闭合所列已封口优化输入和原缓存 facts 的顺序 worker 交接、facts 最后借用及最终输出寿命。其余完整语义宽度/候选与暂存收拢、初始化/搬迁/峰值成本、适用并发准备/失败/发布/运行和固定 FEX 三方验收仍需接续；前批栈帧增加及周期/耗时正向变化保持原记录，完整总目标未完成。

## R2 活跃 value epoch 的有序常量反馈

本批 `r2-constant-feedback-v2` 接续 `r1-active-facts-handoff-v2`，修改 values.cpp、value_worklist.h/.cpp、passes.h、workspace.h 及既有 ir_contract_test.cpp，零新增/退役文件。常量反馈接入已有改写 epoch，没有新增队列容器、配置、统计字段、临时 probe 或测试脚本；内部 pass 接口随该职责扩展。

初始 CSE 仍执行一次有序扫描，沿用八条实际 IR 的标量窗口、块内向量共享及跨控制边界保留的 replacement；这次没有退役它，也没有删除原本不存在的重复 CSE 全块扫描。新增工作是在初始 CSE 之后，已借用的 value epoch 中继续响应新 LoadImm 和删除：通知最多后续八条实际节点，查询最多向前八条，Goto/NotGoto/BindLabel 截断，不用稀疏 ID 计算距离。已有重复共享节点在行中标记，不重新充当定义锚点。

普通规范化和 CSE 任务共用现有 u32 队列，每行分别去重两类任务；首次真实常量反馈才把队列转为堆顺序，普通任务的编码始终优先于 CSE，后者按原始行顺序处理。这样先完成单用立即数等规范化，再允许 CSE 增加 uses。两类任务共同消耗原 8N+32 可选访问预算；耗尽不截断已提交替换的全部 use 更新，也不截断必要 DCE/flags 固定点。每行最多可挂两个任务，实际容量和比较成本不能因共用同一容器而记作零。

真实匹配后，Block 借用可更新的直接 uses，已登记 Function 消费原 HIRUseList；只为本块、定义之后的普通用户取得稳定行快照，跳过 pseudo 配对与控制参数，不改跨块导出。连续重复引用先去重，最终排序去重；Value/Lambda/Params 使用与初始 CSE 相同的参数改写主体，保留原始类型视图。零 uses 的匹配不取快照；已有手工/未登记 Function 的显式全 roster fallback 仍存在。快照超过内联容量时仍可能分配，未把这部分私有堆记成 workspace 计量。

无 value/width 候选的路径继续不建立 value epoch，执行原初始 CSE 与必要清理；本批不为纯常量路径的后续 DCE 另开身份表。初始 CSE/资格预查与 replacement 表仍待收口，因此不能认定全部 CSE 已候选化或完整 R2 完成。

新增三个永久 IR case：普通/稀疏 Block 与已登记多块 Function 的六条开关轨迹检查锚点顺序、原始视图、300 个重复 Params、Lambda、pseudo、控制参数和跨块 uses，并以完整原始引用重新计数；Block/Function 各开关两种的四条轨迹检查新常量先折叠成单用立即数；十六条 DCE 轨迹检查删除一/两节点后距离从九/十缩至八，以及三种局部控制边界。用例经生产 BeginValueCleanup 资格入口建立 epoch，未为测试另建旁路规则。

v1 的 135 项 native 全部符合预期，十组静态输出也正确，但检查发现反馈任务可能先共享常量，再尝试单用立即数折叠；静态同时暴露某个 Function 增长 68 bytes。v2 将普通/CSE 任务在同一槽类型中分开排队，并增加直接验证该优先级的 case。保留 v1 source、135 份运行记录及十组静态证据，v1 未采 PMU。优先级用例已通过，pthread_rwlock_unlock 的 68-byte 增长在 v2 仍存在；这两个发现不能据此建立因果归属。

最终 source 冻结 458 个路径、109 个删除路径，相邻版六个既有文件修改。source digest 为 `6b2af7fb2dae4565aa5fc6596df232e6f77331f84b9aae1beb2bc701af6db419`，overlay 为 `52b2397f879e51cda80d62c7e647370d6df119a737259fbea86c03563240f2d7`。十四个 Android 目标集中构建并冻结，最终 no work；部署十三份，前后十七项设备输入哈希一致。候选 launcher 为 `36734fee766073edf356ae99f2c10a206aa86eaf20400177545baba2b26379c2`。静态/PMU 参考仍用 `r3-spill-query-windows-v1` 的 `7ff9549ef7a7f24f95b3806f734e2894c4a301256e0f9a4cbf78d180776f9eff`，其全部生产源码和 launcher 与相邻 active-facts 批次相同。

v2 集中运行 135 项，全部为新运行且符合预期退出码。主套件 732 cases、225105 断言，参考为 732/225206；十一例/六十六个已知失败断言、完整失败签名、case 顺序及覆盖保持，零 error。成功断言总量变化原样保留，不作单一归因。IR 快/慢各 70 cases、19435/19519 断言，零失败/error，只增加所列三个 case；fixedclass 为 2 cases/39 断言通过。131 个 mode 标记与逐项最新参考一致，三个 cycle 的 rescue_interrupt 为零；参考日志身份覆盖前述三个相邻矩阵来源，未把复合参考写成重新运行旧版。

十组静态均核对实际模式、输出/CRC 和参考哈希。所有模式的非 formatter guest PC 顺序相同，但真实 IR 与机器码形状已有变化，不再声称只在格式化函数中变化：

| 模式 | CoreMark 记录数，参考 → 候选 | CoreMark RA host bytes，参考 → 候选 | bench host bytes，参考 → 候选 |
| --- | ---: | ---: | ---: |
| Function | 427 → 427 | 286836 → 286876 | 19896 → 19896 |
| Function bound-off | 426 → 426 | 286748 → 286792 | 19904 → 19904 |
| 实际 Block | 2415 → 2412 | 205128 → 204800 | 16468 → 16464 |
| Function fixed-class | 426 → 427 | 286416 → 286548 | 20068 → 20068 |
| 实际 Block fixed-class | 2413 → 2414 | 206168 → 206436 | 16492 → 16488 |

Function/default 与 bound-off 的完整 PC 集合分别减少 81/80 个 IR 节点；三种 Function bench 各减少四个 IR，host bytes 与 shape 保持。两种 Block bench 各减少四个 host bytes。七组完整 PC 集合/顺序相同，对应 spill defs/loads/stores 全部不变；其余三组 CoreMark 的 formatter roster 变化，只有聚合观察，不冒充精确共同单元的 spill 验收。

CoreMark/default Function 的 spill defs/loads/stores 仍为 321/90/61，bound-off 相同；fixed Function 仍为 838/297/216。实际 Block 为 735/307/208 → 734/309/209，loads/stores 分别增加约 0.651466%/0.480769%；fixed Block 聚合仍为 1590/640/465。十组观察量均低于 2% 增长线，所有模式的精确同单元 spill 门仍未全部证明。

实际 guest 符号确认，`0x234b40` 为 pthread_rwlock_unlock，IR 689 → 682，host bytes 2472 → 2540，增加 68；`0x2262c5` 位于 Scudo populateFreeListAndPopBlocks，Block host bytes 348 → 360，增加 12。增长及其余小幅减少均保留，没有把 IR 减少或 formatter roster 差异当作整体代码收益；这两处机器码变化及锁/分配器负载质量仍待进一步归因。

七处整数/向量隔离主体及共享纯规则、widths、flags、DCE、pipeline 五个完整文件保持。DWARF 中四十六种有效布局相同，ValueWorklist 64、ValueRow 24、CompileWorkspace 224、SemanticFacts 624、PerfStats2 1480680 bytes；共享位使用原行尾 padding。实际 launcher 中 DedupConstants 自身栈帧仍为 432、Drain 为 64；Queue 从 112 增至 144，新 QueueFollowingConstants/ReplaceConstantUses 自身分别为 128/272 bytes。这些是指定函数自身固定帧，不能相加冒充传递或同时峰值。

十一份 workspace 记录请求/释放、分项合计和寿命闭合，十一份 scratch 容量/覆盖检查通过。计量也发生变化：default Function CoreMark 的请求字节 265376 → 265364，bench 67036 → 67032，部分显式初始化随较少 IR 略降；Block bench 请求字节 73092 保持。IR 测试新增三例，不能要求其存储总量与旧套件相同。队列访问、堆比较、私有快照及全初始化/搬迁流量未被这些字节覆盖，不能从布局或少量容量下降推出编译成本改善。

Function 和实际 Block 各交错三轮 reference-2000、candidate-2000、candidate-12000、reference-12000，共 24 个 PMU 点，退出和 CRC 全部正确，每点保留前后六项条件。下表为各版本三轮系数中位数的相对变化，每格为斜率 / 截距：

| 模式 | instructions | cpu-cycles | wall |
| --- | ---: | ---: | ---: |
| Function | +0.008539% / +0.416303% | +1.208700% / +2.128115% | +1.778578% / +2.781700% |
| 实际 Block | -0.046559% / +0.630463% | +0.520046% / +0.984628% | -0.367086% / +0.219854% |

配对轮次变化的中位数另存，不能混用；例如 Function wall 斜率的配对中位数为 -0.727854%，与上表的“系数中位数之比”不同。以下为完整配对观察范围，不是置信区间：

| 模式/指标 | 斜率变化范围 | 截距变化范围 |
| --- | ---: | ---: |
| Function instructions | -0.009195% ～ +0.026298% | +0.390592% ～ +0.446567% |
| Function cycles | -0.262915% ～ +2.660410% | -0.100845% ～ +2.256128% |
| Function wall | -1.092434% ～ +3.367212% | -0.244864% ～ +4.187780% |
| Block instructions | -0.063824% ～ +0.014689% | +0.248025% ～ +0.703991% |
| Block cycles | -1.436589% ～ +0.604475% | +0.282816% ～ +2.578937% |
| Block wall | -1.023249% ～ +0.136099% | +0.219854% ～ +4.090870% |

walt，current/max 快照均为 1171200 kHz；cpuss 30.1–34.4°C、cpu1-0 30.9–34.0°C、cpu1-6 29.7–31.6°C。快照不证明锁频，缺少事件运行比，不报告 IPC。指令斜率仅在本次观察中低于 1% 退化线；两模式每个配对轮次的指令截距均增加，反馈覆盖扩大尚未形成编译成本收益。截距混合启动、编译、运行时及 formatter 成本，周期/耗时变化也未完成归因；本批没有追加采样以平滑这些结果，正式性能和固定 FEX 三方门保持 false。

三十六项已检查结构/行为门通过，231 项原始哈希核对涉及 190 个不同路径，证据前缀 `/private/tmp/swiftvm-middle-results/r2-constant-feedback-v2`。本批交付活跃 epoch 的常量反馈与规范化优先级，初始 CSE/候选播种和 replacement 暂存仍需收口，新增查询/队列成本与两处代码增长仍需处理；其余类型、state/helper effects、完整动态成本与 R1–R7、FEX 三方验收继续接续，完整 R2 和总目标未完成。

## R2 初始常量共享复用活跃 uses

本批在 `r2-constant-feedback-v2` 上清理初始常量共享的用户运输。已有 value epoch 时，ShareBlockConstants 在同一次有序候选访问中，经 ReplaceConstantUses 直接更新实际用户，不再建立初始 replacement 哈希表，也不再为其遍历后续每条指令的 Value/Lambda/Params 参数。无 epoch 路径继续使用原有轻量替换表，不为常量-only 单元另建身份表或反向图。两条运输路径共用同一份候选代码；标量八条实际 IR、类型/完整位值匹配、不推进重复锚点、局部边界清候选，以及向量/零向量块内共享的规则保持。

活跃路径的初始标量、VecLoadConst、VecSharedZero 真匹配与后续标量反馈共用已有稳定行快照：Block 按需维护直接 uses，已登记 Function 使用 HIRUseList，手工/未登记 Function 的原有全 roster fallback 保留。仅更新本块定义之后的普通用户；局部边界后的既有普通用户继续重定向，pseudo 配对、控制参数及跨块导出不动。零 uses 匹配不查询消费图；大量重复 Params 先按稳定身份快照，再改写实际参数。共享标记方法收为 worklist 私有成员，未增加 buffer、profiling、feature、运行时 API、源文件或临时 probe。

Values → 初始 CSE → Widths 的原有阶段顺序与后续普通任务优先级未改。初始有序 CSE、资格预查、无 epoch 替换表及向量候选表仍存在；活跃路径真正匹配时也可能首次准备 Block use 图，快照超过内联容量仍可能分配。这里退役的是活跃路径的替换表与逐参数查表，不能扩大为全部初始扫描或完整 CSE 成本已经消失。

扩展已有完整键/绝对位置 oracle case，分别走借用已占用的无 epoch 路径和通过真实 Xor 候选打开的生产 BeginValueCleanup 路径。普通/稀疏 Block、已登记多块 Function 各覆盖 const_cse 开关，共十二条轨迹；活跃轨迹另检查跨局部分支后的已有标量/向量用户、300 个重复且带宽度视图的 Params、Lambda、pseudo/control 与跨块导出。期望值仍由独立旧策略计算，逐节点核对原始视图、完整 use-count 和 HIRUseList，没有新增 case 或测试程序。普通折叠、原始参数改写和后续常量反馈七个函数主体及 worklist/widths/flags/DCE/pipeline/纯规则六个完整文件保持。

最终冻结 458 个 source 路径、109 个删除路径，相邻版仅修改 values.cpp、value_worklist.h 与已有 ir_contract_test.cpp。source digest 为 `cd557459fc44990367632861dcf53fae4991c643555bac7477b935b3b08bd66d`，overlay 为 `daa81b20c9b8fbbd2800c2666995ff39bc1335a1a88728bd590b9d39cf605b47`。十四个 Android 目标集中构建并冻结，最终 no work；按所选运行部署八份，前后十一项设备输入哈希一致。候选 launcher 为 `eabc2c3cc273ceeb283cbd9d220ab0d4032d64f7e4afb55d8bcd090adf638bf2`，静态参考为实际相邻版 launcher `36734fee766073edf356ae99f2c10a206aa86eaf20400177545baba2b26379c2` 及其原始日志。

红魔上集中运行二十一项定向 native，全部为新运行且退出码符合预期；覆盖主套件、IR 快/慢、fixedclass、constant-window、optimized Block/Function 线程交接、prepared blocks/regions、native entry、已发布/region/call 退出、两单元 cycle 和 request-call。十七个 mode 标记与相邻版一致，所选 cycle 的 rescue_interrupt 为零。本批不重复其余 114 项，也不将其计为本批通过。主套件 732 cases、225369 断言，对照 732/225105；十一例/六十六个既有失败断言、完整失败签名、case 顺序及覆盖相同，零 error，成功断言总量差异原样保留且未作归因。IR 两模式均为 70 cases，25047/25131 断言，零失败/error，断言各增加 5612 来自已有 oracle 的扩展；fixedclass 仍为 2 cases/39 断言。

十组静态配置均核对实际模式、CRC/五项 bench checksum 与参考日志身份：

| 模式 | CoreMark 记录数，参考 → 候选 | CoreMark RA host bytes，参考 → 候选 | bench host bytes，双方相同 |
| --- | ---: | ---: | ---: |
| Function | 427 → 426 | 286876 → 286784 | 19896 |
| Function bound-off | 426 → 426 | 286792 → 286792 | 19904 |
| 实际 Block | 2412 → 2420 | 204800 → 205564 | 16464 |
| Function fixed-class | 427 → 427 | 286548 → 286548 | 20068 |
| Block fixed-class | 2414 → 2415 | 206436 → 206512 | 16488 |

十组的非 formatter 有序记录相同，所有共同 PC 的记录也相同；五组 bench 完整 IR/host 尺寸记录及 RA 形状相同，全部观察到的 spill defs/loads/stores 汇总相同。三个 CoreMark formatter roster 不同，其余七组完整 PC 顺序相同；据此不宣布全部模式的同工作集 spill 门已完成。Function 记录包含 IR/host 尺寸，不将尺寸相同写成机器码字节逐一相等。pthread_rwlock_unlock 的 2540-byte Function 和 Scudo 的 360-byte Block 仍存在，相对更早参考的 +68/+12 bytes 未被本批消除。

实际主测试二进制的四十六项有效布局均相同。初始共享外层函数由旧 DedupConstants 的 432-byte 自身固定帧变为 ShareBlockConstants 的 400 bytes；其自身静态代码由 5724 变为 3312 bytes。Queue/Drain/ReplaceConstantUses/QueueFollowingConstants 的自身固定帧及静态代码保持。新函数内联了候选访问并调用其他改写辅助函数，这些数值仅为各函数自身，不能推算全部编译器代码、嵌套栈峰值或动态收益。

十一份 workspace 日志全部闭合且加总一致；五组 bench 和 bound-off/fixed-class Function CoreMark 共七组完整字段相同，十组静态的六个分配/释放/峰值基本字段相同，三个 formatter roster 的其他计数保留差异。IR-slow 因 oracle 多六条轨迹，记录 343 次成对申请/释放、6677152 bytes、逻辑峰值 3981724、末尾 live/retained 为零；不以变化后的测试负载推断编译成本。十一份 scratch 容量检查通过。私有替换表、用户快照、潜在 use 图准备、队列比较与全部初始化/搬迁成本仍须接续，不以 workspace 计数和布局相同宣称总堆成本下降。

本批没有重跑 PMU；上一批相对更早参考的 Function/Block 指令截距约 +0.42%/+0.63%，尚未由本批采样证明消除。二十九项已检查结构/行为门通过，193 项原始哈希核对涉及 172 个不同路径，证据前缀 `/private/tmp/swiftvm-middle-results/r2-indexed-constant-seed-v1`；native/static 两份归档分别含 25/35 个文件并与提取字节一致。初始扫描/候选播种、无 epoch 替换表、完整动态成本、类型/state/helper effects 及其余 R1–R7 继续接续，完整 R2、正式性能和固定 FEX 三方门保持未完成。

## R3 发表索引复用与 W-write 查询归一

本批承接 r2-indexed-constant-seed-v1，在七个既有生产文件中退役五份相同的 BitCast 根解析、两份局部 terminal 递归、两份完整 W-write 分类副本和两个单行转发包装。调用方复用公共 ResolveBitCastSource、VisitTerminalValues 及目标层 WWriteRoot/HasKnownWWrite；未增加源文件、常驻字段、feature、profiling、临时 probe 或独立测试程序。生产代码净减 97 行是结构记录，不能代替成本验收。

WWriteAliases 显式区分 Raw/Resolved：输入 builder 的默认 Raw 不解析别名，也不将 BitCast 或 GetHostGPR capture 视为真实 W-write。Resolved 查询沿原投影和别名找到根，保留进入根的实际 Value 类型视图；常量完整位值、精确低 32-bit extract、U32→U64 投影、unsigned narrow load 与真实 U32 返回规则共用一份。GetHostGPR 的两类物理例外仍分开：host-publication 初筛保留原 feature/源宽度/offset/不同目标 home 条件；最终宽度复证仍要求已提交 width owner、相同 anchor 和原始 U32 capture 视图。共享纯规则不承担这些物理证明，也不把窄读视图当作高位清零。

residences 与 width_contracts 的 terminal 查询沿既有只读 VisitTerminalValues，保持同一 raw terminal 家族与调用方自己的 alias 解析。命中后不再查询后续 alias，但共享遍历仍可访问余下 terminal 结构；没有将其写为必然减少所有访问。FPR 必要契约及只读 vector lowering 也复用同一 BitCast 根解析。物理算法、两个 capture 例外分支、其余 GPR input/residence 方法、FPR/vector 主体和旧 pinned 测试主体经十一项源码比较核对，未因归一扩大资格。

FinishConstraints 在所有物理编辑完成后，复用 publication_index 的既有 u32 容量，一次筛选后按 Copy → Select → Spill、旧指针 key、原始插入序组合排序并提交写入；Transfer 排除、Copy 的 source==UINT16_MAX 条件、每家族/key 首次插入、Select result-home 发表均保持。三组临时指针 vector 和三次选择分类访问退役。正常生产先由 PreparePublications 建立完整查询索引，索引完成后拒绝新增 selection，最终过滤子集不超过原容量；手工未索引就调用 Finish 的路径仍可首次申请。组合排序不保证比较次数更少；原整块 resident_values 收集和其余窗口扫描仍存在。

新增一个永久主套件 case，十九种明确预期形状覆盖 Raw/Resolved 别名、完整/超 32-bit 常量、普通 U32/宽值、unsigned/signed load 结果、capture、嵌套投影、不符合投影资格的输入及窄 operand view。原物理宽度、mapping drift、capture/pressure、发表与发射用例保留，没有为排序注入测试专用 selection API。

最终 source 冻结 458 个路径、109 个删除路径，相邻版仅八个既有文件修改（七个生产文件和已有 pinned_gpr_plan_test.cpp）。source digest 为 `5a8dfb189526877f29085b0d06fd2286b45c9bde248f34a882d725900a8caa00`，overlay 为 `6cfbc98e8c80da55f5de7a70f28eb4ab617b6c1fc2cd7956075e91f2ce66b751`。十四个 Android 目标集中构建/冻结，最终 no work；部署九份，前后十四项设备输入哈希一致。候选 launcher 为 `aac8a410d035971804f4324175635c3e9bad252a08cbd964990197b5e6d01908`。静态参考为实际相邻 indexed-seed 的 `eabc2c3cc273ceeb283cbd9d220ab0d4032d64f7e4afb55d8bcd090adf638bf2`；PMU 则明确采用最近实测 constant-feedback-v2 的 `36734fee766073edf356ae99f2c10a206aa86eaf20400177545baba2b26379c2`，包含上一批初始 CSE 用户运输和本批 GPR 查询/暂存两组变化，不能将合并结果单独归因给本批。

红魔上集中运行二十八项定向 native，全部为新运行且退出码符合预期，包括上一批二十一项及 disk/AOT entry、AOT links、static/indirect call 与 AOT call 两种模式。其余 107 项未重复，不计为本批通过。主套件 733 cases、225471 断言，对照 732/225369，只增加所列 case 且该 case 通过；共同 case 顺序及完整失败签名相同，仍为十一例/六十六个既有失败断言、零 error。成功断言总量变化没有作单一归因。IR 快/慢均为 70 cases、25047/25131 断言，fixedclass 为 2 cases/39 断言，均零失败/error；二十四个 mode 标记与逐项最新参考相同，所选 cycle 的 rescue_interrupt 为零。

十组静态配置的模式、输出/CRC、参考日志身份均正确；非 formatter 有序记录及全部共同 PC 的记录保持：

| 模式 | CoreMark 记录数，参考 → 候选 | CoreMark RA host bytes，参考 → 候选 | bench host bytes，双方相同 |
| --- | ---: | ---: | ---: |
| Function | 426 → 426 | 286784 → 286784 | 19896 |
| Function bound-off | 426 → 426 | 286792 → 286792 | 19904 |
| 实际 Block | 2420 → 2414 | 205564 → 205276 | 16464 |
| Function fixed-class | 427 → 427 | 286548 → 286548 | 20068 |
| Block fixed-class | 2415 → 2416 | 206512 → 206428 | 16488 |

五组 bench 与三组 Function CoreMark 的完整尺寸记录、PC 顺序和 RA 形状相同；全部观察到的 spill defs/loads/stores 汇总相同。两组 Block CoreMark 差异仅在已验证 formatter 符号内，其完整同工作集 spill 门仍不据此宣布完成。记录包含尺寸/IR 数量，不写成全部机器码字节相等。较早批次 pthread_rwlock_unlock 的 2540-byte Function 和 Scudo 的 360-byte Block 保持，相对更早参考的 +68/+12 bytes 未消除。

五十项有效布局均相同，包含 ResidencePreparation 448、GPRResidenceFacts 144、GPRBlockPreparation 176、PublicationSelection 24 bytes。十一份 workspace 日志闭合且加总一致；其中 IR-slow、五组 bench 和三组 Function CoreMark 共九组完整字段相同，全部十组静态六个分配/释放/峰值基本字段相同，两组 Block formatter 的其他计数保留差异。十一份 scratch 容量检查通过。旧临时 vector、private index、别名/terminal 查询及全部初始化/搬迁/重叠峰值不在既有 workspace 统计的完整覆盖内。

| 自身静态函数 | code bytes，参考 → 候选 | 自身固定帧 bytes，参考 → 候选 |
| --- | ---: | ---: |
| FinishConstraints | 496 → 1292 | 144 → 208 |
| HasKnownWWrite | 828 → 540 | 96 → 96 |
| WWriteRoot | 原无独立函数 → 1092 | 原无独立函数 → 176 |

FinishConstraints 旧版还包含未合入本表的 outlined append/sort 辅助函数；新 WWriteRoot 替代的旧逻辑也存在于其他函数。表中不能加总为整个调用链或编译器的代码/栈峰值，也不证明新函数没有额外成本。外层帧增加 64 bytes、独立根查询帧和组合排序成本继续保留待计量，不能用删除容器或布局相同掩盖。

PMU 按 Function/Block 各三轮交错采集 2000/12000 两点，共二十四点，全部退出成功且 CRC 正确。下表斜率/截距变化是引擎系数中位数的相对变化，配对列为每轮相对变化的中位数及完整观察区间；两种统计口径分别报告：

| 模式 | 指标 | 斜率变化 % | 截距变化 % | 配对斜率中位数 % [范围] | 配对截距中位数 % [范围] |
| --- | --- | ---: | ---: | --- | --- |
| Function | instructions | +0.011196 | -0.035913 | +0.011196 [+0.010685, +0.013304] | -0.036124 [-0.044672, -0.025004] |
| Function | cycles | -0.080638 | -0.291019 | -0.080638 [-0.359126, +1.706836] | -0.673379 [-0.750856, +0.460058] |
| Function | wall | -0.096681 | -1.032271 | +1.103624 [-0.762504, +1.451714] | -1.032271 [-1.677112, +0.188866] |
| Block | instructions | +0.018527 | -0.191301 | +0.052603 [+0.018527, +0.056066] | -0.375179 [-0.489066, -0.191301] |
| Block | cycles | -0.204477 | +2.034612 | -0.204477 [-4.908985, +0.665253] | +4.423914 [-2.179344, +6.731487] |
| Block | wall | +0.872189 | +0.529005 | +1.115161 [-6.047111, +1.309315] | +2.557443 [-2.614943, +9.220470] |

walt，max 快照为 1171200 kHz；current 范围 729600–1171200 kHz，Block 第一轮 reference/2000 和 candidate/12000 两点的 after 为 729600，其他 current 快照为 1171200。cpuss 30.5–34.0°C、cpu1-0 30.9–33.6°C、cpu1-6 28.9–32.0°C。快照不证明锁频，也不能将所有周期/耗时变化归因于频率；缺少事件运行比，不报告 IPC。所有配对指令斜率观察值低于 1% 退化线；指令截距各配对轮次均下降，但 Block 周期/耗时截距中位数仍上升（约 +2.03%/+0.53%）。不追加采样平滑结果，不把混合启动/编译/runtime/formatter 的截距写成纯编译收益或正式性能改善；上一批相对更早参考的指令截距增长也未据此完成归因。

二十三项已检查结构/行为门通过，212 项原始哈希核对涉及 191 个不同路径，证据前缀 `/private/tmp/swiftvm-middle-results/r3-gpr-proof-queries-v1`。native/static/PMU/合并结果归档分别含 32/42/24/66 个文件，并与提取字节一致。本批交付共享目标查询和发表排序暂存退役；其余 fixed/spill/helper ABI、候选/窗口、完整动态成本、跨调用方适用验证及 R1–R7 继续接续。完整 R3、总目标、正式性能和固定 FEX 三方门保持未完成。

## R5/R6 共享 host-call 目标在准备阶段冻结

本批承接 r3-gpr-proof-queries-v1，退役 Function emitter 的 PrepareHostCallThunks 全段 IR 计数及其 target→uses/Label 树。新增 ARM64 compile/call_preparation.h/.cpp：封口后借用同一 SemanticFacts 的八字节 BlockSummary，只遍历 RPO 中有 CallLambda/Sse42Str presence 的块，按完整直接地址记录是否重复，输出有序目标集。公共摘要仅新增既有 mask 中的 bit1024，不持有地址、IR 或 consumer 边。

原始策略保持：只有 Function（含单块 Function）启用共享，独立 Block 和独立 HIR Block 入口不启用；完整非零直接 CallLambda 或已命中的 SSE4.2 helper 地址出现至少两次才选择，Lambda Value 即使来自 LoadImm 也不当作直接目标。未知/零目标、inline SSE 模式与不可达成员不贡献；仍统计封口 IR 的全部 raw occurrence，不因其他 action 覆盖另改阈值。decode 可做候选初筛，但 Function 单元边界、DCE 与最终 IR 尚未完成时不能确定这个共享集合，当前在最终 target preparation 中完成。

lower.cpp 在每块准备完成后、Finalize/冻结之前存入结果。UnitActions 拥有 const u64[] 和 u16 长度；空结果不申请数组，长度复用已有 header padding。开始/中止准备清空数组，查询受 building/Ready 和 allocation identity/generation/attempt 约束，过期物理结果返回空视图；没有保留独立持久子 plan、回调或可写数组。PrepareSharedHostCalls 的临时 map<bool> 和输出 vector 仍有实际成本，复制到精确数组前后也有容量重叠，不将移动阶段写成消除全部分析或分配。

JitTranslator::BindHostCallThunks 仅读取冻结地址并构建有序连续 Label 表；FindHostCallThunk 二分查询同时服务 BL 与 B，Label 的绑定和冷代码仍是可变发射状态。emitter 不扫描 IR 重新计数、不为唯一目标先建树节点、不以缺少准备结果回退到计数。原四指令目标物化和 EmitHostCall 的 ABI/保存/恢复主体保持，原直接/动态/helper 效果契约不变；同时退役一个未使用的 scalar-bit resolver，审计用 BitCast 查询复用公共实现。其余发射的动态 transport、flags、terminal suppression 和 helper 处理保留。

新增两个既有测试文件中的永久 case，没有新增测试程序、脚本、feature、profiling 或 probe。第一项静态/阶段 case 检查高 32 位不同但低位相同的两个完整目标、跨块重复、300 个后续重复、零/动态/单次目标、SSE helper/inline 模式及不可达成员；包含一个无候选可达块，确认不建 consumer/spill 图。该 case 的占位 helper 地址只用于静态选择，未执行；保存/冻结、过期查询、新准备清空和 abort 也均检查。第二项在 builder/准备工作区结束并移动 owning 结果后确认数组地址仍稳定，真实发射得到两处 BL；另分别运行 Block、单块 Function、多块 Function 的直接/动态 CallLambda 六种路径，结果均为 73。新原生执行覆盖 CallLambda；共享 SSE helper 的专项执行/完整代表性 helper 性能集合仍待后续适用验收，不能把静态选择检查写成它们都已实测。

最终 source overlay 覆盖 461 个路径、109 个删除路径；相邻版新增两个生产文件、修改十二个既有文件。helper_call_contract_test.cpp 原为未修改的 HEAD 文件，本次首次进入 overlay 的差异集合，实际是修改既有文件，不是第三个新增文件。source digest 为 `61a3c56894e8c7d2153e0ca1801d99421ae82778f1e9dbb0a2c531174da1c263`，overlay 为 `841381b1e612b70de4b99963c18a75b1d2bf5c0bc0b478999f329bf6d92ec144`。十四个 Android 目标集中构建/冻结，最终 no work；部署十三份，前后十六项设备输入哈希一致。候选 launcher 为 `d07cb505f7dd1383c76104fdc36e149a024197ef9f9cbd844843b1d7673e235b`；参考为实际相邻 gpr-proof-queries 的 `aac8a410d035971804f4324175635c3e9bad252a08cbd964990197b5e6d01908` 及其原始日志。

红魔上一次集中运行完整现有矩阵 135 项，全部为新运行且退出码符合预期。主套件 735 cases、225548 断言，对照 733/225471，仅增加所列两项且均通过；共同 case 顺序及完整失败签名相同，仍为十一例/六十六个既有失败断言、零 error。成功断言总量变化原样记录，不作单一归因。IR 快/慢各 70 cases、25047/25131 断言，fixedclass 2 cases/39 断言，均零失败/error；131 个 mode 标记与逐项最新参考相同，三个 cycle 的 rescue_interrupt 均为零。此完整 135 项也不替代未覆盖的并发、共享 SSE/AOT host-helper 及固定 FEX 三方验证。

十组静态均核对模式、输出/CRC、参考日志身份；非 formatter 有序记录及全部共同 PC 的记录相同：

| 模式 | CoreMark 记录数，参考 → 候选 | CoreMark RA host bytes，参考 → 候选 | bench host bytes，双方相同 |
| --- | ---: | ---: | ---: |
| Function | 426 → 427 | 286784 → 286876 | 19896 |
| Function bound-off | 426 → 427 | 286792 → 286884 | 19904 |
| 实际 Block | 2414 → 2415 | 205276 → 205112 | 16464 |
| Function fixed-class | 427 → 427 | 286548 → 286548 | 20068 |
| Block fixed-class | 2416 → 2415 | 206428 → 206512 | 16488 |

五组 bench 与 fixed-class Function CoreMark 的完整 PC/尺寸记录、RA 形状相同，全部观测 spill defs/loads/stores 汇总相同。四组 CoreMark formatter roster 不同，全部模式同工作集 spill 门保持未完成；尺寸记录相同不等于逐字节机器码比较。较早 pthread_rwlock_unlock 的 2540-byte Function 和 Scudo 的 360-byte Block 保持，相对更早参考的 +68/+12 bytes 未消除。

五十二项有效布局仅 UnitActions 从 848 增至 856 bytes，普通单元也承担这八字节固定 owner 成本。BlockSummary 8、SemanticFacts 624、CompileWorkspace 224、ARM64 JitTranslator 3320、HostCallThunk 16 bytes 保持；最终共享目标数组 payload 为每目标 8 bytes，发射标签记录的动态容量/Label 分配和 allocator 开销另计。不能将空数组或原计数树退役写成零总体开销。

| 自身静态函数 | code bytes | 自身固定帧 bytes |
| --- | ---: | ---: |
| 旧 PrepareHostCallThunks（emitter） | 1872 | 176 |
| 新 PrepareSharedHostCalls（preparation） | 2136 | 224 |
| 新 StoreSharedHostCalls（copy/owning） | 592 | 128 |
| 新 BindHostCallThunks（emitter） | 544 | 112 |

这些函数属于不同阶段，另有未计入的树/vector/Label 辅助函数；表中不能相加推断整个编译器代码或同时栈/堆峰值。计数从 codegen 移到既有 plan_targets 计时域后，阶段桶变化也不能直接算作收益。

十一份 workspace 日志闭合且加总一致；IR-slow、五组 bench 和 fixed-class Function CoreMark 共七组完整字段相同，全部十组静态六个分配/释放/峰值基本字段相同。四组 formatter roster 的其他计数保留差异；十一份 scratch 容量检查通过。新临时计数 map、输出 vector、冻结数组和发射标签不在这些 workspace 计量的完整覆盖内，完整私有堆及编译成本仍待收口。本批未重跑 PMU：这两组常规负载并非共享 host-call 的完整代表性性能集合；不预记阶段搬迁、容器删除或 emitter 自身帧变化为性能收益。上一批合并 PMU 的 Block 周期/耗时截距正向变化仍待归因。

二十二项已检查结构/行为门通过，187 项原始哈希核对涉及 166 个不同路径；证据前缀 `/private/tmp/swiftvm-middle-results/r5-prepared-host-calls-v1`。native/static 两份归档分别含 139/149 个文件，提取字节一致。十项源码比较保护其余 ABI、scalar-bit、translator/target 准备及旧 helper 用例；原 helper-contract 三个 case 的基线来自只读 HEAD，哈希单列，没有冒称它们曾在相邻 overlay 内。完整 R5/R6、总目标、完整成本及正式性能/FEX 三方门保持未完成。

## R6 共享 helper 的实际执行与持久化验收

本批只扩展既有 `arm64/call_persistence_test.cpp`，没有新增生产文件、测试程序、脚本、配置或 probe。被调的两块 Function 在原有加法/SaveFlags 之后跨块执行两次普通 AAPCS CallLambda，以及各两次 0x02/0x1a SSE vector helper。普通 helper 精确声明无 guest-state 观察/写入、无直接 fault 和重入，保留 host NZCV 可能改变的契约；向量输入与第一项标量结果跨块存活。测试独立计算模 2^64 的标量结果，以 abc/zbab 和 abc/abc 的四个完整 packed 常量验证 mask/index/SF/ZF/CF/OF，不调用被测 SSE evaluator 生成期望值。

实际代码扫描还逐一检查三个当前宿主完整目标各有且仅有一条 `HostImageAbs64/BranchTarget` relocation；因此不能用两个普通内联物化代替共享 thunk 通过此检查。原有加法结果、callee NZCV、static/indirect Call 的返回帧、缺失入口 fault、重载后零编译、退休/撤销以及 AOT 拒绝矩阵仍保留。新 helper 检查在实际运行后读取结果，不以静态选择或反汇编代替执行。

Android 十四个目标集中构建并冻结，只有该测试产物变化，其余十三个 SHA-256 与 r5-prepared-host-calls-v1 完全相同。v1 首轮构建发现测试把 `const void*` allocation owner 直接传给字节地址 API，未部署/运行；v2 显式转换为 `const u8*` 后构建通过。v2 source digest 为 `35fedd79add4f320dc38205fa238b286f3090abe1256a113a5aaab619a933db3`，overlay 为 `867097fc2fa5fb5f5d8be19534b62b7680ad9f6aeba2c2bc33af73fc2e497e33`；仍为 461 个路径、109 个删除路径，相邻源码只有上述既有测试文件不同。测试二进制 SHA-256 为 `07d4355566a4a41ddab8c95e2b075e1a6a17b192efd264e22749ebc2b46e9e98`；launcher 保持 `d07cb505f7dd1383c76104fdc36e149a024197ef9f9cbd844843b1d7673e235b`。

红魔 bf1359ff 上一次按影响范围集中验收八个有效组合：原六个注册模式 static/indirect/aot-static/aot-indirect/aot-producer/aot-reject，以及 SVM_IR_FAST=0 的 static/aot-static。全部通过。三个磁盘 writer/reader 的独立 PID 与实际 host image base 均不同，每个 writer compiled=2/loaded=0，每个 reader loaded=2/compiled=0，三个共享目标均重扫命中当前宿主地址；旧格式 reject 仍不发布入口：

| 模式 | writer host image | reader host image |
| --- | --- | --- |
| static | 0x650ec9c000 | 0x5eb042c000 |
| indirect | 0x5a1b3b8000 | 0x6389abc000 |
| static，slow IR | 0x5f0a818000 | 0x5dab048000 |

aot-static/indirect/slow-static 各执行四次；安装 RX 与仍存活的 source RX 不同，销毁 source 后继续执行，间接模式的两次强制缺失入口恢复及后续退休均通过。拒绝模式保持十八种拒绝、一个兼容 fallback 和一个 pristine 安装。aot-producer 保留原有真实 x86 静态 Call 用例，其三次符号调用通过；新增 helper 是公有 TranslateIR Function 的 capture/install 路径，不能声称真实 guest helper AOT producer 已覆盖。

最初还尝试额外 flags0 aot-static，候选和相邻参考均 exit=1，消息完全相同：production serializable emitter captures the actual static or indirect Call shape。现有 CTest 配置明确要求 SVM_FLAGS_REGS=1；该额外组合没有通过夹具自身形状准入，不算有效验收，也不改弱检查来凑通过。另一个预列 flags0 组合未运行。原始两份拒绝日志保留，因此本批共有九次候选尝试、一次参考诊断；“八个通过”只指上述有效组合。

没有重跑未变化产物：129 项 native 执行、十项静态执行与四份 XML 复用相邻批原路径/原始字节，逐项保留 binary/log SHA-256 与命令，并与相邻归档核对。复用主套件的既有十一例/六十六断言失败集合未冒称新运行；也没有重测 PMU、私有堆或总体编译收益。五项设备输入前后哈希相同；本批十份原始日志归档与设备哈希及提取字节一致，证据前缀为 /private/tmp/swiftvm-middle-results/r6-shared-helper-revival-v2。

本批补齐 flags1 共享 SSE/CallLambda 的执行与持久化适用验收；普通 owner 八字节成本、map/vector/数组/Label 分配、其他候选/CFG/调用方、flags0 适用覆盖、真实 guest helper AOT producer、固定源码 FEX 与正式性能仍待接续。完整 R5/R6 和总目标保持未完成。

## R5 PF token 物理后缀暂存退役

本批相邻源码为 r6-shared-helper-revival-v2；只修改 compile/lower_flags.cpp 与既有 flags_token_plan_test.cpp，没有新增源文件、API、脚本、probe 或开关。LowerFlagsTokens 原先为 31 个 GPR 各建立发表和定义 vector，收集全部成员后再 upper_bound/逐项查后缀；现在以每 home 的最后发表/定义指针、前一项 ID 与最后 clobber ID 替代这 62 个容器。PhysicalSuffix 的实际 DWARF 大小为 32 bytes，31 行局部记录共 992 bytes，不进入 UnitActions 或持久 owner。

发表资格保持：候选必须是最后一次发表，前一次发表及最后 clobber 不能晚于 producer。普通 GPR 的每个同 home 定义必然覆盖原结果，故 producer 后最多允许一个定义，且它必须通过既有 ZeroExtend32To64 wrapper/共享物理位置检查；两个及以上定义直接拒绝。早于 producer 的多次定义/发表不阻塞，BitCast 不增加物理定义。使用原先封口 IR 的严格有序 ID，稀疏跨度仍可；旧 upper_bound 已依赖同一顺序，不增加新编号假设。

一旦块中存在 preserved 定义，继续使用原有逐 capture 点、完整后缀遍历和 GPRLocationAt 证明；不能以静态 ValueGPR home 摘要替代动态驻留。候选筛选、parity/funnel demand、wrapper/Shares、点收集、actions 排序/合并及其余 carry/PSTATE lowering 经源码段比较保持。先判断实际候选再收集后缀；候选、points/actions vector、共享 use 图、preserved 扫描和必要 fixed/FPR 契约保留。这项判断依赖最终 allocation，不能在 decode 阶段承诺。

新增一项永久回归用例，二十四个完整事件 trace 有显式期望值，不用新摘要算法生成 oracle；各跑无/三项旧 home 前缀，以及从零稠密/非零稀疏编号，合计九十六种组合。覆盖单/双 wrapper、同 home 覆盖、BitCast、其他 GPR、未入候选 span 的重复/部分发表、前后 clobber。原五个 case 源码保持，包含多块 Function 无序 payload 封口与 forwarded preserved capture；Block 与 Function 的生产入口不合并。

source digest 为 `ddf59db4b170de4169a6048717f3f441736a134405ec1f9d57db9f495608cc3f`，overlay 为 `7892e3045eb8e93e66ab258b67ff6dc820123756edddf6ad75a0eb7ffc32d68d`；冻结路径/删除数仍为 461/109。十四个 Android 目标集中构建通过，部署所需九份产物，十二项设备输入前后哈希一致。候选 launcher 为 `379298de522979d4b2056e11e167879b704d8eb9a58686bcc18bf6e4ab9b6e5e`；静态参考为相邻批已确认哈希相同的 r5-prepared-host-calls-v1 原始 launcher/log，未把参考记作新运行。

红魔集中运行三十六项适用回归：主套件、IR 快/慢、fixed-class、十个 optimized/prepared/window 生命周期组合、entry 发布/缓存/链接、三个 cycle、静态 Call/返回与 latch，以及八个共享 helper 持久化组合。退出码均符合预期。主套件 736 cases、226003 断言，对照 735/225548；仅增加上述 case 且通过，共同 case 顺序及完整失败签名相同，仍有十一例/六十六个既有失败断言、零 error。总断言变化不全部归因于新增用例。IR 快/慢各 70 cases、25047/25131 断言，fixed-class 2/39，全部零失败/error；三十二个 mode 标记与逐项最新参考相同，三个 cycle 的 rescue_interrupt 均为零。这是按影响选择的集合，不冒称重跑完整 135 项矩阵或 RV native。

十组静态的输出/CRC、实际模式与参考身份均通过；全部非 formatter 有序记录和共同 PC 的记录相同，全部观测 spill defs/loads/stores 汇总相同：

| 模式 | CoreMark 记录数，参考 → 候选 | CoreMark RA host bytes，参考 → 候选 | bench host bytes，双方相同 |
| --- | ---: | ---: | ---: |
| Function | 427 → 427 | 286876 → 286876 | 19896 |
| Function bound-off | 427 → 426 | 286884 → 286792 | 19904 |
| 实际 Block | 2415 → 2412 | 205112 → 204800 | 16464 |
| Function fixed-class | 427 → 426 | 286548 → 286456 | 20068 |
| Block fixed-class | 2415 → 2413 | 206512 → 206140 | 16488 |

五组 bench 与默认 Function CoreMark 共六组完整工作集相同；其余四组只有 formatter roster 差异，全部模式的严格同工作集 spill 门继续开放。尺寸/IR 记录一致不等于逐字节机器码比较；pthread_rwlock_unlock 与 Scudo 相对更早参考的 +68/+12-byte 增长未据此消除。

LowerFlagsTokens 自身静态代码从 6556 降至 5708 bytes，自身固定帧从 1920 降至 1312 bytes（少 608 bytes）。这不包含其调用的 helper、同时栈/堆峰值或整体编译时间，不据此声明整体性能改善。十组静态 workspace 六个分配/释放/峰值基本字段相同，五组 bench、默认 Function CoreMark 和 IR-slow 共七份完整字段相同；十一份日志均闭合且加总一致，十一项 scratch 容量检查通过。IR-slow 保持 343 次分配/释放、6677152 bytes、peak 3981724、零 live/retained；这些计量不覆盖全部私有容器，完整堆/编译收益未实测。

本批不重复 PMU。九项源码段比较和二十二项结构/行为检查通过，167 个不同路径的原始哈希核对有记录；native/static 归档分别含 40/50 个文件，与设备及提取字节一致，证据前缀为 /private/tmp/swiftvm-middle-results/r5-flags-token-suffix-v1。八字节 UnitActions owner 增长、其余候选/暂存、正式性能与固定源码 FEX 三方门继续接续；完整 R5/R6 和总目标保持未完成。

## R6 终结位置发表准备与实际寄存器恢复

本批从 r5-flags-token-suffix-v1 接续，新增 compile/location_preparation.h/.cpp，退役 jit/TerminalLocationPublication 的 Prepare、Defers、候选全段扫描和 deferred unordered_set。原 Prepare 调用 context.X，这个接口在 MEM 情形可租 scratch/发出 reload，不是纯位置查询。新选择只用 AllocationView，JIT 对象现在只管理实际恢复标签与冷代码；没有保留扫描 fallback、兼容入口、probe、配置或新测试程序/脚本。

八字节 BlockSummary 在原始基础访问中增加 SetLocation 与 CallReturn/PopRSB 两个 presence，分别排除空家族和保留原先“块内有 call/pop 即必需”的选择规则，不把 PopRSBHint terminal 本身混同为 body PopRSB。新增比较仍有基础扫描成本，摘要不持有目标寄存器/标签/消费图。仅 Function 的 RPO 可执行成员参加；支持 Invalid/ReturnToDispatch/PopRSBHint，反向跳过连续 CallReturn/PopRSB 定位最后 SetLocation，普通指令会截断，立即数与不可达成员不贡献。

Finalize 完成物理编辑封口和必要证书后，对有效 Function 执行选择。按块最后 IR ID 查询 WholeValue，包含尾部元数据后的名义消费点；直接 GPR 或已规划 reload register 可以分组，饱和两次计数避免回绕。body call/pop 的单次必需选择不依赖是否已知寄存器；普通 MEM 若只有依赖实际转发状态的 scratch，则使用常规发表，不预先租用或猜测。与旧的无 consumer context.X 不保证对所有可能输入选中完全同一集合；这是显式的最终位置/保守可选策略，不能把它描述成只挪函数。

UnitActions 复制持有排序 const u16[] 与长度，每选中 ID 两字节；无项不分配数组。只读查询检查 SetLocation opcode、实际块和 allocation 身份/代际；开始/abort 清空，冻结后禁止写，错代际返回否。u16 长度复用 header padding，但 owning 指针使 UnitActions 从 856 增到 864 bytes，普通单位也承担八字节。候选/输出 vector、精确数组复制时容量重叠与 allocator 开销仍存在，不由空 payload 推断零总体成本。

发射 SetLocation 仅读取冻结选择：常规路径立即发表，延迟路径暂不 materialize 目标，只记录值与编码状态。TickIR 将选中项作为 deferred consumer，使用既有 backing 规则；因此不能只把生产者的最后一次 IR 使用转发给这个不实际读取的指令而丢失槽内容。EmitBlockTerminal/间接转发仍使用真实 scratch 状态；恢复标签在实际转发读取目标时按实际寄存器创建，CallReturn/PopRSB 后的寄存器变化不靠早先标签猜测。原 L1/continuation/call 编码 ABI、冷发表代码及 JitContext spill 本体没有改变；候选与标签的职责已分开，未引入 Bind/replay。

既有 block_static_lowering_plan_test.cpp 增加三个永久 case。静态 case 覆盖十八种完整期望选择：重复/单次、两种必需 metadata 及前后位置、截断、非支持 terminal、立即数、被替换的早期位置、不可达成员、planned reload、未知普通 scratch、preserved 在尾部消费点与单元末位置不同、indirect_l1 关闭；不构建 consumer/spill 图。复制结果释放源 vector 后仍可查询，冻结写入、过期、新准备与 abort 也检查。第二项释放 builder、移动 owning prepared unit 后实际发射。第三项保留真实分配/准入并强制目标经过 MEM：普通发表、前置 PopRSB、后置且消费目标的 PopRSB、后置但不消费目标的 metadata 四种路径真实运行，均到达目标返回 73，必需恢复偏移就位。它们没有在旧生产代码上运行，不冒称新用例已观察到旧版失败；专门 MEM target 缺失入口 fault/AOT/并发 sweep 仍开放。

v1 集中矩阵在新增静态 case 的 scenario 14 发现夹具漏声明固定 GPR 池，主套件是原六十六失败断言加一项夹具 error；保存该次源码/二进制/十份日志/XML 归档，不接受它。修正为声明 20/22 池，另在最终运行前补入不消费目标的第四种 transport。v2 只构建/冻结，未运行 native；最终为 v3。三者生产源码相同，十四份产物中只有主测试二进制变化。先前六项已通过的 IR 快/慢与 optimized Block/Function fast0/1 在核对实际执行 ELF 与 v3 等价产物 SHA 后复用原命令/原文件名和字节；失败的 v1 主套件不复用。

最终源码覆盖 463 个路径、109 个删除路径，相邻版新增两个生产文件、修改十四个既有文件；source digest 为 `78a1b3dd454ddbfac3b0d5111688e922eca18fb79d1914bca2aa2e8ab4a26b7d`，overlay 为 `e02a33f18b10f97fd02ddaec454fc133ac42fa1b6f0f93c4c95c820120621eff`。十四个 Android 目标构建/冻结完成，部署十三份，十六项设备输入前后哈希相同。候选 launcher 为 `6deeb80ac4a2e2b45f9de6b8c6689bd321968f93fc0fdb6d1846b895a0ab1bbe`；主测试为 `64a0c322bcc99053035b93f435a877f02c96535af1d4f39b3b7b082321171fbf`。

红魔完成原完整 135 项加两个 slow-IR helper 组合，共 137 项：131 项新运行、六项上述精确复用，全部退出码符合预期。主套件 739 cases、225959 断言，对照 736/226003；仅新增所列三项且均通过，共同 case 顺序及完整失败签名相同，仍为十一例/六十六个既有失败断言、零 error。成功断言总量变化原样记录，不全部归因于新增 case。IR 快/慢各 70 cases、25047/25131，fixed-class 2/39，零失败/error；133 个逐项最新 mode 标记相同，三个 cycle 的 rescue_interrupt 均为零。既有 latch/fault、退休/并发、关闭、缓存/AOT 与 call-return 验证不替代仍开放的特定 MEM target 恢复或固定 FEX 门。

十组静态输出/CRC、模式与参考日志身份均通过；全部非 formatter 有序记录、全部共同 PC 记录及观测 spill defs/loads/stores 汇总相同：

| 模式 | CoreMark 记录数，参考 → 候选 | CoreMark RA host bytes，参考 → 候选 | bench host bytes，双方相同 |
| --- | ---: | ---: | ---: |
| Function | 427 → 426 | 286876 → 286784 | 19896 |
| Function bound-off | 426 → 427 | 286792 → 286884 | 19904 |
| 实际 Block | 2412 → 2416 | 204800 → 205184 | 16464 |
| Function fixed-class | 426 → 426 | 286456 → 286456 | 20068 |
| Block fixed-class | 2413 → 2413 | 206140 → 206140 | 16488 |

五组 bench 与两组 fixed-class CoreMark 共七个完整工作集相同；其余三组差异只在已识别 formatter roster。全部模式的严格同工作集 spill 门仍未完成，尺寸记录也不是逐字节机器码证明。较早 pthread/Scudo 的 +68/+12-byte 增长未消除。

五十五项有效布局仅以下三项变化；BlockSummary 8、BlockActions 12、EdgeBlockAction 和其余准备/物理类型保持：

| 对象 | 参考 bytes | 候选 bytes |
| --- | ---: | ---: |
| UnitActions | 856 | 864 |
| ARM64 JitTranslator | 3320 | 3280 |
| TerminalLocationPublication | 296 | 256 |

| 自身静态函数 | 参考 code / 固定帧 bytes | 候选 code / 固定帧 bytes |
| --- | ---: | ---: |
| 旧 TerminalLocationPublication::Prepare | 1628 / 368 | 已删除 |
| PrepareDeferredLocations | 无 | 1716 / 272 |
| StoreDeferredLocations | 无 | 612 / 128 |
| DefersTerminalLocation | 无 | 292 / 64 |
| EmitSetLocation | 824 / 160 | 800 / 144 |
| EmitIndirectForward | 548 / 144 | 580 / 144 |
| EmitContinuationForward | 536 / 112 | 568 / 112 |
| EmitIndirectCallForward | 744 / 176 | 772 / 176 |

它们属于不同对象/阶段及调用关系，不能把表格直接相加推断整体代码、同时栈/堆峰值或编译收益。十组静态 workspace 六个基本分配/释放/峰值字段相同；七个同工作集静态与 IR-slow 共八份完整字段相同，十一份日志闭合、加总一致，十一项 scratch 容量检查通过。IR-slow 保持 343 次分配/释放、6677152 bytes、peak 3981724、零 live/retained。新私有候选/数组/Label、摘要新增比较及实际查询成本不由这些工作区计量完整覆盖。

本批未重复 PMU。二十七项源码比较和三十项结构/行为检查通过，491 个不同路径哈希有记录；最终 native/static 两份归档含 141/151 个文件（含保留原名的复用证据），另有十文件 v1 失败归档，均核对设备与提取字节。证据前缀为 /private/tmp/swiftvm-middle-results/r6-terminal-location-preparation-v3。完整 R5/R6、总目标、完整私有成本、正式性能与固定源码 FEX 三方门保持未完成。

## R6 自然压力下的间接目标溢出、缓存与 AOT 恢复

本批 r6-spilled-call-revival-v2 只修改既有 source/tests/arm64/call_persistence_test.cpp，生产源码与上一批完全一致；没有增加测试程序、仓库脚本、分配钩子或发布接口。间接 Source 在压力输入前计算 LoadUniform(32) XOR LoadUniform(56) 作为真正的 SSA 目标；salt 在运行前置零，不能由编译器常量化为 guest-home alias。随后先读取四十八个不同 U64 输入，再进行求和、逐项 XOR 和从全一值减去全部输入三种独立归约。输入各有三次使用，目标有两次晚使用，真实分配器按既有策略将目标溢出。三项输出都可观察并独立校验，sum/difference 使用算术级数闭式期望；统一 uniform 窗口从 128 增为 528 bytes，属于测试夹具。

扩展原有缺失入口信号观察器，在 SIGSEGV/SIGBUS 时只读取当前 State 的固定六十四个 u64 spill 槽，保存匹配目标值的数量。每个 Runtime 执行前先确认槽中不存在该目标，压力输入均高于 32 位且与 guest PC 不同。只有真实缺失入口 fault 才要求观察到目标 backing；随后继续核对 LR−4 的精确 IndirectCallMiss 记录、实际调用返回、独立 callee NZCV、guest return witness、共享 scalar/SSE helper 输出及外部帧平衡。该观察证明 fault 时确实存在目标的 spill backing，不识别具体 Allocation 槽号，也不区分 planned reload register 与未知可选 scratch；后一边界由前批强制 MEM transport 用例补充，完整 sweep 仍开放。

红魔 NX729J（bf1359ff）集中运行十项，全部通过：六个既有注册模式，以及 static、aot-static、indirect、aot-indirect 的 SVM_IR_FAST=0 组合。两个间接磁盘组合各在编译 writer 和独立 exec reader 中完成四次调用、两次强制 miss，reader 均 loaded=2/compiled=0；所有四个磁盘组合的 write/read/reject PID 不同，writer/reader host image 地址不同，helper_relocs 均为三。两个间接 AOT 组合各完成四次调用、两次强制 miss；仍验证 source RX 与安装 RX 同时存在时地址不同、source 销毁后执行，以及目标失效。合计十二次真实缺失入口 fault 都观察到目标 spill backing。旧格式缓存拒绝、十八项 AOT 拒绝/兼容回退及真实 guest AOT producer 原用例也通过；快/慢路径没有混同为 Block 分配验收。

十四个 Android 目标构建通过，十三个 ELF 与 r6-terminal-location-preparation-v3 的 SHA 完全相同，只部署新 call 测试。保留原命令、文件名、实际 ELF 哈希与归档字节，精确复用上一批非 call 的 129 项 native 结果及十组静态结果；本批新运行十项，合成适用条目为 139，未重跑主套件、静态或 PMU。原主套件已知十一例/六十六个失败断言仍属于复用参考，不能把这次十项全通过描述为主套件零失败。十份新日志与 151 份复用静态归档文件均校验提取字节；新 native 归档 SHA 为 `5770a5acf7d36b1ea31863a8cc268c3bfe0cba72c3e4b86998e15ebcc11a8a7e`。

首轮 v1 只完成 static，通过；indirect 的新增 backing 断言失败，因为单次使用的压力输入比两次使用的目标便宜，目标没有实际溢出。该次源码/ELF/两份原始日志及归档保留，不接受为覆盖。v2 将三种独立归约合并进同一夹具后才完成十项；两份失败阶段日志归档 SHA 为 `7631bc70140827588ee4b9fc3045bfc6897d7cbe5d850ce094d98ef9a8bb6852`，没有重复运行无关模式。

最终 source digest 为 `3eba48fecd0899180de8b5aa1d7eeee73935e567d2ce41ebd3ceef3cda952985`，overlay 为 `e8abe2b7ee72571a9a44a67acb6de50ec82c22e380fd94c9a20eee50dbed368a`；仍为 463 个源码路径与 109 个删除路径，新增/删除路径均为零。call 测试 ELF 为 `41cbf7ebaaa53c4ff5adcec3ee74efb60fe8f8da253f0464362d6492c3166c9e`，launcher 仍为 `6deeb80ac4a2e2b45f9de6b8c6689bd321968f93fc0fdb6d1846b895a0ab1bbe`；三个设备输入前后哈希相同。证据前缀 /private/tmp/swiftvm-middle-results/r6-spilled-call-revival-v2。flags0、独立 Block 的持久化溢出目标、完整取消/准备/发布/运行并发、剩余私有成本与正式固定源码 FEX 三方性能门仍未完成，完整 R5/R6 和总目标不据此关项。

## R3/R2H 终结 Value 遍历共享与 scratch 递归清理

r3-terminal-use-sharing-v1 接续已接受的 r6-spilled-call-revival-v2，只修改 compile/passes/loop_hoist.cpp、compile/allocate/scratch.cpp 和既有 tests/ir_contract_test.cpp。hoist 的函数使用摘要、跨块活跃性和压力末次使用三处改用 analysis/uses.h 的 VisitTerminalValues，删除本地 TerminalUses 模板。保持 If 条件、Switch selector、分支、Condition 和 CheckHalt 的原次序及重复 occurrence；仅访问原始 terminal Value，不追加 hoist anchors、不统一别名解析。ScalarRoot 在整数/浮点 BitCast 之间停止的判断及其余提升资格、helper/effects、压力、CFG 与元数据所有权原样保留。

scratch 的 CountTerminalReloads 使用同一只读遍历器，删除 std::function 递归以及按值接收 terminal 分支的 visitor；后者会复制包含递归终结结构的分支，Switch 还涉及其 cases 容器。IsSpilled、ResolveBitCastSource、根定义返回类型判定 GPR/FPR 的规则及必要 headroom 保持。terminal 仍按每个 occurrence 累计，包括重复同值；逐 IR 指令的去重 reload 策略独立，不能将二者合并。两份完整生产文件在仅替换上述遍历与 include 后进行忽略空白比较，八项源码身份/边界检查通过；没有增加文件、配置、API、probe 或测试脚本。

既有嵌套终结契约 case 增加四个断言：同一个 If/Condition/Switch/CheckHalt 结构，真实根分别映射 GPR/MEM，selector 的 BitCast 节点单独有 GPR 映射；期望根在 MEM 时三个 occurrence 都计入 GPR reload，根在 GPR 时为零，FPR 均为零。该期望不由公共遍历器生成，没有新 case；原始视图/稀疏编号检查继续执行。既有提升压力、只读 helper、跨块活跃性和 owning metadata 测试保持。

十四个 Android 目标构建通过，在红魔 bf1359ff 部署九个需要运行的产物；九个 ELF 与两个 guest 共十一项前后哈希相同。集中运行三十八项 native，全部退出码符合预期：主套件 739 cases、225984 断言，对照 739/225959，仍为十一例/六十六个既有失败断言、零 error；全部共同 case 顺序和完整失败签名相同。主套件总断言增加二十五未作归因，不归入只在 IR 程序新增的四个断言。IR 快/慢各 70 cases、25051/25135 断言，比各自参考增加四，零失败/error；fixed-class 2/39 亦零失败/error。三十四个 mode 标记与各自最新参考一致，三个 cycle rescue_interrupt 均为零；十个 call 持久化组合继续通过，十二次目标 spill-backing fault 也保持。该选择集合不等同于完整 native/RV 或取消/发布并发矩阵。

十组新静态运行的 CRC/五项 bench checksum、实际 Block/Function 模式与参考日志身份通过。全部非 formatter 有序记录及全部共同 PC 记录相同；七组完整工作集相同，另外三组 CoreMark 的 formatter roster 不同，观测 spill defs/loads/stores 汇总仍全部相同、target_overflow 为零：

| 模式 | CoreMark 记录数，参考 → 候选 | CoreMark RA host bytes，参考 → 候选 | bench host bytes，双方相同 |
| --- | ---: | ---: | ---: |
| Function | 426 → 426 | 286784 → 286784 | 19896 |
| Function bound-off | 427 → 426 | 286884 → 286792 | 19904 |
| Block | 2416 → 2418 | 205184 → 205608 | 16464 |
| Function fixed-class | 426 → 426 | 286456 → 286456 | 20068 |
| Block fixed-class | 2413 → 2412 | 206140 → 206128 | 16488 |

以上相同记录不是逐机器字节证明，三个不同 formatter 工作集也不据汇总 spill 当成完整同工作集门。十一份 workspace 日志均闭合并加总一致，十组静态的基本六字段相同；七组完整工作集与 IR-slow 合计八份完整字段相同。IR-slow 保持 343 次分配/释放、6677152 bytes、peak 3981724、零 live/retained；十一项 scratch 容量检查通过。std::function/终结容器复制及 hoist 私有摘要的实际堆/栈成本不由这些工作区字段完整覆盖。

仅测 launcher 中列出函数自身的固定帧/代码：CountTerminalReloads 为 232/128 → 108/96 bytes；HoistLoopInvariants 为 7936/848 → 7948/864；FunctionSummary 构造为 9944/480 → 9960/480。hoist 自身固定帧增加十六、两处 hoist 代码各有增长，完整调用栈、helper/template 代码总量及动态峰值仍开放，不将 Count 减少抵销成总体收益。本批没有 PMU 或正式性能结论。

source digest 为 `87c34f8e45f341de62580e11b34689348ce27b81f8d4866a5e8c581da454a4c3`，overlay 为 `09fcd09f16777266d8c200b52aa1fa0d55206872d3e66f911e227cf30d73ee90`，仍为 463 个路径/109 个删除路径，本批无新增/退役路径。launcher 为 `ce1c03551b191a66ffbd566d3e0c2e690bc7b12049b80766337b9b1933b7a683`，主测试 `29ef27a35c24d8df1379d6a29a65acda98f8da1c5f8d8a7b8d0f43eb753a0158`，IR `fd764a021c10921ebafee7a60dd7a443f32870c8faef66a047c68f5f91edfb42`。native 归档含四十二份日志/XML，SHA `e494c6a4309c731959d4f70b272258dd42a98d619652d58c8c5b7855f2fcb659`；合并 static 归档含五十二份，SHA `09f3a32dbf35efa2e2355302277e12bb1a9898089bf582b715eb809716e6de31`。证据前缀 /private/tmp/swiftvm-middle-results/r3-terminal-use-sharing-v1。完整 R1/R3/R5/R6、总目标、私有成本和固定源码 FEX 正式三方门保持未完成。

## R2 名册构建时播种初始 value/width 队列

r2-roster-value-seed-v2 接续 r3-terminal-use-sharing-v1，只修改 value_worklist.cpp/.h 与既有 ir_contract_test.cpp。构造名册时直接为全新 eligible 行写入首阶段 Simplify 任务，最后反转既有 u32 队列，保持原先 reverse-row 播种后从前向后的栈处理顺序。All/Values 首阶段仍为 Values，Widths-only 直接准备宽度任务；构造只建立身份与队列，不改 IR。不可达行仍只保留删除身份、不入优化队列，稀疏 IR ID 不参与排序；8N+32 预算仍按实际名册行数在构造结束时设置。没有增加 owner 字段、ValueRow 字段、工作区缓冲、配置、兼容入口、probe 或测试程序/脚本。

删除 Run 中首阶段的完整 Seed 扫描；原后续播种收为 SeedWidths，仅 All 的 value/CSE 完成后再执行，继续发现新宽度候选并保留已有反馈去重次序。初始任务不需要通用 Queue 的共享标记、heap/多原因转换；通用反馈 Queue、Drain、DCE/flags 通知和 scalar/vector CSE 主体不变，必要 DCE 不受可选预算截断。候选资格预查与初始逐块有序 CSE 扫描仍存在；每行候选判断和所选任务的反转也有成本，不能把这一步描述成全部播种或普通工作均已消失。

既有 Block/Function 传播和 flags 用例合计增加六个断言：构造后确有待办任务，原始输入仍未被改写；随后沿原 Run、DCE/flags 编辑和 Drain 流程验证结果。IR 快/慢的完整有序 CSE 参考、控制边界、重复不延长窗口、跨块导出、原始宽度、无候选零 roster、异常/工作区复用用例继续执行，没有新增 case。八项源码比较证明，仅上述首次播种/阶段交接不同；字段、预查、值/宽度/CSE/DCE/flags 规则和 pipeline 源码保持。

v1 先用通用 Queue 初播，三十八项 native 与十组静态通过；代码检查发现 BuildRoster 自身由 1996 增为 3300 bytes、固定帧 240 → 256。此时尚未运行 PMU，保留源码/ELF/原始日志及两份归档，不接受为本批最终结果。v2 使用全新行的直接任务准入后才运行最终矩阵和 PMU；v1 原计划中的二十四项 PMU 没有被记作实测。两版没有 native 正确性失败，不将这个尺寸修正描述成故障修复。

十四个 Android 目标构建通过，红魔 bf1359ff 部署九个需要执行的产物，九个候选 ELF、两个 guest 与一个 PMU 参考 launcher 共十二项设备输入前后哈希相同。最终三十八项 native 退出码符合预期：主套件 739 cases、225828 断言，相邻参考 739/225984；仍为十一例/六十六个既有失败断言、零 error，共同 case 顺序与完整失败签名相同。主套件断言总数减少一百五十六原样记录、未作归因；新增六个检查仅位于 IR 程序，不用来解释该变化。IR 快/慢各 70 cases、25057/25141 断言，比参考各增六，零失败/error；fixed-class 2/39 零失败/error。三十四个 mode 标记相同，三个 cycle rescue_interrupt 均为零，十二次真实 spill-target fault 继续通过。

十组新静态的输出/CRC、五项 bench checksum、实际模式与参考原始日志身份通过。全部非 formatter 有序记录和全部共同 PC 记录相同，全部观测 spill defs/loads/stores 汇总相同、target_overflow 为零。七组完整工作集相同，三组 CoreMark formatter roster 不同：

| 模式 | CoreMark 记录数，参考 → 候选 | CoreMark RA host bytes，参考 → 候选 | bench host bytes，双方相同 |
| --- | ---: | ---: | ---: |
| Function | 426 → 426 | 286784 → 286784 | 19896 |
| Function bound-off | 426 → 427 | 286792 → 286884 | 19904 |
| Block | 2418 → 2414 | 205608 → 205276 | 16464 |
| Function fixed-class | 426 → 426 | 286456 → 286456 | 20068 |
| Block fixed-class | 2412 → 2416 | 206128 → 206428 | 16488 |

十一份 workspace 日志闭合、加总一致，十组静态的分配/释放次数及字节、live/peak 六字段相同；七组完整工作集与 IR-slow 合计八份完整字段相同。IR-slow 仍为 343 次分配/释放、6677152 bytes、peak 3981724、零 live/retained；scratch 的十一份容量检查通过。没有新增缓冲不等于完整初始化、搬迁及私有成本门完成，formatter 工作集差异也不由 spill 汇总代替完整同工作集门。

launcher 中仅计自身代码/固定帧：BuildRoster 1996/240 → 1440/240 bytes；构造 336/128 → 344/128；BeginValueCleanup 1152/160 → 1224/160；通用 Queue 628/144 保持。最终没有独立 Run 符号，源方法仍在并由调用端内联；SeedWidths 自身为 272/112，参考 Seed 没有独立符号，不能将二者当成同一外部函数直接比较。辅助模板代码、完整调用栈与同时堆/栈峰值未由这些数值证明。

另集中运行二十四项两版 PMU：Function/实际 Block、2000/12000 两个固定工作点、三次配对，重复间交替版本及工作点次序；使用 simpleperf stat --cpu 4 的 cpu-cycles/instructions 与 taskset 10，JIT cache 为空。仅参考相邻冻结 launcher `ce1c03551b191a66ffbd566d3e0c2e690bc7b12049b80766337b9b1933b7a683`；未加入来源尚未确认的设备 FEX。斜率为两工作点差除以一万，截距为短点减两千倍斜率；以下为逐配对百分比变化的中位数：

| 模式 | 指令斜率 | 指令截距 | 周期斜率 | 周期截距 | 耗时斜率 | 耗时截距 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Function | +0.132540% | −0.296204% | +0.167111% | +0.857698% | +1.150529% | +0.951461% |
| Block | −0.036549% | −0.605752% | −0.381076% | +2.099989% | +0.942161% | +0.810161% |

三次配对的每个原值、斜率/截距及百分比全范围均保留在 pmu-summary.json。例如 Function 指令斜率范围 +0.007324% 至 +0.740568%，Block 指令截距 −0.856356% 至 +0.121652%，Block 周期截距 −3.827326% 至 +3.988992%；没有只报告有利方向。采样前后 governor 均 walt，频率快照 614400–1171200 kHz，电池温度 27.6–27.7°C；没有锁频，电池温度不能代替 CPU thermal trace。工具未报告事件运行/启用时间比例，不计算 IPC 或声称事件缩放已验证。全部 CRC 为 2000 点 0x4983 / 12000 点 0xd340，guest 的 ERROR!/Errors detected 仅对应不足十秒的既有时长检查，不能用作正式 CoreMark 分数。截距包括启动/loader/运行时等，不能等同纯编译成本。指令截距中位数下降不构成整体收益证明，周期/耗时的正向变化继续保留待归因。

source digest 为 `1f74708329d1f2201907663936ab3d1f2162e60761ce4076c8b26492c7a38d37`，overlay 为 `ab70398955f2e9c5d6e3840015ea7fdeb675a264e19c0f0a3df442c7c121b0d3`，仍 463 个路径/109 个删除路径，本批无新增/退役路径。最终 launcher `c42ddf3d6870ca68d30b6c24504fd93a18be7752f5871fecb15af5591e8cdfcb`，主测试 `3c47e8d1517c121c856edcaf4f18d37b683f588aa393c7812f649604d408b942`，IR `c192300fc1745af4bb5e648719b473f697f311c0202415b46a3921f2f7f7e5b3`。三份 native/static/PMU 归档含 42/52/72 个文件，SHA 依次为 `0340a106ae100706cd80d95a0b173b5382856d756bc45940b603690d397fdf02`、`185e9a6e87d203d022fe4f764a1fc742eb4baca1afdf9b80755ba22b7ba1a9e2`、`fc59e1105e2c95dd17e45f8abca568fc33676e9b365a07b84799f51c4d1a2c62`；证据前缀 /private/tmp/swiftvm-middle-results/r2-roster-value-seed-v2。完整 R2、完整私有成本、总目标、并发/RV 矩阵与固定源码 FEX 正式三方门保持未完成。

## R6 Div128 选择规则收口与旧 JIT 分析类退役

r6-div128-selection-v2 接续 r2-roster-value-seed-v2，退役 backend/arm64/jit/div128_narrowing_analysis.cpp/.h、Div128NarrowingAnalysis 及两个 CMake 源项；compile/scratch 不再依赖这份 jit 头。生产发射原本已消费准备好的 div128_native 位，本批完善规则归属和输入边界，不能描述成首次将 Div128 静态决定移出 emitter。规则归 lower_scalar.cpp 的纯 SelectNativeDiv128；正常 LowerScratch 传入本块共享 const BlockAnalysisIndex，使用 InstructionPosition 的真实指针/编号核对，从既有有序 roster 向前查询 uniform 写窗口，不访问 Block 的 intrusive iterator、不补建直接/resolved 图。

共享核心保留原 Overlaps、十六层解析上限、BitCast/无运算 GetOperand、无符号 high=0，以及带符号 high=AsrImm(low,63) 且 source/low 根 Def 相同的判定。最近重叠写必须 offset/size 完全匹配；部分写、UniformBarrier、Goto/NotGoto/BindLabel、三种 Call、X87Op 与 Sse42Str 继续截断。尚未用更宽松的 helper effects 替换该保守边界，effects 的进一步统一另行接续。十四项源码比较核对核心规则、原 scalar 逻辑、scratch 调用交接、CMake 删除和其余源码身份；类型参数改为有效 Inst 引用，旧 nullable 指令接口随类退役。

公开手工编码准备经 PrepareManualDiv128 显式核对当前 Block 的节点身份，再访问该 load 的真实前缀；blockless 情形只做 SSA 证明。其成员查找属于手工准备，正常 selector 没有这个 fallback；没有为手工调用临时建立完整 consumer 图。两条入口共用相同的 uniform/SSA/最终 native 规则，不保存持久分析对象。结果仍沿原 InstructionScratchFacts.div128_native 位冻结；ScratchRange、UnitActions、JitContext/emitter 和公共 compile 层的结构/源码未改，没有新增 owner、payload、文件、probe、配置或测试程序/脚本。

沿用 spill_forwarding_test.cpp 的三个既有 Div128 case。实际 Block 执行由四组增至六组，新增带符号 −100/7 的直接与 StoreUniform→LoadUniform 形状，期望 quotient=−14、remainder=−2，并核对 native sdiv/udiv 数量与准备位。原 unsigned wide/control-join、部分 store、barrier 与同 ID 的外块 uniform 负例保留；直接旧类查询已改为实际冻结位或新 selector。另以 signed/unsigned、BitCast/GetOperand、15/16/17 层的十二种显式输入，覆盖包含边界的解析上限、稀疏编号和 blockless 手工结果，并检查 selector 不创建 consumer 图；alias 期望不由被测匹配器生成。本批新增的真实 Div128 执行属于 Block；通用单/多块 Function 准备和 owning 用例继续运行，专门完整 Function/故障/AOT 家族矩阵仍开放。

v1 因模板中 auto 指针类型依赖造成 GetArg 解析编译错误，修正为显式 const ir::Inst* 后才形成 v2。失败后误归档的十四个旧输出已逐一确认与前版哈希相同，副本全部移除，清单改标 stale-output-copies；未部署或运行，保留失败源码、诊断和作废清单。v2 构建成功后冻结十四个 Android 目标，部署所需九份；九个 ELF 加两个 guest 共十一项设备输入前后哈希相同。没有把 v1 旧输出作为候选或复用结果。

红魔 bf1359ff 集中运行三十八项 native，退出码符合预期。主套件仍为 739 cases，226137 断言，对照 739/225828；共同 case 顺序和完整失败签名相同，十一例/六十六个既有失败断言、零 error。修改的三个 Div128 case 均通过，断言总量增加三百零九未全部归因于本批新增检查。IR 快/慢均 70 cases、25057/25141 断言，fixed-class 2/39，零失败/error；三十四个 mode 标记相同，三个 cycle rescue_interrupt 为零，十二次真实 spill-target fault 继续通过。未据该选择集合声明重跑完整 native/RV/并发矩阵。

十组新静态的模式、CRC/bench checksum 与参考原始日志身份通过；全部非 formatter 有序记录、全部共同 PC 分组记录、观测 spill defs/loads/stores 汇总相同，target_overflow 为零。六组完整有序记录相同；Function bound-off CoreMark 的完整 PC/记录多重集合相同，formatter 内部顺序不同；另外三组 CoreMark formatter roster 不同：

| 模式 | CoreMark 记录数，参考 → 候选 | CoreMark RA host bytes，参考 → 候选 | bench host bytes，双方相同 |
| --- | ---: | ---: | ---: |
| Function | 426 → 427 | 286784 → 286876 | 19896 |
| Function bound-off | 427 → 427 | 286884 → 286884 | 19904 |
| Block | 2414 → 2418 | 205276 → 205576 | 16464 |
| Function fixed-class | 426 → 426 | 286456 → 286456 | 20068 |
| Block fixed-class | 2416 → 2414 | 206428 → 206612 | 16488 |

十一份 workspace 日志闭合且加总一致，十组静态分配/释放次数、字节及 live/peak 六字段相同；七份静态完整字段与 IR-slow 合计八份完整字段相同。IR-slow 保持 343 次分配/释放、6677152 bytes、peak 3981724、零 live/retained；十一项 scratch 容量检查通过。有序序列、多重集合和观测 spill 汇总分别记录，不把 formatter 差异当成全部同序列门已完成，也不由记录相同推断机器字节完全相同。

仅计 launcher 中函数自身的代码/固定帧：LowerScratch 3164/352 → 3160/336 bytes，PrepareInstructionScratchWithBlock 384/112 → 380/96。旧 CanLowerNatively、Resolve、ResolveUniformLoad 分别 1364/160、412/128、1028/176；新 SelectNativeDiv128 与 PrepareManualDiv128 各为 1376/144。入口和内联/辅助函数划分已变，不把这些数值加总成整体代码或同时调用栈/堆峰值收益。本批不重复 PMU，上一批 PMU 也不作为本版 ELF 的新实测；完整成本与正式性能门保持开放。

源码仍无新增路径，修改五个既有文件、退役两个；冻结 461 个路径/111 个删除路径。source digest 为 `c0f18bbda026c29e4cae6675807981b8af5b49a32451a92c15c242f9d9c791b9`，overlay 为 `c1191defd03f08a9ea698f4e4b3d477253c1ac12aadf7658fe429ec4d45f1db1`。launcher `a2c1f14257ac36c70c46cb8f23a28859f77650f9dd188c1418ebb000fcdce212`，主测试 `1d02ccbca78255ced1cbaff2863cfd459785e227038211e2a4b40fb1e341ebbf`，IR `facb585e04ce6c0db4168616b31bc05c32256f4a1897fe156be76689d04b0f7d`。native/static 两份归档含 42/52 个文件，SHA 分别 `c9e1b619a708110ecef87ff27eb06a2f2f44f4184349865ace9a377491df3103`、`1be4c0e936734aede021314d69120e852e12fb5bae750ddac2a6c1d3128b7f23`；证据前缀 /private/tmp/swiftvm-middle-results/r6-div128-selection-v2。完整 R5/R6、总目标、私有成本、Function/恢复/RV/并发与固定源码 FEX 正式三方门仍未完成。

## R6 Function 宽除法恢复与 pair-call trampoline 重绑定

r6-function-div128-revival-v2 接续 r6-div128-selection-v2。最初 v1 仅扩充既有 call_persistence 夹具，两条磁盘/AOT 模式均在捕获阶段失败：constant branch target outside the host image。真实宽除法通过运行时生成的独立 trampoline 调用 helper，旧代码将该地址绝对物化，原扫描器正确拒绝跨代码缓冲区地址。这个覆盖缺口不是上一批 Div128 selector 迁移造成；v1 的两个失败记录、源码和二进制继续保留，不计作通过。

本批把可用 region 中的双结果调用改为一条 BL，调用同一 EmitPairCallTrampoline 生成的 region 入口。PairCallFrame 布局、参数、结果、保存/恢复和 flags 顺序保持；十四项源码比较核对旧非 region 路径、帧常量、trampoline 编码体、其余 region 入口及原并发退役断言。公共 compile、分配和 UnitActions 未改，也没有新增源文件、测试程序、配置、probe 或仓库测试脚本。

- pair_call_trampoline 继续定义唯一帧/编码体，新增共享的 x11 目标寄存器常量；region_link_trampoline 导出新入口偏移，CodeCache 保存入口并在关闭时撤销。
- JitContext 记录实际 BL 编码位置，合并 Function region 的编码状态，在 placement 时回填；该列表不保存 IR 选择，也不参与寄存器分配。region 不可用时保留原四条地址物化加 BLR；不能把本批验收推广成该回退的完整缓存支持。
- SerialRegionBranch 增加 PairCall，既有十种枚举编号不变。PairCall 保存 BL .，其余入口保持 B .；冻结只识别本 cache 的真实已知入口，恢复绑定到目标 cache。扫描器在准确的 PairCall recipe 处把 x11 作为 helper target，要求紧邻四条可解码物化和主程序镜像地址；没有放宽任意跨缓冲区分支。
- ApplyRelocations 与 AOT 全单元发布前检查要求唯一的四指令 x11 helper relocation。缺失/重复 relocation、B/BL 混淆和错误 kind/mask 均拒绝。代码格式从 34 升至 35；recipe 字段宽度和 AOT 外层布局未变，外层版本仍为 6，兼容键继续检查代码版本。

目标仍是实际 TranslateIR 的两块 Function：第一块以 high=AsrImm(low,63) 做 native signed 除法；第二块以 high=1 做真实 unsigned 128/64 helper 除法，商与余数都可观察。输入 36 与 UINT64_MAX 使用独立常量答案；除法输入、带符号商余数和原 SSE 值跨 helper/内部边使用。捕获要求恰好一条 SDIV、零条 UDIV，并核对 DivideUnsigned128 的唯一 helper relocation 与 PairCall recipe。安装到不同 RX、销毁 source 后，直接核对实际 BL 跳入 destination region，再检查结果、NZCV、返回体、帧、缺失入口和失效。真实 x86 guest AOT producer 仍是独立用例，不能称它已生成本批手工 HIR 除法。

14 个 Android 目标编译成功，冻结后才部署 10 个实际使用的 ELF；10 ELF 与两个 guest 的 SHA 在运行前后完全一致。17 个现有源文件修改，冻结清单为 467 个路径、111 个累计退役路径；本批无新增/退役源文件。

| 身份 | SHA-256 |
| --- | --- |
| source digest | d6ad3f1655e36af55faa15f9c55159fb96a0bffb9130c45735b0c669c498520d |
| source overlay | 0377a8cee8ca10118839be104775a6b52cd0ff7782f106081d570e8f4b35d871 |
| launcher | 5e0f7acb7bd028acafd36fa9872d92b289bbdf9b0e1f32b8ce3ffb98d07d08b0 |
| main suite | b5c4236454a043251e023d1ed5f54bfb4064bafe40b91cc874747ac2e0eb15dc |
| IR suite | 2b99f43892ff061bdc21c3436d9ea8017d2637d4735e1b6a0140c6597efac956 |
| call persistence | f140a066ae9f5a4705caa1a29dafbc3f005e2e9ae3a1494be0c5695cbcdd0075 |

红魔 bf1359ff 集中执行 48 项计划内候选命令，另有一项最初的 flags0 诊断，共 49 次候选执行；另运行 3 次相邻基线对照，没有重用旧 ELF 的结果。46 次候选退出与预期一致，主套件的预期退出仍为 66。四组快/慢 IR 的独立 exec 磁盘 writer/reader 均恢复两单元、零重新编译、四个 helper relocation，PID 和主镜像地址各自不同；四个 AOT 除法模式均在 source 销毁后执行。间接目标的十二次实际 spill-backed miss 仍保留。AOT 矩阵现有 25 个情形，其中 23 个拒绝，新增 5 个 pair-call 损坏情形；另有 compatibility/pristine 两条执行路径。

三个失败必须保留：默认 flags0 的 shutdown-aot-linked-call-executor，以及显式 flags1 的 AOT/磁盘两个 linked-call-executor 模式，均在“restored linked Call executor completes or ejects at the retired return”处失败。每条均用相邻原始 shutdown ELF a06e98cdc4a7f89b04197fd8b16c9ffcb0d98ded9628d05f11f45c744520627c 在相同环境执行，退出 1 且错误日志字节一致。未修改该断言；未证明实际返回原因，不能计为关闭/并发通过。其余新增的 owners/resource-cycles 和六项显式 flags1 的 multi-protect、concurrent-compile、concurrent-install 的 AOT/磁盘组合通过。后续 r6-call-retirement-boundary-v2 已定位并修正这三个断言失败，见下一节；本节保留当时原始结果，不追改历史退出码。

主 XML 为 740 case、226057 次断言，较相邻 739/226137 新增一个 pair-call scanner section；11 个失败 case、66 个失败断言、零 error 保持，所有共享 case 的顺序与逐项失败签名相同。断言总数变化不全部归因于新增用例。IR 快/慢为 70/25057、70/25141，fixed 为 2/39，均零失败；34 个原 mode marker 中 28 个逐字相同，entry-reject 增加两个检查，五个 AOT call marker 按新增结果/代码/拒绝检查增长。不能把 all_exits_expected 或 all_markers_equal 改写为 true。

10 个 CoreMark/bench 静态组合均有正确 CRC/checksum、实际模式和零 target overflow。五个 bench 的完整有序记录相同。CoreMark 的共同变化均在 __cpu_indicator_init：两种 Block 模式各七个 CPUID 点每处少 16 bytes；两份原始 host dump 的十四处比较核对四条 x11 helper 物化指向 QueryCpuid，原四条 x16 trampoline 地址物化加 BLR 替换为 BL .，其余指令逐字不变。这是既有发布前 dump 的比较；恢复后实际 BL 绑定由原生夹具另验。Function 共同点 0x26a0bf 保持 IR=809，普通与 bound-off 的 host 2968→2856，fixed 的 2936→2824；不宣称整份 Function 机器码逐字等价。

| CoreMark 模式 | 记录数：相邻→候选 | RA host bytes：相邻→候选 |
| --- | --- | --- |
| Function | 427→426 | 286876→286624 |
| bound-off Function | 427→426 | 286884→286632 |
| Block | 2418→2414 | 205576→204988 |
| fixed Function | 426→426 | 286456→286296 |
| fixed Block | 2414→2413 | 206612→206028 |

非 formatter 有序记录在计入上述 CPUID 尺寸变化后相同，新增/缺失 PC 全在既有 formatter 符号内；10 组 spill_units/defs/loads/stores、pair_fallbacks 汇总逐项相同。总 host bytes 和 roster 差异不能全部算作 pair-call 收益。11 份 workspace 日志均闭合并满足分项加总，10 个负载的基础六字段相同，7 份完整字段相同；IR slow 仍为 343 次分配/释放、6677152 bytes、peak=3981724、零 live/retained。新编码列表和 region body 的私有成本不在这些公共计量中。

DWARF 的 JitContext 为 1384→1408（+24），CodeCache 为 360→368（+8），JitTranslator=3280 与 UnitActions=864 不变。BL 列表的动态容量及每 region 的额外 body 仍须计量。自身固定帧 ScanCodeUnit 1248→1312（+64），FreezeRegionBranches 144→160（+16）；ApplyRelocations=192、Flush=368、EmitPreserveAllPairCall=272 保持。这些是各函数自身静态帧，不是总栈峰值或性能。没有运行 PMU，没有新增吞吐、耗时或正式 CoreMark score 结论。

证据位于 /private/tmp/swiftvm-middle-results，前缀 r6-function-div128-revival-v2：plan/build-record、source-manifest/overlay/comparison/format、14 ELF 清单、test-executions 与 regression-reference-executions、test-summary、shape-summary/static-quality、pair-code-equivalence、source-equivalence、frame/layout、storage-summary/comparison、hash-stability、quality。native.tar 共 56 文件（49 候选日志、4 XML、3 基线日志），SHA=2d43cd5906d5427267aec7f7207709db1e6cc09a6a134b9d3a79d19d6f7fe1f8；results.tar 共 66 文件，加入 10 静态日志，SHA=789d48585990ec2db39bf1e789235e956158d18379316c063dd31cfd66f5d447。22 项质量核对和十四项源码比较通过，但完整 R5/R6、并发 Call 退役、私有成本、RISC-V native、可复现 FEX 和正式性能均未完成。

## R6 恢复 Call 退役的 SMC 返回边界与夹具存储

r6-call-retirement-boundary-v2 接续 r6-function-div128-revival-v2，只修改现有 address_space_shutdown_test.cpp。此前三个错误不能因在基线出现就当作正确性通过；本批沿实际 continuation/poll/回收代码核对后，确认夹具包含两类问题，生产 Runtime、JIT、SMC 和 ABI 实现保持原始 SHA。

BlockingReturningTargetArtifact 向 uniform 偏移 32 写入八字节 callback 结果，而两个调用方沿用 TestConfig 的 32 字节缓冲。现以共享 kCallCallbackOffset=32 和 size=offset+sizeof(u64) 将这两个夹具配置为 40 字节，并在生成目标时要求有效范围；其他夹具不统一放大。旧越界运行即使两版同样失败，也不能据此推导生产退出语义。保留的 v1 诊断在扩大缓冲且未改变原返回预期时，flags0 已通过；flags1 精确报告 actual=0x8、expected=0x100、location=0x2180，也就是 CodeMiss 与该夹具的 return_pc。

另一个过期假设是“flags1 有保留 continuation，所以在 caller 被退役后仍返回 CallHost”。AddressSpace 的实际 ExitLatchEnabled 是 BackedgeLatchEnabled 或 config.region_edges；本夹具 region_edges=true，SMC 在撤销 dispatch alias 后发布 generation。ForwardContinuation 先消费帧、在调用 continuation 前检查 request；SMC-only 的冷退出选择 CodeMiss，并发表实际 guest 返回目标。SVM_BACKEDGE_LATCH=0 并不会关闭显式 region 的这条行为。故两种 flags 表示都必须在 return_pc 报 CodeMiss，旧返回体不应执行；没有为通过旧断言而删除生产 poll 或修改 SMC 协议。

两个既有 retire/invalidate 模式保留冷暖两轮、真实物理链接、独立 callee、signal invalidation 的准确指令恢复、epoch/signal-reader 等待、单次释放、别名及元数据撤销和重复退休拒绝。在这些条件上补充：

- runner 在原 blocking callback 的发布同步之前交出 State 指针；主线程只在 callback 尚未释放时原子读取请求，确认非零 SMC generation 且无 Signal/全局 Retirement 位，没有增加生产 hook。
- release 前核对 return_pc 的 canonical、Call 和 pending Call 三个表均撤销。
- 首次退出严格要求 CodeMiss、准确 return_pc、value=37、callback=84、return witness=0、空 RSB 与 committed NZCV=0。
- 同一个 Runtime 再次从该 PC 进入仍须 CodeMiss；返回 witness 不变、NZCV 不变、RSB 仍空。诊断 helper 永久报告实际/预期 reason 与 PC，不是临时 probe。
- warmup 仍要求未退役时真实返回体产生 0x51ab；这与退休后的零 witness 构成独立观察，不把“代码没执行”作为普遍成功条件。

14 个 Android 目标统一构建成功；冻结后的 14 ELF 中，仅 shutdown 二进制发生改变，其余 13 个与相邻原始文件 SHA 完全相同。467 个源码路径、111 个累计删除路径保持；实际差异只有该测试文件，无新增源文件/程序/配置/测试脚本，生产文件全部未变。

| 身份 | SHA-256 |
| --- | --- |
| source digest | 1e8a24cbbde3be074023636c9e9c0dbb136a761e127aaea8f76c7ba0e9d3951d |
| source overlay | 9607ccbf788e2570aa50498358d06ccda7fca839330b1b628c19d303425fbbf8 |
| shutdown ELF | 91f3d024a3cacf28f38698b4976c1dc1841917656b4f7e12dcd4e6fede06b9f3 |

红魔 bf1359ff 集中运行 24 项，全部退出 0。受影响的 executor/invalidate × AOT/disk × flags0/1 × 快/慢 IR 共十六组，均验证 code_miss=1、retry_miss=1、reader_wait=1、frees=1、entries=2。另运行 owners、resource-cycles 及六个 flags1 的 multi-protect、concurrent-compile、concurrent-install AOT/disk 组合；八个 mode marker 与相邻逐字相同。v1 的两次诊断不计入这 24 次通过。

以 13 个原始 ELF 的准确 SHA 身份复用相邻 38 项非 shutdown 原生记录和 10 项静态记录，保留其原始命令、二进制名、log/XML 名称及字节哈希；不复用相邻的任何 shutdown 结果，也不把旧执行称作新跑。适用集合因此为 24 新执行 + 38 原执行；主 XML 仍沿用 740 case/226057 次断言、已知 11 个失败 case/66 个失败断言/零 error，IR 快/慢和 fixed 的既有零失败结果保持。完整并发矩阵、其他故障家族、私有成本、正式 PMU 与可复现 FEX 继续开放；本批没有运行静态负载或 PMU。

证据前缀为 /private/tmp/swiftvm-middle-results/r6-call-retirement-boundary-v2：plan/build-record、source-manifest/overlay/comparison/format、binaries/binary-comparison、deployment、test-executions/test-summary、reused-evidence、source-equivalence、quality。原生归档 26 个文件（24 个最终日志及两个 v1 诊断），SHA=2afcf6f2583fbc05a220393bb95a696f3a69be369a93bbbe45b0b89b1a49ebc2；18 项质量核对与九项源码/原断言核对通过。v1 的源码、冻结二进制和诊断证据保留；设备上的中间 ELF 经 SHA 确认后清理。

## R5 条件准备统一索引与废弃 FPR 分析退役

r5-indexed-predicate-cleanup-v2 接续 r6-call-retirement-boundary-v2。原 LowerSelects 已归准备层，本批不是首次把条件分析移出 emitter；它去掉了残留的第二份 Block 输入和 intrusive iterator 窗口。LowerSelects 只接受 const BlockAnalysisIndex 与原 defer callback，Select 的有序访问、producer 所属块/真实指针核对和两点之间的安全窗口统一来自已有 roster。布尔递归深度四层、立即数 1/0、raw-use 限制、共享常量计数、pseudo-use、defer 顺序和 covered/action 内容保持。没有把 pseudo-use 直接改成全局计数，跨块条件与同 ID 的外块节点继续拒绝 direct 融合。

LowerIntegerBranch 的 condition 查找也由整段 std::find 改为现有 InstructionPosition；后续 dead-edge、carry polarity、NZCV/CBZ 双窗口、固定 home/dirty/preservation、窄立即数与 helper/fault 规则保持。正常 dense 编号直接核对位置与指针，稀疏手工编号沿既有有序查找，不新增索引或 owner。17 项源码比较核对规则、完整后缀、CMake 删除和其余源项身份；prepared actions、emitter、公共分析及分配源码未改。LowerSelects 仍使用既有共享常量 map，边准备仍有自己的短期 discarded 集合；Complete 在家族入口显式准备直接使用图的触发方式也未改，不能宣称全部条件扫描或私有成本已经收口。

ScalarFPRLiveness 原来只有 register_alloc_fpr_test.cpp 的 ScalarProductUpperDead 调用，既不决定生产分配也不决定 emitter。本批删除该类、scalar_fpr_liveness.cpp/.h 和两个 runtime CMake 源项；没有把同一份递归哈希算法迁到别处。原两个 bool 查询改为实际生产 Block 编译、Module 节点与 dispatch 入口发布、ARM64 Runtime 执行。32/64 位分别覆盖 product 作为后继标量加法的左/右操作数：显式输入 3、2、4 的结果为 10，各 inactive lane 使用不同 bit pattern，输出必须继承真实左侧 merge source。新的结果/高位期望不由分配器、matcher 或旧 liveness 算法生成；非 ARM64 构建只执行可用的编译检查，不记作跨架构 native 证明。

Select 的既有行为矩阵从 16 增至 18 种，新增 100+3*i 的稀疏 ID 正例及相同 ID 的外块 condition 负例；其余窗口阻断、未来 pseudo、共享常量与多使用限制保持。既有“stale analysis”用例改名为“one indexed roster”，反映 API 不再接受第二份独立 Block；空 roster、defer 拒绝、覆盖重置与其他 allocation 身份检查继续存在。

v1 的 14 目标编译通过，但 replacement FPR 用例最初只调用 TranslateIR(Block)，未完成普通 Block 调用方负责的 Module/dispatch 发布。主 XML 在首次 Runtime 入口返回 CodeMiss，退出 67，新增 case 尚未执行标量机器码。v2 补齐正式调用方发布并核对实际可查入口后再运行；不改生产 Runtime 或算术发射。v1 的 12 次执行中，11 次独立通过模式仅在对应 ELF 与 v2 完全同 SHA 时复用，失败主套件从未计为通过；v1 原始源码、二进制、日志/XML 均保留。

| 身份 | SHA-256 |
| --- | --- |
| source digest | 49b688b41f1690051202021890f473331f71bf95a259e92c46a537cdb98e64ab |
| source overlay | 4bacc02e59cb0909db58403d4e849be63aa7882da4fd44ac0791bd01d1807e8a |
| launcher | 3719b934d271ac4b78c7348705675335c7e90c03c57954887af027bacecc5d05 |
| main suite | 2ce7ad902fb9c5ec57577ca487574f016343202d9ccaa2c6064a91897a3d8a42 |
| IR suite | 7f1ce7f620247845858fb9060de609a9c1c5c404e7a71a7c8022105df558fc59 |

本批实际修改 7 个既有文件、退役 2 个文件，没有新增源文件、配置、测试程序、probe 或仓库测试脚本。冻结清单为 465 个源码路径、113 个累计退役路径；14 个 Android ELF 构建成功，10 个实际使用 ELF 与两个 guest 的设备 SHA 在测试前后完全相同。源码、测试和生成 build.ninja 对旧类/路径引用均为零，主测试 ELF 也没有旧类与独立 helper 的定义。

红魔 bf1359ff 的适用原生集合为 62 项：51 次 v2 新运行、11 次有准确 ELF 身份和原始命令/log/XML 字节的 v1 结果。主 XML 为 740 case、226212 次断言；与相邻 740/226057 相比，两对改名/替换用例全部通过，共享 case 的顺序与逐项失败签名保持。11 个既有失败 case、66 个失败断言、零 error 未变；断言总量 +155 不全部归因于新增断言。IR 快/慢 70/25057、70/25141，fixed 2/39 仍零失败；58 个 mode marker 与相邻逐字相同，三类 cycle rescue 为零，十二次实际 spill-backed target miss 保留。上一批的十六组 Call 退役/失效及八项 shutdown 回归均重新运行通过。

10 个静态负载均为本批新执行，CRC/checksum、实际 Block/Function 模式和零 target overflow 保持；所有共同 PC 的 IR/host 记录、非 formatter 有序记录及 spill_units/defs/loads/stores/pair_fallbacks 汇总相同，6 组完整有序记录及完整 multiset 相同。其余四组差异仅在 formatter roster：bound-off Function CoreMark 426→427、RA host 286632→286724；Block 2414→2412、204988→204688；fixed Function 426→427、286296→286388；fixed Block 2413→2414、206028→206500。普通 Function CoreMark 426/286624 和五组 bench 的 host 数值 19896、19904、16464、20068、16488 保持；不把 formatter 工作集变化记为代码生成收益。

11 份 workspace 日志均闭合、分项与 total 相符；10 个静态负载基础六字段相同，7 份完整字段相同。IR slow 保持 343 次分配/释放、6677152 bytes、peak=3981724、零 live/retained。这些公共统计不等于 LowerSelects 的临时 map、条件融合集合或整次编译堆/栈峰值已经完整计量。

| 函数自身静态指标 | 相邻→候选代码 bytes | 相邻→候选固定帧 bytes |
| --- | --- | --- |
| LowerSelects | 2452→2548 | 320→320 |
| LowerIntegerBranch | 7508→7476 | 560→576 |
| TargetPreparation::Complete | 7748→7744 | 1728→1728 |

LowerSelects 的自身代码增加 96 bytes，整数分支自身帧增加 16 bytes，后续完整成本验收继续覆盖这些代价；不据此宣称性能提升。DWARF 的 UnitActions=864、JitContext=1408、BlockAnalysisIndex=240、SemanticFacts=624 在两版一致，没有新增持久字段。本批没有运行 PMU。

证据前缀 /private/tmp/swiftvm-middle-results/r5-indexed-predicate-cleanup-v2，包括 plan/build-record、source-manifest/overlay/comparison/format、14 ELF 清单、v1-binary-comparison/reused-evidence、test-executions/test-summary、shape-summary、source-equivalence、frame/layout、storage-summary/comparison、deployment/hash-stability、cleanup、quality。native.tar 为 66 个文件（62 个日志、4 XML，保留 11 个 v1 原文件名），SHA=2738d8dc656b8b5d4de4d5a0d19740c74f23be0be901cd49ef68309a902a2e0e；results.tar 为 76 个文件，SHA=35b65fe295c6fb56d987117bbf0c9a8c445b3e559c2d6be7fe0646d33cbe3ff5。v1 原生归档 15 文件，SHA=a6b0b1ad0709ec91327953a3eb0c6ceb229d4d33d0a9e36e61cce48d84420045。29 项批次核对和 17 项源码比较通过，设备上十个 v1 中间 ELF 经 SHA 核对后删除，主机证据保留。完整 R1/R5/R6、私有成本、其余 native/并发矩阵、固定 FEX 与正式性能保持未完成。

## 首批 PMU 实测

本组只校验采集协议与明显退化，每个点采样一次，不能作为正式质量门或收益结论。统一使用 `simpleperf stat --cpu 4 -e cpu-cycles,instructions taskset 10 ...`，SVM 的 `SVM_JIT_CACHE` 为空；FEX 每个点使用独立的 APP_DATA/APP_CACHE 路径，服务使用本轮独立 socket。原有其他 FEX 服务未被重启或关闭。本组日志未出现可读性探测时的 `sched_setaffinity` 警告。

| 指标 | SVM 基线 | SVM 候选 | 设备 FEX 参照 |
| --- | ---: | ---: | ---: |
| instructions，2000 次 | 5,835,667,928 | 5,835,641,513 | 1,438,837,216 |
| instructions，12000 次 | 17,163,025,415 | 17,163,368,693 | 8,004,602,787 |
| cpu-cycles，2000 次 | 2,626,572,483 | 2,632,599,286 | 413,568,814 |
| cpu-cycles，12000 次 | 6,044,451,015 | 6,037,058,032 | 2,117,099,489 |
| 指令斜率，kins/iter | 1132.7357 | 1132.7727 | 656.5766 |
| 周期斜率，kcycles/iter | 341.7879 | 340.4459 | 170.3531 |
| 启动/编译相关指令截距，B | 3.570196 | 3.570096 | 0.125684 |
| 指令/周期斜率算术比（未校验事件运行比） | 3.3141 | 3.3273 | 3.8542 |

候选相对基线的指令斜率变化为约 `+0.0033%`，周期斜率约 `-0.39%`；单组数据不构成性能提升证明。候选/FEX 指令斜率比约 `1.7253`。本批以结构清理和元数据寿命修复为主，尚未实施后续 recolor、共享改写队列和候选 lowering。

采样使用相同 guest，SHA-256 为 `d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba`。FEX launcher 哈希为 `5bcc2db652dd0c7e9b4a6acdbdb6058a0db3ae650171b86054589a46522b1898`，FEXServer 为 `0469ae00ec6b7afe030ee18b0c64bf0b425d19fd9ff4bcb8e0f47a3dafe7ebf6`。

原始 JUnit、运行日志、六份 PMU 输出及 `pmu-summary.json` 保存在 `/private/tmp/swiftvm-middle-results`；设备日志保留在 `/data/local/tmp/svm-middle-r1/results`。本轮未逐点采集温度与运行模式，也未确认计数事件运行比；表中的斜率算术比不能作为有效 IPC，正式比较仍需补齐这些条件并重复采样。固定工作量运行低于 CoreMark 的 10 秒计分要求，程序会输出时长提示；这里仅使用正确 CRC 的工作量和 PMU 计数，不引用为正式 CoreMark 分数。SVM 两版均有既有的 `sched_getscheduler` 未实现提示，日志已保留。

## R5 谓词候选工作区复用与哈希暂存退役

r5-predicate-workspace-v2 接续 r5-indexed-predicate-cleanup-v2，只修改八个既有源文件，没有新增源文件、配置、probe 或仓库测试脚本。源码清单保持 465 个路径和 113 个累计退役路径。lower_predicates 的共享常量 unordered_map、ConditionRecipe 与 EdgeCandidate 的两份 unordered_set 已全部删除；没有另建工作区成员、索引所有者或冻结子结果。两个公开选择接口显式接收已有 CompileWorkspace，consumer、vector、Select 与 branch 各自独占、顺序复用同一 LoweringCandidate 缓冲。

Select 仍按最终 roster 顺序选择，单使用常量直接覆盖，遇到实际共享 arm 才借用候选缓冲。每个被替代 arm 记录一次经过真实指针核对的位置，末尾排序分组后与 GetUses(false) 比较；只数被替代的 true/false 参数，常量兼任 condition、未被替代用户、pseudo 及跨块使用不被误计。共享常量的 covered 输出顺序改为 roster 顺序，StoreSelects 仅将这些 ID 写入统一覆盖位，不依赖旧 unordered_map 的遍历顺序；SelectAction 的顺序及内容不改。

分支 condition recipe 只用最多三个指针的局部数组，完整 discarded 则随 SaveFlags、BranchOnlyFlags、ClearFlags、InvertCarry 和 polarity store 的窗口增长，不能限制为三个元素。所有 imported condition/constant 先通过 InstructionPosition 核对真实节点，再追加位置；重复项排序去重，后续与完整 roster 线性合并，避免逐指令哈希查询和增长集合的反复线性查找。首轮扫描只有初始 condition 三节点与一个 polarity constant 可能在后续再次遇到；后者保留手工 forward-reference 的跳过行为，第二个 polarity store 直接拒绝。producer/condition/narrow load 的最终真实身份检查继续存在，NZCV 与 CBZ physical home 的两段完整窗口、dirty/preservation、宽度、helper/fault 及观察规则保持。纯拒绝的时机调整不提交物理编辑。

这些统计仍属于最终编号后的目标准备。decode 可以播种形状，不能提前判定最终 shared-use 数量、跨块使用或物理 home。借用 span 只在 LowerIntegerBranch 返回前有效；转成 owning covered IDs 后销毁 scope，拒绝及异常路径也清空借用，未进入 UnitActions 或 emitter。普通块没有新增整块初始化；有 dead-edge proof 的失败候选可以短暂借用已有容量，不能把零 allocator 请求写成零调用或零帧成本。

既有 Select 行为矩阵由 18 增至 20，新增常量同时作为 condition 与 arm 的两个 multiplicity 边界；这些形状继续使用稀疏编号。现有 independent-select、外块/同 ID、pseudo/defer、生命周期和 unordered payload 用例复用同一显式工作区。既有 integer branch 身份用例还保留一组 DCE 后手工插入的 64 条冗余 Carry 清除：Carry 在两条后继均死，Zero 关系保持；核对原分支资格、精确覆盖数量与每条写入覆盖，覆盖完整 discarded 大于三个节点的规划窗口。该 64 条形状是规划检查，不单独声称实际机器码执行；既有实际分支执行和静态负载结果见下文。

v1 编译在既有 independent-select 测试 lambda 缺少工作区捕获处失败；没有冻结或部署任何 v1 ELF。v2 仅补显式捕获后完成 14 个 Android 目标构建，失败日志及 v1 源码快照保留。10 个实际使用 ELF 和两个 guest 的设备 SHA 在本批测试前后相同。

| 身份 | SHA-256 |
| --- | --- |
| source digest | b8ab8284b55c52a7953b64a5e4390237ee0840ceda340580fe64a891c883b950 |
| source overlay | 8dc8c30b6641d6dbcc7993c37ec131e5a4b43e02580b95d956cf093d97353a2a |
| launcher | 4af2677e14f58a5f86077199bdbae5421d211b6f5a8afff45d445b1865e06d96 |
| main suite | 58ab5444d303a3c60fc81b5c89653ac4c21b7e6073a8cd2e7e2f975f438593cf |
| IR suite | ab38276e7228a48e04a3eb682fb5c9477c09cf40ed827b5b06e757cf5f358dff |

红魔 bf1359ff 完成 62 项新原生运行，没有复用旧执行；全部退出码符合预期。主 XML 为 740 case、226202 次断言，原有 11 个失败 case、66 个失败断言、零 error 的逐项签名与 case 顺序不变；两项扩展边界所在 case 均通过。断言总数较相邻少 10，不以总差值反推新增边界的执行数量。IR 快/慢 70/25057、70/25141 与 fixed 2/39 仍零失败，58 个 mode marker 逐字相同，三类 cycle rescue 为零，十二次实际 spill-backed target miss、十六组 Call 退役/失效与八项 shutdown 回归保留。

10 个静态负载均为新运行，正确 CRC/checksum、真实 Block/Function 模式与零 target overflow 保持。所有共同 PC 的 IR/host 记录、非 formatter 有序序列及五项 spill 指标相同，六组完整 ordered/multiset 相同。其余仅 formatter roster 变化：bound-off Function CoreMark 427→426、RA host 286724→286632；Block 2412→2415、204688→205408；fixed Function 427→426、286388→286296；fixed Block 2414→2416、206500→206316。普通 Function CoreMark 426/286624 和五组 bench host 字节数保持，不能把这些 formatter 总量差异记成编码优化或回退。

11 份工作区日志均请求/释放闭合且汇总正确。10 个静态负载的六项基础分配字段全部相同；IR slow 的全部字段相同，为 343 alloc/free、6677152 allocated/released bytes、3981724 peak bytes，最终 live/retained 为零。候选缓冲 zero/pattern init 为零，没有新增整块初始化。共享缓冲借用并非零事件：普通 Function CoreMark lowering retention_events 为 2751→3261，bench 168→222，Block CoreMark 1777→2210；这些样本的候选缓冲请求数、分配字节和峰值仍为 9/2044/1536、6/252/192、9/2044/1536。原临时哈希的普通 allocator 请求不在该统计内，不能据此计算总分配减少；更大候选仍可能扩容，排序比较、调用链和所有私有暂存的成本继续开放。

| 函数自身的静态产物 | 相邻 → 本批代码字节 | 相邻 → 本批固定帧字节 |
| --- | --- | --- |
| LowerSelects | 2548 → 2728（+180） | 320 → 304（-16） |
| LowerIntegerBranch | 7476 → 7060（-416） | 576 → 432（-144） |
| TargetPreparation::Complete | 7744 → 7772（+28） | 1728 → 1728 |

该表仅统计函数自身，不包括调用的 sort/helper、同时在栈峰值和堆暂存，不能合算成完整路径收益。DWARF 核对 UnitActions 864、JitContext 1408、BlockAnalysisIndex 240、SemanticFacts 624、CompileWorkspace 224 bytes 均未增长。本批没有运行 PMU，不作延迟、IPC 或正式性能结论；完整私有成本、图触发、其余条件融合、FEX 和 R0–R7 验收继续开放。

证据前缀为 /private/tmp/swiftvm-middle-results/r5-predicate-workspace-v2：plan/build-record、source-manifest/overlay/comparison/format、binaries/binary-comparison/used-binaries、deployment、test/shape-executions、test/shape/storage-summary、storage-comparison、source-equivalence、frame、layout、archive-manifest 和 quality。20 项源码/存储不变量及 27 项质量核对通过。原生归档 66 文件 SHA=8382f27852cc53dc1c6007298f0e2d3df0ff04fb2cba998a478330a36b02aab1；完整结果归档 76 文件 SHA=874243e1199223a1a21b07972a4a65fbda42f437c55d66ba463374fe63abe302，全部归档成员字节与提取文件逐一核对。

## R1 条件消费者按实际查询准备共享使用图

r1-predicate-query-demand-v2 接续 r5-predicate-workspace-v2。本批修改十个既有文件，没有增加源文件、索引所有者、工作区字段、配置、probe 或仓库测试脚本；冻结清单仍为 465 个源码路径、113 个累计退役路径。LowerSelects、LowerConsumers、LowerCarryConditions 及 incoming-zero 接收准备显式使用可变 BlockAnalysisIndex，准备的仍是原来唯一的本块直接使用图。IR、allocation、冻结动作与 emitter 算法未修改；选择函数的可变性只表示允许显式准备分析缓存。

BlockAnalysisIndex 新增 PrepareUsesOf 与 PrepareHasPseudoUse 两个准备入口。维护中的全局 GetUses(false)==0 可以证明没有直接引用，global pseudo count==0 可以证明没有 pseudo 引用；这两种情况不建图。其他情况只在 uses_ready 为假时调用原 PrepareUses，再调用原 const Uses/HasPseudoUse。非零全局计数只能证明存在引用，不能证明用户属于本块：terminal 使用、外块普通用户、外块 pseudo 与本块同 ID 节点继续按共享图及真实 Def 区分。原直接/别名图构造、dense/sparse/compact 规则、const 查询的缺前置拒绝、已有 span 稳定性均保持，没有 const getter 的隐式构图。

Complete 中 Select、consumer、integer branch 的三处整图预准备已删除。Select 只有实际进入 condition 安全窗口、且该节点确有全局 pseudo 引用时才请求局部归属；共享 arm 计数与纯布尔形状不依赖反向图。consumer 的 pair/FCmp/local/compound/string 查询按实际执行请求边；Boolean related-use 把已有 GetUses()==1 前提移到查询之前，unused/shared condition 不再为最终不会读取的边建图。audit、PSTATE 仍只用原 roster 规则。整数分支本来只读 roster/位置、计数和物理 home，入口不再额外构图，完整 NZCV/CBZ 证明仍执行。

同时审计后续调用方，raw-carry 的 SoleUser 与 AddIncomingZeros 自己显式声明边依赖，不以 earlier consumer 已建图作为隐含前置。前者仍检查恰好一个 total use 与真实本块用户；后者只在实际 incoming 值上准备相同共享图。其他分配、FPR、scratch、flags-only spill 和必要证明保留自己的触发条件；本批的零构图结论针对已列出的选择/查询路径，不能推广为所有生产输入或整个编译流程零构图。

既有 consumer Plan helper 从 eager Build 改为 SemanticFacts 的 roster-only PrepareIndex，原 pair/FCmp/flags 用例从真实未就绪图开始。新增一个永久用例覆盖 13 种形状：空输入、unused/shared/stored/terminal condition、外块普通用户、compound-clear、audit-only、boolean/direct/future-pseudo/foreign-pseudo Select 与普通未命中 Select；同时检查实际动作、局部/外块区别、Select 前后与 consumer 后的 UsesReady、SemanticFacts 只读可见性。两个既有 integer branch 家族也改为 fresh roster，短/64 条 dead-flag 写入、外来物理索引、映射变化与重置的原断言保留，并核对规划后图仍未准备。这是构图触发与选择结果验证，没有加入独立测试程序。

v1 的 14 目标编译、62 项原生和 10 个静态负载均通过适用核对。反汇编随后显示每次已就绪查询仍会进入 out-of-line PrepareUses 再返回；v2 仅在两个新入口加 uses_ready 判断，避免这次重复调用，并重新集中构建/验证。v1 源码、ELF、日志与归档保留，没有复用为 v2 原生结果；设备上的十个 v1 ELF 在 host/device SHA 一致后清除，并逐一核验不存在。v2 部署时，执行驱动遗漏分段 stdout 首段造成一次哈希误报；修正为收集完整输出后，全部 12 个哈希通过，再开始候选执行，未把误报当作设备产物变化。

| 身份 | SHA-256 |
| --- | --- |
| source digest | 02bbad82d0b38dd4628d96ca5268bbeb4207c1646b111d44c3669b35e4ce7171 |
| source overlay | c14dac6ec0b094904d43c61b84c87ebdf82e76acd27d80ed02231c15aa6efdbf |
| launcher | 68bef43619604cab917a0865937eb8d3fc25498d17198d1db5454bff70b9b71e |
| main suite | 6d29c1286e75b6c8679d2e4cb554f5ac227e7a3f495542ef5415800bbbf5a360 |
| IR suite | 936a25e12d93b1ae5cec9df67cf6b1bb2363f560b51472250d9960c6ab09053b |

v2 红魔 bf1359ff 的 62 项原生均为新运行，10 个实际使用 ELF 与两个 guest 的测试前后 SHA 相同。主 XML 为 741 case、226351 次断言；新增触发矩阵通过，所有共同 case 的顺序与逐项失败签名保持，仍为原有 11 个失败 case、66 个失败断言、零 error。总断言数不作为新增矩阵的执行计数。IR 快/慢 70/25057、70/25141 与 fixed 2/39 零失败；58 个 mode marker 相同，三类 cycle rescue 为零，十二次真实 spill-backed target miss、十六组 Call 退役/失效与八项 shutdown 回归保持。

10 组静态负载均为本批新执行，CRC/checksum、实际 Block/Function 模式和零 target overflow 保持。所有共同 PC 的 IR/host 记录、非 formatter 有序序列和五项 spill 指标相同，六组完整 ordered/multiset 相同。其余 formatter roster 变化为：bound-off Function CoreMark 426→427、RA host 286632→286724；Block 2415→2418、205408→205288；fixed Function 426→427、286296→286388；fixed Block 2416→2412、206316→206016。普通 Function CoreMark 426/286624 和五组 bench 全记录相同，不将 formatter 总量差异归为编码收益或回退。

11 份工作区日志请求/释放闭合且汇总正确，10 个静态负载的六项基础分配字段相同，其中六组全部字段相同；IR slow 的全部字段相同，为 343 alloc/free、6677152 allocated/released bytes、3981724 peak bytes、最终零 live/retained。直接使用图及其他私有容器的标准 allocator 成本不在这九类工作区记录中，不能由工作区计数相同或 isolated case 零构图推断总成本下降。

| 函数自身的静态产物 | 相邻 → 本批代码字节 | 相邻 → 本批固定帧字节 |
| --- | --- | --- |
| LowerSelects | 2728 → 2788（+60） | 304 → 320（+16） |
| LowerConsumers | 10712 → 10968（+256） | 832 → 832 |
| LowerCarryConditions | 1984 → 2112（+128） | 272 → 272 |
| LowerIntegerBranch | 7060 → 7060 | 432 → 432 |
| TargetPreparation::Complete | 7772 → 7740（-32） | 1728 → 1728 |
| 新 outlined PrepareHasPseudoUse | 原无独立产物 → 256 | 原无独立产物 → 48 |

PrepareUses 自身仍为 292 code bytes、112 frame bytes。只统计上述函数自身会漏掉首次建图的栈嵌套：相邻 Complete→PrepareUses 的部分静态固定帧合计为 1840 bytes；本批列出的可能路径 Complete→LowerSelects→PrepareHasPseudoUse→PrepareUses 为 2208，Complete→LowerConsumers→PrepareUses 为 2672，增加 outlined 查询一层时为 2720，Complete→LowerCarryConditions→PrepareUses 为 2112。调用链由实际 BL 与各函数栈调整核对，但这些只是触发首次构图时的静态部分合计，未证明具体生产负载走过该首次构图链，也不包括更深 builder/callback、堆暂存或实际同时峰值。已就绪路径跳过构图调用，不据此宣称零查询开销或整体栈降低；新增代码、查询成本和更深首次构图仍需完整成本门验收。

DWARF 核对 UnitActions 864、JitContext 1408、BlockAnalysisIndex 240、SemanticFacts 624、CompileWorkspace 224 bytes 均未增长。14 项源码/准备时机核对与 30 项质量核对通过；本批没有运行 PMU，不作耗时、IPC 或正式性能结论。其他家族构图触发、完整私有成本、并发准备/执行矩阵、可复现 FEX 及 R0–R7 仍开放。

证据前缀为 /private/tmp/swiftvm-middle-results/r1-predicate-query-demand-v2：plan/build-record、source-manifest/overlay/comparison/format、binaries/binary-comparison/used-binaries、deployment（含观察误报与完整输出）、test/shape-executions、test/shape/storage-summary、storage-comparison、source-equivalence、frame、layout、graph-build-cost、intermediate-cleanup、archive-manifest 和 quality。原生归档 66 文件 SHA=4a0587adc7f4793f3ea30b92f1f73d73bcc06560087f403b153a5405561183ed；完整结果归档 76 文件 SHA=0fe33ffe16f50ce28073446f421410c0fb2e255637b7146a18e9cd3bbeaec887，全部归档成员与提取文件逐字节核对。

## R5 PF 候选与 capture 点共享工作区

r5-pf-capture-workspace-v2 接续 r1-predicate-query-demand-v2，修改八个既有文件，没有新增源文件、索引 owner、工作区字段、配置、probe 或仓库测试脚本；仍为 465 个源码路径和 113 个累计退役路径。LowerFlagsTokens 的私有 Candidate（AArch64 DWARF 为 24 bytes）vector 与每候选的 points vector 已删除，改为显式借用现有 LoweringCandidateScope。原 Candidate 类型及其关联定义在最终 ELF 中消失，公共 LoweringCandidate 仍为四字节，不进入冻结结果。

同一借用的固定前段按原发表输入顺序保存候选：position 是当前 roster 的发表位置，roles 低五位为原 fixed home，第六位保留已选 zero-extension source 决定。消费时从同一封口 IR 读取已选来源，不重新执行资格匹配；真实外块发表即使 ID 相同也提前拒绝，原物理后缀指针比对本来也不会接受它。追加 capture 尾段前复制当前 entry，保护前段免于 vector 扩容造成引用悬空；进入下一候选只缩回前段大小，不能清除尚未消费的候选。

只有 preserved 路径发现实际额外 OR capture 后才使用尾段，保存本块位置并加入 producer，按统一位置投影排序去重。普通单点及没有额外 OR 的 preserved 单点直接采用 producer ID，没有单元素 points 容器；普通分支仍以 UINT16_MAX 表示整块保留，preserved 分支保持精确点位。两条 AND 路径可到达同一个 OR，最后仅生成一份该点证书。没有候选不借用缓冲、不收集物理后缀；首次申请、扩容、输出 retentions vector 和调用方发表指针投影仍可能分配，不能把删除 points 容器写成整个函数零分配。

Resolve/Shares/Wrapper、parity 需求、完整 31-home 后缀、全块 preserved 判定、每点物理 alias/wrapper、clobber/发表唯一性及最终排序合并保持。preserved 仍由全块任一真实定义触发，其他值的点位变化也会影响 Shares，不能仅看 producer 自身。CapturesParity 与 capture 收集改用已存在的 PrepareUsesOf 显式请求同一共享图，const 查询不补图。普通阶段的后缀摘要与 preserved 的逐点完整扫描继续分开。decode 只可播种语义形状，实际 home、capture/保存点与最终使用图仍在编号、分配后准备。

扩展两个既有测试而未新建程序：ownership 用例新增同 ID 的外块发表拒绝；forwarded-capture 用例覆盖 dense/sparse × 0/32 个额外 direct capture，含两条 AND 汇合到同一 OR、独立第二 producer/发表、候选输入顺序反转、冻结/reset 后结果仍独立。单 capture 形状有两条最终证书，多 capture 形状有 35 条，汇合重复点只计一次。这些是规划/所有权断言，不单独声称所有手工 capture 指令已运行机器码；真实编译运行证据按下面范围报告。

v1 的 14 目标构建、62 项原生与 10 个静态负载均符合原规则。成本检查发现专用 point comparator 多生成排序模板，launcher .text 由 4471788 增至 4476644（+4856）。v2 仅把 sort/unique 改为其他家族已在用的位置投影，.text 为 4473084，较 v1 减少 3560，仍较相邻增加 1296。两个版本的源码/ELF/日志保持原始身份；v1 十个设备 ELF 经 host/device SHA 核对后删除并确认不存在，host 证据保留。

| 身份 | SHA-256 |
| --- | --- |
| source digest | 99f8c94118d5c5cd546b7709ef0d748613c7a4198d77c10b19b753ca51f0a760 |
| source overlay | c28acf94e0328f371383d61b661035af5c8e4d3322fad56441617709b5b9b7d8 |
| launcher | bc342d6b0f051432f4825742da0c97c5f639034857f6ff49cc1d5c26391c9643 |
| main suite | ea44a488cd8d373296053b45e92b01de41a9ad1a979ec25c1b02622af4a058ce |
| IR suite | 959fedf4e0a9fe05bc61225e4728cddb92eec3f9b3529881b4e65b2e2b5e11ba |

v2 的修正只涉及等价排序投影，集中复测六项：主套件、IR 快/慢、fixed 契约、flags1 static 磁盘恢复与 AOT static Call，另外重新运行十个静态负载。四个使用 ELF 与两个 guest 的测试前后 SHA 相同；六项均为 v2 新运行，没有把 v1 执行重标为 v2。v2 没有重复完整生命周期/关闭矩阵，完整最终 ELF 的 R7 矩阵继续开放。v1 的 62 项、58 个相同 mode marker、三类 cycle 零 rescue、十二次 spill-backed target miss 和完整 shutdown 原记录作为独立历史证据保留；本次两个 static Call 模式实际 target_spill_misses 为零，不算作新的 spill fault 验证。

v2 主 XML 为 741 case、226612 次断言，共同 case 顺序及逐项失败签名保持，仍为原有 11 个失败 case、66 个失败断言、零 error；两个扩展 case 均通过。总断言变化不作为新增形状执行计数。IR 快/慢为 70/25057、70/25141，fixed 为 2/39，均零失败；两个 Call mode marker 与相邻相同，磁盘读回为 loaded=2/compiled=0/helper_relocs=4，AOT 记录为 units=2/executions=4/faults=0/source_destroyed=1。

十个静态负载的 CRC/checksum、真实 Block/Function 模式及零 target overflow 保持；所有共同 PC 的 IR/host 记录、非 formatter 有序序列及五项 spill 指标相同，七组完整 ordered/multiset 相同。其余只在 formatter roster：Block CoreMark 2418→2416、RA host 205288→205240；fixed Function 427→426、286388→286296；fixed Block 2412→2413、206016→206028。普通 Function CoreMark、bound-off Function CoreMark 和五组 bench 全记录相同，不用 formatter 总量差异宣称编码收益。

11 份工作区记录请求/释放闭合且汇总正确，全部六项基础分配字段与相邻相同，候选缓冲 zero/pattern initialization 为零。借用事件并非零成本：普通 Function CoreMark lowering retention_events 3261→3990、bench 222→257、Block CoreMark 2212→2710；这些样本的候选缓冲 allocations/allocated_bytes/peak_bytes 分别仍为 9/2044/1536、6/252/192、9/2044/1536。IR slow 仍为 343 alloc/free、6677152 allocated/released bytes、3981724 peak bytes，最终 live/retained 为零，total retention_events 594→598。旧私有 vector、图和其他标准 allocator 请求不在这九类记录内，不能由工作区字段相同推断总分配、真实峰值或延迟下降；大 capture 输入仍可能扩容，沿用现有超限回收和异常释放边界。

| 静态产物 | 相邻 → v1 → v2 |
| --- | --- |
| LowerFlagsTokens 自身 code bytes | 5708 → 6204 → 6248 |
| LowerFlagsTokens 自身固定 frame bytes | 1312 → 1344 → 1344 |
| launcher 实际 .text bytes | 4471788 → 4476644 → 4473084 |
| 名称关联代码地址去重数 | 5 → 8 → 5 |
| 名称关联代码字节合计 | 11240 → 15752 → 12176 |

名称关联集合只包括 demangled 名称含 LowerFlagsTokens/CapturesParity 的已定义 code symbol，含模板、排除共享通用 callee，不能当作独占或整个家族总成本。v2 新 outlined seed 子函数为 416 code bytes/96 own-frame bytes，其他调用的 sort/helper 与递归/同时在栈峰值另计；函数自身帧增加 32、代码增加 540 不能由候选数据记录缩小掩盖。其余五个被核对的选择/Complete 函数自身代码和帧相同，UnitActions 864、JitContext 1408、BlockAnalysisIndex 240、SemanticFacts 624、CompileWorkspace 224 bytes 均未增长。

18 项源码/存储/投影核对与 30 项质量核对通过。本批没有 PMU，不作正式性能结论；发表输入指针投影、共享缓冲的容量/排序/借用成本、完整私有堆和栈、其余 R1/R5、最终矩阵与可复现 FEX 继续开放。证据前缀为 /private/tmp/swiftvm-middle-results/r5-pf-capture-workspace-v2，含 plan/build-record、source-manifest/overlay/comparison/format、binaries/binary-comparison/used-binaries、deployment、test/shape-executions、test/shape/storage-summary、storage-comparison、frame、family-cost、layout、source-equivalence、intermediate-cleanup、archive-manifest、quality。v2 原生归档 10 文件 SHA=d28b444d0445572247f2ebcdc5d5177aeb03319f278cb61e129a6caf85a541cb，完整结果归档 20 文件 SHA=9145091d254700325510fb1d8c444a5bb9ae759ce7be7c264945d3fec376df6f，成员与提取文件逐字节核对。v1 的 66/76 文件归档仍保留其独立清单，不合并计入 v2 新运行数。

## R2 Decode 立即数来源历史收口

本批于 2026-10-24 完成，基线为 `r5-pf-capture-workspace-v2`，最终冻结为 `r2-builder-immediate-history-v3`。仅收口 builder 来源存储及其契约，不认定完整 R2、R7、私有堆成本或正式 FEX/PMU 验收完成。

### 实现与保留的行为

- 删除 IrBuilder 的 `bound_positions` 全寿命哈希，以有序 `ImmediateSource` vector 保存指针和 append 位置。来源资格只由既有两处登记建立：实际 LoadImm 追加，以及 width 节点折叠成 LoadImm；没有新增完整 consumer 图、CFG 或工作区字段，既有 uniform_labels 哈希仍保留。
- 五条窗口计数仍只涵盖经 IrBuilder、目标能力启用时的真实 append，metadata 也计入；外部 raw appends 和 HIR 合并后返回空的 AdvancePC 不推进计数。Block 不依赖未编号的 Id，Function 不依赖包含外部追加的全局 Id。
- 延期候选按原位置查询；原位置更早的 width 常量有序插入，较晚的 LoadImm 不得淘汰早期查询需要的来源。没有延期候选时可以裁剪，首个候选入队后保留至提交完成；异常取消清空候选后也裁剪。
- 裁剪保留 `bound_index - window - 1` 起的来源，覆盖能力关闭后仍使用最后登记位置的既有零减折叠。不同定义可共享冻结位置，不能只按位置去重。保持同一 builder 内合法跨块常量引用，不把 decode 顺序视为支配关系，外部未登记常量即使紧邻用户也不取得资格。
- 稳定配置且已裁剪时只保留最后六个位置；延期指令可需要更多历史，配置切换可在同位置产生多条记录。vector 的高水位容量保留至 builder 销毁，长指令的有序插入与搬移成本仍需完整计量，不能宣称通用六条容量上限。
- 修改五份既有源文件，新增/退役源文件均为零；累计 source 清单 465 份、已退役 113 份。未增加临时 probe 或测试脚本。方案中的当前 CodeFormat 同步修正为既有版本 35、AOT schema 6；本批未改变序列化版本。

### 原生验证与证据复用

`r2-builder-immediate-history-v1` 的新增测试误用了不存在的 ZeroExtend 名称，构建失败，未冻结或部署产物。改为既有 ZeroExtend64 后，v2 十四个 Android 目标构建成功，在红魔 NX729J（bf1359ff）新跑八项原生检查和十组静态负载。最终复核将未登记常量移至用户紧邻处，避免距离超窗掩盖错误资格；v3 仅修改这份测试文件，重新构建 swift_ir_test 并新跑 IR fast/slow，十三个其他 ELF 与 v2 字节相同。最终八项适用原生结果为两项新跑、六项按 ELF SHA 原名复用；十组静态结果也按相同 ELF SHA 原名复用，未改写原始命令、日志或 XML。v2 的八次执行与 v3 的两次执行不合并冒充同一版本的新跑矩阵。

- 主套件为 741 cases、226752 assertions，既有 11 个失败用例/66 条失败断言/零 error；与基线的共享 case 顺序和失败签名相同。最终 IR fast/slow 各 72 cases、25183/25267 assertions，新增两项语义用例全部通过；fixed-register 两项/39 assertions 通过。
- 两项新语义用例覆盖独立 Block、HIR fast/slow，立即提交/延期提交，五条可折叠与六条不可折叠、未来常量、延期 width 原位置插入、邻近外部 raw 来源、配置冻结位置、跨块引用和 AdvancePC 合并。既有 flags 声明/Unknown、terminal 前提交与异常取消用例继续通过。
- 原 constant-window 寿命模式复用已有 new 失败钩子：预热八个常量后分别在普通调用和无延期候选的 guest scope 内再追加 256 个常量。两个 mode 各新增四项检查，marker 为 123470 checks、injected_failures=0；历史参考来自 r1-predicate-query-demand-v2 的同模式。钩子只覆盖 C++ new，不把 Inst TLS arena 的 malloc 或整个 decode 写成零分配。
- static/disk Call 与 AOT Call 的两个 marker 保持；本批适用模式的 target_spill_misses=0，未借用其他批次的十二次实际 spilled-target miss 作为本批执行。最终涉及的五个设备 ELF 和两个 guest，运行前后七份 SHA 均与冻结身份匹配。

十组静态对照的 CRC、五份 benchmark checksum、实际 Block/Function 模式、spill 指标和零 target_overflow 均保持。七组数量/大小的完整有序记录及 multiset 相同，共同 PC 的 IR/host 指标均未变化。其余仅输出格式化函数的编译单元列表不同，格式化范围外的有序数量/大小记录相同：

| 负载 | 记录数：基线 → 候选 | RA host bytes：基线 → 候选 |
| --- | --- | --- |
| boundoff CoreMark Function | 427 → 426 | 286724 → 286632 |
| CoreMark Block | 2416 → 2415 | 205240 → 205228 |
| fixed-class CoreMark Block | 2413 → 2415 | 206028 → 206400 |

不能把上述三组的总 host bytes 差异解释为同单元代码优化。五组 benchmark 的 RA host bytes 保持 19896、19904、16464、20068、16488。

另从既有日志补查四组实际 Block host dump，重算原始 FNV hash 并核对完整字节长度。两组 benchmark 的原始字节有序记录相同；两组 CoreMark 各有七个共同 PC 的字节不同，不能将前面的数量/大小一致写成原始机器码一致。七个 guest 块均在首个控制转移前包含 CPUID；差异仅限同一 x11 的 MOVZ/MOVK 四条地址装载中的立即数字段，其后 BL 和其他字节相同。两个冻结 ELF 中，唯一同时匹配地址低位的同名符号为 QueryCpuid，偏移分别为 0x16e734/0x16c8f0；每次运行由此推定的 16KB 对齐加载基址在七处一致。只归一这组已核对的 helper 地址字段后，四组共同 PC 的 Block 字节均相同；原始差异、推定基址、guest 反汇编和原日志身份分别保留在 host-byte-comparison、host-helper-symbol-candidates、guest-cpuid-sites 报告中。Function 未采集 body 字节，只报告数量/大小与 spill；此处不宣称十组全机器码相等，也不扩展到最终 relocation 后的执行缓冲。

### 布局、代码与未闭合的成本

Android DWARF 显示 IrBuilder 从 536 降为 520 bytes，来源记录为 16 bytes；UnitActions 864、JitContext 1408、BlockAnalysisIndex 240、SemanticFacts 624、CompileWorkspace 224 均保持。这里只减少 owner 内的容器头部，不能据此推断动态容量或总峰值。

下表为 launcher 中独立函数的 code bytes / 自身固定 frame bytes；无独立符号不代表原工作不存在：

| 函数 | 基线 | 候选 |
| --- | --- | --- |
| AfterAppend | 420 / 112 | 420 / 112 |
| FoldImmediate | 908 / 240 | 1072 / 240 |
| FoldExtend | 1216 / 240 | 1104 / 240 |
| CommitGuestInstruction | 124 / 48 | 132 / 48 |
| RecordImmediateSource | 无独立符号 | 628 / 128 |
| PruneImmediateSources | 无独立符号 | 168 / 48 |

HasImmediateSource 被内联；RecordImmediateSource 与 PruneImmediateSources 的新子调用成本不能从 FoldExtend 的减少中省略。表内各自身帧不能相加作为实际调用链或动态峰值，完整调用者/子调用与堆成本仍开放。

launcher .text 为 4473084 → 4466740（减少 6344 bytes）。按 IrBuilder 名称关联并按代码地址去重的符号为 164 → 169，合计 54828 → 49360 bytes；这是局部关联小计，不包含共享 vector/hash 等通用子调用，也不是完整或独占家族成本。v3 launcher 与 v2 完全相同，DWARF/nm/反汇编沿用原名原字节证据并记录身份。

十组静态日志和最终 IR-slow 的十一份工作区记录均闭合、分项总和正确。静态十组的六项基础分配字段均保持，格式化单元列表变化的三组仅有复用/初始化等访问量差异；其余七组及 IR-slow 全字段相同。IR-slow total 为 343 次申请/释放、6677152 bytes、峰值 3981724、598 retention events，结束时 live/retained 均为零。builder 私有来源 vector 与旧哈希不在这些工作区计数内，不能将该计数视为总堆节省。没有新增 PMU 采样或正式三方性能结论。

### 冻结结果与接续

- source digest：`24ae33ec733a53a3ace70636ddc7fbc7aa55b34a98afb096cc8b5b9a8c2f2589`；overlay SHA：`0233ec1f2ffe57da807ff0ae18d1505ed42b4c726cf4151f543d629f0398aefd`。
- 原生组合归档 12 份成员，SHA：`c7270c54c565dd80150e36a71e553ad68267b74c0570d0aa6f90734c30f5e879`；含静态日志的组合归档 22 份成员，SHA：`e9f3b34c955ad32c3f5b314bc696d05b75ca79b5f3198f6de873ea7572e606c9`；两次新跑的四份设备原始成员归档 SHA：`ad4db3fb2fbafe1bee9b1d17c7f13139e220e14e279599c1442b398a6d98b004`。组合归档保留复用结果的原名、原字节与来源归档，不将其标为新跑。
- 证据根目录为 `/private/tmp/swiftvm-middle-results`，各报告以 `r2-builder-immediate-history-v3` 为前缀；source、build、binary identity、执行/复用、XML/marker、静态 shape、storage、layout/frame/family 和归档身份分别记录。十九项 source 检查通过，质量门结果另见同前缀 quality-gates。v2 不再使用的 IR 设备 ELF 经主机/设备 SHA 核对后清理，另行确认文件不存在；主机冻结 ELF 与原始结果保留。
- 继续初次资格预查/CSE 播种、剩余类型与 guest-state/helper effects；量化长 guest 指令历史、其他私有容器、完整堆与调用链成本。R7 全适用原生矩阵、并发边界、RV native、固定可复现 FEX Android 构建与红魔正式三方 PMU 保持开放。`full_goal_completed=false`、`full_cost_goal_accepted=false`、`formal_fex_comparison_accepted=false`。

## R2 Function DCE 复用活跃 value 身份表

本批于 2026-10-24 在 `r2-builder-immediate-history-v3` 基线上完成，最终冻结为 `r2-dce-owner-roster-v1`。检查初始候选扫描时发现，Function DCE 仍为已有 value epoch 的全部节点建立 owner 哈希；本批消除这份重复归属，初始候选资格预查本身未改变。

### 实现与交接

- RewriteContext 的 LiveOwner 在活跃 epoch 内查询 ValueWorklist 的既有指针 lookup 与存活行，包括不可达节点；Find 先拒绝已置空的行，队列重复项不解引用已删除定义。不建立第二套 owner 哈希，也不为删除逐项访问该哈希。
- 没有 value epoch 的 Function 仍通过自身必要 owner 表完成 DCE；不为常量-only/禁用规范化的单元强开 value 图。独立 Block 保持原逆序删除，不进入 Function 成员查询。
- ValueWorklist 完整构造名册成功后才切换成员来源；此前构造失败不撤销原 DCE 状态。进入/退出 epoch 时清空旧队列与 owner 条目并取消播种标记，后续必要 DCE 从当前 IR 重新播种。工作区队列已有借用与容量继续复用，不为成员来源交接销毁重建该队列借用。先前独立 DCE 的 map bucket 容量可保留，不宣称所有私有容器已经退役。
- 保留 DCE 的必要初次扫描、真实 use 更新、隐式根和 flags/value 通知。新查询采用已有的二分身份查找，完整访问量、交接后的重新播种及总成本仍需归因，不能将少一张表等同于少一次必要扫描。
- 修改六份既有源文件，无新增或退役文件；累计 source 清单 465 份、退役 113 份。未增加临时 probe、测试脚本、owner 字段或配置开关。

### 集中验证

十四个 Android 目标构建成功，之后在红魔 NX729J（bf1359ff）新跑十项原生检查与十组静态负载，没有复用旧执行作为本批新跑。五个设备 ELF 与两个 guest 的运行前后 SHA 全部匹配冻结身份。

- 主套件 741 cases、226349 assertions，既有 11 个失败用例/66 条失败断言/零 error；共享 case 顺序和失败签名保持，无测试退役。IR fast/slow 各 73 cases、25207/25291 assertions，新增 Function 交接用例通过；fixed-register 两项/39 assertions 通过。
- 新语义用例覆盖 HIR fast/slow、进入 epoch 前已做/未做独立 DCE、活跃 epoch 内不可达定义回收和多次 Drain/CollectDead，随后在 epoch 释放后删除真实发表，再独立收完其输入。
- 现有 constant-window 模式预热 512 条 DCE 队列容量，以 128 个常量及一条真实 width 候选打开 value epoch；开启已有 new 失败钩子后完成 Function DCE，确认没有请求重复 owner 表的 C++ new 存储且死依赖链全部清空。fast0/1 各增加两项检查，均为 123472 checks、injected_failures=0。这不覆盖所有 malloc、冷队列扩容或总堆峰值。
- prepared Function fast0/1 的两个寿命 marker 各为 123494 checks、injected_failures=0，沿用 r1-predicate-query-demand-v2 同模式参考；static/disk 与 AOT Call 两个 marker 保持。本批这些 Call 模式 target_spill_misses=0，不宣称执行了其他批次的实际 spilled-target miss 矩阵。

十组静态的 CRC/checksum、Block/Function 模式、共同 PC 的 IR 数量/host 大小、spill 和零 target_overflow 保持。八组完整有序数量/大小记录及 multiset 相同；另两组只有格式化函数的编译单元列表变化，范围外的有序记录相同。boundoff CoreMark Function 为 426/286632 → 427/286724；fixed-class CoreMark Block 为 2415/206400 → 2413/206180（记录数/RA host bytes），不能解释为同单元代码收益。五组 benchmark 的 RA host bytes 仍为 19896、19904、16464、20068、16488。

四组实际 Block dump 重算原始 hash 并核对全部记录长度。两组 benchmark 原始有序字节相同；两组 CoreMark 各七处差异仅限已核对 QueryCpuid 地址装载，归一该类型字段后共同 PC 字节相同，其他字节未屏蔽。此次地址低位有多个同名候选，未沿用“低位唯一”结论：保留全部候选，以基线同一原始日志的既有 helper 证明及当前 decoder_misc.cc 的 QueryCpuid 调用锁定目标，再验证七个 guest CPUID 块、MOVZ/MOVK + BL 形状与每进程一致的推定加载基址；偏移为 0x16c8f0/0x16c940。候选、筛选依据及原始差异分别保留。Function 仍无 body 字节 dump，不宣称十组原始机器码或最终 relocation 后执行缓冲逐字节相同。

### 成本与未完成项

DWARF 显示 RewriteContext 256、ValueWorklist 64、UnitActions 864、JitContext 1408、BlockAnalysisIndex 240、SemanticFacts 624、CompileWorkspace 224 bytes 均保持。

| 函数 | 基线 code/frame bytes | 候选 code/frame bytes |
| --- | --- | --- |
| CollectDead | 888 / 112 | 968 / 112 |
| QueueDead | 268 / 64 | 300 / 80 |
| Erase | 564 / 192 | 596 / 192 |
| ResetDeadOwners | 无独立符号 | 140 / 32 |
| ValueLiveOwner | 无独立符号 | 132 / 0 |

Context LiveOwner 被内联。QueueDead 自身固定帧增加 16 bytes，新 ResetDeadOwners 自身帧 32 bytes；表中帧不代表实际调用链或动态峰值。launcher .text 为 4466740 → 4467148（增加 408 bytes）；按 RewriteContext/ValueWorklist 名称关联并按代码地址去重的符号为 49 → 51，合计 29448 → 29884 bytes，该局部小计不含共享通用子调用，完整代码/栈/堆和时间成本仍开放。

十一份工作区日志均闭合且分项总和正确。十组静态的六项基础分配字段保持，八组全字段相同，两组格式化单元变化只有复用/初始化计数差异。IR-slow 增加了真实新夹具，total 为 365 次申请/释放、6677968 bytes、峰值 3981724、614 retention events，结束 live/retained 均为零；不把这组测试总量当作生产前后成本。旧私有 owner 哈希不在工作区计数内，分配失败用例只证明预热后具体样本，完整私有堆节省和性能尚未验收。没有新增 PMU 或正式 FEX 对照。

source digest：`8068f7e89e238d53b5afff28f1b4d7aa6191413afe24a7dd9778449904108a24`；overlay SHA：`fca05b763dc2f265d7b29dfc6c91848799b383392a9f030a701ac328596f5eb2`。原生归档 14 份成员，SHA：`28f77c3f0a1d61993b0843da1e9101c7e63a86e8d692d99eac65dcbf8630db26`；含静态日志的归档 24 份成员，SHA：`64a7682cbe4052e9b9ea41895f8d62d59737c6e8f659643e71f07e5978a7c2d9`。证据在 `/private/tmp/swiftvm-middle-results` 下以 `r2-dce-owner-roster-v1` 为前缀，十六项 source 检查及同前缀质量门分别记录。

本批只完成活跃 epoch 的 DCE 归属共享。继续候选资格预查、无 epoch 常量运输、类型与 helper/state effects、全局成本、R7 全适用矩阵、并发/RV native、固定 FEX Android 源码及红魔正式三方 PMU。`full_goal_completed=false`、`full_cost_goal_accepted=false`、`formal_fex_comparison_accepted=false`。

## R2 Function flags 状态表与 terminal 需求归并

本批于 2026-10-24 基于 `r2-dce-owner-roster-v1` 实现，最终冻结为 `r2-flags-cfg-state-v2`。仅收口可变 IR 阶段的 Function flags 暂存和查询，不宣称完整 R1/R2、成本或正式性能验收完成。

### 实现与边界

- 删除 live-in 与 transfer 两张逐块哈希，合为 flags.cpp 内部的 FunctionFlagState vector；已有 HIR block order 给出位置，查询同时核对范围与成员指针。成员表含全部现有块，可达 RPO 刷新 transfer；不可达成员的 live-in 初始值保持 None。没有复制 CFG 边，也不导出到 SemanticFacts、Allocation 或 emitter。
- 同一次 EliminateFlags 保留该表及容量；每次必要重算重置 live-in 并根据当前 IR 重建摘要，保留 reverse-RPO 固定点和 flags/DCE 通知驱动的外层固定点。仅首次且启用 branch-only 时计算其前置需求；关闭或已处理首次时，删除原本随后被覆盖的一次计算。branch-only 的真实候选门控仍未完成。
- terminal 直接归并 LinkBlock/LinkBlockFast、If 和 Switch 的需求，不再分配/填充目标 vector。目标 PC 仍在既有 GetSuccessors 中匹配，保留这部分访问成本；不直接采用所有逻辑边，也不将 call-return 自动计为 terminal 目标。
- 缺失边/成员、空块、无 terminal 与其他未知出口仍为 All；CheckHalt/Condition 即使包含内部子 terminal 也保持原观察边界。空 Switch、重复目标、环和局部 label 的既有规则保持；局部 label 摘要哈希未删除。
- 独立 Block 在 Function 表创建前返回，原 branch-only 调用仅传空视图；单块 Function 的必要边界没有被 Block 假设替代。本批修改两份既有源文件，无新增/退役文件或测试脚本、临时 probe。累计 source 清单 465 份、退役 113 份；配置与序列化版本未变。

### 集中验证与暂停接续

v1 新增测试的变量重命名产生重复前缀，构建失败，未冻结/部署 ELF 或执行设备测试。v2 修正测试声明后，十四个 Android 目标构建成功并冻结。本批十项原生检查在暂停前执行；恢复后核对 465 份 overlay 源文件、十四个冻结 ELF 与设备身份一致，保留原始执行记录，不重跑原生检查，再新跑十组静态负载。五个设备 ELF 与两个 guest 的前后七份 SHA 均匹配，原始日志/XML 未改名或重记为其他版本执行。

- 主套件 741 cases、226523 assertions，仍有基线既有的 11 个失败用例/66 条失败断言/零 error；共享 case 顺序与完整失败签名相同，无测试退役。IR fast/slow 各 74 cases、25247/25331 assertions，新用例通过；fixed-register 两项/39 assertions 通过。
- 新用例以独立的路径可达性 oracle 判断输入 flags 是否仍被需求：遇到真实读取/未知观察保留，完整覆盖终止传播，无读取的封闭环不产生需求。二十种 CFG 场景分别运行 HIR fast/slow，覆盖链、环、读后覆盖、空块、缺失目标、嵌套 If/Switch、重复目标、空 Switch 及 CheckHalt/Condition 边界，共四十条 IR 级保留/删除断言；不宣称这四十种图都单独发射执行。
- 六个寿命/Call marker 与本批基线相同，包括 constant-window、prepared Function fast0/1，以及 static/disk 和 AOT Call；本批 Call 模式 target_spill_misses=0，未借用其他批次的实际 spilled-target miss 执行作为本批结果。

十组静态的 CRC/checksum、实际 Block/Function 模式、共同 PC 的 IR 数量/host 大小、spill 与零 target_overflow 保持。七组完整有序数量/大小记录、multiset 和完整 RA shape 相同；另三组差异局限于输出格式化函数的编译单元列表，范围外的有序记录相同：

| 负载 | 记录数：基线 → 候选 | RA host bytes：基线 → 候选 |
| --- | --- | --- |
| CoreMark Block | 2415 → 2412 | 205228 → 204688 |
| fixed-class CoreMark Function | 426 → 427 | 286296 → 286388 |
| fixed-class CoreMark Block | 2413 → 2419 | 206180 → 206696 |

这些总量变化不作为同单元代码收益。五组 benchmark 的 RA host bytes 保持 19896、19904、16464、20068、16488。

另核对四组实际 Block dump 的全部记录长度并重算原始 FNV hash。两组 benchmark 原始有序字节相同；两组 CoreMark 各七个共同 PC 的差异仅限已确认 QueryCpuid 的 MOVZ/MOVK 地址字段，其后 BL 与其他字节相同。以相同基线原日志的已存 helper 证明、未改的 decoder_misc.cc 调用和七个 guest CPUID 块锁定目标；候选地址低位可能匹配的其他符号全部保留，加载基址仍是按符号偏移推定而非 proc maps 观测。只归一该类型字段后共同 PC 字节相同。Function 未采集 body dump，不宣称十组原始机器码或最终 relocation 后的执行缓冲逐字节相同。

### 布局与实际成本

新 FunctionFlagState 为 32 bytes，按 MaxBlockCount 分配槽位，包含空槽；一次调用复用容量，槽位初始化、稀疏成员与局部 label 表私有分配仍需完整计量。这份私有 vector 不计入现有工作区统计，不能将“两表合一”写成零分配。UnitActions 864、JitContext 1408、BlockAnalysisIndex 240、SemanticFacts 624、CompileWorkspace 224、RewriteContext 256、ValueWorklist 64 bytes 均保持。

| 函数 | 基线 code/frame bytes | 候选 code/frame bytes |
| --- | --- | --- |
| EliminateFlags | 896 / 192 | 964 / 224 |
| CleanBlockFlags | 3800 / 464 | 3716 / 480 |
| ComputeFunctionLiveIn | 3932 / 320 | 2672 / 288 |
| ComputeBlockLiveOut | 536 / 176 | 无独立符号 |
| TryBranchOnly | 3068 / 448 | 3072 / 448 |

ComputeBlockLiveOut/FindFlagState 等查询的工作不能因没有独立符号而记为零。terminal visitor 实际由 boost::apply_visitor 承载，独立核对其符号：代码 524 → 684 bytes、自身固定帧 128 → 144 bytes。EliminateFlags 自身帧增加 32 bytes、CleanBlockFlags 增加 16 bytes，ComputeFunctionLiveIn 减少 32 bytes；深 terminal 递归与完整调用链/堆峰值仍开放，不能将各表项相加当作实际峰值。launcher .text 为 4467148 → 4463748，减少 3400 bytes；这不是编译时间或运行性能收益的证明。

十一份工作区日志均闭合且分项总和正确，十组静态的六项基础分配字段保持。七组静态全字段相同，三组格式化单元列表变化仅有 retention_events/pattern_init_bytes 差异。IR-slow 基础分配字段保持，total 为 365 次申请/释放、6677968 bytes、峰值 3981724，retention events 为 614 → 654；新增四十个场景增加了工作区借用事件，不作为相同测试工作量的成本差异。结束时 live/retained 均为零。没有新增 PMU 或正式三方 FEX 性能结论。

### 冻结与接续

source digest：`a9ecb2831b3909ff14b480725ddd1bcf28d8335dbf8e69870687d68aa2358b34`；overlay SHA：`b7506659810042a1c7de4ceaa3c616e83cf4eeb7684e7100754b099b655ee275`。原生归档 14 份成员，SHA：`eda39d9edf94965e686f0b0dee4eed42d5e692203bf565ccedacbdc574129181`；含静态日志归档 24 份成员，SHA：`b73ec53fc4c0a247e9f9a9b9283fc4b70a9c3a85807bfa5adc0ca5fb2ba28490`。证据根目录为 `/private/tmp/swiftvm-middle-results`，前缀 `r2-flags-cfg-state-v2`，source/build/ELF、原生执行、XML/marker、静态 shape、实际 Block 字节、storage、DWARF、函数/visitor 帧及归档身份分别记录；十三项 source 检查通过，综合质量门另见同前缀 quality-gates。

后续仍须收口 branch-only 实际候选触发、其余 flags 临时集合/局部摘要、共享 CFG 查询、初始 value/CSE 资格预查、无 epoch 常量运输、完整私有堆/调用链成本，以及 R7、并发、RV native 与固定 FEX Android 源码和红魔正式三方 PMU。`full_goal_completed=false`、`full_cost_goal_accepted=false`、`formal_fex_comparison_accepted=false`。

## R2 Terminal 只读借用与 branch-only 临时存储退役

本批于 2026-10-24 基于 `r2-flags-cfg-state-v2` 实现，最终冻结为 `r2-terminal-branch-storage-v2`。消除只读 terminal getter 的隐藏递归复制，退役 branch-only 的六份临时容器；不据此宣布完整 R1/R2、成本、R7 或正式性能验收完成。

### 实现与寿命边界

- `Block::GetTerminal()` 从返回值改为返回 `const Terminal&`，Block 继续拥有原 terminal。flags 三处局部读取、ARM64 actions/lowering 及既有 x86 terminal helper 改为引用；直接传给只读分析、分配、发射和公共 use 遍历的调用也不再先隐式复制。解释器的同步 terminal visitor 改为 `const auto&`，避免递归分支替代项的再次复制；RunTerminal 返回后才重新调度，没有在遍历中执行 host helper 或替换 Block。
- 借用在 SetTerminal 或 Block 销毁时失效。HIR reset 在替换前完成 ReleaseTerminalUses；准备/发射保留既有 IR owner，不把 terminal 子节点引用写入持久结果。RISC-V wide 为另一个合成 Block 建立 terminal 的显式快照及既有需要跨替换使用的测试快照保留。复制 terminal 不等于拥有其中 SSA 定义；未将全部显式复制或递归成本宣称为已清除。
- TryBranchOnly 不再复制整块指令，不再用三份 vector 收集 flag writes、polarity stores 和 carry inversions，也不再构造 victims set。一个原始遍历定位 condition 与唯一 marker，保留 producer 窗口及 primary 迭代器，以计数检查原规则；接受后有限重扫窗口，按源顺序删除其他保存/清除/反转/极性写，最后删除 marker。整数删除通知由原 set 顺序变为确定的源顺序，不改变资格规则。
- FCmp 路径不再接受整块 vector 与下标，也不再保存 victims。先在原 producer 窗口中确认 VecFCmp，再按原规则证明 publication 与可穿越指令；全部证明完成后再遍历保留的端点之间删除。两条删除路径均先推进游标，RewriteContext::Erase 仅将相关 DCE 工作排队，当前保留端点继续有效。
- 重复 marker、外部或共享 condition、缺失 AdvancePC、多个 polarity 写、缺失 FCmp publication、carry/宽度和可观察效果等保守拒绝保持；marker 在拒绝后仍由公共清理移除。Function flags 的实际候选前置门控、局部 label 摘要和必要固定点未在本批完成。
- 修改十份既有源文件，没有新增或退役源文件、测试脚本或临时 probe。累计 overlay 为 465 份源文件、退役 113 份；配置和序列化版本未变。生产 getter 清单记录 33 份文件中的 61 处出现，包含声明与定义；清单用于审阅，不单独充当寿命证明。

### 一次集中设备验证

v1 新测试误用不存在的 `Flags::Auxiliary`，编译失败；v2 改为 `AuxiliaryCarry` 后十四个 Android 目标构建成功。v1 未冻结/部署 ELF，也没有设备执行。十四个 v2 ELF 在构建退出零后冻结；红魔 NX729J（`bf1359ff`）上部署实际使用的十个 ELF，执行前后连同两个 guest 的十二份 SHA 均匹配，十四份主机冻结产物也保持不变。

本批新跑八十项原生检查和十组静态负载，各一次。原生范围为现有六十二条路径，加上其余九种寿命模式在快/慢 IR 下的十八项检查，包含取消/发布、构建失败、跨线程、region owner 和 finish retry；不由这八十项推断完整 R7 或 RV native 已覆盖。四份 XML 与最近 flags 批次比较，十项已有原生检查沿用该批次参照，另五十二项保留原 `r1-predicate-query-demand-v2` 的完整矩阵参照名；十八项补充寿命检查只报告实际通过，没有伪造相邻基线。

- 主套件 741 cases、226382 assertions，仍有基线既有的 11 个失败用例/66 条失败断言/零 error；全部共享 case 顺序与失败签名保持，无测试退役。断言总数随既有运行路径变化，不作为工作量相同的成本证明。
- IR fast/slow 各 76 cases、25359/25443 assertions，相比相邻批次各新增 112 条断言。新 terminal 用例检查同一存储的只读借用、嵌套 Value 遍历和替换后的显式快照独立性；branch-only 用例覆盖整数/FCmp × marker 三种位置 × 五种接受/拒绝形状，共三十种 IR 形状。两项新 case 均通过，既有共享 case 签名保持；fixed-register 两项/39 assertions 通过。
- 五十八项既有非 XML marker 按原参照核对：除 constant-window 快/慢各增加五项检查外，其余相同。constant-window 为 123477 checks、零 injected failures，optimized-function 仍为 123494 checks；十八项补充寿命 marker 的实际 mode/fast 和通过状态分别核对。construction、retained、finish-retry 中既有预期故障注入分别保持，不能概括为所有模式均零注入。
- 新增五项寿命检查覆盖：嵌套 terminal getter/公共遍历不触发既有 C++ new 失败钩子、两个真实 Value 保持、快照独立，以及一个独立 Block 的完整 EliminateFlags 命中不触发该钩子且保留 BranchOnlyFlags。该夹具关闭普通 DCE、没有 Adc/Sbb，不代表 Function、FCmp 全部路径或 malloc/arena 分配均为零。Call 路径实际记录十二次 spilled-target miss，未借用其他批次的执行次数。

十组静态的 CRC/checksum、实际 Block/Function 模式、共同 PC 的 IR 数量/host 大小、spill 指标及零 target_overflow 保持。八组完整有序数量/大小记录、multiset 和 RA shape 相同；另外两组只在 CoreMark 格式化符号内改变单元列表，范围外有序记录相同：

| 负载 | 记录数：基线 → 候选 | RA host bytes：基线 → 候选 |
| --- | --- | --- |
| CoreMark Block | 2412 → 2416 | 204688 → 204988 |
| CoreMark fixed Block | 2419 → 2412 | 206696 → 206016 |

四组实际 Block dump 均重新计算并核对原始 FNV/字节长度。两组 bench 的完整有序原始 dump 相同；两组 CoreMark 的七个共同 PC 仅在同一 x11 四条 MOV 宽立即数中的三处地址字段变化，后接 BL0。沿用同一参照日志的 typed CPUID 证明、核对未改变的 frontend 和冻结 ELF 的 QueryCpuid 符号后，只对该地址字段归一化，其余共同字节一致。七处 guest 均在首次控制转移前出现 CPUID；16KB 加载偏移仍是推断，候选低位匹配的其他符号保留，不声称仅凭低位唯一确定 helper。该结论限于已采集的 pre-relocation Block dump，不包含 Function body 或最终重定位字节。

### 存储与静态代码成本

Block 为 256 bytes，另七个既有 owner 的 DWARF 大小保持：UnitActions 864、JitContext 1408、BlockAnalysisIndex 240、SemanticFacts 624、CompileWorkspace 224、RewriteContext 256、ValueWorklist 64；未新增持久 owner。十一份工作区日志闭合且分项总和正确，六项基础分配字段均保持；八组静态和 IR-slow 全字段相同，另两组格式化列表仅影响 retention_events/pattern_init_bytes。私有 C++ 容器及递归堆成本不在该工作区口径内。

下表均为冻结 ELF 中对应独立函数或明确关联 visitor 的代码大小/自身固定栈帧，不能相加当作实际调用链峰值：

| 函数或 visitor | 基线 code/frame bytes | 候选 code/frame bytes |
| --- | --- | --- |
| Block::GetTerminal | 12 / 0 | 8 / 0 |
| TryBranchOnly | 3072 / 448 | 1976 / 304 |
| TryFCmpBranchOnly | 1148 / 160 | 512 / 112 |
| EliminateFlags | 964 / 224 | 928 / 208 |
| CleanBlockFlags | 3716 / 480 | 3596 / 480 |
| ComputeFunctionLiveIn | 2672 / 288 | 2640 / 272 |
| UnitActions::IntegerBranch | 416 / 80 | 388 / 64 |
| LowerIntegerBranch | 7060 / 432 | 6800 / 416 |
| TargetPreparation::Complete | 7740 / 1728 | 7756 / 1744 |
| 解释器 terminal 关联 visitor | 1228 / 176 | 684 / 96 |

旧 getter 以尾调用进入 boost variant 复制构造，其直接 callee 为 680 code bytes/128 frame bytes；新 getter 直接返回成员地址。复制构造仍供必要快照使用，不能把它记为全局退役。RunTerminal 在所查 ELF 没有独立符号，表中解释器项为其关联的 outlined visitor；Function flags 的 TerminalLiveOut 关联 visitor 则保持 684 code bytes/144 frame bytes，两者不是同一项。准备函数自身帧增加十六字节；深 terminal、通用 callee、完整栈链和私有堆峰值仍开放。launcher .text 为 4463748 → 4458412，减少 5336 bytes；没有新增 PMU 或正式三方 FEX 性能结论。

### 冻结与接续

source digest：`fc412f681426eacca664e5a3ab56c63fa9cad8d1be4bd653e57ac825573d99db`；overlay SHA：`2dbb679a7023dfc2c78d7f93df98398c7e1fa60d2071b6ca82835dcaf6d05c43`。原生归档 84 份成员，SHA：`962528f827cacfe721b6a5a0da5e7583f836a42e0fd48882575716bcf18ed3d8`；含静态日志归档 94 份成员，SHA：`8876cc6fd6c10f28a144e4f25261c882c7078a5699e01288bfa2825bdce32b28`。每份原始成员均核对；证据根目录 `/private/tmp/swiftvm-middle-results`、前缀 `r2-terminal-branch-storage-v2`，source/build/ELF、设备身份、执行清单、XML/marker、shape/Block 字节、storage、DWARF 与 getter/visitor 帧分别记录。质量门只接受本批作用域，完整 goal、成本与正式 FEX 比较保持开放。

继续 branch-only 的真实候选触发、其他 flags 暂存/局部 CFG 查询和剩余共享准备成本；保留 Block 与 Function 的不同边界、必要 DCE/CFG 固定点及显式快照。当前集中矩阵不替代其余完整故障/并发、RISC-V native 和正式三方 PMU 验证。

## R2 Flags 候选门控与 carry 区域数组退役

本批于 2026-10-24 基于 `r2-terminal-branch-storage-v2` 实现，最终冻结为 `r2-flags-demand-carry-v2`。修改 flags.cpp、passes.h 和既有 ir_contract_test.cpp 三份源文件；没有新源文件、临时 probe 或测试脚本，累计 overlay 465 份、退役 113 份。只接受本批候选触发与 carry 清理作用域，完整 R1/R2、成本、R7 及正式 FEX 性能仍开放。

### 实现与边界

- BranchOnlyCandidate 在既有 RPO 访问中检查 If、两条 LinkBlock、唯一使用的 condition 及三类原 condition opcode，所得只读 If 视图直接交给资格证明，不另做一轮资格预扫。首次且开关启用时，遇到第一个这类候选才在外层准备原有 live-in；无候选则跳过专用前置计算。快照在任何 branch-only 修改前建立，包含 marker 已证明边的候选，后续候选沿用原快照。晚期 producer 拒绝和 marker-only Function 仍可能准备；不宣称所有未命中路径均零准备。
- 独立 Block 不创建 Function 状态；Function 原有成员/transfer/live-in 表、未知出口和必要 CFG/flags/DCE 固定点保持。求解器留在 EliminateFlags 的外层调用栈，未加入延迟 context 或更深的候选回调。只写不读的 BranchOnlyStats 五个字段、二十一处递增及传参已删除，原 ACCEPT 文本诊断保留；配置、序列化与可见统计接口未改。
- carry 清理删除 Region vector 与保护位 vector，也删除逐 consumer 重复回溯 producer 的循环。进入尚未编辑的区间前一次逆向归纳，用常量大小的 pending 状态传播；到达 writer 时截断后续 consumer 的需求，当前区间同时是 consumer 时仍继续请求更早 producer。原 writer opcode/mask 清单不变，不将 InvertCarry 等操作擅自改作新边界。旧 ClassifyCarryRegions 定义和 passes.h 声明退役；没有 carry consumer 时不扫描区域，关闭 carry 消除且有 consumer 时沿用原直接回退。每轮区域分类访问和清理逆扫各自保留，不宣称只有一遍访问或全部私有堆成本已闭合。

v1 已成功构建、冻结十四个 ELF，并部署五个 ELF 核对七份设备/guest 身份；尚未执行任何设备测试或静态负载。静态成本检查发现，其延迟 context 从 TryBranchOnly/FCmp 内调用全函数求解，使求解器嵌入更深栈链，故未验收。v2 将候选门控放回外层，并清除只写统计，重新构建十四个目标后冻结。v1 源码、ELF、成本及部署记录保留，设备执行次数明确为零，不将 v2 的结果记作 v1 执行。

### 集中验证

红魔 NX729J（`bf1359ff`）新跑十项原生检查及十组静态负载，各一次。五个实际使用的 ELF 加两个 guest 的七份 SHA 前后相同，十四份主机冻结 ELF 保持。此批只改 flags/pass 接缝，未重复相邻批次已运行的八十条矩阵；本批结果不替代完整 R7、RV native 和正式 PMU。

- 主套件 741 cases、226472 assertions，保持既有 11 个失败用例/66 条失败断言/零 error；共享 case 顺序和失败签名相同，无测试退役。IR fast/slow 各 78 cases、29599/29683 assertions，各新增 4240 条；fixed-register 两项/39 assertions 通过。
- carry 新用例以独立的“consumer 之前有无中间 writer”路径查询作 oracle，四组有效区间的全部 256 种写/读组合 × ADC/SBB × 消除开/关，共 1024 组实际 IR，包含透明入口和连续空尾区间。检查原保护边界与其前置未保护区间的 PF 写保留，共 4096 条断言；C writer 分别包含 ClearFlags、SetCarry 和 SaveFlags。不将这些 IR 形状当作单独的 native 指令执行或全部 writer 家族覆盖。
- Function 用例组合 fast/slow builder、branch-only 开/关、后继是否读取 C、整数 marker/FCmp marker/重复 marker 三种入口，共 24 组图；每图在同一 RewriteContext 连续调用两次，核对入口与后续 unmarked 候选的决策及 marker 清理，共 144 条断言。六个既有寿命/Call marker 与相邻批次相同，包括 constant-window 的 123477 checks、optimized-function 的 123494 checks，四个寿命模式均零 injected failures。本批两个 Call 模式实际 target_spill_misses=0，没有复用上一批十二次 miss 作为当前执行结果。

十组静态的 CRC/checksum、实际 Block/Function 模式、共同 PC 的 IR 数量/host 大小、spill 与零 target_overflow 保持。八组完整有序记录、multiset 和 RA shape 相同；另外两组仅在格式化符号中改变单元列表，范围外有序记录相同：

| 负载 | 记录数：基线 → 候选 | RA host bytes：基线 → 候选 |
| --- | --- | --- |
| CoreMark bound-off Function | 427 → 426 | 286724 → 286632 |
| CoreMark fixed Block | 2412 → 2415 | 206016 → 206336 |

四组 Block 原始 dump 的长度和 FNV 均重算。两组 bench 的完整有序原始字节相同；两组 CoreMark 的七个共同 PC 只改变 typed CPUID helper 物化的立即数字段。使用同一参照日志已有证明中的 MOV 组位置，核对四条 x11 MOV、BL0、未变 frontend、冻结符号和一致的推断加载偏移，再只对 imm16 字段归一化，其余共同字节相同。普通 Block 本次只有前两条 MOV 的地址字段变化，fixed Block 为前三条；不要求每次 ASLR 恰好改变三个字段，也不放宽到整条指令或任意地址。保留低位候选歧义和加载偏移属于推断的限制；不扩展为 Function body 或最终重定位字节等价。

### 存储与代码成本

八个原 owner 的 DWARF 布局保持，FunctionFlagState 仍为每槽 32 bytes，无新增持久 owner/context。十一份 workspace 日志闭合且分项求和正确，六项基础分配字段保持；八组静态与 IR-slow 全字段相同，两组格式化列表变化仅影响 retention_events/pattern_init_bytes。私有容器、深递归及总编译堆峰值不在该 workspace 口径内。

| 函数 | 基线 code/frame bytes | 候选 code/frame bytes |
| --- | --- | --- |
| EliminateFlags | 928 / 208 | 1060 / 208 |
| CleanBlockFlags | 3596 / 480 | 3704 / 464 |
| TryBranchOnly | 1976 / 304 | 2168 / 288 |
| TryFCmpBranchOnly | 512 / 112 | 无独立符号，工作并入 TryBranchOnly |
| ClassifyCarryRegions | 892 / 112 | 已退役 |
| CarryRegionProtected | 无 | 284 / 64 |
| ComputeFunctionLiveIn | 2640 / 272 | 2640 / 272 |

候选识别没有独立符号，代码进入两处调用方；TryBranchOnly 的代码增长包含 FCmp 内联，不能将无独立符号当作零成本。EliminateFlags 中保留两处静态求解调用点，分别受候选门控和必要固定点控制；TryBranchOnly/CleanBlockFlags 没有调用全函数求解，不据静态调用点推断实际运行次数或完整栈峰值。Function flags terminal 关联 visitor 保持 684 code bytes/144 frame bytes；准备与其他已查读者自身帧保持。launcher .text 为 4458412 → 4457636，减少 776 bytes，部分调用方代码增长及内联边界如表记录；没有本批 PMU 或正式性能收益结论。

source digest：`1a684a608533bb979e8cbded5088b212b27a1dad1d8807d6d84e667a9dc2c77f`；overlay SHA：`b1ce7aad087892454a29d3880efba59392e935ec88bc47a7100d9341672c24ff`。原生归档 14 份成员，SHA：`569cd9f968eb9f2a32824e27bc3fab7dbc2518c6c24b629d6da7723fb48adc06`；含静态日志归档 24 份成员，SHA：`875477114c063765620648a5a642281783ca5d61241391e2e68c8c3989932a7f`。全部原始成员逐份核对；证据根目录 `/private/tmp/swiftvm-middle-results`、前缀 `r2-flags-demand-carry-v2`。继续晚期候选准备、局部 label 摘要、其他 flags/publication 暂存与完整成本，完整故障/并发、RISC-V native 和正式 FEX Android 三方 PMU 验收仍开放。

## R7 RV64 构建、实际执行与 Android 接缝回归

批次：`r7-rv64-execution-20261024-v5`；相邻已验收源码为 `r2-flags-demand-carry-v2`。本批收口共享中端在真正 RV64 目标上的构建与现有用例执行，不缩小完整 R0–R7 目标。相邻版本仅修改五个既有源文件，无新增、无本批退役；累计源码覆盖仍为 465 项、已退役 113 项。原始产物位于 `/private/tmp/swiftvm-middle-results/` 下的同名批次前缀。

### 接缝修正

| 文件 | 本批修改与边界 |
| --- | --- |
| `source/translator/x86/translator.cpp` | 按 Config 声明顺序排列 rcpc_fault_emulate、region_edges 的 C++20 designated initializer，修复 GCC 拒绝；配置值不变 |
| `source/runtime/CMakeLists.txt` | 显式声明 runtime 对现有 x86 helper 实现的静态链接依赖。使用 INTERFACE 的 LINK_ONLY，GNU ld 得到必要的静态组件重复；runtime 编译命令没有继承 frontend/SoftFloat 的定义 |
| `source/tests/riscv/native_wide_test.cpp` | 四处 StoreUniform 迁到现有 typed 接口；TSO/plain 差分夹具通过 RewriteContext 改写指令并同步 uses，不恢复 const Inst 的可写入口 |
| `source/tests/riscv/backend_test.cpp` | loop-carried Phi 在 EndFunction 完成成员名册后、优化前修补输入；保留 Function writer 的成员检查 |
| `source/tests/riscv/pure_scalar_flags_test.cpp` | uniform-load-width 先合法 typed 构造，再显式制造导入 IR 的声明错配，继续核对 admission 按实际访问宽度拒绝；不放宽 builder |

### 构建与失败中间版

RV64 使用 Debian ARM64 容器中的 GCC 14.2、`source/cmake/riscv64-linux-gnu.cmake`、`rv64g/lp64d` 和 Release，完整十四目标构建退出码为零后才冻结 ELF。首次尝试因 designated initializer 顺序失败，第二次暴露旧夹具接口及 GNU ld 的 helper 链接缺口；这些失败构建没有用于测试。第三版构建成功但未执行，随后将依赖限制为 LINK_ONLY，避免 frontend 编译定义传入 runtime。

第四版冻结后首次执行 37 项，23 通过、14 失败；九项 cache 子进程首先缺少目标 loader。设置继承的 QEMU_LD_PREFIX 后，这九项中的四项通过，其余五项仍失败。另五项是未完成 Function 名册下的 Phi 改写和 typed builder 提前拒绝负例。原始失败 XML、LastTest.log、ELF 和源码均保留；没有删掉用例或放宽生产检查。第五版只修正后两类夹具，跨 hart/seccomp 五项转到目标内核执行。

用户态 QEMU 固定为 10.0.13，应用仓库已有的 riscv-flush-icache-qemu10.patch。预生成的 RV64 vDSO blob 也从修正后的汇编重建，反汇编确认 syscall 259；没有应用旧 QEMU 8 vector-shift 补丁。冻结的用户态 QEMU SHA-256 为 `ff87e18d6498ce2238ad10316437539dd6942b269dab13b5fb24a63c3a394cbe`。

### RV64 执行证据

| 覆盖来源 | 项数 | 证据范围 |
| --- | --- | --- |
| 第五版用户态新执行 | 14 | pure flags；backend 默认、Block、VLEN 128/256；cache reuse；三种 persistence；Zbb、scalar/vector crypto、Zacas。两次集中运行分别 5/5、9/9 通过 |
| 第五版 guest Linux 内核新执行 | 5 | cross-hart、fail-ordinary、fail-signal、warm-fail-signal、fail-data，全为退出码零 |
| 第四版原始成功结果复用 | 18 | 首轮 14 项，加 loader 前缀后四项 cache 用例；所用 ELF 与第五版字节相同，原始版本、时间、命令及日志不改名 |
| 合计 | 37 | 与 CTest 注册名逐项对应，未跳过或删除；属于组合证据，不声称新跑了一次完整 37 项 CTest |

十四个 RV64 ELF 中只有 backend_test 和 pure_flags_test 相对第四版变化，因此重新执行它们涉及的十四项用户态用例。覆盖集合实际使用十个 ELF。独立 TSO ordering、两种 compile-cost 程序和 emit 工具本批仅构建，未作为已执行用例计入。

内核验证采用 QEMU system 10.0.13、Debian Linux 6.12.113、两个 TCG hart、SMP 与 seccomp，启动后执行同一个冻结 raw-cache ELF。cross-hart 完成 128 轮、2048 次远端指令读取；四种拒绝路径核对目标 syscall 失败后的子进程退出 127、没有继续执行。这里只证明 RV64 guest 内核中的正确性，不作物理 RISC-V 硬件或性能声明。

`r7-rv64-execution-20261024-v5-test-summary.json` 保存 37 项到原始日志/哈希的映射；`-kernel-artifacts/` 保存实际使用的 system QEMU、Image、内核配置、OpenSBI、rootfs 和已嵌入的 init。这个仓库外的最小启动编排只执行既有测试模式，没有向仓库加入 probe 或一次性测试脚本。

### 红魔 Android 集中回归

同批 Android/ARM64 十四目标构建已确认完整退出码零并冻结；八个 ELF 相对上一批变化、六个字节相同。指定设备仍为 NX729J / `bf1359ff`，本次实际执行的 launcher、call persistence 和两个 guest 的四份设备哈希在前后均匹配。

新执行 static/AOT-static 两项 native，以及 CoreMark/bench 的 Function、Block 四项静态负载，六项均退出零。AOT 顶层 99 checks；磁盘 write/read/reject 分别 76/74/7 checks，write/read 的 host image 地址不同，磁盘 read 为 loaded=2、compiled=0；所有既有行为标记与上一批相同。IR fast/slow 的程序字节相同，沿用上一批 78 用例、29599/29683 断言及原始 XML，不冒充本批新执行。主套件本批未重跑；11 失败用例、66 失败断言只作为上一批历史基线保留。

CoreMark CRC 为 0x4983，bench 五项 checksum 与参考一致；四种负载 spill 指标相同且 target_overflow 为零。Function 的两种日志代码记录和 Block bench 完全相同；Block CoreMark 的记录数为 2416→2412、RA host bytes 为 204988→204688，差异仅在固定 guest 符号表的格式化范围，范围外有序记录相同。两份 Block dump 已重算哈希；bench 全字节相同，CoreMark 七处 CPUID 目标只改变四条 MOV 的立即数字段，既有类型锚点、未变 frontend 与两版 helper 符号共同核对。进程 load bias 仍是推断值，没有冒充实际观测。

四种负载的九类工作区全部闭合，六项分配/释放基础计数相同。三种全部字段相同，Block CoreMark 仅 retention_events、pattern_init_bytes 随格式化路径变化；不据此声称完整私有堆成本已验收。Android launcher 的 .text 为 4457636→4457668（+32 bytes），链接器 Cortex-A53 843419 跳板为 15→19、每个八字节；其余有名函数尺寸集合相同。没有把代码尺寸或模拟器耗时作为性能收益。

设备日志已在设备上打 tar 后原样 pull，设备/主机包哈希相同，六成员包 SHA-256 为 `bfc14995a79b52ae0682f3d2a8a7f2b66109edd2002dbe1f73b1fa04bebded4d`。第一次 exec-out 经 su PTY 的传输插入 CR，坏传输另存；仅重新收集原始文件，没有重跑测试或修补日志。

### 冻结与剩余范围

源码摘要为 `ed2f1dffcaeeb699141f50940bdd52e503373ee1ce376cf7a4d849421db0728f`，源码 overlay SHA-256 为 `99c0e5c30b80f061db3266c1608ebf7a4cbcd7992ca6fb98a3df41532370acd4`。构建前完整输入快照另含 vendored 依赖，共 2545 文件，摘要为 `14716aa52a474ccba1a77bd58043fd9b05088c10e0380b4cc1b93fbd7c012a8d`；验收后文档单独冻结，不把文档更新冒充编译输入变化。

本批的验收范围是上述五文件接缝、RV64 已注册用例的逐项正确性证据和 Android 定向回归。完整 R7 的故障/并发/关闭及其他适用矩阵、R1/R2/R3/R5 的剩余收敛与整体成本、固定源码 Android FEX 和红魔正式三方 PMU 仍未完成。Docker 已恢复可用，不能继续沿用旧环境不可用的阻塞判断；设备 FEX 的可复现源码和正式对照仍需接续。

## R3 必要宽度复证的候选范围与私有索引退役

批次：`r3-width-contract-index-v2`；源码相邻为 `r7-rv64-execution-20261024-v5`，静态十组完整参考沿用 `r2-flags-demand-carry-v2`。本批修改 `backend/arm64/compile/width_contracts.cpp` 和既有 `tests/pinned_gpr_plan_test.cpp`，无新增/退役文件；累计源码仍为 465 项、已退役 113 项。完整目标继续沿用方案 16.1，不把本批当作整个 R1/R3 或 R7 完成。

### 实现与证明边界

旧 WidthContractCheck 为 owner、owner prefix、unavailable、fixed clobber、all clobber 各保存三十二份 vector，共 160 份；任意必要宽度候选都会触发它们的收集。现在改为两份短期缓冲，并把点位和 owner 原来分开的两次 IR 收集合为一次。

- 原资格检查改为完整候选查询描述：在每个 elision 的指令位置读取 WholeValue 物理 home，分别记录 bridge 与 low32 copy 所需范围；它仍是一遍资格扫描，不宣称消除了全部预扫。无候选不建立检查器；仅 host-read 省略不建立这两份索引。必要共享 resolved-use 图与 terminal 集合按原语义保留。
- 点位只收实际查询 home：bridge 使用 unavailable/fixed clobber，copy 使用 unavailable/完整 clobber。完整 clobber 继续包含 SetHostGPR 与真实 helper ABI；用 home、种类和完整 u32 指令位置组成 u64 键，排序后作闭区间查询。单条点位从四字节变为八字节，不能只按容器头减少推断所有输入的堆字节都降低。
- owner 只为 bridge 查询 home 收集，按 home/真实定义 ID 排序；前缀最大 use-end 归入 owner 本身。birth 直接读取不可变定义 ID，preserved 前缀与 suffix 用同一个 transfer 和前/后标志表达，transfer 当点选择 suffix；不修改物理 mapping、活跃区间、分配重试或发表动作。
- PrepareWidthCertificates 保留必要断言和完整拒绝；CheckWidthContracts 保留同义只读检查。普通 fixed/scratch 检查、所有目标编辑后的 final fixed 复证与 FPR retry 没有删除。语义 U32/已知位仍不作为物理 X 高半部证明；没有给 SemanticFacts 增加宽度表，也没有据此完成完整单元语义宽度摘要。

### 行为用例与失败中间版

在既有 pinned_gpr_plan_test 中增加两个行为用例：40 组检查普通 owner、早生长区间与后生短区间、preserved 前缀/suffix 及 transfer 前/当点/窗口后；78 组检查 bridge/use 闭区间端点、窗口后的空闲变化、相同/不同 home 的发表、fixed scratch 和 caller/callee helper clobber。每组分别核对可选检查与必要证明的成功或拒绝；x30 的普通映射由后一组覆盖，没有暴露私有索引来测试其内部布局。

第一版十四目标构建成功并冻结，主套件执行退出 67：原有 66 失败断言之外，新增夹具把 x30 用于不支持它的 fixed/preserved 前缀，触发 PlanFixedGPRDefinition 合法性检查，形成一个 error；其余批次当时未执行。改为合法的 x29/备用前缀后形成第二版；生产源码没有变化，十四个 ELF 中只有 swift_test 与第一版不同。第一版 XML、源码、ELF、两成员原始结果包及 intermediate-record 保留，未放宽生产检查或把错误版记为通过。

### 红魔集中验证

第二版十四个 Android/ARM64 目标构建完整退出零后冻结；相对源码相邻版十四个 ELF 都有变化。本批在 NX729J / `bf1359ff` 新执行十二项 native 和十项静态负载，没有复用旧测试结果；实际使用五个程序和两个 guest，七份设备哈希在前后均匹配。没有向仓库加入 probe 或一次性测试脚本。

- 主套件 743 用例、226911 断言，新增两个用例均通过。原 11 失败用例、66 失败断言逐项签名和顺序保持，没有新增失败或 error；不能称全套件零失败。
- IR 快慢路径各 78 用例、29599/29683 断言，fixed-class 为 2 用例/39 断言，均通过。四项 static/indirect、磁盘/AOT 调用持久化及四项快慢 IR 的 optimized-function/constant-window 寿命检查通过；八个顶层行为标记与各自原始参考相同，实际 target-spill miss 计六次。寿命 checks 仍为 123494/123477，injected_failures=0，不把标记数量当作完整堆成本。
- CoreMark/bench 的默认 Function、bound-decode off、独立 Block、fixed-class Function/Block 十组全部正常退出，CRC 0x4983 和五项 checksum 匹配；spill 指标不变，target_overflow=0。八组有序代码记录完全相同；另外两组差异只在固定 guest 符号表的格式化范围，范围外有序记录相同。boundoff-core 的记录数 426→427、RA host bytes 286632→286724；fixedclass-block-core 的 RA 指标相同，仅记录顺序变化。
- 四份 Block dump 已重算哈希：两种 bench 全字节相同，两种 CoreMark 各七个 CPUID 目标只改变四条 MOV 的立即数字段。沿用同一参考日志的类型锚点、未改 frontend 和两版 helper 符号共同核对；进程 load bias 仍为推断值，没有冒充实际观测，也不由 Block dump 推断 Function 机器码全等。
- 十组静态和 IR-slow 的九类既有工作区全部闭合，六项基础分配/释放计数均相同。十组全部字段相同；boundoff-core 仅 retention_events、pattern_init_bytes 随格式化路径变化。这些计量不包含全部私有 width 索引容量。

### 存储、代码与计时

DWARF 中 WidthContractCheck 从 3936 降到 144 bytes，新增查询描述为 12 bytes。owner 本体仍为 24 bytes，原来另有每项四字节 prefix 表，现在内联后合计为 24；点位的四→八字节变化和收集范围收紧一并记录。公共 owner/facts 类型没有修改，这也不是所有私有堆、初始化或跨 attempt 复用都完成的证明。

| 自身 outlined 函数 | 参考→本批代码 bytes | 参考→本批固定栈帧 bytes |
| --- | --- | --- |
| PrepareWidthCertificates | 504→512 | 4160→352 |
| CheckWidthContracts | 236→388 | 4080→320 |
| 检查器构造 | 2344→2196 | 288→304 |
| Verify | 8824→8416 | 432→416 |
| 析构 | 360→136 | 48→32 |
| PossibleOwners | 216→344 | 0→0 |
| CollectWidthContractQueries | 新增 688 | 新增 192 |
| HasPoint | 新增 outlined 112 | 新增 0 |

表中固定 SP 减量已包含 shifted immediate；没有把调用者/递归路径、共享 STL 函数或 transitive 栈峰值记成这些自身帧。Android launcher .text 为 4457668→4457100（−568 bytes），不作为整体性能收益。

已有 SVM_PROF2 日志中，默认 CoreMark Function 的 verify_width 同为 4792 次，单次累计计时为 12728173→3809702 ns；十组原始阶段计时均保留，编译数量变化的格式化路径另标。这里只是启用计时后的单次观察，不替代重复采样、PMU 或整体编译成本验收；本批没有新跑 PMU 或 FEX。

### 冻结与接续

源码摘要为 `cac1c064395733de13f30927b8cfb8699e33ee1cdc74f8327434c1c94f2eafa2`，源码 overlay SHA-256 为 `a8a0eabe8adfde691a866bfc66849bdc82148d47a22d89d91dd48ee1b74c1c1a`。十六成员 native 包 SHA-256 为 `db6d1d4a9239fd61c5693c66c81279d85d5501e0a08c8164961b281224c11cdc`，二十六成员完整结果包为 `db0657ec89c9abff08fc9829339d2758db8bd0aa54ad1f7211f17922b8f852f2`；原始字节已核对。

其余资格扫描、这两份私有缓冲的实际容量/总体堆成本与工作区复用仍需收口。R1 完整语义宽度摘要、R3 其他 fixed/spill/helper 接缝、R5 其他候选/发表暂存、R7 完整适用故障/并发/关闭矩阵及固定源码 Android FEX/红魔正式三方 PMU 均保持开放。本批没有修改共享中端或 RISC-V 源码，也没有把上一批 RV64 执行改名为本批新结果。

## R0 固定源码 FEX Android 构建与首次设备运行

批次：`r0-fex-android-rebuild-20261024-v3`。本批只修改仓库外的 Android FEX 参考分支，没有修改 SVM 生产源码或增加仓库 probe/一次性测试脚本。SVM 最近已验证源码仍为 `r3-width-contract-index-v2`；本节不把它的设备结果改记为本批新执行。

### 源码与构建身份

Android 构建源码为 `https://github.com/BulkanDrivers/FEX-android.git` 的 `a156752fae3a4f65fefb1ed95af980bcdfac43d7`（2025 年 8 月 23 日），与方案 2.4 的官方设计参考 `3b7daa5e8dc004f6e9cec3a877f3f3f1ddd6ce11` 不同。旧设备 FEX 的 SHA-256 仍为 `5bcc2db652dd0c7e9b4a6acdbdb6058a0db3ae650171b86054589a46522b1898`，其源码来源尚未确认；不能把本次重建身份追认给旧二进制及历史采样。

六项实际使用的 submodule 已按该提交锁定，依赖工作树均干净；记录包含各依赖提交。使用 NDK `29.0.13846066-beta3`、Clang 21、Android API 30、arm64-v8a、Release、静态 libc++，关闭 LTO、jemalloc、thunks 和 FEX 自身测试。本配置只用于当前 64 位静态 x86_64 guest 的运行链路验证，不声称已经支持 32 位 guest 或 native thunks。

前两轮完整构建退出非零，分别暴露 Bionic/libc++ 接口及信号/ELF 类型差异；没有从失败构建中取旧产物测试。平台适配涉及十一份已有源文件，处理显式头文件、回调存储的所有权、信号掩码、统计文件路径、ELF 类型以及 Android x18 约束。x64 动态 GPR 池由七个变为六个；这是平台 ABI 的实际代码生成约束变化，不能表述为所有代码生成行为不变。Android 将 x18 留给 ShadowCallStack，依据为 [NDK ABI 文档](https://developer.android.com/ndk/guides/abis#arm64-v8a)。

第三轮 FEXLoader/FEXServer 两目标完整构建退出零、零编译错误后冻结。适配 patch SHA-256 为 `3674eaf10401124e11a4c8b55b0795743da5207b04319ecf90cf0b99d7fe32ae`；ELF 及其系统动态依赖已记录：

| 产物 | SHA-256 | 字节数 |
| --- | --- | --- |
| FEXLoader | `69be0441b48221a9be09b25706685485b71ea35ac905b564e2c62ca4b711abc6` | 4182488 |
| FEXServer | `4267db5d574731b90830bb4498a93b6726b3001dfcbd4ad2ef3c7285c50dfed8` | 843304 |

源码、patch、依赖、完整构建命令/日志、ELF 元数据和冻结产物保存在 `/private/tmp/swiftvm-middle-results/r0-fex-android-rebuild-20261024-v3-*`。`--version` 输出为 `FEX-Emu ()`，没有可用的内嵌提交标识；本次身份以冻结的提交、patch 与 ELF 哈希为准，正式交付仍须保留可审阅的构建和版本标识。

### 红魔实际结果与未通过边界

在 NX729J / `bf1359ff` 的独立目录 `/data/local/tmp/svm-middle-fex-a156-20261024-v3` 部署，并使用独立配置、数据目录和 socket；关闭 AOT IR 读写，沿用 CPU 4 的 `taskset 10`。前后两次哈希核对覆盖 FEXLoader、FEXServer、配置及两份 guest，全部匹配。

`--version` 退出零。随后 CoreMark 2000 命令约 5.30 秒后退出 255，日志显示 socket 连接的 errno 111/2 及 `FEXServerClient: Failure to setup client`，尚未开始执行 guest，没有 CRC 结果；未继续 bench，也没有采集 PMU。服务启动失败的根因仍待定位，不能从这一条错误推断 guest 翻译或性能表现。清理命令使用相同私有环境，未操作用户其他 FEX 实例。

执行命令、退出码及原始日志哈希见 `r0-fex-android-rebuild-20261024-v3-device-executions.json`，前后身份见同批 `device-before.json`/`device-after.json`。构建通过不等于运行通过：本批 `formal_FEX_gate_passed=false`。后续运行链路修复及新产物见下节；历史失败记录和二进制身份保留，R0–R7 完整目标继续开放。

### 冷启动目录修复、仓库配方与干净重建

接续批次为 `r0-fex-android-rebuild-20261024-v5`。新增 [tools/fex-android/README.md](../tools/fex-android/README.md) 与 [android-ndk-r29.patch](../tools/fex-android/android-ndk-r29.patch)，固定远端提交、六项 gitlink 依赖、NDK/API/ABI、完整 CMake 命令及平台适配；没有增加测试脚本。SVM 生产源码没有变化，最新 SVM 回归仍归 `r3-width-contract-index-v2`。

v3 的有界前台诊断超时退出 124，服务在这次启动中建立 socket；随后同一份 v3 ELF 在显式预启动服务时通过 CoreMark 2000 与 bench。此组是正确性诊断，不能将前台服务或 adb 的等待耗时记作正式性能。v4 从远端固定提交及仓库补丁在全新源码/构建目录完整重建成功，嵌入版本为 `a156752-dirty`；全新设备目录中的前台服务却退出 255，报告无法创建 `data/Server/`，检查发现该目录实际已经创建，尚未执行 guest。

首次目录创建代码把 `create_directories` 的布尔返回值直接当作成功状态。当前补丁改为检查错误码和实际目录类型，允许已成功建成目录的情况及并发创建后的复用，保留真正错误的拒绝；错误日志同时记录实际错误码。另将无 tag 时的嵌入版本获取改为 `git describe --always --dirty --abbrev=7`。连同原平台适配，补丁共涉及十三份已有 FEX 文件，SHA-256 为 `4ade908f47d73a89d9a883f70197a5824249a4d1f53f45dd4e3df868690e532a`；不改变 FEX IR pass 算法，x18 的物理 ABI 约束变化仍按上节记录。

v5 再次从远端固定提交及该补丁创建干净源码/构建目录，六项依赖均按锁定提交且工作树干净。patch 检查、配置和 FEXLoader/FEXServer 两目标完整构建全部退出零，之后冻结产物；NDK 为 `29.0.13846066-beta3`、Clang 21，CMake 4.2.3、Ninja 1.13.2，其他构建配置沿用上节。

| 产物 | SHA-256 | 字节数 |
| --- | --- | --- |
| FEXLoader | `8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175` | 4182680 |
| FEXServer | `9fe58a9946e9105c49911518356fe29a4ed3143c1fe90803b57afe112c225e8c` | 843688 |

NX729J / `bf1359ff` 使用全新的 `/data/local/tmp/svm-middle-fex-a156-20261024-v5`，未预启动服务，CPU 4，独立数据/配置/socket，关闭持久化 object/AOT IR 缓存。本组设置 `FEX_HOSTFEATURES=disableavx`，收紧与 SVM 缺省 AVX/XSAVE/BMI 路径的差异；并不据此认定 ADX、FSGSBASE 等其他 CPUID 位及 guest 分派都已对齐。

- 服务位置先用普通文件构造真实错误：FEXServer 退出 255，报告 errno 20 / `Not a directory`，未把非法位置当成有效目录；随后清理了本批负例文件。
- `--version` 输出 `FEX-Emu (a156752-dirty)`。第一个 guest 启动前实际确认 `data/Server` 不存在；launcher 自动创建目录并拉起服务，CoreMark 2000 退出零、CRC `0x4983`。后续使用已有目录，CoreMark 12000 退出零、CRC `0xd340`，bench 退出零、五项 checksum 全部匹配。
- 两份 ELF、配置和两份 guest 的五项哈希在运行前后全部一致。清理使用本批私有环境，结束后 `/proc/net/unix` 中没有本批监听 socket；未操作用户其他 FEX 实例。所有结果均为本批新执行，不复用旧 ELF 的 guest 结果。

短迭代 CoreMark 仍报告运行不足十秒，因此本组只核对固定工作量、退出和 CRC，不发布 CoreMark 跑分。没有采集 PMU，也未把 adb/服务等待时间作为编译或运行性能。

本批源码/patch/六项依赖、配置/构建命令与日志见 `/private/tmp/swiftvm-middle-results/r0-fex-android-rebuild-20261024-v5-reproduction.json`，实际运行见同批 `device-executions.json`、前后身份及各原始日志。v3 的自动启动失败和预启动成功、v4 的新目录失败均保留原始结果，不改写为 v5 成功。固定来源和最小运行链路已推进，正式 `formal_FEX_gate_passed` 仍为 false；继续参照版本适用性、guest 可见能力与实际分派、服务/冷热计数范围及红魔正式多轮三方 PMU，SVM 其余 R1–R7 要求保持开放。

## R3 终结成员查询与只读宽度检查的暂存清理

批次：`r3-terminal-membership-v2`；源码与全部 native/静态参考均为相邻的 `r3-width-contract-index-v2`。四份已有文件修改：ARM64 constraints/width_contracts、公共 compile/CMakeLists 和既有 ir_contract_test；新增公共 `analysis/terminal_uses.{h,cpp}`，无源文件退役，累计源码为 467 项、已退役仍为 113 项。完整单元语义宽度摘要继续开放，本批没有用语义值位事实替代物理 W 写或 X 高半部证明。

### 归属和行为

fixed 声明的单元 Raw 终结集合与宽度复证的本块 Resolved 集合改用 TerminalValueUses。构造时显式选择策略与 Block/Function 范围，Function 仍只遍历 RPO 可执行成员，提升锚点不混入 terminal 集合；有序插入只保留不同定义的指针，四个内联槽覆盖小集合，成员查询 const 且不构图/分配。使用 std::less 的指针全序，定义身份不由编号替代。两处旧 unordered_set 及宽度文件中未使用的终结 helper 已删除，constraints 的另一份 alias 访问集合仍有独立职责并保留。同一请求的两种策略仍分别构造短期结果，不宣称已建立单份全单元共享缓存；较大集合的插入搬迁、实际容量及跨 attempt 复用仍须收口。

原可选 CheckWidthContracts 也创建 WidthCertificates，验证成功时依次填充 width/low32/reads 三份 vector，返回后立即丢弃。现在相同 Verify 只在必要 Prepare 请求输出时填充正式证书，可选 Check 直接返回 bool；必要失败断言、可选失败拒绝、检查顺序和证书内 ID 顺序保持。owner/点位两份必要物理索引继续保留，没有将整个 Check 说成无分配，也没有删除基础/final fixed 或 FPR retry。

### 集中回归与中间记录

第一版生产文件编译通过，但新增夹具对 const Value 调用可变 SetCastType 接口，完整构建退出一；更正为局部 Value 副本后形成第二版。第一版源码和构建日志保留，没有从失败构建冻结或上机运行产物。第二版十四个 Android/ARM64 目标完整构建退出零后冻结，相邻十四个 ELF 均有变化。

NX729J / `bf1359ff` 新执行十二项 native、十组静态负载；五个实际运行程序与两份 guest 的七项设备哈希在前后均匹配，没有复用旧执行。主套件仍为 743 用例，11 个失败用例/66 个失败断言/零 error 的逐项签名及顺序相同。总断言为 226769，相邻为 226911，差值 −142 如实保留；既有 RunColdChild 的 waitpid 轮询每次未结束时执行 REQUIRE，成功断言总量会随调度变化。JUnit 未记录逐用例断言数，本批没有把全部 −142 精确归因于该轮询。

新增两组用例位于 swift_ir_test：嵌套 If/Switch/Condition/CheckHalt、重复别名、跨内联容量集合、相同编号的外块定义、锚点独立性、Function RPO/不可达成员及独立 Block 范围。快慢 IR 各为 80 用例、29832/29916 断言，均新增 233 个成功断言；其余公共用例签名和顺序相同。主套件不包含 ir_contract_test，初始执行计划的新增用例归属说明已据 CMake 更正并保留更正理由；执行命令和原始结果没有改变。fixed-class 2 用例/39 断言通过；八个持久化/寿命行为标记逐项相同，target_spill_misses 合计六次，寿命 checks 123494/123477、injected_failures=0。

### 生成代码与存储边界

十组 Function/bound-decode off/独立 Block/fixed-class Function/Block 的 CRC 或五项 checksum 正确，spill 指标相同，target_overflow=0。六组完整有序记录相同；另外四组只增加/减少既有 formatter 符号范围内的记录，范围外有序记录相同，公共 PC 的记录大小没有变化：

| 负载 | 参考→本批记录数 | 参考→本批 RA host bytes |
| --- | --- | --- |
| Function CoreMark | 426→427 | 286624→286716 |
| Block CoreMark | 2416→2417 | 204988→205220 |
| fixed-class Function CoreMark | 427→426 | 286388→286296 |
| fixed-class Block CoreMark | 2415→2419 | 206336→206716 |

四份 Block 原始 dump 的哈希已重新核对；两种 bench 全字节相同，两种 CoreMark 的公共 PC 仅七个 CPUID 目标存在四条 MOV 的立即数字段差异，所有其他公共字节相同。沿用同一参考日志的类型锚点、未改 frontend 与两份真实 ELF 符号核对；load bias 仍为推断，新增/缺失记录的 formatter 边界另由符号表证明，不据此推断 Function 机器码全等。

十组静态加 IR-slow 的九类既有工作区都闭合，六项基础分配/释放计数全部相同。四组上述 CoreMark 的 retention_events/pattern_init_bytes 随 formatter 路径变化，其他样本全部字段相同。该工作区统计不覆盖 TerminalValueUses、owner/point 私有堆，不作为完整成本通过的证据。

### 静态成本及接续

DWARF 的 TerminalValueUses 为 56 bytes，WidthContractCheck 从 144 增至 160 bytes，Owner 仍为 24、候选查询描述仍为 12 bytes。launcher .text 为 4457100→4456708（−392 bytes）；有序插入的搬迁及新构造调用成本继续计量，不能只按移除哈希推断所有输入的总收益。

| 自身 outlined 函数 | 参考→本批代码 bytes | 参考→本批固定栈帧 bytes |
| --- | --- | --- |
| PrepareWidthCertificates | 512→488 | 352→288 |
| CheckWidthContracts | 388→292 | 320→240 |
| Verify | 8416→7952 | 416→320 |
| Width 检查器构造/析构 | 2196→2200 / 136→116 | 304→288 / 32→32 |
| fixed Block/Function 输入构造 | 892→844 / 2484→2400 | 1824→1840 / 448→448 |
| Terminal Block/Function 构造 | 新增 304 / 376 | 新增 128 / 176 |
| Terminal Contains | 新增 80 | 0 |

表中只记各函数自身固定 SP 减量，包含 shifted immediate，没有把调用链、递归 visitor 或动态栈峰值当作自身帧。本批没有采集 PMU，不将这些静态差值和单次 profiling 时间当作总体性能收益。

源码摘要 `8db8a37f812284e37401ec342e078488e89e88a50491a772e460c5332db2c7d0`，源码 overlay SHA-256 为 `d19f08f36b02cd657868404339cdb613a369fa0444d05e46251a473797f2261c`；十六成员 native 包为 `7fa7e10b9c390917831215dda35b030273000f7341ed45e698769596a63b60f6`，二十六成员完整结果包为 `390af75dc3977ee6292705ed8148a59149802973d7c89490205e920861900c21`。批次材料位于 `/private/tmp/swiftvm-middle-results/r3-terminal-membership-v2-*`，二十二项范围内检查通过。完整宽度摘要、其余 fixed/spill/helper 接缝、候选扫描与私有成本、当前共享模块的 RV64 后续执行及完整 R7 矩阵、固定 FEX/红魔正式三方 PMU 仍开放。

## R0 红魔采样入口与原始基线资格

本批只新增 [tools/redmagic/pmu-point.sh](../tools/redmagic/pmu-point.sh)、[compare.py](../tools/redmagic/compare.py) 和 [README](../tools/redmagic/README.md)，落实方案 14.6 的长期采样与比较入口，没有给 SVM/FEX 插入 probe。SVM 生产源码及十四份构建产物未改，不重复构建或重跑 native 套件；最新生产回归仍为 r3-terminal-membership-v2。原始基线仍是提交 2804a51b 的 launcher a38c2db8，候选仍为 ff7fa90d，固定 FEXLoader/FEXServer 仍为 8632943c/9fe58a99；完整哈希沿用各自产物记录。

### 工作量资格与计数协议

固定 guest 的 CPU 能力初始化会读取 CPUID，memset_resolver 与 __memset_chk_resolver 根据 __cpu_model 的 AVX2 位选实现。按真实符号长度核对后，SVM 已有独立 Block 记录与本批固定 FEX 的单块 JIT 命名均含同一组十四个 memset_generic 编译入口，在准确的 memset_avx2 范围未见入口。FEX 诊断启用其已有 JIT 命名、关闭 multiblock；该配置只用于分派诊断，正式计时不沿用。映射名称是文件坐标，使用可执行 PT_LOAD 的 vaddr-offset 转为 guest PC，并用 ELF 入口及全部映射范围验证；不同 LOAD 的基址并不相同。没有把相邻符号 SetDefaultHeapTaggingLevel 算成 AVX2 memset。

这些是编译条目记录，不能代替完整执行 trace。两侧 CPU 初始化条目仍有差别；FEX disableavx 并未对齐 vendor/model、ADX、FSGSBASE、RNG 等所有能力，因此 complete_guest_capabilities_equal 仍为 false。Scudo CRC 检测符号范围内在这两份诊断中均未见命名入口，不据此宣称其在整个运行中不执行。诊断、精确符号/LOAD 范围和映射转换保存于 r0-fixed-fex-workload-20261024-v1-*，FEX 只删除了本批 PID 19612 对应且哈希匹配的 perf map，私有服务已结束。

原始格式验证确认 simpleperf 1.build.13464643 的普通 CSV 汇总没有 enabled/running；新入口使用 --verbose 原始行，要求两种事件同组、CPU 4、running=enabled>0、原始/汇总一致。先将 profiler taskset 10，再创建目标，避免 --cpu 4 过滤器遗漏目标绑核之前的工作。每点保存命令/环境、身份、实际 PID/affinity、前后频率/三个命名热区/电池/USB 条件。父 shell 等待实际 launcher，分别保存 profiler-exit-code 和 target-exit-code；同样的两层 shell/查询开销计入三条臂。没有改变设备性能模式、频率、充电或散热设置。

### 中止批次保留

r0-redmagic-pmu-20261024-v1 预定三十六点：Block/Function × 三条臂 × 三轮 × 2000/12000，轮转臂/模式/端点顺序。八项部署输入的前后哈希一致，固定 FEXServer 以私有环境提前启动并单独采集寿命聚合成本。前八点退出与 CRC 正确；第九点是原始基线的 Block 2000，stderr 为 std::logic_error / invalid final low32 copy at IR 3，目标因 Aborted 终止，没有 CoreMark 输出。此时 simpleperf 返回零；最初入口用 CRC 缺失拒绝该观察并停止后续采样。

原始九点及服务原始 CSV 已取回，比较器同时报告 CRC 缺失与矩阵未完成，measurement_valid=false、summary 为空，没有由单轮 Function 或部分 Block 记录生成性能比值。服务聚合为 instructions 77747305、cycles 59490002、wall 42.178781 秒，原始 running/enabled 均为 1；范围包含启动、这批已执行请求、空闲和关闭，不能解释成逐点服务成本或纯启动时间。只通过本批私有环境关闭服务，监听 socket 清除，transport 退出零。完整材料在 /private/tmp/swiftvm-middle-results/r0-redmagic-pmu-20261024-v1/，含 manifest、冻结脚本、execution.py、原始点和 comparison.json。

### 目标退出修正与有界验证

针对上述真实遗漏，设备入口不再把 profiler 状态当成目标状态，主机同时验证计数父 PID、实际等待的子 PID、launcher 自报 PID/affinity 与两个退出状态。新脚本/比较器的 literal wrapper 也作离线一致性检查。r0-redmagic-pmu-tools-20261024-v2-contract 的十七项离线检查通过：完整合成三十六点可解析；错误 CRC/迭代、目标/计数 PID、affinity、原始计数/有效时间/继承、身份/充电变化、缺点/重复点，以及 profiler/target 独立非零均被拒绝。合成数值只验证工具契约，不作为设备性能证据，未向仓库新增单次测试脚本。

红魔仅追加一组必要的三点验证，使用新私有目录 r0-redmagic-pmu-20261024-v2-qualification：

| 观察 | profiler / target / 设备入口退出 | 结果 |
| --- | --- | --- |
| 原始基线 Block 2000 | 0 / 134 / 134 | 相同 low32 必要复证中止；比较器明确因 target 非零拒绝 |
| 当前候选 Block 2000 | 0 / 0 / 0 | CRC 0x4983，PID/affinity、同组原始计数与身份检查通过 |
| 固定 FEX 冷服务 2000 | 0 / 0 / 0 | 全新 data/socket/tmp，无预启动，CRC 0x4983；相同协议通过 |

九项旧/新部署输入前后哈希相同，FEX 只关闭自己的私有服务，结束后无本批监听 socket。该三点不计算性能比值，也不混入此前单层 shell 的观察；真实命令、两个退出码、CSV 和验证结果归同批 qualification.json。原始基线的独立 Block 资格继续定位，后续相邻可运行版本的对照必须保留实际来源，不能覆盖或追认此处原始失败。完整宽度/私有成本、其余阶段与正式多轮三方门继续开放，formal_FEX_gate_passed 和 full_refactor_complete 均为 false。

## R1/R5 flags 事实的直接存储与需求借用

批次 r1-flags-demand-storage-v2，相邻参照为 r3-terminal-membership-v2。修改 facts.cpp、lower_integer_flags.cpp/h 和既有两份测试；没有新增/退役源文件，累计仍为 467 项源码、113 项退役文件。没有加入 SVM/FEX probe、新配置或单次测试脚本。

### 删除对象与边界

基础扫描现在直接填充 SemanticFacts 的单元 flags 向量和 BlockRecord 的局部 packed pseudo demand。原 BlockFlags 临时记录集合、每块临时 PseudoFlagsPlan、末尾逐块再次查找/移动，以及单元 demands 临时向量已删除。局部 backing 仍在 PrepareIndex 时一次移动到稳定索引，prepared 动作继续拥有独立副本；生命周期没有改成借用已销毁的 facts。flags 先就绪再补活跃性不会清空已经移交的结果，未完成收集的重入会重置部分计数与 home。可达跨块 pseudo 仍进入单元需求，本块 producer 才进入局部需求，不可达块不污染这两种结果。

窄整数 flags 准备不再把 packed demand 逐 IR 展开成 std::map。IntegerFlagsInputs 借用源索引、PseudoFlagsView 和选中分支覆盖；查询首先检查实际 producer 归属，原始类型、稀疏编号、外块同 ID 和 branch-only 语义保持。StaticFlagDemand 类型及只被测试设置、生产始终为空的 excluded_comparisons 集合已退役，原对应负例改为真实 BranchOnlyFlags 需求。候选资格与既有接收边访问合并，首次命中后停止资格判定，匹配规则与访问顺序保持；carry/compare 的私有候选、dead-input/memo/集合仍有独立职责，未一并宣称清空。

### 构建与集中回归

第一版完整构建退出一：intrusive 指令列表不满足 ranges::any_of 的约束，新 narrow 夹具也缺少 scalar 类型限定。第二版仅改公共候选查询的遍历适配和夹具类型，十四个 Android 目标完整构建退出零后才冻结；十四份 ELF 均与相邻版本不同。失败源码和构建日志保留，没有冻结或执行失败构建中的产物。

红魔 bf1359ff 新执行十二项 native 与十组静态，五个程序/两份 guest 的七项前后哈希一致。主套件 744 用例、226837 断言，对照 743/226769；新增窄 flags 借用用例通过，共同用例顺序与完整失败签名相同，仍为十一例/六十六个失败断言、零 error。总成功断言增加 68 不全部归因于新增用例；既有 RunColdChild 轮询会随调度改变成功断言总量，JUnit 不提供逐用例断言计数。

IR 快慢各 81 用例、29981/30065 断言，分别比参考增加 149，新增共用 flags 用例通过。覆盖八个有 flags 的线性 Function 块、跨块需求、不可达成员、先 flags/先活跃性、索引移交后稳定性和可变新输入重置；另一新增主套件用例覆盖稠密/稀疏编号、同编号外块、空需求和分支覆盖。fixed-class 2/39 通过；八项持久化/寿命顶层标记与参考逐项相同，真实 target_spill_misses 合计六次。

十组 CoreMark/bench 的 CRC/五项 checksum、实际模式、spill 指标和零 target_overflow 保持。七组完整有序形态相同；三组仅在已有 formatter 符号范围变化，公共 PC 的 IR/host 大小与范围外有序记录相同：

| 负载 | 记录数，参考→本批 | RA host bytes，参考→本批 |
| --- | --- | --- |
| Block CoreMark | 2417→2414 | 205220→204988 |
| fixed-class Function CoreMark | 426→427 | 286296→286388 |
| fixed-class Block CoreMark | 2419→2413 | 206716→206028 |

四份 Block dump 的原始哈希重新验证；两份 bench 全字节相同，两份 CoreMark 公共 PC 仅七个 CPUID 目标的四条 MOV 立即数变化，其他公共字节相同。使用同一参考日志的先前具名 helper 证明、真实 ELF 符号和未改变的 frontend；load bias 仍为推断，不扩为 Function 字节等价。十组静态加 IR-slow 的九类 workspace 全部闭合，六项基础分配计数逐组相同，只有上述三组 formatter 的 retention_events/pattern_init_bytes 不同；这些计数不覆盖本批退役的私有集合及剩余私有候选成本。

### 静态成本

实际 NDK libc++ DWARF：IntegerFlagsInputs 104→72 bytes；SemanticFacts/BlockRecord/PseudoFlagsPlan 保持 624/120/32 bytes，退役的 StaticFlagDemand/BlockFlags 不再存在。launcher .text 4456708→4455644，减少 1064 bytes。

| 自身 outlined 函数 | 代码 bytes，参考→本批 | 自身固定 SP 减量 bytes，参考→本批 |
| --- | --- | --- |
| PrepareBaseFacts | 2568→2224 | 640→400 |
| 块访问 lambda | 4184→3600 | 384→336 |
| LowerIntegerFlags | 2752→2156 | 400→368 |
| LowerNarrowIntegerFlags | 7100→7700 | 688→688 |
| HasNarrowCandidate | 新增 420 | 新增 112 |

只记各自 SP 调整，包含 shifted immediate；不将固定帧求和当作传递栈峰值。新成员身份查询的内联代码和资格 matcher outlining 成本均保留，不能只以入口变小推断全部动态收益。

### 相邻三方 PMU

r1-flags-demand-storage-v2-pmu 使用基线 ff7fa90d（相邻 r3-terminal-membership-v2）、候选 60cc95f3 和同一固定 FEX 8632943c/9fe58a99。比较范围是本批五份源文件，不追认为原始 2804a51b 的累计对照，其 Block low32 失败没有覆盖或撤销。NX729J、root、CPU 4、无 user/kernel 过滤、默认继承、同组 cycles/instructions、两层退出 wrapper；三十六个主点及六个独立冷服务辅助点均完成，CRC 正确、目标/profiler 零退出，原始 running=enabled>0、PID/affinity、八项部署身份与完整矩阵均通过。实际调用仓库主机入口生成 comparison-final.json，measurement_valid=true。

每轮两个端点配对，以下为候选/相邻参考比值变化的中位数，括号保留三轮最小/最大；它不是两个独立中位数的比值：

| 指标 | Function | 独立 Block |
| --- | --- | --- |
| instructions 斜率 | +0.0030%（−0.0269～+0.0204%） | +0.0299%（+0.0089～+0.3625%） |
| instructions 截距 | +0.0265%（−0.0348～+0.1037%） | −0.2380%（−2.6392～+0.0156%） |
| cycles 斜率 | −0.0818%（−1.4338～+3.6019%） | +0.3399%（−0.9718～+1.2444%） |
| cycles 截距 | +0.3176%（−2.1317～+1.9547%） | −4.1129%（−4.2670～+0.4019%） |
| wall 斜率 | +0.0954%（−1.2376～+3.5526%） | +0.2851%（−1.0616～+1.1223%） |
| wall 截距 | +0.2100%（−2.0548～+1.8773%） | −3.7640%（−3.9945～+0.6296%） |

六组成对 SVM 指令斜率均在 1% 门内，数值接近不变；cycles/wall 波动与 Block 截距离散保留，不宣称稳定加速。governor 为 walt、上下限 614400/1171200 kHz，当前读数在 614400～1171200 之间；三个命名热区范围为 31.3～34.8、31.3～34.4、30.9～32.4°C，电池 Charging/79%、USB online。亲和性不等于锁频，三轮范围不是置信区间。

限定外部参照下，候选/FEX 的 instructions 斜率中位比值为 Function 1.6904、Block 2.1529，cycles 为 1.9475/2.5195，wall 为 1.9493/2.5154；instructions 截距比为 11.5965/7.4858。这些截距包含 loader、dispatcher、运行时和编译，不能称纯编译差距。FEX 在两个队列中使用相同配置，不把它等同 SVM Function RA；完整 CPUID 能力仍不相等，其他代表性负载与正式 FEX 门继续开放。

预启动私有 FEXServer 不在主点的进程树内，单列聚合为 instructions 78417966、cycles 62213959、wall 192.111705 秒，包含启动、全部主点请求、空闲和关闭，未按点均摊。六个冷服务点使用全新 data/socket/tmp、无预启动，默认继承覆盖 launcher 活跃时的自动服务启动，结束后的服务尾部与管理关闭在该范围外。各点原始计数完整保留且不混入主三方比值；私有服务均正常关闭，无残留监听 socket。短迭代不发布为官方 CoreMark 分数。

源码摘要 f174bc9449f0cb2afe4073f14bbb540653a6c80e00f4c71e00d82eb9574653ef，overlay 为 684aa3f17917f128a2e5ec8b3d0079a8d877ee2dbe130e0450b119931e96b632。构建/源码/测试/shape/机器码/存储/静态成本材料在 /private/tmp/swiftvm-middle-results/r1-flags-demand-storage-v2-*，PMU 的冻结工具、完整 manifest/命令、raw、逐点设备条件和主机入口结果在同名 -pmu/。完整私有容量/初始化/搬迁、其余候选/CFG/类型与 guest-state effects、当前 RV64 及完整故障/并发矩阵继续接续；full_refactor_complete、full_cost_gate_passed、formal_FEX_gate_passed 均为 false。

## R5 窄整数 flags 的直接动作与短期证明工作区

批次 r5-narrow-flags-stream-v1，相邻参照为 r1-flags-demand-storage-v2。修改 lower.cpp、lower_integer_flags.cpp/h 及两份既有测试；新增/退役源文件均为零，累计仍为 467 项源码与 113 项退役。目标公开入口显式增加 CompileWorkspace 参数，全部生产和测试消费者已迁移，没有保留旧重载、新环境开关或 probe。

### 已退役的准备状态

carry/compare 在既有的先 carry、后 compare 顺序中直接形成最终 IntegerFlagsAction。NarrowCandidates、CarryCandidate、CompareCandidate 三种中转类型、两棵候选 map、一份 covered set、逐 carry 的 unordered memo/std::function，以及 zero_chain/dead_inputs 两份指针向量已删除。匹配器不再先存值/指针候选，再用另一轮遍历翻译成编号动作。compare 对已选 carry 的查询只需要“是否有 test ID 大于当前 producer”，而每个已选 carry 已通过不变的 PSTATE 检查，故以最大已选 test ID 表达同一条件；拒绝的 carry 不进入这个摘要。

短期覆盖与 dead-use 状态共享显式借用的 InstructionRowScope，键是块内 roster 位置，不是全 Function ID。每行 u32 包含覆盖位、当前证明的 visiting/accepted/rejected、待提交根和本次匹配内部的查询标记；这不是持久 IR/分配 epoch。先前选择会改变 covered，因此每个 carry 的证明上下文独立，不能复用此前 rejected 结果。整条 zero chain 的根都证明完后才提交覆盖，保持旧候选的可见顺序。C++ 成员递归仍存在，visiting 状态保守拒绝 use 环；没有把这次容器清理说成消除了深度成本。

纯 compare 路径不借用行表。实际 carry 在首个写入时初始化本块 N×4 bytes，容量遵守既有 workspace 复用/释放规则；可能增加初始化或扩容，不能说整个选择无分配。外块定义、全局仅使用一次的立即数仍进入最终 unit-ID 覆盖，不强行塞入本块位置表。覆盖最终排序去重，结果不持有行表、pstate 或候选指针；准备结束后工作区可继续用于其他家族。

### 一次构建与集中回归

十四个 Android/ARM64 目标首次完整构建退出零后冻结，十四份 ELF 均与相邻版本不同。红魔 bf1359ff 新执行十二项 native、十组静态；实际五个程序和两份 guest 的七项部署哈希在前后相同。主套件 746 用例、229333 断言，对照 744/226837；新增两组用例通过，共同用例顺序和完整失败签名相同，仍有十一例/六十六个失败断言、零 error。总成功断言增加 2496，新增两个用例的源码断言计数合计 2446；未将剩余差值精确归因于任何单个用例或轮询。

新增主套件覆盖两次/三百次连续选择、共享零值的跨候选 rejected→accepted、dead-use 环、从 50000 开始的真实编号与局部位置分离，以及前/中/后三个 compare 对两条 carry 的依赖。第二条 carry 因完整宽度发表被拒绝时，中间 compare 也必须拒绝，不能用未经选择的 test 放宽证明。

IR 快慢各 82 用例、29994/30087 断言，分别比参考增加 13/22，新增工作区用例通过。它使用真实跨 HIR 块的单使用立即数验证覆盖；纯 compare 在工作区行已被占用时仍成功，carry 的冲突借用必须抛出且不改变已有行。覆盖门切换后的三次结果保持独立，旧结果仍可查询，异常后再次借用成功。慢 IR 实际启用 SVM_PROF2，新增九个计量断言验证零 compare 行请求、carry 容量复用、N×4 初始化和最终释放。fixed-class 仍为 2 用例/39 断言，八项持久化/寿命标记逐项相同，真实 target_spill_misses 六次。

### 生成代码和可见存储

十组 CRC/五项 checksum、实际 Block/Function 模式、spill 指标和零 target_overflow 保持。五组 bench 的全部有序记录相同；五组 CoreMark 的差异均限已有 formatter 符号范围，公共 PC 的 IR/host 大小和范围外顺序相同：

| CoreMark 配置 | 记录数，参考→本批 | RA host bytes，参考→本批 |
| --- | --- | --- |
| Function | 427→426 | 286716→286624 |
| bound-decode off | 427→426 | 286724→286632 |
| 独立 Block | 2414→2418 | 204988→205288 |
| fixed-class Function | 427→427 | 286388→286388 |
| fixed-class Block | 2413→2418 | 206028→206824 |

四份 Block 原始 dump 的哈希重新计算：两份 bench 全字节相同，两份 CoreMark 的公共 PC 仅七个 CPUID 目标的四条 MOV 立即数变化，其他公共字节相同。目标由相同参考日志的具名 helper 证明、真实 ELF 符号和未改 frontend 支持；load bias 仍为推断，不扩为 Function 机器码全等。

十组静态的九类 workspace 都闭合，六项基础分配字段逐组相同；四组 CoreMark 的 retention_events/pattern_init_bytes 随 formatter 工作集变化，其他样本字段相同。新增 IR-slow 用例的 instruction_rows 与 total 都增加一对分配/释放、36 bytes、四次 retention、144 bytes pattern fill；其余缓冲、live/peak/retained 和 zero-init 相同。这与九指令夹具的四次显式行初始化一致，新增可见成本保留，没有用它推定旧未计量哈希的净堆收益。

### 静态成本和后续范围

实际 NDK DWARF：IntegerFlagsInputs 保持 72 bytes，IntegerFlagsBuilder 保持 152，SemanticFacts/BlockRecord/PseudoFlagsPlan 保持 624/120/32。旧 88-byte NarrowCandidates、56-byte CarryCandidate、40-byte CompareCandidate 均不再存在；短期 CompareSelection 为 16 bytes，含最终动作和仅用于本轮覆盖的立即数定义。launcher .text 为 4455644→4452916，减少 2728 bytes。

| 自身 outlined 函数 | 代码 bytes，参考→本批 | 自身固定 SP 减量 bytes，参考→本批 |
| --- | --- | --- |
| LowerIntegerFlags | 2156→2120 | 368→368 |
| LowerNarrowIntegerFlags | 7700→5308 | 688→448 |
| ProveDeadUses | 新增 1904 | 新增 160 |
| Cover / WriteRow | 新增 712 / 88 | 新增 160 / 32 |
| compare 收尾 lambda | 新增 764 | 新增 128 |
| builder 析构 | 128→140 | 32→32 |

这些只覆盖各函数自身 SP 调整；旧模板/回调与新成员递归的成本会随 outlining 移动，不把入口减量当作完整传递栈收益。首次行初始化、实际容量、PSTATE 的更晚触发、深层死用图及完整动态编译成本继续验收。本批没有再采集 PMU 或重新执行 RV64，不把相邻 r1-flags-demand-storage-v2 的三方结果追认为本 ELF 的新执行。

源码摘要 4a01b51439c74c5639cf5a4f56ec826a81fba45e53d832a558940e15d18a6d71，overlay 为 fc7de62069c897f3af457bfc80108fcb59d789df6a5423c90b7ec576ff3467f9，launcher 为 34024ac96b78782ed4aff24430163f7d09279871b8726be7937a2c3363facece；源码/构建/执行/代码/存储/静态成本归 /private/tmp/swiftvm-middle-results/r5-narrow-flags-stream-v1-*。完整宽度、其余目标暂存/共享扫描、原始基线 Block 资格、FEX 完整能力与其他代表负载、当前公共模块 RV64 和完整故障/并发/关闭矩阵继续开放；不将本家族的容器退役当作整个 R5 或 R0–R7 完成。

本批三十二项范围核验已通过，覆盖冻结源码/ELF、原始归档、用例与失败签名、静态代码和 workspace 成本。核验只读取既有结果，未追加设备执行；记录归 r5-narrow-flags-stream-v1-quality-gates.json，源码相邻差异另存 source-comparison.patch。它不改变上文保留的失败与未验收范围。

## R5 驻留结果直接持有最终紧凑动作

批次 r5-residence-actions-v1，相邻参照为 r5-narrow-flags-stream-v1。修改十五个既有源文件，包括准备入口、copy/select 提交、memory 消费、测试支持和既有 transferred-select 用例；新增/退役源文件均为零，累计仍为 467 项源码与 113 项退役。没有新增 probe、环境开关或测试脚本。

### 退役与交接

旧 GPRResidenceFacts 的 zero_extends、sign_extends、resident_values 三份指针哈希集合和 result_homes map 已删除。GPRResidences 只拥有一个块身份与连续的最终 GPRResidenceAction，按编号排序，重复标记直接合并，同一结果 home 保持首次提交语义。动作仍是四字节；查询不分配、不排序，必须先核对节点的真实所属块。外块定义不能生成本块的动作，相同编号的独立块也不能共享授权。

ResidencePreparation 的 Facts 入口已退役，所有生产与测试消费者显式读取 Residences。FinishConstraints 移交唯一、可移动且不可复制的结果；PrepareMemory 在移交前只读查询 sign-extension 标记。StoreGPRResidences 改为消费拥有结果的参数，验证块身份和编号/home 范围，第一份动作缓冲直接转入 UnitActions，后续块追加紧凑记录。原先按四个容器再投影动作的遍历已删除，必要的 target-write witness 和既有物理提交顺序保持独立；UnitActions 的整单元排序/一致性检查继续保留。

该结构没有增加全块稠密表、公共 workspace 种类或持久 UnitActions 字段。稀疏 vector 的增长和有序插入仍可能分配或搬迁，不能把删掉哈希容器视为零分配，完整私有容量与动态成本继续验收。

### 一次构建与集中验证

十四个 Android/ARM64 目标首次完整构建退出零后冻结，十四份 ELF 均与相邻版本不同。红魔 bf1359ff 新执行十二项 native、十组静态；实际五个程序和两份 guest 的七项部署哈希在前后相同。主套件仍为 746 用例，229520 断言；相邻版本为 229333，仍有十一例/六十六个失败断言、零 error，共同用例顺序和完整失败签名相同。

既有“Select uses a transferred pinned value without reloading its spill”实际执行用例增加六个净断言：构造编号相同的另一块，验证本块准备结果拒绝外块查询和记录，随后核对真正准备并封口的 UnitActions 保留原 extension、拒绝外块节点，最后继续执行原来的机器码检查。没有新增用例名或测试源文件。总成功断言增加 187，除明确的六项外，剩余 181 不精确归因于任何用例或轮询；JUnit 不提供逐用例断言总数，既有 RunColdChild 的轮询断言数依赖调度。

IR 快慢配置仍各 82 用例、29994/30087 断言，fixed-class 仍为 2 用例/39 断言，均零失败。八项持久化/寿命标记逐项相同，真实 target_spill_misses 六次。十组 CRC/五项 checksum、实际 Block/Function 模式、spill 指标和零 target_overflow 保持；五组 bench 的完整有序记录和完整 shape 相同。五组 CoreMark 的公共 PC 大小及 formatter 范围外顺序相同，变化限已有格式化符号范围：

| CoreMark 配置 | 记录数，参考→本批 | RA host bytes，参考→本批 |
| --- | --- | --- |
| Function | 426→426 | 286624→286624 |
| bound-decode off | 426→427 | 286632→286724 |
| 独立 Block | 2418→2415 | 205288→205000 |
| fixed-class Function | 427→427 | 286388→286388 |
| fixed-class Block | 2418→2418 | 206824→206624 |

四份 Block 原始 dump 的哈希重新计算：两份 bench 全字节相同，两份 CoreMark 的公共 PC 仅七个已证明 CPUID 目标的四条 MOV 立即数组发生变化，其他公共字节相同。仍以相同参考日志的具名 helper 证明、真实 ELF 符号和未改 frontend 定位目标；load bias 仍为推断，不据此声明 Function 机器码全等。

十一份 workspace 样本的九类缓冲全部闭合，六项基础分配字段逐样本相同，IR-slow 所有字段相同。三组 CoreMark 的 retention/pattern fill 随 formatter 工作集变化，其他样本字段相同。这些计数未覆盖旧私有哈希或新私有 vector，不能作为它们净堆收益的证明。

### 实际布局、代码与未完成成本

| 实际 NDK DWARF 类型 | bytes，参考→本批 |
| --- | --- |
| GPRResidenceFacts / GPRResidences | 144→32，旧类型已退役 |
| GPRBlockPreparation | 176→64 |
| ResidencePreparation | 448→336 |
| PendingGPRBlock | 296→184 |
| UnitActions / SemanticFacts / CompileWorkspace | 864 / 624 / 224，均不变 |
| GPRResidenceAction / PublicationSelection | 4 / 24，均不变 |

launcher .text 为 4452916→4453324，增加 408 bytes。ResidencePreparation::Prepare 的自身代码 1924→1228 bytes、自身固定 SP 减量 288→176；FinishConstraints 为 1292→1308、208→208。StoreGPRResidences 为 540→576、112→112，旧 516-byte/160-byte 投影 lambda 退役；新增 Record、RecordResultHome、Find 的代码分别为 240/396/128 bytes，自身 SP 减量为 48/96/0。Prepare 辅助逻辑可能被内联，以上不是完整调用链或动态栈峰值；对象缩小也不替代 vector 容量、扩容重叠、插入搬迁和完整编译成本验收。

本批三十二项范围核验通过，源码、十四份冻结/构建 ELF、原始归档、失败签名、静态结果与成本记录已核对。本批未再采集 PMU 或重新执行 RV64，不把之前其他 ELF 的结果追认为新执行。源码摘要 c2a9db6bd757ef2d63d3100c42c3a7ee69f8297c2668331b1e7583e51ef8ed06，overlay 为 61cd2b6636fad9fd9adc28c91f78e6c8a6d6591dbf021519a3d5cc6c255b18d1，launcher 为 912713b93e488e8e210842c732c71d7812011a3f808e8c5677693611815afd77；结果归 /private/tmp/swiftvm-middle-results/r5-residence-actions-v1-*。其余 pinned 候选/窗口、共享扫描、完整宽度和 guest-state/helper 契约、全成本、原始基线 Block/FEX 资格及完整故障/并发矩阵继续开放，整个 R0–R7 目标保持进行中。

## R5 pinned 候选借用使用图与 transfer 单队列

批次 r5-pinned-use-views-v2，相邻参照为 r5-residence-actions-v1。修改十个既有源文件；新增/退役源文件均为零，累计仍为 467 项源码与 113 项退役。没有新增 probe、环境开关或测试脚本。

### 数据与作用域

select 和 publication-view 的两份 aliases vector 已退役，候选直接借用封口后 BlockAnalysisIndex 的直接使用列表，显式跳过 publication。全局 use 计数、alias 子消费者、原始类型与存活窗口的原检查顺序保持。物理 recolor 不改写 IR；它补充的 resolved 使用表是独立存储，不会使已借用的直接使用 span 失效。候选只在同步匹配/提交期间持有视图，不进入 PendingGPRBlock 或 UnitActions。SelectCandidate 的 publication 副本、PublicationViewCandidate 的 value/publication/last-use 副本也已删除；必要 component 列表仍在，不能据此宣称全部候选暂存已退役。

transfer 的独立 StackVector worklist 和返回 aliases vector 合为一份短期队列。read root 隐式处理，后续 alias 以四字节 LoweringCandidate 保存块 roster 位置，按原广度顺序证明；每轮重新取得视图，避免扩容后继续访问旧 span。该队列同时作为提交时的只读视图，由显式 FixedGPRTransferUses 作用域持有；没有 alias 的直接 transfer 不借用。返回视图在作用域重置/结束前消费，跨候选不保留证明。预分配与目标准备两个调用方均已迁移；Block/Function 的初始 fixed 输入各自显式传入外层 workspace，没有兼容重载。发表前 source、发表后 target、全局/局部使用数、原始边宽度及 helper/overwrite 生存规则保持。

MEM 来源发表直接返回最终 PublicationSelection，SpilledPublicationCandidate 类型退役；仍按原 candidate→recolor→home→selection 顺序执行必要证明。非 SetHostGPR 节点在建立 transfer use scope 前跳过；其余家族的完整播种/扫描收敛继续接续。

### 中间版本与最终执行

v1 首次十四目标构建和十二项 native/十组静态均通过范围内核对；实际布局却暴露新的 use scope 继承了带虚析构的 DeleteCopyAndMove，大小为 32 bytes。现已去掉该 scope 以及两个私有 final 准备类的标记继承，显式保留禁止复制/移动，不需要多态析构。三个类的最终 ELF vtable 均不存在；未改全局标记基类或公共 AllocationWriter。v1 源码、ELF、原始执行及布局记录保留在 intermediate-record，不能作为最终 ELF 的执行证据。

v2 十四个 Android/ARM64 目标完整构建退出零后重新冻结，十四份 ELF 均与相邻参照不同。最终红魔 bf1359ff 的十二项 native、十组静态全部新执行；实际五个程序和两份 guest 的七项部署哈希在前后相同，没有复用 v1 执行。

主套件仍为 746 用例、229255 断言，参照为 229520；完整失败签名和共同用例顺序相同，仍有十一例/六十六个失败断言、零 error。既有 long-alias 实际发射用例增加一项 CHECK，验证 300 层链得到相同机器码。总成功断言差值 −265，新增一项之外的 −266 不精确归因于任何单个用例或轮询；既有 RunColdChild 的轮询次数依赖调度，JUnit 没有逐用例断言总数。

IR 快慢配置均从 82 增至 83 用例，断言为 30031/30129，分别增加 37/42，均零失败。新增用例使用真实 GetHostGPR→publication→alias→ALU 图、从 50000 开始的编号和十二层链：直接 transfer 在工作区已被占用时仍成功；实际 alias 的借用冲突抛出且不改写原占用内容；解除冲突后得到正确的局部位置；目标晚期 clobber 拒绝后重新匹配成功。慢配置启用 SVM_PROF2，额外五个断言核对无直接请求、容量复用及最终释放。fixed-class 仍为 2 用例/39 断言，八项持久化/寿命标记逐项相同，真实 target_spill_misses 六次。

### 代码与工作区

十组 CRC/五项 checksum、实际 Block/Function 模式、spill 指标和零 target_overflow 保持。五组 bench 的全部有序记录与完整 shape 相同；五组 CoreMark 的公共 PC 大小及 formatter 范围外顺序相同，差异限已有格式化符号范围：

| CoreMark 配置 | 记录数，参考→本批 | RA host bytes，参考→本批 |
| --- | --- | --- |
| Function | 426→426 | 286624→286624 |
| bound-decode off | 427→427 | 286724→286724 |
| 独立 Block | 2415→2417 | 205000→205220 |
| fixed-class Function | 427→427 | 286388→286388 |
| fixed-class Block | 2418→2417 | 206624→206556 |

四份 Block 原始 dump 的哈希重新计算：两份 bench 全字节相同，两份 CoreMark 的公共 PC 仅七个已证明 CPUID 目标的四条 MOV 立即数组发生变化，其他公共字节相同。继续借相同参考日志的具名 helper 证明、真实 ELF 符号和未改 frontend 定位；load bias 为推断，不据此声明 Function 机器码全等。

十一份 workspace 样本的九类缓冲全部闭合。十组静态的六项基础分配字段逐组相同，五组 bench 所有字段相同；新 borrow 有真实管理成本，CoreMark 五种配置的总 retention_events 分别增加 139/141/110/163/99。两组 Block 的 pattern_init_bytes 分别增加 412、减少 112；其他静态样本初始化字段保持。不能把全部 retention 差值归为 formatter。

新增 IR-slow 夹具的 lowering_candidates 与 total 均增加五对分配/释放、124 allocated/released bytes、四次 retention；其余缓冲、live/peak/retained、zero/pattern fill 均相同。十二项记录的容量能复用，但旧私有指针列表未纳入这些计量，不能由此推定净堆收益或称整个匹配无分配。

### 实际布局和剩余成本

| 实际 NDK DWARF 类型 | bytes，参考→本批 |
| --- | --- |
| FixedGPRTransfer | 48→32 |
| FixedGPRTransferUses | 新增 24；中间 v1 的 32 不作为最终大小 |
| SelectCandidate | 56→40 |
| PublicationViewCandidate | 48→24 |
| SpilledPublicationCandidate | 24→退役 |
| ResidencePreparation | 336→328 |
| TargetPreparation | 2168→2152 |
| PendingGPRBlock / UnitActions / SemanticFacts / CompileWorkspace | 184 / 864 / 624 / 224，均不变 |
| LoweringCandidate / GPRResidenceAction / PublicationSelection | 4 / 4 / 24，均不变 |

launcher .text 为 4453324→4453172，减少 152 bytes。MatchFixedGPRTransfer 自身代码 2236→2248 bytes、固定 SP 减量 496→352；其 target 调用方 PrepareFixedGPRTransfers 则为 492→696、256→304，MatchTransfer 为 396→416、112→128。MatchPinnedSelectPublication 为 1576→1440、224→240，调用方为 928→904、240→224；publication-view matcher 为 1352→1236、208→208，调用方为 320→276、144→128。spill 提交为 404→388、176→160；新 Append 为 160-byte 代码和 64-byte 自身 SP 减量。成本在 matcher、作用域和调用方之间移动，不能用单个函数减量推导完整传递栈峰值或编译提速。

本批三十四项范围核验通过，源码、十四份冻结/构建 ELF、原始归档、中间版本隔离、失败签名、代码与存储记录已核对；没有再采集 PMU 或重新执行 RV64。源码摘要 544a892268ed1e9a90cb7289c8782ff3e32efb6583a934bab91a225079f85b7e，overlay 为 4314bd1b98a9ba85a4b84c30d7f49c0682ac8723227ee1fda998026c2c9cb312，launcher 为 76f077bd777708bf0c4ecfe2815dd62cbe6583e3ae5e1ccf8c5b44c3e8138900；结果归 /private/tmp/swiftvm-middle-results/r5-pinned-use-views-v2-*。CopyCandidate、recolor 的 component/definitions/IDs/sites 暂存、其余窗口与共享播种、完整宽度和 guest-state/helper 契约、全成本、原始基线 Block/FEX 资格及完整故障/并发矩阵继续开放，整个 R0–R7 目标保持进行中。

## R5 Copy 标量证明、语义 root 与临时写入复用

批次 r5-copy-root-stream-v2，相邻参照为 r5-pinned-use-views-v2。修改七个既有源文件，新增/退役源文件均为零，累计仍为 467 项源码与 113 项退役；没有新增 probe、环境开关或测试脚本。

### 数据与原顺序

CopyCandidate 的 aliases 与 transferred_uses 两份 owning vector 已删除，只保留 read/narrow/extend、source/target、width、signed 和 last-use 的标量证明。匹配仍先检查各来源的全局 use 数、角色及 alias 子使用闭合，之后才对已证明的邻接关系进行投影。VisitCopyAliases 按原 narrow-extension、Add source、outer-extension 顺序访问共享直接使用表，保留每个 occurrence；RegisterCopyUses 按原 narrow、read 顺序登记转移使用。角色只取不可变 opcode 与已证明的 source 条件，不在 recolor 把 MEM 改为 GPR 后重新执行匹配或重判 source 类型。

copy/select/spill 的物理 component 现在只建立一份 u32 语义 root ID 列表。AppendRecolorValue 继续做 BitCast 语义来源、本块归属、标量宽度、anchor 排除与去重，保留原首 owner 和成员顺序；旧 component 与 definitions 两份指针列表已退役。recolor 从唯一 roster 找回定义，公共 AllocationEdit 内的物理 ResolveId 仍独立，不能把这两种归一化混为一层。正常编号查询为稠密读取，手工稀疏编号仍可能线性查找，相关成本保留。

原 terminal 私有递归已接入共享 VisitTerminalValues，判断仍采用原 AllocationId 成员规则；命中后停止额外成员查询，但共享遍历仍会访问剩余 terminal 结构，不宣称消除了全部递归或遍历成本。逐点 clobber sites、scratch/fixed/width 的必要证明和局部事务回滚均保留。

fixed 证明不再复制整份 target_writes：只在该证明作用域临时追加当前 witness，ScopeExit 在成功、拒绝和异常退出时恢复逻辑长度，结束后才继续 width 复证与 Commit。缓冲仍可能首次增容；FinishConstraints 释放纯证明留下的空容量，避免随空 pending 结果保留。非空的真实写入继续按原顺序交接，没有新增公共 workspace 或持久结果字段。

### Function 外块 MEM 归属修正

窗口审查发现原 MatchSpilledGPRPublication 在证明 producer 属于本块前，就对本块列表调用 iterator_to(producer)。Function 合法的外块 MEM 定义可能通过前置类型、单使用和相邻编号窗口检查，随后发生外块链表遍历；旧 recolor 较晚的本块检查不能保护这一访问。现在在取得或遍历迭代器前，用只读 roster 明确拒绝局部 recolor，保留普通 MEM 路径。

v1 的十四目标构建退出零，但尚未冻结 ELF 或上机；发现该边界后合入守卫与真实双块回归，重新构建 v2。中间源码/构建证据保留在 intermediate-record，未以它冒充最终执行，也没有重复设备测试。

### 集中验证

最终十四个 Android/ARM64 目标构建退出零后冻结，十四份 ELF 均与相邻版本不同。红魔 bf1359ff 新执行十二项 native、十组静态；实际五个程序和两份 guest 的七项部署哈希在前后相同。主套件仍为 746 用例、229133 断言，参照为 229255；完整失败签名和共同用例顺序相同，仍有十一例/六十六个失败断言、零 error。

既有 transferred-select 实际发射用例补入 narrow 与 published 两类 alias 的独立 ALU 消费，同时保留直接 Select、源 home 覆写和普通发射检查；新增两个位置断言均为目标 home 23。总成功断言差值 −122，新增两项之外的 −124 不精确归因于任何单个用例或轮询；既有 RunColdChild 轮询数依赖调度，JUnit 没有逐用例断言总数。

IR 快慢配置均从 83 增至 84 用例，断言为 30041/30139，分别增加十项，均零失败。新增真实双 HIR 块用例将前驱的单使用 LoadImm 强制映为 MEM，在后继 SetHostGPR 处请求局部发表；确认 producer 确实不在后继 roster、不产生特殊选择或 value-home/coalesced 标记，普通位置仍为原 MEM slot 0。fixed-class 仍为 2 用例/39 断言；八项持久化/寿命标记逐项相同，真实 target_spill_misses 六次。

十组 CRC/五项 checksum、实际 Block/Function 模式、spill 指标和零 target_overflow 保持。五组 bench 的全部有序记录和完整 shape 相同；五组 CoreMark 的公共 PC 大小与 formatter 范围外顺序相同，差异限已有格式化符号范围：

| CoreMark 配置 | 记录数，参考→本批 | RA host bytes，参考→本批 |
| --- | --- | --- |
| Function | 426→427 | 286624→286716 |
| bound-decode off | 427→427 | 286724→286724 |
| 独立 Block | 2417→2418 | 205220→205288 |
| fixed-class Function | 427→427 | 286388→286388 |
| fixed-class Block | 2417→2418 | 206556→206788 |

四份 Block 原始 dump 的哈希重新计算：两份 bench 全字节相同，两份 CoreMark 的公共 PC 仅七个已证明 CPUID 目标的四条 MOV 立即数组发生变化，其他公共字节相同。定位继续使用相同参考日志的具名 helper 证明、真实 ELF 符号和未改 frontend；load bias 为推断，不据此声明 Function 机器码全等。

十一份 workspace 样本的九类缓冲全部闭合，六项基础分配字段逐组相同，IR-slow 所有字段相同。三组 CoreMark 的 retention/pattern fill 随 formatter 工作集变化，其他样本字段相同。这里没有计量已删的私有 vector、剩余 ID/sites 缓冲或 target_writes 的扩容，不把相同公共计数外推为净堆收益。

### 实际成本与接续

实际 NDK DWARF 的 CopyCandidate 为 96→40 bytes；ResidencePreparation 328、TargetPreparation 2152、GPRBlockPreparation 64、PendingGPRBlock 184、UnitActions 864、SemanticFacts 624、CompileWorkspace 224、AllocationEdit 280、GPRRecolorSite 8 均保持。launcher .text 为 4453172→4453292，增加 120 bytes。

MatchPinnedGPRCopy 自身代码/固定 SP 减量为 6360/496→4284/384，调用方 PreparePinnedGPRCopies 为 1164/288→1280/272；新的 alias 子证明 lambda 为 952/176，RegisterCopyUses 为 624/160，AppendRecolorValue 为 696/128。RecolorPublication 自身反而为 3244/624→4224/640，新增投影/terminal/证明逻辑的 outlined 成本也另计。FinishConstraints 为 1308/208→1368/208；必要 sites、公共事务的 roots/old-homes/events 和 journal 仍存在。这些都是各函数自身的代码与 SP 减量，不代表完整传递栈峰值、净私有容量或编译提速。

本批三十五项范围核验通过，源码、十四份冻结/构建 ELF、原始归档、中间构建隔离、失败签名、静态代码与成本记录已核对。没有再采集 PMU 或重新执行 RV64。源码摘要 78a13c157d4d74431123043033de1dfe126aa56d1ee35895741cf0052100e23b，overlay 为 b0e5ebd4acbd76ae999aced44b0bc1d1e038809d70234902cd1de299c83b59a3，launcher 为 c8eb1599c0b741517992863499a9b86fcbb2f4d6ed115f65eb2bd18c4c795929；结果归 /private/tmp/swiftvm-middle-results/r5-copy-root-stream-v2-*。其余候选播种/资格与 pseudo-demand 扫描、共享窗口和公共事务私有暂存、完整语义宽度及 guest-state/helper 契约、全成本、原始基线 Block/FEX 资格和完整故障/并发矩阵继续开放，整个 R0–R7 目标保持进行中。

## R5 共享发表候选、索引阶段与精确 pseudo 查询

批次 r5-publication-roster-v1，相邻参照为 r5-copy-root-stream-v2。修改八个既有源文件，新增/退役源文件均为零，累计仍为 467 项源码与 113 项退役。没有新增 probe、环境开关或仓库测试脚本；设备测量复用 tools/redmagic 的正式入口。

### 候选与阶段边界

copy、select、transfer、publication-view 和 spill 五类选择现在只访问同一块局部候选序列，顺序来自唯一 UseFacts roster。候选在已有 dead-write/version 正向访问中播种，只提取零偏移、固定 home 且未被本轮判死的 SetHostGPR；保留每个家族的原提交顺序，不把五轮家族优先级改成逐节点交错。宽度、coalesced 和 pool 等条件并非五类都相同，仍在各自 matcher 判断；特别是不按发表视图宽度统一过滤，以免排除原先只检查底层 producer 的 select。

既有 publication_index 的 u32 缓冲依次承载块内候选位置、选择查询索引和最终写入排序。旧 publications_indexed 布尔被一字节 Unseeded/Candidates/Selections 状态替代，没有另加 owner 字段或公共 workspace 种类。IndexPublications 显式覆盖为选择索引，保持可重复调用；之后不允许再选择或请求候选，const PublicationFor 只查准备好的选择。Finish 结束查询，保留原物理写入顺序并清理阶段，不把候选位置解释成跨块 ID。

所有可选家族关闭时不在初始访问中分配候选；此后真实的显式 spill 请求可由非 const PreparePublicationCandidates 补建一次。各家族及显式 spill 接口删去重复 Block 参数，直接使用会话绑定的本块事实；Block 与 Function 的分配范围和策略保持各自边界。候选迭代期间只追加 selections，不改变正在借用的候选缓冲；最终索引覆盖发生在全部家族结束之后。

版本复用检测改用 ForEachValue，省去原 GetValues 临时集合。原 occurrence 顺序保持；首次命中后不再做版本成员查询，但仍会遍历该指令剩余参数结构，不宣称完全短路了底层遍历。

### scratch 的 pseudo 需求

RecolorPublication 仅在 GetUses(false) 与默认 GetUses() 不等时准备并查询本块 pseudo 使用边：前者含 pseudo，后者默认排除 pseudo。非空时继续构造完整 FlagsDemand 的 requested、pseudo_count、branch_only、has_save，不能把全单元 packed PseudoFlagsPlan 当作本块需求。SaveFlags(None) 虽没有有效位，仍是一个 pseudo occurrence；没有加新的需求表或缓存。后续必要的 resolved-use、scratch/fixed/width 证明仍保留，不宣称整个准备过程不再建图。

### 集中构建与实机验证

十四个 Android/ARM64 目标首次完整构建退出零后冻结，十四份 ELF 均与相邻版本不同。红魔 bf1359ff 新执行十二项 native 与十组静态；五个程序和两份 guest 的七项实际部署哈希前后匹配。

主套件仍为 746 用例，229439 断言，相邻为 229133；零 error，仍有十一例/六十六个失败断言。共同用例顺序和完整失败签名保持，主测试源码未改。成功断言总数 +306 不精确归因；既有 RunColdChild 的轮询断言依赖调度，JUnit 没有逐用例断言总数。

IR 快慢配置各为 85 用例、30064/30162 断言，分别增加二十三项，全部通过。新增用例用真实块与强制 MEM 位置验证正常预播种、全部家族关闭后的显式 spill、目标 home 23、选择与最终物理写入，两个变体共二十二项断言；既有 Function 外块 MEM 用例另加一项索引阶段拒绝断言。fixed-class 的 2 用例/39 断言全部通过，八项持久化/寿命标记保持，真实 target_spill_misses 为六次。

十组 CoreMark CRC/五项 bench checksum、实际 Block/Function 模式、spill 指标和零 target_overflow 保持。五组 bench 的完整有序记录及 shape 相同；五组 CoreMark 的公共 PC 大小不变，formatter 外顺序相同，差异限已有格式化符号范围：

| CoreMark 配置 | 记录数，参考→本批 | RA host bytes，参考→本批 |
| --- | --- | --- |
| Function | 427→426 | 286716→286624 |
| bound-decode off | 427→427 | 286724→286724 |
| 独立 Block | 2418→2415 | 205288→205000 |
| fixed-class Function | 427→427 | 286388→286388 |
| fixed-class Block | 2418→2419 | 206788→206696 |

四份 Block 原始 dump 重新核对哈希：两份 bench 全字节相同，两份 CoreMark 的公共 PC 仅七个已有 typed CPUID 目标的四条 MOV 立即数组发生变化，其他公共字节相同。具名 helper 证明继续使用同一参照日志、实际 ELF 符号与未改 frontend；load bias 为推断，不声明 Function 机器码全等。

十一份 workspace 样本的九类缓冲全部闭合，六项基础分配字段逐组相同，IR-slow 所有字段相同。普通 Function、Block 和 fixed-class Block 的 CoreMark 只有 retention_events/pattern_init_bytes 随 formatter 工作集变化，其余字段与样本保持。publication_index 为私有缓冲，候选容量、增长及剩余 ID/sites/事务暂存不在这些公共计数中；相同计数不能证明净堆收益。

### 静态成本

实际 NDK DWARF 的 CopyCandidate 为 40 bytes、ResidencePreparation 328、TargetPreparation 2152、GPRBlockPreparation 64、PendingGPRBlock 184、PublicationSelection 24、GPRResidenceAction 4、UnitActions 864、SemanticFacts 624、CompileWorkspace 224、AllocationEdit 280、GPRRecolorSite 8，均与相邻相同；新 PublicationIndexStage 为一字节。launcher .text 为 4453292→4456508，增加 3216 bytes。

| 函数 | 自身代码 bytes，参考→本批 | 自身固定 SP 减量 bytes，参考→本批 |
| --- | --- | --- |
| PrepareDeadPinnedGPRWrites | 3064→3700 | 672→640 |
| Prepare | 1228→1944 | 176→176 |
| PreparePinnedGPRCopies | 1280→1396 | 272→288 |
| PreparePinnedSelectPublications | 756→872 | 224→224 |
| PrepareFixedGPRTransfers | 696→832 | 304→320 |
| PreparePinnedGPRPublicationViews | 276→420 | 128→160 |
| PrepareSpilledGPRPublications | 492→588 | 160→176 |
| RecolorPublication | 4224→4260 | 640→640 |
| IndexPublications | 568→564 | 112→96 |
| FinishConstraints | 1368→1368 | 208→208 |
| IsPublicationCandidate，新函数 | —→432 | —→64 |
| PreparePublicationCandidates，新函数 | —→808 | —→160 |

这些数值是单函数自己的代码和 SP 减量，不是传递栈峰值。五次全 roster 输入改为访问 M 个粗候选，但增加了私有 M×4 字节的逻辑内容及相应容量/增长管理；并未消除所有 matcher、窗口或 use-graph 扫描。净私有内存与完整编译成本仍待完成。

### 相邻三方 PMU

本批使用同一红魔、CPU4、grouped cpu-cycles/instructions 与独立目标/profiler 退出记录，三个 round、两个迭代点 2000/12000，分别对 Function 与 Block 比较三方：相邻冻结 copy-root-stream-v2、当前 publication-roster-v1 和固定源码 FEX。共三十六个主点、六个独立冷服务辅助点，全部目标/profiler 退出与 CRC 通过；原始同组 enabled/running 比值均为一。实际 compare.py CLI 退出零且输入 manifest 前后哈希不变，五百五十份 raw 文件均与设备 sha256sum 匹配，八项部署身份前后相同，源码前后保持冻结。

以下为逐轮配对两点模型的本批/相邻变化百分比；保留全部三轮，MAD 是中位绝对偏差，不是置信区间，也不挑最快轮次。

| 配置 | 指标 | 变化中位数 | 三轮范围 | MAD，百分点 |
| --- | --- | --- | --- | --- |
| function | 指令斜率 | +0.0068% | -0.0033%～+0.0314% | 0.0101 |
| function | 指令截距 | -0.3269% | -0.3695%～-0.3235% | 0.0034 |
| function | 周期斜率 | +1.1001% | -0.6281%～+1.2646% | 0.1645 |
| function | 周期截距 | -2.6168% | -3.0570%～-0.2696% | 0.4402 |
| function | 耗时斜率 | +1.2319% | -0.6326%～+1.2942% | 0.0623 |
| function | 耗时截距 | -2.6369% | -3.1032%～-0.2865% | 0.4663 |
| block | 指令斜率 | +0.0371% | -0.0038%～+0.0710% | 0.0338 |
| block | 指令截距 | -0.5499% | -0.6162%～-0.2654% | 0.0664 |
| block | 周期斜率 | +1.0663% | -1.4993%～+1.2541% | 0.1878 |
| block | 周期截距 | -2.0752% | -4.2670%～+0.6199% | 2.1919 |
| block | 耗时斜率 | +1.0506% | -1.4611%～+1.2836% | 0.2331 |
| block | 耗时截距 | -2.0914% | -4.2364%～+0.5519% | 2.1449 |

六个指令斜率均在相邻百分之一退化门内，变化最大为 +0.0710%。周期/耗时斜率中位数约增加百分之一，三轮有正有负；这些正向变化继续作为未归因成本保留。指令截距虽下降，也同时包含 loader/runtime/编译，不解释为纯编译提速；没有据此关闭完整成本门。

设备 primary 测量前后 governor 均为 walt，CPU4 下上限均为 614400/1171200 kHz，瞬时频率在此范围；USB 在线、Charging、电量 83%，battery 温度 28.4～28.6°C。thermal 31/cpuss-0 为 32.0～35.2°C，35/cpu-1-0 为 32.4～35.2°C，41/cpu-1-6 为 31.6～33.6°C；并非固定瞬时频率实验。

FEXLoader/FEXServer 继续采用 a156752fae3a4f65fefb1ed95af980bcdfac43d7 与既有 Android 补丁，ELF 为 8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175 / 9fe58a9946e9105c49911518356fe29a4ed3143c1fe90803b57afe112c225e8c。固定 guest 和显式配置保持，FEX 仍有剩余 CPUID 能力差异。下列比值仅为该外部参照的本组观测，不是完整等价配置的正式性能结论：

| 配置 | 当前/FEX 指令斜率中位数 | 周期斜率中位数 | 耗时斜率中位数 | 指令截距中位数 |
| --- | --- | --- | --- | --- |
| function | 1.6905 | 1.9540 | 1.9529 | 11.4702 |
| block | 2.1533 | 2.4773 | 2.4764 | 7.4230 |

预启动私有 foreground FEXServer 位于主测点进程树之外；其启动、整组请求、间隔空闲与退出聚合为 79245067 instructions、65601118 cycles、193.117301 秒。不能把这一整组聚合算作单点或纯启动成本。六个 cold-service 点各自使用新目录，自动服务启动在 launcher 活跃期间按继承范围计入，尾部与清理另列；它们不混入三十六个主点。主服务 transport 退出零、活动 socket 清空，各 cold 私有服务也已清理。

本组只比较紧邻版本；起点 2804a51b 的 Block low32 资格失败仍保留，没有被替换成相邻基线。其他代表负载、完整 guest 能力与版本适用性仍待完成，formal FEX gate 保持未通过。

### 归档与接续

本批四十四项范围核验通过：源码及十四份冻结/构建 ELF、十二项原生与十组静态、原始归档、失败签名、代码/存储和新的 PMU 协议均已核对；没有重新执行 RV64。源码摘要 66a1ee7fb1aa2d7679147e0e932f85f2f7c1cfd1e6d7f30d69b24d82aca569ce，overlay 为 0ca76a6a01be29c9668a047e7ec0ab55b29463740b110c7bf03013954496287b，launcher 为 a041bc2acb5e48c6d5ad1b8389174f90fed86257fde8030e2f0619697a85a0ba；结果归 /private/tmp/swiftvm-middle-results/r5-publication-roster-v1-*，PMU 原始记录位于同名 -pmu 目录。

继续家族资格/窗口与其他 pseudo 查询、ID/sites 和公共事务 roots/old-homes/events 的私有暂存、完整语义宽度及 guest-state/helper 契约、净私有容量/初始化/复制与传递栈/编译成本、当前公共修改的 RV64 及完整故障/缓存/AOT/SMC/link/并发/关闭矩阵。文档的文件职责已同步；整个 R0–R7 目标仍在进行中，full-cost/full-refactor gate 均未关闭。

## R4 recolor 紧凑快照与按释放触发的 mask 修复

批次 r4-recolor-values-v1，相邻参照为 r5-publication-roster-v1。修改两个既有源文件：公共 allocate/recolor.cpp 与既有 tests/cases/allocation.cpp，新增/退役源文件均为零，累计仍为 467 项源码与 113 项退役。没有新增 probe、配置开关或仓库测试脚本；只在全部修改和十四目标构建完成后集中上机一次。

### 事务与单次调用的不同状态

原 RecolorGPR 先建立、排序并去重一份 u32 roots，再为相同物理 root 建立 OldHome 列表，后者复制完整十六字节 Map。提交后实际只需查询旧 home、原先是否 GPR 与 fixed 标记；两份列表已合为一份八字节 RecolorValue，保存 id、old_home、was_gpr、was_fixed。成员查询、兼容检查、mapping 与 placement 重写均借用同一排序表，不再拥有并行 old_homes。

输入仍先校验编号，再按公共物理 ResolveId 归一化，排序、去重；它与上层语义 root ID 是不同层次。UnsupportedLocation/OutsideWindow/capture/fixed/owner/partial-alias/consumer/reload 的原准入顺序与首次拒绝保持，所有前置检查完成后才写物理位置。没有新增后端依赖、公共 owner 字段或 workspace 缓冲种类，Block 与 Function 的调用范围继续由现有会话和窗口规定。

紧凑快照记录的是本次 RecolorGPR 开始时的映射，AllocationEdit 的 first-write 日志记录整次事务开始前状态。一个事务可以连续 recolor 同一组值，两者不能合并：第二次的 placement 兼容与 retarget 必须使用第一次完成后的 home，最终拒绝仍恢复事务之前的 mapping、视图、reload 与 masks。完整 journal、外来值的稀疏索引和必要缓存失效均保留。

### 按实际释放重建 mask

mapping 与 placement 重写、目标 reload 删除仍按原顺序进行，后者也将被释放的 scratch 寄存器加入 released 位集。只有 released 非零时才扫描全 mapping、建立和排序 Event、初始化三十二项 live 计数并逐 site 重建对应位。MEM 来源且没有被删除 reload、或同 home 仅切换 fixed class 的场景不释放寄存器，原算法也不会改动任何 site mask 位，现在跳过这一空工作。

真实释放场景仍保留捕获点 birth、live end、其他 live owner、其他 reload owner、保留寄存器和 site clobber；未释放的位与 FPR masks 不动。两种路径末尾都执行原 fixed/preservation 缓存失效。没有把 scratch/宽度等目标准入下放或省略，调用方仍在 Commit 前复证并可回滚。

### 集中实机验证

首次完整十四个 Android/ARM64 目标构建退出零后冻结；十四份 ELF 均与相邻版本不同，构建与冻结哈希一致。红魔 bf1359ff 新执行十二项 native、十组静态，实际五个程序与两份 guest 的七项部署身份前后匹配，退出全部符合预期。

主套件为 748 条 JUnit 用例记录、229310 断言；相邻为 746/229439。失败集合与完整签名及共同记录顺序保持，仍为十一例/六十六个失败断言、零 error。新增的是原 allocation TEST_CASE 内两个 SECTION，JUnit 分别产生两条通过记录；最初 coverage 元数据误按 TEST_CASE 数量标为零新增，已按冻结源码中的两个 SECTION 名修正，并保留前后计划哈希。源码、ELF、执行、原始 XML 和失败预期均未改，也没有为此重新上机。

既有 recolor 用例改用无序、重复及 REF 输入，仍证明合并后的物理 root 集相同；两个新分支分别覆盖：

- 同一事务先迁到 home 19，再迁到 home 20，确认第二次基于当前 placement，最终析构回滚恢复初始 mapping/视图/reload/version；包含复用的既有检查，共三十三项直接断言。
- MEM-only root 与 REF alias 经稀疏 site 转到固定 home，再在同 home 切为普通 GPR；确认全过程 site 的 GPR/FPR masks 保持、只登记一个改动值，分别验证提交与回滚，两种路径共四十三项直接断言。

新增源码直接断言合计七十六项。全套成功断言总数差值为 −129，新增之外的 −205 不精确归因于某例或轮询；既有 RunColdChild 轮询依赖调度，JUnit 没有逐用例断言数。IR 快慢均保持 85 例、30064/30162 断言且全部通过；fixed-class 保持 2 例/39 断言全通过，八项寿命/持久化标记逐项保持，真实 target_spill_misses 六次。

十组 CoreMark CRC、五项 bench checksum、实际 Block/Function 模式、spill 指标和零 target_overflow 保持。五组 bench 的完整有序记录与 shape 相同；普通 Function 与 fixed-class Function CoreMark 也完整相同，其他 CoreMark 变化限已有 formatter 范围，公共 PC 大小及 formatter 外顺序保持：

| CoreMark 配置 | 记录数，参考→本批 | RA host bytes，参考→本批 |
| --- | --- | --- |
| Function | 426→426 | 286624→286624 |
| bound-decode off | 427→426 | 286724→286632 |
| 独立 Block | 2415→2416 | 205000→204988 |
| fixed-class Function | 427→427 | 286388→286388 |
| fixed-class Block | 2419→2418 | 206696→206624 |

四份 Block dump 的原始哈希重新核对；两份 bench 全字节相同，两份 CoreMark 仅各七个具名 CPUID helper 目标的四条 MOV 立即数组不同，其他公共字节相同。同一参照日志的 typed 证明、真实 ELF 符号与未改 frontend 已核对，load bias 仍为推断，不声明 Function 全机器码相同。

十一份 workspace 样本的九类缓冲全部闭合，六项基础分配字段逐组相同，IR-slow 全部字段相同。只有 bound-decode-off、Block 与 fixed-class Block CoreMark 的 retention_events/pattern_init_bytes 随 formatter 工作集变化；其余字段/样本保持。私有 root/event/journal 的容量不在这些计数中，不能据此宣称净堆收益。

### 实际成本与接续

实际 NDK DWARF 确认 Map 为十六字节、旧 OldHome 为二十字节且在候选中消失，新 RecolorValue 为八字节。按去重后的逻辑 root 计算，原四字节 ID 加二十字节 OldHome 的并行内容改为一份八字节记录；输入归一化前可能含重复/REF，初始 root 元素由四变八字节，相应 capacity、增长、排序移动和净分配流量仍需计量，不能把逻辑每项缩小外推到所有容量形状。

AllocationEdit 280、GPRRecolorSite 8、CopyCandidate 40、ResidencePreparation 328、TargetPreparation 2152、GPRBlockPreparation 64、PendingGPRBlock 184、PublicationSelection 24、GPRResidenceAction 4、UnitActions 864、SemanticFacts 624、CompileWorkspace 224 均保持。RecolorGPR 主体自身代码/固定 SP 减量从 4892/416 降为 4816/384；排序/查找特化及 outlining 的代码布局另列，launcher .text 反而为 4456508→4459876，增加 3368 bytes。自身帧不代表传递或动态栈峰值，完整私有容量和总体编译成本继续开放。

本批三十六项范围核验通过，源码、十四份冻结/构建 ELF、失败签名、新 SECTION 覆盖、原始归档、静态代码与存储记录均已核对；没有再次采集 PMU 或执行 RV64。上一批合格 PMU 的 publication-roster ELF 身份不变，不作为本批 c6f9e2250cfbb1270fa75a35996829f3ca1141606935d968b620f5a5273dc1f5 的执行证据。源码摘要 dab756d67b08e7fe8ae9cae9ea75f696d2a70bc8b05db17a548e961e594926ab，overlay 为 1ba75272d9fea8ed273d250327173473e74a8de705a0f488dfda3f135d18411d；结果归 /private/tmp/swiftvm-middle-results/r4-recolor-values-v1-*。

剩余事务 journal/event、候选/窗口的私有存储，完整语义宽度及 guest-state/helper/fault 契约、传递栈与总体成本、当前公共修改的 RV64 和完整缓存/AOT/SMC/link/并发/关闭矩阵继续推进；原始基线 Block 与正式 FEX 资格限制保持。文件职责已同步，整个 R0–R7 目标仍在进行中。

## R1 整数算术/移位位事实与可变反馈

批次 r1-integer-bit-transfer-v1，相邻参照为 r4-recolor-values-v1。修改五个既有文件：analysis/value_bits.h/.cpp、passes/value_worklist.h/.cpp 与 tests/ir_contract_test.cpp；新增/退役源文件均为零，累计仍为 467 项源码与 113 项退役。没有增加 probe、配置开关或仓库脚本。

### 共享语义与实际消费者

有界 IntegerValueAnalysis 新增 Add/Sub/Neg、AndNot 和同宽 32/64 位立即数 Lsl/Lsr/Asr/Ror 的位事实。加减按位传播可能的 sum/carry；减法通过补码及初始进位处理，常量路径使用 u64 模运算，避免宿主有符号溢出。full-adder 的六十四项转移在编译期生成，ELF 中只占六十四字节只读数据，没有逐查询初始化。未知位保留所有可能取值，不假定不同输入独立的取值组合一定真实可达；这种上界允许丢失精度，不允许凭空证明位。

立即数移位保留已知零/一位，按逻辑零填充、算术符号填充和旋转转移；零位移不执行宿主宽度大小的移位。窄输入、不同输入/结果位宽及变量移位仍保守未知；同宽立即数超出合法范围使整次查询未知。literal 的原始 operand-view 语义和非 literal 加宽不推断高位为零的边界保持。不能用这些值事实代替物理 W/X、coalesced alias 或 guest home 证明。

查询仍无分配、无缓存、无 consumer 图；总访问预算三十二，非法/循环/耗尽时整次查询未知。decode 的 FoldExtend 与残余 pass 的 FoldWidthValue 继续消费同一入口，仅在完整结果确定且输出契约允许时改成常量。它扩大实际分析能力，不是完整单元宽度摘要，剩余算术/移位、CFG 传播与生命周期契约继续开放。

TransfersIntegerValueBits 现在同时定义查询和可变 worklist 的转移 opcode 范围；旧 ValueWorklist::TransfersBits 声明/实现已删除。Add/Sub/Neg/AndNot 和移位即使没有自己的局部折叠，也会将已改变输入的通知继续传给后续宽度节点；候选资格与传播资格仍是不同职责。队列仍使用原稳定身份、唯一使用视图与 8N+32 预算，没有另建缓存或 epoch。

### 集中验证与控制进程恢复

首次完整十四目标 Android/ARM64 构建退出零后冻结，十四份 ELF 均与相邻不同。集中执行先安排快/慢 IR，再运行主套件及其他 native；十二项原生均完成且退出符合预期。控制进程随后以 143 结束，尚无静态完成记录；检查确认主机与设备没有相关残留运行，源码仍冻结，七项设备身份匹配。保留十二条原生执行记录及其原始哈希，只继续十组静态，最后再核对设备身份并以 adb pull 和 SHA 获取原始归档。没有重新执行主套件；原因与恢复记录归 execution-resume，控制中断不作为测试通过或失败证据替代。

主套件仍为 748 条记录，229288 断言，参照 229310；共同用例顺序和完整失败签名保持，仍为十一例/六十六个失败断言、零 error。主测试源码未改，成功断言总数 −22 不精确归因；既有 RunColdChild 轮询数依赖调度，JUnit 没有逐用例断言总数。八项寿命/持久化标记相同，真实 target_spill_misses 六次，fixed-class 2 例/39 断言均通过。

IR 快慢各由 85 增至 87 例，77408/77506 断言，分别增加 47344 项且全部通过。既有 typed decode/residual 对照由六种扩至十四种形状，验证算术/逻辑/移位链后的实际宽度常量化。两个新增用例分别覆盖：

- 在 8/16/32/64 位加减、取负与 AndNot 中枚举独立四位输入域，并检查每一个报告的已知零/一位、常量和符号复制范围；同宽 32/64 位移位以独立的逐位搬运参考枚举输入和零/一/半宽/末位计数，不调用被测 adder 或 shift。
- 保持一个真实可变 ValueWorklist，先确认输出不能折叠，再通过 RewriteContext 将上游改成常量；通知跨过 Sub 与 Lsr 后，后续 ZeroExtend32To64 实际变为零常量。关闭自身局部规则不能阻断这种传播。

### 静态输出与真实代码变化

十组 CRC、五项 bench checksum、实际 Block/Function 模式、spill 指标和零 target_overflow 保持；五组 bench 的有序记录与完整 shape 相同。三个 Function CoreMark 配置的公共 PC 大小保持，普通 Function 的完整记录也相同。Block 模式确实出现两处新优化，旧“公共 PC 全等且只变 formatter”的严格条件不再成立；对照保留这些真实变化：

| 客体 PC / 符号 | Block / fixed-class Block 主机 bytes | 核对结果 |
| --- | --- | --- |
| 0x21c09a / core_init_matrix | 112→96 | -1+1 与 8+8 的宽度结果改为 MOV 0 / MOV 16，四条主机指令删除 |
| 0x25f8d6 / __dtoa | 416→420 | 1+1 改为常量 2；后续短窗口消费者分别装载 2，寄存器选择及分支位移随之变化，净增加一条指令 |

前后客体反汇编和四组配对主机汇编已核对并归档；raw dump 中未解码的指令或 inline data 原字保留为 .word，没有从比较中丢弃。这个审查与实际 CRC 执行不构成形式等价证明，也不关闭正式性能门。

| CoreMark 配置 | 记录数，参考→本批 | RA host bytes，参考→本批 |
| --- | --- | --- |
| Function | 426→426 | 286624→286624 |
| bound-decode off | 426→427 | 286632→286724 |
| 独立 Block | 2416→2419 | 204988→205552 |
| fixed-class Function | 427→426 | 286388→286296 |
| fixed-class Block | 2418→2418 | 206624→206812 |

新旧 formatter 路径的额外/缺少记录全部位于既有符号范围。排除 formatter 和已单列的两处真实折叠后，有序记录相同。四份 Block dump 均重新验证原始哈希；两份 bench 全字节相同，两份 CoreMark 除各七个已证明 CPUID 目标的四条 MOV 地址组及上述两处真实折叠外，所有公共字节相同。CPUID 地址规范化与真实代码变化分开记录，不把后者写成机器码全等；Function 全机器码亦未声明相同。

十一份 workspace 样本全部闭合且九类缓冲合计匹配。十组静态的六项基础分配字段相同；bound-off、Block、fixed-class Function/Block 的 CoreMark 只有 formatter 相关 retention/pattern fill 不同。包含新覆盖的 IR-slow 相邻增加九次分配/释放、396 bytes 请求/释放，峰值及 peak-retained 各增八字节，pattern 初始化增 280 bytes，retention_events 增 83；不把该总差值精确分配给某一个用例或外推成生产成本。

### 实际代码、栈与接续

IntegerValueBits 24、IntegerValueAnalysis 8、ValueWorklist 64、SemanticFacts 624、CompileWorkspace 224 及本批记录的其余物理/target owner 布局均相同。launcher .text 为 4459876→4461372，增加 1496 bytes，另增加六十四字节只读 adder 表。

| 函数 | 自身代码 bytes，参考→本批 | 自身固定 SP 减量 bytes，参考→本批 |
| --- | --- | --- |
| Definition | 1836→1760 | 352→480 |
| Query | 892→892 | 160→160 |
| Add，新函数 | —→432 | —→0 |
| Shift，新函数 | —→1160 | —→176 |
| QueueUsers | 1648→1576 | 160→160 |
| Queue | 628→628 | 144→144 |
| FoldWidthValue | 412→412 | 160→160 |
| ApplyIntegerWidth | 456→456 | 80→80 |
| AnalyzeIntegerValue | 176→176 | 80→80 |

Definition 自身帧增长 128 bytes，Shift 还增加一层调用帧；三十二次 value 访问的上限不是总机器指令或递归栈峰值。位传递运算、worklist 传播、全部传递栈及整体编译成本继续计量，不凭四条静态指令减少宣称总性能提高。本批没有采集 PMU 或执行 RV64，上一批测量仍对应各自冻结 ELF。

本批三十六项范围核验通过；源码、十四份冻结/构建 ELF、原生恢复记录、失败签名、原始归档、真实 Block 优化与 CPUID 地址差异、存储和静态成本已核对。源码摘要 db5e2705fe295ff738a51162c011d73b15585f801cc867ea173e469d2a2af95c，overlay 为 9c44a6de13ef2d52ef60dcd33fbb65bc366b95721b49e7b0d6bbc20fe735a548，launcher 为 911b4711dc639128593f2a7dbde0183c948f0684ac39b3c1b43e9de1aa1603bc；结果归 /private/tmp/swiftvm-middle-results/r1-integer-bit-transfer-v1-*。

完整单元语义宽度、其余 guest-state/helper/fault 契约，候选/事务私有存储与总体成本、当前公共修改的 RV64 和完整缓存/AOT/SMC/link/并发/关闭矩阵，原始 Block 基线与正式 FEX 资格仍开放。方案文件职责已同步，整个 R0–R7 目标保持进行中。

## R2 helper 状态解析、写范围策略与 CallLocation 失效

批次 r2-helper-state-effects-v1，相邻参照为 r1-integer-bit-transfer-v1。修改十一个既有源文件，没有新建或退役源文件。helper_call_contract.h/.cpp 是此前尚未修改的 Git 跟踪文件，本次首次加入变更源码归档，条目因此从 467 增至 469；其相邻原文由不变的 2804a51b 读取并保存独立 SHA/snapshot，不能将它们误记为新增源文件。累计退役仍为 113 项，没有新增 probe、配置开关或仓库脚本。

### 公共 trait 来源与目标规则

ir/args.h/.cpp 新增五字节 HelperStateEffects 值记录及唯一 Resolve 解析函数，保存 direct、guest-state access、fault、reentry 与 uniform effect ID。间接 Lambda 保持 conservative 默认值；原 Lambda 的 packed 元数据和读接口未改。此记录只描述直接 helper body，Reads/Writes 本身不替代 reentry 判断，也不包含 host ABI、寄存器或 FPCR 策略。

ir/instr.cpp 的 Inst::GetEffects 与 ARM64 HelperCallContract 均使用同一解析函数，删除各自重复的 guest-state/fault/reentry 解包和分类。Inst 继续仅在直接 CallLambda 且 NoReentry 时按 body traits 清除 guest-state/fault 位；ReadMemory、WriteMemory、Call 副作用不精化，CallLocation/CallDynamic 的通用 effects 保持 opaque。HelperCallContract 以同一值记录替换原五个分散字段，继续独立处理 FeatureSet、preserve-all/general-only/pinned ABI、host NZCV/FPCR 和精确寄存器 clobber；Sse42Str 的已审计特例保持，未将目标规则移入公共 IR。

没有新增逐指令 effects 表、SemanticFacts 字段或派生缓存。各消费者仍在需要时查询同一解析函数，不宣称全单元只解析一次；签名/持久化路径对原始 Lambda tags 的读取保留。

### 写范围的两种语义

UniformEffectSet 是独立的 call-wide uniform 写范围承诺，不说明 helper 读哪些值，也不证明无 fault 或 reentry。原转发路径允许用已声明范围精确失效，而循环提升还要求直接且不重入。两者不能合并成一个更强或更弱的准入：

| UniformWritePolicy | 用途与准入 |
| --- | --- |
| Declared | decode/残余前向缓存按显式声明的范围失效；Unknown、非法数组或越界时全部失效。不因 ReadOnly 等 body traits 自动采用更强证明 |
| NoReentry | 循环稳定状态先要求直接、NoReentry；ReadOnly/None 可证明空写，否则还须已知合法写范围。仅有 None 写 ID 而仍可能 reentry 的旧调用继续拒绝循环提升 |

HelperUniformWrites 要求显式传入策略，无旧两参数重载；builder、state 和 loop_hoist 全部迁移。范围 lookup、空数组与边界验证共用实现；先验证全部范围再返回，loop-hoist 不再自行读取 trait/逐项校验或在发现晚期非法范围前先填充部分 writes。原循环提升的正负资格保持。写范围没有用于放宽 state DSE、fault sink 或 observer 边界，未知 helper 仍保守。

### CallLocation 缓存边界修正

代码审查确认 IrBuilder::UniformAfterAppend 与公共 state forward switch 只列 CallLambda/CallDynamic，漏掉 CallLocation；因此 StoreUniform 后经过未知 CallLocation，后续 LoadUniform 仍可能复用调用前值。现三种调用均走同一 Declared 失效入口：范围特性开启时只清除已声明写区间，关闭或未知/非法时全部清除。这个修正针对语义缓存边界，不改变 helper ABI 或调用发射。

新增真实 IR 用例覆盖 decode 与残余 pass、范围开启/关闭两态、Unknown/None/局部写范围：未知调用后两份 load 均保留，None 仅在范围开启时允许两份转发，局部写只失效被写低区间；高区间保留合法转发。测试使用既有 SVM_BUILDER_UNIFORM 开关确保 builder 路径实际启用，并在作用域结束恢复原配置；没有加入生产开关或执行伪 helper 地址。

### 集中验证

首次完整十四目标 Android/ARM64 构建退出零后冻结，十四份 ELF 均与相邻不同。红魔 bf1359ff 一组完成十二项 native、十组静态，全部退出符合预期；七项实际程序/guest 部署身份前后相同，原始结果归档经 adb pull 和 SHA 核对。没有重复测试或本批 PMU/RV64 执行。

主套件保持 748 例，229341 断言，相邻 229288；共同用例顺序及完整失败签名一致，仍为十一例/六十六个失败断言、零 error。主测试源码未改，成功断言数 +53 不精确归因于某一例或轮询；RunColdChild 的既有调度差异保留。八项寿命/持久化标记相同，真实 target_spill_misses 六次，fixed-class 2 例/39 断言全部通过。

IR 快慢均由 87 增至 90 例，78259/78357 断言，各增 851 项且全部通过。三个新增用例的直接断言分别为：

- guest-state 四种 access、fault/reentry 两态与直接/间接组合，独立比较公共记录、目标契约、IR effects 和保守 memory/call 位，共 640 项。
- 三种 Call、直接/间接与七种写范围/body 情况，验证 Declared 与 NoReentry 的不同准入、非法数组/越界拒绝及 opaque CallDynamic/CallLocation effects，共 186 项。
- decode/残余、范围开关与三个 footprint 的真实 CallLocation 转发边界，共 25 项。

十组 CoreMark CRC、五项 bench checksum、实际 Block/Function 模式、spill 指标和零 target_overflow 保持。五组 bench 与三组 Function CoreMark 的完整有序记录及 shape 均相同；两组 Block CoreMark 的差异限已有 formatter 符号，公共 PC 大小和 formatter 外顺序保持。上一批两处真实算术折叠也保留相同代码大小，没有在本批重新记为新收益。

| CoreMark 配置 | 记录数，参考→本批 | RA host bytes，参考→本批 |
| --- | --- | --- |
| Function | 426→426 | 286624→286624 |
| bound-decode off | 427→427 | 286724→286724 |
| 独立 Block | 2419→2414 | 205552→204972 |
| fixed-class Function | 426→426 | 286296→286296 |
| fixed-class Block | 2418→2413 | 206812→206016 |

四份 Block dump 原始哈希重新验证；两份 bench 全字节相同，两份 CoreMark 的公共字节仅各七个 CPUID helper 目标的四条 MOV 地址组不同。使用相同参照日志的 typed 证明、实际 ELF 符号与未改 frontend；load bias 仍为推断，不声明 Function 全机器码相同。十一份 workspace 样本九类缓冲全部闭合/合计匹配，六项基础字段逐组相同，IR-slow 全部字段相同；两组 Block CoreMark 只有 formatter 相关 retention_events/pattern_init_bytes 变化。私有 uniform 字节表、metadata 注册和完整编译成本不在这些公共计数内。

### 布局、代码与栈

实际 NDK DWARF 确认 HelperStateEffects 为五字节、UniformWritePolicy 一字节；HelperCallContract 仍二十字节、Lambda 仍十一字节。SemanticFacts 624、CompileWorkspace 224、UnitActions 864、ResidencePreparation 328、TargetPreparation 2152 及其他记录的分配/target owner 均相同。launcher .text 为 4461372→4461428，增加 56 bytes。

| 函数 | 自身代码 bytes，参考→本批 | 自身固定 SP 减量 bytes，参考→本批 |
| --- | --- | --- |
| HelperStateEffects::Resolve，新函数 | —→96 | —→0 |
| HelperCallContract::Resolve(Inst) | 760→1112 | 128→160 |
| HelperCallContract::Resolve(Lambda) | 288→216 | 48→64 |
| HelperUniformWrites | 376→440 | 112→112 |
| IrBuilder::UniformAfterAppend | 968→972 | 96→96 |
| CleanupStateBlock 主体 | 9036→9036 | 3280→3280 |
| Inst::GetEffects | 376→340 | 96→96 |

目标 Inst/Lambda Resolve 自身帧分别增长三十二/十六字节，公共解析函数是新增调用。自身帧不代表完整传递栈峰值；不能把逻辑解包合并、相同 owner 布局或相同公共 workspace 计数外推为净私有容量、完整编译或运行提速。

本批三十六项范围核验通过，源码/首次修改的原文、十四份冻结与构建 ELF、失败签名、原始归档、代码与存储记录均已核对。源码摘要 c943da20a445472e6417af2121808d0677e49dce43cdeec853f6ee7f98761210，overlay 为 da36c2207218e563d81edcc0e2d2c3ef097800b3e9d8a6401f8f5e0692a71b91，launcher 为 01dc59b0d1f1d2d5f5b4eec6189868cb74dea8268e55f2e34f4c34127059f145；结果归 /private/tmp/swiftvm-middle-results/r2-helper-state-effects-v1-*。

GuestStateMap 的不同入口/物理视图未盲目合并；其余 guest-state/helper/fault/observer 契约、完整单元语义宽度、私有容量与全部编译/传递栈成本、当前公共 IR 修改的 RV64 以及完整缓存/AOT/SMC/link/并发/关闭矩阵，原始 Block/FEX 资格仍开放。文件职责已同步，整个 R0–R7 目标仍在进行中。

## R7 当前公共中端的 RV64 检查点与强 fence 回归

批次 r7-rv64-execution-20261024-v6，验证当前 r2-helper-state-effects-v1 源码，未修改生产代码或测试源文件。变更归档仍为 469 项、退役 113 项；完整构建输入另冻结 2552 份文件，此数是本次输入清单，不是全仓库源文件数。没有新增 probe、生产开关或仓库脚本。

### 构建与执行环境

使用独立只读源码快照，GCC 14.2.0、Release、rv64g/lp64d 和目标 sysroot，配置及完整十四目标构建均首次退出零；随后冻结十四份 EM_RISCV ELF。与 v5 相比九份 ELF 改变、五份相同，本次注册用例全部重新执行，没有将相同 ELF 的历史成功记成新执行。

用户态与 system QEMU 均沿用已验证的 10.0.13 构建及既有 flush-icache 适配，未修改仿真器。需要目标 syscall/seccomp 的用例使用 Linux 6.12.113、OpenSBI、两 hart 和 TCG；重新打包本批 rootfs，内嵌的 raw-cache ELF 与本批冻结 ELF 一致，init 的五种模式及内核、固件不变。完整 rootfs 输入、内核配置、运行库、仿真器与启动命令各自保存身份记录。

### 一组分环境验证

| 范围 | 本批实际执行 |
| --- | --- |
| 已注册用户态 | 32 项 CTest 全部新执行，退出零，无 failed/error/skipped；含 Block/Function、扩展/向量、flags、调用、恢复、持久化、边界与寿命用例 |
| 已注册内核态 | 5 项 raw-cache 模式在双 hart guest Linux 中全部新执行，外层各退出零；未放回已知能力不足的用户态环境重复尝试 |
| 独立 TSO 可执行程序 | 既有程序以 --expect-strong 完整执行，退出零，220321 项 checks |
| 仅构建 | 两个 compile-cost 程序和 emit 工具；未在 TCG 上采集或宣称编译性能 |

三十七项注册名称、实际命令和 properties 与 v5 完全一致。结果为三十二项新用户态加五项新 guest 内核，覆盖全部注册项、涉及十份 ELF，复用数为零；不是一次三十七项 CTest。独立 TSO 是额外的一份程序，不混入注册数量。

cross-hart 完成 128 轮、2048 次同地址重用读取，hart 0/1 均可见；四种 seccomp 拒绝模式的子进程预期退出 127，未返回或继续执行 synthetic post operation。测试外层退出零表示这些失败关闭断言通过，不能将子进程 127 当成未发生错误路径。

TSO 本批检查八种 load/store 编码、MP/LB/WRC/SB-fence/SB-allowed 五类各四种宽度且各 5000 轮，二十组 forbidden 均为零；八组软件 CAS128（四种宽度、registered/anonymous 两态）各有两个 writer 完成 5000 次成功、reader 完成 20000 次读取并见到两名 writer，lease/gate 恢复检查通过。强 fence 的编码差值与既有未对齐回退一致；当前实现没有生产窄 fence 控制，不将 --expect-narrow 误用为当前实现的通过标准。

### 身份、归档与剩余范围

结果归 /private/tmp/swiftvm-middle-results/r7-rv64-execution-20261024-v6-*，含 source-manifest/inputs、build-record、binaries/binary-comparison、registered、user-plan/execution/JUnit/LastTest、kernel-vm-plan/rootfs-inputs/artifacts/console/execution、tso-plan/execution 与逐项 test-summary。源码摘要仍为 c943da20a445472e6417af2121808d0677e49dce43cdeec853f6ee7f98761210，完整冻结输入摘要为 4af2914061935df84440f0227d42806081b519e02e9ace28dfc247dbab33341b。原始日志、ELF 和运行环境均按 SHA 关联，范围核验另见 quality-gates.json。执行结束后仅停止本批自建容器，其余任务容器保持原状态。

本批未重跑 Android 或 PMU，最新红魔结果仍对应上一批 helper-state ELF。三十二项用户态、五项 guest 内核和 TSO 均是模拟执行；不证明物理 RV64 的缓存/内存模型，也不提供性能数据。完整类型/CFG、guest-state/helper/fault 契约、其余 fault/cache/AOT/SMC/link/并发/关闭矩阵和总体成本、正式 FEX 对照继续开放，完整 R0–R7 不因本检查点完成而关闭。

## R2 固定结果默认类型、冷错误路径与 RV64 接续

最终批次 r2-fixed-result-types-v4，相邻参照为 r2-helper-state-effects-v1。修改十个既有源文件，无新增/退役源文件。ir_meta.h、ir_types.h 此前未进入变更归档，本次首次加入，归档由 469 增至 471 项；旧内容由 2804a51b 保存 snapshot/SHA，不是新增文件。累计退役保持 113 项，无新增 probe、配置开关或仓库脚本。

### 参数登记前定型

ir.inc 的二十三种固定结果操作由 ir_meta.h 投影到既有 IRMeta::result_type；TestFlags/TestNotFlags 的 token 改为 BOOL，ArgType 仍为 Value，opcode 编号不变。Inst 构造和空结果 SetInst 先取得默认结果类型，再登记参数；删除构造函数中两种 predicate 的单独判断。固定结果不再采用首参数的宽度，HIRValue 的初始记录也保持正确。

IrBuilder 的固定结果默认入口直接进入 AppendTyped/共享规则，显式 template 类型须匹配固定结果；无类型声明的 ZeroExtend64 也能执行已有常量规则，不修改来源定义。显式运行时类型检查与 FlagsIntent::Unknown 保持，未把“尚无 pseudo use”当成无 flags 输出。Block/HIR 的低层显式结果及改写契约保留；三处刻意使用 U32 predicate 定义的夹具显式恢复 U32 use view。通用 Value 返回操作的旧参数推导仍须迁移，本批不是全部类型入口清零。

GetIRMetaInfo 继续拒绝 COUNT/越界 opcode，异常类别和原消息保持；字符串构造/错误报告归非内联冷函数，声明向调用方表达冷路径。非法构造在读表前拒绝，没有通过删检查缩小构造代码。未改变 runtime/dispatcher、opcode 编号或序列化字段；这些事实不替代完整缓存兼容性验收。

### 中间记录与最终集中回归

v1 完整十四目标构建通过后，首项 IR 测试发现新增独立 Block 夹具遗漏 Module::Push，运行时正确返回 CodeMiss；仅补登记及成功检查，失败 XML、日志、源码和 ELF 保留。v2 的十二项 native/十组静态通过，但静态检查发现构造有效路径因内联错误报告携带九十六字节帧，因此继续拆出冷错误路径。v3 仅构建/检查成本；v4 补齐声明属性，launcher 与 v3 完整字节相同，v4 执行最终分组回归。RV64 v7 仅构建冻结 v2 输入，没有执行。沿革见 intermediate-records.json，未把中间成功冒充最终执行。

v4 首次完整十四目标 Android 构建退出零后冻结。红魔 bf1359ff 集中完成十二项 native、十组静态，七项程序/guest 身份执行前后相同，二十六份原始结果经 adb pull/SHA 核对。

- 主套件保持 748 例、229452 断言，相邻为 229341；十一例/六十六个失败断言、零 error。共同用例顺序及完整失败签名相同，成功断言数 +111 未逐例归因，不计作新增覆盖或性能收益。
- IR 快慢均为 92 例、78730/78828 断言，各新增两例/471 断言，全部通过。覆盖六种构建入口、快慢参数/HIR use 登记、定义/视图、空结果改写、非法 opcode；独立 oracle 比较完整 uniform 字节和未写 padding，分别在 Block/Function 上验证解释器/JIT 的截断、扩展、合法宽视图和布尔结果。未知 flags 与 live flags 的既有覆盖未删。
- fixed-class 2 例/39 断言通过；八项寿命/持久化标记相同，真实 target_spill_misses 仍为六次。

### 实际代码与工作区

十组 CRC/checksum、实际 Block/Function 模式和零 target_overflow 符合预期，spill_units/pair_fallbacks 未增。五组 bench 的完整有序记录和全部 shape 相同，机器码并非全部原样。CoreMark 的有序编译 PC 在原先固定的格式化符号外相同，新增/缺失 PC 均在该格式化范围；不是所有 IR/code 大小相同，也不是完整执行 trace 相同。

| CoreMark 配置 | 记录数，参考→本批 | RA host bytes，参考→本批 | spill_defs，参考→本批 | spill_loads，参考→本批 | spill_stores，参考→本批 |
| --- | --- | --- | --- | --- | --- |
| Function | 426→427 | 286624→286532 | 321→312 | 90→89 | 61→60 |
| Function，bound off | 427→427 | 286724→286540 | 321→312 | 90→89 | 61→60 |
| Block | 2414→2414 | 204972→204860 | 734→692 | 309→302 | 209→201 |
| fixed Function | 426→426 | 286296→285664 | 838→763 | 297→266 | 216→185 |
| fixed Block | 2413→2416 | 206016→205620 | 1590→1444 | 640→587 | 465→411 |

四组 Block dump 的长度/FNV/SHA 全部复核。十四组 CPUID 仅归一化已确认目标的四条 MOV immediate，保留相同 frontend 目标与原日志身份，load bias 仍为推断。另有 394 条 MOVZ W/X 差异仅在同位置、同寄存器、同 0/16-bit shift 和 immediate、完整 X 值相等时记录局部等价；不扩到 MOVN、任意 W/X 转换或寄存器移动。

归一化后普通/fixed Block CoreMark 仍有 43/49 处主体差异，两个 Block bench 各一处，全部 word 连同差异保留；本机 LLVM 仍有十六个未解码 word，以 .word 明示，未跳过。没有 Function 主体字节等价声明。bench 的 0x401430 保留 live ADD flags，另一路已知结果物化为 131068；matrix 的部分桥接/重载减少。增长点同样保留：0x21d303 和 __dtoa 的 0x25f528 各增加独立 W 零常量；scudo 除法块 0x2262c5 增加 spill backing、净增八字节。总体 spill 减少不表示每块都更好。sectionless bench 按实际 RX PT_LOAD 字节生成不执行的 ET_REL 审阅对象，与 CoreMark 分别绑定输入 SHA；初次 reader 错用 guest 上下文的记录保留并修正，未重跑执行。这是局部解释及完整差异留档，不是整程序形式等价证明。

十一组可见工作区（十组静态和 IR slow）的九类缓冲均闭合，total 与分项相符。静态组 allocation_calls 不变，allocated_bytes 降低 132–1240 bytes、peak_bytes 降低 52–1240 bytes；初始化/retention 逐项保存，普通 Block Core 的 zero_init 增加 464 bytes。IR slow 新增一笔/72 bytes、峰值增加 84 bytes，含新增覆盖的整组计数未逐例归因。不是全部私有堆/初始化成本。

### 布局与剩余成本

Android ELF 中 IRMeta 仍为 56、Inst 为 94、IrBuilder 为 520、HIRValue 为 36 bytes，metadata 表仍为 10864 bytes；新增默认类型利用原 padding，无逐值表或 SemanticFacts 新字段，其他已计量 owner 布局保持。

构造自身代码 68→100 bytes；最终有效路径在首次 ret 前无 SP 调整或调用，十六字节帧只位于非法 opcode 分支，冷报告自身为 124 bytes/64-byte frame。v2 的 288-byte/96-byte 正常路径已退役，不能把冷分支帧当作有效构造的固定开销，也不能由此宣称全部传递栈降低。

launcher .text 4461428→4540812（+79384 bytes）；未拆冷路径的类型修订中间版本为 4461860。符号对比显示内联/outlined 归属变化，如 Hoist 外层增大、FunctionSummary 构造不再独立存在，不能只按单函数归因。这个体积增长、动态元数据查询、传递栈和编译周期继续计入成本门。没有以 owner 不变或 spill 减少宣称整体提速，本批没有 PMU。

### 最终 RV64 与归档

r7-rv64-execution-20261024-v8 用同一 v4 源码的独立只读快照完成十四目标真实 RV64 构建，完整输入 2552 份、九份 ELF 相对 v6 改变。三十七项注册命令/properties 不变：十九项受影响用户态全部新执行通过；十三项用户态/五项 guest 内核仅因完整 ELF 字节相同、QEMU/sysroot/镜像与原命令仍适用，复用 v6 原成功，保留版本、日志、XML、内核/rootfs/固件身份。不是一次新三十七项 CTest，也没有本批新内核启动。独立 --expect-strong TSO 另完整新执行，220321 checks、八种编码、二十组 5000 轮 litmus、八组软件 CAS128 均通过；两个 compile-cost 与 emit 仅构建。v7/v8 自建容器结束后停止，未改其他任务容器。

源码摘要 528c9693193c98369a6299c32234a32122e224fb364bd5c38316181448a4889e，overlay 3e77965a8071e9447bc089dadec45b753b3aeba3a25c72197126d29d49456f61，launcher cc2ee0b9c7c807b3bbeb9f4fe104ec9772d5ae50fe6ea3d130f456eeff97aa4b。Android 结果归 /private/tmp/swiftvm-middle-results/r2-fixed-result-types-v4-*，RV64 归 /private/tmp/swiftvm-middle-results/r7-rv64-execution-20261024-v8-*，范围核验和文档快照分别列账。

继续通用 Value 入口、完整宽度/CFG 与其余 guest-state/helper/fault 契约，候选/事务/元数据私有成本、其余运行时/并发矩阵和正式 FEX 对照。本批没有关闭完整 R2、R7、成本门或 R0–R7 目标。

## 结果类型在构造前确定与无类型入口退役（2026-10-24）

本批为 `r2-construction-types-v3`，相邻参照为 `r2-fixed-result-types-v4`。目标是删除“写入参数时顺便给结果定型”的生命周期耦合，而不是再加一套事后补类型 pass。改动涉及既有五十二份源文件，没有新增源文件或逐值缓存；Block、单块 Function 和通用 Function 的分配路径继续独立。

### 单一声明与文件职责

既有 `ir.inc` 的一百九十一项声明新增结果来源字段，opcode 顺序、C++ 返回 token、逻辑参数 token 与基本 effects 的生成函数逐项保持。参数的物理槽位规则不变。

| 结果来源 | 数量 | 构造时的规则 |
| --- | ---: | --- |
| Void | 29 | 元数据声明无结果，如 StoreUniform、AdvancePC |
| Fixed | 23 | 从既有 BOOL/U32/U64 返回 token 取得固定结果类型 |
| Arg0 | 101 | 只读取逻辑参数零的直接 Value/use-site view、Imm、Uniform 或 Local；不搜索后续参数，不深入 Operand/Lambda/Params |
| Explicit | 35 | 访存、选择、转换等必须由调用方给出结果类型，不能拿地址、条件或转换输入的类型代替 |
| Call | 3 | CallLambda/CallLocation/CallDynamic 必须声明结果类型；允许显式 ValueType::VOID 表示不取返回值 |

| 文件 | 本批后的职责 |
| --- | --- |
| `ir/ir.inc`、`ir/ir_meta.h`、`ir/ir_types.h` | 唯一签名和结果来源声明；生成静态规则及运行时 IRMeta 的 result_type/result_policy，不另建 opcode schema |
| `ir/instr.h` | 共享纯结果来源规则；静态工厂在创建节点前解析类型，动态入口没有 hint 时才查询元数据。低层 SetInst 在参数改写前确定结果；已知的显式改写类型保持 |
| `ir/instr.cpp` | 类型已知的 Inst 构造、参数存储、使用计数与封口检查；SetArg/SetArgFresh 不再改变结果类型 |
| `ir/ir_meta.cpp` | 运行时表与非法 opcode/缺失类型的独立冷错误路径 |
| `ir/block.h`、`ir/hir_builder.h/.cpp` | 将最终类型直接交给构造函数，再登记参数、编号和 HIR；无结果调用的编号保留空行，不创建 HIRValue |
| `compile/builder/ir_builder.h`、`guest_state.cpp` | 类型明确后调用现有共享规则和 guest-state 逻辑；默认规则也可走 AppendTyped，Unknown flags 继续保守处理 |
| `frontend/x86/decoder*.cc` | 明确访存宽度、选择的数据宽度、转换目标容器和 helper 返回类型。POPA 使用其 U32 访问类型；SETcc 为 U8，其余选择使用相应数据容器；AVX 分半转换明确 V128 |
| `backend/riscv64/jit/function.cpp`、`wide.cpp` | 克隆、控制 token 和宽值临时节点在参数复制前取得已知类型 |

默认模板参数改用 C++ `void` 表示“没有提供 hint”；`TypedValue<ValueType::VOID>` 和 `ValueType::VOID` 表示真实无结果，不能再触发推导。Block、三种 HIR 工厂与 IrBuilder 的具名 Explicit/Call 入口要求显式 hint；动态 AppendInst 缺失类型时在分配、链接和 HIR 编号前拒绝。仍保留低层显式覆盖和改写契约，IrBuilder 的具名工厂校验固定返回 token；这不等于完整的参数/SSA/宽度规则已经验收。

八处 setter 推导分支、未使用的 InstAllocator、两种 Inst::Create 及无类型 CallHost 家族包装已删除。二十二个 setter 函数体均不再访问 ret_type，使用计数、fresh-slot 检查与封口约束保留。两个实际返回 void 的时间戳/随机数 frontend helper 显式声明 VOID；调用的 effects、参数使用、执行和内存写入继续存在。

### 集中验证与边界

v1/v2 仅作编译诊断，分别修正公共 token 名称遮蔽/宏返回表达式，以及遗漏的内部/测试调用；失败输入和日志保留，没有冻结成成功产物，也没有执行测试。v3 完成整批构建后才冻结和执行，未增加仓库 probe 或一次性测试脚本。

| 验证 | 本批结果 |
| --- | --- |
| Android 构建 | NDK 29-beta3、Clang 21、API 28、Release，沿用共同 fmt 设置；十四目标全部成功，十四份冻结 ELF 均相对参照变化 |
| 红魔实际执行 | NX729J `bf1359ff`；十二组原生验证、十组负载/编译形状采样均新执行，设备七项产物身份前后核对，二十六份原始文件拉回并校验 SHA |
| 快/慢 IR 套件 | 各九十四用例、79090/79188 断言全部通过；比参照各新增两个用例、360 断言 |
| 新覆盖 | 六种工厂的直接参数/use-site view 类型、HIR 登记与连续编号；缺失 hint 不追加节点；setter 不改结果；Block/Function 的解释器和 JIT 对窄加法、64 位选择、显式 U16 访存及忽略 helper 返回值的独立全缓冲 oracle |
| 主套件 | 748 用例、229148 断言、0 errors；既有 11 失败用例/66 失败断言的名称、内容及共同用例顺序均与参照一致。成功断言总量减少 304，未逐例归因，不把总量变化当作新增覆盖；没有删除用例 |
| 其他原生覆盖 | fixed-class 两例/39 断言通过；八组生命周期/持久化成功标记保持，实际 spilled target miss 仍为六次 |
| RV64 | `r7-rv64-execution-20261024-v9` 在同一源码的只读完整快照上构建十四目标；十九项受影响用户态新执行通过，另十三项用户态/五项内核以相同完整 ELF、命令、环境复用既有证据。三十七项是 RV64 专用注册范围，不是仓库全部 CTest |
| RV64 强 fence | 独立完整新执行，220321 checks；八种编码、二十组各 5000 轮 litmus、八组软件 CAS128 均通过。两个 compile-cost 程序和 emit 仅构建；本批没有新内核启动或物理 RV64 证明 |

RV64 原始复用证据通过 v8 保留到其实际来源，五项内核仍对应 v6 的 Linux/OpenSBI/rootfs/QEMU 执行。v9 完成后确认容器空闲再停止，其他任务容器未改动；不是一次新执行三十七项 CTest。

### 编译形状、存储与代码成本

十组 CRC/bench 校验值、实际 Block/Function 模式与零 target overflow 均成立，已采样的 spill 指标没有增加。五组 bench 的完整编译记录和形状保持；两组 Block bench 的原始 host dump 逐字节相同。CoreMark 的记录变化和代码变化分别留存，不能由 checksum 相同推导整程序等价。

| CoreMark 模式 | 编译记录数，参照→本批 | RA host bytes，参照→本批 | spill_defs，参照→本批 |
| --- | --- | --- | --- |
| Function | 427→427 | 286532→286528 | 312→311 |
| bound-off Function | 427→427 | 286540→286540 | 312→312 |
| Block | 2414→2412 | 204860→204384 | 692→691 |
| fixed Function | 426→426 | 285664→285660 | 763→762 |
| fixed Block | 2416→2416 | 205620→205328 | 1444→1443 |

其余 spill units/loads/stores/pair fallbacks 保持。排除既有 formatter 符号范围后，十组编译记录的 PC 顺序相同；这是编译记录，不能称为 guest 执行轨迹。两个 Block CoreMark dump 各保留一处共同 PC 主体差异：`__cpu_indicator_init` 的 `0x269b3e` 以直接 reload 替代 reload 加 `lsr w..., #0` 搬运；后续 W 宽使用和覆盖连同完整 diff 保留。未匹配 PC 都在 __d2b_D2A/__dtoa；不是把任意差异都归于 formatter。十四组 CPUID 仍只归一化经既有目标身份及完整 DecodeCpuid 函数文本相同所支持的四条 MOV immediate，load bias 仍是推断；其余函数变化不作“整个 frontend 未变”的声明。本批没有额外 MOVZ W/X 归一化或未解码 word，也不声明 Function 主体字节等价。

十一组可见 workspace 的九类缓冲均闭合、分项与 total 相符。Function/fixed Function Core 的 allocated/peak bytes 各减少 7916/7824；Block/fixed Block Core 各减少一笔申请，allocated/peak bytes 减少 5024/4224。bench 和 bound-off 另逐项留账。IR slow 因整组覆盖变化增加一笔/64 bytes 和 256 bytes pattern initialization，峰值保持；没有逐例归因，也不把可见 workspace 当作全部私有堆成本。

二十一项已计量 owner 布局保持，IRMeta/Inst/IrBuilder/HIRValue 仍为 56/94/520/36 bytes，metadata 表仍为 10864 bytes。类型已知的 Inst 构造为 72 bytes（参照默认构造 100），合法路径在首次 ret 前无 SP 调整、调用或元数据表加载；所记录的十六字节帧仍只在非法 opcode 分支。普通 SetArg(Value) 自身代码/静态 SP 减量总和 376/128→148/80；fresh 为 464/128→284/128。FoldExtend 外层代码 1192→1392、静态 SP 减量总和 240→224，原独立 lambda 不再单列，不能由单符号增减推导传递峰值。

launcher .text 为 4540812→4532188，减少 8624 bytes；相对前一轮 helper-state 参照仍累计增加 70760 bytes，该增长不能因本批下降而抹去。没有本批 PMU、动态编译周期或完整私有堆/传递栈验收。

### 证据与继续工作

源码摘要 `db2a837c814eedfcf1d26ac6466fde1ba21511d9ab3e9411ef435ee69eede760`，overlay SHA `c88c2cd5c58fcfbe1eeeeab2d39b377560d0478495b191636d7db8282e462b1b`；launcher SHA `1e98f2c027bcd9232e5f827b96a516067b5d49cd729111694b5b21931b988506`。Android/RV64 构建日志 SHA 分别为 `405354e2723f3af7f923366698f7f2b9186701a3294cd73be1ad2325bc4ec188`、`986adbb8728a71f85b7417442c955bf0ba091e77664b14a4b66f2bef9897ccd4`。

本批原始数据与解析结果归 `/private/tmp/swiftvm-middle-results/r2-construction-types-v3-*`；源码相邻差异 patch SHA 为 `0b11a7a4b151cdf9cf7cad2c28c1193a6c644b50fc854bec547ad860cf52d99a`。RV64 完整 2552 份输入摘要为 `f08f3bc69c3800d4d966497020aa51cfbe90ef8e450bdc3df880f791260a25cf`，证据归 `r7-rv64-execution-20261024-v9-*`。`-construction-contract.json` 核对签名/effects、setter 和编译 PC 顺序；`-quality.json` 的二十项范围检查通过，并显式保持 full_goal/完整 R2/R7/成本/FEX gate 为 false。

继续完整类型/宽度/CFG 契约、有序候选及私有存储成本、guest-state/helper/fault/观察边界、完整运行时与并发矩阵，以及固定版本 FEX 的正式 Block/Function 对照。本批只关闭参数 setter 的隐式定型和缺省 VOID sentinel 接口，不关闭完整 R0–R7。

## 类型读取视图与封口检查合流（2026-10-24）

本批为 r2-type-view-admission-v9，相邻参照为 r2-construction-types-v3；十六个既有源文件修改，没有新增或退役源文件。冻结 overlay 仍为 472 项、累计删除 113 项；overlay 不是完整仓库。本批补齐基础类型和数据可用性，并清除该检查中的临时值快照，不代表完整 R2/R7 或成本验收。

### 实现与边界

- Value 的规范定义和本次读取视图都必须具有有效的非 VOID 类型；合法 cast 不能掩盖无结果调用、VOID 或越界定义。真正的数据读取另外要求定义具有数据结果，排除带有遗留正类型的非值节点和分支 token。
- Inst 的结果检查只负责基本存在性和枚举范围，低层显式结果覆盖继续独立于默认构造规则。RV64 Function lowering 通过目标表绑定的 VOID Goto/NotGoto 可以作为控制节点存在，不能被读成数据；有类型 BindLabel 引用继续由绑定关系验证。
- CheckTypeViews 与 ForEachValue 共用完整嵌套参数枚举，在物理槽、Lambda 和全部 Params 上逐 occurrence 处理，拒绝非法 Imm/Uniform/Local 类型和不等宽的 StoreUniform。签名/类型视图本身不证明所有 opcode 宽度、Phi、CFG 或转换语义。
- SSAValidation::ObserveInstruction 在已有封口 facts 访问中同时检查类型、记录原始使用和收集 use-end；私有 Use 不再重复数据/分支元数据判断。terminal 与提升锚点经 UseImplicit 检查。该阶段不增加第二遍完整参数扫描，不新增持久字段或逐值缓存。
- ARM64 优化前检查普通参数和隐式根，必要封口证书仍在物理分配前生效；RV64 Prepare 与直接发射预检共享基础规则，保留各自 ABI、Phi/local、配置 span 和绑定契约。优化前与优化后的检查处于不同生命周期，不能因共享实现而互相代替。Block、合格单块 Function 和通用 Function 的分配策略未合并。

### 集中验证与失败修正

| 验证 | 本批结果 |
| --- | --- |
| Android 构建与设备 | NDK 29-beta3、Clang 21、API 28、Release，共同 fmt 设置；十四目标成功。红魔 NX729J bf1359ff 新执行十二项原生验证和十组工作负载，七项设备产物身份前后核对，二十六份原始文件传回并校验 SHA |
| 快/慢 IR | 各 96 用例、79416/79514 断言全部通过；相邻各新增两个用例、326 断言。覆盖 65 条嵌套读取 occurrence、规范定义/读取视图、控制与数据角色、Block/实际完成的 HIRFunction、terminal 和锚点 |
| 主套件 | 748 用例、229179 断言、0 errors；既有 11 失败用例/66 失败断言的名称、内容和共同用例顺序与参照一致，没有增删用例。总断言相邻增加 31，未逐例归因，不用总量差值代替覆盖证明 |
| 其他原生覆盖 | fixed-class 两例/39 断言通过；八组生命周期/持久化成功标记与参照相同，实际 spilled target miss 六次 |
| RV64 实际执行 | v11 在生产修正后的 v8 快照构建十四目标，十九项受影响用户态新执行通过，十三项用户态/五项 guest 内核按完整相同 ELF、命令、环境和原记录复用。独立强 fence 新执行 220321 checks，包含八种编码、二十组各 5000 轮 litmus、八组软件 CAS128 |
| RV64 最终源码闭合 | v12 对最终 v9 完整快照构建十四目标成功；相对 v11 只有 ARM64 IR 测试夹具变动，十四份完整 ELF 全相同。因此 v12 没有再执行测试，复用全部三十七项成功记录和强 fence 原记录，保留实际来源命令、时间、日志及内核证据链；不是新跑三十七项 CTest |

RV64 三十七项仍只指 RV64 专用注册范围；两个 compile-cost 程序和 emit 仅构建。v10 的十九项新执行曾有八项失败：七项由 VOID lowered branch 被误判为缺失数据结果引起，一项由统一 StoreUniform 宽度检查先于后端 span 诊断引起。修正的是控制节点结果角色和具体诊断预期，没有放宽普通数据使用或 store 宽度检查。v11/v12 均确认只剩 sleep 后停止自身容器，其他任务容器不变。

v1/v2 为编译诊断；v3/v4 的 null If 夹具在构造时就非法，先前将其归因于优化器的判断已撤回。v5 夹具未完成 HIR，随后正常结束并先计算 RPO/编号；v6 快/慢 IR 通过，主套件三处 BOOL 包装视图与声明写宽不一致。夹具现按已有 U32/U64 结果契约显式读取，输出槽加入非零哨兵，CRC 高半部还须保留哨兵。v7 的两项 IR 仅在与 v6 完整 ELF 相同后复用。v8 新加的临时控制节点用例还曾因外层 reader 持有已结束作用域的定义而中止；v9 将 reader 放入相同作用域，十二项 Android 验证全部新执行。失败记录、原始退出码和已纠正的诊断均保留在 attempts 账本中，不归为成功执行。

### 生成代码、存储与成本

十组 guest CRC/bench 校验值、实际 Block/Function 模式和零 target overflow 均成立；五项 spill 指标均与相邻参照相同。五组 bench 完整编译记录/形状保持；CoreMark 的实际变化如下：

| CoreMark 模式 | 编译记录数，参照→本批 | RA host bytes，参照→本批 | spill_defs |
| --- | --- | --- | --- |
| Function | 427→427 | 286528→286528 | 311→311 |
| bound-off Function | 427→427 | 286540→286540 | 312→312 |
| Block | 2412→2417 | 204384→204776 | 691→691 |
| fixed Function | 426→427 | 285660→285752 | 762→762 |
| fixed Block | 2416→2413 | 205328→205040 | 1443→1443 |

差异均在既有 guest formatter 符号范围；范围外完整有序编译记录一致。这不是运行轨迹。四份 Block host dump 的长度、FNV 与 SHA 重新核对，两个 bench dump 原始有序字节相同；CoreMark 各七处 CPUID 只按原始同日志目标证明、完整 DecodeCpuid 文本及原始选择源码 SHA 归一化四条 MOV immediate。其余共同 PC 主体字节相同，本批没有 MOVZ W/X 归一化或未解码 word；未匹配的 formatter PC 仍单列。load bias 是推断，不声明完整程序或 Function 主体字节等价。

十一组可见 workspace 的九类缓冲均闭合、分项与 total 相符；申请/释放次数、字节、live 和 peak 六项都与相邻保持。三组 CoreMark 的 pattern initialization/retention events，以及 IR slow 的 retention events 有变化，逐缓冲留账；这些统计不覆盖全部私有堆。

二十一项已计量 owner 布局保持，IRMeta/Inst/IrBuilder/HIRValue 为 56/94/520/36 bytes，SemanticFacts/CompileWorkspace 为 624/224 bytes，metadata 表 10864 bytes。launcher .text 为 4532188→4537068，增加 4880 bytes。下面是实际 ELF 的自身函数代码字节/静态 SP 减量总和，不是传递栈峰值：

| 函数 | 参照→本批，code/SP bytes |
| --- | --- |
| Inst::HasValidTypeViews | 588/160→1272/144 |
| Inst::HasValidResultType | 新增 108/0 |
| SSAValidation::Use | 360/64→496/112 |
| SSAValidation::Define | 704/112→432/64 |
| SSAValidation::UseImplicit | 新增 128/16 |
| SemanticFacts::PrepareBaseFacts | 2224/400→2224/400 |
| ARM64 SupportsIR(Block) | 212/80→472/144 |
| ARM64 SupportsIR(Function) | 316/112→116/32 |

内联、外提和调用边界会移动工作，不能把局部下降加总成总体收益。最终 launcher 与 v8 的完整 ELF 相同，cost 复用保留其原始 nm、DWARF、size 与反汇编文件。本批没有 PMU、动态编译周期、全部私有容量/初始化或传递栈验收。

### 证据与接续

最终源码摘要 0f20d78b28449afa75f3600f7fb8231514c281f39ba562de4ce961e945130d1e，overlay SHA 3db414b5a2940fdff50923e9b701c0f0b855dd25415ffbd895730347541827d0；launcher SHA 659870bc342f5b7b273654eef5577c642962b14e49341a3d2d76d4a3dd26217d。Android/RV64 最终构建日志 SHA 分别为 3c1dd6c6e5a9115b46f09cd33ee888ffa96e3c279e03d96d1c0c7bba50bcc3cc、8c35b1682f73592d30612d7b0fabdb384fcfff35cb5a076cb48b85b1368aa794。源码相邻差异 patch SHA 为 80e52fed6846a5531d517abb56a262150412990396da408d6b66ec22c13b085d。

证据归 /private/tmp/swiftvm-middle-results/r2-type-view-admission-v9-* 及 r7-rv64-execution-20261024-v12-*，实际新 RV64 执行归 v11；type-view-contract、test/shape/storage-summary、host-byte-comparison、cost、attempts 与 execution-reuse 分别保留实现边界、原始结果及来源。二十项限定质量检查通过，完整 R0–R7、R2、R7、动态成本与正式 FEX 门继续为未通过。未增加仓库 probe、临时开关、一次性测试脚本或源文件。下一批继续完整参数/宽度/CFG 契约、有序候选与 guest-state/fault/观察接缝，并对已记录的代码和自身帧增长接续成本工作。

## fault sink 的原始顺序遍历与稀疏位置（2026-10-24）

本批 r2-state-sink-sparse-positions-v2 以 r2-type-view-admission-v9 为相邻参照，只修改 state_sink.cpp、state_dse.cpp 和既有 ir_contract_test.cpp；没有新增或退役源文件，overlay 仍为 472 项、累计删除 113 项。本批清除重复存储与只读快照，保留状态发表和故障观察的原有语义；完整 R2/R7 与动态成本尚未验收。

### 清理结果与约束

- sink 的整块 original 指针 vector 已退役。主循环先保存后继，再处理当前节点；flush 只移动/删除先前摘下的 store，不删除观察点或未来游标，重插入的 store 不重复进入主循环。
- 原来给全部 N 条指令建立的位置哈希，改为仅插入配方请求的唯一 producer/boundary，条目数至多为配方数的两倍。存在请求时仍有一次原始位置准备遍历；没有请求则没有位置条目和该遍历。没有借此取消必要的原始状态访问，稀疏哈希也没有全部退役。
- 当前源位置通过原始访问序号推进，不依赖 Inst ID 或指针排序。未在本块找到的请求保留 missing 状态，不能把未知边界当作无限远位置。精确 boundary 与严格 producer < current < boundary 的提前发表条件分别保留。
- DSE 与 sink 的四处只读 GetValues 快照改用完整 ForEachValue，保留参数顺序、重复 occurrence、嵌套 Lambda/Params、原始工作队列和 64 次访问上限。两种 survivor 策略没有合并；RewriteContext 改写/删除通知及 value worklist 的可变快照未改。
- 没有新增持久字段、逐值缓存、公共 API 或迁移开关。pending/published、selected/victim/covered、remaining 搬运、必要 consumer 图及完整私有存储仍待接续；无配方不等于整个 sink 零分配。

### 一次集中验证

v1 仅有编译诊断：新夹具误用了私有 Block::EditInst，v2 改用公共 RewriteContext，未放宽生产写接口。v1 没有执行原生测试或工作负载。v2 完成全组构建和冻结后集中执行，未增加仓库 probe 或一次性测试脚本。

| 验证 | 本批结果 |
| --- | --- |
| Android 构建/设备 | NDK 29-beta3、Clang 21、API 28、Release，共同 fmt 设置；十四目标成功。红魔 NX729J bf1359ff 上十二项原生验证、十组工作负载全部新执行，七项设备身份前后核对，二十六份原始文件校验传回 |
| 快/慢 IR | 各 97 用例、80622/80720 断言全部通过；相邻各新增一个用例、1206 断言。十二组独立 store/capture 序列覆盖普通/稀疏 ID Block、实际 HIRFunction、producer 前后新增观察点、无配方和缺失/外块边界；核对非 store 完整顺序、carrier 身份、发表值及 store 数量 |
| 主套件 | 748 用例、229512 断言、0 errors；既有 11 失败用例/66 失败断言的内容和共同用例顺序与参照相同，没有增删主套件用例。总断言相邻增加 333，未逐例归因，不作为新增覆盖证明 |
| 其他原生覆盖 | fixed-class 两例/39 断言通过，八组生命周期/持久化成功标记保持，实际 spilled target miss 六次 |
| RV64 | v14 在最终源码完整快照构建十四目标，四份完整 ELF 相对 v12 变化；据实际产物选择十七项受影响用户态新执行，全部通过。十五项用户态/五项 guest 内核按完整相同 ELF、命令、环境和原记录复用 |
| RV64 强 fence | 完整 ELF 和环境未变，复用实际 v11 的完整 220321-check 执行及原时间/命令/log SHA，没有重复执行；仍包含八种编码、二十组各 5000 轮 litmus 和八组软件 CAS128 |

v13 只构建生产改动，没有执行测试；最终 v14 覆盖修正后的 ARM64 夹具源码，产物与 v13 相同。三十七项是 RV64 专用注册范围，不是一次新三十七项 CTest；两个 compile-cost 程序和 emit 仍仅构建。五项内核继续沿原证据链指向实际执行来源，本批没有新内核启动或物理 RV64 证明。v13/v14 自身容器在确认空闲后停止，其他任务容器未动。

### 代码形状、存储与成本

十组 guest 校验值、实际 Block/Function 模式和零 target overflow 成立，五项 spill 指标全部与相邻相同。Function、bound-off Function、fixed Function 及五组 bench 的完整编译记录/形状保持；Block CoreMark 记录数 2417→2415、RA host bytes 204776→204924，fixed Block 为 2413→2415、205040→205344。差异都在既有 formatter 符号范围，范围外完整有序编译记录一致，不称为运行轨迹。

四份 Block host dump 的原始长度、FNV/SHA 重新核对。两个 bench 的有序原始字节相同；两个 CoreMark 各七处 CPUID 只按既有同日志 helper 目标证明、完整 DecodeCpuid 文本和原始选择源码 SHA 归一化四条 MOV immediate，其余共同 PC 主体字节相同。没有 MOVZ W/X 归一化或未解码 word，未匹配 formatter PC 单列，load bias 仍为推断；没有 Function 或完整程序字节等价声明。

十一组可见 workspace 的九类缓冲均闭合、分项与 total 相符，申请/释放次数、字节、live 和 peak 六项均相同；只有两组 Block CoreMark 的 pattern initialization/retention events 变化，IR slow 全字段保持。这些计数不包含退役的私有 original vector、稀疏哈希、pending/flush 等全部堆请求，不能据此推导总堆或延迟下降。

二十一项已计量 owner 布局保持，metadata 表 10864 bytes；Inst/IrBuilder/SemanticFacts/CompileWorkspace 仍为 94/520/624/224 bytes。launcher .text 为 4537068→4538844，增加 1776 bytes；相对 construction-types-v3 仍累计增加 6656 bytes，不能用本地函数下降抹去总增长。实际 ELF 的自身代码/静态 SP 减量总和如下：

| 函数 | 参照→本批，code/SP bytes |
| --- | --- |
| SinkBlockStores | 4400/560→4072/560 |
| sink flush lambda | 4036/512→4268/432 |
| sink HasObservableUse | 812/384→1272/304 |
| DSE ChainWouldStrandASurvivor | 616/368→836/304 |
| DSE consumer 构建 lambda | 724/256→996/192 |
| EliminateStateDeadStores 外层 | 8128/1392→8128/1392 |

部分自身帧下降伴随流式枚举代码增长；内联与外提会移动成本，这些值不是动态/传递栈峰值。本批没有 PMU、动态编译周期或完整私有容量/初始化验收。

### 证据与接续

源码摘要 1caff0be031b3dcd6a619f5a5ebd7a2e1c5203ae8ee674121bdd756e6a36709b，overlay SHA 0ea129e56eac4bd1e7d768cd6be19b04694dcb67501ede04870a48e5ab0036a2；launcher SHA 668433242b32b4c7bbbf854b06edb5ef10062b1b192e256424e4b9061e448251。Android/RV64 最终构建日志 SHA 为 3c1dd6c6e5a9115b46f09cd33ee888ffa96e3c279e03d96d1c0c7bba50bcc3cc、8c35b1682f73592d30612d7b0fabdb384fcfff35cb5a076cb48b85b1368aa794；相邻源码 patch SHA 为 24343f91756272913a3143fc990725bda22dd47301dfb902e07f742d44efeb9d。

证据归 /private/tmp/swiftvm-middle-results/r2-state-sink-sparse-positions-v2-* 和 r7-rv64-execution-20261024-v14-*；state-sink-contract、test/shape/storage-summary、host-byte-comparison、cost、attempts 及 RV64 user-plan/tso-reuse 保留各自来源。二十项限定质量检查通过，完整 R0–R7、R2、R7、动态成本及正式 FEX 门仍未通过。接续 pending/flush 缓冲、remaining 搬运和观察/故障接缝的清理与计量，同时保留完整参数/宽度/CFG、有序候选和其他未完成阶段。

## fault sink 的 flush 缓冲收敛（2026-10-24）

本批 r2-state-flush-storage-v1 以 r2-state-sink-sparse-positions-v2 为相邻参照，修改既有 state_sink.cpp、ir_contract_test.cpp 和 hir_lifetime_test.cpp；没有新增或退役源文件，overlay 仍为 472 项、累计删除 113 项。成组修改完成后首次构建及一次集中执行均通过预期范围；完整 R0–R7、R2/R7 与动态成本仍开放。

### 实现与边界

- selected/victim 两份 vector<bool> 已合为 PendingStore 的 Pending/Publish/Erase 三态；原有元素布局仍为 32 bytes，没有增加持久 IR/facts 字段。每次选择先重置全部记录状态，部分范围 flush 不携带上次的选择结果。
- 独立 remaining vector、reserve 和 move 移交已退役。全部 carrier 查询、发表记录、重连和 SetArg 完成后，才按源顺序稳定压缩未选中的记录、删除已证明的 victim；写入只覆盖已经访问的行，保留 pending 容量，不使 carrier 指针跨压缩存活。
- coverage 改为该块 sink 内延迟准备、每次 assign 清零并复用，仍保留必要的完整覆盖检查。pending 在首个实际 XMM store 才 reserve，published 表和更新只在存在 capture 配方时准备；无配方不维护无人读取的 committed-value 字节。
- survivor、别名宽度、提前/精确边界和先重新挂入 Block 再删除/改写的约束保持；只读 helper、DSE、可变 use/DCE 快照与 value worklist 未改。三态仅为临时写记录，不是新的发射 actions。
- pending/coverage 容量保留至本次 SinkBlockStores 结束，没有新增跨块 workspace 借用。配方路径仍有完整 uniform span 的 published 表、稀疏位置/consumer 查询；容量保留时间、完整初始化和异常退出矩阵仍待验收。

### 集中验证

| 验证 | 本批结果 |
| --- | --- |
| Android/设备 | NDK 29-beta3、Clang 21、API 28、Release，共同 fmt 设置；十四目标首次构建成功。红魔 NX729J bf1359ff 上十二项原生验证及十组负载全部新执行；七项产物身份前后核对、二十六份原始文件校验传回 |
| 快/慢 IR | 各 98 用例、80661/80759 断言全通过；相邻各新增一个用例、39 断言。独立 256-byte oracle 覆盖 Block/Function、三种非零/零填充、sink 前/后解释器及实际 JIT；包含超过初始 pending 容量、部分范围稳定保留、无命中选择、coverage 重置、多个窄写共同覆盖、精确转发、空 flush 和 terminal-only store |
| 分配请求回归 | 扩展已有独立寿命测试的 C++ new 钩子，不在生产加入 probe。无配方、uniform span 64：无 store 而含 barrier 时请求数/字节为 0；峰值 pending 1 时，8 与 64 次 flush 的正请求数及请求字节相同，原始 8/64 个必要发表均保留。fast1/fast0 的 constant-window 模式各按预先声明增加两项 checks，123477→123479，其他成功标记保持 |
| 主套件 | 748 用例、229305 断言、0 errors；既有 11 失败用例/66 失败断言的内容与共同用例顺序和参照一致，没有增删主套件用例。总断言减少 207，未逐例归因，不当作覆盖增减证明 |
| 其他覆盖 | fixed-class 两例/39 断言通过；八组生命周期/持久化标记均符合原值或上述预定增量，实际 spilled target miss 六次 |
| RV64 | v15 在最终完整源码快照构建十四目标，四 ELF 相对 v14 变化；十七项受影响用户态新执行全通过，十五项用户态/五项 guest 内核按完整相同 ELF、原命令、环境及原记录复用。完整强 fence ELF/环境未变，复用实际 v11 的 220321-check 执行及原始时间、命令和 log SHA |

RV64 三十七项仍只是专用注册范围，不是新跑全部三十七项 CTest；两个 compile-cost 程序和 emit 仅构建。TSO 包含原来的八种编码、二十组各 5000 轮 litmus、八组软件 CAS128；本批无新内核启动、物理 RV64 或 PMU 证明。自身 v15 容器确认空闲后停止，其他任务容器未动。C++ 分配比较只覆盖该夹具和 new 请求，不代表所有 malloc/arena 或整个 sink/编译器成本。

### 代码与存储成本

十组 guest 校验值、实际模式和零 target overflow 成立，五项 spill 指标全与相邻相同。五组 bench 完整编译记录/形状保持；CoreMark 范围外的有序编译记录保持，变化均在既有 formatter 符号范围。bound-off/fixed Function 记录各 427→426、RA host bytes 为 286540→286448、285752→285660；Block 为 2415→2417、204924→204776，fixed Block 为 2415→2413、205344→205040。不能把这些编译记录当作运行轨迹。

四份 Block host dump 的长度/FNV/SHA 重新核对，两个 bench 的有序原始字节相同。两组 CoreMark 各七处 CPUID 仍只在原始同日志 helper 目标、完整 DecodeCpuid 文本及选择源码 SHA 支持下归一化四条 MOV immediate，其余共同 PC 主体相同。没有 MOVZ W/X 归一化或未解码 word，未匹配 formatter PC 单列；load bias 为推断，不声明 Function/完整程序字节等价。

十一组可见 workspace 的九类缓冲闭合、分项与 total 相符。十组负载的申请/释放次数、字节、live 和 peak 六项保持，formatter 相关的 pattern initialization/retention events 变化单列。IR slow 整组申请/释放 382→389，allocated/released bytes 6678660→6679676，增加 7 次和 1016 bytes，峰值仍为 3981816；分缓冲 peak-retained/retention 变化保留，未逐例归因。这些工作区计数不包含全部私有 pending/coverage/published 请求。

二十一项 owner 布局及 10864-byte metadata 表保持；PendingStore/CaptureByte 的实际 ELF 布局均为 32→32 bytes。launcher .text 为 4538844→4537772，减少 1072 bytes，相对 construction-types-v3 仍累计增加 5584 bytes，不能抹去此前增长。SinkBlockStores 自身 code/SP 为 4072/560→4232/592，flush lambda 为 4268/432→3552/368：外层自身帧增 32 bytes，flush 自身帧减 64 bytes。内联/外提与调用关系会移动成本，这些值不是动态或传递栈峰值；完整私有堆、保留时间、初始化、编译周期和正式性能门未通过。

### 证据与继续工作

源码摘要 51127afbc3a8a2c24ca39b6fbfbd76405b5d9f2cb0d1619dbb832de1647e155d，overlay SHA a68be58165c437f06339451bfcac9f268481b2280b886a64d855496fa2925dbf；launcher SHA 7849e41492f3ae7dc944398ea0828d75873f1d4110b533306d4c2d5d7e5f8195。Android/RV64 构建日志 SHA 为 c566841071e34fcb70a96c473c947a000c93392b142ac4b44e3d16d904b330ea、a9932e3d3ed60a4afd64d63ea3fb0663f18a815afb8be1924795a61f0d2bce1d；相邻源码 patch SHA 为 a530faf69f2eb94093ba8888715a07875036ba3759100647dd58ad87433c7e96。证据归 /private/tmp/swiftvm-middle-results/r2-state-flush-storage-v1-* 与 r7-rv64-execution-20261024-v15-*，flush-contract、test/shape/storage-summary、cost、host-byte-comparison 和 user-plan/tso-reuse 保留实际来源。

二十项限定质量检查通过；没有新增源文件、临时开关、生产 probe 或一次性测试脚本。下一批检查 detached store 在异常退出时的持有与回收，接续 capture/published 有效载荷、consumer 查询及完整私有成本；完整类型/宽度/CFG、有序候选、运行时矩阵和正式 FEX 对照仍在原目标范围内。

## fault sink 脱链节点的异常回收（2026-10-24）

本批 r2-state-sink-unwind-v1 以 r2-state-flush-storage-v1 为相邻参照，修改既有 state_sink.cpp、hir_lifetime_test.cpp 和 tests/CMakeLists.txt；没有新增或退役源文件，overlay 仍为 472 项、累计删除 113 项。三处修改合并后一次构建、一次集中设备执行；本批限定质量检查通过，完整 R0–R7、R2/R7、动态成本及正式 FEX 门仍开放。

### 归属修复与失败输入契约

Block 的普通销毁只回收其实际链上指令，不能依靠 Inst 池自动找回脱链节点。RewriteContext::Erase 也可能已经释放 victim，再因删除通知的队列扩容抛出 bad_alloc；遍历所有旧指针并查询 owner 会访问已释放对象。

- PendingStore 新增 detached 持有标记，使用原有布局填充，实际 ELF 仍为 32 bytes。先成功加入 pending，再摘链并标记，扩容失败的当前节点继续由 Block 持有。
- 成功重连后立即解除持有，再进入 SetArg 等通知；就地压缩复制未选中行时同步转移持有，旧槽立即解除，避免之后删除通知失败时重复挂入。
- 捕获异常后只处理仍持有的记录，使用不分配的 intrusive 插入交回原 Block；空链与非空链分别处理，清除 capture recipes 后重抛。已重连/已删除的记录不再解引用，没有新增独立 owning 列表或全 IR 回放。
- 失败输入必须销毁，不能重试优化或发射。此清理保证普通 owner 可回收节点，不恢复源顺序、已删除指令或优化前的语义；不将它声明为事务回滚。

### 集中验证

| 验证 | 本批结果 |
| --- | --- |
| Android/红魔 | NDK 29-beta3、Clang 21、API 28、Release，共同 fmt 设置；十四目标首次构建成功。NX729J bf1359ff 上十四项原生验证与十组负载全部新执行，五项使用产物及两份 guest 身份前后核对，二十八份原始文件校验传回 |
| 永久失败回归 | 在既有 hir_lifetime_test.cpp 中增加 state-sink-failure，并注册 fast 0/1。五类输入为独立 Block/实际 Function 的有配方和无配方，以及第二块仅含外块定义 store 的双块 Function；逐个实际 C++ new 分配点注入，直到首个成功执行。两种模式各 41 次注入，清理不再申请 C++ new、节点 Block 归属、raw use-count/HIR use-list 与实际参数枚举一致，销毁后池回收通过；另有断言要求真实经过 victim 已删除和 pending 持有已转移后的失败 |
| 快/慢 IR | 各 98 用例、80661/80759 断言全通过；相邻没有增删 IR 用例或改变断言数，既有独立字节观察及解释器/JIT oracle 继续通过 |
| 主套件 | 748 用例、229430 断言、0 errors；既有 11 个失败用例/66 个失败断言的归一化内容和共同用例顺序与参照一致，无新增/退役主套件用例。总断言相对 229305 增加 125，未逐例归因，不当作覆盖增加证明 |
| 其他原生覆盖 | 原有八份生命周期/持久化成功标记原样一致，包括两种 constant-window 的 123479 checks；新增两份故障模式成功标记。fixed-class 两例/39 断言通过，实际 spilled target miss 六次 |
| RV64 v16 | 最终完整源码快照 2552 文件构建十四目标，四 ELF 相对 v15 变化；十七项受影响用户态 CTest 新执行全通过，十五项用户态/五项 guest 内核按完整相同 ELF、原注册命令/属性、环境与原记录复用。完整强 fence ELF 和环境不变，仍引用实际 v11 的 220321-check 执行及原始命令、时间和 log SHA |

故障钩子仅位于既有独立测试程序，没有生产 probe、临时开关或新增一次性测试脚本。五类夹具内每个实际 C++ new 分配点的扫查，不证明全部 carrier SetArg、活跃 value epoch、其他 pass、所有 malloc/arena、异步调用方或完整运行时失败矩阵；成功标记中的 checks 包括重复池回收检查，不按其数量扩张覆盖声明。RV64 三十七项只是专用注册范围；本批没有新内核启动、物理 RV64 或 PMU，两个 compile-cost 程序和 emit 仅构建。自身 v16 容器确认空闲后停止，其他任务容器未动。

### 代码、存储与实际负载

十组 guest 校验值、实际模式和零 target overflow 成立，五项 spill 指标全部与相邻相同。五组 bench 的完整有序编译记录及形状保持；CoreMark 范围外的有序记录保持，差异位于已有 formatter 符号范围。bound-off/fixed Function 记录各 426→427、RA host bytes 为 286448→286540、285660→285752；普通 Block 为 2417→2416、204776→205140，fixed Block 为 2413→2413、205040→205040。这些是编译记录，不是运行轨迹。

四份 Block raw host dump 的长度/FNV/SHA 重新核对；两组 bench 完整原始字节相同，两组 CoreMark 各七处 CPUID 只在原始同日志 helper 目标、完整 DecodeCpuid 文本及选择源码 SHA 支持下归一化四条 MOV immediate，其余共同 PC 主体相同。没有额外 MOVZ W/X 归一化或未解码 word，未匹配 formatter PC 单列；load bias 为推断，不声明 Function 或完整程序字节等价。

十一组可见 workspace 的九类缓冲闭合，分项与 total 相符。十组负载申请/释放次数、字节、live 和 peak 六项保持，formatter 相关的初始化/保留事件差异单列；IR slow 的全部已记录工作区字段与相邻相同，申请/释放各 389 次、allocated/released bytes 各 6679676、live 0、peak 3981816。它们不包含全部私有 pending/coverage/published 请求。

二十一项 owner 布局、10864-byte metadata 表及 PendingStore/CaptureByte 的 32-byte 布局均保持。launcher .text 为 4537772→4538532，增加 760 bytes；相对 construction-types-v3 累计增加 6344 bytes，此前 flush 缓冲批次的下降不抵消当前增长。SinkBlockStores 自身 code/SP 为 4232/592→4380/592，flush lambda 为 3552/368→3576/368；两处自身帧保持。完整 .text 还包含其他内联/外提及异常处理成本，不能只累加这两处函数；这些数值也不是动态/传递栈峰值或整体编译周期。

### 证据与后续工作

源码摘要 4b0f691be3afa24cbf9fc043f90bbb5b7094187ed7432ada304c96dbe6f64c88，overlay SHA f3fa8cf876eefcf480819e1a68ea5ad68a4b95ecfcf0c8da9d34f8d0d2f74de8；相邻源码 patch SHA 3b0cb12a1d1f6529abf06ab2237179a5d798d33573989b66716e817533c9484c。Android/RV64 构建日志 SHA 为 d990bd6e34935d2e8039a45da06c8be84d28c69cf37deb6693b05e6b9ffb3a9d、e0eec3d0ccb2a9e0871c98725f0a9d40208c08527f71c6438210f8e6485a96c8。完整记录归 /private/tmp/swiftvm-middle-results/r2-state-sink-unwind-v1-* 与 r7-rv64-execution-20261024-v16-*，包含 unwind-contract、test/shape/storage-summary、cost、host-byte-comparison、逐例原始记录和 user-plan/tso-reuse。

二十项限定质量检查通过。继续 capture/published 的有效载荷、稀疏位置与 consumer 查询、完整私有容量/初始化/保留时间，以及其余改写与运行时故障矩阵；完整参数签名、类型/宽度/CFG/Phi、有序候选、整体成本及正式 FEX 对照仍属于原目标，不以本批异常清理代替这些验收。

## capture 载荷收敛与部分发表顺序修复（2026-10-24）

本批最终 r2-state-capture-slots-v3 以 r2-state-sink-unwind-v1 为已验收相邻参照，修改 state_sink.cpp、ARM64 compile/register_contract.cpp、ir_contract_test.cpp 和 hir_lifetime_test.cpp。没有新增或退役源文件；overlay 仍为 472 项、累计删除 113 项。二十项限定质量检查通过，完整 R0–R7、R2/R3/R7、总体成本与正式 FEX 门继续开放。

### 实现与复现

- capture 的 full-span latest/CaptureByte、dirty 字节列表和临时 slot 列表退役。只跟踪完整且 16-byte 对齐的 V128 写候选，后续合格重叠写使候选失效；观察点按 offset 顺序输出原 recipes。无 XMM 范围时清除旧输出后直接结束。短期 CaptureSlot 为 24 bytes，recipes 仍由原 Block 持有。
- full-span published/CaptureByte 表退役为唯一 offset/size 请求的 24-byte PublishedCapture。保存完整发表 producer 与源字节有效位，局部覆盖清除相交位，同 producer 的低窄视图可恢复前缀；同槽多个 boundary 不复制证明。原稀疏位置、survivor/observer/forward 规则和 detached 异常回收保持。
- 修复真实顺序错误：旧窄写与本次读取不重叠，却与即将发表的新宽写重叠时，不能留到之后再写回。现有 coverage 逆扫直接纳入这些较早写，并传播传递重叠依赖；较晚且与读取无关的写仍可保留。完全覆盖 DSE 与 survivor 检查继续执行，不加全块扫描或容器。
- 新永久字节 oracle 在旧生产源码的红魔执行中，sink 前解释器通过、sink 后字节比较失败。修复后覆盖直接/传递重叠及较晚窄写，分别以 Block/Function、三种初始填充、优化前后解释器及实际 JIT 验证。
- 首次集中 IR 执行又暴露精确 scratch 预算遗漏：非对齐 uniform 地址需要 VIXL 地址 GPR，旧预算声明零。Load/StoreUniform 现按实际 State 地址/宽度使用同一 scaled/unscaled 编码谓词，必要时计入一个地址临时量；MXCSR 的三个显式临时量另加。未改变 emitter 编码或放宽审计。

v1 在首项 IR 因此拒绝时停止，没有继续主套件/负载；v2 的快慢 IR 通过，随后慢入口发现新计量夹具把 StoreMemory 地址传成 Value 而非 Operand。v3 仅修正该测试包装，十三份完整 ELF 与 v2 相同；快慢 IR 的两份成功记录按完整 ELF、环境/参数和原始输出身份复用，原命令与时间保持。主套件和十组负载均留到最终版本执行；修改后的寿命程序重新执行，复用范围仅上述两份 IR。没有把两次中断记为完整批次通过。

### 最终集中验证

| 验证 | 结果与适用范围 |
| --- | --- |
| Android/红魔 | NDK 29-beta3、Clang 21、API 28、Release，共同 fmt 设置；十四目标构建成功。NX729J bf1359ff 覆盖十四项原生：十二项最终产物新执行、两份快慢 IR 按上述身份复用；十组负载全部新执行。五项使用产物和两份 guest 前后核对，二十八份原始文件校验传回 |
| 快/慢 IR | 各 101 用例、80999/81097 断言全通过；相邻各新增三例、338 断言。除字节顺序 oracle 外，还验证多槽/稀疏范围、部分/非对齐/V256 覆盖、重复相同定义的最新 store 身份、重复 capture/无 XMM 清理，以及已发表值经低窄视图失效/恢复后的真实 JIT |
| 分配请求 | scalar-only capture 在 span 65536 时 C++ new 请求数/字节为零；一个槽在 span 64/65536 下的 capture 请求数及字节相同，carrier-free committed-proof 准备也相同。两种 constant-window 模式各增加四项 checks，123479→123483。carrier-free 夹具不含 pending coverage，不能推广为整个 sink 与 span 无关 |
| 失败与寿命 | 两种 state-sink-failure 模式各仍为 41 次注入、2020982 checks；已有异常归属/无新请求/uses/回收证明保持。其余八份旧成功标记仅上述 constant-window 的预定四项变化，其他原样相同；实际 spilled target miss 六次 |
| 主套件 | 748 用例、229342 断言、0 errors；既有 11 失败用例/66 失败断言的归一化内容与共同用例顺序保持，无增删主套件用例。相对 229430 的总断言差值为 −88，未逐例归因，不作为覆盖增减证明 |
| RV64 v19 | 最终完整 2552-file 快照构建十四目标，九 ELF 相对已验收 v16 变化；十九项受影响用户态 CTest 新执行通过，十三项用户态/五项 guest 内核按相同完整 ELF、原命令/属性、环境与原记录复用。强 fence 完整 ELF 也变化，因此新执行 220321 checks，通过八种编码、二十组各 5000 轮 litmus 和八组软件 CAS128 检查 |

三十七项是 RV64 专用注册范围，不是一次新跑全部三十七项 CTest。两个 compile-cost 程序和 emit 仅构建，没有新 guest 内核、物理 RV64 或 PMU。本任务 v17/v18 只形成中途构建/身份记录，均未执行测试；v19 执行结束后确认空闲并停止，其他 worker 未动。V256 在本批只验证配方覆盖，不作为 ARM64 V256 执行支持的证明。无新增生产 probe、临时开关或一次性测试脚本。

### 实际负载、存储与代码成本

十组校验值、实际模式、零 target overflow 及五项 spill 指标均与相邻相同。五组 bench 的完整有序记录及形状保持；CoreMark 范围外的有序编译记录保持，差异均位于已有 formatter 符号范围。bound-off Function 为 427→426、RA host bytes 286540→286448；普通 Block 为 2416→2418、205140→205192；fixed Block 为 2413→2418、205040→205632；普通/fixed Function 的其余对应记录保持。这些不是运行轨迹。

四份 Block raw dump 的长度、FNV 与 SHA 重新核对。两份 bench 完整有序原始字节相同；两份 CoreMark 各七处 CPUID 仅在同一参照日志的旧 target 证明、完整未改的 frontend/DecodeCpuid 和当前 ELF 符号支持下归一化四条 typed MOV immediate，其余共同 PC 主体相同。未匹配 formatter PC 单列；没有额外 MOVZ W/X 或任意 relocation 掩码，load bias 为推断，不宣称完整 Function/程序字节等价。

十一组可见 workspace 的九类缓冲闭合，分项与 total 相符。十组负载的申请/释放次数、字节、live 和 peak 六项保持；formatter 的初始化/保留事件变化单列。IR slow 整组申请/释放各 389→390，allocated/released bytes 各 6679676→6679684，增加一次和 8 bytes，live 0、peak 3981816 保持；新增三例与其他变化未逐例归因。这些计数不包含完整私有 capture/published/pending 堆。

二十一项 owner 布局和 10864-byte metadata 表保持。PendingStore 为 32→32 bytes；旧 CaptureByte 32-byte 逐字节记录退役，新 CaptureSlot/PublishedCapture 各 24 bytes。launcher .text 为 4538532→4544756，增加 6224 bytes；相对 construction-types-v3 累计增加 12568 bytes。新 CaptureSlot 的三份排序符号共 6088 bytes，不能用 capture 主函数自身变小掩盖外提成本。

CaptureLatestStateValues 自身 code/SP 为 3472/352→2132/288，SinkBlockStores 为 4380/592→5216/592，flush lambda 为 3576/368→4032/368；精确预算入口 ScratchBudget 为 2192/144→2532/160。三个新排序子函数自身帧为 288、672、416 bytes。它们不是传递或动态栈峰值；候选查找/排序、私有容量/初始化/保留时间和整体编译周期仍待收口，未宣称总体提速。

### 证据与继续工作

源码摘要 0bde84605638c7552b0185967c92485ac5f833f0ae09c19cf3587d7581c06879，overlay SHA 85466330e52cdbe715e729ccf903628acd3745eebf038565b656ee8c235bcaad；launcher SHA a38e542f30d38d5c23e91a52d85f821a68fb5798da8350aa1828cf81364fa40c，相邻 patch SHA 2450d9f06e47925225fe9f9be93308c0a0183c8da38e58111bba7920792a8873。Android/RV64 构建日志 SHA 为 94ab4033a6655ecc4d381e0a94765c4d04b55ccb8e6d97d17bb74c6af2b4bcce、8c35b1682f73592d30612d7b0fabdb384fcfff35cb5a076cb48b85b1368aa794。证据归 /private/tmp/swiftvm-middle-results/r2-state-capture-slots-v3-* 与 r7-rv64-execution-20261024-v19-*；capture-contract 保留旧源码失败复现及两次中途停止，test-executions 标明两份 IR 复用的真实来源，cost 标明完整相同 launcher 的 v2 静态分析来源，TSO 使用 v19 新执行记录。

下一步先收掉新槽记录排序特化，接续稀疏位置/consumer 查询及剩余 full-span coverage、真实私有容量/初始化/保留时间；完整参数签名、类型/宽度/CFG/Phi、有序候选、其余改写/运行时故障、总体成本与正式 FEX 门继续按原目标推进。

## 捕获候选改为有序借用指针（2026-10-24）

本批 r2-capture-order-v1 以 r2-state-capture-slots-v3 为相邻参照，只修改既有 state_sink.cpp；没有新增/退役源文件、改动测试或构建注册。overlay 仍为 472 项、累计删除 113 项。二十项限定质量检查通过，完整 R0–R7、R2/R7、成本及正式 FEX 门继续开放。

### 实现与借用边界

- 退役 24-byte CaptureSlot 与重复的 Value/offset，候选只保存一个 const Inst*；64-bit AArch64 目标每项 8 bytes。capture 准备不改写、删除或摘下 store，它们始终由原 Block 持有；生成配方时从该节点读取原始参数与真实类型视图，不创建新的 IR owner。
- 候选始终按 offset 有序，并且只包含完整、对齐、互不重叠的 V128 槽。一次 lower_bound 定位首个 end > 写入 start 的候选；完整写更新同槽的最新 store 指针，缺失时按位置插入。部分或非对齐写删除从这里开始、start < 写入 end 的连续交叠候选。
- 观察点直接按已有顺序生成原 recipes，再 clear 并复用指针容量；整段 std::sort 和三份大记录排序特化退役。没有为顺序增加第二份候选、键表或有序 tree。
- SinkBlockStores 全部逻辑、前向转发/survivor/observer、部分 flush 依赖闭合、有效字节证明、detached 异常回收和精确 scratch 契约源码保持。候选指针只属于本次只读 capture，不进入 prepared actions 或跨块 facts。

查找需要读取候选 IR 参数，按位置插入/删除需要移动指针尾段；这些访问及真实 vector 容量成本继续计量。不能只由元素变小推断总体堆峰值或编译周期下降。

### 一次集中执行的范围

| 验证 | 本批结果 |
| --- | --- |
| Android/红魔 | 十四目标首次构建成功；NX729J bf1359ff 新执行六项既有原生模式和四组普通 Block/Function 负载。三项使用产物与两份 guest 身份前后核对，十二份原始文件校验传回 |
| 快/慢 IR | 各 101 用例、80999/81097 断言通过；用例、共同顺序和状态与相邻一致，没有新增测试。现有多槽乱序插入、精确最新 store 身份、partial/非对齐/V256 失效、稀疏范围和重复 capture 覆盖继续通过；V256 仍只证明配方覆盖，不是 native 执行支持 |
| 失败/存储模式 | 两种 state-sink-failure 各 41 次注入、2020982 checks；两种 constant-window 各 123483 checks，四份成功标记与相邻原样相同。已有零候选/同峰值重复 flush、span 64/65536 capture 与 carrier-free proof 的 C++ 请求比较继续通过；不当作全部 malloc/arena 或整体私有成本证明 |
| 功能与形状 | 普通 Function/Block 各新执行 CoreMark 与 bench，共四组，校验值、实际模式、零 target overflow 和五项 spill 指标与相邻相同。两份 bench 的完整有序编译记录与形状相同；CoreMark 非 formatter 范围的有序编译记录相同 |
| RV64 v20 | 最终 2552-file 完整快照构建十四目标，四 ELF 相对 v19 变化；十七项受影响用户态 CTest 新执行通过，十五项用户态/五项 guest 内核依完整相同产物、原命令/属性、环境和记录复用。强 fence TSO 完整 ELF 未变，复用实际 v19 的 220321-check 执行及八种编码/二十组 litmus/八组软件 CAS128 原记录 |

本批按改动范围集中验证，没有重跑 Android 主套件、fixed-class 或 call-persistence 组，也未将此前记录追认为变化后的 ELF 成功证据。v3 的主套件仍只是该历史产物的 748 用例、11 失败用例/66 失败断言记录。RV64 三十七项为专用注册范围，不称一次新跑全部三十七项；两个 compile-cost 与 emit 仅构建，无新内核、物理 RV64 或 PMU。自己的 v20 容器空闲后已停止，其他 worker 未动。

### 实际产物与成本

launcher .text 为 4544756→4540140，减少 4616 bytes。此前三份 CaptureSlot 排序特化合计 6088 bytes，现已从最终 ELF 消失；还有普通内联/外提和模板放置变化，不能把该 6088 直接等同于最终净减少量。相对 state-sink-unwind-v1 仍累计增加 1608 bytes，相对 construction-types-v3 仍累计增加 7952 bytes。

CaptureLatestStateValues 自身 code/SP 为 2132/288→2968/320，代码增加 836 bytes、自身帧增加 32 bytes；此前三份排序子函数的 288/672/416-byte 自身帧退役。这不是传递或动态栈峰值证明。SinkBlockStores 的 5216/592、flush 的 4032/368、精确 ScratchBudget 的 2532/160 自身 code/SP 保持。

二十一项 owner 布局和 10864-byte metadata 表保持；PendingStore/PublishedCapture 仍为 32/24 bytes，CaptureSlot 类型退役。候选每项 24→8 bytes 是原 DWARF 记录大小与当前 64-bit 指针表示的比较，不代表实际总分配或容量保留降为三分之一。

四组负载中 Function CoreMark 为 427→426 条编译记录、RA host bytes 286528→286436；Block CoreMark 为 2418→2414、205192→204680，差异均在既有 formatter 符号范围。bench 对应记录/host bytes 保持。两份 Block raw dump 的长度/FNV/SHA 均重核，bench 完整原始字节相同；CoreMark 七处 CPUID 在同一参照日志的 typed target、完整未改 frontend/DecodeCpuid 和当前 ELF 符号支持下只归一化四条 MOV immediate，其余共同 PC 主体相同。未匹配 formatter PC 单列，不声明完整 Function/程序或运行轨迹等价。

五组可见 workspace 的九类缓冲闭合，分项与 total 相符。四组负载的申请/释放次数、字节、live/peak 六项保持；formatter 相关初始化/保留事件变化单列。IR slow 的所有已记录工作区字段与相邻相同：申请/释放各 390 次、allocated/released bytes 各 6679684、live 0、peak 3981816。它们不覆盖全部私有候选/pending/published 请求。

### 证据与后续工作

源码摘要 3bea683c70284758d0b37301468a4932199f5309b0b70067ba5b741c40002cda，overlay SHA be36a9719dc9c10743b0dcff53f627c7787bbda28cbbccee53aee6b549c3b215；launcher SHA d5571f59763ce5f1392ffa24408c6e7d6440c0767f6dde2981306a2431566b0c，patch SHA 5095b44c16ee002075518ec5f72dee666a99e938656e628786ec9b53ff520ec2。Android/RV64 构建日志 SHA 为 5481fe8119bb7dca8c9ee1d1afc3b4df74d9c83a6cda722947910f40a3678c80、60a5a8c3c0a5251e210a62ec82ad38536ed57c979da24ee38b0d926b1263667f。证据归 /private/tmp/swiftvm-middle-results/r2-capture-order-v1-* 与 r7-rv64-execution-20261024-v20-*，order-contract、限定 test/shape/storage-summary、cost、host-byte-comparison 及 TSO 原 v19 复用记录保留实际范围与来源。

继续候选实际容量/查找/指针移动、稀疏位置与 consumer 查询、仍为 full-span 的 coverage 和完整私有初始化/保留时间。完整参数签名、类型/宽度/CFG/Phi、有序候选、其他改写/运行时故障、整体成本及正式 FEX 门继续按原目标推进；本批排序退役不代替这些验收。

## state sink 按实际需求准备 coverage、carrier 与位置（2026-10-24）

本批 r2-state-sink-demand-v1 以 r2-capture-order-v1 为相邻参照，修改既有 state_sink.cpp、ir_contract_test.cpp 和 hir_lifetime_test.cpp；没有新增/退役源文件、测试脚本或构建注册。overlay 仍为 472 项、累计删除 113 项。首次构建及一次集中执行通过二十项限定检查，完整 R0–R7、R2/R7、动态成本和正式 FEX 门保持开放。

### 准备范围与阶段约束

- coverage 不再按 uniform_size 准备；在既有选择循环归纳全部 pending 的最小 offset/最大 end，按该包围区间清零并以相对 byte offset 查询。包含未选中的 pending，保证反向传递重叠依赖可继续扩展；不增加边界预扫或新容器。内部空隙和保留至块结束的峰值容量仍存在。
- PublishedCapture 原 24-byte 布局的四字节尾部填充用于 carrier 索引，实际 DWARF 显示偏移 20，元素仍为 24 bytes。每次有 before 的选中 flush 重置索引，再保存每个唯一 offset/size 的最后一条 Publish 行；各配方不再重扫 pending。索引只在压缩前使用，不是 Inst ID 或持久 fact，pending 数量有显式 32-bit 边界检查。
- 配方无 carrier 时不查提前位置；精确 boundary 也不需要位置。只有命中 carrier 且当前 before 与该配方 boundary 不同时，才首次建立既有配方唯一 producer/boundary 的稀疏表。它仍可能为其他已知配方准备条目，并非逐查询最小表。
- 延迟建表只给非 StoreUniform 节点编号。sink 不改变这些定义/观察点的相对顺序，因此此前 store 的摘链、删除与重连不影响严格 producer < current < boundary。缺失 producer/boundary 仍为缺失，不能当作无限远；原始 ID 稀疏或晚分配不参与顺序判断。
- 已发表字节证明成立时短路 HasObservableUse，保留必要的无证明回退。最终 terminal 后不再读取的 carrier/committed proof 不再更新。capture 准备、前向转发、survivor、DCE/value 通知和 detached 异常回收策略保持；位置表现在可能在 store 脱链后分配，失败输入仍交回 owner 后丢弃，不作事务回滚或重试。

### 集中验证

| 验证 | 结果与范围 |
| --- | --- |
| Android/红魔 | 十四目标首次构建成功；NX729J bf1359ff 新执行七项原生验证、四组普通 Block/Function 负载。四项使用产物和两份 guest 身份前后核对，十四份原始文件校验传回 |
| 快/慢 IR | 各 101 用例、81035/81133 断言通过，用例及共同顺序未变；各增加 36 断言。既有 relink 夹具先在不同槽完成精确 flush 并删除旧 store，再在后续新增观察点触发延迟位置，覆盖 Block、稀疏 ID Block、实际 Function、producer 前/后与缺失 boundary，原始非 store 顺序及 carrier 身份保持 |
| 失败回归 | 既有五类逐点 C++ new 失败夹具在有 capture 时增加真正命中 carrier 的提前观察，让位置分配发生在较早 partial flush 可删除 victim 之后。fast 0/1 各仍注入 41 次，checks 各 2020982→2020998；这 16 项差值来自扩展夹具的实际检查，不按检查数扩张覆盖声明。原无额外清理请求、Block 归属、raw/HIR uses、部分删除后失败与销毁 oracle 均保留 |
| 分配请求 | 两种 constant-window 各增加两项 checks，123483→123485。相同峰值 pending/相同八次 flush 下，uniform span 64/65536 及 offset 64000 的请求数/字节相同，八次必要发表保留；既有零 store、8/64 次 flush、capture 与 carrier-free proof 比较继续通过。单个 V128 样本的 coverage 逻辑范围为 16 bytes，不声称全部 allocator/arena 或总堆下降 |
| 状态/故障子集 | 新执行既有 Uniform*、XMM*、fault capture* 和 resident page-fault 共 11 用例/125 断言，全通过；相对 v3 完整主套件中对应子集的名称、顺序和成功签名保持。没有把未选择的主套件用例记为本批通过 |
| 四组实际负载 | 普通 Block/Function 各执行 CoreMark 与 bench；校验值、实际模式、零 target overflow 及五项 spill 指标与相邻相同。两份 bench 完整编译记录和形状保持；CoreMark 非 formatter 的有序编译记录保持 |
| RV64 v21 | 最终完整 2552-file 快照构建十四目标，四 ELF 相对 v20 变化；十七项受影响用户态 CTest 新执行通过，十五项用户态/五项 guest 内核按完整相同 ELF、命令/属性、环境和原记录复用。强 fence TSO ELF 未变，实际执行仍为 v19 的 220321 checks，复用链保留原命令/时间及八种编码/二十组 litmus/八组软件 CAS128 原记录 |

三十七项仍只是 RV64 专用注册范围，不是本批新跑三十七项 CTest；两个 compile-cost 与 emit 仅构建，无新内核、物理 RV64 或 PMU。自己的 v21 容器确认空闲后停止，其他 worker 未动。Android 完整主套件、fixed-class/call-persistence 未重跑；v3 的 11 失败用例/66 失败断言仍是历史产物的完整套件记录，不追认变化后的 ELF。

### 代码与存储成本

二十一项 owner 布局和 10864-byte metadata 表保持。PendingStore/PublishedCapture 仍为 32/24 bytes；carrier 只使用原填充，没有新容器或持久 IR/facts 字段。位置哈希依然存在，准备触发收窄而非全部退役。

launcher .text 为 4540140→4540356，增加 216 bytes；相对 state-sink-unwind-v1 仍累计增加 1824 bytes，相对 construction-types-v3 累计增加 8168 bytes。SinkBlockStores 自身 code/SP 为 5216/592→4756/656，flush 为 4032/368→4312/384，新延迟位置查询函数自身为 380/112。外层自身帧增 64 bytes、flush 增 16 bytes，冷查询还可能形成更深调用链；这些不是传递/动态栈峰值或整体编译周期证明，不能用主函数代码缩小抵消其他成本。

五组可见 workspace 的九类缓冲闭合，分项与 total 相符。四组负载的申请/释放次数、字节、live/peak 六项保持，formatter 的初始化/保留事件差异单列；IR slow 的所有已记录字段与相邻相同：申请/释放各 390 次、allocated/released bytes 各 6679684、live 0、peak 3981816。私有 coverage/位置表/候选的全部容量与保留时间不在这些计数内。

Function CoreMark 的编译记录为 426→427，RA host bytes 286436→286528；Block 为 2414→2412、204680→204384，差异都在已有 formatter 符号范围。两份 Block raw dump 的长度/FNV/SHA 重核，bench 完整原始字节相同；CoreMark 七处 CPUID 仅在同参照日志的 typed helper、完整未改 frontend/DecodeCpuid 与当前 ELF 符号证明下归一化四条 MOV immediate，其余共同 PC 主体相同。未匹配 formatter PC 单列，不宣称完整 Function/程序或运行轨迹等价。

### 证据与继续工作

源码摘要 47e6f487dbccb88a162ffa0e5120bad17203b8272552c5bf340ac8b2cc59edcc，overlay SHA 2ef5f21bb05932b14bea90ff25f725546823e8263ceaf4a96f2b2a49f8d5a80a；launcher SHA 9541f62a3541a1c9babe275eacddfe2049b43a057a53b4cba252af9449f48a18，patch SHA 9f71e15d07d2b2d0d425e51560f69414df3afdf215aab3ebbe00f57184c0ff0b。Android/RV64 构建日志 SHA 为 7c74e98e7dd7d04c46018622a1ecdff7e436173d19ed64005a385dd6cc5f475f、03417aa54aa90ff0aac411e7d854dff899e31f7414898f01d7921f36a3abce0a。证据归 /private/tmp/swiftvm-middle-results/r2-state-sink-demand-v1-* 与 r7-rv64-execution-20261024-v21-*，demand-contract、PublishedCapture-members、限定 test/shape/storage-summary、cost、host-byte-comparison 及实际 v19 TSO 的复用链记录保留各自范围。

继续覆盖区间内部空隙与峰值保留、稀疏位置/必要 observer、候选查找/移动、完整初始化/传递栈/编译周期。完整参数签名、类型/宽度/CFG/Phi、有序候选与其他改写/运行时故障、正式 FEX 对照仍属于原目标；本批准备触发收窄不代替完整验收。

## 参数签名、嵌套枚举与 HIR uses 收敛（2026-10-24）

本批最终 r2-argument-signatures-v4 以 r2-state-sink-demand-v1 为相邻已验收源码，修改二十四个既有源文件，没有新增源文件、构建注册、临时 probe 或测试脚本。overlay 仍为 472 项、累计删除 113 项。二十项限定检查通过；完整 R0–R7、R2/R7、总体成本与正式 FEX 门继续开放。

### 源文件与契约

- ir_types.h、ir_meta.h、ir.inc：区分物理 ArgType 与单字节 ArgSignature；既有唯一运行时签名向量的元素保持一字节。CallLambda 显式声明三个可选 Value/Imm 槽，Not 显式声明可选完整 Operand；编译期验证可选项只能是末尾连续后缀。Block、HIRBlock、HIRFunction、IrBuilder 与 Inst 的具名工厂直接消费 schema 的 ValidArgCount，原 opcode 个数特判已退役，结果来源策略保持。
- instr.h/.cpp：CheckArguments、ForEachValue 和 ForEachValueUse 共享同一嵌套枚举。验证全部声明参数种类、required/optional 形状、内部 Operand 槽、未使用尾槽、Value/Imm/Uniform/Local 类型、Params 数据、Lambda 来源、Cond/Flags 与 OperandOp；PlusExt 的 shift_ext 不能超过 63。StoreUniform 的同宽别名规则保持。旧 CheckTypeViews/HasValidTypeViews、三类 Call 的 Validate 提前返回及重复签名循环已删除。
- args.h/.cpp：Lambda::IsImm 识别普通或合法 helper 标签立即数；GetImm 的前置检查共用该谓词。空 Lambda 可由参数门拒绝，不先读取其 Imm。
- hir_builder.cpp：UseInst/UnUseInst 删除两套逻辑/物理元数据循环，统一访问实际 Value occurrence。直接参数与 Lambda 保留物理槽，Params 映射到既有 USE_FUNC_CALL；重复值不去重，立即数不创建 HIRUse。UseFreshArg 的构造快路径仍保留。
- SSA、ARM64 准入/audit、RV64 准备/直接发射及 values 改写检查接入新规则。封口时参数形状和 use-end 同次访问，不增加第二遍完整参数扫描；优化前拒绝和封口后证明保留各自生命周期。RV64 先检查参数，再查询 native ABI，缺参不能进入类型化 GetArg。

这里完成的是已存储 IR 的参数种类和形状。工厂的编译期数量检查还需补齐 C++ 参数包种类/物理槽容量的构造前约束；BOOL 的 opcode 语义、全部运算宽度/转换与立即数值域、Phi/CFG 和更完整 SSA 仍需接续；不把默认结果类型当作全部低层显式契约。ARM64 Phi/local 与 RV64 的既有能力边界保持，Block 与 Function 分配仍分别处理。

### 实际执行与失败修正

集中修改后逐次修正实际发现的问题，没有为原样失败重复重试：

1. v1 的新增 HIR 夹具在 Function 结束前创建 RewriteContext，触发 roster 阶段检查。v2 改为先创建两种调用、结束 Function 并完成 RPO/编号，再改写参数，生产阶段门不变。
2. v2 完整主套件暴露旧 resident-string 夹具将 SaveFlags 返回的 Void 传给 GetFlags。v3 保留 SaveFlags，改为引用原算术结果，并独立要求 0 - 1 且清除 C 后的 NZCV 为 N=1/Z=C=V=0；不放宽参数门。
3. RV64 v24 的十九项新执行中十八项通过，pure_flags 仅因仍匹配旧 operand-view 错误文本失败。最终 v4 更新精确预期为完整参数门的文本，拒绝、未缓存和状态写计数检查均保留。该改动不进入 Android 的十四个目标，因此 Android 最终无工作构建后，按完整 ELF 相同复用实际 v2/v3 结果。

| 验证 | 实际来源与结果 |
| --- | --- |
| Android 构建与设备 | NX729J bf1359ff；十四目标已构建，最终 v4 的全部 ELF 与 v3 完整相同。实际使用的 ELF 与两个 guest 在设备执行前后核对，原始文件传回校验。v2 执行六项 IR/寿命，v3 执行六项主套件/调用/helper 及四组负载；最终 v4 不重复执行 |
| 快/慢 IR | 实际 v2 各 104 用例、81232/81330 断言，全通过；各相邻增 197 断言。新增三例覆盖完整/缺失 Operand、非法内部/尾槽、可选参数空洞、Value/Imm 混合调用、tagged/空 Lambda、必需空 Params、非法 nested data 与 HIR 登记/撤销。既有准入例扩展九种畸形形状，分别经过 Block/Function 封口 SSA 与优化前 ARM64 拒绝；修正一处原 CallLambda/Params 夹具为 CallDynamic |
| 完整主套件 | 实际 v3 新执行 748 用例、229562 断言，0 errors、66 failed assertions，仍为历史十一失败用例；相对实际 capture-slots-v3 的名称/顺序/失败签名一致。相邻完整套件总断言增加 220，未逐例归因，不据此扩张覆盖或声称全绿 |
| helper 与构建慢路径 | v3 额外以 SVM_IR_FAST=0 执行两个既有 shared-host-call/field-identity 用例，17178 断言通过。原 field-identity 夹具改为混合 Value/Imm，逐槽和 HIR use 链仍比较快/慢构建 |
| 调用与持久化 | v3 的 static/aot-static/indirect/aot-indirect 四模式通过，checks 为 13/99/13/111，成功标记与各自实际历史记录相同；未将此范围扩大为完整 flags0/Block/并发验收 |
| 寿命和分配失败 | 实际 v2 constant-window 两模式各 123485 checks；state-sink-failure 两模式各 2020998 checks、41 次真实 C++ new 故障，标记与相邻相同；最终完整 ELF 相同后复用，未声称 v4 新跑 |
| 普通负载 | v3 分别执行 Block/Function 的 CoreMark 与 bench；校验值、实际模式、零 target overflow 与五项 spill 指标保持。没有 PMU 重跑 |
| RV64 v25 | 最终 2552-file 源码快照构建十四目标；相对已验收 v21 九 ELF 变化，相对失败尝试 v24 只有 pure_flags ELF 变化。v25 新执行一项 pure_flags 通过；三十一项用户态与五项 guest 内核按完整相同 ELF、命令/属性、QEMU/镜像及原日志复用，其中十八项用户态实际成功于 v24，另十三项沿 v21 的既有链。三十七项只指 RV64 专用注册范围 |
| 强 fence TSO | v25 对变化后的完整 ELF 新执行 --expect-strong，220321 checks，通过八种编码、二十组各 5000 轮 litmus 和八组软件 CAS128；保留实际命令、时间和原始记录。这是有限 QEMU 回归，不是物理 RV64、正式内存模型证明或 PMU |

RV64 的两个 compile-cost 与 emit 仅构建；无新 guest 内核启动或物理板验证。v22/v23 只完成构建，v24 的失败不抹除且仅复用其实际通过并保持 ELF 的十八项。自己的所有 RV 尝试容器在确认只剩 sleep 后停止，其他 worker 未动。

### 代码、布局与存储

二十一项 owner 布局保持，包括 IRMeta 56、Inst 94、IrBuilder 520、SemanticFacts 624 bytes；元数据表仍为 10864 bytes。没有新增逐指令签名/effects/type 缓存或持久工作区。直接与间接 uses 的顺序及 Params 标记由同一枚举维护。

launcher .text 为 4540356→4533060，减少 7296 bytes。实际 HIRFunction::UseInst 的自身 code/SP 为 2452/256→332/80，UnUseInst 为 1072/160→332/80；HIRValue::Use/UnUse 的实际 callee 代码与 SP 也独立保留。Inst::Validate 为 1096/112→576/112，HasValidTypeViews→HasValidArguments 为 1272/144→1452/96。一处包含 SSA 收集的 PrepareBaseFacts lambda 为 4572/416→4852/464，自身帧增加 48 bytes；另一层 value 回调为 500/112→504/112，新增 Lambda::IsImm 为 20/0。上述 SP 取各符号可见自身减量，未证明传递调用链或动态峰值，不能用总代码缩小抵消新增签名分支和栈成本。

五组可见 workspace 的九类缓冲闭合，各项与 total 相符。四组普通负载六项基础申请/释放/字节/live/peak 相同；Block CoreMark 的 formatter 变化单列为 retention events 22257→22320、pattern init bytes 817224→819572。IR slow 的实际 v2 全部已记字段与相邻保持：申请/释放各 390 次、bytes 各 6679684、live 0、peak 3981816。这些不覆盖所有私有 malloc/arena、保留时间或总体编译成本。

Function CoreMark/bench 与 Block bench 的完整有序编译记录保持。Block CoreMark 2412→2418 条、host bytes 204384→204980，差异仅在已记录 formatter；其余有序记录保持。两份 Block 原始 dump 长度/FNV/SHA 重新核对，bench 完整原始字节相同；CoreMark 只依据相同参照日志的旧证明、未改完整 frontend/DecodeCpuid 与当前 ELF 符号规范化七处 CPUID 的四个 MOV immediate 字段，其余共享 PC 字节保持。未匹配 formatter PC、推断加载偏移和非完整 Function/程序/运行轨迹等价的边界继续保留。

### 证据与后续

源码摘要 f2febca10458d8a221a23e0c8a5bb5edebbd86ce65f370e69eb71267a787bd81，overlay SHA d629d50c0cbad572610afa0642f3535f4946a121990eee63493bb50c83a68724；launcher 使用实际 svm_argument_signatures_v3，SHA 32f112edd1913167bdffe529cf951854b1829c853e0a01247f5854ba48417e89。patch SHA a4bd69c6f76d6fdc1630d743f47a1233c6113577cb42eb5773297064c49af0dd。证据归 /private/tmp/swiftvm-middle-results/r2-argument-signatures-v4-*、实际 v2/v3 的 native/results 和 r7-rv64-execution-20261024-v25-*；android-binding、quality、attempts、cost 与 RV24 partial summary 保留各自真实来源。

下一步接续构造前参数包约束、opcode 位宽/值域/转换及 CFG/Phi 契约，并继续有序候选、guest-state/helper/fault/观察 effects 和完整动态成本。当前参数形状门、去重或限定测试通过不替代完整 R2/R7、运行时矩阵与正式 FEX/PMU 验收。

## 构造前参数包约束与静态签名借用（2026-10-24）

本批 r2-construction-signatures-v2 以 r2-argument-signatures-v4 为相邻已验收源码，修改十四个既有源文件，无新增源文件、构建注册、临时 probe 或测试脚本。overlay 仍为 472 项、累计删除 113 项。二十项限定检查通过，完整 R0–R7、R2/R7、总体编译成本与正式 FEX 门继续开放。

### 文件职责与构造边界

- instr.h 的 ArgumentKind/ArgumentSlots/ArgumentStorage 将实际 C++ 参数类投影到既有 ArgType，并在模板约束中拒绝未知类、单独 Operand::Op 或超过五的总物理槽；完整 Operand 占三槽。ValidArgumentPack 共用 schema 的长度与种类匹配，具名 Inst、Block、HIRBlock、HIRFunction、HIRBuilder、IrBuilder 工厂以 requires 排除错误调用，避免先创建节点再发现种类错误。
- SetArgs/SetArgsFresh 按编译期种类计算展开位置，旧 arg_index 的临时 Arg 构造退役。原始 setter 仍可组成未完成/导入的 IR，不能据此跳过存储后视图、内部载荷、SSA 或封口检查；带索引 SetArg 保留其改写职责。
- Block/HIRBlock/HIRFunction 的动态完整入口在 new Inst、uses 登记与编号之前运行 CheckArgumentPack；SetInst 在 opcode/result 写入前检查输入包。RV64 wide.cpp/function.cpp 的完整 Emit/Inst 同样先检查，内部占位 New 与后续填槽的阶段不被混淆。HIRBuilder 只在 append 成功后更新 pending-flags，terminal 后的 AdvancePC 忽略路径也先验证输入。
- ir_meta.h 共用 AcceptsArguments/ValidArgCount，ir_types.h 将 IRMeta.arguments 改为只读 span；ir_meta.cpp 直接借用既有 kArgs，constinit 表及 trivial-destructor 断言排除元数据动态构造/析构。没有新平行 opcode 表或逐指令类型缓存。
- 两处未使用且将函数指针直接转发为 IR 参数的 Block/HIRBuilder CallHost 适配器删除；实际 IrBuilder 的 typed CallHost/CallHostWithTraits 保留，并直接使用 CallLambda 签名约束，原硬编码三参数计数退役。

第一轮 v1 编译在三组旧夹具处产生八项诊断，没有执行设备测试：use-index 用例的七处 Add(Value,Value)、不可达块的 LoadMemory(Value) 及固定-clobber 用例的 SetLocation(Imm) 缺少声明要求的包装。v2 只将同一来源放入 Operand/Lambda，保留定义身份、重复 uses、指令数/编号和 clobber 意图；没有放宽生产约束。完整工程通过后才进行一次集中设备执行。

### 集中验证与来源

| 验证 | 结果与范围 |
| --- | --- |
| Android/红魔 | 十四目标构建成功，NX729J bf1359ff 新执行十二项原生与四组普通负载；五份使用的 ELF 和两个 guest 执行前后哈希核对，二十份原始文件校验传回 |
| 快/慢 IR | 各 104 用例、81549/81647 断言全通过，均相邻增加 317。既有六种构造路线、两种 fast 模式各验证六类动态错误包；拒绝后指令数、raw uses、HIR 编号/记录保持，后续合法调用连续编号。SetInst 错误输入也保持旧 opcode、result、立即数和 uses。编译期 concepts 分别验证五类工厂及 Inst 的错误种类/物理槽排除，没有新增测试 case 或脚本 |
| 元数据身份 | 既有 IR meta consistency 对 191 项 opcode 各增加签名长度及 data 指针身份检查，共 382 断言，确保运行时借用同一静态签名而非独立副本；空签名同样保留一致来源 |
| 完整主套件 | 748 用例、230055 断言，0 errors、66 failed assertions，仍为十一历史失败用例；共同名称、顺序和失败签名与实际相邻记录保持。总断言增 493，其中 382 来自上列身份检查，其余 111 未逐例归因；不扩张覆盖或声称全绿 |
| helper/持久化 | fast-disabled 的两个 helper/field-identity 用例共 17178 断言通过；static/aot-static/indirect/aot-indirect checks 仍为 13/99/13/111，成功标记保持。完整 flags0/独立 Block/并发矩阵未据此补齐 |
| 寿命/失败回收 | constant-window 两模式各 123485 checks；state-sink-failure 两模式各 2020998 checks、41 次实际 C++ new 故障，标记与相邻相同；这是已有作用域的故障证明 |
| 普通负载 | Block/Function 分别执行 CoreMark 与 bench，校验值、实际模式、零 target overflow 和五项 spill 指标保持，未重跑 PMU |
| RV64 v27 | 最终 2552-file 完整源码构建十四目标，相对已验收 v25 九 ELF 变化；十九项用户态 CTest 新执行通过，十三项用户态/五项 guest 内核按完整相同 ELF、原命令/属性、QEMU/镜像与日志复用。v26 仅构建未执行；最终夹具修改不进入该十四目标，v27 对完整修正源码重新配置/构建并确认产物 |
| 强 fence TSO | v27 对变化后的完整 ELF 新执行 --expect-strong，220321 checks，通过八种编码、二十组各 5000 轮 litmus 与八组软件 CAS128；原命令、时间、日志和完整身份保留 |

三十七项仍仅为 RV64 专用注册范围；两个 compile-cost 与 emit 仅构建，无新内核启动、物理 RV64 或正式 PMU。自己的 v26/v27 容器在空闲后停止，其他 worker 未动。

### 元数据、代码与成本

IRMeta 为 56→48 bytes，194 项运行时表为 10864→9312 bytes；其他二十项已记录 owner 布局保持，Inst 仍 94、IrBuilder 520、SemanticFacts 624 bytes。191 条声明中 183 条非空、共 420 个单字节签名元素不再被 vector 复制。实际 metadata object 的 __cxx_global_var_init 18392 bytes 及四字节 _GLOBAL__sub_I 包装均消失，表符号由 BSS 的 10864 bytes 转为静态数据的 9312 bytes。这个结论由 constinit/trivial 约束、跨 TU 指针身份与实际 object 共同支持；不是全部进程堆、加载重定位或启动耗时的证明。

launcher .text 为 4533060→4536468，净增 3408 bytes。对完整模板参数列表归一化后匹配 108 个显式结果创建变体，合计 code 为 112712→130464，增加 17752 bytes，自身 SP 均保持，最大可见自身减量仍 192；LLVM 对新 constrained 参数包显示为 T const&...，原完整符号与汇编均保留。三个新 outlined CheckArgumentPack 自身 code/SP 为 196/16、212/16、204/16；它们可能增加调用链深度，不能据此推断传递/动态栈峰值。SSA 收集 lambda 为 4852/464→4844/464。

元数据初始化代码消失并未消除构造门的成本：具名工厂虽已有编译期证明，仍流经动态 CheckArgumentPack，多数检查进入创建函数内联。下一批优先传递这一证明、让真正动态入口只验证一次，同时保持未完成 IR 与优化前/封口后的必要检查；不能将当前代码净增当作完成性能门。

完整带 debug ELF 为 121583816→122730784 bytes；.rodata 179676→180124、.bss 7930752→7919872、.data.rel.ro 82968→92280、.init_array 200→192。所有 section 分项与记录保留，不以 .text 代替整个文件/只读常量/重定位或加载成本。

五组可见 workspace 的九类缓冲闭合，各项与 total 相符，四负载六项基础申请/释放/字节/live/peak 保持；Block CoreMark 的 retention events 22320→22329、pattern init bytes 819572→821296，formatter 变化单列。IR slow 已记录字段仍为申请/释放各 390、bytes 各 6679684、live 0、peak 3981816。没有全部私有 malloc/arena、初始化和保留时间的总体证明。

Function CoreMark/bench 与 Block bench 完整有序记录保持；Block CoreMark 为 2418→2419 条、host bytes 204980→205260，差异限于已记录 formatter，其余有序记录保持。两份 Block 原始长度/FNV/SHA 均核对，bench 完整原始字节相同；CoreMark 仅依据相同参照日志的旧证明、未改完整 frontend/DecodeCpuid 与当前 ELF 符号规范化七处 CPUID 的四个 MOV immediate 字段，其余共享 PC 字节保持。未匹配 formatter PC、推断偏移与非完整程序/Function/运行轨迹等价的边界仍保留。

### 证据与后续

源码摘要 66748f7628ede0a6211be4c0de5f1f11aa727bc24ce01f5f84544a3ac4a69308，overlay SHA dc73499cdcd638a0f6e8673699093a45dee9b07021f5c497a87a19b3dd349ebe；launcher svm_construction_signatures_v2 SHA 9a2432e9cf6d0f96f22535543c3f3510ff5b2ef0dcb9a3ede91e72c29ed2cd02，patch SHA d83fe7d68c735d43b1e03ae8d35d73c0d6d99955c668d901fd5b157bec73d4ed。证据归 /private/tmp/swiftvm-middle-results/r2-construction-signatures-v2-* 与 r7-rv64-execution-20261024-v27-*，metadata-storage、constructor-cost、cost、quality、test/shape/storage-summary 保留各自范围。

先处理具名/动态创建入口的重复验证及其代码/周期成本，再接续全部 opcode 位宽/值域/转换、CFG/Phi、有序候选、guest-state/helper/fault/观察 effects 与完整运行时、成本和正式 FEX/PMU 门。当前构造种类/容量及限定回归通过不代表完整 R2/R7 或原目标完成。

## 已知签名贯通共享构造入口（2026-10-24）

本批接续上一批具名工厂重复进入动态参数门的问题。冻结批次为 `r2-known-construction-v1`，相邻参照为 `r2-construction-signatures-v2`，源码摘要为 `024201db7b30465481b86089a18dff4e01e670c13aff1cc8b905a9998bdc0b9c`；仅修改七个既有 C++ 源文件与两份文档，没有新增源文件、测试用例、构建注册、临时 probe 或测试脚本。472 项源码覆盖、累计 113 个退役路径与上一批相同。源码先冻结再构建，下面的设备与 RV64 验证均使用本批产物。

### 构造入口与文件职责

| 文件 | 本批职责与边界 |
| --- | --- |
| `ir/ir_meta.h` | 从同一 ir.inc 生成 InstructionSignature 类型投影，仅引用既有 constexpr kArgs；没有平行运行时表或新增签名载荷 |
| `ir/instr.h` | ConstructionOp 只持有一字节 opcode，OperationFor 按 ArgumentKind 参数包绑定；KnownOperation 由原签名作编译期约束，CheckArgumentPack 在动态校验后返回同一类型；具名 SetInst 与枚举入口共享写入，保留已有/显式结果类型和可变阶段检查 |
| `ir/block.h` | 枚举入口先检查一次，具名入口传入已知 opcode，二者共享同一参数包的分配、fast/slow 参数登记和链接主体 |
| `ir/hir_builder.h` | HIRBlock/HIRFunction 同样共享主体；HIRBuilder 将凭据直接传给当前 Function，coalescing 和 terminal 后 AdvancePC 路径不重复进入参数门，flags 记账仍在实际 append 后 |
| `compile/builder/ir_builder.h` | 具名折叠后的原始创建与动态 append 共用按参数种类绑定的入口；转给 HIRBuilder/Block 时继续传凭据，AfterAppend 与立即数位置记录保持 |
| `compile/builder/guest_state.cpp` | 十处 Load/StoreUniform、BitExtract/BitCast 和 Get/SetHostGPR/FPR 已知 opcode 创建改走模板入口；字节事实、映射及失效规则保持 |
| `tests/ir_contract_test.cpp` | 扩展既有构造用例，静态拒绝凭据/参数种类错配、非法已知签名、默认/枚举直接构造及隐式枚举转换；在六条路径、两种模式下核对枚举/已知/动态检查结果的实际创建与编号连续性 |

具名入口经过 KnownOperation，动态入口经过 CheckArgumentPack，然后都将 OperationFor 传入共享创建主体。该对象按规范化参数种类绑定，没有把大型主体按每个 opcode 再实例化一份，也没有在 Inst、builder 或 facts 中增加持久字段。TypedValue 与普通 Value 共享相同参数种类；缺参、错误种类与总槽超过五仍由同一签名拒绝。

凭据只证明 C++ 参数种类、数量和物理容量，不证明实际 Value/Operand 载荷、数据视图、位宽、立即数值域、定义归属或 SSA。原始 SetArgs/SetArgsFresh 与分阶段 RV64 placeholder 的职责不变；优化前 SupportsIR、存储后的 CheckArguments 和封口 SSA 必要证明没有删除。动态无 hint 的结果来源解析、低层显式覆盖和 VOID 控制节点规则也没有改变。

### 集中 Android 验证

Android 一次完整构建十四目标成功，十四个 ELF 均已冻结。红魔 NX729J（bf1359ff）集中执行十二项 native 和四项工作负载；五个使用中的 ELF、两个 guest 输入在执行前后完整哈希一致，二十份原始 log/XML 已回收并校验。未因本批改动逐文件重复上机。

| 验证 | 本批结果 |
| --- | --- |
| IR fast / slow | 各 104 用例、81705 / 81803 断言，无失败；各比相邻版本增加 156 个断言，来自六条路径与两种模式的三类成功创建和 HIR 编号/结果记录检查 |
| 主套件 | 748 用例、230007 断言，0 error、66 个失败断言、11 个历史失败用例；共同用例名称、顺序与失败签名一致 |
| 主套件计数边界 | 相邻版本为 230055，成功断言总数减少 48，尚未逐项归因；不宣称全部断言路径相同，不把预期退出 66 写成全通过 |
| fast-disabled helper | 既有两个用例、17178 断言通过 |
| call 持久化 | static/AOT-static/indirect/AOT-indirect 的既有 checks 和目标 spill/miss 标记保持 |
| lifetime | constant-window fast0/1 各 123485 checks；state-sink-failure fast0/1 各 2020998 checks、41 个实际 C++ 请求注入，标记与相邻参照相同 |
| Function/Block CoreMark 与 bench | 四次实际模式、CRC/checksum 与 overflow 检查通过，五类 spill 指标相同 |

Function CoreMark 的有序记录数 427→426、host bytes 286528→286436；Block CoreMark 为 2419→2416、205260→204972，差异均落在已记录的 formatter 符号内。两个 bench 的完整有序记录与 host bytes 保持相同。Block 原始 dump 的长度、自定义 FNV 与 SHA 均重新核对；CoreMark 的七处 CPUID 仅按先前同一参照日志的目标证明、未改变的完整 DecodeCpuid 文本及当前 ELF 符号，归一化四条 MOV 的立即数字段，其余共同 PC 字节相同；Block bench 完整原始顺序相同。formatter 的未匹配记录和推断 load bias 限制仍单列，不扩展为完整程序等价性证明。

五组可见工作区的九类缓冲均闭合，请求/释放次数、请求/释放字节、live 与 peak 六项合计保持。两个 bench 与 IR slow 的所有记录字段相同；Function CoreMark 的 retention events 17552→17537、pattern init 569740→569620，Block CoreMark 为 22329→22308、821296→820772。IR slow 仍为 390 次分配/释放、6679684 bytes、live 0、peak 3981816。没有据此推断未计量的私有容量与完整动态成本。

### 构造代码与存储成本

| 项目 | 相邻参照 → 本批 |
| --- | --- |
| launcher .text | 4536468→4509948，减少 26520 bytes |
| 108 个匹配创建主体 | code 130464→112712，减少 17752 bytes；按完整模板参数前缀匹配，枚举包装与凭据主体分开记录 |
| 匹配主体自身 SP 调整 | 全部保持相同，最大可见自身减量 192 bytes；不代表传递或动态栈峰值 |
| 额外动态枚举包装 | 一个 Block::AppendInst 的 Uniform/Value 参数包入口，372 bytes/自身 SP 96 bytes；正常路径在恢复 96-byte 帧后尾跳到共享主体，该具体路径不叠加这两个自身帧 |
| outlined 参数门 | launcher 上一批三个 196/212/204-byte CheckArgumentPack 符号不再存在；动态检查仍以内联形式存在，凭据主体没有直接参数门分支 |
| 元数据 | IRMeta 48 bytes、表 9312 bytes 保持；构建前后完整 ir_meta.cpp.o 的 SHA 相同，继续无元数据全局初始化 |
| 既有布局 | 所测 21 个公共/owner 布局及 PendingStore 32、PublishedCapture 24 bytes 均保持 |
| 完整 debug ELF | 122730784→121946864 bytes；不将文件减少等同运行时加载成本 |
| 其余分区 | rodata、bss、data.rel.ro、init_array 保持；异常表、eh_frame 与全部 debug 分区差异另存完整记录 |

launcher 冻结产物 SHA 为 `cb9d153a4d95f244655b47defe16c4f9522f92642386c3c27c5ee8a94b62f57b`，源码 overlay SHA 为 `8fd41e6d83e7a6b82e83c8abff477e3160a348d1536268614b224c5eb3e29bb5`，本批 source patch SHA 为 `df49da158eef5c611cdf27f06f28c9a4f675934755b9dd0892ac42829da9a072`。此次没有 PMU 采样，不据 .text/自身 SP/可见缓冲宣称完整编译周期、稳态性能或完整成本门通过。

### RV64 与未完成范围

RV64 v28 在 2552 项完整只读源码快照上构建十四目标成功，覆盖同一本批 472 项 overlay。相对已验收 v27 九个 ELF 变化，十九项受影响用户态 CTest 新执行通过；十三项用户态与五项 guest 内核按完整相同 ELF、注册命令/属性、镜像/QEMU 及原始执行记录复用。强 fence TSO 使用变化后的实际 v28 ELF 新执行，八组编码、二十项各 5000 轮 litmus、八组 software CAS128 共 220321 checks 通过。两个 compile-cost 可执行程序和 emit 仅构建，三十七项仍仅 RV64 专用注册范围；没有新 guest 内核启动、物理 RV64 或硬件内存模型证明。确认仅剩 sleep 的自有 v28 容器已停止，未处理其他容器。

本批三十二项范围内检查通过，证据入口为 `r2-known-construction-v1-quality.json`、`-source-manifest.json`、`-plan.json`、`-test-summary.json`、`-construction-cost-comparison.json`、`-metadata-object-comparison.json`、`-storage-comparison.json`、`-host-byte-comparison.json` 与 `r7-rv64-execution-20261024-v28-test-summary.json`，均位于 /private/tmp/swiftvm-middle-results。原始 ELF、命令、log/XML、汇编、布局和哈希随记录保存。

具名路径重复进入动态签名门这一项已落实；完整 R0–R7 继续开放。继续 opcode 位宽/值域/转换与 CFG/Phi、其他候选/工作区及初始化成本、guest-state/fault/观察和并发矩阵，以及分别适用 Block/Function 的固定源码 FEX 与正式成本/PMU 验收。单批构造代码减少不能替代这些交付。

## 标量参数值域与解释器准入（2026-10-24）

本批从 C++ 参数包约束继续接入实际值约束。最终冻结批次 `r2-scalar-contracts-v4`，相邻参照 `r2-known-construction-v1`，源码摘要 `20529557213d76e0ce36eb13ed6d7868f7ee53c3c88985f5127ba2e5f3e351c7`。修改十个既有 C++ 文件和两份文档，新加一个有明确边界 oracle 的 IR 用例并扩展既有执行用例；没有新增源文件、构建注册、临时 probe 或测试脚本。472 项源码覆盖及累计 113 个退役路径保持。

### 公共规则与阶段边界

- `ir_types.h` 提供 IsScalarIntegerType/IsScalarBitWidth 纯谓词；`instr.h/.cpp` 的 HasValidScalarArguments 仅在签名和嵌套视图已检查后读取固定字段，不再枚举 uses，不保存参数或类型快照。默认不为未覆盖 opcode 添加新限制。
- 覆盖十四项：三种 BitExtract/BitInsert/BitClear、ByteSwap、Crc32c、ZeroExtend32/ZeroExtend32To64/ZeroExtend64/SignExtend、四种 CountLeading/TrailingZeros32/64 和 PopCount。要求这些操作的输入/结果为标量整数视图；位域与 CRC 的第二数据输入也检查。32/64 位计数接受同宽有符号别名，显式结果不必等于默认 token。
- 位域位置和长度必须是整数 Imm，lsb<64 且 width≤64-lsb；先界定位置再减法，拒绝大值/溢出组合。零宽 extract 返回 0，insert/clear 保持原值。ByteSwap selector 为 16/32/64，CRC 为 8/16/32/64，按完整值读取，拒绝高位非零但低 32 位貌似有效的选择值。
- ARM64 SupportsIR、封口 SSA/liveness 与 RV64 Prepare/Translate 继续共享 CheckArguments。优化前、构建后与封口后的不同生命周期检查均保留；构造凭据仍只证明 C++ 种类/数量/容量。基础域不等于完整类型/宽度、转换方向、全部窄读取或 CFG/SSA 证明。
- `riscv64/jit/translator_scalar_bits.cpp` 的三处晚期范围拒绝退役，FieldMask 只计算已准入范围。核对未改动的 WidePlan：这十四项不新生成或拆分，普通标量克隆保留读取视图、结果与完整 Imm；该限定属性不替代所有 lowering 的证明。native ABI、Phi/local、条件、uniform span 和 pair 检查保持。
- `arm64/jit/translator_alu_scalar_bits.cpp` 补齐零宽三分支；`translator_alu_scalar.cpp` 使宽结果的 ByteSwap16 明确清除高位，并允许计数的同宽 signed 输入；必要局部防御仍保留。
- `interp/interpreter.cpp` 把共同检查并入原最大编号/宽向量预扫，嵌套 Value 同次判定 V256，Uniform/Local 按合法 opcode 查询；旧五槽宽度循环删除。非法输入在 stack 分配和执行前返回 IllegalCode。单条 fallback 沿用已验证输入约定。

### 集中验证与修正记录

v1 在首次构建发现两处 VIXL W/X 条件表达式类型错误及一处 const Value 的 use-site view 写法，未上机执行。修正后的 v2 快慢 IR 通过，主套件新增 oracle 出现一处失败，批次随即停止：夹具在 TranslateIR 已释放指令后才运行解释器，输出保持哨兵值。依据 runtime 的 DestroyInstrs 回收路径，v3 改为先消费解释器参考，再编译/执行 JIT；生产回收策略保持。仅 swift_test ELF 改变，v3 六项新执行，六项按完整相同 ELF、参数/环境及原始日志复用。随后成本核对发现新 helper 的空定义诊断带入固定帧，并补齐非整数位域立即数拒绝，形成最终 v4；这些实际修改后才重跑相应验证，未逐文件反复执行套件。

最终 Android 十四目标完整构建成功，十二项 native 与四项普通负载均为本批新执行；红魔 NX729J（bf1359ff）的五个使用 ELF 与两个 guest 输入在前后完整哈希一致，二十份原始 log/XML 已校验回收。

| 验证 | 最终 v4 结果 |
| --- | --- |
| 快/慢 IR | 各 105 用例、81985/82083 断言，无失败。相邻各加一例/280 断言，其中新 scalar 单元 154，Block/Function 准入及解释器拒绝矩阵 126 |
| 新负例 | 两种单元各九种非法形状；含死生产者且开启 ConstantFolding/DCE，优化前即拒绝。sealed SSA 无需 FunctionCFG；解释器在执行前返回 IllegalCode |
| 新执行 oracle | 既有 scalar-bit 生产用例增加八种操作、五组边界输入，先解释后实际 ARM64 JIT；禁用折叠，覆盖零宽三操作、wide ByteSwap16 清高位和四种 signed-view 计数。共 168 个明确新增断言 |
| 主套件 | 748 用例、230364 断言，0 error、66 个失败断言、11 个历史失败用例；共同名称、顺序和失败签名相同。相邻总数 +357，其中上述 168 已对应，其余 +189 尚未逐项归因；不宣称全部断言路径相同 |
| helper 与持久化 | fast-disabled 两例/17178 断言通过；static/AOT-static/indirect/AOT-indirect 的既有 checks、spill/miss 标记保持 |
| lifetime | constant-window fast0/1 各 123485 checks；state-sink-failure fast0/1 各 2020998 checks/41 个实际 C++ 请求注入，标记相同 |
| 四组普通负载 | Function/Block 的 CoreMark 与 bench 实际模式、CRC/checksum/overflow 通过，五类 spill 指标保持 |

Function CoreMark 记录数 426→427、host bytes 286436→286528；Block CoreMark 为 2416→2412、204972→204384；差异均在已记录的 formatter 内。两个 bench 的完整有序记录与 host bytes 相同。Block 原始 dump 的长度、自定义 FNV、SHA 均核对；CoreMark 七处 CPUID 只按同一参照日志的目标证明、未改变的完整 DecodeCpuid 文本和当前 ELF 符号，归一四条 MOV 的立即数字段，共同 PC 的其余字节相同。Block bench 原始有序字节相同。未匹配 formatter 与推断 load bias 的限制继续单列，不扩大为完整程序等价。

五组可见工作区的九类缓冲均闭合，请求/释放次数、请求/释放字节、live 与 peak 六项合计保持。两个 bench 与 IR slow 全字段相同；Function CoreMark retention events 17537→17552、pattern init 569620→569740，Block CoreMark 为 22308→22257、820772→817224。IR slow 仍为 390 次分配/释放、6679684 bytes、live 0、peak 3981816。私有堆、保留时间和完整初始化成本未由此验收。

### 成本与兼容性

| 所测项（Android launcher ELF） | 相邻参照 → 最终候选 |
| --- | --- |
| launcher .text | 4509948→4513036，增加 3088 bytes |
| 新 scalar 主体 / field helper | code/SP 为 1196/16、348/0 bytes；无 fmt/AssertFailed 调用。中间 v3 为 1864/96、480/0，最终正常拒绝路径避免空定义格式化固定帧 |
| SSA 收集 lambda | 4844/464→4856/464；Use 496/112、Define 432/64 保持 |
| HasValidArguments | 1452/96→1480/96 |
| Interpreter::Run 整块入口 | 2124/224→3480/272；增加 1356 code bytes 与自身 SP 48 bytes，动态 alloca 与完整栈链另计 |
| Interpreter::Run 单条入口 | 2892/16 保持 |
| launcher 内 RV64 EmitScalarBits 主机函数 | 5004/224→4788/208；此表是 AArch64 主机代码，不是 RV64 发射产物的性能结论 |
| 108 个共享创建主体 | code 总计 112712、自身 SP 全部保持；构造期已知签名贯通没有回退 |
| 元数据与 owner | IRMeta 48、表 9312 bytes，继续无元数据全局初始化；原始对象哈希因调试信息改变，strip-debug 后完整对象相同。所测 21 个布局、PendingStore 32/PublishedCapture 24 bytes 均保持 |
| 完整 debug ELF | 121946864→121974496 bytes；其余分区另存完整记录，不等同运行时加载/编译成本 |

本批未作 PMU 采样；新检查的动态调用成本、解释器入口成本及传递/动态栈继续计入未完成成本门。launcher SHA 为 `131f799f102ab27e0a5d1c5aff629f9089e6691585692ab6d685dd5fe7816052`，源码 overlay SHA 为 `9b9e1f330a8cbd4f71cd8012fd317c36679222490317496f17ae8782160327e3`，本批 patch SHA 为 `44ad96fb1f6b080b1c4c824abcce23fb1174b678f06dda1f9018ea22cb9f5925`。

RV64 最终 v32 在 2552 项只读完整源码上构建十四目标，匹配本批 472 项 overlay。相对已验证 v31 九个 ELF 变化；十九项受影响用户态 CTest 新执行通过，十三项用户态/五项 guest 内核按完整相同 ELF、注册命令/属性、镜像与 QEMU、原始记录复用。变化后的 v32 强 fence TSO 新执行八组编码、二十项各 5000 轮 litmus 和八组 software CAS128，共 220321 checks 通过。v29 仅失败构建，v30 仅成功构建，v31/v32 的执行来源分开记录；两个 compile-cost 与 emit 仅构建，三十七项仍只覆盖 RV64 专用注册。没有新增 guest 内核启动、物理 RV64 或硬件内存模型证明。确认只剩 sleep 后停止本批自有 v29–v32 容器。

三十一项范围内检查通过。证据入口位于 /private/tmp/swiftvm-middle-results：`r2-scalar-contracts-v4-quality.json`、`-source-manifest.json`、`-test-summary.json`、`-cost-comparison.json`、`-selected-frames.json`、`-metadata-stripped-comparison.json`、`-storage-comparison.json`、`-host-byte-comparison.json` 及 `r7-rv64-execution-20261024-v32-test-summary.json`。源码/文档改动前快照、各次构建/执行日志、完整 ELF、XML、汇编和哈希均保留。

完整 R0–R7 继续开放。十四项基础域不等于任意 scalar 源/结果位宽、窄读取规范化和转换方向证明；其余算术、向量/FP、地址、CFG/Phi 与后向 label、guest-state/fault/观察/并发矩阵、候选及工作区成本和固定 FEX/正式 PMU 仍须接续。

## 标量寄存器宽度统一与 S64 截断修复（2026-10-25）

本批最终为 `r2-scalar-register-widths-v2`，相邻参照 `r2-scalar-contracts-v4`，源码摘要 `28f297acb55d40d8c33812833f6b937ed8eb43faacc2cd203a58efff27b67e57`。修改八个既有源文件和两份文档，新增一个永久执行回归并扩展既有 IR 用例；472 项源码覆盖、累计 113 个退役路径保持，没有新增源文件、构建项、临时 probe 或仓库测试脚本。

### 改动与语义边界

- `ir/ir_types.h` 增加同宽整数谓词，接受 U/S 的 8/16/32/64 位配对；非法枚举、浮点/向量或其他宽度返回 false。标量参数、ARM64 和解释器的零计数断言共用该谓词。
- `arm64/jit/jit_context.cpp` 的 R、RForWrite、GetTmpGPR 原先只把 U64 识别为 X，导致 S64 选择 W。现在同宽有符号整数保留高三十二位。永久回归在生产修复前已实际观察到 `S64(0x100000000)+1` 的 JIT 输出为 1，独立解释器与预期为 0x100000001；原始失败 ELF/XML/命令保留。
- `compile/rules/value_rules.cpp` 删除先查大小的本地 ScalarInteger，先用共同整数域拒绝 VOID/FP/非法类型，再查合法整数大小，避免无效枚举进入大小查询。
- raw BitCast/GetResult 契约未扩围：解释器原始槽位运输、ARM64 别名及 RV64 的 raw 写入仍保持。不能由本次 W/X 修复推导所有窄类型读取应统一截断。后续需分别证明逻辑读取、原始位运输、转换方向与各 opcode 的关系。

### 已完成的集中验证

Android 十四目标完整构建成功。新增永久回归先执行解释器，再交给会释放 IR 的 JIT 编译；独立 Block 和合格单块 HIRFunction 各覆盖 S64/U64/S32/U32、十类操作与六组边界输入，共 2000 个断言通过。该覆盖不代替通用多块 Function 的 CFG 证明。

首次新增用例构建缺少直接 builder include，补齐后才执行修复前复现。v1 的集中执行随后因测试把逻辑 Not 当作位取反而中止；v2 仅修正独立预期为 `a == 0 ? 1 : 0`，生产代码不变。完整重建确认只改变主测试 ELF；最终六项 native 新执行，快/慢 IR 与四个 lifetime 模式按完整相同 ELF、参数/环境和原始日志复用 v1。未执行的 v1 后续项目不追认为已运行。

| 验证范围 | 最终结果 |
| --- | --- |
| 快/慢 IR | 各 105 用例、82123/82221 断言通过；各新增 138 个类型/位宽和无效折叠输入断言，实际来源为相同 ELF 的 v1 执行 |
| 主套件 | 749 用例、232195 断言、0 error；仍为 11 个历史失败用例、66 个失败断言，共同名称、顺序及失败签名相同，新例通过 |
| 断言总量边界 | 相邻总数增加 1831；永久新例明确增加 2000，其余 −169 尚未逐项归因，不声明全部成功断言路径相同 |
| helper/调用持久化 | helper slow 两例/17178 断言、static/AOT-static/indirect/AOT-indirect 本批新执行通过，既有 spill/miss 标记保持 |
| lifetime | constant-window fast0/1、state-sink-failure fast0/1 的相同 ELF/参数及原始成功日志复用；后者每模式 41 个实际 C++ 请求注入记录保留 |
| 四组普通负载 | 红魔 NX729J（bf1359ff）上 Function/Block 的 CoreMark/bench 新执行通过，CRC/checksum/overflow 与五类 spill 指标保持；使用 ELF/guest 输入前后完整哈希相同 |

Function CoreMark 有序记录 427→426、host bytes 286528→286436，差异仅在已记录 formatter；其余三组完整有序记录保持。Block CoreMark 的 2412 条/204384 host bytes、两个 bench 的记录与 host bytes 保持。两份 Block 原始 dump 的长度/FNV/SHA 重新核对，bench 完整原始顺序相同；CoreMark 七处 CPUID 仅凭同参照日志的 typed target、完整未改 frontend/DecodeCpuid 与当前 ELF 符号证明，归一四条 MOV 的立即数字段，其他共同 PC 字节相同。未匹配 formatter 与推断 load bias 的边界仍保留，不扩展为完整程序/Function/轨迹等价。

五组可见工作区的九类缓冲闭合，请求/释放次数、字节、live 与 peak 六项合计保持。Function CoreMark retention events 17552→17537、pattern init 569740→569620，其余四组全部字段保持。私有堆、保留时间与整体初始化成本继续开放。

### 静态成本与 RV64 范围

| Android launcher 所测项 | 相邻参照 → 最终候选（bytes） |
| --- | --- |
| .text / 完整 debug ELF | 4513036→4512892（−144） / 121974496→121973952 |
| JitContext::R code/自身 SP | 612/128→616/128 |
| RForWrite code/自身 SP | 336/112→340/112 |
| GetTmpGPR code/自身 SP | 112/64→124/64 |
| FoldIntegerImmediate code/自身 SP | 548/96→520/96 |
| FoldWidthValue code/自身 SP | 412/160→328/144 |
| 108 个创建主体 | code 总计 112712、自身 SP 均保持 |
| 元数据与布局 | 21 项 owner、PendingStore 32/PublishedCapture 24 保持；IRMeta 48、表 9312，无全局初始化，strip-debug 后完整对象相同 |

v2 launcher 与 v1 完整哈希相同，因此复用原汇编/布局/section 输入；主测试已使用修正后的新 ELF。完整分区和所测变化函数另存记录。自身固定帧与代码体积不代替传递/动态栈、整体编译成本或 PMU；本批未作硬件 PMU 采样。

RV64 最终 v34 在 2552 项完整只读源码上构建十四目标，匹配本批 472 项 overlay。v33 仅构建；v34 的完整构建无新编译任务后，按相对已验证 v32 的产物变化执行十九项用户态 CTest，全部通过；十三项用户态与五项 guest 内核按完整相同 ELF、注册命令/属性、镜像/QEMU 和原记录复用。v34 强 fence TSO 新执行 220321 checks 通过。两个 compile-cost 与 emit 仅构建，三十七项是 RV64 专用注册范围，未新增 guest 内核启动或物理 RV64 验证。核对自有容器 ID/镜像且只剩 sleep 后，已停止 v33/v34。

范围内二十六项检查通过。证据位于 /private/tmp/swiftvm-middle-results：`r2-scalar-register-widths-v2-quality.json`、`-test-summary.json`、`-cost-comparison.json`、`-selected-frames.json`、`-metadata-object-comparison.json`、`-storage-comparison.json`、`-host-byte-comparison.json`、修复前失败与修复后 focused XML，以及 `r7-rv64-execution-20261025-v34-test-summary.json`。构建失败、v1 测试预期失败和最终原始结果均保留。

完整 R0–R7 继续开放。下一步优先接续逻辑位宽读取与 raw 运输、转换方向及其余 scalar/vector/FP/address 契约，然后继续 CFG/Phi/后向 label、准备候选与工作区成本、guest-state/fault/观察和运行时矩阵，以及固定 FEX 的正式 PMU 验收。

## 整数转换输入视图与共享位宽查询（2026-10-25）

最终批次 `r2-integer-conversion-views-v2`，相邻参照 `r2-scalar-register-widths-v2`，源码摘要 `36c3a7c75f3d5d474ca25ad47a489a955a91e866f92ebe26c4dd9733e5d085c1`。修改十个既有源文件和两份文档，各新增一个永久 IR/原生执行用例，并扩展 RV64 既有 scalar 测试；472 项 overlay、累计 113 个退役路径保持。没有新增源文件、构建注册、临时 probe 或仓库测试脚本。

### 共享规则与后端边界

- `ir/ir_types.h` 的 GetScalarIntegerBits 给出 U/S 的 8/16/32/64 位宽，其他类型返回 0；同宽谓词复用该查询。IntegerConversionInputBits 接收 opcode 和输入位宽：ZeroExtend32/ZeroExtend32To64 先限制到 32，其余两项保留输入视图宽度。它也允许分析中的 1–64 位部分事实，不保存额外表/缓存或 IRMeta 字段。
- `ir/ir.inc` 明确数值契约：零扩展读取视图内的位；SignExtend 复制输入视图的符号位；低层显式整数结果覆盖保留结果宽度的低位，因此收窄也有定义。具名 frontend 工厂仍按原默认结果规则构造，不用低层覆盖替代专门的 ZeroExtend32To64。
- `compile/analysis/value_bits.cpp` 删除本地 IntegerWidth 和先查大小/浮点的重复分类，四处改用共同整数宽度查询；零扩展事实复用同一输入位宽上限。decode/pass 继续借用既有 FoldWidthValue，不另建常量求值副本。
- `interp/interpreter.{h,cpp}` 的四个具名入口共用 RunIntegerConversion，在读取的数值副本上先取输入有效位，再按需要符号扩展，最后由既有写入处理结果宽度。ReadScalar 和 raw BitCast/GetResult 未整体修改；其他算术/比较/移位读取继续单独接续。
- `riscv64/jit/translator.cpp` 将转换从 Neg/Test 家族分出，在结果寄存器上掩码或符号扩展。只有零扩展的输入位数不超过结果位宽，或符号扩展的结果为 64 位时，才证明结果已规范化，避免重复结果掩码。cached 与未缓存两种模式均有独立数值 oracle。
- `arm64/jit/translator_alu_scalar_bits.cpp` 三处读取共同转换宽度；SignExtend 的 64→32 改为 W/W 移动。32 位转换的宽结果即使与源同寄存器，也需要 W 写清高半；residence 的零位证明按实际被复制的位宽查询。必要宽度/分配省略证明及独立 retry 保持。

### 复现、优化开关与集中验证

仅加入永久新例时，未修改的生产代码已实际失败：直接 U8 视图读取 0xabcdef0080008000，零扩展到 U16 应为 0，解释器却得到 0x8000。修复前 ELF/XML/命令完整保留。

v1 完整构建 Android 十四目标后，十二项 native 和四组普通负载新执行。直接发射矩阵通过后，为覆盖可能跳过普通 emitter 的宽度省略，v2 仅给永久用例加入优化开/关两种模式；生产源码和十三个 ELF 不变，仅 swift_test 改变。完整重建成功后，只新执行主套件和 helper slow；另外十项 native、四组负载及静态成本按完整相同 ELF、输入/参数/环境和原始记录复用 v1。没有为本批重复执行无变化的产物。

| 验证 | 最终范围与结果 |
| --- | --- |
| 原生转换矩阵 | Block/合格单块 HIRFunction × 优化开/关 × 八种源/结果整数类型 × 四类转换 × 直接读取视图/BitCast/GetResult × 来源保留/可复用，两种执行引擎各八组边界值，共 251904 个明确新增断言通过 |
| 来源观察范围 | 保留模式在转换后比对源的低 32 位原始内容；可复用模式允许源死亡，覆盖结果同寄存器时的清位。该观察不作为完整 16-byte raw 运输证明 |
| 快/慢 IR | 各 106 用例、94411/94509 断言通过，实际执行来源为相同 ELF 的 v1；各新增 12288 个完整参数准入及独立逐位常量 oracle 断言 |
| 主套件 | v2 实际执行 750 用例、483862 断言，0 error；仍为 11 个历史失败用例、66 个失败断言，共同名称/顺序/失败签名相同 |
| 断言总量边界 | 相邻总数增加 251667，明确新增 251904；其余 −237 尚未逐项归因，不将失败签名相同解释为全部成功断言路径相同 |
| helper/持久化/lifetime | v2 helper slow 两例/17178 断言通过；static/AOT-static/indirect/AOT-indirect、constant-window fast0/1 和 state-sink-failure fast0/1 的相同 ELF/原始成功记录复用 v1，后者每模式 41 次真实 C++ 请求注入保持 |
| 红魔普通负载 | NX729J（bf1359ff）的 Function/Block CoreMark、bench 在 v1 新执行，v2 复用同一 launcher ELF；CRC/checksum、实际模式、overflow 与五类 spill 指标保持，使用产物及 guest 输入前后完整哈希一致 |

Function CoreMark 有序记录 426→427、host bytes 286436→286528；Block CoreMark 为 2412→2415、204384→205100，差异限于已记录 formatter。两个 bench 的完整有序记录及 host bytes 保持。两份 Block 原始 dump 长度/FNV/SHA 重核，bench 完整原始字节相同；CoreMark 七处 CPUID 只凭同一参照日志的 typed target、未改完整 frontend/DecodeCpuid 和当前同一 ELF 符号证明，归一四条 MOV 的立即数字段，其余共同 PC 字节相同。未匹配 formatter 与推断 load bias 的限制保留，不扩展为完整程序/Function/轨迹等价。

五组可见工作区的九类缓冲闭合，请求/释放次数、字节、live 与 peak 六项总计保持。Function CoreMark retention events 17537→17552、pattern init 569620→569740；Block 为 22257→22285、817224→820948。两个 bench 与 IR slow 全字段相同。私有堆、保留时间及整体初始化成本继续开放。

### 静态成本及 RV64 来源

| Android launcher 所测项 | 相邻参照 → 候选（code/自身 SP，bytes） |
| --- | --- |
| .text / 完整 debug ELF | 4512892→4512364（−528） / 121973952→121968784 |
| 解释器转换执行主体 | 原四个出线主体 code 合计 1932、各自身 SP 112；现仅共享 RunIntegerConversion 692/112，具名入口仍为 dispatch 薄封装 |
| 解释器单条 Run / 标量参数主体 | 2892/16→2844/16；1196/16→1180/16 |
| ApplyIntegerWidth / IntegerValueAnalysis::Query | 456/80→472/32；924/160→880/144 |
| ARM64 SignExtend / ZeroExtend32 | 1120/176→1232/176；1552/192→1572/192 |
| ARM64 ZeroExtend64 / ZeroExtend32To64 | 768/144→840/160；1216/160 保持 |
| RV64 EmitScalar 的 AArch64 主机函数 | 5980/240→6488/240；不是 RV64 产物的硬件性能结论 |
| RForWrite | 340/112→348/112 |
| 创建主体、元数据与布局 | 108 个创建主体 code 合计 112712、自身 SP 保持；21 项 owner 与 PendingStore 32/PublishedCapture 24 保持，IRMeta 48、表 9312，无全局初始化，strip-debug 后完整对象相同 |

完整分区、其余所测变化函数和汇编保存在记录中。ZeroExtend64 的自身帧增加 16 bytes 与 RV64 主机函数代码增加 508 bytes 明确保留；总体 .text 减少不替代完整编译周期、私有存储、传递/动态栈或 PMU 门。本批未进行 PMU 采样。

RV64 v35 在 v1 的 2552 项完整只读源码上构建十四目标，相对已验证 v34 九个 ELF 改变；十九项受影响用户态 CTest 新执行通过，其中原始输出含转换矩阵的 RV64G/Zbb 通过标记，十三项用户态/五项 guest 内核按相同产物和环境复用。v35 强 fence TSO 新执行 220321 checks 通过。最终 v36 在 v2 完整源码再次构建十四目标，无新编译任务且十四 ELF 全部相同，因此三十二项用户态、五项 guest 内核及 TSO 均按原始实际来源复用，没有调用空 CTest 或追认为新执行。两个 compile-cost 与 emit 仅构建，三十七项仅 RV64 专用注册范围；没有新增内核启动、物理 RV64 或硬件内存模型证明。核对 ID/镜像且只剩 sleep 后，已停止自有 v35/v36。

三十四项范围内检查通过。证据位于 /private/tmp/swiftvm-middle-results：`r2-integer-conversion-views-v2-quality.json`、`-source-manifest.json`、`-test-summary.json`、`-cost-comparison.json`、`-reused-evidence.json`，v1 的修复前失败、集中原始归档与汇编/存储/host-byte 记录，以及 `r7-rv64-execution-20261025-v35-test-summary.json` / `v36-test-summary.json`。所有实际执行与复用来源分开记录。

完整 R0–R7 继续开放。优先接续其余窄算术/移位/比较的逻辑读取与 raw 运输边界，再推进向量/FP/地址、flags/effects、CFG/Phi/后向 label、候选/工作区与完整成本、guest-state/fault/观察/运行时矩阵，以及固定 FEX 的正式 PMU 验收。

## 整数移位语义、共享位分析与三后端收拢（2026-10-25）

最终 `r2-integer-shift-contracts-v4`，相邻 `r2-integer-conversion-views-v2`，源码摘要 `6488cffcb06df652b6375bc9ffddd4d3c0cb6c782c1039fc88111a36ae4fffc8`。十九个既有源文件和两份文档修改，新增两个永久 IR 用例、一个永久原生用例，扩展既有 RV64 测试。overlay 472→473 是首次纳入既有受版本控制的 tests/riscv/test_support.h，累计 113 个退役路径保持。没有新增生产源文件、构建注册、临时 probe 或仓库测试脚本。

### 文件职责及 decode/pass 边界

| 文件 | 最终职责 |
| --- | --- |
| `ir/ir_types.h` | 纯移位分类、有效输入宽度及计数归一；不存入节点、元数据或 facts |
| `ir/ir.inc`、`ir/instr.cpp` | 声明数值语义；共有基础整数域准入由十四项扩大至二十二项 |
| `compile/analysis/value_bits.{h,cpp}` | 八类窄/混合宽度移位及低计数位推导；查询与 worklist 通知同范围，不新增图或缓存 |
| `backend/interp/interpreter.{h,cpp}` | 八个薄入口共用 RunIntegerShift；仅规范化数值副本，raw BitCast/GetResult 主体不变 |
| `backend/arm64/jit/translator.h`、`translator_alu_scalar{,_bits}.cpp` | 八个入口归 bits 的共享主体，四个立即数主体从巨型 scalar 文件删除；既有 coverage/ExtractShift 动作优先 |
| `backend/arm64/compile/register_contract.cpp` | 精确 RorValue 为窄/暂存输入计一个 GPR，其他默认变量移位预算保持 |
| `backend/riscv64/jit/translator.{h,cpp}`、`translator_scalar_bits.cpp` | 八项从 EmitScalar 移至 bits 家族；结果寄存器只申请一次，逻辑输入、word/符号归一与 Zbb/基本编码分开 |
| `tests/ir_contract_test.cpp`、`scalar_bit_codegen_test.cpp` | 共有准入、位事实、builder/改写反馈及独立逐位解释器/JIT oracle |
| `tests/riscv/{native_scalar_test.cpp,test_support.h,backend_test.cpp}` | RV64G/Zbb 永久矩阵、缓存开关、raw/共享计数与保留检查，独立案例批量编译，静止后释放代码 |

运算位宽取整数结果类型，读取 min(输入视图, 结果) 位；逻辑/循环移位先零扩展，ASR 复制有效输入的最高位，不凭 signed token 改变。Imm/Value 计数均对结果位宽取模，窄/混合结果及完整 64 位立即数有定义。x86 byte/word shift 既有 32 位显式容器继续承担 guest 规则。

decode 在事实足够时经既有 FoldWidthValue 直接折叠，变量只要求低 3/4/5/6 个计数位确定。残余 pass 在定义/计数改写后经同一转移范围继续传播；不增加 evaluator、pass 或 epoch。未知计数仅保留独立证明的稳定零值/全一等结果。完整 CFG、跨块固定点、物理分配与封口证明仍在原阶段，Block/Function 不混成一种分配策略。

### 修复前证据、夹具收敛与集中验证

仅加入永久新例时，旧生产代码已失败：U8 视图读取 raw 0x180 后 LsrImm 1，解释器为 64、ARM64 为 192。修复前 ELF/XML、实际命令与设备前后哈希保留。

v1 严格 IR 的负例直接创建非法指令，被 checked builder 提前拒绝；v2 改为合法构造后经 RewriteContext 注入非法域，再验证优化前/封口拒绝。v2 主套件在 Catch2 累积保存成功断言/capture 时出现 Scudo 映射不足和 bad_alloc，未认定为生产泄漏。v3 保留全部数值比较，仅失败时展开详情，另用两条 Catch 断言检查总量，红魔执行通过。

RV64 v37、v39 的普通/Block/Zbb 三项各超时 180 秒；仅复用 arena 仍不足。最终 v4 将独立案例批量编译，保留各自 source/count 的使用终点、保留条件及原 PRNG 的 case/cache/sample 顺序，每模式实际 1024 内核、278528 数值比较。所有同步调用结束后 FreeCode，QuiescentRawCache 收尾。未删除输入/计数组合、提高 RV64 超时、关闭缓存变体或绕过生产编译。

v3 对照还暴露 ChaCha 多出十六条清位指令、64 bytes。v4 将窄立即数 LSR/ASR 合成 UBFX/SBFX，超过有效输入位的右移直接得零/符号复制，既有完整矩阵验证该编码路径。实际使用的 ELF 均改变，最终集中执行一轮。

| 验证 | v4 实际范围与结果 |
| --- | --- |
| Android 构建 | 红魔 NX729J（bf1359ff），十四目标完整构建成功；使用的五个 ELF 与 guest 输入前后完整哈希一致 |
| 移位 oracle | Block/合格单块 Function × 优化开/关 × 八种源/结果整数类型 × 八类操作 × 直接视图/BitCast/GetResult × 保留/可复用，55296 程序、2605056 次解释器/JIT oracle 检查（含 halt/translation）；保留模式观察完整 64 位 raw 数据及计数，非完整 16-byte 运输证明 |
| 快/慢 IR | 各 108 用例、100134/100232 断言、0 failure/error；各相邻 +5723，含低计数位事实、非法角色、builder 折叠与 count 改写反馈 |
| 主套件 | 新执行 751 用例、483904 断言、0 error；仍为 11 个历史失败/66 个失败断言，共同名称、顺序和失败签名相同 |
| 计数边界 | 主套件相邻 +42，明确新增 Catch 覆盖断言为 2，其余 +40 未逐项归因；2605056 为含 halt/translation 的 oracle 检查数，不能全称数值比较或混称 Catch 断言 |
| 其他 native | 十二项全部新执行，helper slow 两例/17178 断言、四类 call/AOT 持久化及两模式 constant-window/state-sink-failure 保持原成功标记 |
| 四组负载 | Function/Block CoreMark 与 bench 新执行，CRC/checksum、真实模式、overflow 及五类 spill 指标保持 |

Function CoreMark 有序记录 427→426、host bytes 286528→286436；Block 为 2415→2417、205100→204776，数量/大小差异限于已记录 formatter。两个 bench 的有序记录/host bytes 保持，Block bench 完整原始字节相同。Block CoreMark 七处 CPUID 只凭同一参照日志的 typed target、未改完整 frontend/DecodeCpuid 与实际 ELF 符号归一四条 MOV 立即数字段。

Block ChaCha 0x257a26 的 1180 bytes 中恰有十六个指令字由 LSR W#8 改为 UBFX W#8#8，其余 raw bytes 相同。相同后续 byte mask、临时寄存器下一次覆盖前无其他读取，证明这些局部 guest 观察一致；两指令本身不是相同 raw 结果，不做任意 relocation/指令掩码归一。Function ChaCha 的 IR/host 为 1720/4336，大小恢复参照，但未采集完整 body 字节。未匹配 formatter、推断 load bias 与完整程序/轨迹等价的限制保留。

五组可见工作区九类缓冲均闭合；四个普通负载的请求/释放次数、字节、live/peak 六项总计保持，两个 bench 全字段保持。Function CoreMark retention 17552→17537、pattern init 569740→569620；Block 为 22285→22309、820948→818408。IR slow 包含新夹具，请求 390→397、字节 6679684→6680068、peak 3981816 保持，不记为生产零开销或完整私有存储证明。

### 静态成本及 RV64 执行来源

| Android launcher 所测项 | 相邻 → v4（code/自身 SP decrement 合计，bytes） |
| --- | --- |
| .text / 完整 debug ELF | 4512364→4506980（−5384） / 121968784→121916824 |
| ARM64 八移位入口 | 原 code 合计 5880、各自身 SP 128–160；现共享主体 3172/272，加八个 4/0 尾跳入口 |
| 解释器八移位入口 | 原八出线主体 code 合计 5748、各自身 SP 128/144；现 RunIntegerShift 1316/160，具名入口保留 dispatch |
| 解释器单条 Run / 标量参数主体 | 2844/16→2732/16；1180/16→1272/32 |
| 位查询 Shift / Definition | 1248/176→1696/256；1964/480→2732/480 |
| ScratchBudget | 2516/160→2796/160 |
| RV64 主机 EmitScalar / 新移位主体 | 6488/240→3796/192，加新 2420/192；为 AArch64 launcher 符号，不是 RV64 硬件性能 |
| 创建、布局与元数据 | 108 创建主体 code 合计 112712、自身 SP 保持；21 项 owner、PendingStore 32/PublishedCapture 24 保持；IRMeta 48、表 9312，无全局初始化，strip-debug 对象相同 |

新共享主体和位查询自身栈增长明确单列，薄入口零帧不代表传递栈下降。完整分区、其余函数及汇编另存。成本仅接受与最终冻结 v4 ELF 完整 SHA 相同的测量；误选 v1 launcher 的旧测量已标记拒绝并保留。总体 .text 减少不替代私有容量、保留/初始化、传递/动态栈、完整编译周期或 PMU 门，本批未采 PMU。

RV64 最终 v40 对应本批 2552 项只读输入、473 项 overlay，十四目标构建通过，相对已验证 v36 九个 ELF 改变。十七项用户态 CTest 新执行通过；十五项用户态/五项 guest 内核按完整相同 ELF、命令/属性、镜像/QEMU 及原记录复用。其中 initialization/pure-flags 两项实际成功来源为 v37，保留该轮外层退出 8 及其他三项超时；其余十三项与五个内核经 v36 接续实际来源。

v40 普通/Block/Zbb 输出各确认 1024 kernels、278528 values，约 74.68/75.10/39.73 秒，均在原 180 秒内；此为夹具完成证据，非翻译器性能基准。强 fence TSO 在 v40 新执行 220321 checks，通过 8 组 encoding、20 组 litmus、8 组软件 CAS128，有限 QEMU 回归不作为物理内存模型证明。两个 compile-cost 与 emit 仅构建；三十七项仅 RV64 专用注册范围，无新 guest 内核启动或物理 RV64 验证。

v38 仅冻结源码；v39 超时记录实际运行容器为 v37，同镜像/共享 ELF 的关系另存，未改写来源。最终 v40 使用自己的源码/构建/执行容器。自有 v37/v39/v40 核对 ID、镜像及仅剩 sleep 后已停止，其他容器未处理。

本批 37 项范围检查通过。证据在 /private/tmp/swiftvm-middle-results：`r2-integer-shift-contracts-v4-quality.json`、`-test-summary.json`、`-cost-comparison.json`、`-metadata-object-comparison.json`、`-storage-comparison.json`、`-host-byte-comparison.json`、`-chacha-shift-proof.json`，及 `r7-rv64-execution-20261025-v40-test-summary.json`、`-tso-execution.json`。修复前失败、夹具失败、报告内存失败、超时与测量拒绝记录均保留。

完整 R0–R7 继续开放，接续其余整数/向量/FP/地址与 flags/effects、CFG/Phi/后向 label、候选/工作区私有成本、guest-state/fault/观察及运行时矩阵和固定 FEX 正式 PMU。当前有界查询及单批成本不能替代这些交付。

## 整数谓词、条件视图与三后端收拢（2026-10-25）

最终 `r2-integer-predicates-v5`，相邻 `r2-integer-shift-contracts-v4`，源码摘要 `f61431ebacb620e55ba7115cf7844a087dd02ce55b1ba562acbeb59a6e4d902f`。本批修改 37 个既有源文件；overlay 473、累计退役 113 路径保持。新增三个永久 IR 用例、四个永久原生用例，加强既有缓存/RV64 用例，没有新增生产文件、构建注册、临时 probe 或仓库测试脚本。

### 文件职责与阶段边界

| 文件（runtime 下省略前缀） | 本批职责 |
| --- | --- |
| `ir/ir_types.h`、`ir/instr.{h,cpp}`、`ir/ir.inc` | 谓词分类、视图/selector 语义和整数域准入；真实物化 primitive 与 0/1 literal 共用 HasBooleanResult，不新增节点字段/metadata 缓存 |
| `compile/analysis/value_bits.{h,cpp}` | 谓词/选择器有界事实及同范围通知；臂交集、加宽未知，不建新图或逐值缓存 |
| `compile/rules/value_rules.{h,cpp}` | FoldConditionalValue 仅在完整逻辑结果和输出契约允许时提案，参数无效时拒绝，Unknown 保守 |
| `compile/builder/ir_builder.{h,cpp}`、`builder/instruction.cpp` | 原 FoldExtend 归 FoldValue，decode/延期提交共用规则；writer 维护身份、类型、uses 和来源历史 |
| `compile/passes/{values,value_worklist}.cpp` | 残余折叠/级联，条件值候选和通知，保留 publication/flags 边界 |
| `backend/interp/interpreter.{h,cpp}` | ReadBoolean 规范化副本，四类谓词归 RunIntegerPredicate，条件读取共用；raw BitCast/GetResult 不变 |
| `backend/arm64/compile/lower_predicates.cpp` | 删除 primitive/literal 布尔判定副本，共用 Inst 契约，有界 And/Or 递归保持 |
| ARM64 `jit/translator.h`、`translator_alu_scalar_bits.cpp` | 共享 EmitIntegerPredicate、CompareIntegerZero/ConditionRegister；preserving-PSTATE 在结果寄存器清窄视图 |
| ARM64 `translator_alu_scalar.cpp`、`translator_flags.cpp` | 删除旧 Not/TestBit 主体，二参 Not 的 Operand 补位保留 |
| ARM64 `translator_{select,control,terminal,region}.cpp` | 共享条件读取，LocalCondition/直接条件/region/终结/自回边优先级保持 |
| RV64 `jit/translator.{h,cpp}`、`jit_context.{h,cpp}`、`translator_scalar_bits.cpp` | 谓词家族、ReadCondition、局部分支/terminal；条件暂存 t0，避免覆盖别名数据臂，raw Read 不变 |
| RV64 `jit/translator_vector.cpp` | GPR-pair/RVV 选择器也使用 ReadCondition，V256 经既有拆分接续 |
| `backend/code_serial.h` | 代码格式 35→36，拒绝语义修正前产物，AOT 外层布局仍为 6 |
| `tests/{ir_contract_test,scalar_bit_codegen_test}.cpp` | 准入/事实/decode/反馈及结果/raw、flags、局部/Function 边的独立 oracle |
| `tests/riscv/{native_scalar_test,backend_test,native_vector_test}.cpp` | RV64G/Block/Zbb 谓词/别名，raw 向量运输，V128/V256 GPR/RVV 选择器，选择检查前置 |
| `tests/arm64/static_latch_cache_version_test.cpp` | 当前动态谓词产物执行/安装、raw 保留，旧 30/31/32/35 在发布前拒绝 |

TestZero/TestNotZero/一参 Not 按输入视图测试零/非零；TestBit index 在 [0,64)，超出输入视图的位为零，结果 U/S 和宽度不改变真值。只有真实物化 primitive 与 0/1 literal 具有完整 scalar 0/1 保证；普通 known bits、raw alias、guest home 和延期 native condition 不冒充物理证书。条件规范化不施加到数据臂或 Switch；选择结果视图外没有新增保证。

decode 处理当前可证明的谓词/选择器，pass 处理后续改写出现的机会。纯规则不操作节点/uses，writer/worklist 负责身份、通知和寿命；查询预算仍为 32，没有新持久字段。Block/Function 分配及活跃性独立。三块 Function If 的后继真实读取前块 SSA raw 值，独立 Block 后继重载 uniform；解释器仅参考 Block/单块路径，多块 Function 使用独立 marker/raw oracle 和真实 JIT。

### 最终回归与实际来源

修复前永久用例复现 `raw=0x100` 的 U8 TestZero：解释器和 ARM64 都返回 0，独立预期为 1。最终四个 oracle 在优化 None/All、Block/适用 Function、八种整数源/结果、direct/BitCast/GetResult 和保留/可复用形状上通过：

| 永久 oracle | 程序数 | 结果/raw 比较数 |
| --- | ---: | ---: |
| 谓词及完整 scalar 0/1 | 12288 | 331776 |
| 条件视图、数据臂别名和来源保留 | 12288 | 552960 |
| pending flags 与 raw 来源 | 384 | 34560 |
| Goto/NotGoto、Block/三块 Function If | 576 | 14256 |
| 合计 | 25536 | 933552 |

比较失败才进入 Catch，覆盖计数另有 8 条 Catch 断言。flags 矩阵检查 N/Z/V，flags0 另执行同一永久用例，不据此宣称 CF/PF/AF 闭合。格式 36 动态谓词产物执行/安装及旧 35 拒绝通过，缓存用例为 118 checks。

Android 最终十四目标构建通过。十五项 native 中十一项新执行、四项 lifetime 按完整相同 ELF、参数/环境/绑核/timeout 从 v4 实际成功记录复用；四个普通负载新执行，原始归档 21 成员逐字节哈希核对。IR 快/慢各 111 用例、107322/107420 断言，分别相邻 +7188；主套件 755 用例、484181 断言，保留完全相同的 11 个历史失败用例、66 条失败断言、0 error，既有失败签名和共同顺序一致。总断言相邻 +277，其中新覆盖计数 8，其余成功断言差额 269 尚未逐项归因，不能称为全量计数归因或全绿。

RV64 最终 v45 对应 2552 项只读输入、473 overlay，十四目标构建通过。十九项受影响用户态 CTest 新执行通过，十三项用户态/五项 guest 内核按完整相同 ELF、命令/属性、镜像/QEMU 和原成功记录经 v40 复用，保留实际旧来源及外层失败信息。普通/Block/Zbb 各确认 288 kernels、36864 predicate values、36864 selection values、288 branch values；GPR 两入口和 RVV VLEN128/256 各确认 V128/V256 选择器通过。强 fence TSO 新执行 220321 checks，通过 8 组 encoding、20 组 litmus、8 组软件 CAS128。两个 compile-cost 与 emit 仅构建；三十七项仅 RV64 专用注册范围，没有新内核启动或物理 RV64 证明。

v1 的 Operand 临时值构建错误、v2 的当前块/提前折叠夹具错误、v3 的 else/then 顺序和布尔常量代码形状失败、旧 raw BitCast 用例混用真值的错误预期，以及 v4 的 RV64 vector 读取遗漏，均保留原始构建/JUnit/日志与实际来源。最终保留 raw 断言、共享条件契约和全部相关检查。自有 v41–v45 核对 ID、镜像及仅余 sleep 后停止，其他容器未处理。

### 代码、工作区及静态成本

Function CoreMark 426 单元/286436 host bytes、Function bench 22/19896、Block bench 101/16464 的有序形状保持；Block CoreMark 2417/204776→2416/204684，大小/记录差异位于 formatter，spill 与 formatter 外有序大小保持。Block bench 完整 101 段 raw bytes 相同。Block CoreMark 七处单条 CMP Wn,#0→TST Wn,#0xff 经逐字/LLVM 反汇编核对其余 body bytes 相同；另七处 CPUID 仅规范化已证明的四条 typed MOV immediate。零测试是有意的逻辑读取变化，不声称 NZCV 相同，不做任意指令/relocation 掩码归一；未匹配 formatter、未采完整 Function body 和完整程序/轨迹等价的边界保留。

五组可见工作区九类缓冲闭合。四个普通负载的请求/释放次数、字节、live/peak 六项总计保持；Function CoreMark 和两个 bench 全字段保持。Block CoreMark retention 22309→22304、pattern init 818408→818200。IR slow 含新夹具，请求 397→419、字节 6680068→6681460、peak 3981816 保持，不将增量全记为生产开销或零开销。

| Android launcher 所测项 | 相邻 → 最终（code/自身 SP，bytes） |
| --- | --- |
| .text / 完整 debug ELF | 4506980→4508084（+1104） / 121916824→121941248 |
| ARM64 三个 Test* 入口及共享主体 | 原合计 2276、各自身 SP 144–160；现三个 4/0 尾跳入口，加 EmitIntegerPredicate 1468/176，Not 二参路径保留 |
| CompareIntegerZero / ConditionRegister | 新 452/144、468/160 |
| EmitZeroTestPreservingPstate | 648/160→956/176 |
| IsAdjustedBoolean / HasBooleanResult | 1464/128→984/64；新共有判定 172/16 |
| 位查询 Definition / ApplyIntegerPredicate | 2732/480→3072/592；新转移 420/32 |
| FoldExtend→FoldValue / FoldConditionalValue | 1384/224→1488/224；新纯规则 316/96 |
| 解释器谓词主体 / ReadBoolean | 新 952/144、384/96；RunNot 624/112→264/80 |
| RV64 EmitScalar / 谓词 / ReadCondition | 3796/192→3392/192；新 972/160、212/64，为 AArch64 主机符号，不是 RV64 硬件性能 |
| RV64 EmitVector 的 AArch64 主体 | 11832/496 保持，条件调用目标变化另记 |
| 创建、布局与元数据 | 108 创建主体合计 112712、自身 SP 保持；21 owner、PendingStore 32/PublishedCapture 24 保持；IRMeta 48、表 9312、无全局初始化，strip-debug 对象相同 |

薄入口、局部帧降低和共享主体增长分别记录；完整私有容量/堆、保留/初始化、传递/动态栈和编译周期未据此验收，本批未采 PMU。另修正上一批计数术语：2605056 包含 halt/translation，不能全称数值结果比较；原始归档和旧记录标签保留。

本批 33 项范围核对通过。证据位于 /private/tmp/swiftvm-middle-results：`r2-integer-predicates-v5-{quality,test-summary,cost-comparison,storage-comparison,host-byte-comparison,narrow-condition-code-review,reused-native-evidence}.json`、冻结源码/产物与原始归档，及 `r7-rv64-execution-20261025-v45-{test-summary,tso-execution,container-stop}.json`。完整 R0–R7、R2/R7、完整成本和正式 FEX 质量门均未完成；接续其余算术/整数比较、bound-decode 全配置、类型/CFG/Phi、effects/恢复与正式性能验收。

## R2：拆分可变 flags 分析并退役局部 label 哈希（2026-10-25）

本批为 `r2-local-flag-flow-v3`，相邻已验收批次为 `r2-integer-predicates-v5`。源码摘要 `ec1083da28caa80133f64f9825e5f82381ab2c430360aa161ebff629c3f37832`，变更 patch SHA256 为 `0645027c6e4e0733306c9f3c90a39f91a6aad87dcdd41f2d6b7b955a4bc87fa7`。覆盖文件从 473 增至 475，累计退役路径仍为 113。仅修改 CMake、flags pass 和既有 IR 测试三个源文件，新增 `analysis/flag_flow.{h,cpp}` 两个生产文件；没有新增测试目标、命令入口、临时 probe 或仓库测试脚本，缓存格式保持 36。

### 职责与存储边界

- `passes/flags.cpp` 从 939 行缩到 817 行，负责改写、carry 保护、branch-only 和 flags/DCE 调度；纯 transfer、成员校验、terminal 需求及 Function 固定点归 `analysis/flag_flow.cpp`（193 行），短期接口归对应头文件（75 行）。
- 独立 Block 的构造不分配 Function 状态表、不求 Function 固定点；Function 仍按 MaxBlockCount 建带完整指针身份的状态槽，按必要 RPO 摘要求固定点。查询空/外来 HIRBlock 先返回未知，不先读取 terminal；目标 PC 匹配、缺失/观察出口、空块守卫与改写后重新传播保持。
- 摘要与逆扫清理的两张 label 哈希退役，共用一个当前块的有序快照缓冲。实际经过 BindLabel 才收集绑定键并原地堆排序；查找不扫描或分配。索引有目标仍不构成本轮已绑定证明，未知/后向目标保守回退 All；carry 屏障推进 epoch 丢弃已见快照，回绕清除旧 epoch。
- 切换块时清除键并复用容量；同一块可复用键。flags reader/writer 改写后重算，创建/重定向 label 后必须重建作用域。分析不进入 SemanticFacts、封口结果或 emitter，不在 decode 求跨块需求。带绑定块增加一次索引准备访问和 O(L log L) 排序，容量可能保留到整次清理结束；必要逆扫、摘要刷新和两层固定点没有删除。

### 集中验证及适用范围

Android 与 RV64 均在最终同一源码上完整构建十四目标并退出 0 后才记录最终 ELF、执行测试。红魔 NX729J（`bf1359ff`）前后核对产物和 guest 哈希；本批十五项 native 均新执行，四组普通 Block/Function 负载新执行，归档二十五个原始成员。没有重复运行完整主套件或 PMU。

既有 IR 套件新增三个永久用例：独立前向路径枚举核对局部需求及改写后重算；Block/Function 实际 flags 清理；carry 屏障丢弃已见快照。覆盖空/读/覆写、嵌套和重叠 Goto/NotGoto、未绑定目标、十二层标签、合法 Positive 位、外来同序号空 Switch 和空指针。HIR 显式快/慢模式及独立 Block 的进程快/慢模式分别验证；不是对任意后向局部循环的精确活跃性承诺。

| 验证 | 本批实际结果 |
| --- | --- |
| IR 默认 / 慢模式 | 各 114 用例、0 failure/error；107539 / 107637 断言，各相邻增加 217 |
| 完整 native 主套件 | 755 用例、483905 断言；保留相同 11 个历史失败用例、66 个失败断言、0 error；共同用例顺序与失败签名相同 |
| 主套件断言差额 | 相邻 484181→483905，减少 276；本批无新增 main Catch 用例，该差额仍未逐项归因，不能写成覆盖完全等价 |
| 原四组整数谓词 oracle | 25536 程序、933552 次结果/raw 比较保持；flags0、条件控制和快/慢 helper、持久化与寿命 marker 均符合预期 |
| 缓存 | 当前格式 36 round trip 及旧 30/31/32/35 拒绝，118 checks |
| RV64 v48 | 18 项用户态新执行通过；14 项用户态和 5 项 guest 内核按完整相同 ELF、注册命令/属性、环境及原成功记录复用 |
| 强 fence TSO | ELF 未变，复用 v45 的实际 220321 checks 成功记录；本批不称新执行 |
| RV64 范围 | 仅 37 个 RV 专用注册项；两个 compile-cost 与 emit 仅构建，没有新内核启动或物理 RV64 验证 |

RV64 冻结输入 2554 文件（摘要 `91c750d13218cad5643e4e06e7a013aad12a79d5b3f3e9ee4263eea4ce66e61c`），十四 ELF 中七个变化；复用保留实际旧成功记录、时间戳及历史外层失败信息。全部测试结束并核对只有 sleep 后，仅停止本批 v48 容器。

### 代码、栈、容量与机器码

launcher `.text` 为 4508084→4508476，增加 392 bytes；带 debug 的完整 ELF 为 121941248→121915520，不能把后者当成运行时代码收益。先前 std::sort 的候选曾使 text 增加 5568 bytes，最终原地堆排序消除该特化增长，原失败/成本记录保留。108 个既有构造主体共 112712 bytes 的 code/自身 SP 保持；21 项 owner 布局、PendingStore 32、PublishedCapture 24、IRMeta 48/表 9312 及去 debug 后 metadata object 保持。

Android DWARF 单独测得 FlagFlow 为 80 bytes，FunctionState/LabelState 各 32，Transfer 16；不以此推断旧哈希节点或总堆收益。EliminateFlags 自身 code/SP 为 1060/208→808/192，CleanBlockFlags 为 3704/464→3124/416；旧 ComputeFunctionLiveIn 2632/272 拆为 ComputeLiveIn 728/160 和 Summarize 684/144。新增 PrepareLabels 1588/192、独立 Block 构造 328/96、Function 构造 468/128、BeginBlock 356/96、LiveOut 220/80；新增调用帧与递归链仍须完整计量，不能只累计降低的局部帧。

四组普通负载 checksum/CRC、实际模式、spill 和非 formatter 尺寸保持。Function CoreMark 426→427 条尺寸记录、286436→286528 host bytes；Block CoreMark 2416 条记录保持、204684→204972 host bytes，差异均限于 guest formatter。Block CoreMark 原始 hash 重新计算，七处已限定 CPUID helper 地址物化之外无共同主体字节变化；存在 formatter 的不同块集合，不能称完整 raw 相同或程序等价。Block bench 101 条记录、16464 bytes 全部 raw 相同；Function bench 22 条尺寸记录、19896 bytes 保持，未据此宣称完整主体 raw 相同。

四组普通负载及慢 IR 的可见工作区六项基础总量均保持，五组均清零并闭合分项。Core/Block Core 的 retention_events 与 pattern_init_bytes 有差异，bench 两组全字段相同；慢 IR 仍为 419 请求、6681460 allocated bytes、3981816 peak，retention_events 859→883。新增 label 缓冲不属于这九类计量，私有请求/容量、保留时间、初始化、传递/动态栈与完整编译周期仍开放，本批没有 PMU。

### 失败记录、环境恢复和剩余门

v1 测试构建错误地通过只读 HIRBlock 请求可变访问，修正为 EditBlock；v2 新 IR 用例使用不合法的高位 flags 掩码，被构造检查拒绝，最终改用合法 Positive 边界，没有放松生产参数检查。两次失败记录保留，v2 仅执行首项 IR 后停止，没有追认未运行的主套件或负载。

最终 v3 的产物复制和 RV48 初次编译另遇宿主系统盘 ENOSPC，未开始最终测试。仅将自有 4585590729-byte DWARF 证据复制到 Data2，流式 SHA256 核对后用符号链接保留原路径；迁移清单位于 `/Volumes/Data2/swiftvm-middle-results-relocated-20261025/relocations.jsonl`。截断的第五个 ELF 另存 `.partial-enospc`，原构建日志及其哈希保留。空间恢复后启动原 OrbStack 环境，核对同一个 v48 container ID/image/只读源码挂载及工具哈希，保留原 configure 0/build 1，用新 resume1 日志完成同一快照的十四目标构建；没有清理 Docker 数据或操作其他容器。

35 项范围核对通过。证据位于 `/private/tmp/swiftvm-middle-results`：`r2-local-flag-flow-v3-{quality,test-summary,cost-comparison,private-cost,storage-comparison,shape-summary,host-byte-comparison,freeze-recovery}.json` 及冻结源码/产物/原始归档；RV 为 `r7-rv64-execution-20261025-v48-{test-summary,tso-execution,build-record,environment-resume1,container-stop}.json`。本批拆分与 label 哈希退役已完成范围内验证，完整 R0–R7、完整成本、后向局部循环、其余算术/类型/CFG/Phi、恢复/并发及正式 FEX 质量门继续开放。

## R1/R2/R7：退役隐式 pass 入口（2026-10-25）

本批 `r2-pass-context-v1` 接续 `r2-local-flag-flow-v3`。源码摘要 `632a737bfbc25a779ec246c944261a0611618ee4f0dbff08f36686822a9ff081`，patch SHA256 为 `2155a2e0a3396b835e72199ba268c4b0d1b550eed57800515673105fc4df4a1f`。修改八份生产文件和十二份既有测试文件，唯一新增文件是 `tests/support/rewrite_fixture.h`；overlay 476，累计退役路径仍为 113。没有新增测试用例、二进制、构建项、统计开关、临时 probe 或仓库脚本，缓存格式保持 36。

### 接口与调用方

- `passes.h` 中十五个 Block/HIR 便利重载及七份 cpp 中的对应实现退役，删除十四处 CompileWorkspaceScope 获取。六个真正的残余 pass 入口只接收显式 RewriteContext，DCE 直接调用其 CollectDead；Block/Function 仍由上下文的不同构造和处理路径区分。公共 Optimize 与 RV64 生产调用原已使用显式上下文，本批没有替换它们或增加兼容别名。
- 七份生产 cpp 除被删包装外逐字节相同；`passes.h` 除十五项声明删除外逐字节相同。flags.cpp 817→805 行，生产头/实现共删除 119 行。真实 state 家族 helper CaptureLatestStateValues 仍被 state.cpp 和测试使用，本批保留，不把它误当作无调用包装删除；其家族接口边界继续收口。
- 十二份测试的 99 处调用改为真实 RewriteContext/CollectDead。新夹具只有工作区 scope 和上下文，按此顺序构造、反向销毁；临时夹具完整语句的寿命覆盖 pass 调用，不复制算法或生成测试专用 pass。flags/DCE 保持原清理开关，value/width/state/sink/finalize 保持 false；Block owner 与独立块边界保持。原四次 fast/legacy state 调用改为显式 FeatureSet.uniform_fast，其他字段保持默认。
- 测试宏序列及实际用例列表保持，既有断言未作为本批迁移目标删除。Android DWARF 测得该测试夹具 496 bytes；没有给生产 owner、workspace、SemanticFacts 或 prepared actions 增加字段/缓冲，不以夹具内联或代码删除推断总堆收益。

### 一次集中验证

最终源码的 Android、RV64 均一次完整构建十四目标并退出 0，再冻结对应十四 ELF；没有构建失败后继续使用半套产物。本批红魔 NX729J（`bf1359ff`）十五项 native 均新执行，四组普通负载新执行，前后设备/guest 哈希一致，原始归档二十五成员。没有新增/反复运行完整主套件或 PMU。

| 验证 | 本批实际结果 |
| --- | --- |
| IR 默认 / 慢模式 | 各 114 用例、107539 / 107637 断言、0 failure/error；相邻计数相同 |
| 完整主套件 | 755 用例、483856 断言、0 error；相同 11 个历史失败用例和 66 个失败断言，顺序/失败签名保持 |
| 主套件成功断言差额 | 483905→483856，减少 49；没有新/退役用例，该差额尚未逐项归因，不能称完整断言覆盖等价 |
| 其他既有验证 | 四组谓词 oracle 25536 程序/933552 次结果/raw 比较保持；helper、flags0、控制、磁盘/AOT 与四项寿命 marker 符合预期；缓存 36 与旧 30/31/32/35 拒绝仍为 118 checks |
| RV64 v49 | 19 项用户态新执行通过，13 项用户态/5 项 guest 内核按相同完整 ELF、命令/属性、环境和实际旧成功记录复用 |
| 强 fence TSO | 完整 ELF 未变，经 v48 复用实际 v45 的 220321 checks；本批没有新执行、内核启动或物理 RV64 验证 |

RV64 固定输入 2555 文件，摘要 `e26edc411e32bde502bbeb2aa19eb4c5fb073d9f3880063b230b45de0ac3572e`；十四 ELF 中八个变化。三十七项仅为 RV 专用注册范围，两个 compile-cost 和 emit 仍仅构建。复用保留实际旧记录、时间戳及历史外层失败信息。结束后核对只有 sleep，仅停止本批 v49 容器。

### 代码与栈的实际变化

launcher `.text` 4508476→4508820，增加 344 bytes；完整含 debug ELF 121915520→121664968。Clang 在删除多余调用方后将 CleanupStateBlock 和 SinkBlockStores 分别内联到唯一的真实 pass 入口，不能只比较入口自身帧就判断整个路径增长或下降：

| 家族 | 原入口 code / 自身 SP | 原块处理 code / 自身 SP | 本批内联入口 code / 自身 SP | 原直接调用边界固定帧合计→本批 |
| --- | --- | --- | --- | --- |
| CleanupState | 100 / 64 | 9048 / 3280 | 9276 / 3312 | 3344→3312 |
| SinkStateStores | 80 / 48 | 4764 / 656 | 5048 / 672 | 704→672 |

以上 code/SP 单位均为 bytes，SP 为所测自身减量。相对原入口加块处理，两组代码分别增加 128/204 bytes；普通函数区间并集增加 332，具名 `__CortexA53843419_*` stub 的额外非重叠覆盖增加 8，无函数符号覆盖部分增加 4，合为 text 的 344。没有把最后四字节擅自定为填充。原入口在空 Blocks 检查后才建立小帧，本批内联入口在空检查前也预留大固定帧；活跃块直接调用边界各减 32 bytes 不推广为空输入、深层调用或完整动态栈收益。保留编译器当前选择，空集合入口与整体成本继续验收。

108 个既有创建主体的 112712-byte code/自身 SP、21 项 owner 布局、PendingStore 32、PublishedCapture 24、IRMeta 48/表 9312 以及去 debug 的 metadata object 保持；无新增生产常驻事实。四组普通负载的结果、实际 Block/Function 模式、spill 和非 formatter 尺寸保持。Function Core/bench 的全部尺寸记录相同；Block Core 2416 条、204972→205140 host bytes，差异限于 formatter，七处已限定 CPUID 地址物化之外无共同主体字节变化。Block bench 101 条/16464 bytes 全 raw 相同，不推广为完整程序等价。

五组可见工作区基础总量均保持并闭合；四组全字段相同，只有 Block Core 的 retention_events 22313→22298、pattern_init_bytes 820772→820552。慢 IR 仍为 419 请求、6681460 allocated bytes、3981816 peak。完整私有容量/初始化、保留、动态/传递栈、编译周期及正式 PMU 均未据此验收。

35 项范围核对通过。证据位于 `/private/tmp/swiftvm-middle-results`：`r2-pass-context-v1-{quality,structural-proof,caller-migration,retired-wrappers,test-summary,cost-comparison,inlining-review,fixture-cost,storage-comparison,shape-summary,host-byte-comparison}.json`、冻结源码/产物与原始归档；RV 为 `r7-rv64-execution-20261025-v49-{build-record,test-summary,tso-execution,container-stop}.json`。完整 R0–R7、类型/算术/CFG/Phi、家族接口、恢复/并发、完整成本和正式 FEX 质量门继续开放。

## R5/R6：固定寄存器状态准备移出 JIT（2026-10-25）

本批 `r5-register-state-preparation-v2` 接续 `r2-pass-context-v1`。源码摘要 `e0eb9c4781b34dbc65f2b997fb352d762f52768863a27cabb293e8be18b9fabb`，patch SHA256 为 `b47c7435d11512dc5e183cec18bf605e7a231a85f92c6de79873362833f9173c`。修改二十份既有源文件，新增五份后端准备文件，同时删除原 JIT 中五份 guest_state_map 文件；overlay 仍为 476，累计退役路径 113→118。旧类、路径、别名及 CMake 引用清零，没有转发头、新测试目标、迁移开关、临时 probe 或仓库脚本；缓存格式保持 36。

### 职责和准备边界

| 文件（backend/arm64/compile/） | 本批职责 |
| --- | --- |
| `register_state.h` | RegisterStatePreparation：单次准备的物理 fixed-home、入口高位、版本与观察/fault 证明；不作为公共 IR 值位事实或 emitter 状态 |
| `register_state.cpp` | BeginBlock 与访问索引失效/容量复用；显式 PrepareHomeSurvival/PrepareFirstClobber/PreparePublicationWindow；紧凑 home/role/position 表 |
| `register_widths.cpp` | BeginFunction、共享 FunctionCFG 的入口/出口宽度固定点和显式入口查询；保留原根/未知前驱/call-return 播种 |
| `register_versions.cpp` | 实际复用发表触发的物理版本、早写失效、原始视图及固定 home uses；保留不同准备策略的输入 |
| `register_capture.cpp` | 实际需要的 observer/fault 捕获与只读覆盖证明，保持 boundary/version/home 和物理高位约束 |

原三份 CFG/版本/fault worker 在类型、API 和纯 effects 委托改名后，经去 includes/comments/空白归一核对，主体保持。多个准备实例仍有不同播种和物理视图，未按同一 Block 生硬合并；ActiveState 的稠密/稀疏版本、三十 home 列表及实际捕获继续保留。通用 IR/effects/CFG 的其他统一工作不因该目标家族迁移而宣称完成。

所有可能扫描或建立索引的方法均是非 const 的 Prepare 接口，不再以 mutable 缓存在 const 查询中建表。共享窄读/transfer 匹配显式借用可变准备对象；原 MatchSpilledGPRPublication 的 const 也退役，避免通过引用成员隐藏准备。FixedHomeForUse、ResidentUseCount、HasEntryWidthFacts 和内部已准备 span 查询继续只读。三处重复观察包装退役，调用方直接使用带原 FeatureSet 的纯 TargetMayFaultOrObserve；动态脏状态、NZCV、标签和实际编码仍归 JIT。

### 访问表与成本边界

旧索引为三十二份 writes、三十二份 reads 和一份 observers，共 65 个 vector 头；本批为一个有序 u32 accesses 和一个 observers vector。key 包含 home、读/写角色及原 u16 指令位置，未扩展原位置域。相应角色范围和严格 after/before 端点、ignored、helper ABI 和 fault-capture 规则保持。

未准备索引时，survival 继续线扫；只有跨度至少 63 且完整成功的扫描才计数，三次后才为后续 survival 请求建表。first-clobber/window 依原策略显式要求索引。BeginBlock 不新增 IR 资格扫描；重置只清两份逻辑内容并保留容量。新表在准备时排序，const span 查询只查数据，但每次范围定位增加二分访问，不能记为所有查询零工作。

Android DWARF 测得原 GuestStateMap 和新 RegisterStatePreparation 均为 1536 bytes；按需索引对象 1560→48，减少 1512 bytes 的 vector 头/对象空间。每条读/写位置载荷从 u16 的 2 bytes 增至带角色的 4 bytes，observer 指针仍为 8 bytes；容量、扩容重叠、保留和真实 allocator 流量另计，不能用对象头减少推断总堆下降。

launcher `.text` 4508820→4507556，减少 1264 bytes；含 debug 的完整 ELF 121664968→121597176。108 个既有创建主体共 112712-byte code/自身 SP、21 项 owner 布局、PendingStore 32、PublishedCapture 24、IRMeta 48/表 9312 及去 debug 后 metadata object 保持。按改名配对的 23 个方法自身 SP 均保持：索引准备 code 2900→1976/自身 SP 208，重置 536→32/SP 0，survival 552→560/SP 144，window 936→1172/SP 160；BeginBlock 204→220/SP 32，析构 180→252/SP 32。新增 HomeAccesses 为 360 bytes、自身 SP 96，准备后的查询调用链可能经过它，不能从原方法自身 SP 不变推断完整栈不增。完整编译周期、私有容量/初始化与正式 PMU 继续开放。

### 集中验证与实际来源

v1 两端构建均因 constraints.cpp 改用纯 effects 函数后缺少直接 effects.h 包含而退出 1，日志/源码快照保留，没有冻结最终 ELF 或运行测试。v2 仅补齐该依赖，Android 与 RV64 均完整构建十四目标退出 0，冻结最终 ELF 后先完成静态成本核对，再执行一轮集中 native 回归。

红魔 NX729J（`bf1359ff`）十五项 native 均新执行、四组普通 Block/Function 负载新执行；设备前后产物与 guest 哈希一致，原始归档二十五成员。既有 forward oracle 增加 480 项 first-clobber 和 192 项 window 检查，独立遍历原始 IR，覆盖角色、端点、忽略项、ABI 切换和重新绑定；没有新增 TestCase 或复制索引算法。

| 验证 | 实际结果 |
| --- | --- |
| IR 默认 / 慢模式 | 各 114 用例、107539 / 107637 断言、0 failure/error，相邻不变 |
| 完整主套件 | 755 用例、484559 断言、相同 11 个历史失败用例/66 个失败断言、0 error；共同顺序与失败签名保持 |
| 断言差额 | 483856→484559，增加 703；672 属于本批已验证检查，其余成功断言差额 31 未逐项归因 |
| 既有其他覆盖 | 四组谓词 oracle 25536 程序/933552 次结果/raw 比较、helper/flags0/控制、磁盘/AOT 和寿命 marker 保持；缓存格式 36 及旧 30/31/32/35 拒绝仍为 118 checks |
| RV64 v51 | 十四 ELF 中四个变化；17 项用户态新执行通过，15 项用户态/5 项 guest 内核按完整相同 ELF、命令/属性、环境和实际旧成功记录复用 |
| 强 fence TSO | ELF 未变，经 v49 复用实际 v45 的 220321 checks；没有新 TSO 执行、guest 内核启动或物理 RV64 验证 |

RV64 固定输入 2555 文件，摘要 `3c985e90ddfea930427d7a21848ff5fd0a9a21ff04ae9c36eeef9a15df6aeb13`；三十七项仅 RV 专用注册范围，两个 compile-cost 与 emit 仍仅构建。复用保留实际旧原始日志、时间及历史外层失败信息。失败 v50 和完成 v51 均核对只有 sleep 后分别停止，仅操作本批自己的容器。

四组普通负载 checksum/CRC、实际模式、spill 与非 formatter 尺寸保持。Function CoreMark 427→426 条、286528→286436 host bytes；Block CoreMark 2416 条、205140→204684 host bytes，差异限于 formatter。七处已限定 CPUID 地址物化之外无共同主体字节变化；Block bench 101 条/16464 bytes 全 raw 相同，Function bench 22 条/19896 bytes 尺寸保持，不推广为完整程序等价。

五组可见工作区基础总量均保持并闭合，bench/Block bench/慢 IR 三组全字段相同；Core 与 Block Core 的 retention_events/pattern_init_bytes 有差异，慢 IR 仍为 419 请求、6681460 allocated bytes、3981816 peak。新的私有访问表不属于这九类计量，没有将可见工作区保持写成其实际堆收益。

39 项范围核对通过。证据位于 `/private/tmp/swiftvm-middle-results`：`r5-register-state-preparation-v2-{quality,structural-proof,private-cost,method-cost-comparison,test-summary,cost-comparison,storage-comparison,shape-summary,host-byte-comparison}.json`、最终源码/产物与原始归档；RV 为 `r7-rv64-execution-20261025-v51-{build-record,test-summary,tso-execution,container-stop}.json`。完整 R0–R7、公共逻辑 guest-state/类型/CFG/Phi、其余候选/暂存、恢复/并发、完整成本与正式 FEX 质量门继续开放。

## R0/R5：五类负载的三方 PMU 阶段报告与存储接续（2026-10-25）

本段对应已验收源码 `r5-register-state-preparation-v2`，摘要 `e0eb9c4781b34dbc65f2b997fb352d762f52768863a27cabb293e8be18b9fabb`；相邻参照为 `r2-pass-context-v1`，摘要 `632a737bfbc25a779ec246c944261a0611618ee4f0dbff08f36686822a9ff081`。两者均以 Git HEAD `2804a51b6c0477756ca081bec5ba744d152d81d8` 加冻结 overlay 标识，不能只用 HEAD 区分产物。本段新增 PMU 和工具验收，没有重新执行前段 native/RV64 全套件。

### 输入、运行范围与完整性

| 产物 | SHA-256 |
| --- | --- |
| SVM 相邻参照 launcher | `606be57a7cbd29a9106b97230bea1f370e662e5a5bda8c893a1ecdcdace432b3` |
| SVM 当前 launcher | `d9a7373e7bf844eb52ef9e7307c595845926e6c2250d2390a961ef1fb24808c5` |
| 固定 FEXLoader | `8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175` |
| 固定 FEXServer | `9fe58a9946e9105c49911518356fe29a4ed3143c1fe90803b57afe112c225e8c` |

FEX 使用 Android 分支 `a156752fae3a4f65fefb1ed95af980bcdfac43d7` 与已冻结平台补丁 `4ade908f47d73a89d9a883f70197a5824249a4d1f53f45dd4e3df868690e532a`，不是官方 2026 年设计参考提交。Android x18 保留导致其动态 GPR 池为六个的适配限制继续保留。CoreMark guest 为 `d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba`；现有 freestanding bench guest 为 `df60aa3f57d40ed3eb0b44d124f2cb8c9d15899f90021b6d20e94be179240a6d`。

红魔 NX729J/`bf1359ff`、CPU4、分组 instructions/cpu-cycles、双 shell 等待与 PID/affinity 证明、两类退出状态、raw running==enabled、前后部署哈希及设备条件均按 14.6。五组各有 36 个主点、6 个辅助服务冷启动点和一个独立服务寿命计数，共 180 主点/30 冷服务点，协议检查均通过；每组设备 raw 文件取回前后及本地哈希闭合。测量不改变频率、性能模式、风扇、充电或其他服务。

Function/Block 列表示 `SVM_FUNC_BASE=1/0` 的运行时编译模式；本轮保持 `SVM_RA_1BLK` 的默认快速路径，没有把 Function 模式内的每个单元都证明为走完整 Function 分配。独立 Block、合格单块 Function 和多块 Function 分配核心仍由显式入口与 14.2 的配置矩阵分别验收。FEX 两个配对组使用同一配置，组名对应 SVM 的配对上下文。

CoreMark 继续使用固定 2000/12000 与 CRC。bench-suite 的 int/fp/mem/branch 使用 scale=1/2；其完整 stdout 在 macOS x86-64/Rosetta 下由现有 `bench_suite_native.c` 独立执行生成，五种 kernel 的 scale=1 均与仓库 golden 一致。`tools/redmagic` 已扩展完整 stdout SHA-256 校验及独立 kernel 两点矩阵；旧 CoreMark 原始归档经新比较器复算，数值保持。对真实 int 归档的离线破坏检查确认错误 scale argv、stdout、重复端点与缺失主点都会被拒绝，没有新建测试脚本、guest probe 或测试目标。

### 相邻候选/参照：逐轮配对斜率

表项为中位数变化百分比 [最小, 最大]，每项三轮；MAD 与每个原始值另保存在阶段 JSON。斜率分母是 CoreMark 的 10000 iterations 或 bench 的 1 scale。

| 负载 | 模式 | instructions Δ% | cycles Δ% | wall Δ% |
| --- | --- | --- | --- | --- |
| CoreMark | function | -0.016 [-0.019, +0.010] | +0.185 [-1.014, +0.281] | +0.169 [-1.144, +0.207] |
| CoreMark | block | +0.012 [+0.003, +0.022] | -1.482 [-2.164, +1.982] | -1.453 [-2.129, +1.961] |
| int | function | +0.104 [+0.066, +0.302] | +0.553 [+0.006, +1.135] | +0.795 [+0.700, +1.273] |
| int | block | -0.010 [-0.049, +0.036] | +0.935 [+0.232, +1.069] | +0.860 [-6.815, +1.226] |
| fp | function | -0.123 [-0.167, +0.167] | -1.621 [-2.449, +3.378] | -1.623 [-2.274, +3.582] |
| fp | block | -0.024 [-0.045, +0.036] | -0.960 [-1.684, +0.994] | -1.115 [-1.735, +1.109] |
| mem | function | +0.004 [-0.037, +0.037] | +0.354 [-0.268, +3.540] | +0.197 [+0.176, +3.527] |
| mem | block | +0.006 [-0.009, +0.054] | -0.737 [-3.598, -0.351] | -0.738 [-3.649, -0.413] |
| branch | function | -0.011 [-0.020, +0.002] | -0.025 [-1.108, +2.352] | -0.330 [-1.487, +2.542] |
| branch | block | +0.023 [-0.048, +0.049] | +2.527 [+0.191, +2.986] | +2.457 [+0.176, +3.285] |

所有已测指令斜率中位数在相邻版本的约 −0.123%～+0.104% 内，未观察到稳定指令收益。Block branch 的 cycles/wall 中位分别上升约 2.527%/2.457%，三轮同方向；原因尚未归因，不能将它记为整体无退化或直接归入零噪声。其余周期/时间也保留全部波动，不用单一中位数宣称稳定加速。该批不是新的 recolor/Bind 扩大，既有 spill 门仍以同单元静态记录另验。

### 固定 FEX 的条件性参照

以下为候选/FEX 斜率，中位数 [最小, 最大]；较小表示该指标较低。该表仅对应上述固定工作负载、构建和配置。

| 负载 | 配对模式 | instructions 比 | cycles 比 | wall 比 |
| --- | --- | --- | --- | --- |
| CoreMark | function | 1.6897 [1.6896, 1.6899] | 1.9530 [1.9511, 1.9738] | 1.9581 [1.9465, 1.9728] |
| CoreMark | block | 2.1518 [2.1515, 2.1526] | 2.5018 [2.4738, 2.5593] | 2.5018 [2.4730, 2.5643] |
| int | function | 1.5005 [1.5001, 1.5005] | 1.0044 [0.9764, 1.0146] | 1.0061 [0.9665, 1.0153] |
| int | block | 2.0981 [2.0942, 2.1002] | 1.0870 [1.0505, 1.0879] | 1.0865 [0.9722, 1.0869] |
| fp | function | 3.4444 [3.4443, 3.4445] | 3.0474 [2.8316, 3.0563] | 3.0682 [2.8271, 3.0833] |
| fp | block | 3.2308 [3.2194, 3.2368] | 2.9110 [2.8090, 2.9501] | 2.9339 [2.7977, 2.9492] |
| mem | function | 1.3569 [1.3564, 1.3579] | 1.7974 [1.7765, 1.8423] | 1.7969 [1.7770, 1.8420] |
| mem | block | 1.9049 [1.9043, 1.9051] | 1.8446 [1.7921, 1.8565] | 1.8460 [1.7916, 1.8562] |
| branch | function | 1.4539 [1.4533, 1.4545] | 1.2167 [1.1914, 1.2339] | 1.2149 [1.1949, 1.2279] |
| branch | block | 1.7495 [1.7488, 1.7496] | 1.4368 [1.4116, 1.4659] | 1.4321 [1.4107, 1.4640] |

FPR/SSE 浮点负载的指令与周期差距最大，值得用已有 IR/host 形状和独立热点归因继续检查；整数 Function 的周期接近 FEX，不能用其结果代表 FPR 或访存。以上差距未证明来自哪一个准备/分配/发射阶段，也没有据此改动 ISA 语义、关闭必要证明或降低正确性要求。

### 三臂绝对估计

计数单位均为百万；时间为 ms。低点为 CoreMark 2000 或 bench scale=1。每格为三轮中位数，slope/intercept 先逐轮配对求得。原始分布、MAD、IPC 和负截距均在 JSON 保留。截距包含 loader、dispatcher、编译、运行时及 wrapper，不能称为纯编译成本。

| 负载/模式 | 引擎 | 指令 slope M | 周期 slope M | wall slope ms | 低点指令 M | 低点周期 M | 低点 wall ms | 指令截距 M | 周期截距 M | wall 截距 ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| CoreMark/function | baseline | 1.045098 | 0.329281 | 0.282 | 5337.207623 | 2373.759207 | 2043.593 | 3247.060391 | 1714.494885 | 1481.374 |
| CoreMark/function | candidate | 1.045153 | 0.328758 | 0.282 | 5324.405536 | 2358.109605 | 2028.359 | 3234.567385 | 1704.413515 | 1468.017 |
| CoreMark/function | fex | 0.618482 | 0.167309 | 0.143 | 1515.504216 | 494.970730 | 433.884 | 278.489675 | 160.024532 | 147.008 |
| CoreMark/block | baseline | 1.330515 | 0.420739 | 0.361 | 4760.464138 | 2157.635963 | 1856.311 | 2099.433900 | 1304.750581 | 1127.697 |
| CoreMark/block | candidate | 1.330814 | 0.418335 | 0.359 | 4752.841867 | 2161.382369 | 1859.288 | 2091.515616 | 1330.667444 | 1146.748 |
| CoreMark/block | fex | 0.618479 | 0.167556 | 0.143 | 1515.478432 | 494.800423 | 432.168 | 278.513095 | 159.509951 | 145.529 |
| int/function | baseline | 601.311810 | 245.995742 | 209.925 | 783.161972 | 366.975578 | 320.986 | 181.987205 | 122.082492 | 111.061 |
| int/function | candidate | 601.906433 | 247.612635 | 212.598 | 782.670203 | 366.604024 | 320.393 | 180.763770 | 118.991389 | 107.795 |
| int/function | fex | 401.168754 | 247.665525 | 212.454 | 530.985264 | 340.670982 | 298.691 | 129.860660 | 93.005457 | 85.910 |
| int/block | baseline | 841.717912 | 266.310911 | 228.425 | 998.014531 | 369.248894 | 322.208 | 156.296619 | 102.906054 | 93.620 |
| int/block | candidate | 841.421027 | 268.800233 | 230.389 | 998.015102 | 366.662366 | 319.880 | 156.515481 | 97.862133 | 89.491 |
| int/block | fex | 401.040087 | 247.646629 | 212.892 | 531.072445 | 340.337345 | 297.997 | 130.032358 | 93.066869 | 85.353 |
| fp/function | baseline | 593.788533 | 228.122796 | 195.649 | 804.779947 | 366.314445 | 319.999 | 210.835072 | 138.431043 | 124.388 |
| fp/function | candidate | 593.708299 | 229.069160 | 196.749 | 804.902188 | 363.652899 | 317.888 | 211.251361 | 134.583739 | 121.139 |
| fp/function | fex | 172.366956 | 77.161908 | 66.051 | 302.767822 | 169.521965 | 151.531 | 130.668762 | 92.360057 | 85.480 |
| fp/block | baseline | 557.359821 | 221.613174 | 190.125 | 750.779292 | 343.915776 | 301.057 | 193.459225 | 121.598580 | 110.333 |
| fp/block | candidate | 557.226928 | 219.529522 | 188.005 | 750.430840 | 344.751800 | 301.547 | 193.203912 | 125.265077 | 113.542 |
| fp/block | fex | 172.474583 | 76.198952 | 64.632 | 302.746996 | 168.942961 | 151.921 | 130.272413 | 92.528178 | 86.794 |
| mem/function | baseline | 2098.661638 | 2343.701828 | 1998.392 | 2368.836046 | 2536.001494 | 2181.216 | 270.228927 | 192.074185 | 182.824 |
| mem/function | candidate | 2099.019533 | 2352.219436 | 2014.679 | 2368.685609 | 2539.268408 | 2181.214 | 269.973089 | 186.575333 | 166.093 |
| mem/function | fex | 1546.470215 | 1308.683346 | 1121.225 | 1681.002327 | 1468.241931 | 1264.167 | 134.094405 | 159.558585 | 142.942 |
| mem/block | baseline | 2945.095659 | 2403.377609 | 2059.197 | 3114.285293 | 2584.314450 | 2219.520 | 169.208065 | 170.819422 | 152.347 |
| mem/block | candidate | 2945.279929 | 2385.671425 | 2044.001 | 3113.689730 | 2586.856636 | 2222.806 | 168.111801 | 180.640411 | 160.832 |
| mem/block | fex | 1546.491686 | 1289.738545 | 1104.705 | 1681.277606 | 1468.473299 | 1264.219 | 134.785920 | 172.708710 | 154.328 |
| branch/function | baseline | 769.516198 | 232.114289 | 199.579 | 974.945650 | 359.734629 | 314.229 | 205.336420 | 127.620340 | 114.650 |
| branch/function | candidate | 769.528992 | 234.547404 | 201.407 | 974.730821 | 356.828584 | 312.187 | 205.167017 | 122.281180 | 110.780 |
| branch/function | fex | 529.060922 | 192.661560 | 164.827 | 658.849261 | 287.132808 | 253.236 | 129.889560 | 94.471248 | 88.409 |
| branch/block | baseline | 925.508317 | 273.007459 | 234.293 | 1085.781653 | 386.277148 | 337.456 | 160.435187 | 113.269689 | 103.163 |
| branch/block | candidate | 925.795257 | 277.435608 | 237.726 | 1085.672128 | 384.689049 | 336.123 | 159.870594 | 107.253441 | 98.397 |
| branch/block | fex | 529.144622 | 192.948517 | 165.322 | 658.900783 | 288.043373 | 253.283 | 129.737089 | 95.094856 | 87.994 |

### 服务和证据边界

主点 FEXServer 在 launcher 树外预启动并单列整个寿命，以下计数包含启动、请求、空闲和关闭；不按请求均摊，也不混入主比值。每组六个冷服务点均独立保留，其范围只覆盖 launcher 活跃期继承的服务工作，尾部与管理关闭在外。

| 负载 | 服务 instructions | 服务 cycles | 服务 wall seconds |
| --- | --- | --- | --- |
| CoreMark | 78518350 | 62846997 | 193.996885 |
| int | 79056020 | 68013145 | 107.033796 |
| fp | 78283726 | 61088863 | 105.430459 |
| mem | 78542777 | 63015260 | 190.338187 |
| branch | 78326028 | 65396908 | 106.618067 |

`FEX_HOSTFEATURES=disableavx` 及两个 SVM 版本相同的 CPUID/XSAVE 源码不等于跨引擎全部 guest 能力相等。bench 源码直接选择 kernel/scale，ELF 无 PT_DYNAMIC/PT_INTERP；该合成 ELF 没有 section headers，本机 LLVM objdump 报 unexpected EOF，失败输出保留，不能把源码事实写成完成了机器码全量 ISA 审计。高寄存器压力、任意 helper、短 Block 密集、多块 Function/调用及完整纯编译/私有容量门仍需按实际范围接续；本段不认定正式 FEX 门或完整 R0–R7 完成。

### 用户清盘指令与后续路径

用户要求立即释放系统盘后，调度者在 call 第四个观测点的目标/profiler 均退出 0 后停止，随后仅关闭该批私有服务。call 的四个 raw 观察已保存，控制返回尚未写回的最后一点保持原状；没有补齐矩阵、混合后续采样或记为性能通过。原集合的 KeyboardInterrupt/退出 130 与清盘原因分别保留。

本线从系统盘清理/迁出 277194584064 bytes（258.158 GiB）：结果目录 253.161 GiB，另迁出固定缓存约 4.996 GiB。Data2 保留证据约 2.095 GiB、可复用缓存约 4.996 GiB，没有原样搬迁整个结果目录。Git HEAD、tracked diff、全部未跟踪文件、476 项源码摘要及 118 项退役路径均未因清盘改变；无本线活动写入者或未解决清理错误。全局 df 增量含其他会话同时清理，不全部归因本线。

- `/private/tmp/swiftvm-middle-results` 已指向 `/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results`；后续大结果写 Data2。
- 两个固定 build 路径分别指向 `/Volumes/Data2/swiftvm-middle-build-cache/android` 和 `rv64`，逐文件哈希及 symlink 验证后接续，不重复创建缓存。
- 当前/相邻 ELF checkpoint 压缩为 884987172 bytes，81 个路径/47 个唯一 SHA256；其余 39391 项元数据压缩为 700088478 bytes，全部解压条目已核对。旧不再使用的 ELF 已删除，完整当前测试 ELF 集仍在 checkpoint 内，需要时仅在 Data2 恢复。
- 迁移后的有效 PMU 使用各批 `manifest-relocated.json`；原始 manifest 不变，派生文件只重定位 host artifact/golden 路径，五组复算与原数值一致。

阶段汇总为 `r0-register-state-pmu-stage-report-20261025-v1.json`，五组目录为 `r5-register-state-preparation-v2-pmu-v2` 及 `r5-register-state-preparation-v2-{int,fp,mem,branch}-pmu-v1`，均在上述 active-results。恢复说明为 `/Volumes/Data2/swiftvm-middle-preserved-20261025/PRESERVATION-AND-RESUME.txt`；清盘回执为 `/Volumes/Data2/WinEmu/coordinator/scratch/disk-cleanup-20261025/swiftvm-owner-cleanup-receipt.json`。

## R1/R7：无返回值结果与控制 token 的公共类型边界（2026-10-25）

本批 `r1-void-results-v2` 接续 `r5-register-state-preparation-v2`。源码摘要 `b0f5a37ebb9571c20e9116069ca117f5712267008c425d2f974f02ba6e5c9a51`，patch SHA256 为 `b6325817f182dc19abed714574be05ee9076b75ad45656c1e7bc234ab13c4519`。修改四份既有源文件；overlay 476、累计退役路径 118 保持，无新增源文件、测试目标、TestCase、配置开关或缓存版本，未留下临时 probe/测试脚本。

### 具体问题和修正

默认 Inst::SetInst 在原返回类型非 VOID 时，会把它保留给替换指令；原 HasValidResultType 又接受任意合法非 VOID 枚举，即使新指令的 metadata 返回类别是 Void。于是 LoadImm 经 Reset 改为 Nop/状态写，可能留下不属于该操作的数据结果类型。显式给原始 void 节点填入数据返回类型也不会被结果检查拒绝。该问题涉及 builder 后的公共改写和封口输入验证，不能由 target emitter 补救。

- `ir/instr.h`：动态默认 setter 委托已验证的 OperationFor 默认 setter，只保留一份默认解析。数据操作保留原显式视图；普通 Void 操作按 metadata 解析为 VOID。显式类型入口仍保留原值，畸形输入由必要检查拒绝，不静默改写它。
- `ir/ir_meta.h`：唯一纯 `IsControlToken` 分类描述 Goto/NotGoto/BindLabel 的身份视图，不新增 opcode、enum、metadata 行或状态表。
- `ir/instr.cpp`：普通 Void 操作拒绝非 VOID 结果，未定义占位 opcode 也不作为合法结果；合法 typed/untyped 控制 token 与无返回值 Call 保持。`Value::HasValidDataView` 共用控制角色分类，typed label 仍不是数据 SSA 值。
- `tests/ir_contract_test.cpp`：既有类型视图用例增加 32 项检查，覆盖具名/动态重写、值视图保留、普通 void 结果拒绝、typed label 身份及独立 Block/Function 必要检查的拒绝/修复。既有 TestCase 名单保持。

三种方法体归一移除后，Inst 的成员/方法签名声明保持；metadata 头只增加纯分类函数，cpp 只改结果验证主体。该边界不保存物理驻留，不构建 use/CFG 索引，也没有新增 owner/成员存储。完整类型/SSA/逻辑 guest-state 仍需继续按原方案验收。

### 首版失败及最终回归

首版 `r1-void-results-v1` 的 Android 十四目标和本地回归通过，但 RV64 v52 的十九项新执行有七项失败，原因为 BindLabel 结果类型被拒绝。RV64 Function lowering 的 Label() 合法创建带 U8 身份视图的自引用 BindLabel；metadata 没有机器数据结果，不代表它不能有控制身份类型。修正置于公共角色分类，RV64 lowering 未加绕过分支。v52 原始失败日志、source/patch 与一个实际失败 ELF 压缩保留为 `r1-void-results-v1-failed-checkpoint.tar.zst`（4020654 bytes）；没有把该版记作通过，未执行其强 TSO。修正后重新冻结完整源码，再集中验证。

| 验证 | v2 实际结果 |
| --- | --- |
| Android / RV64 构建 | 各十四目标完整结束、退出 0 后才冻结 |
| IR 默认 / 慢模式 | 各 114 用例，107571 / 107669 断言，0 failure/error；各增加 32 |
| 完整主套件 | 755 用例、484809 断言；相同 11 个历史失败用例/66 个失败断言，0 error；共有失败签名与顺序保持 |
| 主套件断言差额 | 484559→484809，增加 250，全部未逐项归因；IR 新增 32 项不编入主集，旧归因已勘误 |
| 红魔 native / 普通负载 | 十五项均新执行，四组 Block/Function 普通负载新执行；前后部署哈希保持，原始归档二十五成员 |
| RV64 v53 | 十四 ELF 中九个变化；十九项用户态新执行通过，十三项用户态/五项 guest 内核按完整相同 ELF、命令/属性、原始成功日志与冻结环境复用 |
| 强 fence TSO | ELF 改变，本次重新执行 v53，220321 checks 通过；不是复用旧次数，也不作为硬件或形式证明 |

RV64 v53 的完整输入为 2555 文件，摘要 `e9b0899114d6622eff9bd5b8ac6ef3212391174e610a1d49a6b31000337915fc`。QEMU 用户态/系统二进制、环境 image、sysroot 与复用命令保持；只为实际需要的旧日志与内核证明恢复十个文件（114799483 bytes），逐项核对 SHA256，没有因清盘而重跑旧测试。失败 v52 与完成 v53 均核对空闲后停止自己的容器，未操作其他容器。两个 compile-cost 与 emit 仍仅构建，未用它们宣称完整成本通过。

### 静态成本与普通形状

launcher `.text` 4507556→4508092，增加 536 bytes；完整 ELF 121597176→121695640。HasValidResultType 自身 code 108→140 bytes，两版所测自身 SP 均为零且无直接调用；不推断整个创建路径/传递栈或动态耗时收益。IRMeta 运行时表保持 9312 bytes，没有第二份结果类型表。

Function CoreMark 的 426 条/286436 host bytes、Function bench 的 22 条/19896 bytes 与有序形状保持。Block CoreMark 2416→2414 条、204684→204672 bytes，形状差异限于 formatter，spill 保持；本批未重新做 CoreMark 的完整 helper 地址归一。Block bench 101 条/16464 bytes 的全部 raw 记录一致；Function 没有捕获 body bytes，不宣称其逐字节一致或完整程序等价。

五组可见 workspace 基础六项均保持并闭合；Core/bench/Block bench/慢 IR 全字段相同，Block Core 的 retention_events 22304→22283、pattern_init_bytes 818200→818596。慢 IR 仍为 419 请求、6681460 allocated bytes、3981816 peak。没有把这些计量写成全部私有堆或纯编译时间收益。本批没有重复 PMU；上一段五组 PMU 对应前一源码批次，其 Block branch 周期退化与 FPR/FEX 差距仍需归因。

### 交付和保留

二十项范围核对通过，`full_R0_R7_complete/formal_FEX_gate_passed` 保持 false。证据在 Data2 实际结果目录：`r1-void-results-v2-{source-manifest,structural-proof,test-summary,cost,result-type-method-cost,shape-summary,host-byte-comparison,storage-comparison,quality}.json`、对应 `r1-void-results-v2-change.patch`、原始日志及 `r7-rv64-execution-20261025-v53-*`。当前二十八 ELF checkpoint 压缩为 452559928 bytes，SHA256 `09b3774d6dedd3fd1a9bb3178972bbf2356dd2edac93cb5cd97c2144cd5728bf`，全部解压条目已验证。

按用户的磁盘纪律，失败批次二十八份可再生 ELF 已回收 1856453920 bytes，临时恢复的前一批二十七份展开 ELF 也已回收 1733321448 bytes；前一批完整 checkpoint 仍在清盘保留包。当前结果、源码快照与两个固定 build 缓存继续实际写 Data2，旧路径软链接接续。剩余公共 GuestStateFacts/EntryPolicy、完整单元宽度摘要、其他候选/暂存与适用恢复/并发、完整成本及正式 FEX/代表性负载门继续按原目标推进。

## R1/R3：guest-state 字节读取与局部分支交汇共用逻辑（2026-10-25）

本批 `r1-guest-state-reads-v2` 接续 `r1-void-results-v2`，源码摘要 `8092e83fb1bdd7817457677a135b46a0a8e1152dbc92fdb0199fb0ee06a9e61e`，patch SHA256 `c3b749b0d516b9b6d1fcd6da20073d5aac9174fcbdb1aa02f1189e11ddeef8bc`。七份既有文件改动，一份真实分析实现新增；overlay 从 476 到 477，累计退役路径仍为 118。没有新增 TestCase、测试目标、配置开关、IR opcode、metadata 行或缓存版本，也未新增临时 probe/仓库测试脚本。

### 已替换的重复实现与边界

| 文件 | 本批职责与退役内容 |
| --- | --- |
| `compile/analysis/guest_state.h` | 共享 GuestStateByteFact、瞬时 GuestStateRead、ReadKind/ReadPolicy 与非 owning span 查询接口；字节仍以定义身份和源偏移标识，Value 使用边上的 cast view 不改变定义身份 |
| `compile/analysis/guest_state.cpp` | 唯一读取分类、连续字节来源检查及等长表的局部分支 meet；无 IR 改写、物理 home 选择或额外持久 fact owner |
| `compile/builder/ir_builder.h`、`compile/builder/guest_state.cpp` | 两份私有读取结果/字节结构及 UniformFoldFor/UniformWouldFold 退役；使用共享规则，保留 feature/layout/mapped eligibility 与 decode 期短期状态 |
| `compile/passes/state.cpp` | 本地 UniformValue、重复读取视图判定和 meet 循环退役；HIR 改写与 use 注册、统计、helper 失效、映射写入和 DSE 留在残余 pass |
| `compile/passes/state_sink.cpp` | 先按原算法选最新 pending store 并证明每个字节来自同一条 store，再调用共享分类；capture、observer、flush/relink 和异常回滚 worker 未改 |
| `compile/CMakeLists.txt`、`tests/ir_contract_test.cpp` | 接入新实现；既有 state forwarding 用例增加 28 项脏高位、连续性/缺失、类型视图、FPR/lane 策略、边界与 meet 检查 |

普通 Forward 仅把完整 64 位标量和精确 FPR 视图作为别名；U8/U16/U32 即使逻辑同宽也保留 BitExtract，避免 cast/窄算术的脏高位泄漏。ForwardNarrowFPR 另允许偏移零的低 FPR 视图；非零偏移 FPR 和普通 FPR→GPR 转发保持拒绝。PendingStore 保留该消费者原有的精确标量别名、低 FPR 别名及 V128→U64 两个 lane 提取规则，不能泛化成普通转发规则。

sink 的 pending 队列接受 XMM 范围内的标量和向量写，并非只有 V128；“单个对齐完整 V128 写”的限制属于 capture recipe，不是 pending 转发的全局条件。该差异已保留，三类 Call 的 Declared 写范围失效、其他 observer/flush 及 physical home/独立入口播种继续分开。Block、Function 和跨块入口策略没有合并。

结构审计验证两份旧字节结构的字段和 Defined 主体等价；builder 的持久成员除相同布局的 fact 类型替换外保持。sink 的 store 选择/连续性前缀及其余 capture/flush worker、残余 pass 中共享 read/meet 之外的 eligibility、写事实、effects、DSE 与统计主体均逐字相等。这里只交付实际公共逻辑基础；完整 GuestStateFacts/EntryPolicy、封口后版本/观察窗口与 guest-state CFG 数据流仍未交付。

### 构建、一次集中回归与限制

计数勘误（后续源码归属核实）：`ir_contract_test.cpp` 仅属于独立 `swift_ir_test`，没有编入 `swift_test`；本批 IR 新增 28 项及上一批 IR 新增 32 项均不能解释主集变化。上一批主集 +250、本批 -178 都保留为未归因的成功计数差异；原始 XML/日志不变，已修正两份派生 summary，旧归因子检查由勘误取代。

首版 v1 的新增夹具误用不存在的 V128 模板类型，Android 构建退出 1；改为既有 ValueType::V128 重载，生产读取算法未因该修复改变。首版没有冻结 ELF 或运行测试；其 RV64 v54 十四目标构建虽已成功，也没有执行测试或追认为接受版。首版源码/patch、失败日志和 v54 构建记录保留；v54 确认仅剩 sleep 后停止。v2 再冻结完整源码，两个固定缓存完整构建退出 0 后才冻结产物。

| 验证 | v2 实际结果 |
| --- | --- |
| Android / RV64 构建 | 各十四目标完整成功；RV64 v55 源输入 2556 文件，摘要 `cb8009c2aa06ea1470a1b27078b92b448c535488cc92325d9ef2aeabced21364` |
| 红魔原生 | 十五项新执行及四种普通形状，实际退出均符合预期；25 个原始成员传回并逐项核验 |
| 快 / 慢 IR | 各 114 cases，107599 / 107697 assertions，0 failure/error；相邻版各增加已知 28 项 |
| 完整主测试集 | 755 cases、484631 assertions，仍为相同 11 个历史失败 case / 66 个失败 assertion，0 error；不是全通过 |
| 主集计数限制 | 相邻版 484809，本批总计少 178；新增 28 项仅属于独立 IR 目标；主集 -178 项成功计数差异全部未归因，不宣称完整覆盖/成功计数等价，也未为凑齐计数重跑 |
| helper / predicates | helper-slow 2 cases / 17178 checks；四份既有独立 predicate oracle 共 25536 programs / 933552 comparisons，以及九项 marker 均保持 |
| RV64 v55 | 九份 ELF 改变，十九项受影响用户态新执行通过；十三项用户态、五项 guest 内核按完整相同 ELF、命令/属性、工具环境及实际旧成功日志复用，不称新跑三十七项 |
| 强 TSO | 本批 ELF 已变，实际重新执行 220321 checks，通过；有限 QEMU 回归，不代替物理硬件/形式证明 |
| 范围 | 两个 compile-cost executable 和 emit 仅构建；没有新 guest 内核启动、PMU 或正式 FEX 门验收 |

同一 TestCase 名单、顺序和归一后的失败签名一致；这一检查不覆盖上表未归因的成功断言计数差异。现有 pending-store partial/overlap 的独立字节 oracle、解释器与 JIT 路径继续由集中 IR 执行覆盖。

### 生成质量与实际成本

- Function CoreMark 为 426→427 条记录，host bytes 286436→286528；Block CoreMark 为 2414→2412、204672→204384。差异均在既有 formatter 符号范围，范围外完整有序记录一致，spill 指标不变；不把这些 formatter 差异解释为优化收益。
- Function bench 22 条 / 19896 bytes、Block bench 101 条 / 16464 bytes 的完整有序形状保持。Block bench 捕获的 101 份完整机器码校验后逐项相同；Function dump 为空，不作 Function 机器码等价声明，也没有新增 Core helper 地址归一证据。
- 五组 workspace 账本均闭合，六个基础分配/释放/存活/峰值字段保持。Function Core retention_events 17537→17552、pattern_init_bytes 569620→569740；Block Core 为 22283→22262、818596→817224。其余三组全字段相同，慢 IR 仍为 419 requests、6681460 allocated bytes、3981816 peak bytes。这些数据不证明完整私有容量或总 heap 改善。
- launcher 完整 ELF 121646960 bytes，SHA256 `9f4b8f98755beb3bcd9b0b235d53a86d8b12e48b5038562ada654f205fcc7731`；.text 从 4508092 降至 4507068（-1024），IRMeta 表仍 9312 bytes。
- CleanupState 自身常量栈帧由 3312 到 3328 bytes（+16）；其 forwarding lambda 288→272。LoadUniform 192、UniformOnLoad 112、UniformAfterAppend 96、SinkStateStores 672 保持；旧 UniformFoldFor 128 被 64-byte 的适配方法和共享调用替代。新增 ReadGuestStateBytes、ClassifyGuestStateRead、MeetGuestStateBytes 自身帧分别为 80、112、96 bytes，存在额外嵌套和策略分派。所选方法的汇编与直接调用已保留，不将代码减少当作总栈或编译周期收益。

### 证据与滚动回收

23 项批次检查通过。主要记录均在 Data2 的实际 active-results 目录：`r1-guest-state-reads-v2-{source-manifest,reference,quality,test-summary,shape-summary,storage-comparison,structural-proof,cost,method-cost,host-byte-comparison}.json`、原始 `results.tar` 和 `r7-rv64-execution-20261025-v55-*`。

当前二十八份完整 native/RV ELF 已生成并逐项解压哈希核验的 `r1-guest-state-reads-v2-current-elfs.tar.zst`（452826085 bytes，SHA256 `47335a2f5f1e00134c1dbbe65b19dbf10ba8f9af5509c1239789ed4848a37aee`）；当前展开产物保留供下批直接比较。上一批 `r1-void-results-v2` 的原压缩检查点仍保留，重新核验二十八份条目且确认无活动句柄后，删除其二十八份展开副本，Data2 回收 1856544768 bytes。失败首版未冻结 ELF，无额外旧二进制副本可回收。全程只操作本线结果和固定缓存；系统盘三个旧入口仍是指向 Data2 的 symlink，du 为 0。

系统盘原清理回执 `/Volumes/Data2/WinEmu/coordinator/scratch/disk-cleanup-20261025/swiftvm-owner-cleanup-receipt.json` 保持原清理时点的 277194584064-byte 释放记录，追加复查说明不把后续重构改动追认为“清理前后未变”。本批生产变更为 progress；完整 R0–R7、公共 GuestStateFacts/EntryPolicy、完整成本和正式 FEX 门仍未完成。

## R1/R3/R5：按需共享观察候选与 pending 窄读整改（2026-10-25）

本批 `r3-guest-state-observations-v1` 接续 `r1-guest-state-reads-v2`。源码摘要 `083355ba9bb0d54ee0c4e19a227f95f0dbc86d5b7ea475d279d575c8d953da6f`，patch SHA256 `d4f995c7df1524603ae0d8b19247de5eafde456edc4aaf1c5c3d65fa38b37647`。修改十五份既有文件，overlay 477、累计退役路径 118 保持；没有新增源文件、TestCase、测试目标或迁移开关，未留下临时 probe/仓库脚本。

### 发现的真实错误与旧代码证据

pending-store 先证明同一条 store 覆盖所有读取字节，这个身份条件正确；但它把精确 U8/U16/U32 读当作 BitCast 别名。BitCast 不发射截断，Value cast 也只改变使用边的逻辑视图；当窄写来自带脏高位的 U64 定义，helper 又消费完整 X 寄存器时，结果与真正窄 LoadUniform 不符。例：U8 读应得到 0xfe，实际传递 0xa5a5a5a5feebcafe。

先只扩充现有 sink overlap 用例，生产源码与上一接受版完全相同，完整十四目标构建后运行该一项。`r1-pending-narrow-red-v1` 实际退出 12，记录十二个失败 assertion，覆盖两种 IR 输入形状、U8/U16/U32 与两种运行时脏值；没有把它记作接受版或通过结果。红魔端实际 ELF/XML/log 与主机 SHA256 一致。此时单块 Function 默认可能走块分配快捷路径，因此这份 red witness 不宣称已经覆盖独立 Function 分配器。

整改后 pending 的精确窄标量和可表示的部分标量读取走共享 BitExtract；完整 64 位标量/精确 FPR 仍可别名，pending 的低 FPR 和 V128→U64 lane0/1 规则保持。sink 仍先逐字节证明同一 PendingStore，使用 RewriteContext 注册新 operand；capture、observer、flush/relink 与失败回滚 worker 未改。代码发射语义改变，`kCacheFormatVersion` 从 36 到 37，显式拒绝 36；AOT schema 和序列化载荷结构不改。

### 公共事实归属与文件职责

| 文件 | 本批结果 |
| --- | --- |
| `compile/analysis/guest_state.h/.cpp` | GuestStateObservations 为请求过的块拥有保守候选流；候选来自静态 IR effect，并保留 GetUniformAddress / 可能由 helper 实现的 Sse42Str。流不缓存 feature/helper/entry/物理 home 决策；移动块记录只转移 heap payload，既有 span 保持稳定。共享读取规则删除 pending 不安全的精确窄别名 |
| `compile/analysis/semantic_facts.h`、`facts.cpp` | 一个按需唯一 owner；Prepare/Find 显式分开，新 BeginInput 清除。普通 home clobber/survival 查询不建立观察候选；查询不强制 CFG/use 图 |
| `backend/arm64/compile/register_state.h/.cpp` | HomeAccessIndex 的 observer vector 退役，保留物理读写/clobber 索引；发表窗口在物理读写守卫通过后借用共享候选，逐次应用原 feature-bound helper/fault/capture 条件。BeginBlock 必须绑定真实 SemanticFacts，不保留双参数兼容入口 |
| `backend/arm64/compile/constraints.cpp`、`lower.cpp` | 预分配与 target 准备传递同一 owning 输入的 facts；物理版本、入口宽度、home 和不同播种视图仍由各自 RegisterStatePreparation 持有 |
| `compile/passes/state_sink.cpp`、`backend/code_serial.h` | 发射窄读 BitExtract；缓存 key 升为 37，排除旧错误代码 |
| 既有测试文件 | IR overlap case 增加 33 项 runtime 检查；main 的 interval oracle 增加 17 项按需、策略切换复用、输入失效、十二块 Function span 稳定与不强构图检查；其余调用方绑定既有 facts，cache fixture 加旧 36 拒绝 |

十二项结构证明核验物理 clobber/entry、home index/survival/first-clobber 与原窗口 home 守卫保持，目标和约束的其他工作保持，字节连续性与 meet 保持，sink 其余 worker、其他 semantic 分析保持。候选是保守超集，不能当作“已经需要发表”的结论；FeatureSet/helper ABI 改变时仍重做该消费者的筛选。BeginInput 是新可变输入的失效边界，已发布视图不跨新输入复用。

这交付实际拥有、实际消费的观察子事实，不是空 GuestStateFacts wrapper。完整逻辑版本、EntryPolicy、覆盖/窗口证明和 guest-state CFG 数据流仍开放，decode/pass 的临时字节状态也不与封口 facts 混成同一存储。

### 集中回归与分配模式

两个平台各十四目标完整成功、退出 0 后才冻结；RV64 v56 为 2556 文件输入，摘要 `716dca2ead0fb8974c883da46b991afac9969b33e47dc2177bccad059c14a36b`。没有中途执行部分构建产物。

| 验证 | 实际结果 |
| --- | --- |
| 红魔主计划 | 十五项 native 新执行、四种普通形状，所有退出符合预期；25 个原始成员逐项核验 |
| 快 / 慢 IR | 各 114 cases，107632 / 107730 assertions，0 failure/error；各增加已知 33 项，包括原十二失败路径 |
| 完整主集 | 755 cases / 484872 assertions，相同 11 个历史失败 case、66 个失败 assertion，0 error；不是全通过 |
| 主集计数 | 相邻 484631→484872，+241；已核实编入 main 的 interval case 新增 17 项，其余 +224 未归因。IR 新增 33 项不编入 main，不从主集变化中扣除 |
| 额外分配配置 | 同一冻结 ELF，仅运行既有 overlap case，`SVM_RA_1BLK=0`，150 checks / 0 failure；Block 输入保留独立块路径，Function 输入按真实 PrepareFunction→AllocateFunction 条件使用完整 Function 分配器。不是重跑完整 IR/main，也不代替多块/多 Function 全部质量门 |
| helper / predicates | helper-slow 2 cases / 17178 checks；四份 oracle 25536 programs / 933552 comparisons、九项 marker 保持 |
| cache / AOT | current=37 round trip；旧 30/31/32/35/36 均明确拒绝，marker 136 checks |
| RV64 v56 | 九份 ELF 变化，十九项用户态新执行通过；十三项用户态和五项 guest 内核按完整相同 ELF、命令/属性、工具环境与实际成功日志复用 |
| 强 TSO | 本批实际重新执行 220321 checks，通过；有限 QEMU 回归，不是物理硬件或形式证明 |
| 范围限制 | 两个 compile-cost executable 与 emit 仅构建；没有新 guest 内核启动或 PMU/FEX 正式验收 |

额外配置控制的是 RA_1BLK，结论结合当前 runtime.cpp 的真实 Function 入口、compiler.cpp 未提供 block_intervals override、allocation.cpp 的 FeatureSet 分支；不从单块 Function 名称或 SVM_FUNC_BASE 值推断完整 Function 分配器。

上一批和前一批的主集断言归因也已勘误：`ir_contract_test.cpp` 独立编入 swift_ir_test，不属于 swift_test；原先把 28/32 项 IR 新检查计入 main 的归因不成立。两份派生 summary 已改为 main 新增 0、差异全额 -178 / +250 未归因，原始 XML、日志和实际 case/failure 数保持。旧对应归因子检查被勘误取代。本批 17 项来自确实在 main 执行的 block_analysis_index 用例，另 224 项继续保留，未为了凑齐计数重跑。

### 实测大小、额外工作与未验收成本

- DWARF 实测：SemanticFacts 624→632 bytes（新增按需 owner 指针），RegisterStatePreparation 1536 保持，私有 HomeAccessIndex 48→24；GuestStateObservations 自身 24，每个已请求块的记录 32，候选 payload 和容量另计。没有观察窗口需求时不创建该 owner/payload。共用候选不等于总体 heap 或 retention 下降。
- 新候选在块内扫描静态 metadata，按请求块查找；每个发表窗口仍要应用 feature-bound 筛选。请求块记录与候选容量保留至同一 facts 生命周期结束。记录查找、动态扩容/重叠、潜在更宽候选集合、重复筛选和完整编译成本尚未完整计量。
- launcher 完整 ELF 121762112 bytes，SHA256 `168918e9c0cc860cf8cbcc45924ff2b0b13038bc1373cd3afcc2473a9a82712b`；.text 4507068→4509044（+1976），IRMeta 表仍 9312 bytes。
- PrepareHomeAccessIndex code 1976→1740，自身 SP 208 保持；PreparePublicationWindow code 1172→1176、SP 160 保持。新增 SemanticFacts::PrepareGuestStateObservations code/SP 320/128、GuestStateObservations::Prepare 704/160，vector 扩容子过程 320/64，存在额外调用层级。ClassifyGuestStateRead 508/112→460/96；SinkStateStores 4356→4548、SP 672 保持，CleanupState 9172/3328 保持。这不是整体栈或周期改善证据。
- Function CoreMark 427 条 / 286528 host bytes，Function bench 22 / 19896 的完整有序形状保持；Block CoreMark 2412→2417、204384→204776，差异限于 formatter，范围外完整有序记录和 spill 指标保持。Block bench 101 / 16464 的形状和全部捕获机器码相同；Function body dump 为空，不作完整机器码等价声明。
- 五组 workspace 账本闭合、六个基础字段保持；Block Core retention_events 22262→22314、pattern_init_bytes 817224→818408；慢 IR retention_events 883→898。其余三组全字段相同。该账本不覆盖上述公共/私有对象的全部 heap 和容量。

### 证据与滚动回收

28 项批次检查通过。主要记录为 `r3-guest-state-observations-v1-{source-manifest,reference,quality,test-summary,structural-proof,layout-cost,cost,method-cost,shape-summary,storage-comparison,host-byte-comparison}.json`；额外配置原始 XML/log 与 `ra1blk0-execution.json` 单独保留，RV64 为 `r7-rv64-execution-20261025-v56-*`。源码和所有大结果、固定缓存及编译 TMPDIR 继续实际位于 Data2。

当前二十八份完整 native/RV ELF 的检查点为 `r3-guest-state-observations-v1-current-elfs.tar.zst`，453364267 bytes，SHA256 `94df9f2916c8ba7b3ea0aa1d6480e829f0453f8437f78ab5e6883a2755bbf831`，逐项解压哈希核验，展开产物保留供下一批直接比较。相邻 `r1-guest-state-reads-v2` 的旧压缩检查点仍保留，重新核验二十八条目且确认无活动句柄后回收其二十八份展开副本，Data2 释放 1856249856 bytes。实际 red ELF、源码和原始失败证据另压缩为 `r1-pending-narrow-red-v1-checkpoint.tar.zst`（34354571 bytes、SHA256 `4d9c626843e7b33fd858e8198ce049fb484c36ffe6c6d3a2d4d52dafd576b8df`），核验后释放其展开 ELF 136806400 bytes，十二项失败事实不丢弃、不追认为通过。

本批是 production progress；完整 GuestStateFacts/EntryPolicy/version/CFG、全部类型/SSA、故障/关闭/并发与恢复家族、完整编译和私有容量成本、代表性矩阵与正式 FEX/R0–R7 门继续开放。

## R2/R7：退役零使用结果守卫，修正 fault 前发表（2026-10-25）

当前生产批次为 `r2-state-orphan-guard-retirement-v2`，比较起点仍是 `r3-guest-state-observations-v1`；未把失败首版作为接受基线。源码摘要 `0e4e3655209e77d414cb5edcf7eaa29e97a17dc2aaf8f4c6b592be84e54942a6`，patch SHA256 `1fb82a8dc65ba90810fd4d080b758d127c7f9ad129cd56bef418a3efb68e091a`。仅修改六份既有文件，overlay 477、累计退役路径 118 保持，没有新增源文件、TestCase、目标、迁移开关或临时 probe/仓库脚本。

### 退役内容与不能合并的语义

Block 的区间入口已经为存活的零使用定义建立 `[id,id]` 区间；Function 入口也枚举存活 HIR value 并取得必要末端。因此状态 pass 不再需要为了“否则没有结果寄存器”保留虚假 StoreUniform use。两个分配入口仍独立，不以单块 Function 名称推断实际算法。

| 文件 | 当前修改 |
| --- | --- |
| `compile/passes/state_dse.cpp` | 删除 SurvivesDCE、递归祖先守卫和整套 UniformConsumers/HasObservableUse 图；完整 byte 覆盖才删除 store。真实 capture 的最后 carrier 按发表边界保留，不依赖 producer 的普通 SSA 使用。反向遇到 MayFault/Unknown、GetUniformAddress、UniformBarrier 清空覆盖证明，helper/位置与分支处理继续保守 |
| `compile/passes/state_sink.cpp` | 删除同类结果分配守卫和多余同值旁路，允许删除完全覆盖且没有语义发表要求的 store；保持真实 capture、重叠依赖、flush/relink 及失败归属。两处缺失 capture 检查的 HasObservableUse 旧回退尚未退役，不能把它称为逻辑发表证明 |
| `backend/code_serial.h`、既有 cache fixture | 代码格式 37→38，增加旧 37 的明确拒绝；AOT schema 6 与序列化载荷布局不改 |
| `tests/cases/uniform.cpp`、`tests/ir_contract_test.cpp` | 扩充既有 case，新增 2 项 main 检查及 213 项独立 IR 检查；不增加 TestCase 或测试目标 |

状态两文件分别 495→379、603→534 行，净减 185 行。decode 仍可以同时维护静态 effects、局部已知值和安全前向转发；后向 dead-store 覆盖、未来 fault 观察及全块 capture 生命周期留在残余 pass。物理 home 版本受忽略/合并发表、提前写入和独立入口播种影响，尚未迁入逻辑 GuestStateFacts，不能直接合并这些版本表。

### 真实失败及整改

首版 `r2-state-orphan-guard-retirement-v1` 的原集中回归通过相应范围，但没有覆盖一个关键边界：值有普通 StoreMemory consumer，后者 fault 时 guest state 仍必须已经发表。SSA consumer 不会提交 uniform；删除最后 carrier 会得到错误 fault 状态。

仅扩充既有 overlap fixture、保持首版生产代码后，`r2-state-orphan-capture-red-v1` 实际退出 2，Block 与默认单块 Function 输入各有一次 uniform 字节比较失败；两次均确实进入 PageFatal。该失败不能由此前 IR/main 通过结果覆盖，也没有把它记为接受版。默认 Function 输入可能走块快捷路径，这份 red 不宣称覆盖独立 Function 分配器。

修正版将 carrier 保留与 SSA uses 分开，并修正原 DSE 把“前向值不失效”误用为“后向发表可删除”的问题。访存不改 guest state，但可能 fault；没有恢复证明时，其后的覆盖不能消除 fault 前的发表。新的检查同时覆盖无 XMM 映射的 U64、XMM V128、直接/派生 producer，以及普通访存、TSO load、atomic、helper 的零使用结果。纯中间链可以删除，必要副作用根保留并得到真实目标；首个定义编号零也在覆盖内。保护页 fault 只在 JIT 内存操作上执行，不用 helper 内的 C++ fault 冒充 JIT 恢复。

### 集中验证与范围

Android 与 RV64 各十四个目标完整构建退出 0 后冻结，未执行中间产物。RV64 v58 的 2556 文件输入摘要为 `7fbfcc47df560af8d9786242ebe0e8037a78fb8ec0cb52000e940beeed48b0ba`。

| 验证 | 实际结果 |
| --- | --- |
| 红魔主计划 | 15 项 native 新执行、4 种普通形状，退出符合预期；25 个原始归档成员逐项核验 |
| 快 / 慢 IR | 各 114 cases，107845 / 107943 assertions，0 failure/error；各比相邻版增加 213 项 |
| main | 755 cases / 484700 assertions，同样 11 个历史失败 case、66 个失败 assertion，0 error；不是全通过 |
| main 计数 | 484872→484700，合计 −172；实际 main case 新增 2 项，其他 −174 未归因。独立 IR 的 213 项不计入 main，也不重跑来凑齐成功计数 |
| 独立 Function 配置 | 同一冻结 ELF，只追加运行既有 overlap case，`SVM_RA_1BLK=0`，363 checks / 0 failure。真实 PrepareFunction→AllocateFunction 条件选完整 Function 分配器；独立 Block 仍走块入口，不代替多块/多 Function 质量矩阵 |
| helper / predicates / cache | helper-slow 2 cases / 17178 checks；四份 oracle 与其 933552 comparisons 保持；9 项 marker 符合预期。cache marker 实际 154 checks，current=38，旧 30/31/32/35/36/37 拒绝 |
| RV64 v58 | 9 份 ELF 变化；19 项用户态新执行通过，13 项用户态及 5 项 guest 内核按完整相同 ELF、命令/属性、工具环境与实际旧成功日志复用 |
| 强 TSO | 本批实际新执行 220321 checks，通过；有限 QEMU 回归，不是物理硬件或形式证明 |
| 结构与状态 | 17 项结构检查、21 项范围内证据检查通过；自有 v58 容器确认只剩 sleep 后停止，其他项目容器未动 |

两项 compile-cost executable 和 emit 只构建；没有新 guest 内核启动，没有重跑 PMU/FEX。普通形状结果通过 CRC/输出、实际入口和 target_overflow 检查，但下列生成代码/spill 非退化门没有通过。`quality.json` 明确记录 `complete_quality_gate_passed=false`、`quality_nonregression_gate_passed=false`，没有用范围内正确性替代完整质量验收。

### 成本变化与待处理退化

launcher ELF 121632272 bytes，SHA256 `b8177c57119d4e8d1f7bc66dc876af2d85535e9502fd4f9a581fd8e8cd4f9c2d`；编译器 .text 4509044→4505348，减少 3696 bytes。DSE 主过程 code 8080→7512，自身固定 SP 1392→1344；sink flush code 4196→3272、SP 384→208；SinkStateStores code 4548、SP 672 保持。这些是选定函数自身成本，不是整个编译链栈峰值或周期证明。

| 普通形状 | 捕获记录数 | RA host bytes | spill defs / loads / stores |
| --- | --- | --- | --- |
| Function CoreMark | 427→427 | 286528→288496 | 311/89/60→455/131/84 |
| Function bench | 22→22 | 19896→20320 | 161/245/150→159/266/147 |
| Block CoreMark | 2417→2414 | 204776→208472 | 691/302/201→1076/433/272 |
| Block bench | 101→101 | 16464→16764 | 188/262/162→185/281/155 |

四组均有 formatter 之外的实际差异，不能将变化全部归为输出代码噪声。Block bench 的 101 份捕获机器码中 17 条有序记录改变；Function dump 为空，不能据此作完整机器码相等声明。保守 fault 发表增加存活状态与寄存器压力；后续只有具备真实恢复/覆盖证明才可减少这些发表，不恢复已证明错误的旧 DSE。

五组 workspace 账本闭合，live/retained 为零；四组普通形状的请求数分别仍为 61/34/51/47，但 allocated bytes 分别增加 668/1408/592/1376，初始化量也有增加。峰值变化包含 Block bench 下降，不能笼统称全部增加或全部降低。慢 IR 因扩充夹具有额外请求和保留事件。DSE 私有 consumer 图确实退役，但账本不覆盖全部私有堆/容量；完整动态成本、代表性矩阵和正式三方 PMU 仍开放。

### 保留证据与回收

主要记录为 `r2-state-orphan-guard-retirement-v2-{source-manifest,reference,structural-proof,test-summary,quality,cost,method-cost,shape-summary,storage-comparison,host-byte-comparison}.json`；额外 Function 配置为 `ra1blk0-execution.json`，RV64 为 `r7-rv64-execution-20261025-v58-*`。源码和 Git/WIP 保留。

当前 28 份 native/RV ELF 压缩为 `r2-state-orphan-guard-retirement-v2-current-elfs.tar.zst`，452961493 bytes，SHA256 `530bd41b7126cd9c86f16b1293f1a6133fb4cc2fb35cf9f8a7010aff79581d15`，每项解压哈希核验，当前展开副本保留供直接比较。真实两失败现场另存 `/Volumes/Data2/swiftvm-middle-preserved-20261025/r2-state-orphan-capture-red-v1-checkpoint.tar.zst`，34419404 bytes，SHA256 `62abd7ecd8e848f35cd80052eaa124b02948ad866ca5508e092f3fa6657555cf`；包含实际失败 ELF、源码 overlay/manifest、构建、原始 XML/log 和设备哈希。

重新核验相邻版 28 项压缩条目、确认目标均无活动文件句柄后，回收相邻展开副本 28 份、已否决 v1 的可再生成副本 28 份以及 red 展开 ELF，共 57 份，Data2 回收 allocated bytes 3853811712。v1 源码、命令与原始结果仍保留，唯一失败证据不丢弃。清理前后 Git HEAD/status/diff 哈希一致，当前 28 份 ELF 完整；全局 df 变化不全部归给本线。

系统盘原清理释放量仍单独为 277194584064 bytes；三个原 /private/tmp 入口是 Data2 symlink、各自 du 为 0，不把这次 Data2 回收重复计入系统盘。原指定回执持续记录独立复查和保留路径。大结果、编译 TMPDIR 和固定缓存继续实际位于 Data2。

本批保留实际语义修复与旧代码退役，范围内正确性通过；生成代码/spill 退化、完整动态成本、sink 两处旧发表回退、逻辑 GuestStateFacts/EntryPolicy/version/CFG 和完整 R0–R7/FEX 质量门继续开放，目标未完成。

## R2/R7：完整退役状态捕获配方，修正跨范围观察（2026-10-25）

当前生产批次 `r2-state-capture-retirement-v4` 接续 `r2-state-orphan-guard-retirement-v2`。源码摘要 `751727b319d083b86597b9d387985ff9866a80494061bf3d83cd4568591455a4`，patch SHA256 `e2ab8ac7ccadfc1696a582e4f1c86a7f3eaacd8a18e5997ddbc2c7c06ddea3e4`。修改十三份既有文件，生产净减 378 行；overlay 477、累计退役路径 118 保持，没有新增源文件、目标、迁移开关、probe 或仓库测试脚本。一个仅检查旧结构的 TestCase 删除，两个既有 case 改名并改为语义检查，没有新增 TestCase。

### 从真实状态写建立不变量

上批已使反向 DSE 在 fault/状态观察之前保留实际写入。原捕获配方是在补偿旧 DSE 丢掉这些发表后，再把未来 producer 装进既有 carrier 的机制；继续保留它会同时维护真实 IR 与第二份过期候选状态。当前删除整个传递链，sink 直接使用当前 typed store 的原始参数，不能用普通 SSA consumer 代替发表证明。

| 文件 | 当前职责与退役 |
| --- | --- |
| `passes/state.cpp` | 只做当前流的前向字节转发及调度 DSE，不再先清理/创建 capture 配方 |
| `passes/state_dse.cpp` | 原反向扫描、fault/unknown、helper、映射与分支规则保持；最后约 89 行 carrier 搜索、修补及配方运输删除，379→290 行 |
| `passes/state_sink.cpp` | 保留 pending/coverage、同 store 转发、部分 flush 依赖、顺序重连及失败归属。recipe/published/位置哈希/提前 retarget/HasObservableUse 全部删除，534→283 行；PendingStore 不再复制 Value，直接借用原操作数 |
| `ir/block.h`、`passes/dce.cpp` | UniformCaptureRecipe 类型、Block 的 vector 与 get/set/take/clear API 删除，DCE 不再添加配方隐式根；真实 store 的 SSA 使用与提升 anchors/prefix 根保留 |
| `passes/passes.h`、`compile/pipeline.cpp` | CaptureLatestStateValues helper 删除，SinkStateStores 的未使用 FeatureSet 参数及所有调用实参删除；功能资格开关仍在外层 pipeline |
| 缓存与既有测试 | 跨范围观察的发射修复升代码格式到 39、拒绝 38；AOT schema 6 与载荷结构不改。fixture 检查实际状态、顺序、错误回收，不维护退役对象 |

sink 的 fault/control 判断改为查询既有静态 effects，加显式地址/位置/映射状态/RSB 操作。对 191 个有效声明 opcode，与原表逐项相同；Unknown 保守处理。DSE 的完整反向主体与全部 fault/分支守卫逐字保持，前向 transfer 只去掉捕获初始化。Block 和 Function 的目标分配仍独立，保留存活的零使用副作用定义；这次没有用另一套 consumer 图替代旧图，也没有合并物理入口宽度/版本播种。

### 跨资格范围的实际错误

代码审计发现：sink 原先只处理完整包含于 XMM 范围的读写。一个不全含的直接访问仍可能与 pending 相交。例如 [0,16) 的 V128 写已摘链，随后 [8,24) 的 V128 写直接执行；若不先 flush，后续读取可转发旧字节，块尾较早写还会重新覆盖新写。跨边界的直接读也可能在 pending 尚未发表时读取旧 context。此问题由顺序分析发现，本批没有额外执行旧 bug 红版，不把分析推断写成设备复现。

现对有效但不全含的访问先检查声明范围，交叠时在原节点前执行部分 flush，复用同一反向依赖闭合；不相交不扫描 pending/准备 coverage，无 pending 早退。offset/size 先以减法检查，避免溢出误判资格；越界访问前保守全 flush，最终非法 IR 的准入责任不因此取消。四种程序覆盖范围尾部写、尾部读、头部写与间隙，分别用 Block/Function、三种初始填充比较解释器改写前后和真实 JIT，实际新增 156 项检查。

另一既有用例用实际 CallLambda 读取完整值、低 64 位替换、低 32 位恢复及低 64 位恢复四个时点，新增 24 项运行时比较；也要求尚未 StoreUniform 的已计算值不提前成为 guest state。退役结构用例、双重配方配置与旧 API 检查共移除 844 项，因此快慢 IR 各净变 −664，不能把计数降低误报为丢失仍需验证的语义覆盖。

### 构建、夹具修正与 RV64 证据复用

首版删除旧证明块时误删仍需要的 Overlaps，Android/RV59 构建各实际退出 1，未执行测试、未冻结 ELF；补回原函数后完整构建通过。跨范围修复随后合并进入 v3/代码格式 39。v3 首次 IR 执行实际退出 1，记录一个构造 error、零失败 assertion、107143 checks；新 helper 夹具把 Params 容器用于只接受独立 OptionalData 的 CallLambda，既有类型检查按预期拒绝。v4 只把该调用改为两个独立参数，不修改准入规则；v3 未执行 main 或普通形状。错误源码、实际 ELF、设备哈希及 XML/log 保留，不追认为通过。

RV61 在 v3 的 2556 文件输入上完整构建十四目标，输入摘要 `49e17ec88532132f0fe6c56f956c3f7b1977f581267b02ac433f1730ea4a60e6`，实际生产摘要 `170e1afc24855deb0bc88d7b50590050ffa33a8a78ae3d63015c8b1eac8d39e8`。它已完成受影响用户态与强 TSO 执行。v4 仅修正 `ir_contract_test.cpp`；实际 Ninja 的十四目标 440 条命令及依赖清单均不包含该文件（连 basename 也不含），其余编译输入相同。由此复用 RV61 实际结果，原记录仍标记 v3 编译来源，另以 `rv64-source-equivalence.json` 说明最终源码的依赖资格；文档变动同属闭包外输入。不称为 v4 全源 RV 重建，也不再重复 RV 测试。native 十四目标完整退出 0 后冻结，只有 IR ELF 相对 v3 改变，其他十三份逐字相同，方法/DWARF 证据按相同 ELF 输入复用。

| 验证 | 实际结果 |
| --- | --- |
| 红魔主计划 | 15 项 native 与 4 种普通形状，所有退出符合预期；25 个原始成员哈希核验 |
| 快 / 慢 IR | 各 113 cases，107181 / 107279 assertions，0 failure/error；两项改名、一项退役与参考 roster 明确对应 |
| main | 755 cases / 484711 assertions，同样 11 个历史失败 case、66 个失败 assertion，0 error；不是全通过 |
| main 计数 | 484700→484711，+11 全部未归因；本批没有新增 main assertion，三个旧配方检查替换为三个真实发表检查。独立 IR 的新增/退役不计入 main |
| 完整 Function 配置 | 同一冻结 ELF，`SVM_RA_1BLK=0` 仅运行 overlap 与中间观察两个既有 case，558 checks、0 failure；Function 真正走完整分配入口，Block 保持独立，不代替多块/多 Function 矩阵 |
| predicates / helper / cache | 四份 oracle 的 933552 comparisons 保持；helper-slow 2 cases/17178 checks。代码格式 39 round trip、旧 30/31/32/35/36/37/38 拒绝，marker 实际 172 checks |
| 寿命/失败注入 | 两种 fast 各 33 次实际 C++ new 注入失败、1690946 checks，通过删除/转移后回收；原 41 点因真实分配和配方检查退役而下降。constant-window 各 123481 checks，比原少四项已退役 capture/proof 检查 |
| RV61 实际结果 | 十四 ELF 中九份改变，19 项用户态新执行通过，13 项用户态与 5 项 guest 内核成功按相同 ELF/命令/环境复用；强 TSO 本批实际新执行 220321 checks，通过 |
| 范围限制 | 两项 compile-cost executable 与 emit 仅构建；无新 guest 内核启动、物理 RV64、PMU/FEX 测试。自有 RV59/60/61 已确认 idle 后停止，其他项目容器未动 |

20 项结构检查和 20 项范围内证据检查通过。`complete_quality_gate_passed`、完整非退化/FEX/R7/总目标标志仍为 false；旧 main 失败、未归因成功计数及后续质量门均保留。

### 体积、布局与实际普通形状

DWARF 只取 `runtime/ir/block.h` 中的 Block：256→232 bytes；另一个 VIXL Block 112 bytes 不变，不能混算。PendingStore 32→24，48-byte UniformCaptureRecipe 与 24-byte PublishedCapture 不再存在。没有配方、published 或位置表的动态构建；pending/coverage 的峰值容量仍保留至块返回，额外范围查询、完整私有堆与总成本不由这些布局推导。

launcher 121231656 bytes，SHA256 `6a3ca76722e183e1745369a79c8666bcd49070e7a1eb574f9c274e3eaa60a3b7`；.text 4505348→4492588，减少 12760 bytes。DSE code 7512→5628、自身 SP 1344→1296；SinkStateStores 4548→3728、SP 672→480，flush 3272→2584、SP 208→112。CaptureLatestStateValues 的 3216-byte 过程与 320-byte 自身帧、HasObservableUse 的 1140/272 及位置准备子过程退役。这些不是整个调用链栈峰值或周期证明。

| 普通形状 | 捕获记录数 | RA host bytes | spill 变化 |
| --- | --- | --- | --- |
| Function CoreMark | 427→427 | 288496→288484 | 全部所比较指标保持，defs/loads/stores 为 455/131/84 |
| Function bench | 22→22 | 20320→20320 | 保持 159/266/147，完整有序形状相同 |
| Block CoreMark | 2414→2419 | 208472→208656 | 保持 1076/433/272，差异均位于 formatter，范围外有序记录相同 |
| Block bench | 101→101 | 16764→16764 | 保持 185/281/155，完整有序形状及 101 份捕获机器码相同 |

Function CoreMark 的两个共同 PC 发生变化，host 总量减 12，含 formatter 外差异；没有逐 PC 发射归因，不作完整代码相等声明。Function body dump 为空，不能由零条 dump 证明机器码等价。上批加强 fault 发表造成的累计压力没有被本批消除；相邻 spill 保持也不表示已通过最终性能门。

五组 workspace 基础六字段保持并闭合。Function Core 的 pattern_init_bytes 少 36；Block Core retention_events 增 53、pattern_init_bytes 增 284；慢 IR retention_events 增 20。两个 bench 全字段保持。账本不含所有已删除私有容器的堆和保留容量，不能宣称总内存/速度收益已经验收。

### 证据与滚动回收

主要记录为 `r2-state-capture-retirement-v4-{source-manifest,reference,structural-proof,test-summary,quality,cost,method-cost,layout-cost,shape-summary,storage-comparison,host-byte-comparison,rv64-closure,rv64-source-equivalence}.json`，以及额外配置的 `ra1blk0-execution.json`；实际 RV 构建/执行保留 `r7-rv64-execution-20261025-v61-*`。没有把复用记录的真实编译来源或时间改写成最终批次。

当前 native14/RV61 14 的检查点为 `r2-state-capture-retirement-v4-current-elfs.tar.zst`，451288180 bytes，SHA256 `ce927155896d049fb39ac3d1dcc48ae78e13135de14e84817e808a316cb7017b`；每项解压哈希核验，当前展开 28 份保留。相邻 orphan-v2 的原压缩归档保留并重新逐项核验 28 份，之后回收其展开副本 1856933888 allocated bytes。加上未执行 v2 和已替换夹具 v3 的两组各十四份，本批合计回收 56 份、5436190720 allocated bytes；删除前无目标活动句柄，清理前后 Git HEAD/status/diff 一致，未删除唯一失败证据。

v3 夹具错误现场另存 `/Volumes/Data2/swiftvm-middle-preserved-20261025/r2-state-capture-retirement-v3-fixture-error-checkpoint.tar.zst`，34318239 bytes，SHA256 `2008ab5a66b8163aa3735baa64b5846db84a8e5805179cb7f34a13f834cdde7b`，包含实际一个构造 error 的 ELF、完整源、设备哈希与原始 XML/log，逐项解压核验。仅编译失败的 v1 没有冻结 ELF，保留小量源码和构建诊断。所有大结果、源快照、编译 TMPDIR 和固定缓存继续实际位于 Data2；原系统盘清理的 277194584064-byte 释放量与本次 Data2 回收分开记录。

本批实现完整捕获配方退役及跨范围观察修复，是 production progress。继续逻辑 GuestStateFacts/EntryPolicy/version/CFG、真实恢复证明下的发表减量、其余 typed/SSA/helper 接缝和完整 R0–R7/FEX/动态成本；不再沿已经删除的配方、carrier 或位置表扩展新框架。

## R1/R3：共享状态定义身份，按候选建立物理版本（2026-10-25）

本批 `r1-guest-state-definitions-v1` 接续 `r2-state-capture-retirement-v4`。源码摘要 `1976b5d87aae0d94eaf26ec5bc5b88d5de58c87b79a04d4e9a8ab7393e3a249f`，patch SHA256 `8b554979e9dc5dbdb7e846aaee88e9be7c1281351f12355633701737cf83e881`。修改八份既有文件，生产净增 47 行；overlay 477、累计退役路径 118 不变。没有新增源文件、TestCase、目标、运行模式、开关、probe 或仓库测试脚本。

### 公共身份与物理结果分别归属

旧 ActiveState 为全块每条指令创建一个版本容器，复制定义身份表，并为非稠密/外块定义维护独立哈希。现在公共 GuestStateFacts 唯一保存实际可能发表的定义身份，物理视图只按候选数建立 active-version 行；不把忽略发表、coalesced write、提前 clobber 或入口高位的结果并入公共事实。

| 既有文件 | 当前职责 |
| --- | --- |
| `analysis/guest_state.h`、`guest_state.cpp` | GuestStateFacts 是实际 owning 对象，分别按需持有有序观察候选与 GPR 定义目录；原 GuestStateObservations 类退役，没有类型别名兼容壳。定义目录从 GetHostGPR/SetHostGPR 的真实来源出发，包含三种扩展别名链，以真实 Inst 指针排序和去重，不按 ARM64 home 或忽略策略过滤 |
| `analysis/semantic_facts.h`、`facts.cpp` | 唯一按需持有上述对象，分别提供 Prepare/Find；BeginInput 统一失效。新增块不会使原 payload span 失效，任何一族准备都不强建 index、uses 或 CFG |
| `arm64/compile/register_state.h` | ActiveState 借用公共身份目录，仅持有本视图的版本行和三十个 home 工作集。原全指令 owners/rows、稠密开关与 fallback unordered_map 删除；按指针二分确定行，外块同编号不会误命中 |
| `arm64/compile/register_versions.cpp` | 实际存在复用发表才请求目录；发表、扩展、高位、忽略、合并及提前 clobber 传递规则保持，失效操作按必有候选行执行。没有把该临时状态移交 emitter |
| 两份既有测试文件 | `block_analysis_index_test.cpp` 补实际定义/别名/外块身份、两种物理忽略视图和跨块稳定性；`hir_lifetime_test.cpp` 在现有 constant-window 模式补热视图真实 C++ 请求检查 |

目录建立时，本地唯一 u16 编号用短内联位图去重，完整稀疏编号跨度最多 8 KiB；外块来源独立按指针去重，故与本地相同的数字 ID 仍为不同定义。共享别名尾首次访问后早停，避免重复种子反复展开。先在局部完整构建，再发布到 owner；失败不暴露半份目录。观察与定义 payload 独立按需建立，其块记录增长不移动已借用的元素缓冲。

这是 GuestStateFacts 的部分 owning 实现：已共享候选身份与观察，完整 EntryPolicy、逻辑版本、覆盖/窗口证明、恢复摘要与 CFG 数据流仍未完成。物理视图继续独立播种；Block 与 Function 分配入口保持各自语义，不能因拥有公共目录而混用入口高位结论。

### 真实成本及限制

DWARF 显示 GuestStateFacts 为 48 bytes，原观察 owner 为 24 bytes；SemanticFacts 632、RegisterStatePreparation 1536 bytes 不变。ActiveState 1776→1720 bytes，公共块记录仍为 32、ActiveValue 仍为 24 bytes。减少的是按全指令数建立的热视图行与重复身份存储，不是所有堆请求。

冷准备仍包含一次块扫描、别名遍历、位图初始化、外块临时哈希及 K log K 排序；视图查询从稠密 O(1) 改为指针 O(log K)。共享目录保留至 facts 失效，比原单视图身份表寿命更长；失效后空行的外块容量也可能比旧哈希节点保留更久。不能由对象变小或热视图检查推出完整冷准备内存、吞吐或总编译收益。

实际 launcher 121231656→121171544 bytes，.text 4492588→4490836（−1752）。PrepareValueVersions 自身代码 6996→7092 bytes，自身固定 SP 减量 2448→2368；InvalidateHome 940→776 bytes，自身帧 192→208。新增目录主函数 1096 bytes/320-byte 自身帧，其别名去重 lambda 772 bytes/176-byte 自身帧另计。以上不包含调用者叠加、动态 payload 或整流水线峰值。

### 集中验证与普通代码形状

Android 与 RV62 均在当前完整源码上构建十四目标退出 0 后冻结。RV62 的 2556 文件输入摘要为 `bd00fa7461590942c72ad1f2a269bee6563ef20b6522ef610bb312ba523f4f40`，此次不需要上一批的 IR 夹具排除资格来代替完整源构建。

| 验证 | 实际结果 |
| --- | --- |
| 红魔主计划 | 15 项 native 与 4 种普通形状退出均符合预期；25 个原始成员和设备前后 ELF/guest 哈希核验 |
| 快 / 慢 IR | 各 113 cases，107181 / 107279 assertions，0 failure/error；没有新增、改名或删除 case，也没有 IR assertion 增减 |
| main | 755 cases / 484854 assertions，保持同样 11 个历史失败 case、66 个失败 assertion、0 error；不是全通过 |
| main 计数归属 | 相邻 484711→484854，实际 +143；既有 main 用例新增 29 项，余下 +114 未逐项归因。寿命模式的两项检查不计入 main，不重跑成功计数以凑相等 |
| 新增语义检查 | 外块/本地同 ID、三种扩展链、七个真实目录候选、大量无关指令、两个独立 ignored-publication 视图、十二块跨两族增长的 payload 稳定性及不强建其他事实 |
| 寿命/限定分配 | fast0/1 的 state-sink-failure 各实际 33 次注入、1690946 checks；constant-window 各 123483 checks，比相邻增加两项。预热后，8/2048 个无关 Nop 的物理视图重建要求真实非空 resident 结果，且非零 C++ 请求数和字节相同；不含冷目录成本 |
| 独立完整 Function 配置 | 同一冻结 IR ELF，以 `SVM_RA_1BLK=0` 仅执行两个既有 case，558 checks、0 failure；没有重跑整套 IR/main |
| cache / helper / predicates | 代码格式 39、AOT schema 6 不变；九份 native marker 符合预期，原四份 oracle 的 933552 comparisons 保持 |
| RV62 | 九份 ELF 改变，19 项用户态新执行通过，13 项用户态/5 项 guest 内核按完整相同 ELF、命令/属性、环境和原成功记录复用；强 TSO 新执行 220321 checks 通过 |
| 限制 | 两项 compile-cost executable 与 emit 仅构建，无新 guest 内核启动、物理 RV64 或 PMU/FEX 运行；自有 RV62 确认仅 sleep 后停止，其他项目容器未动 |

四种普通形状的 spill units/defs/loads/stores、pair fallback 均与相邻相同，CRC/checksum 正确且无 target overflow。Function core 427→426 条记录、host bytes 288484→288392；Block core 2419→2414、208656→208280。两个 core 的差异均限格式化函数，格式化外的有序记录相同，不把动态输出路径差异视为目录优化的性能收益。Function bench 22 条/20320 bytes、Block bench 101 条/16764 bytes 的完整形状相同；Block bench 捕获的 101 份实际机器码也逐字相同。Function 原始 dump 为空，不声称机器码相等。

五组 workspace 基础六字段保持且请求/释放闭合。Function core 的 retention events −15、pattern bytes −120；Block core −49/−904；两个 bench 与慢 IR 全字段相同。该 ledger 不覆盖新增公共目录、私有 STL 容器或完整堆成本。

17 项结构检查、21 项范围内证据检查通过。`complete_quality_gate_passed`、完整非退化、FEX、R7 和总目标标志继续为 false，历史 main 失败、未归因成功计数和累计 fault 发表压力仍开放。

### 磁盘清理与检查点

用户要求的系统盘清理已完成并重新核验：原结果与两个缓存路径均为指向 Data2 的零占用 symlink，按原 owner 目录独立测量共释放 277194584064 bytes；不将其他会话影响的全局 df 差值归入本线。原测量与本批 native/RV62 都已结束，无结果/缓存写句柄，协调者回执已更新。

当前 native/RV62 二十八份 ELF 压缩为 `r1-guest-state-definitions-v1-current-elfs.tar.zst`，451333754 bytes，SHA256 `a372b6a05085098233724ab85e5d50b0a74c3930be9afd9ee9b3d91e40d5ab97`，逐成员解压重哈希通过。上一批压缩检查点二十八成员再次验证、确认无打开句柄后，仅回收其 native14/RV61 实际14 展开副本，独立 du 下降 1850912768 bytes；该数属于 Data2，不重复计入系统盘释放量。当前28、旧压缩检查点、唯一错误和 PMU 证据均保留。大临时与固定缓存继续实际写 Data2，不恢复历史全量展开集。


## R2：统一 decode/pass 映射规则与局部分支状态（2026-10-25）

本批 `r2-guest-state-flow-v1` 接续 `r1-guest-state-definitions-v1`，源码摘要 `e21210ca6c9be95d4b47f97d6e6da9c0e34e31e00fee4abd9633b323d917dfc9`，patch SHA256 `7d8816b104728e271cb3549419805b3c1cdb99be77f94b2a9bb10684de8db4c2`。修改七份既有文件，生产净减 62 行；overlay 477、累计退役 118 不变，没有新增源文件、TestCase、目标、运行模式、开关、probe 或仓库脚本。

### 一个映射查询，两类调用方策略

decode 的 LoadUniform/StoreUniform 和残余 pass 的对应两条路径，原来各自解析映射、检查完整包含和连续区间、判断类型与标量 FPR lane。现全部调用 `analysis/guest_state.cpp` 的 ResolveGuestStateAccess，以真实访问类型返回 Invalid/Memory/Register 及原 binding。store 使用 Value 类型决定宽度，不能用 Uniform 描述符替代实际写入范围。

公共查询保留两项顺序约束：从未映射字节起步而跨入映射的访问仍拒绝；resident FPR 的显式 deferral 先选择内存路径，再考虑寄存器包含条件。原 scalar↔FPR lane 资格、连续区间规则与真实 context 边界一致，边界以减法检查。decode 仍额外要求 mapped store 全宽，部分写留给能看到完整字节上下文的 pass；非法 decode 形状保留 raw IR，由残余 pass 原诊断拒绝，不提前吞掉错误。

`builder/guest_state.cpp` 的多处 raw load/store 收尾合并，351→262 行。`passes/state.cpp` 503→458 行，四套重复的映射资格判定归公共查询；helper 范围、mapped GPR 更新、统计、DSE 顺序和 rewrite 归属仍由原调用方处理。ARM64 的入口高位、忽略/合并发表、提前 clobber、物理版本和 fault capture 源文件逐份哈希不变；没有把这些结果写进公共逻辑表。

### 一个临时分支快照 owner

GuestStateBranches 唯一定义 Remember/Merge/Reset。decode 与 pass 各自持有本次流的 owner；live 字节表仍由调用方按原 vector/内联 StackVector 存储。两份旧 unordered_map 声明、查找/尺寸检查/求交/erase 算法和 builder 的 uniform_labels 清理已退役，没有增加第二份状态载荷。

Remember 保存独立边快照，同一 key 更新复用其 vector 容量；Merge 只在存在且尺寸一致时调用原 MeetGuestStateBytes，随后销毁该快照。无快照/尺寸不合返回 nullopt，调用方执行原清空与计数；有效求交保留零字节时返回 optional{0}，不能误当“没有边”。所有 builder 出口继续 Reset，异常未完成单元由原 owner 回收。这里是封口前的临时逻辑传递，不能将其等同于已完成的 post-seal GuestStateFacts 版本/EntryPolicy/CFG。

### 成本与布局

Android DWARF 中 GuestStateBranches 为 40 bytes，替代原 40-byte map；IrBuilder 仍为 520。GuestStateAccess 为 9 bytes，GuestRegisterBinding 8；SemanticFacts 632、GuestStateFacts 48、RegisterStatePreparation 1536、ActiveState 1720 bytes 保持。快照 map 节点、字节 vector、哈希桶与保留容量仍存在，没有完整私有堆或零分配声明。

launcher 121171544→121122712 bytes，.text 4490836→4486300（−4536）。LoadUniform 1744→688、StoreUniform 1004→712 bytes，两者自身固定帧均 192→160；UniformAfterAppend 876→628 bytes，自身帧 96→64。新增公共查询 1092 bytes/144-byte 自身帧，Remember 140/80、Merge 284/112 分别另计。调用方帧下降同时引入公共调用，不能据此推导最大叠加栈、整流水线吞吐或 PMU/FEX 收益。

### 集中验证

Android 十四目标一次完整构建 0，90.16 秒；RV63 十四目标在当前完整源码上构建 0。其 2556 文件输入摘要 `d42c561c01f0f2b7b43354f55c240807c5e16c301a17873ef9e0cb9e6cdca2df`。本机 runner 曾在冻结工具返回 running 时过早读 plan，首个本地 preflight 因文件未就绪退出 1，未 push 或运行目标；随后消费同一冻结 handle 的终态 0，再开始第一次实际测试。该编排错误单独保留，不记为目标测试失败，也没有重建或重复执行来改写结果。

| 验证 | 实际结果 |
| --- | --- |
| 红魔主计划 | 15 项 native 与 4 种普通形状，所有退出符合预期；25 个原始成员与设备前后哈希核验 |
| 快 / 慢 IR | 各 113 cases，107208 / 107306 assertions，0 failure/error；同一既有 case 新增 27 项，与相邻实际各 +27 对应 |
| 新增边界 | GPR/FPR/scalar lane、未映射跨界、连续映射空洞、越界/VOID、显式 FPR deferral；独立分支快照、真实定义身份、缺失/尺寸失配、空交集、同 key 更新与 Reset |
| main | 755 cases / 484624 assertions，同样 11 个历史失败 case、66 个失败 assertion、0 error |
| 计数归属 | main 484854→484624，−230 未逐项归因；本批 main/lifetime 没有新增断言。IR 的 +27 不计入 main，不为成功计数凑相等而重跑 |
| native marker | 九份实际 marker 与相邻逐字相同；state-sink-failure 各 33 次注入/1690946 checks，constant-window 各 123483，cache39/AOT6 和原 oracle 保持 |
| RV63 | 九份 ELF 改变；19 项用户态新执行通过，13 项用户态与 5 项 guest 内核按相同 ELF/命令/属性/环境及原成功记录复用；强 TSO 实际新执行 220321 checks |
| 适用范围 | 两项 compile-cost 与 emit 仅构建；无新 guest 内核、物理 RV64 或 PMU/FEX 运行。未新增 RA1BLK=0 配置，旧 558-check 结果保留其原批次身份 |
| 资源 | 自有 RV63 确认只剩 sleep 后停止，其他容器与项目未动 |

Function core 的 426 条记录/288392 host bytes 与相邻完整有序记录相同；Function bench 22/20320、Block bench 101/16764 同样保持。Block core 2414→2416 条、208280→208272 bytes，新增五个/缺失三个 PC，无共同 PC 形状改变，差异均为格式化路径，格式化外有序记录相同。四种 spill 指标一致且无 overflow；Block bench 的 101 份实际机器码逐字相同。Function dump 仍为空，不作机器码相等声明。

五组 workspace 基础六字段均保持且闭合；Function core、两个 bench 与慢 IR 全字段相同。Block core 仅 retention events +21、pattern bytes −468。该 ledger 不统计快照私有 STL 容量和完整编译成本。

18 项结构检查、21 项范围内证据检查通过；完整非退化、FEX、R7 与总目标继续开放。对原 main 失败另做只读来源核对：两项夹具硬编码 /tmp；八项 AFP 配置在测试中强制打开，而设备公开 cpuinfo 未报告 afp；一项为 direct-link 并发进度超时。分类不是通过，未跳过任何测试，也未据单份 cpuinfo 推断每颗核的完整能力；需继续查实际 capability、夹具路径和并发时序。

### 检查点与及时回收

当前二十八份 ELF 压缩为 `r2-guest-state-flow-v1-current-elfs.tar.zst`，451244374 bytes，SHA256 `74329def837f58e56b43a7ca65691c5460c8dc1a0643e11b413cf539c8b37b66`，逐成员解压哈希核验。相邻 definitions-v1 native14/RV62 实际14 展开副本在旧压缩档二十八成员重新验证、确认无打开句柄后回收，Data2 独立 du 减少 1850490880 bytes；两份压缩检查点与当前28保留。原系统释放 277194584064 bytes 单列，不将本批 Data2 回收叠加。结果和固定缓存继续实际写 Data2，协调者回执同步更新。


## R1/R3：共享编号后 GPR 窗口输入，分离物理 clobber 投影（2026-10-25）

本批 `r1-guest-state-windows-v2` 接续 `r2-guest-state-flow-v1`，源码摘要 `bb333e35fe225c99f55acc46dc615a26d52d6892a96d28dbec0a75df144e790e`，patch SHA256 `f240434201918dc720f755934faf1bbcdafcf496bee37e7cca2f8ef527dcbd52`。修改十份既有文件，生产净增 81 行；overlay 477、累计退役 118 保持，没有新增源文件、TestCase、目标、运行模式、开关、probe 或仓库脚本。

### 静态输入与物理策略分别持有

原 HomeAccessIndex 在每个物理视图中扫描全块，把 Get/Set 位置与 helper/Xchg clobber 一起扩成按 home 排序的 key。Get/Set 是封口 IR 的固定输入，helper/Xchg 的实际 mask 则依赖目标和 FeatureSet。现在静态部分按需归唯一 GuestStateFacts，目标只构建自己的 clobber 投影。

| 既有文件 | 当前工作 |
| --- | --- |
| `analysis/guest_state.h`、`guest_state.cpp` | GuestStateGPRAccesses 保存按 home/读写角色/u16 位置排序的 Get/Set key，及潜在 barrier 指令身份；FirstAccess/Barriers 只读且不构图。GuestStateFacts 在完整构造后发布 unique_ptr，跨请求块增长维持 pointee 稳定，独立于观察和定义目录 |
| `analysis/semantic_facts.h`、`facts.cpp` | 显式 PrepareGuestStateGPRAccesses 与 const Find；BeginInput 随原 owner 统一失效。无需准备 CFG、consumer/use 图或 flags。现有六种潜在 GPR barrier 分类由公共 constexpr 谓词唯一声明 |
| `analysis/block_summary.h` | 利用既有八字节摘要的 spare bit 记录 GPR window 候选；Get/Set 与潜在 barrier 都纳入，同次基础扫描收集，不新增逐指令表 |
| `arm64/compile/register_state.h`、`register_state.cpp` | 原 HomeAccessIndex/HomeAccesses/packed-key 构建退役；借用公共静态索引，目标仅持有按位置的 {position, homes} mask 投影和 home union。独立解析当前 FeatureSet 的 helper/Xchg，查询合并首个静态写与实际 clobber；发表窗口另外检查静态读 |
| `register_widths.cpp`、`register_versions.cpp` | 两个 Begin 入口重置新视图；入口宽度算法仅更换 cache reset 名称。版本传递主体保持，只把原私有六-op MayClobberFixedHomes 谓词接到公共候选，原函数删除 |
| `block_analysis_index_test.cpp` | 在既有 case 中验证原始序列 oracle、不同 helper/FeatureSet 视图、空摘要、输入失效、十二块三族增长和稳定引用；没有新建 case |

潜在 barrier 当前为 CallLambda/CallLocation/CallDynamic/X87Op/Sse42Str/XchgBarrier，与旧版本传递谓词逐项一致；实际是否 clobber 仍由目标解析。公共输入不保存物理位置、高位、忽略/合并发表的结果或入口 seed。新增 helper 家族需同步公共候选声明，不能只扩展目标 resolver。

### 门控、查询与寿命

首次构建仍由三次成功且跨度至少 63 的 survival 请求，或显式 first-clobber/window 请求触发；同一封口输入已准备时，新物理视图立即复用，不再先做三轮扫描。已有 FindSummary 表明整个 GPR window 家族为空时，直接完成空准备，不创建公共窗口记录或物理投影，也不为这个判断强制 PrepareSummary/flags。

共同输入只排序一次；helper/Xchg 每个物理视图重新解析为 mask，每个实际 barrier 一条八字节记录，替代旧按 home 展开的多条位置。home union 先排除该 home 没有 clobber 的投影；相关查询二分 after 后仅扫描必要 barrier，遇到首个静态写/before 即结束。该相关 barrier 扫描与旧单表查找的复杂度不同，不能预记为更快。

忽略位置和开区间边界保持。发表观察仍使用独立 observation 候选及原 target/fault-width 复证，未把 home-only 查询强制变成 observation 准备。BeginBlock/BeginFunction 清理借用和投影；失败重试先丢弃部分 mask，容量按原准备 owner 复用。公共记录存活至 facts 失效，长于旧单视图索引，准备结果不把它交给 emitter。

这批补齐的是编号后的静态窗口输入；完整逻辑版本、EntryPolicy、覆盖/恢复摘要和 CFG 状态流仍开放。Block/Function 的分配能力与物理入口高位、忽略/提前 clobber 播种继续独立。

### 实际存储和代码成本

Android DWARF：GuestStateFacts 48→72 bytes，新增共享访问记录 56 bytes，另有八字节 roster slot；RegisterStatePreparation 1536→1544。原 24-byte HomeAccessIndex 消失，物理 ClobberProjection 为 32 bytes，mask entry 为八字节。BlockSummary 8、SemanticFacts 632、ActiveState 1720 bytes 保持。共享 key 四字节、barrier 身份指针八字节；独立 mask、所有 vector 容量/allocator、较长事实寿命及只读家族的冷准备成本仍需完整计量。新增窗口索引/投影自身的分配失败注入尚未扩充；既有 state-sink-failure 模式通过不替代该路径的完整寿命验收。无 ready summary 的显式空请求仍可创建空记录，不声称所有空查询零分配。

launcher 121122712→121339080 bytes，.text 4486300→4493700，实际 +7400。旧 PrepareHomeAccessIndex 1740 bytes/208-byte 自身帧、HomeAccesses 360/96 退役；新 PrepareGPRWindows 936/176、FirstClobber 656/128。PrepareHomeSurvival 560→508、自身帧 144 保持；PrepareFirstClobber 344→244、112 保持；PreparePublicationWindow 1176→948、160 保持。

新增 SemanticFacts prepare wrapper 328 bytes/128-byte 自身帧、GuestStateFacts prepare 420/112、公共构造 1184/192、FirstAccess 304 bytes（未见自身常量 SP 减量）分别另计。局部自身帧不能代表新调用链总栈；体积增长、metadata 和相关查询工作均保留为成本项，没有完整编译吞吐/内存或 PMU/FEX 收益结论。

### 构建修正与真实执行来源

v1 首次 native 构建实际退出 1，新加 main 夹具遗漏 swift::u32/u64 限定；没有冻结或运行 v1 native ELF。v2 只修正该文件九处类型拼写，十四目标增量构建退出 0，实际仅重编该对象并链接 swift_test，4.82 秒；失败源码/patch/log 保留，生产代码未因夹具修正变化。

RV64 在 v1 的完整 2556 输入上构建十四目标退出 0，实际输入摘要 `7a8da91f7d3429334eea6e5d5465ba9c37ab0afacb199dbb007211a72b4c0e11`，实际 overlay 源码摘要 `e2776d58d7d1e0e265e14421068605ce037f4f055665fb2de07f6a82589f0696`。实际 Ninja 十四目标 440 条命令及 298641 行依赖均不含 block_analysis_index_test.cpp（包括 basename），其余输入相同；因此以独立 source-equivalence 记录资格化最终 v2，RV 原记录保持 v1 来源，不称为 v2 全源 RV 重建。第一次 RV 冻结 preflight 因 live repo 已修夹具而拒绝；完成上述依赖资格后才冻结、执行，没有重建或重跑 RV。最终文档变动另以相同实际闭包排除。

| 验证 | 实际结果 |
| --- | --- |
| 红魔主计划 | v2 的 15 项 native 与 4 种普通形状退出符合预期，25 个原始成员及设备前后哈希核验 |
| 快 / 慢 IR | 各 113 cases，107208 / 107306 assertions，0 failure/error，与相邻计数和 roster 相同 |
| main | 755 cases / 484552 assertions，同样 11 个历史失败 case、66 个失败 assertion、0 error |
| 新增 main 覆盖 | 既有 case 的 23 项检查通过；静态查询独立走原始指令序列，涵盖读/写、忽略位置和边界，另覆盖全部六种 barrier 输入、两种 helper view、摘要跳过、输入失效及跨块稳定对象 |
| 计数归属 | 484624→484552，实际 −72；新增 main 为 +23，余下 −95 未逐项归因。IR/lifetime 没有新增检查，不为计数重跑 |
| native marker | 九份与相邻逐字相同；cache39/AOT6、933552 oracle comparisons、helper-slow 17178 checks 保持，state-sink-failure 两个 fast 各有 33 次实际失败注入/1690946 checks、constant-window 123483 |
| RV64 实际 v1 | 九份 ELF 改变，19 项用户态新执行通过，13 项用户态/5 项 guest 内核按完整相同 ELF、命令/属性/环境和原成功记录复用；强 TSO 实际新执行 220321 checks，通过 |
| 范围与资源 | 两项 compile-cost executable 与 emit 仅构建，无新 guest 内核、物理 RV64、额外 RA 配置或 PMU/FEX 运行；自有 RV64 确认仅 sleep 后停止，其他项目未动 |

Function core 426 条/288392 host bytes、Function bench 22/20320、Block bench 101/16764 的完整有序形状与相邻相同。Block core 2416→2414、208272→208280 bytes，无共同 PC 形状改变，差异均为格式化路径，格式化外有序记录相同。四种 spill 指标一致且无 overflow；Block bench 的 101 份实际机器码逐字相同，Function 原始 dump 为空而不作字节相等声明。

五组 workspace 基础六字段保持并闭合；除 Block core 的 retention events −21、pattern bytes +468 外，其余四组全字段相同。该 ledger 不覆盖新增共享/私有 STL 容量。23 项结构核对、23 项范围内证据核对通过；完整成本/非退化、FEX、R7、总目标标志继续为 false，原 main 路径/AFP/并发问题与累计 fault 发表压力仍待处理。

### 检查点及清理

当前 native v2 / 实际 RV64 v1 共二十八份 ELF 压缩为 `r1-guest-state-windows-v2-current-elfs.tar.zst`，452260262 bytes，SHA256 `eafffa9f7f8987a84ce5a74cd44d3f6af38f34678f3bccfb6fde0ec61d55fe61`，逐成员解压哈希通过。相邻 flow-v1 native14/RV63 实际14 展开副本在旧压缩档二十八成员重新核验、确认无打开句柄后回收，Data2 独立 du 减少 1849974784 bytes；两份压缩检查点、当前28及唯一失败/PMU证据保留。原系统盘释放 277194584064 bytes 单列不累加，大结果与固定缓存继续实际写 Data2，协调者回执已同步。


## 下一批交付

公共 owning Optimize/Prepare 入口、SemanticFacts 的唯一归属、AllocationView/Writer 与 ARM64 AllocationPreparation 分离、显式分配输入及公共物理执行已接入生产。R4 的局部事务、narrow-preserved 和 Bind/replay 退役已落地。FPR 契约、scalar FPR、select/token、narrow/self-XOR、resident/carry、memory、consumer/PSTATE、窄整数 flags/dead-edge branch 及本批 GPR 发表/pinned memory 选择已接入 UnitActions；各批证据与限制见前文。这些成果不等于完整 R1/R3/R5 或正式 R0–R7 验收。

此前公共 Void/control token、读取/meet、完整捕获配方退役、跨范围观察和 decode/pass 规则共享已落地，代码格式维持 39。GuestStateFacts 当前独立持有观察、定义与编号后 GPR 窗口输入；原每视图混合 HomeAccessIndex 退役，物理 clobber 仍按目标/FeatureSet 投影，入口高位与忽略/提前 clobber 播种独立。native v2 与实际 RV64 v1 按真实编译闭包资格化，范围内正确性/普通 spill 验证保持；新增 metadata、较长事实寿命、查询/调用链和 +7400 text 的成本尚未完整验收。继续 EntryPolicy、逻辑版本、覆盖/恢复及 CFG 流，接续完整动态成本、累计 fault 发表压力和 FEX 门；原 main 的临时路径、AFP 能力与并发进度问题继续按实际证据处理。

旧 R2 阶段记录中的 capture/published/位置/consumer 余项现已由整体退役收口，下面的历史累计清单不再要求扩展这些对象。后续重点为唯一逻辑事实、实际 typed 写、故障恢复和跨块契约；尚需计量的 pending/coverage 私有容量及完整动态成本继续保留。

余下工作继续按原目标推进，合并相关修改后再集中验证，避免逐文件反复上机：

- R1：基础 facts/摘要、按需直接/resolved 图及普通块零构图已接入；memory-feeder 与 spill 参数合扫，low32/固定发表收紧构图，consumer 候选已迁入外层工作区。Select/consumer/branch 的入口整图预准备随后已退役，实际边与局部 pseudo 查询通过显式可变入口共享同一索引；raw-carry 与 incoming-zero 自己声明依赖，13 种触发形状及 roster-only 分支验证通过。查询代码增长、首次构图更深的静态栈链和完整成本按上文保留。value/width 无候选时跳过暂存 epoch，仍有资格预扫；纯整数位事实已接入 decode/残余折叠。工作区已拆为独立文件，九类暂存的请求/释放、逻辑峰值、闲置容量过程和借用结束超限回收已接入；分配 writer 独占工作区、三个显式批量初始化点及独立工作区并发统计也已验证。整单元声明 GPR home 已并入基础摘要，FPR tie/publication 共用一次局部投影；捕获寄存器原始视图宽度/末次位置已由实际候选触发共享稀疏缓存，旧无条件哈希收集退役，该缓存使 owner 增长八字节；FunctionCFG 结构已由活跃性/入口宽度共享，传播策略独立；本批将 CFG use-end 与入口依赖移入按需 FunctionValueFlow，纯图仍为 72 bytes，SemanticFacts 为 624 bytes，独立 Flow 为 64 bytes，新增状态边结果的成本另计。CFG 活跃性四组生产哈希集合与复制固定点也已退役，改为 value_flow 文件中的稀疏需求传播；独立定义行表随后也已删除，定义块暂存在既有端点缓冲高半部，返回前清除；专属 workspace 参数退役。五组 bench 的旧九类工作区完整计数恢复到引入定义表之前，其他传播暂存及输出清理成本仍待完整计量。prepared 的两个独立 Block/合并 region 已验证工作区复用、真实扩容/超限回收、创建线程退出、结果移动与主 owner 先释放后的顺序延迟发射；已封口 OptimizedUnit 及原缓存 facts 在优化后、分配前的顺序 worker 交接，及 facts/两线程工作区结束后的发射也已验证。独立 pass 的十五个隐式便利入口和十四处 scope 获取随后已退役，测试以资源夹具保持显式上下文，state 家族 helper 边界及两处内联的空入口成本继续收口。继续完整单元语义宽度摘要、其他 CFG 视图/候选扫描、其余 GPR/publication/flags 暂存迁移、完整初始化/搬迁成本，以及适用并发准备、失败、发布/运行验收；不以单个路径计量或固定布局替代完整成本收益验收。
- R2：fault sink 已退役整块原始指针副本，并仅为配方请求准备源位置；四处状态只读参数快照也已退役，直接链遍历、提前观察与缺失边界已验证。selected/victim/remaining 随后已退役，coverage/pending 容量按实际工作复用，published 仅为配方准备，独立字节及限定 C++ 请求回归已通过；detached-store 的扩容/重连/部分压缩持有及限定分配失败回收随后已验证，失败输入交回原 owner 后销毁；capture/published 字节载荷随后已收为完整槽候选与唯一请求字节掩码，dirty/slot 中转退役；CaptureSlot 与其排序特化随后已退役；coverage、carrier 与位置查询随后已按实际需求收窄；继续有序候选/稀疏哈希和必要 observer、包围区间内部空隙、峰值容量/初始化/保留时间、其余改写故障矩阵及完整动态成本。受影响 value/width worklist 已接入 DCE/flags 反馈，旧 ConsumerIndex 退役；公共条件/flags 位查询、helper 写范围校验及 ClearFlags 的提交前后/融合消费已接入并验证。标量常量整段哈希已退役为惰性八指令窗口，replacement 为空时不访问参数改写，特定不同标量输入的初始 CSE 零堆请求已验证。本批活跃 epoch 的常量/删除窗口反馈已接入同一队列，普通规范化优先于有序 CSE，原始视图、pseudo/控制参数和跨块导出保持；活跃 epoch 的初始 replacement 暂存及逐参数查表已退役，无 epoch 路径仍保留轻量替换表；初始有序候选/资格预查继续收口。首阶段 Simplify 已并入名册访问，删除其独立播种；构造只准备任务、保持原处理顺序，All 的 CSE 后宽度重新播种仍必要。十四个目标及集中回归/静态、二十四项两版 PMU 已记录；无新增 owner/缓冲，完整私有成本未闭合，周期/耗时正向变化保留待归因。队列可挂两类任务且自身帧增加，指令截距上升及 pthread/Scudo 代码增长保留待处理。decode 的全寿命立即数来源哈希随后已退役为按 append 位置排序、提交后裁剪的历史，延期 width、外部来源、跨块与能力冻结语义保持；owner 缩小十六字节、局部新函数成本和最终验证复用范围按上文记录，长指令历史/私有堆与正式 PMU 仍开放。Function DCE 随后改为在活跃 epoch 复用既有存活行和归属，重复 owner 哈希不再建立；进入/退出的必要重新播种、无 epoch 独立 DCE 和 Block 逆扫均保留。代码增长 408 bytes 与 QueueDead 自身帧增加十六字节另计，完整成本仍开放。Function flags 的 live-in/transfer 两份逐块哈希随后合为一份短期状态表，并删除每次 terminal 查询的目标 vector；必要固定点、未知出口、局部 label 摘要及后继 PC 匹配保留。launcher text 减少 3400 bytes，入口/块清理与递归 visitor 自身帧增长按上文记录；terminal 借用与六份 branch-only 临时容器随后退役；本批前置需求接入终结/condition 基本形状门控，carry 两份区域数组和只写统计退役。局部 label 两张哈希随后已合为独立 FlagFlow 的一个惰性有序快照缓冲，纯 transfer/CFG 求解移出 flags.cpp，独立 Block 不建立 Function 表；晚期拒绝/marker-only、额外绑定准备访问、容量保留与完整成本仍开放。构造前结果类型规则、setter 推导退役及基础类型/数据可用性合流已分别完成；已存储参数形状随后已由 ArgSignature/CheckArguments 收口，HIR UseInst/UnUseInst 共用带位置枚举；构造前参数种类/容量随后已通过 constraints 和动态先检查接入，metadata vector 改为 constinit span；具名签名结果现已贯通共享入口，动态参数门只校验一次，构造主体/launcher text 减少及动态包装成本按本批记录；十四项标量基础整数域、selector 和计数输入宽度已接入共享检查，Block/Function 的新增准入及 Block 实际执行已验证；继续其余 opcode 位宽/值域、完整窄视图/转换与 CFG/Phi 契约、初次候选播种、更完整的 guest-state 提交/观察 effects 与已知 helper 细化，消除新增工作和原扫描的叠加成本，并收口完整动态成本、代码体积及周期代价；Unknown flags 输出继续保守处理。Value::SetType 和反复 uniform restamp 已删除，不能据此认定全部 R2 完成。 同宽整数谓词与三处 S64 W/X、四类整数转换、八类移位及整数谓词/条件视图已完成范围内验证；继续窄算术/整数比较、bound-decode 全配置、其余类型与 CFG 契约，完整成本和成功断言差额仍开放。
- R3：compiler 已集中目标编辑封口、整单元 fixed 输入绑定、必要证书、可选 audit 与最终动作冻结。两种物理冲突索引已改为显式准备，const 查询不建表，Freeze 无消费者预热退役；branch 只接收 AllocationView 和身份绑定的只读 home 索引。preservation 的三十二份 vector 已合为单一缓冲，索引为 40、writer 为 192 bytes。memory 已拆出可变 PrepareMemory 和只读 selector/lowering：roster 查询不构图，窄加载只补直接 uses，首个物理 post-index 候选通过 preservation 窗口后才补 resolved uses。可选 scalar FPR 已拆出 PrepareVector：一次投影与 consumer 共用独占候选缓冲，仅实际 load 目标的物理浮点 owner 请求 resolved uses，仅选中的两用转换补直接 uses；lower_vector 只读输入。raw low/high/extension 边宽度守卫已补齐，便捷构造的 U64 返回视图继续合法。局部候选、十二字节窗口、局部表初始化和两类索引动态容量成本仍需完整计量。必要 FPR 也已分离为 fpr_preparation 的实际物理候选/图准备和 fpr_contracts 的只读复证，owner 仅投影相关 home，四状态缓存按实际块位置借用既有工作区；过期索引不再返回有效空证明。scratch 已改为显式 pseudo 图准备、只读 range lowering 与精确 MEM occurrence 运输；等值事实只合并本块连续编号，正常已准入发射不再以扫描补缺失记录。永久 SVM_PROF2 字段记录准备/封口容量域，仍须区分旧未计量图、扩容重叠和实际分配。必要 fixed 复证的三十二份 writer vector 与消费哈希已收为完整键的有序缓冲，spill ownership 四棵关联树及 prepared-values 查找已合为一棵有序映射，一次 raw 访问保留完整 occurrence 和独立 alias ID；本批 consumer flags 仅由实际标量 MEM occurrence 触发，定义定位借用唯一块索引并在 pair/transfer/handoff 共用，terminal 接入共享只读遍历；块处理自身栈帧增加 64 bytes，周期/耗时正向变化保留待归因。本批 W-write 根/写入规则已按 Raw/Resolved 策略共享，两个 capture 物理例外保持；五份 BitCast 解析及两份 terminal 递归退役。发表收尾复用查询索引，三组临时排序容器删除，外层自身固定帧增加 64 bytes；新根查询自身帧和合并 PMU 的 Block 周期/耗时截距正向变化保留。终结 reload 与 hoist 三处原始值遍历随后接入共享 VisitTerminalValues，删除两份本地递归及 scratch 的分支复制；各自 occurrence/别名/浮点边界保持，hoist 自身固定帧增加十六字节另计。宽度复证的五组三十二份 vector 随后已退役为两份候选 home 索引，prefix 归 owner；3936→144 bytes 的临时对象和固定帧、688-byte 新查询函数及点位四→八字节的成本已记录。两组边界用例、十二项 native 与十组静态已验证，私有容量/复用及整体编译成本仍未验收。随后 fixed/width 的终结哈希已归公共 Raw/Resolved 成员查询，只读 Check 的三组丢弃证书退役；对象增至 160 bytes，必要/可选入口自身帧为 288/240 bytes，fixed Block 自身帧增加十六字节及新构造/插入成本另计。两组新 IR 用例和十二项 native/十组静态已通过范围内核对，主套件成功断言数的 −142 差值及未逐项归因边界按上文保留。继续其余 fixed/spill/helper ABI 接缝、候选暂存与完整动态成本；保留普通 attempt 的基础 fixed/scratch 检查、全部目标编辑后的含 target-write 最终检查和独立 FPR retry。公共 Finish 只清理准备状态，不能据此宣称两次 fixed 检查重复。owning 包装、删除旧 alloc 或收拢 Finalize 不等同于完整 R3 完成。
- R5/R6：旧 alloc 目录及全部旧 pinned、region/backedge/PFAF/edge-flags 文件、GPRBlockPlan 和每块子结果 shared_ptr 已退役。必要入口证书与最终边/原子动作归 UnitActions，临时驻留/发表选择归 ResidencePreparation；必要证明/effects 在 compile 下接续。PushRSB、gap 指令计数与 IsStrictInternalAdvancePC 尾部准备已移出 emitter；末条主体改为按实际 suppression 从尾部定位。fixed/width 共用 terminal Value 遍历，普通循环图/回边候选共用原始目标缓冲，独立 audit 保持。本批共享 host-call 的 Function 目标选择已移到显式 call_preparation 并按既有 presence 触发，emitter 全段计数/树节点退役，冻结地址与可变 Label 分离；UnitActions owner 增长八字节，临时计数/数组/标签及完整成本另计。既有 flags1 持久化夹具已补跨块共享 CallLambda/两种 SSE helper 的实际执行、不同进程地址下磁盘重载与 AOT 安装；flags0 的适用 helper 验收、真实 guest helper AOT producer 和代表性性能仍待接续。PF token 物理后缀已改为固定最后两项记录，删除 62 个临时 vector；私有 Candidate 与 points 随后也已改用同一共享候选缓冲的固定前段/复用尾段，单点直接采用 producer ID。新旧保存与物理窗口规则保持，普通 Candidate 的记录为四字节，source 角色不外逃；launcher text 增加 1296 bytes、自身帧增加 32 bytes 与新 seed 子函数的成本按上文记录，调用方发表投影及完整成本仍待接续。普通判定与 preserved 点位完整扫描分开，后者和必要 clobber/宽度证明保留。终结位置发表 Prepare/Defers 及 emitter 选择集合已退役，静态 ID 归 UnitActions，实际 scratch/标签归编码；新增 owner 八字节、两类暂存及完整成本另计，既有 call 持久化夹具随后以自然压力补齐 flags1 Function 的实际目标 spill backing、十二次缺失入口 fault、独立进程磁盘和 AOT 安装恢复，快/慢 IR 均通过；具体未知 scratch、flags0、独立 Block 与完整并发边界仍待接续。Div128 旧 jit 分析类/文件及构建项随后退役，纯 indexed selector 已归 lower_scalar，准备后的 scratch 位与 emitter 保持；signed 实际 Block 和有界别名/所属块用例已验证，专门的多块 Function native signed/真实宽除法、跨 helper/边结果和磁盘/AOT 恢复随后补齐，region pair-call 改为显式 BL 重绑定；完整故障家族仍开放。新增 JitContext 二十四字节和 CodeCache 八字节、region body 及调用点暂存成本继续计量。Select 的独立 Block 参数及 intrusive 安全窗口、整数分支的线性 condition 定位随后已接入共享索引，ScalarFPRLiveness 的两份生产文件及唯一测试 helper 已退役，实际 FPR 合并来源由四种 native 形状验证；LowerSelects 自身代码增加 96 bytes，LowerIntegerBranch 自身帧增加 16 bytes，完整成本保持开放。Select 共享计数与 branch discarded 的临时 map/set 随后也已退役，顺序借用既有候选工作区；局部帧降低与 Select 代码增长按上文记录，不替代完整成本验收。固定 home 的静态状态随后已归 register_* 准备，旧 GuestStateMap 五文件和 const 惰性建表退役；header/载荷/二分和调用栈成本分别保留，ActiveState 与公共逻辑仍待接续。继续 pinned 候选/窗口合并、其余条件融合、其他家族的临时计数/集合与图触发、RISC-V/AOT 和其他调用方及剩余共享 CFG/准备扫描；非零四指令审计模型覆盖与正向周期变化仍待接续。
- R7/R0：清理其余旧 pass 与兼容入口；定义归属、块内先定义后使用、唯一编号与跨块支配已加入 ARM64 必要证书，call-return 逻辑边已贯通 RPO、SSA 支配、活跃性及独立 audit。本批已接入 relay/入口宽度依赖、强制普通局部传递、私有 body 不导出、外部与环恢复 canonical 重建，以及静态/间接调用的空 continuation 返回回退；真实 disk/AOT round trip 已验证。前向局部分支的必要路径证明也已接入，含 terminal/提升锚点和 HIR 导出；继续后向 label 的局部循环活跃性、完整类型/SSA 规则、任意 fault 恢复和完整 guest-call 物理驻留保存；真实负载重新进入原因与避免过度私有化的成本仍需接续。恢复后的 Call source 在 callee 执行期间退役/失效的具体失败已定位：callback 写越界使旧结果不可靠，flags1 的旧返回预期遗漏 region SMC poll；扩大夹具存储并改为精确 CodeMiss/PC 后，以额外 request、返回入口撤销、NZCV、帧和再次进入断言补强，十六组原生用例通过；生产 Runtime/JIT/SMC 未改。前一批 RV64 v51 在 register-state-preparation-v2 完整源码构建十四目标，十七项用户态新执行通过，十五项用户态/五项 guest 内核及强 fence TSO 220321 checks 按完整相同 ELF、命令/属性、环境和实际旧成功记录复用，保留原来源与外层失败信息。两个 compile-cost 与 emit 仅构建，三十七项仅 RV64 专用注册范围。继续完整适用的故障/寿命/关闭/并发矩阵和物理 RV64 验证；这些模拟结果不替代剩余契约与成本门。固定 Android FEX 的补丁及构建配方已归 tools/fex-android，干净重建后首次自动启动、CoreMark 两点和 bench 已通过，非法服务目录继续拒绝。继续参照版本适用性与能力对齐，再分别验证 Block/Function 和正式三方 PMU，单组 cycles/wall 差异与代码增长仍须归因。旧设备 FEX 的源码提交不追认，新重建产物也尚未通过正式三方质量门。


## R1：封口 GPR 字节版本与显式入口策略（2026-10-25，versions-v3）

本节是当前状态，接续 windows-v2 的共享访问输入。已修改八个既有文件并新增一个实际生产文件 `analysis/guest_state_cfg.cpp`；production 净增 382 行，source overlay 478 项、retired 118 项。没有新增测试目标、运行开关、临时 probe 或仓库测试脚本；在既有 main 文件新增一个有六个 SECTION 的语义用例，并扩展既有 constant-window 失败模式。

- 最终 source SHA256：`17dfc7964d0f8d2d3d8aa645458ff2cdf9395c56f8b79d1ea786a65e7a45eb8f`。
- 相邻 windows-v2 patch SHA256：`3ac954f7f94d23fbfded532540fb3a7eea4123ecb6cacf5c98becbcb9c2da8a4`。
- 代码缓存格式 39→40；AOT schema 保持 6。native cache-version 用例以 40 为下限，并实际验证旧格式拒绝。

### 实际职责与语义

`GuestStateFacts` 第四族是按需的逻辑 GPR 版本证明，前三族及其地址稳定性继续保留。`GuestStateEntryPolicy::Block/Function` 由实际准备上下文显式选择；初始约束的独立块视图保持 Block，后续 BeginFunction 视图使用 Function。单块 Function 的区间优化不能改变其入口策略。Function 借用唯一公共 CFG；外部入口、函数入口、call-return 和缺失前驱不继承猜测的状态。

查询证明某指令之前的指定逻辑字节，部分写仅覆盖声明字节；透明 alias、零/符号扩展及整字节提取保留正确来源。缓存包含 expected Value 的使用类型，避免把同一 Def 的窄/宽视图混同。局部前向 label 同时验证 taken 与 fallthrough；Function 回边按字节工作表收敛，遇到源定义块入口即拒绝跨代动态 SSA 身份。只读且非重入的直接 helper 可以保留逻辑状态，未知/重入/写入边界保守失效。

实际消费者是 `RegisterStatePreparation::PreparePublicationWindow` 的完整 64 位整数路径。原 clobber/读取检查及后续物理准入继续执行；窄值 dirty high、fault capture、CurrentEntryExtensionFacts 与 DefinitionExtensionFacts 没有合并。Find 不分配、不扫 IR；合法负答案与未准备不同。未封口或 SSA 未通过的输入不发布新证明。

### 构建、运行及成本反馈

V1 native 全目标构建退出 1：仅新夹具对 const Value 调用非 const SetCastType；没有 native freeze/运行。V2 修复夹具并补转换数值检查，全十四目标构建 0，完成 16 项 native、4 项普通负载和 4 项既有 static-latch fault 模式。成本检查发现公共准备函数被内联成 848-byte 帧；V3 压紧 LogicalByte 并分出冷 constructor/Prove，再以最终源码完成十四目标构建 0（增量 5.01s）及同一相关回归。V2/RV65 只构建、未运行，不冒充最终 RV66 的运行证据。

| 当前红魔实测 | 结果与限制 |
| --- | --- |
| 新语义用例 | 1 个 TEST_CASE、6 个 JUnit section 分项、44 条断言，全通过；覆盖 partial/write high、转换字节、helper/reentry、局部交汇、三类 Function 入口及循环动态实例 |
| 主套件 | JUnit 761 分项、484692 断言；原相同 11 分项/66 条断言失败、0 error，全部共享签名一致；相邻总数 +140 = 44 已知新增 +96 未归因，未为凑计数重跑 |
| IR fast1/fast0 | 各 113 分项，107208/107306 断言，0 fail/error |
| constant-window fast1/fast0 | 各 123578 checks、16 个实际分配失败注入点；相邻 +95 checks；覆盖 Block 版本缓存及 target clobber projection 的失败无答案、重试与热查询不分配；不宣称覆盖 Function 大图全部分配失败路径 |
| 旧 sink failure fast1/fast0 | 各 1690946 checks、33 injections；保持不变 |
| call/cache 既有模式 | 13/99/13/111 及 cache172；九个 marker 与成本修正前 V2 完全相同 |
| static-latch fault | 原生 call/branch 与 AOT call/branch 四模式 0；分别 38/33/45/40 checks，flags1/latch1；这是既有 signal/AOT 回归，未声称已计数全部新准入机器码 site |
| 普通 Core/bench | 四臂 CRC/checksum、实际模式和退出均正确；spill 指标均相同。Function Core 426→427、288392→288484 bytes，差异限既有 formatter 区间；其余三臂完整顺序形状相同（Block Core2414/208280，Fbench22/20320，Bbench101/16764） |
| 捕获机器码/工作区 | Bbench101 段原始字节相等；空 Function dump 不作字节等价。五组 workspace 基础六项相同且归零，只有 Function Core retention +15、pattern bytes +120；该账本不含新缓存的 STL 私有容量 |

Android DWARF：SemanticFacts632、RegisterStatePreparation1544、ActiveState1720 不变；GuestStateFacts72→80、VersionViews24、GuestStateVersions104、Input40、Answer24；本族 Label 为12，其他同名 DWARF Label 不混算。LogicalByte24→16。公共 PrepareGPRMatch 自身帧848→240，冷 constructor320、Prove656、ByteOf176，Semantic Prepare176、target PublicationWindow176；仅是各函数自身编码，不证明整条调用链最大栈下降。launcher 121339080→121826224 bytes、text4493700→4507716（+14016）；V3 text 比 V2 还多2368。容量、分配器元数据、更多持有时间、完整 Function 候选扫描、热缓存入口和动态编译成本继续列为质量工作，未宣称性能改进。

### RV64 与证据边界

实际批次 `r7-rv64-execution-20261025-v66` 编译/运行源码就是最终 versions-v3，source SHA 同上；2557 个冻结输入 SHA 为 `58ce89b3cbe2b9e9f450d81538045151ecdb29e3b29ab13c2ea4b41bc87ed5db`。十四目标完整构建 0，九个完整 ELF 改变；19 项受影响用户态 fresh 通过，13 项用户态及5项 guest-kernel 以完整 ELF、已验证压缩成员、命令/properties、工具和环境相同复用原成功。实际原日志、时间和内核 artifact hash 保留；没有重新 boot 内核，也没有声称 fresh 全37项。

独立 strong-TSO 再执行 220321 checks 通过，TCG 有限回归不作为物理 RV 内存模型证明。两项 compile-cost 和 emit 继续仅构建。最终 native/RV 的生产代码完全一致；随后仅修改本文和方案文档，按实际 Ninja 命令/依赖证明其不在构建闭包，不能写成重新构建的结果。自有 RV66 完成后确认只有 sleep 再停止；共享 adb 与外项目资源未动。

### 存储与继续施工

当前28 ELF 已逐成员解压复核：`/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/r1-guest-state-versions-v3-current-elfs.tar.zst`，453724219 bytes，SHA256 `f4c52e159b42fd6a7e63942324938ac2430f227d286c94645f2b28ab2a2469a5`。本轮只展开实际 native7/RV14，其余 native7 直接引用固定缓存；21 个已结束副本随后回收 1045651456 allocated bytes，独立 du 降幅相同，固定缓存28产物仍在。

V2 的7个实际 native 输入另压缩保留238571895 bytes、SHA256 `31cf03f22b7e00d709bf48bbefa21e0c4964f04bd845921be0429a1b6314c930`，删除984072192 bytes 展开副本，扣除该归档后净减少745496576 bytes。保留源码/补丁、原始成功及失败日志、PMU/旧 red 与唯一结论；不复制新的完整构建树。系统盘原释放277194584064 bytes 为历史独立测量，本轮 Data2 回收不重复加到该数；三个 tmp 入口仍为 Data2 软链接。

本节功能性证据不等于 R1 或完整重构完成。继续：完整 uniform/FPR 逻辑状态家族、Function/局部分支扩容的分配失败覆盖、热入口与 Function 按需范围的进一步整理、其他语义宽度/改写与生命周期契约；R2 sink/物理压力完整质量、R7 后向 label/Phi/type/SSA/fault 范围，以及正式动态成本和 PMU/FEX 门仍开放。Block 与 Function 继续分别验收，不新增迁移开关或临时 probe。


## R1：GPR 查询按需范围与缓存入口（2026-10-25，demand-v2）

本节接续 versions-v3。五个既有文件改动，production 净增92行，无新增源文件/目标/测试用例/运行模式/配置/probe/仓库脚本；继续在原语义用例中加一个 SECTION，并扩展原 constant-window。代码格式40、AOT schema6保持，合法输入的逻辑匹配谓词及物理入口/宽度/IR改写未改变。

- Source SHA256：`c81dd62abf665216647d5d779c7a11f744fa451222b4f3c10bf0ad20137c019f`；overlay478、retired118。
- 相对 versions-v3 patch：`70c2f6e42ae2fe34e5e92193e10a6c052f1e55e6fc5af81b081d72898d9fe144`。
- 初版 demand-v1 native十四目标构建0（98.06s），没有运行。运行前进一步在每个前驱加入前向 label，只改 lifetime 夹具；最终 demand-v2 native十四目标增量构建0（4.39s）。RV67仅冻结旧输入、未编译/运行；实际最终批次是RV68，不写成旧源码等价复用。

### 准备边界和职责

`SemanticFacts::PrepareGuestStateGPRMatch` 的热分支只查已经过归属/SSA检查、并属于当前不可变输入的键，命中不再遍历 owned blocks 或调用 SSA 准备。单独 cold 成员保留所有形状、归属、Function RPO 与 SSA 检查。Find 同样只查已经发布的完整键，不构图。Block owner 按真实指针有序，Function 有独立唯一 owner；不同策略、Value 类型视图及合法 byte offset 不互相污染。

Function 的局部证明不额外强开 CFG，也不为其他成员准备观察流；若 SSA 必需检查自己需要 CFG，继续允许该必要准备。入口仍有未决字节时才借用公共 CFG、建立 canonical-root 可达性和图 ID→稀疏输入 ID。整个图不复制边；只有访问到的块才准备观察/label 输入。尚未跨块时私有局部输入可替换，已发布正/负答案不受影响；跨块后 ID 稳定追加。label/point 的部分扩容在失败时撤回，图状态完整后才发布。整数转换读取范围改用现有 IntegerConversionInputBits，旧本地 min32 判定退役。

### 实际验证

| 范围 | 最终结果 |
| --- | --- |
| 原语义用例 | 7个 JUnit section、66断言全通过，相邻新增22；新增类型/offset缓存隔离、局部 Function 不额外构图、不扫描未查询后继、跨输入替换/CFG增长后旧答案仍可查 |
| 主套件 | JUnit762分项、484876断言；原相同11分项/66断言失败、0error，全部共享失败签名一致。总数+184=22已知新增+162其他未归因，未为凑总数重跑 |
| IR | fast1/fast0 各113分项、107208/107306断言，0fail/error |
| constant-window | 每个 fast 模式125603 checks、217 injections；相邻+2025 checks、+201 injections。两个新夹具分别冷 Function 与已有局部答案，18前驱/20块、每臂一个label及汇合块48个label（共66）；覆盖输入/label/visited/worklist/graph扩容失败，无部分答案，旧答案存活、重试成功、热查询不分配。仅为该具体有限形状，不宣称所有任意图的故障矩阵 |
| 既有回归 | 16项 native 与4普通负载退出均符合预定（主套件为历史66）；其余七个旧marker完全一致，sink每模式1690946/33 injections。原生/AOT call、branch static-latch四模式另执行0，38/33/45/40 checks |
| 普通形状 | 四臂 CRC/checksum、实际模式和退出正确，spill相同、无overflow。Function Core427→426、288484→288392 bytes，变化仍限旧formatter区间；其余三臂完整顺序形状相同：Bcore2414/208280、Fbench22/20320、Bbench101/16764 |
| 捕获/账本 | Bbench101段机器码原始字节相等；空Function dump不推导等价。五组workspace基础六项相同且归零，只有Fcore retention−15/pattern−120；该账本不含本族STL私有存储 |

没有在每个文件修改后运行矩阵：最终生产代码与加强夹具合并后才执行一次上述相关集合。外部 fmt/VIXL/intrusive-list 的既有 warning 保留，未顺带修改依赖。

### 成本和适用范围

Android实测 SemanticFacts632、GuestStateFacts80、GuestStateVersions104、Input40、LogicalByte16、Answer24、RegisterStatePreparation1544、ActiveState1720不变。VersionViews24→32；跨块才出现FunctionInputs56 +每CFG成员5字节的vector元素（容量/分配器另计）。输入记录40×实际访问块、label12×已准备label、答案24×查询数；向量容量、局部输入切换的重扫及生命周期都继续计成本，不把稀疏逻辑长度当成实际容量。

Semantic热入口224 code bytes/自身80-byte帧（前为796/176）；cold为868/176；GuestStateFacts准备1204/192（前1624/240），现只在miss调用；owner准备912/160，输入1964/320，Function1312/256，Prove5468/656，ByteOf2120/192（前2256/176）。这些只为各函数自身编码；不推导整条调用链最大栈。launcher121826224→121905008、text4507716→4510756（+3040），完整编译耗时、私有总堆和正式PMU/FEX仍未验收。

### RV64 和保存

实际源码为最终 demand-v2，批次 `r7-rv64-execution-20261025-v68`。2557冻结输入 SHA256 `a0294b12f6b94daf307aa136f161d59997ca60cc0297fd1d3ed7657857948a8c`；十四目标完整构建0，四个完整ELF改变。新执行17项用户态通过；15用户态与5内核以完整ELF、实际命令/properties、工具和环境一致复用原成功，旧二进制从已验证压缩成员核对，不重新展开旧bank。独立strong-TSO的220321 checks也因完整ELF相同复用实际RV66成功，不冒充本批fresh。两项compile-cost和emit继续仅构建，没有新boot、物理RV或新全37项执行。最终文档修订另以实际Ninja闭包排除，不改写编译/运行来源。

当前28 ELF已逐成员解压验证：`/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/r1-guest-state-demand-v2-current-elfs.tar.zst`，454069530 bytes，SHA256 `8f4e4f20b09b9badd522c8dc8f720ecae44c022e6c6cc469bafd3092764c1abc`。只展开native7/RV14，其余native7引用固定缓存；完成后回收21副本 1046364160 allocated bytes，独立du降幅相同，固定cache28仍在。保留相邻versions-v3归档和全部唯一成功/失败/PMU源码证据；大输出/TMPDIR继续实际写Data2。原系统盘277194584064-byte释放是历史独立测量，本轮Data2回收不重复计入。

继续施工：逻辑 uniform/FPR 的真实消费者、其余语义宽度/改写与生命周期契约、R2压力/完整动态成本、R7后向局部label/Phi/type/SSA/fault恢复，以及正式PMU/FEX质量门。此批完成具体按需准备和扩容故障回归，不代表完整R1或全重构完成。


## R1/R2：decode 与残余 pass 的 uniform 转发规则收敛（2026-10-25，uniform-forwarding-v2）

本节接续 demand-v2。六个既有生产文件加一个既有测试文件修改，production净减115行；无新增源文件、目标、根测试用例、运行模式、配置、probe或仓库脚本。代码格式40、AOT schema6保持。前一 goal turn 是功能性进展，本轮继续完成实际公共规则迁移，完整重构目标保持开放。

- Source SHA256：`717aab7ee92a678f4d06e1c46c6cc2d2a4120fa5427d752112ee2577418ceb10`；overlay478、retired118。
- 相对 demand-v2 patch：`c2af4140928458ddbb52534d2b69db279d0af7b36d932209e067f2272b3be8e7`。
- v1 native十四目标94.71s退出1：生产代码已编译，夹具错误地向 BindLabel 返回的 Void 请求 Def；没有v1运行或RV构建。修正为取得真实标签指令后，最终v2十四目标增量构建0（16.28s、两步），再集中运行相关集合。

### 最终职责与退役

`analysis/guest_state.cpp` 的 DescribeGuestStateForwardAccess 在已有范围/映射分类上统一读取转发、load播种、store写入/忽略/完整失效资格；沿用原固定GPR子集、range/XMM/narrow开关与resident FPR deferral。它是显式配置/布局策略，不进入封口后的GuestStateFacts缓存，也不证明物理寄存器高位。

GuestStateForwarding 对调用方span执行实际typed Value宽度的字节记录、完整失效、HelperUniformWrites::Declared范围失效和局部taken/fallthrough交集。每次操作结束不保留byte span；对象只持有短期分支快照，builder terminal清除、pass块结束释放。空交集与缺失快照继续区分。builder不为了统计扫描整个byte表；pass根据更新分类保留原统计。旧GuestStateBranches类、builder的UniformInvalidateHelper、pass的helper/分支独立更新和两边重复byte写循环退役。

builder仍创建typed IR，pass仍通过RewriteContext改写；部分mapped store继续留给pass，mapped FPR store仍保守全失效。普通memory fault观察、DSE/sink发表、物理CurrentEntry/DefinitionEntry及窄值高半证明继续各自负责。公共策略按实际访问只分类一次，decode普通load和完整mapped store复用该结果；没有新增第二套byte表或共享可变Function状态。

### 实际验证与成本

| 范围 | 最终证据 |
| --- | --- |
| IR fast1/fast0 | 各113分项、107680/107778断言，0fail/error，各相邻+472。原helper用例扩为三种Call、U64/V128、完整/空/部分写声明；该部分验证IR失效，不调用任意地址。另32组decode/pass、path/XMM/narrow开关和条件分支程序，实际解释器在三种填充值下对照完整64-byte oracle，残余路径含改写前后 |
| Native | 15项既有回归与4个普通负载，退出全部符合预定。主762分项、484917断言，仍相同11分项/66断言失败、0error，共享失败签名相同。主断言+41未归因，没有为凑数量重跑。两个window各125603/217 injections、两个sink各1690946/33 injections及其余五个marker全部与相邻一致；本批未再单独重复原GPR focus或extra static-latch四模式 |
| 普通形状 | CRC/checksum、实际Block/Function模式正确、无overflow、spill相同。Fcore426/288392B、Fbench22/20320B、Bbench101/16764B完整顺序形状相同；Bcore2414→2413、208280→207980B，所有差异仅在原formatter范围，不归因为本次优化或噪声。Bbench101段捕获机器码字节相同；空Function dump不推导等价 |
| 存储账本 | 五组workspace基础六项均相同且归零；仅Bcore pattern_init_bytes837732→836724，其余四组所有字段相同。该账本不包含本族STL私有容量 |
| Android大小 | IrBuilder520不变，GuestStateBranches40→GuestStateForwarding40；临时GuestStateForwardAccess12、Update8。SemanticFacts632、GuestStateFacts80、RegisterStatePreparation1544、ActiveState1720不变。launcher121905008→121897256 bytes，text4510756→4509620（−1136） |

公共Write/Invalidate/Observe/Describe各自编码384/68/792/488 bytes，自身SP下降128/0/144/112；仅为单函数编码记录，不能把text减量、表头相同或自身帧当作整条调用链/全部堆/动态编译耗时改善。完整容量/生命周期、动态成本和正式PMU/FEX门未验收。

### RV64 与保存

实际最终源码批次 `r7-rv64-execution-20261025-v69`，2557冻结输入SHA256 `148267b94180abdb82c2a05df8f1776d89aca2da8dde2cdc628b16538eeb6a60`，对应上述最终v2源digest。十四目标完整构建0，九个完整ELF改变；新执行19项用户态通过，13用户态及5内核以完整ELF、实际命令/properties、镜像与QEMU一致复用原成功。strong-TSO完整ELF改变，本批**重新执行220321 checks通过**。两项compile-cost和emit仍仅构建；没有新boot、物理RV或新全37项执行。旧ELF从demand-v2归档28成员全部解压哈希核对，只为布局临时恢复一个launcher并在工具结束后删除，没有重新展开旧bank。

当前28 ELF已逐成员解压验证：`/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/r1-uniform-forwarding-v2-current-elfs.tar.zst`，454059515 bytes、SHA256 `1bb1a88f4821c8ae1be0f23446e24a00de8de0b432db35c837a90b7627e09eb5`。仅native6/RV14展开，八个native仅构建产物引用固定cache；完成且无活跃消费者后回收20副本 **929329152 allocated bytes**，独立parent du降幅相同，当前固定cache28产物保留并核对。手机6个ELF及1传输tar再回收869838848 allocated bytes；原始日志和guest输入保留，shared adb与外项目未动，自有RV69容器已停。

源/Git/WIP、原始成功/失败日志、唯一PMU证据及相邻checkpoint保留；tmp入口继续实际写Data2，结果路径的展开ELF已不存在，后续只从索引恢复所需named member。以上Data2/手机回收不重复计入原系统盘277194584064-byte历史释放。完整uniform/FPR Function状态、其余语义宽度/SSA/Phi/局部回边与fault恢复、R2压力和正式动态/PMU/FEX质量门继续接续；本批不代表完整R1/R2/R7或完整重构完成。


## R1/R2：固定状态优化资格由后端声明（2026-10-25，state-target-policy-v1）

本节接续uniform-forwarding-v2。六个既有生产文件、两个既有测试文件修改，production净增21行，无新增文件/目标/根测试用例/CLI模式/配置/probe/仓库脚本。代码格式40、AOT schema6保持；策略是由现有ISA/绑定配置导出的编译数据，不是新环境开关或持久化布局。

- Source SHA256：`c0f9ed6bbdf89541f5d1335747f745be8954a36d38a9c6f6dbc5359de7c9339d`；overlay478、retired118。
- 相对前批patch：`4a61d15d202f654b7d2005b3c26d975a1d94ad3c56373f50692ef897c3d36f16`。
- Native十四目标一次完整构建0（96.95s），没有夹具修正/第二次构建或中途运行；完成这一批后集中验证。

### 职责与验证

ARM64 target.cpp的ConfigureGuestStatePolicy声明既有0–9/22/23/29强制转发/load播种集合，以及完整22/23/29扩展映射的映射前DSE顺序。AddressSpace在所有GPR/FPR绑定建立后，仅对ARM64调用；布局随后经const getter交给编译流程。默认及非ARM64布局不从数值自动继承该合同。公共规则只查GuestStateOptimizationPolicy的mask/阶段位，旧共享层的ARM64编号分支已清零；映射、typed字节更新、helper/fault、物理高位与Block/Function分配入口继续独立。

旧特殊DSE顺序没有简单删除：部分SetHost写仍有物理宽度义务，映射前的逻辑byte删除与XMM-only补扫按ARM64显式策略保持。两个合成ARM64映射夹具改为请求真实后端策略；原helper用例再加入96组decode/pass、默认/ARM64/自定义home20资格、range开/关、home5/20/22/29与U32/U64对照，同时检查load→load、store→load及实际SSA来源。自定义资格仅验证公共逻辑接口，不宣称ARM64物理层新增home20优化合同。

| 范围 | 实际结果 |
| --- | --- |
| IR fast1/fast0 | 各113分项、108064/108162断言，0fail/error；每模式新增384条通过断言，原helper/向量字节oracle继续通过 |
| Native | 15项既有回归与4普通负载退出符合预定。主762分项、484836断言，原相同11分项/66断言失败、0error，共享失败签名相同；主断言−81未归因，没有为凑总数重跑。九个旧mode marker全部相同：window每模式125603/217 injections，sink每模式1690946/33及其余五个marker不变 |
| 普通形状 | CRC/checksum、实际模式、spill正确且无overflow。Fcore426/288392B、Fbench22/20320B、Bbench101/16764B完整顺序形状相同；Bcore2413→2412、207980→207972B，仅原formatter范围不同，不归因为优化或噪声。Bbench101段捕获机器码字节相同；空Function dump不推导等价 |
| Workspace | 五组基础六项相同且归零；只有Bcore retention_events−21、pattern_init_bytes−436，其余四组所有字段相同。该账本不包含本策略分配或其他STL私有容量 |
| Android布局/代码 | GuestStateLayout96→104、策略8 bytes，每AddressSpace持有一次；binding8、access12、update8、IrBuilder520、SemanticFacts632、GuestStateFacts80、RegisterStatePreparation1544、ActiveState1720不变。launcher121897256→121901904 bytes，text4509620→4509636（+16） |

ConfigureGuestStatePolicy自身36 code bytes、无SP下降；公共Describe488→512 code bytes、自身SP仍112。该批以消除后端语义泄漏为结果，不能把八字节逻辑记录当作实际malloc size class，也不宣称动态编译/全链栈/全部堆或PMU改善。完整成本门继续开放。

### RV64、固定缓存与回收

实际最终源码批次`r7-rv64-execution-20261025-v70`，2557输入SHA256 `70f450356c0029d8da10d528d26cf3fc7d179271e3a393b04626bb22ba9a5d14`，对应上述最终源digest；十四目标构建0、九个完整ELF改变。19项受影响用户态新执行通过，13用户态与5内核以完整ELF、命令/properties、镜像和工具一致复用原成功；strong-TSO重新执行220321 checks通过。两项compile-cost及emit仅构建，无新boot/物理RV或新全37项执行。相邻归档28成员全部流式解压核对，没有恢复旧bank；原运行来源/时间戳保留。

本批native14/RV14直接使用完成后的固定cache文件，运行前后与归档时核对SHA，整个批次**零展开结果ELF副本**。当前28成员归档`/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/r1-state-target-policy-v1-current-elfs.tar.zst`，451175069 bytes、SHA256 `e6eb91b247471185f8276d104f4f956ff12a455046ded3a3128fb406d4ced66c`，全部成员解压核对；固定cache28产物保留，未来缓存被下一批修改后必须按SHA核对或从该索引恢复指定成员，不能仅凭旧.path假定仍是原二进制。手机6个ELF/1传输tar结束后回收869900288 allocated bytes，原始日志和guest输入不动；自有RV70容器已停，shared adb及外项目未动。

旧RV输入快照52–68共17目录、43454文件已在无活跃mount/descriptor后压缩，逐成员与原始inputs manifest哈希核对，再复查内容后回收。归档`/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/r1-state-target-policy-v1-older-RV-inputs.tar.zst`，218947520 bytes、SHA256 `6e4741ae976421b78193776c45adb72ba4943c1e20862ed5911834e855c4abe2`；恢复索引为`/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/r1-state-target-policy-v1-source-snapshot-reclamation.json`。gross释放1121542144 allocated bytes，独立固定cache du降幅相同；扣除归档allocated bytes净减少885903360 bytes（尚未扣小索引），不把压缩逻辑大小冒充实际分配。当前70及相邻69源码目录保留，旧执行/构建/仅冻结身份不因压缩改变；主工作树源码/Git/WIP和唯一raw证据不受此回收影响。Data2/手机释放与原系统盘277194584064-byte历史释放分别记录，不从全盘df变动推导本线收益。

完整重构继续：按实际消费者收敛剩余FPR/uniform逻辑事实与重复窗口，核对14.3中尚未完整证明的flags/类型/SSA/恢复契约及14.2配置矩阵，完成15.3/15.4动态与私有存储门、正式同机PMU/FEX对照。后向label和ARM64 Phi/local按方案保持既有拒绝能力边界；本批不代表完整R1/R2/R7或全目标完成。


## R1/R3：共享 GPR/FPR 访问窗口接入真实 FPR 消费者（2026-10-25，register-state-windows-v2）

本节接续 state-target-policy-v1。修改17个既有文件（15 production、2测试），production净增94行；无新增源文件、target、根测试用例、运行模式、配置、probe或仓库脚本。Block/Function 分配入口继续独立，代码格式40、AOT schema6保持。

- Source SHA256：`c38a3903d5fef61824a6f008e988f83de7da4e19989d185cb966df8060950254`，overlay478、retired118。
- 相对前批patch：`aba8cf80f40cb64ad10c9716a1f3dbf597b45720204b884fb103829b05afc30a`。
- v1 native十四目标构建退出1（102.18s）：两处测试迁移错误为漏传bank参数、第二夹具缺少新增身份断言的局部绑定；没有v1 runtime或RV构建。修正夹具后，v2另补实际生产的FPR-write presence门控，最终十四目标增量构建0（53.22s、139步），再集中执行。本批不把v1产物当成v2资格，也不把增量耗时当成干净构建耗时。

### 文件职责与边界

`analysis/guest_state.{h,cpp}` 以 GuestStateRegisterAccesses 取代 GPR-only目录/API。Get/SetHostGPR、Get/SetHostFPR（含部分访问）共用四字节 bank/home/读写/编号项，按键排序；FirstAccess、Positions返回首位置或严格开区间，查询不复制元素、不分配。GPRBarriers仍单列，目标 FeatureSet/helper clobber 投影不进入公共静态索引。GuestStateObservationWindow只截取唯一观察流的span，最终消费者继续使用原观察谓词。

`arm64/compile/fpr_inputs.cpp` 的AES/发表候选通过原形状、merge与use-end检查后显式请求共享寄存器/观察流，替代逐候选的全IR读写/观察窗口。反向未来写窗口和现有use-end投影仍保留，普通mapping、物理owner和两种scalar insert策略不混入公共索引。`inputs.cpp` 只传递真实块与SemanticFacts。

`fpr_preparation.{h,cpp}` 显式接收AllocationWriter，先核对IR/索引身份与实际proof roles，再从writer借用同一SemanticFacts。基础facts同次记录八字节BlockSummary中的HasFPRWrites；无SetHostFPR的读取/scalar tie及纯shuffle需求不为本家族建寄存器/观察窗口，重复准备复用稳定对象。`fpr_contracts.{h,cpp}` 仍消费只读AllocationView、const BlockAnalysisIndex、寄存器指针和可选观察span；五处全指令窗口改为共享查询，保留最后使用的inclusive端点、scalar同owner写入例外、AES committed-home的LoadMemory/StoreMemory/SaveFlags例外、递归拒绝与独立contract retry。物理owner/recolor/高半及fault证书没有转入公共事实。

该目录依赖最终封口IR和编号，仍属分配/契约准备阶段；decode继续只执行局部已知字节和静态effects等可当场证明的工作。本批没有把最终窗口提前到decode，也未交付跨CFG的FPR/uniform版本传播；现有Block/Function入口策略及动态SSA守卫保持。

### 集中验证与成本

| 范围 | 最终实际结果 |
| --- | --- |
| Native | 15项回归和4普通负载，退出符合预定；实际使用6个native ELF，其余8个仅完成构建。主JUnit762分项、506131断言，仍相同11分项/66断言失败、0error，共享失败签名相同。总断言相邻+21295，不把总差值全部归因或为凑数重跑 |
| IR与旧marker | fast1/fast0各113分项、108064/108162断言，0fail/error，与相邻相同。九个旧marker完全一致：每模式window125603 checks/217 injections、sink1690946/33及其余五个marker不变；本批没有另重复extra static-latch四模式 |
| 真实新覆盖 | 在原block-analysis根用例内，以独立线性typed-IR walk对照两种bank、home、读写、ignored首项和严格区间；含部分访问、空home、dense/sparse编号、最后ID65535及65536/UINT_MAX上界，观察span也以线性oracle核对，不隐式建立uses/CFG。原19场景FPR准备夹具与递归/稀疏夹具核对按需出现和重复准备的对象身份 |
| 普通形状 | CRC/checksum、实际Block/Function模式、spill正确且无overflow。Fbench22/20320B、Bbench101/16764B完整顺序形状相同；Fcore426→427、288392→288484B，Bcore2412→2414、207972→208280B，全部差异在原formatter范围，不归因为本次优化或噪声 |
| 捕获与workspace | Bbench101段捕获机器码字节相同；空Function dump不推导等价。五组workspace基础六项相同且归零；Fcore retention+15/pattern+120、Bcore retention+21/pattern+1444，其余三组全字段相同。该账本不包含新增共享目录元素和STL私有容量 |
| Android实际布局 | SemanticFacts632、GuestStateFacts80、共享访问目录56、BlockSummary8、RegisterStatePreparation1544、ActiveState1720保持；FPRContractBuilder96→128、FPRContract40。目录每项4字节，新增FPR项可能让仅GPR消费者承担额外冷构造/排序/保留容量；vector容量、分配器、owner与proof行另计，不从header不变推导总堆下降 |
| 代码与栈 | launcher121901904→121943280 bytes，text4509636→4509484（−152）。共享目录构造1176 code bytes/自身192-byte帧，AccessRange336/0、FirstAccess96/32、ObservationWindow192/0，PrepareFPRContract1996/384；只记录各函数自身编码，不推导整链最大栈、动态编译耗时或PMU改善 |

### RV64、清理与保存

实际最终源码批次 `r7-rv64-execution-20261025-v71`，2557冻结输入SHA256 `19279fb1e939a1d3796bc65401a5fd3261f14c92b0a2d9047487cf5ca42b828a`；十四目标构建0、九个完整ELF改变。19项受影响用户态新执行通过；13用户态和5内核以完整ELF、命令/properties、镜像和工具相同复用原成功。strong-TSO本批重新执行220321 checks通过。两项compile-cost及emit仍仅构建，没有新boot、物理RV或新全37项执行。相邻policy归档28成员流式解压核对，零恢复旧ELF副本；最终两份文档改动另以实际Ninja闭包排除。

当前28 ELF直接读取完成后的固定cache，整个批次零展开结果ELF副本。归档 `/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/r1-register-state-windows-v2-current-elfs.tar.zst`，451368571 bytes、SHA256 `333a572007476d099881d2a93151a8fec042bf2ea5549e39beba458d3e345fff`，全部28成员解压校验；未来固定cache被下一批重建后必须按记录SHA核对或恢复指定成员。手机6个ELF和1传输tar已在校验且无消费者后回收870277120 allocated bytes，原始日志和guest输入保留；自有RV71容器在全部执行结束后核对仅剩sleep再停，shared adb与外项目未动。

用户优先清盘指令已在恢复构建前收口：原系统盘三个tmp入口均为Data2软链接、du为0；原独立释放277194584064 bytes是历史实测，本次复核新增系统盘释放0。另删除11份已被后续版本替代、可重建的旧native/RV二进制归档，删除项allocated bytes和即时parent du降幅均为4764229632 bytes；不把并发全盘df变化全算本线收益。旧源胶囊、精确ELF身份清单、构建来源、唯一成功/失败/PMU原始证据，以及原始两份保留归档和当前/相邻有效checkpoint保留。旧11份二进制归档不再承诺逐字节恢复，历史封存记录不改写；退休索引为 `/Volumes/Data2/swiftvm-middle-preserved-20261025/owner-urgent-cleanup-recheck-20261025-retirement.json`。代码/Git及2675项工作文件在清理前后一致；回执写入用户指定的WinEmu协调路径。

完整目标仍开放：其余真实消费者的重复分析、14.2/14.3既有配置与正确性要求、15.3/15.4动态编译/完整私有容量及正式红魔PMU/FEX质量门继续接续。后向局部label和ARM64 Phi/local保持方案既有拒绝边界，不把新增能力实现作为本批验收义务。



## R7：发射器只消费冻结 scratch 与 spill 输入（2026-10-25，emission-inputs-v2）

本批接续 register-state-windows-v2，修改5个既有文件（2个生产文件、3个测试/support文件），production净减81行。没有新增源文件、目标、根测试用例、运行模式、配置、probe或仓库脚本；Block/Function 分配策略、代码格式40、AOT schema6保持。

- 最终Source SHA256：`d9602bcc5eb5c44e43dda85040360cca531f282b979cc07050bf8ba1a8f20358`，overlay478、retired118。
- 相对相邻版本patch：`87cc7f4b9d6370055e1583eeac317c4e9b239be267dfdc75c27733db017a97cb`。
- v1 Source为`b08a23bae85d72cd6d9fe973e16ecea75f5def5943688905d5e04d72655d2cd2`。完整native十四目标构建0（40.47s、106步），首轮实际8项回归在主套件发现3个新error后停止，没有继续call/sink或普通负载。三项均为隔离encoder测试helper未先ImportUnitEdges，ImportBlockActions的既有断言拒绝；原66失败断言未变。
- v2仅修正该测试helper，显式调用既有完整unit证书检查与edge导入，再绑定block actions；不绕过断言，不改变期望值，生产源码与v1逐字节相同。最终十四目标增量构建0（5.35s、5步）；13个完整native ELF与v1相同，只有主测试ELF改变。最终实际重新执行完整15项与4负载；首版七个成功项也确实重复执行，没有写成证据复用。

### 退役内容与必要守卫

`jit/jit_context.{h,cpp}` 删除三个无引用opcode分类器及TickIR的缺失scratch兼容分支。TickIR在任何pending spill写回/本指令编码前验证绑定块、IR归属和冻结allocation epoch，获取一次准备好的scratch range；flush和本指令共用该行。CurrentUnitScratch只读UnitActions，不再调用PrepareUnitScratch。FlushSpillWrites不再调用PrepareSpillInput或重新分类opcode，也不重复查找整份scratch view；target-input守卫失配不授权forwarding，required backing和正常写回继续保留。terminal的空consumer flush不请求输入图。

`tests/support/jit_translator_test_support.h` 的BeginBlock显式导入既有unit/edge/block actions，替代新建BlockAnalysisIndex并另存pseudo flags的BindPseudoFlags。原pressure.cpp中的SSE4.2、Add/Sub和绝对GetOperand三种隔离编码测量继续使用原映射和答案。region_compile_metrics的旧类型存在性兼容lambda改为直接准备调用；AssemblerOnlyTag继续只用于原始汇编缓冲。没有添加第二套准备容器、状态字段或兼容API。

### 集中正确性、形状与成本

| 范围 | 实际证据 |
| --- | --- |
| Native主集合 | 15项回归和4普通负载退出符合预定；JUnit762分项、506265断言，仍相同11分项/66断言失败、0error，共享失败签名相同。主断言总数相邻+134未归因，没有为凑数量重跑 |
| IR/故障注入 | fast1/fast0各113分项、108064/108162断言，0fail/error，九个旧marker相同：window每模式125603 checks/217 injections、sink1690946/33等不变 |
| 既有生命周期/恢复集合 | 复用原137项matrix的具体命令/env，9个组合在本批主集合已完成，另128项新执行全部0；含快慢IR、prepared块/多region、entry/disk/AOT/拒绝、shutdown、static/dynamic retirement、54种latch、cache与call持久化。固定class独立JUnit39断言0fail/error。此有限137项集合不替代14.2/14.3全部配置/语义要求 |
| 普通形状 | Fcore427/288484B、Fbench22/20320B、Bbench101/16764B完整顺序记录相同；Bcore2414→2416、208280→208272B，仅formatter范围不同，不归因为本批优化或噪声。CRC/checksum、实际模式、spill与overflow检查通过；Bbench101段捕获机器码相同，空Function dump不推导字节等价 |
| Workspace | 五组基础六项相同且归零；只有Bcore retention_events+21、pattern_init_bytes−468，其余四组所有字段相同。该账本不包含运行时scratch容器或所有STL私有容量 |
| Android布局/代码 | 通过带父namespace的DWARF核对ARM64 JitContext1408、JitTranslator3280、UnitActions864均不变；InstructionScratchFacts16、UnitScratchFacts8、ScratchRange20、SpillInputAction16。launcher121943280→121935976 bytes，text4509484→4504140（−5344） |
| 自身编码/栈 | TickIR892→980 code bytes，自身SP下降144→160；consumer FlushSpillWrites860→952、128→208，零参数flush84/48不变；CurrentUnitScratch现为284/96，旧版未独立列出该符号，不能当作旧版零成本。RequireBlockForEmission260/64不变。记录各函数自身编码，不推导最大叠加栈或全编译动态收益 |

### RV64与精确来源

实际RV构建为`r7-rv64-execution-20261025-v72`，实际编译Source仍为v1；2557输入SHA256 `b113611b09c26d4acd567c1c8c6cbf493a8ca2a829e299bd68c9f677eb401866`，十四目标构建0，四个完整ELF改变。v2只改的ARM64测试helper不在实际十四目标的Ninja命令/全部编译依赖中，已单独保存源码资格记录，不重复RV构建、不改写实际编译来源。

17项受影响用户态新执行通过，15用户态与5内核以完整ELF、命令/properties及环境一致复用原成功；strong-TSO完整ELF未变，复用实际RV71的220321 checks，未称本批fresh。两项compile-cost与emit仍仅构建，没有新boot或物理RV验收。自有RV72在全部执行终止、仅剩sleep后停止；外项目容器及shared adb未动。

当前28产物归档`/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/r7-emission-inputs-v2-current-elfs.tar.zst`，451345685 bytes、SHA256 `2273d3439a28c256c49e84737edca053ff8a834eb148cb28997da6b648e2e626`；所有28成员解压校验，未创建展开的整组结果ELF副本。PMU只恢复相邻launcher这一个named member，当前launcher与固定缓存共用inode，六类负载共享一个输入bank。固定缓存后续改变时按SHA核对或恢复指定成员，不能凭旧path假定产品身份。


### 当前六类负载的同机三方 PMU

冻结上述最终源码与产物后，使用既有tools/redmagic协议完成CoreMark、int/fp/mem/branch/call六组，每组36主点、6独立冷服务点及1份私有FEXServer寿命计数；合计216主点、36冷服务点、6服务寿命，六组measurement_valid均为true。原清盘时停止的四个call点保持历史身份，本批另建完整矩阵，不回填旧点。主PMU不启用PROF/PROF2诊断；两种退出状态、原始running==enabled、CPU4 affinity、stdout/CRC和前后输入哈希均闭合，原始数据取回前后及本地哈希相同。

固定FEXLoader/Server为8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175 / 9fe58a9946e9105c49911518356fe29a4ed3143c1fe90803b57afe112c225e8c，仍为Android a156752与原4ade908补丁，x18保留、六GPR池等参照限制不变。当前SVM launcher为c814f27148c6114f4435a5074b89e1a789896212e6384a5f1d95d3b77780cf3b，相邻register-state-windows-v2为25bf2d46ce70674fd2461b5d6254fe767c1c7f51369210ae8fc08c468c4c4c91；无起点累计比较。

下表候选/相邻为三轮配对斜率变化的中位数[最小,最大]；候选/FEX为各自斜率中位数的比值。完整三轮、MAD、绝对计数、wall time和截距留在JSON。Function/Block仅表示SVM运行模式及FEX的配对上下文，不把FEX视为两种分配入口，也不把Function中的默认单块快速路径当成完整Function分配。

| 负载/模式 | 指令斜率变化 | cycles斜率变化 | 候选/FEX 指令 / cycles |
| --- | --- | --- | --- |
| core / function | -0.002% [-0.002, +0.001] | -0.804% [-0.854, +2.265] | 1.737 / 1.951 |
| core / block | -0.010% [-0.022, +0.051] | -1.772% [-1.844, +1.207] | 2.214 / 2.565 |
| int / function | +0.004% [-0.113, +0.021] | -0.190% [-1.317, +0.053] | 1.498 / 0.996 |
| int / block | +0.025% [+0.010, +0.127] | +0.135% [-0.269, +1.062] | 2.098 / 1.061 |
| fp / function | -0.112% [-0.112, +0.215] | +0.276% [+0.249, +0.666] | 3.440 / 3.002 |
| fp / block | -0.095% [-0.113, +0.053] | +0.304% [-1.163, +1.700] | 3.227 / 2.859 |
| mem / function | +0.023% [-0.015, +0.031] | -3.006% [-3.165, -2.503] | 1.378 / 1.756 |
| mem / block | +0.018% [-0.040, +0.033] | -0.121% [-0.590, +2.076] | 1.926 / 1.860 |
| branch / function | +0.020% [-0.109, +0.027] | +0.344% [-2.436, +0.953] | 1.452 / 1.175 |
| branch / block | -0.012% [-0.022, +0.041] | -1.016% [-1.392, +0.588] | 1.750 / 1.413 |
| call / function | -0.000% [-0.019, +0.010] | -3.256% [-6.404, +1.401] | 1.798 / 1.740 |
| call / block | -0.008% [-0.044, -0.001] | +0.003% [-2.695, +0.216] | 2.867 / 3.788 |

12组模式/负载的全部配对指令斜率最大退化均小于1%，观测最大约+0.215%；四种普通形状的spill相同。但周期既有正也有负，Function fp三轮cycles均上升，mem三轮均下降，尚未建立到本批删除的因果，不以text减少宣称整体加速或称其为噪声。FP相对FEX约3.440/3.227倍指令、call约1.798/2.867倍指令的差距仍在。该固定guest/CRC集合不证明所有高压力/helper形状、全部CPUID能力或正式全重构FEX质量门。

六组主点的governor为walt、CPU4 min/max614400/1171200 kHz、USB在线且Charging；cpuss的组内before范围总体32.0–36.3°C，cpu1-0为32.0–36.7°C，cpu1-6为30.9–34.8°C，电池28.0–28.7°C。每点前后条件原文保留，不改变散热/频率/充电、不将跨组条件变化合并成收益。

采样接线有两项保留记录：PMU-v1因旧oracle guest路径在零adb命令/零采样点前失败；后续使用相同哈希的现有guest。call-v2的42个点均已完成且raw闭合，离线汇总因兼容tmp软链接下的外部相对路径失败；只生成manifest-resolved和离线路径修复记录，保留原manifest/异常，不重跑任何call点。其余五组从物理Data2路径开始并直接通过。

### 独立编译诊断与尚未关闭的成本项

六组主PMU全部结束后，另用已有SVM_PROF/SVM_PROF2在同一CoreMark2000输入做12次交错的新进程诊断（两种请求模式×两产物×三轮），退出与CRC全部正确；24份原始stdout/stderr及传输tar逐文件校验。该组不混入主PMU，也不把嵌套inclusive timers相加。

| 请求模式 | translate_total配对中位数 | codegen_total | codegen_body | codegen_complete |
| --- | --- | --- | --- |
| function | +2.187% | +5.326% | -1.334% | +12.894% |
| block | +2.289% | +3.709% | +0.164% | +0.977% |

Function的codegen_complete三轮比值1.129/1.032/1.137，codegen_total在两种模式的三轮均上升；这些观察不证明构建成本门通过，需继续定位。各轮实际调用数略有formatter差异，JSON同时保留总ns、calls和每call归一结果，不能以一次计数或固定帧推断完整收益。

诊断还发现旧粗粒度PROF的Block路径没有填写block_units：请求SVM_FUNC_BASE=0时粗func_units/block_units均为0，但详细translate/regalloc/body和scratch准备计数明确非零；源码source/translator/x86/translator.cpp仅有Function的func_units更新。最初汇总错误地要求Block粗计数为正而停止，后续只修正离线解释、保留零值与全部12次运行，没有重跑或补造计数。当前模式标签来自显式请求，不能由这个未接线计数器证明实际分配入口类型。该缺口、Function准备计时上升、完整私有堆/容量和叠加栈仍为后续成本验收事项。

### 存储和继续施工

全部消费结束后，手机19个测试ELF（含首版6个与最终13个）、24个私有PMU ELF和4个传输tar共47路径按SHA校验后删除，回收4045529088 allocated bytes；raw运行/PMU/profile日志、guest输入和配置保留，私有进程与socket已关闭。Data2另退役两份被当前/相邻版本替代的旧ELF归档及一次共享PMU输入bank的四个ELF名字；只按删除前link-count1统计，实际唯一allocated记录减少1027186688 bytes。candidate/FEX硬链接删除不重复计作释放；parent du还含指向外部固定cache的链接，不能冒充物理释放。恢复指定baseline/current launcher的索引、旧二进制身份、原始seal、源/Git/WIP及2675项工作文件保留且清理前后校验相同。当前与相邻28成员归档均保留；两个已退役中间二进制归档不再承诺逐字节恢复。

阶段汇总为`/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/r7-emission-inputs-v2-six-workload-PMU-summary.json`，编译诊断为同目录`r7-emission-inputs-v2-compile-profile-v1/summary.json`；回收索引为`r7-emission-inputs-v2-rolling-reclamation.json`。大输出和TMPDIR实际位于Data2，继续复用固定Android/RV缓存。新Data2及手机回收不重复计入原系统盘277194584064-byte历史释放。

本批完成具体发射fallback退役、相关正确性矩阵和当前六类PMU采样；完整目标保持开放。下一步按实际数据修正粗粒度Block计数的生产位置，定位编译准备成本上升及15.4完整私有容量，再对照14.2/14.3完成剩余验收。后向局部label和ARM64 Phi/local维持方案既有拒绝边界，不以新能力实现扩张本次范围。

## R7：补齐编译统计生产位置（2026-10-25，compile-accounting-v1）

先完成用户指定的磁盘释放复查，再恢复本批重构。原 `/private/tmp/swiftvm-middle-results` 及两处构建入口均为指向 Data2 的软链接，系统盘 entry allocated 为0；原系统盘累计释放277194584064 bytes保持历史身份。本次额外删除39个已结束的旧PMU二进制、工具及guest包装副本，按唯一inode实际回收269594624 bytes，独立active-results的du下降相同。原两份证据压缩包、当前/相邻产物归档再次SHA校验，清理边界内2675项代码/Git/WIP指纹相同，无写入者和清理错误；结果写入用户指定WinEmu协调回执。之后的源文件修改属于恢复执行的重构，大数据和TMPDIR仍实际落在Data2，复用固定缓存。

本批修改6个既有生产文件，另更新方案与进度文档；没有新增源文件、配置、目标、测试用例、probe或仓库脚本。Block/Function分配算法和准入、发码、cache40/AOT6保持原实现。Source SHA256为 `d2d9848f0ba7b3649f94f7b457d3770586a55f583719449051000b2a1cea2b9f`，overlay478、retired118；相对emission-inputs-v2的patch为 `650660014b13a440c41757bf4e0576134c9c72cdf65e24d437f4b0cc13d7bfdd`。

### 接线及口径

| 既有文件 | 本批改动 |
| --- | --- |
| `translator/x86/translator.cpp` | Block实际完成新decode后填写block_units和decoded_blocks，decode计时与细分时钟同边界；Function原计数位置保留，更新受PROF开关保护 |
| `runtime/compile/pipeline.cpp` | Block优化、重编号与封口纳入粗opt阶段 |
| `runtime/compile/allocate/allocation.cpp` | 公共AllocateBlock/AllocateFunction分别记录默认关闭的入口时钟与calls，参数准备、内部attempt/retry另计 |
| `runtime/backend/arm64/compile/compiler.cpp` | 补Block粗regalloc；为既有edge/state/terminal准备、payload/位置封口、完成请求的输入释放增加plan_edges/seal/release三个阶段桶 |
| `runtime/backend/runtime.cpp` | 普通Block及已封口HIRBlock两条发射入口补粗codegen/publish、host/IR容量统计；普通Block释放IR补粗时钟。既有缓存命中、发布与回退流程保留 |
| `runtime/common/perf_stats.h` | 明确前端decode单元与实际分配入口的区别，输出上述五个默认关闭的细分桶；single/multi继续只表示decode形状 |

Function入口内部仍可使用原单块interval快速路径；公开入口次数不等于多块Function分配或内部attempt次数。plan_edges内含deferred-location的plan_targets区间，细分时钟保持inclusive。旧Block缺失的粗统计不回填，新旧覆盖范围改变不计作耗时退化。

### 一次集中构建和有限验证

十四个Android目标固定缓存增量构建0（116.51s、356步）；十四个完整ELF均改变。当前launcher SHA256为 `2bb18f25033227457ed6f619e9950fbfac4319e21d50ab9ff4614f02b30c1fce`，122016256 bytes；text4504140→4505140（+1000）。新增五个PerfCounter2共80 bytes属于进程级诊断结构，未新增per-unit/per-node所有者；当前Android DWARF的PerfStats2为1480760 bytes。这些静态数据不代替动态私有容量或默认路径开销验收。

实际执行5项当前native调用及1项相邻控制，另6次CoreMark2000诊断与2次关闭诊断的CoreMark12000负载；没有重复137项生命周期矩阵或六组PMU。

| 范围 | 实际结果与限制 |
| --- | --- |
| 默认主套件 | 762分项、506174断言，仍相同11分项/66失败、0error，全部失败签名与相邻一致；总断言−91未归因，没有为凑数量重跑 |
| IR快慢两路 | 各113分项、108064/108162断言，均0fail/error，与相邻一致 |
| 既有region隐藏指标 | 441断言通过；该旧fixture未检查Translate的bool结果，单凭此通过不作为完整成功发射/编译成本证据 |
| 既有backedge隐藏指标 | 当前在Translate(function)准入处失败；从相邻已校验归档只恢复主测试这一个成员，以同命令/env控制复现相同失败。两个实际exit1和原始JUnit均保留，未修改期望值或绕过守卫，不计为成本/正确性通过；主套件及已完成IR/region没有重跑 |
| 生产CoreMark | 六次2000诊断退出0、crcfinal=4983；两次12000普通负载退出0、crcfinal=d340，普通负载无PROF输出。固定次数不足十秒的CoreMark提示保留，不宣称标准CoreMark成绩 |
| 来源/传输 | 当前生产日志及相邻控制共34份raw文件逐文件核对device/tar/local哈希一致；初次离线解析误用了另一种CRC文字格式，随后只改离线解析，运行点没有重采 |

### 观察到的入口和准备阶段

Function请求三轮均为func_units437、block_units7、decoded_blocks4902，真实公共分配入口各为Function437/Block7；Block请求三轮block_units为2414/2412/2416，实际Block入口与之相同，Function入口均为0。Block粗decode/opt/regalloc/codegen/publish/IR-release及host/IR容量统计均非零，补齐了原生产接线缺口。这里的437个Function入口仍不证明全部为多块interval算法。

| 当前源码诊断中位数 | Function请求 | Block请求 |
| --- | --- | --- |
| codegen_complete | 225.383 ms | 202.159 ms |
| plan_declare | 96.119 ms | 82.927 ms |
| plan_perblock | 68.017 ms | 62.214 ms |
| plan_verify | 23.388 ms | 17.573 ms |
| 新plan_edges | 19.358 ms | 9.586 ms |
| 新plan_seal | 3.219 ms | 7.206 ms |
| 新plan_release | 1.810 ms | 5.203 ms |

Function的edge阶段占complete约8.69%，seal约1.37%。这些是新增时钟下当前源码的组成，不能与旧源码缺失时钟机械比较，也未解释上一批Function complete三轮均上升的原因。声明、逐块准备、部分setup及对象析构仍需结合工作量/容量归因；完整私有堆、最大叠加栈和总体编译成本门保持开放。

### 产物和接续

当前native14采用单份流式压缩归档 `r7-compile-accounting-v1-native-elfs.tar.zst`，407707216 bytes、SHA256 `6fbc23b03033a556fd1fdcf651a94d0b2320a353104dea990ceddcd0882a78fe`；全部14成员解压哈希验证，没有新建展开的结果ELF bank。固定Android缓存保留当前十四产物；相邻emission-inputs-v2的28成员归档继续保留。当前公共头与实现发生编译输入变化，本批尚未重编RV，原RV72和旧六类PMU仅对其实际旧源码成立，不升级为本批资格。

消费结束后，手机4个当前/控制ELF及2个传输tar共6路径校验后删除，回收791756800 allocated bytes，34份原始日志保留；host仅恢复过的单个相邻控制ELF也已删除，265228288 bytes临时展开量没有计作永久新增释放。shared adb保持，结果实际写Data2，无新展开的整组ELF结果副本。

详细记录位于 `/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/r7-compile-accounting-v1-runtime/summary.json`。本批完成Block统计和分配入口口径接线、补准备尾部归因。继续核对隐藏指标的发射准入/成功前提，完成当前源码RV资格及15.4私有容量、工作量和编译成本收敛；完整重构、质量和正式FEX/R7门均未宣告完成。

## R7：修正隔离指标输入并补齐 RV64 来源资格（2026-10-25，metric-inputs-v1）

本批只修改既有 `source/tests/region_compile_metrics_test.cpp`，生产源码与compile-accounting-v1相同，另接续两份文档；没有新增源文件、用例、目标、模式、配置、probe或仓库脚本。Source SHA256为 `3f3a3b407cc155ccf815c822757b5ca7ccc2e5272c48adca1172a4dab50aed11`，overlay478、retired118。

### 修正原因与行为

上一批backedge指标在相邻与当前二进制均于发射前被拒绝，目标声明/准备计数为0。沿SSA检查定位到两个U32状态写：`TestFlags(ValueType::U32, ...)` 的定义是U32，但返回的BOOL包装携带U8使用视图；StoreUniform的32-bit范围与该视图不符，SSA宽度守卫正确拒绝。现保留这三个显式U32 flags定义的canonical view（PF、AF及窄整数carry指标），保持原输出槽和结果语义；未放宽生产检查或添加补偿IR。

五处原来忽略Translate结果的测量调用改为检查成功，所有手工FeatureSet改取实际Module快照；region指标通过模块override显式保留flags_region_branch策略。拒绝结果不会再因prologue已有非零字节而被记作成功编译。

### 一次集中 Android 验证

只增量构建主测试目标，退出0、4.75s；仅主测试ELF改变，其余十三个完整Android ELF与相邻逐字节相同，原生产CoreMark诊断/CRC证据按实际二进制身份引用。未重复默认主套件、IR套件、137项生命周期矩阵或六组PMU；上一批默认主套件11分项/66失败和未归因的断言−91保持原来源，不改写为本批新执行。

在指定红魔手机CPU4，以已有PROF2和完整VERIFY_PLAN执行一次组合过滤，八个既有隐藏用例共7370断言，0failure/error：

| 既有指标 | 输出形状/长度记录 |
| --- | --- |
| region compile | 9 |
| integer branch compile / phases | 12 / 4 |
| narrow integer | 12 |
| scalar preparation | 12 |
| bit/funnel | 18 |
| backedge compile | 12 |

合计79条指标记录；另有backedge native smoke实际执行两形状×两长度×三初始值的冷路径并核对PF/AF答案。公共分配入口实际记录Block1530/Function529，target verify/edges/seal各2059次；这些是现有手工夹具的隔离数据，不能当作封口生产pipeline、关闭诊断的性能或完整私有堆成本。上一批失败raw及相邻控制保留，修复后成功单独记录。

### 当前 RV64 编译与执行

实际批次为 `r7-rv64-execution-20261025-v73`。2557个精确输入冻结在Data2固定source目录，输入摘要 `bc075112d1461cc99babc80834051284c6dee1b807cca2c94d011d159e254e84`；使用已有环境image `64885f0cafd88e81bc8689534edaf5bf01e26ca00505657a3c978ad57380e48a`、固定 `/build` 缓存及新的固定容器名 `swiftvm-middle-rv64-fixed`，以后复用，不再每批新建环境image/构建目录。

十四目标完整增量构建0（180.47s、441步），12个完整ELF改变，host-features与emit两ELF不变。当前既定RV集合的37条注册command/properties均与原记录相同；全仓实际注册254项，37指这些RV目标的既有子集。

- 29项受影响用户态在patched QEMU下新执行，全部通过；3项host-feature用例以完整ELF、command/properties、QEMU和image/sysroot相同复用原成功，记录仅引用原case路径/hash，不递归复制复用历史。
- 5项依赖真实Linux内核的icache模式在一次新guest boot中执行：cross-hart、fail-ordinary、fail-signal、warm-fail-signal、fail-data均退出0；双hart、128轮/2048次取指和失败关闭记录保留。复用原kernel/BIOS/init及其余rootfs成员，只替换实际改变的测试ELF；新rootfs输入SHA256为 `5f8d84ab0649160a7237ef00a117ecae56a045fd6b705dd67454f55c79e5551b`。
- strong-TSO完整ELF也改变，实际新执行通过220321 checks；没有复用旧计数。两项compile-cost和emit仍仅构建，不宣称物理RV缓存/内存序或性能证明。

独立复核rootfs时确认：唯一发生内容/元数据值变化的成员是测试ELF；重打包同时将newc header十六进制字母规范化为小写，因此不宣称其余header原始字节相同。所有非测试成员的字段值、名字与数据相同，实际执行的完整压缩输入及SHA均保留；没有因此重新启动guest。

此前离线装配过程曾混淆全仓注册表与所选37项、以及QEMU-system实际路径；在发出任何测试点前修正。实际新执行为29用户态、5个guest内核场景及一次strong-TSO，无因此产生的运行重采。

### 有界保留与继续施工

当前28产物以索引 `r7-metric-inputs-v1-current-checkpoint.json` 解析：只新增1个native与12个RV改变成员的归档 `r7-metric-inputs-v1-changed-elfs.tar.zst`，87926517 bytes、SHA256 `38ce7316d0c5b29244bfb9a39381cf8a06bca5b98ef44b4ac037bbc82abf6c05`；13成员均解压校验，其余15成员引用两份已有、重新核对归档SHA的已验证产物，不再复制完整28组。

当前RV全部2557编译输入另存单份可恢复压缩包 `r7-rv64-execution-20261025-v73-compiled-inputs.tar.zst`，11991801 bytes、SHA256 `5e3e6940e48704b75978ff4e77e4a2d6303709e2de78cc0b03165c420cd3998c`，全部成员解压核对。下批只有在consumer关闭且旧输入完整归档后才更新固定source目录。内核原始console、用户态JUnit/LastTest及strong-TSO日志各保留一份，派生记录通过路径和hash引用。

消费结束后删除手机主测试ELF及传输tar，回收265682944 allocated bytes，三份raw保留；Data2内核打包副本12845056 bytes也已删除，精确输入压缩包留在结果归档。后者属于本批创建并回收的临时副本，没有追加到历史永久释放量。固定RV容器只剩sleep时停止，记录的容器exit137与此前构建/测试exit0分别保存；shared adb及其他项目容器未动。系统盘原tmp入口继续为零allocated的Data2软链接。

接续记录分别为同一Data2结果目录的 `r7-metric-inputs-v1-runtime/summary.json` 和 `r7-rv64-execution-20261025-v73-test-summary.json`。指标输入与当前RV资格缺口已修正；准备阶段的私有临时容量、构造/析构及可归因工作量继续收敛，再完成15.4和剩余14.2/14.3验收。原六类PMU仍只对emission-inputs-v2及其相邻源码成立，完整成本、正式FEX/质量/R7与全重构目标保持开放。

## R7：收短目标准备输入的寿命（2026-10-25，preparation-lifetimes-v2）

本批实际修改三个既有生产文件：`compile/lower.cpp`、`compile/residences.h`、`compile/residence_preparation.cpp`；另修改一个既有测试辅助文件 `source/tests/support/register_alloc_test_support.cpp`，文档另计。Source SHA256为 `7959833c4e82d226b3573598b7fbbbe8dd4acec45f5068d75f5ad545dcabc24f`，overlay478、retired118；没有新增源文件、测试、配置、probe或仓库脚本。

### 实际退役与交接

- FPR contract roster 改为遇到第一条实际证明输出时才按块数建立。presence 触发的必要验证仍执行，invalid 的空输出也在任何 GPR 声明前拒绝；没有证明行的单元不分配、默认构造整组空 contract。
- 保持每块 GPR 声明后紧接其 FPR 认证的顺序。`CertifyFPR` 复制 ID/action/control，原证明 payload 随该次认证结束释放；逐块 lowering 前释放整个 roster 的头部存储。有证明输出时仍是逐块稠密 roster，没有将其包装成新的稀疏容器或所有者层。
- publication 和 memory selection 的临时 owner 移入本块作用域，在 `Store*` 复制必要行及本块消费者结束后释放。PF 发表指针投影、物理写的最终认证及 resident 的块身份保护保留，没有把 ID 当作跨独立块身份。
- blocks/pending 根据既知块数 reserve；删除 `GPRBlockPreparation` 内生产路径无人读取的重复块指针。测试辅助使用既有 `allocation.InputBlock()`，不新增兼容字段。独立 Block 和 Function 分配入口、单块 Function 的实际算法选择、cache40/AOT6及既有准入策略均保持。

### 容量与代价分别记录

本次 ARM64 DWARF 与实际 launcher 编码给出以下结果，基线为 metric-inputs-v1 的实际生产二进制：

| 对象或主体 | 相邻 | 当前 | 解释 |
| --- | ---: | ---: | --- |
| `GPRBlockPreparation` | 64 B | 56 B | 去掉重复指针 |
| `PendingGPRBlock` | 184 B | 176 B | 包含上一行子对象，不能合计成每块减少16 B |
| `FPRContract` / `TargetPreparation` / `UnitActions` | 40 / 2160 / 864 B | 40 / 2160 / 864 B | 固定头保持 |
| `DeclareGPRBlock` code / 自身固定栈 | 1880 / 336 B | 1872 / 336 B | 局部编码变化 |
| `Complete` code / 自身固定栈 | 7992 / 1664 B | 8572 / 1760 B | code增加580 B，自身栈增加96 B |
| launcher `.text` / 完整 ELF | 4505140 / 122016256 B | 4505908 / 122025080 B | text增加768 B；完整文件还含debug等内容 |

无实际证明输出时，旧 roster 的每块40 B逻辑头部数组消失；pending 的逻辑记录每块减少8 B，并避免逐项几何扩容。这里没有新增私有堆计数器；allocator舍入、真实峰值/保留时间、其他容器、最大叠加栈与整个编译器成本仍未验收。生命周期缩短与新增自身栈需要一起评估，不能只引用布局减少。

### 一次集中原生验证与三轮对照

首轮 native 构建因测试辅助仍读取已删除的 `draft.block` 失败（退出1、19.78s），原日志保留；改为既有块输入后，十四目标增量构建退出0、3.17s。成功构建、失败尝试和源码资格分别记录，没有把首轮写成成功。

指定红魔CPU4共执行21次既有程序：当前18次、相邻控制3次。默认主套件762分项、506167断言、11失败分项/66失败断言、0error，失败签名与compile-accounting-v1相同；断言比该次原始记录少7，原因未归因。IR fast/slow各113分项、108064/108162断言通过；四个既有HIR寿命模式各覆盖fast0/1，共8次通过。

八个既有隐藏指标/冷路径用例按三轮 reference/candidate 交替执行，每次7370断言、0failure/error，输出79行：75行带机器码/质量字段的指标，以及4行阶段时钟。六次输出中相同形状/长度的bytes/hash/spill/reload/store等非时钟字段一致。下表是每行三次配对百分比变化中位数，再在同家族各行取中位数，不能作为总体编译时间：

| 家族 | 行数 | 配对行变化中位数 |
| --- | ---: | ---: |
| region | 9 | −0.82% |
| integer branch | 12 | −0.08% |
| narrow integer | 12 | +0.02% |
| scalar preparation | 12 | −0.29% |
| bit/funnel | 18 | +0.50% |
| backedge | 12 | −0.12% |

逐行原值、三轮median/MAD/min/max与五类阶段时钟保存在 `r7-preparation-lifetimes-v2-runtime/paired-metrics.json`。指标使用手工可变夹具，PROF2/VERIFY_PLAN开启；没有据这些小幅差异宣称提速、完整私有堆下降或解释既有成本回归，也没有新增PMU点。

普通 Function/Block 两路径各运行CoreMark2000和bench一次：CoreMark的`[0]crcfinal=0x4983`及bench五项checksum正确，短于10秒的原始时长告警保留，不宣称标准CoreMark成绩。Function请求的CoreMark实际公共入口为Function437次、Block7次，bench为Function23次；Block请求则分别为Block2418/101次、Function0次。这是公共入口计数，不能冒充Function内部interval算法或所解码块数。

### RV64 与有界保留

实际编译批次 `r7-rv64-execution-20261025-v74` 使用preparation-lifetimes-v1源码：十四目标增量构建退出0、9.37s，只有4个RV完整ELF改变。最终v2唯一额外修改是ARM64测试辅助文件；实际14目标命令及416个选中object的依赖均不包含它，生产源码相同，因此单独记录资格，没有为此重编或改写实际编译来源。

37项既有RV子集中，17个受影响用户态用例新执行全部通过（119.24s）；15个用户态及5个真实Linux guest内核模式按完整ELF、command/properties、QEMU/image和相关输入身份精确复用。strong-TSO也复用原220321检查；没有新guest boot。全仓注册254项，两项compile-cost和emit仍仅构建，不宣称全仓、物理RV或正式内存序/性能证明。

当前28产物的索引为 `r7-preparation-lifetimes-v2-current-checkpoint.json`：新增18个改变成员的单份流式归档406165161 B，SHA256 `57648afbd20f165ece051b33b54461c3ac01f1d3cc362925b4a13439f3ed9b10`；全部成员解压验证，10个未变RV成员引用已有验证归档。没有创建展开的整组结果bank。实际RV2557输入单份归档11620183 B，SHA256 `0845c87bb4c400e50128ccc276871c11c89459bcfa21c80713c2a0824c8e7953`，全部解压校验；consumer关闭且旧输入归档后才能更新固定source目录。

本轮期间先完成用户磁盘清理复核：原tmp结果与两个构建入口均为Data2软链接，系统盘占用0；历史277194584064 B释放量不重复计数。四份已停用RV69..72输入压缩为47097567 B，10228文件解压验证后删除展开副本，Data2净回收217239552 B（不含小量记录开销）。手机5个ELF及1个tar在21次运行结束、51份raw核验后删除，回收896987136 B；原始结果保留。后者是本轮临时副本回收，不计作系统盘/Data2永久新增释放。

记录实际位于 `/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/`；原生解释见 `r7-preparation-lifetimes-v2-runtime/summary.json`，RV见 `r7-rv64-execution-20261025-v74-test-summary.json`。固定缓存、源码/Git/WIP、必要相邻证据保留，闲置固定RV容器停止，shared adb与其他项目资源未动。目标准备的本批短期输入已收口；15.4总私有成本、最大叠加栈、剩余14.2/14.3和当前源码正式PMU/FEX/质量/R7验收继续开展，全重构目标保持开放。

## R7：退役标量选择的五张指针表（2026-10-25，scalar-selection-storage-v1）

本批修改四个既有生产文件：ARM64 `compile/lower.cpp`、`compile/lower_scalar.h/.cpp`，以及公共 `analysis/block_summary.h`；另迁移既有 `lowering_coverage_test.cpp` 的调用。Source SHA256为 `6eb5c706e832040b6e88942836dea81375f4e4395234cec62fc49134bccf865a`，overlay478、retired118。没有新增源文件、测试用例、模式、配置、probe或仓库脚本。

### 生产交接与实际工作量

调用方的blocked-extract/discarded/flagged三张指针哈希，以及窄位builder的extended/masked-shift两张哈希全部退役。`ScalarBitInputs`现在只借用已有copy、整数flags、branch及coverage；packed pseudo需求读取同一共享index。物理视图、coalescing、flags输入和发表选择仍在分配后查询，没有前移到decode。匹配先用共享roster的相邻指针确认归属，再按ID读取实际结果；跨块tied来源仍允许，局部masked-shift角色不按外块同号节点匹配。

公共必需摘要增加Xor及And/两种ZeroExtend的presence位，仍为8 B。无对应opcode时分别跳过copy全扫和窄位候选准备；不创建consumer图。窄位有候选时，用既有workspace缓冲一次收集ZeroExtend、And及可能被消费的Lsr/Asr位置，四阶段按原优先级遍历这些四字节记录，复用角色位记录选中的shift；输出保持一次最终排序，结果不借用临时缓冲，旧Block便利入口删除。

设块有N条IR、候选记录Nc条，窄位入口有需求时仍有一次N访问和四次Nc访问，另有真实匹配的二分/物理查询及输出工作；最坏Nc=N。调用方原N次投影扫描已删除，并非所有输入只剩一次总访问。共享摘要的额外比较、每条候选的字段写入及缓冲清理/保留也属于成本。

### 布局、编码与容量代价

| 对象或主体 | 相邻 | 当前 |
| --- | ---: | ---: |
| `ScalarBitInputs` / `NarrowBitBuilder` | 128 / 152 B | 32 / 80 B |
| `BlockSummary` / `TargetPreparation` / `UnitActions` | 8 / 2160 / 864 B | 8 / 2160 / 864 B |
| `Complete` code / 自身固定栈 | 8572 / 1760 B | 7988 / 1664 B |
| `LowerNarrowBits` code / 自身固定栈 | 312 / 192 B | 284 / 144 B |
| builder `Build` code / 自身固定栈 | 7128 / 304 B | 6932 / 288 B |
| launcher `.text` / 完整 ELF | 4505908 / 122025080 B | 4505844 / 122022472 B |

新增SeedCandidates为468 B code/128 B自身栈；FindCandidate为276 B/0自身栈，CopyDiscarded为108 B/0，BlockedExtract为232 B/32。包含这些helper的标量方法组由9836增至10608 B，不能只列Build减少196 B；`Complete`另减少584 B，launcher总text只减少64 B。无候选的入口仍可有144 B自身固定帧；完整调用链、最大叠加栈和默认关闭诊断的完整成本尚未证明。

既有workspace统计还观察到新增共享容量，而非“复用即零扩容”：

| 普通诊断路径 | lowering-candidates分配次数 | 累计请求字节 | 峰值live字节 | 峰值retained字节 |
| --- | --- | --- | --- | --- |
| Function CoreMark | 9→9 | 2044→2044 | 1536→1536 | 1024→1024 |
| Block CoreMark | 9→9 | 2044→2044 | 1536→1536 | 1024→1024 |
| Function bench | 6→7 | 252→508 | 192→384 | 128→256 |
| Block bench | 6→7 | 252→508 | 192→384 | 128→256 |

四条当前路径最终live/retained均为0；scope保留事件分别增至6438/4253/350/218。两次CoreMark的实际unit数有差异，差量不能严格归因；旧五张哈希未纳入workspace分配统计，该表也不能推断所有C++私有堆的净收益。实际buffer、普通路径和引用日志见 `r7-scalar-selection-storage-v1-runtime/metrics-and-profile.json`。

### 一轮集中验证与保留

Android十四目标一次增量构建退出0、49.89s。指定红魔CPU4执行16次既有程序：主套件762分项、506141断言，原11失败分项/66失败断言签名保持、0error；比相邻少26断言，未归因。IR fast/slow各113分项、108064/108162断言通过；四种HIR寿命模式×fast0/1共8次通过。八个既有隐藏指标一次执行7370断言通过，75条机器码/质量记录和4条阶段记录的非时钟字段与相邻一致。没有新增配对耗时或PMU矩阵。

普通Function/Block各一次CoreMark2000和bench，CRC4983及五项checksum正确，短时长告警保留。CoreMark实际公共分配入口为Function438+Block7，或Block2412+Function0；bench为Function23，或Block101。入口统计与Function内部interval算法、解码块数仍分别解释。

RV75按本批实际源码构建十四目标退出0、37.48s，4个完整ELF改变；17项受影响用户态新执行通过（118.59s），15用户态与5真实guest内核场景精确复用，strong-TSO复用原220321检查，无新boot。37仅指既有RV子集，全仓仍254项；两项compile-cost与emit仅构建。

当前28索引 `r7-scalar-selection-storage-v1-current-checkpoint.json` 解析18个改变成员的新归档406138504 B（SHA256 `331ddcdecd816513c436525089a1f0c13543e0daf38efc39493aced0935b6525`）和10个已有未变RV成员，新增成员均解压校验，无展开结果bank。实际RV2557输入归档11633021 B，SHA256 `c675becbe70136301dda9d46a35e88c3545d8807f56122c8721c292e83abf90f`，全部解压验证；继续复用固定source/cache/container。

当前及紧邻preparation-lifetimes-v2的28产物均可恢复后，回收不再被两者引用的compile-accounting-v1旧native14生成物归档，独立du确认407707648 B。旧源码overlay、原始运行/失败记录和历史manifest保留；旧checkpoint对该已退休归档的引用只作历史记录，不再承诺逐字节恢复旧归档。手机本轮4个ELF和1个tar回收631177216 B，36份原始文件保留；固定RV容器只剩sleep后停止，其他项目/shared adb未动。

原生结果见 `r7-scalar-selection-storage-v1-runtime/summary.json`，RV见 `r7-rv64-execution-20261025-v75-test-summary.json`，均位于既有Data2结果目录。本批完成五张临时表与重复全块扫描的收敛；真实私有堆/完整调用链成本、剩余14.2/14.3及正式PMU/FEX/质量/R7继续验收，全重构目标保持开放。


## R2：标量 ALU 整数域准入（2026-10-26，scalar-alu-admission-v2）

本批补齐共享实际参数检查中遗漏的十六类整数 ALU，不改变运算数值规则、位宽策略或后端支持能力。唯一生产改动在 `runtime/ir/instr.cpp`；沿用同一签名/嵌套参数检查及固定语义槽校验，不增加 opcode 表、逐指令缓存或第二遍 uses 枚举。

Add/Sub/Adc/Sbb/Mul/Div/And/Or/Xor/AndNot、二元 Not、Neg、MulSub、SignedDiv64、MulHigh、Div128 的 Value/Imm 和结果必须为整数域。Operand 保留单项、空右侧、Value+Imm、Value+Value、Imm+Value 等原有形式；有符号别名及现有显式结果覆盖继续接受。本批不额外限制同宽、固定 64 位或 selector 数值，不规范化 BitCast/GetResult 的原始位运输。

### 实际缺口与永久回归

先仅增加永久测试，在未改生产代码时以 fail-fast 执行一次：无用的 V64 Add 被 CF/DCE 删除，PrepareBlock 错误返回成功。实际首个失败是 REQUIRE_FALSE(prepared)，test exit=1；原始 XML/stdout/stderr、构建记录和当时的测试源码压缩快照均保留。最终用例后来补齐 Operand 排列和安全的节点归属检查，不能将该早期 RED 说成最终测试源码的执行结果。

最终 `ir_contract_test.cpp` 既有测试目标覆盖 462 个合法整数断言及 424 个独立非法 Block/Function，包含 V64/V128 输入、Imm、结果和活跃物理槽。拒绝后先确认原节点仍在链表中，再读取它，确保不是优化后才失败；同时检查空代码缓存、SupportsIR、封口 SSA 与 interpreter。既有 RV64 pure_flags 程序增加 248 个非法 Block/Function，直接发射与模块编译均拒绝，直接入口缓冲区为 0。没有临时 probe、新测试脚本、目标或运行模式。

### 一轮集中验证及一次必要的夹具修正

- Android v1 十四目标构建退出 0、27.64 s；红魔 CPU4 的既有 16 次集中执行全部达到预期终态。IR fast/slow 各 114 分项、112342/112440 断言通过，各比相邻增加 4278 断言。新增 ALU 用例只在这两份 IR suite 中执行，不在默认 main suite 中。
- 默认 main 762 分项、506232 断言；原 11 个失败分项/66 个失败断言及完整失败正文保持，0 error。总断言比相邻多 91，未归因。HIR 八种寿命组合通过；八个隐藏指标一次执行 7370 断言通过，75 条代码质量与 4 条阶段记录的非计时字段保持。Function/Block 的 CoreMark2000 CRC4983 与五项 bench checksum 正确，短时长告警保留。没有新增配对耗时或 PMU 矩阵。
- RV76 十四目标构建退出 0，新执行 19 个既有用户态用例，其中 15 通过、4 失败。新 248 个非法 ALU 检查已通过；四个 NativeWide 路径仍期待旧的晚期 V256 错误消息，实际已由公共准入提前拒绝。完整失败记录保留。
- v2 只修改既有 `riscv/native_wide_test.cpp`，改为验证准确的提前拒绝消息以及零发射字节。Android 十四目标增量构建 2.60 s，只有 RV helper ELF 改变；16 次原生运行用到的四个 ELF 完全相同，因此复用 v1 的实际运行结果，不重新标记来源。RV77 十四目标构建 4.89 s，只有 backend ELF 改变，13 个对应用户态用例新执行通过（136.12 s）。另 19 用户态和 5 真实 guest 内核用例按完整 ELF、命令/属性及执行环境精确复用，既有 RV 子集 37 项全部有通过资格；strong-TSO 新执行 220321 检查通过。37 不代表全仓 254 项，compile-cost 两目标和 emit 仅构建。

v1 实际源码摘要为 `235014148f47f8346884d4ccbef1b64de43d9afa193a7684d5c23ddb507876a1`，v2 为 `82b90c1cf8bbf78347ff4672ed50959e48adc5685ee5f35fa3c416b4484e682d`。v2 的唯一源码差异是上述 RV 夹具，生产主体和原生四个实测 ELF 保持。文档在测量后更新，与实际编译源码的差异单独闭合，不把文档更新冒充重编译。

### 成本、保留与接续

| 实际主体/产物 | 相邻 | 当前 |
| --- | ---: | ---: |
| HasValidArguments code / 自身 SP | 1540 / 96 B | 1540 / 96 B |
| HasValidScalarArguments code / 自身 SP | 1300 / 32 B | 2016 / 96 B |
| launcher text / 完整 ELF | 4505844 / 122022472 B | 4506740 / 122027472 B |

整数域门是必要正确性检查，不能为减少帧而放行非法节点；新增 716 B 主体、64 B 自身帧和 launcher text 896 B 如实计入。表中自身 SP 不是最大叠加栈、完整私有堆或动态编译成本，代码质量记录相同也不能证明运行成本没有变化。

原始记录位于既有 Data2 结果目录：原生 `r2-scalar-alu-admission-v1-runtime/summary.json`，RV 失败与修正后记录分别为 `r7-rv64-execution-20261026-v76-test-summary.json`、`r7-rv64-execution-20261026-v77-test-summary.json`。当前 28 产物索引为 `r2-scalar-alu-admission-v2-current-checkpoint.json`：只新增两个变化 ELF 的 31085148 B 压缩归档，其余 26 项按索引复用；v1 的 23 个改变 ELF 归档及必要相邻源码/原始结果保留，无展开结果 bank。实际 RV77 的 2557 文件输入已压缩并全部解压校验。

无消费者且设备/本地 hash 闭合后，手机本批四个最终 ELF、RED 临时 ELF 和 tar 共回收 769224704 B，39 份唯一原始文件保留。系统盘原有 SwiftVM 临时入口仍实际指向 Data2；本批手机回收不重复计入原系统盘 277194584064 B 历史释放。后续复用固定构建缓存、source 和容器，继续及时回收生成副本。

下一步优先处理已确认的生产缺口：Params 的单一所有权、安装前可保留的可写节点与封口/uses 旁路；terminal 递归形状在全部入口的共同准入，以及合法 AL/NV 的 ARM64 分支发射。后者已发现 VIXL 条件分支不接受 AL/NV，尚需真实正向执行证明修正。Block 与 Function 分配仍分别验收。完整私有/动态成本、14.2/14.3剩余契约及正式红魔 PMU/FEX/质量/R7 门保持开放，全重构目标继续执行。


## R2/R7：Params 单一所有权与 terminal 契约收口（2026-10-26，owned-params-terminal-v6）

本批修复两个已确认的封口/准入缺口，并清理其重复实现：Params 构建器可以保留可写节点、浅拷贝安装；terminal 只有 Value 检查而没有各节点自己的完整形状规则。最终新增生产文件为 0，没有新迁移开关、probe、运行模式或测试脚本；原有 Block/Function 分配入口及能力边界保持分别验收。

### 文件职责与退役

| 现有文件 | 当前职责与本批收敛 |
| --- | --- |
| `runtime/ir/args.{h,cpp}` | Params move-only owner；私有 slab Node 与平凡 installed Storage 分离。迭代只返回独立数据快照，两个 Arg getter 都借用 View；ArgClass 不再提供 const Params 包装旁路，移动清空来源。Cond 枚举谓词也归基本 IR |
| `runtime/ir/instr.{h,cpp}` | 消费入口的 ConstructionArguments 与签名证明分开；Params&& 安装先验证再采用，按实际 occurrence 登记 uses；普通销毁注销，整体释放不解引用其他定义，均清空所有权 |
| `runtime/ir/block.h`、`hir_builder.{h,cpp}` | 正确转发 owner；链入前局部 Inst owner 处理失败，已有归属在 raw HIR 接管前拒绝。快慢 HIR 统一从 installed Inst 登记，UseFreshArg/AppendValueFast 退役；普通参数在真实大创建主体前归一化 |
| `runtime/compile/builder/ir_builder.h` | typed/raw 创建的真实主体也先归一普通引用，保留 Params&&；不为 T/T&/const T& 复制正文 |
| `compile/passes/passes.h`、`values.cpp`、RV `function.cpp/wide.cpp` | 改写转移 owner，真实逻辑复制仍显式新建列表；不共享可被替换或销毁的链表 |
| `runtime/ir/terminal.h`、`compile/analysis/uses.h` | 一个递归实现承载 shape 检查与只读 Value 枚举；包括未选择子树、全部 case 和独立 anchors，不复制树 |
| `compile/analysis/ssa.{h,cpp}`、`facts.cpp` | 形状、SSA 与 use-end 复用原访问；全部 Function 成员检查形状，不可达成员不附加可达支配要求。融合与独立路径结果一致 |
| ARM64 `compile/target.cpp/compiler.cpp`，RV `compile.cpp/condition_emission_contract.h`，`interp/interpreter.cpp` | 在优化删除、直接代码/帧发射及解释器指令副作用前消费共同契约；专用 capacity 的额外限制继续独立 |
| ARM64 `translator_terminal.cpp/translator_region.cpp` | AL/NV 都走 then，先按真实 pending 状态发表并作 canonical 交接；不把它们交给 VIXL 条件 B 或异或反转 |
| ARM64 `trampolines.cpp` | 公共新 Run 清旧 park、从公开 flags 播种 PSTATE；同次 helper 返回保留 park/unpark |
| `backend/code_serial.h` 与既有缓存 fixture | 共享 JIT/AOT 生成语义版本 40→41；在安装前拒绝旧代码。外壳格式 6/ABI 1 不变 |

没有把必要准入当作可选优化跳过。普通 Invalid dispatcher、空/重复 Switch case、混宽 signed Imm、有效宽整数 If 及窄 BOOL 视图继续合法；Standalone 专用入口原有 U8/explicit/owned-before 条件没有被推广到公共 API。ARM64 原有 Phi/local/后向 label 等拒绝边界未扩成新能力。

### 失败证据与 flags 修复

最初几次编译暴露的命名空间限定、两个旧可写迭代循环及测试私有接口调用已修正，源与日志保留，没有在失败构建上报告运行成功。首次真正原生执行为 V4：只执行首项 IR，119 分项、113498 断言、1 个新失败；后17项未跑。flat 256 个状态样本通过，region EQ 在新一次 Run 的 V=1 输入下选择正确，却把 V 清成0。完整失败、实际源码和28产物已单独保存，不复用为最终通过。

根因是初次 JitRun 加载新的公开 flags 后又恢复上次 Run 的 park；region 透明条件将旧 PSTATE 合回，覆盖新状态。公开 State 在停止状态允许写入，且没有另一个 import-flags API；park 不能取代公开 ABI。修复仅在公共入口重新播种，同次 helper 重入及内部边协议保留。

AL/NV 单边还需同时处理 dirty 算术标志、已清 canonical 位及部分清位，不能无条件 MRS/MSR。最终使用既有真实 mask 发表后交接；nested Condition 不获得外层 direct-link merge bypass，依赖现有门控和实际 inline 发表，不能泛称丢弃 deferred 描述符总是安全。

### 最终集中执行

| 范围 | 实际结果 |
| --- | --- |
| Android 构建 | 最终 V6 十四目标退出0，128.46 s；编译语义版本41 |
| 红魔原生执行 | 20/20达到预期终态，CPU4、隔离环境；main exit66，其余19次exit0 |
| IR fast/slow，flags_regs=1 | 各119分项，115594/115692断言，0fail/error；各比本批前增加3252断言 |
| terminal 既有 tag，flags_regs=0 | 6分项、3458断言，0fail/error |
| 新条件 oracle | 每次相关IR执行44个编译单元、704次Run；全部16条件×NZCV，Block与三块region；AL/NV覆盖dirty/full-clear/partial-clear，检查精确NZCV及AF bit26保持 |
| 默认 main | 762分项、506274断言；原11失败分项/66失败断言完整正文及stderr精确保持，0error；总断言+42未归因 |
| Params／HIR寿命 | 既有8组合通过，另construction fast0/1各330059 checks、5次既有注入失败后通过。私有Node容量8、峰值4，检查精确归还/重复释放、未安装与异常展开、move/复用、拒绝不消费、普通替换/erase及整体teardown |
| 缓存拒绝 | 既有fixture 191 checks通过；current41/old40实际status4、units0，现有入口与代码偏移身份保持；其余旧版本也拒绝 |
| 质量与普通正确性 | 八个既有隐藏用例7370断言通过；75条代码质量及4条阶段记录的非计时字段与本批前相同。Function/Block CoreMark2000 CRC4983及五项bench checksum保持 |
| RV83 | 实际V6十四目标构建退出0、83.87 s；19用户态新执行通过（129.40 s），13用户态＋5真实guest内核精确复用，既有37项全部有通过资格；strong-TSO新执行220321 checks通过 |

CoreMark实际短于10秒，原始ERROR/Errors detected保留；这里只确认CRC，不报告有效性能分数。RV37不是全仓254项、物理RV或完整内存模型证明。新Params测试不宣称Push节点OOM或完整RewriteContext事务恢复。没有新增PMU或配对耗时矩阵。

### 真实代码成本

V4曾增加49136 B可避免正文：AppendTyped 85→117份、RawTyped 53→110、HIRBuilder AppendInst 53→110；归一后的实际参数组合数从未增长。最终把归一化放在这三处真实主体入口，覆盖成员函数直接raw调用：

| 实际编译组 | 本批前 | 最终V6 |
| --- | ---: | ---: |
| typed builder | 85份 / 24712 B | 85份 / 24712 B |
| raw builder | 53份 / 16324 B | 53份 / 16324 B |
| HIRBuilder | 53份 / 20424 B | 53份 / 20424 B |
| HIR登记相关组 | 50份 / 17756 B | 2份 / 760 B |
| launcher text | 4506740 B | 4507036 B（+296） |
| 完整launcher ELF | 122027472 B | 122992344 B |

terminal新旧相关遍历组净增5584 B，与SSA命名组有重叠，不能重复加总；完整ELF包括debug等内容，不能当作纯代码或运行内存。上述是实际编码，不代表整体私有堆、最大叠加栈或运行时间收益。

### 来源、回收与下一步

最终479代码overlay摘要为 `3b42bffa52d77ce4f7faef2b87c9e7bd7a7cd3af42fc5e9b28f37e5007bb9e17`，118退役路径保持不存在；HEAD/index未变。文档在运行后独立更新，和真实2557编译输入的差异另作closure。原生结果见 `r2-owned-params-terminal-v6-runtime/summary.json`，RV见 `r7-rv64-execution-20261026-v83-test-summary.json`，均在既有Data2结果目录。

当前28索引 `r2-owned-params-terminal-v6-current-checkpoint.json` 只新增23个改变成员的456654366 B归档（SHA256 `780faf324a8e8199e287341421197603645409c55073a265ad22decabc4e5173`），5个完整ELF不变成员精确复用；全部新增成员解压校验。紧邻V4失败正向证据及本批前合格ALUv2参照也可恢复，无展开bank。实际RV83的2557输入归档12953911 B，SHA256 `061a14cc2ec9bafd7324a852128d20e04873109084aa14cba0c142ca0510bfba`，全部解压校验。

无消费者且SHA闭合后，手机V4/V6共回收9个ELF和2个tar，合计1387540480 B，3份失败与45份最终raw保留；固定RV容器只剩sleep后停止。当前/紧邻/本批前合格参照不再依赖的两份旧生成物归档再回收812306432 B，独立du差额精确一致。其旧源码、唯一原始记录和历史索引保留，退休归档不再承诺逐字节恢复。系统盘SwiftVM临时入口仍指向Data2且自身分配0，不重复计入原277194584064 B历史释放。

结构性删除及本批Params/terminal/flags缺口已收口，不再把它们列作“下一步”；R2E按未交付且无实验残留处理。全目标仍有具体门：映射最终适用矩阵；解释既有Function codegen_complete配对1.129/1.032/1.137的增幅；补齐所有私有容器、共存容量/保留与调用链最大栈；冻结后复用已合格Android FEX、guest和六组交错协议采集当前候选，写人工分项阶段判定。旧216主点/36冷服务点仅属于旧候选，不能贴给当前源码；比较器刻意不自动签formal FEX，原始a38c2db8 Block基线仍不合格，合格相邻参照不能冒充累计收益。此批不宣告完整成本、正式质量/FEX/R7或全目标完成。


## R7：冻结 V6 的既有矩阵与成本复核（2026-10-26，final-acceptance-v1）

本阶段没有改代码或重建产物。沿用479代码摘要 `3b42bffa52d77ce4f7faef2b87c9e7bd7a7cd3af42fc5e9b28f37e5007bb9e17`、原生十四目标与RV83；只有两份文档在验收后更新。三个并行只读审查分别完成矩阵、私有成本与旧计时归因，未新增probe、测试脚本、目标或运行模式。

### 当前产物的137项矩阵

旧emission-inputs-v2的原生完整ELF均已改变，不能继续把旧结果当当前通过。逐项比较命令、参数、配置和完整ELF后，final20可复用8项；其余129项在红魔CPU4集中新执行，全部exit0，driver耗时29.226604 s。六个optimized/construction组合在final20中另开PROF2，本阶段按旧要求的未开启profiling配置再执行。flags_regs缺省与显式1的归一有配置实现及既有missing/enabled环境身份测试支持，不擅自归一其他开关。

| 既有137项中的家族 | 当前资格 |
| --- | --- |
| IR fast/slow、默认main | 3项沿用final20；main仍为同一11失败分项/66失败断言，不称全套零失败 |
| HIR寿命 | 28项：prepared四项复用，余24项新执行 |
| entry / shutdown / call persistence | 6 / 2 / 8项新执行 |
| static-call latch | 54项新执行 |
| retirement control / cycle / static / dynamic | 8 / 3 / 17 / 6项新执行 |
| fixed-register-inputs选择 | 1次新执行，2分项/39断言、0fail；原配置为SVM_X86_PIN_EXT=3，不冒充启用ra_fixed_class |
| 缓存版本 | 1项沿用final20的current41/old40拒绝 |

冻结计划和实际argv逐字段核对，129新执行与8复用恰好构成137个无交叉、无遗漏的原条目。记录为 `r7-final-acceptance-v1-execution-plan.json`、`-executions.json` 与 `-matrix-closure-review.json`；259份raw的设备、本地及218112 B传输tar摘要完全一致。

这是V6/final-acceptance阶段的既有有限矩阵闭合，不是整个14.2/14.3或CTest全仓完成。该阶段当时没有显式SVM_RA_1BLK=0、SVM_BOUND_DECODE=0对照，也不含独立state-sink-failure fast0/1；完整SetCarry/SetOverflow/SaveFlags观察组合不能由当时AL/NV样本代替。这些配置与flags组合已由下文V5记录补齐，不再列为当前缺项。R2E未交付且无实验残留，不为可选实验补开关；残余state-sink的pending属于正常pass，与R2E分开。FEX/PMU门不由137项或RV37代签。

### 成本缺项的具体化

直接复用V6四份已有普通负载日志，不重复运行workspace-only测试：

| 负载 | requested峰值 | 保留峰值 |
| --- | ---: | ---: |
| Function CoreMark | 186852 B | 177788 B |
| Block CoreMark | 89612 B | 85896 B |
| Function bench | 62196 B | 61648 B |
| Block bench | 38488 B | 34688 B |

现有九缓冲统计已包括增长时旧/新容量重叠。每个idle workspace payload有6029312 B静态上界；active、嵌套/并发合计不受此值约束，退出为0不证明中途无保留。五组尚缺共存核算为IR/Params/HIR pool、残余pass、facts/allocation、目标准备输入与最终actions、多prepared region与发射/发表元数据；只在共同寿命检查点相加，避免重复计数或相加不同阶段峰值。

该阶段d0c40fd…实际ELF的公开TranslateIR到Finalize主体，独立Block/Function的存活固定帧前缀为17520/7744 B，已含Finalize自身帧；Block TranslateIR自身9968 B，Complete自身1664 B。前缀是完整最大栈的下界；尚未计入Finalize子调用、其他更大分支、上层decoder、递归深度及未解析间接/外部库边。数值作为V6历史对照保留，V5实测见后文；细目见 `r2-owned-params-terminal-v6-cost-closure-review.json`。

旧Function的codegen_complete比值1.129/1.032/1.137现已按原始12次日志和计时边界复核。四个互不重叠子桶declare/targets/perblock/verify的增量合计为28.316/6.111/29.672 ms，对应Complete增量28.127/6.900/29.856 ms；第一组主要落在verify_fixed与逐块准备，第三组主要在builder_dead。两组还有unit/IR数量差异，既有BC/CB/BC顺序也不足以分离次序、调度、布局与真实退化。定位到桶不等于证明因果或噪声，当前新增细分阶段不能回填旧差值。旧记录冻结保留，本阶段没有为追求某个比值追加历史采样；明细见 `r7-final-acceptance-v1-compile-cost-attribution-review.json`。

### 保留与后续

取回并校验raw、确认无消费者且十个完整ELF仍匹配current28后，手机临时十ELF及tar回收1314942976 B，独立du差额完全一致，259raw保留。固定缓存与current28/adjacent28/本批前合格参照未变，没有新增展开ELF bank。原系统盘277194584064 B历史释放只计一次，原临时入口仍在系统盘分配0、实际落Data2；本阶段手机回收单列，指定协调回执已更新。

本节保留V6阶段的接续边界；上述有限配置/flags缺项随后已补齐。五组私有容量、调用链及当前冻结候选六负载三方PMU的最新状态见下文。复用已合格FEX、guest、固定缓存和现有协议，不以旧候选216个主点/36冷服务点代替当前测量；原始a38c2db8 Block不合格仍保留，不能宣称最初版本至最终版本的累计收益。

## R7：单 owner 发射与 flags 观察（2026-10-26，block-emission-owner-v5）

最终479路径源码摘要为 `df9d69f748f25b69ed79775293e13f25b33057d60656a2902ccb8dcea6d249ff`，共享JIT/AOT生成语义版本42。两条Block TranslateIR（独立Block与HIRBlock）只保留一份optional BlockEmission；required-region分配失败、尚未发表时，按translator先于context的顺序销毁，再同槽重建并复用原prepared allocation/actions。owner自身不新增堆分配，不再同时保留原编码器与fallback编码器；Function分配策略不变。

永久flags oracle覆盖SetCarry/SetOverflow、SaveFlags全/部分观察，独立Block、单块Function、三块Function，flags0/1与优化0/1。每次完整IR调用新增111896断言，按已执行case及冻结循环核算6144次Run。V2真实发现非透明条件出口的PF token会在NZCV没有dirty位时丢失；最终terminal/region在flags-enabled非canonical出口先执行既有MergeNZCV、保持原dirty mask，再分支。cache42对旧41拒绝且安装单元为0。V2失败源码、ELF和raw保留，不作为最终通过。

| 当前资格 | 结果与边界 |
| --- | --- |
| V4原生计划 | 151项全部达到预期终态；IR fast/slow各120行、227490/227588断言、0失败。main为762行/506582断言，同一11失败分项/66失败断言正文与V6完全一致，成功断言增308尚未归因；79条固定记录按非计时字段一致 |
| V4遗漏与V5修正 | V4计划漏排cycles，追加实跑exit1；根因是HIR夹具只有Function编号，未填写独立Block的max_instr_id。V5只修测试：补ReIdInstr并验证已有ID/HIRValue/HIRUse节点不变，独立target/source按生产用法Push到Module，HIR源仍走内部detach/publish；聚合断言拆开且原强约束保留 |
| V5最终原生 | 仅重建shutdown目标，9.2775 s、exit0；owners/resource-cycles/cycles三项新执行全0，cycles为147检查，含两种Block API正常/首次region拒绝的4程序、8次Run。其余13个完整ELF与V4逐字相同，精确复用149项，合计152个不同资格项（151个exit0、main exit66） |
| 本批配置补齐 | SVM_RA_1BLK=0、SVM_BOUND_DECODE=0均实际执行；state-sink-failure fast0/1各1690946检查、33次注入失败、exit0；不再作为当前缺项 |
| RV84 | 实际V3代码十四目标构建74.967 s；19项用户态新执行124.411 s通过，13项用户态/5项真实模拟Linux guest内核按完整相同产物精确复用；强TSO另新执行220321检查通过。V3至V5只有两份未进入RV依赖图的测试差异。资格范围37项，全仓注册254项，不能写成全仓全跑或物理RV硬件证明 |

独立资格汇总为 `r7-block-emission-owner-v5-runtime/current-qualification-independent.json`；V4原始资格、补跑cycles失败及V5三项新执行分别保留。表中精确复用依据完整ELF、实际命令/配置和成功来源，不是仅凭生产源码相同。

最终成本从launcher `485f040c…` 与shutdown `2640f6b9…` 两个实际ELF核对：

| TranslateIR入口 | 自身固定帧 | 含Finalize自身帧的同链主体前缀 |
| --- | ---: | ---: |
| 独立Block | 5264 B | 12816 B |
| HIRBlock | 5104 B | 12656 B |
| Function | 144 B | 7744 B |
| Function span | 176 B | 7888 B |

独立Block自身帧及前缀相对V6均少4704 B；四入口自身帧与V2保持。最终launcher `.text` 为4506892 B，比V6少144 B，包含owner、flags与cache变更，不全部归因于owner。SinkStateStores/FinalizeStateMappings/CollectDead/PreparePinnedMemoryAddresses/GPRCopyCoalescing五个旧子图源文件未变、当前根帧一致，沿用原外部库、诊断与配置首次初始化切分范围内的1536/1312/1056/1040/960 B；尾调用按已恢复SP处理。这些有界结果和明确链前缀不是完整最大栈，证据见 `r7-block-emission-owner-v5-final-native-cost.json` 与同名 `.asm`。

Block CoreMark profile相对V6净增9单元/1368 B，已全部核算到浮点输出格式化的PC集合变化；公共PC的host大小一致，捕获字节除七处CPUID helper目标地址立即数外相同。没有逐PC IR计数或分支参数记录，不能推出IR相同、把具体格式化分支分给某一次printf，或解释墙钟因果；短CoreMark样本仍不具备正式性能资格。明细为 `r7-block-emission-owner-v4-runtime/block-core-profile-attribution-independent.json`。

当前28产物中23个改变ELF归档为 `r7-block-emission-owner-v5-changed-elfs.tar.zst`，411399239 B，SHA256 `a50f95c1676d442cbdde6222bb4c363d0a4eae8f73875ecde2baefded413d794`；另5个按既有完整归档精确复用。RV2557输入归档为11299152 B，SHA256 `bc65f05225c9ea6ca0c0975445c11ed4a8519a028e95a65cc508d1818e395f35`。索引分别为 `r7-block-emission-owner-v5-current-checkpoint.json` 与 `r7-rv64-execution-20261026-v84-compiled-source-checkpoint.json`，实际都在 `/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/`，不新增展开ELF bank。

本批当时剩余收口为堆工具v3正常退出、共同寿命容量/保留、限定调用链与六组PMU；v3真实输出正确但exit134的失败保留。后续工具V4合格、生产递归边界与当前候选状态见下文，不将上述历史待办继续滚动。R2E未交付且无残留，不新增实验义务；旧候选和合格相邻参照不能代替当前测量，也不能证明原始不合格Block起点的累计收益。

## R7：公共独立 audit 与最终验收（2026-10-26，public-audit-v2）

本批481路径源摘要为 `6049ce553edc88a518a1ca551f4d0ad8d3ec360b2a424a5457a5301a7b3ebb24`，cache42保持。公共 `compile/audit/verify.{h,cpp}` 与CMake已实际交付：IRReference从raw身份/编号/owner、完整参数及Params/Lambda重复边、唯一前向Goto binding、CFG/入口/外部入口/call-return支配建立独立参考，覆盖terminal与hoist隐式根；不借生产facts/SSA/证书结果。ARM64 audit消费只读参考并保留home/view/preservation/helper/clobber物理策略；compiler删除无消费者的PrepareResolvedUses循环。默认false的verify_plan仍是唯一生产接点，不新增普通路径参考扫描或持久owner。

| 当前V2证据 | 结果与限制 |
| --- | --- |
| 原生构建/执行 | V1首次测试编译因使用私有API失败，修正夹具后V2十四目标18.481 s构建成功；153项实际执行全部达到预期终态，含既有152项配置与1个新增API选集 |
| API与完整IR | API选集实际4 case/63检查；完整IR fast/slow各124行、227553/227651断言，0失败。首个IR union只选到API，不能冒称物理分配验证 |
| 真实审计接线 | fixedclass在swift_test另执行2行/39断言。10个真实负载均有plan_verify_calls>0，加metrics共11项audit-on，经过实际准备和发射；不是仅构造IRReference对象 |
| main与确定质量 | 762行/506484断言，既有11失败分项/66失败断言的完整正文与V5一致；总断言少98无法从现有聚合日志逐case复原，边界见下段。79条固定非_ns记录、五组bench确定质量字段保持，不推出全私有堆等价 |
| RV85 | 实际当前V2十四目标5.111 s构建成功；4个完整ELF改变，17项新执行120.332 s通过，15项用户态/5项真实guest内核及strongTSO220321检查精确复用；资格范围37/254 |

原生独立汇总为 `r7-public-audit-v2-runtime/qualification-independent-review.json`，313份raw的设备记录/本地/tar身份一致；RV见 `r7-rv64-execution-20261026-v85-test-summary.json`。源摘要、构建、完整ELF及实际配置均按现有JSON核对，不把“预期终态”改写成全仓零失败。

main成功断言差额的有限审阅为 `r7-public-audit-v2-main-assertion-delta-review.json`（SHA256 `01790cbf3d6d745bc31d55be0845145e80b9e2024278fd121e342097bdee90b2`）。V5复用的实际V4 main完整ELF为79d7a0ed…；两份JUnit的762条有序case在排除time后class/name/status及全部子节点逐项一致，11失败分项/66失败断言与stderr保持。Catch只写suite聚合成功计数，没有per-case totals或成功assertion记录。W81夹具在两版源码逐字相同，Android parent实际启动3个child，其成功断言数为12＋P，P是未记录的waitpid(WNOHANG)==0轮数；child各15/16/16断言保持，stdout只差三个RNG seed。确有非恒定总计来源，但无法据此把−98全部归给poll或seed；未补probe/log/测试或回跑。无新增失败的结论来自实际case/failure结果，成功总数的精确逐case分配继续列为观测限制。

成本由当前launcher `9bb79d52…`、shutdown `53c00d5d…` 核对。四入口自身固定帧按Block/HIRBlock/Function/span为5264/5104/144/176 B；Finalize自身帧分配前的调用者前缀为11136/10976/6064/6208 B，含其1680 B后的主体前缀为12816/12656/7744/7888 B，均与V5相同。旧报告“at Finalize entry”标签实际已含Finalize，现明确两种边界，不再重复加帧。当前 `.text` 为4526380/4410748 B，比V5增加19488/19448 B，Finalize code增加48 B；IRReference构造、BuildGraph、Verify与ARM audit自身帧为192/352/352/1232 B，两个352 B阶段顺序执行。新增audit关闭不等于静态零成本或默认机器码完全相同，尚不据此作PMU结论。证据为 `r7-public-audit-v2-final-native-cost.json` 与同名 `.asm`。

标准堆工具V4已合格，旧四流仍绑定block-emission-owner-v3 launcher `485f040c…`；其报告和工具v3的FORTIFY失败保留，不改贴当前版本。当前V2四流绑定实际 `9bb79d52…`、源 `6049ce55…`，每流均正常退出、输出正确、完整EOF c/R，0解析错、0重复live指针、0unknown free；178份相关/guard源码与冻结来源匹配。独立报告为 `r7-public-audit-v2-four-workload-requested-capacity-review.json`（SHA256 `827cc2bc2e271d8928b4faf94d43118916a22da84752c06ee485da47b3f58cce`）。

| 普通负载 | 整体public requested峰值 | 编译存储（含D/W与Inst backing）的同事件共同峰值 |
| --- | ---: | ---: |
| Function bench | 1312666 B | 1079132 B |
| Block bench | 540042 B | 242456 B |
| Function CoreMark | 4487920 B | 2271282 B |
| Block CoreMark | 3165539 B | 828495 B |

右列不是各家族独立峰值之和，也未把publication/cache输出P和运行时R算入编译存储；整体峰值当刻C去I＋D＋W、Inst I、P、R、未归属U逐项恰好等于整体分母，四流U均0。C1–C5＋W的较窄旧口径含I但不含decode D，不能简称完整中间流程。退出时动态HIR/Block/Function、各暂存与九workspace backing为0；Inst arena仍分别保留4/1/7/3个64 KiB根，每流另有一个64 KiB signal栈根，TLS存储另列。这是观察窗口内保留与源码寿命事实，不直接叫泄漏。verify_plan=0下audit allocation及trace frame均0；不声称开启审计也零成本。

当前四流在整体及编译共同峰值时均有4 MiB trampoline＋32 MiB模块arena：36 MiB logical backing extent、72 MiB共享RW/RX视图；240 B MemMap/Impl头已计入P，不重复加。dlmalloc不额外增长backing，正常checked shutdown支持退出后零逻辑arena/view保留。以上不是RSS、usable-size/碎片或munmap syscall实测；collector自身递归分配被抑制，OS物理域、guest映射和固定帧仍分开。映射边界见 `r7-final-cost-v1-mapped-backing-boundary-review.json` 及当前四流报告。

当前V2限定域栈报告为 `r7-public-audit-v2-internal-stack-closure.json`（SHA256 `256339ee39778808a57e2ff4dca5b3d2fcc58b3d0a3a3790a7522c7b9f69050b`）。正常整数value分析18个内部节点闭合，无正常PLT或间接callback；H_value为GetArg<Operand>112＋PublicIndex96＝208 B。共享budget32下，Q(0)≤144、Q(b)≤736＋max(208,Q(b−1))，含Analyze自身80 B得到23840 B保守上界；旧23776＋208＝23984 B仍是更松界。Definition在尾调Shift前恢复592 B，预算耗尽Query也先恢复再尾调，未把已退出帧相加。正常断言prologue仍计入，失败格式化/unwind/信号不在该正常内部域；不称该数是必然可达或全流水线最大值。

生产terminal构造D≤2、节点≤3，guest CFG深度不乘到编译递归深度。正常SVM/VIXL内部、具体MacroAssembler虚调用、If lambda及literal/veneer池已闭合；402节点只存一份压缩bodygraph（SHA256 `68aa7bcc7cf73f19ab5f2ffcc214a22935f28c2efd7f361032f189aff7d1428f`），不在各报告重复嵌入。有效区间n≤PTRDIFF_MAX给introsort预算k≤124；BranchInfo递归为176×k＋320≤22144 B，加880 B外层terminal与2912 B实际helper前缀，从EmitTerminal入口得25936 B保守内部上界。池和label循环不逐元素叠帧，已恢复的尾调用帧不重复计入；该最大源域形状不被称为本批实测峰值。

| 发射入口（两份实际ELF） | EmitTerminal wrapper分配前的P_emit | 加完整正常内部界25936 B |
| --- | ---: | ---: |
| Block launcher9bb | 6992 B | 32928 B |
| HIRBlock shutdown53c | 6832 B | 32768 B |
| Function launcher9bb | 4016 B | 29952 B |
| Function span launcher9bb | 4048 B | 29984 B |

完整关系为P_emit＋max(25936,max_j(P_lib,j＋H_lib,j))；P_lib已含相应terminal/dispatcher/具体helper，不能再叠一遍，也不把所有库/阶段峰值相加。launcher的TLS/realloc/memmove入口前缀为4000/3968/3952 B，new/memcpy/delete为3824 B，lock/condwait/unlock为1904 B，ldexp为1696 B；报告逐条列其余库和实际调用边。已链接new/delete包装的正常帧另有死用报告证明，库项从具名入口开始，不是整个SVM/VIXL黑箱。53c按实际testNew/TLS重算，最大普通libc前缀4064 B，once callback/key-create已知内部4080 B还需加仍存活的外部once/key-create栈；free为3824 B。shared cone的324个正常调用骨架与4个明确域叶核验一致，测试allocator差异未被当作同一ELF。

终版为 `r7-public-audit-v2-emission-stack-closure.json`（SHA256 `92fad4616f338a26d76b5aa979741c0344e74e7845f48e6404af856878e8c157`）。已初始化config、关闭普通diagnostics/audit/density/timing、有效prepared IR、具体MacroAssembler及active producer/module根是正常域条件；zero-ref销毁、并发Shutdown、失败/unwind/信号和任意手写嵌套terminal另域。库callee绝对栈和更上层host/decoder未被写零；这里交付的是明确入口/输入的完整内部关系，不冒充全进程数值最大值，Finalize也已在发射前退出。

死用证明报告为 `r7-public-audit-v2-ProveDeadUses-stack-closure.json`（SHA256 `888f097627726ac90596ff45b77d2ad5de230f155823059f51d66fe745d609a3`），绑定当前9bb79…，17份相关源与overlay/基HEAD一致。所有正常内部调用已核对：递归自身160 B，额外H_dead最大352 B＝RowAt112＋RowAssign64＋RecordAllocate64＋Perf2Enabled32＋已初始化GetSvmConfig80；Demand/Covered/InstructionPosition等在正文内联。配置已在前序生产入口初始化，首次workspace guard仍单独保留。库边界不是零：H＝max(352,208＋M,176＋F,320＋G,272＋E)，M/F/G/E分别为malloc/free/guard-libc/atexit入口以后的外部栈；linked new、delete、guard及PLT之前的内部帧均已计入。

| 死用证明入口基准 | Block | HIRBlock | Function | Function span |
| --- | ---: | ---: | ---: | ---: |
| TranslateIR入口至首次递归前P_dead | 11952 B | 11792 B | 6880 B | 7024 B |
| 生产x86 frontend入口至首次递归前P_dead | 14560 B | 此API不在该launcher链上 | 8096 B | 8240 B |

第四API以实际shutdown53c00…单独核对，补充为 `r7-public-audit-v2-ProveDeadUses-HIRBlock-supplement.json`（SHA256 `cd5a9c9bf63eac9c008c00a7f8a0340b729ded185d958d0bd117cd7e8ac8db71`）。21个共享helper的size、SP机器字/相对位置和branch/call骨架一致。测试全局new48→emutls64及首次once callback16的内部最大额外子链304 B，仍不超过共享H=352 B；但正常malloc/TLS-libc前缀为224/288 B，首次callback及其外库链另列，不能复制launcher的208 B切口。该API前缀11792 B已含自身5104 B，首层递归加入后为11952 B；与独立Block首次递归前恰好同数的11952 B是不同边界。

必要SSA在FromOptimized及TargetComplete入口均先于lower，封口roster不变；u16容量、invalid/越界/重复ID拒绝给出有效RPO块0≤N≤65535，不将未参与Define的unreachable成员或无效手写IR混入证明。N是本块analysis.Instructions().size()，没有采集本批逐query的实际N/深度，聚合ir_insts与guest decode cap不能代替。保守总式为P_dead＋160×(N＋1)＋352＋L，L＝max(M,F,G,E)；初始化只可能在递归root，同路径精化为P_dead＋max(512,160×(N＋1)＋112,368＋M,336＋F,480＋G,432＋E)。最大递归源域包络10485760 B不是实测峰值、预留量或适配OS线程栈的保证。此前host/JIT bridge另计，Finalize是另一阶段，不能相加为同时存活帧。

旧V5仅完成Core36主点＋6冷服务点：首点后maxfreq由2188800降至2054400 kHz，整组协议无效，后五组未启动；正确性、退出、身份与PMU raw保留，不用于有效性能结论。用户让手机保持空闲后，当前V2一次完成最终六组216主点、36冷服务点及6个独立服务寿命；每组550份raw，共3300份，身份、输出、目标/profiler退出及服务process/socket闭合均经独立核验。全部主点及跨组的fingerprint、governor、min/maxfreq、充电和USB条件一致：CPU4、walt、614400/1171200 kHz，未改设备设置。没有补点、替换慢点或复用旧无效样本。

每格为三轮配对斜率比的中位数，依次为instructions / cpu-cycles / wall；大于1表示候选成本较高。SVM合格参照为源c38a3903…、完整ELF25bf2d46…，当前候选为6049ce55…/9bb79d52…。FEX固定commit为a156752f…、Android补丁4ade908f…、Loader8632943c…；所有完整身份、三轮原值、range、MAD、两端点总量、截距、冷服务及服务寿命总量见独立汇总和人工阶段报告。

| 负载 | 候选/合格参照 Function | 候选/合格参照 Block | 候选/FEX Function | 候选/FEX Block |
| --- | --- | --- | --- | --- |
| CoreMark | 0.999936 / 0.999025 / 0.997632 | 1.000060 / 0.993433 / 0.994166 | 1.737505 / 1.967434 / 1.960721 | 2.214363 / 2.606003 / 2.605939 |
| int | 0.999277 / 0.991732 / 0.993571 | 1.000865 / 1.007569 / 1.010765 | 1.500002 / 1.012494 / 1.007513 | 2.096736 / 1.086096 / 1.086862 |
| fp | 0.999613 / 0.994687 / 0.994134 | 1.000201 / 0.997503 / 0.998543 | 3.439605 / 2.995210 / 3.000735 | 3.234680 / 2.805631 / 2.812826 |
| mem | 0.999860 / 1.007984 / 1.009367 | 0.999822 / 0.984280 / 0.983840 | 1.378356 / 1.796953 / 1.798942 | 1.925458 / 1.827955 / 1.827850 |
| branch | 1.000672 / 1.008495 / 0.994178 | 1.000026 / 1.010390 / 0.985488 | 1.455028 / 1.215946 / 1.224631 | 1.750160 / 1.432113 / 1.407381 |
| call | 0.999869 / 0.995320 / 0.999350 | 1.000076 / 0.989391 / 0.989907 | 1.797819 / 1.743158 / 1.742730 | 2.867181 / 3.808717 / 3.808074 |

12组模式/负载的36个配对instructions比值全部≤1.01，最高观察退化0.249746%；本轮指令质量门通过。79条固定非_ns记录和五组bench确定质量对照仍绑定V5；R4已签的同单元spill规则不由不同动态集合的聚合数重新证明，本批公共audit不改变分配策略。周期/耗时保留各自方向和波动：例如branch两种模式的范围均跨1，call Function周期范围0.935738–1.057307，不据中位数称稳定提速或统计显著。FEX差距保持原样，FP Function耗时约3.000735倍、call Block约3.808074倍，没有证明追平或全能力等价。

启动截距包含loader/dispatcher/runtime/compile等混合成本，负值和不可定义比值保留；每组warm FEXServer完整寿命及36个cold观察分别列总量，不按主点摊销。CoreMark短运行警告保留，不称官方分数。原a38c2db8 Block基线仍不合格，因此不宣称重构起点的累计收益。人工签收的是15.3/15.5所列本批质量与可复现外部参照，比较器的自动formal_FEX_gate_passed=false保持原始记录，未用它代签全目标。

当前28产物的18个改变ELF以 `r7-public-audit-v2-changed-elfs.tar.zst` 保留，403034997 B，SHA256 `400d7e6604f31a6a6d96a10d15d28f07c069df5f8a63be4fe30705ed5d70deec`；另10个按完整身份引用既有归档，全部成员已解压核验。实际RV2559输入归档为11303296 B，SHA256 `3ea4eae0d0548631006e771d3d3e6f1dd9d4c013c55cc7d3a3e90a19cd224453`；481路径代码overlay另保留1799949 B。索引为 `r7-public-audit-v2-current-checkpoint.json` 和 `r7-rv64-execution-20261026-v85-compiled-source-checkpoint.json`。原始编译输入快照不因后续文档更新而回写，最终两份文档另做摘要闭合；固定native/RV缓存、必要相邻参照及唯一raw保留，不新建展开产物bank。

独立汇总为 `r7-public-audit-v2-pmu-independent-summary.json`；root阶段报告为 `r7-public-audit-v2-final-pmu-stage-verdict.json`（SHA256 `0f3db642990a43488cdaec9d9dec4f6b66ea243bde83599730a8bdec53cb4357`）。PMU、当前四流成本、限定内部栈关系和清盘已完成；本轮最终人工签收及适用边界见下段。

清盘与测量均已实际收口：原系统盘results释放271829954560 B、两份固定缓存迁出释放5364629504 B，合计277194584064 B，只计一次；三个旧tmp入口均为Data2软链接且独立du为0。PMU结束后检查exe/cwd/fd及socket、核对恢复源和完整文件摘要，再删除共享4ELF与六份simpleperf，独立du释放272650240 B；3300份唯一raw及小配置保留。第一次逐fd检查超时发生在unlink前，精确身份确认后只停止自身遗留只读检查进程，失败记录和解决记录均保留，最终无未解决清理错误或本线设备运行者。本轮接续的手机回收累计6041136128 B，另有Data2旧堆工具62218240 B，均与系统盘历史释放分列；全局df变化不归因给本线。最新记录为 `r7-public-audit-v2-final-pmu-phone-reclamation.json` 与 `r7-public-audit-v2-post-pmu-storage-followup.json`，指定WinEmu协调回执已更新。后续仅主机离线栈分析及文档写入Data2；代码/Git/WIP、固定缓存及必要归档未动。

最终人工签收：本轮R0–R7核心结构、公共独立audit、现有有限正确性/质量、当前成本及指定红魔三方PMU按方案范围完成。最终记录为 `r7-public-audit-v2-final-stage-verdict.json`，独立完成条件审阅为 `r7-public-audit-v2-final-completion-offline-review.json`（SHA256 `9f0e1acf5fcd9f9ce4308ba1b1e16540235e10fb1c42517e58421c94317642ab`）。最后重新核对481代码、118退役路径、28个实际固定缓存ELF以及HEAD/index，均与冻结身份一致，无额外未冻结源改动；记录为 `r7-public-audit-v2-final-code-and-products-identity.json`（SHA256 `6fe3af602b229decfa1c97370d12f9adcdbda670ae064e7ffb27fca15c2ab767`）。两份最终文档另在 `r7-public-audit-v2-final-source-documentation-closure.json` 记录摘要，不回写编译输入快照。

签收保留全部实际限制：main仍为既有11失败/66失败断言，成功总计−98不可精确逐case恢复；RV仅37/254有限资格；三轮观察不证明统计显著总体收益；FEX能力/性能和原不合格Block起点累计收益不外推；旧Function计时因果未定；requested/mapped/stack各域不冒充RSS、usable/碎片或全进程/任意IR最大值。这些范围不被改写成通过，也不从当前目标自动扩成新一轮优化/探针/采样。唯一raw、代码/Git/WIP及恢复归档保留，设备测量和本线VM已关闭；没有待完成的本轮生产重构或必需补测项。
