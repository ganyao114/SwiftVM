# W67：RA 形态只读计数器探针与七语料实测

日期：2026-08-01  
基线：`d6acf74`（master，含 W64/W65）  
工作分支：`w67-ra-prof`

## 1. 结论

`SVM_RA_SHAPE_PROF` 已落地为默认关闭的翻译期只读探针。关闭时不发射任何新指令，函数指纹对 `/tmp/svm-build` 为零 diff；打开时，两个自一致轮次连同 `host_bytes` 完全一致，且对 master 的 unit/IR 指纹仍为零 diff。

七语料给出的路线图裁定是：

- **P1 暂不立项。** XMM OFF 的 59,528 个 unit 中只有 25 个 spill unit（0.04200%），最高水位 10/64 slot；XMM ON 为 22/59,494（0.03698%），最高仍为 10。容量离 64 很远。
- **P3 的静态推断得到实测确认。** `SVM_XMM_STATIC=1` 的 59,494 个 unit 全部报告 FPR pool=12，不是 16；动态 FPR live 分布为 `0:57148, 1:1876, 2:468, 3:2`，峰值仅 3。释放 v11-v14 会把所有 unit 的物理池和 value headroom 精确增加 4，但当前语料没有显示 FPR RA spill 是主要矛盾。
- **P2 只有有限但真实的静态收益面。** 七语料共到达 13 个 direct helper target、884 个翻译 call site；严格 leaf 只有 `Bsr64`、`Bsf64`、`Sse42StrStage` 三个 target，共 229 site（25.90%）。XMM ON 样本中 leaf 为 228/883（25.82%）；只去掉每个 leaf 的 16 条 Q save/restore，静态上限是 3,648 条 snapshot 指令（14,592 code bytes），占全部 helper snapshot 指令 12.07%。这不是运行时调用频度，不能据此给墙钟收益。
- **P5 不过立项门。** 当前实现 PF/AF producer 都会 materialize，实测均为 100%；即使模拟 unit-local source，首次不可越过边界给出的强制 materialize 下界仍为 PF 38,549/39,184（98.38%）、AF 18,263/18,655（97.90%），远高于 `<0.5` 门槛。

因此建议保持现有默认档位：`SVM_RA_SHAPE_PROF` 默认 OFF；P1/P5 不进入实现 A/B，P2 只能继续做严格 leaf 原型，P3 可作为 cold-edge ABI/稳健性工作独立评估，不能从本次数据宣称性能收益。

## 2. 实现

### 2.1 开关、聚合与输出

- 新增 `SVM_RA_SHAPE_PROF`，未设置或设为 `0` 时关闭；设为 `1`/`stderr` 输出到 stderr，其他非零字符串作为输出路径。
- `PerfStats2::kGetenvNames` 从 53 扩为 54 并登记新名称。
- RA 在每个 unit 内使用普通局部结构累计；unit 成功 `FinalizeCode` 后才以 relaxed atomics 合入进程总计。多线程翻译没有普通整数数据竞争，JIT 生成代码中没有计数器或额外指令。
- 进程退出时输出总计和直方图；direct helper 地址以相对 `svm_ra_shape_prof_anchor` 的 delta 输出，既可跨 ASLR 做 post-link 解析，也不增加 `dladdr`/`libdl` 依赖。

### 2.2 计数定义

- RA：只记录最终通过验证的 allocation pass；W65 因 spill 预留 x18 后的重跑会覆盖首轮，首轮不重复计数。`max_live` 是 emitter 可见的保守 live/dirty 集，包含活跃 spill def；instruction-local scratch 由独立的 `scratch_gpr/fpr` reserve 表示。
- spill：`spill_defs` 来自 RA 分配；`spill_loads/stores` 在实际发射 `Ldr/Str` 时计数；high-water 是同时占用的 u64 slot 顶点。连续 pair fallback 对当前唯一的连续请求 `TryGetConsecutiveTmpV2` 计数。
- helper：按 `direct_aapcs`、`indirect_aapcs`、`xstate_sync_aapcs` 分类，记录翻译到达并发射的 call site、实际 caller snapshot 指令数、code bytes、内存传输 bytes，以及 XSAVE/XRSTOR 的静态 XMM 同步量。
- flags：`SaveFlags` 与 `PublishFCmpFlags` 计 producer 和当前实际 materialize；`LocalParitySet`、PF/AF `TestFlags` 以及 `FCmpCondSet(VS/VC)` 计直接消费。另沿优化后的 IR 模拟 source 保持，按首次不可越过的 edge/helper/fault/other 边界计强制 materialize 下界。

### 2.3 有意取舍

“动态 call 次数”在本探针中定义为**实际工作负载触发翻译并发射的 call site 次数**，不是 guest 运行时执行次数。后者必须向 JIT 热代码插入 increment，与“探针不得进入 JIT 发射热代码”和 ON 仍不改变发射的硬约束冲突，因此没有实现。由此，helper 数据衡量代码面和 ABI 改造覆盖面，不能直接加权墙钟。

callee prologue/epilogue 与普通下游调用依赖最终链接产物，探针只输出 direct target delta；本报告对 Orb Release ELF 用 `nm -n -S -C` 与范围限定的 `objdump -d -C` 做 post-link 审计。间接 target 无法静态归属；本批语料恰好没有 indirect 或 xstate-sync site。为控制输出量，每个 unit 不单独打印一行，而是输出 pool/live/reserve/high-water 直方图和 spill 总计。

flags 的 `materialize` 是当前代码真实次数；`force_*` 是 P5 source 形态的保守预测。一次直接消费后同一 source 仍可能在后续 edge/fault 被迫物化，因此 direct 与 force 不是互斥分桶。

## 3. 采样方法

Orb `wine-ci`（Linux arm64），W67 构建目录 `/tmp/w67-build`，`SVM_JIT_CACHE=`，每项各跑 `SVM_XMM_STATIC=0/1`：

- CoreMark：150,000 iterations，已输出 `Correct operation validated`。
- STREAM：默认参数。
- smallpt：64 spp，320×240。
- SQLite speedtest：size 100，`main,orm/25,cte/20,json,fp/3,parsenumber/25,star,app`。
- c-ray：1 thread，64 samples，320×240。
- 7-Zip：`b -mmt1 -md=16m`。
- OpenSSL-speed：SHA-256 与 AES-128-GCM 各 8 秒，下面合并为一个语料行。

所有进程 rc=0；CoreMark CRC 判据通过。unit 数是该次执行实际到达的翻译集合，因此 CoreMark/c-ray/OpenSSL 的两态总数有极小启动路径波动；比率均按各自分母计算。

## 4. 七语料数据

### 4.1 默认形态（XMM static OFF）

| 语料 | units | spill units | defs / loads / stores | slot HW | helper sites | strict leaf | PF force/prod | AF force/prod |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| coremark | 1,694 | 0 | 0 / 0 / 0 | 0 | 24 | 37.50% | 1,108/1,129 (98.14%) | 550/564 (97.52%) |
| stream | 1,435 | 0 | 0 / 0 / 0 | 0 | 30 | 40.00% | 976/1,008 (96.83%) | 500/522 (95.79%) |
| smallpt | 1,769 | 0 | 0 / 0 / 0 | 0 | 27 | 40.74% | 1,224/1,247 (98.16%) | 543/558 (97.31%) |
| sqlite | 21,453 | 5 (0.0233%) | 1,056 / 1,078 / 832 | 9 | 344 | 18.02% | 14,941/15,160 (98.56%) | 6,937/7,067 (98.16%) |
| c-ray | 8,357 | 4 (0.0479%) | 174 / 307 / 172 | 8 | 191 | 20.42% | 5,346/5,417 (98.69%) | 2,715/2,763 (98.26%) |
| 7zip | 6,944 | 7 (0.1008%) | 446 / 530 / 433 | 8 | 84 | 40.48% | 4,204/4,379 (96.00%) | 2,349/2,448 (95.96%) |
| openssl-speed | 17,876 | 9 (0.0503%) | 1,699 / 2,320 / 1,679 | 10 | 184 | 33.70% | 10,750/10,844 (99.13%) | 4,669/4,733 (98.65%) |
| **合计** | **59,528** | **25 (0.04200%)** | **3,375 / 4,235 / 3,116** | **10** | **884** | **229 (25.90%)** | **38,549/39,184 (98.38%)** | **18,263/18,655 (97.90%)** |

全部语料 `pair_fallbacks=0`、`target_overflow=0`。默认 FPR pool 恒为 28；聚合 `max_live_fpr` 峰值 18，普通 reserve=3，只有 25 个 spill unit 使用了更高 reserve（FPR reserve 5/6/7）。大量 spill def 仍只复用到最高 10 个 slot，说明当前 slot 回收有效，P1 的 64-slot 容量风险没有出现。

GPR pool 直方图为 `11:25, 12:59503`：恰好只有 W65 条件化 x18 的 25 个 spill unit 从 12 缩为 11。`max_live_gpr` 为 `0:1361, 1:3434, 2:19580, 3:26823, 4:5648, 5:2001, 6:472, 7:117, 8:24, 9:57, 10:3, 11:1, 12:3, 13:4`；GPR scratch reserve 为 `3:59503, 5:1, 8:16, 9:8`。高于物理 pool 的 live 值由 spill 承载，不表示寄存器越界。

### 4.2 XMM static ON 的 FPR 实测

直方图写作 `live值:unit数`；reserve 列只列非默认 unit，未列者均为 3。

| 语料 | units | FPR pool | max-live FPR histogram | 非默认 FPR reserve | spill units / HW | helper snapshot insns / code bytes |
|---|---:|---:|---|---|---:|---:|
| coremark | 1,701 | 12 | 0:1648, 1:44, 2:9 | 无 | 0 / 0 | 806 / 3,224 |
| stream | 1,435 | 12 | 0:1358, 1:53, 2:24 | 无 | 0 / 0 | 1,006 / 4,024 |
| smallpt | 1,769 | 12 | 0:1541, 1:166, 2:62 | 无 | 0 / 0 | 904 / 3,616 |
| sqlite | 21,453 | 12 | 0:20933, 1:456, 2:64 | 6:4, 7:1 | 5 / 9 | 11,892 / 47,568 |
| c-ray | 8,323 | 12 | 0:7666, 1:501, 2:156 | 7:1 | 1 / 7 | 6,474 / 25,896 |
| 7zip | 6,944 | 12 | 0:6685, 1:205, 2:54 | 6:4, 7:3 | 7 / 8 | 2,830 / 11,320 |
| openssl-speed | 17,869 | 12 | 0:17317, 1:451, 2:99, 3:2 | 6:7, 7:2 | 9 / 10 | 6,302 / 25,208 |
| **合计** | **59,494** | **12** | **0:57148, 1:1876, 2:468, 3:2** | **6:15, 7:7** | **22 / 10** | **30,214 / 120,856** |

普通 unit 当前 value capacity 为 `12-3=9`；reserve=7 的最窄形态仍为 5。观测 max live 不超过 3，因此当前 headroom 的保守下界为 2，普通形态至少为 6。P3 释放 v11-v14 后 pool 变为 16，所有这些数精确增加 4：普通 capacity/headroom 至少 13/10，最窄形态至少 9/6。由于 FPR max live 从未达到最窄 capacity，本批 spill 可判定不是 FPR 容量 spill；P3 的价值在 emitter scratch/cold-edge ABI 余量，而不是消除本批 RA spill。

### 4.3 helper ABI 与 post-link leaf 审计

| target 组 | target 数 | XMM OFF sites | prologue/epilogue | 普通下游调用点 | strict leaf |
|---|---:|---:|---|---:|---|
| `Bsr64`, `Bsf64` | 2 | 152 | 0 / 0 | 0 | 是 |
| `Sse42StrStage` | 1 | 77 | 0 / 0 | 0 | 是 |
| `DivQU64/RU64/QS64/RS64` | 4 | 402 | 每个 1 STP / 1 LDP | 共 4 个 BL 到 libgcc | 否 |
| `Sse42StrEval` | 1 | 77 | 0 / 0 | 1 个外部 tail `B` 到 EvalFast | 否 |
| `RepMovs`, `RepStos1/4/8` | 4 | 116 | Movs 4 对；Stos 各 2 对 | 共 10 个 BL | 否 |
| `X87Dispatch` | 1 | 60 | 6 STP / 对称恢复 | 123 个 BL/BLR | 否 |

XMM OFF 的全部 helper snapshot 为 16,138 instructions / 64,552 code bytes / 310,576 memory bytes。XMM ON 为 30,214 / 120,856 / 761,744；其中严格 leaf site 自身占 7,578 条 snapshot 指令。按 W66 的 preserve-all 上限，只移除 228 个 ON leaf site 的 16 条 Q save/restore，即 3,648 instructions、14,592 code bytes、116,736 memory bytes；不能移除 x0-x8 pin snapshot 或 JIT 自身的 x29/x30 link pair。

本语料所有 helper 均为 `direct_aapcs`，`indirect_aapcs=0`、`xstate_sync_aapcs=0`，因此后两类仍需专门语料才能给收益面。

### 4.4 PF/AF

XMM OFF 聚合：

- PF：39,184 producer，38 次直接消费，39,184 次当前 materialize；预测强制项为 edge 29,064、helper 44、fault 9,441、other 0，合计 38,549（98.38%）。
- AF：18,655 producer，0 次直接消费，18,655 次当前 materialize；预测强制项为 edge 12,677、helper 23、fault 5,563、other 0，合计 18,263（97.90%）。

XMM ON 的比率相同到舍入精度（PF 98.38%，AF 97.90%）。P5 的收益前提是 source 能跨过绝大多数当前 materialize；数据反而表明 edge/fault 已经覆盖几乎所有 producer，且直接消费极少，所以不进入实现。

## 5. 验收结果

| 门 | 结果 |
|---|---|
| OFF 指纹 vs master | PASS；4,400 function units，零 diff；同 build 两遍含 host_bytes 自一致 |
| ON 指纹自一致 | PASS；4,400 units，两遍含 host_bytes 一致；对 master unit/IR 仍零 diff；输出 16 行非空 profile |
| macOS 全量 | PASS；139,961 assertions / 123 cases |
| Orb W67 全量 | PASS；142,571 / 123 |
| Orb master 同条件对照 | PASS；142,571 / 123 |
| func_tests 六格 | PASS；function/block/interpreter × probe 0/1 均 rc=101、checksum `9f52b7d59285dbe5`、stdout SHA-256 `9c3194ff498da03869bbabbd81241fd2ec771619281f969c4b4edc55577a6810` |
| probe ON func_tests | PASS；1,130 units、16 helper sites，17 行 profile，聚合非空合理 |
| probe ON CoreMark | PASS；1,694 units、24 helper sites，18 行 profile，CRC `Correct operation validated` |
| `SVM_RA_SHAPE_PROF=1` stderr | PASS；hello rc=42，stderr 有 12 行聚合输出 |
| 七语料正确性 | PASS；16 个 XMM OFF/ON 子运行全部 rc=0，`target_overflow=0` |
| 静态检查 | `git diff --check` PASS |

没有改动 `source/translator/linux/linker/`、`SwiftVM-bench/harness/run_matrix.sh` 或 `func_fingerprint_golden.txt`，也没有执行 git commit/add/checkout/reset/stash/push。

## 6. 改动文件

- `source/runtime/common/ra_shape_prof.h`、`ra_shape_prof.cpp`：新探针、原子聚合、输出与 flags 投影。
- `source/runtime/ir/opts/register_alloc_pass.cpp`：最终 RA pass 的 pool/live/reserve/spill/high-water 采样。
- `source/runtime/backend/reg_alloc.h`：unit-local 计数载体。
- `source/runtime/backend/arm64/jit/jit_context.cpp`、`.h`：spill load/store、pair fallback 与成功 unit 提交。
- `source/runtime/backend/arm64/jit/translator_control.cpp`：helper ABI、snapshot 与 direct target 计数。
- `source/runtime/common/perf_stats.h`：环境变量注册表 53→54。
- `source/runtime/CMakeLists.txt`：加入新源文件。
- `docs/w67-ra-shape-prof.md`：本报告。
