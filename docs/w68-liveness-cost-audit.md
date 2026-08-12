# 路二：家占用代价的频度加权活性审计

## 0. 裁决

**NO-GO：当前 L3 + fixed-class 的实测 IR/RA 形状中，不存在“旧值跨块边界续命、
占用 fixed home，随后阻塞新版本”的事件。** CoreMark、SQLite 的 production
region 态以及 7zip 抽查均为零；RE=0 对照也为零。因此预注册门不通过：

| 预注册条件 | 实测 | 裁决 |
|---|---:|---|
| CoreMark 至少一个正净值 root | 0 | **FAIL** |
| SQLite 至少一个正净值 root | 0 | **FAIL** |
| CoreMark + SQLite entry-weighted 总净值严格为正 | 0 | **FAIL** |

这里的 `net=0` 是**候选集为空**，不是说边界杀值免费，也不是重述 P-1 的
同点 relocation 零和结论。probe 对同块冲突有大量命中，说明观测路径工作正常；
空集只发生在本任务新增的“跨边界”维度。

量化死因是：当前 measured region 虽是多块 RA unit，但 fixed-home 活区在候选
producer 处仍是 block-local SSA range；没有一个 owner 从较早 block 延伸到较晚
block 的新版本 producer。换言之，本任务希望利用的频度不对称在当前表示中尚未
形成，不能据此增加 early-kill cost model 或机制开关。

## 1. 基线、口径与探针

### 1.1 基线和运行配置

全程没有调用 git 命令。只读 `.git/HEAD` 与 ref 文件确认 host w68 和初始 VM
clone 均为：

```text
91952f28c1d32b4350d61be08db8edfe5eef4560
```

主采集配置：

```text
SVM_X86_PIN_EXT=3
SVM_RA_FIXED_CLASS=1
SVM_JIT_CACHE=
SVM_REGION_EDGES unset       # production region
SVM_EXEC_PROF unset          # 全程禁用
```

RE=0 对照只增加 `SVM_REGION_EDGES=0`。CoreMark 命令严格为
`coremark_x64 0 0 0x66 150000 7 1 2000`；SQLite 使用 fresh DB 和
`--size 1 --testset main`；7zip 抽查为 `b 1 -mmt1 -md16m`。

RA shape 直接确认 Linux identity pool10：

| corpus / region | `gpr_pool=10` units | spill 后 `gpr_pool=9` units | fixed hazards | forward evictions |
|---|---:|---:|---:|---:|
| CoreMark | 621 | 2 | 1,194 | 0 |
| SQLite | 4,831 | 30 | 7,702 | 0 |

这与任务指定的 pool10（spilling unit 重跑 pool9）一致。

### 1.2 两条“占家”观测通道

现行顺序是先收集活区，再规划 fixed affinity，最后做正向分配：
`register_alloc_pass.cpp:260-280`。probe 没有改 affinity、mapping、active mask、
spill、helper 或 emitter，只读取最终通过 `RunVerified` 的 scan。

为避免漏掉不同来源的 owner，审计拆成两条独立通道：

1. **fixed range 通道**：镜像 `fixed_ranges[target]` 的 `(start,end)`，额外只记
   owner definition block；覆盖入口 `GetHostGPR` range
   (`register_alloc_pass.cpp:426-440`) 和已接受 publication range
   (`register_alloc_pass.cpp:510-512`)。在新 producer 窗口重叠时，比较 owner
   definition block 与 producer block。
2. **producer-input 通道**：覆盖 producer 输入值已经物理映射到 target home、
   且 use-end 越过 producer 的冲突，即现行检查
   `register_alloc_pass.cpp:484-495`。该通道独立比较 input definition block 与
   producer block。

现行 observer/fixed-clobber 与 range 拒绝分别位于
`register_alloc_pass.cpp:464-504`；forward fixed-owner 冲突位于
`register_alloc_pass.cpp:524-547`。后者四组 `evictions=0`，所以没有遗漏的
forward-only 跨边 owner。

fixed-range probe 给每个 final RA unit 生成稳定 `(root, signature)`，临时把
既有 block-entry counter 输出扩展为 `(root, signature, block_pc, entries)`；若
出现事件，可逐版本精确连接 producer、SetHost 与 residual-use block，不按同 guest
PC 的多个版本平均。由于最终跨边事件为零，任何版本选择策略都不会改变结论。

### 1.3 预注册权重和成本模型

对一个候选事件，预注册计算为：

```text
W_life      = Σ residual_use_count(block) × entry(block)
W_producer  = entry(producer_block)
W_transport = Σ entry(SetHost block)              # 可删 transport

kill_nonremat = entry(boundary) + Σ entry(residual-use block)
kill_loadimm  = Σ entry(residual-use block)
kill_recompute= Σ entry(residual-use block)

net = W_transport - kill_cost
```

`W_producer`单列但不当作收益：early kill 不能删除 incoming producer。非
rematerializable owner 保守计一次 boundary spill 和每个 residual-use block 一次
reload；`LoadImm` 与只依赖 immediate/zero 的廉价纯算分别分类。该模型不给机制
虚构收益。

## 2. 五项采集总表

### 2.1 旧值 residual use 与 entry 权重

| regime / corpus | final RA units | 跨边 owner events | residual uses | residual-life weight |
|---|---:|---:|---:|---:|
| region / CoreMark | 626 | **0** | 0 | 0 |
| region / SQLite | 4,931 | **0** | 0 | 0 |
| RE=0 / CoreMark | 1,699 | **0** | 0 | 0 |
| RE=0 / SQLite | 14,712 | **0** | 0 | 0 |
| region / 7zip spot | 2,823 | **0** | 0 | 0 |

没有 event，故“旧值残余 use 数”和“各残余 block entry 权重”均为空表，不存在
被汇总隐藏的冷 residual block。

### 2.2 新版本 producer、SetHost 与 transport 权重

| regime / corpus | 跨边 blocked producers | producer weight | 后续 SetHost sites | transport weight |
|---|---:|---:|---:|---:|
| region / CoreMark | 0 | 0 | 0 | 0 |
| region / SQLite | 0 | 0 | 0 | 0 |
| RE=0 / CoreMark | 0 | 0 | 0 | 0 |
| RE=0 / SQLite | 0 | 0 | 0 | 0 |
| region / 7zip spot | 0 | 0 | 0 | 0 |

注意这里不重复计 P-1 已审计的同点 relocation；只要 owner 与 producer 在同一
block，本表就明确排除。

### 2.3 边界杀值成本与 rematerialize 分类

| regime / corpus | spill+reload events / weight | LoadImm events / weight | cheap recompute events / weight |
|---|---:|---:|---:|
| region / CoreMark | 0 / 0 | 0 / 0 | 0 / 0 |
| region / SQLite | 0 / 0 | 0 / 0 | 0 / 0 |
| RE=0 / CoreMark | 0 / 0 | 0 / 0 | 0 / 0 |
| RE=0 / SQLite | 0 / 0 | 0 / 0 | 0 / 0 |
| region / 7zip spot | 0 / 0 | 0 / 0 | 0 / 0 |

这些零值表示“没有可定价事件”；不能解释为实施 early kill 会产生零 spill 或零
reload。

### 2.4 频度不对称度分布

`asymmetry = W_transport / W_life`。候选集为空，故比值本身未定义；按预注册
分桶报告事件数为：

| regime / corpus | 有定义样本 | `>=2x` | `>=5x` | `>=10x` |
|---|---:|---:|---:|---:|
| region / CoreMark | 0 | 0 | 0 | 0 |
| region / SQLite | 0 | 0 | 0 | 0 |
| RE=0 / CoreMark | 0 | 0 | 0 | 0 |
| RE=0 / SQLite | 0 | 0 | 0 | 0 |
| region / 7zip spot | 0 | 0 | 0 | 0 |

### 2.5 逐 root 与全语料净值

| regime / corpus | 有跨边 event roots | 正净值 roots | 负净值 roots | saved transport | kill cost | net |
|---|---:|---:|---:|---:|---:|---:|
| region / CoreMark | 0 | **0** | 0 | 0 | 0 | **0** |
| region / SQLite | 0 | **0** | 0 | 0 | 0 | **0** |
| RE=0 / CoreMark | 0 | **0** | 0 | 0 | 0 | **0** |
| RE=0 / SQLite | 0 | **0** | 0 | 0 | 0 | **0** |
| region / 7zip spot | 0 | **0** | 0 | 0 | 0 | **0** |

CoreMark + SQLite production region 合计仍为 `0 - 0 = 0`，未过“严格为正”。

## 3. probe 覆盖负控：同块形状大量存在

为了区分“probe 没工作”和“跨边维度为空”，保留同块命中计数：

| regime / corpus | fixed-range 同块重叠 | producer-input 同块占家 | 跨块合计 |
|---|---:|---:|---:|
| region / CoreMark | 995 | 1 | **0** |
| region / SQLite | 6,568 | 3 | **0** |
| RE=0 / CoreMark | 630 | 1 | **0** |
| RE=0 / SQLite | 4,636 | 2 | **0** |
| region / 7zip spot | 3,587 | 0 | **0** |

同时，现行 fixed hazards 分别是 CoreMark region 1,194、SQLite region 7,702；
其余拒绝来自 observer、same-home access、fixed-clobber 等非“跨边 owner 占家”
原因。大量同块命中以及非零 hazards 证明候选扫描、target-home 识别和 final-scan
落账均被实际执行。

实测结果支持以下限定性判断：当前 region 构造会在 block 边界重新建立
`GetHostGPR` SSA 定义，fixed range 没有跨 block 保留成后续 producer 的 incumbent。
这是对 91952f2 和三套语料的观测，不外推为所有未来 IR 形态的永久不变量。

## 4. Oracle、形状复证与机械撤销

### 4.1 workload oracle

- CoreMark region / RE=0 均得到 CRC
  `e9f5/e714/1fd7/8e3a/25b5` 和 `Correct operation validated`；
- SQLite region / RE=0 均打印 `TOTAL`，每次使用独立 fresh DB；
- 7zip 单线程 16 MiB 字典 benchmark 完成并打印压缩、解压与总计行；
- 所有运行显式清除 `SVM_EXEC_PROF`。

### 4.2 top shape 前后逐点一致

临时 probe 撤销、隔离目录重建后，以相同命令复跑：

- CoreMark region top-20：`rank/PC/host_static/move_static/nan_static`
  **20/20 一致**；
- SQLite region top-20：同 tuple **20/20 一致**；
- CoreMark RE=0：1,695 个前后共同 PC 的
  `(host_static,move,nan,spill,state)` **1,695/1,695 一致**；
- SQLite RE=0：**14,708/14,708 一致**。

RE=0 的 translated set 有正常运行间漂移：CoreMark 前后各 1,703 PC、共同
1,695；SQLite 为 14,710 / 14,712、共同 14,708。共同 PC 中 host-byte placement
分别有 15、36 个 4-byte 级差异，但 emitter 指令数、move、spill、NaN 和 state
shape 全部零差异；production top-20 也零差异。

### 4.3 源码 SHA 与构建

首次六文件 probe 和补充 input probe 都已机械撤销。恢复后隔离构建受影响 targets
`[10/10]` 通过；源码中已搜不到 `SVM_RA_LIVENESS_*`、`BoundaryAudit` 或
`svm-ra-live-*`。host w68 和恢复源的 SHA-256 为：

```text
94c241c5614a30521430a387d7b3491dcb8c9a61345fe403495aaee0caf3b66f  register_alloc_pass.cpp
7dd1c398bc879082cb685251f2850689429be39677c6450ee56e7b4751c919c9  register_alloc_verified.inc
3cc29e0e69c8e6ee8ff0627a64ffe20f315fa6cb9992ffeab441c90771f4eb29  ra_shape_prof.h
81dcd7d9de7eb1ca61d2fc4df473647b7c4417580e51ee5e6ece4a910973ae1e  hot_coalesce_prof.h
33d560537d378fddd20255883284a298f0c18a6cc9835677d891e3f9617eda37  hot_coalesce_prof.cpp
51cdfa309ee08ef5a9bcac0698d7a34dec404a7a1199277efb64a588b4ec7e7b  jit_context.cpp
```

采集中发现共享 VM clone 同时被另一战役临时写入并构建。没有修改或吸收其代码；
补充 probe、撤销重建和 after-shape 复证均转移到
`~/svm-phasec/liveness-src` + `liveness-build` 隔离目录，仍严格位于用户允许的
`~/svm-phasec/` 范围内。本报告是 w68 唯一保留的交付改动。

## 5. NO-GO 重开条件

不建议实现 cost model、early-kill rewrite 或新开关。量化重开必须先由同口径
analysis-only probe 同时证明：

1. production region 中 `cross_boundary_owner_events > 0`，且事件必须来自
   owner 在更早 block 定义、在后续 producer block 仍有 residual use；
2. CoreMark 与 SQLite 各自至少一个 root 满足
   `entry_weighted(transport_saved - spill - reload - rematerialize) > 0`；
3. 两语料合并总净值严格为正，不能用一个语料补贴另一个全负语料；
4. 对所有正候选，fault/helper/observer/fixed-clobber 契约可证明闭合，新增
   spill/helper/host bytes 按实际发生次数计入，不得假设为零；
5. 收益必须在 production region 存活，RE=0 只作定位透镜。

若未来 P2/HomeFact 让一个 home version 真正跨内部边存活，应在该表示落地后重跑
本审计；当前 canonical-per-block 表示下继续调 early-kill 阈值不会制造候选。

## 6. 原始证据索引

VM 内证据均位于允许范围：

```text
/home/swift/svm-phasec/liveness-evidence/
  clean-before/{coremark,sqlite}-{region,re0}/
  probe/{coremark,sqlite}-{region,re0}/
  probe/7zip-region/
  input-probe/{coremark,sqlite}-{region,re0}/
  input-probe/7zip-region/
  restored/{coremark,sqlite}-{region,re0}/
  compare/                         # top/common-PC 规范化结果与 diff
  source-clean/SHA256SUMS
  source-probe.SHA256SUMS
  source-restored.SHA256SUMS
  input-probe.SHA256SUMS
  input-restored.SHA256SUMS
  input-build.log
  input-restored-build.log
```
