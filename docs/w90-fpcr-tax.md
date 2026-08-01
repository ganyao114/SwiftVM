# W90：FPCR 边界税削减 spike

## 1. 结论

一期归因、MXCSR→FPCR 构造缓存、二期残税诊断和 helper 级产品化豁免均已完成。
`SVM_SSE_AFP_NAN` 仍默认 OFF；OFF 不发射缓存、helper effect 或探针相关代码。

实现前，c-ray/smallpt 的 AFP 边界机械静态税分别有 99.7810% / 99.1869% 来自 direct
helper。实现后，两项真实语料的 helper 返回缓存查询均为 **100% hit**：c-ray
4,808,127 / 4,808,127，smallpt 311,205 / 311,205；完整 MXCSR→FPCR 重建仅剩每次
JitRun 的入口初始化，分别为 9,209 / 2,211 次。探针机械动态估算分别下降
46.036579% / 45.724362%。

一期 Orb A/B 证明缓存使 smallpt 从显著回退回到 null，却没有改善 c-ray：c-ray 仍为
−5.03%，CI [−6.51%, −3.55%]。二期在 macOS 上用等长 NOP 因果对照与 1/1024
`CNTVCT_EL0` 采样进一步坐实：direct helper 的 host/guest FPCR 切换本身平均约
29.08 ns/次；完全跳过后，4.808M 次的累计估算 0.1398 s，与墙钟 ON→skip 恢复的
0.138 s 闭合。guard 删除确实改变热块尺寸和布局，但在保持 ON/skip 发码长度一致后，
skip 仍恢复到 OFF 水平，所以布局不是 c-ray 残税主因。

产品实现新增默认保守的 `HostFpEffect` 调用点契约；仅经源码和反汇编双重审计的
helper/action 显式标为 `FPCRTransparent`。AFP ON 下这些调用保留 guest FPCR，跳过整个
host/guest 切换对。最终 c-ray profile 中 4,807,926 次 direct helper 有 4,789,805 次
免切换，覆盖 **99.623102%**；剩余 cache lookup 仅 18,121 次（0.376898%），全部命中，
完整 rebuild 9,150 次且精确等于 runtime entry。相对一期缓存后的机械动态估算从约
33.8M 条降到 273,247 条。Orb 产品门全绿，独立 profile 复证 99.6229% helper 穿越
免切换；最终 A/B 中 c-ray 与 smallpt 均回到 CI 跨零的 null，STREAM Add/Triad/Scale
保留正收益。**翻盘条件齐备；本工作树仍维持默认 OFF，等待 orchestrator 合并、
双平台复验并另行执行默认 ON 与 golden 重生成。**

## 2. 测量探针

新增默认 OFF 的 `SVM_FPCR_TAX_PROF=<path>`，作为第 62 个环境变量。它不复用
`SVM_EXEC_PROF`，也不向 RegAlloc 增加固定 clobber：JIT 计数序列在栈上保存/恢复
`ip0/ip1`，避免改变待测 RA 形态。

计数存于每个 `RuntimeProfileInterface` 尾部的 Runtime 私有数组，Runtime 析构时才用
relaxed atomic 汇总到进程计数。探针开启时禁用磁盘 JIT cache，避免含进程私有计数
地址契约的代码跨进程复用。普通 dispatcher entry 也计数，作为“unit 调度本身不交
FPCR 税”的对照。

实现后探针还记录：

- `cache_lookups`：所有 guest-FPCR 恢复点；
- `cache_hits` / `cache_misses`：当前 `context.mxcsr` 与栈缓存 source 的比较结果；
- `rebuild_executed`：JitRun 入口初始化加所有 miss 的完整重建数。

探针自身的保存、计数和恢复指令不计入机械税估算。

## 3. 实现前归因

Release 构建：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

c-ray：

```sh
cd /private/tmp/w90-profile/cray
SVM_SSE_AFP_NAN=1 \
SVM_FPCR_TAX_PROF=/private/tmp/w90-profile/cray/fpcr.count \
/tmp/w64/build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/cray_x64 \
  /Users/swift/CLionProjects/SwiftVM-bench/src/c-ray/input/scene.json \
  -j 1 -s 8 -d 160x120 -o rendered.png --no-sdl
```

smallpt：

```sh
cd /private/tmp/w90-profile/smallpt
SVM_SSE_AFP_NAN=1 \
SVM_FPCR_TAX_PROF=/private/tmp/w90-profile/smallpt/fpcr.count \
/tmp/w64/build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/smallpt_wh_x64 64 320 240
```

两项均 rc=0 并生成非空图像。实现前原始计数：

| 边界 | c-ray events | c-ray dynamic est / 占比 | smallpt events | smallpt dynamic est / 占比 |
|---|---:|---:|---:|---:|
| runtime entry | 9,145 | 118,885 / 0.1898% | 2,211 | 28,743 / 0.7047% |
| runtime return | 9,145 | 18,290 / 0.0292% | 2,211 | 4,422 / 0.1084% |
| dispatcher entry | 20,744,556 | 0 / 0% | 36,147,516 | 0 / 0% |
| asm interpreter | 0 | 0 / 0% | 0 | 0 / 0% |
| trampoline CallHost | 0 | 0 / 0% | 0 | 0 / 0% |
| direct helper | 4,808,130 | 62,505,690 / **99.7810%** | 311,205 | 4,045,665 / **99.1869%** |
| MemoryCopy | 0 | 0 / 0% | 0 | 0 / 0% |
| StoreUniform(mxcsr) | 0 | 0 / 0% | 0 | 0 / 0% |
| **合计** | — | **62,642,865** | — | **4,078,830** |

dispatcher/runtime-entry 比分别为 2,268.404 和 16,348.944。普通 unit 返回在
`halt_reason == 0` 时直接回 `code_dispatcher`，不会离开 JitRun，也不会恢复/重装
FPCR。因此 W89 的 small-unit 回退并非“每 unit 一次税”，而是 helper 边界密度高。

## 4. 缓存机制

### 4.1 每 JitRun 独立栈帧

AFP runtime-entry 帧由 16 B 扩为 32 B：

| offset | 内容 |
|---:|---|
| `+0` | `host_fpcr`，进入 JitRun 时 `MRS FPCR` 保存 |
| `+8` | `guest_fpcr`，由 MXCSR 完整构造的缓存值 |
| `+16` | `source_mxcsr`，构造上述值时使用的 MXCSR |
| `+24` | 对齐/保留 |

入口从 `context.mxcsr` 完整构造一次 guest FPCR，写入后两项缓存再 `MSR FPCR`。退出
JitRun 时恢复 `host_fpcr` 并一次性弹出 32 B。缓存不跨 Runtime、不跨线程，也不跨
JitRun。

### 4.2 所有恢复点统一比较

`EmitSseAFPRestoreGuestFPCRCached` 是唯一的 host→guest AFP 恢复路径，覆盖：

- asm interpreter 返回；
- trampoline `CallHost` 返回；
- `EmitHostCall` direct helper 返回；
- `EmitMemoryCopy` 返回；
- `StoreUniform(mxcsr)` 即时同步。

每处均执行同一协议，无 helper/MemoryCopy 清洁性特例：

1. 从当前 JitRun 栈帧加载 `{cached_guest_fpcr, source_mxcsr}`；
2. 从 `context.mxcsr` 重新加载当前值并按 32 位比较；
3. 相等：直接 `MSR FPCR, cached_guest_fpcr`；
4. 不等：从已加载的当前 MXCSR 完整构造 guest FPCR，同步写回两项缓存，再 `MSR`。

`StoreUniform(mxcsr)` 使用 allocator 可见的三个临时 GPR，避免 XPOOL 与固定 ip clobber
冲突；它也走同一比较协议，写入新值时自然 miss 并更新缓存。signal 路径保持 W89 的
现有原子副本契约，完全不访问 JitRun 栈缓存。

### 4.3 静态净账

| 边界 | 实现前 AFP 专属指令 | 实现后 hit | miss |
|---|---:|---:|---:|
| runtime entry | 13 | 14（入口总是初始化） | — |
| runtime return | 2 | 2 | — |
| interpreter / CallHost / direct helper / MemoryCopy | 13 | 7 | 18 |
| StoreUniform(mxcsr) | 11 | 5 | 16 |

helper 热路径从 13→7，单次机械减少 46.153846%。miss 比旧路径多 5 条，代价来自比较、
缓存写回和控制流；因此必须由动态 hit 率证明，而不能只看单点静态形状。

## 5. 实现后复测

复现命令与 §3 相同，仅把输出路径改为 `fpcr-after.count`。结果：

| 指标 | c-ray | smallpt |
|---|---:|---:|
| runtime entry / return | 9,209 / 9,209 | 2,211 / 2,211 |
| dispatcher entry | 20,744,594 | 36,147,516 |
| direct helper | 4,808,127 | 311,205 |
| cache lookup | 4,808,127 | 311,205 |
| cache hit | 4,808,127 | 311,205 |
| cache miss | **0** | **0** |
| cache hit rate | **100.000000%** | **100.000000%** |
| rebuild executed | 9,209 | 2,211 |
| 机械动态估算 | 33,804,233 | 2,213,811 |
| 相对实现前 | **−46.036579%** | **−45.724362%** |

两轮独立运行的 c-ray entry 数有 9,145→9,209 的轻微运行间差异，helper 数仅差 3；
结论不依赖逐事件完全相同，因为实现后所有 4,808,127 次 helper lookup 都命中，且
`rebuild_executed == runtime_entry`。smallpt 两轮 entry/helper 数完全一致。

## 6. 定向正确性

扩展 `AFP guest FPCR is rebuilt from MXCSR across host-call boundaries`：

- 普通只读 helper 返回走 cache hit，helper 观察到精确 host FPCR；
- 第二个 helper 在 host FPCR 下直接把 `context.mxcsr` 从 round-up 改为
  round-to-nearest，返回后的 SSE add 得到 `0x3f800000`，证明统一比较捕获变化并走
  miss/rebuild；
- 随后的 DAZ、FTZ `StoreUniform(mxcsr)` 仍分别得到零结果；
- JitRun 返回后 host FPCR 逐位恢复。

用探针运行该用例得到 2 次 direct helper、3 次 StoreUniform、5 次 lookup、1 次 hit、
4 次 miss、5 次 rebuild（4 个 miss + 1 次入口），与用例结构精确相符。用例结果为
9 assertions / 1 case PASS。

语料输出 OFF/ON 对账：

| 语料 | 对账口径 | OFF | ON |
|---|---|---|---|
| smallpt | PPM MD5 | `e5b19a8c9ed08153f862f8eb11afe3b3` | 同左 |
| c-ray | PNG IDAT MD5（排除时间元数据） | `f9afd5bed804b5c2654303c9252d09c4` | 同左 |

## 7. macOS 验证

| 状态 | 结果 |
|---|---|
| 默认 OFF 全量 `swift_test` | PASS，140,325 assertions / 139 cases |
| `SVM_SSE_AFP_NAN=1` 全量 `swift_test` | PASS，140,325 assertions / 139 cases |
| 定向 helper-MXCSR cache miss | PASS，9 assertions / 1 case |
| helper effect / ConfigHash / X87 allowlist/target 定向 | PASS，66 assertions / 5 cases |
| transparent helper FPCR/FPSR 运行时观察 | PASS（包含在全量） |
| transparent helper 反汇编形状（mac Clang） | 8/8 helper symbols PASS |
| c-ray OFF / ON / unsafe-skip | PNG IDAT MD5 三态均 `f9afd5bed804b5c2654303c9252d09c4` |
| `git diff --check` | PASS |

当前 Codex 文件沙箱仍拒绝 Unicorn 2.1.4 的 `hw.cachelinesize` sysctl，后者会退到
`MRS CTR_EL0` 并在测试进程内 SIGILL。双态全量都使用 build 目录内、不进 git 的
DYLD interpose，只为 `hw.cachelinesize` 返回本机 128，并为 AFP capability probe
返回 1；两态条件完全相同。该测试环境绕行不在产品 diff 中。

## 8. 改动清单

- `source/runtime/backend/arm64/fpcr_mode.h`：32 B 帧布局、拆分完整构造器、统一缓存恢复器；
- `source/runtime/backend/arm64/trampolines.cpp`：入口初始化、退出恢复、interpreter/
  CallHost 缓存路径与探针；
- `source/runtime/backend/arm64/jit/translator_control.cpp`：direct helper 统一缓存路径；
- `source/runtime/backend/arm64/jit/translator_mem.cpp`：MemoryCopy 与 MXCSR store 统一缓存路径；
- `source/runtime/backend/arm64/jit/jit_context.{h,cpp}`：不改变 RA 形态的 JIT 私有计数；
- `source/runtime/common/fpcr_tax_prof.{h,cpp}`、`source/runtime/backend/context.h`、
  `runtime.cpp`、`jit_cache.cpp`、`CMakeLists.txt`：探针注册、私有汇总和 cache 隔离；
- `source/runtime/common/perf_stats.h`、`source/tests/main_case.cpp`：一期 env 61→62 与
  定向回归；二期诊断 env 62→64；
- `source/runtime/ir/args.{h,cpp}`、`source/runtime/frontend/ir_assembler.h`：新增默认保守、
  可与 uniform effect / preserve-all ABI 正交组合的 `HostFpEffect` 调用点元数据；
- `source/runtime/frontend/x86/decoder_alu.cc`、`decoder_sse42str.cc`：标注整数除法、
  Bsf/Bsr 与 Sse42StrStage；
- `source/runtime/frontend/x86/decoder_x87.cc`、`x87.{h,cpp}`、
  `source/runtime/backend/arm64/jit/translator_x87.cpp`：X87 action 级关闭白名单与专用
  fail-closed FP-free dispatch；
- `source/runtime/backend/arm64/jit/translator_control.cpp`：transparent direct helper
  跳过完整 FPCR 切换对，并增加产品覆盖计数；
- `source/runtime/backend/code_serial.cpp`：把 effective `Config::sse_afp_nan` 纳入
  `ComputeConfigHash`，覆盖不经环境变量构造 Config 的嵌入式调用者；
- `/private/tmp/w90-helper-shape-gate.sh`、`/private/tmp/w90-product-orb.sh`：不进 git 的
  GCC/Clang helper 形状审计与 Orb 全门脚本；
- `docs/w90-fpcr-tax.md`：本报告。

未改 signal handler、默认开关、linker、harness、golden 或 `docs/project-status.md`。

当前分支 `w90-fpcr-tax` 的**未提交工作树**共 24 个 tracked 文件、无 untracked 文件，
可按提交意图分为三层：

1. phase-2 诊断：`fpcr_tax_prof` 的 timing/unsafe-skip/target-phase sampler，
   `hot_coalesce_prof` 的 host layout/link 输出，以及相应的 JIT/Runtime 私有缓冲和 env
   注册；
2. phase-3 产品实现：`HostFpEffect` 元数据、helper/action 标注、X87 专用 fail-closed
   dispatcher、`EmitHostCall` transparent 快路、exact 覆盖计数和 effective AFP
   `ComputeConfigHash`；
3. 回归与报告：`main_case.cpp` 的契约/FPCR/FPSR/形状/ConfigHash/OFF-target 测试与本文。

`translator_control.cpp`、`fpcr_tax_prof.*`、`main_case.cpp` 同时承载诊断与产品验证，不能
按文件机械拆分；其余 diff 均落在 `source/runtime/`、`source/tests/main_case.cpp` 和本
报告范围内。工作树未包含 linker、harness、golden 或 project-status 改动。

## 9. 一期 Orb 门与墙钟实测

orchestrator 在 Orb 安静机执行一期正确性门，结果：

| 门 | 结果 |
|---|---|
| `swift_test` OFF / ON | 双态 PASS，142,861 assertions / 133 cases |
| helper-fault OFF / ON | 双态 38/38 |
| func_tests | 六格 rc=101，checksum `9f52b7d59285dbe5` 一致 |
| OFF fingerprint vs master | zero diff，含 `host_bytes` |
| ON fuzz | 固定五 seed 全过 |

同一二进制仅切 env、奇偶换序交错的缓存后 A/B：

| benchmark | ON 相对 OFF | 判定 |
|---|---:|---|
| STREAM Add | +11.74%，CI [+10.21%, +13.26%]，7/7 | 决定性胜 |
| STREAM Triad | +24.07%，CI [+22.62%, +25.53%]，7/7 | 决定性胜 |
| STREAM Scale | +3.03%，CI [+2.07%, +4.00%]，7/7 | 小胜 |
| smallpt | −0.57%，CI [−1.35%, +0.22%] | 回到 null |
| c-ray | **−5.03%**，CI [−6.51%, −3.55%]，0/7 | 显著回退未动 |

因此一期缓存有真实价值，但 c-ray 残税不能由“完整构造链仍在执行”解释：其 4,808,127
次 helper lookup 已 100% hit。以下为二期诊断。

## 10. 二期诊断设施

新增两个注册在 env 表末尾、默认 OFF 的诊断开关：

- `SVM_FPCR_TAX_SKIP_SWITCH=1`：**UNSAFE，仅因果诊断**。只在 AFP direct helper
  边界跳过 host FPCR 恢复与 guest FPCR 重装；启用时打印一次警告。为隔离布局因素，
  被跳过的 2 条 host-side 指令与 16 条 guest-side cache/miss 发码逐条替换为 NOP，
  ON 与 ON+skip 的 helper site 静态长度不变。它不进入默认或普通 ON 路径。
- `SVM_FPCR_TAX_TIMING=<path>`：每 1024 次 direct helper 采 1 次，读取
  `CNTVCT_EL0`，记录 `start/host_ready/helper_done/guest_ready`、helper target 和第 2
  参数。每个 Runtime 独占 8192 项缓冲，析构后汇总，无热路径原子竞争。

`SVM_RA_HOT_COALESCE` 的默认 OFF 输出扩展为每个翻译版本的 host 地址、allocation
offset、物理字节数、64/128/4096 对齐余数和最多四个 link target/delta。三项设施均仅在
显式诊断 env 下发码；普通 OFF 路径不变。

## 11. 诊断 A/C：切换因果对照与单次实测

### 11.1 `CNTVCT_EL0` 分段

macOS c-ray `-j 1 -s 8 -d 160x120`，每态约 4.808M 次 direct helper，采样 4,694
项，计数器频率 1 GHz：

| 分段 | 普通 ON mean | unsafe skip mean | 差值 |
|---|---:|---:|---:|
| start→host ready | 22.971 ns | 9.509 ns | 13.461 ns |
| helper body | 16.777 ns | 16.183 ns | 0.593 ns |
| helper done→guest ready | 17.212 ns | 2.191 ns | 15.021 ns |
| **总计** | **56.960 ns** | **27.884 ns** | **29.076 ns** |

两态都承担相同的四个时间戳和采样控制开销，故绝对总值包含探针税；差值才是切换的
因果估计。helper body 只差 0.59 ns，恢复来自边界而不是 helper 本体。这里产品路径的
host 侧是 `LDR cached_host_fpcr + MSR FPCR`，guest hit 侧是
`LDP cached_guest/source + LDR context.mxcsr + CMP/B + MSR FPCR`；路径中没有 MRS，
上述 MRS 仅为诊断读取 `CNTVCT_EL0`。

按 4,808,045 次的两态平均事件数计算，`29.075842 ns × 4,808,045 = 0.139798 s`。
这与同一批 macOS 墙钟 ON mean 1.908 s、skip mean 1.770 s 的 **0.138 s** 差值只差
约 1.8 ms。

### 11.2 等长墙钟因果对照

五组 OFF / ON / unsafe-skip 三态交错；ON 固定在中间，OFF 与 skip 每组换序：

| rep | OFF | ON | ON + unsafe skip |
|---:|---:|---:|---:|
| 1 | 1.78 s | 1.89 s | 1.76 s |
| 2 | 1.78 s | 1.91 s | 1.77 s |
| 3 | 1.88 s | 1.91 s | 1.78 s |
| 4 | 1.79 s | 1.92 s | 1.78 s |
| 5 | 1.75 s | 1.91 s | 1.76 s |
| median | **1.78 s** | **1.91 s** | **1.77 s** |
| mean | **1.796 s** | **1.908 s** | **1.770 s** |

逐对 ON/OFF 回退 median +7.26%；skip/OFF median −0.56%，回到同一水平；skip 相对
ON 五组全胜，median −7.29%。由于 ON/skip 的被测 helper site 等长，恢复不能归因于
删代码引起的块移动。

固定输入三态 PNG IDAT MD5 均为 `f9afd5bed804b5c2654303c9252d09c4`。这只说明该输入
恰好未触发 unsafe 语义差异，不构成省略切换的正确性证明。

结论：在 macOS 复现中，缓存命中后的 c-ray 残税由每次 direct helper 的真实 FPCR
切换对造成，而不是先前用总墙钟反推的约 458 ns 全都落在单次切换上。Orb 的绝对
2.2 s 差值还混入平台频率、运行尺寸或其他路径；需由 §15 的同二进制 Orb 诊断复验
确认，但因果机制已经在 macOS 上闭合。

## 12. helper 频率与可豁免表面积

二期 Orb 初版 1/1024 采样按 target RVA 聚合（orchestrator 复证数据）：

| helper | samples / 4,694 | 占比 |
|---|---:|---:|
| `X87Dispatch` | 2,501 | 53.28% |
| `DivQU64` + `DivRU64` | 2,054 | 43.76% |
| `Bsf64` + `Bsr64` | 120 | 2.56% |
| `RepMovs` + `RepStos8` | 19 | 0.40% |

该轮 `X87Dispatch` 再按 command（第 2 参数低 8 位）分解：

| X87 action | samples / 2,501 | 占 X87 |
|---|---:|---:|
| `LoadConstant` (15) | 841 | 33.63% |
| `Remainder` (11) | 825 | 32.99% |
| `StoreReg` (7) | 823 | 32.91% |
| `StoreControl` (19) | 12 | 0.48% |

这四类当前实现均为整数/SoftFloat 状态操作；尤其 `Remainder` 走 `extF80_rem` 和整数
商计算，不执行依赖 host FPCR 的 native FP。但后续审计发现，原 sampler 仅用全局
`calls % 1024`，会与周期性 helper 序列锁相并漏掉完整 target/action。采样相位加入
`target >> 4` 后，又稳定暴露 `LoadFloat`、`StoreFloat`、`StoreStatus` 三类热 action；
最终产品覆盖以无采样的 exact counter 为准，不再依赖上述样本占比。

`X87Dispatch` 仍不能整体标成 clean：
同一 target 的 `Transcendental` action 会调用 `sin/cos/tan/atan2/log/exp2` 等 host
double/libm。因而 target-only helper 注册表既会漏掉 c-ray 53.28% 的机会，也会把
transcendental 错误豁免；元数据必须允许 call-site/action 级分类。

## 13. 诊断 B：热块布局

OFF→ON 的 W37 guard 删除确实改变了发码布局。例如 top 热块：

| guest PC | OFF static / bytes / mod64 | ON static / bytes / mod64 | link delta 示例 |
|---|---|---|---|
| `0x402fed` | 303 / 5,212 / 32 | 267 / 4,588 / 48 | 17,088→12,704；5,552→4,608 |
| `0x403088` | 193 / 1,292 / 16 | 157 / 668 / 48 | 1,632→688；13,152→8,768 |
| `0x407450` | 446 / 3,184 / 0 | 340 / 1,400 / 0 | — |
| `0x457ad0` | 168 / 1,192 / 48 | 126 / 544 / 48 | — |

全局 layout summary 也有运行间轻微版本/边数差异，说明不能用两个独立进程的绝对地址
逐项相等作为门。关键控制实验是普通 ON 与 NOP-preserved skip：top-20 共同热块的
`host_bytes`、`host_static`、`nan_static`、mod64 全部相同；19/20 个已解析 target delta
相同，唯一外部 target 相差 192 B，属于独立运行的翻译/分配次序差异。即便如此，skip
仍从 1.908 s 恢复到 1.770 s。

结论：guard 删除会移动/缩小热块，布局是假说成立的“伴随变化”，但不是本次残税的
主因。等长切换消融已把布局保持在 ON 形状而恢复全部可见回退。

## 14. helper 级产品化实现

### 14.1 标注模型与防漂移

IR `Lambda` 新增与 uniform effect、W73 helper ABI 正交的 `HostFpEffect`：

- `MayTouch` 的枚举值固定为 0，也是 `HelperCallTraits` 默认值。所有既有、新增、动态
  target 和未显式标注调用点自动走保守路径；
- `FPCRTransparent` 必须由调用点显式 opt-in，表示该目标及当前 action 不执行 host
  FP/NEON、不读写 FPCR/FPSR、不写 `context.mxcsr`；
- 三类元数据共用 `Lambda` tagged-immediate 的独立 bit，组合测试同时断言 uniform-pure、
  preserve-all 与 transparent 均可读回，避免一类标签覆盖另一类。

X87 另设关闭白名单 `X87ActionFPCRTransparent`；不认识的 action 和所有未列 action
一律 false。transparent action 调到专用 `X87DispatchFPFree`，入口再次检查 command；
误路由时直接返回 guest-fault 标志，绝不在 guest FPCR 下尾调保守 dispatcher。测试遍历
当前全部 action，逐项钉死恰好七个 true，并验证保守 action fail closed。

### 14.2 完整 FPCR-transparent 清单与证据

| helper / action | 标注粒度 | 判定依据 |
|---|---|---|
| `DivQU64`, `DivRU64`, `DivQS64`, `DivRS64` | 调用点 | 只用 `u64/s64` 与 `unsigned/signed __int128` 除余；无 FP 类型、intrinsic、NEON 或 FP 状态访问 |
| `Bsf64`, `Bsr64` | 调用点 | 只用整数 `__builtin_ctzll/__builtin_clzll`；并与 preserve-all ABI 标签组合 |
| `Sse42StrStage` | 调用点 | 仅把三个 GPR 值写 thread-local staging struct 并返回整数 token；真正使用整数 NEON 的 `Sse42StrEval` 未标注 |
| X87 `LoadFloat` | action | guest f32/f64/m80 位模式经 SoftFloat `extFloat80_t` 路径装载；无 native FP |
| X87 `StoreFloat` | action | SoftFloat 转换和整数 guest store；C1/pop 均为整数状态操作 |
| X87 `StoreReg` | action | ext80 结构复制、tag/FSW 整数更新 |
| X87 `Remainder` | action | `extF80_rem` 与整数商/condition-code 计算 |
| X87 `LoadConstant` | action | 预编码 ext80 常量表与整数栈状态更新 |
| X87 `StoreControl`, `StoreStatus` | action | 16-bit guest store或整数返回，无 FP 执行 |

没有标注 `RepMovs/RepStos8`：虽为整数候选，但当前 c-ray 动态面仅约 0.4%，且本轮没有
为其建立独立专用实现/完整形状证据，按默认保守规则退出。所有其他 X87 action 也保持
保守；尤其 `Transcendental` 明确进入 host double/libm，`Binary/Unary/Scale/Extract`
等即便源码使用 SoftFloat，也未在本轮收益面内完成独立可达体形状证明。

mac Clang 对上述 8 个实际 helper symbol（X87 专用 dispatcher、四个 Div、Bsf/Bsr、
Sse42StrStage）做函数范围反汇编，拒绝 FP/NEON 寄存器、FP mnemonic、FPCR/FPSR，结果
8/8 PASS。Orb 脚本对 GCC 13 产物执行同一语义门；不比较两编译器逐字节产物，因为
整数指令选择可以合法不同。

### 14.3 emit 与正确性边界

`EmitHostCall` 仅在 `effective sse_afp_nan && direct-target && FPCRTransparent` 时跳过
整个 `LDR host_fpcr + MSR FPCR` 及返回侧 compare/cache restore/MSR；寄存器 snapshot、
参数/返回值、fault sink 与 helper ABI 均不变。interpreter、trampoline CallHost、
MemoryCopy、StoreUniform(mxcsr)、未知/动态 helper 以及所有保守 X87 action 继续走原
切换与 cache 协议，没有扩大豁免。

X87 专用 dispatcher 的 target 选择也受 effective AFP 门控，而不只是 effect 消费受
门控；否则 OFF 虽不会跳过 FPCR，却会把另一个函数地址物化进 host code，破坏 OFF
指纹。FLD1 定向测试分别以 effective=false/true 解码，钉死 OFF 为原
`X87Dispatch + MayTouch`、ON 为 `X87DispatchFPFree + FPCRTransparent`。

signal 契约保持不变：整个 JitRun 期间 `jit_guest_fpcr_active=true`，并在进入前以原子
副本发布 host FPCR。保守 helper 被打断时硬件已经是 host FPCR，signal handler 的恢复
是幂等的；transparent helper 被打断时硬件仍是 guest FPCR，handler 先从原子副本恢复
host FPCR 再运行 C++ fault/SMC 回调。`sigreturn` 恢复被打断的 ucontext，因此前者回到
host、后者回到 guest，精确延续各自调用点语义；handler 不访问 JitRun 栈缓存。

定向运行时用非默认 host FPCR/FPSR 进入 JIT，让 transparent helper 直接观测：helper
看到从 MXCSR 构造的 guest FPCR，FPSR 未被污染；JitRun 返回后 host FPCR/FPSR 逐位
恢复。形状测试则对同一 call site 只切换 trait，断言保守路径恰有两条 `MSR FPCR`、
transparent 路径为 0，且两边都保留一个间接调用。

### 14.4 磁盘 cache 隔离

产品标注改变 AFP ON 发码。CLI 路径原本由 `ComputeEnvHash` 的原始
`SVM_SSE_AFP_NAN` 值隔离，但 programmatic embedder 可以直接构造 `Config`，没有 env
值可依赖。因此 `ComputeConfigHash` 现在显式哈希 **effective**
`Config::sse_afp_nan`；capability 不成立时该值仍 false，不制造伪 ON cache 分片。

构造性路径为：`JitDiskCache::Key()` 直接把 `ComputeConfigHash(address_space.GetConfig())`
写进 `ValidityKey.config_hash`，文件名和载入校验均使用该字段。程序化测试构造除
`sse_afp_nan` 外逐字段相同的两个 Config，断言 hash 不同；把 ON Config 重置为 false
后再断言 hash 完全相同。该测试不依赖环境变量，直接覆盖本次漏口。

### 14.5 产品 profile 静态/动态净账

最终 mac c-ray exact profile：

| 指标 | 结果 |
|---|---:|
| direct helper | 4,807,926 |
| transparent helper | 4,789,805 |
| **免切换覆盖率** | **99.623102%** |
| 保守 helper / cache lookup | 18,121 / 18,121 |
| cache hit / miss | 18,121 / 0 |
| rebuild executed / runtime entry | 9,150 / 9,150 |
| dispatcher entry | 20,744,248 |
| 机械动态估算 | **273,247** |

相较 §5 一期缓存后的 33,804,233，估算再降 99.191678%；相较 §3 原始 62,642,865，
累计下降 99.563802%。该数只计算 FPCR 边界发码的机械指令，不等价于墙钟收益；最终
墙钟结果见 §16。

## 15. 二期验证与 Orb 复验

二期最终诊断版本 macOS：

| 状态 | 结果 |
|---|---|
| 默认 OFF 全量 `swift_test` | PASS，140,252 assertions / 133 cases |
| `SVM_SSE_AFP_NAN=1` 全量 `swift_test` | PASS，140,252 assertions / 133 cases |
| `git diff --check` | PASS |

Orb 复验由 orchestrator 在同一二进制仅切 env 的条件下完成：

| 状态 | mean wall |
|---|---:|
| OFF | 1.8905 s |
| AFP ON | 1.9995 s |
| AFP ON + NOP-preserved unsafe skip | 1.8635 s |

三态 PNG IDAT 逐位一致。Orb `CNTVCT_EL0` 差值同样为 29.076 ns/次；
`29.076 ns × 4.808M = 0.1398 s`，与该批墙钟 ON→skip 的约 0.138 s 恢复闭合。
ON/skip top 热 unit 的地址相对布局、尺寸、对齐及 link 分布无可解释差异，排除布局
主因。初版样本中 helper 分布为 X87 53.28%、Div 43.76%，与 mac 机制一致；§12 记录
了 sampler 锁相修复及随后补出的 X87 action。

因此二期 Orb 复证也把真凶限定为 direct helper 两侧的 FPCR `MSR` 切换对，而不是
MXCSR 构造指令数或 guard 删除后的 block 布局。诊断开关仍默认 OFF，且不进入产品
正确性路径。

## 16. 产品化 Orb 门与最终 A/B

### 16.1 Orb 产品门实测

orchestrator 在 Orb 上执行 `/private/tmp/w90-product-orb.sh`，全部通过：

| 门 | 结果 |
|---|---|
| `swift_test` OFF / ON | 双态 PASS，**142,934 assertions / 139 cases** |
| helper-fault OFF / ON | **38/38 × 2** |
| func_tests | 六格 rc=101，checksum `9f52b7d59285dbe5` 逐位一致 |
| OFF fingerprint vs master | **zero diff**，4,400 units / 11 guests，含 `host_bytes` |
| ON fuzz | 固定五 seed 全过 |
| GCC 13 helper 反汇编形状 | **8/8 PASS** |

GCC 形状门覆盖 `X87DispatchFPFree`、`DivQU64`、`DivRU64`、`DivQS64`、
`DivRS64`、`Bsf64`、`Bsr64`、`Sse42StrStage`；与 mac Clang 的 8/8 结果一致，但门只
比较“无 FP/NEON/FPCR/FPSR”语义，不要求两个编译器选择相同整数指令。

Orb c-ray exact profile 复证：

| 指标 | 结果 |
|---|---:|
| direct helper | 4,808,031 |
| transparent helper | 4,789,899 |
| **免切换覆盖率** | **99.6229%** |
| 保守 helper / cache lookup | 18,132 / 18,132 |
| rebuild executed / runtime entry | **9,188 / 9,188** |

覆盖率与 mac 99.623102% 一致，且完整 rebuild 再次精确等于 JitRun 入口数，产品路径
不存在额外 MXCSR→FPCR rebuild 风暴。

### 16.2 最终墙钟 A/B

测量由 orchestrator 在 Orb 安静机上执行：同一二进制仅切换 env，奇偶换序交错配对；
正值表示 AFP ON 更快。

| benchmark | ON 相对 OFF | 95% CI | 胜场 | 判定 |
|---|---:|---:|---:|---|
| STREAM Copy | +0.96% | [+0.47%, +1.44%] | — | null / 系统偏置锚点 |
| STREAM Scale | +2.49% | [+0.96%, +4.01%] | 6/7 | 小胜 |
| STREAM Add | **+11.69%** | **[+10.36%, +13.02%]** | 7/7 | 决定性胜 |
| STREAM Triad | **+22.98%** | **[+21.52%, +24.44%]** | 7/7 | 决定性胜 |
| c-ray | +1.43% | [−0.34%, +3.21%] | 5/7 | **CI 跨零，回退消除** |

STREAM Copy 的两态代码逐指令相同，却测得 +0.96%，说明本次会话存在约 ±1% 的系统
偏置；因此所有绝对值不超过 1% 的结果都应视为误差带内，而不能解释成产品收益或回退。
Add/Triad 远超该底噪，方向和 W89 guard 静态账一致；Scale 是较小正收益，其 CI 下沿
刚好位于该系统误差带边缘。

smallpt 在 Orb VM 上对约 20 s 级负载存在明显瞬时抖动，三组独立配对结果为：

| 组 | 配对数 | ON 相对 OFF | 95% CI | 说明 |
|---|---:|---:|---:|---|
| 1 | 5 | −0.57% | [−1.35%, +0.22%] | CI 跨零 |
| 2 | 5 | −3.42% | [−8.23%, +1.39%] | 含一个约 +11% 离群，CI 跨零 |
| 3 | 9 | +5.69% | [−2.90%, +14.29%] | 含约 +17% / +31% 两个 ON 离群；剔除后 +0.37% |

三组 CI 全部跨零，方向也不稳定；结合离群、Copy 暴露的 ±1% 系统偏置和剔除离群后
+0.37%，裁定 smallpt 为噪声内 null，不再存在可复现回退。

### 16.3 决断

翻盘条件全部满足：

- c-ray 从 −5.03% 显著回退恢复为 CI 跨零；
- smallpt 三组均为 CI 跨零的 null；
- STREAM Triad/Add/Scale 的 CI 下沿分别为 +21.52% / +10.36% / +0.96%，其中
  Triad/Add 明确超过既定 +15% / +5% 保留门；
- 双平台现有正确性门、OFF 指纹、helper 形状和 99.6% 覆盖门均通过。

**结论：`SVM_SSE_AFP_NAN` 已具备翻默认条件。当前工作树仍维持默认 OFF，等待
orchestrator 合并和双平台复验后执行默认 ON。** AFP ON 会改变指纹语料发码，翻转时
必须按新默认重新生成并复验 golden；该动作不属于本工作树，也未在此执行。
