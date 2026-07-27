# SwiftVM 性能基线与剖析（2026-07-26）

本文最初**只建立基线与剖析、产出优化候选清单**，未做任何优化实现。§5 的「兑现
进度」表记录此后哪些候选已落地及其实测收益——**基线数字保持写就时的原样，不随
优化更新**，否则就没有基线可言。全部数字来自实测，未估算、未外推；测不到的地方
明确写「测不到」。

被测提交：`2bbf1f1`。测量在**独立 detached worktree** 中构建，避免并行开发改动污染。
构建类型：**`CMAKE_BUILD_TYPE=Release`**（Apple clang 21，`-DFMT_CONSTEVAL=`）。
宿主：Darwin 27 / Apple M4 Max（10 P + 4 E 核），AC 供电。

---

## 0. 怎么重跑

```sh
# 1. 重建 guest 与黄金校验值（黄金值来自真实 x86-64 执行，Rosetta 下）
bash source/translator/linux/tests/build_bench_tests.sh

# 2. 基线
python3 source/translator/linux/tests/bench_run.py \
    --svm <build>/source/translator/linux/svm_translator_linux --reps 15 \
    --out source/translator/linux/tests/bench_suite_x86_64.baseline.txt

# 3. 开关对比矩阵（同一次调用内交错，才可横向比较）
python3 source/translator/linux/tests/bench_run.py --svm <build>/... --reps 9 \
    --only int,fp,mem,branch,call,func_tests \
    --config "default=" --config "no_uniform_elim=SVM_UNIFORM_ELIM=0" \
    --config "no_static_regs=SVM_STATIC_REGS=0" --config "no_func_base=SVM_FUNC_BASE=0" \
    --out source/translator/linux/tests/bench_config_matrix.txt
```

已检入的数据文件（仿照 `.native.txt` 黄金值惯例）：

| 文件 | 内容 |
|---|---|
| `tests/bench_suite_x86_64.native.txt` | 五个 kernel 的**黄金校验和**，由真实 x86-64 执行同一份 `bench_suite_kernels.h` 得到 |
| `tests/bench_suite_x86_64.baseline.txt` | 三次 15-rep 墙钟基线（不同宿主负载） |
| `tests/bench_config_matrix.txt` | 四种开关配置的交错对比 |
| `tests/bench_expansion_counters.txt` | **确定性**计数（host/guest 指令膨胀比、翻译分阶段耗时） |

### 工作负载

`bench_suite_x86_64` 是 freestanding guest（自带 `_start`、裸 syscall、无 libc），
与 `avx_real_x86_64` 同一套交叉编译recipe（`clang -target x86_64-unknown-linux-gnu -c`
+ `mklinuxelf.py`，本机无 ELF linker）。freestanding 是**刻意的**：静态 glibc guest
在几百毫秒的运行里大部分时间在编译 libc 启动代码，那测的是「翻译 glibc 有多贵」而
不是「被测 kernel 跑得多快」。

| kernel | 压什么 | 迭代数（scale=1） |
|---|---|---|
| `int` | 64 位整数 ALU + 长依赖链，循环体零访存 | 20 M |
| `fp` | SSE2 packed double/single 算术，工作集常驻寄存器 | 4 M |
| `mem` | 4 MiB 数组：顺序写 + 跨步 RMW + 依赖型指针追逐 | 300 |
| `branch` | 数据相关的不可预测分支，6 种不同 flag 形状 | 12 M |
| `call` | 4 层不可内联 guest 调用链（call/ret、RSB、函数级编译） | 16 M |
| `func_tests_x86_64` | 真实静态 glibc 程序（**翻译主导**，见 §3） | — |
| `x87_bench_x86_64` | x87 吞吐（默认 SoftFloat helper 路径） | — |

迭代数是**实测标定**的，不是猜的：scale=1 时每个 kernel 约 0.40–0.51 s，约为翻译器
启动地板（~15 ms）的 30 倍。

### 噪声控制（本机是共享工作站，测量期间宿主负载 6–16）

macOS 没有 governor、没有 cpuset、没有 taskset，**无法钉住频率**；P/E 核切换会让被抢占
后重新调度的进程落到 E 核上。可用手段与已采用的做法：

1. **中位数 + MAD + min**，不用均值：一次协同调度的编译会把 0.40 s 拉到 0.9 s，均值会被
   带偏，中位数不会。同时报 `min`——在有争抢的机器上 min 最接近无干扰采样。
2. **交错执行**：按 (workload, config) 轮次交错，而不是把一种配置连着跑完。热漂移与
   后台负载是分钟级变化的，交错把这种变化摊到所有配置上。
3. **每 rep 记录 1 分钟 loadavg**，并把区间写进数据文件。**loadavg 区间不重叠的两次运行
   不可比较**。
4. 丢弃每组第一 rep 作为 warm-up。
5. 同时记录子进程 user+sys CPU 时间；wall 与 cpu 明显背离本身就是干扰的证据。
6. **每 rep 校验 guest 校验和与退出码**。算错的运行不是快，崩溃的运行更不是快
   （见 §5 候选 #2：`SVM_FUNC_BASE=0` 下 x87 bench 直接 abort，没有退出码检查时它会被
   报成 11 倍加速）。

实测效果：**单次运行内相对 MAD 为 0.1–2.9%**；**跨运行随负载变化最大 15%**。
所以结论一律建立在「同一次交错调用内的相对比较」上，绝对值只作参考。

另外：**确定性指标不受噪声影响**——host/guest 指令膨胀比、helper 调用点数、编译单元数
全部是宿主负载无关的整数，是本报告里可信度最高的一类数据。

---

## 1. 基线（Release，宿主 loadavg 6.40–7.28 那一次）

| workload | median (s) | min (s) | rel MAD |
|---|---|---|---|
| int | 0.4176 | 0.4126 | 0.85% |
| fp | 0.3920 | 0.3893 | 0.09% |
| mem | 0.5127 | 0.5077 | 0.78% |
| branch | 0.4183 | 0.4159 | 0.21% |
| call | 0.4074 | 0.3906 | 1.00% |
| func_tests（真实 glibc） | 0.0307 | 0.0303 | 0.43% |
| x87_bench | 0.2420 | 0.2385 | 0.84% |

---

## 2. 剖析手段

### 2a. 临时插桩 `SVM_PROF`（**只在独立 worktree**，未进主工作区）

`source/runtime/common/perf_stats.h` + 5 处调用点。设计约束：**任何计时都不放在
「每执行一个 guest block 一次」的路径上**——所有被计时的函数都是「每编译一个单元一次」，
所以计时开销不会扰动被测对象。执行时间用「墙钟 − 翻译时间」反推，不给热循环加钟。

计数：`translate_ns` 及 decode/opt/regalloc/codegen 四个子阶段、编译单元数、
guest 指令数（数 `AdvancePC` IR op）、IR 指令数、**发射的 host 字节数**、
`EmitHostCall` 发射点数与其占用字节、regalloc spill 次数。`SVM_PROF=2` 额外每编译单元
打一行，给出**逐 guest 函数**的膨胀比。

**插桩开销实测**：未设 `SVM_PROF` 时，插桩构建与纯净构建的中位数差 −1.1% ~ +3.1%，
多数在 MAD 之内。因此**墙钟基线一律用纯净构建**，插桩构建只取计数。

### 2b. `sample`（macOS）

关键性质：JIT 生成的 host 代码在匿名内存里、没有符号，`sample` 报成
`??? (in <unknown binary>)`。这恰好是一个**免费的判别器**：栈顶落在 `???` = guest 代码执行，
落在符号上 = 翻译器 C++ 或 host helper。

### 2c. 两点回归（不插桩即可分离翻译/执行）

同一 kernel 跑 scale=S 与 2S。循环体是直线代码，scale 只改迭代数、不改被翻译的代码量，
所以 **斜率 = 纯执行，截距 = 启动 + 翻译**。

---

## 3. 时间花在哪（数据）

### 3a. 两点回归（7 次取中位数，同一时段）

| kernel | 模式 | t(S=1) | t(S=2) | 斜率=执行 | 截距=启动+翻译 |
|---|---|---|---|---|---|
| int | func | 0.3966 | 0.8041 | **0.4075** | −0.011 |
| int | block | 0.2637 | 0.5210 | **0.2573** | 0.006 |
| fp | func | 0.3918 | 0.7663 | 0.3745 | 0.017 |
| mem | func | 0.5083 | 1.0136 | 0.5053 | 0.003 |
| branch | func | 0.4191 | 0.8281 | **0.4090** | 0.010 |
| branch | block | 0.3425 | 0.6360 | **0.2935** | 0.049 |
| call | func | 0.3932 | 0.7843 | **0.3912** | 0.002 |
| call | block | 0.4338 | 0.8624 | **0.4286** | 0.005 |

结论：**微基准里翻译只占 0.5–12% 的截距，执行占绝对主导**；而 func/block 的差异全部落在
**斜率**上——即差在**生成代码的质量**，不是差在编译开销。

### 3b. 真实程序：翻译才是主导

| 程序 | 墙钟 | `translate_ns` | 翻译占比 |
|---|---|---|---|
| `func_tests_x86_64`（静态 glibc） | 30.7 ms | **18.1 ms** | **59%** |
| `real_busy_x86_64`（静态 glibc） | ~30 ms | **17.9 ms** | ~60% |

翻译子阶段（func_tests，7017 条 guest 指令、937 个编译单元）：

| 阶段 | ns | 占 translate_ns |
|---|---|---|
| decode（x86 → IR） | 5,255,000 | 29% |
| opt pass pipeline | 1,510,000 | 8% |
| **regalloc** | **3,501,000** | **19%** |
| codegen（ARM64 发射） | 3,113,000 | 17% |
| 其余（IR 构造/发布/加锁/`getenv`…） | 4,714,000 | 26% |

**翻译吞吐 ≈ 0.39 M guest 指令/秒**（7017 / 18.1 ms），即**每条 guest 指令约 2.6 µs**。
`real_busy` 独立复现同一数量级（7238 / 17.9 ms = 0.40 M/s）。

### 3c. dispatcher 往返与 helper 调用

- `jit_entries`（返回 C++ 的次数）：微基准 7–45 次，func_tests 939 次。**不是热点**。
  注意在生成代码内部还有一个 dispatcher 循环（`trampolines.cpp:202` 的
  `Cbz w0, &code_dispatcher`），C++ 侧数不到它——本轮**未测**这一层的往返频率。
- `sample` 栈顶分布：`int`/`fp`/`mem` 的 C++ 符号占比为 **0**，全部在 JIT 代码里。
  唯一出现的 host helper 符号是 **`swift::x86::MulHiS64`**（int 21/2396 = 0.9%，
  call 30/2364 = 1.3%）。见候选 #1。
- x87 默认路径：`SVM_X87_JIT_STATS=1` 实测 **16,000,006 次 helper 调用 / 0.24 s**，
  与 `docs/project-status.md` 记载一致。

---

## 4. 生成代码质量（确定性数据，与负载无关）

### 4a. 膨胀比

| workload | ir/guest | **host/guest** | host/ir |
|---|---|---|---|
| int（func） | 9.53 | **20.09** | 2.11 |
| int（block） | 6.81 | **13.78** | 2.02 |
| fp（func） | 9.24 | 20.08 | 2.17 |
| mem（func） | 8.63 | 16.47 | 1.91 |
| branch（func） | 9.07 | 18.34 | 2.02 |
| call（func） | 8.71 | 18.32 | 2.10 |
| func_tests（真实 glibc） | 7.69 | **13.68** | 1.78 |
| x87_bench（默认） | 6.37 | **85.3** | 13.4 |
| x87_bench（`SVM_X87_JIT=1`） | 5.56 | **151.7** | 27.3 |

逐 guest 函数（函数模式，`SVM_PROF=2`）：`kernel_int` 67 条 guest 指令 → **1059 条 host
指令（15.8×）**；`kernel_branch` 85 → 1078（12.7×）；`kernel_fp` 431 → 8456（19.6×）；
`kernel_mem` 133 → 1299（9.8×）。

**每条 guest 指令 13–20 条 host 指令**是本项目最核心的效率指标，也是目前最大的优化空间。
拆开看是两层：`ir/guest ≈ 7–9.5`（前端把一条 x86 展开成太多 IR）× `host/ir ≈ 1.8–2.2`
（后端每条 IR 落成约 2 条 ARM64）。**两层都有余量，但前端那层的系数更大。**

### 4b. helper 调用的静态成本

`EmitHostCall` 每个发射点保存/恢复 10 对 GPR + 全部 32 个 Q 寄存器（`kSaveBytes = 672`），
实测每个发射点 **376–378 字节 ≈ 94 条 host 指令**（`host_call_bytes / host_call_sites`：
int 6800/18、fp 6048/16、branch 5296/14、call 4544/12、func_tests 19616/52 全部落在 377–379）。

helper 序言/尾声占全部发射代码的比例：int 16.7%、fp 9.0%、branch 17.4%、call 20.1%、
func_tests 5.1%、**x87_bench 90.8%**。

### 4c. spill

全部被测 workload 的 `spill_gpr = spill_fpr = 0`。**寄存器 spill 不是当前热点**
（与 `project-status.md` §5 的记载一致：池 14 下重负载用例 spill 仍为 0）。

---

## 5. 优化候选清单

排序依据 (预期收益 × 置信度) / 实现成本。每项都标注数据来源与正确性风险。

### 兑现进度（本文写就之后）

| 候选 | 提交 | 实测收益 | 备注 |
|---|---|---|---|
| #1 `imul` 高半走 helper | `e01cbe4` | **mul/add 比值 3.27 → 1.000** | 新增 `MulHigh` IR，落 `SMULH`/`UMULH`。乘法循环现与加法循环等速 |
| #3 `EmitHostCall` 全量保存 | `64a48d9` | **x87_bench 1.52×** | 保存集改为 RegAlloc 逐指令活跃集；其余负载持平 |
| #2 函数模式净亏损 | `d42bb4f` | **int −41%、branch −21%** | 根因是接线漏洞：`PassPipeline::RunFunction` 只跑注册为 function pass 的条目，而只有 UniformElimination 注册了——函数模式**从来没跑过 flag 消除与死代码消除**。两个 pass 的 `Run(HIRFunction*)` 重载早就写好，只是没被调用 |
| SSE NaN 修正改用 NEON | 未提交 | **`fp` 6.49×**（0.4094 → 0.0631 s） | `EmitVecFloatNaNFixup` 原本逐 lane 用 8 个 GPR 做位手术（f32 packed 约 160 条 host 指令）；改为 `Fcmeq`/`Bif`/`Bit` 一次算全部 lane，9–12 条、3 个 V 临时、0 个 GPR。发射字节：`vec_float_nan_pressure` −76.5%、`avx_real`（`SVM_AVX=1`）−44.8%、`fp` kernel −25.4% |
| 直接 jmp/call 走 L2 分派槽 | 未提交 | **`call` −5.0%**（0.1908 → 0.1813 s） | 生成代码内 dispatcher 往返：`call` kernel **64 000 009 → 11**。代价是每个直接跳转出口多 3 条指令（`call` 发射字节 +1.8%） |
| #2 后续：函数编译惰性化（`SVM_FUNC_LAZY`，默认 1 块/单元） | 未提交 | **`func_tests` 墙钟 −10.1%**（0.0386 → 0.0347 s，同次交错、rel MAD ≤1.5%）；**发射 host 字节 −30.3%**（344 000 → 239 920）；**编译的 guest 块 −29.7%**（1631 → 1147）；`call`/`int`/`branch`/`mem`/`fp` 持平 | 见下方「#2 后续」 |
| §6b RSB 命中率 1.81% —— 根因已定位并修复 | 未提交 | **`func_tests` 命中率 1.81% → 94.70%**、`real_busy` 2.52% → 92.35%、`real_hello` 2.73% → 93.50%、`real_busy_musl` 46.28% → 62.81%；`bench call` 100.00% 不变。**墙钟测不出**（`call` +0.2%、`func_tests` +2.5%，均在 MAD 内） | 见下方「§6b 续：RSB 根因」。改动一条指令：pop 未命中时 `rsb_ptr += 16` |
| §3.4 `CheckCond` 的 `LoadImm/LoadImm/CondSelect` → `CondSet` | 未提交 | **全语料 IR −2.08%**（146 830 → 143 783）、**发射 host 字节 −1.65%**（739 496 → 727 308）；`LoadImm` 条数 **−20.0%**（15 199 → 12 152）。**墙钟测不出**（8 个 workload 全在 MAD 内，宿主 loadavg 41–49） | 新增 `CondSet(Cond) -> 0/1` IR，ARM64 落 `CSET`、解释器落 `EvalCondition`。26 个程序的确定性计数，见下方「§3.4 续」 |
| §6e `AdvancePC` 合并（「惰性 NZCV」里唯一有数据支持的那部分） | 未提交 | **全语料 IR −7.09%**（132 952 → 123 528）；发射 host 字节 −0.02%（4 400 个单元里 32 个变小、**0 个变大**、单元集合逐个一致）。**`translate_ns` 只 −1.3~−1.5%** | 在 `HIRBuilder::AppendInst` 里把不可能做任何事的 `AdvancePC` 丢掉，立即数**向后**折进本块上一条保留的 `AdvancePC`。见 §6e |
| §6f `ir::Inst` 每条走一次 `malloc` | 未提交 | 函数模式 **`decode_ns` −7.6~−9.2%**、`translate_ns` −1.6~−2.1%；**块模式 `translate_ns` −4.9~−5.4%**。max RSS 不升 | `Inst` 的 slab 只有 `swift_test` 初始化过，产品路径一直退化成 malloc/free。改成线程局部 bump arena + free list。见 §6f |

两项都用「同一干净基座产出的两个二进制、同一次调用内交错、中位数」测得，
所以结论不受宿主负载影响。#3 另有 25 个 guest e2e 程序的**退出码逐行比对**。

一条教训值得记住：#3 第一次测量是在主工作树上做的，而树里有其他并行改动
（`signal_handler.cpp` 等），结果把别人的 SIGBUS 算到了这个改动头上。**性能
与行为比对必须在独立 worktree 上取基线。**

#2 顺带暴露一个真实正确性缺陷：`Inst::HasSideEffects` 没把
`CallLambda`/`CallLocation`/`CallDynamic` 算进去。它们返回 U64 但常常没人用
返回值，DCE 会删掉——而它们是通过 state 指针改 guest 状态的 helper（FNINIT、
SoftFloat x87、xsave）。**这个缺陷在 DCE 没被接进函数模式时是不可见的**，接上
的那一刻立刻显形（九次 FLD1 后 TOP=0 而非 7）。一个没被调用的 pass 会掩盖别处
的错误假设。

#2 结论：**函数模式应继续默认开**。回归已消失，翻转默认值会丢掉 `call` 的
10.5%。仍落后的 `func_tests` 是**翻译成本**不是代码质量——`SVM_FUNC_STATS=1`
显示 compiled=148 但 compiled_blocks=847（5.72 块/函数），把静态可达的块都急切
编译了，而短命程序大部分从不执行。下一步是热度启发式或惰性块编译。

#### #2 后续：函数编译惰性化（基座 `1e2573e`，独立源码快照，2026-07-27）

**先量后改，量出来的结论推翻了预设。** 三个实测事实：

1. **函数模式没有跨块的寄存器状态。** 在 `TranslateIR(HIRFunction*)` 里数「操作数的
   定义指令是否在别的块」：`func_tests` 147 单元 / 822 块 / **17 431 个操作数，跨块 0**；
   `real_busy`/`hello`/`loop`/`basic_coverage_smoke`/`smc`/`x87_bench`/`random_smoke`
   同样为 0。根因：uniform 消除与 flag 消除都是**块内**的（`UniformEliminationPass::Run`
   的 `uniform_values` 每块清零），且每块结尾都有 `FlushFlags`。
   所以**每个已解码块本来就是自洽的入口**——`TranslateIR` 一直在把它们全部
   `PushCodeCache` 发布为合法入口，正是依赖这条性质。任务书担心的「函数级 RegAlloc
   如何保持一致」**不存在**。
2. **急切编译的块里 56% 从不执行。** 对比同一 guest 在函数模式与块模式下编译的块地址
   集合（块模式只编译执行到的块）：`func_tests` 函数模式 1578 个不同块 vs 块模式 1147，
   块模式集合是函数模式的**真子集**；单看函数单元内部的块，769 个里 431 个
   （**56.0%**）从未执行。`real_busy` 55.9%、`func_tests_musl` 26.5%。
3. **更大的编译单元买不到任何东西。** `DirectBlockLink` 关、模块非 read-only，所以
   `JitContext::Forward` 对**函数内部**的边也走 L2 分派槽——与两个独立单元之间的链接
   完全一样。实测把每单元块数从 1 扫到 64，`int`/`fp`/`mem`/`branch`/**`call`** 的墙钟
   全在 MAD 之内，而编译的块数与发射字节单调上升（N=1 的 1147 块 / 239 920 字节 →
   N=64 的 2285 块 / 453 400 字节）。

结论：**函数级编译的价值全部在流水线（function passes + 函数级 RegAlloc），不在单元
大小。** 证据：N=1（每单元一个块，与块模式同样的粒度与链接方式）在 `call` 上仍比块模式
快 **12.2%**（0.1877 vs 0.2106 s），同样 1147 个块只发射 239 920 字节而块模式 252 772
（−5.1%）。**因此不需要「热度启发式把函数升级为大单元」——没有可升级的收益。**

做法：`SVM_FUNC_LAZY=N`（默认 **1**）限制每次函数编译解码的块数；未解码的后继留成空块，
发射时跳过、地址不发布，边通过 L2 槽的空槽分支写 `current_loc` 并回分派器，由分派器按需
编译新区域。`SVM_FUNC_LAZY=0` 恢复急切整函数解码。

**仍未拿到的那一半**：块模式的 `translate_ns` 是 11.0 ms，惰性函数模式 16.6 ms，急切
20.6 ms（`func_tests`，同为 1147 个块）。差额全在**每单元的流水线固定成本**，已分相测出
（ns，1147 块）：

| 阶段 | 函数流水线 | 块流水线 | 差 |
|---|---|---|---|
| decode（含 HIRValue 记账） | 3 910 000 | 1 720 000 | **+2 190 000** |
| ComputeRPO + IdByRPO（每单元两次） | 1 450 000 | 0 | **+1 450 000** |
| opt passes | 2 960 000 | 2 710 000 | +250 000 |
| regalloc | 2 670 000 | 1 820 000 | +850 000 |
| codegen | 2 540 000 | 2 490 000 | +50 000 |
| publish | 670 000 | 1 430 000 | −760 000 |
| 容器开销（`HIRPools` 构造/析构等） | 2 760 000 | 1 150 000 | +1 610 000 |

**一个已试过且失败的候选**：`IdByRPO` 只对 id 真的变了的值做 rbtree erase/insert——
实测 RPO 阶段 1.45 → 1.59 ms（噪声内，无收益），说明绝大多数值的 id 确实都会变。**未采用。**

**惰性化踩到的真实缺陷（已修）**：`SVM_ENABLE_JIT=0` 的 IR 解释器用
`ir::Function::FindBlock` 找下一个位置，而 `HIRFunction::EndFunction` 把**所有** HIR 块
（含未解码的空块）都交给 `ir::Function`；部分编译的单元于是让解释器拿到一个空块、
什么都不执行、**死循环**。`run_helper_fault_tests.sh` 的 `SVM_ENABLE_JIT=0` 形状挂死抓到
了它。当前修法是**解释器路径保持急切**（那条路是交叉验证用的，翻译成本无所谓）；
更彻底的修法——根本不把未解码块放进 `ir::Function`，顺带也能缩小
`SmcTracker::ClearDispatchSlots` 的遍历——**没做**。

---

### #1 `imul r64,r64` 走 host helper 求高 64 位 —— 应内联为 `SMULH`/`UMULH`

**数据（专门做的隔离实验，`imul_probe`，两条循环 guest 指令数完全相同）**

| 循环 | guest 指令 | host 指令 | host/guest | 20 M 次墙钟 | host_call_sites |
|---|---|---|---|---|---|
| `a*=K; b*=K`（2×`imul r64,r64`） | 37 | **1286** | 34.8 | **0.470 s** | 16 |
| `a+=K; b+=K` | 37 | **466** | 12.6 | **0.144 s** | 8 |
| `a^=a<<11` 等 | 51 | 677 | 13.3 | 0.200 s | 8 |

同样 37 条 guest 指令，乘法版**多发射 820 条 host 指令、慢 3.27 倍**。
差额 8 个 helper 发射点，`host_call_bytes` 差额 6048−3040 = 3008 字节 = 8 × 376，逐字节吻合。
折算 **每条 `imul` 约 8.2 ns（M4 上约 30+ 周期）**，而 ARM64 原生 `mul` 是 1 条指令。
`sample` 独立佐证：唯一出现的 host helper 符号就是 `MulHiS64`。

**发射点**：`source/runtime/frontend/x86/decoder_alu.cc:109-110`
（`assembler->CallHost(&MulHiS64/&MulHiU64, a, b)`）。

**做法**：新增 `MulHi` 类 IR op（有符号/无符号各一），后端落 `SMULH`/`UMULH` 单指令。
x86 两操作数 `imul` 只写低 64 位，OF/CF 由「完整 128 位结果是否是低 64 位的符号扩展」决定，
ARM64 用 `mul` + `smulh` + `cmp` 三条即可，无需 helper。

**影响面**：所有整数密集 guest（哈希、PRNG、大整数、编译器生成的除法-乘法优化到处是
`imul`）。**收益大、置信度高、成本中（需要新增 IR opcode + 后端 emitter）**。

**正确性风险：中。** `imul`/`mul`/`imul r/m` 三种形式的 OF/CF 语义各不相同，且 32 位形式要
写高半 EDX。必须走 Unicorn 差分 fuzz 的整数族全覆盖，并且**边界必须带负数与 `INT64_MIN`**
（`project-status.md` 已有教训：只用正数会让符号相关的判定式一起变绿）。

---

### #2 函数级编译（`SVM_FUNC_BASE=1`，默认开）在整数/分支类负载上是**净亏损**

**数据（一次交错运行内，全部校验和与退出码通过）**

| workload | default（函数模式） | `SVM_FUNC_BASE=0`（块模式） | 差 |
|---|---|---|---|
| int | 0.4176 | **0.2670** | **块模式快 36%** |
| branch | 0.4183 | **0.3299** | **块模式快 21%** |
| func_tests（真实 glibc） | 0.0307 | **0.0223** | **块模式快 27%** |
| mem | 0.5127 | 0.5035 | 块模式快 1.8% |
| fp | 0.3920 | 0.3877 | 块模式快 1.1% |
| call | 0.4074 | **0.4426** | **函数模式快 8%** |

两点回归证明差异在**斜率（执行）**而非截距（翻译）：int 执行斜率 0.4075 → 0.2573。
确定性佐证：同一份 `kernel_int`，函数模式 `ir/guest = 9.53`、`host/guest = 20.09`；
块模式 `6.81` / `13.78`——**函数模式每条 guest 指令多生成 40% 的 IR**。

**根因未定位**（本轮只到「多生成 IR」这一层，缺 host 代码反汇编工具）。
下一步应先做一个 host 代码 dump（`JitContext::Flush` 处 dump 到文件 + vixl disassembler），
对比同一个 `kernel_int` 循环体在两种模式下的实际指令序列。

**做法**：先解释清楚，再考虑（a）修函数模式的 IR 膨胀，或（b）改默认值 / 加启发式
（例如「单块自循环走块模式，多块含调用走函数模式」——`call` 是唯一函数模式更快的负载，
正好符合这条启发式）。

**收益大、置信度高（数据很硬）、成本高（根因未知）**。

**正确性风险：低（改默认值本身）/ 高（改函数模式代码生成）。** 但注意一个**已存在的缺陷**：
`SVM_FUNC_BASE=0` 跑 `x87_bench_x86_64` 会 abort：
`Check Failed! No free temporary GPR`（先打印 `RegisterAllocPass: 2 value(s) spilled`）。
即**块模式在 x87 路径上今天就是坏的**——这与 `project-status.md` 记载的
「`GetTmpX`/`GetTmpV` 无释放机制、GPR 池约 14 个」是同一个问题。
在把块模式变成默认之前必须先修它。

---

### #3 x87 默认路径：90.8% 的发射代码是 helper 序言/尾声

**数据**：`x87_bench` 默认路径 27 条 guest 指令 → 2302 条 host 指令（**85.3×**），
其中 `host_call_bytes / host_bytes = 8360 / 9208 = 90.8%`。
运行期 `SVM_X87_JIT_STATS=1` 实测 **16,000,006 次 helper 调用 / 0.24 s**。

**做法**：这是 `docs/project-status.md` 已知项（`SVM_X87_JIT=1` 把调用降到 2 M / 0.45 s），
本轮的新数据是**成本结构**：贵的不是 SoftFloat 本身，而是**每次调用 672 字节的
寄存器保存/恢复**。因此有一条独立于「继续内联更多 x87 op」的正交优化：
**按被调用 helper 的实际 clobber 集裁剪保存集**（`EmitHostCall`，
`translator_control.cpp:62`，当前无条件保存全部 32 个 Q 寄存器）。
guest 上下文里活跃的 SIMD 值通常远少于 32 个，RegAlloc 已经知道哪些 V 寄存器活着。

**收益大（对 x87 与所有走 helper 的 SSE/AVX 路径同时生效）、置信度高、成本中。**

**正确性风险：中高。** 少保存一个实际被 clobber 的寄存器 = 静默数据损坏，且只在特定
寄存器压力下暴露。必须由 RegAlloc 的活跃集驱动（而非人工枚举），并且要有「故意少保存
一个」的**变异测试**证明断言有牙。

---

### #4 SMC 写保护按**宿主页**粒度（macOS/arm64 = 16 KiB），代码旁的数据引发重译风暴

**数据（本轮意外发现，三档对照实测）**

| guest 数据相对代码的位置 | 编译单元数 | 重译的 guest 指令 | `translate_ns` | 该 workload 墙钟 |
|---|---|---|---|---|
| `.bss` 紧跟 `.text`（进同一 4 KiB 页） | **545** | 136,370 | **402 ms** | 452 ms |
| 对齐 4 KiB（仍在同一 16 KiB 宿主页内） | **457**（两个 PC 各编译 **222 次**） | 26,825 | 71 ms | 90 ms |
| 对齐 64 KiB（离开宿主页） | **8** | 586 | 2.1 ms | — |

即：同一份 guest、同一段代码，仅因数据与代码共享**宿主页**，就产生了 **68 倍**的编译量和
400 ms 的翻译开销。根因：`smc_tracker.cpp:32` 用 `getpagesize()`，macOS/arm64 返回 **16384**，
而 `mprotect` 只能按宿主页做——于是保护范围比真正的代码范围大得多。

**为什么这不只是我的 benchmark 的问题**：真实静态链接程序的 `.data`/`.bss` 就紧邻 `.text`，
glibc 的 GOT/IFUNC 解析结果、`errno`、malloc 元数据都可能落在代码尾页里。
**本轮未测**真实 glibc guest 上这个效应有多大（`func_tests` 的 `raced=0`、
`block_units=790` 表明它没有明显重译风暴，但没有专门做对照实验）——**这是下一步该测的第一件事**。

**做法**（按成本递增）：①对「宿主页内、但落在已知代码范围之外」的写做**字节级过滤**，
不触发失效；②保留一份代码字节的影子副本，faulting store 之后比对，真无改动就不失效；
③页内细粒度（子页位图）标记哪些区间真有翻译。

**收益：真实程序上未知但可能很大；置信度：机制已实测确凿，量级待测；成本：中。**

**正确性风险：高。** SMC 是正确性红线，放宽失效条件一旦漏判就是执行陈旧代码。
任何改动都必须过 `smc=99`、`clone_smc_mt`（512 次补丁）与 MT QSBR 回收路径。

---

### #5 前端 IR 膨胀：每条 guest 指令 7–9.5 条 IR

**数据**：`ir_per_guest` 在全部 workload 上 6.6–9.5（真实 glibc 7.69）。
`host_per_ir` 只有 1.78–2.27，说明**后端每条 IR 落 2 条 ARM64 是合理的**，
膨胀主要来自前端展开。x86 的 `add r,r` 语义上是 1 条 ALU + 5 个 flag 位，
把 flag 计算显式化就很容易到 6–8 条 IR。

**做法**：需要**逐 opcode 的 IR 计数分布**才能定位（本轮只有总量，未按 opcode 拆分）。
建议下一步在 `JitTranslator::Translate(ir::Inst*)`（`translator.cpp:186`）按 IR opcode
累计 `CurrentBufferSize()` 增量，得到「哪个 IR op 吃掉了多少 host 字节」；
同时在 decoder 侧按 x86 mnemonic 统计产出的 IR 条数。
`FlagElimination` 已经在跑（`no_uniform_elim` 那一列证明 pass 有效），但**未测**它删掉了
多少比例的 flag 计算。

**收益大、置信度中（方向确凿、具体着力点未知）、成本中（先要一轮测量）。**
**正确性风险：视具体改动而定；flag 语义是 x86 最容易错的部分。**

---

### #6 `UniformElimination` 是当前最有价值的 pass —— 关掉会慢 17–37%

**数据（同一次交错运行）**

| workload | default | `SVM_UNIFORM_ELIM=0` | 关掉的代价 |
|---|---|---|---|
| mem | 0.5127 | 0.7005 | **+37%** |
| branch | 0.4183 | 0.5111 | **+22%** |
| int | 0.4176 | 0.4877 | **+17%** |
| fp | 0.3920 | 0.4328 | +10% |
| call | 0.4074 | 0.4000 | −1.8%（噪声内） |
| func_tests | 0.0307 | 0.0291 | −5%（翻译主导，少跑一个 pass 更快） |

这不是候选，是**结论**：uniform（guest 上下文）访存消除是目前收益最大的已有优化，
任何改动都不得削弱它；同时说明**继续在这个方向投入（更激进的 uniform 提升/下沉）大概率有回报**。

---

### #7 静态寄存器映射（`SVM_STATIC_REGS`）当前**测不出收益**

**数据**：default vs `SVM_STATIC_REGS=0`，五个微基准差异 −1.1% ~ +1.1%，**全部在 MAD 之内**；
func_tests 完全持平（0.0307 / 0.0307）。

即 RSP→x19、RBX→x20、RBP→x21 这三个静态映射在这些 workload 上**没有可测量的收益**。
不建议移除（它可能对别的负载有用，且是 uniform 消除的前置），但**不应把它算作已兑现的优化**，
也说明「再多映射几个寄存器」的预期收益应当被下调。

**成本：零（这是测量结论，不是改动）。**

---

### #8 翻译吞吐 0.39 M guest 指令/秒 —— 对短命真实程序是主导成本

**数据**：真实 glibc 程序 59–60% 的时间在翻译；分阶段 decode 29% / regalloc 19% /
codegen 17% / opt 8% / **其余 26%**。

那个「其余 26%」（约 4.7 ms / 937 个单元 = 每单元 5 µs）值得先查：
`X86Instance::Impl::Translate` 里有**每次编译多达 6 次 `std::getenv`**
（`translator/x86/translator.cpp:420, 487, 549, 553, 563, 579, 628`；
`runtime/backend/runtime.cpp:440` 还有一次），Darwin 的 `getenv` 要遍历 `environ`。
`decoder_x87.cc:76`、`decoder_bmi.cc:248`、`xsave.h:65`（其注释自己承认「每次都 getenv
而不缓存」）在**解码路径**上也有。

**这是唯一一项符合「一行且零风险」的改动**（把 `getenv` 结果缓存进 function-local
`static const bool`，与 `perf_stats.h::Enabled()` 同一写法）。**本轮没有做**——因为这些文件
属于并行 agent 的领地（`source/runtime/frontend/x86/`、`source/runtime/backend/`）。
预期收益：**未测**，需要先量一次 `getenv` 在这条路径上的实际占比再决定。

**收益未知、置信度低（未测）、成本极低。建议先测再改。**

---

## 6. 本轮明确「测不到」的部分

- **生成代码内部的 dispatcher 往返频率**：`trampolines.cpp:202` 的
  `Cbz w0, &code_dispatcher` 让 JIT 在不返回 C++ 的情况下反复派发，C++ 侧计数器数不到。
  要测必须在发射的代码里插计数器。
- **候选 #2 的根因**：缺 host 代码反汇编 dump，只能观察到 IR 层的膨胀。
- **候选 #4 在真实 glibc guest 上的量级**：机制确凿，真实程序上的影响未做对照实验。
- **IR 泄漏（`Module`/`Block` 所有权）对性能的影响**：本轮 workload 均为短命进程，
  未观察到 RSS 增长导致的性能退化，**没有测**长时间运行的场景。
- **`vpsllv/vpsrlv/vpsrav` 的分解成本**：AVX 默认关闭且本轮 workload 不含 VEX 指令，
  这条线索**本轮完全未触及**。
- **多线程 guest 的性能**：`clone_lock_rmw_x86_64` 是仓库里第二长的负载（~1.3 s），
  未纳入基线（它测的是竞争而非吞吐，方差性质不同）。

### 6b. 后续补测（2026-07-27，基座 `6a4ba4f`，插桩只在独立 worktree）

**生成代码内 dispatcher 往返**——在 `code_dispatcher`、L1/L2 命中点与 `code_cache_miss`
各插一个计数器（用该点已死的 scratch 寄存器），得到：

| guest | dispatcher 迭代 | L1 命中 | L2 命中 | miss |
|---|---|---|---|---|
| `bench call` | **64 000 009** | 63 999 992 | 6 | 11 |
| `func_tests`（真实 glibc） | 14 108 | 13 049 | 132 | 927 |
| `real_busy` | 11 534 | 10 427 | 115 | 992 |
| `bench int/fp/mem/branch` | 4–5 | 0 | ≤1 | 4–5 |

结论：这一层**只在 call 密集代码上是热点**（每个直接 call 一次往返，L1 命中率
99.99998%），真实短命程序上 miss 数≈编译单元数（每个新单元一次），不是热点。

**RSB 命中率**（同一批插桩）：`bench call` **100.00%**（64 M pop / 64 M hit），
但 `func_tests` 只有 **1.81%**（3587 pop / 65 hit）、`real_busy` **2.52%**。
即 RSB 在微基准上完美、在真实 glibc 上几乎不起作用——**根因未查**（推测返回地址
落在调用者单元内部、不是编译单元入口，L2 槽因此永远是空的）。

**候选 #4（SMC 宿主页粒度）在真实 glibc guest 上的量级：无。** 计数
`RegisterNode`/`mprotect`/写保护故障/失效节点：

| guest | 保护页 | mprotect | **故障** | **失效节点** |
|---|---|---|---|---|
| `func_tests` | 21 | 21 | **0** | **0** |
| `real_busy` | 23 | 23 | **0** | **0** |
| `func_tests_musl` / `real_busy_musl` | 2 | 2 | **0** | **0** |
| `real_hello` | 20 | 20 | **0** | **0** |
| `clone_smc_mt`（真 SMC） | 515 | 1028 | 513 | 1537 |

真实静态 ELF 的 `.text` 与 `.data`/`.bss` 落在不同的 PT_LOAD 段、按宿主页对齐映射，
所以数据写从不落进含代码的 16 KiB 宿主页。基线里 545→8 编译单元那个现象来自
`mklinuxelf.py` 造的 freestanding guest（全部塞进一个 PT_LOAD）。
**候选 #4 应当降级**：真实语料上收益为零，而它是清单里正确性风险最高的一项。

### 6c. §6b 续：RSB 根因（2026-07-27，基座 `8d8ddba`，插桩只在独立 worktree）

§6b 的推测（「返回地址不是编译单元入口，L2 槽永远是空」）**只对了 1.5%**。把 pop 的
未命中拆成三类（下溢 / 地址不符 / L2 槽为空）后，HEAD 上的实测是：

| guest | pop | hit | 下溢 | **地址不符** | L2 槽空 | **push 因缓冲区满被跳过** |
|---|---|---|---|---|---|---|
| `func_tests` | 3587 | 65 (1.81%) | 0 | **3469** | 53 | **3463 / 3592** |
| `real_busy` | 2616 | 66 (2.52%) | 0 | **2497** | 53 | **2491 / 2621** |
| `real_hello` | 2383 | 65 (2.73%) | 0 | 2265 | 53 | 2259 / 2388 |
| `func_tests_musl` | 1211 | 1152 (95.13%) | 0 | 24 | 35 | 0 |

**真正的根因**：`EmitRSBPop` 的未命中路径 `Ret()` 回分派器时**不推进 `rsb_ptr`**，
而这条 `ret` 已经消耗掉了一个返回地址。于是第一次「L2 槽为空」（返回目标还没编译，
每个新区域必然发生一次）就把缓冲区永久失步：下一条 `ret` 与同一个陈旧帧比较、
必然不符；push 一直堆叠到 `rsb_bottom`，此后**每一次 push 都被溢出保护跳过**。
一次未匹配就够，整个进程再也恢复不了——`push_skipped_full` 占 96.4% 就是这个。
musl 语料命中率高，只是因为它恰好没在早期踩到那一次。

**修法**（`JitContext::EmitRSBPop`，一条 `Add`）：读到帧的两条未命中路径统一
`rsb_ptr += 16` 丢弃该帧；下溢路径没读到帧，不动。丢弃**语义上无条件安全**——RSB 只是
预测，体系结构上的返回目标始终在 `state->current_loc`，未命中就回分派器，错误预测
最多损失一次分派往返。修后 `miss_addr` 在**全部 guest 上归零**，`push_skipped_full`
归零，命中率见兑现表。

**变异测试**：
* 去掉 pop 的 L2 空槽保护（`Cbz(ip2, ...)`）→ 8 个 e2e guest 变成 139（SIGSEGV）。**被抓。**
* 去掉地址比较（`B(&rsb_miss, ne)`）→ swift_test、25 个 e2e、helper-fault 全绿。**没被抓**，
  而这**不是测试弱**：修好之后全语料 `miss_addr = 0`，即这批 guest 里根本不存在
  真正的 call/ret 失配，比较永远成立。要抓它需要一个带 longjmp / 栈切换的 guest，
  **语料里没有**。

### 3.4 续：`CondSet`（2026-07-27，同一基座）

`docs/ir-expansion-attribution.md` §3.4 指出 `CheckCond` 每次条件求值新建
`LoadImm(1)/LoadImm(0)/CondSelect`。新增 `INST(CondSet, BOOL, Cond)`（ir.inc 末尾），
ARM64 落一条 `CSET`，解释器落 `EvalCondition`。26 个程序的确定性计数
（按 IR opcode 累加 `CurrentBufferSize()` 增量，插桩只在独立 worktree）：

| | IR 条数 | 发射 host 字节 |
|---|---|---|
| `8d8ddba` | 146 830 | 739 496 |
| +CondSet | **143 783（−2.08%）** | **727 308（−1.65%）** |

逐 opcode：`CondSelect` 2767 → 0，`CondSet` 0 → 2767（字节相同，两者都是 1 条指令
加同样的 NZCV 重载），`LoadImm` **15 199 → 12 152（−20.0%）**、其字节 −18.1%。
墙钟与 `ir-expansion-attribution.md` 的结论一致：**测不出**。

**踩到的真实缺陷（自己的）**：`Inst::SetArg(const Cond&)` 不推断返回类型，而 `CondSet`
的唯一参数就是 `Cond`，所以 `ret_type` 停在 `VOID`。JIT 无事（RegAlloc 看的是 opcode
的 meta 返回类型），但 `Interpreter::WriteScalar` 对 VOID **直接 return，什么都不写**——
条件读到栈上的残留值。这正是「只做一个后端」那类分歧的镜像版本。
`run_helper_fault_tests.sh` 的 `SVM_ENABLE_JIT=0` 形状抓到了它（38 → 37）。
现在前端显式 `SetType(U8)`，两个后端都对 `VOID` 断言。

**变异测试**：

| 变异 | 结果 |
|---|---|
| `EmitCondSet` 条件取反 | swift_test 94 例中 **29 例失败**；几乎每个 e2e guest 退出码错。**被抓** |
| 只把解释器 `RunCondSet` 取反（JIT 保持正确） | swift_test **23 处断言失败**、helper-fault 38 → 37；e2e 退出码不变（走 JIT）。**被抓**，且证明解释器侧真的被覆盖 |
| `flags_elimination_pass` 里去掉 `case OpCode::CondSet` | `TEST_CASE("Fuzz x86 mixed sequences")` 的 Unicorn 差分出现 **8 处分歧**。**被抓**——那一行是承重的 |
| `CSET` 改 `CSETM`（true 变全 1 而非 1） | swift_test 四配置、25 个 e2e、AVX/isolation/malformed/helper-fault **全绿**。**没被抓** |

最后一条是个**已知覆盖缺口**，已写进 `ir.inc` 的注释：当前 x86 前端所有消费者都只判
非零（Jcc 判非零；JA/JBE 用 AND/OR 组合，全 1 照样对；`DecodeSetCC` 又用
`Select(cond,1,0)` 重新归一），所以「结果恰为 0/1」这条契约今天没有任何测试钉住它。

### 6d. x87 内联的成本结构（只测量，未改动）

`SVM_X87_JIT=1` 下 host/guest 仍是 151.7。按 IR opcode 拆开 `x87_bench` 的发射字节：

| 配置 | 总发射字节 | 主导 opcode | 条数 | 该 opcode 字节 | 占比 | 字节/条 |
|---|---|---|---|---|---|---|
| 默认（SoftFloat） | 3040 | `CallLambda` | 22 | 2376 | 78.2% | **108** |
| `SVM_X87_JIT=1` | 11 220 | `X87Op` | 22 | 10 692 | **95.3%** | **486** |

即：`64a48d9` 把 helper 发射点从 376 字节压到 **108 字节**之后，**默认的 SoftFloat 路径
在代码体积上已经比内联路径便宜 3.7 倍**（3040 vs 11 220），而内联路径换来的是约 1.9×
的执行速度。下一轮要动 x87，靶子是明确的：**每个 `X87Op` 486 字节 ≈ 121 条 ARM64 指令**
（`x87_midtier` 同量级：46 条 `X87Op` / 12 648 字节 = 275 字节/条）。本轮**没有改动**
x87：`X87Op` 是全语料唯一需要 8 个临时 GPR 的 opcode，且 `x87.cpp` 与 `translator_x87.cpp`
在最近三个提交里连续被改过，风险/收益比不如上面两项。

### 6e. 「惰性 NZCV」：先量，结果推翻了它的前提（2026-07-27，基座 `14c4b60`）

`docs/ir-expansion-attribution.md` §2 记 `AdvancePC` 占 **12.56% 的 IR / 7.66% 的 host
字节**，上一轮据此估计惰性化能省 **翻译侧 1.0–1.3 ms（−8~−11%）**。先在独立 worktree
里给 `EmitAdvancePC` 加三个计数器（25 个 e2e guest，函数模式默认档）：

| | 次数 | 占 `adv_seen` |
|---|---|---|
| `adv_seen`（到达后端的 `AdvancePC`） | 21 562 | 100% |
| 其中 NZCV 真的脏、要发 `Mrs/And/And/Orr` | **4 016** | **18.6%** |
| 其中有待落地的 `ClearFlags` | 2 543 | 11.8% |
| `merge_total`（后端全部 `MergeNZCV` 发射点） | 4 601 | — |

两个结论：**(a) 至少 69.6% 的 `AdvancePC` 一条 host 指令都不发**，那 7.66% 全部来自
剩下的 18.6%；**(b) 87% 的 merge 就发生在 `AdvancePC` 上**，而 ALU 发射器（`EmitAdd`
等）在写 NZCV 之前本来就先 `MergeNZCV()`。所以「把落地推迟到用时」只是把 merge 从
指令边界挪到消费点或块尾，**并不会减少 merge 的条数**——`flags` 是跨块存活的 guest
EFLAGS，块尾必须落地。惰性化能省的只有 `Condition` 终结符那两条 `LoadNZCVFromFlags`。

因此本轮只做**有数据支持的那一半**：把不可能做任何事的 `AdvancePC` 从 IR 里删掉。

**做法**（`HIRBuilder::AppendInst` / `FoldAdvancePC`，`SVM_ADVPC_COALESCE=0` 可关）：
自上一条**保留**的 `AdvancePC` 以来若没有出现 `SaveFlags`/`ClearFlags`/`SetCarry`/
`SetOverflow`，这条 `AdvancePC` 就丢弃，立即数**向后**折进上一条保留的那条。方向是
关键：向后折之后「已保留立即数之和 == 全部立即数之和」在每一步都成立，因此不需要任何
块结束钩子，**块的 guest 字节长度逐块不变**（`runtime.cpp` 的 SMC 范围就是这个和）。
向前累加能再多删约 2%（每块第一条），代价是需要 10 处块切换钩子且会动块长度——**没做**。

**结果（22 个 guest 的逐单元指纹，`SVM_PROF=2`）**：IR 132 952 → 123 528（−7.09%）；
host 字节 934 116 → 933 948（−0.02%）；**单元集合逐个一致，32 个单元变小、0 个变大**。
那 32 个单元的**机制未确证**。第一个假设——`EmitMemOperand` 的 post-index peephole
只往后看 3 条指令（`translator.cpp:398` 的 `search_times < 3`），`AdvancePC` 占掉窗口
里一格——**用实验否掉了**：把该 peephole 整个关掉重建两侧，同样这 32 个单元、同样的
差值。剩下最可能的是**寄存器分配**：活跃区间按指令位置算，少 7% 的指令会挪动区间端点，
进而改变 `EmitHostCall` 按活跃集裁剪出来的保存集（一对 `Stp/Ldp` = 8 字节）与
`context.R(v, true)` 能否省掉一条 `Mov`。差值恰好都是 4/8 字节，与这个解释相符。

**但翻译时间几乎没动**：同一次交错、11–13 reps 中位数，`func_tests` −0.33%、
`real_busy` −0.17%、`real_hello` −0.36%、`func_tests_musl` −0.21%（`opt_ns`/`regalloc_ns`/
`codegen_ns` 各降 0.2–2%，`decode_ns` 反升 0.4–1.4%，正负相抵）。
**这是本节最该记住的一条：`AdvancePC` 是 IR 里最便宜的一条指令**（VOID、无值、
在每个 pass 的 `switch` 里都落 `default`），它占 12.6% 的**条数**不等于占 12.6% 的
**成本**。上一轮 1.0–1.3 ms 的估计正是按条数占比外推的，实测差了一个数量级。
（叠加 §6f 之后合计 `translate_ns` −2.05~−3.89%；用 `SVM_ADVPC_COALESCE=0` 分离出
本节自己的份额是 −1.28 / −1.59 / −2.27 / +0.07 个百分点，四个 guest 相差很大，
说明它已经掉进噪声里了。）

**变异测试**：

| 变异 | 结果 |
|---|---|
| A1：`FoldAdvancePC` 忽略 `flags_since_advance`（连真正的落地点也丢） | 25 个 e2e 里 5 个退出码错（func_tests 101→134、func_tests_musl 101→1、real_busy 0→134、real_hello 42→134、x87_topvirt_stress 0→1）。**被抓** |
| A3：`LeavesPendingFlags` 不再算 `SaveFlags` | 4 个 e2e 退出码错。**被抓** |
| A2：向后折时**丢掉**立即数（块 guest 长度缩水） | 25 个 e2e、swift_test 四配置 94/94、`run_smc_stress_tests.sh` 200 轮**全绿**。**没被抓** |

A2 是一个真实的覆盖缺口，值得单独记：**仓库里没有任何测试钉住「一个块的
`AdvancePC` 立即数之和 == 该块的 guest 字节长度」**。它逃掉是因为 SMC 写保护是宿主页
（16 KiB）粒度、且相邻块首尾相接，块长度缩水几乎总能被下一个块的范围盖住。
本节选向后折而不是向前累加，正是因为向后折在算术上**不可能**丢字节——测试抓不到的
不变量，只能靠构造来保证。

### 6f. `decode_ns` 的真正大头：`ir::Inst` 每条走一次 `malloc`

上一轮的猜测是「`HIRUse` 都是独立池对象，小型内联 use 列表能去掉大部分流量」，未测。
**先量：** 在 `HIRValue::Use` 与 `HIRFunction::UseInst` 里加边际成本探针（每次多做 N 遍
同样的工作，11 次取中位数，25 个 guest 的 use 分布另计）：

| 项 | 单价 | 全量 | 占 `func_tests` 的 `decode_ns`（2.84 ms） |
|---|---|---|---|
| `ObjectPool::Create`（`HIRUse`） | **1.0 ns** | 23 036 次 | **0.8%** |
| `UseInst` 的元数据遍历 | ~3.3 ns/条 | 32 078 条 | ~3.7% |
| **`new Inst` + `delete`** | **24.9 ns** | 32 078 条 | **~19%（只算分配侧）** |
| 裸 `malloc+free(sizeof(Inst))` | 15.8 ns | — | — |

use 分布（25 个 guest，98 165 个 `HIRValue`）：**86.2% 恰好 1 个 use**、4.1% 为 0、
8.1% 为 2、1.6% ≥3。内联 use 槽确实能去掉 89% 的 `HIRUse` 池对象——**但那是 0.8%**。
**猜测被证伪**，没有做。

真正的大头是它旁边那条：`Inst : SlabObject<Inst, true>`，而 `InitializeSlabHeap`
**只有 `source/tests/main_case.cpp` 调用过**。也就是说 `swift_test` 用 slab，
`svm_translator_linux` 与任何嵌入方一直在走 `TryAllocate` 的退化分支：一次
seq_cst `ldar`（永远读到空表）+ 一次 `malloc`，每条 IR 指令一次。

**把 slab 打开不是解**：`SlabHeap::Initialize` 建自由表是每个对象一次 seq_cst CAS
（按 `main_case.cpp` 的规模是一百万次），而 `SlabAllocator::Allocate/Free` 本身各是一个
seq_cst CAS 循环，和 malloc 同量级。

**做法**（`instr.h`/`instr.cpp`）：`Inst` 自己的 `operator new`/`delete`，线程局部
64 KiB bump arena + free list；槽位对齐到 16 字节（`Inst` 是 `pack(1)`，而运行时到处
按普通指针读它的 `next_pseudo_inst`/`list_node`）。chunk **永不归还**——一条 `Inst`
可能被另一个线程释放，所以没有哪个 chunk 归某个线程所有；free list 因此也不设上限，
「挂在 free list 上」和「arena 里的空闲空间」是同一件事。

**中间结果值得记**：只做 free list（不做 bump）在默认的**函数模式**下是 **+0.5~+1.0%
的净亏**——因为函数模式根本不调用 `Block::DestroyInstrs`，IR 指令是**泄漏**的
（§6 已列为未测项），free list 永远是空的。块模式（会析构）同一版本 **−5.9%**。
这条差异本身就是「函数模式 IR 泄漏」的一个可测量后果。

**最终实测**（同一次交错、11 reps 中位数，含 §6e）：

| guest | 模式 | `translate_ns` | `decode_ns` | 仅 §6f（`SVM_ADVPC_COALESCE=0`）的 `translate_ns` |
|---|---|---|---|---|
| func_tests | 函数（默认） | **−3.36%** | **−9.69%** | −2.08% |
| real_busy | 函数 | −3.62% | −10.84% | −2.03% |
| real_hello | 函数 | −3.89% | −11.86% | −1.62% |
| func_tests_musl | 函数 | −2.05% | −8.63% | −2.12% |
| func_tests | 块（`SVM_FUNC_BASE=0`） | −4.91% | — | −4.97% |
| real_busy | 块 | −5.25% | — | −5.13% |
| real_hello | 块 | −5.19% | — | −5.41% |
| func_tests_musl | 块 | −5.27% | — | −5.33% |

max RSS 不升（func_tests 147 504 → 147 056 KiB，real_busy 149 136 → 147 152，
clone_lock_rmw 156 176 → 156 080）。

**墙钟：不做结论。** 宿主 loadavg 43，81 reps 交错、中位数：func_tests −1.05%、
real_busy −2.59%、real_hello −1.16%、func_tests_musl −0.97%——四个方向一致且与
「翻译占 59% × translate_ns −2~−4%」的预测相符，但**每一个都落在本次运行的 MAD
（1.4–3.4%）之内**，所以这四个数只能算「不矛盾」，不能算「更快」。

**变异测试**：

| 变异 | 结果 |
|---|---|
| B1：chunk 边界判据 `left < kInstSlot` 改成 `left == 0`（每个 chunk 溢出最多一格） | 25 个 e2e 全绿；`swift_test` **逐用例**跑（每个用例一个进程）也全绿；但**单进程跑完整个 `swift_test`** 三次分别 139/138/133（SIGSEGV/SIGBUS/SIGTRAP）。**被抓**——只在长命进程里，per-case harness 分配量不够碰到 chunk 边界 |
| B2：槽位不对齐（`kInstSlot = sizeof(Inst)`） | 25 个 e2e、单进程 `swift_test` 三次全绿。**没被抓**（arm64 普通访存容忍非对齐；对齐是按可移植性与 `ldp/stp` 保守保留的） |

## 7. 本轮做的唯一非测量改动

无。插桩全部在独立 worktree（`SVM_PROF`），主工作区只新增了
`source/translator/linux/tests/bench_*` 与本文档。未 commit。

（§6c / §3.4 续 / §6d 那一轮的改动是 6 个文件：`jit_context.cpp`、`ir.inc`、
`translator_alu.cpp`、`interpreter.cpp`、`decoder.cc`、`flags_elimination_pass.cpp`；
插桩同样只在独立 worktree，主工作区无残留——`git grep -n "emitstat\|svm_rsb_prof"`
应无输出。未 commit。）
