# W74：move/tied-copy 消除筛选报告

日期：2026-08-01

基线：`23c9b742022474f5dbdc4b16e4ffd3434fc352a1`（`w74-tiedcopy`）

## 结论

本项在候选筛选门关闭，**没有实现 `SVM_RA_TIED_COPY`**。

Orb Linux c-ray `-s 8 -j 1 -d 160x120` 的 W71 同口径结果为：

| 分类 | entry-weighted 动态条数 | 占全部 host 指令 | 占 move/桥 |
|---|---:|---:|---:|
| a. RA 权威 last-use 可证明的 tied-destination | 260,201 | 0.000970% | 0.003125% |
| b. RA 权威 last-use 可证明的零宽/identity bridge | 860,249,941 | 3.206644% | 10.332292% |
| **a+b 可删候选** | **860,510,142** | **3.207614%** | **10.335417%** |
| c. 其余语义/固定寄存器/lane 搬运 | 7,465,328,289 | 27.827550% | 89.664583% |
| W71 move/桥总计 | 8,325,838,431 | 31.035164% | 100% |

`a+b=3.207614% < 5%`，未通过题设的实现门。这个数还是“每个候选最终都净省
一条 host 指令”的机械上限，不是 wall 收益预估；因此没有理由进入高风险的分配
变更、全量矩阵和七对性能 A/B。临时只读筛选计数已撤回，最终工作树只新增本文档。

为避免漏掉题设点名的 `fmov` 到 GPR，本轮另做了更宽的反证：把热主线所有
`fmov/umov/smov`（包括语义必需项）一律假定可删，得到 204,338,273 条、
`0.761689% host_dynamic`。它与上述 GPR 候选不重叠；两者相加也只有
**3.969303%**，仍低于 5%（还差 1.030697 个百分点）。因此关闭结论不依赖于
逐一证明每条 cross-bank move 不可删。

另一个关键结果是 `0x402e70`：它虽贡献几乎全部动态 spill，但在 W65 的 x18
按 unit 条件保留后的**最终分配**里只有 1 条 b 类候选、0 条 a 类候选。也就是说，
本轮没有可捆绑的“spill + tied-copy”面积。

## 1. 筛选方法

### 1.1 动态分母

沿用 W71 `SVM_RA_HOT_COALESCE`：每个 unit 的 entry counter 乘静态热主线指令数；
探针、NaN cold stub 与 terminal 后冷区不进 `host_dynamic`。本次原文首行位于 Orb：

`/tmp/w74-screen-final/hot.count`

```text
[svm-hot-coalesce] units=8283 versions=8283 executed_units=8283 overflow=0 entries=341791167 host_dynamic=26827112965 spill_reloads=265551874 spill_writebacks=222927996 spill_dynamic=488479870 spill_pct=1.820844 move_dynamic=8325838431 move_pct=31.035164 tied_copy_dynamic=260201 tied_copy_pct=0.000970 zero_bridge_dynamic=860249941 zero_bridge_pct=3.206644 ...
```

图片 IDAT MD5 为 `f9afd5bed804b5c2654303c9252d09c4`，与未加筛选计数的
同参数基线一致。

cross-bank 极宽上界原文在 `/tmp/w74-cross/hot.count`：

```text
[svm-hot-coalesce] ... host_dynamic=26826981582 ... move_dynamic=8325802160 move_pct=31.035181 fmov_dynamic=204338273 fmov_pct=0.761689 ...
```

这里的临时 `fmov_dynamic` 识别 VIXL mnemonic `fmov/umov/smov`，没有做
def-use 筛选，故只能抬高上界、不会漏算成乐观结果。

### 1.2 last-use 判据

筛选临时复用了 `LinearScanAllocator::TryTieGPR` 的权威条件，没有从相邻 IR、
`Inst::GetUses()` 或 emitter 做近似：

```text
source interval is still active
source allocation is GPR
source.id != destination.id
source.live.end == destination.live.start
```

检查发生在 `ExpireOldIntervals(current)` 之后、普通 `AllocGPR(current)` 之前。
筛选只计数，仍调用原来的 `AllocGPR`，不转移寄存器、不改 dirty mask、不改 emitter。
现有 `TryTieMemoryOperand` / `TryTieNarrowLoad` 先执行，故本表只计尚未被 master
消掉的差集。

spill unit 会因 W65 为 x18 重跑 RA。计数在重跑前清零，只记录**最终发射那一遍**；
否则 `0x402e70` 的首遍动态池形态会把不可发射的候选重复算入。

### 1.3 分类边界

a 类只接受具备 destructive/copy emitter、且 tie 后确实能删除一条 copy 的 GPR
结果：`BitClear`、`BitInsert`、`LocalParitySet`、`GetResult` 与 ordered
`FCmpCondSet(VC)`。

b 类只接受 identity/宽度链，且同样通过上述 last-use：单 value `GetOperand`、
`BitExtract(value, 0, full_width)`、可成为同一 W/X 寄存器的 `ZeroExtend32`、
`ZeroExtend32To64`、`ZeroExtend64`，以及 64-bit identity `SignExtend`。

以下都归 c，而没有借 31.04% 的宽松口径冒充可删：

- SSE 标量的“复制旧 128-bit destination + 插入 lane 0”；若 source 真在这里死亡，
  W23 `TryTieScalarInsert` 已经先行消除，当前残余必须保留其他 lanes。
- `fmov`/`umov`/`ins` 等 GPR/FPR 或 lane 搬运；两个寄存器 bank 不能通过 GPR
  interval 所有权转移折叠。作为保守检查，所有 `fmov/umov/smov` 已整体加入上界，
  仍不够 5%；`ins` 残余则必须保留可观察的其他 lanes。
- pin `SetHostGPR`、state 写回与 helper/terminal 固定寄存器搬运；目标不在动态
  RA 池，把 producer 约束到固定 pin 是另一项 constrained-allocation 设计。
- 实际 bitfield、符号扩展、`BFI/BFXIL` 部分写与 NaN guard result merge。
- immediate materialization、dispatch/link 地址构造。

## 2. 热 unit 结果

下表的 `c = move - a - b`。静态条数均为每次 entry 的热主线条数；动态排序与
W71 一致。

| rank | guest PC | entries | host | move/桥 | a | b | c | b 动态条数 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | `0x402fed` | 11,723,259 | 230 | 82 | 0 | 12 | 70 | 140,679,108 |
| 2 | `0x403088` | 8,524,283 | 215 | 66 | 0 | 6 | 60 | 51,145,698 |
| 3 | `0x407450` | 3,486,673 | 473 | 157 | 0 | 19 | 138 | 66,246,787 |
| 4 | `0x402e70` | 3,278,291 | 375 | 79 | 0 | 1 | 78 | 3,278,291 |
| 5 | `0x457ad0` | 4,968,099 | 174 | 34 | 0 | 4 | 30 | 19,872,396 |
| 6 | `0x403110` | 8,567,828 | 88 | 30 | 0 | 0 | 30 | 0 |
| 7 | `0x40308c` | 3,198,976 | 214 | 66 | 0 | 6 | 60 | 19,193,856 |
| 8 | `0x457b70` | 3,562,209 | 160 | 33 | 0 | 5 | 28 | 17,811,045 |
| 9 | `0x402fc0` | 11,779,907 | 42 | 18 | 0 | 2 | 16 | 23,559,814 |
| 10 | `0x402f62` | 3,278,288 | 134 | 54 | 0 | 9 | 45 | 29,504,592 |
| 11 | `0x407a40` | 1,512,697 | 240 | 78 | 0 | 8 | 70 | 12,101,576 |
| 12 | `0x41e8f0` | 5,086,142 | 70 | 37 | 0 | 12 | 25 | 61,033,704 |

最热的 20 个 unit 全部 `a=0`；a 类的 260,201 次动态实例全部落在长尾，已小到
无法影响决策。

### 2.1 dump 交叉核对

`0x402fed` 的 post-uniform IR 与 host 位于：

- `/tmp/w74-dump/all.err:519921` 起：IR；
- `/tmp/w74-dump/all.err:520124`：完整 host bytes；
- `/tmp/w74-402fed.asm`：GNU objdump 反汇编。

其前 230 条是 W71 口径热主线。代表形状包括：

```text
lsr  w11, w9, #0             identity width bridge（仅 last-use 成立才计 b）
mov  v2.16b, v0.16b
mov  v2.s[0], v3.s[0]        scalar lane merge（c，旧高 lanes 可观察）
mov  x6, v3.d[0]
fmov s1, w6                  cross-bank transfer（c）
bfi/bfxil ...                flags/partial-register write（c）
mov/movk x11, ...            terminal dispatch materialization（c）
```

`0x402e70` 的对应证据为 `/tmp/w74-dump/all.err:519129` 与
`/tmp/w74-402e70.asm`。其 149 条 spill access 与 79 条 move/桥交织，但最终只有
1 条满足 b 类的 GPR source allocation + exact last-use；其余 source 多为 MEM spill
或仍活跃，不能用物理寄存器所有权转移消除。

## 3. Gate 决断

实现门要求 `a+b >= 5% host_dynamic`。实测为 `3.207614%`，差 1.792386 个百分点；
并且这是 100% 精确候选都净省一条的上限。即使再把全部 cross-bank move 当成
免费可删，极宽上限也仅 `3.969303%`。故：

- 不新增 `SVM_RA_TIED_COPY`，`kGetenvNames` 保持 56；
- 不改 register allocator、emitter、测试或 golden；
- 不进行 c-ray 七对 A/B、双平台全量、func_tests 六格与 ON/OFF RA shape 边界，
  因为不存在 ON 路径；
- W71 的 31.04% 不能转化为本路线的预期收益。当前够资格的精确子集只有 3.21%，
  建议关闭 W74，而不是放宽 last-use 或把固定 pin/lane merge 混入同一实现。

## 4. 只读性与工作树验证

临时筛选二进制在 probe OFF 时与 `/tmp/svm-build` 做函数指纹：

```text
self-consistency: OK (4400 function units over 11 guests, host_bytes included)
fingerprint: OK (matches /tmp/svm-build/source/translator/linux/svm_translator_linux)
```

运行命令：

```sh
orb -m wine-ci bash -lc \
  '/private/tmp/w65/source/translator/linux/tests/run_func_fingerprint_tests.sh \
   /tmp/w74-build/source/translator/linux/svm_translator_linux \
   --against /tmp/svm-build/source/translator/linux/svm_translator_linux'
```

筛选完成后已用 `apply_patch` 撤回所有临时计数代码。最终 `git diff --check` 通过，
`git status --short` 只有 `docs/w74-tiedcopy-spike.md`。

## 5. 复现命令

构建与 c-ray：

```sh
orb -m wine-ci bash -lc '
  cmake -S /private/tmp/w65 -B /tmp/w74-build -DCMAKE_BUILD_TYPE=Release &&
  cmake --build /tmp/w74-build --target svm_translator_linux -j8 &&
  rm -rf /tmp/w74-screen-final && mkdir -p /tmp/w74-screen-final &&
  cd /tmp/w74-screen-final &&
  env -u SVM_JIT_CACHE \
    SVM_RA_HOT_COALESCE=/tmp/w74-screen-final/hot.count \
    SVM_RA_SHAPE_PROF=/tmp/w74-screen-final/shape.count \
    /tmp/w74-build/source/translator/linux/svm_translator_linux \
    /Users/swift/CLionProjects/SwiftVM-bench/bin/cray_x64 \
    /Users/swift/CLionProjects/SwiftVM-bench/src/c-ray/input/scene.json \
    -j 1 -s 8 -d 160x120 -o out.png --no-sdl'
```

IR/host dump（`SVM_VIXL_HOST_DUMP` 输出到 stderr）：

```sh
orb -m wine-ci bash -lc '
  rm -rf /tmp/w74-dump && mkdir -p /tmp/w74-dump && cd /tmp/w74-dump &&
  env -u SVM_JIT_CACHE SVM_DUMP_IR=1 SVM_DUMP_IR_POST=1 \
    SVM_VIXL_HOST_DUMP=1 \
    /tmp/w74-build/source/translator/linux/svm_translator_linux \
    /Users/swift/CLionProjects/SwiftVM-bench/bin/cray_x64 \
    /Users/swift/CLionProjects/SwiftVM-bench/src/c-ray/input/scene.json \
    -j 1 -s 1 -d 64x48 -o out.png --no-sdl \
    >all.log 2>all.err'
```

抽取并反汇编单个 unit：

```sh
grep -m1 '\[svm-host\] pc=0x402fed ' /tmp/w74-dump/all.err |
  sed 's/.* bytes=//' | xxd -r -p > /tmp/w74-402fed.bin
objdump -D -b binary -m aarch64 /tmp/w74-402fed.bin > /tmp/w74-402fed.asm
```
