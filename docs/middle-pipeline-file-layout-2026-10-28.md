# 中间编译流程：最终目录、逐文件职责与退役映射

**当前状态（2026-11-02）：8cda已合A，最少检查仍有一个新增fixture失败；02测试修正未应用，当前版本没有新的ordinary计时。** 最新普通方向仍属e0d6，最终两倍目标未达。完整source、构建、原例、归因及ordinary资格统一见[主方案7.1](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)，本文末段只列本轮身份边界。

本轮按[净工作与路线修正](spill-and-compile-root-cause-plan-2026-10-28.md#compile-net-work-correction)及[审查42](compile-gap-review-2026-10-31.md#performance-route-correction-latest)修订。同Store是当前选用的实现，正确性合同、真实旧工作退出和完整反馈决定是否采用，不能由载体、文件数或少表推出性能结论。

本文给出当前选用路线的目标目录、逐源职责和旧工作去向，**不是当前目录清单，不表示迁移已经完成**。文件随真实算法收口；文件数不等于 pass 数，同一决定不能在 inputs→preparation→actions→certificates 间反复重述。

第76批before为f7e6/d391生产基础及原scratch fixture修正。紧凑移位78f完成必要语义和唯一完整方向后，无大幅收益，29文件整包撤回；新增shift.h/opcode/profile不属于当前或目标树，历史落位只见6.1。Facts/target共访亦已撤回。第78批实际位置包before为a10，整包25文件现已撤回，历史证据见第4节，现行资格统一见[主方案2.1](spill-and-compile-root-cause-plan-2026-10-28.md#21-当前源码开发计时和最终资格分别记录)，不重复转移候选结果。

**当前结构沿[主方案5.3.11](spill-and-compile-root-cause-plan-2026-10-28.md#5311-下一完整替代同一store中的后端虚拟与物理程序)的同Store实现组织：Semantic、BackendVirtual、PhysicalReady顺序接管同一程序。** actual直产、旧native入口退役和最终位置RCpc各沿原文件收口；public/retained/Unknown及消费前source miss保合法语义分支。同Store并非唯一减工前提；单consumer frame、稀疏选择或关系索引若能净退旧工作，也按真实合同评估，不能仅因仍读Inst就断言完整Describe/Complete必然存在。不得为目录统一另建preparation层，现行资格只见主方案7.1。

## 1. 文件组织的约束

| 交接 | 唯一权威 | 必须避免 |
| --- | --- | --- |
| frontend→builder | guest行为、公共typed语义、现纯能力与原提交边界 | ARM64 NativeOpcode/Form/home/VIXL进入frontend；decode直接产NativeInst |
| builder→residual→最终SSA | Inst/Arg、唯一uses/order、当前epoch Facts | 第二用途图、birth标志豁免深分析、SSA Finish未结束就销毁定义 |
| semantic→BackendVirtual | 原IRStore同槽、原NodeId/order；原stream/B元数据 | dense Selection与原Inst并存；转换后仍回查semantic API |
| virtual→两scope RA | 中立AllocationProgramView借原程序/CFG；原Allocation/Writer | 旧semantic ends后补机器form；复制第二allocator或N/E输入程序 |
| RA→state/核验 | 原Writer成功结果、同源实际effects；原state/独立checker | 早写只晚改Map、candidate自证、entry-width乐观union |
| PhysicalReady→kernel/重发 | 原程序中的稳定operand和真实transport资源 | 每次重发再binding、完整wire副本、kernel回producer/Map |
| 发布/结束 | 原Prepared/ExecutionAccess与持久code object | 另立生命周期manager；移动timer/最后释放换统计 |

文件按维护职责划分，不要求每文件一遍全N或一份结果。公共allocator只认识机器读写、值类别与fixed/tie/clobber/scratch等中立约束；ARM64 form、state、选择和编码归后端。Block与Function共享适用的assignment，保不同live/CFG范围。

独立核验保真实结果、完整scope及错误分类，不固定逐query重找全scope的算法。必要关系索引可以由现有owner唯一生产，供真实consumer共读；须同时退出被替代的发现/重复owned行，说明raw与actual域、编辑/恢复失效和最后读者，并计初始化、容量、访问与结束。禁止重复影子或Passed自证；也不能仅因增加一份索引便否决净减工。

原PhysicalSelection只保unit/B信息及真实稀疏候选；成功recipe移入同槽本体后结束。新增`native.*`不是另一个IR owner；`allocate/program.*`是借用协议，不存第二图。冷/public适配继续共享family规则，不成为normal全旧链回退。

## 2. 最终目录总表

`name.{h,cpp}`代表两个文件。树覆盖中间流程；既有decoder/emitter逐指令文件、通用runtime和ISA辅助类型继续保留。以下给出维护上的落位，不要求为实现新算法先搬全树，也不为每个职责强制新建文件。目标新名称在迁移前不伪装成当前文件；当前→目标去向见第7节。

```text
source/runtime/
├── ir/
│   ├── ir.inc / ir_meta.{h,cpp} / ir_types.h / opcodes.h
│   ├── instr.{h,cpp} / args.{h,cpp}
│   ├── store.{h,cpp} / order.{h,cpp} / uses.{h,cpp}
│   ├── block.{h,cpp} / function.{h,cpp}
│   ├── function_ir.{h,cpp} / hir_builder.{h,cpp}
│   ├── hir_storage.{h,cpp} / hir_pools.h
│   ├── terminal.h / function_entry_provenance.h
│   ├── location.{h,cpp} / host_reg.h
│   └── atomic_rmw.h / load_extend.h / low32_copy.h
├── frontend/
│   ├── x86/
│   │   ├── guest_layout.{h,cpp}
│   │   ├── decoder.{h,cc} / scalar_demand.{h,cpp}
│   │   └── decoder_*.cc / vex_decoder.* / x87.* / helpers/
│   └── arm64/arm64_frontend.*
├── compile/
│   ├── target.h / guest_state.h / guest_bindings.{h,cpp}
│   ├── unit.{h,cpp} / optimized_unit.{h,cpp}
│   ├── pipeline.{h,cpp} / workspace.{h,cpp}
│   ├── builder/
│   │   ├── ir_builder.{h,cpp}
│   │   └── instruction.cpp / guest_state.cpp
│   ├── rules/value_rules.{h,cpp}
│   ├── passes/
│   │   ├── passes.h / state_internal.h
│   │   ├── value_worklist.{h,cpp}
│   │   ├── values.cpp / widths.cpp / dce.cpp / flags.cpp
│   │   ├── state.cpp / state_dse.cpp / state_sink.cpp / state_map.cpp
│   │   └── loop_hoist.{h,cpp}
│   ├── analysis/
│   │   ├── semantic_facts.{h,cpp} / block_index.h / block_summary.h
│   │   ├── roots.{h,cpp} / consumer_roles.h
│   │   ├── cfg.{h,cpp} / ssa.{h,cpp} / liveness.{h,cpp}
│   │   ├── value_flow.{h,cpp} / value_bits.{h,cpp}
│   │   ├── effects.{h,cpp} / register_reads.{h,cpp}
│   │   ├── pseudo_flags.{h,cpp} / flag_flow.{h,cpp}
│   │   └── guest_state.{h,cpp} / guest_state_cfg.cpp
│   ├── allocate/
│   │   ├── program.{h,cpp}                 中立程序/CFG借用与受检适配
│   │   ├── allocator.{h,cpp} / scan_internal.h
│   │   ├── schedule_block.cpp / schedule_function.cpp
│   │   ├── assign.cpp / assign_gpr.cpp / assign_fpr.cpp / assign_fixed.cpp
│   │   ├── coalesce_gpr.cpp / coalesce_width.cpp
│   │   ├── allocation.{h,cpp} / locations.cpp
│   │   ├── writer.{h,cpp} / writer_locations.cpp / location_indexes.{h,cpp}
│   │   ├── spill.cpp / reloads.cpp / repair.{h,cpp} / scratch.{h,cpp}
│   │   └── inputs.{h,cpp} / contracts.{h,cpp} / verify.cpp
│   └── audit/verify.{h,cpp}
└── backend/
    ├── compile_binding.{h,cpp}
    ├── guest_helpers.{h,cpp} / system_helpers.{h,cpp}
    ├── arm64/
    │   ├── host_features.{h,cpp}
    │   ├── compile/
    │   │   ├── compiler.{h,cpp} / compiler_internal.h
    │   │   ├── target.{h,cpp}
    │   │   ├── native.{h,cpp}                  同Store的virtual/physical对象
    │   │   ├── constraints.{h,cpp} / constraints_internal.h
    │   │   ├── constraints_scratch.cpp        同源rich scratch规则，无独立pass
    │   │   ├── state.{h,cpp} / state_widths.cpp / state_publication.cpp
    │   │   ├── select.h / select_scalar.cpp / select_flags.cpp
    │   │   ├── select_integer_flags.cpp / select_memory.cpp
    │   │   ├── select_vector.cpp / select_control.cpp
    │   │   ├── flags_graph.h / flags_native_graph.h    纯图规则与借用适配
    │   │   ├── physical.{h,cpp}
    │   │   ├── verify.{h,cpp} / verify_gpr.cpp / verify_fpr.cpp
    │   │   └── verify_gpr_core.h / verify_gpr_native.h  私有核验读口
    │   ├── jit/       原实际编码、同源kernel、code object emitter
    │   └── helpers/   宿主实现和ABI
    ├── interp/        公共语义消费者
    └── riscv64/       自身目标能力、分配、恢复与编码
```

`function_ir.*`、`semantic_facts.cpp`、`roots.*`和allocator新文件名都是目标组织，当前未迁。公共`pipeline.cpp`调度语义；ARM64目标调度归`compiler.cpp`，不新增目标pipeline或`select.cpp`。`optimized_unit.*`有真实seal/Facts边界，不能因其交接已为move而继续造空壳重构。

3ef的typed kernel、原ctx重建重发与最后退役机制继续复用；支持域默认由同Store PhysicalReady承担重发输入，source miss/retained保语义分支。actual输入/RA/state/资源已沿原owner接管，完整负载native覆盖率及净时间尚未建立，不能把转换格式当收益。第79批新增的`native.*`和`allocate/program.*`已经存在，其余目标拆分不随之视为已完成。

## 3. IR 与公共编译层逐文件职责

### 3.1 IR：语义、编译 CFG owner 与持久 owner

| 文件，相对 `runtime/ir/` | 最终负责工作 |
| --- | --- |
| `ir.inc`、`ir_meta.h/.cpp` | 唯一opcode/schema、签名、结果/effects；必要时驱动普通公共形态的同源字段、构造/编辑/枚举合同，不另建forms registry，physical form不定义guest语义 |
| `ir_types.h`、`opcodes.h`、`host_reg.h` | 公共枚举和小类型，无目标编译策略 |
| `instr.h/.cpp` | typed结果/flags、受控字段编辑、BirthPayload与计数/失败原子性；公共表示候选须整链替代旧packet解释，不能只加fast getter；保最后typed清理 |
| `args.h/.cpp` | per-use type-view、完整Imm/EA、Params/Lambda/foreign的真实escape与资源；复杂参数按实际长度处理，不为定长form截断范围 |
| `store.h/.cpp` | managed槽、NodeId/session、粗phase和112B backend body；Virtual/Ready复用原alive位表达真实backend membership。提供cursor转交/退役原语，页目录容量门保留；不解释ARM64对象/析构、opcode或复制CFG，native committed prefix及资源析构归原stream |
| `order.h/.cpp` | 插入/移动、实际程序次序、唯一cold链；不以NodeId猜执行距离 |
| `uses.h/.cpp` | Block/Function唯一occurrence/反向关系、编辑通知和关闭图；不等同于persistent Function专属图 |
| `block.h/.cpp`、`function.h/.cpp` | 持久地址范围、真实parent、锁、发布/缓存/retirement；独立Block另持自身InstructionOwner/InstList、按需block_uses和IRStore lease，消费时只结束其语义部分；Function的可结束编译CFG单列 |
| `function_ir.h/.cpp` | 从当前hir_builder拆出的真实HIRFunction/HIRBlock/Edge、CFG/RPO、entry/provenance、唯一FunctionUses/EditBinding与Store lease；不复制第二份图，不为独立Block再包一层Function |
| `hir_builder.h/.cpp` | 结构化构造、建边、current-block、call-return和guest收尾API；借用真实编译CFG owner |
| `hir_storage.h/.cpp`、`hir_pools.h` | 既有CFG pool/session与HIRPoolLease，保重入、多Prepared/跨线程真实归还；不强合IRStore成统一arena |
| `terminal.h`、`function_entry_provenance.h` | counted terminal/implicit root及canonical entry/provenance语义 |
| `location.h/.cpp` | 公共地址身份，不决定物理位置 |
| `atomic_rmw.h`、`load_extend.h`、`low32_copy.h` | 原atomic、access/result/extend和低32位只读语义 |

当前`hir_builder.h`实际拥有Store lease、FunctionUses和EditBinding；拆owner是职责收敛，不新增lifecycle框架或公共IR。Store负责内存/phase，backend结果owner负责physical overflow/资源；semantic alive结束与payload真正回收分别记账。

### 3.2 绑定、请求和语义提交

| 文件，相对 `runtime/compile/` | 最终负责工作 |
| --- | --- |
| `target.h` | SemanticContext、CompileEnvironment、TargetBuildView、窄纯能力；Known/EarlyOff/Unknown，无RA或逐节点证书 |
| `guest_state.h` | 逻辑byte layout、别名、partial/high32 |
| `guest_bindings.h/.cpp` | backend一次装配logical→host bindings及粗保活；Unknown不虚构物理事实 |
| `unit.h/.cpp` | 本次IR/session、实际group/parent、环境/lease；借用ExecutionAccess，真实guards归原Access |
| `optimized_unit.h/.cpp` | 最终seal及完整semantic SSA Finish；第79批已增加TakeIRWithCFG，唯一移交原FunctionCFG并结束Facts。全组其余semantic读者仍须由原调度闭合，不为旧Inst读口延寿，不把移动CFG当全部读者已迁 |
| `pipeline.h/.cpp` | Block/Function residual、反馈、hoist、最终编号/seal与原Facts交接；不负责ARM64输入/位置修正，不另建目标pipeline或defer相序旁路 |
| `workspace.h/.cpp` | 原队列/游标/容量、重入和retained上界；区分容量与实际初始化/触达，无需求困难状态不建立，不存第二identity/uses |
| `builder/ir_builder.h/.cpp` | 可覆盖族append前纯选择与一次提交；列真实少出生节点/边及后续服务。公共字段表示若替换，连写口/编辑/用途/分析一起迁；AfterAppend单列 |
| `builder/instruction.cpp` | guest输出/FlagsIntent、observer/原提交边界和收尾；不是append后通用回滚 |
| `builder/guest_state.cpp` | 同span owner的byte/full/partial forwarding和已有deferred，观察前真实物化 |
| `rules/value_rules.h/.cpp` | builder/residual共享的纯value/width/immediate/predicate规则，接受现有facts，不重走同一祖先 |

### 3.3 必要 pass 与按需分析

| 文件，相对 `runtime/compile/` | 最终负责工作 |
| --- | --- |
| `passes/passes.h`、`passes/state_internal.h` | rewrite入口、真实编辑/出生/归属/root通知、私有state声明，无第二owner |
| `passes/value_worklist.h/.cpp` | 固定epoch roster、顺序/CSE/替换/退役、深整数需求闭包、dirty和反馈；不自动接纳后来birth |
| `passes/values.cpp` | 用已准备facts执行实际value改写 |
| `passes/widths.cpp` | per-use type-view、load/extend单用/邻接/观察合同 |
| `passes/dce.cpp` | 同uses/roots的DCE；M1条件性配套为独立Block首次seed后的释放事件队列，覆盖raw/member、出生/root/直接UnUse；Function既有队列复用，不把小项当构造主线 |
| `passes/flags.cpp` | flags清理与原branch-only首轮/优先级；M1后续处理相关IR/uses/root/顺序dirty块与live_out变化块的并集，跨块计数通知producer所属块；仍与DCE/value闭合 |
| `passes/state.cpp` | 原cleanup/sink/map依赖调度；不增加同义ready层 |
| `passes/state_dse.cpp`、`state_sink.cpp`、`state_map.cpp` | 分别承担byte-exact DSE、observer sinking、合法logical→bound映射；真实算法不盲合 |
| `passes/loop_hoist.h/.cpp` | 原CFG/entry/observer下的提升、必要新epoch/反馈 |
| `analysis/semantic_facts.h/.cpp` | 同sealed输入的共享事实、已有融合SSA/summary/index/live与缓存，承接当前facts.h/.cpp。独立target共访接口已撤回；公共层不含ARM64、第二decoded表或逐node资格 |
| `analysis/block_index.h`、`block_summary.h` | 最终顺序、摘要与consumer索引，复用原uses |
| `analysis/roots.h/.cpp` | 承接当前analysis/uses.h和terminal_uses的roots遍历/membership；保counted与implicit、raw与resolved不同接口 |
| `analysis/consumer_roles.h` | 实际consumer角色分类，不维护另一份边 |
| `analysis/cfg.h/.cpp`、`ssa.h/.cpp`、`liveness.h/.cpp` | 完整semantic SSA在转换前完成；原CFG继续供Function机器live。机器use/def改变则原allocation域维护/重建，不能借旧semantic ends冒充；Block不预付Function固定点 |
| `analysis/value_bits.h/.cpp` | 纯整数transfer、numeric/raw投影、target carrier，manual/raw有界查询 |
| `analysis/value_flow.h/.cpp` | 真实值关系和CFGUseEnds等消费者，不机械并入bits |
| `analysis/effects.h/.cpp`、`register_reads.h/.cpp` | 实际effects/observer与guest读集合 |
| `analysis/pseudo_flags.h/.cpp` | direct/legacy关系及实际FlagsDemand，供最终form冻结 |
| `analysis/flag_flow.h/.cpp` | flags自身的label/terminal/CFG需求和transfer；M1首轮完整求解，后续dirty前驱闭包从bottom重求最小固定点，处理下降及未知关系；不套value-use图 |
| `analysis/guest_state.h/.cpp`、`guest_state_cfg.cpp` | span/snapshot/meet、版本/别名与CFG状态；不是SSA物理Map |
| `audit/verify.h/.cpp` | 独立语义/分析参考，不能用生产facts自证 |

首次roster、深分析、未来uses、state/flags/hoist都有真实工作。改名不删除这些计算；已落地IntegerFacts仅替代相应深解释，不代表全部pass冗余。

## 4. 公共分配器：六项服务与逐文件职责

两个scope向共同assignment提供各自真实可用范围。Block的局部kill/affinity与Function的CFG/live不是同一算法，不规定必须包装成相同interval对象。下表是源文件职责目标，不增加SchedulePlan/ReadyInterval/Certificate；不以必须拆成约21个cpp为交付条件，体量不值得独立时保留同职责源文件。

| 服务 | 目标文件，相对 `compile/allocate/` | 每个文件负责的工作 |
| --- | --- | --- |
| 程序访问 | `program.h/.cpp` | 中立借用协议、实际defs/uses/REF与原CFG；一份ProgramLiveness给初始描述/质量政策/全部不变attempt借用，区分global/local端点、完整occurrence与跨块标记。N规模状态/传播计费；Matches不检测编辑版本，改变机器合同须显式维护/重建，不拥有第二N/E图 |
| 请求/重试 | `allocator.h/.cpp` | 两范围入口、普通成功与原有限retry；InvalidProgram立即退出、真scratch短缺才修复/升reserve、最终FPR复证才关闭coalescing重试。checked/native政策边界明确；借program/live至Finish，不存第二结果 |
| 私有声明 | `scan_internal.h` | 当前scan、active槽、attempt与共用函数声明，不成为对外preparation |
| Block调度 | `schedule_block.cpp` | 选中machine uses/defs及必要implicit/terminal上的局部kill/use-end/affinity；稳定合同先于首live，Map相关变更维护受影响范围，正向分配并交付actual effects。保public/preseed/压力/未来fixed读；不是复活只换收集的definition stream |
| Function调度 | `schedule_function.cpp` | 在原CFG上由实际machine uses/defs取得跨块live、entry/join/loop与单块Function范围；复用原范围数据，不复制第二CFG，不沿旧generic ends后补选择 |
| 共用assignment | `assign.cpp` | 当前值分配、expiry、active回收、gap dirty masks；不内联所有寄存器类算法 |
| GPR/FPR/fixed | `assign_gpr.cpp`、`assign_fpr.cpp`、`assign_fixed.cpp` | 真实birth与逻辑ID分开；初始预着色/fixed/REF优先级、GPR/tie/ABI、完整FPR resident/destructive/scalar/AES/merge/future-read及post-publication，保原不同端点和跨块live-in。真实writes由同Writer交付，不再给晚阶段重投影 |
| 质量策略 | `coalesce_gpr.cpp`、`coalesce_width.cpp` | guest read/write/copy与宽度/component/low32/preserve；依赖齐备便在原Writer完成，不强制保留独立晚选择遍历。映射原优先级与最终good encoding，允许替换实现方式 |
| 唯一结果 | `allocation.h/.cpp` | owning tables、只读view、请求结束/有界复用；actual selected/cold uses按其原权威合同保存，不重新物化inferred/原用途；已撤分组存储不列默认实现；不再同时承担公开调度实现 |
| 只读查询 | `locations.cpp` | Map/REF及actual location/copy窗口；借当前完整actual输入与纯target读取规则，保selected存在优先、exact-width、publication/最新view/LowView及最终preserved过滤；不解引用native身份为旧Inst；唯一必要索引须替代原查询，按实际失效和结束计费 |
| 唯一写口 | `writer.h/.cpp`、`writer_locations.cpp` | Map/slot/dirty/reset及fixed/preserved/publication/view/epoch；copy安装一次保原行稳定identity/id/type/B窗口，核actual program/site，不让后续查询回旧对象。原journal受控提交，容量先备/回滚不分配，不加第二mutation owner |
| 必要索引 | `location_indexes.h/.cpp` | 现有home_index的fixed声明区间、preservation_index的preservation冲突按实际职责收敛；不预置已撤回的placement bitmap/REF目录。索引是否必要由下一主要服务算法决定，不复制Map、不union关系、不建第二真值 |
| spill/reload | `spill.cpp`、`reloads.cpp` | 前者victim/cost/slot/forced-spill；后者backing、reload注册/查询及故障/forwarding合同，保完整ID/flags与多home语义。Allocation按原最后读者寿命保实际payload；具体存储由下一主要算法决定，已撤grouped region不作前置 |
| 局部修复 | `repair.h/.cpp` | 原Writer上的segment repair/recolor、真实触及数据及对应writes/transport失效；Function保跨块依赖，失败是否需全scope重试明确，不用额外spill换少重试 |
| scratch | `scratch.h/.cpp` | 同ScratchSite/Profile实际需求、terminal reload/短缺/重试；不是最终fixed verifier |
| 输入 | `inputs.h/.cpp` | 同源form的真实稀疏fixed/tie/scratch/ABI/pool约束、按class/home/实际点排序的guest-home writes及原请求质量选项；跨不变attempt复用。常规operand读原程序，不含实际Map镜像；raw ABI clobber不是guest-home write |
| 合同 | `contracts.h/.cpp` | 公共fixed/ABI/区间原语和安装；保typed/fixed合同，首查询前完成批次 |
| 核验 | `verify.cpp` | 原fixed核验内核，默认attempt与私有Complete责任明确；消费成功选择给出的实际结果替换目标，保独立clobber/writeback；不是validated缓存或独立audit替身 |

原支持域默认program路径沿用原LinearScan/controller与完整质量算法；保留语义分支仍走其合法入口，没有第二套生产allocator。缺失或覆盖/顺序/端点非法的scratch表在改Map前拒绝，已提交且仍依赖语义的政策输入返回SemanticPolicyRequired。目标粗准入仍须识别未提交的width/flags/observer等需求，并证明真实scratch预算；不补默认零预算。M2/M3仍须跨Describe、位置修改、架构状态和交付替代；保fixed/REF/FPR、pressure、spill/repair与受界retry，生产入口粗准入时选支持路径，不先付完整新旧两链。

第80批已在现`program.h/.cpp`、`spill.cpp`、`internal.h`、`linear_scan.cpp`接原GPR压力读者：借actual birth-site/traits及本点inputs，保原成本、next-use、Map/dirty和预算，不新增schedule/pressure manager。ARM64 `native.cpp`负责同程序桥接，`select_scalar.cpp`与旧consumer solver共用同一pair准入。完整fixed/width/FPR、原controller/repair和normal入口仍待迁，目标文件拆分没有因此自动完成。

C3/6f7b源码已接actual birth、共享machine live、初始fixed/FPR/GPR/width、结束coalescing及原controller/repair；四目标和所述最少检查已过。这些读口迁移不证明成功质量决定已兑现成最终machine form/state，也不表示normal旧链已退出。native publication接缝目前最多有`P_pub×D`次definition访问（P_pub为publication数、D为实际definition数），Map/REF解析另计；future-fixed-read循环及二分按每个相关tie候选计，不能当全scope只付一次。均为仍需核对的真实成本；唯一必要索引须替代原发现工作并交净账，不能掩盖重复。完整边界见[主方案5.3.12](spill-and-compile-root-cause-plan-2026-10-28.md#5312-下一相依组先闭合机器合同再替换默认链)。

第78批25文件实际位置候选必要语义通过，唯一方向无大幅收益后撤回；placement_index.h/.cpp已删除，Writer/state/late消费者当时恢复a10。其源码职责与失败证据仅保存在[完整补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/owner-range-index/whole-package-current/full-from-a10.patch)，不作为目标文件或下一实现前置。第79批从该恢复版继续实现，已不再生产源码逐字等a10；历史停止结论见[主方案5.3.9](spill-and-compile-root-cause-plan-2026-10-28.md#539-实施中实际位置生产与消费者复用)。

Block局部查询不枚举其它Function块成员；Function publication对真实live-in/foreign producer的全scope义务另计。REF保真实跨块根、fixed/numeric分开和prospective关系不混用，仍是正确性/范围约束；实现方式不固定为位图/邻接/hash目录。不能为保下表布局再引入manager。当前source/state缓存、matcher payload和按raw分组存储均已随候选撤回，未获大幅方向支持，未来职责不规定恢复它们。

## 5. ARM64、运行时装配与交接

### 5.1 装配与 ARM64 核心逐文件职责

| 文件，相对 `runtime/backend/` | 最终负责工作 |
| --- | --- |
| `compile_binding.h/.cpp` | 一次装配环境、layout/bindings、target与合并兼容性 |
| `guest_helpers.h/.cpp`、`system_helpers.h/.cpp` | 公共helper语义/effects与宿主实现装配 |
| `arm64/host_features.h/.cpp` | 实际host特性；frontend不重复探测 |
| `arm64/compile/compiler.h/.cpp` | 原Storage/session/Prepared调度：全组preclose/metadata→同Store移交→两scope RA/state/原事务→独立检查/资源→物理化→真实编码/重发/发布。源域关闭前分流，关闭后禁止Inst fallback；不另建pipeline/Plan，也不复制family规则 |
| `arm64/compile/compiler_internal.h` | 同session私有声明和短寿命临时工作，不是新结果owner |
| `arm64/compile/target.h/.cpp` | 现纯能力实现，Known语义规范化；不向frontend暴露native类型 |
| `arm64/compile/native.h/.cpp` | 同Store唯一NativeInst/cells、actual defs/uses/birth、稀疏多role与定义owner、稳定header/origin及成功physical/resource提交。保持112B/16B，绝不复制完整Current；原计数与actual use分别保真。定义转移不包办cohort/state/live，最终family与生产读者未闭合 |
| `arm64/compile/constraints.h/.cpp` | ABI/pool/fixed/helper与中立sink适配；原Descriptor借同一ProgramLiveness直接生产actual Inputs。支持域不叠加旧semantic Describe/Finish，checked/public适配保真实读者；initial Complete与最终Ready分开，family语义来自同源select schema |
| `arm64/compile/constraints_internal.h` | 约束调用内的纯规则声明，不持有程序/结果，不成为preparation或新增管理层 |
| `arm64/compile/constraints_scratch.cpp` | 原rich scratch的need/两pool/outside-value/reload/overlap与emission-local override；semantic/native共享同一规则。独立文件是规则体量边界，不增加全N pass或独立scratch结果owner |
| `arm64/compile/state.h/.cpp` | actual read/write/guest commit/observer转移及Function entry/loop/edge；原copy行承接稳定id/type/窗口。把固定home读取规则共享为纯规则供checked/native调用，不把Inst延寿或复制policy；保版本/alias/copy/fault，不建第二历史 |
| `arm64/compile/state_widths.cpp` | 实际W/X/partial transfer、entry-width与per-use view；保独立zero/sign/unknown及循环合同 |
| `arm64/compile/state_publication.cpp` | guest publication/recovery/版本与必要transport；原SubmitNative内结束matched placement/Get sweep与最后scalar/pub修改，再按需fold/install、登记selected。无reused保早退，compiler不另跑晚form循环 |
| `arm64/compile/select.h` | family接口和同源form合同；Selection只存稀疏候选/unit/B信息，成功行移入NativeInst后结束 |
| `arm64/compile/select_scalar.cpp` | scalar/extend/bitcast/GetResult/predicate/narrow、预RA宽视图及Get32稳定B；严格SelectZero W schema，六类late scalar的共同形态/最终位置读者，kernel真实宽度与先读后写alias。首写lazy checkpoint，物理依赖选择仍归原owner |
| `arm64/compile/select_flags.cpp` | direct/legacy flags、真实consumer/token及CF/PF/AF合同；选择接管时同步转交numeric/local-key/condition义务，退休旧LocalCondition属性/event。原Q核raw逆向归属，最终graph独立核窗口；保真实首次consumer solver成本 |
| `arm64/compile/select_integer_flags.cpp` | integer/carry/narrow flags组合及coverage优先级；不把candidate当已覆盖 |
| `arm64/compile/flags_graph.h`、`flags_native_graph.h` | 前者是checked/native共用的原forward/dead-closure、PSTATE与retention纯规则，后者是native实际/raw/位置借用适配；无独立graph owner、缓存、pass或Passed表。完整raw图/CFG证明仍读真实程序，不用静态形态自签 |
| `arm64/compile/select_memory.cpp` | generic EA/load/store/extend/TSO/atomic/CAS、writeback/direct-narrow及Get/Set实际发表form；共享默认width/offset/clear-high规则，交付真实source role/transport并保fault次序；不用Generic占位冒充可编码发表 |
| `arm64/compile/select_vector.cpp` | vector/FPR/tuple、破坏性alias、真实uses/defs/pool/scratch与准入；不丢late coalescing来换compile |
| `arm64/compile/select_control.cpp` | helper/Params/Lambda、动态callee、terminal/entry/edge/loop及true/false安全form；host布局仍晚定 |
| `arm64/compile/physical.h/.cpp` | 原PhysicalStream/metadata与唯一资源向量、稳定origin、真实consumer resolver；最终资源完成即向本body交handle，不建N handle表。Current为native只读借用，193 kernel已有读口；补MEM/adoption须与原transport/resource同组。旧variant/Params归冷存储，公共/retained/packed保原能力；支持域默认路由与normal覆盖分开记录 |
| `arm64/compile/verify.h/.cpp` | 必要最终合同调度、opcode/schema检查与独立audit接口；不是producer Ready/certificate |
| `arm64/compile/verify_gpr.cpp`、`verify_fpr.cpp` | 稳定BackendVirtual上从实际schema/effects、Allocation/live独立检查，不信producer/Ready；FPR与checked共享family规则、publication保consumer路径且occupancy查全scope。InvalidProgram/UnsupportedForm/ContractRejected分开，仅真正质量拒绝走原retry；不建证书镜像 |
| `arm64/compile/verify_gpr_core.h`、`verify_gpr_native.h` | 独立GPR合同与native真实读口；按write owner、完整allocation lease、actual-use/terminal及P前硬窗口核同点交接。可用一次实际use访视替代逐Getter全scope发现，保observed count完整性、fault/capture和错误分类；B-local截断不证明Function死亡，不养证明影子或Passed |

`native.h`定义实际对象与借用视图，`native.cpp`落实转交/最终兑现，不另加native_builder/prepare/actions/certificate层。约束的GPR/FPR/fixed/scratch规则随family和公共原语收敛，**目标树不再保四个独立Describe builder文件**；全部真实读者未迁完前，它们仍是现码，不能提前删除或偷偷搬成另一套prepare。

后续按[主方案5.3.12](spill-and-compile-root-cause-plan-2026-10-28.md#5312-下一相依组先闭合机器合同再替换默认链)以真实纵向相依组闭合形态：family语义归现select族，body/operand与program适配归native，birth/live与共同assignment归公共allocate。稳定use/def/birth合同先于首machine live；公共层不按ARM64 opcode特判。选定组的生产者、两scope/state、最后kernel和独立检查接通后即做少量完整反馈，不等全部族迁完；未覆盖域保前置合法分支并计入覆盖与总成本。

文件数不等于pass数；select、constraints、state、verify各有合同，不要求各扫全N。真实extra writes/result replacement与clobber/writeback仍分开表达。第77/78撤回的Q/owner/source目录不纳入目标树。

### 5.1.1 直接交接与实际依赖

```text
公共语义build/residual/hoist → 完整SSA Finish及最后图匹配
  → 原terminal/entry/hoist值移交，全组semantic closure
  → 同Store BackendVirtual（成功form＋实际uses/defs/effects）
  → 中立program view → Block局部 / Function原CFG machine live
  → 原Writer分配/有限候选准入/受界retry
  → actual state/entry transfer、剩余兑现、独立最终check
  → 同operand原位物理化 → PhysicalReady
  → 现kernel；true失败只重建ctx发false，共读同一程序
  → 最终Flush/持久metadata发布/最后资源结束
```

稳定form的真实use/def/根/birth必须先于首machine live，不能只强调“birth前”。真正依赖Map的变更在受影响interval/owner开放前维护live、fixed/tie/clobber和queue/pressure，或受界回滚/重建；不能只改Map/recipe。责任归原schedule/assignment/Writer，不另立候选排程图。只有不改这些合同的编码细节晚定；Function entry/form环保有界求解，完整SSA Finish先完成。

| 数据 | 原owner与结束点 |
| --- | --- |
| semantic Inst/Users/图匹配 | 原Store/编译CFG；全部semantic读者闭合后结束，不跨native RA/编码保活 |
| CFG/entry/terminal/hoist | 原CFG及PhysicalStream的B/unit元数据；引用值先virtual再physical，不留旧terminal树 |
| virtual/physical程序 | 同一Store及原Prepared/stream租约；Virtual承受retry，Ready后不可变并供重发 |
| actual positions | 原Allocation/Writer；最终物理化/必要检查后，无真实读者即可按原owner结束，不把释放挪出timer |
| state/copy/version | 原state；实际转移/恢复/最终检查结束后释放，持久输出必须自持 |
| offsets/fault/link/entry | 原emitter/code object；每次emit重建host数据，唯一成功image发布 |

公共/retained无消费权事前选checked路线；EarlyOff仍可Known，Unknown仍有真实generic build/受控绑定。不能先付完整新链再正常回完整旧链。原支持域默认已接提前closure与同Store接管；source miss/retained保IR到Flush。真实normal NativeReady覆盖须独立观察，组件夹具不能替代采用率。

### 5.1.2 同一文件不再隐含无限重复工作

| 服务 | 迁移结束条件 |
| --- | --- |
| target语义描述 | 所有normal family的operand/effects直接供原RA与state；完整旧Describe/Declare/Select同义发现退出，不是在其后增加lowering |
| machine live | 真实use/def/根/birth合同先于首份machine live；旧ends只在完全等价时复用。必要唯一关系索引按原owner生命周期准入，不复制已有用途图 |
| 位置与早写 | 稳定use/def/birth变化先于首machine live；Map相关变更在受影响interval/owner开放前维护完整依赖，或受界回滚/重建。仅不改变该合同的编码细节晚定；失败不残留Map/form/width/use-home |
| logical MEM资源 | 必要raw occurrence/multiplicity/use-death成为同程序真实transport事件；不拿numeric input代替，不保全量旧ordinary args/Users |
| state/entry | 必需alias/fault/guest version/P输出与Function meet保留；同source/历史不被各层重建，迁移工作不算全部节省 |
| 最终检查 | 真实Allocation与opcode/effects独立受检；候选、selected或Ready不自证；GPR/FPR/width/fixed合同保留 |
| 编码与重发 | 193 kernel借稳定body；不回semantic producer/Map，不每emit重建Current或再走packed codec |
| retry与结束 | virtual程序稳定、事务稀疏回滚；上界/重建范围可审查；随旧数据退出删初始化/复制/清理，不靠扩大缓存 |

这些是实际退出条件，不是一行一个类/文件/逐节点证书。过程只统一构建和受影响正确性；完整链闭合后一次方向，最终集中验收。

### 5.1.3 最新初始合同的文件接线

本表区分当前已写接线与该职责的结束条件。C3/6f7b已有四目标和限定最少检查，资格范围见主方案2.1；表中完整终态仍未全部交付。最终目录仍按第2～4节，不先搬全树或新增pass。

| 当前源文件（同列共享职责） | 已接源码与下一责任 |
| --- | --- |
| ARM64 `select.h`、`select_scalar.cpp` | 最后semantic阶段稀疏转交whole-unit/Function-local flags，静态form/pair与粗准入；窄/跨类BitCast当前事前走原合法入口。后续扩完整形态，不把decode变成物理选择器 |
| ARM64 `native.h/.cpp` | 同槽实际defs/uses/birth/B identity、site/raw/range/block-use借用；Original位与自动退休行保原cast/计数，MEM保真descriptor。C4进一步写稳定header/origin、稀疏role、resource及定义owner原语，已有本版限定构建/检查；完整cohort/state与memory transport和normal准备仍待兑现，不回已死Inst |
| ARM64 `constraints.h/.cpp`、`constraints_internal.h`、`constraints_scratch.cpp`、`target.h/.cpp` | 已实现借live的initial入口、rich scratch/两pool、actual partial访问和原observer；净RA clobber与raw ABI分域。后续补全支持域并使旧normal Describe实际退出，不保第二完整输入程序 |
| ARM64 `constraints_gpr.cpp`、`constraints_fpr.cpp`、`helper_call_contract.h/.cpp` | 前者在semantic最后读者保存原稀疏GPR/width拓扑；FPR共享完整family规则读actual程序/访问；helper纯ABI规则共享。最终分别并入select族/共享约束规则，不各养一份投影或额外Prepare层 |
| 公共 `program.h/.cpp`、`scratch.h` | 中立借用/一份machine live、ProgramInputError与actual scratch核算；已修非法callback分类、点/块/输出边界及FPR block span。静态准入一次、动态非法就地退出，不加证书图；Matches不证明编辑版本 |
| 公共 `inputs.h/.cpp`、`constraints.h` | 原稀疏quality/fixed行承接actual token/birth与必要旧语义计数；唯一RegisterAccess/observer序列由producer写，原消费者借。请求选项显式带入，不另建N目录/E缓存 |
| 公共 `intervals.cpp`、`fixed.cpp`、`locations.cpp`、`writer_locations.cpp` | actual birth贯穿初始映射、固定冲突、位置/preservation/recolor；联合def借原排序稀疏约束而非ID游标。后续独立同点输出/extra write核验仍归最终check |
| 公共 `gpr.cpp`、`gpr_writes.cpp`、`width.cpp`、`ties.cpp`、`coalesce.cpp`、`gpr_policy.h` | 原完整quality顺序与事务已读program；私有helper不拥有图，最终归coalesce_gpr/coalesce_width等同职责文件。后续在原调度/Writer兑现真实form与live，不再加actions或certificate包装 |
| 公共 `fpr.cpp`、`linear_scan.cpp`、`internal.h` | 原assignment、固定读/active、publication/coalescing及pressure读actual程序；保Function live-in和端点。post-publication的P_pub×D及future-read循环照计，最终FPR证明不信任producer recipe |
| 公共 `allocation.h/.cpp`、`controller.h`、`writer.h/.cpp` | no-Facts Writer与原attempt/controller、FPR retry、Reset/Finish；已修checked/native pool预算分支及invalid立即退出。AllocationState拥有inputs、借program/live至Finish，不建第二控制器 |
| 公共 `spill_regions.cpp` | actual/raw分开消费，原MEM-only region、ownership/transfer/hand-off/headroom与Block local repair共用规则；保strict余量和完整ownership正例。最后MEM/Tick/换源count0寿命交真实kernel资源协议，当前不称闭合 |
| ARM64 `compiler.*`、`state*`、`verify*`、`jit/translator*_physical.cpp` | 下一主要工作：生产normal调用者、真正form/state/entry/fault兑现、独立最终核验和193kernel最后读口；旧Describe/Complete/binder退出后才认服务替代，Ready及同槽本身不自证 |
| `tests/native_program_test.cpp` | 完整Block质量/retry例在本版通过，Function/FPR等其它native阳性保aea身份；raw alias保canonical U64/raw VOID，原pressure ownership正例不削弱。仅证明所覆盖Core/BackendVirtual，不冒称final Ready/性能 |

本节C3所述构建/边界问题当时已收口，历史322整合组构建断点保原身份，最新默认接管见主方案7.1。不按这些文件逐个增加QoR/PMU/FEX检查；相序、真实复杂度和normal退出条件只在[主方案5.3.12](spill-and-compile-root-cause-plan-2026-10-28.md#5312-下一相依组先闭合机器合同再替换默认链)维护。本表是文件职责映射，不是另一条实施计划。

### 5.1.4 C4实际接缝的文件归属

本节保留标题以稳定原链接。C6限定事务保原身份；前轮C7/3bdd060c控制引用与普通内存组已完成受影响检查，该轮默认gate为false。准确资格见[主方案7.1](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)。**沿现有owner完成服务，不新增resource-manager、cohort registry、binding-plan或certificate层。**

| 当前源文件 | 唯一职责与当前边界 | 目标落位/退出条件 |
| --- | --- | --- |
| ARM64 `native.h/.cpp` | 同Store程序、actual form/use/def/effects、Original/raw header、whole reference/write_owner；body checkpoint、actual-use比较、Prepare/Freeze及原word的PreparedResource。已构建。 | 留`native.*`；只承接真实结果，所有用途/raw退休后再结束。prepared保持virtual但禁止普通编辑；不另建N handle表。 |
| ARM64 `select.h`、`select_memory.cpp`、`select_scalar.cpp` | Get32静态B、SelectZero shared-W、typed GPR/Whole MEM发表、六类scalar schema已合；MEM32保Mov W，fused/selected及未建立reload仍缺域。 | 留原family；producer/resource共享原adoption规则，source/lease与独立reader同组。稳定B先live，首写lazy记journal，保局部/整批两时期恢复，不扩大公共canonical白名单。 |
| ARM64 `constraints_gpr.cpp`、`constraints_fixed.cpp`、`constraints_internal.h` | 已合入同次拓扑/UseFacts生产preclose窗口，借原四类匹配主体把真实fixed/preserved声明直接写原Inputs；Block/Function范围独立。 | 最终随`constraints.*`及原family规则收敛，窗口在TakeIR前结束。不复制旧Describe/Map输出；不新增长寿preclose表或另一个遍历owner。排序插入/seed复制仍计价。 |
| ARM64 `select_vector.cpp`、`constraints_fpr.cpp`、`constraints_scratch.cpp` | 静态FPR/effective Config、Covered/AesTied及actual scratch；final读同源scalar及实际defs/绑定/copy/reload/preservation，FPR body修改先结束。 | 留原族规则；NoBody不是硬件写，actual GPR不按raw MapMEM误算。Get32已过限定scratch/Ready；used FCmp/Sse42/Indexed等缺域仍须补，不外推完整质量资格。 |
| ARM64 `physical.h/.cpp` | 编辑期NativeLocation借同一Writer/result与actual frame；最后冻结resolver、typed资源policy、同体Freeze/退休，已构建。 | 留`physical.*`；资源在所有body仍virtual时生产，payload入原库后才ResetCurrent；所有body/controls预检结束再首个Freeze。输入编辑重绑frame，不留全域operand镜像。 |
| 公共 `allocate/program.h/.cpp` | 中立actual sites/uses/defs/birth、raw traits与原CFG借用；新增可选terminal-only同步借用，直接枚举真实roots而不重扫body。 | 留`program.*`，不认识ARM form，不复制terminal roster/N/E程序；actual writer不当whole REF，不读committed前缀Definition。 |
| 公共 `allocate/allocation.h`、`locations.cpp` | 原Map/roles、copy/width/capture和GPRCopiesFor；新增只读ForEachSpillReload借原point-vector，不改stored key。 | 留原owner和selected/inferred/LowView/preserved顺序；借用同步结束，不提前Freeze。reload真实访问是point slots加rows，不按纯rows报成本。 |
| 公共 `allocate/writer.h/.cpp`、`writer_locations.cpp` | 真实pool、B copy安装、原fixed/preservation及编辑期owner；原RA已建立preservation，通常只借现成payload。原copy/action journal扩展已合入构建，运行rollback未闭合。 | 留Writer/locations；同一AllocationEdit受检替换，只有真实preservation失效才move-journal后重建，不常态全域重扫。 |
| 公共 `allocate/recolor.cpp`、`transaction.h/.cpp` | Recolor已读实际definition birth，preserved保transfer；Map/fixed/selected/copy/region/obligation及批量transport事务，body checkpoint由原调用方持有。 | 最终归`repair.*`，不先搬目录拆层；journal只记触及项，失败完整恢复/重绑，最终独立检查后Commit。D=0仍fresh proof，不提前Commit换copy安装。 |
| 公共 `allocate/intervals.cpp`、`linear_scan.cpp`、`ties.cpp`、`writer.cpp` | shared-write联合lease/pressure/next-use、独立逻辑用途及原两scope调度。 | 最终归schedule/assign/coalesce；补fixed/read/member tie、Capture/LONG、最早owner前局部回退，不再新增allocator或候选图。 |
| 公共 `allocate/spill_regions.cpp`、原transaction/locations与ARM64 `physical.cpp`、原JitContext | canonical物化、全组actual覆盖与各raw退休；late失效处理、materialization_last_use/required_backing及资源消费已合入构建；完整selected memory/adoption、flags/control/recovery仍缺。 | 最终按`reloads.cpp`/`repair.*`/`physical.*`/原JitContext收口；NoMat先退失效region，backing优先；writer/key/birth/end越旧lease回原owner。SameReads比较实际输入，不能只比count/end。 |
| 公共 `analysis/guest_state.h`、`guest_state_cfg.cpp`、`allocate/inputs.h` | checked保唯一UseFacts所需拓扑，native候选/用途从实际转换及Descriptor直产；原byte/label/CFG算法保持；精确anchor资格只写原候选/行，RecolorAnchored已合入构建。 | 留原analysis/inputs；不重建source图或晚逐候选扫E。匹配成本、payload、foreign/catalog照计，资格不代替actual proof。 |
| 公共 `allocate/inputs.h`、`gpr_writes.cpp`、`fixed.cpp`与ARM64 selector | Get32静态B在quality行出生前编辑实际body，随后直接生产actual publication/hint/preferences；旧inactive派生行修补及undo已退役；native width提交读actual stored/home。raw软hint的value/source仍保原身份。 | 复用原输入/提交owner；无hint合法，hard/未限定hint修改前typed拒绝。不复制GPRInputs或从已清空semantic表误判无commit，不新增quality证书层。 |
| ARM64 `state.h/.cpp`、`state_widths.cpp`、`state_publication.cpp` | 原版本/copy/observer/fault与唯一native WidthTransfer/Meet；最后scalar/pub仍在SubmitNative的placement/Get后、fold/selected前。NativeWidthInputs首写核逻辑输入；FinishNativeResidence按旧边界释窗口。 | 留原state；逻辑transfer改变typed EntryWidth回滚，不只求新width而借旧copy。原B×30 facts供proof后metadata复用，EndNative先于locations冻结；C7 ClearCompound只读同解，不另立solver/N表。 |
| ARM64 `verify_gpr.cpp`/`verify_fpr.cpp`、公共`fixed_verify.cpp`及`constraints_scratch.cpp` | 共享纯规则与独立合同核raw资格、实际lease/copy捕获、路径与actual scratch；重复发现算法可按净账替换。 | 无Passed表；保same-point、preserved transfer/ActiveAt、terminal和真实CFG。proof/actual scratch在编辑期完成，后续实际编辑使之失效。 |
| ARM64 `verify_gpr_core.h`、`verify_gpr_native.h`及公共`allocate/fixed_verify_internal.h` | 共享原publication/width/low32/read、实际native路径及fixed主体；最终发表检查替换旧site绑定片段，Value/Low32/preserved核strict-before来源，同home省W写核当前实际高零。 | 不新增证明pass或公开证书；高零、逐请求defs/CFG与B级scratch照计，按需而非每值追溯，随普通服务批处理收敛。 |
| ARM64 `compiler.cpp`、`plan.h`/`plan_build.cpp`与原JitContext | 原Storage/PrepareNativeGroup贯通有限真实scope；删除preclose semantic宽度解/30B查询。Edits在Stage前后各一批EvaluateTransport，原resource/Tick owner不变。 | 不新建准备层或搬family规则；D/J证据分别重建，NoMat才整scope uses/terminal，需核reload才访原storage。原preservation请求×actions、Capture/Stage/fresh proof保留，该轮默认gate为false。 |
| `tests/native_program_test.cpp`与原受影响行为测试 | C6/min09的joint一例2985断言及其它阳性保原身份，partial view仍无global LowView；C7控制/恢复与ADC/SBB实际Run增量均已合入。c984成组11过1 owner失败，2266仅新构建通过，不把历史、后续单例或红例前缀并成当前全绿。 | 保原Map/pressure/policy及断言；相依组统一最少真实owner语义，不逐文件QoR/PMU/FEX，精确资格以主方案7.1为准。 |

本表是维护归属，不是执行pass表。已删无改候选快照、preclose semantic宽度解/30B查询及逐请求transport全域分类；保实际D_edit两时期恢复、单一native必要解、Stage前后两次独立推导、point slots/rows与preservation的Σ D_t×A_t。preclose插入/seed、原Recolor/定义/raw/CFG、高零、fold/journal/profile/双resolve继续计入[六段净账](spill-and-compile-root-cause-plan-2026-10-28.md#581-当前knownnative的净工作账)。不建N Passed/owner目录，不凭文件数保证两倍门；Known/EarlyOff/Unknown、独立Block/Function及最终不降质量目标保持。

### 5.1.5 下一flags服务的文件归属

C7/3bdd060c数值flags/控制引用/普通内存与395的CondSetControl/owner602保原限定资格；本地state/width/flags合流、metadata与同owner恢复已整合322待验证。**沿现文件/owner闭合服务，不增加flags-plan、resource-manager、第二key map或另一份程序。** 下表是维护职责，不是pass/扫描表；完整合同见[主方案5.3.12.7](spill-and-compile-root-cause-plan-2026-10-28.md#native-flags-next-service)。

| 现有文件 | 唯一职责及退出条件 |
| --- | --- |
| `ir/instr.h/.cpp`；公共`analysis/effects.h/.cpp`、`analysis/pseudo_flags.h/.cpp` | 共享合法outputs、FlagAccess、packed merge与effective demand纯规则；RA/本B emission/branch-only分域，checked/native只换读口。common不依赖ARM，不新增flags图/固定点。 |
| ARM64 `select_scalar.cpp`、`select_flags.cpp`、`select_integer_flags.cpp`、`select.h` | 最后semantic用原consumer/integer matcher闭合稳定图；稀疏需求/原source交原槽，CMP/ConstantFlagsOnly/matched condition的实际input/def变化先于live。纯规则就近共享，不把每个步骤拆成一层结果。 |
| ARM64 `flags_graph.h`、`flags_native_graph.h` | 数值组已共享原forward、PSTATE、retention与完整accepted dead closure；native adapter重读真实raw/actual图与位置。下一控制proof分别核Carry原tuple、raw terminal及NZCV祖先窗口，不把所有Covered归作死整数；普通Extend逻辑立即数改借同源投影。 |
| ARM64 `native.h/.cpp`；公共`allocate/program.h/.cpp` | 表达唯一程序的actual data use/def与必要Logical/control关系；未覆盖Original head直接借作Logical，只有换槽才保稀疏原参数。CMP无数值def，AddCarry保真实result，SetCarry/OF保arg0实际read。原C6 U32 bridge Label0与混合prefix合同保留，Header不能作类别证书。 |
| ARM64 `constraints_gpr.cpp`、`select_flags.cpp`、`select_scalar.cpp`及原`AllocationInputs`/journal | 有限D同步换源/退役项的reads/views/hints/preferences/def需求，soft真实删除或失活，沿原ordinal撤销；hard关系兑现或破坏前分流。无候选快退，有候选批量闭合，不复制全GPRInputs或逐候选扫scope。 |
| ARM64 `target.h/.cpp`、`constraints.h/.cpp`、`constraints_scratch.cpp`；公共`allocate/writer.h/.cpp`、`intervals.cpp` | 原discard纯能力与flags-only MEM受检适配；保raw总数、actual use、backing不同义。沿原installer/FixedReady退出新增candidate×F扫描；按实际输入/def/transport算scratch，精确ConstantFlagsOnly当前0/0不吞未来observer预算。 |
| ARM64 `select_flags.cpp`、`select_control.cpp`、`state_publication.cpp` | 真正Map相关token、carry/direct-control/backedge选择在原B最后位置访问兑现；读实际NativeLocation、publication suffix/preserved原点，不恒homes=0。原事务内使相关live/state/选择失效，不在proof后补form。 |
| ARM64 `state.h`、`state_widths.cpp`、`state_publication.cpp`、`physical.h/.cpp`、`select_control.cpp` | ClearCompound共用唯一native入口解、SSA/CFG/terminal carry谓词；PhysicalIf末尾local_condition承接精确控制域，actual state/live/resource跳过其数值用，raw退休借原树另核。无依赖不强制Function solve，未完成metadata不能默认false。 |
| ARM64 `verify.h`、`verify_gpr.cpp`及原flags独立reader | 现site访问累计临时保守needs_flags并在同B一次完整proof；重读actual form/use/def/control、原raw闭合、site/birth/location和必要entry事实。借同一entry解先于EndNative，不另建Passed表或重扫B筛选。 |
| ARM64 `compiler.cpp`、`physical.cpp`、`native.cpp` | 原Storage最后semantic准备→预破坏准入→有限D/首次live→原scope RA/late编辑→fresh proof/resource→metadata/EndNative→Freeze接线；all-virtual合需求入原ResourceRecord后，pseudo word才转handle，prepared/prefix禁读成需求。 |
| ARM64 `jit/translator.h`、`translator_control.cpp`、`translator_flags_physical.cpp`、`translator_region.cpp`、`translator_scalar_bits_physical.cpp`、`translator_scalar_physical.cpp`、`translator_terminal.cpp` | 主树adapter已沿Current LocalConditionKey/PhysicalIf精确域读原local_conditions；已合CondSetControl补真正无Result与Generic必写、native数值读口及单一Begin/End mode；不为退BOOL回ScalarRead。保LocalParity/FCmp真实temp/写入、Carry Tst/merge、Tick/raw；缺key guard不是backedge动态恢复。原CMP无destination读口已合，不再迁193 kernel。 |
| `tests/native_program_test.cpp`及既有flags/terminal/width例 | 当前原public-owner一例492断言、原Switch一例230断言通过；历史数值1/282与checked17/2839保原source。真实scope/压力不变；控制组后续只补受影响真实接点，不把旧阳性重计为本版整套，不逐helper扩矩阵。 |

body arg0的窄Label-kind引用＋Current LocalConditionKey、PhysicalIf末尾默认false标记已在3bdd整组构建并获限定检查，保完整token/type/view，不伪造DISCARD或放宽NONE/Map。first/mask1 recipe已交同一key；原raw terminal借同一PhysicalTerminal树，未增加公共raw Access。Switch命中子树前恢复保存的物理NZCV，原Condition reload仍是屏障；最终reader按实际kernel而非form名判断Select等的NZCV保持。CondSetControl已获限定检查，backedge/PFAF端点和本地合流另闭。原raw、finite D、REF/归属、窗口/CFG、preserved P×D和资源费用保持显式，该轮默认normal未切换。

### 5.1.6 普通内存接收域的文件归属

当前3bdd已集成并获public-owner限定检查；本表不增加文件或pass，不授复杂selected/fused或guest执行资格。行为合同见[主方案5.3.12.8](spill-and-compile-root-cause-plan-2026-10-28.md#native-memory-next-service)。

| 原文件 | 唯一职责及必须同步的读者 |
| --- | --- |
| `select_memory.cpp`、`select.h` | 同源普通TSO/Extend family/schema、EncodeLoadExtendMemoryControl及NativeLoadExtendDescriptor纯投影；checked写口与native转换共读access/sign/extend规则，不新增memory结果表 |
| `native.h/.cpp` | 声明同槽逻辑投影读口；转换尾把原Cell3 descriptor写为真实PhysicalMemoryControl、定LoadExtend form。稳定身份/类型与raw预算沿原槽，不存第二raw立即数 |
| `constraints.cpp`、`constraints_scratch.cpp` | initial/实际scratch读同源family；MayFault对应原FlagAccess read All，不能硬核None来缩域。TSO load/store保原候选范围、两pool与MEM需求，不放宽其它内存族 |
| `state.cpp`、`state_widths.cpp`、`verify_gpr_native.h`、`flags_native_graph.h` | 原逻辑descriptor读者借同源逆投影，保访问宽度、sign及extend32高零；不把实际控制字当原4bit描述符解码 |
| `physical.cpp`、原memory kernel | resource仅接精确ordinary LoadExtend形态；保RCpc/unaligned/fault/barrier、真实backing/reload及原plain Extend forwarding，不以Generic豁免验证 |
| `compiler.cpp` | 全部接点同组完成后缩对应source粗门；fused、复杂Map/EA、direct-narrow及跨类另闭，不靠永久Unsupported完成normal替代 |

<a id="native-local-control-layout"></a>

### 5.1.7 本地控制、宽度与flags合流的逐文件职责

**本相依组沿现有文件完成，不增加控制图或管理层。** 控制/state/width/Save/metadata、同owner恢复、scratch与限定location已接；33da原fixed457/controller295/owner1989通过，cee0四失败保历史身份，dd0当时的限定原例与默认路由见5.1.11；当前修复资格见5.1.13。下表是维护责任，目标目录仍用第2节；文件数不是pass数，当前资格见主方案7.1。

| 现有文件/owner | 负责的工作与边界 |
| --- | --- |
| 公共`analysis/guest_state.h`、`ir/ir_meta.h`及`ir/instr.cpp`，ARM64 `native.h/.cpp`、`select.h` | checked/native共享前向label、ByteOf及Boolean纯规则。完整Store/token/view、同B前向/单绑定；Goto数值条件/key、Bind actual角色与raw身份边分开。`NativeDefinitionGPRWrite`按实际definition/site/birth/form/effective demand分Value/Temporary/DynamicScratch/MayWriteValue/None；MayWriteValue只作负向排除，不授真实W写/高零种子。稀疏emission需求与raw packed需求分开。无第二CFG或N索引。 |
| `target.h`、`constraints.cpp`、`constraints_scratch.cpp`、`physical.h/.cpp` | 共享原FlagsResult目的地和dirty→forwarded→fixed→10/12/13 lease的scratch纯规则；initial/resource核真实mask及已prepared UnitScratch。保无Result形态的真实scratch、raw Tick和fault/MEM寿命。同一PhysicalIf存actual源，只有真换源另保稀疏raw token/view/sense，不复制terminal树。 |
| `state.h/.cpp` | 六参BeginNative借真实inputs/live/Writer/resources/config和prepared UnitScratch，End结束借用。Goto在真实prefix后保存活跃copy的version/home/width/extension，Bind保taken/fallthrough交集；逻辑byte相等不证明taken home实际可用，不伪造join写。处理MEM输入LDR、birth reload.reg、NoMOV/FlagsResult及可能clobber；MEM U32不自证X高零。 |
| 公共`backend/gpr_value_facts.h`、ARM64 `state_widths.cpp` | 原unary/Meet和唯一Function solver处理local合流。真实Goto才提升多源摘要，Get在定义点捕获；local内联、foreign只保实际Q_get来源及稀疏依赖，Get事实改变而exit不变仍入原队列。local/foreign独立oracle在c984定向通过；不授所有capture/foreign域资格，不建N历史、第二CFG或每轮IR重放。 |
| `select_flags.cpp`、`select_integer_flags.cpp`、`select_control.cpp`、`select.h`、`flags_graph.h`及`flags_native_graph.h` | preclose只选原metadata及hard窗口；唯一Native转换后用Apply在首次live前稀疏编辑真实baseline。共用原raw Backedge/PFAF/narrow matcher，保UXTB/H、CMP、真实temp；延迟Save所需result不能被Covered退掉。原RawPstate加前向label Meet，最终reader独立核实际端点、NZCV/PF/AF/home/window并精确分类MetadataCohortRejected，不以metadata/header授proof。 |
| ARM64 `constraints_fixed.cpp`、`constraints.cpp`、`constraints.h`与公共`allocate/constraints.h` | 首次actual live后、原allocation owner打开前重准入fixed，不只延长end；Derived来源仅由原installer明确标记。新增use的真实definition、terminal/REF与破坏窗口按原capture政策处理，受影响原行进入fixed journal。Explicit实际use扩域已接capture/fixed_end/allocation_begin；原两硬home、REF alias/root capture与canonical lease分别保义。精确三类typed residual保留，GPR/U64例227通过不授partial MEM。最终归原constraints/installer，不新增分配修正pass。 |
| `jit/translator.h/.cpp`、`translator_flags*.cpp`、`translator_control.cpp`、`translator_region.cpp`及`translator_terminal.cpp` | 原flags/key owner按Goto当点保存L个短寿命表示和scratch，Bind相容直连/异构Merge/Flush；保raw NZCV，key birth只作失效过滤。SaveResult在真实Save点发表legacy flags，intrinsic保producer原点；ADC/SBB用真实result/原operands恢复CF。保Switch恢复及非S fallback；schema/effects/scratch须早已交付。 |
| `jit/jit_context.cpp` | 原BeginInstructionScratch委托同一scratch mask纯规则，GetTmp/GetSpillTmp仍执行真实租借；原FlagsResultRegister委托同一实际目的地规则。JIT与proof不能各写一套看似相同的优先级。 |
| `verify_gpr.cpp`、`verify_gpr_native.h`及原flags最终reader | 原GPR site访问累计needs_control，对相关B一次独立token核验；Bind在处理prefix前拆路径。原实际writer正证不含未知dynamic可能home；窗口/byte/高零负向排除才读prepared scratch的可能clobber。逐byte保offset，MEM/capture须实际支配；不加Passed表或新writer目录。 |
| 公共`allocate/controller.h`、`allocation.cpp`、`writer.h`、`writer_locations.cpp` | 原owner窄口结束编辑保唯一State/Inputs；旧baseline有效时先清Map/soft birth，再清旧fixed声明/ID/publication/ready及preservation索引，重装新fixed。RebuildProgram接原RunAttempts保选项/FPR预算；不复制const身份Inputs，不新建retry manager。 |
| ARM64 `plan.h`、`plan_build.cpp`、`compiler.cpp` Storage | 保原AllocationState，结束借用后先删Derived、旧gpr仍活时恢复Explicit ordinal，再保CallABI清generated preferences并reset gpr/实际列；逆body→Withdraw→冷路重收C/P→actual static/live/Describe→原RebuildProgram。已无flags/Get32语义行undo。保一次metadata和独立FPR状态；RunAttempts局部eviction每次进入重建，不虚称跨重建共享余量。调度沿原owner收敛，不固化preparation层。 |
| 既有`native_program_test.cpp`、`function_width_transfer_test.cpp`、flags/terminal用例 | 原owner已合控制/Save、Get/foreign、FPR与metadata恢复检查，以及直接Runtime.Run的ADC/SBB增量；c984宽度oracle、controller/state/joint及fixed/稀疏存储定向通过，public owner总例仍红，控制SECTION仅编码。不能把计划16次或失败例前缀拆成独立执行资格。相依组最少检查，无逐helper QoR矩阵。 |

这里的多处文件共同服务一次决定，不能逐文件计为pass。真实费用包含F行重资格、D_f×F移动、Σ_Goto activeD快照/排序/交集、publication扫描/逐发表追链/去重前排序及Q_get依赖与Φ、每query seen/profile/位置解析、Save新增压力、全PhysicalIf布局及一次失败N/E/P＋RA，统一见[净工作账](spill-and-compile-root-cause-plan-2026-10-28.md#58-正常路径的工作量表与退出标准)。当前已开限定SetLocation/RSB/call-return域，精确条件见下一节；selected memory、真实hoist anchors及三类fixed残余仍未闭合。foreign宽度实现不代表所有capture/MEM域可用。支持域已默认绕过旧服务；r2首个真实guest启动失败尚待修复，随后再开始新一轮完整方向。normal覆盖与收益尚未测得。

<a id="native-final-resource-layout"></a>

### 5.1.8 最终scratch交接、terminal root与fixed扩域的文件责任

沿现有文件完成，不新增resource pass、控制表或fixed修正层。第2节目标树不变；下表是同一服务在原owner内的责任及真实旧工作退出，不以文件数计pass。

| 当前文件 | 本轮责任与后续边界 |
| --- | --- |
| `compile/compiler.cpp`原Storage/Proof | GPR/FPR/transport先独立通过，再授权原scratch终态输出；`final_scratch_started`禁止此后metadata/FPR重建。EndTargetPlanning后唯一move，资源循环顺序Decode/BuildNativeCurrent/Prepare；**已删除该循环的DescribeNativeScratchProfile**。仍保全scope资源先完成再Freeze，原Ready/重发/发布/退休。 |
| `compile/constraints_scratch.cpp`及`constraints.h` | 原CheckNativeScratch从最终真实body/live/位置重新推导并核资源，必要时写原backing的相邻压缩行。control/location独立核验在clear之前；冷拒绝只查不写。初始/状态/末次推导仍在，不宣称全流程一次profile。 |
| 公共`allocate/writer.h/.cpp`、`controller.h` | FinalProgramScratchInputs核唯一program/live/原inputs及相位；TakePreparedScratchInputs保checked原协议、允许native合法冻结相位唯一move。非法owner/相位/重复非空transfer拒绝；不以const_cast、第二N向量或Passed证书伪造交接。 |
| ARM64 `select_control.cpp`、`select.h`、`physical.h` | semantic关闭前把动态location/CallReturn resume交原B terminal roots，独立IR source/view/CFG准入。按真实terminal时点延长machine live；符合原policy的可选同home延迟在Edits、transport/proof前兑现。实际form与最终复核共用NativeControlLocationForms，各次仍解析真实位置、收集候选、判断home，不能称三阶段一次纯解码。 |
| `native.cpp`、`physical.cpp`与`verify_gpr_native.h` | 原program/live/raw/资源/Freeze读者接terminal roots；原body occurrence只核销一次，派生root不加raw count。保MEM backing/reload与真实末端寿命，原raw/control/data分工不变。 |
| `jit/translator.h/.cpp`、`translator_control.cpp`、`translator_terminal.cpp` | 当前B编码期间借原metadata，直接读最终physical location/resume。PushRSB动态/noRSB/L1按原Covered语义；Pop marker不做terminal真正pop；不读已关闭Inst、不以0表示未知位置，保checked/packed能力。 |
| `select_memory.cpp`、`native.h`、`flags_native_graph.h`、`state.cpp`与独立GPR reader | 同源NativeGPRPublicationByteOffset把实际发表控制字投影为逻辑byte offset；完整shape/home/window/width仍分别检查，畸形投影失效。point0真实capture在原state fold处理，不靠插Nop改变场景。 |
| 原公共`allocate/constraints.h`、fixed/home/preservation索引、writer/location、interval/recolor与ARM64 fixed/transport读者 | 已接capture/fixed_end/allocation_begin，保原两硬home与REF alias/root两次capture；原REF访问扩canonical lease，不移动真实动作。实际publication源/逻辑offset、partial MEM prefix scratch与独立proof同步。dde原case457含GPR/U64、Function双capture、U32/MEM与完整Residence；三类typed residual见主方案5.3.12.13。 |
| 公共`allocate/allocation.h/.cpp`、`locations.cpp`、`writer.h`、`writer_locations.cpp`、`home_index.cpp`、`preservation_index.cpp`、`transaction.h/.cpp` | 原Allocation保32B placement/u32 slot、32行稳定页与唯一active roster；原Writer是fixed可变写口，原索引借stable结果，原transaction保普通32B snapshot及只随D_f的fixed undo。仅Release重置slot/K，Finish后仍供emit/reemit；页全部纳入原128KiB aggregate预算。没有新目录、cache或修正pass。 |
| `native_program_test.cpp`与原`cases/allocation.cpp` | 33da原fixed457/controller295/owner1989三例通过；历史stable45/rollback35保原source。自然U32→ZeroExtend64覆盖late coalescer，f3d0回退仍按真实PC区分。当前新增WB路径红，不手填Map或拆红例授绿。 |

限定支持域、终态失败和新增成本见[主方案5.3.12.11～14](spill-and-compile-root-cause-plan-2026-10-28.md#native-final-scratch-handoff)与[5.8净账](spill-and-compile-root-cause-plan-2026-10-28.md#58-正常路径的工作量表与退出标准)。约N次比较、S≤N行追加和N−S次端点更新/grow、control扫描/临时候选与独立复核仍存在；fixed结果按32N＋1536P_h及指针/heap/索引/undo收费。dense fixed可更大，以上不授compile收益或最终质量资格。

<a id="native-memory-consumer-layout"></a>

### 5.1.9 普通内存仍沿原真实消费者收敛

stage01～06已合；WB actual Cell1、窄Store Cell3及raw-only matcher各保真实合同。是否接管和是否覆盖完整负载分别记账，资格以主方案7.1为准；不授全P/MEM组合，不新增memory preparation或结果manager。

| 原文件 | 当前责任及下一真实退出 |
| --- | --- |
| `native.h/.cpp`、`select.h`、`select_memory.cpp` | 同Store actual程序/稀疏recipe，首次live前完成EA/Zero/narrow/lane/load与WB extra SSA。有效边Type/RawView和canonical definition分开，输出读Result。stage05早SSA对齐已随stage06退出，source quartet与actual memory schema保持；物理地址与Store数据分别按真实Cell1/Cell3读，不混用logical FindArgument。 |
| `constraints.cpp`、`constraints_gpr.cpp`、`constraints_fixed.cpp`、`constraints_fpr.cpp` | 原body/use/raw/terminal访问直接生产actual access/pool/ABI、GPR/residence/Derived-fixed/FPR。WB Get读末端直接取access、output version=true；无旧semantic一点修订。Explicit硬窗口独立准入。 |
| 公共`allocate/ties.cpp`、`gpr_writes.cpp` | 原actual clobber及late coalescer核produced/stored/write owner、canonical lease、fixed/REF/shared/observer窗口，保真实writer和REF；原候选×N与根查询仍收费。 |
| `select_vector.cpp`、`state_publication.cpp`、`state_widths.cpp` | actual lane/清高/load写、residence/Copy及最终state/width仍归原owner。无实际删除的晚DSE请求已经移除，baseline真实写保持；不能把初始输入直产理解为删掉运行state。 |
| `physical.h/.cpp`、`jit/translator_mem.cpp` | 同源access/extend/view/control/resource与TSO/fault；WB沿原Pre/Post kernel，selected Definition地址读真实X位置。stage06已沿原实际消费/Freeze接口交最终对齐与P数值事实，Get/alias只保至最后reader。 |
| `verify_gpr_native.h`、`verify_fpr.cpp`、`constraints_scratch.cpp` | 独立重读实际source/result/home/width/offset/backing/lease，WB借纯实际fixed定义reader；signed窄load写X不能误作W。最终alignment reader应与producer独立，不信资格位。 |
| `compiler.cpp`原Preclose/Storage | source门、单转换、first live/RA/state/proof/resources与精确冷恢复；stage06已在两个既有晚循环接producer/reader并退早hook/reader；默认true、15类trace退役及DecodedBlocks单scalar交接也归原Storage，不增扫描或manager。 |
| 原`native_program_test.cpp` | NativeRequest及原Block/Function fixture已迁真实actual候选/输入；dd0原owner2230/NarrowRead125通过；NarrowRead只补真实六参BeginNative，capture/partial/LowView断言不变。cee0 fixed/controller与红例保历史receipt，不合成当前全套，不重复helper质量矩阵。 |

阶段和资格见[主方案5.3.12.8](spill-and-compile-root-cause-plan-2026-10-28.md#native-memory-domain-status)；最终位置/P生命周期见[5.3.12.16](spill-and-compile-root-cause-plan-2026-10-28.md#native-final-location-alignment)。checked/Unknown合法HomeAlignment不随native早hook一同删除。

### 5.1.10 M1构造与空事件的责任仍归原写口

| 原文件 | 实际责任 / 资格 |
| --- | --- |
| `compile/builder/ir_builder.h`、`instruction.cpp`、原`rules/value_rules.cpp` | typed参数先于append，guest commit只续未完工作；两有限M1包已合，be原typed194/cancel17通过。32B PendingValue保FlagsIntent/Original和两个actual-op reader；大append体继续共享。 |
| `frontend/x86/decoder_alu.cc` | INC/DEC＋NOT少literal已合，be原decode flags例1058通过；窄/locked/miss的Value与CF/fault合同保持。普通五族立即数和PSHUFD已有，不重复计新增。 |
| `ir/ir_meta.h/.cpp` | 194条effects赋值唯一源已移至header constexpr，动态表与静态投影共用；只裁不可能事件，未增第二opcode清单。 |
| `compile/builder/guest_state.cpp`、`instruction.cpp`、`analysis/guest_state.h/.cpp` | 原真实forwarding/barrier/flag reader保留；只跳不可能事件和无规则fold。大append分配体按参数形状共享，raw/public及未完成continuation保动态读者。静态opcode不等于已知后端。 |

两份M1包已经合入并经受影响原例检查；组合ELF.text −50128B不作M1单包归因，也不授时间。target miss、模板/text及真实observer/未完fold继续收费，backend form不进入decoder。

<a id="native-actual-input-layout"></a>

### 5.1.11 actual输入直接生产已落地：原文件中的生产与退役

actual输入的原producer/caller已接，现行职责与退出工作见下表；代码路由不授完整负载覆盖或收益，资格统一见主方案7.1。不新增pass、input manager或第二operand程序。

| 原文件 / owner | 现行唯一职责 | 已退出的native工作 |
| --- | --- | --- |
| `compiler.cpp`原Preclose/Storage | source/SSA/CFG/纯桥与pool policy，最后Begin C；同槽转换Record C/P、全scope Close；实际static→live→Describe→原RA/state/proof；冷路按同owner恢复并重建actual | 语义GPR/residence/fixed预生产caller、flags全域修订/quality绑定，旧GPR计划与派生行快照恢复。默认true/诊断退役及DecodedBlocks单scalar交接已在这里完成；Return清零，实际支持域与消费前两类typed fallback保持。 |
| `constraints.cpp` | 原Descriptor逐body/use/raw/terminal访问交scratch/access、raw R/actual A与实际GPR/fixed/FPR输入 | 末端BindNativeGPRQuality及初始Derived修订；Explicit实际重资格保留。真正候选触发的全域closure未删除。 |
| `constraints_gpr.cpp` | C/P记录规范、actual publication/read/view/birth/窗口与实际用途聚合，完成原GPRInputs | native GPRInputBuilder语义预生产和semantic→actual绑定；checked原服务按caller保留，死native_topology已删。 |
| `constraints_fixed.cpp` | actual Derived各家族、Explicit ordinal/hard/capture/transfer与独立实际重资格；WB output为access上的version extra def | semantic RegisterState准备、Derived grow/repair与旧WB一点桥；纯实际fixed reader保持。 |
| `state_publication.cpp`、`state.cpp/.h` | 从实际C/P/R/A完成原residence/alias/Copy，转交真实lease/observer/terminal和完整Function域，结束临时候选 | semantic ResidenceBlock/Topology预生产/再校正与旧恢复字段；最终state实际fold仍保留。 |
| `select_scalar.cpp`、`select_memory.cpp`、`select.h` | 纯source资格及actual W-view/Get32/memory编辑先于live，真实body journal；source/private图匹配保持窄口 | 旧semantic GPR行驱动准入、view/Get32包装，旧DescribeWB/ReadFixed兼容接口；WB quartet/source/actual proof保持。 |
| `constraints_internal.h`、原公共`allocate/inputs.h` | 原owner的窄内部接口；ResidenceUse.actual_kind表达公共Numeric/Implicit/Terminal，空值只作raw边，40B/BlockRange64B静态断言 | raw count与actual lease混借、ARM form/Passed进入公共层；没有第二反向图或ready manager。 |
| 原`native_program_test.cpp` | NativeRequest用pool policy/Begin，Block/Function在真实转换Record/Close；断言在actual行真正产生的阶段读 | 测试助手预造semantic质量行、逐旧view包装断言及派生行undo；原hard/capture/压力/quality/retry合同保持。 |

费用仍含C/P规范/排序、raw/actual前缀共存与聚合、候选D触发真实N/E闭合、P×range/family、Explicit及冷重建；C/D可=N、R/A可=E，复用40B不等于没有流量。支持域默认开关已开；完整负载native覆盖率尚未建立；见[主方案实际输入](spill-and-compile-root-cause-plan-2026-10-28.md#native-actual-input-service)与[默认接管](spill-and-compile-root-cause-plan-2026-10-28.md#native-default-cutover)。

<a id="native-final-location-alignment-layout"></a>

### 5.1.12 已合最终位置RCpc的逐文件责任

stage06及原compiler两个晚循环已整合，旧组件before/after/compiler仅为历史reference，不能覆盖现caller或再次列作待合服务。[交接说明](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-memory-access-service-20261101/stage06-final-location-alignment/stage06-handoff.md)保原合同；当前构建/执行资格只见主方案7.1，不由组件结果外推全部P/MEM组合。

| 原源文件 / owner | 实现责任与最后消费者 |
| --- | --- |
| `compiler.cpp`（root两循环hunk） | BuildNativeCurrent后、PreparePhysical前形成对齐位/capture P；PreparePhysical后只填既有MEM-definition事实，Freeze后独立复核；不新增N pass；已整合到现actual caller，两次N级数值fold仍计。 |
| ARM64 `select.h`、`select_memory.cpp` | 两晚循环API及B-local 32 GPR normal/active low3数值状态，读取真实P/copy/lease；退stage05递归早查询。两次fold、alias追踪及栈状态真实收费。 |
| ARM64 `state_publication.cpp` | 移除stage05早alignment实现与对应journal/change bookkeeping，保scalar/state实际闭合。 |
| ARM64 `verify_gpr_native.h` | 移除早SSA对齐reader，要求晚producer前没有premature alignment位；原GPR/WB/no-copy/width/lease证明保持。最终low3独立reader在select_memory随Freeze循环执行。 |
| ARM64 `native.cpp`、`native.h` | 复用原MEM-definition P descriptor及晚reader API，延寿Get/whole alias字段到最后reader；原FreezeNative publication visit结束借用，不带Ready、不添N历史。 |
| 公共 `allocate/allocation.h` | 原preservation action两个尾padding字节存中立capture数值事实，action16B有静态断言；不改变capture/value/width/lease身份。 |
| 公共 `allocate/writer.h`、`writer_locations.cpp` | 原frozen action事实写口，核真实P/ordinal，不更换action/index/location；不引入新的allocation owner或证明表。 |
| ARM64 `physical.h`、`physical.cpp` | PhysicalValue两个prefix padding字节存真实MEM definition low3，新字段已参与四目标构建；本轮无独立sizeof/峰值结果，不写零开销；PhysicalResourceWriter同步原P action/current resource副本并保fact-aware去重。reader在真实P独立重算，再按value/source/slot/width/offset/lease消费。 |
| 原`jit/translator_mem.cpp` | 继续消费同源Aligned/AlignedBackedge与guard，不是本包新增kernel或源文件；ForceChecks/fault_emulate/16B/biased/page-table/未知/未证backedge保持，checked合法HomeAlignment不退。 |

P普通MEM定义须严格早于read，capture可依真实prefix同点；REF equality遇shared_write停止，shared lease不作值等价。AddCarry和helper恢复按真实kernel，padding写入/初始化/offset和两次数值fold全计费用。最后位置/P事实归backend，frontend仍只提交公共语义，见[目标边界7.7](target-aware-middle-pipeline-design-2026-10-28.md#77-实际输入直接生产保持后端边界)。

<a id="native-handoff-layout"></a>

### 5.1.13 同点交接、条件接管与物理身份的文件责任

这组修复继续使用第2节文件树，**不增加源文件、pass或包装层**。实际源与目标组织的责任如下；完整合同集中在[主方案5.3.12.18](spill-and-compile-root-cause-plan-2026-10-28.md#native-handoff-obligations)。

| 当前源文件 / 目标责任 | 工作与边界 |
| --- | --- |
| 公共`allocate/ties.cpp` → 目标assignment内的GPR tie规则 | 原LinearScan先读后写、旧end==新start的合法tie仍成立；原ABI bank/pool与真实hard/soft约束决定分配。不为proof或fixture改全局端点/政策 |
| ARM64 `verify_gpr_core.h` | publication核验只允许确证的最后incoming端点交接，P前所有fixed/inherited硬段照查；Checked view原合同保留 |
| ARM64 `verify_gpr_native.h` | 用真实write owner的id+identity、完整scope聚合LiveEnd及完成的actual-use枚举证明交接；terminal与Function live-out独立核，不把B-local LeaseEnd当global死亡 |
| ARM64 `select_scalar.cpp`、`select_flags.cpp` | 前者保原Select唯一/合法形态候选，后者在成功选择处转交条件消费并退休旧event/action；原Q逆向归属与最终graph同属原独立核验，不新建ownership表 |
| ARM64 `native.h/.cpp`、`physical.h/.cpp` | body/header保原Store身份，寄存器cell保final id/canonical/class/slot/view；MEM按真实transport需要保完整producer descriptor。不为所有寄存器扩一份raw origin |
| 原`tests/native_program_test.cpp` | 通过真实ABI/pool、Descriptor/RA和final definition/read建立见证，保same-home/完整scope/prefix/terminal与数值用途；MEM另核token。test-only修正不冒充已运行 |
| 原compiler/runtime/translator失败边界 | 临时定位变量、打印和body快照在基本执行恢复后、普通计时前删除；typed失败、消费后终止和精确metadata预算保留 |

write-owner/use/prefix、Q/raw窗口和最终独立graph仍计完整compile；迁移不固化逐query全scope发现，也不由未新增文件认领收益。历史正反例、实际guest及当前资格统一见主方案7.1。

<a id="canonical-raw-flags-layout"></a>

### 5.1.14 窄flags资格、成功交接与最终prefix证明仍归原文件

第2节目标文件树不增加目录或源文件；新问题按原写者闭合，不形成decoder资格表→actions→certificate的新层。详细合同见[主方案5.3.12.19](spill-and-compile-root-cause-plan-2026-10-28.md#canonical-raw-flags-obligations)。

| 现有源文件 | 负责的决定与退出条件 |
| --- | --- |
| `frontend/x86/decoder_alu.cc` | 窄legacy logical Save撤raw NZCV资格、保PF named-result；宽/deferred/FP不受一刀切。原无条件NZCV承诺退出，不加入ARM form/home或新资格缓存 |
| `frontend/x86/decoder_control.cc` | 保已有真实双后继dead-edge marker，canonical条件也可提交该证明；marker与raw资格分开，不在decode决定最终BranchOnly/物理来源 |
| `compile/passes/flags.cpp` | 原TryBranchOnly中闭合required全mask及primary→condition完整窗口（含AdvancePC后gap）；所有门成功才同RewriteContext将Save/CondSet一起交接。失败保canonical，不能依后端Reject兜合法输入 |
| `backend/arm64/compile/flags_graph.h`、`select_flags.cpp` | 原RawPstate/actual prefix visit在Bind meet后、Observe前独立核incoming全mask，含key/numeric；物理Carry可读性与canonical极性分开。精确Covered退休例外须已有独证，不添新表/扫描 |
| `backend/arm64/compile/select_scalar.cpp` | 已合逻辑立即数纯shape/实际W-X选择与初始独立位置核验；可选miss保Generic真实输入，不以语义位宽机械相等拒合法形态 |
| 原`jit/translator_flags_physical.cpp`、`translator_scalar_physical.cpp`及terminal消费者 | 保普通SaveResult canonical发表/恢复、BranchOnly SaveResult发表后真实NZCV加载、Generic/raw BranchOnly不发表及CondSetControl已有恢复合同；local终端只消费已证明raw条件。不增统一LocalCondSet reload或永久numeric/spill层 |
| 原`tests/cases/decode.cpp`、`branch_only_flags_test.cpp`、`native_program_test.cpp` | 原例分别覆盖资格、成功同节点交接与完整窗口、最后key/numeric读取；含AdvancePC后writer/fault及更早发表后fault。a326三个owning例共2993，后续owner各保原source，不合成当前整套 |

完整窗口、成功交接与incoming独证仍沿原frontend/pass/backend责任收口，旧候选缺口不重复列作当前故障。窗口/prefix的实际工作照计，资格统一见主方案7.1。

### 5.1.15 实际effect、VOID角色及冷publication继续收敛在原写口

| 现有源文件 | 唯一职责与后续收口 |
| --- | --- |
| `backend/arm64/compile/target.cpp` | 审计普通StoreMemoryTSO真实kernel保NZCV；guest状态/fault观察仍独立，不把全部memory泛化为透明 |
| `backend/arm64/compile/flags_graph.h`、`select_flags.cpp` | 原PstateWrite传defined/zero，原prefix保当前hardware-zero及Meet/覆盖/失效；实际S form/demand是资格，legacy Clear/Save沿同source合同。RawPstate 32→40B、LocalPstateEdge 48→56B及原访问成本照计，不立新图/表 |
| `backend/arm64/compile/constraints_gpr.cpp` | actual描述直接交原AllocationInputs；PinnedW由已有consumer-role写口授予，VOID store/publication不做producer size查询。保Low32 view及真实用途；该谓词修复不等于整个constraints已退役 |
| backend/arm64/jit/translator_mem.cpp、translator_scalar_physical.cpp与jit_context.cpp | publication借原ScalarX按role读取，Value不依赖pinned；Whole MEM沿原物化/reload，Low32/Definition保既有合同。R/X/W的原Whole ASSERT保留，不伪pin/改role |
| backend/arm64/compile/select_memory.cpp | GPRUseHome(any)只给候选；原Whole→Value须真实width/bits/home/no-reload合格，候选miss清residence并保Whole实际copy；explicit Value仍严格，坏身份/invalid不被吞。常态一query，miss多一次Whole解析；原MEM/高零及独证不变 |
| `backend/arm64/compile/state_publication.cpp`、`compiler.cpp` | 原state消费并实现publication；原compiler结束借用/回滚、同owner撤metadata、fresh输入/live/RA后重入。只进入retry不算全链成功，保精确typed失败与独立FPR预算，不再建恢复manager |
| 原`tests/native_program_test.cpp` | 扩已有owner最少见证：真实逻辑flags、TSO store、Low32唯一VOID use及冷恢复；按相依组检查，不为每helper建立QoR/PMU矩阵 |
| 原constraints/compiler/proof/runtime/translator定位口 | 临时失败/body定位及17项归因已退，ordinary catch保留；归因只服务算法选型，不成为永久manager或转授普通性能，资格见主方案7.1 |

旧冷publication拒口已经沿candidate binding与role消费修正，原失败不回填。当前资格以主方案7.1为准；后续按[净工作修正](spill-and-compile-root-cause-plan-2026-10-28.md#compile-net-work-correction)决定真实退出项，不由第2节文件树证明性能。

### 5.2 入口、owner 与持久结果

| owner/入口 | 合同 |
| --- | --- |
| normal Block caller | fresh构造前持真实guard，移交到owned compile；失败exact remove自己的未发布节点 |
| native Function caller | 完整actual-parent/Store group按固定顺序持锁；原Function分配；失败私有新组不发布 |
| public/retained Prepare | 无消费权或组外活语义借用则保输入；完整fresh组内部共持parent/多个Prepared不因此失权 |
| CompileUnit/Prepared | 原owner持phase/游标/唯一资源；ExecutionAccess持真实guards，最后同步释放 |
| JitTranslator/JitContext | native/physical当前参数、transport、scratch、pending spill、flags和同源kernel；动态Tick/物化在实际点处理。public/retained语义入口仍有Allocation/Actions context，不冒称全迁 |
| FunctionCodeObjectEmitter | 原whole-object布局/entry/edge、重发与发布资源；目标共读同一PhysicalReady，region失败完整重建ctx发false、不重绑定；最终Flush后结束真实程序租约。支持域默认借唯一PhysicalReady，source miss/retained保原IR路线；发布只读唯一成功image |
| code object/fault catalog/serialization | 自持持久entry/fault/SMC/link/身份；原撤回/retirement，不借临时IR/Facts |

跨线程可移交合法immutable结果，不能把原线程锁留到任意最后析构。Emit(true)结束不代表最后consumer；目标normal重发借同一PhysicalReady；支持域默认重发借同一PhysicalReady，不重绑定语义；source miss分支仍借原IR/Allocation/Selection，冷physical replay借其原物理流；语义public/retained继续借原IR/Allocation/Actions。持久metadata按各自最后消费者结束。

## 6. 前端、其它后端与编码文件

| 文件/域 | 职责 |
| --- | --- |
| `frontend/x86/guest_layout.*` | guest逻辑byte/alias，不含物理bindings |
| `decoder.h/.cc` | 一次parse、owned bytes/PC、窗口/drain、提交/失败；不扩大C2窗口作为新性能路线 |
| `scalar_demand.h/.cpp` | 当前有限纯GPR byte/flags需求；不拥有另一解析队列 |
| `decoder_*.cc`、`vex_decoder.*`、`x87.*` | 真实guest ISA lowering，typed提交与原边界 |
| `frontend/arm64/arm64_frontend.*` | 自身guest语义，共用公共builder |
| ARM64 `jit/translator*.cpp` | 保真实编码、动态物化/transport/flags和offset；native与真实physical replay接193个typed kernel；public/retained语义入口及旧handler仍有真实读者，待同源迁移后才能删 |
| `jit/function_code_object_emitter.*` | 多块布局、入口/边、重发和发布资源；目标每attempt重建原ctx/ASM并共读唯一PhysicalReady，只有最终image进入Flush/publication，不增加第二code-object owner或片段布局层 |
| ARM64 `helpers/` | 宿主实现/ABI，不倒挂到frontend |
| `backend/interp/`、`backend/riscv64/` | 实际公共语义消费者及自身合法化/分配/恢复，不接ARM64 physical form |

`translator_*_physical.cpp`与旧handler相同机器码逻辑只允许迁移期并存；按真实family共用kernel，不再造一份op/operand图。原Lsl/Lsr/Asr等shift的同源迁移已完成，不重复认领；新ScalarShift整包已撤回，不属于下一输出迁移。Unknown保可执行generic；Generic V256不因physical overflow存在就自动成为ARM64合法能力。

<a id="61-当前紧凑移位候选的文件落位"></a>

### 6.1 已撤回的紧凑移位候选

78f曾在29个文件贯通公共语义、builder、x86 decode、effects/flags、解释器、ARM64 target/scratch/state/select/physical及同源JIT，并扩充三个既有cases和physical-consumption；只新增公共`shift.h`，没有新建preparation/actions/certificates层。必要语义通过，但完整方向B−0.04%/F−2.32%不支持新增公共opcode/profile/wire的维护成本，整包已撤回。

`shift.h`和ConditionalShift不留在本树，原193opcode及profile恢复。完整源文件清单及最终实现保存在[最终候选](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/final-qualified-candidate/full-from-f7e6.patch)；其职责表是候选历史，不成为未来文件/接口要求。[完整决定](spill-and-compile-root-cause-plan-2026-10-28.md#5211-当前完整候选shlshrsar直接公共语义)。

## 7. 当前→目标映射与删除条件

### 7.1 公共 IR 和分析

| 当前来源 | 目标去向/删除条件 |
| --- | --- |
| `hir_builder.h/.cpp`中的HIRFunction/HIRBlock/Edge与Store/Uses/EditBinding | 移到`function_ir.*`；原builder只留构造。旧类型声明/实现迁完，不复制新图 |
| `analysis/facts.cpp`、`semantic_facts.h` | 归`semantic_facts.*`；`facts.h`中的BlockUseEnds/FlagsDemand分别归live/flag数据。全部包含者迁完后删旧文件 |
| `analysis/uses.h`、`terminal_uses.*` | 归`roots.*`，保原counted/implicit/membership差别；不是替代`ir/uses.*` |
| HIRValue/HIRUse/pools、旧完整Actions捕获、native origin覆盖 | 已退出正常路径，不重开同义项目；当前最终N退役仍有真实工作，不能称全部尾扫消失 |

### 7.2 分配器按函数迁移

| 当前文件 | 目标去向 |
| --- | --- |
| 原Inst/Facts查询散布于RA | 新`program.*`中立借用协议；同LinearScan读实际uses/defs，旧semantic适配只供checked入口，不复制算法 |
| `allocation.cpp`、`controller.h`中的入口/retry | `allocator.*`；结果表/view/释放实现与原writer中的对应函数归`allocation.*` |
| `internal.h` | `scan_internal.h`，保私有状态，不导出调度层 |
| `linear_scan.cpp`、`intervals.cpp` | 按范围进`schedule_block/function`，共同assignment进`assign.cpp`；不能整删Function的合法interval路线 |
| `gpr.cpp`、`fpr.cpp`、`fixed.cpp`、`ties.cpp` | 按真实函数进`assign_gpr/fpr/fixed`和对应schedule；`gpr.cpp`的GPRCopyCoalescing、CoalesceGuestGPRReads进`coalesce_gpr`，CoalesceLow32Copies进`coalesce_width`；future读端点/tie/压力保合同 |
| `gpr_writes.cpp`、`coalesce.cpp`、`width.cpp` | 按实际策略进`coalesce_gpr/width`；没有替代算法不能仅因重命名删除 |
| `locations.cpp`、`writer.cpp`、`writer_locations.cpp` | 查询、owning结果和唯一mutation按第4节拆清，无新结果owner |
| `home_index.*`、`preservation_index.*` | 必要fixed/preservation查询归`location_indexes.*`，保各自事实与失效；`placement_index.*`已撤回/不存在，不列来源、目标或前置 |
| `spill.cpp`、`spill_regions.cpp` | 前者保spill决策，后者的region/backing归`reloads.cpp`、TryRepairLocalGPR及Writer::SpillLocalGPR归`repair.*`；不在writer中另留一份修复流程 |
| `transaction.*`、`recolor.cpp`、局部repair函数 | 同一`repair.*`；仅局部实际触及数据，不全结果snapshot |
| `inputs.*`、`constraints.*`、`fixed_validation.h`、`fixed_verify.cpp` | 分别归inputs、contracts、verify；attempt/Complete责任显式保留 |

### 7.3 plan* 和双编码链

| 当前真实内容/读者 | 最终去向 | 删除条件 |
| --- | --- | --- |
| `plan_build*`等中的请求相序/失败/Prepared职责 | compiler/session；成功输出直接在同Store native程序中，Selection仅保稀疏候选/unit信息 | 原公开准备与正常调用者全迁，不把PlanBuilder原样搬成internal就称退役 |
| `constraints_gpr.cpp` | family读写/view/早写候选归`select_scalar/memory/control`，guest publication/版本转移归`state_publication`；公共tie安装归allocate输入/合同 | 全部normal与public checked读者改用同源规则；删旧GPRInputBuilder投影及CMake/include |
| `constraints_fpr.cpp` | FPR/tuple/alias形态归`select_vector`及相应scalar/helper规则，中立pool/tie/future-read安装归`constraints`与公共allocate | 真实FPR quality/admission读者接入actual machine合同后删旧builder；不能因删文件丢coalescing |
| `constraints_fixed.cpp` | opcode实际fixed/extra写语义归对应select schema，helper ABI/clobber公共原语归`constraints`，最终实际位置检查归`verify` | result replacement、独立clobber/writeback及public检查全迁后删旧语义再描述 |
| `constraints_scratch.cpp` | family scratch种类/形态归对应select schema，ABI/profile公共原语归`constraints`，资源安装仍直交原scratch owner，最终检查归`verify` | 当前TakePreparedScratch已是move，不再计一遍“免拷贝”；所有consumer接同源合同后删独立builder |
| `constraints_internal.h` | 仅保真实select/constraints私有声明于其现header；旧GPR/FPR/fixed/scratch builder声明随上述实现退出 | 最后reader/include/CMake迁完才删除，不原样藏入compiler_internal或新增prepare层 |
| Consumer/IntegerFlags/ZeroExtend临时solver数据 | 对应constraints/state/select原调用寿命 | 最后实际matcher/核验读者迁完；不再生成Ready Actions/certificate |
| 成功稀疏选择、retained路径的真实选择 | 同一select输出与同源规则 | public/retained仍合法非消费；不为删接口赋伪消费权 |
| 最终form/schema、资源和实际roles | native；共有metadata/冷replay归physical | 每次emit不再由UnitActions查询/转译同一form；无第二全程序 |
| `plan_identity*`等的身份、生产核验和audit | 粗环境身份归compiler/target，持久身份归code object，核验归verify/audit | 不再建certificate manager，不以候选自证；缓存身份按真实格式/语义决定 |
| 相同retained/physical机器码逻辑 | jit同源寄存器级kernel | 真实binding/先读后clobber/Tick/fault差异仍保；旧重复实现无读者后删 |

编译器API目前仍暴露PlanBuilder/UnitActions，不能称plan已无消费者。所有旧文件退役同时清CMake、include、字段、转换、开关和夹具；不长期养完整双链。

## 8. 实施与验证范围

本树不先于算法搬迁。目标文件名未落地时修改其第7节真实来源；文件随生产者和最后消费者一起收口。Block stream/Function Word及codec调参已停止，3ef输出/重发是基础。

| 工作线 | 主要修改域 | 必须实际退出的旧工作 |
| --- | --- | --- |
| M1构造/公共解释 | ir.inc/ir_meta、instr/args、builder/rules、uses/rewrite、Facts及target；必要反馈 | 少出生节点/边；第74批公共字段替代不重认领；保真实首次服务，不另缓存N字段 |
| M2＋M3跨阶段位置服务 | 原constraints、两范围assignment/Writer/repair、state/select/verify、当前binder/resource | 同Store BackendVirtual→PhysicalReady贯通两scope machine live、RA、state/独立check与kernel；旧Describe/Complete/每emit binding真实退出 |
| M1条件性DCE/flags | dce/flags/FlagFlow、真实编辑/出生/根写口 | 变化后的重复全扫/无关求解；首轮seed/固定点保留，不夸大为构造主线 |
| M4请求/发布/结束 | workspace/Storage、Allocation、runtime/code-object及真实metadata | 无消费者准备/复制、空需求复杂状态；初始化/触达/清理分列，不只提高缓存预算 |
| 已完成输出基础 | compiler/physical/jit/context/emitter | 复用3ef真实模式重发机制与typed kernel；新的同Store程序只有上游服务同时退出才成立，不再单开格式微调 |

M1构造/首次服务与M4存储触达须和后端减工共同完整交付，不能仅以原语保留或缓存复用代替净工作。先形成可运行真实纵向整包，最少受影响语义后做少量完整反馈；未覆盖域保合法入口并计总成本，不等全族、不逐helper铺QoR。顺序见主方案7.2，最终门仍在6.3/6.4。

当前源码依据：[实际IR owner](../source/runtime/ir/hir_builder.h)、[Store](../source/runtime/ir/store.h)、[用途](../source/runtime/ir/uses.h)、[pipeline](../source/runtime/compile/pipeline.cpp)、[allocator](../source/runtime/compile/allocate/allocation.cpp)、[writer](../source/runtime/compile/allocate/writer.h)、[Complete](../source/runtime/backend/arm64/compile/compiler.cpp)、[actual physical](../source/runtime/backend/arm64/compile/physical.cpp)、[emitter](../source/runtime/backend/arm64/jit/translator.cpp)。目标文件未迁前，仅作为目标名称列示。

中途统一owning构建和最少受影响语义；完整算法一次方向；最终集中质量/资源/两范围fresh固定FEX门。最终验收只在主方案6.3/6.4维护，原失败和未知状态保留。

## 9. 版本与证据

8cda2889为当前源码检查点：A产品已合，translator与swift_ir_test两owning目标构建成功；三个原case为2过1失败、3714断言中3713通过。public owner和fixed-suffix整例通过；新增NarrowRead负例在绑定时继承旧源专属placement而失败，尚未到stale-count检查，不记为产品回退或全绿。外部02只修该fixture，未应用、构建或执行；8cda没有新的ordinary计时。

e0d6仍为最新普通完整四点：Block 2066.780ms、Function 1099.290ms，各自fresh固定FEX为46.624/47.297ms，约44.33×/23.24×。均CRC通过，Function原失败/Block重解码fallback已计入总时间；这是开发方向，不授最终质量、完整生命周期或七对采用资格，ordinary实际NativeReady覆盖仍unobserved。

3625的独立Block＋完整Function归因已闭合：native GPR proof分别695.636/103.768ms，占各自native admission约88.77%/89.41%；这是A前诊断版，不是ordinary比值，也不是A全部可省时间。A保留的provenance/window/CFG/fault/capture与RawDirect成本仍在。详细身份、原失败、统计端点及后续结果只以[主方案7.1](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)为单一入口；[归因原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/native-admission-3625-summary-01/summary.json)、[A构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-consumer-read-proof-build-01/build.json)和[A原例](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-consumer-read-proof-minimum-01/execution.json)保持独立资格。本轮仅修文档，goal保持paused，不应用02、不构建或上机。
