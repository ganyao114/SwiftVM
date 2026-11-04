# SwiftVM / FEX compile 差距审查

**当前路线复审见[42](#performance-route-correction-latest)，当前实现、计时及归因资格统一见[主方案7.1](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)。** 原1～41节按各自source和审查日期保持，其中95bd进行中、某C批次normal关闭等是当时状态，后修不回填。

2026-11-02对照[用户最后明确要求修订的546行版本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-recovery-20261102/04-last-user-directed-20261031/docs/spill-and-compile-root-cause-plan-2026-10-28.md)：两倍目标、最终6.3/6.4与6.5停止规则未改变。后续实质变化是semantic接管时点、actual机器输入和稳定operand交付；有必要正确性补强，净工作与及时完整反馈仍未闭合。当前8cda的A已构建、原三例2过1新增fixture前提失败，无新ordinary；e0d6仍B44.33×/F23.24×。重构保持暂停，本轮仅修文档。

下文各轮保原证据身份；结构完成、默认true、旧API退出不能换算为性能资格。

## 1. 当前源码和计时属于不同版本

- 原审查时共享源码已核为 `3ef11319b014832d240b8319b1d6342c2153b6549e28c7557719e2d2f9444098`。native 默认 true、必要时 false 重发已经恢复；唯一 sealed IR / Allocation / Selection 保留至最终 Flush，随后退役。该版本在当时已有构建和最少语义结果，**尚无完整 compile 计时**；后续状态以第10节及主文2.1为准。
- 最近有同轮 fresh FEX 分母的版本是 `da7962c6`。下表只属于它。
- 后续 `0641c577` 的 false-link 开发对照是 Block 745.145→749.408 ms、Function 885.665→860.270 ms；没有显示大幅收益，也没有 fresh FEX 分母。不能把这个点或 da 的比值标成当前 3ef 的结果。
- 审查时主方案7.1仍写当前为0641、重发尚待实现；后续文档修订已纠正为3ef。审查还发现外部checkpoint中名为`current_source_ordinary_translate_ms` / `current_source_fresh_FEX_ratio`的旧值；这些标签不能代替原测量身份，现行方案明确当前3ef未计时。

证据：[M0 原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m0-current-fex-baseline-20261031/compile-gap-summary.json)、[0641 四点账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/typed-current/direction-prepared/accounting.json)、[3ef 构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/build-stage-delayed-retirement-20261031/build.json)、[3ef 最少语义](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/native-output-prototype-20261031/delayed-retirement/minimum-semantics-summary.json)。

## 2. 耗时账已经否定“只改一个局部即可根治”

以下为 da 的同轮普通开发点，单位 ms；每种模式只有一对，不是最终统计验收。

| 原始统计范围 | 独立 Block | Function |
| --- | ---: | ---: |
| decode/build | 111.158 | 165.636 |
| RPO/编号 | 0 | 6.562 |
| opt | 102.233 | 89.723 |
| regalloc（含目标输入准备等） | 167.879 | 170.426 |
| codegen（含 Complete，不是纯编码） | 299.320 | 325.891 |
| publish | 35.229 | 29.074 |
| ir_free | 0.714 | 0.705 |
| other | 75.400 | 92.347 |
| **SVM 完整 Translate** | **791.933** | **880.364** |
| **各自 fresh FEX CompileBlock 累计** | **47.477** | **47.276** |
| **比值** | **16.68×** | **18.62×** |

三个直接推论：

1. 要到 2×，完整 SVM 成本仍需减少约 **88.0% / 89.3%**，即再改善 **8.34 / 9.31 倍**。几个小项不能填满这个差额。
2. regalloc + codegen 占约 **59.0% / 56.4%**。即使这两桶理想化全部归零，仍剩 **324.733 / 384.047 ms**，高于约 95 ms 的目标。构造/公共优化不能退出主线。
3. decode + RPO + opt 已达 **213.391 / 261.921 ms**；publish + free + other 还有 **111.342 / 122.126 ms**。other 含真正编译工作，不能统称管理开销或可删除预算。

阶段名也不能跨模式直接解释为相同内容：当前 [pipeline.cpp](../source/runtime/compile/pipeline.cpp) 中，Block 的 opt scope 覆盖 SealIR / FromFinalIR；Function 在它们之前停止 opt。首次 Facts / SSA 的部分费用因此在 Function 的 other 中。完整总量可以比较，分桶解释必须跟随调用边界。

另两个既有 PROF2 诊断进程显示：实际 assignment **18.483 / 27.122 ms**，sort **0.822 / 0.353 ms**，regalloc_total **171.328 / 169.083 ms**，AllocateBlock/Function **52.301 / 58.031 ms**，codegen_complete **236.431 / 262.863 ms**。这些是带观察成本且有嵌套的范围；Complete 项还含 FVI，不能相加、与普通点相减，或全部标为可省时间。[诊断解释](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-m0-current-fex-baseline-20261031/M0-result.md)。

FEX 的约 47 ms 覆盖整个 CompileBlock，包括 decode、IR、passes、RA、emit、IR 交还、SMC 和入口发布；并非拿 emit 对 SVM whole。其线程/编译器初始化、外层 dispatcher lookup、server 启停不在该累计内，资源物理回收也可能更晚；端点差异仍保留，不能用系数修正。已有 19.2 MHz 换算与 final ticks 相符。[固定 FEX 源码与计时边界复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)。

## 3. 首要结构问题：目标描述、分配、Complete 和绑定尚未成为一条简短的决策链

当前正常路径仍实际执行：

```text
sealed IR + 首次公共 Facts
  → target Describe：FPR/GPR/width/fixed/scratch/consumer 等输入
  → common RA：live/interval、约束、位置、coalescing、spill/repair
  → Complete：架构状态、publication、width、最终选择、实际 writes 核验
  → 结合原 IR + Allocation + Selection，形成当前操作数与运输资源
  → Tick / scratch / 编码 / 发布
```

**源码事实：**

- [constraints.cpp](../source/runtime/backend/arm64/compile/constraints.cpp) 的 `DescribeBlock`（279 起，主循环 344 起）遍历指令及输入，生产多种合同；随后还有 Finish、排序和初始 fixed 约束。它已经合并了若干访问，但共用一个外层循环不意味着循环内的工作消失。
- [compiler.cpp](../source/runtime/backend/arm64/compile/compiler.cpp) 的 `DeclareGPRBlock`（1255 起）和 `Complete`（1303 起）仍完成 register state、GPR declaration、publications、memory/flags/vector/narrow、scratch、实际物理写入核验和 edges。
- 同文件 961–1014 的正常 native 循环仍读取原语义，选择 family writer，查询 scratch/flags，绑定当前参数与资源，再调用 emitter。typed-current 取消了打包/解包，不会自动取消这些生产者。

以一个可合并的 `SetHostGPR` 为例，实际链条可以跨越：

| 阶段 | 当前源码位置 | 真正在决定什么 |
| --- | --- | --- |
| 输入描述 | constraints.cpp:361、408、432 | 静态 publication 候选、fixed 事件和 GPR 输入 |
| 真实分配 | [ties.cpp](../source/runtime/compile/allocate/ties.cpp):224–253 | birth 能否取得 home；实际 mapping / fixed definition / coalescing |
| 架构状态 | [state.cpp](../source/runtime/backend/arm64/compile/state.cpp)、[state_publication.cpp](../source/runtime/backend/arm64/compile/state_publication.cpp) | guest-copy/version、home width、观察点和 fault 状态 |
| 最终 publication | state_publication.cpp:444；compiler.cpp:1364、1391 | 最后位置修改结束；依实际 writers 核验 publication，决定 Value/Skip 等 |
| 交付与绑定 | compiler.cpp:1422；[select_memory.cpp](../source/runtime/backend/arm64/compile/select_memory.cpp):344 | 写入唯一 recipe；读取真正 source/home/role，形成可编码操作数 |

这些并非全是同义复制：SSA location、guest-copy、宽度和 fault recovery 是不同事实，候选不等于最终实际 writes。部分普通 birth 已经避免晚重选，不能重复认领。问题在于职责之间仍依赖大量重新解释与查询；唯一 owner 只解决了谁负责，尚未充分解决负责一次要做多少工作。

**与固定 FEX 的关键差异：** FEX 在每个 CodeBlock 反向计算 kill / SRA affinity，正向落实位置、冲突、fill/spill/copy，把 RA 数据输入直接改成 physical immediate，目的节点携带物理寄存器。之后 JIT `GetReg` 映射物理 Class/Reg，普通路径不再重建 SSA 位置。它仍有压力慢分支、局部合并、编码选择和必要检查。[固定 RA 源码](https://github.com/BulkanDrivers/FEX-android/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp)、[本地 JITClass.h](/Volumes/Data2/swiftvm-middle-build-cache/fex-android/source/FEXCore/Source/Interface/Core/JIT/JITClass.h)。

**推断：** 最有覆盖面的候选是跨“输入约束 → 实际分配/架构状态 → 最终交付”的整个决策服务。必须在实际位置确定时形成权威结果，让后续消费者直接使用；不能增加一张 event/certificate 表却继续保留旧解释链。该方向尚未闭合到可承诺 2× 的具体算法。

## 4. 前端已提前优化，但公共表示仍有持续成本

Known 能力已经接入真实生产路径：[compile_binding.cpp](../source/runtime/backend/compile_binding.cpp):73 设置 early_build，`bound_decode` 默认开启，builder 已有出生期 fold、deferred state/flags、EA、load-extend 和有限需求分析。再次提出“让 decode 知道后端”不会产生新收益。

当前剩余工作包括：

- 构造唯一可修改语义 IR，维护真实 uses、owner 和编辑失效；随后优化仍需解释保留的指令和输入。
- [pipeline.cpp](../source/runtime/compile/pipeline.cpp):41 的 state / flags / value / DCE 闭环仍执行。ValueWorklist 已有候选门，不能说所有块都无条件建立完整 worklist；未来 uses、跨块需求及改写后的机会仍不可能全部在出生时解决。
- 优化前 [target.cpp](../source/runtime/backend/arm64/compile/target.cpp):80 的 SupportsIR 与优化后 [optimized_unit.cpp](../source/runtime/compile/optimized_unit.cpp):17 的首次 Facts/SSA 是不同状态的检查。后者已经把多项收集合并在一个访问中，且有缓存；不能再将多次 Prepare 调用计成多次重建。
- [instr.h](../source/runtime/ir/instr.h):759 的 typed 读取仍需解释字段 kind、变长 payload 和 local Value；managed 槽为 16B prefix + 48B Inst。读取便宜程度不能只看结构体尺寸。FEX 使用线性节点/typed payload，RA 后数据输入直接物理化；两种表示和消费方式有实质差异。

项目 `ASSERT` 在 Release 仍检查条件，失败格式化已在冷路径；小写 C `assert` 会被 NDEBUG 去掉。同页 local resolver 的若干 bounds/alive 检查属于后者，不能声称所有指令访问都承担相同生产检查。改进应针对受控路径的完整构造/读取合同，保留 raw/manual/Unknown 和编辑后验证；没有独占计时支持“关掉 assert 就能根治”。

**方案缺口：** 当前 M1 的 DCE/flags 增量化只覆盖这条成本线的一部分。其优先级下降，不等于 213/262 ms 的构造与公共工作已经解决。应明确下一次替代究竟取消哪些首次解释、通用展开和保留节点；继续保同一 IR 底座时也可以改变提交与消费算法。单纯再压缩槽尺寸或更换 getter 不足以立为主路线。

## 5. 请求与资源成本是放大因素，但不是完整解释

FEX 的线程级 decoder、IR builder、pass objects、backend 和 arena 跨请求复用；IR 用完后交还实际 pool，正常路径不逐节点销毁。SVM 已有 Storage / workspace / Store 复用与 bulk teardown，不能说仍然全部每次 malloc。但每请求 Map / LiveEnd 域仍实际初始化，若干临时向量/状态仍构造和结束；部分初始化及 Facts 在 other 中。[locations.cpp](../source/runtime/compile/allocate/locations.cpp):29–67。

固定 FEX 工作量点中，255 个 CompileBlock roots 覆盖 7,123 个 decoder 保留块、7,191 个 IR CodeBlocks 和 29,915 次成功 guest lowering；RA 输入 code nodes 139,954，backend 输入 91,970。255 不是 guest 块数，也没有证据表明 FEX 少处理一个数量级 IR。不同阶段及不同实现的 IR 计数字段不能直接相除。[工作量证据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-compile-work-census/analysis/report.md)。

M0 的 SVM Block 有 2,415 个 block units，Function 有 437 个 function units 加 18 个 block units。更多小请求会放大固定成本，但 Function 的请求数已经接近 FEX 的量级，仍有巨大差距；请求次数不能独自解释结果。请求资源摊销与 RA scope 必须分别处理，不能为凑请求数把 Block 改成 Function，也不能把 FEX 局部 RA 当成 SVM Function 的等价替代。

## 6. 已证的小项与未证的放大点

- 已有 strict compile PMU 中，Complete 约 17%、物理形成约 12%、实际 emit 约 8%–9%；Packet::Append 不足 1%。这些是旧版本合格 samples 的 cycles 权重，不是当前毫秒，也不是可删份额。typed-current 的完整负反馈进一步否定了继续以 codec 为主要提速方向。[P0](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/stage-current-physical-ab-native-20261030/P0-result.md)。
- 普通点没有开启 fine PROF2；disabled PerfScope2 不在每次调用读取 TLS。剩余一次 Translate 入口 prior-save TLS 读取不能作为数量级解释。[已关闭观察者假设](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/observer-disabled-tls-review/report.md)。
- [verify_gpr.cpp](../source/runtime/backend/arm64/compile/verify_gpr.cpp):796 的 VerifyHostRead 会从块前缀寻找 publication/observer；重复查询可形成平方级工作。[select.h](../source/runtime/backend/arm64/compile/select.h):279 的有序 vector 在非单调新插入时移动后缀，BlockOutput 查找也线性。它们是条件性放大点，缺当前动态次数和规模分布，不能宣布它们造成了全部差距，更不能称“指数复杂度”。
- 原 Store 每页错误翻倍目录容量的缺陷已经修复，旧失败应保留，但不能继续把它算作当前 CoreMark 编译慢的原因。

## 7. 现有方案需要怎样改

1. **把 M2 的输入侧完整替代补成主算法。** 当前 5.3.1 的历史窗口查询有具体设计，输出 codec/native 也已实现，但它们没有覆盖整个 Describe / Complete。应沿一个真实值从约束到实际写入及最终读取列出旧循环退出，目标是减少决策次数、数据解释和结果重建。
2. **重审 M2 / M3 的边界。** 18/27 ms 的 assignment 诊断足以否定单独优化排序/扫描作为主线；它不足以否定一种同时替代目标准备、后置 declaration、绑定的分配流程。FEX 的优势恰有跨这些旧桶的部分。未来算法应按职责和依赖划分，不能受当前 timer/目录名限制。
3. **为构造/公共分析保留同等明确的整改主线。** 保留已完成的 Known 优化和 Facts 复用，具体指出下一次减少哪些 IR 出生、通用解释及首次服务。只列 DCE dirty 队列与 flags 增量化，无法解释全部前端成本线。
4. **保正确性合同，允许替换实现合同的方式。** 最终 writes、恢复、宽度、Block/Function 范围都必须成立；不要求永远保留每一套旧候选表、查询接口和证明遍历。生产核验应读取真实结果，独立 audit 保持独立，不能靠候选自证；同时不能因旧实现用全图重建就把全图重建写成不可改变的义务。
5. **对最终物理结果只保留一个权威归属。** 当前 CurrentInstruction 和同源 encoder 是已有基础。后续改上游实际决策与交付，不能又建一层“更直接的物理 IR”并保留原链。纯 target 能力留在公共接口，实际 home / transport / machine code 留在 backend；Unknown / EarlyOff 有真实消费者。
6. **更新状态和收益资格。** 3ef 重发/退役已完成最低语义验证，但性能尚未测；0641 的 codec 试验无大幅收益。二者都不能继承 da 的比值或作为尚未兑现的未来收益。最终两种 scope 各自 ≤2×、不降低最终生成质量的门不变。

## 8. 本轮结论的边界

现有证据足以定位主要成本范围，指出仍真实执行的结构性放大链，并否定多个小项主攻路线；**尚不足以把 16.68× / 18.62× 的每一毫秒全部归到某个服务，或证明一个具体组合一定达到两倍门。** 当前输入准备、Complete 内部关键服务和首次公共构造的独占成本尚不完整。

下一次设计审查应提交跨这些阶段的正向算法和旧工作退出清单，而非继续目录整理、codec 调参或复用已完成收益。必要归因应只回答会改变该算法选择的缺口，复用现有计时工具；施工按成组构建和最少相关语义检查推进，最终集中验证生成质量、运行表现、资源与完整 compile。

独立只读复核：[后端职责链](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/compile-cost-source-review-3ef113-20261031/review.md)、[固定 FEX 流程](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)。


## 9. 新版方案的 compile 性能上限复审

后续落实记录：本节要求已写入主方案5.2.6、5.3.5～5.4.1及5.8，并同步设计细则。当前[净工作账](spill-and-compile-root-cause-plan-2026-10-28.md#581-当前knownnative的净工作账)、[FPR准入替代项](spill-and-compile-root-cause-plan-2026-10-28.md#537-fpr准入作为跨阶段替代的具体闭合项)和retry范围仍分别记录真实资格；公共字段/原用途分组已在第74批退出，独立Facts/target共访在第75批完整反馈后撤回，不再作为待办。下文保留审查时描述，不把要求或历史实现统称当前已完成。



本节保留上一轮主方案复审，不重复第1～8节对旧方案的状态批评。复审时SVM为3ef11319；固定FEX为a156752f＋原Android补丁。该次复审只读源码/原结果，无构建、设备运行或性能采样；后续实施与撤回见第10节。

**审查结论：方向成立，但性能可达性尚未闭合。** 新方案正确地合并M2/M3、保留M1/M4并拒绝重复认领输出迁移；仍缺少普通路径实际要处理多少节点、边、状态、查询和重试的约束。只按目前的职责表实现，不能确保完整compile进入FEX两倍以内。本轮没有证据证明≤2×不可能，也没有证据保证下一组合能达到。

这里区分两个问题：一是固定输入/配方下能否达到最终≤2×门，必须由完整实现与同轮测量证明；二是算法是否因剩余全量服务、乘积式查询或整scope重试而预先限制可达到的性能，可以先通过源码与成本合同排除。对任意规模/任意CFG的绝对延迟或统一两倍上界，现有资料不足以证明，尤其Function保持FEX块内RA没有的跨块义务。最终6.3/6.4规定的实际工作负载与两种scope均不改变。

### 9.1 FEX真正便宜的常态路径，应对应到具体工作

| 固定FEX行为 | 新方案还须落实的成本合同 |
| --- | --- |
| 生成的typed op直接写实际payload，构造时使用host能力；保必要use-count | 不只把Describe放进Facts的外循环：明确哪些参数字段只解释一次，哪些中间节点/边不出生，哪些首次服务仍保留 |
| 块内反向kill/affinity、正向分配；短缺处实际spill/fill，首次spill才建立额外next-use状态 | 正常受约束分配直接交付真实结果；局部短缺不要惯常放大成整个scope重算或更高全局reserve |
| RA把数据operand改成物理寄存器，backend读取class/slot | SVM的最后binder应读取已定的实际位置，不能继续承担普通语义推断、copy选择和原逻辑生命周期的多次发现 |
| IR/payload arena与pass/backend对象复用，已PostRA不再维护原SSA用途 | 分别约束backing、初始化、嵌套释放和最后消费者；保唯一重发输入不等于永久保所有早期分析对象 |

FEX仍有真实DFE/CFG分析、N级初始化、首次spill额外反扫、编码与发布；不是只有两趟或零检查。[固定RA源码](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp)、[默认passes](https://github.com/FEX-Emu/FEX/blob/a156752fae3a4f65fefb1ed95af980bcdfac43d7/FEXCore/Source/Interface/IR/PassManager.cpp)。详细本地行号与版本/计时边界沿用[固定FEX复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)。不能复制其块内范围、每guest状态flush或更差spill来降低SVM最终质量。

### 9.2 P1：M1尚未给出幸存IR的净成本目标

即使完成M1共访，幸存节点仍经过出生/计数维护、真实反向用途、ValueWorklist首次roster/CSE/深事实、必要state/flags/hoist、最终schema/SSA/live/index。当前普通Block已建立并复用同一UseGraph，基础Facts也已共访和缓存；这些不存在可重新认领的普通“双图”收益。

源码锚点：[ValueWorklist固定roster与首次准备](/Volumes/Data2/SwiftVM/source/runtime/compile/passes/value_worklist.cpp:164)、[普通Block用途接入](/Volumes/Data2/SwiftVM/source/runtime/compile/passes/value_worklist.cpp:629)、[首次Facts](/Volumes/Data2/SwiftVM/source/runtime/compile/analysis/facts.cpp:392)、[融合访问仍承担的实际服务](/Volumes/Data2/SwiftVM/source/runtime/compile/analysis/facts.cpp:544)、[Function值传播](/Volumes/Data2/SwiftVM/source/runtime/compile/analysis/value_flow.cpp:96)。

应补两项完整设计：

1. **每条形态变化说明整链少做什么。** 除“直接提交最终IR”，还要列出生节点/occurrence、uses维护、第一次分析/优化和后端需求的净减少。已有birth、EA、load-extend、deferred、IntegerFacts先扣除；若只是早查一次相同规则、后面服务仍完整存在，就不构成主要替代。
2. **不要把现有参数表示冻结成不可改的终点。** 当前公共Inst仍使用tag、变长payload、offset及local引用投影；编辑/遍历/校验各有真实解释。可评审的较大替代是由同一schema生成常见公共语义form及其构造、编辑、遍历读口，直接访问实际字段；Params/Lambda/foreign等保真实受检escape。它必须贯通builder→uses/rewrite→Facts→target及其他消费者，并删除原同义packet解释，不能再存N份decoded缓存、引入公共ARM64 form或仅改getter/槽大小。

第二项是未闭合的结构候选，**不是本轮批准新开packed迁移，也不是已证收益**。若维持现布局就能达到同等工作量和访问成本，无需为了名字更换格式。首先需要明确普通路径的字段读写合同、受控写口和最后检查；不能只关ASSERT，或凭Known/owned/seal跳过未满足的schema、SSA和物理合同。

表示依据：[参数offset与typed读](/Volumes/Data2/SwiftVM/source/runtime/ir/instr.h:730)、[实际Value/Params/Imm解释](/Volumes/Data2/SwiftVM/source/runtime/ir/instr.h:759)、[同页/跨页local引用](/Volumes/Data2/SwiftVM/source/runtime/ir/instr.h:1332)。其中若干小写assert在NDEBUG下退出，不能统称所有边界检查都付生产成本。

### 9.3 P1：一次最终绑定，尚未等于一次最终输入处理

目前normal native仍有以下真实链：

```text
family writer按实际role绑定Value/Params
  → metadata/origin和实际location/copy查询
  → BuildCurrentImpl再次读取原逻辑occurrences
  → MEM判重、multiplicity、last-use、preservation/reload/forwarding
  → CurrentInstruction + 当前资源 → Tick/编码
```

[BuildCurrentImpl](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/physical.cpp:1724)在tick分支再次ForEachValue，1756起用线性find_if对MEM输入判重。若单条Params有k个不同MEM输入，可产生O(k²)去重。这是源码结构上界，**没有当前动态分布或独占耗时，不是已证CoreMark主热点**。这些是原逻辑occurrences；被EA/publication折叠的输入仍可能影响pending-spill生命周期，不能直接用最后机器操作数代替。

此外，[MatchGPRCopyUses](/Volumes/Data2/SwiftVM/source/runtime/compile/allocate/locations.cpp:535)选出候选后又遍历consumer全部Value；[BindIncomingGPR](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/physical.cpp:1441)可达此链。必须限定结论：当前Params writer明确用WholeValue，不触发这里的copy推断；主要已核native入口为小arity的Add/Select等。不能把该查询的通用k²上界说成已发生的大Params热点。[Params实际角色](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/select_control.cpp:120)。

应补的算法合同：

- 当前consumer的语义字段/occurrences只解析一次，供实际role绑定和原逻辑资源服务借用；保持两种用途不同的含义，不复制全E图。
- 可在原当前frame中按definition聚合width与multiplicity；小固定arity用有界直接处理，大Params用一次排序/归并等有明确上界的办法，退出逐项线性判重。必须保持原occurrence顺序选择、selected整对遮蔽、preserved过滤和真实last-use语义。
- 位置/state服务应提前给出依赖实际位置的成功选择；binder消费结果，普通路径不再为取得寄存器重新走语义copy推断。一个必要位置读取仍存在，不能宣称Bind全部消失。
- 当前normal SemanticBorrow与消费/retained兼容读口可按真实粗权限分域。活IR可以直接交必要id/type/view，仅MEM等真实消费者需要完整token/原始uses；retained/mixed保原合同。现OriginOf直接读槽前缀，**没有每operand页地址二分或全局registry查找**，不能据错误机制论证收益。
- 当前frame、typed kernel和3ef重发继续使用，不再造持久物理影子。重发仍从唯一原输入重新绑定/编码；其费用如实计入。

最后一项覆盖整个semantic→binder→resource交接，不单独开origin getter微优化包。具体读口见[Metadata/RawConsumer](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/physical.cpp:1394)、[OriginOf](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/physical.cpp:1280)、[IRStore::ReadOrigin](/Volumes/Data2/SwiftVM/source/runtime/ir/store.cpp:300)。

### 9.4 P1：按事件访问一次，仍可能重复解释整条值链

[PublishValue](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/state.cpp:451)沿extend链发表alias；[ValueExtensionFacts](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/state.cpp:482)组合活copy并递归解释包装；[publication复用判定](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/state_publication.cpp:1467)还会再沿版本链查询。因此“S个sites有序处理一次”不证明总工作为O(S)，仍可能包含查询次数×链长和真实copy输出量。

应把不依赖实际位置的carrier/extension结构transfer交原稳定输入生产者计算一次；原state owner只与当时实际home/copy/observer事实组合。需要多个alias的真实输出仍保留，不能canonical化抹掉partial-width、raw-slot或fault身份。动态事实不跨位置编辑/观察边界缓存。

必须分别说明稳定链解释次数、实际alias/copy数量、每个事件的更新和查询范围。已有Function width已经预编block transfer，固定点只传播B/home状态；不能再认领“每轮不重读IR”的收益。[现有Function width算法](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/state_widths.cpp:223)。

这仍是M2/M3同一跨阶段服务，不建立额外value-summary图、通用事件库或证书层。新实现若只把旧递归藏进有序visit，性能工作未完成。

### 9.5 P1：重试须有范围上界，inputs复用还不够

当前[reserve ladder](/Volumes/Data2/SwiftVM/source/runtime/compile/allocate/allocation.cpp:223)每rung重置结果并重跑整个分配scope；[FPR合同重试](/Volumes/Data2/SwiftVM/source/runtime/compile/allocate/allocation.cpp:327)亦可重跑，随后重新Complete。FEX在压力点处理spill/fill并继续，首次spill才补next-use；它没有SVM这一套整scope reserve重试。

SVM已有局部GPR修复，不能再声称所有短缺都全量重试；但该修复支持域有限，并非通用的Function局部修复。[现有修复边界](/Volumes/Data2/SwiftVM/source/runtime/compile/allocate/spill_regions.cpp:330)。当前没有新的retry频率/权重，故不以此解释全部慢时。

应补：

- 正常成功路径列清一次分配实际承担的工作；静态输入复用与每次结果重置/重建分别计账。
- 区分scratch不足、固定位置/FPR冲突、可选coalescing失败、编码region失败；不能都落成整unit重算。
- 尽可能在原Writer内只改真实受影响的location segment及其writes/transport/依赖选择。Function沿自身跨块live范围修复，不能退化成块内算法。
- 必要全scope重试保合法终止路径，列触发原因、次数上界、复用项、重建域和产生的质量变化。不能仅减少重试次数就接受更多spill/MOV或提高全局reserve。
- region失败的true→false只重发，不重跑RA/Complete；此点3ef已经实现，不重复列收益。

新旧路径的粗准入与同一路线中的合法retry是两件事。前者不能先付完整新链再付完整旧链；后者也不能因为“是必要重试”就不约束重复工作。

### 9.6 P2：容量复用还没有定义真实常态触达成本

[Allocation::PrepareRequestForReuse](/Volumes/Data2/SwiftVM/source/runtime/compile/allocate/locations.cpp:60)仍初始化实际Map/LiveEnd域，结束还处理嵌套copy/preservation等资源；[Storage idle预算](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/compiler.cpp:441)为128KiB。FEX的双arena复用是虚拟backing/偏移和实际pool协议；不能将双8MiB视为16MiB常驻RSS，也不能将其与SVM单个Storage预算直接相除。

应按同一真实unit统计并设计：managed槽与实际参数、唯一UseGraph、ValueRow、index/use-end、Allocation、state/Selection、当前frame，以及它们的初始化/访问/清理字节。区分live数量、capacity、真正触及字节、grow重叠和跨请求累计；64N的槽容量不等于每次扫描读取64N。

无相关需求时，不建立对应困难状态；普通dense结果一次初始化后随真实写口更新，稀疏复杂结果按真实事件产生。复用空backing和最终retained上界沿原owner，不延长活identity。当前3ef保唯一IR至Flush是已成立的重发合同；更早结束其他数据须先迁完真实读者，不为cleanup另开一轮生命周期工程。

本项是整条主线的内存工作预算，**不是提高缓存阈值或bulk-free便能解决数量级差距的结论**。M0显式ir_free很小，其他结束成本分散，当前3ef也没有计时；不把所有N清理当主要可删项。

### 9.7 必须补到方案里的工作量合同

符号只用于审查实际算法，不要求新增profiling框架：N_b/E_b为本次真实出生节点/occurrences，N/E为最终语义节点/occurrences，B/G为块/CFG边，P为有实际消费者的最终copy/preservation/transport输出，H为实际编码工作，Φ为必要固定点传播，R为整scope重做次数，k为单指令可变参数规模。

| 域 | 应写明的正常路径合同 | 不可隐含的额外成本 |
| --- | --- | --- |
| 构造 | 每个最终公共形态一次提交，明确少出生的N_b/E_b；真实写口维护不变量 | 先构造再删除、同一语义多次解释、generic miss后的重复工作 |
| residual与公共事实 | 首次必要服务逐项列出；变化沿原owner反馈；稳定typed字段共用 | “同一个循环”里继续全部旧服务、无变化仍重算、第二图 |
| Block RA | 局部用途/affinity与正向实际位置交付；普通短缺就地处理 | 原collect/sort/晚declaration仍全量并行存在，整scope ladder |
| Function RA | 保原CFG/SSA/live，Φ与普通N/E工作分别归属；重用真正稳定transfer | 用块内算法冒充Function，或同一CFG事实多处重建 |
| state与选择 | 稳定源链只解释一次，实际事件与P输出有明确范围 | sites数量掩盖查询×链长、candidate表数量冒充必要P |
| 当前绑定/资源 | 一次当前语义解析交付实际role和原逻辑用途；常见寄存器操作直接消费实际位置 | copy推断重扫、MEM Params逐个线性判重、与whole/point角色混用 |
| 编码/发布/结束 | 与H、实际metadata、实际资源及最后消费者对应 | 重发/retained/失配清理被漏记、初始化/free移出服务 |

理想普通Block工作应能解释为少数固定遍数的N/E，加真实构造、P、H；Function另外承担真实Φ。可变Params允许有说明的O(k log k)整理，不把它藏成O(k²)。这不是声称所有输入严格线性：相同big-O也可能差几十倍，必须同时列每遍内容和触达量；R、Φ、P不能用含糊名称吸收任意重复工作。

这是代码与算法的审查要求，不是每次编辑新增测试门。既有最低语义、完整算法一次方向反馈、最终集中质量/资源/FEX节奏保持不变。

### 9.8 两倍目标对覆盖范围的硬约束

以最近具有fresh FEX分母的da开发账做尺度分析，不转标签为当前3ef：

| 假设保留的旧成本 | Block | Function | 对路线的含义 |
| --- | ---: | ---: | --- |
| 2F目标 | 94.955ms | 94.552ms | 历史单对的目标尺度，不是下一轮fresh分母 |
| decode/build完全不变 | 111.158ms | 165.636ms | 仅这一项已经超出目标，M1不能只改后续Facts/DCE |
| regalloc＋codegen全部理想归零后的剩余 | 324.733ms | 384.047ms | 仍为各自FEX的6.84×/8.12×，后端单线不够 |

令p为**确实被替代的旧耗时占比**、s为该部分的新旧加速比，其他工作不变，则新总耗时比为 `1-p+p/s`。按da尺度，即使s趋于无穷，p也至少要达到88.0%/89.3%；若只覆盖95%的旧耗时，该部分还要约13.59×/16.55×才能达到总目标。这是条件数学关系，不是覆盖率/收益预测，不能用“改了95%文件/节点”替代p。

因此，M1/M2/M3/M4需要一起形成大量真实工作退出；仅把所有旧服务各提速一点没有已证达标路线。反过来，这些旧阶段不是不可降低的物理下限：直接形态、删除分析需求和改变交付算法都可以改变它们。当前缺少完整实现，不能把旧保留成本宣判为最终必然成本。

### 9.9 本轮收口与后续顺序

本轮不再增加codec、窗口、getter或清理微包，也不扩设备矩阵。建议下一方案修订只补三项可审查产出：

1. **正常Known/native整条服务的工作表**：每个实际输入/字段/位置/状态在哪里产生、读取几次、哪些首次服务留下、哪些循环退出；public/retained/Unknown另有真实入口。
2. **两个RA scope及state的困难路径表**：每种失效/retry的范围、次数、依赖和质量合同；当前consumer逻辑用途与实际operand分别交付。
3. **同一完整实现组合的覆盖与剩余成本账**：贯通生产者至最后消费者后，再做一次whole-compile方向反馈；没有大幅方向就根据实际保留服务改算法，不继续局部调参。最终仍沿原6.3/6.4。

当前可确认的是这几个具体设计缺口及其修补方向；不能确认某个未实现组合已具备两倍性能。主方案本已如实保留这一资格边界，本次复审将其具体化为普通路径、复杂度、重试和触达成本要求，而不是新增一套管理层。

## 10. 最新结论与方案落地

本节保存截至第78批撤回及before归因时的结论，当前第79批见第12节。第75～78批四类完整候选均完成必要语义和唯一整体方向，没有大幅收益后撤回。第78不是只测一个索引：它贯通25文件的实际生产/全部相关normal消费者与state交付，仍未改变总尺度。当时495887ca生产精确恢复a10，唯一保旧Function fixture修正。没有fresh FEX或最终质量/资源资格。

1. **共访不再立项。** 4f04必要语义通过，完整方向B742.573→748.575ms、F812.220→838.349ms无大幅收益后撤回。Facts换桶而主要后置服务仍在，不重开回调/索引/候选容量变体。
2. **真实少出生也需足够覆盖。** 78f在Known＋early＋canonical分支用一个公共ScalarShift及直接条件flags替代count/CF/OF/guard展开，保Src/Dst/fault及generic。修正真实构建、默认Operand Plus/schema和重复结束Function的夹具问题后，3个既有case执行4次共1095断言、physical164725checks/1注入、CoreMark B/F CRC4983通过。完整方向B727.675579→727.419967ms、F824.532966→805.382119ms没有大幅收益；工作量不同、各side/mode一点，无统计或质量结论。新增opcode/profile/wire及消费者整包29文件撤回，当时精确恢复f7e6；不将单族少节点或横跨文件数当旧耗时覆盖率。
3. **库调用不能统称临时容器管理。** 原123d整个qualified compile中，allocator self B4.697%/F6.105%；含明确allocator atomic/lock的互斥并集6.923%/9.050%，已包含self；copy/set2.462%/2.632%。associated加copy/set及库初始化/销毁/生命周期的宽包络12.217%/13.218%，既非可删份额，也非全部heap成本上界。完整库leaf30.942%/34.694%还含查询/排序/atomic/cache flush，不能当malloc/free。
4. **M4随真实服务删除存储。** 没有集中到单个可退役per-block owner的证据；换allocator、PMR或空backing复用不能消除仍必要的初始化/fill/查询/核验。不另开通用容器工程，不为同一假设追加采样。
5. **M2/M3仍有实质缺口。** 普通scalar BirthOnly的publication实际决定、稀疏FVI sites和当前raw分组已存在，不重复认领。剩余目标需求、实际改位、width/copy/state、晚选择及最终核验/交付须连生产者与消费者改变。单个核验子服务也不代表全部input/RA/Complete退出，不能先加一张实际写入表就宣布决定链已短。
6. **完整GPR最终核验替代也没有大幅方向。** 22b确实退出了正常核验的全N points/owners投影、sorted writer-key和逐Q前缀/窗口查询；9cases962断言、physical164721checks/1注入、CoreMark B/F CRC4983通过。但完整compile为B714.961→730.337ms、F815.108→818.285ms；每side/mode一点，F工作量相同、B最终IR/host略异，不能统计归因或由此判定全部后端成本。新增Q求解、ordered fixed与诊断API已撤回，停止窗口/游标/interval变体；FPR准入也只能随完整M2/M3迁移，不能顺势另开核验微包。

上述采样分母为B820/868765709 period、F921/980295089 period的整个qualified compile；Other服务/文件归属和库leaf是交叉投影，不相加，不换算当前毫秒或FEX比值。Other62.985%/62.321%仅指qualified余集，内部已细分93.07%/95.48%另用Other分母，未知仍排除。[库leaf原报告](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/p0-library-leaf-review-20261031/report.md)、[Other原报告](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/qualified-other-work-review-123d-20261031/report.md)。

实际结论已同步[主文5.2.1.1](spill-and-compile-root-cause-plan-2026-10-28.md#5211-当前完整候选shlshrsar直接公共语义)、[5.3.8核验替代结论](spill-and-compile-root-cause-plan-2026-10-28.md#538-已否决以完整gpr最终核验替代承担主要提速)、[5.3.9实际位置服务](spill-and-compile-root-cause-plan-2026-10-28.md#539-实施中实际位置生产与消费者复用)及[7.2后续执行](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)。单族或单项库占比都不能证明覆盖88%/89%的旧耗时；尚无已证两倍路线。最终质量与独立Block/Function各≤2×同轮fresh FEX不变，主方案6.3/6.4保持冻结原文。

第77批独立保留的actual direct-narrow结果目标由成功selector写原PhysicalGPRWrite，原fixed核验用真实home替代推导的旧result-home，保其他clobber和writeback。a10四目标构建、6cases303断言、physical164721checks/1注入和CoreMark B/F CRC4983已独立通过；它没有替代inputs/初始RA或整个state/Complete，不承担性能收益。[候选方向原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/direction/accounting.json)、[保留合同独立资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/gpr-ordered-admission-20261031/actual-result-retained/qualification.json)。

第78批当时替代的是实际决定的生产与读取：Writer随真实Map/REF/Width/Low32修改维护分开的邻接；recolor、copy、publication caller与局部修复直接消费，exact selected/cold use迁为按raw32分组、point32有序的唯一payload。Map、live ends、preserved区间、架构副本与Selection各守原权威；这些候选实现随后整包撤回，不属于当前生产或新目标树。

最终861/mainb994四目标、14cases979断言和B/F CRC4983通过，覆盖审查确认实际位置包的normal晚发现及Function局部范围接完。唯一整包四点B731.728826→731.534357ms，F804.853740→833.553024ms；各侧/mode一点、工作量略异，无大幅方向，不认定统计回退或isolated cost。[整包四点原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/owner-range-index/direction/accounting.json)。

25文件候选已撤：恢复22原文件、删placement_index两文件，只保已证过时的Function BirthOnly fixture修正。source495887ca、三生产ELF与a10逐字相同，main120e；四目标恢复构建和该例28断言通过，原a10 CRC/physical按同ELF原身份引用。旧失败/错误折叠猜测与最终候选原始结果保留，不能改写为全程通过。[撤回与精确恢复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/withdrawal.json)、[恢复资格](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/withdrawal/qualification.json)。

这次结果要求改变下一步：不再批准owner/source/word/hash/晚发现变体，也不能把旧代码中的静态可优化循环自动当主因。普通32/64完整flags writer、默认narrow align和Complete借用consumer rows已存在。本轮已经用恢复后的current main120e完成一B一F native PMU，严格分母759/803579066period及857/915799836period，raw/API与当前符号逐行核对，未知/unqualified不混入，采集场关闭。target inputs16.12%/13.06%、Complete14.93%/20.23%、native全域17.13%/16.35%、真正RA6.35%/6.78%。这些服务含必要工作，非可删比例或普通毫秒；首次Facts/uses/roster或单一publication/proof不足承担根治。下一范围是inputs→实际决定→Complete→native整个语义交付，M1/M4仍不可缺。PROF2 appendclock与density/dump/verify不能替代同域证据，同问题不再重采，详见[主文5.8.2](spill-and-compile-root-cause-plan-2026-10-28.md#582-当前a10生产的主要服务新native-pmu归因)。[当前出生链复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/m1-scalar-semantic-birth-review.md)、[下一归因边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/actual-placement-service-20261031/next-major-service-review/major-service-attribution-review.md)。

没有已证≤2×的算法组合。最终质量/资源及Block/Function各≤2×同轮fresh固定FEX不变，主方案6.3/6.4冻结原文保持。

## 11. 最新整链设计决定：同Store程序替代，不另养一份Selection程序

作出本节设计决定时source仍为495887ca / main120e，当轮没有新构建、设备运行或计时。第1～9节保持当时审查内容；第10节保留实验撤回与before PMU事实。后续第79批见第12节，不用新方案或新组件覆盖旧结果。

1. **选择唯一程序载体。** 原Store中Inst结束后，同槽接管为backend virtual程序；最后同一operand变为physical。原Selection仅保真实稀疏候选/unit/B信息。若普通generic仍读Inst，则旧链没退出；若dense Selection存全部N/E同时保Inst，就是第二程序，两者均不作为新终态。
2. **选择决定RA实际工作。** 同源form/operand/effects直接给机器reads/defs、fixed/tie/clobber/scratch及actual transfer；原两个范围的allocator在此live域分配。writeback/direct narrow/forward改变早写或use-end，必须在受影响最早birth前事务准入，不能沿旧semantic ends分配后补Map/形态。
3. **保留最后读者与资源合同。** 完整semantic SSA Finish及图匹配先完成；原CFG/terminal/entry/hoist转交后关闭语义对象。logical MEM use/death不能由selected numeric输入替代，必要义务成为同程序真实transport事件。Function entry-width/form循环、FPR失败/受界retry、raw/width/fixed/extra writes独立check均保留。
4. **重发读取稳定结果。** 193 kernel直接借PhysicalReady，不回Inst/producer/Map识别来源；true/false只重建编码ctx/offset及动态状态，不RA/选择/重绑定。不得先完整旧Describe+Complete，再加输出转换，或经旧packed codec往返当作新算法。
5. **早建与性能边界。** Known借纯能力先规范化公共可编辑语义；前端不产NativeInst。M1构造/首次服务与M4真实触达必须共同大幅退出，约48～50%的当前观察并集包含必需工作，不能当收益。主线已确定，尚无已证≤2×组合，也没有为新方案填达标毫秒。

实施细节统一见[主方案5.3.11](spill-and-compile-root-cause-plan-2026-10-28.md#5311-下一完整替代同一store中的后端虚拟与物理程序)、[IR阶段/资源](ir-representation-redesign-2026-10-28.md#4-生命周期与最后消费者)、[逐文件结构](middle-pipeline-file-layout-2026-10-28.md#2-最终目录总表)。新增`native.*`是真实同槽对象，`allocate/program.*`是中立借用协议，不再引入管理层。相依组只做必要构建/正确性；整链一次方向，稳定后集中最终质量/资源与两范围fresh FEX门，6.3/6.4不改。

## 12. 同Store首组实施复核：组件已验证，默认链仍待替换

**最新进展是载体与原core能实际接线，完整compile差距尚未重新测量。** cf79edbc/main4bed419c在原128B槽内放入112B NativeInst，原CFG/terminal移交、中立program、实际machine live与原LinearScan有限core已通过组件整合。四Android目标及红魔6native cases293断言、6既有cases338断言、physical164721检查/1项注入通过；首轮失败和修正后通过各保原身份。[检查点与原始回执](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c1-checkpoint.json)。

| 复核发现 | 对方案的具体修正 |
| --- | --- |
| normal仍走Describe→RA→Complete→semantic binding | 不能记旧生产工作已退出，也不为carrier做单独性能矩阵。下一以同族生产者、质量分配/state/check和kernel最后读者成组替代 |
| default arg/result投影不等于实际machine schema | pair secondary在producer点联合定义，no-body/逻辑事件分开；GetResult/GetFlags按自身编码及numeric/implicit读取分别处理。定义枚举/查询、birth块与output resolver一致，再准备live/pressure |
| core只有有限准入 | 只拒绝已提交且仍依赖语义的政策输入及缺失/区间非法的scratch表；尚未提交的width/flags/observer需求和真实scratch预算须由目标粗准入负责，不用漏提交/默认零预算使core成功 |
| 同槽不等于资源/查询工作免费 | Params仍有逐指令转换与暂时重叠；Store membership/原B定位/REF追根、N规模临时live/身份状态及Prepare/Freeze双resolve仍计费，不再新建全N/E缓存掩盖 |
| 组件NativeReady不是完整target验收 | state/fixed/width/flags/transport独立check、193kernel新读口及同Ready重发未迁完；不能用Ready位或原physical测试自证全族质量 |

相对FEX要落实的是在RA前明确实际形态与读写合同、让同一决定直达最终编码，不是再增加一种输出格式。SVM仍保自己的Block/Function范围、完整SSA与独立正确性合同；完整旧Describe/Complete/binder退出前不能认领其成本。对应准入、联合输出和退役条件已写入[主方案5.3.12](spill-and-compile-root-cause-plan-2026-10-28.md#5312-下一相依组先闭合机器合同再替换默认链)及[逐文件职责](middle-pipeline-file-layout-2026-10-28.md#5-arm64运行时装配与交接)。

cf79没有CoreMark、compile方向、PMU、QoR或fresh FEX数据；第10节before份额及da的16.68×/18.62×不能改标为当前差距。M1构造/首次服务与M4真实触达仍须共同减少，约48～50%观察并集含必要工作，不是预计收益；没有已证明满足≤2×的完整组合。成组最少语义、完整算法一次整体方向、稳定后集中6.3/6.4的节奏不变。

## 13. 第80批：首批机器合同与原压力读者已接线

当前94cb29e9/maind9a92ac9相对cf79改11文件：原pair准入共享，NativeDiv静态Native/Helper选择和GetResult/GetFlags的实际读写在live前确定；同程序持secondary唯一def、producer birth/B/site与输出点。原GPR pressure/victim/next-use改读actual site/inputs/traits，保原Writer提交、预算和用户开关，不新造allocator。四Android目标及23cases2104断言通过；编译失败与执行清单数量准入错误均保留，没有重复通过项。[本组检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c2-machine-schema-20261031/checkpoint.json)。

这仍是normal之前的相依组：完整scratch/ABI、fixed/GPR/FPR/width、原attempt/repair、state/observer、logical-only DISCARD/非MEM退休、transport及193kernel新读口尚未贯通。FPR新candidate接口没有由本轮native GPR压力例取得运行资格。新静态准备目前是fixture入口，旧normal的Describe/scratch matcher/Complete/binder继续执行；不能把本组23例或31个累计文件代表整线退出。

下一须替代完整初始约束及质量/state/最后交付服务，M1/M4共同减少真实工作；不继续用载体/字段/新用例补数量。94cb没有CoreMark、compile方向或fresh FEX，旧PMU份额和da历史倍数不改标为新数据。最终质量和两scope各≤2×fresh固定FEX仍未完成，6.3/6.4不变。

## 14. 最新初始需求与质量服务复核：C3仍为未构建接缝

本节保留前轮23dc快照；当前整合状态以[第15节](#c3-integration-review)为准。

本次按用户要求只修改方案文档。C3工作树冻结为23dc79d9，相对C2改20文件，未构建、无对应新ELF；scratch recipe三字段尚未补齐，完整initial/FPR等入口仅声明。第13节的23cases2104断言只属于C2／94cb，不转给C3。新normal继续关闭，不能因fixed字段或FPR消费者已改就写成完整质量服务替代。

| 最新结论 | 主方案与细则的修正 |
| --- | --- |
| 逻辑ID不等于机器出生点 | fixed/位置/pressure/output统一传actual birth及所属块；联合def不保证ID单调，初始fixed读不能沿旧递增游标。保各消费者原端点，同点非REF输出需独立最终核验 |
| 描述、quality与RA不能各做机器live | 一份ProgramLiveness供target initial/FPR/原attempt借用，区分全局CFG端点、定义块局部端点、完整occurrence与真实跨块；Matches不是编辑版本证明，改form/use/def/birth须显式维护/重建 |
| scratch需求不能靠零值推定 | 最后semantic读者稀疏交付whole-unit RA与Function emission-local两域；完整rich scratch/两pool/实际HasUse来自同源规则，缺域显式拒绝。初始Complete不等于CoreAllocated或最终Ready |
| 三种寄存器影响不能混在一起 | guest-home partial访问、活值池净clobber、helper raw ABI/capture分别生产/消费；preserve-all helper的raw破坏不能OR进净RA mask。当前effects masks尚未完整生产，0不证明无访问 |
| FPR不能只迁tie或关质量 | 迁完resident/destructive/scalar/insert/AES/merge/publication、完整窗口/observer与跨块live-in检查，显式带原配置；接初始安装、结束coalescing和原forced/reset/retry/controller，才能退出旧政策投影 |
| 共用接口不消除内部复杂度 | native publication接缝仍逐publication枚举实际definitions，另有future-read二分/active查找、N状态/CFG、双resolve与prefix/REF工作；全部计净账，不恢复已撤N目录或把组件包装成性能候选 |

这些修改采用固定FEX的实质：**选中形态直接供RA、同一分配决定直达稳定physical args及编码**。它们不证明SVM能达到FEX两倍，不复制其块内scope，也不删除SVM真实质量/CFG/异常恢复合同。完整支持域必须同时退出旧Describe/Finish、语义政策投影、Complete同义发现/重建和每emit semantic binder；M1构造/首次服务与M4真实触达同步收敛。约48～50%的before观察并集含必要工作，不能换算预计收益。

具体相序、失败边界与成本集中见[主方案5.3.12](spill-and-compile-root-cause-plan-2026-10-28.md#5312-下一相依组先闭合机器合同再替换默认链)，文件落位见[结构细则5.1.3](middle-pipeline-file-layout-2026-10-28.md#513-最新初始合同的文件接线)。依据：[C3实际断口与三域复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/backend-initial-constraints-document-review.md)、[fixed-birth端点与独立核验债务](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/fixed-residence-birth-review.md)、[FPR消费接缝实际范围](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/C3/fpr-component/partial-receipt.json)。

本轮没有新代码修改、构建、设备运行、PMU或FEX点；第1～9节历史与主方案6.3/6.4保持原文。后续仍成组最少正确性、完整算法一次整体方向，最终集中质量/资源和Block/Function分别≤2×同轮fresh固定FEX；目标仍未完成。

<a id="c3-integration-review"></a>

## 15. C3整合复核：首轮构建失败与下一步

本节替代第14节的“当前”判断，保留其23dc时态。当前源码f636296b相对23dc改40文件、相对C2改46文件，2399个现存source/build输入。initial/flags/actual fixed、完整GPR/FPR/width读者、原attempt/controller、Original/raw/region已接源码；首轮四目标构建在program.cpp两处u32窄化初始化失败，exit1且source前后不变，无本版新ELF或设备结果。[冻结范围](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/frozen-build-01.json)、[构建终态](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/build-01/build.json)。

只读review确认五项必修：编译错误；坏callback/点/块/输出边界与scratch shortage分流；FPR block publication span须在改Map前核；declared-pool预算不能无意改变checked策略；alias raw cast为原VOID而canonical类型为U64。普通/terminal reload计价、严格region余量与pressure完整ownership正例应保留。详细来源与未修状态见[本次review记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/docs-integration-review-20261031/review-findings.md)，修改责任集中在[主方案5.3.12.5](spill-and-compile-root-cause-plan-2026-10-28.md#53125-当前必修项修边界不另造证明层)，不增加校验管理层或逐attempt证书。

本组解决的是旧质量服务如何读actual程序，不是完成normal或证明compile变快。下一仍有三处主要工作：成功quality/coalescing决定真正写入machine form并维护live；actual state及独立最终FPR/GPR/fixed/width/同点输出检查；原raw与actual lifetime共同结束MEM资源，再交193kernel与同Ready重发。窄/跨类BitCast、有既有映射或声明的入口和未迁目标形态保合法旧路径，不能通过漏支持、关质量或放宽合同宣告完成。

FEX的对照单位依旧是整条正常编译服务：选中形态直接供RA，成功位置和operand直达编码。SVM需使旧Describe/Finish、语义质量投影、Complete同义发现与每emit binder真正退出，同时交付M1构造/首次解释和M4真实触达/结束的减工。唯一稀疏access/observer已接；P_pub×D、future-fixed-read循环、N状态/CFG、REF/prefix、双resolve和原有限retry仍实际计费，接口共用不证明近线性，更不证明≤2×。

本轮仅更新八份文档并核既有构建终态，没有修源、新构建或设备/计时。随后先修这组必修并统一最少检查，继续同一整链；完整旧服务退出后一次整体方向，稳定后集中原6.3/6.4。没有新compile/FEX差距，C2通过、da历史倍数及495/a10 PMU保原身份；最终不降质量、两范围各≤2×fresh固定FEX的目标未完成。

<a id="c3-checked-integration"></a>

## 16. 第81批：初始质量服务已检查，normal和性能目标仍未完成

当前source6f7b18b2/main1b1e0466完成四Android目标及本版8cases47049断言：完整native initial/GPR/FPR/retry一例70、既有质量/width/region/repair六例223、原Function width固定点一例46756。AEA前版native tag12例11过1失败，不能合并成当前12例全过；只补跑修正受影响的一例及未执行两命令。两个自有namespace均核hash关闭，无活动进程或清理错误。[完整检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/c3-checkpoint.json)。

本组已修u32窄化、非法callback/shortage分类、FPR全部消费span、checked pool政策及raw alias VOID视图；原计价和pressure完整ownership正例保留。首轮语义失败还暴露fixture根本未进入GPR coalescing政策域，并错误要求初始U64读的reuse marker。仅修该case真实pool并明确验证政策开启，以原kind/home/fixed-class合同核U64读，保全部GPR/FPR写合并及retry正例；没有生产硬Mark或关质量。[最终fixture依据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/quality-followup-03/tests/pool-domain/receipt.json)。

下一整组的具体读口债务已经落到原文件：成功selector/quality写同一native form/operand/effects，并在实际birth前维护live；state/独立复证随后闭合，最终resolver不在Freeze中回查已physical prefix的virtual Definition。现193kernel可复用，但CurrentInstruction须借同一NativeInst，普通args/Params不再填第二frame；pair arg4/primary、required roles、no-body Tick、原raw与actual count0源的MEM安全、canonical materialization key与raw退休identity必须一致交付。资源沿原PhysicalStream一次生产，true/false共读Ready，最后Flush/发布再结束原owner。[actual form/state写口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/corrections-02/next-actual-forms-review.md)、[最后resolver/资源/kernel接口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/corrections-02/next-native-kernel-resource-review.md)。这些是同一服务的相依工作，不重开getter/codec微性能候选。

normal仍沿旧Describe→RA→Complete→semantic binder，尚无C3 compile/FEX/QoR/PMU结果。M1少构造/少首次解释与M4触达/结束必须一起减少，不能从本批构建/断言数量或约48～50%历史观察并集外推收益。完整服务退出后一次整体方向，稳定后集中原6.3/6.4，最终目标不变且未达成。


## 17. C4接缝与M1最新复核：未构建，不继承C3资格

本次按最新结论修订方案，不继续实现或启动测量。文档轮冻结C4 source`1fa59a936a43959f988be7b932a36c7b5aab9595c37d5bfbd75bca0045c14098`，2399个source/build输入、相对C3改12个源文件；三组件在文档修改前收口。C3/6f7b的四目标和8cases47049断言仍是最近已检查基线，C4没有新ELF/语义/compile资格。

本次需要改变方案的结论是：

1. **借用视图、稳定header与定义转移不能代替完整realization。** Current已有两指针/Params按需读口，native已有稀疏role、resource和定义owner原语；但成功质量决定还须同时改变actual reads/defs/effects、body、live/fixed/pressure和state。原copy/location需要真实consumer输入与共享纯policy；raw用途和actual inputs换源后不能并为一个frame/count。生产resolver/资源/driver仍未接，normal没有退出旧Describe/Complete/binder。
2. **独立FPR proof必须保路径和错误语义。** typed reader不依赖FPR候选recipe；publication匹配局限当前consumer/proof实际块，不能以其它分支的较早RPO Set替代路径证明。foreign producer/live-in及全scope occupancy仍保留，同site多个actual输出分别检查。Indexed mask/direct、无body publication、非pair FPR定义转交等WIP缺域是UnsupportedForm，不是可关coalescing的ContractRejected；只有支持域内真正质量拒绝才沿原retry。proof后改actual程序必须失效重证。
3. **最后资源同时满足raw退休和actual寿命。** NativeOriginOf借稳定header/RawToken跨physical prefix，不回virtual Definition。canonical物化/reload/memo key与raw退休key分开且scratch/实际物化一致；原count0的新actual source、no-body/Tick、pending、terminal/fault/copy/capture都要保。原资源行随成功body提交，kernel直接借Ready，不加N handle/current副本，也不逐emit重扫Params。
4. **M1尚无已证的大范围scalar替代。** 最新有限复核扣除已有birth/EA/deferred/flags/scalar-demand后，仅确认PSHUFD immediate→mask→immediate往返可避免。它作为native向量迁移的构造前提：纯能力选择公共literal，原Indexed/manual/Unknown/EarlyOff及retry保合法合同。不新增一套mask证明去永久化可消除的工作；没有动态权重或收益证据，不能拿有限向量改进补足两倍预算。

对应的逐文件职责和先后顺序已写入[主方案5.3.12.6](spill-and-compile-root-cause-plan-2026-10-28.md#53126-下一大段让质量决定成为真正可编码的程序)、[M1边界](spill-and-compile-root-cause-plan-2026-10-28.md#527-当前可证的构造改进与未闭合的大项)及[结构细则5.1.4](middle-pipeline-file-layout-2026-10-28.md#514-c4实际接缝的文件归属)。native proof仍有每候选P×D occupancy、重复成功子证明、链/point查询；preflight/freeze双resolve、原publication乘积扫描、CFG及真实retry继续计费，不能由只读视图或组件数量推出compile上限。

固定FEX对照仍用第3/9节已有快照：借鉴其目标形态早定、同程序分配落实物理输入和JIT直接读取，不能照抄其块内RA或降低最终生成质量。先使完整普通服务退出再做一次整包方向，最终集中原6.3/6.4；无新FEX分母或性能倍率，**≤2×尚未被证明**。

依据：[源码冻结](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/plan-revision-20261031/source-freeze.json)、[native组件](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/native-schema/review.md)、[kernel读口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/kernel-view/review.md)、[FPR WIP与缺域](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/fpr-proof/review.md)、[有限M1复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c3-initial-constraints-20261031/integration-01/corrections-02/next-m1-service-review.md)。


## 18. C4同合同兑现与最终读者接线

当前source6f2f2dfd/main495bf4c6，较C3改26文件；四Android目标统一构建，本版10cases805断言及physical-consumption164721检查/1注入通过。前一133d的native tag12例11过1失败，11阳性保原身份；只续跑修正后的整合例及原未执行两命令，不合称当前全native通过。两个设备namespace核hash关闭，无活动进程或清理错误。

本组把同source/view/point、不改defs/birth的实际角色/编码与publication转为同体native数据；无body publication保真实Implicit state-use。原FPR family共享独立reader，FPR edits逆序恢复且重新Prepare同一live之后才原retry；actual resolver借现current frame、原资源行随body提交handle，最终原位Freeze并退休。Low32明确低视图与普通位置查询保不同优先级，raw退休key/count与canonical MEM物化key分开。NativeReady编码/true-false/退休分支已编译；本fixture没有执行新生产emitter或认证完整memory transport。

真实失败均保留：6461的无效BOOL枚举已撤回（现IR布尔语义用整数carrier）；e8ac是旧friend未随verifier模板化同步；133d整合例误用依赖旧fixed descriptor的semantic inputs入口，后以原Writer按program scope借同一controller inputs解决。未弱化原断言/政策，也未新增类型、证书表或控制器。

仍需完成：width/low32桥、direct narrow/跨类FPR cohort的真实预RA uses/defs/birth及joint write/active/pressure；完整native state/entry/observer/fault/GPR最终核验、实际memory forwarding/adoption和生产prepare；M1/M4大幅净退出。FPR静态form后置改写可使journal改动数C≈N，必须计容量/工作并在下一预RA合同中收敛，不能靠D+1整scope重跑宣称根治。原normal旧Describe/Complete/binder仍在，无新CoreMark/compile/QoR/PMU/FEX，两模式≤2×未完成。

[检查点与完整源码补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/integration-02/c4-checkpoint.json)、[B类真实依赖](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/actual-gpr-forms/b-cohort-dependencies.md)、[FPR/M1/cohort复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c4-native-realization-20261031/fpr-proof/form-state/m1-and-cohort-review.md)。第17节是此前未构建文档快照，原限制不转为当前所有形态仍未实现；最终门不改。


## 19. 最新方案修订：预RA合同与真实普通成本

本轮将最新结论直接写回主方案和设计细则，**没有新增实现、构建、设备运行或性能结果**。文档修改前工人已在安全编辑边界停写；当前C5 source`7d6218901c98f7ef526b54012add6ef5a3a758fc200573aaae0634351f439861`，只比已检查C4/6f2f多6文件WIP。4文件是PSHUFD纯能力保形，2文件是未接调用的native U32 primitive读取；preRA FPR、cohort、native state和normal入口仍未贯通。C4 main495bf4c6及其限定检查只属于C4，未产生C5 ELF。

| 最新发现 | 已写回方案的修正 |
| --- | --- |
| FPR静态form晚定可使checkpoint改动数C≈N | 在semantic→native时直接选静态form，initial按同schema、实际effective scalar-insert与form-specific scratch核需求；晚事务只处理真正依赖位置的决定。当前代码尚未完成该前置。 |
| 转移definition与同home不够表达一次写的多视图 | whole reference保完整值等价；独立write_owner表达同site/birth一次真实写。原assignment须完整联合lease、active/pressure、next-use/victim、spill/reload、fixed/tie与释放，不用跨宽whole REF、假use或只回填Map替代。 |
| direct-narrow/跨类load还缺真实主输出替换 | 原事务直接提升已有后方结果并退出无其它实际义务的旧primary；保raw身份/access/fault。当前append-only checkpoint不允许先Extra再任意erase；VOID publication不造V128，SCVTF/FMOV不因转def而消失。 |
| 每候选全扫program不是反向用途服务的等价替代 | 先完成会改用途/flags/ID的工作，再由唯一UseFacts匹配并封存图/类型资格；封存后只作读者关闭/转交，实际位置/压力/state仍归native，不新造P_candidate×E。GuestStateGPRMatch共享原byte/label/CFG算法、width共享原transfer/meet，不删阳性或造第二历史。 |
| 单调禁用候选仍可能导致逐候选全scope重建 | 局部cohort在最早受影响owner前履约/回退，维护原live与稳定调度；全scope重启只作明确受界困难回退，不能作为compile主算法或偷偷消耗原eviction预算。 |
| NativeReady生产分支已编译，但没有生产者贯通 | 原Storage在旧Describe之前接完整新prepare；全组semantic读者先闭合。最后actual state/transport、原retry与独立检查完成才Freeze/emit；不先付完整Describe/Complete再保存新body。 |

对照仍固定第9.1节的FEX源码：借鉴早定形态、RA落实实际操作数、JIT直接读与原owner复用；保SVM自己的Function范围与质量合同。把原同义生产者/读者/循环实际退出写成完成条件，比“少pass/少文件/同槽”更能限制普通路径成本。M1少出生/首次服务和M4触达仍与M2/M3并行，有限PSHUFD不能补齐总预算；旧48～50% qualified-cycle观察并集不是whole耗时份额或预计收益。

主方案现在以[5.3.12.6](spill-and-compile-root-cause-plan-2026-10-28.md#53126-下一大段让质量决定成为真正可编码的程序)集中维护完整算法，以[7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)维护施工顺序，[IR的三种关系](ir-representation-redesign-2026-10-28.md#532-一个真实写的多个视图不是完整值等价)及[逐文件归属](middle-pipeline-file-layout-2026-10-28.md#514-c4实际接缝的文件归属)同步。中途只统一构建/最少受影响语义；完整服务一次方向、稳定后集中原最终门，不逐helper加QoR/PMU/FEX。

**性能可达性仍未被证明。** 本修订排除已知的重复解释、静态大journal、假alias和逐候选全扫等路线，但不承诺实现后天然≤2×。最后仍须不降低生成/运行质量、满足资源合同，并让独立Block与Function各≤2×同轮fresh固定FEX。主方案6.3/6.4冻结原文不改，原测量/失败/撤回各保原身份。[本轮源码与文档冻结回执](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c5-native-state-cohort-20261031/plan-revision-20261031/start.json)。
## 20. C5实现复核与剩余闭合顺序

本节按当前C5源码`85fb7bd4bfafe2dfd6aad8307a50b657181630a0e751da6f8fefd2e812f3b22d`修订方案；较7d621890文档快照更新36源文件，完整normal仍关闭。**最新四目标统一build-03失败，源码前后不变，未复制合格C5 ELF，也没有C5运行或性能结果。** 失败点是新增state fixture直接调用受保护编辑接口；此前f4b1e43b的move/完整类型错误、63eb2824的graph matcher局部变量遗漏各保原失败身份。接线不再写成“仅6文件helper”，也不把C4限定通过数转给C5。[最新构建回执](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c5-native-state-cohort-20261031/integration-01/build-03/build.json)、[失败源码保存](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c5-native-state-cohort-20261031/integration-01/failed-build-03/receipt.json)。

| 最新代码事实 | 方案修正与仍待完成 |
| --- | --- |
| 静态FPR form、effective Config和actual scratch已有共享生产/检查源码，native晚realization移除相应静态改写 | 生产转换入口未接通，normal费用尚未退出。used VecFCmp/flags、Sse42第四payload和旧Indexed mask仍有缺域；保原retry政策，不能靠假零scratch或Unsupported→ContractRejected补洞。 |
| write_owner、Map shared_write、联合区间/pressure/next-use与GPR/MEM lease已有共同core源码 | 保raw逻辑身份、whole-reference和物理写三域。primitive首域不代表Capture/LONG/非primitive或完整fixed/read/member tie；现SemanticPolicyRequired须在preclose可判断，最终仍需迁完原质量能力。 |
| shared site的primary或任一member活跃均驱动ScratchValueUses | 不只核预算，还保flags临时寄存器、flags-only与dead-result spill正确；root UseCount=0不能删仍有成员需求的真实W写。MEM/implicit/terminal/Tick继续计价。 |
| ReplacePrimary/Additional与checkpoint、OriginalArgument及raw-only合法身份原语已有 | direct-narrow/跨类真实selector仍须完整兑现源、def、effects、access/fault、observer与transport。不能跨宽whole alias，不能VOID造值、删除SCVTF/FMOV或任意erase/reorder。 |
| shared MEM backing和按canonical key去重已有，MachineOwned仍依owner独立次数与raw_exact | 成员会保守失去完整ownership/definition-transfer/handoff；在原region内联合actual义务与各raw退休恢复forwarding/adoption，不能把共享槽或去重点当完整运输证明。 |
| checked/native共享原byte/label/CFG matcher、WidthTransfer/Meet及preclose稀疏Residence拓扑 | canonical reference/raw_view/实际early write分别保真。CohortRequired按原domain/index接真实预RA合同和局部回退，不静默丢阳性、不逐候选整scope重算。 |
| BindNativeConsumers可在编辑期准备actual frame；final NativeResolver仍要求冻结 | 位置选择借原编辑期role/copy服务，原事务保至独立GPR/fixed/width proof后才Freeze。C4在Freeze后运行A类fixture不证明生产晚拒绝/rollback已闭合，更不能靠放宽最终resolver接通。 |

**还要修正一次“稀疏即便宜”的判断。** 当前SubmitNative对每Copy扫描(read,pub)内publication，最坏仍有P_copy×P_pub。该logical guest-write资格可在原preclose按点首访用固定home数量的临时last Set完成，partial写与严格端点保真，随同一match交付并退出晚pub循环；actual clobber、early write、observer/fault仍由原state和独立checker判断。其余byte/extension窗口、链查询与CFG Φ仍照计，不将少一段循环写成整个state线性，不单开索引微试验。

新增净成本也须记录：提前匹配了原Map gate会拒绝的候选，原inputs的publication资格与alias/use payload，按需catalog/bitmap/foreign set，reused域内raw E resident计数、B级clobber投影及home/CFG转移。保原!reused早退；共享算法和同槽本身没有性能资格。这些工作须与原Describe/Complete、语义投影、binder及无消费者初始化/清理的退出共同核算。

与固定FEX的对照仍以[第9.1节](#91-fex真正便宜的常态路径应对应到具体工作)的原源码流程为准：早定真实形态、RA落实同一程序的操作数、编码直接读取，并由原owner复用/结束。SVM保独立Block/Function、异常恢复和质量合同；不把完整旧链跑完再转换，也不把Function降成Block来凑数字。M1广泛减少出生/首次解释与M4真实触达仍须并行，有限PSHUFD或shared-write组件不承担整线提速承诺。

下一顺序已写入[主方案7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)：收口当前构建/最少受影响语义；闭合preclose与静态family、真实B/共享transport；原state/CohortRequired与独立proof；编辑期位置查询、最后Freeze与真实生产入口。文件职责沿[原文件归属](middle-pipeline-file-layout-2026-10-28.md#514-c4实际接缝的文件归属)，不新增allocator、manager、operand副本或certificate层。完整旧服务退出后才做一次整体方向，稳定后集中冻结6.3/6.4；不逐helper重跑QoR/PMU/FEX矩阵。

**仍无已证明兑现≤2×的完整实现组合。** 最终不降低生成/运行质量和资源合同，独立Block、Function各≤2×同轮fresh固定FEX的目标不变；没有新compile点或FEX分母，不能报告新的差距比值。本轮只更新六份方案与证据链接，源码/Git索引及历史结论不变。[本轮冻结回执](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c5-native-state-cohort-20261031/plan-revision-02-20261031/start.json)。

## 21. C6整线复核与事务闭合

本节接续C5构建/执行和C6三组件审查，修改同一完整服务的相序、职责与成本合同。**最近限定检查基线是C5/329cb954；C6主树3d740a7d是本轮未构建快照，外部组件未合入、未构建/执行，normal仍关闭。** 本轮只改六份文档，不合代码、不启动构建或设备。

### 21.1 资格与实现状态

C5四Android目标及本版11 cases / 47428断言、physical-consumption 164721检查 / 1注入通过。11例为native state借用37断言、9个FPR/coalescing例635断言、原Function width例46756断言；state例只走`reused=false`与一位置窗口，不证明完整reused fold或byte/observer matcher。351bcb03的14 native中13阳性、state例失败保原身份；1017通过断言含失败例前缀，不能全归13例或转给329。build-05只修fixture在Describe前借真实配置pool的顺序，保FlagsRegsEnabled默认true及x12，不改policy、不放宽ProgramInputError。[C5检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c5-native-state-cohort-20261031/integration-01/c5-checkpoint.json)。

| C6部分 | 当前状态与未完成范围 |
| --- | --- |
| 主树 | 编辑期NativeLocation、GPRCopiesFor借用、共享Get/Set shape、PublicationValue64→Covered规范化及logical Set窗口前置已有未构建源码。新的真实reused/observer/capture fixture未执行。 |
| 外部target/state | 已冻结；当前target阳性限原matched no-source Copy、本块U32 primary且id等于actual birth、LoadU32或Add(MEM)及已兑现桥/别名。调用原Recolor兑现fixed/early write和同源implicit commit，Copy后每受影响B一次最终fold/install再selected。未合入。 |
| 外部独立GPR/fixed/width | 六路径补丁已冻结，三个private reader/core headers共享原算法；未合入，不等于生产成本已退出。 |
| 外部shared transport/原事务 | 12文件草案已冻结，含raw traits、普通native资源policy、required_backing/物化寿命及原AllocationEdit的copy/preservation/region journal；未合入。仍缺完整selected memory/PublicationValue adoption、flags/control/recovery和复杂保存路径，Stage/Submit不授NativeReady。 |
| 原服务待修处 | native Recolor部分nonfixed仍以id作actual birth；最终actual scratch、静态B/完整family、生产准备/真实Ready emitter、M1/M4退出未闭合。 |

[外部target合同与成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/state-cohort/target-recolor/review.md)、[外部proof合同与成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c5-native-state-cohort-20261031/next-group/native-proof/implementation-review.md)、[文档轮冻结](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/plan-revision-01-20261031/start.json)。[外部transport范围与未覆盖域](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c5-native-state-cohort-20261031/next-group/shared-transport/doc-boundary-summary.md)。外部receipt仅证明材料保存，接口同名或C5通过数不能转成C6资格。

### 21.2 先兑现稳定B，分开三种关系和两种寿命

Get不在`HasCanonicalU32Carrier`白名单，LoadU32/Add等真实primitive W组不能为Get→To64背书。后者仍是真B：由preclose原稀疏资格驱动预RA把publication实际输入改为Get U32与明确clear-high W，仅无其它实际义务时退出bridge machine use/def，保raw身份/退休，再由唯一machine live计算Get真正end。不能伪造wide shared-write、跨宽whole alias或假implicit读，也不必只因缺动态hook就延后全部B。精确read/extend/narrow/alias anchor资格写原候选行，不整B禁recolor或建N目录；只有真正依赖Map的B才需原owner调度维护/局部回退。

logical/raw身份、whole reference与actual write/lease分别表达；raw最后使用决定逻辑退休，canonical全组最后需求决定物化/transport scratch结束。root count0不删活成员依赖的真实写，短member结束不free共同home；纯GPR合法alias也不自动获得另一实际writer的MEM lease。完整Capture/LONG、direct-narrow/跨类/fixed/tie仍须兑现原阳性政策，CohortRequired不冒充ContractRejected或静默关优化。

### 21.3 原事务保到最终证明，失效数据不能自证

原spill regions早于部分coalescing和late state建立。body/Map/fixed、copy/width/capture/selected、reload/backing与派生读者须沿同一个AllocationEdit和body journal收口；不能中途Commit只为安装copy。捕获受影响writer/site/birth/key/旧lease与预算，同key动作合并；接受early write即更新原按point投影，不每publication Reset再扫B。Copy决定结束后，仅受影响B补一次最终catalog/fold/install，再selected，这是initial fold外的一次额外工作。

整批必要live/transport更新后，按真实旧lease保留、降级或退出失效regions，再独立证明。canonical MEM且有真实preserved suffix/capture的read-elision应按RequireBacking核原fixed/copy；只有全组实际读均独立来自GPR且无MEM/reload依赖才可NoMaterialization。最后verifier还须检查本批编辑请求的分类，不能宽泛检查“有preserved可读slot”就让错误NoMat通过。真实writer/key/birth改变或end超旧lease回原owner，不能晚扩lease；失败还原Map/copy/selected/region/obligation和body并重绑。原preservation通常借现成payload，只有实际失效才journal后重建一次。

Covered Get保availability definition，Covered Set保logical guest commit，真实W producer才有其机器body与输出；Get/Set Effects mask、ProgramDefinition枚举或PhysicalGPRWrite行均不能自证硬件写。NoBody不凭旧definition-owned reload证明capture；合法preserved MEM suffix需真实transfer/宽度/offset、copy到slot与Function路径支配。最终actual scratch按实际form输出、绑定输入及真实copy/reload/preservation计价，不能按所有MapMEM availability defs或raw参数盲加reload，也不能保未初始化region来抵扣旧公式。

独立GPR/fixed/width共享原算法，分别核raw资格/Original closure、actual lease与copy/capture；Function沿真实CFG/return关系，不按RPO数字窗口证明来源，普通同点read-before-write与terminal-after-output保真。检查在最终virtual、locations编辑期进行，后续实际编辑使其失效；final resolver仍需Freeze。不支持/非法域typed拒绝，候选不自签Passed证书，Stage/Submit也不授最终Ready。

### 21.4 迁移组件与性能交付分开

logical Set前置只退出对应`P_copy×P_pub`，实际clobber/observer/fault仍需证明。target草案保窗口`Σ(W+E_window)`及原Recolor逐候选全域Map/width/low32/selected/reload；外部checker保Q×D/Q×N、困难raw闭合Q×E、Covered-read Q_read×E_actual、Map链；CFG/return的B²是每次路径查询，路径成本须另计T_path×B²＋Σ_q body_touched(q)与每查询B级scratch。T_path由相关consumer/capture及递归copy查询驱动，不和Q_read×E_actual当作互不相依的总账相加。最终B fold/排序、projection插入、member聚合、journals、preservation重建、Φ、双resolve均照计。**这些首先是正确性读者迁移，没有证明线性或完整compile收益。**

最终普通服务应在原按home/site的state/quality访问中批量处理相关义务，优先用原完整稀疏raw见证，共享必要的实际定义/固定写准备。独立性要求从实际结果推导义务，不要求每Q全扫scope；仍依赖乘积算法的普通域不能作为最终性能实现交付。全scope重启仍有`(K+1)×(N+E+P)`等成本，只能是受界困难回退，不再开已否决owner/source/bitmap微试验来代替整线。

固定FEX对照继续以[9.1具体流程](#91-fex真正便宜的常态路径应对应到具体工作)为准：早定真实形态，RA和编码消费同一权威程序。原Storage在旧Describe之前接支持域，semantic全组闭合→静态B/form→一份machine live/原scope RA→编辑期决定/同一事务→最终检查→Freeze/Ready→真实编码/Flush/发布/结束；不先跑完整旧Describe/Complete再转换。M1广泛少出生/首次解释与M4真实触达/结束并行，有限PSHUFD或一项proof不承担整线承诺。

最新相序见[主方案5.3.12.6](spill-and-compile-root-cause-plan-2026-10-28.md#53126-下一大段让质量决定成为真正可编码的程序)、[7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)，职责见[逐文件表5.1.4](middle-pipeline-file-layout-2026-10-28.md#514-c4实际接缝的文件归属)、[IR late寿命](ir-representation-redesign-2026-10-28.md#533-late编辑的值区域与借用寿命)。相依组一次构建/最少受影响语义，完整旧服务退出后一次整体方向，稳定后集中6.3/6.4，不逐helper加QoR/PMU/FEX。最终不降生成/运行质量和资源满足，独立Block/Function各≤2×同轮fresh固定FEX不变；目前无新计时分母、无已证明达标组合。


## 22. C6构建与首轮语义：相序已经落地，闭合尚未完成

本节替代第21节“当前未合入/未构建”的判断，保留其3d740a7d文档快照。当前C6 source `a1ddfde82da41ee0787f2a79297ca2718e4eac510256aedf042851f41f72eb2c`，2402个source/build输入，相对C5更新35源文件。原target/state、shared transport/AllocationEdit、独立GPR/fixed/width、actual scratch和两阶段资源准备已整合；四Android owning目标构建通过，最少语义仍有失败，新normal关闭。**没有新compile、CoreMark、QoR、PMU或fresh FEX结果，不能把旧毫秒或比值换成当前身份。** [当前检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/c6-checkpoint-build-03.json)。

### 22.1 构建与语义结果按原身份保留

| source / 执行 | 结果及仍未证明的部分 |
| --- | --- |
| ec916aa0 / build-01 | min/max实参类型不匹配失败；只修明确u32类型，不改proof规则。 |
| 1a9adc2f / build-02 | 新pair fixture误用AllocationInputs默认构造、写const字段失败；改用既有构造接口，不放宽接口。 |
| a1ddfde8 / build-03 | 四目标generic Release/O3/NDEBUG/ThinLTO/noPGO完成，source前后不变；四ELF已保存。 |
| a1ddfde8 / native tag | 15 cases中11过4失败；1108断言中1104通过，包含失败例前缀，不能全部归11阳性。其余FPR/coalescing、Function width和physical-consumption三命令未执行。 |

[构建回执](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/build-03/build.json)、[首轮执行与原日志](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-01/execution.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-01/closure.json)。自有namespace核hash关闭，无活动进程或清理错误。C5/329仍是最近完成整组最少检查的基线，不替C6补未执行项。

四处失败已缩小为两类接口问题。三例在Describe initial返回UnsupportedForm：Generic SetHostGPR默认inline_reads为0x1f，新共同schema却要求严格为1，实际ForEachUse仍只有cell0。修同源默认producer或真实输入解码口径，Covered仍严格零inline且有真实implicit source，未知NoBody仍拒绝；不以关政策或绕过schema修正。Get64与此轮Nop不是该拒点。另一例是新增pair Recolor fixture在AbortTargetPlanning后调用要求locations冻结的Finish；按原失败结束协议修fixture，不删生产断言。当前reused/observer、联合proof/transport rollback及resource断言还未走通，不能称已验证。

### 22.2 最终资源准备必须在首个不可逆替换之前结束

原AllocationEdit提交、locations冻结与body物理化是三个不同边界。独立proof/actual scratch依赖编辑期NativeLocation，先完成；原Submit/Commit只提交Map/运输，之后EndTargetPlanning冻结locations，才取得final NativeResolver。

```text
fresh live / transport失效处理 → 独立proof＋actual scratch
→ 原AllocationEdit Submit/Commit → EndTargetPlanning
→ final resolver
→ 全体body：actual schema/profile → Decode → BuildNativeCurrent
               → StoreInstructionState → PreparePhysical(handle)
→ PreflightNativeControls
→ 全体body FreezePhysical → FreezeNativeControls → stream.FreezeNative
→ PhysicalReady → 真实生产kernel/重发/Flush/发布/最后结束
```

program.Definition拒绝committed前缀，不能逐条“描述→Freeze→描述后继”。成功Prepare保virtual cells；原pseudo_demand word退役后存原资源库handle，payload在ResetCurrent前保存，不建N handle表。prepared后禁止普通source/form/effects修改、新checkpoint和PseudoDemand读取；重复Prepare只匹配同一不可变位置/角色，失败保缓存，冲突handle先拒。旧checkpoint尚可在commit前整体恢复，但对应proof/profile/resource失效。controls须在首个body Freeze前预检真实kind/id/type/raw-view/consumer/Whole角色；Freeze仍二次resolve，实际替换后失败废弃scope/prefix，不能做普通RA retry。Controller Finish只释放请求输入，不授Ready。

### 22.3 静态B交付真实程序，也修全部旧输入消费者

Get32→To64→publication仍为外部首域，未合入、未构建。由稳定匹配在首次live/initial/质量Bind之前改actual publication source，保64位commit与clear-high W含义；无其它实际义务才退bridge machine use/def，保raw header/Original/Tick。同步使原quality view失活、修软publication hint的真实source/birth及原preferences；后续所有reader须消费inactive语义，不能只让selector跳过。native width提交检查应读同源GPRInputs publication的actual stored/home，不能从已清空的semantic fixed_validation误判没有commit。

候选Get本身及Wide的hard fixed/read/tie、须扩旧preserved lease、额外actual bridge reader和Get64/extract在修改前typed拒绝；这些是迁移缺域，最终仍须保原合法政策，不能变成永久关coalescing。候选D_B及hard行批量排序查找、actual-use closure一次处理，不能每bridge重扫E，也不复制整份GPRInputs。

U32类型不证明已有真实W capture。只有真实Get32 W写、实际目的区别于旧guest home且capture保留，才可用真实Implicit source省publication copy，同时保64位clear-high commit；不能降成丢语义的Covered、冒用AdjustResident或扩大primitive白名单。否则publication发真实W copy，source独立可用。所有Copy/PublicationValue/NarrowRead/Get body修改在受影响B最后一次fold/install前结束，再selected；不能把新捕获自身作为消除该捕获的依据。最终physical前缀需要的最少raw关系随原body/codec合同保留，不靠重新读取已物理化定义或新N目录。

### 22.4 联合事务不能用已失效的结果证明自己

原AllocationEdit的copy/width/capture、preservation action、Map/fixed/selected、regions/backing journal已经合入，body checkpoint仍由原调用方持有。批量Stage更新必要live并按真实旧writer/key/birth/lease保留、RequireBacking或NoMaterialization；超旧lease回原owner。SameReads比较actual source/view/role/点/读种类，Numeric→Implicit可能保持相同count/end，不能只比聚合数。该比较不证明output、lease、fault或backing；D=0不建pending，仍fresh proof再普通Commit。

联合fixture以真实RA得到preserved MEM suffix，先故意给错误NoMaterialization，再要求独立proof/classifier拒绝、完整还原Map/copy/region/body并重绑，之后重证。它还要运行到这些断言；当前initial失败不能记作故障注入已过，也不能拿被删除的旧def-owned reload证明capture。合法suffix核真实fixed transfer/宽度/offset、copy到slot及Function路径。NoBody availability不当硬件输出，actual scratch核真实绑定、implicit/terminal与preservation前缀峰值，Invalid/Unsupported与真实shortfall分流。

### 22.5 构建接线不提供compile性能上限

固定FEX的可借鉴点仍见[9.1](#91-fex真正便宜的常态路径应对应到具体工作)：早确定真实形态，原RA和编码直接消费同一权威程序。当前SVM只是把必需合同接到同一程序，旧生产Describe/Complete/binder尚未退出；没有可以据此宣布达标的组合。

native actual birth修复仍枚举真实definitions；原Recolor逐候选全域、raw闭合Q×E、Covered read Q_read×E_actual、CFG路径T_path×B²＋实际body访问、窗口/最后fold/排序、journal和Φ均保成本。final proof与resource还重读profile，Prepare/Freeze两遍及双resolve不因复用handle word消失。普通域应在原按home/site访问中批量准备必要义务并复用原稀疏见证，独立性不要求每请求扫全scope；这些乘积不能留到性能验收后再修。不新增证书或缓存管理层，不重开已撤owner/source/bitmap微包。

顺序已同步[主方案7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)及[逐文件职责5.1.4](middle-pipeline-file-layout-2026-10-28.md#514-c4实际接缝的文件归属)：修真实断点→静态B/完整机器合同→原事务最终proof/资源→旧Describe之前的生产准备/真实编码，同时退出M1/M4重复工作。相依组一次构建/最少受影响语义，完整旧服务退出后一次整体方向，稳定后集中冻结6.3/6.4；中途允许生成质量变化，不逐helper跑QoR/PMU/FEX。最终质量/运行与资源门、独立Block和Function各≤2×同轮fresh固定FEX保持不变。

## 23. C6最新集成：真实机器合同与生产替代顺序

2026-11-01修订。本节承接第22节，历史正文不改；以下区分最新已构建代码、未构建测试WIP和外部候选。**新normal仍关闭，没有新的compile/QoR/PMU/fresh FEX结果。** 当前问题已从initial接口推进到真实Capture/shared-W、普通发表最终form和原生产准备owner；不能继续按第22节把Get32 B或fixed/preserved producer写成尚未合入。

### 23.1 最新验证边界

| 身份 | 实际结果 | 未获得的资格 |
| --- | --- | --- |
| 858f5ffb / build-04 | 修SetHostGPR默认mask及pair Abort/Finish协议；四目标通过，原4失败例续查为3过1失败 | 联合例转到旧fixture fixed输入结束协议，尚非整组通过 |
| 645f5e51 / build-05 | fixture调用隐藏的基类重载导致编译失败，失败原文保留 | 无新成功ELF；临时hook随后由真实preclose producer替代 |
| 40ab44a2 / build-06 | 四目标通过；联合例推进到真实reload缺失 | 保持原完整压力直到suffix会占满reload home；后续改正常消费次序，不手填region或更改eviction |
| 33e1dc33 / build-07 | 四目标generic Release/O3/NDEBUG/ThinLTO/noPGO通过；native16例15过1失败，1897断言1896通过 | 通过数含失败例前缀；联合例width/capture请求仍为空，其余3条命令未执行；无完整rollback、生产执行或性能资格 |
| 63fcbc7c / 当前共享树 | 仅在33e1之上改测试：真实SelectZero→To64发表请求Capture，Get32另一home增加真实observer以走W-copy | 未构建、未执行，不继承33e1通过数 |
| 三项外部候选 | SelectZero backend W-view、typed普通GPR发表、原Storage整组生产接线 | 未集成/构建/执行，不能写成normal替代或≤2×交付 |

33e1完整source为`33e1dc3391674d6314b7124501e99191ab44a7abc37daf7262b9e9c3022373f8`，2402个输入。通过组件含actual-birth/pair、reused/observer/capture分支、初始/最终GPR/FPR/scratch资源及Get32两home到PhysicalReady；Get32两home可同走retained capture，仍需真实W-copy及生产emit。联合例已取得真正preserved MEM suffix/reload，但未走完后续独立分类、错误NoMaterialization拒绝、rollback和重证。C5/329仍是最近完成整组最少检查的基线。

[build-07及四ELF身份](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/build-07/build.json)、[最新执行及失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-04/execution.json)、[设备关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-04/closure.json)。设备namespace已核hash关闭，无活动进程或清理错误。本轮只更新文档，没有新建运行。

### 23.2 真实输入与机器形态的断点

**一、真实fixed/preserved输入应在最后semantic阶段直接生产。** 新接缝已共享原四类匹配主体：同次GPR topology可导出原preclose窗口，随后借相同Facts/workspace向原AllocationInputs.fixed写真实id/birth声明，窗口在TakeIR前结束。Block/Function保各自范围；没有先分配Map/placements再复制或抛弃，也不再让fixture从旧Describe提取fixed行。这不覆盖hard shared-write/read/tie的全部联合约束；排序插入、显式seed原子复制与原窗口访问仍有成本。

**二、Capture与实际W写必须由真正生产者成立。** 原SelectCapture资格来自Set(To64(SelectZero))。手工多调用一次fold并传true，不能改变最后fold使用的原Residence.native_capture；额外ordinary use只阻止两birth单use偏好，bridge前observer也不阻止width策略扫描bridge.birth→end。因此须在首live/initial/Bind前，由backend严格证明Generic SelectZero的真实U32 primary/三输入/无flags与额外独立输出，再沿原cohort交付同site/birth宽视图。公共HasCanonicalU32Carrier不扩；独立proof读实际写/lease，不信WidthSelected或U32类型。该外部候选才是联合例当前需要的机器合同，不能再换IR规避width、强制capture或关闭质量策略。

**三、普通GPR发表尚缺真正最终form。** 当前RealizeNativeGPRPublication可对非coalesced Generic返回true，真实EmitSetHostGPR却只消费Covered/Copy/Biased/PublicationValue。新producer应共用原PreparePublications的width/offset/ClearHigh/AdjustResident代数，写出真实source role与body，首写通知原journal，typed返回缺域。32位同home只有low4相同，高位未知仍需MovW；不能错误Covered再把合法质量域变成Unsupported。Get32特殊Clear64不能被默认规则覆盖。MEM adoption/fused/尚未迁移Copy或Biased/selected memory继续明确缺域，不能假AdoptsSpill或留Generic给kernel。这是外部候选，尚无运行结果。

已合入Get32稳定B保raw header/次数/Original/Tick，真实Wide use/def退出；同步所有quality/actual hint/preference读者，raw hint value/source保持。无hint支持，wide-only/未限定hint、hard fixed/read/tie及lease扩张事前typed拒绝。actual-use闭合批量一次，候选与hard行排序查找；不逐bridge扫E或复制整份GPRInputs。物理提交后保最少raw源关系为无Original/Output/Implicit标志的Label，后继只读header/token/view，不回查committed定义；该混合前缀已由组件例检查。

**同组前置还包括真实late scalar form的共同schema。** 源码核对：RealizeNativeScalarGPR可产ZeroExtendWriteW、SignExtendMove、ZeroExtendMove、LogicalImmediate、OperandAbsolute、OperandDirect，CheckNativeInitialForm在特殊view/FPR/Get/Set分支外却只准Generic/GetResultRaw/BitNumeric；最终actual scratch复用该入口，合法late结果仍会Unsupported。应在原scalar family共享严格actual form/use/def/effects，不能仅扩枚举白名单、假Generic、关features或让compiler另写一套规则。未迁源域只在关闭前精确typed分流，不能拿迁移缺口永久缩小normal。此项尚未实现/验证，不由Storage候选代替。

[Get32合入回执](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-merge-04-static-b/receipt.json)、[fixed/preserved合入回执](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-merge-05-fixed-preclose/receipt.json)、[尚未集成的SelectZero W-view及成本](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/native-select-w-view/review.md)、[尚未集成的发表候选与调用合同](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/native-gpr-publication/interface-and-order.md)。外部回执只证明候选材料保存，不代替主树构建或执行。

### 23.3 原Storage串通整组，破坏前分流，破坏后保持单一程序

外部生产候选只扩展原Storage，不新建pipeline/transaction/certificate manager。真实顺序为：

```text
整个Function组：最终SSA与实际pool/config
  → static family + GPR residence/fixed/preclose窗口
  → entry/terminal/edge/shared-call等最后semantic元数据
  → 全组TakeIRWithCFG → 全组CloseSemantic/BeginNative
各原scope：静态B/W-view → 一份actual live/initial/quality Bind
  → 原Block或完整Function controller / RA / 原retry
  → 原state/Edit + 实际Copy/发表/Get/form编辑
  → 受影响B最终fold/install/selected + 必要live/transport失效处理
  → fresh GPR/fixed/width/FPR proof + 独立transport分类 + actual scratch
  → Submit/Commit → locations冻结
  → 全body schema/resource/Prepare(handle) + controls预检
  → 全body Freeze + controls Freeze → PhysicalReady
  → 原kernel/重发/Flush/发布/最后结束
```

原Function始终整体RA，单块Function也不降格Block。flags-token/local-control/hoist、used FCmp/Sse42/Indexed-direct、复杂memory access/selected等未迁源域在semantic关闭前typed选原合法入口，保原config/features；最终交付不能以这份迁移清单永久缩小负载。关闭后缺合同只能整scope失败，不能回Inst、重做旧Complete或从新body重建第二语义图。真正质量ContractRejected才沿原完整恢复/retry；Unsupported/Invalid不伪装质量失败。

原body与Map/fixed/copy/width/capture/selected、reload/backing journal持续到独立最终检查；D=0仍fresh proof。transport分类不接受selector自报，须由实际reads/locations/旧lease/真实preservation与路径独立核SameReads、RequireBacking或NoMaterialization；超旧writer/key/birth/lease回原owner。资源准备和controls预检全部在首个body Freeze之前完成；原Commit、Controller Finish或组件PhysicalReady各自不能授权未完成的生产Ready。实际替换后失败废弃scope，不能普通RA retry。normal gate保持false，直到完整支持域真实贯通。

### 23.4 FEX对照转化为旧服务退出条件

继续使用固定Android FEX `a156752f…`，其反向kill/affinity→正向位置/fill/spill/copy→同IR物理操作数→JIT直接GetReg的源码事实不变；它仍有DFE/CFG、压力冷分支、N初始化与发布。局部CodeBlock RA、每guest flush及较小动态bank不能替代SVM Function或降低最终质量。[固定生产流程复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)。

| 对照点 | 本轮必须兑现的改法 | 不能冒认的进度 |
| --- | --- | --- |
| 早定实际形态再RA | 稳定B与SelectZero W-view在首actual live前，静态FPR/真实fixed输入同源交付 | U32类型、旧policy标记、fixture能Freeze都不证明真实机器合同已成立 |
| 分配结果直接供编码 | 原RA/state/Edit把普通发表及运输写成kernel可消费form，最终同槽物理化 | Generic bool成功、接口Complete、只迁新编码入口不代表旧Complete/binder退出 |
| 一个owner结束实际决定 | 原Storage在旧Describe之前进新链，完整Function组先关闭读者 | 先跑完整旧Describe/Complete再转换，或新增owner/certificate表，仍支付原大项 |
| 请求对象/arena有真实复用 | M4随旧数据退出减少初始化/复制/清理，保原retained预算与结束边界 | 延后释放、移动timer、更多缓存/更小分配范围不算提速 |

M1少出生/少首次解释必须并行，不能由Get32、SelectZero或PSHUFD一族承担整线收益；旧约48～50% qualified-cycle并集包含必要工作，也不是当前可删份额。没有依据为新组合预填可达毫秒或称性能上限已经保证。

### 23.5 把新增成本一起收敛，再做整包反馈

| 仍保留或新出现的工作 | 同一原服务内的收敛要求 |
| --- | --- |
| 固定行排序插入，最坏F²移动；显式seed原子复制 | 保真正声明/稀疏输入，批量有序交付；不复制旧Map/placements或常态完整Inputs |
| semantic entry-width先供旧edge元数据、native state再求一次 | 共用transfer与必要边界交付；实际编辑后失效，不能直接继承旧最终状态自证 |
| scalar内部snapshot＋owner临时snapshot | 首个真实写通知原journal，按D_edit保存触及body；避免每候选两次构造，更不建N快照表 |
| 独立transport按D_edit重扫actual N/E/旧reload窗口 | 利用原home/site/state访问批量核实际义务；分类独立但不要求每编辑扫全scope |
| 原Recolor、Q×D_def/Q×N定义枚举、raw闭合、Covered-read与CFG路径查询 | 在完整普通服务中批量处理必要义务/复用原稀疏见证；无新N owner/Passed目录，不由接口复用宣称线性 |
| 同producer多view的CheckVirtual、definition转移/重复id核对/逐次reserve，可能有Σ D_source²重复 | 新SelectZero primary schema只读固定槽/三实际读，成员仍按原定义/lease独立核；真实Extra backing及原重复访问照计，不把固定大小schema当整个cohort线性 |
| 最后B fold/排序、projection/journal/preservation、profile重读、两遍资源/双resolve及scope retry | 按实际失效/触达记净账；困难回退受界，普通乘积工作不留到最终性能验收之后 |

实施依赖调整为：真实Capture/W-view、默认发表producer及共同scalar schema→原事务/fresh proof/资源闭合→原Storage生产替代，最后两项可在外部并行但按同一相依组整合；同时推进M1/M4和上述净减工。维护落位见[逐文件表5.1.4](middle-pipeline-file-layout-2026-10-28.md#514-c4实际接缝的文件归属)，完整执行表见[主方案7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)。成组一次owning构建/最少受影响语义，完整旧服务退出后一次整体方向，稳定后集中冻结6.3/6.4；不逐helper加QoR/PMU/FEX。最终质量/资源与独立Block、Function各≤2×同轮fresh固定FEX不变，当前仍没有已证明达标组合。

## 24. C6生产准备已运行：发表与净成本仍待闭合

2026-11-01续审，承接第23节，保留第1～23节原文。当前source为`f2e5a7193eaa29bbb61e23913ed43b45dc034bac0eaf7d0614f96203d9a8c56c`，2402个source/build输入。以下引用既有build-11/min05终态，本轮只改六份方案，不构建或运行手机。**默认gate仍false，没有本版guest执行、compile/QoR/PMU/fresh FEX资格。**

### 24.1 已合入，不再列为外部候选

| 原第23节待办 | 当前实现与边界 |
| --- | --- |
| SelectZero实际W与宽视图 | backend严格schema核Generic U32 primary、三Original Whole输入及真实同site写；cohort先于live，独立KnownW/BridgeShape重读，公共canonical白名单未扩。joint尚未到Capture/width断言。 |
| 普通GPR发表 | typed realizer与checked共享control规则，写真实PublicationValue/source role；同home32高位未知仍Mov W。GPR/residence/Get32已接，MEM/reload/fused/selected缺域。 |
| 六类late scalar | initial/最终actual位置与scratch共用严格schema，保width/extension/W tie/alias；And point W宽度与kernel对齐，不统一压窄Definition输入。 |
| 最后form与fold | 原最后site访问已移入SubmitNative：placement/Get sweep→scalar/pub→按需final fold/install→selected；FPR变化先结束，compiler不另跑晚form循环。 |
| 原Storage准备 | PrecloseNative/TakeNativeIR/PrepareNativeProgram与公共PrepareNativeGroup已合入；完整组semantic closure后仍按原scope走controller/事务/proof/resource与Ready编码。 |

四Android owning目标generic Release/O3/NDEBUG/ThinLTO/noPGO通过，源码/cache前后不变，四ELF各保hash。build-08～10同名Operand失败、build-10误重跑相同b55源码均保留，不算新算法检查。[build-11](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/build-11/build.json)、[完整身份与失败索引](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)。

### 24.2 真实入口阳性和两个失败

min05共17 native cases、15通过2失败；1599断言1597通过，含失败例前缀，不能全归15阳性。新增真实生产例的两个section覆盖独立Block与完整双Function组（含跨块SSA/共同parent）；实际Optimize/fresh authority→PrepareNativeGroup→原scope RA/state/事务/proof/resource→NativeReady→true/false/重复false编码→幂等退役通过。没有packed/retained语义fallback；未强制code-cache/region申请失败，未发布或执行guest代码。

| 失败 | 已知事实与下一处理 |
| --- | --- |
| joint Submit | Kind=StatePolicyRequired，尚无domain/index；Capture/width、Stage分类、bad NoMaterialization拒绝、rollback与good RequireBacking未达。先保具体site与actual source/role/location/reload/residence；MEM/reload发表是强候选，不能写成已定位，不改Map/pressure/eviction/backing或原断言求过。 |
| Get32 observer/home21 | Prepare/Submit Complete、publication body=true已过；Get仍Generic capture，测试期待Covered失败，后续proof/scratch/Ready及下一home未达。observer可要求保capture；分别验证capture＋W-copy和真正GetCovered/Get32LowView域，不通过删形态断言悄然删语义覆盖。 |

checked FPR/coalescing、原Function width固定点、physical-consumption三命令未执行。自有红魔namespace关闭，无活跃进程或清理错误。[执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-05/execution.json)、[原始输出](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-05/native-c6-actual-forms-and-owner.stdout)、[收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-05/closure.json)。

### 24.3 已落实的相序与未闭合的MEM

保`state_changed || (forms_changed && native_reused)`，纯role/encoding变化不新建state需求。所有body变化结束后才final fold/selected；原Stage、fresh live/独立GPR/FPR/fixed/width/scratch/transport后才Submit/Commit及locations Freeze；全体virtual资源/Prepare与controls预检先于首个body Freeze。无改候选快照已退出，实际D_edit仍可需局部/整批两个时期的恢复记录，不能计划删其一。

最终发表检查在原GPR site访问内替换旧绑定片段。Whole且canonical GPR home吻合、无preserved覆盖可借RA/live/transport；Value/Low32/preserved核strict-before来源，不借本pub自证。samehome32省Mov W须读最终真实home写/捕获/CFG的高32零，曾经W birth或完整U64 availability不足。该按需查询保真实触达成本，不加N历史目录。

原RA/spill、preservation、shared transport和D_edit事务已接；HasRealBacking按完整primary/secondary output token在actual birth核MEM写，不拿primary代secondary。这不授普通PublicationValue的MEM adoption资格。若定位确为该域，沿原materialization/pending writeback/adoption/fault/backing顺序联改producer、actual use/raw退休、scratch/resource与独立reader，保owns_all_uses、lease及materialization末次需求；不能只去GPR限制、假AdoptsSpill或永久禁forwarding。193 kernel已有native Current读口，待补具体form与完整调用覆盖，不重开全族codec迁移。

### 24.4 六段实际成本与后续退出顺序

显式新链支持域已绕过旧完整Describe、Complete与每emit semantic binder；默认仍旧链。新链仍付最后semantic准备→同Store转换/actual live/initial→两scope RA→A/state/最后form→fresh proof/transport→全虚拟资源/Freeze/Ready编码及结束。现付、已绕过和待退工作统一见[主方案六段净账](spill-and-compile-root-cause-plan-2026-10-28.md#581-当前knownnative的净工作账)，不能把入口跑通算成六项费用都已削减。

| 成本 | 当前要求 |
| --- | --- |
| fixed与entry-width | preclose排序插入最坏F²移动/seed复制、semantic/native双求解仍计价；批量交必要边界，不借失效最终状态。 |
| form/journal/fold | 无改候选快照已退，保D_edit两时期必要恢复、去重/容量；纯encoding不增B fold，真state修改不省义务。 |
| transport/定义/raw/CFG | D_edit×scope分类及原Recolor/definition/path乘积查询随普通服务批处理；借实际home/site/稀疏见证，无N owner/Passed表，不要求每请求全扫来证明独立。 |
| 同home高零 | Q_high按需查实际body/definition/CFG并计B级scratch；最多32 home状态/块不是整compile线性保证。 |
| 资源与M4 | proof/resource profile重读、两遍准备/双resolve、初始化/最后清理如实计；随旧数据退出删触达，不挪timer/释放或扩retained预算。 |

固定FEX仍提供早定form、同IR RA落实位置和JIT直读的源码对照；也仍付数据流/CFG、压力分支、初始化与发布，局部RA不替代SVM Function。M1广泛少出生/少首次解释与M2/M3/M4共同减工；旧约48～50%观察并集非当前可删比例，不保证≤2×。[固定流程复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)。

### 24.5 执行与目标

先定位两失败→按实因修合同、完成原负→正事务/Get分域/未执行最少检查→补普通负载支持域与失败恢复→在原正常调用点切换并删旧服务；不再新建准备owner。施工表见[主方案7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)，逐文件职责见[结构表5.1.4](middle-pipeline-file-layout-2026-10-28.md#514-c4实际接缝的文件归属)。

相依组一次owning构建＋最少受影响语义，完整旧服务退出后一次整体方向，稳定后集中冻结6.3/6.4，不逐helper跑QoR/PMU/FEX。最终不降生成/运行质量、资源满足，Block/Function各≤2×同轮fresh固定FEX；**当前无已证明达标的完整组合。** 本轮不改源码/Git索引、历史1～23节或最终门。[修订冻结](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/plan-revision-04-20261101/start.json)。

<a id="c6-width-transport-latest"></a>

## 25. C6重复服务已收敛：MEM、入口宽度与transport的最新边界

2026-11-01续审，保留第1～24节全部原文与失败时态。当前source `05bc15c71ae2a04d10856763c0f2f90966d50af55dd83d614561b13352c66a48`，2402个source/build输入；build-16/min08已在文档冻结前结束。默认native gate仍false，C7仅外部实施；没有本版guest运行、compile/QoR/PMU/fresh FEX或≤2×资格。

### 25.1 结果必须绑定当前实现

| 身份 | 本版结果，不转借后续资格 |
| --- | --- |
| build-13/min06，53c4947a | 四目标通过；native17中16过1失败，其余三命令过。joint明确定位Form/point41 SetHostGPR、source40 Whole MEM；Get32 capture/W-copy两home的proof/Ready闭合。 |
| build-14/min07，3d86237e | MEM/单一native宽度服务已合，四目标通过；native18中15过3失败，其余三命令过。三失败前提分别是StoreUniform不支持fixed-home consumer、x24非fixed home、NarrowRead仍有preserved Get capture，不以改policy绕过。 |
| build-12、build-15 | 测试诊断变量重名、测试namespace遗漏导致编译失败。保原失败source/log，不归为production服务构建通过。 |
| build-16/min08，05bc15c7 | 四owning Android目标通过。native18中17过1失败，1958断言1957通过含失败前缀；其它9 cases/635、1 case/46756及physical164721检查/1注入全部过。自有设备namespace关闭，无活动进程/清理错误。 |

min08生产准备例的四section已覆盖独立Block、真正MEM源/原压力、完整双Function组及无跨块SSA的width-only Function，沿原RA/事务/proof/resource走到Ready、true/false/重复false编码与退役。它没有强制code-cache失败，没有发布/执行guest，不能当默认normal负载已迁完。partial raw-view例保真实Get32 capture、Covered body和canonical GPR Whole源；不是global LowView@22阳性，原same-home guard不放宽。

唯一失败joint已过SubmitNative/Capture/width、两fault capture及selected home23；Stage为Staged、backed_rows>0，但retained_rows为0。原bad NoMaterialization拒绝、完整rollback、good RequireBacking和后续proof未达；先定位请求、原region与实际use的因果，不预判batch实现错，也不删正行义务、减pressure或改Map/policy求过。[当前构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/build-16/build.json)、[执行与独立命令](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-08/execution.json)、[失败原文](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-08/native-c6-mem-width-batch-and-real-owner.stdout)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-08/closure.json)。

### 25.2 MEM是完整源合同，不能只放开kind

主树typed PublicationValue已支持Whole MEM及原已建立的reload关系；32位MEM保实际Mov W，Get32省body仍限已证高零的真实GPR。producer/resource共享checked原adoption代数；原pending writeback、required_backing、preservation、owns_all_uses与same-key优先级不变。独立reader在原GPR site访问内核完整primary/secondary/shared输出token、canonical slot、actual birth/lease、严格来源与真实CFG路径，不用MEM slot冒充home或用本次pub自证。

完整selected-memory、fused/unestablished reload与local-control MEM仍有明确缺域。新压力section通过准备/编码不授全部adoption、fault执行或guest正确性。原GPR backing写读X/u64，保真实W写后的零高位完整值，不改成只有四字节的假设。[合入差异](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/native-mem-publication/source.patch)。

### 25.3 删除重复semantic宽度服务，保真实失效合同

preclose不再建立semantic entry_state、编译全Function transfer/solve或做30B查询；保原SSA incoming基值、entry contract、terminal和唯一CFG移交。原native按需width solution同时供residence和最后metadata：前者已经求解时删除一次重复求解；前者未需求解时，只把metadata必需的一次求解移到native，不能两种分支都报“省一次”。Block不付Function solver。

首写时的栈上NativeWidthInputs独立读原逻辑transfer输入：opcode/type、完整Original/view、Get/Set home与逻辑offset、LoadImm、canonical-W、helper presence/clobber。late逻辑输入改变typed返回EntryWidth并沿原rollback，不只重算entry后继续借旧copy/selected；role、encoding、Covered和真实机器写历史另按各自合同受检。

FinishNativeResidence在旧结束点释放matcher/window/catalog，仅原B×30 entry facts和必要借用留到joint proof/Commit后，回填原edges/unit_state_edges/compound_clear_allowed/Function entries，再EndNative，先于locations Freeze。没有新N/E表、manager或提前发布metadata。下一ClearCompound要在final form之前只读同一solution和原carry/terminal谓词，不能读默认false或移到proof之后修改body。[合入差异](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/native-entry-width-consolidation/source.patch)。

### 25.4 transport按批次独立重证，不宣称整链线性

原Edits仍是唯一journal。每个D请求分别Classify/ReadsUnchanged/IndependentReads/HasRealBacking和MapCount reload查询已退出，改为Stage前后各自一次EvaluateTransport；两阶段重新产生证据，第二次不借第一次阳性。实际改动site合并完整output/birth验证，原checkpoint只比较一次；NoMat存在才整批一次actual uses＋真实terminal，缺NoMat只看变动body。Stage后按原stored key借reload storage，不能沿新REF隐藏旧行；D=0只跳此服务，fresh proof保留。

| 必须保留在净账 | 准确边界 |
| --- | --- |
| D/K/J排序与join、输出核对 | 临时工作空间O(D+J)，不建N/E镜像；实际native token/定义读取、REF深度和NativeLocation查询都收费。 |
| reload point slots与rows | 原存储是vector of point vectors，访问是R point slots加L rows/key join，不写成纯L。all-SameReads不为零额外义务遍历reload。 |
| preservation backing | 同transfer的D_t请求各查A_t原actions，保Σ D_t×A_t；两次独立阶段都付，未被reload join消除。 |
| 原其它服务 | Capture去重、Site查找、live、Stage/journal、fresh GPR/FPR/fixed/width/scratch/transport、retry及resource/Freeze不变。 |

[合入差异](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/native-transport-batch/source.patch)、[补充成本说明的receipt前后对应](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/transport-receipt-notes-provenance.json)。这是实际删掉一段重复服务，不是新profile或compile毫秒证据。fixed插入/seed、原Recolor/定义/raw/CFG、高零、profile重读/双resolve及M1/M4仍须整线减工，详见[六段账](spill-and-compile-root-cause-plan-2026-10-28.md#581-当前knownnative的净工作账)。

### 25.5 下一组完成flags服务，不再重迁kernel

当前gate保护的是真实断口：packed pseudo demand尚未写recipe；最终resource未合intrinsic demand；Save/Branch raw anchor仍可能被当Numeric；原discard/flags-only MEM、token/carry/direct-control/backedge质量尚未全部迁。Current/193 kernel/PhysicalCondition已存在，不能再列作待实现的编码器。

下一外部组共享原pseudo/access/output纯规则，保持whole-unit RA、本B emission和packed branch-only三种含义；静态图变化先RA，Map相关suffix/event在原B最后位置访问内完成，保原token retention、PF/AF和direct condition。原HasNumericUses实际用GetUses(false)总次数，不能按函数名偷改policy；Logical anchor也不自动授DISCARD。flags-only MEM另读原raw与actual backing，保持Tick/required_backing/pending顺序。

ClearCompound先只读同一入口width solution与同源terminal/carry谓词，最终metadata再用同一谓词，不增第二solve/表或proof后form编辑。有Original输入的covered整数链仍须Tick/Logical退休，不能套无值输入的exact Disabled快退；未闭合形态保typed缺域。最终全virtual资源合成packed＋intrinsic再把pseudo word复用handle。完整接收域闭合后才缩小flags/Condition粗gate；局部NotGoto/BindLabel、RSB、selected memory等仍独立，不能由flags已迁保证全部CoreMark准入。完整合同见[主方案5.3.12.7](spill-and-compile-root-cause-plan-2026-10-28.md#native-flags-next-service)、[逐文件表5.1.5](middle-pipeline-file-layout-2026-10-28.md#515-下一flags服务的文件归属)。

### 25.6 FEX对照和验证节奏保持完整请求

固定FEX仍提供早定form、同IR RA、JIT直读的设计对照，SVM仍保完整Function及异常恢复。新链已在有限显式域绕过旧完整Describe/Complete/binder；默认normal未切，不能将显式服务减工换成默认耗时改善，也不能从旧48～50%周期并集推算本版收益。

先闭joint原事务，再接flags与其余真实域，同步M1少出生/首次解释和M4触达退出；完整旧服务退出后一次整体方向，最终按冻结6.3/6.4集中质量/资源/两scope fresh FEX。中途允许质量变化，不逐helper新增QoR/PMU/FEX矩阵。**当前没有已证明可兑现≤2×的完整组合。** 本文修订仅改六份方案，冻结第1～24节、旧方案正文、最终门、当前源码/Git索引及既有receipt。[修订边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/plan-revision-05-20261101/start.json)。

<a id="c7-actual-use-def-latest"></a>

## 26. C6事务已闭合，C7必须按真实use/def接通条件消费者

2026-11-01复核。本节基于已有C6终态、主树C7/9fa79e0f和外部相依实现结论修订方案；本轮只改文档，无新构建、设备运行、PMU或FEX测量。第1～25节保原时态，不把后来的通过数或WIP回填为历史资格。

### 26.1 当前源码、最后构建和已执行范围分开

C6/2b3c7543/build-17四Android owning目标通过；min09只补受影响joint，一例2985断言全过。原Submit/Capture/width/fault/selected、真实SameReads reload正行、错误NoMaterialization独立拒绝、完整snapshot rollback/live-resource重绑、正确RequireBacking及fresh proof、资源/Freeze/Ready/退休现已闭合。旧min08 retained_rows为0是fixture未把不变压力MEM key放进requests；修正从真实RA选择有reload的压力值，两轮显式SameReads并逐字段检查，不改产品/RA/policy/压力，也不删原断言。

05bc/min08其余17阳性与三条命令仍属05bc；没有补跑成2b3c或C7整套18/18。当前共享树`9fa79e0f86453b2558f46cd5fb2aa4c2db297696747777c33a9f9d979903dd94`为C7未构建WIP，默认normal false；三批已合共享flags基础、discard/MEM接缝、部分selection/width与CMP kernel无结果读口。后续实际图/有限D/control/proof尚未整组接通，不能转借C6资格。[C6检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/c6-checkpoint-build-17.json)、[min09执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-09-joint/execution.json)、[设备关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c6-native-finalization-20261031/integration-01/minimum-semantics-09-joint/closure.json)、[C7最新合并](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/merge-03-resultless-compare-kernel/receipt.json)。无当前guest、compile/QoR/PMU/fresh FEX资格。

### 26.2 CMP和条件链暴露同一个模型问题

现[EmitSub](../source/runtime/backend/arm64/jit/translator_scalar_physical.cpp)的SubCompare只发Cmp/flags，不写数值结果；[FoldCcFromCarryTest](../source/runtime/backend/arm64/jit/translator_flags_physical.cpp)建立local_conditions并Tst/merge，不写旧TestFlags的BOOL；部分CondSet/predicate/combine也沿原local map快退。若native仍保这些Result/Numeric edges，RA会为不存在的机器写准备interval/lease/可能的spill，最终scratch也会按假输出计价。源码能证明模型不一致；尚无动态权重，不能声称它已解释历史CoreMark全部spill或compile差距。

因此必须在首次machine live/initial前兑现真实图：CMP保真实data输入与NZCV body、退数值def；AddCarry仍保实际result；matched condition链仅退原kernel没有写的BOOL输入/定义，普通物化路径不变。ConstantFlagsOnly沿原完整合法条件退data inputs/def，保Logical锚、Tick与deferred flag commit。SetCarry/Overflow实际ScalarRead(arg0)+Bfi，readmask必须为1；Invert才无data。原operand tag/operation也必须与实际ADC/CMP kernel一致，不得忽略shift/ext/Minus；不命中保合法generic。

raw SSA header/Original occurrence继续服务身份、用途次数、MEM和退休，不能冒充机器def，也不能因actual use=0就抹掉。原head在actual mask之外可直接借作Logical，只有实际换槽才保稀疏原参数。原GetUses(false)总数、numeric-only和actual machine live分别检查，不以一个“无use”标记替代三个域。精确形态与schema修正尚在外部相依组，当前合入kernel的一处无结果读口并不代表完成。

### 26.3 有限D与真实condition消费者是一组接口

原ProveDeadUses递归认可的dead arithmetic消费者也须交完整稀疏accepted closure并真退body/def；只Cover零链、让后继继续actual读被退定义会留下ghost。沿原成功路径闭合，不逐root扫N或补availability假def。预RA顺序明确为完整graph兑现→Constant批量实际闭合→有限D→剩余GPRView/Get32→首次共同machine live；所有读者尊重inactive，不先解析已退定义。此相序待组装验证，不授单次全流程live的承诺。

退def或换源时同步原read/view/consumer/hint/preference/definition需求，复用原ordinal journal批量修订与恢复；soft真实删除/失活，hard fixed/read/tie/preserved及residence合同兑现或在semantic破坏前typed分流。只改Program Result但留原质量行，会重新制造假lease或缺失真实读取。无Q快退，有Q合并一次实际Numeric/Implicit/Terminal/REF闭合，原rows×logD、排序/REF和恢复费用照计；不能复制全GPRInputs或每候选扫scope。

typed condition引用必须到真实Select/terminal，并在Freeze后保可用身份；key可能是combine而非test。它不作为GPR Numeric/Implicit root，原raw BOOL/terminal退休另保。原local_conditions消费同一key，已退数值def的值不能在查找失败时回ScalarRead。具体槽载体仍待native/Current/terminal/resource读者同组接通，不将Label/DISCARD草案写为已实施。CF polarity、host NZCV/PSTATE覆盖窗口、实际birth/location/preserved suffix须由最终reader独立重证，纯graph/header/events不是证明。

foreign sole-use LoadImm只借原稀疏recipe/空槽consumer提示定位，仍独立核原global count、完整token/type/view、scope和原RHS/立即数/CMP窗口，避免逐Q全scope找user。C6 U32 bridge与flags covered可以同shape，保原Label0/prefix协议，按原实际关系重证，不能加伪transport位当类别。

### 26.4 保正确性，减少新增的同义工作

最终flags proof接原GPR site访问，临时累积保守needs_flags并结合真实terminal/metadata决定同B一次full reader；unknown/observer纳入，不另扫全B或保存Passed表。必要entry事实只借原native解，先于EndNative，ClearCompound准备与最终metadata共用原CFG/terminal谓词。whole-unit RA、本B emission和packed branch-only分域，同源effective demand保branch/PFAF覆盖；全部virtual时先产原ResourceRecord，之后pseudo word变handle，静态/prefix不得解handle为需求。

原discard不因Save转Logical而放宽：rawtotal1/actualnumeric0另走受检flags-only MEM，保reload/preservation/required_backing和canonical寿命。固定行借原installer/FixedReady，退出新增candidate×F扫描，原F安装/核验不消失。精确ConstantFlagsOnly当前只排队C/V/AF clear，intrinsic scratch为0/0；未来observer/CF merge、真实MEM/Tick/transport在各原点计费。CMP结果less后不再虚计输出，SetCarry/OF保真实读取与原预算。

新增候选/raw闭合、原参数校验、Store归属/Definition/REF查询、finite-D rows×logD、late suffix/preserved P×D、原独立proof和资源双resolve都计净账。一个B一次调用或同槽存储不保证全链线性，也不能按外层pass减少估计毫秒。相依组合当前未构建；默认/source粗门按完整接收域闭合，不能永久缩域、关闭质量政策或保全Generic/CSET+CBZ来制造进展。

### 26.5 FEX对照仍以完整旧服务退出为单位

固定FEX先定目标形态，局部RA把实际data operands改为physical immediate，JIT直接读取；它仍有DFE/CFG、初始化、spill冷分支、真实编码和发布。借鉴的是唯一决定落到实际程序的时点，不是照抄其块内RA、每guest flush或取消SVM合同。[固定源码与计时边界复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)。

C7纠正假use/def和重复解释只是下一相依组。必须继续退出默认旧Describe/Finish、Complete同义发现/重建、每emit semantic binder，补普通hard/control/memory等域及失败恢复；M1少出生/少首次解释、M4少真实初始化/发布/结束同步推进。历史约700/800ms、495/a10 PMU和da的约47ms FEX分母不改标当前，不能由源码推演已达数量级改善或保证≤2×。

实施顺序已写入[主方案7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)，职责见[文件表5.1.5](middle-pipeline-file-layout-2026-10-28.md#515-下一flags服务的文件归属)：相依组一次owning构建＋最少受影响真实public-owner语义，完整旧服务退出后一次整体方向，稳定后集中冻结6.3/6.4。中途允许生成质量变化，不逐helper跑QoR/PMU/FEX；最终不降低生成/运行质量、资源满足且独立Block/Function各≤2×同轮fresh固定FEX才完成。**目前没有已证明可兑现两倍门的完整组合。**

<a id="c7-numeric-control-memory-latest"></a>

## 27. 数值flags已过受影响检查；控制、内存与完整compile仍待闭合

2026-11-01续审，以本次文档冻结的主树和既有receipt为准；只修六份现行文档，不启动构建、设备运行或测量。第1～26节保原文、source及失败时态，不回填后来通过数。

### 27.1 最新验证边界

当前source为`caeacb811157bfbd9c33c52d1b15ed6636d22781571e03b3a55c4363906f1db8`，2404个source/build输入；默认`kNativePreparationEnabled=false`。普通数值flags与合法递归terminal Condition按支持域放开，condition-only/Carry及普通TSO/Extend仍未整组接通。

| 当前或历史身份 | 已执行内容及严格边界 |
| --- | --- |
| 当前numeric-build-05 | 四Android owning目标generic Release/O3/NDEBUG/ThinLTO/noPGO通过，source/cache不变，保四ELF身份；不表示四套测试都已执行 |
| 当前min03/public owner | 原真实公共owner一例282断言通过，包含Block flags压力、Logical Save→MEM/真DISCARD、AddCarry/CMP/ConstantFlagsOnly、完整Function组/Condition/宽度入口、Ready true/false/重复编码和退役；未执行guest |
| 当前min03/checked | **仅原先失败17例**，2839断言通过。不是最新native18或checked22整套重跑 |
| 原4aeccf38/min01～02 | min01 native18例17过1失败，checked实际22例5过17失败；原runner误计划19例而停止，计数审计和原日志保留。min02只续此前未执行的Function width1/46756与physical164721检查/1注入；这些阳性不转授当前source |
| 控制/普通内存 | D04 carrier、semantic retired-header和JIT adapter已封外部包；selection/full control proof与普通memory receiver/runtime adapter继续外部实施。没有主树整组构建、运行或性能资格 |

证据：[C7数值检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/c7-numeric-checkpoint.json)、[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/numeric-build-05/build.json)、[执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/numeric-minimum-03-failed-only/execution.json)、[设备收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/numeric-minimum-03-failed-only/closure.json)。红魔自有namespace已关闭，无活跃进程/清理错误。没有当前CoreMark、spill统计、compile、QoR、PMU或fresh FEX结果。

### 27.2 数值组纠正了实际图，也暴露共用规则的边界

CMP真实无数值结果；ConstantFlagsOnly保Logical/Tick并退实际data；AddCarry保真实结果；SetCarry/OF保arg0读取。原ProveDeadUses认可的**完整accepted closure**已随同退出实际body/def，有限D同步原read/view/hint/preference/def行，随后才交剩余GPRView/Get32与首次machine live。保原MEM/required-backing、固定installer、实际suffix/preserved及fresh独立proof，没有借flags分类放宽DISCARD。

共享规则须保持原语义和借用期：ConsumerBuilder的workspace借用在下一IntegerFlagsBuilder使用前真正结束，Finish不代表RAII借用已结束；MatchCompare先核Sub及真实值再查宽度；checked仍借原demand owner；packed BranchOnly marker是OR，不是全部伪写均branch-only；合法scalar BOOL不限U8。canonical定义与per-use view分别从真实header/原cast验证，不能把view当定义类型。原失败及夹具API错误保留，不削弱断言求通过。

这些修正避免支持域的ghost live、假output lease与错误压力；本轮没有测CoreMark或实际spill，不能将它们说成历史回退的完整归因或性能恢复。

### 27.3 控制key、实际数据写与raw退休必须分开

采用外部已实现的窄协议：body arg0为Label-kind NativeCell，保完整reference/type/RawView，Current LocalConditionKey只读该精确槽；PhysicalIf在末尾追加默认false的local_condition，标记域的NONE表示无数值位置。原被换Value只退休一次，不伪造DISCARD，不放宽一般ValueAt/ReadPhysical/Map NONE，不建第二控制程序。原first/mask1 recipe在转换时交key，避免新增Q_control×prefix寻源。

退data不能只看Local事件。always-key源和精确Carry tuple须先闭合；Carry实际登记combine key且保Tst/merge，LocalParity保真实W temp/lease；Generic CondSet/TestZero与部分FCmp仍有真实数值fallback。继续在原最早owner闭合lazy状态与机器图，不把保守Result冒称必然硬件写，或永久多发CSET来换支持。

actual terminal仅跳过精确local If的data use；原raw If/Switch occurrence及退休仍借同一PhysicalTerminal树，dynamic/call-return/hoist不混入原Header raw总数。目前实际公共spill/ownership读者没有新增raw-terminal Access需求，先用原树纯raw模式，不建第二树或E表。

独立reader必须重核原图/REF/raw closure、真实key/位置、CF极性和NZCV窗口，含嵌套Condition的LoadNZCV与Switch比较；map命中不自证。backedge动态save_in_nzcv/nzcv_dirty/nzcv_requested不匹配须给不可达证明或真实canonical flags恢复，missing-key guard/PANIC不是闭合。integer-branch先raw改形后原位置/home准入，backedge/PFAF保原边、PF/AF及metadata唯一写者，不能晚删BOOL却继续旧live。

本轮续审另确认旧legacy raw Carry只检查Save含CF，而实际raw kernel的HI/LS同时读host C/Z；若只发布CF，guest ZF不一定来自该算术，不能替换成raw条件。控制组应让checked/native共享真实C|Z发布条件，并由最终reader核source merged demand与窗口；不满足就沿合法Packed carry，不能拒整个unit或加假ZF需求。intrinsic原分支已要求C|Z。该正确性修正仍在外部组，min03不提供其通过资格，也不属于compile收益。

### 27.4 普通memory不能靠放开Generic或coarse gate接通

下一外部组接普通TSO load/store及LoadExtend/LoadExtendTSO。真实断口是原arg1的4bit LoadExtend描述符位于native Cell3，而kernel期待PhysicalMemoryControl：同源EncodeLoadExtendControl在转换尾原槽写入真实控制字及LoadExtend form；state、flags、verify等原逻辑读者用NativeLoadExtendDescriptor逆投影，不存第二raw Imm或memory结果表。

访问字节数/sign/结果宽度/extend32清高分别保持；普通六个memory meta带MayFault，公共FlagAccess因此read All，不能硬核flags_read=None或删fault observer来接收。guest flags观察与host NZCV clobber分域。原TSO/RCpc、unaligned、barrier、fault点、backing/owned-reload与plain Extend forwarding照走；TSO load无地址候选、TSO store仅真实value候选，不顺带打开fused/复杂Map/EA/direct-narrow。全部读者与最终resource同组迁移才缩粗门。

### 27.5 FEX对照与compile目标仍约束整条工作量

固定FEX的可借鉴处仍是早定target形态、RA在同一IR交physical operands、JIT直接读取，并复用线程级compiler/arena；它仍付DFE/CFG、初始化、压力冷分支、真实发码和发布。其块内RA不代替SVM Function，约47ms历史CompileBlock包含范围与SVM完整Translate端点差异仍保留。[固定源码与计时复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)。

数值正确、少表、同槽和一次外层访问均不证明数量级收益。正常旧Describe/Finish、Complete同义发现与每emit binder尚未从默认链退出；M1少出生/少首次解释与M4实际初始化/发布/结束仍必须并行推进。finite-D原行×logD、raw/REF/归属查询、每候选窗口/CFG、fixed插入/seed、Recolor乘积、preservation×actions、profile重读与资源双resolve全部计净账。一个B一次proof调用不代表整链O(N+E)，不新增N表也不代表减少了这些访问。

执行按[主方案7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)：控制相依组一起接通，普通内存并行，其余真实域/恢复与默认旧工作退出继续完成；文件归属见[5.1.5](middle-pipeline-file-layout-2026-10-28.md#515-下一flags服务的文件归属)、[5.1.6](middle-pipeline-file-layout-2026-10-28.md#516-普通内存接收域的文件归属)。中途只成组构建和最少受影响正确性，完整算法/旧服务替代后一次整体方向，稳定后集中6.3/6.4；不逐helper跑QoR/PMU/FEX。**最终不降生成/运行质量、资源满足且Block/Function各≤2×同轮fresh固定FEX的目标不变，当前没有已证明达标的完整组合。**

<a id="c7-control-memory-join-latest"></a>

## 28. 控制与普通内存已获限定检查；本地合流是默认替代前的实质缺口

2026-11-01按主树`3bdd060cbfabee0f1b087b5b11f28d727ece6605e5c44edb348f0b143355ee17`、原build/test receipts、原JIT终结代码及外部组件边界复核；本次仅改文档，无新源码接线、构建、设备或性能测量。第1～27节保持原身份，下列纠正不倒填其当时结论。

### 28.1 最新已验证范围，不合并旧阳性

| 身份 | 已执行与限制 |
| --- | --- |
| 3bdd/control-memory-build-02 | 四Android owning目标generic Release/O3/NDEBUG/ThinLTO/noPGO通过，source/cache不变，ELF保hash；不是四套测试全跑。 |
| 3bdd/minimum-02 | 原public owner **1 case/492断言**，原checked Switch **1 case/230断言**通过；前者沿原Block/压力/完整双Function、typed key/If、C|Z raw/CF-only Packed和六普通内存族到resources/Ready/重发/退休。未执行native guest/CoreMark或量compile。 |
| 13888/build-01/minimum-01 | 四目标通过；owner **1 case/398断言，396过2失败**，三个checked **3 cases/413断言**通过。失败为原RawView预期与fixture未开Carry能力，后修保真实view/FeatureSet；原失败、413阳性和两次namespace关闭分别保留，不折算为3bdd整套通过。 |
| 外部lazy封包 | `flags-lazy-condition`14个既有文件，patch SHA `0aba4313959717b9eaefbc748e9e4fc1cfbe3b8c0bda89152ef3d9a6ab96661c`，未集成/构建/设备验证。 |
| 外部token/metadata | token为草稿；conditional byte/offset、numeric Boolean源合同及root state/width/flags join仍未闭合。metadata02的原matcher复用已施工，实际A/B端点及C ZeroTarget尚未封包，不授其Ready或全域资格。 |

当前证据：[四目标](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/control-memory-build-02/build.json)、[两例执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/control-memory-minimum-02/execution.json)、[收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/control-memory-minimum-02/closure.json)；[原失败/413阳性](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/control-memory-minimum-01/execution.json)、[lazy说明](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/flags-lazy-condition/interface-and-net-work.md)。默认`kNativePreparationEnabled=false`，没有新版compile或fresh FEX资格。

### 28.2 机器写入合同比Local事件、form和map更强

主树已将always-key/完整Carry真正接到Label-kind body参数和PhysicalIf.local_condition；actual不计虚假data use，原raw occurrence仍在同一程序/terminal树退休。普通TSO/Extend已完成Cell3的实际控制字、全部逻辑逆投影、MayFault read All、资源和原TSO/fault路径；此前“外部待集成”不再是当前状态。复杂selected/fused/跨类及完整guest运行仍另证。

剩余CondSet不能只让RA保一个保守Result而让kernel按map跳过写入。外部新形态CondSetControl只取原Last＋唯一安全terminal、无真实data消费者的域：已发同ID raw key优先，否则真Prepare/Merge/Flush/Load；保原scratch且无data Result。同一外部包中的native Generic CondSet/TestZero/TestNotZero每条lazy分支实际写Result；hard撤回Generic而同ID已有独立证明的raw key时，真CSET该condition，不能读尚未发表的x26。主树3bdd仍有Last/Pending Record后早返，不能将这项修正记为已集成。numeric Select/Goto/If读真实data，只有显式key才能查map。Begin/End的单一native mode与清旧key只管理真实消费范围，不是来源证明，不另建key map/证书。

原全组hard访问可在转换前撤回精确可选Last形态为Generic；借实际D个row指针与原sorted窗口，不再每B重扫F，也不删hard义务。该可逆性依赖没有后续body Label、两种形态都不保证保持NZCV，故不改变先前窗口/Carry；不能推广为任意late换图。主树whole-group hard扫描已接，lazy可选撤回本身仍属外部包。

**Switch修正原第27节笼统的clobber判断。** 原`translator_terminal.cpp`先MRS/保存实际NZCV，再CMP各case；每个命中子树在递归前LDR/MSR恢复保存值。因此Switch继承父安全性，不能无条件拒合法raw key；已有祖先Condition reload屏障仍保留。原Switch fixture预期正确，修改的是shared matcher/final reader，非削弱测试。外部Select保持谓词也须看真实路径：Direct可MSR、numeric Boolean可CMP，只有精确explicit local-key路径可按其actual kernel宣称保持。

### 28.3 本地Goto/BindLabel不是删除一条粗门

现Goto/Bind token、GPR逆向路径草稿不能代替三类实际合流：state copy/home/byte交集、Function宽度Meet、flags pending/CF/PF/AF/token/NZCV。单source unary WidthTransfer和线性flag_state在分支跳过更新后均不足以给出全路径事实；原逻辑FlagFlow合法也不自动证明physical捕获/位置或JIT pending一致。

原owner须同组交付：token仅metadata而无假def/REF，Goto条件原Value照常raw退休，Bind身份边另核计数/唯一绑定而非data/MEM退休；actual GPR在Bind拆taken/fallthrough，taken含Goto本点prefix，MEM/capture须真实支配；state不在join伪发表硬件写；Get宽度取Get定义点而非晚Set时当前home；真实label才准备稀疏多源摘要并交原同一Function solver；flags owner在真实入边定兼容或canonicalize/join、预算effects/scratch并使非共同key失效。无control scope保原GPR查询快路，无本地Goto的B在initial/local width保快路；控制Function内跨CFG query仍可能访问有Goto前驱，不能按当前B无Goto免除路径费。不建完整CFG/width/flags新图。窄numeric BooleanBranch省normalizer时还须重证原HasBooleanResult/materialization，不能凭form放行。

完整方案见[5.3.12.9](spill-and-compile-root-cause-plan-2026-10-28.md#native-local-control-service)，逐源见[5.1.7](middle-pipeline-file-layout-2026-10-28.md#native-local-control-layout)。这些是待实现合同，不是本次代码已兑现或单元测试已覆盖的结论。

### 28.4 branch/backedge/PFAF须交实际端点，ZeroTarget最多一次原scope重建

窄branch-only SubDeadNarrow8/16仍需UXTB/UXTH和真实CMP；省SSA Result后真实临时值归原scratch。普通SubCompare直接W读且CF需求不同，不能强套。Backedge省略VOID StoreUniform/Save/Clear/Invert仍按原Value occurrence保Covered＋Tick；PFAF/Backedge的tag、PF/AF、requested/dirty/polarity、ABI/XPOOL和原metadata输出同源兑现；HostGPR重读沿原真实copy/CFG/home窗口与numeric clobber/preservation核验，不由GuestWrite mask或新增home证书自签。原non-S mismatch fallback若保NZCV，可沿实际已证明key继续分支，不为“恢复”强制MSR覆盖raw条件；guard或map存在仍不能授proof。

ZeroTarget的真实SSA源必须在首次live前进入同一PhysicalIf.cond；只在真换源时稀疏保原token/view/sense，raw与actual根分开。late原fixed-home/preserved条件失败，不能用“SSA可读”冒充原质量命中，也不能直接晚换回operand。原Storage/Writer须回滚当前allocation/late edits，移回唯一PreparedInputs，恢复原body/soft/terminal cohort，fresh live/`DescribeNativeAllocationInputs`/原controller最多整scope再跑一次，不重进semantic Describe/Finish/Complete；第二次不重选同一失败可选项。新cohort预算与原FPR retry分别记录，Function不拆Block，禁止Inst回退或另建恢复manager。此owner窄口尚未实现。

### 28.5 FEX启发仍以整条服务退出为采用单位

固定FEX在同一target IR早定形态，RA写物理operand供JIT直接读取，同时仍承担实际DFE/CFG、每unit初始化和压力冷分支；它的局部RA不代替SVM Function。新控制/内存合同使显式新链能继续接收普通输入，并不证明默认旧Describe/Complete/binder已退出，更不产生数量级收益。M1真实少出生/少首次解释、M4初始化/发布/结束必须同步减工，不继续把全部提速押在flags、布局或最终proof索引上。

净账新增/保留：D排序与原hard查询、raw/REF/窗口/CFG、Generic实际物化与原scratch、已有key清理、控制B初始/最终N_B＋L访问、每query路径工作表（草稿约4×B/16×B bytes seen backing，另有L hash与frontier）及其分配/初始化，按服务扣除旧1×B/4×B或原本无需seen的实际成本、稀疏width Meet与flags join、partial/高零证明和一次失败完整N/E＋RA。共享规则、一B一次入口、没有N表都不证明总线性。尚无周期/毫秒分解，不预填可省预算或用历史47ms/700ms推当前倍数。

下一顺序为精确lazy形态、并行本地合流/实际metadata端点、原owner一次cohort重建、其余支持域、默认替代与旧服务删除；M1/M4并行。每个相依组统一owning构建＋最少受影响检查，完整服务退出后一次方向，稳定后集中原6.3/6.4。中间允许生成质量变化；最终不降质量、资源满足，Block/Function各≤2×同轮fresh固定FEX目标不变，**目前仍未达成**。

<a id="c7-lazy-state-implementation"></a>

## 29. CondSet相依组已集成；状态合流仍是未验证实现

主树39522227四Android目标通过，原public owner一例602断言通过；85fb原owner525断言中的一次失败来自BOOL U8视图与U32 uniform槽不匹配，后修只将数值store改为canonical Value，terminal仍U8。85fb的两个checked用例235断言保原身份。原namespace均关闭，源码/ELF/失败记录见[本轮检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/c7-lazy-checkpoint.json)。第1～28节保原时态，不回填通过。

CondSetControl、Generic实际写入、raw key优先CSET、显式numeric/key读口、原hard转换前撤回和Select实际NZCV谓词已集成；602只覆盖原owner所列接收域，不授权尚未完成的branch/backedge/PFAF/raw-key metadata或本地控制。默认normal仍false，无新guest/CoreMark/compile/QoR/PMU/fresh FEX结果。

外部state/width五文件已写实际Get点快照、真实Goto才提升的多源Meet、同一Function solver、Bind的copy/宽度路径交集，以及独立枚举前向路径的oracle。三个owning TU仅Android语法检查通过，未集成/链接/执行。原width测试刻意锁定publication时重读home的实现行为，但它不是SSA语义：原Get kernel在定义点复制，后写home前有真实preservation，Set必须读取oldGet值。该实现与oracle须共同修正，旧源/注释保留；不能只改单侧expected求过。foreign Get64的精确跨块宽度来源、MEM birth reload实际home写及完整路径状态仍待闭合，不授最终Function质量。

flags复核另发现producer在Goto前而legacy Save只在fallthrough时，原pseudo demand可能把架构commit提前；FlagFlow/SSA Finish没有完成物理规范化。仅Bind清token或canonicalize不能还原taken旧flags，需原flags owner保持真实Save发表原点及其输入/捕获/effects/scratch，在首次live前交付。intrinsic producer发表须与此分开。该工作仍外部施工，不以永久Unsupported或删除原flags政策代替。

完整目标不变：local control/metadata/一次cohort重建及其余支持域闭合，默认旧服务真实退出，M1/M4同步减工后做整体方向；最终质量/资源与两scope各≤2×同轮fresh固定FEX仍未完成。

<a id="c7-owner-rebuild-latest"></a>

## 30. 可恢复基线、原owner寿命与真实写入：最新方案纠正

2026-11-01只读复核当前39522227源码、原receipts及外部WIP，并修订六份方案；未集成外部组件、构建或运行设备。第1～29节保原时态。当前四Android目标与owner一例602断言仍归395，85fb的原失败和checked两例235断言保原身份；没有新增guest/CoreMark/compile/QoR/PMU/fresh FEX结论。

### 30.1 可选形态必须在实际可逆边界修改

此前方案只说“预RA编辑、late失败恢复”，没有确定journal记录哪一种合法程序。NativeVirtualCheckpoint保存Extra尺寸，Restore不能把已删Result/body重新长出来；若preclose先覆盖ScalarRecipe，唯一转换已经丢失general基线，后续static journal无法恢复它。新相序因此明确为：preclose只选原Block metadata、原全组hardguard借真实D；唯一Native转换先生成合法flags/general；之后Apply在Constant/有限D/首次live前稀疏修改真实Native body，保旧Extra prefix、只append新行。

A/B外部已写Apply/Withdraw；精确MetadataCohortRejected的最终分类仍施工，C ZeroTarget实际换源尚未实现。integer_branch Covered CondSet由原ImportIntegerBranchCondition导入，不等于一般LocalConditionOutput；原header、同B metadata.condition完整身份及event word须保真。有限D checkpoint含原numeric/Constant，恢复输入行后须对最终general body重新执行原PrepareNativeFlagsInputs，不能只恢复metadata而沿用未过滤行。详见[主方案5.3.12.10](spill-and-compile-root-cause-plan-2026-10-28.md#native-cohort-owner-rebuild)。

### 30.2 原FPR重试存在真实寿命断点，不能用新wrapper掩盖

当前native失败路径在[compiler.cpp](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/compiler.cpp:1487)恢复late edits后调用AbortTargetPlanning；其经[plan_build.cpp](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/compile/plan_build.cpp:403)调用AbortLocationEditing，销毁allocation_state，而紧接的[RetryWithoutFPRCoalescing](/Volumes/Data2/SwiftVM/source/runtime/compile/allocate/allocation.cpp:369)要求该owner及inputs仍存在。这是源码可确认的失败路径断点，602限定成功用例不能给它通过资格。

新合同在原Writer/PlanBuilder上区分“结束当前位置编辑、保inputs供重试”和“最终abort销毁”。外部八文件已写suspend、CancelProgramTargetPlanningForRetry、TakeProgramState/RebuildProgram及原测试夹具增量；尚未集成/构建/执行，Storage的实际metadata恢复消费者仍未接通。取回的是同一唯一AllocationState，Inputs的const身份不赋值/复制；先在旧cohort有效时清Map/soft birth，再逆序恢复body/输入/terminal，清派生列和callback、fresh live/native Describe，原controller最多整scope再一次。CallReturn preference不可重复生成；原FPR禁用和累计次数不因Begin/SetUnit重置。

只有可选late PSTATE/PFAF/home/window拒绝可使用新预算，Invalid/Unsupported/坏callback/transport/scratch等不能兜底；原FPR预算另记。A/B全撤回后canonical backedge恢复false，旧unit_state_edges由原CompleteNativeEntryState重接，保原requires_unit_state基准和FunctionEntryContract，不重读已死Inst。未来C若改变entry基准须另闭合。整批general能限制组合搜索，但可能丢失其他成功优化，因此不等于最终质量无损；6.3仍适用，不能永久关闭政策。

### 30.3 Save原点与合流不能靠清key补救

FlagFlow/SSA Finish只解决逻辑需求。producer在Goto前、legacy Save只在fallthrough时，把Save demand提前commit会改变taken的旧架构flags，AF也不能在Bind清状态后重建。intrinsic FlagOutputs仍属producer，legacy延迟部分须在原Save点执行：外部SaveResult以原head保Logical raw退休，以实际result/必要原operands计算flags；ADC/SBB用result和原左右操作数恢复carry/borrow，不读Save时当前CF，逻辑CV为实际常量0。

原ScalarRecipe/NativeExtra只稀疏记录emission demand override，raw packed需求不变；真实retains、FlagsResult目的地、scratch、Commit和资源共用effective读口，handle不解作需求。原resultless/Covered匹配必须保住Save真实要读的值。原L label owner存Goto实际flags表示及当点scratch，Bind相容直连、异构Merge/Flush并保合法raw NZCV；原唯一key map的birth只过滤fallthrough-only来源，不自证跨join。此组已有外部代码，RawPstate前向Meet、完整schema/scratch及独立最终reader仍未闭合，无构建/执行资格。

### 30.4 实际writer的正证与scratch可能覆盖是两个问题

外部纯规则按真实definition/site/birth、form及effective demand区分Value、Temporary、DynamicScratch和None。同home Get64/部分Get32可能无MOV；MEM birth可能写reload.reg；仅MEM输入的reload才实际发LDR，GPR＋reload直接读；Or/Xor走普通目的地，legacy Add/Neg/ADC等可能走FlagsResult。不得用MEM slot、header type或guest写mask推实际home。

token/实际writer组件随后冻结11个既有文件，补丁SHA `2a76752e00f2cd116ae3ec19407afae22ee0271287afde499dd4f5c29d6bc34e`；[外部回执](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-control-token/component-receipt.json)明确尚未集成/语法编译/构建/执行。其before基于较早共享树，须按hunk保当前lazy、内存和metadata增量，不能整文件覆盖；封包不是整条控制服务交付。

共享原InstructionScratchGPRMask保持dirty→forwarded→fixed→仅10/12/13 lease的优先级。未知dynamic可能home只用于窗口/byte/高零负向排除，不能被PhysicalGPRWrite行用来自证真实early-write；必须借prepared原UnitScratch，缺失明确失败，不能把零值当无覆盖。最终GPR proof、state/width及JitContext原实际执行必须共同接读口；只完成一个helper不闭合整链。

Bind先拆路径，再处理各自prefix；taken仍含Goto当点prefix，Bind前prefix仅属fallthrough。partial BFI/UBFX按同源ByteOf逐byte核offset及清高，原MEM/capture/定义实际支配仍独立成立。BooleanBranch还须原Boolean分类、真实primary/view/birth及数据物化，local key/temp不冒充0/1值。原GPR site访问后对相关B一次独立token核验仍由主线程接线，不能以接口存在当通过。

### 30.5 Function宽度必须保Getter定义点及跨块来源

外部五文件的local多源Meet、实际Get点快照及独立枚举路径oracle只获三TU语法检查；foreign Getter当前unknown是未闭合项，不能成为永久质量退让。方案要求只为实际发表依赖的Q_get保来源B/Get点表达式，在原唯一Function solver接真实稀疏依赖；即使来源B稍后覆盖home、exit宽度未变，Get事实变化也须唤醒消费者。不建N历史/第二CFG，也不在每轮Φ重放IR。

Getter的SSA值事实与physical home是否真W写分别证明；不能由窄视图推高零，也不能让新的摘要算法自己生成oracle expected。foreign来源、真实writer接线和运行均尚未完成，当前不授最终Function质量。

### 30.6 FEX对照仍要求净减工与完整反馈

固定FEX的同一target IR→原RA写物理operand→JIT直接读，以及线程级compiler/arena复用仍是采用依据；它仍付DFE/CFG、压力冷路和发布，不替代SVM Function义务。[固定源码复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)未新增计时。以上修正使替代路线可正确执行、失败可收口，尚未证明完整旧服务退出或数量级收益。

净账须补真实D journal（最坏D=N）、Q_get摘要/依赖、L flags快照和真实Save新增读/live压力。token路径当前home/高零seen为4×B bytes/次，单byte为32×B bytes/次、每word最多8次query，另L hash/frontier；第28节16×B是旧草稿，不再引用为当前预算。无control scope保原路径，但实际writer/归属查询仍有增量。初/末N_B＋L、Q×B初始化、原Q×D/Q×E及窗口/REF/资源双resolve和一次失败完整N/E/P＋RA全部计入，不能称整链线性或用局部表变少推收益。

本轮将这些责任放回原select/native/state/physical/verify、Allocation/Writer/Storage和JIT文件，不新建cohort registry、retry manager或certificate层；逐文件见[5.1.7](middle-pipeline-file-layout-2026-10-28.md#native-local-control-layout)，执行见[主方案7.2](spill-and-compile-root-cause-plan-2026-10-28.md#72-下一步执行表)。M1真实少出生/少首次解释、M4真实触达和结束同步减工，默认旧Describe/Finish、Complete同义发现及每emit semantic binder退出后再做完整方向。相依组最少检查，稳定后集中原6.3/6.4；最终质量/资源及Block、Function各≤2×同轮fresh固定FEX目标不变，当前未达成。

本轮[修订边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-05-owner-20261101/start.json)记录只改六文档；产品源码、Git索引、既有receipt、原复核1～29、旧方案正文和冻结最终门保持不变。

<a id="c7-integrated-control-latest"></a>

## 31. 控制相依组已整合：真实use、唯一owner与净减工约束

2026-11-01根据主树及固定FEX流程更新六份方案，不改产品源码、不构建或上机；第1～30节保原时态。**当前源码32222313已整合token/实际writer、state/width、Save/fixed、A/B/C metadata与同owner恢复，整组构建尚未通过。** build-01～04均失败，当前04是select_control.cpp的backedge成员引用错误；源码前后不变、无本组保存的成功ELF。最后成功395的四目标/owner602、85fb原失败/checked235均保原身份。[当前失败记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/control-join-build-04/build.json)不被接口已合入或旧阳性替代。

### 31.1 方案与实际消费者对齐

| 本轮结论 | 方案中的具体责任 |
| --- | --- |
| actual use变化会改变原fixed合法性 | SaveResult延长result/原operands，ZeroTarget改变terminal源。首次actual live后、原owner开放前，由native Describe重准入fixed，检查真实definition、terminal/REF及新增窗口；只改end不够。Origin由原installer明确标Derived，Explicit未闭合capture域不得默删。 |
| 同home不代表真写，同逻辑值不代表真实copy存在 | actual writer区分None/Value/Temporary/DynamicScratch/MayWriteValue；可能写只作负向clobber，不授W高零/byte种子。state借真实resources/config/prepared UnitScratch，在Goto实际prefix后保存活跃copy，Bind核路径交集；logical byte相等不能自证taken home。 |
| Getter语义与home状态分别维护 | Get在定义点捕获，local内联transfer，foreign只保实际Q_get依赖，接原唯一Function队列；即使来源exit不变也传播Get变化。独立oracle枚举具体路径，不能借同摘要生成expected。实现已整合待执行，不再写foreign尚未实现。 |
| 冷恢复必须重装实际合同 | 原AllocationState始终唯一；旧关系有效时清Map/soft birth，再清fixed声明/ID/publication/ready及索引。fixed_rows→Get32→flags_rows→逆body→Withdraw后，fresh static/live/Describe/actual fixed，接原RunAttempts；CallReturn行唯一，冷preferences canonical恢复/预留容量，原FPR预算不重置。 |

ZeroTarget已实际改同一PhysicalIf.cond，原raw与actual根分开；转换后Apply/Withdraw与late MetadataCohortRejected已有Storage消费者。它们不是另一套IR，也不授全域运行资格。当前仍有Explicit非preserved的foreign/REF/新破坏性suffix/第三residence、selected memory、RSB/hoist及used FCmp/Sse42等缺域；source gate仍拒CallReturn等，存在affinity恢复代码不表示已开放准入。完整合同在[主方案5.3.12.9～10](spill-and-compile-root-cause-plan-2026-10-28.md#native-local-control-service)，维护落位在[结构表5.1.7](middle-pipeline-file-layout-2026-10-28.md#native-local-control-layout)。

### 31.2 新增正确性工作不能被记成编译收益

必须扣账的新增费用是：F行actual fixed比较、受影响D_f排序/definitions/terminal/REF/窗口及最坏D_f×F移动；Σ_Goto activeD的copy快照/排序/交集及L flags状态；publication扫描、逐发表追BitCast/ZeroExtend链、去重前候选排序，以及distinct Q_get摘要/依赖/原队列Φ；每query的seen初始化、prepared profile和真实位置解析；SaveResult额外live/Params/scratch及可能spill；D级可逆journal和一次失败完整N/E/P＋RA。PhysicalIf的inline optional原条件payload还增大所有同类节点，不能标作只随D增长。

普通无控制/无cohort请求继续在原边界结束短寿命状态，正常成功不重建scope。精确metadata冷失败只允许一次general重建，不能变成逐候选搜索，Invalid/Unsupported/必要transport/scratch失败不兜底。保最终独立proof；下一收敛是在原Describe/Writer/state真实访问中交付事实、删除同义解释和无人消费存储，避免再养一层输入/动作/证书。不重开已否决的最终proof索引或单族移位微调，也不把少文件、少表或规则共享推成净收益。见[主方案5.8](spill-and-compile-root-cause-plan-2026-10-28.md#58-正常路径的工作量表与退出标准)。

### 31.3 FEX参照与后续实施顺序

[固定FEX源码复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)的事实仍是：先decode，再由dispatcher/IREmitter按已知目标建形态；DFE/CFG和压力分支真实运行；局部RA写物理operand，JIT直接读；线程级compiler/arena复用。SVM应减少构造及inputs→state/selection→最终交付的重复工作，同时保自己的Function跨块、fault/observer和独立合同。输出直连已做，不能再次认领；FEX局部RA、每guest flush及timer前初始化也不授权缩scope、降质量或挪成本。

下一顺序是：修当前构建断点→统一owning构建及最少相依语义→直接执行少量新链输出→补真实hard/普通缺域→默认替代并删除旧Describe/Finish、Complete同义发现和每emit binder；M1少出生/少首次解释、M4少初始化/复制/结束同步推进。新的ADC/SBB实际执行SECTION已在[外部封包](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-flags-runtime-check/component-receipt.json)，未合入/构建/运行，预期执行数不是结果。

完整旧服务退出后一次整体方向，稳定后集中原6.3/6.4；中途允许生成质量变化，不逐helper跑QoR/PMU/FEX。**当前没有新guest/CoreMark/compile/QoR/PMU/fresh FEX结果，最终不降质量且Block、完整Function各≤2×同轮fresh固定FEX仍未达成，也没有已证明可兑现该门的完整组合。** 历史47ms、700/800ms与495/a10采样均不改标当前。

[本轮文档边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-06-integrated-control-20261101/start.json)保共享树322、索引、既有receipt、复核1～30、旧正文及冻结最终门；本轮只是使计划、文件责任与实际代码/资格一致。

<a id="c7-scratch-location-latest"></a>

## 32. 从末次证明直接交消费者：已退一轮profile，默认整链仍待替代

2026-11-01核对当前源码、原build10/minimum05及三项只读复核后更新六份方案。本轮不改产品、不构建/上机；第1～31节保原时态。**当前53f747ad未构建；b6fd四Android目标成功，红魔9条独立原用例8过/55860断言，public owner 994断言中993过、1失败。** 53f包括真实静态ABI正例和flags reader的畸形发表逻辑偏移拒绝，不只是test改动；不继承b6fd任何结果。准确receipt与失败正文见[主方案7.1](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)。

### 32.1 保独立合同，交真正被消费的结果

merge56已删除资源循环逐body的DescribeNativeScratchProfile，而非只合并外层visit。GPR/FPR/transport全通过，Writer核原program/live/inputs后授权末次独立scratch检查终态写入原seed backing；后续唯一move给资源循环，直接Decode/BuildNativeCurrent/Prepare。control/location先核、profile全部完成后才存prepared handle，Freeze实际cells在所有虚拟读者结束之后。`final_scratch_started`从授权起阻止metadata/FPR重试，包括clear前或部分输出失败；冷拒绝只检查不输出，保原seed。

这是FEX“上游决定在真实写口成立，后续直接消费”的一个具体落实，不是借FEX之名取消核验。initial/state/final profile、实际位置/materialization/raw/fault仍必要；相邻压缩新增约N比较、S≤N行追加、N−S次端点更新及顺序读取，grow/搬移/重叠与剩余Prepare/Freeze resolve均计成本。不能写“只算一次profile”，也没有本版毫秒或收益资格。合同见[5.3.12.11](spill-and-compile-root-cause-plan-2026-10-28.md#native-final-scratch-handoff)。

### 32.2 原读点、原窗口和真实机器资源须分开

| 最新结论 | 方案的修正 |
| --- | --- |
| SetLocation/CallReturn/RSB已接限定域 | 原B动态location/resume root交真实terminal点，保原id/token/view/raw计数；machine live/MEM/backing/Freeze照实际读点。Block原发表与Function可选延迟分开；动态Push按原Covered、Pop真正动作归terminal，不冒称新增执行能力。缺域/畸形/晚form失配准确typed分类，晚home失配不能借metadata一次重建。 |
| 固定值可有两硬home＋普通captured tail | 原transfer/fixed_end、真实capture、Map allocation_begin各自保义；alias早capture与root晚capture可同时必要，Map lease提前不移动root动作。同步原索引、recolor、action/scratch/transport和独立proof，不能只延end。外部包仍未合/未构建。 |
| dense字段不是稀疏费用 | 三u32方案预期FixedGPRDefinition36→48、GPRPlacement64→76B，实际布局待构建。按N高水位付初始化/清理/保留，F/D journal、D×N_B真实窗口、REF及每capture寻源另计。宽backing、无post-body点、root已死的REF保typed residual。 |
| 新native形态改变了逻辑读取协议 | PublicationValue/Copy控制字不是byte offset；共享纯投影、各核shape/home/window。point0 capture合法，原width run原位替换，不能用unsigned−1、重叠行或插Nop避开。 |
| 可选质量命中必须有真实配置前提 | 私有配置缺完整静态guest bank时，原policy合法不给scalar affinity；Get32 capture后WholeValue也可换home。ZeroTarget“仍可读”不等于“保指定home”。53f以真实bank描述符修正正例，保私有capture回退，不放宽守卫；重跑结果仍待取得。 |

控制支持域见[5.3.12.12](spill-and-compile-root-cause-plan-2026-10-28.md#native-control-location-domain)，fixed扩域与残余见[5.3.12.13](spill-and-compile-root-cause-plan-2026-10-28.md#native-fixed-capture-contract)，文件责任见[结构5.1.8](middle-pipeline-file-layout-2026-10-28.md#native-final-resource-layout)。两个控制SECTION仅prepare/reemit/退休；ADC/SBB实际Runtime.Run属于另一个SECTION，public owner总例仍红，不能把编码、预设16次或失败前缀拆成独立guest通过资格。

### 32.3 compile上限仍由完整净工作决定

[固定FEX源码复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)仍支持早定目标形态、DFE/CFG必要求解、局部RA同IR物理化、JIT直接读及线程arena复用；不能借其局部RA缩SVM Function，借每guest flush降质量，或把timer前初始化移出SVM服务账。输出格式/typed kernel已完成，不是后续可重复认领的大项。

接下来收口真实失败及fixed消费者组、普通缺域，完成默认替代并实际删除旧Describe/Finish、语义RA投影、Complete同义发现、每emit binder和无人消费初始化。M1广泛少出生/少首次解释、M4真实初始化/结束仍为主线；scratch交接仅是已退出的一轮，剩余state/形态/resource的重复纯解码在原访问收敛，不另养manager、证明缓存或完整operand影子。**支持更多op、整组能链接或少一张表，都不能证明≤2×上限。**

相依组只做一次必要owning构建和最少受影响语义；完整默认服务退出后一次整包方向，稳定后集中冻结6.3/6.4。中途允许生成质量变化，不逐helper铺QoR/PMU/FEX。没有新完整compile/CoreMark/QoR/PMU/fresh FEX，最终不降质量且独立Block、完整Function各≤2×同轮fresh固定FEX继续开放；历史47ms/700/800ms不改标当前。

[本轮文档边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-07-scratch-control-20261101/start.json)固定source53f、索引、原receipt、复核1～31、旧正文与最终门。原实现目标仍在进行，文档修订不产生默认启用、整体质量或性能资格。

<a id="c7-fixed-storage-latest"></a>

## 33. fixed扩域与稀疏承载已验证有限路径，默认完整compile仍未替代

2026-11-01依据当前主树、原build14～16/minimum receipts和只读fixed/memory复核更新六份方案。本次仅改文档；起点前在运行的build16已结束，未新启动构建或设备运行。第1～32节保原文与当时身份，特别是第32节的“外部fixed未合/53f未构建”不再作为现行状态。

### 33.1 当前构建与成组阳性不能互相转授

| 身份 | 真实资格 |
| --- | --- |
| 2266add5 / build16 | 四Android generic Release/O3/NDEBUG/ThinLTO/noPGO目标成功、source前后不变。较7bcb仅加source阶段integer-branch候选诊断；未该版设备执行，默认gate仍false。临时普通路径诊断须在性能点/默认启用前移除。 |
| c984ba2a / build14 / sparse-fixed-minimum-01 | 12条独立原用例中11过/56167断言；public owner另998条中997过、1失败。11阳性包括fixed227、stable45、rollback35和原controller/state/joint/宽度/checked检查；失败前缀不进入阳性。 |
| 7bcbf655 / build15 / owner-only minimum07 | 只重跑原owner仍998/997/1；PC日志将policy0定位为f3d0私有point0合法回退，未解释f3c0正例。原namespace closed、无活跃进程或清理错误。 |

证据：[build16](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/control-join-build-16/build.json)、[12条执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/sparse-fixed-minimum-01/execution.json)、[成组收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/sparse-fixed-minimum-01/closure.json)、[owner最新失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/control-join-minimum-07-owner-only/execution.json)与[PC日志](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/control-join-minimum-07-owner-only/native-public-owner.stderr)。无新版完整compile、CoreMark/QoR、PMU或fresh FEX，不能改标历史47ms与700/800ms。

### 33.2 已完成的fixed结构不再列未来提速，真实残余继续明确

Explicit实际use扩域已经沿原installer/indices/Writer/interval/recolor/action/scratch/transport/独立proof闭合，capture/fixed_end/allocation_begin分开保原两硬home、真实保存点和普通lease。alias在C、root在P>C的两个动作都可必要；canonical backing lease提前不等于移动P。actual publication源按最终argument/root/width和逻辑offset核对，不拿控制字当byte offset。

c984的fixed227为GPR/U64、本地三段residence正负例与完整Function双capture，不证明U32/MEM。早先7816 fixture跳过actual publication realize的UnsupportedForm失败保原版本；修fixture后通过不放宽产品proof。无after-body capture点、窄shared-write成员不能建立宽backing、REF capture超root真实寿命的三类残余仍typed拒绝。[partial MEM压力增量](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-fixed-partial-mem-followup-7bcbf655-20261101/component-receipt.json)仅外部冻结/机械检查，保默认eviction、真实pressure/backing/两capture与唯一fresh proof，未合未跑。

稀疏结果也已落地：原Allocation保32B普通placement/u32 slot、32行stable页和唯一active roster；原Writer/transaction改可变结果。K为Release前曾获slot的distinct IDs，含withdraw/rollback墓碑，不等于当前F；跨epoch/切inputs/冷恢复/FPR retry保slot，Finish后保emit/reemit读者，仅Release重置。页仍受原128KiB aggregate idle预算全额约束。

[净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-sparse-fixed-storage-20261101/net-work.md)必须计`32N + 1536P_h`、指针/heap、页尾初始化、grow/额外indirection、F输入/索引与D_f undo。退出的是普通N内嵌默认构造/epoch复制、普通D full-fixed snapshot及同请求换inputs后N重初始化；N placement/Freeze和全部真实证明仍在。相同capacity C的leading差额`44C−1536P_h`还未扣新开销，dense fixed可更大。stable45/rollback35通过不授净时间，76→32不是完整成本结论。

### 33.3 撤回错误归因，保留原质量与独立合同

f3c0正例和f3d0私有回退复用source0/point3/target22；此前未带PC的`policy=0`被错归到正例。当前日志只证明f3d0回退前提，不证明f3c0失去affinity。f3c0未打印receiver失败同样兼容receiver成功后别处触发一次metadata恢复，不能反推未进入。

只读代码复核还确认dead-edge proof是普通bool，不因Facts/revision自动失效；最小原图匹配字段读实际Inst，空窗口和无Save的本例不能仅靠前置ScalarRecipe解释撤回。暂未有根因证据支持修改numeric-output、原pin2/coalesce、指定home三点或publication/window守卫。下一步用真实候选/receiver/Withdraw身份定位，够定位即删临时普通路径诊断，不增加通过缓存、证书或恢复manager。

### 33.4 普通memory按真实消费者排，不按bench名称估覆盖

普通Generic/TSO/Extend、限定local control/location已接，不能全部重新写为未支持。外部[stage01](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-memory-access-service-20261101/stage01-handoff.md)13文件只获机械检查，未合未构建：相邻EA Offset/Recomputed、ZeroStore、相邻retimed窄load、V128源V32/V64 lane store。保原TSO全分支与真实fault资源，不靠删guard接通。

remaining包括SetHostFPR(U64) partial lane发表/成对清高、ScalarFPRLoad与实际state/资源/独立FPR、Pre/Post fixed-home写回及fault/capture/transport、RCpc alignment/backedge省guard合同；stage02仅基线副本，无新delta。旧FP1真实有U64 lane发表，而VecFCmpMask不是standalone used VecFCmp；indexed mask也不命中12 direct controls。hoist feature默认开启不证明当前有selected anchor。旧日志只证形态，不是当前请求或时间覆盖，详见[源门复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-memory-access-service-20261101/source-gate-audit.md)。

### 33.5 FEX参照和两倍门仍约束完整工作退出

[固定FEX源码](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)仍支持decode后target-aware出生、真实DFE/CFG、局部RA同IR提交physical args、JIT直接读、线程compiler/arena复用；不支持缩SVM Function范围、抹fault/observer或把初始化/最后释放移出服务。历史CompileBlock约47ms仍是完整累计的原身份，不是纯JIT，也不是当前新分母。

本轮修正的是支持和成本合同，未证明compile上限。下一相依组依次收口真实owner失败、partial MEM与完整普通memory，再接默认请求并删除旧Describe/Finish、语义RA投影、Complete同义发现、每emit binder及无人消费存储。M1广泛少出生/少首次解释与M4真实初始化/发布/结束同步，已落地typed输出、scratch一轮与fixed稀疏化不重复认领。构建＋最少受影响语义随完整组，默认实质替代后一次完整方向、稳定后集中最终质量/fresh FEX；最终不降生成质量，Block/完整Function分别≤2×，当前尚无已证兑现组合。

<a id="c7-birth-memory-latest"></a>

## 34. 构造往返和partial MEM已有结果，lane/load及保基线修复仍待验证

2026-11-01按主树c7f34422、最近dde原构建/设备receipt、merge69～77及两个外部M1封包修订六份方案。本轮只改文档，没有合包、构建、设备执行或新采样；第1～33节保原文和原source，不回填后修结果。

### 34.1 当前源码与最近执行分开

| 身份 | 已有资格 |
| --- | --- |
| dde682d3 / native-birth-memory-build/minimum-01 | 四Android目标成功；独立typed birth194、partial MEM/fixed457、controller295断言通过；public owner另1293条中1291过、2失败。红例前缀不合成绿色，原设备namespace closed、无活跃进程/清理错误。 |
| 当前c7f34422 / merge75～77后 | 保全baseline写、有效edge view/真实EA Result修复，以及整数FPR lane、U64 LDR D、U32→ZeroExtend64 LDR S已合；**未构建、未执行**，不继承dde资格。默认gate=false。 |
| 外部M1与stage04 | INC/DEC＋NOT、typed空事件两个封包仅机械检查；Pre/Post仍未冻结WIP，RCpc仅审查无实现。均无构建/运行/性能资格。 |

证据：[dde构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-birth-memory-build-01/build.json)、[独立执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-birth-memory-minimum-01/execution.json)、[owner两失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-birth-memory-minimum-01/native-public-owner.stdout)、[设备关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-birth-memory-minimum-01/closure.json)；[merge75](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/merge-75-native-baseline-and-lane-view/receipt.json)、[merge76](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/merge-76-fpr-lane-and-direct-load/receipt.json)、[merge77](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/merge-77-u32-fixed-fpr-load/receipt.json)。没有新完整compile、CoreMark/QoR、PMU或fresh FEX结果。

### 34.2 修的是原数据合同，不追加证明层

f3c0正例实际选中。原retired ZeroTarget检查在Freeze后读取已消费Cell4 event；merge69让committed只读仍有效stable header/raw/effects，virtual阶段仍核事件/operand。原U32/EQ/no-overwrite准备/重发/退休前缀通过，不授后续所有变体。f3d0 policy0是独立合法回退，不能继续据此调整affinity或窗口。

overwrite后续失败首先是已有native register_writes没进入原RA clobber；merge73接原mask、保inclusive interval/own-publication排除和最终terminal guard。dde继而停在只返回CohortRequired、从不实际删写的native reverse检测。merge75只删这个请求检测，保原publication/read/capture/reused/fold和全部baseline写，checked DSE不动；未执行，不能称恢复已过。common前置DSE仅原子集，最终DSE/质量仍待核。late非WidthRoot recolor实际窗口重准入是另一剩余问题，不混作本次根因。

memory同理：Operand(Value)的Plus＋empty right与实际Result槽均是已有布局，reader不能猜；NativeCell.Type为有效边类型，canonical V128由同Store身份恢复；signed窄load写X不能授KnownW，extension合法也不等于精确原目标宽度。这些直接修原schema/reader，不新增canonical目录、Passed表或wrapper。

### 34.3 已落实与外部候选按实际净工作收费

partial MEM跟进已在原case457通过：真实压力/默认eviction、U32 whole REF同MEM backing、22/23双capture、完整Residence/Get退休与fresh transport/proof。历史227没有完整Residence，不能回填；457不授所有fault/guest或性能。

typed scalar直接构造已进共享默认builder，原例194通过；退出旧参数往返和普通commit解释，N与32B FlagsIntent owner仍在。外部INC/DEC＋NOT包才少两类局部literal出生；另一个typed事件包仅少空分派/无规则fold，以唯一194条effects投影保持14个原forwarding操作和3个实际flag observer。静态opcode不等于Known target，Unknown/EarlyOff/raw及未完continuation保合同。现有ThinLTO ELF的具体调用证明原工作没有全部编译消除，不证明权重或提速；新增query/header/template/text/data也收费。见[literal净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/m1-decoder-unary-immediates-20261101/net-work.md)及[事件/机器码净账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/m1-known-append-events-20261101/net-work.md)。

stage01～03复用同一原source/Apply访问，真实FPR load副作用先于live，旧整数结果不虚构allocation；raw commit/Tick/fault/partial保存仍保。四个Runtime.Run fixture仅为新源码。Pre/Post须同组闭合真实额外def、固定写回、硬输入/冷恢复与故障状态；RCpc须迁alignment/backedge条件，已有kernel或普通TSO编码不构成准入证明。

### 34.4 FEX参照仍要求默认旧服务退出，不能由小项保证上限

固定FEX的target-aware emitter、同IR局部RA→physical operand、JIT直接读与线程级arena是净工作参照，DFE/CFG、spill冷路和发布仍真实存在；不是把它的局部scope、每guest flush或校验边界照搬来降SVM质量。SVM已有每线程单idle assembler复用，代码buffer容量门为256KiB，也不再列为未来M4收益。

下一顺序改为：当前修复/lane-load相依组最少验证；M1两包只按净账合并；闭合实际默认请求的写回/RCpc及精确残余；接支持域默认入口并删旧Describe/Finish、RA语义投影、Complete发现、每emit binder与无人消费数据；再做一次完整方向。不要让不触发的冷域阻止常态反馈，也不能常态跑完整新旧双链。M1广泛少构造/首次解释与M4实际初始化/结束仍未解决；正确性扩域、较小结构或本轮组件通过都不是数量级收益。

最终仍按原6.3/6.4：不降生成/运行质量，独立Block和完整Function各≤2×同轮fresh固定FEX；完整初始化、发布和最后结束在口径内。中途成组构建与最少受影响语义，不逐helper做QoR/PMU/FEX。**当前没有已证明满足性能门的完整组合。** [本轮修订边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-09-birth-memory-20261101/start.json)固定主树、索引、原receipt、旧正文、审查1～33与最终门。

<a id="c7-actual-inputs-latest"></a>

## 35. 构造与coalescer已获限定结果，下一主线直接生产actual输入并退旧解释

2026-11-01按当前c6e53ce8、be/9b/33da/6d各自原receipt和RCpc冻结包修订。本文档更新未改产品、未合包、未构建、未上设备；第1～34节原文保持。结论是：**形态和正确性接线已有进展，默认旧服务与输入重复解释尚未退出，也没有新的完整compile或fresh FEX结果。**

### 35.1 当前资格与下一修复

| source / 原结果 | 可确认范围 |
| --- | --- |
| be235db1 | 四目标；M1相关typed194/cancel17/helper890/controller295/decode flags1058五独立例2454通过，owner1745/1744/1失败。 |
| 9b4249b9 | 四目标；真实source-ready证明修正后原owner整例1861通过，既有lane/load运行SECTION获限定结果。be绿色不改标为9b全套。 |
| 33da2b12 | 四目标；实际窗口late coalescer、自然W→X owner变体，fixed457/controller295/owner1989三例2741通过。 |
| 当前c6e53ce8 | 四目标；新增WB owner2027/2024/3失败。实际base/output同正确GPR、use与canonical端点正确，base Definition返回pinned=false，被位置检查拒绝；无WB执行阳性。 |
| stage05 / actual-input-service | RCpc已冻结未合未构建；actual输入仅外部WIP，residence/alias/caller/冷恢复/原fixture未全部接替。 |

WB须提取原full-X read-definition纯字段合同，让final reader核实际Allocation固定定义；source仍核Origin与quartet，base仍同home/无reload/末端/lease/不别名。output必须是version=true的真实固定写。这是未应用的修复计划；不删检查、不伪造Derived、不扩metadata重试原因。临时WB一点Derived缩窗桥随直接生产退出，独立actual read proof保留。

证据：[be独立例](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-lane-m1-minimum-01/execution.json)、[9b owner](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-lane-m1-owner-03/execution.json)、[33da原组](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-coalescer-minimum-01/execution.json)、[当前构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-writeback-build-03/build.json)、[当前失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-writeback-owner-02/execution.json)和[子句日志](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-writeback-owner-02/native-public-owner.stderr)。相关设备namespace均已关闭，无活跃写入或清理错误。

### 35.2 主要结构缺口是语义预生产→实际重绑定仍存在

当前native支持域没有运行完整旧Describe/Finish，**但Preclose仍调用PrepareGPRQualityTopology、语义residence与fixed准备**，转换后又做PrepareNativeFlagsInputs全域再解释、BindNativeGPRQuality及Derived fixed重资格。少表/同槽/少一次外层visit没有消除这里同一义务的多次描述。

下一完整组在原转换访问收集候选，实际形态/use/def/terminal闭合后，以原Descriptor的body/use/terminal访问直接填原AllocationInputs；同包退native语义预生产、flags再解释、quality重绑定与WB兼容桥。原semantic SSA/source gate、checked/Unknown/raw、Explicit硬合同、REF/复杂Copy、完整Function及独立proof保持。冷metadata恢复actual body后从同owner重建Derived，保一次general scope与独立FPR预算。详见[主方案5.3.12.15](spill-and-compile-root-cause-plan-2026-10-28.md#native-actual-input-service)与[逐源5.1.11](middle-pipeline-file-layout-2026-10-28.md#native-actual-input-layout)。

source W-view/Get32必须解除对旧GPR行的依赖；ZeroTarget raw隐式资格与actual terminal lease、WB terminal/foreign旧base拒绝、final consumer kind/identity及原NativeRequest fixture均为同包退出条件，不能以post-consume Unsupported或更多retry补缺。D/E_D可等于N/E，候选过滤、双前缀峰值、排序/compact、Explicit与retained容量计入净账；复用原40B行只是未构建设计，不是成本消失。

### 35.3 M1和quality修正分别记工作，不能授完整性能

M1两包已合：INC/DEC＋NOT少局部literal，typed事件以唯一194条effects裁空分派；原flags/forwarding/observer/continuation保持。组合dde→be ELF.text −50128B、.rodata 0，pure OpMadd无AfterAppend，Unknown-flags TestNotZero仍为真实fold调用；包含多项产品差异，既不作M1归因也不证明I-cache或compile下降。见[静态ELF证据](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-lane-m1-code-size-01/evidence.json)。

late coalescer已按实际produced/stored/write owner、canonical lease和原clobber/observer/fixed/REF窗口重核，原Q×N及新增根查询仍在；no-copy publication先证真实numeric source，再证guest witness与末端窗口。正确性修正不等于compile优化。M1广泛首次解释与M4真正初始化/复制/发布/结束仍是主线，不重复认领已落地typed输出、scratch交接、stable页和线程assembler复用。

RCpc stage05借原SubmitNative及独立site reader生产/重核实际对齐，使用总访问预算；非相邻home、深链、特殊寻址和未证backedge保guard。尚未完全退出旧alignment服务，不能将部分SSA证明授旧physical-home全部质量或性能。

### 35.4 FEX参照落实到决策终结，不增加另一层描述

[固定FEX复核](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)仍显示decode后的target-aware emitter、同IR局部RA直接写physical operand、JIT读取及线程arena；DFE/CFG/压力与发布真实存在。SVM下一步借鉴的是选定形态直接交机器分配输入和最终资源，保自己的Function/fault/observer合同，不复制局部RA范围或删除证明。

实施顺序为：修WB明确合同；完成actual输入producer/consumer/冷恢复及旧工作退出；配套闭合实际触发的RCpc等残余；接默认支持域并退完整旧链，再做一次完整方向。成组构建和最少受影响原例即可推进，中途允许生成质量变化；稳定后集中原6.3/6.4，最终生成质量不降，独立Block/完整Function各≤2×同轮fresh固定FEX。当前方案给出真实退出条件，**尚未证明compile上限或已达目标**。[本次文档修订边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-10-actual-inputs-20261101/start.json)保存source/index、原正文、审查1～34与最终门。


<a id="c7-actual-inputs-integrated-latest"></a>

## 36. actual输入已经直产，下一关键是实际边界与默认旧服务退出

2026-11-01按当前cee0ce21、merge88～93及原build/minimum receipt更新。文档本轮不改产品、不合包、不构建或设备运行；第1～35节保持历史时态。本版的进展是**native语义预生产→实际修订链已经退出**；尚未证明默认覆盖或完整compile大幅收益。

### 36.1 当前证据与四个失败边界

`native-actual-input-build-04`保存同source四Android目标。红魔bf1359ff三命令独立执行：fixed457、controller294两例通过；owner2075/2071/4失败。前三在Runtime pre-store f800、两块Function f840、metadata Post f8a0：source_result均Complete，stderr在initial-inputs分别result1/1/3；第四unknown RCpc control cell既非Immediate也非Empty。失败边界已知，根因及修复未完成。controller295→294是两个旧逐view包装断言变一个actual批量入口断言，原wide/分配/retry质量断言保持。

[构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-actual-input-build-04/build.json)、[三例结果](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-actual-input-minimum-01/execution.json)、[失败stdout](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-actual-input-minimum-01/native-public-owner.stdout)与[定位stderr](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-actual-input-minimum-01/native-public-owner.stderr)保原source/ELF。[namespace关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-actual-input-minimum-01/closure.json)无活跃进程或清理错误。33da历史三绿2741不合授cee0全套；没有新的完整compile、QoR/PMU或fresh FEX，默认gate仍false。

### 36.2 真正退出与仍付出的工作分别记账

原转换记录C/P，整scope全部成员提交后Close；actual memory/flags/W-view/Get32先闭合，再由fresh machine live及原Descriptor body/use/raw/terminal直接生产GPR/residence/Derived-fixed和原scratch/pool/ABI。八个旧native quality/residence/fixed/flags/view/Get32 API及两个WB兼容API已无声明/定义/caller；checked合法semantic builder保留。WB实际full-X read、output version与selected Definition JIT读取修正已合，不能把当前initial失败继续标成旧pinned注解根因。

raw资格与actual lease独立，optional actual_kind只是公共Numeric/Implicit/Terminal；raw为空，不是Passed。40B ResidenceUse/64B BlockRange已静态断言，C/D仍可=N、R/A可=E。规范/排序/聚合、候选触发全域closure、原P×range/family及alias/REF/Explicit、临时共存/retained均计费；Known-W/复杂Copy/WB source仍可能建semantic live，不能认领全图成本已消失。

冷路必须先结束借用/取回同AllocationState，删Derived，在旧gpr活时恢复Explicit ordinal；保CallABI清generated偏好与旧actual列，reset gpr，再逆body/Withdraw、仅冷路重收C/P、fresh static/live/Describe、原RebuildProgram。不恢复旧semantic计划。一次metadata和FPR独立状态/预算配置保持；RunAttempts每次仍重建eviction8，不能虚称跨重建累计余量。详见[主方案5.3.12.10](spill-and-compile-root-cause-plan-2026-10-28.md#native-cohort-owner-rebuild)及[实际输入](spill-and-compile-root-cause-plan-2026-10-28.md#native-actual-input-service)。

### 36.3 RCpc最终位置属于原资源服务，不能再造输入层

stage05已合，只有早期有界SSA子集；stage06最终位置/P值为外部冻结候选，未合入/构建/执行。它拟复用原BuildNativeCurrent/PreparePhysical与FreezePhysical两循环，producer/reader各自B-local 32 GPR normal/active状态，在真实capture/definition P独立重算后按原identity/slot/width/offset/lease及完整alias消费。capture借原action两个尾padding字节，普通spill借PhysicalValue两个prefix padding字节；action16B有静态断言，后者整布局待构建，不叫零成本或证明缓存。

两次实际状态fold、P读写/初始化及alias查询收费；Get source/offset与whole alias link只保到原FreezeNative最后reader，不带Ready、不加N目录/pass。stage05早hook/reader同包退，helper raw ABI与保存后net clobber、fixed/preserved/shared/reload/lease和frame顺序保持，ForceChecks/fault_emulate/16B/biased/page-table/未知/未证backedge不乐观放行。checked/Unknown合法alignment不退。逐源见[5.1.12](middle-pipeline-file-layout-2026-10-28.md#native-final-location-alignment-layout)；[外部冻结交接](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-memory-access-service-20261101/stage06-final-location-alignment/stage06-handoff.md)中的source.patch与独立compiler hunk须同组整合，reference compiler不能覆盖现caller，dry-run不授构建/语义资格。

### 36.4 按FEX决定链推进默认采用，再判断完整收益

固定FEX的target-aware构造、DFE/CFG、局部RA写同IR物理operand、JIT读取及线程arena仍是[已核的参照](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/fex-fixed-pipeline-review-20261031/report.md)；它不授权缩小SVM Function、删fault/observer或把完整timer改成emit。actual输入替代已向这个方向减掉一段真实重复解释，但输出直连、scratch/assembler复用是既有能力，不重复预计收益。

下一组先修四失败边界，完成真实最终位置接缝；原owner最少检查收口后清临时默认诊断、以DecodedBlocks对齐guest块统计，并在原消费权/target门内统一切默认支持域。单guest块Function有synthetic entry，仍保Function owner/CFG。真实覆盖用IsNative/NativeReady，不能借CanEmitForExecution或旧AllocateFunction计数。消费前只有StatePolicyRequired/UnsupportedForm可回原链，InvalidProgram与Complete＋nullopt结束组；合法source回退、metadata重建、Function失败Block重解码及初始化/发布/结束全计完整成本。

默认支持域退出旧Describe/Finish、Complete同义发现及每emit binder后，才做一次完整Block/Function方向；若仍远离目标，按真实剩余构造/首轮解释、actual窗口/前缀、state/REF/CFG和资源/请求费用改算法，不继续少表/合循环微调。相依组构建＋最少受影响原例，中间允许质量变化，稳定后集中原6.3/6.4；最终质量不降、两scope分别≤2×同轮fresh固定FEX不变。**方案仍无源码级上限保证，目前未达标。** [本次修订边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-11-integrated-actual-inputs-20261101/start.json)保存产品/index及原审查/正文/最终门身份。

<a id="c7-native-default-latest"></a>

## 37. 默认支持域已接管；真实normal覆盖与完整compile收益仍待方向

2026-11-01修订12依据source `dd0e8480eac1ae9a92c88da2bc810b02ba24180d61fa8c6a5581b0737e43dd7b`与[默认接管检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-default-checkpoint.json)更新。本次只修六份方案文档，不改产品、索引，不构建或上设备；第1～36节保原字节，修复不回填旧阳性或失败。主方案最终6.3/6.4不变。

### 37.1 同source里程碑与有限证据

actual输入直产、WB actual Cell1/窄Store Cell3/raw-only matcher修复、stage06最终位置和默认接管均已合。`native-default-build-01`四Android目标成功；`native-default-minimum-01`原owner2230和NarrowRead125，共2355断言通过、stderr空、namespace closed。owner已含原WB Pre/Post、两块Function与精确metadata恢复；NarrowRead补真实六参BeginNative而保capture/partial/LowView断言。旧cee0四失败和final-location轮NarrowRead缺BeginNative失败仍保原身份，不拼历史例为当前全套。

本结果不授全部P/MEM/capture/spill/helper组合、normal guest或最终质量。**r2的首个真实Block CoreMark guest启动后提前exit1、stdout/CRC为空；其余三点未执行，没有有效性能点或fresh FEX分母。** prof的39 block units/886 IR在请求前段累计，不能证明39块均完成新链或定位失败PC；19.962138ms只是提前退出前累计，不是优化收益。normal真正IsNative/NativeReady覆盖仍未测得。显式owner不等于normal采用率，旧CanEmitForExecution分类也不等于同Store新链。

### 37.2 默认路由保原消费权与typed失败；打印工作真退出

唯一`kNativePreparationEnabled=true`只开启原PrepareBlock/PrepareFunction/PrepareFunctions支持域。ARM64 JIT、匹配target/environment、fresh ExecutionAccess、density/dump/verify和Function func_ir_free/完整组门保持。Unknown/RV64/public/retained与无消费权FinalBlock保其合法服务，Known/EarlyOff本身不授消费权。

全部成员先Preclose，仅消费前StatePolicyRequired/UnsupportedForm可交未消费OptimizedUnit给原链；InvalidProgram及Complete＋nullopt结束整组。source_result=Complete只说明source门，不是Describe或Ready。Take/Close后只按实际程序及原精确metadata冷恢复，不回已死Inst；单guest块Function保synthetic entry、Function owner与完整CFG。

15类临时trace、四个纯打印wrapper/88调用点及专用查询已删，真实typed return/Reject与最终独立证明保留；必要异常日志继续。fixed_single/multi改在TakeIRWithCFG前保存原DecodedBlocks至同Storage单size_t，成功后Record、Return清零；synthetic entry不改变guest块分类，scalar/padding纳原128KiB预算，无新N/B表。

### 37.3 已退出服务与真实净账

支持域默认不跑semantic GPR/residence/Derived-fixed预生产、flags整轮修订、Bind、WB一点桥、旧Describe/Complete同义发现和每emit binder，这是代码路由事实。原Descriptor实际body/use/raw/terminal直接交原AllocationInputs，两scope原RA/state/transport与fresh独立proof保持；checked保留分支有真实读者，不一并删除。

stage06已在原BuildNativeCurrent/PreparePhysical和FreezePhysical两晚visit分别形成/独立重算真实GPR/P low3，stage05早hook/reader退出。P沿原value/source/slot/width/offset/lease，普通MEM严格早于read，capture可按真实prefix同点；REF遇shared_write不借值等价，source/alias借用在最后reader后结束。ForceChecks/fault_emulate/16B/biased/page-table/未知与未证backedge保持；不是Passed或新历史图。

稀疏不免费：C/D可=N、R/A可=E，规范/排序/closure、临时共存/capacity/retained、source可能首建semantic live、P×range/family、Explicit/REF/CFG/Φ、两次晚fold/P读写与资源双resolve照计。metadata按一次原scope同owner恢复，RunAttempts每次初始化eviction8，原FPR独立状态/预算配置保持；失败额外N/E/P与RA不移出完整compile。

### 37.4 按FEX决定链保完整方向与最终门

FEX已知目标早定形态、同IR局部RA写物理operand、JIT读取与线程arena是固定参照；不以其块内范围、可选核验或旧计时替代SVM完整Function/fault/observer合同。当前默认路由缩短了一段真实决定链，但没有新分母或源码级≤2×保证。

两次中止分别保留：r1因host适配器point父目录缺失，在任何guest前退出70，0 guest；r2只修该目录创建，首个Block guest实际启动后target exit1/无CRC，capture据此exit78。r2不是r1的预启动故障，也不证明具体native/RA/proof根因。Block-FEX、Function-FEX、Function-SVM均未执行；原执行账未补点或重跑替换。[r2原执行记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-development-direction-adapter-r2-20261101/prepared-native-default-dd0e8480-r2/execution.json)、[target退出码](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-development-direction-adapter-r2-20261101/prepared-native-default-dd0e8480-r2/device/points/block-svm/sample/target-exit-code.txt)、[prof stderr](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-development-direction-adapter-r2-20261101/prepared-native-default-dd0e8480-r2/device/points/block-svm/sample/stderr.txt)与[私服关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-development-direction-adapter-r2-20261101/prepared-native-default-dd0e8480-r2/device/service/closure.txt)保原身份。owned target清理与私服stop/wait/finish均成功，owned server generation/socket缺席。

执行顺序改为：先读现有logcat的异常halt reason/RIP，必要时仅在原失败return输出PC/typed result/阶段；先区分guest主动退出与authority、prepare、finalize、emit或allocation失败，再按所定位合同成组修复。Android LOG_ERROR走logcat，stderr只有prof不等于没有异常日志。维持default、原source/消费后终止与精确metadata预算，不以关闭默认、缩Function、放宽proof或扩大fallback制造成功；诊断运行不充当普通性能点。原失败guest正常结束且CRC恢复后，才开始新一轮完整四点方向，不新开逐helper检查矩阵。

完整请求包含source miss后原链、metadata整scope重建、Function失败Block重解码、初始化/发布/最后结束，不能把cold unsupported移出口径。coarse regalloc只包原controller，source/actual准备、Descriptor与state/resource可能落在other；codegen包PrepareForExecution/emit，不代表全部后端。有效完整Translate才是方向依据，不把小桶、部分累计或旧47ms当新比值。若仍远离目标，依据真实剩余构造/首解释、actual窗口/state/REF/CFG及资源/请求费用改算法；不以合visit、少字段或重命名表承担主要提速。中途成组owning构建＋最少受影响原例，稳定后集中原6.3/6.4；最终质量不降、独立Block和完整Function分别≤2×同轮fresh固定FEX，当前未达标。

<a id="c7-native-contract-repair-latest"></a>

## 38. 启动拒绝已定位：修正交接合同，性能资格仍未建立

2026-11-01修订13对照当前source `f8ed7a198a58f3aed8a3b72fddb7654a019d3a27a6b9f116497f1836bd07a344`、已完成构建/原owner与独立启动诊断。本轮只改六份文档，不合代码、不构建或上设备；第1～37节、旧方案正文与最终6.3/6.4保原字节。第37节的“原因未知”是原审查时态，下面记录后续证据，不回填原失败。

### 38.1 当前代码、失败与未验证范围

| 身份 | 已证事实 | 不提供的资格 |
| --- | --- | --- |
| 原dd0四点方向r2 | 首个Block guest exit1、无CRC；原logcat与pid匹配，IllegalCode/RIP`0x231136` | 39块/886IR与19.962ms不构成正常点、采用率或完整差距；原其余三点未执行 |
| `8bedfa0c`同点修复诊断 | 原guest越过publication拒绝，继续到`0x257cb0`的flags核验；181块/6497IR后exit1、无CRC | 不是完整guest，也不是compile改进；不能与旧FEX或原39块累计相除 |
| `b175bf25`flags定位 | Disabled CondSet残留LocalCondition，SelectDirect实际已拥有Condition读取；原generic检查拒绝无Result | 不能删generic检查或为Disabled源免证；后续f8ed实际修复单独记录 |
| 当前f8ed构建/原owner | 四Android目标成功；原owner2346断言中2344通过、两个新增fixture断言失败，namespace关闭 | 非整例全绿；未运行最新修复的原CoreMark，无新CRC/完整compile/fresh FEX或最终质量 |

原始依据：[logcat定位记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/original-failure-observation.json)、[同点修复后诊断](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/one-guest-adapter-root-staging-fix-r2/runs/handoff-startup-r1/execution.json)、[flags诊断](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/one-guest-adapter-root-staging-fix-r2/runs/flags-startup-r1/execution.json)、[当前构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-startup-repair-build-01/build.json)、[owner原执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-startup-repair-owner-01/execution.json)、[断言原文](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-startup-repair-owner-01/native-public-owner.stdout)及[关闭](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-startup-repair-owner-01/closure.json)。诊断source/ELF不充当普通性能版本。

两个新fixture问题分别是：默认测试pool缺生产ABI bank前提，预插新Add硬窗又改变旧值分配，因此先在old1/new0的same-home前提失败；numeric Select测试把MEM descriptor专有producer token当成普通寄存器必存字段。原失败未单独采read.kind，不将具体register/MEM class当本次实测；后者已有按实际class分支核验的test-only修正包，前者仍需真实Block/完整Function域分别闭合。两者均未在本轮合入/构建/重跑，不授测试通过，也不倒推证明产品无其它错误。

### 38.2 从具体拒绝收敛为通用合同

1. **同点交接核完整allocation scope。** 原allocator允许旧end==新start的合法先读后写；publication旧证明没有识别这一端点。新证明归一真实write owner，要求完整聚合LiveEnd==P、新actual birth==P、完整use枚举确实读同owner id+identity且无terminal旧读取，P前所有fixed/inherited窗口继续查。不能把B-local LeaseEnd截断当Function全局死亡，也不能全局放松区间或将shared write视作whole alias。[核心proof](../source/runtime/backend/arm64/compile/verify_gpr_core.h)、[native读口](../source/runtime/backend/arm64/compile/verify_gpr_native.h)、[原tie](../source/runtime/compile/allocate/ties.cpp)。
2. **选择接管同时结束旧交付。** DirectSelect接管guest condition时，原CondSet不再产numeric或local key；成功选择处仅清ConsumerAction.LocalCondition属性及对应的LocalCondition event，保其它action属性。原Q仍核Disabled源精确唯一归属与零额外numeric/key/terminal/live义务，最终graph独立核raw身份、type/occurrence、PSTATE/observer窗口；Carry-other继续原tuple证明。generic无Result拒绝保持，清event不豁免悬空源。[实际选择/核验](../source/runtime/backend/arm64/compile/select_flags.cpp)。
3. **物理身份按真实表示承担。** final id、canonical物化id、Store NodeId/domain与MEM producer token不同。普通GPR/FPR的ReadPhysical从cell取得实际位置与final身份，不填producer；MEM返回完整descriptor。测试核原body与实际definition/read/Allocation，MEM再核token；不为错误断言扩大普通cell或新增origin表。[读口](../source/runtime/backend/arm64/compile/native.cpp)、[PhysicalValue](../source/runtime/backend/arm64/compile/physical.h)。

这些合同归现有select、native和独立proof，**不需要新源文件、pass、ownership目录或certificate层**。同点write-owner/use/terminal和prefix查询、原Q/raw和最终graph照计成本；保最终独立性不代表必须保旧中间表。所有公共/retained/Unknown与消费前合法分支、原typed失败和精确metadata预算保持。主方案[5.3.12.18](spill-and-compile-root-cause-plan-2026-10-28.md#native-handoff-obligations)、[逐源责任5.1.13](middle-pipeline-file-layout-2026-10-28.md#native-handoff-layout)已据此修订。

完整Function的测试另有准入边界：当前scalar soft-birth要求producer/publication同B；跨B Explicit即使数字硬窗相邻，也必须通过现Requalify的foreign/capture/lease合同。尚未成立的正例不能记绿，不为夹具增加产品域、删same-home或挪点掩盖B末截断；完整Function实际live-out拒绝要求保留。此项是待闭合fixture/准入，不是新测得的compile根因。

### 38.3 对照固定FEX：交付后停止旧义务，不照搬范围

固定本地FEX `a156752fae3a4f65fefb1ed95af980bcdfac43d7`的RA先使source同时可用，再处理kill、写physical args及分配destination；JIT按class/reg读取物理参数。`RemovePostRA`只unlink，不再按semantic arg更新uses。这里可借鉴的是阶段交付必须明确结束旧读者/义务；SVM自己的raw退休、完整Function lease、fault/observer和独立证明仍需保留。

源码依据：[FEX RA](/Volumes/Data2/swiftvm-middle-build-cache/fex-android/source/FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp:697)、[JIT物理读口](/Volumes/Data2/swiftvm-middle-build-cache/fex-android/source/FEXCore/Source/Interface/Core/JIT/JITClass.h:91)、[RemovePostRA](/Volumes/Data2/swiftvm-middle-build-cache/fex-android/source/FEXCore/Source/Interface/IR/IREmitter.cpp:149)。只读源码与历史Android构建背景不是新FEX测量，也不保证SVM能达到同样常数。

### 38.4 余下顺序与性能目标

先按同一相依组修正fixture、最少原owner和原失败guest基本执行；恢复CRC后退出临时阶段打印/最多64条body快照，冻结普通版本，再开始新一轮完整Block/Function四点方向。旧r1/r2和独立诊断各保原失败，不补点替换。此顺序只要求基本可执行性，中途不增加逐helper QoR/PMU/FEX矩阵，最终仍集中原6.3/6.4。

有效方向之后，才按真实剩余工作选大算法。M1当前只读审查指出出生规则→首次residual发现/roster→真实反馈是一个候选；state/flags先于ValueWorklist修改程序、roster服务CSE/深事实/Users等接缝未闭合，不能用birth-done跳过首轮。actual C/R/A与P×range/family、state/Recolor/REF/CFG/Φ、两个晚fold/双resolve和M4触达仍收费。已完成的typed五槽、EA/extend/deferred、有限M1和旧Describe/Bind退役不再认领未来收益。[M1剩余审查](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/m1-remaining-work-review.md)。

**最终质量不降，独立Block与完整Function分别≤2×同轮fresh固定FEX，当前没有达标证据或源码级上限保证。** 完整Translate仍计source miss后原链、metadata整scope重建、Function失败Block重解码、初始化/发布/最后结束；不以少表、合遍历、部分累计或旧47ms制造新比值。本轮[文档边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-13-native-contract-repair-20261101/start.json)与[校验](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-13-native-contract-repair-20261101/validation.json)保存产品/索引和冻结历史。

<a id="canonical-raw-flags-contract-latest"></a>

## 39. 窄flags现场已确认：构造资格、完整窗口与最终读取必须成组闭合

2026-11-01修订14对照当前诊断source `32e68ab318385f3624c1df01572cffacebfa1534500d14bd46c74be549af01fd`及已结束的原构建/执行。本文修订只改六份文档，不合任何候选或测试包、不构建/上设备；第1～38节、旧方案正文和最终6.3/6.4保原字节。第38节的夹具待修/原guest未跑是当时状态，不回填历史。

### 39.1 已落地与实际资格

| 身份 | 已证事实 | 不能继承的结论 |
| --- | --- | --- |
| 9e959229，native-logical-width-build/owner-01 | 四Android目标成功；原owner一例2809断言全过。已修真实ABI/pool、普通寄存器与MEM身份、事务外lease编辑、完整两块Function B末正例/live-out负例及逻辑立即数W宽度 | 不授其它历史case、完整guest或最终质量；后续32e6不称重跑四目标/整例 |
| 9e95，logical-startup-r1 | 原Block CoreMark exit1/无CRC，284块/9298IR；匹配pid13963的logcat定位PageFatal=2、RIP0x2480ae memmove | 约215ms是失败前累计，不能与旧47ms/FEX相除；本检查点尚未区分bad incoming size与loop/control |
| 9e95，exectrace9e-r1 | 同RSP最后4096条在0x248075/0x2480ae间交替，含2048次连续body | trace无RDX采样，不捏造initial size或机器减法值；instrumentation及重复signal dump不作普通计时 |
| 当前32e6，memshape-r1 | 仅compiler形态快照和translator重建，原guest仍失败；实际native头无integer/backedge/PFAF，ordinary SaveResult＋LocalCondSet/local If的组合已采到 | 现场合同已确认不等于修复完成或唯一剩余错误；无有效CRC、完整compile、fresh FEX或≤2×保证 |

依据：[9e95冻结检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-logical-width-checkpoint.json)、[四目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-logical-width-build-01/build.json)、[2809断言原执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-logical-width-owner-01/execution.json)、[原guest失败](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/one-guest-adapter-root-staging-fix-r2/runs/logical-startup-r1/execution.json)、[原pid logcat读取](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/logical-startup-9e95-check-01/logcat-read.json)、[循环诊断](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/one-guest-bounded-exectrace-adapter-r1/runs/exectrace9e-r1/trace-analysis.json)、[32e6形态/终止记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/memshape-32e6-check-01/terminal.json)及[实际头部快照](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/memshape-32e6-check-01/head-shape-excerpt.txt)。各执行及服务已闭合；原记录的根因认识保持原时态。

### 39.2 实际窄TEST没有兑现raw NZCV承诺

guest头`0x248075: testb $0x18,%dl; jne 0x2480ae`，body每次SUB8后回头。若TEST/分支及SUB8都正确，`q=(RDX>>3)&3`每次减1模4，任何uint64初值最多进入body三次；2048次同栈连续body排除了“只有incoming size坏”的解释，尚不能单靠ring决定哪个机器动作错。[原guest反汇编](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/guest-pc-2480ae.disassembly.txt)。

32e6实际快照进一步确认：And U8 LogicalImmediate、Or U8 Generic的effective NZCV demand均为0（Or虽有raw pseudo，emission override为0）；普通SaveFlags为SaveResult，真实U8结果参数和NZP mask有效；后面LocalCondSet NE只有LocalCondition event、无Result，PhysicalIf.local_condition=1，直接B.cond。该普通SaveResult更新canonical flags后恢复incoming host NZCV，分支前无恢复；本次条件遂没有使用新TEST结果。原loop body Sub留的旧Z可以被继续读，不能把raw pseudo/header或map key当成来源证明。

源码依据：[窄logical构造](/Volumes/Data2/SwiftVM/source/runtime/frontend/x86/decoder_alu.cc:742)、[SaveResult实际kernel](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/jit/translator_flags_physical.cpp:148)、[LocalCondSet实际读取](/Volumes/Data2/SwiftVM/source/runtime/backend/arm64/jit/translator_scalar_physical.cpp:1092)。CondSetControl是另一个合法域：已有key/pending可用则复用，否则原Prepare/Merge/Flush/Load取得canonical条件。BranchOnly SaveResult先写canonical requested bits再实际加载NZCV；Generic/raw BranchOnly依producer的已证明raw结果而不发表canonical。不能给LocalCondSet统一插reload，否则后者会被旧canonical覆盖；不能将两类BranchOnly混写成同一发表行为。

同组已合的宽度修复也须按实际kernel理解：U8结果/U32输入可同为W；selector在提交前以共享纯shape核实际32/64位宽与mask，optional miss保Generic完整输入，独立reader仍核实际位置。不能因语义type不同拒合法W，也不能盲选后以半提交触发StatePolicy。2809断言包含该真实回归，不提供时间收益。

### 39.3 三处原写口闭合，候选还差完整窗口

1. **Decode不发过强承诺。** 窄legacy logical Save撤raw NZCV资格、保PF named-result；宽/deferred/FP沿原合同。BranchOnlyEdges只证明两后继incoming flags死，不授权当前PSTATE；canonical CondSet可以带真实marker，仍须后续完整证明。
2. **原flags pass成功时同时交接。** 全部required位须来自实际primary，不能继承INC未写CF；完整primary/发表→condition窗口必须安全，跨过AdvancePC，排除相交写者及会观察被撤发表的读/fault/helper/control。全部门成立后，原RewriteContext同节点将Save/CondSet同步改为BranchOnly/Local，保持结果身份/type/terminal use；没证明则保canonical。只放宽matcher接纳CondSet，会把raw提供者和canonical消费者拆开。
3. **最终proof核真实incoming全mask。** 原actual prefix visit在Bind meet后、Observe当前body前核ConditionFlags，包含key与numeric CSET，不受Result/HasPstateTests摘要跳过。物理Carry可读不替代canonical极性、condition映射/tuple/window；普通Save不能从旧physical_values借新资格。仅已有metadata+integer独证且没有真实body/PSTATE读的Covered退休ZeroTarget可按原合同跳过；Generic/noResult继续严格。条件生成后的last-consumer窗口保原graph证明。

| 外部交付物 | 当前状态与必须补的工作 |
| --- | --- |
| frontend/pass＋最少decode/flags回归，patch `c0770ac0302a8f0ad64b9630e98da05285bb263d520c26746b1df564c558ed15` | 冻结候选，未合/构建/执行；原扫描只到producer_advance，漏其后condition gap，**完整资格尚未闭合，不可直接采用** |
| incoming-mask独证，patch `1e2b1509e0ac8faed4e0e28166f08695e9e1566feec1c912630a253d7b9d7c18` | 复用RawPstate原字段和原visit，无新表/扫描；未合/构建/执行，完整mask不能替前端合法域交付 |
| native owner回归草案 | 外部草案、尚未独立收口/合入/构建/执行；拟核canonical正确分支、key/numeric stale raw及缺一个required位，不授测试通过 |

候选依据：[frontend/pass交付](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/narrow-logical-condition-contract-01/HANDOFF.md)、[原冻结补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/narrow-logical-condition-contract-01/source.patch)、[incoming-mask补丁](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/local-condition-physical-prefix-fix/source.patch)、[owner草案](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/canonical-condition-owner-regression/source.patch)。本轮只读复审发现`SaveFlags(Z)→AdvancePC→ClearFlags(Z)→CondSetNE`可被错误转换，gap fault/observer也可能观察撤掉的canonical状态；现包LaterWrite/Fault均放在AdvancePC之前，未覆盖此缺口。须先修原资格窗口并补**AdvancePC后**writer/MayFault反例，确认不转换且canonical仍合法，不能依最终Reject为合法输入兜底。

### 39.4 性能路线、成本与验证节奏

这组修复沿原decoder、TryBranchOnly、RawPstate/prefix和kernel合同，无新源文件、N表、manager或pass；原prefix增加opcode/fullmask核对及完整窗口访问仍收费。本组纠正运行错误，**不能以断言全绿、少一个form或故障位置推进宣称compile加速**。固定FEX仍仅作为早定形态、同IR RA/physical提交及arena复用的源码参照，不照搬其局部范围或可选核验，也不把旧47ms当新分母。

下一步按一个相依组完成窗口资格＋成功交接＋独立prefix proof，再统一owning构建和最少受影响原owner/decode/BranchOnly例，运行原失败guest确认正常结束/CRC。随后删全部临时失败打印/阶段变量/body和两PC快照，冻结普通版本，开始新的完整Block/Function四点方向。stock EXEC_TRACE在每次恢复signal均dump，已产生260组约18.3MB；单ring4096条不等于总输出上限，不再用它重复扩采。中途不逐helper开QoR/PMU/FEX矩阵。

有效方向之后再按M1首次构造/解释/反馈、M2/M3 actual窗口/state/REF/CFG/Φ和M4资源/请求触达选大算法；当前M1剩余只是静态候选，两次晚fold、P读写/双resolve及冷路费用仍在。本次正确性组不替代这些真实退工，也不证明性能上限。完整Translate计source miss后原链、metadata整scope重建、Function失败Block重解码及初始化/发布/结束；**最终质量不降，独立Block与完整Function各≤2×同轮fresh固定FEX，当前未达标。**

主方案[5.3.12.19](spill-and-compile-root-cause-plan-2026-10-28.md#canonical-raw-flags-obligations)、[7.1/7.2](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)、[逐源职责5.1.14](middle-pipeline-file-layout-2026-10-28.md#canonical-raw-flags-layout)、[IR合同5.3.13](ir-representation-redesign-2026-10-28.md#5313-canonical状态raw-nzcv与条件身份不能互相代证)及[目标边界7.8](target-aware-middle-pipeline-design-2026-10-28.md#78-decode只能承诺实际可交付的局部flags域)已同步。[修订边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-14-canonical-raw-flags-20261101/start.json)和[文档核验](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-14-canonical-raw-flags-20261101/validation.json)记录source、Git索引及历史冻结；未stage/commit/push。

<a id="actual-flags-cold-publication-latest"></a>

## 40. 实际flags及VOID角色已修，metadata冷publication尚未闭合

2026-11-01修订15对照source `861ab086021f3270db6e91c98149431fd15268680534f0f7accf5b8e2ce3d151`、已结束的原构建/最少语义及guest。**本轮只改六份文档，不改产品/测试、不构建或上设备。** 第1～39节保原字节，第39节的候选未合是当时状态；新结论不回填旧失败或转换测试身份。

### 40.1 已合相依组及限定资格

| 原source / 构建与执行 | 实际结果 | 边界 |
| --- | --- | --- |
| a3264f14 / native-local-condition-build-01、canonical-condition-minimum-01 | 四目标translator、swift_test、swift_ir_test、swift_cases_test成功；owner2910＋decode26＋flags57＝三例2993断言通过 | merge114为完整03窗口＋incoming独证＋真实owner；c077/02未采用。2993不是owner一例，不包含HIR lifetime |
| a1ff4964 / native-tso-pstate-build/owner-01 | translator＋swift_ir_test两目标成功，owner一例2937通过 | merge115只补普通StoreMemoryTSO保NZCV；仍为fault/guest观察者，不推广到load/atomic或四目标资格 |
| cee10355 / native-logical-pstate-build/owner-01 | 同两目标成功，owner一例2993通过 | merge116真实S defined/zero-CV与legacy Clear/Save已合；此处2993与a326三例总和独立。原guestgpr-finish异常不回填成功 |
| 当前861ab086 / native-pinned-w-build-02、owner-01 | 两目标成功，原owner一例3024全部通过；真实Low32唯一VOID StoreTSO保view/use、ActualPinnedW不再对VOID做size查询 | merge118保已有ALU＋4B授予写口；build01夹具API编译失败仍保原记录。当前不是四目标/全部case或最终质量重跑 |

原依据：[a326构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-local-condition-build-01/build.json)、[三例执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/canonical-condition-minimum-01/execution.json)、[TSO构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-tso-pstate-build-01/build.json)、[2937断言](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-tso-pstate-owner-01/execution.json)、[logical构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-logical-pstate-build-01/build.json)、[2993单例](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-logical-pstate-owner-01/execution.json)、[当前构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-pinned-w-build-02/build.json)及[当前3024断言](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-pinned-w-owner-01/execution.json)。各执行及namespace已结束；后修不改变原receipt。

### 40.2 新确认合同与真实成本

原flags pass已闭合primary/发表→condition全窗口，含AdvancePC后writer/fault及更早发表后、最终arithmetic前的fault，首次新发表前的operand fault仍可合法。成功时同RewriteContext同步Save/CondSet交BranchOnly/Local；未证明保canonical。最终prefix先meet、后核key/numeric完整incoming mask、再Observe，原极性/tuple/consumer窗口仍独证；不再把候选缺口列为当前未修。

普通StoreMemoryTSO真实RCpc/RCpcImm、非S地址/对齐检查、DMB＋STR保PSTATE；这是硬件clobber属性，**不取消它的fault/guest观察属性**。逻辑producer另按实际S form/demand交`PstateWrite{defined,zero}`，动态位与硬件CV零分开，只有真实S域可授`clear∩CV`；无S OR/XOR、Covered/Disabled/ConstantFlagsOnly等不由opcode获资格。legacy Clear只消费已证当前zero、GenericSave核同source；Meet交集、新writer覆盖、clobber/替换性恢复失效。ordinary SaveResult仍撤新requested资格，旧zero不能证明新动态N/Z。

consumer结果VOID不是非法源，也不具PinnedW资格。Low32的唯一store用途必须保留，W producer资格只沿现有NativeGPRConsumerRoles产生；删重复eager size查询即可，不需要新类型表/过滤pass或手写Map。具体职责归原[文件表5.1.15](middle-pipeline-file-layout-2026-10-28.md#5115-实际effectvoid角色及冷publication继续收敛在原写口)。

没有新增pass/N表/manager，仍有真实额外费用：正常64位布局RawPstate 32→40B、LocalPstateEdge 48→56B，原snapshot capacity每项多8B，各prefix实例及原visit中的mask/shape、完整窗口查询收费。`PstateWrite`只是瞬时值。**这组纠正实际合同，不构成compile提速证据**，后续成本账不能将“原visit复用”写成零成本。

### 40.3 当前失败已转到同owner metadata冷恢复后的publication

[原pinned-w-startup-r1执行](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/one-guest-adapter-root-staging-fix-r2/runs/pinned-w-startup-r1/execution.json)记录Block CoreMark exit1、CRC空；失败前1462块/35904IR，不是完整负载覆盖或编译成绩。原目标generation已不存在、本地children已wait，errors为空；旧gpr-finish CHECK已越过。

实际PC0x2197cd先在producer20经`ReadNativeZeroBranchHome`有效但无home，返回合法MetadataCohortRejected；同owner结束旧借用/回滚、恢复actual基线、撤metadata并fresh live/Describe/原RA后进入第二轮。随后point9 SetHostGPR Generic/body=1在`RealizeNativeGPRPublication`返回StatePolicyRequired，metadata realize尚未运行。**重建已进入不等于整条冷路成功，前一次optional miss也不自动意味着proof错误。** 原[stderr](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/one-guest-adapter-root-staging-fix-r2/runs/pinned-w-startup-r1/device/sample/stderr.txt)保精确相序。

当前source0的role2为WholeValue，不能仅因缺residence判失败；journal回调存在，bodyless Get32拒口不适用。高零/同home证明失败在真实body路径通常只放弃elide，不能据此放宽证明。静态借用相序复核尚未确认遗漏；actual/raw身份/type/view、BypassInputTransport、fresh binding及kind/reload/home的具体拒口仍未知，**不先定stale Map、悬空借用或宽度根因**。

下一次只在原失败出口记录具体guard及相应source/raw/实际绑定，不另走全body；由精确证据修原realization或恢复写口，并在现有owner补最少冷路正负见证。不禁ZeroTarget/native、不放home/高零/位置proof、不扩metadata预算、不消费后走死Inst保底。完整Function及独立FPR合同保持。

### 40.4 实施顺序与完整compile方向

1. 当前冷publication的定点定位→原写口修复→相依owning构建＋最少原owner→原失败guest基本执行/CRC。已合flags组不重复立项，只有改动frontend/pass才重跑对应拥有者的例。
2. 基本执行恢复后，清所有临时阶段/打印/body/两PC快照。已有十文件retirement包未合，后加constraints.cpp异常诊断也必须退出；按最新hunk清，不能整文件盖掉新logical合同。
3. 冻结普通版本，完成新的Block-SVM、Block-FEX、Function-FEX、Function-SVM四点。同轮fresh固定FEX为分母；旧失败不补点，失败前Translate、断言数、PC推进和旧47ms都不提供新比值。
4. 完整结果仍远离目标，则改M1构造/首解释/反馈、M2/M3 actual候选/窗口/state/实际位置及M4资源/请求触达的大算法。正确性组不替代真实退工，不继续已否决codec/窗口微调，也不把合visit当数量级收益。

固定FEX参照仍为早定目标、source/kill/destination顺序、同IR物理参数及compiler/arena复用，见[主方案3](spill-and-compile-root-cause-plan-2026-10-28.md#3-对照-fex缩短决策链保留自己的合同)。SVM完整Function、fault/observer和独立proof保留；source miss后原链、metadata整scope重建、Function失败Block重解码、初始化/发布/最后结束全计Translate。中间成组最少检查，稳定后集中原6.3/6.4，**最终质量不降、独立Block及完整Function分别≤2×同轮fresh固定FEX；当前未达标，也没有源码级上限保证。**

主方案[5.3.12.19](spill-and-compile-root-cause-plan-2026-10-28.md#canonical-raw-flags-obligations)、[5.3.12.20](spill-and-compile-root-cause-plan-2026-10-28.md#actual-effects-cold-publication)、[7.1/7.2](spill-and-compile-root-cause-plan-2026-10-28.md#71-当前代码状态)、逐源[5.1.15](middle-pipeline-file-layout-2026-10-28.md#5115-实际effectvoid角色及冷publication继续收敛在原写口)、IR[5.3.14](ir-representation-redesign-2026-10-28.md#5314-实际写位与consumer角色不由语义opcode或结果类型代证)及目标[7.9](target-aware-middle-pipeline-design-2026-10-28.md#79-后端实际kernel属性不扩成decode承诺)同步。[本轮边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-15-actual-flags-cold-publication-20261101/start.json)及[文档核验](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-15-actual-flags-cold-publication-20261101/validation.json)保源/索引和历史冻结；未stage/commit/push。

<a id="native-ready-direction-latest"></a>

## 41. 基本执行已恢复，普通完整方向仍远超两倍门

修订16按原receipt同步：37c811d1为语义/基本guest身份，e0d6e7e8为最新ordinary四点，当前95bd38e9仅新增unit/attempt归因，已开始分解、整组结论待原receipt。本包只改外部六文档，根节点最终接线；第1～40节、旧21正文和最终6.3/6.4保原字节。

### 41.1 候选、binding与kernel消费在原写口闭合

any GPRUseHome可返回inferred候选；preserved/whole placement仍可能决定不同Value位置。Whole→Value只在候选width、实际W/X bits、GPR/no-reload及home吻合时提交，合法miss清residence保原Whole实际copy；explicit Value和坏身份/invalid继续严格。真实RA回归先证明candidate≠actual，不手写Map，再核strict负例、Whole copy、fresh GPR/scratch及物理冻结。Value可pinned=false，publication改借原ScalarX按role读取，R/X/W原Whole ASSERT及MEM物化/捕获合同保持；正常一query，候选miss多一次Whole解析。

37c8 translator＋swift_ir_test两目标成功，原owner1 case/3162断言通过，原Block guest publication-role-startup-r1 exit0/CRC0x4983。不是四目标/全套或最终质量重跑。[37c8两目标构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-publication-role-build-01/build.json)、[37c8原owner](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-publication-role-owner-01/execution.json)、[原Block guest恢复](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/one-guest-adapter-root-staging-fix-r2/runs/publication-role-startup-r1/execution.json)

### 41.2 诊断退出后的有效普通四点

13文件临时定位已退、ordinary catch保留；e0d6 translator-only成功，四新guest及私有FEX服务/子进程关闭，source/index独立前后不变。

| Mode | SVM inclusive Translate / ms | fresh固定FEX CompileBlock / ms | 比值 |
| --- | ---: | ---: | ---: |
| Block | 2066.780070 | 46.623802 | 44.32886× |
| Function | 1099.290002 | 47.296979 | 23.24229× |

普通Block stderr仅4行svm-prof；Function保6条原「[physical-final] Check failed: "!loop_hoist_body_entry"」后最终CRC通过，原失败/Block重解码fallback已计总时间。CRC不能掩盖该限制，505 Function units/99 Block units不是native成功覆盖，也不由这6条归因Block other。

FEX分母为单initial-context最终CompileBlock累计，SVM端点为inclusive Translate，差异不抹平。各mode一对开发方向、均CRC0x4983；ordinary NativeReady覆盖仍unobserved，非最终质量/七对采用或完整任务生命周期。旧失败部分累计、旧47ms与当前结果不互换，未授本组单机制收益。[统一检查点](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-ready-checkpoint.json)、[普通translator构建](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-ordinary-ready-build-02/build.json)、[完整四点原账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-development-direction-adapter-r3-20261101/prepared-native-ready-e0d6-r3/compile-gap-summary.json)、[四点执行与收口](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-development-direction-adapter-r3-20261101/prepared-native-ready-e0d6-r3/execution.json)

### 41.3 先分路径归因，再退出最大算法

other B1451.756/F320.287ms含native Preclose/Describe/state/独证/资源准备；checked Complete归codegen。Function可能有大量source miss，现无actual路径计数，不能按两个桶或约0.6/0.53ms每请求认定固定初始化。当前95bd仅用现PerfStats2的12段unit/attempt范围及少量成功计数作单Block＋完整Function归因；不转授e0d6分母或37c8测试。

[两项静态算法census](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/native-normal-request-algorithm-census-01/README.md)只提供两项静态候选：resident Get的R×(N+U)用途发现、RawDirect的Q×G＋Qmiss×raw stream恢复，均列原合同/失效/整段退出点，非计时根因或收益保证。不建第二N/U图/Passed，不以合visit、压字段或关闭proof/Function/优化替代主要减工。分段后选最大真实算法，完整相依组最少语义与少量ordinary方向，稳定后集中原最终门。[归因接线](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/merge-124-native-phase-attribution/source.patch)、[归因版构建记录](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/integration-01/native-phase-attribution-build-01/build.json)


<a id="performance-route-correction-latest"></a>

## 42. 对照最后用户定稿：保机器合同，纠正载体前提和净成本交付

### 42.1 基准与合理补强

基准为2026-10-31用户“根据最新结论修改方案文档”后形成的546行版本，主文SHA0f7f1d72…cab29，八份恢复副本与当时校验匹配。旧版已有native、typed-current、193同源kernel及true→false重发，保sealed语义IR和Allocation/Selection至Flush、重发重新绑定。当前选用同Store Semantic→BackendVirtual→PhysicalReady，实际machine uses/defs供原两范围RA，稳定operand供kernel和重发。

真实source/use/def/birth先于machine live，raw退休与机器读取分开、whole REF与shared-write分开、capture先于真实clobber、完整Function lease/CFG等是正确补强，不能因恢复旧方案而撤回。最终质量、各scope≤2×fresh FEX及停止规则均与旧版逐字相同；不能将回退解释为后来加严最终门。

### 42.2 必须纠正的设计与执行判断

| 发现 | 修正后的要求 |
| --- | --- |
| 以“稀疏Selection只有两种结局”证明必须换载体 | 撤回排他论证。继续读Inst不等于必须跑完整Describe/Complete；同Store是当前实现选择，比较退出/保留/新增及失败成本，不声称唯一最优 |
| 一次完整收益要等全部机器合同/读者迁完 | 保必要相依接口，在代表性真实单元上闭合完整纵向包；首次可运行即反馈compile方向，扩family前决定算法，不等全部ISA |
| 把乘积扫描当作“保正确性” | 保原独立合同；actual/raw/definition等关系分别有完整域、唯一owner和最后reader。允许净收益成立的索引，既不信producer Passed，也不因禁止任何表而每query扫scope |
| 后端主线长期压住M1/M4 | 共同列真实出生/首次解释、实际决定/来源证明、初始化/结束的净工作；有限直接构造、少字段或少API不承担整个目标 |
| 当前状态与迁移历史混杂 | 主7.1为唯一当前状态、7.2为待恢复执行；旧C批次/归因进行中按历史source理解，不重复安排已迁读口 |

资源Prepare/Freeze、controls及双resolve有真实失败/寿命依赖；减少它们须交代最终结果承载和全部reader，不能直接删第二遍，或另存全N Current/descriptor却不计成本。

### 42.3 新证据与不能外推的结论

95bd/3625两轮归因已完成；GPR B695.636/F103.768ms约占native准入89%，归因版NativeReady为1275Block及77Function＋83Block。metadata仅B一次0.202ms、F零，checked费用仍明显。[原阶段归因](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/native-phase-95bd-summary-01/summary.json)、[原准入归因](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-default-startup-repair-20261101/native-admission-3625-summary-01/summary.json)不是ordinary/FEX时间，路径覆盖不转授e0d6。

最新ordinary e0d6为B2066.780070/F1099.290002ms，同轮FEX比值44.33×/23.24×。decode＋RPO＋opt约200.736/257.096ms；若保持不变，已超过约93.248/94.594ms的2F总门。单修GPR不能证明完整目标有解。[原普通账](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/native-development-direction-adapter-r3-20261101/prepared-native-ready-e0d6-r3/compile-gap-summary.json)保单点方向和端点差异，不作精确设计因果分摊。

当前8cda已合consumer侧actual-use核验A，退17个临时timer及4个成功scalar；两目标构建成功。原三case2过1新增fixture前提失败，3714断言中3713过；负例未到stale-count，02仅外部冻结未应用。A只退出每Get全scope实际consumer发现及重复绑定的一段，RawDirect与完整provenance/lease/fault/CFG仍收费，无新ordinary收益，不能将695ms全部认作A可省。

### 42.4 本次文档决定

主5.3.11纠正载体依据，5.8.3给已测成本与算法退出要求，6.1更新当前总账尺度、6.2要求首次可运行纵向反馈，7.1/7.2统一当前身份及待恢复工作；结构、IR、target-aware及历史入口同步。原正文1～41、最终6.3/6.4、停止规则和旧21编号正文不改。只修文档，不恢复实现，不产生构建、设备或性能资格。[本轮边界](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-17-net-work-correction-20261102/start.json)、[核验](/Volumes/Data2/swiftvm-middle-preserved-20261025/middle-rebuild-implementation-20261029/canonical-native-service-20261031/c7-native-flags-20261101/plan-revision-17-net-work-correction-20261102/validation.json)。
