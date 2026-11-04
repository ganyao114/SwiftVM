# SwiftVM 编译成本优化执行记录

实施范围沿用 [优化方案](compile-cost-optimization-plan-2026-10-27.md)。目标仍是 Block 与 Function 的 compile 成本接近固定 FEX 的数量级，同时保持生成代码质量。原始基线为代码提交 `ed41e6f6`、guard-r6 launcher `02085b0b…`；尚未完成最终目标。

## 当前状态

当前工程检查点已更新为 C31：源码 `3cb2a8f7…`、非插桩PGO主程序 `56fb68c9…`、匹配profile `0e4affdd…`、cache52。16目标构建、54质量命令、相邻和直接原R6各七轮56点、普通PMU72点均完成独立复核。相对原R6的累计Core Function/Block编译耗时配对中位减少21.00%/33.86%，FP减少23.02%/27.88%；12个普通指令斜率比范围0.998687–1.004711，全部满足原1.01门。

当前运行限制保留：相对R6，FP Function/Block wall slope中位增加0.906%/0.338%，各2/3轮增加且范围跨1，不能称运行零回退。root选定收据为 `compile-cost-c31-pgo-r1/selected-checkpoint.json`。外部FEX同数量级目标尚未完成；历史C19的源码、profile、精确ELF及下面的各批证据保持。

正式外部28点已经完成有效性与计时独立复核：Function/Block配对中位分别为21.0054×/13.1035×，两组7/7均未过严格小于10倍门。C31普通调用链及两次Function重建诊断均已收口。其后的C34 raw-first原型完成16目标构建、定向16用例/1570断言、完整54质量、四点MaxRSS与原七轮56点，但Core正门失败，已拒绝并撤回。当前源码和固定构建缓存已精确恢复C31/source3cb/PGO56fb/profile0e4a，手机16角色也已恢复、其他9项不变；C34身份只代表历史实验。目标仍未达到，下一项N3只进入具体布局/全生命周期净账设计。

随后 C32 完成临时存储原型、54 条质量命令及七轮 56 点，但 Core Function/Block 配对中位仅下降 3.8277%/0.8155%，未通过原 5% 正门，已经拒绝并撤回。该次收口时主机源码、typed PGO 配置和 16 个产物精确恢复 C31；手机 16 个角色统一为 C31 PGO、其他九个参照/输入不变。此前七个仅部署角色原为历史二进制，统一部署不等于恢复它们的旧身份，也不等于运行了全部 16 个角色。C33 的有界跨 scope 复用仅准备 results 提案，尚未应用；当前C34实验没有新的外部差距读数。

此前完成的Function逐段形态诊断由独立raw parser核对104次self late-entry的PC/回边/marker和原始流，各诊断均精确恢复C31。随后C34完整实现共享支持选择、owned字节窗口和Function内层重试，完成质量及性能矩阵后因正门失败撤回；不再把它列作当前待实施包。Block顺序分配仍因实际覆盖与通用等价条件不足停止。

历史C19：源码 `0b283cbb…`、非插桩PGO主程序 `5a94db9e…`。相邻与直接原R6各七轮56点、普通PMU72点、14目标构建、22套件及FP48实际字节均完成独立复核。累计Core Function/Block编译耗时中位减少20.59%/29.69%，FP减少20.80%/27.10%；12个普通指令斜率比均通过1.01门。

历史C19的运行限制继续保留：相对R6，Core Function/Block的wall slope中位增加0.264%/0.677%，三轮均增加；FP增加0.961%/0.821%，各轮跨1。有限质量门不能证明全部运行时间零回退。旧原始ELF、profile及配置已经保全，新旧批次不合并或求差作为改动因果收益。

随后 C24 与 C25 都完成质量门及七轮对照，均因主要性能门失败撤回生产修改。C25 删除了每次 scope 的堆分配，但 CoreMark Function/Block 中位只改善约 1.12%/1.07%，范围均跨 1；没有凭少分配保留额外 32 B context。旧测试夹具的零 FPR 问题另行修正，候选与一致旧实现均通过四个定向命令；该测试修正保留。恢复基线源码 `8401918b…` 等于 C20 生产源码加夹具修正，16 目标恢复构建已完成；手机主程序恢复选定 C19 PGO，host cache 仍为通用构建，未宣称全套 PGO 产物恢复。全部实验原证据保持原身份。

C7/C8/C10/C14 的拒绝决定及精确恢复记录保持；C11/C9/C18/C20 的单包结果仍按各自原范围解释。它们随 C19 组合通过既定质量门，不因此追认单包稳定提速，也不相乘阶段比例。下面的实验编号保持原含义。

C26 已完成质量门与七轮 56 点，但 CoreMark Block 七轮全部更慢，中位增加 1.85%，触发预定负控，两文件已精确恢复 `8401918b…`。相关七个既有案例补充的完整 SECTION 覆盖及原始收集器行数错误均保留；原测试没有重跑。C26 删除不带入新候选。

C27 单 vector 候选已完成质量门及七轮 56 点，CoreMark Function/Block 中位比为 0.997848/1.007654，四组范围均跨 1，两项 co-primary 都未通过预定 5% 门。五文件已恢复 `8401918b…`；16 个 host 产物精确恢复，手机主程序为选定 C19 PGO、15 个独立测试为通用构建，三个参照角色未动。没有保留该候选，也没有补点或进入其 PGO/普通 PMU。源审查、实物和质量通过不能替代实际性能采用门。

全部结果实际写入 `/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results`，下文简称 `active-results`。复用 `/Volumes/Data2/swiftvm-middle-build-cache/android`；没有新建整套构建或展开产物目录。

N0 已完成首批实际证据：7291 个已有样本完成精确地址/内联归因，修正了 symbol-only 报告的跨 PID 聚合偏差；56 份 SVM 日志与 FEX snapshot 完成工作量核账。另使用已有 IR 明细计数各运行一次 Function/Block，原 Function 结果读取错误保留且没有重跑。N0 本身未修改生产源码，也未产生提速结论；详细结果见下面的 N0 记录。

C28 字面量保留原型已经拒绝。R1 构建的命名空间拼写和 R2 新测试的 width-fold oracle 已分别修正，失败记录保留；R3 源码 `765a990d…` 完成 16 目标构建和六阶段质量门，724 行/894604 项断言包含原样受控的 22 项 R6 历史失败，未新增失败，1346 项 native checks 和 FP48 实际字节核对通过。但七轮 56 点及独立重放没有通过预定收益门，五文件实验修改、16 个 host 产物已精确恢复 `8401918b…` 基线，手机主程序恢复选定 C19 PGO、15 个测试恢复通用构建。实验 ELF `666c6f6e…`、源码与原始结果已保全，选定 C19 PGO 检查点未被替换。

随后 C29 独立修复 HIR AdvancePC 的窄立即数累计问题。原生产已精确复现两项预期失败；修后原语义门没有新增失败，但 Function 一个 FP 缓冲因准确 guest 范围触发晚入口重新切块，由 2680 B 变为 2228 B，原严格字节门失败保持。最新 r4 的 32 条独立正确性补证及 74 个 raw 已全部完成独立复核，包含真实 x86 循环/故障状态、cache52 对旧51的 disk/AOT 拒绝及 FP 对 r1 的字节保持。r2/r3 的测试前提失败均保留，C30 七 opcode Block 合并已 NO-GO。C31 fault owner 索引原型的 generic 质量和一次七轮56点已独立复核：Core Block 配对中位下降 5.0072%、7/7 更快，其他三组无稳定收益。匹配源码的新PGO也已通过16目标构建、54质量与相邻七轮56点的独立复核：Core Function/Block中位改善12.0351%/21.4596%，FP改善11.2212%/17.5673%，四组各7/7更快。直接原R6也已通过，Core Function/Block累计配对减少21.00%/33.86%；普通PMU也已完成，C31已选为当前有限检查点；耗时限制和外部FEX目标继续保留。

## 首批实现与测量

首批源码完整集合摘要为 `745be1ef…`，launcher 为 `e9de5574…`，cache 格式继续为 51。新清单核对 2418 个 source 文件和 118 条退役路径；它与旧 guard-r6 的 491 文件覆盖清单使用不同摘要域，不能通过摘要差异推导文件增删。

实际删除或缩减的工作如下：

- scratch 输入在有序块中直接生成变化点；两 pool 的逐指令预算和 reload bound 保留。乱序、稀疏交错的手工 Function 输入保留原完整记录和排序回退。
- 固定约束最终验证在有序访问中使用单调 cursor，随机源链及乱序输入保留原查询。
- 无 memory 的块跳过四轮专用匹配，无 pseudo consumer 的块跳过 flags-only-spill 全块访问。
- 宽度证明直接追加整单元临时结果，全部验证成功后才统一 Certify。

CoreMark2000 与 FP1 的配对结果如下；比值均为 candidate/baseline，基线为原 guard-r6。

| 负载 | 模式 | 三轮中位数 | 三轮范围 | 当前观测 |
| --- | --- | ---: | --- | --- |
| CoreMark2000 | Function | 0.982459 | 0.977064–0.997316 | 三对改善，中位约 1.75% |
| CoreMark2000 | Block | 0.960248 | 0.960037–0.997298 | 三对改善，中位约 3.98% |
| FP1 | Function | 0.969797 | 0.963224–0.982131 | 三对改善，中位约 3.02% |
| FP1 | Block | 0.992395 | 0.956696–1.018407 | 波动跨 1，未证明稳定收益 |

24 点全部通过原有 validator 重放，314 个 raw 文件的大小与摘要一致。CoreMark Block 的请求、IR 和 host-byte 计数有小差异，原样保留；其他三组工作量计数相同。计时仍为 inclusive translate，不按 IR 数归一，不相加阶段计时，不相乘历史比例。原计划的部分 limits 文本沿用了上一轮模板，实际 arms、scope 和原始输入均明确为 `02085` 对 `e9de`；独立审查单独更正模板标签，不回写已冻结计划。

首批 CoreMark 当前 translate 中位数为 Block 905.654 ms、Function 1251.543 ms。固定 FEX 的历史单槽观测为 54.522 ms；此前的计时范围限制继续适用，首批改善不能作为编译成本已接近 FEX 的依据。

## 首批正确性与生成代码质量

14 个既有 native 目标构建成功，构建前后源码稳定。两种分配模式的 FP1 共 48 个实际发射缓冲，长度、FNV 和完整机器字节均与原 R6 对应记录一致：Block 42 个单元、6736 B；Function 6 个单元、8444 B。因此本批原 FP 正常热回边仍为 51 条，未通过增加运行代码换取 compile 改善。

受影响门测共 19 份 JUnit、597 行、827098 个实际断言检查；353 个独立 native checks 通过。额外扩展的 XPOOL0 标签在两种模式各出现三例、11 个失败断言。精确重建原 R6 的 `swift_test` 后，其 SHA 与历史 `90d9db12…` 完全一致，在相同配置下逐项复现这些失败，消息、实际数值和退出码均相同；没有将它们标为通过或删除。其余已执行门无新增失败。

新增永久用例覆盖 scratch 变化点、乱序 Block、跨块交错 ID 和实际固定 home 覆写拒绝。全 IR 的 fast/slow 两引擎各执行 229553 个断言检查，零失败。原 main 与 RV 的其他历史范围限制保持不变。

原始保留的非性能错误也单独记录：C0 采集后的本地检查曾误将第七行温度纳入固定条件，实际六条件一致，原失败和更正资格均保留；首个门测部署因 adb 无权覆盖 root 文件而在 guest 启动前失败，随后通过本任务私有暂存目录部署，没有替换失败测量点。

## 第二批实现范围

第二批以首批 `e9de5574…` 为相邻参照，同时保留原始 `02085b0b…`，供最终直接对照。

1. FPR ties/publication 的首轮收集并入现有输入访问，保留反向 writes 和 value-end 的真实依赖；永久合同加入 ties-only 场景。
2. 既有 summary 证明小于 64 条、且尚无可复用目录的块，首次 clobber 使用现有语义 matcher 直接找最小后继 ID；长块和已建目录继续原路。
3. 精确 publication guard 提前排除注定不改写的候选；完整 unknown 输入的整数加减分析跳过等价逐位计算。两操作数 Query、预算及无效输入处理保持原合同。
4. 六张可选 coalescing/width/AES 表在首次物理编辑时才建立，mandatory mapping 和 live ends 保持原布局。snapshot、rollback、commit、retry 和冻结后的只读查询保留默认状态语义；新增生命周期和拒绝用例。

publication 指针投影的候选暂缓：裸 action ID 不能维持现有 foreign publication 同 ID 负测的身份合同。没有删除该负测或用不完整证据替代它。

第二批源码摘要为 `743ccaed…`，launcher 为 `8b29c2e0…`。相对首批 `e9de5574…` 的配对结果如下：

| 负载 | 模式 | 三轮中位数 | 三轮范围 | 当前观测 |
| --- | --- | ---: | --- | --- |
| CoreMark2000 | Function | 0.997438 | 0.987297–1.007853 | 波动跨 1 |
| CoreMark2000 | Block | 0.995669 | 0.949533–0.999520 | 三对改善，中位约 0.43% |
| FP1 | Function | 0.994335 | 0.952664–1.001970 | 波动跨 1 |
| FP1 | Block | 0.973681 | 0.950079–1.015278 | 波动跨 1 |

24 点原采集器均判有效。中间两批比例不能相乘作为相对原 R6 的累计收益；最终仍需直接对照。增加状态或生命周期复杂度的部分还需评估保留价值，不因减少分配调用就认定取得耗时收益。

14 个既有 native 目标构建成功。19 份 JUnit 共 601 行、828034 个实际断言检查，其中 22 个失败断言与精确原 R6 控制逐项相同；353 个独立 native checks 通过，没有新增失败。全 IR fast/slow 各 229571 个断言检查、零失败。FP1 两模式共 48 个发射缓冲继续与原 R6 逐字节一致。本批尚未完成新版本的普通 PMU 运行质量比较，这些有限结果不能代替全工作负载质量验收。

补采的当前 CoreMark IR 中，同型 U64 BitCast 的可见上界约占最终 IR 行的 4.5%。alias 自身的 pseudo、terminal/hoist 隐式根及控制边界仍须单独验证；当前转储不包含所有 Operand/hoist 信息，不能把这个上界记为可安全删除量或预估提速。暂不为这部分增加独立 pass。

## 冷诊断与同源码构建实验

第三个检查点只在第二批基础上修改 `logging.h` 与诊断测试：把 `ASSERT_MSG` 的格式化放入单个 cold/noinline helper，条件、参数延迟求值、消息和异常保持原语义。源码摘要 `707b9c9d…`，launcher `d1756eac…`。实际 text 净减 30408 B；UnUse/Validate 普通帧由 112 B 降至 32 B，无原格式化临时对象引入的 canary。Use 在 linked ELF 中已不再保留独立符号；其新 object 定义为 152 B/32 B，不能与旧 linked 符号混成同域比较。其他大函数仍可能因自身局部对象保留较大帧。

20 份 JUnit 共 608 行、828103 个实际断言检查，含原样受控的 22 个基线失败断言；353 个 native checks 通过，未出现新增失败。新增诊断用例与原诊断组共执行 69 个断言检查。FP1 的 48 个发射缓冲继续与原 R6 字节相同。

相对第二批 C3 的三轮 translate 比值如下：

| 负载 | 模式 | 中位数 | 范围 |
| --- | --- | ---: | --- |
| CoreMark2000 | Function | 0.973913 | 0.973140–1.008184 |
| CoreMark2000 | Block | 0.995660 | 0.992950–1.010188 |
| FP1 | Function | 0.990135 | 0.987027–1.003980 |
| FP1 | Block | 0.979733 | 0.961232–0.998175 |

前三组跨 1，FP Block 三对改善但中位数未到 5% 门槛。按净减少普通路径代码和栈帧保留，不记为已证明的明显 compile 提速。

ThinLTO 实验保持同一份 `707b9c9d…` 源码，仅增加标准 CMake Release IPO 和 LLD 线程/缓存参数。先构建 launcher 及依赖闭包；此前 13 个测试 ELF 仍属 C4，不能贴为 LTO 资格。原 CMakeCache、生成命令和精确 C4 launcher 已保留，完整编译/链接配置另列。没有启用 fast-math、禁异常、关闭断言或栈保护。

同源码 ThinLTO 的 24 点已经完成并独立重放，12 个前后配对均小于 1：

| 负载 | 模式 | ThinLTO/普通构建中位数 | 范围 | translate 当前中位数 |
| --- | --- | ---: | --- | ---: |
| CoreMark2000 | Function | 0.916115 | 0.915554–0.927874 | 1131.189 ms |
| CoreMark2000 | Block | 0.894520 | 0.864250–0.915155 | 813.965 ms |
| FP1 | Function | 0.942698 | 0.911403–0.944200 | 27.401 ms |
| FP1 | Block | 0.909986 | 0.903725–0.918808 | 23.313 ms |

此实验的 launcher 为 `2f1606c5…`，构建绑定摘要为 `2a65e8d2…`。48 个 FP 缓冲继续 byte-identical。ELF 文件由 116.51 MB 降至 100.52 MB，但 `llvm-size text` 聚合项增加 272214 B，不能以文件缩小称机器代码体积改善。该 text 项也不全等于 `.text` 指令节。当前证据支持进入完整资格，尚不是正式采用或 FEX 同数量级完成。

首次链接因缓存百分比写成 `cache_size=0` 而失败；保留原 failed 记录。修正为 `0%` 与显式 1073741824 B 后，复用已编译对象，重配及链接成功。15.38 s 是补充链接耗时，不能当首次全构建时间。完整 CMakeCache 还记录十项旧路径到 Data2 的生成目录刷新；主动构建选项变化为 IPO 和 Release linker flags 两项。

第二批的复杂度审查建议已应用到后续选定候选：撤回无稳定收益的六表延迟物化、短块 clobber 双路径和提前 publication guard，保留删除遍历的 FPR 首轮融合及等价 known-bits 短路。225 断言的 rollback/commit/retry 测试只查公开语义，继续保留。

## 选定组合 C6 的直接对照

C6 源码摘要 `559404cd…`、launcher `c57327da…`、源码与构建绑定摘要 `70988195…`。全 14 个目标均按同一 Release ThinLTO 配方新构建成功，耗时 244.39 s；这是主机工程构建时间。20 份 JUnit 共 608 行、828103 个断言检查，保留与精确原 R6 完全相同的 22 个失败断言，零新增失败；353 个 native checks 通过。FP1 两模式共 48 个发射缓冲继续与原 R6 逐字节相同。

当前版本直接对原始 R6 `02085b0b…` 的结果如下，四组各三对均改善：

| 负载 | 模式 | candidate/R6 中位数 | 三轮范围 | candidate translate 中位数 |
| --- | --- | ---: | --- | ---: |
| CoreMark2000 | Function | 0.905595 | 0.896584–0.906274 | 1139.200 ms |
| CoreMark2000 | Block | 0.896350 | 0.879856–0.906302 | 834.693 ms |
| FP1 | Function | 0.913470 | 0.908532–0.918333 | 27.205 ms |
| FP1 | Block | 0.908260 | 0.897567–0.917454 | 23.563 ms |

以上取每对比值的中位数，耗时列另取 candidate 的中位数，不能用两列反推逐轮数据。这是直接累计对照，没有相乘中间检查点比例；原始工作量差异全部保留，不按 IR 数归一。它完成一个约一成改善的工程检查点，尚未完成 FEX 同数量级目标。

当前普通采样另保留在 `compile-cost-c6-profile`：Function 4225 个样本、458 个错误调用链（10.84%）；Block 3781/237（6.27%）。两种模式的输入、CRC、六条件及全 14 native/源码在采样前后稳定。普通采样不作为 coarse 耗时点，也不能把 LTO 后内联归属变化当成某个源函数新增或消失的实际工作量。

## C6 普通 PMU 质量门

CoreMark2000/12000 与 FP1/2 各 36 点，共 72 个预定观测全部执行成功。原 `compare.py` 未修改，完整重放结果一致；每点 grouped cycles/instructions 均 running=enabled，输出、实际 argv/env、输入身份和六条件通过。SVM 的 PROF、PROF2、VERIFY_PLAN 及其他诊断变量完全缺席；固定 FEX 显式严格四项 TSO。两个私有 warm service 各自覆盖完整 36 点，成本单列，标准关闭、真实等待退出，无残余采集器。

以下均为 C6/原 R6 的逐轮斜率比值，再取三轮中位数：

| 负载 | 模式 | 指令中位数（范围） | cycles 中位数 | elapsed 中位数（范围） |
| --- | --- | --- | ---: | --- |
| CoreMark | Function | 0.999871（0.999870–1.003214） | 1.001869 | 1.002398（1.001782–1.002622） |
| CoreMark | Block | 0.999718（0.999713–0.999899） | 0.982273 | 0.981862（0.980578–0.984762） |
| FP | Function | 1.001187（0.996359–1.001599） | 1.000483 | 1.002519（0.997010–1.032144） |
| FP | Block | 0.998454（0.997186–1.000073） | 0.979223 | 0.973969（0.972566–0.995210） |

12 个逐轮指令比值全部不超过 1.01，按预定质量门通过；FP 48 个缓冲仍 byte-identical。Core Function 的 elapsed 三轮均有约 0.18%–0.26% 的小幅增加，FP Function 第一轮为 1.032144，原点完整保留，没有追加样本。这些结果不等于全部负载运行时间零回退，也不把 compile 改善直接写成运行加速。Block 的两个固定负载本批 elapsed 三对改善，仍是三轮有限观测。

当前/FEX strict 的探索性 elapsed 斜率中位数为 Core Function 2.345186、Block 2.627095；FP 分别为 1.001429、1.036759，范围及 instructions/cycles 全部保留在原报告。FEX 的 mode 标签只表示陪同比较的 SVM 子序列，FEX 本身只有一套固定调用策略。不能把 FP 结果推广为全部负载接近 FEX，也不能用端点截距估算 compile 成本。

## C7 DCE 容器实验：撤回

Function 的首次 flags/DCE 在 value worklist 建立前，使用逐条分配的 hash owner 目录；value epoch 内已经复用 worklist 的现有 owner 查询。C7 仅将前一种目录改为排序 vector 与 tombstone，保留原初始删点顺序和活动 epoch 的路径。独立 Block 原本反向删除，无需该目录。

候选源码 `5dd9f278…`、launcher `2598d1f5…`。14 个目标构建、828103 个断言检查（含原样受控的 22 个失败）、353 个 native checks 和 48 个 FP 发射缓冲对照完成，无新增失败。但相邻 C6 的 CoreMark 12 点没有通过采用条件：

| 模式 | C7/C6 translate 中位数 | 三轮比值 | 决定 |
| --- | ---: | --- | --- |
| Function | 0.956743 | 0.948596、0.956743、0.992946 | 未达 5% 中位改善 |
| Block | 1.011351 | 1.000015、1.015590、1.011351 | 三轮变慢且中位增加超过 1%，触发负控停止 |

保留原始点，没有追加或替换样本，也没有为该候选继续普通 PMU 采用验收。源码恢复后重建全部 14 个目标，每个 ELF 的 SHA 均精确恢复为 C6；手机本任务的五个部署文件也已恢复相同身份。失败候选源码差分、精确压缩 ELF 和独立复核均保留，不能将其收益计入当前组合。

把 value worklist 构造提前也暂缓：首次 DCE 删除节点后，原晚构造的连续位置与提前构造留下的空洞不同，会改变 Nearby 窗口、工作预算与候选队列，可能漏掉原有折叠。修复需要真正的阶段重置和重新编号工作，不能当作免费复用。

## C6 的既有 PROF2 诊断

另对精确 C6 `c57327da…` 的 Function、Block 各运行一次原 CoreMark2000，启用已有 PROF2。两次输入、CRC、退出码和六条件有效；这是有插桩开销的归因记录，不加入正式性能表，也不与普通 coarse 点混合。

Function 的 regalloc 总括号为 319.839 ms，其中公开 AllocateFunction/AllocateBlock 计时合计 146.741 ms；Block 为 232.595/105.298 ms。相减得到的 173.098/127.297 ms 还包含准备、构造、析构和计时开销，不能命名为纯 Describe 成本。

target Complete 为 Function 187.882 ms、Block 164.492 ms；plan_declare 为 78.216/66.017 ms，其 plan_perblock 60.141/52.144 ms 是嵌套范围。mandatory verify 为 18.918/13.307 ms，保留生产检查。两种模式的 FPR contract retry 均为零。后续优先审查实际输入准备和 declaration 工作，不围绕本批没有发生的重试增加机制，也不把这些嵌套计时相加。

## C8 已知位加减分析：撤回

只修改现有 `analysis/value_bits.cpp` 的加减已知位推导：固定次数的无符号加法和位运算替换最多 64 次的逐位状态循环，删除旧表。常量、完整 unknown、identity 短路，以及 Query、32 次查询预算、无效输入和 sign 的合同保持原样；不增加状态、缓存或 pass。独立具体整数枚举和全部 1–64 位边界验证了与旧算法相同的精度，而非只证明一个更弱的安全结果。

源码 `7110a973…`、launcher `f5e469d2…`，14 个 Release ThinLTO 目标构建成功。候选的两个公开 IR 用例覆盖小位域全部 ternary 组合、跨字节/字/双字/64 位的进借位、Neg、sign、视图、预算、cycle 与畸形输入；每个完整 IR 引擎新增 53008 个实际断言。20 份 JUnit 共 612 行、934119 个断言检查，其中 22 个失败与精确原 R6 控制逐项一致，零新增失败；353 个 native checks 通过。48 个 FP 缓冲继续与原 R6 逐字节相同。

预定 CoreMark2000/FP1、两种模式、三轮共 24 点执行完成，对照精确 C6，同一构建配方，原采集器不变：

| 负载 | 模式 | C8/C6 translate 中位数 | 三轮范围 |
| --- | --- | ---: | --- |
| CoreMark2000 | Function | 0.994494 | 0.989600–1.001240 |
| CoreMark2000 | Block | 1.038216 | 1.017772–1.067411 |
| FP1 | Function | 1.018199 | 0.988842–1.018478 |
| FP1 | Block | 1.039349 | 1.003946–1.055202 |

两种 Block 子序列均三轮变慢且中位增加超过 1%，触发预定停止条件。没有补采、替换点或继续普通 PMU 采用验收；完整源码、测试、数学证明、汇编和压缩 ELF 作为失败候选保留。两文件撤回后，全部 14 个重建 ELF 精确恢复 C6。机器码中 Add 由 660 B 减至 544 B、64 B 表被删除，并没有转化为本批整体编译收益；其他阶段和工作量差异也原样保留，不据此认定变慢的根因。

## C11 消费者局部搜索：暂存精简，未证明提速

`register_versions.cpp` 在每条实际指令的输入访问前记录 use 表长度，仅在本条指令已经追加的后缀中查找重复 `(definition, consumer)`。之前的记录属于不同消费者，无法匹配。改动只有一个局部 offset 和搜索起点；保留 GetValues、最后一个合格宽度 view 覆盖、逐 occurrence 计数、窄 Sub 拒绝、fault 顺序及最后排序。整个方法中的全局 use-count 查询仍可能线性，不能据此称整个方法已变为线性。

源码 `20b38b28…`、launcher `da5fad49…`，14 个目标按相同 Release ThinLTO 构建。新增公开语义用例覆盖 16 个先前消费者、同定义的 64/32 位输入交换顺序、18 个唯一 use 记录与 20 个实际 occurrence，以及重复 preparation 的完整结果一致。21 份 JUnit 共 626 行、863082 个断言检查，其中原 22 个失败与 R6 控制逐项一致；353 个 native checks 通过。FP 两模式 48 个缓冲仍与 R6/C6 逐字节相同。

前 20 份回归先执行完成，额外 block-analysis 套件的调度修改晚于原采集器启动，没有进入该进程。随后仅执行缺少的完整 `[block-analysis]` 套件一次，18 行、34979 个断言通过，确认新增用例恰好执行一次；前 20 份没有重跑。原执行记录与补充记录分别保留，以上为两者的合计。其后才冻结性能计划。

相邻 C6 的预定 24 点结果如下，四组范围均跨 1，没有追加或替换样本：

| 负载 | 模式 | C11/C6 translate 中位数 | 三轮范围 |
| --- | --- | ---: | --- |
| CoreMark2000 | Function | 1.002626 | 0.993687–1.013063 |
| CoreMark2000 | Block | 1.000790 | 0.992646–1.048379 |
| FP1 | Function | 0.996397 | 0.990788–1.018921 |
| FP1 | Block | 0.999477 | 0.974381–1.005654 |

没有达到 5% 采用目标，也没有触发三轮同向变慢的停止条件。仅暂存这一处局部、可证明删去无效比较的精简，不声称整体编译改善；普通 PMU 与最终直接原基线对照仍须在选定组合上完成。Core Block 的实际 IR、单元和 host-byte 差异完整保留，没有归一。

## C9 冷诊断模板：FP 小幅改善，组合资格待定

在暂存 C11 基础上，仅给现有 DiagnosticFormat/Print 两模板增加 cold/noinline 属性，未改变函数签名、宏外层过滤、函数内层过滤、消息、参数求值和异常。源码 `a3ad471b…`、launcher `b9478294…`，14 个目标以相同配方构建成功；280.55 s 是主机工程构建时间。新增诊断回归覆盖 disabled direct call、参数关闭 channel、格式/参数异常、临时 format/value 生命周期及精确 stderr，两模板合计诊断组实际为 91 个断言。

21 份 JUnit 共 627 行、863104 个断言检查，原 22 个失败与 R6 控制逐项一致；353 个 native checks 通过。48 个 FP 发射缓冲与 R6/C6 逐字节相同。相邻 C11 的预定 24 点如下：

| 负载 | 模式 | C9/C11 translate 中位数 | 三轮范围 |
| --- | --- | ---: | --- |
| CoreMark2000 | Function | 1.002955 | 0.983830–1.006963 |
| CoreMark2000 | Block | 0.984376 | 0.983464–1.014015 |
| FP1 | Function | 0.976384 | 0.953983–0.990001 |
| FP1 | Block | 0.980199 | 0.968806–0.984404 |

FP 两组各三轮改善，分别约 2.36%/1.98%；Core 两组跨 1。没有达到 5% 目标或触发负控停止条件。暂存两个属性的修改，等待选定组合的普通 PMU 和直接原 R6 对照，不作为全部负载明显提速或同数量级目标完成。

实际汇编中，AllocationView::ResolveId 从 440 B/128 B 帧变为 292 B/80 B 帧，仍有 canary。ValueType/ValueGPR 两个 getter 内联解析后，自身变大，但普通调用链的同时栈帧从 32+128 B 变为 80 B，少一次 ResolveId 调用。独立诊断模板更多地保留为函数，整体 `.text` 增加 18980 B；没有将栈缩减解释为全局代码缩小或整体提速。冷诊断路径的总峰值栈未在本项证明。

## C14 相同源码的调度实验：拒绝

实际 NDK driver 两次 `-###` 比较只有 tune-cpu 不同，generic CPU、ISA features、ABI、API28 保持一致。固定 FEX 原构建同样为 generic/none，这不是原始差距的已证成因。实验仅改变两个 Release flags，源级 crypto、O3/NDEBUG、ThinLTO、异常和栈保护保留。

源码与 C9 同为 `a3ad471b…`，调度版 launcher `3dbb869d…`。14 个目标构建耗时 319.37 s，是主机工程时间；21 份 JUnit 共 863104 个断言检查，保留 22 个精确原 R6 失败，零新增失败；353 个 native checks 通过。48 个 FP 缓冲仍与原 R6/C9 逐字节相同。同源码原 C9 `b9478294…` 的 24 点对照如下：

| 负载 | 模式 | C14/C9 translate 中位数 | 三轮范围 |
| --- | --- | ---: | --- |
| CoreMark2000 | Function | 1.000872 | 0.983097–1.013209 |
| CoreMark2000 | Block | 0.981604 | 0.976890–0.996232 |
| FP1 | Function | 0.989546 | 0.963893–1.019049 |
| FP1 | Block | 1.036594 | 1.024090–1.090669 |

FP Block 三轮均增加且中位超过 1%，按预定条件拒绝，没有追加或替换点、没有继续普通 PMU 采用验收。全部工作量差异、六条件、原始 PMU 和精确调度版 ELF 保留。

两项 flags 已恢复为空，全部 typed cache 值与 C9 一致，生成的 Ninja 文件也逐字节相同。最初回退校验把两处 CMake 自动更新的帮助注释纳入原始 cache SHA，触发过严断言；原失败保留，另用完整字段与生成命令核对收口。原 C9 cache 为 `006556ca…`，实际恢复 cache 为 `6af59b1f…`，未假称两文件逐字节相同。

调度版 14 个旧 ELF 没有重贴成 C9；下一次全构建直接验证 C10 的新源码。相邻参照仍为精确保存的原 C9，拒绝候选只留归档。最新完整普通 PMU 资格仍属于 C6。

## C10 输出容量已满时提前返回：拒绝

只在已有 decompose 路径中，结果计数和 nextOffset 已发表、全部显式 STOP 返回处理后，非 filtered/非 textual、容量已满且仍有输入时返回原 MEMORYERR。原 prefix 和容量检查留在原位，fetch、PC、IR 提交、SMC 范围不变；原 BSD 版权头保留并标注 SwiftVM 本地修改。

源码 `8e93aa45…`、launcher `638fe5e5…`，14 个目标以恢复后的通用 Release ThinLTO 构建。22 份 JUnit 共 631 行、911028 个断言检查，含精确复现的 22 个原 R6 失败，无新增失败；353 个 native checks 通过。完整 distorm-fast 套件为 47924 个断言，覆盖既有差分用例及新增的 12 输入 × 两模式 × 三容量 72 次 API 调用。48 个 FP 缓冲仍与 R6/C9 逐字节相同。

实际汇编确认，容量已满时跳过下一轮 PrefixState 存储、prefix 检查及可能的 prefix 扫描；decode_internal 从 3332 B 增至 3348 B。旧容量检查本来就在第二次 decode_inst 之前，因此没有“少解码一条指令”的收益主张。

相邻 C9 的 24 点结果如下：

| 负载 | 模式 | C10/C9 translate 中位数 | 三轮范围 |
| --- | --- | ---: | --- |
| CoreMark2000 | Function | 1.005558 | 1.002082–1.010002 |
| CoreMark2000 | Block | 1.012077 | 1.001529–1.026857 |
| FP1 | Function | 1.016324 | 1.006939–1.021952 |
| FP1 | Block | 1.010498 | 0.982381–1.042117 |

Core Block 和 FP Function 各三轮均增加且中位超过 1%，按预定条件拒绝；没有追加点或普通 PMU 采用验收。源码和新增测试以拒绝快照保留。两文件恢复后，14 个重建 ELF 逐个 SHA 精确等于 C9，手机五文件同样恢复；据此复用原 C9 回归资格，不重复执行相同测试。

## C17 同一 ELF 的 A/A 校准

多项窄候选的差异仅约百分之几，另开独立的测量校准。两个路径均使用精确 C9 `b9478294…`、相同源码和通用编译配置，原 24 点顺序、CoreMark2000/FP1、两种模式及 validator 不变。24 点全部完成，并经独立重放核对 314 个 raw 文件、36 项实际 artifact 身份及原始指标。

| 固定输入 | 模式 | 同 ELF 的角色比中位数 | 三轮范围 |
| --- | --- | ---: | ---: |
| CoreMark2000 | Function | 0.995913 | 0.989538–1.011531 |
| CoreMark2000 | Block | 0.994578 | 0.990011–1.033812 |
| FP1 | Function | 1.000842 | 0.984285–1.002520 |
| FP1 | Block | 0.993622 | 0.968958–1.038212 |

四组范围均跨 1，12 对中 7 对小于 1、5 对大于 1。这次观测不提供正式的噪声置信区间、独立性或角色偏差结论；不能把它当优化收益，不能替换、补齐或撤销已有拒绝结果。

后续候选和测量规则只在新的预定批次中采用，不回写本轮历史。原始工作量差异继续保留，不按 IR 数归一。

## C18 Opcode 参数布局：暂存清理，未证明实质提速

仅修改 `ir_types.h`、`instr.h` 和既有 `ir_contract_test.cpp`，对照精确 C9 `b9478294…`。七参数 constexpr 构造函数把首 Operand 的逻辑位置写入原 IRMeta padding，GetArg 的非零索引据此计算物理位置。原 bounds/type 检查、精确错误文字及 metadata 常量初始化保留，没有逐实例或逐编译单元缓存。构造函数使类型失去 aggregate 属性，仓库实际初始化和消费者已核对；没有机械更改 cache51。

独立合同检查覆盖 191 个 schema、420 个有效逻辑位置，并检查更广的签名组合；新增永久回归为现有测试中的 25 项公开 getter 合同，包括 Operand 后的 value、四参数非 Operand 指令以及负数/越界索引。实际 LP64 native 的 IRMeta 仍为 48 B，所有旧字段 offset 不变，ELF 的 194 项表仍为 9312 B；ILP32 仅有布局分析，未实际构建。

14 目标构建完成。22 个 JUnit 套件有 628 个 case rows、863924 个断言检查，其中 22 个历史失败与精确原 R6 逐项相同，没有新增失败；353 个 native checks 通过。FP Block 的 42 个缓冲共 6736 B、Function 的 6 个缓冲共 8444 B，全部与原 R6 的对应 PC、顺序及实际字节相同。

实际 native 对照中，代表性 Query 的非零参数访问少一次依赖读取；通用 Value/Imm getter 从 560 B 缩至 180 B，Query 为 3556→3548 B，自身帧保持 336 B/canary。所见动态 index 0 路径仍避开 descriptor，不能泛化为全部机器调用路径。整体 .text 减少 19100 B，ELF 文件增加 848 B；没有据此推算速度。

采样策略在应用源码、构建和性能观测前冻结：两负载 × 两模式 × 前后 × 七轮，共 56 点，复用原偶/奇四臂顺序。producer 只改说明文字、最终点数和 summary 轮数，validator AST 与原版相同。性能采用要求相关负载七对全部改善且中位至少改善 5%；负控七对全部变慢且中位达到 1% 时暂缓。56 点完成，没有追加或替换；独立重放核对全部 730 个 raw、44 项 artifact 和 28 个配对结果。

| 固定输入 | 模式 | C18/C9 translate 中位比 | 七轮范围 |
| --- | --- | ---: | ---: |
| CoreMark2000 | Function | 1.002422 | 0.983441–1.010892 |
| CoreMark2000 | Block | 0.987566 | 0.969520–1.011447 |
| FP1 | Function | 0.987705 | 0.968828–1.041235 |
| FP1 | Block | 0.998409 | 0.943223–1.020002 |

四组范围均跨 1，未达到 5% 提速门，也未触发七轮一致的负控停止条件。Core Function 的 r2/r4 分别存在一个请求/decoded block、15 个 IR、92 B host code 的工作量差异；Core Block 有六对工作量不同。全部原始差异保留，没有归一化或称两边工作量相同。FP 两模式各七对的已记录工作量相同。

暂存为净结构清理，保持 provisional：唯一构造器从既有 schema 计算只读字段，复用 constinit owner，没有实例缓存、分配或失效机制，并确实移除了 getter 的签名前缀依赖。维护代价是源码增加和失去 aggregate 属性，不能称类型 API 完全不变。完整采用仍需选定组合的直接原 R6 对照和普通 PMU，静态 text 和本轮中位数不代表稳定提速。精确 launcher 已压缩保留，26282620 B；全部读者和测量作业已退出。

## C19 标准 PGO：编译与既定指令质量检查点收口

NDK 29 的实际 Clang 21 driver dry-run 支持 IR-based generate/use 与 ThinLTO 配合，后续实际插桩构建已完成。配方包括原参数及回退、Data2 固定缓存、训练期 atomic counter update、独立 profile 命名空间及同工具链严格合并。训练计划只复用既有 CoreMark2000/FP1 × 两种模式的四次正常退出运行；不作为性能结果，也不代表通用训练覆盖。

前置 C20 已收口，选为同源码非 PGO 参照；暂存组合不自动成为已完整采用版本。C19 的原始完整 cache/typed values、Ninja、工具身份及精确回退 argv 已保存。实际配置只改六个字段，87 个 C 和 754 个 C++ 规则均含 generate/atomic 选项，完整 Ninja 图剔除预定选项及 gen-cache 路径后与原图等价，其余 typed cache 值一致。

generate 只构建 launcher 及依赖，工程构建耗时 134.312 s。实际训练 ELF 为 `86593fa4…`、134421528 B，LLVM profile 数据段、runtime 和写出入口已检查；该阶段 13 个原测试 ELF 逐个 SHA 不变，没有冒称全 14 个 PGO 资格。

首个训练设置在向 root 运行目录直接 adb push observer 时权限失败，尚未出现任何 native execute 命令。原脚本、计划、失败记录和日志保持原 SHA；v2 只换私有输出命名空间，并使用 shell 临时上传后由 root 安装，四个场景、输入、顺序、oracle 与校验不变。v2 的 Core Function、Core Block、FP Function、FP Block 各实际执行一次，全部正常 wait 退出 0。64 个 run 文件含 4 份 raw，共 6794704 B；六条件、PID/starttime/affinity、实际 env/argv 和 oracle 已独立核对。

同 NDK 的 llvm-profdata 对每份 raw 完整解析，随后以 `--failure-mode=any` 严格各合并一次，得到 `59038523…`、2173952 B 的 indexed profile。7181 个函数记录和 82516 个计数点仅说明有效 profile 内容，不代表全路径覆盖或性能；raw 与 indexed 格式版本分别记录，不强求数值相同。训练数据和插桩时间没有进入性能表。

use 阶段实际完成 14 目标重建，工程耗时 333.416 s；此时间也不是 guest JIT compile 耗时。主 ELF `5a94db9e…`、103200232 B，无 profile 数据段和全部 defined/undefined runtime/counter 符号。source/build/config/profile/tool 绑定摘要为 `216d8c86…`，源码与 C20 的 2418 个文件逐字一致。配置仍只在 Data2 固定 cache 中实验，未改仓库默认选项。

真实 PGO 警告共 12 条，全部来自未训练的测试/Catch main 符号，LLVM 因 hash 不匹配拒用这些不相关计数；生产相关为 0，原告警未抑制。原脚本的宽词法字段收进 796 条 verbose 命令且没有覆盖这 12 个真实 header，已另存精确分类，不把该字段当警告数量或完整清单。

22 套件有 628 个 case rows、863924 个断言检查，原 22 个失败与精确 R6 相同，没有新增失败；353 个 native checks 通过。FP Block 的 42 个缓冲/6736 B 和 Function 的 6 个缓冲/8444 B，均与 C20 和原始 R6 的对应 PC、顺序及实际字节相同。

七轮 56 点均已完成，使用原 producer、顺序、env 与校验器；性能环境没有 PROF2 或 LLVM_PROFILE_FILE。28 对全部更快，四组均达到预定 5% 门：

| 固定输入 | 模式 | PGO/C20 translate 中位比 | 七轮范围 | 中位减少 |
| --- | --- | ---: | ---: | ---: |
| CoreMark2000 | Function | 0.888000 | 0.868194–0.896988 | 11.20% |
| CoreMark2000 | Block | 0.819807 | 0.808482–0.844857 | 18.02% |
| FP1 | Function | 0.896223 | 0.872686–0.909296 | 10.38% |
| FP1 | Block | 0.836739 | 0.811708–0.852261 | 16.33% |

Core Function 两对、Block 七对有实际工作量差异，原样保留；FP 两模式相同。该小样本门不提供统计显著性或相同 miss 分母。完整独立重放已核对 84 项实际 artifact、730 个 raw 文件及全部 28 对，结果与原汇总一致。

另一个预先冻结的独立七轮 56 点批次，直接比较当前 PGO 与精确原 R6 `02085b0b…`。原始 R6 保持独立 host/phone 角色，未被 C20 的相邻参照覆盖；491 文件历史 overlay 与当前 2418 文件完整清单分别保留原摘要，没有跨域源码相同或完整差分主张。实际 82 项 artifact、730 个 raw、28 对结果均经独立重放，全部 28 个比值小于 0.95。

| 固定输入 | 模式 | 当前/R6 translate 中位比 | 七轮范围 | 原 R6 / 当前耗时中位数 | 配对中位减少 |
| --- | --- | ---: | ---: | ---: | ---: |
| CoreMark2000 | Function | 0.794073 | 0.777783–0.808221 | 1259.934 / 1002.443 ms | 20.59% |
| CoreMark2000 | Block | 0.703080 | 0.670476–0.705076 | 942.445 / 660.198 ms | 29.69% |
| FP1 | Function | 0.792009 | 0.748276–0.805572 | 30.291 / 24.051 ms | 20.80% |
| FP1 | Block | 0.729031 | 0.712424–0.751878 | 26.310 / 19.175 ms | 27.10% |

Core Function 四对、Block 七对有已记录工作量差异，全部保留；FP 两模式相同。以上是直接累计对照，没有相乘此前比例，也没有按 IR 或请求归一化。

另一套不含诊断变量的普通 PMU 72 点及独立重放均完成：两负载、两模式、R6/当前/固定严格 TSO FEX 三臂、两档工作量、三轮。944 个原始设备文件、两套完整 comparison 和各点真实身份、环境、argv、退出、CPU4 grouped PMU、oracle、六条件均核实。两个私有 FEX 服务按原精确 PID/starttime/exe/env 生命周期标准关闭，transport 退出 0；服务全生命周期计数单列，没有摊入 primary。

12 个逐轮 instructions 斜率比均不超过既定 1.01 门，实际范围为 0.999781–1.003017。以下为逐轮比值的中位数；斜率仍包括随工作量变化的 native/runtime 工作，不把截距称为纯编译成本。

| 固定输入 | 模式 | 当前/R6 instructions | 当前/R6 cycles | 当前/R6 wall | 当前/严格 FEX wall，探索性 |
| --- | --- | ---: | ---: | ---: | ---: |
| CoreMark | Function | 1.000038 | 1.002678 | 1.002642 | 2.344985 |
| CoreMark | Block | 0.999906 | 1.007347 | 1.006772 | 2.667288 |
| FP | Function | 1.001666 | 1.010824 | 1.009612 | 0.993606 |
| FP | Block | 1.000693 | 1.007177 | 1.008206 | 1.032231 |

CoreMark 两模式的 wall/cycles 三轮均增加；Block 最大 wall 增加 1.624%。FP wall 范围分别为 Function 0.979477–1.047149、Block 0.966773–1.022823，全部原点保留。有限指令质量门与 48 个 FP 字节对照通过，不能据此宣称全部运行时间零回退或通用程序质量保证。

选为当前编译与既定指令质量工程检查点，完整 FEX 同数量级目标继续保持未完成。另复用原 collector 完成两份当前 PGO 普通调用链，仅作热点归因：Function 3879 个样本，原 symbol-only 报告列 215 个 error callchains（5.543%）；Block 为 3412/229（6.712%），lost 均为零。22 个 raw、15 条命令、输入身份、六条件、CPU4 进程树及候选 build ID 已离线核对。后续 N0 证明其中包含报告聚合偏差，不能全归为物理 FP 展开损失；原 raw 和报告保持。采样占比不换算成 coarse 毫秒，也不与旧 C6 的归一化占比直接比较成本。

## C20 Scratch 最终 buffer 所有权：暂存清理，未证明实质提速

只修改 `allocation_preparation.h/.cpp`。StoreScratch 改为按值接收 ScratchActions，保持所有原 ASSERT 谓词及顺序，先检查 ranges/inputs 的边界与定义身份，再把两种 payload 各自的首个 buffer 转交 UnitActions，后续结果 append。scratch 计算、capacity growth、块处理时点、最终排序和重复/冲突处理不变，没有增加缓存或修改分配模式。

现有七个调用中，四个传 prvalue，包括实际 runtime 调用；三个 named 测试保留原副本用于独立 oracle，按值接口允许这些有意复制，无须重排测试。没有新增镜像测试。成功结果的行序与边界保持，异常仍依赖既有 unit 级 Abort；不宣称单次 StoreScratch 或显式 move 实参具有强回滚保证。

14 目标通用 Release ThinLTO 构建完成，工程构建耗时 123.893 s，源码和 cache/Ninja 前后身份一致。该时间是工程构建成本，不是 guest JIT compile 耗时。22 套件有 628 个 case rows、863924 个断言检查，含与精确原 R6 一致的 22 个受控失败，没有新增失败；353 个 native checks 通过。48 个 FP 缓冲与 C18/R6 的对应 PC、顺序和实际字节相同。

预定七轮 56 点策略在源码应用前冻结；56 点均完成。独立重放核对了 730 个 raw、28 对和 46 项 artifact，producer 与 C18 逐字相同。四组范围都跨 1，没有达到 5% 提速门，也没有触发七轮一致的负控停止条件。

| 固定输入 | 模式 | C20/C18 translate 中位比 | 七轮范围 |
| --- | --- | ---: | ---: |
| CoreMark2000 | Function | 0.990354 | 0.967676–1.025745 |
| CoreMark2000 | Block | 0.993682 | 0.979948–1.023514 |
| FP1 | Function | 0.993340 | 0.976122–1.026277 |
| FP1 | Block | 0.985236 | 0.951072–1.018421 |

Core Function 有五对实际工作量不同，各增加一个请求/decoded block、15 个 IR 和 92 B host code；Core Block 七对均有差异。全部原始差异保留，未按 IR 或单元归一；FP 两模式的已记录工作量相同。离线资格汇总首次误将 Catch 的可执行名 classname 前缀也要求跨角色相同，原失败记录保留，随后改为精确核验各角色 classname，再比较其余全部失败属性和正文；没有重跑测试或性能点。

四次既有 PROF2 容量诊断及 52 个 point raw 已独立重放。临时/冻结是不同阶段快照，不能相加当峰值，也不比较插桩耗时。

| 模式 | C18 frozen capacity | C20 frozen capacity | 原始差值 |
| --- | ---: | ---: | ---: |
| Function | 2151796 B | 2184876 B | +33080 B，+1.537% |
| Block | 1832312 B | 1403872 B | −428440 B，−23.382% |

这四点也有工作量差异：Function 少一个 unit/decoded block、15 个 IR、14 个 range；Block 的 unit/decoded 相同，但少 15 个 IR/12 个 range、host bytes 增加 4。不能当作同工作量等比改善。诊断的 host before/after 分别核 19/21 项 artifact，14 目标资格来自绑定的实际 build/native/gates，没有称四点当场逐个重哈希全部测试 ELF。

实际汇编确认 fresh destination 的首 buffer 路径只转移三指针并清源，没有新分配或深拷贝；prvalue 参数也没有额外拷贝。后续 append 仍可能分配和复制，没有全进程 malloc 次数证据。StoreScratch 自身帧 144→96 B，Complete 1680→1712 B，native .text 增加 264 B，不能据此推出总峰值。

按预声明的净结构清理例外暂存，不称为单包提速或普遍内存改善。精确主程序已保留为单成员压缩包 24196080 B，差分与两源文件快照均已保存；本包读者及测量作业已关闭。随后作为 C19 同源参照，选定组合已完成直接原 R6 与普通指令质量门，Function 容量和 native 帧增加等取舍继续保留。

## 原 C20/C21 结构工作包

结合固定 FEX 的 RA 直接形成物理结果、后端单次消费的做法，当前源码确认两项候选。C20 先让 scratch 最终 buffer 交给已有 UnitActions，删除 `StoreScratch` 的首块重复分配和复制；随后按实际最后消费者再审查 memory/integer-flags payload。C21 单独审查最终 scratch、width 查询摘要与 flags-only-spill 的外围扫描共享，完整 width/fixed 证明和事务顺序保持。

C20 scratch 首批作为净清理进入 C19 组合，PGO 检查点已收口。C21 尚未应用或测量，后移 scratch 仍缺跨块 recolor 和最终 generation 的证明。该整包当前暂缓，不再列为立即实施；原依赖限制见方案 8.6，新的排序和进入条件见方案第 4 节。

## C24 普通 Block 精确故障站点批次：撤回

依据当前 PGO 热点和实际发布顺序，普通 Block 的精确站点在入口公开前，使用已完成的 emitter 相对地址向量。唯一 FaultMetadata 定义移至既有 fault_entry.h；Module 同步接收 span，catalog 一次查找 owner、分配并形成最终 Snapshot，再按原 SC 协议发布。保留逐站点检查、原 lower_bound 输入顺序、empty 无操作、whole/backedge 调用，以及 Function/HIRBlock/RV/cache 的原发布位置。没有另加 owner 索引或绝对 FaultEntry 镜像。

8 文件已冻结并独立审查，源码 `87fa02df…`、source/build 绑定 `5b53aa2d…`。通用 Release ThinLTO 的所有 typed cache 值与原同源参照一致，Ninja 图逐字相同，原始 cache 的 CMake 帮助文字差异单列；没有 PGO 选项。16 目标实际构建耗时 307.086 s，这是工程构建时间。main 为 `2012db5a…`，16 个 ELF 与构建前后源码、配置均已登记。

原 22 套件的 628 个 case rows、863924 个断言检查保持，原 22 个失败对精确 R6 的完整属性和文字相同，没有新增失败；原 353 个原生检查通过。新 batch 的旧 Add 序列 oracle、OOM、single/batch signal、单次快照导出、并发、seal、late/empty 和 reader grace 等实际测试也已运行：Fault4/Terminal6 共 2992 个检查通过，合计原生检查为 3345。OOM 可见状态检查结合源代码异常保证，不称为一般堆泄漏检测；并发覆盖每个 reader 看到过完整批次，没有保证最后一代专门被观察。

FP48 实际 PC、顺序和字节均等于 C20/R6，预定七轮 56 点也已完整运行并独立重放 730 个 raw 和全部 28 对。结果如下：

| 输入 | 模式 | C24/C20 translate 中位比 | 七轮范围 |
| --- | --- | ---: | ---: |
| CoreMark2000 | Function | 1.006291 | 0.986148–1.019320 |
| CoreMark2000 | Block | 1.017876 | 0.978874–1.022800 |
| FP1 | Function | 1.016165 | 0.971761–1.025206 |
| FP1 | Block | 0.985615 | 0.978024–1.022091 |

CoreMark Block 三快四慢，未满足预先冻结的七对全部更快且中位改善至少 5%。四组范围都跨 1，没有触发另一条“七对全慢、中位至少 1.01”的正式负控；正门失败已足以撤回新增 API，不能凭少调用次数保留。Core Function 四对、Block 六对有工作量差异，原样记录，FP 相同；没有归一化、补点或重采。

候选 ELF 已压缩为 26299173 B 并流式读回核对，8 文件 before/after、差分、新测试和原始结果均保存。8 文件随后精确恢复 `0b283cbb…`，全部 2418 源文件及 118 条退役路径核验。为下一项独立实验继续复用通用缓存，没有做一次冗余 PGO 重建后再切回通用配置；选定 C19 PGO ELF/profile/资格保持原身份，不把 live cache 内被撤回的实验 ELF 贴成 C19。

## C25 发射 scratch scope 所有权：性能门未通过，生产修改撤回

当前 PGO 调用链和代码共同确认：extended pool 路径每次 BeginVixlScratch 都 make_unique，End reset/free，TickIR、terminal 和 cold 发射反复经过该边界。原 VIXL scope 在 ARM64 实物中为 32 B，JitContext 为 1368 B。整套 MacroAssembler 的 4 KiB 缓冲复用另行暂缓，Function 多 region 已共享其首个 context 的 assembler，不能重复算收益。

本项只改 jit_context.h/.cpp 两行：unique_ptr 改为 optional，make_unique 赋值改为 emplace。原 bool、arrow、reset、Close→SvmEndScratchContract、预算检查和嵌套/异常销毁次序保持；现有 DeleteCopyAndMove 保证内联对象地址稳定，XPOOL0 继续不构造该 scope。没有增加 VIXL API、手工状态位、TLS 池或长期缓冲。源合同已独立审查。

七轮 56 点策略在改源前冻结，CoreMark 两种模式独立报告，至少一组要求七对全部更快且中位改善 5%；四组继续适用既定负控。16 目标通用构建完成，工程耗时 174.979 s，main 为 `c74b2bf5…`。实物 JitContext 为 1400 B，比参照增加 32 B；独立 Block 翻译固定帧为 5232 B，也增加 32 B。Begin/End 的 new/delete 已消失，自身固定帧各减少 16 B，不能将这些局部帧变化相加当运行峰值或提速证据。

r1 的首个冷 indirect-target 定向命令未通过：夹具的 FPR mask 将全部寄存器标为不可用，在 AllocateBlock 阶段即以 pool 3/0 拒绝，未进入预期冷发射及四项反汇编断言。精确原 R6 `90d9db12…` 在相同环境复现完整错误，r1 仍记为失败，后三个命令未执行，没有性能点。失败测试 ELF `8af01942…` 已压缩为 63295197 B 并流式核对，原始 JUnit/退出/身份记录保持。

独立夹具审查后另立 r2，在任何性能观测前保留原七轮策略；仅将 FPR mask 改为正常可用并说明目的。GPR 仍只留 x8/x9/x10，四项断言和生产预算检查保持。r2 源码 `7e80f6ba…` 相对 C20 涉及三文件；16 目标重建只更新 swift_test，另 15 个 ELF（含 main）与 r1 完全相同。修正后的四个定向命令通过 24 项断言。同夹具旧所有权控制另从一致的 C20 生产源码构建，唯一测试 ELF 为 `0d8578de…`，也通过相同四命令及 24 项断言；压缩保留后恢复候选并重建，全部 16 个 ELF 精确等于 r2，未混用 context 布局。

原 22 套件的 863924 项断言含 22 项精确 R6 历史失败，没有新增失败；原 353 项 native checks 与额外 Fault4/Terminal6 的 993 项全部通过。FP48 的 PC、顺序和实际字节均等于 C20/R6。原失败 r1、实际执行的 collector 源码与后续上传暂存目录名修正分别保留，四个测试没有重跑或替换。

七轮 56 点与独立重放全部完成，核对 93 项 artifact、730 个 raw 和 28 对结果；原环境、CPU4、设备六条件、退出和 oracle 均有效。计时比值为 candidate/C20 非 PGO：

| 负载 | 模式 | 七轮中位数 | 范围 | 更快／更慢 |
| --- | --- | ---: | --- | --- |
| CoreMark2000 | Function | 0.988763 | 0.976529–1.009396 | 6／1 |
| CoreMark2000 | Block | 0.989275 | 0.950854–1.027561 | 4／3 |
| FP1 | Function | 0.994928 | 0.944717–1.012126 | 4／3 |
| FP1 | Block | 0.983771 | 0.952562–1.022996 | 4／3 |

两个 co-primary 均未通过事先冻结的七对全部更快且中位至少改善 5% 门。四组跨 1，没有任组触发七对全慢的正式负控；拒绝依据是正门失败，不是已证一致退化。Core Function 四对、Block 七对有工作量差异，FP 全相同；保留所有差异，没有归一化、补点或重测。

生产两行已经恢复，独立验证过的夹具修正保留；完整源码 `8401918b…` 恰等于 C20 生产源码加该测试修正。候选 main `c74b2bf5…` 已单成员压缩为 26285765 B 并流式核对，源码差分及全部结果保全。恢复 16 目标实际构建成功，原 C20 的 13 个非 swift_test 产物逐字节一致，修正夹具 swift_test 与控制组 `0d8578de…` 一致，额外两目标登记实际身份。手机主程序已恢复选定 PGO `5a94db9e…`，原 R6 与 previous 角色未动；host 为通用构建，没有冒称全 14 个 PGO 产物恢复。本线没有活跃测量任务，私有上传暂存及展开的临时 PGO 主程序已移除。未启动本候选的 PGO、直接原 R6 或普通 PMU，选定 C19 检查点保持。

## C26/C27：完成对照，均撤回

C26 仅删除构造期第一次 clobber 构建，保留 post-Install 权威构建；四个直接 GPR/width 测试 helper 显式建立原前置输入。C27 随后独立把 active list/vector 合为一个 vector，slow 顺序和 fast heap、MEM 生命周期与分配策略保持；C27 没有带入 C26 删除。

两批各完成 16 目标通用构建、源审查、实物核对、质量门、FP48 实际字节核对，以及原协议七轮 56 点和完整独立重放。每批候选 Catch 合计 716 行、867964 项断言，包含原样复现的 22 项 R6 历史失败，没有新增失败；1346 个 native checks 通过。新增测量覆盖使用七个既有案例的完整 SECTION，未新增永久测试；source840 的 84 行/4016 项基线控制独立列出，不加入候选总数。

| 负载与模式 | C26 / 通用基线 | C27 / 通用基线 |
| --- | ---: | ---: |
| CoreMark Function | 1.001054 | 0.997848 |
| CoreMark Block | 1.018476 | 1.007654 |
| FP Function | 1.006813 | 0.993502 |
| FP Block | 0.986538 | 0.983840 |

C26 的 CoreMark Block 七对全慢，触发预定负控；C27 四组范围均跨 1，两 co-primary 未达 5% 正门，没有触发正式负控。两批都没有补点或更换观测，均撤回，不因减少调用或节点分配保留。

原 baseline XPOOL0 的收集器曾误把七个 TEST_CASE 当成七行 JUnit，实际 VM 全部 42 个 SECTION rows / 2008 项断言通过；收集器 failed 和缺少 postidentity 的限制保留。按完整名册独立接纳原 raw 后，只接续未执行的 XPOOL1，没有重跑 XPOOL0。旧 14 目标元数据比较也在任何性能点前改为实际 16，原错误保留。详见方案 8.10–8.11 和下面的原始记录。

两批生产源码均恢复；最终 source840、全部 16 个 host SHA/大小与基线精确一致。手机主程序为选定 C19 PGO5a94，15 个测试为恢复后的 generic840，三参考角色未变；上传 stage 与临时展开主程序已移除，未称全套 PGO 恢复，也未启动两候选的 PGO/直接 R6/普通 PMU。最终收据另行独立核对，当前无本线活跃手机测量。

下一阶段已按固定 FEX 重排为 N0 源码归因与工作量核账，后续候选按证据进入。已有 IR 存储、空工作量和 TargetComplete 审计未发现已获证的大候选；不据此继续叠加微小容器实验。工作包、目录和文件责任、decode 与 pass 边界及停止条件见[优化方案第 4 节](compile-cost-optimization-plan-2026-10-27.md#4-下一阶段工作包先找足够大的工作再实施)。

## N0：已有采样的源码归因与工作量核账

两份原始 C19 perf.data 未改变。临时展开并流式核对精确 `5a94db9e…`、103200232 B ELF，GNU build ID 为 `db0bf457…`；全部 7291 个 sample 的 CPU、PID、period 和原报告总账相符。3961 个主程序 frame 的运行地址、映射文件 offset、ELF PT_LOAD 与可执行标志逐项一致，3479 个不同 VA 完成精确 DWARF inline/source 展开。caller 地址按原记录使用，没有再次减 4，也没有把 nearest-line 猜测混入精确行号。

**报告聚合修正。** 原 `--sort symbol` 把不同 PID 的同名叶样本合并，后续 caller 可能沿用较早 PID 的映射。相同 perf.data 只改为 `--sort pid,symbol` 后，Translate 周期总数与逐样本 API/raw 精确一致：

| 模式 | 原 symbol-only Translate 周期 | PID 分组 / API / raw 周期 | 全进程树占比 |
| --- | ---: | ---: | ---: |
| Function | 1180800444 | 1227999867 | 55.7275% |
| Block | 768893426 | 804714763 | 41.6459% |

原报告 215/229 个 error callchains，在 PID 分组后为 38/72；两者都不能不经审查就等同物理展开失败数。最初提出的 unknown-barrier 假说经完整 raw 检查未能解释差异，其 failed 脚本与错误输出保留；随后用聚合 key 的实际干预及官方源路径核实跨 PID 机制。没有改采样、补点、筛掉 unknown 或将该修正记作性能改善。按最近已识别 frame 划分的 exclusive stage 是归因标签，并非真正计时括号；inline-inclusive 项仍重叠。

当前主要逻辑叶调用者包括 Function 的 DecodedEnd 1.91%、ValueWorklist::Find 1.30%、IntervalFixedClobbers 0.78%，以及 Block 的 FaultCatalog::Add 2.73%、ValueWorklist::Find 0.80%，分母均为整个进程树。stdlib 中的二分/哈希成本已按调用者拆开，不把整个容器头文件或整个父阶段当可省时间。旧 DecodedEnd、FaultCatalog 失败候选不因仍在采样中出现而重开；IR getter 热度也不能单独证明缓存缺失或整套布局重写收益。

**工作量账本。** 原 compile_stats 工具重放 56/56 份 SVM 日志、28 对统计和 6/6 份 FEX snapshot，与原结果逐对象相符：

| C19 直接对照观测 | translate 中位 ms | frontend 请求 | decoded blocks | 发布 IR ID 槽计数 |
| --- | ---: | --- | ---: | ---: |
| Core Function | 1002.443119 | 439 Function + 7 Block | 4906 | 82819 |
| Core Block | 660.197893 | 2418 Block | 2418 | 69434 |
| FP Function | 24.051146 | 6 Function | 80 | 1924 |
| FP Block | 19.175000 | 42 Block | 42 | 2260 |

`ir_insts` 的源实现累加 MaxInstrCount/MaxInstrId，首先是发布路径的 ID 空间计数，不是独立、全域精确的 live-node census。账本保留原值，并为易误读的派生字段名另存勘误及旧版本。FEX 严格三次 47.256875 ms 观测缺少同次 compile-entry/miss/IR 数；旧 DumpIR 的 282 request pairs、8575 CodeBlock 来自另一诊断运行，当时四项 TSO 控制未显式设置，不能补为 47 ms 的分母。该转储 header 的 NumHostInstructions 实际来自前端 TotalInstructionCount，也不是 AArch64 指令数。

## N0：一次既有 IR 构造明细诊断

保持选定 C19 main、原 Core2000 guest、CPU4 和既有六条件，仅启用已有 `SVM_IR_DETAIL=1`、PROF1/PROF2，各执行一次 Function/Block。没有新探针、构建或生产修改，所有诊断 ns 排除在性能结果外。

| 模式 | 已插桩 append 事件 | 同次发布 ID 槽计数 | 算术差值 |
| --- | ---: | ---: | ---: |
| Function | 147856 | 82834 | 65022 |
| Block | 85417 | 69283 | 16134 |

ir_alloc/ir_args/ir_link 的 calls 分别是一轮构造事件，而不是实际参数槽数量；它们也没有覆盖任意 raw Inst 分配。发布、构造、放弃解码及 residual 改写的作用域不同，差值不是立即折叠、精确删除或可消除节点数。两模式均 exit 0、单个 CRC4983，短 Core2000 仍按原 oracle 检查，不作为官方 CoreMark 成绩。

原 Function 已运行成功，但 `exec-out + su` 的结果读取把引号包裹的 cat 当成字面命令；collector 失败及五份读错误文件保持。R2 用 adb pull 追回同一批 raw，并逐文件等于手机 SHA，只接续尚未运行的 Block，没有重跑 Function。R2 同时在剩余运行前纠正未执行的长跑文本 oracle，沿用原工具的单 CRC 规则。Function 原即时 after 快照缺失，延迟恢复的条件/身份单列；未冒充原即时记录。所有计数字段和两次真实运行经独立核对。

源码审查另否决直接用 Owner.block 删除 DCE 成员表：死 `Add(p,p)` 会使 p 两次入队，第一次 delete 后第二次读取 GetBlock 是 UAF；arena 保留空间不能延续对象生命。原 C7 的 tombstone 目录与此不同，仍保持拒绝。下一步转向真实阶段、opcode 与 guest 来源的 roster，先查已有数据，不能为省哈希弱化身份与退役检查。

临时 103200232 B 精确 ELF 已在读者关闭后回收，手机私有诊断副本在 SHA 保全后删除；原 archive、profile、报告、失败及全部唯一 raw 留在 Data2。Host 源码/native 与手机 main 均保持原身份，没有本线活跃采集任务。

## N2 首个实际 roster：收缩到 U64 常量物化

直接分析 C19 source0b、ELF5a94 的既有 FP 发射记录，没有借用 C27，也没有补测。42 个成功 Block 发射均按有序 invocation 配到唯一 raw/final，无重复 PC、失败或孤儿 frame；本例 final 的 IDs 均 dense，另按真实 Inst 行独立数得 3077→2260。

| opcode | raw 行数 | final 行数 |
| --- | ---: | ---: |
| StoreUniform | 318 | 3 |
| SaveFlags | 169 | 56 |
| And | 107 | 11 |
| LsrImm | 96 | 0 |
| LoadImm | 83 | 42 |
| SetHostFPR | 0 | 99 |
| SetHostGPR | 134 | 192 |

以上同时存在 guest-state 映射、替换、flags 清理及其他改写，不能说“817 个 builder 节点可删”或“315 个死写”。LoadImm 的全部净差来自 U64 的 73→32；U8/U16/U32、AdvancePC591 与 BitCast169 的净数保持。32 个 post-uniform、51 个 post-elim 是有条件且可能仍有后续 DSE/DCE 的中间态，事件和行段保留，不能求和当额外工作量。

后续源码审查确认部分 `ToValue(Src(...))` 字面量会由现有 FoldIntegerImmediate 还原为 OperandImm，已进入下面的 C28 原型。不能把这条具体构造路径等同于全部 41 个净差，也没有由此预测提速；不为此重复 IRDetail，不把 flags/state 整体提前。

## C28：decode 保留可编码整数字面量，收益门失败撤回

原型只在 op0 寄存器、原始 op1 立即数、32/64 位、无 exchange/LOCK 时，复用 builder 原有 feature/target 捕获和纯立即数规则，保留字面量到算术 Operand。涵盖 ADD/SUB/CMP 与 AND/OR/XOR/TEST；不合格和功能关闭路径在 RHS 原位置 ToValue，进位、窄位宽、内存及原子路径继续原构造。没有新增 pass、缓存、开关或规则副本，详细文件责任见方案 4.3。

两个新增用例使用实际 x86 bytes，合计 144＋28 次内部 decode/inspect，检查类型、规范化、flags、写回和回退顺序。这不是 172 行独立 JUnit，也不是运行语义验证；HIR fixture 的 fast=true 限制明确保留。生产及测试源码经过独立审查。

R1 source `ebb6e4c3…` 的 16 目标构建在 25.703 s 后退出 1，唯一编译错误为新测试中的 `reinterpret_cast<VAddr>` 未限定命名空间。构建日志、原测试快照和补丁原样保留，没有 C28 测试或性能样本。读者关闭后只将该 token 改为 `swift::VAddr`，R2 的四个生产文件与 R1 逐字节一致；source `df889ea6…`、source/build binding `19ec675e…` 已冻结，16 目标实际构建成功。此次增量 native 构建耗时 96.956 s，仅为工程记录，不是 guest JIT compile 成本。构建前后源码、CMakeCache 和 build.ninja 均稳定。

R2 首个 literal 命令以 1 项断言失败退出，后五阶段未运行，也未产生性能点。失败仅在 32 位 AND0 写回形状：既有 FoldWidthValue 可以把 ZeroExtend32To64 改为 LoadImm(U64,0)，OR-1 同理可成为 U64 的 0xffffffff。独立源码复核确认这是测试预期过强。先单独核对 R2 FP48，PC、顺序、实际字节均等于 C20/R6；该独立核对没有把失败 sequence 改成成功。

R3 只对这两个已列的 32 位写回要求精确的 U64 LoadImm 类型和值，其余 producer、flags、写次数、回退和顺序断言保留。source `765a990d…`、binding `df329913…` 冻结后实际 16 目标构建成功，仅 swift_test 与 R2 不同，其余 15 个 ELF 逐字节相同。新的完整六阶段实际结束：literal 四命令各 2 行/6660 项检查；原 22 套件 628 行/863924 项含受控 22 项历史失败；定向 84 行/4016 项、focused 4 行/24 项均通过。合计 724 行/894604 项，未把另存的两份原 R6 控制 XML 加入候选总数。native 353＋993 项全部通过，FP48 再次逐字节一致。

实际 native 审计发现整个 `.text` 增加 816 B，ArithWithFlags 栈帧 352→400 B、DecodeAddSub 288→304 B；逻辑 helper 栈帧不变。PrepareIntegerSource 自身栈帧为 48 B，回退会先调用 ToValue 再组装 DataClass；8 类对象大小和成员偏移均未改变。这些代价与少构造 IR 的收益必须一起接受耗时验证，不能从静态大小直接推算速度。

实际 raw/final 与 C20 按 invocation、块 occurrence、PC 和 host 发射配对，没有未配对项。Block raw 3077→3058、LoadImm 83→64；Function raw 2548→2529、LoadImm 154→135。两模式各少 19 个 U64 LoadImm，其他可见 raw 节点及 ID 对齐的参数边相同，final 仍分别为 2260/1924 行、42/91 个 LoadImm。它证明已发射单元中少了这些构造节点，不覆盖全部 append/放弃解码，也不把原始 41 个净差全部归给本规则。R3 main 与 R2 逐字节相同，native 审计直接复用原 R2 实物并补充 R3 身份，没有重新导出相同反汇编。

原七轮 56 点全部结束，730 个 raw 文件、172 条实际命令及 28 对阶段/计数重放一致。与恢复后的 source840/generic B8e 相邻比较如下，均为 candidate/baseline：

| 组别 | translate 中位比 | 七对范围 | 快/慢轮数 |
| --- | ---: | --- | ---: |
| CoreMark Function | 0.996791 | 0.984531–1.001851 | 5/2 |
| CoreMark Block | 1.003239 | 0.994689–1.021236 | 2/5 |
| FP Function | 0.995139 | 0.961033–1.015049 | 4/3 |
| FP Block | 1.019820 | 0.986790–1.032745 | 2/5 |

四组都跨 1，没有触发七对全慢的正式负控；两个 Core co-primary 都未通过事先冻结的七对全快且中位至少改善 5% 门，因此拒绝。Core 14 对均有工作量计数差异，FP 14 对相同；原样保留，不按节点数归一，不补点或重采。少构造节点的原型整体未表现出达到门槛的收益；现有结果不足以分别量化新增准入、DataClass 搬运、调用和节点删除的耗时贡献。

host 准备曾因缺失 author 的 `final-binding.json` 在 registry 汇总处失败，实际质量资格及 byte-exact producer 已写，尚无 plan 或性能点；原失败保存后只续完相同 host plan，不覆盖资格或 producer。整个采样仍使用原 byte-exact `b4ab582d…` producer 与原 56 点顺序。此工具准备错误不被隐藏，也未产生替换样本。

R3 五个 after 文件和累积差分已保全，随后只恢复这五个文件，完整 2418 源文件/118 条退役路径再次等于 source840；16 目标实际恢复构建成功，全部 SHA/大小与原基线逐字节一致。手机为选定 C19 main 加 15 个恢复后的 generic 测试，三个固定参照未动，未冒称全套 PGO 恢复。763 个手机私有输出文件在逐文件等于 host SHA 后删除，七个本线目录已清空，没有活跃本线 benchmark。主程序压缩归档为 26103473 B，完整流式读回 SHA/大小一致，R3 main 与 R2 相同而复用同一份归档。未启动本候选的 PGO、直接 R6 或普通 PMU，也不把实验 ELF 标成选定 C19。

## N1/N3 与下一项构造路线的准入收口

N1 没有找到可立即删除的大块重复 Query。其全路径周期占比为 Function 1.1695%、Block 0.4747%，SimplifyValue 上下文更小；互斥规则不重复算，同根 memo 会改变原 32 次访问预算和精度。clean width seed 存在具体重复子例，但覆盖未获证，保持 HOLD。

N3 的 PublicIndex 方案也不进入实验：两模式各 14 个样本，仅占整个进程树周期 0.3617%/0.4109%；PublicIndex 与 GetArg 的去重 union 为 1.1165%/0.7645%，其中多项检查和重构仍需保留。只替换元数据 offset 算术不能被当作大块 IR 访问优化；后续表示研究需 refill 等真实证据。

后续只读枚举已确认 CPU4 支持通用 L1D/L2D refill；L1D read-only refill 专项不支持 CPU4，不能跨核套用。一次非 guest 的 `sha256sum` 能力检查中，cycles、instructions、L1D/L2D refill、backend stall 五事件同组全部取得正计数，time_running/time_enabled 相等。这是 PMU 能力证据，不是 compiler workload、缓存瓶颈或提速证据。

随后在冻结脚本和计划后，实际对选定 C19 main 各运行一次 Function/Block Core2000。原 PID/wait、isolated env、guest/oracle 保持，既有 simpleperf 改用四个 user 域事件同组 400 Hz；这与旧 2000 Hz、含 kernel 的 cycles 采样不同，不能直接对比占比。Function/Block 分别记录 2542/2281 个样本，采集器报告 lost=0，两个 profiler/target 均 exit0、单 CRC4983/Iterations2000；22 个 raw 文件逐文件与手机 SHA 相同。采集前后 source840/retired118、16 个 generic ELF 和独立 selected C19 ELF 均保持。

离线归因已收口：4823 个样本逐条与 raw 的 IP/PID/TID/时间/CPU/period/event ID 相同，八组 event 的总账及 Translate prefix 均与按 PID 分组的 CLI 精确一致。主程序 3063 个不同 frame 的映射和 PF_X PT_LOAD 通过，2710 个 VA 完成精确 inline/source 展开；无额外 PC 偏移或 nearest-line。两模式各六 PID 的 FORK/COMM/EXIT 树和样本生存区间闭合，四属性均为 400 Hz、exclude_kernel=1，实际无 lost records。

Function 的 L1 refill 中，DecodedEnd 路径为本事件 period 的 12.01%，IR traversal context 为 14.20%；Block L1 中，FaultCatalog::Add 的逻辑 owner 为 18.36%。L2 同时落在 allocator、emission 等路径；这些 inclusive context 不相加，也不能换算为 compile 毫秒。采样 precise_ip=0，PC 不是实际 load/miss 地址；record sample_type 没有 read 字段，能力检查中的无 multiplex 不是当前采样 time_running 的新证明。现有覆盖尚不支持整套 IR 布局重写，也不因此重开已拒 DecodedEnd/FaultCatalog 候选。

分析读者关闭后，22 个手机重复 raw 与临时 103200232 B 精确 ELF 已回收，唯一 host raw、可复算脚本/结果和 C19 压缩归档保持。没有新的 compile 提速结论，原性能表不变。

另对 decode 期避免 guest-state 死写做了源码准入审查。C20 FP 的 StoreUniform318→3 中，92 个改成 SetHostGPR，99 个改成 SetHostFPR，119 个在 DSE 阶段消失，5 个在后续 sink 路径消失，3 个保留；已有 mapped GPR 写另死 34 个。它说明有更大结构工作，却没有直接给出 decode 可避免数量。现有 builder 转发和 pass 的 ordered sinking 保持原责任，暂不增加第二份值图。

固定 FEX `Core.cpp:612–624` 每条 guest 指令前调用 `FlushRegisterCache(true)`，其 SRA mask 包含 GPR、PF/AF、FPR，随后清除相应 cache/write/partial 状态。因此不能把 FEX 描述成普通寄存器跨整个 block 只发布最后写，也不能用它证明 SVM 跨指令延迟发布安全。新构造路线仍在设计 HOLD，须先闭合 fault/partial/order/异常恢复和 terminal，未实现或宣称收益。

进一步按真实 AdvancePC 分段核对上述 153 个首次 DSE victims：0 个可由同一 guest 指令内后续写覆盖，152 个为跨边界的全宽同范围写，另 1 个 U8 写由跨边界 U64 写覆盖；没有 branch meet 或多个 partial 联合覆盖。故“仅合并单条 guest 内写回”的窄方案也为 NO-GO，不能用它替代仍待证明的跨指令合同。

## C29：原严格字节失败保持，r4 正确性补证完成

检查更大构造路线时确认 HIR 已有 AdvancePC 合并，Block 则直接追加。当前 FP Block 的 591 个 marker 若原样重放 HIR 规则，可合并 400 个；但规则用原窄 Imm 类型存放累加值，真实 Function 的两处 roster 已出现额外丢失 256 字节的对应证据。不能把数学上不丢字节的加法等同于窄类型 Get() 之后仍不丢字节，也未据此宣称实际 SMC 漏洞已被验证。

C29 先冻结独立 correctness 协议并保全两文件 before，只在原生产中加入三个 `[advance_pc_width]` 用例，显式覆盖 HIR fast/slow、合并开关、窄 signed/unsigned 位模式、U64 超界、正常合并、flags 及切块。最初误选 `swift_test` 构建，命令成功但没有编译新用例；在手机执行前根据 CMake 修正为 `swift_ir_test`，原 no-op 日志保留，未制造测试样本。正确目标实际构建完成，生产与其余 15 个 ELF 未变。

原生产一次真实运行得到 3 行/639 项检查，其中两项精确复现预期失败：200＋100 的 guest 字节总量为 44，U64 MAX＋1 的两条原 marker 只剩一条；正常类型/flags/切块用例通过。该负控保持为失败证据，不加入修后质量通过总数。

随后只修改 `hir_builder.cpp::FoldAdvancePC`：先以 u64 读取两个原值，在相加前检查溢出，再用原类型构造合并值并 round-trip；不能完整表示时返回 false，让原追加路径保留新 marker，不强制拓宽。生产 guard 与三个新用例共两文件冻结为 source `61d9b558…`、source/build binding `112f61de…`，实际 16 目标通用构建完成，主程序为 `08dad478…`。没有性能采样，不使用旧 PGO profile 构建改变后的生产源码。

原五阶段 collector 的前四阶段通过：Catch 共 722 行、869676 项断言包含与原 R6 精确一致的 22 项历史失败，1346 项 native checks 通过。两次完整 IR 命令各有 133 行、230452 项检查，相对旧版各增加三个新用例和 856 项 suite 级检查，三个新增用例均无失败。第五阶段的两次 FP 运行均退出 0、输出 oracle 相同，但 Function 的一个缓冲变化，sequence 保持 failed；没有将部分语义通过改写为全部质量通过。

实际缓冲审查显示 Block 的 42 个缓冲与 C20/R6 精确相同；Function 的六个缓冲中五个相同，PC `0x400f10` 的缓冲由 2680 B/670 条 ARM64 指令变为 2228 B/557 条。两版尾部 1700 B 精确相同，包含本次审查识别的稳态向量循环；变化位于前缀。解码源码和真实 raw IR 进一步表明，准确长度触发 `FunctionRegionDecoder` 的 `DecodedEnd→NearestEntry` 重置和重解码，原重叠循环体被拆回独立入口。新前缀与原前缀、独立循环体之间的可见值依赖相符，但打印 IR 没有覆盖全部 Operand，机器码缩短也不能单独证明首轮、故障和 retirement 状态正确。

持久化链路会把 AdvancePC 总量写回 guest end，再保存至 SerialBlock、guest hash 和范围注册；恢复旧缓存不会重新执行该 HIR guard。因此需显式将兼容版本 51 升至 52，并验证其余 key 匹配时旧51仍被拒绝，AOT 布局版本保持。真实新 ELF 的 BuildID 通常还会先行失配，不能据源码链路声称已验证一次旧缓存或 SMC 漏洞。

原 emission 断言失败发生在其 `identity_after` 之前。另行延迟核对已确认全部 2418 个源文件、118 条退役路径、host16 和配置精确；手机只有实际 collector 安装的七个 C29 角色，其余九个未执行角色保持历史 source840，且无活跃测量。该收据不补造原 collector 的末尾身份。根检查曾误以为手机16项都应已部署 C29，错误检查失败并保留说明，未替换测试点。

r2 的独立正确性协议在新增编辑前冻结：保留 r1 严格字节失败，新增真实 x86 的晚入口解码、真 0/1/17 次循环及首轮/一轮后故障状态回归，并验证 cache52 对旧51的 disk/AOT 拒绝。`fp 0` 实际会归一为 `fp 1`、均执行四百万次，不能充当 0/1 次测试。执行回归要求真实 ARM64 JIT，检查全部 16 个 GPR、16 个 XMM、精确 RIP 和六个算术 flags；Function 还检查公开的切块范围。初稿曾把 JNZ 的 6 字节计入 surviving-marker 范围，首次构建前的独立源码审查已修正 oracle 并保留首稿与勘误；它没有修改实际 guest 字节、执行状态预期或生产范围算法。终结指令末尾 marker 丢弃的原有限制保留，不能称已补齐所有终结字节依赖。

r2 共六文件相对 source840 冻结为 `0ac6cedd…`，相对 r1 只新增兼容头和三个测试文件的变化；原 guard 和三个 HIR 语义用例 SHA 不变。16 目标实际构建完成，主程序 `c92a7de9…`、swift_test `9ff0e3c9…`，源码与配置均稳定。32 条补证命令在执行前冻结；采集器的超时保留、FP 设备 timeout 与九角色退出检查经复核补齐，原未执行的 v1 计划保留，实际使用 v2。

实际 v2 在第一条 `consumer-func0-flags0-coalesce0` 停止，target exit2：两行共 609 项 suite 级检查，仅执行回归的首轮 load 故障有两项 RIP 失败，分别为 CF0/CF1；实际是 prefix 入口 `code+8`，期望是 loop/load `code+304`。真实解码回归没有失败，其余已执行的 GPR/XMM/flags 检查没有失败。此点为 Block 且合并关闭，不能直接归因为 HIR guard；PageFatal 的现有 RIP 契约与恢复元数据正在独立审计。原点与失败完整保留，其他 31 条未运行，没有补点或改写通过。另行延迟检查确认 source/host16、phone9 C29r2＋7 历史840、FP 输入身份均稳定，全部本线角色退出。它没有补造 failed collector 缺失的 after 字段。

r3 只按源码合同修正 PageFatal 的报告 PC，保持全部生产和其余测试 SHA；随后前四条 Block 命令各 609 项检查通过，但第五条 Function 命令在真零计数时因 `REQUIRE(prefix)` 失败：冷前缀尚不属于入口 Function。此时 Function 的 flags readback 尚未执行，不能记为已通过。独立审查确认 Module::GetNode 使用精确 root key，JMP 的外部目标可另行懒编译；测试不能要求未执行的冷路径已被吸收，也不能要求依赖前驱 SSA 的内部 body 有公开 canonical entry。

r4 的修改限定在同一测试：零计数仍首先运行，保留 root 实际 native Function 和所有架构状态检查；非零从真实 prefix root 查询 Function，再检查固定的 prefix/loop 范围，并验证 prefix 的原生代码分配。内部 loop 不需要独立 GetNode/GetCodeCache。最初尚未构建的 public-loop 草稿与协议保留，执行前已据私有入口合同另冻 v2；没有观测点被替换。全部生产源码和另外 2417 个源条目保持 r3/r2 身份，16 目标构建只有 swift_test 变化。

最终 r4 source `24ae103a…`、主程序 `c92a7de9…` 完成实际 32 条命令：10 条 Catch 共 22 行、6880 项 suite 检查，20 条 native 的 parent/single-process checks 为 1652，零新增失败；disk 子进程的检查另列，不混加。静态/间接 Call 的磁盘控制均为三个不同 fresh exec PID，write/read/reject 实际 loaded/compiled 分别 0/2、2/0、0/0；旧版本字段为 51。AOT 在其余 key 不变时输出精确 `format version 51 != 52` 并拒绝发布，当前 artifact 继续执行。真实 disk latch 和 AOT request/latch 也通过。

两次 FP 实际输出及发射缓冲均精确等于 r1：Block 42 个/6736 B、Function 六个/7992 B。原 r1 对 R6 的单一有意变化仍单独保留，没有改写旧严格门。九个部署角色与 FP 输入的前后身份相同，原 collector 已完成 source/host16/config 末尾核对及九角色/FEX idle 检查。精确 generic 主程序压缩保全并流式读回核对。独立完整复核确认 32 个实际执行、72 条 transport 命令、74 个 raw 全部一致；native 八个独立 child PID 的 392 项检查另列，R1/R2/R3 的 failed execution 和 raw 均保持原样。

这次仅完成有限正确性补证，没有测量编译提速，也没有替代 C19 的原 PGO 检查点。普通访存精确 instruction-PC、安全 retry、终结指令末尾 marker 的既有限制没有因此解决；后续若更换选定检查点，仍需匹配源码 PGO、直接原 R6 和普通 PMU 资格。

Block 合并的七 opcode 版本进一步收敛为 NO-GO。398/591 只是原安全求和计数上界，Function 当前 193 个再合并为零；源码反例表明 marker 窗口可同时导致原 branch-only recipe 失效、或 FCMP recipe 新获资格。即使增加 ReadFlags/effects 与前一 guest 边界，也只删 174 条且仍有九个窗口变化；最保守前缀方案仅删 17 条，不启动实验。实际锁内 builder 生命周期可以闭合，没有证实生产 UAF；Function-only 的 token/strict-suffix 已从独立 Block 的阻塞表中剔除。仍需迁移实际语义消费者才可能继续，不能把这些离线计数当安全删量或收益。

## C31：generic 与匹配 PGO 相邻门通过，有限检查点收口

新归因定位到 FaultCatalog::Add 的 owner 搜索循环：普通 Block 的循环体占全进程 period 的 2.5576%，Function 仅 0.2071%；另一份 L1 refill 诊断的循环行占本事件 period 的 17.4123%。这些分母与批次分开，未折算成 compile 毫秒。旧 C24 bulk 不删除新 owner 首次 Add 的全链搜索，故本项是独立机制，没有重测旧补丁。

三文件方案在编辑前冻结，保全 before，并选定有序 map：`(module, owner_start)→既有 Allocation*` 仅在原 writer_mutex 内使用，保留 head 同 key 快路。Reader 的 SC 链不读索引；Retire(Allocation*) 在原回收前删除索引，统一覆盖 Remove、Seal 和析构。新 owner 在 emplace 成功前同时保留 Snapshot 与 Allocation 的 unique_ptr，避免索引分配 OOM 泄漏。没有新增公开 API、配置、unchecked fresh 入口或持久格式变更。

测试先独立应用于旧生产，实际只重建 FaultCatalogTest，其他 15 个 ELF 精确不变；snapshots/signal/concurrent/integration 四模式共 144 checks 通过。新增测试覆盖跨 module 同 owner、held Reader 下分代复用、逐预算 OOM 后完整回滚与对象平衡、同 key 重试、每个实际分配机会的信号中断及无分配撤销。随后才应用精确两文件生产补丁，source `3cb2a8f7…`、主程序 `09a07474…` 的全部 16 个目标构建完成，166.708 s 为主机工程构建时间。

候选实际 54 条质量命令全部完成并独立重放：32 条专项加原 22 条 Catch，146 个 raw 一致；Catch 共 656 行、872516 项检查，包含与原 R6 精确相同的 22 项受控历史失败，无新增失败。20 条 native 的 parent/single-process 为 1773 checks，八个子进程 392 checks 单列；新增索引分配机会使 snapshots 为 117、signal 为 24，旧控制分别为 107/20。FP 的 42＋6 个实际缓冲、PC/顺序、输出均精确等于 C29r4，未改写 C29 历史失败。

实际 ARM64 布局为 Catalog 80→104 B、map 24 B、OwnerKey 16 B、tree node 56 B；Snapshot 32 B、Allocation 40 B、Reader 8 B 及旧字段 offset 不变。Reader::Lookup 336 B/84 条原始机器码精确相同；LookupAllocation 仅五处到相同符号的 BL 位移变化，其余 words 与三个 LDAR 不变。主 `.text` 增加 496 B；Add 从 1776→2044 B，本地入口栈 192→176 B，不宣称总栈峰值或速度改善。

当前实验参照为 generic C29r4 `c92a7de9…`，不是旧 PGO。host/phone previous 两角色已更新到这个精确 ELF，原 B8e 流式核验后仍保存在 C20 压缩归档，原 R6 两角色未动。独立 postflight 确认 source/host16/config、手机九个候选角色与七个未执行历史840角色、previous/R6/两 guest 身份和全部本线进程 idle；未冒称手机16个都已部署候选。源码、质量与 ELF 读者均关闭后，固定原 `b4ab582d…` 生产者完成唯一一次七轮 56 点，原收益与负控门未改。

全部 365 个 artifact、730 个 raw、56 个唯一 PID、172 条命令与 28 对结果已独立重放，退出/输出/分组 PMU/CPU4/六条件/前后身份均通过。Root 又单独确认所有本线 VM、R6 控制及 simpleperf idle，不把 producer 的 child wait 冒称独立全角色 pidof。

| 负载与模式 | 配对 candidate/baseline 中位比 | 七轮范围 | 更快对数 | baseline / candidate translate 中位 ms |
| --- | ---: | ---: | ---: | ---: |
| CoreMark Function | 1.003302060 | 0.994413–1.032920 | 2/7 | 1127.432 / 1129.703 |
| CoreMark Block | 0.949927841 | 0.935613–0.963972 | 7/7 | 817.623 / 774.270 |
| FP Function | 1.003582871 | 0.996913–1.148459 | 3/7 | 26.341 / 26.504 |
| FP Block | 1.004335687 | 0.989321–1.025120 | 3/7 | 22.979 / 23.097 |

Core Block 的配对中位下降 5.00721590587393%，刚达到原 ≥5% 正门；另三组均跨 1，没有稳定提速结论。四组均未满足全七对更慢且中位 ≥1.01 的负控条件。表中时间列单独取中位数，不能用其差反推配对比例；这不是纯编译 CPU 或同工作量的归一比较。

Core Function 的 r3/r4/r6 实际多一个 func request、一个 decoded block、92 B host code 和 15 个 IR ID 槽位。Block 七对的请求差为 −1、＋5、−3、−1、−1、＋1、＋4，host bytes/IR 槽位也有双向差异，请求中位为 2416→2417；FP 计数全部相同。这些观测没有被删去、按节点归一或归因为某个未经定位的 guest PC。FP Function 的 1.148459 原始离群点也保留，没有替换。

决定仅为 `ADVANCE_TO_FRESH_SOURCE_MATCHED_PGO_VALIDATION_ONLY`：暂存当前源码并进入新的 PGO 资格，不宣称 Function 收益、替换选定 C19 或完成 FEX 目标。新生产不得套用旧 `59038523…` profile；后续匹配 profile、直接原 R6、普通 PMU 和运行时间限制必须各自如实验收。

新的 PGO 相邻参照已冻结为当前 generic `09a07474…`，host/phone previous 同步更新；原 C29r4 `c92a7de9…` 精确压缩件仍保留，原 coarse 的基线记录不改写。`compile-cost-c31-pgo-r1/generate/prepare.json` 已核对 source `3cb2a8f7…`、16 个 generic 产物及工具身份。独立复核通过后，实际 configure 只改变六个预定 typed-cache 值，完整 Ninja 图在移除这些选项和缓存路径差异后等价，rules 文件不变。训练主程序 `3112b17e…` 实际构建完成，141.132 s 仅为工程构建时间；十五个测试在该阶段保持 generic SHA。

四次训练分别运行 Core2000/FP1 的 Function/Block，各一次正常退出，输入和六条件保持；每次取得一份归属实际训练 PID 的 raw。仅以这四份新 raw 严格等权合并，得到 profile `0e4affdd…`、2175728 B，没有跳过输入或 LLVM profile 诊断。profile-use 首次 prepare 因脚本误将用户目录下的 NDK 工具限制到 Data2 而失败，未配置或构建；失败及旧脚本保持，r2 仅给精确固定 profdata 工具增加只读路径例外，完整 profile 资格复核通过后才继续。

16 目标 profile-use 构建已完成，工程构建耗时 327.360 s，实际主程序为 `56fb68c9…`、103325776 B，source/build/profile binding 为 `95520c0a…`。主程序无插桩段及 profile runtime 符号；14 条实际 profile 告警均为测试或 Catch `main` 的 hash 不匹配、计数已丢弃，生产及未分类 profile 告警为零。独立配方审查通过。精确主程序压缩件 27657985 B，已流式读回核对，没有展开副本。

54 条质量命令也已完成：补充 Catch 22 行/6880 项断言、1773 项 native parent/single-process checks，原 22 组 Catch 634 行/865636 项断言，历史 22 项失败精确保留且无新增；FP48 共14728 B 保持原接受的 C29r4 实际 PC/顺序/字节。root 重核全部146个 raw 并完成独立现场 post54：source2418/退役118、host16/config/profile、手机9个实际PGO角色与7个未执行历史840、previous09a/R6/guest/训练角色精确，全部本线进程 idle。

独立运行质量重放已通过（`e2bc4426…`），同时独立核实8个子进程392项 checks，未并入1773的父/单进程合计。新 PGO 相邻规则 `7a829e3c…` 已预先冻结；实际计划 `dd7ab475…` 的246项输入 SHA、完整56点顺序、环境和固定角色通过独立执行前复核。唯一一轮七轮56点已完成并通过独立全量重放（`8f69880f…`）：730个 raw、172条命令、56个不同native PID、六条件及全部计时/计数与原比较器一致。

| 负载/模式 | generic Translate 中位 ms | freshPGO 中位 ms | 配对比中位数 | 七轮范围 | 配对中位减少 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Core Function | 1126.708 | 991.107 | 0.879649 | 0.861029–0.902802 | 12.0351% |
| Core Block | 783.576 | 620.577 | 0.785404 | 0.771350–0.824415 | 21.4596% |
| FP Function | 26.485 | 23.554 | 0.887788 | 0.861503–0.909264 | 11.2212% |
| FP Block | 22.965 | 18.930 | 0.824327 | 0.803065–0.835658 | 17.5673% |

四组各七对全部更快，预定正门通过，无负控触发。Core Function 三对请求/decoded 相差 -1/+1/-1，伴随 ±92 host B 与 ±15 IR ID slots；Core Block 七对也保留混合计数差异，其中一对 decoded 数相同但 host bytes/ID slots 不同。FP计数完全相同。没有归一化、替换或追加点，也不称为纯编译CPU或完全同工作量收益。时间列分别取中位数，不能用它们反推配对比例。

freshPGO 各桶的独立中位数中，Core Function decode/opt/regalloc/codegen 分别为164.317/191.129/269.187/246.583 ms，Block为60.199/85.785/182.276/193.494 ms。regalloc含输入准备，codegen含target completion与发射；不同桶中位数不相加。独立策略判决 `b07cb631…` 和root决定 `690780f8…` 仅允许进入直接原R6与普通PMU，尚未替换C19或完成FEX目标。

随后独立冻结的直接原R6七轮56点也已完成，原baseline始终为 `02085b0b…`，没有覆盖generic `09a07474…` 的previous角色。完整269项身份、730个raw、56个不同native PID、172条命令及28对全量结果通过独立重放 `612a265f…`，四组均通过原累计正门且无负控。原R6的491文件overlay与当前2418文件完整源域分别保留；C29已接受的范围正确性与一个Function缓冲变化保持独立证据，不以本轮时间追认逐字节等同R6。

| 负载/模式 | 原R6中位 ms | freshPGO中位 ms | 配对比中位数 | 七轮范围 | 累计配对中位减少 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Core Function | 1264.423 | 995.320 | 0.790007 | 0.767382–0.796901 | 20.9993% |
| Core Block | 934.471 | 614.355 | 0.661396 | 0.642767–0.669978 | 33.8604% |
| FP Function | 29.975 | 23.169 | 0.769790 | 0.740735–0.789126 | 23.0210% |
| FP Block | 26.517 | 19.037 | 0.721247 | 0.708044–0.746950 | 27.8753% |

这些累计结果直接来自该批配对，不是相邻比例相乘，也不与旧C19跨批求差作为新改动的因果收益。

普通Core36＋FP36三臂PMU随后也已完成，独立报告 `b39e87b2…` 重放944个raw、原采集器与完整比较schema。两套私有FEXServer的startup、全部36请求、间隙idle及shutdown分别计量，实际PID/starttime/exe/env、grouped PMU、关闭与transport退出0全部闭合，没有裁减字段或摊销到primary。SVM诊断变量完全缺席，包括显式0。

| 模式 | 增量instructions比中位数 | 三轮范围 | wall slope比中位数 | 三轮范围 |
| --- | ---: | ---: | ---: | ---: |
| Core Function | 0.999991 | 0.999967–1.000020 | 0.991636 | 0.983776–1.006257 |
| Core Block | 0.999951 | 0.999587–1.000098 | 0.985014 | 0.973146–0.997492 |
| FP Function | 1.001454 | 0.999371–1.002695 | 1.009059 | 0.959524–1.025055 |
| FP Block | 1.000067 | 0.998687–1.004711 | 1.003381 | 0.982946–1.011491 |

比值均为current/R6的原逐轮增量模型。12个instructions斜率比全部≤1.01，既定指令质量门通过。FP Function/Block的wall中位仍增加0.906%/0.338%，各2/3轮增加；cycles斜率中位比也为1.008167/1.004637。必须保留这些范围、原undefined及负intercept规则，不能把低工作量wholewall改善说成稳定运行零回退，也不能把模型intercept当纯compile CPU。

root据此选定 `selected-checkpoint.json`（`811bc92e…`）为当前有限编译与既定指令质量检查点；旧C19精确压缩件不动。现场post72再次确认source2418/退役118/host16/config/profile及手机9PGO＋7历史测试、previous09a/R6/guest/训练角色精确，全部本线进程idle。外部FEX同数量级目标未完成，下一步才冻结有逐点生命周期证明的对称28点编译服务elapsed协议。

## 更新的固定 FEX 参照

相同性能 FEXLoader `8632943c…` 的三次新标准统计，在显式严格四项 TSO 配置下得到 48.995677、46.766979、47.256875 ms，中位数 47.256875 ms。原始 ticks 为 940717、897926、907332；按既有永久用例实际读出的 19200000 Hz 换算，频率证据并非本批新读。Signal 计时另列。每次 live 均见单 PID、一个 task 和相同 TID 的最终槽，但未证明全部线程历史或槽未复用；仍称观测单槽累计计时，不升级为纯 miss CPU 或全线程总量。

额外一次普通 FEX 采样的 raw 有效，但性能 ELF 缺少所需符号，907 个样本含 132 个错误调用链；未得到 counter 内外编译工作的可靠归因结论，没有据此修改计时范围结论或追加采样。两次独占 warm service 均通过私有环境标准关闭并等待真实退出。

C15 源码复核确认，普通 CompileBlock 的计时覆盖前端解码、IR/passes/RA、后端、代码区分配复制、I-cache 刷新、延迟回池和单元 mapping 发布；未发现整段普通编译另交未计时 worker。编译器初始化、最终析构、dispatcher 及 ExitFunctionLink 返回后的 direct-link 修补在外。CompileSingleStep 是独立未累加入口，但现有三点没有大量使用的证据。线程槽复用和 fork 的历史仍不能从 final/live 单槽恢复，47.256875 ms 继续只称观测槽累计，不升级为纯 miss CPU 或全线程总量。

随后完成一次新的联合生命周期资格观测，未计入性能表。simpleperf从启动前包住原C4 FD collector，CPU0记录器/collector、CPU4 target与原安排一致。独立复核 `8f3bcab5…` 重核74份raw，140条FORK、141条EXIT、144条COMM组成141个完整退出节点；目标没有额外线程、后代或TID复用，同次统计槽可归属于一个guest compilation context。

原观察器曾因采样attr的task/comm_exec为0给出HOLD；此必要条件过严。设备内核对应的[固定提交实现](https://android.googlesource.com/kernel/common/+/1f083f1cee620c59f5f27b72969f784b716bbeac/kernel/events/core.c#7894)以comm/mmap/mmap2/mmap_data/task任一项匹配任务事件，COMM_EXEC也由真实exec路径直接记录。当前实际FD启用了相应属性；精确Android工具二进制、全部记录ID、开始/结束次序及四类零丢失元数据共同补足策略证明，不需要假设未序列化的辅助FD。原collect/inspection的HOLD字节保持，独立结论另存，无新增trace或重跑。

通过的是这一次单上下文生命周期资格，不能追溯三次历史47ms，也不把本次带追踪的计时放入普通性能表。正式同批28点仍须冻结两引擎对称观察方式，并逐点取得相同的生命周期证明。SVM原point内部还有simpleperf stat，不能再原样包一层record而让FEX没有对应内层；外部方案须只复用其输入/快照/真实wait/oracle逻辑，明确删除这层不对称观察。

## C31：同批外部28点有效，数量级门未通过

外部协议先冻结两种SVM模式各七对、14个fresh FEX分母，严格要求各scope七个比值及其中位数均小于10；没有把内部5%收益门当完整目标。统一CPU0/default-inherit/400Hz user-cycles observer、CPU4 target，SVM capture移除旧内层stat。原C4 FD collector与配置保持，按最终fresh-origin统计槽ticks换算，不用final-live差、不用CPU4的1171200kHz替代19200000Hz CNTFRQ。

唯一28点全部完成，私有服务正常stop、原foreground owner wait、finish均退出0。汇总返回非零是预定数值门失败，采集本身为complete、errors为空。独立全量复核 `e4877cc7…` 重核1428份点内raw、529份service raw、2407个唯一文件身份和210条真实命令，另从28份原始PERFILE2逐项走metadata ID、四类丢失及3815个任务世代，全部退出，无未解释target后代。每点计时、输入、env、argv、CRC与完整observer/服务历史均闭合。

独立计时账本 `098bf296…` 再从14份SVM stderr和14份FEX最终槽按有理数重算，结果与原汇总全字段一致：

| SVM分配模式 | SVM独立中位ms | FEX独立中位ms | 配对比中位数 | 七对范围 | 判定 |
| --- | ---: | ---: | ---: | ---: | --- |
| Function | 1004.728388 | 47.193125 | 21.005411× | 20.634127–21.479383× | 7/7不满足`<10` |
| Block | 620.956892 | 47.502604 | 13.103481× | 12.621382–13.314632× | 7/7不满足`<10` |

该表是明确不同端点下、共同observer中的完整编译服务elapsed工程对照，不是同边界纯CPU/每miss/每IR成本。两时间列独立取中位数，不用于反算配对中位比；旧47.256875ms没有被追溯升级或当新分母。FEX本身没有切换到SVM的两种RA范围。

root post-external再次实核source2418/退役118/host16/config/profile、全部手机实际角色与参照/guest/训练角色原样，所有本线进程idle。原执行、失败数值判决及所有原始点保留，无重跑、补点、归一或比值相乘。该次外部28点收口时，Function重放临时诊断补丁尚未应用；随后已完成当前普通调用链及一次重放诊断，并精确撤回临时修改，见后两节。C31有限检查点保持，完整FEX同数量级目标明确未完成。

## C31：当前普通调用链归因完成

两次新的Core2000普通调用链使用当前非插桩PGO主程序 `56fb68c9…`，全部SVM诊断变量缺席。Function/Block分别记录3847/3256个样本、2176989229/1845484163 period，四类丢失均零；按真实native exec/EXIT圈定范围，以 `--sort pid,symbol` 与reportlib逐样本核账，再用精确PT_LOAD/DWARF展开main的inline源码。原raw、R1未执行/HOLD和R2角色名修正记录均保留。

记录到Translate父链的样本分别为2104/1269、period1195759946/721080142；报告error callchains61/94保持，缺失父链不补造。allocator/CAS/malloc/free的逐样本去重union为453/284个样本、257565851/161182070 period，占这部分编译链period的21.5399%/22.3529%。CAS-only且没有Scudo上下文的样本单列；剔除后约20.21%/21.18%。这些不是函数调用次数或可节省毫秒。

归因覆盖分散：Function的allocator源码owner包括CollectDead15961307 period/28样本、ResetDeadOwners13656821/24；Block包括EndVixlScratch7951711/14、BlockAnalysisIndex::PrepareUses7945460/14、RegisterSite5701297/10。表中immediate physical caller与DWARF source owner分别保留，没有把所有分配归给一个workspace表。旧C25 scratch方案仍已拒绝。libc/linker实物的16B GNU BuildID按perf固定20B零填充ABI逐字节匹配，使用本批专属symfs软链，没有NDK stub、猜测源码行或新ELF副本。

ValueWorklist::Find的inline-aware self为Function26214786 period/46样本、Block9734054/17，inclusive为33033048/58与12592711/22；这些嵌套数不能相加。HIR FindValueRecord已经是ID直索引，Function self13060009/23不是待删除的二分查询。workspace的clear通常保留capacity；Value epoch中的DCE已借其roster，独立owners map来自此前阶段，并非同一时点重复两张表。新证据支持逐owner减少短命物化与已有row的重复解析，但不证明整个union可删除或某对象反复分配N次。

主分析 `compile-cost-c31-pgo-r1-profile/offline-attribution/analysis.json` SHA `3d2fadef…9666e`，caller union SHA `857d2847…ca66cf`；最终 `offline-reader-closure.json` SHA `6d2b74a8…94c192` 明确关闭全部native/source/cache读者。最初capture资格收据中的reader未关闭状态仅代表当时阶段。

随后只读审查了冻结后action消费：FixedGPRDefinitionAt已直接索引，scratch已有span/cursor；可扩展到部分current-instruction payload的游标仍须保留随机source/复合key查询。精确去重search上界仅Function7样本/3995589period、Block2/1144055，尚混有不可删随机访问，不作为新性能包。报告 `compile-cost-c31-frozen-action-consumption-review.json` SHA `716cb9b0…05c2c`。已有row沿helper传递的两文件小补丁只在R准备，未应用、未测量，也没有列为数量级主路线。

## Function reset/replay：一次诊断结束，恢复当前 C31

临时补丁只给既有PROF2/IR_DETAIL/DECODE_PROF增加有界标量，不改变decode、reset、接受/拒绝或分配策略。两文件worklist清理提案与它分开，仍未应用。同一作者的源码复核准确标为author recheck；root另读全部差分，不把作者重读称为人员独立。临时源码摘要 `2052671d…`，generic主程序 `a17c19fd…`，100759288B；未使用C19或C31 profile，也没有把诊断时间放入性能表。

Core2000 Function随后Block各运行一次，两个目标均正常退出0、CRC4983、iterations2000。实际/proc PID/starttime/exe/cmdline/full environ/CPU4 affinity、六条件和18个原raw的SHA/大小已独立核账。Function结果如下：

| 角色或原因 | calls / reset attempts | accepted / successful | 实际擦除HIR节点 |
| --- | ---: | ---: | ---: |
| function_normal | 4899 | 27989 accepted | — |
| frontier_replay / frontier_split | 237 | 774 accepted；237 successful | 8747 |
| late_entry_replay / late_entry | 104 | 470 accepted；104 successful | 10293 |
| standalone fallback | 7 | 347 accepted | — |

两个reset原因均无refusal/exception，frontier的237次边界比较全部match；late-entry没有原有match检查，未伪造接受字段。Block仅standalone2417calls/15612accepted，其余角色和reset均零。各角色attempts与原有 `[svm-decode] attempts` 的29580/15612精确闭合。Function replay accepted合计1244，占Function三角色accepted的4.2554647%，若含standalone则4.2055443%；不是唯一guest数量，也不是耗时或净删除比例。

实际擦除合计19040，包含重建后仍需要保留的prefix，不能全部记为可删IR。append角色和虽分别等于进程全局147841/85482，未获得单编译线程/不重叠的完整证明，因此其归属列保持HOLD。最大单次invocation不是整个Function的raw保留峰值。本诊断只准入raw暂存的源码可行性设计，尚不支持采用两阶段或承诺收益；Block不为此增加预扫描或第二套CFG。

观测后已反向撤回七文件诊断补丁，2418源文件/118退役路径恢复 `3cb2a8f7…`，typed PGO配置恢复，并实际重新构建main；16个host ELF逐一恢复原SHA，主程序精确为 `56fb68c9…`。诊断ELF以26165924B压缩件保存并完整流式核对，未保留展开副本。私有手机目录全部19文件（18 raw加ELF）对host/native SHA核对后删除，回收98618KiB；五个受保护原始输入前后SHA相同，无活跃诊断目标或清理错误。

独立计数资格 `compile-cost-function-replay-diagnostic-r1/qualification-independent-review.json` SHA `db261ff8…62c07`，状态 `PASS_FINITE_REPLAY_COUNTS_APPEND_ATTRIBUTION_HOLD`。构建/观测/恢复/压缩/清理分别保留独立记录；这里没有新增性能点、补采或修改选定检查点。

## C32：临时存储组合未通过采用门，已恢复 C31

本轮借鉴 FEX 的编译线程缓冲归属，实际实现了九个生产文件和一个测试文件的原型：DCE Function owner map、LinearScan 慢路径 active list 借用 workspace 私有 PMR pool，Facts/BlockAnalysisIndex 共用单个 facts-owned monotonic resource。独立 Block 不构建 Function owner 表；独立 Block 的 RA 仍走原 list，只有合格单块 Function 走原快速 vector。优化、分配顺序、spill/eviction、固定/宽度/scratch/恢复生产检查均未改变。

源码摘要 `e2236a36…`、通用主程序 `7041d74a…`，16 个目标实际完成构建，未使用旧 PGO。新加的两项 Facts/index 生命周期案例先在旧生产上实际通过 276 项断言，再在候选中通过；覆盖移出 Facts、稀疏/外来定义、别名及并存 Facts。候选 54 条质量命令完成独立复核，合计 658 个 JUnit rows、872792 项断言，包含与原 R6 完全一致的 22 个历史受控失败，没有新增失败。20 个 parent native 场景的 1773 项检查与八个 fresh-child 的 392 项分别核账；FP Block42/6736 B、Function6/7992 B 的顺序、PC 和字节均等于 C29 r4。构建/部署范围为 16 个角色，实际执行为九个角色，其余七个仅部署。

实物审计表明 Facts、BlockAnalysisIndex 各增加 48 B，workspace 增加 8 B，LinearScan 增加 8 B，不能以少了节点 new/delete 推导总内存下降。单点普通 MaxRSS 检查的 Function 为 18888→18880 KiB、Block 为 16112→15968 KiB，没有触发预定停止门；这不是峰值 arena/分配次数结论或稳定内存改善。实际 R2 收集器身份正确，协议中沿用 R1 的 bytes 元数据由单独 erratum 说明，原协议与 raw 未回写。

原七轮 56 点已完成并独立完整重放，730 个 raw、172 条命令有效。212 个输入 artifact 中，187 项 results 文件独立重哈希；25 项非 results 输入依赖原 producer 前后 guard，不冒称独立现场重读全部 212 项。没有补点、替换或 IR 归一化。下表为同配方 generic 候选/相邻 generic C31，不能和选定 C31 PGO 跨批求差。

| 负载与模式 | 配对中位比 | 七轮范围 | 更快 / 更慢 |
| --- | ---: | --- | ---: |
| Core Function | 0.9617233025 | 0.936892–0.971488 | 7 / 0 |
| Core Block | 0.9918451878 | 0.979116–1.025135 | 5 / 2 |
| FP Function | 0.9444744064 | 0.925240–0.986173 | 7 / 0 |
| FP Block | 0.9681108894 | 0.936145–1.010200 | 6 / 1 |

Core 两组均未通过预定正门；FP 只用于负控，Function 的 5.55% 改善不能替代 Core。四组均未触发预定 negative HOLD，但这不构成采用。Core Function 两对、Core Block 五对实际工作量计数不同，原样保留；FP 各对相同。独立决策为 `measurement_valid=true, policy_passed=false, REJECT`，没有进入 C32 的 PGO、普通 PMU 或 FEX 新对照。

C32 每个最外层 scope 结束都释放 pool 全部 chunk，实际只跨 scope 保留 resource 包装对象，未验证跨编译请求的 chunk 复用。结果不能分解为某个容器的因果贡献，也不能将 Scudo union 的约 21%/22% 当成下一包可省比例。有界复用只作为不同机制的备选提案；raw 边界先稳定再构造 HIR 已升为下一项结构设计，独立 Block 不增加两阶段。

拒绝后已保存 26189391 B 精确主程序压缩件，并完整解压流式核对 SHA 和长度；十份源码 afterimage、全部原始质量/性能结果与补丁保留。生产和新增测试全部反向撤回，2418 源文件/118 退役路径恢复 `3cb2a8f7…`，原 typed PGO 配置和全部 16 个 host ELF 精确恢复，主程序为 `56fb68c9…`、profile 为 `0e4affdd…`。手机全部 16 个部署角色现为选定 C31 PGO；此前七个历史仅部署角色此次统一了版本，没有冒称恢复它们原先的历史 SHA 或执行了全部 16 个角色。另九个参照/输入 SHA 不变，上传暂存目录已移除，最后 idle 检查无活跃目标、无清理错误。

根收口收据 `compile-cost-c32-transient-storage-r1/withdrawal-closure.json` SHA `1ebc8dba…98f90`；独立采用决策 SHA `63ba0b41…595d`。当前有限检查点仍为 C31，完整目标没有完成。

## C32 后的两项只读路线审查

固定历史 FP Block 转储有六组不同起点的尾部重叠，计 700 个显式 AdvancePC 标记字节，Function 跨单元为 21 字节；不是当前耗时或可省 IR 比例。同起点重发的旧 Core 记录缺少完整 guest 版本、owner 代次和 SMC 时序。直接复用现有尾部为 NO-GO：没有公共入口和 live-in/flags/frame/fault/SMC 合同，相同 guest 字节还存在常量特化与动态读取的真实上下文差异。FEX 也只导出标记为 EntryPoint 的 CodeBlock。SVM 的 exact-entry 内存复用已存在，空 `SVM_JIT_CACHE` 只关闭磁盘缓存；这不是“缓存没开”的测量错误。

另一项 sealed-value visitor 审查也为 NO-GO。`Inst::ForEachValue` 的完整调用链不能算成可删工作；去掉已存在的 FromOptimized SSA 准入前成本后，仍混有必要 live-slot 分类。可行的私有 index owner 路线仅命中 Function 五个样本、Block 零个，不能作为数量级主路线，也不为它新增 unchecked getter。两项审查只读既有证据，未启动新测量，未改生产代码。

## Function raw-first：实施前的逐段形态补证

上一轮的 341 次 reset 聚合计数没有 owner/target/指令形态，无法决定 raw-first 的真实首范围。本轮因此预先限定一次 Core2000 Function 诊断，不增加 Block 或性能点。两份 cpp 只用既有 `PROF2 + IR_DETAIL + DECODE_PROF` 门输出已经验证过的窗口、PC、stream/TID、普通 lowering 结果和 late-entry 事件；没有新增 GetPointer 或改变原接受/拒绝/Reset 决策。独立源码审查通过，通用主程序 `9efff928…`、临时源码 `ce9ca200…` 实际构建完成，没有使用旧 PGO。

采集器 R1 在任何 guest 前因实际 build schema 不匹配而被拦住，原未执行脚本和 HOLD 收据保留。R2 只修正三项：读取实际 configuration 三文件、最后完成 idle/身份/host 检查才清除活跃标记、idle 同时拒绝 stderr。R2 经过独立终审，协议仅作 status 的语义冻结，四项实际 CLI SHA 和采集前冻结的归约规则另由 root admission 绑定；草案 SHA 没有贴成最终协议。实际唯一一次 Function 正常退出，PID9293、CPU4/mask10、完整 argv/env、CRC4983 和 iterations2000 均通过。九份 raw 共6171543 B，remote-before/host/remote-after 摘要一致；原 25 个手机角色/输入及六条件前后保持，无 timeout 或重跑。

作者执行冻结归约器后，另一位审查者以独立 parser 重放原始 trace，没有执行或导入作者脚本。结果如下：

| 观察项 | 本次实测 |
| --- | ---: |
| 完整 decoder streams / regions | 5247 / 5240 |
| instruction 事件 | 29580 |
| 实际 trace TID | 9293，一个 |
| self late-entry attempt/result 成功且完整关联 | 104 / 104 |
| 不同 owner/target/marker-end 三元组 | 62 |
| 原始流 / prefix replay 指令观察次数 | 1890 / 470 |
| reset 前日志节点和 | 10293 |
| 首终结 direct Jcc：rel8 / rel32 | 103 / 1 |
| outer frontier attempts | 237，显式 reset 结果仍 unknown |

104 次观察均满足冻结类别：普通 legacy、实际 lowering 成功的顺序流，首终结为直接 Jcc，target 命中已记录指令边界且 `start < target < terminal_pc`，marker-derived end 等于该终结 PC。终结为 JNZ89、JB8、JZ3、JA2、JBE1、JAE1。原始流包括 SIMD、CMOV/SET、DIV3、CMPXCHG4 等；这不是只含简单 GPR 的输入。这里的形态成立不代表纯 raw 支持选择已实现，更不证明任意同 mnemonic 的非法/不支持形态都可继续。10293 是本次记录的 pre-reset 节点和，不冒称新诊断精确 Erase 计数、全部可删量或毫秒。

后续实际实现前置已经收窄：共享支持/终止选择，原 else→then placeholder/队列/预算，独立 prefix/suffix 语义状态，当前字节与未消费结构 witness 校验，以及不穿过通用异常 catch 的 Function 内层取消/重试。ALU/BMI/x87 的六处 raw 前缀重读须统一到同一已核窗口；`CancelGuestInstruction` 只清 pending-fold，不是整个 region 的回滚。保留 prefix 的另一方案只减少 470 次 prefix replay，仍构造再丢弃 suffix，不替代完整 raw-first，也不单独开性能包。

诊断/设计阶段曾在 results 准备六处读取的三文件最小补丁，针对当时C31源码3cb逐hunk与SHA核对，通过root只读apply-check。该独立提案未单独应用、构建或测量；当时 `insn_bytes` 可指向live guest memory，提案本身没有建立owned窗口。其六处读取迁移此后已随C34的non-emitting Fetch、共享selector及未发布attempt完整取消/重试一并实施，不再作为待做的独立清理。

唯一诊断结束后，临时补丁已反向撤回，2418 源文件、118 退役路径、原三个配置文件和全部 16 个 host ELF 精确恢复 C31，主程序 `56fb68c9…`。精确诊断 ELF 以26127477 B 压缩件保留并完整解压核对。手机私有目录十文件先全部核 SHA，再逐文件删除并移除空目录；文件总量106791703 B，目录原占104439 KiB，另25个输入前后未变，无活跃任务或清理错误。原 raw、完整 PC roster、冻结代码及审查记录保留在 Data2。上述为诊断结束时的恢复结论；此后的C34原型另列如下，FEX差距仍用既有外部28点。

## 独立 Block 顺序分配：本轮源码准入停止

具体源码审查否决了用 `use_ends` 直接替代 interval/active 的通用方案：全块预映射供未来 FPR read/tie 使用，fixed transfer 重排 start、last_use 延长 end，全块峰值还决定保留哪些 fixed plan；VOID gap 必须先记录旧 active 集合，再执行严格 `end < start` 的 expiry。同批 FreeSpill 使用 bitmap，其 bit 清除可交换，原先把释放“顺序”泛称为阻塞的推测已修正。

当前精确 C31 Block 样本中，collection 六个叶样本仍属于必需读取，构造 count 三个样本、expiry 入口一个样本，排序 self 未观测；宽松去重 union 也只有整记录 period 的0.30914%，不能把 RA 桶5.83%当可省。保留预遍历/relocation 合并的模型仍要索引、active/tie/victim 工作。因此不进入本轮原型或56点；独立 Block、单块 Function 和多块 Function 分配范围保持。

## C34：完整验证后拒绝，已精确恢复C31

按既有事务/字节/取消设计实施13个源文件，没有新增源文件或目标。实验r3源码摘要 `8acf2ed0b2d533bbe8689396a46b923578cedb36711332d8794d2bcef55addd2`，generic主程序 `e372ea6e2dfb9d83e9ab6dccec8e5ee4f5d7993d910eafab61e13bbd667ca999`，`swift_test` 为 `8f20d6df00c07917aaaa734a8ea8e4eff97659de88b800ae547f37fbd5e71865`，cache52。完整source/native/build身份在 `compile-cost-c34-function-raw-first-r1/{source-r3,native-r3,build-r3}.json`。上述为原实验身份；当前W/B和phone16均已精确恢复选定C31 PGO。

实现包括共享owned fetch、原selector中的支持/终止分类、值类型raw记录、六处handler前缀读取迁移，以及region内合格self late-entry的prefix/suffix规划和fresh lower。影响结构但尚未lower的witness也在发布前复核。CodeChanged只在Function内部销毁整个未发布attempt后有限重试，最后回原Function decode；不触发通用异常catch的BlockOnly。依赖旧IR的outer frontier、跨块分配、call-return及最终证明保留。

r1在构建前因取指异常被过宽catch而修正；r2完成16目标实际构建，独立审查在执行前发现JMP误标Linear及新测试的旧语义oracle问题，未运行r2。r3修正后再次完成16目标构建。各版source、补丁和实际build日志独立保留，没有覆盖未通过记录。

一次单独frontier命令实际退出0，执行全部16用例、1570断言，0失败/错误/跳过。原collector末尾误把私有程序名派生的JUnit classname写成固定`swift_test.global`，故 `targeted-frontier-r3/execution.json` 仍为failed。独立审查从实际argv推导 `swift_c34_frontier_r3.global`，重核原XML、摘要、前后source/native/config和手机输入身份后形成 `qualification.json`（SHA `301787942b14118d925e8064c018427b78ec72b96789b40dae330662460c804a`）；没有改写failed记录或重跑测试。

完整54质量计划经独立执行前终审后，已唯一实际执行完毕，sequence与32/22两个collector均complete、terminal exit0。原32+22命令/过滤器/历史失败比较器不改，16角色部署、9角色实际执行、7仅部署；frontier全16用例来自上面的独立前置记录，不计入54。计划 `quality/frozen-execution-plan.json` SHA `9e7d64650d14e63ec38597113a94803c0ff7c517fe1134016bb643323f7c7df6`，inputs SHA `2a12cab12371f44cff7255b2f3d4baf596e4be660185f91997b8cc1ddfe54bbb`。独立审查重算146个raw后PASS：656 rows/874948断言，原22项R6历史失败的完整failure name/tag/attrs/text和exit精确匹配，无新增失败/error/skip；FP42/6736 B与6/7992 B按实际PC/order/bytes/length/FNV保持。20个父/单进程native命令1773 checks与8个不同PID子阶段392 checks分域核算，不相互替代。`full-quality-independent-review.json` SHA `a64b72bd9fa70138d5ff9fdbe347f9f2f6f12779a20d20e0374f7d6e31f7cae5`，所有reader关闭，仅签正确性与代码门。

Root另完成 `post-quality-inputs.json`（SHA `945d1c5421d0e78299fa4df07b63d2d559e0247fd1c96b12d8ed44763553f979`）：source2418/退役118/native16/config3全部保持，手机16角色实际为C34generic、其他九项仍原SHA，全部角色、FEX及定向私有程序无活跃进程，两个upload stage关闭。手机16角色改为实验身份不等于替换选定C31检查点，未宣称16角色全部执行。

native审计已完成，对精确generic基线 `09a07474…` 与当前 `e372ea6e…`：`.text` +15320 B，record112 B，RawPlan48 B，RegionDecoder304→384 B。Translate入口帧2816→2944 B、DecodeBlock1056→1104 B、Run544→720 B、新Predecode704 B。每region record容量4096条，稳态448 KiB，扩容旧/新buffer同时存活时推导672 KiB，另计metadata/allocator/IR等；这不是全进程上限或实测RSS。未见4096条record的栈数组。收据 `native-audit/native-final-review.json` SHA `bb900617a042cf1622dbcba3c948bbed6c3cc63ee84aa4779f05f842d37776a4`，所有reader已关闭。

四点MaxRSS也已唯一实际执行并独立复核通过：Function baseline18704/candidate19088 KiB（+384），Block baseline16044/candidate15920 KiB（−124）。41commands、20份point原raw和8份condition raw、实际PID/CPU4、env-i、CRC2000、前后身份和严格双AND停止门均重核；无追加或替换点。`memory-sanity/execution.json` SHA `d53a120c278026b08a0b9d7eebbb88e25194464278a722fe4f25e1eff0896f1d`，独立收据 SHA `027053898c7d50eb8230e07c14f4b33d0194211ee718d1da031dd69eb65b9e9d`。不据此声称内存改善、arena峰值硬上限或性能提速；ordinary无任何PROF/VERIFY诊断变量。

随后root重新核对source2418/退役118/native16/config3、手机完整25项、六条件及CPUonline、all-owned idle，收据 `device-readiness-before-coarse.json` SHA `11b4e70720a590501eb12df8eb470dbaf0fc76aada6cf3219739f9d5219c6923`。七轮56点计划随后生成（`compile-cost-c34-function-raw-first-r1-coarse-v1/plan.json` SHA `418aa9c4e9169adba1a8f9584f09922da7846c98961bfc6d451aee4a6a63de2e`），270个实际artifacts、原producer `b4ab582d…`。该批后来已唯一运行并闭合，结果如下。生成器R1对未来RSS字段的假定在执行前被发现并改为消费真实singular `binding.execution`；原未执行R1保留，R2唯一prepare terminal0，未产生或替换测量点。

原56点唯一实际执行terminal0，无替换/补点。另一份独立parser从730个raw、172commands、112个前后telemetry对象及245个R内artifact重建计时、PMU、PID、argv/env、身份、CRC/FP及六条件，结论是**测量有效、原采用门失败**。非R的25个registry对象由producer前后实测及root actual postguard闭合，未把历史B路径当当前候选ELF重hash。28个ratio使用整数Fraction：

| 组 | candidate/baseline配对中位 | 七对范围 | 更快/更慢 |
| --- | ---: | ---: | ---: |
| Core Function | 0.9969761686 | 0.988170–1.028249 | 4/3 |
| Core Block | 1.0167464253 | 0.973570–1.050716 | 3/4 |
| FP Function | 0.9868632277 | 0.971069–0.991493 | 7/0 |
| FP Block | 1.0054482508 | 0.983213–1.031232 | 2/5 |

两个Core正门均false；四组亦没有触发all7变慢的负控。FP不能补开Core门，因此REJECT，无新PGO/普通72/FEX点。独立raw审计SHA `d17f161931cc4f52482886774b7a652d28b4925ea3f4e6a895becc005af46f27`，策略判决SHA `bf25534e64ad006ad9a7f018ae3525efccf8f17e57abd8806b9728f517e58e38`，所有reader在恢复前关闭。

Function两臂的439 Function+7 Block、4906 decoded blocks、340944 host bytes和82819发布ID槽全部一致；decode桶配对中位1.001332，未见净下降。Block单元数有小变动（baseline2415–2419、candidate2414–2419），host bytes/ID槽也变化，逐对delta原样保留，不归一或补点。ir_insts仍是ID槽统计，不是live节点；阶段桶不是纯预解码/校验耗时。

Root随后只guarded回写13份原源码，完整2418/118验证回3cb，再恢复原6项PGO flags和392typed cache，以匹配0e4a profile重建16目标，三配置和16ELF逐项exact。`restore-c31.json` SHA `b9c8f50c474f9c6418150d59667bb4448794812b1dec617039ea079df0c977dd`。之后phone16精确恢复selectedPGO、其他9项不变，无额外target执行；`restore-phone-c31.json` SHA `7d3a9953c0613fe52f2cb905ec85ad250eb8c26c8d8af2cda2f1a380b5e230ae`。root再核source/392typed/config3/host16/fullphone25/idle闭合，`withdrawal-closure.json` SHA `81e2b5720bf1c3bee9cab28ad061102d2c3e38db9b9dbb6c7125170444d7f616`；独立撤回复核 `withdrawal-independent-review.json` SHA `7b71f9646b3fb5b07ce55f4f2bddc6f93517dcd9436ee34e22a1c88fdf426c9a` 已PASS，R内原件、实际journal与压缩件完整读回均重核；其当前W/B/phone资格明确依赖root的实际闭合，而非把历史B槽再hash成候选。未reset、checkout、丢弃其他WIP或使用旧profile编译C34。

实验main和swift_test压缩件完整读回为原e372/8f20，分别23789014/56757146 B，合计80546160 B留在Data2。私有frontier目录仅在ELF/XML前后SHA核对后删除两文件，回收230552491逻辑B；230 MB主机暂存也回收。首次exec-out引号错误所得107 B状态文本和failed journal保留，未删设备文件；后用标准adb pull完成同一ELF的保全与清理，没有重跑测试或性能点。原JUnit classname观察错误及failed execution也保持。既有21.0054×/13.1035×外部差距不变。

## 下一项N3：具体布局与完整生命周期净账

有界源码审查在53个与C31/C34均同SHA的SVM文件和5个固定FEX源文件上完成，887215 B文本快照及单文件清单保留；所有W/B/FEX reader已关闭。报告 `compile-cost-ir-representation-design-r1/review.md` SHA `c9321d4c4799797a474e72bd1b478ee3ecda31edab78bf869067bdb2613014db`，结论为存在完整单一store迁移方向，但生产性能原型仍HOLD。

当前94 B Inst里60 B是5个12 B Arg容量，默认构造不等于memset60 B；Inst已有96 B stride的TLS bump/freelist，FEX20 B header另加payload/双arena。稳定header、opcode payload和唯一owner须连同HIR值/用途、mutable队列、DCE/facts/两种RA及backend/序列化消费者完整迁移，不能只换getter或永久保留旧facade。

唯一缺口为候选具体header/用途/参数布局在真实owner生命周期内的净存储和构造/初始化/索引账，必须含tombstone、不完整raw节点、旧/新payload、Params/Lambda、chunk slack、multi-region/双prepared/重试共存。下一步先设计并核ABI，才考虑现有IR_DETAIL两模式各一次有界aggregate census；没有新56或收益结论，不把旧profile用于新诊断源。若净账不足则停止N3，不退回旧小包。完整两模式FEX同数量级且代码质量不退的goal继续active。

## N3 R2：模型已复核，诊断源码正在实现

R1模型审查没有放行完整census，明确指出累计分配/释放工作和Block懒用途图的索引域两处缺口。R2已按无跨store缓存的主策略补齐生命周期动作，并将Links及20 B edge的端点统一到完整stable-slot域R，包含墓碑和scope之外的槽。原R1模型与HOLD记录保持原样。R2模型SHA `440fca78…`，独立模型审查SHA `00801df3…`；结论只允许进入诊断实现，不代表生产迁移或收益。

当前已写出两组暂存hook及中央有界observer。它们观察实际Inst/Params生命周期、Block归属、共享HIR池/lease、数组分配请求（包括编译器cookie）、worklist和prepared借用，补计堆比较、重排写入与owner表增删。完成后按现有开关分别执行Function、Block一次Core2000，仅作全生命周期净账，不计入性能表。完整组合源码、generic构建和实采仍待完成；16 MiB预算、原始结果和任何HOLD都必须保留，不能追加替换点。

本阶段已实际核对2418个C31源文件无差异，并保全原host16、392项typed cache及三个配置文件。暂存Inst/Args/Block单元及中央observer已做NDK语法检查，覆盖其它单元的检查和独立源码审查继续进行；这些检查不是VM测试或性能证据。当前正式FEX差距仍为Function 21.0054倍、Block 13.1035倍，目标未完成。

## 可复核记录

| 内容 | active-results 下的记录 |
| --- | --- |
| 历史 C0 普通采样 | `compile-cost-c0-profile/qualification.json`、`compile-cost-c0-hotspot-review.json` |
| 首批 source 与 build | `compile-cost-c12-r1-source.json`、`compile-cost-c12-r1-build.json`、`compile-cost-c12-r1-native.json` |
| 首批门测与原基线控制 | `compile-cost-c12-r1-gate-qualification.json`、`compile-cost-c12-r1-baseline-control/execution.json` |
| 原始失败与未执行部分的接续 | `compile-cost-c12-r1-gate/execution.json`、`compile-cost-c12-r1-gate-v2/execution.json`、`compile-cost-c12-r1-gate-continuation/execution.json` |
| 实际发射字节 | `compile-cost-c12-r1-emission/execution.json` |
| 24 点计时与原始验证 | `compile-cost-c12-r1-coarse-v1/execution.json`、`paired-summary.json` |
| 独立复核 | `compile-cost-c12-r1-independent-review.json`，SHA `514d5ad0…` |
| 精确产物与源差分 | `compile-cost-c12-r1-preservation.json`、`compile-cost-c12-r1-launcher.tar.zst`、`compile-cost-c12-r1-source-delta.patch` |
| 第二批冻结与构建 | `compile-cost-c3-r1-source.json`、`compile-cost-c3-r1-build.json` |
| 第二批门测与发射字节 | `compile-cost-c3-r1-gate-qualification.json`、`compile-cost-c3-r1-emission/execution.json` |
| 第二批 24 点计时 | `compile-cost-c3-r1-coarse-v1/execution.json`、`paired-summary.json` |
| 第二批精确产物与源差分 | `compile-cost-c3-r1-preservation.json`、`compile-cost-c3-r1-launcher.tar.zst`、`compile-cost-c3-r1-source-delta.patch` |
| 当前 CoreMark IR 与 alias 上界 | `compile-cost-c3-r1-core-ir/execution.json`、`compile-cost-c5-r1-alias-scale-review.json` |
| 冷诊断实际汇编与语义 | `compile-cost-c4-r1-cold-assert-asm-review.json`、`compile-cost-c4-r1-gate-qualification.json` |
| 冷诊断计时与精确归档 | `compile-cost-c4-r1-coarse-v1/execution.json`、`compile-cost-c4-r1-preservation.json` |
| ThinLTO 配方与实际构建 | `compile-cost-thinlto-r1-recipe.json`、`compile-cost-c5-r1-thinlto/build.json` |
| ThinLTO 修正配方与成功链接 | `compile-cost-thinlto-r2-recipe.json`、`compile-cost-c5-r2-thinlto/build.json` |
| 同源码 ThinLTO 独立结果 | `compile-cost-c5-lto-r2-coarse-v1/independent-review.json`、`compile-cost-c5-lto-r2-preservation.json` |
| 同源码 ThinLTO 实际汇编 | `compile-cost-c5-lto-r2-asm-review.json` |
| 复杂度撤回与后续 14 目标构建 | `compile-cost-c3-r1-complexity-retention-review.json`、`compile-cost-c6-r1-source.json`、`compile-cost-c6-r1-build.json` |
| C6 完整受影响门测与发射 | `compile-cost-c6-r1-gate-qualification.json`、`compile-cost-c6-r1-emission/execution.json` |
| C6 直接原始基线对照 | `compile-cost-c6-r1-coarse-v1/execution.json`、`paired-summary.json` |
| C6 精确保留与当前普通采样 | `compile-cost-c6-r1-preservation.json`、`compile-cost-c6-profile/execution.json` |
| C6 coarse 与门测独立复核 | `compile-cost-c6-r1-coarse-v1/independent-review.json` |
| C6 普通 PMU 全量结果 | `compile-cost-c6-r1-core-pmu-v1/manifest.json`、`compile-cost-c6-r1-fp-pmu-v1/manifest.json` |
| C6 普通 PMU 独立重放 | `compile-cost-c6-r1-ordinary-pmu-review.json`、`compile-cost-c6-r1-ordinary-pmu-root-independent-review.json` |
| C7 拒绝与精确恢复 C6 | `compile-cost-c7-dce-r1-preservation-and-decision.json`、`compile-cost-c7-dce-r1-restore-c6/execution.json` |
| C7 负控独立复核 | `compile-cost-c7-dce-r1-core-coarse-v1/independent-review.json` |
| C6 既有 PROF2 诊断范围 | `compile-cost-c6-prof2-diagnostic/review.json`、`counter-scope.json` |
| C8 数学与实际汇编 | `compile-cost-c8-known-bits-r1-independent-math-review.json`、`compile-cost-c8-known-bits-r1-native-review.json` |
| C8 拒绝、归档与恢复 | `compile-cost-c8-known-bits-r1-preservation-and-decision.json`、`compile-cost-c8-known-bits-r1-restore-c6/execution.json` |
| C8 24 点独立复核 | `compile-cost-c8-known-bits-r1-coarse-v1/independent-review.json` |
| C11 实际最小差分与合同 | `compile-cost-c11-register-versions-r1-root-final.patch`、`compile-cost-c11-resident-use-contract-review.json` |
| C11 回归及缺少套件的补充 | `compile-cost-c11-register-versions-r1-gate-qualification.json`、`compile-cost-c11-register-versions-r1-gate-continuation/execution.json` |
| C11 相邻对照及独立复核 | `compile-cost-c11-register-versions-r1-coarse-v1/paired-summary.json`、`independent-review.json` |
| C11 精确归档与源快照 | `compile-cost-c11-register-versions-r1-preservation-and-decision.json` |
| C9 构建与受影响回归 | `compile-cost-c9-cold-diagnostic-r1-build.json`、`compile-cost-c9-cold-diagnostic-r1-gate-qualification.json` |
| C9 相邻对照与暂存决定 | `compile-cost-c9-cold-diagnostic-r1-coarse-v1/paired-summary.json`、`compile-cost-c9-cold-diagnostic-r1-root-decision.json` |
| C9 实际汇编与精确归档 | `compile-cost-c9-cold-diagnostic-r1-native-review.json`、`compile-cost-c9-cold-diagnostic-r1-preservation-and-decision.json` |
| C9 完整独立复核 | `compile-cost-c9-cold-diagnostic-r1-coarse-v1/independent-review.json` |
| C14 原构建目标审查与 driver 对照 | `compile-cost-c14-build-target-review.json`、`compile-cost-c14-build-target-dryrun.json` |
| C14 两项配置变化与构建 | `compile-cost-c14-mtune-r1/configuration.json`、`compile-cost-c14-mtune-r1-build.json` |
| C14 拒绝与完整独立复核 | `compile-cost-c14-mtune-r1-preservation-and-decision.json`、`compile-cost-c14-mtune-r1-coarse-v1/independent-review.json` |
| C14 通用配置恢复及注释差异资格 | `compile-cost-c14-mtune-r1-rollback/execution.json`、`qualification.json` |
| C10 两文件冻结、合同和实际汇编 | `compile-cost-c10-early-capacity-r1-root-freeze.json`、`compile-cost-c10-early-capacity-r1-contract-review.json`、`compile-cost-c10-early-capacity-r1-native-review.json` |
| C10 拒绝与独立复核 | `compile-cost-c10-early-capacity-r1-preservation-and-decision.json`、`compile-cost-c10-early-capacity-r1-coarse-v1/independent-review.json` |
| C10 精确恢复 C9 | `compile-cost-c10-early-capacity-r1-restore-c9/execution.json`、`device-restore.json` |
| C17 同 ELF 的预定 A/A 校准 | `compile-cost-c17-c9-aa-r1/plan.json`、`execution.json` |
| C17 完成及独立重放 | `compile-cost-c17-c9-aa-r1/paired-summary.json`、`independent-review.json` |
| C18 预定采样策略及三文件差分 | `compile-cost-c18-argument-layout-r1-sampling-policy.json`、`compile-cost-c18-argument-layout-r1-unapplied/complete.delta.patch` |
| C18 源码、构建及质量 | `compile-cost-c18-argument-layout-r1-source.json`、`compile-cost-c18-argument-layout-r1-build.json`、`compile-cost-c18-argument-layout-r1-gate-qualification.json` |
| C18 实际布局与指令复核 | `compile-cost-c18-argument-layout-r1-contract-review.json`、`compile-cost-c18-argument-layout-r1-native-review.json` |
| C18 发射字节与七轮计划 | `compile-cost-c18-argument-layout-r1-emission/execution.json`、`compile-cost-c18-argument-layout-r1-coarse-v1/plan.json` |
| C18 完整独立重放与暂存决定 | `compile-cost-c18-argument-layout-r1-coarse-v1/independent-review.json`、`compile-cost-c18-argument-layout-r1-root-decision.json` |
| C18 精确产物及三文件快照 | `compile-cost-c18-argument-layout-r1-preservation.json`、`compile-cost-c18-argument-layout-r1-launcher.tar.zst` |
| C19 PGO 可行性及未执行配方 | `compile-cost-c19-pgo-feasibility-review.json` |
| C20/C21 源码与固定 FEX 对照 | `compile-cost-fex-owner-consolidation-review.json` |
| C20 两文件冻结及构建 | `compile-cost-c20-scratch-owner-r1-root-freeze.json`、`compile-cost-c20-scratch-owner-r1-build.json` |
| C20 独立差分及相邻参照 | `compile-cost-c20-scratch-owner-r1-isolated.delta.patch`、`compile-cost-c20-scratch-owner-r1-baseline-role.json` |
| C20 门测、发射与独立复核 | `compile-cost-c20-scratch-owner-r1-gate-qualification.json`、`compile-cost-c20-scratch-owner-r1-emission/execution.json`、`compile-cost-c20-scratch-owner-r1-coarse-v1/independent-review.json` |
| C20 实际汇编与容量取舍 | `compile-cost-c20-scratch-owner-r1-native-review.json`、`compile-cost-c20-scratch-capacity-comparison.json` |
| C20 精确归档与暂存决定 | `compile-cost-c20-scratch-owner-r1-preservation.json`、`compile-cost-c20-scratch-owner-r1-root-decision.json` |
| C19 同源码参照及预定策略 | `compile-cost-c19-pgo-r1-baseline-role.json`、`compile-cost-c19-pgo-r1-sampling-policy.json` |
| C19 实际配置及插桩构建 | `compile-cost-c19-pgo-r1-generate/prepare.json`、`configure.json`、`build.json`、`training-native.json` |
| C19 初次设置失败与 v2 修复 | `compile-cost-c19-pgo-r1-training/train.json`、`compile-cost-c19-pgo-r1-training-v2/setup-repair.json`、`setup-independent-review.json` |
| C19 四训练与严格合并 | `compile-cost-c19-pgo-r1-training-v2/train.json`、`merge.json`、`qualification.json`、`independent-review.json` |
| C19 use 构建及独立审查 | `compile-cost-c19-pgo-r1-use/build.json`、`native.json`、`independent-build-review.json` |
| C19 真实告警分类 | `compile-cost-c19-pgo-r1-use/profile-diagnostic-classification.json` |
| C19 质量与相邻计时 | `compile-cost-c19-pgo-r1-gate-qualification.json`、`compile-cost-c19-pgo-r1-coarse-v1/paired-summary.json` |
| C19 精确主程序与邻接决定 | `compile-cost-c19-pgo-r1-preservation.json`、`compile-cost-c19-pgo-r1-adjacent-decision.json` |
| C19 相邻完整独立重放 | `compile-cost-c19-pgo-r1-coarse-v1/independent-review.json` |
| C19 直接原 R6 累计对照 | `compile-cost-c19-pgo-r1-direct-original-r6-coarse-v1/paired-summary.json`、`root-cumulative-decision.json`、`independent-review.json` |
| C19 普通 PMU 冻结与实物复核 | `compile-cost-c19-pgo-r1-pmu72-freeze.json`、`compile-cost-c19-pgo-r1-pmu72-frozen-host-independent-review.json` |
| C19 普通 PMU 原始完整结果 | `compile-cost-c19-pgo-r1-core-pmu-v1/manifest.json`、`compile-cost-c19-pgo-r1-fp-pmu-v1/manifest.json` |
| C19 普通 PMU 独立复核与采用范围 | `compile-cost-c19-pgo-r1-pmu72-independent-review.json`、`compile-cost-c19-pgo-r1-root-adoption-decision.json` |
| 当前 PGO 普通热点与边界 | `compile-cost-c19-pgo-r1-profile/qualification.json`、`compile-cost-c19-pgo-r1-current-hotspot-parent-review.json` |
| C22 局部 use-count 索引暂缓 | `compile-cost-c22-home-use-counts-review.json` |
| C24 预定策略、完整重放与撤回 | `compile-cost-c24-block-fault-batch-r1-sampling-policy.json`、`compile-cost-c24-block-fault-batch-r1-coarse-v1/independent-review.json`、`compile-cost-c24-block-fault-batch-r1-root-decision.json` |
| C24 生产源码恢复 | `compile-cost-c24-block-fault-batch-r1/source-only-restore-to-c19.json` |
| C25 原夹具失败与精确保全 | `compile-cost-c25-vixl-scratch-owner-r1-focused/execution.json`、`compile-cost-c25-vixl-scratch-owner-r1-failed-fixture-preservation.json` |
| C25 实物布局及堆调用核对 | `compile-cost-c25-vixl-scratch-owner-r1/independent-native-owner-final-review.json` |
| C25 修正后策略与实际质量 | `compile-cost-c25-vixl-scratch-owner-r2-sampling-policy.json`、`compile-cost-c25-vixl-scratch-owner-r2-gate-qualification.json` |
| C25 实际执行源码与暂存命名修正 | `compile-cost-c25-vixl-scratch-owner-r2/focused-execution-source-erratum.json` |
| 同夹具一致旧实现控制与候选恢复 | `compile-cost-c25-fixture-control-r1/independent-review.json` |
| C25 全部七轮原始执行 | `compile-cost-c25-vixl-scratch-owner-r2-coarse-v1/plan.json`、`compile-cost-c25-vixl-scratch-owner-r2-coarse-v1/execution.json` |
| C25 完整独立重放及预定策略判决 | `compile-cost-c25-vixl-scratch-owner-r2-coarse-v1/independent-review.json`、`compile-cost-c25-vixl-scratch-owner-r2-independent-policy-decision.json` |
| C25 不采用决定与精确候选归档 | `compile-cost-c25-vixl-scratch-owner-r2-root-decision.json`、`compile-cost-c25-vixl-scratch-owner-r2/launcher-preservation.json` |
| C25 撤回后的实际构建与最终恢复 | `compile-cost-c25-withdraw-to-c20-fixture-r1-build.json`、`compile-cost-c25-withdraw-to-c20-fixture-r1/final-restoration-receipt.json` |
| 手机选定主程序与夹具测试恢复 | `compile-cost-c25-withdraw-to-c20-fixture-r1/phone-selected-main-and-fixture-test-restoration.json` |
| 后续分配重复与稳定序容器可行性 | `compile-cost-next-major-fex-structural-review-20261027.json`、`compile-cost-active-interval-storage-feasibility-20261027.json` |
| 发射入口记录 owner 的限制 | `compile-cost-next-emitter-entry-owner-feasibility-supplement.json` |
| C26 冻结策略与源合同 | `compile-cost-c26-fixed-clobber-r1-sampling-policy.json`、`compile-cost-c26-fixed-clobber-r1-source-contract-review.json` |
| C26 实物与完整质量 | `compile-cost-c26-fixed-clobber-r1/native-audit/candidate-native-final-review.json`、`compile-cost-c26-fixed-clobber-r1-gate-qualification.json` |
| 七案例 SECTION 名册、原失败与接纳 | `compile-cost-c26-fixed-clobber-r1-targeted-section-manifest-r2.json`、`compile-cost-c26-fixed-clobber-r1-targeted-protocol-erratum-r2.json`、`compile-cost-c26-fixed-clobber-r1-targeted-baseline-xpool0-admission-r2.json` |
| C26 七轮重放、负控与源码恢复 | `compile-cost-c26-fixed-clobber-r1-coarse-v1/independent-review.json`、`compile-cost-c26-fixed-clobber-r1-independent-policy-decision.json`、`compile-cost-c26-fixed-clobber-r1/source-only-restore-to-840.json` |
| C27 冻结策略与源合同 | `compile-cost-c27-active-interval-vector-r1-sampling-policy.json`、`compile-cost-c27-active-interval-vector-r1-source-contract-review.json` |
| C27 实物与完整质量 | `compile-cost-c27-active-interval-vector-r1/native-audit/native-final-review.json`、`compile-cost-c27-active-interval-vector-r1-gate-qualification.json` |
| C27 七轮重放与正门未通过 | `compile-cost-c27-active-interval-vector-r1-coarse-v1/independent-review.json`、`compile-cost-c27-active-interval-vector-r1-independent-policy-decision.json` |
| 最新 source840 / host16 / phone19 最终恢复 | `compile-cost-c27-withdraw-to-c20-fixture-r1-build.json`、`compile-cost-c27-withdraw-to-c20-fixture-r1/final-restoration-receipt.json` |
| IR 存储与 owner 可行性 | `compile-cost-ir-storage-feasibility-audit/feasibility-review.json` |
| 空单元工作覆盖与最终输入复用的限制 | `compile-cost-empty-unit-work-feasibility-review.json`、`compile-cost-target-complete-existing-inputs-feasibility-review.json` |
| 下一阶段 FEX 对照与独立排序审查 | `compile-cost-fex-structural-plan-input-20261027.json`、`compile-cost-next-stage-priority-plan-review-20261027.json` |
| N0 全部采样、映射与精确 inline/source | `compile-cost-next-stage-source-attribution/analysis.r3.json`、`compile-cost-next-stage-source-attribution/independent-review.json` |
| PID 分组修正与原失败假说 | `compile-cost-next-stage-source-attribution/reporting-aggregation-erratum.json`、`compile-cost-next-stage-source-attribution/raw-prefix-failed-hypothesis/` |
| N0 SVM/FEX 工作量与边界账本 | `compile-cost-next-stage-unit-ledger.json`、`compile-cost-next-stage-unit-ledger-id-slot-erratum.json` |
| 已有 IR 明细诊断与原读错误 | `compile-cost-ir-construction-diagnostic-c19-r1/plan.json`、`compile-cost-ir-construction-diagnostic-c19-r1/execution.json` |
| 原 Function 追回、单次 Block 与独立核对 | `compile-cost-ir-construction-diagnostic-c19-r1/continuation-r2.json`、`compile-cost-ir-construction-diagnostic-c19-r1/independent-review.json` |
| Owner 直借的 UAF 反例 | `compile-cost-n0-owner-borrow-feasibility-review.json` |
| N2 真实 C19 raw/final roster 与阶段观测边界 | `compile-cost-n2-existing-roster-review.json` |
| C28 冻结政策、R1 构建失败与 R2 测试失败 | `compile-cost-c28-literal-preservation-r1-sampling-policy.json`、`compile-cost-c28-literal-preservation-r1-build.json`、`compile-cost-c28-literal-preservation-r2/quality-sequence-execution.json` |
| C28 R3 完整质量、实物与实际少构造节点 | `compile-cost-c28-literal-preservation-r3/quality-sequence-execution.json`、`compile-cost-c28-literal-preservation-r3/native-audit/native-final-review.json`、`compile-cost-c28-literal-preservation-r2/ir-construction-review.json` |
| C28 七轮完整重放与拒绝 | `compile-cost-c28-literal-preservation-r3-coarse-v1/independent-review.json`、`compile-cost-c28-literal-preservation-r3-independent-policy-decision.json` |
| C28 精确恢复及手机私有输出清理 | `compile-cost-c28-withdraw-to-c20-fixture-r1/final-restoration-receipt.json`、`compile-cost-c28-withdraw-to-c20-fixture-r1/phone-private-output-cleanup.json` |
| N1 Query 与 N3 offset 小候选的 NO-GO | `compile-cost-n1-recursive-query-readonly-review.json`、`compile-cost-n3-public-index-offset-admission-review.json` |
| decode 延迟写回设计的实际准入与 FEX flush 限制 | `compile-cost-next-decode-dirty-publication-feasibility-review.json` |
| CPU4 PMU 事件枚举及同组能力检查 | `compile-cost-n3-pmu-capability-discovery-r1/execution.json`、`compile-cost-n3-pmu-capability-discovery-r1/counter-group-execution.json` |
| 实际两模式 C19 四事件诊断及完整离线归因 | `compile-cost-n3-cache-profile-c19-r1/analysis.json`、`compile-cost-n3-cache-profile-c19-r1/root-assessment.json`、`compile-cost-n3-cache-profile-c19-r1/temporary-cleanup.json` |
| 单 guest 写回合并覆盖为零的 NO-GO | `compile-cost-n2-single-guest-state-publication-coverage-review.json` |
| Block AdvancePC 候选、现有窄宽度问题与消费者边界 | `compile-cost-n2-block-advancepc-admission-review.json` |
| C29 原生产语义负控及独立 guard 源码冻结 | `compile-cost-c29-advancepc-width-control-r1/negative-control/execution.json`、`compile-cost-c29-advancepc-width-guard-r1-source-contract-review.json` |
| C29 r1 部分语义资格与严格字节失败 | `compile-cost-c29-advancepc-width-guard-r1/qualification-partial-review.json`、`compile-cost-c29-advancepc-width-guard-r1/quality-sequence-execution.json` |
| C29 实际缓冲、切块因果及持久化审查 | `compile-cost-c29-advancepc-width-guard-r1/generated-buffer-review/generated-buffer-review.json`、`compile-cost-c29-advancepc-width-guard-r1/emission-causality-and-persistence-review.json` |
| C29 延迟身份、精确归档与预先冻结补证 | `compile-cost-c29-advancepc-width-guard-r1/delayed-post-quality-inputs.json`、`compile-cost-c29-advancepc-width-guard-r1/launcher-preservation.json`、`compile-cost-c29-advancepc-width-guard-r2/supplemental-correctness-protocol.json` |
| C29 r2 实际构建、冻结计划与首条失败 | `compile-cost-c29-advancepc-width-guard-r2-build.json`、`compile-cost-c29-advancepc-width-guard-r2/frozen-execution-plan-v2.json`、`compile-cost-c29-advancepc-width-guard-r2/supplemental-quality/execution.json`、`compile-cost-c29-advancepc-width-guard-r2/delayed-post-failed-first-command.json` |
| C29 PageFatal 与懒 owner 的测试前提审查 | `compile-cost-c29-advancepc-width-guard-r2/pagefatal-rip-contract-review.json`、`compile-cost-c29-advancepc-width-guard-r3/lazy-owner-contract-review.json`、`compile-cost-c29-advancepc-width-guard-r3/failed-fifth-consumer-review.json` |
| C29 r4 预先协议、完整32命令与精确保全 | `compile-cost-c29-advancepc-width-guard-r4/supplemental-correctness-protocol-v2.json`、`compile-cost-c29-advancepc-width-guard-r4/frozen-execution-plan.json`、`compile-cost-c29-advancepc-width-guard-r4/supplemental-quality/execution.json`、`compile-cost-c29-advancepc-width-guard-r4/launcher-preservation.json` |
| C29 r4 独立完整复核 | `compile-cost-c29-advancepc-width-guard-r4/qualification-review.json`，SHA `13d3bf1f…` |
| Block 合并的共享 owner 设计与未闭合消费者 | `compile-cost-c30-block-advancepc-shared-owner-design-review.json` |
| C30 源码反例、真实 owner 与离线范围收敛 | `compile-cost-c30-block-advancepc-consumer-admission-convergence.json` |
| C31 实际 owner-search 循环归因与单 writer 生命周期准入 | `compile-cost-c31-fault-owner-index-admission-review.json` |
| C31 编辑前冻结的三文件、控制与七轮策略 | `compile-cost-c31-fault-owner-index-r1-sampling-policy.json` |
| C31 旧生产正向控制及独立复核 | `compile-cost-c31-fault-owner-index-control-r1/positive-control/execution.json`、`compile-cost-c31-fault-owner-index-control-r1/positive-control-review.json` |
| C31 实际 16 构建、54 质量与完整独立复核 | `compile-cost-c31-fault-owner-index-r1-build.json`、`compile-cost-c31-fault-owner-index-r1/quality-sequence-execution.json`、`compile-cost-c31-fault-owner-index-r1/full-quality-independent-review.json` |
| C31 实物审计、postflight 与明确基线角色 | `compile-cost-c31-fault-owner-index-r1/native-audit/native-final-review.json`、`compile-cost-c31-fault-owner-index-r1/post-quality-inputs.json`、`compile-cost-c31-fault-owner-index-r1/baseline-role-preparation.json` |
| C31 原56点、独立重放与预定策略决定 | `compile-cost-c31-fault-owner-index-r1-coarse-v1/{plan,execution,independent-review}.json`、`compile-cost-c31-fault-owner-index-r1-independent-policy-decision.json` |
| C31 generic 暂存边界与测后全角色检查 | `compile-cost-c31-fault-owner-index-r1/root-adjacent-decision.json`、`compile-cost-c31-fault-owner-index-r1/post-coarse-inputs.json` |
| C31 匹配 PGO、四次训练与严格合并 | `compile-cost-c31-pgo-r1/generate/{prepare,configure,build,training-native}.json`、`compile-cost-c31-pgo-r1/training/{plan,train,merge,qualification}.json` |
| C31 profile-use 预检失败与路径修正 | `compile-cost-c31-pgo-r1/use-prepare-failed-r1/prepare.json`、`compile-cost-c31-pgo-r1/use-r2-path-guard-repair.json` |
| C31 freshPGO 构建配方独立复核与实际16产物 | `compile-cost-c31-pgo-r1/fresh-pgo-recipe-independent-review.json`、`compile-cost-c31-pgo-r1/use/{build,native,profile-diagnostic-classification}.json` |
| C31 freshPGO 54质量 root 汇总与现场检查 | `compile-cost-c31-pgo-r1/gate-qualification.json`、`compile-cost-c31-pgo-r1/post-quality-inputs.json` |
| C31 freshPGO 相邻56完整重放与策略判决 | `compile-cost-c31-pgo-r1-coarse-v1/{plan,execution,paired-summary,independent-review}.json`、`compile-cost-c31-pgo-r1/{independent-policy-decision,root-adjacent-decision,post-coarse-inputs}.json` |
| C31 freshPGO 直接原R6完整56与独立判决 | `compile-cost-c31-pgo-r1-direct-original-r6-coarse-v1/{plan,execution,paired-summary,independent-review}.json`、`compile-cost-c31-pgo-r1/{direct-original-r6-independent-policy-decision,post-direct-inputs,post-direct-root-closure}.json` |
| C31普通72完整复核、现场收口与新检查点 | `compile-cost-c31-pgo-r1/ordinary-pmu72-independent-review.json`、`compile-cost-c31-pgo-r1/{post-pmu72-inputs,post-pmu72-root-closure,selected-checkpoint}.json` |
| 外部同数量级工程门的有限适配协议 | `compile-cost-external-engineering-gate-protocol-r1.json` |
| 当前同批外部28点、有效测量与失败数值门 | `compile-cost-external-engineering-gate-r1/{plan,execution,engineering-gate-summary,independent-final-review,independent-timing-ledger,post-execution-inputs}.json` |
| 当前C31普通调用链资格、main源码归因与caller去重 | `compile-cost-c31-pgo-r1-profile/qualification-independent-review.json`、`compile-cost-c31-pgo-r1-profile/offline-attribution/{analysis,allocator-caller-union,shared-library-fixed20-admission}.json`、`compile-cost-c31-pgo-r1-profile/offline-reader-closure.json` |
| Function重放临时generic、两次原始观测与独立计数 | `compile-cost-function-replay-diagnostic-r1/{build,collection,qualification-independent-review}.json`、`compile-cost-function-replay-diagnostic-r1/{function,block}/{stdout,stderr}.txt` |
| 临时诊断精确保全、恢复及手机回收 | `compile-cost-function-replay-diagnostic-r1/{diagnostic-preservation,restore,private-device-cleanup}.json` |
| 仅在R准备的两文件row往返清理，未应用 | `compile-cost-c31-value-row-identity-proposal/{proposed.patch,contract-review.json}` |
| 冻结后action消费游标的低覆盖暂缓结论 | `compile-cost-c31-frozen-action-consumption-review.json` |
| C32 冻结策略、源码与实际16目标 | `compile-cost-c32-transient-storage-r1/{policy,source,native,candidate}.json` |
| C32 旧生产生命周期控制与精确保全 | `compile-cost-c32-transient-storage-r1/{control-qualification,control-preservation}.json` |
| C32 完整54质量、实际身份与实物审计 | `compile-cost-c32-transient-storage-r1/{full-quality-independent-review,post-quality-inputs}.json`、`compile-cost-c32-transient-storage-r1/native-audit/native-final-review.json` |
| C32 单点RSS资格与元数据说明 | `compile-cost-c32-transient-storage-r1/memory-sanity/{qualification-independent-review,metadata-erratum}.json` |
| C32 原56点完整重放与独立拒绝 | `compile-cost-c32-transient-storage-r1-coarse-v1/{execution,independent-review}.json`、`compile-cost-c32-transient-storage-r1/independent-policy-decision.json` |
| C32 精确保全、主机/手机恢复与根收口 | `compile-cost-c32-transient-storage-r1/{candidate-preservation,restore-c31,restore-phone-c31,withdrawal-closure}.json` |
| C32 撤回的独立离线核账及其现场证据依赖 | `compile-cost-c32-transient-storage-r1/withdrawal-independent-review.json` |
| 直接尾部复用与sealed visitor的NO-GO | `compile-cost-c32-transient-storage-r1/{next-duplicate-translation-review,next-sealed-value-visitor-review}.json` |
| FEX 三包源码审查与现有合同边界 | `compile-cost-fex-plan-review-20261028-r3.md` |
| C33 有界跨scope复用的备选提案，未应用/构建/测量 | `compile-cost-c33-bounded-workspace-pool-r1/{proposed.patch,review.md,source-proposal-review.json}` |
| raw-first decoder 与 frontier 的源码合同 | `compile-cost-function-raw-first-design-r1/{decoder-contract-review,frontier-contract-review}.md` |
| 当前字节、未消费witness和Function取消层次 | `compile-cost-function-raw-first-design-r1/current-byte-and-cancel-review.md` |
| 六处prefix读取的历史独立提案，未单独应用；迁移已随C34实施 | `compile-cost-function-raw-first-design-r1/{instruction-window.proposed.patch,instruction-window.review.md,root-next-implementation-admission.json}` |
| 形状诊断源码、独立构建准入与实际产物 | `compile-cost-function-raw-shape-diagnostic-r1/{proposal,source-contract-independent-review,root-source-review,build,diagnostic-source}.json` |
| 形状采集 R1 未执行 HOLD 与 R2 最终冻结 | `compile-cost-function-raw-shape-diagnostic-r1/{collection-preexecution-independent-review,collection-preexecution-independent-review-r2,protocol-r2,root-preexecution-admission}.json` |
| 唯一 Function 捕获与独立核账 | `compile-cost-function-raw-shape-diagnostic-r1/{collection,capture-independent-review}.json` |
| 冻结归约、作者清单与独立 raw parser | `compile-cost-function-raw-shape-diagnostic-r1/{reduce.py,reduction-method.md,reduction.json,author-reduction-summary.json,shape-independent-review.json}` |
| 诊断精确保全、原配置恢复及私有目录回收 | `compile-cost-function-raw-shape-diagnostic-r1/{diagnostic-preservation,restore,private-device-cleanup,root-restoration-closure}.json` |
| Block 顺序分配的具体 NO-GO 与覆盖 | `compile-cost-block-sequential-ra-design-r1/review.md` |
| C34各版冻结源码、实际16目标构建和原生身份 | `compile-cost-c34-function-raw-first-r1/{source-r3,build-r3,native-r3}.json`及同目录r1/r2保留记录 |
| C34定向frontier原collector失败及独立实际测试资格 | `compile-cost-c34-function-raw-first-r1/targeted-frontier-r3/{execution,qualification}.json` |
| C34实际generic布局/栈审计与全部reader关闭 | `compile-cost-c34-function-raw-first-r1/native-audit/native-final-review.json` |
| C34完整质量计划、输入和独立执行前终审，非结果 | `compile-cost-c34-function-raw-first-r1/quality/{inputs,frozen-execution-plan,quality-preexecution-independent-review}.json` |
| C34唯一完整54实际执行及root输入/空闲收口 | `compile-cost-c34-function-raw-first-r1/quality/{quality-sequence-execution,supplemental-quality/execution,original22/execution}.json`、`compile-cost-c34-function-raw-first-r1/post-quality-inputs.json` |
| C34完整146个raw的独立质量重放，非性能结论 | `compile-cost-c34-function-raw-first-r1/full-quality-independent-review.json` |
| C34四点MaxRSS原始执行与独立资格，非改善结论 | `compile-cost-c34-function-raw-first-r1/memory-sanity/{execution,qualification-independent-review}.json` |
| C34原56全raw独立重放与REJECT判决 | `compile-cost-c34-function-raw-first-r1-coarse-v1/{execution,independent-review}.json`、`compile-cost-c34-function-raw-first-r1/independent-policy-decision.json` |
| C34原源码/原配置/host16及phone16精确恢复 | `compile-cost-c34-function-raw-first-r1/{restore-c31,restore-phone-c31,withdrawal-closure}.json` |
| C34撤回的独立最终核账与当前资格边界 | `compile-cost-c34-function-raw-first-r1/withdrawal-independent-review.json` |
| C34唯一ELF压缩保全、私有副本回收与原失败保留 | `compile-cost-c34-function-raw-first-r1/{launcher-r3-preservation,private-frontier-preservation-and-cleanup-r2}.json` |
| N3完整表示迁移的有界可行性审查与源快照 | `compile-cost-ir-representation-design-r1/{review.md,snapshot-manifest.json}` |
| C34实际fresh readiness及原56点冻结计划 | `compile-cost-c34-function-raw-first-r1/device-readiness-before-coarse.json`、`compile-cost-c34-function-raw-first-r1-coarse-v1/plan.json` |
| 下一项 Function 整段重建与 FEX 联合资格观测 | `c31-pgo-r1/next-mechanism-review.md` |
| FEX 唯一联合资格观测、原HOLD与独立补证 | `compile-cost-fex-lifetime-qualification-r1/{plan,collect,lifecycle-inspection,qualification-independent-review,post-collection-closure}.json` |
| 外部同批 elapsed 与纯 CPU 边界的预先协议审查 | `compile-cost-external-acceptance-boundary-protocol-review.json` |
| 临时 ELF 与手机副本回收 | `compile-cost-next-stage-source-attribution/temporary-cleanup.json`、`compile-cost-ir-construction-diagnostic-c19-r1/private-device-cleanup.json` |
| FEX 普通编译计时范围复核 | `compile-cost-c15-fex-accounting-review.json` |
| 待定候选的只读审查 | `compile-cost-c10-decode-work-review.json`、`compile-cost-c13-dense-uses-feasibility-review.json` |
| 固定 FEX 新标准统计与有限采样 | `compile-cost-c4-r1-fex-reference/review.json` |

首批压缩件完整读回核对，包含一个精确 ELF 和显式列明的 163 B macOS 归档元数据；元数据没有被当作 ELF。后续构建会复用固定缓存，历史身份以冻结清单、实际 raw 与精确归档共同核对。
