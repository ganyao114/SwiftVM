# SwiftVM 中间编译流程重构方案：以完整 compile 性能决定改造路线

2026-11-02 按[用户最后明确要求修订的546行版本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-recovery-20261102/04-last-user-directed-20261031/docs/spill-and-compile-root-cause-plan-2026-10-28.md)与[路线差异复审](compile-gap-review-2026-10-31.md#performance-route-correction-latest)修订。本文件负责当前算法、净工作和后续执行；[逐文件结构](middle-pipeline-file-layout-2026-10-28.md)、[IR合同](ir-representation-redesign-2026-10-28.md)、[目标绑定](target-aware-middle-pipeline-design-2026-10-28.md)细化同一要求，历史运行与迁移按原身份保存在实施记录和证据附录。

**最新执行方向：先冻结代码、固定框架并验证完整成本可行性，再补细节。** 原8cda实现已保全，框架工作隔离在[独立工作树](/Volumes/Data2/SwiftVM-framework-20261102)，原仓库源码和Git索引保持不动。阶段/数据流/查询上界及完成门以[框架合同](compile-framework-contract-2026-11-02.md)为本方案的现阶段约束；原A/02及opcode/cohort扩域均暂不推进。用户已进一步授权解决框架问题；具体算法、检查机制和请求级重做上界见合同第12节。独立工作树已实施首次分析解耦与完整raw/site关系两个通用包，框架整体及性能资格仍未完成，不因局部接通就宣称≤2×。

**目标：不降低最终生成质量，大幅减少完整冷 compile；独立 Block、Function 分别不超过同轮 fresh 固定 FEX 的两倍。** 当前仍未达标。下一主线从构造、约束、分配一直改到实际操作数交付；不再把已完成的物理格式、typed codec 或生命周期迁移列成未来提速来源。

**本方案规定算法、旧工作退出条件和正常/困难路径的工作量合同，不声称已经证明能兑现两倍门。** 只合并外层遍历还不够：必须减少其中的真实构造、解释、状态查询和整scope重算。第5.8节区分现码工作和目标合同，不新增管理层或测量框架。

**当前选用的承载方式：原IRStore的一份程序依次为Semantic、BackendVirtual、PhysicalReady。** 保留已经接通的真实机器use/def、原两范围RA、state和稳定operand交付；同槽、提前结束语义对象或少一张表均不提供性能结论。本路线须与“现有IR＋稀疏成功选择＋当前consumer一次交付”等满足相同合同的实现比较净工作，不作为减少重复决策的唯一前提。具体取舍见[5.3.11](#compile-net-work-correction)。

**阅读与状态优先级：** 第4节规定当前实现的交接，第5.8节规定真实工作与退出要求，第6节规定反馈/采用，第7.1节是当前状态唯一入口，第7.2节是当前框架阶段的执行表，原细节阶段不恢复。第2.1及5.3.12的各source、C批次、失败与断言是有版本的历史证据；其中“当时未启用/仍失败/下一组”不产生新的待办，也不覆盖7.1。旧细节目标不恢复；本轮只推进独立框架工作树中的通用算法与最少纵向反馈，原仓库源码/索引继续冻结。

**原仓库冻结实现8cda2889；独立框架候选151211da，两目标构建及7个受影响case/4066断言通过。以下普通计时仍为历史e0d6e7e8：Block 2066.780070ms、Function 1099.290002ms，分别为同轮固定FEX的44.33×、23.24×。** 两轮独立归因95bd/3625已经完成；GPR独立证明占native准入约89%，不是完整compile的89%。当前A已退出一段按Get全scope寻找actual uses的发现工作，两目标构建成功，三个原例2过1新增fixture失败；尚无A的普通计时。归因、当前代码、旧语义阳性及ordinary分别记录，完整资格见[7.1](#71-当前代码状态)。

**现行实施合同：** 同一Store在首次machine live前交真实use/def、guest home写入和可恢复metadata；原Descriptor直接生产原GPR/residence/Derived-fixed输入，原Block/完整Function RA消费实际程序。native语义预生产、flags全域修订、semantic→actual重绑定与WB一点兼容桥已退出。source SSA/CFG与必要semantic live、Explicit硬窗口/capture/lease和最终独立证明保持。冷metadata取回同一owner、从恢复后的actual程序重建；保原预算配置及独立FPR状态，不虚称跨重建累计eviction余量。详见[直接生产合同](spill-and-compile-root-cause-plan-2026-10-28.md#native-actual-input-service)、[默认切换边界](spill-and-compile-root-cause-plan-2026-10-28.md#native-default-cutover)及[净工作账](spill-and-compile-root-cause-plan-2026-10-28.md#581-当前knownnative的净工作账)。


第78批实际位置整包没有大幅收益，新增算法已撤回：Block731.728826→731.534357ms，Function804.853740→833.553024ms；各side/mode一点且工作量略异，不作统计或单服务归因。恢复版495887ca/main120e是第79批起点，不再标作当前源码。停止owner/source/hash/位图和晚发现的继续调参。[停止决定](#539-实施中实际位置生产与消费者复用)。

第77批进一步说明：即使正常最终GPR核验不再构造全N points/owners和排序writer-key，也没有获得大幅完整收益。停止Q窗口/游标/interval调参；不能把最终核验子服务代表全部constraints/RA/state/Complete。M1构造/公共服务、M2/M3实际位置/state/选择大链和M4真实触达仍需实质替代，**目前没有已证明可兑现两倍门的完整实现组合**。

## 1. 目标、范围与改线原则

### 1.1 最终同时满足的结果

| 维度 | 交付要求 |
| --- | --- |
| 完整 compile | 同 guest、功能与结束条件；独立 Block 和 Function 各自 ≤2× 同轮 fresh 固定 FEX |
| 生成和运行质量 | 最终正确性、异常恢复、spill/transport、编码、实际运行和资源满足 6.3/6.4；中间版本允许生成质量变化 |
| Block 分配 | 局部 uses/live、分配和恢复；不为凑 FEX 请求数改成 Function RA |
| Function 分配 | 跨块 SSA/live、entry/join/loop、call-return、parent/code object；单块 Function 仍遵守 Function 合同 |
| 前后端解耦 | frontend 表达 guest 行为；公共纯能力允许按已知 backend 提前优化；位置、运输、机器码由 backend 决定 |
| 可维护性 | 一个决定一个写者；数据有真实消费者和结束点；替换后删除旧循环、状态、转换和入口 |

完整服务从真实编译请求开始，包含初始化、decode/build、必要优化/分析、分配、目标完成、绑定/编码、发布及最后消费者后的真实释放或有界复用。缓存命中、减少 guest 功能、降低最终质量、移动 timer/清理、PGO 和缩小分配范围都不算本次达标手段。

### 1.2 本次纠正的路线错误

1. **旧 timer 桶不是新算法边界。** assignment 的 18/27 ms 只否定以排序/扫描为主攻；不能据此否定同时替代 Describe、晚 declaration、实际写入重建和最终绑定的分配服务。撤销“M3 因 assignment 小而整体暂缓”的决定。
2. **构造与公共分析必须有独立主线。** DCE/flags 的局部增量化不能代表全部 decode/opt/Facts。M1 扩为减少中间节点出生、重复 typed 解释和真实反馈，保必要的首次分析。
3. **已落地输出不再当未来收益。** 当前已沿用 3ef 的 typed-current、同源 kernel、原 true→false 重发和最终 Flush 后退役；codec 单次开发反馈无大幅收益，停止同类调参。
4. **保合同，不固化旧求解方式。** SSA 位置、guest-copy、宽度、fault、实际写入仍须成立；不要求保留每套候选表、重建链和全域遍历。能够共享访问时共享，依赖未来信息时保留必要求解。
5. **约束保留工作的成本。** 一次共访不等于少一次服务，一次role绑定不等于只读一次原用途，有序sites也可能隐藏查询次数×链长。每项列清真实读写和重复边界，不能只报pass数。
6. **不预分配收益。** 成本表说明必须覆盖的范围，不是每项可省毫秒。完整组合仍需实测，不用预计百分比相加凑两倍门；旧耗时覆盖率与文件/节点覆盖率分别报告。

### 1.3 已完成基础与已停止试验

| 内容 | 后续定位 |
| --- | --- |
| Inst/Arg、Store/order/uses；HIRValue/HIRUse 退役 | 唯一语义底座；不再做同义迁移 |
| CompileEnvironment、layout/bindings、Known/EarlyOff/Unknown | 保留真实分支和公共纯能力 |
| typed birth、浅 fold、完整 EA/load-extend、state/flags deferred、IntegerFacts/dirty | 已有能力；部分规则仍在 AfterAppend，不一概算作出生前消除 |
| 同 revision Facts、基础 SSA/索引共访、inputs 跨 attempt/FPR retry | 已复用；下一包只处理剩余首次解释和重复服务 |
| 首次checked Facts＋稳定目标输入共访 | 完整候选语义通过、开发方向无大幅收益，已撤回；停止回调/临时索引/容量调参 |
| fixed 核验责任移交、FixedWriter 前缀共享、实际成功 selection | 已有；不能再认领“少一次验证/再加二分”的收益 |
| 全族 native、typed-current 与193个同源 kernel | 已有；完整codec无大幅效果，停止同类调参；新增ScalarShift整包已撤回 |
| 单独紧凑化SHL/SHR/SAR | 必要语义通过、完整方向无大幅收益，新增opcode/profile/wire/kernel整包撤回；不扩大为全ISA规则队列 |
| 完整GPR最终核验替代 | 必要语义通过、完整方向无大幅收益，新增Q请求/有序核验/诊断API撤回；不继续窗口、游标或interval调参 |
| actual direct-narrow结果目标合同 | 成功selector输出真实替换目标，原fixed核验消费；独立构建/最少语义通过，只是正确性合同澄清，无提速结论 |
| 实际位置生产与消费者复用 | 第78完整25文件包必要语义通过，唯一完整方向无大幅收益后撤回；停止owner/source/bitmap/分组存储继续调参，见5.3.9 |
| 同Store native载体与中立program/core接线 | 第79批28文件组件组、四目标和最少语义通过；normal未启用，完整质量政策、state与kernel读者仍待迁，无性能资格 |
| actual birth、共享machine live与完整初始/质量读者接线 | C3/6f7b相对C2改46文件，四目标及该版8cases47049断言通过；initial/GPR/FPR/width/controller/raw/region已有范围限定检查，normal及最终形态/state/check/MEM/kernel未闭合，依5.3.12继续 |
| C4同合同形态、implicit publication与最后读者 | 6f2f四目标/限定最少检查通过；actual consumer resolver、资源/handle、NativeReady/退休夹具与FPR独立proof已检查。生产编码分支已编译，完整normal与B类cohort/state/transport未贯通，无性能资格 |
| 唯一 IR 保至最终 Flush、原 true→false 重发、整组退役 | 3ef 已完成最低语义验证；不是下一待实现包，也未证明提速 |
| Block definition stream、Function use-end Word | 完整反馈无明显收益，新增实现已撤回；不继续同类调参 |
| IRStore 目录容量守卫 | 正确性修复保留；不能继续作为当前 CoreMark 慢的原因 |
| Known＋early的typed scalar直接构造 | 已合入共享默认builder，dde原guest commit例194断言通过；减少参数往返与普通commit解释，不减少N，不等于M1完成 |
| partial MEM/fixed完整Residence跟进 | 已合入且dde原case457断言通过；含真实压力下U32/whole REF双capture与fresh proof，不再列为外部未实现 |
| ordinary memory stage01～03 | 已合；所含lane/load与修复在9b原owner1861、后续33da原owner1989中获限定检查，不授全域memory或最终质量 |
| INC/DEC＋NOT、typed空事件 | 已进共享默认构造；be原guest/取消/helper/decode flags等独立例通过，少literal与少分派分账，不宣称M1完成或时间收益 |
| late actual-window coalescer | 已合；33da原fixed/controller/owner三例2741通过，保REF/shared writer和实际lease/窗口，原候选乘积访问尚在 |
| actual-input直接生产及旧native输入服务退役 | merge90～93已合，后续WB actual Cell1/窄Store Cell3与raw-only matcher修正已合；dd0四目标、owner2230/NarrowRead125通过。默认支持域不跑旧服务是路由事实，完整负载覆盖率/时间收益未建立 |
| WB固定读/version、RCpc最终位置与默认接管 | full-X实际固定读、version及Definition kernel保持；stage06已合并退stage05早hook/reader。merge99开原支持域默认、退临时trace并接DecodedBlocks；限定原例不授全P/MEM组合或端到端收益 |
| 同点publication、DirectSelect接管及逻辑立即数宽度修正 | 已合；9e95四目标及原owner2809断言通过，含完整两块Function正/反例和实际W宽度回归。保完整scope/prefix/raw独证；CoreMark进入运行期后仍失败，不授CRC/性能资格 |

停止结论针对已测实现及同类调参。不同假设可以重新设计，但必须解释旧服务如何退出，不能只更名重启。

## 2. 施工起点与现有证据

### 2.1 当前源码、开发计时和最终资格分别记录

| 身份 | 已知结果 | 不能继承的结论 |
| --- | --- | --- |
| **最新普通e0d6e7e8，native-ordinary-ready-build-02 / 四点r3** | 普通translator-only构建成功；完整四点均CRC0x4983、source/index独立前后不变、子进程/私有FEX服务关闭；Block 2066.78007/46.623802ms＝44.32886×，Function 1099.290002/47.296979ms＝23.24229×；[完整四点原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-development-direction-adapter-r3-20261101/prepared-native-ready-e0d6-r3/compile-gap-summary.json) | 各mode一对开发方向，SVM inclusive Translate与fresh FEX单initial-context CompileBlock累计边界不同；ordinary native覆盖unobserved，非最终质量/采用/完整生命周期 |
| **语义与基本执行37c811d1，native-publication-role-build/owner-01** | translator＋swift_ir_test两目标成功；原owner一例3162全部通过，原Block guest exit0/CRC0x4983；[37c8原owner](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-publication-role-owner-01/execution.json)、[原Block guest恢复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/one-guest-adapter-root-staging-fix-r2/runs/publication-role-startup-r1/execution.json) | 不等于e0d6重跑测试或当前95bd测试；fixture验证真实RA候选与binding差异、explicit Value严格拒绝、Whole实际copy及fresh proof，未授全域或最终质量 |
| **历史95bd38e9，第一轮unit/attempt归因** | 仅接12段现PerfStats2范围与少量成功路径计数；[归因接线](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/merge-124-native-phase-attribution/source.patch)、[归因版构建记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-phase-attribution-build-01/build.json) | 归因已完成，随后3625细分native准入；均为PROF2诊断，不转授ordinary/FEX计时或旧测试资格 |
| **3625724f，native准入归因已闭合** | GPR B695.636/F103.768ms，占native准入88.77%/89.41%；两mode均原CRC及关闭收口；[原归因](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/native-admission-3625-summary-01/summary.json) | 只锁定GPR整体，不能将695ms全部归给Get发现，更不转授A收益或ordinary覆盖 |
| **当前8cda2889，consumer侧actual-use核验A** | 两目标构建成功；三原case为2过1失败，3714断言中3713过；新增NarrowRead负例在真实绑定前提处失败，未到stale-count检查；[原构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-consumer-read-proof-build-01/build.json)、[原检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-consumer-read-proof-minimum-01/execution.json) | test-only 02仅外部冻结，未应用/构建/执行；无新的guest/ordinary/FEX或最终质量，不将两个阳性拼成整组通过 |
| 历史861ab086，native-pinned-w-build-02 / owner-01 | 两目标成功，原owner一例3024断言通过；Low32唯一VOID consumer角色修复已合 | 当时原Block guest仍exit1/无CRC；原失败与构建保原身份，后续修复见37c8/e0d6，不回填旧结果 |
| cee10355，native-logical-pstate-build/owner-01 | 两目标成功，原owner**一例2993**断言通过；实际S写入的defined/zero-CV及legacy Clear/Save合同已合；[原执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-logical-pstate-owner-01/execution.json) | 其guest的gpr-finish异常由后续VOID角色修复处理；不把本轮2993与a326三例合计混用 |
| a1ff4964，native-tso-pstate-build/owner-01 | 两目标成功，原owner一例2937断言通过；仅补普通StoreMemoryTSO的真实NZCV保持能力，fault/observer保持；[原执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-tso-pstate-owner-01/execution.json) | 未重跑四目标；不能扩为所有load/store/atomic，也不授完整guest或性能 |
| a3264f14，native-local-condition-build / canonical-condition-minimum-01 | 四目标translator/`swift_test`/`swift_ir_test`/`swift_cases_test`成功；三例共2993断言＝owner2910＋decode26＋flags57；完整窗口、成功同节点交接及incoming全mask已合；[原执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/canonical-condition-minimum-01/execution.json) | 三例2993不是owner一例；原guest仍失败，不能将旧c077/02草案当已采用版本 |
| 历史32e68ab3，memmove形态定位 | 2404个source/build输入；相对9e95只有临时compiler快照，translator构建成功；原guest exit1/无CRC，已观察窄SaveResult与LocalCondSet/If的不一致；[实际形态与终止](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/memshape-32e6-check-01/terminal.json) | 诊断构建不是四目标/整例重跑，不继承9e95断言；无普通完整compile、QoR或fresh FEX |
| **已验证语义9e959229，native-logical-width-build/owner-01** | 四Android目标成功；原owner一例2809断言全过；完整Function B末交接/live-out拒绝、最终身份与实际W宽度回归通过；[检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-logical-width-checkpoint.json) | 原CoreMark仍PageFatal/无CRC；此检查点写于形态定位前，之后根因见审查39，不回填其原记录 |
| 历史f8ed7a19，native-startup-repair-build/owner-01 | 四目标成功；原owner2346/2344/2失败；两处新增fixture问题及后续修正各保原身份 | 后修不将本轮失败改成成功；不是当前测试状态 |
| 历史dd0e8480，native-default-build/minimum-01 | 四Android目标和原owner2230/NarrowRead125，共2355断言通过；支持域默认true；[检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-default-checkpoint.json) | 仅属dd0与所列原例；不转授后续source的新断言、完整负载覆盖、最终质量或性能 |
| 历史dd0，四点方向r2已中止 | 首个Block CoreMark guest启动后exit1、无CRC；39 block units/886 IR，其余三点未执行；后续logcat定位IllegalCode/RIP`0x231136`；[原执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-development-direction-adapter-r2-20261101/prepared-native-default-dd0e8480-r2/execution.json)、[后续定位](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/original-failure-observation.json) | 无有效性能点；19.962138ms不是完整compile。定位记录保当时尚未确因状态，后续诊断与修复见审查38，不改原失败 |
| 历史cee0ce21，native-actual-input-build-04 / minimum-01 | 四目标成功；fixed457/controller294通过，owner2075/2071/4失败 | 后续actual物理Cell读口、raw-only matcher及合法control/fixture phase修复不回填旧结果；第36节保原审查时态 |
| 历史c6e53ce8，native-writeback-build-03 / owner-02 | 四目标成功；owner2027/2024/3失败，当时base Definition/pinned合同拒绝 | 后续实际固定读/version与JIT读取修复已合，旧失败仍保原source，不能充当当前根因 |
| 历史33da2b12，native-coalescer-build/minimum-01 | 四目标成功；fixed457、controller295、owner1989，共3个独立case/2741断言通过 | 只授该版受影响原例；不是当前source、完整guest或性能资格 |
| **9b4249b9，native-lane-m1-build-04 / owner-03** | 四目标成功，原owner整例1861通过；同值无机器copy的发表证明已修实际source可读性 | be的5例不搬到9b合成同source全套；outer publication witness与三位置/末端窗口保持 |
| **be235db1，native-lane-m1-build-02 / minimum-01** | 四目标成功；typed194、cancel17、helper890、controller295、decode INC/DEC/NOT flags1058五例/2454断言通过，owner另1745条中1744过、1失败 | 后修9b使owner通过，原失败仍保be身份；构造少分派与text变化不授compile收益 |
| 历史dde682d3，native-birth-memory-build/minimum-01 | 四目标成功；typed194、fixed457、controller295三例通过，owner1293/1291/2失败 | 后来的lane/load、基线保留及coalescer修复不回填本轮 |
| 历史ce4a22d9，control-join-build-17/fixed-final-minimum-01 | fixed457通过；ZeroTarget U32/EQ/no-overwrite的原准备、true/false重发、退休前缀通过；总owner1031中1030过、1失败 | 正例header读已结束virtual event的错误已定位修正；其余宽度/条件/overwrite不由该前缀授绿 |
| 历史c984ba2a，build-14/sparse-fixed-minimum-01 | 12条原用例11过/56167断言，public owner998中997过、1失败；fixed227为GPR/U64及Function REF双capture | 后续457新增partial MEM/完整Residence证据保独立版本；不能把两轮阳性拼成当前全套 |
| 历史7bcbf655/2266add5，build-15～16 | 四目标成功；7bcb owner仍998/997/1，2266仅构建。PC诊断将policy0定位为f3d0 | 后续已确认f3c0正例实际选中；不再列“候选未选中/affinity窗口”作未定位主因 |
| 历史781667c8，build-13/fixed-domain-minimum-01 | 四目标成功；新fixed正例在最终proof报UnsupportedForm，原owner亦红，其余限定阳性保原receipt | 后查fixture只ConvertAll而未实际realize publication；c984修fixture后227通过，不回填本轮，也不放宽产品proof |
| 历史53f747ad，build-11/minimum-06-owner-only | 真实静态ABI正例及畸形偏移拒绝已合；四目标成功，owner仍失败 | 真实ABI夹具修正未消除正例失败；原b6fd/53f的未区分PC诊断不能充当根因 |
| 历史b6fd91e6，build-10/minimum-05 | 四目标通过；8例/55860断言通过；public owner 994断言中993过、1失败 | 非整组全绿；实际Save执行SECTION属于红色总例，不拆授独立执行资格；无性能资格 |
| 历史C7/32222313，control-join-build-04 | token/实际writer、state/width、Save/fixed、A/B/C metadata与原owner恢复已整合；build-04 exit1，源码前后不变 | 当时没有成功的整组链接、设备执行或性能结果；不能继承395的602断言。该失败仍保原身份，当前状态见7.1 |
| 历史成功C7/39522227，lazy-build-02/owner-only minimum-02 | 四Android目标与原owner一例602断言通过；CondSetControl/Generic真写/显式数值读口与hard可选撤回已集成 | 85fb原SSA fixture失败及两个checked235断言保原身份；当时state/width仅外部语法通过，本地flags发表原点/metadata/默认替代和性能仍未闭合 |
| 历史C7/3bdd060c，control-memory-build-02/minimum-02 | 四Android目标通过；真实public owner一例492断言、原checked Switch一例230断言通过；Label/If控制引用、Carry闭合与普通TSO/Extend真实资源已接 | 仅受影响范围，非native18/checked22全套；无本版native guest/compile资格。CondSetControl外部封包，本地控制三类合流与metadata仍待闭合，默认gate关闭 |
| C7/13888c06，control-memory-build-01/minimum-01 | 四目标通过；public owner一例398断言中396过、2失败，三个checked用例413断言通过；原日志与namespace关闭记录保留 | 两个owner失败分别是原BOOL读取视图预期错误、fixture未开启原Carry能力；后修不回填本轮。413断言不转授3bdd；Switch修正后仅重跑受影响两例 |
| C7/caeacb81，历史numeric-build-05/min03 | 四Android目标通过；真实public owner一例282断言、原先失败checked17例2839断言通过；数值flags实际图/有限D/独立proof和原准备/编码相依组已接 | 非最新native18/checked22整套重跑；完整condition-only/control和普通TSO/Extend仍未接，默认gate关闭，无guest/compile资格 |
| C7/4aeccf38，numeric-build-03/min01～02 | 四目标通过；min01 native18例17过1失败、4667断言4666过，checked实际22例5过17失败、217断言200过；另min02补先前未执行的Function width1/46756及physical164721/1通过 | 原计划误写19个checked例，实际22；原失败、计数审计及续跑分别保留。后修只重跑受影响owner与失败17例，不合并成caeacb81全套通过 |
| C7/numeric-build-01～02失败 | 先是fixture经const Def修改，后是修正误调private EditInst；改用已有public AppendInst构造同样三个producer后build-03通过 | 测试API错误与产品失败分开，失败日志/source保留，不回填通过 |
| C6/2b3c7543，历史build-17/min09 | 四Android目标通过；仅受影响joint一例2985断言通过，原负→回滚→正事务完成 | 只修原fixture对真实unchanged压力MEM key的请求与逐字段检查，未改产品/policy/压力；没有当前整套18例或性能资格 |
| C6/05bc15c7，历史build-16/min08 | 四Android目标通过；native18例17过1失败、1958断言1957通过；另9例/635、1例/46756及physical 164721检查/1注入通过 | 当时retained_rows为0：Stage只统计本次requests，原fixture未请求不变压力MEM正行；原失败保留，后来joint通过不回填此轮 |
| C6/3d86237e，build-14/min07 | 四目标通过；MEM、单一native入口宽度服务及新夹具已合；native18例15过3失败，其余三命令通过 | StoreUniform不是受支持的fixed-home consumer；x24不属于fixed-home bank；NarrowRead仍有preserved Get capture。保原失败，不把这些夹具前提错误归成新policy要求 |
| C6/53c4947a，build-13/min06 | 四目标通过；native17例16过1失败，其余三命令通过；Get32两home的capture/W-copy与后续proof/Ready闭合 | joint的普通发表明确定位为Form/point41、SetHostGPR源40、Whole MEM；后续MEM修复不回填本轮结果 |
| C6/build-12、build-15失败 | 分别为测试诊断变量重名和测试调用遗漏backend命名空间；后续build-13/16修正 | 失败日志、源码和原receipt保留，不能写成production算法检查通过 |
| C6/f2e5a719，历史build-11/min05 | 四目标通过；native17例15过2失败、1599断言1597通过；真实Block/完整Function组准备/编码例通过 | 当时joint缺domain/site、Get capture预期错误、其余3命令未执行；原时态保留，不继续当当前断点 |
| C6/4ad8789e、b55c7d75，build-08～10失败 | 新测试头文件引入同名Operand，先后限定失败；build-10误重跑同一b55源码，未提供新结论。最终显式ir::Operand后build-11通过 | 原失败、相同源码重复构建和日志独立保留，不能回填为成功或称三次都验证了新算法 |
| C6/63fcbc7c，历史未构建测试WIP | 当时较33e1仅改native测试：真实SelectZero→To64请求Capture、Get32 observer要求W-copy | 原快照无构建/运行资格；相关实现与后续结果按f2e5独立记录 |
| C6/33e1dc33，历史build-07 | 四Android目标；Get32静态B、全部相关输入读者及真实fixed/preserved producer已合入；native16例15过1失败、1897断言1896通过 | 通过断言含失败例前缀；当时joint停在真实width/capture请求为空，其余3命令未执行，不回填后来结果 |
| C6/a1ddfde8，历史build-03 | 四目标通过、native15例11过4失败 | 当时的initial mask与pair fixture失败保原身份；后续修正和通过数不回填，见复核第22、23节 |
| **最近完成整组最少检查C5/329cb954 / main bcd6a8d9** | 较C4改40源文件；四Android目标；本版11 cases / 47428断言，physical 164721检查 / 1注入 | state例37断言为reused=false借用/窗口检查，不覆盖完整native fold/observer；无生产NativeReady执行、compile/QoR/PMU/FEX或最终采用资格 |
| C5/351bcb03，前一构建/执行 | 四目标构建通过；14 native中13例通过、state例因实际pool在描述后配置而失败；共1018断言、1017通过含失败例前缀 | 13阳性保本身份，不把1017全归阳性或转给329；已修fixture顺序，保原FlagsRegsEnabled政策与pool完整性检查 |
| C5/85fb7bd4，历史build-03失败 | 较7d快照更新36源文件；当时build-03 exit1、源码前后不变 | 新增fixture调用受保护编辑接口未通过编译；原失败保留，后续成功独立归351b/329 |
| C5/7d621890，前一文档快照 | 当时相对C4只改6文件：PSHUFD纯能力保形与未接生产的native U32 primitive reader；当时未构建 | 历史WIP身份，不将后续源码、失败或通过结果回填给它 |
| C4/6f2f2dfd / main495bf4c6，历史限定检查 | 较C3改26文件；四Android目标，本版1native整合例170＋9checked例635＝10cases805断言；physical-consumption164721检查/1注入 | 限定same-contract形态/implicit publication、FPR proof/retry与resolver/资源/NativeReady退休夹具；编码分支仅获构建，完整cohort/state/transport及normal未完成，无新CoreMark/compile/QoR/PMU/FEX |
| C4文档轮1fa59a93，未构建快照 | 当时较C3改12文件，三组件冻结 | 仅文档轮WIP身份；后续按6f2f独立记录，不把当前结果回填当时组件 |
| **前一已检查C3/6f7b18b2 / main1b1e0466，第81批** | 四Android目标；本版1native完整质量/retry例70断言＋6既有例223断言＋1Function width例46756断言，共8cases47049断言 | normal未启；无本版其它11native、CoreMark、lifetime、compile/QoR/PMU/FEX结果；前版阳性保原身份，完整目标未完成 |
| f636296b，前轮首轮构建失败 | 2399个现存source/build输入；相对23dc改40文件、相对C2改46文件；initial、GPR/FPR/width、原controller及raw/region接线见5.3.12 | program.cpp两处u32窄化初始化编译错误，exit1、构建期间源码未变；无本版新ELF、运行或性能结果，当时review必修未修；后续修复/通过按6f7b独立记录，新normal仍关闭 |
| 23dc79d9，前轮C3文档快照 | 相对C2改20文件，未构建；当时initial/FPR producer仅声明、recipe字段未齐 | 仅保前轮WIP身份；这些断口已在f636源码继续接线，不能继续作为当前缺项，也不产生通过资格 |
| **94cb29e9 / maind9a92ac9，第80批C2，前一已检查基线** | 相对cf79改11文件，累计31文件；四Android目标；10native/614＋7既有pair/1439＋6既有eviction/51 | 共23cases2104断言；normal未启用，完整质量/state/transport/kernel未迁完，无本版CoreMark、compile方向、PMU/FEX或最终采用资格；不继承cf79的lifetime结果 |
| **cf79edbc / main4bed419c，第79批同Store首组、第80批before** | 28个source/build文件；四Android目标构建；6native cases293断言＋6既有cases338断言；physical164721检查/1项注入 | 原组件与core接线资格；不转为当前94cb生命周期、运行或性能资格 |
| **495887ca / main120e58c2，第79批before，生产精确恢复a10** | 四目标；三生产ELF等a10，仅既有Function fixture修正；独立1case28断言通过 | 原a10生产CRC/physical及下行before计时保原ELF身份；不继承候选861资格，无fresh FEX/最终采用资格 |
| **861af9f6 / mainb9947fdb，整包已撤回** | 四目标、14cases979断言、B/F CRC4983；唯一方向B731.729→731.534ms、F804.854→833.553ms | 各侧各mode一点，工作量略异，无大幅收益；候选25file和所有失败/通过/四点保留，不继续位图/source表调参 |
| c0e22c5b / main b95e83ae，第五组 | 四目标构建；晚write/width发现消费实际成员，fixed服务消费稀疏声明；已有检查前三命令通过 | Function collectors旧断言失败，before88ee同例同断言也失败；后续检查及CRC未执行，不能记为整组通过 |
| **88ee2548 / main dec41b10，最近必要检查完整通过** | 第四组：typed source五角色共享尾、动态anchor组合与matched alias/use交付；四目标构建、13cases33768断言、CoreMark B/F CRC4983 | 只支持该源码身份的必要语义；无compile/QoR/PMU/fresh FEX或最终采用资格 |
| 8b183700 / main ebf1d558，第三组 | 四目标构建、15cases1434断言、CoreMark B/F CRC4983；真实reload唯一owner及全部读写者迁移 | 前一相依组；无该版性能测量，不转资格给后续源码 |
| f47090ea / main d9902ea0，第78批第二组 | 四目标构建、12cases854断言、CoreMark B/F CRC4983；actual Map/关系/分组exact-use及部分真实消费者 | 前一相依组，无该版性能测量；不转资格给8b |
| 58466977 / main c7003854，第78批首组 | 四目标构建、10cases772断言、CoreMark B/F CRC4983；实际位置目录与首批消费者 | 只是同一在建算法的前一组，不是独立性能候选；结果不转给f470 |
| a10c7d3f / main120e58c2，本包before | 原f7e6生产查询/核验恢复，只保留6文件的actual direct-narrow result-home输出与fixed消费者合同；四目标及6cases303断言、原physical/CRC已独立通过 | 第78当轮before B731.728826/F804.853740ms，各mode一点；无fresh FEX/统计/最终采用资格，不继承22b方向 |
| 22b18ead / main3d626b60，已撤回 | 四目标构建；9cases962断言、physical164721checks/1注入、CoreMarkB/F CRC4983；B714.961→730.337ms、F815.108→818.285ms | 各side/mode一点，无大幅收益；F工作量相同，B最终IR/host略异，无统计/QoR/FEX资格 |
| 78f0083b / main 7c4750dc，已撤回 | 四目标构建；4cases1095断言、physical164725checks/1注入、CoreMark B/F CRC4983；B727.676→727.420ms、F824.533→805.382ms | 各side/mode一点、工作量不同，无大幅收益或最终质量/FEX资格；恢复f7e6不能继承候选结果 |
| **本轮before保留基线f7e6c3a8 / 原main e9229afe** | 四目标构建；除既有scratch fixture外源码逐文件精确等于d391，main/swift_test/lifetime SHA均相同；修正fixture通过2110断言 | 原相同ELF语义及方向保原身份引用；不是4f04结果转移，无当前fresh FEX或最终采用资格 |
| 4f04f843 / main ac435344，已撤回 | 8cases3011断言、physical-consumption164721checks/1注入、CoreMark B/F CRC4983；B742.573→748.575ms、F812.220→838.349ms | 各side/mode一点，少量工作量不同，无大幅收益；原先两次fixture失败保留，不转给当前代码 |
| d391c501 / main e9229afe | 四目标构建，既有10 cases444断言、构造快/慢各557224checks/11注入、physical-consumption164721checks/1注入，CoreMark B/F CRC4983；完整方向B726.782/F823.502ms | 最近已完成方向；各side/mode一点，无大幅效果、fresh FEX分母或最终质量/资源资格，不能转给4f04 |
| 3ef11319 / main e4e43f63 | 原重发/退役最低语义；本轮before完整compile B761.850/F858.283ms | 各mode一点；保原source身份，不继承当前字段/frame实现或新FEX资格 |
| 0641c577 / main 70d33e87 | 同 false-link codec 开发四点：B 745.145→749.408 ms，F 885.665→860.270 ms；无大幅改善 | 单点且 units/IR/host bytes 略异，不孤立归因 codec；无 fresh FEX 分母，不代表 3ef |
| da7962c6 / main 7ec0b7f2 | 最近一次同轮 fresh FEX 开发基线：B 791.933 / 47.477 ms，F 880.364 / 47.276 ms | 每 mode 一对，不是当前源码计时，也不是最终统计/QoR/资源通过 |
| 27311c76 / main 10289a6a | Word＋Store 组合 B 761.945→768.738 ms，F 872.672→884.470 ms；Word 撤回 | 不孤立归因 Word/Store，不转标签为保留版 |
| 固定 FEX | commit a156752f…＋Android patch 4ade908f…，普通 Loader 8632943c… / Server 9fe58a99… | 历史约47.5 ms不能充当下一候选的 fresh 分母 |

前轮文档核对冻结的C3整合源码为`f636296b1240992ffa9b3da327947028a9f2f17be124c5e700fbfabd8723c727`，2399个现存source/build输入；相对前轮23dc改40文件、相对C2改46文件。[冻结清单](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/frozen-build-01.json)、[完整source清单](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/source-frozen-build-01.json)。四owning目标的首轮统一构建已exit1，`program.cpp:600–601`将`unsigned long`表达式放入u32短缺量初始化，构建期间源码不变，未保存成功的新目标ELF；不是仍在运行，也不是测试失败。[构建终态](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/build-01/build.json)、[原始输出](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/build-01/stdout)。

最近已检查的C3源码为`6f7b18b215cab19cfb1f789e112f591465c9fe0645bd0eb740176136903101b2`，main为`1b1e0466229db396dce8d97e1693e72182ceef7f2917a2516b4c7e58c9d1fd79`。相对C2仍46文件、2399输入，四目标构建源码不变，该版最少检查8cases47049断言通过；完整source及相对C2的46文件补丁均保留。[C3检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/c3-checkpoint.json)、[四目标](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/build-04/build.json)、[本版三命令](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/minimum-semantics-c3-continuation-01/execution.json)。前版aea的native tag为12例11过1失败、730/731断言，不能转写“当前12例全过”；只补跑修正后受影响的一例及原未执行两条，不重跑11个阳性。[原失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/minimum-semantics-c3/execution.json)、[后续关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/minimum-semantics-c3-continuation-01/closure.json)。

前一C4文档轮源码冻结为`1fa59a936a43959f988be7b932a36c7b5aab9595c37d5bfbd75bca0045c14098`，仍2399个source/build输入、较C3改12个源文件；当时三组件停止写源、未统一构建/接生产调用；该轮仅修订文档，无新设备/性能结果。[本轮源码清单](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/plan-revision-20261031/source-before.json)、[接缝责任](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/interfaces.md)。

前轮23dc的未构建快照仍保原身份；当前recipe字段、initial/FPR producer和原controller不再是“仅声明”，但接线不等于通过。当时的五项必修见5.3.12.5，现已在本批修复；上述链接保留原文档轮时态，不代表后续没有源码构建。[本次核对与只读结论](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/docs-integration-review-20261031/review-findings.md)、[文档修订前身份](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/docs-integration-review-20261031/before.json)。


前一已检查的C2 source为`94cb29e941c4c77eaebd524e6893c59d57af9147400d5adfdf715a01ce1bad76`，main为`d9a92ac98ee12855e864f82ef4dcba4051882b70d02121a4e042aebd4287258a`。第80批四目标与三项最少正确性范围通过，两次自有设备namespace均关闭。首轮私有REF接口编译失败及pair清单3/实际7的审计失败保留；pair原命令本身exit0/1439断言通过，经真实源码roster核对，只补跑未执行的eviction，不重复通过项。[本组检查点与完整源码](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/checkpoint.json)、[统一构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/build-02/build.json)、[原始native/pair命令](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/minimum-semantics/execution.json)、[roster审计修正](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/minimum-semantics/roster-correction.json)、[仅剩余eviction](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/minimum-semantics-continuation/execution.json)。没有C2的CoreMark、compile方向、PMU、lifetime矩阵或fresh FEX；该组资格不转给C3。

第79批首组、第80批before为`cf79edbc3392983c2a0958e3b5d41c6a79498ce9e21e80b923d01f3d3434bc28` / main`4bed419c…`。[首组检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c1-checkpoint.json)保原四目标/最少语义及physical生命周期资格。C2新core已接GPR实际压力与next-use，但仍拒绝已提交、依赖语义的GPR/FPR/fixed等政策输入；C3源码已增加真实initial与quality入口并放开相应已绑定政策，本批已修已知边界并通过所述最少检查，仍不能称所有目标形态和最终准入已验证。core不能自动发现漏提交的width/flags/observer需求，目标完整粗准入仍须成立。CoreAllocated或组件NativeReady不证明全族质量/transport已迁完。

第79批before是`495887cac6153a3401ddc9b17776c369ba782c576b2a02c397955b0cb0a0b454` / main`120e58c2…`。该版生产源码及translator/swift_ir/lifetime三ELF逐字等a10，只有swift_test含既有fixture修正；原a10 CRC/physical证据按同一ELF原身份引用。[恢复资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/qualification.json)、[相同生产ELF核对](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/production-identity.json)、[撤回与精确恢复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/withdrawal.json)。下面各中途组保留当时结果；最近同轮fresh FEX分母仍只属于da，不代表当前94cb。

本包before为`a10c7d3f1273d8125a14f4ff54f1f787e750485e269c3d27112f5ab9f0cea3f0`，main为`120e58c2607c2bbca3788547d6eafb8a4d0d0b9e57608308dea98ee0004f5320`。[原独立资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/actual-result-retained/qualification.json)保持原版本身份。

保留基线全值为 `f7e6c3a8a46a8b62ac290b0d79a82ac98659b2c9bd6d4b766d9f8057e04c377f`。[恢复资格回执](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/shared-final-facts-20261031/restored-qualification.json)核对生产逐文件及三个未改动目标的字节身份；只有原scratch fixture改变并单独通过。共访候选的先前失败、修正后8case结果、四点方向及撤回均保留，不重标旧失败。

已撤回候选全值为`78f0083bbf1b31c1ec2705cd04d333cee15d0c8e760c78a30af3d98d0f3f67de`。[最终候选源码](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/final-qualified-candidate/full-from-f7e6.patch)、[必要语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/minimum-semantics-summary.json)、[完整方向](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/direction/accounting.json)、[撤回决定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/direction/decision.json)及[精确恢复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/withdrawal.json)分别保持真实身份。07ef/4daa/9837的构建失败、ff5f的schema失败、c959的夹具SIGSEGV及修正后78f结果均保留；恢复构建及ELF身份以当前回执为准。

d391的[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-direct-public-and-current-input-20261031-r2/build.json)、[最少语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/direct-public-arguments-20261031/minimum-semantics-summary.json)、[四点原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/direct-public-arguments-20261031/direction/accounting.json)分别保持原资格；Function该组units/IR/host bytes相同，Block略有不同，不孤立归因两个组件。原true→false与最后Flush退役继续沿用，最低见证未强制制造生产region分配失败或证明所有fallback/QoR路径。

### 2.2 已有证据的使用边界

- 旧 P0 合格 compile cycles 中，Complete 约17%、物理形成约12%、实际 emit 约8%–9%，Packet::Append 不足1%。不是当前毫秒，也不是可全删比例。
- 固定 FEX 的255个请求实际覆盖7123个 decoder blocks、7191个 IR CodeBlocks、29915次成功 guest lowering；RA 输入 code nodes 139954、backend 输入91970。与 SVM 的节点计数不在同一阶段，不能推导“少一个数量级 IR”或归一化每 IR 耗时。
- 普通点没有 fine PROF2；disabled PerfScope2 不在每次调用读取 TLS。日志/检查的全部独占成本未闭合，但已有证据不支持它们是数量级根因。
- VerifyHostRead 前缀查询、非单调有序插入等有条件性平方放大风险，尚无当前动态权重。没有输入规模实验，不能把差距称为已证指数复杂度。
- Store 每页错误倍增目录容量的缺陷已经修复。原失败、撤回和旧版本数据保持原身份。

依据见[差距审查](compile-gap-review-2026-10-31.md)、[证据附录](middle-pipeline-rebuild-evidence-2026-10-30.md)、[FEX 工作量](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-compile-work-census/analysis/report.md)。不重复执行已关闭的 P0、codec 或观察者试验。

旧P0尚未展开的`compile-service-other`本次仅用原qualified样本及已有符号离线归约，未新采样或改原三段结果。它是source123d历史PMU的余集，**不是PROF计时的other桶**。在原qualified compile分母内，decode/build约15.29%/19.67%、目标inputs/初始fixed约14.64%/12.01%、residual/编号约7.97%/10.60%、RA attempts/其检查约6.53%/8.42%、首次Facts约4.53%/3.62%；仍有4.37%/2.82% qualified未细分，全部unqualified继续排除。该证据只说明需覆盖多个真实服务，不换算当前毫秒、可删份额或FEX比值。[离线原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/qualified-other-work-review-123d-20261031/report.md)。

同批已保存记录的库leaf有限复核使用**整个qualified compile**分母：Block820样本/868765709 period，Function921样本/980295089 period；period为cpu-cycles采样权重。它与上述Other服务/文件归属是交叉投影，不能相加。

| 历史库leaf范围 | Block | Function | 解释边界 |
| --- | ---: | ---: | --- |
| allocator/Scudo/new-delete实际leaf | 4.697% | 6.105% | 库leaf自身，不是所有heap相关工作 |
| allocator-associated并集 | 6.923% | 9.050% | 已含上一行；另含原前缀明确属于allocator的atomic/lock，不重复相加 |
| 内存copy/set实际leaf | 2.462% | 2.632% | 包含必要复制/清零，不全是临时容器 |
| associated＋copy/set＋库初始化/销毁及生命周期宽包络 | 12.217% | 13.218% | 已含前述范围，既非可删份额，也非全部heap成本上界 |

完整已识别库leaf30.942%/34.694%还包含查找、排序、结构维护、atomic和必要cache flush，不能统称malloc/free。未知及项目self中的潜在heap工作未被冒认；nearest project caller仅作归属，不是caller self。当前没有集中到一个可退役per-block owner的证据，**不以PMR、空backing复用或缓存预算单独承诺根治**，也不为这条已否定假设追加采样。[原报告](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p0-library-leaf-review-20261031/report.md)。

### 2.3 M0已执行：当前同轮对照与实际优先级

本节“当前同轮”指 M0 当时的 da/main7ec，后续源码不能继承。普通点为 PROF=1、无 PROF2；FEX 为私有固定服务、fresh Loader 和独立 data/SHM。原 argv、CRC4983、产物身份和结束条件已核，自有运行场关闭。

| mode | da 完整 Translate ms | 同轮 fresh FEX CompileBlock ms | 开发比值 | 至2F仍须减少ms |
| --- | ---: | ---: | ---: | ---: |
| Block | 791.932693 | 47.477448 | 16.6802× | 696.977797 |
| Function | 880.364420 | 47.275781 | 18.6219× | 785.812858 |

另两个现成 PROF2 诊断中，outer DCE 2.699/1.822 ms、flags 13.826/29.444 ms、actual assignment 18.483/27.122 ms、sort 0.822/0.353 ms。flags 包含内部 DCE；regalloc_total 171.328/169.083 ms 与 AllocateBlock/Function 52.301/58.031 ms 的差额还包括 Initialize、PlanBuilder、Describe/Finish、初始 fixed；codegen_complete 236.431/262.863 ms 还含 FVI。**这些嵌套诊断不能相加、与 ordinary 相减，或直接作为可省时间。**

修订后的选型是：小 DCE/flags、sort/assignment 和窗口查询均不能单独担当主线；**M1 覆盖构造到公共解释，M2/M3 合作替换目标输入到最终交付的决策链，M4 覆盖请求/发布/结束。** M0 不重做；仅在缺失信息会改变具体算法时补最少归因，不把普通性能采样提前为每个接口片段的门。

[M0 原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m0-current-fex-baseline-20261031/M0-result.md)、[ordinary 收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m0-current-fex-baseline-20261031/closure.json)、[诊断](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m0-residual-diagnostic-20261031/execution.json)。

## 3. 对照 FEX：缩短决策链，保留自己的合同

只对照固定生产版本。FEX 并非“单遍、无检查”，也不能用其块内 RA 替代 SVM Function。

| 固定 FEX 的实际行为 | SVM 的对应改法 | 必须保留的边界 |
| --- | --- | --- |
| IREmitter/dispatcher 使用已知 host immediate、address、固定银行语义 | 公共纯能力在构造时选合法语义形态，减少中间节点 | frontend 不引用 ARM64 form、home 或 VIXL；Unknown 有真正 generic 分支 |
| 默认优化主要为 x87 与 DeadFlagCalculationElimination，RA 另加 | 少产生需要反复优化的语义；按真实变化处理 residual | 不是通用 DCE 的直接对照，不能删除 SVM 的未来用途/CFG/恢复分析 |
| 每个 CodeBlock 反向 kill/SRA affinity，正向受约束分配并处理 fill/spill/copy/merge | 让目标约束、实际位置和依赖位置的选择在同一服务落实；减少晚阶段反投影 | Block/Function 分开；point/whole/REF/MEM 不可缩成单个 node.Reg |
| RA 将 data args 改成 physical immediate，backend GetReg 直接取 class/reg | 同一程序的 virtual operand 在最后可失败边界后改为实际位置；kernel 直接读稳定 physical operand | 先迁完全部语义读者；true/false 共读 PhysicalReady；不复制完整 Selection/operand 图，不拿块内 RA 替代 Function |
| 首次真实 spill 才准备额外 next-use 状态 | 困难域按真实需求分配/初始化，必要状态可有界复用 | FEX 仍有 N 初始化；不能靠多 spill 或遗漏未来用途省编译 |
| 线程级 builder/backend/pass objects、arena/pool 复用 | 缩减实际请求初始化和最后清理；只复用空 backing | 不改变两种 RA scope，不把延后释放算作已释放 |

FEX 每个 guest instruction 都有寄存器缓存 flush，不能虚构任意跨 guest 的状态延迟作为对照。其约47 ms是整个 CompileBlock累计，含decode/passes/RA/emit/IR交还/SMC/entry发布；线程/编译器初始化、外层lookup和server启停不在内。保留端点差异，不改系数补差。

源码与计时依据：[固定 FEX 复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)、[PassManager](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-compile-work-census/before/FEXCore/Source/Interface/IR/PassManager.cpp)、[RA](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-compile-work-census/before/FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp)。

## 4. 重构后的目标流程

### 4.1 一条语义链，两种分配算法

```text
runtime 绑定不可变环境与实际请求/group
  → decode + typed build：能直接成立的公共形态一次提交
  → 必要 residual / state / flags / DCE / hoist 反馈
  → 最终编号/seal、完整 semantic SSA Finish；一次目标形态/资源规范化
  → 关闭全部 semantic 读者；原 CFG/terminal/entry/hoist 元数据转交
  → 同一 Store：Semantic → Consuming → BackendVirtual
       ├─ Block：实际 machine uses/defs 的局部 live/kill/affinity
       └─ Function：同一原 CFG 上的跨块 machine live/entry/join/loop
       → 原 Writer 分配、有限候选准入和必要修复/重试
       → 最后位置编辑；实际 state/entry 求解及剩余 form 兑现
       → 独立 fixed/width/actual writes/observer/scratch 核验
       → 同一 operand 原位物理化，PhysicalReady（此前准备所有容量）
  → PhysicalReady 直接供现有 kernel 读取
       → 原 true，实际 region 失败才重建 context 发 false
          （共读稳定程序，不重做 RA、选择、绑定或资源描述）
       → 最终 Flush → 持久 metadata 发布、最后 lease/资源结束
    public/retained/不具备消费权：事前选择原 checked 语义入口
    Unknown/EarlyOff：保真实 generic build；实际绑定后再合法 lowering
  → 最后 owner 的真实释放或有界复用
```

本图描述当前选用实现，不规定唯一承载方式或每个箭头一遍程序。稳定的source/use/def/root/birth变化必须先于首份machine live；真正依赖位置的候选先选择完整待履约合同，再让原RA落实，晚变更维护其live/fixed/pressure/state或进行有界重建。提前销毁semantic与消除重复算法是两个决定，均须说明必要读者及净成本；不为更早结束对象单独重开全族迁移。

完整semantic SSA Finish仍读先前定义和分支，先完成最后semantic读者才能转换。但对象寿命边界不代替机器范围边界：稳定use/def/root/birth变化先于首份machine live；位置依赖候选先选完整合同，晚改变维护或有界重建相关范围。不能仅凭尚未到birth/emit，沿已收集的旧区间继续分配。

### 4.2 一个决定的写者、最后消费者和失效

| 决定 | 现有唯一承载者 | 最早决定 / 最后消费者 | 实际失效 |
| --- | --- | --- | --- |
| 语义与用途 | Inst/Arg、uses、RewriteContext | 构造/合法编辑；完整SSA及最后图匹配结束后关闭 | payload、root、顺序或成员改变 |
| semantic SSA/CFG 与 machine live | 原 Facts/CFG 与原 allocation 范围数据，各司其职 | SSA在转换前完成；machine ends来自选中uses/defs，供相应范围RA | 语义/顺序改变；或机器use/def/根/最早写点改变，不混用旧ends |
| 后端程序/operand/effects | 原 IRStore、PhysicalStream/Prepared | semantic closure后接管原槽；virtual供RA，physical供核验/编码/重发 | 虚拟阶段的真实选择/事务编辑；Ready后不可变 |
| 静态目标需求 | 同源 form schema；原 AllocationInputs 保真实稀疏约束 | 一次目标规范化直接交付；跨attempt复用 | 输入/环境或form合同改变，不是每个retry自动失效 |
| 实际 Map/REF/point/whole、spill/preservation | Allocation / AllocationWriter | 对应 RA scope 信息齐备；状态求解及最终 binder | 真实位置编辑/attempt/repair |
| guest-copy/version/home-width/fault | RegisterStatePreparation / state | 按实际顺序消费真实位置与写事件；选择和恢复核验 | 相关位置/写/观察边界改变 |
| 成功 form/events/transport | 原节点，转换后为 NativeInst；原 Selection 只保必要稀疏候选及unit/B信息 | 唯一成功决定直接在程序中兑现，相关recipe移入后结束；最终检查/编码 | 相关位置、状态、coverage或attempt改变，事务回滚 |
| 跨重发输入与最后寿命 | Prepared::Storage / ExecutionAccess | 原请求粗准入；最后成功 Flush 或失败收口 | 整请求放弃/明确退役 |
| offsets / entry / fault / link / SMC | emitter / 原 code-object owner | 实际编码和发布；持久消费者 | image/布局变化，重发须重建 offsets |

不再加统一 DecisionManager、certificate graph 或完整 decoded-args cache。表中不同事实可以共访，不能合成一个含糊的 home。生产核验读取真实输出，独立 audit 保独立参考；“validated/owned/known”位不能自证。

### 4.3 decode/build 与 pass 的分工

| 工作 | decode/build 同时完成 | 必须等后续信息 |
| --- | --- | --- |
| parse/PC/bytes | 一次 parse、原 extent、owned bytes、提交/取消边界 | CFG 最终次序、host layout/offset |
| 纯值 | 常量、恒等、当前浅形态、合法 immediate；命中只提交结果，miss 一次 generic | 首次深解释、CSE、未来 uses、改写后的机会 |
| 宽度 | 定义本身可证的 narrow/carrier、已有事实的纯 transfer | per-use view、深 facts、实际 W/X、whole/point preserve |
| memory | 完整 EA、access/result/extend、合法 TSO 语义形态 | alias/writeback、atomic、实际 clobber/reload/scratch、fault 恢复 |
| guest state | byte/partial/full forwarding、已知覆盖和已有 deferred | observer-aware DSE/sink、join/loop、实际恢复位置 |
| flags | 输出意图、安全局部延迟、已有有限需求窗口；分别表达canonical发表、raw NZCV资格与PF named-result；dead-edge marker不授raw资格 | CFG live与完整producer→condition窗口、required全mask、partial/carry及fault/observer；实际PSTATE来源与最后消费窗口由后端核 |
| uses/control | 真实 counts/effects、terminal/implicit roots | 反向用途、CFG/SSA/live、hoist/entry 与最终编号 |
| target | 纯 capability、语义形态支持和 carrier | 固定位置、真实 writes、transport、机器码和特殊资源 |

Known 不授予消费权，也不证明 SSA/物理分配正确。EarlyOff 关闭 builder 能力但目标仍可 Known；Unknown 保 generic 构造及支持的消费者/明确合法化。详见[目标设计](target-aware-middle-pipeline-design-2026-10-28.md)。

## 5. 按生产工作拆分的实施主线

### 5.1 M0：基线和选型已完成

复用第2节结果，不再开一轮相同基线/PMU。每个新包先给出旧工作、覆盖域、替代算法、剩余依赖、失败及净新增；仅补会改变选型的缺口。日志、clock、CRC、源码和 ELF 身份保持各自资格。

### 5.2 M1：构造、公共首次解释与反馈

**这是独立主线，不能缩成DCE/flags或最终Facts共访。** da的decode＋RPO＋opt为213.391/261.921ms，其中decode/build本身为111.158/165.636ms；若它保持不变，已经超过当时约95ms的2F总目标。以下构造、公共解释和表示改造必须具体到生产者与消费者；Function部分Facts还在other。

#### 5.2.1 直接提交，减少中间节点及其后续工作

在现 builder/rules 内按完整语义族改造：纯规则在 append 前决定已有值、最终公共形态或 generic；一次提交完整参数、counts/owner/uses。优先替代仍经通用展开再折叠/识别的真实路径，不能把现有 EA、load-extend、birth、deferred 又算收益，也不能把 AfterAppend 简化写成“节点没出生”。

```text
decoded guest + 当前真实构造事实 + 可选纯 target 能力
  → 无副作用选择
  → 支持：只提交最终公共语义
    不支持/EarlyOff/Unknown：一次 generic
  → 原输出/flags/observer 收尾
```

每族列出旧节点/边、原解释/删除次数、实际支持域和新增选择成本；仅命中率高不够。完整 Imm、per-use view、访存宽度、异常次序和 flags 保留。append/访存/helper 后不重放 generic，不用“birth 完成”标记跳过未来用途、深事实或编辑后的 residual。C2 旧有限窗口已经负反馈停止，不用扩窗口填主线。

<a id="5211-当前完整候选shlshrsar直接公共语义"></a>

##### 5.2.1.1 已否决：单独紧凑化SHL/SHR/SAR

78f候选在Known＋early＋canonical分支，用一个公共ScalarShift和直接ConditionalShift输出替换count/CF/结果/NZP/OF及必要零计数guard展开。保原Src/Dst/fault、已有立即数0捷径及Unknown/EarlyOff/非canonical展开；公共schema、may-preserve flags、解释器、raw/Current同源helper、Definition结果role、scratch和retained profile均接入。它确实减少命中域中间节点及相应维护，不是换桶；没有替代原Facts/Describe/Finish、两scope RA或state/Complete。

修正实际构建/构造和夹具问题后，3个既有case执行4次、共1095断言、physical-consumption164725checks/1注入、CoreMark B/F CRC4983通过；这不是所有flags-only/value-only、最终生成/运行或资源资格。完整方向如下：

| 范围 | d391 before | 78f after | 单点变化 |
| --- | ---: | ---: | ---: |
| Block | 727.675579ms | 727.419967ms | −0.04% |
| Function | 824.532966ms | 805.382119ms | −2.32% |

各side/mode一点。Block请求2414→2419、最终IR59550→59077、host字节242744→244668；Function请求438→435、decoded blocks4599→4534、最终IR69909→67832、host字节347300→343944。不能把工作量不同的单点作统计提速、同族独占收益、出生节点覆盖率或生成质量判断。没有大幅方向支持新增opcode/profile/wire及编码维护，整包29文件已精确撤回f7e6，`ir/shift.h`删除，原193个opcode和原profile恢复；完整最终补丁、源码及全部失败保留。

**停止该族count/kernel/scratch/getter调参，也不依此铺全ISA专用规则。** M1需改能覆盖更多真实构造/首次服务的生产消费算法，M2/M3仍须改变剩余大决定链。未来只有作为实质不同、能退出主要保留服务的完整算法组成部分才重新考虑，不以已有正确实现为理由长期保留未获主要收益支持的复杂分支。[完整决定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compact-scalar-shift-20261031/direction/decision.json)。

<a id="522-同一次最终-typed-解释交付公共事实与稳定目标输入"></a>

#### 5.2.2 已否决：单独融合首次Facts与目标描述

候选4f04完整实现了内部Optimize延后首次Facts、原目标owner提前就绪、公共窄visitor共用raw/resolved、完整SSA成功后补齐真实需求并交RA。普通完整DescribeBlock及第二次target参数遍历确实退出，未跳过schema/SSA或改两种RA范围。修正既有fixture后8case3011断言及必要语义通过。

完整四点为B742.572591→748.574803ms（+0.81%）、F812.219636→838.349437ms（+3.22%）；各side/mode一点且少量工作量不同，不作统计回退/单helper归因。Block opt约−28.6ms而allocation约+34.3ms；Function other约−31.5ms而allocation约+40.5ms，首次Facts实际换桶，**不是删除这些服务**。

剩余GPR窗口、FPR条件全量扫描、完整consumer solver、rich scratch和Function局部emission/两pool闭合仍执行；新增回调、B级Finish ranges、真实候选、较早first-consumer行、临时索引恢复与归并。其成本和维护复杂度没有得到大幅收益支持，整包生产及接口已撤回。当前恢复原eager Facts→原Describe/Finish，不保FinalFactsVisitor、defer flag、ConsumerPrefix或scratch草稿旁路。

**停止独立共访、回调、候选容量及临时索引调参。** 未来若重用字段，必须嵌入会真正消除目标需求/实际位置/state/选择重复服务的完整算法，先列实际消费者和旧工作退出；不能再次把“外层一次”当收益依据。旧实现和所有结果保存在[撤回决定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/shared-final-facts-20261031/direction/decision.json)及完整源码包中。

优化前SupportsIR与优化后schema/SSA继续针对不同输入状态；Known/fresh/seal不授予豁免，EarlyOff/Unknown保原分支。真实出生/计数、唯一用途、ValueWorklist首次roster/CSE/深事实、state/flags/hoist和公共Facts仍须在M1列净工作，不认领“双图删除”或“首次工作全省”。

DCE/flags的以下设计仍有用，但只作为本线的条件性配套；不以它们包办构造成本。

#### 5.2.3 条件性配套：独立 Block 的 DCE

当前 [dce.cpp](../source/runtime/compile/passes/dce.cpp) 中，`QueueDead` 对 single_block 直接返回；`AfterChange`/erase/root 变化设置 `block_dead_pending`；下一次 `CollectDead` 反扫整个 Block。Function 已采用 seed＋queue，不能重复认领 Function 的收益。

替代算法复用现有稳定 handle、真实 counts、owner 和编辑通知：

```text
第一次 CollectDead：刷新真实 roots，首轮扫描 seed 可删除定义
每次改写/删除：仅把旧输入、受影响定义和失去 root 的定义排队
drain：先验证 session/alive/member，再按原 Removable 检查并删除
       删除触发真实输入 counts/flags/value 通知，继续排队
后续无变化：不重扫 Block；异常中断后按原语义重建剩余活图的队列
```

必须覆盖 counted 与 implicit roots、terminal 变化、重复 Params、raw/manual/mixed、已删除条目、全块放弃和异常。还须迁移 Append/Insert/adopt/remove 的出生与归属通知，包括 flags 清理中新建 ClearFlags；独立 Value::UnUse 等直接降计数写口要通知原队列，或明确回退到受检全扫。不能只删除 single_block 的 return。普通 managed 域不新增身份表；raw 冷域可以保留受检 membership。通过真实 Destroy/Erase 反馈，不在 RewriteContext 外另建计数器。闭合后删除旧 single_block 全扫分支及仅为它存在的 pending 状态，首轮 O(N＋E)工作保留。

#### 5.2.4 条件性配套：Function flags 的增量固定点

当前 [flags.cpp](../source/runtime/compile/passes/flags.cpp) 每轮仅用 `HasFlagChanges` 决定要不要再跑，随后仍对全部 Function `ComputeLiveIn`、全部 blocks `CleanBlockFlags`；[flag_flow.cpp](../source/runtime/compile/analysis/flag_flow.cpp) 每次清全 states、重算全部 block transfer、全图迭代。真实 dirty blocks 已在 RewriteContext 中记录，但未限制求解域。

替代算法保首次完整计算，在同一RewriteContext中保活原FlagFlow的B级transfer/live-in；不在每次EliminateFlags入口丢掉再建立。后续只重算dirty transfer和受影响的需求闭包：

```text
首次：原 CFG、全部 block transfer、完整最小固定点和原首轮清理
编辑：记录 transfer 改变块；root/terminal/label/CFG 改变覆盖真实依赖
求解：取 dirty 的反向可达前驱闭包，将闭包内需求置 bottom
      闭包外 successor 需求作为不变边界，重求闭包内最小固定点
清理：重访相关语义/uses/root/顺序有修改的dirty块 ∪ live_out改变块
      transfer相等只停止向无关前驱传播，不取消dirty块自己的清理
DCE/value 再引起修改则进入同一闭环；新 hoist epoch 不借旧结果
```

**需求会下降，不能简单从旧 live_in 继续单调传播。** 自循环中的已删 flags reader 可能留下自支撑旧需求；反向闭包 bottom 重解或等价的完整 SCC 方案必须处理。沿 flags 自己的 terminal/局部 label/外部观察关系取闭包，不能直接套 value use-end 的图。逻辑 call-return、未知跳转、partial/carry、BindLabel/Goto/NotGoto、MayFault/helper/AdvancePC、顺序变化、branch-only 首轮及 dirty 通知不完整时的受检回退保留。固定拓扑的首次分析不省，退化到全图时仍计真实成本。

dirty清理范围还包括真实use计数改变的producer所属块，以及implicit-root增删引用的定义所属块；consumer删除可跨块影响producer。即使聚合transfer/live_out相等，ClearFlags/branch-only仍可能因uses/roots变化产生新机会，不能跳过。复用原关系和通知，不为此建立第二张依赖图。

#### 5.2.5 反馈改造的退出清单

| 退出 | 保留 | 新增/风险 |
| --- | --- | --- |
| Block 每次反馈的完整反扫；Function 每次 flags 修改后的全 N transfer 重算和无关块清理 | 首次 seed、真实 uses/roots、最小固定点、flags/value/DCE 反馈和优化质量 | 队列与去重、dirty closure、下降重解、raw 回退、异常恢复和容量结束 |

沿原 `RewriteContext`、`ValueWorklist`、`FlagFlow`、workspace 实施；不增加通用 pass manager/dirty graph。第一轮/小块可能没有收益甚至增加固定成本，必须看整个 residual 和完整 compile。覆盖与收益小就停止扩展，不推广为“全 pass 已增量”。

#### 5.2.6 公共参数表示服从整条提交与读取合同

**第74批已实施这一表示替代：** 五槽直接语义字段贯通构造/编辑/uses/Facts/target，packet、offset、本地Value解码和公共overflow服务退出；birth不再构造字段image，只预检count delta后从原参数提交。same-origin private tag只允许用途图在held Store内读稳定prefix，不授予producer Inst读取权。semantic Inst104B、managed Slot128B，真实physical48B格式另保。完整方向仅约4%变化；保维护成果，不继续字段/容量调参，不将此写成M1大项完成。

后续以这套实际字段为基础，维持同一schema下构造、编辑、用途枚举、最终检查与target读取的完整合同；位置、A64 form和机器码不进入公共IR。Params/Lambda/foreign等真实复杂数据保受检escape，不新建DecodedIR、全N字段缓存或验证证书。

第74批已经迁完builder→uses/rewrite→Facts→target及其他实际读写者，旧packet不再作为待迁对象。完整Imm、per-use view、numeric/raw-slot、multiplicity、编辑失效和异常原子性继续保留，Known/EarlyOff/Unknown共用。下一步是减少真实出生与首次服务，而非再次换槽、加getter或恢复packed层。常见固定arity有界，大Params按实际长度付费；不能关ASSERT或用owned/seal位代替未满足的检查。

#### 5.2.7 当前可证的构造改进与未闭合的大项

**少出生、少首次解释和跳过空事件分别记账。** 普通五族ALU的IntegerOperand、全宽GPR/flags延迟发表、typed birth/EA与AdvancePC合并已有实现。新增有限包已经合入共享默认builder；广泛M1减工仍未完成，不能把两个局部改动当作数量级方案。

| 生产工作 / 当前状态 | 真正退出的工作 | 仍保留与新增费用 |
| --- | --- | --- |
| typed scalar直接构造已合，be原例194通过 | Known＋early＋真实guest scope先形成五族ALU最终operand或真实Neg，再append；少旧payload改写和普通guest commit参数往返 | N不减；PendingValue仍32B并保完整FlagsIntent。Original保同guest宽度变化、foreign/raw continuation及两个实际op读者；Unknown/EarlyOff保原路径 |
| INC/DEC＋普通NOT少literal已合，be原decode例1058通过 | admitted非locked32/64位INC/DEC用原IntegerOperand避免one的实际出生；普通NOT不再出生零用途all-ones | 原CF和其余flags保真；8/16位窄NZCV、locked AtomicRMW真实Value及fault顺序保留；miss仍LoadImm并多付原target查询。无总N/毫秒测量 |
| 已知opcode的typed append事件裁剪已合，be原取消17/helper890通过 | 唯一194条semantic effects定义静态投影；跳过不可能的pre/post事件和无规则fold调用 | 大AppendRawConstructed按参数形状共享；真实forwarding/barrier/flag observer、未完fold、raw/public动态入口与构造检查保持。N不减，header/template/text与I-cache成本仍需核 |

静态已知opcode与Known backend是不同知识。effects只有一个权威来源；GPR-before-flags的flush顺序、14类forwarding事件和3个实际flag observer保持，不能借静态事件裁剪改变Unknown/EarlyOff的target语义。

**已有链接后机器码证据，尚无时间证据。** dde→be的组合ELF差额为.text −50128B、.rodata 0、.eh_frame −1044B；同一差额包含phase/memory/lane等修复，不能归因给M1单包。具体纯OpMadd已无AfterAppend调用；另一个Unknown-flags TestNotZero仍为真实fold调用共享AfterAppend。这只说明检查到的静态代码形态，不证明执行频率、I-cache或DBT compile改善。证据见[ELF节与调用核对](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-lane-m1-code-size-01/evidence.json)。

be的五个独立绿色例共2454断言；同版owner另红，修source证明后的9b才获原owner1861整例通过。两轮不拼成同source六例。PSHUFD immediate保形已经落地；literal未来仍可能取得flags/pseudo/branch身份，继续沿原decoder/builder/rules/metadata写口收敛，不增literal interning、事件registry或每N状态目录。M4随真正退出的对象、边与读者删除触达。

证据：[M1独立原例](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-lane-m1-minimum-01/execution.json)、[9b原owner](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-lane-m1-owner-03/execution.json)、[构造与lane检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-lane-m1-checkpoint.json)。

**M1的下一候选仅完成静态审查，尚未选型。** 可审查普通值从出生纯规则到首次residual发现/roster、再到真实编辑反馈的整组重复工作；不能把birth已跑过当作跳过初轮的资格。现pipeline先做state/flags，ValueWorklist尚未存在时相关改写不会自动进入它的队列；首次roster又服务CSE、深事实、Users和DCE反馈。任何替代须接住这些真实消费者与改写，才能退出同义发现或独有的全域初始化/结束；若仍需全N roster，就承认该工作保留。最终Facts/SSA独立合同不变，不新增N级birth-done/Passed目录，也不重开五槽/getter/codec或已撤回的Facts共访。是否主攻由恢复后的有效完整方向决定，不用这次启动诊断预判热点。[只读剩余工作审查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/m1-remaining-work-review.md)。

### 5.3 M2：目标约束、架构状态与最终交付一起收敛

**M2 与第5.4节 M3 是同一跨阶段算法的两侧，不以 Describe、RA、Complete 的旧计时/文件边界拆开优化。** 当前 `constraints.cpp`、`DeclareGPRBlock/Complete` 和 native visitor 各有真实工作；需要结束的是已经确定的同一个决定被重新解释、投影和绑定，而非删掉所有这些职责。

#### 5.3.1 正向算法、依赖顺序与旧工作退出

| 步骤 | 新服务怎样做 | 实际退出的旧工作 | 仍须承担 |
| --- | --- | --- | --- |
| 目标规范化 | 原family的同源operand/effect定义一次选shape、明确读写、scratch、候选及coverage依赖 | 普通路径完整Describe与晚Select的同义再发现；不是在其后增加lowering副本 | 真实future uses、两pool、fixed/clobber、完整类型/flags/memory语义 |
| 唯一程序交接 | 全部semantic读者闭合后，同槽转为BackendVirtual；稀疏recipe移入本体 | 原Inst/Users借用和第二份dense Selection操作数镜像 | 原NodeId/order、真实CFG/terminal/entry、Params资源和失败前缀清理 |
| 范围与位置 | M3消费实际machine uses/defs和中立约束；Writer在最早birth前履约 | 旧generic live-end后补形态、同决定晚declaration和反投影 | 两scope真实live、Map/REF、MEM/preservation、压力/重试 |
| 实际state与选择 | 成功form产生真实read/write/guest commit/observer事件，原state消费；物理依赖按有限候选兑现 | opcode再猜writes/width/source、独立重建版本历史后binder再查 | alias/guest version、capture先于write、Function entry/loop meet、真实P输出 |
| 核验/物理化 | 独立check实际allocation/effects，最后可失败边界后把同一virtual operand改为physical | Actions→recipe→binder重复包装，正常逐次BuildCurrent重绑定 | fixed/raw端点/width/fault/scratch合同；容量和异常协议 |
| 编码/资源执行 | 现kernel借PhysicalReady；资源描述已在本程序内，真实Tick/pending/scratch仍按点执行 | 每次emit重新匹配source/home和重建logical pairs；true/false重复binding | transport multiplicity/last-use、动态状态、实际机器码/relocation/Flush |

“实际 writes”不能由静态候选冒充；候选不是位置，SSA Map不是guest-copy。一次最终物理兑现、独立检查和encoding中的动态资源仍必要；不把迁移的真实工作全算成节省。

**先闭合位置与状态的相互依赖，才合并遍历。** 会改machine use/def、早写点、固定/隐式资源的决定不仅要在Map冻结前，还须维护该attempt已准备的机器live/排程，并在受影响interval/owner开放前闭合；不能只把晚改Map搬到另一个函数。仅不改这些合同的encoding细节可晚定。Function entry-width与form相互影响时，需真实共同合同的有界求解，或先定form再做原meet，禁止乐观union后补检查、非单调反复全域重跑。

原优先级和最终 good encoding 需映射到新服务。memory/publication/branch 对 integer-flags 的可见性、predicate/resident/narrow 时点必须分别说明；只要保持相同正确性依赖，可以改变旧全域阶段顺序和实现方式。尚不能迁走的求解明确保留，不再通过新包装伪称退役。

#### 5.3.2 用一条真实决定链约束实现

以 `SetHostGPR` 为贯穿例：

- 目标规范化明确source/view、guest home/observer、实际机器读写与logical commit，直接提交同节点form及原稀疏需求；不提前声称“最终位置成立”。
- Block/Function按选中machine live与fixed/tie决定birth home、REF/coalesce/transport。BirthOnly已有真实成功分配，继续复用，不重算收益。
- 原state按实际点消费read/fault/write/guest commit，判定恢复与省略；机器birth与guest版本提交时点分开。全部改位走原Writer，改变过去birth的选择须提前作为小组事务准入。
- 唯一成功Value/Skip/form及实际source进入同一native指令，独立verifier检查真实writes/raw端点/width，不能信候选或Ready位。
- 最终把virtual位置兑现为physical；kernel及true/false重发共读该稳定程序，不再重绑定。MEM逻辑closeout保为实际transport事件，不能跟随numeric输入省略。

这一族只是暴露接口和循环依赖的纵向切口，不是低覆盖快路的性能交付。每包同时迁完整生产域和其读者，保持通用/困难分支可执行；同类机制覆盖不足时直接报告，不按一个 opcode 的局部省时外推全程序。

现有sites/publications/points、FixedWriter前缀共享和home-key查找继续服务必要核验。第77批新增的VerifyHostRead/width查询归约与有序fixed核验已经撤回，**不再列为下一实施包**。5.3.6针对的是稳定源链与动态state的生产/消费算法；不能以它为名重启Q窗口、游标或interval变体，也不另建完整事件投影。

#### 5.3.3 已整合：typed当前参数直达同源kernel

第71批退出fresh native完整body/resource持久化与第二遍body解释；第72批以一个CurrentInstruction/CurrentParams连接193个physical kernel，真实retained wire每次解码一条后调用同一kernel。public/retained的语义IR＋Allocation/UnitActions入口仍在，不能声称所有旧handler都已迁完。完整 Imm/EA/Params/Lambda、nested consumer、真实 role、MEM uses、flags/scratch 保留；typed frame 的复制/reset/capacity 仍计成本。

同 false-link 开发四点为 Block 745.145458→749.408020 ms（+0.57%）、Function 885.665462→860.270114 ms（−2.87%）。各 side/mode 一点且少量工作量不同，无大幅改善，不孤立归因、不继承 da/FEX 分母。停止 codec/getter/variant/容量调参；结构作为当前基础保留。[原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/typed-current/direction-prepared/accounting.json)、[停止决定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/typed-current/decision.json)。

<a id="534-新主线保唯一语义输入至模式确定恢复原完整重发"></a>

#### 5.3.4 已完成最低语义验证：唯一输入延迟退役与原模式重发

自3ef起已整合以下完整生命周期，d391继续沿用，不再列为待实现收益：

```text
全组 Prepare/Complete、实际位置冻结及必要核验
  → metadata 一次捕获/移交，唯一 sealed IR + Allocation/Selection 保活
  → 从空 context/ASM 做 typed visitor，按原有效模式发 true
  → 实际 region 失败：重建所有 contexts，以同一输入发 false
     （不重跑 RA/Complete，不重复 move metadata）
  → 唯一成功 image 最终 Flush
  → 全 group CloseSemantic，再 cursor Abort/Retire，清 Allocation/Selection
  → 原持久 metadata 发布及最后 owner 结束
```

正常 native 逐 N 的24B origin覆盖、早消费和混合前缀/后缀格式已删除。SemanticBorrow不冒充完整PhysicalReady；确有PhysicalReady的retained流及部分转换失败协议仍有效。没有消费权的public/retained可以继续保语义IR＋Allocation/UnitActions及原编码入口，不能为统一图而赋予伪消费权。失败路径先结束 encoder/span/cold 借用，再按原权限收口；ExecutionAccess 持真实 group/parent guards 到对应结束点。

原 uses/Params/Map/Selection 与机器码缓冲重叠，真实 fallback 重做绑定/编码，最后 N 清理仍存在；这些成本不能挪出完整服务。四目标构建、最低语义及本轮完整方向见2.1，最终质量资格仍未取得。旧片段重布局/origin 方案保留为历史，不再作为下一前置。

#### 5.3.5 当前指令一次语义解析，分别交付实际操作数与原逻辑用途

本节记录现码的输入/资源合同及已完成部分；其“保活原IR、每次emit形成frame”不再是新终态。新终态须把必要信息交入同一native程序，见5.3.11；已退出的原用途循环不再计未来收益。

**第74批已收敛原用途这一部分：** native按当前rawID/ordinal分组，logical MEM与懒推断copy共同借用；原整consumer的重复ForEach和MEM线性判重退出，推断不回写selected。family的实际编码字段读取、额外selected来源、role Map及实际运输仍保留，不能称整条consumer只解释一次。冷/显式physical保原链。完整方向见2.1，剩余目标服务继续按本节合同推进。

第74批之前，normal native在family绑定后另行ForEachValue读取原逻辑occurrences，并对MEM逐项线性判重；该域现已由当前raw分组替代。冷/显式physical仍保原链，不能把它误写成当前normal native尚待退出的收益。当前剩余是family编码字段、额外selected来源、各role的位置读取及真实运输，不等于整条consumer只解释一次。

当前保活路线使用CurrentInstruction/Params、当前resource frame、Allocation/Selection：

1. 在当前consumer取得真实typed字段及原occurrences，供family和resource共同借用；选择折叠后的机器操作数继续来自真实Selection，**不能代替原逻辑用途**。Covered/Disabled、folded EA/publication、terminal等仍遵守各自Tick和MEM身份合同。
2. 普通结果由已冻结Map直接给位置；已有显式selected copy/view沿原稀疏输出。未selected的architectural copy仅在本条frame按原规则求值一次，各role借其结果，不各自重扫consumer；推断结果不升级为selected，不回写selected-use表或保存跨指令E结果。一次必要位置读取和动态资源执行保留。
3. 原逻辑MEM输入在当前frame聚合。小固定arity用有界处理；大Params用一次排序/归并等有说明上界的方法，保原raw definition、domain/body冲突判定、occurrence顺序和multiplicity。不能按canonical backing合并不同原输入，不能丢失重复或foreign冲突，不能只用live end推last-use。
4. copy查询借当前width/occurrence解析，保exact-width、publication严格早于use、最新copy与原最后成功view。显式selected整对遮蔽，窄selected优先LowView，推断copy仍在原LowView之后；最后选定pair才做preserved过滤，不为过滤失败重试旧候选。当前Params用WholeValue，不走这条copy推断；其大参数问题是MEM判重，两个问题不混算。
5. normal SemanticBorrow在原粗准入后使用活IR的必要id/type/view；仅MEM等真实读者取得完整token/原始uses。retained/mixed读口保原能力。现OriginOf直接读前缀，并非每operand页地址二分，不能用不存在的成本论证收益。

当前frame只活到本条编码/closeout，不保存全N/E中间表。需要从原参数以外读取的真正选中来源单独列明，不把“当前解析一次”夸成所有祖先信息均已免费取得。true→false仍从唯一原输入重新绑定/编码，不能借这一包恢复已删除的origin覆盖或物理影子。

本节剩余目标是binder、family writer与logical resource的完整交付；已退出的原用途循环不再列收益。仅加getter或缓存字段不算整条服务完成。历史源码事实及严格范围见[复审9.3](compile-gap-review-2026-10-31.md#93-p1一次最终绑定尚未等于一次最终输入处理)。

<a id="536-架构状态的成本按源链事件和实际输出分别约束"></a>

#### 5.3.6 架构状态的源结构、动态事件与真实输出必须分清

第78批曾在原RegisterStatePreparation按definition指针＋完整ValueType准备五角色共享尾，matcher直接交已接受alias/use；完整生产/消费实现和必要语义均成立，但整包没有大幅compile方向，因此这些新增hash/尾缓存和候选payload已随包撤回，不作为下一算法前置。

保留的合同是：PublicationRoot、ReuseEnd、PhysicalWriteRoot、Extension和PublishedAliases各自停止规则不同；S64/U64、raw指针身份、独立zero/sign override/cap、原catalog与显式unknown-zero不能合并。动态home/copy/version/entry/observer/fault必须来自实际事件点，capture先于clobber/write；真实alias/copy输出P仍逐项产生，Function入口meet不能降为Block。

下一替代若需要稳定结构，必须连普通主要需求、成功决定和最后消费者一起减少真实工作，不以“五个role可缓存”或再次搬到inputs作为覆盖证据。首次role解释、catalog/动态聚合、matcher、完整候选证明和P输出要逐项计费；原稀疏GPR publication输入也不是全state需求目录。没有当前动态权重前不批准下一source/owner缓存变体，更不能把源链上限当整个Complete的独占毫秒。

#### 5.3.7 FPR准入作为跨阶段替代的具体闭合项

当前`PrepareFPRProofs`仍先全N发现候选；需要owner时，再收集实际FPR owner并排序，随后核验uses、访问窗口和observer。它与初始FPR输入是不同职责，不能合称“一次FPR准备”。本轮不再单独为此批准一套owner索引或proof优化包。

随5.3.11整体迁移：同源form定义真实FPR读写、tuple/alias、fixed/pool和未来观察合同；实际machine live及原Writer提供位置；必要admission直接检验成功结果，失败仍可关闭coalescing并按原范围重试。独立核验保留，不能相信候选自证。所有proof形态、REF、late改位、跨块窗口和回滚的消费者接入后，才删除旧全N重新发现/排序；若某项仍必要，明确保留并计费，不以“同访”宣称消失。

这只是完整M2/M3的覆盖项。不能删除late coalescing而接受更多spill，也不能用新FPR候选代替实际owner/width/fixed证据；本节不承担独占毫秒或两倍性能承诺。

#### 5.3.8 已否决：以完整GPR最终核验替代承担主要提速

22b18ead在同一原owner中将normal完整核验改为实际Q请求与真实N roster归约，退出全N points/owners投影、sorted writer-key及逐查询prefix/window；保持早publication→FlagsTokens/memory/intflags、晚width/fixed的可见性。实际查询直接写回原PublicationAction，无Q→bool结果表；Function每块清局部状态，保foreign空前缀、原CFG/live/entry及恢复义务。public/manual保独立受检入口。固定声明按真实D/P区间有序核验，真实交集K和排序仍计费，不是严格线性承诺。

四目标构建及最少9case962断言、physical164721checks/1注入、CoreMark B/F CRC4983通过。唯一完整方向B714.960950→730.336552ms（+2.15%），F815.108293→818.285155ms（+0.39%）。每side/mode一点，F的units/decoded/IR/host相同；B请求同为2418，最终IR59614→59664、host243136→243324。无大幅收益，不作统计/独占归因，新增Q/窗口/有序固定核验和新诊断API整包撤回，停止同类调参。

只独立保留实际写入合同澄清：late direct narrow load实际写extension的home，由成功selector写入原PhysicalGPRWrite的replaces_result，fixed classifier用该目标替代旧普通result-home，同时保独立clobber/publication/writeback。早期prepared keys不能去掉旧结果时预先选原受检实际路径，不能保phantom写。该批保留版a10独立完成四目标构建、6cases303断言、physical164721checks/1注入及CoreMark B/F CRC4983；这项不承担性能收益，不继承候选qualification，也不据此声称已发现实际错误输出。

该试验只替代最终GPR核验，没有退役输入准备、初始RA或整个state/Complete，不能因这项失败否定需要贯通大决定链，也不能再用更小的核验变体接续。完整候选、初始构建失败、必要语义、四点及撤回见[决定](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/direction/decision.json)。

<a id="539-实施中实际位置生产与消费者复用"></a>

#### 5.3.9 已停止：实际位置生产与消费者整包无大幅收益

本节记录第78批实施与撤回时态；恢复版495/main120e现为第79批before，当前94cb组件状态见2.1，不用本节旧身份覆盖。

从a10到861af9f6的25文件候选已贯通原assignment/Writer实际写口、独立REF/Width/Low32关系、actual selected uses、唯一reload、normal晚write/width/fixed发现、Function本块范围以及typed source/state和matched角色交付。独立审查确认此前漏迁的正常消费者已经接完，允许一次整包方向；没有把它缩成单独容器或getter试验。

四目标构建、14cases979断言和CoreMark B/F CRC4983通过；原source fixture失败、c0e旧Function collectors失败及before88ee同例重放均保留。候选的全package源码/ELF/patch和证据仍可复查：[候选完整源码与补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/owner-range-index/whole-package-current/files.json)、[候选必要语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/owner-range-index/minimum-semantics-summary.json)。

| scope | 当轮a10 before完整compile | 候选861 after完整compile | 单点变化 |
| --- | ---: | ---: | ---: |
| 独立Block | 731.728826ms | 731.534357ms | −0.03% |
| Function | 804.853740ms | 833.553024ms | ＋3.57% |

四点均退出0且CRC4983，native原true/真实false回退、同PROF1、同guest/env与CPU4只读准入。B units2419→2413、IR59758→59494、host243500→242520；F func435→437、decoded4534→4598、IR69090→69898、host342656→347208。每side/mode只有一点，**不能认定统计回退，也不能将差额归到某个索引或state服务**；足以确认这次没有需要的大幅方向。没有新PMU、最终QoR或fresh FEX分母，不能与旧47ms相除。[整包四点原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/owner-range-index/direction/accounting.json)。

因此整包新增算法撤回：恢复22个原文件，删除placement_index.h/.cpp，唯一独立保留pinned_gpr_transfer_test.cpp旧fixture修正。恢复以copyfile更新源时间；四目标重建后translator/swift_ir/lifetime逐字等a10，当前source495887ca，main120e。修正fixture确认真实BirthOnly请求后要求成功生成owner/home/publication/end/version，1case28断言通过，生产合同未放宽。原a10生产CRC/physical按同ELF原身份引用，不再跑相同质量矩阵。[撤回与精确恢复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/withdrawal.json)、[恢复资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/qualification.json)。

**停止owner bitmap/邻接、grouped use/reload、source尾缓存/matched payload及晚发现这条实现路线的继续调参。** 这不是说相关旧工作零成本，也不是否定任何未来减少重复决定；是当前完整替代没有足够总收益支持新增维护复杂度。不能按“结构看起来更好”保留这批性能中间表，或再用更小的word/window/container变体继续。a10里独立正确的direct-narrow实际结果目标合同继续保留，不属撤回包。

<a id="placement-range-cost"></a>

#### 5.3.10 已撤候选的范围与净成本教训

本节保留算法审查约束，**不描述当前数据结构，不要求下一实现保留候选文件**。本块问题不得先枚举全Function home/REF成员再滤块，最坏会形成Q_b×K_function,h放大；本地REF却仍可追到外块canonical并读取原跨块事实。真正incoming/released-home/preserved的scope义务不能裁成本块，未提交width关系也不能由当前位置邻接代替。

861用唯一32W个u64的canonical-home位图、独立REF树/位图替换旧home链。局部仍扫home/REF两组words＋全部本地REF解析；GPR＋REF约16.125N，对比旧12N，Width/Low32各12N另计。global空home在backing建立后也扫W，首次preseed/初始化、每次写口、rollback/reset/retry/release均付费。source五role首次walk、门前请求/全尾max、hash/backing、matcher向量与真实dirty/P/region输出也未消失。

这些成本已包含在唯一四点内，不能因局部范围正确、复杂度好看、少几张表或25文件贯通就推算主要耗时退出。保留正确性合同，退役未获收益支持的具体中间表示；下一个方案必须先解释当前普通主要流量中究竟什么工作占大头，避免再次从“有可写优化”倒推主攻方向。

<a id="compile-net-work-correction"></a>

<a id="5311-下一完整替代同一store中的后端虚拟与物理程序"></a>

#### 5.3.11 当前承载方案：以净工作比较取舍

**本节记录已经选用并实施的同Store路线，不证明它是唯一或最快的路线。** 旧版已有native、typed-current和193个同源kernel，保sealed语义IR＋Allocation/Selection至Flush；本轮真正改变的是程序接管时点、machine live来源和稳定operand交付。选择本路线的理由是让已选机器形态直接服务原RA及最后消费者，并避免第二份完整操作数程序，而不是以提前销毁Inst作为提速目标。

此前“稀疏Selection只有两种结局”的排他论证撤回：generic继续读取Inst，不必然意味着完整Describe/Complete继续执行；原有成功选择和单consumer frame也能退出重复发现。实现可保留当前NativeInst，或比较沿现有权威数据收敛的方式，但不得同时维护两套完整程序。评估单位是相同真实输入下的退出、保留、新增、miss/retry及最后结束成本。以下机器合同继续适用；接线与原例资格统一见7.1，C批次状态不再作为当前待办。

```text
IRStore / PhysicalStream（原CompileUnit / Prepared持有）
  Semantic → Consuming → BackendVirtual → PhysicalReady → Retired
  同一NodeId、原order与存储租约；Inst结束后同槽构造NativeInst

NativeInst：target macro-op/form + 原结果身份 + virtual operands
            + 真正implicit/transport资源；最后同一字段改为physical
AllocationProgramView：借原程序/CFG，向公共RA提供机器uses/defs/约束
SelectNativeVirtual → AllocateBlock / AllocateFunction
  → RealizeAllocationDependentForms / 原state transfer
  → VerifyNativeAllocation → FreezeNativeOperands → 现kernel
```

`AllocationProgramView`只借用原程序/CFG，不拥有N/E容器；公共层只认识值类别、真实use/def、fixed/tie/clobber/scratch等中立合同，不能包含ARM64 form/NZCV/寻址enum。CurrentInstruction是只读借用视图，不是每节点常驻的完整编码帧；native路径直接借body，旧variant/Params只留在原PhysicalBuilder的冷存储。第79批已将112B NativeInst放入原128B managed槽（16B prefix＋112B body），104B Inst合同与48B packed record仍分别保留；完整Imm64、per-use view、Params/Lambda、MEM资源与原NodeId/order都有实际接线。secondary等完整目标语义仍须按5.3.12规范化，不能由容器可表示推为全族已支持，也不以字节大小认领主要收益。

| 边界 | 必须同时完成的合同与退出条件 |
| --- | --- |
| Known早建 | 通过现纯能力尽早规范化**公共可编辑语义IR**，少出生中间节点；frontend不产NativeInst、不含ARM form/home。不在sealed Inst偷写semantic字段，不把早建命中当SSA豁免 |
| 最后semantic读者 | generic改写/hoist/最终编号、完整SSA Finish、真实future-use/图匹配结束；terminal/entry/call-return/hoist值转到原stream/B元数据。SSA Finish仍读nonlocal定义，不能边首访边销毁 |
| 转交 | 预备真实容量；可复用资源/成功recipe唯一转交，异构Params在当前指令构造唯一native backing、cursor析构旧Params，再move准备结果。转换和暂时重叠计费，不称原vector零拷贝。关闭Users/图借用；已转换前缀不再通过Inst/Value解释，失败时原cursor先结束semantic尾部，原stream再按committed prefix析构native对象，Store只管phase/alive/退役原语 |
| 机器live | RA消费所选形态实际reads/defs/implicit roots与最早birth；semantic SSA/counts另保真实用途。旧ends只在意义完全不变时复用；改变源、折叠或提前写就维护/重建原allocation范围数据，不另建反向E图 |
| 候选与live的相序 | 静态form先定；位置候选为当前attempt选一个明确待履约合同，机器live按它准备，实际准入成功后才成为最终actual。cohort改变use/def/根/birth时，须在任何受影响interval/owner开放前原子维护live、fixed/tie/clobber、队列/pressure及依赖state；不能只改recipe或Map |
| 物理依赖候选 | 影响过去birth/use/def的writeback、direct narrow、forward、flags共用，以原tie/REF/事务中的少量关联点为cohort，在最早受影响birth前整体准入。失败完整回滚或用完整generic合同；不能晚改Map补账。cohort不是新全域依赖图 |
| coverage | 保memory/publication/branch对integer-flags等真实优先级。候选不算成功覆盖；Disabled、无machine body、仍需Tick/逻辑closeout的Covered分开，不能一个位删全部义务 |
| actual state | 同源form的实际read/write/extension/guest commit/observer驱动原state；fault capture在真实clobber/write之前。逻辑版本、raw alias、copy窗口、Function entry/loop meet和P输出仍计算，不把迁移当全省 |
| MEM资源 | selected numeric inputs不能替代原logical occurrences。第一完整实现把必要raw身份、multiplicity、原use/death/terminal点变成同指令真实transport/retirement事件；MEM-only descriptor保token/count/reload。不保完整旧ordinary operands/Users影子；改变pending-spill语义须连forward/reload/observer一起迁 |
| 重试/最终检查 | attempt间保virtual cells不变，稀疏form编辑走原事务；reset原结果后重试，不恢复raw IR。独立checker读opcode/role/effects及实际Allocation，保fixed/raw端点/width/extra writes/fault/scratch失败语义，不能凭selected/Ready位跳过 |
| 原位物理化 | 所有可失败检查与容量准备后，做一次真实operand兑现并发布不可变PhysicalReady。不能每个use都用裸Map替换，须兑现point/whole/LowView/selected copy与preserved过滤；若仍会失败，保明确prefix abort，不假称noexcept |
| 重发/最终结束 | true/false只重建ctx/ASM、host offsets、relocation及动态资源执行状态，共读相同PhysicalReady。影响资源的mode差异先给真实共同安全合同或显式变体，不靠永久最大reserve损害质量。最后资源/lease真实释放或有界复用仍计完整服务 |

现码先`CollectLiveIntervals`、排序/准备pressure，再进入assignment。因此“当前尚未执行source birth”不足以证明可以局部换源：例如`x → ZeroExtend z → Add y`折叠会延长x、删除z的machine def，已建区间必须同步改变。若上述维护无法在原owner开放前完成，就回滚到最早受影响owner之前，或结束该attempt，换完整generic/另一有限合同后重建真实机器范围；不得沿旧semantic ends或只回滚Map继续。候选回退须有单调禁用或明确受界顺序，不枚举跨cohort组合；每种attempt的上界、重建N/E/P及失败成本在实施中填实，不预称所有路径只分配一次。

**Function限制。** 保原CFG、外部/缺失entry、foreign/terminal/hoist roots、跨块SSA/live、call-return及parent/code-object边界。`entry width → form → transfer → entry width`若形成环，必须有真实共同合同及有界求解，或先确定form再按原meet计算；不能用乐观entry union或非单调反复全域重分配补偿。单块Function仍用Function入口。Block局部发现不枚举其它Function成员，真实跨块义务不可裁块。

**普通路径覆盖。** scalar明确源view、W/X/partial和flags transfer；memory包含generic/TSO/atomic、writeback/direct-narrow真正早写与fault顺序；条件分开Boolean、NZCV和guest flags，保CF/PF/AF；控制包含Params/动态callee、terminal/entry/edge和mode替代。向量/FPR/tuple及helper ABI也须同源交付。不能只做几个融合，其余普通流量整块跑旧Complete。

| 当前服务 | 目标去向；旧入口/循环何时可删 |
| --- | --- |
| 优化前SupportsIR；最终schema/SSA | 两种不同准入保留；仅取消新语义不再需要的同义解释，不将后者降为producer自证 |
| DescribeBlock/Finish及GPR/FPR/width/scratch语义投影 | selected operation直接喂原公共约束与机器live；所有normal初始需求接入后，完整旧Describe从该域退出 |
| DeclareGPRBlock、晚source/home/memory/flags再发现 | shape一次规范化，物理关系在原RA/cohort兑现；actual effects直交state，不再actions→recipe反投影 |
| FPR准入及GPR/fixed/width核验 | 成功结果和opcode规范直接受检，保失败/关闭coalescing重试；发现/排序能否删由消费者迁完决定，不预置新owner目录 |
| PrepareValueVersions、dead-root及native copy-history重放 | 必要alias/版本/entry/fault由原state按actual events转移；独立历史重建和相同source重查退出，真实求解仍计费 |
| 每次emit的PhysicalBinder/family TryWrite/BuildCurrent | 最后物理化直接形成稳定operand及必要transport事件；kernel借同一body，不再重建semantic current/逻辑pair |
| plan/Actions/certificates兼容层 | normal调用者迁完即删字段/转换/入口；public无消费权的checked adapter保真实能力，其完整旧链不进入normal快路 |

**已验证范围与剩余成本。** 第79批实际Store/Block/Function夹具已覆盖同址对象转交、Params/Lambda析构、失败前缀/语义尾、墓碑与NodeId/final-id区分、换源后的机器live、无body BitCast/REF、terminal专用root及其consumer view、原四块CFG/call-return和单块Function范围。公共core复用原LinearScan；缺失或覆盖/顺序/端点非法的scratch表在改Map前拒绝；这不独立证明预算与opcode语义匹配。已提交且仍依赖语义的政策输入返回SemanticPolicyRequired；未提交需求须由前置目标准入识别。夹具中的显式零scratch只适用于其已知简单形态，不能作为生产默认值。

这些接线没有使默认normal旧服务退出。第80批接原GPR pressure/next-use，C3接actual birth/initial/GPR/FPR/width/controller/raw/spill读者；后续C6落实typed GPR/MEM发表、scalar schema、final form相序、单一native入口宽度、批量transport与有限真实Storage准备/Ready编码。193 kernel读口不再是未来迁移项；其余form/state/控制域与默认替代见5.3.12.6。旧质量能读program或新入口能编码，均不证明普通服务已经净退出。

机器liveness的N规模数组/临时身份核对与CFG传播仍有成本；token查询仍含Store membership、B定位、REF追根及pair的committed-prefix查找，不称免费O(1)。Original/raw已开始自动保留与退休，logical-only非MEM已有处理，但新actual source的原逻辑计数为0、pending MEM与最终Tick/释放仍要由最后资源读者闭合。Params逐项转换、稀疏backing/析构及Prepare/Freeze双resolve继续计费，不先加N目录、E缓存或新owner索引掩盖。


以上合同不以“全部ISA先迁完”作为性能反馈前置。选择有代表性的真实编译单元，让输入生产者、原范围RA和最后消费者完整跑通，连同合法miss/retry计入before/after；不以toy夹具、单helper或只统计命中域代替完整反馈。未退出的乘积查询不能因列为保留费用而获得性能资格。M1减少真实出生/首次解释、M4减少相应触达共同进入总成本计划，不等后端迁完再处理；不要求每个实现包同时修改三条线。[5.8.3](#native-measured-net-work)给出热点与退出安排；现有方案仍未证明可达两倍门。

依据：[同Store及完整读者账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/canonical-delivery-design/store-owned-native-delivery-review.md)、[machine live/最早birth/state复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/canonical-delivery-design/gpr_actual_writes-design.md)、[稀疏Selection覆盖限制](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/canonical-delivery-design/design-review.md)、[早建解耦复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/canonical-delivery-design/docs-update/early-build-review.md)。报告对永久载体有不同建议；本文采同Store，同时保同源operand/effect与最早birth合同，不保三套并列路线。

<a id="5312-下一相依组先闭合机器合同再替换默认链"></a>

#### 5.3.12 机器合同与有版本的迁移记录

**本节保留机器合同及迁移记录，不是继续铺开全部family的执行清单。** 各C批次“当前/下一组/未启用/失败”按其原source理解；涉及actual source/birth、raw/lease、fault/capture、Function范围和事务的合同仍有效。后续只按7.2选定的完整算法包推进，不能因历史缺口仍写在这里而重迁已有读口。

通用容器不能靠“每个语义arg都是machine use、每个result都在id出生”直接接入normal。先在现select族完成静态形态规范化；最后semantic图匹配必须在首个Store转换前完成。只为真实关联点保少量cohort信息，成功结果进入同一native本体，不建立全N/E翻译表。

| 对象 | 必须交付的真实机器合同 | 相序及旧工作退出条件 |
| --- | --- | --- |
| `Div128Remainder` / `CpuidUpper` | secondary自身无machine body，其非REF结果在Div128/Cpuid producer点与primary联合定义；producer有真实secondary output link，逻辑secondary身份/closeout仍在。完整接入还须交付联合pressure/scratch/ABI与独立目标alias检查；C2未证明后三者全部闭合 | 首版保原同块、合法U64 producer/secondary、唯一且严格后继等准入。唯一def枚举、Definition查询、birth所属块、输出位置resolver point全部一致；禁止只改live-end或只回填Map。跨块pair在粗准入拒绝，不猜测支持 |
| `BitCast` / `GetResult` / `GetFlags`等pseudo | whole-alias BitCast无数值body/read，实际late use沿REF传播；GetResult在自身点真实编码，实际数值读取为arg0；GetFlags在自身点编码并读隐式flags state | 按实际kernel分别定义reads/defs/body/Tick/逻辑事件；不能把六类pseudo统一当alias或提前出生。旧Function保守提前区间不等于真实machine birth |
| 全部normal初始需求 | 同源form实际uses/defs与fixed/tie/clobber、真实rich scratch、ABI/两pool、GPR/FPR/width质量政策 | 先完成初始合同及原共同assignment/repair读者迁移，再删除该支持域完整Describe。保scratch表及已提交语义政策的显式拒绝，目标粗准入另须识别未提交的实际需求；禁止补零预算或关闭质量能力使core表面成功 |
| actual state与最后交付 | early write与guest commit分开；保entry/join/loop、fault/observer、raw/width、真实logical MEM transport和独立check；193kernel直接借native operand | 同族生产者、分配/state/check和最终读者成组替换，迁完才删旧Complete发现/重建与semantic binder。true/false共读Ready；不先跑全旧链再转换新body |

pair必须有一个权威机器定义和可定位的逻辑身份，不允许producer与secondary各枚举一次def。`value.id`是逻辑owner/key，`birth/Begin`是实际机器定义点；live、fixed、位置与输出resolver都须使用真实点及所属块，不能仍用secondary自身id。联合定义枚举可能为producer、secondary、下一ordinary，逻辑ID并不单调，固定读不能继续只靠递增ID游标。需要查找时借同程序关联或原已排序稀疏约束，不建立全N result directory。

**端点按消费者原合同保留。** LinearScan仍是`end < start`才expire；fixed writer核验保`(Begin,end]`，忽略自身定义写并包含后续end；home-index仍保`row.begin <= query.end && row.end > query.begin`，preserved/transfer和FPR tie分别保原端点。不能把所有范围统一改成同一开闭区间。多个非REF输出在同birth的目标冲突须独立检查；排除了birth的fixed窗口、按逻辑ID筛owner的旧checker和CoreAllocated都不能证明同点安全。ordinary result的实际替代也不能抹去其它clobber/HostWrite。相关现码见[NativeInst定义/用途枚举](../source/runtime/backend/arm64/compile/native.h)、[native适配](../source/runtime/backend/arm64/compile/native.cpp)、[fixed合同](../source/runtime/compile/allocate/constraints.h)、[实际scalar kernel](../source/runtime/backend/arm64/jit/translator_scalar_physical.cpp)。

这一组的验收是完整machine合同与原质量服务接通，而非新增若干opcode特判。未支持域在销毁semantic对象前选择原合法入口；发生不可恢复的转换/物理化前缀失败则按原请求收口，不能返回旧链读取已死Inst。成组构建和最少受影响语义后继续贯通，完整旧工作退出后才做一次整体compile方向；本次修订不新增测试矩阵。

<a id="53121-c3在建状态与完整交付分开"></a>

##### 5.3.12.1 C3源码接线、验证与normal退出分别记录

C3/6f7b已完成下表整合与已知边界修复，四目标和所述8个最少case通过；**完整旧normal服务仍未退出**。前版11个native阳性保aea身份，不转作本版运行；不继承C2的23例或任何旧性能结果。

| 已接入当前源码 | 仍需完成或验证 |
| --- | --- |
| actual birth贯穿初始fixed安装/收集、Writer/location/preservation/recolor与质量读者；原稀疏fixed读按真实Begin查询，联合输出不依赖ID单调 | 保原预着色/fixed/REF等优先级和各自端点；独立最终核验须覆盖同birth非REF多输出及真实额外写，旧逻辑ID roster不能代替 |
| 一份ProgramLiveness供initial、FPR与原attempt借用；native回填原B identity，提供site/definition/raw-use/block-use/range借用接口 | 本组已修点/块/顺序/定义边界及InvalidProgram分流并通过对应负例；Matches仍只核域，编辑form/use/def/birth须维护或重建live |
| semantic关闭前已接whole-unit/Function-local flags、原GPR/width拓扑；native initial生产rich scratch、两pool、actual home访问、净clobber及raw helper ABI | 当前只覆盖明确支持的形态；未知或未迁形态仍拒绝，诊断和public/preseed域保合法旧入口；不以0或缺省关闭质量补洞 |
| GPR/width完整旧质量读者、中立policy helper、FPR resident/tie/scalar/unary/AES/merge/publication及跨块live-in读者已接program | 这仍包含分配后决策，不证明实际machine form/uses、guest state或最终check已兑现；遍历次序按各原政策分别保留，不能统一排序改质量 |
| no-Facts Writer、AllocateProgram、原RunAttempts、结束coalescing/FPR关闭coalescing重试及Block局部repair接线 | checked pool预算已恢复原策略，完整质量/FPR重试场景已过；program/live借用仍须贯穿原重试至Finish，初始空Map准入不代表公共预分配域已迁 |
| 原raw occurrence用NativeCell的Original位与唯一退休行转交；shared spill region/ownership/hand-off与局部repair读actual/raw接口 | scratch非法与真短缺已沿原scan错误域分流，GPR policy读者也已接传播；完整MEM生命周期、换源后原count为0的实际源、最终Freeze/kernel/Tick/重发仍待闭合 |

范围依据：[整合冻结清单](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/frozen-build-01.json)、[backend initial与三域组件](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/backend-initial-and-raw/component.json)、[GPR/width组件](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/gpr-width-program/revision-02/component.json)、[FPR组件](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/fpr-native/component-receipt.json)、[raw/spill组件](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/spill-program/component-receipt.json)。组件回执只证明保存范围，不代替当前整包构建或运行。

##### 5.3.12.2 初始需求的同源生产与失败边界

当前顺序以5.3.12.15为准：actual-input整组已经替换C3的语义质量预生产及转换后修订。下列表示现行支持域合同；构建和受影响语义资格分开见7.1：

```text
最后semantic读者：全组SSA/编号/flags及source门完成；保raw身份与必要semantic live，不生产native质量计划
  → 粗准入/terminal-entry转交；原槽转换时直接确定静态form（含有效FPR options）
  → 在受影响owner开放前选明cohort机器合同：actual reads/defs/birth/write-owner
  → 一份ProgramLiveness与同源initial（actual form/scratch/fixed/tie/clobber/ABI）
  → 原Block/Function controller：完整quality、共同assignment及局部cohort履约/回退
  → native state/真实transport → 最终独立检查（晚编辑失效与原retry闭合）
  → 最后可失败边界 → 原位物理化 → 同一Ready交给kernel/重发 → Flush/发布/结束
```

| 需求 | 唯一生产者与消费合同 |
| --- | --- |
| scratch flags | 最后semantic阶段用原`SemanticFacts::FlagsFor`保whole-unit RA需求，Function用原`LocalScratchFlags`保emission-local需求；在真实flag-sensitive/pseudo关联点稀疏转交。两个domain分别标明已知，数值相同也不省略域证明；缺失不等于零。无pseudo的已知简单form可由本体固有profile/outputs推得，不能每个N节点另建flags表 |
| rich scratch/两pool | 原同源family规则消费真实form、flags与`live.HasUse`，生成原ScratchProfile及端点/租用合同；helper形态与native形态分别算。不得把语义use-count、空表或零预算当完整机器证明 |
| guest-home访问 | 实际Get/Set及partial offset、真实early writeback的home读写，由同源选中form完整生产；与observer/capture顺序一起交state/FPR。C3已在支持域由实际Get/Set生产mask及原稀疏访问序列；新增/未知形态仍须完整描述，缺失不等于0。非法或缺失显式拒绝 |
| 分配池净clobber | 原fixed破坏/真实机器写对当前活值池的净影响，供allocation和scratch消费；独立于guest-home访问。临时调用内被保存/恢复的寄存器不能误算成全live-range破坏 |
| helper原始ABI破坏 | 原HelperCallContract用于call/capture与先保存后破坏顺序；preserve-all trampoline的raw ABI clobber不能直接OR进净RA mask。Div128Helper/Cpuid与NativeDiv保实际形态差别 |
| 请求质量选项 | scalar-insert、resident、width/intwidth tie、coalescing、eviction及pool从原请求/配置显式传入；不能用`AllocationOptions{}`默认false或静默关能力让core通过 |

`DescribeNativeAllocationInputs`返回`Complete`只表示该次初始inputs生产完成；`PrepareNativeGroup`的输出参数`source_result=Complete`则表示消费前source门已通过，并在Take之前设置，不能据此认定initial已成功。本版owner前三失败正是source已Complete而initial失败。CoreAllocated也只表示核心分配完成，均不授state/transport/proof/Ready资格。非Complete时整份部分inputs不得交allocator。已知不支持的能力在semantic关闭前粗准入；转换后发现非法合同则按当前请求安全失败，不重新读取已死Inst。未迁政策仍显式拒绝，既不能漏提交，也不能先跑完整旧Describe再给新入口盖章。

##### 5.3.12.3 一份机器存活期接通完整质量服务

`ProgramLiveness`属于原RA范围状态，目标描述、FPR政策、核心分配和所有未改变合同的attempt共同借用；不能各自Prepare一遍或另建第二用途图。其意义必须固定：

| 查询 | 真实含义与用途 |
| --- | --- |
| `UseEnd` | 全scope的实际use-end及原CFG/call-return传播；Function不能拿局部端点代替 |
| `LocalUseEnd` | 定义所属块内实际numeric/implicit/terminal的最大点，缺省为实际birth；与全局CFG端点分开供fixed/FPR原局部政策 |
| `UseCount` / `HasUse` | 完整机器occurrence计数，不按point去重；raw alias与wholeREF root各记一次，同一身份不重复。logical retirement和no-body alias边不冒充机器读取 |
| `UsesStayInBlock` | 实际用途是否跨出定义块；不能用ID区间或旧roster猜测 |

任何form/operand/cohort改变实际读写、根或birth，调用者必须在受影响owner重新开放前维护或重新Prepare这些状态及依赖约束；`Matches(program)`不是revision证明，不新造revision manager掩盖这一责任。Block不额外承担Function局部/跨块数组；Function新增N规模状态、初始化与CFG传播如实计费。

FPR迁移必须包括resident、destructive tie、scalar binary/minmax/unary与insert、SHUFPS、AES链、两种merge、ordinary publication/recolor、input alias、future fixed reads、active expiry及post-publication。共享原family规则，仅把输入换为actual definition/operand/窗口和上述live；不只接tie，或复制旧`OperandEnd`快照。fixed读按实际Begin看未来、保原写窗口；post-publication须包含Function live-in/foreign producer的真实birth/end、原B/CFG身份与observer，不能只扫store块，也不能一律拒绝跨块来规避。

原预着色/discard/fixed/REF优先级、MapFixedInput/预分配安装及结束时的FPR publication/coalescing必须从program入口真实执行；非空政策不能绕过，空政策也不是“无需求”的证明。FPR候选交回原forced-spill/共享8预算/reset/reserve/controller，只有原独立的最终FPR合同复证拒绝才沿原disable-coalescing重试；initial缺域/不支持或scratch不足不能借此降级。不得复制第二allocator或提前关闭质量能力。

C3已将这些质量读者与原RunAttempts接线；本版完整Block质量/FPR关闭coalescing重试已运行通过，其它既有native范围保原AEA身份。每次RunAttempts内eviction共享8额度（该次首扫加重试最多9次，之后仍有原有限reserve ladder），原独立FPR admission失败后的disable-coalescing重试门保一次；不能把两者合称全请求最多9扫。AllocationState只拥有inputs，program/live属于借用，必须活到重试及Finish完成；native Writer不依赖已结束SemanticFacts，Block局部repair不向Function扩域。

原语义GetUses计数、完整机器UseCount及raw occurrence是三种用途。当前native在原转换/Descriptor访问记录raw资格和actual用途，直接生产GPR/width/residence输入，已不再把语义拓扑先写成质量计划；checked仍保真正需要的semantic服务。REF只表示whole-value等价，Low32、宽度共home与每次use view不并为whole alias。后续成功形态改变读写时，仍须在受影响owner开放前维护/重建live；不能把旧post-RA决定盖成Ready。

##### 5.3.12.4 剩余复杂度不能被接口复用遮住

C3初始producer已生成唯一已排序的稀疏RegisterAccess与publication observers，FPR/GPR借用；不再为每个publication全扫body分类guest访问。native post-publication仍对每个publication枚举实际definitions：以P_pub为publication数、D为实际definition数，最多有`P_pub×D`次definition访问；每次Map/REF等解析成本另计，不能将访问次数冒称完整复杂度上界。future-fixed-read有R_fixed项，每个reserved-home tie候选循环并二分birth，二分本身为`O(R_fixed log R_fixed)`；T个相关候选须计`O(T·R_fixed log R_fixed)`，另有实际Map/REF、active查找与成功后的heap重建。上述符号不与5.8的输出P、retry次数R混用。

这些初始服务已有范围限定的构建/最少检查，仍不可达于完整normal，不算提速或已退出旧图成本。原准入中的canonical definition核对也有真实查询成本，不能据共借live称其免费。完整迁移应优先借原active/residence、已排序稀疏inputs和同源窗口；不能以漏Function live-in换近线性，也不为此恢复已撤N owner目录/第二图。若乘积式扫描保留，必须在整包净工作账明确其范围，不能声称“每个值只处理一次”。

与FEX对照的采用单位仍是**整个普通服务退出**：支持域不再跑旧Describe/Finish、旧语义质量投影、Complete同义发现/重建和每次emit的semantic binder；保留必要CFG/独立合同与实际编码。接口共用、同槽存储及本组46文件变化均不能证明≤2×。M1减少真实出生/首次解释、M4随旧服务删触达与清理须并行纳入同一整包，不留到性能不达标后再做微调。中间成组最少检查，完整旧工作退出后一次整包方向，最终统一6.3/6.4。

##### 5.3.12.5 当前必修项：修边界，不另造证明层

下表记录本组已修复的构建与边界问题，均沿原owner落实，没有新增pass、证明层或性能候选。修复后四目标及最少检查见2.1；列原错误供审查，不再作为下一待写任务。

| 原发现 | 已落实的修正与保留合同 |
| --- | --- |
| program.cpp的scratch短缺量两处u32窄化初始化导致构建exit1 | 在原计算处明确合法宽度/范围，保原严格余量公式；不能删检查或扩大预算规避编译错误 |
| VerifyProgramCore中坏use顺序/Resolve失败只返回false；点/块/output边界未全部先核，可能先访问ScratchFor/Dirty/ValueType | 原Program/scan错误域明确InvalidProgram；在索引/推进前核点、块、身份和birth。CheckInstr及spill Headroom同时分清invalid与真shortage，不把坏callback当无region或质量成功；controller观察invalid即退出，不LocalRepair、不升reserve |
| FPRInputs的blocks publication span未在ProgramInputError完整核对 | 首次Map写前用原准入核全部被消费的稀疏span、索引/类型/块域；`first <= size`且`count <= size-first`，防溢出。不能信任target producer、也不能等分配后subspan越界才发现 |
| 原共享RunAttempts无条件用declared pool取budget，连checked首尝试都改变 | 保原checked入口当前pool的reserve/allow-zero/incremental ladder；native所需declared-pool政策在自身分支明确选择。此次中立接线不夹带质量政策变更 |
| alias测试把原raw view的VOID期待成U64 | canonical定义类型仍是U64；raw per-use cast须保原VOID。修fixture断言，不在producer补cast，也不弱化真实ownership正例 |

另修正了完整质量fixture的两个错误前提：初始U64 GetHostGPR按原策略不设HostReadCoalesced，改以真实kind/home22/fixed-class强合同检查；原toy pool也未启用声称要测的GPR coalescing，改该case使用既有有效pool并显式断政策开启，所有GPR23/FPR17写合并及retry正例保留。生产Policy和marker未为测试改动。[原策略依据与fixture差异](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/quality-followup-03/tests/pool-domain/receipt.json)。第二次真实编译暴露的semantic terminal point提升为int也改为原u32/0u；原失败均保留。

静态inputs完整性在原准入边界核一次，动态callback非法在首次观察处失败；不为此在每个attempt重建全N证书、第二用途图或新validation manager。普通distinct reload、terminal逐occurrence extra、region excluded/reserved严格余量公式已按源码复核，不修改计价。pressure例的birth→last完整ownership且非fault-backed是原策略应有结果，失败应修生产接线，不能删断言换通过。

<a id="53126-下一大段让质量决定成为真正可编码的程序"></a>

##### 5.3.12.6 机器决定、事务与编码交付合同

**本节是现行实施合同，不新增一套pass或管理层。** C6限定joint事务保原build-17/min09身份；历史C7/3bdd060c数值、控制引用与普通内存保各自限定检查；当前支持域已默认同Store直产，normal覆盖与限定资格见7.1。对照固定FEX，实施单位是早定实际形态→原RA落实同一程序→kernel直接读取，以及旧服务的实际退出；保SVM Function、异常恢复与独立检查，不把“有了新链”算作净减工。

| 已有基础 | 当前相依组与仍待完成 |
| --- | --- |
| C5：静态FPR form/effective Config/实际scratch、primitive shared-write lease/core、primary替换原语、原state/width/CFG共享算法 | 后续C6已在限定域贯通原transport、独立proof和生产准备；继续补完整Capture/LONG/fixed/read/tie及复杂位置cohort，不重复迁移已接读口，限定检查不授全支持域资格。 |
| C6原事务transport、独立GPR/fixed/width、actual scratch和两阶段资源准备 | 分类与独立核验各按整批重新推导；2b3c/min09受影响joint已过原Submit/Capture/width、真实SameReads/backing、错误拒绝、完整rollback、正确重证及Ready/退休。05bc/min08的retained_rows=0保作历史断点，见7.1。 |
| C6主树：GetU32静态B、真实fixed/preserved及SelectZero shared-W | 稳定B先于live/initial，公共canonical白名单不变。Get32两home的真实capture/W-copy及后续proof/Ready已过；新partial-view例保真实preserved capture与Whole来源，不伪称全局LowView@22阳性。 |
| C6主树：typed普通发表、六类scalar schema与final form顺序 | 支持Whole MEM和已建立的reload关系；producer/resource共享原adoption规则，独立reader重核真实writer/lease/路径。fused、selected-memory及未建立reload入口仍缺域。所有final form先于最后fold/selected。 |
| C6主树：Function入口宽度 | 删除preclose semantic transfer/solve与30B查询；residence和最终metadata共借原native solution。真实late逻辑输入改变时typed拒绝并回滚，不缓存失效最终值继续。 |
| C6主树：原Storage生产准备 | 公共入口的Block、完整双Function组、真实MEM压力和width-only Function均随min08生产例通过Ready/编码/退役；未强制code-cache失败、未发布或执行guest；该C6轮默认gate为false。 |

**一、最后semantic闭合时交付稳定资格，转换前选择完整机器合同。**

先完成结构/用途/flags/编号编辑和SSA Finish，再借唯一UseFacts交付图/类型、原次数及必要稳定资格；全部semantic读者在转换前闭合。目标是最后读者后不再逐候选回扫程序找consumer；迁移版仍有的raw/actual、definition和路径发现属于待退出算法，见5.8.3。机器读取与raw退休分别保真；需要逆向关系时指定唯一权威、真实完整域和最后reader，不能以禁止一切索引为由保留Q次全scope搜索。

**当前native的Derived fixed由actual definition/live端点直接生产。** 原`PrepareGPRQualityTopology`→`PrepareNativeFixedInputs`的semantic预生产及Derived事后修订已在merge90退役；Explicit硬窗口、preserved/transfer/capture保持独立输入和实际重资格。source阶段保原SSA/归属/精确typed门，不能先Consume再回checked。Derived各家族复用原纯规则与Allocation owner，原checked匹配按其真实caller保留；Explicit seed、稀疏插入/索引及最终独立核验照计。完整顺序见5.3.12.15。

稳定资格还要保原精确anchor约束：原read/extend/narrow/alias图命中后，只标记真正受该合同锚定的候选或行，防另一A决定先改坏它；不建全N目录或整块禁止recolor。提前匹配会为原Map gate本可早退的候选增加工作，照计净成本。`RecolorAnchored`已有本次构建接线，不等于其所有运行域已获检查。

A表示在已兑现机器合同内确定位置、copy和发表；B表示必须改变实际source/use/def/birth。先问B是否真的依赖Map：可由稳定图/类型决定的B在预RA原位兑现，一份实际live从最终读写生成；真正依赖Map的B才进入原owner的局部调度维护/回退。不能把所有B都变成动态RA hook，也不能每拒绝一个候选就重启全scope作为常态。

预RA修改原quality行后，所有真实读者一并消费其失活/新source语义；native宽度提交检查读同源GPRInputs publication的actual stored/home，不能从已清空的semantic fixed_validation误判没有commit。真实Get32 W capture与publication的64位clear-high commit分别证明；只有实际目的区别于旧guest home且capture保留时才可省copy，仍保clear-high/Implicit语义，不能改普通Covered或冒用AdjustResident。所有相关body编辑先结束，再完成原受影响B最终fold/install与selected，防止catalog尚有效而程序已改。

| 典型域 | 正确路由 |
| --- | --- |
| 真正primitive U32结果，例如支持的LoadU32/Add | 可由一次实际W写产生合法宽视图，后续A才用原Recolor/fixed/实际early-write兑现目标home。当前已合入target首域仅覆盖原matched no-source Copy、本块真实U32 primary且id等于actual birth、LoadU32或Add(MEM)及已兑现桥/别名；不是所有U32输入。 |
| Get U32 → To64 → publication | 已合入的首域在首次live/actual quality行出生前改实际publication输入为Get32，保64位commit＋clear-high；仅唯一实际Wide reader就是该发表、无其它义务时退bridge machine use/def。raw header/次数/Original/Tick和fixed软hint的raw value/source不改；随后直接生产actual source/birth与原preferences，不再失活旧quality行。无hint合法；wide-only/未经原publication资格的hint、Get/Wide hard fixed/read/tie或lease扩张修改前typed拒绝。Get64/extract不属于首域。 |
| SelectZero U32 → To64及真实Capture | 已合入`NativeSelectZeroWViewShape`：严格Generic U32 primary、三个Original Whole scalar输入、无flags/pseudo/独立输出；实际早写只限同primary。cohort在首次live前交同site/birth宽视图，KnownW/BridgeShape独立重读；已有shared member仍逐项核lease。min08 joint已过真实Capture/width前缀，完整事务仍失败，不把前缀转成全例阳性。 |
| direct-narrow、sign/load-primary、跨类转换 | 原子兑现真实primary、source、access/result/extend、effects/fault/observer及transport；没有真实结果不能从VOID造V128，有SCVTF/FMOV计算不能只搬definition后删计算。 |

**二、静态form在RA之前，位置选择留在原编辑期。**

`select_vector.cpp`的共享规则在转换时直接选ScalarLegacy/ScalarInsert、literal shuffle等静态form；取原请求的有效配置、feature与mode。`constraints_fpr.cpp`及`constraints_scratch.cpp`读同一form，保direct/table差异、ScalarInsert的GPR输入物化、两pool和MEM需求；不先默认再晚改并保存C×checkpoint，也不永久取所有变体最大reserve。

Known＋early的PSHUFD只通过公共纯能力提交已有literal语义，backend保控制分类；Unknown/EarlyOff/manual与既有Indexed mask均有合法受检分支。used VecFCmp/flags、Sse42额外payload及旧Indexed mask仍须补齐。home相关AES/tie/Get/Set/发表保resident、future read、foreign/live-in及原retry，不能整族关闭coalescing。有限PSHUFD不能代表M1完成。

C6主树的`NativeLocation(writer, consumer, cell, point, kind, out)`的kind是`NativeUseKind`，Value/Low32/Whole/Incoming等角色仍取实际cell；它只在同一Writer/result的编辑期借原绑定规则，不提交physical handle，不制造一个可Freeze的最终resolver。输入编辑后重绑受影响frame；`GPRCopiesFor`只借原B的copy/width/capture，Reset/替换/retry前结束借用。最终`NativeResolver`仍须locations冻结，不能为方便A而提前Freeze或放宽边界。

**共同schema须核真实输入，producer与reader采用同一含义。** SetHostGPR默认mask已在producer修为唯一实际值cell0；其它home/offset是立即数。Covered仍须零inline read及真实implicit commit，Original资格另核，未知NoBody不通行。

**普通GPR发表已扩到真实MEM源，继续补齐其余实际源合同。** 原selector共享`GPRPublicationValueControl`与checked默认规则，写真实width/offset/ClearHigh/AdjustResident、source role与PublicationValue；首写才通知原journal。Whole MEM与已有reload关系已接，MEM32保AdjustResident以保证实际Mov W；no-body Get32仍限真实无reload GPR且已证高零。FPR、fused bypass、selected-memory及未建立的GPR reload入口仍typed拒绝，不让Generic假成功进kernel。

producer与资源共用原`!coalesced && !fused_source && !residence`的adoption规则，原JitContext的pending writeback、required_backing优先级、owns_all_uses、same-key与forward/materialization寿命不改。独立`NativeReadNumericMemory`在原GPR site访问内核完整primary/secondary/shared-root输出token、actual birth、canonical slot/lease与真实CFG路径；MEM slot不能作GPR home，不能借当前publication自证。local-control MEM、fused/selected等尚未获得完整资格。真实MEM压力例已沿公共准备/编码通过；这不证明guest执行，也不授所有MEM路径adoption资格。不减少pressure、改Map或关闭原政策来补域，不新增publication plan。

**六类late scalar已有同源实际schema，后续复用这一入口。** `CheckNativeScalarInitialForm`/`CheckNativeScalarFormLocations`覆盖ZeroExtendWriteW、SignExtendMove、ZeroExtendMove、LogicalImmediate、OperandAbsolute、OperandDirect；initial与最后actual scratch共用，最终按真实绑定核位置/extension/W tie与alias，不能仅白名单或假Generic。LogicalImmediate采用kernel真实输出宽度：And的≤32位point结果用W，不能一律套definition.bits；也不能把所有窄输入或Definition角色压成W。局部失败恢复与整批失败恢复有不同时期；已改成首个真实修改才建立相应checkpoint，未改候选不保存快照，真正触及D_edit的两时期记录仍照计。

**最后形态访问已移入原SubmitNative，不再由compiler事后补写。** 它在matched placement/Get-read sweep之后，最终B fold/install与selected登记之前；FPR body变化先结束。普通纯编码修改仅在既有`native_reused` base存在时使其重建，真实state修改保原义务，即`state_changed || (forms_changed && native_reused)`；不能因LogicalImmediate/role变化让无state需求的每块都建catalog/跑fold。此移动复用原访问，不新加一遍form pass。

**三、三种身份、两种结束时刻分别表达，真正修改共同分配与运输。**

| 关系 | 权威与合同 |
| --- | --- |
| logical/raw身份 | 原id/type/per-use view/次数及OriginalArgument；raw header可以在machine def退出后继续合法退休，不据此虚构实际读写。 |
| whole reference | `ProgramDefinition.reference`只表示完整值等价；`ResolveProgramValue`沿它解析，Low32/宽度桥/同home/跨类均不自动等价。 |
| actual write与lease | `write_owner`和原Map的`shared_write`表示同site/birth的一次真实同类写；各成员actual occurrences/count/end独立，共同lease/pressure/next-use覆盖全体，短成员结束不释放仍活home，root count0不删真实输出。 |

raw最后使用决定各逻辑身份的退休；canonical materialization最后需求决定实际值与运输scratch何时可释放，两者不能用一个`last_use`冒充。共同MEM key去重不改变raw multiplicity，纯GPR可用的alias也不自动获得另一实际writer的MEM lease。完整Function仍按CFG求真正用途和lease，不按逻辑id或RPO数字窗口推断可用。

共同core还须联合fixed/read/member tie、dirty、Capture/LONG和原质量优先级，局部失败在最早受影响owner前恢复完整generic及live/稳定调度。现有`SemanticPolicyRequired`是迁移缺域，不能把这些原阳性丢掉交付。`ReplacePrimary`/Additional只是原语：双方容量/checkpoint先备齐，保真正存在的后方逻辑结果与原访问宽度，建立依赖前完成主输出替换。

原spill-use/region服务必须同时核全组actual occurrences、各raw退休预算、access/fault/width与真实consumer transport，恢复合法ownership/definition-transfer/forwarding/adoption。可借原raw traits与实际定义枚举；只在真正multi-raw spill组准备额外预算，不建N身份目录或第二E图。NoBody本身没有输出写，不能因它还保availability definition就给它definition-owned reload；另有真实preservation capture时，凭原fixed transfer/宽度/offset、真实copy到slot及路径证明保合法MEM suffix。

已合入transport分开`materialization_last_use`与raw `last_use`，并把确有fault/未来需求的`required_backing`接到实际flush决策，优先于forward/adopt/dead-flags。普通PublicationValue已有MEM资源分支；无copy不自动adopt MEM，合法forwarding不永久禁用，也不扩大backing代替最终质量。完整selected memory mode/home、flags/control/recovery与复杂局部跳转仍待补。X宽度backing保存真实W写零高位后的完整结果，不能将U32来源误读成只存四字节的slot。

**四、late编辑与copy/state/transport在同一个原事务内结束。**

原spill region先于部分coalescing及late state建立，late换源、Covered/NoBody或实际writer变化会使旧ownership失效。原`AllocationEdit`、原body checkpoint和原state借用一起延长到最后检查；不另立transaction manager，不在中途Commit来绕过copy安装限制。

| 原事务中的顺序 | 必须保住的行为 |
| --- | --- |
| 首次真实编辑前捕获 | 保存受影响实际writer/site/birth、canonical key、全组预算/end与旧lease；同key多次编辑合并动作，只借本批必要编辑记录，不全scope快照。 |
| A/B与实际early write | body、Map/fixed、copy选择及实际event同步；接受early write立即更新原按point的clobber投影，不每publication Reset再扫B。guest逻辑access、净RA clobber与真实硬件output分别受检。 |
| Copy家族结束 | 对真正受影响B一次最终catalog/fold，安装原copy/width/capture，再注册selected tuples；这是原initial fold之外的一次，未受影响B不补；body/view相关读者按真实失效边界重绑。原copy payload按首次替换move到journal，rollback无需再次分配。 |
| 最后transport失效处理 | 旧lease仍涵盖时，可保不变reads、降级为明确backing义务或删除已无物化的旧regions。必要live在整批一次更新；reload rows归并处理，修改前备齐journal。实际writer/key/birth改变或end越过旧lease，回到原owner，不在晚阶段擅自扩lease。canonical MEM且有真实preserved suffix/capture的read-elision登记RequireBacking并核原fixed/copy；只有全组实际读均独立GPR且无MEM/reload依赖才可NoMaterialization，最后verifier须独立核本批编辑请求分类。SameReads还要按原checkpoint比较actual source/view/role/点/读种类，不能仅比count/end；Numeric→Implicit可有相同聚合量。 |
| 最后证明后提交 | 最终state/copy/GPR/FPR/fixed/width/scratch/transport独立检查成功才Commit。失败恢复Map、fixed/selected/copy/width/capture、reload/obligation、body及派生借用；不是只还原指令外壳。 |

原RA已建立preservation actions；通常直接借现成payload。只有实际字段/目标或其scratch关联真的失效，才在原owner首次move-journal后重建一次，不常态再做F级扫描。删除旧def-owned region后，最终proof不能再拿该region证明capture，必须读真实preservation动作与路径。跨真实writer的MEM需求不能仅靠同canonical key、LiveEnd或旧count证明安全。

Stage返回NoChanges时不产生pending事务；调用方仍须fresh proof再普通Commit，不能以D=0跳过核验或误调Submit。MachineUsesUnchanged只比较实际输入，不替代output、lease、fault或backing证明。05bc/min08的retained_rows=0已定位为fixture未请求不变压力MEM key；2b3c/min09从真实RA选有reload的MEM值，在两轮显式请求SameReads并逐字段核原行，已过bad NoMaterialization拒绝→完整rollback→good RequireBacking重证。保原压力、产品政策和全部断言，不把后来通过回填min08。

原`Edits::EvaluateTransport(false/true)`分别在Stage前分类、Stage后独立重证，短期D/J行各自重建，不传递“已通过”证据。按actual site合并完整输出token/birth核对；改动body与原checkpoint只比较一次。存在NoMaterialization需求时整批一次访问actual body uses和真实terminal roots；否则只看改动body的当前输入。Stage后按原stored key一次借reload point storage，不沿新REF藏旧行；真实代价为point slots加rows，而非仅rows。零D只跳过此分类服务，完整fresh proof仍执行。

**五、state只重算真实依赖，独立检查读最终事实。**

原byte/label/CFG matcher、WidthTransfer/Meet由checked/native共享；canonical reference、raw_view、实际early write分开。`CohortRequired{domain,index}`须兑现原阳性的预RA机器合同，不冒充`ContractRejected`或静默关政策。保`!reused`在catalog前的早退；真正reused域的raw resident、observer/fault、entry/join/loop与Φ照计。

C6主树已把逐Copy扫描publications的logical资格前移到原按点首访，以固定home临时`last_guest_set`核严格(read,pub)开区间，partial和非候选Set同样更新；退出旧`P_copy×P_pub`那一段循环。它只证明logical窗口，不能替代actual clobber、fault/capture或byte/extension证明。observer可合法保Get capture：home21保capture＋W-copy，home23保capture但省pub copy，两域的后续proof/Ready已过。新Get32→U16 NarrowRead例也必须保原preserved Get捕获，并使用canonical captured GPR的Whole读取；省Get body不等于全局LowView@22，该全局域仍未获此例证明。不放宽AssignGPRLowView的same-home guard求过。

**Function入口宽度只有一个native求解者。** Preclose只捕获原SSA incoming基值、entry contract和terminal，删除semantic `entry_state`的transfer/solve与30B查询。原`BuildNativeFunctionWidthFacts`按需求解：residence已用则metadata复用；residence没用则metadata首次求必需解，不能声称该分支也省掉一次求解。Block不付Function solver。

原首写callback的栈上`NativeWidthInputs`核opcode/type、完整Original source/view、guest home/逻辑offset、LoadImm、canonical-W与同源helper clobber；真实late逻辑transfer输入变化返回`StatePolicyRequired/EntryWidth/point`并进入原rollback，不只重算width后继续借旧copy/selected。仅encoding/role/Covered改变且逻辑输入保持时可复用。`FinishNativeResidence`仍在旧EndNative边界释放matcher/window/catalog等，只有原B×30 facts及必需借用保到独立proof/Commit之后，回填原edges、unit_state_edges、compound_clear_allowed与Function entries，再EndNative，先于locations冻结。下一flags的ClearCompound须提前只读同一解和同一carry/terminal谓词，不把默认metadata=false当policy，也不挪到proof后改form。

独立GPR/fixed/width检查沿原`VerifyPublication`、`ReproveWidthChainBridge`、`ReproveLow32Copy`、`VerifyHostRead`和common `CheckFixedConstraints`共享算法，只替换checked/native读口。它核最终actual form/operand/location/effects、原raw资格及Original closure、实际machine lease、copy/capture三域，不接受候选、Map shared_write或state producer自签通过。

| 容易再次混淆的边界 | 检查必须读取 |
| --- | --- |
| logical id与actual birth | native借原`ProgramDefinition`的site/birth，包含retimed pair、shared member和primary替换。C6已修原Recolor三处读取，preserved residence仍从transfer开始；checked语义读者保原合同，不增加N owner目录。实际枚举成本保留，pair fixture结束协议已修且33e1组件例通过，不转为完整生产资格。 |
| availability def与真正写 | Covered Get可保结果/site/birth而无capture指令；是否写、写宽度及输出scratch取实际form和body。Get/Set的`Effects.gpr_reads/writes`包含logical guest access，不能充当全部硬件读写。 |
| raw资格与物理需求 | 原次数/OriginalArgument闭合保pseudo/alias/terminal义务；实际uses/defs与原machine live给物理lease，不以owner UseCount或raw-only header自证。 |
| 当前值与路径证明 | Covered read按每个实际consumer的位置/copy/宽度/严格publication与最新view判断；Function沿真实CFG前驱及return证明来源，不按RPO窗口推断。普通同点读先写后，terminal读保其实际在写后的位置。 |
| 最终scratch | 实际defs、绑定角色/输入和实际preservation/copy/reload共同计价；不对所有MapMEM availability defs盲加输出/reload，也不以旧HasSpillReload偶然折扣掩盖NoBody。原flags-only/严格余量及InvalidProgram域不降级。 |

最终发表独立检查已接入原GPR site访问，替换旧输入绑定片段，不新增全域pass。ordinary Whole且canonical GPR home相符、无preserved覆盖时可借原RA/transport权威；Value/Low32/preserved必须证明严格早于publication的实际来源，不能用本次发表自证。32位同home省Mov W还须从最终实际写/捕获/CFG核当前高32为零；早期birth是W或SSA低32仍活不构成充分证明。`NativeHomeHighZeroBefore`只服务确需省写的查询；其真实路径/定义访问计成本，不能扩大为每值通用追溯。

不支持form/非法程序必须typed失败；只有真正`ContractRejected`进入原质量retry。所有proof相关编辑均先失效、更新后再检查，proof后不得偷改程序。最终actual scratch核验使用编辑期NativeLocation，在locations冻结前完成；支持域内真正容量不足与InvalidProgram/UnsupportedForm分开。共享形态检查只是共同schema入口，不替代独立state、lease与资源检查，不新增Passed表或certificate层。

**六、同一个准备owner在旧Describe之前接通生产链。**

原Storage已沿公共Block、Function及Function组入口接入同Store支持域，完整SSA、实际pool/config、static family和entry/terminal等必要语义输入先闭合。C6 min08中的生产例只证明当时所列准备/编码/退役范围；此后默认切换、37c8基本guest、e0d6普通方向及8cda当前检查按7.1分别记录，不再把“尚未执行guest”写成后续源码的当前结论。

**原支持域现由唯一`kNativePreparationEnabled=true`默认接管；完整负载native覆盖率仍未建立。** flags-token、local-control及限定location/RSB/call-return已有真实接线，不能继续统称未迁；hoist、used FCmp、Sse42、Indexed-direct、复杂selected memory及未闭合hard/cohort域仍在破坏前typed选择原合法入口，不能把缺域清单固化为缩水的性能范围。关闭后缺合同则整scope失败，不回Inst或追加旧Complete。原真正FPR质量ContractRejected保独立预算；可选metadata晚拒绝只有按[5.3.12.10](#native-cohort-owner-rebuild)完整恢复后才可重建一次，Invalid/Unsupported不能借预算。c984原controller/joint及红owner保历史身份，dd0当时所列原例通过，当前资格见7.1；终态scratch输出开始后两种retry均禁止。默认支持域已在原调用点切换，semantic预产/Bind/兼容桥与旧服务绕行见5.3.12.15～17；不再新建准备owner。

```text
原Storage选择支持域
  → 最后semantic闭合/全组转交
  → 静态form与可提前兑现的B
  → 一份machine live/initial → 原Block或Function controller
  → 原编辑期A/state/位置相关B及原事务
  → 批量失效处理/StageNativeSpillTransport（或NoChanges）/fresh live
  → 独立GPR/FPR/transport，三项通过才授权终态scratch
  → 末次独立scratch核验并覆写原backing（此后失败结束请求）
  → 原transport Submit/Commit
  → 同一native宽度解完成原entry/edge metadata → EndNative
  → EndTargetPlanning（locations冻结）
  → final NativeResolver／唯一TakePreparedScratchInputs
  → 全体body：顺序读最终profile → Decode → BuildNativeCurrent
                 → StoreInstructionState → PreparePhysical(handle)
  → PreflightNativeControls
  → 全体body：FreezePhysical（缓存handle）
  → FreezeNativeControls → stream.FreezeNative → PhysicalReady
  → 真实kernel、必要重发、Flush、发布、最后结束
```

**首个body Freeze前，完成整个Block/Function scope的actual schema/resource读取与controls预检。** `program.Definition`拒绝committed前缀，不能逐条“describe→Freeze→再describe后继”。资源payload在ResetCurrent前进入原InstructionState库；成功Prepare仍保virtual cells，后继可读其定义。退役的`pseudo_demand` word复用为该handle，由`PreparedResource()`读，不新增N handle表或另一资源manager；prepared后禁止PseudoDemand、新checkpoint及普通form/source/effects编辑。

重复Prepare只匹配同一不可变位置/角色快照，失败保prepared/cache；默认MAX保原handle，显式冲突在写cell前拒绝。终态scratch授权之前，尚未commit的编辑可用先前checkpoint恢复，并重做失效的proof/profile/resource准备；终态输出一旦开始即使未Commit也不重试。controls先核kind/id/type/raw-view/consumer/Whole角色，再以同一冻结快照实际替换。Freeze仍逐cell二次resolve；首个实际替换后意外失败须废弃整个scope/prefix，不回semantic或普通RA retry。原AllocationEdit Commit只提交Map/运输，Controller Finish只释放分配输入，二者均不授PhysicalReady。

public/retained/未支持域在semantic关闭前选择原合法入口，转换后不回Inst。initial Complete/CoreAllocated不等于Ready；true→false共读稳定operand，重建动态编码状态。资源容量预检、两遍body访问及preflight/freeze双resolve仍是当前成本，须按真实读者改造，不能简单删除容量或失败边界。C6轮红owner及当时guest未执行保持历史身份，后续基本执行、普通计时和当前检查以7.1为准。

退役的Get32宽桥仅保所需raw header与源token/view；后继不可重读已committed定义。实际Freeze把退役桥cell0保存为**无Original/Output/Implicit标志的Label**，其它cells清退，供后继Header读口核对。virtual原始源仍需Original。此混合前缀合同已合入并由Get32组件例检查，不另建N前缀表，也不等于机器码已执行。

性能交付要求旧Describe/Finish、语义质量投影、Complete同义发现/重建、每次emit的semantic binder及无人消费的初始化/复制/清理真实退出；不能先跑完整旧链再转换。M1须广泛减少出生/首次解释，M4随旧数据退出减少真实触达，有限shared-write/PSHUFD不承担整线提速承诺。

**七、迁移中尚未退出的算法成本；不得作为正确性合同固化。**

当前target实现仍含每候选窗口`Σ(W+E_window)`及原Recolor的全域Map/width/low32/selected/reload检查，最坏保留`P×N`等工作；独立raw闭合缺少原精确span时也可能有`Q×E`扫描，不能写成线性。已合入native checker还保每个owner请求枚举真实defs/site的Q×D/Q×N、逐Covered-read的Q_read×E_actual、Map/REF链总长度；CFG/return的B²是每次路径查询的最坏关系访问，路径项应另记T_path×B²＋Σ_q body_touched(q)，每查询还用B级scratch。T_path受相关consumer/capture和递归copy查询驱动，不把它与Q_read×E_actual当作互不相依的总账相加。它是正确性迁移，不是新性能上限证明。实际definitions枚举替代错误id-as-birth是正确性修复，须计真实访问。最终B catalog/fold及排序、projection插入移位、member聚合、C×checkpoint、preservation重建、CFG Φ、双resolve和整scope retry均计净账。transport还计raw组预算、本批编辑记录/排序、必要live更新、旧reload点行及journal、旧新copy/action payload同时存活与真正失效时的重建。

静态B也须交净账：以D_B个真实候选保存原quality行/ordinal和body checkpoint，复用一次全actual-use闭合与排序查找，不逐bridge重扫E；候选Get/Wide的hard行检查同样批量完成。预RA输入修订和inactive行仍由所有真实读者消费，不能只让selector跳过。资源阶段已消费末次独立profile输出，退出其整轮重发现；initial/state/final推导、真实resource和Prepare/Freeze两次resolve仍计费，后续在同一原owner内按真实依赖收敛，不另造缓存证书。

已退出两段同义服务：preclose semantic入口宽度求解/30B查询，以及transport逐请求Classify/ReadsUnchanged/IndependentReads/HasRealBacking和reload全域查询。后者改为Stage前后各一批，实际uses/terminal及reload只在对应需求存在时访问；两次独立推导都收费。preserved backing仍对同transfer的每请求查原action，保`Σ_t D_t×A_t`；原Capture去重、Site查找、REF深度、live、Stage journal和全部fresh proof均未消失，不能把整链写成线性。

固定行插入、Recolor、definition/raw/CFG发现、profile/双resolve及scope retry都是实现工作，必须逐项给出真实触发域、写者/读者和替代算法；只登记最坏复杂度不构成交付。当前A已经退出Covered Get的一段全scope actual-use重发现，但全局lease/fault/capture及RawDirect未由此消失。完整身份与两scope合同保持、全部新增费用扣除后，才由普通方向决定扩展。

SelectZero的新增primary纯schema只核固定槽与实际三读，不为每次查询再扫全部shared defs；但同producer多view时，原`CheckVirtual`、`TransferDefinitionTo`的重复id核对/逐次reserve及独立definition/raw reader仍可能形成`Σ D_source²`重复。新合同只修真实写与use-def交付，Extra/shared-definition backing和这些原访问成本照计，不能据固定大小schema称整个cohort已线性。

原scope重建若触发K次，仍是`(K+1)×(N+E+P)`加assignment/CFG等，只能作为明确受界的困难回退。先把可由稳定资格决定的B前置、按真实B合并fold、复用原事务与原求解器；剩余乘积工作须随整个正常服务收敛，不再批准owner/source/bitmap/getter微包来代替主线。完整普通服务应在原按home/site的state/quality访问中批量处理相关义务，优先消费原完整稀疏raw见证、共享同一scope的实际定义/固定写准备；独立性要求从实际程序推导义务，不要求每请求全扫scope。若普通域仍依赖上述乘积查询，它只能先获得迁移组件资格，不能当作最终性能实现交付；收敛工作随整线推进。表5.8继续区分保留、退出与新增，不用文件数、少pass或组件通过数保证两倍门。

相依组统一构建＋最少受影响既有语义；完整旧服务退出后一次整体方向，稳定后集中原6.3/6.4。中途生成质量可变，不逐helper增加QoR/PMU/FEX矩阵；最终不降低生成/运行质量、资源满足且Block/Function各≤2×同轮fresh固定FEX才完成。**目前没有已证明可兑现两倍门的完整组合。**

<a id="native-flags-next-service"></a>

##### 5.3.12.7 下一完整支持域：flags与真实控制

**本地合流、Save原点与branch/backedge/PFAF/ZeroTarget沿原owner完成整合，历史322构建失败保原身份；dd0所列原例通过、支持域默认已开。** normal覆盖、全组合与最终质量仍未授信。Current/193 kernel不再重迁，不把已接形态、组件after或防错guard当运行/性能资格，准确状态见7.1。

**一、先从实际kernel确定机器use/def，再准备live/initial/RA。**

| 形态 | 必须交出的实际程序 | raw与资源义务 |
| --- | --- | --- |
| AddCarry/窄整数改形 | ADC/SBB等仍有真实数值结果；按原matcher交实际输入、CF准备与真实结果birth | 原flags/clear/宽度语义保留；不能因相关死链无结果就把AddCarry一并退掉 |
| SubCompareImmediate/Value | CMP只写host NZCV，保真实输入和machine body，**没有数值Result/extra def或输出lease**；原SSA header不充当机器写 | 原numeric-only预算及实际Numeric/Implicit/Terminal/REF/joint闭合分别成立；原CF反相、额外NZCV在观察前被覆盖的窗口独立重证。schema/质量/proof已接，受影响public-owner与checked检查通过，不扩为所有control资格 |
| covered整数flags链 | 没有machine body/data reads/defs，原未覆盖head留作Logical；只在实际换槽时存必要原operand | 有Original值输入就保Tick及MEM/raw退休，不套无值输入Disabled；无值输入的精确disabled形态另核 |
| ConstantFlagsOnly | 共享原合法性谓词；仅原LogicalZeroCVA、非空C/V/AF清零子集、无FlagValueOutputs、effective.set=None、无真实data消费者且非hoist等完整条件成立时，预RA退data inputs/def | 保原Logical head、count/view、Tick与CommitFlagOutputs。精确当前intrinsic scratch为0/0；未来observer/CF merge在其原点收费，MEM/transport另计，不由raw use=0猜资格 |
| CarryRaw/Packed与LocalCondition命中链 | 真实test保Tst/merge等flag本体；仅matched predicate/combine等退掉原本没有写出的BOOL def/use，普通需物化的BOOL保真实结果 | control依赖必须显式交付到真实Select/terminal；不能用假Numeric/Implicit或假GPR def维持旧live，也不能永久退回CSET+CBZ冒充原质量 |
| SetCarry/SetOverflow | ScalarRead(arg0)+Bfi：实际readmask为1、无数值结果；InvertCarry才无data输入 | 保guest CF/OF访问及原intrinsic 2和实际MEM/normalization预算，不能因同为flags opcode漏掉RDRAND等数据读 |

AddCarry/CMP所选kernel若只消费raw值，matcher必须同源核原operand tag/operation：仅等价的unary Plus可进入该形态；原shift/ext/Minus不能被丢弃，未命中保原合法generic，不拒整个unit。保存原tag本身不是证明，也不能把此正确性修正折算为compile收益。

最后semantic准备继续用原UseFacts和consumer/integer/flags matcher；whole-Function RA demand、本B emission demand、packed BranchOnly marker存在与intrinsic输出限制分开（legacy marker按OR保留，不等于每个pseudo均为branch-only）。checked/native共读forward carry/zero/narrow/compare、raw PSTATE prefix和effective demand纯规则；branch/PFAF覆盖与原PhysicalScalarDemand同源，不另建flags SSA图或第二CFG固定点。Save/Branch raw锚不是实际数值读，GetFlags实际读All；host NZCV clobber也不能从logical FlagAccess直接推断。

Original head若本来就位于actual mask之外，直接借作Logical，避免为每条Save/Boolean/covered链另分配raw行。只有实际覆盖原槽才存稀疏token/view；raw总数、numeric-only数和actual machine uses各有读者，不以其中之一替代其它域。foreign sole-use LoadImm的consumer token可借原稀疏recipe/空槽作**定位提示**；独立reader仍核完整identity/view、global count、scope、原RHS/立即数及真实CMP窗口，不用Q次全scope寻user，也不把提示当通过证明。

原ProveDeadUses若递归认可其它dead arithmetic消费者，必须交完整accepted dead closure并让那些消费者真正退出machine body/def；只Cover零链而让后继继续actual读被退def，会留下ghost依赖。沿成功证明路径用原稀疏worklist闭合，不能逐root全扫N，也不能补availability假def掩盖缺口。

**二、机器图闭合后直接生产实际约束，不能只退Result。**

预RA现行相序是：最后semantic选择/原全组hardguard→同槽转换记录C/P→实际metadata/constant/W-view/Get32编辑与body journal→fresh machine live→原Descriptor直接生产actual行及Explicit实际重资格。旧有限D修订read/view/hint/preference/def和转换后flags再解释已经退出；先退Result再借旧语义行兜底不成立。新增metadata可逆边界见5.3.12.10，不能据共享一份live宣称全流程只求一次。

在全组semantic破坏前检查原flags接收域及硬fixed/read/tie/preserved、复杂Copy与source合法性；不支持组合在Consume前选原入口。actual形态完成后，read/view/consumer/hint/guest preference及Derived由当前body/use直接生产，不先制造失活soft行或UINTMAX假hint，不复制GPRInputs。Explicit硬行仍按实际定义/terminal/REF/capture窗口重资格，真正缺域不能靠消费后Unsupported或扩大retry隐去。

无候选Q时退出该候选服务；有Q时批量实际Use/raw/terminal/REF闭合，不逐候选重扫E。没有N Passed/owner目录，仍付候选排序、membership、R/A聚合与峰值capacity。无cohort时journal在原静态边界结束；有cohort仅保真实static body和Explicit undo，已无flags/Get32派生行恢复。fresh machine live及原完整controller仍必须运行。

**三、条件链必须交到真实控制消费者，才允许删数值结果。**

**窄控制引用协议已集成并获限定原owner检查。** body条件arg0借现Label-kind cell保完整NativeReference、canonical身份与原RawView；它没有Numeric/Implicit/Original角色，原被替换Value occurrence只移交一次Logical退休行。Current的`LocalConditionKey`只借此精确槽，普通参数仍走严格ValueAt/ReadPhysical。terminal在`PhysicalIf`末尾追加默认false的`local_condition`，保旧四字段聚合含义；标记为true时cond的NONE仅表示没有数值位置，仍保原token/type/view/consumer/point。不能将其伪装DISCARD或放宽一般NONE/Map guard，Switch/dynamic/call-return仍各守真实数据域。

控制引用与退data def分别判定。主树已支持always-key LocalCondSet、满足原FLAGS_REGS/PSTATE和单flag条件的TestFlags/TestNotFlags、完整Carry macro；Carry登记原combine key。LocalParity仍需真实W结果和lease，FCmp部分分支也有实际结果/temp，不能整族退def。当前已集成的`CondSetControl`只覆盖原Last＋唯一安全terminal、无真实data消费者的精确域：有同ID已发raw key时直接消费，不能先LoadNZCV覆盖它；无key才沿原PrepareFlagRead/Merge/Flush/LoadNZCV取得条件。选中形态无数值Result，却保完整原scratch与raw退休。同一已集成实现要求native Generic CondSet/TestZero/TestNotZero在每个合法lazy分支实际写出Result。hard撤回为Generic后，若已有同ID且独立证明有效的raw key，必须从该真实condition执行CSET，不能读尚未发表的x26；无key才用原合法数据路径。这项Generic修正已进入395；本轮四变体不代替尚未完工的raw-key metadata端点/hard恢复证明。数值Select/Goto/If只能读实际数值，显式Label/local If才能取key。不能用map推断来跳过已声明的data写入。

preclose沿原成功consumer recipe的first与replacement_mask bit0交付真实key，转换时读取原first得到完整token，不新增每控制输入的prefix寻源。有限D和硬域准入共用同源`NativeRetiredLocalConditionHeader`，区分真无data源与LocalParity临时值。转换/Freeze核稳定header、scope/B、token和view，不能回死semantic定义，也不把静态形态或Label当证明。

actual terminal访问只跳过精确local If的数值用，flags原raw闭合和kernel退休仍借**同一棵PhysicalTerminal树**看到原If/Switch occurrence一次。原Header raw总数不混入后造dynamic/call-return/hoist metadata用途。当前公共spill/ownership读者已有所需actual访问，未发现必须新增raw-terminal Access的消费者；先用现树的纯raw模式，不建第二树、E表或新的公共Access。

原local_conditions沿同一key消费；key存在仅表示登记关系，不证明NZCV仍有效。独立reader核raw closure、site/birth/location、PSTATE/CF极性与真实consumer窗口。**按实际终结kernel区分祖先：Condition的LoadNZCV可能破坏原raw条件；Switch则在比较前保存物理NZCV，每个命中子树入口先LDR/MSR恢复，必须继承父安全性，不能无条件判为clobber，也不能解除已有Condition屏障。** 该修正已入3bdd并通过原Switch用例230断言。已集成lazy包还收紧Select保留谓词：仅显式local-key CSEL/CSET路径可宣称保持，Direct可能MSR、numeric Boolean可能CMP，form名本身不是证明。

legacy raw Carry的C|Z修正已集成：仅发布CF的Save不能借producer的host Z执行HI/LS；checked/native共用原Save真实发布C|Z的纯匹配，最终reader另核merged demand及实际窗口。仅CF保原合法Packed carry，不拒整个unit。当前public owner含C|Z raw与CF-only Packed限定阳性，不转授其它metadata端点。

integer-branch、backedge和PFAF仍须兑现原机器端点，不能只复制metadata。窄branch-only SubDeadNarrow8/16保UXTB/UXTH、原CMP及CF语义；退SSA Result后，归一化需要的真实临时值转入原scratch预算，不直接套普通SubCompare schema。Backedge省掉的VOID StoreUniform/Save/Clear/Invert若仍有原Value occurrence就保Covered＋Tick/raw退休；真正无输入且闭合的指令才可Disabled。PFAF匹配须保原unary Plus/tag、PF/AF来源、requested/polarity、ABI与XPOOL限制；HostGPR重读沿原NativeGPRProofView/NativeValueInHome核真实copy、CFG、home窗口及numeric defs/clobber/preservation，不能仅凭GuestWrite mask或另造home证书。退data后不回ScalarRead；若原backedge mismatch回退仅用MRS/BFI和non-S packing而保持NZCV，可沿原合法分支消费已独立证明的key，不强制新增MSR恢复。map存在、missing-key guard、PANIC或optional空值均不能替代完整source/window证明。

**四、质量选择、独立proof和资源沿原owner闭合。**

ClearCompound在final form前只读同一native width解、原SSA incoming与CFG/terminal carry谓词；无相关依赖不强制Function求解，Block不付Function solver。最终metadata共用谓词，不能拿未完成的默认false当政策。late token借真实NativeLocation、publication suffix和preserved原点，不用所有token homes=0代替原质量。

在原GPR site访问累计临时needs_flags，结合真实terminal/metadata保守判断；只对确实需要的B调用一次完整独立reader，不增加一遍全scope筛选。未知形态/observer保守纳入，不能先认领跳过比例。reader读actual masks/forms/use/def、raw occurrence、实际site/birth/location和必要entry事实；entry事实只借原解，先于EndNative结束，不能自签header/events或再做一次solve。

C6 U32 bridge与covered flags可能同shape，Header不是语义类别或证书。保原bridge的Label0与prefix读口，按原source/actual publication关系分别重证，不能靠伪transport位区分。所有body仍virtual时用同源effective demand产原ResourceRecord，之后pseudo word才变handle；静态/prefix检查不得把prepared handle解成pseudo需求。相应scratch、raw Tick、backing、资源/Freeze及退休必须同组接通。

原HasNumericUses实际读GetUses(false)总次数：实际Numeric为0而Save Logical rawtotal为1，仍不能直接授DISCARD；原flags-only MEM另核raw、真正Map/reload/preservation/required_backing和canonical寿命。固定约束沿原installer/FixedReady消费，退出新增的逐候选×F查找；原F安装/验证费用保留，不让空owner吞非空inputs。

主树同Store链已接本地Goto/BindLabel、同owner metadata恢复及actual直产，原支持域默认true。dd0所列原例通过，normal覆盖和全组合仍见7.1；cee0/33da等保原source身份。CondSetControl可选hard冲突仍在转换前恢复真实Generic；原Last/后续Label、窗口、metadata journal与独立证明保持。

<a id="native-memory-next-service"></a>

##### 5.3.12.8 紧接普通内存：交真实控制字，迁全部逻辑读者

**普通TSO/Extend与stage01～03已集成。** 9b和33da的原owner整例分别1861、1989断言通过，包含已接lane/load路径；它们不授新增WB或全部fault/guest质量。六个普通内存族继续复用native、select_memory、constraints、physical、state与verify。LoadExtend描述符在原Cell3就地成为PhysicalMemoryControl，全部逻辑读者经同源逆投影；原fault/TSO/RCpc、真实资源与重发/退休保持。

| 决定 | 唯一生产者与消费合同 |
| --- | --- |
| 普通访问形态 | 原memory family纯schema同源供checked与native；保access bytes、结果宽度、signed/unsigned及extend32清高，不从结果类型反猜访问宽度 |
| LoadExtend控制字 | 同源EncodeLoadExtendMemoryControl在转换尾就地写原Cell3，form明确LoadExtend；不加第二raw Imm或完整memory表。逻辑读者经同源NativeLoadExtendDescriptor逆投影原语义，不能把控制字重新当4bit descriptor解码 |
| 全部真实读者 | constraints初始需求、state宽度/源规则、verify_gpr与flags matcher读相同逻辑投影；physical资源核实际form/role。checked与native不各维护一份sign/width判断 |
| fault observer | 六个普通load/store族的原meta带MayFault，公共FlagAccess因此读取All；schema必须核同源真实访问，不能硬要求flags_read=None或抹掉观察义务来获准入。guest flags观察与host NZCV实际clobber分别证明 |
| TSO/fault/transport | 原TSO/RCpc、unaligned、barrier及fault点照走；plain Extend保原MEM forwarding。TSO load原无地址候选、TSO store原仅value候选，不能顺带打开地址重算或放宽generic资源adoption |
| 关闭与恢复 | 当前普通族已接真实Virtual/Ready/重发资源并缩对应粗门；fused、复杂Map相关EA/direct-narrow、跨类/FPR等另按原合同迁移；缺域不计作最终降低compile工作量 |

这种同源投影只复用原槽与纯规则，不是另立memory描述层。转换、逆投影、资源核验和原fault/barrier仍有实际费用；扩可编码域是替代旧服务的必要条件，不凭接口数或通过组件数认领提速。逐源归属见[结构表5.1.6](middle-pipeline-file-layout-2026-10-28.md#516-普通内存接收域的文件归属)。

<a id="native-memory-domain-status"></a>

**内存状态按实际合同区分；已合实现、限定原例与未测组合分别记录。**

| 域 | 当前实现与下一义务 |
| --- | --- |
| stage01相邻EA/Zero/narrow/lane store | 已合且随9b/33da原owner整例通过。Offset/Recomputed、WZR/XZR、窄load重定时及V128源V32/V64边视图在首live前形成真实程序。 |
| stage02整数FPR lane、清高与U64 load | 已合，所含原owner路径获同版限定检查。partial lane、真实FMOV-D/EOR-Q和LDR D写由actual FPR/state/resource/kernel共同消费，raw commit/Tick保留。 |
| stage03 U32 load→ZeroExtend64→低lane | 已合并获上述owner检查。LDR S在access实际写FPR，extension/低高发表保raw身份；不虚构退休整数值的allocation，不顺带放开其它宽cohort。 |
| stage04 Pre/Post写回 | actual extra SSA、固定读/version与JIT Definition读取保持；WB base直接读actual Cell1，窄Store读actual Cell3，raw-only地址matcher不读机器槽。dd0 owner整例含原Pre/Post、两块Function和metadata恢复路径通过；不授全部压力/P/MEM组合。 |
| stage05 RCpc early alignment/backedge | 早SSA producer/reader已由stage06同组删除；checked/Unknown合法HomeAlignment保留。原unknown Generic load的Cell3真实Output允许表示隐式0，未放松产品guard。 |
| stage06最终位置alignment | 已合入原两个晚resource循环，从真实GPR/capture/spill P数值事实分别生产/独立复核，四目标与所列owner/NarrowRead通过；两次fold/P读写仍收费，不代表全P/MEM组合已测。见5.3.12.16。 |
| hoist、used浮点flags、Sse42等 | 按实际默认请求及原精确typed门处理；VecFCmpMask不等于VecFCmp。atomic/MemoryCopy/Local、AddPhi等保持原范围，不按opcode总数扩大队列。 |

**WB实际固定读与输出version修正已落地。** Definition位置可正确返回GPR但pinned=false；final reader改由实际`FixedGPRDefinition`证明原full-X、owner/end/last_use/version/elided及无transfer/capture等合同，仍核同home/no-reload、actual use/locality、canonical lease、数据别名和完整quartet。selected JIT的Definition地址读取真实X位置；输出version=true。此前initial失败已修实际读口；不能回滚这些合同或用Map/home强制通过。

WB的Post旧semantic Get末端与actual access不同；旧一点Derived缩窗journal、Origin包装和声明/调用在merge90一起退役。现在直接由actual Get/access/use端点生产Derived，实际纯固定定义reader、source gate与quartet证明继续保留。terminal/foreign旧base和不支持Copy在source时间精确拒绝，不把真实缺域转成消费后的语义回退。

stage06已在capture/reload/transport和实际写点确定后独立重算，保ForceChecks、rcpc_fault_emulate、16B与normal/backedge语义；stage05早事实或旧Get home不能替代实际值。真实P payload、两次状态fold及生命周期见5.3.12.16，checked/Unknown合法HomeAlignment继续。

共同规则保持：Operand(Value/Imm)允许None或Plus＋empty right且无shift/ext；EA用真实Result；边Type/RawView与canonical definition区分；extension精确目标宽度与signed窄load写X由独立reader核。零literal仅在所有raw occurrences都是真encoded-zero时退役。所有source邻居/用途、roster/sort、逆投影与独立proof继续收费，不另造memory表或N pass。

证据：[33da历史原组](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-coalescer-minimum-01/execution.json)、[历史cee0 actual输入构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-actual-input-build-04/build.json)、[历史cee0三例/四失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-actual-input-minimum-01/execution.json)及[历史cee0失败日志](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-actual-input-minimum-01/native-public-owner.stderr)；[stage05原外部冻结记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-memory-access-service-20261101/stage05-rcpc-actual-alignment/receipt.json)保原时态，后续stage06与默认接管见[默认接管检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-default-checkpoint.json)。逐源责任见[结构5.1.9](middle-pipeline-file-layout-2026-10-28.md#native-memory-consumer-layout)。

<a id="native-local-control-service"></a>

##### 5.3.12.9 本地Goto/BindLabel：实际路径、宽度与flags必须共同合流

**本节相依组已整合；33da原controller295/fixed457/owner1989三例通过，当前新增WB仍失败。** token/实际writer、state/width、SaveResult/flags join及metadata按下面合同执行；原同B、唯一真实绑定、严格前向Goto/NotGoto协议保持。历史宽度/state证据仍归原source，最终质量与默认替代未完成。

| 合同 | 原owner中的真实工作 |
| --- | --- |
| 控制token与数据 | Goto/NotGoto只有实际数值条件才计一个data use；显式key不计GPR use，BindLabel不计data use/def。保完整u32身份、Store/cookie和原RawView；Goto原条件Value occurrence仍按原raw预算退休，BindLabel→Goto是control身份边，独立核计数/唯一绑定，不进普通data ProgramRawUse/Logical MEM退休，也不经Program.Definition或伪DISCARD。VOID和保留U64/V128等合法类型只是control header元数据，不能因type虚构GPR/FPR结果、REF或lease。原Bind getter已有Label读取能力，不重迁codec。 |
| actual路径与位置 | 原GPR逆向query遇Bind先拆taken/fallthrough，再分别处理其prefix；taken包含Goto本点的prefix/scratch/capture/clobber，Bind前prefix只在fallthrough执行。原定义、MEM/reload、capture必须支配真实使用；publication的最新区间不能代替被跳过路径上的实际值。partial/offset沿原ByteOf规则逐真实byte追踪，保BFI/UBFX偏移和清高语义；不能用当前publication或被跳过MovW作种子。numeric Boolean另核原HasBooleanResult、真实primary/view/birth及实际数据物化，不凭form或local key推0/1值。 |
| native state fold | 在同一原body访问处理Goto/Bind。Goto在实际prefix后快照活跃copy的version/home/width/extension，Bind的prefix仅fallthrough执行，taken保Goto快照；只取共同实际副本，logical byte相等或同birth不证明真实home。prefix覆盖home时，point>0的旧copy止于point−1；合法point0 capture仍执行，原初始width run原位更新（含unknown），不产生unsigned −1或重叠first0行。新copy publication=point且只供严格后继查询，当前点读原preservation descriptor；不在Bind伪发表写。BeginNative借真实inputs/live/Writer/resources/config与prepared UnitScratch，End结束借用；独立physical proof另核路径。 |
| Function宽度 | 保原B×30入口facts及同一CFG solver；真实Goto才提升为多incoming Meet摘要，普通块保unary快路。实际发表所依赖的Get在定义点捕获；local捕获归入transfer，foreign Get只为实际Q_get保来源B/Get点表达式及稀疏依赖。已在原工作队列加入foreign变化通知：来源Get改变而来源B exit未变时仍唤醒消费者。不建N历史、第二CFG或每轮重放IR。独立oracle逐真实路径求具体Get事实，不借被测摘要生成expected；原local/foreign独立oracle在c984定向通过，见7.1；不据此授全部capture/foreign域资格。 |
| flags pending/token | 原FlagFlow/SSA Finish只解决逻辑需求，不规范化physical pending。原L个label保存Goto真实flags表示及当点scratch；Bind相容直连，异构入边用原Merge/Flush汇合，保有效raw NZCV，不盲MSR。原唯一key map可附actual birth清除fallthrough-only key，但birth/map命中不证明跨join有效性；独立reader重核source/mask/polarity与NZCV窗口。所有effects、scratch、fault顺序在首次live前交付，不在kernel临时补未预算动作。 |

**legacy Save必须保真实发表原点。** producer在Goto前、Save只在fallthrough时，producer提前commit会污染taken旧flags，尤其AF不能靠Bind清状态补救。intrinsic FlagOutputs仍属producer；legacy延迟部分由原Save点的`SaveResult`实际形态计算，head只保Logical raw退休，真实result/必要原operands进实际参数。ADC/SBB由result和原左右操作数恢复carry/borrow，不读Save时当前CF；逻辑CV为真实常量0，不造数值定义。原ScalarRecipe/NativeExtra仅稀疏记录emission demand override，raw packed需求不改；实际retains、目的地、scratch、Commit及资源共同读effective demand，prepared后的handle不可解作需求。原resultless/Covered匹配须保住延迟Save真正要读的值。形态、RawPstate前向label Meet、schema/scratch和独立reader已接线；实际ADC/SBB执行SECTION也已合入并随b6fd原owner运行，但该总例仍失败，整组资格待闭合。此修复会增加真实live、Params和scratch，可能增加capture/spill/reload；不是最终生成质量不变的证明。

**实际use变化必须在原owner开放前兑现fixed合同。** SaveResult/ZeroTarget延长result/operands/terminal源；当前原Describe从actual definitions/live直接生产Derived，仅对原Explicit调用实际Requalify和ordinal journal，不再重资格旧semantic Derived。REF/capture/fixed_end/allocation_begin与实际消费者保持，c984旧227及后续457保各自资格；5.3.12.13的三类真正残余精确typed拒绝，不概括全部REF缺失或永久关闭fixed。真正晚编辑仍须维护依赖，不能只改SetLiveEnd。

**实际writer正证与可能clobber分开。** 原`NativeDefinitionGPRWrite`按已验证的definition/site/birth、实际form及effective demand区分Value、Temporary、DynamicScratch、MayWriteValue与None。MEM birth可能实际写reload.reg；只有MEM输入reload才发LDR，GPR＋reload仍直接读；同home Get64及部分Get32无MOV；Or/Xor普通目的地与legacy FlagsResult目的地不可混判。MayWriteValue与未知dynamic只用于窗口/byte/高零的负向排除，不能作为真实early-write、W清高或byte种子的正证；MEM U32 reload也不证明X高零。MEM slot、header type、guest写mask均不能推home。共享原`InstructionScratchGPRMask`保持dirty→forwarded→fixed→仅10/12/13租借的顺序；查询须借prepared UnitScratch及真实config/resources，缺失明确失败，不能把零值当无clobber。state、width、最终proof和JIT已接同源规则，仍需整组验证。

integer_branch的Covered CondSet由原ImportIntegerBranchCondition导入key，不冒充一般NativeLocalConditionOutput。以精确原header、同B metadata.condition完整身份和实际控制源核验，Freeze保原event word；独立reader仍核实际producer、raw closure及NZCV窗口，不新增通过标记。

初始接收在原B访问内核实际L个token；最终proof在原GPR site访问累计needs_control，对相关B独立重核一次，无Passed表。无control的整个scope保原路径容器，但新增实际writer/原source查询仍计成本，不能写零增工；控制Function的跨CFG query按实际前驱路径收费。当前每query的seen backing为：home/高零32状态约4×B bytes，单byte按home×8个offset共256状态约32×B bytes，一个word最多8次独立byte query；另有L级hash buckets/nodes与frontier。早稿16×B已不适用。初/末`N_B＋L`、Q×B初始化、原Q×D/Q×E、REF/归属、重叠窗口/递归、稀疏Getter/Meet和flags快照均照计；与原已有4×B/1×B及原本无seen的服务分别扣账，不称整链线性，也不重开最终proof索引微调承担主要提速。

ZeroTarget已在首次live前将同一PhysicalIf.cond换为真实SSA源；仅真换源保原raw token/view/sense，actual与raw根分开。保原fixed-home/preserved/alias政策及真实live延长/MEM压力，不能用“源仍可读”替代原质量命中。晚失败按下一节恢复同一BackendVirtual并有界重建，不直接换回operand。恢复夹具与同值无机器copy发表的真实source正证已修；7bcb失败保历史身份，dd0原owner2230中的metadata恢复路径已通过，不授所有恢复组合；inline optional原条件payload还会增大所有PhysicalIf及其递归节点，不能算成纯D项。

合流及回退均在已有`state*`、`select*`、`constraints*`、`physical*`、原Allocation/Writer/Storage和jit flags/control内完成，文件责任见[5.1.7](middle-pipeline-file-layout-2026-10-28.md#native-local-control-layout)。临时typed缺域只描述施工状态；完整目标仍包含原合法本地控制与质量政策。相依组统一构建、最少受影响正负/重发检查；中间不逐点要求QoR/PMU/FEX。

<a id="native-cohort-owner-rebuild"></a>

##### 5.3.12.10 可逆metadata与原allocation owner的一次重建

**可恢复基线仍是真实Native body，当前输入恢复已改为actual重建。** preclose只选原Block metadata，原全组hardguard核窗口；一次转换先产生合法general/flags body，`ApplyNativeFlagsMetadata`在首次live前借static journal记录真实body，再改可选形态。checkpoint只允许收缩Extra，不能假称可恢复已经被转换删掉的Result/body，不添加第二次generic lowering。

1. **常态一次生产。** 同槽转换记录C/P→实际memory/flags、constant/W-view/Get32→fresh machine live→原Descriptor内部GPR质量→Explicit实际重资格→Derived fixed→residence→原controller。只有真实可撤回cohort保body与Explicit journal，D最坏可=N；flags/GPR语义行快照、Get32派生行undo和Derived兼容修订已退役。
2. **精确失败分类。** 只有可选metadata的late PSTATE/PFAF/home/window条件拒绝可用`MetadataCohortRejected`。坏raw/program、Unsupported、必要transport/scratch或无效callback不能借预算；终态scratch输出开始后禁止重试。若另有原FPR拒绝，先metadata再按原独立FPR路径处理。
3. **先结束借用。** 原AllocationEdit回滚，state.EndNative，恢复late body/transport/live，resources.ResetCurrent；`CancelProgramTargetPlanningForRetry`结束位置编辑但保AllocationState。prepared_inputs等借用结束后，`TakeProgramState`在旧关系仍有效时清Map/soft births、旧fixed声明/ready及索引，再移出唯一owner。Inputs有const身份，不复制/赋值整份。
4. **按所有权次序撤输入。** 先erase Derived fixed；旧gpr owner仍活着时恢复Explicit canonical ordinal journal。再删generated GuestPublication/GuestPublicationSource preferences，保CallABI，随后reset gpr与实际scratch/clobber/read/write/observer/FPR/reload/profile列。不能先销毁恢复函数仍借用的gpr，也不能让Derived插入改变Explicit相对序列。
5. **从恢复后的实际程序重新准备。** 逆序恢复static body，`WithdrawNativeFlagsMetadata`还原terminal/source/sense与可选batch；仅该冷路重收集C/P，重跑actual static形态、fresh live与原Describe。保完整Function和原entry/edge基准，重建Generated行并接原`RebuildProgram`/`RunAttempts`，不恢复semantic GPR计划，不调用已退役的flags重解释。

**有界general重建与累计预算不是同一承诺。** 原scope只允许一次cohort_rebuilt，不重选已撤batch；原质量选项、reserve配置、FPR禁用状态/独立次数保留。现`RunAttempts`每次进入仍初始化eviction_budget=8，不能写成metadata前后共享剩余额度，或全请求最多九扫。冷路实际额外C/P收集、N/E/P、live/Describe/RA、资源恢复和journal全部计费；正常成功不重建scope。

cee0的fixed457/controller294与owner四失败保原身份；dd0同版原owner2230已走过精确metadata Post恢复/执行，仍不授全部组合或normal覆盖。整批撤回可终止组合搜索，最终是否无质量回退仍由6.3/6.4确认，不另建retry manager或certificate层。

<a id="native-final-scratch-handoff"></a>

##### 5.3.12.11 末次独立scratch结果直接交真实资源消费者

**merge56已经删除资源阶段逐body的整轮`DescribeNativeScratchProfile`重算；不是待实现项，也没有测得compile收益。** 原initial Describe、state中的实际profile查询与最终独立scratch核验仍存在，不能写成“profile全流程只计算一次”。这一改法复用原owner和数据，不增加certificate或第二个全N输出。

| 时点/owner | 实际合同 |
| --- | --- |
| 冷拒绝仍可能重试 | 先独立计算GPR、FPR和transport。任一拒绝，scratch仅检查、不输出，原RA seed仍可用；仅精确metadata/FPR原因按原预算处理。 |
| 进入终态输出 | 三项全部通过，Writer核对原program/live/inputs身份与编辑相位，交原`AllocationInputs::scratch`可写backing。设置`final_scratch_started`，随后任何失败都结束请求，包括真正clear之前的校验失败；不再沿metadata/FPR重建。该局部相位守卫不代替独立proof。 |
| 原末次独立检查 | 先核实际control/location，再clear并顺序输出；每site仍从最终body、effective demand、live、locations独立推导，核真实schema/标量形态、preservation、materialization、pool及scratch/clobber。相邻相同profile在同B压缩；不可跨B合并，不读已覆写seed自证。 |
| 交接和资源生产 | 整scope最终profiles都完成后，`EndTargetPlanning → TakePreparedScratchInputs`唯一move；资源循环按ID覆盖顺序`Decode → BuildNativeCurrent → PreparePhysical`。不再重发现profile；真实resource/MEM/raw/flags/fault工作照做。checked Take原相位保持，错误owner、非法相位及非空scope重复take拒绝。 |
| 最后不可逆替换 | 所有虚拟资源/schema读者完成后才Freeze实际cells。Prepare可复用已结束pseudo word存handle；之后不可把handle解作flags需求。true/false/reemit共读同一Ready。 |

删除的是一整轮profile**发现**，不是最终独立合同。新增约N次相邻比较、S次行追加、合并site的N−S次`.last`端点更新及顺序读取；最终S行，最坏S=N。复用backing不保证容量够，grow、搬移和旧新缓冲重叠仍计费；初始seed与final rows在同一存储先后占用，不是同时两份N向量。原source gate已要求count≤65535，此交接不缩现有域，也不授count=65536资格。

目前state profile、最终scalar形态核验、resource memory policy、provenance/REF及Prepare/Freeze resolve仍可能重复解释。下一收敛只在同次真实访问共用纯解码，保各自位置/raw/materialization/资源独立检查；不新建通过表，不恢复已否决的最终proof索引工程。实际落位见[结构5.1.8](middle-pipeline-file-layout-2026-10-28.md#native-final-resource-layout)。

<a id="native-control-location-domain"></a>

##### 5.3.12.12 SetLocation、RSB与call-return按真实消费时点交付

**merge57/58已扩限定域，不能继续写成全部拒绝；也不能从编译或准备/重发推定实际guest运行通过。** 原B记录持有`terminal_dynamic_location`与`terminal_call_return`，在semantic关闭前取得合法来源，同Store的machine live、位置解析、Freeze和JIT读它们；不另建control值表。无相关presence不扫描该家族；存在候选时实际source、尾部和CFG检查照计。

| 已交付域 | 必须成立的条件与用途 |
| --- | --- |
| SetLocation | 常量或定义和使用视图均为完整64位标量整数。独立Block保原位置发表；Function＋indirect_l1＋动态location＋兼容terminal时可延迟读取，其中CallReturn/PopRSB尾部强制DeferredLocation，其余共享须实际GPR/reload同home。独立Block、L1关闭或常量location不强制该form。兼容terminal仅Invalid/ReturnToDispatch/PopRSBHint；不能把未知reload当同home。 |
| PushRSB | 常量key、RSB开启、indirect_l1关闭时真正入栈。动态key、RSB关闭或indirect_l1开启时，沿原合法语义为Covered、无机器读取；仍保Tick/raw退休。这不提供动态Push的新增执行能力，不以0代未知目标。 |
| PopRSB | 指令仍为no-op marker，仅支持块尾直接配对普通PopRSBHint。真正pop/转发/退出归terminal，不造提前machine use；非尾或其他嵌套组合不在该域。 |
| CallReturn | 最后无条件SetLocation＋CallReturn，terminal为ReturnToDispatch/Invalid；callee及raw view与前一location一致，resume为完整64位整数。Function returnPC/return block须匹配原CFG，仍按整Function RA并保原CallABI x14偏好；机器读取在实际terminal。 |

窄definition/view的动态location/resume、含本地Goto/Bind的CallReturn、先前Pop/重复/非tail CallReturn、错配callee/view/returnPC、非tail Pop等合法缺域，在破坏semantic前报`StatePolicyRequired`。结构核验识别的坏native header/effects、token/root身份或元数据结构报`InvalidProgram`；原AddPhi还可能先被SupportsIR报`UnsupportedForm`。晚form与真实home不一致仍为内部StatePolicyRequired，**不能借MetadataCohortRejected的一次重建预算，也不授权checked fallback**。public/不具消费权及Preclose已知缺域可保输入、选择原合法checked入口；PrepareNativeGroup在Take/Close前将source_result置Complete，之后晚失败对外为Complete＋nullopt，整组结束，不在已消费程序上回Inst。可选同home控制形态在原Edits中、transport staging/独立Proof之前兑现，最终control/location核验先于终态scratch覆写。原raw统计仍按body occurrence，terminal机器root不重复算raw occurrence；原control token也不变成data def。

`JitTranslator`在当前B编码期间只借原B元数据，直接读最终physical root，保动态location/call-return的真实终结行为及checked/packed路径。原owner新增Block常/动态marker、Function CFG正负与重发夹具已合入b6fd；该public owner总体仍红，没有这些控制序列的独立实际guest执行资格。Known frontend仍只提交公共location/call/return语义；具体root、RSB配置、home、运输和host编码留在backend，Unknown/EarlyOff不变。

<a id="native-fixed-capture-contract"></a>

##### 5.3.12.13 fixed扩域已合入：保原硬窗口与真实capture

**Explicit实际use扩域已接入原installer、Writer、索引、interval/recolor、action/scratch/transport和最终独立proof。** 它不再是外部待实现包，也不是新分配修正pass。`FixedGPRDefinition`用`capture/fixed_end/allocation_begin`三个u32分开表达真实保存点、原硬home结束点、普通分配起点；capture来源由原transfer时序推导，不再存一份capture-home权威。

1. 原home0→home1两段硬合同保持原端点；实际晚use可以进入普通captured tail，不能靠延长hard end假装机器home一直有效。capture从该点真实有效的home读取，读口、prefix动作、scratch与最终proof共同遵守。
2. REF的逻辑id与canonical backing owner分开。alias在C捕获、root在P>C捕获可以同时必要；原REF访问在canonical lease上取min，让Map覆盖C，但不把root动作挪到C，也不删另一条路径依赖的P。
3. actual publication透明性按最终`FindArgument(0)`、真实root/width与同源发表shape判断；width/bit-offset/clear-high控制字不是逻辑byte offset。`NativeGPRPublicationByteOffset`供state/flags/独立GPR共同投影，畸形投影失效。合法point0 capture不插Nop、不做无符号point−1。
4. partial MEM prefix capture需要真实value-pool GPR与正确owned/clear时序；由原max_scratch与程序Verify核实际prefix峰值，不伪增body scratch，也不借旧Get的reload/region冒充capture backing。

**三种精确残余仍明确拒绝：** 非elided真实producer在最后body点写值、动作模型没有合法after-body capture；whole REF借窄shared-write成员而不能建立所需宽backing；纯REF的晚capture超过root真实backing寿命。它们需要真实动作时点或更宽lease/owner能力，不能只改Map/live、静默删Explicit合同或新增通用fallback。其它foreign/REF不再一概列为永久缺域，资格按实际合同与证据分开。

**partial MEM跟进已经合入并通过原完整case，不能再列未实现。** ce4与dde的原fixed用例均457断言通过：保原GPR/U64正负与完整Function alias早于root双capture，新增24个真实long-live压力值、默认eviction下U32 whole REF root/alias共享MEM，两次width4 capture分别从22/23真实home取值；走原Residence准备/提交并退役Get，再RequireBacking→Stage→fresh live/fold→Submit，最后唯一一批GPR/scratch独立proof。没有手填Map、扩大scratch或移动原硬窗口。历史227仅actual publication/journal/fresh proof，不能反写成当时已走完整Residence；457也不证明guest fault/运行质量或全部foreign。

**ZeroTarget与late coalescer已取得同版完整原例的限定结果。** 原f3c0选中后，merge69先修Freeze后误读Cell4 event，merge73再把actual register_writes交原RA clobber，merge75删没有实际删除行为的晚CohortRequired请求；完整baseline写、checked DSE和原state读者保留。

9b进一步修同值无机器copy的publication证明：确认实际interval/home/width/definition/SameValue后，递归检查publication严格之前的真实numeric source是否可读。外层仍要求guest publication witness，保持三次位置核对、原独立实际写证明与publication到terminal的scratch/preservation窗口；不能只凭Map或publication自身自证。9b原owner整例1861通过，后续33da加自然LoadUniform(U32)→ZeroExtend64和EQ/NE/overwrite后owner1989通过。

late `CoalesceGuestGPRWrites`非BirthOnly/非WidthRoot的actual-window修正已合入。原coalescer按实际produced/stored/write owner与canonical lease，结合原clobber、ReadsCross、observer、fixed/REF/shared成员和真实operand寿命后才调用原Map/Mark。REF/shared仍映射真实writer，alias保持REF；原checked重载与WidthRoot核心不改。33da原fixed457/controller295/owner1989共2741通过；原候选×N访问及新增root/lease/index查询仍在，没有compile提速结论。三个fixed typed残余和全域DSE/最终质量继续保留。

证据：[partial MEM及phase修复合入](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/merge-69-fixed-mem-and-retired-phase/receipt.json)、[原457结果](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-birth-memory-minimum-01/explicit-fixed-actual-domain.stdout)、[实际guest写clobber修复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/merge-73-native-home-write-clobber/receipt.json)及[保基线修复与边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/merge-75-native-baseline-and-lane-view/receipt.json)。

<a id="native-sparse-fixed-result"></a>

##### 5.3.12.14 fixed结果稀疏存储已落地，但净收益仍须扣完整费用

可变fixed结果已从所有普通`GPRPlacement`中移出；placement以`sizeof==32`静态断言约束，只含普通字段和一个u32 slot。原Allocation直接拥有每页32条的稳定`FixedGPRDefinition`小页和唯一active-ID有序roster；immutable inputs.fixed仍是输入合同，不是被复制为另一套程序。普通lookup先读slot，fixed lookup再读页指针/行，不能把O(1)称作零费用。

slot属于**请求**，不属于location epoch或active声明：删除/重装、rollback、SetAllocationUnit、metadata整组恢复及FPR retry保持身份，已有definition借用不因增长失效；Finish结束控制器/inputs后，结果页仍服务emit/reemit。只有最后读者结束后的`ReleaseRequestForReuse`重置K与slot；保留页按未提高的原128KiB aggregate idle预算全额计账，超预算沿原owner回收，不增加另一个cache manager。

N为实际placement域，C为普通placement capacity，F为当前active fixed行，K为本请求自Release后曾获slot的distinct IDs，包含撤回/回滚墓碑，K≤N；P=ceil(K/32)，保留页数P_h可大于当前P。按48B fixed字段布局，本请求对象主项为`32N + 1536P`，完整backing主项为`32C + 1536P_h`；另加页指针capacity、逐页分配/heap开销、active-ID与原索引、D普通undo和D_f fixed undo。每新页初始化32行，未用尾行至多31条；grow、旧/新指针buffer重叠及额外dependent load照计。dense fixed下总量可能反而变大。

真正退出的是普通N行内嵌fixed的默认构造、epoch整记录保存/恢复、普通D undo的完整fixed副本，以及同请求换inputs后下一次触达时的全N重新初始化。普通placement列和Freeze访问仍在；F安装/重资格、索引排序/插入/移动、capture与独立proof也仍在。普通undo只保32B placement及u32 undo ordinal，只有真正D_f fixed编辑才复制完整fixed undo行；by-value undo增长仍收费。不能把trivial vector.clear称为删掉N次写。

在相同placement capacity C下，相对扩域后76B内嵌布局，leading backing差额仅为`44C − 1536P_h`，还未扣页指针、heap及inline owner等费用；`76→32`不是全链净量、真实内存流量或compile收益。[原净工作账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-sparse-fixed-storage-20261101/net-work.md)保这些边界。c984的原stable-owner与rollback用例各45、35断言通过，覆盖跨页、异常中止、slot前普通snapshot、删除重装、Finish/view与Release retained核算；不授毫秒、PMU或≤2×FEX资格。

<a id="native-actual-input-service"></a>

##### 5.3.12.15 已合入：实际机器程序直接生产原分配输入

**merge90～93的producer、生产caller和原fixture已整组落地，后续logical/actual读口修复已合；dd0四目标及owner2230/NarrowRead125通过，原支持域已默认接管。** 本节区分源码路由、限定语义、尚无有效结果的完整负载覆盖率及完整性能，不把已经退役的服务重列未来收益。

| 已退出的native工作 | 现行唯一生产者 / 保留合同 |
| --- | --- |
| `PrepareGPRQualityTopology`、`PrepareNativeResidenceBlock/Topology`、`PrepareNativeFixedInputs`的语义预生产 | source保SSA/host/type/归属/CFG与纯桥匹配；实际GPR/residence/Derived-fixed由原Descriptor及原家族规则直接生产。checked需要的semantic builder保留，死native_topology分支已删除。 |
| `PrepareNativeFlagsInputs`全N修订、`BindNativeGPRQuality`、旧`PrepareNativeGPRViewCohort`/`PrepareNativeGetU32Publications` | actual形态先闭合，原转换记录C/P，实际W-view/Get32读取当前body；不先产语义行再改token/end。Explicit实际重资格保持。 |
| 初始Derived端点grow/repair、WB一点缩窗及`DescribeNativeMemoryWritebackInputs`/`NativeMemoryWritebackReadFixed`包装 | Derived直接读实际definition/live；保纯`NativeMemoryWritebackReadDefinition`、actual extra SSA/source quartet和独立proof。十个退役API的声明、定义和caller均为零。 |
| flags/quality行快照、Get32派生行undo与冷恢复旧GPR计划 | static body真编辑journal及Explicit ordinal undo；精确冷路从恢复actual body重收C/P并重建同owner。 |

现行支持域的顺序只有一份程序和一个输入owner：

```text
最后semantic/source门 + 原pool policy
 → BeginActualNativeGPRCandidates
 → 原同槽转换逐body Record C/P，整scope全部成员提交后 Close/规范
 → actual memory/flags + ConstantFlagsOnly + actual W-view/Get32
 → 第一份fresh machine live
 → 原Descriptor body/use/raw/terminal访问
 → actual GPR/publication/window → Explicit实际重资格 → Derived fixed → residence
 → 原Block/完整Function controller、state/transport、独立proof、resources、Ready
```

C是候选value，P是Set publication；它们借原输入vector而不是新N目录。Descriptor的真实use访问收actual A，另读raw资格R；whole REF只做完整值等价，不把write_owner/Low32/共享写当alias。`ResidenceUse.actual_kind`缺省仅表示raw边，非空为公共Numeric/Implicit/Terminal，final reader重读真实consumer/point/kind；tag不是通过证明。现编译期断言为ResidenceUse40B、BlockRange64B，不等于两套临时前缀免费。

**旧入口退出不代表全链线性。** C/D可接近N，R/A可接近E。规范/去重、C/P和R/A排序、membership查询、W/Get32真实候选触发的全域use/terminal闭合、原P×range/family窗口、REF/alias和Explicit处理仍在。临时C/P/R/A与最终行可同时存活；clear后的capacity、临时候选/排序backing与原128KiB idle保留策略均入账。source Known-W/复杂Get Copy/WB门仍可建立semantic ValueLiveness；该首次图工作没有被本组删除。

publication保持原`AccessesCross`的`(after, through]`区间；不能额外加一跳把source邻居访问漏掉。BirthOnly遇本地Goto/NotGoto/BindLabel suffix仍按原边界取消可选模式，保原flags/偏好及实际lease证明。窄/有符号type、raw预算、复杂Copy和WB terminal/foreign门保原精确范围；消费后的Invalid/缺域不回死Inst，不扩大metadata retry原因。

冷恢复相序以5.3.12.10为准；WB实际base取Cell1、窄Store取Cell3及raw-only地址matcher已修，unknown control/原fixture phase已按真实表示迁移。source_result=Complete只表示Preclose通过，不等于Describe/Complete或NativeReady。源码路由已默认接管；e0d6普通完整四点已取得，但完整负载native覆盖率仍unobserved，没有最终质量资格或源码级≤2×保证。逐源职责见[5.1.11](middle-pipeline-file-layout-2026-10-28.md#native-actual-input-layout)。

<a id="native-final-location-alignment"></a>

##### 5.3.12.16 已合入：最终位置与真实P数值事实

stage06已随actual输入/原compiler caller成组整合，stage05 early SSA producer/reader已经退出。dd0四目标成功、原owner2230和NarrowRead125通过；这是所列原例的限定结果，不是全capture/spill/MEM/helper组合或最终质量资格。对齐决定复用两个原有晚循环：

| 原有访问 | 唯一责任及独立性 |
| --- | --- |
| 最终`BuildNativeCurrent`→`PreparePhysical`循环 | 真实位置与transport已定；BuildNativeCurrent后、PreparePhysical前沿实际prefix/写入/omission形成Aligned/AlignedBackedge及capture P事实，PreparePhysical后只填既有MEM-definition事实。B-local 32 GPR normal/active low3状态不改machine use/def/live/width/scratch/RA。 |
| 原`FreezePhysical`循环 | 从frozen actual cells/resources独立重算状态；在实际capture/definition P重新核数值、原value/source/slot/width/offset/backing lease和完整alias，再消费事实。不信producer Passed位、旧Get home或仅相同slot。 |

capture P的ordinary/alternate low3沿原`GPRPreservationAction`两个尾padding字节交付；普通spill P沿原`PhysicalValue`两个prefix padding字节交付，只有真实MEM结果描述保`defined_low3`。它们是带原身份/寿命的数值事实，不是证明缓存。action16B有静态断言；PhysicalValue新字段已参与本版构建，但本轮没有独立sizeof或资源峰值结果，不预写尺寸不变或零开销。padding利用新增初始化、读写及offset义务。

数值传播按真实kernel：AddCarry是ADC(value,0,CF)，CF未知不能假定加一；Cpuid/Div128Helper pair trampoline按真实恢复/结果处理，raw ABI不直接当net clobber。REF equality-key遇shared_write即停止，shared lease不证明值相等；普通MEM definition P须严格早于read，capture P按真实prefix可同点。

Get source/offset和whole alias link只延寿到原FreezeNative publication visit，在最后reader后结束，不进入Ready或新增N历史目录。helper raw ABI与保存后净破坏分别核；Fixed、preserved/shared、reload、lease和frame/prefix门保持。ForceChecks、fault_emulate、16B、biased/page-table及未知/未证backedge保原guard，checked/Unknown合法HomeAlignment继续。

净账包含两次最终顺序状态fold、B-local状态、P读写与有界alias查询；复用原两个visit不消除N级工作。原[外部交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-memory-access-service-20261101/stage06-final-location-alignment/stage06-handoff.md)和[净工作账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-memory-access-service-20261101/stage06-final-location-alignment/net-work.md)保组件before/hash；当前构建/限定执行身份见[默认接管检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-default-checkpoint.json)。不另开逐helper QoR/PMU/FEX矩阵。

<a id="native-default-cutover"></a>

##### 5.3.12.17 默认支持域已接管，真实normal覆盖与完整收益待观察

merge99把唯一`kNativePreparationEnabled`改为true，原PrepareBlock/PrepareFunction/PrepareFunctions支持域现默认尝试native。原ARM64 JIT、匹配target/environment、fresh ExecutionAccess、density/dump/verify、Function func_ir_free和完整组门均未放松。public/retained、Unknown/RV64与无消费权PrepareFinalBlock保原合法服务，不因开关获得伪消费权。

全部成员先Preclose；仅消费前`StatePolicyRequired/UnsupportedForm`能把未消费OptimizedUnit交原FromOptimized。`InvalidProgram`或`Complete＋nullopt`结束整组；source_result=Complete只证明source门，不代表actual Descriptor、独立proof或Ready已过。一旦Take/Close，只能按实际程序和原精确冷恢复合同结束或重建，不做逐Inst fallback。单guest块Function仍包含synthetic entry并保Function owner/CFG，不以成员数或旧公开AllocateFunction计数零降成Block。

merge99时15类临时诊断已清除：source/initial/fixed/residence、WB子句、metadata/retry定位及其打印专用查询；4个纯打印wrapper/88个调用点退出，真实typed return/Reject和独立证明保留。原必要异常诊断继续。fixed_single/multi现在在TakeIRWithCFG前把原`OptimizedUnit::DecodedBlocks()`交同Storage的一个size_t，原Function准备成功后Record，Return归cache前清零；不按physical.Blocks.size计synthetic entry。新增scalar及padding纳原128KiB sizeof(Storage)预算，不增N/B表或manager。

**默认支持域不跑旧服务是现码路由事实，normal采用率和端到端收益仍未观察。** 显式owner2230/NarrowRead125通过不授普通负载资格；当前已在CoreMark观察实际native块，但完整覆盖率和正常结束仍须按真正IsNative/NativeReady与完整执行确认。`CanEmitForExecution`局部native还包含旧semantic direct-selection，不能当新链采用率。selected hoist、atomic/MemoryCopy/Local、部分宽Get Copy、used VecFCmp/SSE4.2、窄dynamic location及未闭合CallReturn的原精确门保持，不粗暴全部native或全部checked。

实际方向已尝试，但未产生有效性能点：r1因host适配器缺point父目录在guest前退出70，0 guest；r2修该目录后，首个真实Block CoreMark guest启动后提前exit1、无stdout/CRC，其余三点未执行。r2采集父进程返回78是对target exit1/缺CRC的拒绝，不能与r1预启动失败混为一类。私服stop/wait/finish及owned generation/socket均已闭合，原记录保留；详见[审查37](compile-gap-review-2026-10-31.md#c7-native-default-latest)。

publication同点、DirectSelect、实际W宽度、canonical→raw、TSO/zero-CV、VOID consumer及候选binding/kernel角色均已沿原写口修复，各原source与失败保历史身份。37c8原owner及基本guest、e0d6普通完整四点见[7.1](#71-当前代码状态)；13文件临时定位已退，普通catch保持。归因版已有路径事件计数，ordinary/current完整覆盖仍未测；95bd/3625归因已完成，不能跨source借用。默认、typed失败与精确metadata预算保持，最新身份见7.1。

完整计时继续包含source拒绝后原链、metadata整scope重建、Function失败后真实Block重解码及初始化/发布/最后结束。诊断或改变生产资格的运行与普通方向分开；中途按相依组owning构建＋最少受影响原例推进，稳定后集中6.3/6.4，不逐helper扩矩阵。正常支持域绕过旧Describe/Finish、Complete同义发现和每emit binder仍只是代码事实。

<a id="native-handoff-obligations"></a>

##### 5.3.12.18 同点交接与条件接管：按实际机器义务闭合

这轮拒绝来自两处不一致：分配允许的先读后写没有被publication证明正确识别；DirectSelect已承担条件读取，原CondSet却仍声明LocalCondition义务。修复落实在原select与独立proof，不增加inputs/preparation/actions/certificates包装层，不改变Block/Function范围，也不靠通用豁免消除拒绝。

| 交接 | 必须同时成立的合同 | 原规则继续约束什么 |
| --- | --- | --- |
| 旧值在P读完、新结果在P写入同一home | 归一到真实write owner后，旧值在**完整allocation scope**的聚合LiveEnd恰为P；新实际definition birth为P，完整actual-use枚举确实读取同一owner的id及identity；按实际schema/kernel允许先读后写，且不存在仍需旧值的terminal读取 | 只允许本次端点交接；P前所有同home硬段、inherited/canonical fixed窗口仍查。不是把全局闭区间改成半开，也不是“同birth即不冲突” |
| Function端点 | 使用未截到本B的原聚合LiveEnd；跨B/foreign/terminal和shared lease仍有效 | `min(globalEnd, B.last)`仅可用于相应B内查询，不能证明完整Function旧值已死。单块Function及synthetic entry仍遵守Function合同 |
| CondSet被Direct/DirectBoolean Select接管 | 成功选择时同步退休旧ConsumerAction.LocalCondition和物理LocalCondition event；既不声称产数值结果，也不声称产local key | 原Q仍收Disabled CondSet，反向核精确唯一raw occurrence、真实arg0消费者、无额外numeric/key/terminal/live义务；Carry-other只按原完整tuple证明 |
| 数值结果、local key、consumer-owned condition | 三者分别描述真实交付；多一个数值用途仍需要真实result/位置。Select的Condition读取在它自己的实际点兑现 | 最终graph独立核source/type/occurrence和PSTATE/observer窗口；清事件不能让悬空Disabled源消失，普通generic无Result拒绝保留 |

普通物理寄存器参数的合同是final value id、canonical物化id、class/slot、width/role/view及实际consumer；原Store NodeId仍由body/header身份承载。`ReadPhysical`不会为普通GPR/FPR填`producer` token；MEM返回完整descriptor时才承担该token/transport合同。核验和测试分别检查真实定义↔读取↔Allocation及原Store body身份；MEM另核完整producer身份。不能为满足错误断言给所有寄存器加raw token或新N目录，见[IR 2.2](ir-representation-redesign-2026-10-28.md#22-物理记录的目标形态)。

净账按真实访问记录：publication新增受条件约束的write-owner/use/terminal核对及P前窗口查询；条件接管沿原稀疏选择/Q census完成，保留raw与最终graph证明。不新增全域pass或结果镜像，也不声称这些查询免费或足以显著缩短compile。当时修复/fixture未授guest成功；此后37c8基本CRC恢复，e0d6普通方向仍慢，当前资格统一见7.1。

真实fixture现已在9e95收口：Block恢复原guest ABI bank/pool及coalescing；完整两块Function保Add在B0.last=P，并由真实Switch使用新值，经过原Descriptor→完整Function RA/state→fresh GPR/scratch。B1增加旧值实际读取后，完整聚合lease>P须在原publication拒绝，恢复后再过独证。旧跨B Add/发表夹具的StatePolicyRequired仍保原资格；数字端点相邻不保证准入。延长lease的负例在AllocationEdit结束后编辑，不为夹具扩产品域、挪点、手写Map或改全局政策。

<a id="canonical-raw-flags-obligations"></a>

##### 5.3.12.19 canonical发表、raw条件与真实kernel宽度必须分别兑现

32e6现场曾确认窄TEST的普通SaveResult发表canonical NZP后恢复incoming host NZCV，而LocalCondSet/If直接读旧Z。**merge114现已成组修复构造资格、完整窗口、成功交接与最终incoming证明；不再列为未合候选。** a326四目标及三个受影响原例共2993断言通过，但后续原guest仍失败，不能称全部运行问题已解决。

逻辑立即数按**实际W/X kernel宽度**匹配：共享纯shape先核source view、输入/结果的实际32/64位宽及mask可编码性，再提交form/role；语义U8结果和U32输入可以同用W。可选miss保Generic真实输入，最终独立reader仍核实际位置/宽度/mask，不把半提交变成StatePolicy失败。

| 决定 / 原写者 | 已合合同及不成立时的行为 |
| --- | --- |
| decode局部资格与dead-edge marker | 窄legacy logical Save撤raw NZCV承诺、保PF named-result，宽/deferred/FP保持；canonical Jcc也可附真实双后继dead marker。marker不授权当前PSTATE，没证明保canonical CondSet |
| 原flags pass成功交接 | required全部位须由实际primary提供，不继承INC未写CF；原扫描覆盖primary/发表→condition，含AdvancePC后gap。相交flags写、读/fault/helper/control或极性观察均按原合同拒转换；更早canonical发表后、最终arithmetic前的fault也须拒，首次新发表前的operand fault仍可合法。全部门成立后同RewriteContext同步Save→BranchOnly、同节点CondSet→Local，保持身份/type/use；miss保canonical |
| 最终incoming独证 | 原actual prefix先Bind meet、再核ConditionFlags全mask、最后Observe；覆盖key和numeric CSET，不依赖Result/HasPstateTests摘要。物理Carry可读性与canonical极性、tuple及last-consumer窗口分别证明；仅已有metadata＋integer独证且无body/PSTATE读取的Covered退休ZeroTarget沿原例外 |
| 实际kernel/terminal | 普通SaveResult发表后恢复incoming NZCV；BranchOnly SaveResult先写canonical requested bits再加载NZCV；Generic/raw BranchOnly不发表，依producer真实raw结果。CondSetControl沿原pending/key/必要恢复，local If只读已证明raw；不统一插canonical reload |

完整窗口已补AdvancePC后writer/fault及更早发表后fault见证，实际采用的是03封包；c077/02的缺口保为历史，不重复安排重修。a326的三个原例分别由`swift_ir_test`、`swift_cases_test`、`swift_test`拥有，不能混作owner全套。后续TSO及logical-prefix补齐见下节；具体原身份见[审查40](compile-gap-review-2026-10-31.md#actual-flags-cold-publication-latest)。

<a id="actual-effects-cold-publication"></a>

##### 5.3.12.20 实际effect、consumer角色与metadata冷恢复由原写口兑现

| 已确认边界 | 现码与下一步合同 |
| --- | --- |
| NZCV保持与guest观察分开 | `target.cpp`仅把已审计的普通StoreMemoryTSO加入真实PSTATE保持域；RCpc/RCpcImm、非S地址/对齐检查、DMB＋STR不改NZCV。它仍可fault、观察guest状态，不能跨越需要canonical发表的窗口；不推广到TSO load或atomic |
| 动态defined与hardware-zero分开 | `ActualPstateProducer`按真实form/demand返回`PstateWrite{defined,zero}`：仅真实S逻辑kernel授予动态位及`clear∩CV`，硬件CV零另记。无S的PF-only/clear-only OR/XOR、Covered/Disabled/ConstantFlagsOnly等不凭opcode获资格。legacy Clear只消费已证明的当前hardware zero，孤立Clear无来源；GenericSave仍核同source并保未触及位 |
| prefix的覆盖/失效 | `physical_zero`沿原prefix维护，Meet取交集、新S覆盖、clobber/MSR替换或InvertCarry失效；ordinary SaveResult仍撤新requested资格并恢复原硬件表达式，不能借旧zero冒充新动态N/Z。原canonical极性、fullmask和consumer窗口不放宽 |
| producer宽度与consumer角色分开 | `ActualPinnedW`直接问既有`NativeGPRConsumerRoles`；该写口仅为真实ALU＋4B producer授PinnedW。合法Low32的唯一consumer可以是VOID store/publication，不能先对consumer的VOID结果做size查询。保原view、普通use和真实store，不伪造W producer、不删use、不改RA范围 |
| 候选home与实际binding分开 | GPRUseHome(any)可给出inferred候选，不承诺GPRLocationAt(Value)同址。原Whole仅在候选width、实际W/X bits、GPR/no-reload及home全部吻合时retag；合法候选miss清residence，保原Whole并解析真实GPR/MEM copy。原explicit Value仍严格，坏身份/invalid不能吞成miss；常态一次query，候选miss才多一次Whole query |
| kernel按role读取，pinned独立 | EmitSetHostGPR经原ScalarX/ScalarRead分派：Value使用UseGPR，definition/Whole沿原物化，真实pinned Low32/Definition沿原合同。合法Value可pinned=false；不伪pin或改Whole躲ASSERT，原R/X/W的Whole guard保留，MEM的MaterializationId/reload与控制字/MOV/BFI/清高不变 |
| 精确metadata撤回的全链成功 | 合法ZeroTarget缺home仍为MetadataCohortRejected；原同owner一次整scope恢复/重Describe/RA及独立FPR状态保持。publication候选绑定及kernel角色修复已合，37c8原失败guest恢复exit0/CRC；不以进入retry或单次Describe成功代替全链资格 |

定点诊断已确认原publication的raw/actual身份一致，但候选home4与真实Value/Map GPR9不同；这允许来自preserved suffix或whole-placement的既有优先级，不能直接定为stale Map。Realize只把候选当可选绑定；Whole候选miss保真实copy，explicit Value不默退，最终独立proof继续核完整source/window、preserved capture和MEM backing。之后编码又暴露Value/pinned=false误入context.X的kernel缺口，改用既有ScalarX兑现role；两个写口问题分别修正，无新wrapper/位置目录或扩大重试。

以上沿原select_memory、translator_mem与独立checker收口，无新pass/N表/manager。既有RawPstate 32→40B、LocalPstateEdge 48→56B及原prefix/窗口访问仍收费；新增候选绑定只在miss多一次Whole解析，普通命中一query。3162断言和原guest CRC是限定正确性/基本执行证据；e0d6完整方向已测，但不能把总差距归因于这条修复或授予单机制收益。

基本执行、诊断退出、e0d6普通四点和两轮归因已按原身份闭合；当前8cda已合consumer侧actual-use核验A与17项临时计时退休。A最少检查仍有一个新增fixture前提失败，无新ordinary收益。后续按7.2先完成该纵向包的有限验证与完整反馈，再按剩余主要成本选型；本次文档不启动它。

### 5.4 M3：两种分配范围参与同一跨阶段服务

**不再按旧 assignment 子桶决定是否参与。** 保留两个 scope，共享实际 assignment/Writer 和质量政策；改造范围是从约束消费到最后实际位置提交，而非再换一次 interval 容器。

| 范围 | 正向算法和必要输入 | 不能省掉 |
| --- | --- | --- |
| 独立 Block | 读同一native程序的实际machine reads/defs及必要implicit/terminal roots，得到局部kill/use-end与fixed affinity；沿真实birth完成assignment、copy/spill/reload/repair | public/preseed、未来FPR fixed read、两pool、reserve/pressure、REF/point/whole/MEM、真实fault；logical retirement不是numeric use |
| Function（含单块 Function） | 在原CFG上由实际machine uses/defs求跨块use-end、entry/join/loop及合法次序；原Writer沿该范围分配并交付actual effects，不沿旧semantic ends后补form | external/missing roots、call-return、foreign/hoist/terminal、跨块用途、loop、Function parent/code object，不能逐块降格 |
| 困难输入 | 首次真实需要时建立额外 next-use/spill/修复状态；明确首次建立、复用、失效和结束 | 不以更多 spill 或漏 future use 换 compile；already-needed live 信息不能省 |

原预着色→discard→fixed→REF 优先级、FPR future reads、reserve ladder、publication-before-use、恢复和首候选须逐项映射，最终符合6.3；不把旧调度/排序当作不可改变的质量合同。已否决的 Block stream 只改收集/调度，Word 只改传播，均不等于这个跨阶段替代，也不授权重启其微调。

生产入口先做真实粗准入，支持域走新服务，未覆盖public/manual/目标能力域直接选原正确路径；不能先付完整新算法再付完整旧链。正常retry属于同一服务，也必须给出成本边界。通用路径成本/覆盖率计入whole compile，迁移完成删除被替代旧链。

#### 5.4.1 困难路径、重算范围与终止合同

| 触发 | 目标处理及保留的输入 | 必须界定的重做工作 |
| --- | --- | --- |
| 局部GPR/scratch不足 | 在原Writer修复真正受影响的location segment；更新writes、transport及依赖选择，复用不变inputs/live | 触及范围、继续点及失效集合；现已有受限局部修复先扣除，不声称首次实现 |
| 精确MetadataCohortRejected，含ZeroTarget | 按5.3.12.10结束借用、回滚late编辑，取回唯一AllocationState；恢复真实Native基线、原输入行和terminal后fresh live/native Describe/原controller | 最多整scope一次general重建，撤回batch不重选；额外N/E/P、派生列恢复和RA与原FPR retry分账。Invalid/Unsupported不借预算，不回semantic Describe/Complete或逐候选重启；整批撤回不自证质量无损 |
| 原FPR质量ContractRejected | 禁用可选coalescing一次，沿原controller整scope RA；与资源不足/Invalid分开 | 保真实失败相序、独立预算与所有权，列明连带重做的GPR及准备前缀；不声称只重FPR或整个Complete必然重做 |
| 必要全scope reserve/合法回退 | 保原正确终止路径及稳定virtual程序；静态输入复用，稀疏实际选择回滚 | 触发、有限尝试集合/上界与每次N/E/P工作；不从raw IR重新lower，不提前覆盖virtual cells |
| 语义编辑/环境变化 | 仅在合法可变阶段，沿原owner失效，重新取得真实准入 | 不使用旧sealed Facts或旧位置结果，不能伪称普通retry |
| 编码region失败 | 复用现真实true→false机制，目标输入改为同一不可变PhysicalReady | 重建context/offset及动态资源执行状态，再编码；目标不再重binding。当前保原IR的行为见5.3.4，不冒称已迁完 |

现码的有限上界也要明确保留：每次进入`RunAttempts`，普通路径先试一次，之后reserve ladder最多5档；该次调用的eviction预算8，最多9次`AllocateRegisters`调用，再可能落入上述ladder。metadata RebuildProgram再次进入时会重置该局部额度，不是跨整个请求的累计剩余上限。部分重启启用`finish_on_candidate`，会提前停止，**调用次数不能直接乘完整N/E成本**。每次reset仍重建Map/live-end、spill/reload和位置相关结果，Facts/inputs复用已存在。

原checked Complete的FPR admission在GPR declaration和BeginFunction state之前，失败禁用coalescing一次、整scope重跑RA，连带GPR及此前准备，不等于整个Complete重做。native proof在state/transport/实际编辑之后判FPR，按5.3.12.10结束借用并回滚相应结果；现码已接保owner路径。33da原controller与owner整例通过，c984的joint例保原source；dd0原owner2230包含WB与精确metadata恢复路径，通过范围不授全部冷组合。终态scratch覆写开始后禁止这两类重试，见5.3.12.11。两条路径各自保原预算，当前没有动态频率或独占成本。依据：[RunAttempts](../source/runtime/compile/allocate/allocation.cpp)、[两入口相序](../source/runtime/backend/arm64/compile/compiler.cpp)。

Block修复限于其真实局部范围；Function修复沿跨块live/entry/join/loop依赖，不能逐块降格。普通成功路径应一次实际分配完成，压力处理尽量继续前进；保留的整scope重试需明确受界，不能靠“inputs已缓存”隐藏反复全量结果重建。

实施包必须填实每种retry的合法触发、尝试上界、复用项和失效范围，不预填未经设计的“最多一次”。降低重试次数不授权提高全局reserve、更多spill/MOV或削弱最终质量；不可恢复错误按原请求失败语义结束。原6.3/6.4不变。

### 5.5 M4：请求、编码和最后结束

| 域 | 实际改造 | 保留义务与退出条件 |
| --- | --- | --- |
| 请求准备 | 同实际 request/group 的环境、workspace、缓冲和小状态复用；困难状态按需 | 不改变 RA scope；Map/LiveEnd 的实际 resize/清零不能宣称“缓存免费” |
| 编码 | 现193个同源kernel直接借同Store的physical body；允许按真实reg equality/立即数做局部编码，禁止回semantic producer/Map重选form或home | 不另写193套kernel，不经旧48B wire往返decode；真实Tick/pending、scratch、fault、relocation、重发和Flush仍必要 |
| 发布 | 在真实 code allocation 中直接形成持久 entry/fault/link/SMC 数据，删除无消费者的临时副本 | 已落地 batch/按需序列化先扣除；I-cache、可见性、撤回与 grace 保留 |
| 结束 | 源头减少临时数据，从而少初始化/清理；原 owner 最后释放或有界复用空 backing | 资源移交不等于释放；不把 N 清理、last lease 或 fallback 移出 timer |

按真实producer/consumer统计对象数、触及条目、初始化/复制/清理字节和最后保留容量；不要只统计alloc调用或capacity。无相关义务时不建立对应spill/next-use/copy等困难状态；正常dense结果一次初始化后沿真实写口更新，复杂输出按实际事件产生。当前Storage复用仍会resize实际Map/LiveEnd，128KiB idle预算与FEX虚拟arena不是同口径，不能靠调大缓存上限推算收益。当前JitContext已每线程保一个代码buffer容量至多256KiB的idle assembler，ResetForReuse并为active/reentrant请求保持独立owner；这是已有实现，不能重新立项为M4预期收益。真正尚需退出的是无消费者存储及其实际初始化/访问/结束。

当前managed语义槽为128B、Inst104B，真实physical记录48B；区分槽容量、实际初始化/读取字节、grow重叠与跨请求累计，不将它们等同于一次峰值或每遍全量读取。3ef末次N退役仍计费，但旧显式ir_free很小，不能把cleanup独自立成主要性能路线。更早结束某数据必须先迁完真实读者，不为此重开生命周期工程。

旧库leaf结果见2.2：空backing复用只能减少真实malloc/grow/free，不能据此省掉仍需要的resize、清零、fill、排序、核验及所有权工作。M4应随M1少出生、M2/M3少关系/少输出，直接删除对应存储的初始化、复制和结束；可保有界空容量，但不先开展通用allocator/容器改造，也不将交叉采样投影相加凑覆盖。

请求数只解释放大，不可由2415/255直接推倍数。Function请求数量已较接近 FEX，仍很慢，说明合并请求不能替代前两条主线。other 还含首次 Facts/准入等真实工作，不统称管理层。

### 5.6 Spill 与最终生成质量

历史 CoreMark 回退仍需随最终组合收敛。区分 SSA spill/reload、窄 preserve、guest publication、edge transport；按真实 live、fixed/tie、call/clobber、scratch、reserve ladder、attempt/FPR retry 解释放大。fault 可恢复语义不能撤回，已有精确定价、局部 repair、spill-region 和 good encoding 先作基础。

C7已消除支持域中可证的假machine def/use及其旧hint/view需求，这是避免虚假live/lease压力的必要修正；当前control-memory最少检查没有运行CoreMark或统计实际spill，不能据此声称解释了历史spill增加的全部原因，更不能写成运行性能已经恢复。控制链还须按真实写入区分条件key、数值结果与临时结果，再讨论其压力收益。

开发期间允许改变生成质量，登记受影响族及已知缺口，只做相依构建和必要语义；稳定后集中恢复和验证。不能为了即时机器码完全一致，把旧每张表和每个顺序永远保留下来；最终仍遵守6.3/6.4，不以更多运行指令抵消 compile 收益。

### 5.7 代码组织服从决定归属

```text
runtime/ir                         唯一语义、用途、顺序和实际 owner
runtime/compile
  builder / rules                  一次语义提交、纯能力
  passes / analysis                必要反馈与公共事实，同次解释供稳定需求
  allocate/program                 原程序/CFG的中立借用接口，无第二图
  allocate                         两个范围、共同 assignment、唯一 Writer
  unit / optimized_unit / pipeline / workspace
runtime/backend/arm64
  compile/compiler                 原 session/Prepared 调度与寿命
  compile/target / constraints     纯能力、ABI/pool与中立约束适配
  compile/native / select          同Store唯一程序、同源form与operand/effects
  compile/state / verify           实际state/CFG转移、独立结果合同
  compile/physical                 共有metadata、冷/public兼容replay
  jit                              同源 kernel、编码/布局/发布
```

完整文件树和每源职责见[结构细则](middle-pipeline-file-layout-2026-10-28.md)。新增`native.*`定义实际对象/只读视图，`allocate/program.*`只规定中立访问协议，均不拥有第二程序或manager。原Allocation/Writer、state、Prepared与Store继续拥有真实结果和寿命；Selection不扩成完整operand图。公共Writer只提交位置/真实写入合同，不包含ARM64类型。

`native.*`与`allocate/program.*`已在第79批落地，其余新文件名仍是目标组织，非实施前置；按真实消费者迁移。native对象/访问与机器program适配归native，共有metadata/冷replay和原寿命owner归physical。plan* 中调度归compiler、临时求解归其调用寿命，成功form/operand/effects和真实transport归同Store native；Selection只保稀疏候选/unit/B信息，独立核验归verify/audit。**不是把旧PlanBuilder原样藏进internal。** 所有消费者迁完同时删旧字段、转换、CMake、include、失效开关及夹具。

### 5.8 正常路径的工作量表与退出标准

每个完整实现包必须维护以下**实际工作表**，复用原净工作说明；这是算法审查产物，不是新profiler、每节点证书或逐文件测试门。外层visit合并后，表中各真实服务仍须分别说明去留。

符号：N_b/E_b为实际出生节点/occurrences（包括后续删除者），N/E为最终语义节点/occurrences；counted、implicit、terminal和实际机器operand分别列，不强行等同。B/G为真实块/CFG边，P为有真实消费者的最终copy/preservation/transport输出，H为实际编码及修正工作，Φ为必要固定点传播，R为整scope分配重做次数，k为单指令可变参数规模。不能把旧候选表数量称为必要P。

| 服务 | 正常路径必须说明的工作 | 困难路径与退出标准 |
| --- | --- | --- |
| 构造/公共表示 | 每形态一次提交；字段、counts/owner维护；具体少出生的N_b/E_b | miss只构造一次generic；AfterAppend不算未出生；5.2.6替换须覆盖所有真实读写者 |
| 用途与residual | 唯一用途owner，实际首次roster/CSE/深事实/state/flags/hoist；变化沿原反馈闭合 | 不存在的普通双E图不认领；无变化不重做同义全域服务；raw/hoist新epoch另计 |
| 最终公共/目标输入 | 完整semantic schema/SSA；一次target规范化，同槽move原参数/资源进入BackendVirtual | SSA Finish与最后图匹配后才关闭；完整旧Describe/Complete不再叠加，不能建全N operand影子 |
| Block分配 | 实际machine uses/defs上的局部kill/affinity；真实birth前准入，shared write联合lease/压力/next-use/spill由原assignment兑现 | 不用跨宽whole REF或假implicit保旧end；局部回退维护原队列；不得把逐候选整scope重启作为普通算法，retry按5.4.1计N/E/P及R |
| Function分配 | 原CFG上实际machine live、entry/join/loop；普通N/E与Φ分别归属 | 不套块内范围；entry-width/form循环须真实有界求解，修复/重做按依赖 |
| 架构state/选择 | 原Descriptor直接交actual拓扑及raw资格；原copy/版本/width行、实际事件及P输出；最终独立证明读真实结果 | 不以晚Residence逐候选全扫重建P_candidate×E；selected/inferred/LowView、preserved及publication路径保真；遗留P_pub×D、重复子证明/链/point与Φ明计 |
| 物理兑现/资源 | 稳定header跨prefix，主role先定、例外role稀疏；原resource行随成功body提交；MEM保raw退休与canonical物化的不同key | 当前actual inputs与原raw用途仅同义时共借，不用其并集污染counts；preflight/freeze双resolve及大Params整理照计，无N handle/完整current副本 |
| 编码/发布 | kernel借稳定PhysicalReady，Params按请求cell读取，H和实际metadata；动态资源仍执行 | 不逐emit全体校验/复制body或向Allocation补role；true→false只重建ctx/offset/动态状态，public/旧packed保真实能力 |
| 初始化/结束 | 每种存储的live、capacity、实际初始化/读写/清理和最后消费者 | 无需求状态不建立；资源移交不算释放，不靠移出timer或扩大retained达标 |

理想普通Block应能解释为少数固定遍数的N/E，加真实构造、P和H；Function额外承担Φ。这不是所有输入严格线性的承诺。相同big-O仍可差几十倍，必须列每遍内容、访问次数和触及量；大Params可采用有说明的O(k log k)整理，R/Φ/P也不能吸收未说明的重复工作。

C5/C6还须把以下新增工作列入同一净账：preclose会为原先在Map gate处就退出的候选提前支付图匹配；原inputs新增publication资格、匹配后的alias/use payload；按需catalog/bitmap/foreign set、reused域内raw E resident计数、B级clobber投影、30 home的转移与CFG Φ。保留`!reused`在catalog建立前的真实早退；稀疏、共用算法或提前完成不使这些费用归零。shared-write聚合后的spill点用于物理成本，不能充当raw multiplicity。表中服务的减少必须扣除这些新增工作后再由整包方向判断。 C6还须计原Recolor每候选全域工作、最终B catalog/fold与排序、projection插入移位、raw闭合Q×E、实际改动journal及preservation失效重建、canonical materialization寿命和最终actual scratch；logical窗口前置仅删除对应P_copy×P_pub，不消除这些成本。

固定行插入/seed复制、同producer多view重复definition/容量核对仍进入净账。C6已删除preclose semantic入口宽度求解与30B查询，最终metadata共用native按需解；transport则改为Stage前后独立各一批，不再对每D_edit重复全域分类。两次推导、需求触发的scope访问、原point storage和`Σ_t D_t×A_t`preservation搜索仍计费。scalar无改候选的双快照已退出；保实际D_edit局部/整批两个时期的必要checkpoint。samehome省W写的Q_high查询仍计真实body/definition/CFG访问与B级scratch；最多32 home状态/块不代表整个compile线性。准确退出与保留见[最新复核](compile-gap-review-2026-10-31.md#c6-width-transport-latest)。静态Get32的一轮actual-use closure与候选排序保留，不能每候选重扫E；独立性来自实际结果，不来自逐请求整scope重算。

C7另列原本不存在的数值def/use及其伴随费用：CMP、ConstantFlagsOnly和matched condition链须先交真实机器合同，再由原Descriptor直接生产actual约束；否则会把假live、假输出lease与spill带入RA，不能在scratch末端靠折扣修正。无Q不付候选闭合；有Q批量raw/actual/REF闭合、候选排序、raw/actual membership与聚合、真实body及Explicit恢复写入照计。Original head可直接借作Logical时不复制raw行；完整identity、Store归属、Definition/REF查询仍计真实路径成本，不宣称整个helper O(1)。

固定输入由原installer一次完成，借FixedReady与既有固定owner退出新增candidate×F扫描；这不是删F安装/核验。最终flags reader在原site访问累计临时保守谓词，不是新增B遍历或Passed目录；proof所需实际图和窗口仍读。late token保留真实suffix、preserved P×D/点查询，不能把一B一次reader写成整链O(N+E)。C7是正常服务替代的相依组；95bd/3625已有unit/attempt及准入分解，见5.8.3；这些修正仍不能折算成单机制或数量级收益。

当前控制组已借原全组hard访问与窗口，退出每B重复全F扫描；这不删除原hard安装、D指针排序或独立proof。已合CondSetControl另保原Merge/Flush scratch、已有key-map的重发清理和真实Generic数据写入；本地控制另付5.3.12.9的L路径、稀疏宽度摘要及真实flags合流，ZeroTarget一次失败另付完整N/E＋RA，不藏入FPR retry或所谓常数fallback。

控制扩域还须避免新增`Q_control×prefixN`寻源：借原first/mask recipe交真实key，而不是逐控制参数调用会扫旧prefix的source回调。raw/REF闭合、每候选实际窗口及CFG查询、metadata访问、LocalParity真temp与Carry的Tst/merge仍收费；raw terminal只借原树并不消除访问。普通memory同槽控制字消除重复语义编码定义，转换/逆投影、fault/TSO和最终资源核验仍是真工作。组件少一张表或不新增N状态不能代替这份净账。

本地控制相依组的新增正确性工作也必须扣入净账；下表是源码成本与下一收敛责任，不是新测量框架或可省毫秒预算。F为原fixed行数，D_f为需重准入的行数，Q_get为去重后的实际Getter，L为真实label，activeD为Goto当点活跃physical copy数。保留最终独立合同，优先在原真实访问交付必要事实，不再加一层等价描述或索引manager。

| 本轮实际工作 | 当前费用与收敛要求 |
| --- | --- |
| actual fixed输入及Explicit重资格 | Derived现从原Descriptor实际definitions/live直接生产，旧semantic Derived比较/修订已退出。Explicit F_e仍需比较和真实definitions/terminal/REF/窗口，D_e调整/排序/删除恢复及最坏D_e×F_e移位收费；不能把稀疏journal说成无实际访问。 |
| 本地copy与flags合流 | 原Goto保存实际活跃copy，计Σ_Goto activeD的快照、排序和Bind交集；L个flags表示/scratch及raw meet另计。无Goto不建copy快照，不为常态无控制请求预建完整状态。 |
| Getter宽度 | 先扫描相关publication、逐发表追BitCast/ZeroExtend链，再对去重前候选排序；不能只按distinct Q_get计费。其摘要、foreign依赖去重和原队列增量重算照计，来源exit未变仍通知。原B×30和Φ保留，不增加第二CFG或N历史。 |
| 真实writer与独立proof | prepared scratch profile/实际位置解析、home/高零4×B或byte32×B的每query backing、每word至多8次byte query及原REF/路径查询均计费；规则共享不代表只执行一次。不得以增加Passed表消除独立性。 |
| SaveResult | 真正延长result/operands的live，额外Params、scratch、编码及可能的capture/spill/reload全计。正确的Save原点不是质量收益，不把额外压力归为无关噪声。 |
| 可逆metadata与重建 | D级body/输入journal最坏D=N；PhysicalIf的inline optional原条件payload影响所有同类节点尺寸。精确冷失败另付一次完整N/E/P、fresh live/Describe/RA与恢复，分项报告但仍包含在完整compile，不能藏入原FPR预算或从端到端扣除。 |
| 最终scratch交接（已合） | 删除资源阶段一整轮profile发现；新增约N次相邻比较、S行追加、N−S次端点更新及顺序读取，S≤N，grow/移动仍计。initial/state/final独立推导、真实resource与双resolve保留，不宣称仅算一次。 |
| location/call-return（已合限定域） | 原B terminal roots、真实来源/尾部/Function CFG及最终独立检查计费；不复制控制表，不把延迟machine root加进raw occurrence。无presence只省该家族访问，不等于整个B零成本。 |
| 第三residence/REF扩域（已合，限定验证） | capture、固定窗口、allocation_begin保独立语义；F比较/重资格、真实D×N_B窗口、REF追踪、canonical lease取min和每capture寻源仍收费。dde原case457已含U32/MEM压力双capture与完整Residence，不授全部fault/guest或三类typed residual。 |
| 稀疏fixed结果（已合，限定验证） | 普通placement32B含slot；结果payload按1536P_h及页指针/heap计，K含墓碑、Release才重置。删普通N内嵌构造/epoch复制、普通D full-fixed undo及同请求切inputs后的N重初始化；保N placement/Freeze、F输入/索引和D_f full undo。dense fixed可更大，不能只报76→32。 |
| 普通selected memory（stage01～06已合，限定验证） | dd0 owner2230/NarrowRead125通过，包含原WB/RCpc与恢复路径；不授全P/MEM组合。actual fixed、独立proof/TSO/RCpc照计；stage05早hook已退，stage06两次最终状态fold/P费用保持。 |
| actual guest写入与晚coalescing | 初始clobber增加W条实际write及mask/points；late非WidthRoot actual-window已合且33da三例通过。原候选×N与新增owner/lease/index访问保持，正确输入不等于降时。 |
| actual-input-service（已合、支持域默认true，限定原例通过） | native语义GPR/residence/fixed预生产、flags再解释、quality重绑定和WB一点桥已经退出；C/P记录规范、raw R/actual A读取、排序/compact与Explicit处理仍计费。C/D可=N、R/A可=E，临时共存/retained与原128KiB idle预算不可漏，必要semantic source live仍在。 |

每包至少给出一条正常Known/native路径和它实际支持的困难分支；public/retained/EarlyOff/Unknown沿各自合同。原三种编码入口保持，不能为简化统计赋予伪消费权。实现完成的依据是旧生产者、读者和循环实际退出，不是表里写了“一次”或加了ready位。

#### 5.8.1 当前Known/native的净工作账

原支持域已默认尝试同Store新链；进入该链就绕过完整旧Describe/Complete和每emit semantic binder，并已退出native语义质量预生产、flags修订与GPR重绑定。dd0四目标及原owner/NarrowRead通过，但已观察实际native块，完整负载NativeReady覆盖率尚未建立。下表按代码分支记账，不能把显式fixture当默认负载覆盖或把旧PMU/fresh FEX转移成本版。

| 实际服务 | 支持域默认链（代码路由） | 保留分支及共同成本 | 下一项真实退出 |
| --- | --- | --- | --- |
| M1：decode/build、residual/公共事实 | 仍先Optimize；共享typed birth、有限INC/DEC/NOT少literal与空事件裁剪已有 | 两链仍付N_b/E_b、counts/owner及首次roster/CSE/深事实/state/flags/hoist与反馈 | 广泛少出生/首次解释，扣target miss/continuation/template/text；有限M1不授时间 |
| ① 最后semantic/source门 | 保SSA/host/type/归属/CFG、pool政策、metadata与纯桥；不预产GPR/residence/Derived-fixed计划 | Known-W/复杂Copy/WB仍可能触发完整semantic live；miss后继续原链全计 | 减真实首次图工作须有准确依赖，不能把本组写成消灭semantic图成本 |
| ② 同Store转换/actual initial | 原转换收C/P；actual形态→fresh live/Descriptor直接生产GPR/residence/fixed；旧flags N修订、Bind、WB桥已退 | public/retained/Unknown及source miss保checked Describe/Finish；C/D可=N、R/A可=E，规范/排序/compact、closure和临时共存/retained照计 | 收敛原候选、实际用途和P×range/family，不另建质量层或N图 |
| ③ 两scope RA | 原Block/完整Function controller、machine live、placement32B/稳定页及原完整质量政策 | 原policy/assignment/active/dirty/fixed/repair/retry；冷重建按每次原controller实际预算计 | 原访问内批量处理义务，保局部修复，不关政策或缩scope |
| ④ state与最终form | 不跑旧Complete；付native observer/CFG、placement/Get sweep、final fold、copy快照/foreign Getter与flags meet | checked分支保原Complete；L/Q_get/Φ/REF、Recolor与P×family等查询仍是真工作 | 减实际重复state/定义/值链，不重复认领已删semantic solver |
| ⑤ fresh独立证明 | transport两批、真实writer/NZCV/token、REF/CFG/高零/metadata、actual WB fixed read保持 | 真实窗口/lease/alias、initial/state/final scratch及prepared资源均需核 | 保独立合同，按完整scope核同点read-before-write及原prefix窗口；按原Q/graph核接管归属。不加Passed表、不扩retry或用Map自授正证 |
| ⑥ resources、编码、发布/结束 | 无每emit binder/资源profile重发现；Decode/BuildNativeCurrent/Prepare、controls和Freeze双resolve、Ready编码/退役照计 | stage06新增两个既有晚visit中的数值fold、B-local/P初始化读写与alias；保留分支仍按原binder重发 | 随真实producer/reader退出删无消费者初始化/复制/结束；不能把少一张表当减时 |

默认true不等于完整请求都走native；[默认接管检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-default-checkpoint.json)保留当时normal覆盖未观察的状态；当前实际块快照也不提供完整覆盖率。source miss→原链、metadata冷重建、Function失败Block重解码与初始化/发布/结束全计，冷unsupported不能移出口径。原48～50%旧qualified-cycle并集包含必要工作，不能相加成预计收益；源码没有≤2×保证。

旧P0/Other/库leaf、a10及历史47ms均保原身份；e0d6是最新ordinary方向，B/F仍44.33×/23.24×。两轮归因已闭合，native GPR独证是主要准入成本，路径覆盖也有限；other不等于固定初始化。下一步不重复相同分段，而按5.8.3和7.2退出具体重复发现、state/资源及M1首次工作，source miss/旧链、失败重解码和最后结束全计。

coarse timer仍须按真实调用边界解释：native的source、actual转换、首次Descriptor和state/resource准备在PrepareBlock/Function内，不能因不在regalloc/codegen旧桶内而漏算；regalloc_ns主要包原controller，runtime的codegen_ns另包PrepareForExecution/emit。桶名不等于完整职责。后续以有效完整Translate判断方向，必要独占归因另列；不移动timer，也不把重叠scope相加来凑完整compile。

SVM_PROF2逐append clock/atomic、density/dump/VERIFY会改变生产域；日志或旧source census不作当前覆盖/每IR成本。测试仍是相依组最少原语义、稳定后集中6.3/6.4，不逐helper扩QoR/PMU/FEX矩阵。

<a id="582-当前a10生产的主要服务新native-pmu归因"></a>

#### 5.8.2 第79批before的a10生产：既有native PMU归因

采集身份为source495887ca、main120e（生产逐字等a10），不是当前94cb/d9a9。同guest2000、CPU4原只读门、仅PROF1；每scope一次，无STOP/attach/逐点注入。两record退出0、CRC4983，原始数据约7MiB，自有场已关闭。raw/API的精确Translate前缀与原ELF映射核对闭合；严格合格分母为Block759样本/803579066period、Function857样本/915799836period。record报告lost/truncation为0，但未知映射/无记录调用者仍显式保留；所有unqualified不进入下表。

| 互斥服务 | Block严格compile周期占比 | Function严格compile周期占比 |
| --- | ---: | ---: |
| decode/build/region | 15.43% | 18.19% |
| mutable state/flags/value优化 | 8.49% | 10.34% |
| 首次final Facts/SSA | 4.96% | 4.47% |
| 首次uses图 | 2.12% | 0.71% |
| 首次value roster | 0.27% | 0.47% |
| 优化前host支持准入 | 1.52% | 1.41% |
| target inputs＋初始fixed | 16.12% | 13.06% |
| 真正runtime::compile RA attempts | 6.35% | 6.78% |
| Complete declaration/selection/check | 14.93% | 20.23% |
| native bind/select/resource | 5.46% | 3.99% |
| native实际emit | 9.01% | 8.37% |
| native envelope未细分 | 2.66% | 3.98% |
| publish | 4.51% | 2.71% |
| 输入/emitter结束 | 4.06% | 2.70% |
| qualified服务尚未解析 | 4.12% | 2.59% |

全部1616条审计行按整数period闭合；显示百分比有舍入。`arm64::Storage::AllocateBlock/Function`包含Complete，不能误算为实际RA。按实际callee/inline上下文归类的库leaf计入其服务，但caller-inclusive不等于caller self；这些是一次采样中必要与重复工作共同形成的周期域，**不是可删比例、当前普通毫秒、全量动态调用计数或FEX比值**。[当前原账与规则](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/current-native-attribution/service-ownership/report.md)、[逐行审计](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/current-native-attribution/service-ownership/analysis.json)。

当前证据已否决继续主攻单个helper：首次Facts/uses/roster、单个GPR核验、binder或publication均不足以解释总尺度；普通finalform反推虽可见，不能仅凭代码可写批准它承担根治。下一源码改造必须覆盖inputs→实际决定→Complete→native交付的大语义链，其互斥并集观察为48.18%/49.64%，包含实际emit/必要合同，绝不能全算可省。M1构造/残余与M4发布/结束仍须同时收敛；当前实际出生shape/gate、真实调用/失败次数及可替代比例还未知，没有据此承诺两倍上限。

定向复核进一步否决把普通publication finalform直交付独立立为主攻：PreparePublications与VerifyPublication的命名祖先去重包络仅B1.812%/F2.118%，连相关未解析glue保守计入也只有1.944%/2.235%。这仍是caller-inclusive观察包络，不能识别真实BirthOnly成功/返回Skip次数，更不能当可省份额；不能借整个Complete20%替该小链背书。[同raw定向复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/p1-current-native-review/report.md)。

<a id="native-measured-net-work"></a>

#### 5.8.3 已测主要成本与下一算法的净工作要求

95bd先分native group与attempt，3625再分准入；两mode均原CRC、关闭及source/index核对通过。它们是PROF2归因，父子范围有嵌套，不与e0d6 ordinary/FEX混算或重复相加。[第一轮](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/native-phase-95bd-summary-01/summary.json)、[准入分解](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/native-admission-3625-summary-01/summary.json)保原身份。

| 3625归因，ms | Block | Function模式中的native子集 |
| --- | ---: | ---: |
| GPR独立证明 | 695.636 | 103.768 |
| native准入，包含上述GPR | 783.671 | 116.055 |
| GPR占native准入 | 88.77% | 89.41% |
| actual inputs准备 | 117.630 | 17.226 |
| 实际位置/state/form兑现 | 199.813 | 27.793 |
| 最后资源准备/物理化 | 112.551 | 15.335 |
| 精确metadata重建 | 0.202 / 1次 | 0 / 0次 |
| NativeReady事件 | 1275 Block | 77 Function＋83 Block |

这些计数只属于归因source，不转授ordinary覆盖。checked路径仍有费用；95bd的checked codegen_complete为B176.500/F315.433ms，不能与3625子阶段相加。GPR整体优先减工，不代表695ms全部来自Get发现；metadata重建未显示为主要成本。

| 工作包与状态 | 唯一数据与替代算法 | 退出与验收边界 |
| --- | --- | --- |
| A：consumer侧actual-use核验，8cda已合 | 原body/implicit/terminal访问直接用真实cell及已绑定PhysicalValue；WholeREF只作完整值等价，按实际Getter稀疏保存的observed计数独立对照fresh live | 退出每Get全scope实际users寻找、consumer重找/重枚举和二次绑定。map bucket/节点、REF解析及剩余lease/fault/capture照计；三例未全绿、无ordinary收益，不称整个proof线性 |
| B：RawDirect逐query恢复raw users，未实现候选 | 在原raw/actual生产及inputs owner中设计完整可索引的raw关系；reader独立核真实consumer、multiplicity、terminal和header总数 | 目标退出每query扫publications/views/residence/aliases及困难全流恢复。现候选raw前缀不是完整E；先界定完整域、失效、最后reader、字节/初始化，再比较净成本 |
| 来源/state证明：待选整包 | 稳定结构transfer与动态home/copy/observer分开；原scope访问批量交付实际义务，独立checker从actual程序核来源与路径 | 退出相同链反复解释和每query恢复相同关系；完整保REF/捕获/MEM和CFG。必要路径求解照计，不固化每observer全scope重算 |
| 资源/物理化：待选整包 | 核对原body/cell/resource的Prepare、controls与Freeze输入和最后依赖，一次决定贯通真实reader | 减少重复profile/字段解码及相同binding。若要新增全N descriptor或改变失败边界，先明确承载、字节与净收益，不能仅删除第二遍 |
| M1：构造到首次公共服务，同等进入计划 | 原builder/rules/ValueWorklist及编辑通知共同减少真实出生与重复发现；保首次CSE/深事实/Users/DCE和dirty闭合 | 给N_b/E_b与实际解释次数变化。直接字段及有限INC/DEC/NOT不代表广泛减工，不重开无收益Facts共访、移位、codec/getter调参 |
| M4：随旧工作退出同包落实 | 原workspace/inputs/state/resources只有真实消费者才建数据，最后reader后结束或有界复用空backing | 删除不再需要的初始化、复制、排序和清理，不以缓存预算或延后释放冒充算法收益 |

允许有真实查询需求、完整域和最后reader的关系索引；禁止同一职责的重复权威、第二份可编码程序和producer自签Passed。若不用索引导致Q次全scope寻找，须与一次生产/有限保留比较净成本，不能以“不新增表”代替论证。实际source/role/lease/CFG编辑沿原owner失效，不沿用上次正证。

每包列正常、miss/checked、必要retry和最后结束的真实工作；先在代表性真实单元上闭合纵向包并取得普通方向，再扩域。M1、M2/M3、M4同账；同账不等于同包施工，各包按真实依赖闭合后即可反馈，单个热点优化不代表两倍目标有解。

## 6. 两倍目标的成本预算与验收

### 6.1 总目标、已测规模与尚未解释的余额

最新ordinary e0d6完整账为B2066.780070/F1099.290002ms，同轮fresh固定FEX为46.623802/47.296979ms；2F总门约93.248/94.594ms，当前仍需约95.49%/91.40%的整体降时。[原普通账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-development-direction-adapter-r3-20261101/prepared-native-ready-e0d6-r3/compile-gap-summary.json)保端点差异及单点方向资格。

decode＋RPO＋opt本身约B200.736/F257.096ms。如果它们保持不变，单独优化GPR或其他后端子项不能达到总门。因此M1首次工作、M2/M3实际决定/证明、M4触达必须共同有净减工；不能将归因695ms从普通账扣除，也不预分配各项可省毫秒。

以下保留M0 da/main7ec的历史互斥粗桶和条件数学关系，用于解释原设计尺度，**不作为新source预算或未来不可突破的下限**。

| 完整成本，ms | Block | Function |
| --- | ---: | ---: |
| decode/build | 111.158 | 165.636 |
| RPO/编号 | 0.000 | 6.562 |
| opt | 102.233 | 89.723 |
| allocation准备＋RA | 167.879 | 170.426 |
| Complete＋绑定/资源＋实际emit | 299.320 | 325.891 |
| publication | 35.229 | 29.074 |
| 显式IR-free | 0.714 | 0.705 |
| other，含wait | 75.400 | 92.347 |
| **完整translate** | **791.933** | **880.364** |

在这份历史账中，Block的opt包含SealIR/FromFinalIR；Function的opt先停止，再做Seal/Facts，所以这部分落入other。共访候选曾把首次Facts延至输入准备，子桶换位而whole未改善；现已恢复原相序。不能把换桶计作省时或改whole服务端点。[实际范围](../source/runtime/compile/pipeline.cpp)。历史wait 0.866/0.157ms已在other，不另加减。

decode＋RPO＋opt 为213.391/261.921 ms；regalloc＋codegen 为467.199/496.317 ms（59.0%/56.4%）；publish＋free＋other 为111.342/122.126 ms。这只是互斥 timer 账，M1/M2/M3/M4 不按桶认领独占收益。

2F约94.955/94.552ms，需要约8.34/9.31倍改善，即减少88.0%/89.3%。即便regalloc＋codegen理想归零，仍剩324.733/384.047ms；若decode/build保持111.158/165.636ms，仅此项也超过当时总目标。必须同时改构造/公共服务和请求费用。“input准备大”不等于全可删，“首次”也不等于实现不能变。

令p为真正被替代的旧耗时占比，s为该部分加速比，其余工作不变时新旧总比为`1-p+p/s`。在da尺度上，即使s无限大，p也至少为88.0%/89.3%；若只覆盖95%的旧耗时，该部分仍需约13.59×/16.55×改善。此为条件数学关系，不是收益预测或新的采样门；不能用文件数、节点数或单族命中率充当p，也不能把旧阶段费用说成未来不可突破的下限。

每包记录退出/保留/新增工作及完整反馈；交叉收益不累加，未解释余额保持未知。本文具体算法尚需闭合真实依赖并实现，不填预计毫秒，不承诺3ef或下一个包必然达到2×。

### 6.2 开发反馈与最终采用分开

| 时点 | 最少动作 | 决定与边界 |
| --- | --- | --- |
| 文档/设计 | 既有源码与证据、数据权威/失效/最后reader、净工作及代表性输入 | 不启动构建或手机，不恢复暂停的实现 |
| 相依代码组 | owning目标统一构建＋最少受影响既有语义 | 修真实失败，不按文件/helper新增QoR/PMU/FEX门 |
| 首个可运行纵向整包 | 同配方before/after，独立Block和完整Function各一次完整开发方向，保相同guest/配置和原失败收口 | 在扩family或复杂cohort前检查总成本、支持域及miss/retry；无需等全部ISA迁完，不以toy夹具或单helper替代 |
| 缺口会改变算法时 | 复用现工具，只补必要路径/阶段归因 | 已完成95bd/3625不原样重做，阶段计时不与ordinary混算 |
| 完整包无收益或明显回退 | 暂停同类扩展，核对退出/保留/新增和路径变化；决定改算法、撤回或仅保必要维护成果 | 不以“后面还要迁很多”无限延期判断，也不把中间粗点当最终统计结果 |
| 稳定最终组合 | 集中6.3/6.4 | 最终不降生成/运行质量、资源通过、各scope≤2×fresh固定FEX |

中间生成质量允许变化，必要语义错误仍须解决。完整compile方向与逐片QoR是不同检查；每个纵向包仅一份净工作说明，计初始化、source miss/checked、retry、编码发布和最后结束，不另立测量框架或逐节点证书。不移动timer、不缩RA范围，不以多spill或少功能换方向。

以下6.3/6.4保留冻结原文和门槛；其中历史时态不代表当前源码已经验收。

### 6.3 不降低生成质量的统一采用门

以下是主要流程稳定后的集中验收标准，不是每个重构片段的继续条件。按用户最新要求，中间产物允许影响生成质量；过程中只做必要构建和受影响的正确性合同，不逐片运行生成质量、lifetime、PMU或FEX矩阵。已知中间质量缺口明确记录，随完整流程收敛；最终质量与两模式compile目标仍须通过，不能把过程允许变化写成最终退化额度。

先声明候选类别。**A 类**仅改变绑定接缝、存储或用途管理，预期语义、优化顺序、实际分配和编码均等价。**B 类**有意改变直接构造、成型或分配，实施前列清允许改变的 family/PC/路径。出现未声明的代码变化先 HOLD 归因，不能采后把 A 类改称 B 类豁免。

另在采样前声明运行证据属于**同码等价**还是**实际改码**，与 A/B 的实施范围正交：P2b 可改变 IR 而保持最终机器码不变。只有整个被测 workload × mode 的机器码、布局合同、metadata 及运行路径实证等价，才使用下述 A/A 噪声分支；不能拿某个未改子路径给同组真正改码部分豁免。

| 门 | 必须检查的结果 | 通过条件与失败动作 |
| --- | --- | --- |
| 语义、异常和寿命 | 实际执行相关现有 cases；ISA 独立参考、fault/recovery、partial/high32、CF/PF/AF、TSO/atomic、FP/NaN、terminal/root、manual/import、重入、多 Prepared/parent | 无新增意外失败、跳过或 expected-failure 漂移；SSA/fixed/width/scratch/recovery合同及独立audit保留；具体重复扫描可按第5.3节的实际所有权/失效合同替代。错误或遗漏必需合同即 NO-GO；原22条历史断言逐项保留状态，不以失败总数相同代替逐项核对 |
| A 类生成等价 | 同输入的 IR/terminal/root/type-view、最终顺序、映射、spill/transport、指令及机器码；fault/entry/link metadata 按真实语义比较 | 确定性输出 exact；仅预先声明的地址/重定位字段可归一化，不归一化 opcode、宽度、访存或普通 MOV。差异未解释即 HOLD，证实调度/编码/恢复退化即 NO-GO |
| B 类生成质量 | 四矩阵 Block、对应 Function 根、命中和未命中/generic/known-unsupported 路径；spill、其它 transport、总 host 指令/字节分别报告 | 改善项与未改善项逐路径列出；热点不得把 spill 减少换成更多总指令或更差循环编码，既有 good encoding/首候选规则不能丢失。新增慢路径必须声明可达条件并实际覆盖；不能以平均值抵消某个路径退化 |
| 普通运行质量 | CoreMark/FP1 × Function/Block 分组；沿用两规模斜率的 instructions、cycles、wall，逐指标独立比较 candidate/baseline | **实际改码的默认回退预算为0：每组每指标至少七对，所有配对比与配对中位均≤1.00。** 未达成不得采用；分布跨1、方向证据不足为 HOLD，确定性退化或可靠的运行退化为 NO-GO。同码等价用下述分支，不平均跨 workload/模式/指标抵消 |
| 资源与真实回收 | 四点 MaxRSS sanity、owner/operand/link/payload 实际 capacity、retired extent、grow 峰值、last lease 与 idle retained | 沿用并前置冻结 MaxRSS 同时 `>1.10×` 且增加 `>8192KiB` 的 HOLD 门；本包新增/改造的 owner 有明确 retained/session 边界，不把真实回收移出服务账。原 TLS arena 的保留成本如实记账，不要求纯绑定片先重写它，也不全算成本包收益；MaxRSS sanity 不代替完整峰值模型 |

预先声明并实证同码等价的 A 类或 B 类，普通运行仍要实测；对系统噪声可使用**候选测量前冻结的同一基线 ELF A/A 波动界**判等价。未冻结该界或出现超界/持续变慢即 HOLD；不能只凭 code exact 忽略 dispatcher/link/metadata 路径的真实变化。这个分支只证明相同生成质量下的测量等价，不给实际改码分配运行回退额度，也不作为运行提速证据。没有等价证明且比值跨1时，不能写“已证无回退”。

旧协议中的 `≤1.01` 保留其历史含义；**不能把1%测量容差写成零退化，也不自动继承为未来候选的退化预算。** 本次重写保留最终采用规则，没有运行新的质量批次或宣称当前代码已通过。

### 6.4 结构交付、方向判断、性能采用与最终目标

| 层级 | 判决依据 | 结论边界 |
| --- | --- | --- |
| 结构交付 | 完整生产者到最后消费者、唯一决策归属、旧工作退出、必要构建和受影响语义检查 | 可以记录实现完成；不称性能收益或最终质量通过 |
| 阶段方向判断 | 第6.2节少量同配方端到端粗点，结合覆盖和净工作说明 | 决定扩大/调整/停止；一两个点不支持统计提速或最终采用，也不能无限以基础改造为由忽略中性结果 |
| 单机制正式性能采用（确需隔离时） | 质量/资源与ordinary按冻结协议集中执行；保原Core Function或Block至少一组all7<1且median≤0.95正门，任组all7>1且median≥1.01为compile负控NO-GO；其它组分别报告 | 5%只是该机制资格，不是大幅目标完成；中性组不等于零回退，FP不充当正门；该七对机制门不前移为每片编辑/方向判断的要求 |
| 最终外部完成 | 最终部署产物对固定FEX普通Loader8632943c…/Server9fe58a99…；每个预定可比workload×Block/Function至少七对fresh分母，每对及中位SVM/FEX≤2，并通过质量/资源门 | 不复用历史47ms、不用诊断ELF/旧PGO/阶段份额推算，不改变分配范围或减少guest功能 |

最终workload roster固定CoreMark、FP1及双方确实支持的代表性bench/cold-compile输入，登记准确guest SHA/argv、语义和结束条件；不支持者明确不适用，不跳过后计PASS。R3只证明其CoreMark范围；一个输入达标不代表完整目标完成。

完整compile-service包含真实初始化、发布、最后消费者后的释放/复用及明确残差，禁止移出timer记作删除。独立Block、单块Function、多块Function、fallback以及请求/分配parent/code object/合并parent的分母分别报告，不将请求数量直接当可省倍数。

设备继续用红魔bf1359ff，沿既有ADB/PMU与隔离服务协议。方向点只读记录状态，异常漂移不作为收益；正式对照满足冻结的频率准入条件，变化则停止该批。大临时数据实际写Data2，复用固定构建缓存，及时回收已用完的重复临时产物；原始必要证据按版本保留。本次文档重写不启动设备测量。

### 6.5 停止扩展与改变路线

1. 旧完整链仍在、普通流量多数回退，或只是转移初始化/清理：未形成替代。
2. 完整反馈只有小变化，且没有待迁移的大服务：停止同类调参；维护成果可以保留，不计大幅性能完成。
3. 与已否决方案不同的算法须指出新的成本依据和退出方式；不能重命名旧 stream、Word、codec 或窗口试验。
4. 粗准入前缺乏覆盖说明、state/位置依赖未闭合、核验只相信候选：先补该真实接口，不能边写边再长出一层证书。
5. 证据说明成本在另一服务则调整顺序；只做必要归因，不无限等待完美归因，也不假设所有首次工作不能改。

## 7. 当前状态与后续执行

### 7.1 当前代码状态

**原仓库冻结source为8cda2889f05a96fb3a12c021013d18c05ea4dca0fbdb2a63f81f8318364dd23e，2404个source/build输入。独立框架候选为151211daad58a0ec5334cb9a876dbd08df3249ff9b00d8494757c8da852898e1。用户已授权解决框架问题，旧细节扩域不恢复。** 原支持域默认true、Known/EarlyOff/Unknown、独立Block/完整Function、typed失败和原质量政策保持。当前同Store已接通不代表性能采用。

| 身份 | 实际结果与界限 |
| --- | --- |
| 37c811d1，publication binding/role | 两目标、原owner一例3162断言及原Block guest CRC通过；独立旧语义/基本执行身份，不转授当前source |
| e0d6e7e8，最新ordinary | 退13文件临时定位；translator-only及四点完成，各mode一对。B2066.780070/46.623802ms＝44.32886×，F1099.290002/47.296979ms＝23.24229×；ordinary NativeReady覆盖unobserved，非最终质量/采用或完整任务生命周期 |
| 95bd38e9，第一轮归因 | 两mode原CRC、关闭及source/index核对通过；native group/attempt子阶段和路径事件已闭合，见5.8.3 |
| 3625724f，第二轮准入归因 | GPR B695.636/F103.768ms，占native准入88.77%/89.41%；NativeReady为1275Block及77Function＋83Block。PROF2不转授ordinary时间/覆盖；metadata重建B1次0.202ms、F0 |
| 8cda2889，当前A＋计时退休 | consumer侧actual-use核验已合，17个临时timer及4个成功scalar已退；translator＋swift_ir_test两目标构建成功。三个原case为2 PASS/1 FAIL，3714断言中3713通过。原production owner和fixed-suffix整例通过；新增NarrowRead负例在真实绑定前提处失败，未到stale-count检查，后续该case发码未执行 |
| test-only 02，未应用 | 外部冻结的WholeValue/U16 raw-view及真实绑定前提修正，未应用/构建/执行；没有修改产品guard/RA/Map，不授当前A新guest/ordinary/FEX或最终质量 |

普通Function四点保6条原physical-final的loop_hoist_body_entry拒绝，后续fallback最终CRC通过；失败及Block重解码已计总时间。505Function/99Block粗计数不是NativeReady覆盖。消费前source miss保checked链，转换后不回死Inst；归因只锁定主要服务，不能把全部回退归给同槽、一个Get循环或必要语义测试。

FEX为单initial-context最终CompileBlock累计，SVM为inclusive Translate，端点差异保留。最新ordinary未达2×或最终质量，8cda不继承e0d6计时。详细证据：[普通完整账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-development-direction-adapter-r3-20261101/prepared-native-ready-e0d6-r3/compile-gap-summary.json)、[95bd归因](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/native-phase-95bd-summary-01/summary.json)、[3625归因](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/native-admission-3625-summary-01/summary.json)、[A两目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-consumer-read-proof-build-01/build.json)、[A原三例检查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-consumer-read-proof-minimum-01/execution.json)。历史raw/失败/checkpoint均不回填。

### 7.2 下一步执行表

**算法与检查机制以[框架合同v2第12节](compile-framework-contract-2026-11-02.md#12-五项框架问题的算法决定)为准。** 原8cda/Git/WIP继续冻结；实施只写独立工作树，不自动恢复旧A/02或opcode/cohort扩域。首次分析、关系/CFG、RA交付和必要动态检查属于框架，不能藏在适配器中。

| 包 | 已固定的算法/实际工作 | 状态与进入下一步的条件 |
| --- | --- | --- |
| F0 基线保护 | 原代码、索引、WIP/证据保全，工作树隔离 | 已完成；原源码/索引保持 |
| P1 首次分析 | sealed schema/SSA与可选semantic live/index/capture解耦，真实reader按需完整Prepare | 候选151211da已实施；无布局/API/优化顺序改动；仍被reader请求的分析照付 |
| P2 完整关系 | 正式GPR准入内一次point目录和完整raw CSR，退出候选逐查及Q×全raw回扫 | 候选已实施；三个metadata reader暂保原扫描，不为每次query额外建N目录 |
| P3 home/CFG | Boolean窗口计数取代owner枚举；有限must-equality分区与RawPstate按真实CFG/Local Goto求解 | 算法已选，尚待实际接通与独立参考核对；空间/Φ/常数全部入账 |
| P4 最终交付 | 原actual决定与唯一Map；结束全部虚拟reader后原位一次CommitPhysical | 尚待整组迁移；当前Prepare/Freeze双绑定继续计费，不能直接删除 |
| F3/P5 完整反馈 | decode至发布/最后结束，含source miss、全部attempt及Function失败重解码 | 代表性真实两scope反馈与fresh固定FEX；仍超2F或normal乘积发现未退出则不通过 |
| F4 扩域 | 按固定读口补family、复杂cohort及剩余域 | F1～F3闭合后进行，不提前铺opcode或以局部case替代完整门 |

本轮复用8cda固定缓存，先重链接出逐字相同的基线IR测试ELF，再在独立缓存编译实际修改TU和private header的所有真实引用TU；translator/IR test两目标链接完成。7个受影响case/4066断言通过，device namespace已关闭。block_analysis_index_test.cpp必要fixture适配已编译，其所属完整swift_test未重链/执行。本轮首版bfd97fc5曾因目录新增前提未覆盖control reader及首份增量配方漏依赖TU发生SIGABRT，失败、精确source与产物保留，不回填通过；151211da修复后同组通过。

实现、构建和最少语义按新source独立记录：[build-02](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/framework-first-20261102/framework-resolution-20261102/build-02/build.json)、[minimum-02](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/framework-first-20261102/framework-resolution-20261102/minimum-02/execution.json)。最终6.3/6.4及6.5保持；框架整体性能尚未确认，下一步不以两包完成恢复细节扩域。

本轮完整对照未闭合，F3不通过：01因产物路径不在原校验器证据根而停；02/03的FEX均正常guest退出/CRC，但短运行中的proc取证缺项，原严格collector拒绝并停止后续点，Function SVM没有获准启动。三批失败、所列原始点和关闭均保留，不选快点、不回填完整四点PASS。所有本地子进程与私有FEX服务已结束。CPU4本轮上限为只读观测的2803200，历史e0d为1171200，不能跨条件归因P1/P2收益；本轮没有新的合格两scope FEX倍数。详见[完整性记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/framework-first-20261102/framework-resolution-20261102/direction-feedback.json)。在proc读取仍依赖目标存活窗口时停止重复采样；下一包先完成P3/P4，再用有界同owner读取机制收口完整反馈。

### 7.3 协作与审查边界

主线程拥有共享树、整体接线、统一构建/最少语义与唯一设备；工人只提供外部组件或只读审查。结构/正确性与完整compile性能分别汇报，失败及旧source不回填。大临时数据写Data2、固定缓存复用，及时回收可再生产物。

修订17仅更新八份现有Markdown，恢复最后用户定稿版的净成本约束，纠正载体论证、目标/过渡算法及当前/历史状态。source8cda、ordinary e0d6、两次归因及旧语义37c8分开；最终6.3/6.4、6.5停止条款、审查1～41和旧21正文保持原字节。没有产品/测试修改、构建、设备运行、stage/commit/push，不恢复暂停目标。[本轮边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-17-net-work-correction-20261102/start.json)、[静态核验](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-17-net-work-correction-20261102/validation.json)。
