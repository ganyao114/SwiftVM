# SwiftVM 项目状态（2026-07-26）

本文档记录当前里程碑状态、已验证能力、性能快照与已知问题。架构细节见 [ARCHITECTURE.md](../ARCHITECTURE.md)，x86 指令覆盖清单见 [x86-instruction-census.md](x86-instruction-census.md)。

## 一句话状态

x86_64 guest → 自定义 IR → host ARM64 JIT(vixl) 的 DBT 主干在真实 glibc/musl 静态二进制上端到端验证通过；多线程 guest(clone/futex)、TSO 内存序、SMC 自修改代码（含 MT 安全回收）、SSE2 基线、x87(opt-in JIT）均已落地。master = `76cdcd0`。

---

## 1. 已验证能力矩阵

| 子系统 | 状态 | 验证方式 |
|---|---|---|
| x86_64 前端（distorm） | 440/1107 mnemonic 已处理；52 特权级、598 异构明确跳过；16 个长模式非法项记录不实现 | 普查脚本 + fuzz |
| SSE2 基线 | packed int/float、scalar、cvt、ucomis 全 NEON 化 | 差分 fuzz + 定向断言 |
| x87 FPU | 默认 SoftFloat 位精确；opt-in JIT(SVM_X87_JIT=1)：栈管理/算术/比较/转换/FSQRT 内联 + TOP 虚拟化（再 opt-in) | 定向 2,049 断言 × 多模式 |
| LOCK 原子 | LOCK RMW、cmpxchg8b/16b(LDAXP/STLXP)；非对齐 LOCK→#GP(PageFatal)，非对齐无 LOCK→全局锁 | MT raw 测试 + Rosetta 仲裁 |
| 多线程 guest | clone/futex/每线程独立 Runtime + 共享 AddressSpace | clone_* 五件套 |
| TSO 内存序 | relaxed/acqrel(LRCPC 快路径 + REP 栅栏 + 栈/TLS 松弛） | litmus + 双模式矩阵 |
| SMC | 写保护 + 信号失效；MT 安全 QSBR epoch 回收（SVM_SMC_MT) | smc=99 + clone_smc_mt 512 次补丁 |
| 函数级编译 | FunctionBaseCompile 默认启用；CallLambda 函数级放开 | func_tests 3 模式 checksum |
| 静态寄存器映射 | RSP→x19 默认启用 | 矩阵 |
| 异常处理 | host 信号链（SMC→JIT fault→PageFatal)；非对齐 LOCK 语义按 Rosetta 仲裁 | bad_pointer=1 等 |

**9-binary 矩阵**（双 TSO 模式全过）:`hello=42 loop=186 real_hello=42 real_hello_musl=42 real_busy=0 real_busy_musl=0 bad_pointer=1 smc=99 clone_futex_smoke=0`
**func_tests**:function/block/interpreter 三模式 checksum `9f52b7d59285dbe5`
**TSO litmus**:relaxed `mp_bad>0`（检测器有效）,acqrel `mp_bad=0`（红线）

---

## 2. 性能快照（macOS arm64)

| 基准 | 数值 | 说明 |
|---|---|---|
| x87 bench 默认（SoftFloat 位精确） | 16,000,006 helper calls / ~1.63s | 基线 |
| x87 bench SVM_X87_JIT=1 | 2,000,001 calls / ~0.45s | FADD/FSUB 的 IXC 位精确守卫仍 bail |
| compare/sqrt 循环 opt-in | 40,038 → 6 helper calls | 中盘内联收益 |
| TOP 虚拟化 | stock bench 持平（0.45→0.46s) | bailout 主导，收益在 x87 密集内联段；默认 OFF |

---

## 3. 环境变量开关

| 变量 | 取值 | 默认 | 说明 |
|---|---|---|---|
| SVM_TSO_MODE | relaxed\|acqrel\|hardware | relaxed | **字符串**，0/1 会静默落 relaxed |
| SVM_X87_JIT | 0/1 | 0 | x87 JIT opt-in；默认 SoftFloat 位精确 |
| SVM_X87_JIT_STATS | 0/1 | 0 | helper call 计数 |
| SVM_X87_TOPVIRT | 0/1 | 0 | TOP 虚拟化（需同时 SVM_X87_JIT=1) |
| SVM_SMC_MT | 0/1 | 1 | MT SMC 回收 kill switch |
| SVM_FUNC_BASE | 0/1 | 1 | 函数级编译 |
| SVM_ENABLE_JIT | 0/1 | 1 | 0=纯解释器 |
| SVM_STATIC_REGS | 0/1 | 1 | RSP→x19 |
| SVM_UNIFORM_ELIM | 0/1 | 1 | uniform 消除 pass |
| SVM_ARM64_LRCPC | 0/1 | 1 | TSO LRCPC 快路径 |
| SVM_FORCE_FIXED_STACK | 0/1 | — | 诊断：强制 guest 栈 fixed/fallback(布局 flake repro) |
| SWIFT_FUZZ_SEED | u64 | 随机 | fuzz 定值复现 |

---

## 4. 测试与验证体系

- **差分 fuzz**(`swift_test "Fuzz x86*"`,Unicorn oracle):32 族 per-case 运行。**已知 flake:Unicorn 自身偶发 SIGILL(rc=132,x86_fuzz.cpp:6023/6030)~10-30%/run，会 abort 整个 Catch2**——方法论：per-case 循环 + 每例 3 次重试；管道中 `$?` 是 grep 的码不是测试的码。
- **Rosetta 仲裁**(`arch -x86_64`):ISA 语义 ground truth（真实 x86 边界行为，如 FCOM IE、FIST 边界、非对齐 LOCK 信号形态）。
- **raw 测试二进制**(`source/translator/linux/tests/`,.S + 生成器 + 已检入 ELF):smoke/coverage/MT/TSO/x87 各家族；`build_real_tests.sh` 在 orb ubuntu-x64 VM 或 `clang -target x86_64-linux-gnu -nostdlib -static` 重建。
- **func_tests**:真实 C 多块函数，三模式 checksum 必须逐位一致。
- **MT/TSO**:clone_* 五件套 + litmus(mp/sb00 合法方差区间历史已知）。

---

## 5. 已知问题与遗留

### P0 — 潜伏 bug（触发面已收窄但未根治）

1. **防御性 spill 路径误编译**:GetTmpX 池耗尽边缘的 spill 路径会产生错误代码（曾观测 pc=0xffc00000=不定 f32 NaN 常量被当代码地址跳）。NaN fixup 已降压（16→8 临时）大幅降低暴露面，但 spill 路径本身的 bug 未修。任何新的高压力 emitter 都可能再次踩中。**新增保留寄存器必须审计所有 emitter 的峰值临时数**（池=19，保留清单见 trampolines.cpp)。
2. **vixl ip0/ip1 clobber 类**:vixl 宏物化不可编码立即数走 x16/x17，会静默踩掉分配到这里的 emitter 临时。当前缓解=涉事 emitter 入口 `context.ReserveTmpX(ip0/ip1)`(EmitX87Op、EmitVecFloatNaNFixup 已做），但**全 JIT 其他 emitter 未审计**。

### P1 — 忠实度差异（默认路径冻结，不影响 Unicorn fuzz)

3. **helper vs 真实 x86 两处**(Rosetta 仲裁发现，Unicorn 与 helper 一致所以 fuzz 盲区）:①FIST 向上舍入时真实 x86 置 C1,helper 清 C1;②m64 load helper 预 quiet SNaN 但不把 IE 传给后续 FSQRT。修复需同步更新 fuzz 期望。
4. **x87 opt-in reduced 语义**:FMUL/FDIV 走 f64 受控精度（文档化 diverge),FADD/FSUB 以 IXC 守卫保位精确；FILD m64 守卫 |x|≤2^53。provenance 标记 0xA5(canonical)/0xA6(reduced-ready)。
5. **TOP 虚拟化默认 OFF**:stock bench 收益为零（bailout 主导），该模式 fuzz 覆盖薄。关键不变量已写入代码注释（pin 读取必经 TOP reload 的全寄存器 Ubfx 顺带清陈旧 mask)。

### P2 — 工程遗留

6. **x87 14-byte legacy env**(FNSTENV 32-bit 形式）未实现；FIP/FDP 非逐指令精确（helper 路径正确，内联路径是块级近似）。
7. **16 个长模式非法 mnemonic**（普查确认不可达，记录不实现）;32-bit guest 模式所有构造点均 is_64bit=true。
8. **静态映射仅 RSP→x19**:RBX/RBP 扩展未做（大改，需 FillStaticRegs/SpillStaticRegs 边界纪律）。
9. **CallLambda 多块函数 liveness** 保守回退（host-call 跨块活区间未证明）；>64 块+lambda 走块编译（任务 #43)。
10. **aarch64 解释器 PageFatal 缺口**（既有）。
11. **"Flag elimination" 定向断言失败**(main_case.cpp:476,master 基线即失败，非近期引入）。
12. **信号 backpatch TSO 不做的结论已固化**:FEX 用它服务非对齐机制而非优化；我们编译期对齐证明+廉价分支已获同等快路径，成本（并发代码补丁+I-cache 同步+信号链耦合）不值。

---

## 6. 开发工作流约定（本仓库现行）

- **codex 实现 + host 独立复核**:codex 沙箱无法运行 Unicorn(初始化即 SIGILL),fuzz 验收必须 host 侧做；codex 永不 git commit。历史上每轮独立复核都抓到过真 bug(2 个 x87 provenance、movbe decode、cpuid 测试自相矛盾、vixl ip0/ip1 类）。
- **分支隔离验证不足（三次验证）**:cmpxchg16b(x13)+指令覆盖（x30)+16-temp emitter 各自绿、合并才 PANIC;dlmalloc interposition 靠布局 oracle 潜伏。**合并后必须在 master 重跑全量**。
- **多 worktree 并行**:SwiftVM-{x87,x87mid,x87top,tso,static,sse2,fixstack,nanfix};master 只在主仓库检出。
- **调试布局 flake 的工具链**:SVM_FORCE_FIXED_STACK + macOS .ips 崩溃报告（~/Library/Logs/DiagnosticReports,python 两行式 json 解析）+ /cores(ulimit -c unlimited，系统会快速清理需立刻分析）+ lldb（注意会改变布局掩盖 flake)。
