# 前后端指令覆盖修复：2026-11-07

本轮对应 review 的 1–6 项，基线为 `5925d6d205bb5e498df1c23611e90fc1b6a2aefc`。实现位于 `/Volumes/Data2/SwiftVM-submit-20261104`。没有增加优化 pass、持久中间表或第二份 Program；Block / Function 分配仍独立。默认屏蔽 SSE 异常的执行保持 JIT。

## 已实施

| 项目 | 修复及职责位置 |
| --- | --- |
| 1. ARM 窄向量 | `frontend/arm64/semantics.h`、`semantics_registers.inc`、`semantics_memory.inc`：B/H/S/D 访存与 Uniform 传输使用真实 U8/U16/U32/U64；V128 取低半使用 `VecExtract64`。写回清除架构规定的高位，Q=0 NEON 使用低半标量载体，避免把 V8/V16/V32/V64 提交到仅接纳 V128 的后端。 |
| 2. x64 Crypto ABI | `backend/x64/emit_crypto.cpp`：SysV 第三个 128 位 aggregate 整体在栈上传递，后续立即数使用 R9，保留活跃 SSA 保存区。覆盖无硬件扩展的 AES/PCLMUL/SHA fallback。 |
| 3. FMA 舍入 | `arch/x86/floating.{h,cpp}`、`backend/x64/helpers.cpp`：使用显式 `softfloat_state` 的 f32/f64 融合乘加，保留一次舍入及 RD 下的负零。原 bits API 保留，实际发码数值 helper 同时返回新异常状态。所需 SoftFloat 源码及来源 SHA 固定在 `externals/SoftFloat-3e/FMA_UPSTREAM.json`。 |
| 4. ARM 状态指令 | `arch/arm64/service.h`、`frontend/arm64/state_service.h` 负责编码/分类；`execution/arm_state_service.cpp` 拥有 LDXR/STXR、LDAXR/STLXR、LDXP/STXP、LDAXP/STLXP、CLREX 的实际 reservation 和内存操作，及 CNTVCT/CNTPCT/CNTFRQ、CTR、DC CVAU/CVAC/CIVAC、IC IVAU 服务。 |
| 5. 原子访存 | `runtime/atomic.h` 是唯一请求/结果 ABI；`execution/atomic_service.cpp` 负责完整范围与 backing 资格、SMC 准备和提交。`platform/linux/faultable_atomic.{h,cpp}` 负责 Linux 故障恢复；四个 `backend/*/atomic_native.S` 承担真实 CAS 指令与 fault epilogue。ARM/RV/LA 的非对齐冷路径、x64 无 CX16 的 linked CAS128 都使用同一 owner 服务。 |
| 6. SSE/AVX 异常状态 | 共用纯数值叶负责 direct/checked 执行；各后端保留普通原生算术并累计异常。`ReadFPStatus(shadow)` 在原生代码内合并硬件状态与显式 canonical shadow；STMXCSR 原生完成，无新增 decoder frontier。前端保留 COMIS/UCOMIS、比较谓词 signaling、ROUND precision suppression 维度。 |

另修复了验证触发的 MINSS/MAXSS 合法 F3 前缀准入遗漏、AFP DAZ 下 MIN/MAX 选择结果的 signed-zero 处理，以及 ARM FP 转换/比较的 scratch 与 spill alias。

## 执行合同

### 原子与 reservation

- `AtomicCall` 是单次调用的 80 字节临时包。`NativeRunContext.atomic_service` 固定在 offset 152；原 Uniform state 仍在 offset 160。没有通用逐节点调用表。
- 对齐的热原子路径保留原生实现；非对齐 ARM/RV/LA 使用冷分支。可装入一个原生 CAS 容器的访问使用 masked CAS，保留邻接字节并与外部硬件访问互锁。
- 普通 CMPXCHG 比较失败仍完成 old→old 的真实写周期；store-exclusive 比较失败不写内存。完整范围检查先于可能的写入，跨页 fault 不允许部分提交。
- RV/LA 无原生宽原子能力时，跨容器/128 位软件提交使用进程级 guest 执行锁，适用于稳定私有匿名内存。整个 JIT/direct 执行域共同持锁，因此普通访存和较小宽度的硬件原子也被覆盖。外部共享映射的这些访问继续返回 Unsupported；普通进程私有 mutex 不能提供外部互锁。
- `BackingKind::AnonymousShared` 与私有匿名内存分开记录。file/private/shared 的标量访问可使用真实 faultable CAS，不再全部按“非匿名”拒绝。
- ARM exclusive 当前限定私有匿名域。reservation 的有效位在 Uniform state 中，地址/宽度/观测值由 GuestThread 持有；普通 guest 写入、owner 写入、系统调用和故障会清除有效位，防止仅靠值相同误判自身 ABA。没有为外部共享写入宣称监视能力。
- 静态 CAS leaf 的 SIGBUS/SIGSEGV 回到汇编 fault epilogue，以普通状态返回调用者；不跨 C++ 锁/资源栈 longjmp。direct 模式也安装这一故障服务。

### 128 位软件原子的全局锁

`execution/guest_execution.{h,cpp}` 只负责一把进程级递归 mutex 及其 RAII 所有权 `GuestExecutionLock`。`FaultScope` 在注册故障服务前取得锁，覆盖本次完整 JIT/direct 执行及 owner 服务，恢复宿主 handler 后才释放。这个所有权在既有 `sigsetjmp` 之前构造，原生故障回到该边界时不会遗失锁。

`atomic_service.cpp::PrivateCommit` 复用同一锁，完成安全读取、比较、SMC 准备和提交。递归只用于 owner 在持锁期间进入软件提交；没有给每条访存、每个 block 或对齐热原子路径增加加锁操作。128 位硬件 CAS 可用时仍优先使用硬件；跨容器标量软件提交也沿用这一协议。`AtomicCall`、NativeRunContext、IR、后端发码均无新增布局或协议。

这将原有单 guest 执行域的约束变成实际互斥，不代表新增了 guest 多线程支持。宿主通过 `Memory()` / `HostPointer()` 获得的借用只能在 guest 停止时使用，或在整个访问期间持有同一执行锁；仅在获取指针时加锁不能保护其后的解引用。guest `MAP_SHARED`、独立 native 线程和其他进程没有自动加入该协议，因此无硬件支持的共享 128 位操作仍不接纳。后续实现并行 guest 执行时，需要在软件提交期间暂停其他普通访问，不能只保留 CAS128 helper 内的锁。

本次增量验证：红魔和 RV64 真 Linux 内核的执行锁与原子定点检查通过。CAS128 成功/失败覆盖 JIT/direct × Block/Function，检查 observed、ZF、目标与无关 flags；另检查对齐、只读且比较失败时的 fault 不提交。RV 明确记录 `HasAtomic128=false`，因此实际覆盖软件提交及共享映射 Unsupported，并非只在有原生 CAS128 的 ARM 主机上验证。原有非对齐及跨页 fault 检查仍通过。

与加入执行锁前的精确 WIP runner 在红魔 CPU4 交错各 3 次，完整 compile 中位数：Block 43.645 → 43.835 ms（+0.44%），Function 53.526 → 53.557 ms（+0.06%），未见明显劣化。这里使用短 CRC 检查负载测 compile，不宣称有效 CoreMark 跑分。Block 动态 compile 次数在 2050–2055 之间变化，同次数的两对样本发布字节均为 387,760；Function 六个样本均为 676 次、421,800 字节。没有要求动态路径不同的样本总字节严格相等，也未重跑整套 benchmark。增量源码、实际二进制 SHA、原始结果及清理回执见 `/Volumes/Data2/swiftvm-middle-results-relocated-20261025/atomic-global-lock-20261107/`。

### 浮点

- MXCSR 六个 sticky 位均有实际累计：invalid、denormal-input、divide-by-zero、overflow、underflow、precision。处理 DAZ/FTZ、NaN 优先级、有效 scalar lanes、比较 signaling 和 ROUND suppression 的差异。
- `NativeRunContext.guest_fpsr` 保存实际宿主状态，跨 root/cold/helper 边界保留；宿主原有 FP 环境在返回时恢复。context 大小变为 1040，code-image configuration 已包含该大小，旧缓存不会被当作新 ABI 加载。
- 普通算术数值 IR 声明 `WriteHostState`，无用数值结果仍保留异常副作用。现有 Uniform forwarding walk 通过 `UniformPolicy.numeric_control` 精确失效相应 4 字节状态；原先使用 `Unknown` 的 FMA/ROUND 仍保留其保守屏障。
- guest 取消异常屏蔽时，VM 明确选择共用 direct 数值执行路径。同一架构指令的所有 lanes / AVX 两半完成异常归并后，按前置异常优先级检查。fault 恢复该指令的寄存器状态，保留新 sticky 位和原 PC，并返回既有 GuestFault 合同。重新屏蔽后可继续 JIT。
- 上述精确停止不等于新增 Linux guest SIGFPE handler 投递；运行时信号模型仍沿用现有边界。普通 FP quiet-NaN payload/sign 的既有许可没有扩展。

## 验证

- 23 个 ARM 指令形态的 direct 执行，以及四后端 × Block/Function 的 184 次编译全部通过；覆盖此前失败的 B/H/S/D load/store、D pair、Q=0 向量形态。
- 红魔 `bf1359ff`、CPU4：ARM 前端 10 组、ARM native 39 组通过；运行时 8 个 native 组通过。SSE 组含 11 个 masked 样例在 JIT/direct 各执行一次，以及 2 次 unmasked 精确 PC/目标保持检查。
- ARM exclusive/cache/counter ELF：红魔原生对照，以及 Block/Function × JIT/direct/mixed 六种组合均 exit42、输出一致。
- x64、RV64G、LA64D 在真实 Linux 内核的 QEMU 中执行原子、浮点、ARM fault/control 与 exclusive JIT/mixed 集成，全部通过。RV 禁用 V 与额外位操作扩展；LA 禁用 LSX/LASX，使用 16 KiB 页。模拟器结果只用于功能验证。
- 共用数值叶 6 组、125 条断言通过；x64 Crypto/FMA/比较/ROUND 实际发码定点 5 组通过。host 的 x86 direct、IR optimize、ARM frontend 合同全部通过。
- 最终仅将 ARM/x64 的纯架构 CAS 汇编移入各自 backend；重建后的 `.text` 与原对象逐字节相同，Linux 层保留 ucontext 恢复。最终验证按实际二进制 SHA 分批保留。中途失败与修正记录没有覆盖；后续只补跑受修正影响的项。

完整证据根：`/Volumes/Data2/swiftvm-middle-results-relocated-20261025/instruction-coverage-fixes-20261107/`。其中 `x64/summary.json`、`rv-la-final-summary.json`、`phone/verified-tests.json`、`arm-shape-summary.json` 分别绑定实际结果。

## compile 成本护栏

从精确 parent Git tree 单独构建 Android runner，NDK29/API28、Release/O3、ThinLTO、static libc++ 的有效参数逐项匹配。红魔 CPU4 上 parent/current 交错各 3 次，无磁盘 code cache，CoreMark CRC 均为 `0x4983`，采样前后频率上限一致。

| 口径 | 分配 | parent 中位数 ms | 本轮中位数 ms | 变化 |
| --- | --- | ---: | ---: | ---: |
| 完整 compile，普通计时 | Block | 42.704 | 43.408 | +1.65% |
| 完整 compile，普通计时 | Function | 53.033 | 53.068 | +0.07% |
| 扣除字节 decode，诊断计时 | Block | 41.508 | 41.448 | −0.15% |
| 扣除字节 decode，诊断计时 | Function | 50.252 | 50.194 | −0.12% |

本轮没有明显 compile 劣化。完整发布字节数由 Block 370,384 → 387,944、Function 378,008 → 421,800，包含新增原子冷分支和异常状态处理；这些值没有被当作热路径动态指令数。

实际参数、source/Git blob 校验、所有样本与 CPU 频率记录见证据根 `compile-guard/`。这是对本轮 parent 的回归护栏，未重新锚定全套 FEX benchmark。

语义依据包括 [Intel SDM](https://cdrdv2-public.intel.com/671110/325383-sdm-vol-2abcd.pdf) 与 [Volume 1 异常优先级说明](https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf)，以及固定版本 SoftFloat 源码。
