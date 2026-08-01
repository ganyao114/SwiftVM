# W88：整数宽度链 tied-copy 实现与验收

## 1. 结论

`SVM_RA_INTWIDTH_TIE` 已按 W86 的严格证明边界实现，代码中的默认值仍为
**OFF**。它只在源值同时满足以下条件时，把现有物理 GPR 的所有权转给整数宽度
identity 结果：

1. 候选是允许的 `U32 BitExtract(v, 0, 32)` 或
   `ZeroExtend32To64(U32)`；
2. 递归 producer 证明确认该物理寄存器确实经过 W 写，高 32 位为零；
3. RA 的权威 live interval 证明 source 恰在当前 instruction 开始处结束。

任一证明失败即走原 `AllocGPR` 路径。`GetHostGPR` 和纯 `BitCast` 明确拒绝，
因此不会把一个仅以 W 视图读取、但 X 高半仍带旧值的寄存器误当作 W-clean。
OFF 路径不会调用新 selector，指纹对 master 逐位相同。

Linux 验收和性能数据由 orchestrator 在安静的 orb Ubuntu/aarch64 VM 亲跑。
关键结果如下：

- OFF 对 master 4184f6c：4400 个 function unit、11 个 guest，含
  `host_bytes`，零 diff。
- ON 的 CoreMark 共同 1698 个 unit 中 215 个缩短，`ir_changed=0`、
  `host_increased=0`；所有变化均为 host copy 删除。
- CoreMark 中位数提升 **0.34%**，7 对中 6 对为正；配对均值 95% CI 为
  **[+0.06%, +0.64%]**。
- 7zip 中位数提升 **7.34%**，7/7 对全正；配对均值 95% CI 为
  **[+6.38%, +8.23%]**。
- CoreMark 的 spill、scratch、pair fallback 和 target overflow 边界均未增长；
  全量测试、func_tests 六格和 CoreMark CRC 全绿。

### 默认建议

建议 orchestrator 在合流裁定时将该开关翻为默认 **ON**，但本 spike 的提交形态
依纪律保持默认 OFF。理由是 7zip 的 +7.34% 远离噪声且与 W86 的 9.064% 机械
上限相符；CoreMark 虽只有 +0.34%，配对 CI 仍为正；STREAM/smallpt 没有可信
反回归信号；静态检查未发现 IR 改写、host 增长或 RA 边界恶化。更重要的是，
该优化的安全性不依赖启发式 last-use，而由 `HasKnownWWrite` 与现有
`TryTieGPR` 的精确 interval 终点共同证明。

## 2. 实现

### 2.1 开关与调用路径

- `source/runtime/common/perf_stats.h`
  - `PerfStats2::kGetenvNames` 从 59 增至 60；
  - 末项登记 `SVM_RA_INTWIDTH_TIE`。
- `source/runtime/ir/opts/register_alloc_pass.cpp`
  - `IntWidthTieEnabled()` 仅在环境变量存在且不为 `0` 时返回 true，故默认 OFF；
  - function、block、scalar-insert 和 spill 后重跑的 RA 路径都传递同一 gate；
  - OFF 时不调用新 selector，保持原分配顺序与 codegen。
- `source/runtime/ir/opts/register_alloc_pass.h`
  - 增加显式 gate 的 test-only 入口，使同一测试进程可比较 OFF/ON，不受
    once-cached 环境变量影响。

### 2.2 候选与证明

`LinearScanAllocator::IntWidthTieSource()` 只接纳两类候选：

| 候选 | 接纳条件 | 失败形态 |
| --- | --- | --- |
| `BitExtract(v, 0, 32) -> U32` | `v` 递归证明为 W-clean | 原 `AllocGPR` + 正常 bridge |
| `ZeroExtend32To64(v: U32)` | `v` 递归证明为 W-clean | 原 `AllocGPR` + 正常 zero extend |

本次没有纳入可选的 `ZeroExtend64(U32)` identity 类，因此无需改变
`EmitZeroExtend64`。一般 `ubfx xD, xS, #0, #32` 也不会因“看起来像截断”而被
直接接受。

`HasKnownWWrite()` 的递归规则为：

- 普通 U32 producer（Load、ALU、shift 等）是实际 W 写，可接受；
- 只穿过 identity `BitExtract(...,0,32)` 和 `ZeroExtend32To64(U32)`；
- `GetHostGPR` 只是 pinned X 寄存器的 W 视图，不是物理 W 写，拒绝；
- 纯 `BitCast` 不产生物理 W 写，拒绝；
- 未定义值、非白名单 identity 或证明中断均拒绝。

selector 命中后仍调用既有 `TryTieGPR`。后者要求 source/result 均为 GPR，且
source interval 的 `live.end == current.start`；所以所有权转移不会延长 source
live range，也不会使用 W70/W74 已否决的近似 last-use。

backend 已有 `SharesGPR` no-op 快路径。RA 让 source/result 共享物理 GPR 后，
emitter 只删除 identity bridge，不引入新指令、不改 IR 语义。

### 2.3 测试新增

`source/tests/main_case.cpp` 新增 2 个 case、63 个 assertion：

- RA proof 矩阵：U32 Load、ALU、shift、两层
  `BitExtract -> ZeroExtend32To64`；
- 反例：`GetHostGPR`、`GetHostGPR -> BitExtract`、纯 `BitCast`、source 尚有
  later use；
- runtime 高半矩阵：register/memory producer，W/X consumer，upper32 为全零、
  全一和随机图样，包含连续两层 tie；
- `SVM_X86_PIN_EXT=0/1/2/3 × SVM_RA_INTWIDTH_TIE=0/1` 定向执行均为
  `63 assertions / 2 cases` 通过。

spill、helper 和 fault 边界继续由两态全量 suite 与 func_tests 覆盖；CoreMark
的 RA_SHAPE_PROF 另行确认优化没有改变实际 workload 的 spill/scratch 形态。

## 3. 验收门结果

### 3.1 指纹与 host bytes

orb 上的核心命令形态为：

```sh
bash source/translator/linux/tests/run_func_fingerprint_tests.sh \
  /tmp/w88-build/source/translator/linux/svm_translator_linux \
  --against /tmp/svm-build/source/translator/linux/svm_translator_linux

SVM_RA_INTWIDTH_TIE=1 \
  bash source/translator/linux/tests/run_func_fingerprint_tests.sh \
  /tmp/w88-build/source/translator/linux/svm_translator_linux
```

| 门 | 结果 |
| --- | --- |
| OFF self-consistency | OK，4400 function units / 11 guests，含 `host_bytes` |
| OFF vs master 4184f6c | fingerprint OK，零 diff |
| ON self-consistency / golden | rc=0，fingerprint OK |

ON 指纹仍与 golden 一致不代表优化没有命中：这 11 个 directed/real guest
不含 CoreMark，W86 的主要候选不在这组语料内。

CoreMark 用
`SVM_PROF=2 SVM_FUNC_BASE=1 SVM_JIT_CACHE=` 禁用 cache 后采样。单次退出时
unit 集合存在既有抖动：OFF 为 1706/1714/1718，ON 为 1711/1714/1715；这是
lazy compilation 与进程退出 dump 的竞态，因为同一状态内部也不稳定。共同的
1698 个 PC 才用于形态比较：

| 指标 | 结果 |
| --- | ---: |
| common PCs | 1698 |
| host changed | 215 |
| IR changed | 0 |
| host bytes increased | 0 |

最大降幅如下：

| function unit PC | host bytes 降幅 |
| --- | ---: |
| `0x4049b0` | 216 B |
| `0x4018af` | 88 B |
| `0x404026` | 84 B |
| `0x402dbc` | 76 B |
| `0x402922` | 52 B |
| `0x40408b` | 48 B |
| `0x465b85` | 48 B |
| `0x405620` | 48 B |
| `0x402d1c` | 44 B |
| `0x4048cd` | 44 B |
| `0x4104a8` | 40 B |
| `0x402ca8` | 36 B |

### 3.2 W86 块粒度到 W88 函数粒度对账

用 guest ELF 的符号与反汇编核实：

```sh
nm -n /Users/swift/CLionProjects/SwiftVM-bench/bin/coremark_x64 \
  | sed -n '/matrix_mul_matrix$/,/matrix_test$/p'
gobjdump -d --start-address=0x402d10 --stop-address=0x402e60 \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/coremark_x64
```

得到：

- `matrix_mul_matrix` 的符号范围为 `[0x402d10, 0x402db0)`；入口
  `endbr/test/je` 后的可编译函数主体从 `0x402d1c` 开始，内环回边块
  `0x402d58` 明确位于该函数内；
- `matrix_mul_matrix_bitextract` 的范围为 `[0x402db0, 0x402e60)`；函数主体
  从 `0x402dbc` 开始，内环回边块 `0x402df8` 明确位于该函数内。

因此 W86 的 block PC 与 W88 `SVM_FUNC_BASE=1` unit 映射成立：

| W86 block | 所属函数 unit | W86 内环预测 | W88 函数 unit 实测 | 差额 |
| --- | --- | ---: | ---: | ---: |
| `0x402d58` | `0x402d1c` | 8 条 / 32 B | 11 条 / 44 B | 3 条 / 12 B |
| `0x402df8` | `0x402dbc` | 16 条 / 64 B | 19 条 / 76 B | 3 条 / 12 B |

多出的三条不是 W86 内环预测失准，而是 function RA 把同一函数的入口与外层循环
块也纳入一个 unit。临时、未提交的 owner trace 在目标内环以外记录到的合格
tie site 为：

- `matrix_mul_matrix`：`0x402d1c`（函数主体/初值）、`0x402d40`（外层行
  初始化）、`0x402d48`（列/地址设置）、`0x402d86`（外层递增/回边）；
- `matrix_mul_matrix_bitextract`：对应的 `0x402dbc`、`0x402de0`、
  `0x402de8`、`0x402e37`。

函数级 host-byte 差分证明这些同族 block 合计再删除三条。由于 function RA 跨
block 统一分配，而 orb 留存的是最终 function-unit bytes，不带逐 host instruction
的 guest-block owner，不能再把这三条唯一分摊到上述四个 block。这里给出 trace
证明的来源集合和可证明的合计三条，不作“每块恰一条”的臆测。该诊断 trace 已从
正式源码移除。

### 3.3 功能与回归矩阵

| 验收项 | OFF | ON | 裁定 |
| --- | --- | --- | --- |
| orb 全量 `swift_test` | 142648 / 126 | 142648 / 126 | 两态全绿 |
| master 基线 | 142585 / 124 | — | +63 assertions / +2 cases 正是新增定向测试 |
| func_tests default | rc=101 | rc=101 | 一致 |
| func_tests block (`SVM_FUNC_BASE=0`) | rc=101 | rc=101 | 一致 |
| func_tests interpreter | rc=101 | rc=101 | 一致 |
| func_tests output | `sha=a20dc01e9637fd7a` | 同左 | 一致 |
| func_tests checksum | `9f52b7d59285dbe5` | 同左 | 一致 |
| CoreMark 150k | — | Correct operation validated | 五 CRC 全对 |

CoreMark ON 的 CRC 为：

```text
seedcrc      0xe9f5
crclist      0xe714
crcmatrix    0x1fd7
crcstate     0x8e3a
crcfinal     0x25b5
```

macOS 上，排除 fuzz 家族的两态 suite 均通过
`139972 assertions / 92 cases`；pin 0–3 的新定向矩阵两态均通过
`63 assertions / 2 cases`。完整 fuzz 家族在 Codex 执行沙箱与 orchestrator
原生终端之间表现不同，详见附录 A；master 与 W88 在同一环境中同向，因此不是
W88 回归。

### 3.4 RA_SHAPE_PROF 边界

CoreMark 150k 的两态结果：

| 指标 | OFF | ON |
| --- | ---: | ---: |
| spill units | 0 | 0 |
| spill defs / loads / stores | 0 / 0 / 0 | 0 / 0 / 0 |
| spill high-water | 0 | 0 |
| pair fallbacks | 0 | 0 |
| target overflow | 0 | 0 |
| scratch GPR/FPR | 3 / 3 | 3 / 3 |

两次统计的总 unit 数可随前述 lazy dump 竞态变化，但每个 unit 的 scratch 档位、
spill 计数和 high-water 分布形状一致。候选通过所有权转移复用已有寄存器，未增加
live range，所以结果符合设计预期。helper snapshot 在本优化中没有 ABI 或 pin map
变化；全量/helper/fault 测试两态通过，未出现间接增长或边界错误。

## 4. 性能 A/B

所有 Linux 性能数据均由 orchestrator 在同一 orb VM、同一 W88 binary 上亲跑，
只切换 `SVM_RA_INTWIDTH_TIE`。每对交错执行，奇数对 OFF→ON、偶数对 ON→OFF，
以抵消单向温漂。95% CI 以每对百分比变化的算术均值、sample standard deviation
和双侧 Student t 区间计算；n=7 使用 df=6，n=3 使用 df=2。

### 4.1 CoreMark

单位为 Iterations/Sec，越高越好。

| pair | OFF | ON | ON/OFF |
| ---: | ---: | ---: | ---: |
| 1 | 12617.77 | 12652.89 | +0.28% |
| 2 | 12645.42 | 12686.06 | +0.32% |
| 3 | 12552.30 | 12666.78 | +0.91% |
| 4 | 12633.71 | 12649.69 | +0.13% |
| 5 | 12547.05 | 12623.07 | +0.61% |
| 6 | 12607.16 | 12635.84 | +0.23% |
| 7 | 12586.00 | 12582.84 | -0.03% |

- OFF 中位：12607.16；ON 中位：12649.69；中位比 **+0.34%**。
- 6/7 对为正。
- 配对均值：**+0.35%**；95% CI：**[+0.06%, +0.64%]**。

收益不大，但 CI 刚性跨过零线；与 W86 的 CoreMark 机械可删比例相比，说明删除的
width copy 并非全部落在同等关键路径上。

### 4.2 7zip

命令为 `7zip_x64 b -mmt1 -md=16m`，指标为 Tot MIPS，越高越好。

| pair | OFF | ON | ON/OFF |
| ---: | ---: | ---: | ---: |
| 1 | 2576 | 2783 | +8.04% |
| 2 | 2571 | 2751 | +7.00% |
| 3 | 2586 | 2749 | +6.30% |
| 4 | 2571 | 2735 | +6.38% |
| 5 | 2572 | 2795 | +8.67% |
| 6 | 2589 | 2803 | +8.27% |
| 7 | 2597 | 2765 | +6.47% |

- OFF 中位：2576；ON 中位：2765；中位比 **+7.34%**。
- 7/7 对为正，每对绝对增量为 163–223 MIPS。
- 配对均值：**+7.30%**；95% CI：**[+6.38%, +8.23%]**。

W86 对 7zip 的同类 bridge 机械上限为 9.064%，实测墙钟/吞吐收益达到其中大部分，
同时方向完全一致，是建议默认 ON 的主要依据。

### 4.3 smallpt 与 STREAM smoke

smallpt 为 64 spp、320×240，wall time 越低越好：

| pair | OFF ms | ON ms |
| ---: | ---: | ---: |
| 1 | 20632 | 21049 |
| 2 | 33720 | 29879 |
| 3 | 26246 | 23355 |

run 间散布约 ±30%。按 lower-is-better 的配对变化计算，均值 +7.75%，95% CI
为 `[-13.19%, +28.70%]`，完整覆盖零；W86 机械上限仅 0.772%。裁定为 null，
既不能宣称收益，也没有可信回归信号。

STREAM 原始 MB/s：

| pair | state | Copy | Scale | Add | Triad |
| ---: | --- | ---: | ---: | ---: | ---: |
| 1 | OFF | 47853.7 | 21160.6 | 28887.8 | 24778.7 |
| 1 | ON | 53736.5 | 24875.6 | 31893.6 | 25303.9 |
| 2 | OFF | 50769.9 | 23318.5 | 30345.2 | 26905.1 |
| 2 | ON | 48040.4 | 23148.2 | 31157.6 | 26137.5 |
| 3 | OFF | 54199.8 | 23735.4 | 34211.3 | 28638.0 |
| 3 | ON | 51695.8 | 23947.6 | 30860.1 | 25967.7 |

orchestrator 的中位摘要为 Copy +1.8%、Scale +2.7%、Add +2.7%、Triad -2.9%；
各项 run 间散布约 ±10%。配对均值的 95% CI 分别为 Copy
`[-24.05%, +25.58%]`、Scale `[-19.24%, +31.05%]`、Add
`[-24.23%, +26.42%]`、Triad `[-17.61%, +10.90%]`，全部跨零。W86 的 STREAM
机械上限仅 0.000032%，因此这些变化只能裁定为测量噪声。

## 5. 风险与保留条件

### 5.1 已封住的正确性风险

- **高半泄漏**：`GetHostGPR` 和 `BitCast` 不算 W 写；identity 链不能洗白它们。
- **错误所有权转移**：只用 `TryTieGPR` 的权威 interval 边界，不用邻近指令或
  手工 use-count 近似。
- **live range / spill 回归**：tie 不延长 source；RA_SHAPE_PROF 的 spill、
  high-water、scratch 和 fallback 均不增长。
- **意外扩面**：未纳入一般 `ubfx` 或可选 `ZeroExtend64`，proof 失败逐项回退。
- **OFF 漂移**：OFF 不进入 selector，指纹和 host bytes 对 master 零 diff。

### 5.2 剩余风险与监测建议

- CoreMark unit PC 集合的退出时抖动会干扰直接 set-equality；后续静态 A/B 应继续
  取共同 PC，并同时要求 `ir_changed=0`、`host_increased=0`。
- function RA 会把一个 W86 block 候选扩展为同函数多 block 的合计收益。新语料
  若出现 host 增长，应先检查 proof/fallback，而不是只看热点 block 名。
- smallpt/STREAM 的三对数据只够反回归 smoke，不足以估计亚百分比效应。
- 若默认翻为 ON，建议保留 `SVM_RA_INTWIDTH_TIE=0` 作为现场回退，并继续把
  指纹、RA_SHAPE_PROF 和 high-half 定向矩阵作为门禁。

## 附录 A：macOS fuzz SIGILL 复核与裁定

### A.1 最初复现命令与环境

最初报告 SIGILL 时使用的是 Codex `exec_command` 的默认 login zsh 环境，未用
`env -i`，命令原样为：

```sh
/tmp/w88-mac-build/source/tests/swift_test 'Fuzz x86 alu' --reporter compact

/Users/swift/CLionProjects/SwiftVM/cmake-build-release/source/tests/swift_test \
  'Fuzz x86 alu' --reporter compact
```

前者是 W88 build，后者是未修改 master build。两者都由继承环境启动，均在 Catch2
标注的 `source/tests/fuzz/x86_fuzz.cpp:1249` 处以 rc=132 / SIGILL 结束。全量
两态的原命令为：

```sh
SVM_RA_INTWIDTH_TIE=0 /tmp/w88-mac-build/source/tests/swift_test --reporter compact
SVM_RA_INTWIDTH_TIE=1 /tmp/w88-mac-build/source/tests/swift_test --reporter compact
```

固定 fuzz seed 的复核还使用过 `SWIFT_FUZZ_SEED=101`；OFF/ON 同向失败。排除
`Fuzz x86 alu` 后，下一 fuzz family 又在 `x86_fuzz.cpp:1326` 同形 SIGILL，说明
它不是某个 W88 width-chain candidate 特有的误编译。

当时继承环境中 `env | rg '^SVM_'` 为空；因此没有证据把它具体归因到某一个
`SVM_*` 泄漏变量。`x86_fuzz.cpp:1249` 本身是 Catch2 test-case 起点，不是已定位
的非法 host opcode。

### A.2 干净环境复核

orchestrator 在原生终端亲跑：

```sh
env -i HOME=/Users/swift \
  PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  /tmp/w88-mac-build/source/tests/swift_test \
  'Fuzz x86 alu' --reporter compact
```

定向 `Fuzz x86 alu` 连续 5 次、完整 fuzz family 连续 10 次均零失败。

作为反向核查，当前 Codex 受限执行上下文中，同一 `env -i` 命令对 W88 新鲜重建
仍 5/5 SIGILL；未修改 master 的同一命令也 SIGILL。这说明 `env -i` 只能清空传给
进程的普通环境变量，不能移除 Codex launcher/sandbox 的执行上下文差异。结果按
launcher 分组，而不按 master/W88 或开关状态分组：

| 执行上下文 | master | W88 | 结果 |
| --- | --- | --- | --- |
| orchestrator 原生终端，`env -i` | 未单独记录 | 5 次定向 + 10 次 fuzz family 全过 | 通过 |
| Codex 执行沙箱，继承环境 | SIGILL | SIGILL | 同向失败 |
| Codex 执行沙箱，`env -i` | SIGILL | 5/5 SIGILL | 同向失败 |

### A.3 裁定

该 SIGILL **不是 W88 缺陷**，也不能严格收窄为某个已知 `SVM_*` 变量泄漏；它是
Codex launcher/sandbox 相关的执行环境伪影。未修改 master 与 W88 在 Codex
上下文同向，而 W88 在原生干净环境重复运行全绿，已排除
`SVM_RA_INTWIDTH_TIE` 因果关系。
本任务按 environment artifact 关闭，不修改 fuzz、Unicorn 或 JIT 代码。若未来在
原生终端的干净环境复现，必须保存实际 seed、非法 host PC 与反汇编后另立缺陷；
当前没有这样的原生复现证据。

## 附录 B：改动清单

- `source/runtime/common/perf_stats.h`
- `source/runtime/ir/opts/register_alloc_pass.cpp`
- `source/runtime/ir/opts/register_alloc_pass.h`
- `source/tests/main_case.cpp`
- `docs/w88-intwidth-tie.md`

未改动 linker、benchmark harness 或 fingerprint golden；没有重新生成 golden。
