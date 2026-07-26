# SwiftVM 性能基线与剖析（2026-07-26）

本轮**只建立基线与剖析、产出优化候选清单**，未做任何优化实现。全部数字来自实测，
未估算、未外推；测不到的地方明确写「测不到」。

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

两项都用「同一干净基座产出的两个二进制、同一次调用内交错、中位数」测得，
所以结论不受宿主负载影响。#3 另有 25 个 guest e2e 程序的**退出码逐行比对**。

一条教训值得记住：#3 第一次测量是在主工作树上做的，而树里有其他并行改动
（`signal_handler.cpp` 等），结果把别人的 SIGBUS 算到了这个改动头上。**性能
与行为比对必须在独立 worktree 上取基线。**

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

## 7. 本轮做的唯一非测量改动

无。插桩全部在独立 worktree（`SVM_PROF`），主工作区只新增了
`source/translator/linux/tests/bench_*` 与本文档。未 commit。
