# P2 HomeFact 毛池分解审计

## 0. 结论先行

**裁决：NO-GO，不进入 `SVM_RA_HOME_FACT` 机制实现。**

按生产默认 region 形态、`RE=0` block entry 权重乘 `RE=1` region shape 的
统一口径，P2 的严格 logical-exact 池为：

| 语料 | host dynamic | R 类 transport | logical-exact | exact / host | 当前 `location==home` exact |
|---|---:|---:|---:|---:|---:|
| CoreMark | 191,429,710,665 | 20,976,387,565 | 324,517,261 | 0.169523% | **0** |
| sqlite | 519,723,835 | 54,750,079 | 882,237 | 0.169751% | **0** |
| zip7 | 297,296,326,126 | 60,083,340,200 | 4,118,650,696 | 1.385369% | **0** |
| smallpt | 144,229,574,607 | 8,501,101,064 | 769,410 | **0.000533%** | **0** |
| c-ray | 445,719,725,983 | 37,213,218,282 | 17,096,808 | **0.003836%** | **0** |

这里特意区分两种 exact：

- `logical-exact`：IN 中版本一致、定义支配 use、宽度 canonical；这是
  HomeFact 数据流能证明的同版本 transport。
- 设计 §5.1 真正允许直接判 trivial 的 exact 还要求
  `location == fixed home`。用现行 RA map 复核，五语料该项全部为 **0**。
  因而 P2 脱离 P0 的事务式 owner/tie，不具备直接零发射条件。

更决定性的是 rays 门：即使做不安全的反事实——忽略所有 public-entry，
只按内部 CFG 传播——smallpt/c-ray 也仅有 **0.040227% / 0.086692% host**，
远低于预注册的各自 0.5%。public-entry repair 再扣回后只剩严格值
0.000533% / 0.003836%。这不是实现技巧能补足的缺口。

CoreMark 冻结账 `5,861,995,080 / 58,863,042,249 = 9.958702%` 已逐字闭合；
冻结账的五类分解见 §3.1。结论不是“R 类毛池不存在”，而是其中绝大多数是
必要的新版本发布、observer 后重建或 partial write，当前 P2 证明只能吃到
很小一角。

---

## 1. 审计器与口径

### 1.1 analysis-only 插桩

临时插桩挂在 RA 完成、发码开始之前，仅在既有
`SVM_DENSITY_PROF=1 + SVM_RA_HOT_COALESCE_ALL=1` 时执行：

1. 对每个 `HIRFunction` 从 `GetHIRBlocksRPO()` 建只读 CFG；
2. 校验 block order id 唯一、successor/predecessor 双向对称；
3. 自建 dominator fixed point；
4. RPO 迭代 `home -> {reason, version token, def block}` 的 IN/OUT；
5. 对每条 Get/SetHostGPR 记录 strict/internal 原因和现行 RA 物理家；
6. 用 emitter `[svm-gap-op] bytes/4` 关联最终实际 transport 指令数。

采集期间为避免 sqlite/rays 被全 opcode 日志放大，临时把 gap 输出收窄为
GetHostGPR/SetHostGPR；这只改打印条件，不进 IR/RA/emitter 决策。

所有临时源码、header 和分析脚本已机械删除；最终源码中搜索
`homefact_census|p2_homefact` 无命中。

### 1.2 两套账为什么同时报告

本轮用两套互补口径：

- **join（裁决口径）**：`RE=0` 每个 guest block 的 entries × `RE=1`
  region shape。region 内部入口不经过 region root counter，因此这是动态账
  的完整口径。
- **region-direct（冻结账复核）**：`RE=1` root entries × `RE=1` shape。
  它与 move 归因报告中 R 类 9.958702% 的原口径完全相同，因此用于五类
  digit-exact 拆分；但不拿它替代 rays 的 join 裁决。

`RE=0` 不参与事实传播，只提供 entry 权重；HomeFact 分类全部来自生产
`SVM_REGION_EDGES=1` 的 HIRFunction。

### 1.3 五类互斥规则

| 类 | 规则 |
|---|---|
| exact-match | full U32/U64 home，版本 token 完全相同且定义支配 use；partial 不得进入 |
| multi-pred mismatch | 所有普通内部 predecessor OUT 精确交集失败；不建 phi |
| external-entry | public/external/canonical entry 不能信任前驱事实 |
| observer clear | fault/helper/signal/control observer 清空后的 read；以及 SetHost 首次发布一个不同 full version |
| partial-write | U8/U16、非零 offset、未知 SetHost；U32 按 x86 语义视作完整定义且 X 高半为零 |

为了让 SetHost 半边也被五类穷尽，“发布不同完整版本”计入
`observer/new-version publication`。它不是说 SetHost 之前一定发生了一个
fault observer，而是说目标 home 尚不持有同一版本，这条发布不能由
HomeFact 删除。报告中会把 Get/Set 半边分开，避免把必要发布误当成
observer 清空损失。

---

## 2. CFG 前置与 external-entry 实测

### 2.1 证明洞 #4：生产 CFG 前置

源码结论：

- `runtime.cpp:825-839` 在生产 pipeline 中无条件 `ComputeRPO()` 并
  `IdByRPO()`；RPO 是现成前置。
- `hir_builder.cpp:257-305` 的 `ComputeRPO()` 只做 successor DFS/RPO，
  不计算 dominance。
- `cfg_analysis_pass.cpp:237-246` 明确 `CFGAnalysisPass::Run` 只有
  `main_case.cpp` 测试调用，不在生产 PassPipeline。

因此设计证明洞 #4 的答案是：**RPO 已具备，dominance 不具备**。本次探针
按设计允许的第二条路径自建最小 dominance，并 fail-closed 校验整个
function；实测没有一个 function 因 CFG 结构失败：

| 语料 | HIR roots | blocks | external predecessor edges | invalid roots | asymmetric edge | duplicate order id |
|---|---:|---:|---:|---:|---:|---:|
| CoreMark | 623 | 3,321 | 623 | 0 | 0 | 0 |
| sqlite | 4,858 | 26,363 | 4,858 | 0 | 0 | 0 |
| zip7 | 2,388 | 10,147 | 2,388 | 0 | 0 | 0 |
| smallpt | 628 | 3,458 | 628 | 0 | 0 | 0 |
| c-ray | 3,393 | 14,318 | 3,393 | 0 | 0 | 0 |

结构校验缺失面为 **0 function / 57,607 blocks**。所以 CFG 完整性不是
本轮毛池被吃掉的主因；若将来实现，仍需把这套校验变成正式、fail-loud
前置，不能复用 tests-only CFGAnalysisPass 的隐含结果。

### 2.2 每个 decoded block 都是 public entry

`runtime.cpp:961-978` 会遍历 `GetHIRBlocksRPO()`，对每个非空 decoded
block 同时 `PublishLinkTarget` 和 `PushCodeCache`。external link、RSB、
code miss 都可直接落到 unit 内任意 block label。

因此 strict P2 不能假设一个 block 只从 CFG predecessor 到达。没有双入口
ABI 时，internal fact 在 public entry 必须 reconcile；这正是设计中把跨
direct-link fast fact 推迟到 P3 的原因。

---

## 3. 五类分解

### 3.1 CoreMark 冻结 9.958702% 毛池（region-direct）

分母与旧 move 归因账逐字一致：

`5,861,995,080 / 58,863,042,249 = 9.958702% host`

| 类 | dynamic insn | host % | R 类 % |
|---|---:|---:|---:|
| exact-match（logical） | 226,950,145 | 0.385556% | 3.871551% |
| multi-pred mismatch | 81,533,285 | 0.138514% | 1.390879% |
| external-entry | 719,426,019 | 1.222203% | 12.272716% |
| observer/new-version | 3,635,170,767 | 6.175642% | 62.012518% |
| partial-write | 1,198,914,864 | 2.036787% | 20.452335% |
| **合计** | **5,861,995,080** | **9.958702%** | **100.000000%** |

忽略 public-entry 的 internal 反事实 exact 为 231,300,278
(0.392947% host)，只比 strict 多 4,350,133。按每消一条 transport 至少需
一条 canonical entry repair 的机械下界，新增 repair 也是 4,350,133，
cross-block 部分净值不大于 0；剩下的 strict logical-exact 为
226,950,145。现行 RA 对这些 exact 的 `location==home` 命中仍为 0。

### 3.2 生产 join：CoreMark / sqlite / zip7

| 语料 | 类 | dynamic insn | host % | R 类 % |
|---|---|---:|---:|---:|
| CoreMark | exact | 324,517,261 | 0.169523% | 1.547% |
|  | multi-pred | 593,167,107 | 0.309862% | 2.828% |
|  | external | 1,409,746,268 | 0.736430% | 6.721% |
|  | observer/new-version | 13,671,990,212 | 7.142042% | 65.178% |
|  | partial | 4,976,966,717 | 2.599893% | 23.727% |
| sqlite | exact | 882,237 | 0.169751% | 1.611% |
|  | multi-pred | 302,424 | 0.058189% | 0.552% |
|  | external | 1,979,708 | 0.380915% | 3.616% |
|  | observer/new-version | 49,363,478 | 9.498021% | 90.161% |
|  | partial | 2,222,232 | 0.427579% | 4.059% |
| zip7 | exact | 4,118,650,696 | 1.385369% | 6.855% |
|  | multi-pred | 4,714,503,328 | 1.585793% | 7.847% |
|  | external | 5,115,869,743 | 1.720798% | 8.515% |
|  | observer/new-version | 44,379,640,975 | 14.927746% | 73.863% |
|  | partial | 1,754,675,458 | 0.590211% | 2.920% |

zip7 有足够大的 logical-exact，但 rays 是预注册硬门；且 zip7 exact 的
现行物理家命中同样为 0，不能绕开 P0 事务式 owner/tie 单独兑现。

### 3.3 Get/Set 半边：为什么 observer 是主桶

生产 join 的代表性拆分：

| 语料 | Get exact | Get observer | Set exact | Set observer/new-version | Set partial |
|---|---:|---:|---:|---:|---:|
| CoreMark | 324,517,260 | 3,655,072,948 | 1 | 10,016,917,264 | 3,068,881,234 |
| sqlite | 882,230 | 5,135,348 | 7 | 44,228,130 | 1,027,032 |
| zip7 | 4,118,650,695 | 9,734,502,666 | 1 | 34,645,138,309 | 78,135,813 |
| smallpt | 769,409 | 394,997,681 | 1 | 7,803,037,689 | 139,563,907 |
| c-ray | 17,096,360 | 1,544,951,637 | 448 | 33,326,840,934 | 984,246,958 |

结论：R 毛池的主体不是“同一值重复搬一次”，而是完整新版本首次发布。
P2 的版本 fact 可以证明重复发布，但不能删除架构 home 尚未持有新版本时的
第一次 SetHost。fault/helper 前继续要求 canonical，又进一步缩短可复用窗口。

---

## 4. rays 门与净值

### 4.1 smallpt

| 项 | dynamic insn | host % |
|---|---:|---:|
| strict logical-exact | 769,410 | **0.000533%** |
| 忽略 public entry 的 internal exact | 58,019,626 | 0.040227% |
| canonical/public-entry repair 下界 | 57,250,216 | 0.039694% |
| repair 后上界 | 769,410 | **0.000533%** |
| 当前 `location==home` 可直接零发射 | 0 | **0.000000%** |

### 4.2 c-ray

| 项 | dynamic insn | host % |
|---|---:|---:|
| strict logical-exact | 17,096,808 | **0.003836%** |
| 忽略 public entry 的 internal exact | 386,404,727 | 0.086692% |
| canonical/public-entry repair 下界 | 369,307,919 | 0.082856% |
| repair 后上界 | 17,096,808 | **0.003836%** |
| 当前 `location==home` 可直接零发射 | 0 | **0.000000%** |

两条 rays 即使把 public-entry 安全约束全部拿掉，也分别离 0.5% 门差
12.4 倍和 5.8 倍；严格值差 937 倍和 130 倍。故 P3 双入口也不足以把
P2 本身救过门，更不应为此先支付 cache/SMC ABI 成本。

---

## 5. observer opcode 全表审计（证明洞 #5）

IR 全表共 174 opcode。本次不是手写一个小白名单，而是先取三套现有
proof-bearing predicate 的并集：

- `UniformStoreSinkPass::IsFaultOrObservationPoint`
  (`uniform_store_sink_pass.cpp:120-171`)；
- `IsPinnedCoalesceObserver`
  (`register_alloc_coalesce_gpr.cpp:77-103`)；
- `IsHostCoalesceObserver`
  (`translator_mem.cpp:120-147`)。

然后逐个 `ir.inc` opcode 分类。最终为：

1. **全 fact clear，29 个**：
   `SaveFlags`, `LoadMemory`, `StoreMemory`, `LoadMemoryTSO`,
   `StoreMemoryTSO`, `MemoryCopy`, `MemoryCopyTSO`, `CompareAndSwap`,
   `CompareAndSwap128`, `CheckMemoryAlignment`, `AtomicExchange`,
   `AtomicFetchAdd`, `AtomicRMW`, `CallLambda`, `CallLocation`,
   `CallDynamic`, `X87Op`, `Sse42Str`, `GetUniformAddress`,
   `UniformBarrier`, `GetHostFPR`, `SetHostFPR`, `SetLocation`,
   `GetLocation`, `Goto`, `NotGoto`, `BindLabel`, `PushRSB`, `PopRSB`。
2. **逐 home transfer，2 个**：`GetHostGPR`, `SetHostGPR`；完整写更新版本，
   partial 写降级，不能粗暴清全部 home。
3. **block terminal，无独立 opcode**：successor 为空时清全部 fact；
   public/direct-link/RSB/code-miss 由每 block external-entry 规则覆盖。
4. **剩余 143 个 preserve**：纯整数/向量 ALU、flags 值运算、local/uniform
   普通读写、`MemoryBarrierTSO` 等；它们不 fault、不调用 opaque helper、
   不直接改变 fixed-home 架构值。

SMC cold 与 signal cold stub 不是独立 IR opcode：guest memory op 已在 clear
集合，失效后经 public canonical entry 重入；signal observer 经 terminal/
cold edge 覆盖。custom helper 统一落入 CallLambda/CallDynamic，X87 和
Sse42Str 独立覆盖。

全表审计还暴露出现有 predicate 漂移：UniformStoreSink 包含控制、
Get/SetHost、location；Pinned/Host coalesce 额外包含 SaveFlags/Sse42Str，
却不含前一组控制项。探针使用并集，因此本次没有因“小名单不同步”虚增
exact。若未来实现 P2，应抽成唯一共享 observer contract；否则证明洞 #5
仍会重新出现。

---

## 6. 正确性、撤桩与零发码影响证明

所有采样都显式 `env -u SVM_EXEC_PROF -u SVM_JIT_CACHE`。原始数据位于：

`/private/tmp/p2-homefact-20260812/`

关键产物：`coremark.json`, `sqlite.json`, `zip7.json`, `smallpt.json`,
`cray.json`，以及每臂的 `hot.log/stdout.log/stderr.log/rc`。

运行结果：

- CoreMark rc=0，五个 CRC 为 e9f5/e714/1fd7/8e3a/25b5，
  `Correct operation validated`；
- sqlite RE0/RE1 rc=0，均有 TOTAL；
- zip7 RE0/RE1 rc=0，均有 `Tot:`；
- smallpt RE0/RE1 rc=0，PPM SHA-256 均为
  `3603641108884967db932feea1b977dbc69754b6c358dfb2e76f5d3d0bc8b836`；
- c-ray RE0/RE1 rc=0；PNG metadata 不同，但转成无 metadata BMP 后
  SHA-256 均为
  `a2e4d50b8bf3a3b7f289300b030c758c2ba1f9f0ca780c891a07bff653d8c11c`。

撤桩后：

- `cmake --build build -j8` rc=0；只有仓内既存 warning（`#pragma once`
  in cpp、vendored iterator/intrusive-list、logging macro redefine），无
  probe warning；
- CoreMark 再跑两次均 rc=0、CRC/validation 正确；
- probe run 与 clean run 的 top-10/top-20/top-50/top-100 公共热 PC：
  `host_static/move_static/spill_static/host_bytes` **0 diff**；
- 两次 clean run 自身也有同样的 13–14 个极冷 coverage/version shape
  漂移（多数 entries=0，少数 entries=1–3），说明全公共 PC 的冷项差异是
  JIT 覆盖噪声而非 probe 发码影响。probe-clean 与 clean-clean 的
  top-500 都同为 2 个冷 diff；生产热形状逐点一致。

最终工作区只保留本报告；临时 header、分析脚本、runtime hook 和 gap
打印收窄均已删除。

---

## 7. 最终建议与 reopen 条件

### 7.1 当前建议

1. **P2 `SVM_RA_HOME_FACT`：NO-GO，不实施。** rays 双双远低于 0.5%。
2. **不要拿 zip7 的 1.385% logical-exact 绕过 rays 预注册门。** 当前
   `location==home` 为 0，P2 单独并不能零发射；依赖 P0 后才有资格重算。
3. **P3 双入口不因本结果重开。** 即使免费去掉全部 public-entry，rays
   仍不过门。
4. observer 主桶不是可删池：首次发布新版本与 fault/helper canonical
   边界是现有正确性合同，不能把它们改名为 HomeFact 优化。

### 7.2 量化 reopen 条件

只有同时出现以下新事实才值得重新 census：

- P0 事务式 owner 已能让 logical-exact 的 `location==home` 大量非零，
  且不增加 relocation/spill/edge repair；
- rays 的严格 exact 实测至少提升到各自 0.5% host，而不是从 zip7 外推；
- 或 fault-context recipe / 双入口 ABI 已因其他独立收益落地，使 observer
  clear/public repair 的成本不是由 P2 单独承担。

在这些条件之前，设计 §8.2 门 1 已完成，门 3 明确失败；P2 在当前证明
体系与当前语料分布下封存。
