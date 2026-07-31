# W69：c-ray 对 FEX 差距重分解与负载敏感性调查

日期：2026-08-01

SwiftVM 基线：`5a19dcc`（分支 `w69-cray-gap`）

性质：纯调研；未修改生产代码、benchmark harness 或 golden

## 结论摘要

1. **题设中的“安静机约 19s”与“受载膨胀 2.4–3.1 倍”都不是有效基线。** `baseline3` 的原始输出是 `1m 19s`/`1m 20s`，旧 parser 只取了末尾的 19/20；对应 host wall 实际为 79.316–80.629s。W64 已记录这一事实，本轮再次从原始 artifact 核对。因此不存在一条可信的 `19s -> 45–59s` 时间序列，也不能继续把“安静差距约 1.4 倍”当成已知事实。
2. **可控过载实验直接证伪了“SVM 对负载约比 FEX 敏感 3 倍”。** Orb 16 vCPU 中并发 24 个 `yes`，c-ray `-s 8` 三次中位：SVM `6.54 -> 11.67s = 1.784x`，FEX `1.86 -> 3.59s = 1.930x`。该实验里 FEX 反而略敏感，但三次离散较大，只应裁定“两者同量级、没有 SVM 独有的 3 倍机制”。
3. **当前约 3.45–3.49 倍差距主要是每个 guest 工作量对应的 host 执行形态，不是 off-CPU 等待。** Orb 自然负载三次中位为 SVM 44.52s、FEX 12.89s（3.454x），两者 `user ~= wall`。在 base/load 两态，SVM 主渲染线程的 12.278 亿次 exit、9.800 亿次 link hit、7870 万次 dispatch 等计数逐项完全相同；负载改变的是这些固定工作获得 CPU 的速率。
4. **三个同 guest 入口的代表热块揭示了可复核的形态差。** 关闭 FEX multiblock 以对齐 unit 边界，SVM 热主线合计 941 条、FEX 239 条。702 条差额中：state（spill+GPR+XMM）40.5%，move/宽度桥 22.8%，精确 NaN guard 14.5%，其余为 semantic/guest-memory 9.7%、dispatch 7.3%、flags 5.3%。其中最热的 `0x402fed` 单块有 84 条 `State::spill_area` 读写，说明 W67 的“静态 spill unit 仅约 0.05%”不能推出运行时不重要。
5. **目前没有足够数据批准一个声称有确定正收益的生产优化项目。** 可以立的是带动态计数与 A/B 门的 spike：优先做热点 spill/copy 联合归因；其次才是 unit-local XMM 值驻留。全局/跨 unit XMM pin、直接链接、SMC syscall、额外翻译线程均不应重开。文末给出的百分比均明确区分“实测”“宽松上限”和“净收益数据不足”；宽松上限不作为立项依据。

## 1. 数据完整性与实验边界

### 1.1 旧 19 秒基线为何作废

`docs/w64-cray-stall-root-cause.md` 已给出 baseline3 的原始对账：

| baseline3 SVM rep | 旧 `render_s` | `.status` host wall | guest 原始输出 |
|---:|---:|---:|---|
| 2 | 19 | 79.749s | `Finished render in 1m 19s` |
| 3 | 20 | 80.629s | `Finished render in 1m 20s` |
| 4 | 19 | 79.806s | `Finished render in 1m 19s` |
| 5 | 19 | 79.316s | `Finished render in 1m 19s` |

旧 awk 只解析 `$NF`，把 `1m 19s` 记成 19s。故：

- 不能用 19s 构造“安静机 SVM/FEX 约 1.4x”；
- 不能用 19s 与 post-W64 的 45–59s 构造“2.4–3.1x 负载膨胀”；
- baseline3 的约 79.8s 还早于后续优化波次，也不能拿来充当当前 `5a19dcc` 的安静基线。

post-W64 已有正式 artifact `results/cray-postw64`：SVM 45.23s、FEX 12.96s、比值 3.490，5/5 完成且 oracle 一致。本轮 Orb 自然负载三对交错复测得到 3.454x，与它互证。

### 1.2 本轮没有伪装成“安静机”的数据

正式采样前按要求检查了 `codex/cmake/swift_test` 等进程。构建/测试虽已结束，但 macOS 主机一直有其他长期计算任务；采样时 load average 约 6–14，收尾检查为 `8.31/23.23/21.10`。因此没有满足定义的安静窗口。

本报告把问题 2 改写为：

- 用当前可获得的运行对热 PC 和 host 代码形态取样；已生成的 JIT 代码不随宿主负载变化，适合回答“多发了哪些指令”；
- **不**把该样本宣称为安静机墙钟或安静机动态 cycle 分解；
- 真正的 current-master quiet wall A/B 仍是数据不足项，需在可验证的空闲窗口补跑。

### 1.3 方法与证据位置

| 目的 | 方法 | 主要证据 |
|---|---|---|
| 当前自然负载 A/B | Orb，SVM/FEX 交错，各 3 次，c-ray 正式参数 | `SwiftVM-bench/results/w69-evidence/orb-natural-ab/results.tsv` |
| 可控负载 A/B | Orb 16 vCPU；base 与并发 24 个 `yes`；各 3 次；`-s 8` | `.../orb-load-s8/results.tsv`、`.../orb-load24-s8/results.tsv` |
| 两态构成 | `SVM_EXEC_PROF=1 SVM_PROF2=1`，base/load 各一次 | `.../prof-base-load/{base,load}/err.txt` |
| syscall 构成 | Orb `strace -f -c`，SVM/FEX 各一次 | `.../strace/{svm,fex}/summary.txt` |
| macOS 热点 | `SVM_EXEC_MAP=1` + `sample` 8 秒 + host dump | `/tmp/w69-evidence/natural-load/`、`/tmp/w69-evidence/host-dump/` |
| RA 静态形态 | `SVM_RA_SHAPE_PROF=1` | `/tmp/w69-evidence/natural-load/ra-shape.txt` |
| FEX 同 PC 代码 | `FEX_BLOCKJITNAMING=1` 取 perf map，读 live JIT memory，AArch64 objdump | `.../fex-mb0-live/` |
| FEX multiblock 净值 | `FEX_MULTIBLOCK=1/0` 交错，各 3 次 | `.../fex-mb-ab/results.tsv` |
| JIT disk cache | 变化输出路径三轮；另做完全相同 argv 正对照 | `.../jit-cache/` 与 `/tmp/w69-cache-constant/` |

省略号均以 `/Users/swift/CLionProjects/SwiftVM-bench/results/w69-evidence` 为根。本轮没有为取证修改生产源码，也没有临时探针补丁；使用的四个 SVM 探针均为现有默认 OFF 开关。

## 2. 负载敏感性：测量结果

### 2.1 自然负载下 SVM/FEX 都是 CPU 执行受限

Orb 三对正式参数交错结果：

| engine | render 三次 | render 中位 | host wall 中位 | user 中位 | sys 中位 |
|---|---|---:|---:|---:|---:|
| SVM | 43.98 / 44.52 / 44.89s | 44.52s | 44.837s | 44.805s | 0.151s |
| FEX | 12.27 / 12.89 / 12.95s | 12.89s | 12.996s | 13.026s | 0.028s |

推导：`44.52 / 12.89 = 3.454x`。两边 user 与 wall 均近似相等；SVM 并没有把约三分之二墙钟花在睡眠、锁等待或翻译线程互等上。macOS SVM 长样本也一致：real 51.24s、user 50.71s、sys 0.15s，零 swap、零 block input，只有 34 次 voluntary context switch。

### 2.2 可控过载没有复现 SVM 独有超敏

`8 x yes` 未超过 16 vCPU，SVM/FEX 都没有变慢，故只作为负载发生器校准。正式压力采用 24 个 `yes`，采样时 runnable 为 26；load average 的 1 分钟 EWMA 尚未爬满，不用它计算倍率。

| engine | base `-s 8` 三次 | base 中位 | 24-worker 三次 | load 中位 | 膨胀 |
|---|---|---:|---|---:|---:|
| SVM | 6.30 / 6.54 / 6.54s | 6.54s | 11.67 / 10.52 / 14.51s | 11.67s | **1.784x** |
| FEX | 1.84 / 1.87 / 1.86s | 1.86s | 4.50 / 3.59 / 3.29s | 3.59s | **1.930x** |

负载三次离散意味着不能解读 1.784 与 1.930 的小差，但足以否定“只有 SVM 膨胀约 3 倍”。负载下两边 user CPU 也有相近幅度增加：

- SVM：base 中位约 6.80s，load 中位约 8.40s，约 +23.5%；
- FEX：base 中位约 1.99s，load 中位约 2.44s，约 +22.6%。

这符合共享 CPU 时的 off-CPU 等待，加上频率/迁核/cache 干扰使实际 CPU time 略增；不是某个 SVM 特有慢路径被触发。

### 2.3 固定 guest 工作的执行计数逐项不变

在 `-s 8` base/load 各一次的完整 profile 中，主渲染 Runtime：

| `SVM_EXEC_PROF` 项 | base | 24-worker load | 差值 |
|---|---:|---:|---:|
| exits | 1,227,775,283 | 1,227,775,283 | 0 |
| direct exits | 864,183,703 | 864,183,703 | 0 |
| indirect exits | 25,368,416 | 25,368,416 | 0 |
| call / ret | 169,111,418 / 169,111,417 | 相同 | 0 |
| link hit / miss | 979,959,975 / 323 | 相同 | 0 |
| RSB hit / miss | 169,111,335 / 82 | 相同 | 0 |
| dispatch | 78,703,519 | 78,703,519 | 0 |
| GPR / XMM uniform accesses | 1,329,148,813 / 12,187,474,762 | 相同 | 0 |

主执行 elapsed 从 6.718s 变成 17.550s，只是同一串工作被调度得更慢。这也是本轮对“负载机制”最强的反证。

## 3. 五个候选假设逐项裁定

### a. 翻译线程与执行线程抢核：证伪

**机制检查。** SVM 没有 c-ray 专用后台翻译池。`X86Core::Impl::Run()` 在当前 guest host thread 上收到 `CodeMiss` 后同步调用 `Translate(pc)`，随后恢复执行（`source/translator/x86/translator.cpp:1054-1083`）；`Translate` 还由一个 coarse mutex 串行保护（同文件 `711-716`）。`Runtime::Impl::Run()` 也是当前调用线程直接进入 JIT（`source/runtime/backend/runtime.cpp:335-405`）。

**计数。** base 翻译总计 190.9ms / 8327 calls；load 是 503.3ms / 8403 calls。负载下计时包含该线程被抢占，因而膨胀合理，但：

- base 翻译只占 7.142s wall 的 2.7%；
- load 翻译只占 18.549s wall 的 2.7%；
- 即使把 load 的全部 503ms 清零，也解释不了新增的约 11.4s。

`SVM_PROF2` 两态均为 `multi_units=0`；这表示当前 SVM 是单 unit 编译，不表示另有翻译线程。

**线程数。** c-ray `-j 1` 仍创建线程池：strace 中 SVM 与 FEX 均有 17 次 `clone3`，SVM profile 也出现一个主渲染 Runtime 加多个几乎空闲的 Runtime。FEX live thread 表为 18 个线程，真正活跃的是渲染线程；两引擎面对的是同一 guest 线程结构。

**裁定：证伪。** 不应增加并行翻译线程；它只会为约 0.2–0.5s 的一次性工作引入新的抢核与发布同步成本。

### b. SMC mprotect/写窗口频率在受载下放大：证伪

| 指标 | base | 24-worker load | 负载 wall 占比 |
|---|---:|---:|---:|
| `CloseWriteWindow` calls | 9,744 | 10,789 | — |
| close total | 29.82ms | 69.01ms | 0.37% |
| strace SVM `mprotect` | 784 calls / 2.882ms | 单独 syscall 样本 | 极小 |

close 次数有约 10.7% 的非确定路径差异，但绝对时间不足 70ms。即使全部消除，也远小于负载新增墙钟。FEX 的同一 strace 样本为 77 次 `mprotect` / 0.205ms；调用数虽少一个数量级，仍不能解释 3.45x 执行差距。

源码上 `CloseWriteWindow` 在每次 JIT 返回后执行（`runtime.cpp:369-383`），现有 `SVM_SMC_DIRTY_HINT` 可优化无工作快返；但本格实测上限仅几十毫秒，**不构成立项依据**。

### c. 受载导致 JIT cache 抖动或代码页回收：证伪负载机制；发现独立 key 问题

**内存代码 cache。** `Module::AllocCodeCache` 先在现有 arena 分配，不足时新增至少 32MiB 的 arena（`module.cpp:243-259`）；没有依据宿主负载主动驱逐热代码的策略。`ReclaimCode` 由 SMC retirement 路径调用，不是 OS load feedback。macOS 长样本零 swap、零 block input；没有代码页回收风暴证据。

**base/load 形态。** 两态编译 unit 数约 8302/8338，主线程 exit/link/dispatch 计数完全相同，没有受载触发的反复翻译或 cache miss 风暴。

**disk cache 的独立发现。** harness 每轮把 c-ray `-o` 指向不同 work path；而 `ComputeGuestId()` 默认 compatibility mode 哈希全部 argv（`source/runtime/backend/code_serial.cpp:752-778`）。因此新 cache 目录的 prime/warm/load 三次均为 `loaded=0`，并各自产生一个不同 cache 文件。这不是负载抖动，而是 key 失配。

完全相同 argv 的正对照：

| run | loaded | compiled | stored | render（`-s 1`，单次仅作功能对照） |
|---|---:|---:|---:|---:|
| prime | 0 | 8,253 | 8,087 | 875ms |
| same argv warm | 8,087 | 196 | 8,117 | 856ms |

说明 cache 本身能复用。已有 `SVM_JIT_CACHE_EXEC_ID=1` 只按 guest executable identity 建 key，正好排除输出路径；默认仍 OFF。全正式 mac profile 的总翻译只有 231ms / 51.24s = 0.45%，所以即使完全复用，当前 c-ray 全量收益上限也不足约 0.5%，不能解释主差距。

### d. block link/direct link 在受载下改变行为：证伪

`SVM_BLOCK_LINK` 默认 ON，`SVM_DIRECT_BLOCK_LINK` 默认 OFF。base/load 的 link hit、link miss 和 dispatch **逐项相同**，见 2.3 节；load 不会改变 link 决策。

现有 direct-link 开关只供测量：源码明确指出它把 host 地址烘进源块、没有 incoming-link invalidation，SMC 后可能跳到已释放代码；还会禁用 JIT disk cache（`source/translator/x86/translator.cpp:636-685`、`jit_cache.cpp:93-97`）。它不是可发布的性能候选。

FEX `FEX_MULTIBLOCK=1/0` 的全参数交错三次中位：

| FEX 模式 | 三次 render | 中位 |
|---|---|---:|
| multiblock ON | 12.73 / 13.22 / 12.77s | 12.77s |
| multiblock OFF | 13.74 / 13.78 / 13.79s | 13.78s |

ON 相对 OFF 减时 `(13.78-12.77)/13.78 = 7.33%`，或 1.079x speedup。它是真实收益，但只能解释 FEX 自身约 7%，远非 SVM/FEX 的 3.45x；也不能把该百分比直接移植成 SVM region JIT 的预估。

### e. guest 线程数/额外 runtime 线程带来的特殊抢占：证伪

- 两引擎运行同一个 c-ray，strace 均观察到 17 个 `clone3`；FEX live 为 18 threads。
- `-j 1` 只把渲染工作限定为一个 local worker，不取消 c-ray 的其余线程结构。
- SVM profile 中只有一个 Runtime 承担 12.278 亿 exits；其余 Runtime 的 exit 数比主线程小多个数量级，多为等待/控制线程。
- 自然负载下两边 `user ~= wall`；可控过载下 SVM 与 FEX 膨胀同量级。

**裁定：证伪。** 现象是单个重 CPU worker 与其他宿主任务争 CPU，不是 SVM 多出一组活跃 runtime worker。

## 4. 热区与同块反汇编方法

### 4.1 SVM 热 PC

macOS 8 秒 `sample` 共 6242 个目标线程样本，借助 `SVM_EXEC_MAP` 将 6141 个映射到 JIT unit。前列 unit：

| guest entry | samples | 占 mapped | guest 符号/区域 |
|---:|---:|---:|---|
| `0x402fed` | 928 | 15.11% | `traverse_bvh_generic` 热循环 |
| `0x403088` | 376 | 6.12% | 同一 BVH traversal 热区 |
| `0x402e70` | 369 | 6.01% | 同一 BVH traversal 热区 |
| `0x407450` | 364 | 5.93% | `rayIntersectsWithPolygon` |
| `0x40308c` | 240 | 3.91% | BVH traversal 相邻块 |
| `0x403110` | 209 | 3.40% | BVH traversal 相邻块 |

以下精确对齐选择 `0x402fed`、`0x403088`、`0x407450`；三者合计 1668/6141 = 27.16% mapped samples。没有把相邻 PC 强行并入，以免把不同 unit 边界混算。

### 4.2 对齐与计数纪律

1. SVM 用现有 host dump 直接取得每个 guest entry 的 JIT bytes。
2. FEX 安装版启用 `FEX_BLOCKJITNAMING=1` 生成 `/tmp/perf-PID.map`，从 live `/proc/PID/mem` 取同 guest entry 的 JIT bytes；用 clone `f2e35f336f0b` 辅助核对机制。
3. 为同块比较，FEX 取 `FEX_MULTIBLOCK=0`；默认 ON 会从更早入口融合多个 guest block，不能与 SVM 单 unit 直接比长度。
4. 只计运行时热主线：
   - SVM 总 dump 还包含 NaN cold correction stub；三个 unit 总条数分别为 382/273/677，热主线切出 302/193/446；
   - FEX perf map 尺寸还含 link record/metadata，按终端和数据边界剔除；
   - 不把 cold stub 的静态体积当成每轮执行成本。
5. 分类互斥且以 state base `x28` 为准：`State::spill_area` offset 144–655 记 spill，uniform state 从 656 起；XMM 区间从 816 起；`x26`/NZCV pack 记 flags；`fcmp value,value` + `b.vs` 记 NaN guard；`mov/fmov` 与 W/X、GPR/FPR 桥记 move/width；terminal 后的 lookup/link 序列记 dispatch。

这是一份**静态热主线指令构成**，不是 cycle attribution。某些 load、branch、FP op 延迟不同，不能直接把条数比例当墙钟比例。

## 5. 热块指令级差距重分解（不冒充 quiet wall）

### 5.1 三个精确块

| guest entry / engine | 热主线总数 | spill | GPR state | XMM state | NaN guard | flags | move/宽度桥 | dispatch | semantic + guest mem |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `0x402fed` SVM | 302 | 84 | 19 | 21 | 24 | 23 | 63 | 17 | 51 |
| `0x402fed` FEX | 48 | 0 | 0 | 0 | 0 | 4 | 9 | 2 | 33 |
| `0x403088` SVM | 193 | 0 | 14 | 40 | 24 | 14 | 38 | 17 | 46 |
| `0x403088` FEX | 36 | 0 | 0 | 0 | 0 | 3 | 4 | 2 | 27 |
| `0x407450` SVM | 446 | 0 | 32 | 74 | 54 | 8 | 131 | 26 | 121 |
| `0x407450` FEX | 155 | 0 | 0 | 0 | 0 | 1 | 59 | 5 | 90 |

三个块合计 SVM 941、FEX 239，静态条数比 3.94x；当前自然负载墙钟约 3.45x。两者数值接近只是交叉验证，不能据此认定一条 host 指令等成本。

### 5.2 702 条差额如何组成

| 类别 | SVM | FEX | 差额 | 占 702 条差额 |
|---|---:|---:|---:|---:|
| spill | 84 | 0 | 84 | 12.0% |
| GPR state | 65 | 0 | 65 | 9.3% |
| XMM state | 135 | 0 | 135 | 19.2% |
| **state 小计** | **284** | **0** | **284** | **40.5%** |
| move/宽度桥 | 232 | 72 | 160 | 22.8% |
| 精确 NaN guard | 102 | 0 | 102 | 14.5% |
| semantic + guest memory | 218 | 150 | 68 | 9.7% |
| dispatch | 60 | 9 | 51 | 7.3% |
| flags | 45 | 8 | 37 | 5.3% |
| **合计** | **941** | **239** | **702** | **100%** |

因此，state + move + NaN + flags + dispatch 这些 bookkeeping 类合计解释 `634/702 = 90.3%` 的静态差额。与 W41–W44 的“state 第一大类”方向一致，但 W42 的旧墙钟归因比例不能沿用；本表是新的同块证据。

### 5.3 最重要的新发现：静态稀有 spill 被热点反转

本次 `SVM_RA_SHAPE_PROF` 全程序为：8307 units、12 spill units（0.144%），spill load/store 423/331，spill high-water 最大 8/64。从静态 unit 数看仍很稀少。

但最高热的 `0x402fed` 正是 spill unit，热主线 302 条里有 84 条 spill-area load/store，占该块 27.8%；它本身又占 mapped samples 15.11%。这否定了“spill unit 少，所以 c-ray spill 不重要”的推理。W67 的全语料静态结论没有测动态热度，本轮补上了缺口。

同时要避免反向过度推断：

- 以 `15.11% samples * 84/302 = 4.20%` 得到的只是“若这些指令等成本且全部可消除”的全程**宽松上限**；
- spill 可能是高压力活跃值的必要代价，消除它可能增加其他 move、延长 live range 或挤爆 scratch；
- 没有候选实现 A/B 前，不存在可承诺的正向净收益。

### 5.4 XMM、NaN 与 FEX 语义边界

三个块中 XMM state 差额 135 条、NaN guard 差额 102 条。FEX 的寄存器驻留形态确实避免了热块内的 XMM context 往返；它的这些块也没有 SVM 的逐操作 NaN self-compare guard。

但不能把 237 条全部列为“安全可删”：

- `SVM_XMM_STATIC=1` 单开历史实测 c-ray **-26%**，host code +16.4%、向量栈流量 +256%；W17 的 partial/lazy/evict 路径三轮全负，跨 unit XMM 驻留轴已关闭。
- 当前 XMM static ON 后动态 FPR 池只有 12 个，NaN cold edge 还固定占 v11–v14；直接照搬 FEX pin 会重新触发已知寄存器压力和 helper snapshot 边界成本。
- SVM 的 NaN cold path 用 guard 保持精确 x86 NaN payload/符号与 invalid indefinite 语义。FEX 不发 guard 不证明我们可无条件删除；必须是数据流证明 finite，或引入可验证的 speculation/deopt，才能算正确候选。

因此，本轮只允许研究 **unit-local、pressure-aware 的 SSA/coalescing**，不重开全局静态 pin 或跨 unit residency。

## 6. 可立项候选与边界净账

下表中的“上限”均按三个已对齐热块和 sample 权重推导，是宽松上限，只能用于淘汰或确定探针优先级，**不能用于默认 ON 或承诺收益**。

| 优先级 | 候选 | 推导链与收益口径 | 必须先记的边界成本 | 风险 | 建议开关 | 裁定 |
|---|---|---|---|---|---|---|
| P0 | 修正 benchmark disk-cache identity | 当前 full profile 翻译 `231ms / 51.24s = 0.45%`；相同 argv 实测 loaded 8087、compiled 196。全参数预计净值 **0–0.5%**，不是主差距 | cache header/guest-bytes 校验、不同 codegen env 隔离；现有实现已覆盖 | 低 | 已有 `SVM_JIT_CACHE_EXEC_ID=1` | 可作为 benchmark 配置卫生，不算新生产优化 |
| P1 | 热点 spill + tied/copy 联合 coalescing spike | spill-only 宽松上限 4.20%；move/width 三块加权宽松上限 6.10%，两者高度重叠。**净收益数据不足，无正向下界** | live range 延长、scratch 获取、terminal/cold-edge use、host code growth、其他 unit 新增 spill；必须按动态执行频率而非 unit 数记账 | 中 | `SVM_RA_HOT_COALESCE=1` | **可立测量/设计 spike，不足以立实现项目** |
| P2 | unit-local XMM SSA residency/重复 state 往返消除 | spill+GPR+XMM 三块加权宽松上限 9.33%，其中 XMM 单项 3.30%；**净收益数据不足**，9.33% 仅为淘汰上限 | 每个候选必须给出新增 max-live FPR、spill、scratch、helper/fault edge 强制物化和 code size；任一越过旧负格即停 | 高 | `SVM_XMM_BLOCK_RESIDENCY=1` | 仅在 P1 pressure 计数器就绪后做 spike；不得跨 unit |
| P3 | proven-finite NaN guard elimination | 三块 guard 加权宽松上限 2.68%；**净收益数据不足，无正向下界** | finite provenance 穿过 load/call/bitcast 的正确性；每个未证明值必须保留现路径；NaN pressure 测试与 fingerprint | 很高（正确性） | `SVM_SSE_NAN_PROVEN_FINITE=1` | 可做 IR proof spike；不能做启发式 fast-math |
| P4 | SMC-safe region/multiblock JIT | FEX 自身 ON 相对 OFF 实测减时 7.33%，只是外部参考；SVM 净收益 **无法预估** | region entry/exit state flush、flags/XMM 失效、SMC incoming links、fault map、disk-cache relocation、code growth、compile latency | 很高 | `SVM_REGION_JIT=1` | 当前不立项；先交边界设计与计数，不能借 FEX 7.33% 立项 |

### 6.1 P1 的最小正确 spike

P1 不应一开始改分配算法，而应先在现有 `SVM_RA_SHAPE_PROF` 旁补动态可归因证据：

1. 让 JIT 为 spill load/store、RA-introduced copy、width bridge 各记独立执行计数，限定开关 ON 时才进热代码；或仅对 top-N guest PC 生成 instrumented build。
2. 对 `0x402fed` 输出 interval 冲突图：每个 spill slot 的 def/use、跨 terminal/cold edge 情况、触发 spill 的同时 live 集。
3. 候选必须同时报：删掉多少 spill/copy、新增多少别类 move、新增 spill 的 unit 数、code bytes、scratch escalation/PANIC 计数。
4. A/B 先用 `-s 8` 多轮 oracle，再跑正式 c-ray 与全 benchmark 防回归。只有下界显著为正才进入生产实现。

这条路线针对的是动态热点反例，不推翻 W67 “全局扩 spill 容量不立项”的结论。

### 6.2 P2 必须与 pressure guard 捆绑

P2 只允许单 unit 内把可证明同一 guest XMM version 的 load/store 留在动态 FPR 中。硬门：

- 不改变 static pin map；
- 不保留 v16–v31 跨 unit；
- 每个 transformation 在 RA 前估算 peak live，RA 后若出现新 FPR spill 或 scratch escalation 则回退该 unit；
- helper、fault、uniform barrier、guest memory fault-before-commit 边必须恢复精确 state；
- 单独记录物化边界成本，不能只报删掉 135 条 state 指令的机械数。

如果热点 XMM 往返本来就是 fault/观察边强制物化，P2 会很快被计数否掉；这是合理结果。

### 6.3 明确不建议的候选

| 候选 | 不建议理由 |
|---|---|
| 增加翻译线程/并发度 | 翻译只占 0.2–0.5s，且当前在执行线程同步完成；新增线程的调度和 publish 锁成本没有回收面 |
| 优化 SMC mprotect 作为 c-ray 主项目 | 实测 close 总成本 29.8–69.0ms，strace mprotect 2.9ms；数量级不足 |
| `SVM_DIRECT_BLOCK_LINK=1` | 已知不具备 SMC incoming-link 安全，且禁用 disk cache；不是生产候选 |
| 全量 XMM static 或 partial/lazy pin 再搜索 | W13/W17 已有 -26%、-0.9% 至 -9.0% 和重翻译环路负证据；本轮没有新的边界解法 |
| flags 作为第一优先级 | 三块差额仅 37/702=5.3%；加权全删上限约 1.7%，且现有 lazy flags 已吃掉易得部分 |
| 仅靠 dispatcher 微调追 3.45x | 主渲染 dispatch/exits 约 6.41%，三块差额中 dispatch 7.3%；link miss 只有 323/12.28 亿 exits，查找失败不是主因 |

## 7. 建议的后续阶段与验证门

### Phase A：补齐真实性基线，不改代码

- 条件：macOS `pgrep` 无其他 build/test/长期 CPU worker，连续 5 分钟 load 稳定；或在独占 Orb CPU set/VM 上做严格同核配额。
- 运行：当前 `5a19dcc` SVM/FEX 正式参数，交错 5 对；记录 render、host wall/user/sys、频率/温度信息、oracle。
- 目的：得到当前真正 quiet 比值。旧 1.4x 不得再作为先验。
- 开关：SVM 使用现默认；FEX 保持 harness 的 `MULTIBLOCK=1 ABILOCALFLAGS=1`。

### Phase B：P1 动态归因 spike，默认 OFF

- 开关：`SVM_RA_HOT_COALESCE=1`，另有只读计数模式；若需要生产源码探针，应在独立实验分支，最终实现前移除不必要热计数。
- 收益门：c-ray 至少 7 对 A/B，95% CI 下界为正；host retired instructions 与 render wall 同向；oracle 全同。
- 边界门：全量 RA shape 不得增加 spill units/high-water、scratch escalation、helper snapshot；host bytes 增长不超过实测删除量所能解释的范围。
- 回归门：mac/orb `swift_test`、func_tests 六格、fingerprint A/B、全 benchmark matrix。

### Phase C：P2 unit-local XMM，必须依赖 Phase B 的 pressure 数据

- 开关：`SVM_XMM_BLOCK_RESIDENCY=1`，默认 OFF；不能与 `SVM_XMM_STATIC` 混为同一实验。
- 每 unit 自动回退条件：新增 FPR spill、超过 scratch budget、遇到无法证明的 helper/fault/barrier edge。
- 收益门：不仅比较 c-ray，还必须覆盖 smallpt、STREAM、AES/GHASH、NaN pressure 与 helper fault；任何 W13/W17 旧失败形态复现即停止。

### Phase D：region JIT 仅保留为设计研究

在写代码前必须先给出：region entry/exit 的 GPR/XMM/flags flush 指令数、SMC target-to-source invalidation、fault table 粒度、disk-cache relocation 格式、最大 code growth 和 compile latency。FEX 的 7.33% 只说明 multiblock 在 FEX 架构内有价值，不能替 SVM 支付这些边界成本。

## 8. 数据不足项

以下结论本轮明确不能给：

1. **current-master 真安静 wall。** 主机没有合格空闲窗口；旧 19s 已证伪。
2. **安静态动态 cycle 分类。** 本轮是受载样本的热 PC + 静态 host 指令分类；代码形态有效，但调度和微架构 cycle 不能冒充 quiet。
3. **三个热块之外的全程序动态分类。** 三块覆盖 27.16% mapped samples，足以发现主形态和 hotspot spill，但不是全覆盖。
4. **P1/P2 的正向净收益下界。** 当前只有机械上限，没有候选实现的新增边界成本；因此只批准计数/设计 spike。
5. **FEX 无 NaN guard 是否与 SVM 精确语义等价。** 本轮只比较常规 c-ray oracle，未做跨引擎 NaN payload/exception edge 对照。
6. **region JIT 在 SVM 的收益。** FEX 7.33% A/B 不能移植；SVM 的 state/SMC/cache 边界净账尚未建立。

## 9. 最终裁定

- “SVM 的墙钟对负载敏感度约为 FEX 的 3 倍”：**证伪，根因是旧分钟解析 artifact；可控过载下两者膨胀同量级。**
- “受载触发翻译竞争、SMC syscall、JIT cache 抖动或 link 行为变化”：**均证伪为主机制。**
- 当前 c-ray 对 FEX 的大差距：**确认主要来自执行形态；三个精确热块的差额 90.3% 是 state/move/NaN/flags/dispatch bookkeeping。**
- 最值得继续的点：**不是全局驻留重写，而是先解释并削减 `0x402fed` 这一动态热点里的 spill+copy。** 该方向只批准带边界计数的 spike；在拿到净 A/B 前不承诺正收益。
- 本轮仓内唯一应新增的内容是本报告；没有生产源码、harness、golden 或测试基线改动。
