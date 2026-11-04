# ARM64 编译流程：实际目录、接口与逐文件职责

日期：2026-10-28。本文记录已落地的源码组织，与[整改主方案](arm64-compile-pipeline-consolidation-plan-2026-10-28.md)第 4 节对应。目录/API 及数据流收敛已实施，最终版本已完成本轮验收。

后续拟实施的完整中间流程结构见[最终文件结构与退役映射](middle-pipeline-file-layout-2026-10-28.md)。本页继续记录实际r7，不把未来文件当作已落地；后续保留真实physical/验证职责，逐规则前移语义，不重复计算本页已经完成的删除。

最终 `layout-r7` 已构建全部16个Android目标，既定检查、151个扩充用例、33次Function对照及30项独立lifetime检查均完成；22条历史断言失败逐项保持，无新增失败。实际FP缓冲区字节一致，Function逐对象代码大小与同轮普通构建基线一致。完整范围及限制见[实施进度](arm64-compile-pipeline-consolidation-progress-2026-10-28.md)，不报告新的 FEX 性能差距。

## 实际边界

根目录为 `source/runtime/backend/arm64/compile/`。当前 [CMakeLists.txt](../source/runtime/backend/arm64/compile/CMakeLists.txt) 列出 52 个 C++ 源/头文件：12 个公开合同头、3 个私有头和 37 个实现文件，构建清单本身另计。下表逐一对应这 52 个路径。这里的“公开”指责任域间必需的合同，不表示新增应用 API。

文件数不等于 pass 数、遍历次数或性能结果。共同输入遍历、依赖不同物理事实的选择、最终验证及动态发射仍按真实依赖编排；同一算法也可以因实现规模保留多个私有源文件。

`compiler` 管入口、重试和所有权；`plan` 管唯一稀疏最终结果与受限写口；`target` 管目标规则。selector、registers 和 verify 各域有单个必要公开头，输入 builder、证明角色和临时索引不再各自形成一套公开 inputs/preparation/actions/certificates 接口。

## 公开合同与私有头

| 文件 | 职责与边界 |
| --- | --- |
| [compiler.h](../source/runtime/backend/arm64/compile/compiler.h) | Block、FinalBlock、Function 与批量 Function 的正常入口，`Arm64PreparedUnit` 所有权及 standalone 准入；`PlanBuildResult` 区分 Ready、Rejected 和物理重试。 |
| [plan.h](../source/runtime/backend/arm64/compile/plan.h) | 唯一最终 owner `UnitActions`、必要稀疏 row、coverage 借用、`UnitCertificate` 与受限写口 `PlanBuilder` 的同一合同。 |
| [target.h](../source/runtime/backend/arm64/compile/target.h) | ARM64 能力、立即数编码规则、guest-state/hoist 策略、观察与 clobber 规则，以及可直接查询的目标事实。 |
| [select/scalar.h](../source/runtime/backend/arm64/compile/select/scalar.h) | scalar copy/narrow 选择的必要输入与临时结果；借用最终 integer-flags 段供 narrowing 消费。 |
| [select/vector.h](../source/runtime/backend/arm64/compile/select/vector.h) | vector/scalar-FPR 选择的必要结果及 `PrepareVector`、`SelectPshufdDirect` 入口；私有 proof-role/owner 不再公开。 |
| [select/memory.h](../source/runtime/backend/arm64/compile/select/memory.h) | memory override 的选择入口和同一最终 payload 上的临时角色索引；声明 view 在 append/sort 前后的有效期。 |
| [select/flags.h](../source/runtime/backend/arm64/compile/select/flags.h) | carry、token、consumer、predicate、integer flags 和 branch 的必要合同；保留不同 coverage 阶段的真实依赖。 |
| [select/control.h](../source/runtime/backend/arm64/compile/select/control.h) | CFG/edge/backedge/PFAF、atomic cohort、共享调用、延迟 location 与 terminal helper 的选择合同。 |
| [registers/constraints.h](../source/runtime/backend/arm64/compile/registers/constraints.h) | Block/Function 分配请求描述、fixed 约束及 shared matcher 合同；区分只读既有请求与创建新请求的入口。 |
| [registers/state.h](../source/runtime/backend/arm64/compile/registers/state.h) | `RegisterStatePreparation`、`ResidencePreparation`、`GPRResidences` 及 W-write 查询；约束 guest-state 与物理 home 的使用窗口。 |
| [registers/resources.h](../source/runtime/backend/arm64/compile/registers/resources.h) | 固有预算、固定 clobber、pool 策略、静态 scratch ranges 和 exact MEM transport 的资源合同。 |
| [verify/verify.h](../source/runtime/backend/arm64/compile/verify/verify.h) | 统一生产验证声明、fixed 描述、width 复证、FPR admission 与独立 audit；保留有真实消费者的 FPR 诊断查询。 |
| [detail/compiler.h](../source/runtime/backend/arm64/compile/detail/compiler.h) | 私有 `CompilerSession` 的编排状态与跨实现文件成员声明；不成为第二个最终 owner。 |
| [registers/detail/describe.h](../source/runtime/backend/arm64/compile/registers/detail/describe.h) | 共同 IR 输入遍历真正使用的 GPR/FPR/spill 私有 builder 与 scratch 描述声明；不向 selector/JIT 暴露输入构造细节。 |
| [registers/detail/write_successors.h](../source/runtime/backend/arm64/compile/registers/detail/write_successors.h) | preallocation 与 publication 共用、但 observer/full-write 策略不同的模板索引；保留实际算法和测试消费者。 |

## 实现文件

### 入口、最终结果与目标规则

| 文件 | 工作 |
| --- | --- |
| [compiler.cpp](../source/runtime/backend/arm64/compile/compiler.cpp) | Block/Function 入口、物理重试、PreparedUnit 的 IR/Allocation/plan 所有权和最终准入；不承载各域逐指令 matcher。 |
| [pipeline.cpp](../source/runtime/backend/arm64/compile/pipeline.cpp) | 同一个 `CompilerSession` 的依赖顺序：FPR admission、GPR edits、各 selector 的原覆盖顺序及最终验证。 |
| [plan.cpp](../source/runtime/backend/arm64/compile/plan.cpp) | 最终记录查询、coverage、唯一 phase、reset 与一次封口；整理最终稀疏表并退役已消费的旧标记。 |
| [plan_build.cpp](../source/runtime/backend/arm64/compile/plan_build.cpp) | `PlanBuilder` 的受限写口、物理编辑/abort 边界、完整 buffer 接管和必要借用 span 的形成；在所有 backedge 已提交后计算静态入口闭包再封口。 |
| [plan_identity.cpp](../source/runtime/backend/arm64/compile/plan_identity.cpp) | CFG/成员/入口/provenance 深合同及最终 edge/atomic 数据查询；不以 GPR generation 代替 IR 身份检查。 |
| [target.cpp](../source/runtime/backend/arm64/compile/target.cpp) | 目标能力与编码规则、guest-state/hoist 策略及观察/clobber 规则的实现；不复制普通 IR。 |

### 选择

| 文件 | 工作 |
| --- | --- |
| [select/scalar.cpp](../source/runtime/backend/arm64/compile/select/scalar.cpp) | scalar copy、narrow/extend 和唯一零扩展跨节点选择；普通 copy、扩展和已确定物理位置的局部编码留在 JIT。 |
| [select/vector.cpp](../source/runtime/backend/arm64/compile/select/vector.cpp) | 共享 vector 候选投影、scalar-FPR publication/resident 选择和 direct-shuffle 选择；物理正确性复证另属 verify。 |
| [select/memory.cpp](../source/runtime/backend/arm64/compile/select/memory.cpp) | memory payload 的直接构造与临时角色借用、access override 选择及直接写入公共物理 witness；最终 coverage/backedge 后分析静态对齐，写同一稀疏记录的两种激活结果。 |
| [select/memory_updates.cpp](../source/runtime/backend/arm64/compile/select/memory_updates.cpp) | memory update/address/value 窗口匹配；保留首个可编码候选被窗口拒绝后回退的原规则。 |
| [select/flags.cpp](../source/runtime/backend/arm64/compile/select/flags.cpp) | carry 与 NZCV/parity token 的用途窗口和最终 row 生成；动态 token 消费进度仍由 emitter 维护。 |
| [select/consumers.cpp](../source/runtime/backend/arm64/compile/select/consumers.cpp) | consumer/PSTATE 和可选 ALU audit 选择；临时 PSTATE 在 carry/incoming-zero 等最后消费者结束后退出。 |
| [select/integer_flags.cpp](../source/runtime/backend/arm64/compile/select/integer_flags.cpp) | 整数 flags、窄输入、compare/carry 等专门匹配和依赖证明。 |
| [select/predicates.cpp](../source/runtime/backend/arm64/compile/select/predicates.cpp) | predicate/condition/branch 选择、候选窗口和对应覆盖。 |
| [select/control.cpp](../source/runtime/backend/arm64/compile/select/control.cpp) | 函数共享调用、延迟 location、atomic cohort 及局部 terminal helper；实际 label、veneer 和 host offset 属于发码。 |
| [select/edges.cpp](../source/runtime/backend/arm64/compile/select/edges.cpp) | 单元 CFG 边、backedge 与 PFAF 的选择及透明后继静态入口闭包；实际 storage/ABI 激活仍属 JIT，不混入代码安装/retirement。 |
| [select/edge_flags.cpp](../source/runtime/backend/arm64/compile/select/edge_flags.cpp) | 边 flags 透明性、token overwrite 和 incoming NZCV acceptance 的分析。 |

### 寄存器约束、状态与资源

| 文件 | 工作 |
| --- | --- |
| [registers/constraints.cpp](../source/runtime/backend/arm64/compile/registers/constraints.cpp) | 一次共同IR语义描述，编排GPR/FPR/spill/scratch能力；Prepare直接初始化唯一fixed writer，不再产普通fixed模板。 |
| [registers/fixed_constraints.cpp](../source/runtime/backend/arm64/compile/registers/fixed_constraints.cpp) | 分配前fixed guest-state/capture/遗留early-home直接提交AllocationWriter；精确soft候选跳过重复hard proposal，successor按需一次构造。保Function原全unit优先级和批提交，无InitialConstraints影子owner。 |
| [registers/gpr_constraints.cpp](../source/runtime/backend/arm64/compile/registers/gpr_constraints.cpp) | GPR、width 与 publication 的候选和偏好形成；不持有另一份最终省略决定。 |
| [registers/fpr_constraints.cpp](../source/runtime/backend/arm64/compile/registers/fpr_constraints.cpp) | FPR destructive tie、publication 和相关输入约束形成；不预言尚未分配的 home。 |
| [registers/state.cpp](../source/runtime/backend/arm64/compile/registers/state.cpp) | guest-state/fixed-home 状态与 fault capture，以及 `WWriteRoot`/`HasKnownWWrite` 的唯一查询定义。 |
| [registers/versions.cpp](../source/runtime/backend/arm64/compile/registers/versions.cpp) | guest-state version、incoming/use 版本及 early-clobber 等合同。 |
| [registers/widths.cpp](../source/runtime/backend/arm64/compile/registers/widths.cpp) | home 的宽度/extension 事实及 Function 入口宽度传播；与最终物理 width 复证分属不同职责。 |
| [registers/residences.cpp](../source/runtime/backend/arm64/compile/registers/residences.cpp) | 同一 residence builder 的 home/read 与 publication 工作索引；纯 transfer matcher供前置分段约束，后置 transfer 重选已退役。 |
| [registers/publications.cpp](../source/runtime/backend/arm64/compile/registers/publications.cpp) | `SetHostGPR` 的最终 guest-state publication，以及 coalesced/zero-extension publication 的证明。 |
| [registers/publication_copies.cpp](../source/runtime/backend/arm64/compile/registers/publication_copies.cpp) | guest-state copy publication 与其 alias 使用窗口。 |
| [registers/publication_values.cpp](../source/runtime/backend/arm64/compile/registers/publication_values.cpp) | Select/low-view publication；相邻 spilled scalar 直接进入目标 home 已移至公共 RA，后置 Spill 决策链已退役。 |
| [registers/publication_windows.cpp](../source/runtime/backend/arm64/compile/registers/publication_windows.cpp) | publication 的观察窗口、完整写边界与首次冲突查询。 |
| [registers/resources.cpp](../source/runtime/backend/arm64/compile/registers/resources.cpp) | 固有 GPR/FPR scratch 预算、固定 clobber 与 pool 策略，供 allocator 和 emitter 共用。 |
| [registers/scratch.cpp](../source/runtime/backend/arm64/compile/registers/scratch.cpp) | 最终静态 scratch ranges 与 exact MEM transport facts；不获取实际 lease 或维护 pending spill 进度。 |

原`registers/spill_transport.cpp`及SpillInputs的sites/segments/blocks已整份退役。五个region能力位与独立forward-kind在现`target.h/.cpp`纯分类，generic allocation的原MEM扫描现场处理control位置/真实映射，ARM scratch只取forward-kind；不新建transport plan或段表。两组能力保各自旧域，unknown/null保原回退。

### 生产验证与独立审计

| 文件 | 工作 |
| --- | --- |
| [verify/verify.cpp](../source/runtime/backend/arm64/compile/verify/verify.cpp) | fixed/width 生产检查编排、零存储与 flags-only-spill 检查；不重新实现 selector。 |
| [verify/widths.cpp](../source/runtime/backend/arm64/compile/verify/widths.cpp) | 基于实际 home、owner、W-write、clobber 与 terminal use 复证最终省略；通过受限口直接认证。 |
| [verify/fixed.cpp](../source/runtime/backend/arm64/compile/verify/fixed.cpp) | fixed 语义描述及已绑定输入复用；正常请求复用原描述，手工/不匹配输入显式描述。 |
| [verify/fpr.cpp](../source/runtime/backend/arm64/compile/verify/fpr.cpp) | 同一个 `FPRAdmissionVerifier`、必要候选/owner/observation 工作集、生产直写与诊断输出；维持全单元 admission 顺序。 |
| [verify/audit.cpp](../source/runtime/backend/arm64/compile/verify/audit.cpp) | optional 独立参考审计；不读取 production checked bits 作为独立 oracle。 |

## 数据如何流经这些文件

1. `registers/constraints.cpp` 在共同输入遍历中描述目标约束，复用公共 SemanticFacts；`DescribeAllocationInputs` 保持既有绑定和映射，`PrepareAllocationInputs` 先建立新请求，二者失效语义不同。
2. 公共 allocator 继续分别执行 Block 与 Function 分配。单块 Function 的局部 fast path 保留原准入条件，不把 Function 静默改为独立 Block。
3. `pipeline.cpp` 先完成所有 block 的 FPR admission，再开始 GPR edits 和依赖 mapping 的选择。FPR 生产输出直接写入私有最终 owner；诊断 `FPRContract`/`PrepareFPRContract` 保留真实查询用途，不参与生产 instruction 表回灌。
4. 各 selector 按原覆盖优先级提交。完整 buffer 在最后一个真实消费者结束后移动；integer-flags span 借用刚存入的最终段，narrowing 在下一次 append、reset 或最终排序前结束借用。
5. 最终 fixed/width 等 checker 从实际 IR、Allocation 和合同复证，直接认证同一私有结果。封口只保留最终消费者需要的记录；独立 audit 保留独立参考。
   最终 coverage 和 backedge 已稳定后，memory 对齐分析按原 Track-before-dispatch 顺序执行；必要时惰性分出实际 backedge 激活后的状态，只有改变默认访存展开的结果进入原 memory owner。封口时共享透明后继的静态入口计算，实际激活条件不提前固定。
6. [compiler.h](../source/runtime/backend/arm64/compile/compiler.h) 中的 `Arm64PreparedUnit` 持有 sealed IR、Allocation 与 `UnitActions`。正常 Function 发码通过 [FunctionCodeObjectEmitter](../source/runtime/backend/arm64/jit/function_code_object_emitter.h)借用已有 prepared 序列，借用期保持序列及单元存活且不移动，覆盖 emission、direct-link fallback 和 flush。
7. 发码按原 IR、最终物理位置和额外稀疏决定执行。实际 lease、pending spill 消费、label、host/fault/relocation offset 属于发射时状态；发布时的 guest code、映射、SMC/retirement 有效性检查仍独立。

`UnitActions` 是唯一最终 owner；原 `AllocationPreparation` 直接改名 `PlanBuilder`，原 `TargetPreparation` 直接改名为私有 `CompilerSession`，没有并行 owner 或兼容别名。`PlanBuildResult` 保留 Ready、Rejected、RetryWithoutFPRCoalescing 三类结果。

目标准备的唯一 phase 为 `Empty → AdmittingFPR → FPRAdmitted → Planning → Ready`。它不替代 Allocation 的物理编辑、冻结、generation 和 attempt，也不替代 `UnitCertificate` 的有效性；对外是否可发码仍由 compiler 的完整准入判断。

## 已退役的重复链路与保留的合同

| 对象 | 当前归属与保留义务 |
| --- | --- |
| width 三个 proof-ID vector、拼接和逐 ID 回灌 | `verify/widths.cpp` 保留实际 final 检查，直接认证唯一 owner；checked bit 不作为复证依据。 |
| To32/To64 最终零扩展决定的旧 residence/tied/narrow 扩展标记 | `select/scalar.cpp` 形成必要的 `ZeroExtendAction`；准备期和其它 opcode 的真实消费者保留。普通 copy/扩展、确定 home/view 的本地 transport 不另建同义行。 |
| 生产 FPRContract instruction 投影与 CertifyFPR 导入 | 生产 admission 只返回状态并直写最终 proof/control；诊断查询表与同一实际 verifier 保留。 |
| memory 同类型 payload 的复制与重复排序、emitter 静态 recipe 重建 | 记录由唯一 owner 保存，准备期角色索引借用同一 payload；实际“EA 已省略”进度、pending spill 和 exact consumer 检查保留。 |
| emitter 的静态 home-alignment map、地址链推导与 Track 钩子 | 最终 coverage 后的 memory selector 负责分析；同一 `MemoryAction` 保存普通/实际backedge激活两种省略事实，emitter 只查询，并保留实际检查、fault 路径和 scratch lease。 |
| emitter 重复执行的透明 CFG 入口链遍历 | 封口时直接写已有 `EdgeBlockAction` 的一个 bit，共享后缀的临时工作集随后退出；原 `CanImportBackedgeFlagsRecipe` 晚门保留。 |
| flags-only spill 的 EmissionCertificates 返回/导入容器 | 最终 checker 直接记录结果；零存储每块临时 vector 退役，全单元 pending IDs 保留到原认证时点。 |
| MemoryActions::Write 到公共 witness 的转译 | memory 选择直接写公共 PhysicalGPRWrite，保留完整 instruction、consuming-use 和 home 身份。 |
| CarryActions、FlagsTokenActions 纯包装及 buffer 逐行复制 | 直接返回/接管最终 row vector；copies、PSTATE、vector publication/resident 等有真实后续消费者的临时结果按各自寿命保留。 |
| 多份 reset 清单与 ready/building 组合 | 同一 owner phase 与 Reset；物理编辑状态仍由 Allocation 维持，失败和重试不消费私有半完成结果。 |
| actions/admission/allocation_preparation 及各域旧公开路径 | 实际声明进入上表责任域头，旧路径不留转发头或兼容 alias。UnitCertificate 深合同与 optional audit 没有因此删除。 |

allocator 内部 width-chain/owner、spill/reload/preserve 及其真实消费者仍是 [Allocation](../source/runtime/compile/allocate/allocation.h) 的计算与验证状态；不能因最终省略决定收敛就把这些状态一并清除。

最终对齐分析在排序封口前运行一次。它按 unit/RPO 和 IR 顺序消费原 memory 记录前缀，用偏移避免 append 使引用失效，新的对齐专用记录追加到同一 vector；稀疏或非单调 ID 不改变这个顺序合同。修改记录生产顺序时须同步更新该合同，不增加一份最终索引或提前重复排序。

## 实际拆分与维护位置

- `width_contracts.cpp` 中的跨节点零扩展选择移至 `select/scalar.cpp`，独立复证归 `verify/widths.cpp`。`registers/widths.cpp` 管 guest-state 宽度事实，二者不是同一工作的两份实现。
- `residences.cpp` 的 guest-state publication 证明移至 `registers/publications.cpp`，memory update/address/value 窗口归 `select/memory_updates.cpp`；publication 的含义是 guest-state 写回，不是 JIT 代码安装。
- `WWriteRoot`/`HasKnownWWrite` 从 GPR 输入构造中移到 `registers/state`；GPR/FPR/spill 私有输入 builder 归 `registers/detail/describe.h`。
- vector proof-role/owner 和 FPR 证明工作集随真实算法留在各自 cpp；direct-shuffle 选择归 vector，物理 FPR admission/诊断归 verify。
- target/effects、vector 两实现、FPR 准备与 checker、memory 记录与选择、state/capture、residence/read 及 control 小实现已按职责合并；不同算法规模、寿命或最后消费者仍可保留私有拆分。

修改一种形式时，先判断它是原 IR/最终 Allocation 可直接决定的局部编码，还是需要额外跨节点选择；再修改对应 selector、合同和 emitter。不能为普通形式逐条新增 plan row，也不能为减少文件数合并有真实依赖的阶段。本轮行为、生成代码和生命周期验收已按主方案第 6 节完成；后续变更继续遵守同一标准。
