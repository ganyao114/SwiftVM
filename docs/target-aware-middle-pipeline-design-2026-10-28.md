# 已知后端的直接构造、通用分支与前后端解耦

**当前状态（2026-11-02）：8cda已合A，最少检查仍有一个新增fixture失败；02测试修正未应用，当前版本没有新的ordinary计时。** 最新普通方向仍属e0d6，最终两倍目标未达。完整source、构建、原例、归因及ordinary资格统一见[主方案7.1](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)，本文末段只列本轮身份边界。

本轮按[净工作与路线修正](spill-and-compile-root-cause-plan-2026-10-28.md#compile-net-work-correction)及[审查42](compile-gap-review-2026-10-31.md#performance-route-correction-latest)修订。同Store是当前选用的实现，正确性合同、真实旧工作退出和完整反馈决定是否采用，不能由载体、文件数或少表推出性能结论。

本文规定decode可做的工作、Known/EarlyOff/Unknown与后端交接；不另设优化队列或采用门。源码、ELF和测量身份以主方案7.1为单一入口。

**DBT 的普通 JIT 已知后端，应该利用这一点少构造、少改写；前端通过公共语义能力提交，实际物理位置仍由后端决定。** Unknown 保留完整 generic 与显式后绑定。当前native与必要physical replay改变编码输入，不反向污染公共IR或frontend；public/retained的合法语义编码入口仍保留。

支持域现默认沿同Store actual链走原scope RA/事务/独立proof/resource，稳定PhysicalReady供true/false重发与退役；消费前source miss/public/retained/Unknown保原IR/Allocation/Selection分支。已观察实际native块，完整负载NativeReady覆盖率尚未建立；提前结束语义对象仍须闭合全部真实读者，Known不授消费权。第75/76共访和紧凑移位、第78实际位置整包因完整方向无大幅收益撤回，不重开同类调参。

第77批保留的实际结果目标合同归后端成功选择：direct-narrow最终写入哪个home，须在实际分配与合法选择后输出给原核验消费者。decode可以表达合法的公共语义及纯能力需求，不能提前把候选home写成真实机器写入，也不承担GPR核验证明；新增有序核验层已撤回。

已撤的第78批实际位置目录与exact-use分组曾归公共分配器及原Writer，不恢复为下一前置。第79批中立program沿用解耦：公共层只认识机器值类别、use/def/REF与中立约束，ARM64 family/schema和实际资源留在backend。Known/EarlyOff/Unknown继续有效，Block与Function给共同assignment各自真实合法范围。不能以该后端迁移替代M1少构造，也不能由少数消费者已迁认领整个选择/state成本退出。

**当前后端选用同Store的Semantic → BackendVirtual → PhysicalReady，作为具体实现而非唯一提速前提。** Known早建仍提交公共可编辑语义；frontend不承担home/lease/P事实证明。若其它窄表示或索引能贯通真实消费者、净退重复工作，也按同样合同评估；原撤回结果不因换名字重新授信。

## 1. 设计目标与边界

要缩短的是一个 guest 动作从解析到实际编码的处理链。当前支持的常量/立即数、EA/load-extend、浅 width/predicate、state/flags deferred 直接使用，不再安排一轮同义目标绑定工程。

本次明确两条相接的算法：M1在公共语义层减少节点出生和首次重复解释；M2/M3在后端把稳定约束、两范围分配、架构状态和实际操作数交付收敛。已有Known、birth、EA、IntegerFacts、deferred、有限C2窗口是基础，不能再次认领收益。仍需未来uses、CFG/observer和实际位置的决定不强塞进decode；Unknown/EarlyOff不是只会导出IR的摆设。

## 2. 当前已有能力与不能重复认领的工作

| 已有基础 | 继续使用 |
| --- | --- |
| CompileEnvironment/SemanticContext/TargetBuildView | runtime 一次装配，同一请求粗粒度保活，不每节点 shared_ptr |
| GuestStateLayout/GuestStateBindings 分离 | frontend 只知道逻辑 bytes/别名；物理 bindings 由 backend 装配 |
| typed builder、FlagsIntent、共享 rules | 已有部分append前纯选择，也有AfterAppend/提交后简化；分别计真实出生、解释和改写，不统称出生前完成 |
| Inst/Arg、Store/order/uses | 单一语义输入，不再引入 HIRValue/HIRUse 或公共 MIR |
| 原 Allocation/质量策略 | 两个真实范围、唯一Writer与最终质量合同保留；跨目标描述/晚声明/绑定的算法可以重写，不固定旧表和遍历 |

`LoadMemoryExtend/LoadMemoryExtendTSO` 已存在，保持真实 access/result/extend；不以新名字发起全 ISA 迁移。已有 full-GPR deferred、部分完整 flags 延迟及 Function AdvancePC coalescing 都先从新收益中扣除。

## 3. 一次目标绑定与三种分支

| 输入 | build | 后续 |
| --- | --- | --- |
| Known＋early＋单项支持 | 公共纯规则选择合法语义形态，一次提交 | 目标为必要residual/完整SSA后，具消费权且覆盖完整者进入backend virtual、原范围machine RA及PhysicalReady；支持域按同Store接管；消费前合法拒绝/public/retained/Unknown仍保语义IR至Flush，实际块快照不等于完整覆盖 |
| Known＋EarlyOff/单项 unsupported | 同 SemanticContext 的 generic 构造，后端仍 Known | 必要合法化/优化后交给该 backend；不伪装 Unknown |
| Unknown | 完整 generic、effect、异常顺序；允许不依赖 host 的纯折叠 | 解释器/实际支持后端消费，或显式绑定后的受控合法化；不等到编码中途才发现不支持 |

Unknown→Known 如需改语义 IR，发生在可变阶段，闭合 uses/flags/DCE 反馈后最终编号/seal。已 seal 的 public/import 输入使用支持的 generic 消费者或明确受控重构，不能偷改。

EarlyOff只关闭当前环境的BuilderTarget/BuilderBindings；target身份仍可为Known，FlagsPolicy独立从target取得，不能把它清成Unknown。Unknown则确实没有host target能力。当前接口见`compile/target.h`的三个独立查询。

环境包括 guest 模式、有效 host feature、ABI、TSO/内存政策和 helper 能力。裸 policy 地址相同不等于配置等价；IR lease 也不会自动保活 layout/binding，不能借 decode 栈或已销毁 AddressSpace。

## 4. 前后端解耦

| 层 | 知道/决定 | 不承担 |
| --- | --- | --- |
| frontend | guest opcode/输入、原 PC、flags 输出意图、access/result 和异常次序 | ARM64/VIXL、物理 home、scratch、后端 helper 地址 |
| builder/rules | 公共 typed IR、当前受控构造事实、纯能力 | live/active、分配/transport、编码 proof |
| backend capability | 该语义形态是否支持、feature/ABI 和纯 carrier 规则 | 执行 guest/helper、修改 frontend、拥有第二值图 |
| residual/Facts | 未来用途、深 facts、CFG/state/flags 和真实反馈 | 猜最终物理寄存器/故障 host offset |
| 公共Allocation/Writer | 两种范围、真实位置/写入/transport合同 | ARM64 form、机器码或重新decode |
| backend native/select/state | 同Store机器程序、实际uses/defs/effects、形态准入、架构版本/恢复及最终operand | 另写guest ISA语义、完整Selection影子、沿旧semantic ends后补form |
| emitter/runtime | 编码状态、host offsets、持久发布 | 沿旧逻辑图重推分配或保临时 IR 借用 |

能力接口保持窄且纯，例如某 access/result/extend 形态是否可直接表示、某 carrier 的定义语义。不提供“任意 callback 接管 frontend”的万能 factory，也不传 ARM64 form ID 或物理寄存器号。

## 5. 纯选择、直接构造、一次提交

```text
同一 decoded 输入和当前 guest 语义
  → 纯 rules/capability 选择
  → 命中：已有值 / 最终合法 IR
  → miss：一次 generic 构造
  → 原 guest-instruction 输出、flags、observer 与边界收尾
```

纯选择不 append、不访存/调用 helper、不产生需要回滚的节点或用途。完整 Imm/type-view、实际访存宽度和 EA 保留；U16 load 经 U8 view 不能偷换 U8 访问。Params、Lambda、重复输入及 terminal roots 按原 occurrence 维护。

GuestInstructionScope 不是 rollback transaction。失败不能重放 generic 访存/helper；不可逆 append 前就确定分支。decode/构造异常由请求 owner 放弃未发布部分结果，不允许后续重试复用被未执行后缀证明删除的前缀。

Known可通过现CompileEnvironment/TargetBuildView提前做target-aware语义规范化，产物仍是公共可编辑Inst。NativeOpcode/Form、具体register/role realization与NativeInst寿命归backend；原semantic读者未全迁完前，decode直接产native对象仅是未设计完成的后续方向，不能算本轮收益。不在sealed Inst偷写字段，也不为早建另存一张全N target descriptor。

只对真实已完成且同有效域的同义浅匹配免后续工作。未来 use、深位分析和 rewrite 后的新机会不是 early 已处理；不添加一个“birth 完成”位跳过所有首次 residual。

进一步前移优化必须说明什么节点/输入边不再出生、哪个后续同义解释真实退出，以及miss/失败成本。已有规则命中统计不代替完整compile；不为了制造命中覆盖扩大窗口、白名单或降低恢复/flags义务。若真正主项是幸存节点的反复求解，就改对应pass/后端算法，而非继续把规则往decode堆。

### 5.1 每个语义族提交净退出清单

M1以完整语义族为单位说明：旧中间节点/边、实际出生后的参数与counts/uses维护、首次优化/分析需求、目标描述和最终读取；新路径分别取消、保留或新增哪些工作。代码命中率、文件数和早期API调用数不能代替旧耗时覆盖率。

| 路径 | 要求 |
| --- | --- |
| Known＋支持 | 不依赖位置的形态、carrier/flags/访存语义尽早落实；少出生的节点/边和退出的后续服务可逐项指出 |
| Known＋miss/EarlyOff | 一次generic构造，保原target支持和必要合法化；不能先完整生成专用结果再重做generic |
| Unknown | 公共语义仍完整，实际支持的消费者/明确绑定路径可执行；不借ARM64形态假装通用 |
| raw/manual/编辑后的IR | 受检入口、真实写口和失效闭合；不能拿出生资格替代未来输入状态 |

共同的公共字段表示可以按[IR细则5.4.1](ir-representation-redesign-2026-10-28.md#541-公共语义形态的字段与写口合同)替代旧参数解释，但须贯通构造、编辑、用途、Facts与target；不缓存另一份N字段、不新增ARM64公共form。保持现布局能满足合同则无需迁格式。这与已经完成的native typed-current codec是不同服务，后者不重复立项。

### 5.2 已撤回：以单独紧凑移位承担构造提速

78f在Known＋early＋canonical分支直接提交ScalarShift，Unknown/EarlyOff/非canonical沿原展开，保Src/Dst/fault与既有立即数0捷径。它减少命中域中间节点并通过必要语义，但完整方向没有大幅收益，新增纯能力、公共opcode/profile、消费者及retained wire整包撤回；不作为当前能力表或下一实现要求。

这条反馈说明早期选择应落实到真实生产者，但单族少节点不足以证明整线成本覆盖。M1仍需主要流量的构造/首次服务替代，M2/M3仍处理实际位置/state/选择的大链；不铺全ISA特殊规则或扩大窗口。[完整结论](spill-and-compile-root-cause-plan-2026-10-28.md#5211-当前完整候选shlshrsar直接公共语义)。

### 5.3 下一向量迁移的前置改进：PSHUFD immediate保形

该前置已在当前`decoder_internal.h::VecShuffle32Lowered`实现：Known＋early且公共纯能力接受控制时，直接提交公共VecShuffle32 immediate；未接受保原常量cache/indexed，Unknown/EarlyOff保原真实generic行为。不再将这项源码改动列为未来M1收益，actual direct form的FPR/资源/retry和最终质量仍归后端。

共享typed scalar也已在真实guest scope先形成最终operand，再append，dde原guest commit194断言通过；它少参数往返与普通commit解释，**不减少N**。32B PendingValue继续保完整FlagsIntent，Original处理同guest宽度变化/foreign/raw continuation。不能把这个“参数已完成”合同泛化为后端消费权。

INC/DEC＋普通NOT少literal与typed空事件两个包已经合入共享默认构造。be原decode flags1058、typed194/cancel17/helper890等独立例通过；前者少两类局部literal，后者少空分派而不减N。事件投影取唯一semantic effects，静态opcode与Known backend能力各有边界；Unknown/EarlyOff、原signature/owner/uses、真实observer与未完fold保持。组合ELF.text −50128B不作单包归因，不授compile时间。

## 6. 构造与 pass 各自承担什么

| 域 | decode/build 同时完成 | pass/物理阶段完成 |
| --- | --- | --- |
| 解析/PC | 一次 parse、原 bytes/extent、真实指令提交边界 | host layout、entry/fault/link offsets |
| 纯值/宽度 | 常量、恒等、浅 shape、已知合法 immediate/carrier | 深值/位传播、未来 use/type-view、CSE 与反馈 |
| memory/EA/extend | 真实访问/结果/扩展、完整 EA、已知 TSO 语义形态 | alias/writeback、reload/clobber/scratch、atomic 和实际故障点 |
| guest state | byte/partial/full forwarding、已知覆盖、现有 deferred | observer/fault-aware DSE/sink、join/loop、实际恢复位置 |
| flags | 输出意图、安全局部延迟、有限纯需求 | CFG live、carry/partial、CF/PF/AF/NZCV 和 fault canonicalization |
| uses/控制 | counts/effects、原 terminal/root/retirement | 反向用途、CFG/SSA/live、hoist/entry 及最终次序 |

### 6.1 C2 的一次解析与输出需求合同

当前 decoder 拥有最多 32 条实际 _DInst、原 PC 和 owned bytes；scalar_demand 共用真实 family 查询，逆向计算 GPR byte 与完整 flags 需求，原顺序 lowering。窗口退出 all-live，flags All/None。初版 0F 负门拒绝 MOVZX/MOVSX 窗口，不能据 family 枚举声称已命中。

guest64、Known、early_build 与公共 scalar_output_demand 同时满足才启用；其它继续 streaming。memory/atomic/carry/partial/helper/control、短 fetch/解析错误等先 drain 再原路径；已解析 barrier 不二次 parse。高字节读取与32位写清高半语义保持。

原 fault flags/direct backedge 消费者已修补并通过最少检查，完整组合四点没有大幅降时；**窗口、guard、opcode、marker 扩张停止**。保留当前服务，不把普通早期能力扩张安排成新的主路线。

## 7. 整体流程与最后一次可变闭合

```text
一次环境绑定 → target-aware公共语义build → 必要residual/反馈/hoist
  → 最后编号/seal、完整SSA Finish和最后semantic图匹配
  → 原CFG/terminal/entry/hoist转交，关闭全部semantic读者
  → 同Store BackendVirtual：一次规范化form＋真实operand/effects
  → 稳定form/use/def/根/birth合同；Map相关候选先选完整待履约合同
  → 中立program view → Block局部 / Function跨块machine live与RA
  → Map相关变更维护受影响interval/owner，或受界回滚/重建
  → 原state/entry transfer、剩余兑现、独立fixed/width/observer等check
  → 同operand物理化 → PhysicalReady
  → 现同源kernel，true/false共读程序，仅重建发码context/offset/动态状态
  → 最终Flush、自持metadata发布、真实资源/lease结束
```

这是同Store服务合同。原支持域默认已接actual输入与原两scope RA/state/独立proof，退出旧Describe/Complete同义发现与每emit binder；source miss/public/retained等保语义分支，完整负载native覆盖率尚未建立。第75共访、第76单族移位、第77最终核验及第78实际位置整包均在必要语义/整体方向后无大幅收益撤回，不重新引入其defer/B ranges、owner/source缓存或新公共opcode。

完整SSA Finish仍读nonlocal定义/分支；不能首次访问完一条就销毁Inst。最后semantic读者包含未来uses/width/flags图匹配、late GPR/FPR/spill、terminal/entry/call-return/hoist及runtime真实extent。转换前转交真实B/unit元数据，关闭图后不回Inst/Users，不保第二terminal/operand/CFG图。

family同源operand/effect schema直接提交原公共约束、实际machine reads/defs和后续state效果。公共RA只读值类别与中立fixed/tie/clobber/scratch合同，不认识ARM64 form；继续原Writer/quality policy及Block/Function范围。旧semantic liveness只在完全等价时复用；fold、换源、早写改变实际生存，就维护/重建原allocation范围数据，不能分配后再补。

下一组先在后端闭合实际machine schema，而不是让frontend承担机器位置证明：Div128/Cpuid的secondary无body、结果在producer点联合定义，保持真实output link、birth块与resolver point；GetResult/GetFlags按自身真实编码及numeric/implicit state读取分别处理。所有这些合同先于machine live/interval/pressure准备，仍保原同块pair准入和logical closeout。新core拒绝已提交且仍依赖语义的政策输入及缺失/区间非法的scratch表；目标粗准入另须识别尚未提交的width/flags/observer等需求，并给出真实rich scratch预算，不能靠core自动检测。简单fixture的零scratch不成为生产默认值。详见[主方案5.3.12](spill-and-compile-root-cause-plan-2026-10-28.md#5312-下一相依组先闭合机器合同再替换默认链)。

writeback、direct narrow、forward或NZCV共用中，稳定的use/def/birth变化先于首machine live；真正依赖Map的变更须在受影响interval/owner开放前以原事务维护完整合同，或受界回滚/重建。机器早写不等于guest版本早提交；fault capture和observer顺序保留。候选不算coverage，Disabled与无body仍需Tick/closeout的Covered不同。仅资源合同不变的局部编码细节可以留在kernel。

具体还须在受影响interval/owner开放前闭合：当前attempt先选明确待履约机器合同并据此准备live；候选变更同步live/fixed/tie/clobber/queue/pressure及依赖state。已收集区间不能因“尚未emit”而继续复用；做不到局部更新就回滚或结束attempt，换完整合同受界重建，禁止只改Map/recipe、沿旧semantic ends或枚举跨cohort组合。已知后端并不能免除此依赖。

原state保raw alias/guest version、宽度、copy/preservation、fault及真实P输出。Function entry-width与form/transfer成环时，需真实共同合同及有界求解，或先定form再原meet；不能乐观union或非单调反复全域重跑。原CFG、外部/缺失entry、loop与单块Function范围不变，局部发现不枚举他块成员但真实跨块义务不可删。

selected numeric输入不能代替logical MEM use/death。必要raw occurrence/multiplicity/terminal closeout交入同程序的真实transport事件，MEM-only descriptor保token/count/reload；不得保完整旧ordinary operands/Users影子。更换pending-spill语义必须连forward/reload/observer算法一起迁。

retry期间同一virtual程序稳定、真实form编辑可稀疏回滚，reset原Allocation后按原有限政策重试，不回raw IR重新lower。独立checker读opcode/role/effects和实际结果；独立性不要求逐query全scope重找关系。可由原owner生产有完整域、真实消费者、失效与最后读者的唯一必要索引，删除对应重复发现；禁止关系影子与Passed自证，初始化/访问/结束照计。容量与可失败检查完成后物理化，Ready不是免验证证书；true/false共读，不提前丢弃仍需事件。

Known不授予消费权。完整fresh实际组由原ExecutionAccess持真实guard；public/retained/raw/foreign或不支持域事前选原checked路线，不先付完整新链再付完整旧链。EarlyOff仍可能Known并在后端合法lower；Unknown有真实generic消费者或显式受控绑定，不把它降成只会导出IR。生命周期与失败协议见[IR合同](ir-representation-redesign-2026-10-28.md#4-生命周期与最后消费者)。

首次Facts/consumer solver、已有Known flags/EA/deferred/birth能力继续计真实成本，不能重复认领收益。M1仍须减少大量实际构造/首次解释，M4随旧数据退出减少触达；工作量与停止条件统一见主方案5.8/7.2，不新增frontend万能callback或逐节点测量框架。

### 7.1 最新初始需求不会倒灌frontend

decode可以结合已知后端的**公共纯能力**确定立即数/地址与合法公共语义形态、FlagsIntent等当前信息；whole-unit future flags、pair唯一性、跨块FPR/observer与实际分配需求依赖未来，仍在必要pass/最后semantic读者闭合。不能为了提前建native而在decode猜定这些结果。

最后semantic阶段把whole-unit allocation flags与Function emission-local flags分别稀疏转交；之后的native initial入口借同一ProgramLiveness，不再回Users/FlagsFor或重新Prepare。公共RA只消费实际birth/use/def与fixed/tie/scratch等中立约束，完整scalar-insert/resident/width/eviction选项从原请求显式带入，不以默认关闭代替迁移。

Covered Get的availability definition、真实producer的硬件写、Covered Set的logical guest commit还须分别表达。Get/Set的Effects访问mask不能作为完整硬件读写证明；最终actual scratch读取实际form输出、当前绑定与真实preservation/reload，不按所有MapMEM定义或raw源盲计。

guest-home partial读写、分配池净clobber、helper原始ABI/capture是三个不同合同；preserve-all helper保存/恢复的raw ABI破坏不能合入净RA mask。C3已在支持域由真实Get/Set与partial访问生产effects和唯一稀疏access/observer序列；缺失或未知形态仍拒绝，零不是通用“无需求”。这些都归backend的同源form/约束，不向frontend暴露home、NZCV或ARM form。

C6上述输入、静态B、typed GPR/MEM发表、scalar schema、原Storage准备与单一入口宽度服务保原限定资格。C7/3bdd060c的数值flags、有限D、控制引用与普通内存已按各轮完成受影响检查，不转借前版其它阳性。支持域默认gate已true，完整粗准入仍在semantic关闭前选择合法路径；initial Complete/CoreAllocated不等于Ready，Known/EarlyOff/Unknown及单块Function范围不变。

C4的公共分配器只借actual consumer输入和target提供的纯读取规则，不认识ARM64 opcode/form，也不能把opaque native身份转回Inst。规则仍由原backend/state共享；最终kernel只读已经兑现的role/location，不在读取时补推selected/inferred。该内部借用不是接管frontend的万能callback，也不改变Known/EarlyOff/Unknown。

**backend可提前决定的图变化不必等到late RA。** 已合Get32→To64→publication首域在首次live/actual输入出生前改真实publication源和64位clear-high commit，退没有其它义务bridge的machine use/def，随后直接生产实际quality/hint/preferences；已不再同步inactive旧行或journal尚未出生的派生输入。raw header/次数/Original/Tick保留，无hint合法；wide-only/未限定hint、Get/Wide hard fixed/read/tie、lease扩张、Get64/extract与额外bridge义务仍在source/actual编辑前精确拒绝。位置/LowView/copy仍由原backend owner兑现，frontend不知W/home，不复制target IR或扩大白名单。

可提前决定的是source/def合同，真实capture及零高位仍需实际位置和body证明。若Get32有合法retained capture、目的区别于旧guest source home且已独立证明零高位，publication可保64位clear-high commit与真实Implicit source，不能丢写入语义。否则publication保真实W copy，原source须独立可用；observer可能要求同时保Get capture。W-copy与Get省读分别受检，不能为了Covered夹具改政策。所有body变化在受影响B最后fold/install前结束，再selected，归后端原owner。

SelectZero仍由decode提交公共语义和宽度；backend纯schema在首次live前核Generic W primary、三合法输入及共享成员，沿原cohort交同site/birth宽视图，公共canonical白名单不变。Capture需求现在按actual definitions/uses与原硬窗口产生，source保真实raw资格；observer不替代W证明。typed普通发表已共用control/adoption规则并支持Whole MEM及已建立reload，fused/selected等仍缺域；均为backend义务，不由decode猜home、改Map或制造AdoptsSpill。

六类late scalar已由原family向initial与最终位置/资源读者交严格schema，核实际width、extension/W tie及先读后写alias，不把修正倒推给decode、假Generic或关features。最终form访问已移入原SubmitNative，在最后fold/selected前；`state_changed || (forms_changed && native_reused)`保无state需求早退。源域未迁在semantic关闭前精确分流，最终仍保原合法质量能力。

原Storage全组preclose/移交/原scope准备保有限Block/完整Function、真实MEM压力与width-only Function各轮资格；dd0阶段原支持域默认true、所列原例通过；9e95交接/宽度及新增fixture在原owner2809中通过，32e6临时形态诊断已观察实际native块；完整负载覆盖尚无有效结果。全部semantic读者先闭合，关闭后不回Inst。preclose semantic入口width solve/30B查询已删，residence/metadata共借native解；transport整批Stage前后各自重推证据，非逐D_edit全域分类。原定义/CFG、高零、preservation请求×actions、profile/双resolve和真实两时期journal仍计费，不能重复立项已删服务或删必要恢复记录。

已知后端允许提前优化公共语义，并不使未来信息提前可得。GPR/width的必要raw身份/计数在最后semantic边界保真，质量拓扑改由actual程序直产，FPR/scalar-insert/resident/width/eviction选项从真实请求带入；backend承担实际形态与位置。Core分配通过不代表全部质量选择已改写machine operands；这部分由原后端调度/Writer兑现并维护machine live，不能把修正倒推到decode，也不能先跑全旧Complete再转换。

### 7.2 flags可在decode确定什么，后端还必须决定什么

已知后端的纯能力可帮助decode直接提交合法公共形态、立即数/EA及FlagsIntent；guest指令边界已有deferred flags和fault/observer flush继续复用。FlagsIntent::{None,Declared,Unknown}不等于未来liveness，不能因当前没有use将Unknown改None，更不能提前猜PSTATE/home或把token/branch form倒灌frontend。

C7最后semantic仍消费原middle FlagFlow/UseFacts，分别交whole-Function RA demand、本B emission demand与packed BranchOnly marker OR，不另建flags图/固定点；marker存在不等于全部pseudo均branch-only，intrinsic输出限制另核。原integer/consumer matcher在semantic结束前给稳定source/def合同；Map相关token/preserved suffix在后端原B最后位置访问兑现。前端不决定真实数值def是否退役，不替代actual use/REF/terminal、fixed/joint或NZCV窗口证明。

后端必须先使程序与kernel一致：CMP无数值def，AddCarry保真实result，SetCarry/OF读arg0；ConstantFlagsOnly及matched local-condition链只在完整条件成立时退data inputs/def，并同步有限D质量输入。数值组已接完整accepted dead closure及原实际图/proof，合法scalar BOOL宽度不能一律收窄为U8。控制key与机器结果分别判定：LocalParity真temp和CondSet等数值fallback保留，继续闭合原lazy状态选择，不按Local事件整族退def。不能靠全Generic、假implicit或永久CSET+CBZ保虚假支持域，也不让decode知道这些ARM形态。

主树已接窄Label-kind arg0/Current LocalConditionKey和PhysicalIf末尾local_condition，保完整身份/type/view；first/mask1 recipe交key，actual数据与raw退休仍借原程序/树。独立reader核真实NZCV窗口：Condition可能reload，Switch命中子树前则恢复保存的物理NZCV。已合CondSetControl选无数据形态；同一实现的普通native Generic必须实际写数据；hard撤回Generic且已有同ID合法raw key时真CSET该condition，不读未发表x26，数值读口不隐式拿map key。这些Generic修正在395获所列限定检查。未知后端和EarlyOff仍走公共语义与真实合法fallback；这些机器决定不进入frontend。

ClearCompound在final form之前借唯一native入口width解、SSA基值和原CFG/terminal谓词；没有相关依赖不强制求解，最终metadata共用。全部virtual时用同源effective demand产原resource后，pseudo word才转handle；静态/prefix检查禁把handle读作需求。Save Logical不自动获DISCARD，原raw计数与flags-only MEM政策保留，C6 U32 bridge Label0不因同shape的flags covered失效。

数值flags、精确always-key/Carry、Goto/Label/branch/backedge/PFAF与metadata沿原真实接口交接；支持域默认路由与未覆盖域消费前typed门分开。选择接管时退休旧消费义务，窄legacy raw和publication角色合同仍归原写口，不把历史失败列作新工作。形态与actual输入见[主方案5.3.12.15](spill-and-compile-root-cause-plan-2026-10-28.md#native-actual-input-service)，资格只见主方案7.1。

**frontend提交条件语义，不决定其机器交付方式。** 同一CondSet需求在后端可能落实为真实数值result、local key，或由DirectSelect在自己的点读取条件。后端成功接管时同步退休旧LocalCondition属性/event，原raw唯一归属和最终PSTATE/observer窗口仍须独立核；额外numeric/terminal用途不可因相同shape被丢弃。这轮实际接管修复归原select_flags，不在decode加“已处理”位或ARM64 form，见[交接合同](spill-and-compile-root-cause-plan-2026-10-28.md#native-handoff-obligations)。

### 7.3 普通TSO/Extend：提前保语义，后端交实际控制

decode可通过公共纯能力直接提交完整EA、访问字节数、sign/zero extension、结果宽度与TSO语义，Known/EarlyOff/Unknown均有合法公共表示；它不产生PhysicalMemoryControl或ARM64 form。真实机器控制字在后端转换同槽产生，checked/native共享一份规则。

主树普通TSO/Extend已将原Cell3的4bit LoadExtend descriptor就地转为实际控制字；state、flags与核验等逻辑读者统一借同源逆投影，不另存raw字段。原MayFault带来的guest FlagAccess read All保持，与host NZCV clobber分开；RCpc/unaligned/fault/barrier、extend32清高、backing/reload与MEM forwarding保真。六普通族已有真实owner/Ready限定检查，fused/复杂Map选择另闭；不是仅删coarse gate，更不授完整guest/性能资格。见[主方案5.3.12.8](spill-and-compile-root-cause-plan-2026-10-28.md#native-memory-next-service)。

### 7.4 本地控制的早定边界与后端合流

decode可提交公共Goto/NotGoto/BindLabel、原guest flags行为、真实Save执行位置与观察边界，保合法token/type/view；已知后端允许早定纯形态能力。真正的raw key、NZCV/pending、home/copy/width合流、窄branch临时归一化与ZeroTarget质量条件仍在后端。原FlagFlow/SSA Finish不规范化physical flags；不能因后端已知而把只在fallthrough执行的legacy Save提前到分支前producer。intrinsic producer发表另保原合同。

本地相依组沿token/GPR实际路径、Goto真实copy快照/Bind交集、唯一Function width解与flags入边状态交接，资格见主方案7.1。SaveResult在原Save点读result/必要operands，不借当时CF；稀疏actual demand与raw退休分开。新增use/live必须在原allocation owner打开前重准入fixed，Derived来源显式标记，已合Explicit扩域及其精确capture残余都须遵守原合同，未验证域不可略过。Get按定义点捕获，foreign通知沿原队列，即使来源exit不变也传播；getter窄事实与真实W写分别证明。MayWriteValue/dynamic只作负向clobber，L快照/key birth也不自证跨join。具体合同见[主方案5.3.12.9](spill-and-compile-root-cause-plan-2026-10-28.md#native-local-control-service)。

**早定形态不等于提前破坏可恢复基线。** preclose只选metadata，唯一Native转换后、首次live前Apply并稀疏记录baseline；Extra只append，保可恢复prefix。ZeroTarget实际换源和原Storage恢复消费者已接：精确late拒绝先结束借用/回滚，保唯一AllocationState，清旧fixed-ready，恢复fixed/Get32/flags/body/terminal，再fresh static/live/native Describe/原controller最多整scope一次；FPR独立预算不重置。整批general不自证最终质量，Invalid/Unsupported不能借预算。Known不预言晚Map；Unknown/EarlyOff保合法语义，不改变分配范围、不向frontend暴露ARM form/home。详见[5.3.12.10](spill-and-compile-root-cause-plan-2026-10-28.md#native-cohort-owner-rebuild)。

固定FEX的早定优势来自实际选择和physical operand交付；它先decode再dispatcher/IREmitter建IR，DFE/CFG/压力仍真实求解，不证明“decode能做完所有分析”。SVM让纯目标能力在出生前减少generic展开，保未来use/路径/位置相关工作给原pass/后端；目前输出直连已做，后续必须净删重复构造、state/selection/约束解释。新增fixed资格、Save压力、L/Q_get与失败重建都入账；最终独立合同保留，跨文件共用规则不是性能上限证明。

### 7.5 已知后端不能提前假定最终home、capture或terminal读点

Known构造可提前交公共SetLocation/call/return语义、完整64位definition/view、原PC/CFG及flags/异常意图，不能承诺某个physical home或把terminal读取提前到marker。backend在消费semantic前交原B动态location/resume root，后续machine live、MEM backing和Freeze按真实使用时点闭合；frontend不包含RSB/L1物理策略、home、capture或ARM form。当前RSB的动态/noRSB/L1 Covered是原语义，没有新增动态Push能力；精确支持域见[主方案5.3.12.12](spill-and-compile-root-cause-plan-2026-10-28.md#native-control-location-domain)。

静态guest bank来自真实CompileEnvironment/layout/binding，不能由Known、一个保留bit或test里的Get22猜测已经开启原scalar affinity。原ZeroTarget正例已用真实完整bank，另保私有Get32/point0合法回退。f3c0 U32/EQ/no-overwrite前缀已过，根因是Freeze后reader仍读已结束virtual event；f3d0 policy0不解释它。后续overwrite暴露真实home写未进RA clobber和晚deadwrite请求检测，修复资格分开；不改全局feature、不手填Map、不降低home命中合同。实际终态scratch在后端末次独立检查中覆写原backing，直接交资源消费者，不把机器profile或Passed证明倒灌builder。

Unknown/EarlyOff继续有完整generic行为和受控后绑定；解释器与其他backend不依赖ARM metadata。FEX启发是把已知、纯且稳定的形态提前确定，让后端RA/实际输出承担自己的合同；它不证明guest定义点、真实机器写点、capture或最终home可在decode一并确定。fixed实际use扩域已由原后端/公共Writer消费，dde原fixed457已检查GPR/U64与真实压力U32/MEM双capture；中立接口仅传真实use/def/location约束，不让frontend依赖fixed页、ARM form或最终home。可变fixed结果的stable页归原Allocation，slot/K仅Release重置；它是后端结果承载，不是decode阶段新事实。

**Known不能预言旧值已死或新结果必定同home。** 同点GPR交接由后端在原RA/独立proof按完整scope的actual writer/use/terminal及P前硬窗口确认；B-local截断不能代替Function lease。decode继续只表达完整公共语义及合法能力，fixture通过原ABI bank/pool构造真实分配，不把测试预pin造成的分配变化当生产失败。

### 7.6 可提前选择的memory形态与必须晚定的物理义务

Known可以经公共纯能力保留合法EA、立即数、access/extend和lane视图需求；真正跨类位置、Pre/Post固定home写回、capture/fault/transport、RCpc alignment/backedge条件归后端实际状态。EarlyOff/Unknown继续完整generic语义与受控后绑定，ARM form/control不进入decoder。

当前stage01～03在首live前交相邻EA/Zero/narrow/lane store、整数partial lane/清高、LDR D/S实际写，所含原owner路径在9b/33da整例通过。canonical definition与有效边Type/RawView保持分离，extension精确目标宽度和RA前actual clobber由后端交付。

stage04 Pre/Post、full-X fixed reader/version及Definition JIT保持，WB base按actual Cell1、窄Store按actual Cell3读，地址matcher仅raw，旧一点桥已退。stage06最终位置/P事实已在backend原两个晚visit替代stage05早SSA；dd0所列原例通过，unknown Generic load的隐式0 fixture按真实Output表示迁移，未放松产品guard。EA/立即数可早选，真实home/capture/spill/guard须由最终位置与独立reader决定，decoder不承诺这些结果。

static guest identity、原IR定义点、机器数据写点和最终home不能混同。retired raw header按phase保有效信息，晚DSE若无实际body/use编辑便不能声称已经删除；baseline完整写保留不等于全域DSE/最终QoR等价。旧FP1的SetHostFPR(U64)、VecFCmpMask或indexed mask只帮助查真实形态，不给当前source授覆盖率。

### 7.7 实际输入直接生产保持后端边界

本组已经实现：source保公共语义/SSA/CFG和已知能力准入，W-view/Get32用纯source/actual-form规则摆脱旧GPR行；同Store转换收C/P，actual形态和use/def成立后，原Descriptor直接生产原分配输入。对应native语义预生产、flags全域修订、semantic→actual绑定及WB一点桥已退；necessary semantic live、实际state和最终独立合同仍保留。

公共AllocationInputs/program只表达中立value/use/def/REF/terminal与固定需求，ResidenceUse.actual_kind是可选的公共kind，raw时为空；ARM64 form/source资格与独立证明留backend。Known/EarlyOff/Unknown都不授decoder fixed/pinned/lease/Ready权力。Block局部和Function完整CFG保持，单guest块Function也不降级。

public/retained、Unknown/RV64及无consume权入口保原服务；支持域全部成员先Preclose，仅消费前StatePolicyRequired/UnsupportedForm可走尚未消费的原链；InvalidProgram及Complete＋nullopt结束整组，不回退。source_result=Complete不代表actual initial完成；Take之后不回死Inst，只有原精确metadata同owner恢复，实际失败全计完整compile。

最终RCpc对齐也留在backend：stage06已在两个既有晚resource循环按真实GPR/capture/spill P值生产并独立复算，Get/alias借用最后reader后结束，不把新事实或ARM home放进frontend/公共语义。早stage05 producer/reader已退，两个fold/P读写和初始化仍计；当前所列原例不授全P/MEM组合、完整alignment/质量或normal覆盖。

默认gate已true；dd0四目标及owner2230/NarrowRead125通过，临时trace退役、DecodedBlocks经原Storage单scalar交接已合。所有原target/consume/source门及两类消费前fallback保留；完整负载IsNative/NativeReady覆盖率尚未建立，`CanEmitForExecution`不等于该覆盖，无新完整compile/fresh FEX。详见[直接生产](spill-and-compile-root-cause-plan-2026-10-28.md#native-actual-input-service)、[最终位置](spill-and-compile-root-cause-plan-2026-10-28.md#native-final-location-alignment)与[默认边界](spill-and-compile-root-cause-plan-2026-10-28.md#native-default-cutover)。

### 7.8 decode只能承诺实际可交付的局部flags域

Known允许提前选择合法语义形态，不能让“已保存guest flags”等同“host raw flags仍是该结果”。对窄legacy logical Save，decode撤NZCV承诺、保PF的named-value关系；宽/deferred/FP和其它已证明路径沿原合同。Jcc即使先构成canonical CondSet，也可附真实双后继dead marker；marker不授PSTATE资格，frontend不出现ARM form、位置或backend proof。

需要未来顺序/uses/fault信息的交接由原flags pass完成：required全mask来自实际primary，primary/发表→condition完整窗口透明，包括AdvancePC之后的gap；全部原feature/双后继/observer等条件成立后，原RewriteContext同节点将Save/CondSet同步交BranchOnly/Local。未成立保canonical合法路径，不能仅放宽matcher或把后端拒绝当成功。

后端在既有actual prefix visit独立核incoming mask，再保原consumer窗口；raw key与numeric CSET均覆盖，Carry极性不因mask齐而豁免。普通SaveResult发表后恢复incoming NZCV；BranchOnly SaveResult先写canonical再加载NZCV，Generic/raw BranchOnly则不发表。不能统一恢复canonical覆盖后者正确的raw结果。source资格与final proof是同一合同的生产/独立消费，不再各建一层表。

完整窗口和同节点成功交接继续保原source/pass/独立reader合同，不把后续角色问题归为旧candidate未合。逐源见[文件表5.1.14](middle-pipeline-file-layout-2026-10-28.md#canonical-raw-flags-layout)，当前资格与顺序统一见主方案7.1/7.2。

### 7.9 后端实际kernel属性不扩成decode承诺

普通StoreMemoryTSO保PSTATE与它会fault/观察guest flags是两个属性；前者的后端kernel审计不能授权前端撤去必要canonical发表。逻辑producer的defined/zero按实际S form与emission demand判断，不能由公共opcode推断；VOID consumer角色与W producer宽度也在既有后端descriptor内区分。Known早优化仍只提交公共语义，Unknown/EarlyOff保持真实可执行分支，不外泄ARM form/home/RawPstate。

metadata缺home仍可按精确预算同owner撤回；publication的any home只是后端candidate，不能当Value实际binding或capture证明。原Whole可选miss保真实copy，explicit Value严格；kernel沿既有ScalarX按role消费，pinned不是Value合法性的代号。此类位置/lease/MaterializationId合同不形成frontend补丁、cold标记或统一恢复层。具体资格见主方案7.1；减工按真实normal路径与完整反馈选型，不改变解耦、完整Function或最终两倍门。详见[实际合同](spill-and-compile-root-cause-plan-2026-10-28.md#actual-effects-cold-publication)。

## 8. 对照 FEX 应采用什么

固定 FEX 的具体代码行为见[主方案第3节](spill-and-compile-root-cause-plan-2026-10-28.md#3-对照-fex缩短决策链保留自己的合同)。采用其已知目标早成型、RA直接落实实际位置/输入、backend读取physical args与工作集复用；SVM用同Store接管后稳定物理程序重发，保公共语义及独立Function RA。不是拿FEX块内范围或可选校验边界替换SVM合同。FEX每guest指令仍flush寄存器缓存，不能据此假设任意跨guest延迟状态。

不将 FEX 的块内请求、InlineConstant、校验边界或旧诊断时间当成直接可比的成本。现有证据不支持“SVM 节点多一数量级”；需同时减少真实工作次数和幸存节点每次处理成本。

## 9. 目标身份、旧入口与缓存

| 入口/数据 | 合同 |
| --- | --- |
| normal Block/Function/merged Functions | 同一不可变环境，实际 parent 和原 RA 范围不变 |
| manual/import/PrepareFinalBlock/已 seal | typed/membership/环境和 target 准入，不能假装 controlled decoder |
| Unknown、解释器、RV64 | 真正 generic 消费/显式合法化或受检拒绝，不只验证导出成功 |
| 多 Prepared、重入、shared parent | 实际借用/lease 清楚；不能覆盖活 backing 或借用次数推断消费权 |
| code cache/serialization | 实际语义/格式变化才更新身份；持久结果不借临时 policy/IR/form |

仅文件移动不更改 cache 身份。实际 form/恢复格式变化按原序列化合同更新，避免旧缓存错解释。

## 10. 源码组织与质量要求

公共 `compile/target.h` 和 backend `target.*` 定义纯能力边界；builder/rules提交公共IR；passes/analysis处理未来信息；第79批已存在的`compile/allocate/program.*`给common RA提供中立借用协议、machine live和有限core，backend `native.*`定义原Store中真实程序。目标是constraints缩为ABI/pool与中立适配，同源选择归select，actual transfer归state，独立check归verify，共有metadata与冷/public replay归physical；现constraints builders尚未完成退役。不是每层各一张表/一遍扫描，不新增target pipeline。完整文件名见[结构表](middle-pipeline-file-layout-2026-10-28.md#32-绑定请求和语义提交)。

不新建 forms registry、第二 micro-IR、逐节点 target certificate 或通用试探/回滚层。native/physical schema是backend私有合同，frontend不能引用；Selection不能扩成与Inst并存的全N/E程序。Known直接native birth在当前semantic读者协议下尚不成立，不写成本轮完成条件。

## 11. 实施、成本与停止点

M1必须完整交付少出生的节点/边、幸存节点首次roster/CSE/深解释/state/flags/SSA及反馈的净工作，扣已有能力，列miss/失败及新接口成本；不能只改小DCE/flags，也不恢复已撤无收益Facts共访。M2/M3贯通实际位置/state/绑定的生产者与读者；M4同时交付真实live/capacity、初始化/复制/访问/清理及最后lease的减工，不只调缓存或移动结束。三者不能以“后端先全部完成”互相等待。

现有typed kernel和重发能力不重复立项；codec、Block流、Function Word、C2窗口及无收益Facts共访的停止结论保留。可重设真实首次服务，但须退出具体工作，不能只换共访/回调包装。必要归因只回答会改变完整算法的问题。

常态/困难工作表复用现净工作说明，分别列次数、触达量、真实首次服务和重做范围；不会因此新增逐字段探针或测试门。相同big-O不是速度保证，旧耗时覆盖p也不等于节点/文件覆盖。

先形成可实际运行的真实纵向相依包，闭合该组生产者/消费者及两scope合同后做少量完整compile反馈；其余未迁族保前置合法分支并计覆盖/总成本，不等全族齐备。中间成组构建＋最少受影响语义，稳定后集中质量/资源/fresh FEX，不逐helper验QoR。中间允许变化不成为最终退化额度，原6.3/6.4不改，尚无已证≤2×组合。

## 12. 版本与证据

当前代码：[公共 target](../source/runtime/compile/target.h)、[builder](../source/runtime/compile/builder/ir_builder.h)、[guest 收尾](../source/runtime/compile/builder/instruction.cpp)、[state](../source/runtime/compile/builder/guest_state.cpp)、[纯 rules](../source/runtime/compile/rules/value_rules.cpp)、[ARM64 能力](../source/runtime/backend/arm64/compile/target.cpp)、[语义 pipeline](../source/runtime/compile/pipeline.cpp)。

8cda2889为当前源码检查点：A产品已合，translator与swift_ir_test两owning目标构建成功；三个原case为2过1失败、3714断言中3713通过。public owner和fixed-suffix整例通过；新增NarrowRead负例在绑定时继承旧源专属placement而失败，尚未到stale-count检查，不记为产品回退或全绿。外部02只修该fixture，未应用、构建或执行；8cda没有新的ordinary计时。

e0d6仍为最新普通完整四点：Block 2066.780ms、Function 1099.290ms，各自fresh固定FEX为46.624/47.297ms，约44.33×/23.24×。均CRC通过，Function原失败/Block重解码fallback已计入总时间；这是开发方向，不授最终质量、完整生命周期或七对采用资格，ordinary实际NativeReady覆盖仍unobserved。

3625的独立Block＋完整Function归因已闭合：native GPR proof分别695.636/103.768ms，占各自native admission约88.77%/89.41%；这是A前诊断版，不是ordinary比值，也不是A全部可省时间。A保留的provenance/window/CFG/fault/capture与RawDirect成本仍在。详细身份、原失败、统计端点及后续结果只以[主方案7.1](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)为单一入口；[归因原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/native-admission-3625-summary-01/summary.json)、[A构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-consumer-read-proof-build-01/build.json)和[A原例](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-consumer-read-proof-minimum-01/execution.json)保持独立资格。本轮仅修文档，goal保持paused，不应用02、不构建或上机。

独立Block构造事务本身不依赖ARM64或Known；Function入口宽度仍属backend。Known/EarlyOff/Unknown、真实两种RA范围、raw/public/shared边界不变。原bad_alloc、CRC/构建失败和撤回保持历史，不补成成功点。第76/77批的必要语义、方向及撤回不等于整M1/M2完成；本次文档安全边界不启动额外构建或设备运行。
