# W78：Linux FlagM / FlagM2 HWCAP 检测补齐

## 1. 结论

Linux AArch64 现在从 auxv 检测 FlagM 与 FlagM2，并把结果写入既有
`Config::arm64_features`：FlagM 复用 `Arm64Features::FlagM`，FlagM2 复用 W47
已有的 `Arm64Features::AXFlag`。`X64Decoder` 不再自行做 Apple-only sysctl；它从
所属 Config 的 feature snapshot 决定 CFINV/FlagM2 lowering。结果是 Orb 上两个已知
carry unit 精确从 48 降到 45 IR，除此以外对 master 无 codegen 变化。

生产改动通过双平台全量构建、全量 `swift_test`、carry 定向矩阵、固定种子 fuzz、
FlagM2 compact 差分、`func_tests` 六格和 CoreMark 150k。macOS 对 master 的 4,400-unit
指纹为零 diff，证明 mac codegen 逐位不变。

有两处 prompt 中的数值预期必须按实测修正：

1. Linux arm64 UAPI 的位号是 `HWCAP_FLAGM = 1 << 27`、
   `HWCAP2_FLAGM2 = 1 << 7`，不是 `1 << 4` / `1 << 1`。后两者实际分别是
   PMULL / SVE2。实现按内核 UAPI 使用 27/7，并在 fallback 旁写明出处。
2. 修复后对 golden 的根因只剩 st_blksize，但 unified diff 的内容行数仍为
   18（9 `-` + 9 `+`），没有降到 12。原因是修复前 FlagM 的 `+3` 恰好抵消
   st_blksize 的 `-3`，使 `real_busy` / `func_tests` totals 不出现在 diff；修复后
   两个 totals 各下降 3，新增四条 totals 文本，正好抵消移除的四条 FlagM 文本。
   完整前后内容见 §6。没有为满足错误的文本行数预期而改 golden 或隐藏 totals。

## 2. 根因与现场

Orb 的当前 CPU/auxv：

```text
Features : ... flagm ... flagm2 ...
AT_HWCAP:  0x2fb3ffff
AT_HWCAP2: 0x184181
```

Orb 安装的 Linux AArch64 UAPI 头 `/usr/include/aarch64-linux-gnu/asm/hwcap.h`：

```c
#define HWCAP_PMULL      (1 << 4)
#define HWCAP_FLAGM      (1 << 27)
#define HWCAP2_SVE2      (1 << 1)
#define HWCAP2_FLAGM2    (1 << 7)
```

`0x2fb3ffff` 含 bit 27，`0x184181` 含 bit 7，因此 Orb 合法支持两项 feature。
原实现的 `FlagsCfinvEnabled()` 只在 `__APPLE__ && __aarch64__` 内调用
`hw.optional.arm.FEAT_FlagM` sysctl，Linux 固定返回 false。已知 inverted carry 的
ADC/SBB 因而不能用单条 `InvertCarry`，而走物化 carry 的回退序列；优化后每个现场
多 3 IR。W47 的 FlagM2 sysctl 同样只覆盖 Apple，Linux 的 Config 缺少 `AXFlag`，
即使显式开启 `SVM_FLAGS_FCMP_COMPACT=1` 也不能选择 compact lowering。

若照 prompt 的 4/1 fallback 实现，FlagM 会被错误绑定到 PMULL，且 Orb 的 HWCAP2
bit 1 实际为 0，会漏掉真实存在的 FlagM2；在别的 CPU 上还可能发射不受支持的
flag 指令。因此不能采用该位号。

## 3. 生产改动

### `source/translator/x86/translator.cpp`

- Linux AArch64 条件编译引入 `<asm/hwcap.h>` 与 `<sys/auxv.h>`。
- 老内核头缺宏时按 Linux arm64 UAPI 定义 `HWCAP_FLAGM` bit 27、
  `HWCAP2_FLAGM2` bit 7。
- `DetectArm64Features()` 的 Linux 分支用
  `getauxval(AT_HWCAP)` / `getauxval(AT_HWCAP2)` 检测两项 feature。
- auxv 是进程常量；两次读取结果由函数内 `static const` 缓存，之后每个 Instance
  收到相同 bitmap。
- Apple sysctl 分支的检测条件和写入位没有改动。

### `source/runtime/frontend/x86/decoder.{h,cc}`

- `X64Decoder` 构造时从既有 `arm64_features` 参数 snapshot `FlagM`；FlagM2 继续
  使用 W47 已有的 `AXFlag` 与 `flags_fcmp_compact_`。
- `FlagsCfinvEnabled()` 改为读取该 snapshot，同时保留现有
  `SVM_FLAGS_CFINV=0` 强制关闭语义。
- 删除 decoder 内第二套 Apple-only FlagM sysctl，避免 feature source 分裂。

最终生产代码只改以上三个文件；carry 矩阵用临时测试夹具验证，跑完后已撤下，
所以全量 assertion/case 基线不变。

## 4. Config、IR 与 JIT cache key

路径沿用 W47，没有新造 feature/key 机制：

1. `DetectArm64Features()` 的结果写入 `Config::arm64_features`。
2. function/block decoder 构造时都传入其 AddressSpace/Module 的同一 Config bitmap。
3. FlagM 决定 ADC/SBB 前是否生成 `InvertCarry` IR；FlagM2 的 compact 决定继续由
   `PublishFCmpFlags(..., Imm(compact))` 固化在 IR。
4. `ComputeConfigHash()` 已直接 hash 完整 `config.arm64_features`，所以 FlagM/FlagM2
   bitmap 不同的机器不会复用同一 JIT cache artifact。
5. `ComputeEnvHash()` hash 所有 `SVM_*` / `SWIFT_*` 原始值（只排除 cache 自己的
   knob），因此 `SVM_FLAGS_CFINV=0` 和 `SVM_FLAGS_FCMP_COMPACT=0/1` 也会分 cache key。

env 语义实测：Linux 上 `SVM_FLAGS_CFINV=0` 后，修复版对 master 的 4,400-unit
指纹重新变为零 diff；非零值也不能绕过宿主 feature 检测。FlagM2 compact 仍是
opt-in：只有 `SVM_FLAGS_FCMP_COMPACT` 非零且 Config 含 `AXFlag` 时才启用，显式
`=0` 继续强制关闭。

## 5. 正确性矩阵

### Carry 定向矩阵

临时夹具覆盖 8 个语义场景：

- `cmp` 产生 carry/no-carry 后分别消费 `adc`；
- `cmp` 产生 borrow/no-borrow 后分别消费 `sbb`；
- `neg` 的 zero/nonzero carry 后分别消费 `adc` 与 `sbb`。

每个场景各跑同 unit 与 producer `hlt` 后重新进入 Runtime 的跨 unit 形态，共 16 行。
每行比较 JIT、解释器和独立 128-bit 算术 oracle，并校验结果与最终 CF (`setb`)。

| 平台 | CFINV 状态 | 结果 |
|---|---|---|
| macOS | default / `=1` / `=0` | 每态 106 assertions，PASS |
| Orb Linux | default / `=1` / `=0` | 每态 106 assertions，PASS |

### 固定种子 fuzz

Orb 上对 `Fuzz x86 alu` 与 `Fuzz x86 inc dec neg not` 两族，用相同的五组
`SWIFT_FUZZ_SEED`（`1`、`0x12345678`、`0xdeadbeef`、`0xc0ffee`、
`0xffffffffffffffff`）分别跑 `SVM_FLAGS_CFINV=1/0`：共 20 次，全部 PASS。

### FlagM2 定向

`COMIS compact flags all consumers JIT interpreter differential` 在 macOS 与 Orb
分别用 `SVM_FLAGS_FCMP_COMPACT=1/0` 运行；四次均为 3,482 assertions / 1 case，
全部 PASS。Orb 的 ON 运行也证明 HWCAP2 bit 7 路径不会错误发出非法指令。

## 6. 指纹前后对比

### 修复版 vs master（验收 A/B）

自一致门：4,400 function units / 11 guests，包含 host_bytes，两遍一致。跨 build
差异精确为两个目标 unit 及其派生 totals：

```diff
-real_busy pc=0x439970 ir=48
+real_busy pc=0x439970 ir=45
-real_busy TOTALS ... ir_insts=27602
+real_busy TOTALS ... ir_insts=27599

-func_tests pc=0x419c10 ir=48
+func_tests pc=0x419c10 ir=45
-func_tests TOTALS ... ir_insts=26987
+func_tests TOTALS ... ir_insts=26984
```

unit 数、decoded block 数均不变，无其他 unit/IR 差异。`SVM_FLAGS_CFINV=0` 时 A/B
零 diff。macOS 修复版 vs 当前 master 同样是 4,400 units、包含 host_bytes 的零 diff。

### master vs golden（修复前，18 内容行）

- `real_hello`：st_blksize unit-set 4 行 + totals 2 行；
- `real_busy`：FlagM unit 2 行 + st_blksize unit-set 4 行；两项 `-3/+3` 抵消，
  totals 不出现；
- `func_tests`：同上。

合计 9 removals + 9 additions。FlagM 内容为：

```diff
-real_busy pc=0x439970 ir=45
+real_busy pc=0x439970 ir=48
-func_tests pc=0x419c10 ir=45
+func_tests pc=0x419c10 ir=48
```

### 修复版 vs golden（修复后，仍为 18 内容行）

两个 FlagM unit 已与 golden 一致并完全消失；残余只包含三个 guest 的 st_blksize
unit-set 漂移。每个 guest 都是 4 条 unit-set 内容行和 2 条 totals 内容行：

```diff
# real_hello / real_busy / func_tests 各自同形
+... pc=<new-a> ir=10
+... pc=<new-b> ir=12
-... pc=<old-a> ir=13
-... pc=<old-b> ir=12
-... TOTALS ... ir_insts=<golden>
+... TOTALS ... ir_insts=<golden-3>
```

因此仍是 3 × 6 = 18 内容行（9 removals + 9 additions），但根因已经从
“st_blksize + FlagM”收敛为仅 st_blksize。golden 未修改、未重新生成。

## 7. 其余验收门

| 门 | 结果 |
|---|---|
| macOS 干净 configure 后 `all` | PASS |
| Orb Linux 干净 configure 后 `all` | PASS |
| macOS 全量 `swift_test` | PASS：139,975 assertions / 124 cases |
| Orb Linux 全量 `swift_test` | PASS：142,585 / 124 |
| `func_tests` 六格 | function / block / interpreter × CFINV 1/0 全部 rc=101；checksum `9f52b7d59285dbe5`；stdout SHA-256 全为 `9c3194ff498da03869bbabbd81241fd2ec771619281f969c4b4edc55577a6810` |
| Linux 指纹 A/B | 仅两个 48→45 unit 及派生 totals；自一致含 host_bytes PASS |
| Linux CFINV=0 指纹 A/B | 零 diff |
| macOS 指纹 A/B | 零 diff；自一致含 host_bytes PASS |
| 固定种子 carry fuzz | 两族 × 五 seed × CFINV 两态，共 20 次 PASS |
| FlagM2 compact 定向 | 两平台 ON/OFF 均 3,482 assertions PASS |
| `git diff --check` | PASS |

### CoreMark 150k

Orb 上关闭 disk cache，按 CFINV ON/OFF 做三对交错；六次均 rc=0、
`Correct operation validated`，CRC 为
`0xe9f5/0xe714/0x1fd7/0x8e3a/0x25b5`。

| pair | ON wall / Iterations/s | OFF wall / Iterations/s | ON/OFF throughput |
|---:|---:|---:|---:|
| 1 | 12.186s / 12,359.9 | 12.161s / 12,380.3 | 0.99835 |
| 2 | 12.172s / 12,367.1 | 12.096s / 12,444.0 | 0.99382 |
| 3 | 12.021s / 12,523.0 | 12.098s / 12,443.0 | 1.00643 |

配对 ratio 中位 0.99835（约 -0.17%，2 负 1 正），属于当前受载窗口噪声，不能声称
正收益；本项按合法 lowering 与每 site 静态 -3 IR 落地，不以这组三对墙钟立收益结论。

## 8. 纪律与工作树

- 未执行 commit/push/add/checkout/reset/stash。
- 未改 `source/translator/linux/linker/`、
  `SwiftVM-bench/harness/run_matrix.sh` 或任何 golden。
- 最终改动：三个生产文件与本报告；没有遗留探针或临时测试代码。

