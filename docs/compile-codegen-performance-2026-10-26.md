# Compile 与生成代码优化执行记录

本文按先后记录两轮任务。前一轮根据固定 FEX 的真实行为优化 compile 耗时，再按生成代码机制差距优化体积和质量，已完成下述 r1–r9 的有限验证。后续消除逐操作 NaN guard、使生成代码数量接近 FEX 的目标，已在本文覆盖的普通 FP 机制和固定基准中完成验证：Block、Function 正常回边各 51 条，FEX 参照 46 条，原 guard 与快照清零。两种分配模式分别验收；下文各阶段保留当时状态，最终测量见文末。

前一轮终点r9直接对原始9bb基线：CoreMark的Function/Block编译请求累计耗时分别减少约6.84%/5.88%，fp1分别减少约9.00%/9.29%。r9相对相邻已接受r7的FP发射缓冲各少32 B，标准PMU指令斜率减少约2.86%/3.06%，当时cycles和elapsed尚未证明稳定改善。该轮生产改动净增7行，没有新增pass或opcode。以下历史结果不改贴为后续二进制；不宣称FEX完整语义等价、精确纯编译差距或整个中间流程重构完成。

## 对照和约束

- SVM 起点为 public-audit-v2：代码摘要6049ce553edc88a518a1ca551f4d0ad8d3ec360b2a424a5457a5301a7b3ebb24，launcher9bb79d52…。原始代码/Git/WIP和压缩产物索引保留；固定输入缓存另留一个独立inode的baseline ELF，防止后续链接覆写基线。
- 实测FEX固定a156752fae3a4f65fefb1ed95af980bcdfac43d7及Android补丁4ade908f…。现存v5源码和build已逐文件校验迁到Data2固定缓存，原逻辑路径保留软链，Git和WIP未变；没有创建替代clone或展开整套产物bank。
- 测量复用现有SVM_PROF粗计时及FEX_PROFILESTATS标准统计，不使用启动截距估计纯compile，不加guest probe或新编译pass。源码机制报告为active-results/perf-compile-r1-fex-pipeline-review.json，新的主机解码器为tools/redmagic/compile_stats.py。
- SVM translate_ns与FEX AccumulatedJITTime均属含查询/部分锁/发布的编译请求累计耗时；入口和初始化范围存在明确差别，完整边界见机制报告。两者不是排除所有等待的CPU时间，也不提供相同的miss/unit计数。不要把SVM减wait后的值与未经扣除的FEX值冒充同口径纯miss时间。
- FEX统计是CNTVCT ticks，按CNTFRQ换算；本轮现有scalar-publication永久用例实读19200000 Hz。该值不是CPU频率或PMUcycles。标准stats退出会清TID并摘Head，FD最终残存槽不自动代表任意多线程进程的完整累计；必须保留live/final、PID/starttime及单线程/无复用证据。
- 普通PMU不传任何profiling变量。compile粗计时仅传SVM_PROF=1，必须不传SVM_PROF2：其当前语义为变量存在即开启，设置0同样开启。独立audit、shape和host dump在compile比较中关闭。

## 第一批：删除无收益操作

| 文件 | 改动 | 保持的契约 |
| --- | --- | --- |
| source/runtime/ir/ir_meta.cpp | 保留GetValueSizePow合法类型检查，用整数移位替换pow，删除cmath include；对应FEX纯整数OpSizeToSize机制 | 所有合法类型宽度相同，非法枚举/VOID仍走原PANIC |
| source/runtime/backend/arm64/compile/publication_windows.cpp | published_versions为空时跳过ForEachValue | candidate/deadwrite/coalesced及PrepareValueVersions状态和顺序不变，不增加分析表 |

第一批代码摘要为42758ec0bea083d62d2f42e12c5cfc14bc96d72f20223ad330b5f17b98efa395，launcher a882768d…。没有新增compiler源文件、pass、opcode或运行开关。十四个既有native目标一次增量构建成功，耗时5.667 s；这是工程构建时间，不是JIT compile性能。

集中回归：完整IR fast/slow和包含首写前/空publication/复用后、Function真实targetprep、现有scalar-publication及计数器频率读取的选择集均exit0，无failure/error。部署初次在root目录写权限阶段停止，尚未运行程序；改为既有stage后root原子安装。之后一个baseline计时因SVM_PROF2=0仍开启细粒度计时被校验拒绝，原始点保留；既有测试不重跑，新的coarse-v2批次不传该变量。

CoreMark2000三轮配对结果（候选/baseline）：

| 模式 | translate比值中位数 | 三轮范围 | 当前结论 |
| --- | ---: | --- | --- |
| Function | 0.994206 | 0.969171–1.012515 | 波动跨1，未证明实质提速 |
| Block | 0.993483 | 0.960325–1.011561 | 波动跨1，未证明实质提速 |

所有12点保留，条件fingerprint/governor/min/max/charging/USB一致，目标/CRC及输入前后身份通过。Function每轮两臂的IR/unit/host-size相同；Block因格式化等执行路径变化存在unit/IR数量差异，不把聚合数强行归一成相同动态单元集合。记录位于active-results/perf-compile-r1-phone/coarse-v2。没有追加样本追求有利比值。

实际反汇编确认GetValueSizeByte正常路径已删除ldexp/浮点转换，但仍保留96 B固定帧和canary：非法类型的PANIC格式化仍在函数正文。下一步先看默认配置采样热点，再决定是否将冷失败路径外置；不能关闭必要检查或仅凭API调用数认定重复构图（sealed SSA已有缓存）。

## 默认路径热点与差距的含义

两次既有simpleperf采样没有传PROF/PROF2、shape或dump，关闭额外plan audit，分别使用Function和Block请求。Function有4325个sample、380个错误callchain；Block有3960个sample、175个错误callchain。采样覆盖整个进程，包括guest执行和内核；以下是可识别C++编译调用链的inclusive占比，不能当成纯编译时间百分比，也不能把父子节点相加。

| 调用链 | Function，占整个进程采样 | Block，占整个进程采样 |
| --- | ---: | ---: |
| 输入描述Describe（包含BuildFixed） | 8.51% | 8.98% |
| 其中BuildFixedGPRInputs | 3.79% | 3.51% |
| allocator入口AllocateFunction/Block | 6.91% | 5.99% |
| 其中LinearScan::AllocateRegisters | 5.50% | 4.01% |
| 分配后CompleteARM64Allocation | 8.94% | 9.60% |

Storage::AllocateFunction/Block的24.33%/25.38%已经包含Describe、allocator入口和Complete，不能另加一次Complete。既有粗regalloc计时在Complete前结束，后者又进入粗codegen计时；不能把粗codegen都解释为assembler发射。

这些结果说明，结构重构后仍有实质性输入描述、物理约束和目标处理成本，Block同样负担这些工作。不能把全部差距归给Function的跨块分配；固定FEX即使开启Multiblock，其RA仍按CodeBlock重置，也不能直接视为SVM Function allocator的对等物。FEX默认pass、builder和目标lowering的职责分配需要整体比较，pass数量本身不能证明哪个阶段更快。

全局Scudo、原子操作以及细小访问器的采样只提供候选，尚未把整个差距逐项归因。未知JIT PC不据此认定为某个flags/FP热点。原始采样位于active-results/perf-compile-r1-default-profiles。

## 第二批与r4检查点：检查保留，冷失败路径外置

plain ASSERT仍只求值一次、保留短路与原std::logic_error文本；只把条件文本的格式化移到base/logging.cpp中的一个cold/noinline helper。GetValueSizePow的非法类型分支同样外置。ASSERT_MSG不变，没有关闭检查，也没有新增pass、分析表或开关。

实际ELF反汇编确认GetValueSizeByte从232 B降到72 B，正常路径没有栈访问/canary；错误分支仍调用cold helper。PublicIndex的独立弱符号不再需要，符合常用IR访问器重新内联的结果。r2原生ELF的text总量从5437764 B降到5130724 B，减少307040 B；这是编译器自身的体积，不能冒充guest生成代码的缩减。

新增两个永久负向契约用例，覆盖14种合法宽度、3种非法宽度、ASSERT条件求值/短路及精确异常消息。r2的完整IR fast/slow及选定发布契约均通过，共256个JUnit testcase元素、456763项断言；另6个HIR/2个entry运行通过。一次漏传HIR mode的入口以2退出，源码确认在fixture初始化前返回，原记录保留；补齐参数后不重复已经通过的前三项。

r2相对r1的三轮compile中位数改善为Function约3.45%、Block约2.15%，但Function范围跨1。随后尝试把GetValues临时集合与既有operand visit合并：r3相对r2的Function中位数1.010590、Block0.989284，均跨1，未证明收益。该两文件增量已精确回退；不因为少一次遍历就预记为提速。CSR反向use索引也因归因和复杂度证据不足暂缓。

r4仅在r2上保留下一节的packed64 guard。source摘要39871c023421663242c8778a7c9b4465b8ef3f6aac04ec7c36055195959f2ebe，launcher c09de2e31ea2055fa15eb0ebd7b273dce3e02fe6252c3c81bf183ebb91492b6d。相对本轮原始public-audit-v2，该检查点12点CoreMark2000的编译请求累计耗时为：

| 模式 | 候选/原始baseline，三轮配对中位数 | 三轮范围 | baseline/candidate耗时中位数 |
| --- | ---: | --- | --- |
| Function | 0.956621 | 0.941477–0.960672 | 1333.719 / 1275.864 ms |
| Block | 0.960224 | 0.959933–0.976612 | 984.417 / 946.653 ms |

三个配对均更快，这是当前固定负载的小样本观察，不代表已经填平FEX差距。Function两对全部七项工作量计数一致，另一对差1个function/decoded block、15 IR；Block一对一致、另外两对有动态unit/IR差异。全量保留，不按IR数强行归一。最终数据位于active-results/perf-compile-codegen-r4-phone；独立报告perf-compile-codegen-r4-independent-review.json重算全部比例、483个源文件及14个ELF，核对12点完整argv/env/身份/CRC和24个PMU事件，running均等于enabled。端点当前频率有变化，未声称全程锁定频率；不把n=3写成普遍性能保证。

固定FEX标准stats的三个单槽观测为55.717760、54.388125、54.521666 ms，来自19200000 Hz的CNTVCT ticks。r0辅助metadata缺失导致collector78，原始counter保留且不重跑；r1/r2在明确记录children接口不可用后collector0。live/final均看到同一TID槽，但未证明完整线程历史，不能签完整进程JIT累计或精确的SVM/FEX pure-miss倍数。报告为active-results/perf-compile-r1-fex-stats/standard-stats-three-observations-review.json。

## 第五批：复用已有指令索引，删除重复哈希表

r5只修改source/runtime/backend/arm64/compile/constraints.cpp：PrepareBlock原来为preservegpr遍历全块、给每条IR向unordered_map插入ID和指针，最后仅用它查找FirstClobber给出的split指令。该块已有BlockAnalysisIndex，现在直接调用uses.FindInstruction，并保留非空ASSERT。索引与IR同属当前不可变块，BeginBlock会重置投影；稠密ID和稀疏回退均保留正确查找，没有增加新表、pass或分析状态。不可达的缺失ID分支改为明确ASSERT，不能将其写成异常类型完全不变。

历史heap分配流与原始ELF内联位置核对确认，这张表的构造确实触发node/bucket分配；例如Function CoreMark记录到112482次请求、4977264 B累计请求量。此数属于历史采样，既不是r5实测分配量，也不是同时驻留峰值。机制与符号证据分别见active-results/perf-compile-r5-ir-allocation-mechanism-review.json及perf-compile-r5-map-allocation-site-symbol-qualification.json。

r5的source摘要81207cb8b817b1457760833ea0e11fb49d5e315a7a950073ed34fcfda7ddef97，launcher9730849ee1d43ed18e6fb70b16da0953bad92693964473ca29e8d87a80b581c5。十四个native目标集中增量构建成功；r5完整IR fast/slow和GPR保存/跨块发布选择集共4次、294个JUnit testcase元素、524988项断言，0失败。另一个r4基线用例独立保留，未计入r5回归总数。

| 模式 | r5/r4 compile配对比值中位数 | 三轮范围 | 结论 |
| --- | ---: | --- | --- |
| Function | 0.988278 | 0.972613–0.992727 | 本批三对均更快，约减少1.17% |
| Block | 0.979200 | 0.969848–1.009050 | 范围跨1，未证明稳定提速 |

12点条件、身份、输出和PMU组有效。Function第三对有1个unit/15 IR差异，Block三对工作量均有变化；不逐IR归一，也不把本批比例与r4相对9bb的比例相乘作为新的直接测量。原始记录位于active-results/perf-compile-r5-phone，独立复核为perf-compile-r5-independent-review.json。

固定FEX的标准beforeopt/afteropt诊断还观察到CoreMark的282对完整IR帧：列出的代码节点206031→109324，显式flags名称子集30774→9045；这些端点合并了多个pass和RA，不能拆成某个pass的独立收益。FEX节点总数与SVM的80465 IR不属同语义计数，不能据此算每IR效率；但它也不支持“全部compile差距只是SVM IR更多”的解释。重复入口RIP不自动代表可消除的重复编译。证据见active-results/perf-compile-r5-fex-ir-observation/summary.json；FP诊断的晚期/proc采集失败仍保留为collector75，未冒充完整成功测量。

## 第六批：删除可证明冗余的别名去重集合

r6仍仅修改constraints.cpp：preserve-family已维护family列表，却同时用unordered_set去重。扩展边严格是单输入BitCast；从GetHostGPR根可达的图没有汇合或环，因此合法IR上seen.insert恒成功。删除该集合，保留use.count==1和arg0.Def()==definition的always-on断言。没有扩大别名规则、减少宽度/跨块/控制流检查或增加新状态。历史FunctionCore/BlockCore采样中，该集合分别触发9522/6857次分配、195216/139752 B累计请求；这些历史站点计数不当作r6实测分配差或峰值。

r6的source摘要f48a433d1e7d4701104fa1f8cd99b051b1ba1df5b674892e1b92d9d68fc0761f，launcher516a377ea7f6dc2b11400e492953ad39be667b38b5b6fabe58b4508224227f79。十四个native目标集中增量构建成功。既有IR fast/slow和GPR保存选择集4次、294个JUnit testcase元素、524988项断言均通过；既有用例已覆盖嵌套别名、实际clobber、spill压力、terminal/helper/control拒绝及Function loop/join，无需新增镜像实现的测试。

| 模式 | r6/r5 compile配对比值中位数 | 三轮范围 |
| --- | ---: | --- |
| Function | 1.007326 | 0.991366–1.009499 |
| Block | 1.000566 | 0.984340–1.014637 |

两种模式均跨1，没有稳定耗时收益。保留此项的理由是删除冗余容器和分配、简化代码，不将其计为compile提速；也不追加样本追求有利数字。原始记录为active-results/perf-compile-r6-phone，机制与实际差分审计为perf-compile-r6-redundant-preserve-family-set-review.json和perf-compile-r6-independent-delta-coverage-review.json。perf-compile-r6-independent-review.json另核对全部483源文件、118条退役路径、14个ELF、4份XML、12点原始计数及172个原始非JSON文件，issues为空。

## 第七批：decode直接转发同型FPR值

固定FEX的OpcodeDispatcher.h中，LoadRegCache命中后返回已有RegCache.Value，局部宽度处理单列。SVM的IrBuilder::LoadUniform已有guest-state字节转发证明，却在Alias命中时无条件AppendRawTyped<BitCast>，即使输入视图和定义本身都已是相同V128。这让后续pass、使用链、分配和释放重复处理恒等节点。

r7在guest_state.cpp的Alias且IsFloatValueType路径复用现有FoldSameTypeCast(...,FlagsIntent::None())：只有目标类型、输入视图、定义ReturnType三者完全相同才直接返回原Value。Extract、窄/宽视图、同宽不同类型和整数producer身份均保留原处理；未改变原始BitCast API的未知flags契约。旧append_fold不执行AfterAppend/事实更新，跳过恒等节点没有跳掉原有观察事件；真正consumer照常登记普通/HIR uses。不增加pass、IR opcode、分析表、开关或新compiler源文件。code_serial.h将显式缓存版本升为43。

source摘要8982ef6b99ea51554b4bae0f781ed9f57cd265f3e6e84650c25a2d14f2a2385d，launcher294a5ea4aa25f7c7861997ff03be06c54fa80b0025cea1e3a9a1daabe57e76c8。十四个native目标构建成功；IR fast/slow、FPR/index选择集和四组AVX literal共7份JUnit、279行、521071项断言，0失败。四组AVX明确启用JIT/AVX、关闭AFP、启用coldpath，覆盖Function×XPOOL四配置。既有cache-version程序208项检查通过，实际拒绝了其列出的9个历史版本；没有运行旧42的动态拒绝分支，不冒充该项已有运行证据。

| 负载 | 模式 | r7/r6 compile配对中位数 | 三轮范围 | 结论 |
| --- | --- | ---: | --- | --- |
| CoreMark2000 | Function | 0.995153 | 0.992902–1.013701 | 跨1，未证明稳定改善 |
| CoreMark2000 | Block | 1.008568 | 0.988085–1.039557 | 跨1，未证明稳定改善 |
| fp1 | Function | 0.939031 | 0.921907–0.959927 | 三对均更快，约减少6.10% |
| fp1 | Block | 0.958664 | 0.948038–0.969908 | 三对均更快，约减少4.13% |

FP的独立12点在CoreMark批次完成后预定，目标是检查恒等FPR转发集中出现的负载；两组分开报告，不合并为普遍收益、不逐IR归一。FP同模式每对请求数、decoded blocks和host bytes一致：Function的累计IR计数2216→1877、HIR池容量计数341888→304256 B，Block为2467→2228。池计数不是RSS或全进程峰值。Function的translate中位数31.648→29.718 ms，Block为27.188→25.775 ms。CoreMark中Function只减少约739个IR，未宣称该低占比改动同样取得上述提速。

新采集的R6/R7诊断中，Function自回边400ff0最终IR为129→70，Block为174→115，均消除59个BitCast；两模式metadata合并不同，不互相按IR数比较。最终IR帧按首个空行截断，不能把下一次pre-IR混入。Block42单元/8196 B、Function6单元/8800 B的48个发射缓冲区逐字节完全相同；这里没有branch归一、没有把code-hash字符串当字节证明，也没有运行速度或生成代码体积收益。这一批的收益是更少编译工作。诊断没有独立guest SHA/设备快照门，固定bank、launcher前后SHA与输出oracle的证据边界独立记录，不把它用于计时结论。

完整原值为active-results/perf-compile-r7-phone、perf-compile-r7-fp-coarse和perf-compile-r7-diagnostics；perf-compile-r7-independent-review.json重算全部24个coarse点及48个缓冲，issues为空。审查R7时已开始R8 WIP，两条变化源码用R8-before原字节重建；14个R7 ELF在允许覆盖固定构建缓存前已实际读完并确认稳定，不把R8工作树或ELF改贴为R7。

## 第八批：Function frontier扫描候选未通过净收益验收

默认Function采样中的DecodedEnd是一个真实热点：FindSplit先扫描每个块的AdvancePC，再排除target<=start的块；选中owner后又为provenance重扫一次。候选仅提前起点判断，并用本次局部owner_end填provenance，包括歧义分支；没有新增缓存或索引。没有直接使用decoder.pc或node_size：终结后AdvancePC可能不再追加、失败lowering可能已推进pc，已有范围准备还使用max(sum,1)，这些都与frontier合同不同。固定FEX在decode维护Size/InstSize并定位已解码块的机制不能直接套用。源审计见perf-compile-r8-function-decode-frontier-review.json。

候选source62d144a1286c34259730ea553eddbbc110e9d194c6f2e47ffbfb0d95d58b2151、launcher0bfe58d6…。新增一个有实际覆盖价值的负向用例：两个结束地址不同的重叠owner，仍拒绝歧义并保存首owner边界和依赖，同时检查重复拒绝及起点/终点边界。frontier fast/slow各9个case、90项断言通过；没有重跑未改动的IR/FP矩阵。

| 首批12点CoreMark | 候选/r7中位数 | 三轮范围 |
| --- | ---: | --- |
| Function | 0.979990 | 0.971106–0.987455 |
| Block对照 | 1.018542 | 1.000980–1.030277 |

Block真实路径不执行Frontier，但其三对均较慢；未改阶段也同向变化，端点当前频率未固定，工作量每对亦有细小变化。不能直接归因于代码、频率或某个单一因素，也不能忽略该负控。针对这一明确疑问，只追加一次预定的六点Block确认，互补首批的臂顺序；确认比值1.011321、1.001174、0.988754，保留原三对后的六对中位数为1.006247，范围0.988754–1.030277。没有第三批或删除原点。确认记录的12点文字模板在结束后纠正为六点范围，原文字保留在scope_template_preserved，计划顺序/原始样本未变。

最终不接受这项生产优化：Function观察改善不足以在当前Block疑问下证明净收益。function_decode_frontier.cpp已逐字恢复r7，保留新增负测；重建后launcher与r7的294a5ea4…逐字节相同，另13个native目标中仅swift_test因新增用例变化。恢复后两次frontier回归仍各9个case/90项断言通过。最终全源摘要d2c8ee5ed28743ac640e9cab26e070ea43aa2c7ef07d7b505eaf952231c37ea9，非测试源码全部与已接受r7相同；测试专用快照不重新标记为新的性能测量。

原候选、18点原始结果、明确决策及失败前保护均保留：perf-compile-r8-phone、perf-compile-r8-block-confirmation、perf-compile-r8-retention-decision.json、perf-compile-r8-rejected-launcher-preservation.json。恢复回归的清理前检查曾将Magisk日志参数中的历史命令误认为活动测试，未删除文件即停止；随后按/proc实际exe核对并完成清理，不重跑已通过测试、不动系统日志进程。原失败execution保留，后续closure.json单独记录解决过程。

## 第九批：packed比较直接写目标，完成当前二进制的原始基线对照

固定FEX的真实FP post-RA路径仍保留四组独立VAnd/VAndn/VOr，不能声称它在此处自动融合BSL。其普通packed EQ/LT/GT/LE等比较直接写GetVReg(Node)分配的目标；SVM的EmitVecFCmpMask却总是先写compare临时，再用ORR复制到结果。这正是实际热路径的四处mask copy。FEX另有VBSL通用操作，可按别名选BSL/BIF/BIT，但本轮不为此新增opcode或匹配规则。源证据分别为perf-compile-r9-fex-bitselect-source-review.json与perf-compile-r9-fex-packed-compare-source-review.json，未采集FEX最终host bytes。

r9仅将packed的compare直接设为已分配result，删除尾部复制；scalar保持临时compare、复制原left高位及INS。predicate5/7在写result前先将两次原输入比较结果写到既有独立临时，其他基础predicate为单条读后写，invert仍原地MVN，因此允许结果与已死亡source同寄存器。NaN语义、NZCV、保守scratch预算、分配与publication合同不变。显式缓存版本43→44，没有新增pass、opcode、分析状态、索引表或开关。

当前完整源摘要7580131be329777eb9cc989238eb04d073e92163d97e532a414248511d9e7736，launcher f13b7f0ba513bfa76b9f23c96b6684e5bd5f96f8d9a31b7b9e9a148d65084878。十四个native目标构建成功，已有7组JUnit共283行、583503项断言，0failure/error/skip。四组Function×XPOOL均实际执行FP literal及完整32 AVX比较谓词的两个case，每组23307项断言；all32用例的4928条literal reference分别验证JIT/interpreter、packed/scalar、32/64位、寄存器/内存与128/256位结果。旧FP literal子集没有覆盖IR predicate5，本轮没有用旧子集冒充该项证据。已有aot-reject门106项检查通过，实际覆盖当前44的前一版本43拒绝及拒绝后的零安装；不把它写成磁盘cache旧43运行门。

这次24个coarse点直接比较当前r9与本轮原始6049ce…/9bb，不相乘历史比例，也不将累计改善归于r9单改：

| 负载 | 模式 | 当前/原始compile配对中位数 | 三轮范围 |
| --- | --- | ---: | --- |
| CoreMark2000 | Function | 0.931565 | 0.926577–0.935023 |
| CoreMark2000 | Block | 0.941208 | 0.934268–0.952746 |
| fp1 | Function | 0.910026 | 0.878174–0.910028 |
| fp1 | Block | 0.907102 | 0.872294–0.910973 |

四组各三对均更快。含义仍是既有编译请求累计耗时，不是pure miss或CPU-only；逐对工作量计数原样保留，不按IR归一。perf-codegen-r9-independent-compile24-regression-review.json核对当前483源文件、118条退役路径、14个完整ELF、24点312个raw文件、48个PMU事件和所有条件/输出/PROF2缺席，未发现问题。

新采集的r7/r9发射对照为：

| 模式 | 全部单元字节 | 选定单元 | 有限值正常回边路径指令数 |
| --- | --- | --- | --- |
| Block，42单元 | 8196→8164 B | 400ff0：1000→984 B；400f7f：1588→1572 B | 131→127 |
| Function，6单元 | 8800→8768 B | 400f10：4128→4096 B，含两条算术路径 | 140→136 |

96条host记录的长度和FNV独立重算通过；48个对照单元中仅3个变化：共16个compare改目标、16个随后copy被删，80个PC相对分支重编码后目标一致，其余非PC差异为0，ADR无额外变化。14条FP运算、56条NaN guard、五处source snapshot、三组常量加载/屏障及Function的八次SSA spill访问保持。IR不变：去除host/exec-map/size日志后，两臂剩余诊断文本逐字相同。新诊断包括guest/launcher前后SHA和六项设备快照；缓冲仍在发射阶段，不是链接/SMC后最终内存。资格报告为perf-codegen-r9-qualification-independent.json。

随后完成独立标准36点PMU：相邻已接受r7 294为baseline、r9 f13为candidate、固定FEX使用前述严格四项TSO控制，SVM两臂同为acqrel。两模式、fp scale1/2、三轮的原计划顺序全部保留，没有替换或追加点。常规采样没有PROF/VERIFY/dump等变量；六条件一致、事件running=enabled、全部oracle/身份有效。以下是两端点斜率的三轮配对比值：

| 模式 | r9/r7 instructions | r9/r7 cycles（范围） | r9/r7 elapsed（范围） |
| --- | ---: | --- | --- |
| Function | 0.971353 | 0.994662（0.950287–1.001787） | 0.981103（0.947651–1.001757） |
| Block | 0.969435 | 0.997614（0.995985–1.000241） | 0.998846（0.996073–1.002459） |

指令斜率稳定减少约2.86%/3.06%，代码体积与指令密度已改善；cycles与elapsed仍跨1，未确认稳定运行提速，不能将少MOV直接等同于吞吐改善。同期r9/FEX严格策略探索性elapsed斜率为Function1.191546、Block1.150061，instructions为2.948980/2.752969；这不是与FEX默认策略的对照，也不构成完整语义等价证明。

perf-codegen-r9-fp-pmu-v1保存完整原始记录，未改的compare.py逐字段重放相同，root-independent-replay.json再次独立复核。唯一自有warm服务8388/start622557266的完整寿命PMU单列、未摊入主点；核对完整环境、exe和前后身份后标准关闭，transport0、无活动服务，手机测量窗口已释放。原冻结native manifest中继承的changed_since_accepted_r7布尔值属于先前快照，不能用于判断当前复用；输入身份一律按完整SHA，新perf-codegen-r9-native-identity-qualified-index.json明确映射，未改动测量所引用的原manifest字节。

相对原9bb的最终接受增量为8个既有生产文件、2个测试文件，生产41行增加/34行删除，净增7行；没有新compiler源文件、pass、opcode、分析表、状态或开关。两个被否决的inputs/scratch计数合并及frontier生产改动均不在最终delta。完整patch/source archive与逐文件前后SHA为perf-codegen-r9-accepted-total.delta.*；原481个归档成员已逐字验SHA，两个原本未修改的logging文件从原HEAD读取，不将大规模既有重构WIP算成本轮增量。

独立完成性审核为perf-codegen-r9-goal-completion-independent-review.json，逐项对应原始要求及上述证据，未发现本轮范围内缺失的工程验证。当前r9唯一launcher已压缩保留并逐字解压验SHA；关闭消费者后，仅移除手机一个旧cache43静态测试ELF，回收111280128 B。当前构建、基线、全部raw及源/Git/WIP保留；这次手机回收单列，不计入系统盘历史释放量。

## 已识别的生成代码机制差距

- FP guest400ff0热循环14个packed操作，原始9bb发射时缓冲区1032 B，其中14个结果NaN检查合计64条/256 B；r4缩减后的对应缓冲区为1000 B。固定FEX核心VFADD/VFMUL为单NEON操作，但当前CPU无AFP，NaN payload/SNaN优先级的一般等价性尚未证明，不能通过强开AFP或删除NaN修复取得表面收益；FEX源级形状不冒充最终发表字节。
- Block call leaf401600中SVM发射时132 B/33条，IMUL overflow切片11条/44 B。FEX保留typed NZCV关系，核心有SMULH/SBFX/CMP/CCMP源级形状。新方案优先评估现有TestNotZero/SetCarry/SetOverflow对signed64的窄替换，不新增pass/opcode/action；必须验证PF/AF/NZ、raw-carry资格、ADC/SBB及代码/compile代价。
- 机制证据及未应用候选补丁分别为active-results/perf-codegen-r1-mechanism-review.json、perf-codegen-r1-imul-flags-plan.json和perf-codegen-r1-imul-flags-candidate.patch。尚未修改该生成代码路径，不预记收益；旧prepublication RET不当作dispatcher瓶颈证据。

已实施的窄优化是packed64结果NaN guard：FCMEQ.2D的每个结果为64位全零或全一，把它当作四个32位word做既有UMINV.4S归约，与旧双UMOV+AND检测等价；继续FMOV.W/CBZ.W，不改变NZCV或NaN冷修复。每个2D guard从5条降到4条；4S保持原4条，保守scratch预算保持。

组合候选r3的真实发射缓冲对照中，Block42个unit总计8260→8196 B；其中loop400ff0为1032→1000 B，400f7f也少32 B。Function6个unit总计8864→8800 B；含两份算术路径的400f10为4192→4128 B。按删除指令建立地址映射后，其他非相对地址指令字节相同，差异只涉及相对分支/ADR重定位。四个既有AVX literal oracle明确启用AVX/JIT、关闭AFP并启用coldpath，在FUNC0/1×XPOOL0/1均有7699项断言且0失败。r4对同一guard另行运行最终ELF资格与发射对照，不能把r3组合身份改贴为r4。

r4已用最终ELF完成独立资格复核：483个源文件、118条退役路径、14个完整ELF身份一致；7次测试共276个JUnit testcase元素、520740项断言、0失败。四次AVX各7699项断言；四个fp1完整输出均匹配既有Rosetta oracle。r4自身与原始9bb的发射对照重复得到上述42/6个unit及32/64 B缩减，1870项指令/目标核对没有发现其他变化；没有把r3组合身份改贴为r4。报告为active-results/perf-compile-codegen-r4-qualification-independent.json。

这些是FinalizeCode后的发射缓冲，不是链接/SMC后最终内存字节。固定FEX现有ELF的VIXL disassembler关闭，JIT map也不含字节，不能用其源级一条NEON形状冒充整个最终host body的大小。IMUL候选因提前提交flags可能增加后续ADC/SBB/PSTATE恢复成本，暂不实施。

## r4 FP标准PMU：指令减少，耗时没有一致收益

active-results/perf-compile-codegen-r4-fp-pmu-v2保留同一红魔、固定9bb/c09/FEX、两种SVM模式、三轮和fp scale1/2的完整36点。所有点按预定顺序执行一次；原compare.py判定measurement_valid=true、issues/errors均为空。完整stdout匹配既有Rosetta oracle，原始PMU不多路复用，前后六项条件一致。自有warm FEXServer的完整寿命PMU另外记录，未摊入逐点主比较；PID/starttime/exe/完整env校验后标准-k关闭，transport0，没有残存活动服务。

以下为每轮两端点差值得到的斜率，再计算候选/参考的三轮配对比值；不是单点总耗时，也不是纯guest指令或pure-compile时间。

| 模式 | 指令斜率候选/9bb，中位数（范围） | cycles斜率候选/9bb | elapsed斜率候选/9bb，中位数（范围） |
| --- | --- | ---: | --- |
| Function | 0.944992（0.944708–0.945005） | 1.001646 | 1.004821（0.992870–1.125523） |
| Block | 0.942686（0.941823–0.943973） | 0.989234 | 0.985580（0.972901–1.011003） |

指令斜率分别减少约5.50%/5.73%，但cycles和elapsed均有配对跨1；不能把短guard和低指令数直接写成稳定运行提速。该批r4相对固定FEX默认配置的instruction斜率为Function3.252031倍、Block3.043117倍，elapsed约2.961299/2.971005倍。后续发现此跨引擎比较混入内存序策略差异，不能将约3倍全部归为编译器代码质量差距；见下一节独立干预。精简局部序列的实际收益仍须验证依赖链、指令组合及完整lowering；本轮没有把周期变化强行归因到某个微架构端口。

正式36点前的失败不混入结果：v1先因标准sh新增的精确`_=`服务环境项校验停止（0主点），随后因oracle artifact相对/绝对路径登记不一致，在1个真实基线点后停止，两次自有服务均已精确关闭。v2还有一次在任何adb前的工作目录错误（0服务/0点）；改用数据集cwd后原producer/plan字节不变，root执行真实初始化/完整artifact循环及旧首点的未改validate_sample通过，再开始全新完整36点。全部失败manifest/原始点保留，不补35点或跨服务寿命拼成原矩阵。

r4保留5个既有生产文件的窄修改和2个永久契约用例；r5/r6精简同一个既有生产文件，r7再修改转发入口和显式缓存版本。r8的生产增量已回退，只保留一个负向用例。没有新增compiler源文件、pass、opcode、分析状态或开关。各批独立delta/source archive和测量身份保留，不拼成未测量的总改善比例。

r1/r2、r4、r5/r6中间参考ELF均在关闭消费者后压缩、逐成员解压SHA验证，三次Data2净回收分别为182960128、91566080、182960128 B；手机旧r4及旧r5/r6副本分别回收117477376、234758144 B。唯一精确ELF、所有raw/源/Git/WIP仍保留；被拒R8 launcher也单独压缩保存。当前固定缓存保留原9bb、受保护的更早25bf与已接受r7参考，手机复用同一输入bank。手机/Data2回收不混入系统盘历史释放量，旧结果路径仍为0分配字节的Data2软链。

## r5内存序策略干预：约3倍差距的一个重要混杂因素

固定FEX默认TSOAutoMigration=true、TSOEnabled=true、VectorTSOEnabled=false。软件TSO还受IsMemoryShared和地址NonTSO资格控制；不能将配置中的TSOEnabled=true理解成每次向量访问均有屏障。实际默认FP afteropt中无LoadMemTSO/StoreMemTSO，选定自回边热块的三次向量读取为普通LoadMem；SVM默认acqrel对应热路径有三组LDR Q/DMB ISHLD。CoreMark的FEX IR却已有大量TSO访存，因此不能将FP结论推广成FEX默认所有工作负载都采用relaxed。源文件和配置的固定版本审计见active-results/perf-compile-r5-memory-policy-confounder-review.json。

独立批次active-results/perf-compile-r5-fp-memory-policy-v1使用同一r5 ELF、同一FEX ELF、Function模式、fp scale1/2、三轮，共24个主点。SVM两臂仅显式选择已有acqrel/relaxed；FEX严格臂仅增加TSOAUTOMIGRATION=0、TSOENABLED=1、VECTORTSOENABLED=1、MEMCPYSETTSOENABLED=1。默认生产策略未修改。严格臂afteropt已核对同一自回边三个SSA读取变为LoadMemTSO；不是仅看环境变量推测生效。

| 比较，均为两端点斜率的三轮配对比值 | instructions中位数 | elapsed中位数（范围） |
| --- | ---: | --- |
| 同一SVM：relaxed/acqrel | 0.957369 | 0.734982（0.715139–0.752631） |
| 同一FEX：严格/默认 | 1.071322 | 2.503787（2.473537–2.602563） |
| 跨引擎：SVM acqrel/FEX默认 | 3.256536 | 3.000744（2.999818–3.278095） |
| 跨引擎探索：SVM acqrel/FEX严格 | 3.035285 | 1.213139（1.198112–1.259564） |
| 跨引擎探索：SVM relaxed/FEX默认 | 3.119212 | 2.258454（2.204811–2.344294） |

这证明策略对本FP负载耗时有实质影响，之前约3倍不能全部解释成中间流程或RA效率差。它没有证明两个引擎的严格/relaxed分别具有完整相同语义，也不能把FEX四个控制项的全部效果归于所选三次读取。约1.21倍不是“已对齐语义后的最终差距”；strict比较的指令斜率仍相差约3.04倍，也不意味着剩余耗时可线性按指令数量缩小。两端点差值仍含规模相关的进程/运行时工作，不是纯guest计数；截距不能当作纯compile耗时。

全部24点保持原计划顺序，无替换/追加主点，完整输出和输入身份通过。PMU组running=enabled；前后六项条件相同，本批CPU4上限1171200 kHz，未调整手机设置，也未将其与旧批次绝对耗时拼接。自有warm服务的完整寿命单独记录并已标准关闭，手机测量窗口已释放。root复用未改的validate_sample逐点核对、哈希312个原始文件并重算全部配对比例，报告为该目录root-independent-review.json。原strict诊断曾把全流i128载入总数误当成热块数量，失败execution保留；接纳已有原始IR的块内资格后才运行独立完整24点，未重复诊断或篡改原失败。

目前生成代码仍有明确成本。r4的Block有限值正常回边路径131条，其中14条FP运算、56条结果guard、20条比较/逻辑选择，另有状态保存、常量读取/屏障、循环flags与分支；Function对应路径还有3次STR Q/5次LDR Q的SSA spill访问。各项来自实际路径分类，不是完整动态成本分摊。r7与r4的48个实际发射缓冲区又直接逐字节比对相同，证据为perf-compile-r7-retained-R4-emission-equality.json；旧r4 PMU仍保留其历史身份，不能因此改贴为r7的新运行测量。

r5最终IR的四组逻辑选择均隔着BitCast，最后一组还额外SetHostFPR发布，当时拟议的严格VecBitSelect融合0/4满足资格，未加入新opcode或放宽活跃性证明。r7已删去这些同型Alias，旧0/4不能自动外推到新图；后续如重开候选，须重新证明完整使用计数、隐式根和跨后端语义，再衡量所需复杂度。NZCV单mask复用仍因cold scratch契约未证而暂缓。当前4条packed guard未找到更短的完整等价序列；五处source snapshot均有必要，实际R5的14个FP操作也无左右同源机会，不做无收益的额外分支。

## 测量边界与后续候选

1. 当前r9相对原9bb的最终24点compile对照已完成，累计改善直接取该批数据，不相乘历史比例。固定FEX与SVM的实际编译unit/IR工作量、入口/初始化和线程累计范围仍不完全对等；不能将观测到的单槽约55ms与SVM请求累计直接签成精确倍数。两者均为O3/NDEBUG，FEX另外禁用部分断言与异常，SVM的必要契约仍在；未发现计时单位或普通路径重复累计足以解释差距的已发生缺陷。FP内存序干预解释的是运行时间，不能拿它消解compile热点。
2. 默认路径采样表明，目标输入准备、物理约束、分配及分配后的Complete仍有实际成本；职责拆分不会自动删除这些工作。后续可针对具体机制减少工作量，继续分别评估Block/Function；仅凭函数调用次数不能认定重复分析，也不能以新pass或dense表换取未经测量的局部收益。
3. 已按r7的新图重新审阅逻辑选择候选，并保留full uses、隐式根、marker边界及跨后端实现预算的设计。本轮选择直接消除packed compare复制的窄修改；未来若重开VecBitSelect，仍须实现并验证这些资格约束。当前guard与compare生成代码更小、动态指令更少，稳定cycles/elapsed提速仍未证明；如后续目标要求运行时提速或公平的跨引擎性能门，应先明确内存序、FP及计时口径，再沿真实路径验证。必要NaN修复和flags提交契约继续保留。
4. 更大的输入准备优化、BSL、IMUL flags与SSA spill属于后续候选，不是本轮漏做的验收项。后续仍按变更批次集中回归和测量，复用固定缓存，保留失败/原值、代码/Git/WIP和唯一证据，及时回收关闭的可再生成数据。

## 后续目标：消除逐操作 NaN guard，并继续接近 FEX 指令规模

用户在了解原检查机制后明确要求参考FEX消除guard。此轮采用固定FEX在无AFP时的普通浮点机制：ADD/SUB/MUL/DIV、SQRT和FMA直接使用宿主算术的NaN选择，不逐结果软件修复NaN符号、payload及源优先级；AFP仍受真实硬件能力门控。没有开启fast-math、伪造AFP或新增有限值分析pass。MIN/MAX选源、比较掩码、转换哨兵、有限值/零/无穷结果和未写入lane继续维持原合同。前文“保留NaN修复”的结论属于上一轮按位策略，不能用于描述此轮新合同。

### guard-r1：已验证的阶段检查点，目标尚未收口

该阶段source为05032fcd7085a286515a00f70023792c2aa51325277fca437e79435ec2846184，launcher为cb36bb5c80d2edcf928f5b6274116c246a07041e2eb0d279e1ac612817b56bcb，cache版本45。删除NaN guard、源快照、冷站点/共享修复ABI、相应x13 clobber和固定FPR预留；标量64位tied结果改用有预算的临时寄存器，保留高lane。v11–v14的可用性不再依赖AFP，已有pool关闭选项仍保留。FMA仅在accumulator设置可能覆盖乘数时租借一个临时；packed SQRT直接写结果，scalar SQRT只计算有效lane。

相对相邻已接受r9，22个既有生产文件增加125行、删除929行，净删804行，没有新compiler源文件、pass、opcode、分析表或持久状态。14个测试文件同步合同，唯一新增文件为test-only NaN比较helper；它只对指定算术结果lane中的quiet NaN放宽符号/payload，未修改10份原literal/op数据，有限/零/无穷、SNaN、未写入lane和旁观寄存器仍有严格检查。完整patch/源增量归档为active-results/guard-elimination-r1-source-delta.*。

十四个原生目标实际构建通过，源490文件及118个退役路径稳定。完整IR fast/slow、物理/暂存/发射合同和四组Function×XPOOL的8个FP oracle case共334个JUnit记录、813365项断言，0failure/error/skip；已有AOT拒绝门实际拒绝旧44并exit0。构建过程发现的旧FeatureCount断言及四个退役fixture参数已修正；此前失败build/source清单分别保留为v1–v3，不改贴为最终v4成功。

固定FEX的标准disassembler-on诊断构建与性能863 ELF分开登记，未新建clone或完整build树。第一次诊断只有IR：FEX缺省SilentLog=true、OutputLog=server吞掉了native日志。确认JIT对象实际含VIXL_DISASSEMBLER后，仅增加标准SILENTLOG=0/OUTPUTLOG=stderr，单独补一次诊断；原3次输出不改写。补充输出是实际CodeBegin/CodeOnlySize的反汇编文本，不含逐条原始编码。诊断guest编排漏了taskset，不能签CPU4性能资格；静态指令及位模式证据单独使用。正式PMU仍用原性能863并由未改point脚本继承mask10。

实际FEX正常回边为46条A64指令，对应同一段47条x86 guest指令。当前SVM逐条核对得到：

| 模式 | r9正常路径→guard-r1 | guard-r1/FEX路径指令比 | 全批发射缓冲字节 |
| --- | --- | ---: | --- |
| Block | 127→66；FEX为46 | 1.43478 | 8164→6424 B |
| Function | 136→66；FEX为46 | 1.43478 | 8768→6776 B |

两模式正常回边中NaN guard、源snapshot、NaN冷ABI和SSA spill均为0；全单元仍有状态/栈访问，不能宣称全部spill消失。共有42条可对应工作：14条FP算术、4条guest比较、12条选择逻辑、12条常量地址/读取/DMB。SVM另有4条非NaN复制及20条flags/交接，FEX另有4条flags，因此仍差20条。SVM全48个单元仅5个变化，所有缓冲长度/FNV通过；FEX完整区域含链接数据，不能把其全部word位置当成动态指令。SVM字节为发射阶段，未冒充链接/SMC后的最终内存。

使用两份已有永久C参考生成器原样编译的静态Linux guest，在同一手机分别运行SVM两模式与原FEX863：普通二元算术288行、SQRT72行、FMA3360行逐结果位一致，无有限/零/无穷/非规格数偏差。它们共同与原Rosetta literal存在已记录的NaN位差。MIN/MAX另外核对：SVM全部维持原literal，FEX的MAXPS/MAXPD有26行不同，包含有限/NaN选源及零符号；没有为模仿FEX而复制这些差异。详情为guard-elimination-actual-SVM-FEX-FP-opcode-lane-review.json，不据有限输入表宣称两个引擎全面等价。

随后完成原顺序标准36点：相邻r9为baseline，guard-r1为candidate，FEX使用前述严格四项TSO，SVM均acqrel，scale1/2各三轮。普通点无PROF/VERIFY/dump，全部输入、输出、六条件及PMU组有效，自有服务完整寿命单列并标准关闭。以下为三轮配对斜率中位比：

| 模式 | 当前/r9 instructions | 当前/r9 cycles | 当前/r9 elapsed | 当前/FEX instructions | 当前/FEX elapsed（范围） |
| --- | ---: | ---: | ---: | ---: | --- |
| Function | 0.486659 | 0.882218 | 0.884929 | 1.436026 | 1.032878（1.027768–1.232279） |
| Block | 0.520741 | 0.909455 | 0.907801 | 1.430767 | 1.048821（1.040992–1.060173） |

三对当前/r9的cycles和elapsed均下降。Function/FEX第三轮elapsed比1.232279完整保留，不能把中位约3.3%写成每轮都只差3.3%。统计范围是CPU4过滤的继承进程树，包含随规模变化的运行时工作；不是纯guest计数或无条件全CPU总量。两端点截距不当作纯compile。原36点及root未改compare重放位于guard-elimination-r1-fp-pmu-v1，独立复核为guard-elimination-r1-measurements-independent-review.json。

另行24个coarse点直接比较当前与相邻r9，PROF=1、PROF2完全缺席，不改普通PMU validator：Core的Function/Block中位比分别1.004033/0.997599，均跨1；FP Function为0.968458（0.967765–0.974293），FP Block为1.003158（0.972204–1.129957）。FP Block第三点约13%的变慢不删除，未证明Block编译提速，也不把各阶段比例相乘。原始计数、工作量差异与全部24点位于guard-elimination-r1-coarse-v1。

该阶段证明guard已清除且数量显著减少，尚不把66对46写成完全对齐。下一候选只针对实际剩余的home复制和被布尔化的相邻INC/DEC条件，复用已有producer、别名、fault/observer和local-NZCV证明；不盲开实验loop开关，不删除必要PF/AF、CF或故障出口物化。用户完整目标继续保持进行中。

### guard-r2：消除四次发表复制与相邻条件的布尔化

R2 v2 的 source 为8ff64385940025b2086505c50d22f70c2c08fbc8f0f0d96f62646f1a79cc96c2，launcher为cdbf9141d379efffa4fd5cd85898f23788b5904ae782ac2b5cfe006cc7022dd6，cache版本46。两个既有FPR producer白名单仅补入LoadMemoryTSO和VecAndNot；V128、完整home、最后使用、旧home、fault/observer窗口与物理重分配认证保持。REG32/64 INC/DEC复用现有local-NZCV条件，窄宽度与内存形态仍走旧路，CF快照与恢复此阶段仍保留。

两模式实际正常回边均由66降为60条：四个发表copy全部消失；原Ubfx/Eor/CBZ三条布尔分支改为单B.cond，再省两条。公共42条工作不变，余18条为flags、poll和交接；相对FEX实46条仍多14条，为1.30435倍。完整Block 42个unit合计6424→6300 B，Function 6个unit合计6776→6644 B。全96条R1/R2 host记录的长度与FNV均重算通过；这些仍是发射缓冲，未冒充最终发表内存，也没有R2运行速度结论。

受影响9份JUnit共72行、324874断言通过，旧45版AOT拒绝106 checks通过。原首轮仍登记失败：新增INC8夹具的biased mapping在XPOOL0触发pool4分配错误。仅将夹具的三个地址改为direct mapping，保留全部96组合与语义断言后重新冻结；生产launcher不变。首轮已通过的第一模式FPR用例未重复采集，原失败XML不改写。独立记录为active-results/guard-alignment-r2-independent-emission-and-correctness-review.json；当前launcher另以精确单成员压缩包保留。

后续新增故障回归时发现共享测试头的覆盖缺口：RISC-V assembler把Catch的CHECK宏替换成assert，Release构建会省略这些表达式。上述计数是原始JUnit实际记录的断言数，不能额外包含未执行的CHECK。旧向量故障用例虽实际调用过Run，其CHECK结果与清理调用仍须在宏隔离后重新验证；原记录不回填为新的完整故障资格。修复通过push/undef/pop隔离测试头中的assembler宏，实际Android Release预处理已确认恢复Catch CHECK；相应新增测试仍需真实运行。

进一步核对FEX后，不能直接照搬其PF/AF删除：实际FEX pre-RA后优化IR中，循环的StoreAF/StorePF由普通CFG活跃性分析删去，而普通访存fault不作为flags读取。Signal路径仍从固定PF/AF寄存器重构，未观察到针对已删除flags的逐PC重算。SVM同一Function的普通CFG LiveOut虽只有CF，Block的未知出口却仍为All；两者都不能据此证明访存故障时PF/AF也无须恢复。现有将非空Function出口需求提升为All的策略暂时保持，避免用正常续行的证明替代故障语义。源级和实际IR证据见active-results/guard-alignment-fex-pfaf-mechanism-review.json。

### guard-r3/r4：carry 优化保留，47 条的错误准入不验收

R3仅对FlagM/CFINV已启用、carry表示为Direct的REG32/64 INC/DEC省略CF快照/恢复。窄宽度、内存及旧carry表示保留原路。实际测试发现两处scratch叠加，修复为四种宽位算术复用自己新建的AF快照，以及NZV用五条位域指令合并；预算未扩大，原断言未删。R3 v2普通回边实测Block57、Function47，但Function的BranchOnlyFlags(ZF)删去了PF/AF与NZV发布。这是删除假CF读取后暴露的已有DCE准入问题，不能把47对FEX46当成完整对齐。

永久回归在可读页后布置PROT_NONE，分别检查首次和第二轮循环前缀访存故障。初版CHECK被assembler宏覆盖的29断言运行、以及读取未同步context.ef的677断言运行均保留，但不作为guest flags证据。有效夹具用独立POPFQ/HLT播种，故障后在同一core用LAHF/SETO读回实际flags，先检查RIP、GPR和全部16个XMM。Block的24组故障场景通过；Function首次故障12组对照通过，第二轮12组各错PF/AF/ZF/SF/OF五位，共60错，CF正确。前端双方MOV[mem]先于flags覆盖的结构负例另实际发现marker误判。权威RED及独立审查见active-results/guard-alignment-r3-fault-before-fix-guest-readback与guard-alignment-r3-authoritative-red-independent-review.json。

R4 v2 source为e801292c1baabc18a307be6da91854065ca7fd74e3434ed478629d3bc98eeacd，launcher为8b2b2f173d255bba5707c677ef08b2ca921144691d51eff7519f6318f1a1de91，cache版本48。共享flags效果把MayFault纳入观察；前端仅以已审计无故障的寄存器前缀/完整覆盖证明dead flags，移除不安全的CALL追踪，保留direct-JMP依赖；已解码HIR需求不再被marker覆盖。纯NoFault helper仍保持原契约。另一次新IR夹具的const编译错误只通过owner.EditBlock修正，没有const_cast或断言放宽。

同一有效故障夹具与环境下，R4两模式均为2行/864断言、零失败；两套IR引擎各128行/227682断言，旧126例完整保留，新增2例36个小IR。合计260行、457092断言，Function原60个真实flags错误归零。当前实际回边为Block57、Function58：保留故障所需AF/PF/NZV发布，Function另有一次PF token交接。相对FEX46仍多11/12条。Block42个unit合计6688 B、Function6个unit7900 B；相对R3错误版本增加412/1412 B，不把语义恢复写成体积收益。R1–R4共192条host记录的长度/FNV均通过；独立报告为active-results/guard-alignment-r4-independent-emission-and-correctness-review.json。

这仍是有限检查点：完整NZCV的Deferred边与transparent传播尚需对齐fault观察，producer之后的故障窗口也未收口；复杂标量TSO地址与前置flags提交的scratch峰值另待验证。宏修复后的FPR故障与FP广域重验、最终PMU/coarse对照均未完成。没有用上述窄回归签完整fault合同或新运行速度，用户目标继续保持进行中。

### guard-r5：修复完整 NZCV 故障交接与前置观察的 scratch 叠加

在 R4 生产代码不变时扩展永久故障夹具：原 DEC 的 24 组保留，另加 SUB 的 24 组，冷后继也先访存再覆盖 flags。实际 Block 的 48 组通过；Function 的 SUB 第二次前缀故障出现 ZF/SF/OF 各 12 错、CF 6 错，共 42 错，读回值均为旧种子；PF/AF 正确。首故障对照与原 DEC 均通过。另两份永久 IR/edge 回归实际复现了 fault-only 透明链错误，以及算术 producer 后、AdvancePC 前的故障错误删除 flags。REQUIRE 中止后未到达的情形不计通过。原始 RED 和身份审查保留于 active-results/guard-alignment-r5-beforefix-gate 与 guard-alignment-r5-beforefix-independent-review.json。

标量 TSO 永久回归覆盖复合大位移的 load，以及寄存器、常量、spill 三类 store。四个独立 section 在修复前都实际因声明 3、申请 4 个 scratch 而中止 Translate，尚未执行 Runtime。该夹具在自身 module 中强制 XPOOL1、使用 Block 分配；外层四种环境组合不能额外证明该夹具的 Function 或 XPOOL0 路线。修复只在两处地址折叠复用已完成 flags 合并的 shared scratch，未扩大预算或预留集合。原 RED 位于 active-results/guard-alignment-r5-scratch-beforefix-gate。

R5 source 为 e1fa7ff7c90fdf965cce5c8fc857129800577d3fc5dac9323dbc97bfa4080f13，launcher 为 d50fbef065f55b695d3572c2ed0c2d6f8276d005f79556bf44b14864fc10789a，cache 版本 49。生产改动还包括：MayFault 阻断未来覆盖对当前 incoming NZCV 的准入，fault-only 块不再传播透明性，以及按实际指令身份拒绝 producer 后的故障窗口。完整 14 个原生目标构建通过。

修复后 8 份 JUnit 共 454 行、637094 个实际断言，失败、错误及跳过均为零；覆盖四种分配环境的相关合同、两个浮点语义组合，以及两套完整 IR 引擎。新 primary-window 用例完整执行 4 个图、16 断言；新 edge 情形和四种 scratch source section 均达到后续检查，scratch 的有效访存结果与真实 flags 也通过。宏修复后的 resident vector fault 用例此次重新执行。旧 48 版 AOT 被拒绝，当前兼容对照保留，独立 native gate 共 106 checks 通过。实际执行位于 active-results/guard-alignment-r5-gate。

两次原 FP1 发射的正常回边仍为 Block 57、Function 58，对照 FEX 46；所选回边机器字与 R4 相同。完整 Block 42 个 unit 仍为 6688 B，Function 6 个 unit 为 7908 B，增加的 8 B 在其他交接段。故障正确性恢复没有被写成代码体积或速度收益。下一步继续复用已有 loop-lazy 机制，收口入口、PF token、冷路径 scratch 与 cache/AOT 资格后再验证；当前尚无 R5 PMU 或新的编译耗时结论。

### guard-r6：两种分配模式的正常回边收敛到 51 条

R6 复用已有 loop-lazy 配方：普通入口先恢复 canonical flags，只有已证明的自环进入保留 NZCV 的本地入口；每轮在源值仍存活时发布 PF，NZCV 留到冷出口及故障恢复。冷合并使用已保留的寄存器；effective XPOOL 包含 PIN_EXT3 的隐式启用，不再误用可分配的 x16/x17。实际启用磁盘缓存或 SerializableCodeEmission 的单元不导入私有自环入口，缓存继续采用标准 ABI。未新增优化开关、持久状态或 scratch 预算。

首版实际只有 Function 降到 51，Block 仍为 57，不能以两者共用部分代码为由合并验收。Block 除旧 latch 准入条件外，还保留 DEC 的 AdvancePC、LocalCondSet 以及末尾 Jcc 的 AdvancePC。调整后仅额外接受“条件之后且位于指令表末尾”的分支边界；原有两个条件之前的边界负例保留，新增 32 组静态正反例验证该限定。

新增永久 guest 回归用先声明的 JRCXZ 目标保证前驱 ADD 经真实内部 LinkBlock 进入循环，并覆盖透明中继；故障后用 LAHF/SETO 读回六 flags，同时检查实际 Function 归属、跳转和私有恢复范围。它发现显式关闭 loop-lazy 时普通无条件内部边漏提交 NZCV/PF：12 个外来入口组合各错 CF/PF/ZF/SF/OF，AF 正确，共 60 错。最终 IR 保留完整 SaveFlags；实际机器码只写 AF、搬运 PF token 后就进入故障 LDR，根因是普通边的发表遗漏。原 RED 与诊断分别保留于 active-results/guard-alignment-r6-gate-v4 和 guard-alignment-r6-loopoff-debug，没有放宽断言或删除关闭开关的对照。

修复复用既有目标接受表：只有目标接受实际 pending mask 时才延迟 NZCV，并独立发布 PF；其他普通内部边执行合并。缓存版本升为 51，拒绝旧 50 的存储代码。最终 source 为 778c3f9cbf90374f9dda555f79af33313c2797dcc47e42c80600e3057998bbb4，launcher 为 02085b0bedb2cebc8eebea93d71ed44604cf2f6cc2d1f436b494f3d43b27bfe9。

最终 19 份 JUnit 共 495 行、819884 断言，失败、错误和跳过均为零；上述 60 错归零。原生 AOT/退役门另有 353 checks 通过。真正 eligible 的磁盘循环验证自动保存、重载及执行，并在写入和加载两侧检查无私有宽恢复；AOT 以实际普通私有恢复为正控，验证 serializable 源及安装产物均无私有入口，销毁源实例后仍正确执行计数器、六 flags 与 V128 数据。四种浮点语义环境、两套完整 IR 引擎及显式关闭开关、legacy carry、PIN_EXT3 对照均保留。测试夹具自身强制的模块或分配形态仍按各自范围解释。原始结果为 active-results/guard-alignment-r6-gate-v5。

| 原 FP1 正常热回边 | Block | Function |
| --- | ---: | ---: |
| 原 r9 | 127 | 136 |
| R5 | 57 | 58 |
| R6 最终 | 51 | 51 |
| 固定 FEX 实际反汇编 | 46 | 46 |

两个 SVM 正常回边的机器字节 SHA256 相同，均包含原来公共的 42 条工作，以及 AF 4、SUBS 1、PF 1、条件分支 1、退出轮询 1、回跳 1。相对 FEX 仍多 5 条，为 1.108696 倍；热路径中 arithmetic NaN guard、发表复制、NaN 快照及私有状态 Q 访存均为零。完整发射缓冲是 Block 42 个 unit、6736 B，Function 6 个 unit、8444 B；相对 R5 增加 48/536 B，包含新入口、冷恢复和普通边发表。原始发射及逐条注释位于 active-results/guard-alignment-r6-emission-v5 和 guard-alignment-r6-normal-loop-annotated-A64-v5.asm。这些字节记录的范围仍是发表前缓冲。

补充的 flags consumer、token 和冷路径现有回归另有一份 JUnit、35 行、36239 断言通过。合计 20 份 JUnit、530 行、856123 个实际断言，加上独立的 353 个 native checks。代码及路由的独立资格见 active-results/guard-alignment-r6-v5-independent-codegen-qualification.json 与 guard-alignment-r6-completion-independent-audit.json。

### guard-r6 最终测量：代码数量接近，编译累计耗时未全面改善

使用红魔 NX729J、同一 CPU4，在 walt、614400–1171200 kHz、Charging、USB 在线的固定六条件下，完成 36 个普通 FP 点及 24 个独立 coarse 点，全部通过原指标校验，没有替点。普通点不传 profiling/diagnostic 变量；coarse 仅开 PROF1，PROF2 缺席、VERIFY_PLAN=0。原 r9、当前候选、标准 FEX 三个 ELF 身份固定，两个 SVM arm 均显式 acqrel；FEX 沿用原严格 TSO 配置。原始目录分别为 active-results/guard-alignment-r6-fp-pmu-v1 与 guard-alignment-r6-coarse-v1。

普通 FP 的两规模增量比值如下，均为三轮逐对比值的中位数；统计域是 CPU4 过滤的继承进程树，包括随工作量变化的运行时工作。

| 模式 | 当前/r9 指令 | 当前/r9 cycles | 当前/r9 elapsed | 当前/FEX 指令 | 当前/FEX cycles | 当前/FEX elapsed |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Function | 0.376191 | 0.838065 | 0.839650 | 1.110020 | 1.019391 | 1.020662 |
| Block | 0.402257 | 0.896852 | 0.890988 | 1.107421 | 1.023048 | 1.016276 |

Function/FEX 的增量 elapsed 三轮范围为 0.998623–1.024818；Block 为 1.006758–1.020747。静态 51/46 与动态增量指令约 1.11 倍相符；这不把完整进程耗时也定义为约 2% 差距。完整进程的当前/FEX elapsed 中位数在规模 1/2 下分别为 Function 1.102691/1.071604、Block 1.086751/1.054818，初始化、加载、编译与输出等成本仍在。两端点截距不作为纯 compile 时间。

FEXServer 单独覆盖启动、36 次请求、期间空闲和关闭的完整生命周期，记录 61,223,081 cycles、78,462,865 instructions、124.307429 s；不摊入或重复加入各 primary 点。原服务 PID 30811、transport 和私有 socket 均关闭。普通 36 点的原 compare 重放结果与冻结结果逐字段一致。

24 个 coarse 点直接比较当前与原 r9，保留所有工作量和计数器差异。以下为 inclusive translate_ns 的逐对比值，不是排除命中、等待、发布和 teardown 的纯编译 CPU 时间，也没有新的 FEX pure-compile 对照。

| 工作负载 | 模式 | 当前/r9 中位数 | 三轮范围 | 本轮观测 |
| --- | --- | ---: | --- | --- |
| CoreMark2000 | Function | 1.006047 | 1.001989–1.028445 | 三对均增加 |
| CoreMark2000 | Block | 0.996613 | 0.991662–1.019362 | 跨 1，未证明稳定收益 |
| FP1 | Function | 1.037361 | 1.011859–1.039513 | 三对均增加，中位约 3.74% |
| FP1 | Block | 0.981839 | 0.980760–0.991016 | 三对均下降，中位约 1.82% |

FP Function 当前三次 translate 为 30.385/30.456/30.367 ms，原 r9 为 29.290/30.099/29.212 ms；Block 当前为 26.400/25.931/25.790 ms，原 r9 为 26.889/26.440/26.024 ms。不把较少的热指令写成编译全面提速，也不对阶段计时求和、按 IR 数归一化或叠乘历史收益。

### 与 FEX 现有编译累计读数的对照

下表将最新 guard-r6 的 CoreMark2000 `translate_ns` 与前述固定 FEX 的历史标准 JIT 单槽观测并列。各项均取三次观测中位数；这是跨批计时器读数对照。

| 实现 | 累计耗时中位数 | 相对 FEX 读数 |
| --- | ---: | ---: |
| 固定 FEX，已观测线程槽 | 54.522 ms | 1 |
| guard-r6 Block | 929.206 ms | 17.043 |
| guard-r6 Function | 1260.931 ms | 23.127 |

FEX 原始三点及线程槽资格见 `active-results/perf-compile-r1-fex-stats/standard-stats-three-observations-review.json`，SVM 三点见 `active-results/guard-alignment-r6-coarse-v1`。两端计时覆盖的查询、锁等待与初始化边界不同，FEX 尚无完整线程生命周期累计证明，实际编译单元与 miss 数也未对齐。因此 17/23 倍仅是现有累计计时读数之比；精确同口径的纯 compile 倍数仍未测全。本轮生成代码与运行速度的改善不作为编译成本已追平的依据。

最终保留普通算术 NaN 符号/载荷按 native 传播的策略变化，以及 MINMAX、比较、转换、scalar 上半 lane 和精确故障状态的既有契约；51 条仍比 FEX 多 5 条。该目标的完成范围是 guard 清理及已验证代码数量接近，不代表全 ISA/bit-exact 等价、FEX 性能完全相同或整个中间流程重建计划完成。原始失败、所有 60 个测量点及设备条件完整保留。最终 metadata 更正副本为 active-results/guard-alignment-r6-source-manifest-final.json；测量绑定的原 v5 清单及其 SHA 原样保留。

测量独立复核为 active-results/guard-alignment-r6-independent-measurement-review.json：原 compare、coarse validator 与 parser 重放均与冻结输出逐字段一致，所有原始点、输入身份、38 次服务身份检查及关闭证据通过。精确 launcher 以 guard-alignment-r6-launcher.tar.zst 单成员保留，完整源覆盖、退役清单与 Git/WIP 身份索引另有 guard-alignment-r6-source-overlay.tar.zst；压缩件均逐流读回校验。代码整理提交为 `ed41e6f6`，暂存源码逐文件匹配上述冻结清单；Git 提交不回写测量清单或改变原始结果的身份。
