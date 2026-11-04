# 中间流程重构实施记录

**2026-11-02 框架v2：原仓库8cda/Git索引继续冻结；用户已授权在独立工作树解决框架问题。当前候选151211da已实施首次分析解耦与完整raw/site关系，两目标构建及7个受影响case/4066断言通过；其余home/CFG与单次physical交付尚待实现，性能未授信。当前状态见[主方案7.1](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)，执行表见[7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)，算法、检查分类与重做上界见[框架合同第12节](compile-framework-contract-2026-11-02.md#12-五项框架问题的算法决定)。旧细节扩域不恢复。**

以下8cda冻结基线已合A（resident Get按实际consumer做用途独证），两目标构建通过；3个case中2过、1个fixture失败，跟进02修正未应用。尚无A的ordinary完整方向或提速资格，B（RawDirect）未实现。原raw、receipt、checkpoint及失败身份不回填。

最新ordinary仍为e0d6：Block完整Translate 2066.780ms、Function 1099.290ms，分别约为同轮fresh固定FEX的44.33×、23.24×。95bd与3625两轮PROF2归因均已闭合；3625的GPR独证695.636/103.768ms约占各自native admission的89%，不是A独占成本、可省时间或A的实测收益，不替换ordinary分母与端点。

原546行方案已包含native与物理交付；路线纠正须按实际退役、保留、新增工作及覆盖核对净减工，并以完整算法反馈判断继续或改线，不能用结构迁移、旧API退出或构建断言代替完整compile下降。最终不降质量、Block与完整Function各≤2×及Known/EarlyOff/Unknown合同不变。见[净减工与反馈纠正](spill-and-compile-root-cause-plan-2026-10-28.md#compile-net-work-correction)和[审查42](compile-gap-review-2026-10-31.md#performance-route-correction-latest)。

**以下前言、C4/C5及各批正文均为历史记录。** 其中“当前”“最新”“normal未启用/关闭”等描述只属于所列source当时状态，不代表8cda现状。历史主体和既有证据链接保持原字节；当前资格及暂停状态以上述主方案入口为准。

---

**同Store载体、首批machine schema与GPR压力读者已验证，完整normal替代仍待贯通。** 原Store同槽的Semantic → BackendVirtual → PhysicalReady是唯一程序主线；carrier/program/core已接线，完整实际machine合同、质量政策、state/独立check、transport与193个kernel的新程序读口尚未迁完。Known早建仍产公共可编辑语义，frontend不含ARM form/home；不扩dense Selection或新增管理层。实现边界见[主方案5.3.11](spill-and-compile-root-cause-plan-2026-10-28.md#5311-下一完整替代同一store中的后端虚拟与物理程序)，顺序见[7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)。

起点：layout-r7，source `3ef7f002fc18183881661138e51fbe732f3de54b05ec9b62384b7d23288e92c3`。当前实施依据为[主方案](spill-and-compile-root-cause-plan-2026-10-28.md)、[目标绑定](target-aware-middle-pipeline-design-2026-10-28.md)、[IR合同](ir-representation-redesign-2026-10-28.md)及[文件结构](middle-pipeline-file-layout-2026-10-28.md)。原代码/Git/WIP与历史失败保留，未提交或push。

**最新状态（2026-10-31）：C4源码6f2f2dfd / main495bf4c6已完成四Android目标构建及本版10cases/805断言、physical-consumption 164721检查/1注入。** 已检查范围为同合同形态、真实implicit publication、FPR独立proof/原retry、真实resolver/资源handle与NativeReady/退休夹具；另11个native阳性保133d07c7身份。NativeReady生产编码分支已编译，完整预RA cohort、native state/内存transport和normal准备未迁完，新normal仍未启用；无本版CoreMark、compile/QoR/PMU/FEX结果。最终不降质量、Block与Function各≤2×同轮fresh固定FEX仍未完成。[C4检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/integration-02/c4-checkpoint.json)、[下一步](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)。

## 第八十二批：同合同native兑现、独立FPR证明与最终读者接线

C4当前source`6f2f2dfd62f04e6d83156a8ffed3fb4c02c3206039f383811e555d61d9c2dbef`、main`495bf4c6…`，较C3改26文件。实际A类角色/encoding、真实implicit publication、共享checked/native FPR规则及typed错误域、stable-header actual resolver、当前资源/handle/原位Freeze/NativeReady退休已进入代码；生产编码分支按原B/committed链直接借body，body与Tick分开。纯state读规则、opaque copy调用域、Low32明确绑定、canonical MEM key与各raw退休计数同步，未增加程序/结果镜像或controller。

四目标generic Release/O3/ThinLTO/noPGO统一构建源码不变。6461无效BOOL枚举和e8ac旧friend与模板冲突两次build失败原样保留；133d四目标通过后native tag12例11过1失败、738断言737过，失败在新fixture错用semantic inputs入口。原controller的program-scope只读借用口补齐后，6f2f四目标重建；只续失败整合例170断言、原未执行9checked例635断言和physical-consumption164721检查/1注入，均通过并核hash清理自有运行场。11个native阳性仍保133d身份，不求和冒称当前12例全过。

fixture实际检查realization→显式live Prepare→FPR独立proof→逆序checkpoint恢复/Prepare→原retry，以及closed Allocation→真实resolver/资源→逐body handle/Freeze→NativeReady/幂等退休。它不执行新生产emitter、不认证完整GPR晚拒绝rollback、B类cohort或memory forwarding。完整预RA形态/state/entry/fault/transport、normal旧链退出及M1/M4仍未完成；当前无新CoreMark/compile/QoR/PMU/FEX，最终质量/资源及两范围各≤2×fresh固定FEX未通过。未stage/commit/push。

[检查点/产物/必要结果](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/integration-02/c4-checkpoint.json)、[完整源码及相对C3补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/integration-02/qualified-c4/source.json)。补丁reverse apply-check通过，代码/Git/WIP与失败身份保留，最终6.3/6.4原文不改。以下文档修订与第81批保原时态。

## 2026-10-31方案修订：C4交接合同与有限M1结论

用户要求“根据最新结论修改方案文档”。本轮只更新八份文档；C4原在途组件先在安全边界冻结，此后不继续实现、构建或设备运行。源码快照为`1fa59a936a43959f988be7b932a36c7b5aab9595c37d5bfbd75bca0045c14098`，2399输入，较C3改12个源文件；这些是已存在且未构建的WIP，不是新的验证批次。native schema与kernel借用有冻结回执，独立FPR proof和公共copy读口未接完整生产调用，normal仍关闭。

主方案5.3.12.6补齐actual form/cohort、state/copy与角色优先级、独立FPR的路径/错误域、跨prefix稳定源信息、资源handle、raw/canonical寿命及kernel/driver收口；5.2.7记录M1尚无足够依据的大范围scalar算法，只将PSHUFD公共immediate保形列为向量迁移前提。目标目录和逐文件责任同步，沿原owner，不加inputs/preparation/actions/certificates包装。

C3的四目标与8cases47049断言、aea的11个native阳性及所有失败均保持原source/ELF身份；C4不继承。不新增compile/FEX倍率或收益结论。原6.3/6.4逐字冻结，最终质量/资源和独立Block、Function各≤2×同轮fresh固定FEX仍未完成；无stage/commit/push。

[本次源码冻结](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/plan-revision-20261031/source-freeze.json)、[改前文档快照](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/plan-revision-20261031/docs-before.json)、[当前实施主线](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)。下文第81批及更早正文保留当时状态，不把其中“当前”转成C4资格。

## 第八十一批：完整初始需求、原质量分配与raw/region整合

当前source`6f7b18b215cab19cfb1f789e112f591465c9fe0645bd0eb740176136903101b2`，main`1b1e0466229db396dce8d97e1693e72182ceef7f2917a2516b4c7e58c9d1fd79`；相对C2改46文件，完整source与补丁保存。initial/rich scratch/flags/三域寄存器影响、actual birth/fixed、共享live、GPR/FPR/width、原RunAttempts/repair、no-Facts Writer与Original/raw/region读者已接。未stage/commit/push；normal未启、整目标active。

真实构建失败分开保留：f636在program.cpp两处u32窄化失败；391a在semantic terminal point的int→u32初始化失败；修后aea四目标成功。首次native tag12例11过1失败、730/731断言：新增完整质量fixture误要求U64读reuse marker且pool未启用GPR政策。按原checked合同修真实pool/强位置期待并保所有写合并/重试正例，另补GPR policy scratch非法传播，才得到6f7b当前四目标成功。

当前红魔只补跑受影响失败case和原未执行两命令：1native质量/retry70断言、6既有质量/width/region/repair例223断言、1Function width例46756断言，共8cases47049断言。先前11个native阳性保持aea身份，不重跑、也不转写为当前12例全过。两自有namespace均核hash关闭，无活动进程或清理错误；没有CoreMark/lifetime/compile/QoR/PMU/FEX矩阵。

本批未兑现全部actual form/state/独立最终证明、selected/inferred role、MEM count0/pending/Tick、193kernel借用及同Ready重发；旧Describe/Complete/binder仍在，M1/M4也未完成，不认领性能收益。具体后续以主方案5.3.12.6和7.2为准，原6.3/6.4不改。

[检查点/完整源码/补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/c3-checkpoint.json)、[当前构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/build-04/build.json)、[首轮语义失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/minimum-semantics-c3/execution.json)、[当前最少检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/minimum-semantics-c3-continuation-01/execution.json)、[fixture原政策依据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/quality-followup-03/tests/pool-domain/receipt.json)。下面两条“方案修订”保留当时f636和23dc时态，不再代表当前资格。

## 2026-10-31方案修订：C3初始服务已接线，首轮构建失败

根据最新整合与只读review更新八份方案及证据入口。当前source`f636296b1240992ffa9b3da327947028a9f2f17be124c5e700fbfabd8723c727`，2399输入，相对23dc改40文件、相对C2改46文件。initial/flags/实际fixed、GPR/FPR/width、原controller、no-Facts Writer与raw/region已接源码；首轮四owning目标build因program.cpp两处u32窄化初始化exit1，source未变。**无当前新ELF或运行资格，normal关闭，最近已检查基线仍为C2。**

主方案不再把recipe/initial/FPR/controller误列仅声明，新增5.3.12.5当前五项必修与5.3.12.6真正form/state/独立核验/MEM/kernel收口；文件结构逐项落原owner。非法callback不得当scratch短缺重试，静态FPR span在Map前核，checked pool政策不随迁移改变，raw canonical类型与原view/计数分别保真；不改计价或弱化完整ownership正例。

下一先成组修当前错误、统一构建及最少受影响语义，随后继续同一normal替代，不单立inputs/载体性能候选。M1/M4共同减真实工作，保P_pub×D、REF/双resolve/CFG/真实retry成本；完整旧服务退出后一次整体方向，最终集中6.3/6.4。本轮仅改文档，没有修源、再构建、设备或性能点；原目标active、无stage/commit/push，历史门和第80批及更早记录保持原文。

依据：[首轮失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/build-01/build.json)、[冻结清单](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/frozen-build-01.json)、[只读review](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/docs-integration-review-20261031/review-findings.md)、[本轮文档前快照](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/docs-integration-review-20261031/before.json)、[现行执行顺序](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)。下条23dc记录是前轮文档快照，不代表当前producer仍缺实现。

## 2026-10-31方案修订：C3在建接缝不记为新通过批次

以下保留前轮23dc文档快照，当前整合状态见上一条f636记录。

按用户“根据最新结论修改方案文档”，本轮冻结源码后只更新文档。C3 source`23dc79d934e9211d6c36e09991dabd2940b3b9e4a77c6bcf2568cdc304e28ce1`，2398个现存source/build输入，相对最近已检查C2／94cb改20文件；没有新ELF、构建、语义运行、compile方向或FEX结果。原Git/WIP与C2失败/通过保留，未stage/commit/push。

已写入的接缝为actual-birth字段及Writer/location/preservation等消费者、共享machine live查询/借用、native scratch-flags承接/共享规则和FPR消费部分。`select.h`三项recipe字段尚未接齐，完整initial/native scratch/fixed-clobber/FPR入口只声明；`constraints_fpr.cpp`尚未迁，initial安装、core政策准入、post-coalescing、原controller/state/transport/kernel未贯通。**这是未构建的WIP，不能称可构建版本或FPR/fixed完整政策已完成。**

方案将这些断口写为同一整包的相依工作：先在最后semantic阶段交付全局/local flags，生产actual home访问/净RA clobber/raw ABI三域及完整rich scratch；一份machine live供initial/FPR/原attempt借用；真实birth贯穿初始fixed与完整quality/post-publication；保原请求选项、retry与独立同点多输出检查，再贯通state/transport/kernel并使旧Describe/Complete/binder退出。新增publication乘积扫描、fixed-read二分、N状态、双resolve/prefix等成本如实保留，不新增目录/第二图或微包性能候选。

具体决策以[主方案5.3.12](spill-and-compile-root-cause-plan-2026-10-28.md#5312-下一相依组先闭合机器合同再替换默认链)与[执行表7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)为准，逐文件接线见[结构细则5.1.3](middle-pipeline-file-layout-2026-10-28.md#513-最新初始合同的文件接线)。本次同步八份文档，最终6.3/6.4及旧测量身份不改；无大幅compile收益或≤2×已兑现结论。

依据：[文档修订范围与源码身份](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/docs-plan-revision-20261031/before.json)、[backend初始合同复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/backend-initial-constraints-document-review.md)、[fixed-birth组件](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/fixed-residence-birth-component.json)、[FPR在建范围](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/C3/fpr-component/partial-receipt.json)。

## 第八十批：实际pair/pseudo与GPR压力读者接线

本组before为cf79，第一个冻结源265a9c11在统一构建发现新压力读者调用私有ResolveId；只将该调用改为现有公开Mapping的有界REF追链，保循环/越界拒绝，不扩大可见性。修正后source `94cb29e941c4c77eaebd524e6893c59d57af9147400d5adfdf715a01ce1bad76` / main`d9a92ac9…`，相对cf79共11文件，累计相对495共31文件；原失败、补丁与WIP均保存，未stage/commit/push。

| 真实工作 | 本组交付及尚未退出的边界 |
| --- | --- |
| 静态scalar准入 | 原BuildPairResults与新PrepareNativeScalarForms共用AdmitScalarPair；旧normal仍只遍原event范围。新准备在完整semantic关闭前收原稀疏pair/raw-width/form关系，Div128复用原纯matcher早定Native/Helper；旧scratch matcher/完整Describe尚未退役 |
| 唯一定义与实际读者 | producer持secondary唯一非REF输出，secondary无body/无重复def；token经同程序owner查询，birth/B/site/output point一致。GetResult只读arg0，GetFlags读完整flags状态，NativeDiv只数值读low/divisor，排除的原边进入真实LogicalUse |
| 原压力质量算法 | ProgramSite/traits/uses_at借实际producer输入，保原victim cost/next-use、输入保护、真实Map/dirty提交、8预算及用户eviction开关；checked原Index/Inst分支保留。FPR接口只返回完整candidate和重试请求，原controller/repair及完整fixed/width政策仍未迁 |
| 资源与实际成本 | NativeInst仍112B，HasPseudoUses复用header空位且不污染flags；pair/logical有真实稀疏Extra/backing，prefix查找、N范围状态与preflight/freeze双resolve仍计费。logical-only DISCARD/非MEM退休、完整transport和kernel新读口仍待闭合，不放宽拒绝掩盖 |

四Android owning目标在固定Data2缓存、Release O3/ThinLTO无PGO构建通过。红魔bf1359ff无PROF/PMU的最少检查：10native cases/614断言（真实Block/原Function CFG、NativeDiv参数空洞、同点联合输出及实际GPR压力）、7既有pair cases/1439断言、6既有eviction cases/51断言，共23cases2104断言。

执行清单把pair预期误写3而实际标签有7，原命令exit0且1439断言全过，脚本在数量准入停止。保留原失败状态及raw；核源码roster后只补跑尚未执行的eviction，不重复native/pair。两个namespace均核hash关闭，无活动进程/清理错误。新native压力见证为GPR；FPR candidate/forced retry只获构建和源码复核，不冒称运行覆盖。没有重跑lifetime或CoreMark，更无compile方向、QoR、PMU/fresh FEX资格。

依据：[完整检查点/源码/补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/checkpoint.json)、[修正后构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/build-02/build.json)、[原始native/pair运行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/minimum-semantics/execution.json)、[roster修正](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/minimum-semantics/roster-correction.json)、[仅余下eviction](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/minimum-semantics-continuation/execution.json)。

normal仍执行旧Describe→RA→Complete→semantic binder，完整目标未完成。下一主线是同一程序上的完整初始约束、fixed/GPR/FPR/width质量服务、原attempt/repair、actual state/transport及kernel最后读者，按真实支持域成组替换；M1构造/首次服务与M4真实触达同样不能缺。不能以组件/文件/通过用例增加代替大服务退出或≤2×目标。

## 第七十九批：同Store载体与原分配core接线，normal未启用

第78批撤回后的495887ca为before；本批28个source/build文件形成首个相依组，当前source `cf79edbc3392983c2a0958e3b5d41c6a79498ce9e21e80b923d01f3d3434bc28` / main `4bed419c…`。原Git/WIP保留，未stage/commit/push；完整差异与before已保存。**本批是组件与共同core整合，不是完整normal替代或性能候选通过。**

### 实际进入代码的合同

| 范围 | 本批实现 | 尚未完成 |
| --- | --- | --- |
| Store/原stream | 原128B槽内接管112B NativeInst；完整Imm64/view、Params/Lambda/必要扩展；粗phase、原alive位的backend membership、cursor先结束semantic尾部、原stream按committed prefix析构native对象 | 全组生产semantic最后读者尚未全部迁走，normal不提前关闭 |
| CFG/terminal | 完整SSA后TakeIRWithCFG唯一移交原FunctionCFG；原B元数据承接terminal，保consumer view/point/role；单块Function仍为Function | entry/width/observer及所有动态控制读者尚未全部迁移 |
| machine live/core | 中立program借同一程序/CFG，真实defs/uses/REF、terminal/call-return传播；原LinearScan用id/type接有限core，无第二allocator | 原GPR/FPR/fixed/width、pressure eviction/repair、state/transport等完整政策仍待迁。core仅拒绝已提交的语义政策输入，不能自动识别漏提交需求 |
| 兑现/资源 | virtual cells在preflight前保留，真实cell/control物理化；析构与退役有真实owner，墓碑/外来同id拒绝 | 完整target独立check、193kernel新读口与Ready重发未接通；Prepare/Freeze双resolve及临时live/membership成本仍在 |

Params是当前指令转换为唯一NativeCell backing、cursor销毁旧Params、再move准备结果；不是旧vector backing零拷贝。显式scratch表缺失或覆盖/顺序/端点非法时，core在改Map前拒绝；表形态正确不证明预算与opcode语义匹配，简单fixture的零临时合同不能推广到生产。

### 构建、最少语义与失败保留

使用固定Data2缓存统一构建`svm_translator_linux`、`swift_test`、`swift_ir_test`、`swift_hir_lifetime_test`，Release O3/ThinLTO、无PGO。build-04在cf79源码前后身份一致，四目标通过。红魔bf1359ff三条最少语义命令、无PROF/PMU：

- `[native-program]`：6 cases / 293 assertions，覆盖真实Store转交、复杂资源、失败前缀、换源machine live、alias/逻辑退休分离、terminal per-use BOOL及原Function CFG/call-return。
- 六个受影响既有分配/选择合同：6 cases / 338 assertions。
- `physical-consumption`：164721 checks / 1 injected failure，按原检查口径通过；不代表193kernel已接新程序。

首两次构建失败、首次设备合同失败及各source/ELF均保留；修正包括夹具unit/原target输入接线和明确scratch表，以及core改Map前的区间准入，未补生产默认零预算。最终通过不覆盖原失败，也不转给后续源码。两次自有设备namespace均已关闭，无活动进程或清理错误。

证据：[检查点与完整补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c1-checkpoint.json)、[统一构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/build-04/build.json)、[首次设备失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/minimum-semantics/execution.json)、[修正后最少语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/minimum-semantics-r2/execution.json)、[最终设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/minimum-semantics-r2/closure.json)。

### 下一相依组与性能边界

先闭合pair/pseudo实际machine schema：Div128/Cpuid secondary的producer birth、联合defs/output link及真实scratch/ABI/alias；GetResult/GetFlags独立的自身编码与numeric/implicit读取。再将完整需求、两scope质量分配、actual state/独立check、logical MEM transport和kernel成组接入，旧Describe/Complete/binder读者真正退出后才做一次完整方向。详见[主方案5.3.12](spill-and-compile-root-cause-plan-2026-10-28.md#5312-下一相依组先闭合机器合同再替换默认链)及[7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)。

本版没有CoreMark、compile方向、QoR、PMU或fresh FEX；不能沿用495/a10计时或da的FEX分母。最终不降生成质量及Block/Function各≤2×fresh固定FEX仍未完成，主方案6.3/6.4冻结门不变。本次按已有结论修订文档，不新增构建/设备矩阵。

## 第七十八批：实际位置整包必要语义通过，无大幅完整方向后撤回

a10为before；584/f470/8b/88ee/c0e至最终861af9f6是同一包的六个相依组。最终25文件贯通实际Writer/独立关系、actual selected uses、唯一reload、所有已识别normal晚owner/fixed发现、Function本块范围、typed source/state及matched角色。没有将整包缩成单独索引测试；整链覆盖和代码审查未发现阻断，四目标及14cases979断言、CoreMark B/F CRC4983通过。[候选完整源码与补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/owner-range-index/whole-package-current/files.json)、[候选必要语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/owner-range-index/minimum-semantics-summary.json)。

中途失败保留：source fixture先缺BeginTargetPlanning，随后用不支持fixed homes24/25/26；生产三文件未因此变化，最终合法homes19/20/21通过。先前常量折叠猜测已纠正。第五组旧Function collectors断言要求无owner，before88ee同例同断言失败；它与既有scalar BirthOnly成功owner合同冲突，修正为明确要求owner/home/publication/end/version，没有放宽生产检查。各中途构建与通过身份仍在原回执，均不转给恢复代码。

唯一整包方向：a10/main120e对861/mainb994，顺序B-before、B-after、F-after、F-before，PROF1、同guest2000/env、原true/真实false回退及只读CPU4准入。B731.728826→731.534357ms（−0.03%），F804.853740→833.553024ms（＋3.57%）；四点CRC4983且自有场关闭。B units2419→2413、IR59758→59494、host243500→242520；F func435→437、decoded4534→4598、IR69090→69898、host342656→347208。各侧/mode一点、工作量略异，无统计回退/单服务归因/最终QoR/资源/fresh FEX资格。[整包四点原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/owner-range-index/direction/accounting.json)。

**没有大幅收益，25文件候选算法撤回，停止同类owner/source/bitmap/grouped-storage/晚发现调参。** 恢复22原文件、删除placement_index.h/.cpp；唯一独立保留旧pinned_gpr_transfer fixture修正。copyfile刷新源时间，当前source495887ca；四目标恢复构建后translator、swift_ir、lifetime逐字等a10，main120e。修正fixture独立1case28断言通过，自有场关闭；原a10生产CRC/physical按同ELF原身份引用，没有重跑相同矩阵，也不继承候选资格。[撤回与精确恢复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/withdrawal.json)、[相同生产ELF核对](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/production-identity.json)、[恢复资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/qualification.json)。

候选局部复杂度改进未转成完整收益；其首次preseed、GPR＋REF约16.125N对旧12N、独立Width/Low32各12N、local words/REF Resolve、source五role/hash/尾栈、候选vectors、真实dirty/P/region及完整proof/动态state都包含在方向内。不能从它没收益推导每项零成本，但也不能因为接口更整齐继续保留新增中间表示。

后续已按[现行主文5.8.2](spill-and-compile-root-cause-plan-2026-10-28.md#582-当前a10生产的主要服务新native-pmu归因)完成恢复a10生产的一B一F native PMU，同raw严格编译分母759/857样本、period803579066/915799836，场关闭且lost/truncation为0。输入/初始fixed16.12%/13.06%、Complete14.93%/20.23%、native全域17.13%/16.35%、实际RA6.35%/6.78%；这是周期服务归属，不是可删份额或普通毫秒。下一按整个inputs→实际决定→Complete→native交付链重写，已存在Known flags、默认narrow align与consumer复用不重复认领；首次graph和单个publication/proof不批准为根治包。[当前出生链复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/m1-scalar-semantic-birth-review.md)、[下一归因边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/major-service-attribution-review.md)。M1/M2/M3/M4及最终质量/两scope≤2×目标均未完成；不再用旧目录或一个冷候选的静态可优化性决定主攻。

## 第七十七批：GPR最终核验完整替代无大幅收益，独立收口实际写入合同

候选source22b18ead、main3d626b60，10源文件完成normal早publication与晚read/width/low32的Q-request归约、固定D/P有序核验、late direct-narrow实际结果输出。无新manager/第二IR；旧public/manual reference、两RA范围、Function入口/CFG/live/fault、真实编码/重发/寿命均保留。初始std::min/max的u16/u32编译错误修正后四目标成功，源码构建期间不变。

最少7命令通过：9cases962断言、physical164721checks/1注入、CoreMark B/F CRC4983；负例比较原独立期待与新核验的preserved transfer/endpoint/clobber/完整消费键。唯一四点B714.960950→730.336552ms（+2.15%），F815.108293→818.285155ms（+0.39%）；各side/mode一点，F实际工作量相同，B请求相同但最终IR/host略异，不作统计或单helper归因。没有fresh FEX、最终QoR或资源资格。

**没有大幅收益，删除新增Q/游标/interval算法及诊断API，停止同类调参。** 最终候选完整补丁SHA`5d5e0d59456d4cd9ef8ad799affabf39493970f47336931c6f6b1fe566a06773`、初次失败、成功语义及全部四点保留。checks和direction两个自有namespace核hash/无活动进程后关闭，不重采更快点。

仅独立保留实际direct-narrow result-home合同：成功selector在原writes标明替换普通结果位置；fixed writer classifier删除phantom旧home，保其他clobber/HostWrite/early writeback；旧prepared projection遇替换先选原受检实际输入路径。只6文件，原compiler/verify/pinned fixture及所有新ordered API恢复；copyfile更新恢复输入时间，避免上一轮缓存旧mtime问题。当前a10c7d3f已独立完成四目标构建及最少受影响语义，不拿候选结果作保留版qualification；不为这条正确性合同再跑性能矩阵。

保留版的独立资格：source`a10c7d3f1273d8125a14f4ff54f1f787e750485e269c3d27112f5ab9f0cea3f0`、main`120e58c2607c2bbca3788547d6eafb8a4d0d0b9e57608308dea98ee0004f5320`；四目标构建源码不变，最少6cases303断言、physical164721checks/1注入、CoreMark B/F CRC4983通过，自有namespace关闭。没有再跑性能，也不继承22b候选点。[独立资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/actual-result-retained/qualification.json)。

该包未替代整个inputs/初始RA/state/Complete，目标仍active、未达成。主方案6.3/6.4冻结原文保留；无stage/commit/push。

[候选完整源码](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/final-candidate/full-from-f7e6.patch)、[候选必要语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/minimum-semantics-summary.json)、[方向原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/direction/accounting.json)、[决定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/direction/decision.json)、[独立保留内容](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/actual-result-retained/integration.json)。

## 第七十六批：紧凑移位整族完整反馈与撤回

本轮按新方案继续代码整合，修正真实失败后完成四owning目标、7条最少语义命令和唯一完整四点方向。最终候选source`78f0083bbf1b31c1ec2705cd04d333cee15d0c8e760c78a30af3d98d0f3f67de`、main`7c4750dc26cdaeb1039374fecbacdc8d280ca48e44b668fd66378f0030eae99d`。

| 修正与验证 | 实际结果 |
| --- | --- |
| 生产集成 | 两入口ReserveTmpX使用显式XRegister；新族按Definition点绑定结果，匹配raw探测；count构造将默认Plus的单数据明确为None，保严格schema |
| 夹具 | retained用公开AppendInst取得mutable Inst；新Function fixture删除ReturnToHost后的重复EndFunction，不改生产生命周期或原期望 |
| 必要语义 | 3个既有case执行4次、1095断言；physical164725checks/1真实注入；CoreMark B/F CRC4983；无中途QoR/PMU/FEX矩阵 |
| 唯一方向 | B727.675579→727.419967ms（−0.04%）；F824.532966→805.382119ms（−2.32%） |

各side/mode一点，Block请求2414→2419/IR59550→59077/host242744→244668，Function请求438→435/decoded4599→4534/IR69909→67832/host347300→343944。工作量不同，不作统计提速或生成质量推断。单族确实少出生了中间IR，但没有大幅完整方向支持新增公共opcode/profile/wire/kernel及支持准入维护，**整包29文件撤回，停止该族调参**；不铺全ISA规则，不追加同假设测量。

所有构建失败、schema IllegalCode和新fixture SIGSEGV分别保留，不合并重标为成功。最终完整源码、正确版本补丁SHA`f9328200154247087cc2c963bfd53af9097127a177549ea7dfe24d47e6adb3bc`、成功语义和四点原始证据均存Data2。checks/checks-r5/checks-r6/direction四个自有设备namespace核对实际文件/SHA及无活动进程后关闭。

当前完整source manifest精确恢复本轮前f7e6，新增shift.h删除，原193opcode及profile恢复；所有此前代码/Git/WIP和独立scratch fixture修正不动。固定缓存最终重建后，main/swift_test/swift_ir_test/lifetime四ELF均逐字节等于原f7e6产物，不重复未改变ELF的语义矩阵。当前目标active，未达标；M1构造/首次服务、M2/M3大决定链及M4真实触达仍需实质替代，不将小变化作为两倍路线证明。

恢复缓存还暴露了独立构建收口问题：copy2保留旧mtime使首个恢复bank残留SwiftVM `kArgs_ScalarShift`元数据，虽source manifest相同，四ELF都不同；该bank未运行或继承旧资格。记录并隔离后，更新时间失效全部28个现存owned输入，再成组重建，四ELF完全恢复。原宽名字过滤的5个VIXL匹配也已单独排除，不冒认成新节点。后续源码恢复必须使实际修改输入的时间/依赖失效，source哈希本身不替代缓存一致性。[恢复资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/restored-qualification.json)。

后端只读审查进一步扣除已有普通BirthOnly决定、FVI稀疏sites及当前raw分组。按最终实际写入有序核验GPR是一个具体候选，仍有extra-write endpoint/view、Function入口义务及真实成本覆盖缺口；它不等于输入/RA/state全线替代，也未被认领为数量级主攻。最终6.3/6.4冻结原文不变，无stage/commit/push。

[最终候选](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/final-qualified-candidate/full-from-f7e6.patch)、[成功构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-compact-scalar-shift-20261031-r6/build.json)、[必要语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/minimum-semantics-summary.json)、[完整方向](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/direction/accounting.json)、[决定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/direction/decision.json)、[精确恢复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/withdrawal.json)。

## 实施中历史：紧凑移位首次整合（07ef，后续以第76批为准）

本条登记本轮文档任务前已经发生的源码整合及首轮构建失败，**不是新完成批次，也不是本轮执行的新构建**。source为`07ef204497b14440712804e76f23da0abbe75e421cbe4dff1120b037e7c046a4`，对照保留基线f7e6，29文件的完整补丁SHA为`74958d9036b5bdcc9151865c89bb9ea233a3bd22d334f107e7ba115f558f2a8f`。

Known早期且canonical的SHL/SHR/SAR分支直接提交一个ScalarShift＋ConditionalShift，覆盖8/16/32/64位及立即数/CL，保原Src/Dst/fault和已有立即数0捷径；Unknown/EarlyOff/非canonical保原展开。公共直接条件flags、may-preserve分析、解释器、raw/Current同源A64 helper、静态3 GPR/0 FPR预算、W-write及retained profile已接入源码，三个既有cases和physical-consumption已扩充。它们均未因写入源码而取得语义资格；旧Facts/Describe/Finish、RA/state/Complete仍保留，不算已替代。

首轮在固定Android缓存构建四owning目标，27秒后退出1，source前后不变；两个编码入口的`ReserveTmpX(result.X())`无法将Register隐式转为XRegister。未有成功的新ELF，未运行候选语义/CoreMark或性能。另已只读确认raw结果会探测Definition点，Current新族却默认WholeValue，选择规则不同；需闭合角色合同，具体动态失败witness未执行。失败日志及原组件包全部保留。

后续先修实际编译和结果角色问题，再统一构建；最少受影响范围为既有shape、Block/Function条件flags＋解释器、同一原shift执行case各跑B/F、retained physical-consumption及CoreMark B/F。通过后完整整族只做一次B/F方向，最终质量集中验收；不新增中途QoR/PMU/FEX矩阵。当前无动态覆盖、净节点量或compile收益，不承诺两倍门已可达。

本轮仅按用户要求更新主方案、文件结构、IR、目标绑定及证据入口；工人只读复核候选边界和原library-leaf报告，没有修改源码或追加构建/设备动作。新增的库leaf结论为历史采样交叉投影，M4应随真实服务删除初始化/结束，不独立以PMR/cache包根治。主方案6.3/6.4冻结原文不变，原重构目标继续未完成；不stage、commit或push。

[整合清单](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/integration.json)、[完整候选补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/source.patch)、[构建失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-compact-scalar-shift-20261031/build.json)、[最新方案](/Volumes/Data2/SwiftVM/docs/spill-and-compile-root-cause-plan-2026-10-28.md)、[实际文件落位](middle-pipeline-file-layout-2026-10-28.md#61-当前紧凑移位候选的文件落位)。

## 第七十五批：首次Facts共访负反馈与撤回

本轮收口上一条实施中记录，而非重开另一组共访variant。候选4f04 / main ac435344在原固定cache四目标构建后，修正既有scratch fixture并完成最少7命令：8cases3011断言、physical-consumption164721checks/1真实失败注入、原native true/false与retained、CoreMark B/F CRC4983/退出0；自有目录逐file核hash后关闭。rich XOR原case仍是冷/manual分配见证，fresh XMM/fault与CoreMark才覆盖真实私有pipeline，资格不混用。

随后唯一PROF1完整方向，双方原native true/真实region失败false后备一致：

| 范围 | d391 before | 4f04 after | 单点变化 |
| --- | ---: | ---: | ---: |
| Block | 742.572591ms | 748.574803ms | +0.81% |
| Function | 812.219636ms | 838.349437ms | +3.22% |

各side/mode一点，units/IR/host bytes略异，不作统计回退或单helper归因，没有fresh FEX/最终QoR/资源资格。Block opt约−28.6ms而regalloc约+34.3ms，Function other约−31.5ms而regalloc约+40.5ms；首次Facts换桶，不是退出这些工作。所有点CRC/exit/原始输入与CPU状态门通过，完整raw保留，自有运行场已关闭，没有重采更快点。

普通DescribeBlock及第二次target参数遍历虽确实退出，GPR窗口、FPR条件完整扫描、consumer solver、rich scratch/Function局部需求/两pool、实际RA/state/Complete仍在；新回调、B ranges、候选和索引恢复增加维护复杂度。**没有大幅效果，整包生产/接口撤回，停止同类回调/容量/索引调参。** 18个源文件按受控快照精确恢复d391，完整after patch、四次构建、原失败、通过结果及四点均保留。

只独立保留经原d391复现确认的原scratch fixture修正：相同节点/删除规模留下可压缩连续pair和真实ID洞，等profile只在连续覆盖时合并，并检查区间不重叠。当前source为`f7e6c3a8a46a8b62ac290b0d79a82ac98659b2c9bd6d4b766d9f8057e04c377f`，与d391逐文件差异仅该test；恢复后四目标构建通过，main/swift_test/lifetime SHA均与d391相同，因此只运行改变的原scratch case，2110断言通过，自有目录关闭。没有重复未改变ELF的语义矩阵或追加性能点。

此外仅离线展开旧source123d已qualified的P0 Other：517/575样本，原分母/rawprefix/地址/period闭合，0新采样/构建/设备命令。它显示decode/build、inputs/fixed、residual、RA等多个真实服务，不能当当前毫秒或可删份额。FPR-only和稳定transfer候选仍缺当前主成本覆盖，不据“有可写代码”立为数量级主攻。目标active、未完成；主方案6.3/6.4原文保持。

[候选最少语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/shared-final-facts-20261031/minimum-semantics-summary.json)、[完整方向与原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/shared-final-facts-20261031/direction/accounting.json)、[撤回决定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/shared-final-facts-20261031/direction/decision.json)、[精确恢复及fixture资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/shared-final-facts-20261031/restored-qualification.json)、[恢复后构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-first-facts-withdrawn-fixture-20261031/build.json)、[历史Other离线展开](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/qualified-other-work-review-123d-20261031/report.md)。

## 2026-10-31 最新结论同步方案（仅文档）

本轮按用户要求修订主方案、文件结构、IR、目标绑定及证据/历史入口，没有修改源码、构建或访问设备。主方案增加当前Known/native净工作账、FPR分配后准入的具体退出项和现码retry上界；将已实现共访与尚未完成的语义/性能资格分开。公共直接字段和native原用途分组不再以待实现候选重复立项。

沿用最终两scope各自≤2×fresh FEX、最终生成质量及前后端解耦要求；主方案6.3/6.4冻结原文不改。实现仍按相依组构建与最少语义、完整算法一次方向、最终集中验收推进，不增加中途QoR/PMU/FEX矩阵。

## 实施中：首次Facts与目标输入共访

以下为上一文档轮待检时的历史记录，后续通过、完整反馈及撤回以第75批为准，不再表示当前源码。

本条登记文档修订前已经发生的整合、构建和失败，**不是已完成批次，也不是本轮新执行**。当前source为`4f04f8434899698073354ba496532772089559307a2bb53b44439dc83d0a592f`，main为`ac435344d3958bd05844782b18ef60eabd7e9396ec29e35b2414cb837dd6a093`。

内部Optimize保最后编号/seal，延后首次Facts，原Storage/Allocation/PlanBuilder先就绪；公共窄FinalFactsVisitor共用原checked访问和raw/resolved occurrence，backend直接生产原inputs草稿。整输入SSA成功及必要后置闭合后才移交RA，失败reset原request；公开FromFinalIR仍eager，已准备Facts或policy不同者预先走原受检路径。完整schema/SSA、支持准入、Block/Function范围和原重发/退役不改。

| 实际退出 | 保留与新增工作 |
| --- | --- |
| 普通完整DescribeBlock、第二次target参数遍历、同occurrence的重复bitcast解释 | 首次公共N/E检查与局部target分类、回调、原索引/summary/live；无第二N/E表 |
| consumer前缀不再另建完整目标描述循环 | B级Finish ranges、真实FPR/CallReturn候选、较早first-consumer行、临时索引恢复、原完整consumer solver及clobber归并仍计费 |
| scratch静态基础与checked visit共用 | rich flags、Function局部emission需求、两pool和真实profile整理后置；GPR窗口、FPR条件全量扫描、实际RA/state/Complete未退出 |

四个owning目标在r4回执中构建成功，Release/O3/ThinLTO、无PGO、源码前后不变。此前语义状态须逐项保留：

- 初次五case组四过一失败；rich fixture误加到冷布局，调整回仅layout0。第二次仍在原scratch压缩断言失败，其余计划命令未执行。
- 用原d391二进制单跑同一既有case，复现`22 < 22`失败：稀疏布局删掉所有连续Nop，却要求必定压缩。它不在第74批已运行的最少case中，不能据旧最少语义宣称该case原本通过。
- 随后保持原节点/删除规模，留下真实连续同profile pair及真实ID洞；等profile只要求在连续覆盖时合并，并检查区间不重叠。r3/r4仅改此既有fixture，生产main均为ac435344；**修正后的当前fixture尚未运行，不能将前两次失败改写为通过**。

两次自有语义运行场及原版诊断场已收口，原失败保留。当前没有本包ordinary compile、fresh FEX、PMU、最终生成质量/运行/资源资格；下一步先最少语义，通过后再做一次完整算法方向。共访只是M1/M2的一个完整子服务，不能认领整个大项或≤2×已完成。

[当前四目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-shared-final-facts-20261031-r4/build.json)、[初次语义失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/shared-final-facts-20261031/checks/execution.json)、[第二次语义失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/shared-final-facts-20261031/checks-r2/execution.json)、[原版fixture复现](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/shared-final-facts-20261031/baseline-scratch-fixture/receipt.json)。

## 第七十四批：公共参数直接字段与当前输入共用

用户恢复实现目标后，主线程整合两个完整服务替代，共14源文件。source `d391c501b8386847ffe9f5cf1784c64430b496fa69d46d329fa6cfb8ccc8d9c9`，main `e9229afe2962048bd98aacb7183b489e2f2dfb064b0ae30e3466e817a9faea7d`。

| 原工作退出 | 实际替代 / 保留 / 新成本 |
| --- | --- |
| 公共tag/offset变长packet、local Value解码、编辑搬移与公共overflow分配/清理 | 五个直接语义cell；构造/编辑/uses/Facts/target全部迁移。原schema/SSA、views/完整Imm、graph/observer事务、Params sole owner保留；birth只预检count delta再直接提交。Inst104B/managed Slot128B，physical48B原格式保留，实际backing增加未获最终资源资格 |
| native资源独立ForEach、按role重复全consumer copy推断、大MEM Params线性判重 | 当前rawID＋ordinal分组供原logical MEM与懒推断pair共同借用；保最后成功view、selected/LowView/preserved优先级、原domain/body冲突、multiplicity和distinct首次顺序。O(k log k)当前组、实际role Map/field读取和P/H仍计费，cold/显式physical保旧链；无跨指令E表 |

两个有限源码peer均未见必修，不冒充运行资格。第一次成组构建发现CompileUnit旧guard引用已删除overflow字段，修正后四个owning目标Release/O3/ThinLTO、无PGO通过，source前后相同。旧semantic overflow用例改为实际直接字段/无第二payload见证，没有造假分配；原Params、graph、foreign、tombstone/部分失败检查保留。

红魔bf1359ff最少8命令通过：既有10 cases444断言；construction fast/slow各557224checks/11真实注入；physical-consumption164721checks/1注入；原true/false native重发、retained、CoreMark B/F CRC4983及退出0。采集脚本将expected文件map重名为lifetime regex，导致首次仅cleanup未执行；原runner/execution/initial closure保留，独立close-only重新核对精确文件/SHA后回收自有目录，**未重跑测试**。无活跃写入/剩余清理错误。

随后仅一次同配方完整PROF1四点，原native true及真实region-failure false后备两边一致：

| RA范围 | 3ef before | d391 after | 单点变化 |
| --- | ---: | ---: | ---: |
| Block | 761.849865ms | 726.782447ms | −4.60% |
| Function | 858.282612ms | 823.502034ms | −4.05% |

每side/mode一点；Function units/IR/host bytes相同，Block略异，不分离两个组件归因、不作统计收益/最终质量资源采用。采样CRC/exit/身份及粗桶闭合通过，自有采集目录已核hash回收，无新PMU/QoR/FEX。**没有大幅效果，停止字段/frame局部调参；维护成果保留，完整目标仍active。** 下一包处理最终公共首次服务与稳定目标输入的相序/整段服务退出，状态源结构、两scope重做及M4缺口继续保留。

[净源码补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/direct-public-arguments-20261031/source.patch)、[当前输入交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/current-consumer-preparation-20261031/manifest.json)、[首次构建失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-direct-public-and-current-input-20261031/build.json)、[四目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-direct-public-and-current-input-20261031-r2/build.json)、[最少语义/闭场](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/direct-public-arguments-20261031/minimum-semantics-summary.json)、[四点原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/direct-public-arguments-20261031/direction/accounting.json)、[方向报告](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/direct-public-arguments-20261031/direction/report.md)。

## 2026-10-31 性能上限复审落实为工作量合同（仅文档）

按[最新复审第9节](compile-gap-review-2026-10-31.md#9-新版方案的-compile-性能上限复审)补充现行方案，代码状态仍为第七十三批3ef；没有新增源码批次、构建、设备运行、计时或实现任务恢复。

- 主方案5.2.6规定公共参数表示的整链字段/写口合同：必要时替代原packet解释，覆盖构造、编辑、用途、Facts和target，不建N字段镜像，不重复认领现birth/UseGraph/Facts。
- 5.3.5将当前语义解析供实际role与原逻辑用途共同借用，保MEM multiplicity/domain冲突和大Params有界聚合；未selected推断copy只活在当前frame，保持原LowView/selected优先级，不回写selected-use表。
- 5.3.6约束稳定源链、实际事件及alias输出；5.4.1区分局部scratch、FPR合同、语义失效与region重发，要求填实有限重试及重建范围，保两种RA scope与最终质量。
- 5.8加入正常/困难路径工作表，M4按实际live/capacity/初始化/触达/清理记账；6.1用旧da账说明覆盖要求，条件算式不是收益预测。
- IR、目标绑定、文件职责及历史阅读入口同步。没有新增manager/图、性能探针或逐字段测试门；相依组最少语义、完整组合方向反馈、最终集中验收保持，6.3/6.4冻结条款逐字保留。

本次补齐的是待实现的具体要求，不代表新算法或≤2×性能已验证。以下上一轮文档及源码记录保留其当时状态。

## 2026-10-31 差距审查后的方案修订（仅文档）

本次依据[现码与固定FEX审查](compile-gap-review-2026-10-31.md)修订主方案、文件结构、IR与目标绑定，并同步本记录/证据入口。没有生产源码改动、新构建、手机运行或采样，也没有恢复暂停的实现任务。

- M1从DCE/flags小项扩为构造与公共首次解释：少出生节点，共用最终checked typed访问；已有Facts缓存不再认领。共访需要调整原输入owner准备相序，完整准入后发表，失败不留下可消费半成品。
- M2/M3共同替代目标需求→两范围分配→架构状态→实际writes/选择→最终绑定。assignment子桶小不再作为暂停整个服务的依据；位置冻结和form/flags完成分开，SSA位置与guest-copy事实分开。
- M4保留真实请求、初始化、发布和结束主线；不移动timer，不改RA范围。3ef输出/重发是已完成基础，不重开codec或origin格式工程。
- 文件表说明唯一写者、最后消费者、失效与旧工作去向；当前public/retained语义入口仍保留，不能误称所有路径均已physical化。
- 主方案6.3/6.4原文逐字保留；开发继续按相依组构建/最少语义、完整算法方向反馈、最终集中质量/资源/两范围≤2×。

这轮产生的是更明确的算法与实施边界，不产生新性能资格；没有已证明可兑现两倍门的完整组合。

## 第七十三批：唯一语义输入延迟退役与完整模式重发

本条补记已经完成的代码批次，**不是本次文档轮新执行**。source为`3ef11319b014832d240b8319b1d6342c2153b6549e28c7557719e2d2f9444098`，main为`e4e43f6386f5eb7a3dd55dc639aedb03671173da98d2da9a4c26726325ae568d`。

普通native保唯一sealed IR、frozen Allocation/Selection到最终有效模式与Flush；真实B/edge/terminal/sharedcall metadata捕获/移交一次。每次从空contexts/ASM重走typed visitor；原true的真实region申请失败才完整重建contexts发false，不重跑RA/Complete、不重复move metadata。沿原权限，在最终Flush且最后语义借用结束后，publication前全group CloseSemantic，再cursor Abort/Retire并清Allocation/Selection。失败先结束encoder/span/cold借用再收口；原ExecutionAccess与最后lease按真实边界结束。

第71/72批普通native逐N 24B origin覆盖、早消费和混合prefix/suffix格式已删除，不建第二IR/physical图或机器码片段重布局层。193个typed physical kernel与真实retained wire继续共用；public/retained的语义IR＋Allocation/UnitActions路径仍合法，并非都已转为physical stream。

四个owning目标构建成功：`svm_translator_linux`、`swift_test`、`swift_ir_test`、`swift_hir_lifetime_test`，Release/O3/ThinLTO、无PGO，构建前后源码不变。最少检查为原三个cases98断言、原physical-consumption模式164721checks/1分配失败注入、CoreMark2000的Block/Function CRC4983和退出0；自有运行场已核hash关闭，无活跃写入/清理错误。

实际见证包括同一Prepared的true再false native发码、原IR/Map一直存活到显式退役、原retained重放/部分失败所有权及真实guest observer/XMM fault。**没有强制制造生产region分配失败，也未覆盖所有fallback/QoR路径。** 原uses/Params/Map/Selection与机器码缓冲重叠，真实fallback再次绑定/编码，末次N清理都仍计费。

该批没有ordinary compile、fresh FEX、PMU或最终质量/资源采用；大幅性能算法和两倍门仍未完成。第72批“私包开始、尚未整合”的状态由本条取代，旧codec四点继续只属于0641。

[整合回执](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/delayed-retirement/integration/receipt.json)、[四目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-delayed-retirement-20261031/build.json)、[最少语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/delayed-retirement/minimum-semantics-summary.json)、[运行场收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/delayed-retirement/checks/closure.json)。

以下各批保留其当时记录、判断及失败；后续执行以现行主方案为准。

## 第七十二批：完整typed-current codec替换与必要语义

当前source`0641c577a3719ab9dcd6221fd4cf4f8aabbd7718c81264f4551592944cd63662`，main`70d33e87ee9ad85a8cae06fec538a785b9c5aa6b7132de8f2f6116c8f70906a9`。Root成组整合4个compile producer源、15个jit consumer源和唯一既有lifetime API接线，共20文件；source review发现的enum-class到原u8 SetFlags参数显式转换随组修正，未改变算法或旧断言。

正常native setter直接写一个CurrentInstruction/CurrentParams；Packet、Seal、External payload、Materialize及参数Reader退出该域。全部193 kernel和实际flags/EA/hostcall/atomic/JitContext读者同步迁移；retained只在真实输出边界wire编码，每次replay顺序Decode一条后调用同一typed kernel，不保第二套packed kernel。当前typed capacity、actual roles/Params复制与reset仍计工作；不把绑定、logical资源/MEM uses、Complete/运输/实际编码记成退出。

统一四owning目标构建通过，source前后不变，Release/O3/ThinLTO无PGO。必要检查仅原三个case80断言、唯一变更客户端physical-consumption原模式（164721checks/1分配失败注入、两次实际replay/overflow/last lease），及原CoreMark B/F CRC4983/退出0；全部通过，自有目录核hash回收，无活跃进程/清理错误。没有新case/断言/规模、lifetime矩阵或QoR/PMU/FEX。随后按主文5.3.3实际执行同false-mode完整四点，结果见下，不继承M0 FEX分母。

唯一四点已实际完成关闭：B745.145458→749.408020ms（+0.57%），F885.665462→860.270114ms（−2.87%），各side/mode仅一点，units/IR/host bytes也略异。无大幅效果，不孤立归因codec/codegen，不称零回退或正式性能采用；停止同类variant/getter/容量调参，结构保留。所有点CRC/exit0、原始身份/状态验证，namespace逐file hash回收，无活跃资源。详见[四点账与限制](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/typed-current/direction-prepared/report.md)。

原合法false-link模式仍固定，尚未恢复原完整生成质量。有限代码核对已选择下一完整算法：保唯一原IR/Map/Selection至实际cache模式与最终Flush，原ctx true失败再false只读重发，最后group Close→Abort→Retire；不建physical影子或VIXL片段层。私包开始实现，尚未整合/构建。主要输入算法和最终质量/资源/两倍门仍未完成。没有提交或push。

[整合与root接线修正](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/typed-current/integration/receipt.json)、[四目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-typed-current-20261031/build.json)、[最少语义/闭场](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/typed-current/minimum-semantics-summary.json)。

## 第七十一批：M2全族native语义原型与最少检查

Root将资源/current-record组、全族emission组及一个既有region准入case扩展成组整合，共12源文件；source为`e944c2fe63176e312f6f078f84cd16440428a88591f8ae118f3d9f993716555f`。三owning目标统一构建通过（Release/O3/ThinLTO，无PGO、source前后不变），最少三个case80断言、CoreMark B/F CRC4983与退出0通过，自有目录核hash回收，无活跃写入/清理错误。无该原型的QoR或compile性能结果。旧M0的da/main7ec与16.68×/18.62×保持基线身份。

普通fresh Block/Function在原完整Complete和实际组权限后，由最后visitor直接调用同源编码kernel；当前只保一个48B packed适配记录和当前资源frame，编码/flags/scratch结束再销毁，原槽只留24B origin，旧完整body/resource序列和第二遍body遍历在该域退出。原子组、两RA scope、真实MEM逻辑用途、Disabled AdvancePC、Covered Tick、cold/terminal和部分失败suffix结束合同保留。retained/public及显式旧physical路径继续合法，origin格式不授予重发码权限。

该批固定现有合法`Emit(false)`，允许中途direct-link/continuation质量差异；完整typed直连、控制模式变体/reloc与replay未完成，不做性能采用。最少检查为原组准入case的retained/native两路径、已有fresh decode观察者与XMM fault case、CoreMark B/F完整CRC。没有新增QoR/PMU/FEX矩阵或性能点。

[整合记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/integration/receipt.json)、[统一构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-native-output-prototype-20261031/build.json)、[最少语义与闭场](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/minimum-semantics-summary.json)。

## 2026-10-31 M0：当前fresh FEX开发基线与选型（非源码批次）

当前source仍da/main7ec。M0现已完成同轮普通开发对照：Block791.933ms/FEX47.477ms=16.68×，Function880.364ms/FEX47.276ms=18.62×；另两个现成PROF2诊断只用于选型，不作普通耗时。四个ordinary与两个诊断进程均CRC4983/退出0，私有FEX服务和自有目录已关闭回收。M1外层DCE/flags和M3sort/assignment据实际范围暂缓主攻，转向M2真实target-input准备和最终交付；目前没有已证的达标算法组合。详情见[主文M0结果](spill-and-compile-root-cause-plan-2026-10-28.md#23-m0已执行当前同轮对照与实际优先级)。

Root复用旧普通采集和raw/FD/lifecycle校验，仅适配新namespace和四点schedule，不运行原42点driver。当前main7ec已核hash，固定普通FEXLoader863/Server9fe与guest原bank只读；SVM PROF=1，无PROF2/DECODE/LOW/IR_DETAIL。四点顺序B-SVM、B-FEX、F-FEX、F-SVM，各一个fresh分母；所有点完整证据通过，server正常-k/真实wait0及socket退出。随后唯一两个PROF2进程对同source作阶段诊断，不用于FEX比值，也不补替普通点；同样通过并closed。

本轮未新build/改源码，原构建和必要语义资格保身份。ordinary精确ns账及counts见原JSON；Block C213.391/A-B467.199/D111.342ms，Function255.359/496.317/128.688ms均按原timer分桶。Block opt范围还包含返回期间的Facts等工作，不叫纯residual。FEX计时仍initial-context CompileBlock，排更早线程初始化和server启动；单对不替代最终统计/质量/资源。

现成诊断 outerDCE2.699/1.822、flags13.826/29.444、assignment18.483/27.122、sort0.822/0.353ms，含observer/必需工作，flags含内部DCE。由此暂缓M1/M3主攻，不开启这些小域新patch/采样队列。regalloc_total171.328/169.083与Allocate52.301/58.031之间仍含Initialize、PlanBuilder和完整target输入/fixed准备，不能都归Describe；sealed首次SSA/BaseFacts已在更早入口完成，不能再认领缓存收益。Complete计数236.431/262.863含FVI，下一重点是M2完整输入/交付替代，尤其native body/replay真正退役物理中转。

清理审计在删除前两次停止：补存FEX生成的Server.lock，并将12个host-only dump文件从remote roster分开；随后296个自有文件逐项hash核对再删，独立验证目录不存在，protected bank不变。诊断自有目录也已闭合；没有额外warmup/替换点、频率写入或其它项目操作。当前源码仍da，未stage/commit/push，最终目标未完成。

证据：[M0报告](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m0-current-fex-baseline-20261031/M0-result.md)、[普通原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m0-current-fex-baseline-20261031/compile-gap-summary.json)、[ordinary闭场](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m0-current-fex-baseline-20261031/closure.json)、[独立诊断](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m0-residual-diagnostic-20261031/execution.json)。

## 2026-10-31方案重写：按实际大项选择完整算法

本轮按用户“帮我重写方案”重写[实施主文](spill-and-compile-root-cause-plan-2026-10-28.md)，同步结构、IR、目标绑定和历史入口；不是第71个代码批次。源码仍为da/main7ec，未构建、测试、访问手机或新增性能点；此前第70批源码/构建/必要语义/撤回身份不变。

新路线先M0同轮fresh基线与大项选型；已有基础移出待办，取消0.7F/0.9F/0.4F等无算法支撑的分线承诺。具体M1为Block DCE因果队列和Function flags dirty前驱闭包bottom重解，补齐出生、直接降计数、root及跨块producer通知；dirty块即使transfer相等也须自身清理。M2区分有序事件查询与最后消费直接生成native body的完整交付候选，后者须先解决Emit(false)、控制片段布局和所有metadata重定位；M3实际allocator替换保两scope，Function尚未闭合；M4按真正编码/服务大项实施。

当前没有已证能兑现≤2×的算法组合。候选需列旧循环、替代伪码、所有读者、退出/保留/新增和绝对影响，不再用“完整服务已实现”代替主要成本覆盖。中途成组构建＋最少语义，算法级开发反馈，最终集中采用；主文6.3/6.4逐字保留。必要新证据针对不同假设，不重做已关闭P0或给失败候选调参补快点。

[逐文件目标结构](middle-pipeline-file-layout-2026-10-28.md)仍为最终组织，未迁前明确目标名；目录搬迁随算法收口。没有stage/commit/push，原Git/WIP和全部历史失败保留。当前大幅性能/最终质量/资源与两范围freshFEX目标仍未完成。

## 2026-10-31：第70批代码状态（M0测量前）

本轮完成一个不同于intervalstream的Function数据流替代，收口失败与一次完整组合方向。新算法未显示大幅收益即停止；同时定位并保留一个原版已存在的实际容量错误：每个256节点page都无条件reserve目录capacity×2，导致目录对page指数膨胀。容量守卫保原两目录预备后发表/异常/lease协议，没有新增管理层。

完整函数求解替代已撤回，独立dense fixed-point oracle与原最大case未改，当前原求解器配合Store修复再次通过四case98断言与CoreMarkB/F。新源da/main7ec只有必要语义，没有追加普通计时或FEX/QoR/PMU点。七文档及实际source/ELF/失败/闭场身份同步，6.3/6.4最终门逐字保持；目标active未完成。

## 历史执行轮：Block完整候选反馈收口

新方案明确整段算法必须接受完整服务反馈。本轮已精确合preseed gate/witness、三目标构建、最少必要语义、只读设备状态、绑定/执行原四点并关闭全部自有运行场；没有新PMU、QoR、FEX或额外快值。独立离线复核通过，静态生产准入可达但未统计实际命中比例。按停止条件撤回无大幅收益候选，下一算法缺口明确保留。

七份文档同步到当前02ae和真实新账，6.3/6.4最终门逐字不变。没有把候选235断言或after时间转标签给当前代码，也没有把负反馈包装成性能完成。完整资料位于`block-stream-allocation-20261030/closed-direction/`和`stage-block-stream-allocation-direction-20261030/`，目标保持active/未完成。

## 历史文档轮：按完整算法、两种分配范围重新组织方案（纯文档）

按用户“帮我重写方案”，重写主文及最终文件树，同步IR、目标绑定、证据入口和历史导航。主文以当前780.850/890.689ms完整账限制收益上限；已有IR/物理消费/IntegerFacts等全部作为基线，P0已关闭。Block definition stream列为已实现、待修补验证的完整候选；Function新算法、Complete整段替代、构造与固定费用达标算法显式保留缺口。已否决的fold/immutable模板、窗口/字段/getter/heap不再接力。

最终目录补清真实编译CFG owner `function_ir.*`、构造器、persistent Function和存储lease；allocator按六项服务、两个scope schedule和共同assignment组织，列逐文件职责及旧源按函数去向。版本/失效边界按实际Store session、worklist epoch、sealed Facts、attempt和location编辑分别说明，不新增版本或证书框架。plan*明确正常临时solver、retained/public最后读者和退役条件；共享编码kernel仍随算法收口，不算性能阶段。

本轮未修改生产/测试源码、未合入私有gate/witness、未构建或访问设备，未提交/push。三个源码资格分开记录，不把工作树构建成功写成已验证算法。主方案6.3/6.4逐字保留，SHA `b05fc438eec3a711eccc5a64ba919d1143d4593defc094022713cb72fad414c4`。最终生成质量不降低和原两模式各自≤2×fresh FEX门不变；中途只统一必要构建和最少相关语义，完整算法一次方向反馈。

## 历史文档轮：以当时完整成本重写实施方案（纯文档）

本轮按用户要求重写主文，明确当前约780.850/890.689ms并修正旧总数和旧构造费用。IntegerFacts、同槽物理消费/最后结束、Block birth、Function entry-width标为已完成基线；不再作为未来大幅收益。主线改为P0一次有限当前归因→P1当前分配到编码服务的整段替代，列出Complete、资源形成、实际编码、RA四种选型条件及退出工作；P2/P3保留未解预算，不成为P1前置或微优化接力。

配套修正逐源结构及已退役/当前剩余映射：不新建select.cpp调度层，ExecutionAccess持真实锁而CompileUnit借用权限，明确ValueWorklist固定epoch，retained接口按实际读者迁完再删。维护收口不记性能。主方案6.3/6.4逐字保持，原失败及历史批次不改写。本轮只改文档与状态记录，未改源码、构建、测试或访问设备；P0材料仅待执行，未提交/push。当前源码和产物仍为123d/main33c04、实际生产c904。

## 执行约定

2026-10-29 用户明确调整实施节奏：重构中允许暂时影响生成质量，不对每个小改动反复跑完整生成质量、PMU或FEX对照。实现阶段集中处理真实结构，以构建和必要合同检查发现接缝；阶段整合后集中做语义、生成质量、资源及普通性能验收。最终不降低生成质量与两模式分别≤2×FEX的目标保持，阶段中间状态不冒称已过最终门。

2026-10-30再次确认同一节奏：不在每次结构变更后检查生成质量；按完整改动批次做必要构建和最小受影响语义检查，通过后继续实现。只有新改动、失败或具体未决问题才补相关检查，不因文件数或改动量自动扩成矩阵。

全部新增结果位于Data2；沿用固定Android构建缓存，不在系统tmp堆积大产物。必要合同与阶段归因分别保留实际源码、ELF和范围，不把旧PMU72转标签充作当前质量基线；目前没有本轮新的合格普通FEX对比。

第六十批旧C2反馈无大幅降时，C1否决、D旧核对结论保留。之后普通direct物理路径已贯通，旧物理方向组CRC失败及修正保持各自身份；第六十六批以两侧已direct的产物测新Block构造/Function宽度服务，约781/891ms仍无大幅结果。停止该组服务同类扩张；当时安排的Complete/物理形成/实际emit有限归因现已由P0完成关闭，不重采，不以目录/getter/新格式小包接力。较早literal/entry独立草案未整合，不与本批真实发布合同混称。

2026-10-30按用户要求重写方案：主文整篇重写，历史根因和测量移至[证据附录](middle-pipeline-rebuild-evidence-2026-10-30.md)，最终6.3/6.4原文保留，文件树标明保留/已退役/条件性收敛。后续阶段A/B/C/D以重写主文为准，与本记录历史阶段字母不混用。当时Prepared Storage仅完成只读分析、scratch直接交付仅为候选；该次文档重写未改生产代码或测量，文档维护不计代码批次。随后实现及必要检查见第五十八批。

此前2026-10-30按用户要求的重写明确尚无可信的两倍算法组合，维护性/配套交付与主算法分开记，先闭合三条成本线。该次文档重写时共享代码为4d6，C1/C2尚待有限设计，未改源码或测量。其后C1否决、D核对和C2实现/失败见第五十九批；不将这个历史段落作为当前待办。

上一次纯文档轮按当时代码重写主文：撤销C1首轮退役承诺，记录C2构建/原见证失败和未测状态，细化5.3.3后端消费表示的替换条件、全部消费者与预算缺口；同步逐文件职责、Known/EarlyOff/Unknown及两种RA范围。最终6.3/6.4逐字保持。该次只改六份文档，不新增源码、构建、设备或性能执行，不计新代码批次；随后真实代码执行见第六十批。

## 历史文档轮：物理消费落码前的完整编译架构方案重写

按用户“帮我重写方案”整篇重写主方案、文件结构、IR合同和目标绑定，同步历史入口；不新增代码批次。现行路线保留公共Inst/Arg/Store/order/uses和原两种RA，selector直接写最终物理form；正常执行型请求在最后typed读取处消费原槽，同一结果供emit/re-emit/Flush/metadata，最后owner真实结束。主线不再是先全Actions再Copy，也不为lifecycle另起层。

目标结构按入口、纯能力、约束、架构状态、最终选择/物理格式、验证收敛ARM64 compile。逐源职责和旧plan/registers/select/verify去向写入结构表，真实分配和编码算法保留；文件合并本身不计性能。完整普通Block/Function消费者、guard/group、部分转换失败、用途图关闭、资源唯一移交和重发协议成组落实，不以少数opcode或getter包装宣称完成。

构造侧明确有限深事实服务只替换width/conditional逐root解释；新增闭包/初始化/dirty/sort/fallback照计。C195/237、A/B377/411、D98/133预算继续互斥，真实RA/运输/编码、发布和未归因58/86ms不能由新架构自动认领。当前约671/780ms不是提速，新架构尚无支持≤2×的实证；不再以无限通用audit或配套小包填缺口。

原最终6.3/6.4逐字保留，SHA `b05fc438eec3a711eccc5a64ba919d1143d4593defc094022713cb72fad414c4`。重构中生成质量允许变化，成组必要构建/最少受影响语义；完整算法一次方向四点，最终集中质量/资源/fresh固定FEX。此次只做文档审阅与链接/一致性检查，没有源码变更、构建、测试或设备执行，原Git/WIP保留，未提交/push。

设计来源：[构造](</Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/whole-compile-service-redesign-20261030/construction.md>)、[物理消费者](</Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/whole-compile-service-redesign-20261030/physical.md>)、[最后消费者](</Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/whole-compile-service-redesign-20261030/lifetime.md>)。私有草案是有限设计，真实payload移交/计账及失败清理以最新IR细则为准。

## 当前实施批次

下表保留起点的分项实施记录，不是当前待办队列；后续各批继续修正状态。当前施工起点和完整主阶段只按主方案7.1/7.2及本记录最新两批，避免将已经退役的表示/用途工作再次列成待办。

| 路线 | 正在落实的实际删除/接口变化 | 状态与待验证边界 |
| --- | --- | --- |
| P3a Function用途 | 移除HIRValue/HIRUse独立对象与values/uses pools；新增FunctionUses，Inst/Arg继续作为定义/type-view权威，按Store/NodeId的head+occurrence locator保存反向关系；实际写入口维护输入链，RPO仅改独立order view | 旧API、sealed consumer复用及连续consumer存储已接，通过六目标构建及相关定向合同；第六批已改为按实际owned writer与完整成员状态借用，不再比较同一payload与自身；冷manual/unowned输入保留显式扫描。CFG/entry/terminal/root/lease与两种分配范围保留，净性能待测 |
| P2a 目标绑定 | 拆纯GuestStateLayout与唯一GuestStateBindings；公共target小合同；runtime一次解析、Module缓存同一不可变环境，IrBuilder/CompileUnit/Prepared保活；decode与prepare不再各自Resolve不同目标 | 绑定、host features、物理表与helper/profile/TSC三片已过六目标构建；Unknown不含物理映射，公共接口不依赖ARM64/VIXL/RA控制器；完整语义/ABI仍待后续集中验收 |
| P2b 字面整数构造 | 非LOCK Add/Sub/Cmp/And/Or/Xor在LoadImm分配前，复用同一纯immediate policy选择直接Operand；不先建SSA常量再改写其consumer | 已过集中构建与字面提交/解释器回退合同；Known+EarlyOff、Known unsupported与Unknown保留SSA路径，窄NZCV/ADC/SBB/LOCK保留原处理。不声称已有覆盖率或compile收益 |
| P1a scratch定价 | 既有Inst precise入口为普通scalar Select给上界2，EA按实际形态覆盖旧EmitOperand回退与direct-dst两条路径；窄重复输入按occurrence计费 | 已过集中构建与有压力的完整发码合同，尚未穷尽全部flags/fault分支；全局reserve3、FPR、reload、pool和安全ladder不变，特殊host home/bitcast/未识别形态保留default |
| P5a 发码缓冲 | 复用MacroAssembler实际buffer容量，完整logical reset、重入回退和借用者先退役；不缓存Allocation或UnitActions | 已过集中构建与本批真实发码/buffer增长合同；完整reentry/线程退出仍待验收。不等于独立Block请求分组，不改变逐块分配、code allocation、入口、fault/SMC/link所有权 |
| P3b 存储与可变清理 | 生产Inst归IRStore，稳定身份与RPO位置分离；普通Function/真独立Block worklist借唯一编号，cold保NodeId→row目录，DCE排队保存handle；Params改连续存储 | 基础迁移及后续BB编号共访已构建并过相关合同；mutable worklist的当前epoch roster仍有独立职责，不能把编号当存活身份 |
| P3c 紧凑参数与顺序 | 删除固定Arg[5]；S批为48B Inst＋16B身份header，真实managed slot64B；保24B内联typed字段，同源Value按cast为4/5B、raw/foreign为9B；正常顺序为NodeId32双链，混合来源用单一cold链 | S-r2七目标构建、construction与IR合同通过；successor fixture编号修正已随T构建和原case通过，原S失败记录保留。coarse owner目录、冷链容量及迭代读取成本另计；无新普通compile结论 |
| P6a 最终宽度查询 | residence/narrow在最后追加之后一次准备有序查询，删除逐ZeroExtend全unit线性查询与Seal重复排序 | 构建与既有真实Complete入口合同通过；原归并/冲突/选择优先级保留，完整质量与收益待集中验收 |

各片按不重叠的源文件责任并行实施，Root统一构建与整合；不对部分文件能编译作整体完成判决。P2剩余直接构造、P3独立Block按需用途／唯一operand关系以及剩余P1/P4仍需按完整目标继续，不以本批局部完成替代全目标。

## 证据

- [实施目录、修改前源码快照与审查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/)；修改前881个runtime/tests/translator等文件共约29.7MB，逐项与r7 manifest一致。
- [普通质量基线工具及只读身份核查备忘](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/ordinary-quality-baseline-plan-r1/EXECUTION-NOTES.md)。

后续构建、测试及差距以实际产物和收据追加；当前没有新的普通运行/compile性能结论。

## 首次集中整合

`build-stage-a`暴露新target头对枚举宏的隐式包含依赖；补齐自包含头后，`build-stage-a-r2`通过五目标：`svm_translator_linux`、`swift_ir_test`、`swift_hir_lifetime_test`、`swift_address_space_shutdown_test`、`swift_riscv_backend_test`。实际generic Release/ThinLTO产物已独立保存，source为 `f9913311ddf66611e080733fe02b45215965b110ace61aa7eb9375f7bcf99797`；这份中途构建不作为普通性能资格。

红魔只执行一次阶段定向检查：IR四用例358断言通过，HIR construction/prepared/thread-exit及shutdown construction均退出零并报告实际checks。IR筛选中一个多星号模式未命中，原stdout已保留，因此只声明实际执行的四用例，不冒称完整IR矩阵通过。私有三份设备二进制在核对hash、确认无活动进程后回收；无PMU/FEX进程、无频率写入、未改protected bank。

证据：[构建与实际产物](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-a-r2/build.json)、[定向检查原始收据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-a-contract-checks/execution.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-a-contract-checks/closure.json)。上述收据只证明该阶段范围，不替代后续结构改动的集中构建和最终质量/性能门。

## 第二次集中整合

`build-stage-b-r2`在源码停止写入后通过六目标（前五项加`swift_test`）；source `2e580343d5c94a778034040681efc3acf7bdf774a2973c4746041640892e296f`，构建前后manifest一致。主程序与IR测试的实际产物分别为`e4ac9d70…8b417c`、`d972eda2…764515`，已独立保存。此前`build-stage-b`构建期间发生源码写入，收据明确标为不可采用，不用于资格结论。

共享consumer索引的raw edit边界已收敛：不新增Inst dirty位、编辑包装或逐节点证明表，复用既有sealed SSA输入遍历核对用途occurrence次序、定义与重复项；核对成功的单元才借FunctionUses，其余保留显式Arg读取。两个实际sealed case覆盖mixed Lambda/Params与raw SetArg后的回退，共77断言通过；没有启动完整生成质量、PMU或FEX矩阵。设备私有ELF核hash、确认无活动进程后已回收。

证据：[六目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-b-r2/build.json)、[两项合同检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-b-consumer-check/execution.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-b-consumer-check/closure.json)。P2b/P1a及helper、连续consumer存储、发码buffer的后续源码不属于该冻结产物。

## 对实际工作量的补充

[冻结R7工作量账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compile-work-census.md)从已关闭的旧诊断重算：Block decode15,597次、append85,419次、final IR69,297条；Function decode29,580次、append147,841次，不同final/scratch/emitter覆盖分别97,360/82,819/81,859，不能混为同一分母。

FEX的255次CompileBlock是组请求，内部RA逐CodeBlock重新分配；本批没有记录其内部块和完整IR/operand数量。因此继续研究请求固定成本的分摊，同时保留独立Block RA；不把2415/255或header尺寸比当作提速倍数。当前先落实无需扩大decode范围的实际buffer容量复用；完整请求分组还涉及安全预读、组内取消、代码回收与多入口发布合同。

## 第三次集中整合

本批真实收敛了以下工作，均属于中间实现，尚无新的普通compile或完整生成质量结论：

- helper职责分离：前端保留decode与portable guest语义；ARM64汇编/NEON和物理ABI归backend；runtime编译portable REP/SSE/XSAVE/X87实现后，删除对整个decoder库的反向链接。两份汇编迁移逐字一致。guest profile当前只迁原先依赖host的crypto/SHA部分，CPUID与opcode读取同份配置；Unknown使用portable普通ABI，尚无后续native-helper重绑定。
- 字面整数提交：非LOCK Add/Sub/Cmp/And/Or/Xor在LoadImm出生前调用既有纯策略，supported路径不创建该常量节点/输入边；关闭早期选择、不支持和Unknown仍构造SSA。窄NZCV、ADC/SBB和LOCK保留原处理。
- consumer存储：dense块的raw/resolved consumers改为offsets＋连续buffer，同consumer重复Params合并；compact/sparse/incoming语义保留。它删除逐definition的小分配，但新增count/fill两次输入读取及临时cursor，净compile收益仍须测量。
- scratch首片：普通Select上界2；EA公式同时覆盖direct-dst和旧EmitOperand，宽base＋宽index上界1、两个窄host输入仍为3，特殊/未识别域保留default。reload、全局reserve和安全ladder不变。
- 发码容量：每线程最多一个≤256KiB idle MacroAssembler，重入独占另一实例；完整reset清理两种pool-owned literal及临时索引，保留实际codebuffer容量。borrowed assembler不回池；Function对象先销毁translator，再逆序释放context，owner最后退役。它不是请求分组或共享code allocation。

首次构建定位到新SSE共享TLS的`constinit`定义缺少显式初始化；加`{}`后，`build-stage-c-r2`六目标全部通过，前后source manifest一致，source为`68f77bd96f075804c06cd429ff6c6e3211f817450c358e4a6e37b5a8ee782ba5`。generic Release/ThinLTO未使用PGO。实际主程序为`2b17ee03…17948c`，测试为`1a92f213…37b4bf`，已独立保存。

只执行一批七项相关合同：立即数提交及解释器回退、Select/EA完整发码、dense/compact/incoming alias用途、SSE调用合同和buffer增长，共421断言通过。没有跑完整生成质量、PMU或FEX；设备私有ELF核hash、确认无活动进程后回收。完整fallback、重入/线程退出、guest语义与最终运行/compile门仍在后续集中验收范围，不用这七项代替。

证据：[构建收据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-c-r2/build.json)、[七项合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-c-contract-checks/execution.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-c-contract-checks/closure.json)、[helper交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p2-binding/stage-c/README.md)、[consumer存储交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p3-consumer-storage/implementation-review.md)、[buffer源码交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p5a-emission-buffer/handoff-root-review.json)。

## 第四次集中整合

本批首先落实稳定存储和实际节点删除，紧凑payload留在下一片继续实现：

- IRStore拥有生产指令页、alive位图、session和最后lease回收，替换原先不会真正归还的大TLS Inst arena。NodeId为32位稳定身份，RPO仍为独立编号；raw/stack手写Inst保留普通语义。同Function的Block共享Store，多region和跨Store移动保留原始域并由粗粒度lease保活。未给每个Value增加共享指针。
- 当前真实布局仍是95B Inst、96B指令槽加16B外部header，合计112B/node，另有page位图/目录。这比旧96B槽更大，是支持后续迁移的中间状态；不把生命周期收敛冒称内存或性能收益。固定Arg[5]、独立FunctionUses rows/positions仍须继续清理。
- Params删除逐参数Node/slab和尾插链搜索，以连续、几何增长的单次存储保留混合立即数/值、顺序、move/adopt与失败原子性。ValueWorklist保留原处理次序，普通存储指令不再按Inst*排序查row；DCE在解引用前通过handle判活，raw路径保留成员检查。代价包括按Store extent初始化的u32目录、较大的handle队列与少量域lease，不能只报删表。
- 已知后端的单次4/8B scalar内存访问在GetAddress/VexAddress出生前选择可编码地址。真实VEX prefix事实参与排除判断；EarlyOff/Unknown/地址模式不支持时沿原generic路径。共享地址、vector、atomic、segment/地址覆盖等不强行合并。本片同时修复ARM64原native绝对地址使用xzr作为memory base的错误：Rn31实际为SP，现在先物化完整64位地址；cache版本更新为53。
- 无spill且已有所有scratch change point都落在reserve内时，验证直接消费这些profile，避免全IR逐条重复查询；fixed约束和溢出时的原路径仍在。
- 整体退役时清除terminal与loop roots，避免保留指向已退役定义的terminal。缓存命中须先于fresh IR准入；合并region由HIR wrapper保活实际persistent owner，secondary原header保留原有guard。

`build-stage-d`因新增const bitfield读取方式错误失败，收据保留。修复后`build-stage-d-r2`六目标通过，source为`f66f6a1e6cc3f62df133d39114bdc1da46d74e80f4daf945317c719a70bbe15e`，前后manifest一致，generic Release/ThinLTO无PGO。实际主程序`b96f3b2f…c147206`、测试`110e0459…237dd3`、IR测试`7bdedf3e…a8c8a4b`及lifetime测试均独立保存。

这一批只跑新增合同，实际结果有通过也有失败，未宣布整批通过：两项scalar地址与native绝对地址合同通过；Params adoption和原Function DCE case通过；construction、thread-exit、prepared-regions通过。新worklist case把仅统计本地consumer的接口误当跨块查询，已调整用例布局而保留正反断言；region-owner用例迁移时只复制旧addresses、漏掉新handles和第二Store lease，已补齐；cached secondary region暴露Module只有primary node而canonical entry独立发布的边界，缓存getter已改为借实际Function owner解析所请求入口。这三项修正尚不属于D-r2产物，待下一次集中构建后验证。

设备私有三份ELF核hash、确认无实际活动测试进程后已回收；Magisk日志上报进程携带的命令文本未被当作测试写入者。没有执行完整生成质量、PMU或FEX对照。

证据：[D-r2六目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-d-r2/build.json)、[定向检查原始结果](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-d-contract-checks/execution.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-d-contract-checks/closure.json)、[Store交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p3-store-owner/implementation.md)、[worklist净成本与合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p3-stable-worklist/implementation-review.md)、[内存操作数交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p2b-memory-operand/README.md)。

## 第五次集中整合：紧凑参数与最终选择查询

本片继续实际表示迁移，使用独立冻结源码及产物验证，不借第四次产物作证明：

- 删除Inst中的固定Arg[5]；保留现有schema和公共语义，24B内联payload只编码实际字段，少数超出形态持有普通heap存储。Inst恢复自然对齐，源码以64B对象／80B managed slot为静态合同；外部16B身份header仍计费，不把64B说成完整分配成本。
- same-origin Store的常见cast（VOID/U8/U32/U64/V128）由kind tag表达，只存完整NodeId32的4B；其它cast为NodeId32＋原始cast byte的5B，raw/foreign保留完整9B指针视图。默认cast继续跟随producer结果类型，不能把当前Type()冻结成显式cast。由同一constexpr映射派生编码、长度与还原，没有截断ID或新增每节点表；三参数CallLambda由26B降为23B、五值CAS128由25B降为20B，可留在内联空间。内部有效引用直接按唯一Page布局定位，跨回调／stale队列仍用session/handle受检查询。这里增加了page间接读取及tag映射，压缩本身不构成净提速证明。
- GetArg按typed字段直接读，ArgKindAt只读tag；ArgAt返回可复制、只读、无所有权的语义snapshot，Params仍是借用view。前后端只查种类的reader已迁到tag查询，旧auto&与返回类型断言随之迁移。普通value遍历和schema核验各用同一byte cursor，不为每个物理槽构造通用Arg。
- 所有参数写入先核新引用并一次预算／reserve，再释放旧occurrence或Params、直写typed字段。SetInst以旧opcode退pseudo计数后完整替换；raw SetArgs仍只覆盖提交前缀，SetArg仍允许构造后续准入拒绝的空洞。分配失败须保持旧opcode、类型、参数、次数与incoming Params owner；新增集中合同覆盖这些真实异常路径。
- overflow容量计入Store::ReservedBytes，Params独立容量仍单列。DCE／bulk retire实际释放overflow；自然Owner布局可能增加每Block的少量padding，也纳入后续完整内存账。当前FunctionUses的producer复制及row尚未迁移，不能宣称唯一operand/use owner已经完成。
- 最终宽度选择原先每个ZeroExtend线性扫描全unit的residences和narrow，形成E×(R＋K)查询。现在在最后追加之后为同一owner数组一次准备有序查询，原Seal不再重复排序；同ID residence的OR、home冲突、narrow唯一性及原选择优先级仍保留。旧manual增量路径在未准备时沿线性查询，memory等尚会追加的数组不提前冻结。详见[P6审查与实际改动](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p6-completion-consolidation-review.md)。
- 五处只读scalar/publication/reload计数直接遍历occurrence，删除不必要的临时Values物化；Rewrite/DCE需要跨编辑保存旧输入的snapshot仍保留。这没有改变次序、重复用途或旧输入反馈合同。

同一阶段还收口merged parent的publication范围：单region的Prepare/借用可以合法成立，但不能据此发布或退役整个共享parent。uncached runtime路径需要完整parent覆盖及原canonical owner在首；cached命中仍解析所请求region入口。primary也保留既有persistent_owner标记；冷重试借O(1)的HIRFunction::Contains逐块核覆盖，普通fresh请求不加这次块遍历。发布／JitCache／退役使用同一actual owner，不扩展成任意regroup功能。[风险与实现交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p3-store-owner/publication-group-admission/source-ready.json)保留原scope。

集中构建先修正模板auto返回声明顺序、只读cache/lock const接缝及一处RV读取迁移的条件括号。`build-stage-e-r4`六目标全部通过，前后manifest一致，source `0fd5ddae469f877fff289ec56784ef86747dd78e4bb5d161c8a035029b35f108`，generic Release/ThinLTO无PGO。64B自然Inst及80B完整managed slot由实际编译静态断言通过，页28,704→20,512B；相对重构前96B槽的净差为16B，仍要加实际spill／Params／用途表／页空洞和allocator开销。主ELF为`e056a5c3…15bfc5`，已独立保存；host translator的.text从D-r2的4,606,120增至4,853,620B，不能只报节点缩小而忽略代码体积成本。

只执行一批必要合同：四个runtime/地址/宽度case共403断言，五个snapshot/Params/worklist/通知/DCE case共995断言；construction（12次实际注入失败）、region-owner、prepared-regions三个寿命mode全部通过。D-r2的三个失败点已修复并重验；不据此宣称完整生成质量合格。设备私有三份ELF核hash、无活动测试进程后回收，closure关闭。没有运行完整生成质量、PMU或FEX矩阵，当前仍无新的普通运行／compile性能结论。

证据：[E-r4六目标与ELF身份](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-e-r4/build.json)、[九项合同与寿命结果](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-e-contract-checks/execution.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-e-contract-checks/closure.json)、[紧凑参数净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p3c-inline-payload/implementation.md)、[ELF区段成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p3-compact-payload/elf-sections-d-e.json)。


## 第六次集中整合：用途关系与实际写入口融合

本批不新增平行IR。typed payload仍是producer/type-view唯一权威；Function用途拓扑按origin Store与完整NodeId直接持head，RPO只修改独立order view，删除旧producer副本、currentId→row和重排时第二份目录。独立Block保留按需、不同epoch次序的用途视图，不能把两种分配范围混成一种。

已安装／脱离Block但仍在Function scope内的节点，SetArg/SetArgs/SetInst由同一实际写入口维护用途及活动Rewrite反馈。范围改写只准备和摘除变更槽，未变Params/occurrence保留；完整消费者次序按原语义重新挂尾，不再重新描述每条边。opaque Change只保留目标身份与旧flags，callback里对其它节点的直接改写立即反馈，退出时安全完成目标次序；删除target或整块也不能再解引用旧指针。只在活动worklist或已seed的跨块DCE确实需要时，才对变更范围保存同步旧输入snapshot。

同一scope借用借助真实owned writer和完整成员状态；raw/manual未满足条件继续显式读Arg。managed detached节点通过粗粒度Store scope找回writer；owned raw节点复用原16B分配prefix，stack/unowned借用不读prefix。scope销毁先清Block及仍活owned raw绑定，Store lease不延长已经销毁的用途service。raw class delete按物理来源判别，不能把raw_scope当成Store退役。顺便恢复紧凑payload迁移前由Arg成员隐含保证的Inst不可复制／不可移动合同，避免复制sole-owned overflow或Params。

本片同时收敛ARM64最终阶段的三个确定重复项：flags-only spill记录并入现有scratch occurrence visit、独立Block借既有完整FlagsDemand（Function仍用local scope）；flags token直接借PublicationAction owner，删除临时Inst* vector；edge查询直接借已排序block roster，删除复制locations。失败scratch attempt在普通无诊断路径首个失败即返回，诊断模式仍收全失败；reserve/pool/ladder不改，不能把这一项称作局部资源修复。

用途元数据并非所有维度都更小：当前head为16B/issued extent、order key为8B/ordinal、occurrence为24B；写入draft约16B/input，page-address目录16B/page。新增目录计入Store资源账，large Params仍可能有瞬时draft分配。删除重复producer与查找／扫描，不等于已经证明内存或compile净收益。

`build-stage-f`保持源码稳定但因store.h对未完整Inst使用std::launder失败；inline定义已移到完整Inst之后，检查保持原样。`build-stage-f-r2`六目标全部通过，前后manifest一致，source `67b4cb2574a53d34c65641cad38e078b5f288e5793d434a6d273d4779719f36e`，generic Release/ThinLTO无PGO。主ELF `5c2a0aef…a9c6af`已独立保存。

红魔仅跑这一批必要合同：三项后端case／402断言、六项用途／raw／opaque／Params case／116断言全部通过；construction（12次实际注入失败）、region-owner、prepared-regions三个寿命mode通过。没有执行完整生成质量、PMU或FEX矩阵。设备私有三份ELF核hash、无活动进程后回收，closure关闭。这个结果证明本批接缝，不证明普通compile已达标。

源码维护成本也单列：用途融合的18文件切片净增898行，用于统一写入、范围增量、生命周期及raw兼容，并非总源码减少。后续优先削减生产路径的实际表示／遍历，不继续为少用冷模式扩包装层。

证据：[六目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-f-r2/build.json)、[定向合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-f-contract-checks/execution.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-f-contract-checks/closure.json)、[用途融合实现及净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/f-use-fusion/implementation-review.md)。


## 第七次集中整合：整数load、扩展与r32写回合成

本批继续减少值的全流程工作，新增的只是两个显式语义opcode与一个现Imm中的轻量descriptor，没有第二份target plan：

- `ir/load_extend.h`规定精确access字节、符号扩展目标字节以及最终32/64位整数结果。unsigned descriptor规范为直接扩至result；signed允许先扩至32，再清高位到64。因此`movsx eax,m8`与`movsx rax,m8`不同。`GetMemoryAccessType`统一访问宽度投影，ReturnType只描述结果，不能决定range check、TSO对齐或fault访问字节数。描述仍占既有Imm的实际9B，不把低4位编码误称为1B实际payload；Inst/managed slot布局不变。
- 已知ARM64的x86-64 guest，在MOV32、MOVZX32/64、MOVSX/MOVSXD32/64内存源出生前，经同一bool能力选择直接load+完整GPR写回，避免旧load→extension→r32清高位链。普通MOV64本来是一条load，保持原表示。Unknown、EarlyOff、cap关闭、寄存器源、16位部分写与32-bit guest保留原构造；地址能力与新load能力正交，1/2字节EA沿既有native gates开放。
- residual复用当前value/width epoch，只对相邻、同Block、唯一用途、无pseudo/隐式根且raw view宽度相符的load→extension成型。保持extension结果身份，显式退役旧MayFault load，不能等DCE而留下两次访存。不跨AdvancePC/effect边界；access8后截32、新load再Sign、非相邻shared/pinned链保留原fallback。root缓存未准备的冷配置保守不改，不新增逐节点根表。
- 解释器与RV64均实际消费新语义；ARM64 non-TSO合成LDRB/H/SB/SH/SW，TSO复用原acquire、unaligned、RCpc和fault-emulation分派，在公共成功continuation后扩展。地址、固定home与transfer继续借原owner，旧load+extension matcher仅服务generic输入。cache唯一版本升至54。
- 独立Block的mutable用途查询先用精确总引用数证明空答案，无pseudo时RequestedFlags直接空返回，避免为无答案查询建立整块反向图。确有用途的块仍按需构建本epoch图，不借别的分配范围或失效SSA视图。
- ARM地址matcher原把byte数传给要求log2的VIXL `IsImmLSScaled`，本批按真实access改为log2。该项扩大部分合法scaled地址接受范围，是单独的实际选择策略变化，不称纯等价文件迁移。

`build-stage-g-r2`六目标通过且前后source一致，source `57c2cc7c525181b588d4fdef5237845452d5317f47a255c645e445f107545878`；generic Release/ThinLTO无PGO。主ELF为`0860fa17…0053ea6`。必要合同为三个新增case／718断言和一个既有width case／42断言，全部通过：涵盖保护页面边界、实际signed32/64结果、TSO三类路径、四种目标绑定及相邻链的fault/root/view负门。首轮新residual测试误调仅Values入口，已改为生产同一All入口重验；生产源码未为这一fixture改动。未跑完整寿命或生成质量矩阵、PMU、FEX。

随后一轮compile阶段归因在首个Block点被真实合同拒绝：`invalid final low32 copy at IR 4`，对应CoreMark块`0x21a97c`。原点与单独失败前缀保留，未取得有效计时、未重标签为性能通过；Function点未运行。该接缝已在下述G-r5修复，最终校验保持。两次设备私有场均核hash并收口。P4只读审查也明确：late carrier最多省一条MOV，不能据此声称capture/interval/spill下降，不为它新造框架。

证据：[G-r2构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-g-r2/build.json)、[四项必要合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-g-r2-contract-checks/execution.json)、[前端交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p2b-load-extend-frontend/README.md)、[IR/portable/residual交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p2-load-extend-portable/handoff.md)、[ARM compile净变化](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/g-load-extend-arm-compile/implementation-review.md)、[失败归因点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-g-cost-diagnostic/execution.json)、[失败前缀](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-g-low32-failure/)。

### G-r5：保留guest home，完成一次阶段归因

实际witness显示，low32 Recolor把尚存活的独立guest寄存器home29提前用作parent SSA的物理位置。parent SSA在copy点死亡，并不意味着其原guest home内容可以提前改变。修复只在既有Recolor入口拒绝reserved guest/runtime home作为迁移目标；普通动态位置、LiveView/Transfer和最终宽度/owner检查保持。临时witness源码已移除。新增真实发码用例在完整12个固定GPR home下，从返回host的trampoline观察原寄存器和部分写后的目标，避免额外GetHost读改变原压力。

`build-stage-g-r5`六目标通过，source `fcae66cf0a8105004fd7a031573632ea6279969d041dfb734b06d733e7095335`，主ELF `adde8df6…43c3cfa`；新回归和两项既有low32合同共3 case／33断言通过。失败G-r2、临时witness和失败构建收据仍保留，不合并成通过记录。

随后只做一次Block、一次Function的现有阶段计时归因，均CRC `0x4983`、退出零，设备私有目录已核hash回收。下表是这一点的粗桶，单位ms；它包含诊断开销，不能与旧普通42点或FEX直接算新倍数。

| 阶段 | 独立Block | Function |
| --- | ---: | ---: |
| translate总计 | 932.815 | 1255.696 |
| decode | 159.959 | 343.983 |
| opt | 115.035 | 140.668 |
| regalloc | 235.915 | 301.942 |
| codegen（包含Complete） | 292.753 | 314.305 |
| publish | 39.596 | 52.235 |

嵌套桶中，Complete为184.710／190.891ms，planDeclare为74.649／80.022ms，HIR/Block append为31.189／86.802ms。它们不能再与父桶相加；regalloc减allocator也不能直接全部归为Describe。紧凑布局及少建IR尚未证明净compile提速，继续优先删除重复输入处理与physical completion结果转写，不按表数量或单点相邻比值预记收益。

证据：[G-r5构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-g-r5/build.json)、[low32必要合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-g-r5-low32-check/execution.json)、[修复边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/g-low32-admission-fix/)、[阶段账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-g-r5-cost-diagnostic/accounting.json)。

## 第八次集中整合：出生提交与最终位置owner

- HIR fast append从源C++参数直接填充既有用途`Prepared`，再写typed payload并提交同一birth；删除先写payload、再解码收集输入和再次定位definition的过程。普通fast构造原本就跳过完整Inst验证，本片没有冒称删除该验证。分配预留仍先于Params转移、计数与用途发布；raw/manual、slow构造和mutable setter保留各自合同，无新增节点字段或第二张输入表。
- completion的反向write扫描直接产出dead/coalesced候选及fault-capture需求，删除next-write目录和后续capture整IR扫描。用途索引已准备时借raw consumer位置判定publication是否复用；未准备时仍查实际后续输入，不强行为此建立全块用途图。后一分支也可能用于普通独立Block，尚无覆盖率或净收益结论。
- version分析直接向最终AllocationWriter登记base use-home，删除FixedHomeUse数组、排序和再转写。重复occurrence计数、同consumer最后type-view、selected优先级和preservation拒绝保持；不同physical policy由不同writer拥有输出，未添加任意writer的事务回滚承诺。
- SemanticFacts成员查询复用HIR owner已有的受检membership ordinal，删除逐查询全Function roster扫描，不新建索引。

`build-stage-h`六目标通过，source `fb6b06419c32b3e7761c9d589dc66141c11f9f355ba823de5665363f83bbad26`，前后manifest一致，主ELF `80edc136…74c41e`。必要IR混合参数合同70断言、construction的12个真实分配失败点通过。首轮swift_test合并filter退出1且无输出，不能当作已运行case；改用明确前缀后实际执行9 case／33785断言通过，其中包含五项预定合同及四项同前缀narrow读合同。

`constant-window`资源门槛在普通Block的第257个节点触发真实IRStore新页分配，不经过本片HIR birth入口。原fixture暖8个节点后要求再追加256个节点完全不分配，与第四批起的每页256槽冲突；测试已将必要节点容量增长与bounded immediate-history分配约束分开，不扩大生产页、不新增缓存、不关闭全局失败注入。迁移后的运行结果随I组收口，在此之前不宣布该项通过。没有运行完整生成质量、PMU或新FEX对照。

证据：[H构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-h/build.json)、[出生合并](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p3-typed-birth-merge/handoff.md)、[publication净变化](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/h-publication-use-owner/implementation-review.md)、[原始合同收据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-h-contract-checks/execution.json)、[实际swift合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-h-swift-filter-check/execution.json)。

### 一次原生采样的用途

H immutable ELF各采一次Block/Function调用栈，cpu-cycles:u、目标400Hz、DWARF8192，不传PROF/PROF2。样本752／793，零loss，部分调用链错误；CRC4983、实际目标退出零，设备私有场已回收。Describe inclusive占整个进程user cycles约8.26%／10.60%，Complete约8.60%／7.40%；包含guest执行、未知JIT PC且父子重叠，不能相加、视为纯compile百分比或重算FEX倍数。只用于选择下面一组代码审查，没有追加采样验证小百分比。

证据：[采样解释与范围](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-h-native-cost-sample-r2/analysis.md)。原首次部署未运行目标即因目录owner停止，空目录独立清除，失败收据保留。

## 第九次集中整合：现有端点与身份查询复用

- `BlockUseEnds`在已有用途visit内，同时记resolved/implicit端点与raw普通输入端点；每个local槽仍为4B的两个显式u16，点来自Inst最终编号，不截断NodeId。新增代价是coarse Block指针/order标记和每次occurrence的raw更新。raw查询只回答sealed、有序且由当前index确认pointer会员的本块定义；foreign同ID、detach、无序与mutable输入无答案，回原实际window，不以零充当无答案。terminal/hoist只参与原resolved端点。
- publication gate先保留已有raw consumer图分支；尚未建图时只借`FindBlockUses`的已准备结果，删支持域内逐publication／alias的后续输入重扫。未准备则继续原fallback，不为这条gate创建新表或reverse graph。物理位置、版本与最后type-view义务不归该语义端点决定。
- typed输入预算共用同一次nonnull/mutable与实际origin检查、现有compact kind选择和用途提交。same-origin NodeId只省身份查找，仍进入同一个Registered/BornValue/Visible、当前op/type与最终order资格函数；同Store未注册不是会员。raw/foreign保持原查找、missing输入及失败预留合同，无新字段／输入表。
- Function worklist从一次现有Users范围辨别有效空范围和未登记定义，删除先FindDefinition再Users内部重复Find的过程；未登记仍沿原manual扫描，独立Block按需用途服务保持独立。
- Function frontier先排除`target<=start`再算DecodedEnd，并在同次FindSplit复用首个选定owner的end给正常／ambiguous provenance。无per-block端点缓存、不使用decoder.pc或NodeSize，重新decode的范围验证仍重新算。历史r8曾因当时Block负控疑问未采用该局部候选的记录保持；本轮按成组实施重新纳入，未冒称已有提速。
- immediate-history fixture只在实际IRStore尚未发出当前节点时允许backing容量增长，并核实ReservedBytes增长、节点及list各加一；原失败钩子持续armed，身份推进后的history分配及scope析构仍必须被捕获。保留264个常量、两种scope和其余failure家族，无生产扩页或缓存补丁。

I首次统一构建在publication_windows新增调用处缺少SemanticFacts完整类型头而停止；仅补齐该consumer头文件后继续同一固定缓存的增量构建，失败收据保留。`build-stage-i-r2`六目标通过，source `18b9f90d6a3c0485e4f5f8619a10634ba0a5f9a0c5e78bfffd3b81df078e471a`，前后manifest一致；generic Release/ThinLTO无PGO，实际主ELF `6b6547f0…b97602b`。

本组仅执行受影响合同：frontier七项／64断言、IR四项／227断言通过；construction报告12个真实分配失败点，修正后的constant-window报告217个真实失败点，均通过。后者同时核本组use-home资源边界，未扩到完整寿命或生成质量矩阵。三份设备私有ELF核hash、确认无实际活动进程后回收，namespace已关闭。没有追加普通性能或FEX采样，不宣称净compile改善、最终生成质量达标或≤2×已完成。

证据：[raw端点净工作与回退域](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/i-raw-use-endpoints/implementation-review.md)、[输入身份与Users](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p3-input-identity-reuse/handoff.md)、[frontier边界审查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/i-frontier-local-work/review.md)、[资源fixture迁移](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/h-constant-window-capacity/README.md)、[I-r2构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-i-r2/build.json)、[必要合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-i-contract-checks/execution.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-i-contract-checks/closure.json)。

## 第十次集中整合：共享语义site与单次改写

本组没有增加新的pass或源文件层级，连续落实以下实际工作：

- 既有`FixedValidationInputs::Site`保持8B，用剩余三bit记录GetOperand、control及helper，包括clobber为零的helper。分配描述先形成这一owner，再供早期固定分配与completion借用；替换六处early、三处late全块条件候选筛选。普通同features的GPR/helper/scratch描述复用同visit标量，确有不同policy的冷请求另算。移除normal preserve路径的controls/helpers两个临时vector；它们原来每块构造一次，不能误称原有逐publication平方复杂度。
- sealed dense member借同一site span；其它输入沿实际roster顺序。跨Block的ID互斥来自最终编号和入口SSA，不是`SitesFor`独立认证任意手工域。fault successor反向访问、first-candidate顺序、物理冲突及最终fixed/width/scratch检查保留。新增代价包括更多site行、spare capacity、每次span边界查询及一个8B coarse borrower；不把所有Complete成本算作已删除。
- `LoadUniform(Uniform, FlagsIntent)`默认Unknown保独立身份。x86 R的明确data读取传None；只有offset0、请求/视图/producer return均为U64的精确Alias才直接借原值。FPR原策略、窄/partial/dirty-high与宽化view负门保留，无frontend ARM依赖。这是有意少建整数BitCast的B类改动，不提前声明机器码等价或运行改善。
- Operand读取从一次payload prefix开始，用同一cursor读op/left/right，继续调用原typed reader和tag校验。双data非Void由三次定位改为一次；常见right Void原本只定位两次，因此是二改一。没有offset缓存或节点字段。
- `RewriteContext::SetInst`与原SetArg同形，直接委托现有whole-node原子setter。十一处已知单次pass callback及builder私有shim迁为直写，避免Reset／opcode／SetReturn的中间状态和末尾重复通知／rehang。相同SetReturn原本是no-op，Rehang也已有sequence快跳，不能夸大每次固定删除三次完整遍历。通用opaque Change及公开HIR回调仍支持跨节点、移动、自删、异常和次序合同。
- 修正最后可变闭合：初次清理与选中hoist prefix后的新epoch复用同一value/DCE/flags循环，先保新roots/lease，不重跑state/sink/hoist。无选中anchor不新建epoch；选中但原本就在front也可能仅新增prefix/root metadata，仍如实计新的候选探测／必要roster成本。新增用例证明hoist移开常量后新暴露的load+extension会在seal前成型，cap关闭保generic。
- Function replay仅在现有PROF2中加两类reset/重解成本计数，关闭时不取统计lease或遍历IR；未实现额外commit锚、raw sidecar或prefix缓存。源码审查确认保prefix需要真实guest commit边界，合并后的AdvancePC及decoder.pc不能替代它。

`build-stage-j`六目标通过。审查发现新接线在逐节点循环中比较完整FeatureSet，提到块外后`build-stage-j-r2`再次通过，source `d665f8efb76af08cedcf51073746c54a5269c8d1475f6a1d582f074f0e429692`，前后manifest一致；generic Release/ThinLTO无PGO。实际主ELF `9e083f07…6d5f2ae`。定向检查为Swift三项／64断言、IR七项／244断言，construction的12与constant-window的217个真实失败点均通过；设备私有测试场核hash后关闭，没有扩完整生成质量矩阵。

### 本组归因与下一步依据

同一J-r2 immutable ELF各采一个Block/Function CoreMark2000，均退出零、CRC4983，实际argv/env/CPU4/前后身份保留，私有场关闭。加入RA_DIAG/RA_SHAPE与已有PROF/PROF2后，本点translate为909.850／1212.325ms；这不是ordinary比较，不能与G点或旧FEX算新速度比。Complete为171.068／177.752ms、pass_const为38.096／61.635ms；父子范围不能相加。

Function共237次split reset和104次late-entry reset，均成功；两类销毁8011／9139个旧IR，重解分别发出3320／1681个Store身份。reset＋prefix重解及已有边界收口共15.469ms，约本点decode的4.61%。新发身份不是旧prefix精确P，append与final的差额也不是全部replay损失。这个约1.28% translate范围不支持优先新建逐guest commit锚，下一组先处理两个分配模式共有的规范化与重复分析。

当前静态发射shape为Block 2415单位、44 spill单位、679 defs、223 loads／135 stores；Function 436对象、13 spill对象、269 defs、55 loads／33 stores，overflow均零。reserve7为18／4；预算失败中已无原Select/GetOperand，剩SelectZero、BitInsert等。这是当前单点的发射计数，未作精确host bytes归因或动态运行对照，不用它宣布CoreMark回退已修复。

下一步IR重点为共享浅层整数形态事实，复用真正已规范化的64位值进行窄data read及宽度归一化；禁止每次read递归32节点或把窄Value view当作physical high-zero。分配重点按实际emitter审SelectZero/BitInsert精确定价与合法mixed宽度，保安全ladder。完整生成质量、资源与两模式分别≤2×FEX仍未验收，不标目标完成。

证据：[共享site净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/j-shared-fixed-sites/report.md)、[data转发](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/j-exact-u64-data-forwarding/handoff.md)、[Operand游标](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/j-operand-read-cursor/handoff.md)、[原子改写](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/j-direct-whole-rewrite/handoff.md)、[末次闭合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/j-final-mutable-closure/handoff.md)、[J-r2构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-j-r2/build.json)、[必要合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-j-contract-checks/summary.json)、[成本账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-j-cost-diagnostic/analysis.md)。


## 第十一次集中整合：少建规范化值与一次规则调度

本组继续删除两个分配模式共用的工作，没有新增pass、源文件层级、缓存、逐节点证书或字段：

- `AnalyzeIntegerShape`只读当前definition及直接输入type，经原宽度语义取得结构事实；LoadMemoryExtend/TSO的descriptor leaf由浅查询和原有界full分析共享。data读取仅在offset0、显式None、source view与真实return都U64，且被丢弃位全部已知零时复用窄view。typed宽化与x86 r32写归一消费同一`FoldWidthAlias`，命中时decode不再出生extract和重复zext两个节点；Unknown/flags输出、dirty upper、AH及窄producer宽化view保持原路径。
- residual先保原memory-fusion及DirectSource选择，再用同一别名规则；保结果身份为BitCast，由原DCE清无用extract，不宣称该路也删除两个节点。BitExtract读取raw scalar slot，区别于conversion的logical view；同owner raw事实入口保U64 literal窄view的完整bits、窄literal按实际return掩码，其它窄producer先Unknown64，部分常量折叠因此保守减少，未改解释器oracle。
- value/width对同一初始候选依次执行，闭合后仍按实际list顺序共享常量，再只消费真实反馈。删除active阶段字段、SeedWidths及其全roster再次排队；原首anchor/8节点CSE窗口、NodeHandle退休、用途通知、implicit roots与8N+32预算保持。width提前会改变中间形状和预算分配，未声称最终同码。
- BitInsert普通整数路径按result裁字段、统一W/X，源与result同位置时先保存；源归一与保存共用最多一个tmp，destination直接写，零/越界字段也保证narrow/同W结果规范。GetResult整数raw-copy、capture原visit的真实宽度需求与HasKnownWWrite同步，不能用窄view替代完整容器。纯slot投影普通numeric为O1，仅实际rawcopy链有有界walk/循环拒绝。vector/FPR-origin旧域尚未纳入这项保证；full-cover source-only小优化仍未做，不把它记作收益。
- 原ScratchBudget统一定价：普通非空BitInsert GPR1/FPR0、空字段0；Select/SelectZero计flags merge、VIXL mask及直接U8/U16宿主条件抽取。MEM transport继续另计，global reserve/pool/ladder和最终检查保持。cache唯一版本升55，退役旧整数raw-copy/BitInsert发码。
- residual state的preserved-fact全表count只服务PROF2/IR dump，普通诊断关闭时不再执行；实际Write/Invalidate/DSE没有随这项计数门控删掉。后续将收敛唯一字节表owner，避免已空再全表清空。

`build-stage-k`六目标通过，source `6b1176a071f691d166cb02929f1ce6769f47840ff17747050d3e5276ad0bf9d9`，前后manifest一致；generic Release/ThinLTO无PGO，immutable主ELF `ed97d779…8a530dd`。集中检查仅Swift五case／1511断言与IR八case／8372断言，全部通过；其中既有selection oracle另报告12288 programs／552960 checks，不冒称完整生成质量矩阵。没有重跑寿命全套、PMU、ordinary compile或FEX对照。设备私有两个ELF核hash、确认无实际活动进程后清除，namespace关闭。

以上仅证明本组构建与受影响合同，未取得新的普通compile倍数、最终生成质量采用或≤2×结论。下一组先把GuestStateForwarding的活表与变更收回同一owner，再删除已空重复清除；ARM侧继续核实际多份publication描述的合并，不追加过渡包装。

证据：[浅层规则交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/k-normalized-scalar-forwarding/handoff.md)、[整数slot与scratch净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/k-scalar-scratch-implementation/report.md)、[单次调度](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/k-one-value-worklist/handoff.md)、[state计数门控](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/k-state-diagnostic-only/README.md)、[K构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-k/build.json)、[必要合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-k-contract-checks/summary.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-k-contract-checks/closure.json)、[下一组净工作审查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/k-next-compile-review.md)。


## 第十二次集中整合：唯一字节表与publication描述owner

本组按较大范围连续整改，不逐小片采质量/PMU/FEX，实际完成以下收敛：

- `GuestStateForwarding`收回唯一活字节表与分支快照，外部只读const span，static mutator与caller重复活表删除。保守may-have-facts由成员Write/Invalidate/Initialize/Merge维护，已空的full/range invalidation跳过填充，非空range不额外扫表求精确占用；known-empty meet仍返回真实Merge(0)并退役对应snapshot，missing-edge不混为成功。Reset仍只忘snapshot；所有mapped专用refill迁同owner。Initialize先完成可能抛出的准备/reserve，再用nothrow复制/赋值提交，失败不谎称空表。复用原residual的StackVector256内联容量，不新建第二buffer或每block heap；builder则增加2560B内联payload预留（ByteFact为10B；M实际编译纠正此前误写的约6KiB），不能称零成本。无layout/size0/builder_uniform关闭不填槽；Unknown/EarlyOff保原logical forwarding。
- 同一GPR publication在原SetHost访问内形成一个稀疏记录，RA与completion借同owner；删除GPRWriteInputs及其capture数组、WidthInputs.publications、WPublication及其range目录，以及Finish::DescribeWrites二次描述。long-chain仍保必要拓扑，legacy fixed-publication的extension-first优先级独立保留，通用6B base表也不强塞大对象。
- 共享记录实际40B，保存stored/produced真实指针及各自edge view、真实instruction与roster position、完整u32端点/窄load身份和角色位。没有为32B目标互推position/ID或丢实际type view。净payload是40U相对原32A+24W+6P+4C，三类重叠域收益与单角色增大并存；BlockRange由72B降48B，vector容量尚未测量，不把它写成request/RSS已净降。
- early/late真正相同的结构事实和已准入observer窗口才复用；feature/key、实际block/index/dense门保留，mode0仍可走较广late matcher。审查发现窄memory component可把proof_start前移到load，早期producer→store证明不覆盖该早段，已保回该路径完整source→store检查。物理冲突、liveness、width、fault/recovery及最终消费检查未取消。capture读从已有shared sites/actual roster取得，包含原all-offset读，不再复制数组；width-root所需coalesced capture home按每个physical call归约一次。
- 最终base-facts同次SSA visit复用既有TracksInputs结果，仅对reachable、完整normal输入且raw==resolved、实际同Block的locality判定省重复FindDefinition；alias root、cross-block、implicit与manual/partial仍回原路径。schema/SSA/Finish保持。普通Block删掉PrepareBlock前相邻重复SupportsIR，唯一准入仍先于OptimizeBlock；HIR ownership/publication前检查保留。
- builder宽度折叠改用O1浅形态，raw BitExtract仅literal走O1 raw事实、完整64-bit definition走浅形态、其它先Unknown64；conditional只在所有直接Value叶均为LoadImm时早折。深值树留现residual worklist，复用同一纯宽度规则；不加depth策略、cache或pass。pending、flags输出合同与immediate原append位置保持。删除的是早期未命中树的深查询/重复探查，早期成功的深折叠会转到residual并增加相应候选，不能把整个decode桶记作净删除。另将只服务lowering诊断的operand分类移到原诊断开关内。

**阶段差异继续明确保留。** builder原先不受int_imm_fold/ConstantFolding开关限制的深折叠，改浅后在关闭residual的冷配置可能留下更多合法树。K的full64 identity alias也会移除原width深查询触发点；孤立shift当前不是standalone常量候选，可能保留LsrValue，即使完整facts可证明0。原路径的alias/事实断言仍在同case，另用确有截断意义的width操作验证residual最终深折叠；没有靠改opcode隐去缺口。下一组通过共享O1代数/shift规则补该覆盖并继续少生IR，不为每个shift增加32-node深查询。最终生成质量采用门仍未通过，不能凭本组合同宣布完成。

### 构建与必要检查收口

首次L构建在generic lambda的dependent GetArg写法停止，仅改具体Inst指针参数；r2在新sparse fixture的const list view上SetId停止，仅改为AppendInst时保mutable pointer。两份失败收据保留。r3六目标通过，source `34feaea1…ffd774a`。

随后18个必要case实际执行：Swift三case通过（17断言），IR十五case中十四通过，一个legacy ClearedInput fixture在表重命名后清错字段而失败。只改为清fixed_publications，原mapping负门断言保持；r4六目标继续通过，source `d81f58db190a1e2904250e5aa0d9039cc5b43ff527d278c52714de87a1f09ed5`，前后manifest一致。r3→r4唯一源码差为该测试文件，生产main完全相同：`37a2e1ec…07b6650`，generic Release/ThinLTO无PGO。只重跑失败case，其31断言全部通过，其余17个不重复。两次设备场均核hash、确认无实际活动进程后清除；原失败不改写成通过。

本组未跑完整寿命、生成质量矩阵、PMU、ordinary compile或新FEX对照。仍没有当前源码达到≤2×的证据，目标保持未完成。

证据：[唯一字节表](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/l-state-owner/README.md)、[publication唯一owner净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/l-single-publication-owner/report.md)、[publication独立复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/l-gpr-publication-peer.md)、[最终facts资格复用](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/l-final-facts-identity/handoff.md)、[构造浅折叠及覆盖缺口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/l-builder-shallow-folding/handoff.md)、[r4最终构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-l-r4/build.json)、[两次必要合同完整账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-l-contract-checks/summary.json)。


## 第十三次集中整合：共享标量规则与直接最终动作

本组继续成组实施，已完成统一构建和受影响合同检查。以下记录实际机制、失败修正与净成本边界，不作为性能或最终生成质量验收。

- builder 与 residual 共享 O(1) 标量规则：相同 definition/整数 view 的 Xor、Sub，零掩码 And、全掩码 Or，以及按结果位宽归一 count 后可证明为零的逻辑右移。仅显式无 flags 输出的构造可提前折叠；Unknown/实际输出维持身份，residual 保原节点身份并校验实际 schema。复用既有 literal 才真正少一个 producer；返回新 Imm 仍需 LoadImm，不能按命中次数宣称少节点。LSR 进入原 worklist，补上上一组 identity alias 移除 width 触发点后的覆盖缺口，不给每个 shift 添加深树分析。
- literal 读取按实际 producer ReturnType 还原存储整数，而非按 use view 截断，也不直接把窄 LoadImm 的完整 Imm 当成有效值。新规则与原 builder/residual 的 target-immediate 转换共用这个投影。U8 producer 的 UINT64_MAX 只能提供 0xff；U64 producer 的窄 view 则仍保实际完整 slot。复用现有 literal 的门仍比较真实未掩码 Imm，避免复用带脏高位的物理结果。规则未命中后进入旧立即数路径可能再次做常数时间投影，未为此新增缓存或 checked 字段。
- GuestStateByteFact 拆字段的提案已被实际构建否决：首次M构建的 static_assert 明确给出 `16 <= 9` 为假。Value 已在既有 pragma pack(1) 内，原 ByteFact 是9B Value加1B offset，共10B；拆成自然对齐字段实际增长到16B，原“24B降16B”的静态判断错误。本组撤销这个表示变更及仅为它新增的接口/fixture，不修改 Value 的既有布局。保留独立的空快照优化：已知空分支快照用既有空 vector 表示，Merge 必须清除分支后重新填入的 fallthrough facts，缺失快照仍是不同情况。此前L记录中约6KiB inline开销同样不正确，实际256项 payload 为2560B。
- ARM64 已把 GPR residence 与零扩展的多份阶段描述收回最终 UnitActions，删除 GPRResidences 和只剩 writes 的包装。物理 width reproof、fresh owner/clobber 扫描与真实 fallback 保留。审查发现分阶段向全 Function 有序 zext 数组插入会引入最坏平方搬移，已改为生产 Begin 后借现有 width sites 预建最终6B槽，后续 lower_bound 原位更新，Seal 裁默认 Local 空行。实际代价包括未命中候选的6B槽及一次有序性检查，手工冷接口缺槽仍保有序插入；没有新建镜像表或 ready 字段。不能把仅删除中间表视为净成本下降。源文件及 fixture 迁移收齐后统一构建，集成失败仅修正具体错误后增量续建；没有按小片分别运行测试。

完整质量、寿命/资源和两个分配模式分别不超过固定 fresh FEX 两倍的最终门保持不变；本组尚无新 ordinary compile 倍数。

首次M构建已停止，source `5faa6652747acf920efdf328b15237eb148d47a8bd94831dea1b57256dd4e1ad`，manifest前后一致；失败原因是上述ByteFact布局断言，不是运行测试。原收据保存在 `build-stage-m/build.json`，不覆盖。修正后另建M-r2收据继续统一构建。

M-r2布局恢复后继续构建，在 `plan_build.cpp` 新调用 `SemanticFacts::FindIndex` 缺完整类型头文件处停止；仅补 `analysis/semantic_facts.h` include。该轮 source `1753214e84bb8bcec3d4adbe8a33e9939325a9b0f67519db36e43fc76f90f882`，前后manifest一致，失败收据保留；M-r3继续使用同一固定构建缓存。

### M构建与必要合同收口

M-r3六目标构建通过，source `d763a0c750c8177e1cec4c4feb49c15cda08c577f1d6ff8faed78ba44765f59c`，manifest前后一致；generic Release/ThinLTO无PGO，主ELF `a1dd344390d3d914059ab1922705f4fbc7e19bba11e0730ab4d21b107e07e835`。

红魔 `bf1359ff` 仅执行本组受影响合同：Swift九case/140断言，IR四case/5577断言，全部通过。覆盖scalar leaf/实际literal slot、LSR残余候选、空snapshot清除refill、阶段可见性、最终zext传输与跨block同ID隔离。没有运行完整生成质量、寿命全套、PMU、ordinary compile或FEX矩阵。两个设备ELF核hash，无活动进程后清除，私有namespace已关闭。

原拆字段布局提案已撤销，不保留空包装/getter或相应专用fixture。最终actions仍会为未命中zext候选预留6B槽，Seal删除默认行但容量高水位仍计费；新增ZeroExtend32候选可能增加实际8B shared site。删除重复表不等于这些成本为零，也不能用本组构建或合同代替最终≤2×门。目标保持未完成。

证据：[共享标量规则](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/m-scalar-canonicalization/handoff.md)、[ByteFact错误提案撤销及空快照保留](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/m-byte-facts-compact/correction-restore-original/README.md)、[最终动作净工作账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/m-action-emission-owner/report.md)、[fixture迁移](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/n-owner-fixtures/README.md)、[M-r3构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-m-r3/build.json)、[合同执行及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m-contract-checks/execution.json)。


### M后下一组选择：复用现有阶段诊断

K/L/M三组实现后，各取一个Block与Function诊断点，避免继续仅凭J旧热点选工作。沿用现有collector、同一CoreMark 2000输入与CRC4983，启用既有PROF2/RA_DIAG/RA_SHAPE；无完整质量矩阵、无fresh FEX分母。这不是ordinary倍数或J→M收益报告。

|本点ms|Block|Function|
|---|---:|---:|
|translate|868.864|1028.900|
|decode|152.097|277.378|
|opt|94.312|78.982|
|regalloc|231.023|241.379|
|codegen|268.295|297.278|

codegen中嵌套complete为166.617/178.263ms，plan_declare为62.951/70.436ms，plan_perblock为52.219/58.562ms；不能把子项再加回父项。下一组主要审分配输入准备、publication版本与consumer事实的重复描述和遍历，避免继续只缩末端小表。

本点静态spill计数Block为1037 defs/439 loads/271 stores，Function为646/222/143，高于J记录；reserve7单位从J的18/4增为30/10，而spill单位数为36/13。现象集中于少量单位走高reserve，尚不能精确归因到K/L/M的某一条改动；按用户节奏保留为未解决质量风险并分析现有日志，不以此中断重构或立即启动完整矩阵。

原始结果、环境、argv/PID/hash与闭合账见[阶段诊断](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m-cost-diagnostic/analysis.md)。两个诊断点均正常退出，设备场已核hash并清除。


## 第十四次集中整合：用途端点与活跃版本单一存储

基于M后实际诊断，本组不继续缩末端包装，推进两个重复工作来源：

1. 在既有base facts/SSA输入访问中收集必要的raw/resolved用途端点，区分ordinary、implicit、physical与Function raw alias；正常路径删除已有Definitions×Users重走，manual/partial保原校正。Block端点保持按需，不以B×unit表换扫描；首次候选前缀回补、额外初始化及稀疏排序都须计净成本。
2. 在RegisterStatePreparation内合并per-definition版本容器、home定义名单和resident计数查询，用同一活跃条目池表达两类索引。Android NDK实际类型探针确认旧ActiveValue24B、每定义small_vector72B、home容器56B；新候选槽12B、entry28B、home目录8B。失效条目复用free链，不能按历史publication无限增长；resident计数生命周期保到BeginBlock，active池仍在原准备scope结束时释放，不转移计时边界。保持同definition重复view最后一次优先、strict publication比较及fault capture顺序。

Function正常路径的资格由现listed节点完整归属和FunctionUses真实consumer集合计数共同确认。新增8B/service的集合基数，在原Consumer位的提交/移除/销毁处同步；它不缓存优化判决。unreachable或额外借入consumer、partial/manual继续原Users校正。raw alias校正复用原recorded_ends，每alias定义一行，输入访问lower_bound原位max，不按每occurrence追加临时行；初始化及查找成本单独计费。

本组已完成下述构建和受影响合同，尚未作收益或最终质量判决。P1b局部物理修复暂不混入：现SpillReload仅利用空闲reg，不能自行减少scratch缺口；需要明确victim/slot/前后缀和fault/fixed/implicit合同。默认eviction路径后的base3也不能直接删除，它可能是首次无forced-spill的完整分配，与此前相同reserve数值不代表同一状态。


N首次构建在owner复核发现异常路径后主动停止（exit130，源码前后一致），没有把中断记成编译成功：新Block端点collector若途中分配失败，半成品不能留给下次按已有缓存复用。仅加当前collector的ScopeExit回滚，正常Finish后取消，已有完整ends不清除；随后N-r2从固定缓存续建。原收据与停止原因见 `build-stage-n/{build.json,stop-reason.json}`。

### N构建与必要合同收口

N-r2六目标通过，source `4ffc31de9cba07432ba22715cac3753b17852e648bbb29046481703f984740fe`，manifest前后一致；generic Release/ThinLTO无PGO，主ELF `e30bee4ac7963c1bcec7172e533fb205ac31774965af22299b0f871db489c525`。

仅在红魔执行Swift六case/151断言、IR五case/322断言，全部通过。覆盖最后operand view、home覆盖与helper/fault边界、raw detachment/consumer计数迁移与bulk退役、raw alias/implicit/unreachable/manual/sparse用途端点。两个ELF核hash、确认无活动进程后删除，私有namespace已关闭。没有运行完整寿命/生成质量矩阵、PMU、ordinary compile或FEX对照。

真实净成本仍有限制：正常Block若首候选很晚，前缀回补可接近原operand遍历；Function新增每alias定义行初始化与每alias输入二分更新；用途owner新增8B集合计数。active-version的12D槽保存到下一BeginBlock，可能大于旧16K稀疏resident计数尾部留存；pool为28B/峰active，初始reserve D、结束D槽头重置都计费。删除旧72B/definition small_vector、重复home名单与count线扫，不等于所有输入的峰值和耗时已经下降。

证据：[IR端点与生命周期](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/n-ir-use-collection/handoff.md)、[活跃版本owner](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/o-active-version-owner/report.md)、[独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/o-active-version-peer.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-n-r2/build.json)、[必要合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-n-contract-checks/summary.json)、[spill/ladder剩余分析](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/next-spill-work.md)。整体重构与最终两模式≤2×门仍未完成。


### N后普通compile规模核对

复用现有collector各取一个普通粗计时点（未开PROF2/DECODE/RA_DIAG/RA_SHAPE）：Block738.946ms、Function883.259ms；其中decode88.370/178.902、opt84.848/70.494、RA207.645/231.179、codegen250.285/272.698ms。实际环境及无prof2输出均核对，CRC4983，设备场已清理。只有单点且无频率准入或fresh FEX分母，不能报告正式倍数，也不与M诊断计时相除认领收益。证据见[普通小样本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-n-ordinary-pilot/analysis.md)。下一组仍需删除普通热路径的大块工作。


## 第十五次集中整合：签名内参数合同与分配输入工作收敛

N普通compile小样本仍为数百毫秒，本组继续删除普通路径工作，而非仅调整最终动作容器：

- 将现有常见scalar参数domain纳入同一ir.inc签名描述，按现ArgSignature的u8位域表达，原storage/optional语义不变；ResultTypePolicy保原source并表达既有integer结果限制。顺游CheckArguments已解出的Value/Imm直接校验domain，删除HasValidScalarArguments末尾同family重新读参数。rare跨参数field/descriptor规则仍由原owner检查，StoreUniform宽度比较复用顺游局部宽度。要求不增加IRMeta/Inst尺寸、不改变生成C++ API、也不扩原合法域。
- DescribeBlock同一次输入访问收集reload occurrence计数及必要scalar分类，删除scratch中的第二遍计数。正常sealed/dense、同feature请求用当前block常数大小状态及共享Fixed sites表达publication前后边界，逐候选producer→store和live suffix的重复真实IR扫描退役；手工/foreign/sparse/不同physical policy保原扫描，不能把site未覆盖的observer当成不存在。publication索引只在最终过滤后建立。
- LinearScan普通构造后立即AllocateRegisters；构造期那次FixedClobbers表在InstallInputConstraints前没有消费者，删除这次初始化及只包它的InitializeTargetInputs。正式表仍在安装约束后构建，四个绕过AllocateRegisters、直接调用coalescing的manual helper显式调用既有private初始化，不新增cache或epoch状态。

本组已通过下述集中构建和必要合同；尚未作新收益判决，不把整个RA输入准备计时都记为可删成本。

窗口快路径的净工作账包括132B前缀位置状态、168B临时反向位置数组及8B当前block site起点；删除HostWrite容器及逐publication窗口扫描，但增加一次共享site反向访问、当前tail压缩。cold narrow-read回退由读×write名单检查改为读×实际instruction扫描，保留手工非单调ID语义，不能宣称所有冷路径更快。过滤在LongStep/legacy索引形成之前完成；后缀拒绝的rich row同时恢复原live_end/narrow_load默认值。

按用户要求，过程中只做必要构建及受影响合同；整组完成后再安排生成质量和性能验收，不以每个小改动的质量波动中断重构。


### O构建与必要合同收口

六目标一次构建通过，source `8482d8f8bed57f7d2c2c11f3a66e842941379adf668fbac2c6b3c78301c312af`，manifest前后一致；generic Release/ThinLTO无PGO，主ELF `8924f15f6b9a52dc38f964f5e40ebf04329dfa494b7e9f3b9deefa8d2000cc8b`。IRMeta/Inst等布局断言随真实Android构建通过。

红魔仅执行Swift三case/426断言、IR八case/5157断言，全部通过。覆盖scalar签名与既有合法域、sealed/dense窗口正反例、SaveFlags例外、mutable/sparse回退、窄读tail顺序及manual coalescing初始化。独立peer复核窗口变换未发现阻塞问题。设备两个ELF核hash并清除，确认无活动测试进程，私有namespace已关闭。

没有运行完整生成质量、寿命/资源、PMU或FEX矩阵；本批没有新compile收益或≤2×判决。整体重构与最终验收仍未完成。

证据：[签名合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/o-ir-signature-domain/handoff.md)、[输入访问与窗口净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p-input-visit-fold/report.md)、[分配初始化](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ra-attempt-init-once/README.md)、[窗口peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/o-input-window-peer.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-o/build.json)、[必要合同及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-o-contract-checks/summary.json)。


## 第十六次集中整合：可变优化与用途编辑的重复工作

继续P2/P3主线，未启动新质量/性能采样。本批先解除Function DCE对ValueWorklist临时roster的身份依赖：DCE以已有stable handle/coarse store lease及raw membership独立维持队列，value epoch创建/销毁不再清空它。实际正常顺序是flags cleanup先seed、value cleanup后开始，因而可删除后者强制的一次完整Function重扫；不能把epoch销毁后通常不再运行的DCE也算成第二次必然收益。Block仍按自己的反向消除算法运行。异常seed/drain仍可重置重建，手工raw在实际删除/bulk退役时注销。

另外两包源码已收口：FunctionUses复用真正Unlink的occurrence，并将partial edit的新边合入同一次最终顺序挂链；bulk Discard仍挂旧ring的tombstone不复用。Reserve先补齐真实free槽，避免提交期分配，新增槽初始化和保守预算留存都计成本，不声称峰值等于max-live。x86条件值构造复用CheckCond精确0/1结果，删除SETcc的额外1/0/Select节点；已知carry直接选择同义CondSet，Unknown/PF及CMOV读序保原语义。三包已通过下述集中构建和受影响合同；本批不代表最终性能或质量验收通过。


P独立只读复核已收口：DCE pending-live/stale任务跨value epoch，uses真正脱链槽与Discard墓碑区分、partial输入顺序均未发现阻塞问题。仅在既有stable-domains case补队列跨epoch，前端既有条件case保25组CMP并补2组ADD，覆盖0/1、AH/内存窄写；用途编辑直接复用原mixed Params/回调用例，不新建资源观测getter或测试矩阵。


### P构建与必要合同收口

六目标构建通过，source `ca36343e29427928c3ff554643df2024e0c854728aecda0650a23792cbe18cac`，manifest前后一致；generic Release/ThinLTO无PGO，主ELF `4b21e1d506db28ceb6d5c29e2f34c01f4b17c4555f83e1fa0d129713056c44ed`。额外既有 `swift_narrow_carry_test` 目标在同一source上补建通过，没有源码修补或重跑整批。

首轮Swift一个case/62断言、IR五case/170断言通过；SETcc filter误放swift_test，Catch输出未匹配而进程仍exit0，因此首轮只记六个case覆盖，不将未匹配项记PASS。核对CMake后，独立carry目标只运行该既有case，在 `SVM_FLAGS_REGS=1/0` 各通过两断言（内部保原25组CMP、另两组ADD，核六类SETcc整byte及保留bits）。最终七个不同case、八次case执行通过；原未匹配输出及coverage修正记录均保留。没有扩展成完整ADC/SBB、质量、寿命/资源或PMU/FEX矩阵。两次设备namespace均核hash、确认无活动进程后关闭。

这批实际删除Function一次常见重复DCE seed、同数量用途改参的尾部slot追加和partial新边重复挂接，以及SETcc/已知carry冗余节点。free槽预留仍有初始化与保守容量留存，Block的DCE算法仍独立，且尚无新ordinary compile收益或≤2×判决。整体目标保持未完成。

证据：[DCE净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p-dce-owner/report.md)、[用途编辑及容量边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p-ir-use-edits/handoff.md)、[条件直接构造](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p-front-next.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-p/build.json)、[必要合同及补测闭合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-p-contract-checks/summary.json)。


## 第十七次集中整合：无输入变化的编辑通知与状态位直接构造

继续P2/P3。Block按需用途图当前在每次BeforeChange/AfterChange中整consumer拆建，返回类型、flags及纯立即数范围也如此。本批从真实setter的旧/新参数kind提供是否可能改变输入用途的信息，跳过确实无拓扑变化时的影子图工作；不为每条ValueUse增slot字段或新建一个owner。Touch、flags需求与DCE通知仍要完成；Function用途注册次序合同不在本包中取消。

前端另推进CarryValue/CMC和POPF直接位构造：已知carry极性选择现有TestFlags/TestNotFlags；POPF用BitExtract直接生成实际所需宽度，PF/ZF保留明确flags producer；Pop的fault与RSP提交、Sign/AF语义、Unknown carry恢复均不弱化。本组已按下述证据完成构建及必要合同，尚无收益判决。


Q预审发现一个需单独闭合的backend边界：ARM64 `EmitBitExtract` 最后直接 `Ubfx(result, context.R(value),...)`，窄U16/U32输入的W视图和U64输出X视图不能直接搭配VIXL编码。故本批POPF窄输入保留既有ZeroExtend64（其合法alias折叠仍由原规则决定），只对原U64 Pop直接复用；bit结果统一U64，DF继续原NarrowTo(U8)。未用cast伪造零扩展，也不把解释器/RV的支持视为ARM支持。通用跨宽BitExtract发码应另作完整语义修复/定向合同；当前新增构造不会进入该未支持形态。


### Q构建与必要合同收口

七目标一次构建通过，source `13110ac6f3d70dba3ece389374ee78d2f34c9a0b1dcc63daeefc30f266846c9d`，manifest前后一致；generic Release/ThinLTO无PGO，主ELF `a773d3840ae721c4ca9042b531946d3d0e8594f7a33d92efd6b36db663d2272b`。期间用户暂停后恢复，续查并等待原构建session，没有另建或重复启动。

红魔Swift两case/1716断言、IR四case/122断言通过。新增POPF/CMC一个四程序case在 `SVM_FLAGS_CFINV=0` 及 `SVM_FLAGS_REGS=1/0` 各通过两断言，覆盖真实Unknown/Inverted/Direct carry与POPFQ/W的flags及RSP字节数；不称新增fault-injection测试。共七个不同case、八次case执行，三个ELF核hash并清除，无活动测试进程，私有namespace关闭。

实际净账：metadata-only通知只在已有observer内检查旧新kind；无需observer的普通birth不新增该扫描。Operand运算符标签不携边，左右各自Value标签仍保守判断；Lambda/Params仍按可能含Value处理。跳过Block已materialize影子图的整输入拆建与无关旧输入快照，保留Touch、flags及target DCE反馈；FunctionUses次序、opaque Change和retire合同未改。POPF窄模式少建13节点/15输入、U64少14/16，保窄扩展与SF/AF/DF规则；这些是构造工作计数，没有据此推算compile提速。

证据：[编辑通知](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/q-metadata-edits/handoff.md)、[状态位直接构造](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/q-front-direct.md)、[独立前端复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/q-flags-direct/final-peer-ir-middle.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-q/build.json)、[必要合同及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-q-contract-checks/summary.json)。最终质量、寿命/资源及普通两模式≤2×FEX仍未验收，整体目标未完成。


### Q后一次归因采样与下一批选择

连续N/O/P/Q后仅取Block/Function各一个阶段诊断点（不带完整质量或FEX矩阵）。本点translate847.335/1023.392ms，decode149.555/277.624、opt94.731/76.430、RA220.319/230.212、codegen262.773/299.696ms；均带PROF2/DECODE/RA诊断开销，无频率准入或配对，不能作为ordinary对比或本批收益。Complete仍161.478/179.512ms；RA detail外内差120.752/131.826ms的实际边界已按Block/Function分别核对，不把它一概叫inputs，也不重复减live_scan。

局部节点/通知改动没有提供足以认领最终数量级改善的证据；下一批转向Function reverse用途的延迟构建、分配输入重复visit及Complete的整体工作合并，先按现有owner和消费者逐一闭合。已核StableIdentity/FindAlive/compact local解码为inline O1、普通HIRFunction单store，不将空raw/cold域扫描当主热点。

首次collector因遗留旧namespace guard在启动前拒绝，原收据保留；修正后两个点正常退出CRC4983，设备场已核hash关闭。证据：[Q后诊断](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-q-cost-diagnostic-r2/analysis.md)。


## 第十八次集中整合：延迟用途拓扑与实际spill消费点

Q后归因继续显示大量成本，下一批不再把O1身份访问的小调整作为主线。本批三个完整owner包已通过下述构建和必要合同，尚无性能/最终质量验收：

- Function reverse topology：公共默认保持Eager，生产translator显式选择Deferred，和ir_fast检查策略无关。Unbuilt保真实definitions/Consumer/Normal/sequence、lease和IR精确计数，但不外借Complete；在有效mutable优化边界或最终Seal前，枚举所有live normal Consumer（含detached），安全Project现payload、重算missing并按原sequence一次建现有occurrence/ring。Ready后沿用既有增量维护及Discard墓碑；不在const Users或Begin/Commit中暗中分配，不养第二份graph。必须计live payload扫描、临时consumer顺序Key及E_final建图，不能仅挪计时桶认领收益。
- SpillInputAction：当前Complete对全IR再访值、建16B consumer/definition rows并在Seal排序，实际消费者只在JitContext有pending GPR write时才查询一对。将该同raw身份、重复次数、MEM、last-use证明推迟到实际消费点，删除全量owner/投影/sort。保required_backing/FPR/dirty/deferred/reload/fixed门，保flags-only特殊证明在原静态指令访问中。foreign同ID冲突仍拒绝，不能用旧id-only helper直接替代；要计pending writes×当前consumer参数数的实际代价，不声称整个scratch阶段消失。

RegisterReadUses的目录/operand扫描已融入PrepareBaseFacts既有Definitions loop及checked输入访问；保BitInsert真实stored-width、pseudo/terminal/hoist语义，不以普通liveness末端偷换。不增加OptimizedUnit请求入口或新表；仅首个GetHost按需创建同一稀疏owner，SSA成功后才发布，mutable/manual或收集不完整仍用原冷constructor。


### R构建与必要合同收口

首轮source `049380426798ec57a32594a4b2c351cac4080484065e7013c560da1e27018b5b` 的生产代码编译通过，在旧backedge测试的ScratchActions聚合初始化器处停止：退休inputs字段仍留一个空占位。确认原构建terminal exit1、manifest不变后，仅移除该占位；原失败收据与一行修正快照保留，不覆盖。R-r2七目标通过，source `071d01c0fb06f918071a6acd8ec508c6f87dc705df683f23caf5eccb980bf698`，前后manifest一致；generic Release/ThinLTO无PGO，主ELF `129300ce8cb1c72bd7267c011457a6d61c81b53585bcf9cd5eb357ad2972721d`。

红魔Swift七case/1033断言、IR四case/235断言全部通过。Swift组显式 `SVM_FUNC_BASE=1`，复用既有INC/DEC carry译码执行case核实际Function生产入口；spill cases分别覆盖normal sealed查询、manual回退、重复/foreign/raw身份、flags-only以及deferred/faulting consumer必需backing。IR组覆盖Deferred到Ready的顺序/opaque callback/detached consumer，以及sealed捕获宽度融合和sparse回退。只运行既有lifetime `construction` 模式，433755 checks、13个注入失败通过；本包只增加一次部分materialize失败与retry，没有运行完整寿命矩阵。三个ELF核hash并清除，无活动进程，设备场关闭。

边界明确：Deferred是受控同步writer策略，公共默认Eager；禁止setter通知回调跨已Reserve/Begin draft激活拓扑。现首slice在有候选的Function ValueWorklist前或最终Seal前准备，不声称严格等到实际Users首查；C个consumer按8B Key排序、live payload投影与E_final条24B occurrence建图仍计费。捕获融合保原view先读再Resolve及BitInsert raw-slot规则，后续Imm的kind/domain先守门，坏schema仍由原SSA拒绝；cold两visit保留。PendingSpillInput普通路径按现有效证书/只读唯一编号合同只扫当前参数，manual先核实际roster，潜在成本分别为W_pending×E_current与W_pending×(N+E_current)。flags-only proof、scratch/range及最终Reprove未删除。

本批没有新普通compile、PMU或FEX对比，不将计时桶迁移、表尺寸变化或所删行数当提速证据。最终质量/资源和两模式≤2×FEX门仍未通过，整体目标未完成。

证据：[延迟用途拓扑](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/r-function-use-topology/handoff.md)、[用途生命周期独立复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/r-function-use-topology/peer-front-binding.md)、[捕获需求融合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/r-capture-demand-fold/README.md)、[spill owner删除](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/r-spill-input-owner/report.md)、[初始化器迁移](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/r-integration-fix/reason.json)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-r-r2/build.json)、[必要合同及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-r-contract-checks/summary.json)。


### R后普通规模与一次调用栈归因

普通粗计时各取一Block/Function：translate722.674/862.437ms，decode89.439/168.025、opt87.695/86.044、RA195.872/211.668、codegen239.667/261.881ms；实际环境无PROF2/DECODE/RA诊断，无频率准入/配对/fresh FEX分母，不作N→R收益或正式倍数。CRC4983，场已关闭。

另各取一个cpu-cycles:u 1000Hz/DWARF16384原生调用栈点，不开启上述诊断，按实际目标PID过滤；1815/1849样本，record零loss。Describe inclusive为全进程user cycles的5.77%/6.64%、BuildFixedGPRInputs2.02%/2.71%、Complete7.20%/8.27%。含guest/JIT/runtime、父子重叠，不能当纯compile百分比或直接乘普通毫秒。新数据用于选择更大结构工作，不再从空raw/cold查找或单个小家族推测数量级改善。设备场及唯一原始结果已收口。

证据：[R普通pilot](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-r-ordinary-pilot/analysis.md)、[R原生栈](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-r-native-cost-sample/analysis.md)。


## 第十九次集中整合：64B managed slot与fixed successor消费

根据R普通点和原生栈继续P3c，已批准完整节点路径：Inst目标48B（16B原语义meta、8B正常NodeId32双链/cold指针union、24B原payload），现16B prefix补ownerIndex，目标managed stride64B。正常Block不再携16B intrusive指针链和8B Owner*；Store维护可复用的coarse owner目录，raw/cold经唯一ColdLink{list,prev,next}24B取顺序和owner，未linked stack/raw不读prefix。旧NodeId32、最终instruction ID16、Value cast、payload和lease/retire合同不改。首次normal→mixed promotion O(N)明确计费，之后实际插删/移动及iterator_to须保持O1；所有owner/cold资源在原append graph Begin前闭合。设计不能仅列16B/node红利：coarse目录/list额外字段、owner索引读取、ID链步进分支、冷promotion和真实page容量均须计净账；当前没有性能收益判决。

另已落fixed successor包：整轮BuildFixedGPRInputs无法直接硬塞入Describe，因为全unit transfer/narrow/capture/address声明和批result proposal有真实优先级依赖。本片保这些顺序，复用8B canonical sites表达相同successor observer/control事件，退役normal reverse-N roster及dense N pointer投影，仅同一稀疏owner保存真正following-write对；增加observer site行、W'排序和查询成本明列。FullRead是immutable ReturnType size8的重复投影，唯一验证消费者直接读原Inst，位域改作ResultBoundary。新路径须同features+sealed/dense，不满足则原roster；不混用先前GPR coalescer的observer策略。

P1b局部spill修复另留下一独立行为包：已有只读方案以MEM base＋现SpillReload保GPR前缀、真实独占slot及required backing，完整Verify失败回原ladder；必须闭合physical writer/width/home以及寄存器置换同步，不能只清dirty。当前S不混入该算法或新质量矩阵。

### S构建、净成本与最小合同

S首轮构建在重复的copy/move deleted声明处失败，源码前后一致；仅删后一组四行，保留原禁用合同。S-r2七目标通过，source `29e7e023b866a35c1067565c87352d9c92e76747230eaa5f732b1167fe6dfc24`，manifest不变；generic Release/ThinLTO无PGO，主ELF `f20f4ad7a9333b173badec24aff6cd84df7b276bbf0c05d67db86a3d212fd7d7`。实际项目头内断言确认Inst48、managed slot64；早先NDK微型probe只证明当时布局，不代替本次完整构建。

正常路径删除每节点16B，但增加coarse成本：InstructionOwner24→32B、order root16→32B、每Store目录16B×capacity、import记录16→24B、iterator8→16B。256槽Page为20512→16416B；slot仍align16，64B stride不等于cache-line对齐。normal步进增加格式/ID/页内判断，跨页增加目录读取，GetOwner改走prefix索引；这些不能由节点尺寸推断已抵消。cold首次约264B allocator-requested payload（不含allocator头/取整）；密集cold managed至少64+24=88B/live，加chunk slack可能大于旧80B，必须另计。

红魔最小检查：IR三个case/76断言和既有construction模式516076 checks/15个注入失败全部通过，涵盖跨页、正常/混合顺序、raw借用、owner复用与失败重试。Swift三个case中Function译码执行、hoist通过；successor新增native段在`REQUIRE(sites)`失败，原因是测试仍用默认重复ID建立index，SealIR只封口、不编号，生产sealed+dense门正确拒绝。保留原cold未编号覆盖，在native段前显式ReId并重建index；该修正未单独重建，随下一批集中验证，当前不报全绿。三个设备ELF均核hash、无活动进程后清除，私有场已关闭。

fixed successor保全unit提案优先级，normal借既有canonical sites，稀疏following-write pair为16B/实际pair，增加排序/二分查询；observer-only行可增加8B/行。`ra_coalesce=0`不额外记录只服务result-home的boundary。未运行新ordinary、质量、PMU或FEX矩阵，不认领compile收益，最终两模式≤2×及质量/资源门仍未通过。

证据：[节点与顺序净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/s-packed-node/handoff.md)、[fixed消费审查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/s-fixed-input-review.md)、[S-r2构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-s-r2/build.json)、[最小合同与真实失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-s-contract-checks/summary.json)、[fixture修补](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/s-successor-fixture-numbering/)。

## 第二十次集中整合：局部spill修复与剩余主路径

P1b首先只开放独立Block的完整unforced reserve3映射：实际GPR缺口为1、FPR不缺，最多尝试一个普通U64同块victim。复用现MEM base、真实独占slot与SpillReload前缀，required backing由原emitter写入；完整Verify失败仍回原ladder。special/fixed/width/home/alias与跨CFG不在首片准入，不用Block结果替代Function；所有额外候选检查、prefix资源和失败成本计入。本包不运行新的生成质量矩阵。

同时闭合Q记录的ordinary scalar BitExtract边界：读取真实raw slot，按slot/result裁有效位，再选择同宽W/W或X/X编码；保原合法X编码及完整64位恒等省略，普通narrow alias不能漏掉numeric结果的清高位。只补一个含narrow/raw-copy/alias/MEM的定点case，cache版本56，已随本批构建和必要合同通过。

多块Function的CFG endpoint seeds已并入最终SSA/operand visit，使用既有FunctionValueFlow owner；后续仍执行原needs去重和CFG传播，删的是原完整operand/implicit重复解码与alias解析。sealed且有SSA检查、RPO>1才融合（已checked的sealed manual也在内），其余保原collector。creation-order验证不变，仅另带原RPO owner标签；pseudo/raw/implicit区别不混。两处失败路径清未发布seed，不破坏entry_values。新增vector控制对象、reachable u8→u16的净+B、4N ends/needs更早与SSA scratch重叠均计费，不称零内存收益。

Complete物理消费包已实现：publication proof、fixed writers与width owners有不同窗口/早晚物理状态，普通bulk publication只在最后mapping编辑后使用同一GPRPhysicalChecks owner，早期memory-value caller保即时原证明；后续fixed/width检查共用同一visit。原三种谓词/端点保留，不以一张过早的共同表偷换；真正替代的writers/consumed投影才退役。额外pub-only homes的owner、本块home-mask visit与最终sort均计成本，不以关闭合同、新包名或目录整理认领数量级改善。集中验证范围与尚未通过的最终门见下文。

### T集中构建与必要合同收口

物理消费包已完成：`verify/widths.cpp`退役，原实体直接扩为`verify/physical_gpr.cpp::GPRPhysicalChecks`。all Declare后关闭mapping编辑，每块先收口bulk publication，保原token/select/memory/narrow次序，再用同一owner核fixed/width，随块释放；early memory-value证明保原即时roster。generic借domain0 sorted writer keys和原位排序的PhysicalGPRWrite，不依赖ARM width枚举；normal verification input与consumed-pointer投影退役，manual及独立audit保留。

不采用每块扫描全unit home index的试案，避免新增B×M工作；保廉价本块N的home mask，再做共同physical投影，无fixed声明时恢复原early return。zero-store仍在原Finalize全unit发布，当前width选择仅借同块proof子span，保后块可见性。points合并F+Q与pub-only owner会增加部分瞬时峰值；旧fixed/width向量原本先后存活，不能据“合一”宣称RSS下降。窗口、早期M×N、实际重叠候选和每请求对象创建仍计成本。

首轮source `36d916d6…`在迁移helper缺`analysis/uses.h`声明处停止，terminal exit1、源码前后一致；仅补显式include。T-r2三个目标（主程序/Swift/IR）通过，source `dafb475307e0f7e5f34e06602bc6ef8fd999febbbc3e6fd6d0e662fd57ce8542`，manifest不变；generic Release/ThinLTO无PGO，主ELF `29575b4b74c6a7ea6ec2b81bc5851424ddf320562ec30dbbf7d0b9aaf12b9988`，cache版本56。

红魔仅运行本组8个case：Swift6case/1755断言、IR2case/104断言全过。覆盖单缺口真实SelectZero分配、prefix STR早于suffix LDR及native结果、置换policy回旧ladder；共享publication及冷width drift负控；跨宽/raw/alias/MEM提取；多块CFG seed对照；一个真实Function译码入口。S编号fixture原case本次全过，原S失败及修补收据不覆盖。没有重跑已过的完整寿命模式或质量/PMU/FEX矩阵；两个设备ELF核hash、无活动进程后清除，场已关闭。

本批未证明CoreMark会命中受限局部spill修复，Function局部修复尚未开放；无新ordinary compile收益及≤2×FEX判决。整体目标继续，下一包转向构造阶段完整flags writer的延后决定/原位置物化，保数据SSA和Unknown路径。

证据：[P1b与准入净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/t-local-repair/report.md)、[Function seeds净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/t-function-flow-seed/handoff.md)、[物理消费与时点审查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/t-final-physical-consumption/report.md)、[T-r2构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-t-r2/build.json)、[8个必要合同与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-t-contract-checks/summary.json)。

### T后普通成本规模

只各取一个ordinary Block/Function点，未启动质量/PMU/FEX矩阵。T-r2 translate为746.679/931.395ms；decode91.451/181.491、opt93.149/96.231、RA198.212/213.724、codegen250.489/294.812ms。实际环境无PROF2/DECODE/RA诊断；CRC4983、argv/hash核验通过，cpu4 max前后均1171200kHz，未写频率，场已关闭。

这没有支持所需数量级改善；也没有频率准入、配对或fresh FEX分母，不能按R旧单点认领稳定收益/回退或最新正式倍数。T的IR ID slots仍为64743/76969，不能把槽位缩小、表合并等同实际工作量下降。继续推进U减少flags birth，同时单独审查S正常ID步进/owner目录和T扩展物理owner的跨阶段新增成本，不以既成目录或尺寸目标保留无收益实现。

证据：[T ordinary规模与资格边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-t-ordinary-pilot/analysis.md)。

## 第二十一次集中整合：构造阶段完整flags writer延后决定

已知目标通过公共`BuilderCapabilities::deferred_scalar_flags`提供能力，Unknown/early-build-off保持空能力；前端不接触ARM寄存器或发码表。首片覆盖canonical carry下32/64位普通寄存器/立即数ADD/SUB/AND/OR/XOR，保数据SSA。单个pending槽由后续完整writer直接替换，被覆盖的Save/Clear/Invert从不出生；CMP/TEST、narrow、ADC/SBB、partial和未支持族保原eager。

存活组按原producer这个精确前驱回填Clear→Save→Invert，publication和AdvancePC不移动；正常decode结束、终端/控制变化、flags/effect/fault/helper和重配入口先兑现，异常取消未物化槽。复用公共IR effects识别观察，不新造ISA屏障表。HIR复用原flags_since_advance保原marker，回填保存/恢复当前状态；MarkLocalNZCV保提示，TryLocalCondition在实际消费前兑现。SUB安装后立即给decoder原canonical最终Direct状态，recipe只保存原Invert指令，不在回填时改当前carry选择。

Root整合另使Try要求producer确实存在于当前GuestInstructionScope的原fold entries，并经原EditBlock取得mutable owner；不加逐节点信任位或伪use/root。IrBuilder现成员GuestStateForwarding已显式禁copy/move，pending槽不会复制给另一writer。存活组仍付构造、MoveBefore、coarse槽及每append的空槽判断/effect读取成本；延后birth也会影响原即时值历史窗的时序，不能按少几个节点推算收益。

先在Data2副本并行实现/peer，T终止且设备收口后核11文件before/after/hash再应用；源码副本使用当前mtime写入，避免旧mtime令Ninja漏重编。U三目标一次通过，source `dd4a919a1b37cdce75a00654b8c77734c2e90d1e68e20db993fcae31b646fb02`，前后manifest一致，generic Release/ThinLTO无PGO，主ELF `3a2050af360e5e4dfb27c6fe139778d382799966aa34c9955f30656e52d46982`。

仅跑3个定向case：两个原builder case40断言，验证实际Extent少出生、两原marker、原位组、fault兑现与异常取消；一个原译码case208断言，追加两段完整writer后直接ADC/SBB的短程序，保原carry场景。全部通过，两个设备ELF核hash清除，场关闭。未运行质量/寿命/PMU/FEX矩阵；没有新的普通性能采用或最终≤2×结论。

证据：[副本实现与成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/u-deferred-flags-staging/handoff.md)、[独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/u-deferred-flags-staging/peer-front-binding.md)、[builder不可复制证据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/u-builder-ownership-review/review.md)、[U构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-u/build.json)、[3个必要合同及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-u-contract-checks/summary.json)。

### V：publication 索引边际成本实验收口

同一U基线仅将普通构造的`publication_homes`置0，保all-Declare后的证明时点、fixed/width共同owner、完整扫描fallback以及U的节点布局与flags行为。两模式各两对ordinary点，Block按ABBA、Function按BAAB执行；实际环境未启PROF2/DECODE/RA诊断，无PMU或FEX矩阵，CRC4983及实际ELF/argv/env核验通过，全部原始点保留。

Block两对B/A为0.989227、1.012729，Function为1.001192、0.990636；四对中三对的编译单位/IR总量不同，不能据此归因或认领净收益，也不通过追加点或事后放宽工作量条件改变判决。维持HOLD，B不采用，继续保留正常publication索引。

已精确恢复整个U source manifest `dd4a919a1b37cdce75a00654b8c77734c2e90d1e68e20db993fcae31b646fb02`，main-only缓存同步构建通过、源码前后不变，主ELF仍为`3a2050af360e5e4dfb27c6fe139778d382799966aa34c9955f30656e52d46982`。未对相同已测源码重复合同；设备场和私有进程已收口，无活动采样。小表优化没有提供数量级改善证据，后续主线转为W的GPR决策归一，而非继续扩展边际采样。

证据：[V原始账与可比性](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/v-publication-index-ab/ordinary/accounting.json)、[采用判决](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/v-publication-index-ab/decision.json)、[精确恢复构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-v-restore-a/build.json)。整体目标仍未完成。


## 第二十二次集中整合：退役两条后置位置重选分支

W先替换完整的相邻scalar direct-publication家族：目标只在原publication行标记相邻raw source能力，公共RA在同一Allocation事务将实际MEM映射改为reserved GPR，并标记coalesced publication。旧native `MatchSpilledGPRPublication`、`PrepareSpilledGPRPublications`、Spill selection及其recolor→Assign overlay链均删除；没有新表或持久FixedDefinition。仍使用现事务的MapCount/alias/reload与必要released-home事件，不能把两个stack sites说成O(1)。仅普通sealed、dense、同features且真实producer→VOID SetHost相邻；更宽旧机会暂取普通Value发表，属于中间策略缺口，最终质量尚未验收。

同批删除后置transfer matcher/选择、whole-value suffix写口、PublicationAction::Transfer及JIT分支。必要unsafe transfer已有前置的唯一分段FixedDefinition，原纯matcher保留；source-safe/manual残余仍可普通发表和按真实guest版本取值，不再创造另一套全值suffix覆盖层。旧copy/select、memory-update等物理选择尚未收敛，不能称Complete已只读或全GPR authority完成。

新相邻路径的跨块incoming占用由现RecolorGPR的MapCount访问补齐：普通canonical home在begin之前出生且真实last read越begin时拒绝，preserved普通suffix以transfer为birth；death==begin保先读后写。全unit fixed index继续覆盖固定prefix/transfer。因本窗口只有producer和VOID发表，删除原局部prefix重复扫描；不把这个incoming-only门扩大成任意窗口证明，旧发表后GetHost新版本例外仍保留。

W首构建仅因u16 ID加一整数提升触发u32列表初始化narrowing而停止；确认terminal/source unchanged后单行修正。W-r2三目标通过。原七个定向合同六个通过，一个新相邻positive失败：迁移时沿用的旧压力池开放guest homes0..5，未满足该known-target能力的policy域。保留失败和设备收口证据，仅将positive改为六个non-guest值寄存器，原cold用例池不动；没有为迎合用例扩大生产准入。该修正随下一整合包构建，失败用例在W2真实通过，不覆写原记录。

证据：[W主替代及实际成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/w-gpr-authority-implementation/report.md)、[transfer退役](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/w-transfer-retirement/handoff.md)、[incoming独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/w-spill-direct-peer/review.md)、[原合同及修正后继](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-w-contract-checks/followup-stage-w2.json)。

## 第二十三次集中整合：删除初始fixed影子owner

W2实际删除 `InitialConstraints`、其4N value→entry目录、entries backing及第二home index；`BuildFixedGPRInputs`与普通`input.fixed`搬运/Install重放退役。Describe只供语义、target能力、clobber与scratch；原Prepare在同一请求直接向唯一AllocationWriter声明guest homes并Plan/Finish fixed。Function仍按全unit PrepareBlock→capture→address→result proposals→批提交原顺序，未偷换成逐Block分配。真正手工输入的显式`input.fixed.push_back`仍由generic Install处理；故意跳早期选择的既有fixtures改用SetAllocationUnit＋纯Describe，而非清已空模板。

异常经现SetAllocationUnit清半成品，成功后才发布ready；同一owner的demotion在physical retry保留，新请求清旧定义。正常trampoline已有guest保留集；冷调用原先free的home会更早进入既有declared-pool policy，可能影响中间预算/质量，未加临时恢复pool包装。旧entries是move到input.fixed的同一backing，删除不能冒称省了两份同时存在的vector。

真实placements及publication记录更早创建，可能抬高与Function analyses/input表的重叠峰值；初始home index从旧稀疏K entries改读真实N placements一次。地址修改后到result批提交前没有再Plan，不是逐publication重建。保留这笔净成本，不另造稀疏collector补回影子owner，不宣称RSS或compile收益。

W2三目标通过，source `73eec2e3514a520ebfbe6181ef11fada3391a6149b3b17c706f2ec6c60532afa`，源码前后不变，generic Release/ThinLTO无PGO；主ELF `8a233c819d389c8101ffc6799ff34409e2d60c0629b20d482191ac6bfb73c39a`。仅集中七个受影响既有合同：Swift六case/87断言、IR一case/26断言全部通过，含W修正的三形态positive、Function分段、故障窗口、压力与显式manual、retry/new-request边界。两个设备ELF核hash清除，namespace关闭。没有整体生成质量、lifetime、PMU、普通性能或FEX矩阵；Function fixture通过不等于整套Function workload已验收。

本段W2收口时，W3仍在Data2副本；其后已按下一节整合，替换“先spill/建reload、后RA recolor”的工作。最终质量/资源和Block、Function分别≤2×fresh FEX门仍未通过，整体目标继续。

证据：[W2替代与净成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/w2-direct-fixed-owner/handoff.md)、[独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/w2-direct-fixed-peer.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-w2/build.json)、[七个合同与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-w2-contract-checks/summary.json)。


## 第二十四次集中整合：在真实分配事件消费home偏好

W3不再先把相邻结果spill、建立reload，再做整unit MapCount recolor。既有preferences目录用明确CallABI/GuestPublication种类携可选home及实际publication ID，目标原Describe直接形成这一个输入；旧SpillDirect标记与post `CoalesceSpilledGPRPublications`整步删除。分配事件保持ABI→forced spill→guest soft affinity→原tie/ordinary pool顺序，未有preselected fixed的候选才可进入；成功直接写唯一canonical Map/coalesced标记并加入原active owner，不创建FixedDefinition或overlay。拒绝后普通分配，不另跑后置solver。

实际占用由现active/fast-heap、全unit固定prefix/transfer索引及原fixed-read目录共同覆盖。独立birth cursor只消费已出生且实际成功映射的read，32个端点用actual slot与alias_end；Function再合入已准备CFGUseEnds，避免遗漏已被剔出ordinary interval表的premapped read跨回边寿命。其余SSA/typed/pseudo、implicit terminal/hoist、原fault结果点与最终scratch/fixed/width合同保留，不将Function改为Block分配。

净账仍须计：Preference由4B变8B，每个新guest hint8B；仅为旧SpillDirect保留的40B publication行可不再出生，原有其它mode/flag行仍在，capacity不自动缩。attempt增加一个u32 cursor及32个u32端点约132B字段payload；偏好查询、每hint actual index查找及active扫描仍付费，没有常数时间或提速结论。root保原std::sort并比较(value,kind)保证ABI先，不为未承诺的同kind稳定性新增stable_sort scratch；浮点interval跳过scalar偏好目录查询。

三个目标一次通过，source `e38d22f50a22cf99d1e1b5354da577cbc6a134e0264d56cb0fec5e2756551b24`，前后manifest相同，generic Release/ThinLTO无PGO，主ELF `0c4e69f8c1fff60764b6a18caf4fd94bdb8a77585f387715ffd4fef4d6076184`。沿用本批七个既有filter，Swift六case/94断言、IR一case/26断言全部通过；仅在原Function collector case内补一个两块Function的实际hint/general-allocation正路径，保无fixed模板及home22/coalesced断言。没有新增测试目标或生成质量/性能矩阵，两个ELF核hash清除、设备namespace关闭。

这只是一个真实事件内分配家族。其后W4已与P1c合批整合并通过独立构建及必要合同，见下一节；W3产物不作为后两包的验证。剩余未覆盖的early hard result homes与late copy/select/memory-update决策仍待收敛。没有fresh FEX倍数或最终质量采用结论，整体目标继续。

证据：[事件内替代及成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/w3-gpr-affinity-events/report.md)、[输入语义与storage净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/w3-gpr-affinity-events/ir-input-handoff.md)、[独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/w3-event-affinity-peer.md)、[实际整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/w3-integration-apply/applied.json)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-w3/build.json)、[七个合同及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-w3-contract-checks/summary.json)。

## 第二十五次集中整合：正常soft路径闭合与同attempt GPR驱逐

W4使W3的soft affinity真正进入正常Prepare入口。原EarlyResultHomes借同一AllocationInputs的已排序preferences；仅当raw producer、home和publication ID精确匹配GuestPublication，且对应soft consumer启用时，停止为该值重复形成optional hard-result proposal。mandatory capture/transfer/address不动，遗留wrapped/非相邻等hard家族保原全Function优先级与批提交。GPRWriteSuccessors仍为原owner，但延到第一个真正遗留hard候选才构造；混合块仍构造完整原拓扑一次，不按每个soft候选认领一轮全块遍历。

既有三形态direct-publication正用例改走真实Prepare，并在RA之前断言没有result FixedDefinition，保原home22和实际机器码检查。纯Describe的冷wide-view及原手工hard fixture继续保各自合同。W4改变optional选择的优先级，不声称最终生成质量已验收。

P1c把普通GPR被选中驱逐后的处理移入同一次LinearScan。复用原整值use/next-use/end/ID成本比较，victim整个SSA根直接成为现canonical MEM；后续现BuildSpillReloads正常消费，不保留另一层GPR前缀或remap。actual active entry留在原end，只更新MEM类型及slot/counter；腾出的home直接交给incoming。Function仍用全局CFG端点及跨块active，绝不在块边界重置。fixed/preserved、pseudo、width/low32/AES owner、incoming raw/resolved operands等保守排除，FPR仍走原forced-spill restart。

回溯整值到MEM不能使用“当前空闲”的历史slot：较早的短值可能在victim定义与当前压力点之间覆盖它。因此成功前准备一个从未分配过的fresh high-water slot，再提交mapping。到64-slot上限则拒绝victim；不能声称slot高水降低。历史dirty mask只清victim birth与incoming birth之间该home的普通占用，保birth读后写与固定/helper/publication clobber。实际仍付active比较、operand投影、选中home核对及最多八次成功的历史mask区间写；没有逐候选全unit Verify。

默认reserve的同一个RunAttempts选择循环用一个u32余量，共限八次GPR成功或接受的FPR forced-spill restart。native FPR admission触发的外层新RunAttempts仍保原生命周期，不能把这个余量称为跨全部physical retry的总限额。原完整Verify、后验失败清映射、reserve ladder和受限P1b修复保留。删除的是GPR选择导致的整unit Reset/collect/sort/assign重跑，所有RA重试并未消失。

两包合批三目标一次通过，source `ef5c76d5e497980499a2b3d137681e61bcb295ed8426679814873ff73644ab85`，前后manifest一致，generic Release/ThinLTO无PGO；main `275ec038b4732fc7e11f56f520c9c5fd98f36583ffd0aeb893a65cf35b716351`。只运行九个受影响合同：Swift八case/89断言、IR一case/26断言全过，包含真实soft入口、Function分段、六个eviction/fallback成本与历史slot冲突用例以及retry/new-request边界。两个测试ELF核hash清除，设备场已关闭。没有重跑整体生成质量、寿命、PMU或FEX矩阵。

两点ordinary规模观察已独立收口：Block/Function translate为754.792/918.468ms；decode94.651/183.070、opt97.583/95.670、RA195.068/211.297、codegen253.590/284.638ms。无PROF2/RA/DECODE诊断，CRC4983，cpu4 max前后1171200kHz。各一个点且编译单位/IR数量与T不同，不计算稳定提速或最新FEX倍数。结果仍未支持数量级改善，下一步扩大到numeric32 carrier的prebirth节点删除和正常Function反向用途图复用；不为每个小片再开矩阵。

证据：[W4替代与净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/w4-result-home-affinity/report.md)、[P1c实现与限制](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p1c-in-attempt-gpr-eviction/handoff.md)、[P1c独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p1c-in-attempt-peer.md)、[合批构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-w4-p1c/build.json)、[九个必要合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-w4-p1c-contract-checks/summary.json)、[ordinary规模及资格边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-w4-p1c-ordinary-pilot/analysis.md)。

## 第二十六次集中整合：完整numeric U32 carrier少出生节点

X使已知目标下的完整numeric U32结果直接供应既有32/64位view。这里没有给Value增加转换tag；源primitive的整个scalar slot已经等于零扩展数值，view本身仍不做数值转换。公共BuilderCapabilities只增加纯definition predicate，ARM实现归原registers/state；decoder不包含ARM/VIXL或分配器接口。限定Add/Sub/And/Or/Xor/Mul、普通/ordered及extended scalar loads、fits32 literal，并包含既有Sub→Neg正规化闭包。GetHost/GetResult/BitCast/GetOperand/BitExtract/SignExtend、S32与narrow/partial来源不因ReturnType或W名称进入该浅层准入。

同一纯规则接通prebirth ZeroExtend32To64/ZeroExtend64、offset-zero U32/U64 byte forwarding及现residual candidate。None/NoOutputs转换可直接返回同definition；Unknown/Declared身份、源flags、faulting load、multiuse和implicit roots保留。residual复用原worklist的实际用户编辑，WithDefinition保每条Value/Lambda/Params的旧Type；非会员/control/pseudo用途不强搬，零total uses且非root才erase。不能以换成BitCast认领节点退役；已经发出的slot/tombstone及capacity也不会因residual erase立即回收。

完整数值语义同时核过interpreter/RV的numeric mask与ARM实际W结果，spill backing仍为StrX/LdrX。SignExtend的resident省写可留下sign64高半，因此明确不准入。后续真实改写仅可能成为完整numeric literal、其同类型BitCast，或保U32的Neg；这些都保持原完整slot。Shape仍浅查，full value-bits在原32次预算内追真实RawSlot，保Neg/literal alias的high-zero及high-mask/TestZero反馈；dirty GetResult/U64的高半不通过Query32伪造为零。Unknown/default query仍保原路径，没有逐node证书或cache。

分配接口也随消失的wrapper一起迁移。原九op GuestPublication soft域仅补强carrier的4→8宽度门，保raw/single-use/相邻/end/effect/feature/root合同。Or/Xor/Mul原来还可通过wrapper进入非相邻hard窗口，因此不只补一个相邻hint：现EarlyResultHomes将这个已证透明widening按原wrapped规则消费，保Add/Sub/And排除、pseudo/terminal/window/successor/conflict与全Function批提交；W4的精确soft skip保持在前。没有增加第二home authority或扩大soft opcode。

同批把`IRStore::ResolveOrderLink`退役，普通operand与指令顺序共用原`ResolveLocalReference`。同一实际page以稳定NodeId差值投影原Slot数组，跨page仍走原目录；保operand的Debug extent/alive检查，order同页增加相同检查。没有改slot/identity/lease/layout或raw/foreign表示。新operand同页分支及跨页成本如实保留，未独立计时；不以少一次目录读取声称数量级收益。

本批没有新生产文件。numeric实现涉及原20文件，另三个IR投影源及一个既有压力fixture由root整合。首构建source `7df02d80…`稳定，但root新增fixture对const Value调用非const SetCastType，构建停止；保留失败，只改为显式Value副本。X-r2三目标完成，source `9291b939dfc1550fefee6f4e55b8a066d4e353496a1325255495b0126bedc3f6`，前后manifest一致，generic Release/ThinLTO无PGO；main `f79411b07afaef8e815e9e01958d94d84778c0c6ea2359b2b0991837e2a943da`。

仅集中三个原case：两个IR case共251断言，一个真实压力/native发表case21断言，全过。后者沿原三形态，将Add改为真实U32 producer供应U64 publication，保无FixedDefinition-before-RA、home22和机器码断言；不新增case/target。测试前源码匹配已完成构建，两个设备ELF核hash清除，namespace关闭。没有重跑质量/寿命/PMU/FEX矩阵或ordinary pilot，也没有新的正式compile/质量采用结论。

能实际少出生的normalizer省一个managed slot和相应input edge及后续工作，实际省多少受机会覆盖和原有folding限制；head/occurrence/分析容量的取整、残余tombstone、新cap pointer/浅probe与有界raw-copy查询成本仍计入净账。尚未测实际N/E下降及完整服务净收益。下一包删除正常Function的raw consumer镜像，保独立Block按需分析；最终质量/资源以及两模式分别≤2×fresh FEX仍未通过。

证据：[20文件交接与净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/numeric32-canonical-carrier/handoff.md)、[独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/numeric32-canonical-carrier/peer-front-binding.md)、[共同NodeId投影](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/x-local-id-projection/report.md)、[投影独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/x-local-id-projection/peer-front-binding.md)、[合批构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-x-numeric32-r2/build.json)、[三个必要合同及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-x-numeric32-contract-checks/summary.json)。

## 第二十七次集中整合：Function直接消费权威raw用途

Y不再为符合资格的Function从权威输入关系复制一份raw consumer CSR。SemanticFacts已验证SSA、输入完整性、实际consumer数量相等、所有非空creation block可达，且本块dense时，BlockAnalysisIndex只借原FunctionUses::Users。原raw offsets、16B grouped rows、external map和两轮count/fill不初始化。独立Block、manual/partial/sparse/compact及含非空unreachable block的Function保原按需图；后者仍用Function RA，未缩成Block分配。

原Users登记顺序会随Rehang改变，不是最终程序顺序。普通predicate/count直接借同一source；first-candidate、window边界和last明确取真实position/min/max，再验证原先那一个候选，不改成“任一个能通过证明”。只有SelectCandidate/PublicationViewCandidate这两类真正带出matcher的列表才保存小owned有序投影；旧Block/cold仍借原span，每次从当前variant重新取span，避免inline move后缓存指针失效。嵌套查询不复用一个会覆盖外层的scratch span。

重复Params不再先逐occurrence解码consumer再在BlockIndex重组。既有Users::Iterator增加ConsumeConsumer薄API，沿同一环按consumer key成组推进并只投影一次正常consumer；公开原occurrence枚举及顺序不变。仍逐link访问，cold/mixed仍付原NextCold扫描，不是O(1)取整组，也不增加UseGroup图或持久cache。UseRange iterator的当前row引用只活到该iterator推进，现有实际调用已核无跨推进引用或临时range迭代器外逃。

resolved并未全删。它仍保受alias影响的root投影及原聚合/程序序，正常Function新增一次本块实际input discovery，再运行原affected-root count/fill。块内没有BitCast也可能读到块外alias，因此未发现不能当作“无alias”，真正discovery后才ready；无alias时不创建present/offsets/rows。原BlockUseEnds唯一生产枚举接口在借图模式改走其原本块input Record，不枚举全Function.Values或另养producer set。

peer找到的unreachable反例已具体收口：Eager图可能仍有Complete consumer，但RPO将其producer清Visible，SSA对不可达块只验schema；计数相等不足以允许Users替代旧局部CSR。首包复用原all_reachable粗门，不新增可达性信任位或目录。代价是一个非空unreachable块会令整个Function保旧图，不能宣称所有正常编译请求都已命中借用。

本包32文件/84调用点普查，实际改17文件；生产15文件+366/-106，两个原测试文件+79/-8。新增代码是借用迭代、薄分组和必要冷域保留，不以行数增长掩盖是否真实退役raw表，也不以少表推算compile收益。真实新增成本包括global fanout逐块/逐query扫描、normal raw last从CSR.back改为fold、实际候选排序/小容量增长，以及Block span路径的range控制。存在Q×global degree的最坏重复访问；resolved discovery/容量也计入，未称所有访问都更少。

root按context patch与X复合，保gpr_constraints、state.h和IR fixture里的numeric改动，未用整个旧after覆盖。三目标一次通过，source `97ccca5e3efcedf7009b3c0e738e2bf18540ae1d097fdf7543ad90484e2b3ae7`，前后manifest一致，generic Release/ThinLTO无PGO；main `6d0b87c3b0635750a42e42c2bda1bf0b3552e32bee9153839ebf327d5717f15c`。

只集中四个既有case：Swift三个case/194断言、IR一个case/96断言全过。dense mixed Params case显式要求实际borrowed raw与stored resolved正路径，保原独立扫描oracle、登记序反转、无本地BitCast的跨块alias、嵌套借用、foreign同ID拒绝及Eager unreachable回退；另保真实Function fixed-segment与现有native压力发表。没有新增case/target或质量/寿命/PMU/FEX矩阵。测试前源码匹配构建，两个设备ELF核hash清除，namespace关闭。随后安排的ordinary粗计时在第一个Block点触发下述合同缺口，有效计时点为零，Function点未运行，不作正式性能采用。

证据：[实现范围与净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-consumer-unification-review.md)、[核心独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-consumer-unification-review/peer-ir-store-owner.md)、[消费者独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-consumer-unification-review/consumer-peer-front-binding.md)、[实际复合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/y-function-uses-integration/applied.json)、[Y构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-y-function-uses/build.json)、[四个必要合同及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-y-function-uses-contract-checks/summary.json)。

### Y后普通点：完整高零view的最终重证明遗漏

原ordinary第一Block点target exit134、collector78、无CRC，消息为`invalid final GPR read elision at IR 7`。原始点保留、Function未运行、未自动补点，设备场已关闭。随后同source/ELF单独执行一次DumpIR诊断，预期重现exit134，亦已收口；它不计为性能点。

精确witness在block `0x2699cc`：U32 LoadImm零以U64 view发表到home22，IR7的U32 GetHost已按新增强canonical high-zero能力获准省略。最终`VerifyHostRead`仍仅认显式ZExt32To64或4字节view，遗漏了这个新8字节view，因此在读到末端Uses查询前拒绝。不是Y的Users顺序或Block CSR空答。输入准入、物理完成与独立重证明都要消费新增形态；只通过原四个case不能替代这个真实编译入口。最小修复限定为最终证明补同强predicate，并迁一个现有read fixture，保actual home、latest publication、observer和后续写窗口，暂不宣称修复已构建通过。

证据：[失败点与零有效计时](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-y-ordinary-pilot/failure.json)、[定向最终IR](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/y-read-elision-debug/witness-final-ir.txt)。

### Y后窄修复及集中成本收口

最终`VerifyHostRead`已补同强canonical U32→U64 view的high-zero分支；独立actual home、最新publication、observer、width-component committed/owner和后续写窗口都保留。生产仅一个cpp的布尔分支改变，没有关闭checker或恢复所有转换节点。原有target-planning fixture改为完整U32 literal发表后再读同home U32，要求RA确实标read coalesced、Complete确实给HostReadVerified，并保freeze/idempotence/emitter检查。

最初两次fixture失败保留：bare Config没有x86静态绑定，默认pool未保留x0..5，导致X86PinExtLevel2Enabled为false、根本没有生成ReadReuse候选。root最初猜直接StoreUniform观察者导致拒绝，增加一个扩展consumer未解决；只读核查已否定该猜测，现恢复直接consumer，仅给fixture配置真实pinned pool合同。未改生产门或删断言。修正后原case14断言通过，设备ELF核hash清除、场关闭。

只构建受影响的main及既有swift_codegen_test两个目标；最终source `e2f171d1caa18e993cdcb4af42050063a4b32ee920e13116b4589e3625bccf34`，manifest稳定，generic Release/ThinLTO无PGO。main `24b93f7f766e78b1808ff6484459c26fc1a5f1d7d23494ad677d197152f08feb`；两次夹具修正都未再改变该生产ELF。未重跑其余已过合同或质量/寿命/PMU/FEX矩阵。

以修正后源码另取各一个ordinary点，Block/Function translate为728.642/899.677ms；decode95.306/182.617、opt93.092/92.047、RA187.048/196.473、codegen242.586/285.301ms。CRC4983，cpu4 max前后1171200kHz，无PROF2/RA/DECODE/DUMP诊断，场关闭。实际单位/IR数量有差异，无配对/频率准入/fresh FEX分母，不认领稳定收益、最终生成质量或最新正式FEX倍数。规模仍未支持数量级改善，继续按大owner与每值真实工作选择更大的表示/流程收敛，而非追加小家族矩阵。

证据：[窄修复与根因](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/y-canonical-read-elision-reproof/handoff.md)、[fixture准入核查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/y-canonical-read-elision-reproof/read-admission-followup.md)、[最终构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-y-read-reproof-r3/build.json)、[一个必要合同与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-y-read-reproof-r3-contract/summary.json)、[修正后ordinary成本账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-y-reproof-ordinary-pilot/analysis.md)。

### 当前原生采样：按实际服务栈确定下一批工作

沿用上述source和main，仅取一次Function原生采样，无SVM profiler、源码插桩、FEX对比或质量矩阵。目标CPU4、父采集CPU0，cpu-cycles:u 1000Hz、DWARF栈16384字节，保留展开错误记录；未写频率/governor。原始点与私有设备场均已收口，不追加重复点。

实际目标PID共1877样本；954样本在遇到未知DSO前有真实Translate服务祖先，占该PID周期权重50.103%。资格由原始帧确定，API导出及独立peer重取的成员相同。32条是同PID exec前样本；859条未知叶占全PID权重47.635%，继续保留为未知，不能推断成guest或compile。

已确认服务内部，按最近阶段marker互斥归组：decode/build 21.31%、target Complete 16.54%、emission 15.17%、allocation inputs 12.02%、residual opt 10.94%、physical RA 8.49%、semantic facts 8.13%、其余7.39%。这是栈归属，不是PROF2精确计时，不能乘ordinary毫秒数。另一个最近项目源owner视图中IR storage/args/uses为26.83%，ARM inputs/Complete/checks为17.09%；这两个视图不得相加。最大原生self叶仅3.52%，单个项目owner最大约1.99%，没有一个getter或布局字段足以解释十倍开销。

原始918条unwinding_result均紧邻且同时间戳唯一对应一条sample，peer据此关联实际PID/TID；38条合格服务样本带原始error_code=1，约占服务周期权重4.00%。保留其已知服务栈，不把所有展开错误归入compile，也不猜错误枚举或used_time单位。这一诊断足以指导整族表示和重复工作的收敛，不证明后续改造收益或当前对FEX倍数。

后续按完整功能批次实施，只做必要构建和受影响正确性合同；遵照用户要求，过程不逐片检查生成质量、不再为每个微调展开性能矩阵。主要流程稳定后集中验收最终生成质量、资源以及独立Block/Function各自相对fresh FEX的compile目标。

证据：[原始点及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-current-native-cost-sample/closure.json)、[原生归约](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-current-native-cost-sample/offline/analysis.json)、[独立成员与展开记录核对](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-current-native-cost-sample/offline-field-review/peer.md)。

## 第二十八次集中整合：普通scalar flags输出归真实producer

Z已一次整合core19、portable11、ARM分配/窗口9及ARM发码/选择25个源文件的完整包：共审64源、实际改46源，source `3e167afc5f181747af42695bca5822d85703079e8f230e51c2ce7b5f240116f6`，未涉及源码与原WIP保持一致。没有新增生产文件、opcode、pass、MIR或第二用途图。固定缓存main与swift_ir_test两目标一次构建通过，generic Release/ThinLTO无PGO，源码前后一致。main `48414be6d6e8fce3e1a1c3216d02f56b422d48e324b5b50115d49e7ac7573b24`。

现`instr.h/.cpp`在原header padding/spare bits保存mask、profile及branch-only语义，统一构建的static_assert确认Inst48、payload offset24及managed stride64保持。ArithmeticRaw保原SaveGuestFlags的SUB/Neg无借位C，ArithmeticCanonicalCarry仅对实际输出CF取反；LogicalZeroCVA从结果求N/Z/P并清C/V/AF。只准实际U32/U64 Add/Sub/Neg或And/Or/Xor；无输出归None，非法组合在原编辑边界拒绝。动态effects保flags-only producer活着，不制造data/pseudo use。原SetInst/SetReturn保活动语义并验证新形态，reset/teardown清除，真实clone显式复制，cache版本升57，restricted-resume identity纳入字段。

`builder/instruction.cpp`仍有唯一pending完整writer。只在原flush时给幸存producer设置输出，不出生SaveFlags/ClearFlags/InvertCarry或伪边；被覆盖writer连字段也不先设置再取消。首包仍是U既有known-target wide ADD/SUB/AND/OR/XOR域及原Sub→Neg闭包；Unknown/early-off、narrow/partial/carry/CMP/TEST等保旧路径。观察、fault/helper/control、terminal、异常取消及原AdvancePC边界不变，不顺便删除PC提交语义。

公共analysis/worklist仍用原FlagFlow、BaseFacts、PseudoFlagsPlan和DCE：需求同时认直接输出与真实legacy use；value-dependent与constant clear分别裁剪。只剩逻辑C/V/AF时，无数值/implicit root则原identity改ClearFlags并释放ALU输入，有数据/root则在原位置形成已有ClearFlags后释放producer输出，使现fold/CSE/hoist恢复。这个分支不计作节点节省。直接算术只增加ReverseEvent，不能因新表示强开旧来无consumer的整块LowerConsumers；逻辑constant clear保原Entry语义。

ARM在现`translator*.cpp`、`select/{flags,consumers,predicates,edge_flags,memory}.cpp`和`target.cpp`内闭合真实producer提交、raw/canonicalCF、PF/AF、PSTATE/local-condition/跨块与fault合同。直接输出没有假Save消费者，early/soft的旧total-use纯数值资格明确排除value flags，constant-only则保原numeric+独立clear机会。scratch请求在原resources/scratch owner合并，不新造逐node证明。peer发现LoopHoist anchor不增加GetUses，已令direct+本块anchors非空保守保留真实数值计算，避免Cmp/atomic flags-only选择丢掉活值；不将anchor伪装成普通use。独立Block与Function的原分配范围不变。

解释器单指令Run在原结果后提交输出；RV在读操作数前保留真实结果位置、共享原flags公式并在数值结果落位前消费保留的输入，原Function/Wide clone复制字段。不另生兼容指令流。ARM构建包含RV发码C++源，但ARM手机不能执行RV机器码，不将编译或静态核对说成RV运行验证。

对实际幸存U完整writer，ADD少发一个slot，SUB/逻辑组少发两个；结构上为`64*(A+2S+2L)`字节issued slots，实际页/容量取整及覆盖量另算。R早已延迟反向拓扑，不重复把未出生边算成一次eager分配节省。新增固定字段初始化/读取、flush通知和现有消费者分支仍收费。FLAGS_REGS=0且direct flags-only结果为MEM时，当前保守保留backing写回，可能多一条store；这是明确中间QoR缺口，不靠假Save、自边、抢scratch额度或弱化required-backing合同遮掉。最终生成质量尚待集中验收。

本批仅复用三个受影响旧case：构造/flags语义、observer/terminal/cancel和实际ARM全/部分flags读写，111958断言全部通过。后者将原Add+Save迁为producer输出，保原输入oracle/维度及prefix legacy路径，不再扩新矩阵。root在构建前发现private EditInst访问不合法，已改为现IrBuilder受控friend和测试真实出生时的mutable Inst*，没有const_cast旁路或开放公共写口。设备测试前源码匹配构建，测试ELF核hash删除、私有namespace关闭；没有新增ordinary/PMU/FEX点或生成质量矩阵。必要语义合同通过不等于最终生成质量或compile目标已达成，整体目标继续。

证据：[唯一整合及源码绑定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/z-producer-flags-integration/applied.json)、[core语义与净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/z-producer-flags-output/core-handoff.md)、[portable职责](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/z-producer-flags-output/portable-handoff.md)、[分配/隐式root与scratch闭合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/z-producer-flags-output/arm-middle-handoff.md)、[统一构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-z-producer-flags/build.json)、[三个必要语义合同及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-z-producer-flags-contract-checks/summary.json)。

## 第二十九次集中整合：退役spill静态输入owner及无用数值写回

AA完整删除`SpillInputBuilder → SpillInputs sites/segments/blocks → sort/copy → BlockFor/FlagsFor/Segments`链及`registers/spill_transport.cpp`，原11源范围净删163行。唯一纯target分类归现`target.h/.cpp`，AllocationInputs只携一个可空函数指针：低五位是原region能力，另外两位是原立即forward-kind。两组集合逐opcode保持，不能union后扩大资格；公共allocator只解释region位，不包含ARM寄存器/发码接口，ARM scratch只取自己的投影。没有新项目文件、图、包装层或兼容镜像。

原输入Describe共用的N遍历仍用于其它约束，不能声称删除一轮独立pass。BuildSegment在原actual MEM operand扫描中遇Goto/NotGoto/BindLabel直接收口，局部position与截短span保持一致，不先预扫或另建段表。alias/raw AllocationId、occurrence和唯一consumer position、std::map首候选顺序、terminal、actual dirty/fixed/headroom、slot/backing及MapSpillReload保持。P1b的原单段门移到它已有选定victim证明visit，不扩候选或Function范围。

默认/unknown/null仍不启用region优化，manual MapMemSpill入口仍按实际mapping工作。首包没有加入`spill_count==0`跳过门：普通fresh scan虽可说明其含义，任意手工/预置map的入口不提供同一fresh合同，不能靠计数掩盖活MEM。unit有scalar能力但个别Function member只有FPR时，原被BlockRange排除的空投影现在可能发生；明确计入，不加B表补回。新增实际MEM候选的indirect调用，以及scratch原inline分类改out-of-line的成本也保留。已删除的分类/二分/容器不等于已证compile净收益。

同批收口Z的一个中间质量缺口：`JitContext::EndInstructionScratch`在真实producer与flags Commit之后、原lease结束前，仅去掉当前无数值存活义务的canonical MEM pending写。门包括真实总use0、含anchor保护的scratch无数值需求、live end等于birth、无fixed/whole/additional-residence/width-owner，最终仍精确核当前value/slot/结果reg且非FPR/非required_backing。其它pending、forwarded前驱、任何恢复义务及原GetSpillTmpX/reload预算、memo和lease都不变。没有假Save、自边、新证明表或API。每个合格条目省后续一个scalar store，候选增加现队列O(P) erase；这不是compile数量级收益声明。

两包一次合入12个变化（含1文件删除），source `a97cf816589ac65090dfda5c03bb48c1beabcd2d6dc016c0ef96661b1cebc0c5`，其它源码/WIP精确保留。main与swift_test两目标一次构建通过，Release/ThinLTO无PGO，源码前后稳定；main `1c961dcf8221b7bf279dc61a1fc748203ca0be63babfc8d8ca5a72973dc83ebb`。没有为未受影响的Z IR测试另建目标或重跑。

只集中跑四个既有case：spill输入/边界、occurrence/alias、load ownership邻接，以及压力下直接XOR flags/存活结果的实际native语义。最后一例保原三shape/三输入及AF初始化oracle，仅把真实出生的XOR+Save迁成producer字段，无新循环/case。统一`SVM_FLAGS_REGS=0`，347断言全过；设备ELF核hash删除、进程及namespace关闭。没有新增生成质量、lifetime、ordinary、PMU或FEX矩阵，最终质量/资源与两模式≤2×fresh FEX仍未验收。

证据：[AA交接与净成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/aa-spill-transport-owner-retirement/handoff.md)、[静态分类与边界独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/aa-spill-transport-front-peer/review.md)、[无用写回闭环](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/z-direct-flags-writeback-closure/handoff.md)、[精确整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/aa-spill-transport-integration/applied.json)、[统一构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-aa-spill-transport/build.json)、[四个必要合同及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-aa-spill-transport-contract-checks/summary.json)。

### Z/AA后规模观察与后续优先级

两批完成后仅取独立Block/Function各一个ordinary coarse点，无PROF2/RA/DECODE/DUMP附加诊断。translate为724.444/887.482ms，decode94.428/181.705、opt93.393/97.404、RA177.629/182.791、codegen248.238/284.380、publish34.285/48.085ms。Block2419单位/61893编号IR/226792 host字节；Function431单位、4564 decoded blocks、73289编号IR/339716 host字节，arena分别477454/5979191B。两点CRC4983，cpu4 max前后1171200kHz，私有进程/目录关闭，无频率写入。这不是配对性能采用或生成质量矩阵，编号IR也不是SSA、issued slots或input occurrences；不算最新FEX倍数。总体仍未显示数量级改善。

第一次采集因root复用脚本漏改namespace admission guard，在sample创建和target启动前exit64，零计时点；原记录已保留。单独r2只修目录保护后执行原定两点，不覆盖原失败，不重复任何已运行target样本。

两条只读替代路线已据实际代码收口。Seal后把FunctionUses改CSR暂不实施：当前已是唯一ring且Y已删normal raw CSR，Seal后没有持续Rehang可省；新转换反而增加N/E重排、峰值及scoped seal解除的恢复合同。P5晚边界replay是真实可删族，但要完整分离现legacy/VEX/六raw recognizer的parse/lift并保owned bytes、WRPKRU terminal与唯一支持匹配；旧历史reset/redecode仅约1.28%，不能据请求数推断它是当前compile主因。现多region本就共享一次code object，不再认领已存在的对象合并。

下一步先补齐SVM/FEX实际decoded guest、IR/SSA及input总量的可比定义，复用现有visit的最少累计诊断；不新建通用跟踪框架、不逐node计时、不把诊断耗时作为ordinary。源码清理继续选择真实重复owner，避免仅凭少表/小字段假设已接近两倍门。

证据：[两个ordinary规模点及资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-z-aa-ordinary-pilot-r2/accounting.json)、[零target的采集配置失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-z-aa-ordinary-pilot/setup-failure.json)、[Seal→CSR否决](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/readonly-function-use-owner-review/review.md)、[P5实际工作族与边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p5-compile-granularity-review/review.md)。

## 第三十次集中整合：GPR consumer-role按需查询

AB实际退休`GPRInputs::ConsumerRule`、最终/临时两层consumer_rules、copy、sort和每query的ID lower_bound。现GPRInputs只携可空纯callback与请求级enabled mask，backend匿名实现复用原opcode/ReturnType/helper规则；features及两pool OR门按原builder形成，width emission仍严格受ra_width_chain而非long-only开关控制。三个consumer复用原view/publication range，coalesce宽度事务每块新增一次原BlockFor查询，不能以unit callback非空给其它member扩大资格。

Clobber/CrossesClobber、fixed sites/steps、Publication/ReadReuse/ValueView/BlockRange及所有物理检查保持。GPR Clobber有full helper mask与所有语义SetHost屏障，FixedValidation sites只存部分mask且使用不同physical features，实际allocation masks还包含placement/省略状态；这些不是同一事实，未强行合表。新增query的间接常数分类和每块一次range查询如实收费。实际改10源、审12源、+45/-41；净行数略增不掩盖整份稀疏owner确已删除，也不据此认领compile收益。

临时4cpp工作量诊断已先精确还原普通a97源码，之后一次合入AB，source `4220a63e434ad6c22e9b4987b0bd637fcc294ba99d29655e2185fe4bf3b92267`。main与swift_ir_test两目标构建通过，源码前后一致、generic Release/ThinLTO无PGO；main `73725c951ec4d982228273515624fb391d0ecd324a2ad73056f1bd9631119e25`。固定缓存与保留产物已回到普通代码，工作量诊断ELF另存，未混作性能分母。

仅集中跑原narrow/low32与width bridge两个受影响case，49断言全部通过；fixture只迁真实API/新cap字段，没有新增case/镜像或形态矩阵。手机ELF核hash删除、私有进程/namespace关闭。未对该片追加生成质量/PMU/FEX矩阵或ordinary点。最终质量/资源与两模式≤2×fresh FEX仍未验收。

证据：[交接与净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ab-gpr-consumer-role-retirement/handoff.md)、[实际资格独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ab-gpr-consumer-role-retirement/peer-front.md)、[精确整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ab-gpr-consumer-role-integration/applied.json)、[普通构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-ab-gpr-consumer-role/build.json)、[两个必要合同及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-ab-gpr-consumer-role-contract-checks/summary.json)。

## 第三十一次集中整合：scratch scope就地存储与受控append延迟建图

AC将不可移动的`JitContext`末尾`unique_ptr<UseScratchRegisterScope>`改为`optional`，在原位置emplace/reset；VIXL的parent/current-scope栈、Include/Exclude、lease及combined预算不变。每次实际启用该scope少一次new/delete，代价是每个context的固定存储增加，目标ABI净sizeof尚未单独测量。它不是取消scratch合同，也没有据此认领RSS或compile收益。

AD保留公开`PrepareAppend`在外借draft前先PrepareUses的合同，只让Block内部同步append/insert调用私有`PrepareAppendOwned`。同service、原registered/definition要求一致的unchanged重插不再先建全图；新normal、无previous-owner、无Value结果且VOID的候选沿原ScanInputs证明实际零输入后也可保持Deferred。raw、跨scope、missing、实际Value及其它候选仍在Reserve/Begin前materialize。ConsumerCount、sequence、注册顺序、slot反馈和Seal行为保留，没有新增phase位、缓存或图。

实际提前入口是state sink的同consumer重插，以及Z存活data配套的零输入ClearFlags；原RewriteContext构造并不会主动建图。AD没有移动ValueWorklist构造或增加DCE轮，只避免在state/flags清理完成前为这些无拓扑变化的操作建立E。最终仍需建立同一reverse topology，存储格式与head extent不变；可少处理多少边取决于随后真实删除的consumer，不能直接换算毫秒。

两项一次合入6个文件，source `2c766e2e01e55aaa1f8d94897ce7579de626cac5e40c8cf881591b2f2153b67c`，其余WIP保留。main与swift_ir_test两目标构建通过，源码前后一致、generic Release/ThinLTO无PGO；main `455edcbc4dbff40d7e626596a68e9360ccc7c3aa89dc36a768475258b1e7e5dd`。仅跑三个受影响的既有语义case，84断言通过；设备ELF核hash删除、进程与私有namespace关闭。按用户要求不追加生成质量、ordinary、PMU或FEX矩阵；最终质量与两模式≤2×目标尚未验收。

证据：[AC职责及存储代价](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ac-inline-vixl-scratch-scope/handoff.md)、[AD实际提前入口及fallback](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ad-deferred-append-materialization/handoff.md)、[独立复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ac-ad-ir-middle-peer/review.md)、[精确整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ac-ad-integration/applied.json)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-ac-ad/build.json)、[三个必要合同及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-ac-ad-contract-checks/summary.json)。


## 第三十二次集中整合：删除逐值堆节点与通用状态机开销

AE–AI一次合入13个生产文件，复用现有owner、能力与文件；没有新增pass或逐节点证明层。五项实际变化如下。

| 范围 | 删除的工作与保留的合同 |
| --- | --- |
| AE，`common/svm_config.*`、`perf_stats.h` | 配置唯一对象由原锁慢初始化、release发布一个指针，getter只acquire读；退休heap once_flag与逐getter的State/once路径。关闭详细诊断时不再先取decode/lowering TLS；启用后的depth、Stop与路径恢复保持。test reload仍限无并发reader，driver内部ABI写仍限创建translator前，发布指针不提供并发reload能力。 |
| AF，`allocate/internal.h`、`linear_scan.cpp`、`ties.cpp`、`fpr.cpp`、`spill.cpp` | 已排序live_interval成为唯一记录，normal std::list及fast第二份完整interval退休；active只存u32位置。normal一次稳定compact、fast保原end-only heap，保严格end<start、tie首match、CFG/preserved端点、actual map、P1c cache/fresh slot及全部Verify/ladder。 |
| AG，`ir/instr.cpp` | 仅在析构已有真实owner/hook清理之后，直接执行原UnregisterRange与FreePayload，去掉DestroyArgs通用编辑状态机。公开DestroyArgs、bulk ReleaseArgs及raw/Params/use计数原函数保持；没有将graph unlink与计数的两次E访问冒称合并。 |
| AH，`jit/jit_context.*` | 两个逐指令spill map改为inline 2/4条且可增长的现StackVector记录，forwarded标记归use条目，第三个set退休；不再逐值new/delete树节点。所有普通插入先find，唯一consumer Flush位于Tick清空之后且最多forward一条。普通GPR/FPR重复访问仍重载，forwarded GPR仍不读未写slot，required_backing、pending次序与scratch预算保持。 |
| AI，`frontend/x86/decoder.cc`、`decoder_alu.cc` | 原普通reg/imm32/64 gate扩大至CMP/TEST，复用唯一pending producer输出；成功CMP少SaveFlags+canonical InvertCarry，TEST少ClearFlags+SaveFlags，均少一条pseudo数据边。TEST保local NZCV/PF提示，CF/PF/AF与消费前兑现保持；memory/narrow/LOCK/Unknown/off走原路。实际producer和flags计算并未删除。 |

净成本包含新增间接读取、normal tie删除时的u32后缀搬移、小memo的O(K)查找与overflow增长，以及每个diagnostic part scope的一个借用指针；没有据删除节点/少字段直接推算收益。AF不为normal按N强制reserve，视图增长只移动索引，记录在同attempt内不再扩容/重排；AH inline数不是合法输入上限。配置getter在本次ELF中无独立符号，已检查的decode scope关闭分支先退出而不调用emutls；这是静态代码形态，非耗时证明。

统一main、swift_ir_test、swift_test构建一次通过；source `135df91d1d1035256850cb3775213d35c4b77e787c4c3527f55ab9900eff4500`，源码前后一致、generic Release/ThinLTO无PGO；main `ff7341287026be79bc8e614d877039d5f9d58ce3f2e8e2918e953100ad01687c`。其它WIP与临时诊断恢复状态保留。

只集中执行5个已有case：IR两个GPR/FPR tie输入合同98断言，Swift配置reload/hash、single-block/general allocation map、真实spilled XOR三个合同201断言，均通过（统一FLAGS_REGS=0）。另将原`flags_branch_only_x86_64.S`原样装配为7,232B guest，在本次主ELF只跑一次Function=1、FLAGS_REGS=1，覆盖CMP/TEST全Jcc、CMP→ADC及跨edge/observer，原PASS判据通过；未调用旧六模式脚本，也未新增case或改oracle。未声称已执行TEST→ADC或CMP/TEST→SBB精确runtime oracle。四个设备ELF核hash删除，私有进程及namespace关闭。

没有生成质量、lifetime、PMU或FEX矩阵；最终质量/资源与Block、Function分别≤2×fresh FEX仍未验收。此批之后的ordinary规模观察单独记账，不以必要合同通过代替性能采用。

证据：[五包精确整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ae-ai-integration/applied.json)、[AE发布与诊断边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ae-compile-runtime-overhead/handoff.md)、[AF唯一interval净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/af-active-interval-view/handoff.md)、[AG析构边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ag-retired-inst-payload-release/handoff.md)、[AH memo及独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ah-spill-scratch-memo/peer-ir-middle.md)、[AI覆盖及回退](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ai-cmp-test-direct-flags/handoff.md)、[统一构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-ae-ai/build.json)、[五个合同与单guest及设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-ae-ai-contract-checks/summary.json)。


### AE–AI后规模观察与仍未删除的乘法项

本批仅取Block/Function各一个ordinary coarse点，未启PROF2/IR_DETAIL/DECODE/RA/DUMP附加诊断。translate为695.054/839.014ms；decode87.833/171.224、opt91.660/94.927、RA171.798/172.048、codegen231.305/263.988、publish36.060/45.789、ir_free6.625/11.426ms。codegen含target Complete与实际emit，不能用旧诊断比例拆出本次独占毫秒。Block2414单位/59946编号IR/226584 host字节；Function429单位/4560 decoded blocks/70647编号IR/326132 host字节，报告ir_arena_bytes分别477454/5782199B。

两个点CRC4983、target退出0，实际ELF/guest/argv/env核验通过；cpu4 max前后均1171200kHz，无频率写入。设备唯一结果hash归档后，owned进程/文件/目录全部关闭。新旧点工作量不同且无配对，不计算稳定净收益或最新FEX倍数；总体仍是几百毫秒量级，未接近最终两倍门，不为补足统计再追加点。

两条只读结论避免继续错误删法。`FunctionDecodeFrontier::DecodedEnd`使用仍在块内的AdvancePC和，当前GetEndLocation可能只是声明范围/旧reset范围，decoder.pc也可能先于失败推进且包含被HIR终端丢掉的尾指令；没有可直接借用的等价accepted-end，不新增缓存或删marker来绕过。Fixed-home索引在普通birth/W3软hint上复用clean结果，并非每值重建；但late copy/select成功会真正修改component fixed声明，下一候选便全placement扫描并排序。因此有真实O(S·N+ΣKlogK)域，不能靠删Recolor末尾invalidate消掉；需要收敛后置决策/真实声明的维护方式，尚未实施或证明该域在当前点的权重。

下一批优先退役direct producer flags的密集需求影子，使原数组只保legacy伪边、实际查询从唯一Inst合并；Function局部与unit需求、packed全branch贡献和rich branch存在性仍分别保持。另有uses原收集visit内的有序判断可省不必要sort，尚未合入，不重复认领收益。

证据：[两点账与资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-ae-ai-ordinary-pilot/accounting.json)、[普通执行与收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-ae-ai-ordinary-pilot/execution.json)、[DecodedEnd边界否决](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/decoded-end-owner-review/report.md)、[真实late声明重建链](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/al-fixed-index-invalidation/review.md)。


## 第三十三次集中整合：direct flags单一属性与参数直接定位

AJ、AK R2、AN一次合入11个文件（10生产、1已有测试），未新增源文件/pass/图。普通use图在原consumer收集visit同时比较严格sequence，已序直接建立原topology，无序/同序仍用原sort；不把NodeId或RPO等同于注册顺序，没有新扫描、缓存或状态位。

AK使unit rich数组与block packed只保真实legacy SaveFlags/BranchOnlyFlags，不再把producer直接属性写成两份dense影子、再copy到UnitActions。`PrepareLegacyFlags`显式准备，`FlagsFor(actual Inst)`合并唯一节点上的direct需求；packed的`For/SavesParity`原已合并intrinsic，现`Empty/Words`只表示legacy存储是否为空。mixed legacy仍可能分配原完整span，未声称所有N表都消失。rich branch_only表示存在branch需求的OR，packed表示所有贡献均只供branch，不能互换。

Function rich需求保持原reachable域；正常Optimize/Describe在查询前已有PrepareValueLiveness及SSA/all-nonempty-reachable/consumer检查，复用原function_uses_validated，empty unreachable synthetic member不使其失效。只有未通过该资格、且确有direct输出的cold/partial query才查实际RPO会员，最坏O(D×B)如实计；没有新N/B表或额外SSA调用。本块final scratch仍只借本块legacy需求再合intrinsic，不能拿Function全局需求代替本块pseudo域。

AN保持48B Inst与64B managed槽，把原24B payload改为始终存活的array：19B数据和5个u8 offset，spilled指针memcpy编码在同array前部。每次完整参数提交按实际kinds一次更新目录，GetArg/朋友Project直接读取偏移；顺序visitor仍走原byte cursor。Grow保旧目录，old unlink后写完新payload才更新5offset，再Compact/Commit；Free清目录，原Params/use/通知/预留失败合同不变，bulk不去读已退役definition。

为保常见BitExtract/SetHostGPR内联，tag15改为ValueType+u16的3B非负小整数，public仍是Imm；负数、无效/非整数type及大值保9B原路，完整64位值和有符号/类型语义不变。显式local V128改用既有generic NodeId+cast（5B），default VOID view不变。每个实际commit新增五个offset写，每次小Imm有分类/解码成本；相对旧data字节，净公式为-6×eligibleImm数+explicitLocalV128数，inline门从24降19。CAS128五common local20B、Lambda+三local23B、Lambda+两显式V128 21B及其它raw/foreign/大Imm20–24B组合可新增overflow；这不是无代价压缩或RSS收益结论。

R1 AK遗漏的cold Block RecordProducer调用由独立peer在构建前发现；R1原材料保留，R2只纳入block_index.h删除该3行并修正相关旧注释。整合前对全source所有旧API名做virtual overlay，残留0，再应用一次。原shared flags用例的packed true断言迁false：Save(C/P)有真实非branch义务，所以不是all-branch-only；rich存在性仍true，原mask/count/跨块oracle保留。原Z三filter确未覆盖这一个旧case，不能把这次迁移说成旧批已通过。

两个目标main+swift_ir_test一次构建通过，source `b5a0b24d986fed4450dc15a8165c401a186a1b3e724b888758a4e7fd0b1b7622`，源码前后一致、generic Release/ThinLTO无PGO；main `9b726940d92252207640897c6e8fb11b54334b9181ed29b9eb040a965c6a2c0e`。48B/payload offset24/所有kind最多55B可入u8的编译断言实际通过。

只集中跑7个已有IRcase，679断言全部通过，覆盖direct/legacy、跨块与unreachable、序列/反馈、全宽立即数和invalid域、payload grow/compact/snapshot/Params。原payload case只加5个公开65535→65536/旧snapshot/类型与S8(-1)往返断言，无新case/循环/私有布局镜像。精确复用上一批7,232B flags guest，只跑一次Function=1/FLAGS_REGS=1，原PASS判据通过；三个设备ELF核hash删除，私有进程/目录关闭。没有额外ordinary、质量、lifetime、PMU或FEX矩阵，本批净性能未测，最终质量/资源与两模式≤2×门仍未通过。

证据：[唯一整合与API收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/aj-ak-an-integration/applied.json)、[AJ有序收集](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/aj-function-use-ordered-materialization/handoff.md)、[AK职责与净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ak-direct-flags-projection-retirement/handoff.md)、[AK独立peer与首版问题](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ak-direct-flags-projection-retirement/peer-front.md)、[AN完整布局/代价](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/an-inst-small-immediate-offsets/handoff.md)、[AN独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/an-inst-small-immediate-offsets/peer-ir-middle.md)、[统一构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-aj-ak-an/build.json)、[集中语义与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-aj-ak-an-contract-checks/summary.json)。


## 第三十四批：Select发表的真实birth与flags-only目的分配

AO先精确合入14文件（13生产、1既有fixture），中间source `efba8899a0c684f648accc803821d6af58f6490eb415d862621d463f599cc7f0`。后续4行总用途修正、AQ R2的16个生产文件和一个既有Low32 fixture补段已集中合入，合批source `64da58c600437360ac25e7fac653d3cd4c3e903e816f8a4f4a69c2fa1c27a8f5`；相对上一已验b5a0实际改变28生产文件、2个已有fixture文件，不改无关WIP。main/IR/swift_test三个受影响目标已一次统一构建通过，构建前后源码一致，generic Release/ThinLTO无PGO；没有逐小片质量、PMU或FEX矩阵。

SelectZero U32→显式U64 widening→full SetHost的纯窗口，Describe只给现Preference两条8B偏好：source出生只选自身home、不标发表；conversion出生必须读实际source同home、真实last-use等于当前birth，再决定其映射和发表省略。仍用原active/fixed/borrowed/CFG端点，未把Function缩成Block。该覆盖域退出early-hard、generic fixed-publication reuse和guest-write后选，late Select matcher/SelectCandidate/专用ResultHome链退休；最终same-W ZeroExtendAction及VerifyPublication沿原路径。其它copy/memory决策继续存在，不称Complete已只读。

旧宽U64、发表后low alias及依赖physical capture的机会有中间QoR缺口；首case仅增真实U32/sealed正路并保原wide canonical fallback检查，故障/覆写旧输入不动，没有将旧全部优化悄悄当作保持。legacy fixedpub descriptor仍在每块Finish采集/搬运：此时后续块还会append preferences，原唯一sort在全unit末；为省尾项不引入第二sort/候选镜像/重扫。此成本明确未删除，不虚称整份input已退役。

AO原peer之后确认一个计数门遗漏：`GetUses()`默认排除pseudo，不能当总用途。static/generic配对资格的4处单用途门已改为`GetUses(false)`，单独补丁保留，不冒称旧peer哈希覆盖该修正。

AQ把`DISCARD`加入唯一实际Allocation Map，和未分配`NONE`分开；只有ARM纯能力接受的32/64 Add/Sub/And及原feature允许的Neg、direct非空NZCV值需求、完整sealed SSA/liveness确认无numeric/legacy/terminal/跨块/hoist用途、没有fixed/home/resident/width义务时选择。generic只借请求级可空callback，不包含ARM规则；未知能力维持原分配。Block与Function原收集入口均在固定预分配之后选实际结果，重复收集不重复建interval，reset/retry沿原Map生命周期；不能把Function用途降为本块计数。

实际删除是入选结果的interval push/sort/active/expire与GPR/spill目的选择；IR节点、operand边、Map/LiveEnd、每IR dirty mask和最终scratch visit仍在，初始容量也未按入选数缩小。新增代价是每HasValue纯target门、命中后的当前facts/placement查询和局部terminal根检查，以及Low32候选额外一次既有grouped用户枚举，可能使原用途owner提前prepare。没有新增逐N状态、用途图或certificate，不据此宣称N/E、内存或总compile已经下降。

AQ R1在应用前撤回：同一默认GetUses问题已在四处改总计数；Low32三种共置与相邻fallback曾把DISCARD消费当有目的寄存器，和最终ownership检查冲突。R2在共同入口遇实际DISCARD消费者时保守保留显式extract/copy，最终checker及全局readonly-op表不放宽。scratch/audit新增查询先守HasValue，发码仅实际DISCARD可映射WZR/XZR；原CF极性、constant-clear、旧PF token兑现与无新Parity token链保留。两路有限peer已绑定R2最终16文件，无剩余确认mustfix；这不是执行测试。只在已有Low32 case加一个无循环段，检查实际DISCARD和未共置bridge，经原Complete保持Ready；不增加retry/根/质量矩阵。

最近ordinary仍仅上一批135df的695.054/839.014ms，不是AO或AQ耗时，也没有fresh FEX倍数。最终不降低质量与两模式≤2×目标未完成；重构中的暂时QoR缺口留到阶段收口恢复/验收。

证据：[AO实际合入与待验证状态](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ao-integration/applied.json)、[职责/净成本/覆盖缺口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ao-select-publication-birth/handoff.md)、[有限peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ao-select-publication-birth/peer-ir-middle.md)、[flags-only数值分配现状](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/flags-only-numeric-destination-review/review.md)。

增量证据：[AO总用途修正](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ao-total-use-guard-fix/applied.json)、[AQ合入](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/aq-integration/applied.json)、[AQ职责与净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/aq-flags-only-destination/handoff.md)、[generic有限复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/aq-flags-only-destination/peer-ir-middle.md)、[ARM发码有限复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/aq-flags-only-destination/peer-front.md)、[单fixture补段](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/aq-flags-only-destination/test-ir-middle-applied.json)。

### 集中构建与必要语义收口

main SHA `60d41a58cca15d16d253798885f92ed29e9893a867cc4b4102cb4b0aa2e3bc22`，IR `c7a838a3b70f3bf3ec4fb78f560efe2b79fc2fc3780a3486daa39d79ca7b1838`，swift_test `373d221c1b8eb66f562da964dabe4da56013f5e7c403261f289e2cbdd1f7b04a`；本轮明确重建此前缓存中较旧的swift_test。

红魔上只运行6个既有case：3个Select发布/覆写/故障、原flags-only spilled XOR及存活用途、原direct flag writers native oracle、原Low32 physical reuse加上述单段；全部112,062断言通过。direct case原有Block/Function/flags ABI/优化与PF/AF/GetFlags fallback循环未扩展，也不把其断言量认作新质量矩阵。原7,232B W59 flags guest精确复用，只跑一次Function=1/FLAGS_REGS=1，原PASS判据通过。四个设备文件核hash后删除，私有进程及目录关闭。未追加ordinary、lifetime、CoreMark、PMU或FEX测量；这批仅完成构建和必要语义，不代表最终质量/资源/两倍门通过。

证据：[合批变更边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ao-aq-combined.json)、[一次三目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-ao-aq/build.json)、[最少合同与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-ao-aq-contract-checks/summary.json)。

## 第三十五批：Facts块记录直达、edge语义合并访问、RMW地址单次构造

本批相对上一已验`64da58c6…`合入11生产文件及3个已有测试文件；源变更不含无关WIP。AR、AS、AT先分别在Data2冻结，再统一构建三个受影响目标，不逐包运行质量或性能矩阵。初次合批source `77fc1fe44354a92534e770fdeb875da34a7e926bde05289c909e7cf879065baa`在编译时发现AT只读查询调用旧`ForceCast`会丢const；只在新位移查询改用`std::bit_cast<s64>`并显式include bit，旧工具及其余调用不改。修正后source `f61eb3fc3cf4b7445d2f4b6f158697947b75d239a7ac28fb41a097900c54b77e`的同组三目标构建已通过，前后源码一致，generic Release/ThinLTO无PGO；原失败收据和原冻结peer身份保留。

AS处理真实存在的块记录目录乘法：`PrepareBlockRecord`、`FindIndex`、`FindBlockUses`、`FindSummary`不再逐次`find_if(blocks)`。HIRFunction的唯一`FindBlock`复用原`instruction_owner.hir_order`，核范围、非空HIR槽及真实Block指针；原Contains委托同一入口。Facts原BlockRecord容器直接用membership ordinal为槽，不加第二目录，Standalone仍为槽0。首次reserve完整member span后才发布inline ends/summary，空洞默认null；RPO/IR编号不代替membership，foreign同ordinal不能命中。mutable BeginInput仍终止旧借用并清空。首次准备由B²身份比较收敛为O(B)建槽，后续Q次查找由O(Q×B)收敛为O(Q)受检定位；这不是新测得的毫秒归因，也没有删除N/E或记录字段。高ordinal的稀疏冷请求会初始化前缀空洞，最坏构造B行而非旧M行，需计入净账。

AR只融合active LowerUnitEdges原本分别进行的NZCV prefix与透明性/可选write-summary访问，直接写既有最终EdgeBlockAction。两个原状态各自停止：prefix遇barrier/局部控制/AdvancePC后不再调用barrier；透明性若还需找后续writer则继续，已完成的任一状态不能截断另一个。单查询公共入口共用同一规则体。删除第二段重复iterator读取，保原effect/flag判断，新增局部done分支；token、acceptance、audit和inactive路径不动，没有新B/N表、DTO或证书。未将不同语义事实假装为同一事实，也未宣称全部edge处理消失。

AT覆盖ADD/SUB/ADC/SBB/AND/OR/XOR/INC/DEC/NEG/NOT八个handler、11种opcode的普通memory-destination路径，复用原RmwOperand/ReadForWrite/WriteBack。新资格只读decoded事实，寄存器目的先廉价排除；已知可用目标、native、完整64位地址、非LOCK、无真实段覆盖及受支持shape才通过同一ScalarMemoryQuery的ReadModifyWrite纯能力。Single准入原样，Shared/Atomic/Escaping拒绝；Unknown/early-off/不支持形态原Src+Dst不动，旧复杂AddSub/exchange特例也保留。在旧load位置只取一次EA，经原carry/flags/ALU后交原store，内存访问数量、宽度、TSO顺序和故障原点不变，不引入atomic或融合内存opcode。

真正少建的是原Dst再次物化地址的场景，例如Relaxed裸base的第二个GetOperand；TSO/已composite场景只认少一次地址构造查询，不把原本不存在的SSA当收益。新增纯资格查询、七个既有RmwOperand的optional局部及EA更长的真实活跃区间是成本，中间spill/QoR可能变化，不能据少一个节点直接宣称最终采用。

必要检查只沿已有case：AR将两处原固定透明性/晚writer期待接到融合入口，AS在原多块case补稳定借用、foreign同ordinal及BeginInput失效断言，AT在原RMW case补一条Known裸base固定例并保原Unknown期待。该例使用32B padded fetch buffer，查两次访问共用实际地址且比Unknown少一个GetOperand；无新scenario循环，唯一新增IR-count遍历如实记录。native INC/DEC现有独立flags oracle与Single SSE现例作为相关消费者检查，不开全ALU fuzz或质量矩阵。

证据：[本批实际边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ar-as-at-combined.json)、[AR职责与净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ar-edge-prefix-analysis/handoff.md)、[AR有限复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ar-edge-prefix-analysis/peer-ir-middle.md)、[AS职责与净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/as-semantic-block-record-owner/handoff.md)、[AS有限复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/as-semantic-block-record-owner/peer-ir-middle.md)、[AT职责与净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/at-rmw-address-once/handoff.md)、[AT R3有限复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/at-rmw-address-once/peer-ir-middle-r3.md)、[const位移构建修正](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/at-displacement-const-fix/applied.json)。


### 合批构建及必要语义结果

主程序SHA `2396315c5ff3be959aadab9d2df1fd63db716e0b8e68427f8c9c87c81996f710`，IR `537b99af8039886e65bf0ca7fdae440dd8b55922744f477ad834b97bf41de9cf`，swift_test `802815273ce5f2037e7ed9f212101d0b0d197fd9d1c798a60e8695ec9d31ea14`。红魔上7个现有Swift case共1,311断言、2个现有IR case共279断言全部通过，合计9case/1,590断言；新Known裸base的实际同EA/少1GetOperand期待通过。原INC/DEC native flags oracle、Single SSE、edge prefix/透明性/acceptance/canonical closure及Facts稳定借用/重置均沿原case执行，未加scenario矩阵。

原W59 flags guest精确复用一次Function=1/FLAGS_REGS=1，原PASS判据通过；四个设备文件核hash后删除，私有进程和目录关闭。本批没有ordinary、生成质量、lifetime、CoreMark、PMU或FEX测量。最近ordinary仍是旧135df的Block695.054/Function839.014ms，不绑定到本批；最终生成质量/资源与两模式≤2×门仍未验收。

下一步依据也作了只读核实：当前owned/sealed的order迭代、GetBlock/GetArg/ForEachValue/Value::Type没有逐operand TLS、全局目录、锁或二分。慢机制只在粗lease、detached及pointer Params/Lambda/foreign投影等真实边界，不能再认领一个“去全局owner查找”的大收益。resolved-alias index仍有discover/count/fill三次输入访问，但它是不同语义的派生视图；尚未决定可删除/融合方案，不以该观察新建缓存或图。

证据：[成功的同组重建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-ar-as-at-r2/build.json)、[九case与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-ar-as-at-contract-checks/summary.json)、[IR热访问否决与真实剩余边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ir-hot-owned-sealed-access-review/review.md)。


### AJ–AT多批合并后的普通compile规模核对

在上述已构建且通过必要合同的`f61eb3fc…`主ELF `2396315c…`上，只运行Block/Function各一个普通粗计时点。无新增PROF2/decode/IR dump、PMU或生成质量矩阵，两点CRC4983、exit0，ELF/guest/实际argv/env身份闭合，前后CPU4上限均1,171,200；未执行频率准入、配对净收益或fresh FEX分母采集。全部owned设备文件核hash回收、进程及目录关闭。

| 同点粗桶 ms | Block | Function |
| --- | ---: | ---: |
| translate | 688.099371 | 838.632411 |
| decode | 88.233653 | 171.945299 |
| rpo | 0.000000 | 4.593603 |
| opt | 90.708134 | 93.532389 |
| regalloc | 173.144856 | 174.727293 |
| codegen，含Complete与emit | 226.489251 | 262.458767 |
| publish | 37.256301 | 45.636459 |
| ir_free | 6.449685 | 11.204848 |
| other，含wait | 65.817491 | 74.533753 |

wait另记0.840165/0.162885ms，是other子范围，不再次扣减。Block为2,420单位/60,127编号IR/227,336 host bytes；Function为429单位/4,560解码块/70,620编号IR/326,112 host bytes。编号IR不是实际numeric allocation数，不能拿DISCARD覆盖数代填。与旧点的单元/IR工作量和采样时点也不同，不计算相邻因果收益或最新FEX倍数。

这批后仍在约0.7/0.84秒规模，尚未改变主要成本的量级。单次getter复用和少量形态少建值只能作局部改进；下一批优先减少真实payload/派生用途索引的整轮处理及目标完成工作，继续成组推进，不能把局部删除等同于两倍目标接近完成。codegen粗桶仍不能用旧profile比例拆成当前Complete/assembler毫秒。

证据：[两点完整收据与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-ar-as-at-ordinary-pilot/execution.json)、[同点成本账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-ar-as-at-ordinary-pilot/accounting.json)。

## 第三十六批：resolved用途准备、同次GPR物化、FPR view出生

本批合入12生产文件、2测试文件，source `ed425d29a704a3057844adcc4f26376067d881698b40dd7cafbd583fd5cefeec`；相对已验`f61eb3fc…`无无关WIP或删除变化。main、swift_codegen_test、swift_test已一次统一构建通过，源码前后一致，generic Release/ThinLTO无PGO；codegen目标才包含AU实际fixture，不把未重建的swift_ir_test称为当前产物。上节688.099/838.632ms仍只绑定f61，不能移作本批成绩。

AU保留有真实消费者的resolved视图，处理其旧discover/count/fill三轮payload访问。正常Function仅在既有local-roster has_bitcasts为真时预检本块BitCast的借用raw Users；有实际本块alias occurrence便将root discovery并入count。count先统计本地root分组，完整发现后以同一present位裁未affected计数，再prefix/fill；external收集移到fill，以免漏掉alias发现前的direct root用户。无新N/E/B表，复用原alias_roots/present/offset/cursor及输出；零alias不建派生数组，foreign-only仍探测实际输入并在首matching consumer结束后停，不声称中断同一Params内部。重复次数、按consumer聚组、程序序与raw/implicit差别保留。真实新增成本是有local alias时的opcode预检/已有fanout和未affected root的暂时计数；只供外块使用的local alias仍有预检成本，不把“最多旧三轮payload”说成全部工作上界。resolved_ready及派生has_bitcasts仍在成功后发布，失败retry清旧partial结果。

AV复用同次实际位置查询：GPRLocationAt的两条MEM出口用已解析canonical ID查原短reload段，显式输出既有row/null，非MEM/undefined清null；JitContext立即消费该slot/row，不再重复ValueMem和reload查询。逻辑id仍用于原scratch/pending账；required_backing/fault_backed、conditional路径再次Ldr、forwarded例外和每access租借顺序原样保留。非当前consumer公有UseGPR仍走原慢路，不跨Tick或allocation epoch缓存。Add的Value负probe可直接复用；Sub/And/Or/Xor的Low32负probe可能与WholeValue不同，四函数保原文，Definition/FlagsResultRegister等也未机械合并。额外outptr、栈上borrow和私有调用成本明列；reload行原本是短段搜索，并非删除全N扫描，本包只作局部重复查询收敛。

AX在已有GuestStateRead classifier已证明offset0 FPR Alias时，按新增可选纯能力、明确无flags及V32/V64/V128真narrow返回原槽的Value type-view，省最外层BitCast定义、一个输入occurrence及对应用途/后续节点工作。ARM64唯一启用该能力，前端通过公共BuilderCapabilities；Unknown/EarlyOff/RV保旧形态。检查的是direct Def及source-view宽度，不是递归canonical root；direct GetResult/V256/整数跨类拒绝，保留的更深BitCast/GetResult/wide split/bridge操作未被删除。数据源ReturnType与高lane原始位不变，不承诺zero-extension或清高位。decoder XmmScalarV显式声明其无flags数据读取，低级AppendInst仍创建独立定义。没有新planner、图、表或物理home选择。

AU只在原mixed-Params独立oracle fixture补direct-before-alias及只供外块用的alias边界；AV只复用现spill/fault/preserved/flags-only四case；AX一个固定case核三种低lane读取、原始dirty bits、generic fallback及真实MOVDQU→CVTTSD/SS的cap开/关效果。没有新scenario矩阵。两路有限peer及root全patch审无剩余确认mustfix；执行结果另列如下，不将有限检查当作最终生成质量验收。

证据：[实际整合边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/au-av-ax-combined.json)、[AU职责/净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/au-resolved-alias-owner/handoff.md)、[AU有限peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/au-resolved-alias-owner/peer-front.md)、[AV职责/净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/av-gpr-materialization-reuse/handoff.md)、[AV接口/MEM有限peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/av-gpr-materialization-reuse/peer-ir-store-owner.md)、[AX职责/净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ax-fpr-low-lane-view/handoff.md)、[AX有限peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ax-fpr-low-lane-view/peer-ir-store-owner.md)。


### 合批构建及必要语义收口

主ELF SHA `f7b948af6d97f41b228ed18399f7e677da68d1f341a1f49f90e9bb521399de2f`，swift_codegen_test `76ec8140d244eb664160dd68c2c0ee4c4c755794a7a491331cb4eb14ca5a10af`，swift_test `1a9ceada8aeb00aa7e59e98332f31b72aafc0f38046fa18cde91e5a577d62323`。只运行2个现有alias索引case（188断言）、4个现有spill/fault/preserved/flags case及AX一个固定case（共210断言），合计7case/398断言全部通过。原W59 flags guest仅一次Function=1/FLAGS_REGS=1，原PASS判据通过。四个owned设备文件核hash回收，进程与namespace关闭；没有新增生成质量、lifetime、CoreMark、ordinary、PMU统计或FEX矩阵。

证据：[一次三目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-au-av-ax/build.json)、[必要合同与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-au-av-ax-contract-checks/summary.json)。下一步仅准备一次当前Function native调用栈采样定位仍存主成本，不因微小边际删除推断已接近最终目标；最终质量/资源与两模式≤2×门仍未通过。


### 当前native归因及下一整组删除对象

在本批已验`ed425d29…`/main `f7b948af…`上只采一次Function native调用栈，未启SVM profiler或新增质量/FEX矩阵。实际目标PID1850条记录中，892条由原始已知栈前缀严格确认属于Translate服务，period为944,895,360，raw/API身份与原CSV服务period一致。最近stage marker互斥归属为decode/build22.23%、target Complete18.04%、emit12.19%、residual10.85%、allocation inputs10.59%、RA10.20%、Facts7.66%、other8.24%；这不是PROF2毫秒组成，不能乘旧ordinary总时间。另一套最近实际项目owner视图中IR为33.67%，跨多个阶段分布，不能和stage相加。未知叶/未知前缀不推定成guest或compile，947条额外unwind记录缺唯一显式关联字段，原数值和文本保留，不假报精确展开成功率。设备全部owned文件保留核hash后回收、进程与目录关闭。

这次证据进一步否决两个数量级假设：普通value visitor已无完整schema检查，极宽的参数读解释相关nearest-owner合计约4.41%，并非instr.h全部13.26%可省；native allocator相关叶约5.43%，CAS另3.61%，其中最大CAS实际落GuestMemory::RangeIsMapped共享锁调用者，不是IR引用计数。未据此继续换allocator或另开更多opcode微调。

下一组AY具体收敛同一静态clobber语义owner：旧FixedValidation Site只存default|(helper&low10)，GPRInputs另存full32+SetHost，Complete又解同一helper。计划由同一Site保存default及fullhelper两个真实量，删除正常GPR clobber行/范围并直接供Complete静态投影；bridge仍default-only，copy/窗口仍fullhelper+semantic SetHost，fixed验证仍low10。当前普通Describe本来就只解一次helper，不重复认领该收益。actual maps、Dirty、preserved transfer、physical points/owners及最终checker不被静态事实代替；manual/不同physical features按实际roster和显式features走冷纯target查询，不盲借全局ID范围或再造一份常驻表。

AY尚在私有实现，当前shared源码保持本批已验版本。Site8→12B的预计增加、旧8B clobber行及每块范围字段删除、冷fallback额外计算都须实际计账，不承诺净内存下降，也不把全部Complete18%说成可省收益。最终生成质量/资源和两模式≤2×fresh FEX门仍未证明。

证据：[本次native采集及严格归因](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-au-av-ax-native-cost/summary.json)、[同一raw的独立复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-au-av-ax-native-cost/peer-ir-store-owner/review.md)、[参数访问上界与否决](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/cross-stage-argument-access-review/review.md)、[固定范围查询有限结论](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fixed-site-scope-review/review.md)。

## 第三十七批：静态target-clobber的唯一owner

AY已精确合入13生产文件、1个原fixture，source `dd56b5f3013072a252ebc9725b0cb475012ea89c2e5090d040f0d91a2ad53fb6`，不动无关WIP。main/IR/swift_test已一次集中构建通过，源码前后一致，generic Release/ThinLTO无PGO；不把ed425d29的native点代作本批结果。

原`FixedValidationInputs::Site`8B只存default|(helper&low10)，`GPRInputs`另有full32+SetHost的clobber临时数组、最终vector、复制和每块first/count，Complete还会重解同一指令的default/helper语义。现在Site12B存default与完整helper两个量，按消费者派生：固定验证仍default|(helper&0x3ff)，bridge仍default-only，copy/窗口用full helper并无条件加语义SetHost home。pub helper窗口只用helper低10；state survival原政策不扩大，Helper flag即使mask0仍在。普通Describe此前已只解一次helper，不再次认领该收益。

真实删除为GPRInputBuilder临时clobber数组、最终GPRInputs::clobbers及复制、两个range字段和对应query接口；normal Complete借同一Site，不重复还原静态clobber语义。per-pool scratch clobbers/deltas、实际Map/Dirty/preserved transfer、publication、dynamic physical points/owners及final checker仍按原时点执行。它们与静态语义不是同一事实，不跨retry缓存。

仅精确Prepared、现Facts匹配input且已有成功SSA、全FeatureSet、sameindex和sealed/dense编号域均合格时借site；不凭单块dense假定manual全unit编号不重叠。Site owner增加一个真实FeatureSet值并保contract key，不借悬空指针；未新增认证位、逐节点表或证书。GPRCopy在foreign/manual/交错编号/不同physicalfeatures时沿实际roster、显式allocation.GetFeatures调用请求级纯target能力，null保守拒共置；保原inclusive窗口及“只对非零clobber行early-break”的旧filtered-table行为。公开AllocationView standalone和ownedlocalfallback仍用cold实际语义解析；生产PlanBuilder私有构造只同步借现Facts，不让新分析owner逸出。

净账不能只报删表：Site增加4S字节，删除旧约8K clobber payload、每受影响block8B范围字段及临时/最终vector元数据/容量；每unit新增FeatureSet值及一个纯callback，同步coalescing scope多optional span和资格查询。正常查询会遍历比原K更宽的site集合；冷路径由稀疏K行改为实际N指令/每query重解，可能更慢。既有Helper flag已涵盖helper，因此此次fullmask字段本身不新增site行，但没有宣称总体RSS下降或全部Complete18%可省。

一个原IR case只迁移CopyGPRInputs并增加既有zero-mask helper两投影/FeatureSet mismatch必要断言；最少复用原fixed width/helper/Function及sealed Low32 case，不增scenario矩阵。两路有限peer与root全patch审无确认mustfix，仍由统一构建及必要执行结果决定本批接线是否成立，最终质量/资源/两模式≤2×门另待集中验收。

证据：[实际合入与源码边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ay-integration/applied.json)、[职责/净账/冷路径](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ay-target-clobber-owner/handoff.md)、[generic与生命周期有限复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ay-target-clobber-owner/peer-ir-middle.md)、[target语义有限复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ay-clobber-target-peer/review.md)。


### 构建与必要合同结果

main SHA `b993a9c85d2e58156ee70b25db0275ac5781b6d1bbd1e6a298525dd6fa9c89c9`，IR `a7e3db255270b0d19859c92c0b3abba78227b211740a3f0d48a23ab86f3805f5`，swift_test `f9117107921e8f6a620fda540278a9fee01f0b79ef731dfe8d38b215c2819b1f`。12B Site编译断言通过；旧GPR Clobber类型/vector/range/query在受影响production/test support域引用归零。未把独立scratch clobbers/deltas误删。

红魔上只跑5个现有case：fixed-width endpoint/clobber families、helper preservation、Function residence segments共302断言，原prepared-fixed validation和sealed Low32共78断言，合计380断言全过。原W59 flags guest只一次Function=1/FLAGS_REGS=1，原PASS判据通过；4个设备文件核hash删除，私有进程及namespace关闭。没有新增scenario、ordinary、质量、资源、CoreMark、PMU或FEX矩阵；AY净耗时/内存及最终质量门未测，不用前一版本的688/839ms或native占比认领新收益，≤2×目标继续保持未验收。

证据：[单次三目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-ay/build.json)、[必要语义与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-ay-contract-checks/summary.json)。


## 第三十八批：Facts索引共访、PSTATE唯一结果行与GPR区间访问

AZ/BA已精确整合11个生产文件、2个现有fixture，source `fbe1cfed3d8e5805e5a1786781f5312631f54675d9e3fb6ca930dff7910c3cf0`，无关WIP保持。构建与必要语义检查按整批收口，不逐片运行生成质量、资源、CoreMark、PMU或FEX矩阵。本文本次记录的日期沿原实施文件名，收据使用实际UTC时间。

| 现有文件归属 | 本批实际工作与删除对象 |
| --- | --- |
| `compile/analysis/{block_index.h,semantic_facts.h,facts.cpp}`、`compile/optimized_unit.cpp` | 普通首次mandatory Facts访问直接填现唯一index roster，同步ordered/dense/BitCast存在性；删除之后另走链表的N访问和单独dense N检查。公共PrepareValueLiveness接口不变，OptimizedUnit经已有friend走private入口；不加ARM依赖或N/E中转表 |
| `compile/analysis/block_summary.h` | 同一ConsumerRoles分类复用原u32 presence一个Test位，摘要保持8B；无Test的consumer块不执行PSTATE前缀转移 |
| `arm64/compile/select/{flags.h,flags.cpp,consumers.cpp,integer_flags.cpp}`、`arm64/compile/pipeline.cpp` | 原独立PstateEligibility/Test表退役。私有prefix在consumers.cpp的既有候选扫描中直接填ConsumerAction.pstate_mask，IntegerFlags借唯一结果；删除正常第二N扫描、T行payload及T搬写。flags.cpp保真正token/carry职责，不再持第二套PSTATE描述 |
| `compile/allocate/gpr.cpp` | LiveView/Transfer/事务Rewrite用现有有序roster二分到实际闭区间；normal稀疏clobber site也从begin起步。省候选前缀重扫；实际物理检查、cold过滤/early-break、整块canonical-owner overlap与失败回滚仍执行 |

索引发布仍等完整SSA及原Function/all-reachable/use-count资格，失败/异常回收本次草稿；空块不提前配index，generic/manual/已有facts仍原按需路径。已有roster push及ordered/alias判定只是迁入必经访问，不算删除；原8N指针数组更早与SSA/flow临时量同时存活，不能称总峰值降低。目标Describe仍依赖未来legacy demand、完整homes/ends，故保留它的真实阶段，没有把整个ARM选择回调进未完成的Facts。

PSTATE始终先采test mask，再转移当前指令；direct/legacy定义、clear/set/invert、helper/vector clobber、ADD/ADC与canonical SUB/NEG极性、firstAdvance比较保留。prefix域指针及firstAdvance只在当前lowering作用域借用，StoreConsumers只提交原actions/audit，不带进最终UnitActions；foreign同ID仍需exact membership/opcode核对。standalone只用同一action owner冷算prefix，不强造其它consumer选择；cold有同action行排序和额外空控制header成本。正常有Test时只迁原状态转移，最终行数量/排序不增加；新增presence分支不称免费。

GPR有序窗口用lower_bound(begin)/upper_bound(end)保闭端点，Transfer用u32的bridge+1；unordered返回原roster，仍执行旧各循环的过滤和早停。所有scratch/Dirty/map读取与回滚不省略，也不将不依赖物理的静态site代替canonical owner。新二分使短块/全区间未必更快。没有新文件夹、包装层、逐节点证明位或跨attempt缓存。

这些是具体重复工作退役，不是2×验收结果。旧native样本的BuildImpl最近owner约0.11%，不能把整个inputs桶归本包；本批未作新普通耗时或FEX对照，不用旧688/839ms推算收益，最终质量/资源及两种模式分别≤2×目标仍未证明。

证据：[精确整合与全源边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/az-ba-integration/applied.json)、[Facts依赖与实现](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/az-facts-inputs-review/store-owner.md)、[PSTATE唯一owner](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/az-complete-work-review/handoff.md)、[GPR区间净工作](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ba-gpr-proof-windows/handoff.md)、[GPR有限peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/ba-gpr-proof-windows/peer-ir-middle.md)。


### AZ/BA构建与必要合同结果

AZ索引包的独立有限peer已完成，5项after/shared/终manifest一致，无确认mustfix；[复核记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/az-facts-inputs-review/peer-front.md)。

本批只构建main与受影响的swift_codegen_test两个目标，一次90步构建成功，generic Release/ThinLTO无PGO，构建前后完整source manifest一致。main SHA `d5a89d0a03ab2999a757f4ef0c55887c0dd082bbabd4d6b499e4def28de79474`；codegen test `3f1b8da95177fb32697d99229d6430f59c744246e34fc889496a0b3774d64622`。缓存中的swift_ir_test、swift_test仍为AY版本，不称当前本批产物。

红魔上仅6个既有case：mixed Params/alias索引，flags prefix极性，consumer inversion/foreign域，narrow carry availability，low32 live view与transfer/recolor，共313断言全过。未新增case/维度；原W59 flags guest只一次Function=1/FLAGS_REGS=1，原PASS判据通过。3个设备文件核hash回收，owned进程和namespace关闭。没有新质量、资源、ordinary、CoreMark、PMU或FEX矩阵，不据此宣布最终生成质量或2×目标通过。

证据：[单次两目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-az-ba/build.json)、[必要语义与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-az-ba-contract-checks/summary.json)。


## 第三十九批：worklist唯一顺序、physical不可用区间与shift载体闭包

BB/BC/BD R2已成组精确整合11生产文件、3个现有测试文件，source `80d0c4a9121adef6be90f97fbfc42f6dff55e509b86f4e3eff41febff3b67611`。两包对同一IR测试文件的不同hunk先在最小原始树合并，再一次应用combined patch；全源前后清单核实无关WIP未变。构建和必要语义检查仍按整批收口，不逐片跑生成质量/PMU/FEX矩阵。

| 现有文件 | 实际变化与保留边界 |
| --- | --- |
| `compile/passes/value_worklist.h/.cpp`、`compile/pipeline.cpp` | 协调入口选择在原roster访问中执行真实编号；普通live指令以Id加exact row pointer定位，不再先经StableIdentity/Store域/NodeId目录转换。raw墓碑先拦，stored队列仍经原handle/session/FindAlive解析；cold目录与粗lease不删 |
| `ir/hir_builder.h/.cpp`、`ir/block.h/.cpp` | 原唯一编号owner共享带visitor的同一实现，row与Id同步形成；完整Function同步唯一Definitions，reachable RPO及unreachable尾/Visible/bound保原义，独立Block更新原max_instr_id。公共手工默认、混合/raw顺序及仍借source FunctionUses的standalone Block保原目录，不改兄弟块编号 |
| `arm64/compile/verify/verify.h`、`physical_gpr.cpp` | 实际same-index sealed/dense观察域内，连续Unavailable点在原u64 points中记成一个start/end闭区间；其它kind仍全u32点。查询保全u32，先拒逆区间/超过u16域的begin，再显式clamp end；不把尾后65536截断为0。cold稀疏/无序/foreign仍原逐点数据与查询 |
| `arm64/compile/registers/state.cpp`、`select/scalar.cpp` | 现canonical U32 callback补齐Lsl/Lsr/Asr/Ror的Imm/Value八primitive；可在已有规则中少建数值widen/projection节点，无新cap/DTO。晚期ShiftMask融合同时核实际shift结果宽度，防去wrapper后用源宽度误解释计数 |

BB所有可能的constructor资源分配先完成：紧凑rows/初始queue容量、B级真实normal-origin粗lease、原Definitions容量及reachable B scratch。之后才在同一N访问写Id/order并填roster，最后把worklist发布给反馈；没有旧ID回滚日志或额外N“证明”扫描。正常省去4ΣStoreExtent目录初始化/写及热查询转换，但Function Reorder仍付一次原membership Find及Key/Id写，初始queue预留4N容量，原pre/post编号也仍在；公共编号新增B计数/预留成本。不宣称所有编号pass消失，也不把保留缓存容量说成RSS已下降。

BC保持当前DirtyGPR逐IR/queried-home读取及其它clobber/owner/FixedWriter合同，删除的是同连续状态的重复实体化。Unavailable低32用start16/endInclusive16，kind/home高组及其它kind的全u32编码不动；最后开始不晚于query end的同组run，以闭end是否覆盖begin判相交。R条run不超过原U个不可用点，减少8(U-R)有效payload及同一sort输入；新增128B暂态open-start和格式分支/编解码。FixedWriter前缀仍原样借给最终验证，真实physical检查与最后Certify未省略。几何容量、栈/堆实际峰值与查询净耗时未测。

BD沿既有primitive predicate、纯能力和FlagsIntent合同构造，不放开GetHost/GetResult/BitCast、dirty窄槽、SignExtend或Select族。portable数值结果按ret32归一，ARM八族普通/窄源staging/shift0与同寄存器路径真正写W；后续允许的零fold/literal替换仍保完整数值槽。机会覆盖与生成质量没有实测，不能按八opcode推导compile收益。

独立peer在未应用的BD R1找到一个真实接缝：`src:U64→LsrImm:U32(count33)→ZExt:U64→And:U64(1)`去掉ZExt后，旧matcher只看source/use/consumer64，可能从src bit33取值；原ret32把33归一为1，应取bit1。R1已完整归档并撤回，未应用或构建。R2在原matcher增加真实ret宽==use宽门，连原source/use/consumer宽门一起约束；一个原lowering case固定负控检查实际action，同时保原同宽正例。独立接缝复核确认mustfix闭合，不能把R1称为通过版本。

本批只局部迁移原worklist两case、canonical forwarding原case、physical endpoint原case及原lowering case，无新增scenario循环；未修改的mixed/raw/stale queue case作为必要冷路径检查。原flags guest只计划单次Function运行。最终生成质量/资源与独立两模式≤2×fresh FEX门仍未证明，本批不作新倍数或毫秒推断。

证据：[组合源码与WIP边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bb-bc-bd-integration/applied.json)、[BB身份/异常/净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bb-worklist-identity-review/handoff.md)、[BB有限peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bb-worklist-identity-review/peer-ir-middle.md)、[BC连续状态净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bc-physical-availability-review/handoff.md)、[BC root有限复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bc-physical-availability-review/peer-root.md)、[BD R2交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bd-canonical-shift-carrier/handoff.md)、[BD R1反例](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bd-canonical-shift-carrier/r1/peer-r1-mustfix.md)、[BD R2接缝复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bd-canonical-shift-carrier/peer-r2-ir-store-owner.md)。


### BB/BC/BD构建、必要检查与一次粗成本点

一次三个必要目标构建通过，235步，generic Release/ThinLTO无PGO，全源前后manifest相同。main `825fb352ed97b61e70d2d5ace67372fc8b6dac7bb660a857b0c4c3f29371f19d`，IR test `739db00db26375b3a762477aae6b0077eaaefe63a7f15740882b8afd791f108c`，codegen test `dd1c7f8884cfa06708162b30ed0656a9a8a4af554ec7e19fef7dc00f82ed29b9`；swift_test未重建，不称当前产物。

红魔上仅6个原case：IR的两个worklist反馈/epoch、未改的mixed/raw/stale队列及canonical forwarding共301断言，codegen的physical endpoint/clobber与实际宽度融合门共390断言，合计691断言全过。原W59 flags guest仅一次Function=1/FLAGS_REGS=1，原PASS判据通过；4个设备文件核hash回收，进程和namespace关闭。没有新的场景循环、质量/资源/PMU/FEX矩阵；这些检查不替代最终质量验收。

本阶段收口后仅补一次Block及一次Function普通粗timer规模点，source仍80d0c4a9、同主ELF、CoreMark2000 CRC4983/exit0，实际argv/env/ELF/guest身份前后一致，CPU4上限前后原值均1171200kHz，没有写频率。使用原SVM_PROF=2粗统计（Function保原per-unit输出），无PROF2/新增IR/RA/DECODE诊断；采集CPU0、目标CPU4。所有owned原始结果保留核hash后设备文件/目录和进程已关闭。

| 当前点 ms | Block | Function |
| --- | ---: | ---: |
| translate | 685.193 | 829.523 |
| decode | 90.922 | 172.266 |
| RPO/编号 | 0.000 | 4.663 |
| opt | 90.631 | 92.971 |
| regalloc | 167.355 | 167.103 |
| Complete与发码 | 226.922 | 256.588 |
| publish | 37.019 | 47.268 |
| ir_free | 5.914 | 10.934 |
| other（含另列wait） | 66.430 | 77.730 |

每点以整数ns闭合，other=translate−七阶段；wait是其中子范围，不再扣一次。codegen仍不能用旧native比例拆成本批Complete/emit毫秒。各一个点，无频率准入/成对收益/fresh FEX分母；不从与旧点的几毫秒变化宣称稳定收益或新FEX倍数。

实际counts：Block2418单位、59653 numbered IR、226804 host bytes、arena430296；Function430单位/4561 decoded blocks、70302 IR、326072 host bytes、pool159504/arena5808640。单位数与旧点不同，aggregate bytes/IR不是生成质量的因果证据。粗成本仍约0.685/0.830秒，没有数量级改善证据；分配与目标完成/发码仍是后续删除重复工作的重点，不能把已退的目录/run/shift family认领成整桶收益。完整生成质量/资源和独立两模式≤2×fresh FEX门继续未证明。

证据：[一次三目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-bb-bc-bd/build.json)、[必要语义和设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bb-bc-bd-contract-checks/summary.json)、[普通粗成本原始绑定与整数账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bb-bc-bd-ordinary-pilot/accounting.json)。


## 第四十批：Function出生批接入与scratch静态描述复用

BE/BF已整合12个生产文件、3个既有测试文件，source `99cdc7ecf0dfa5300ac7c16842924297b175778c20c0c8c95b517f2e60cb276e`。没有新增生产目录、pass、逐节点证明位或第二用途图；无关WIP保持。

| 实际文件 | 职责变化与删除工作 |
| --- | --- |
| `ir/uses.h/.cpp`、`hir_builder.h/.cpp` | 受控HIR Deferred构造只保实际payload/counts/owner，原IdByRPO的同一访问一次填唯一heads/order；正常出生不再逐节点走Prepared/Reserve/Begin/Commit。显式PrepareUses和真正冷入口仍能提前接入。只增一个实际HIR owner的粗生命周期指针，不新增Topology模式 |
| `ir/instr.h`、`block.h/.cpp` | 同owner构造期setter不触发登记；raw/foreign/missing source在计数和Params转移前接入并冷重放。公开detach在丢roster前接入，普通Destroy/Reset/bulk不为退休节点建立元数据；unchecked append收为private。公共Eager及非HIR Deferred保原路径 |
| `compile/allocate/writer.h/.cpp` | 从本次真实request借原不可变输入，Feature/FVI静态政策在同一入口核对；不包装第二owner或复制scratch表 |
| `arm64/compile/registers/scratch.cpp`、`resources.h/.cpp` | 最终必要访问借现压缩ScratchProfile.need和FVI默认fixed mask，以单调游标消费；精确Function Add/Sub/And/Or/Xor仍按本块rich flags重算，无XPOOL的scratch-only固定特例也保原规则。其余动态facts、DISCARD、Mul、Div128、lease、真实物理与最终检查保留 |

BE明确改变了受控normal初始用途调度：首次按确定的程序顺序接入，不记录和重放decode期间的历史登记尾序。公开Eager的可见顺序保留。有限caller复核没有发现normal语义依赖历史B,A顺序，但有界worklist的结果仍可能变化；这不是纯表示等价，最终生成质量须阶段收口。const查询不暗中建图，未准备的Definitions/FindDefinition明确拒绝。所有批接入资源在发布前取得；原反向图部分失败回滚仍执行。constructor不再为pending HIR预留尚不用的64行order。

BF没有删除整个scratch阶段，也没有把全Function flags替换为local需求。新增块级资格、一次profile定位及两个既有span的游标读取；保全部最终访问、动态工作和异常/冷回退。BE接入后的Unready尾序与PrepareUses的临时consumer收集/sort仍存在，留给下一片实际收敛，不能提前计作本批删除。

一次四目标generic Release/ThinLTO无PGO构建成功，源码前后相同。随后发现scratch用例实际归`swift_test`，初次误送`swift_codegen_test`得到No tests ran；记录完整保留，只补建真实目标，没有重跑已通过检查。当前main、IR、codegen、HIR lifetime和swift_test均有同99c源码的独立产物。

红魔上必要检查为5个既有Catch case、2096断言；construction fast1仅一次，保原失败注入模式并覆盖新的批准备失败/重试；原W59 flags guest仅一次Function运行。全部通过。两次私有namespace共6个文件核hash后回收，无owned活动进程或清理错误。未新增case/场景循环，未跑本批ordinary/CoreMark/PMU/FEX、生成质量或资源矩阵。

主ELF静态`.text`从5021024增至5150680字节（+129656），大部分文件总增长来自debug节；不能把模板/兼容分支成本藏在源码删除账外。该数据不证明运行或compile变化。本批没有新耗时或FEX倍率结论，独立Block与Function的最终质量/资源及≤2×fresh FEX门仍未完成。

证据：[精确整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/be-bf-integration/applied.json)、[BE有限复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/be-function-birth-adoption/peer-root.md)、[BF有限peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bf-scratch-description-reuse/peer-front.md)、[主构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-be-bf/build.json)、[补充真实scratch目标](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-be-bf-scratch-target/build.json)、[必要检查及路由修正总收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-be-bf-contract-checks/summary.json)。


## 第四十一批：首次用途图直接读真实顺序，sink不再摘链

BH采用R2，source `c1901a1a9ec3d071019b3ad6ffc591592128c1d83308553e118431dc489bbd72`；10生产文件、3既有测试文件，无关WIP保持。R1在独立发布边界仍可能借已退休wrapper的roster，未应用；R2在真实export前完成PrepareUses，完整保留R1材料。

| 实际文件 | 本批职责与删除对象 |
| --- | --- |
| `ir/uses.h/.cpp`、`hir_builder.h`、`instr.h` | 用真实三态PendingBirths→BirthsReady→UsesReady和同HIR owner描述生命周期。normal首次图按当前RPO及未达尾的实际指令顺序建立，删除8N consumer临时数组、issued-head收集/历史sort和Unready尾序维护；Ready及无HIR Deferred保原历史路径。cold pack只重收集输入，随后共用一次payload提交，不再递归整writer |
| `ir/order.h/.cpp`、`block.h/.cpp` | 唯一顺序提供同owner原子MoveBefore（null表示end）；normal/cold均保原link、owner和store lease，不erase、不分配。真正public detach先建完整用途，正常同Block移动始终留在唯一roster |
| `compile/passes/state_sink.cpp` | pending store留在原Block，以同owner splice晚移；先完成overlap/coverage决策和全部surviving Publish，再做可能抛的Erase。删除detached字段、重新adopt及catch重挂；观察/fault/部分重叠与原输入语义保留 |
| `ir/hir_builder.cpp` | standalone export先核exact parent，PrepareUses后才DetachBlock，避免导出后再借可先退休的临时roster。普通Function/sink不提前建E |

不能用Definitions或旧Id代替当前roster：input-free VOID可能不是definition，编号后的移动/删除也会使旧顺序过期。新增B字节临时reachable（inline32）、实际order迭代和membership Find；初次N sequence赋值只是改到首次构图，没有消失。真实N/E投影、24B occurrence、全部最终物理/width/fault合同保持；未新增N/B持久表、Topology模式或逐节点证明位。

R2加两处root测试迁移：现native standalone live-SSA用例切到生产Deferred工厂；原IR fixture修正private EditInst访问为已有Inst setter。首次构建因后者失败，生产代码未改，失败收据保留；修正后四个必要目标一次成功、generic Release/ThinLTO无PGO、构建前后全源一致。main `c6965bd0…`，IR `9cb29ca8…`，swift_test `8b86bdf4…`，HIR lifetime `7dea166a…`；swift_codegen_test未重新link，仍属BE/BF源码99c，不称当前产物。

红魔上5个既有Catch case/1195断言通过；construction与state-sink-failure各fast1一次，保原注入循环，覆盖首link后的失败回滚及victim删除后的安全退役；原W59 guest只一次Function运行。没有新增case/场景循环或完整质量/资源/PMU/FEX矩阵。5设备文件核hash回收，namespace和owned进程关闭。

### BE/BF/BH阶段的一次粗成本点

同一c190源码/主ELF，两个新进程各一次CoreMark2000，CRC4983/exit0，原始ELF/argv/env身份前后一致。保原SVM_PROF=2粗统计及Function的原per-unit输出，无PROF2/IR/RA/DECODE新增探针；collector CPU0、target CPU4。CPU4上限两点前后均1171200kHz，未写频率；没有频率准入、paired gain或fresh FEX分母，不能与旧点之差当因果收益。

| 当前点 ms | 独立Block | Function |
| --- | ---: | ---: |
| translate | 682.888 | 798.481 |
| decode | 90.159 | 147.667 |
| RPO/编号 | 0.000 | 6.193 |
| opt | 90.617 | 87.348 |
| regalloc | 165.158 | 166.341 |
| Complete与发码 | 225.552 | 256.987 |
| publish | 37.941 | 45.450 |
| ir_free | 6.272 | 11.353 |
| other（含另列wait） | 67.190 | 77.142 |

other以整数ns闭合为translate减七阶段，wait不再重复扣除。当前codegen仍合计Complete与emit，不借旧native比例拆毫秒。实际counts为Block2418单位/59703 IR/226992 host bytes；Function429单位/4560 decoded blocks/70291 IR/325980 host bytes，单位集合不同，不用聚合大小冒称生成质量变化。

这轮仍没有数量级改善证据。RA及Complete/emit当前合计约391/423ms，下一重点是target Describe/Complete对相同语义的重复描述与最终owner，而非继续把小getter/单opcode当根治。主ELF.text比BE/BF增51568B，真实资源净值仍待集中验收。完整不降生成质量及两模式分别≤2×fresh fixed FEX目标保持未完成。

证据：[实际整合与失败修补](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bh-integration/integrated-final.json)、[BH有限复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bh-initial-uses-program-order/peer-root.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-bh-r2/build.json)、[必要语义与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bh-contract-checks/summary.json)、[阶段两点原始绑定和整数账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-be-bf-bh-ordinary-pilot/accounting.json)。

## 第四十二批：目标语义直接写最终owner，输入与物理重试分开

BI采用R2，source `1053149f9259b0900fa04994772b75beebec7c31d90498f2aff5901ee2acc359`；13生产文件、2既有fixture，无新源文件/CMake条目，无关WIP保留。

| 实际文件 | 实际工作及退役对象 |
| --- | --- |
| `arm64/compile/registers/constraints.*` | 同一次必经Describe访问直接调用ConsumerBuilder，XOR真实输入次数合入既有ForEachValue并写最终copy位。只在同features、成功SSA、readonly/dense/ordered的实际输入域准入；Function用既有可达ordinal bound排除未达块的纯输出，包括VOID编号，不强建reverse uses或第二成员表。资格失败整unit清纯结果、留冷路径 |
| `arm64/compile/select/consumers.cpp`、`flags.h` | 同一个非拥有stack writer直接借最终consumer/audit vectors，候选事件暂存在实际稀疏行、求解后裁掉seed-only行。删除独立candidate镜像及正常Complete的N seed访问、owning ConsumerActions结果和Store搬运；原pair/terminal/FCmp/局部条件/PSTATE规则共享。保原4N row workspace，并复用其空位标记首次consumer，避免同时再持一份4N lease |
| `arm64/compile/select/scalar.*`、`pipeline.cpp` | 同一纯matcher直接写UnitActions的SelfXor/CopyInputDiscarded，冷查询仍复用相同精确typed view/总用途/局部definition条件。正常Complete删除copy N扫描、flags结果sort/merge/Certify搬运；Narrow在原依赖位置读最终copy位。BitInputCovered、width/MEM/reload、coverage/claim与物理证明仍留原阶段 |
| `arm64/compile/plan.*`、`plan_build.cpp`、`compile/allocate/writer*` | 真正输入请求与物理attempt分开。InputGeneration随新SetAllocationUnit/实际Abort前进，RA/FPR retry不变。UnitActions借实际immutable输入owner；最初Begin不扫N位，真实失败重试清物理列、保纯结果。Ready重入从最终排序后的实际行恢复纯集合，保零mask Test与relation-only行；新请求/Abort/features或audit policy变化清旧结果。每块借用在late IncomingZero追加前结束，纯prefix二分加真实late-tail查询，Seal仍做最终封口 |

同一个决策不再经过正常owning result→Store→最终owner的转换。公共/手工入口仍有明确owned query；显式Store可能在提前seed之后发生，R1会重复同ID，R2已按原schema合并并保持后续显式Store的真实prefix查询边界，取消自动reuse资格而不丢其它块的IncomingZero。

这不是所有目标工作都变成一次访问：真实关系/E窗口、稀疏求解、Function排序/裁剪、一次firstAdvance投影、late-tail查询和真实重试mask仍存在。最终vector临时seed容量可能为8S，高于旧4C加实际输出；新增粗粒度request/input/prefix寿命状态也有成本。无新N持久表、per-block publication索引或target证书；不凭少两次扫描承诺毫秒、资源净降或两倍达标。

首次构建仅因迁移fixture未限定父namespace的GPRSMask/FPRSMask失败；root限定`backend::`后生产代码不变，失败及精确补丁保留。修正后三个必要目标一次增量构建通过，generic Release/ThinLTO无PGO，前后全源一致：main `c5688578…`、codegen test `6862f70d…`、swift_test `5a73aea5…`。IR/lifetime ELF仍属BH，不称BI产物。

红魔仅运行4个既有case/282断言及原W59 guest一次：scalar final-owner/物理位清理、predicate borrowed-owner/显式Store、consumer窗口/receiving-edge和实际Block/Function FPR retry通过；未加case/场景循环。没有跑lifetime、CoreMark、生成质量、资源、PMU或FEX矩阵。4个设备文件核hash回收，owned进程与namespace关闭，测试后source仍一致。当前没有BI普通compile耗时结论；BH的683/798ms和旧R7 FEX倍率都保持历史身份。

下一批在私有Data2实现完整mapped U64 GPR publication延迟出生，尝试避免先建再被DSE删除的节点及登记/早期遍历；这不自动减少最终RA节点，跨guest延迟也不能冒充固定FEX的策略或既有QoR等价。实际fault/observer/块切换/配置/退出前物化，Unknown/manual/partial等保eager。该片尚未整合，不属于本次源码和检查。

证据：[整合与源码绑定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bi-integration/integrated-final.json)、[有限源码复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bi-single-target-semantic-owner/peer-root.md)、[交接与真实净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bi-single-target-semantic-owner/handoff.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-bi-r2/build.json)、[必要语义与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bi-contract-checks/summary.json)。最终质量/资源与独立Block、Function分别≤2×fresh fixed FEX仍未完成。

## 第四十三批：完整GPR发表延迟出生，覆盖写不先建再删

BJ source `2c95e7189a3d34b1133605a6df7c38b5feead01062c2b54322e13e8e8d9c0d7e`；6生产文件、1既有IR fixture，无新源文件/CMake条目。author从a6开始读取，与root的BI测试namespace修复无交叠；整合前七文件逐hash核对实际1053，全源只变这七文件，无关WIP保持。

| 实际文件 | 唯一职责与边界 |
| --- | --- |
| `compile/builder/guest_state.cpp` | 仅现Known/Early环境、fixed binding、真实GuestInstructionScope、同Block、完整8B非FPR且U64 source与Write准入。值仍只存原GuestStateForwarding bytes；同offset覆盖移到最后写顺序，至多16个键，满时先drain。观察前交换整批keys，再从同bytes读完整U64并经原私有构造发SetHostGPR，不另存Value/Inst图 |
| `compile/builder/ir_builder.h/.cpp` | raw AppendInst保真实Inst返回并先物化；typed Void继续同schema/AfterAppend，AdvancePC保原flags/byte意义而不每guest drain。preappend按既有guest-state observation effect及helper-backed纯家族先drain；换块/terminal/link/call-return、配置前清待写，narrow/FPR/unsupported读写及实际state-read miss先物化。所有外部edit/seal/value pass之前必须Flush |
| `compile/builder/instruction.cpp` | 原Commit的原地fold继续；正常GuestInstructionScope允许pending跨guest，但不跨Block。无真实pending取消不改变Unknown/manual旧行为；有真实pending取消或已交换batch出生失败，清bytes/keys并弃整decode unit，不承诺恢复上一guest、不造journal |
| `frontend/x86/decoder.cc`、`frontend/arm64/arm64_frontend.cpp` | 两个真实GuestInstructionScope前端统一在Decode正常结束flush、异常cancel。root发现ARM64入口不能靠“总会经过terminal”假设漏掉；generic builder优化不依赖x86私有收口 |

这批把原本会被覆盖的SetHostGPR出生、输入登记和早期扫描直接省掉。原DSE已经会删其中victim，因此不能把这项工作认作最终RA/codegen N再次减少。跨guest延后实际发表可能改变live range/coalescing，允许作为中间实现继续推进，最终QoR门仍保持。固定本地FEX在guest边界flush register cache，这不是其同策略复刻，也不能据类比证明恢复或性能等价。

新增成本是一个pending guard、实际observer判定、K≤16的键查找/移尾、drain时byte read与binding投影，以及coarse Block指针；没有N/B持久表、逐节点模式或第二恢复图。Unknown/early-off、manual ConfigureGuestState、无scope、partial/overlap、FPR与不支持访问保原eager。原generic DSE/state sink、物理恢复/width/MEM/reload/claim检查不删除。

两必要目标一次构建成功，generic Release/ThinLTO无PGO，前后全源一致：main `95145556…`、IR test `bc038f14…`。只跑两个既有IR case/907断言和原W59 guest一次：原helper/forwarding case在已有一个mapped U64组合接真实环境，证明覆盖写无节点/无uses、AdvancePC后仍未物化、观察前survivor按序真实出生；原commit/terminal/cancel case保冷手工行为。未新增case、shape循环或lifetime/CoreMark/质量/资源/PMU/FEX矩阵。三个设备文件核hash回收，owned进程与namespace关闭；检查后source仍一致。

当前只有main/IR ELF属于BJ，codegen/swift_test仍属BI1053，lifetime仍属BHc190。没有本批普通compile或完整生成质量结论，不用BH粗点或旧R7倍率替代当前结果。后续继续减少真实重复工作，完整质量/资源和独立Block、Function分别≤2×fresh fixed FEX仍未完成。

证据：[实际整合与完整源码绑定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bj-integration/integrated-final.json)、[有限源码复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bj-deferred-gpr-publication/peer-root.md)、[交接与实际成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bj-deferred-gpr-publication/handoff.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-bj/build.json)、[必要语义与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bj-contract-checks/summary.json)。

## 第四十四批：按签名上界证明scratch headroom，直接应用现mask

BL/BM共同整合，source `58499f1976ed8ad942ee175275f32188a7ec68895cc129335f58e38a5c540f1d`；4生产文件、1既有fixture，无新源文件或CMake条目，无关WIP不变。

| 实际文件 | 本批职责与实删工作 |
| --- | --- |
| `ir/ir_types.h` | 同IRMeta构造器从ir.inc签名生成argument Value-occurrence保守上界：Value/Lambda≤1，Operand≤2，optional按存在计，Params/未知kind无界。一个opcode常量字段使用原padding，48B IRMeta断言实际构建通过；不是每Inst/edge字段或独立opcode规则表 |
| `compile/allocate/scratch.cpp`、`internal.h` | 现Verify每块一次核同一Facts输入、成功SSA、readonly、existing exact index、dense/ordered及Function可达ordinal域。有限上界同时保守计入GPR/FPR，避免把整数view错当canonical GPR；实际MEM destination与terminal extras另计。真实dirty masks及outside pool能容纳最坏peak时，直接证明通过，省参数读取、ResolveBitCast、distinct去重和HasSpillReload查询。否则原checker、shortfall、local repair、诊断和retry不变；物理edit callers仍完整检查 |
| `compile/allocate/scratch.cpp` | 两条证明路径共用ScratchPeak，保原legacy preservation/reload互斥max与precise direct-sum公式。SSAValidation原必经访问确实调用CheckArguments和HasValidResultType，无新增逐节点可信标记或额外N证明表；不把public inputs.reload_bound当硬上界 |
| `arm64/jit/jit_context.cpp` | 当前fixed-clobber mask按set bits执行原Mark；已求出的allowed mask一次CPURegList Include，不再逐x11..x17重复提交。scope/Exclude、X87 ip0、cold/no-pool、SvmBegin/End、V scratch禁入、actual acquisition及combined-budget断言保持。未改vendor；编译器可能早已折叠部分旧常量循环，不据源码循环次数承诺运行收益 |

这只删除可由现有事实直接证明点的重复工作：原N访问、profile查询、固定约束和terminal reload发现仍在，失败点会先多付一次保守bound/destination/mask计算。Params、跨类保守FPR不足、cold/manual或未达Function成员可以全部回退，未测命中率、毫秒或资源净收益。它不是通过关闭mandatory scratch验证提速。

三个必要目标一次构建通过，generic Release/ThinLTO无PGO，构建前后全源一致：main `a8414493…`、IR `024d2d82…`、swift_test `c0d82c6e…`。红魔只跑两项原case/50断言和原W59一次：moved-request reserve case提供真实sealed SSA溢出/充足headroom正控，保每点full-checker oracle；原Select/EA full-emission pressure case消费实际scratch合同。无新case/loop、完整质量/资源/普通compile/PMU/FEX矩阵。4设备文件核hash回收，owned进程与namespace关闭，检查后source仍相同。codegen test仍属BI1053、lifetime属BHc190，不改称本批产物。

### 保留实际策略，继续完成P3b前提

BK有限审查确认`GPRInputs::fixed_publications`目前仍服务独立合法策略：coalesce/width关闭时rich publication可空而legacy仍复用home，wrapper多用途和extension-first顺序也不能由现rich/FVI替代。它原来没有sort或Complete二次搬写，因此本轮保留，未把另一份更胖table当删除成果。

P3b首次有限核查也确认不能直接移交旧workspace图：ValueUse键是mutable worklist row，最终ReId会压掉DCE洞；workspace borrow又不能逃逸到owning OptimizedUnit/Facts。直接硬接FunctionUses会增加16×StoreExtent heads、24E occurrence及不必要HIR order。该项保持未完成，随后按完整目标继续私有实现稳定Store/NodeId键与真实IR-owned按需服务，优化器与sealed index借同owner，退休normal重复关系。允许必要owner接口和中间容量变化，真实净账与最后资源/质量/compile门不缩减；不能用Function收益代替独立Block目标。私有P3b尚未整合，不属于本次source/检查。

证据：[实际整合与源码绑定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bl-bm-integration/integrated-final.json)、[BM有限复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bm-scratch-headroom-proof/peer-root.md)、[BM交接与成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bm-scratch-headroom-proof/handoff.md)、[BL mask范围](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bl-scratch-mask-application/review.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-bl-bm/build.json)、[必要语义与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bl-bm-contract-checks/summary.json)、[BK不同策略裁决](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bk-fixed-publication-input-owner/review.md)、[P3b原编号/寿命阻塞](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bn-block-uses-owner/r1/review.md)。最终不降生成质量及独立Block、Function分别≤2×fresh fixed FEX仍未完成。


## 第四十五批：独立Block可变与sealed用途共用真实IR owner

BN r2整合11生产文件及3个既有fixture，最终source `16d11ff1e46be367fd2d8ed0ccebd9299ac4ab04767abe5cda7a22ebd2fe1651`。无新增源文件或CMake条目，无关WIP保留。原R1关于编号与寿命阻塞的报告仍留在`bn-block-uses-owner/r1`，本批补齐真实owner与稳定key后实施，不把简单借旧workspace图当解决方案。

| 实际文件 | 职责与实删工作 |
| --- | --- |
| `ir/block.*`、`store.h` | normal独立Block在首个实际Users请求时才创建owner；纯Find、编号和Seal不强建。排除mixed/raw、已有Store use scope、HIR binding与Function所有者。图先完整构建后发布；持久writer属于图，外部workspace不逃逸 |
| `ir/uses.*` | 复用同一关系服务，用Store/NodeId heads表示Block成员，不建HIR Definitions/order数组。setter沿原Prepare/Reserve/Begin/Commit维护实际occurrences；Block顺序变更置dirty，最终ReId原N访问重挂原rings，Seal处理晚移动，无第二排序表。Function原顺序语义保留 |
| `compile/passes/value_worklist.cpp`、`dce.cpp` | 正常Block查询借IR图，删除该路径的`8N` links和`20E`用途。嵌套scope内首次创建时恢复仍存活的outer observer，最外层退出回到持久writer；原raw/mixed/HIR冷域保旧局部表示 |
| `compile/analysis/block_index.h`、`facts.cpp`、`semantic_facts.h`、`unit.cpp` | 在原必经schema/SSA访问中核实际owner、每consumer Complete及完整成员数，成功后索引借同图；第一次需求发生在sealed阶段时显式Prepare才建图。正常路径删除`4(L+1)+16G` raw CSR及count/fill的E重复访问。const查询不构建，不另加逐节点证明表；manual/partial资格不足保冷CSR |

代价同样保留：新图有`16M` heads、`24E` occurrence slots及实际domain/lease容量；这里M是含tombstone/空洞的Store extent，不是live N。未查询Block只增加8B owner；持久EditBinding放在图内，使共享FunctionUses对象也增加约80B常量（源码布局估算，未以编译sizeof认领）。真实dirty编号仍有E relink与payload定位；图容量跟随Block/Store，可比workspace活得更久。删旧数组不等于总资源或compile已经下降。

可变调度由原私有LIFO转为权威图history序，是本轮允许的中间QoR变化；不再养一份旧图模拟顺序。sealed Block的first/last/group仍跟最终实际程序序，Function保原规则，两种分配范围不变。

generic Release/ThinLTO无PGO的五个必要目标一次构建通过，生产source `a80e6798…`。红魔只运行5个既有case/131断言及原W59 guest各一次Block/Function，均通过。原`optimized-block fast1`在完成实际交接和发码后的回收检查崩溃：旧Block fixture只填addresses，未填已迁移的NodeHandle与Store lease；实际PC归约为CheckRetiredInstructions对空handle.store的解引用。共享一份capture实现后，只有`hir_lifetime_test.cpp`改变；仅重建该目标、复验同一mode通过，其余case与guest不重跑。最终source16d11的生产代码与a80构建完全一致，主ELF `597eea11…`、IR `ed0be905…`、codegen `815502c2…`、swift_test `54985170…`沿用该构建，修后lifetime为`a4f06d83…`。原失败记录保留，不改写成首次全部通过。

初次6份和复验1份设备文件分别核hash回收，两个namespace与owned进程关闭。本批未跑生成质量、资源、CoreMark、ordinary compile、PMU或FEX矩阵。BO的GPR事件owner与BP的可选分配列在私有副本继续按完整批次实现，未算入本source。最终质量/资源与独立Block、Function分别≤2×fresh fixed FEX仍未完成。

证据：[实际整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bn-integration/integrated-final.json)、[用途owner交接与容量](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bn-block-uses-owner/handoff.md)、[root资格审查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bn-integration/peer-root.md)、[lifecycle有限peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bn-block-uses-owner/peer-layout-ir-middle.md)、[五目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-bn/build.json)、[fixture缺口与最小复验](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bn-integration/lifetime-fixture-fix.json)、[结果汇总与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bn-contract-checks/summary.json)。


## 第四十六批：发表事实归唯一Site，可选物理列按需启用

BO/BP合批source `58bcafd76973a01f8473105a94c457f1a70e184b2b323fdaa96339553c12b8a7`，20生产文件、1既有fixture；17文件BO与4文件BP互不交叠，逐before/after hash精确整合，无新源文件/CMake条目，无关WIP保留。不对两个片段分别构建或跑矩阵。

| 实际文件 | 唯一职责与退役对象 |
| --- | --- |
| `compile/allocate/fixed_validation.h`、`arm64/compile/verify/fixed.cpp`、`verify.h` | 原12B Site内复用同一32-bit role word：helper承载原clobber mask，明确native HostWrite承载已解析source ID、stored view width与whole/full/fixed-home/SelectCapture事实。无inactive union或第二N表；默认manual/audit没有显式source，保原raw路径。实际构建保12B断言 |
| `arm64/compile/registers/constraints.cpp`、`detail/describe.h`、`gpr_constraints.cpp` | 必经Describe同次写Site，rich政策即时借该事件，不留跨append引用；删除原raw publication的push和独立sort，静态home/whole/width/fixed-home不再重新描述。rich40B能力行与legacy16B extension-first资格并非同义，仍各保其原强弱合同、窗口及优先级；legacy完整matcher前后字节相同 |
| `compile/allocate/inputs.*`、`gpr_writes.cpp`、`width.cpp`、`ties.cpp`、`spill_regions.cpp` | 删除AllocationInputs原raw Publication类型和vector。RA按原inclusive/strict端点借Site引用；Publishes保已Resolve的source身份，PublicationsFor保原front/back numeric窗口；初始clobber与local spill repair仍看全部发表，包括partial/未知source，不缩成只看rich候选 |
| `arm64/compile/registers/publication_windows.cpp`、`residences.cpp`、`state.h`、`verify/physical_gpr.cpp` | 同policy、同index且readonly/dense域才在原backward访问用局部cursor借稀疏事件；缺资格保raw。capture仍计入稍后判dead的whole写，next[]只存full写，因此删除其重复full复判；原observer/read/boundary清理、coalesced root与物理admission顺序不变。helper mask经角色getter保持原语义 |
| `compile/allocate/locations.cpp`、`writer.cpp`、`transaction.cpp`、`recolor.cpp` | 六个原可选vector分别首写resize/default-fill，不再每请求无条件初始化N个host read/write、width anchor/owner、Low32、AES项；Map/live_ends不变。缺列查询/transaction snapshot用原默认，rollback不分配，Reset clear逻辑长度并保capacity，后续首写重建默认值；Map.publication_elided独立语义保留。无新表、proxy、epoch或Allocation对象缓存 |

净账：raw表删除`8P`逻辑行及一个vector对象/独立sort；RA改为过滤S个现有稀疏事件，可能多访问非publication项，不能只记删除。可选列未使用时省N分配/填充，用到的列仍在首写付N初始化，retry/失败事务可能保高水capacity；新增empty/size guards和默认snapshot也入成本。没有把distinct rich/legacy能力硬并或转存到另一个publication vector。

三个必要目标generic Release/ThinLTO无PGO一次构建成功，前后全源一致：main `31a5bb8f…`、IR `5f066d41…`、codegen `0befa27f…`。红魔仅跑两个原IR case（native事件借同owner与manual/helper回退、legacy home复用及失败种类）和原可选列transaction/retry case，共3case/354断言；原W59各一次Block/Function通过。没有新增case、场景循环或lifetime/生成质量/资源/PMU/FEX矩阵，4份设备文件核hash回收并关闭owned进程/namespace。swift_test与lifetime仍属前一BN，不改称本批产物。

### 合批后的两点compile粗账

BI/BJ/BL/BM/BN/BO/BP全部接线后，只各取一个Block/Function ordinary粗点，未加IR/host/RA dump，沿用同一SVM_PROF=2 collector；Function原有425条unit日志如实保留。两个点均exit0、CRC4983，实际guest/main/argv/env绑定，设备文件收口。不因后续修改再追加点挑结果，也不将这两个点当收益或质量验收。

| 当前点，ms | Block | Function |
| --- | ---: | ---: |
| 完整translate | 700.265 | 813.041 |
| decode | 93.165 | 154.665 |
| RPO/编号 | 0.000 | 6.397 |
| opt | 97.965 | 89.128 |
| regalloc | 180.362 | 177.141 |
| Complete与发码 | 215.809 | 247.945 |
| publish | 36.154 | 47.746 |
| ir_free | 13.142 | 11.288 |
| other（含wait） | 63.668 | 78.731 |

整数ns按translate减七阶段闭合，wait不重复扣除。Block2418单位/59653 IR/227268 host bytes，Function429单位/4560 decoded blocks/70291 IR/326228 host bytes；集合不同，不把聚合差异当机器码质量变化。CPU4快照仍walt、min614400/max1171200、cur614400，只有只读观察，无频率准入或配对，也无fresh FEX分母。

这些点仍未显示数量级改善，RA加Complete/发码约396/425ms。后续以当前真实编译栈定位可整体删除的准备、位置与编码工作，不继续把小getter/字段或目录收敛当根治。只安排一条同main的Function native归因，不开重复质量矩阵。完整不降生成质量、资源及独立Block、Function分别≤2×fresh fixed FEX仍未完成。

证据：[精确整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bo-bp-integration/integrated-final.json)、[BO交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bo-gpr-semantic-owner/handoff.md)、[common RA有限peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bo-gpr-semantic-owner/peer-common-ra.md)、[BP首写/失败/净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bp-optional-allocation-columns/handoff.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-bo-bp/build.json)、[最小检查与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bo-bp-contract-checks/summary.json)、[两点粗账与原始绑定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bi-through-bp-ordinary-pilot/accounting.json)。


### 同一58bc源码的一次native归因

复用既有collector，只采一次Function，source58bc、main31a5bb8f、实际PID9238，cpu-cycles:u/1kHz/DWARF16KiB。设备场已核hash关闭；没有追加质量或FEX矩阵。2860条record sample全量对账，target1812条/1,959,187,691 period；在首unknown前存在真实Translate的strict compile为863条/918,583,773 period（target46.89%）。此处是user-cycle归因，不能换成普通毫秒或FEX倍率。

离leaf最近的recorded阶段互斥占比：decode20.45%、Complete16.91%、inputs13.19%、emission13.06%、residual9.81%、RA9.76%、facts8.29%、other8.52%。最近项目owner的另一张交叉投影中IR representation/storage合30.87%，不能与阶段占比相加；最大单owner order++仅1.63%，不支持把instr.h总额认作一个getter热点。GPRPhysicalChecks constructor祖先3.77%、FixedConstraintsHold祖先2.44%也属inclusive，不能简单相加或冒称全为可删验证。

首次offline因substring同时匹配实际Translate和nested lambda在CSV对账失败。原dump证明精确入口与旧substring的strict样本集合相同，适配只收紧实际入口符号，不合并两行；原失败、脚本与adapter证据保留，同一raw完成归约，无重采样。unknown leaf884条/991,932,028 period；944条unwinding_result原整数2:1、1:57、4:886均缺显式PID/TID/time，原样保留不猜关联；lost/truncated计数0不代表完整展开。

据此直接进入下一合批：BQ删除FixedValidation Step链镜像/hash/排序/二分，透明residence例外按需读唯一typed IR；该nearest owner只占0.84%，不作为数量级改善承诺。BR在真实Allocation所有权内维护稀疏声明ID枚举域，FixedGPRDefinition及热ID查询/借用pointer保原处；接各实际fixed声明消费者，删除从IR或全placement重找声明的访问。目录约24B控制+4F capacity、late插入移位和rollback维护均计成本，不要求零字节；不能把perBlock查询变成B×F扫描，不增第二N证明表，不删强制final reproof。这里记录该次采样后的实施决策；BQ/BR随后已在第四十七批合批完成必要构建/检查，未无故重跑原BO/BP用例。

证据：[完整有限归因](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bo-bp-native-cost/offline/report.md)、[原数与精确source/ELF/raw收据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bo-bp-native-cost/offline/receipt.json)、[BQ实删源码交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bq-fixed-transparent-source/handoff.md)。


## 第四十七批：退役透明链镜像，直接枚举真实物理声明

BQ/BR共14生产文件、2既有fixture，无新源文件/CMake条目，source `c16a9bf476ac54ad9e6639d271140ecd6207136d615a297a19655df1527eba27`。两个private package同源于58bc；`fixed_verify.cpp`和`verify.h`按精确hunk组合，保留双方改动，未整文件覆盖。无关WIP保留。

| 实际文件 | 本批责任与实删工作 |
| --- | --- |
| `compile/allocate/fixed_validation.h`、`inputs.cpp`、`arm64/compile/verify/fixed.cpp`、`verify.h` | 完整删除Step/steps/StepFor、eager DescribeSteps递归、unordered_set去重和pointer sort。FVI不再拥有一份instruction/source/type/parent链；native publication Site与原Block域保留 |
| `compile/allocate/fixed_verify.cpp` | 只有真正的transparent-residence需求才读真实typed arg0链。原四parent严格为ZeroExtend32To64/ZeroExtend32/ZeroExtend64/SignExtend，首节点另许SetHost，首BitCast不放行；后续BitCast直接投影、后续SetHost停止，每边保实际Value.Type而非ReturnType/alias-root。所有fixed placement/shape/window reproof和失败分类保留 |
| `compile/allocate/allocation.h`、`locations.cpp`、`writer.h`、`writer_locations.cpp` | 固定声明本体仍唯一嵌于原gpr_placements，hot ID query与已借FixedDef指针不搬动。实际Allocation维护有序u32声明ID目录；Plan写前预留和完成可抛资源，再插/删成员并写本体。new request/Abort同步清，Reset和动态location epoch保声明；span借用在下一次insert/erase/request结束 |
| `compile/allocate/transaction.cpp` | rollback先删全部touched当前成员，再按原placement snapshot恢复有效声明与本体。目录capacity至少原F0，恢复无分配，且不复制整表；只对新增目录作该承诺，原unordered publication/use-map恢复没有被改称整体noexcept |
| `home_index.cpp`、`preservation_index.cpp`、`writer_locations.cpp` | home/preservation派生索引与preservation actions直接遍历实际F个声明，删除原完整placement重发现；原区间投影、排序、transfer和scratch/ABI规则保留 |
| `fixed_verify.cpp`、`arm64/compile/verify/physical_gpr.cpp`、`verify.h` | common首访homes与最终声明检查、target FixedHomes在合格块通过binary bounds只取Fblock。必须同实际SourceBlock、readonly+dense+ordered及原FVI exact index/SitesFor门；cold mutable/sparse/foreign保原实际roster与首个failure kind，不改成每块扫描整个Function的F目录。writer/operand窗口与强制最终验证仍执行 |
| `compile/analysis/block_index.h` | 索引记录实际source Block：BeginBuild清，成功FinishBuild最后发布，pure getter不构建用途/SSA。补足foreign index不能凭同数值ID冒充当前块的归属边界；这是每index的coarse owner，无每node证明位 |

BQ退役原24B Step逻辑行及hash buckets/nodes/vector控制、链描述与排序。新需求链可能被不同窗口重读，不能把0.84%的nearest owner当整个inputs收益。BR新增24B vector控制＋4×capacity，初cap4；每index新增8B actual owner。声明插入/删除/目录rollback都有真实移动成本，初建非单调与late插入最坏累计O(F²)，touched恢复最坏O(TF)；clear保capacity直到真实Allocation结束。三个原fixed全placement扫描改F，合格块访问为O(logF+Fblock)，不声称保留的writer/operand N工作消失。

四个必要目标generic Release/ThinLTO无PGO一次构建通过，全源前后相同：main `30119297…`、IR `0a9e5e70…`、codegen `0e0209d0…`、lean cases `c496e1db…`。BR原allocation.cpp用例同时属于swift_test和swift_cases_test，本批选后者避免整套聚合器重链。红魔只执行原fixed-validation views/failure case、原memory/publication receiver case及两个原fixed-home/transaction case，共4case/187断言；原W59各一次Block/Function通过。没有新case/循环、额外lifetime、CoreMark、生成质量、资源或PMU/FEX矩阵。5份设备文件核hash回收、owned进程与namespace关闭。swift_test和lifetime仍为旧BN产物，不称本批。

本批没有普通compile或native新点；700/813ms与863条合格native样本仍绑定58bc。下一BS在真实声明修改入口增量维护现有home index，避免Ready索引每次late改动后全量F构造/sort；其后在第四十八批整合，不计入c16。最终不降生成质量、资源与独立Block/Function分别≤2×fresh fixed FEX仍未完成。

证据：[精确组合及source绑定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bq-br-integration/integrated-final.json)、[BQ交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bq-fixed-transparent-source/handoff.md)、[BR实际生命周期和成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/br-physical-declaration-owner/handoff.md)、[BR有限peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/br-physical-declaration-owner/peer-owner-lifetime.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-bq-br/build.json)、[最小语义与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bq-br-contract-checks/summary.json)。

## 第四十八批：声明修改直接维护已有冲突索引

BS R2整合8文件（7生产、1既有fixture），source `102777a86104483c48c03a9b6ee984ffe85cb4657b7b43d5b8293b26d93d29aa`。无新源文件、目录、CMake条目或第二索引。`constraints.*`的FixedGPRHomeIndex以同一投影函数服务初建与修改：只有home/end/transferred_home/transfer/last_use改变才删除旧区间、插入新区间，并重算受影响home的prefix最大end。原Conflicts查询、端点和忽略owner语义不变。

`home_index.*`只在Ready状态转发修改，dirty不提前构建。`writer_locations.cpp`先完成journal、placement、publication及必要容量准备，再更新索引和唯一FixedDef。`recolor.cpp`、`writer.cpp`的实际着色/局部spill路径不再在末尾无条件丢弃索引：固定声明的修改已全部经过Plan入口，局部spill也先拒绝fixed/transferred residence。preservation依赖动态位置，其原失效仍保留；transaction rollback、新请求和Abort仍触发重建。这与仅删除失效而没有维护入口的旧错误方案不同。

净删除的是Ready修改后的全F重投影和sort，不是消除全部区间成本。原vector仍可能移动O(I)个区间，受影响home的prefix更新为O(I_home)，容量不足仍增长；连续非单调修改最坏仍可二次。首次/dirty重建、所有强制物理合同与最终验证保留，没有新增每node证明位或缓存快照。

R1归档后，R2把两个真实调用者的接入一并构建，不拆成两轮。generic Release/ThinLTO无PGO、前后全源一致：main `bb53c47c…`、codegen `962de4ea…`、lean cases `2d4b98c5…`。红魔只跑两个原fixed-home/transaction case和原GPR finalization retry case，共3case/67断言，原W59在Block/Function各一次通过；4份设备文件核hash回收、owned进程及namespace关闭。IR测试产物仍为BQ/BR，未改称BS。本批没有生成质量、资源、lifetime、CoreMark、ordinary、native、PMU或FEX矩阵，未声明耗时收益；最终质量和两模式分别≤2×fresh fixed FEX目标继续保留。

证据：[精确整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bs-integration/integrated-final.json)、[实际写入口与调用者交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bs-live-fixed-home-index/handoff.md)、[有限生命周期审查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bs-live-fixed-home-index/peer-update-lifetime.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-bs/build.json)、[最小语义与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bs-contract-checks/summary.json)。

## 第四十九批：初建声明批次与实际出生前折叠收口

按2026-10-30重写方案恢复实施，BT/BV统一整合18文件（16生产、2既有fixture），source `c4892c3e9c323e751d37a0116862faff412a8fd356e55de218f9dc416bfc7d01`。BU封口快照草稿有实际可写别名问题，保持未整合，不作为本批或P6前置。

BT保留原Plan提交顺序与唯一FixedDef本体，仅对已证明无目录/冲突查询的native prefix和cold inputs.fixed安装loop用同一ID目录append；ScopeExit成功/异常离开均恢复有序，首个EarlyResultHomes/Conflicts前关闭。已有定义更新不重复append，clear在同目录移除；Ready interval索引仍逐Plan即时更新。close先is_sorted，只有非单调才sort，替代真实分family安装产生的反复移动；保留late insert/erase、任意cold clear与dirty索引重建成本。无第二F/N表或sort getter，未声明总耗时收益。

BV在Known＋EarlyOn＋真实GuestInstructionScope＋显式FlagsIntent::None域，直接用同一浅width/raw-slot与literal predicate/select规则消费typed参数。命中直接返回叶值/只出生最终LoadImm，删除首次codec/count/登记后再入pending、Commit和SetInst改写的工作；HIR Deferred本来没有eager occurrence登记，不能重复计省。未命中不再重跑同次出生后规则，后续输入变化交最终residual，过程生成质量可变化。未造临时Inst、第二计划或逐node证明位。

真实decoder.cc和decoder_alu.cc的79处纯转换/谓词/选择显式提交None，Save/GetFlags仍挂原ALU/逻辑producer。Unknown intent、raw Inst*、early-off及Unknown目标保原新路径准入边界；真实None可让已有portable alias在Unknown也合法命中，但不借未绑定canonical host性质。raw BitExtract读完整slot，窄edge view不截断原值；Select所有输入包括未选臂先验证。79处只是源码接线数，缺当前动态命中/少造记录的证据，不当作性能覆盖率。

main、IR及lean cases三个必要目标generic Release/ThinLTO无PGO构建通过，前后全源一致：main `b93d7d43…`、IR `e1a984a6…`、cases `98b688b5…`。红魔只复用两个既有width/predicate case及一个fixed-home case，共3case/11114断言，未新增case/循环；原W59在Block/Function各一次通过。4份设备文件核hash回收，owned进程与namespace关闭。codegen/lifetime仍为旧产物，不改称本批。没有QoR/资源/lifetime/CoreMark/ordinary/native/PMU/FEX矩阵。

本组属已有改动收口，不继续按小包排队。P6下一主阶段迁移普通scalar publication的实际分配位置选择，退出同覆盖late全block干扰扫描和Map改写；WidthRoot多publication组件与尚未替代家族保原路径，最终物理合同继续执行。完整生产阶段闭合后按新方案取少量before/after端到端方向点，不等待最终质量矩阵，也不把单家族预设为数量级收益。

证据：[整合与源码绑定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bt-bv-integration/integrated-final.json)、[BT调用链和部分提交](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bt-fixed-initial-install/handoff.md)、[BV接线与净工作](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/bv-value-birth-forms/handoff.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-bt-bv/build.json)、[最小语义与设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-bt-bv-contract-checks/summary.json)。


## 第五十批：普通位置owner接缝、residual入口及代码对象发布

三项依赖改动合并构建，25个生产/既有测试文件，最终source `d45483121c80cf37c6e150251e77fa86cff913cd48cc09b6000cbd92ca92ecec`；主ELF `9810f46c244b1219524330d9fe47707a29a76b8e3f5cb1815ed39e07be143bed`。IR fixture重叠hunk按内容组合，保留双方改动；无新增源文件、CMake条目或测试case/循环。原代码/Git和无关WIP保留，未提交或push。

### 实际删除和仍需支付的工作

| 范围 | 实际变化 | 边界及新增成本 |
| --- | --- | --- |
| 普通scalar publication的birth选择 | 使用现有Publication标志和Preference，在真实active/live/pool/CFG/控制边界下选home；退出已迁域early-hard及两个late coalesce重搜/二次Map。fixed、REF、width、guest read和recolor经共同writer约束，旧owner不能被隐式覆盖；retry先撤elide再撤本次动态声明 | 仍用既有fixed声明承接该接缝，成功增加目录/index维护和通用preflight；保留其它策略及最终物理证明。TSO load、显式ZeroExtend wrapper及复杂component等保余域。它是接缝原型，非完整P6 |
| residual初始匹配 | typed shape快速排除不能命中的局部规则，仍做必要传播；受控生产调用显式CurrentInputs，仅在无局部匹配、无scalar CSE删除窗口反馈机会时免建整个value epoch/roster | public/manual默认RuleFamily，保留未来LoadUniform→LoadImm等改写机会；修改后重分型，DCE缩短CSE窗口的反馈仍闭合。新preflight自身付费；没有宣称减少IR N/E或已退役全部residual |
| code object发布 | fault entries按真实代码对象一次move/发布/retire，保等start恢复优先级；无真实磁盘缓存消费者时免建序列化镜像；复用同一封口edges的canonical-entry决定 | fault/SMC/link/entry及实际code allocation保留；失败未发表代码按原owner撤销。不是整个Function发布OOM事务，不代表全部publication/other预算已解决 |

### 必要正确性和产物身份

首轮构建仅在新fixture调用受保护生命周期API处失败，生产对象均编过；fixture改用公开PlanBuilder入口后五个必要目标通过。其后既有负向fixture试图用MapRegister隐式撤回fixed声明，被新增owner合同提前拒绝：只修fixture，先显式撤回未提交声明，再故意安装坏结果并保留原独立failure-kind及VerifyThrows断言。未放宽生产检查。

r3最终五目标构建通过。生产代码相对r2完全相同，主程序、cases、fault、entry四个ELF逐字节一致，复用此前已通过的证据；只重跑修正的IR负向case一次。最终覆盖9个不同Catch case（IR8、lean allocation1）、既有fault模式511/24/6/11检查、entry native77检查，原W59 guest Block/Function各一次通过。初次IR有7通过/1失败及331通过/1失败断言，修正case为45断言；不把重叠前缀相加成最终总断言。两次检查的设备文件核hash回收、进程及namespace关闭。codegen、完整lifetime等仍为旧产物，不改称当前检查。

证据：[精确整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compile-owner-integration/integrated-final.json)、[最终构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-compile-owner-group-r3/build.json)、[检查及失败修正汇总](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compile-owner-integration/contract-summary.json)。各私有补丁及review仍独立保存；BU封口快照和整Block用途图提前回收支持包未整合。

### 一次预定四点：完整时间没有一致大幅改善

为回答“共同owner新增查询/目录成本是否抵消已删除工作”这一具体问题，只做一次接缝方向实验。before c489/main `b93d7d43…`，after d454/main `9810f46c…`；同Release O3/ThinLTO无PGO、同CoreMark guest/argv/env/独立分配范围/服务端点，预先固定Block before→after、Function before→after，四点全部exit0、CRC4983，实际身份前后匹配。

| 互斥粗桶，ms | Block before | Block after | Function before | Function after |
| --- | ---: | ---: | ---: | ---: |
| decode/build | 96.492 | 96.317 | 153.821 | 160.354 |
| RPO | 0.000 | 0.000 | 6.269 | 6.847 |
| residual opt | 100.560 | 97.921 | 88.799 | 90.133 |
| allocation inputs/RA | 174.197 | 166.766 | 166.485 | 170.446 |
| Complete/emit | 224.439 | 220.524 | 242.269 | 248.230 |
| publication | 36.522 | 32.061 | 45.922 | 36.679 |
| 显式IR free | 13.300 | 13.077 | 11.349 | 11.270 |
| other，含wait | 65.960 | 64.388 | 77.972 | 79.877 |
| **完整translate** | **711.472** | **691.054** | **792.885** | **803.837** |

原始整数ns闭合。单点变化Block−2.87%、Function+1.38%，既不能判统计收益，也不能判最终回退；结论仅是**没有显示一致的大幅改善，不能支持继续把同类小包当作性能主线**。publication点值两模式下降，但三项成组改动不可分离因果，更不能用桶内下降代替完整compile结论。

Block实际请求2418→2419、IR59748→59631、host bytes227480→224724；Function请求430→430、decoded blocks4561→4561、IR70240→70240、host bytes326332→325880。请求微差与机器码变化限制比较，IR/host计数不是生成质量结论。所有8份只读设备快照为walt、min614400/max1171200/current614400，温度约33℃；没有频率写入，也不把快照当作锁频或统计资格。四点设备namespace核hash清理并关闭；未追加点挑快值。

本次`SVM_PROF=2`不等于`SVM_PROF2`：实际env未开启细粒度timer或decode细计时。旧R7细插桩的十万级clock不能解释这四点；Function保原unit输出，Block无unit输出仍慢。粗计时/日志自身绝对成本尚未量化，不把它全排除，也不据此宣称700ms都是观察成本。

证据：[预定范围](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-compile-owner-direction/plan.json)、[执行与原始身份](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-compile-owner-direction/execution.json)、[整数账及只读状态](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-compile-owner-direction/accounting.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-compile-owner-direction/closure.json)、[观察成本复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/profiler-observer-cost-review-20261030/report.md)。

### 主线调整

执行主方案6.5：不再优先增加birth-only、局部shape/getter或小回收支持包。下一边界审计普通RA的canonical Map/REF、fixed/preserved、SpillReload如何直接贯通target preparation和真实emitter，以及架构副本/width/fault的版本折叠还有哪些整段工作可退出。RA Map本来已经存在且不会被BeginLocationEditing擦掉；epoch失效的是另一语义的guest residence/view/use-home。不得误把二者当同义表盲删，或将所有soft选择变成fixed并增加证明负担。

构造侧并行找可整段减少真实N/E、初始图或全轮遍历的主改动，复用已有证据，不开新逐IR探针。共同目标是缩短每个值贯穿流程所付的工作；若仍保留数百ms，明确改实际算法，不再以“后面还有小包”解释未达标。本批不做完整QoR/资源/PMU/FEX矩阵；最终质量不降及两模式各≤2×fresh fixed FEX仍未完成。


## 第五十一批：实际前驱发现与按block闭合state流程

source `0230aa8ef88b13968a75261af099ca5ca4a2178cc0def0aa8bf3e149c4a92aff`，8个文件，无新增源/CMake项；两个算法源片段按精确scope整合，IR fixture保留双方hunk，无关WIP不变。主ELF `b624ccfd89d53459bbd7b1ab1fcffc70c5a6c00c28cc8f93863f590992ba39ee`，IR `f744e0c6d99c45ee47958d7290986bf7e9d9824ded3616b0ca54d8f13c0a3226`。

- **LongStep完整前驱发现**：从真实typed operand反查全部先前step，仍保同home/closed判据、原ordinal升序和重复operand的link去重；已有ordered index限定真实开窗口，原views按位置二分。删除逐step整block跳过窗口外节点、枚举所有历史step和逐operand线性找view。只有L个临时u32排序索引，没有新N表、Fixed Step镜像或物理选择改动；非有序manual保原raw窗口/view查询。复杂度按真实边与窗口入账，长重叠窗口最坏仍可二次，未认领整个inputs成本。
- **按block闭合state调度**：生产`OptimizeState`贯通原Cleanup→Sink→Finalize块内相序。一次实际opcode角色扫描证明无logical Load/Store时，替换原forward扫描，不初始化/回放byte表，并退出无输入的sink/finalmap两轮；原fault-aware DSE、partial宽度和before/after/XMM repair判据保留。有logical输入仍走原算法；uniform_fast=OFF及公开manual单pass保留。没有N/E减少；logical命中的新增分类前缀和no-logical的N扫描照常计入opt，子timer下降不当总收益。

根审和独立peer已核跨block无共享transfer状态、state决定不读use-count，且没有在state间Drain dead/value队列；因此按block调度不依赖别块source的改写顺序。pre-DSE只删指定store，原Erase通知不附带DCE，可由order.size差更新局部阈值。原state forwarding case改调生产入口，并在同case补mapped-only三写/MayFault见证：最早覆盖写删除，fault前和最终发表保留；没有新增case或循环。LongStep在原sparse case内增加三步dense关系断言。

两个owning目标按原Android generic Release/O3/ThinLTO无PGO、Data2固定cache合批构建通过；源码与配置前后相同。只运行3个既有IRcase共136断言、原W59在Block/Function各一次，全部通过；3份设备文件核hash回收、namespace及owned进程关闭。cases/fault/entry仍属d454，codegen和完整lifetime仍为更旧产物，不改称0230结果。没有性能、QoR、资源、native、PMU或FEX矩阵。

本批是实际算法和一项完整块内state服务的结构交付；不是完整P2/P6或两倍目标完成，也不再单独为LongStep helper取点。完整guest-copy生命周期服务仍在私有producer→consumer迁移和独立审阅，没有混入本构建；当前耗时未知，上一份d454的691/804ms只保其原身份。

证据：[精确整合与scope](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-state-algorithm-integration/integrated-final.json)、[LongStep算法与默认开关复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-algorithm-cost-review-20261030/report.md)、[state净工作/语义审阅](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/construct-major-cost-review-20261030/report.md)、[state独立peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/construct-major-cost-review-20261030/peer-review-bv.md)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-state-and-input-algorithm/build.json)、[最少检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-state-and-input-contract-checks/execution.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-state-and-input-contract-checks/closure.json)。

## 第五十二批：完整guest-copy生命周期服务与方向判断

当前source `8716f551187d0cfa27dd24ea5ef65d90e896984a0f3cf0cacd9b6f19c26bb779`；before0230。私有18文件补丁8ace478f…经独立有限源审后整合，无新增源/CMake项；Root补同一scope内直接owner头文件。无关源码/Git/WIP保留。

### 实际服务和净成本

native退出逐operand incoming收集/排序、unselected逐E pair/hash写入及fault active副本快照。Allocation拥有实际copy区间、独立home-width runs与真实captured fault边界；按raw定义/width/home/publication排序，typed查询经实际Inst/Block贯通selector和emitter。selected exact-use仍是稀疏例外，cold/manual保原语义，不将其与canonical Map/REF硬并。

保留一次真正architectural N事件fold、static catalogue、Function entry-width、extension facts及最终physical Certify/Freeze。新增P/W/F/B容量、排序/安装检查、typed解析/区间查询、真实GetHost用途owner按需准备、recolor对actual copies及相关uses的guard；首次has_use_homes还可能建立既有N大小placement列。没有当前动态P/E/query/冷域覆盖census，不把支持域静态广泛当运行覆盖或把旧E表字节减少当净提速。

保持latest exact-width先于narrow eligibility、last-success typed occurrence、selected pair整体覆盖、selected additional/count与preserved过滤顺序，以及publication<boundary≤last的fault判据；copy.extension与home high32事实不同。REF/shared source/preserved身份在native投影安装后由唯一writer强制；Reset/NewInput/Abort清区间，BeginBlock不清前面Function块，必要重配先结束原规划域。

`TakeIR`立即销毁Facts/BlockAnalysisIndex；新发码查询只借Prepared内CompileUnit保活的真实IR，没捕获旧index/span。真实resident计数消费者才准备同一用途owner，不因whole-block UsesReady=false退回完整E物化，不新建第二图。

### 构建与最少检查

首次源码5caf的构建失败：`versions.cpp`缺`FixedValidationContractKey`的直接owner声明。Root加`verify/verify.h`形成8716，失败和精确修补保留。r2的main/IR/codegen三个CMake目标全部成功；随后的产物保留脚本误假定旧host16清单包含codegen而报错，已核build.ninja及链接日志、补拷codegen并修复元数据，**没有重跑构建，也没有把保留脚本错误当编译失败**。原脚本失败元数据独立保留。

当前main `7a92acec25741b9eab5414aa39c858db032342a4e223525af88b15e931990b29`，IR `645caf7931f7c12b49f6b66e35ea5141b59bc2bc3c1740261c20cfae1f4c9e96`，codegen `a84384d336e0fc84c3e9a4552a0c223434c859191926aac0cf8c4096182b62a2`；Android Release/O3/ThinLTO无PGO，配置/source前后相同。

只运行5个既有case共260断言：codegen4case/215，IR1case/45；两处原fixture明确覆盖native查询、last typed view、retry/reset，以及真实FromOptimized/TakeIR后的IR借用。另有既有cold/preserved与独立最终物理负例。原W59在Block0/Function1各一次PASS；4份owned设备文件核hash回收，namespace/owned进程关闭。没有逐片QoR/资源/lifetime/native/PMU/FEX矩阵。native fault宽度、partial/helper与late-recolor的完整独立运行覆盖尚未补齐，只具相应源审及既有合同，最终集中门仍开。

### 预定完整服务四点

before0230/mainb624，after8716/main7a92；同guest d92d8d77…、原CoreMark2000 argv/env、独立RA范围、O3/ThinLTO无PGO、CPU4与原服务端点。只开SVM_PROF=2，未开SVM_PROF2/SVM_DECODE_PROF，未加细clock、dump或PMU。恰好四点、无补采挑快值。

| 模式 | before完整ms | after完整ms | 单点变化 | before→after实际单位 |
| --- | ---: | ---: | ---: | --- |
| Block | 688.599071 | 684.747030 | −0.56% | 2415→2419 Block |
| Function | 779.469689 | 793.181970 | +1.76% | 429→430 Function；decoded blocks4560→4561 |

所有点退出0、CRC4983，实际argv/环境/ELF/前后身份核对，raw hashes重新离线核验通过；namespace核hash清理关闭、无owned active进程。8份只读快照均walt、min614400/max1171200/current614400，快照不证明持续锁频。IR/host计数也略变，不能视为逐单位匹配A/B、生成等价或统计收益/回退，更不能除以旧47ms算FEX倍率。

after的inputs+RA+Complete+emit仍385.732/418.427ms，RPO+publish+显式free+other仍109.213/132.023ms；codegen单点218.313→221.336及242.680→249.681ms不能分离因果。结论是**整段服务没有显示大幅总下降，继续同类lookup/资格微调不足以支撑最终目标**。按预定停止规则进入主方案算法改线，不以“缺最后消费者”推迟判断，也不把结构交付写成正式性能采用。

证据：[18文件整合及失败修正](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/guest-copy-lifecycle-integration/integrated-final.json)、[独立源审](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/guest-copy-lifecycle-independent-review-20261030/review.json)、[r2构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-guest-copy-lifecycle-r2/build.json)、[仅产物保留修复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-guest-copy-lifecycle-r2/preservation-repair.json)、[最少检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-guest-copy-contract-checks/execution.json)、[检查收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-guest-copy-contract-checks/closure.json)、[方向计划](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-guest-copy-service-direction/plan.json)、[执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-guest-copy-service-direction/execution.json)、[整数净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-guest-copy-service-direction/accounting.json)、[方向收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-guest-copy-service-direction/closure.json)。

## 第五十三批：依据完整服务结果重写方案

用户要求重写方案。本轮改写主方案执行部分及target/IR设计，更新最终文件职责表、旧入口与本记录；生产源码仍8716，没有追加构建、设备执行、QoR/PMU/FEX矩阵。

主方案将已完成的IR/用途/64B槽、state调度及guest-copy服务作为稳定基础；下一主线是正常interval/active、位置选择、最后physical窗口/owner准入与真实operand物化的整段算法。先用一次有界净账消除覆盖/新增工作未知，不建立新profiling框架；构造/residual和发布/真实结束同步承担完整预算。初始工程上限分别0.70F/0.90F/0.40F，F为同轮fresh固定FEX总耗时，合计≤2F；这些是设计目标，当前没有经证据证明可达的完整机制组合。

私有P2补丁53f3148a…及BV有限源审已收口，无剩余实质源码阻塞，但未整合/构建/运行。它仅涉及受限Known ARM64纯GPR完整flags writer/透明区间、部分marker和被覆盖metadata，不计已有lazy flags/state及Function旧coalescer的收益；新增callback、分类、移动和拒绝成本都保留。当前不能证明覆盖主要compile成本，定位次线原型，shared未采用。

文档清除“guest-copy仍私有”“0230尚未测”和未采用32B/operand/edit/view/CFG迁Store草图混入当前待办的问题。完整目录和每个源文件职责继续明确列出，文件整理随实际旧工作退出；最终6.3/6.4质量、资源、原分配范围与每模式≤2×fresh FEX门原文保持。改写前历史正文和失败证据原样保存，见[本轮收据目录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/plan-rewrite-algorithm-budget-20261030/)。

## 第五十四批：一次有界净工作核对及假设修正

根据第五十三批预定A，base8716/main7a92临时安装沿已有visit的aggregate，15个原文件加一个临时header；私有source0124/main6c74构建完成后，全部精确恢复8716。再以独立保留ELF运行原CoreMark2000 Block/Function各一次，同轮记录aggregate和既有simpleperf原生栈，未另起第二轮统计、普通性能、QoR/PMU质量或FEX矩阵。两点exit0/CRC4983、实际argv/env/CPU/ELF与前后身份闭合；owned文件核hash保存后回收，namespace关闭。

实际active expiry迭代40687/58692、最大active11/11；dirty snapshot59621/74436，约每attempt一次N，不能说每birth全N。零spill collect为2313/4363块、55640/66915节点；非零余域仍真实存在。prefix probes0/70、实际窗口迭代0/4078。physical observe59029/69107，publication/read窗口总迭代较小、每query owner最多4。否决active heap、全N物理事件索引、prefix cursor主线，保稳定选择/slot顺序。

native guest-copy真实fold983/1434块、38060/37090节点、cold0/0；P11199/12298、query9908/10534、policy probes22614/23707。resident用途枚举及recolor均0，不把它们冒称当前workload的主要新增费。BaseFacts与SSA各59621/70229是同一融合visit；后续SSA cache调用4836/5418、扫描0，不存在三轮重复SSA全扫。typed birth首轮LocalIdentity42808/67142、payload第二轮42808/66551，重复确实存在，但未分pending/noParams资格，不据此捏造后续image命中率。

raw/API逐样本PID/TID/IP/time/period/CPU及ELF映射核对。首unknown之前含exact真实Translate的strict-prefix为B731样本/777168895period、F838/893018461；record_stat lost/truncated0，仍有大量unknown与短链，不强分摊。最近recorded阶段B/F分别decode16.20/21.01%、Complete19.99/17.69%、emit14.94/13.62%、inputs11.82/11.63%、residual9.10/11.60%、RA8.62/8.60%、Facts8.89/7.75%、other10.44/8.09%。另一closest-source投影的IR访问26.35/30.17%不能叠加；最大单getter不足2%。私有observer约1.24/1.34%是该投影记录，不是已校准开销界；这些份额均不换算普通ms。

本次真正改变了选择，A已完成，不以新audit循环代替实施。完整schema有未挂hook项，零值不证明无工作；节点、operand、query、ID距离、sort元素是不同计数单位，不相加。Complete nearest-recorded家族与inline-expanded Declare边界也不相加。

证据：[净工作报告](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-8716-net-work-audit/report.md)、[完整收据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-8716-net-work-audit/receipt.json)、[全部计数](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-8716-net-work-audit/work-counts.json)、[native归约](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-8716-net-work-audit/native-offline/analysis.json)、[精确还原](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-8716-net-work-audit/production-after.json)。

## 第五十五批：构造GPR事件与零spill空服务整合

before8716，current`0b19c5bf…`，主ELF`a97be47c…`。三个完整小服务及一个既有fixture合计8文件，无新源/CMake项，无关源码/WIP保留：

| 文件（相对source/runtime/，测试另注） | 实际变化 |
| --- | --- |
| `ir/instr.h`、`ir/uses.h/.cpp` | managed pending无Params新生参数在原准入visit编码最终payload；共享旧序列化源，≤5source预检count后保精确reserve、一次copy/noexcept计数提交；cold激活及raw/manual/eager/Params保原路线 |
| `backend/arm64/compile/verify/fixed.cpp` | 已有FVI保留真实SetLocation/GetUniformAddress/UniformBarrier边界site，不扩大ResultBoundary/RA政策 |
| `backend/arm64/compile/registers/publication_windows.cpp`、`versions.cpp` | 同一FVI稀疏sites驱动反向dead和正向架构版本两相；保capture/early-clobber/write/publication顺序、外域root卡住的原语义、raw fallback，无第二N图 |
| `compile/allocate/linear_scan.cpp` | 真实完整scan的调用边界凭无candidate/spill及当前sealed SSA免空BuildSpillReloads；callee原实现保留 |
| `source/tests/spill_forwarding_test.cpp` | 只加强既有三shape循环的Ordinary：seal/SSA后预置MEM并直接调helper，确保未完整scan也仍建reload；无新case/循环 |

### 真实正确性修正与检查

旧私有zero-spill草案856c把guard放在BuildSpillReloads内部，有限peer起初误判无阻塞。Root发现test support可直接构造scan、预置MEM并调用该helper，sealed/SSA不能证明已执行映射；原结论明确撤销，改为aa02189e在真实完整scan调用处判断，不加scan-complete位。通用/test helper行为不变；fixture覆盖这一反例。typed-birth v1独立EncodeBirth分类链也已由同一serializer/准入visit替代，旧包仅留证。

最终补丁typed-birth46cfcd1f、GPR6f9f9e74、zero-spillaa02189e及fixture62ac7b99经有限源审后整合。typed birth新增栈字节写、引用聚合、copy及模板体积；GPR新位置合并查询，catalog/entry-width/extension/最终合同仍付费。未减少语义N/E、未退出residual首轮，不能称整个P2/C完成。

Android Release/O3/ThinLTO无PGO；main/IR/codegen/lifetime四目标一次成组构建通过。最少检查原驱动将spill用例归错codegen目标，Catch报告No test cases matched且驱动明确失败；原记录保留，不把exit0当匹配成功。只补建实际owner `swift_test`，只补跑这一例，之前成功检查不重复。最后7既有case共180断言（IR4/100，codegen2/65，swift_test1/15）、原construction模式516081checks/16注入失败及原W59 Block0/Function1 FLAGS1各一次均通过。五个实际owning目标同一source，私有诊断字符串未进入ELF。两检查namespace均核hash回收关闭，无owned activeprocess；这只是受影响正确性范围，不是最终QoR/资源/全lifetime或FEX资格。

### 唯一四点与停止同类扩展

before8716/main7a92→after0b19/maina97be47c，原guest/argv/env/独立分配范围及完整服务端点；仅SVM_PROF=2，未加细clock/dump/PMU。恰好四点，不补采挑快值：

| 模式 | before完整ms | after完整ms | 单点变化 | 实际计数 |
| --- | ---: | ---: | ---: | --- |
| Block | 687.634403 | 687.696311 | +0.009% | 两侧2413块、59494IR、224100host bytes相同 |
| Function | 788.552643 | 787.800094 | −0.095% | 429→430单位，4560→4561块，70229→70240IR，325788→325880host bytes |

所有点exit0/CRC4983，argv/env/ELF/前后身份和raw hash通过；namespace核hash回收关闭。8份只读快照walt/min614400/max1171200，current为614400或1171200；快照不证明持续同频。各侧一点及Function工作量微差限制统计解释，但**没有显示大幅改善，不能支撑两倍根治路线**。codegen点值略增、RA点值下降不能分离成因，也不据字节相同冒称运行质量不变。

本次after仍为构造+residual196.117/244.352ms、inputs+RA+Complete+emit382.393/410.006ms、发布/结束/其余109.186/133.443ms。停止继续扩展同类payloadimage/GPR稀疏getter/资格小包；后续必须指出更大整段中真实可退出的主要工作。主方案7.2将A标完成，清除被事实否决的heap/大窗口/重复SSA假设，6.1换入本source全成本；最终6.3/6.4门保持。

证据：[精确8文件整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/constructor-and-gpr-event-integration/integration.json)、[构造peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p2-typed-birth-independent-review-20261030/review.json)、[GPR及正确zero-spill peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-event-and-zero-independent-review-20261030/review.json)、[四目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-constructor-and-gpr-events/build.json)、[实际漏项owner构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-constructor-and-gpr-events-missing-owner/build.json)、[合并最少检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-constructor-and-gpr-contract-checks/combined-contract-summary.json)、[原失败执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-constructor-and-gpr-contract-checks/execution.json)、[单一漏项补跑](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-constructor-and-gpr-contract-checks/missing-owner-check/execution.json)、[合并设备关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-constructor-and-gpr-contract-checks/combined-closure.json)、[冻结四点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-constructor-and-gpr-service-direction/plan.json)、[原执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-constructor-and-gpr-service-direction/execution.json)、[整数账与限制](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-constructor-and-gpr-service-direction/accounting.json)、[方向namespace关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-constructor-and-gpr-service-direction/closure.json)。

## 第五十六批：据实际流量修正主线和文件职责

只更新主方案与target/IR/逐文件职责文档，生产source保持0b19，不追加构建、设备或测量。最终6.3/6.4及主方案历史1～4、8～9原文保持。A已完成，0b19四点及全成本替换旧‘当前8716’状态；原失败和各自版本均保留。

有限物理源码复核确认：默认width-chain gate OFF，不执行baseline→v2trial/rollback；真实BirthOnly提前退出early-hard，late也跳过，拒绝birth不复活旧晚搜。撤回普通路径普遍三次决定home及coalesce/gpr_writes全退役承诺。FEX原地物理化不能直接覆盖现sealed typed operands：TakeIR后真实view/flags/fault/transport消费者及写权未迁完，增加完整physical镜像亦不符合边界。

物理侧仅定义‘当前operand→ordered transport→真实handler’完整替换候选，须迁真实handlers并退出旧解析/物化链；目前未证明可删主要N遍历或字段，栈tuple/role处理可能抵消收益。构造侧仅定义‘一次真实已解析owned scalar片段→guest需求反扫→只lower活输出’候选；无当前命中率/净收益证据，不能退出整轮residual，parse/lower接缝、fault/PC/bytes/frontier/replay必须保，C34/raw-first不重开。两者均未立项，不据有限报告自动起原型或新四点，不充当已证两倍路线。

独立文档核对纠正FVI归属：当前verify/fixed.cpp只有ContractKey/Builder/Description，属于FVI准备/复用；目标归同一constraints owner。最终fixed/type-view在公共allocate/fixed_verify及ARM64 verify流程，不能把准备文件当最终check薄包装盲删。target文档同步本source构造+residual196.117/244.352ms；所有目录去留以真实职责和消费者闭合为条件，搬函数不认领性能收益。

证据：[有限物理复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/next-whole-physical-algorithm-review-20261030/report.md)、[有限构造复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/next-whole-construction-algorithm-review-20261030/report.md)、[文档更新与校验目录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/plan-reconcile-net-work-and-direction-20261030/)。所有必要检查/四点仍属第五十五批，没有新增性能或最终资格结论。

## 第五十七批：完整state-forwarding服务与输入职责收敛

before0b19/maina97be；初次成组source`0b283c7a…`，最后`f6e26de4…`仅修正同一IR fixture两断言，所有生产源码及主ELF字节不变。23文件、退役1源，无新source/header/目录/CMake库。Root唯一shared写者；P2完整span包v2及BV真实终止包经有限P6源审整合，无关源码/Git/WIP保留。

### 实际退出的完整服务

| 源文件/owner | 实际交付及保留合同 |
| --- | --- |
| `compile/analysis/guest_state.h/.cpp` | GuestStateForwarding唯一span/snapshot owner；删除U大小ByteFact初始化/clear/write/copy/meet，live gaps为未知；Read保首byte原Value，meet按def/sourcebyte保fallthrough原view；multi-range/helper/known-empty-vs-missing/Reset原语义保持 |
| `ir/args.h` | HasSameView只比较真实def和raw cast，原operator==仍只比较def。默认VOID视图随producer类型变化，不能按当时Type()等价压并 |
| `compile/builder/guest_state.cpp`、`ir_builder.h` | 全部合法logical append及mapped定制路径先准备容量再发表IR；undefined/invalid raw值回原准入；deferred key在唯一owner Write成功后调整，原fault/observer/AdvancePC顺序保持 |
| `compile/passes/state.cpp` | 全部读写/branch/范围与统计消费者迁同一span owner；mapped SetInst前按原seed/write资格PrepareWrite。无logical load/store的旧mapped-only省工保留；独立fault-aware DSE/PendingStore读取分类保持 |
| `backend/arm64/compile/registers/constraints.h/.cpp`及直接使用方 | 从verify逐字迁FVI ContractKey/Builder/Description声明和函数体，退役verify/fixed.cpp及CMake项。输入描述归输入owner，真实fixed/width最终check保原owner；只作职责归位，不认领性能收益 |
| `ir/block.cpp/.h` | DestroyInstrs最后before_bulk返回后，复用原Observer/binding条件结束Block自有用途图，免随后逐node Discard/Forget/E维护；活observer保旧，Function图及origin/import/最后Store lease不动 |

旧GuestStateByteFact、ReadGuestStateBytes、MeetGuestStateBytes和Bytes API全退役，未为旧test留生产dense算法。ClassifyGuestStateRead函数体逐字保持，仍供实际forwarding及state_sink PendingStore消费。主IR N/E、目标支持和Block/Function分配范围未减少，不把数据维护退出写成整个C/B/D完成。

### 新成本与实际源审修正

span live条目为S，但容量按实际已知byte数B加一次写宽度和几何增长准备，以保证后续split、Invalidate、multi-range clear及meet不再分配。每次增长请求受当时U限制，Initialize/Invalidate/Reset不shrink容量，保留历史峰值可高于后来的较小U。inline由256 ByteFact变16 Span；B接近U时容量可能更大。Read有二分/跨span读取，Write/clear有suffix移动，反向meet最坏有O(K×S)移动。这些都是实际新增成本，不声称只按S存内存或一律O(S)。

P2 v1 `9144687d…` 没有通过：Root发现public malformed Value在原准入前读取Type、peer发现residual mapped SetInst先于首次可能抛的reserve。v2 `239fff68…`分别先Defined/validtype/descriptor/bounds检查、按既有seed/write在改写前准备；invalid输入继续由原schema报错。v1/发现/修正收据保留，v2源码有限终审无剩余实质阻塞；新span增长的allocation失败尚未注入，不以不相关旧lifetime mode冒称覆盖。

BV `2176bd65…`仅终止真正破坏性bulk操作中的Block自有图；Function最后owner的domain注销已有实际职责，正常显式FreeIR发生时Function graph仍活着，故未扩大退休或添加整N资格扫描/标志。旧继续优化期间的提前用途图草稿仍延后。

### 构建与最少检查

首次四个CMake owning目标main/IR/codegen/lifetime全部通过，O3/ThinLTO无PGO，source前后相同。main `c56d67b76ee8d4770fcc3186d2a5c44f64c13ad5e7b6f42898c10d30e02c3ac8`；私有计数探针未进入ELF。

五个既有IR case首次4通过、1新fixture失败：LoadImm<U64>返回值本身未必携带显式rawcast，不能用它证明默认view不同。Root改为明确构造implicit与explicit U64 view，仅test两断言变化，生产源码完全相同。r2 Ninja恰好IR编译/链接两步，main/codegen/lifetime原SHA复用，未重新复制相同ELF；只重跑原state case，95断言通过，其余成功case不重跑。原失败总账1025断言含该case部分执行，与95重叠，**不累计成整组断言总数**。

5个distinct case覆盖实际forwarding HIR新用途、builder/residual/helper/path/partial策略、显式rawview和malformed受检失败、Block图Observer回退/无Observer终止/Params/stable handle，以及FVI重用和最终fixed负例。原W59 Block0/Function1 FLAGS1各一次PASS；两设备namespace核hash回收关闭，无owned activeprocess。不运行旧construction/state-sink-failure充作新PrepareWrite失败覆盖，没有生成质量/全lifetime/PMU/FEX矩阵。

### 一次完整服务方向点

必要检查收口后，Root预先冻结一组完整compile before/after，验证真实U维护删除能否覆盖B容量、查找和移动新增成本。before0b19/maina97be，afterf6/mainc56，原CoreMark2000 guest/argv/env、O3/ThinLTO无PGO、独立RA范围和服务端点。只有SVM_PROF=2粗桶；无新细clock/dump/PMU/FEX。恰好四点，无补采挑快值：

| 模式 | before完整ms | after完整ms | 单点变化 | 实际工作量差异 |
| --- | ---: | ---: | ---: | --- |
| Block | 700.955042 | 686.771951 | −2.02% | 2414→2419块；59550→59631IR；224292→224724host bytes |
| Function | 794.476103 | 775.006680 | −2.45% | 430→429单位；4561→4560块；70240→70229IR；325880→325788host bytes |

四点exit0/CRC4983，argv/env/CPU/ELF前后身份及raw hashes闭合，namespace核hash回收关闭。8份只读快照walt/min614400/max1171200/current614400，不证明持续锁频。数量微差和每侧一点限制因果/统计解释；不能将2%左右写成稳定收益或FEX新倍率，也不能把Block显式free下降直接归因一个子改动。

after互斥账：构造+residual198.660/234.291ms、inputs+RA+Complete+emit387.173/407.826ms、RPO+publish+显式free+other100.939/132.889ms。结论仍是**没有所需数量级改善**，停止同类span/容器/getter扩展作为主路线；继续针对更大生产算法闭合预算，最终不降生成质量、资源及每模式≤2×fresh固定FEX保持未完成。

证据：[完整整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/state-span-and-input-owner-integration/integration.json)、[FVI职责迁移](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/constraint-description-owner-consolidation-20261030/integration.json)、[span v2交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/guest-state-span-service-20261030/HANDOFF.md)、[span有限终审](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/guest-state-span-service-peer-20261030/review.json)、[Block真实终止交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/last-consumer-retirement-service-20261030/HANDOFF.md)、[其有限终审](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/last-consumer-retirement-peer-20261030/review.json)、[初次四目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-state-spans-and-input-owner/build.json)、[fixture修正及生产不变证明](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/state-span-and-input-owner-integration/fixture-explicit-view-correction/correction.json)、[仅IR两步增量构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-state-spans-and-input-owner-fixture-r2/build.json)、[最少检查合并收据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-state-span-and-input-contract-checks/combined-contract-summary.json)、[原失败保留](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-state-span-and-input-contract-checks/execution.json)、[只补原case](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-state-span-and-input-contract-checks/fixture-correction-check/execution.json)、[设备关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-state-span-and-input-contract-checks/combined-closure.json)、[四点预先协议](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-state-span-service-direction/plan.json)、[执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-state-span-service-direction/execution.json)、[整数账及限制](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-state-span-service-direction/accounting.json)、[四点设备关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-state-span-service-direction/closure.json)。


## 第五十八批：scratch唯一描述交付与真实Prepared缓冲复用

本批是重写方案阶段A/B与D的两个完整组件，**不是整个正常分配到发码主阶段完成**。Root单写者整合21文件，source `4d6f5645f458a49de13303861022ebadd3e42aea80a13a3f21ecbb482334e6fc`；完整补丁`7d96c012dffb5d421e95cc0e63c3f34db6f9fd4cd68857b1755d3f63013cf018`可在f6各文件before树独立apply-check。

- 正常Describe形成唯一ScratchSite/Profile，经RA/FPR retry和SealTargetEdits后把实际vector move给UnitActions。退役LowerScratchImpl全N末端描述、ScratchRange及PrepareScratchForEmission；没有第二预算表或N/E镜像。普通emission直读同一need；Function五类precise flags保独立block-local override，不能由整函数需求或当时相等数值替代。
- 稳定clobbers、uses/hoist/Mul/spill-kind前移到既有visit，后几项查询不认领为完全删除。晚DISCARD/Div128/flags-only保原物理判据；normal移交还检查原discard回调身份，公开换policy入同encoder的cold全value路径。Tick接瞬时optional值，原pending/backing、x10/x12/x13 lease和VIXL combined audit保持。
- Prepared最后owner结束先清Actions/Allocation，再结束CompileUnit/IR。仅同origin线程、完整FeatureSet+初始pools匹配时复用实际Map/live-end/直接写入列等backing，最多一个accounted retained≤128KiB idle；nested独占，跨线程/TLS退出直接删除，工厂失败不缓存。嵌套payload/hash节点真实析构，两scratch列的capacity均计费。move-fed列仍可能被新vector替换，不把所有保留capacity说成省分配。
- 新成本包括24B合并site、更细change points/更大的RA查询域、R行late过滤、Tick解码、弱引用/identity与清理计账。每请求N槽初始化、RA/Complete、证书及实际IR最后释放仍在；128KiB是容器计账估算，不是allocator/RSS数学上界。本批未测净性能，不预支收益。

私包P2 v1/v2和API中间版本保留未采用身份；最终只clear v3 `3bcd04f17…`（17文件），修正遗漏optional消费者、discard policy gate及普通budget单源。曾提出abort→retry需归还backing的疑点，经实际AbortLocationEditing会弃整个request的代码核对后撤回，未添加归还包装层。BV `6fc5ff5…`（6文件）经独立有限全字段/生命周期审，Root三方合并plan.h/.cpp并补两scratch容量列；最后实际seam审也通过。未开展新一轮全景诊断。

一次generic Release/O3/ThinLTO、无PGO、固定Data2 cache的合批构建完成，5目标/source/cache前后一致：

| 目标 | ELF SHA前缀 |
| --- | --- |
| svm_translator_linux | `67000c52…` |
| swift_test | `0673d0bf…` |
| swift_ir_test | `9ec16bf8…` |
| swift_codegen_test | `4adc09df…` |
| swift_hir_lifetime_test | `f016e953…` |

红魔bf1359ff的预定最少检查均通过：swift_test的6个既有case合计2319断言；原prepared-blocks/prepared-regions模式在IR_FAST=1各一次，通过实际backing复用、旧IR tombstone、不同输入发码、多Prepared/跨线程/secondary最后survivor检查。Root只在原模式增加live-destination move assignment接缝，没有新case/loop/失败注入矩阵；两模式现有checks分别123518/123520、injected_failures均0，不与Catch断言相加。原W59 guest/hash/argv保持，独立Block及Function各一次PASS。运行目录文件哈希核对后删除，closure=closed，无遗留本批进程。

没有逐片QoR、MaxRSS/资源、ordinary方向、native、PMU或fresh FEX批次，不能用上述检查证明最终质量/性能。整个A/B主域和三段总预算继续保持未完成，不为这两个组件新增独立四点；最近f6方向与8716 native都不移标签。

证据：[整合与范围](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/normal-allocation-emission-service-20261030/integration.json)、[合批构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-normal-allocation-emission-service/build.json)、[必要检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/normal-allocation-emission-contract-checks/execution.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/normal-allocation-emission-contract-checks/closure.json)、[P2终审](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/scratch-direct-handoff-peer-20261030/review.md)、[BV终审](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/prepared-result-storage-reuse-peer-20261030/review.md)、[实际合并接缝](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/normal-allocation-emission-service-20261030/scratch-storage-seam-peer.md)。


## 第五十九批：一次实际解析的scalar输出需求，原故障flags见证未通过

本批是C2完整减工算法，不混入私有literal复用、entry owner或新的getter/cache。Root整合11文件（2个新文件），source `f62528ad35ab882e47e750689549ed540f95fc34feed5f5959c6865826c21900`，合并补丁 `de080512234b80af4fb344797e68b6d06c6e41c14e3705bf2b01598a062d7018`。原Git/WIP保留，未提交/push。

### 实际交付与保留合同

- decoder.cc将实际parse与lower拆开，只parse一次。支持域缓存最多32条真实_DInst、原PC和owned bytes；逆向GPR字节/完整flags需求后，按原guest顺序和原GuestInstructionScope提交。raw/barrier/error/短fetch/stop/full-window均先drain；已解析非候选复用同一结果，不做长度预扫后再parse。
- 新scalar_demand.h/.cpp唯一family/roles供逆向需求和实际路由共用；decoder_alu消费完整/仅数据/仅flags/无纯输出，default完整路径保原语义。仅guest64、Known、early_build和公共scalar_output_demand同时成立时启用，不读取ARM64名称/home。初版0F负门使MOVZX/MOVSX不实际入窗。
- 真实纯输出未出生，其payload/use、后续opt/RA/编码处理才减少；幸存IR的首次深分析、CSE、DCE/flags反馈、原Block/Function RA及恢复仍保留。新增准入、栈复制、逆向transfer和路由成本未知。
- 窗口中已省输出的前缀不能在后缀失败后被复用。独立Block decode/finalize异常由request owner从Module移除未发布对象，Function私有构造在发布前unwind；不replay访存/helper。

有限family/pipeline/实际消费者审查收口后，一次generic Release/O3/ThinLTO无PGO、固定Data2缓存的合批构建完成，225 Ninja步骤、3 owning目标，source与配置前后一致：

| 目标 | ELF SHA前缀 |
| --- | --- |
| svm_translator_linux | `e666e65f…` |
| swift_test | `e9e377f6…` |
| swift_ir_test | `59aca832…` |

### 原正确性失败与有限诊断

红魔bf1359ff上新focused case `owned decoded scalar demand preserves observers and original guest PCs` 首先以独立Block运行。45条断言中43通过、2失败：访存故障后同core再进入guest执行LAHF/SETO，预期 `LAHF=0x96、OF=1`，实际 `0x16、0`。前面的GPR/store/原PC等断言通过，不能据此将整个case记PASS。

首个case失败即停止原检查清单，Function及其余尚未执行case/W59不冒称通过。另一次仅启用既有IR/postIR/host-byte dump复现同一失败，保留4份host dump；这是同一见证的诊断，不能把43条通过断言再累计一次。两次namespace均核hash删除，closure=closed、active_processes=[]、errors=[]；构建和检查进程均已结束。

源码复核发现普通memory fault出口可能直接返回host，而pending NZCV尚未合入canonical flags；新Run再从canonical word恢复。C2对fault前ADD仍请求完整数据/flags，相关fault源文件未被C2修改，但没有同fixture的4d6实跑，不能断言历史已证或C2完全无关。保留原见证，不在fault前加PUSHFQ改变观察条件。

plain-fault flags冷修补已完整私有冻结：5文件补丁 `195f9335025e4e8286cba3b5c0704ac3c9ef1c50c0f8b655eea4f77aa57703c0`，设计复用既有deferred stub/fault metadata、覆盖普通memory实际发码范围及raw发布入口，目标为正常热路径不新增Merge。它尚未整合/构建/运行；不能称恢复已修复。

### 方向协议与下一步

C2实施前已冻结一组完整服务方向：4d6/main670为before，Block before/after、Function before/after各一点，共四点，原CoreMark2000 guest/argv/env/RA范围。**当前尚未执行任何点。** 必须先修真实语义失败、通过原case与尚未执行的最少相关检查，再登记含必要修复的新after身份；组合净值不能独归C2。若只小幅下降，停止同类窗口/guard/opcode扩张。

本批不运行QoR/资源/PMU/fresh FEX矩阵，不宣称最终生成质量、compile改善或两倍目标通过。上一个最少相关检查通过的代码仍为4d6；最近方向686.772/775.007ms仍只属较早f6。C1有限候选已否决、D未找到新的整段重复，A/B物理整链与D预算仍需按重写主方案闭合。

证据：[整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-service-20261030/integration.json)、[冻结算法与方向边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-service-20261030/algorithm-plan.json)、[合批构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-owned-decoded-demand/build.json)、[原失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-contract-checks/execution.json)、[其设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-contract-checks/closure.json)、[一次既有dump诊断](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-fault-trace/execution.json)、[其收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-fault-trace/closure.json)、[冷修补私有交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-service-20261030/plain-fault-flags-fix/HANDOFF.md)、[C1裁决](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/birth-residual-closure-decision-20261030/decision.md)、[D裁决](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/setup-final-consumer-service-decision-20261030/report.md)。


## 第六十批：C2完整组合的恢复/消费者收口与四点裁决

本批贯通原C2和新发现的必要恢复合同，不混literal/entry/cache私包。相对4d6的完整组合20文件（2新文件），补丁`b5149eec3af4d70eb88edfac90f239fd9e65fc9179dd73547c1d37fb9831cecf`；before/after逐文件哈希与完整source manifest一致，私有before树可apply-check，原Git/WIP保留，未提交/push。

### 实际修复与原见证

1. 原f625实际host dump：ADDS后AF进入x26、PF结果进入x12，N/V直到故障LDAPR仍只在PSTATE。5文件冷恢复补丁`195f9335…`统一76个ordinary memory实际macro源码点，按当点mask/token/clear与精确范围记录，冷段先保两scratch/SP、PF、NZCV、clear，再原ReturnHost；raw/prepared/Function消费同metadata。普通访存热路径不新增Merge。0fa4/main2e58构建通过，原case Block/Function各45条通过；没有同fixture旧4d6实跑，不将历史归因补成已测。
2. 原foreign-loop case先在旧4d6/current0fa4相同地失败：FuncIRFree已清terminal，fixture仍要求LinkBlock。两侧释放前final IR实际LinkBlock相同，数量IR57/host416相同。仅修测试按实际寿命检查empty/Invalid或原LinkBlock，不改guest/arch期望。首次测试修正误用std::variant编译失败，r3改为已有boost::get，失败构建保留；生产代码不变。
3. 随后原strict metadata断言暴露真实遗漏：LowerBackedge只找SaveFlags，漏当前Sub[flags=All]。3文件补丁`38eb8292…`同visit接direct/legacy publication，分开final publication点/真实producer，保PF/AF、raw/canonical carry与原prefix/suffix证明。ordinary只让位必装owner的self-prefix；dead-successor继续普通恢复，self终端晚fallback仍保专用恢复。未改metadata>4或任何flags断言。
4. dfd181/main0764的原focused两模式各45、本地Block loop1522、foreign Function loop1098全部通过。剩余混合3case中carry/frontier两case通过，bounded因IRFree后boost::get已退休terminal抛异常。仅修bounded fixture：始终检查持久end=base+8193和下一入口未编译；IR释放检查empty/Invalid，保留时仍原ExternalLinkBlock精确target。最终source6f9仅此test变化；两步swift_test重建，main/IR硬链接复用原哈希，只补bounded及未跑项。

以上私包均有一次有限独立源码peer，原失败、程序、预期、错误构建和修正身份都保留，不将静态clear称运行通过。没有新增case/loop/PMU/质量矩阵。dead-successor与强制晚fallback未做新失败注入，最终manual/fallback/完整质量仍按原门集中检查。

### 构建与最少相关检查收口

生产dfd181一次7 Ninja步骤完成3 owning目标，Release/O3/ThinLTO无PGO、固定Data2 cache；最后test-only两步的当前产物如下：

| 目标 | 当前SHA前缀与来源 |
| --- | --- |
| svm_translator_linux | `076402d0…`，dfd181生产构建，6f9原样复用 |
| swift_test | `bce9cf41…`，6f9最后bounded fixture修正 |
| swift_ir_test | `201b384a…`，dfd181生产构建，6f9原样复用 |

最少检查按真实source/ELF合并：C2原case Block/Function各45；local Block loop1522；foreign Function loop1098；旧carry/frontier两case在混合批确实通过且未因test-only修正重跑；bounded修正后8断言；2 IR case25断言；原W59两模式各PASS。不将混合失败批的assert总数或同一见证重复执行累计成新通过总数。所有本批namespace逐hash回收，closed，无active/error。

### 一次完整服务方向与停止决定

原C2实施前已声明四点。在任何测量之前，因真实必要恢复/消费者遗漏，明确扩为整个组合净值；before4d6/main670，after6f9/main0764，原CoreMark2000/hash/argv、独立RA范围、完整端点。预备阶段旧收据没有checks字段，统一同一原始通过结果后才冻结plan；此前仅本地准备失败，未建测量namespace、未执行或重跑任何点。

| 模式 | before完整ms | after完整ms | 单点变化 | 原始数量变化 |
| --- | ---: | ---: | ---: | --- |
| Block | 664.765032 | 671.020379 | +0.94% | units2415→2415，最终IR59571→59571，host bytes224420→242872 |
| Function | 774.984569 | 780.393137 | +0.70% | units430→431，块4561→4564，最终IR70240→69481，host bytes325880→346248 |

恰好四点、exit0/CRC4983，source/ELF、argv/env、原始hash与namespace收口闭合。仅SVM_PROF=2基本粗桶；SVM_PROF2和其它细诊断环境未设置。八只读状态governor=walt、min614400/max1171200相同，current在614400/1171200间，不证明持续锁频。各侧一点和实际数量差异不支持统计回退、隔离C2因果或最终质量结论。

**没有显示大幅compile下降，按预先停止条件结束scalar window/guard/opcode/marker扩张，不补采挑快值。** 正确性与真实消费者补齐有明确作用，不能当作降时收益。最终IR数不是出生总数，不能据Block相同final IR推断没有省出生；同样不能把Function约1%final IR差异当作数量级优化。host字节增长含新冷恢复与选择变化，未逐热/冷归因，不冒称运行质量已通过。

当前三线after：构造/residual195.348/236.862ms，inputs/RA/物理完成/emit377.388/411.022ms，RPO/发布/结束/other98.283/132.510ms。A/B全部假想归零也还有293.632/369.371ms，故单独重写emitter不足以闭合历史95ms尺度。当前未解释D差额58.259/85.602ms不分派给未测owner，不重开通用profiling。

### 下一整体设计及完成边界

消费式emitter、Allocation和具体格式的有限正向设计已形成：在现owner中粗切换、同backing replayable stream、保NodeId32与实际多view/transport、typed资源单owner。当前48B Inst的24B storage含5B offsets，实际inline19B；退役旧解释后才可能用满24B，未据此预支收益。

实际span写权/所有逻辑borrow、未来flags/helper/Params/memory/atomic/V256回访、完整失败与重发仍未闭合，format表达不等于全部handler迁移。event-only/getter包装已否决为主线。下一算法必须同时解释C/A-B/D的大段减工和未知余额，不自动启动单段物理格式工程，更不以另一个完整影子图兜底。

整体目标仍active且未达成：未跑本批最终QoR、资源、PMU或fresh固定FEX；不将约671/780ms除以旧分母声称新倍数。最终不降生成质量、两种原RA范围、完整服务及各自≤2×fresh FEX保持不变。

证据：[完整组合与裁决](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-service-20261030/final-package/decision.json)、[冷修补整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-service-20261030/plain-fault-flags-fix/integration/integration.json)、[backedge整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-service-20261030/direct-flags-backedge-integration/integration.json)、[生产构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-owned-decoded-demand-backedge-closure/build.json)、[最终test-only构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-owned-decoded-demand-bounded-fixture/build.json)、[最少检查合并](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-backedge-contract-checks/combined-contract-summary.json)、[关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/owned-decoded-demand-backedge-contract-checks/combined-closure.json)、[四点预先plan](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-owned-decoded-demand-direction/plan.json)、[四点执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-owned-decoded-demand-direction/execution.json)、[四点互斥账/限制](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-owned-decoded-demand-direction/accounting.json)、[四点关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-owned-decoded-demand-direction/closure.json)、[emitter设计](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-redesign-20261030/emitter-design.md)、[分配设计](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-redesign-20261030/allocation-design.md)、[具体格式](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-redesign-20261030/format-design.md)。


## 第六十一批：消费式物理流的相依实现与必要检查

本批属于预先声明的一个完整物理消费算法，before仍为6f9/main0764。**尚未启用普通生产消费，旧Complete/Actions尚未退役，本算法性能点为零。** 以下是相依实现进度，不是新的性能阶段，也不把静态入口数或编译成功计作整链交付。

### 已进入代码的实际合同

- fresh执行型请求的真实消费权、整Store/parent组准入，以及CompileUnit关闭HIR用途/edit/anchor后移交真实typed cursors；组内多个Prepared共享parent不因此失权，公开Prepare、raw/import、保留IR及其它后端继续原语义路线。
- 同16B prefix＋48B backing的PhysicalInst、实际角色绑定和Prepared内唯一PhysicalStream；按原128KiB idle预算计容量，不另开缓存。Whole/Definition/Low32查询角色与实际写入位分开，不凭某个查询角色推断write。
- 193个opcode均有物理编码入口，标量/flags、向量、memory/atomic、call/helper、X87和control final writer接口已编译；call/X87/terminal/region等复用实际编码内核，不构造假的IR Value。它们尚未被普通driver调用，静态覆盖不代表动态执行通过。
- 原FunctionEntryContract改为语义阶段Capture、发码后原位BindCodeOffsets；真实owner穿过Emit(true)、Emit(false)、Flush和发布，持久entry/SMC依赖不回访退休IR。
- 物理前缀、typed尾部和最后Store租约分开真实结束。加强后的既有见证要求builder和持久Block仍存活时，结束PhysicalStream/CompileUnit即可退出源Store租约；不把semantic alive清空当作页面已释放，也不把允许的小页缓存写成零retained。

### 成组构建、原失败与最少检查

最近四个owning目标的Release/O3/ThinLTO、无PGO构建源为`7459badc4cbddefe03fdfddc9806b7a433c83db55d6f4313081dc8c218713ce7`，源码和配置前后一致。main `49d2d1c6…`、swift_test `370e2d54…`、swift_ir_test `7aebd98f…`、swift_hir_lifetime_test `0f9f18fd…`。

红魔bf1359ff只执行受影响的必要检查：既有`physical-consumption`单模式报告checks=164691、injected_failures=1、passed；这是同一个寿命/部分失败/重放见证中的检查次数，不是164691个独立语义用例。入口合同、canonical stateless links及真实x86长extent晚入口3个既有case共146条断言通过。此前fresh entry/concurrency两case112条断言亦通过，不因后续变化重复累计。设备两份私有ELF核hash、确认无活动进程后回收，closure=closed，无错误。

本批原失败分别保留：显式X/WRegister构造与flags宏名冲突；fixture误判Lambda11B＋ParamsStorage8B会超过19B；result-query角色与实际write混淆；新标量入口const Register与既有flags helper签名不符。相应修正的构建/见证身份分开，不将原失败覆写为通过。真实overflow改用超过19B的原typed字段，不以虚构call ABI造压测。

### 已合但尚未构建的下一相依部分

当前共享源码为`b0737b8b0428b752f804e7aad46f291244fecfda328949b7c6def8b0dd6b4005`：新合入实际resource backing、原logical occurrence/MEM count、动态pending scalar匹配、preservation，以及同owner Block/unit/Hot元数据。非MEM实际结果仍保存原MEM定义最后use count，不能因Covered或selected-GPR省掉原运输合同。当前scalar/vector参数拷贝修正logical index与physical slot分离，scratch沿实际current view传入。有限peer发现空ResourceBytes计账除零，Root已加零值guard；这些新变化尚未构建，不借7459的通过结论覆盖。

完整group final visitor、真实last Tick点的terminal/晚值绑定、普通Block/Function driver接线、旧正常Actions到Copy/Map链退役、共享选择规则收敛仍未完成。full density诊断在消费前沿保留路线；普通Hot/FlagsAudit不能读取退休IR或被静默关闭。不得以主要流量fallback作为整链交付。

下一步按这些相依关系整合、成组构建与最少受影响语义；完整算法真正闭合后才允许原先登记的Block/Function before/after共四点。未运行本算法QoR/PMU/fresh FEX，不将旧671/780ms转标，不宣称大幅降时或最终目标完成。共享树由Root单写、工人私有补丁，原Git/WIP保留，未提交/push。

证据：[算法预声明](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/algorithm-plan.json)、[动态施工状态](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/current-progress.json)、[四目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-physical-complete-consumers-r2/build.json)、[必要检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/minimum-complete-consumers-r2/receipt.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/minimum-complete-consumers-r2/closure.json)、[资源与元数据新整合](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/complete-consumer-resource-integration/integration.json)。


## 第六十二批：普通物理消费贯通与旧准备链退役边界

本批仍属同一个预先声明的完整算法，性能before保持6f9/main0764，方向点为零。普通Block与共享实际parent的Function现已调用同组final writer，覆盖提交后原Allocation与Actions真实结束，emitter只消费不可变物理流；同流用于Emit(true)、Emit(false)、Flush和元数据发布。full density/IR dump等保留语义诊断沿原保留路线，Hot/FlagsAudit使用自持实际元数据与RAShape，不静默关闭。

成组生产构建源`3e3e4136e4866f7cc437da6335a7731546c89f2d7382853bb76e60a57f787310`，四owning目标通过；main `6c050dc9…`、swift_test `84435134…`、swift_ir_test `eabdf640…`、lifetime `13a685c3…`。仅加强既有lifetime夹具后的源码为`d61912181c1d7134b55b9aca84dda49aed42c0dc7d67eacd162ef4b732c486b1`，其lifetime ELF为`3060e89a…`，其余三份ELF复用相同生产文件。

红魔bf1359ff最少受影响检查通过：标量observer Block/Function各45、Block前缀fault loop1522、foreign Function loop1098条断言；加强的既有physical-consumption模式checks=164721、injected_failures=1。后者明确断言原生Block和两个独立RA、共持实际parent的Function不能落回retained路径，旧MapCount=0/Actions.Ready=false、实际重新发码bytes相同，builder/Block仍活着时最终Store租约可退出。这些是有限接缝见证，不是完整opcode语义或最终质量门。设备私有ELF核hash后清除，确认无本线活动进程，closure=closed，无错误。

原失败仍保留：第一次生产接线在emitter预检误调用私有BindPhysicalEdges，Root移去不必要调用，没有扩大权限；加强夹具首次编译中Operand歧义改为ir::Operand。资源空宽度计账除零、lastTick跨块继承、actual terminal与CallReturn/PopRSB、原MEM count和动态pending选择均按真实合同修正，没有借旧IR解释physical字节。

**这不是完整性能候选交付。** 当前准备阶段仍执行旧Complete/Actions，并有整N捕获和临时四列recipe复制。下一相依批次由原Describe/实际matcher成功点直接写最终form、Block/unit元数据及scratch owner，保留必要RA重试/宽度/FPR/CFG/恢复合同，移去正常路径的旧proof通信和完整结果封装。Root在任何RA前统一判定整个实际编译组的direct准入；已准入后失败拒绝整个fresh请求，不退回一个未生成旧结果的假fallback。原公开/保留/组外borrow/其它后端仍有真实legacy路线。

构造侧有限深整数事实私包仍待整合。复核发现其内层依赖task尚未实际扣除所声称的8N+32预算，必须修正实际fuel或净账后才能合入；不把宣称的有界任务当作已经实现的合同。新增N行事实、实际P/B列、排序/失效/资源容量照计。没有新QoR/PMU/FEX或compile点，不把旧671/780ms转标或宣称≤2×。

证据：[完整源码checkpoint](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/execution-group-checkpoint/receipt.json)、[生产四目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-physical-execution-group-r2/build.json)、[加强见证构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-physical-native-witness-r2/build.json)、[必要语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/minimum-execution-group-r2/receipt.json)、[真实原生消费见证](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/minimum-native-witness-r2/receipt.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/minimum-native-witness-r2/closure.json)。


## 第六十三批：直接准备替换与实际 CoreMark 失败

本批仍属同一个预先声明的物理消费＋有限整数事实候选。Root合入P2 core/scalar/vector、BV flags/pseudo、P6 memory/resources，以及全组RA前准入／同源edge内核／最终协调。相对d619为36个源文件；完整6f9到a226 checkpoint为87文件、16新文件，均保原WIP、未提交/push。

### 实际退出工作与保留合同

normal在原Describe/真实matcher或验证成功处写唯一实际form、B/unit和scratch结果。End关闭真实位置编辑后清临时Actions；不再生成正常完整UnitCertificate/SealPayloads结果，不再整N Capture，也不保FinalUnit四列recipe副本。真实求解临时输入、mandatory FPR/GPR/width/fixed/scratch/CFG检查、RA retry、RCpc Track、transport/编码及发布/真实free继续执行。retained/public/raw/诊断等明确分支仍生成真实旧表示，不以一个假ready切回未生成的结果。

Root统一在整个实际ExecutionAccess组的任何RA前选择输出；准入后失权或失败拒绝整个fresh请求。有限peer发现提前Rejected误入要求frozen的Controller::Finish，已按实际frozen状态收口。raw uniform/polarity/BooleanBranch/pseudo放在必经的DescribePhysical，避免只在有consumer决策的块构造的ConsumerBuilder漏读。最后Block/unit元数据只move一次，最后Tick／terminal后缀／持久CallReturn、全组close→begin、typed尾部与physical前缀清理、不可变reemit和最后租约沿已验证内核。

C包原633ca6内层闭包与checked出口未实际扣除宣称预算，Root以20aafa修正：queue、root请求／投影、每次闭包任务及32访问checked预留真实共享8N+32；每次闭包最多32任务／32栈项，预算耗尽不发布脏父事实。公开32访问API保持。+48N事实初始化、heap/dirty/访问成本照计；memoized cutoff与旧root展开不保证机会或精度完全等价，不据此声明最终生成质量通过。

### 构建和最少检查

生产源`b89b7bebd8e035fbd805893ebd8eeb6d48962c9861655251a6e14118a8fefa52`四owning Release/O3/ThinLTO、无PGO通过，main `acf9c5fb10150addc20c20f1064538eb5c0487adaca0b9e32e464d81c97bcd3f`。test-only夹具修正后源码`a226ebba1312a133974eef1c23a8234975ddbc12d02eff215b089cb41b5175ed`，三个生产ELF硬链接复用，lifetime为`914f6f69…`。

首轮physical-consumption低层手写binder夹具错误地向fresh direct结果索取已退役Actions scratch。修正为明确的public retained准备，仍持实际fresh write guard；后续强制原生Block／共享parent Function进入direct且Map/Actions结束的见证原样保留，未恢复生产旧输出。修后该单模式checks=164721、injected_failures=1通过；整数反馈9、条件反馈14、scalar observer B/F各45、本地Block前缀fault1522、foreign Function前缀fault1098条断言通过。这是7个受影响检查，不是完整QoR或164721个独立case。3份私有ELF核hash后清除，运行场closed，无活动进程／错误。

### 方向失败，不以成功退出替代校验

按原plan只启动Block-before和Block-after两次；before固定guest CRC为0x4983，after退出0但CRC为0x325b，因此collector返回78并停止，Function-before/after未执行。完整方向组没有成立，原始输出、身份、只读状态和失败均保留，运行场核hash回收且closed。没有重采替换，没有新fresh FEX分母，也不把错误执行的compile数字当作收益／回退或最终目标证据。

围绕这个实际失败做两次无profiler的正确性隔离：同acf9候选仅verify_plan=1、RA前走真实retained路线，CRC4983；此前6c050dc9物理中间版（完整Actions＋Capture，无新direct/C）verify0也CRC4983。两者均退出0且运行场closed。由此优先查新direct选择/输出/协调，不把C公式或全部物理emitter先判坏。

### 已确认缺口与当前修复边界

P2找到BitExtract真实Width/Low32验证成功后的体省略没有写最终form。b4d717fd在原验证成功点直接写Covered，保留Tick和原证明，不恢复bitmap或新pass；四owning构建`4293a45c…`／main`2147a0a3…`通过。但必要无prof CoreMark Block仍CRC325b，Function因首失败未跑，所以该缺口成立，不能宣称已经解释CRC。

又找到旧Capture的numeric-first优先级被新末端same-GPR省略覆盖：数值BitExtract本应执行的8/16位截断可能被跳过。7b84f673已用一行条件恢复原if/else接受域，保留前一真实RA省略修补；当前尚待必要构建与从固定guest的crcu16提取的小回归确认。该leaf实际416B、末ret替换hlt，仅作失败缩小，不代表保留原调用者/RSB分组。后续只做真实失败相关正确性验证，原失败方向不补采覆盖。

这次错误进一步明确退役原则：证明bitmap可以结束，实际省略／截断／写入义务及原优先级必须交给最终form。候选尚无合格的新完整方向，最近完整成本账仍为6f9的约671/780ms；最终生成质量、资源和两种原RA各自≤2×同轮fresh固定FEX仍未完成。

证据：[整组依赖](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/direct-normal-integration/manifest.json)、[完整候选checkpoint](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/direct-candidate-checkpoint/receipt.json)、[四目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-direct-physical-consumption/build.json)、[夹具修正构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-direct-physical-consumption-fixture/build.json)、[7项必要检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/minimum-direct-physical-consumption-r2/receipt.json)、[检查收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/minimum-direct-physical-consumption-r2/closure.json)、[方向失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-direct-physical-consumption-direction/semantic-failure.json)、[方向原执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-direct-physical-consumption-direction/execution.json)、[同产物retained隔离](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/coremark-correctness-localization/retained.json)、[物理中间版隔离](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/coremark-correctness-localization/physical-intermediate/intermediate.json)、[别名省略修补](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/physical-width-omission-fix/root-integration.json)、[该修补必要检查仍失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/coremark-width-omission-validation/receipt.json)、[numeric优先级修补](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/physical-numeric-extract-priority-fix/root-integration.json)。


## 第六十四批：实际分支交付与低视图读取收口

第六十三批的失败方向保持原样，不改CRC oracle、不恢复旧正常结果链，也不补采覆盖。numeric优先级修补＋固定guest的416B crcu16小回归先在`fa64c708…`构建：叶子独立递推B/F各30条断言通过，但完整CoreMark Block仍CRC325b；因此不能以叶子通过或前两次修补宣称根因已经解决。

### 从真实 host 差分定位交付错误

同一fa64/main74a960e5只使用现有SVM_VIXL_HOST_DUMP，retained verify1为CRC4983、direct verify0为325b，无profiler/PMU或计时解释。2066个共同guest PC中2017个host body完全相同，49个不同；真实CLI的crcu16/crcu32/crc16及main入口都相同，避免继续猜叶子公式或整个memory模块。原bin与仅.data反汇编保留，初版.strtab噪声已从展示移去，原bytes未改。

真实差分确认两类漏交付。其一，main21a9bc的cmp #1，以及21c36c/21c79d的cmp #0x2c/#0x78，direct使用了仍含地址／旧bool的临时寄存器：StoreIntegerBranch仍只写旧actions.integer_branches，关联常量／predicate却已被实际Disable，End清旧表后丢掉选择。Root在原Store增加显式actual Block参数，同一函数先写唯一Block metadata再停发关联节点；normal不保旧最终row，retained和已有生命周期合同保留。

其二，219a8d新增UXTH写回x29；x29是guest RDX，读取低16视图不能截断这个架构容器。原BitExtract body省略含GPRLowView，direct剥离时未交付；最终writer恢复对当前frozen Allocation实际low-view的一次常数查询并编码Covered。它不读已转换producer，不新增proof、N表或遍历，JIT仍不借Map。FlagsInput和MemoryTransfer Value的对应省略已有真实成功写者，未为假设重造表。此前Width/Low32真实省略及numeric-first修补继续保留。

### 当前产物与必要检查

源码`1ce177f14a4ba6a042932ed4f30fd347ddd717ed9a9e84e5dea2ca60b46cf6c9`，owning CLI/test generic Release/O3/ThinLTO、无PGO构建通过；main `2b909bc1983048053404ed5a66f6f6be9e5ddb3996ec298f6693777b2bd026fb`，swift_test `a648cf0f47947e17f22bf3635c05bb4b760bb80be649a10db0171697ea9ebd07`。这次不重链／重跑未受接缝变化影响的IR/lifetime目标，前期四目标和原生最后租约见证保留各自身份。

红魔bf1359ff本次必要正确性批次通过：原整数分支生命周期21条、真实416B CRC16 leaf B/F各30条，以及原完整CoreMark B/F均CRC4983。因恢复integer_branch实际交付会让它重新优先于backedge选择，又补验两项直接相关的既有故障恢复：本地Block prefix fault1522、foreign Function prefix fault1098条通过。没有扩大其它配置矩阵；所有新运行均无profiler/PMU、无性能取点或运行提速解释。两处私有运行场均核hash清除，无活动进程和清理错误。

完整6f9到当前checkpoint为90文件、16新文件，逐文件baseline hash及私有patch apply-check通过。正常最终输出退役和实际选择正确性已有这批有限证据；保留路径／迁移handler重复及目标布局收敛、最终完整生成质量／资源门和两种原RA各自≤2×fresh固定FEX仍未完成。最近合格的完整方向账仍为6f9约671/780ms，不将本批正确性通过包装成compile改善。

证据：[实际host差分](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/coremark-host-differential/receipt.json)、[共同PC/49变化](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/coremark-host-differential/host-index.json)、[分支真实写者修复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/integer-branch-output-handoff-fix/receipt.json)、[只读别名修复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/physical-low-view-alias-fix/root-integration.json)、[当前构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-branch-and-lowview-closure/build.json)、[正确性收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/minimum-branch-lowview-closure/receipt.json)、[受影响fault收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/branch-lowview-fault-closure/receipt.json)、[完整源码checkpoint](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/physical-correctness-closed-checkpoint/receipt.json)。

## 第六十五批：职责布局与重复编码收敛

本批按现行方案完成维护收敛，不将文件减少记作JIT compile收益，也没有重新提交第六十三批失败方向。源码从1ce177f1变为`add999d33bcb56ab5718caea8b300ef79034048ab6c082e57e7288953faff62a`。

全仓调用核对证明两个CapturePhysicalMemoryRecipe入口只有相互委托，零外部调用者；对应PhysicalMemorySelections/Options、专用地址／候选捕获helper及recipe旧三ID存储一起退出。实际RecordPhysical*、FinalizePhysicalMemoryInputs、WritePhysicalMemory及动态pending-scalar资源合同保留。整数shift的retained与physical入口各保原专有选择，只绑定实际寄存器／立即数，再调用同一个机器码内核；source/result/count顺序、窄视图、mask、alias修正及scratch租用次序保持，不造伪Inst/Value桥。

ARM64 compile原41cpp/17h归并为23cpp/10h：目标调度归compiler，约束／scratch归constraints，架构版本／宽度／publication归state，语义选择与最终写者按同一family合并，独立检查归verify。最终form/schema移入physical.h，8个physical emitter不再包含select.h。两个internal头保留真实私有实现声明，没有新增owner、pass或中间表；旧registers/select/verify/detail源引用和CMake条目已退出。plan*真实过渡接口和其余非shift重复编码仍存在，不能宣称最终代码组织全部完成。

Generic Release/O3/ThinLTO、无PGO四owning目标通过：main `f7a5689622178e559df91f5f0ac3269468139350f35b32a99d20436198ac1c27`、swift_test `23fd4581a3b2e809f7fad9d127bc8abbbd8eed974d50163ac651c95baa5e8667`；IR/lifetime仅完成本批构建，未冒称重跑其旧见证。红魔bf1359ff必要检查仅运行既有shift独立参考（55296程序／2605056校验，含原内部优化开关与B/F）及真实x86立即移位边界／flags外部Block、Function各一次，均通过。没有新测试、完整CoreMark重复、fault/QoR/PMU/FEX矩阵或计时点。设备私有运行场核hash清除，无活动进程。完整CoreMark CRC4983和fault恢复的最近证据仍归1ce版本。

152个实际源文件差异（18新、43删）已保留精确before/after及patch，私有apply-check通过；源码diff大部分为同职责归并和include迁移，不将规模认作性能。最近有效完整计时仍6f9的约671/780ms；当前维护代码没有合格新结果，最终质量／资源／两模式≤2×fresh FEX仍开放。

限定的后续算法核对否决“把state/width/physical checker一并视为重复”假设。仅Function按需入口宽度fixed-point可设计为一次编译块transfer、变更队列求CFG；Block无收益，触发率和净时间未知，本批不整合、不作为两模式大幅主线或新方向点。

证据：[维护源码交付](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/layout-and-encoding-consolidation-20261030/manifest.json)、[成组构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-layout-and-shift-consolidation/build.json)、[必要shift检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/minimum-layout-and-shift-consolidation/receipt.json)、[有限算法裁决](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/arm64-width-owner-service-decision-20261030/report.md)。

## 第六十六批：独立 Block 构造事务与 Function 宽度求解

本批替换两项完整真实服务，保留原两种RA、所有质量/观察/资源义务，不把41→23文件归并当性能机制。生产source `c9043aa1ee2816c087ff64b714a2f6cb70e235f0ead74b2ab755ee22468d9138`；最后source `123d3d0983feea8eeafa7b8ba20d0d21440dac8697e5a31e010a6dcf777eb0ee`仅fixture不同。main `33c04a195e7c97f51f32cafe2904b03d6dce43453e4fa5765f2d0043a079f4b9`与另两生产ELF核hash复用，IR测试单独重链为368e9ec0…。7文件、1新测试源的before/after和patch已完整保留，私有apply-check通过，未提交/push。

### 实际退出、新增和保留

Function入口宽度的旧fixed-point每round重走全IR和unary/alias链。现在首次按旧publication点规则编译每B的30home transfer，零宽unknown以256参与clamp，sign只保留/截断/清除/常量；CFG仅在incoming变化时求摘要，exit变化才唤醒successors，首访全部B、ring队列≤B。旧meet、function/external/call-return/无incoming/Missing语义、helper与partial写规则完全保留。没有Inst/Value留在摘要，摘要/queue在原调用内真实释放；仍付首轮N+D、B×30初始化（每home≤16B）、约5B/B队列/状态及实际边传播成本。独立物理检查和局部架构版本fold没有删除。

独立managed Block的适用no-Params pack原先AppendKinds之后又WriteArgument/WriteData、重查local identity、逐occurrence Use。它现在复用Function已有BirthPayload；仅两个真实CreateInstruction→unique_ptr draft分支能借actual IRStore调用私有入口。首遍source检查/编码、完整distinct multiplicity预检、exact reserve后立即copy+计数提交。失败草稿按原真实NodeId退休，不回绕identity；真实active graph继续PrepareAppendOwned/ScanInputs/Reserve/Commit。public fresh、raw/HIRBlock/eager、Params、空pack、slow支不因此获得新权限。增加有界栈image/duplicate聚合与copy，纯Imm可新增复制；当前Inst::Use本就没有第二CheckMutable，未虚报该收益。

### 必要验证与原有失败

初次四owning generic Release/O3/ThinLTO、无PGO构建通过。新Function宽度case以原逐round具体IR解释为独立参考，192种CFG/entry/write组合；与原类型构造case通过（首组3case中两过，唯一失败是新增用途fixture）。peer发现测试home24并非fixed，已改29并断言真实function/external/call-return/Missing覆盖，不把跳过分支算验证。

新增用途图fixture最初在固定roster ValueWorklist中append/erase未登记节点，触row!=nil；修正epoch后，原mutable case又触原Pending断言。一次冻结add999/main对应IR ELF的实际基线核证实原case也在同断言7/6/1失败。最终将新graph见证拆成独立Block/workspace/rewrite，原mutable case从add999逐字恢复，不改Pending/Drain、不skip、不添expected-failure。当前独立graph case通过；原case仍同一7/6/1，明确是baseline-equivalent failure而非PASS。原两次失败、基线核、源case hash及所有ELF身份保留，没有将2个较早通过case转标为新IR ELF重跑。

随后只续跑未完成检查：construction fast0/1各557246检查/17注入通过，原完整CoreMark B/F各CRC4983通过。未扩大其它fault/运行QoR/PMU/FEX矩阵；各检查现场核hash删除、关闭，无活动进程。当前新检查的有限通过不代替最终质量门，原有队列断言仍未解决。

### 一组预先冻结的完整 compile 方向

新组add999/mainf7a56896 → 当前main33c04，两侧均已有正确direct物理路径，只测上述新构造/CFG服务。原6f9→a226物理组仍失败，未补齐、替换或重新归因。输入/ELF/命令/env/CPU4/端点及最多四点在执行前冻结；collector只改私有namespace，真实Translate端点（含Prepared/physical/IR/Store/builder同步结束）源码逐hash不变。只用原coarse SVM_PROF=2，未启fine/PMU/native/FEX；每点先留raw再校验，恰好四点exit0/CRC4983，身份/粗账/状态闭合，所有owned文件核hash回收。

| 原RA模式 | before ms | after ms | 单点变化 |
| --- | ---: | ---: | ---: |
| Block | 810.172755 | 780.850253 | −3.62% |
| Function | 888.303132 | 890.689113 | +0.27% |

当前after三线C206.890717/251.001304、A/B466.567962/505.439790、D107.391574/134.248019ms。显式IR-free仅0.726550/0.666035ms，真实末尾服务部分已合入final visit，不能说free消失。Complete/物理形成/emit仍合在291.520887/325.941402ms；没有足够当前证据将其全部归因编码、检查或Packet，不用旧8716 native份额改标签解释它。

Block units2416→2419、final IR59693→59626、host bytes243204→243228；Function units438→437（另18Block）、decoded blocks4599→4598、IR69909→69898、host bytes347300→347208。CPU4 max1171200一致，current只读snapshot有614400等变化；不称连续等频或代码质量等价。只有每侧每模式一次，不能独归两个服务、证明统计增减或fresh FEX倍数。

**裁决：没有大幅compile改善，停止这两项服务的同类扩张。** 新增逻辑已保持其合同并完成有限验证，但未取得性能正式采用、最终QoR/资源或≤2×资格。下一项大算法尚未选定；只对当前真实A/B的Complete、物理形成、实际emit作一次限定归因，再决定完整服务替代，不续接格式/getter/更多窗口小包。

另两项有限反证保留：普通ADD/SUB/CMP及native carry已紧凑，不普遍先展开flags整树再重聚合；原logical Tick输入、实际encoding roles/probe和MEM preservation不同，不能用现有binder事件直接替换transport扫描。未据此制造缓存、普通域小包或新的性能点。

证据：[算法及新增成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/width-transfer-and-block-birth-20261030/algorithm-plan.json)、[源码checkpoint](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/width-transfer-and-block-birth-20261030/manifest.json)、[必要检查汇总](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/width-transfer-and-block-birth-20261030/contract-summary.json)、[实际r3检查及基线失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/width-transfer-and-block-birth-20261030/minimum-checks-r3/receipt.json)、[四点原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-block-birth-and-function-width-direction/accounting.json)、[四点关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-block-birth-and-function-width-direction/closure.json)、[flags有限反证](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/flags-construction-service-decision-20261030/report.md)、[logical/physical输入反证](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/physical-consumption-implementation-20261030/final-writer-transport-fusion-decision-20261030/decision.md)。

## 第六十七批：正常私有编译路径收敛 fixed 核验责任（P1首段）

P0在原123d/main33c04上恰好执行Block/Function各一次native，并一次离线归约，strict qualified compile样本820/921。Complete约17.36%/16.88%，物理形成12.01%/11.53%，实际发码7.65%/9.27%；lost/truncated0但unknown/短栈缺口保留，不换算普通ms。运行场核hash关闭，P0路线停止。已共享的final N观察和FixedWriter前缀不重复认领；新增完整fold需额外ordinary-owner/typed transfer/晚写反馈，有限反例不支持净少工作，故未增加broker。

真正退出的是正常私有direct请求在RA-attempt的独立fixed核验：原全N writer分类、owned_writers构造/sort和fixed区间核验由原Complete必经的最终核验承担，后者保留全部最终位置和actual target writes合同。AllocationWriter默认Attempt责任；公开PlanBuilder即使传PhysicalSelection指针仍checked，只有friend Prepared内部两处私有tag构造可移交。原scratch/retry、quality算法、manual/retained和独立audit不变；native SSA早拒绝不写Ready。无新N/E表、结果owner、逐节点证明缓存或timer移动。

源码02ae6f8cfa88c43f01f0350ff7dc5bec6bd2b6e865e477e8297c61d35b8fdd3c；6文件，5生产＋1既有IR用例。CLI126011080B/9c03d806…、swift_test264030216B/0123cef1…、swift_ir_test140483208B/2607b2ce…均在本源码实际构建，Release/O3/ThinLTO无PGO，源未漂移。

必要语义共7命令：5用例542断言（fixed/preserve124、width endpoints357、finalize retry9、FPR retry5、public authority/failure kinds47），完整CoreMark B/F CRC4983。首个用例exit0/124assert已过，但原capture未赶上快速退出导致/proc采集失败；原execution保留失败且已closed。未重跑首条，只用精确父端argv/env、ELF/guest hash和实际退出/断言的语义envelope执行其余6条，全部过并closed；没有任何断言或生产源码补丁来迎合测试。原mutable历史失败本批未重跑，不改其状态。

[源码/净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p1-fixed-validation-handoff-20261030/algorithm-plan.json)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-p1-fixed-validation-handoff/build.json)、[必要语义汇总](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p1-fixed-validation-handoff-20261030/contract-summary.json)、[首组采集失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p1-fixed-validation-handoff-20261030/checks/execution.json)、[仅剩余6条](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p1-fixed-validation-handoff-20261030/checks-remaining6/execution.json)、[有限peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-final-fixed-verification-20261030/peer-review.md)。本批不等于P1全成；0性能点、0新QoR/PMU/FEX矩阵、最终门仍开，未提交/push。


## 第六十八批早期在途记录：Block definition stream（历史，收口见第六十九批）

本节从既有工作树和构建/peer收据补录候选状态，文档重写本轮没有改源码。相对已验证基线02ae，现有候选按定义顺序产生普通interval、合并稀疏preserved事件、共用原AllocateInterval，以active槽替代完整历史存储，并为冷局部repair从原index/实际Map重构历史。符合条件的独立Block退出完整interval materialize、preserved全表调整/排序及重复压力遍历；完整fixed/REF/discard/FPR预访、压力/active、spill/retry仍执行。Function及单块Function保原scope/调度，Function模式实际Block fallback独立记。等(start,end)以id决定次序为声明的B类差异，不能声称exact分配或最终QoR。

第一次构建source59ec因原GetClearCount非const API调用失败；修正局部const后，source `be1d676cbe860395a60040675ed3bd54a5df6c6c708898a042f71c2e085c0f63` 的R2 owning构建完成、源码前后不变，main SHA `b43b9a4637627e81df344ca611a76a06dfa3882c0b6a312d0901742cd783d112`。原失败与R2收据分别保留，构建成功没有授予语义/性能资格。

审查确认首次准备前公开调用者可预置raw Map，而现stream会跳过已seed普通定义，改变legacy行为。最小gate（SHA `2225afec77eabffe9e4b3dabbea78cf929dd0f8f86257ff0ea5d0374839f1682`）和原U64无pressure形状追加MEM见证（SHA `1b543c2f9f694df6ed10d4b0211b127cf4de83645ca7037121b8553d12496948`）仅冻结于私有补丁，尚未合入。应在既有clobber预访、任何Map副作用前取消stream，保seed的legacy路径，不增加全N扫描/资格表；同源SSA有效不能证明物理Map为空。

**当前未运行新stream的必要语义或完整计时。** 已准备方向包before为02ae，after仍未绑定、0点；先修补、统一构建，最少三个受影响既有cases及完整CoreMark B/F闭合后，才可由Root执行原四点。完整算法中性且无大项余量即停止同类扩张，不接力heap/getter/packing。02ae已有语义和123d已有时间均不转标给候选，最终目标保持未完成。

材料：[R2 build](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-block-stream-allocation-r2/build.json)、[生产候选](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/block-stream-allocation-20261030/algorithm-plan.json)、[peer](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/block-stream-allocation-20261030/peer-review.md)、[gate](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/block-definition-stream-preseed-gate-20261030/HANDOFF.md)、[witness](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/block-definition-stream-preseed-check-20261030/HANDOFF.md)、[未执行方向计划](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-block-stream-allocation-direction-20261030/plan.json)。


## 第六十九批：Block定义流完整反馈、停止与恢复基线

### 实际修补、构建和必要语义

当前原候选be1d的preseed Map反例由精确gate `2225afec…`＋最小MEM见证 `1b543c2f…`收口。Root逐文件核before/after，git apply check/whitespace通过后合入两文件；没有增加全域扫描/资格位。source `2317b009402af563b7a291d13b0dd3952066016b443472c046a84eee39bc54db`的R3构建svm_translator_linux、swift_test、swift_ir_test三个owning目标，Release/O3/NDEBUG/ThinLTO/noPGO，source前后不变。main `3a7ef7cd0b014e376ab42b5baea02152e680ad9906bde66b85bd9b96917a90a3`、test `7e554ebc…`、IR `d7119c73…`。

Root审核执行最少direct-parent runner：原preservation case174断言、local-repair47、实际FPR retry14，共3cases235断言；完整CoreMark Block/Function均exit0/CRC4983。前后ELF/guest SHA、argv/env、唯一case输出闭合；不做STOP/proc竞速，不启PROF/PROF2/PMU。自己的`svm-block-stream-r3-20261030`仅在无自有进程、精确文件集合/hash匹配时删除，closure closed。原mutable Pending及旧采集失败不重跑或改PASS。

### 唯一四点与净账

after只在上述同source的必要语义closed后绑定R3；before为原02ae/main9c03。Root只读状态符合准备合同（cpu4 max1171200，未写频率），review原runner/collector/validator/account后，只运行冻结的Block-before/after、Function-before/after四点。四点CRC4983、exit0、exact env/argv/ELF、CPU4 target/CPU0 collector、整数账与source delta闭合，所有raw拉回逐文件核hash后删除自有方向namespace，absence验证通过。没有追加、替换或挑快点。

| 模式 | before02ae ms | candidate2317 ms | 单点变化 | inputs＋RA变化 ms |
| --- | ---: | ---: | ---: | ---: |
| Block | 779.200031 | 777.893051 | −0.17% | −0.212326 |
| Function | 888.574683 | 881.191150 | −0.83% | −2.820435 |

七互斥桶＋other逐点精确闭合，wait已含，不能再二减。Block units2415→2415、IR59653→59571、host242944→242872；Function438→437根、另18个Block、IR69909→69898、host347300→347208。工作分母不exact，不将小变化归于sort/slots/某组件或Block fallback。只读快照也不证明连续同频/同温/同负载。

候选确实退出符合条件Block的完整历史interval、preserved全表调整/排序、重复pressure遍历；仍做真正fixed/REF/FPR预访、pressure/active/assignment、spill/reload/reserve/retry和最终合同，新增稀疏事件merge、slot维护和冷历史重构。有限静态核对确认普通private路径准入可达，未发现恒真legacy门；实际命中比例未知。反馈未显示足以支撑新增复杂度的大幅降时，不声称统计等价、最终代码/运行质量、资源、freshFEX或≤2×。

### 撤回与当前状态

按主方案停止同算法家族，包含Function stream和heap/getter/packing接力。Root逐一确认当前15file仍等于2317 checkpoint的after SHA，再写回原归档before字节；完整source manifest恢复`02ae6f8cfa88c43f01f0350ff7dc5bec6bd2b6e865e477e8297c61d35b8fdd3c`。未git reset/checkout/stage/commit/push，不覆盖其它WIP。候选完整源码/三产物/必要语义/raw方向和原失败都保留。回到原byte-exact源码直接复用02ae bank及其既有必要语义，不额外build或重复点。

当前02ae同身份before账：C209.271/256.249、A/B461.939/498.586、D107.990/133.740ms；即使A/B理想化全零仍317.261/389.989ms。固定责任后移、物理消费、IntegerFacts等保留基线，但Function/Complete、构造和固定费用的达标级算法仍未选定/验证；最终质量/资源与两范围freshFEX≤2×未完成。不能将停止一个候选写成目标完成，也不能转做文件搬迁代替主要性能问题。

材料：[R3 build](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-block-stream-allocation-r3/build.json)、[完整候选15file](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/block-stream-allocation-20261030/checkpoint-2317/manifest.json)、[必要语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/block-stream-allocation-20261030/contract-summary.json)、[四点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-block-stream-allocation-direction-20261030/accounting.json)、[原raw/执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-block-stream-allocation-direction-20261030/execution.json)、[闭场](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-block-stream-allocation-direction-20261030/closure.json)、[撤回](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/block-stream-allocation-20261030/withdrawal/receipt.json)、[整包净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/block-stream-allocation-20261030/closed-direction/net-work-and-decision.json)。


## 第七十批：Function跨块求解完整试验与IRStore目录容量缺陷修复

### 实际算法与必要构建

针对`PrepareCFGUseEnds`每cross-value独立遍历同CFG及反复取tail/写endpoint的真实服务，试验每≤64个实际value的seen/pending/live_out/kill传播和按真实tail Id降序每值一次max归约；不建立B×N图、不改两RA范围、原seeds/cache/definition kill/implicit/return语义。最坏碎片传播不保证64倍，新增32B×B mask、tail排序/批次初始化和bit归约如实记账。private生产包57ac9f0d…＋原case强化包e126ed44…，Root核hash合入两文件。

首fc52758b构建在test见证误调用private `Block::EditInst`失败；改为已有`RewriteContext::Change`受控修改，保原oracle/原外层16迭代、65个真实root source/133Params和逆RPO块ID范围、NodeId稳定。12b5c617三owning构建通过，原raw fixed-point case61断言和lazy/cache12断言过，但原最大65535case抛`std::bad_alloc`，检查组当即停止，后续logical-return/CoreMark未执行。

### 原基线同故障与Store正确性根因

仅用原02ae的IRtest ELF `2607b2ce…`、同clean env重现原最大case，仍`bad_alloc`。case正文sha `a23377688863f2e030935700f5e521e6ec8d018cd2507e59243f8a4a2265317e`逐字一致。原两个失败组均closed并保留，未skip、缩规模或弱化断言。

Root与独立源查定位`IRStore::Allocate`：每次新page都执行`reserve(max(size+1,capacity*2))`，即使目录尚有空间也翻倍。每页256nodes，65535节点需约256页，旧容量随页数至少2^(P−1)增长，而非正常O(P)；第30页的地址目录请求理论上已约8GiB，实际异常点未伪称已取栈。Word kernel此case仅2blocks/64B State，不是新65k状态表。

Root仅为page/address两个目录添加size==capacity守卫；Page仍先由局部unique_ptr持有，两reserve都完成后才insert/push/发表，失败不增next_node或逻辑size，lower_bound在reserve后取得。真实page/槽/身份/payload/owner/phase、最后lease释放/原缓存边界均不变。没有延期释放或新缓存。采样前将这项必要修复加入组合、留原方案快照，声明可能影响跨页pointer-order发表cohort的B类范围，不做采后豁免。

27311c76/main10289a6组合三个owning构建通过；新source六条必要命令全部通过：raw65/逆RPO61、cache12、原65535边界6、实际call-return19，共四cases98断言；完整CoreMarkB/F CRC4983。独立参考函数/原最大case正文/原ASSERT保持，所有自有语义目录核hash关闭。

### 唯一完整四点与停止

同配方before02ae/main9c03、after273/main10289a6，只原四点，无PMU/QoR/FEX或替换快值。source delta恰word cpp＋Store cpp＋原fixture，timer/default不变；4点exit0/CRC4983、exactargv/env/CPU4目标/CPU0collector/只读state均过，七互斥桶＋other逐整数ns闭合，wait未二减。raw拉回逐文件核hash后删除自有运行场，absence通过。

| 模式 | before02ae ms | composite273 ms | 单点变化 |
| --- | ---: | ---: | ---: |
| Block | 761.944706 | 768.737507 | +0.89% |
| Function | 872.672450 | 884.470468 | +1.35% |

Block请求2419→2415、IR59626→59653、host243228→242944；Function根437→438（各另18个Block）、IR69898→69909、host347208→347300。分母不exact，快照不代表连续同频/温度/负载。组合未显示大幅收益，不能孤立判Word或Store快慢，也不支持统计回退/等价、QoR/资源或freshFEX结论。

按停止条件，Word生产`value_flow.cpp`写回归档02ae原字节，移除新增mask/tail/work复杂度，不继续batch/位集调参。保留独立必要的Store修复，以及原case的算法无关65值/逆RPO覆盖；不保第二生产求解器或新API。候选源码/三产物/必要语义/raw四点/全部失败和撤回收据完整保留。随后单独保留版没有追加性能点，也不继承273时延。

### 当前实际保留版

当前da7962c6相对02ae只改`ir/store.cpp`和`ir_contract_test.cpp`，所有其它生产源码（包括Function求解器）逐字相同。独立保留版三个owning目标实际构建、source前后不变：main `7ec0b7f2…`、test `a8162b2a…`、IRtest `2fb34b05…`。在该精确产物上重新收口同四cases98断言与完整CoreMarkB/F CRC4983，原最大65535case用原求解器成功，证明容量修补独立于Word。新source的six-command自有目录核hash关闭。

源码阶段目录、编译cache和大临时产物均在Data2；没有stage/commit/push。当前只有必要构建/语义资格，没有普通compile、生成/运行质量、资源或freshFEX≤2×采用资格；大幅性能目标仍未完成。同步七份文档及root checkpoint，不把原失败或旧时延转标签。

资料：[原算法计划](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-ends-word-20261031/algorithm-plan.json)、[采样前范围修正](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-ends-word-20261031/algorithm-plan-r2-with-store-growth.json)、[原候选max失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-ends-word-20261031/checks-prepared/execution.json)、[02ae单条同故障](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-ends-word-20261031/baseline-max-only/execution.json)、[Store修补](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-ends-word-20261031/store-directory-growth-fix/receipt.json)、[组合98断言](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-ends-word-20261031/contract-summary.json)、[四点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-function-use-ends-word-direction-20261031/accounting.json)、[闭场](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-function-use-ends-word-direction-20261031/closure.json)、[Word撤回](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-ends-word-20261031/word-withdrawal/receipt.json)、[当前保留版](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-ends-word-20261031/retained/manifest.json)、[当前必要语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/function-use-ends-word-20261031/retained-contract-summary.json)。


## 2026-11-02：冻结基线后的框架v2与两个通用原型

本轮在独立工作树实施，原仓库source8cda及Git索引不动。新候选source151211da，9个源文件变化：首次sealed schema/SSA与可选semantic live/index/capture解耦；正式GPR admission一次生产精确point目录和完整原raw consumer/terminal关系。原A、02未应用、默认gate、两种RA范围及所有现行动态checker保留。三个control/metadata复用reader仍保原扫描，归入下一包，不隐瞒为目录已全面共享。

先重链接出SHA42e5dcd0…的逐字相同8cda基线IR ELF，再在独立缓存编译修改TU及private proof header所有引用TU。新translator和IR test链接成功；7个受影响case/4066断言通过。block-analysis既有fixture已编译但其完整swift_test未重链/执行。首版bfd97fc5的SIGABRT及漏依赖配方保留；兼容修复后151211da才授上述最少语义，不更改旧失败身份。

五项框架决策以[框架合同第12节](compile-framework-contract-2026-11-02.md#12-五项框架问题的算法决定)为准，包含准确raw/actual域、home Boolean窗口/有限分区算法、动态准入与独立audit分类、一次最终operand交付、原owner的最多3次RunAttempts。P3/P4尚待实现；不是整体框架已完工或已证性能上限。

三批普通方向在宿主路径校验/快速FEX的proc取证处停止，FEX未提供完整两scope合格分母；不报告新倍数，不跨CPU频率条件认领收益。所有私有服务和本地子进程均结束。产物、原失败、最少语义及方向完整性见[本轮记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/framework-first-20261102/framework-resolution-20261102/direction-feedback.json)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/framework-first-20261102/framework-resolution-20261102/build-02/build.json)、[最少语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/framework-first-20261102/framework-resolution-20261102/minimum-02/execution.json)。无stage/commit/push，最终6.3/6.4/6.5不变。
