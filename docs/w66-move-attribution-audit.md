# move/桥接残余桶逐条归因审计

## 0. 结论先行

**结论：CoreMark 的 34.483159% move 桶已逐条闭合，但没有发现一条不落回既有封存路线、且机械可消下界达到 0.5% host 的新机制。** 这次审计把“move 很多”拆成了七个互斥责任类；最大的三类分别是宽度桥 `11.845502% host`、fixed-home 读写未合并 `9.958702%`、其他算术/常量/EA 搬运 `7.579915%`。它们并不是同一个 RA 缺口：

| 互斥成因 | 动态 move 条数 | 占 host | 占 move | 结论 |
|---|---:|---:|---:|---|
| 1. 宽度桥接 | 6,972,622,544 | 11.845501566% | 34.351555537% | raw 池大；通用 width-chain/v2 已被生产 region 态与 STREAM 门证伪，不重开 |
| 2. RA 合并被拒 / fixed-home transport | 5,861,994,920 | 9.958701990% | 28.879900321% | FEX SRA 确实更短；SwiftVM 的可用载体就是已否 fixed-class pin 或跨 block home SSA |
| 3. 固定需求挤兑（本轮实际全为 flags） | 2,151,479,289 | 3.655059646% | 10.599549856% | x26 PF/AF/packed-flags 更新；W-β/lazy-flags 路线已封存 |
| 4. helper/调用边界 | 143 | 0.000000243% | 0.000000705% | CoreMark 可忽略，无候选 |
| 5. region/terminal 边界 | 849,970,519 | 1.443979945% | 4.187493200% | current location、RSB、terminal tail；无新 fault/signal 载体 |
| 6. XMM/FPR 残余 | 224 | 0.000000381% | 0.000001104% | CoreMark 为零量级；zip7/sqlite 抽查仍低于 0.5%，全 XMM 固定映射已否 |
| 7. 其他 | 4,461,768,762 | 7.579915380% | 21.981499278% | LoadImm 1.795B、窄算术 operand/flags 前处理、EA copy；均非一个可统一删除的 owner |
| **合计** | **20,297,836,401** | **34.483159151%** | **100%** | 与 hot log 仅差 101 条 |

最关键的两个反直觉结果：

1. **x27 cache-base 在整个 CoreMark region 形状里直接贡献的官方 move 是 0 条；x24/x28 也都是 0。** B0 的 x27 `0.68%` 罚单不能记到“mov x27”头上，它来自非 move 发码或分配压力。固定寄存器可见税是 x26 flags 链，不是 cache-base reload。
2. top-10 热 unit 已覆盖 `9,556,490,434` 条 move，即全 move 的 **47.081326%**。这些 unit 的每一条 move 都能落到下文唯一类别；没有“未知大尾巴”。

因此本轮不建议新增 gate。若将来有新基础设施，重开顺序只能是：新的 fault-safe unit-local home SSA > 新的 flags 边界载体 > 新的异步/边界 ABI；不能再以“move 桶大”为由重开 width-chain 或 fixed-class。

## 1. 口径、命令与闭合

### 1.1 生产态口径

本轮所有动态数字都来自 region 默认生产态，显式使用：

```sh
env -u SVM_JIT_CACHE -u SVM_EXEC_PROF \
  SVM_REGION_EDGES=1 \
  SVM_PROF=2 SVM_DENSITY_PROF=1 \
  SVM_RA_HOT_COALESCE=/private/tmp/move-attribution-20260812/coremark-prod150/hot.log \
  SVM_RA_HOT_COALESCE_ALL=1 \
  build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/coremark_x64 \
  0 0 0x66 150000 7 1 2000
```

结果：

```text
host_dynamic=58,863,042,273
move_dynamic=20,297,836,502
move_pct=34.483159
spill_dynamic=4
crcfinal=0x25b5
Correct operation validated.
```

逐条 Capstone + `[svm-gap-op]` owner join 得到：

```text
pcs=2848, missing=0
host_dynamic=58,863,041,847
official_move_dynamic=20,297,836,401
hotlog_move_dynamic=20,297,836,502
closure delta=-101
```

`101 / 20,297,836,502 = 4.98e-9`，来自 VIXL/Capstone alias 口径的极小尾差，不影响任何百分比的前九位。块态只用于读 emitter 形态，没有拿块态 entries 进入正文。

原始证据：

- `/private/tmp/move-attribution-20260812/coremark-prod150/{hot.log,stderr.log,stdout.log}`；
- `/private/tmp/move-attribution-20260812/coremark-re1/stderr.log`：逐 IR span + host bytes；
- `/private/tmp/move-attribution-20260812/coremark-prod-owner.{txt,csv}`；
- `/private/tmp/move-attribution-20260812/coremark-top10-moves.txt`。

### 1.2 分类优先级

每条最终 A64 `mov/fmov/umov/smov/ins/dup/uxt*/sxt*/ubfx/sbfx/bfi/bfxil/extr` 或 `lsl/lsr/asr #0` 只进一类，优先级为：

1. terminal span → region 边界；
2. helper span → helper；
3. Get/SetHostFPR → FPR；
4. Set/GetHostGPR → RA fixed-home transport；
5. `BitExtract/ZeroExtend*/SignExtend` → 宽度桥；
6. PF/AF/packed x26 链（含为 AF 提位的 `ubfx`）→ 固定需求；
7. 其余 → 其他。

这样避免把 terminal 内的一条 x26 更新同时算进“fixed”和“boundary”。

## 2. RA guard：为什么这些 copy 没有被 W-α 吃掉

当前实现的 guard 链在 `source/runtime/ir/opts/register_alloc_coalesce_gpr.cpp`：

| guard | file:line | 本轮对应形态 |
|---|---|---|
| fixed-home 不在 coalesce target 集 | `:16-18`, `:658-661` | x19/x20/x21 等 callee-saved home 的 GetHost；根本不进 W-α |
| read 前没有同 block 最新发布，或中间有 observer | `:683-700` | region block 入口的 x22/x23/x29/x0..x9 read；不能凭跨 HIR block 状态猜值 |
| 最新发布未证明 high32=0 | `:702-724` | U32 view 只是读 W 视图、不是一次物理 W write |
| read 活跃期内 home 再写 | `:726-734` | 必须保持 snapshot 语义 |
| partial SetHost offset 非零 | `:808-811` | `bfxil x22/x29,...,#0,#8/#16`；不允许冒充完整发布 |
| producer multi-use / store 不是 last use | `:819-829` | 一个 SSA 同时供算术、flags 或另一个 home |
| zext 根不是已知 W write | `:835-845`, `HasKnownWWrite :105-127` | `ZeroExtend32To64(GetHostGPR)`；读 W 不等于把 X 高半写零 |
| producer 不在 alias-emission 白名单 | `:848-854`, 白名单 `:20-57` | LoadMemory、ZeroExtend32、某些复杂 producer |
| 第三方 fixed-home 活跃区间相交 | `:745-786`, 调用 `:963-964` | 同一 home 被另一个 tie/producer 占用 |
| fault/helper/flags observer 或同 home 读写穿窗 | `:967-990`, observer 表 `:77-100` | memory、SaveFlags、CallLambda、Set/GetHost 穿过 producer→publish 窗 |
| producer 输入本身占 target 且仍活 | `:992-1000` | destructive alias 会毁尚未消费的输入 |

emitter 的 fallback 在 `translator_mem.cpp:957-1024`：未标 coalesced 时真实发 `Mov/Ubfx`；这不是空兜底。下文 top-10 中的 R 类都可由上述某一条直接解释。

为避免从最终编码反猜 guard，本轮临时在上述每个 `continue` 前打印
`block/id/target/reason`，用同一 build 跑短 CoreMark，然后已机械撤销并重建。
原始日志在 `/private/tmp/move-attribution-20260812/coremark-guard/stderr.log`，
逐 move 与 reject reason 的 join 在
`/private/tmp/move-attribution-20260812/coremark-top10-guards.txt`。top-10
实际出现的拒因只有 `not-u32`、`not-pinned-target`、`no-latest-store`、
`high32-not-zero`、`not-last-use`、`width-or-high32-proof`、
`producer-not-eligible`、`observer-or-home-access`。**没有任何 top-10 move
命中 `third-party-target-conflict`**，所以不存在一个未披露的“第三方占家”；
被保护的目标家已在逐条表中明确为 x0/x1/x20/x21/x22/x23/x29。

### 2.1 FEX 的差别

FEX 不是“guard 更松”，而是分配模型不同。其当前源码：

- `.cache/FEX/.../IR/Passes/RegisterAllocationPass.cpp:183-217` 把 Load/StoreRegister 解成 `GPRFixed/FPRFixed` 家；
- `:220-228` 在 SSA 家与 fixed 家一致时将 Load/StoreRegister 判为 trivial；
- `:608-632` 反向建立 SRA affinity，并在中间出现同 home load/store hazard 时取消。

此前同 guest 的 `/proc/$pid/mem` 真码仍保存在 `/private/tmp/coremark-forensic-20260810/fex-current/`。matrix object 的热段直接是：

```asm
ldrh w20,[x24]
sxth w6,w20
ldrh w20,[x21]
sxth w11,w20
mul  w6,w6,w11
add  w10,w10,w6
subs w20,w13,w4
cfinv
b.ne ...
```

即宽运算值在既定家中连续使用，不需要 SwiftVM 的每节点 `lsr #0/mov`。但把这个模型搬回 SwiftVM 就是已经实测失败的 fixed-class pin / 跨 block ABI，不是一个尚未尝试的局部 guard。

## 3. top-10 热 unit：逐条 move 唯一归因

标签：`W` 宽度桥；`R` RA/fixed-home；`F` flags 固定需求；`B` region/terminal；`V` FPR；`O` 其他。top-10 按 `entries × host_static` 排序。

| PC | entries | host/move static | move dynamic | 占全 host |
|---|---:|---:|---:|---:|
| 0x4033bb | 145,200,000 | 23 / 6 | 871,200,000 | 1.480046% |
| 0x403630 | 76,800,000 | 36 / 11 | 844,800,000 | 1.435196% |
| 0x403688 | 76,800,000 | 36 / 11 | 844,800,000 | 1.435196% |
| 0x4034b0 | 193,200,000 | 13 / 4 | 772,800,000 | 1.312878% |
| 0x402218 | 31,207,643 | 75 / 38 | 1,185,890,434 | 2.014660% |
| 0x402de8 | 43,200,000 | 54 / 31 | 1,339,200,000 | 2.275112% |
| 0x4033ec | 237,600,000 | 9 / 4 | 950,400,000 | 1.614595% |
| 0x402df8 | 48,600,000 | 42 / 23 | 1,117,800,000 | 1.898984% |
| 0x403552 | 92,400,000 | 22 / 6 | 554,400,000 | 0.941847% |
| 0x403320 | 153,600,000 | 13 / 7 | 1,075,200,000 | 1.826613% |

### 3.1 0x4033bb

```text
+008 mov   w29,w7                 R  SetHostGPR id65；not-last-use(:819-829)，home=x29
+010 uxtb  w6,w7                  W  BitExtract
+014 uxtb  w8,w7                  W  BitExtract 的第二 use
+01c mov   w6,w7                  O  8-bit Or operand 对齐
+020 sxtb  x8,w6                  O  同一窄 Or/flags 语义链
+038 bfxil x26,x6,#0,#8           F  PF 发布到 packed x26
```

### 3.2 0x403630 / 0x403688（逐条相同）

```text
+000 ubfx  x6,x22,#0,#32          R  block 入口 read；无 latest_store(:683-700)，home=x22
+004 mov   w7,w6                  W  ZeroExtend32To64
+008 mov   w22,w7                 R  SetHost id2；not-last-use(:819-829)，home=x22
+00c mov   x6,#1                  O  LoadImm；此常量同时参与 EA/算术，不是当前 single-use fold
+034 mov   x22,x7                 R  SetHost id20；not-last-use(:819-829)，home=x22
+038 mov   w6,#0                  O  LoadImm，窄 Sub 输入
+044 mov   w9,w6                  O  destructive Sub operand copy
+050 bfxil x26,x7,#0,#8           F  PF
+05c ubfx  x11,x11,#4,#1          F  AF bit extraction
+060 bfi   x26,x11,#26,#1         F  AF 写 packed x26
+064 mov   w6,#1                  O  polarity/结果常量
```

### 3.3 0x4034b0

```text
+000 ubfx x6,x29,#0,#32           R  block 入口 read；无 latest_store，home=x29
+008 mov  w6,#9                   O  LoadImm
+00c uxtb w8,w29                  W  byte view
+014 mov  w9,w6                   O  narrow Sub destructive input
```

### 3.4 0x402218

```text
+000 mov   x6,x1                  O  GetOperand/EA copy
+008 mov   w22,w7                 R  SetHost id13；not-last-use(:819-829)，home=x22
+00c lsr   w6,w7,#0               W  BitExtract
+010 mov   w8,w6                  W  ZeroExtend32To64
+014 mov   w29,w8                 R  SetHost id17；not-last-use(:819-829)，home=x29
+018 uxth  w6,w7                  W  BitExtract16
+01c uxth  w7,w6                  W  ZeroExtend32
+024 bfxil x22,x6,#0,#16          R  SetHost id21；16-bit width/high32 proof拒绝(:864-868)
+028 uxtb  w6,w8                  W  BitExtract8
+02c uxtb  w7,w8                  W  第二 view
+034 bfxil x29,x8,#0,#8           R  SetHost id26；8-bit width/high32 proof拒绝(:864-868)
+038 ubfx  x6,x29,#0,#32          R  GetHost id28；latest publish 的 high32 未证零(:702-724)，home=x29
+03c ubfx  x7,x22,#0,#32          R  GetHost id29；high32 未证零，home=x22
+044 mov   w22,w8                 R  SetHost id32；not-last-use，home=x22
+048 uxth  w6,w8                  W  BitExtract16
+04c mov   x7,x1                  O  GetOperand/EA copy
+054 mov   x6,x0                  O  GetOperand/EA copy
+05c mov   w22,w7                 R  SetHost id43；not-last-use，home=x22
+060 lsr   w6,w7,#0               W  BitExtract
+064 mov   w8,w6                  W  ZeroExtend32To64
+068 mov   w29,w8                 R  SetHost id46；not-last-use，home=x29
+06c uxth  w6,w7                  W  BitExtract16
+070 uxth  w7,w6                  W  ZeroExtend32
+078 bfxil x22,x6,#0,#16          R  SetHost id50；16-bit width/high32 proof拒绝
+07c uxtb  w6,w8                  W  BitExtract8
+080 uxtb  w7,w8                  W  第二 view
+088 bfxil x29,x8,#0,#8           R  SetHost id55；8-bit width/high32 proof拒绝
+08c ubfx  x6,x29,#0,#32          R  GetHost id57；high32 未证零，home=x29
+090 ubfx  x7,x22,#0,#32          R  GetHost id58；high32 未证零，home=x22
+098 mov   w22,w8                 R  SetHost id61；not-last-use，home=x22
+0a4 mov   w29,w7                 R  SetHost id68；not-last-use，home=x29
+0a8 uxth  w6,w8                  W  BitExtract16
+0b8 mov   w22,w8                 R  SetHost id78；not-last-use，home=x22
+0c0 bfxil x26,x6,#0,#8           F  PF
+0cc ubfx  x9,x9,#4,#1            F  AF extraction
+0d0 bfi   x26,x9,#26,#1          F  AF packed write
+0d4 mov   w7,#1                  O  polarity/constant
+0dc mov   w22,w6                 R  SetHost id86；observer/home-access 穿过 producer→store(:967-990)，home=x22
```

这里的 `R` 并非都“差一个 guard 就能删”：`bfxil` 是 8/16-bit architectural write，未满足完整 W/X width proof；两个 read 必须保持旧 home snapshot；多次 x22/x29 publish 的窗口彼此交错，正是 sqlite/width-chain 判例禁止的半事务状态。

### 3.5 0x402de8

```text
+004 mov  w7,w6                   W  ZeroExtend32To64
+008 mov  w22,w7                  R  SetHost id4；not-last-use，home=x22
+00c ubfx x0,x20,#0,#32           R  home=x20 不在 coalesce target 集(:658-661)
+010 ubfx x23,x21,#0,#32          R  home=x21 不在 target 集
+014 mov  w6,w1                   R  home=x1 read，无同 block latest_store
+018 lsr  w11,w6,#0               W  BitExtract
+030 lsr  w6,w23,#0               W  BitExtract
+034 mov  w22,w6                  W  ZeroExtend32To64
+038 lsr  w6,w0,#0                W  BitExtract
+03c mov  w29,w6                  W  ZeroExtend32To64
+040 lsr  w6,w23,#0               W  BitExtract
+048 lsr  w13,w0,#0               W  BitExtract
+058 mov  w29,w8                  W  ZeroExtend32To64
+064 mov  w22,w7                  W  ZeroExtend32To64
+068 lsr  w7,w22,#0               W  BitExtract
+06c lsr  w8,w29,#0               W  BitExtract
+074 mov  w22,w6                  W  ZeroExtend32To64
+078 lsr  w7,w22,#0               W  BitExtract
+07c mov  w29,w7                  W  ZeroExtend32To64
+080 lsr  w6,w22,#0               W  BitExtract
+088 mov  w22,w8                  W  ZeroExtend32To64
+08c lsr  w7,w29,#0               W  BitExtract
+094 mov  w29,w6                  W  ZeroExtend32To64
+098 lsr  w8,w22,#0               W  BitExtract
+0a0 lsr  w6,w29,#0               W  BitExtract
+0a8 lsr  w8,w29,#0               W  BitExtract
+0ac lsr  w7,w22,#0               W  BitExtract
+0b4 mov  w29,w6                  W  ZeroExtend32To64
+0b8 lsr  w6,w29,#0               W  BitExtract
+0bc lsr  w12,w1,#0               W  BitExtract
+0c4 lsr  w9,w23,#0               W  BitExtract
```

### 3.6 0x4033ec

```text
+000 mov  w6,#0x2c                O  LoadImm
+004 ubfx x7,x29,#0,#8            R  GetHost id88；not-u32(:652-655)，home=x29
+008 uxtb w9,w7                   O  narrow Sub operand normalize
+010 mov  w11,w6                  O  destructive Sub input
```

### 3.7 0x402df8

```text
+000 ubfx x6,x23,#0,#32           R  block 入口 read；无 latest_store，home=x23
+004 mov  w22,w6                  W  ZeroExtend32To64
+008 mov  w29,w0                  R  home=x0 read；无同 block latest_store
+00c lsr  w11,w6,#0               W  BitExtract
+014 lsr  w12,w29,#0              W  BitExtract
+024 mov  w29,w9                  W  ZeroExtend32To64
+030 mov  w22,w7                  W  ZeroExtend32To64
+034 lsr  w7,w22,#0               W  BitExtract
+038 lsr  w9,w29,#0               W  BitExtract
+040 mov  w22,w8                  W  ZeroExtend32To64
+044 lsr  w7,w22,#0               W  BitExtract
+048 mov  w29,w7                  W  ZeroExtend32To64
+04c lsr  w8,w22,#0               W  BitExtract
+054 mov  w22,w9                  W  ZeroExtend32To64
+058 lsr  w7,w29,#0               W  BitExtract
+060 mov  w29,w8                  W  ZeroExtend32To64
+064 lsr  w9,w22,#0               W  BitExtract
+06c lsr  w8,w29,#0               W  BitExtract
+074 lsr  w9,w29,#0               W  BitExtract
+078 lsr  w7,w22,#0               W  BitExtract
+080 mov  w29,w8                  W  ZeroExtend32To64
+084 lsr  w8,w29,#0               W  BitExtract
+08c lsr  w6,w23,#0               W  BitExtract
```

本点 SVM 为 42/23，FEX 当前 SRA 参照为 20/5。机械差是 18 move/entry，即 `18 × 48,600,000 = 874,800,000` 条、**1.486162% host**。这是真实下界，但 21 条 W 中只有 2 条 R；消掉 W 的实现正是已经关闭的通用 width ownership。v2 在生产 region 态本点保持 42/23，不能把块态收益外推到这里。

### 3.8 0x403552

```text
+004 bfxil x26,x6,#0,#8           F  PF
+00c ubfx  x7,x7,#4,#1            F  AF extraction
+010 bfi   x26,x7,#26,#1          F  AF packed write
+018 mov   x29,x6                 R  SetHost id5；observer/home-access 穿窗(:967-990)，home=x29
+02c mov   x11,#0x3383            B  SetLocation
+040 mov   w11,#0x19c1            B  terminal/link tail target
```

### 3.9 0x403320

```text
+008 mov  x29,x6                  R  SetHost id5；not-last-use，home=x29
+00c mov  x22,x0                  R  SetHost id7；producer-not-eligible(:848-854)，home=x22
+014 mov  w23,w7                  R  SetHost id13；not-last-use，home=x23
+018 uxtb w6,w7                   W  BitExtract8
+01c uxtb w8,w7                   W  第二 byte view
+024 mov  w6,w7                   O  byte Or operand copy
+028 sxtb x8,w6                   O  byte Or/flags normalize
```

## 4. 全程序 owner 细分

### 4.1 宽度桥

| IR owner | dynamic | 占 host |
|---|---:|---:|
| BitExtract | 4,688,508,324 | 7.965114% |
| ZeroExtend32To64 | 1,960,377,291 | 3.330404% |
| ZeroExtend32 | 253,965,610 | 0.431452% |
| SignExtend | 69,771,150 | 0.118531% |
| 其余 ZExt64/alias 尾差 | 169 | 0.000000% |

编码形态以 `lsr #0` 2.923B、`uxtb` 1.417B、`mov W` 1.990B、`uxth` 0.573B 为主。FEX 同锚点确实更短，但在当前证明体系内机械可消下界是 **0**：唯一已证实现是 width-chain/v2，而它在 production region 态不保收益且触发 STREAM shape 门。

### 4.2 fixed-home transport / RA 拒绝

| IR owner | dynamic | 占 host |
|---|---:|---:|
| SetHostGPR | 3,723,080,927 | 6.324989% |
| GetHostGPR | 2,138,913,993 | 3.633713% |

这里包括三种本质不同的拒绝：block 入口无 publication history、partial home read/write、producer/target 活跃窗冲突。FEX 用进程级 SRA 家消掉它们；SwiftVM 的 fixed-class 实测是 spill `267×`、move `+4.32%`，UniformLocalSSA 第一门也已 NO-GO。因此“放松某个 guard”的可行机械下界为 **0**；raw 9.96% 不能冒充候选收益。

### 4.3 fixed flags

精确编码：

```text
bfxil x26,... PF       1,153,941,488
ubfx ..., #4,#1 AF       498,768,807
bfi x26,... AF           498,768,799
其他 flags owner                 195
合计                 2,151,479,289 = 3.655059646% host
```

`0x403630/0x403688` 两个同构 root 各有三条 F；仅这两个点的机械差为 `6 × 76,800,000 = 460,800,000`，即 **0.782834% host**。FEX PF/AF 专用 fixed 家与 local flags recipe 能省；SwiftVM 对应是 W-β / lazy flags，均有 ABI 或 STREAM 墙钟罚单，故本轮不立项。

### 4.4 其他

| 子类 | dynamic | 占 host | 说明 |
|---|---:|---:|---|
| LoadImm/VecLoadConst | 1,795,109,110 | 3.049637% | INT_IMM_FOLD 已 ON；剩余不是已证明的同 block single-use encodable consumer |
| 算术 fallback（扣除 PF/AF 链） | 2,478,832,126 | 4.211185913% | 窄 Sub/Or operand normalize、destructive input copy；与 width/flags proof 重叠 |
| EA/memory owner | 187,827,160 | 0.319092% | 低于 0.5%，且 bounded-bias 三项寻址已有 EA_FIXED_REG NO-GO |
| 其余极小 owner | 366 | 近零 | 不成池 |

上表算术百分比按精确条数为 `2,478,832,126 / 58,863,041,847 = 4.211185913%`；正文裁决使用总类的精确 `7.579915380%`，避免把 flags/width 交叉 owner 二次计账。

### 4.5 helper、boundary、FPR

- `CallLambda` move 仅 143 条；caller-save 大洗牌不在 CoreMark 热路径 move 桶。
- terminal：SetLocation 334,967,926；PushRSB 180,034,636；terminal tail 334,967,957。它们是外部边契约，不是 RA copy。
- FPR：SetHostFPR 223 + GetHostFPR 1。CoreMark 完全不是 XMM 残余样本。

## 5. zip7 / sqlite 锚点抽查

这两次运行仅作成因交叉验证，不替代各自 canonical 全矩阵账；均为 region=1、无 JIT cache、无 EXEC_PROF。

### 5.1 zip7

命令 `7zip_x64 b -mmt1 -md=16m`，oracle `Tot:`、rc=0。抽查：

| PC | entries | host/move | owner 静态分解 | 裁决 |
|---|---:|---:|---|---|
| 0x5b28d0 | 本短窗 0 | 143/7 | W1 + R3 + O3 | long-chain 已 ON 后，255 条桥已不存在；残余不是重开理由 |
| 0x41e148 | 2,090 | 148/76 | W23 + R46 + O7 | table loop 的大头是 publish/home；需要 fixed SRA，EA 只有1 |
| 0x42d0b0 | 441,868,158 | 14/3 | EA1 + R2 | byte-copy 锚精确说明 EA 本体不是 move 大头；EA_FIXED_REG 已 NO-GO |
| 0x42d710 | 35,355,227 | 128/47 | W6 + R18 + flags/other23 | predicate/value flags 与 publish 混合；predicate-fuse 审计已 NO-GO |
| 0x42d000 | 43,672,413 | 84/29 | W1 + R11 + flags/other17 | 同上 |

本窗口全程序 owner 也呈同样结构：W 10.192% host、publish 9.936%、home load 9.782%、算术 fallback 3.995%。这不是一个“再加一条 W-α 白名单”可吃的池。

### 5.2 sqlite

`sqlite_speedtest_x64 --size 1 --testset main <fresh.db>` 输出 TOTAL、rc=0。抽查：

| PC | entries | host/move | owner 静态分解 | 裁决 |
|---|---:|---:|---|---|
| 0x4a5040 | 10,467 | 63/17 | W2 + R14 + O1 | cap16 region 内 publish 密集；cycle-close 已审计 NO-GO |
| 0x4a5358 | 341,373 | 24/5 | B2 + O3 | 外部 terminal，不是 RA coalesce 缺口 |
| 0x456645 | 本短窗 0 | 65/26 | W11 + R10 + O5 | narrow rotate 已翻盘后残余是 home/其他 |
| 0x40dd30 | 76,341 | 67/25 | W9 + R13 + O3 | 同构 |
| 0x44f538 | 228 | 27/13 | W5 + R6 + EA2 | 数量和权重均不成立新池 |

## 6. 机械可消下界与候选裁决

“机械可消下界”只在同 guest PC 的 FEX 真码明确更短时计，不把整个 owner 当可删：

| 排名 | 形态 | 已闭合机械差 | 所需载体 | 本轮裁决 / gate |
|---:|---|---:|---|---|
| 1 | 0x402df8 width + home transport | 18×48.6M = 874.8M（1.4862% host） | 多节点 ownership / SRA | **封存**；WIDTH_CHAIN v2 生产 region 态该点 42/23 不变，不设新 gate |
| 2 | 0x403630/688 PF/AF 链 | 6×76.8M = 460.8M（0.7828%） | local lazy flags 或 PF/AF fixed 家 | **封存**；W-β 与 lazy flags 两张罚单，不设新 gate |
| 3 | fixed-home Set/Get 全局 raw | raw 9.9587%，但无独立可行下界 | 固定 SRA / cross-block home SSA | **封存**；fixed-class 与 UniformLocalSSA 均已失败 |
| 4 | terminal | raw 1.4440%，FEX 同样有 dispatcher fallback | 新 signal/fault 边界 ABI | **无可行机制** |
| 5 | EA | raw 0.3191% | bounded-bias 可编码三项 | **低于门且已否** |
| 6 | FPR | CoreMark 224 条；zip/sqlite 抽查 <0.3% | 全 XMM/局部 SSA | **低于门或已否** |

没有新 gate 名可诚实提出。若未来重开，第一门必须是新的正确性载体本身，而不是重新量 move：

1. fixed-home：先证明 fault-safe、无跨 module ABI 的跨 HIR-block home fact，并且 rays 净值重新超过 0.5%；
2. flags：先有不依赖热路径 lazy recipe 的 committed-flags recovery；
3. boundary：先有替代 poll/current-location 的有界 signal observer；
4. width：必须解释为何 production region 内候选资格不再蒸发，并保持 STREAM 公共 PC/bytes 逐字。

在这些前提出现前，**move 残余方向空间按当前证明体系耗尽**。

## 7. 构建与审计产物

```text
cmake --build build -j8: rc=0
新增 warning: 0（仅既有 warning）
CoreMark: rc=0, crcfinal=0x25b5, Correct operation validated
zip7: rc=0, Tot: present
sqlite: rc=0, TOTAL present
生产源码改动: 0
诊断源码改动: 0
```

临时 guard probe 撤销并完成最终重建后，又跑了一次 2,000-iteration shape
capture；top-10 的 `host_static/move_static/unit bytes` 与 probe 前逐点一致：

```text
0x4033bb 23/6/132   0x403630 36/11/184  0x403688 36/11/184
0x4034b0 13/4/92    0x402218 75/38/340  0x402de8 54/31/256
0x4033ec 9/4/76     0x402df8 42/23/208  0x403552 22/6/128
0x403320 13/7/92
```

这证明临时诊断没有遗留发码变化；2,000 次短跑按 CoreMark 规范会因不足
10 秒打印 duration error，仅用于 shape 对照，oracle 采用前述 150,000 次
完整 validated 运行。

本任务没有改生产代码、配置、测试或 golden；因此没有为纯审计重复跑 suite/指纹。所有分析脚本调用均使用仓内现有 `tools/coremark_forensic_analyze.py`，临时 monkey-patch 只在 Python 进程内禁用与本任务无关的旧 immediate-fold 扫描，没有写回文件。
