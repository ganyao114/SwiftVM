# P-1：relocation 联合分布 dry-run 审计报告

## 0. 裁决

**NO-GO：不放行 P0/P1 实现。** Linux identity pool10 上确实存在大量
headroom 充足、observer 安全的 fixed-home tail relocation 形状，但当前
`PlanFixedGPRAffinities()` 暴露的每个安全候选都严格是：

```text
saved SetHost = 1
relocation mov = 1
added spill/reload/helper = 0
host-byte delta = 0
net = 1 - 1 - 0 - 0 = 0
```

设计 §3.1 明确要求候选严格优于 baseline、相等也回退。因此 CoreMark 与
SQLite 的 cost selector 都得到 `Apply=0`；生产 region 态不存在正收益，P-1
三项放行条件只通过第 2 项。

| §11 放行条件 | CoreMark region | SQLite region | 裁决 |
|---|---:|---:|---|
| 可 relocation conflict 的 entry-weighted 净值严格为正 | `0` | `0` | **FAIL** |
| Apply/Revert 后新增 spill/helper/host bytes 全为零 | `0 / 0 / 0` | `0 / 0 / 0` | PASS |
| 收益在生产 region 态存活 | `Apply=0` | `Apply=0` | **FAIL** |

这次 NO-GO 的第一死因不是 pool starvation，也不是 fault/helper：它死在
**transport cardinality**。余尾几乎全为 1，但一个 relocation 仍只能换掉一个
SetHost，无法满足严格正收益。

## 1. 基线与采集方法

### 1.1 基线确认

没有调用任何 git 命令。VM 内只读 `.git/HEAD` 与对应 ref 文件得到：

```text
HEAD=refs/heads/master
refs/heads/master=d8786bc657d36926a32b0a74c1fe919e5dbbc4e0
```

环境为 OrbStack Ubuntu/aarch64；源码、build 和证据全部位于
`/home/swift/svm-phasec/`。两臂均使用：

```text
SVM_X86_PIN_EXT=3
SVM_RA_FIXED_CLASS=1
SVM_JIT_CACHE=
```

生产组清除 `SVM_REGION_EDGES`，块态组设 `SVM_REGION_EDGES=0`；所有运行均
显式清除 `SVM_EXEC_PROF`。

### 1.2 probe 放置与“不改决策”证明

临时 probe 只观察现行路径：

- reverse-plan hazard：`register_alloc_pass.cpp:413-512`；
- forward fixed owner fallback：`register_alloc_pass.cpp:524-557`；
- x18/eviction/scratch ladder 稳定后，只在最终通过 `RunVerified` 的 scan 上落行：
  `register_alloc_verified.inc:38-169`。

probe 没有修改 affinity、mapping、active mask、spill、helper 或 emitter；环境变量
未设置时完全不收集。为避免 region 多版本和异步编译误配，临时 metadata 给每个
最终 RA event tuple 计算稳定 signature，既有 block-entry counter 输出
`(root, signature, entries)`；分析严格按 `(root, signature)` 连接，不按版本顺序
或均摊猜测。

定义：

- `hazard`：现行 reverse-plan 拒绝的 fixed write candidate；
- `fixed conflict`：hazard 中确有 incumbent interval 的子集；
- `structural`：恰好一个 incumbent、incoming 在 baseline 已占普通 pool slot，
  且无 observer/fixed-clobber/fault/helper/same-home access/pending-write；
- `free_pool`：baseline 最终 dirty mask 在 cut 后的 clear count；
- `headroom = free_pool - scratch_reserve`；本语料所有相关 final scan 的
  `scratch_reserve=3`；
- 动态数：每个 root 的静态事件数乘该 `(root, signature)` 的 root entry；region
  内部 block entries 不重复摊入 root。

现行 candidate 的 `GetUses()==1` 且 `use_end[source]==SetHost.Id()`
（`register_alloc_pass.cpp:452-460`），所以 fixed conflict 相对 baseline 最多新增
删除一条 publication SetHost。SetHost 之后 baseline 与候选都已 canonical，后续
Get/Set 不是本次 relocation 的增量收益。因此 `elidable=1` 不是估算，而是当前
selector 窗口的上界。

## 2. 七项采集总表

### 2.1 总量、fixed conflict 与逐 root 加权

| regime / corpus | executed roots | hazards 静态 | fixed conflict 静态 | structural 静态 | fixed conflict 加权 | structural 加权 | 正净值 roots |
|---|---:|---:|---:|---:|---:|---:|---:|
| region / CoreMark | 623 | 1,194 | 1,134 | 923 | 1,716,048,582 | 1,390,094,154 | **0** |
| region / SQLite | 4,860 | 7,701 | 7,245 | 6,132 | 10,840,190 | 9,136,561 | **0** |
| RE=0 / CoreMark | 1,701 | 761 | 727 | 604 | 1,181,125,912 | 1,057,373,061 | **0** |
| RE=0 / SQLite | 14,711 | 5,633 | 5,302 | 4,530 | 7,623,940 | 6,204,646 | **0** |

现行 forward allocator 的 `fixed_evictions` 四组均为 **0**；联合分布全部来自
reverse-plan 的 `fixed_hazards`。这符合代码顺序：range/RAW/WAW 在
`PlanFixedGPRAffinities()` 已 fail-closed，未把有冲突的 affinity 送入 forward
allocator。

生产 region 中，fixed conflict 占全部 hazard 的 CoreMark `94.974874%`、
SQLite `94.078691%`。结构安全候选占 fixed conflict 的静态比例分别为
`81.393298%`、`84.637681%`；entry-weighted 比例为 `81.005524%`、
`84.284141%`。候选很多，但全部是 1-for-1。

### 2.2 target home 全分布（生产 region）

| target home | CoreMark 静态 | CoreMark 加权 | SQLite 静态 | SQLite 加权 |
|---:|---:|---:|---:|---:|
| x0 | 22 | 600,075 | 63 | 125,302 |
| x1 | 11 | 52 | 30 | 81,549 |
| x2 | 5 | 46 | 11 | 6,469 |
| x3 | 8 | 10 | 21 | 1,495 |
| x4 | 7 | 122 | 33 | 432 |
| x5 | 4 | 46 | 3 | 691 |
| x6 | 3 | 4 | 53 | 22,039 |
| x7 | 6 | 65 | 33 | 63,627 |
| x8 | 8 | 13 | 35 | 230,545 |
| x9 | 7 | 29 | 23 | 483,443 |
| x19 | **942** | **914,444,203** | **6,425** | **9,251,346** |
| x20 | 37 | 6,001,308 | 202 | 74,465 |
| x21 | 20 | 133 | 150 | 201,448 |
| x22 | 16 | 114 | 70 | 121,159 |
| x23 | 7 | **455,400,009** | 21 | 8,720 |
| x29 | 31 | **339,602,353** | 72 | 167,460 |
| 合计 | 1,134 | 1,716,048,582 | 7,245 | 10,840,190 |

CoreMark 静态由 x19 主导（83.068783%），动态则分成 x19 53.287781%、
x23 26.537711%、x29 19.789787%；少数 x23/x29 root 极热。SQLite 的 x19
同时主导静态（88.681850%）和动态（85.343024%）。

### 2.3 incoming/incumbent interval 与余尾

#### Incoming interval 长度

| 长度 | CoreMark 静态 / 加权 | SQLite 静态 / 加权 |
|---:|---:|---:|
| 2 | 949 / 1,390,095,276 | 6,196 / 9,246,888 |
| 3 | 33 / 1,363 | 112 / 166,189 |
| 4 | 15 / 153,600,115 | 75 / 69,301 |
| 5 | 133 / 172,351,741 | 840 / 1,351,658 |
| 6 | 4 / 87 | 22 / 6,154 |

#### Incumbent interval 长度

| 长度桶 | CoreMark 静态 / 加权 | SQLite 静态 / 加权 |
|---:|---:|---:|
| 2 | 302 / 395,741,227 | 1,930 / 2,932,418 |
| 3 | 797 / 835,507,241 | 5,208 / 7,754,023 |
| 4–8 | 15 / 455,400,052 | 42 / 47,098 |
| 9–16 | 9 / 29,400,026 | 23 / 76,195 |
| ≥17 | 11 / 36 | 42 / 30,456 |

#### Incumbent 余尾长度（producer 前 cut 到最后 use）

| 余尾 | CoreMark 静态 / 加权 | SQLite 静态 / 加权 |
|---:|---:|---:|
| 1 | **1,133 / 1,716,048,578** | **7,243 / 10,839,650** |
| 2 | 1 / 4 | 2 / 540 |

CoreMark 99.911817% 静态 conflict、几乎 100% 动态 conflict 的 tail=1；
SQLite 分别为 99.972395% / 99.995019%。短尾说明 relocation 实施简单，却不
产生“一个 relocation 换多条 transport”的 cardinality 优势。

### 2.4 cut 点 pool headroom 与资格转移

| headroom (`free_pool-3`) | CoreMark 静态 / 加权 | SQLite 静态 / 加权 |
|---:|---:|---:|
| 1 | 2 / 2 | 31 / 9,507 |
| 3 | 3 / 11 | 8 / 628 |
| 4 | 15 / 602,271 | 32 / 20,025 |
| 5 | 56 / 153,600,306 | 225 / 457,001 |
| 6 | **1,038 / 1,527,928,168** | **6,640 / 10,151,511** |
| 7 | 20 / 33,917,824 | 309 / 201,518 |

headroom=6 占 CoreMark fixed conflict 的 91.534392% 静态 / 89.037582%
动态，占 SQLite 的 91.649413% / 93.646984%。pool10 本身不是主要拒绝维度。

但“有 free headroom”不等于设计 §3.1 的“资格转移成立”：incoming 必须已经在
baseline 占有一根普通 pool 家，才能把同一资格交给 incumbent。缺少 baseline
slot 的 fixed conflict 为：

| corpus | 静态 | entry-weighted |
|---|---:|---:|
| CoreMark region | 56 | 2,490 |
| SQLite region | 180 | 266,624 |

对其强行 relocation 会使 `candidate_peak_pool = baseline_peak_pool + 1`，所以
probe 按设计直接拒绝；所有 structural candidate 都满足
`candidate_peak_pool == baseline_peak_pool`。

### 2.5 observer 距离与拒绝原因

| 首个 observer 距离 | CoreMark 静态 / 加权 | SQLite 静态 / 加权 |
|---:|---:|---:|
| 无 (`-1`) | 979 / 1,390,096,644 | 6,309 / 9,402,978 |
| 1 | 151 / 325,951,851 | 911 / 1,430,847 |
| 2 | 4 / 87 | 25 / 6,365 |

固定 conflict 的拒绝原因（原因可重叠）：

| 原因 | CoreMark 静态 / 加权 | SQLite 静态 / 加权 |
|---|---:|---:|
| observer / same-home access | 155 / 325,951,938 | 936 / 1,437,212 |
| fixed-clobber | **0 / 0** | **0 / 0** |
| fault | **0 / 0** | **0 / 0** |
| helper | **0 / 0** | **0 / 0** |
| multiple incumbents | 1 / 1 | 4 / 223 |
| 无 baseline pool slot | 56 / 2,490 | 180 / 266,624 |

这说明本语料的安全障碍集中在紧邻 cut 的 observer/home access；fault、helper
和 fixed-clobber 没有吞掉 relocation 候选。它们未来仍需 Verify 契约，但不是
这次净收益为零的原因。

### 2.6 baseline transport、候选 relocation 与 dry-run 差额

| regime / corpus | fail-closed SetHost（hazard）静态 / 加权 | fixed-conflict 部分 | structural 可消 transport | candidate relocation | `saved-relocation` | Apply |
|---|---:|---:|---:|---:|---:|---:|
| region / CoreMark | 1,194 / 1,720,851,110 | 1,134 / 1,716,048,582 | 923 / 1,390,094,154 | 923 / 1,390,094,154 | **0** | 0 |
| region / SQLite | 7,701 / 11,846,436 | 7,245 / 10,840,190 | 6,132 / 9,136,561 | 6,132 / 9,136,561 | **0** | 0 |
| RE=0 / CoreMark | 761 / 1,181,726,116 | 727 / 1,181,125,912 | 604 / 1,057,373,061 | 604 / 1,057,373,061 | **0** | 0 |
| RE=0 / SQLite | 5,633 / 8,189,352 | 5,302 / 7,623,940 | 4,530 / 6,204,646 | 4,530 / 6,204,646 | **0** | 0 |

对每个 structural 候选，shadow Apply 的模拟差额均为：

```text
spill defs/loads/stores delta = 0
reload delta                 = 0
helper save delta            = 0
host bytes delta             = 4 - 4 = 0
```

随后严格 cost gate 因 `net==0` Revert。Revert 后四项全为零，满足不劣性，但也
没有任何可发布收益。

### 2.7 联合分布的最高权重 cell

联合键为 `(target, tail, headroom, observer_distance)`：

| corpus | 联合 cell | 静态 | entry-weighted |
|---|---|---:|---:|
| CoreMark region | `(x19,1,6,none)` | 829 | 870,774,911 |
| CoreMark region | `(x23,1,6,none)` | 6 | 454,800,009 |
| CoreMark region | `(x29,1,6,1)` | 13 | 157,200,085 |
| CoreMark region | `(x29,1,5,1)` | 2 | 153,600,003 |
| SQLite region | `(x19,1,6,none)` | 5,589 | 8,264,903 |
| SQLite region | `(x19,1,6,1)` | 702 | 889,801 |
| SQLite region | `(x9,1,6,none)` | 6 | 481,802 |
| SQLite region | `(x8,1,6,1)` | 7 | 156,109 |

全量联合 cell 在 VM 证据目录各自的 `joint.csv`；精确边际在 `analysis.json`。

## 3. 逐 root 净值

| corpus / regime | roots | 有 fixed conflict | 有 structural candidate | 正净值 root | 负净值 root |
|---|---:|---:|---:|---:|---:|
| CoreMark region | 623 | 280 | 254 | **0** | 0 |
| SQLite region | 4,860 | 1,752 | 1,554 | **0** | 0 |
| CoreMark RE=0 | 1,701 | 323 | 273 | **0** | 0 |
| SQLite RE=0 | 14,711 | 2,029 | 1,778 | **0** | 0 |

生产 region 最高 conflict 权重 roots 也全部净零：

| corpus | root | entries | weighted conflicts | weighted structural | weighted net |
|---|---:|---:|---:|---:|---:|
| CoreMark | `0x403320` | 153,600,000 | 307,200,000 | 153,600,000 | **0** |
| CoreMark | `0x403360` | 98,400,000 | 295,200,000 | 295,200,000 | **0** |
| CoreMark | `0x402250` | 33,317,718 | 233,224,026 | 233,224,026 | **0** |
| CoreMark | `0x4034b0` | 193,200,000 | 193,200,000 | 193,200,000 | **0** |
| SQLite | `0x4a5138` | 471,071 | 471,071 | 471,071 | **0** |
| SQLite | `0x4595a0` | 51,527 | 412,216 | 360,689 | **0** |
| SQLite | `0x4f6600` | 67,091 | 335,455 | 268,364 | **0** |
| SQLite | `0x459693` | 51,527 | 309,162 | 257,635 | **0** |

完整逐 root 表为各目录的 `per-root.csv`。所有非零 conflict root 的
`candidate_net_weighted` 均为 0，没有被汇总掩盖的正收益 root。

## 4. region 存活性与块态透镜

RE=0 没有揭示 region 被掩盖的正收益：两口径的 structural candidate 都是
1-for-1，净值同为 0。region 反而保留了更大的 candidate population，但只放大
零净值：CoreMark structural 从 1,057,373,061 加权项增到 1,390,094,154；
SQLite 从 6,204,646 增到 9,136,561。故第 3 条不是“收益蒸发”，而是从一开始
就没有严格正收益可存活。

## 5. 构建、oracle 与探针撤销

### 5.1 构建与语料

- d8786bc clean build：PASS；
- probe build：PASS；
- 机械撤销后完整受影响 target 重建：`[60/60]` PASS；
- CoreMark 四组均返回 0，CRC 为
  `e9f5/e714/1fd7/8e3a/25b5`，打印 `Correct operation validated`；
- SQLite 四组均返回 0并打印 `TOTAL`；每组使用独立 fresh DB；
- 全过程未启用 `SVM_EXEC_PROF`。

### 5.2 top shape 逐点复证

probe 与撤销后同命令重跑：

- CoreMark region top-20 hot roots：rank、PC、host_static、move_static
  **20/20 逐点一致**；
- SQLite region top-20：同四项 **20/20 逐点一致**；
- CoreMark RE=0：1,698 个共同 root 的
  `(host_bytes,host_static,move,spill,state)` **1,698/1,698 一致**；
- SQLite RE=0：**14,711/14,711 一致**；
- spill shape 四组均保持 `spill_loads/spill_stores` 不变，fixed forward
  `evictions=0` 不变。

region 的全体 translated set 会随运行时 region/version 成形略变；CoreMark 两遍
共有 2,873 PC，其中 2,860 的完整 tuple 一致，13 个落在不同 region version；
SQLite 22,050 个共同 PC 中 host_static/move/spill/state 全一致，只有 2 个
host-byte placement 值相差 4 bytes。top-20 与确定性的 RE=0 全量比较均为零 diff，
没有 probe 导致的 allocation/emitter 形状变化。

### 5.3 机械撤销证据

临时 probe 的六个文件已全部恢复；源码中搜索不到
`SVM_RA_RELOC_AUDIT`、`RelocAudit`、`reloc_audit` 或 `svm-hot-version`。
host w68 与 VM 恢复后 SHA-256 逐文件相同：

```text
94c241c5614a30521430a387d7b3491dcb8c9a61345fe403495aaee0caf3b66f  register_alloc_pass.cpp
7dd1c398bc879082cb685251f2850689429be39677c6450ee56e7b4751c919c9  register_alloc_verified.inc
3cc29e0e69c8e6ee8ff0627a64ffe20f315fa6cb9992ffeab441c90771f4eb29  ra_shape_prof.h
81dcd7d9de7eb1ca61d2fc4df473647b7c4417580e51ee5e6ece4a910973ae1e  hot_coalesce_prof.h
33d560537d378fddd20255883284a298f0c18a6cc9835677d891e3f9617eda37  hot_coalesce_prof.cpp
51cdfa309ee08ef5a9bcac0698d7a34dec404a7a1199277efb64a588b4ec7e7b  jit_context.cpp
```

本报告是 w68 的唯一交付改动；没有保留机制或 probe 代码。

## 6. 缺口形态与重开条件

### 6.1 NO-GO 的主形态

1. **cardinality 主死因**：CoreMark/SQLite 的 tail=1 接近 100%，且当前窗口每个
   incoming 只有一条 SetHost publication；`1 saved - 1 relocation = 0`。
2. **pool 不是主死因**：约 92% 静态 conflict 的 headroom=6，structural 比例
   约 81–85%。增加 pool 不会把 1-for-1 变成正收益。
3. **observer 是次级过滤器**：多数只在 cut 后 1 条；但即使全部解除，新增候选
   仍为净零。
4. **fault/helper/fixed-clobber 本语料为零**：放宽这些契约不会制造收益。
5. **动态热点集中**：CoreMark x23/x29 少量静态 site 很热，但同样净零；只按
   target 热度选取也不能过严格 cost gate。

### 6.2 量化重开前置

P1 只有在 selector 输入扩展后才值得重开，且必须先由 analysis-only probe 证明：

1. 存在 `elidable Get/Set >= 2` 的同一 immutable HomeTransaction；不能把
   SetHost 后 baseline 本来已 canonical 的读取重复记收益；
2. CoreMark、SQLite production region 各自至少一个 root 满足
   `entry_weighted(saved-relocation-spill-helper) > 0`，全语料总净值也严格为正；
3. candidate peak pool 不超过 baseline，新增 spill/reload/helper/host bytes
   仍全为零；
4. P0 若要单独实现，必须另案证明它有独立价值且 OFF/ON 零发码变化；本 P-1
   不批准以“先铺基座”为由绕过 §11 的 P0/P1 联合门。

在出现多 transport component 的实测联合分布之前，建议保持现行 fail-closed
fixed-class 机制，不进入 fragment/location/emitter 实现。

## 7. 原始证据索引

VM 内：

```text
/home/swift/svm-phasec/p1-evidence/probe-v2/{region,re0}-{coremark,sqlite}/
  audit.log       # 每 final RA root/event 原始行
  hot.log         # (root,signature) entry 与 hot shape
  shape.log       # RA/spill/helper 总账
  analysis.json   # 精确边际与总数
  joint.csv       # 四维联合分布全表
  per-root.csv    # 逐 root entry-weighted 全表
  stdout.log      # CRC/TOTAL oracle

/home/swift/svm-phasec/p1-evidence/restored/...
/home/swift/svm-phasec/p1-evidence/probe-source/SHA256SUMS
/home/swift/svm-phasec/p1-evidence/restored-source-sha256.txt
/home/swift/svm-phasec/p1-build-probe-v2.log
/home/swift/svm-phasec/p1-build-restored.log
```
