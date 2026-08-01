# W81：self-backedge exit latch + flags minimum-live-set spike

日期：2026-08-01  
基线：`d9970cb`  
分支：`w81-backedge`  
开关：`SVM_BACKEDGE_LATCH`、`SVM_BACKEDGE_FLAGS`，均默认 OFF；后者仅在前者 ON 时生效。

## 结论

P0 的正确性目标成立：self-backedge 现在可在每次 guest iteration 的回边处用
`LDAR + CBNZ` 观察统一 request；Signal 与 SMC 都以 release 发布，JIT 以 acquire
读取，冷 veneer 完整提交状态后返回。master 上 `alarm(1)` 自环 5 秒超时的问题在
LATCH ON 后有界退出；`exit_group`、三类 SMC、per-block fault subrange 和 disk-cache
revive 的定向门也通过。OFF 路径保留原 State 布局、原 trampoline L1 load 和原发码，
指纹对 master 零差。

P1 已跑的 directed/fuzz 正确性子集与 STREAM 静态目标成立：三热块稳态为
`32/35/43`，按 W79 的同口径补回 9 条一次性 cold-link tail 后是要求的
`41/44/52`。但它没有通过项目性能门，而且要求的 producer/width/polarity/consumer
完整 Cartesian product 未单独穷举，不能宣称 P1 完成全部正确性证明：
CoreMark 的 27 个 proof candidate 中只有 `0x405fd0` eligible；18 个已经没有目标
PF/AF/NZCV producer（主体已被 W59 branch-only 吸收），其余因 polarity 非单用或
pre-producer clobber 被拒。P0 poll 覆盖 CoreMark 大量既有 branch-only 自环，而 P1
没有足够消除量补偿。因此 P1 维持默认 OFF，并按 W79 的失败纪律撤出翻盘候选；代码
只保留为 opt-in spike，不能据 STREAM 单一强覆盖语料建议默认开启。

## 1. 改动清单

### 1.1 P0：统一 exit request

- `common/backedge_control.{h,cpp}`：static-cached env；bit 63 是 sticky Signal，低
  63 位是单调 SMC generation。
- `State` 不新增前置字段。OFF 时首槽仍是旧 `l1_code_cache`；LATCH ON 时同一个
  8-byte union 槽成为 `exit_request`，L1 指针移到既有 `RuntimeProfileInterface`
  的尾部。这样 OFF 的全部 State/uniform offset 不动，ON 的 poll 仍可用 offset 0，
  严格两条指令。
- `SignalInterrupt()`：先 release 写 `running=false`，再 release `fetch_or` Signal；
  OFF 时仍走旧 `halt_reason=Signal`。
- `SmcTracker`：每个 Runtime 注册 request 指针；清完 shared L2 与所有 Runtime L1
  slot 后才 release `fetch_add` generation，禁止“先通知、后清 slot”的重入窗口。
- self `Forward()`：完整物化之后发 `LDAR request; CBNZ veneer; B self`。veneer 发布
  当前 guest block，区分 Signal/CodeMiss 并 `Ret`。
- C++ 边界：返回后 acquire 读取 generation，先 `CloseWriteWindow()`，再只对完全
  相同的 generation 做 CAS acknowledge；并发更新令 CAS 失败，留给下一边界处理。
  若 Signal 与 SMC 同时发生，sticky Signal 最终覆盖 SMC-only 的 CodeMiss 返回值。
- dispatcher 的 ON 路径多一次 interface→L1 load；OFF 仍是一条旧的 State→L1 load。

内存序链为：

```text
Signal: running=false --release--> request.fetch_or --release--+
                                                              |
SMC: clear all dispatch slots --> request.fetch_add --release-+--> LDAR acquire
                                                                    |
                                               full deopt veneer <--+
                                                                    |
                     CloseWriteWindow --> exact-generation CAS acknowledge
```

### 1.2 P0：fault map

- `FaultEntry` 增加 allocation owner 与可选 recovery；删除/reclaim 按 owner 一次移除
  全部 subrange。
- function 编译保留一个 whole-unit committed-state fallback，同时为每个 emitted
  block 登记精确 subrange；重叠 lookup 优先更具体且带 recovery 的项。
- block/lazy 同样登记精确 hot subrange；host PC 不在精确范围或 proof 没有 recovery
  时回旧全局 `label_fault_return_host`。
- P0 disk-cache revive 用已序列化的 block `code_offset` 重建 per-block subrange；结构
  非单调/越界即拒绝 revive。

### 1.3 P1：六条 minimum-live-set sink

P1 只延迟：

- `carry_inverted` 的 `mov + strb`；
- requested NZCV 的 `mrs + and + and + orr`。

PF byte 与 AF 的 5 条仍在每次热迭代更新 x26。published/external entry 与 self-local
entry 分开：所有外部 link/RSB/cache-miss 都先进入 initializer；只有 self edge 直接回
local body。initializer 先读取 committed polarity；若它与本 block 的编译期 polarity
不同，则条件翻转 x26 的 C 位，再写入新 polarity 并从 x26 恢复 host NZCV。这个规范化
是必要的：否则 external 首轮在 producer 前 fault 会把旧 NZCV 与新 polarity 混合。

cold successor、Signal/SMC deopt 与 eligible block 的 fault recovery 都执行相同的
`EmitBackedgeMaterialize()` 后再离开 local ABI。

### 1.4 disk cache 与 env

- `kGetenvNames`：56→58，末两项为 `SVM_BACKEDGE_LATCH`、
  `SVM_BACKEDGE_FLAGS`。
- LATCH ON、FLAGS OFF：disk cache 可用并按 block offsets 重建 fault map。
- FLAGS ON：明确禁用 disk cache并打印 warning；当前 `SerialBlock` 不序列化 eligibility
  与 recovery relocation，不能安全 revive P1 unit。

## 2. P1 双边证明逐条核对

| W79 条件 | 实现证明 | 失败动作 |
|---|---|---|
| self target 同 block | terminal 必须是 `If`，两边都为 direct `LinkBlock/Fast`，且恰一边等于 block start | block-local fallback |
| 唯一 cold direct successor | 另一边必须是唯一 direct location | fallback |
| PF/AF 已在 x26 current | final `SaveFlags` 必须同时请求 PF、AF 与 NZCV；PF/AF emitter 保持启用 | fallback |
| carry polarity 编译期已知 | final carry store 必须是 U8 `0/1` LoadImm，且 LoadImm 单 use | fallback |
| 回边到下一 producer 无 NZCV clobber | 只接受逐项审过 emitter 的窄 allowlist；外部入口先恢复/规范化 NZCV | 首个未批准 op 即 fallback |
| producer 到 safepoint 无 fault/observer | memory、atomic、alignment、helper、dynamic call、x87、SSE4.2 string 等全部拒绝 | fallback |
| 所有外部入口 committed | published label 固定先跳 initializer；self edge 单独指向 local label | 无旁路入口 |
| 静态 proof 与 emitter state 一致 | terminal 再核对 `save_in_nzcv/nzcv_dirty/requested mask` | 现场补回 polarity + 普通 MergeNZCV，整 block 不优化 |

验证语料没有出现 `emitter fallback`；所有回退均发生在静态 proof 阶段。

## 3. fault / signal / SMC 结果

### 3.1 Signal 与 exit_group

| 项目 | 结果 |
|---|---|
| `/private/tmp/w79-self-alarm-x64`，master / OFF | `timeout 5` rc=124 |
| 同 guest，LATCH ON / LATCH+FLAGS ON | rc=0，约 1.04 秒有界退出 |
| alarm 并发压力 | 1,000 次，32-way；0 failure / 0 timeout |
| W64 两次 Run 间 race | 既有 test 内循环 1,000 次，仍仅 3 个聚合 assertion；通过 |
| raw-clone：另一 guest thread 条件自环，leader `exit_group` | native/P0/P1 均 rc=0；P0/P1 约 26/17ms（单次） |
| exit_group 高负载压力 | 1,000 次，32-way；0 failure / 0 timeout |

静态 latency 上界是一次 guest iteration：request 若在本轮 `LDAR` 前发布则本轮退出；
若落在 `LDAR` 与 `B self` 之间，最多再执行一轮后命中。实测 latency 单列见 §7。

### 3.2 SMC

| 类型 | 证据 | 结果 |
|---|---|---|
| 本线程自写 | `smc_x86_64` 两次 patch 42→99→7 | rc=99（该 guest 的 success code） |
| 另一 guest thread 写 | `clone_smc_mt_x86_64` | rc=0 |
| 无同步多线程 churn | `run_smc_stress_tests.sh`, 200 runs | host_fails=0, guest_lost=0, timeouts=0 |
| host syscall emulation copy 写 code page | 两次 `rt_sigprocmask`：先设置受控 8-byte mask，再由 `oldset` 的 `TryWrite` 覆盖热 code | P0/P1 均 rc=99（patch 42→98 后 success） |
| host-side teardown 写 guest memory | helper-fault `clone_pf` | worker PageFatal，leader rc=0，ctid 成功清零 |

这些路径均在清 slot 后发布 request；self loop 返回 C++ 后执行 CloseWriteWindow，旧
self branch 不再继续留在已 detach translation 中。

### 3.3 fault subrange / recovery

临时 multi-block guest 在同一 function 内提供两个 fault block：

| 模式 | path A | path B |
|---|---|---|
| function | PageFatal guest entry `0x401007` | `0x401020` |
| lazy function | `0x401007` | `0x401020` |
| block | `0x401007` | `0x401020` |
| disk prime / warm load | prime A=`0x401007` | warm B=`0x401020`；`loaded=2 compiled=0` |

eligible flags-fault guest 在 external/self 规范化后的 pre-producer load 故障，function/
lazy 均从 block recovery 返回 PageFatal `0x401010`。pin 0–3 × XMM fault sink 0/1 共
8 格全部得到同一结果。另跑三项结构化 fault 测试，8 个 pin/sink 组合均为
`16 assertions / 3 cases` 通过。

P1 + `SVM_JIT_CACHE` 明确输出
`SVM_BACKEDGE_FLAGS is incompatible; cache disabled`，没有用 imprecise cache entry
冒充 recovery。

## 4. flags 正确性、external entry 与 fuzz

- W59 branch-only directed matrix：function/block/interpreter × 原开关两态，6/6 PASS；
- Add/Sub/Cmp/Test、8/16/32/64、normal/inverted CF 与 LAHF/PUSHF/SETcc/CMOV/
  ADC/SBB/Jcc 的基本语义由既有 differential fuzz 覆盖；固定 seed
  `0x81,0x8101..0x8104`，每 seed 4/4 cases PASS。这里必须限定口径：本轮没有另造
  一个把上述维度逐项做 Cartesian product、并同时证明每个 case 都命中 P1
  eligibility 的专用 guest，因此不能把 broad fuzz 写成“完整 P1 交叉矩阵已穷举”；
- fault filters：pin 0–3 × XMM fault sink 两态，8/8 PASS；
- external link/RSB/cache-miss：func_tests function/block/interpreter 在 OFF 与
  LATCH+FLAGS 两态 stdout SHA-256 全为
  `9c3194ff498da03869bbabbd81241fd2ec771619281f969c4b4edc55577a6810`，rc=101，
  checksum `9f52b7d59285dbe5`；
- cold branch 先 materialize 再 Forward；request 与 branch decision 竞争时只可能提交
  loop-head 或 cold-successor 的一套完整状态，不存在两边混合。

## 5. 静态形态、cold bytes 与 metadata 净账

W71 运行期窗口的精确结果：

| STREAM block | OFF | P0 LATCH | P1 steady | W79 完整热形态口径 |
|---|---:|---:|---:|---:|
| Scale `0x401a70` | 45 | 47 | **32** | **41 = 32+9 cold-link** |
| Add `0x401b00` | 48 | 50 | **35** | **44 = 35+9** |
| Triad `0x401ba0` | 56 | 58 | **43** | **52 = 43+9** |

所以 P0 poll 正好 +2；P1 相对“旧 flags 全物化 + 新 poll”稳态净删 15 条，其中 9 条
是把旧 inline cold-link tail 移出 W71 热窗口，真正每次回边的净收益仍是 W79 预估的
`-6+2=-4`。

不能只报热窗口。完整 emitted unit bytes 为：

| block | OFF | P0 | P1 | P0→P1 |
|---|---:|---:|---:|---:|
| Scale | 256 | 300 | 392 | +92 B |
| Add | 268 | 312 | 404 | +92 B |
| Triad | 328 | 372 | 464 | +92 B |

P0 每个 self unit 增加 8B 热 poll 与约 36B cold exit stub。P1 相对 P0 的热窗口少
60B，但 external polarity normalization、cold-successor materialize、fault recovery
等 out-of-line code 合计多 152B，净增长 92B；这是动态极冷但真实的 I-cache/code
cache 成本。

metadata 也增长：旧 `FaultEntry` 24B，新项 40B。N-block function 从一个 24B entry
变为 fallback + N 个 subrange，即 `40*(N+1)`，增量 `40N+16`；block-mode P0 单项
增 16B，P1 eligible block 的 whole fallback + precise recovery 为 80B（相对旧 +56B）。

## 6. RA shape

STREAM 两态：

- spill_units/defs/loads/stores：均 0；spill high-water 均 0；
- max live GPR/FPR 峰值仍为 7/8；scratch GPR/FPR 均为每 unit 3；
- helper：30 calls，snapshot 526 instructions / 2,104 code bytes /
  10,208 memory bytes，两态完全相同；
- pair fallback 无增长。

结论：P1 没有借缩池、增 spill 或 helper snapshot 换取 STREAM 收益。

## 7. 性能 A/B 与 interrupt latency

本节所有正式数据均为 orb Linux 安静窗口、交错顺序、oracle 通过。CI 为 paired
percentage delta 的双侧 95% t-CI。

### 7.1 P1 增量：LATCH-only → LATCH+FLAGS

| 语料 | paired mean | 95% CI | 裁定 |
|---|---:|---:|---|
| STREAM Copy | +7.811% | `[+7.013%, +8.609%]` | 正 |
| STREAM Scale | +19.142% | `[+17.996%, +20.287%]` | 正 |
| STREAM Add | +16.943% | `[+16.095%, +17.791%]` | 正 |
| STREAM Triad | +14.044% | `[+12.793%, +15.295%]` | 正 |
| CoreMark 150k | **-0.630%** | `[-1.871%, +0.611%]` | **未过正 CI 门** |

CoreMark 7/7 都通过 CRC validated。首轮另有一个 8.9k it/s 的明确受载样本，完整保留
在 `/tmp/w81-coremark-ab-first.tsv`，没有混入上表；安静重跑的 7 对仍不支持正收益。

### 7.2 完整开关：OFF → LATCH+FLAGS

最终 7 对在独立、空闲的 `wine-ci` ARM64 Orb 上完成；四项 STREAM 均 7/7
`Solution Validates`，CoreMark 7/7 CRC validated。原始数据：
`/private/tmp/w81-stream-full-ab.tsv`、`/private/tmp/w81-coremark-full-ab.tsv`。

| 语料 | paired mean | 95% CI | 裁定 |
|---|---:|---:|---|
| STREAM Copy | +6.384% | `[+6.174%, +6.595%]` | 正 |
| STREAM Scale | +13.925% | `[+13.797%, +14.052%]` | 正 |
| STREAM Add | +10.875% | `[+10.781%, +10.969%]` | 正 |
| STREAM Triad | +9.069% | `[+8.688%, +9.450%]` | 正 |
| CoreMark 150k | **-0.844%** | `[-1.652%, -0.035%]` | **显著负向** |

完整开关的 CoreMark CI 已全落在零下；它比 §7.1 的 P0→P1 结果更直接地证明，对低
eligibility 语料，P0 poll 成本没有被六条 sink 偿还。

### 7.3 低覆盖反回归

三对交错 smoke 数据如下，原始表为 `/private/tmp/w81-anti-ab.tsv`。正值表示 P1 更快；
c-ray 用程序打印的 render 秒数，7zip 用 `Tot` MIPS，OpenSSL 用 SHA256 kB/s。

| 语料 | paired mean | 95% CI | oracle |
|---|---:|---:|---|
| c-ray `-s 4 -d 128x96 -j1` | **-1.258%** | `[-8.175%, +5.658%]` | OFF/P1 canonical PNG IDAT 都为 `d0348e6265ed38e5ddb39d5b549805b4` |
| 7zip `-mmt1 -md=4m` | +1.854% | `[-2.764%, +6.471%]` | 6/6 有唯一 `Tot` row |
| OpenSSL SHA256 | -0.609% | `[-1.282%, +0.063%]` | 6/6 有唯一 result row |

c-ray 的完整 PNG 文件 MD5 会随动态 metadata 改变，不能作像素 oracle；按 harness 的
口径只签名拼接 IDAT payload 后 OFF/P1 精确一致。三对 smoke CI 都较宽，不用于声称
收益；c-ray 点估计回归超过 1%，已经触发“维持 OFF”纪律，方向也与 CoreMark 硬门
失败一致。

### 7.4 interrupt latency

结构上为 `LDAR` 采样间隔的一次 guest iteration 上界。`alarm(1)` 的 host-wall
上界样本会把 alarm 之前的 loader/JIT 启动、timer 调度、signal frame 安装和进程退出
都算入。10 次 mac P0 的 `wall-1s` 中位/最大为 **40.319/47.493ms**，10 次 P1 为
**40.213/41.143ms**；这是保守端到端上界，不拿它替代上述“一次 iteration”的
指令级上界。

## 8. proof fallback 清单

STREAM 三个目标块全部 eligible，无 emitter fallback。CoreMark 27 个 self/terminal
候选的结果如下：

- eligible：`0x405fd0`；
- 缺目标 final PF/AF/NZCV producer（多数已由 W59 branch-only 消除）：
  `0x402680, 0x402aa8, 0x402af8, 0x402cd0, 0x402d58, 0x402df8,
  0x402ea0, 0x402ed8, 0x403128, 0x403286, 0x403778, 0x4037d0,
  0x403880, 0x4038e0, 0x403908, 0x403930, 0x403990, 0x47edc0`；
- polarity LoadImm 不是单 use：`0x4038a8, 0x403958, 0x4039b8`；
- pre-producer emitter 不在 NZCV-neutral allowlist：
  `0x406c58(op81), 0x406cb0(op168), 0x40ec50(op124),
  0x4178c0(op127), 0x46e910(op92)`。

这些拒绝是本任务要求的逐 block 保守回退，不是 function 级“部分相信”。

## 9. 全部门与最终裁定

| 门 | 结果 |
|---|---|
| `git diff --check` | PASS |
| OFF fingerprint vs `/tmp/svm-build` | 4,400 function units；cross-build 零 diff；same-binary host bytes 自一致 |
| ON fingerprint | 4,402 function units；same-binary host bytes 自一致 |
| mac full | 139975 assertions / 124 cases |
| orb full | 142585 assertions / 124 cases |
| func_tests 六格 | 全部 rc=101，checksum/SHA 完全一致 |
| fixed-seed fuzz | 5 seeds × 4 cases，全过 |
| P1 要求的 producer/width/polarity/consumer 全 Cartesian product | **未单独穷举；broad fuzz 不能替代 eligibility 见证，记为未过门** |
| SMC stress | 200/200 |
| golden | 未修改 |

最终建议：

1. P0 已完成正确性 infra spike，默认 OFF；其 latch/fault-map 机制可以作为后续 region/
   deopt 工作的基础，但若讨论默认 ON，必须另交 dispatcher 间接 load、poll 与 metadata
   的全语料成本。
2. P1 不翻盘。STREAM 强正不能覆盖 CoreMark 的 proof 命中塌缩；CoreMark 正 CI 硬门
   已失败，且完整 correctness Cartesian product 未交付，任一项都足以满足 W79
   “失败即撤”的停止条件。
3. 若重开，先解决“W59 已吸收大部分 CoreMark self loop、P0 poll 却仍全付”的覆盖
   失配；不能放宽 fault/helper proof，也不能靠减少 poll 频率突破一次 iteration 的
   interrupt latency 契约。

## 10. 清洁度

- 未 commit/push/add/checkout/reset/stash；
- 未修改 `source/translator/linux/linker/`、
  `SwiftVM-bench/harness/run_matrix.sh` 或 golden；
- 临时 guest/probe 均在 `/private/tmp/w81-*`，不属于仓内改动。
