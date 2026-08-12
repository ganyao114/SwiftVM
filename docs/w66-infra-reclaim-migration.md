# 路一：基建寄存器压缩 × guest 家迁 callee-saved 审计

## 0. 结论先行

**NO-GO，不进入机制 spike。** 在生产 region 形态下，把 x0–x9 中任一
guest fixed home 迁入一个新释放的 callee-saved 槽，省下的 helper
snapshot/restore 远小于释放 x25/x27 所引入的热路径税：

- Linux identity（本方向的 pool10 主形态）三语料没有一个直接净值为正；
- Darwin+bias 只有 SQLite 的“释放 x27、迁一个家”是正值：
  `+716,500` 条（`+0.224162% host`），但同一格 Linux 是
  `-1,082,082` 条（`-0.390884%`）；
- x25 单独释放、x25+x27 同时释放在两平台三语料全部严格为负；
- CoreMark/zip7 的 helper 边界极少，迁家收益只有百至千条，而 x25/x27
  的 pointer/reload 税是 5.29 亿至 17.73 亿条；
- “迁最热家/次热家”在本账里几乎退化成同一列。原因不是 guest 家热度，
  而是 `EmitHostCall` 在 helper 边界为所有 caller-saved fixed home 做正确性
  保存，真正可删条数受 `STP/LDP` 奇偶配对和 argument overlap 支配。

因此没有注册 `SVM_*` 开关，也没有生产源码改动。x26/x28 未触碰；x24/x10
按 identity/bias 既有条件回收处理，不混入 x25/x27 的净账。

## 1. 口径与探针

### 1.1 运行形态

- checkout：指挥官给定的 `91952f2`；全程未执行任何 git 命令；
- region：`SVM_REGION_EDGES=1`；
- full-pin 审计形态：`SVM_X86_PIN_EXT=3 SVM_RA_FIXED_CLASS=1`；
- 动态权重：`SVM_RA_HOT_COALESCE_ALL=1 SVM_RA_SHAPE_PROF=1`
  `SVM_PROF=2 SVM_DENSITY_PROF=1`；
- 明确 `env -u SVM_JIT_CACHE -u SVM_EXEC_PROF`；
- CoreMark：`0 0 0x66 150000 7 1 2000`；
- SQLite：`--size 1 --testset main <fresh-db>`；
- zip7：`b -mmt1 -md=16m`。

Mac 为 Darwin+bias 图；Orb Ubuntu 为 Linux identity 图。后者实测 pool
直方图重新对上既有 w68 记录：CoreMark `9:2,10:621`，SQLite
`9:28,10:4831`；不是把 Darwin pool7 误当 Linux 上限。

### 1.2 只读计算

临时 probe 在每个已存在的 `CallLambda` 发码点做两次**只读**计算：

1. 当前 `save_gprs` 的真实 `STP/LDP` 指令数；
2. 假设某个 x0–x9 guest home 已迁至 callee-saved 后重新算配对数；若旧
   host code 同时承载 helper 参数/allocator live value，则加回，不假定可删；
3. 对任意两家同时迁移也直接重算，不把两个单家收益相加；
4. RSB push/pop site 与现有 hot entry counter 相乘；
5. x27 直接复用现有 `CacheBaseReloadInstructions` 的 RSBHit 与 Dispatcher
   两类记录。

这给出的是“静态全局重映射”的可删上限，不假定被腾出的 x0–x9 会被 RA
重新分配给其他 live temp。真实 spike 只可能比该上限更难证明。

源码契约证据：

- level3 映射为 `x0..x9 = RSI,RDI,R8,R9,R10,R11,R12,R13,R14,R15`：
  `source/translator/x86/translator.cpp:88-105`；
- helper 显式把 caller-saved fixed descriptors 并入 live mask，并按配对
  生成保存集：`source/runtime/backend/arm64/jit/translator_control.cpp:194-230`；
- x25 是 RSB pointer，push/pop 直接 pre/post-index：
  `source/runtime/backend/arm64/jit/jit_context.cpp:726-775`；
- x27 是 L2 base，RSB hit 直接以它寻址：同文件 `:823-838`。

## 2. caller-saved 家税账

表中“逻辑税”是该家在 helper 边界的一次 save + restore；“实际可删”按
整组 `STP/LDP` 重新配对，才是净账使用值。

### 2.1 Linux identity

| guest 家 | host | CoreMark 逻辑/可删 | SQLite 逻辑/可删 | zip7 逻辑/可删 |
|---|---:|---:|---:|---:|
| RSI | x0 | 450 / 288 | 3,790,964 / 1,987,366 | 2,756 / 2,258 |
| RDI | x1 | 450 / 268 | 3,790,964 / 1,987,272 | 2,756 / 2,258 |
| R8 | x2 | 450 / 288 | 3,790,964 / 1,987,366 | 2,756 / 2,258 |
| R9 | x3 | 450 / 288 | 3,790,964 / 1,987,366 | 2,756 / 2,258 |
| R10 | x4 | 450 / 288 | 3,790,964 / 1,987,366 | 2,756 / 2,258 |
| R11 | x5 | 450 / 288 | 3,790,964 / 1,987,142 | 2,756 / 2,258 |
| R12 | x6 | 450 / 288 | 3,790,964 / 1,987,366 | 2,756 / 2,258 |
| R13 | x7 | 450 / 288 | 3,790,964 / 1,987,366 | 2,756 / 2,188 |
| R14 | x8 | 450 / 288 | 3,790,964 / 1,987,248 | 2,756 / 2,258 |
| R15 | x9 | 450 / 288 | 3,790,964 / 1,987,366 | 2,756 / 2,258 |

helper call site 的 entry-weighted 次数分别为 225 / 1,895,482 / 1,378。
RDI、R11、R14/R13 的小差异来自 helper 参数 overlap；没有一个 guest 家
形成独立热税大户。

### 2.2 Darwin+bias

| guest 家 | host | CoreMark 逻辑/可删 | SQLite 逻辑/可删 | zip7 逻辑/可删 |
|---|---:|---:|---:|---:|
| RSI | x0 | 450 / 298 | 3,789,450 / 3,786,482 | 1,030 / 708 |
| RDI | x1 | 450 / 288 | 3,789,450 / 3,786,388 | 1,030 / 708 |
| R8 | x2 | 450 / 298 | 3,789,450 / 3,786,482 | 1,030 / 708 |
| R9 | x3 | 450 / 298 | 3,789,450 / 3,786,482 | 1,030 / 708 |
| R10 | x4 | 450 / 298 | 3,789,450 / 3,786,482 | 1,030 / 708 |
| R11 | x5 | 450 / 298 | 3,789,450 / 3,786,258 | 1,030 / 708 |
| R12 | x6 | 450 / 298 | 3,789,450 / 3,786,482 | 1,030 / 708 |
| R13 | x7 | 450 / 298 | 3,789,450 / 3,786,482 | 1,030 / 638 |
| R14 | x8 | 450 / 298 | 3,789,450 / 3,786,482 | 1,030 / 708 |
| R15 | x9 | 450 / 298 | 3,789,450 / 3,786,482 | 1,030 / 708 |

helper call site 次数分别为 225 / 1,894,725 / 515。Darwin SQLite 的
单家节省显著高于 Linux，是两平台 helper save-set 配对奇偶不同，不是
guest 家更热。

### 2.3 双家配对

把 x0+另一个无 overlap 家同时迁走时的**真实**最佳节省：

| 平台 | CoreMark | SQLite | zip7 |
|---|---:|---:|---:|
| Darwin+bias | 450 | 3,789,450 | 1,030 |
| Linux identity | 450 | 3,790,964 | 2,756 |

这说明不能把两个单家节省直接相加。多数现场只少一对 STP/LDP，第二个
callee 槽主要增加 pool 容量，并不再减少一对 helper 指令。

## 3. x27 与 x25 的直接税

### 3.1 x27 unpin

x27 reload = `RSBHit + Dispatcher`：

| 平台/语料 | RSBHit | Dispatcher | x27 总税 | 占 host |
|---|---:|---:|---:|---:|
| Darwin CoreMark | 194,416,870 | 334,967,896 | 529,384,766 | 0.878775% |
| Darwin SQLite | 793,814 | 2,276,168 | 3,069,982 | 0.960467% |
| Darwin zip7 | 144,930,585 | 597,730,393 | 742,660,978 | 0.562709% |
| Linux CoreMark | 194,416,868 | 334,967,898 | 529,384,766 | 0.943627% |
| Linux SQLite | 793,881 | 2,275,567 | 3,069,448 | 1.108786% |
| Linux zip7 | 144,932,735 | 597,735,057 | 742,667,792 | 0.646664% |

旧 B0 的 `1,016,545,472` 条是旧代码/旧运行长度上的模型点；本轮同 build、
同 workload entries 重算后，模型结构一致但不能把旧绝对数硬搬进当前矩阵。
并且旧报告已明确：`0.679715%` 是 PF/AF 收益扣成本后的净值，不是 x27
reload 本身（`docs/w68-register-reclaim-audit.md:172-190`）。

### 3.2 x25 unpin

按任务冻结口径，x25 pointer 税为 `(push + pop) × 2`：

| 平台/语料 | push | pop | x25 总税 | 占 host |
|---|---:|---:|---:|---:|
| Darwin CoreMark | 300,415,483 | 315,617,308 | 1,232,065,582 | 2.045221% |
| Darwin SQLite | 5,306,530 | 1,458,057 | 13,529,174 | 4.232704% |
| Darwin zip7 | 353,226,051 | 162,066,615 | 1,030,585,332 | 0.780868% |
| Linux CoreMark | 300,415,483 | 315,617,306 | 1,232,065,578 | 2.196154% |
| Linux SQLite | 5,306,924 | 1,458,545 | 13,530,938 | 4.887820% |
| Linux zip7 | 353,233,209 | 162,071,167 | 1,030,608,752 | 0.897383% |

x25 不是闲置 callee-save；关闭 RSB 时它已经条件归池，活动 RSB 下才有本表
税（`docs/w68-register-reclaim-audit.md:147-154`）。所以另加一个同义 gate
不能免费获得槽位。

## 4. 迁移净账矩阵

定义：

```text
direct net = 实际可删 helper snapshot 指令
             - x27 cache-base reload
             - x25 RSB pointer load/store
```

“最热家”固定取 x0/RSI；“次热家”取 x2/R8。两者在三语料都并列最佳，
所以单槽两列数字相同；若取非并列的 x1/x5/x7，只会更差。

### 4.1 Linux identity（裁决主表）

| 释放项 | 迁最热 x0 | 迁次热 x2 | 两槽都用时 |
|---|---:|---:|---:|
| x27 / CoreMark | -529,384,478 (-0.943627%) | 同左 | — |
| x27 / SQLite | -1,082,082 (-0.390884%) | 同左 | — |
| x27 / zip7 | -742,665,534 (-0.646662%) | 同左 | — |
| x25 / CoreMark | -1,232,065,290 (-2.196153%) | 同左 | — |
| x25 / SQLite | -11,543,572 (-4.169918%) | 同左 | — |
| x25 / zip7 | -1,030,606,494 (-0.897381%) | 同左 | — |
| x25+x27 / CoreMark | — | — | -1,761,449,894 (-3.139780%) |
| x25+x27 / SQLite | — | — | -12,809,422 (-4.627185%) |
| x25+x27 / zip7 | — | — | -1,773,273,788 (-1.544045%) |

### 4.2 Darwin+bias（平台交叉表）

| 释放项 | CoreMark | SQLite | zip7 |
|---|---:|---:|---:|
| x27 + 单家 | -529,384,468 (-0.878775%) | **+716,500 (+0.224162%)** | -742,660,270 (-0.562709%) |
| x25 + 单家 | -1,232,065,284 (-2.045221%) | -9,742,692 (-3.048075%) | -1,030,584,624 (-0.780867%) |
| x25+x27 + 双家 | -1,761,449,898 (-2.923996%) | -12,809,706 (-4.007613%) | -1,773,245,280 (-1.343576%) |

## 5. pool +1/+2 边际：能证明什么

按要求不重跑 pool cap，只引用既有 w68 直方图
（`docs/w68-register-reclaim-audit.md:260-291`）：

| 语料 | pool10 | pool11（任一槽） | pool12（两槽） |
|---|---:|---:|---:|
| CoreMark 至少 spill unit / peak excess | 2 / 6 | 2 / 4 | 0 / 0 |
| SQLite 至少 spill unit / peak excess | 28 / 86 | 24 / 54 | 2 / 8 |
| CoreMark 保守 spill-unit 区间 | 2–20 | 2–20 | 0–18 |
| SQLite 保守 spill-unit 区间 | 28–213 | 24–209 | 2–187 |

这些是**容量边际**，不是可直接加到净账上的动态指令数；旧报告也明确禁止
把 peak excess 臆造为 spill load/store。本轮实测提供两个约束：

- Linux CoreMark 当前 spill dynamic 只有 72 条。即使 pool11/12 把它
  全部清零，也不可能填平 5.29 亿/12.32 亿条直接税；
- Linux SQLite 当前 spill dynamic 为 1,191,068。即使违反直方图、乐观
  假设 pool11 全清，也只会把 x27 单槽从 `-1,082,082` 推到理论
  `+108,986`，而直方图已经证明 pool11 仍至少有 24 个 spill unit；
- zip7 当前 spill dynamic 为 4,665,058,732，但旧 w68 没有 zip7 的
  pool10→11 联合直方图。按预注册纪律不能把“可能的大池”当收益。更重要的
  是同一全局映射已经在 CoreMark 确定回退，故不能据此批准 spike。

所以 pool 边际没有改变 NO-GO。若以后允许按 unit 做可回滚的 infra lease，
那是新的 RA/ABI 机制，不是本任务的“释放一个全局 callee 槽”。

## 6. x24/x10 与 Darwin+bias 单列

既有池审计给出的平台图是：Linux identity 首轮 pool10、Darwin identity
pool9、Darwin+bias pool7（`docs/w68-register-reclaim-audit.md:25-37`）。

- Linux identity 下 x24、x10 已经在 pool，本方向不能重复回收；
- bias 下 x24 是 permanent page-table base，x10 是 memory scratch/窄 lease；
  回收它们会给 guest memory operand 增加地址供给和 live interval，不是一个
  免费 callee 家；证据见同报告 `:82`、`:161-170`；
- 本次 Mac shape 的确是 pool7，Linux shape 对上 pool10。由于没有获准重跑
  Darwin pool7→8 的边际，本报告不虚构“Darwin 可能收益最大”的动态数；
- x26 packed flags 与 x28 State* 按任务边界完全未动。

## 7. 探针撤销、自证与原始产物

临时插桩已从 mac worktree 与 Orb clone 机械撤销；两边均完成增量重建。
清理后源码中下列 probe token 全部为零命中：

```text
svm-infra
helper_home
helper_pair
rsb_push_sites
rsb_pop_sites
liveness_audit
```

Mac 清理构建：`[100%] Built target swift_test`，新增 warning 集合为空；输出
只含基线已有的 intrusive-list、macro redefine、return-type 等 warning。

用同一 CoreMark 形态重跑清理态：

```text
seedcrc=0xe9f5 crclist=0xe714 crcmatrix=0x1fd7
crcstate=0x8e3a crcfinal=0x25b5
Correct operation validated
```

探针态 top-20 热 PC 与清理态 20/20 共同 PC 的
`host_bytes/host_static/move_static/spill_static/state_saved_static` **逐点
零 diff**。全覆盖集合因 region 形成时的冷启动覆盖噪声为 2894/2895 unit，
不把覆盖差误报成发码差。

原始日志：

- `/private/tmp/infra-coremark2-hot.log`
- `/private/tmp/infra-sqlite1-hot.log`
- `/private/tmp/infra-zip7-hot.log`
- `/private/tmp/infra-coremark-clean-hot.log`
- `/private/tmp/infra-reclaim-clean-build.log`
- Orb：`~/svm-phasec/infra-reclaim-audit/{coremark,sqlite,zip7}/hot.log`
- Orb 清理构建：`~/svm-phasec/infra-reclaim-clean-build.log`

## 8. 量化重开条件

1. **x27**：必须用真实生产 A/B 证明 pool+1 的动态 spill/bridge 降幅同时
   超过 CoreMark `529,384,766`、zip7 `742,667,792`，并且不依赖 workload
   识别；只证明 SQLite Darwin 的 `+716,500` 不够。
2. **x25**：必须先把 RSB pointer 的额外税从“每 push/pop 两条”降到接近
   零，同时证明 signal/fault/direct-link 上 pointer 一致性；否则最小门槛
   是 CoreMark `1,232,065,578` 条，远高于 helper snapshot 池。
3. **两槽**：必须有 unit-local、可回滚的 pool 使用机制，不能把 pool12
   容量投影直接当收益；CoreMark 直接税门为 `1,761,450,344` 条。
4. **Darwin+bias**：若另案重开 x24，第一门是 page-table/base 供给的真实
   memory-op 税，而不是复用本报告的 x25/x27 矩阵。

在这些条件出现前，本方向钉死为 **NO-GO / 不装机 / 不注册 gate**。
