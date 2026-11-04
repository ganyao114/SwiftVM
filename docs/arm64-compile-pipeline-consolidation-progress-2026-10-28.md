# ARM64 编译流程整改实施记录

日期：2026-10-28。设计与验收依据为 [整改方案](arm64-compile-pipeline-consolidation-plan-2026-10-28.md)。目标包含完整的数据流收敛、目录整理和代码生成质量验证，不能以单个试点完成代替整项完成。

后续性能分析见 [spill 增长、compile 成本及 FEX 两倍目标方案](spill-and-compile-root-cause-plan-2026-10-28.md)。本轮结构整改的通过不替代 CoreMark 运行回归或 compile 性能验收；后续诊断与新目标在那里独立记录。

## 基线与结果位置

整改从包含既有 WIP 的工作树开始，Git HEAD 为 `660b6ba14e3c8b6e6e21dc601fb66072b07b191f`，不能只用该提交代表基线内容。

结果根目录：

```text
/Volumes/Data2/swiftvm-middle-preserved-20261025/arm64-compile-consolidation-20261028/
```

`baseline/receipt.json` 记录源码与 16 个原有 Android 产物的身份。`baseline/source.tar.zst` 已逐项读回校验，保存 2418 个源码文件；另存 Git 状态、原有 WIP patch 及构建配置。后续大文件继续使用 Data2 和固定 Android 构建缓存。

## 已落盘的改动与最终验证

| 范围 | 当前变更 | 验证状态 |
| --- | --- | --- |
| PreparedUnit 借用 | Function emitter 直接借用 prepared span，删除重复的裸 view 容器；保留全单元准入、CFG/入口检查及 fallback 的借用寿命 | `layout-r7` 完整质量组及30组独立生命周期检查通过 |
| scratch 入口 | `PrepareScratch` 先并入原 scratch 模块，随后随目录迁移由 `registers/resources.h` 声明、`registers/scratch.cpp` 实现；删除旧 preparation 文件，更新构建和测试引用 | 合并时函数体与基线逐字一致，`layout-r7` 资源合同及完整质量组通过 |
| width/零扩展 | checker 直接认证私有结果，退役三个 proof-ID 中转容器及搬运；To32/To64 的 emitter、TickIR、spill 消费者共用稀疏决定，退役旧 final bits/rows；随后删除仅复制普通零值/home事实的 action | `layout-r7` 扩充质量组通过，包含普通零扩展及全部19个具名copy/width用例 |
| 准备结果及 reset | 显式区分 `Rejected`、物理重试及成功；重复拒绝保持；Begin/Abort 共用唯一 payload reset 清单 | 完整质量门及失败注入生命周期检查通过 |
| memory | transfers/addresses 单 owner、直接物理写回 witness、退役静态 EA recipe map，保留实际省略 marker；access/coverage 直接提交，删除 `MemoryActions`、`StoreMemory` 和 `memory_preparation` 两文件 | 完整质量门通过，包含稀疏/非单调编号、首候选及 fault 合同 |
| coverage | 后续选择器借用最终 owner，删除 `flag_coverage` 及重复 set 循环；predicate 覆盖在 integer-flags 后、narrowing 前提交 | 真实选择器阶段、reset/abort 和完整质量门通过 |
| 领域提交 | 八类提交消费已有缓冲区，整数 flags 只借用最终 owner 的当前块 span，copies/Consumer 在最后消费者后移动；删除两个纯 vector DTO | `layout-r7` 相关用例及完整质量组通过 |
| FPR admission | 生产 verifier 直接认证同一 owner，退役 instructions/vector/导入链；唯一 Phase 替代 ready/building；所有 block 成功后才开始 GPR edits；诊断/算法测试的 FPRContract 保留 | `layout-r7` 扩充质量组通过，包含全部canonical shuffle控制、私有admission、实际部分认证后拒绝及物理重试 |
| 目录/API | 实际迁移为12个公开头、3个私有头、37个实现文件；删除81个旧路径，更新消费者与构建列表，无兼容别名 | `layout-r7` 全16目标构建及扩充质量组通过；逐文件职责见[实际布局](arm64-compile-pipeline-layout-2026-10-28.md) |
| flags-only spill/零存储中转 | 生产 checker 直接记录 flags-only spill，删除 `EmissionCertificates`；零存储直接追加到原全单元 pending vector，保留原最终认证时点 | `layout-r7` 全16目标构建及扩充质量组通过 |
| canonical backedge 入口 | 静态透明后继闭包在封口时直接写已有 edge row 的一个 bit；JIT 复用已有查询并保留实际 storage/ABI 晚门，删除重复链遍历 | `layout-r7` 完整质量组及30组独立生命周期检查通过；四种Block/Function与pool组合均实际执行新增闭包用例 |
| 静态 memory 对齐 | Add/Sub/bitcast 地址链、home residue 和失效分析移至最终coverage/backedge之后；同一memory记录保存普通/实际backedge激活两种事实，删除JIT静态map与Track | `layout-r7` 完整质量组及30组独立生命周期检查通过；更新/失效、实际激活及真实guard/执行/fault用例均已执行 |

普通 Release 配置已在固定构建缓存中完成：与封存配置比较，只有六个预期 flags 发生变化，移除旧源码的 PGO profile，保留 O3、NDEBUG 和 ThinLTO。配置不构成构建或测试通过的证据。核对结果见 `initial-structure-check.json`。

`builds/structure-r1` 在两处旧测试接口调用处失败，原始输出保留。修正封口后不可再写证明的 fixture 后，`builds/structure-r2/build.json` 记录全部 16 个目标构建成功、构建前后源码身份一致及产物快照。

`quality-runs/structure-r2-q1` 在第五项调用中发现两项新增用例的问题并停止：诊断 fixture 将 Presence 开关的 `"0"` 错当作关闭；非单调编号用例暴露 `BlockAnalysisIndex::InstructionPosition` 在未排序 roster 上使用二分查找的缺陷。前者改为 Unset；后者保留正常稠密/有序路径，为罕见未排序输入增加精确指针查找，并扩展通用 index 回归。保留原失败输出，不把该轮记为通过。

修复及 coverage/memory 增量已纳入 `builds/structure-r3/build.json`。该阶段完整既定检查无新增失败、FP48实际缓冲区一致；Function 对照工具另修正了解码函数与发码对象的口径、guest stderr 紧接日志前缀的解析，以及未经基线核对的1000数量下限。原失败收据保持不变，`structure-r3-reviewed-gate.json` 记录对实际完整原始输出的复核，不以改写原收据消除错误。

三项后续数据增量已合入32个文件；四处冲突经人工处理、独立审查，23个受影响源文件先在结果目录通过真实 NDK 参数语法检查。`builds/structure-r4/build.json` 随后记录全部16目标真实构建成功、构建前后源码身份一致。

`quality-runs/structure-r4-q1/execution.json` 正常完成，无需离线修正其状态：54项既定检查、2项新增筛选及11个guest的33次Function对照均完成；22条历史失败逐项保持，无新增失败。FP48按原guest PC、顺序和实际bytes完全一致。解码函数951、发码对象930分别记录，逐guest与普通构建基线匹配；不能把PC/IR指令数签名当作全部IR内容或机器码的哈希。

`lifetime-runs/structure-r4-lifetime-r1/execution.json` 另记录已有15个模式×两种IR构建设置的30次独立检查全部通过：8157924次checks、516次注入失败，包含prepared跨线程、多region、优化结果移交、IR退出及重试。这些计数单独记录，不与native20的1773项parent/single或8个child的392项混加。

目录/API 迁移与普通零值 action 去冗余随后实际应用到源码。`layout-r1`、`layout-r2` 分别在新增 fixture 的值类型和显式 vector 头引用处构建失败，修正后 `builds/layout-r3/build.json` 记录全部16目标成功。实际52文件及 CMake 集合已核对；73个本地文档链接和24个固定 Git 引用核验通过。

`quality-runs/layout-r3-q1/execution.json` 仍为失败记录：原54项既定调用及FP48完成，无新增历史断言失败；扩充的 `shuffle-plan` 用例在 `scenario=5` 崩溃。该场景刻意不创建 shuffle result，新增的 abort 检查却无条件访问 `result.Id()`；已加 Defined 条件，保留 mask 拒绝、abort 清理及所有实际 result 的检查。其后尚未执行的具名 copy/width-fact、IR admission 和33次 Function 对照不能计为通过。修正后随 flags-only spill 增量进入 `layout-r4`，失败原始输出保持原状。

`builds/layout-r4/build.json` 全16目标成功。该轮原54项及FP48再次完成；扩充组中的外部 CFG 负例触发原有 `SVM_VERIFY_PLAN=1` 诊断异常，而 fixture 只接受普通模式的 `Rejected`，因此 `quality-runs/layout-r4-q1/execution.json` 仍记失败。用例已显式遍历两种诊断设置、恢复环境并核对各自行为；生产 assertion 保留。后续未执行的组仍不记通过。

`builds/layout-r5/build.json` 全16目标成功，`quality-runs/layout-r5-q1/execution.json` 正常完成：54项既定调用、124个领域用例、19个具名copy/width用例、8个IR admission用例，以及11个guest的33次Function对照均完成。19个具名用例与源码清单逐项一致。FP48实际缓冲区、guest PC和顺序保持相同；22条历史断言失败逐项保持，无新增失败。新增真实中途FPR拒绝用例覆盖先认证有效前缀、随后拒绝、Abort及保留输入的物理重试。此轮证明目录迁移和 flags-only spill 增量；之后的静态分析增量须另验。

`builds/layout-r5/source.tar.zst` 封存该轮全部2382个源码清单项。因之后已合入canonical增量，归档时从其冻结base恢复9个文件的旧内容，再逐项比对构建manifest；归档全部读回SHA校验通过，详情见同目录 `source-checkpoint.json`。这份源码与该轮16个产物、质量收据绑定，不把当前后续工作树冒充已验证版本。

`builds/layout-r6/build.json` 记录 canonical 与 alignment 合入后的全16目标成功。`quality-runs/layout-r6-q1/execution.json` 在首个综合合同组失败，未进入后续组及独立lifetime：新增activation fixture将会破坏pending NZCV的TSO访问放入self backedge前缀，原selector按合同拒绝；新增执行fixture只留下两个scratch寄存器，原预算检查拒绝。两项均须修正测试构造，保留生产选择、预算和检查；该失败轮不计验收通过。

## 最终版本与验收范围

`layout-r7` 仅修正上述两处fixture，未修改生产选择、预算或检查。`builds/layout-r7/build.json` 记录全部16目标成功，源码摘要为 `3ef7f002fc18183881661138e51fbe732f3de54b05ec9b62384b7d23288e92c3`；构建前后源码身份一致。以下设备验证均使用该版本，在红魔 `bf1359ff` 的独立目录运行。

| 证据 | 最终结果 |
| --- | --- |
| `quality-runs/layout-r7-q1/execution.json` | 正常完成54项既定调用及3项扩充筛选；124个领域、19个具名copy/width、8个IR admission用例无失败。22条历史断言失败逐项保持，无新增失败、崩溃或跳过。 |
| 实际FP代码缓冲区 | Block42个/6736字节、Function6个/7992字节，按guest PC、顺序和实际bytes全部一致。 |
| Function对照 | 11个guest×基线/A/B共33次完成；同轮基线与候选的输出、逐对象PC/IR指令数签名、单元数量及逐对象代码大小一致，无Function→Block静默回退。该轮解码函数949、发码对象928，分别记账。 |
| 新静态分析用例 | 新增对齐更新/失效、真实activation、实际guard/执行，以及canonical闭包用例均出现在四种Function/pool综合组的实际用例清单中。 |
| `lifetime-runs/layout-r7-lifetime-r1/execution.json` | 15个已有模式×两种IR设置共30次独立调用全部通过；8157924次checks、516次注入失败。 |
| 目录与接口 | CMake与实际52文件集合一致：12个公开头、3个私有头、37个实现文件；81个旧路径及旧类型的生产引用退役；75个本地文档链接有效。 |

Function签名在此是PC与IR指令数对照，不能据此声称全部IR或全部机器码哈希相同；host helper绝对地址、relocation等差异不属于FP48字节一致的范围。逐对象代码大小相同也不独立证明每条指令都相同。本轮结论结合合同回归、静态迁移核对、实际FP字节、Function对照及执行/fault测试成立，不外推为新的FEX运行或compile成本结论。

本轮整改及既定验收已完成。原始失败收据、普通构建基线、源码快照和最终产物保留在上述Data2结果根目录；最终2382项源码归档 `builds/layout-r7/source.tar.zst` 已逐项读回校验，16个产物位于同目录 `bin/`。最终证据索引见 `final-audit.json`。后续源码变更须更新构建与验收证据。本次没有提交或push；历史FEX性能工作未恢复。
