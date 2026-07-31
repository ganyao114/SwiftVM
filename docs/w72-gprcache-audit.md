# W72：unit-local GPR cache 现状核查与关闭结论

日期：2026-08-01

基线：`89c5073`

分支：`w72-gprcache-audit`

## 1. 结论

**关闭 W44 的 unit-local GPR cache 候选，不进入实现 spike；W44 队列清空。**

W44 的 8–18% 预估来自 pin 波次以前。当前默认 `SVM_X86_PIN_EXT=2` 已使
RAX/RCX/RDX/RBX/RSP/RBP/RSI/RDI/R8–R11 全部驻留；并非预期中的 RBP/RSP，
实际只剩 R12–R15 访问 `ThreadContext64`。重新测得：

- W44：GPR state 占 host 12.0%，动态 7.559 次/exit。
- 当前：6944 个到达 unit 的 post-IR 仅剩 5710 条 R12–R15 load/store，占
  277644 个 host instruction slot 的 **2.0566%**；`SVM_EXEC_PROF` 实测
  **1.282711 次/exit**，比 W44 低 **83.03%**。
- 即使忽略所有正确性边界，每个块/寄存器只保留首次必要 load 和最后必要
  store，最多也只能删 1107 条，即全体 host slot 的 **0.3987%**。
- 当前源码的 7zip 8 秒采样有 6176 个 JIT mapped samples。R12–R15 的实际
  state LDR/STR 命中 261 个（4.2260%）；同一个“不看 CFG、只留首载末写”的
  乐观模型只覆盖 61 个样本，即 **0.9877%**。它仍未扣除 fault/helper/分支/
  unit-exit 强制物化，因此真实收益面严格更小。

这已经满足任务的关闭门“7zip 残余动态收益上限 <2%”。继续实现会重新承担
W32 已实证的 partial-write bridge、uniform invalidation 和 direct-link 边界风险，
却没有足够的收益预算覆盖它们。

## 2. 可复现方法

### 2.1 构建与 7zip dump

Orb `wine-ci`，源码为 VM 内可见的 `/private/tmp/w65`，只新建 `/tmp/w72-build`；
没有触碰 `/tmp/svm-build`：

```bash
cmake -S /private/tmp/w65 -B /tmp/w72-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/w72-build --target svm_translator_linux -j8
```

PIN2/PIN3 各跑一次，JIT disk cache 关闭：

```bash
for pin in 2 3; do
  mkdir -p /tmp/w72-audit/pin${pin}
  SVM_JIT_CACHE= \
  SVM_X86_PIN_EXT=${pin} \
  SVM_JIT_SCRATCH_XPOOL=1 \
  SVM_RA_SHAPE_PROF=/tmp/w72-audit/pin${pin}/shape.prof \
  SVM_PROF2=1 \
  SVM_DUMP_IR=1 \
  SVM_DUMP_IR_POST=1 \
  SVM_FLAGS_DEBUG=1 \
  SVM_VIXL_HOST_DUMP=1 \
    /tmp/w72-build/source/translator/linux/svm_translator_linux \
    /Users/swift/CLionProjects/SwiftVM-bench/bin/7zip_x64 \
    b -mmt1 -md=16m \
    >/tmp/w72-audit/pin${pin}/bench.out \
    2>/tmp/w72-audit/pin${pin}/dump.log
done
```

两态均 rc=0；各到达 6944 个 unit。`[svm-host] size=` 求和得到 host bytes；
除以 4 得 instruction slots。raw/post-IR 中只统计 `LoadUniform`/
`StoreUniform` 的 offset 96、104、112、120，即 R12、R13、R14、R15。

### 2.2 动态 GPR uniform 计数

现有探针在 block 入口一次性把该 block 的静态 post-IR GPR uniform 数加入计数器，
不是在每次 state 访问旁插桩；定义见
`source/runtime/backend/arm64/jit/translator.cpp:53-75`。运行命令：

```bash
SVM_JIT_CACHE= \
SVM_X86_PIN_EXT=2 \
SVM_JIT_SCRATCH_XPOOL=0 \
SVM_EXEC_PROF=1 \
  /tmp/w72-build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/7zip_x64 \
  b -mmt1 -md=16m
```

这里仅计数轮使用 `XPOOL=0`：当前 master 的 `SVM_EXEC_PROF=1` 与默认 XPOOL
仍会在少数 spill unit 触发既有断言：

```text
Check Failed! spilling unit reached emission without conditional x18 reservation
```

这是 W64 已记录的探针 scratch-contract 问题。XPOOL 不改 uniform 优化后的 IR；
本轮只使用 access/exits 计数，不使用它的墙钟。原始输出：

```text
[svm-exec] elapsed_s=51.160606571 exits=10969799244 ...
gpr_uniform=14071078068 xmm_uniform=807174839 pad=0
```

因此 `14071078068 / 10969799244 = 1.282711` 次 GPR state access/exit；相对
W44 的 7.559，下降 `(1 - 1.282711/7.559) = 83.03%`。

### 2.3 热度采样与动态上限

Orb 没有 `perf`。为取得 unit 热度，在 macOS 用同一 HEAD 新建
`/private/tmp/w72-build-mac`，以默认 PIN2 运行 7zip；`SVM_EXEC_MAP=1` 输出地址区间，
`sample` 采 8 秒：

```bash
SVM_JIT_CACHE= SVM_X86_PIN_EXT=2 SVM_JIT_SCRATCH_XPOOL=1 SVM_EXEC_MAP=1 \
  /private/tmp/w72-build-mac/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/7zip_x64 b -mmt1 -md=16m &
pid=$!
sample "$pid" 8 -file /tmp/w72-audit-mac/sample.txt
wait "$pid"
```

另跑一次 `SVM_VIXL_HOST_DUMP=1` 取得 bytes。两轮均为 5961 个相同 guest unit，
5961/5961 的 size 全相同，故用 `sample_pc - exec_start` 对齐到 dump instruction。
AArch64 指令按 `[x28, #offset]` 解码；`State::uniform_buffer_begin=656`，故
R12–R15 分别位于 state offset 752/760/768/776。这里只把采样用于 hotness 和
访问指令占样本比例；Linux 的 unit 静态计数、pool、spill 和 host slots 全部来自
Orb PIN2 dump，不把 Darwin code size 混入 Linux 净账。

乐观可删模型按每个 unit/寄存器的 host 指令顺序保留：若首个访问是 load，则保留
该 load；若存在 store，则保留最后一个 store；其余全算可删。这个模型故意忽略
CFG 多路径、fault、helper、partial write 和 exit，因而只能高估收益。

## 3. 当前默认 PIN2 的残余

### 3.1 pin 覆盖与残余寄存器

`source/translator/x86/translator.cpp:48-61` 的 level-2 map 是：

| guest GPR | host |
|---|---|
| RAX / RCX / RDX | x22 / x23 / x29 |
| RBX / RSP / RBP | x20 / x19 / x21 |
| RSI / RDI / R8 / R9 / R10 / R11 | x0 / x1 / x2 / x3 / x4 / x5 |

所以默认未 pin 的普通 GPR **只有 R12–R15**。post-IR 中上述 12 个已 pin GPR 的
memory-backed load/store 均为 0；RBP/RSP 不在残余集合。

### 3.2 raw 到 post-UniformElimination

| guest | raw load | raw store | post load | post store | 已消 load |
|---|---:|---:|---:|---:|---:|
| R12 | 1621 | 722 | 1390 | 722 | 231 |
| R13 | 1092 | 614 | 864 | 614 | 228 |
| R14 | 817 | 439 | 684 | 439 | 133 |
| R15 | 751 | 403 | 594 | 403 | 157 |
| **合计** | **4281** | **2178** | **3532** | **2178** | **749** |

现有 UniformElimination 已拿掉 749/4281 = **17.50%** raw loads，或 raw 总访问的
749/6459 = **11.60%**。剩余 5710 个 post-IR state op 各直发一条 LDR/STR
（`translator_mem.cpp:433-476,485-505`）。本次 PIN2 host 总量为：

```text
units=6944 host_bytes=1110576 instruction_slots=277644
```

所以剩余 GPR state 静态占比为 `5710/277644 = 2.0566%`，而非 W44 的 12.0%。

### 3.3 热 unit

下表按 macOS 当前源码采样的 guest entry 排名；host slots 与 R12–R15 L/S 来自
Orb Linux PIN2 post-IR。`乐观可删`仍是无视所有边界的首载/末写模型。

| rank | guest unit | samples | Linux slots | R12–R15 L/S | state/slots | 乐观可删/slots |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | `0x427fea` | 212 | 348 | 0 / 0 | 0% | 0 / 0% |
| 2 | `0x42cbc0` | 128 | 34 | 0 / 0 | 0% | 0 / 0% |
| 3 | `0x42ee88` | 117 | 136 | 3 / 1 | 2.941% | 1 / 0.735% |
| 4 | `0x42d0b0` | 116 | 22 | 0 / 0 | 0% | 0 / 0% |
| 5 | `0x4280f5` | 112 | 112 | 4 / 2 | 5.357% | 3 / 2.679% |
| 6 | `0x42d2ca` | 106 | 31 | 0 / 0 | 0% | 0 / 0% |
| 7 | `0x430d5d` | 99 | 178 | 10 / 3 | 7.303% | 6 / 3.371% |
| 8 | `0x428290` | 98 | 31 | 0 / 0 | 0% | 0 / 0% |
| 9 | `0x428145` | 97 | 44 | 1 / 1 | 4.545% | 0 / 0% |
| 10 | `0x431619` | 92 | 307 | 3 / 4 | 2.280% | 5 / 1.629% |
| 12 | `0x428140` | 84 | 25 | 0 / 0 | 0% | 0 / 0% |

W70 已记录的 LZMA CL-shift unit `0x42c480` 本次为 440 bytes/110 slots，post-IR
R12–R15 为 3 load + 3 store，state 占 5.455%；三组都是必要首载+末写，乐观可删
仍为 0。相邻 `0x42c9d5` 为 276 bytes/69 slots，仅 1 个 R12 store，占 1.449%，
乐观可删也是 0。

前 50 个 sampled hot unit 覆盖 55.23% mapped samples；用 sample 权重乘以各 unit
静态可删比例得到 0.590% 的热度代理。全 420 个 sampled unit 为 0.649%。更直接地，
把 sample PC 对到实际 state 指令：

| guest state | state LDR/STR samples | 乐观可删 samples |
|---|---:|---:|
| R12 | 116 | 36 |
| R13 | 59 | 6 |
| R14 | 32 | 14 |
| R15 | 54 | 5 |
| **合计** | **261 / 6176 (4.2260%)** | **61 / 6176 (0.9877%)** |

这 0.9877% 已把跨路径上互斥的末写也错误地当成可删，仍小于 2%；真实安全形态
只会更低。

### 3.4 全语料的无边界机械上限

逐 block/寄存器至少保留首次必要 load 与最后必要 store：

| guest | post ops (L/S) | 最少保留 L/S | 最多可删 |
|---|---:|---:|---:|
| R12 | 2112 (1390/722) | 977/650 | 485 |
| R13 | 1478 (864/614) | 689/542 | 247 |
| R14 | 1123 (684/439) | 551/401 | 171 |
| R15 | 997 (594/403) | 456/337 | 204 |
| **合计** | **5710 (3532/2178)** | **2673/1930** | **1107** |

所以即使不存在任何 fault/helper/direct-link/partial-write 成本，静态机械上限也只有
`1107/277644 = 0.3987%` host slots。

## 4. PIN3 为什么净负，unit-local 是否能避开

W60 已记录：PIN3 把 R12–R15 固定到 x6–x9，动态池缩到 x11–x17；4400-unit
host +3.89%，spill memory ops 5425→14071（2.6x），CoreMark level3/level2
五对 **-10.18%**。当前 7zip 的 W67 探针同样复证该机制：

```text
PIN2:
units=6944 spill_units=7 spill_defs=446 spill_loads=530 spill_stores=433
gpr_pool bins=11:7,12:6937
helper direct calls=84 snapshot_instructions=1492 bytes=5968

PIN3:
units=6944 spill_units=249 spill_defs=6527 spill_loads=7333 spill_stores=5879
gpr_pool bins=7:244,8:6700
helper direct calls=84 snapshot_instructions=1752 bytes=7008
```

| 项 | PIN2 | PIN3 | 变化 |
|---|---:|---:|---:|
| spill units | 7 (0.1008%) | 249 (3.5858%) | 35.57x |
| spill L/S | 530 / 433 | 7333 / 5879 | +12249 条 memory ops |
| helper snapshot instructions | 1492 | 1752 | +260 (+17.43%) |
| R12–R15 state ops | 5710 | 0 | -5710 |
| host instruction slots | 277644 | 287401 | **+9757 (+3.514%)** |

PIN3 删完 5710 条 residual state op，却在 spill/helper/搬运上付出更多。dump-active
单轮 Tot MIPS 为 PIN2 2450、PIN3 2410（ratio 0.9837）；这不是正式 A/B，只作为
形态负账的方向检查，性能裁定仍引用 W60 五对结果。

unit-local cache 把值交给普通 RA 时，不会永久拿走 x6–x9，也不会无条件扩大 helper
static-pin snapshot，因此能避开 PIN3 的全局缩池成本；但它会拉长 R12–R15 live
interval，并在观察边界物化，高压块仍可能增加本地 spill。它避开 W60 最坏机制，
却无法把 0.4–1.0% 的机械预算放大成原先的 8–18%。

## 5. 与 UniformElimination、W57 的差集和边界

- UniformElimination 已对 R12–R15 做 byte-fact/store→load 转发与 DSE，本次直接
  消掉 749 次 load；这是 unit-local cache 的“读合并”主体。
- W57 `UniformStoreSinkPass` 只接受 `xmm_uniform_ranges`，所以对 GPR 的直接覆盖
  是 0。它提供的是已验证的 store-delay 边界模型。当前尚未覆盖的无边界理想差集
  是 859 个额外 load + 248 个重复 store = 1107 条。
- `uniform_store_sink_pass.cpp:74-122,297-305` 要求在 guest memory/atomic、helper/
  X87/GetUniformAddress、UniformBarrier、Get/SetHostGPR/FPR、Set/GetLocation、局部分支、
  RSB 操作以及 unit terminal 物化。fault/SMC/sigreturn 和运行时信号恢复都读取 state。
- direct link 目标 unit 仍按“未 pin GPR 已提交到 uniform state”的既有契约编译；
  跨 unit 保留值需要 incoming-link/SMC invalidation/所有入口一致的新 ABI，已超出
  unit-local 边界。

W32 正是该风险的实测版本：目标 20 条消除，被 uniform 全表失效 +18、partial-write
bridge +48 吃掉，top-5 288→334、全语料 IR +2.96%、CoreMark -3.58%。本次 residual
仍有 U8/U16/U32 partial access；安全实现必须做旧高位合并或保守 flush，不能达到
本文故意高估的“全删”上限。

## 6. 决断与队列状态

不提出实现 spike：

1. 当前动态访问密度只有 1.2827/exit，较 W44 下降 83.03%。
2. 无边界静态上限 0.3987%；直接 sample 乐观上限 0.9877%，均小于 2%。
3. 实现必须重新承担 W32/W57 的 fault、helper、partial-write、CFG 和 direct-link
   契约，实际收益严格小于上述上限，并有新增 spill 风险。

因此 W44 最后一项关闭，队列清空。只有未来 pin 默认档位改变、执行探针显示
R12–R15 可删访问重新超过 2%，或 fixed-class RA 已作为独立架构重塑完成后，才值得
用新数据重开。

## 7. 工作树纪律

- 只新增 `docs/w72-gprcache-audit.md`。
- 未修改任何源代码、测试、golden、`source/translator/linux/linker/` 或
  `SwiftVM-bench/harness/run_matrix.sh`。
- 未重新生成 `func_fingerprint_golden.txt`。
- 未执行 git commit/push/add/checkout/reset/stash。
