# 编译流程重建：紧凑 IR、公共优化与直接机器提交

2026-11-02，按用户最新纠正重置路线。本文件取代旧框架 v2 的增量优化顺序。旧源码、合同和证据已封存，不能继续把旧链补丁称为框架重建。

2026-11-03 后续目标见[生成质量追平 FEX 方案](codegen-parity-plan-2026-11-03.md)：GPR 保留中立 Uniform IR，compile 各 scope 仍≤2×，生成质量目标提高为追平 FEX。第34节是已测基线，不代表新目标已达到；最新实现和测量见[实施进度](codegen-parity-progress-2026-11-03.md)。FaultSite已为32B，Home仍16B，首次eviction的20B Home修订尚未实施。

## 1. 当前边界

施工树：`/Volumes/Data2/SwiftVM-rebuild-20261102`。原仓库、旧工作树、Git/WIP及全部证据保留。旧26版只完成构建，未运行设备测试或性能比较，不再沿该路线推进。

新树已移除旧 `runtime/ir`、`runtime/compile`、`backend/arm64/compile` 实现，共176文件。只保留中立opcode声明表作为新schema输入。顶层CMake链接新IR core、直接发射到新IR的x86子集入口、新公共优化、新ARM64标量后端、纯distorm C库、必要测试和基准，不编译旧VM流程，不能经wrapper接回旧服务。

施工树随后移除了2206个未参与新构建的旧文件；删除前逐文件核对其与冻结26版完全相同。当前树只保留实际构建输入、预算检查脚本、当前合同和必要说明，旧参考代码统一在冻结仓库/快照读取，不在新树混排。

第一阶段从没有优化pass、IR分析、分配器或后端的构建层起步。2026-11-03接入B、标量/向量C、整数flags/Jcc、自动Function区域发现和独立应用执行服务；当前已测进度与本轮验收见第34节，前文保留各自检查点及测量范围。原Bionic/CoreMark已在两scope完整运行，完整端点compile与集中质量验收已取得同轮证据。

## 2. 严格施工顺序

施工避免过度防御性编程，沿实际owner维护必要合同；按功能批次集中复用既有检查，不在实现中途逐helper增加验证层、测试或质量矩阵。

| 阶段 | 实际工作 | 进入下一阶段的条件 |
| --- | --- | --- |
| A：IR构建与提交 | 从零建立存储、追加、Block/Function所有权、terminal与提交 | 必要结构/失败/生命周期合同成立；提交无全域扫描、分配或payload复制；真实应用固定请求序列满足A预算门（见第9节） |
| B：公共优化流程 | 在新IR上逐项重建真正需要的优化 | 逐项说明输入、写口、实际遍历及结束点；不导入旧PassPipeline/RewriteContext/Facts体系 |
| C：机器交接与分配 | 对接新IR和后端机器合同，分别重建Block与完整Function分配 | 实际位置决定直接交编码；新增分析只由真实消费者请求；不恢复plan/actions/certificates往返 |
| D：完整执行验收 | 运行时发布、异常恢复、缓存和生命周期；集中核生成/运行质量 | 两种范围完整冷compile分别≤2×同轮fresh固定FEX，最终质量不回退 |

原先“A未结束不得进入B/C”的施工冻结已被第12节的decode后置及本轮推进指令替代：在既有唯一存储、局部写口和O(1)提交合同上补B和真实C，不等待完整新decoder；实际A预算资格仍开放，不能因进入下一施工阶段就标为通过。只为实际消费者计算使用计数与寿命，不先搭建通用CFG/证明平台。旧流程不以兼容路径混入A。未知后端分支保留，但不先执行完整Known规划再回旧链。

## 3. 第一阶段IR合同

- 稳定u32身份；真实operand保读取view、完整立即数及语义参数。不使用HirValue/HirUse，不每node堆分配。
- Block和Function请求明确分开，Function允许跨块引用。保存控制边不等于构建或求解CFG。
- 已知opcode的frontend使用同一声明生成的`Emit<Op>`；动态opcode使用`Append`。二者共用实际存储、局部检查和唯一追加事务，不能保第二IR、第二schema或转换表。
- append只做当前字段/schema、类型、范围及实际同unit引用检查，不遍历scope。静态写口的signature/result policy编译期展开，实际句柄、view、字面量和复合payload检查保留。
- IR Builder/Submit不维护反向uses、SSA dominance、liveness、flags需求、CSE/DCE、guest-state数据流、物理home、证明或预分配表。优化器和分配器的阶段内临时数据分别归实际消费者，不进入逐node常驻IR。已知前端可在运行时明确合同下复用同块实际numeric GPR值，并在固定缓存中归并尚未观察的完整写；16槽及valid/dirty掩码归前端发射owner，不进入IR Builder或提交。
- 新建时指令表只追加，不以vector插入/删除移动已发布NodeId。Builder::Edit移动已有Program的唯一Owner；Rewrite仅原位替换同自然结果类型的已有指令，保持unit、ID、块和顺序，引用必须早于目标。EraseUnused仅限Editor，把已无data/flags/terminal use且无可观察效果的操作置为Nop/Void；调用方负责实际uses，写口不扫描全域。没有插入、重编号、拓扑重建或自动语义等价证明。
- 只替换一个Value edge时，ReplaceValueUse直接写实际8B word，局部检查owner、真实边、前向顺序及新旧有效读类型精确等同；不重走整条指令schema或重新编码。等价性仍由优化规则负责，use账仍由现有优化rows维护。改变opcode/shape继续使用原Rewrite。
- 内部存储为24B Operation、24B BlockRecord和8B word；普通原子输入直接存，真实复合输入保descriptor。完整64位payload用紧随扩展word，不截断flags/立即数/helper traits。
- operand_count/Reserve以物理word计；逻辑cell、Value-use边数另计。Operands(local NodeId)是实际操作数的forward range，WordCount为O(1)。Operands(const Operation&)只接受当前Program实际记录，外来或复制记录不按碰巧相同的offset读取。OperandArena()仅暴露原始物理word，可含改写遗留范围；不得将它当作逻辑cell或live引用目录。
- submit仅唯一移交实际IR存储。不得隐藏FromFinalIR/SealIR、编号、遍历校验、统计全N或建立Facts。必要闭合状态由实际构造写口维护。
- AppendResult为16B的error/node/value_unit，Value由GetValue按需组成；完整u64 unit/u32 node不截断，Void成功仍保真实node但返回空Value。没有第二份node、packed结构或额外结果wrapper。
- append失败不得发布半条指令或污染旧前缀/所有权；过期、跨unit及提交后写入明确拒绝。公开导入数据的完整检查另设明确入口。
- Known/EarlyOff/Unknown保中立不可变环境。Known frontend可按ShiftedAddress能力直接发射公共移位地址表达式；EarlyOff/Unknown保一般算术路径。物理home和编码仍归backend，构建不做分配。
- opcodes.inc同时声明schema和语义effects。EffectsFor只读取opcode及当前metadata；flags输出、隐式flags读写、guest/local/host状态、内存、fault、ordered和控制效果显式区分。未完整定义的helper/FP等保守Unknown，不能当纯运算删除或移动。该表不是新分析pass，也不证明跨操作重排正确。

过程中允许生成质量暂时变化。当前标量编码器只接受已实现的实际语义，不能以子集或空后端时间对比FEX完整compile；也不能为了可运行在提交尾部重新挂旧pass和分析。

## 4. 重构后文件组织

```text
CMakeLists.txt                       # 仅新core、测试、基准
source/runtime/ir/
  opcodes.inc                        # 中立声明，无旧实现
  types.h / opcode.h                 # 类型/身份/operand/scope/target和schema
  program.h / program.cpp            # 唯一存储、提交后实际视图、Owner移动
  builder.h / builder.cpp            # 追加/最小改写写口、Block闭合、常数提交
  builder_impl.h                     # 同schema局部校验与实际编码；追加事务/静态展开
source/runtime/frontend/x86/
  decode.h / decode.cpp               # 一个实际decoded buffer和有界Function发现
  build_ir.h / build_ir.cpp           # 实际Emitter、块内GPR出生复用、flags recipe和直接IR发射
source/runtime/compile/
  optimize.h / optimize.cpp           # 同Owner局部折叠、拷贝/Uniform转发及DCE
source/runtime/backend/arm64/
  allocate.h / allocate.cpp           # Block/Function分开入口；直接产生唯一实际home
  compile.h / compile.cpp             # 读原IR和实际home，直接编码ARM64 words
  integer_helpers.cpp                 # 真实整数helper及私有clobber ABI
source/runtime/execution/
  guest_state.h                      # 实际guest状态及寄存器编号
  memory.h / memory.cpp              # guest映射/ELF/栈/源代码页写保护的唯一owner
  native_code.h / native_code.cpp     # native slabs、发布、fault范围和安全释放
  dispatch.h / dispatch.cpp          # 唯一目录的原生probe、Root/cold/exit交接ABI
  fault.h / fault.cpp                # 宿主fault定位、未对齐回补与真实代码写故障恢复
  runtime.h / runtime.cpp            # 编译请求、唯一PC/实际链接目录、syscall及退休时点
source/launcher/linux/main.cpp        # CLI与粗粒度诊断，不进入codegen
source/runtime/externals/distorm/     # 仅纯字节解码，不用旧DisDecode wrapper
source/tests/rebuild/
  ir_construction_test.cpp           # 结构、失败、Owner/改写/读口及零分配提交
  x86_fixtures.h                     # 明确的真实x86字节夹具，不是应用工作负载
  x86_construction_test.cpp          # 子集decode、PC、宽度、拒绝/提交合同
  ir_optimize_test.cpp               # effects、flags、views、DCE与转发边界
  arm64_compile_test.cpp             # 标量编码、分配、循环/spill和明确拒绝
  compile_pipeline_test.cpp          # 两scope×三target模式×优化开关，Android实际执行
benchmarks/
  ir_build_submit_bench.cpp          # 固定请求序列；默认完整顺序生命周期；诊断分项
  x86_build_submit_bench.cpp         # 子集真实字节构建/提交/释放；实际失败前缀工作账
  normalize_birth_trace.py           # 仅资格工具：完整raw→显式body/full规范输入
  ir_birth_replay_bench.cpp          # 实际写口重放、非计时核对、固定样本生命周期
  ir_compile_bench.cpp               # 有真实分配/编码的标量链路；明确synthetic
  check_compile_budget.py           # 固定数值预算，缺失真实输入拒绝授予资格
  compile_budget.template.json      # 必需测量字段模板，空值保持空值
```

同一工作不能在builder、submit、wrapper和receiver中重复描述。不为分层增加中间对象。

## 5. 成本与验证边界

默认固定请求次数，逐请求build→submit→释放Program/Builder，主时间包括失败、放弃和析构。候选不得按自身速度校准batch。construction/submit/析构分项只在显式batch-diagnostic模式观测，不能替代默认延迟。完整的未计时复核重放相同请求序列，逐个核对规范语义摘要、N/E和提交结果；计时区只累计常数计数，避免为统计再扫描全IR。固定emission fixture必须标为synthetic，x86夹具必须标为partial-ISA，二者都不能通过真实应用预算门。

submit必须实际零分配、零全域访问、零payload复制；用源码和必要合同测试核实，不增每node Passed或遍历统计表。构建为摊销O(N+E+真实payload)，扩容与失败成本计入；热缓存/预留不能代替冷成本。

默认不启用细粒度探针；SVM_PROF/PROF2的存在式开关不能用字符串0假装关闭。A预算和完整FEX两倍门按第9节分别执行；A通过不继承完整compile或质量资格。

过程只跑受影响的必要检查，不逐helper跑QoR/PMU/FEX矩阵。最终质量门不降。设备仍为用户连接的红魔，保Android/ADB/PMU条件；当前先建独立host/Android core，不占手机测旧路线。

## 6. 已停止路线

旧26源码、v2合同和构建保存在原证据根。最后旧普通方向24：Block1695.860341ms/FEX46.767552ms=36.26147×；Function1151.049011ms/FEX46.891979ms=24.54682×，不合格。旧结果不证明新路线的上限；删除工作后的阶段时间不能称完整compile提速。

A未完成前，不继续原P1–P5补丁清单；整体框架未获完成资格。


## 7. 历史第一阶段检查点：紧凑构建层（2026-11-02）

A仍在进行，**没有进入B/C，没有授予完整框架或≤2×资格**。真实frontend仅覆盖x86-64 MOV32/64、LEA64、NOP、直接JMP和C3 RET，其它指令明确拒绝，不回旧链。所有退出写实际PC；memory访问前写fault PC并保TSO；r32写零扩展，RET先读栈再提交RSP/PC。Known/EarlyOff/Unknown当前只保存metadata。

本轮解决构建层的布局和容量问题：

| 对象 | 上版新IR | 当前 | 合同 |
| --- | ---: | ---: | --- |
| Operation | 32B | 24B | 完整u64 flags保留，profile/branch标记合并 |
| BlockRecord | 72B | 24B | 内部terminal去重复unit，EndBlock仍完整检查外部句柄 |
| 普通operand word | 16B cell | 8B word | Value/view完整；wide payload紧随额外word |
| 两值普通Add | 80B | 40B | 输入直接存引用，无Identity header |

外部Identity输入仍需完整检查header/child后规范存储；真实frontend直接提交原子输入。没有第二decoded表、字面量查重表或新增逐node状态证明。CellRange只借用实际word存储，按标记跳过wide载荷。

保留std::vector三表和唯一Owner。初始容量按实际写入懒分配：32个Operation、96个word；块表Block从1块、Function从4块开始，之后几何增长。所有获取容量的成本均计入构建；未引入allocator/cache管理器，未用预热掩盖冷容量成本。删除了按输入数量预估operand容量的写法，避免Identity省略后仍触发多余扩容。实际写口先保障完整1/2word容量，当前追加失败退回旧prefix，ID/块计数只在最后提交。

历史主机Release和红魔Android的两项必要合同检查退出成功。历史ASan+UBSan也退出成功，但recover模式日志实际含distorm未对齐读取/有符号移位告警，不能称sanitizer干净通过；本轮改为遇错即停后修复这些问题。上述历史检查覆盖宽载荷/flags/traits保真、view、跨unit和stale拒绝、Identity规范、真实扩容失败回滚、精确容量内零分配追加、Submit零分配及原x86合同。只读复核未发现隐藏全N工作或第二份IR。Submit仍是常数完整性门加Owner移动，无payload访问/复制/分配。

以下为本轮修复前的历史测量，使用候选自适应batch、分批build/submit/释放，只核对每batch第一份Program。数值可复核，但其生命周期和校验范围不足以作为默认逐请求延迟或预算资格。修复后的固定请求测量另存phase-a-contract-fixes，不能混用。

历史红魔同一输入对照（Known、growth，每请求构建中位数）：

| 范围 | 上版新IR | 当前紧凑IR | 实际存储变化 |
| --- | ---: | ---: | --- |
| synthetic Block，24条IR | 4.597µs | 3.226µs | 2072→1224B，分配10→4 |
| synthetic Block，262144条IR | 52.418ms | 39.349ms | 20622168→11883992B |
| synthetic Function，32块/262144条IR | 54.986ms | 38.741ms | 20622896→11884064B |
| 实际x86字节子集，单Block | 4.521µs | 3.069µs | 1352→872B |
| 实际x86字节子集，三块Function | 9.762µs | 7.003µs | 3288→2024B |

最后两行是decode→build→submit整体，未开allocation tracker；前三行开tracker且Submit/析构单列，不能相减归因。synthetic每case5 samples/1 warmup；x86为9 samples/1 warmup、batch256。exact Reserve单列诊断，小请求变化不明显，个别中位数略慢，不能取代growth结果。两布局对每batch第一份Program按canonical_semantic_v1逐operation比完整payload/view/metadata/terminal，N/E及规范语义摘要一致；不是比较布局raw checksum。

手机CPU4 affinity，policy前后walt、614.4–1171.2MHz、温度26.3°C相同；这是范围而非锁定实频。设备测试及基准退出成功、namespace已清理。仅修opcode声明表旧存储注释后重新构建，四个Android产物与已测镜像逐字节一致，无需重测。

该历史检查点build-input digest为`7094df23bc282808ff9d5e175b03455a0a5538936e8fdfaabf62723848061515`；[本轮摘要](/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/phase-a-compact/measure-summary.md)、[源码清单](/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/phase-a-compact/build-source-manifest.json)与原始样本/命令/设备收口记录均在`phase-a-compact`证据目录。上版`phase-a-emit`保留为独立历史基线，不将其旧raw checksum改名冒充新语义核对。

上述结果**不是全ISA frontend、真实应用冷编译或FEX对比**。当时历史IR dump省略compound参数，没有可无损重放的真实append trace（本轮第11节已从真实写口另行采到）；旧优化后census也不能当构造出生数。A下一步仍需从新frontend实际写口取得真实应用的形态/规模，复核基础成本预算，再进入中间优化重建；本轮不先实现pass填补功能。


## 8. 最小改写和解码边界修复

`Builder::Edit(Program&&)`只移动同一个唯一Storage；Edit阶段不能创建块或追加指令。Rewrite不改变结果自然类型，局部检查当前schema、payload、view、同unit及严格早于目标的引用；因此当前不承诺新增循环Phi或调整控制结构。先校验并数实际word，再写实际存储；不建立decoded plan、use链、Facts、修复动作或证书。类型保持只保证结构，不代表新旧表达式语义等价。

已有word范围足够时复用；末尾范围可缩短/增长；非末尾变大时只在原arena追加真实载荷，再提交记录。零word记录没有payload，旧offset不参与有效性。失败不改变原记录或有效IR；扩容实际计入改写成本。非末尾缩短或搬移可保留失效word，当前不承诺无界编辑的空间回收；尚未加入pass，不为假设消费者添加free-list、压缩或整理器。以后确有消费者需要删除/大规模改写时，应先定义语义和成本，不能把整理偷渡到Submit。

严格sanitizer还暴露vendor使用任意字节地址做typed整数解引用，以及`1 << 31`有符号位移。LE读口改为同宽本地整数的memcpy，保留编译器生成非对齐load的能力；高位flag常量改为unsigned。CTest固定sanitizer遇错即停，不再仅凭recover模式退出码授予通过。

distorm在legacy前缀使REX不再紧贴opcode时，必须同时清除REX有效标记、prefix类型和vrex扩展位。修复仅在PET_REX下执行，不能误清合法VEX状态。回归覆盖失效REX.B/R、有效最终REX、重复REX、内存base/index及原REX+VEX拒绝边界。

## 9. 可执行成本预算

以下是从当前不含decode的最多2×FEX目标分配的**设计上限**，不是现有实测结果。双方统一按第12节口径剔除decode，旧完整compile分母不再用于新口径。Block和Function分别验收，不合并平均，也不把FEX的Function请求伪造成Block请求。

| 归属 | 相对同轮fresh固定FEX不含decode的冷compile上限 | 包含的实际成本 |
| --- | ---: | --- |
| A | 0.50× | IR所需输入材料化、存储初始化、构造/提交、阶段内失败放弃和Owner释放（纯decode暂排） |
| B | 0.25× | 公共优化及其真实需要的分析/改写 |
| C | 0.90× | 选择/分配/编码及实际后端交接 |
| D | 0.25× | 发布、运行时集成和生命周期 |
| 其它/余量 | 0.10× | 未被前四段覆盖的完整端点成本 |
| 总计 | 2.00× | 不含decode的完整compile端点；失败/放弃请求也纳入固定序列 |

阶段划分必须互斥且能对回端到端总量；最终真实流水线测量不能再为每个阶段重复计同一份Owner释放，也不能把释放挪到未计时处。独立A试验包含释放，其后在完整流程中按实际发生点归属。

历史组件脚本有A和full两种模式：A成功只授A段资格；其full逐项且总量均满足的规则是本方案自加的组件试验强门。上表份额是设计分配，不能将它们偷换为用户新增的逐项硬cap；用户硬要求仍是两scope完整端点各≤2×同轮FEX。最终实际应用验收见第34节，分项仍保留作成本归因，不能重复收费或藏失败；也不能用A synthetic PASS替代完整应用。输入身份、负数/NaN/缺失数字的基本有效性要求不变。

必须提供可核实的工作负载身份/hash、固定请求序列、Block/Function分开完整attempt账本（成功/失败/放弃）、测量端点、同轮设备和配置、FEX源码/二进制身份及fresh状态。synthetic、partial-ISA、缺完整attempt或只测成功请求时拒绝PASS。模板保留空值；目前已有冻结旧前端的完整出生trace，但尚缺完整IR构造/后续编译的同端点数据及剔除decode后的fresh FEX参照；不要求先补齐新字节decoder，因此预期结果为MISSING，而不是A通过。

该检查点只做基础问题的必要结构检查、sanitizer和红魔子集复核，没有展开QoR/PMU/FEX矩阵。其B/C停工状态已由第13节的推进实施更新；A预算资格仍开放。


## 10. 前一轮修复验证（2026-11-02）

六项review问题已落实到新基础层，未增加pass、分析、分配器或后端。另修复distorm的未对齐读取/有符号移位，并在CTest固定sanitizer遇错即停。Release、严格ASan+UBSan（无告警）和红魔Android两项必要合同检查均通过。最小改写的成功、失败、真实扩容失败、非尾部遗留word、尾后零word锚点、foreign/copy读口和Owner生命周期均在原合同测试覆盖。

共享encoder起初重复判断wide payload；ELF审查确认其发生在fast路径，本轮已复用同一次判定。普通Append仍单遍，没有把Rewrite计数/顺序检查带进Append实际分支；部分helper的ABI/outline仍有差别，未宣称零开销。

测量改为固定序列的完整顺序生命周期；完整未计时重放核对每个请求，batch诊断另核对所有实际slot。最终host动态/静态writer与基线的规范摘要一致；Android42组固定序列、24组短ABBA摘要一致。顺序计时实例不另扫描payload，不能称逐个计时实例都计算过hash。

短ABBA Known/growth结果如下。每序列64成功+2失败弃置，单位µs/全部attempt，含释放和allocation tracker；每版本合并两轮各9 samples取中位数。

| 范围 | 每个成功请求N | 紧凑基线 | 本轮修复后 | 比值 |
| --- | ---: | ---: | ---: | ---: |
| block | 24 | 3.378 | 3.442 | 1.0188× |
| function | 24 | 3.764 | 3.820 | 1.0148× |
| block | 128 | 13.976 | 14.281 | 1.0219× |
| function | 128 | 14.920 | 15.149 | 1.0154× |

较长顺序试验的首组IR基线相对后续短试验快近一倍，存在明显顺序/状态差异，原因尚未确定；该组IR耗时比值全部标记不可比，原始数据保留。policy前后walt、614.4–1171.2MHz、28.2→28.3°C；策略范围相同不等于实频锁定。以上仅为synthetic基础层描述性数据，不能称完整compile性能不回退或达到FEX预算。

当时预算脚本schema自测通过，空模板A和full均明确MISSING/exit2。没有用synthetic/部分ISA或历史FEX填补缺失值；该时点B/C未开工，完整compile及最终生成/运行质量未验收。当前施工进度见第13节。

最终build-input digest：`212929869b5480fdba1e0ba6afc9226add0d68c04965b128b76de75a96c72ab8`。当前[修复摘要](/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/phase-a-contract-fixes/measure-summary.md)、[源码清单](/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/phase-a-contract-fixes/build-source-manifest.json)、patch、源码快照、完整日志及设备hash/收口记录均在`phase-a-contract-fixes`。先前初测和发现问题的版本单独保留，没有改名冒充最终实测。


## 11. 真实出生工作账与writer成本（2026-11-03）

本轮先落实性能review的验收和输入闭合：不扩展优化pass或分配器，原仓库/Git/index/WIP继续冻结。正式A/full仍为MISSING。

### 11.1 预算门和实际失败统计

预算schema2对candidate/FEX同等要求真实application_compile资格，拒绝synthetic、partial-ISA和birth_ir_writer_replay。二者都须给同一设备单调wall时轴上的原始区间；按union算时间，phase必须在完整window内且互斥，other按覆盖差计算。Optional duration必须与区间时长精确一致。A-only完整window全部计A；full具有A/B/C/D及剩余成本，显式0阶段允许空区间+0。旧布尔互斥承诺、重叠分项和不合格FEX参照反例现均拒绝。不能用旧累积duration造出原始时间点。

Builder::CountWork明确为O(N+live words+B)的显式资格读口，记录N/Value occurrences/physical words/B；不进入Append/Rewrite/Submit。BuildIR仅collected非null才在Owner尚活着时采样，成功提交前或失败catch各一次，默认路径不扫描。当前x86 schema4固定失败包含UD2、MOV后UD2、首条MOV拒绝但已生成LoadUniform，真实N/E/word分别0/0/0、2/1/4、1/0/1（各B=1）；不能由accepted指令数推IR工作量。

### 11.2 真实字段对齐，不缩窄输入来迎合schema

原16bit expression code低8bit保operation、高8bit保shift；新增AddShift明确表示base+(index<<shift)，shift<64，保持24B Operation/24B Block/8B word。完整Code参与canonical；未截断shift_ext。

Arg0/IntegerArg0在result未指定时推导，显式结果按请求保存，输入view不改。真实输入中BitExtract U64源→U32结果等揭示了原来的过严同宽限制，已修正；Integer域、Fixed/Void和Rewrite自然结果保持检查仍在。helper八字节traits保完整u64；源进程的effect字典完整留在绑定sidecar，当前helper effects仍保守Unknown，不冒充已导入新runtime注册表。

### 11.3 采集、重放与计时范围

独立5TU诊断覆盖只借用原冻结8cda cohort的只读源/库，是原baseline，不是26或新frontend。2404原源码和15原链接输入逐SHA不变。真实写口记录每次birth的完整物理参数、view/flags/traits、后续mutation、完整11种递归terminal和失败/弃置；独立审计对seq、request生命周期、引用、origin、frontier零违规。

Block配置2414尝试、67456出生、42821 Ebody。Function配置实际500 Function（98186出生/71733 Ebody）+98 Block fallback（4013/3028），其中64个Function后端失败随后同PC回退。4个Function尝试各有两个真实owner，规范输入因此有504 Function Q+98 Block Q；所有64失败保X，没有过滤。14,741次退役来自374次前端ResetDecodedBlock重解码，其出生全部保留，不能拿最终frontier替代出生量。此发现属于冻结旧前端。

诊断normalizer只在benchmark准备阶段运行。BODY_ONLY保每个owner内所有birth顺序/参数，复杂terminal投影Unreachable；220次源块回访各加replay段，原5873块→6093，原关联/terminal全留sidecar。2,254/4,105次flags-only改动明确折入metadata；12,592次AdvancePC后改及unlink/retire保raw，成本未在出生writer指标中重放。FULL_TERMINALS对不可表示语义失败关闭，不偷换终结。

主机static/generic两份完整输入和手机四个组合均通过资格及N/E/word对账。P逐payload核对；X只核真实birth结果+CountWork，不补造终结也不称X payload canonical。primary含unit材料化、opcode分派、构建/Submit或弃置/释放及逐请求timer，解析/CountWork/完整payload核对在计时外，无allocation tracker。

| 真实输入/实际scope | owner请求 | 出生N | Ebody | physical words | static中位ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| Block配置/Block | 2414 | 67456 | 42821 | 124349 | 11.536925 |
| Function配置/Function | 504 | 98186 | 71733 | 197751 | 16.558325 |
| Function配置/Block回退 | 98 | 4013 | 3028 | 7052 | 0.644170 |

Function配置按同sample两scope相加后取median为17.193900ms；不是两个独立median之和。每完整序列5 samples/1 warmup，红魔CPU4；policy前后walt、614400–1171200kHz、cur1171200kHz、27.9°C，不宣称实频锁定。generic描述性总量11.368148/17.101153ms，不据短样本宣称稳定差异。

这些是**出生体writer组件成本**，没有实际新frontend的取指/发现/decode、后续编辑、优化、RA、编码和发布，不与旧路线的完整compile时间相减。源CoreMark不足10秒有validity ERROR，CRCs两模式一致仅作输入运行事实，不授分数或新QoR。完整A≤0.50×FEX和最终≤2×FEX均未验收；本轮未扩展中途QoR/PMU/FEX矩阵。

### 11.4 下一阶段仍需满足的边界

新Function formation必须在IR出生前完成实际发现，同一decoded buffer只解析一次并交给发射；不把旧前端的清块重解码迁入新树。Known纯能力选择、Unknown一般路径以及进入B前的改写增长边界继续保留，不用本轮body-only成绩代替这些工作。现有三vector唯一Owner和紧凑布局保留，不能因trace诊断另建生产IR/plan/certificate层。

当前build-input digest：`f99aaa7c5a82fdfc28bb9a7ed5ddd8b2eb49998443727eccc90c72235a6f174e`。[本轮摘要](/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/phase-a-performance-closure/measure-summary.md)、[完整源码清单](/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/phase-a-performance-closure/build-source-manifest.json)、source.patch、真实raw/规范输入/normalization sidecar、独立审计和全部命令/样本均已保留；采集和重放两个设备namespace均核hash后关闭。


## 12. 当前比较暂不计decode（2026-11-03，用户最新指令）

用户明确要求“decode可以暂时不计入差距，这个后面再优化”。本节覆盖前文把decode纳入A和完整compile分母的旧口径；历史原始结果不改写。

当前比较：`T_SVM_without_decode / T_FEX_without_decode <= 2`，Block/Function分别验收。IR构建/提交、优化、Block或Function分配、修正、编码、发布、失败/弃置及释放全部计入；最终生成质量目标不变。A预算仍0.50×，B0.25×、C0.90×、D0.25×、other0.10×，分母统一改为不含decode的同轮fresh固定FEX。

以已解码输入交给IR发射为主要边界：字节decoder的ISA覆盖和速度暂不作为推进中间框架的前置门；可先用可靠的已解码输入或完整构造输入测量，随后接优化/分配/编码。IR所需的输入材料化、buffer初始化、IR块创建/清理、重写和生命周期仍收费，即使代码位于名为decode的函数内。冻结旧前端14,741次额外出生属于IR构建与废弃工作，不能随decode一起抹掉。真实Function形成的单次解析/单一owner原则保留，decoder本身的速度优化后置。

验收schema3在manifest与candidate/FEX结果均要求`comparison_basis:compile_excluding_decode`。`wall_intervals_ns`只含应收费的编译区间；各方另列同一wall时轴的`excluded_decode_intervals_ns`并声明`decode_exclusion_kind:byte_decode_only`，两类区间不得重叠。已提供解码输入的路径显式记空列表，不要求补测无关decoder。不得把含IR操作的整个frontend方法排除，也不能拿旧完整compile时长猜减一个decoder占比。`partial_compile_coverage`校验剩余编译语义是否完整，字节decoder覆盖不再作为该指标门。

目前11.536925/17.193900ms依然只是出生体writer子项，包含原第11节的body-only终结投影和未重放编辑边界；未突然变成“完整的不含decode compile”。后续将IR优化、分配、编码和生命周期接入后，在相同已解码输入与fresh固定FEX不含decode端点上验收。缺少剩余流程或有效FEX分母时继续MISSING；缺少完整新字节decoder本身不再阻塞这条路径。本轮只改口径、验收工具和文档，不重跑设备或QoR矩阵。

这是当前框架比较的临时口径；后续优化decode后仍须恢复完整端点复核，不能把本轮不含decode的通过称为最终全流程达标。本次仅资格脚本/模板与文档改变，C++及已有实测镜像未变，无设备重跑。当前源码输入摘要为`60da11f3bcc7bbf9d3436c15b3d0ed0aa0af0c82a32826e03cbca320704fc8a5`；[口径变更与检查记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/compile-without-decode-20261103/receipt.json)保留schema自测、旧口径拒绝、双方decode排除和缺证据结果。

## 13. 在新框架上推进B与标量C（2026-11-03）

本轮是实际实现增量。主链为`Builder → Program → Optimize → 同一Program → AllocateBlock/AllocateFunction → ARM64 words`。只有Program移交可写Owner，没有HIR→另一IR、inputs→preparation→actions→certificates、公共PassManager或证明缓存。后端不包含x86头，公共IR和优化器不包含ARM64头；唯一前移接口是中立TargetFeature能力。

### 13.1 同存储改写与公共优化

Editor只读视图与Program共享真实Storage；Rewrite/Erase不移动指令数组。operand视图每次写后重新借用，不能把改写前的range当成稳定buffer。`EraseUnused`不替调用方证明无uses；优化器负责data/flags/terminal引用，core只做effects及metadata的局部拒绝。删除节点变为Nop/Void，保留ID、块和顺序；实际payload目录始终按每条Operation读取，旧arena范围不当作live输入。

`Optimize`只有一张临时u64[N]表，打包当前use计数、live和producer保守保护位。三次有界遍历依次建立真实根与使用关系、做局部改写、反向释放失去uses的纯操作。终结输入、fault/ordered/control/unknown、状态写和flags元数据均保留；依赖producer原opcode的flags/helper消费保护对应producer。整数折叠按自然类型和consumer view工作，不悄改view或把窄结果强制扩到源宽度。未实现的运算保留原形。

块内Uniform转发使用固定小缓存；精确宽度/类型命中才能转发，重叠写、非标量写、块边界及保守效果屏障失效。StoreUniform不因这项优化消除。没有跨块guest-state求解，也不为单一局部优化搭建全scope别名系统。

每次改写前按`old_words + 2*N`检查实际arena-size硬上限，允许调用者进一步压低；不是malloc capacity字节上限，也不会回收接管时已经存在的更大arena。每节点至多一次正向Rewrite，删除不增加operand存储；不自动压缩、重编号或在Submit做清理遍历。预算不足时该次写入原子拒绝，并返回同一有效Owner；已完成的等价局部编辑保留，不克隆全scope做回滚。dense表在接管Owner之前分配；接管后的扩容失败也返还Owner。

### 13.2 已知后端前移公共地址形态

ARM64请求入口提供`KnownTargetInfo()`，声明接受ShiftedAddress。x86 frontend在Known模式可将base+index*scale直接提交为`GetOperand(AddShift, shift)`，消去单独Lsl节点；位移仍为普通公共算术。EarlyOff、Unknown或未声明能力时按通用Add/Lsl发射。前端没有宿主寄存器号、spill或机器指令字，未知后端不先跑Known规划再降级。

### 13.3 真实分配与直接编码

`allocate.cpp`分别暴露Block和Function入口，只产一份按源NodeId索引的实际home/definition-position/last-use表。每个值的寄存器或spill位置在整个单元固定，编码器在定义处存spill，使用处直接读取，没有repair actions或第二份机器IR。当前按固定14寄存器bank扫描；spill为每个值独立8B槽，优先建立有界可执行基线，尚未加入槽复用、重材料化或最终质量优化。

Block不建立CFG顺序、DFS或回边结构，按单块实际顺序处理本地uses。Function直接读取真实terminal做一次迭代DFS，得到唯一RPO块顺序；入口可达部分在前，未到达组件追加，源IR和ID不移动，也不复制edges。实际home中的definition-position、use/terminal位置及回边都采用这份顺序，编码器消费同一block_order；不能把任意IR出生顺序或块ID顺序当作执行位置。只验证本次use的定义位置在前，完整SSA dominance仍是生产者合同。

Function的所有RPO向后控制边按实际块位置建立端点，重叠/交叉回边的区间由右向左在同一B级range-max结构中闭合，再按实际use延长块外定义的寿命；不能把textual last-use当作循环liveness。RPO下相同节点位置的空块由块rank区分控制边，不保留旧出生坐标的u64 tag补丁；区间与terminal仍为实际u32位置。无回边路径保持O(N+E+B)，有回边的额外工作为O((B+E)log B)，固定寄存器扫描为O(14N)，没有逐node扫描全部块或全图迭代。IR生产者须保证定义支配use；本实现不把Builder的“更早出生”检查冒充SSA dominance证明。Phi暂不接受。

`compile.cpp`直接读取原IR及上述实际home，输出baseline ARM64机器码words。ABI为`u64 fn(u8* guest_state)`；x19保存state，保存callee-saved寄存器和返回地址，16B栈对齐；不占平台保留x18。范围包括明确支持的整数、guest-state、内存、地址和控制形态；未实现opcode、flags或operand形态明确拒绝，失败输出不发布。TSO初版使用普通标量访问与保守屏障，不把可能未对齐的guest地址直接套用LDAR/STLR。

库只产真实机器码，不接旧VM发布器。测试在Android以W^X映射和cache flush执行；guest fault映射/恢复、完整dispatcher、code cache、完整helper及最终生成质量属于后续D，不能把测试映射包装成运行时已完成。

### 13.4 当前测量与下一步

真实出生记录重放新增显式`--pipeline optimize`；每个P执行真实优化并释放，每个X保留完整构建/弃置工作，绝不伪造其B成本。资格重放先核writer原规范，再核优化输出实际引用和稳定身份；这只是结构核对，不能宣称全语义等价证明。body-only终结及未重放旧编辑的限制继续保留。

另设`ir_compile_bench`，固定请求/节点数，真实完成构建、B、Block或Function分配、编码及释放；输入依赖guest-state并具有寄存器压力，不允许全折叠后用空后端冒充编译。它明确标synthetic且不含运行时发布，不通过应用/FEX预算门。当前不做中途QoR/PMU矩阵，集中验证核心合同、sanitizer和红魔标量执行；最终完整应用和不含decode的fresh FEX ≤2×仍须单独验收。

本轮RPO修正版实测与必要验证已完成，源码输入为52文件、摘要`43ef838a78d03bf5e8f3c52078efd71241a1d46ee6b739dfb4901b1c0bf714ad`。Host Release 5/5、严格ASan+UBSan 5/5均通过且无sanitizer诊断；红魔五项合同通过，backend完成437项检查（含原生机器码执行），pipeline实际执行两scope×三target模式×优化开关的12条路径。首轮新测试错误地要求可安全DCE的alias wrapper存活，修正为检查真实live consumer和view后通过；没有放宽raw语义、effects或预算guard。初轮失败日志保留。

同轮红魔CPU4、walt min614400/max1171200kHz，前后cur1171200kHz，电池26.8°C；没有锁频。static writer/optimizer以writer→optimizer→optimizer→writer顺序，每组1 warmup+5固定样本。Block出生序列完整N67456：writer中位11.041465ms，writer+B中位20.914320ms。Function配置完整N102199：逐样本先合Function/Block回退后，writer中位16.438544ms，writer+B中位30.300603ms；64个X及其构建/弃置成本保留。二者的差额只能作组件增量观察，不是独立探针测出的纯B时间。normalized Program仍包含旧retired births，不能把它叫实际最终IR优化输入。

标量构建→优化→实际分配→编码→释放，在128×320节点时Block/Function中位12.785313/13.204219ms；128×1088节点为44.198594/44.504792ms。每请求19个真实spill槽，总栈帧176B。节点3.4倍对应耗时约3.37–3.46倍，当前夹具未呈现全scope反复扫描带来的超线性增长；它不证明所有Function/完整应用的性能上限，也不替代实际FEX分母。样本、机器码镜像hash及手机临时目录清理检查在[修正后设备记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/phase-b-c-implementation-20261103/revision2/android/execution.json)。完整A/full预算模板均继续MISSING/exit2。初版C测量/镜像身份单独保留，不冒充本修正版；A+B replay不依赖ARM64库，其可执行文件hash与前轮完全一致，故原ABBA数据继续作为同一组件证据。

该检查点的功能缺口是实际opcode/flags/helpers、可靠已解码输入上的Function形成、异常与发布生命周期；其中整数flags及Jcc已由第14节推进，其余逐项按消费者加入，不恢复已退役的中间表/包装层。源码和本轮验证记录保存在`phase-b-c-implementation-20261103`。收口独审还发现出生顺序无法代替执行顺序的无环/空头/非空循环头反例，已由第13.3节的统一RPO根治并原生回归，未保留空头坐标特判。独立固定seed的32,818个CFG模型与完整dataflow liveness oracle差分未发现漏保活；oracle仅为外部诊断，生产不引入求解器。存在保守寿命延长，不将此基线称为最优分配或最终QoR。原树2404文件与Git index哈希核对均保持冻结，设备临时namespace已独立确认无活动进程、文件hash前后一致并清空；缓存继续在Data2固定目录复用。

## 14. 整数ALU、flags及Jcc闭环（2026-11-03）

本轮沿用第13节的实际框架，没有添加生产源文件、pass或第二套IR。当前构建输入仍52文件；结构保持`ir → frontend/compile → arm64 allocator/writer`。新增行为只落到已有职责中：

| 文件 | 新增实际工作 |
| --- | --- |
| `ir/types.h` | 中立Flag mask与Condition枚举；typed Operand重载，保留原raw64/raw32入口 |
| `ir/builder.h/.cpp` | `SetMetadata(NodeId, metadata)`直接更新真实record，O(1)、无分配、无operand/use遍历；ID、类型、24/24/8布局与Submit不变 |
| `frontend/x86/build_ir.h/.cpp` | 一个实际解码缓冲区、16B块切片及共用发射入口；32/64 ALU与Jcc、流式flags输出、真实出口PC/块 |
| `compile/optimize.cpp` | 原Uniform cache的失效条件增加WriteFlags；三轮遍历和唯一u64[N]表不变 |
| `backend/arm64/compile.h/.cpp` | 显式flags ABI、一个真实shadow flags值、ALU当场计算并合并声明位、条件读取和别名/退出同步 |
| 既有五项合同测试 | core写口、prefix/decoded、flags算术/别名/legacy边界，以及4170组真实前后端分支结果 |
| `benchmarks/ir_compile_bench.cpp` | `--flags off / --flags on`；同一完整标量生命周期的无flags/高密度flags两种实际输入 |

### 14.1 已解码入口与前端提交

`BuildDecodedIR(span<const _DInst>, span<const DecodedBlock>, span<const BlockWindow> source_bytes, ...)`直接借用实际distorm记录。`DecodedBlock{pc, first_instruction, instruction_count}`为16B稀疏切片；不保存第二份标准化opcode/operand。原字节借用用于prefix与RET编码校核，不能从不含原前缀值的`unusedPrefixesMask`臆测。输入必须来自可信decoder且与借用字节一致；本入口只做局部范围、PC、宽度、prefix和IR合同检查，不重解码来认证伪造记录。块按guest PC排列，每个切片独立检查界限，实际记录可以按发现顺序存放，也可只选择共享buffer的一部分；不能要求完整物理partition而迫使发现/迟到拆块复制或重排记录。逐指令PC和下一块边界检查继续拒绝地址重叠/重复使用。未选记录不发射；它们若属于本次请求的实际buffer材料化，完整成本门仍须计费，不能因slice未选就扣掉。

字节`BuildIR`先形成唯一`vector<_DInst>`及块切片，再进入同一个发射过程。解码失败保存真实frontier，已形成的prefix仍按同一发射逻辑处理；更早的Unsupported不能被后续解码错误遮盖，失败frontier不制造fallthrough或假的flags发布。无效scope/input清零计数；其余失败仍在唯一Builder存活时统计真实出生前缀，计时默认不扫描。现有UD2、MOVabs后UD2、MOV CR0,RAX三类失败工作账继续成立。

当前支持32/64位寄存器、寄存器/立即数的ADD/SUB/CMP/AND/OR/XOR/TEST/NEG及16种直接Jcc。CMP/TEST只写flags，不改原GPR（包括32位操作下原GPR高32位）；其余32位写按x86规则零扩展到64位。memory ALU、ADC/SBB、8/16位前端ALU和间接Jcc等继续明确Unsupported，不伪造完整ISA。

同一块内只保存一个栈上`pending{producer NodeId, profile}`：先发射纯数值操作，后续完整flags writer覆盖且无观察点时，旧producer无需再声明flags输出。flags读取、faulting访存/RET、控制出口和块尾将声明补到原producer的真实metadata。没有每指令demand表、反向IR扫描或新增SaveFlags节点；这一轮只接受完整writer，不扩展到INC/DEC等部分写入链。语义写入仍位于原producer处，不能在访存后才计算应该在fault前可见的flags。

Block请求仍只有一个IR块，Jcc用现有Select选择taken/fallthrough PC再Dispatch。Function内部边用真实Branch；外部边按实际terminal收集、排序/去重并创建真实出口stub，各自保存正确PC后Dispatch。前端继续消费caller声明的块入口；自动发现与迟到目标拆片尚未完成，不把这个入口称为完整Function formation。

### 14.2 明确flags语义与ABI

Flag是语义mask：C=0x01、V=0x02、Z=0x04、N=0x08、PF=0x10、AF=0x40，All=0x5f；Condition为EQ..NV的中立条件语义。前端不包含ARM64头、不选择物理寄存器，也不解释后端flags字的布局。CanonicalCarry使Sub/Neg的C为x86 CF；因此JB/JAE读取C，JBE/JA按C或Z读取，不能误用假定C为“不借位”的HI/LS。signed、Z、O、N及parity条件分别保持原语义。

后端`CompileOptions{flags_offset}`显式指定state中一个8B内部flags字；默认UINT32_MAX禁用。它不是guest RFLAGS内存布局，x86调用方必须把该槽与GPR/PC字段分开；不能猜旧cpu结构offset。该ARM64 ABI沿用NZCV位31..28、AF位26、PF原始结果字节7..0的carrier，并保留未声明位。GetFlags读取当前位置完整raw字、忽略mask，不把Value依赖误当producer快照。未来其它后端按语义合同选择实现；本轮没有引入跨后端的物理寄存器映射。

启用后使用当前未分配的x1保存唯一真实flags值；x0/16/17仅作局部临时值，不扩大14寄存器分配bank、不增加frame或per-node分析。单位入口加载，普通guest-memory访存可能fault前提交，Return/Dispatch/Unreachable前提交。与该字段重叠的Uniform读先同步；重叠写先同步后重载，原始memory写后也重载以处理地址别名。未启用时不产生这些load/store或shadow操作。

metadata继续按明确合同接受U32/U64：ArithmeticRaw/ArithmeticCanonicalCarry限Add/Sub/Neg；LogicalZeroCVA限And/Or/Xor；mask必须在All内，branch_only仍拒绝。Add/Sub/Neg当场捕获NZCV，按profile和声明mask合并；只有声明C的Sub/Neg canonical写才翻转C。PF保存低字节、读时求偶校验，AF算`bit4(left ^ right ^ result)`（Neg为source ^ result）；logical仅清声明范围的C/V/AF。部分写保留其它flags，不以一条ADDS/CSET冒充All已实现。

CondSet/LocalCondSet需要时从x1恢复NZCV再求0/1，AL/NV恒真；TestZero等普通CMP不会破坏客体flags。TestFlags保留既有合同：NZCV组选中位any-set，PF/AF与该组结果作AND，TestNotFlags取反。CondSelect、Select、ClearFlags、SetCarry/Overflow、InvertCarry及显式LocalParitySet均已接入，未实现的FP/branch-only等继续拒绝。

Legacy SaveFlags只接受同块紧邻producer，或中间一条与Save mask不交的ClearFlags。writer常数查看最多后两条，在原producer当场capture，Save位置消费单个即时pending ID；允许此域内8/16位精确运算旗。其它间隔、相交Clear或与metadata冲突的形态明确Unsupported，不为旧格式恢复producer投影、flags图或证书。现有B的Sensitive/pin继续保护其opcode/操作数语义。

后端可以同步flags槽，意味着普通Uniform读可观察该字段；因此B必须在WriteFlags处使原Uniform cache失效。修复只增加原effects判定的一bit，没有让优化器认识ARM64布局或传入一套额外facts/options。

### 14.3 本轮验证、成本和后续

Host Release 5/5、严格halt ASan+UBSan 5/5通过。红魔五项合同通过，后端724项检查含真实原生执行；pipeline覆盖12条原路径、4170组ALU/宽度/立即数/16条件/两scope及Known/Unknown/EarlyOff组合，实际核对数值、flags、PC与被选边；另外核对延迟发布、真实访存读取flags槽及memory别名写后条件读取。没有展开中途QoR/PMU矩阵。

同轮CPU4、walt min614400/max1171200kHz，前后cur1171200kHz，电池27.9°C，未锁频。固定128请求、1 warmup/5样本，整个构建→优化→分配→编码→释放为：

| 输入 | 实际N/请求 | Block中位 | Function中位 |
| --- | ---: | ---: | ---: |
| flags off，256算术节点 | 320 | 13.515834ms | 13.764427ms |
| flags off，1024算术节点 | 1088 | 46.619843ms | 46.862916ms |
| 高密度flags on，256算术节点 | 352 | 18.088855ms | 18.287969ms |
| 高密度flags on，1024算术节点 | 1216 | 63.876719ms | 64.169635ms |

每请求仍19个真实spill槽、176B总frame。flags on输入特意让每个算术节点声明All，并让条件结果通过Select进入实际数据链，不能全部DCE；这是高密度组件输入，不代表前端pending优化后的应用分布。当前切片修正版源码摘要`26e1c0577d2eaf06b69b05a63f5c0a0bd1299bf030c5f334a5b253a04967dc0f`。基准可执行文件与初轮完全相同，输入/后端未变，因此原始样本保留，不为接口回归重复测量。上述均为synthetic scalar序列，仍不含运行时发布；不能和旧FEX总数拼接宣称≤2×，A/full正式门继续缺证据。

源码、失败工作账和设备样本在`phase-c-integer-20261103`。本轮发现Data2仅余约1.2GiB，先核对无打开文件/活动writer，清掉本线停用Android ThinLTO缓存文件，保所有二进制/静态库/配置、15个冻结trace link输入及当前重建缓存；独立du/df确认可用空间增加3,557,474,304字节，详细回执已追加到协调者原清理记录。

该检查点剩余单一decoded buffer上的自动Function发现/迟到目标拆片等工作，已由第15节接入；剩余ISA与helper、guest-fault恢复及发布/cache生命周期继续按实际消费者补，不把flags闭环当作完整VM或最终生成质量验收。

## 15. 单缓冲区Function发现、实际入口与纯整数覆盖（2026-11-03）

### 15.1 当前文件结构与责任

本轮只增加一对生产源文件`frontend/x86/decode.h/.cpp`，用于实际字节缓冲区与区域发现。没有Function manager、CFG投影、第二套operand、分配修正表或证书。当前构建输入56文件（含新夹具/基准），IR布局仍24/24/8。

```text
source/runtime/ir/
  program.h/.cpp          # 单一owner、真实EntryBlock及只读视图
  builder.h/.cpp          # 局部写入/SetMetadata/SetEntry、O(1)提交
source/runtime/frontend/x86/
  decode.h/.cpp           # 一份实际_DInst缓冲区、可达发现/迟到拆片/预算
  build_ir.h/.cpp         # 借实际记录一次发射IR；字节/已解码/自动区域入口
source/runtime/compile/
  optimize.h/.cpp         # 原三轮公共优化，不新增分析平台
source/runtime/backend/arm64/
  allocate.h/.cpp         # Block本地；Function从实际Entry做唯一RPO
  compile.h/.cpp          # 直接读原IR与真实home编码，含flags和纯整数
source/tests/rebuild/
  function_fixture.h      # 带不可达坏字节、循环和高PC入口的真实x86夹具
benchmarks/
  function_compile_bench.cpp # 相同机器码的provided-decoded/automatic两端点
```

`DecodeLimits{max_instructions=4096,max_blocks=256}`限制实际区域；这不是按ABI符号定义推断的完整guest函数。`DecodedFunction`只持真实`vector<_DInst>`、16B `DecodedBlock`切片、实际entry索引及少量aggregate工作账。原`BlockWindow`和`DecodedBlock`唯一声明移到decode.h，旧include调用方保持兼容。其它生产模块不包含该ISA decoder头。

### 15.2 发现、迟到拆分和预算

`DecodeFunction(readable_windows, entry_pc, limits)`从实际entry迭代发现可达顺序与直接JMP/Jcc边；RET及其它decoder标记的控制流终止当前流。未知指令保留实际记录，由原Emitter拒绝，不复制supported-opcode表。只读window必须有序、非空、不重叠；指令需完整落在一个window内，独立span边界作为真实退出，不拼padding重解码猜指令。不可达坏字节不扫描。

一个稀疏`map<PC, Slice{end, first, count, pending}>`管理实际覆盖和待发现入口；待处理stack为O(B)。块体开始时缓存下一已知入口和window边界，顺序解码期间不逐D查map。新target落在既有slice内时，对该slice的实际`_DInst.addr`二分，只拆first/count/end；没有销IR、重发IR、排序或复制整份D记录。普通vector几何扩容仍可重定位，目录存index，不承诺裸指针稳定。

S个windows一次验证，D个实际记录只解析一次；工作为`O(S + D + B(log B + log S + log D))`，辅助目录/stack和最终切片为O(B)，没有每D哈希节点或每byte bitmap。闭合时按目录顺序一次输出B个切片，实际D数组仍按发现顺序。`BuildFunctionIR`在这之前不出生IR，闭合后复用`BuildDecodedIR`唯一发射入口。

max_blocks计已接纳的guest入口（包括pending和迟到split），不等于最终IR block数：外部Jcc出口stub仍按实际生成的B另计，最多增加每guest块两条唯一外部边。N耗尽在真实指令/边frontier退出；未解码target可外退，已pending入口保留零指令真实Dispatch块；N满后已有记录的late split仍可在B预算允许时完成。B满时新入口/split外退，不伪造缺失指令。外部target退出；落在已解码指令中间的target明确`OverlappingTarget`失败，不支持重叠指令流。

formation失败保留`DecodedFunction`实际decode_calls（含失败尝试）、committed bytes、splits和budget_exits；其IR未出生，所以`BuildFunctionIR`的accepted bytes/instructions及WorkCounts为0。完整失败调用的材料化/目录/释放成本仍计费，不能从这些0推导0成本。旧`BuildIR`保持原prefix-first错误与实际弃置账，不被新API替换成假成功。

### 15.3 实际入口与后端整数语义

发现顺序、guest PC排序和真正入口是三个不同事实。Program/Builder新增`EntryBlock()`和O(1) `SetEntry(BlockRef)`，保存同一Storage中的实际入口；首次CreateBlock默认0，旧输入兼容，foreign/invalid/consumed保持typed拒绝。字段为per-unit，当前利用已有padding，不增加node或block记录。Submit只附常数入口范围检查，不加CFG/use扫描或分配；moves/Editor保留入口。

自动区域的sorted-PC切片不再把最低PC冒充entry，也不重排原BlockId。Builder设置实际entry，Function分配从该Entry开始既有RPO，编码消费同一schedule；Block仍只有0号本地路径。无新增入口跳板、ID映射表或dom证明。旧`canonical_semantic_v1`及replay-v1夹具限定entry0，当前资格读口显式拒绝其它entry；新区域基准将实际entry、工作账和机器码共同核对，不把旧hash当任意入口的完整语义身份。

本轮补齐纯整数Mul/MulHigh、BitExtract/Insert/Clear、ByteSwap16/32/64、CLZ/CTZ32/64及PopCount。位域与PopCount/MulHigh按已明确的raw-container合同读取（仍验证integer view），numeric输出按自然宽度归一化；位域count0有定义，非法64索引/越界长度不按位截断蒙混。ByteSwap按显式宽度，CLZ/CTZ的零输入返回声明宽度。MulHigh仅明确的64×64 signed/unsigned high合同，未扩展metadata flags白名单；Div/ADC/SBB、FP和helper仍不因此被宣称支持。

LoadImm实际数值此前未按自然宽度截断，raw消费者会暴露脏高位；现只在host编码立即数前mask到自然位，IR完整literal仍保留，且避免发射无用高位MOVK。BitCast/GetFlags的raw carrier保持不变。PopCount使用baseline ASIMD固定v0临时四条指令，v0为当前全整数单元中的caller-saved scratch；不增加FPR分析/frame/helper，后续加入vector home时必须把这条真实scratch约束纳入该bank。

### 15.4 验证与当前成本

Host Release与严格ASan+UBSan均5/5通过；红魔五项合同通过，后端1085项检查含真实执行。原12+4170条数值/flags/Jcc路径全部保留，新增22条自动区域执行覆盖非最低PC入口、循环、late split、不读坏padding、预算frontier以及Known/Unknown/EarlyOff。结构用例另覆盖diamond、128深链迭代、准确N/B预算、先有与后遇指令中间target、外部边及零IR失败账。不是逐helper QoR/PMU矩阵。

新增基准使用相同的真实x86区域、flags ABI及机器码：一个端点每请求自动发现（**含byte decode及buffer材料化**），另一个借已提供的decoded owner，只计发射→优化→分配→编码→释放。每个请求都有未计时复核；两个端点的Entry、N/E/W/B及机器码摘要一致，计时中无CountWork或payload扫描。已提供buffer的首次准备在计时外，明确不是完整请求成本；两者相减也不能叫纯decode或纯formation耗时。

CPU4、walt min614400/max1171200kHz，前后cur1171200kHz，电池30.2→30.3°C，未锁频；每批128请求、1 warmup/5固定样本：

| 循环块 / 总guest块 | N/请求 | 已提供解码输入 | 自动发现，含decode |
| --- | ---: | ---: | ---: |
| 4 / 6 | 37 | 2.421823ms | 2.898021ms |
| 32 / 34 | 205 | 10.465677ms | 13.421666ms |
| 128 / 130 | 781 | 39.104740ms | 50.848282ms |

同轮原320节点、19 spill/176B frame夹具，Block/Function为13.606771/13.803646ms。上述都是synthetic partial-ISA组件，均不含运行时发布；不能对旧FEX总数作比例或授予全应用≤2×。当前输入摘要`cef79654912d104cddc0de058eaff1533af347616129e9200956773a93522b39`，源码/设备hash、原始样本及临时namespace清理在`function-discovery-20261103`。

当前剩余是其它ISA/调用与helper、guest-fault恢复、发布/code-cache生命周期及完整应用fresh FEX对比和最终QoR。自动区域并没有补齐ABI call graph或重叠指令流。后续继续只为真实消费者增加工作，不恢复旧中间层。

## 16. 实际应用执行与首次同口径 FEX 诊断（2026-11-03）

本轮从第15节的可执行片段推进到实际静态 ELF，不恢复旧中间链。当前源码快照70文件，摘要 `91135b9e34fc748a29a3d240d1ec65814de9797640603c8bcfc804ee86a1da83`。原树2404文件、删除清单、Git HEAD和index再次核对均未改变。实现过程中只做受影响检查和真实应用短跑，不逐helper增加QoR/PMU矩阵。

### 16.1 当前文件职责

```text
source/runtime/ir/
  types.h / opcode.h / opcodes.inc   # 中立身份、类型、schema、effects及语义合同
  program.h/.cpp                    # 唯一真实操作/operand/block存储与只读口
  builder.h/.cpp / builder_impl.h    # 出生/编辑/入口/闭合/移交；不做全域分析
source/runtime/frontend/x86/
  decode.h/.cpp                     # 实际字节记录、可达区域与迟到切片
  build_ir.h/.cpp                    # 解码记录→中立IR；标量/flags/调用/SSE2直接发射
source/runtime/compile/
  optimize.h/.cpp                    # 唯一公共编辑流程与阶段内u64[N]表
source/runtime/backend/arm64/
  allocate.h/.cpp                    # Block本地或Function实际RPO、一个真实home表
  compile.h/.cpp                     # 读取原IR/home直接选码、编码及局部flags同步
  integer_helpers.cpp               # 真实2N/N除法与固定保home的私有调用ABI
source/runtime/execution/
  guest_state.h                     # 应用实际GPR/PC/flags/TLS/XMM布局
  memory.h/.cpp                     # ELF/栈/guest映射唯一owner与目录
  native_code.h/.cpp                # 实际可执行slab、发布/清cache/释放
  runtime.h/.cpp                    # 单张PC表、冷compile、dispatch与窄syscall服务
source/launcher/linux/
  main.cpp                          # 应用CLI及显式诊断输出
benchmarks/app_inputs/
  build_coremark.py                 # 固定官方版本、原核心源与普通O2构建
  coremark_linux/
    core_portme.h/.c / start_x86_64.S # 官方算法之外的Linux syscall port
```

不新增MIR、flags plan、分配修正actions、correctness certificates或生命周期包装链。`allocate.cpp`删掉Builder/Program已保证的结构/owner/terminal全N复查，继续在消费时处理不支持的语义。向量与整数共用同一16B/home表：标量x2..x15；V128使用v2..v7/v16..v29；v0/v1/v30/v31为编码器真实临时，v8..v15按宿主ABI保留。向量spill为对齐16B，槽总量按8B单位统计，不把它误叫spill值数。除法remainder槽放在同一home已有padding字段中，不引入配对结果目录。

### 16.2 从真实消费者补齐语义

前端新增8/16位读写（含AH及REX低字节）、MOVZX/SX/SXD、SETcc/CMOVcc、内存ALU、INC/DEC、ADC/SBB、乘除、移位/旋转、BT、隐式符号扩展，以及CALL/JMP间接形式、PUSH/POP/LEAVE、RET imm和SYSCALL。实际CoreMark O2含19种SSE2传送/整数运算：采用既有中立VecIR，不禁vectorize、不改算法来缩成标量子集。没有据静态opcode名称齐全就宣称完整ISA。

32位写通过`StoreUniform(U64 slot, Value U32 view)`表达读低32并清高32；不再先建独立ZeroExtend节点。所有target模式共用中立logical-view合同，raw BitCast例外不变；公共Uniform cache保留该窄view，后续宽load以已有归一化转换读取。立即数移位直接产生Imm IR和源bit CF，不先出生count常量再走动态路径。动态count=0通过已有GetFlags/Select和新增`RestoreFlags(carrier, mask)`保留实际flags；carrier不暴露物理位布局给前端。INC/DEC保CF，窄算术按实际宽度产flags。

内存RMW先保旧fault状态，成功store后才公布新flags；当前用同实际operands再算一次纯ALU，不增加跨存储的隐藏NZCV寿命或SaveFlags证明域。CALL先读取间接目标再压栈；POP的RSP相关EA按更新后的SP算，但fault前不提前提交SP。SYSCALL提交next PC后以既有native返回值1交运行时；普通dispatch返回0，不保额外reason字段、reset或两份状态描述。

Div128的三条value view显式同宽N（8/16/32/64），表示2N/N，signed立即数仍0/1。实际helper检查零除与N位quotient溢出，返回U64 carrier的quotient/remainder；不假定high=0。错误当前进入宿主SIGFPE，尚不是完整guest信号恢复服务。

### 16.3 生命周期与直接编码成本

CodeArena复用1MiB slab；Linux/Android优先memfd RW/RX双别名，发布只复制和两别名cache维护。受限平台有slab级W^X fallback并输出实际使用状态。缓存项只借用entry/size；目录是一张连续开放寻址表，无链表节点、第二direct cache、tombstone或逐dispatch时钟。只在cold插入记录表容量。所有IR/字节buffer临时owner在compile请求计时结束前释放，代码/cache清理及失效成本另入同一compile总账。

标量TSO自然对齐使用baseline LDAR/STLR，8bit无对齐分支；16/32/64在实际EA上局部测试，自然未对齐走plain访存及load后ISHLD/store前ISH。向量维持半屏障。没有因手机有LRCPC/LSE2就假定所有未对齐LDAPUR/STLUR无fault，也没有关闭TSO。FEX此类快路依赖SIGBUS回补，本轮不复制这套运行时。该修改保持原plain未对齐访问的原子性边界，不能据CRC宣布完整并发内存模型验收。

编码器对已自然归一化的数字不重复mask；raw carrier仍按消费view截断。32位ALU直接W形式，立即数算术/移位及常见shuffle当场选码。一个block内仅用一个dirty bit避免重复存同一flags shadow；块入口保守视为dirty，任何真实写flags重新标记，fault/alias/出口仍同步。不建逐node同步计划。公共优化仍三轮：仅实际rewrite时收集旧uses；固定8项Uniform cache用占用mask清空，store失效只做一次，不每次清整份payload。

### 16.4 实际应用证据与剩余差距

输入采用官方CoreMark `1f483d5b8316753a742cbf5590caf5bd0a4e4777` 的5个核心C文件及header原样，普通`-O2`、真实SSE2；独立Linux syscall port处理启动、write、clock_gettime和exit。guest SHA为 `4b6062ed77ba2e3e94699264a4daeadf5b9b8df57a4a3f1a50cda73fb7a64546`。SVM/FEX使用同一个22,584B ELF。它覆盖真实核心算法，不包含旧1.9MiB Bionic产物的libc启动和其它ISA，因此不能拿旧guest的compile/PMU数据拼成新程序比例。

20次迭代两scope均完成，crclist=`e714`、crcmatrix=`1fd7`、crcstate=`8e3a`、crcfinal=`4983`与同轮FEX完全一致；Function118次请求均成功，无Block fallback。Block524次请求、13,084出生nodes；Function18,561 nodes。当前合计spill槽仅2/3个8B单位，最大frame32B。Host第7检查点五项通过；第9检查点受影响优化合同通过；Android第8检查点1124项backend检查及原12+4170 pipeline路径通过，随后第9检查点直接用两scope真实应用重复确认CRC。没有把此前sanitizer结果自动归给这一整轮新运行时。

红魔NX729J/bf1359ff、CPU4，walt min614400/max1171200kHz、前后cur1171200kHz、电池29.9°C；无锁频。三次fresh进程、同一20次迭代输入，只有纯解析调用被扣：SVM围实际`distorm_decompose`；FEX新增scope只围`DecodeInstructionImpl`，原outer `raw_decode`（buffer/形成/拆块等）保留计费。两者实际decoder内部输出记录初始化属于该实现；caller记录复制/目录/IR工作均不扣。探针默认关闭，开启会扰动时间；没有把aggregate ticks伪造成绝对wall interval。

| 真实应用诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| compile去纯byte decode中位 | 10.142ms | 12.737ms | 6.784ms |
| 相对同轮FEX | 1.495× | 1.877× | 1× |

分项独立中位仅用于定位，不能假定其和恰好等于总中位：Block A/B/C/D约3.050/2.202/3.131/1.034ms；Function约4.422/2.751/4.426/0.848ms。A保formation与IR，D含发布和最终code/cache释放。第9节A/B小预算仍有超额：Block B约0.325×FEX；Function A/B约0.652/0.405×，未满足0.50/0.25×。总诊断进入两倍范围不等于完整预算PASS，三次短跑及侵入探针也不能代替最终普通镜像验收。

执行质量仍需工作：第6检查点500次迭代Function约476.9ms，同guest、显式一致TSO策略的普通FEX约78.0ms，CRCfinal均`a14c`，约6.1×。两者均不足10秒，不是有效CoreMark score。后续必须压缩guest-state/flags工作、优化实际内部调度与区域重复成本，并在功能收敛后集中验收真实运行/PMU；不因compile接近就降低生成质量目标。

记录在 `application-runtime-20261103`，包括初始Unsupported前缀、实际源码/镜像、原始输出、byte-compare样本及FEX补丁构建/恢复。独立FEX缓存五个TU插桩后已逐字节恢复ordinary源码及原平台diff，并重建ordinary缓存；没有改冻结的SVM原树。完整预算门仍缺最终原始interval/普通镜像与覆盖证据。动态ELF、完整ISA、guest-fault恢复、写驱动SMC和更广实际应用覆盖继续开放，不宣布整个重构完成。

## 17. 原生执行链、故障恢复与实际热代码收敛（2026-11-03）

本节记录pin12与packed flags的历史检查点，当前实现见第18节。原树继续冻结，所有修改在重建树；各节数值保留其历史版本范围。用户最新约束继续适用：不做过度防御，不为每个中途改动增加测试、完整质量矩阵或反复PMU；保留真实语义合同，必要集成确认集中进行。

### 17.1 文件责任与运行路径

IR仍为24B Operation、24B BlockRecord、8B word及唯一Owner；提交仍为O(1)。本批新增的生产文件只有`execution/fault.h/.cpp`和`execution/dispatch.h/.cpp`，没有新增中间pass、N级镜像、修正action或证书。

| 文件 | 当前实际责任 |
| --- | --- |
| `ir/program.h/.cpp`、`builder.h/.cpp` | 原存储和局部写口；高频Operations/Operands/Blocks等叶子读口在头文件内联，保持原公开边界检查 |
| `frontend/x86/build_ir.cpp` | 同指令同GPR同宽读复用；按真实消费者公布pending flags；FaultLocation能力下只在真正退出写PC，内部Jump不再写PC |
| `compile/optimize.h/.cpp` | 同Owner三轮优化；固定Uniform cache、明确flags字段/私有状态别名政策；等价归一化拷贝转发、候选opcode折叠和DCE |
| `backend/arm64/allocate.h/.cpp` | 分别做Block本地分配或Function实际RPO/寿命；一个16B/home表，按实际可用bank分配 |
| `backend/arm64/compile.h/.cpp` | 直接读IR/home编码；一个局部NZCV一致掩码；输出真实FaultSite/LinkSite，管理本unit的spill frame |
| `execution/dispatch.h/.cpp` | 原生ABI根帧、同一PC目录探测、冷解析/服务返回；实际固定寄存器的发布/恢复 |
| `execution/fault.h/.cpp` | 信号注册/恢复、host-PC定位guest-PC、未对齐救援及致命故障状态恢复 |
| `execution/native_code.h/.cpp` | 代码slab及真实故障表唯一owner，RW/RX别名发布与释放 |
| `execution/runtime.h/.cpp` | 唯一64B/slot PC目录、pending链接、冷compile、失效和syscall；不参与指令选择或分配修正 |

普通执行为`native unit → 已解析链接/原生目录 → native unit`。只有冷miss进入C++编译，syscall和错误返回服务层；不逐unit往返C++、保存根帧或重新加载全状态。根帧一次保存宿主callee-saved寄存器，每个unit只调整自己实际spill区。静态出口使用真实8B DATA relocation，目标发布时只遍历该目标已有等待链；没有第二direct cache或每次发布全扫pending列表。

Block条件出口保留原Select和StorePC，只在出口比较已提交的PC，选择两条已有链接；不读取已超过寿命的condition home，也不把Block改成Function分配。Function内部边仍直接使用原CFG和实际home。普通dispatcher不执行逐hit计数；`--decode-timing`一次选定计数版本，JSON明确标识这些是实际目录查询数，已解析链接不计。

### 17.2 固定状态与flags

运行时固定12个客体GPR：低8个在x20..x27，R8..R11在x12..x15，SSA标量bank相应为x2..x11。额外4个属于宿主caller-saved，冷C++返回后从已发布状态重载，失败返回也重载；既有除法私有stub已保存x1..x15，不再加一层保存。致命故障从实际ucontext恢复这些值。后端默认pin8入口仍可用，未启用该ABI的通用入口保留原14个SSA寄存器。前端没有物理寄存器编号。

packed flags的真实值保留在x1。只在明确的私有CPU状态ABI下，guest访存不触发多余shadow读写；通用别名路径仍同步。Writer的一个掩码记录host NZCV中哪些位已与x1一致，条件消费满足时省MSR；真实CMP/TST、helper或部分flags写入更新该状态，块入口不猜跨块一致性。完整MRS/packed值合同保留，未猜Jcc后flags死亡。32/64位AND带N/Z输出直接ANDS，其它32位logical使用TST W；AF/PF与部分mask语义不变。

公共优化的归一化转发以实际carrier相等为条件：自然数字宽度已满足截断时才消掉ZeroExtend，窄view读取较宽源、BitCast/GetResult/GetFlags等raw载荷仍保留必要转换。连续Uniform读缓存真实源/view，不串出一条归一化wrapper链。LoadImm/FaultLocation等不再构造无用的Operand草稿，Fold先排除不支持折叠的opcode；没有新增常量分析表。

### 17.3 故障PC与内存顺序

`FaultLocation(guest_pc)`是中立位置标记。具备真实consumer时，后端在同一编码循环记录每个实际MayFault操作的native范围，成功路径不再反复写GuestPC。Unknown/EarlyOff或无该能力时仍使用明确PC写入；实际退出始终发布目标PC。外部除法helper的fault不在JIT范围内，调用前仍明确发布PC及固定状态。

标量有序访问保留TSO：实际HWCAP支持时选RCpc load；未对齐SIGBUS只针对该操作记录的fast word，回到真实plain访存/半屏障fallback，有RW别名则就地回补，后续不再重复trap。向量保原半屏障。唯一PC目录在故障时提供native范围查找，不新增热路径故障注册表。致命memory/alignment fault恢复准确guest PC、固定GPR及flags，再返回运行时错误；这仍不是完整guest Linux信号投递。

FEX参照源码确实使用RCpc与SIGBUS回补；另外`IsNonTSOReg(DEFAULT,RSP)`会把普通RSP寻址标记为NonTSO。当前SVM未照搬该语法假设，仍对这些客体访问保留有序语义。因此下表是相同显式TSO配置的实际产品行为比较，不能声称双方每个访存的屏障策略完全相同。后续优化必须明确语义来源，不靠全局关闭TSO降低数值。

### 17.4 当前结果与剩余工作

沿用同一官方CoreMark O2 ELF、红魔CPU4及固定FEX版本。最新组前后walt为614400–1171200kHz、cur1171200kHz，电池27.6°C；未写锁频配置。三次fresh进程的诊断compile只扣实际字节解析调用，formation、IR、优化、分配/编码、发布和释放保留。两个范围均完成所有CRC，Function没有Block fallback。

| 最新诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯字节解码compile中位 | 11.422ms | 12.465ms | 6.639ms |
| 相对同轮FEX | 1.720× | 1.877× | 1× |
| 普通镜像500次guest计时 | 389.001ms | 306.857ms | 77.833ms |
| 相对同轮FEX运行耗时 | 4.998× | 3.943× | 1× |

Function出生N从第16节18561降到16272；native bytes124008。Block出生N12038，native bytes103944。合计spill槽仍为2/3个8B单位，最大unit frame16B。Block实际796条静态链接，诊断目录查询从上个native checkpoint的1284790降到122904（20次输入）；这些是不同层面的工作量，不混称执行unit数量。Function运行从早期476.9ms降到306.9ms，但最终运行质量仍未达收尾条件。

这轮集中沿用现有构建/合同检查和真实应用短跑，没有新增测试文件或按helper展开矩阵。集成时发现AND+TST合并曾把logical opc只置一位而编码成ORR，导致启动栈对齐错误；现已改为ANDS实际opc=11，失败镜像/IR/反汇编保留，失败样本未计入上表。修复后的两scope及原native数值/flags路径通过。此前ASan/UBSan结果不自动当作本轮运行时的完整验收。

结果为短诊断，不是有效CoreMark score或正式预算/质量PASS；没有把累计ticks伪造成绝对interval。总compile诊断在两倍范围内，A/B阶段小预算和最终普通镜像覆盖仍开放。后续重点是flags实际发射和区域/分支成本、剩余状态访存与完整功能覆盖；保持当前唯一存储、唯一home和直接机器提交，不再扩展inputs/preparation/actions/certificates链。

源码、镜像及样本在`qor-local-state-20261103/native-dispatch/pin12/measure`；上一个完整pin8检查点在`compact-hotpath/final-fixed`。PMU及实际RX快照在`native-dispatch/profile`，它观测的是该处记录的旧native镜像，不能当作当前pin12重新采样的结果。

## 18. IR出生收敛、直接选码与原生NZCV（2026-11-03）

本节记录native-NZCV运行检查点，当前进度见第19节。施工树、唯一IR owner、一个公共优化器、独立Block/Function分配和直接机器提交保持既定责任。没有增加生产源文件、IR镜像、flags计划表、验证pass或分配修正链。过程中以已观察到的实际工作为依据，不把每个辅助函数都变成独立测试或PMU门。

### 18.1 当前文件与决策归属

| 文件 | 本批实际工作及边界 |
| --- | --- |
| `ir/types.h`、`ir/opcodes.inc` | 增加中立`ConditionalUniformStore`能力及`StoreUniformSelectImm`操作，表达按逻辑布尔值选择两个完整U64立即数写入状态；不出现ARM寄存器或编码 |
| `frontend/x86/build_ir.cpp` | 移位的flags用固定大小pending recipe延迟到首个真实观察者；被后续完整ALU覆盖的recipe不出生IR。Known Block Jcc直接出生条件PC写，Unknown/EarlyOff保一般Select/Store；Function继续内部CFG |
| `compile/optimize.cpp` | 继续同Owner的公共三轮优化、单个u64/node表；不加入新的截断分析或flags数据流求解 |
| `backend/arm64/allocate.h/.cpp` | 借用同一实际CompileOptions；唯一16B/home表内，将唯一消费者为相邻完整状态写的U32/U64数字结果直接放到目标pin。保留raw transport及部分写边界 |
| `backend/arm64/compile.h/.cpp` | 本地选择MOVN/逻辑立即数；直接消费条件PC写并发出两条链接；提供packed/native两种flags编码；记录实际两字TSO回补位置 |
| `execution/dispatch.h/.cpp` | 实际HWCAP_FLAGM一次选择固定native/packed dispatcher入口；进入目录探测前导出NZCV，命中/冷返回时恢复。普通hit不检查模式 |
| `execution/fault.h/.cpp` | 未对齐故障将原两字访存改成plain访问与半屏障；支持RW别名和实际页范围W^X。致命故障从ucontext取真实NZCV，与x1和pins合并 |
| `execution/runtime.cpp`、`launcher/linux/main.cpp` | 按实际宿主能力设置同进程ABI，Block/Function/fallback不混模式；输出实际`native_nzcv`和flags政策 |

移位recipe保留原来移位前的flags边界，保证count=0、未定义/未写字段及AF策略；只推迟其后纯标量合成。首个CF等观察者仍完整合成，不能把INC/ADC需要的旧CF丢掉。当前应用只实际覆盖常量count，不借此宣称动态count应用覆盖。

相邻home合并只接受同块、下一个StoreUniform是最后且唯一data use、完整U64 pin slot及足够宽的数字view。BitCast/GetResult/GetFlags、branch-only raw结果、8/16位部分写和U64截成U32不进入该合并。它修改真实位置决定，原Store自然成为同寄存器零指令，不创建额外action。

逻辑立即数本地匹配A64重复元素/旋转连续位形式，失败直接回已有寄存器形式；MOVZ/MOVN按实际半字选择，随后只补不同MOVK。算法参照[LLVM的AArch64逻辑立即数编码](https://github.com/llvm/llvm-project/blob/main/llvm/lib/Target/AArch64/MCTargetDesc/AArch64AddressingModes.h)，没有引入通用选码规划器。

### 18.2 Native NZCV是执行ABI，不是新的IR flags结构

`CompileOptions.native_nzcv`默认关闭，只在实际FEAT_FlagM存在时启用。每个IR操作边界、内部边和直接链接边上，硬件N/Z/V等于客体值，硬件C是中立IR Carry的反值。x1继续拥有PF的原始低字节、AF和所有reserved bits；NZCV nibble可以过期。Writer只有一个`packed_current`位，块入口保守置为过期，没有N级状态表或跨块求解。

完整Add/ADC在硬件产出后翻C；canonical Sub/Neg/Sbb自然满足反C，raw形式另翻转。部分S形式先用x30保存旧NZCV，再RMIF恢复未写字段。CFINV/RMIF实现真实单字段写；PF/AF仍在x1维护。GetFlags、显式shadow访问、helper和宿主返回才形成真实packed字。合成CMP/Select等先保存、后恢复客体硬件flags，不能让内部谓词破坏客体状态；ADC/SBB当前局部复用既有通用emitter再恢复，不复制第二份窄算术实现。

静态tail保持硬件flags，动态dispatcher在probe CMP之前导出，ready BR之前恢复。原生根入口从实际GuestState加载，不把宿主任意PSTATE当作客体值。`NativeRunContext`原字段offset不动，新增native模式字段在104，合计112B。实际`FaultSite`仍24B；致命fault由PSTATE翻C后合并x1，内部SIGBUS救援直接返回原context。

两字有序load为`LDAR/LDAPR; NOP`，store为`NOP; STLR`。第一次实际未对齐故障分别回补成`LDR; DMB ISHLD`及`DMB ISH; STR`，后续无对齐分支或重复trap。没有回补服务/位置标记的通用编译入口保留原局部对齐fallback。W^X使用运行时实际页大小，覆盖真实一页或两页；不制造RWX。向量半屏障及FEX RSP策略差异沿用第17.3节。

### 18.3 当前集中结果

同一22,584B官方CoreMark O2 ELF；FEX固定`a156752fae3a4f65fefb1ed95af980bcdfac43d7`及既有Android平台补丁，两者LTO均关闭。红魔NX729J、serial `bf1359ff`、CPU4；前后walt min614400/max1171200kHz、cur1171200kHz，电池26.9°C；未写锁频配置。

| 同轮诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 三次fresh进程、去纯byte decode的compile中位 | 11.098ms | 12.318ms | 6.774ms |
| 相对同轮FEX | 1.638× | 1.818× | 1× |
| 普通镜像500次guest计时 | 293.791ms | 242.684ms | 78.029ms |
| 相对同轮FEX运行耗时 | 3.765× | 3.110× | 1× |
| 20次输入请求数 | 524 | 118 | — |
| 出生IR nodes / operand words | 10958 / 17066 | 15847 / 23529 | — |
| native bytes | 81608 | 93288 | — |
| 合计8B spill槽 / 最大unit frame | 2 / 16B | 3 / 16B | — |

同输入CRClist/matrix/state/final为e714/1fd7/8e3a/4983；500次final为a14c。Function无Block fallback。现有host五项通过，Android沿用原整数flags/Jcc的12+4170路径并实际启用native模式；packed原生backend检查仍通过。现有未对齐输入在双别名及强制memfd不可用的W^X路径均成功，真实各2次恢复/2次回补；真实空地址fault报告准确guest PC。没有新建测试文件或扩大过程矩阵。

测量只扣双方实际byte decoder调用，保留formation、IR、优化、分配/选码、发布、失败和释放成本。当前compile仍为侵入式诊断，A/B小预算及正式普通镜像interval资格开放；500次不足10秒，不是有效CoreMark score。最终生成质量尚未达标，不因总compile诊断在两倍内而收尾。

证据根为`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-direct-emit-20261103`。`device-native`含原始命令、runner.gz、两scope/FEX输出、环境、源码及manifest；74个构建输入摘要为`5357d017dd3344c07a0aa8787f9217d64abc4a632c95ad785a2799e01293f497`。同根`source`是此前packed检查点，不能与`device-native/source`混用。

`profile-native-capture`保存这版2000次输入的simpleperf数据、已核对实际进程的RX快照及反汇编，4196样本、0丢失；解析到该PID的4115个样本中84.2% sample periods落在JIT范围。UBFX、MOV及BFI/BFXIL集中出现，指向窄读、状态搬运和flags合成；采样落点含流水线偏移，不把每种助记符比例当作精确可节省时间。快照不是原子冻结，旧packed profile也不当作这版数据。

下一步优先在现有读口/选码和IR出生处消掉重复截断、搬运及无效flags合成；不增加新pass、N级分析表或验证包装。扩充完整ISA、动态ELF、guest Linux信号服务、写驱动SMC及更广应用覆盖仍是整体未完成范围，功能收敛后再集中做正式compile与生成质量验收。

## 19. 截断、flags出生与实际pin home收敛（2026-11-03）

本节记录pin12和截断收敛的历史检查点，当前实现见第21节。生产文件数不增加；IR仍24B/24B/8B，home仍16B，Submit仍O(1)。不增加MIR、pass、N级flags/截断表、修正action或验证链。

### 19.1 删除实际重复工作

- `compile/optimize.cpp`：既有8项Uniform cache允许同offset的U64 entry满足U32 read，仍改写成真实ZeroExtend32。raw源和窄view继续按原合同截断，不把“同地址”当作可以直接别名。
- `frontend/x86/build_ir.cpp`：MOVZX的数字ReadData源已归零，直接按目标view写回，删除冗余ZeroExtend64；MOVSX保留符号扩展。显式IMUL16/32的数据低积直接产生MulN；溢出专用的宽乘积、high、expected和布尔判定只在真实CF/OF观察时出生。它复用Shift的同一固定recipe，完整flags writer覆盖时直接丢弃。
- 寄存器MUL/IMUL只接管CF/OF，保留旧ALU尚未观察的NZPA输出；Observe(ZF等)只回填旧producer metadata，Observe(CF/OF)才生乘法flags。连续MUL只替换同一recipe；真实memory operand和前一Shift recipe仍先Flush，不增加recipe链。显式IMUL16/32若实际需要溢出，会另算一次完整乘积，这是保留在成本账内的取舍。
- `backend/arm64/allocate.cpp`：相邻完整U64 slot的数字publication扩展到U8/U16。真正AL/AH/AX部分store仍不合并，raw载荷排除不变。
- `backend/arm64/compile.cpp`：完整pin写入直接以目标pin作Read的归零目的，删除scratch→pin的二次搬运；部分store保scratch以免清掉应保留位。已知实际距离的后向条件边直接用B.cond/CBZ/CBNZ；前向或超范围边保原long form，不添加branch relaxation pass或占位NOP。

### 19.2 只在安全寿命内借用完整pin

原有use遍历改为实际RPO和块内逆序，先计该块terminal use；max/loop_end寿命合同不变。每块固定12个下一次写入位置，结合已得到的last_use直接决定真实home，不新增遍历或N表。

在private Uniform ABI下，完整对齐U64 LoadUniform的所有使用必须严格早于该块结束和下一次同slot写入，才可令其实际home为pin，原快照MOV自然消失。U32/部分读取、跨块值及terminal use保持普通SSA home，不能因pin保存了64位载荷就省掉必要的零归一化。

游标同时计入部分StoreUniform、条件store、真实外部/状态写边界，以及相邻publication的**producer实际写pin位置**。后者早于语义Store点；严格小于禁止将仍在该producer输入中使用的旧快照借给被覆盖pin。完成这一个use walk后，bank分配只处理尚未决定pin的实际home，不做分配后修正或输出转换。

### 19.3 当前结果与成本边界

同一guest、FEX版本和显式TSO配置；红魔CPU4前后仍614400–1171200kHz、cur1171200kHz，电池26.9°C，无锁频。

| 当前同轮诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode的compile中位，3次fresh进程 | 11.226ms | 12.437ms | 6.697ms |
| 相对同轮FEX | 1.676× | 1.857× | 1× |
| 普通镜像500次guest计时 | 254.070ms | 203.267ms | 77.955ms |
| 相对同轮FEX运行耗时 | 3.259× | 2.607× | 1× |
| 出生IR nodes / operand words | 10542 / 16541 | 15364 / 22940 | — |
| native bytes | 75424 | 84768 | — |
| 合计8B spill槽 / 最大unit frame | 2 / 16B | 3 / 16B | — |

两个scope所有CRC匹配，Function118次请求无Block fallback。前一整批host五项通过；本次受影响的frontend/backend/pipeline三项通过，复用未改动的其余结果。Android沿用原native整数flags/Jcc路径，仅在现有pipeline测试加一个MUL用例，覆盖旧ZF保留、CF/OF观察和下一完整ALU覆盖；没有新增测试文件或过程矩阵。未改动的信号、W^X和发布实现沿用第18节证据，没有再跑PMU。

借用pin之前的`device-compact`检查点为compile10.483/11.929ms、FEX6.727ms（1.558/1.773×），运行257.863/210.196ms、FEX78.117ms（3.301/2.691×）。最新运行继续下降，但compile这轮上升，不能宣称本批每个改动都同时降低编译成本。backend独立中位从约3.28/4.01升到3.50/4.18ms，其余阶段也有变动；分项中位不相加，不把整轮差值直接归因给单一helper。

总compile诊断仍在两倍范围，裕量有限；A/B内部预算、最终普通镜像与覆盖验收继续开放。运行质量仍有约2.6×/3.3×差距，500次短计时不授予正式CoreMark或最终QoR资格。后续优先压剩余真实状态/flags工作及公共优化成本，不以新增证明层、缩减生成语义或关闭TSO换取数字。

原始结果和当前源码在同证据根`device-borrow`；`device-compact`保前一检查点。已有结构检查曾硬编码旧scratch→pin的MOV X，现改为检查实际清高半部的MOV W目的，首次中止记录单独保留；失败样本未混入上表。第18节PMU快照继续保持原镜像身份，不标作这版新profile。原仓库2404个冻结源文件、HEAD和index均复核未变。

## 20. 固定GPR、机器读取宽度与构建热路（2026-11-03）

本节记录width32检查点，当前实现见第21节。所有改动仍在重建树；文件组织和唯一IR owner不变。没有新增生产源文件、pass、N级表、验证平台或运行时实验开关。过程沿用现有检查集中收口，没有新建测试文件或逐helper测试矩阵。

### 20.1 实际16-GPR ABI

运行时固定16个guest GPR：低8个仍用x20..x27，高8个用x8..x15，SSA bank为x2..x7。ARM64后端仍接受原通用8/12-pin入口；运行时只选定16，不逐unit变更模式。FEX的非ARM64EC AArch64实现也为16个guest GPR加PF/AF静态寄存器，Android动态GPR bank为6；参考其`ArchHelpers/Arm64Emitter.cpp`的x64 SRA/RA，而不是凭寄存器数量猜收益。

`dispatch.cpp`热probe使用pc=x7、base=x2、slot=x3、tag=x4、index=x5、mask=x6、entry=x16，诊断计数只临时用x4，完全避开x8..x15。冷入口发布和重载全部caller-saved pins，成功及失败均保resolver返回x0；致命fault按实际slot数恢复映射。NativeRunContext仍112B，目录slot64B，FaultSite24B；除法私有stub本已保存x1..x15。现有两字TSO回补和flags ABI不变。

16-pin初始检查点为`device-pin16`，运行约3.194/2.591×FEX，compile约1.615/1.859×。合计spill仍2/3个8B槽、最大unit frame16B；没有因SSA bank缩为6而增加spill。实际收益较小，保留该结果，不把SRA数量齐全当作生成质量已齐平。

### 20.2 归一化在真实消费者处完成

`allocate.h/.cpp`在唯一实际Home中增加一个`narrow_pin` bit，Home仍16B。只有private ABI、同块、低32位对齐Uniform、所有使用严格早于块结束和下一次实际pin写的位置，才借用原64位pin；相邻publication预分配的pin优先，不带此bit。U64借用规则不变。

`compile.cpp`的读取区分三件事：IR logical view、显式limit/sign，以及最终机器消费者是否只读取W寄存器低32位。真实W形式的ALU、Mul、shift、STR和最终ZeroExtend32写入可直接消费低位，省掉中间UBFX/MOV；8/16位view仍截取。wide/raw读取和BitCast通过同一个RawValue入口对`narrow_pin`归零，不能把未归零的高半部泄漏给数值或raw carrier。源view大于32时，借用值仍是原U32数值，不能错误地按bit31符号扩展到64。

SignExtend的U32结果直接选择SXTB/SXTH W，一次完成符号扩到32及高32清零，替代SXTH X再MOV W。相邻完整pin写回、跨块值、terminal use和真实pin写点的严格寿命边界继续保留。前端和IR没有物理寄存器字段。

### 20.3 删除构建和提交中的重复工作

- `compile/optimize.cpp`把16项Operand草稿移到该walk外复用。实际Android旧汇编每eligible node有12条STP q0,q0清384B；现在整轮只初始化一次，ReadCells仍完整覆盖本次count前缀，long Params保持原处理。
- `ir/builder_impl.h`缓存本次node index，在容量耗尽时才调用原Grow。schema、限额、operand rollback和原push_back不变；没有引入未检查写口。Operand存储的容量快路原已内联，没有重复增加门。
- `backend/arm64/compile.cpp`直接消费默认metadata的Nop/FaultLocation，不进入完整标量emitter；非默认metadata继续原拒绝。Legacy邻接仍按真实NodeId判断，fault位置仅关联真实MayFault机器范围。机器words按实际N给`2*N+32`的reserve hint，不做预扫或容量证明。

### 20.4 当前集中结果

继续同一官方CoreMark ELF与固定FEX镜像，Release实际编译命令为O3/NDEBUG，双方LTO关闭。红魔CPU4前后仍614400–1171200kHz、cur1171200kHz，电池26.6°C，无锁频。

| 当前同轮诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode的compile中位，3次fresh进程 | 10.150ms | 11.925ms | 6.661ms |
| 相对同轮FEX | 1.524× | 1.790× | 1× |
| 普通镜像500次guest计时 | 245.706ms | 196.981ms | 77.673ms |
| 相对同轮FEX运行耗时 | 3.163× | 2.536× | 1× |
| 出生IR nodes / operand words | 10542 / 16541 | 15364 / 22940 | — |
| native bytes | 72992 | 81640 | — |
| 合计8B spill槽 / 最大unit frame | 2 / 16B | 3 / 16B | — |

所有CRC匹配，Function118次请求无fallback。现有host五项和Android数值/raw-view/flags/Jcc路径均通过；16-pin接线时集中核过原双别名/W^X未对齐输入和准确fatal guest PC，此后ABI未改，复用该证据。没有重复PMU或增加中途质量矩阵。`device-width32`保当前源码/镜像/命令/输出；`device-pin16`保前一检查点，第18节PMU身份不变。

这是短诊断，compile只扣实际byte decoder调用，其他工作继续计费；不授予最终interval、A/B分项预算或CoreMark/QoR资格。运行质量差距仍开放，完整ISA/应用服务覆盖边界沿第18节。

本检查点之后的TSO imm9工作已在第21节实现，以下一节的实际源码和镜像身份为准。

## 21. 实际地址、pin publication与原生handoff（2026-11-03）

本节记录`device-handoff`检查点；原树继续冻结。框架、唯一IR owner和前后端边界不变，没有新增生产文件、pass、N级分析表或修正/证明层。本批只在现有出生、use walk、优化walk和实际emitter中删重复工作。

### 21.1 文件职责及实际决策位置

| 文件（相对`source/runtime`） | 本批负责的工作 | 不向后续阶段传播的临时描述 |
| --- | --- | --- |
| `frontend/x86/build_ir.cpp` | 单次使用的访存直接携带中性`Add(base, final_disp)`表达式；寄存器/比例等仍由原Address逻辑处理 | 不产生EA facts、地址选择plan或物理寄存器 |
| `backend/arm64/compile.h/.cpp` | 实际选LDAPUR/STLUR imm9或原fallback；普通Add/Sub选imm12；实际PC出口交给dispatcher；逻辑NZCV直接选机器指令 | 不生成待执行actions或第二套地址/flags IR |
| `backend/arm64/allocate.cpp` | 在唯一逆序use walk内确定真实Home和pin写入边界，Block/Function路径仍分开 | 不产生publication修正表、copy计划或分配后证明 |
| `compile/optimize.cpp` | 在原walk内对flags运算的数据输入做严格等价copy forwarding | metadata、操作类型及flags profile不重建、不改成第二套语义 |
| `execution/runtime.h/.cpp`、`launcher/linux/main.cpp` | 一次读取实际HWCAP_ILRCPC，传入后端；输出实际启用情况 | 不把CPU寄存器或地址选码规则塞回前端 |
| `execution/fault.cpp` | 真实fault处将带imm9的有序访问回补为保留相同地址的LDUR/STUR和半屏障 | 不套用零offset LDR/STR模板 |
| `execution/dispatch.h/.cpp` | x0接现成GuestPC；热命中保存/恢复原始硬件NZCV，冷miss才打包到x1 | 不增加PC有效性表、flags缓存或第二个目录 |

IR Operation/Block/Cell仍24B/24B/8B，Home16B、NativeCode40B、DispatchSlot64B、NativeRunContext112B。Submit仍O(1)，固定16-GPR ABI及非已知后端的原通用途径保留。

### 21.2 地址在真正使用处选择

只为一次消费的普通Load/Store及不需额外对齐判断的SIMD访存借出最终位移；RMW、复用地址、LEA、栈操作及对齐检查保留完整EA。绝对/RIP地址或没有真实base的地址不强行拆分，V128也保留原表达式语义。

ARM64 emitter在实际capability、fault marker和恢复支持都满足时，局部匹配无shift的`base +/- imm`、范围[-256,255]，选择真实LDAPUR/STLUR。无capability或不匹配时仍执行完整EA；普通Add/Sub可直接选择正/负imm12或其12-bit shift形式，避免新表达式fallback退化为装常量再加寄存器。

SIGBUS回补保留size、imm9、Rn及Rt，转成LDUR/STUR；两条机器字的屏障顺序、RW/RX别名、W^X和cache维护仍由原fault owner负责。未关闭TSO，也没有照搬FEX对RSP的NonTSO假设。实现参考固定FEX源`JIT/MemoryOps.cpp:729/1737`和`ArchHelpers/Arm64.cpp:2105/2120`，不是另建一个地址优化pass。

### 21.3 真实Home延寿与flags输入转发

相邻完整U64 StoreUniform在自身真实use已计入后，直接决定前一个数字producer的Home。原单次使用publication保持；private ABI下若后续使用都严格早于块尾和该目标pin下一次实际写入，可让这个值继续住在已发布的pin中，删掉“先快照到SSA、再复制到pin”的工作。

每块原有固定16项下一次写入游标记录的是**producer实际写pin位置**，不是后面的语义Store点。跨块、terminal use、后续目标pin覆写或不满足原类型/raw约束的情况仍使用普通SSA。此判定只在Store处出现一次；producer处不重复描述，也不做第二次遍历。

带flags metadata的运算可以用既有ForwardCopy规则替换等价数据输入；完整metadata、自然类型、输出和effects保持。Fold与Uniform opcode改写仍显式限于无metadata节点；GetResult/GetFlags等敏感消费者的原限制保留。这样真正死掉的copy由原第三walk删除，无需专门的flags-copy pass。

### 21.4 出口与dispatcher交接

所有链接出口把实际GuestPC放在x0，state.pc仍提交。最后一条完整PC store可直接读到x0、写state、恢复frame并跳转；必须在frame恢复前读出spill PC，任何可能占用x0的flags打包先完成。普通fallback则在最后从state.pc读取一次。reason返回约定不变。

原生dispatcher入口只用x30保存硬件NZCV；probe期间不触碰x30和guest pins，热命中直接MSR恢复。只有真正cold miss才把保存的NZCV按inverse-C合同合并到x1，再发布状态并调用resolver。首次Root入口从真实packed flags恢复，不把任意host PSTATE当成guest状态。packed模式保留。

另在逻辑结果需要独立设置N/Z时用CMP result,#0，直接得到inverse guest-C所需的硬件C=1；已有ANDS仍按原合同翻转C。原PF/AF、部分flags保留和未知后端路径未改变。

### 21.5 集中测量与剩余差距

同一官方CoreMark ELF、固定FEX镜像、显式TSO配置；实际Release为O3/NDEBUG、LTO关闭。红魔CPU4前后614400–1171200kHz、cur1171200kHz，电池26.4°C，无锁频。

| `device-handoff`同轮诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode的compile中位，3次fresh进程 | 10.183ms | 11.965ms | 6.766ms |
| 相对同轮FEX | 1.505× | 1.768× | 1× |
| 普通镜像500次guest计时 | 223.949ms | 173.931ms | 78.721ms |
| 相对同轮FEX运行耗时 | 2.845× | 2.209× | 1× |
| 出生IR nodes / operand words | 10246 / 16541 | 14908 / 22940 | — |
| native bytes | 68120 | 74000 | — |
| 合计8B spill槽 / 最大unit frame | 2 / 16B | 3 / 16B | — |

此前`device-imm9`检查点compile为1.532/1.719×、运行3.069/2.452×；其源码摘要`67b54ac5a0c9e4ce41ec2b0ac9b9cec4f93531190776f6891208b1d2491289bf`。本批运行继续下降，但compile没有各scope都下降，不把短轮差值解释成单个改动的净成本。

两个scope的20次输入CRC为e714/1fd7/8e3a/4983，500次final为a14c。Function118次请求、零fallback。现有host受影响三项及Android原backend/pipeline/frontend路径集中通过；因本批改了交接ABI，同次复用正+9/负−7实际同地址的未对齐输入，双别名与强制W^X各2次恢复/2次回补，原空地址fault仍精确报告0x20119e。没有增加生产检查、测试文件或逐helper矩阵。

`profile-imm9`仅属于**前一镜像**：3332样本、0丢失，实际PID的sample periods约80.34%在JIT、17.58%在原生dispatcher；完整2000次进程计数为SVM/FEX约2.550倍instructions、2.395倍cycles。计数包含装载/JIT，采样有skid，且simpleperf stat的affinity warning保留在原始输出；这些用于热点判断，不授予正式PMU门限资格，也不能标成当前handoff镜像的PMU。

证据根`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-direct-emit-20261103`中的`device-handoff`保留命令、环境、输出、runner及74个构建输入快照，摘要`bbb5beb6d9503477d0341a68a4fe997b58df52cfd7bf687d7b828e77855772b2`；文档更新另存，不改写这份实测快照。旧检查点保持各自身份。

最终目标不变：compile抛开纯解码最多两倍FEX，生成质量不能靠缩减语义达标。当前总compile仅为侵入式诊断；普通镜像interval、A/B预算与更广功能覆盖仍开放。500次不足10秒，运行仍有2.21/2.84倍差距，不宣布CoreMark或最终生成质量达标。下一批先消除有真实热指令依据的AF临时常量装载，优先沿用当前flags ABI；完整ISA、动态ELF、guest信号和写驱动SMC等未完成范围不因这轮数字收缩。

## 22. 实际Value边、AF载体与写驱动SMC（2026-11-03）

本节记录`device-edge-af-smc`检查点，当前实现见第23节。第21节保留上一镜像身份；原仓库继续冻结。没有新增生产源文件、IR/pass、N级分析表或证明层，仍是唯一IR owner、三轮公共优化、Block/Function分开分配和直接编码。第4节已列实际执行层文件，不再把现有运行时工作挤回compile目录。

### 22.1 只改真正变化的Value边

FEX固定源`FEXCore/Source/Interface/IR/IREmitter.cpp`的ReplaceNodeArgument直接改实际参数并维护uses。本实现复用现有优化rows，不复制其常驻NumUses。

`ir/types.h`的既有Iterator向Builder开放实际word位置；`builder.h/.cpp`的ReplaceValueUse只核真实consumer范围/Value边、同owner、严格早于consumer且非Void的source、新旧有效读Type精确相同，然后改该8B word。opcode、自然类型、metadata和其余cell保持。归零结果替换成较窄自然类型producer时，consumer仍显式保留原宽view；原ForwardCopy的raw/normalizer等价性约束未放宽。

`compile/optimize.cpp`沿现有边遍历做一次ForwardCopy，写成功后直接更新旧/新use账。普通copy替换不再展开整条Operand草稿、重读旧uses、全schema校验并全量重编码。只有Fold和Uniform改变opcode时使用原Rewrite；首次读取post-copy草稿时同时取旧uses。结构Rewrite失败仍保留已完成的等价copy，实际Program和use账一致。三walk、8项Uniform cache及优化结果口径不变；没有新epoch、mutation清单或修复计划。

### 22.2 AF只在真正发布时打包

原Add/Sub imm12路径允许有AF输出：S-form覆盖目标前，只把`lhs XOR (literal & 16)`捕获到x0，后续消费者只读bit4。位移后的编码field不代替完整literal；带flags的负立即数仍不偷换ADD/SUB，8/16位及不适配立即数保持原路径。

仅NativeNZCV且Linked的ABI用x29.bit4保存raw AF。ArithmeticFlags的最终XOR直接写x29，省掉每次UBFX/BFI；逻辑清AF直接写零。x1继续拥有raw PF及保留位，packed模式和standalone模式不变。原PC访存改由state x19+pc_offset完成，实际128偏移仍是一条LDR/STR；大或未对齐offset保原地址构造能力。

backend的Pack、Shadow加载、RestoreFlags和ADC/SBB packed桥，以及dispatcher冷发布/冷返回和fatal fault恢复均接实际AF载体。热命中不打包AF；Root首次入口从真实packed状态初始化，不能读未初始化x29。NativeRunContext仍112B，目录slot64B、NativeCode40B、Home16B。frontend没有ARM64载体位布局；GetFlags/RestoreFlags的opaque合同保留，没有为本次验证向前端塞物理flags格式。

### 22.3 实际源页与安全退休

职责保持在执行层：

- `memory.*`只为实际编译的可写执行源页保留保护记录，先完成记录/分配再去W；正常RX程序不建立额外逐unit源表。写恢复只处理这个真实受保护页，不分配；解除保护依据当前映射权限，不能恢复过期mprotect/MAP_FIXED或已unmap页的旧状态。
- `runtime.cpp`把原pending行收为唯一实际DATA链接目录，ready-target链接也保留。未解析target仍用原链，pending计数含义不变；JSON存储量明确改名`link_relocation_rows/capacity`。这里是运行时实际重定位owner，不是编译器action表。
- `fault.*`仅在真实JIT fault site的SIGSEGV/SEGV_ACCERR上请求代码写恢复。成功后重试原store，保留ucontext/PSTATE/pins；运行时撤回全部实际DATA链接并停用native.slots，保留旧目录、fault范围和arena。下一次冷Resolve在已退出native unit后才Invalidate及释放。实际故障服务时间继续进入现有成本账。
- 对可写执行入口，两种scope均令所有控制边外部化，包括自回边。Function仍用Function IR和分配器，但明确约束为单basic-block region及真实出口stub；正常Function发现排除可写区域，不假称RWX也有完整Function覆盖。
- 内核read/readlink/getrandom及C++ GuestWrite写执行范围前先失效并解除保护，避免原来逻辑guest可写、宿主mtrack只读时的EFAULT或宿主memcpy fault。映射/权限变化继续沿现有失效边界处理。

覆盖边界是单guest线程对实际JIT源映射的直接写入，以及已实现的服务写入。当前块继续至下一控制出口后重解码；文件/shared别名和任意块内指令点重同步仍未完成，不授予完整SMC资格。未加入每入口memcmp、每store运行时检查或第二PC目录，RX热路径没有额外SMC分支。

### 22.4 同一批次结果

红魔CPU4仍614400–1171200kHz、cur1171200kHz，电池26.3°C，无锁频；同一官方O2 CoreMark ELF、固定FEX及显式TSO设置，候选实际O3/NDEBUG、LTO关闭。

| 当前同轮诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode的compile中位，3次fresh进程 | 9.917ms | 11.597ms | 6.733ms |
| 相对同轮FEX | 1.473× | 1.722× | 1× |
| 普通镜像500次guest计时 | 213.559ms | 161.037ms | 78.427ms |
| 相对同轮FEX运行耗时 | 2.723× | 2.053× | 1× |
| 出生IR nodes / operand words | 10246 / 16541 | 14908 / 22940 | — |
| native bytes | 65944 | 70712 | — |
| 合计8B spill槽 / 最大unit frame | 2 / 16B | 3 / 16B | — |

CRC匹配，Function118次请求无fallback；正常CoreMark两scope的SMC故障/受约束Function均为0。Fold/Uniform转发/删除数与上一镜像相同；并未靠删除已有优化降成本。Function优化阶段独立中位3.264→2.959ms，frontend扣byte约3.903ms、backend约3.967ms；各分项中位不可相加或当作完整互斥interval。A/B设计预算仍开放，不能因总compile诊断在两倍内宣布框架最终达标。

必要检查集中执行一次：host现有五项通过；Android原三项通过，仅在现有backend测试加一个两unit RawAF用例，实际验证Root/热hit、AF导出/恢复、ADC桥及Return保留位。沿用原正/负imm9未对齐和准确fatal PC输入，没有增加测试文件或逐helper矩阵，没有再采PMU。

一份1552B实际guest同时检查已缓存target改写、同块自修改回边、clock_gettime的C++输出和getrandom的内核输出：Block/Function双别名及Function W^X均exit0，真实各3次写故障、5次失效；Function各6次受约束请求、无Block fallback。原始guest摘要`f4edb6a12b701a7a626d4e8096ae822e0a7a047724c4ab2da71f18f7df3ba519`。

固定FEX运行这份功能输入exit1；一次仅区分失败分支的诊断确定clock_gettime返回-14/EFAULT（退出编码78），与其mtrack只读源页及直接syscall透传相符。不关闭FEX SMC换取通过，也不拿这份输入作性能比。此参考功能失败保留；后续只补跑原来尚未执行的CoreMark样本，计时后脚本缺policy变量的收口错误由独立close补齐，未删样本或重采已完成数据。

当前证据仍在`qor-direct-emit-20261103/device-edge-af-smc`：74个构建输入快照摘要`7b23da685e556bea314155416d84d1fd8739790f3c60d2ef589e76bfedc9e38b`、runner、环境、完整原输出和命令；文档更新另存。`profile-imm9`仍仅代表第21节之前的镜像，未改名为当前profile。

最终普通镜像compile interval与A/B分项、至少10秒的质量验收、更广应用/ISA、真正的helper/FP消费者、动态ELF、guest信号及剩余SMC生命周期仍需完成。目标保持≤2×去纯解码compile及不回退的生成质量；中途继续按成批实现和必要检查推进，不恢复旧plan/actions/certificates或扩大验证平台。

## 23. Known前端减少重复GPR出生（2026-11-03）

本节记录`device-gpr-birth`检查点，当前实现见第25节。框架及文件组织沿第4节，原树仍冻结；未增加生产文件、IR表示、pass或分配后修正层。

### 23.1 决策归真正的发射owner

`frontend/x86/build_ir.cpp`的Emitter持有原Builder唯一owner及16个NodeId、一个valid位图。成功BeginBlock后只清valid；没有每块重建表、跨块数据流或每node记录。现有helper直接收Emitter，IR Builder和O(1) Submit不持有这份前端状态。

ReadRegister仅在32/64位请求与缓存producer自然类型精确一致时复用Value。miss仍发真实LoadUniform。WriteRegister先完成原StoreUniform，再使父GPR失效；只有同自然类型、已归一化数字结果才重新播种。BitCast/GetFlags/GetResult及未知/外部/控制producer不按type猜数值；8/16位及AH写使父slot失效，混宽读保原出生路径，不额外制造转换链。LoadUniform32的narrow_pin只是后端物理借用，后续宽/raw消费仍通过原Read/RawValue归零合同。

所有现有GPR写口，包括栈、CALL/RET、MUL/DIV、POP/LEAVE、XCHG和MOVD/Q均经过WriteRegister；PC/FPR实际独立写口与GPR范围不重叠。缓存不延迟或省略架构写回。当前可恢复fault不修改guest GPR，故普通访存前不必丢弃同块值；未来真正会隐式修改GuestState的helper必须在实际调用边界清缓存，不能凭名称假设无副作用。

`ir/types.h`仅增加中立PrivateGPRState能力位，没有物理寄存器字段。`execution/runtime.cpp`由实际fault/private-state合同算一次private_guest_state，分别供前端能力、B的disjoint策略及后端private ABI消费。KnownTargetInfo默认不宣称此运行时性质；Unknown/EarlyOff的Has仍关闭，不执行Known规划。前后端仍通过语义能力和真实IR交接。

B继续处理剩余实际IR，8项Uniform cache和三walk不变。已经在前端省掉的LoadUniform不再交B展开、改写、计use和删除；没有把同一项决策变成新的inputs/preparation/actions往返。

### 23.2 真实成本及寿命结果

相同红魔CPU4、同guest及固定FEX；前后policy仍614400–1171200kHz、cur1171200kHz，电池26.3°C。3次fresh compile诊断与同轮500次普通镜像结果：

| 当前同轮诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode的compile中位 | 9.449ms | 10.793ms | 6.746ms |
| 相对同轮FEX | 1.401× | 1.600× | 1× |
| 普通镜像500次guest计时 | 211.631ms | 160.010ms | 78.849ms |
| 相对同轮FEX运行耗时 | 2.684× | 2.029× | 1× |
| 出生IR nodes / operand words | 8967 / 15262 | 13050 / 21082 | — |
| native bytes | 65808 | 70512 | — |
| 合计8B spill槽 / 最大unit frame | 2 / 16B | 3 / 16B | — |

相比第22节，Function出生14908→13050、B Uniform转发2083→425、删除1964→330；Block出生10246→8967、转发1425→319、删除1322→242。更少的删除和转发源于更少的冗余出生，原优化仍保留。Function公共优化独立中位2.959→2.372ms、Block2.414→1.829ms。frontend扣byte的Block/Function约2.750/3.844ms，未随出生数同比下降；不能把本批称作IR构建成本已经解决。

缓存同一SSA值到不同GPR，确有延寿风险：原寄存器被改写后，后续另一GPR的使用可能阻止旧publication借pin。此次两scope spill及frame保持2/3槽、16B，未为这个风险预先加入alias修正表、额外liveness或验证链。机器码量略降；短运行变化只作本轮观测，不授予最终QoR。

### 23.3 集中检查及剩余工作

本批只复用受影响的host frontend/pipeline两项及Android同两项。原有pipeline场景启用真实Known能力，在同一组场景补32→64、高/低位部分写及源GPR改写后的旧值观察；没有增加测试文件或扩大模式矩阵。真实flags字段别名场景仍保通用模式，不在没有disjoint合同的输入上强开缓存。Unknown/EarlyOff原分支继续执行。

原SMC实际guest仅复用一次Function双别名路径，仍3次代码写故障、5次失效、零fallback；其余未修改的fault/ABI路径复用第22节证据。正常CoreMark两scope CRC全部匹配，Function仍118次请求、零fallback。未重新采PMU或中途完整质量矩阵。

证据根同前，当前源码、runner及原始命令/输出在`device-gpr-birth`；74个构建输入摘要`a0615696c1496c16c783d7efd352d3fe148db232dd1848b415f56846da497c73`，文档另存。`profile-imm9`继续保留其历史镜像身份。

总compile诊断已降至1.40/1.60倍，但A/B设计预算及最终普通镜像interval尚未完成，500次仍不足10秒。运行质量、更广功能、helper/FP、动态ELF、guest信号和文件写别名SMC等范围不收缩。下一步针对仍高的真实构建/提交调用成本及已知文件映射的写别名生命周期推进；不把中途数值当作整体重构完成。

## 24. 追加结果ABI与已知文件写别名（2026-11-03）

本节记录`device-append-alias`检查点，当前实现见第25节。原树继续冻结，所有实现位于重建树；没有新增生产文件、IR/pass、N级表、验证平台或运行时实验开关。

### 24.1 结果只传一次身份

`ir/program.h`的AppendResult从24B收为16B：error、node及完整u64 value_unit。GetValue仅在unit非0时组成Value；Void成功仍保真实出生node但返回真正Value{}，失败error及invalid node/空unit、error-only bool和追加失败原子性不变。`builder_impl.h`仅改变成功结果构造；生产消费集中在frontend Require，其余迁移为既有测试/benchmark，未改NodeId.value等无关字段。

实际NDK O3对象已确认旧布局通过x8 sret写error、node、重复Value.node和unit；新布局由x0/x1返回，ReadRegister miss不再设置结果缓冲或从栈回读。`append-result-abi`与其`after`保留两份对象压缩副本、DWARF、代表汇编、命令及SHA。该变化确实删除返回边界工作，但本轮frontend延迟没有下降，不把ABI形式改善冒充构建阶段已经达标。

### 24.2 文件身份归唯一映射owner

`execution/memory.*`的MemoryRegion为56B，保VA/size/权限、实际backing种类、dev/inode、file offset及已知file size。匿名ELF填充保持匿名；普通file/memfd的shared/private身份从实际mmap/fstat取得。Protect只改各自子段权限，拆分保持对应file offset；合并还要求相同backing及连续offset，不能按权限合并掉来源。

CodeMutable、WriteAffectsCode与RefreshFileMappings仅读该唯一映射owner。RX源存在同文件重叠MAP_SHARED可写别名时，Function也采用真实单B和外部控制边；normal Function取窗排除这类可变区域。源页发布保护覆盖实际重叠的共享写别名，包括非EXEC的RW映射；MAP_PRIVATE写不作为别人的shared writer，尚未COW的private代码源仍保守观察shared写入。

原页记录、无分配故障恢复、DATA链接撤回、停用native.slots、保旧metadata至冷退出后退休的链路复用。没有第二PC目录、每入口memcmp或每store检查；这些映射判断和保护只发生于冷构建、映射修改或服务调用。

### 24.3 文件服务与已知取指边界

`runtime.cpp`在成功write/writev/pwrite64、ftruncate及带O_TRUNC的open/openat后，一次按当次fd身份刷新所有同文件映射的实际size；存在EXEC观察者时，在guest恢复之前失效。没有file映射时O(1)快退，普通匿名CoreMark的stdout不新增fstat。内核/C++写guest缓冲区仍在写之前解除保护和失效，两个时序不能混用。

CodeAt及Function取窗共同限制在已知文件最后一页，保留该页EOF之后的合法零填充，超出整页不交decoder。新EOF输入以125/编译拒绝结束；它不是guest SIGBUS交付，当前fallback诊断还可能显示pc=0，精确取指fault服务仍属后续范围。外部进程写入/截断、未登记host别名及设备backing特殊语义不在本批覆盖内，不据已知文件路径宣布全SMC完成。

### 24.4 一次集中结果

继续同一官方O2 guest、固定FEX、双方实际O3且LTO关闭；红魔CPU4前后614400–1171200kHz、cur1171200kHz，电池26.3°C。

| 当前同轮诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode的compile中位，3次fresh进程 | 9.598ms | 10.668ms | 6.838ms |
| 相对同轮FEX | 1.404× | 1.560× | 1× |
| 普通镜像500次guest计时 | 208.277ms | 158.894ms | 78.209ms |
| 相对同轮FEX运行耗时 | 2.663× | 2.032× | 1× |
| 出生IR nodes / operand words | 8967 / 15262 | 13050 / 21082 | — |
| native bytes | 65808 | 70512 | — |
| 合计8B spill槽 / 最大unit frame | 2 / 16B | 3 / 16B | — |

CoreMark CRC匹配，Function118次请求、零fallback；IR、机器码及spill工作量与第23节相同。frontend扣byte的独立中位Block/Function约2.769/4.094ms，公共优化约1.828/2.137ms、backend约2.739/3.743ms。各分项中位不相加；本轮结果不证明sret删除使frontend更快，A/B预算仍未通过最终资格。

host既有五项、Android既有IR/backend/frontend/pipeline四项集中通过，API迁移未增加测试case或文件。一份2824B实际guest覆盖shared RX/RW与private RX/COW视图、不同file offset、mprotect拆分/合并、前缀unmap、ready链接、pwrite/write/writev、O_TRUNC/ftruncate及RX自身经别名修改后的回跳。Block/Function均exit0，各3次native写故障、9次文件写失效、16次总失效；Function35次受约束请求、零fallback。仅补一个同guest截断至0后的取指输入，确认decoder前拒绝；没有展开新的质量/PMU/FEX矩阵。

该guest摘要`4d11ce342708b1ed9d947f9deff96a0510437f6e5d40a17a45ea89e53398b0e2`，当前源码及runner/原始输出在同根`device-append-alias`，74个构建输入摘要`8e79b10ce78b03fa11b4cc8bf8b0f16707cc73a070a81cfb5f8c6fa194102c5f`；文档另存。

### 24.5 编译热点决定下一项工作

ABI变化后前端延迟未降，因此针对这一镜像只采了一次compile热点：64个fresh普通Function进程，各20次CoreMark，不启用逐byte时钟；3115样本、0丢失，64个进程及CRC均成功。它包含启动、decoder、编译、guest及释放，是侵入式归因，不是去decode延迟或正式质量分数。当前主ELF筛选1573样本，self比例包括Optimize17.11%、Allocate10.49%及use lambda5.52%、UniformCache Add3.15%；这些百分比不能当作compile阶段时长或可省收益。

`profile-compile-append`保留数据、命令、镜像身份及离线地址分析，明确属于当前检查点；`profile-imm9`仍属早期镜像。下一步只沿真实热点删工作：同块普通use可在原分配walk内跳过必定返回原位置的loop_end，terminal及跨块查询保留；B的Fold/Uniform候选可在同一actual iterator遍历中完成copy与草稿采集，减少第二次解码。两者不增加表、pass或修正层，尚未在本检查点实施。

正式compile interval、A/B预算、至少10秒的最终生成质量及更广真实应用仍开放。原Android Bionic应用覆盖、helper/FP、动态ELF、guest信号及剩余SMC范围不因小syscall port的比值收缩；后续既推进实际热点，也补真实功能。

## 25. 原walk删重复工作与真实TLS寻址（2026-11-03）

本节记录`device-local-tls`检查点，当前实现见第26节。只改五个生产文件，不增加生产文件、pass、IR表示、N级分析表或防御性验证层。原树及Git/WIP继续冻结；执行仍消费同一紧凑Program。

### 25.1 两处工作留在原owner

`backend/arm64/allocate.cpp`的use处理沿原范围、类型及定义先于使用的准入判断；普通operand额外传实际block。同块定义到普通use满足d<u<block_end，中间没有RPO block header，原loop_end两次upper_bound相同，结果就是u，故直接更新last_use。terminal仍不传block，跨块仍查询原闭包：terminal的end坐标可能与后续空块/循环header重合，不能套同块快路。没有另建local-use表或修正pass。

`compile/optimize.cpp`第二轮用同一个actual iterator完成copy边修改和修改后cells/oldUses草稿，删除第二次ReadCells遍历。只对原Fold/Uniform候选采草稿；其他节点保持copy-only，非Value不解Payload/TypeView。超过16个logical cells仍完成全部copy，但不使用截断草稿做结构重写。三轮、原rows、优化集合、Rewrite后的use更新及原错误返回保持。

### 25.2 TLS是中立状态寻址

`frontend/x86/build_ir.h`的StateLayout尾部加入可选U64 FS/GS base offset，默认未配置；`execution/runtime.cpp`只把已有GuestState字段的offsetof接入。`frontend/x86/build_ir.cpp`在实际内存Address上用原LoadUniform+Add叠segment base，absolute、base/index、负disp和RIP形式共用；imm9借的是完整segmented base之后的位移，向量对齐检查看到完整linear address。

LEA、寄存器形式和隐式栈访问忽略segment；PUSH/POP的显式内存使用segment。未配置的真实FS/GS内存仍拒绝，原negative fixture无需改写。Known、EarlyOff及Unknown共用中立语义，没有引入物理ARM寄存器、flags布局或新opcode。segment寄存器指令、FSGSBASE指令及32位address-size不在本批补齐范围。

### 25.3 一次集中收口

host既有五项及Android既有backend项通过；未新增测试case或测试文件。一个1904B真实TLS输入在两scope均exit0、零fallback，合并覆盖FS/GS绝对/索引寻址、LEA及隐式栈忽略、显式PUSH/POP和非对齐GS base加disp后最终对齐的MOVAPS。

仍用同一官方O2 syscall-port guest及固定FEX，双方实际O3、LTO关闭；红魔CPU4前后min/max/cur为614400/1171200/1171200kHz、电池26.1°C。

| 同轮诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode的compile中位，3次fresh进程 | 9.485ms | 10.390ms | 7.564ms |
| 相对同轮FEX | 1.254× | 1.374× | 1× |
| 普通镜像500次guest计时 | 212.679ms | 158.976ms | 78.159ms |
| 相对同轮FEX运行耗时 | 2.721× | 2.034× | 1× |

两scope CRC匹配，Function118次请求、零fallback。出生IR/operand words仍为8967/15262和13050/21082，native bytes65808/70512，spill槽2/3、最大frame16B；这些是工作量相同，不是跨镜像机器码逐字节相同的证明。独立分项中位：frontend扣byte约2.925/4.258ms，B约1.735/2.258ms，backend约2.753/3.257ms。分项中位不相加，本轮FEX分母也较第24节更慢，不能把全部比值变化记为新代码收益。A/B预算及正式interval仍未完成，500次不授予最终QoR或有效CoreMark分数。

本批还尝试原1,921,016B Android/Bionic ELF，SHA为`d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba`。它仍在libc启动阶段exit125，最后拒绝点`__cxa_guard_acquire`的`0x269612: lock cmpxchg dword [rbx],r15d`，尚未运行CoreMark main。下一功能批是实际guard/allocator使用的标量原子同步家族；IR虽声明CAS/Exchange/FetchAdd/RMW效果，当前ARM64尚无对应消费者，不能只放开LOCK前缀就宣称支持。该输入与22,584B syscall port分别保留，不拼接覆盖或性能结论。

当前74个构建输入摘要`3a02599f589c292b879d2c2d9b2195ca437fdaf7723036b4a35dce68d202855e`，runner SHA为`893a46e23bb02ddb85e78a8e50d98d4428cdc363b6c3153f7537e7b25bb7cd78`。源码、命令、原始输出及压缩runner在同证据根`device-local-tls`；更新后文档另存。前一轮PMU仍属于`device-append-alias`，本批没有重复PMU矩阵。

## 26. 标量同步原语的前后端接通（2026-11-03）

本节记录`device-scalar-atomics`检查点，当前实现见第27节。六个生产文件及一个既有backend检查源发生修改，没有新增生产文件、pass、IR表示或N级表。Operation/Block/word仍为24/24/8B，home16B、FaultSite24B、Submit O(1)；原树及Git/WIP保持冻结。

### 26.1 唯一语义声明与发射owner

`ir/types.h`定义中立AtomicRMWOp（Add/Sub/And/Or/Xor/Neg/AddCarry/SubBorrow），`ir/opcodes.inc`明确CAS、Exchange、FetchAdd及RMW返回observed old和locked顺序。RMW的carry是显式0/1输入，内部主机条件码不充当IR数据。

`frontend/x86/build_ir.cpp`接入CMPXCHG、XADD的register/plain memory/LOCK形式，以及LOCK ADD/SUB/ADC/SBB/AND/OR/XOR/INC/DEC/NEG/NOT。LOCK前缀只判断许可家族和内存目的；实际发射函数沿原路径检查形状/宽度，不在前缀层重复做一套schema检查。

CAS32通过U64 Select保未写分支的高半，只在实际32位写入的分支清高；8/16继续原partial写口，AL/AH别名不另造完整parent表示。XADD先写source old、最后写destination sum。同一actual Address覆盖FS/GS；66同步指令的已知decoder宽度问题仅读现成bytes恢复局部operand宽度，不重decode或投影整个instruction流。

带LOCK的内存CAS用CompareAndSwap；未LOCK保LoadTSO→选择desired/old→始终StoreTSO，普通未对齐路径仍合法。可能故障的内存操作完成后才提交GPR及新flags；plain store之后重建实际算术flags producer，复用既有PendingFlags。INC/DEC保CF，NOT/XCHG不改flags，ADC/SBB从显式旧carry计算。

### 26.2 后端直接编码，运行时只选硬件能力

`backend/arm64/compile.h/.cpp`给CompileOptions增加尾部lse_atomics；`execution/runtime.cpp`在既有一次getauxval读取中检查HWCAP_ATOMICS，再传给真正消费者。前端不见物理ARM寄存器或LSE指令名；未启用LSE时，后端使用LL/SC分支。

四原语的8/16/32/64位共用一个发码分支。已知LSE用CASAL/SWPAL/LDADDAL及CLR/SET/EOR AL家族，Sub/Carry转换实际delta，Neg共用exclusive loop；无LSE用LDAXR/STLXR。内部谓词用EOR/CBNZ及非S算术，不破坏guest NZCV。observed old保在scratch16，完整内存访问结束才移动到真实home并存spill，故未完成的写不会先污染guest pin。generic memory沿原shadow前发布/成功后重载，private路径保live carriers。

CAS失配也保写访问：LL/SC分支写回observed old，未用只读后CLREX退出绕过写权限和SMC故障。该义务来自实际x86写周期，而非新增防御检查，见[Intel SDM Vol.2A CMPXCHG，3-148](https://www.intel.com/content/dam/support/us/en/documents/processors/pentium4/sb/25366621.pdf)。本批不额外插双DMB，不新建原子管理器、scratch约束表或证明层。

故障仍借唯一FaultSite表，原子不登记普通TSO两word救援；不能把原子降成普通LDR/STR。当前locked非对齐遇到原生fault会保精确guest PC并失败，完整非对齐原子恢复仍开放。FEX跨域宽CAS实现及其明确承认的tear限制记录在外部review，不作为可以安全照搬的证明。

### 26.3 合批检查与当前性能证据

沿用host五项集中检查；既有arm64检查内新增一个混宽原子顺序case，同一Program在实际可用LSE及默认LL/SC下检查observed old、保邻字节、显式carry和flags。Android原生两路通过；一个2488B真实guest同时覆盖条件32位高半、AL/AH别名、66 word、XADD/LOCK家族和plain未对齐CAS，SVM两scope及固定FEX均exit0。SVM Function整个输入一次请求、零fallback、零spill；该功能结果不授予原子全覆盖/完整VM资格。

集成时现有合同抓到LOCK NOT窄常量错误，已把全1字面量限制到真实宽度；随后只重建/检查受影响前端路径及真实输入，未重复已通过的native backend和FEX检查。初版镜像/源码/失败在`device-scalar-atomics-first`独立保存，修正版不混用它的性能样本。

仍用同一官方O2 syscall-port CoreMark及固定FEX、实际O3且LTO关闭。红魔CPU4前后614400/1171200/1171200kHz，电池26.2°C；三次fresh进程只扣实际byte decoder，Function形成、失败尝试、优化、分配、编码、发布/缓存及释放仍计费。

| 当前compile诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode中位 | 9.275ms | 9.970ms | 6.750ms |
| 相对同轮FEX | 1.374× | 1.477× | 1× |
| 出生IR nodes / operand words | 8967 / 15262 | 13050 / 21082 | — |
| native bytes / spill槽 / 最大frame | 65808 / 2 / 16B | 70512 / 3 / 16B | — |

CRC均匹配，Function118次请求、零fallback。frontend扣byte独立中位约2.985/3.994ms，B1.765/2.105ms，backend2.608/3.273ms，publication1.233/0.391ms；各分项中位不相加。工作量与前轮相同，本批没有重复500次运行质量或PMU矩阵，运行短计时仍仅属于第25节镜像；不靠计数相同宣称质量验收。

旧1,921,016B Bionic ELF仍单列：已发射约970条guest instruction、6次syscall，较第25节194条前进；当前停止在`__cpu_indicator_init`的`0x2699d1: cpuid`，其消费者是memset运行时分派，尚未进入CoreMark main。另有2次unsupported syscall，日志明确首个是x86 syscall269 faccessat；不把未逐项输出的计数都认定为同一个号。guest继续后才遇CPUID，服务缺口与最终拒绝不能混为同一终止原因。后续需真实guest CPU模型/能力查询分派合同和该raw三参服务，不直透传宿主能力或广告未实现AVX/OSXSAVE来走错误分支。

当前74输入摘要`fd54e3d70db72e1e0c4773644091f087b5c4a7ebdcb125b3aef864e3711eb87c`，runner SHA `593684eb4a88b052e16d1d9e8f22615bdb63a50765c6a23a27a46b185fca7bf4`；实际命令、源码及原始输出在`device-scalar-atomics`，文档另存。正式compile interval、A/B预算、最终有效运行质量与原应用覆盖仍未完成，不以这次诊断或启动进展缩减原目标。

## 27. Guest CPU查询与序列化边界（2026-11-03）

本节记录`device-cpu-query`检查点，当前已测实现见第34节。原树和Git/WIP继续冻结；中间框架沿同一Program、三轮B和独立Block/Function分配，不新增pass、N级表或helper管理层。

### 27.1 文件组织与唯一职责

| 文件 | 本批职责 |
| --- | --- |
| `execution/guest_cpu.h/.cpp` | 实际二字返回协议和不可变guest CPU模型；不读取ARM HWCAP，不接收guest-state指针 |
| `execution/runtime.cpp` | 将该callback接入真实前端请求；raw faccessat三参转真实host服务，返回真实errno |
| `frontend/x86/build_ir.h/.cpp` | 可选cpu_query地址；先读旧EAX/ECX，一次查询后拆出四个32位寄存器写回；保持flags并真实退出unit |
| `frontend/x86/decode.h/.cpp` | 实际CPUID记录提交后结束当前流，后继留给下一unit fetch；不重decode或新增supported-ISA census |
| `ir/opcodes.inc` | Cpuid形状收敛为leaf/subleaf/callback三参；CpuidUpper消费该producer的真实第二结果 |
| `backend/arm64/helper_calls.cpp` | 原integer_helpers.cpp改名；两类实际private entry共用448B bank保存/恢复，原八个DIV入口保持参数和结果语义 |
| `backend/arm64/compile.cpp` | 直接call一次、保存upper实际槽、返回lower真实home，完成序列化与flags恢复 |
| `backend/arm64/allocate.cpp` | 在原home.extra_slot为Cpuid分配8B第二结果存储，沿原FrameTooLarge边界，不建第二结果索引表 |

旧Cpuid声明中的未使用feature-mask输入已退役；新树此前没有消费者，无兼容转换层。callback的普通C++协议为`{u64 lower,u64 upper}(u64 leaf,u64 subleaf) noexcept`，两字分别打包EAX/EBX、ECX/EDX。第二结果随实际producer保存，跨另一查询后仍有效；没有全局latest-result缓存。

### 27.2 Guest模型与真正的提交边界

模型声明x86-64 baseline目标：基本vendor/max leaf、CMOV/SSE/SSE2、long mode/SYSCALL/NX、品牌与48位地址信息；未知查询返回定义好的零结果。AVX/AVX2、XSAVE/OSXSAVE、BMI、POPCNT和CX8/CX16等扩展保持关闭。Baseline SSE2的指令消费者仍在重建，query成功不代表整套baseline已完成；当前真实strlen拒绝的字节比较链正是后续必须补齐的消费者。

StateLayout未配置cpu_query时，实际CPUID明确拒绝；Known/EarlyOff/Unknown都沿中立query写口。默认模型不进入guarded XGETBV路径，XGETBV的非法使用仍未实现guest异常交付，没有伪造XSTATE可用结果。

CPUID在两种解码入口均为真实unit终点，不把顺序后继排成内部边。前端捕获旧EAX/ECX，flush现pending flags，一次Cpuid＋Upper后以既有view/LSR32写四个32位结果，无条件写nextPC并外部分派。手工decoded slice中的非末尾CPUID沿原局部消费检查拒绝，已出生前缀仍在实际诊断work内。这样后续取指继续受现有SMC撤链、native.slots停用及冷解析生命周期约束。

ARM64在call前发布现flags/pins并发DMB ISH，在call后ISB，随后把upper存入该producer的实际槽、把lower交真实home并恢复NZCV。private entry共用原保存bank文本，保护x1..15、实际向量home bank及返回地址；普通C++ABI保护x19..29（含raw AF）。没有每query发整套save指令，也不引入新的生命周期对象。

faccessat只读raw syscall269的dirfd/path/mode，用既有ReadString和真实`faccessat(...,0)`；无定义的r10不被当作flags。成功和ENOENT均由同一真实guest检查，没有假成功或额外路径验证层。

### 27.3 一次集中结果

host既有五项通过，Android既有backend项通过（1475 checks）。既有检查文件中新增一个pair lifetime case：同Program做两次不同查询，再读取第一upper，保跨调用live值及flags。一个1656B真实guest在两scope均exit0、零fallback，覆盖四个32位写回清高、vendor/leaf/扩展关闭、其他GPR/XMM和flags保持，以及faccessat真实成功/失败。该输入有8次查询、8个上半结果槽，最大unit frame16B；统计中的这些槽属于实际伴随结果存储，不能直接称为8次寄存器溢出。

继续同一官方O2 syscall-port CoreMark和固定FEX，双方实际O3、LTO关闭；红魔CPU4前后614400/1171200/1171200kHz，电池26.2°C。

| 当前compile诊断，3次fresh进程 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode中位 | 9.499ms | 10.490ms | 6.756ms |
| 相对同轮FEX | 1.406× | 1.553× | 1× |
| 出生IR nodes / operand words | 8967 / 15262 | 13050 / 21082 | — |
| native bytes / 总8B frame槽 / 最大unit frame | 65808 / 2 / 16B | 70512 / 3 / 16B | — |

CRC均匹配、Function118次请求且零fallback。独立分项中位：frontend扣byte约2.917/4.199ms、B1.738/2.088ms、backend2.629/3.381ms、publication1.263/0.517ms；不相加分项中位，不把这轮波动当成已归因收益或退化。本批没有重复运行质量/PMU矩阵，普通500次计时仍属第25节镜像；当前工作量相同不能替代最终质量证据。

原Bionic输入推进到约5339条已发射指令、9次syscall；当前停止在`strlen`的`0x2481b5: pcmpeqb xmm0,xmm1`，后续真实链包含PMOVMSKB及BSF。首个日志未知syscall为157 prctl，静态可定位调用者用于PR_SET_VMA_ANON_NAME；unsupported总计2未逐项输出，不全归为157。尚未证明进入main，也不把该失败片段compile与FEX完整应用比较。下一功能批应闭合SSE2字节比较→标量mask→位扫描的实际纵向链，并独立补该命名服务。

当前76个构建输入摘要`812b93edd8620e2f477b56a9adad20ca5e57812b53a7e5973d51013357b73b81`，runner SHA `3cdaf9a1aa27a893bdf85bd4406e4a201f0d9f5b3603822b807cf092c6d0ab54`；源码、镜像和原始命令/输出在`device-cpu-query`，文档另存。A/B预算、正式完整compile interval、有效最终运行质量、原应用覆盖及既有未完成范围继续开放。

该检查点下一compile候选是完整GPR中间发布出生。第28节先补实际allocator消费者，前端归并仍未实施，净SSA压力和时延收益仍未证明；不单独删前端Store后再堆补救层。

## 28. SSE2扫描链与延迟发布的实际消费者（2026-11-03）

本节记录`device-sse2-scan`检查点，当前已测实现见第34节。五个生产文件和一个既有检查源发生修改，没有新增生产文件、pass、MIR或N级表；24/24/8B IR、16B home和24B FaultSite保持。原仓库及Git/WIP继续冻结。

### 28.1 从真实字节到最终标量索引

`frontend/x86/build_ir.cpp`把PCMPEQ/PCMPGT B/W/D接入既有VecCmpEq/Gt，把PMOVMSKB、MOVMSKPS/PD接入一个VecMovMask，而非前端生成多组extract/shift节点。BSF/BSR16/32/64复用CTZ/CLZ；16位先做真实零扩展，BSR用CLZ xor31/63。ZF按原source是否为0产生，零source的undefined destination不额外保旧值；其它undefined flags不强制补存。原source的zero比较放在count之前，配合已有same-point消费规则缩短旧值寿命。

六种legacy compare的内存源复用完整EA的16B检查，符合[Intel SDM的legacy SSE异常规则](https://cdrdv2-public.intel.com/868137/325462-089-sdm-vol-1-2abcd-3abcd-4.pdf)。完整EA为已对齐RIP/绝对字面地址时直接省去检查出生；FS/GS base未知及寄存器地址仍保检查。寄存器来源不发检查，strlen的MOVDQU→register compare仍按真实范围读取。

`ir/opcodes.inc`明确VecMovMask的lane sign-bit语义；`backend/arm64/compile.cpp`按8/16/32/64位lane先USHR正规化，再逐级USRA聚合，用两次UMOV及W ORR合并，分别7/6/5/4条指令。任意输入均成立，不假设来自全0/FF比较；W结果清完整高半，所有步骤不改NZCV。源spill和目标spill沿既有实际读口及save_result，不增加修正层。

`execution/runtime.cpp`仅补实际PR_SET_VMA_ANON_NAME：确认guest实际映射、用既有ReadString取name，再调用host prctl并返回真实errno，支持null清名。没有第二份VMA命名目录、通用prctl平台或假成功。

### 28.2 绑定仍在原反向use walk内完成

`backend/arm64/allocate.cpp`保原相邻绑定优先；仅Known PrivateGPRState且实际private pins时，接受同B、完整槽、U32/U64同view且last_use==Store的非相邻候选。16个pending producer IDs和active mask是固定目录，候选home仍在原16B Home.reg，未增加N级记录。

Store处记真实Store写点，不乐观前移。实际交叠Uniform读写及MayFault/Unknown/External/Control、UniformBarrier、GetUniformAddress等观察点撤候选；到真实定义仍有效才确认并前移物理写点。更早borrow origins尚未消费，故无需回修。无候选时observer快退，只遍历active槽；相邻绑定覆盖同producer时分离旧pending，不清掉已确认home。四个CLZ/CTZ已按实际RBIT/CLZ读后写语义进入原same-point名单。

这批前端仍发每次GPR Store，未宣称减少出生N；下一步才是在已具消费者上联合归并完整GPR发布。未发布的中间值可能占SSA寄存器，整个应用净压力仍待实现后收口。

### 28.3 合批证据与剩余工作

host既有五项通过，Android既有backend1511 checks通过。既有文件加一组任意位模式的四种lane mask检查，并在原linked RawAF输入中合并可绑定/交叠读撤销/部分写撤销情形，保护真实pin、flags和hot edge。一个2080B实际guest合并比较/任意mask/BSF、BSR的ZF与窄写、未对齐MOVDQU扫描、VMA命名/清名/释放；SVM两scope及固定FEX均exit0，零fallback、零额外frame槽。未展开per-helper矩阵。

同一官方O2 syscall-port输入，双方实际O3、LTO关闭；红魔CPU4前后及运行补采均为614400/1171200/1171200kHz，电池26.2°C。

| 当前同轮诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode compile中位，3次fresh进程 | 9.755ms | 10.786ms | 6.852ms |
| 相对同轮FEX | 1.424× | 1.574× | 1× |
| 普通500次guest计时 | 211.382ms | 158.552ms | 78.397ms |
| 相对同轮FEX运行耗时 | 2.696× | 2.022× | 1× |
| 出生IR nodes / operand words | 8967 / 15262 | 13050 / 21082 | — |
| native bytes / 总8B frame槽 / 最大unit frame | 65576 / 2 / 16B | 70128 / 3 / 16B | — |

CRC匹配，Function118次请求且零fallback；生成量较前轮少232/384B，出生数与frame槽未变。frontend扣byte独立中位2.909/4.490ms，B1.752/2.066ms，backend2.706/3.295ms，publication1.429/0.512ms；不相加分项中位，也不把这轮时长波动当作已归因优化。500次不足有效CoreMark时长，质量和完整预算尚未验收。脚本在已完整保存Block运行结果后因漏导入re中止后处理；修复仅补剩下两scope和收尾，没有重跑已完成的compile或功能批。

原Bionic已发射约8190条guest指令、21次syscall，仍未进入main：最终拒绝`0x220788: popcnt rcx,rax`在StaticTlsLayout::finish_layout。首个未知145为sched_getscheduler查询；“pthread_create”错误文案来自主线程初始化使用的共享__init_thread，不证明benchmark/Scudo线程已创建。unknown总数3未逐项输出，不全部归145。后续应补POPCNT16/32/64及真实调度查询服务；该失败片段不与FEX完整应用作性能比较。

当前76输入摘要`8c0da5fde98fd3a5cd1dc412a908bacd63e8254a3c32f1648487b94076630d3f`，runner SHA `b3ad9c391a9b15c5c130a960afbbee54821c9cb0412349e07cf6706371a30faf`；实际源码、命令、原始结果及压缩镜像在`device-sse2-scan`，文档另存。A/B预算、正式完整compile interval、有效最终运行质量和原应用/其余未完成范围继续开放。

## 29. 完整GPR发布在出生前归并（2026-11-03）

本节记录`device-gpr-publication`检查点，当前已测实现见第34节。沿上一批已具备的分配消费者，前端现在真正省去未观察中间写的Store出生；没有增加pass、N级表、IR类型、生产文件或测试源。24/24/8B IR、16B append result和Home、24B FaultSite保持，原仓库及Git/WIP仍冻结。

### 29.1 固定目录和实际观察点

`frontend/x86/build_ir.cpp`的Emitter复用原16个NodeId/valid，加一个u16 dirty及实际StateLayout。Known PrivateGPRState下，同宽numeric U32/U64完整写只更换缓存并置dirty；被覆盖的Store没有进入Builder。最终U32仍是U64 Uniform + U32 Value view，保持清高语义。原raw/混view及Unknown/EarlyOff路径保留。

混宽/部分读miss和部分写先发布该槽；不能缓存的完整写直接覆盖并丢旧dirty。PrepareFaultPC、实际CPUID调用及每个B结束发布全部dirty；不跨Function内的B保缓存。当前多fault单指令都在最后一次faultable write成功后才更新GPR/栈，因此不需每Emit搭通用observer。Append失败仍令整个构建返回空Program，内部mask不加回滚层。

发布顺序由真实消费者约束：finish及Jcc在条件出生前发布，StorePC在最终PC Store前发布，EndBlock统一封口。因此既有terminal_only和PC-tail/静态链接融合仍可消费原形态。分配仍沿第28节同一逆序walk，实际last_use/观察点决定pin或SSA快照；缓存NodeId没有物理home语义。FEX参考是固定版本OpcodeDispatcher的RegCache实际Written/FlushRegisterCache行为，没有引入其外围层级。

其它文件只补实际消费者：

| 文件 | 本批工作 |
| --- | --- |
| `frontend/x86/build_ir.cpp` | GPR出生归并；POPCNT 16/32/64 register/memory，F3 mandatory和原66宽度恢复 |
| `execution/guest_cpu.cpp` | 完整POPCNT family已接后才声明leaf1.ECX.bit23；其它扩展能力不扩大 |
| `execution/runtime.cpp` | 143/145查询实际执行thread，guest sched_param显式写4B；显式decode_timing逐次输出未知调用 |
| `backend/arm64/compile.cpp` | 只修PopCount的v0 scratch说明；实际编码及分配未改 |

POPCNT沿原raw64 PopCount，窄源先按真实view ZeroExtend64。成功访存后写回，真实清CF/OF/SF/PF/AF，仅ZF以count==0延迟发布；内存源先保旧flags和FaultPC。没有为frontend改变raw IR的既有完整carrier语义。调度查询返回host实际policy/errno，正guest PID/TID只映射本执行线程；不盲写host struct或开放调度写服务。

### 29.2 集中检查与实际工作量

Host原五项通过，Android原12条base + 4170条integer/flags/Jcc路径通过。一个2200B真实guest合并POPCNT窄宽/脏高位/别名/零源/内存、CPUID能力及143/145真实查询，双方同输入exit0，SVM两scope无fallback/未知syscall。该guest用五个SETcc观察C/P/Z/S/O，AF沿既有ClearFlags消费者合同，本输入未直接观察AF。CPUID的一个8B伴随结果槽不称为寄存器溢出。

首fixture误用了未实现的SAHF/LAHF，在POPCNT之前被拒；首镜像、输入、命令和输出保留在`fixture-first`。修正仅换为现有SETcc捕获，生产镜像未变，已通过的host和native pipeline不重跑；没有扩ISA来满足fixture，也没有新增per-helper矩阵。

同一官方O2 CoreMark syscall-port输入、双方实际O3且LTO关闭，红魔CPU4前后614400/1171200/1171200kHz，26.2°C，无锁频：

| 当前诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode compile中位，3次fresh进程 | 9.477ms | 10.270ms | 6.848ms |
| 相对同轮FEX | 1.384× | 1.500× | 1× |
| 出生IR nodes / operand words | 8685 / 14698 | 12586 / 20154 | — |
| native bytes / 总8B frame槽 / 最大unit frame | 65184 / 8 / 16B | 69072 / 4 / 16B | — |
| 普通500次guest计时 | 212.108ms | 157.747ms | 78.502ms |
| 相对同轮FEX运行耗时 | 2.702× | 2.009× | 1× |

相对第28节，出生N实际少282/464、words少564/928、native少392/1056B；总frame槽从2/3增至8/4，最大unit frame未变。省出生并不保证被覆盖中间值无SSA压力，不以更少native字节声称最终生成质量无回退。Function仍118次请求/零fallback，20和500次CRC均匹配；500次不是有效>=10s CoreMark分数。

本轮frontend扣byte的逐sample差值中位2.976/4.159ms，B1.703/2.075ms，backend2.776/3.490ms，publication1.209/0.459ms；独立分项中位不相加，不把所有时长变化归因于本批规则。

原Bionic本轮约6097 emitted guest instructions、20次syscall，145调度错误已消失；当前拒绝`0x21d5c6: pmovsxbd xmm1,xmm1`在共用formatter，尚无完成main/CRC的证据。唯一未知131是sigaltstack，实际caller提交额外stack后不检查返回，不能直接认作当前formatter的触发原因。服务结果改变了执行路径，部分执行的条数不作线性进度或FEX完整性能比较。后续应闭合真实整数vector widening family及其精确2/4/8B内存读取，而非扩大到16B向量读或假成功跳过服务。

76个保存输入摘要`aee5f9cb947487e73d270d03c981303b4af0541090916d4a6fdbefea39119021`，runner SHA `ea0058ed7562c100e126840db4533d052d9cebfb92a1e85ccc2bcdfb5b1edc04`。源码、命令、首失败及最终结果在`device-gpr-publication`，文档另存。正式完整compile interval、A/B预算、最终质量、原应用及其余既有未完成范围仍开放，≤2× compile目标不变。

## 30. 向量扩宽与满bank输入回收（2026-11-03）

本节记录历史检查点`device-scalar-reuse`；先行功能检查点`device-vector-widen`保留其原源码和镜像。最新已测实现/测量见第34节。该批累计修改五个生产源文件，没有新增生产文件、pass、N级表或测试源。原树/Git/WIP继续冻结，紧凑IR、单一owner、三轮公共优化、独立Block/Function分配及16B Home不变。

### 30.1 一次描述扩宽，不先展开IR链

`ir/opcodes.inc`末尾追加中立VecExtend，保留已有opcode ID。输入是V128及(src_lane_bits,dst_lane_bits,is_signed)，结果完整V128，只消费最终128/dst个最低源lane。`frontend/x86/build_ir.cpp`将PMOVSX/ZX的B→W/D/Q、W→D/Q、D→Q十二种形态交到同一节点；没有先造多组CmpGt/Zip再由B识别。

寄存器源直接读取XMM；memory按真实几何只读取2/4/8B，再由既有VecDup64桥接，省去不参与结果的高位清理。原FS/GS、完整EA、TSO和FaultLocation继续消费实际输入。`backend/arm64/compile.cpp`经原VectorValue/结果保存口发1–3条SSHLL/USHLL #0，不建中间位置表或增加物理home。固定FEX的AVX128_ExtendVectorElements同样按原生扩宽逐级降低；SVM把这段固定语义留在一个源节点中。

同一个入口还修正了flags观察点：当前register-only vector既不读也不写EFLAGS，PendingFlags保到真实observer/覆盖；memory形式仍在fault前发布旧flags。两个scope的CoreMark出生N/words不变，native较第29节少56/72B；不把这一小项宣称为大幅运行收益。

### 30.2 实际消费者与分配次序

| 文件 | 当前实际工作 |
| --- | --- |
| `ir/opcodes.inc` | 单个VecExtend语义声明，保原稳定ID |
| `frontend/x86/build_ir.cpp` | 十二种扩宽、PMULLW/D和MOVAPD/UPD/XORPD位操作；真实flags/fault/对齐观察点 |
| `backend/arm64/compile.cpp` | VecExtend直接1–3条原生编码；其余新增front形态消费已有编码 |
| `backend/arm64/allocate.cpp` | 原读后写谓词由pin与满scalar bank复用，共用真实Home/last_use |
| `execution/runtime.cpp` | 将实际TLS需要的MAP_NORESERVE交给原mmap/owned mapping路径 |

固定FEX RA先保存输入寄存器、释放killed源，再分配输出。SVM只在原scalar bank搜索无空槽、U32/U64结果且属于既有read-before-write集合时，读取实际首Value输入；由其Home.reg O(1)定位槽，核last_use==本定义及resident精确身份后直接复用。原通用`<`释放、loop/terminal闭包、vector、borrow及pending发布都保持；没有kill表、修正遍历或禁用flags的条件。Add/Sub/Neg的旧输入AF信息已在写结果前捕获，复用不丢AF/PF/NZCV合同。

当前CoreMark的总frame槽仍8/4，与先行检查点一致，故不能声称此快路已消除第29节的spill增长。低压力纯链本来不额外发MOV；这项仅提供满bank时消费真实最后读取的机会。没有把它扩成对任意opcode/任意端点相等值的整体`<=`规则。

核心30个TEXT函数、3592条静态指令的只读调查只发现四处未接PMULLD整型SIMD需求，连同同族PMULLW消费既有VecMul。MOVAPD/UPD/XORPD是完整128位位操作，不冒充已具scalar-double环境。Legacy完整m128 computation/shuffle保其[16B对齐规则](https://cdrdv2-public.intel.com/821612/248966-Optimization-Reference-Manual-V1-050.pdf)，新增PMULLD也遵[Intel SSE4定义](https://www.intel.com/content/dam/develop/external/us/en/documents/d9156103-705230.pdf)；已知对齐literal仍由原VRead在出生前省掉检查。窄扩宽memory不套16B读取/对齐。

### 30.3 真实失败驱动的两批收口

先行检查点host五项、2816B合并输入两scope/FEX和两scope一次CoreMark20 CRC均通过，没有做QoR扫描。原Bionic终于输出实际fatal：TLS mmap(addr0,len45056,prot0,flags0x4022,fd-1,off0)被case9的0x1833白名单拒掉0x4000，尚未进入GuestMemory/host mmap。原Map本来允许任意正length并按host页取整，不能把45056解释成对齐失败。主线程共享初始化函数打印pthread_create文字，仍不是已创建benchmark线程的证据。

最终批次以具名common flags替代原magic mask并纳入已证实MAP_NORESERVE；host errno与实际owned元数据路径继续负责结果，不盲放其它映射策略。与分配/核心SIMD改动一起集中检查：host五项、手机既有backend1511 checks、12+4170原生pipeline路径通过。一个3424B合并guest覆盖扩宽、精确页尾窄读、低位乘法、位操作及实际memory服务，SVM两scope/FEX均exit0、零fallback/额外frame槽。没有新增per-helper测试源或PMU矩阵。

同轮官方O2 syscall-port输入，双方实际O3、LTO关闭；红魔CPU4前后614400/1171200/1171200kHz、电池26.2°C，无锁频：

| 当前诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode compile中位，3次fresh进程 | 9.839ms | 10.426ms | 6.824ms |
| 相对同轮FEX | 1.442× | 1.528× | 1× |
| 出生IR nodes / operand words | 8685 / 14698 | 12586 / 20154 | — |
| native bytes / 总8B frame槽 / 最大unit frame | 65128 / 8 / 16B | 69000 / 4 / 16B | — |
| 普通500次guest计时 | 211.384ms | 157.596ms | 78.500ms |
| 相对同轮FEX运行耗时 | 2.693× | 2.008× | 1× |

20/500 CRC均匹配，Function118次请求且零fallback。frontend扣byte逐sample差值中位2.980/4.036ms，B1.706/2.022ms，backend2.871/3.491ms，publication1.499/0.486ms；独立分项中位不相加，不把时长变化全部归因于新规则。当前native量/槽数与先行vector检查点相同，不能据此声称镜像逐字节相同。500次不是有效>=10s分数，formal预算和最终质量仍开放。

### 30.4 下一实际边界

原1,921,016B Bionic的TLS mmap fatal已消失；当前约9982 emitted instructions、28 syscalls，最终在共享memcpy/memmove的`0x247d40: prefetchT0 [r8+rsi+0x80]`拒绝。131及两次14仍返回ENOSYS，分别为sigaltstack与rt_sigprocmask；不能拿日志顺序当该memcpy的唯一caller。该函数有多个启动/allocator调用者，当前仍无main完成/CRC证据，不将这段失败compile与FEX完整应用对比。

下一批应一并闭合四种PREFETCH缓存提示及该memcpy长路径的MOVNTDQ、SFENCE，分别保非faulting hint、真实存储/对齐及屏障语义。核心main的时间换算/评分还需要scalar-double完整消费者，必须有真实MXCSR与host FPCR/FPSR owner、NaN/无效转换及异常状态合同；不能因MOVAPD/XORPD已接就宣布FP环境完成。

该检查点76输入摘要`915b0ff55593fe02d29e985d2745110c2e4fd8c847aac9fd0645ae47cb35f6c9`，runner SHA `dc21c74523d6381813fb9f38c4b20a695f7318e8010250a6bea4a01d48e944e0`。原源码、命令、两检查点镜像与结果分别在`device-vector-widen`/`device-scalar-reuse`，文档另存；原应用覆盖、A/B与正式完整compile预算、有效最终质量及其余既有未完成范围继续开放。

## 31. 真实字符串消费者与私有向量出生缓存（2026-11-03）

本节记录已测检查点`device-vector-cache`；最新已测进度见第34节。先行功能批`device-memcpy-stream`保留独立原始证据。两批累计修改五个生产文件，没有新增生产文件、pass、N级表或测试源。原仓库/Git/WIP仍冻结。后续未测源码不能继承本节镜像的性能结果。

### 31.1 文件组织与实际语义

| 文件 | 工作 |
| --- | --- |
| `ir/opcodes.inc` | 末尾追加中立MemoryStoreFence，保稳定ID；Ordered效果由原公共优化保留 |
| `ir/types.h` | 独立PrivateVectorState执行属性；Known后端信息本身不承诺state私有 |
| `frontend/x86/build_ir.cpp` | PREFETCH四种非faulting hint、MOVNTDQ、SFENCE、MOVLPD/HPD、PSUBB、PALIGNR、PSLLDQ/PSRLDQ；固定16槽向量出生缓存和真实观察点发布 |
| `backend/arm64/compile.cpp` | store-only fence直接DMB ST；其余消费者复用已有向量、内存和EXT编码 |
| `execution/runtime.cpp` | 只在实际private_guest_state合同成立时选择私有GPR/向量属性 |

PREFETCH仅在真实memory形态接受，既不求EA也不造load/fault或清pending flags。MOVNTDQ保真实EA/FS/GS、16B对齐、fault PC，发普通V128弱store，SFENCE独立DMB ST，不为每次NT store加全屏障。固定FEX的非SVE路径同样用STR Q，Fence_Store用DMB ST。

MOVLPD/HPD只读写实际8B并保另半槽。PALIGNR按`[SRC低128,旧DST高128]`选字节，memory即使结果全零仍做真实读取/对齐；寄存器无用源不出生。字节左右移0不改槽，1..15复用既有零/EXT，>=16清零。它们不要求新机器位置或新分配分析。

向量缓存保存16个NodeId及valid/dirty mask，完整VRead/VWrite复用/覆盖；不保物理寄存器信息。原fault、helper、block结束、Jcc及tail PC观察点发布。半槽memory读取前先发布，半槽写入后清其缓存；BeginBlock不跨块缓存。Unknown/EarlyOff走原路径，不先执行一遍Known规划。

### 31.2 集中证据与实际收益

先行批host五项和1400B合并guest两scope/FEX通过；当前批host五项、手机既有12+4170 pipeline路径及2816B合并guest两scope/FEX通过，零fallback。没有逐helper新测试源或重复跑无变化的backend矩阵。

同一官方O2 syscall-port ELF，同轮fresh进程三次，红魔CPU4无锁频：

| 当前诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode compile中位 | 9.785ms | 10.230ms | 6.811ms |
| 相对同轮FEX | 1.437× | 1.502× | 1× |
| 出生nodes / operand words | 8583 / 14566 | 12475 / 20013 | — |
| native bytes / 总8B frame槽 / 最大unit frame | 64968 / 8 / 16B | 68872 / 4 / 16B | — |
| 普通500次guest计时 | 206.140ms | 157.778ms | 78.711ms |
| 相对同轮FEX运行耗时 | 2.619× | 2.005× | 1× |

20/500 CRC匹配，Function118请求零fallback。frontend逐sample扣byte中位3.083/4.071ms，B1.672/2.008ms，backend2.732/3.455ms，publication1.366/0.490ms，release0.102/0.084ms；独立分项中位不相加。频率前后614400/1171200/1171200kHz，电池26.2°C。探针有侵入性，500次不足10秒，不能授予完整预算/最终质量资格。

缓存仅少102/111个出生节点、160/128B native；原公共优化本来就能转发V128。它提前已有优化，尚未解释Function仍约2×的运行差距。总frame槽不变，不能宣称spill已解决。下一质量行动是用现有外部PMU/RX工具各采当前普通Function和固定FEX一次，按实际热点决定修改，避免继续凭静态流量猜主因。

当前76输入摘要`a3875f6af2a58212d300c7130376fe6888aac1e9f4f0a2c9671d5793048cef64`，runner SHA `707a1a451a479bd35db56dc25ace5f13e5ac616223d3a60dae0294ffb5387e6c`。源码、镜像、命令、原始结果在`device-vector-cache`；先行批保其历史身份。

### 31.3 后续实际边界与FP范围修正

原Bionic通过前述memcpy/strncmp拒口，现于Scudo的`0x22e3c8: TZCNT rcx,rax`停止；约13668 emitted guest instructions、44 syscalls，仍无main/CRC完成证据。131/14/14/13分别保留真实ENOSYS，不伪造信号服务。不将其部分compile与FEX完整应用对比。

TZCNT可消费既有CTZ及BitScan宽度/故障路径，实现真正16/32/64语义：zero返回宽度，CF来自源零、ZF来自结果零。NewR的CPUID保守广告不是指令白名单，固定FEX亦将F3 handler与BMI1广告分开；支持该子集不要求广告整个BMI1。旧树SVM_BMI关闭时走BSF是独立旧gate，不自动成为新树合同。

第30节“异常状态合同”需限于冻结实现既有范围：原树不自动累积MXCSR sticky或交付IEEE trap，不能将补全它们扩成本轮硬前置条件。后续scalar-double消费者仍需真实raw U32 MXCSR往返、RC/DAZ/FTZ与host FPCR/FPSR隔离、正确lane/NaN/无效转换结果；手写sticky位不能丢。不得先引入每FP指令sticky扫描、unmasked trap平台或独立证明层。具体方案见同批保存的`scalar-double-consumer-review.md`。

原应用覆盖、A/B与正式完整compile interval预算、有效最终质量及其余既有未完成范围继续开放，≤2× compile目标不变。

## 32. 基于实际采样收敛条件消费者（2026-11-03）

当前已测检查点`device-conditional-simd`；先行`device-tzcnt`只做功能批检查。相对第31节只改两个生产源文件和一个既有构造断言，没有新增生产文件、pass、N级表或测试源。后续CRC32C/affinity实现尚未构建或测量，不继承本节数字。原仓库/Git/WIP继续冻结。

### 32.1 一次当前镜像采样，不扩验证平台

`profile-vector-cache`绑定第31节707a…镜像，ordinary Function/FEX各30000次，只用cpu-cycles:u，不开compile探针。双方38587/19341样本、零lost；按实际guest PID的period加权，SVM JIT85.801%、独立dispatcher14.057%，FEX独立dispatcher未采到，不排除其JIT内inline分派。

SVM真实向量state访存仅0.0623%、SP访存0.0313%；不支持继续扩大向量缓存。x1/x29 carrier和PSTATE形态合21.446%，包含必须的AF/PF维护，不能称全部可删。dispatcher主要样本归load→equality链，MRS/MSR仅0.530%整进程；没有证据把14%全算作碰撞或flags打包。完整样本、RX和连续指令片段在该独立目录。

### 32.2 两处消费者直接省工作

| 文件 | 本批工作 |
| --- | --- |
| `frontend/x86/build_ir.cpp` | 真TZCNT16/32/64；packed imm8 W/D/Q逻辑移位、B/W/D/Q加减、完整imm8 SHUFPS；普通CMOV直接CondSelect |
| `backend/arm64/compile.cpp` | 两源32-bit shuffle通用四条INS；NativeNZCV数字零判断用CLZ及bit6 |
| `source/tests/rebuild/x86_construction_test.cpp` | 既有carry条件断言识别CS/CC，保真实条件映射检查，不新增执行矩阵 |

TZCNT source零返回宽度、CF来自源零、ZF来自计数零，16位用bit16 sentinel保证CTZ32返回16。BMI1广告不变。逻辑packed移位明确只接imm8，>=lane宽为零；没有冒充已接变量count形式。SHUFPS 0x88使用VecUnzip32，其余imm8仍完整选源/选lane；C从仍live的两个输入向distinct结果或v31写四lane，保spill与alias。Legacy m128仍真实读取并对齐。

共享DirectCondition只做一次中立条件映射；十二种CMOV在出生时直接发已有CondSelect，省掉CondSet→Select。PF/NP及guest C|Z组合仍走原数值路径，不能把ARM canonical HI/LS直接误作x86 JA/JBE。memory source继续无条件读，32位目的在两路径都清高32。Unknown/EarlyOff也消费同一中立语义，不接物理ARM条件规划。

TestZero/NotZero读取原规范化numeric carrier，用CLZ Xd,Xn后提bit6，NotZero再EOR 1。它不改NZCV/x1/x29，不需要Pack/Restore或新的活性分析。实际访存前的pending flags发布仍保留；没有仅因Jcc读ZF就跨memory删PF/AF，也没有为此新增fault证明平台。

### 32.3 合批结果与剩余差距

host既有五项最终通过；其中carry断言原先绑定旧TestFlags和AL占位，修正既有条件预期后只重跑该项。手机既有12+4170 native pipeline路径、CoreMark20两scope均过。没有逐helper增加测试源或重跑无变化的backend矩阵。

同一官方O2 syscall-port ELF，同轮CPU4，实际O3/LTO关闭、无锁频，前后614400/1171200/1171200kHz、电池26.3°C：

| 当前诊断 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 去纯byte decode compile中位，3次fresh进程 | 9.586ms | 10.300ms | 6.897ms |
| 相对同轮FEX | 1.390× | 1.493× | 1× |
| 出生nodes / operand words | 8511 / 14494 | 12356 / 19894 | — |
| native bytes / 总8B frame槽 / 最大unit frame | 62168 / 7 / 16B | 64040 / 4 / 16B | — |
| 普通100000次guest计时，一次 | 37.765019s | 27.294980s | 14.806729s |
| 相对同轮FEX运行耗时 | 2.551× | 1.843× | 1× |

长运行三者均超过10s并输出Correct operation validated，三项核心CRC匹配，final均0xd340。它比此前短跑具有有效时长，仍只是一次诊断，不能授予最终质量验收；不同迭代数不能直接当严格前后A/B收益。20次迭代诊断中的Function118次compile请求零fallback，native比第31节少4832B；Block少2800B，总frame槽只少1，不声称spill问题已全面解决。

frontend逐sample扣byte中位2.978/4.250ms，B1.766/1.981ms，backend2.733/3.324ms，publication1.259/0.475ms，release0.104/0.083ms；独立分项中位不相加。当前探针仍有侵入性，A/B及完整compile interval正式预算未授通过。

镜像对应76保存输入摘要`a3cf78de595588ba6bc5aeca2483e62dd316f2ad0d840333e4aea606d80c9a3a`，runner SHA `5d03123e860c9f7955f307e04bc5ff6a50fd0241f38d0dee30b5a414415e74e3`。保存的构建输入含原构造断言；其后仅测试预期的修订另存closing-source-overlay，不冒充重新生成过的镜像。命令、失败/修订检查日志、源码、压缩镜像及原始结果均在`device-conditional-simd`。

原Bionic已过Scudo移位/加减/shuffle前沿，现停`0x222c9b: CRC32 rax,rbx`，仍无main/CRC完成证据。真实syscall204 affinity缺口与CRC指令拒绝分开处理，不伪造成功。后续质量优先审实际warm边的PC发布和目录工作，须先厘清cold/helper/fault消费者，再决定是否延后发布；不因采样比例盲改hash或另建缓存。原应用、A/B与正式完整compile预算、最终质量及其余既有未完成范围继续开放。

## 33. MXCSR与标量浮点的实际闭环（2026-11-03）

已测检查点`device-scalar-fp`保留独立源/镜像。补CRC32C完整数据宽度与真实syscall204、MOVSD、signed32/64→double、DIVSD、double→signed32/64截断及UCOMISD/COMISD；先沿实际main/time_in_secs反汇编定位需求，没有扩成完整FP平台。

GuestState在416增加raw U32 MXCSR，默认1f80，size432；旧字段offset保持。NativeRunContext尾112/120/128/136承载host FPCR、host FPSR、guest FPCR与mode，size144。root/cold/helper/exit/fatal边界管理真正浮点环境；LDMXCSR成功写raw后Return3，重新进入前更新模式，不把warm Dispatch误当模式重装。

VecFCmp只输出中立CF/PF/ZF bits，Publish只有该Value；移除了两处旧FlagM2物理Imm。纯数值转换/比较保guest flags，CVTT用raw exponent校正NaN/overflow sentinel，不读旧FPSR.IOC。当前无AFP，绑定唯一raw控制字段，用非S位操作独立完成DAZ/FTZ，不拒绝合法组合、不加软件FPU平台。AFP与generic环境路径保留；手写status原样往返，不新引自动sticky/trap。控制语义可参见[Intel说明](https://www.intel.com/content/www/us/en/docs/fortran-compiler/developer-guide-reference/2024-2/ftz-qftz.html)。

host5、既有1511后端和12+4170 native pipeline路径、单5152B合并输入两scope均通过。一个外部薄wrapper在host DN/FZ/RC及FPSR非默认值下包围同一输入，normal/fatal返回都精确恢复；auxv为hwcap=ffbfffff、hwcap2=26181、CRC1/AFP0。没有新生产开关或逐helper矩阵。

Tiny支持输入的扣byte compile为9.677/10.036/FEX6.749ms；100000次运行36.928/27.153/FEX14.890s，仅保其历史身份。原Bionic在这一检查点停于__dtoa，不能用其失败前缀与完整FEX授予性能通过；当前最终验收使用下面的完整原应用。

## 34. 本轮最终端点与质量验收（2026-11-03）

当前已测/冻结镜像`device-formatter`，source-as-built摘要`0aaf165c7df7c9c099b499ed520ed719629b3140e6703951b1f054cd94a6e8a3`；runner SHA `3e32e04a97ad3635e2a288aa874fd3ed344c5db4443a5127141bacf98fe9bc1e`。测量后只更新文档，另存overlay，不冒充重编译。

最后一个功能批仅改三个生产文件：front将实际__dtoa消费者映为已有中立位操作、scalar add/sub/mul、packed kind1/5与八个CMPPD谓词；C直接消费实际homes，packed转换保两输入lane和sentinel、高64清零，比较支持完整关系集合；runtime走真实非PI futex WAIT/WAKE/bitset/requeue/WAKE_OP，不伪造同步成功。原kind表保持1=dq2pd、5=ttpd2dq，未误重定义4/6。无新生产文件、pass、N级表或FP bank。

| 实际源文件 | 本轮最终职责 |
| --- | --- |
| `ir/types.h`、`opcodes.inc`、`opcode.h` | 紧凑身份/类型/中立语义与schema；不声明物理后端规划 |
| `ir/program.h/.cpp` | 唯一3vector存储和不可变借用视图；24/24/8B |
| `ir/builder.h`、`builder_impl.h`、`builder.cpp` | 局部受检追加/原位编辑；16B append result，O(1) Submit |
| `frontend/x86/decode.h/.cpp` | 一个受界decoded buffer、Function形成及真实终止边界 |
| `frontend/x86/build_ir.h/.cpp` | layout契约与实际x86→中立IR出生、固定状态缓存/flags观察点；Unknown保generic |
| `compile/optimize.h/.cpp` | 同owner三walk、单u64/N和固定8槽uniform cache |
| `backend/arm64/allocate.h/.cpp` | 单16B Home；独立Block轻路及Function RPO/loop闭包 |
| `backend/arm64/compile.h/.cpp` | 实际机器ABI/能力绑定、选码与同Home直接编码；无MIR/actions/certificates |
| `backend/arm64/helper_calls.cpp` | 真实私有helper参数/返回与home bank保存ABI |
| `execution/guest_state.h`、`guest_cpu.h/.cpp` | 唯一guest状态布局和不可变CPU查询模型 |
| `execution/memory.h/.cpp` | ELF/guest映射、文件身份/已知EOF/写别名owner |
| `execution/native_code.h/.cpp` | slab/W^X发布与真正code/fault元数据生命周期 |
| `execution/dispatch.h/.cpp` | 64B槽与144B context ABI，root/native/cold/exit交接 |
| `execution/fault.h/.cpp` | 真实PC/flags恢复、对齐回补与已知代码写恢复 |
| `execution/runtime.h/.cpp` | 编译请求、唯一PC目录、syscall、模式更新与整体执行owner |
| `launcher/linux/main.cpp` | CLI与粗粒度结果；不开常驻逐命中计数 |

### 34.1 完整原应用，不能用Tiny替代

原1,921,016B Android Bionic/CoreMark ELF的SHA仍为`d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba`。两scope及固定FEX均正常输出/exit0；20次三项核心CRC e714/1fd7/8e3a及final4983匹配。再用相同guest/参数100000次集中检查，三者均输出Correct operation validated，final d340匹配并超过10s。

红魔NX729J bf1359ff，CPU4，前后min/max/cur 614400/1171200/1171200kHz、电池26.4°C，无锁频/频率写入。双方实际O3/LTO关闭；FEX有显式FPO，SVM没有该额外选项，精确命令/身份保留。普通FEX `a156752f…` 固定TSO及无AOT/object cache配置保持；编译探针只围实际DecodeInstructionImpl，不减其形成。

| 最终结果 | Block | Requested Function | FEX |
| --- | ---: | ---: | ---: |
| 完整冷compile去纯byte，中位3次fresh | 37.163321ms | 67.079815ms | 47.231302ms |
| 相对FEX compile | 0.786837× | 1.420241× | 1× |
| 普通镜像一次全compile连byte也收费的补充记录 | 40.315ms | 72.138ms | — |
| 100000次guest计时，PMU stat无采样 | 36.943s | 29.236s | 15.093s |
| 相对FEX运行耗时 | 2.447691× | 1.937057× | 1× |
| 全child user指令数相对FEX | 2.754399× | 2.075197× | 1× |
| 全child user周期相对FEX | 2.441414× | 1.934627× | 1× |

三次编译交错轮换scope/FEX顺序。最终长跑把taskset置于实际child前，并逐一核实际exe PID的Cpus_allowed_list=4；前一份仅继承亲和性且有工具告警的PMU结果另存，不混作最终来源。PMU统计包含cold JIT与child setup，无逐指令时钟/采样。运行≤2×不是用户新增硬门；当前时间/指令数均与FEX同数量级，Block仍有明显优化空间，不声称已追平。

### 34.2 实际成本与fallback，不把分项份额换成新目标

20次diag的Block为2041次Block尝试；Function为722 Function+24局部Block retry，总746，不能宣称零fallback。总compile计时包住失败和retry，未将它们或释放隐藏。Block/Function出生N为37146/88901、words66056/153967、native262432/467948B、累计8B frame槽72/62、最大unit frame96/112B。这些是当前完整应用，不能与先前Tiny总数拼成回退结论。

独立分项中位：frontend逐sample扣byte11.540/28.309ms，B6.648/13.465ms，C11.547/21.091ms，publication4.365/2.767ms，release0.331/0.334ms；各自中位不相加。Function A/B占比可继续收敛，但总端点已分别≤2×。第9节的份额是设计归因，旧组件强门和synthetic不能替代或无限阻挡当前真实总端点结论。

### 34.3 结构与收口边界

源码核对确认唯一IR owner和O(1) Submit、B单表三walk、两scope实际分配分支、同Home直编码、前端无ARM64 include、CMake不链接旧中间层。没有补回旧plan/actions/certificates。最后host5和单6728B扩展合并输入两scope通过；环境owner沿第33节的normal/fatal恢复证据，未重复无变化的后端矩阵。

原仓库基准2404文件逐hash保持，原101删除路径未复活，HEAD与index SHA保持冻结；代码在新树，未提交/推送到冻结原树。源、命令、原始输出、CPU mask、PMU计数、最终判定与清理回执均在证据目录。

本轮收口范围仍为第1节的静态单线程路径及实际中层重构。全ISA、动态ELF、完整信号/线程平台、外部文件writer/未注册别名不借此次宣称实现。当前Bionic的六次131/14/13信号设置仍真实ENOSYS，程序自行处理并完成；Function局部fallback保留为明确限制。不为闭合本目标另加FP sticky/trap、通用验证平台或逐helper质量矩阵。
