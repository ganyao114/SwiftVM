# W76：STREAM Scale/Add/Triad 与 Copy 不对称性核查

日期：2026-08-01

SwiftVM 基线：`404238aa9ebdb14839be1c796a2ff603c766abe4`

## 结论摘要

本轮只读核查，没有实现优化。结论有三点：

1. **题设的 Copy 对照并不是“同一个 16B 循环只少一条 FP 指令”。** 当前
   `stream_x64` 的 Copy 调入 glibc `__memcpy_ssse3`，热点 `0x416980` 每轮用
   5 对 `movaps/movntps` 搬 **80B**；Scale/Add/Triad 的热点则每轮只处理
   **16B**。SVM 每个循环 unit 固定有 11 条 flags 物化和 8 条 loop/control，Copy
   把这 19 条摊到 5 个向量，三项算术却每 16B 支付一次。这是最大的不对称来源。
2. 默认精确路径的 packed-f64 NaN guard 确实昂贵：Scale 为 `5/36=13.89%`，
   Add 为 `5/39=12.82%`，Triad 为 `10/47=21.28%`（分母是实际反复执行的
   taken-backedge 路径）。但从 Copy 归一化到每 16B 的差额看，guard 只解释
   Scale 差额的 19.84%、Triad 差额的 27.62%，不是主因；即使全部删除，循环
   仍为 31/34/37 条，远高于 FEX 的 10/11/12 条。
3. **W13 旗标对不应翻盘。** 当前 master 上
   `SVM_XMM_STATIC=1 SVM_SSE_NAN_FAST=1` 把 Scale/Add/Triad 稳态代码
   36/39/47 缩为 30/34/36，但无探针三次中位吞吐反而下降
   **31.10% / 24.65% / 10.53%**。XMM_STATIC 单开得到几乎相同的负数；
   NAN_FAST 已不再能抵消静态 XMM 路径成本。W13 的历史正收益测于 W37 之前的
   13 条 inline NaN 修正；当前默认已是 W37 的 5 条 guard + cold repair，旧结论
   不能外推。

FEX 同址 JIT 抓取也确认了语义与形态差：它对 `mulpd/addpd` 直接发一条 ARM64
`fmul/fadd`，没有 NaN 检测或 payload 修复；循环 flags 保持 host-local，只需
`cfinv`，taken backedge 不写 state。它并非“默认 FTZ”：源码把 guest MXCSR.FTZ
映射到 FPCR.FZ；默认 MXCSR 下 FTZ 关闭。真正不同的是 NaN payload、quieting、
operand priority 与 invalid-indefinite 的位级结果不做 x86 修复。因此 FEX 数字与
SwiftVM `NAN_FAST` 才是近似的性能语义口径，不能把 FEX 结果当作精确语义免费实现。

## 1. Guest 热循环并不等形

guest 反汇编：

| kernel | guest PC | 每轮 guest 主体 | 每轮字节 |
|---|---:|---|---:|
| Copy | `0x416980` | 5× load XMM + 5× non-temporal store | 80 |
| Scale | `0x401a70` | load + `mulpd` + store | 16 |
| Add | `0x401b00` | 2× load + `addpd` + store | 16 |
| Triad | `0x401ba0` | 2× load + `mulpd` + `addpd` + store | 16 |

Copy 的 guest 原文是：

```text
416980 movaps  xmm1,[rsi+0x10]
416984 movaps  xmm2,[rsi+0x20]
416988 movaps  xmm3,[rsi+0x30]
41698c movaps  xmm4,[rsi+0x40]
416990 movaps  xmm5,[rsi+0x50]
416994..4169a4  5× movntps
4169a8 add rdi,0x50
4169ac add rsi,0x50
4169b0 cmp rcx,rdi
4169b3 ja  0x416980
```

Scale/Triad 每轮分别只有：

```text
401a70 movapd xmm0,[r13+rax]
401a77 mulpd  xmm0,xmm1
401a7b movaps [r15+rax],xmm0
401a80 add rax,0x10
401a84 cmp rax,0x1312d000
401a8a jne 0x401a70

401ba0 movapd xmm0,[r13+rax]
401ba7 mulpd  xmm0,xmm1
401bab addpd  xmm0,[r15+rax]
401bb1 movaps [r14+rax],xmm0
401bb6 add rax,0x10
401bba cmp rax,0x1312d000
401bc0 jne 0x401ba0
```

所以 Copy 的好看数字首先包含 guest libc 的 5× unroll 优势，不能直接推出
“SwiftVM 访存快、只在 FP op 上慢 2 倍”。

## 2. SwiftVM 默认热路径分类

### 2.1 口径

`SVM_RA_HOT_COALESCE` 的 `host_static` 包含一次性 fallthrough/link tail；每个
unit 该 tail 为 9 条，但 loop backedge 命中约 4 亿次、fallthrough 只命中 20 次。
本节主表只计实际稳态 taken path：从 unit 入口到回跳 `b`（含）为止。

分类互斥：

- guest memory/address：guest 地址计算及真正的 `[x10]` LDR/STR；
- FP body：`fmul/fadd` 本体；
- NaN guard：W71 同口径的 `fcmeq + 2×umov + and + cbz`；
- XMM bridge：guard 之外的静态 XMM/FPR register copy；
- state：普通 `[x28]` GPR/XMM uniform 读写，不含 flags byte；
- flags：x26、PF/AF、NZCV pack，不含执行 guest CMP 的 `subs`；
- loop/control：index/bound、`subs`、分支。

### 2.2 默认 OFF

| kernel | steady host | memory/address | FP body | NaN guard | XMM bridge | state | flags | loop/control |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Scale | 36 | 6 (16.67%) | 1 (2.78%) | 5 (13.89%) | 0 | 5 (13.89%) | 11 (30.56%) | 8 (22.22%) |
| Add | 39 | 9 (23.08%) | 1 (2.56%) | 5 (12.82%) | 0 | 5 (12.82%) | 11 (28.21%) | 8 (20.51%) |
| Triad | 47 | 9 (19.15%) | 2 (4.26%) | 10 (21.28%) | 0 | 7 (14.89%) | 11 (23.40%) | 8 (17.02%) |

含一次性 exit tail 的 W71 静态数为 45/48/56；对应 guard 占比是
11.11%/10.42%/17.86%。两种分母都给出，避免把冷 exit 当每轮执行。

Scale 的完整关键形状：

```text
ldr x6,[x28,#760]                 state/base
add w10,w6,w22
add x10,x24,w10,uxtw
ldr q0,[x10]                      guest load
ldr q1,[x28,#832]                 XMM1 state
fmul v2.2d,v0.2d,v1.2d            FP body
fcmeq v14.2d,v2.2d,v2.2d
umov x6,v14.d[0]
umov x7,v14.d[1]
and x6,x6,x7
cbz x6,cold                       5-instruction guard
ldr/str ... [x28]                 XMM/state commit
add/add/str q2,[x10]              guest store
...                               11 flags + 8 loop/control
```

### 2.3 Copy 归一化

Copy 稳态为 54 host 条/80B：30 guest memory/address、5 XMM state、11 flags、
8 loop/control。归一到 16B 是 **10.8 条**：memory 6、state 1、固定 flags/control
3.8。

与 Scale 的 36 条相比，额外 25.2 条/16B 的来源是：

| 来源 | 条数 | 占 Copy→Scale 差额 |
|---|---:|---:|
| Copy 未能摊薄的 flags/control | 15.2 | 60.32% |
| NaN guard | 5 | 19.84% |
| 额外 XMM/state | 4 | 15.87% |
| FP 本体 | 1 | 3.97% |

Triad 相对 Copy 的 36.2 条差额中：固定开销 42.0%、NaN 27.6%、state 16.6%、
额外 memory/address 8.3%、FP 本体 5.5%。因此 NaN 是重要单项，但 Copy/算术
不对称的首要原因是 loop unroll 与固定边界成本的组合。

## 3. W13 双开形态

### 3.1 静态重排

| kernel | steady host | memory/address | FP body | NaN guard | XMM bridge | state | flags | loop/control |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Scale | 30 | 6 (20.00%) | 1 (3.33%) | 0 | 2 (6.67%) | 2 (6.67%) | 11 (36.67%) | 8 (26.67%) |
| Add | 34 | 9 (26.47%) | 1 (2.94%) | 0 | 2 (5.88%) | 3 (8.82%) | 11 (32.35%) | 8 (23.53%) |
| Triad | 36 | 9 (25.00%) | 2 (5.56%) | 0 | 3 (8.33%) | 3 (8.33%) | 11 (30.56%) | 8 (22.22%) |

含 exit tail 为 39/43/45。双开删除所有 guard，并把普通 XMM state traffic
换成固定 v16/v17 与动态 v0/v1 间的 `mov`：

```text
ldr q0,[x10]
mov v16.16b,v0.16b
fmul v0.2d,v16.2d,v17.2d
mov v16.16b,v0.16b
...
str q16,[x10]
```

RA_SHAPE 显示 FPR pool 从 28 降到 12，但当前 STREAM 全程仍是 0 spill，hot unit
最大 live 仅 2。因此本次负收益不是 spill 数增加；它与固定寄存器 copy/依赖链及
静态驻留形态有关。要进一步归因需要 PMU，本核查不臆测具体 cycle 原因。

### 3.2 当前 master 的诊断性三次中位

以下均关闭 HOT/SHAPE/dump 探针，三次顺序运行，仅用于淘汰大幅负方案，不当作正式
翻盘 A/B。FEX 是 harness 配置 `MULTIBLOCK=1, ABILOCALFLAGS=1` 的三次中位。

| 配置 | Copy | Scale | Add | Triad |
|---|---:|---:|---:|---:|
| SwiftVM 默认 | 61,984 | 28,344 | 38,761 | 32,629 |
| NAN_FAST only | 62,101 | 29,022 | 43,036 | 40,273 |
| XMM_STATIC only | 85,527 | 19,532 | 29,213 | 29,180 |
| W13 双开 | 85,696 | 19,530 | 29,208 | 29,194 |
| PIN_EXT=3 旁证 | 60,860 | 27,684 | 40,942 | 34,169 |
| FEX | 89,625 | 86,993 | 97,533 | 97,107 |

相对默认：

- NAN_FAST only：Copy +0.19%、Scale +2.39%、Add +11.03%、Triad +23.43%；
- XMM_STATIC only：Copy +37.98%，Scale −31.09%、Add −24.63%、Triad −10.57%；
- W13 双开：Copy +38.26%，Scale −31.10%、Add −24.65%、Triad −10.53%。

双开与 XMM_STATIC-only 几乎重合，说明当前 W37 后的 5 条 guard 已不足以抵消静态
XMM 路径成本。W13 旗标对当前连进入七对 A/B 的方向门都没有通过。

默认 SwiftVM/FEX 同机诊断比为 Copy 0.692、Scale 0.326、Add 0.397、Triad
0.336，与 baseline4 的绝对值不同但不对称方向一致。NAN_FAST-only 也仅提升到
0.693/0.334/0.441/0.415，证明 NaN 语义差异远不足以把三项拉到 0.8。

PIN_EXT=3 只让 host static 45/48/56 降到 43/45/53，Scale 还回退 2.33%；全
STREAM 出现 43 spill units、1,289 spill defs。它不能作为 STREAM 专用解法。

## 4. FEX 同 guest 循环

### 4.1 实码

安装的 FEX 未带 VIXL disassembler。本轮打开 `FEX_BLOCKJITNAMING=1`，从
`/tmp/perf-PID.map` 找到三个 guest entry，SIGSTOP 后由父进程读 `/proc/PID/mem`。
为对齐 SwiftVM 单块 unit，抓取时设 `FEX_MULTIBLOCK=0`；正式计时仍用 harness 的
`MULTIBLOCK=1`。

FEX steady taken path：

```text
# Scale: 10 instructions
ldr  q16,[x17,x4,sxtx]
fmul v16.2d,v16.2d,v17.2d
str  q16,[x29,x4,sxtx]
add  x4,x4,#16
mov/movk bound
mov  x27,x4
subs x26,x4,x20
cfinv
b.ne loop

# Add: 11 instructions
ldr q16,[x19,x4,sxtx]
ldr q2,[x29,x4,sxtx]
fadd v16.2d,v16.2d,v2.2d
str q16,[x17,x4,sxtx]
... same 7-instruction loop tail ...

# Triad: 12 instructions
ldr q16,[x17,x4,sxtx]
fmul v16.2d,v16.2d,v17.2d
ldr q2,[x29,x4,sxtx]
fadd v16.2d,v16.2d,v2.2d
str q16,[x19,x4,sxtx]
... same 7-instruction loop tail ...
```

其分类为：

| kernel | total | memory | FP body | NaN | bridge/state | flags normalization | loop/control |
|---|---:|---:|---:|---:|---:|---:|---:|
| Scale | 10 | 2 | 1 | 0 | 0 | 1 (`cfinv`) | 6 |
| Add | 11 | 3 | 1 | 0 | 0 | 1 | 6 |
| Triad | 12 | 3 | 2 | 0 | 0 | 1 | 6 |

fallthrough exit 另有 `ldr literal + blr` 两条，只在循环结束执行。FEX 的优势不仅是
没有 NaN guard：它把 guest base 放在 host GPR、XMM value 留在 FPR，backedge 上
也不把 flags/XMM 写回 state。SVM 默认 steady 是其 4.36–4.67 倍指令数。

### 4.2 NaN 与 FPCR 语义

FEX source snapshot：`/tmp/w75-fex-audit`，commit
`2fdbff3d1c0058b5851b84ae9db2c00d88f02b60`。

- `FEXCore/Source/Interface/Core/JIT/VectorOps.cpp:142-179` 的
  `DEF_FBINOP` 对 packed 128-bit 直接调用 ARM op；`:251` 与 `:253` 把 VFAdd/
  VFMul 分别映为 `fadd/fmul`。没有 result self-compare、NaN branch 或 repair。
- `MiscOps.cpp:130-148` 把 guest rounding 与 MXCSR.FTZ 写进 FPCR；FTZ 不是无条件
  开启。FEAT_AFP 下 DAZ 才映到 FPCR.FIZ。
- `Arm64Emitter.cpp:641-659` 只设置 NEP/AH（及条件 FIZ），没有设置 FPCR.DN；
  代码注释也限定 AH 的 NaN 影响主要是 min/max。

因此准确表述是：FEX 保留 guest rounding/FTZ 控制，但对普通 packed add/mul 接受
ARM 的 NaN propagation；它不保证 x86 的 operand payload priority、SNaN quieting、
NaN sign 与 invalid indefinite 位形。SwiftVM 默认在
`translator_alu.cpp:1659-1669` 用 5 条 f64 guard，命中后在 `:1727-1749` 修复这些
位级语义。有限数 STREAM 输出一致不代表 NaN 语义等价。

## 5. 决断与建议排序

当前 Scale/Add/Triad 要到 0.8，分别还需约 2.46×/2.01×/2.38×。不存在一个
“删 guard”小改即可达到目标；需要先修测量口径，再处理 hot-backedge 固定成本。

| 顺序 | 路径 | 实测/机械收益面 | 风险 | 开关与依赖 | 建议 |
|---:|---|---|---|---|---|
| P0 | matched-unroll benchmark | Copy 的固定成本被 5× 摊薄；先提供同为 16B/iter 的 Copy，或把四 kernel 统一 unroll | 低；只改 benchmark | 不应算 SVM 优化；不得改现 harness 结果 | **测量前置**，否则 Copy 对照持续误导 |
| P1 | self-backedge flags local，cold exit 才 materialize | 每轮最多删 11 条；Scale 36→25（1.44× static），Triad 47→36（1.31×） | 高；W30 edge/fault 历史红线 | 默认 OFF；依赖 backedge/exit 双边证明、fault map、interrupt 精度 | **最值得的实现 spike**，先做只读 edge-frequency + 设计审计 |
| P2 | 精确 packed-f64 guard 缩短/finite proof | 上限 Scale 13.89%、Triad 21.28%；NAN_FAST-only wall 为 +2.4/+11.0/+23.4% | 中高；NaN bit-exact | 精确方案可默认 OFF 起步；需 NaN payload/SNaN/indefinite 全矩阵 | 可做，但单独远达不到 0.8 |
| P3 | loop-local XMM/state forwarding，不缩全局 FPR pool | state 为 12.8–14.9%；same-offset 至少 1 条/轮明确可疑 | 高；fault/edge 可观察性，W17 轴曾全负 | 默认 OFF；不得把 FPR pool 28→12，不得新增 spill | P1 后再评估，不能复活全局 XMM pin |
| P4 | hot self-loop 4–5× JIT unroll | 19 条 fixed cost 可由每 16B 19 降至约 3.8；Scale 理想 36→20.8、Triad 47→31.8 | 很高；code size、SMC、fault/interrupt 多映射 | 默认 OFF；依赖精确每迭代 fault map 与 P1 flags 边界 | 只有 P1/P3 后仍不足才立项 |
| P5 | `SVM_SSE_NAN_FAST=1` FEX-comparison profile | 当前 ratios 仅 0.334/0.441/0.415 | 语义高风险 | 必须明确 reduced-NaN profile，默认 OFF | 可作公平性能口径，**不能当精确默认** |
| 关闭 | W13 双开翻盘 | 缩码 13–23%，但当前 wall −31/−25/−11% | 已有确定负方向 | 继续默认 OFF | **不翻盘** |
| 关闭 | PIN_EXT=3 作为 STREAM 解法 | 静态只省 2–3 条，Scale 负，且全程新增 1,289 spills | 高/全局负账 | 现有 opt-in 足够 | 不推进 |

W13 若未来因独立机器结果要求重开，最低数据门应是：当前 master、无探针、至少 7
对交错；Scale/Add/Triad 各自 95% CI 为正；c-ray/smallpt 不回退；RA_SHAPE spill/
high-water/helper snapshot 不增；并明确接受 `vec_float_nan_pressure` 的 reduced-NaN
差异。当前三次预筛已经大幅负向，不建议消耗正式 A/B 资源。

## 6. 复现命令与证据

### 6.1 构建

```sh
orb -m wine-ci bash -lc '
  cmake -S /private/tmp/w65 -B /tmp/w76-build -DCMAKE_BUILD_TYPE=Release &&
  cmake --build /tmp/w76-build --target svm_translator_linux -j8'
```

### 6.2 HOT/SHAPE 与 dump

默认：

```sh
orb -m wine-ci bash -lc '
  rm -rf /tmp/w76-off-hot && mkdir -p /tmp/w76-off-hot &&
  cd /tmp/w76-off-hot &&
  env -u SVM_JIT_CACHE \
    SVM_RA_HOT_COALESCE=/tmp/w76-off-hot/hot.count \
    SVM_RA_SHAPE_PROF=/tmp/w76-off-hot/shape.count \
    /tmp/w76-build/source/translator/linux/svm_translator_linux \
    /Users/swift/CLionProjects/SwiftVM-bench/bin/stream_x64 \
    >stdout.log 2>stderr.log'
```

W13 双开把命令前缀增加：

```sh
SVM_XMM_STATIC=1 SVM_SSE_NAN_FAST=1
```

IR/host dump：

```sh
env -u SVM_JIT_CACHE SVM_DUMP_IR=1 SVM_DUMP_IR_POST=1 \
  SVM_VIXL_HOST_DUMP=1 svm_translator_linux stream_x64 >all.log 2>all.err
```

证据目录：

- `/tmp/w76-off-hot/{hot.count,shape.count,stdout.log}`；
- `/tmp/w76-on-hot/{hot.count,shape.count,stdout.log}`；
- `/tmp/w76-{nan,xmm,pin3}-hot/`；
- `/tmp/w76-{off,on}-dump/all.err`；
- `/tmp/w76-{off,on}-{401a70,401b00,401ba0,416980}.asm`；
- 无探针三次：`/tmp/w76-timing/`。

### 6.3 FEX JIT 抓取

正式对照配置为：

```sh
FEX_MULTIBLOCK=1 FEX_ABILOCALFLAGS=1 FEX_CACHEOBJECTCODECOMPILATION=0 \
  /usr/bin/FEXInterpreter stream_x64
```

单块抓取设置 `FEX_MULTIBLOCK=0 FEX_BLOCKJITNAMING=1`；perf-map 名称中的 guest
file offset 分别为 `+0x1a70/+0x1b00/+0x1ba0`。父进程在 map 出现后 SIGSTOP，
用 `os.pread(/proc/PID/mem, size, host_addr)` 保存，再以：

```sh
objdump -D -b binary -m aarch64 fex-1a70.bin > fex-1a70.asm
```

反汇编。证据位于 Ubuntu VM `/tmp/w76-fex-live/`；三次正式配置计时位于
`/tmp/w76-fex-timing/`。

## 7. 工作树纪律

本轮没有改动 source、tests、golden、linker 或 benchmark harness；最终只新增
`docs/w76-stream-fp-audit.md`。

