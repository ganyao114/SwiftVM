# IR 表示、用途关系与最终操作数交付

**当前状态（2026-11-02）：8cda已合A，最少检查仍有一个新增fixture失败；02测试修正未应用，当前版本没有新的ordinary计时。** 最新普通方向仍属e0d6，最终两倍目标未达。完整source、构建、原例、归因及ordinary资格统一见[主方案7.1](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)，本文末段只列本轮身份边界。

本轮按[净工作与路线修正](spill-and-compile-root-cause-plan-2026-10-28.md#compile-net-work-correction)及[审查42](compile-gap-review-2026-10-31.md#performance-route-correction-latest)修订。同Store是当前选用的实现，正确性合同、真实旧工作退出和完整反馈决定是否采用，不能由载体、文件数或少表推出性能结论。

本文区分当前事实、数据合同和待实现算法；实施顺序见[主方案](spill-and-compile-root-cause-plan-2026-10-28.md)，逐源职责见[结构表](middle-pipeline-file-layout-2026-10-28.md)。

**同Store是当前选用的表示与交接实现。** 支持域在最后semantic reader后结束Inst，由原RA/state/事务/proof/resource形成PhysicalReady；public/retained/Unknown及消费前source miss保合法语义分支。它可退旧Describe/Complete和每emit binder，但并非所有减工算法必须采用的唯一表示。保留稀疏选择或当前frame不必然保留完整旧服务，判断依据是实际读取/求解/转换退出。第78批无收益中间表仍撤回，不以表示名称重启。

下一主线减少构造和幸存节点的重复解释，让目标约束/分配/最终绑定交付同一权威决定；不再先安排packed IR、origin-only格式或另一张物理图。已退役的HIRValue/HIRUse、全量Actions捕获、native packed适配不重复列收益。

第76批before为f7e6/d391，紧凑移位78f已完成必要语义和完整方向，无大幅收益后撤回全部新增opcode/profile/wire及shift.h。当前实际位置包before为a10，既有参数/用途/重发合同继续；最新资格以[主方案2.1](spill-and-compile-root-cause-plan-2026-10-28.md#21-当前源码开发计时和最终资格分别记录)为准。

**同Store的一份程序：Semantic → BackendVirtual → PhysicalReady，actual输入和最终资源已接原支持域默认。** 完整目标见[主方案5.3.11](spill-and-compile-root-cause-plan-2026-10-28.md#5311-下一完整替代同一store中的后端虚拟与物理程序)；实际native块已观察，完整负载覆盖率尚未建立。原Selection不扩为全N/E程序；已合支持域依照[真实机器形态合同](spill-and-compile-root-cause-plan-2026-10-28.md#5312-下一相依组先闭合机器合同再替换默认链)，贯通原质量分配、state、独立check和编码资源读者；仅转换格式或开关不提供性能资格。

## 1. 当前权威数据及已退役对象

下表按实际phase/分支给权威；支持域默认同槽接管，public/retained/Unknown和消费前source miss保语义分支。现码路由不等于normal覆盖已观察，CFG/terminal与machine live见2.2及4.2。

| 数据 | 当前权威 | 重构后的结束/交接 |
| --- | --- | --- |
| opcode、typed 输入、结果、flags | Semantic时为Inst/Arg；支持域转换后为同槽NativeInst/NativeCell | 全部semantic SSA/图匹配结束后接管；source miss/retained分支保IR至Flush，不能双持另一份程序 |
| 每次使用的 type-view、完整 Imm/EA、Params/Lambda | 真实 operand payload | 绑定到该 consumer 的物理角色；资源真实释放或唯一移交 |
| 身份与语义顺序 | Store/domain/session/NodeId32，order | 身份和最终 RA 顺序分开；物理 token 不能取回 Inst |
| counts、反向 uses、implicit/terminal roots | 原唯一用途owner与真实roots；转换后用actual程序 | 支持域在semantic最后reader后关闭；RA读actual uses/defs，raw资格另保精确occurrences，不借旧Users修假end |
| CFG/entry、持久 parent | 原 Block/Function/HIR owner | RA 范围不变；持久范围/发布身份保留，语义 terminal 不再访问 |
| deep facts、SSA/live/state/flags | 当前epoch/同revision Facts | semantic SSA/必要图事实转换前完成；原CFG与真实数字范围数据按读者转交，machine live从actual operands维护/重建，不混用旧ends |
| SSA位置与运输 | Allocation/Writer的Map/REF/region/preservation | 当前跨重发保活；目标在virtual阶段由唯一Writer修改，最终物理兑现/核验后无真实读者即可结束，不移出timer |
| actual selected/cold uses | 原Allocation实际use行及原selected/LowView/preserved优先级 | 最后物理化兑现真实role和必要logical MEM事件；不建独立inferred/原occurrence镜像。第78分组payload/placement目录已撤回 |
| 实际reload payload | 原Allocation与Writer注册/查询/回滚合同 | 完整u32、creation ID/flags、实际Map/dirty和binder/Jit/replay寿命保留；第78唯一region存储实现已撤回，不作新IR表示前置 |
| 架构副本、版本与恢复 | 原state owner | 与SSA位置不同；真实事件现场组合当前anchor，保catalog/entry/observer/fault与真实输出，原最后读者后结束 |
| 最终编码输入 | 支持域同Store PhysicalReady和真实资源；保留语义入口的IR/Allocation/Actions及旧physical replay | 支持域kernel/重发只读稳定physical operand，无每emit semantic binder；source miss/public/retained保真实适配，normal覆盖不由接口存在推定 |

HIRValue/HIRUse及其pool已退出。第74批公共Inst为 **24B origin header＋五个16B直接语义cell＝104B**；加16B prefix并按16B对齐，managed Slot为128B。只初始化实际cell；offset、公共packet/overflow和local NodeId读时解码已删除。local tag仍保护用途投影到held Store prefix，raw/foreign/Params/Lambda保原受检合同。真实physical记录仍为48B，BodyForConsumption只借该前缀，不能把两种布局混用；实际backing增加仍须最终资源验收。

第79批另提供112B BackendBody，容纳32B header＋五个16B NativeCell，**仍为同一个128B managed槽**，不把104B Inst或旧48B packed记录改称native布局。Consuming的committed prefix计数归原stream，cursor负责semantic尾部/转交；转为Virtual/Ready后，复用Store原alive位标记真正存在的backend对象，拒绝墓碑/外来同id token后才作typed读取。没有第二phase位图；Store不解释ARM64析构，实际native资源由原stream结束。

独立Block与Function no-Params birth共用BirthUseCounts，原C++参数保活到提交，不再生成字段image。权限来自实际CreateInstruction后的unique draft/IRStore；source检查、完整multiplicity预检、真实graph reserve与直接字段/计数提交同事务。已激活用途图仍做原准入/ScanInputs；Params、public fresh/raw/slow保checked。无每node资格位、第二图或延迟计数journal。

## 2. 稳定身份、实际参数与顺序

### 2.1 不因压缩改变语义

NodeId 保完整 32 位，不用 allocator 的 u16 编号代替它，也不按 ID 大小推断程序距离。原单元容量/sentinel 检查仍在；同 attempt 已删除身份不复用。跨 Store/session 的相同 ID 不代表相同定义。

逻辑`value.id`、机器`birth`、consumer point与host offset分别有自己的意义。pair secondary保自身身份，但实际定义/binding/fixed写点在producer；联合定义枚举不保证逻辑ID单调。C3将真实Begin接到初始fixed、质量读者与Writer的历史限定检查保原身份；后续最终核验/form/state已在有限支持域接通，当前资格见主方案7.1。LinearScan、fixed writer窗口、home-index及transfer保各自原端点，不把birth迁移当作改写全部区间规则。

Value 的 type-view 属于具体使用；SameDefinition 不等于同一输入语义。完整 64-bit Imm、signed/type、EA 位移、base/index、scale/extend、重复 occurrence 和 Params/Lambda traits 保留。不能截断地址、参数 count/capacity、raw slot bits 或符号信息来压小槽。

语义 opcode/schema、签名、effect 和 occurrence 来自同一 `ir.inc/ir_meta`。目标 physical form 描述实际编码/角色，不在其中重写另一份 guest ISA 语义。

### 2.2 物理记录的目标形态

目标为原managed槽中的NativeInst：同一operand字段在BackendVirtual表示确切value/view/role，在最终兑现后表示真实GPR/FPR/MEM/zero/immediate。它是已规范化target macro-op，不要求一条对应一条A64。**不将当前完整PhysicalValue/CurrentInstruction每节点复制常驻，不把旧48B packed wire当新程序必经格式。**

| 内容 | virtual阶段 | physical阶段 |
| --- | --- | --- |
| 身份/顺序 | 完整NodeId32、原Store/session及真实程序点；多结果身份与定义点可不同 | 同一身份及原order；final RA编号与host offset仍独立 |
| form/operand | 一次选择的shape、raw view、Whole/Low32/Definition/incoming等实际role、完整Imm64 | 成功form及精确source/destination、class/home/view/extension；不靠producer再推 |
| 实际use/def | 真实machine读点/最早birth、implicit fixed/NZCV/helper等合同 | 相同合同的实际位置和最终effects；W/X/partial不从语义type猜 |
| MEM/transport | 必要raw occurrence、原logical retirement、backing/forward与reload义务 | 实际token/count/region、multiplicity、先读后clobber和last-use；不简化为一个home |
| 复杂参数 | 完整Params/Lambda/composite EA/secondary及traits；可复用资源唯一转交，异构Params按当前指令转换到唯一native backing | 真实长度/Datum、选中角色和extended backing；旧Params由cursor析构，保转换与暂时重叠成本，不称零拷贝 |
| 控制/恢复 | 原CFG/terminal/entry/call-return/hoist意义移入原B/unit元数据，值先virtual | 值已物理化、fault guest地址/size和observer保留；host offsets每次emit生成 |

普通form尽量inline，真实大Params/EA/tuple/MEM扩展使用唯一必要backing。当前已构建C4让Current只保两个借用指针、Params按需读native cells；旧variant/Params留原builder冷State，普通native emit不创建它。主role由实际form决定，只有真正需要多种读取的operand才申请稀疏例外role；读口不retag、不查询Allocation。Incoming仍是transport语义，不扩成新的GPRView。resource handle只在本body物理提交成功后接管已结束的virtual-only字段，读口按phase分开。

存储phase/output-kind粗粒度区分NativeInst与旧PhysicalInst；不能仅因同叫PhysicalReady就错误析构或decode。112B布局只表达可承载内容；可表示secondary不证明producer birth/联合输出已正确。实际输入须履约其机器合同，128B槽本身不授全族准入或性能。

**当前physical身份按表示分别承担，不能把字段名当成所有分支的保证。** `NativeInst::ReadPhysical`对普通GPR/FPR从cell重建final value id、canonical物化id、class/slot、width/extension、role/view与consumer；不会填`PhysicalValue::producer`。MEM返回原完整descriptor，才承诺其producer token和transport字段。原Store NodeId/domain身份在body/header与原需要它的读者中保持，不等于final RA id，也不为普通寄存器另存一份token。

最终读取的测试应核原Store body地址/NodeId、对应实际definition，以及consumer读与Allocation的final id、canonical、class/slot/type/view一致；MEM再核完整producer身份。`read.producer.node==原NodeId`不能无条件套在寄存器上。读口不回查已结束的semantic对象，也不因这个fixture错误增加N级origin目录。[实际读口](../source/runtime/backend/arm64/compile/native.cpp)、[字段合同](../source/runtime/backend/arm64/compile/physical.h)。

### 2.3 身份、版本与失效分别归真实 owner

| 边界 | 实际负责什么 | 不能拿来证明什么 |
| --- | --- | --- |
| Store/domain/session＋NodeId | 实际定义身份、退役和复用范围 | 同数值ID、同地址或相同RPO不代表同一定义 |
| uses/EditBinding与实际写口 | payload/counts、已激活反向关系、改写通知 | uses ready不代表未来Facts或物理Map已验证 |
| ValueWorklist epoch | 固定成员roster、顺序/CSE、需求事实与dirty闭合 | 新birth/hoist不能自动加入旧epoch |
| 同sealed Block/HIRFunction的SemanticFacts | 最终输入的SSA/live/索引与按需缓存 | seal不证明实际物理Map为空，也不授予消费权 |
| Allocation input request / attempt | 不可变inputs的归属、reserve/FPR/eviction重试 | 一次完成不代表下次attempt的位置或软选择有效 |
| location编辑边界/epoch | Map/REF/guest-copy变化、受影响form/role失效 | 早期候选或fixed检查不能替代最终actual writes合同 |
| Prepared/Store粗phase | normal保SemanticBorrow至Flush；第79批组件已支持Semantic→Consuming→BackendVirtual→PhysicalReady→Retired，phase归原Store/stream，无逐node ready表 | 发码成功不等于已退役或完整后端check已完成；memory lease不代表无语义借用 |

这些是现有不同身份与失效边界，不新增统一Version类、逐节点资格表或证书。TakeIR结束Facts的借用；physical origin token与持久metadata不能回桥到已结束的语义服务。

### 2.4 Store目录的真实容量边界

每个Store页仍含256槽；第74批每槽128B（原修复时为64B）。页目录与按地址排序目录各一项对应一页。当前修复使两目录仅在size==capacity时增长；原来每次新page均无条件reserve(capacity*2)，即使有余量也翻倍，导致容量对页数指数膨胀。原02ae和Word候选均在同一65535指令fixture抛bad_alloc；当前原求解器配合容量门后，原规模/断言通过。

两次reserve仍在任何目录insert/push前完成，任一失败不发表新page/slot，下一node不递增，新page由局部unique_ptr释放。该目录修复当时没有改变身份、页大小、payload、粗phase或lease/retained边界；第74批另行替换公共payload并扩大slot，原目录增长算法继续沿用。没有新缓存或延期free。目录增长修复不等同于整个IR表示提速；da有组合普通基线，尚无可孤立归因此修复的收益。

## 3. 一份用途 owner 与受控变更协议

### 3.1 counts、反向边和 implicit roots 不混同

operand 是 producer/type-view 的权威，uses 是反向关系。普通/pseudo 计数、counted terminal 与不计数 implicit roots 保原语义；If/Switch 的真实 Value 用途继续计数，hoist/fault/prefix 根不能强塞 num_use，否则单用/DCE 合同改变。

用途登记次序、优化调度顺序和最终程序顺序是不同合同。first/last consumer 显式按真实位置判断，不为保留没有语义消费者的旧尾序再建 journal。

### 3.2 所有实际改写走同一写口

append/insert、SetArg/replace、erase、detach/move、terminal/root 变化经现 Inst/Block/Store 维护 payload/counts、已 ready uses 和实际失效。它不等于向已创建的 ValueWorklist 自动登记新成员：现 BuildRoster/Find/BeforeErase 要求固定 epoch roster；需要新增节点时在受支持边界结束并重开 epoch，不能在旧 roster 中 erase 新 birth，也不为此增加逐节点资格表。可失败的容量准备在提交前完成；不为小改写增加整图快照或通用事务。

受控构造阶段维护真实 payload/counts/owner，首次真实 Users 需求或编号时批接入同一反向服务；接入后增量维护。公开/manual eager 与受检 raw 路线保持。准备失败不发布部分 ready，const 查询不暗中分配。

旧 occurrence 在改写后失效，即使 consumer/槽位仍存在也不能当作原边。DCE 的单节点删除继续解除用途/root；最后整组消费时关闭图是另一协议，不能模拟 N 次 DCE。

### 3.3 Block 与 Function 独立处理

| 范围 | 保留策略 |
| --- | --- |
| 独立 Block | 出生 counts；必要时建立唯一反向服务；无需图的块不预付 Function 图或 CFG 固定点 |
| Function，含单块 Function | 原跨块 uses、CFG/SSA/live/entry/loop 和 Function RA；受控 decode 批接入 |
| raw/foreign/mixed/组外语义共享 | 明确 membership/owner 与完整受检 fallback，不假装 fresh/owned |

raw edge、alias-resolved聚合、consumer分组和CFG live end可以是不同事实；同一事实在同revision不养两份。允许现有owner生产一个替代重复发现的必要关系索引，明确完整域、真实token/point/view/multiplicity、编辑/reset/冷恢复失效及最后读者；独立checker仍查真实机器结果，不用索引命中或Passed自证。必须计旧行退出、索引初始化/访问/capacity及结束，lazy不是只改ready位仍清全表。

**同点read-before-write按完整scope核，不改变全局区间约定。** 只有真实旧write owner的聚合LiveEnd恰等于新实际birth P、新producer完整actual-use确实读取该owner、按原schema允许先读后写，且无terminal旧值读取时，才可交接最后一个端点；P前所有fixed/inherited硬段照查。把旧lease截到本B末尾不能证明Function无后继用途，shared write也不自动是whole alias。最终proof保id+identity和独立占用检查；Block/完整Function分别建立正确fixture，不能手写Map或删same-home前提换绿。[完整合同](spill-and-compile-root-cause-plan-2026-10-28.md#native-handoff-obligations)。

### 3.4 residual 因果闭环需要哪些通知

M1的主范围是构造和公共解释；独立Block首次seed后的DCE因果队列、Function flags dirty闭包重解是条件性配套，不能包办213/262ms成本线。它们共用现RewriteContext/ValueWorklist/FlagFlow，不改变IR格式，不新建用途图。

Block队列必须接全真实写口：旧输入释放、opcode/参数改变、单节点及bulk erase、Append/Insert/adopt/remove、新生ClearFlags、terminal和implicit root改变。独立Value::UnUse等直接降计数写口必须通知，或者使该域走明确的受检全扫。stored条目用原handle/session/alive/owner；raw条目保实际membership。已有固定ValueWorklist roster不会因此自动接纳新birth；DCE出生通知和value epoch是不同职责。

Function flags缓存只属于同一可变闭环，在原RewriteContext中保活B级transfer/live-in，不能每次调用重建。删除读者可使环路需求下降，受影响前驱闭包必须从bottom重求最小固定点；不能只保旧live_in继续OR。采用flags自身的terminal/label/observer关系，不以value-flow图替代。

局部清理集合为相关语义/uses/root/顺序修改的dirty块与live_out变化块的并集；transfer相等只能停止无关前驱传播，不能跳过dirty块自身。真实use计数或implicit-root变化要通知producer/被引用定义所属块，包括跨块consumer删除。首次分析、hoist新epoch、未知/malformed的受检路径和资源结束仍计费。

### 3.5 BackendVirtual只保一份机器存活期

语义Users在最后读者结束后关闭；机器阶段的`ProgramLiveness`由原RA范围持有，借同一program/CFG，不再建第二反向用途图。initial描述、完整质量读者与未改变合同的原attempt共借一次准备结果；C7数值组已有范围限定构建/检查，完整normal及剩余支持域尚未闭合。AllocationState拥有inputs，借用program/live须延续至原重试与Finish结束。

`UseEnd`含全scope/CFG传播，`LocalUseEnd`只取定义块的实际最大点；`UseCount`保完整机器occurrence、不按point去重，raw alias与wholeREF root各计一次，同身份不重复；`UsesStayInBlock`表示真实跨块。logical retirement/no-body alias不是machine use，MEM原multiplicity也不能由这些数字取代。Block不预付Function局部/跨块数组，Function新增N状态与初始化照计。

form/operand/cohort改变use/def、根或birth时，调用者在受影响owner重开前显式维护或重新Prepare；`Matches(program)`只核借用域，不能证明版本没变。不另立revision manager。scratch flags在semantic关闭前稀疏转交whole-unit与Function-local两个已知域，缺失不能当零；完整协议与C3剩余工作见[主方案5.3.12](spill-and-compile-root-cause-plan-2026-10-28.md#5312-下一相依组先闭合机器合同再替换默认链)。

### 3.6 原始用途、canonical类型与每次读取分别保真

C3以NativeCell的Original位保存原raw occurrence，ReplaceCell自动退休旧原始用途到唯一稀疏LogicalUse行；机器新输入不伪装成Original，whole-alias原arg在原cell借一次，不再手工补同一LogicalUse。canonical定义类型来自真实header；per-use view保原cast，包括VOID。V128定义经U64 view读取仍是同一V128定义的局部读取，不能将该view写回定义类型；也不能把VOID补成U64来修测试。

C7数值组已修正相应实际读口：先核Store/cookie/id并取真实header的canonical类型，再核每次读取的RawView及有效cell类型。`ProgramRawUse.value.type`是canonical类型；重建读取cell时按`view==VOID ? value.type : view`取有效类型，原VOID raw_cast仍保留。不能把U8 view当定义本身的U32类型，也不能在确认有值及合法opcode前对VOID查询字节宽度。

原GetUses(false)总数、GetUses数值数、actual machine uses和原raw退休用途分别服务旧质量政策、机器live及ownership。C4保NumericUseCount原总数意义，新增optional NumericOnlyUseCount仅为确需原数值数的规则；pseudo的真实差异稀疏保留，缺失不是0/1，不能用当前machine count伪造。必要raw身份与总数/数值计数沿原header/中立program转交，当前质量行从actual Descriptor直接生产，不保semantic拓扑预计划；machine UseCount保actual numeric/implicit/terminal全occurrence；raw计数不包含后造机器隐式输入。ProgramResolve只沿合法whole-alias，核下一真实definition的宽度/寄存器类别；Map/REF、Low32或宽度共home不自动授权whole-alias。

原MEM transport仍须把原逻辑末次用途与actual lifetime共同闭合；换源后原count为0但机器仍活的新source不能提前free。当前raw自动退休、logical-only非MEM及MEM descriptor接线不证明最终pending/Tick/Freeze/kernel/重发已正确。窄/跨类BitCast当前在semantic关闭前拒绝并选原合法路径，完整能力迁移仍属最终方案，不靠缩域完成目标。

program回调或inputs非法不能变成“没有用途/没有region/资源不够”。原准入先核静态span；实际use/definition在索引前核点/块/birth/身份，首次发现非法即终止当前attempt，不走reserve或local repair。沿用现错误域，不为此复制N/E程序或增加manager。本组修正见[5.3.12.5](spill-and-compile-root-cause-plan-2026-10-28.md#53125-当前必修项修边界不另造证明层)。

## 4. 生命周期与最后消费者

### 4.1 从请求入口给消费权

只有 runtime 明确移交的 **fresh、execution-only、完整 Store/actual-parent 编译组**可消费。现 CompileUnit 保活环境和 lease，并借用调用栈 ExecutionAccess 登记实际消费权限；parent/block write guards 由 ExecutionAccess 持有。该权限已接入普通 Block/Function；不把编译计数 guard 当作消费权限，也不新增生命周期 owner。

Block 真实 write guard 在优化/Prepare 前进入并连续移交；Function 完整 parent/block 组去重后按固定顺序取锁，合并实际 owner 仍由同组覆盖。不能读锁升级，也不能凭 shared_ptr::use_count、sealed、TakeIR 或最后一个本地 Prepared 判断独占。锁按正确线程/范围归还，idle cache 不留锁。

公开 retained Prepare、组外活 semantic borrower/region、mixed Store、manual/import、其它后端保非消费路线。完整 fresh 组内部共持 parent、多个 Prepared 或 immutable 结果不因此失去消费权。内存 lease 只能证明 backing 活着，不能证明组外语义借用消失。

### 4.2 粗 phase 与同一转换 visit

本节规定同Store native合同；载体、CFG/terminal移交、phase/abort、actual输入和完整原支持域已经默认接管，保留语义分支见4.5。组件NativeReady不替代本输入实际所需的state/fixed/width/flags/transport独立检查，也不要求所有族迁完才反馈一个合法纵向组。无消费权的public/retained保合法checked入口，不强行覆盖。

```text
Semantic：generic编辑/hoist/编号、完整SSA Finish、最后target图匹配
  → Consuming：全部semantic借用结束；原cursor控制同槽资源转交
  → BackendVirtual：原Store唯一native程序；两scope machine live/RA/候选事务
  → PhysicalReady：必要state/CFG与独立检查通过，同operand最终物理化
  → Retired：最后重发/发布借用结束，真实资源归还/有界复用
```

不能边SSA首访边销毁Inst：`SSAValidation::Finish`还读取nonlocal definition及分支对象。转换前迁完semantic读者，包括ValueWorklist/flags/DCE/hoist、最终Facts/SSA/future-use matcher、late gpr/fpr/width/spill源链、terminal/implicit/dynamic callee、Function entry/edge/call-return和runtime extent读取。完整授权组的预检、全部最后读者和元数据移交都闭合后，才转换第一个Store，不能按“某块处理完”提前覆盖。`CloseSemanticForConsume`目前会关uses、清terminal/hoist并detach order，调用前必须把真实CFG/terminal/锚点和原order移交，不能关闭后继续借旧接口。

转换不是再匹配一遍：保原NodeId与顺序、转交可复用资源及真实稀疏recipe、结束Inst后同槽构造NativeInst。第79批Params须在当前Inst仍活时构造NativeCell backing，cursor析构旧Params后再move准备结果；这是真实O(k)转换及暂时资源重叠，不是原vector backing零拷贝。normal不能先完整Describe+Complete再复制输出；目标选择/约束生产本身须取代旧服务。native开始后，common RA与late消费者只读canonical operand/effects及原CFG，不能保存Inst* roster或延寿旧semantic图。

稳定的machine use/def/根/birth及资源合同在首machine live前确定；真正依赖Map的变更必须在受影响interval/owner开放前事务维护，或受界回滚/重建。只有不改变这些合同的等价编码细节可晚定。virtual cells在attempt间不变，实际form少量编辑按原journal回滚；不能第一次RA便把源覆盖为physical、失败再从raw IR恢复。最终独立checker检查opcode/role/effects与真实Allocation，不拿producer选中位自证。

“birth前”还必须满足区间/排程依赖：静态form先确定，物理候选为当前attempt给明确待履约机器合同，live按它准备；改变use/def/根/birth须在任何受影响interval/owner开放前原子更新live、fixed/tie/clobber、queue/pressure和依赖state。若已无法局部维护，回滚到最早受影响owner之前或结束attempt，选完整generic/另一有限合同后重建真实机器范围。现RA先CollectLiveIntervals再assignment，不能因尚未emit就只改Map/recipe继续。回退顺序与尝试上界明确，不枚举跨cohort组合、不沿旧semantic ends；失败成本计完整服务。

pair的logical结果身份与machine def点必须分开：Div128Remainder/CpuidUpper自身无body，在producer点与primary联合定义，birth所属块、唯一def枚举/查询及输出resolver point须一致；其逻辑closeout仍保留。GetResult/GetFlags在自身点编码，分别按真实numeric/implicit state读取建模，不照搬旧Function对全部pseudo的保守提前。首版保原同块/唯一/严格后继等准入；规范化先于ProgramLiveness与pressure准备，core拒绝已提交的未迁语义政策及缺失/区间非法的scratch表；尚未提交的需求与真实scratch预算仍由目标粗准入负责。C2只验证首批schema/GPR压力，不证明完整pair scratch/ABI/alias；同点非REF输出还须独立最终检查，旧逻辑ID roster或排除birth的窗口不足。CoreAllocated和initial Complete都不能推定最终齐全。详见主方案5.3.12。

### 4.3 部分提交失败与重放

容量/可表示性/entry和复杂参数资源先准备。semantic→native转换若部分失败，原cursor先Abort semantic尾部，原PhysicalStream::Reset再按committed_count调用NativeInst::DestroyAt析构native前缀；图关闭后的尾部不得UnUse已转换定义，旧Inst析构不能解释native字节。只以原粗phase/output-kind分派，不另建逐节点phase图；同址reinterpret_cast不是合法对象生命周期。

正常RA/cohort/FPR/reserve失败仍留在BackendVirtual，回滚原位置、稀疏form、受影响machine live与attempt结果后按有界政策重试，不读已死IR。实际机器位置在最终可失败边界后兑现；如要声明FreezeNativeOperands无抛出，必须已备齐容量并完成所有会失败的检查，否则保明确cursor/prefix abort。C4的稳定NativeOriginOf从原Store与header/id/type/count/normalize取得RawToken和canonical类型，不依赖virtual Definition/Result.Reference；prefix物理化后，后续raw/资源查询必须走这个读口，不能读已覆盖的虚拟字段，也不加N origin目录。全局容量/preflight完成后可随本body资源完成提交handle并Freeze，仍有原prefix abort；预检/冻结两次resolve继续计费。

只有转换/物理化中的不可恢复异常、已无法还原的prefix失败或整请求拒绝才放弃fresh请求；Block精确移除自己的未发布节点，Function私有新组整体不发布，已可见发布按原撤回/retirement协议处理。

true/false目标共读同一不可变PhysicalReady，只重建ctx/ASM、labels、host offsets、relocation及动态scratch/flags/pending状态。第一次true不能不可逆丢弃false所需value/事件。mode若影响资源/entry选择，形成ready前给真实共同安全合同或显式合法变体；不能靠永久最大reserve/更多spill掩盖差异。重发不再RA、选择、绑定、重建logical pairs或再次lowering。第79批尚未迁kernel；现Prepare/Freeze两次resolve不可变值仍计真实工作，不能据组件Freeze宣称此重复已退出。

### 4.4 最后资源结束

semantic对象寿命在转交时结束，payload/backing/NativeInst和机器码缓冲仍可能重叠，分别计资源。原资源唯一move不等于已释放。Allocation/state只有在最后核验/物理化/恢复reader结束后才能释放；原PhysicalStream/Prepared/ExecutionAccess保持真实组和lease到最后编码/发布借用结束，不能把N清理或last lease挪出timer。

Params/overflow、Lambda traits、vector/256-bit、secondary outputs、MEM descriptor及fault恢复数据各有真实析构。原cursor负责semantic尾部，PhysicalStream::Reset按表示分派NativeInst/旧packed的DestroyAt，覆盖BackendVirtual、PhysicalReady和失败prefix；Store只提供phase/alive/退役原语，不析构ARM64对象。持久entry/fault/SMC/link metadata自持，不借已退役IR/临时policy。跨线程/重入/多Prepared按实际guard/lease协议归还，idle cache不留锁。

<a id="45-新主线唯一原ir保到真实模式与flush最后统一退役"></a>
<a id="45-当前native唯一原ir保到真实模式与flush最后统一退役"></a>

<a id="45-当前normal-native唯一原ir保到真实模式与flush最后统一退役"></a>

### 4.5 保留语义分支：唯一原IR保到真实模式与Flush，最后统一退役

**本节保留语义分支的实际能力；支持域默认已使用4.2～4.4同Store实现。** public/retained/Unknown或消费前StatePolicyRequired/UnsupportedForm按原权威保IR；InvalidProgram或Complete＋nullopt不回本分支。代码路由、真实覆盖与运行资格分开，当前结果统一见主方案7.1。

该语义continuation自3ef整合、d391沿用，并继续服务其合法分支。Prepared保唯一sealed IR与frozen Allocation/Selection，metadata move只一次；每个attempt从空context/ASM和原InstList做只读typed visitor。原true image的实际region申请失败，才完整重建contexts，以同一输入发false；不重开RA/Complete，不保存physical影子。

最终成功Flush、最后语义借用结束后，在publication前先关闭全group的roots/uses，再让原ConsumptionCursor在Semantic状态Abort、RetirePhysical并清Allocation/Selection。ExecutionAccess、persistent headers和metadata保至各自最后消费者。失败先结束encoder/span/cold借用，再在同request/authority内整组收口；未准入输入按其原owner清理。

SemanticBorrow不是OriginsReady/PhysicalReady。“已发码”和“已退役”也不是同一事实。原普通native的逐N origin覆盖和混合prefix/suffix解释已删除，retained真实wire/公开分支继续合法。Facts在TakeIR时结束，不因保IR而延长到重发。

成本如实保留：原uses/Params/Map/Selection与machine buffer同时存在，真实fallback再次绑定/编码，最后N清理仍必需。构建与最少语义仅证明原用例范围，包括同一Prepared实际true再false和显式退役；不是强制制造region失败或最终QoR/资源通过。[第七十三批](middle-pipeline-rebuild-implementation-2026-10-29.md#第七十三批唯一语义输入延迟退役与完整模式重发)、[主文5.3.4](spill-and-compile-root-cause-plan-2026-10-28.md#534-已完成最低语义验证唯一输入延迟退役与原模式重发)。

## 5. 与构造和后端交接

### 5.1 已知目标构造与未知分支

Known 的纯 target 能力只让公共 builder 选合法语义形态；位置、home、scratch 和实际寄存器留给 backend。EarlyOff/unsupported 用 generic 构造但仍为 Known；Unknown 保完整 generic 和显式后绑定。

EA/load-extend、浅 fold、state/flags deferred 等已存在，不能重复列收益。C2 当前有限纯窗口保留；四点无大幅改善后停止同类扩张。首次 residual 深分析和未来用途不能被 birth 标记替代。

<a id="511-紧凑移位的公共语义与条件flags"></a>

#### 5.1.1 已撤回的紧凑移位公共语义

78f候选贯通直接条件flags、计数规范化、解释器、两编码入口和真实retained profile，必要语义通过；完整方向没有大幅收益，新增第194个opcode、ConditionalShift、profile扩位及wire判别分支全部撤回。现行公共表示继续用原193opcode和原profile，`shift.h`不存在，不将这套候选合同保为新增实现要求。

真实失败也保留：单值Operand默认Plus，必须在构造端明确None；新增Function夹具在ReturnToHost后重复EndFunction，需删除多余调用。最终可重建实现及所有原失败见[主文5.2.1.1](spill-and-compile-root-cause-plan-2026-10-28.md#5211-当前完整候选shlshrsar直接公共语义)。它们不是当前生产缺陷，也不授权放宽schema或改变原生命周期。

### 5.2 有序整数事实的最小正确模型

同 ValueWorklist owner 按真实需求闭包求值，不另建值图。必须区分 numeric-definition 与 raw-slot：例如 U32 literal 保存高 32 位非零后经 U64 view 查询，numeric 的完整 Imm 与按原 ReturnType 遮罩的 raw 值不同。BitCast/GetResult 的 canonical carrier 高位只由该阶段实际 target hook 证明。

事实有 ready/dirty/invalid；输入重写可能使事实减弱，不能只 OR 新 known bits。Select 按实际条件需求解释 arm，rewrite 仍检查完整 schema；不能用常量结果抹掉 malformed 未选 arm。输入身份变化通知直接用户，即使 facts 相等；只对纯 facts 不变停止远端传播。

未知/前向/循环/raw 依赖受检 fallback，hoist 开新 epoch。N 行事实、闭包发现、transfer、排序、dirty 和 fallback 全入账。其范围是 width/conditional 深解释，不能代表整个 build/residual 已解决。流程见[目标设计第7节](target-aware-middle-pipeline-design-2026-10-28.md#7-整体流程与最后一次可变闭合)。

### 5.3 后端职责与唯一实际结果

目标是原Store中的唯一native程序持成功form/operand/effects，原Allocation/Writer持实际位置/transport，原state持guest副本/版本/宽度；这些不同事实不能混成一个home。family同源描述直接喂中立RA约束及最终实际效果，Selection不扩为dense程序。

RA的机器存活域来自canonical实际uses/defs、最早写点与implicit roots；旧semantic计数/SSA首访服务不同目的。折叠改变源/定义点后不得沿旧ends分配，再由binder补救。Function原CFG/entry/loop固定点保留；entry-width与form相互影响需真实有界求解，不能乐观union或非单调整域重分配。

物理化须真正兑现point/whole/LowView/explicit selected、preserved过滤和正确copy优先级，不能一律裸Map(value)。kernel允许按已物理化reg equality、立即数等做局部编码，不得回Inst/producer/Map做来源、形态或位置识别。复用193 kernel与其只读视图，不另写一套编码器。

#### 5.3.1 当前consumer的实际role与原逻辑用途共同交付

第74批已让当前raw分组供copy与MEM资源共用，原用途额外扫描及MEM逐项线性判重已退出；约4%的单次方向不能充作下一大幅收益。现码仍从活IR/Map形成当前frame并在重发时重做。这是目标要整体替代的最后读者，不能用getter/codec再做一轮小包。

迁移须分开selected numeric输入与原logical occurrence：folded EA/publication、无body的Covered仍可能有pending-spill/use-death/Tick义务。第一完整实现把这些必要义务变为同指令的实际transport/retirement事件，保raw definition/domain、view、multiplicity、terminal点与冲突，不保完整旧ordinary operands/Users影子。MEM-only descriptor保真正需要的producer token/count/reload；raw退休key与canonical物化/reload/memo key分开，后者须与scratch实际计数一致；原sole-consumer/last-use数不能替代actual future/implicit/terminal/fault/copy和width/capture检查。若改为完全实际use语义，spill forwarding/last-use/observer算法也必须同步改完。

ordinary operand直接持最终source/role/home，不再跨emit重推architectural copy。C4的native resolver收到真实consumer指针，推断copy只借一个当前完整actual-input frame；原raw occurrence与actual inputs只有完全相同时才能共借，换源后不能将两者并集当一个计数域，也不逐value重找body。explicit selected与临时推断仍区分，必须在最后成功选择处兑现exact-width、publication严格早于use、最新view/LowView优先级和最后preserved过滤，不将推断伪装为selected真值。大Params用有说明的有界整理，保WholeValue合同，不把其MEM去重与copy推断混算。

明确低视图绑定与普通Low32查询仍分开：virtual non-output Low32上的Pinned只表达当前form要求原GPRLowView；resolver独立核home/32bit/pinned后才编码，物理Pinned不由该请求位自证。Numeric→Implicit的publication保原raw cell与真实guest commit，不造假读保旧end。当前fixture已检查FPR sparse恢复→重Prepare live→原retry→真实资源/Freeze/退休；未覆盖完整GPR晚拒绝回滚或生产emitter。真实编辑数C可接近N，checkpoint费用按C实算，不因叫“稀疏”便免账。

C4的`TransferDefinitionTo`以稀疏ExtraDefinition转移machine def；C5新增Additional/ReplacePrimary与相应checkpoint，使原子主输出替换成为可用源码原语。两者均保逻辑值身份，不是跨宽whole REF，也不自动完成cohort。调用者须同时checkpoint两body、准备容量，兑现reads/effects/body/root、live与state；失败两边回滚。原逻辑raw退休仍在，无body也可能要Tick；同点独立写分开核占用，真正shared write按下节联合lease。只改owner或Map不能作为Ready资格。

独立FPR proof在最终virtual信息尚可读时完成，publication保consumer所在实际路径，foreign producer/全scope冲突另按真实合同处理。Unsupported迁移域不等于ContractRejected，不能靠关coalescing重试规避；后续form/use/def/no-body编辑使proof失效。完整合同统一见[主方案5.3.12.6](spill-and-compile-root-cause-plan-2026-10-28.md#53126-下一大段让质量决定成为真正可编码的程序)。

### 5.3.2 一个真实写的多个视图，不是完整值等价

本节合同已在C6限定域接线；2b3c/build-17与min09受影响joint一例2985断言完成原负→回滚→正事务，其余阳性保05bc/min08。历史C7/3bdd060c控制引用与普通内存组完成受影响检查；当前默认接管见主方案7.1，仍无新性能资格。中立程序分别表达三种关系：

| 关系 | 含义与允许的消费者 |
| --- | --- |
| logical value / raw origin | 原id、canonical type、raw counts/argument身份；与实际birth分开，供原质量政策、copy与退休。per-use cast仍独立。`ContainsRawValue`可确认primary退出后的合法raw header，不要求它仍是machine definition。 |
| whole reference | 完整值等价，`ResolveProgramValue`只沿该关系；宽度桥、Low32、同home和跨寄存器类不自动成立。 |
| write_owner | 同实际site/birth的一次写完整产生多个同类合法视图；`ResolveProgramWriteOwner`和原Map的`shared_write`表达物理lease共享，不合并逻辑值、raw次数或actual uses。字段已在原NativeDefinition/ProgramDefinition中，不增加N目录。 |

logical id与实际定义的site/birth分开：retimed pair、shared member与primary替换不能用id推机器出生。C6原Recolor已借ProgramDefinition修三处birth读取，preserved residence仍从transfer开始，不增加N owner目录，原全域检查成本保留。Covered Get可以保availability definition而无实际写；Get/Set逻辑guest access mask不能代替全部硬件读写。

共同core已有联合home、全体成员区间端点、一次active/pressure占用及聚合的spill-use/next-use成本；Map写口沿实际write owner更新，成员REF不参与whole-value flatten。原ProgramLiveness仍保各逻辑值真实occurrence与端点。root本身count0不表示可删真实写，短成员结束不表示可free仍活home；实际site的primary或任一member活跃都驱动`ScratchValueUses`，同时约束预算、flags结果临时寄存器、flags-only和dead-result spill路径。no-body无flags首域只能令intrinsic为零，真实MEM/implicit/terminal/Tick及preservation继续计入。

**完整共同算法仍待闭合。** 目前primitive W-view首域之外，Capture/LONG/非primitive及hard fixed/read/member-tie联合约束尚未全部移植，core对相应约束返回SemanticPolicyRequired。生产须在semantic关闭前选合法入口，最终不能以拒绝原质量域交付。C6已将全共享组actual覆盖与各raw预算接入原spill-use/region，不能继续写成一遇member就永久失去ownership；完整forwarding/adoption、MEM publication和恢复路径仍待闭合运行。不另造E图，不把去重后的物理spill点当raw multiplicity；纯GPR alias也不借另一writer的MEM lease。X宽度backing保存真实W写零高位后的完整结果。

SelectZero实际W合同已合入：backend的`NativeSelectZeroWViewShape`核Generic U32 primary、三个Original Whole scalar输入、无flags/pseudo/独立输出；真实early-write限同primary。原cohort在首次live/initial/Bind前交同site/birth U64 view，KnownW/BridgeShape独立重读。已有shared member逐项核definition/lease；不按U32类型推断写，不扩大公共`HasCanonicalU32Carrier`，不在getter补关系。joint在Submit更早处拒绝，尚未到其Capture/width断言；schema不是通过证明。实际Extra/shared-def backing和各次访问计成本。

`ReplacePrimary`可直接提升确实存在的后方逻辑结果、退出无其它实际义务的旧primary并保raw header；必须在建立依赖旧primary的shared members前操作，保持双方checkpoint无分配恢复。真实direct-narrow/跨类load selector的全部用途、访问宽度、effects/fault/observer、transport与独立proof仍待兑现。不能任意erase/reorder，不能从VOID publication造V128；仍有SCVTF/FMOV的转换/导出不能因转definition而删除计算。

`OriginalArgument`已从Original cell或唯一退休行取得稳定logical source；native实际输入给state/RA，双方不可混算。共享byte/extension/width读者同时保canonical reference与独立raw_view；原header U32不证明当前primary仍是W写，early width须读实际definitions。Covered/no-body仍按dispatcher/kernel合同保所读静态参数和Tick/guest commit。所有依赖改写须在受影响owner开放前维护live与原调度；CohortRequired携原domain/index回到真实预RA合同，局部失败恢复完整generic，不把逐候选整scope重建当主算法。

C6的NativeLocation沿原Writer编辑期借同一绑定规则，GPRCopiesFor只借原B的copy/width/capture，均已构建；BindNativeConsumers准备actual frame，不授最终physical resolver。独立proof/actual scratch先于locations冻结，最终NativeResolver仍保持冻结要求，随后全体资源准备/预检结束才首个body Freeze。完整相序、稀疏拓扑和成本见[主方案5.3.12.6](spill-and-compile-root-cause-plan-2026-10-28.md#53126-下一大段让质量决定成为真正可编码的程序)；不新增cohort registry、生命周期manager、operand镜像或证书层。

**选择改变实际消费，就同时结束旧义务。** CondSet数值输出、local condition key和由DirectSelect在自身点消费的deferred condition是三个不同合同。DirectSelect成功接管后同步清旧ConsumerAction.LocalCondition及物理event，不再伪造数值def/key；额外数值用途仍保真实result/位置。原raw计数、唯一消费者与退休义务保留，原Q逆向核Disabled源归属，最终graph独立核身份/type/occurrence和PSTATE窗口。清event不等于raw DCE，也不是generic无Result例外；不新增Passed或ownership镜像。

### 5.3.3 late编辑的值、区域与借用寿命

以下是同一程序/Allocation的协议，不新增IR、事务manager或certificate层。C6/min09受影响joint已完成原bad NoMat拒绝、完整rollback与good Backing重证；min08 MEM/width-only准备和Get32 capture等阳性保原身份，不转借C7。partial-view例保真实preserved capture与canonical Whole源，明确没有global LowView；不能为匹配夹具改same-home guard。

| 数据/时刻 | 权威与失效条件 |
| --- | --- |
| raw退休 | 原header/OriginalArgument及各raw次数；machine bridge退出后仍可有Tick/退休义务，不能造假use保旧end。Get32退役桥committed前缀的源保为无Original/Output/Implicit标志的Label，后继只核header/token/view，不回查已物理化Definition。 |
| canonical物化结束 | 全共享组实际uses、implicit/terminal/fault/capture的最后需求；与raw `last_use`独立，已合入transport以`materialization_last_use`承接。 |
| copy/width/capture与selected | 原B payload；placement/Get sweep及全部scalar/pub真实编辑结束后，按需最终fold/install再selected。纯编码修改只使既有reused base失效，不新建state需求；首次替换move-journal，Reset/retry/rollback前结束借用并重绑。 |
| spill region/obligation | 原writer/key/birth及真实旧lease；late NoBody/换源后保留SameReads、退出旧region或明确required_backing，先处理失效再独立证明。不凭旧def-owned reload证明capture。 |
| preservation | 原RA已建立真实动作，通常直接借用；只有字段/目标/scratch关联失效时move-journal后重建一次。合法MEM suffix读真实copy/transfer/宽度与CFG路径，不能一概禁用。 |
| 最后actual scratch | 真实form输出、当前绑定、实际copy/reload/preservation；MapMEM availability def或raw原参数不自动产生reload需求，selected GPR输入按实际位置计价。 |
| prepared资源handle | 成功Prepare后原pseudo_demand word转存原InstructionState库handle；payload在ResetCurrent前保存，cells仍virtual。此后只经PreparedResource读，禁止PseudoDemand、新checkpoint和普通body编辑；无N handle表。 |

body、Map/fixed、copy/width/capture/selected、reload/backing义务与派生读者属于同一失败边界。整批必要live/transport后，在最终状态独立核state、GPR/FPR/fixed/width/scratch；Commit后共用native入口width解完成原metadata，再locations Freeze，不能提前Commit只为安装copy。canonical MEM且仍有真实preserved suffix/capture需求时按RequireBacking核原动作；只有全共享组实际读取均独立GPR且无MEM/reload才可NoMat。真实writer/key/birth改变或end越旧lease回最早原owner，不晚造lease；失败完整恢复并重绑。

SameReads比较原checkpoint与当前实际source/view/role/点和Numeric/Implicit种类，count/end相等不证明不变，也不替代output/lease/fault/backing。原Edits在Stage前后各自重建整批证据；同site合并完整output/birth核对，checkpoint只比较一次，存在NoMat才整scope一次actual-use＋terminal访问，否则只看changed bodies；Stage后按原stored key一次借reload point storage。临时仅D/J行，不建N/E镜像或Passed表。两阶段都计真实排序/join、REF深度、point slots/rows及preservation的Σ D_t×A_t；原Capture、live、Stage、fresh proof、retry不消失。

D=0时Stage不建pending，仍fresh proof再普通Commit。末次独立scratch检查先在原backing完成全scope最终profile；资源两遍中的首遍顺序Decode→BuildNativeCurrent→原库→Prepare及controls预检，不再重发现profile，全部先于首个Freeze；program.Definition拒committed前缀。重复Prepare只匹配冻结快照，失败保缓存、冲突先拒；终态scratch授权前的未commit checkpoint恢复会使相应proof/profile/resource失效，授权开始后失败结束请求。实际Freeze后失败废弃scope/prefix，不进行普通RA retry。

稳定图/类型能决定的B应预RA改真实source/use/def/birth，再准备一份machine live。已合入Get32→To64首域在quality行出生前编辑actual body，随后直接生产软hint的actual source/birth和原preferences；hint的raw value/source及raw header/次数/Original/Tick保持，不能保假的wide def/use来掩盖未迁读者。无hint合法，wide-only或未经原publication资格的hint、hard Get/Wide fixed/read/tie、lease扩张、额外bridge义务及Get64/extract修改前typed拒绝；最终仍须保原合法政策。actual-use闭合批量一次，真实body编辑可逆；不再journal尚未出生的派生quality行，也不复制GPRInputs或每bridge扫E。只有位置依赖B需原owner调度维护，不把所有B堆成动态hook或逐候选重启。

普通SetHostGPR共用width/offset/ClearHigh/AdjustResident产typed PublicationValue、真实source role/body；现支持GPR/residence/Get32、Whole MEM及已建立reload。MEM32保实际Mov W，producer/resource共用原adoption代数，pending/required_backing/preservation/owns_all_uses顺序不变。独立reader按完整primary/secondary/shared output token、actual birth、canonical slot/lease与CFG证明来源，不用slot作home或本pub自证。fused、selected-memory、未建立reload及local-control MEM仍缺完整域；准备/编码通过不授guest执行或全部adoption资格。

发表的source与“当前高32为零”是两个证明：ordinary Whole可在无preserved覆盖且home吻合时借RA/live/transport；Value/Low32/preserved须有严格早于pub的真实来源。32位同home若省Mov W，核最终实际home写/捕获/CFG，不因live U32、曾经W birth或完整U64 availability就省清高位。按需查询保B级scratch和实际触达成本，不加N历史表。此读者已在原GPR site访问内替换旧片段，不新增证明层。

六类late scalar已有严格共同schema和最终位置读者，initial/actual scratch沿原入口共用。kernel的实际宽度仍按真实form/point/role核，例如And的≤32位point结果写W；不一律套definition.bits，也不把所有Definition窄输入统一压成W。首个真实修改才建立局部及原批量journal，未改候选不存快照，真实D_edit可有两个不同时期的回滚状态。全部GPR final form访问已移入原SubmitNative、在最后fold/selected前，compiler无第二遍补写；完整相序见[主方案5.3.12.6](spill-and-compile-root-cause-plan-2026-10-28.md#53126-下一大段让质量决定成为真正可编码的程序)。

### 5.3.4 逻辑入口宽度、实际flags与物理资源的交界

C6已删preclose semantic入口width transfer/solve及30B查询。原native按需解同时供residence和最终metadata；前者未用时仍须为metadata求一次必需解，不能一概报省一次。首写栈上NativeWidthInputs核真实逻辑transfer字段，late改变时typed EntryWidth回滚，不借机器use-count相同掩盖逻辑失效，也不只重算后继续复用旧copy/selected。

FinishNativeResidence在旧EndNative边界释放matcher/window/catalog，只有原B×30 facts及必要借用留到proof/Commit后写原entry/edge metadata，随后EndNative先于locations Freeze。没有把semantic最终值带到native，没有新N表、永久revision目录或第二solver。实际home高零仍由最终硬件写/捕获/CFG证明，不能拿逻辑entry-width代替。

**C7必须区分raw语义结果、实际数据def和真实控制依赖。** 原SSA header可作为Logical/raw身份锚，不能据此捏造机器Result。SubCompare只写NZCV，ConstantFlagsOnly和matched predicate/combine可能不写data；AddCarry仍有真实结果，SetCarry/OF则真正读arg0。它们的实际input/def变化先于首次live/initial，现由原Descriptor从actual形态直接生产read/view/hint/preference/def行，不再以有限D修订旧语义计划；否则假lease/spill会被一路带进RA。普通需物化BOOL保真实定义，不能按opcode一律退掉。

原递归dead-use证明须交完整accepted closure：允许的dead消费者也真退body/def，不让它们继续读取已退定义。原稀疏worklist闭合后，按graph兑现→Constant批量闭合→有限D→剩余GPRView/Get32→首次machine live接线；实际读者尊重inactive，不以availability假def修补ghost。

matched Carry/LocalCondition将typed condition身份交真实Select/terminal，可能以combine为key；它不是GPR Numeric/Implicit用。已选协议为body arg0的Label-kind NativeCell（完整reference＋RawView，无Original/data角色）与Current LocalConditionKey；原Value换槽只退休一次。PhysicalIf末尾追加默认false的local_condition，精确标记域才允许cond无数值位置；其它NONE/Map规则保持严格，不伪装DISCARD。D04/Current/JIT已在3bdd整组接通并获限定owner检查；不扩大为完整local control或native guest资格。

控制key与数值结果不是同一个决定。主树always-key/完整Carry可按独立graph退相应data def，LocalParity/部分FCmp真temp保持。已集成395的CondSetControl仅原Last＋唯一安全terminal无Result；同ID真实raw key优先，否则经原Prepare/Merge/Flush/Load取得条件。同一已集成实现的native Generic CondSet/TestZero/TestNotZero每条lazy分支均须真写Result；hard撤回Generic但同ID合法raw key已存在时，从该真实condition执行CSET，不读尚未发表的x26。该Generic修正在395获所列限定检查，数值consumer不得auto-map为key。Static header、Local事件和map命中不证明NZCV可用；Condition reload与Switch恢复的窗口分别核，后者已保存并在命中子树前恢复物理NZCV，不能一律判clobber。non-S backedge合法fallback若保NZCV可继续用已证明key；guard/PANIC不是来源证明，不能强制覆盖raw来源来伪造canonical恢复。

actual terminal只跳过精确local If的data用，raw terminal原occurrence仍从同一树读一次；两者不建第二terminal/用途图。dynamic_location/call_return/hoist后造根不混入原Header raw总数。当前公共spill读者没有新增raw-terminal Access需求；flags raw闭合与kernel退休沿原树纯raw模式借用。

Save/Branch原head若已不属actual mask，可直接借作Logical，避免每节点新增raw行；实际换槽才保稀疏Original token/view。whole-unit RA、本B emission与packed branch-only分开，原总raw次数与actual machine uses分开：Save Logical rawtotal1不自动获DISCARD，原flags-only MEM独立核backing。covered无body仍保原Tick/MEM义务；精确ConstantFlagsOnly当前intrinsic0/0不省未来observer merge。

C6 U32 bridge与flags covered可同shape，Header不能判类别；保原bridge Label0/source与prefix契约，原关系分别独立证明。ClearCompound在final form前借同一width解及原carry/terminal谓词；最终metadata共用，不第二solve、默认false或proof后改body。所有body仍virtual时把同源effective demand写原ResourceRecord，随后pseudo word才变handle，prepared/static/prefix不得再把它当需求。独立flags proof沿原site读实际图、位置、NZCV窗口和必要entry事实；无Passed表，保原事务/资源/Freeze。

数值组graph→Constant实际闭合→有限D→GPRView/Get32→首次machine live已接入原owner；控制扩域沿同一顺序。finite-D的批量闭合不代表全部reader/REF/窗口/CFG查询线性；条件key借原first/mask1 recipe退出新增逐控制输入prefix寻源。完整合同和相序见[主方案5.3.12.7](spill-and-compile-root-cause-plan-2026-10-28.md#native-flags-next-service)，逐源职责见[文件表5.1.5](middle-pipeline-file-layout-2026-10-28.md#515-下一flags服务的文件归属)。

### 5.3.5 普通内存的实际控制字与逻辑投影

主树普通TSO/Extend不加IR槽或memory表，已在3bdd完成同槽转换、全部逻辑读者及限定Ready/resource检查。原LoadExtend arg1在native Cell3，semantic 4bit descriptor与PhysicalMemoryControl编码不同；转换尾用checked/native共享规则写实际控制字及LoadExtend form。state、flags和独立verify借NativeLoadExtendDescriptor同源逆投影，不再将实际控制字当4bit值，也不额外存raw立即数。

访问字节数、结果宽度、sign与extend32清高分别保真；原TSO/RCpc、unaligned/fault/barrier、MEM forwarding/backing/reload照走。所有真实读者与最终resource同组接通才缩粗门，fused/复杂Map相关EA、direct-narrow及跨类另闭。限定检查不授normal、guest或性能资格。见[主方案5.3.12.8](spill-and-compile-root-cause-plan-2026-10-28.md#native-memory-next-service)。

<a id="native-local-control-ir"></a>

### 5.3.6 本地token、路径合流与可逆cohort

本节消费者已整合，WB实际读口与fixture phase修复后dd0原owner/NarrowRead通过。下列为数据合同，不把所列原例授成全部合流/capture/P/MEM组合；历史395/dde/cee0等保原source，具体资格见主方案7.1。

Goto/NotGoto/BindLabel的VOID或保留U64/V128 header不是数值定义；actual角色由真实kernel决定。数值条件一data use，显式key零data use，Bind零data use/def。Goto条件原occurrence照常raw退休；BindLabel→Goto只是完整身份的control边，独立核唯一绑定，不进data ProgramRawUse/Logical MEM退休，不造REF、Implicit或DISCARD。BooleanBranch省normalizer仍须原HasBooleanResult、真实view/birth及数据物化合同。

同一程序在Bind先拆taken/fallthrough；Bind prefix仅fallthrough执行，taken保Goto当点prefix后的快照。state记录活跃copy的version/home/width/extension，Bind取共同实际副本，不伪发表；logical byte相等不证明taken home存在。prefix覆盖home时point>0的旧copy止于point−1；point0真实capture原位更新初始width run（含unknown），不下溢/重叠。新copy publication=point且只供严格后继，当前点读原preservation descriptor。实际writer按definition/site/birth、form/effective demand区分Value/Temporary/DynamicScratch/MayWriteValue/None：同home Get及真实extend可能无MOV，MEM birth可写reload.reg，仅MEM输入发LDR；MayWriteValue只作负向clobber，不授真实写或高零种子。六参BeginNative借原resources/config与prepared UnitScratch；partial/offset沿同源ByteOf，MEM/capture/高零独立受检。

Get值宽度属于定义点，不能在晚Set点借当前home，也不能推physical home真W写。local多incoming摘要进原唯一Function solver，普通块保unary快路；实际Q_get在定义点捕获表达式，local内联，foreign以来源B/Get点及真实稀疏依赖接原队列。Get改变而来源B exit不变也唤醒消费者。独立oracle逐路径求具体Get，不借被测摘要生成expected；local/foreign独立oracle在c984定向通过，不授全部foreign/capture资格；不建N历史、第二CFG或每轮IR重放。

legacy Save保实际控制路径与发表点。intrinsic FlagOutputs在producer；SaveResult在Save点读真实result/必要operands，head仅Logical raw退休，ADC/SBB不用Save时当前CF。稀疏emission override不改raw packed需求；目的地、retains、scratch和资源共读effective需求，handle不可读成需求。原key map的birth只作失效过滤，跨join仍独立核NZCV；L快照沿原flags owner合流。Save新增真实use/live、Params和scratch，可能增加capture/spill/reload，不是零质量费用。

新use必须先于allocation owner重准入原fixed。原installer显式标Origin，Derived不按opcode猜；首次actual live后检查新增窗口/terminal/REF并按原capture政策履约，不只SetLiveEnd。Explicit实际use扩域已沿原capture/fixed_end/allocation_begin接通；GPR/U64及Function双capture限定通过，U32/MEM外部压力例待验证。三类真实能力残余保typed边界，不能概括全部REF缺失或写成全域完成，也不能永久关政策。

metadata可逆相序是：preclose只选原metadata、保ScalarRecipe→唯一Native转换得合法baseline→原static journal记录后Apply→Constant/有限D/GPRView/Get32→首次live。NativeVirtualCheckpoint只记Extra尺寸、只能收缩，必须保旧prefix并append编辑；转换前删掉的Result/body不能靠该checkpoint恢复。ZeroTarget仅真换源保原raw token/view/sense，actual读新源、raw仍退休原occurrence一次，不在late直接换回use。

只有精确MetadataCohortRejected允许原scope一次general重建：先回滚attempt并结束state/resources/inputs借用，取回唯一AllocationState。旧关系有效时清Map/soft birth和fixed声明/ready索引；删Derived后，在旧gpr仍活时恢复Explicit ordinal，再保CallABI清generated preferences并reset gpr/actual列；逆body→Withdraw→冷路重收C/P→actual static/fresh live/Describe→原RebuildProgram。已无flags/Get32语义行undo或旧质量计划恢复。Inputs的const身份不复制/赋值，Function不拆Block、不回Inst；一次metadata、原FPR禁用/独立次数和预算配置保留，RunAttempts每次进入仍重建局部eviction8，不能虚称跨重建共享余量。终态scratch输出后禁止冷重建；dd0 owner包含精确metadata恢复路径，但不授所有metadata/P/MEM组合。完整顺序见[主方案5.3.12.10](spill-and-compile-root-cause-plan-2026-10-28.md#native-cohort-owner-rebuild)。

这些journal/快照是恢复或分析状态，不是第二IR，但全部计费：F行资格比较/受影响窗口及D_f×F移动，Σ_Goto activeD快照/排序/交集，publication扫描/逐发表追BitCast或ZeroExtend链/去重前候选排序，以及distinct Q_get摘要/依赖/队列Φ；另有L flags状态、每query的B级seen/profile/位置解析、实际D编辑及一次失败N/E/P＋RA。byte-query为32×B backing/次、每word至多8次；PhysicalIf的inline optional原条件payload还增大所有同类节点，不能写成纯D存储。符号、净账与旧工作退出见[主方案5.8](spill-and-compile-root-cause-plan-2026-10-28.md#58-正常路径的工作量表与退出标准)。

当前已有5.3.8的限定location/RSB/call-return域；selected memory及部分wide/fixed cohort仍未闭合，foreign宽度方程存在不代表所有foreign MEM/capture/partial可用。没有全域或性能资格；原支持域默认接管与normal真实覆盖分别见主方案7.1。文件职责见[结构表5.1.7](middle-pipeline-file-layout-2026-10-28.md#native-local-control-layout)，不增加控制图、恢复manager或每节点证书。

### 5.3.7 原scratch存储的终态覆写是所有权边界

原AllocationInputs::scratch先为RA提供seed；GPR/FPR/transport全通过后，Writer以唯一原program/live/input身份交终态输出backing。末次独立scratch检查从实际body/live/location重求，不读旧seed自证；原向量clear后压缩写S行，资源阶段唯一move、顺序Decode/BuildNativeCurrent/Prepare。**资源循环已无整轮profile重发现**；不是多存一张最终表或把两个实际生命周期压成Passed位。

终态授权一旦开始（含clear前检查）就不再metadata/FPR retry，部分输出失败结束请求。全scope最终profiles完成后，逐body结束自己的pseudo需求/资源读取，再由Prepare复用该word存handle；全scope虚拟cells/schema读者都完成后才Freeze。不能把全scope资源结束误写成首个Prepare的前提，也不能把handle当pseudo demand。checked Take保持原相位，wrong owner/非法相位/非空重复move拒绝。新增约N比较、S行append、N−S次端点更新及grow照计，initial/state/final独立推导及Prepare/Freeze resolve仍保留。准确相序见[主方案5.3.12.11](spill-and-compile-root-cause-plan-2026-10-28.md#native-final-scratch-handoff)。

### 5.3.8 terminal机器root与固定值多个时段不能混同

原PhysicalBlockMetadata两terminal root在semantic关闭前保存完整token/id/view及raw count。SetLocation动态读取是否延期取决于原Block/Function/L1/尾部政策；CallReturn/Pop尾部须按真实terminal点读取，不能用早期marker use替代。机器live、MEM/reload和physical绑定沿这一点延长；派生root不新增raw header count，原body occurrence只核一次、退休一次。PushRSB动态/noRSB/L1保原Covered语义，Pop真正行为仍归terminal；JIT只借当前原B metadata，不建第二树或读已结束Inst。精确支持/拒绝见[主方案5.3.12.12](spill-and-compile-root-cause-plan-2026-10-28.md#native-control-location-domain)。

fixed的原home0/home1硬窗口、真实capture与普通Map区间各有不同寿命；已合capture/fixed_end/allocation_begin分别表达。REF alias C与root P>C保存可同时必要，canonical lease覆盖C不移动或删除P动作。独立read/action/byte/支配及相同AllocationId仍要核；无after-body点、不能由窄shared-write成员建立宽backing、capture超root真实寿命保精确typed residual。dde原例457含GPR/U64、完整Function双capture及U32/MEM真实压力和完整Residence，不授全fixed或fault/guest质量。细则见[主方案5.3.12.13](spill-and-compile-root-cause-plan-2026-10-28.md#native-fixed-capture-contract)。

发表控制字的同源逻辑投影与之相配：NativeGPRPublicationByteOffset只解码byte offset，不授shape/home/window证明；缺失/畸形须reader失效。私有模块没有完整静态guest bank而无affinity，或部分Get32已capture后WholeValue改home，均可合法回退；不能用手填Map或放宽守卫伪造ZeroTarget正例。

### 5.3.9 稀疏fixed结果沿原Allocation活到最后读者

三字段不再内嵌所有普通placement。原Allocation以32B placement/u32 slot指向每页32条的stable fixed结果；这是原分配结果承载，不是IR、完整operand副本或Passed目录。immutable输入F行、结果active roster、home/preservation索引各保原职责；只有原Writer改可变结果。

slot身份跨epoch、切inputs、metadata/FPR retry和rollback保留；Finish结束原inputs后仍供实际emit/reemit读者读取。仅最后读者结束后的Release重置本请求K/slot，保留页按原128KiB aggregate预算全额核算。ordinary undo保小placement，只有D_f复制完整fixed undo；页增长不能使已借definition失效。

主承载按32N＋1536P_h及指针/heap/索引/事务计，不是简单76→32；K包含本请求墓碑，P_h可来自前次高水位。新页未用尾行仍初始化，dense fixed可更大，额外indirection仍执行。已删普通N内嵌构造/epoch复制、普通D full-fixed副本及同请求换inputs后的N重初始化；N列/Freeze、F重资格和独立proof保留。源码账不授编译性能，见[主方案5.3.12.14](spill-and-compile-root-cause-plan-2026-10-28.md#native-sparse-fixed-result)。

普通memory stage01～06已合，WB extra SSA、实际fixed读/version与JIT Definition保持；actual base取Cell1、窄Store取Cell3，地址matcher仅raw，兼容桥退役。stage06已替代stage05早对齐子集；dd0四目标与所列原例通过，不代表所有压力/P/MEM组合或normal触发覆盖。EA/value/result、真实FPR写、raw身份、fault/terminal与fixed合同各有独立消费者，不从kernel存在推准入或全域质量。

### 5.3.10 本轮纠正的phase、view与真实程序合同

NativeCell.Type表示当前边的有效类型，RawView保显式cast，canonical definition由同Store id/identity恢复。V128→V64不能以canonical Type替代边类型，也不能把view写回定义。pure EA读取真实Result的id/type/identity，允许原参数布局决定结果槽；unary Plus＋empty right是合法现存形态，仍核无shift/ext与真实source。

NarrowLoad retime须保原extension的精确目标宽度；signed byte/half→U64实际写X，high32不是已知零。访问宽度、logical extension与effective view各归原schema/reader，不合并成一条“窄值”标记。

virtual event/operand的最后读者在Freeze前。retired ZeroTarget的committed header核stable opcode/form/type/raw/effects后不再读已清Cell4；uncommitted仍核原事件与operand。不得为保旧validator延长死槽寿命，也不能因committed略过仍有效的raw/资源合同。该phase修复随后随9b原owner1861及33da原owner1989整例通过；新增WB当前仍失败，不转授其执行资格。

真实guest home写行在首次RA之前交原clobber初始化；晚state不能通过只报CohortRequired、却不编辑body/use/def来要求一次不存在的删除转换。当前保全部baseline写，删除原native晚请求检测后已获上述owner限定检查，checked旧DSE不动。新增死写消除必须在pre-live同步修改实际程序，重建对应live/输入；common现有DSE子集不是全域QoR等价证明。

partial MEM/fixed的原完整case457通过，是真实capture、raw Get退役、Residence/transport与fresh proof证据；它不授全guest质量或当前lane/load资格。PendingValue的32B也不是可退役语义payload：FlagsIntent与后续guest flags观察仍活，只有已完成的值参数解释退出。

<a id="native-actual-use-input-contract"></a>

### 5.3.11 actual输入直接生产的身份、用途与固定读合同

**actual输入组已经合入并构建；raw资格与machine use/def/lease分别保真。** 同槽转换记录原C/P候选，全部Function成员提交后规范，actual memory/flags/W-view/Get32闭合后才建首份machine live。原Descriptor的body/use/raw/terminal访问直接生产原GPR/residence/Derived-fixed；旧semantic质量计划、flags修订、Bind和WB一点桥已退。

`ResidenceUse.actual_kind`为空仅表示raw资格边，非空是公共`ProgramUse::Kind`的Numeric/Implicit/Terminal；公共层不加入ARM form或Passed。final reader按真实完整consumer、point、kind及原raw multiplicity独立核，terminal无body身份须精确定义。40B ResidenceUse、64B BlockRange有本版静态断言，但C/P、raw R和actual A与最终行同时存活的capacity/排序/compact仍计费；C/D可=N、R/A可=E。whole REF等价与write-owner/Low32/宽度共home分开，实际lease不能用raw计数代替。

WB保持逻辑ID/raw Tick，extra definition出生在真实access，原update无机器body；Derived Get末端直接用actual use，output version=true。base Definition位置正确GPR却pinned=false时，由实际`FixedGPRDefinition`的full-X、owner/end/last_use/version/elided及无transfer/capture等字段证明forced read，另核quartet/home/no-reload/locality/canonical lease/不别名；JIT按选中Definition读真实X。纯reader保留，旧Origin兼容包装已删。

同值无机器copy的发表分别证明早于publication的真实source和guest witness/末端窗口；AccessesCross仍是`(after,through]`，不额外跳过邻点。BirthOnly跨真实Goto/NotGoto/BindLabel suffix不乐观保模式。source纯桥/复杂Copy/WB gate仍可建semantic ValueLiveness，首次图成本未全部退役；精确typed拒绝发生在Consume前，Consume后不能回已死Inst或扩大metadata retry。

冷metadata必须先删Derived、旧gpr存活时恢复Explicit ordinal，然后清generated preferences/旧actual列并reset gpr，最后逆body/Withdraw、重收C/P、fresh actual static/live/Describe接同owner原controller。保持一次general重建和原FPR独立状态/预算配置；RunAttempts局部eviction每次进入重建，不能说跨重建累计共享。checked/Unknown/raw合法semantic服务保留。

actual输入/默认接管的代码职责不授全部输入质量或源码级≤2×保证。合同见[主方案5.3.12.15](spill-and-compile-root-cause-plan-2026-10-28.md#native-actual-input-service)，历史与当前资格统一见主方案7.1。

### 5.3.12 已合的最终位置/P数值事实不成为第二份IR

stage06已在原PreparePhysical和FreezePhysical两个循环分别生产/独立重算low3值；capture借原preservation action两个尾padding字节，普通spill借PhysicalValue两个prefix padding字节。事实随真实P的value/source/slot/width/offset/lease，读取沿用原actual-use/fixed/backing/控制合同，另核同B、actual owner、实际先后/capture prefix、完整alias和lease；本包不新增CFG支配求解，不以原Get home、slot相同或Passed证明。action16B有静态断言，PhysicalValue新字段已参与四目标构建；无单独sizeof/峰值结果，不授尺寸不变；数值读写/初始化和两次B-local fold收费。

P的值相等只沿原REF追踪，遇shared_write停下；MEM definition P须严格早于read，capture P按prefix可同点。AddCarry与helper保真实kernel/恢复语义，不以假定CF或raw ABI mask授low3。

Get source/offset与whole alias link只延寿到原FreezeNative publication visit的最后reader，随后结束，不进入Ready、不新增N历史。stage05早SSA producer/reader同包退，checked/Unknown HomeAlignment保持；ForceChecks/fault_emulate/16B/biased/page-table及未知/未证backedge保guard。方案/逐文件见[主方案5.3.12.16](spill-and-compile-root-cause-plan-2026-10-28.md#native-final-location-alignment)与[结构5.1.12](middle-pipeline-file-layout-2026-10-28.md#native-final-location-alignment-layout)；当前[默认接管检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-default-checkpoint.json)只授同版构建和owner/NarrowRead，不授全部P/MEM组合、normal覆盖或时间收益。

### 5.3.13 canonical状态、raw NZCV与条件身份不能互相代证

普通SaveResult正确发表guest flags，不等于留下相同host NZCV；当前窄U8实际路径会恢复incoming PSTATE。LocalCondSet的key/LocalCondition event只说明交付方式，数值fallback CSET也直接读PSTATE，二者均须证明完整incoming mask。把event清掉或加Result不能补来源；CondSetControl按原pending/key/必要canonical恢复合同执行，也不强制数值位置。

BranchOnlyEdges只承载真实双后继dead证明，不是raw NZCV许可。只有原flags pass证明required全mask由primary提供、完整primary/发表→condition窗口安全（含AdvancePC后gap）后，才在同一RewriteContext把Save和同身份/type的CondSet一起交接；marker、节点身份与机器来源分别保真。没证明时保普通Save＋canonical CondSet，不以最终Reject冒充合法执行。

最终prefix证明在原actual visit中先Bind meet、再核当前LocalCondSet全mask、最后Observe；覆盖key和numeric两域。物理Carry可读不替代原极性/条件映射/tuple/window，普通Save的旧physical_values不能被当成新结果。唯一例外是已有metadata+integer独证且无body/PSTATE读的Covered退休ZeroTarget；不新增Passed、N表或consumer证书。条件产生后的last-consumer窗口仍由原graph证明，不能用incoming mask替代。

逻辑type与实际W/X亦分开：已合修复允许U8结果/U32输入共用W，独立reader核实际kernel位宽、view及mask；可选选择失败保原Generic参数槽，不提交一半role/form。此处沿用同Store和原用途/位置协议，不需要另一种IR。

完整窗口、成功交接与incoming独证的合同见[主方案5.3.12.19](spill-and-compile-root-cause-plan-2026-10-28.md#canonical-raw-flags-obligations)；旧候选缺口保历史，不回填失败。当前构建、原例和guest资格统一见主方案7.1。

### 5.3.14 实际写位与consumer角色不由语义opcode或结果类型代证

`PstateWrite{defined,zero}`沿原prefix瞬时传递：只对真实S form/demand记录动态值和硬件CV零；非S OR/XOR、Covered等不因opcode获得raw资格。legacy Clear须有当前hardware-zero来源，GenericSave仍核同source；Meet交集、新writer覆盖、clobber/替换性恢复失效。ordinary SaveResult仍撤新requested资格，不能借旧zero冒充新N/Z。RawPstate加一个Flags使正常64位布局32→40B、已有LocalPstateEdge 48→56B；不是新增每N表，但额外容量及访问必须扣账。

Low32是源的真实view；唯一consumer可以是VOID StoreTSO。PinnedW资格由现有NativeGPRConsumerRoles在真实ALU＋4B producer域产生，不能先把consumer当producer做size查询，更不能删用途或手写Map。861a当时原owner3024通过含此回归，不代表最终质量。

metadata撤回恢复同一owner的actual程序并重新交输入/live/RA及publication，原预算和独立FPR状态保持。GPRUseHome(any)的candidate不等于Value最终位置：原Whole仅在width、W/X bits、no-reload及home吻合时retag；合法候选miss清residence保Whole实际GPR/MEM copy。原explicit Value仍严格，identity/type/raw-view/consumer/role错误或invalid不能吞为miss。no-copy、高零、preserved capture/backing与全scope独证继续按实际源证明，不新建位置目录或回已死Inst。

物理role与pinned亦独立：Value可合法读取unpinned canonical GPR；EmitSetHostGPR借ScalarX→ScalarRead→UseGPR兑现，Whole/MEM沿原物化与MaterializationId/reload，原R/X/W Whole guard保持。register cell保id/canonical/slot/view，不伪造producer token；MEM按真实transport保完整身份。资格统一见主方案7.1。

独立完整性不要求每Getter重扫全scope。A在既有最终actual-use访视复用已绑定PhysicalValue，按WholeREF值等价归根，保真实cell role/raw-view、Getter所属B、ordinary/implicit先读及terminal后读。proof内按实际Getter稀疏保存observed count对fresh live.UseCount，缺行按0；无use Getter仍核本体/Definition与完整global lease上的fault/capture，不能用shared-write当值等价或截到B.last。新增哈希节点/bucket、每view map对象及解析成本照计；RawDirect和剩余provenance并未退出。

### 5.4 下一次结构改动必须服务于提交与读取算法

M1在现builder/rules内减少注定消失的中间节点：append前纯选择、一次typed提交，少掉真实参数编码、counts/owner/uses维护及后续访问。已有birth/EA/deferred扣除；AfterAppend不算“未出生”，miss走一次generic，不能回滚并重放guest行为。

幸存节点的首次checked与稳定目标输入共访已完整尝试并撤回：它确实退出普通Describe和第二次target参数遍历，但保留主要后置求解并增加回调、B ranges、候选及索引恢复；完整方向B+0.81%/F+3.22%，各side/mode一点，无大幅收益。当前恢复原eager Facts→Describe/Finish，不保FinalFactsVisitor、defer或ConsumerPrefix接口。结果与完整源码包见[第七十五批](middle-pipeline-rebuild-implementation-2026-10-29.md#第七十五批首次facts共访负反馈与撤回)。

后续改变表示或交接须随真实需求/实际位置/state/选择服务退出，不再单独融合外层访问。公共analysis不含ARM64，不建全N typed副本、第二图或每E缓存；GPR窗口、FPR条件全量扫描、consumer完整solver及rich scratch仍是当前工作，不能列为已消失。

SupportsIR的优化前宿主支持与优化后schema/SSA检查属于不同输入状态。仅完整受控构造/编辑合同可缩减同义检查；Known/fresh/seal本身不授权。raw/manual/import、Unknown/EarlyOff仍有完整generic和必要检查，编辑失效沿同一owner。

#### 5.4.1 公共语义形态的字段与写口合同

第74批公共表示替代已覆盖同一语义schema下的构造、编辑、用途枚举、公共分析与目标读取，原packet已删除。以下是继续维护的字段合同，不是再开一轮格式迁移；后续减少真实出生和首次服务，不保第二格式或N行decoded缓存。

| 字段/行为 | 真实权威与必须保留的合同 | 目标工作 |
| --- | --- | --- |
| op/result/effect | 原ir.inc/ir_meta语义与结果规则 | 同源构造/读取/枚举，不再由多套描述重新解释 |
| Value与Imm | definition/session、per-use cast、完整Imm及numeric/raw差别 | 普通字段直接交付；不用物理home替代语义定义，不截断高位 |
| occurrence/roots | 原counts、唯一用途owner、counted/implicit/terminal区别 | 同一受控写口提交和通知，少出生节点才少维护，不制造第二用途图 |
| 替换/删除/插入 | 原RewriteContext与Store/owner、真实epoch | 容量先准备再提交，实际失效；form变化不是“已验证”证明 |
| Params/Lambda/foreign | 完整长度、traits、资源/异常、公开受检入口 | 复杂数据保真实escape，只按实际规模付费；不强塞定长槽 |
| 最终读取 | 成功schema/SSA与支持准入，各有真实边界 | 与稳定目标需求共用字段；失败不发表半成品，Known/seal不豁免 |

这些职责落在原ir.inc/ir_meta、instr/args、builder、uses/rewrite和Facts/target，不增加forms registry或通用reader manager。公共form不含ARM64 form/home，Known/EarlyOff/Unknown均可表达同一公共语义。整个consumer链迁完才结束旧解释，不能只提供一个新GetArg接口。

幸存节点仍需真实首次roster/CSE/state/flags/SSA/live等服务，逐项填[主方案5.8工作表](spill-and-compile-root-cause-plan-2026-10-28.md#58-正常路径的工作量表与退出标准)。现普通Block UseGraph与Facts复用先扣除，不以共访声称首次义务全消失。

## 6. 真实源文件职责

| 文件 | 修改边界 |
| --- | --- |
| `ir/ir.inc`、`ir_meta.*`、`instr.*`、`args.*` | 同源公共字段/写口/枚举；必要时按5.4.1整链替代旧参数解释，保真实资源、编辑及最终清理，不放ARM64 form |
| `ir/store.*`、`order.*`、`uses.*` | 粗 phase/稳定身份、实际 stream 顺序、关闭用途图、bulk 结束 |
| `ir/block.*`、`function.*` | 持久地址/parent/发布/退休与真实锁；独立Block仍持自身InstructionOwner/InstList、按需block_uses和IRStore lease，消费只结束其语义部分，不增Function包装 |
| 目标 `ir/function_ir.*` | 承接当前hir_builder中的HIRFunction/HIRBlock/Edge、CFG/RPO、FunctionUses/EditBinding、Store lease和entry/provenance；唯一真实编译owner，不复制新图 |
| `ir/hir_builder.*`、`hir_storage.*`、`hir_pools.h` | 前者最终只保构造/建边/current-block；后两者保真实CFG pool/session/lease，与IRStore页和persistent Function生命周期区分 |
| `compile/unit.*`、`optimized_unit.*` | 请求消费权和 guard/group；原 seal/Facts 借用边界 |
| `compile/passes/value_worklist.*`、`analysis/value_bits.*` | 同 owner 深事实/反馈与纯 transfer |
| 已存在`backend/arm64/compile/native.*` | 原Store同槽NativeInst、virtual/physical operand/effect与原program适配；有限真实kernel借用已接入，控制/内存接收域继续闭合，无第二程序owner |
| 已存在`compile/allocate/program.*` | 原程序/CFG的中立借用与machine live/core；保两scope和公共质量政策，必要唯一关系索引沿真实owner，不复制N/E程序图 |
| `backend/arm64/compile/physical.*` | 共有stream/B metadata、实际资源、冷/public及旧packed replay；支持域已使用稳定NativeReady，合法保留分支继续原binder。路由、覆盖和资格见主方案7.1 |
| `backend/arm64/compile/select*`、`compiler.*` | 同源form/operand/effects、最后semantic closure、两范围RA与成功兑现/独立检查；PhysicalReady共用原重发/最后退役；不新增目标pipeline或生命周期owner |
| `backend/arm64/compile/flags_graph.h`、`flags_native_graph.h` | 原checked/native共享的纯图规则与实际/raw借用读口；有限D、control、dead整数closure分别重证，不养第二flags图或Passed表 |
| ARM64 `jit/*`、runtime/fault catalog | 193个physical kernel只读已绑定参数；public/retained语义handler仍有原读者。布局/重发调用对应入口，持久发布不借临时语义 |

当前支持域的NativeReady与合法retained/public入口共用既有kernel，不重新立项编码层迁移。plan兼容接口和重复决策随实际算法收口；整体覆盖、语义及性能资格统一见主方案7.1，结构与退役映射见[结构表](middle-pipeline-file-layout-2026-10-28.md#3-ir-与公共编译层逐文件职责)。不为同一职责新增view/editor/ownership/certificate链；有真实消费者、失效和结束点且能净退旧工作的唯一关系索引可以采用，不能由表数量一概否决。

## 7. 进一步改变表示的准入条件

先改真实提交与读取算法，当前同Store实现也受相同净工作与完整反馈约束。选定真实纵向组必须贯通其全部生产者/最后读者，让新服务替代旧服务；不要求全部族迁完才首次反馈，也不为每个helper加QoR门。未覆盖域走前置合法分支并入总账。不能为“更像FEX”只压槽、保重复physical影子、提前销毁仍有读者的输入或用getter当主路线；已停P0/codec和无收益Facts共访不恢复。

如确需改变表示，必须连生产者、编辑/分析、所有消费者、失败和结束一起迁移，并真正删除旧codec/循环。净资源账包含：

```text
峰值 = 同一Store槽/prefix实际capacity（semantic/native body不重复计）
     + 当前payload/overflow/holes/alignment及转换时新旧Params重叠
     + 已启用uses/roots/CFG/order和当前epoch Facts
     + Allocation/Selection及实际state/transport
     + machine buffer及当前op临时buffer；或retained完整wire及真实资源
     + owner/chunk目录、grow重叠、多个Prepared和idle retained
```

支持域默认同槽转交后计native backing，source miss/public/retained分支仍计semantic保活；不把同一body加两遍。另比较实际初始化/复制/清理字节、operand/role/edge访问、alloc/free和last lease。区分live、capacity、触达字节和跨请求累计；128N槽容量不等于每次扫描读128N，更小结构体不自动减少解释次数。无真实需求的困难状态不建立，资源移交不算释放；历史IR交叉投影不与阶段账相加。不能只提高retained预算或移动free就报告提速。

若新链完成后仍运行完整旧链、另保完整logical shadow，或kernel回查producer，候选未成立。中间成组构建/最少语义，完整算法一次方向，最终门只沿主方案6.3/6.4；不为每个字段做矩阵。

## 8. 源码与设计依据

当前入口：[Inst](../source/runtime/ir/instr.h)、[Arg](../source/runtime/ir/args.h)、[Store](../source/runtime/ir/store.h)、[order](../source/runtime/ir/order.h)、[uses](../source/runtime/ir/uses.h)、[TakeIR](../source/runtime/compile/optimized_unit.cpp)、[worklist](../source/runtime/compile/passes/value_worklist.cpp)、[DCE](../source/runtime/compile/passes/dce.cpp)。

本次设计依据为[3ef差距审查](compile-gap-review-2026-10-31.md)与主方案；旧[构造草案](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/whole-compile-service-redesign-20261030/construction.md)、[物理消费者草案](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/whole-compile-service-redesign-20261030/physical.md)、[最后消费者草案](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/whole-compile-service-redesign-20261030/lifetime.md)保留历史，不覆盖当前native延迟退役事实。

已完成必要语义检查的02ae基线中，P1首段只调整正常私有编译请求的fixed核验责任边界：原每attempt独立writer重建退出，Complete位置冻结和实际写入后的完整核验必经；公开/manual/retained保持原检查。它不改变IR布局、epoch、Known/EarlyOff/Unknown或消费权限，也不新增逐节点资格表。构建/必要语义见[第六十七批](middle-pipeline-rebuild-implementation-2026-10-29.md#第六十七批正常私有编译路径收敛-fixed-核验责任p1首段)；没有新的compile计时或最终质量资格。

历史组件断点不转授当前source；MEM、入口width、transport、prefix与完整Function/raw/lease仍保各自合同。布局和共享纯函数不保证性能；M1构造/首次服务、后端净工作与M4存储触达共同按主方案7.2完整交付，不恢复已撤Facts共访。

8cda2889为当前源码检查点：A产品已合，translator与swift_ir_test两owning目标构建成功；三个原case为2过1失败、3714断言中3713通过。public owner和fixed-suffix整例通过；新增NarrowRead负例在绑定时继承旧源专属placement而失败，尚未到stale-count检查，不记为产品回退或全绿。外部02只修该fixture，未应用、构建或执行；8cda没有新的ordinary计时。

e0d6仍为最新普通完整四点：Block 2066.780ms、Function 1099.290ms，各自fresh固定FEX为46.624/47.297ms，约44.33×/23.24×。均CRC通过，Function原失败/Block重解码fallback已计入总时间；这是开发方向，不授最终质量、完整生命周期或七对采用资格，ordinary实际NativeReady覆盖仍unobserved。

3625的独立Block＋完整Function归因已闭合：native GPR proof分别695.636/103.768ms，占各自native admission约88.77%/89.41%；这是A前诊断版，不是ordinary比值，也不是A全部可省时间。A保留的provenance/window/CFG/fault/capture与RawDirect成本仍在。详细身份、原失败、统计端点及后续结果只以[主方案7.1](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)为单一入口；[归因原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/native-admission-3625-summary-01/summary.json)、[A构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-consumer-read-proof-build-01/build.json)和[A原例](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-consumer-read-proof-minimum-01/execution.json)保持独立资格。本轮仅修文档，goal保持paused，不应用02、不构建或上机。