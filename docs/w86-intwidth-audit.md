# W86：整数宽度 / copy canonicalization 筛选审计

日期：2026-08-01  
基线：`19d2a92f8884dca82ae937fad62a9b9605bc916f`  
测量平台：Orb `ubuntu`，AArch64 Linux

## 1. 结论

本方向通过题设的实现 spike 门，但本任务没有实现优化。

- CoreMark top-10 的 entry-weighted host 分母为 `147,282,900,000` 条；经
  **RA 权威 last-use + 已知 W-write producer** 双重证明，可净删候选为
  `9,331,200,000` 条，即 **6.335562%**。门槛是 5%，通过 1.335562 个百分点。
- 候选只集中在两个 matrix unit：`0x402df8` 每入口 16 条，`0x402d58`
  每入口 8 条。其余八个 top unit 虽有 85 条 move/宽度桥，均没有满足证明的
  候选。
- 全 CoreMark 口径为 `19,235,653,449 / 348,174,711,298 = 5.524713%`，说明
  候选不只存在于 top-10，但 top-10 的集中度更高。
- 七语料外推显示明显的整数负载选择性：7-Zip 为 9.063986%，CoreMark
  5.524713%，c-ray 2.370779%，SQLite 1.643854%，OpenSSL 合并 1.845099%，
  smallpt 0.772228%，STREAM 仅 0.000032%。这不是 W74 的 SSE lane/tied-copy
  重复立项。
- 以上均是“每个证明候选最终净省一条 host 指令”的机械上限，不是墙钟收益。
  建议批准一个默认 OFF 的 `SVM_RA_INTWIDTH_TIE` 实现 spike；是否保留以及是否
  翻默认必须由 CoreMark 交错 A/B 和 RA 边界数据决定。

审计中一度只检查 exact last-use，得到 top-10 7.391489%。复核发现该口径没有
证明跳过 W 写后 X 高半仍为零，过于乐观；最终表增加递归 W-write producer 证明，
结果收紧为 6.335562%。旧值不用于裁定。

## 2. 测量方法与正确性

### 2.1 构建与 CoreMark

```sh
orb -m ubuntu bash -lc '
  cmake -S /private/tmp/w66 -B /tmp/w86-build -DCMAKE_BUILD_TYPE=Release &&
  cmake --build /tmp/w86-build --target svm_translator_linux -j8'
```

CoreMark 的最终严格口径命令为：

```sh
orb -m ubuntu bash -lc '
  gdb -q -batch \
    -ex "set pagination off" \
    -ex "set debuginfod enabled off" \
    -ex "set env SVM_RA_INTWIDTH_AUDIT 1" \
    -ex "set env SVM_RA_HOT_COALESCE /tmp/w86-seven/coremark/hot.txt" \
    -ex "unset env SVM_JIT_CACHE" \
    -ex starti \
    -ex "source /private/tmp/w86-gdb.py" \
    -ex continue \
    --args /tmp/w86-build/source/translator/linux/svm_translator_linux \
      /Users/swift/CLionProjects/SwiftVM-bench/bin/coremark_x64 \
      0x0 0x0 0x66 150000 7 1 2000 \
    >/tmp/w86-seven/coremark/gdb.txt 2>&1'
```

原始正确性输出位于 `/tmp/w86-seven/coremark/gdb.txt`：

```text
seedcrc          : 0xe9f5
[0]crclist       : 0xe714
[0]crcmatrix     : 0x1fd7
[0]crcstate      : 0x8e3a
[0]crcfinal      : 0x25b5
Correct operation validated. See README.md for run and reporting rules.
[w86-all-entry-summary] versions=1700 executed=1700
```

W71 原始聚合位于 `/tmp/w86-seven/coremark/hot.txt`：

```text
[svm-hot-coalesce] units=1700 versions=1700 executed_units=1700 overflow=0
entries=9774084004 host_dynamic=348174711298
spill_reloads=0 spill_writebacks=0 spill_dynamic=0 spill_pct=0.000000
move_dynamic=122217844421 move_pct=35.102447
```

五个 CRC 全部命中已知值并输出 `Correct operation validated`；本报告的所有
CoreMark 表均来自这次正确执行。

### 2.2 临时只读筛选探针

临时探针插在 `LinearScanAllocator` 的现有 memory/narrow ties 之后、普通
`AllocGPR` 之前，只打印候选，随后仍执行原 master 分配；它不转移物理寄存器，
不改变 emitter 或 JIT 指令。退出前已全部撤回。

候选必须同时满足：

1. source 已分配为 GPR，且仍在 allocator 的 active 集；
2. source 的权威 interval `end == current.start`；
3. source/destination 不是同一 SSA value；
4. producer 可递归证明已经产生 W-clean 值：普通 U32 producer 可接受，
   `GetHostGPR`/纯 BitCast 不可接受；identity `BitExtract` 与
   `ZeroExtend32To64` 继续向源追溯；
5. tie 后对应 emitter 有明确 no-op 形态，或 spike 可以用同一寄存器的 W/X
   equality check 把 identity move 变为 no-op。

第 4 条防止把“结果只以 W 读取”误当成“物理 X 高半已经为零”。例如 source
来自 `GetHostGPR(U32)` 时，读取 W view 不代表 pinned X 的高 32 位为零；若随后
`ZeroExtend32To64` 也被 tie 成 no-op，高位会错误泄漏。最终候选因此比单纯
last-use 口径少 4 条 top-10 静态指令。

动态计数使用 W71 的实际 block entry counter。`/private/tmp/w86-gdb.py` 在
`DumpAtExit` 前读取全部 `32768` 个 slot，按 guest PC 聚合
`(entries, host_static)`；最终七语料均为 `versions == executed PC`，没有多版本
形态混算。`/private/tmp/w86-join.py` 对每个 PC 的候选签名去重，再以实际 entries
相乘。探针和 gdb 只存在于 `/private/tmp` 与 Orb `/tmp`，没有进入仓库。

## 3. CoreMark top-10 分类

分类定义：

- `lsr0`：U32 `BitExtract(value, 0, 32)`，AArch64 形态为
  `lsr wD,wS,#0`；source exact-last-use 且已证明 W-clean，tie 后现有
  `EmitBitExtract` 直接返回。
- `ubfx-x32`：`ubfx xD,xS,#0,#32`。top-10 中没有同时满足 last-use 和
  W-clean 的可删项；一般形态仍需清 X 高半，不能仅因 `#0,#32` 就删除。
- `bridge→W`：W identity bridge 后紧接 `ZeroExtend32To64`；链上每一步均有
  W-clean 证明，转移物理寄存器后后一个 W move 可删。
- `tied-dst`：U32 producer 在 `ZeroExtend32To64` 处精确死亡，结果直接继承
  producer 的物理 GPR；producer 本身直接写目标 W 宽度。
- `c`：W71 move/桥总数减上述候选。它包括 source 仍活、未知高半、固定 pin
  copy、flags/partial-register bitfield、dispatch materialization 等，不能由本
  方案删除。

| rank | PC | entries | host | move/桥 | lsr0 | ubfx-x32 | bridge→W | tied-dst | c | 可删动态条数 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | `0x402680` | 887,400,000 | 28 | 9 | 0 | 0 | 0 | 0 | 9 | 0 |
| 2 | `0x402df8` | 388,800,000 | 53 | 30 | 8 | 0 | 1 | 7 | 14 | 6,220,800,000 |
| 3 | `0x402814` | 418,650,000 | 42 | 13 | 0 | 0 | 0 | 0 | 13 | 0 |
| 4 | `0x402668` | 434,250,000 | 40 | 10 | 0 | 0 | 0 | 0 | 10 | 0 |
| 5 | `0x402660` | 433,050,000 | 32 | 8 | 0 | 0 | 0 | 0 | 8 | 0 |
| 6 | `0x402d58` | 388,800,000 | 35 | 17 | 4 | 0 | 0 | 4 | 9 | 3,110,400,000 |
| 7 | `0x402808` | 416,700,000 | 32 | 8 | 0 | 0 | 0 | 0 | 8 | 0 |
| 8 | `0x4033ec` | 248,400,000 | 37 | 12 | 0 | 0 | 0 | 0 | 12 | 0 |
| 9 | `0x403360` | 243,600,000 | 37 | 12 | 0 | 0 | 0 | 0 | 12 | 0 |
| 10 | `0x4033dc` | 196,800,000 | 40 | 13 | 0 | 0 | 0 | 0 | 13 | 0 |
| **合计** | | | **147,282,900,000 dynamic** | **51,305,550,000 dynamic** | **4,665,600,000** | **0** | **388,800,000** | **4,276,800,000** | **41,974,350,000** | **9,331,200,000** |

分项占 top-10 `host_dynamic`：

| 可删类 | 动态条数 | top-10 host 占比 |
|---|---:|---:|
| `lsr0` | 4,665,600,000 | 3.167781% |
| `ubfx-x32` | 0 | 0% |
| `bridge→W` | 388,800,000 | 0.263982% |
| `tied-dst` | 4,276,800,000 | 2.903799% |
| **合计** | **9,331,200,000** | **6.335562%** |

候选占 top-10 move/桥动态账号的 18.187506%。`0x402df8` 的 16 条候选占该
unit 全静态 host 的 30.19%、move/桥的 53.33%；`0x402d58` 的 8 条分别占
22.86% 与 47.06%。这仍是机械上限。

最终候选原文位于 `/tmp/w86-seven/coremark/gdb.txt`。例如：

```text
[w86-intwidth] pc=0x402df8 kind=tied_dest_w32 def=10 source=9 start=10 end=76 src_reg=6
[w86-intwidth] pc=0x402df8 kind=wmov_after_bridge def=40 source=39 start=40 end=47 src_reg=7
[w86-intwidth] pc=0x402df8 kind=lsr0_w32 def=76 source=10 start=76 end=78 src_reg=11
[w86-intwidth] pc=0x402d58 kind=tied_dest_w32 def=10 source=9 start=10 end=45 src_reg=6
[w86-intwidth] pc=0x402d58 kind=lsr0_w32 def=45 source=10 start=45 end=47 src_reg=11
```

所有 top-2 候选的物理 source 都在默认 level-2 动态池 `x6-x11`，不是固定 pin
目标；因此筛选结果没有把 constrained fixed-register copy 偷算成普通 RA tie。

## 4. 机制溯源

### 4.1 前端为何产生这些节点

`source/runtime/frontend/x86/decoder.cc`：

- `X64Decoder::NarrowTo` 用 `ZeroExtend32` 表达小于 64 位的 W-normalize；
- `X64Decoder::R(reg, value)` 对 x86-64 的 32 位 GPR 写必须把完整 64 位架构寄存器
  更新为零扩展值，因此产生 `ZeroExtend32To64(value)`，再做全宽
  `StoreUniform`；pin/UniformElimination 后对应 `SetHostGPR`；
- `GprZextCoalesceEnabled` 是 W19 的开关，只把旧的
  `ZeroExtend64(ZeroExtend32(v))` 两节点折成一个 `ZeroExtend32To64(v)`。

x86 的“写 EAX 清 RAX 高半”语义不能从 IR 删除；问题是它当前成为独立 SSA
interval，RA 没有把 producer 的物理 W 寄存器所有权转交给这个语义节点。

### 4.2 RA 为何没有自然吃掉

`source/runtime/ir/opts/register_alloc_pass.cpp` 的
`LinearScanAllocator::TryTieGPR` 已有正确的权威条件：source 必须仍 active，且
`source.live.end == current.start`，成功后从 active 集移除 source，并把同一物理
GPR 映射给 destination。

但 master 只从已经批准的 `TryTieMemoryOperand`、`TryTieNarrowLoad` 等专门路径
调用它，没有整数 width-chain 的候选选择器。普通 `AllocGPR` 不能复用仍处于 active
集的 source；于是语义上可继承的结果被分到另一个寄存器，backend 只能发 copy。

`FusePinnedWriteChains` 处理的是 `ZeroExtend32To64 -> SetHostGPR(pin)` 的固定目标
链，并有 full static exchange 等整块保守屏障。这里的主要候选是动态 RA interval
之间的 transfer，且常在算术、BitExtract 和多个中间 use 之间，不属于固定 pin
融合的覆盖范围。

### 4.3 backend 形态

`source/runtime/backend/arm64/jit/translator_alu.cpp`：

- `EmitBitExtract` 已在 `left=0`、bits 等于 result 宽度且
  `context.SharesGPR(source,result)` 时不发指令；否则 VIXL 的 `Ubfx` 对 U32
  形态反汇编为 `lsr wD,wS,#0`；
- `EmitZeroExtend32` 的 32 位 default 分支在 W source/destination 不同时发
  `Mov`；
- `EmitZeroExtend32To64` 已在 U32 source 与 result 共用 GPR 时直接返回；
- `EmitZeroExtend64` 的 U32 source 分支目前无条件发 `Mov(result.W(),source.W())`，
  spike 若纳入该小类，需要增加与前两者同样的 equality no-op；
- `source/runtime/backend/arm64/jit/translator_mem.cpp::EmitSetHostGPR` 在 value
  不在固定 pin 时还会发最终 W/X copy。固定目标约束不是本轮普通 tie 的对象。

`0x402df8` 的 post-uniform IR 原文在
`/tmp/w86-core-dump/stderr.txt:80166` 附近，可见连续：

```text
@39 U32 = BitExtract (U64) @37, #0, #32
...
@56 S32 = ZeroExtend32To64 (S32) @47
...
@61 U32 = BitExtract (U64) @56, #0, #32
@62 U32 = ZeroExtend32To64 (U32) @61
...
@75 U32 = ZeroExtend32To64 (U32) @65
```

这也解释了 W19 为什么没有吃掉残余：W19 已经减少一个 IR 节点和一条旧 copy，
但保留的单个 `ZeroExtend32To64` 仍是独立 SSA definition；只有 RA ownership
transfer 才能让现有 `SharesGPR` 快路径把最后一条 move 变为零条。

## 5. 七语料外推

除 CoreMark 外的调用与 W67 口径一致；c-ray 采用 W71/W74 的
`-s 8 -j 1 -d 160x120`，OpenSSL 的 SHA-256 与 AES-128-GCM 各 8 秒后按动态
分母合并。统一执行器为：

```sh
/private/tmp/w86-run-one.sh NAME GUEST [guest args...]
```

该脚本设置严格临时探针和 `SVM_RA_HOT_COALESCE`，在 gdb 下运行并调用
`/private/tmp/w86-join.py`。原始证据目录均为 `/tmp/w86-seven/NAME/`，包括
`gdb.txt` 与 `hot.txt`。具体 guest 参数：

```text
stream:  stream_x64
smallpt: smallpt_wh_x64 64 320 240
sqlite:  sqlite_speedtest_x64 --size 100 --testset main,orm/25,cte/20,json,fp/3,parsenumber/25,star,app /tmp/w86-seven/sqlite/test.db
c-ray:   cray_x64 scene.json -j 1 -s 8 -d 160x120 -o /tmp/w86-seven/cray/out.png --no-sdl
7zip:    7zip_x64 b -mmt1 -md=16m
openssl: openssl_x64 speed -seconds 8 -evp sha256 / aes-128-gcm
```

| 语料 | host_dynamic | 严格候选动态 | lsr0 | tied-dst | bridge→W | W/X identity | 候选占 host |
|---|---:|---:|---:|---:|---:|---:|---:|
| coremark | 348,174,711,298 | 19,235,653,449 | 2.331378% | 3.059264% | 0.134071% | ~0% | **5.524713%** |
| stream | 68,951,537,715 | 22,364 | 0.000007% | 0.000025% | ~0% | ~0% | **0.000032%** |
| smallpt | 273,513,620,243 | 2,112,148,813 | 0.264108% | 0.474588% | 0.033419% | 0.000113% | **0.772228%** |
| sqlite | 186,476,849,008 | 3,065,407,319 | 0.512453% | 1.069792% | 0.028674% | 0.032935% | **1.643854%** |
| c-ray | 26,826,994,340 | 636,008,794 | 0.405432% | 0.557936% | 0.082680% | 1.324731% | **2.370779%** |
| 7zip | 509,444,250,889 | 46,175,955,450 | 3.543023% | 5.066887% | 0.428664% | 0.025412% | **9.063986%** |
| openssl-speed | 669,554,072,246 | 12,353,938,311 | 0.536150% | 1.260677% | 0.048271% | 0.000002% | **1.845099%** |

`W/X identity` 是 `ZeroExtend32/ZeroExtend64` 一类，只有在 source 已 W-clean、
exact last-use 成立，并在 emitter 加 equality no-op 后才计。它在 c-ray 较大，正是
W74 已审计过的宽度 identity 面；本项目的立项门只看 CoreMark top-10，不借这列
替 CoreMark 过门。

外推结论：整数密集的 CoreMark/7-Zip/AES 路径最可能受益；FP/访存型 STREAM
几乎完全无关。OpenSSL 合并行被 SHA（0.761249%）稀释，AES-GCM 单项为
7.269378%，也支持“整数/bitfield 链选择性”，而不是通用 copy elimination。

## 6. Gate 裁定与实现 spike 方案

### 6.1 裁定

题设要求 CoreMark top-10 动态可删子集至少 5%。严格结果为 6.335562%，故：

- **批准实现 spike**；
- 本任务不新增开关、不改 production source、不做性能 A/B；
- 不把 6.34% 写成预期墙钟加速。依赖链、前端吞吐、代码布局和 I-cache 都会让
  wall ratio 与机械条数不同。

### 6.2 推荐实现形态

建议开关名 `SVM_RA_INTWIDTH_TIE`，默认 OFF。实现时登记 env；本次审计不登记、
不改变当前 `kGetenvNames`。

在 `LinearScanAllocator` 内新增 `IntWidthTieSource(inst)`，只返回下列 source：

1. U32 `BitExtract(value,0,32)`，并通过递归 W-clean producer 证明；
2. `ZeroExtend32To64(U32)`，source 是真实 W-writing producer 或已证明的安全
   identity 链；
3. 可选的 `ZeroExtend32/ZeroExtend64(U32)` identity，小类必须同时补 emitter
   equality no-op；
4. 暂不接纳一般 `ubfx xD,xS,#0,#32`。只有能证明 source 高半已零、result 的
   X 语义与省略完全等价时才可另行加入；top-10 当前为零，不影响门。

候选选择后必须调用现有 `TryTieGPR` 完成所有权转移，不另写近似 last-use：

```text
source is active GPR
source.id != destination.id
source.live.end == destination.live.start
remove source from active set
MapRegister(destination, source physical GPR)
```

proof 失败时无条件回到原 `AllocGPR/SpillAtInterval` 形态。开关 OFF 不调用新
selector，保证逐位回退。固定 pin、MEM spill source、helper ABI target、部分寄存器
写和 source 仍活的情况全部排除。

这类 transfer 不增加 live range，只把恰好死亡的物理寄存器交给当前 interval，
理论上不应增大 pool pressure；仍需用 `SVM_RA_SHAPE_PROF` 实测确认 spill defs/
loads/stores、high-water、scratch reserve 与 helper snapshot 都不增长。

### 6.3 实现版建议验收门

- OFF 函数指纹与 `/tmp/svm-build` 零 diff，含 `host_bytes`；
- ON 自一致，且只允许证明候选 unit 的 host bytes 下降；
- CoreMark 150k 五 CRC 全对，func_tests 六格 checksum
  `9f52b7d59285dbe5`，mac/orb 全量两态；
- 定向矩阵：producer 为 GetHostGPR/Load/ALU/shift/BitExtract × consumer 以
  W/X 读取 × upper32 为全零/全一/随机，特别覆盖连续两层 tie，防高半泄漏；
- pin level 0/1/2/3、spill unit、helper/fault 边界均覆盖；
- CoreMark ON/OFF 至少 7 对，输出 95% CI；同时比较 W71 move_dynamic 与
  RA_SHAPE_PROF 边界。只有 CI 为正且无 RA 边界回归才讨论保留 ON 路径；默认翻转
  另需独立数据门。

## 7. 仓库纪律

临时 probe 在报告完成前已用逐段 `apply_patch` 撤回，没有使用
`git checkout/reset/stash`。最终工作树只新增：

- `docs/w86-intwidth-audit.md`

没有改动 production source、tests、golden、`source/translator/linux/linker/` 或
`SwiftVM-bench/harness/run_matrix.sh`，也没有执行 git commit/add/push。
