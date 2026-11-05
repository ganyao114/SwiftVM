# Function重复构造整改：原因、实现与测量 · 2026-11-05 UTC

本轮先用冷请求trace确定重复构造的来源，再实现同unit多入口。最终保留的 **A3策略是复用本次已经成功形成的非空source B，并在当前域的ready入口停止继续形成；不再为CALL的afterPC主动扩大推测解码面**。

**重复构造整改已实现，整体性能验收仍保留候选状态。** A3原固定套件有72条runtime和72条compile有效记录，8项×2scope的32项运行门全过、compile为15/16，合计 **47/48**。唯一失败是x87_bench Block compile：0.623230/0.295572917ms＝**2.108549×**，超过原2×门槛。随后预先固定七轮的配对复核独立保存，不能覆盖原三轮失败。实际重复覆盖审计、完整入口合同及设备清理结果已闭合。

## 1. 真正原因与两次被拒尝试

改前已闭合的CoreMark20 Function trace有705次尝试、700个发布owner。成功单元内部没有重复解码；跨请求出现31990次guest指令、同mode去重16101条，重复15889次。原目录只登记请求root，已经存在的source B没有完整可调用入口；新冷入口重新形成后，又进入以前的公共子图。

[定因报告](evidence/continuation-reuse-20261105/cause/report.md)把真实冷root与此前已编译代码、固定ELF只读跳表精确匹配：436个冷root请求匹配此前CALL的afterPC，承载6742次重复；10个入口匹配__vfprintf已编译间接跳表的实际表项，承载7017次重复。前者含435个Function成功及1个Block retry，静态角色可重叠；这不是每次dispatch最后来源的动态trace。18个完全旧覆盖的新root，18/18都位于以前非空原始DecodedBlock起点，合计1323条指令、3590输入节点。不能将这些组的重复量直接当成将来可删的工作量。

| 版本 | 实际发现 | 处理与结论 |
|---|---|---|
| A1：多入口＋推测CALL续点 | 单次普通初测Function去byte约132.143ms、1333尝试/531fallback。独立诊断为1339/534，计数略异；534次全部前端Unsupported，其中521次INT3恰为非root源B首条。失败前缀251941N，不能被普通成功IR计数48065N隐藏。 | [A1审计](evidence/continuation-reuse-20261105/audit-after/fallback-reasons.md)证明大部分回退来自推测续点进入trap/padding，非实际guest执行了INT3。诊断计时不作QoR。 |
| A2：再加非root FC_INT frontier | CoreMark编译减少，但func_tests Function cycles＝1.367144×FEX，原门槛失败；compile从OLD38.516977升至54.583725ms。48次失败含30次formation和18次发射。 | [拒绝收据](evidence/continuation-reuse-20261105/attempt2/rejection-receipt.json)保留72＋72原记录，不以功能全valid替代性能门。 |
| A3：只复用已形成B＋stop-ready | 保留完整入口stub/共享owner，取消CALL后推测队列及continuations/FC_INT特例。CALL仍是真实region出口；续点第一次真实请求才形成，若已由其他真实CFG边形成则正常复用。 | 不扩ISA，不新增局部回滚或重叠流框架；结果与剩余失败见下。 |

[A2 func_tests审计](evidence/continuation-reuse-20261105/audit-final/func-tests-fallback-reasons.md)中，25次0x4071c9和5次0x4082c7是合法glibc LOCK共享入口：JE跳到F0前缀后一字节，而fallthrough执行LOCK CMPXCHG。两个流的原子语义不同，现有非重叠slice不能把它们合成同一条记录。这不是坏码。18次发射Unsupported为12 XEND、4 RDTSC、1 HLT、1 XGETBV；失败前缀21981N/7615条指令，不能据此声称这些指令执行过。

因此没有为本轮缓存复用增加Builder checkpoint、废弃块回滚、重叠字节流表示或supported-ISA预扫描表。固定FEX确有CALL-after queue及EntryPoint机制，但其指令支持和流表示并不等同于当前SVM；直接移植发现范围会把当前尚不能表示的流拉进大图。A3保留原Function范围/预算与实际分配路径，取消的是新加的推测扩展，不是把Function改称Block。原形成/Unsupported仍按既有规则整次失败再Block fallback。

固定FEX的对应链路已按源码核对：`Frontend.cpp:1037–1039`将CALL的afterPC加入目标与EntryPoints；`JIT/JIT.cpp:899–926`登记实际入口、发完整EmitEntryPoint，并让内部边跳过序言；`Core.cpp:908–910`将每个实际entry登记到LookupCache。SVM借鉴的是完整入口及共享owner合同；形成期查询既有ready入口并剪图是本轮自己的策略，不把它写成FEX原有行为，也没有FEX同口径范围trace可以认领全部编译成本差额。

## 2. 最终文件职责与合同

| 现有文件 | A3职责 |
|---|---|
| ir/types.h、opcodes.inc | Known能力ExternalEntries；中立EntryPoint(Imm U64)，Effect::Location，无数值/flags副作用。Operation24B/Block24B/Cell8B不变。 |
| frontend/x86/decode.h/.cpp | 借用当前域唯一目录的只读ReadyEntryLookup；真实目标和线性扫描遇ready则外退，请求本身root仍解码。沿原sparse map/worklist和唯一append-only D buffer，不主动queue CALL续点，无额外ISA支持表。 |
| frontend/x86/build_ir.h/.cpp | 仅对非root、非空原始source B发EntryPoint；预算空B、synthetic exitstub、REP局部label及块内x87融合点不声明。BeginBlock清GPR/XMM/EA/x87缓存，出口完成pending flags并发布dirty state。 |
| backend/arm64/compile.h/.cpp | CompileResult增加真实{pc,word} Entry资源；原主遍历暂记body位置，尾stub建立同unit frame、必要时发布本entry PC，再B回body。内部边/fallthrough跳过序言。入口/Rejected/长分支重发都清entries；root仍offset0，故障表保持整段base0。 |
| execution/runtime.cpp | 目录slot存实际入口，多个slot共享一个NativeCode range owner；Find可同时返回实际entry和原unit用于稳定anchor。first-ready胜出，pending链在绑定时patch，不能通过别名覆盖共享owner。额外入口在原目录登记，不另建PC表。 |
| 原构造/后端/pipeline测试 | 有限合并场景核真实入口frame/PC/flags、内边不重建frame、owner/fault范围和fallback边界，复用原执行器；无新测试平台。 |

没有新源文件、pass、N级证明表、SSA副本或新寄存器bank。B仍原三walk、单u64/N；allocator未新增入口分析表。C仅增加实际输出资源的O(E)stub生成，Runtime是O(E)原目录注册；形成期ready查询是实际新增成本，已计入前端窗口。

入口声明不是跨entry dominance证明。当前前端各source B不引用前B的缓存SSA，后续producer/pass仍须保持该合同。跨unit入场遵循原Complete flags/pins/FP ABI，EntryPoint零编码不取消有限deferred TEST；scope之间的Uniform语义仍中立。Unknown/EarlyOff和Block保持旧默认路径；mutable source仍Function单块external-edge策略，不把可写代码注册成跨源图入口。空slice写自身PC再dispatch，若登记会自命中，所以明确排除。所有ready/pending、link reset和fault范围仍由原owner管理；单映射不预链接，SMC切空目录、撤链后才在冷点退休旧arena。

## 3. A3同批成本、工作量与原门槛

每case/arm三次，中位数；不挑最好样本。compile仅扣真实byte parser，形成/失败/retry/IR/B/RA/编码/复制/发布/释放均保留；与ordinary runtime分开。完整[compile记录](evidence/continuation-reuse-20261105/compile-records.json)和[runtime记录](evidence/continuation-reuse-20261105/runtime-records.json)均72条且全部valid。以下是原2×compile、1.10×time及1.10×user-cycles门，不临时豁免func_tests或x87。

| case | Block ms | Function ms | FEX ms | B/FEX | Fn/FEX | compile门 |
|---|---:|---:|---:|---:|---:|---|
| coremark | 44.905649 | 51.446873 | 48.025260 | 0.935042× | 1.071246× | B PASS / Fn PASS |
| int | 1.818940 | 1.797454 | 1.592552 | 1.142154× | 1.128663× | B PASS / Fn PASS |
| fp | 2.396197 | 2.430219 | 1.732552 | 1.383045× | 1.402682× | B PASS / Fn PASS |
| mem | 2.208391 | 2.210524 | 1.540000 | 1.434020× | 1.435405× | B PASS / Fn PASS |
| branch | 1.762604 | 1.920841 | 1.531719 | 1.150736× | 1.254043× | B PASS / Fn PASS |
| call | 1.900515 | 1.852456 | 1.640937 | 1.158189× | 1.128901× | B PASS / Fn PASS |
| func_tests | 22.387070 | 32.835098 | 30.286667 | 0.739172× | 1.084144× | B PASS / Fn PASS |
| x87_bench | 0.623230 | 0.526250 | 0.295573 | 2.108549× | 1.780441× | **B FAIL** / Fn PASS |

CoreMark Function去byte成本 **81.981761→51.446873ms（−37.25%）**，func_tests Function **38.516977→32.835098ms（−14.75%）**。前后来自不同轮，固定身份一致但不把小幅变化解释成单hunk因果。以下CoreMark分项选各臂total中位的同一次观察，分项不另取中位数；FEX用嵌套scope的exclusive remaining_ticks闭合。

| CoreMark阶段（ms） | A3 Block | A3 Function | 同轮FEX |
|---|---:|---:|---:|
| 形成＋IR，扣纯byte | 11.979617 | 16.862134 | 22.940313 |
| IR优化 | 9.088025 | 12.056461 | 6.240938 |
| 分配＋编码（含RA） | 14.056878 | 16.892473 | 14.659583 |
| 传输＋发布/链接 | 7.591213 | 4.685397 | 3.146823 |
| 各自释放窗口 | 0.422448 | 0.340209 | 0.351094 |
| 其余窗口内成本 | 1.767468 | 0.610199 | 0.686510 |
| 合计 | 44.905649 | 51.446873 | 48.025260 |

A3 Function发布/链接4.685397ms仍高于OLD4.215056ms，多入口登记有成本，不能只展示前端减少。SVM释放是native arena/cache/保留草稿的实际owner释放；FEX仍是原logical working-buffer释放窗口，不能把两者当相同析构阶段。

| CoreMark20记录计数 | OLD Function | A3 Function |
|---|---:|---:|
| compile attempts / Function / Block retry | 705 /700 /5 | 686 /681 /5 |
| 发布unit owner | 700 | 681 |
| record emitted guest instructions /bytes | 32159 /136270 | 16584 /63906 |
| record IR N /arena words（不含失败prefix） | 87595 /153728 | 50591 /84624 |
| native bytes | 646764 | 393248 |
| spill slots /max frame B | 42 /64 | 31 /64 |
| peak ready PC keys /directory slots | 700 /8192 | 3438 /32768 |
| fault sites /link relocation rows | 12729 /1787 | 6762 /1216 |

owner变少但可调用PC键变多，目录容量8192→32768（32B/slot，约额外0.75MiB）；原native slab仍一个双别名、虚拟2MiB。以上是compile观测的工作/资源，不是动态热度，也不是同mode去重审计。emitted可能含失败发射prefix，而IR N/words明确不含；不得用它冒称所有实际出生工作。A3initial-cost仅是初测/工作核对，正式成本取完整三轮记录。

| case | scope | time ms | time/FEX | user cycles/FEX | instructions/FEX（诊断） | 运行门 |
|---|---|---:|---:|---:|---:|---|
| coremark | Block | 16111.000 | 1.071709× | 1.070386× | 1.037896× | PASS |
| coremark | Function | 15633.000 | 1.039912× | 1.039390× | 1.009988× | PASS |
| int | Block | 301.029 | 0.948069× | 0.991302× | 1.224227× | PASS |
| int | Function | 303.149 | 0.954746× | 0.991705× | 1.266139× | PASS |
| fp | Block | 252.263 | 0.955314× | 0.982424× | 0.990179× | PASS |
| fp | Function | 250.033 | 0.946869× | 0.984902× | 1.005873× | PASS |
| mem | Block | 2149.787 | 1.050011× | 1.057090× | 1.332664× | PASS |
| mem | Function | 2167.412 | 1.058620× | 1.066047× | 1.366208× | PASS |
| branch | Block | 261.421 | 0.980802× | 1.003677× | 1.137676× | PASS |
| branch | Function | 259.579 | 0.973891× | 1.010245× | 1.157627× | PASS |
| call | Block | 477.202 | 1.039253× | 1.078760× | 1.240629× | PASS |
| call | Function | 476.507 | 1.037739× | 1.073915× | 1.249896× | PASS |
| func_tests | Block | 108.749 | 0.785333× | 0.806079× | 0.806901× | PASS |
| func_tests | Function | 120.647 | 0.871255× | 0.961663× | 1.041527× | PASS |
| x87_bench | Block | 555.797 | 0.678043× | 0.660933× | 1.272628× | PASS |
| x87_bench | Function | 553.002 | 0.674633× | 0.657285× | 1.269776× | PASS |

**运行代价必须保留：Core Function OLD15.282s→A3 15.633s，增加约2.30%；同轮FEX15.033s，所以仍在门内，但不能说完全无回退。** 当前Core Function user cycles17,982,268,544、instructions62,632,712,714，同轮FEX分别17,300,794,738和62,013,310,568。额外入口序言后有回body的一条B，目录键数及布局也改变；没有逐入口运行计数，不能把整轮变化直接归因某一条指令。各输入分别判定，不计算跨workload平均分。

## 4. x87失败与独立七轮复核

原三轮Block为0.597501/0.658179/0.623230ms，中位0.623230；FEX为0.295572917/0.306354167/0.293697917ms，中位0.295572917，故原compile门确实失败。工作完全相同：3 attempts、30 guest、59N、106W、2364 native bytes、spill/frame0。同total中位的backend字段OLD0.104270→A3 0.206251ms是主要增加项，但仅凭此不能判定C hunk因果回退或宣布是噪声。

按[事先固定计划](evidence/continuation-reuse-20261105/x87-paired-confirmation/plan.json)旋转旧固定runner/A3/FEX顺序，各七次，无追加或optional stopping；[原记录](evidence/continuation-reuse-20261105/x87-paired-confirmation/records.json)及[独立汇总](evidence/continuation-reuse-20261105/x87-paired-confirmation/summary.json)保留：

| x87 Block七轮配对中位 | OLD | A3 | FEX |
|---|---:|---:|---:|
| 去byte compile ms | 0.641197 | 0.594011 | 0.302135417 |
| /同组FEX | 2.122217× | 1.966042× | 1.000000× |

A3/OLD＝0.926410，反对把原三轮单个backend增项直接当作代码因果回退；同时OLD在这组也超过2×。这份独立复核不能替换原72条compile数据、不能把原47/48改写成全过，更不能宣布小案例已稳定满足2×。原固定套件仍保留candidate/failed-gate状态；七轮复核作为单独证据，不重写原验收结果。

## 5. 身份、验证与实际重复覆盖

A3[118文件清单](evidence/continuation-reuse-20261105/source-manifest.json)摘要 `a78fdec20b3de2f00fe47bb1dbfa67c6664c0efa7cd1b7f30658edd8f1ce1e52`，普通runner SHA `13411690d78fa72df6fc84c61004bfe7c36a2060cc3a9a16cb22dcbfca9d77eb`。base commit记录为`13ffa48f1d984cd7d600b5c93601301462b13570`后的WIP；本轮没有提交或push。

固定FEX源码a156752fae3a4f65fefb1ed95af980bcdfac43d7及原Android patch/config不变；ordinary loader SHA `8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175`，byte-probe loader另存，不混作ordinary runtime。完整guest/FEX/simpleperf/配置身份见[inputs](evidence/continuation-reuse-20261105/inputs.json)：CoreMark `d92d8d77…6c0bcbba`，bench-suite `df60aa3f…9240a6d`，func_tests `4740aad1…3266ae60`，x87 `60350fb2…91bfabb`。

红魔bf1359ff，CPU4/taskset10；policy min/max614400/1171200kHz，runtime前后即时值均1171200、温度raw260→261，未锁频或改服务。每样本fresh launcher，无AOT/object cache，OS page cache保留；PMU是整个子进程user事件，含cold JIT和相同wrapper，外部FEXServer不在该树。CoreMark运行100000迭代用guest loop timer且有有效时长；compile20迭代只按exit/CRC4983判输入有效，不当官方CoreMark成绩。其他time为整子进程elapsed。

[Host原5组合同](evidence/continuation-reuse-20261105/host-ctest.log)通过；[Android18项smoke](evidence/continuation-reuse-20261105/smoke-summary.json)为backend/pipeline＋8workloads双scope，全部valid；[两scope模式/SMC probe](evidence/continuation-reuse-20261105/mode-probe-summary.json)通过。CoreMark有CRC，int/fp/mem/branch/call有原checksum，func_tests保原stdout/exit101。x87仍为继承的exit-only oracle，已有ext80/控制字测试不为该bench补造数值checksum，也不豁免它的性能门。

[A3唯一覆盖审计](evidence/continuation-reuse-20261105/audit-accepted-candidate/report.md)及[结构化数据](evidence/continuation-reuse-20261105/audit-accepted-candidate/analysis.json)已冻结：既有一次CoreMark20诊断exit0/CRC4983，时间不作QoR，不另跑或与ordinary计时拼接。

| 实际诊断工作 | OLD | A3 |
|---|---:|---:|
| 尝试 /发布owner | 705 /700 | 686 /681 |
| 成功guest指令出现 /同mode唯一 /重复 | 31990 /16101 /15889 | 16415 /16224 /191 |
| 成功guest字节出现 /union | 135659 /62142 | 63295 /62642 |
| 成功前端N /失败prefix N | 87595 /251 | 50591 /265 |
| 含失败的全部实际出生N | 87846 | 50856 |

重复出现15889→191，减少15698次（98.80%）；全部实际出生87846→50856，减少36990N（42.11%）。A3为676个Function＋5个Block owner；5次前端失败仍为3 XGETBV、2 SHRD，失败prefix169条指令/265N，formation/B/C失败为0。因此ordinary emitted16584＝成功16415＋失败prefix169；不能把record成功IR计数误叫完整birth数。

实际2757个extra＋681个root＝3438个ready PC，本次每个extra都接受并与非空原始source-B起点、完整unit range owner逐项对应，无empty B或exitstub，没有重复offer覆盖旧owner。stop-ready为186次positive callback，涉及136个mode/PC及124次attempt，其中169次指向extra、17次root，均匹配更早ready登记；这些事件不是被剪掉后代数或节省指令数。

两份覆盖审计实际均mode0，不把跨FP mode合理复制算作冗余；A3唯一指令覆盖比OLD多123条，这个差异保留。格式化时钟等不同冷路径可能是因素，但没有逐动态来源证明，不能把全部总work差认领为单一复用优化。剩余191次重复也没有宣称消失。该次同时留存峰值181200B＝IR153712＋words22368＋links1024＋entries4096，计入Entry草稿容量；这是独立诊断峰值，不替代一般留存门或ordinary观测。失败prefix词数未提供，保持缺失，不额外全图扫描或补猜。A2曾得56次重复的结果仅属被拒A2，不与此审计拼接。

[手机清理回执](evidence/continuation-reuse-20261105/phone-cleanup-receipt.json)已完成：删除本线私有namespace 59,544,576B，df实际空闲增加60,801,024B，目录剩余0，无活动进程、打开文件或清理错误。代码位于`/Volumes/Data2/SwiftVM-submit-20261104`，完整源码、压缩runner和诊断副本保留在Data2的`reconstruction-20261102/continuation-reuse-20261105`；固定构建缓存继续复用。重复构造修复已经落地，原固定套件的x87 Block compile门仍未由三轮数据通过，未宣布整个编译性能目标稳定达标。原定dynamic ELF、全ISA、异步信号等范围未因本轮额外扩大；也没有以收窄工作负载、删除失败样本或放宽原门槛换取通过。

A1/A2原始日志、A2拒绝收据、原三轮A3失败和七轮复核均保留；[A2代码合同审查](evidence/continuation-reuse-20261105/final-code-review.md)是A2的正确性审查，不替代A3性能资格。[上轮结果文档](compile-owner-and-publication-optimization-2026-11-05.md)保留OLD身份及窗口，历史结果没有回写。

[完整汇总](evidence/continuation-reuse-20261105/summary.json)、[同批exclusive成本](evidence/continuation-reuse-20261105/compile-breakdown.json)、[普通原始输出](evidence/continuation-reuse-20261105/raw-output.jsonl)、[本轮11源文件补丁](evidence/continuation-reuse-20261105/changes-from-buffer-reuse.patch)、[身份回执](evidence/continuation-reuse-20261105/receipt.json)。所有覆盖/回退日志计时均排除于QoR，原有效样本均保留。
