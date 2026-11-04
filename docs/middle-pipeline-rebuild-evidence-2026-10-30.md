# 中间流程重构：历史根因与测量证据附录

**2026-11-02 修订17：当前状态统一见[主方案7.1](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)，后续合同见[7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)。重构目标保持paused；本轮仅修正文档，不恢复实现、构建或设备执行。**

当前8cda已合A（resident Get按实际consumer做用途独证），两目标构建通过；3个case中2过、1个fixture失败，跟进02修正未应用。尚无A的ordinary完整方向或提速资格，B（RawDirect）未实现。原raw、receipt、checkpoint及失败身份不回填。

最新ordinary仍为e0d6：Block完整Translate 2066.780ms、Function 1099.290ms，分别约为同轮fresh固定FEX的44.33×、23.24×。95bd与3625两轮PROF2归因均已闭合；3625的GPR独证695.636/103.768ms约占各自native admission的89%，不是A独占成本、可省时间或A的实测收益，不替换ordinary分母与端点。

原546行方案已包含native与物理交付；路线纠正须按实际退役、保留、新增工作及覆盖核对净减工，并以完整算法反馈判断继续或改线，不能用结构迁移、旧API退出或构建断言代替完整compile下降。最终不降质量、Block与完整Function各≤2×及Known/EarlyOff/Unknown合同不变。见[净减工与反馈纠正](spill-and-compile-root-cause-plan-2026-10-28.md#compile-net-work-correction)和[审查42](compile-gap-review-2026-10-31.md#performance-route-correction-latest)。

**以下前言、C4/C5及各批正文均为历史记录。** 其中“当前”“最新”“normal未启用/关闭”等描述只属于所列source当时状态，不代表8cda现状。历史主体和既有证据链接保持原字节；当前资格及暂停状态以上述主方案入口为准。

---

**同Store载体、首批machine schema与GPR压力读者已验证，完整normal替代仍待贯通。** 原Store同槽的Semantic → BackendVirtual → PhysicalReady是唯一程序主线；carrier/program/core已接线，完整实际machine合同、质量政策、state/独立check、transport与193个kernel的新程序读口尚未迁完。Known早建仍产公共可编辑语义，frontend不含ARM form/home；不扩dense Selection或新增管理层。实现边界见[主方案5.3.11](spill-and-compile-root-cause-plan-2026-10-28.md#5311-下一完整替代同一store中的后端虚拟与物理程序)，顺序见[7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)。

**最新状态（2026-10-31）：C4源码6f2f2dfd / main495bf4c6已完成四Android目标构建及本版10cases/805断言、physical-consumption 164721检查/1注入。** 已检查范围为同合同形态、真实implicit publication、FPR独立proof/原retry、真实resolver/资源handle与NativeReady/退休夹具；另11个native阳性保133d07c7身份。NativeReady生产编码分支已编译，完整预RA cohort、native state/内存transport和normal准备未迁完，新normal仍未启用；无本版CoreMark、compile/QoR/PMU/FEX结果。最终不降质量、Block与Function各≤2×同轮fresh固定FEX仍未完成。[C4检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/integration-02/c4-checkpoint.json)、[下一步](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)。

2026-10-31：实施主线按[现行主方案](spill-and-compile-root-cause-plan-2026-10-28.md)及[本轮差距审查](compile-gap-review-2026-10-31.md)。M1覆盖构造/公共解释，M2/M3共同缩短目标需求到实际绑定的决定链，M4覆盖请求/发布/结束。旧timer桶不限定新算法边界，成本预算不冒充收益，主方案6.3/6.4最终门原文不改。

前一C2／第80批的身份按[检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/checkpoint.json)记录；当前C3/6f7b的四目标与限定最少检查见主方案2.1及下表，不共享C2或aea结果。第78的25文件候选无大幅完整方向后撤回，495/main120e为第79批before；旧123d PMU及495/a10主要服务归因保持原身份。历史通过与失败不转移。

本轮文档依据C4三组件源码回执和有限M1复核；没有新增普通compile、QoR、PMU或fresh FEX数据。C4源码完整身份、原文件快照及文档修改校验保存在[本轮修订目录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/plan-revision-20261031/)；组件回执不授予整组构建/运行资格，最新设计结论见[差距审查17](compile-gap-review-2026-10-31.md#17-c4接缝与m1最新复核未构建不继承c3资格)。

## 0. 最新代码与证据身份

| 对象 | 已有资格 | 限制 |
| --- | --- | --- |
| 当前C4/6f2f2dfd / main495bf4c6 | 四目标；本版10cases805断言＋physical164721检查/1注入；原native A/implicit/proof/retry/resolver/resource/退休夹具 | 完整cohort/state/transport/normal未完；生产emitter分支只获构建；11阳性保133d身份，无新CoreMark/compile/QoR/PMU/FEX |
| C4文档轮1fa59a93，未构建快照 | 当时较C3改12文件，三组件冻结 | 保原文档轮资格，后续6f2f独立记录 |
| 前一已检查C3/6f7b18b2 / main1b1e0466，第81批 | 四目标；1native质量/retry70＋6既有例223＋1Function width46756，共8cases47049断言 | 本版未重跑其余11native，亦无CoreMark/lifetime/compile/QoR/PMU/FEX；normal关闭、完整目标未完成 |
| aea16f34 / maine3a51fd9，本批前版 | 四目标；native tag12例11过1失败、730/731断言 | 错误GPR marker/无效政策pool的fixture失败，后修真实政策域；11阳性不转给6f7b |
| f636296b，首轮构建失败 | 相对23dc改40文件、相对C2改46文件；initial、actual birth、GPR/FPR/width、原controller及raw/region源码接线 | program.cpp两处u32窄化错误、exit1、source未变；无新目标ELF/运行/性能，当时review必修未修；本批已修并按6f7b另记，最终form/state/check/MEM/kernel与normal仍未闭合 |
| 23dc79d9，前轮C3未构建快照 | 相对C2改20文件；当时仅字段/消费者接缝 | 保前轮文档证据，不再代表当前producer实现状态，也无通过资格 |
| C2／94cb29e9 / maind9a92ac9，第80批，最近已检查基线 | 四Android目标；10native/614＋7pair/1439＋6eviction/51，共23cases2104断言 | normal未接入，无该版CoreMark/compile方向/lifetime矩阵/PMU/FEX/最终质量；不继承cf79 physical检查，亦不转给C3 |
| cf79edbc / main4bed419c，第79批首组、第80批before | 四Android目标；6native cases293断言＋6既有cases338断言；physical164721检查/1项注入 | 同Store载体、机器live与有限core组件验证；normal未接通，无本版CoreMark/compile方向/PMU/FEX/QoR资格 |
| 495887ca / main120e，第79批before，生产精确恢复a10 | 四目标；三个生产ELF逐字等a10；唯一独立旧Function fixture1case28断言通过 | 原生产CRC/physical与下方before计时按原a10/同ELF身份引用，不继承861或cf79资格 |
| 861af9f6 / mainb9947fdb，已撤候选 | 四目标、14cases979断言、B/F CRC4983；B731.729→731.534ms、F804.854→833.553ms | 各侧/mode一点、工作量略异，无大幅方向；无统计/最终质量/资源/fresh FEX资格 |
| c0e22c5b / mainb95e83ae，第五组 | 四目标构建；晚write/width/fixed消费者已迁 | Function collectors旧断言失败，before88ee同例同断言也失败；后续检查/CRC未跑，整组未通过 |
| 88ee2548 / maindec41b10，最近完整必要检查通过 | 第四组source/state与matched roles；四目标、13cases33768断言、CoreMark B/F CRC4983 | 仅该版必要语义；无性能或最终采用资格 |
| 8b183700 / mainebf1d558，第三组 | 四目标、15cases1434断言、CoreMark B/F CRC4983；唯一reload全读写者 | 历史相依组，无性能测量，不转资格给88ee或当前树 |
| f47090ea / maind9902ea0，第78批第二组 | 四目标构建、12cases854断言、CoreMark B/F CRC4983；唯一exact-use owner与相关消费者 | 前一相依组，无性能测量，不转资格给8b |
| 58466977 / mainc7003854，第78批首组 | 四目标构建、10cases772断言、CoreMark B/F CRC4983；真实Writer邻接和首批消费者 | 同一在建服务的前一相依组，不是独立性能候选，不转资格给f470 |
| a10c7d3f / main120e58c2，本包before | 四目标构建；6文件actual direct-narrow结果目标合同；6cases303断言、physical164721checks/1注入、CoreMark B/F CRC4983独立通过 | 第78当轮before B731.728826/F804.853740ms，各mode一点；无fresh FEX/统计/最终采用资格，不继承22b方向 |
| 22b18ead / main3d626b60，已撤回 | 9cases962断言、physical164721checks/1注入、CoreMarkB/F CRC4983；B714.961→730.337ms、F815.108→818.285ms | 每side/mode一点，无大幅方向、统计/QoR/FEX资格 |
| 78f0083b / main7c4750dc，已撤回 | 四目标构建；3个既有case执行4次1095断言、physical164725checks/1注入、CoreMark B/F CRC4983；B727.676→727.420ms、F824.533→805.382ms | 各side/mode一点、工作量不同，无大幅方向或最终质量/FEX资格；不转给恢复源码 |
| 第76/77批before基线f7e6c3a8 / 原main e9229afe | 四目标构建；生产逐文件和main/swift_test/lifetime精确等于d391，仅原scratch fixture修正并通过2110断言 | 不继承4f04 after结果；相同ELF旧资格按原身份引用，无当前fresh FEX/最终采用资格 |
| 4f04f843 / main ac435344，已撤回 | 8cases3011断言、physical164721checks/1注入、CoreMark B/F CRC4983；唯一方向B742.573→748.575ms、F812.220→838.349ms | 各side/mode一点、少量工作量不同，无大幅收益；原失败及完整after留存，不转给当前代码 |
| d391c501 / main e9229afe | 四目标构建、10cases444断言、构造fast/slow各557224checks/11注入、physical164721checks/1注入、CoreMarkB/F CRC4983；方向B726.782/F823.502ms | 最近已完成方向，相对3ef单点−4.60%/−4.05%；不转给4f04，无大幅效果或freshFEX/最终采用资格 |
| 3ef11319 / main e4e43f63 | 原重发/退役最低语义；第74批before B761.850/F858.283ms | 各mode一点，保原身份，不转新字段/frame资格 |
| 0641c577 typed-current方向点 | B745.145458→749.408020ms，F885.665462→860.270114ms，原false-link各side/mode一点 | 无大幅收益，少量工作量不同，不孤立归因codec；无fresh FEX分母 |
| da7962c6 / main7ec0b7f2 M0 | B791.932693/FEX47.477448ms=16.6802×；F880.364420/FEX47.275781ms=18.6219× | 最近同轮fresh开发基线，每mode一对；不是3ef或最终统计资格 |

C3原始失败的[整合冻结](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/frozen-build-01.json)、[构建失败终态](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/build-01/build.json)、[原始构建输出](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/build-01/stdout)及[只读review/修正文档依据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/docs-integration-review-20261031/review-findings.md)分别保留。首轮build已结束，后续两次成功build与fixture失败/修正各保原身份。当前资格见[C3检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/c3-checkpoint.json)、[build-04](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/build-04/build.json)、[本版最少检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/minimum-semantics-c3-continuation-01/execution.json)与[关闭记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/minimum-semantics-c3-continuation-01/closure.json)。不能将C2旧ELF、aea的11阳性或历史计时转给当前。

第79批完整身份与限制见[检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c1-checkpoint.json)；通过证据为[四目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/build-04/build.json)、[三条最少语义命令](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/minimum-semantics-r2/execution.json)及[设备关闭记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/minimum-semantics-r2/closure.json)。core的区间scratch/已提交政策拒绝不代替目标完整准入；组件Ready不证明state/193kernel已迁，也不延用旧CRC或计时。

[3ef构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-delayed-retirement-20261031/build.json)、[3ef最少语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/delayed-retirement/minimum-semantics-summary.json)、[0641方向原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/typed-current/direction-prepared/accounting.json)、[M0原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m0-current-fex-baseline-20261031/compile-gap-summary.json)。

M0 ordinary为PROF=1，无PROF2。da的decode＋RPO＋opt为213.391/261.921ms，regalloc＋codegen为467.199/496.317ms，publish＋free＋other为111.342/122.126ms。Block opt含Seal/Facts，Function opt先停止而该工作落入other；codegen含Complete，other也含真正编译工作。完整阶段表见[主文6.1](spill-and-compile-root-cause-plan-2026-10-28.md#61-总目标已测规模与尚未解释的余额)。

独立PROF2诊断的assignment18.483/27.122ms、sort0.822/0.353ms只否定小项主攻；不能否定跨Describe/晚declaration/绑定的分配服务。诊断存在嵌套和观察成本，不与ordinary相减或累加。DCE/flags只是构造/公共工作的部分。当前没有足以保证≤2×的已证组合。

FEX约47ms为固定版本整个CompileBlock累计，包含decode、passes、RA、emit、IR交还和SMC/entry发布；线程/编译器初始化、外层lookup及server启停不在内。计数来自不同阶段，不支持“FEX只有255块/少一个数量级IR”。[固定FEX源码与计时复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)。

第75批全部必要语义、完整四点、无大幅效果的撤回及精确恢复见[实施记录](middle-pipeline-rebuild-implementation-2026-10-29.md#第七十五批首次facts共访负反馈与撤回)和[恢复资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/shared-final-facts-20261031/restored-qualification.json)。前两次失败与原d391同fixture失败仍保留；字段/frame、Block stream、Function Word、codec、C2扩张及独立共访的停止结论不因新候选更名重开。

另有[旧qualified Other离线原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/qualified-other-work-review-123d-20261031/report.md)：这是source123d strict PMU余集，不是PROF other桶。原517/575样本及周期分母闭合，全部unqualified继续排除；decode/build、inputs/fixed、residual、RA等多个服务仍有实际工作，不能由一次合并遍历消除，也不将这些历史份额当当前可省时间。

以下第1节起冻结于2026-10-30，从旧主文提取，保留节号、身份、失败和限制；其中“当前/下一步”均是历史状态，**不是现行施工指令，不定义新门**。历史R7/123d/FEX比值不能转标签为3ef。重写前主文在 `/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/plan-rewrite-20261030/before/docs/`。

勘误：默认 `ra_width_chain=false` 时baseline coalescing仍执行，未执行的是v2 trial/rollback；不能将baseline/trial重复分配整体归到当前。已有IntegerFacts、Block birth、Function入口宽度、inputs复用、fixed移交和正常输出不再列未来收益。旧P0关闭不禁止不同假设的必要归因，原Store目录缺陷已修复；中间阶段测试节奏以主方案为准。

### 0.1 紧凑移位：必要语义通过，完整方向无大幅收益后撤回

最终候选source78f0083b、main7c4750dc已成组构建及通过3个既有case四次执行1095断言、physical-consumption164725checks/1注入、CoreMark B/F CRC4983。唯一完整方向B727.675579→727.419967ms、F824.532966→805.382119ms，工作量不同、每side/mode一点，不作统计、单组件或质量归因。没有大幅收益支持新增opcode/profile/wire及消费者，整包29文件精确恢复f7e6，shift.h删除；当前四种设备namespace均独立核hash后关闭。

原07ef/4daa/9837构建失败、ff5f因Operand默认Plus而malformed的IllegalCode、c959因新fixture重复EndFunction的SIGSEGV及修正后78f成功分别保留；不更改terminal/schema/生产生命周期来掩盖失败。没有flags-only/private-x12/value-only的穷尽见证，也无最终生成/运行、资源或fresh FEX资格。完整最终候选补丁SHA为`f9328200154247087cc2c963bfd53af9097127a177549ea7dfe24d47e6adb3bc`。

[最终候选源码](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/final-qualified-candidate/full-from-f7e6.patch)、[必要语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/minimum-semantics-summary.json)、[方向原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/direction/accounting.json)、[撤回决定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/direction/decision.json)、[精确恢复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/withdrawal.json)。

### 0.2 旧123d库leaf有限复核

仅离线读取原样本和原符号，不重跑采样/展开。qualified分母B820样本/868765709 cpu-cycles period，F921/980295089 period；allocator self4.697%/6.105%，含明确allocator atomic/lock的并集6.923%/9.050%，copy/set2.462%/2.632%。associated并集已经含self；再纳copy/set及库初始化/销毁/生命周期的宽包络12.217%/13.218%也已含前述范围，不得相加。

宽包络含真实必要复制、初始化和所有权，既不是可删份额，也不是全部heap上界；项目self/unknown中的潜在工作未被冒认。完整库leaf30.942%/34.694%还含查询、排序、结构维护、atomic与cache flush，不是allocator成本。文件/服务归属和库leaf是同一批记录的交叉投影，不能与Other展开相加或换算当前毫秒。没有单一per-block临时owner可承担根治的证据，M4随真实结果生命周期收敛，不独立开展PMR/cache微调。[原报告](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p0-library-leaf-review-20261031/report.md)。

### 0.3 GPR最终核验替代：必要语义通过，无大幅方向后撤回

22b18ead / main3d626b60完成原owner内的正常Q请求归约和D/P有序固定核验；真实Function局部证明边界、CFG/live/入口宽度与恢复义务保留。最少9cases962断言、physical164721checks/1注入、CoreMark B/F CRC4983通过。唯一完整方向B714.960950→730.336552ms、F815.108293→818.285155ms，各side/mode一点；F工作量相同，B请求相同而最终IR/host略异，不作统计或独占归因。新增算法及诊断API撤回，停止窗口/游标/interval调参。

第77批独立保留版a10c7d3f仅保成功direct-narrow选择的实际结果目标输出及原fixed核验消费，未新增IR或结果投影。四目标构建、6cases303断言、physical164721checks/1注入和CoreMark B/F CRC4983独立通过；没有该版compile点或fresh FEX分母，候选点不转移。两个候选运行场和保留版语义运行场均已关闭；本次文档修改不启动新设备运行。

[最终候选源码](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/final-candidate/full-from-f7e6.patch)、[候选最少语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/minimum-semantics-summary.json)、[方向原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/direction/accounting.json)、[撤回决定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/direction/decision.json)、[保留合同独立资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/actual-result-retained/qualification.json)。

### 0.4 实际位置整包：必要语义通过，无大幅方向后撤回

本小节保留第78批当时状态；495/main120e是第79批before，当前C3工作树与最近已检查C2身份分别见第0节。

a10为before，六相依组至最终861/mainb994共25文件，覆盖实际Writer/独立关系、selected uses/reload、所有已识别normal晚owner/fixed读者、Function局部范围、source/state与matched交付。四目标及14cases979断言、B/F CRC4983通过；[候选完整源码与补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/owner-range-index/whole-package-current/files.json)、[候选必要语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/owner-range-index/minimum-semantics-summary.json)。

一次四点a10/main120e对861/mainb994：B731.728826→731.534357ms（−0.03%），F804.853740→833.553024ms（＋3.57%）。各侧/mode一点且work量略异，不能认定统计回退或孤立归因；所有四点CRC4983、自有场关闭。没有大幅完整收益，不追加快点或FEX分母。[整包四点原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/owner-range-index/direction/accounting.json)。

source fixture三次失败、被纠正的折叠猜测、c0e旧Function断言及before88ee同例失败均保留；final必要通过不抹掉这些事实。已撤25file新增算法，恢复22原文件并删placement_index两文件，唯一保旧scalar BirthOnly fixture更强owner/home/publication/end/version断言。当前source495887ca，三生产ELF逐字等a10/main120e；四目标恢复构建和该例1case28断言通过。原a10生产CRC/physical按同ELF原身份引用，不新跑相同矩阵，也不继承861资格。[撤回与精确恢复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/withdrawal.json)、[相同生产ELF核对](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/production-identity.json)、[恢复资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/qualification.json)。

owner/source/bitmap/grouped-storage/晚发现的继续调参停止。需要当前normal native主要服务权重再批准下一M1或Complete算法；已有32/64 flags、narrow对齐、consumer复用不能重算收益。SVM_PROF2/density/dump/verify不能冒充同一normal观察域。M1/M2/M3/M4及最终两倍目标仍未完成，6.3/6.4和下方历史原文不改。

<a id="05-当前a10生产的新native-pmu归因"></a>

### 0.5 第79批before的a10生产：既有native PMU归因

恢复后的source495887ca/main120e，每scope一次，原guest2000/CPU4只读准入、PROF1，无PROF2/density/dump/verify，无STOP/attach/注入。两record0退出且CRC4983，原raw约7MiB，自有场关闭；record报告lost/truncation为0，未知映射/无记录caller仍排除，不能写成零unwind缺口。严格Translate前缀与raw/API/当前ELF映射核对后，B759样本803579066period、F857样本915799836period进入互斥账。

当前周期服务：inputs/初始fixed16.12%/13.06%、Complete14.93%/20.23%、native bind/select/emit/envelope17.13%/16.35%、真正runtime::compile RA6.35%/6.78%，qualified未解析4.12%/2.59%。全部1616审计行及子项整数闭合，caller归服务不等于caller self；这些不是可删份额、当前普通毫秒或FEX比值，不与旧123d份额相加。第一Facts/uses/roster、单个proof/publication/binder不足主攻；下一必须按主要需求→实际决定→Complete/native交付整个服务选择算法，且M1/M4仍未完成。

[完整当前原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/current-native-attribution/service-ownership/report.md)、[主文完整互斥表](spill-and-compile-root-cause-plan-2026-10-28.md#582-当前a10生产的主要服务新native-pmu归因)。同raw归约不重采；无新ordinary性能/FEX分母或最终采用资格。

## 1. 结论与证据边界

1. **Spill 增长属实，而且高度集中。** 一次 fault-state 正确性修复前后的完整 Block 机器码重算为 GPR spill load `302→433`、store `201→272`，共增加 202 次静态访问。`matrix_test` 的四个入口贡献 184 次，占净增长的 **91.09%**。这不是新增编译单位或 FPR spill 造成的。
2. **必须保存的 fault 状态与分配器的放大效应要分开。** 修复使访存异常点之前的 guest 寄存器版本真正可恢复，不能恢复错误 DSE。与此同时，一个位置缺 scratch 的现行处理会提高整个分配单元的 reserve、缩小普通值池、重做分配；更多整段 MEM interval 又增加 reload 压力。需要整改后一个机制，而非删除前一个合同。
3. **当前诊断已把触发点缩小到实际指令。** 三个矩阵块的已记录失败为 `Select`，另一个为地址计算 `GetOperand`：现行规则给 base scratch 上界 3，又有 1 个尚未由现有 SpillReload region 承接的 MEM reload，`need=4`、`free=3`。应先核对具体形态的精确定价，仍有真实短缺再做局部修复；不能据此把旧批次每条新增 spill 都判为可删。
4. **Compile 差距跨多个阶段。** 冻结基线R7的R3普通42点完整对照中，Function/FEX 为 **23.9962×**、Block/FEX 为 **16.3291×**，两倍门均失败。同点粗桶中，Function 的 decode、优化、分配、目标处理及发码分别约占 **17.71%、18.04%、26.63%、25.53%**；Block 的分配与目标处理及发码合计约 **61.25%**。单独更换 allocator 或关闭验证不能解决数量级差距。
5. **最有价值的结构路线是减少每个值贯穿全流程的工作。** 已知目标时直接构造适合它的语义形态，少建注定消失的值；统一 IR 身份、operand 与用途拓扑；封口前处理真正 residual，RA 后只完成依赖物理位置的稀疏工作。局部 scratch/interval 修复单独推进，不把它夸大为全部 compile 差距的根因。

### 1.1 必须固定的版本

| 对象 | 实际身份 | 可以支持的结论 |
| --- | --- | --- |
| 方案冻结时ARM64整改后源码 | layout-r7，source `3ef7f002fc18183881661138e51fbe732f3de54b05ec9b62384b7d23288e92c3` | 当前代码路径、所有权、诊断计数与质量 |
| 方案冻结时普通Release主程序 | `dc5f0923132e1b19d3d1a3c38d5ee4e10cba5714caf6b6643bd407a68cab090e`，O3/ThinLTO，无 PGO | 本轮 R3 普通 42 点及当前诊断；不能套用旧 C31 PGO 耗时 |
| 历史 PGO 对照的 SVM | C31 source `3cb2a8f7…`，匹配 PGO `56fb68c9…`，profile `0e4affdd…` | 历史 external28 的总差距；历史相邻批次的阶段成本 |
| 历史 spill 对照 | `r3-guest-state-observations-v1` source `083355ba…` → `r2-state-orphan-guard-retirement-v2` source `0e4e3655…` | 该次 fault-state 修复的增量；不等于所有重构前后差异 |
| 实测 FEX | `a156752fae3a4f65fefb1ed95af980bcdfac43d7` + Android 补丁 `4ade908f47d73a89d9a883f70197a5824249a4d1f53f45dd4e3df868690e532a` | 以这个产物为比较对象，不混用其他官方版本 |
| FEX 普通性能产物 | Loader `8632943c…`，Server `9fe58a99…` | 历史合格普通总时间；诊断产物另记身份 |
| FEX 本轮诊断构建 | Loader `cc1ff5f8…`，来自当前 cache 配方；恢复后普通 Loader `28221a86…` | 仅本轮阶段诊断；与 `8632943c…` 的可执行段也不同，不能称只是 strip 差异 |
| CoreMark guest | `d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba` | 本文全部矩阵 PC 的归属与准确输入 |

当前代码/Git/WIP 均保留。本轮分析不恢复旧源码、不将旧 PGO 强套到新源码、不提交或 push 既有 WIP。

### 1.2 历史回退没有被相邻检查点覆盖

旧 `9bb` 与后来的 guard-R6 两批 CoreMark PMU 中，Function 指令斜率约增长 33.05%、wall 约增长 25.70%；Block 约为 12.27%/8.06%。这是需要解释的历史信号，但构建、策略和批次不完全相同，不能直接当因果 A/B。

C31 与紧邻 guard-R6 的指令基本不变，只能说明该段没有再发生同等幅度增长。当前 R7 的 11 guest 质量矩阵不包含 CoreMark，FP 正常路径 51 条与 FEX 46 条也不能证明整数矩阵循环没有退化。

本节分析的 fault-state 修复早于 `9bb`，因此 **不能用本文的 +202 次 spill 访问包办 `9bb→guard-R6` 的全部回退解释**。后续回归追踪须保留原始历史性能锚点，同时使用具有正确 fault 语义的相邻基线验收新实现。

## 2. Spill：直接原因、放大机制与当前补证

### 2.1 静态机器码已经证明的增量

历史两侧 Block 共 2417/2414 份 host buffer，逐份核对长度和原 FNV64，再独立解码每个 A64 word。按两侧一致的 State ABI，spill 区是 `x28+[160,672)`；该批实际访问从 slot2/176 开始。只统计该区的 GPR/FPR load/store，排除了 guest memory、uniform、flags park 等访问。

| 指标 | 修复前 | 修复后 | 变化 |
| --- | ---: | ---: | ---: |
| GPR spill load | 302 | 433 | +131 |
| GPR spill store | 201 | 272 | +71 |
| FPR spill 访问 | 0 | 0 | 0 |
| 有 spill 的 Block 单位 | 40 | 40 | 0 |

guest ELF 的真实符号范围为 `matrix_test [0x21ab30,0x21b877)`：

| Block entry | 符号内偏移 | 每侧编译份数 | 每份 host bytes | 合计新增 load/store |
| --- | ---: | ---: | ---: | ---: |
| `0x21b2c2` | `+0x792` | 4/4 | 188→316 | +44/+24 |
| `0x21b3b0` | `+0x880` | 4/4 | 236→308 | +28/+16 |
| `0x21b1a0` | `+0x670` | 4/4 | 228→288 | +20/+16 |
| `0x21b188` | `+0x658` | 4/4 | 248→308 | +20/+16 |

同一侧同一 PC 的四份 body 字节相同；“四份”是编译次数，不是动态循环次数。四入口增加 1280 字节，其中 736 字节对应 184 条新增 spill 访问，剩余部分包含发表和其他编码。全部 host 增长 3696 字节；新增/缺失入口都没有 spill，不能解释该增量。

Function 需单独看：旧聚合为 spill defs/load/store `311/89/60→455/131/84`，有 spill 的单位仍为 13；六个单位的 reserve 从 3 档进入 7 档。该批没有 Function host dump，不能声称已逐字节复现其 bank、动态权重或每个值的变化。

### 2.2 正确性修复引入的是“可恢复义务”

例如 `0x21b2c2` 在若干矩阵 load 之前已经执行 `xor eax,eax`、`mov r10d,r8d` 和其他 guest 状态改变。后续访存若 fault，必须恢复到故障 guest 指令之前应有的 RAX、R10、索引及结果版本。正常 CFG 上之后会覆盖这些寄存器，不足以删除此前最后可恢复的 carrier。

修复后的 state DSE 在 `MayFault/Unknown` 边界清除相应覆盖证明；state sinking 把 fault、opaque 和真实观察作为发表边界。此前已有 PageFatal 回归证明错误删除不安全。

这要求的是 **版本可恢复**。它并未证明“每个观察点都立即 copy 到固定 home，并让所有旧 SSA 整段存活”是最便宜实现。若以后允许从物理 SSA home、spill slot 或可重物化值恢复，必须同时实现对应恢复消费合同；仅删除发表 MOV 属于误编译。

### 2.3 全单元 reserve 如何放大一个局部缺口

现行 `RunAttempts` 首先用默认 reserve 分配；有界 eviction 尝试后仍不能 Verify，便进入 reserve ladder。普通 L2 没有 compact/L3 的中间小台阶，可能从 3 直接进入较大的保守上界。

```mermaid
flowchart LR
  A[必须保留的 guest 版本或 owner] --> B[局部 live / alias 压力增加]
  B --> C[某个 site 的 scratch 加实际 reload 不满足]
  C --> D[提高整个单元 reserve 并重新分配]
  D --> E[更少普通值位置；更多整 interval 放入 MEM]
  E --> F[更多 load/store 和未承接的 reload]
  F --> C
```

关键点在于两个整单元最大值不一定来自同一位置：`max_scratch` 与 `reload_bound=max(operand_count+1)` 相加是安全的保守兜底，不是同时峰值已经被证明。GPR/FPR 也一起提升；本批额外 spill 却全是 GPR。

当前池中有 10 个普通 GPR。reserve=7 时只剩 3 个普通值位置，固定 guest homes 的规则另算。现行 spill 主要把整个 interval 放入 MEM，而非只在短缺位置切开，因此局部冲突会让循环内多次访问都付费。

历史池为 10 GPR/12 FPR，且 allocator、scratch verifier 和 spill emitter 两侧没有改动。Block 离开 reserve3 的实际单位数为 16，与四入口各四实例一致；但旧日志缺逐单位 attempt 和 IR，**这仍不是逐 SSA 的历史因果证明**。

### 2.4 已存在的优化必须作为基线

不能重新提出“加显式 reload”或把所有 reload 当成重复收费：

- `BuildSpillReloads` 已在分配后、Verify 前为重复用途、可直传的 definition→consumers、部分相邻 `GetOperand→LoadMemory` 建立有物理 GPR 的 region。
- writer 已把这些 region 的位置标 dirty；scratch 检查对 `HasSpillReload` 的 GPR MEM 值不再额外收费。
- 非 precise Add/Sub 的特定 outside-pool 情形已有互斥峰值规则 `max(base,headroom+reload)`；不能再统一扣减。
- 当前 precise 路径按实际实例需求检查，普通 MEM 结果、未获 region 的输入、terminal 以及 MEM FPR 仍有真实费用。

整改对象是 **现有 region 无法承接的实际失败位置，以及全单元 reserve 的扩大方式**，不是重建一份同样的 transport 表。

### 2.5 当前 layout-r7 的独立诊断

在红魔 `bf1359ff` 使用现有 R7 ELF 和现有诊断开关采集 Block/Function，各一次 CoreMark2000，CRC `0x4983`、退出零，argv/ELF/guest/前后身份均保留。没有修改 CPU 频率，没有新生产探针。本次启用 IR、host dump、RA 与 PROF2，时间包含诊断成本，**不用作性能比值**。

当前 Block shape：2415 单位、40 spill 单位，defs/load/store **1096/441/284**，10 GPR/16 FPR；reserve 为 `3:2377, 6:4, 7:34`。这与旧 12 FPR 配置不同，不能把当前 ID 倒填历史日志。

四个矩阵入口各四实例恰好覆盖全部 16 个 `restarts=0, attempts=3, path=ladder` 单位：

| 当前 entry | 首个已记录失败的当前 IR | 可用/需求 GPR | 尚未承接 reload | 最终 reserve 推导 |
| --- | --- | ---: | ---: | --- |
| `0x21b188` | `@83 Select` | 3/4 | 1 | base3 + reload_bound4 → 7 |
| `0x21b1a0` | `@63 Select` | 3/4 | 1 | base3 + reload_bound4 → 7 |
| `0x21b3b0` | `@60 Select` | 3/4 | 1 | base3 + reload_bound4 → 7 |
| `0x21b2c2` | `@66 GetOperand` | 3/4 | 1 | base3 + reload_bound3 → 6 |

失败日志的 FPR 为 `free=16, need=3, reload=0`。最终 reserve 列由当前源码、最终 IR 和聚合 shape 交叉推导，原生每-attempt 日志没有直接打印该字段。

全量 census 是 2375 个一次分配、22 个四次、16 个三次、2 个三次，共 **2517 次 attempt，比单位数多 102 次（4.22%）**。这是不加权的次数，不是 RA 或总 compile 时间比例。`ra-budget` 的 343 行是失败站点日志；Verify 以 `ok &= CheckInstr` 检查所有位置，terminal 也可能重查，不能叫 343 次失败分配。

现有日志还没有每个值被 spill 的 reason、完整 EA operand 与每 rung 映射。故当前可以说“已定位记录到的 scratch 缺口和后续阶梯路径”，不能说“已解释全部 spill 的逐值来源”。

独立解码当前全部 Block host，精确复得 441 load/284 store，全是 GPR。四个失败位置在最终成功 mapping 下的窗口，分别仅为 `LDR+CSEL` 或 `LDR+ADD`，都是 8 字节。`Select`、`GetOperand` 的 base3 来自现有 `resources.cpp::ScratchBudget` 的默认规则，并非已经证明这些形态同时需要三个临时寄存器。该窗口也不能覆盖 flags merge、narrow value、EA fallback 等其他合法分支，因此不能直接给所有此类 opcode 统一降价。

Function-mode 进程诊断必须独立解释：共 439 条分配 census，包含 **432 个 Function parent 与 7 个独立 Block fallback**，合计 472 次 attempt（额外 33 次，7.52% 的不加权数量）。两个矩阵根 `0x21b17d`、`0x21b390` 各出现三次，分别记录 `@96/@91 Select` 预算失败；含 `0x21b2c2` 的 Function 根 `0x21b2bb` 却一次成功、零 spill，不能套用独立 Block 的失败结论。

最终 shape 为 435 个 emitted code object、13 个 spill 对象，defs/load/store `455/131/84`、reserve `3:422,7:13`。432 个 Function parent 经 424 个单 parent 对象和 4 个双 parent 对象形成 428 个 Function code object，再加 7 个 Block 对象等于 435；没有四次未解释失败或悬而未决 parent。分配尝试以 parent 为分母，shape/host 以发射对象为分母。

### 2.6 Spill 整改的具体顺序

| 顺序 | 改什么 | 删除或避免的工作 | 必须保留的合同与验收 |
| --- | --- | --- | --- |
| P0：补最小 witness | 仅在选定矩阵 entry、每 attempt 首个失败记录 unit/mode/rung、base/reload/free、值 ID/区间端点、spill reason | 不建第二图，不加逐节点全量日志；关闭诊断后无数据路径成本 | 区分首个已记录 Verify 失败与首个分配事件；Block/Function 分开 |
| P1a：精确 scratch 定价 | 在现有 `ScratchBudget` owner 内，对已识别且全部合法发码分支峰值可证明的 Select/EA 形态定价；输入准备与最终检查共用规则 | 避免默认保守上界误触发全单元 reserve ladder；已有 SpillReload region 可能随真实 headroom 恢复生效 | 全局默认 reserve3 不变；计入 flags flush/merge、窄值转换、EA fallback、alias 和嵌套 lease，不凭最终两条指令拍预算 |
| P1b：局部资源修复 | 对精确定价后仍存在的缺口，在既有 allocator/SpillReload region 中表达 site scratch/alias/early-clobber；局部 eviction、interval split 或 transport；GPR/FPR 分别处理 | 避免为一个短缺位置扩大整个单元 reserve 和反复 full scan | 完整 scratch/fixed 验证、实际 emitter lease、旧 owner preservation、CFG/terminal 与跨块用途不减；保留安全 ladder 兜底 |
| P4：恢复值的位置及剩余桥接 | 在既有 Allocation preservation/fault metadata 中绑定版本与真实位置，支持可消费的 SSA home/slot/remat；合并已证明的 width/home/mapping copy | 缩短 eager publication 形成的重叠区间，减少恢复义务的实现成本和重复 transport | handler 精确恢复字节/版本/high32/flags；helper/unknown/显式观察仍 flush，保留 partial writes/fixed alias/首候选规则 |

P1 的第一原型先对四个矩阵 Block 做定价覆盖审查，仍走正常 allocator；不得通过改变 guest、换 Function→Block、减寄存器池、无证明地降低 scratch 预算或跳过 Verify 过门。先证明 spill 访问与高 reserve 单位下降、未转成更多其他指令，再验证普通运行 PMU；静态 spill 减少本身不算运行提速。

源码复核得到一个具体首包：在既有 Inst precise 重载中，对已验证的普通 scalar `Select` 采用 GPR 上界 **2**（共享 flags 临时≤1、VIXL 不可编码 mask 临时≤1），不依赖 LocalCondition/PendingNZCV/SelectActions 的动态状态。`GetOperand` 以旧 `EmitOperand` 回退的上界覆盖两种 EA 路径，宽 value base+宽 value index 为 **1**；窄双 GetHost 输入的旧路径仍可真实需要 **3**，保留默认。这样在已记录的 mapping 上有 `2+reload1≤free3`，但仍可能暴露后续失败，不能保证所有单位立即单次成功。

这个首包不改全局 reserve3、FPR 预算、pool、Add/Sub overlap、IR 或 interval。需覆盖 SP/zero/manual 特殊 home、MEM input/result、pending spill、所有 flags 分支，以及 combined audit 累计 VIXL 借过的寄存器集合。若进一步让 direct-dst EA 按 feature 定0，新增 feature 依赖必须进入现有 `ScratchPreparationInputs` 匹配键；第一包不走这个更激进分支。完整源码证明和 fixture 清单见[精确定价复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/svm/scratch-pricing-review.md)。

Function 另测跨块 interval/edge transport，不用 Block 成功替代。P4 涉及恢复语义，必须独立于 P1 和 IR 存储迁移提交，避免同时改变分配、恢复和表示后无法归因。

## 3. Compile：完整成本账与真实计时边界

### 3.1 最新合格的外部总差距

本轮 R3：42 个新进程、两个模式各七个 current/previous/FEX triplet，14 个 fresh FEX 分母，原 env/argv/PID/CPU4/FD/400Hz/no-loss/服务生命周期门保留。执行与归约完成，所有点有效；最终 `≤2×` 数值门两个模式都失败。

| 当前 CoreMark2000 | 当前 R7 generic 中位 ms | FEX 中位 ms | 七对比值中位数 | 七对范围 |
| --- | ---: | ---: | ---: | ---: |
| Function | **1143.169628** | **47.639635** | **23.996188×** | 23.831753–24.648300 |
| 独立 Block | **778.367086** | **47.528281** | **16.329067×** | 16.187934–17.040659 |

同批整改前 C31 generic09a 的独立中位为 Function 1137.232820、Block 783.788801 ms。current/previous 的配对比中位分别 **1.005374743 / 0.992542066**，范围 0.990661–1.035280 / 0.969164–1.020506；只能说这次结构整改没有带来大幅编译提速，不把小中位变化称为稳定收益或回退。

这组当前 generic 与下面历史 PGO 不同，不能把 21→24 倍或 13→16 倍直接归因给目录整改。历史合格 C31 PGO 的 external28 保留如下：

| CoreMark2000 | SVM 独立时间中位 ms | FEX 独立时间中位 ms | 七对比值的中位数 | 七对范围 |
| --- | ---: | ---: | ---: | ---: |
| Function，C31 PGO | 1004.728388 | 47.193125 | 21.005411× | 20.634127–21.479383 |
| 独立 Block，C31 PGO | 620.956892 | 47.502604 | 13.103481× | 12.621382–13.314632 |

共 28 点，14 个独立 fresh FEX 分母。比较的是明确定义的完整 compile-service elapsed；双方现有端点有差异，不是同 IR 工作量下的纯 CPU 成本。比值取每对的比值再聚合，不用两列中位数相除代替。

此前 R1/R2 两次尝试因频率合同变化/捕获失败停止，失败收据与所有原点保留，未拼接。本次在只读观察到 CPU4 上限恢复 1171200 后，冻结全新 R3 批次，完整执行；没有写入频率。正式普通对照用 protected FEX `8632943c…`，不使用阶段诊断 ELF。

### 3.2 冻结基线普通R7：同批逐点闭合后的组成

下表来自本轮 R3 的当前 R7 各七点，与 3.1 当前总差距为**同批、同点、同版本**。每点先计算 `other=translate−七阶段−显式锁等待`，全部闭合后再汇总。百分比用 `Σphase/Σtranslate`，合计 100%；毫秒列为各桶独立中位数，不能相加要求等于总中位数。

| 成本项 | Function 中位 ms | Function 份额 | Block 中位 ms | Block 份额 |
| --- | ---: | ---: | ---: | ---: |
| decode 与前端构造 | 202.572 | 17.710% | 80.509 | 10.354% |
| RPO/编号 | 6.879 | 0.612% | 0 | 0% |
| 优化桶 | 206.060 | 18.037% | 106.301 | 13.617% |
| 分配输入与 RA 桶 | 304.236 | 26.628% | 222.888 | 28.525% |
| 目标完成与真实发码 | 291.480 | 25.534% | 254.336 | 32.726% |
| 发布桶 | 41.862 | 3.648% | 35.625 | 4.481% |
| 原始 IR 释放 | 6.608 | 0.569% | 5.612 | 0.710% |
| 显式两处锁等待 | 0.166 | 0.015% | 0.885 | 0.116% |
| 未归类其他成本 | 83.118 | 7.247% | 73.566 | 9.470% |
| translate 总计 | **1143.170** | **100%** | **778.367** | **100%** |

锁等待很小；`ir_free` 很小也不能说明整个生命周期很便宜。other 仍有实际 setup、Facts、对象构造/析构、code allocation 等，不能将其强行分给某个优化候选。

当前/previous 共 28 个 SVM 点的完整整数账和原始绑定见[本轮普通成本账](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/compile/current-ordinary-coarse-ledger.json)。历史 C31 PGO 粗桶仍单独保留，不能移植为当前阶段时间。

### 3.3 每个桶里到底做什么

| 层次 | 实际工作 | 需要单独识别的放大 |
| --- | --- | --- |
| 请求与前端 | memory-cache 查询、模块/feature setup；x86 decode、typed IR 构造、guest-state forwarding；Function frontier、entry/split/reset/replay | 请求固定成本、重复 guest 区间、已建又擦除的 IR、每 operand 的 HIR use 维护 |
| 可变优化 | state DSE/sink/mapping、flags、value/width、DCE；反馈 worklist 与 bounded optional simplification | 每次 rewrite 的 use 注销/重建、epoch roster、pointer owner/lookup、真实 worklist 重访 |
| 封口事实 | 最终 SSA/liveness/flags/summary；按需 reverse uses 与 alias-resolved consumer 视图 | 与可变期身份/边的重建；不同合同不能简单删表 |
| 分配准备 | 单次 Describe 融合整数/FPR/fixed/spill/scratch/clobber/tie 输入 | 同一稳定目标语义后面又从 generic IR 识别；不能把已融合输入说成多次完整 pass |
| 分配算法 | Block/Function interval、fixed transfer、active、physical mapping、spill/region、eviction/retry/Verify | 全单元 reserve、整 interval spill、重复 full attempt；Function 跨块语义为真实额外工作 |
| 目标完成 | guest-home versions、observation/capture、residence/recolor、memory/flags/vector/edge 选择、physical writes 与最终合同 | 普通语义重复匹配；真正依赖物理位置的证明不能提前猜测 |
| 发码 | emission context、逐 IR dispatch/TickIR/lease、assembler、slow paths、offset/fault/link metadata | 已消失语义节点仍有 per-node 调度；临时容器与真实 encoding 成本要分开 |
| 发布 | code allocation/copy、ICache、fault catalog、provenance、SMC、links、code-object ownership | 批次 setup、持久 metadata；不能把恢复/退休对象放进临时 arena 后释放 |
| 结束与复用 | Facts/index、Allocation/actions、emitter、HIR lease、Inst release、workspace clear/capacity retention | 创建、grow、clear、destroy、最后 borrower 结束均计入，不能通过延后 free 美化 timer |

当前 SSA/liveness/flags 的基础 scan 已融合且缓存；`DescribeBlock` 已融合多个输入家族；attempt/FPR retry 已复用 immutable inputs。整改应找尚存重复表示，而不是再次包装这些已经存在的共享。

### 3.4 同名桶在 Block/Function 并不完全同边界

- Function 在 opt timer 结束后构造 `FromFinalIR/Facts`，落在 other；Block 的 return 求值仍在 opt scope，Facts 算在 opt 内。
- Function 的 Allocation/UnitActions 构造在 RA timer 内，Block 构造在该 timer 之前。
- `regalloc_total` 包括 `PrepareAllocationInputs`，但公开 `Allocate*` 内的 PROF2 从参数求值之后才开始；两者之差不等于纯 Describe。
- codegen 粗桶是 target Complete 与实际 Emit 两个顺序范围之和。Complete 返回后 `TakeIR` 释放 Facts，通常不在这个粗桶内。
- `publish_alloc` 在粗 publish 开始之前；分配失败后重 Emit 的路径也需单独记账。
- `ir_free` 只包原始 IR 清理。Prepared/Allocation/actions、emitter、HIR 最后 lease 的结束分布在别处。

因此下一次诊断以业务阶段的明确端点拆分，保留旧计时器可追溯性，不通过移动工作或计时器消灭 other。

### 3.5 嵌套热点能说明什么，不能说明什么

历史 C31 临时 generic PROF2 诊断各一次中，Function/Block 的 `regalloc_total` 为 312.0/255.8 ms，公开 allocate 子范围为 139.7/116.2 ms；Complete 为 186.9/176.8 ms，最终 `plan_verify` 为 18.8/12.9 ms，真实 `codegen_body` 为 76.4/67.9 ms。这些是带探针的嵌套范围，不能与普通 PGO 粗桶相加，也不能把整个 Complete 归给检查。

同份诊断 sort 只有约 0.6/0.8 ms。普通 PGO 调用链中 allocator/CAS/malloc/free 去重 union 约占确认 Translate 祖先 period 的 21.5%/22.4%，跨优化、Facts、输入、RA、emission、发布和析构。它不是 malloc 次数或可直接省掉的时间；旧 C32 的 pool/PMR 包未通过净收益门。

真正要降低的是这些调用背后重复建立的对象、关系和每节点访问。单独换 `Find`、sort、cursor 或容器，覆盖上限不足以支撑 2 倍目标。

### 3.6 冻结基线R7：不带IR/host/RA dump的阶段诊断

本轮又以现有普通 R7 ELF 分别运行一次 Block/Function，只开启 `SVM_PROF=2/PROF2=1/DECODE_PROF=1`。CRC、退出、实际 argv/env/CPU4、前后身份通过复核。该设置仍有逐指令计时、检查和 `svm-unit` 诊断，**不是 ordinary 性能测量，也不能与旧 PGO 或 FEX 不同探针粒度直接算阶段倍数**。

| 同点诊断项 | Block ms | Function-mode 进程 ms | 解释 |
| --- | ---: | ---: | --- |
| translate | 898.377 | 1302.677 | 各一次，不是稳定中位 |
| decode 粗桶 | 149.328 | 324.959 | 包前端构造；Function 还含 frontier/replay 等 |
| opt 粗桶 | 114.385 | 209.619 | 两模式 Facts 边界不同 |
| regalloc 粗桶 | 227.376 | 302.780 | 不是纯 scan 算法 |
| codegen 粗桶 | 278.220 | 316.204 | 包 Complete 与发射 |
| publish 粗桶 | 38.650 | 51.319 | code allocation 的边界仍见上节 |
| raw IR free | 6.086 | 6.412 | 不代表全部析构 |
| other，含显式 wait | 84.332 | 83.774 | 同点相减；Function 另有 RPO 7.610 ms |
| Complete 子范围 | 172.665 | 190.287 | 上面 codegen 内部，不能再加 |
| `codegen_total−Complete` | 104.251 | 125.663 | 其余初次 codegen 范围，不能当 assembler 纯编码成本 |
| guest instruction lowering 子范围 | 71.713 | 126.947 | 内含 append；不能再加 append 时间 |
| raw decoder scope | 8.787 | 13.685 | 与 distorm/predispatch 等有嵌套；不是整个前端 |

这里最直接的判断是：**把 decode 看成 x86 字节解析、把 RA 看成 linear scan、把 codegen 看成写 A64 都会错。** 当前仅 Complete 的诊断读数就远高于 FEX 整个编译服务的诊断读数；需检查 generic 语义→目标选择→物理完成的工作，而不是只改 allocator 内一个小循环。公开 allocate 子范围约 100.6/135.5 ms，父范围的其余成本也必须归因，不能把差值直接命名为纯 Describe。

该批 Function-mode 的 frontend、allocate、Complete、emit 计数域不同，含 fallback 和多 parent 合并；不会凭计数差编造失败或可删工作。完整原始字段与计时器语义见[当前阶段独立账](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/review/current-stage-accounting.json)。

### 3.7 123d 当前物理路径的限定 native 归因

原main33c04、source123d（生产c904）上的P0恰好B/F各一次、一次离线归约；strict raw-prefix exact Translate资格为B820/F921样本。Complete占qualified period17.36%/16.88%，物理形成12.01%/11.53%，实际发码7.65%/9.27%；其余qualified约63%/62%保留，不据此扩审。record lost/truncated0不代表完整栈，whole-target unknown/短栈和无法关联的unwind原值仍单列。

该证据选择核对Complete真实GPR约束/检查服务，未证明整个服务可删或外部两倍可达；Packet约qualified总分母0.98%/0.76%，不支持格式小包承担主收益。两处真实不同阶段为：RA-attempt独立fixed核验，以及位置/late writes完成后的最终fixed核验。后者已共享GPRPhysicalChecks的唯一FixedWriter前缀，不能再认领第二遍重建。02ae的P1首段已将正常私有direct责任收敛到最终核验，具体代码/必要语义见[第六十七批](middle-pipeline-rebuild-implementation-2026-10-29.md#第六十七批正常私有编译路径收敛-fixed-核验责任p1首段)，尚无新的ordinary降时。

[本组结果](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-current-physical-ab-native-20261030/P0-result.md)、[原分析](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-current-physical-ab-native-20261030/native-offline/analysis.json)、[fold反例](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-constraints-final-fold-decision-20261030/report.md)。这些样本和前述旧8716各保原身份；不转标为02ae/main9c03，不按native period分配普通毫秒。

## 4. FEX 的流程对照与可借鉴边界

### 4.1 FEX 的约 47 ms 没有漏掉整个普通编译流程

固定源码的正常同步路径为：

```text
CompileBlock：总 JIT elapsed
  PreCompile / invalidation lock / lookup
  CompileCode
    GenerateIR
      raw decoder reown/reset、DecodeInstructionsAtEntry
      BeginFunction、各块 guest lowering、Finalize
      decoder delayed disown
      x87（按需）、DFE、RA、可选 assertion validation
    frontend 后二次 lookup，竞争命中则丢弃本次 IR
    ARM64 backend：emission、RIP recovery tail、copy/lock/ICache
    IR delayed disown
  relocations/PostCompile/code-page-SMC/entry publication
```

总 timer 在 PreCompile 之前建立，GenerateIR、RA、backend 同步执行。该批关闭 AOT/object cache，不存在把整个编译工作放到后台线程的解释。ARM64 timer 读 `CNTVCT_EL0`，转换使用 19.2 MHz 的 CNTFRQ，而不是 CPU 的 1.1712 GHz；不存在据此修正掉二十倍的单位错误。

确有不在该 timer 内的部分：每线程 compiler 初始化/最终销毁，dispatcher 已命中的 L1/L2 查询，返回后的 ExitFunctionLink，以及独立 CompileSingleStep 入口。尚无证据证明它们能填平差距，不能任意摊入分母。

### 4.2 真正不同的是每个值所携带的工作

| 机制 | 固定 FEX | 当前 SVM | 正确借鉴方式 |
| --- | --- | --- | --- |
| IR 存储 | 20B OrderedNode + opcode 实际 payload；32-bit ref，两个 bump 区 | Inst 槽 96B，已是 TLS arena/freelist；Function 另有 HIRValue/HIRUse | 对比完整 node/payload/edge/owner/初始化流量，统一存储 authority；不重复提“改 pool” |
| 构造 | raw 边界先稳定；固定 bitmask/ring32 常量、局部即时折叠 | 已有 typed builder、局部 folding、guest-state forwarding | 真正少建节点/边；不得仅把同一次工作改名提前 |
| guest-state 缓存 | 每条 guest 前有 `FlushRegisterCache(true)` | 保有自己的 fault/partial/helper/publication 规则 | 不把 FEX 描述成默认跨 guest 一路不 flush，不照搬其较窄恢复政策 |
| 优化 | 默认 x87→DFE，RA 后续；无默认通用 Compact/ConstProp | 更丰富的 state/flags/value/width/Function 优化 | 保留已有收益与合同，统一底座而非直接删除 pass |
| RA 范围 | 各 CodeBlock 逆序 kill/SRA preference，正序分配/merge/spill | 独立 Block 与 Function 跨块 interval 两种范围 | Block 可借鉴窄状态和局部修复；Function 不降格 |
| 物理结果 | 直接改写当前 IR 的 operands，node.Reg 带目的位置，PostRA | sealed generic IR + Allocation + 稀疏 UnitActions | 同一物理结果只有一个 owner；避免完整 source/完整 MIR/同义 certificates 三份并存 |
| 临时内存 | thread-persistent buffers、逻辑 reset/disown、后续 reclaim | 已有 workspace、HIR lease、Inst arena | 只消除仍存在的真实 grow/初始化/析构；完整计 retained capacity |
| 检查 | assertions OFF 不跑 IRValidation；生产 RA hazards 等仍检查 | optional audit 可关，mandatory fixed/width/scratch/recovery 仍在 | 用构造不变式消除重复求证；不关闭生产合同 |

FEX 的 20B 不是完整 node 总成本，IR 也不是“无链数组”或“完全无 heap”。默认 DFE 仍有 CFG/worklist 重访，RA 仍有 spill/fill/merge 和 RAW/WAW 检查；不能用虚构的单遍无检查流程做设计依据。

### 4.3 工作量分母必须同点采集

另一次 FEX DumpIR 记录 282 请求、8575 CodeBlock、printed nodes `214888→118181`，但其 TSO 环境与 external28 不同，也带 dump。不能用 47 ms 除以上述节点数得到每 IR 成本。

FEX header 的 `NumHostInstructions` 实际来源是 frontend decoded instruction count；SVM 的 `ir_insts=82819/69345` 也是 ID slots，而非精确 live nodes。请求、blocks、decoded guest、allocated/live IR、operand visits、host instructions 必须分别记。Function 更少请求而解码更多 blocks，并不矛盾，也不能仅凭请求数解释全部差距。

当前已补一次无dump的work总量诊断：SVM基于Z/AA普通a97源码，4cpp私有计数沿原visit采集后已精确还原；FEX固定a156/Android补丁与独立计数构建。两SVM点、一个FEX点均CRC4983、身份与服务收口闭合，没有生成质量矩阵或诊断延迟比较。

| 实际量（不同表示不强行归一） | SVM Block进程 | SVM Function进程 | FEX |
| --- | ---: | ---: | ---: |
| guest handler/dispatcher接受数 | 15585 | 25164 | 29915 |
| 成功发布的IR/RA单元 | 2413 Block | 432 Function region | 255 compile roots |
| 发布对象 / backend调用 | 2413对象 | 428对象（含4个双region对象） | 255 backend调用，对象未单独计数 |
| RA前listed instruction/code nodes | 61749 | 73300 | 139954 |
| 声明data结果 / GetHasDest | 28461 | 38556 | 75765 |
| 普通data输入occurrences / RA data-prefix引用 | 36343 | 49338 | 80324 |

SVM的全部valid final snapshot均与成功publication按scope、Store地址/session、kind唯一关联，无歧义或漏项；其data结果含flags-only数值Return，不能叫实际allocated SSA。FEX的RA前还会在RA内裁死值，backend code nodes为91970、Dest27952、物理化data引用为16536 node＋46003 immediate；不得将后RA物理参数当SSA边与SVM直接相除。FEX结构headers另计，create成功挂链181122为compile-scope；SVM issued70436/97137为全进程构造前身份，scope与失败/tombstone含义也不同。

该证据不支持“SVM只是live N/E比FEX大一数量级才慢”的解释；也不能据此把不同语义IROp或阶段当作等价工作算每node速度。下一步优先追查实际重复描述/查询、分配与发射的成本。普通724/887ms规模点和本诊断各有独立源码/ELF收据，不将历史47ms或本次diag ticks拼接成新倍数。[SVM独立核验](</Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/svm-compile-work-census/independent-review/parallel-work-review.md>)、[FEX原始计数归约](</Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-compile-work-census/analysis/report.md>)。

### 4.4 分阶段计时的缺口与补充诊断

普通 FEX 性能 ELF 的 scoped profiler 关闭，旧 perf 只有 907 样本且 132 条坏链，缺完整内部符号。此前没有可靠阶段分解；本轮已用独立诊断构建补足，仍不能把诊断结果冒充普通性能 ELF 的精确阶段时间。

本轮补充诊断仅用固定大小的 per-thread aggregate，在已有同步边界计时，保留原始总 timer 和 CNTFRQ；不启用逐 IR trace、不加全图扫描。诊断 ELF/source/config 独立登记，结果不覆盖历史普通 ELF 的 47 ms，也不用于宣称当前外部性能达标。具体阶段收据和限制以第 8 节证据目录为准。

### 4.5 FEX 本轮三点阶段诊断：完整闭合的实际结果

固定 source + 平台补丁 + 临时探针，独立 Loader `cc1ff5f8…`。三点均 255 个 CompileBlock 请求、CRC `0x4983`、退出零，aggregate 资格/父子闭合通过；私有 FEXServer 正常停止并 wait，socket 关闭。诊断总值为 48.028750、47.502448、47.932917 ms。

以下展示 **core-02 一个点**，互斥桶精确闭合到 912047 CNTVCT ticks（19.2 MHz），不是独立中位数拼接：

| 工作 | ms | 份额 |
| --- | ---: | ---: |
| raw decode 与 decoder frontier | 11.769 | 24.78% |
| IR builder / guest lowering | 10.573 | 22.26% |
| x87 入口/快速拒绝 | 0.057 | 0.12% |
| DFE | 6.097 | 12.84% |
| RA | 8.680 | 18.27% |
| backend，含 emission/tail/copy/ICache | 6.046 | 12.73% |
| publication | 2.124 | 4.47% |
| backend 之外的逻辑 release | 0.199 | 0.42% |
| 其余 setup/lookup/父范围本身与探针成本 | 1.958 | 4.12% |
| **总计** | **47.502** | **100%** |

backend 内部的 emission 为 4.436 ms，tail/copy/cache 为 1.095 ms，剩余为 backend setup 等；这些子项不能再加到上表。逻辑 release 已去掉 backend 内的那部分，没有重复求和。三点 RA 为 8.680–8.771 ms，backend 为 6.046–6.264 ms，builder 为 10.573–11.147 ms。

这不是“FEX 免做 RA/emission”，而是它用更直接的表示与物理结果，在这些阶段做的工作很少。当前 SVM target Complete 本身约 173/190 ms 的诊断现象，使 **RA 前目标成型、RA 后物理结果直接消费、跨阶段表示删除** 成为优先验证的方向。但双方合同、请求/块/值工作量和探针粒度仍不同，不将 `190÷6` 当作已证明的可优化倍数。

探针 v2 曾因真实 guest `exit_group` 直接系统调用退出、绕过 ExecuteThread/Context 析构而没有报告；该次 raw 和失败收据原样保存。v3 在现有 `exit/exit_group` 收尾前通过窄导出函数读取 Core 同一 TLS，不产生 Loader 的另一份统计。只验证本次单主 guest 线程，不能声称 exit_group 收齐其他线程。探针源与普通构建缓存均已恢复且二进制前后哈希相同，普通性能 bank 没有替换。

数据及精确合桶见[三点实际收据](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/diagnostics/fex-stage-r2/execution.json)、[core-02 互斥成本账](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/compile/current-fex-stage-ledger.json)、[探针边界与校验规则](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/fex/probe-v3/README.md)。

同点原 SHMStats 与探针根之差为 1079–1088 ticks，约 56.2–56.7 µs；这是嵌套边界差，**不是插桩开销的上限**。SVM 的十万级细粒度 clock/atomic、unit 日志与 FEX 阶段级探针明显不同。恢复后的 `28221a86…` 与性能 bank 的 `8632943c…` 可执行段长度、地址和 SHA 也不相同，不能用 strip 解释；精确编译配方等价性没有因此建立。普通外部对照始终使用原 `8632943c…`，诊断阶段只用于独立归因，详见[二进制代码身份](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/fex/ordinary-code-identity.json)和[独立跨引擎边界复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/review/current-stage-cross-engine-boundaries.json)。

## 8. 原始证据与复核入口

本轮结果实际保存在 Data2：

- [根分析目录](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/)
- [最新普通 R3 的 42 点归约](/Volumes/Data2/swiftvm-middle-preserved-20261025/compile-gap-20261028-r3/stage/compile-gap-summary.json)，[Root 自原始字节独立重算比值](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/compile/r3-root-raw-numeric-recompute.json)
- [R3 完整原始证据离线复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/compile-gap-20261028-r3/independent-final-review/REPORT.md)，[全部资格与精确比值](/Volumes/Data2/swiftvm-middle-preserved-20261025/compile-gap-20261028-r3/independent-final-review/review-r2.json)；复核者同时是 R3 适配作者，属于独立方法重算，不冒称人员独立
- [历史 spill 重算与源码绑定](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/spill/spill-causality.json)，[四矩阵块完整 A64](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/spill/top4-block-host.asm)
- [当前 Block 诊断收据](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/diagnostics/current-r7/execution.json)，[Function 诊断收据](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/diagnostics/current-r7/function-execution.json)
- [当前普通逐点完整成本账](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/compile/current-ordinary-coarse-ledger.json)，[历史 C31 成本账](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/compile/coarse-ledger.json)，[计时器历史源码绑定](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/compile/timer-source-bindings.json)
- [SVM 逐路径诊断](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/svm/analysis.md)，[最后消费者与计时边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/svm/lifecycle-and-timers.md)
- [当前两种分配范围的矩阵失败补证](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/spill/current-diagnostic/analysis.md)，[Select/EA 完整 scratch 定价与验证建议](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/svm/scratch-pricing-review.md)
- [固定 FEX 源码诊断](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/fex/diagnosis.md)，[精确版本/阶段端点与未知项](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/fex/evidence.json)
- [external28 原始配对计时账](/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/compile-cost-external-engineering-gate-r1/independent-timing-ledger.json)，[完整独立复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results/compile-cost-external-engineering-gate-r1/independent-final-review.json)

上述历史文件只读保留；新增诊断有独立身份。后续实现必须引用确切 source、ELF、profile、guest 与原始收据，不能仅引用本文的简写数字。

后续 target-aware 设计的源码审查与本文改版前快照见[设计证据目录](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/target-aware-design-r1/)。旧性能数据、阈值和失败结论未改。

## 9. 2026-10-28冻结时的交付与限制（历史）

本节保留原冻结记录；其中“当前”“下一轮”均指2026-10-28该次分析，不是2026-10-30后的实施待办。后续状态按第7节和实施记录，执行优先级按重写后的第5～7节。

本轮完成历史 spill 机器码重算、当前两个分配范围的失败位置诊断、Select/EA 定价源码复核、当前普通 R3 全 42 点与逐点成本账、FEX 三点阶段诊断及本方案。R3 另经完整原 gate 离线重放和 Root 自原始字节的有理数重算，结论一致；所有采集者、服务和离线读者已关闭。本轮临时修改的 FEX 源码与普通缓存已精确恢复，关闭后的私有可再生成设备二进制已校验哈希后回收。

以下限制不能被“总账闭合”掩盖：历史首个失败的具体 SSA/区间与动态 spill 热点权重仍缺；当前 ordinary 的 other **7.247%/9.470%** 已有具体源码落点，但还没有逐构造/析构的独占毫秒归属；诊断 FEX 与 protected 性能 ELF 的编译配方等价性未建立。P0 只在这些剩余端点补最小计数/计时，不再构建通用追踪或平行证明层。

P1a 的 `Select=2`、宽 EA 上界1 是已审查的实施候选，尚未修改生产预算或执行新预算的真实 fixtures。P2/P3/P4 也未取得新性能收益资格。**≤2× 是明确的最终验收门，当前未通过；本文不保证某一表示重写必然足够。** 下一轮先冻结第6节协议及补齐当前普通 CoreMark 质量基线；P2a→首个有覆盖的 P2b、P3a 分别形成可否决的交付，P1a 独立过完整分支/combined scratch audit 与两种分配范围验证。本段记录方案冻结时的候选状态；后续实施状态以[实施记录](middle-pipeline-rebuild-implementation-2026-10-29.md)为准。按用户调整，允许在重构分支继续推进未完成最终质量验收的表示迁移；最终采用仍须通过第6节，不能将中间生成质量变化当作最终结论。

本轮再次复审的修改前快照、三项专项 review 及最终文档校验保存在[方案复审目录](/Volumes/Data2/swiftvm-middle-preserved-20261025/spill-compile-root-analysis-20261028/plan-review-r2/)。本轮是文档交付，没有新生产改动、构建、手机执行或性能结论。
