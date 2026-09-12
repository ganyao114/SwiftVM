# osslsha unit-local SHA producer / fixed-home tie 二道审计

## 0. 结论先行

**裁决：两道归零路径均为 NO-GO，可消下界都是 0 条/block。**

| 审计 | 形态毛池 | 严格候选 | 排除后的可消下界 | 裁决 |
|---|---:|---:|---:|---|
| ① unit-local SHA producer | 43 SetHostFPR | 38 个“unit 内生产、SSA 消费不出 block”的 Set | **0** | 38 个全部在下一个同步 fault 点前承担 fixed-home commit；其余 5 个本来就不是纯 local 值 |
| ② fixed-home tie | 24 个实际发码 GetHostFPR | 24 个均为 U64 lane read | **0** | 24 个全部跨 FPR→GPR register class；其中 12 个还读取 offset=8，现有 fixed-FPR alias 证明连候选门都进不了 |

43 与 24 均已逐 IR id 闭合：

- `43 = 14（完整 V128、fault 前）+ 24（U64 partial、fault 前）+ 3（入口/家间 transfer、fault 前）+ 2（terminal 发布）`；
- `24 = 12（low U64 lane）+ 12（high U64 lane）`。

这 67 条是**形态池，不是可删池**。其中 43 条 Set 的 41 条是 resident fixed-home 的 fault-visible commit，另外 2 条是自环/外部退出的架构状态提交；24 条 Get 是 `UMOV FPR lane → GPR` 的实际计算输入，不是同 register class 内的 transport copy。

本轮没有墙钟结论。机器受载，所有结论只来自 RE=0 的条数、最终 IR、RA 映射和源码证明。

## 1. 口径与复现

### 1.1 热块与 RE=0 形态

语料：`openssl_x64 speed -seconds 1 -bytes 8192 -evp sha256`。密度臂显式清除 JIT cache 与 EXEC_PROF，并用 `SVM_REGION_EDGES=0` 取得 RE=0 形态：

```sh
env -u SVM_JIT_CACHE -u SVM_EXEC_PROF \
  SVM_REGION_EDGES=0 SVM_PROF=2 SVM_DENSITY_PROF=1 \
  SVM_RA_HOT_COALESCE=/private/tmp/osslsha-tie-audit/re0/hot.log \
  SVM_RA_HOT_COALESCE_ALL=1 SVM_DUMP_IR=1 \
  build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/openssl_x64 \
  speed -seconds 1 -bytes 8192 -evp sha256
```

原始输出：`/private/tmp/osslsha-tie-audit/re0/`。

```text
[svm-hot-all] pc=0x8ba580 versions=1 entries=4081431
  host_bytes=2932 host_static=723 move_static=183 spill_static=0
```

最终 RA 后逐指令只读打印得到：

- 实际发码 `SetHostFPR = 43`；
- `GetHostFPR = 51`，其中已有 fixed-home alias、零发码 27，实际发码 **24**；
- 43 个 Set 的 `write_coalesced=false`；24 个实际 Get 的 `read_coalesced=false`；
- 24 个实际 Get 全部分配到 GPR（交替使用 x7/x8），该 unit `spill_static=0`。

因此本次一秒 RE=0 样本的形态毛账为：

| 项 | 每 entry | entries | digit-exact 动态条数 | 占该块 host_static |
|---|---:|---:|---:|---:|
| SetHostFPR | 43 | 4,081,431 | 175,501,533 | 5.947441% |
| 实发 GetHostFPR | 24 | 4,081,431 | 97,954,344 | 3.319502% |
| 合计 | 67 | 4,081,431 | 273,455,877 | 9.266943% |

这里的百分比只描述热块内部静态形态；不把它预支为全程序收益。

### 1.2 判定规则

“可删”同时要求：

1. producer 在 `0x8ba580` 内定义，全部 SSA uses 仍在该 block；
2. 删除 fixed-home 发布后，从 producer 到所有消费点之间不存在 fault/helper/signal/host-reg observer；
3. 不承担自环、外部 exit 或下一 guest block 的 canonical fixed-home 可见性；
4. RA 与 emitter 都能证明同家 alias，不跨 GPR/FPR register class，不越过 partial lane；
5. 证明失败按 0 计，不用“理论可以重写 DAG”预支收益。

源码基线与上述规则一致：

- guest memory 全部是 fault observation point：`source/runtime/ir/opts/uniform_store_sink_pass.cpp:120-136`；
- Get/SetHost 与控制流边界也会切断 capture segment：同文件 `:148-168`、`:270-285`；
- resident XMM 不另发 State capture，而由 fault path 从 v17-v27 保存：同文件 `:241-249`；
- fault trampoline 的 `BuildSaveStaticUniform` 确实把静态 FPR 家写回 State：`source/runtime/backend/arm64/trampolines.cpp:485-534`；
- 当前 resident ABI 是 XMM1-7（以及 HI 时 XMM8-11），XMM0 留在 State：`source/translator/x86/translator.cpp:570-588`。

## 2. 审计①：43 个 SetHostFPR

### 2.1 互斥分类，精确闭合到 43

| 类别 | IR ids | 数量 | unit 内生产且 SSA 消费不出 block | 最终排除原因 |
|---|---|---:|---|---|
| A. 完整 V128 SHA/table producer，下一 fault 前提交 | `54,102,141,180,219,258,297,336,375,414,453,492,527,548` | 14 | 是 | fixed home 必须在紧随其后的 LoadMemory fault 前为最新值 |
| B. U64 low/high partial lane 构造 v23，下一 fault 前提交 | `89,90,128,129,167,168,206,207,245,246,284,285,323,324,362,363,401,402,440,441,479,480,521,522` | 24 | 是 | 这是两条 `INS` 完成 V128 消息字构造；既是计算又是 fault-visible commit，不是完整 V128 copy |
| C. 非 unit producer 的 fixed-home transfer | `36,37,549` | 3 | 否 | 来源分别为 v18、v17、v24 的 GetHostFPR/BitCast；是入口/家间可见性迁移 |
| D. terminal 发布 | `585,586` | 2 | 否 | producer 虽在 unit 内，但结果须同时供 self edge 下一迭代和外部 `0x8ba85a` 入口观察 |
| **合计** | 全部 43 个 id | **43** | 严格 local 初筛 **38** | **可消 0** |

生产者细分：A 类中 `54,102` 是 `VecTableLookup8`，其余 12 个是 `VecSha256Msg2`；D 类两个是 `Vec4Add`。B 类 12 对均由标量 `Or(U64)` 生成 low/high 两半，并由 `EmitSetHostFPR` 发 `INS`；对应 emitter 证据见 `source/runtime/backend/arm64/jit/translator_mem.cpp:1117-1150`。

### 2.2 41 个 fault 前发布的逐点闭合

| fixed-home Set ids | 紧随的同步 guest-memory fault IR | 条数 |
|---|---:|---:|
| `36,37` | `LoadMemory 38` | 2 |
| `54` | `LoadMemory 64` | 1 |
| `89,90,102` | `LoadMemory 103` | 3 |
| `128,129,141` | `LoadMemory 142` | 3 |
| `167,168,180` | `LoadMemory 181` | 3 |
| `206,207,219` | `LoadMemory 220` | 3 |
| `245,246,258` | `LoadMemory 259` | 3 |
| `284,285,297` | `LoadMemory 298` | 3 |
| `323,324,336` | `LoadMemory 337` | 3 |
| `362,363,375` | `LoadMemory 376` | 3 |
| `401,402,414` | `LoadMemory 415` | 3 |
| `440,441,453` | `LoadMemory 454` | 3 |
| `479,480,492` | `LoadMemory 493` | 3 |
| `521,522,527` | `LoadMemory 528` | 3 |
| `548,549` | `LoadMemory 550` | 2 |
| **合计** | 15 个 fault 点 | **41** |

另有 `585,586` 位于 conditional terminal 前，故 `41 + 2 = 43`。

这不是把既有“XMM0 每 block 16 条 State capture”重复算入 43。XMM0 的 16 个 `StoreUniform u[160]` ids 是：

```text
35,53,101,140,179,218,257,296,335,374,413,452,491,526,547,564
```

它们是非 resident XMM0 的 State carrier；本节 41 条是 XMM1-11 resident fixed-home 的 commit。两者共同证明同一 fault 不变量，但属于不同 emitted instruction 集合。

### 2.3 为什么“unit-local”仍不能删

初筛的 38 个 producer 确实都在本 block 内产生，显式 SSA uses 也不出 block；但“显式 SSA use 不出 block”不等于“架构值不可观察”。在每组 Set 后，下一条同步 faultable `LoadMemory` 都可能进入 fault recovery。resident XMM 的恢复载体正是 v17-v27 fixed home，而不是任意 RA 临时。因此：

- 若只保留 SSA 临时并删除 Set，fault recovery 会保存旧 fixed home；
- 当前 generic FPR coalescer也把 Load/StoreMemory 列为 observer，见 `source/runtime/ir/opts/register_alloc_coalesce_gpr.cpp:77-99` 与 `source/runtime/ir/opts/register_alloc_coalesce_fpr.cpp:280-297`；
- AES chain 的特例只有在 producer **已经直接写 fixed home** 后才把 memory fault 视为安全，见 `register_alloc_coalesce_fpr.cpp:188-199`。本审计设想的“只留 SSA、不发布”不满足这个前提。

故审计①的安全可消下界是：

```text
0 条 SetHostFPR / block
0 × 4,081,431 = 0 条（本次 RE=0 样本）
```

## 3. 审计②：24 个实际 GetHostFPR

### 3.1 逐条分类与闭合

24 条组成 12 个完全重复的 low/high pair；每个 id 都列在下表。

| 轮次 | fixed home | low lane id（offset 0） | high lane id（offset 8） | low uses | high uses |
|---:|---:|---:|---:|---|---|
| 1 | v21 | 79 | 80 | `LsrImm #32` | `LslImm #32`、`LsrImm #32` |
| 2 | v22 | 118 | 119 | 同上 | 同上 |
| 3 | v19 | 157 | 158 | 同上 | 同上 |
| 4 | v20 | 196 | 197 | 同上 | 同上 |
| 5 | v21 | 235 | 236 | 同上 | 同上 |
| 6 | v22 | 274 | 275 | 同上 | 同上 |
| 7 | v19 | 313 | 314 | 同上 | 同上 |
| 8 | v20 | 352 | 353 | 同上 | 同上 |
| 9 | v21 | 391 | 392 | 同上 | 同上 |
| 10 | v22 | 430 | 431 | 同上 | 同上 |
| 11 | v19 | 469 | 470 | 同上 | 同上 |
| 12 | v20 | 508 | 509 | 同上 | 同上 |
| **合计** | v19/v20/v21/v22 各 6 条 | **12** | **12** | 12 | 24 |

失败原因分类：

| 类别 | 数量 | 现有机制没吃掉的精确原因 | observer / width / spill / fault 登记 |
|---|---:|---|---|
| low U64 lane | 12 | return type 是 U64，`IsFloatValueType=false`，不能映射到 HostFPR | observer：Get-use 小窗内无额外 observer；width/class：失败；spill：0；fault：结果必须经后续 v23 commit 后才可过下一 LoadMemory |
| high U64 lane | 12 | 除 U64 跨 class 外，offset=8 也违反 fixed-FPR alias 的 offset=0 门 | observer：同上；width/class：失败；offset：失败；spill：0；fault：同上 |
| **合计** | **24** | **24/24 在候选入口前 fail-closed** | **可 tie 0** |

RA 的 exact guard 在 `source/runtime/ir/opts/register_alloc_pass.cpp:1782-1799`：只有 `GetHostFPR && offset==0 && IsFloatValueType(return)` 才能成为 fixed FPR alias；还要通过同 block end 和 `LiveRangeCrossesHostRegWrite`。本组 24 条全部是 U64，12 条 high lane 还额外违反 offset=0。

emitter 与该判定一致：非 float GetHostFPR 必须取 GPR result，并按 lane 发 `UMOV`，见 `source/runtime/backend/arm64/jit/translator_mem.cpp:1026-1050`。其独立复证也再次要求 offset=0 和 float type，见同文件 `:420-445`。

因此这里不是“RA 没找到更好的同类 FPR 家”，而是**没有一个物理寄存器能同时作为 U64 GPR 值和 V128 fixed FPR home**。把 producer 固定到 v19-v22 最多可能省某些 full-width publish，仍不能让这 24 个 scalar consumers 与 producer 同家，不能兑现题设的“publish+reload 一起归零”。

审计②的安全可消下界是：

```text
0 条 GetHostFPR / block
0 × 4,081,431 = 0 条（本次 RE=0 样本）
```

## 4. 现有 coalescer 的次级拒绝项

即使忽略上述两条决定性障碍，当前 write 侧也不会误把这些形态吞掉：

- `IsResidentFPRProducer` 白名单没有 `VecSha256Msg2`、`VecTableLookup8`、`Vec4Add`，见 `source/runtime/ir/opts/register_alloc_coalesce_fpr.cpp:28-49`；
- partial U64 Set 在入口先因 offset=8（high half）或非 V128 type 被拒，见同文件 `:229-249`；
- full producer 多数在 Set 后仍有 block-local SSA uses，不满足当前 `last use == SetHostFPR`；对应 RA 门为同文件 `:243-249`，emitter 复证为 `source/runtime/backend/arm64/jit/translator_mem.cpp:604-635`；
- 该 unit `spill_static=0`，所以 24 个 Get 未合并不是 spill-pressure 回退；
- fault/observer 不是 24 个 Get 自身短 use window 的第一拒因，却是不能把整段 scalar rotate DAG 连同 fixed-home commit 一起删掉的边界义务。

## 5. 封存与重开条件

### 5.1 unit-local SSA 归零：封存

在现有 fault 恢复载体下，38 个 local producer 均不能只留在普通 SSA/RA 临时。重开必须先满足以下任一条件：

1. fault table 获得 per-fault FPR recovery recipe，能从任意 RA 物理家恢复 resident XMM；或
2. producer 直接、可证明地写 resident home，并对每个 SHA/table opcode建立 alias-emission + 多 use + 双侧 fixed-home conflict + emitter 独立复证。

条件 1 是此前 fault-context-recipe 方向的独立基建，不在本审计预支；条件 2 只能形成“省 full-width publish”的新候选，不能消掉本轮 24 个跨 class Get。若重开，第一门应先审 14 个 full V128 producer 的多 use/component tie，不能把 B 类 24 个 `INS` 计作 copy 毛池。

### 5.2 fixed-home tie 归零：封存

本方向只有在新增**跨 register-class lane fusion**时才可重开，例如把完整的

```text
GetHostFPR low/high → shifts/or → SetHostFPR low/high
```

识别成一条等价的 vector-lane shuffle/rotate，并同时证明：

- 12 种重复实例的 DAG 无外部 use；
- source/destination alias 逐位安全；
- v23 在下一 faultable LoadMemory 前已经 committed；
- emitter 对实际寄存器与 lane 独立复证；
- RE=0 逐 entry 删除数 digit-exact，spill 与其他六语料公共 PC 为零税。

这已经是新的 SHA message-schedule lowering，不是 fixed-home tie 的自然扩展；在该基建出现前，本题的 fixed-home tie 方向正式封存。

## 6. 清理与默认态零变化证据

审计时曾在 `runtime.cpp` 的 RA 后位置加入只读、仅 `SVM_DUMP_IR=1 && func_start==0x8ba580` 生效的临时打印，用于记录最终 IR id、RA mapping、read/write coalesced 标志。采集后已机械删除并重建：

```text
[100%] Built target swift_test
```

构建输出只有该树已有的 warning（`#pragma once in main file`、intrusive-list、logging macro、VIXL deprecated iterator），没有由审计代码留下的新 warning。

撤探针后重新跑同一 RE=0 形态：

```text
rc=0
[svm-hot-all] pc=0x8ba580 versions=1 entries=4184760
  host_bytes=2932 host_static=723 move_static=183 spill_static=0
audit marker count=0
sha256 258008.23k
```

原始输出：`/private/tmp/osslsha-tie-audit/restored/`。生产源码中不存在 `audit-fpr`/`audit post-ra` 标记；未修改任何默认、FeatureSet/env 计数、docs、golden、linker 或 harness。交付 tracked 内容仅本报告。
