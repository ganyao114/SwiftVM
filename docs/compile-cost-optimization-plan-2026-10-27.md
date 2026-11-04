# SwiftVM 编译成本优化方案

2026-10-28 后续分析：新的 [spill 与 compile 根因及两倍目标方案](spill-and-compile-root-cause-plan-2026-10-28.md) 已单独记录当前源码诊断、两种分配范围和 FEX 对照。后续最终目标为两个模式各自 `≤2×`；本页 `<10×` 与 C24–C34 结果保留为原实验历史，不追改阈值或结论。

2026-10-28 历史状态说明：用户当时暂停了本页 C24–C34 的性能实施，随后 [ARM64 编译流程维护性整改](arm64-compile-pipeline-consolidation-plan-2026-10-28.md) 已完成至 layout-r7。下文“当前 C31/PGO”“下一项 N3”和 `<10×` 均保留当时的实验语境；最新 source、普通 R3 差距与下一步顺序按上述根因方案，不将旧 checkpoint 称作当前部署产物，也不将已完成的目录/owner 收敛再计为未来收益。

日期：2026 年 10 月 27 日，更新至 10 月 28 日。原始测量基线为 `ed41e6f6a26836eabe88e5c8a404b4b363c74e49`，对应源码摘要 `778c3f9c…`、Android launcher `02085b0b…`、cache 格式 51；配套文档和工具起点为 `660b6ba1`。当前完成既定编译与指令质量验收的工程检查点为 C31：源码 `3cb2a8f7…`、PGO launcher `56fb68c9…`、profile `0e4affdd…`、cache52。历史C19及原始证据保留。有限质量门、运行时间的实际增加与完整FEX目标分别记录，不把检查点称为完整目标完成。

本次依据固定FEX源码、C19/C31采样和C24–C34的实际结果重排方案。C31仍是已选定的有限工程检查点，正式FEX28点的数量级门未通过。C34 Function raw-first完成16目标构建、独立frontier16用例/1570断言、完整54质量、四点MaxRSS及原七轮56点；测量有效，但Core正门失败，13个实验文件已撤回，原392项配置、三个配置文件、host16及phone16精确恢复C31 PGO。**当前源码3cb2a8f7…、主程序56fb68c9…、profile0e4affdd…、cache52；C34只保留研究证据。** 下一步仅准入N3完整IR布局与生命周期净账设计，不把原型、质量门或内存门通过称为收益。Block与Function继续分开设计和验收。

## 0 本轮执行方案

最新同批外部测量中，Function/Block 的编译服务 elapsed 配对中位为 FEX 的 **21.0054×/13.1035×**。若只把 FEX 分母视作固定，抵达 10 倍中位边界仍需约 52.4%/23.7% 的下降；这只是差距预算，不是可实现收益预测，最终还须满足每轮严格小于 10 倍。双方完整服务的计时边界仍有差异，不能把这些比值解释成同边界纯 CPU 或每条 IR 的成本。

FEX 提供的主要参考是控制工作量：先稳定能提前确定的解码结构，复用有明确生命周期的存储，让后端直接消费已经完成的结果。SVM 已完成阶段拆分，接下来应删除实际重复工作；继续改目录、减少 pass 名称或关闭生产检查都不能解释或解决当前差距。

| 顺序 | 机制与 FEX 参考 | SVM 的具体工作及删除对象 | 当前状态与下一决策 |
| --- | --- | --- | --- |
| C34已结束并拒绝 | raw边界稳定后再展开HIR | 已完整验证合格self late-entry的owned记录、prefix/suffix、字节witness和有限Function内重试 | Core Function/Block配对中位0.996976/1.016746，两个正门均失败；13文件撤回并精确恢复C31，不重包同一候选求通过 |
| 下一项，先取净账 | 稳定身份、opcode实际payload、唯一存储owner | N3语义迁移路线已审查；设计具体header/用途头/参数payload，并核对全生命周期tombstone、旧payload及索引后的净工作；覆盖Block与Function | 生产原型仍HOLD。先冻结可实现布局与计账公式，再考虑两模式各一次有界IR_DETAIL aggregate census；不以94→20或旧getter热点直接准入 |
| 后续条件项 | DFE 内相关变化协作传播，后端消费 PostRA | 在现有 residual owner 内删除同一稳定 epoch 的重复发现/求解；或在既有 facts→inputs→Complete 交接中删除经证明等价的重复物化 | 现有 worklist、描述共享及冻结 actions 已存在。必须找到尚未共享的具体工作和真实覆盖，不能把整个优化/Complete 桶算作收益 |
| 备选 | compiler/线程缓冲跨请求复用 | 只针对已有 DCE owner/RA 临时 list 研究受限的跨 scope chunk 复用，删除重复上游申请与释放 | C32 已拒绝；C33 仅为独立提案，不把旧结果或 allocator 总占比当作新采用资格 |

**独立 Block 的推进方式。** 当前没有证据支持用顺序分配替代原 allocator，Function raw-first 也不提供 Block 的专属收益。Block 后续先沿现有 `SemanticFacts/BlockAnalysisIndex → AllocationInputs → AllocationPreparation/UnitActions` 核对每份中间数据的生产者、最后消费者、稳定时点和实际分配调用：静态 schema、feature 和最终 IR 已唯一决定的数据，才可能由既有 owner 生产一次；依赖固定 transfer、跨块 recolor、scratch 或最终物理位置的数据继续在原时点生成并验证。先列出可删除的整段构建和全部消费者，再决定是否实现；当前没有据此承诺 Block 能下降 23.7%。共同表示路线同样须先完成准入，不能用 Function 的收益替代 Block 验收。

**执行与停止顺序。** C34已按完整质量→内存→generic七轮56点执行，结果未过正门，已撤回；没有追加、替换点或训练其PGO。下一项N3先取得完整布局净账，若获准实施，仍须完整消费者迁移后按同一顺序验收。机制包过门后才训练匹配源码的PGO，再做直接原R6、普通PMU及外部FEX；Block、单块Function、多块Function分配边界分别保持。

**完整目标的交付。** 每个保留机制都要有旧路径删除清单、源码/构建身份、正确性与生成代码证据、相邻 compile 对照；最终组合必须重新满足两个模式各七对 `<10×` 的外部门。现有普通指令门为前后斜率比不超过 1.01，运行 wall/cycles 变化仍需单独报告；字节门或指令门通过不等于所有运行时间零回退。具体停止规则沿用第 6、7 节，不在实验后降低阈值。

实际实施、失败控制与测量状态见 [优化执行记录](compile-cost-optimization-progress-2026-10-27.md)。

## 1 目标与测量基线

下面是本方案起点 guard-r6 的历史阶段读数，CoreMark2000 三个独立观测中位数，单位为 ms。它们用于解释最初选题，不代表当前 C31 的阶段分摊。

| 现有计时项 | Block | Function |
| --- | ---: | ---: |
| 编译请求累计 translate | 929.206 | 1260.931 |
| decode 与前端处理 | 92.678 | 223.594 |
| RPO 与编号 | 0 | 6.858 |
| IR 优化 | 119.175 | 223.700 |
| 分配输入与物理分配 | 255.867 | 340.216 |
| 目标准备与机器码发射 | 298.938 | 328.915 |
| 发布 | 76.299 | 43.628 |
| 显式 IR 清理 | 6.368 | 7.263 |
| 两处显式锁等待 | 0.850 | 0.159 |

起点 FP1 的 translate 为 Block 25.931 ms、Function 30.385 ms；分配桶分别为 9.225/11.356 ms，目标准备加发射为 8.333/7.539 ms。这些历史读数曾支持首轮检查分配输入与 target completion；当前排序以 2.1 节的新证据为准。

各桶分别取中位数，且计时没有覆盖全部构造、代码区申请和析构。`codegen_ns` 包含 target completion；`publish_ns` 从代码区申请成功后开始；`translate_wait_ns` 只覆盖配置锁和编译范围锁。不得用阶段和反推未记录成本，也不得把 allocator 的 inclusive 采样再加一次 Complete。

固定 FEX 的历史 CoreMark 标准 JIT 单槽中位数为 54.522 ms。17.043/23.127 倍是跨批累计计时器读数之比，尚不能逐阶段分摊或解释为同口径纯编译 CPU 倍数。优化采用与否首先依据 SVM 同版本窗口内的前后对照，另用固定 FEX 检查外部差距。

后续 C6 的直接对照中，CoreMark2000 translate 中位数为 Block 834.693 ms、Function 1139.200 ms；相同性能 FEX ELF 的三次严格 TSO 单槽累计计时中位数为 47.257 ms。C19 PGO 的新七轮直接原 R6 对照得到 Block 660.198 ms、Function 1002.443 ms，累计配对中位改善 29.69%/20.59%，普通 PMU 72 点及独立重放也已完成。与上述 FEX 历史读数的跨批比约为 13.97/21.21，说明剩余差距很大；这些数值继续受两种计时边界、实际编译单元及 FEX 线程槽历史限制，不能当作纯编译 CPU 倍数。详细原始 ticks 和计时范围复核见执行记录。

历史 C19 检查点的实际表现如下。translate 来自直接原 R6 的七轮对照，wall slope 来自普通 PMU 的两档规模差分；两者口径不同，不能直接相减。当前 C31 结果另列在后表，不混用批次。

| 负载与模式 | C19 translate 中位 ms | 相对原 R6 的配对中位改善 | 普通 wall slope 中位比 |
| --- | ---: | ---: | ---: |
| CoreMark Function | 1002.443 | 20.59% | 1.002642 |
| CoreMark Block | 660.198 | 29.69% | 1.006772 |
| FP Function | 24.051 | 20.80% | 1.009612 |
| FP Block | 19.175 | 27.10% | 1.008206 |

12 个普通指令斜率通过既定 ≤1.01 门，但 CoreMark 两模式三轮 wall/cycles 均增加，FP wall 范围跨 1。因此 C19 是有限工程检查点，不能写成全部运行时间无回退。

当前 C31 直接原 R6 的七轮结果与普通 PMU 结果如下，未与历史 C19 跨批相减：

| 负载与模式 | C31 translate 中位 ms | 相对原 R6 的配对中位改善 | 普通 wall slope 中位比 |
| --- | ---: | ---: | ---: |
| CoreMark Function | 995.320 | 20.9993% | 0.991636 |
| CoreMark Block | 614.355 | 33.8604% | 0.985014 |
| FP Function | 23.169 | 23.0210% | 1.009059 |
| FP Block | 19.037 | 27.8753% | 1.003381 |

当前 12 个普通指令斜率均通过 ≤1.01 门。FP Function/Block 的 wall 中位分别上浮约 0.906%/0.338%，两组范围均跨 1；不得据指令门宣称所有运行时间无回退。

完整目标是 Block 与 Function 的编译成本都与固定 FEX 处于同数量级，并保持生成代码质量。首个外部工程门已在测量前冻结为两模式各七对全部 `<10×` 且配对中位 `<10×`。正式 28 点测量有效，但 Function/Block 的配对中位为 21.0054×/13.1035×，均失败，见 6.2。比较的是明确定义、边界仍不同的完整编译服务 elapsed，不是同边界纯 CPU 或每 IR 成本。历史跨批 13.97/21.21 不替代该门，也不能以内部中途改善宣布目标完成。

实施要求分为三层：

1. 保持独立 Block、单块 Function 和多块 Function 的分配边界，分别报告共同成本与函数专有成本。
2. 已完成的内部工程检查点不再作为下一阶段终点；新机制必须说明覆盖的大块实际工作，不以若干百分比的小候选推算数量级改善。
3. 保持生成代码和运行质量。当前 FP 正常热回边两模式均为 51 条，固定 FEX 为 46 条；compile 专项优先保持当前机器码和故障语义，并保留运行时间的实际限制。

原始分项与范围见 `active-results/compile-cost-breakdown-20261027.json`；阶段历史见 [编译与代码质量记录](compile-codegen-performance-2026-10-26.md)。当前 `active-results` 实际位于 `/Volumes/Data2/swiftvm-middle-preserved-20261025/active-results`。

## 2 借鉴 FEX 的具体机制

性能参照固定为 Android FEX `a156752fae3a4f65fefb1ed95af980bcdfac43d7` 加已登记的平台补丁，构建配方见 [FEX Android](../tools/fex-android/README.md)。不混用旧重构方案的另一个官方源码参照。

| FEX 的机制 | SVM 当前对应情况 | 本轮采用方式 |
| --- | --- | --- |
| raw decode 后由 dispatcher 构造 IR，构造时处理适合局部折叠的操作 | typed builder、局部折叠、guest-state 转发已经存在 | 只处理剩余重复工作，不再迁移一遍全部优化 |
| x87 存在性门控，无该家族即可早返回 | 多个 target 家族已有 BlockSummary 门控 | 只补仍在执行且能证明为空的访问 |
| DFE 将 flags 与 DCE 协作，使用 CFG/worklist | 当前 residual passes 已有 flags/value/DCE 通知协作 | 保持协作，避免另加同类 pass 或第二份图 |
| RA 在每个 CodeBlock 内反向分析 kill，再正向分配、spill/fill 和部分合并 | Block 与 Function 共用核心，但 Function 支持更强的跨块合同 | 优化共同输入及局部处理，不替换 Function 的全局活跃性 |
| next-use 在首次需要 spill 时、选择 victim 前按需准备 | SVM 已有按需 spill 索引和重试复用 | 不重复列为新收益 |
| 线程持有 decoder、RA 和 IR 缓冲并复用 | SVM 已有 TLS Inst arena、HIR 池及 workspace | 精简仍存在的每单元容器和数据搬运，控制保留容量 |
| dispatcher 缓存 guest 寄存器写；普通编译每条 guest 指令前执行 SRAOnly flush | SVM builder 已转发值，pass 已做 DSE 和部分 ordered sinking | 研究少构造死写时保留自身 fault/partial/order 合同；FEX 不能证明跨指令延迟所有写回安全 |
| 32 位相对偏移连接节点与按 opcode 大小分配的 payload，双线性区持有 | SVM 已有 arena，但 Inst 仍是固定参数槽与指针表示 | 仅在当前 IR 访问成本获证后研究表示；FEX 仍有双向链接，不称其为无链数组 |
| 后端消费 PostRA 结果 | SVM emitter 消费冻结 allocation/actions | 在冻结前完成必要工作，减少中间结果复制；emitter 不重新分析 IR |

固定源码入口：[compiler 初始化和编译请求](https://github.com/BulkanDrivers/FEX-android/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/Core/Core.cpp)、[默认 pass](https://github.com/BulkanDrivers/FEX-android/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/PassManager.cpp)、[CodeBlock 内 RA](https://github.com/BulkanDrivers/FEX-android/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp)。Android 平台补丁仍以本仓库文件和实际 ELF 身份为准。

本轮已按本地固定源码逐层核对：`Core.cpp:566` 先调用 `DecodeInstructionsAtEntry`，`:571` 再开始 IR function；[Frontend.cpp:1195](https://github.com/BulkanDrivers/FEX-android/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/Core/Frontend.cpp#L1195) 按 `InstSize` 找真实指令边界，通过指针区间拆分已解码列表，错位目标仍外链。后续 `Core.cpp:652–716` 保留 dispatch、decode failure、LOCK 和早停处理。raw-first 借鉴的是避免提前构造再销毁 IR，不能同时缩减语义检查或扩大接受范围；FEX 直接使用 GuestCode 指针的方式也不替代 SVM 的取指/变化合同。

FEX 的默认 pass 列表在该提交中为 x87 优化和 DFE，RA 单独插入；DFE 与 RA 的工作边界不能简单用 pass 数量与 SVM 对比。布局入口见 [IR 节点](https://github.com/BulkanDrivers/FEX-android/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/IR.h#L212)、[双线性存储](https://github.com/BulkanDrivers/FEX-android/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/IntrusiveIRList.h#L18) 与 [opcode payload 生成器](https://github.com/BulkanDrivers/FEX-android/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Scripts/json_ir_generator.py#L624)。

动态 RA 在 `RegisterAllocationPass.cpp:576–587,652` 按 CodeBlock 进行，但 SRA 固定映射可承载跨块 guest 状态；它不提供 SVM Function 所需的任意跨块 SSA interval 合同。缓冲复用也有明确的 `Core.cpp:540–541` reown/reset 与 `:806–809` 后端消费后 disown 边界，不能覆盖仍存活的 prepared 结果。当前实测 Android FEX 的 `ENABLE_JEMALLOC` 与 `ENABLE_JEMALLOC_GLIBC_ALLOC` 均为 OFF；源码默认选项不是实际构建，不能把差距归因给 jemalloc。配置核对记录为 `compile-cost-function-raw-first-design-r1/fex-allocator-build-check.json`。

FEX 的 assertions 编译开关与 SVM 的生产合法性检查不对等。SVM 的 `VERIFY_PLAN=0` 只关独立 audit，不能拿它关闭固定寄存器、宽度、scratch 或恢复证明。FEX 的 PF/AF DFE 也不能直接照搬：SVM 现有普通访存故障回归已经证明正常 CFG 活跃性不足以替代故障状态合同。

### 2.1 当前决策与优先级

FEX 值得借鉴的是减少整个流程中的实际工作，并让信息有明确的生产者和消费阶段。SVM 的多数阶段边界已经建立；下一阶段需要验证具体成本，而不是再照着 FEX 的模块名字重建一遍。

| 优先级与状态 | 路线 | 下一步交付与准入 |
| --- | --- | --- |
| 历史有限检查点 | C19 ThinLTO + PGO 组合 | 精确源码/profile/ELF与原证据保留；旧批次不因新检查点而重写，见 8.5 |
| C29 r4 独立正确性补证通过 | HIR AdvancePC 窄立即数累计截断与持久范围 | 32 条命令、74 个 raw 完成独立核验，已纳入 C31/cache52/freshPGO；历史失败保持，没有独立提速结论 |
| 当前有限检查点 C31 | 发布时查找既有 fault owner 的线性扫描＋匹配PGO | generic仅Core Block中位改善5.0072%；freshPGO相对同源码generic的Core Function/Block中位改善12.0351%/21.4596%；直接R6累计减少21.00%/33.86%。普通72全部指令斜率通过，FP耗时上浮限制保留；正式外部28点测量有效，21.0054×/13.1035×的数值门失败 |
| 首批已完成 N0 | 历史 C19 的 inline/源码归因及双方工作量核账 | 7291 个原样本、逐 PID 报告及真实计数已核账；当前 C31 另有新采样，不跨批转移热点覆盖 |
| C28 已拒绝；N2 继续条件研究 | decode 少构造可证明无用的 IR | 字面量原型仅少 19 个 FP raw 节点/模式，未通过 5% 门；先证明更大覆盖及完整语义，不改包装重测同一候选 |
| 条件进入 N1 | residual 重复计算 | 证明重复 Query/transfer 的实际覆盖，保留预算、失效、flags、类型和故障合同 |
| 暂缓 N3 表示重写 | 共同 IR 访问与表示成本 | PublicIndex 小表覆盖不足、已 NO-GO；两模式 refill 实测覆盖分散，尚不支持整套 IR 重写，详见 4.4 |
| 暂缓 N4 | 封口事实/roster 与目标准备中的重复物化 | 先证明同 owner、同语义、同稳定时点；输入生命周期和最终固定描述已经共享，不能再记收益 |
| N5 / C34已拒绝并撤回 | Function晚入口HIR重建 | 完整质量、内存检查及原56点均完成独立复核；Core正门未过，13文件和host/phone已精确恢复C31；设计合同和全部原证据保留 |
| C32 已拒绝；共同存储路线收窄 | DCE/RA 临时节点池与 facts/index PMR | 54 条质量命令核对后无新增失败；Core 正门失败，九个生产文件和一个测试文件全部撤回。下一项只允许有新生命周期机制及实际覆盖的提案，不原样重测 |
| 已撤回 | C24、C25、C26、C27、C28 | 预定性能门失败；C26 另触发 Block 负控。全部生产实验修改撤回，不重复测量求通过 |

C11/C9/C18/C20 随 C19 组合通过既定质量门，其单包没有因此被追认为稳定提速。C20 的 scratch 所有权清理和其他已完成机制保留原范围，C21 的整体后移仍受依赖限制。具体工作包、文件责任与停止条件见第 4 节。

### 2.2 历史 C19 采样支持什么

C19 的一次 CoreMark2000 普通采样中，Function/Block 分别有 3879/3412 个样本。N0 使用相同 perf.data、精确 ELF/DWARF 和按 PID 分组的报告完成了离线复核；下表已更新为该口径。它是整个进程树的 children 占比，存在嵌套关系，只用于决定继续剖开什么，不能求和、转成 compile 毫秒或直接当可删除比例。

| 已见调用层级 | Function | Block |
| --- | ---: | ---: |
| Translate | 55.73% | 41.65% |
| PrepareFunction / PrepareBlock | 34.86% | 25.81% |
| OptimizeFunction / OptimizeBlock | 11.92% | 5.23% |
| Describe 的主模式入口 | 8.01% | 6.85% |
| CompleteARM64Allocation | 7.93% | 7.09% |
| AllocateFunction / AllocateBlock | 6.55% | 4.83% |
| LinearScan::AllocateRegisters | 5.52% | 3.12% |

原报告使用 `--sort symbol`，跨进程合并叶样本后可能沿用先前 PID 查询后续 caller 映射。只改离线报告为 `--sort pid,symbol`，Translate 周期总数便与逐样本 API/raw 精确一致；没有改动或重采样。旧 215/229（5.54%/6.71%）是原报告的 error-callchain 数，不能全部归为物理 FP 展开失败；PID 分组后为 38/72，也不自动等同物理展开失败。[AOSP 的聚合与调用链构建顺序](https://android.googlesource.com/platform/system/extras/+/29f356ed8a0cb407d663c71ee5b47065d3bcf181/simpleperf/sample_tree.h)辅助解释了本批现象，未声称该主源与 NDK 二进制同版。旧报告和错误假说的失败记录保留，实际字段与独立核账见执行记录。

目前不能把剩余差距统称为“RA 很慢”或“严格检查过多”：准备、残余优化和目标工作都占有份额。已见 CAS4 父链主要属于 Scudo 分配器 bookkeeping/锁，不足以归因为 shared_ptr 或 HIR 引用计数。每次优化仍需从具体源码工作解释成本。

### 2.3 当前 C31 的新证据和执行次序

新的普通 Core2000 Function/Block 调用链已完成采集与离线归因，使用精确 `56fb68c9…` ELF、DWARF 和逐 PID 报告。两份记录分别为 3847/3256 个样本，记录到 Translate 父链的样本为 2104/1269 个；四类丢失均为零。报告仍有 61/94 个 error callchains，不能补造缺失父帧或把所有 unknown 归入编译。实际源码、PT_LOAD、libc/linker build ID 与原始 period 已核账；这是定位证据，不是新增配对性能点。

下表为沿已记录 Translate 调用链、按最近匹配阶段归属的互斥样本 period，占各自整份记录的比例。它既不是 coarse 计时桶，也不是整个编译 elapsed 的完整分摊，不与 2.2 的历史 inclusive 比例相加。

| 源码阶段 | Function | Block |
| --- | ---: | ---: |
| residual 优化 | 11.20% | 4.74% |
| decode / builder | 8.97% | 3.63% |
| emission | 7.44% | 4.90% |
| 物理分配 | 6.59% | 5.83% |
| target completion | 6.58% | 5.81% |
| 分配输入 | 5.90% | 4.92% |
| 封口 facts | 5.48% | 3.79% |
| publication | 0.86% | 1.63% |
| 编译链内未细分 | 1.91% | 3.81% |

编译链内 Scudo/CAS/malloc/free 的逐样本去重 union 为 Function 453 个样本、257565851 period，Block 284 个样本、161182070 period，分别占记录到 Translate 的 period 的 **21.54%/22.35%**。这部分分散在 facts、inputs、RA、Complete、emission、publication 和 owner 表；并非一个容器的成本。CAS-only 且没有 Scudo 上下文的样本另行保留；libc 只有精确 ELF 身份与符号，未作 libc DWARF 源码行归因。不能把这两个比例当可删除工作或编译毫秒。

源码进一步限定了可操作范围：workspace 的 clear 通常已保留 capacity，HIR `FindValueRecord` 已是 ID 直索引；ValueWorklist 与 DCE 的 owner 表在不同时间域使用，不能称为同时维护两份相同表。确实仍存在的是 mutable epoch 的 roster 填充/排序、指针 membership 查询、部分 row→pointer→row 往返，以及多个 owner 各自持有的短命物化。以下按路径列出工作包；C32 后的实际实施顺序与停止条件见 4.9，不按 P 编号机械排队。

| 次序 | 工作包与 FEX 参照 | 具体交付和将删除的工作 | 准入与边界 |
| --- | --- | --- | --- |
| P1，共同路径；C32 后降为备选 | 以明确生命周期减少临时物化；参考 FEX 的 compiler/线程缓冲复用 | 按新 allocator caller 表逐项建立 producer→最后消费者→释放点。C32 证明 scope 内池化与 facts/index PMR 的组合不足；只继续评估跨 scope 有界复用或净删除重复物化，分别列出实际命中 owner、峰值和保留容量 | 不重复建设已有 arena/workspace；facts 与两个 prepared 共存时不能借用会被下一次编译清空的 TLS 数据。跨 owner、跨可变/封口 epoch 或 final actions 的数据，不凭同类型强行共用。旧 C25 与 C32 不原样重测，Scudo union 不能为另一轮小包直接准入 |
| 随 P1 开展的可选清理，不独立启动性能包 | 在已知节点身份下直接消费位置；参考 FEX 的 NodeID/数组访问 | 将 ValueWorklist 内已经持有的 row 沿 helper 传递，删除 row→Inst*→Find(row) 的往返；原始 pointer 的首次 membership、删除 tombstone 继续保留。两文件未应用补丁已准备 | 这不是重写 IR 或替换成另一种 hash；不删除第一次安全准入，不对已删除 queued pointer 读 Id/Owner。覆盖不足时只记净清理，不包装成数量级主路线 |
| P2，仅Function，C34已拒绝 | raw边界先稳定，再展开IR | 历史341次reset/19040次HIR擦除/1244次replay只准入了结构实验；已实测的self late-entry机制未达到正门 | CoreFn中位0.996976（4快3慢）、CoreBlock1.016746（3快4慢），不把erase计数当净收益；所有实验源码撤回，Block未保留两阶段 |
| P3，依赖新准入 | 静态目标描述、Block 局部分配与最终证明的工作合并 | 从 P1 的真实调用者选一个同稳定时点、同语义的生产者，迁移全部消费者后删除旧发现/物化路径。Block 只处理块内需求；Function 保留 CFG/live-out 和跨块物理合同 | 已有静态描述共享、按需 spill、frozen actions 不再计收益。N4 的全量 facts/Describe 同访和整体 scratch 后移反例继续有效；没有可替代旧工作的明确证据就不进入性能矩阵 |

这份排序不承诺一个工作包足以到达 FEX。按当前配对中位比、仅把 FEX 分母视作固定作差距预算，Function 还需下降约 **52.4%**、Block 约 **23.7%** 才到 10 倍边界；这不是收益预测，实际采用仍须重新满足全部七对的严格 `<10×` 门。即便分配器相关工作全部消失也不能据采样比例宣称目标达成。若 P1/P2 的可删工作覆盖不足，应停止小包连测，转向经准入的 IR 身份/表示或编译对象设计，而非关闭生产证明、改变 RA 范围或少编译 guest。

当前证据：`compile-cost-c31-pgo-r1-profile/qualification-independent-review.json`、`offline-attribution/analysis.json`（`3d2fadef…`）、`offline-attribution/allocator-caller-union.json`（`857d2847…`）及 `offline-reader-closure.json`。最初资格收据记录的是当时尚未结束的 reader，最终关闭状态以后者为准。完整调用者、源码归属和未归属量保留在执行记录中。

### 2.4 C32 给出的限制与本次取舍

C32 实际修改了 DCE owner 表、LinearScan 慢路径 list 的分配来源，以及 Facts/BlockAnalysisIndex 的嵌套用途存储，没有改变优化、分配算法或关闭检查。54 条质量命令完成核对、没有新增失败，实际 FP 字节保持，单点 MaxRSS 未触发预定内存停止门；但七轮性能采用门失败：

| 负载与模式 | candidate/baseline 配对中位 | 更快 / 更慢 | 原预定门结论 |
| --- | ---: | ---: | --- |
| Core Function | 0.961723 | 7 / 0 | 改善不足 5%，失败 |
| Core Block | 0.991845 | 5 / 2 | 改善不足 5%且跨 1，失败 |
| FP Function | 0.944474 | 7 / 0 | 负控负载，不能替代 Core 正门 |
| FP Block | 0.968111 | 6 / 1 | 跨 1 |

该组合已拒绝，不保留为默认实现，也不进入其 PGO、普通 PMU 或新 FEX 对照。它只说明这包具体实现未达门，不能把总效果归给其中某个容器，也不能据此否定所有存储路线。尤其 C32 每个最外层 scope 结束都释放 pool 的全部 chunk，实际只跨 scope 保留 resource 包装对象；它没有验证 FEX 式跨编译请求的 buffer 复用。后续若设计有界复用，必须作为不同机制重新准入，不能改阈值后重贴旧数据。

另两项只读审查也收口：固定 FP Block 存在六组不同起点的尾部重叠，但没有公共入口、RA live-in、flags、frame、fault 与 SMC 合同，不能直接跳进旧代码中间；相同 guest 字节还可能已被不同常量上下文特化。既有内存 exact-entry cache 已工作，`SVM_JIT_CACHE=""` 关闭的是磁盘缓存，不能据此声称没有内存代码复用。另一项 sealed-value visitor 的可删工作覆盖很小，不能把全部 visitor 调用链算成可省。

C32 收口时，主机曾恢复 C31 的 2418 个源文件、118 条退役路径、原 typed PGO 配置和 16 个精确 ELF；手机 16 个部署角色统一为选定 C31 PGO，其他九个参照/输入保持原 SHA。此前七个仅部署角色原是历史二进制，此次统一部署不等于恢复它们的旧身份，也不等于运行了 16 个角色。原实验、压缩主程序、十份源码 afterimage 和恢复收据均保留。此后的 C34 实验现场另见 4.10，不把这段历史恢复结论当成当前构建状态。

## 3 保持现有阶段职责

```text
decode 和可变 IR 改写
  -> 封口 IR 与唯一 SemanticFacts
  -> AllocationInputs 和物理分配
  -> target completion 与最终生产证明
  -> 冻结 AllocationView 和 UnitActions
  -> emission 和发布
```

`compile/builder` 继续负责构造期局部规则；`compile/passes` 处理必须依赖完整 IR 的改写；`compile/analysis` 拥有封口事实；`compile/allocate` 拥有物理策略与事务；`backend/arm64/compile` 拥有目标描述、选择及证明；JIT 只消费结果。

优化尽量修改现有源文件，不增加另一套简单块编译器、持久子 plan、配置开关、全局分析表或 proof-token 框架。更改公开接口时，同批迁移调用者并删除旧签名和无调用的辅助实现。

已经完成的工作不再计入待交付收益：SSA/flags/liveness 融合缓存、按需 uses/index、同型 FPR Alias 消除、相同 pool 的 scratch 规则复用、首块 GPR residence buffer 转移，以及已有家族门控。

### 3.1 可以前移的工作与必须保留的依赖

| 阶段 | 可以在此完成的工作 | 继续留在后续阶段的工作与原因 |
| --- | --- | --- |
| 程序构建与静态 schema 初始化 | opcode 的参数布局、类型/effects 等不可变属性；C18 就属于此类 | 每条 guest 指令的值、控制流和物理位置依赖运行时输入 |
| decode 与 typed builder | 当前指令语义展开、局部常量/恒等折叠、可证明的 guest-state 转发；成功提交时交接原始指令来源与范围 | 未接受的 lookahead 不能更新最终编译范围；IR 家族和使用关系会被后续改写，不能直接作为封口事实 |
| residual passes | 依赖完整当前 IR 的 flags/value/DCE 协作、跨指令改写与通知 | 此时 IR 仍可变，不能借用 sealed facts 或物理分配结果 |
| 封口分析与 AllocationInputs | 一次建立权威 SSA、用途/隐式根、CFG 活跃性；生成目标静态要求，复用已有 summary 门控 | decode 不能预知重写后的 use、跨块 live-out 或最后的目标选择 |
| 物理分配与 target completion | 分配范围内的 spill/coalesce、具体 publication/memory/vector/scratch 选择，完成后核实最终物理合同 | 物理编辑、重试、固定 home、宽度和故障恢复必须按最终实际结果验证 |
| 冻结结果、emission 与发布 | 消费现有 allocation/actions，形成机器码与恢复/入口记录并按既定顺序发布 | emitter 不重建另一套 IR 分析；不得以提前分析替代 SMC、fault 和 retirement 的发布合同 |

可变期的局部信息与封口后的权威事实分属不同生命周期。前移一项工作时，必须同步删除后续重复生产者，并覆盖改写、取消、重解码和重试；若需要再维护一套失效系统，先停止该候选。

## 4 下一阶段工作包：先找足够大的工作，再实施

C19 之后的 C24–C28 和本轮 C32 均未产生可采用的单包改善，C31 的 fault owner 索引及匹配 PGO 已按原范围纳入当前检查点。下一阶段不以“又少一个容器、一次扫描或一次 malloc”作为进入完整性能矩阵的充分理由。FEX 的启发应落实为少构造 IR、少求解同一问题、少搬运最终结果；每一项先找到当前热点中的实际调用和覆盖范围。

本节的 N0–N6 是路线编号，C 编号标识实际原型。N0 首批已完成，N2 的 C28 已测量拒绝；其余按证据选择，没有预计收益承诺。

### 4.1 N0：已完成的 C19 源码归因和工作量核账

**此项已完成，不重复采集历史检查点。** 已使用 C19 两份原始 `perf.data`、精确 `5a94db9e…` ELF 和其 DWARF，逐级核对 build ID、MMAP/load bias、进程树及 sample period，并展开 inline frames，按实际编译调用链区分 decode、residual、sealed facts、输入描述、RA、target completion、emission 和 publication，保留无法归属的样本。报告的 PID 聚合问题按 2.2 节处理，unknown JIT 与 guest 执行不能划入 compile。以下是已用方法和后续归因须继续回答的问题。

重点回答四个问题：

1. 大函数中的时间落在什么实际源行：IR 访问/类型检查、bounded known-bits、查找与通知、目标 matcher，还是容器物化？ThinLTO 的符号归属不足以单独回答。
2. 同一 sealed 输入被重新解释几次？逐项列 producer、owner、最后消费者、是否经过物理改写；同名循环不等于重复计算。
3. 哪些节点已经分配、登记 uses，随后在构造期立即被折掉？哪些 residual 查询确实在输入未变时重复发生？已有候选门控、worklist 和按需分析应计入现状。
4. Function 额外成本来自多少真实块/指令、CFG 需求、region/入口处理和重复工作？不能把 Function 与 Block 请求数或 FEX IR 构建数视作同一单位。

**并行整理已有计时边界。** SVM translate 与 FEX 观测线程槽都先列清包含项、未包含项、线程历史和实际编译覆盖。FEX 普通 CompileBlock 已覆盖 decode、passes/RA、后端、代码复制与 I-cache 刷新等；不能把历史 47.257 ms 全部解释成“只统计后端”。先使用现有 raw、转储和统计补全核账；缺失项明确留空。只有现有证据确实无法回答决策问题时，才预先定义一次现有诊断运行，不新增长期计时/采集框架。

实际已交付 `compile-cost-next-stage-source-attribution/analysis.r3.json` 和 `compile-cost-next-stage-unit-ledger.json`。前者报告采样权重、源码位置、调用链资格和未归属量，后者报告双方边界、原始工作量与尚缺证据。它们是历史诊断产物，不是当前性能点；正式外部28点另按6.1–6.2验收。

**退出条件：** 选出一个覆盖实质热点、能说明净删除工作及质量合同的机制；或明确该路线证据不足并转向另一类热点。没有覆盖证据，不继续启动一串容器替换实验。整个进程树的采样占比不乘 translate 毫秒，嵌套 children 占比不相加，也不与旧版本的归一化占比相减当收益。

**N0 首批已完成的证据。** 全部 7291 个原始样本保留，3961 个主程序 frame 的映射与 ELF 可执行 PT_LOAD 逐项相符，3479 个不同地址完成精确 inline/source 展开；没有二次扣 return PC，也没有用 nearest-line 猜测代替无行号项。PID 分组的 CLI、API 和 raw Translate 周期总数精确一致，报告错误率修正不记为性能改善。

源码归因将多个 libc++ 搜索热点分开：ValueWorklist::Find 的逻辑叶调用者占整个进程树 Function 1.30%、Block 0.80%；Function DecodedEnd 约 1.91%，Block FaultCatalog::Add 约 2.73%。后两项对应的旧失败候选仍不重开；这些占比均不是可删除时间。直接用 `Inst::GetBlock()` 消除 DCE 成员表的路线已经否决：`Add(p,p)` 可使 p 重复入队，第一次销毁后第二次读取 owner 是 UAF；仍需在解引用前拒绝退役身份。

另用现有 `SVM_IR_DETAIL=1` 与 PROF2 各执行一次 CoreMark Function/Block。成功 append 事件为 147856/85417；各自发布 ID 槽计数为 82834/69283，算术差为 65022/16134。append 计数不覆盖所有可能的 raw 分配，`ir_insts` 来自 MaxInstr* 的 ID 空间；这不是“构造后立即删除”的精确数，也不是可省比例。Function 原运行因读取命令转义错误漏取结果，原失败保留，只追回现有 raw，未重跑；其后验是延迟恢复，限制明示。Block 仅执行原计划未执行的那一次。两次 exit/CRC 和输入、条件、源及 native 身份均已复核，诊断纳秒不进入性能表。

后续已经匹配了既有 FP Block 的真实阶段、opcode 与 guest 来源，形成 4.3 节的 C28 候选。raw decode dump 已在 builder 局部折叠之后，final dump 又在发布之前，单靠它们和两个总计数仍不能完整分解立即折叠、放弃解码、residual DCE 与状态消除。暂不新增计时/事件框架，也不因这些差值直接重写整个 IR。

### 4.2 N1：减少残余优化的重复求解，Block 与 Function 分别处理

借鉴 FEX DFE 的协作式清理，但 SVM 已经具备 flags/value/DCE 通知和候选队列；本项目标是找出队列背后仍被重复执行的昂贵计算。

- **共同路径：** 在 `passes/value_worklist.cpp`、`values.cpp`、`analysis/value_bits.cpp` 中，分别归因初始 roster、lookup、反向用途、Query 和反馈。当前 `BeginValueCleanup` 已检查候选、初始 queue 已按 opcode 筛选，不能再次把“只处理候选”列为新方案。不新增第二张 IR 图或跨 epoch 事实缓存。
- **Function 条件候选：** `passes/flags.cpp` 的循环已经记录 `flag_changes`，但 `FlagFlow::ComputeLiveIn` 每次仍对完整 RPO 重述所有 block transfer，然后从零求不动点。可以审查在同一 FlagFlow 生命周期内只重述已变化块的 transfer，同时继续清零全部 live-in、运行原有全图求解；不保留上一轮 live-in 作为下界，删除读者可能降低环上的需求。
- **Block：** 保持现有局部 FlagFlow；它不构建 Function states 或 CFG，不为统一接口引入函数级表。Function 候选以 Block 为负控；单块 Function 仍保留其真实入口和隐式根合同。

上述 Function 候选目前仍为 **HOLD**。C19 普通报告中 ComputeLiveIn 的 inclusive 仅占整个进程树 0.57%，不能把 EliminateFlags 的 3.96% 整体当可删除求解成本。只有 N0 的 inline 归因和迭代覆盖证明其值得处理才进入实现。

后续精确 inline 审查也没有发现可立即采用的大块 Query 删除：整个 Query 路径占进程树周期的 Function 1.1695%、Block 0.4747%，其中真正处于 SimplifyValue 的部分更小。FoldWidth 与 FoldConditional 在同一规则中互斥，SimplifyWidth 不递归分析；不能把它们当重复三次查询。Value drain/CSE 后的 clean width seed 存在具体重复子例，但现有样本没有根身份和跨阶段覆盖，暂不为此开性能实验。简单 memo 会改变原 32 次访问预算及可获精度，也不能作为等价删除。

**实施前证明：** 所有相关 Change、Erase、DCE、branch-only 和局部 label 处理均完整通知变化；脏列表在清空前消费；首次 transfer 完整建立；不可复用 label epoch 的临时 demand。改写次序、flags/fault 可观测性、终止预算和生成代码保持。若必须新建持久 CFG/脏表，或缺少完整通知，停止该候选。

### 4.3 N2：在 decode 构造期少产生节点

FEX 的局部操作构造与按 opcode 分配 payload 值得参考。SVM 已有 typed builder、guest-state 转发、同型 cast 折叠，以及 guest 指令提交时的立即数/width/conditional 简化；本项只针对 N0 证明仍然大量出现的“创建后立即消除”。

| 工作 | 实施位置 | 准入与保留条件 |
| --- | --- | --- |
| 能在追加前确定结果的局部规范化 | `builder/ir_builder.h`、`ir_builder.cpp`，复用 `rules/value_rules.*` | 输入类型、FlagsIntent、用途窗口和拒绝语义均已确定，直接返回已有值或规范形式，避免实际节点/Use/UnUse；先列动态频次 |
| 需要完整 guest 指令才知道 flags 观察的规则 | `builder/instruction.cpp` 的提交边界 | 保留原延迟观察，不能为了早折叠忽略稍后附加的 pseudo flags；不扩大 flags 不明时的折叠权限 |
| 同一 guest-state 的局部转发 | `builder/guest_state.cpp` | 优化现有状态表中的重复查询；helper、MayFault、部分寄存器写、边界清空及 memory 语义保持 |
| 完整 IR 或跨块依赖的规则 | 继续在 `passes/` 与封口 `analysis/` | 不在 decode 重建 CFG、SSA/liveness、spill 或最终宽度证明 |

**交付判断：** 报告实际避免的节点、参数读写和生命周期工作，同时核对最终 IR、mapping/actions 与机器码。若只是把同一计算搬到 decode，或需要维护另一份全局摘要，没有净删除工作则停止。机器码改变的规则与纯 compile 存储优化分开验收。

**已取得的真实 roster。** C19 的 42 个 FP Block 实际发射单元均与唯一 raw/final invocation 配对，真实 Inst 行为 3077→2260。主要净变化涉及 StoreUniform、SaveFlags、And、LsrImm，同时 SetHostGPR/FPR 增加，不能把总差当作立即折叠或死写数量。LoadImm 为 83→42，净差全部来自 U64（73→32），其他宽度不变。源码追踪确认 frontend 会先 ToValue 物化部分字面量，再由现有立即数规则还原；这提供了具体候选，仍不能将 41 的净差全记为可避免节点或据 FP 覆盖预测 CoreMark 收益。

**C28 原型与文件责任。** `frontend/x86/decoder_alu.cc` 在 RHS 原读取位置尝试保留 `Src` 已规范的字面量；`decoder.h` 声明局部 helper，并让算术 helper 接收已有 DataClass。`compile/builder/ir_builder.h/.cpp` 仅提供复用现有 `FoldIntegerImmediate` 的纯规范化入口，继续使用 builder 捕获的 feature 与 target capabilities。没有增加 pass、配置、缓存或第二份规则。

准入限定为 op0 寄存器、原始 op1 为立即数、32/64 位、无 exchange 且完全没有 LOCK 前缀；覆盖 ADD/SUB/CMP、AND/OR/XOR/TEST。ADC/SBB、INC/DEC、NEG、窄位宽、内存目标及 LOCK 继续走原 Value 构造。规则拒绝、功能关闭或 target 不支持时，在原位置回退 ToValue；左操作数访问、flags、写回及故障顺序保持。所有 union 读取按 tag 分支，不直接把 DataClass 当 Value。

原型曾在 `tests/cases/decode.cpp` 增加两个 `[literal_operand]` 用例，以真实 x86 字节检查规范化、类型、flags、32 位写回、排除项与回退顺序，随拒绝补丁一同保存在结果目录。它们是 decoded-IR 结构检查，不能替代既有执行语义门；其中 HIR fixture 固定 fast=true，环境矩阵不等于新增用例内部覆盖 HIR slow。

R1 首次构建因新增测试中 `VAddr` 未限定命名空间而失败。R2 修正后，新测试又过度要求 32 位 AND0/OR-1 写回必须保留 ZeroExtend；既有 width 规则允许它成为精确 U64 常量。R3 仅严格修正这两项 oracle，其余断言保持；四个生产文件始终与 R1 逐字节相同。R3 源码 `765a990d…` 已冻结，16 目标构建和六阶段质量门实际完成：724 行、894604 项断言包含原样受控的 22 项 R6 历史失败，没有新增失败；1346 项 native checks 通过，FP48 的 PC、顺序与实际字节均等于 C20/R6。失败记录保持原身份，没有性能点被替换。

静态审计同时记录了代价：整个 `.text` 增加 816 B，ArithWithFlags 的入口栈增加 48 B、DecodeAddSub 增加 16 B，新增 helper 的回退路径增加一次 ToValue 调用。实际七轮 56 点完整结束并独立重放，Core Function/Block 中位比分别为 0.996791/1.003239，四组范围均跨 1，两个 co-primary 均未通过原 5% 门。C28 已拒绝，五文件实验修改恢复 `8401918b…`，不凭少构造节点或质量门通过保留新入口，也不补点、训练 PGO 或重包同一候选求通过。

原型的实际 FP roster 已核对：Block 的 raw Inst 3077→3058、LoadImm 83→64；Function 的 raw Inst 2548→2529、LoadImm 154→135。两模式各只避免 19 个 U64 LoadImm，其余可见 raw 节点经 ID 对齐相同，最终 roster 分别保持 2260/1924 行，机器码相同。这个有限覆盖不能外推为 CoreMark 构造总量或 CPU 收益。

**下一项构造路线的证据与限制。** 原 FP raw StoreUniform 318→final 3 中，191 个变为固定 home 写，124 个消失，不能把 315 全算作死写；其中 119 个直接在 DSE 阶段闭合，另 5 个属于后续 sink。原来已有的 SetHostGPR 另有 34 个被 DSE 删除。builder byte facts 当前只负责值转发，pass 中已有 ordered pending stores、部分覆盖及异常恢复；不能另造第二份状态图或把现有 sink 再记一次收益。跨 guest 指令延迟发布仍为设计阶段 HOLD：须先闭合观察前 flush、partial overlap、故障恢复、取消/异常和 terminal 合同，并证明真实 decode 覆盖。固定 FEX 每条 guest 指令前的 SRA flush 更限定了参考范围。

另一个更大覆盖的具体候选是 Block 路径的 AdvancePC 构造。现有 HIR 已合并，Block 直接追加；当前 FP Block 591 个 marker 按现有七 opcode 规则可合并 400 个，Function 已有 193 个，再合并为零。但审查发现 HIR 现实现沿用窄 Imm 类型存放累计值，会截断 guest 字节数。C29 已在原生产负控中复现两项语义失败，独立加入加法溢出和同类型 round-trip guard，无法表示时保留新 marker，不强制拓宽。修正后 Block 的候选上界为 398 个；它仍受 flags-region、访存故障、retirement、解码范围和构造窗口等合同约束，保持实现 HOLD，不能只凭零发码 marker 数宣布安全或提速。

C29 属于正确性前置修复，没有独立提速结论。其 r1 的 16 目标构建和修后语义门完成，严格 FP 字节门因 Function 一处变化而失败，原结论保留。实际源码与缓冲审查将变化定位为准确 guest end 触发晚入口重新切块、移除重叠循环体。随后补证把 cache 升至 52，并明确普通 PageFatal 的 entry-PC 合同、懒编译 owner 与私有 Function 内部入口，修正了新增测试中过强的指令 PC 和冷块存在性前提；r2/r3 的原失败均保留。r4 在全部生产源码与 r2 相同的前提下完成 32 条新命令，74 个 raw、实际 argv、退出、前后身份和全部结果已独立核验：真实 x86 0/1/17 次与故障状态、旧51 disk/AOT 拒绝、retirement/latch 以及 FP 对 r1 的发射字节保持均通过。这里没有证明普通访存的任意指令 PC/安全 retry，也没有改写 r1 的严格字节失败。该修复和 cache52 已纳入当前 C31，后者完成匹配 PGO、54 质量命令、直接原 R6 与普通 PMU72；组合结果不倒推为 C29 单包性能收益。

**C30 的后续准入收敛：当前七 opcode 方案 NO-GO。** 源码能够构造两个具体窗口变化：`CMP→SETcc→TEST→Jcc` 会失去原 branch-only 资格，`FCMP→MOVAPS→JP` 则可能新获得 FCMP recipe；仅把当前 guest 的 ReadFlags 加作停止条件仍不足，因为还涉及下一条 guest 的 marker。这些是源码反例与离线重放，没有冒称实际机器码或运行回退已测得。加强 effects 与前一 guest 边界的计数只剩 174/591 条可删，仍有九个窗口变化；最保守的流式前缀方案只删 17 条，不为它启动性能实验。实际 fresh builder 的锁内生命周期可以闭合，未发现生产 UAF；Function-only 的 edge token 与 strict internal suffix 也从独立 Block 的阻塞项中剔除。后续若要继续，须先迁移 marker 的实际语义消费者并证明质量，而非重复启用同一个七 opcode 开关。

### 4.4 N3：压缩共同 IR 访问成本，先确定表示瓶颈

FEX 的 `OrderedNodeHeader` 保存 payload、next、previous 三个 32 位相对偏移；opcode payload 在另一线性区按 GetSize 分配。它仍保留双向链接，并非“无链的紧凑数组”，也不能把 OrderedNodeHeader 与 IROp_Header/IRHeader 混为一谈。SVM 已有 TLS arena/freelist 和 HIR 池；新目标不是再加内存池。

当前 ARM64 实物审计为 `Inst` 对象 94 B、arena 对齐槽 96 B；其中固定 `Arg[5]` 为 60 B。`Operand` 占 header 加两个 Data 共三个物理槽。小幅删字段未必跨越 16 B 的槽边界，不能直接宣称减少每节点分配。Lambda、Params、原始/部分构造状态、无效 view/type 和失败诊断均需有等价表示。

仅当 N0 显示 IR 解引用、解包或内存访问是实质共同成本时，才选以下一个原型：

1. **访问路径原型：** 以现有静态 schema/typed operation 为依据减少重复解码。公开的原始 IR 入口与负测继续检查；不能新增任意 unchecked getter，也不能把最终物理证明改成 schema 推断。
2. **表示原型：** 审查按 opcode 实际参数大小保存 payload、稳定节点身份与热字段分布。先在现有 `ir/` 边界内迁移一种受控表示，列出所有合法及非法状态的可表示性、真实 slot/stride、Use/pseudo/owner 和序列化影响，再扩展。不得让长期双表示、转换桥或影子 IR 成为常态。

`Inst::Owner` 改为直接借用稳定 Block 在源码层面可行，但 owner 本身只有 12 B，Inst 指针字段不变，不能作为数量级主路线。只有实际内联、帧和查询成本支持时才独立处理。现有紧凑表示审计尚未证明可直接实施的小方案，本路线保持条件研究。

已经排除一个具体小方案：PublicIndex 自身在两模式各仅 14 个样本，占进程树周期 Function 0.3617%、Block 0.4109%；它与 GetArg 的去重 union 也不能全部当可删。静态生成 physical-offset 表只能删除部分元数据读取/算术，覆盖不足，不进入实验。后续若采用已有 simpleperf 做一次 refill 诊断，须先证明 CPU4 事件支持、有效计数和调用链覆盖；backend stall 单独不证明内存瓶颈，也不从 sizeof 外推收益。

CPU4 的实际事件枚举和一次非 guest 能力检查现已完成：通用 L1D/L2D refill 与 backend stall 可用，和 cycles/instructions 同组均取得正计数，time_running 等于 time_enabled。L1D refill 的 read-only 专项事件不支持 CPU4，不能用其他核的支持情况替代。随后已对选定 C19 各运行一次 Function/Block Core2000，四事件 user 域 400 Hz 共 4823 个样本完成独立 event 分账、API/raw/逐 PID 报告核账、PT_LOAD 与精确 inline 映射；均为 CPU4 用户态，lost=0。

Function 的 L1 refill 中，DecodedEnd 逻辑路径占该事件 period 的 12.01%；Block 的 L1 refill 中，FaultCatalog::Add 占 18.36%，L2 又覆盖 allocator 和 emission 等路径。这是分散的有限正向覆盖，尚不支持整套 IR 布局重写，也不复活已拒绝的旧候选。每个比例只用本事件的分母，不能相加 inclusive contexts、转换 compile 毫秒或直接比较旧含 kernel、2000 Hz 的占比。采样 precise_ip=0，不是实际 load 地址或 miss 延迟；本次 record 没有 fresh time_enabled/running 读数，不能把先前能力检查的无 multiplex 当作本次新证明。表示路线继续 HOLD。

**停止条件：** 收益只体现在 sizeof、没有减少真实访问/stride；破坏 raw IR 拒绝、sealed/compiling/ordered 状态；需要更大的长期旁表；或无法集中删除旧入口。没有这些证据，不进行整套 IR 重写。

### 4.5 N4：封口事实与目标结果只物化必要的一份，当前暂缓

**现有 roster 的融合候选。** `analysis/facts.cpp::PrepareBaseFacts` 已在一次基访中形成 summary、flags、SSA/liveness；后续 `PrepareIndex` 在确实需要时才调用 `block_index.h::BuildImpl` 建立 instruction roster、顺序/稠密资格。后者已经复用 summary 和 pseudo，不能称为重建整套 facts。N0 若确认这两次访问的动态交集足够大，可研究在已明确需要 index 的输入中由同一个 BlockAnalysisIndex 接收 roster，退役后一次物化访问。

不得为了融合而让所有单元强制建 index/reverse uses，或增加另一份 instructions 镜像。保持 sparse/manual IDs、外来 definition、不可达 Function 块、原始 occurrence 与失败重试语义，报告新增保留容量与少访问的实际行数。`SealIR` 本身是 owner 上的 O(1) 封口，不存在可顺手删掉的逐节点 seal 验证扫描。

FEX 后端消费 PostRA 结果的边界，SVM 已通过 frozen AllocationView/UnitActions 实现。`AllocationState` 与 `AllocationInputs` 实际保留到 Complete/Finalize 结束，固定语义描述也已有 feature/contract-key 复用；“输入过早销毁，所以重新计算”不是当前问题。

把整个 base facts 与目标描述直接同访，本轮源审也已否决：生产路径在 OptimizedUnit 构造中已经强制准备 SSA/liveness，Describe 再请求只是命中；改为 lazy 又会改变现有 const Facts 可观察合同。ObserveInstruction 对坏形状可能只记 SSA=false，不能据此立刻调用会抛异常或追 source 链的 target helper。需要完整 flags、homes、use-ends 的消费者仍有后置依赖，不能为消除表面重复遍历改掉这条生产边界。

只允许继续查两类工作：一是在现有 owner 中重复提取同一静态属性，二是在同一物理状态上重复进行只读发现。最终 fixed/width/scratch 检查依赖实际 mapping、coalescing、transfer 和 target edits，不能以早期输入替代。`memory_feeders` 是特定 producer ID，不是全量 memory roster；稀疏 GPR/width 输入也不能直接代替最终全指令访问。

C21 只有收缩到**原时点**的只读共享才可能进入；把 scratch 尾准备整体后移仍缺跨块 recolor 语义证明。当前可复用的 publication 发现 self 只有 0.10%/0.15%，尚无可直接删除的大块重复重建。本项不再排为立即实施的 P1，只有新的源归因证明实际重复工作后才重新准入。

当前 C31 的另一次源审也排除了把冻结后的查询泛称为大量二分搜索：`AllocationView::FixedGPRDefinitionAt` 已直接读取 `gpr_placements[id].fixed`，scratch 已有 block span、单调 cursor 和 ID 倒退回退。仍可研究的只是 UnitActions 中部分当前指令 Residence/Consumer/Publication payload；随机 producer、FCmp、MemoryTransfer 和复合 key 查询不能强行顺序消费。新采样中确认为叶 inline lower/upper_bound、UnitActions payload 且有 JitTranslator 父链的去重上界，Function仅7样本/3995589 period（全记录0.1835%）、Block2样本/1144055（0.0620%），还混有必须保留的随机查询。该游标扩展源上可行，覆盖不足，暂缓实现，不为它独立启动性能矩阵。不能把已是直接索引的 fixed-definition 样本也记入收益。详见 `compile-cost-c31-frozen-action-consumption-review.json`。

涉及 `arm64/compile/inputs.cpp`、`lower.cpp`、`compiler.cpp`、`scratch.cpp`、`width_contracts.cpp`、`certificates.cpp`。如能删除已有局部 buffer，先列最后消费者、异常/重试 rollback、append/sort 失效点，迁移同批删除旧复制接口，不增加长期借用或新的 plan 层。

### 4.6 N5：Function 范围与编译单元策略单列

`function_decode_frontier.*`、`function_region_decoder.*` 的范围维护只在 Function 有意义。新的权威范围必须代表已成功提交的 guest 指令，涵盖 split、重解码、异常取消、SMC 和失效；decoder.pc、lookahead、node_size 不能直接替代。既有 FindSplit/DecodedEnd 小候选已被拒绝，不重测求通过。先证明现有长寿命 owner 能交接结果，再决定是否删除求和。

**新的结构方向是减少整段重建。** 固定 FEX 在 raw decode 与块边界拆分完成后才展开 IR；SVM `FunctionRegionDecoder` 的 interior target 与 newly discovered late entry 两条路径，可能先调用 `ResetDecodedBlock` 擦除已有 HIR，再重新 decode。若覆盖充分，改动应由既有 region/frontier owner 暂存本次 raw 解码结果、先拆合法入口，再对保留指令做一次语义展开。同步退役这两条路径中可替代的 HIR 擦除与重做；不能增加一次 bytes 预扫描后仍原样解码，也不给 Block 增加两阶段或第二套 CFG。

已用临时 generic 诊断各运行一次 Core2000 Function/Block，真实 PID/argv/env/exe、退出、CRC、六条件及全部18个原始文件完成独立核账。Function 的 frontier/late-entry reset 分别为237/104，均成功，实际擦除8747/10293个HIR节点；正常 Function 角色 accepted 为27989，两个 replay 角色为774/470，另有347次 standalone fallback。1244/(27989+1244)=4.2555% 是本次 Function 角色内的 accepted 次数比例，不是唯一 guest 指令或可节省耗时。Block只有15612次standalone accepted，所有reset/replay为零；两臂角色计数分别与原有decode attempts29580/15612闭合。正常 Commit 也调用 `CancelGuestInstruction`，这里没有据其调用数推断回滚，也没拿ID槽位或marker数替代实际擦除。

随后为补齐上述聚合计数缺少的 PC/形态证据，又预先冻结了一次单独的 Core2000 Function 诊断。它没有增加性能点：两份 cpp 临时日志补丁、generic 主程序 `9efff928…`、源码 `ce9ca200…`，不使用旧 PGO，不改变原解码决策或增加 GetPointer。实际 5247 条 decoder stream、5240 个 region、29580 条 instruction 事件均闭合，只有一个实际 Linux TID。104 次 self late-entry 的 owner/target/marker 域、原始流和 prefix replay 全部经另一份独立 raw parser 核对；对应 62 个不同三元组、1890 次原始流指令观察、470 次 prefix replay。终结均为首个直接 Jcc，103 个 rel8、一个 rel32，目标对齐且严格位于 `start < target < terminal_pc`。这里的 10293 仅为本次 reset 前日志节点和，没有重新包装为本次精确 Erase 计数或可省节点。

这回答了真实工作是否落在该观察类别，但没有完成生产准入：流中还包含 SIMD、CMOV/SET、DIV、CMPXCHG 等，不可只支持简单 GPR 后声称覆盖全部。普通 raw 解码成功也不证明当前 handler 必能继续；共享支持/终止选择、placeholder 的 else→then 顺序、预算和 pending roots 仍须等价。旧 outer frontier 的依赖与 call-return provenance 来自已经生成的 IR，不能用 raw `next_pc` 替换；首个具体迁移范围可以是 self late-entry，237 次 outer 路径保留原合同。本次只记录 outer attempt/后续 replay，未记录显式 reset 结果，故该结果列保持 unknown。

诊断的原 6.17 MB stderr、其余八份 raw、全部 PC roster、冻结归约器与精确主程序压缩件保留。采集、形态均完成独立复核，临时源码已经撤回，原 C31 的源文件、配置文件与 16 个 ELF 精确恢复；手机原 25 个角色/输入未改，独占诊断目录十文件在 SHA 核对后回收。证据为 `compile-cost-function-raw-shape-diagnostic-r1/{capture-independent-review,shape-independent-review,root-restoration-closure}.json`，详细实现前置见 4.9。

append 为进程全局差额，虽然角色和恰与全局147841/85482相等，仍未证明单编译线程/不重叠，故其归属列保持 HOLD。19040次擦除包含仍需重新构造的保留前缀，不是完整可删量；当前记录也没有 raw-record 布局或整个 Function 的暂存峰值。证据只准入后续源码可行性设计，尚不支持采用全量 raw 两阶段或承诺收益。临时补丁已撤回，3cb源码、原typed PGO配置及16个原生ELF精确恢复；私有手机诊断目录在全部19文件与host/ELF SHA核对后回收。诊断耗时不进入性能表。

只有真实重复工作足够支撑结构实验，且能证明 raw 内容的安全读取、SMC 依赖与暂存生命周期，才进入原型。raw 暂存限于本次 Function 事务，取消或失败时同时失效；不扩大已提交 guest 范围，不改变 PageFatal 的现有恢复点、call-return provenance、已发布 owner 和持久缓存合同。覆盖很小或需要另一份长期图/cache 时停止。修改集中在现有 `function_region_decoder.*`、`function_decode_frontier.*`、`runtime/frontend/x86/decoder.*` 及对应真实 x86 frontier/fault 用例。

编译单元聚合是另一条研究路线：FEX 可构建多块单元而 RA 仍按 CodeBlock 处理，SVM 的 Function 则有跨块 allocation、live-out 和恢复合同。不能用默认 Function 改 Block、缩小覆盖或增加 cache hits 记作 compile 算法改善。若后续确需研究相同 RA 范围下的代码对象/发布聚合，单独冻结入口、链接、retirement、SMC 和缓存身份，并完整比较运行代码质量，不新增第三套 allocator。

本路线目前仅完成 Function 的条件诊断与源码设计准入，没有采用 raw 重写；当前尚无获证的共同大机制，不用新一轮小容器实验填补空缺。没有外部工作量核账，不根据“2417 / 439 / 282”三个异质计数推算每单元优势。

### 4.7 N6 / C31：已实施的 fault owner 索引与历史准入

这是旧 C24 bulk 之外的新机制。C24 合并同一 owner 的后续精确站点调用，却没有删除新 owner 首次登记时对全部 live Allocation 的搜索。C31 之前的 `FaultCatalog::Add` 在每次 Add 中沿 `head/next` 以 SC 读取寻找 `(module, owner_start)`；连续同 owner 通常命中 head，新 owner 和交错的旧 owner 则可能遍历很长的链。

证据已经到达循环本身：C19 普通 Block 精确归因中，循环行 133–134 为 87 个样本、占全进程 period 的 2.5576%，含循环入口行为 91 个/2.6739%；Function 仅约 0.2071%。另一份四事件诊断中，Block L1 refill 的同一循环行占该事件 period 的 17.4123%。分母和批次不同，不能相加或转 compile 毫秒；fresh miss 与旧 owner 深命中的实际比例、live-owner 数仍未知，也不承诺达到 5% 收益门。

FEX 在固定版本的 [LookupCache.h](https://github.com/BulkanDrivers/FEX-android/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/Core/LookupCache.h#L55) 中以键访问 guest→host 映射，Core.cpp 通过已知 JIT header/tail 读取当前 code object 元数据。这里借鉴的是按稳定身份定位已知结果；FEX 的 GuestToHostMap 不是 SVM 的信号故障 catalog，不能照搬其读写锁或恢复协议。

已采用一个私有有序索引 `(module, owner_start)→既有 Allocation*`，只在原 `writer_mutex` 内使用，删除 Add 的线性搜索；保留真实 head 的同 owner 快路。索引不复制 FaultEntry/Snapshot，不供信号 Reader 使用，不持有 module/IR/executable 的所有权。所有增删都在同一 FaultCatalog 内闭合，不建立跨模块失效回调。容器选择在源码实验前冻结为 `std::map`；比较器使用指针的标准全序，不对无关指针作内建大小比较。节点在 owner 移除时释放，空间随 live owners 增长，避免 hash buckets 的高水位保留问题。

| 文件 | 唯一责任 |
| --- | --- |
| `source/runtime/backend/fault_catalog.h` | 私有 OwnerKey/严格全序比较器和 writer-only 索引；Reader、公开 API 与 Allocation/Snapshot 的读侧合同保持 |
| `source/runtime/backend/fault_catalog.cpp` | Add 在 head 未命中后查索引；新 owner 的索引插入与原 SC 发布形成事务；RemoveAllocation、RemoveModule、Seal 和析构在 Retire 前清除索引 |
| `source/tests/fault_catalog_test.cpp` | 扩展现有 native 模式，验证插入 OOM、跨 module 同地址、同 key 回收重加、held Reader、信号中断和不分配内存的撤销；不创建新框架或 target |

**异常与生命周期是准入条件。** 新 owner 必须由 `unique_ptr<Snapshot>` 和 `unique_ptr<Allocation>` 同时持有资源，索引 emplace 成功后才释放 Snapshot 并执行原 SC store；否则新增的索引分配失败会泄漏 Snapshot。索引插入成功至 head 发布之间不得再有可抛出的操作。每个 live Allocation 恰有一个索引项，Retire 的对象没有索引项；已封口的 Add 拒绝、相同起点的插入顺序、精确恢复站点优先级和 Reader grace 全部保持。

首批不改 Module/runtime 发布调用、不增加 unchecked fresh API、不改变 signal lookup 或 SC 顺序，也不重包旧 C24 批次入口。先完成源码、OOM/信号/撤销审查和既有语义门，FP 缓冲以 C29 正确性基线严格保持；随后独立冻结七轮 56 点。Block 与 Function 分开验收，Function 的额外树节点分配与查找可能抵消收益。若未通过原收益门则撤回，不能凭有序复杂度保留实现或宣称 FEX 数量级已达成。

当前原型已完成这些前置门：新测试先在旧生产实现上四模式通过，随后应用两文件补丁，16 目标实际构建及 32＋22 条候选质量命令完成独立复核；原 22 项 R6 失败精确复现，无新增失败。FP48 与 C29r4 完全相同。实际 ARM64 审计显示 Catalog 80→104 B、每 live owner 新增 56 B tree node，旧 Snapshot/Allocation/Reader 布局不变；主 `.text` 增加 496 B，Add 本地入口栈由 192→176 B，不能据此宣称总栈或速度改善。

随后基线固定为 generic C29r4 `c92a7de9…`、候选 `09a07474…`，原一次七轮 56 点及全部 raw 独立复核完成。Core Block 七对均更快，配对中位比 0.9499278409412607，下降 5.0072%，刚达到原 5% 门；Function 中位比 1.003302、FP 两组 1.003583/1.004336，均跨 1，没有提速结论或全七对负控。Core 的实际请求/host bytes/IR ID 槽位差异和 FP 的离群观测保留，不归一或重采。这一结果当时只准入 freshPGO；后续匹配 PGO、54 质量命令、相邻及直接原 R6 各56点、普通 PMU72现均完成，形成当前 C31 有限检查点。正式外部28点仍为数值失败；不将中间比例相乘或称 FEX 数量级已完成。

### 4.8 代码组织与每组文件职责

延续已经重建的阶段边界，性能工作进入已有 owner。下面列的是下一阶段会触及的源文件职责；头文件声明与对应实现成对迁移，不要求为目录外观搬移已合适的代码。

```text
source/runtime/
  frontend/x86/               当前 x86 指令语义展开与字面量准入
  ir/                         节点、参数、块、HIR 身份及存储
  compile/
    builder/                  decode 同时进行的构造与局部规则
    rules/                    builder 和 residual 共用的纯规则
    passes/                   可变 IR 的完整输入改写与通知
    analysis/                 flags pass 内分析及封口后的权威事实
    allocate/                 Block/Function 物理分配与事务
    pipeline.*                优化阶段编排与封口
    unit.* / optimized_unit.* 输入交接与最终 IR 身份
    workspace.*               有界临时 buffer 的独占借用
  backend/arm64/
    compile/                  目标描述、物理选择、最终证明与 actions
    jit/                      只消费冻结结果并发射机器码
source/translator/x86/        解码范围、frontier、region 与 guest 来源
```

| 源文件 | 单一职责及本轮边界 |
| --- | --- |
| `frontend/x86/decoder.{h,cc}`、`decoder_alu.cc` | x86 操作数读取、访存与 flags 顺序、现有局部规范化；C28 新增字面量保留已撤回，不在此引入 CFG 或物理分配分析 |
| `frontend/x86/decoder_dispatch.cc`、`decoder_bmi.cc`、`decoder_x87.cc` | raw-first 设计中的共享支持/继续/终止选择，以及 legacy handler 的同一指令窗口；不能让第二次 GetPointer 绕过已验证字节 |
| `builder/ir_builder.{h,cpp}` | typed append、可在追加前确定的局部规范化，保持 raw/typed 准入边界 |
| `builder/instruction.cpp` | guest 指令事务与提交、延迟 flags 观察和折叠 |
| `builder/guest_state.cpp` | 局部 guest-state 转发、屏障与失效 |
| `rules/value_rules.{h,cpp}` | 无阶段所有权的纯折叠规则；两处复用，不复制一套优化器 |
| `passes/value_worklist.{h,cpp}`、`values.cpp`、`widths.cpp` | 可变 epoch 的候选、局部改写和反馈；不能借封口事实 |
| `passes/flags.cpp`、`dce.cpp`、`passes.h` | flags/DCE 协作、真实改写与完整通知，保持 fault 根 |
| `analysis/flag_flow.{h,cpp}` | 单次 flags 清理内的 transfer 与 CFG 求解；不跨 epoch 保存可变事实 |
| `analysis/semantic_facts.h`、`facts.cpp`、`ssa.cpp`、`liveness.cpp`、`block_index.h` | 最终 IR 的共享权威 facts、局部用途及 Function 活跃性；仍按需建立索引 |
| `analysis/value_bits.{h,cpp}` | 有预算的位事实查询；原精度、无效输入与预算语义保持 |
| `allocate/allocation.cpp`、`linear_scan.cpp`、`intervals.cpp` | 模式选择、attempt 与 live interval；独立 Block、单块 Function、多块 Function 保留原边界 |
| `allocate/fixed.cpp`、`ties.cpp` | 固定 home、transfer 与偏好的权威时点；顺序分配设计须迁移其 interval 消费者，不能仅删除 interval 生成 |
| `allocate/{ties,fpr,spill}.cpp`、`fixed_verify.cpp`、`transaction.cpp` | 合并、spill、最终物理验证与 rollback；C26/C27 源码保持撤回 |
| `arm64/compile/inputs.cpp`、`{gpr,fpr,spill}_inputs.cpp` | 在封口 facts 上生成现有 AllocationInputs，拥有静态目标要求 |
| `arm64/compile/lower.cpp`、`compiler.cpp` | 目标编辑、逐块 Complete、最终 Seal/Finalize 与冻结；不是重复做优化 pass 的入口 |
| `arm64/compile/{scratch,width_contracts,certificates}.cpp` | 保留原物理稳定时点的准备与生产证明；共享发现不能替代证明 |
| `arm64/compile/{allocation_preparation,actions}.{h,cpp}` | 最终 payload 所有权、准入及只读 actions；没有 workspace 引用逃逸 |
| `ir/{instr,args,block,hir_builder,hir_storage}.{h,cpp}` | 真实 IR 布局、Use/UnUse、owner 与存储；布局原型须集中迁移所有调用者 |
| `ir/ir_meta.*`、`ir.inc`、`allocator.h`、`allocate.cpp` | 静态 schema 与实际节点槽分配/回收；紧凑表示须同时核对 payload、真实 stride 与身份代次 |
| `translator/x86/function_{decode_frontier,region_decoder}.{h,cpp}` | 已接受范围与解码重试，不能向最终 code object 交接临时借用 |
| `translator/x86/translator.cpp` | decode attempt 的未发布所有权、既有 retirement 时点及 Function 内层重试；代码变化状态不能穿过通用异常 catch 而落到 Block |

每包必须列出将退役的旧遍历、接口、临时结果及调用者。删除发生在消费者迁移后；没有已证明可替代者时，不先删除生产验证或只换目录。具体去留还需 N0 及对应准入证明，不把这张职责表当已实施清单。

### 4.9 C32 后的具体实施计划

以下保留C32后的决策顺序及其实际结果。第一项C34已完整测量、拒绝并撤回；当前下一步为第三项N3的具体布局与净账设计，仍未准入生产原型。没有哪项已经证明足以把Function的21倍差距降到10倍以下。

| 顺序 | 工作包 | 适用范围 | 当前准入和交付 |
| --- | --- | --- | --- |
| 1，已结束 | raw边界稳定后一次展开HIR | Function；未改变Function RA | C34通过完整质量和内存门，但原56点Core正门失败；已撤回，见4.10 |
| 2，源码审查已收口 | 按位置推进独立 Block 的局部分配 | 独立 Block；单块/多块 Function 保持原路 | 本轮通用替代不准入：仍需全块预映射、fixed transfer 重排及活跃集合，已观测的可删工作覆盖不足，不进入原型/56 点 |
| 3 | 编译单元内统一稳定身份与紧凑 payload | 共同路径 | 条件研究。需证明端到端访问覆盖和净存储收益；小 visitor/PublicIndex 方案已不准入 |
| 后续条件项 | 同一可变批次内减少重复发现和求解 | Block 与 Function 分别处理 | 必须找到实际重复计算，不能再次“引入”已有 worklist/flags 协作 |
| 不进入本轮性能包 | 编译对象尾部直接共享 | 独立 Block 与 Function 单列 | 当前为 NO-GO，缺合法公共入口、版本及状态合同。独立代码对象设计获得新证据后才重新讨论 |
| 备选 | scope 间有界 buffer 复用 | 已确认的 DCE owner/RA list 临时存储 | 只准备独立提案和生命周期检查；不能用 C32 结果或全部 allocator 占比为其授予性能资格 |

**第一包：将 Function 的重建决策移到 raw 边界。** 借鉴 FEX `GenerateIR` 消费已解码块后再调 dispatcher 的顺序。保留现有 `FunctionRegionDecoder` 和 `FunctionDecodeFrontier` 作为事务 owner，不另建 Function 编译器。设计交付应包含：

1. 为本次事务的 raw 记录定义 PC、长度、必要解码信息、入口/终结原因与有效来源；列出 decoder 临时指针的所有权。不得将借用的 decoder buffer 留给下一次请求或最终代码对象。
2. 先在 raw 边界处理已知合法 split/late entry，再让保留指令进入 typed builder。对覆盖到的路径删除“已展开 HIR → ResetDecodedBlock → 再取指/再展开”，而不是加一次预扫描后保留旧的全部工作。
3. 仍按现有 region/frontier 预算推进，不预先扫描整个函数。区分可由 raw 判定的直接目标与依赖 IR/运行上下文的目标；后者不能凭静态解码假定已知。无法闭合的路径保留原有拒绝/回退语义。
4. 证明 safe fetch、页边界、SMC 与取消，并量出整个事务的 raw 峰值和 raw/HIR 共存区间。当前 GetPointer 不提供代码版本 lease，不能声称 owned bytes 就是全函数稳定 snapshot；以当前生产合同为基线，核对实际被 lower 的字节及影响结构的 witness。没有代码映射 lease/epoch 时，不顺带引入 executable-range cache。
5. 改动集中在 `translator/x86/function_region_decoder.*`、既有 frontier 边界、`runtime/frontend/x86/decoder.*` 与 `decoder_dispatch.cc`；获准 handler 的前缀读取和 `translator.cpp` 的私有取消/重试接口随本包迁移。沿用真实 x86 frontier/fault 测试。跨块 liveness、spill、call-return、恢复记录和最终发布保持原边界。单块 Function 继续原合格分配路径，独立 Block 不经过这套 raw 暂存。

源码设计先用已有案例核对 split、重复入口、拒绝、页边界和 SMC；只有能列清净删除路径、raw 存储成本及全部消费者，才进入原型与性能包。341 次 reset、19040 次实际 HIR 擦除和 1244 次 replay accepted 是定位依据，不是收益预算。若删除覆盖不足，先停止本包，不通过多跑样本弥补。

当前字节与取消的具体实现前置已经明确：

- 将原 Fetch 的窗口获取与 PageFatal 发射分开。规划阶段只获得非发射结果，未知/不支持形态作为 opaque 边界，不能越过本应提前终结的 WRPKRU、FALLBACK 或无效编码继续取指；原即时 decode wrapper 保持原 PageFatal 行为。
- 每条实际语义展开使用同一个已核对的 owned 窗口，legacy `_DInst` 用可修改副本。`decoder_alu.cc` 的 CMPXCHG/LZCNT/BSF 类、`decoder_bmi.cc` 和 `decoder_x87.cc` 还有绕过 `insn_bytes` 的前缀重读，必须迁移获准路径，不能仅替换 pipeline 的指针就宣称同一字节来源。
- 在使用 raw 结构结论前，或整次 decode 完成且尚未发布时，还要核对影响 boundary/placeholder 的未消费 witness。lazy cap=1 可能只展开 prefix，未来 Jcc 已决定切分却未实际 lower，逐个检查已消费 record 会漏掉它。不要顺带扩大 marker-derived guest end 或把投机读取全部加入 guest dependencies。
- byte mismatch 在拥有本次未发布 HIR、raw 和 frontier 的 Function 内层消费，以 fresh builder 重试；反复变化时只关闭本次 raw 规划、回到原 Function decode。不能让普通异常穿过现有 Translate catch：该 catch 会写 block-only 状态并回退 Block。原有 regroup/retirement 若发生在 builder 创建前，须放在内层重试之外且不重复执行，不能声称析构 builder 就能撤销已发布 owner 的副作用。

另一个已审查的较窄方案是保留已生成 prefix、只截断 suffix：需要提交边界、尾 Inst、旧 AdvancePC 累计值与依赖边界，清 terminal uses/outgoing 后逆序删除 suffix，再 Link target。它只能减少本次 470 次 prefix replay 对应的重建，仍先生成并丢弃原始流中的 suffix，不能替代完整 raw-first；不为这一更窄范围单独开启性能包。上述前置已用于C34完整机制实现；实际原56点未达正门，源码已撤回。

设计阶段曾准备三文件、六处 handler 前缀读取的最小补丁，路径为 `compile-cost-function-raw-first-design-r1/instruction-window.proposed.patch`。该单独提案没有建立 owned 窗口，未作为独立候选应用或测量；它的读取迁移已与 owned Fetch/record 一并进入 C34 完整机制包。设计字节/取消的具体调用层次见同目录 `current-byte-and-cancel-review.md`，历史实施准入见 `root-next-implementation-admission.json`；实际实现状态以 4.10 为准。

**第二包：研究独立 Block 的顺序局部分配。** FEX 的 `ConstrainedRAPass::Run` 按 CodeBlock 反向处理 kill/静态偏好，再正向分配。SVM 可先审查 `LinearScan::AllocateRegisters` 中的 interval 物化、排序和 `ExpireOldIntervals` 反复扫描：能否使用已有 `ValueLiveness::use_ends` 和最终输入约束，按真实位置推进相同的分配状态转移，删除其中可替代的整组工作。这是算法设计，C27 的 vector 和 C32 的 pool 没有验证它。

设计集中在现有 `allocate/{intervals,linear_scan,fixed,ties}.cpp`，继续用原 mapping、事务及最终验证，不建第三套 allocator 或第二份 last-use/event 镜像。必须先解决 preserved definition 的 transfer 改变 start 顺序、pseudo 使用 producer start、fixed preferences 消费 interval，以及到期释放的时点和成员集合；MEM/FPR 双槽、VOID scratch gap 和退役 pointer 准入都要保持。`FreeSpill` 使用 bitmap，同一批独立 bit 清除可以交换，不能以不存在的有序 freelist 作为反例。仅有“RA 桶很大”不足以进入原型，需从现有调用链分离 interval 建立/排序/到期工作的实际覆盖，并给出原分配顺序的等价推导。

第一范围仅为独立 Block。合格单块 Function 已有 `CanUseBlockIntervals`、单块 interval 收集和 heap 路径，SSA nonlocal 空集也已有早退，不重新计为收益；多块 Function 保留全局 liveness、跨块 interval/coalesce 和原分配范围。所有工作均在 residual 完成、IR 封口和输入约束建立后，decode 不判断最终 kill 或物理位置。若只适用于玩具块、需要更大事件镜像，或无法保留 spill/victim/释放顺序与现行代码质量门，就停止；改变代码生成策略须另行明确其质量验收，不能称作本包 compile-only 等价。

本轮具体审查已得到停止结论：全块预映射供未来 FPR read/tie 使用，fixed transfer 与 last_use 会重排和延长 interval，全块峰值还决定哪些 fixed plan/interval 保留。VOID 的 `fill_gap` 必须先记录旧 active 集合，再按严格 `end < start` 到期，不能在每个 use 的尾部立即释放。当前精确 Block 调用链中，collection 六个逻辑叶样本全部属于仍须保留的 shape/fixed/use-end 工作；构造期 count 三个样本、expiry 入口一个样本，排序 self 未观测。即使宽松合并也仅为整份记录 period 的 0.30914%，并非 RA 桶的 5.83%，也不是可省时间。保留预遍历再合并 relocation 流的替代模型仍需新索引/活跃集合，不能净删除预定三类工作。因此本轮不实现、不启动性能包；报告为 `compile-cost-block-sequential-ra-design-r1/review.md`。

**第三包：把表示优化作为完整迁移设计。** FEX 的相对 NodeID 与按 opcode 大小分配 payload 可供参考，但 SVM 不能只换一个 getter 后增加旁表。设计范围覆盖 `ir/{instr,args,block,hir_storage}.*`、typed builder、mutable passes、封口分析、AllocationInputs 和序列化消费者；先明确稳定身份、arena 回收代次、退役 queued pointer、raw/部分构造状态及 Params/Lambda 的等价表示。选定受控 schema 范围后集中迁移消费者并删除旧布局入口，不长期并行维护两套 IR。当前 Inst 94 B、arena stride 96 B 只说明机会，不证明提速；没有真实访问或 retained/peak 减少的证据，不启动全量布局重写。

**后续条件项：在已有 residual 协作内缩短一次改写批次。** 借鉴 FEX DFE 的一组相关变化在同一工作过程中传播。SVM 的 `pipeline.cpp::RunResidualPasses`、`passes/{flags,dce,value_worklist,values,widths}.cpp` 已协作，目标只能是同一稳定 epoch 中还在重复生成的 roster、重复 transfer/Query 或相同输入的重复发现。先列出“谁修改了什么、哪些消费者真正失效”；新结果须留在现有 rewrite/worklist owner 内，预算耗尽、异常、擦除与 sparse/foreign ID 继续有效。Block 只处理块内数据，Function 保留 CFG/fault roots。仅减少 row 往返或一次小 getter 不单独开性能包；没有大块真实重复工作就继续 HOLD。

**不进入本轮性能包：直接共享现有代码尾部。** FEX 的 CodeBlocks 不都能被外部跳入，只有标记为 EntryPoint 的入口才导出；SVM 的内部块也不能自动当作公共代码。以后若研究新的代码对象边界，先在既有 code object/入口 owner 中明确 guest 版本、context specialization、RA live-in、NZCV、frame、fault、SMC 和 retirement 的一致性，并取得真实覆盖。涉及文件为 `backend/arm64/jit/jit_context.*`、`function_code_object_emitter.*` 与相应 translator 发布/失效调用者；不建第二份全局目录，不用相同字节或范围重叠替代合同。当前直接复用仍为 NO-GO，不因观察到六组重叠就升为第四个实施包；降低 Function 到 Block 分配也不属于该方案。

**备选存储提案的边界。** 与已撤回 C32 的区别只能是实际保留并复用跨 scope 的 chunk，同时受保留上限约束。例如由现有 workspace 的私有 pool 记录全部 upstream chunk 字节，在最后 borrower 销毁后，小池保留、大池整体 release。64 KiB 可作为待验证的初始保留阈值，不能由旧的 512 B 最大 pooled block、64 blocks/chunk 推导总容量上限。该阈值只约束 outer scope 结束后的这一只 pool；不限制活跃期间峰值、整个 workspace、TLS Inst arena，或未调用 Reset 的 caller-owned workspace。嵌套、两个 prepared 共存及异常退出必须先验证。C33 目前只在 results 中准备提案，未应用、未构建、未测量；不为它重新带回 Facts/index PMR，也不将它排在上述结构设计之前。

每包最终交付必须包含一份删除清单：旧循环/查询/容器/接口、全部调用者迁移、原始与最终 owner、回退及兼容身份。目录按 4.8 的现有职责组织；文件移动和重新命名本身不计 compile 收益。通过机制包相邻采用门后再训练匹配 PGO、直接原 R6、普通 PMU 和外部 FEX，禁止用旧 profile 给新源码贴上“已采用”身份。

### 4.10 C34：完整验证后拒绝，精确恢复C31

已拒绝实验r3的源码为 `8acf2ed0…`、generic主程序 `e372ea6e…`、`swift_test` 为 `8f20d6df…`，cache52；13个实验文件现已撤回，没有保留新API或第二套编译管线。下面记录原型当时的职责与证据，不描述当前生产实现。源码、16目标和配置身份保留在 `compile-cost-c34-function-raw-first-r1/{source-r3,native-r3,build-r3}.json`；当前已精确恢复C31 PGO。

| 文件 | 已实现职责与边界 |
| --- | --- |
| `runtime/frontend/x86/decoder.{h,cc}` | owned 16 B 取指窗口、值类型 `DecodedInstruction`、非发射 Predecode、记录消费与当前字节核对；缓存 `_DInst` 交给 lowering 时使用可修改副本。正常独立 Block 仍即时展开，不做 raw 规划 |
| `decoder_dispatch.cc`、`decoder_sse4.cc` | 在既有支持分派内共享 Linear/DirectConditional/Opaque 选择，不维护第二份 mnemonic 表；JMP 等提前终止语义不能标为 Linear |
| `decoder_alu.cc`、`decoder_bmi.cc`、`decoder_x87.cc` | 六处前缀读取消费同一个 owned 窗口，取消绕过该窗口的重新 GetPointer |
| `decoder_crypto.cc`、`decoder_userland_ext.cc` | 共享原有手工形态解析与支持/终止边界，规划阶段不发射语义或越过 opaque 指令 |
| `translator/x86/function_region_decoder.{h,cpp}` | region 内有界 raw 规划、合格直接回边 split、原顺序的 placeholder/队列、prefix/suffix 分别 fresh lower；发布前复核全部结构 witness，包括 cap=1 尚未展开的 Jcc。依赖旧 IR 的 outer frontier 与 call-return 路径继续保留 |
| `translator/x86/translator.cpp` | 在既有 Function 尝试内部处理 CodeChanged：两次 raw 尝试后，第三次关闭本次 raw 规划、继续原 Function 解码；每次销毁未发布 builder/raw/frontier 组合。真实异常保留原处理，retirement 不在内层重复执行 |
| `tests/function_decode_frontier_test.cpp` | 保留原 frontier 合同，覆盖长 extent、mixed SIMD/DIV/CMPXCHG、opaque JMP/WRPKRU、短窗口、已消费字节和未消费 witness 变化、真实取指异常传播；检查结构、marker 和所有权 |

**真正替代的旧工作。** 对合格 self late-entry，先从 raw 指令边界确定 prefix 终点和 pending suffix，避免先展开整流、再由 late-entry Reset 擦除、再重放 prefix。没有删除整个 `ResetDecodedBlock` 接口，也没有替代全部 237 次 outer frontier 重建；不合格形态和预算耗尽继续走原 Function 路径。不能把历史 19040 个被擦节点全记为这包收益，更不能把全部 decode 成本视作可删量。

**新增成本必须一起验收。** 每个 `FunctionRegionDecoder` 的保留容量按实际 vector capacity 限制为 4096 条 record；实际 ARM64 DWARF 中 record 为 112 B，因此 record 稳态容量上界为 448 KiB。按当前几何扩容规则，旧、新 buffer 同时存在时的 record 存储推导上界为 672 KiB，另计 RawPlan metadata、allocator、HIR、栈等；它不是整个 Function/进程的 hard cap 或实测 RSS。精确 generic ELF 对照中 `.text` 增加 15320 B，Translate 入口帧 2816→2944 B，DecodeBlock 1056→1104 B，DecodePipeline::Run 544→720 B，新 Predecode 704 B；未见 4096 条 record 的栈数组。native 审计不证明总体内存改善，后续仍须按既定 MaxRSS 门检查。

随后已按原四点顺序完成普通、无诊断变量的Core2000 MaxRSS检查：Function baseline/candidate为18704/19088 KiB（+384），Block为16044/15920 KiB（−124），均未满足“同时增加超过10%且超过8192 KiB”的严格AND停止条件。41条命令、20份point原raw和8份condition raw、真实PID/CPU4/CRC/前后身份均经独立复核，收据 `memory-sanity/qualification-independent-review.json` SHA `027053898c7d50eb8230e07c14f4b33d0194211ee718d1da031dd69eb65b9e9d`。这四次单点高水位不能证明内存改善或整个arena峰值；record容量推导与实测RSS继续分别报告。

**原型质量验证与限制。** r3 的 16 目标构建成功。一次独立运行实际执行 frontier 全部 16 用例、1570 项断言，target exit0，无失败或跳过。采集器末尾把私有可执行文件的 JUnit classname 写成固定值，原执行收据因此保留 failed；独立核验根据实际 argv、XML、摘要和前后身份确认测试结果，资格收据为 `targeted-frontier-r3/qualification.json`（`30178794…`），没有重跑或改写失败记录。54条完整质量命令随后唯一执行完毕，并完成146个raw的独立复核：Catch656 rows/874948断言，原22项历史失败精确匹配、没有新增失败；20个父/单进程native命令1773 checks与8个独立子阶段392 checks分别核算；FP42/6736 B与6/7992 B按真实PC/order/bytes/FNV保持。收据 `full-quality-independent-review.json` SHA `a64b72bd9fa70138d5ff9fdbe347f9f2f6f12779a20d20e0374f7d6e31f7cae5` 只签正确性和代码门。Root另核实source2418/退役118/native16/config3稳定，手机16角色为C34、其他九项身份不变，已无活跃运行。另一次frontier的16用例/1570断言没有合并进54计数。随后generic结果如下；C34没有进入其PGO、普通72或外部FEX对照。

原七轮56点已唯一执行并独立重放730个raw：测量有效，正门失败，按原规则拒绝。比较的是精确C31 generic `09a07474…`，没有混入PGO或R6。结果为下表；FP不能补开Core正门，四组亦未触发“七对全变慢且中位≥1.01”的负控条件。

| 组 | candidate/baseline配对中位 | 更快/更慢 | 正门或用途 |
| --- | ---: | ---: | --- |
| Core Function | 0.996976169 | 4/3 | 未达5%且跨1，失败 |
| Core Block | 1.016746425 | 3/4 | 跨1，失败 |
| FP Function | 0.986863228 | 7/0 | 仅负控，不开门 |
| FP Block | 1.005448251 | 2/5 | 仅负控，跨1 |

Function两臂均为439 Function+7 Block、4906 decoded blocks、340944 host bytes、82819发布ID槽；decode桶配对中位1.001332，没有净下降。Block实际单元/host bytes/ID槽有小幅变动，原样保留，不归一；这不改变原门结论。阶段桶不拆成纯预解码或校验耗时。

独立判决 `independent-policy-decision.json` SHA `bf25534e64ad006ad9a7f018ae3525efccf8f17e57abd8806b9728f517e58e38`。13源码已guarded恢复为3cb，392 typed配置、三配置文件、host16和phone16全部精确回到选定C31 PGO，其他九项手机输入不变；root闭合 `withdrawal-closure.json` SHA `81e2b5720bf1c3bee9cab28ad061102d2c3e38db9b9dbb6c7125170444d7f616`。实验main和swift_test压缩读回保留约80.55 MB，其余证据在Data2，自有私有测试副本已核SHA后回收。C31旧profile仅在全部源码恢复3cb后用于其精确重建，没有用于C34。

### 4.11 下一项N3：先冻结布局并取得完整生命周期净账

有界源码审查确认存在完整迁移方向：稳定且不在队列生命周期内复用的header身份、opcode实际payload、唯一存储owner，同批迁移builder、可变passes、HIR值/用途、封口分析、两种分配和backend/序列化消费者；迁移后删除旧value_lookup排序/Find、DCE owner表、可替代的HIRValue对象及二跳表。第一段公有raw/foreign准入、ResolvedUses、可变顺序和真实用途不能凭“稳定slot”一并删除。

当前Inst为94 B、实际槽96 B，60 B来自固定5×12 B Arg；默认构造至少写五个Void tag，不能说每条IR清零60 B。Inst本来已有64 KiB chunk的TLS bump/freelist；FEX则是20 B ordered header再加opcode payload和双arena，不能直接宣称94→20收益。精确C31样本中Find的inclusive period占整份record的Function1.517%/Block0.682%，还含内联子行，不能叠加或转成可省时间。

生产原型仍HOLD。先冻结一个含num_use/pseudo_use/order/opcode/type/owner、stable slot、alive/unfinished、链/payload定位及必要用途头的具体布局，核真实ARM64 ABI；再按HIRStorage/builder的实际owner周期，把tombstone header、改写时旧/新payload重叠、Params/Lambda、用途表、chunk slack和双prepared/重试的共存全部计入。不得把单个Function结束误当共享storage释放，也不得永久保留旧Inst facade或第二份镜像图。

唯一建议的新诊断为复用既有IR_DETAIL渠道：两模式各一次有界aggregate census，先冻结模型/公式和最少hook，再补全当前保留量、候选净量、构造/变形/擦除/用途/未归属账。没有新56、耗时收益或PGO结论；改动后的临时诊断必须独立generic构建，不套用选定旧profile，结束后精确恢复C31。若完整账仍不支持值得迁移的净删除，停止N3，不退回小getter或旧pool包。

53个必要SVM源文件在C31/C34的单文件SHA相同，五个FEX源文件绑定固定a156，887215 B文本快照已保存在 `compile-cost-ir-representation-design-r1/snapshot`。有界审查报告 `review.md` SHA `c9321d4c4799797a474e72bd1b478ee3ecda31edab78bf869067bdb2613014db`；它签语义设计边界，未签布局ABI、净账或性能。

2026-10-28的具体模型已推进到R2，仍未准入生产表示重写。主模型采用64 B header、按opcode保存的payload、独立origin store和Function用途表，保留32位用途计数、16位顺序及稳定身份；64 B不是整条IR成本，必须另加payload、chunk余量、用途、目录、raw staging、workspace和生命周期重叠。191个opcode的公式及Android API28独立布局探针已归档，探针不替代真实VM复杂类型的ABI测量。

R1独立审查提出的两个缺口已修复：store不跨周期缓存，因此逐次计入真实策略要求的分配、释放、构造、初始化和复制；Block懒用途图按所有origin的stable-slot完整域R分配Links，包含墓碑和本scope之外的槽，不能沿用旧live row数低估成本。R2模型 `model-r2.json` SHA `440fca78…`、独立审查 `layout-census-independent-review-r2.json` SHA `00801df3…` 仅准入临时census实现和源码审查。

诊断实现使用既有 `PROF2 && IR_DETAIL` 开关和固定16 MiB observer；暂存源码覆盖Inst/Params、物理Block、HIR lease及缓存池、真实数组API请求、worklist/懒用途图和prepared借用。旧地址在队列里残留时必须保留其birth与墓碑，不能按新birth倒填。只输出有界汇总；无法闭合的来源、容量、复制工作或尾部清理显式HOLD。2026-10-28此阶段正在审查暂存实现，尚无N3 VM执行或提速结果；C31源码、PGO配置和固定手机角色保持原身份。

## 5 分路径的验收边界

| 路径 | 必须保持的行为 | 统计要求 |
| --- | --- | --- |
| 独立 Block | 实际 AllocateBlock；不承担 Function 专有 CFG 与存储 | 单独报告总量、输入、分配、Complete、发射及工作量 |
| 单块 Function | 实际 AllocateFunction；保留入口、隐式根与 locality 合同 | 分别验证既有 block_intervals 的合格与拒绝；不能用 decode 单块标签代替实际路径 |
| 多块 Function | CFG 活跃性、支配、跨块 spill、call-return 和 hoist 保持有效 | 保留重试/重解码及真实单元组成，不按 IR 数强行归一 |

Block 与 Function 的 allocation/actions 构造进入 coarse RA 的边界略有不同，比较时如实保留。缺少独立单块 Function 耗时时先报告真实机制资格，不编造第三组时间。

存储和遍历精简应优先保持同一输入下 mapping、actions、发射缓冲和恢复元数据一致。对于实际地址/重定位导致的差异，保留原始字节并核对真实重定位语义；不能只比较 code-hash 字符串或任意归一分支后宣称逐字节相同。

## 6 测量与采用规则

设备继续使用已连接的红魔 NX729J，SVM 与固定 FEX 均运行 Android 产物。CPU4 affinity、内存序、guest、oracle、ELF、脚本及设备六条件沿用已验收口径。状态改变时保留原始点并停止混合，不调性能模式、频率上限、散热或充电设置来追求通过。

| 时机 | 检查范围 | 结果用途 |
| --- | --- | --- |
| 一个小候选成形 | 受影响目标的增量构建和已有定向合同用例 | 正确性和实际删除工作，不估计总体收益 |
| 一个机制包收口 | C18 起预先冻结七轮：两负载 × 两模式 × 前后 × 七轮，共 56 点；此前批次保持原三轮 24 点 | 比较 inclusive translate 和各桶，保留所有工作量差异；不替换历史点 |
| 需要归因 | 当前版本普通 simpleperf；必要时单独一次已有 PROF2 诊断 | 定位子热点、重试和路径；诊断时间不进入正式性能表 |
| 采用前检查代码质量与运行成本 | 现有发射核对及普通 PMU；正式三臂协议保持完整 schema | 验证既定指令质量门并报告 wall/cycles 变化、spill 与恢复合同；指令门不证明全部运行时间无回退 |
| 最终组合收口 | 集中构建和受影响的原生/RV 既有矩阵；新版本直接对初始基线 | 验证组合与累计改善，不相乘中间比例 |

coarse 明确使用 `SVM_PROF=1 SVM_VERIFY_PLAN=0`，`SVM_PROF2` 完全缺席。普通 PMU 中 `SVM_PROF`、`SVM_PROF2`、`SVM_VERIFY_PLAN` 均缺席，包括显式的 0；同样不传其他 audit、dump、trace 等诊断变量。FEX 的标准统计按实际 CNTFRQ 换算，保留原始 ticks、live/final、PID/starttime 和线程槽资格；FEXServer 完整生命周期单列，不摊到每个 launcher 点。

性能采用规则作用于完整机制包检查点，不作用于每个小改动。当前正门是 Core Function 或 Core Block 至少一组七对 candidate/baseline 全部小于 1，且配对中位不高于 0.95；FP 用于负控，不能替代 Core 开门。四组中任一组七对全部变慢且中位达到 1.01 时暂缓采用，保留原点并说明原因。C18 起按各批测量前冻结的七对判断，旧批次保持原三对及各自原策略，不追溯修改。这个小样本规则不代表统计显著性或观测独立性。范围跨 1 不记为稳定提速，也不追加样本挑到通过；只针对已明确的问题预先定义必要复核。内部单包开门不替代外部两模式各七对均严格小于 10 倍的完整目标。

普通 PMU 沿用既有逐轮 instructions 质量门：有效增量斜率的 candidate/baseline 不超过 1.01，按负载和分配模式分别判断。该门比较 SVM 前后版本，不把当前 SVM/FEX 约 1.11 的指令比误作新回退；不可定义的指标仍按原比较器规则记录。

可证明删除冗余且净简化、但未证明提速的改动，可以作为清理保留并单独标记。新增状态、分支或生命周期复杂度却无收益的候选不采用。每包须列出删除的遍历、临时表、分配或复制；“调用次数少了”不能直接记为耗时收益。

堆检查优先复用已有 requested/live/retained 统计，栈检查使用实际产物和编译器帧信息；单包定向检查，阶段末集中核对。不得用更大的固定栈帧、全单元清零表或无限保留容量交换表面分配次数下降。

### 6.1 外部 FEX 工程门的方法与观测边界

已完成同批、相同Core2000的编译服务elapsed对照：两种SVM分配模式各七轮与固定FEX分别配对，共28点、14个独立FEX分母。协议、源码/ELF/profile、环境、线程资格与严格小于10倍门均在执行前冻结，私有FEXServer生命周期单列；不复用历史跨批比，也不替代内部质量门。实际结果见6.2。

FEX v2 线程槽会在复用时清零，fork 又可能产生另一 region；只看开始/结束的 `/proc` 和 final/live 两个槽快照不能恢复历史。正式点必须取得同次、无丢失的 native task/fork 生命周期证据，并结合固定源码闭合单一 guest compilation context；无法证明时不能把可见槽提升为全进程总编译时间。SVM 原诊断闭合各自六PID；随后新的FEX与FD collector联合资格观测也已独立补证通过，同次141个节点完整退出、目标无额外线程或后代。两者各保留自己的证据，不跨程序或跨轮替代正式点。

已完成的联合资格观测由现有simpleperf command-launch/inherit从启动前包住C4 FD collector，保留完整task/exec/FORK/EXIT、stats inode、主进程starttime与全部后代退出。原观察器强制task/comm_exec位的HOLD保持，依据对应内核和精确工具二进制另行补证；不是删减生命周期合同。正式28点使用两臂一致的外层CPU0/default-inherit/400Hz user-cycles record与CPU4 target，SVM派生capture移除原内层stat。每点均闭合记录策略、四类丢失、exec、线程/slot世代与尾部后代，不以sampled PID或lost samples=0单独证明完整性。此前资格观测不计入本批，也不重算历史47.257ms。

双方计时已经明确包含各自完整编译服务，但边界仍不同。FEX v2 没有 entry、miss/outcome、IR 工作量或 slot-generation 字段，SVM frontend units 也不等于 Translate/RA 次数。因此上述 existing-tool 协议至多签明确定义的 elapsed，不能升级为同边界纯 CPU、每 miss 或每 IR 成本。若后续确需这些精确指标，先另行提出最小计数与边界资格，不新增长期采集框架，也不以旧 DumpIR 的 282 个请求作历史 47 ms 的分母。

原C4 FD collector硬编码Core2000；FP外部编译验证仍须显式适配argv/oracle并另行冻结，不能仅改标签宣称原样支持。普通PMU的FP运行对比保持原有范围。当前C31只是有限编译与既定指令质量检查点，下面的实际外部门未通过，完整目标保持未完成。

### 6.2 当前同批外部结果与下一步

28点已完成，独立全raw与计时账本复核通过：测量有效，数值门失败，无重跑或补点。这里比较各自完整编译服务的既有elapsed端点，不能称同边界纯CPU或每IR成本。FEX仍使用自己的CodeBlock分配，表中模式指SVM的分配范围。

| SVM模式 | SVM耗时独立中位ms | FEX耗时独立中位ms | 配对比中位数 | 七轮范围 | `<10×`门 |
| --- | ---: | ---: | ---: | ---: | --- |
| Function | 1004.728 | 47.193 | 21.0054× | 20.6341–21.4794× | 7/7未通过 |
| Block | 620.957 | 47.503 | 13.1035× | 12.6214–13.3146× | 7/7未通过 |

时间列独立取中位数，配对比从每轮自己的FEX最终fresh-origin ticks计算，二者不互相反推。完整证据见执行记录。当前差距仍要求删除实质工作，不能以内部33.86%的Block累计改善替代数量级目标。

当前 C31 的两份普通调用链已采集、完成源码及 allocator caller 归因并关闭全部 native reader，结果与下一步排序见2.3。它们不入上述性能表、不替换原点。Function 整段 HIR 重放的两次临时 generic 诊断也已结束并各自精确恢复 C31，真实计数见4.6；此后的C34r3完成完整质量、内存和原56点，但未达采用门，现已撤回并精确恢复C31，见4.10；没有新的FEX差距结果。Block 不增加两阶段。N1/N3/N4 原停止条件及所有生产语义/物理证明继续有效。

## 7 正确性与代码质量门

- 保留 PF/AF/NZCV、普通访存故障、helper 观察、scalar 上半 lane、scratch、FPR 合同及冷恢复语义。生成代码专项仍要求 arithmetic NaN guard、源快照和热路径私有 Q 访存为零。
- 保持 sealed IR、准备 writer、冻结 allocation/actions 与 emission 的所有权边界；覆盖两个 prepared 共存、物理重试、嵌套编译和异常回滚。必要的 sparse ID、Params/Lambda、terminal/hoist 隐式根检查继续有效。
- 新旧路径的受影响正确性用例不能出现新增失败。按改动复用既有案例；只有缺少实际合同覆盖时才增加永久语义回归，不为小型容器替换写镜像测试。
- 当前普通 FP 热回边以两模式各 51 条为质量基线，spill 与恢复质量按原固定案例核对。任何代码差异必须单独解释，不能以 FEX 46 条为理由删除故障所需状态。
- cache51是原基线与历史C19的兼容身份。纯分析/存储精简无需机械升级；实际改变持久格式、恢复ABI、入口协议或已缓存代码语义时，更新兼容身份并运行既有旧缓存拒绝、disk/AOT及销毁源后执行门。C29准确guest范围修复的cache52补证已完成并纳入C31，旧51资格没有被直接转给新源码。
- C18 当轮受影响回归覆盖 22 个套件、628 个 case rows、863924 个断言检查，包含与精确原 R6 对照一致的 22 个历史失败；353 个 native checks 通过，没有新增失败。该单包质量资格没有代替采用验收，后续选定 C19 组合的直接对照及普通 PMU 见 8.5。此前 main 的 11 个失败分项/66 个失败断言和 RV37/254 有限资格保持原范围。

## 8 历史工作包与实验结论

本节保留实际实施过程与采用边界。早期 scratch 变化点、固定约束 cursor、家族门控、FPR 首轮融合及证书转移已经实施，不再是下一阶段任务；冷诊断、ThinLTO 和 PGO 已有各自结果。下一阶段以 4.9 的实施顺序为准，2.3 保留各路径的证据归属，第 4 节其余部分列路线约束和历史候选，不按 N 编号或旧的首轮安排重复建设。

每一项性能结果同时对照其相邻精确参照，最终组合另直接对本方案初始基线，不相乘阶段比例，也不以源码行数或容器变化认定收益。

后续实现沿用 Data2 固定源码和构建缓存。大临时数据实际写 `/Volumes/Data2/swiftvm-middle-build-cache` 与既有 results；保留小的唯一原始证据和精确产物压缩件，验证后回收重复展开文件，不新建整套 build/result bank。

### 8.1 早期 C6 工程检查点

C6 已完成原始 R6 的直接 24 点对照：CoreMark 的 Function/Block translate 配对中位数分别减少 9.44%/10.37%，FP 为 8.65%/9.17%。全 14 个目标按 Release ThinLTO 新构建，受影响回归无新增失败，FP 的 48 个发射缓冲与原 R6 完全相同；普通 PMU 72 点的 12 个逐轮指令斜率比值均通过不超过 1.01 的门。实际运行时间的波动和小幅增加保留在执行记录，不能写成全部运行时间零回退。

此检查点尚未完成与 FEX 同数量级的目标。C3 中没有证明收益的六表延迟物化、短块 clobber 阈值分支和提前 publication guard 已撤回，独立语义测试保留。frontier 范围 memo 与新的 alias 消除规则继续暂缓；FaultCatalog bulk/API 已作为 C24 实施、测量后撤回，当前未保留，见 8.7。后续新候选仍各自需要净工作量、生命周期和收益证据。

测得收益依赖完整的构建配方，只有源码改动不代表已启用 ThinLTO。当前固定 Android cache 原 Release linker flags 为空，实际追加参数如下；其他构建目录须保留其已有 linker flags：

```sh
cmake -S /Volumes/Data2/SwiftVM -B /Volumes/Data2/swiftvm-middle-build-cache/android \
  -DCMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE:BOOL=ON \
  '-DCMAKE_EXE_LINKER_FLAGS_RELEASE:STRING=-Wl,--thinlto-jobs=4 -Wl,--thinlto-cache-dir=/Volumes/Data2/swiftvm-middle-build-cache/android/thinlto-cache -Wl,--thinlto-cache-policy=cache_size=0%:cache_size_bytes=1073741824:prune_interval=0s'
```

该命令复用已经配置好的 Android NDK cache，不是从空目录配置 Android 的完整命令。保留 O3/NDEBUG、异常、栈保护和全部生产检查；缓存按链接时回收，不将 1 GiB 目标称为瞬时磁盘硬上限。完整工具、命令、原始失败和源码/配置/ELF 身份以执行记录为准。

### 8.2 后续候选的收缩与排序

C7 的 Function DCE owner 目录和 C8 的已知位并行加法实验都已撤回：正确性及 FP 字节对照没有新增问题，实际配对结果却触发 Block 负控停止条件。恢复后的 14 个 ELF 分别核对为精确 C6。今后继续以实际工作量和净收益决定采用，不因为 vector 替换 hash、静态指令变少或独立数学证明通过就保留性能候选。

新一轮使用已有 C6 PROF2 做两次独立诊断，支持继续检查分配输入和 target declaration。FPR contract retry 两模式均为零；regalloc 总括号与公开 Allocate 计时之差包含构造、析构和插桩，不能当纯 Describe；Complete 的 declare/perblock 等子范围也不能相加。详细数值和实际身份见执行记录。

| 顺序 | 独立候选 | 文件与删除的工作 | 保持的合同 | 实际状态 |
| --- | --- | --- | --- | --- |
| 1 | C11 消费者局部搜索 | `register_versions.cpp`：每条指令进入输入访问前记录当前 `fixed_home_uses.size()`；去重搜索只看此后追加的记录，删除必然属于其他消费者的前缀比较 | 同一消费者重复定义的最后一个合格 view 覆盖、narrow Sub 拒绝、逐操作数 occurrence 计数及最终排序不变；没有新目录 | 暂存净精简，计时近持平 |
| 2 | C9 冷诊断模板 | `base/logging.h`：现有 DiagnosticFormat/Print 模板标为 cold/noinline，减少常用查询函数携带的格式化对象与栈帧 | 原参数求值、两层 channel 过滤、输出、临时值生命周期与异常传播不变；单独构建和归因 | FP 约改善 2%，Core 范围跨 1；当时组合质量待定，后续 C19 已过既定组合质量门，见 8.5 |
| 3 | C10 解码输出容量 | `externals/distorm/decoder.c`：非 filtered decompose 输出已满时，在既有结果计数、nextOffset 和 flow-control stop 处理后提前退出，跳过下一轮无用 prefix 工作 | 排除 legacy textual 与 `DF_RETURN_FC_ONLY` 路径；精确保留状态、已写结果及边界条件；fetch、PC、IR 提交和 SMC 依赖不变 | Core Block 与 FP Function 性能负控停止，已撤回 |

这里借鉴 FEX 的局部数据归属、顺序处理及成功 decode 的范围管理。SVM 当前已有默认快速整数解码、每 block 的 decoder 对象、线程级缓冲复用和按需 reverse uses，这些继续作为现状，不重新列为待实现优化。

FEX 的 executable-range cache 有真实的失效 owner。SVM 的 GetPointer 当前只给出独立映射查询，没有跨指令稳定 range 的 lease/epoch；范围缓存、批量预解码和 frontier memo 继续暂缓。FaultCatalog 的新 owner 查找与同 owner 快照复制须分开归因，C24 不引入 unchecked fresh-path 或额外发布索引。后续 C31 已取得搜索循环本身的采样和完整单 writer 失效点，独立研究其私有 owner 键索引，见 4.7；它不改变 Reader 发布结构。每个候选完成独立采用判断后再进入下一个，不将不同机制打包掩盖负控结果。

### 8.3 独立构建调优实验

C11 的相邻结果接近持平；C9 的 FP 两模式约改善 2%，CoreMark 范围跨 1，均未达到 5% 目标。当轮两项只暂存，通过完整普通 PMU 的组合仍为 C6；后续 C19 组合资格见 8.5。C14 编译器调度实验和随后 C10 解码器实验均因各自负控结果撤回，源码和 14 个 ELF 当时已精确恢复 C9，另做同 ELF 的 A/A 测量校准。

实际 SVM 主 C/C++ 编译命令未指定 CPU/ISA 调优，固定 FEX 也为 `TUNE_CPU=none`、`TUNE_ARCH=generic`；这不是原始差距的已证原因。既有 CPU4 元数据为 implementer `0x41`、part `0xd4d`，对应固定 FEX 脚本识别的 Cortex-A715。NDK driver 的两次 `-###` 对照确认，追加 `-mtune=cortex-a715` 后，cc1 argv 仅新增 tune-cpu；target-cpu 仍为 generic，ISA features、Android API 和 ABI 完全相同。此次不调整 ISA 或运行时 guest capabilities。

实际配置只向既有 `CMAKE_C_FLAGS_RELEASE` 与 `CMAKE_CXX_FLAGS_RELEASE` 追加调度参数。这两个 cache 字段原为空，O3/NDEBUG 由 NDK toolchain 提供；重新生成的命令已核对保留 O3/NDEBUG、ThinLTO、异常、栈保护和源级 crypto 参数，其他 cache 字段不变。实验源码与 C9 逐字节相同，14 目标构建、受影响回归和 48 个 FP 发射缓冲对照均完成，没有新增失败。

对照精确 C9 的 24 点中，Core Block 中位改善 1.84%，FP Block 却三轮变慢、中位增加 3.66%，触发原停止条件。没有补采或继续普通 PMU 采用验收。两项 Release 参数已恢复，全部 typed cache 值和生成的 Ninja 命令与 C9 相同；CMake 自动替换的两处帮助注释另列，未伪称原始 cache 文件逐字节一致。调度版 ELF 作为拒绝候选归档，未贴成 C9 产物；C10 独立验证及撤回后才得到 14 个精确恢复的 C9 ELF。

此调度设置不成为 Android 默认。随后独立实施的标准 LLVM PGO 见 8.5，不将构建实验与源码清理的比例相乘。另复核固定 FEX 的计时边界和线程统计生命周期，继续区分真正的编译工作差异与尚未闭合的测量范围。

### 8.4 校准后再推进的工作

C17 使用同一 `b9478294…` C9 ELF 放入两个角色路径，已完成原 24 点 A/A 校准及独立重放。四组范围均跨 1，全部 12 个配对比值为 0.968958–1.038212；这只是当前协议的一次波动观测，不能当作噪声置信区间或已证角色偏差。没有源代码改动，14 个 ELF 的精确恢复支持复用原资格。校准不能替换旧点或回写拒绝结论。C18 的七轮 56 点策略在应用补丁、构建及性能观测前另行冻结；原校验器逻辑保持不变，只扩展轮数和最终点数。

C16 已确认 REF 链压缩在 BeginLocationEditing 和 FreezeGPRLocations 各做一次，有效冻结结果最多一跳。保留准备期通用 Resolve 与 root/provenance 合同，不再新增链压缩遍历或另一张 resolved cache。

C18 已应用 opcode schema 内的参数布局候选，涉及 `ir_types.h`、`instr.h` 和既有 `ir_contract_test.cpp`。191 个 schema 最多 5 个物理槽、最多一个 Operand；构造时在已有 IRMeta padding 中保存首 Operand 的逻辑位置，替代非零 GetArg 中对签名字节的依赖访问。实际 Android LP64 的 IRMeta 仍为 48 B，旧字段 offset 不变；最终 ELF 的 194 项表仍为 9312 B。ILP32 仅有布局分析，未实际构建。没有新增实例缓存或逐单元分析表。

七参数 constexpr 构造函数保留现有表初始化，constinit 继续约束常量初始化。类型不再是 aggregate，已核对仓库内没有依赖 aggregate/default/designated 初始化的其他消费者。bounds/type 检查和精确拒绝文字保留；所检查的实际动态 index 0 路径仍避开 descriptor 读取，不将源代码短路条件泛化为全部机器路径保证。已有前缀处理原本已展开，不称为删除运行时循环。

14 目标构建、22 套件受影响回归及 48 个 FP 发射缓冲对照已完成，没有新增失败，发射字节与原 R6 相同。实际 native 核对见到代表性 Query 少一次依赖读取、通用 Value getter 从 560 B 缩至 180 B，整体 .text 减少 19100 B；自身栈帧未缩小，静态尺寸不等于运行收益。七轮 56 点均已完成，独立重放核对了全部 730 个 raw 文件和 28 个配对结果。四组范围跨 1，没有达到 5% 提速门，也没有触发七轮一致的负控停止条件。

暂存为不可变 schema 派生元数据和 getter 访问路径清理，仍为 provisional。理由是复用已有 constinit owner，实际消除了每次查询的签名前缀依赖，没有实例缓存、失效系统或分配；源码增加和 aggregate 属性变化是明确维护代价。完整采用继续依赖选定组合的直接原 R6 对照和普通 PMU，不能把本轮中位数称为稳定提速。

C15 确认 FEX 普通编译计时已含解码、优化、RA、后端和大部分单元发布，未发现整段未计时 worker 编译。初始化、最终析构、dispatcher、返回后的 direct-link 修补及独立 single-step 的边界保留；单槽观测仍不足以恢复线程槽复用和 fork 历史。后续继续优先用 SVM 相邻及直接原始基线验证实际收益，不将原始计时比值当纯 miss CPU 倍数。

### 8.5 标准 PGO 的独立实施条件

固定工具链是 NDK 29 所带的 Clang 21。C19 已实际完成 generate/train/use：Android 上四个固定场景正常退出，四份 raw 共 6794704 B，同版工具严格各合并一次，产生 2173952 B 的 indexed profile，SHA 为 `59038523…`。随后完成非插桩 profile-use 的 14 目标构建，主程序 `5a94db9e…` 无 LLVM profile 数据段和 runtime/counter 符号。训练、构建、相邻与累计计时和普通 PMU 72 点均已有证据及独立重放，采用为有限输入下的编译与既定指令质量检查点。

本次同源码参照选为暂存 C20：源码 `0b283cbb…`、非 PGO launcher `b8e5ae98…`，精确主程序及两文件差分已保全。generate 与 use 各自只改六项 cache 值；完整 Ninja 图剔除预定 profile 选项与缓存路径后等价于原通用配置，ASM/ISA、O3/NDEBUG、异常、栈保护和 ThinLTO 保持。插桩阶段只构建 launcher，训练输入 `86593fa4…` 已精确归档；use 阶段才重建 14 目标。测试入口没有被训练，12 条不相关测试 main 的 profile hash mismatch 按 LLVM 规则拒用相应计数并完整保留，没有生产翻译器的相关警告。

当前 profile-use 配方必须与该源码和 `59038523…` profile 一起保留。`CMAKE_C_FLAGS` 使用 `-fprofile-use=<profile>`；`CMAKE_CXX_FLAGS` 在原有 `-DFMT_CONSTEVAL=constexpr` 后追加同一选项；EXE/SHARED/MODULE 的公共 linker flags 也使用该选项。Release executable linker flags 保留四个 ThinLTO jobs 和 1 GiB 回收策略，仅使用独立 `thinlto-cache-pgo-c19-use` 目录。generate/atomic 选项已经移除，未改变默认 ISA。实际六字段完整 argv、14 目标构建 argv 和原始回退值分别保存在 `active-results/compile-cost-c19-pgo-r1-use/configure.json`、`build.json` 与 generate 的 `prepare.json`；其他构建目录须保留自己的既有参数，并使用匹配源码的训练结果。

受影响回归没有新增失败，原 22 个失败精确复现，353 native checks 通过；48 个 FP 发射缓冲与 C20/R6 完全相同。相邻七轮 56 点中，CoreMark Function/Block 的 translate 比值中位数为 0.888000/0.819807，FP 为 0.896223/0.836739，28 对全部小于 1，达到预定 5% 门。这是对同源码非 PGO 构建的结果，不把先前百分比相乘成累计改善，也不据此宣称达到 FEX 同数量级。

另外直接原 R6 的七轮 56 点已证明累计改善 20.59%–29.69%，全部 28 对小于 0.95。普通 PMU 的 12 个逐轮指令斜率比为 0.999781–1.003017，均通过既定 1.01 门。运行时间没有获得全体零回退证据：CoreMark Function/Block 的 wall slope 中位数增加 0.264%/0.677%，三轮均增加；FP 增加 0.961%/0.821%，各轮范围跨 1，Function 最大增加 4.715%。这些实际结果连同 cycle 增幅保留在执行记录，不能由指令门或 FP 字节相同推导出全部运行时间不变。

标准 generate/train/use 流程与代表性输入要求参见 [Clang PGO 指南](https://clang.llvm.org/docs/UsersManual.html#profile-guided-optimization)。本仓库的实际工具链和 Android 配方按上述冻结审查记录执行。

1. 先收口 C18 与其他前置机制包，冻结实际选定源码的非 PGO 参照及全部构建参数；暂存组合不能因此自动成为已完整采用版本。复用 Data2 固定构建树，分开有界的 generate/use LTO 缓存；保留 generic ISA、O3/NDEBUG、异常、栈保护及生产检查。
2. 使用标准 LLVM 插桩构建 launcher，训练期采用 atomic counter update，独立 `LLVM_PROFILE_FILE` 命名空间。仅复用既有 CoreMark2000、FP1 × Block/Function 四次正常退出的训练运行；这些运行只提供 profile，不进入性能结果。核对实际退出、profile 文件、权限和 LLVM 版本，再用同 NDK 的 llvm-profdata 严格合并，不能忽略损坏输入。
3. 以合并 profile 重建非插桩的 profile-use 产物。核对实际编译/链接参数、无 profile runtime 写盘，以及缺失或过期 profile 的诊断。完整运行受影响正确性门、发射核对及预定 coarse；进入采用验收时，再完成原三臂普通 PMU。
4. 单列训练覆盖边界：CoreMark 与 FP 两个固定负载的改善不能证明通用工作负载收益，两档工作量也不是独立程序。PGO 的工程构建时间、缓存容量和生成代码质量与 guest JIT compile 耗时分别报告。无收益或触发负控时恢复原参数和精确基线，不追加样本寻找通过。

PGO 没有引入源码计时器、探针或新的编译流程。训练写盘与插桩时间不进入性能表；正式性能环境不含 LLVM_PROFILE_FILE 或任何 generate/atomic 选项。最终组合仍须直接对原 R6 测量，不能把 ThinLTO、参数布局与 PGO 的百分比相乘。后续改变源码时，须使用匹配源码的 profile 或单列非 PGO 的同源实验，不能把旧 profile 的不匹配当成新版本已验证。

### 8.6 C20/C21：原结构候选与依赖限制

以下两项将首轮 C3 的搬运精简落实到当前剩余生产路径。FEX 的参考点是 RA 把物理结果写入既有 IR，由后端直接消费。SVM 保持冻结 allocation/actions，只借鉴结果由一个最终 owner 持有，不照搬可变 PostRA IR。

**C20 先删除 scratch payload 的重复持有。** 当前 `lower.cpp` 在每块尾部调用 `PrepareScratch`，生成完整 `ScratchActions` 临时结果；`allocation_preparation.cpp::StoreScratch(const ScratchActions&)` 随后再次逐行 push ranges/inputs 到 UnitActions。首批改为在全部原有边界与 owner 检查后转移首个最终 buffer，后续块仍 append；复用同文件 `StoreGPRResidences` 的现成模式。最终行序、块边界、unit scratch 一致性和失败 rollback 保持原值。

| 文件 | 修改职责 | 同批删除的旧工作 |
| --- | --- | --- |
| `backend/arm64/compile/allocation_preparation.{h,cpp}` | StoreScratch 接收最终结果所有权，先完整验证，再首块 move/后续 append | 仅为首块复制重新分配并持有的 ranges/inputs payload |
| `backend/arm64/compile/lower.cpp` | 在实际最后消费位置交接，保持每块准备顺序 | 不再把已完成的 scratch 结果当只读中间副本传入 |
| `backend/arm64/compile/scratch.{h,cpp}`、`scratch_preparation.{h,cpp}` | 首批保持 scratch 计算和按值返回的 PrepareScratch/LowerScratch 接口，现有 prvalue 调用可直接交接 | 不新增适配器、兼容包装或第二套计算入口 |
| 现有 allocation/actions 合同测试 | 复用多块、稀疏 ID、两个 prepared 共存、异常和重试覆盖 | 缺少实际语义覆盖才增加永久案例，不写逐行镜像测试 |

分别记录少了哪些最终 buffer 分配、复制行和同时存活的 payload，不将整个 scratch 桶记为收益。先处理独立 Block；单块 Function 和多块 Function 仍走自己的分配路径，分别核对首块转移与后续 append。不能借用 workspace 内存替代最终所有权，不能用大栈对象或全单元 reserve 换表面分配减少。

scratch 完成独立判断后，再审查 `StoreMemory`、`StoreIntegerFlags` 等同型 payload。`LowerNarrowBits` 等后续消费者仍需局部结果或 coverage；须列清最后消费者后才能移动。首批不同时取消 sort、重排 matcher 或改变 publication 顺序，以免混淆归因。

**C21 在最终物理状态上共享逐指令发现。** 当前 `scratch.cpp` 的 LowerScratch、`width_contracts.cpp::CollectWidthContractQueries` 和 `certificates.cpp` 的 flags-only-spill 资格访问分别扫描 IR。拟沿现有 Finalize 收集阶段共访，产生 scratch 结果、width 查询摘要和 flags-only 候选，后续 width/fixed 证明继续执行原算法。

进入实现前必须证明：将 scratch 尾准备从 per-block seal 移到 `compiler.cpp::Finalize` 的 `SealTargetEdits` 后、`LowerUnitEdges` 前，不改变早块 scratch 与后块 recolor 的语义；pool/features/attempt/generation 相同，invalid SSA 与 FPR retry 出口不取得半成品。若此证明不成立，保留原 scratch 时点，收缩为只合并实际同一时点的只读访问，不能缓存早期不稳定查询。

涉及 `lower.cpp`、`compiler.cpp`、`scratch.cpp`、`width_contracts.{h,cpp}` 和 `certificates.{h,cpp}`。只共享 collector 与已有 owner 下的阶段临时输出，不增加全函数镜像表或通用 visitor 框架。`WidthContractCheck::Verify`、`VerifyFixedConstraints`、独立 audit、当前 Certify 次序及 generation/attempt 绑定保持；现有 Certify 在 LowerUnitEdges 之前，整个 prepared 结果仍须通过既有准入才供发射和发布。carry、compare、memory 等具有顺序依赖的 matcher 不在本包合并。

C20 首批已完成 22 套件、353 native checks、48 个发射缓冲及七轮 56 点对照和独立重放；无新增失败，发射字节等于 C18/R6。四组计时范围跨 1，没有证明 5% 提速。两次每模式的独立容量诊断中，Block 冻结读数减少 428440 B，Function 增加 33080 B，实际工作量也不同，不能当同工作量改善或峰值变化。暂存为净所有权清理，保留 Function 容量增加、Complete 帧增加 32 B、native .text 增加 264 B 等代价，不作普遍内存或稳定速度改善承诺。

选定同源码版本的 C19 PGO 编译与既定指令质量检查点已收口，原始结果和运行限制单独保留。C21 在依赖审查通过后单独进入。每项先运行已有质量门和 48 个发射缓冲核对，再按第 6 节做预定 coarse；后续选定组合仍须直接原 R6 与普通 PMU。若收益不足且引入额外生命周期复杂度，撤回，不把这些候选相加成缩小 FEX 差距的承诺。

### 8.7 C24：普通 Block 精确故障站点批次，已撤回

当前 PGO 普通采样中，Block 的 AddFaultEntry self 为整个进程树的 2.73%；这只支持查找重复工作，不能直接换算成 compile 收益。代码确认每个精确站点重复取得 publication scope、查找同一 catalog owner、复制快照和交换回收。emitter 已拥有完整的相对地址向量，可以在原调用位置借用它，直接投影到一个最终 Snapshot，避免建立绝对地址镜像或新的 owner 索引。

本项借鉴固定 FEX 在代码对象完成后一次物化相对 PC 映射、随后公开入口的边界。SVM 的故障恢复类型、信号安全 catalog、retirement 和 SMC 合同继续使用自己的实现。仅修改独立 `IntrusivePtr<Block>` 路径的精确站点调用：Flush、whole fallback、backedge fallback、精确站点、Cached、link target、SMC 和 disk recording 的先后次序保持。Function 已在故障循环前公开 L2/call/direct 入口，批量延后可见性的证明另立，不同时扩展；HIRBlock、RV 和缓存恢复路径也保留原调用。

唯一 `FaultMetadata` 定义移到既有 `backend/fault_entry.h`，保持字段、默认值和布局，emitter 仍拥有向量并供后续序列化使用。`Module::AddFaultEntries` 接受同步 span，空批次保持原零次循环的无操作语义，非空批次先完成全部生产边界检查；`FaultCatalog::AddSites` 复用既有 owner 的查找与创建分支，按输入顺序执行原 lower_bound 插入，保证同起点的旧优先规则，成功构建后才一次发布。原 SC reader grace、回收和单条 API 的真实其他消费者保留。

生产差分和新增测试已经独立审查；16 目标实际构建成功，原 22 套件无新增失败，原 353 个原生检查和新增 Fault4/Terminal6 的 2992 个检查通过，FP48 字节相同。七轮 56 点及完整独立重放结束后，CoreMark Block 中位比 1.017876，范围 0.978874–1.022800，三快四慢，没有通过提前冻结的 5% 提速门。四组都跨 1，不能称为“七轮全慢”的正式负控，但该新增 API 缺少实际收益，已经撤回。

候选源码、测试、精确 ELF 和全部结果保留；8 文件精确恢复到 `0b283cbb…`。该实验使用同源 C20 非 PGO `b8e5ae98…`，没有在改变后的源码使用旧 profile，也没有补点或重新包装为已采用清理。

### 8.8 C25：发射 scratch scope 的小对象所有权，生产修改已撤回

extended pool 下，每次 BeginVixlScratch 的 make_unique 和 End 的 reset/free 仍在逐 IR 发射路径；当前 PGO 调用链确有 TickIR→new→Scudo。保留现有 JitContext，改用 optional 在相同位置构造、关闭同一个作用域对象，去除这一层反复堆分配。只修改 jit_context.h/.cpp 两行，已有 bool/arrow/reset 与 nested、exception、Close→SvmEndScratchContract 和预算拒绝保持；没有增改 VIXL API、手工状态、TLS 池或配置开关。

ARM64 实物中 scope 仍为 32 B，候选 JitContext 从 1368 B 增至 1400 B，独立 Block 翻译入口的固定栈帧从 5200 B 增至 5232 B。Begin/End 的原 new/delete 调用已消失，其自身固定帧各从 96 B 缩至 80 B；这些帧读数不能相加当运行峰值。XPOOL0 保持 disengaged，单块 Function、多 region 共用 assembler 和重发路径按各自原生命周期运行。原 22 套件之外补跑两个已有关键用例各 XPOOL0/1，零 case、skip 或新失败均不算通过。

首轮 r1 的定向门在发射前失败：旧 indirect-target 夹具将全部 FPR 标为不可用，原 R6 也精确复现同一 allocation 错误。r1 的失败和精确测试 ELF 已保留，没有改为通过。r2 独立登记测试修正，仅令夹具的 FPR 正常可用，原三寄存器 GPR 压力、四项反汇编断言和全部生产检查保持。源码成为两份生产文件加一份测试文件；重建实际只改变 swift_test，其余 15 个 ELF（含主程序）逐字节等于 r1。修正后的四个定向命令实际通过 24 项断言；一致的 C20 生产源码加同一夹具修正，也通过相同四命令及 24 项断言。随后已恢复并重建候选，全部 16 个 ELF 精确等于 r2，不混用两种 context 布局；不把旧 R6 的无效夹具失败当修正后的测试结果。

七轮 56 点及独立重放已完成，全部 730 个 raw、93 项 artifact 和 28 对结果有效。性能参照是精确 C20 非 PGO，未在新源码使用旧 profile。CoreMark Function/Block 中位比为 0.988763/0.989275，范围分别为 0.976529–1.009396 和 0.950854–1.027561；FP 为 0.994928/0.983771，两组也都跨 1。两项 co-primary 均未满足七对全部更快且中位至少改善 5%，没有触发任组七对全慢的正式负控；生产两行因正门失败撤回，没有补点或重测，也未进入匹配 PGO/普通 PMU。

独立验证过的夹具修正保留，当前源码 `8401918b…` 恰为 C20 生产源码加该测试修正。候选源码差分、精确主程序压缩件及全部失败和有效结果均保留。16 目标恢复构建完成：原 C20 的 13 个非 swift_test 产物和修正夹具控制的 swift_test 均精确一致，额外两目标记录实际身份。手机主程序已恢复选定 C19 PGO，host cache 保持通用构建，未宣称恢复全套 14 个 PGO 产物；原 R6 和 previous 角色未动。C19 的原源码/profile/精确 ELF 与既定资格保持，完整 FEX 同数量级目标仍未完成。

### 8.9 C25 之后的结构审查顺序

固定 FEX 的参考点继续是结果在需要时由既有 owner 建立、后续直接消费。以下记录保留 C25 之后的实际审查顺序；C26、C27 都已完成实验并撤回，不再是待实施路线。下一阶段优先级以第 4 节为准。

| 顺序与状态 | 真实工作与文件 | 最小方案和进入条件 |
| --- | --- | --- |
| C26 已撤回 | `compile/allocate/ties.cpp::InitializeTargetInputs` 的构造期 clobber 构建删除实验 | 质量门通过，但 CoreMark Block 七轮全部更慢、中位增加 1.85%，触发预定负控。生产删除和四个测试前置调用均恢复，未带入 C27；见 8.10 |
| 可行性审查，暂缓实现 | `arm64/jit/jit_context.{h,cpp}` 的 Label 与入口 offset/contract 记录，以及 `function_code_object_emitter.{h,cpp}` 的最终消费 | FEX 按 Block ID 访问 JumpTargets 可作参考，SVM 还需外来目标、地址稳定和跨 region 身份。先列清各字段的生产者、重复规则及最后消费者，再判断能否在既有最终 owner 中聚合真实重复记录；不能先加第二份目录或借临时 span 逃逸 |
| C27 已撤回 | `compile/allocate/{internal.h,linear_scan.cpp,ties.cpp,fpr.cpp,spill.cpp}` 的 active_lives | 单 vector 的源审查、实物与质量门通过，但两项 co-primary 均未通过预定 5% 门，四组范围都跨 1；五文件和全部 16 个 host ELF 已精确恢复，见 8.11 |

入口记录的跨 region `AbsorbEmissionState` 使用 `std::map::merge` 移动原节点，不是再复制分配全部记录；单元素 Function span 与独立 Block 也没有跨 region merge。仅把八棵树搬到统一 owner，不能声称消除了节点分配。独立 Block 的五棵 offset/contract 树没有生产插入；本地 pending offset 与 contract 配对在源码层面可行，每个 pending 入口少一个节点、两次查询变一次，但只对 Function 有潜在动态收益，三棵 Label 树继续存在。全量 IR arena、宽度证明替代、提前使用未封口事实、整套 assembler 复用和旧拒绝候选均不因这一审查重新进入实施。

### 8.10 C26：删除分配前被覆盖的 clobber 构建

源合同和实际 ARM64 产物均确认：原构造时的 clobber 结果没有生产消费者，随后安装正式输入约束后立即重建。仅删除 `ties.cpp` 的一次调用，`fixed_class` 保持原构造阶段求值，`AllocateRegisters` 继续执行 InstallInputConstraints、重读 GPR pool 和权威构建。三个生产构造点全部闭合；十五个测试直接构造中，四个 GPR/width 手动 coalescer 在 tests/support 显式补上同一前置初始化，其余七个走完整分配、三个 FPR coalescer 和一个 spill-reload helper 保持原值。没有新状态、表、缓存或生产配置。

候选源码 `030cc635…` 相对已恢复基线 `8401918b…` 只涉及上述两文件，实际差分为生产删 1 行、测试加 4 行。16 目标通用构建已完成，主程序 `2560a881…`。实物中构造期第一次 clobber 调用消失，后置权威构建仍在；LinearScan 仍为 1104 B，40 个成员偏移与所查入口帧未变。构造代码增加 16 B、整体 .text 增加 40 B，不能把源代码删除或调用消失直接当总体提速。

原 22 套件没有新增失败，原 22 项 R6 失败仍精确复现；353+993 项 native checks 通过，原四个定向命令通过 24 项断言，FP48 实际字节等于 C20/R6。按 root 或 SECTION 前缀复核旧 XML 后，另补七个既有案例：每 pool 展开 42 个 SECTION rows，旧实现及候选在 XPOOL0/1 下各通过 84 行、4016 项断言。没有新增永久测试。候选合计 716 行、867964 项断言，包含原 22 项受控失败；基线的 4016 项单列，不加入候选总数。

最初收集器把七个 TEST_CASE 误计为七行 JUnit；首个旧实现运行实际通过全部 2008 项断言，收集器因 42 行而拒绝。原 failed 记录及缺少 postidentity 的限制保留，独立按源码与 Catch 的完整 SECTION 名册接纳其 raw，只接续未执行的 XPOOL1，没有重跑 XPOOL0。另一处沿用旧 14 目标基线的元数据检查也在任何性能点前修正为实际 16，原错误和一处比较常量的差分保留。

七轮 56 点及独立完整重放已结束，150 项 artifact、730 raw 和 28 对结果有效。CoreMark Function 中位比 1.001054，范围 0.990438–1.042823；Block 中位比 1.018476，范围 1.002174–1.042046，七对全部更慢，触发预定负控。FP Function/Block 为 1.006813/0.986538，均跨 1。两 co-primary 也都没有通过 5% 门；Core 两模式各六对存在工作量差异，原样保留，FP 无差异，没有归一化、补点或重测。

C26 两文件已经精确恢复 `8401918b…`，源码与精确 ELF 压缩件及全部结果保留，随后才实施独立 C27。C27 保留原构造期 clobber 调用，未重新包装失败候选；继续复用通用构建缓存，没有为了转入下一实验重复重建一次完整基线，也未将仍在更新的产物贴成选定 PGO。C19 PGO 原身份保持，C26 未进入 PGO、直接原 R6 或普通 PMU。

### 8.11 C27：单 vector 实验，已撤回

候选使用现有 vector 承接活跃区间，删除另一份 list 容器及每 interval 的节点持有。独立 Block 和多块 Function 继续原慢路径顺序；合格的单块 Function 继续原 heap。编译单元、物理分配范围、pool、reserve、spill/eviction 和最终验证没有改变。

| 文件 | 候选当时的修改职责（现已撤回） |
| --- | --- |
| `compile/allocate/internal.h` | 只保留一个 Vector<LiveInterval>，没有新状态或 workspace buffer |
| `compile/allocate/linear_scan.cpp` | 同点插入；fast 保留原 reserve 和 push/pop heap；slow 按原顺序释放并单次稳定压缩，保留 end < start 边界及 MEM/FPR 双槽回收 |
| `compile/allocate/ties.cpp`、`fpr.cpp` | 共用原 find/erase；fast 继续原 make_heap，slow 保留剩余顺序；构造期 clobber 调用保留 |
| `compile/allocate/spill.cpp` | 在同一份 active 数据中收集事实和选择 eviction，保持原路径遍历与平局规则；best 只在函数内存活并转为 ID |

源码 `061d505a…` 相对基线 `8401918b…` 仅五文件，差分 +35/-53；测试代码不变。16 目标已实际构建，主 ELF `054b77a1…`。独立源审查和实物终审均通过：LinearScan 1104→1080 B，Function/Block 的 RunAttempts 入口帧各减少 32 B；.text 减少 800 B，AllocateRegisters 自身增加 136 B。原 slow 每 interval 的 new/delete 节点调用消失，vector 的容量增长、旧 buffer 释放、搬移与 heap 成本仍在。没有宣称全进程零分配或普遍峰值内存改善；扩容期间新旧 buffer 共存的代价保留。

原 22 套件没有新增失败，原 22 项 R6 失败精确复现；候选加四个 focused 和两个 targeted 命令后为 716 个 JUnit 行、867964 项断言。1346 个 native checks 通过，FP48 的 PC、顺序和实际字节相同。历史基线 source840 的 84 行/4016 项 targeted 结果独立复用，不重跑旧点、不加入候选总数，也不伪造旧 XPOOL0 的 postidentity。

原七轮 56 点及独立重放已经完成，146 项 artifact、730 个 raw、28 对结果有效。以下均为同配方通用候选 / 独立 B8e；未补点或替换原始观测。

| 负载与模式 | translate 中位比 | 七轮范围 | 更快 / 更慢 |
| --- | ---: | --- | ---: |
| CoreMark Function | 0.997848 | 0.969216–1.025454 | 4 / 3 |
| CoreMark Block | 1.007654 | 0.989241–1.018026 | 3 / 4 |
| FP Function | 0.993502 | 0.975340–1.018911 | 4 / 3 |
| FP Block | 0.983840 | 0.971324–1.025671 | 4 / 3 |

两项 co-primary 均未通过七对全部更快且中位至少改善 5% 的门，四组都跨 1；没有触发任组七对全慢的正式负控。CoreMark Function/Block 分别有 2/7 对工作量差异，FP 没有差异，原样保留而不按 IR 数归一。候选因正门失败撤回，没有凭容器更小或节点分配消失保留。

五文件已恢复为 `8401918b…`，随后实际完成 16 目标恢复构建；全部 16 个 host ELF 的 SHA 与大小均精确等于同夹具基线。手机主程序恢复为选定 C19 PGO `5a94db9e…`，15 个独立测试 ELF 为恢复后的通用产物，三个原参照角色保持不变；未称全套 14 个 PGO 产物恢复。私有上传目录与展开的临时 PGO ELF 已移除，最终恢复收据独立核对 2418 源文件、16 个 host 产物和 19 个手机角色。本候选没有进入 PGO、直接原 R6 或普通 PMU，C19 资格及完整目标的未完成状态保持。

相关文档：[结构重构方案](middle-pipeline-rebuild-plan-2026-10-21.md)、[历史结构验收](middle-pipeline-rebuild-progress-2026-10-21.md)、[当前性能与代码质量](compile-codegen-performance-2026-10-26.md)、[红魔测量工具](../tools/redmagic/README.md)。
