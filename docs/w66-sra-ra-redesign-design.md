# SRA-capable RA redesign 设计研究

## 0. 结论先行

**结论：设计可成立，但不能把“全 pin 再开一次”作为一个大开关直接实现。推荐拆成三个可独立证伪、逐层 fail-closed 的片：**

1. **P0：不可变 home component + 事务式决策基座**。把目前分散在 fixed-class、W-α write/read、width-chain owner 中的决定统一为一次 `HomeTransaction`；这一步只改变 RA 内部表示，不改变发码。
2. **P1：unit-local fixed-home tail relocation**。冲突时不再把新值整段甩回普通池，而是在 incoming producer 前把旧 fixed-home owner 的余尾搬到一个普通池家；只允许“一条搬运换掉多条 transport”，且候选峰值池占用不得高于当前 fail-closed baseline。
3. **P2：region-local home-version dataflow**。只在同一个 HIRFunction 的普通内部边上传播“home 当前等于哪个已提交值”的事实，使入口 `GetHostGPR`/出口 `SetHostGPR` 可判 trivial；所有 public/external entry、fault/helper/signal/SMC 边界仍以 canonical State/fixed home 为准。

跨 unit direct-link 事实不是 P2 的自然延伸，而是 **P3：canonical/public entry + SRA fast entry 双入口 ABI**。它涉及 link metadata、disk-cache 格式和 SMC/QSBR 发布协议，必须独立开关、独立验收；P0–P2 不依赖它。

当前资料足以完成 P0–P2 的设计，但不足以直接批准 P1 实现：缺少 `fixed conflict × incumbent tail length × pool headroom × observer distance` 的联合直方图。现有 pool10/12 数据只证明“容量灾难基本消失”，不能证明每个搬迁切点都有空闲家。因此首个实施批必须是 **analysis-only selector dry-run**；若 `elided transport - relocation - added spill/reload` 在生产 region 态不能为正，P1 当场 NO-GO。

这不是旧 full-pin 的放宽版：旧机制在冲突时按“整段成功/整段回退”处理，CoreMark/SQLite 分别制造 `+3.526066%/+5.436013%` move；本设计的核心不变量是 **每次局部决策相对已验证 baseline 单调不劣**，任何局部净增、spill 增量、observer 不可证都会逐 unit 恢复 baseline。

---

## 1. 现状证据与问题定义

### 1.1 两条账已经同根

生产 region 默认态 CoreMark：

| 项 | 动态条数 | 占 host |
|---|---:|---:|
| host dynamic | 58,863,042,273 | 100% |
| move/bridge | 20,297,836,502 | 34.483159% |
| fixed-home transport（R 类） | 5,861,994,920 | 9.958702% |
| 其中 SetHost | 3,723,080,927 | 6.324989% |
| 其中 GetHost | 2,138,913,993 | 3.633713% |

出处为 `docs/w66-move-attribution-audit.md` §1–§3。该报告已经证明 W-α 的 guard 是 block-local 的：入口没有可证明的 home history 时，Get/Set 只能保守发 transport。

Linux identity 的 Phase C 又排除了“只是池太小”这一单因解释：

| 语料（region 默认态） | pool histogram | 新 spill unit | move 变化 |
|---|---:|---:|---:|
| CoreMark | `9:2,10:621` | `0→2` | `+710,299,064`（+3.526066%） |
| SQLite | `9:28,10:4831` | `2→28` | `+4,756,922`（+5.436013%） |

`TryAllocateFixedGPR()` 的现行行为与账吻合：fixed home 有 active owner 或 fixed clobber 时，incoming candidate 整区间回退普通 value class，保留 SetHost copy；已经分配的 incumbent 绝不换家（`register_alloc_pass.cpp:524-557`）。这使冲突局部的一次失败扩散成整段 transport。

### 1.2 pool10/12 能证明什么，不能证明什么

`docs/w68-register-reclaim-audit.md` 的实测/投影：

| pool | CoreMark spill-trigger units / excess | SQLite spill-trigger units / excess | 证据性质 |
|---|---:|---:|---|
| 10 | `2 / 6` | `28 / 86` | Phase C 实测 |
| 11 | `2 / 4` | `24 / 54` | max-live 投影 |
| 12 | `0 / 0` ordinary-pressure 下界 | `2 / 8` | max-live 投影 |

CoreMark L3 max-live 直方图尾部只有 `7:7,9:2`；SQLite 为 `7:12,8:4,9:22,12:2`。这足以说明 pool7 式容量灾难不会原样重现，也说明绝大多数 unit 有局部调整空间；但它没有包含 scratch reserve、fixed conflict 同时发生位置与 relocation cut 的联合分布。pool12 也只是上界投影，不是生产中已经安全获得的池。

所以本设计 **只以 pool10 为硬事实**：不得假设 pool11/12，候选不得提高 baseline 峰值池需求。

### 1.3 当前验证模型的缺口

`LinearScanAllocator::Verify()` 目前验证两类事实（`register_alloc_pass.cpp:195-258`）：

- 无 spill 时，逐 instruction 检查 dirty mask 剩余容量是否满足 scratch budget；
- 有 spill 时，逐 instruction `CheckInstr()`，并把 terminal reload 纳入最后 instruction 的 headroom。

`RegAlloc::Map` 则是一 SSA 一个位置；它表达不了“同一个 SSA 在 `[start,cut)` 位于 fixed home、在 `[cut,end]` 位于 pool”的事实。若只在 emitter 临时插一条 mov，Verify 看不到 split 后的真实活跃集，等价于绕过 scratch contract，不能接受。

---

## 2. 最小数据模型

### 2.1 不可变 HomeVersion / ComponentOwner

每个 architectural home 的每次完整写产生一个不可变版本：

```text
HomeVersion = {home_id, version_id, producer_id, width_fact, committed_at}
```

`width_fact` 至少区分：

- `ExactU64`；
- `WWriteHighZero`；
- `SignExtended(bits)`；
- `Partial(bits)` / `Unknown`。

一个 SSA component 第一次进入 home selector 时一次性获得：

```text
ComponentOwner = {root_value, home_version, chosen_home, proof_digest}
```

之后 W-α read/write、fixed-class 和 width-chain 只能查询它，不能分别改判。一个 SSA 向多个 fixed home 发布时，component 必须在创建点选择唯一 owner；其他 home 是显式 publication，不允许后来某轮 pass 偷换 owner。这直接兑现 width-chain v2 post-mortem 的前置。

### 2.2 分段位置，不改 IR

RA 内部增加：

```text
AllocationFragment = {value_id, begin, end, location, home_version}
Relocation = {cut_id, value_id, from, to, width}
```

关键限制：

- fragment 边界只能位于两条 IR instruction 之间；
- relocation 是显式 A64 register move，不使用隐式 VIXL scratch；
- emitter 所有 value lookup 改为 `ValueLocationAt(value, instruction_id)`；terminal 使用 block-end position；
- IR、fault PC、guest PC、observation point 均不重排。

这不是通用 live-range splitting。首片只允许一个形态：**incumbent fixed-home owner 的 non-architectural tail 从 fixed home 搬到普通 pool，incoming committed home version 占回 fixed home。** fixed-clobber、partial write、跨 helper/fault 的复杂切分全部回退。

### 2.3 HomeTransaction：write/read 原子提交

每个 block/home 建一笔事务，输入为：

- fixed reads；
- candidate full writes；
- immutable component owner；
- incumbent live fragment；
- fixed clobber/observer cuts；
- high-zero/width proof；
- baseline map 和 baseline emitted-cost estimate。

事务先在 shadow allocation 上同时决定：

1. incoming write 是否占 fixed home；
2. incumbent 是否在 producer 前 relocation；
3. 同一 instruction 内 read 是否可直接使用 fixed home；
4. SetHost/GetHost 哪些可以标 trivial；
5. relocation 后 dirty/scratch mask。

任一证明失败，整笔恢复原 `AllocGPR` baseline；write pass 的半成品不得暴露给 read pass。现有 width-chain v2 已有“保存 GPR 状态—冻结 owner—验证—恢复”的事务骨架，应扩展同一份快照，而不是再造并行 rollback：快照必须同时包含 mappings、fragments、relocations、read/write markers、width owners、dirty masks 和 home facts。

---

## 3. P1：fixed-home tail relocation

### 3.1 选择算法

当 incoming interval 想占 home `H`，但 `H` 正被 incumbent `Vold` 占用：

1. 保留现行 fail-closed 方案作为已验证 baseline：incoming 全区间进 pool，末端 SetHost。
2. 候选方案在 incoming producer 前的 cut：
   - `mov pool_tail, H` 保存 `Vold`；
   - `Vold` 后续 use 改查 pool fragment；
   - incoming producer 直接写 `H`；
   - 后续同版本 Get/Set 可归零。
3. 比较两个 cost vector，而非只比较 end：

```text
cost = relocation_moves
     + remaining_get_set_transport
     + added_spill_reload
     + helper_save_restore
     + canonical_entry_repairs
```

只有候选严格小于 baseline 才提交；相等也回退，避免 code-placement 无收益扰动。

pool 约束是本设计避免 starvation 的核心：baseline 本来就要给 incoming 一根 pool 家；候选只把这根资格转给 incumbent 的较短 tail。因此同一 cut 上 `candidate_peak_pool <= baseline_peak_pool`。若 baseline 的 incoming 与 incumbent tail 在时间上并不互斥、需要第二根 pool 家，则拒绝，不做 split。

### 3.2 fault/observer 证明

定义硬不变量：

> 在每个 guest-visible observation point，architectural fixed home 必须持有该点最近一次已提交的 HomeVersion；动态 pool fragment 只保存旧 SSA 的计算用途，不承担 guest architectural state。

对 relocation 的唯一批准时序：

```text
old home H committed
mov tail,H             // 非 faulting，保存旧 SSA tail
incoming producer      // 若成功，H 成为新 committed version
observer/faultable op  // 看到新 H
```

若 incoming producer 本身可能 fault，则只能对白名单证明“A64 fault 时 destination 未提交”的 opcode 使用；首片建议直接排除所有 faulting producer，避免把 ISA fault 原子性假设扩散到 RA。

在任何 fault/helper/signal/SetHost/UniformBarrier/外部出口之前：

- 若当前 architectural version 已在 fixed home，允许通过；
- 若只在 pool fragment，事务失败并恢复 baseline；
- 不依赖 cold fault recipe 重建。现有 `FaultEntry` 只有 host range、owner、recovery、guest PC，没有通用 value recipe；FAULT_CONTEXT_RECIPE 审计也已因 c-ray 仅 0.144281% 过不了门。因此“fault 后再补 home”不属于最小设计。

实际边界关系：

- **guest fault**：fault handler 转到 recovery/return-host，trampoline 保存 static descriptors；只要 fixed home canonical，pool 中旧 SSA 可丢弃。
- **SMC write-protect fault（priority 0）**：恢复同一 host ucontext、重执行原路径，寄存器片段保持；若 unit 随后失效，重新从 canonical entry 进入。
- **async guest signal**：`exit_request` poll 后走外部返回，static homes 保存进 State；不把 pool fragment带入 signal frame。
- **helper**：现有 helper dirty-mask 保存调用者活值。split 后 source/destination 均必须进入 cut 附近 dirty mask；fixed home 仍按 static descriptor 保存。若 helper save 数增加，cost/gate 拒绝候选。

### 3.3 与既有机制接口

| 机制 | 接口/约束 |
|---|---|
| W-α coalesce | 改为 HomeTransaction 的 read/write request；不再先写后读两轮各自落标记。原 observer/fixed-clobber/双侧交叉规则全部保留。 |
| width-chain / intwidth tie | 只提供 immutable owner 与 width fact；不得在 split 后重新认领 home。8/16-bit partial 永远不能提供 `WWriteHighZero`。 |
| dbc5491 level3 scratch | relocation 用明确的已分配 pool register，不调用 VIXL `AcquireX`。`CheckInstr` 必须看到 relocation cut 的 src+dst dirty；x16/x17 仍由 scratch contract 动态放行。 |
| x18 重跑 | 任一首轮 spill 会 `ReserveGPRForUnit(18)` + `ResetAllocations()` + 全量递归重跑（`register_alloc_verified.inc:53-70`）。所有 fragment/transaction/home fact 必须在 Reset 时清零并重新计算，禁止复用首轮计划。 |
| spill eviction | 先生成 current baseline（含现行 eviction/restart），再在最终 pool 图上试 relocation；relocation 不得触发新 forced-spill restart。 |
| emitter | 每条省略的 Get/Set 与每条 relocation 独立复证 owner/version/location；失败走 baseline 发码。若 RA 已提交但 emitter 发现内部不一致，应 ASSERT，而不是静默发半套。 |

---

## 4. Verify / RunVerified 扩展

### 4.1 Verify 新增七项

1. 每个 use position 恰有一个覆盖它的 fragment；无洞、无重叠双定义。
2. 同一物理寄存器的重叠 fragment 只能是同一 SSA/HomeVersion 的合法 alias。
3. 每条 relocation 的 source 在 cut 前有效、destination 在 cut 时空闲、width 语义一致。
4. relocation 与 instruction fixed-clobber、VIXL scratch allowed set 不相交。
5. 每个 observation point 的 architectural home version canonical。
6. 逐 instruction dirty mask、scratch budget、spill reload 与 terminal headroom 按 fragment 后的真实集合重算。
7. 候选 cost vector 严格优于保存的 baseline，且 spill defs/loads/stores、helper save、host bytes不增。

第 7 项看似“性能策略”而非正确性，但它是避免 Phase C “桥接造多于消”的验收不变量，应该与事务一起 fail-closed，而不是留给全程序 A/B 才发现。

### 4.2 RunVerified 发布顺序

建议顺序：

```text
allocate current fail-closed baseline
  -> x18 conditional rerun until pool graph stable
  -> Verify(baseline)
  -> clone baseline into shadow plan
  -> build HomeTransaction + relocation candidates
  -> replay dirty/scratch masks
  -> Verify(candidate)
  -> compare cost
  -> atomic publish candidate OR restore baseline
```

这与现有“先产生有 spill 的分配、再 reserve x18 重跑”兼容。反过来先做 split 再触发 x18 会改变池图，使第一遍 free-home 结论失效，禁止。

### 4.3 必须新增的审计计数

第一实现批不发码，只记录：

- fixed conflict 总数及 target home；
- incumbent/incoming interval 长度与 tail 长度；
- cut 处 baseline free-pool 数与 scratch reserve；
- observer/fixed-clobber/fault/helper 拒绝原因；
- baseline transport 数、候选 relocation 数、候选可消 transport 数；
- 若 dry-run Apply 后的 spill/reload/helper/bytes 差；
- 逐 root entry-weighted `saved - relocation - spill - helper`。

没有这张联合分布，pool10 直方图不能独立批准 P1。

---

## 5. P2：fault-safe region-local home fact

### 5.1 Fact 与传播规则

对每个 HIRFunction 建：

```text
HomeFact = {
  home_id,
  home_version,
  ssa_value,
  width_fact,
  location,          // 必须为 fixed home 才可 architectural-canonical
  canonical
}
```

RPO 迭代收敛，IN 为所有普通内部 predecessor OUT 的精确交集；不建 phi，多前驱版本不同即 Unknown。Store/SetHost 完整写产生新 HomeVersion；partial write 只有显式 merge 后才能产生 canonical fact。

一个入口 GetHost 只有在 IN fact 完全相同、版本定义支配当前 block、且 `location==home && canonical` 时才 trivial。SetHost 只有目标 home 已持有同一 HomeVersion 时才 trivial。这样删掉的是 R 类 transport，不是把普通 SSA 跨边藏起来。

### 5.2 清空/降级边界

以下边界把相关 fact 置 Unknown，除非 opcode 有显式 preserve contract：

- public/external block entry；
- dispatcher、direct-link canonical entry、CallHost/CallDynamic/CallLambda；
- fault/signal cold stub、SMC invalidation/re-entry；
- X87、GetUniformAddress、UniformBarrier、未知 SetHost/partial home write；
- module/FeatureSet ABI 不匹配边。

普通 region internal edge 才传播。region split/merge 后重新按实际 HIR CFG 求解；fact 不进入全局 cache，也不按 guest PC 猜测。

当前生产 pipeline 没有把 CFGAnalysisPass 作为普遍前置，因此 P2 不能假设 dominance/RPO 已填好。实现有两种合法选择：

1. 在 RA 前显式运行并验证 CFG analysis；或
2. P2 内构建最小 RPO/dominator，只接受 dense block ids、所有 successor/predecessor 对称、入口唯一的 HIRFunction。

任一结构校验失败，整 function facts 清空，回到 baseline。

### 5.3 signal/fault 后 home 为什么仍正确

编译期 HomeFact 不是运行时隐藏状态，它只证明某条内部边到达时 fixed home 已含某版本。P2 不允许 canonical architectural version 只存在普通 pool：

- signal poll/cold stub 前 fixed home 已 canonical，trampoline 按 static descriptor 保存；signal handler/guest frame不认识 HomeFact，也不需要认识。
- guest fault 进入 recovery 时同理；pool SSA 被抛弃不影响 State。
- SMC priority-0 只恢复原 host context；若代码失效后改走 dispatcher，public canonical entry不接受旧 fact。
- helper 前现有 static descriptor 和 dirty-mask协议继续工作，helper 后只有明确 preserve 的 fact 可继续，其余 Unknown。

这与 fault-context recipe 审计的结论衔接：既然当前 FaultEntry 没有逐 home recipe，P2 就不把 recipe 当成前提；代价是漏掉“home 仅在 pool、可由 cold veneer 重建”的更激进池。

### 5.4 XMM 11 家驻留的统一与分工

XMM resident 已经是跨边 ABI：v17–v27 属 static descriptors，helper/fault/trampoline 统一保存恢复。GPR HomeFact 可复用同一 `HomeId/HomeVersion/boundary descriptor` 抽象，但 P2 首版只选择 GPR：

- XMM resident 可作为“始终 canonical 的 fixed-home fact”验证抽象正确；
- 不改变 FPR pool、驻留区间或 XMM ABI；
- 不把 XMM0/XMM12–15 的普通 SSA 借 P2 变成跨边驻留。

这样是统一 proof schema，不是重开 W-γ 全 XMM 静态映射。

---

## 6. P3：跨 direct-link 的双入口（后续独立项目）

当前任意 published block label、dispatcher/cache miss、RSB 都可能落到 unit 内公开入口。因此不能把 region-local fact 直接外推成跨 unit ABI。

最小可证设计：每个 eligible block 有两个 entry：

- **canonical/public entry**：现有入口；只依赖 State/static homes，必要时完成统一 reconcile，再进入 body。
- **SRA fast entry**：只允许带精确 `HomeContract` 的 direct link 到达。

`HomeContract` 至少包含：

- static descriptor ABI id；
- 每个携带 home 的 version class/width/canonical 位；
- resolved module FeatureSet hash；
- target body version/epoch。

source OUT contract 与 target IN contract逐项相等才可链 fast entry；否则走现有 canonical path。它不携带 SSA id，只表达物理 home 的 architectural version 等价，避免跨 unit SSA ABI。

### 6.1 module/cache/SMC

- Config hash 已包含 `buffers_static_alloc`；module 的 resolved FeatureSet 也进入 module cache identity。P3 仍需为 `HomeContract`/fast-entry layout 增加 cache format 字段，而不是假设现有 feature hash足够。
- disk-cache revive 必须先发布 canonical entry，再重建经过校验的 fast-entry offset/link metadata；旧格式拒 revive，建议 bump cache format。
- SMC invalidation 同时撤销 canonical/fast 两种 link，沿既有 owner epoch/QSBR 回收；新链接只能指向当前 epoch。
- 跨 module 只有 ABI id 与 FeatureSet hash 都精确一致才允许 fast link；首版可更保守地只允许 same-module。B 类 module override 不得通过 fast link 泄漏到另一 module。
- 多 X86Instance 的合同属于各自 AddressSpace/Runtime，不共享 host pointer 或 epoch。

P3 的 stale fast-entry 是野跳级风险，不能与 P1/P2 同批实现。

---

## 7. 三个历史否决的规避证明

### 7.1 pool starvation

旧失败：pool7 把压力尾淹没；pool10 虽好转仍有 CoreMark 2、SQLite 28 个 spill unit。

新不变量：

- 只在已经存在的 baseline pool slot 上做 ownership 交换；候选峰值池占用不得增加；
- x18 重跑后重算，不复用首轮计划；
- 任一新增 spill def/load/store、reserve ladder 升级、scratch headroom 降低都逐 unit restore baseline；
- pool12 不作为正确性或收益前提。

因此 P1 可能漏掉很多候选，但不会靠“假设池无限”过门。

### 7.2 桥接造多于消

旧失败：Phase C fixed-class 在 CoreMark/SQLite 分别增加 3.526%/5.436% move。

新不变量：每笔 HomeTransaction 先保存当前 fail-closed baseline，然后按实际 emitted cost 做严格单调比较：

```text
eliminated Get/Set
  > relocation moves + new edge repairs + new spill/reload + new helper saves
```

相等即回退；无收益 unit 的 mapping、shape、bytes 必须逐位保持 baseline。这样“全程序可能赚回来”不再能掩盖单 unit 造桥。

### 7.3 region 态收益蒸发

旧失败：width-chain v2 块态 −3.72%，生产 region 态约 −1.08%，关键锚点改善消失。

本设计从 selector、cost、验收三层都以 production region HIRFunction 为单位：

- dry-run entries、shape、cost 全取 region 默认态；
- block mode 只作 emitter 透镜，不参与 GO 数字；
- 必报共同 guest-PC 的 region shape/bytes，以及锚点在 region 成形后的真实命中；
- P2 直接以 region CFG 为事实载体，不把单块候选数外推。

---

## 8. 开关与量化门

本报告不改注册表；建议后续实现时分三个 gate，均默认 OFF、进入 FeatureSet/cache hash：

| gate 建议 | 范围 | 依赖 |
|---|---|---|
| `SVM_RA_SPLIT_RELOC` | P0+P1，unit-local component transaction/relocation | fixed-class；不依赖 P2 |
| `SVM_RA_HOME_FACT` | P2，region-local home-version facts | P0；可在 split OFF 下先审计 |
| `SVM_RA_HOME_LINK` | P3，跨 unit 双入口 contract | P0+P2；独立 cache-format/SMC 审核 |

P3 在 contract/cache 还未固化前应按进程级 A 类看待；证明 same-module + ABI/hash 匹配后才有资格降为 B 类。

### 8.1 P1 门

全部以 Linux identity、region 默认态、RE=0 entry 权重、禁 EXEC_PROF：

1. `new spill units/defs/loads/stores == 0`，动态 spill 严格零增；
2. helper caller-save、scratch reserve ladder、host bytes均不增；
3. entry-weighted `saved_transport - relocation - spill - helper > 0`，逐 unit 不劣；
4. CoreMark、SQLite fixed-class move 增量必须从 `+3.526%/+5.436%` 变为净下降；
5. 最终 full-pin P6 仍以 L2 baseline 判：CoreMark dynamic factor ≥1.03、host bytes不增、spill 不超过 baseline×1.25（baseline 为零时即零）；SQLite 新增的 26 个 spill unit必须消失；
6. helper-fault、五 gate fail set、x18 rerun focused tests全绿。

### 8.2 P2 门

1. 先报 R 类 9.958702% 毛池中的 exact-match、multi-pred mismatch、external-entry、observer clear、partial-write 五类分解；
2. 净值扣除 canonical/public-entry repair 与任何新增 edge move；
3. 沿 w66 重开条件：smallpt、c-ray 各自净删 host dynamic ≥0.5%，CoreMark只报告；
4. fault/helper/signal/SMC 边界 shape 与 State store 数不得增加；
5. no-eligible unit 的 mappings/host bytes逐位不变。

### 8.3 组合/翻盘门（P6 风格）

- 七语料 region 默认态 host/move/spill/uniform；
- 公共 PC host_static + unit bytes 双口径；
- STREAM 公共 PC shape 与 unit bytes逐位一致（若 zero eligible）；有 eligible 则单列，不用墙钟解释形状税；
- helper-fault、alarm、sqlite、SMC、disk-cache warm/revive；
- module override 2×2（source/target same/different FeatureSet）；
- Darwin/bias 在尚无池证据前必须保持 gate 惰性或逐 unit fail-closed；
- 任何机制 ON 的净动态不足 3%（full-pin 总方案目标）或 spill 非严格零增，NO-FLIP。

---

## 9. 实施顺序、依赖与成本

```text
P-1 联合分布审计 (analysis-only)
          |
          v
P0 immutable owner + HomeTransaction + baseline snapshot
          |
          +-------------------+
          v                   v
P1 split fragments       P2 region HomeFact audit/dataflow
 + Verify/emitter             |
          \                   /
           v                 v
          P2.5 combined region validation
                    |
                    v
          P3 dual-entry/direct-link/cache/SMC
```

| 片 | 工作量估算 | 主要产物 |
|---|---:|---|
| P-1 联合分布 probe | 2–3 人日 | conflict/tail/headroom/observer/cost 逐 root 表 |
| P0 owner + transaction | 4–6 人日 | 单一决策面、完整 rollback、零发码变化 |
| P1 fragment/location/Verify | 6–8 人日 | segmented map、dirty replay、RunVerified 接线 |
| P1 emitter + focused tests | 4–6 人日 | relocation schedule、独立复证、x18/helper/fault 测试 |
| P2 region dataflow | 5–7 人日 | validated RPO/dominance、HomeFact IN/OUT、审计统计 |
| P2 fault/helper/full gates | 4–5 人日 | production-region 全矩阵与 boundary 证明 |
| P3 dual-entry/link/cache/QSBR | 8–12 人日 | contract ABI、cache bump、SMC/link publication |
| 测量与翻盘证据 | 4–6 人日 | P6 密度/shape/oracle 报告 |

P0–P2 约 **21–30 人日**；P3 另加 **12–18 人日**；完整链约 **33–48 人日**。建议每片独立合并为默认 OFF，不能积成一次大 patch。

---

## 10. 尚未解决的证明洞

1. **联合分布缺失**：现有 pool10/max-live 只有边际分布，无法回答 fixed conflict cut 当下是否有可复用 pool 家。P-1 未过前不批准 P1。
2. **faulting destination 原子性**：尚未逐 opcode证明 fault 时 A64 destination 不被部分提交。首版排除 faulting producer；若未来放开需逐 opcode 白名单和最小 fault guest。
3. **segmented location 的 emitter 覆盖**：当前大量 emitter API 默认 SSA 只有单一 map。必须枚举所有 `ValueGPR/ValueType` 调用是否带 position；遗漏一个就是错码风险。
4. **生产 CFG 支配信息**：CFGAnalysisPass 当前不是所有生产 region 的稳定前置。P2 需要新增且 fail-loud 的 CFG 完整性证明。
5. **observer 完整集合**：Get/SetHost、Call、UniformBarrier、faultable memory、terminal 已知；所有 custom lambda/X87/SMC cold path仍需用 opcode 全表审计，不能靠手写小名单。
6. **direct-link 双入口序列化**：fast-entry offset、contract、epoch 的 cache 格式和 revive 越界校验尚未设计到字段级；P3 前必须先冻结格式并 bump version。
7. **并发重链竞态**：SMC 回收与 direct-link 重指同时发生时，fast contract 的验证/发布需与现有 QSBR 同一原子事务；当前没有证明。
8. **module override 组合**：resolved FeatureSet hash 能识别 codegen，但 static-home ABI id 与 module 生命周期的组合尚无现成字段。P3 首版应 same-module only。
9. **Darwin/bounded-bias**：pool10 是 Linux identity 证据，不能外推 mac pool；首轮产品 gate 应在 Darwin fail-closed，后续另量。
10. **XMM 泛化**：本设计只统一 proof schema，未证明普通 FPR SSA 跨边 home fact 安全；不得据此重开 XMM8–15/全静态映射。

---

## 11. 最终建议

**条件 GO：批准 P-1 analysis-only 联合分布审计；满足下列三条后再批准 P0/P1 实现：**

1. CoreMark 与 SQLite 的可 relocation conflict 中，entry-weighted 净值严格为正；
2. dry-run Apply/Revert 后新 spill/helper/host bytes均为零；
3. 收益在 production region 态存活，而不是只在 block 透镜出现。

P2 可与 P-1 并行做 analysis-only HomeFact census，但机制实现应依赖 P0 的不可变 owner/事务基座。P3 不进入首轮；它只有在 P2 rays 双过 0.5% 且双入口 cache/SMC 合同完成后才重开。

这个排序把两条审计线真正合在同一 RA 机制里，同时保留了三个关键保险：pool 不增压、每个事务不造桥、所有收益只在生产 region regime 裁决。
