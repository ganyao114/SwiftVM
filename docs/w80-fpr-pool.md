# W80：XMM_STATIC 下 FPR 池 12→16

日期：2026-08-01

基线：`4971324`

开关：`SVM_XMM_POOL_EXT`，默认 OFF；仅在 `SVM_XMM_STATIC=1` 且 JIT、
UniformElimination 有效时生效。

## 结论

实现与正确性门均通过。`SVM_XMM_STATIC=1` 时，动态 FPR 池从
`v0-v10,v15`（12 个）恢复为 `v0-v15`（16 个）。构造的 14 个 V128
同时 live 的压力 unit 中，spill 从 16 降到 8；pool12/pool16 的 JIT
执行结果均为四 lane `105.0f`。STREAM 实跑也确认每个 unit 的池从 12
变成 16，但其热 unit 最大 FPR live 只有 2，两态都是零 spill。

性能结论是：**+4 headroom 没有翻转 W13 的 STREAM 负账。** 七对交错中，
Scale/Add/Triad 的中位吞吐只改善 `+0.16%/+0.23%/+0.19%`；当前同机默认态
诊断对照下，pool16 仍分别为 `-31.45%/-25.03%/-11.09%`。CoreMark 三对
中位 `+0.22%`，也是噪声级。没有统计显著负项，但也没有达到足以改变
XMM_STATIC 决策的收益，因此两个开关都继续默认 OFF。

## 1. 寄存器账目与实现

### 1.1 W67 P3 账目复核

- 静态驻留只有 XMM0-15 → `v16-v31`；AVX 高 128-bit 仍在
  `ThreadContext64::ymm_high`。
- W37 精确 NaN cold ABI 原先全程保留 `v11-v14`：`v11` 左源、`v12`
  右源、`v13` 硬件结果/修正结果、`v14` ordered mask；`x13` 保存 cold
  handler continuation。
- 所以旧动态池不是完整 `v0-v15`，而是 `v0-v10,v15`，精确为 12；本次
  释放 `v11-v14` 后为 `v0-v15`，精确为 16。

这与 W66 第 6.2 节和 W67 第 4.2 节的 P3 账目一致。

### 1.2 生效条件与 OFF 契约

`SVM_XMM_POOL_EXT` 加入 getenv 计时注册表，`kGetenvNames` 由 56 增至 57。
只有下式成立时才设置新的 `Optimizations::XmmPoolExt`：

```text
enable_jit && enable_uniform_elim && SVM_XMM_STATIC != 0 &&
SVM_XMM_POOL_EXT != 0
```

因此默认 OFF 和 `POOL_EXT=1,XMM_STATIC=0` 都不会改变 Config、JIT cache key、
trampoline mask 或发码。OFF 对 master 的 4,400-unit 原始指纹（保留
`host=`）逐行零 diff，总 host bytes 都是 713,784。

### 1.3 cold-edge ABI

热路径不再固定写 v11-v14：

- packed 32/64-bit NaN ordered mask 改从该指令的普通 FPR scratch 租用；
- result/source alias 的保护副本也改为普通 scratch；
- `SVM_SSE_NAN_FAST=1` 路径不进入 cold ABI，保持原行为。

只有 NaN 分支真正跳入 site veneer 后才执行：

1. 在 16-byte 对齐的 host stack 上分配 64B，保存 q11-q14；
2. 若 source 本身落在 v11-v14，从刚保存的 stack slot 装入固定 ABI，避免
   寄存器置换环；否则直接搬入 ABI 寄存器；
3. 沿用共享修正 handler 和 x13 continuation；
4. 先把修正后的 q13 复制到 site result，再恢复除 result 物理寄存器外的
   所有 q11-q14，回收 64B，跳回 continuation。

host stack 是线程私有、与 guest XMM state 和 RA spill-slot coloring 独立的。
fast edge 没有新增 save/restore；代价只在 NaN cold edge。

## 2. 边界不变量核对

| 边界 | 核对结果 |
|---|---|
| runtime entry/exit | 静态 descriptor 仍仅覆盖 v16-v31；八对 LDP/STP 与 `ThreadContext64::xmms` 布局未改。v11-v14 是普通 SSA，不是 guest architectural XMM。 |
| XSAVE/XSAVEC | `EmitHostCall` 在 helper 前仍由 `SpillStaticFPRUniforms()` 把 v16-v31 同步到 xmms[]；动态 v11-v14 与该协议无关。 |
| XRSTOR | helper 前同步 v16-v31，helper 后仍由 `RestoreStaticFPRUniforms()` 重载 v16-v31；五 seed 的 XSAVE 定向均通过。 |
| interpreter | `enable_xmm_static` 要求 JIT；`SVM_ENABLE_JIT=0` 下 POOL_EXT 惰性。func_tests 解释器格和 SSE JIT/interpreter 差分均通过。 |
| 普通 host call | `EmitHostCall` 使用 RegAlloc 的 live-FPR mask，按完整 Q 寄存器保存/恢复；分配到 v11-v14 的 continuation-live V128 自动进入 snapshot，不能依赖 AAPCS 只保低 64-bit。 |
| W57 fault sink | pass 在 guest memory、helper、控制流等观察/故障点前 flush pending architectural XMM，块尾 flush_all；这仍只描述 guest state。v11-v14 的动态中间值遵守既有 RA live/fault 边界，不扩大可延迟范围。 |
| signal/fault | 外部 signal 在 unit 边界观察已物化 guest context；cold veneer 内无 guest fault 点，host stack save/restore 可在 signal return 后继续。普通 guest fault、SSE fault/NaN fuzz 和全量套件均通过。 |
| exact NaN | source=result alias、scalar/packed 32/64、多个 site 共享 handler 均走同一保存规则；NaN 压力 guest 的 JIT/解释器和两开关输出一致。 |

额外运行 helper-fault 套件时为 37/38：唯一失败
`fld_m80[SVM_X87_JIT=1]` 是 opcode 7 的既有 GPR scratch-budget
`declared 6, asked for 7`。master 与 W80、XMM_STATIC 0/1、POOL_EXT 0/1
各重复 5 次都同样 rc=134，故不是本次 FPR 变更；其余 37 格通过。

## 3. 正确性与验收门

### 3.1 FPR 高压定向

现有 scratch-saturation case 新增一个 14×V128 live reduction：

```text
LoadUniform V128 × 14
VecFAdd reduction × 13
StoreUniform result
```

- 显式旧池 `v0-v10,v15`：spill 16；
- 新池 `v0-v15`：spill 8；
- 通过真实 XMM_STATIC trampoline 分别执行 pool12/pool16 代码，两态结果均为
  `[105,105,105,105]`；
- 测试使用 backend hard assertions，不增加 Catch case/assertion 计数，所以全量
  基线仍精确为 139,975/124 与 142,585/124。

复现：

```sh
/tmp/w80-build/source/tests/swift_test 'Scratch pool survives*'
```

macOS 为 763 assertions/1 case，Orb 因既有 Linux 条件断言为 779/1。

### 3.2 全部门

| 门 | 结果 |
|---|---|
| macOS `swift_test` OFF | PASS：139,975 assertions / 124 cases |
| macOS `swift_test` XMM_STATIC+POOL_EXT | PASS：139,975 / 124 |
| Orb Linux `swift_test` OFF | PASS：142,585 / 124 |
| Orb Linux `swift_test` XMM_STATIC+POOL_EXT | PASS：142,585 / 124 |
| OFF 指纹 vs `/tmp/svm-build` | PASS：4,400 units；IR/totals 零 diff；额外保留 host 字段逐行零 diff，713,784 bytes |
| `POOL_EXT=1,XMM_STATIC=0` | PASS：对 master 零 diff，证明 modifier 惰性 |
| ON 指纹自一致 | PASS：4,400 units，两遍含 host_bytes 逐行一致 |
| func_tests 六格 | function/block/interpreter × pool12/pool16 全部 rc=101，checksum `9f52b7d59285dbe5`，stdout SHA-256 `9c3194ff498da03869bbabbd81241fd2ec771619281f969c4b4edc55577a6810` |
| ON 定向/fuzz | 五 seed（101/202/303/404/505）的 SSE float edge、SSE batch B JIT/interpreter、XSAVE 全过；`vec_float_nan_pressure` 两池 × JIT/interpreter 均 rc=0、stdout hash 一致 |
| STREAM oracle | 全部 19 次均 `Solution Validates` |
| CoreMark oracle | 六次均 `Correct operation validated`，标准五项 CRC 一致 |
| `git diff --check` | PASS |

Orb 构建目录为 `/tmp/w80-build`；未触碰 `/tmp/svm-build`。

指纹正式门命令：

```sh
FP=/private/tmp/w64/source/translator/linux/tests/run_func_fingerprint_tests.sh
NEW=/tmp/w80-build/source/translator/linux/svm_translator_linux
REF=/tmp/svm-build/source/translator/linux/svm_translator_linux

"$FP" "$NEW" --against "$REF"
SVM_XMM_STATIC=1 SVM_XMM_POOL_EXT=1 "$FP" "$NEW" --against "$NEW"
SVM_XMM_POOL_EXT=1 "$FP" "$NEW" --against "$REF"
```

### 3.3 4,400-unit ON 静态净账

在同一 W80 二进制上比较 `XMM_STATIC=1,POOL_EXT=0/1`，unit PC/IR 集相同：

| 项 | pool12 | pool16 | 差值 |
|---|---:|---:|---:|
| function units | 4,400 | 4,400 | 0 |
| host bytes | 716,948 | 719,156 | +2,208（+0.308%） |
| unit 分布 |  |  | 44 减 / 3 增 / 4,353 不变 |

增加几乎全在故意的 NaN cold corpus：`vec_float_nan_pressure@0x400078`
因每个 cold site 加 q11-q14 保存/恢复，单 unit `13,572→16,676`（+3,104B）；
这是冷代码尺寸，不是非 NaN fast-edge 动态指令。其余净减少来自动态池改变 helper
live snapshot/RA 形状。

## 4. STREAM 与 CoreMark A/B

口径：Orb Ubuntu，禁用 disk JIT cache；两侧都设 `SVM_XMM_STATIC=1`，只切换
`SVM_XMM_POOL_EXT=0/1`；STREAM 七对、CoreMark 150k 三对，奇数对 pool12→pool16、
偶数对反向。计时窗口 `/proc/loadavg` 约 1.2-1.4。STREAM 使用程序报告的
Best Rate MB/s，CoreMark 使用 Iterations/Sec。

### 4.1 STREAM 七对

| kernel | pool12 median | pool16 median | pool16/pool12 逐对 | ratio median | 符号 |
|---|---:|---:|---|---:|---|
| Copy | 78,768.6 | 80,331.4 | 1.02589, 1.00398, 1.02081, 1.05444, 1.01038, 1.00452, 1.00255 | **1.01038** | +++++++ |
| Scale | 18,891.8 | 18,981.6 | 0.99651, 1.00475, 1.00183, 1.00495, 0.99427, 1.00162, 1.00006 | **1.00162** | -+++-++ |
| Add | 28,256.0 | 28,312.8 | 1.01693, 1.00703, 1.00050, 1.00715, 0.99015, 1.00229, 0.99871 | **1.00229** | ++++-+- |
| Triad | 28,236.9 | 28,340.3 | 1.01735, 1.00422, 1.00106, 1.00192, 0.99938, 1.00280, 1.00044 | **1.00192** | ++++-++ |

配对 ratio 的 100k bootstrap median 95% 区间分别为：Copy
`[1.00398,1.02589]`、Scale `[0.99651,1.00475]`、Add
`[0.99871,1.00715]`、Triad `[1.00044,1.00422]`。双侧 sign-test p 值分别
为 0.015625/0.453125/0.453125/0.125；没有负向显著项。Copy 的正向需要更长
独立复验后才能视为收益，三项 FP 算术仍只有约 0.2%。

RA_SHAPE 现场：

```text
pool12: fpr_pool bins=12:1428, max_live_fpr=0:1351,1:53,2:24,
        spill_units=0, spill_defs/loads/stores=0/0/0
pool16: fpr_pool bins=16:1434, max_live_fpr=0:1357,1:53,2:24,
        spill_units=0, spill_defs/loads/stores=0/0/0
```

两次进程的 late-loader unit 数有 6 个波动，但各自所有 unit 都精确使用预期池，
热形状和零 spill 结论一致。

### 4.2 能否翻转 W13 负账

额外跑三次当前默认态（XMM_STATIC=0）作同机诊断；该组三次不是与上表逐对交错，
只用于确认大方向：

| kernel | 默认 median | pool12/default | pool16/default |
|---|---:|---:|---:|
| Copy | 60,755.4 | 1.29649 | 1.32221 |
| Scale | 27,691.2 | 0.68223（-31.78%） | 0.68547（-31.45%） |
| Add | 37,762.8 | 0.74825（-25.18%） | 0.74975（-25.03%） |
| Triad | 31,874.6 | 0.88587（-11.41%） | 0.88912（-11.09%） |

因此答案是明确的“不能”：+4 headroom 只把 Scale/Add/Triad 分别收窄约
0.33/0.15/0.32 个百分点。STREAM 本来就是 pool12 零 spill、max-live=2，
性能负账仍来自 W76 已定位的静态 XMM bridge/依赖形状和固定循环成本，而不是
FPR 容量 spill。

### 4.3 CoreMark 三对

| pair | pool12 Iter/s | pool16 Iter/s | ratio |
|---:|---:|---:|---:|
| 1 | 12,475.05 | 12,495.83 | 1.00167 |
| 2 | 12,450.20 | 12,477.13 | 1.00216 |
| 3 | 12,446.07 | 12,477.13 | 1.00250 |

中位 ratio `1.00216`（3/3 正），样本仅三对且幅度 0.22%，按噪声处理，不能
声称性能收益。

复现核心命令：

```sh
SVM=/tmp/w80-build/source/translator/linux/svm_translator_linux
BIN=/Users/swift/CLionProjects/SwiftVM-bench/bin

env SVM_JIT_CACHE= SVM_XMM_STATIC=1 SVM_XMM_POOL_EXT=0 \
  "$SVM" "$BIN/stream_x64"
env SVM_JIT_CACHE= SVM_XMM_STATIC=1 SVM_XMM_POOL_EXT=1 \
  "$SVM" "$BIN/stream_x64"

env SVM_JIT_CACHE= SVM_XMM_STATIC=1 SVM_XMM_POOL_EXT=0 \
  "$SVM" "$BIN/coremark_x64" 0x0 0x0 0x66 150000 7 1 2000
env SVM_JIT_CACHE= SVM_XMM_STATIC=1 SVM_XMM_POOL_EXT=1 \
  "$SVM" "$BIN/coremark_x64" 0x0 0x0 0x66 150000 7 1 2000
```

## 5. 改动文件与纪律

生产代码：

- `source/runtime/include/config.h`
- `source/runtime/common/perf_stats.h`
- `source/translator/x86/translator.cpp`
- `source/runtime/backend/arm64/trampolines.cpp`
- `source/runtime/backend/arm64/jit/translator.h`
- `source/runtime/backend/arm64/jit/translator.cpp`
- `source/runtime/backend/arm64/jit/translator_alu.cpp`

测试与报告：

- `source/tests/main_case.cpp`
- `docs/w80-fpr-pool.md`

未执行 commit/push/add/checkout/reset/stash；未改
`source/translator/linux/linker/`、`SwiftVM-bench/harness/run_matrix.sh` 或
`func_fingerprint_golden.txt`，也未重新生成 golden。
