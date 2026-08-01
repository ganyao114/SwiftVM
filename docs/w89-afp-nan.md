# W89：SVM_SSE_AFP_NAN spike

## 1. 结论

P0、P1 均已完成，macOS 与 Orb 正确性门全绿，性能 A/B 已完成。
`SVM_SSE_AFP_NAN` 已注册为默认 OFF 的第 60 个环境变量；
只有“用户请求 ON”且宿主能力位包含 `Arm64Features::AFP` 时，派生的 effective-enable
才为真。无 AFP 时静默保留原 W37 guard 路径并只打印一次诊断。

P0 没有删除、缩短或改写任何 NaN guard/cold veneer，且 Orb 全门已通过。P1 只在
backend 发码点按 IR opcode + lane shape 白名单放行 Add/Sub/Mul/Div/Sqrt 的
scalar/packed f32/f64；FMA、MIN/MAX、COMIS、RCP/RSQRT 的 lowering 逐指令保持不变。
macOS 最终代码 OFF/ON 全量均通过 140,188 assertions / 131 cases；Orb 均通过
142,797 assertions / 131 cases。STREAM 实测静态账：
Triad 热 unit `nan_static 10→0`，Scale/Add 各 `5→0`；这些只是可复现的机械发码删减，
不写成墙钟收益。Orb 配对实测显示 STREAM Scale/Add/Triad 分别提升
2.14%/10.70%/22.92%，但 smallpt/c-ray 分别回退 1.09%/4.71%，且后两项置信区间
不跨零；因此开关维持默认 OFF，只作为流式/长循环 FP 密集负载的显式 opt-in。

## 2. capability 与 policy 隔离

`DetectArm64Features()` 现在始终发布硬件事实：

- Linux AArch64：`getauxval(AT_HWCAP2) & HWCAP2_AFP`，缺失头文件宏时采用 UAPI
  的 `1UL << 20`；
- macOS AArch64：`sysctlbyname("hw.optional.arm.FEAT_AFP")`；
- `Config::arm64_features` 保存 capability；`Config::sse_afp_nan` 保存
  requested && capable；原 W23 scalar-insert policy 独立保存在
  `Config::sse_scalar_insert`。

这层拆分很重要：Linux 新识别 AFP 不会顺带打开原先只在 macOS 默认 ON 的
scalar-insert lowering；`SVM_SSE_AFP_NAN` 未设置时也不会进入任何 P0 发码分支。

磁盘缓存不另加版本字段。`ComputeConfigHash()` 已哈希 `arm64_features`，
`ComputeEnvHash()` 已按名字排序哈希全部 `SVM_*`/`SWIFT_*` 原值，所以实际
X86Instance 的 capability 和新环境开关都进入 JIT/AOT key。新增 effective bool
只是这两项的确定性派生量。

## 3. guest FPCR 构造

新增共享 emitter `EmitSseAFPGuestFPCR()`，从零开始构造完整 guest FPCR，而不是
读取/继承 caller FPCR：

| guest FPCR 字段 | 来源 |
|---|---|
| AH[1] | 固定 1 |
| NEP[2] | 固定 1 |
| DN[25] | 固定 0 |
| FIZ[0] | MXCSR.DAZ[6] |
| FZ[24] | MXCSR.FTZ[15] |
| RMode[23:22] | MXCSR.RC[14:13]，down/up 编码交叉映射 |
| trap/reserved | 固定 0，不继承 host |

x87 rounding control 没有参与这条构造链；SSE MXCSR 与 x87 FCW 继续各自独立。
P0 也没有扩大项目当前对 MXCSR exception sticky/trap 的既有合同。

## 4. 边界闭环

| 边界 | 离开 JIT 前 | 回到 JIT 后 |
|---|---|---|
| runtime entry / unit exit | entry 保存 caller FPCR；return C++ 前恢复 | entry 从当前 MXCSR 构造 guest FPCR |
| asm interpreter | 从 runtime-entry 栈槽恢复 host FPCR | 从 interpreter 可能更新后的 context MXCSR 重建 |
| `EmitHostCall` direct helper | helper snapshot 完成后恢复 host FPCR | 保留返回值后从 context MXCSR 重建，再恢复寄存器 |
| trampoline `CallHost` | 写回静态 guest 状态后恢复 host FPCR | C++ 返回后重建，再恢复静态状态 |
| `EmitMemoryCopy` / `HostMemMove` | 该路径绕过 `EmitHostCall`，单独从 runtime-entry 栈槽恢复 | memmove 返回后重建 |
| SIGSEGV/SIGBUS、SMC callback | runtime 在进入 JitRun 前发布 lock-free host-FPCR 副本；callback 首先恢复 | sigreturn 保留被中断的 guest ucontext；fault-return trampoline 最终恢复 host |

signal 路径的 `std::atomic<u64>` 带 `is_always_lock_free` 编译期断言；host FPCR 先写、
active 标志再 release 发布，handler 用 acquire 读取。这样 fault 发生在普通 JIT、
direct helper 或 interpreter 内都不会让 C++ handler 带着 AH/NEP/DN/RMode guest 状态运行。

## 5. 动态 MXCSR 写

- `StoreUniform(ThreadContext64::mxcsr)` 在 AFP effective 状态下立即用三个
  allocator-visible scratch GPR 重建 FPCR。使用动态 scratch 而非固定 x11/x16/x17，
  保持默认 XPOOL 的 live-value/clobber 契约。
- LDMXCSR 和 FXRSTOR 在完成写入后记录 next PC 并 `ReturnToDispatcher`；即时同步后
  不再执行同一旧控制流 unit。
- XRSTOR helper 成功后从其已更新的 context 重建，再退到 next-PC dispatcher；fault
  检查仍先执行，失败路径保持原 PageFatal 顺序。
- STMXCSR、FXSAVE、XSAVE 不改变 MXCSR，不触发退 unit。

这采用 W87 建议的最保守分级；六语料动态写为零，所以没有为罕见写路径设计热快路。

## 6. 定向测试

`source/tests/main_case.cpp` 的 P0 新增五个 case、86 个 macOS assertions：

1. 直接 backend 执行：host 预置 DN/FZ/不同 RMode，guest MXCSR 设 round-up；
   `1.0f + 2^-24` 在 helper 前后均得到 `0x3f800001`，helper 观察到原 host FPCR，
   unit 返回后 host FPCR 逐位恢复。随后同一 unit 写 DAZ/FTZ，输入 flush 和输出
   flush 都得到 0。默认 XPOOL 下通过。
2. W87 的 64 格真值矩阵：8 种 QNaN/SNaN/finite 操作数组合 ×
   Add/Sub/Mul/Div × scalar f32/f64，全部经 x86 decoder/JIT guest 执行；OFF/ON
   都逐位匹配 x86 的 operand-1 priority、SNaN quiet payload/sign，并保持 scalar
   upper lanes。
3. 端到端 x86 guest：`LDMXCSR(round-up); ADDSS; HLT`。OFF 得
   `0x3f800000`，ON 得 `0x3f800001`；同时断言
   `effective == requested && capability`，以及 Apple silicon capability 非零，防止
   ON 靠 fallback 假绿。
4. decoder 结构门：LDMXCSR、FXRSTOR、XRSTOR 各自都以 next-PC
   `SetLocation + ReturnToDispatch` 结束。
5. `MemoryCopy` 发码门：唯一 `BLR HostMemMove` 前后恰有两次 `MSR FPCR`，覆盖其
   绕过 `EmitHostCall` 的特殊路径。

单测命令与结果：

```text
./build/source/tests/swift_test 'AFP guest FPCR is rebuilt*'       8/1 PASS
./build/source/tests/swift_test 'AFP translated SSE arithmetic*'  65/1 PASS (OFF、ON)
./build/source/tests/swift_test 'AFP environment gate applies*'   5/1 PASS (OFF、ON 各跑一次)
./build/source/tests/swift_test 'AFP mode terminates units*'       6/1 PASS
./build/source/tests/swift_test 'AFP mode brackets direct*'        2/1 PASS
SVM_JIT_SCRATCH_XPOOL=1 ... 'AFP guest FPCR is rebuilt*'           8/1 PASS
```

P1 再新增两个 backend 发码结构 case、127 个 assertions：

1. allowlist 矩阵覆盖 Add/Sub/Mul/Div/Sqrt × f32/f64 × scalar/packed；scalar 同时覆盖
   Linux 使用的 legacy merge 和 macOS scalar-insert tied 两条路径。每个 shape 都对比
   AFP OFF/ON 反汇编，要求 ON 指令数严格减少，且 scalar 的 `FCMP+B.vs`、packed-f32
   的 `FCMEQ+UMINV+FMOV+CBZ`、packed-f64 的 `FCMEQ+2×UMOV+AND+CBZ` 消失。
2. exclusion 矩阵覆盖 FMA f32/f64、MIN/MAX scalar/packed f32/f64、COMIS f32/f64、
   RCP/RSQRT scalar/packed。AFP OFF/ON 的 host 指令数和完整反汇编文本必须完全相同，
   证明 P1 没有因同属 FP lowering 而误放行相邻 opcode。

结果：`./build/source/tests/swift_test 'AFP P1*'` 为 127 assertions / 2 cases PASS；
64 格 x86 真值矩阵在最终代码 OFF/ON 各为 65 assertions / 1 case PASS。全量中已有
AVX SQRT NaN/sign/payload 对拍继续覆盖 P1 的 FSQRT 直发语义。

## 7. P1 发码机制与静态账

`JitTranslator::UseAFPNaN()` 是唯一 P1 判定点：

- scalar binary 由八个精确 IR opcode 区分 f32/f64；
- packed binary 只接受 `VecFAdd/Sub/Mul/Div` 且 lane immediate 为 32 或 64；
- unary 只接受 `VecFUnary(kind=0)` 的 SQRT，lane 为 32/64、scalar immediate 为 0/1；
- switch 的 default 明确回退，FMA、MIN/MAX、COMIS、RCP/RSQRT 不可命中。

二元族命中时，既跳过 `EmitVecFloatNaNFixup()`，也跳过
`PreserveNaNColdSource()` 的 alias/cold-ABI 副本；SQRT 命中时直接发对应 scalar/packed
`FSQRT`。OFF、无 AFP capability 和所有排除项仍走原 W37 cold/inline correction。

静态复现命令：

```sh
DYLD_INSERT_LIBRARIES=$PWD/build/libw89_sysctl_shim.dylib \
  SVM_RA_HOT_COALESCE=/private/tmp/w89-stream-off.count \
  ./build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/stream_x64
DYLD_INSERT_LIBRARIES=$PWD/build/libw89_sysctl_shim.dylib \
  SVM_SSE_AFP_NAN=1 SVM_RA_HOT_COALESCE=/private/tmp/w89-stream-on.count \
  ./build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/stream_x64
```

两态均打印 `Solution Validates`。计数文件的同 guest PC 对照：

| STREAM unit | OFF host_static / nan_static | ON host_static / nan_static |
|---|---:|---:|
| Scale `0x401a70` | 45 / 5 | 40 / 0 |
| Add `0x401b00` | 48 / 5 | 43 / 0 |
| Triad `0x401ba0` | 56 / 10 | 46 / 0 |

全程序 entry-weighted `nan_guard_dynamic` 为 8,640,001,800→0。该探针口径是
block entry × 静态 host 指令数，包含一次性 tail，不是 retired instructions 或 cycle；
因此只作为 P1 发码验收，不作为性能 A/B。

## 8. macOS 全量

| 状态 | 结果 |
|---|---|
| `SVM_SSE_AFP_NAN` unset | PASS，140,188 assertions / 131 cases |
| `SVM_SSE_AFP_NAN=1` | PASS，140,188 assertions / 131 cases |
| helper-fault OFF | PASS，38/38 |
| helper-fault ON | PASS，38/38 |
| func_tests 六格 | PASS，function/block/interpreter × OFF/ON 均 rc=101，checksum `9f52b7d59285dbe5` |
| `git diff --check` | PASS |

另做了一次 macOS golden 对拍：两遍 self-consistency 为 4,400 units 且含
host_bytes 一致；但当前文件沙箱令门脚本的外部 `sysctl` 选择
`darwin-noflagm` shard，而 translator 进程内 `sysctlbyname(FEAT_FlagM)` 成功，故命中
W83 已知的 shard-selector 环境不一致：`real_busy:0x439970`、
`func_tests:0x419c10` 各为 48→45 IR。它不是本轮 OFF-vs-master 门；权威的 Orb
同平台 `--against /tmp/svm-build` 已通过 4,400 units / 11 guests zero diff，包含
host_bytes。

当前 Codex 文件沙箱拒绝 `sysctlbyname("hw.cachelinesize")`。Homebrew Unicorn 2.1.4
随后退到 `MRS CTR_EL0`，在 `init_cache_info+88` 自身 SIGILL；crash report 明确栈为
`init_cache_info -> machine_initialize -> uc_init_engine -> uc_mem_map`，尚未进入 SwiftVM。
为运行完整 Unicorn suite，测试命令用 build 目录内、不进 git 的 DYLD interpose shim
只读返回本机 cache line 128，并向 capability probe 返回 Apple silicon 已知存在的 AFP；
其余 sysctl 仍委托 `sysctlnametomib + sysctl`。OFF/ON 都使用相同 shim。P0 产品代码和
最终建议不包含该测试环境绕行。

实际全量命令：

```sh
DYLD_INSERT_LIBRARIES=$PWD/build/libw89_sysctl_shim.dylib \
  ./build/source/tests/swift_test
DYLD_INSERT_LIBRARIES=$PWD/build/libw89_sysctl_shim.dylib \
  SVM_SSE_AFP_NAN=1 ./build/source/tests/swift_test
```

## 9. Orb 门

Orb 首次 GCC 13 构建在 `source/tests/main_case.cpp` 的新增 `MemoryCopy` 发码门停止：
`0x2000ull`/`0x1000ull`/`16u` 在 Linux 类型别名下无法唯一选择 `Imm` 的整数重载。
当时已把 P0 diff 中全部新增的 `Imm{}`/`Lambda{}`/`Value{}` 构造逐项审计；5 个逻辑
调用点合计 7 个 `Imm` 构造和 3 个 `Lambda` 包装，均改用显式
`swift::u32`/`swift::u64`，没有新增 `Value{}` 构造。修后 macOS Clang 重编译通过，
Orb GCC 13 重建也已验证。

P1 首次 Orb 门只有排除项形状测试在 `source/tests/main_case.cpp:3115` 假失败。
ON/OFF 指令数和助记符序列已经相等，完整文本的唯一差异是
`b.mi #+0x8 (addr 0xaaab04b25cc8)` 与
`b.mi #+0x8 (addr 0xaaab04b2ed98)`：VIXL 在反汇编中嵌入 code buffer 绝对地址，
Linux 上两次编译因分配地址/ASLR 不同而不能逐字符比较。修复新增
`NormalizeDisasmForCompare`，只把 `(addr 0x<hex>)` 地址注释替换为固定占位符；
`#+0x8`、`#0x123` 等有语义的相对偏移和立即数原样保留，并用新增自断言固定该契约。
这也解释了最终全量比修复前预期多 1 个 assertion。产品 lowering 没有因该失败改动。

最终 `/private/tmp/w89-p1-orb.sh` 由 orchestrator 在 Orb Ubuntu/GCC 13 执行。
脚本使用 `/tmp/w64/build`，仅以 `/tmp/svm-build` 为 OFF fingerprint 对照；命令为：

```sh
bash /private/tmp/w89-p1-orb.sh
```

最终实测结果：

- swift_test OFF/ON 均为 142,797 assertions / 131 cases，零失败；
- `AFP translated SSE arithmetic*` OFF/ON 各为 65 assertions / 1 case PASS；
  `AFP P1*` 为 127 assertions / 2 cases PASS；
- helper-fault OFF/ON 均为 38/38；
- func_tests function/block/interpreter × OFF/ON 六格均 rc=101，分节哈希逐位一致，
  checksum `9f52b7d59285dbe5`；
- OFF fingerprint 对 `/tmp/svm-build` zero diff：4,400 units / 11 guests，包含
  host_bytes；
- ON 固定五 seed fuzz 全过；
- STREAM OFF/ON 均打印 `Solution Validates`，Scale/Add/Triad 的 nan_static 自动断言
  `5/5/10→0/0/0` 全过。

## 10. 性能 A/B 实测

orchestrator 在 Orb 上使用同一最终 translator 二进制，仅通过环境变量切换
`SVM_SSE_AFP_NAN=0/1`；每对奇偶次序交错，以抵消随时间漂移。下表百分比为 ON 相对
OFF，正值表示 ON 更快；区间为配对差的 t 95% CI。这些是墙钟实测，与 §7 的机械静态
删减口径分开。

| benchmark | mean | median | 95% CI | ON 胜场 |
|---|---:|---:|---:|---:|
| STREAM Copy | −0.75% | −1.10% | [−2.99%, +1.48%] | 3/7 |
| STREAM Scale | +2.14% | +1.98% | [+0.92%, +3.36%] | 6/7 |
| STREAM Add | +10.70% | +11.68% | [+8.71%, +12.68%] | 7/7 |
| STREAM Triad | +22.92% | +22.91% | [+21.15%, +24.70%] | 7/7 |
| smallpt | −1.09% | −0.96% | [−2.16%, −0.03%] | 0/5 |
| c-ray | −4.71% | −5.20% | [−7.59%, −1.82%] | 0/5 |

Copy 的两态代码逐指令相同，fingerprint 门也已证明 OFF 没有发码漂移；其区间跨零，
实测为 null，因而构成这套配对方法的锚点。Scale 是小幅正收益；Add 和 Triad 则是
决定性提升，7/7 同向且区间远离零，与热 unit 静态账 `48→43`、`56→46` 的方向一致。

smallpt 与 c-ray 不是可忽略的噪声，而是真实小回退：两者均为 5/5 同向，95% CI
不跨零。它们的 unit 较小，dispatcher/helper 边界穿越频繁，P0 为正确性所需的 host
FPCR 保存与 guest FPCR 重建税超过 P1 删除 NaN guard 的收益。STREAM 的热循环在单个
unit 内长迭代，边界税被摊薄到近零，而 guard 删除的收益按迭代累积，因此 Add/Triad
能取得明显净收益。

据此不翻默认：`SVM_SSE_AFP_NAN` 继续默认 OFF，定位为流式、长循环、FP 密集负载的
显式 opt-in。后续可另立任务评估“MXCSR 未变化时跳过 FPCR 重建”以削减边界税，再决定
是否具备翻默认条件；本报告不承诺该优化。

## 11. 改动文件

- `source/runtime/include/config.h`、`source/translator/x86/translator.cpp`：capability、
  effective policy 与实例配置；
- `source/runtime/backend/arm64/fpcr_mode.h`：共享 MXCSR→FPCR 构造及 native FPCR
  读写；
- `source/runtime/backend/arm64/trampolines.cpp`、`runtime.cpp`：entry/exit、
  interpreter、CallHost、fault/SMC 边界；
- `source/runtime/backend/arm64/jit/translator.{h,cpp}`、`translator_control.cpp`、
  `translator_mem.cpp`：effective flag、direct helper、MemoryCopy、MXCSR StoreUniform；
- `source/runtime/backend/arm64/jit/translator_alu.cpp`：P1 opcode+shape allowlist，命中时
  跳过 binary cold source/fixup 与 SQRT veneer；
- `source/runtime/frontend/x86/decoder.{h,cc}`、`decoder_sse.cc`、
  `decoder_dispatch.cc`、`decoder_xsave.cc`、`xsave.h`：动态 restore 退 unit；
- `source/runtime/common/perf_stats.h`：环境注册表 59→60；
- `source/tests/main_case.cpp`：注册表、五个 P0 定向 case、两个 P1 发码矩阵 case，以及
  跨平台反汇编地址归一化 helper；
- `docs/w89-afp-nan.md`：本报告。

W37 guard/cold handler 本体仍保留，P1 只在已批准白名单的发码入口绕过；没有改动
golden、`docs/project-status.md`、
`source/translator/linux/linker/` 或 benchmark harness。本 Codex 沙箱没有直接执行
Orb 命令；§9、§10 均采用 orchestrator 的 Orb 实测数据。没有执行 git 写操作。
