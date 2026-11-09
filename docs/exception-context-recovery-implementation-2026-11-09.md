# 异常现场还原实施与验收（2026-11-09）

本轮实现 [异常恢复方案](exception-context-recovery-plan-2026-11-08.md) 的同步恢复、guest 交付、修改后恢复、异步安全点和等待协议。原有 WIP 先提交为 `23a87ff1`，方案提交为 `20073b14`；后续实现从这两个可回退检查点推进，实现提交为 `56b44f1c`。

功能主路径已通过红魔真实内核与其余三种 host 的 Linux system-kernel 验收。性能结果有明确边界：新增机制未出现 Function 编译工作量倍增，CoreMark 的 Function 编译 CPU 诊断增加约 1.45%、编译指令数增加约 4.40%；但 CoreMark 运行仍慢 7.80%，Block 编译墙钟样本波动明显，`func_tests`、x87 的本轮 Function/FEX 中位数超过 2×。本轮不能宣告“零回退”或“全套 ≤2×”。

## 1. 已接通的完整路径

- 同步 native fault：TLS activation → phase/真实访问范围资格 → 原始 kernel GPR/FP frame 恢复 → kernel sigreturn → host-only `SvmNativeExceptionExit` → VM 合并 FP → 交付。旧 `sigsetjmp`、`siglongjmp` 和 `RaiseGuestFault` 从产品及测试调用者退役。
- 软件异常：除法、对齐、FP/x87、非法编码和断点按 ISA 语义走普通 cold exit，形成同一事件。合法但未实现的指令继续属于 Unsupported。decode 已有有效前缀时先执行前缀，再在真实坏 PC 产生事件。
- 异步信号：只入 pending，原 native 流程继续；源指令前的真实边界检查 ready request，再通过普通退栈发布完整 state。dispatcher/L1/L2 没有增加信号或逐 unit 入口 poll。
- 恢复：Linux handler 或 embedding callback 修改 guest context 后，重建 FP 配置并重新进入 dispatcher。`rt_sigreturn` 使用独立 ResumeGuestContext 结果，不被常规 syscall 的返回值、RCX/R11 等写回覆盖。不保存 native continuation，不为 guest signal frame 延长旧 code generation 寿命。
- 失败返回：`RunResult` 区分 GuestExit、GuestSignalTermination、UnhandledGuestEvent、HostFailure。自有 snapshot 在 VM 关闭前复制，VM 销毁后仍有效；cleanup failure 单独附带。CLI 完成清理后按 guest 终止信号产生真实进程 wait status；embedding 默认不杀宿主进程。

## 2. 实际职责与文件组织

```text
source/runtime/
  exception.h                       # 公共事件、stop reason、context view、snapshot、冷回调
  execution/
    fault.h / fault.cpp              # activation借用、同步记录、语义访问失败的当次hint
    exception.cpp                    # VM异常完成、callback、pending与guest OS交付
    runtime.cpp / vm.h               # native退出分流、共享direct服务、恢复后重新派发
    dispatch.h                       # 既有root ABI追加root SP、phase和pending字段
    runtime.h / guest_thread.h       # API结果、唯一运行state、当前事件
    code_image.h                     # ABI变化使持久化code image版本升为2
  frontend/
    guest_events.h                   # 256指令预算、4KiB逻辑PC窗口规则，无CFG分析
    x86/semantics_dispatch.inc        # 共享语义在控制副作用前插入安全点
    x86/semantics_control.inc         # 明确UD2、INT3的fault/trap与PC语义
    x86/build_ir.cpp                  # 融合窗口边界、坏decode前缀处理
    arm64/semantics.h                 # 共享语义的安全点、BRK/非法编码
    arm64/semantics_memory.inc        # NEON多读先暂存，全部成功再提交目的bank
  ir/
    opcodes.inc                      # GuestSafepoint的完整state/flags observer合同
    direct/                          # 复用前端语义，事件交付不重跑已完成副作用
    passes/                          # 现有effect/flags处理识别观察点，无新分析pass
  backend/
    {arm64,x64,riscv64,loongarch64}/
      recovery.h / recovery.cpp      # 窄寄存器view+固定ABI+稀疏recipe，不依赖Linux头
      entry_elf.S                    # root保存、helper阶段、普通exit、host-only异常exit
      emit_control.cpp               # handle派发的pending检查和普通慢退出
      emit_memory.cpp / emit_atomic.cpp # 只登记真实guest访问指令
    arm64/allocation_selection.cpp   # 原有反向选择保留flags/pin observer，显式PC除外
  platform/linux/
    signals.h / signals.cpp          # 进程signal owner、TLS、host altstack、broker
    {arm64,x64,riscv64,loongarch64}_host.cpp # host ucontext/FP解析与trampoline改写
    guest_signals.h / guest_signals.cpp    # dispositions、mask、独立FIFO、选择、交付、restart
    signal_frame.h                   # Linux guest ABI数据和frame编码接口
    signal_frame_x86.cpp             # x86-64 siginfo/ucontext/FP/xstate及sigreturn
    signal_frame_arm64.cpp           # ARM64 ucontext/FPSIMD及sigreturn
    wait_syscall.h / wait_syscall.cpp # HostWait登记、组合mask、取消/完成
    wait_syscall.S                   # 四host预处理选择的窄Linux syscall stub
    syscalls.cpp / syscall_abi.h     # x86/ARM64 guest syscall接线
    run_guest.cpp                   # 关闭前捕获snapshot、清理结果独立返回
```

这些是实际 source 职责，不是额外 pass。没有增加 MIR、逐指令 Home/rollback 表、certificate、任意 host PC deopt 或 per-block owner。Fault metadata 继续随实际发码产生，生命周期仍属于现有 CodeHeap。

## 3. 正确性与平台合同

### 同步故障和部分完成

四后端的 FaultSite 都收窄到真实 guest 内存访问；spill、state、普通 C++ 内部访问不能仅因处于 MayFault IR 范围就冒充 guest fault。FP 状态读取原始 signal frame，x64 原始 MXCSR 与其它后端原始 FPSR/fcsr 保持各自 ABI。恢复后普通出口不会再次覆盖 recipe 或重新采样 handler FP 环境。

窄 faultable atomic helper 仍通过汇编 fixup 普通返回，C++ 锁和 RAII 正常释放。ARM64 对齐拒绝已从裸 BRK 改为正常语义出口。direct Fetch/Read/Write/Atomic 及 ARM state service 仅在失败时更新本次 access、宽度和地址；同一 VM 连续 Write1→Read4 不沿用前一次 hint。

ARM NEON 多寄存器、lane/replicate load 暂存后提交，第二次读失败不会覆盖旧 Q。REP 等仍保留 ISA 允许的部分完成。不能据此宣告 ENTER、所有 x87 多次写入等类别已经逐条完成独立验证，也不会把已有副作用的操作切换 consumer 重跑。

### 安全点与代码质量

已知 x86、ARM64 前端在向后/跨逻辑窗口的直接边、全部间接 CALL/JMP/RET、顺序跨窗口、预算边界、REP 完成元素边界检查。IR/direct 复用语义，默认仍全部 JIT。

ARM64 快路径使用明确 scratch 的 load+CBZ，保留 NZCV、raw PF/AF。完整 flags/pin observer 继续参与原有选择；显式恢复 PC 不再误触发热入口重复 PC 发布，raw flags 只在 pending 慢路完成，慢路发码后恢复 writer 的全部快路 bookkeeping。

原 DeferTest 精确尾形遇到新增 observer 时仍可能失去资格，首版没有再增加匹配层或证明表。poll 的动态工作、完整 flags 保留及内联冷出口都计入本轮数据；不能把两条检查指令称为全部成本。

### Linux guest signals 与等待

实现 dispositions、mask、pending、altstack、sigreturn，以及本轮已有 kill/queue/wait/suspend 路径。标准信号合并，RT 每 signo 独立 4 槽 FIFO，启用时普通上下文准备容量，handler 不分配。ready 与全部 pending 分开，masked 信号不会导致每次 poll 都退出；满队列只对该 signo 形成 kernel-frame mask 反压，drain 只撤销服务新增位。

实际 Bionic 的 SIGRTMIN 为41，保留号码32–40继续提供 guest 软件队列，host ingress 不覆盖 libc/debugger 信号。glibc 使用自己的实际范围；handler 不调用动态 SIGRTMIN。

HostWait 的登记窗口闭合最后 pending 检查到进入内核的竞态；取消仍经窄 stub 普通返回，不跳过 C++ 栈。实际 handler action 决定 SA_RESTART，保存原 syscall 参数；partial IO 不重做，timed FUTEX_WAIT 使用原 monotonic deadline，WAIT_BITSET 保留原绝对期限。临时 wait mask、调用方 base mask 与 RT pressure 组合，互不覆盖。

### 宿主 signal 所有权

handler 由进程服务持有，activation 使用所属线程的 TLS；altstack 复用调用方已有栈，或普通上下文分配 kernel minimum+64KiB 与 guard pages。Bionic 小型 fault 测试发现的 PT_TLS 对齐问题已通过64字节 TLS 对齐修复。

嵌入方可提供 HostSignalBroker。自定义可恢复应用 handler 需要明确 broker，不能直接调用保存的 sa_handler 模拟 kernel 协议。Android 预装 debuggerd fatal handler 只作为未知 host 崩溃的终端接收者，归还 action 后由 kernel 再次交付，不作为可继续 guest 的 handler 链。Catch 的独立 POSIX fatal handler 已关闭，避免各测试之间替换 runtime owner。

## 4. 元数据与内存

- GuestExceptionInfo：64B；只保留一个当前事件。`unit_id` 尚无生产者时保持无效，不伪造 generation 身份；实际 native_offset 可用。
- NativeRunContext：1040B→1072B，追加32B；唯一 guest state 和既有字段偏移不变，不增加永久 GPR。
- FaultSite：保持32B，recipe 内记录实际访问方向/宽度。短 CoreMark Function 的 fault行6768→6445，对应216576→206240B；没有因精确范围增加额外所有权层。
- RT payload：每启用 signo 4×128B=512B，独立有界；普通信号固定32×128B=4096B，另有 dispositions、队列头和少量等待状态。不是每次 native entry 分配。
- 自有完整 snapshot 只在停止返回时分配；guest frame 属于 guest 栈。frame 不持有 native code指针，不形成 retired code pin。host altstack 属于 host线程。

仅实际已知字段有效：不能从 host si_code 编造所有 guest page-fault 错误位。无确认 syndrome 时保持 syndrome_valid=false；纯格式错误无法确认访问时使用 Unknown/0。

## 5. 功能验收

| 环境 | 结果 | 覆盖 |
| --- | --- | --- |
| macOS host 编译合同 | 10个 CTest target通过；最终局部发码变更后相关2 target复验通过 | IR构造/direct、两前端、优化、四后端发码、code image |
| 红魔 bf1359ff，真实Android内核 | 7组exception/API、5组signals、14组runtime、4组底层fault通过 | 两guest ISA、Block/Function、现场修改、FP、异步、嵌套altstack、原子、SMC等 |
| 红魔 persistence 检查点 | 4组、46 assertions通过 | AOT/磁盘缓存与新增ABI |
| x64/RV64/LoongArch64，各真实Linux kernel的QEMU system | 各7组exception/API、5组signals、4组persistence通过 | 共48次case执行，包含嵌套/坏decode前缀 |
| 上述三后端最终hint小修复验 | 各2组API、5组signals通过 | 共21次定点重复，包含连续访存故障hint；最终130源hash一致 |

JIT 往返用例检查零解释执行；embedding 测试另外明确选择 InterpretOnly 验证共享 direct 服务。测试采用独立 guest ABI 布局构造/检查 frame，不用产品 encoder 的 struct 自证。Check 适配器只在失败时向 Catch 记录断言，因此不能用低 assertion 数推导没有执行内部检查。

QEMU system 使用真实 Linux kernel，功能结论有效；不替代实体 host 的性能结论，也不使用 QEMU-user 的信号行为代替内核验收。原失败、harness错误、修复前检查点与最终结果分别保留，未合并成一个伪最终版本。

## 6. 编译与运行成本

红魔固定CPU4（taskset10），频率policy未修改，固定每 arm 3 次并保留全部。每个冻结版本120次compile、30次runtime；首版数据只作为修复前检查点，最终表仅使用最终冻结版本。编译扣除实际byte parser；SVM包含初始化/信号服务和完整native owner释放，FEX固定a156752f+既有Android probe，保留TSO。FEX的CompileBlock为多基本块，应与SVM Function比较。FEX probe不含最后native arena/lookup销毁，这项端点差异继续保留。

编译墙钟中位数，单位ms：

| case | 旧Block | 新Block | 旧Function | 新Function | FEX多块 | 新Function/FEX |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| coremark | 39.481 | 56.378 | 48.444 | 50.343 | 46.561 | 1.08× |
| int | 1.916 | 2.149 | 1.967 | 1.928 | 1.565 | 1.23× |
| fp | 2.535 | 2.467 | 2.601 | 2.858 | 1.720 | 1.66× |
| mem | 2.321 | 2.503 | 2.352 | 2.416 | 1.518 | 1.59× |
| branch | 1.941 | 1.909 | 1.909 | 2.058 | 1.474 | 1.40× |
| call | 2.161 | 2.204 | 2.090 | 2.133 | 1.576 | 1.35× |
| func_tests | 20.155 | 20.246 | 64.877 | 65.775 | 30.437 | 2.16× |
| x87_bench | 0.715 | 0.755 | 0.643 | 0.708 | 0.336 | 2.11× |

CoreMark Block最终样本：旧38.394/39.481/40.297，新56.378/39.818/57.209ms；其中位数约+42.8%，必须如实保留，不能删掉较慢样本或选最小值宣布通过。Function旧47.223–65.724，新48.773–86.177ms；`func_tests`两臂也有大幅墙钟波动。本轮只有3个样本，不对尾延迟或稳定Block回退给出过度确定结论。

为区分额外编译工作与墙钟波动，另做固定次数的独立诊断：

- PMU instruction clock：12次，只有Function的6次与正式编译工作量匹配，其编译指令中位数+4.40%。原始字段名虽以_ns结尾，单位实际是退休用户态指令，不能当时间或FEX比值。
- 精确steady_clock调用者的thread CPU clock：12次，Function的6次工作量匹配，旧61.599→新62.493ms，+1.45%。两臂含相同clock syscall包装成本，只作CPU工作诊断，不能替换上表正式墙钟。guest clock保持真实时钟。
- 两组诊断的Block工作量均未匹配正式样本，全部原样保留，但不进入匹配回归比值；没有继续重跑到满足条件。

短CoreMark Function的IR节点50739→52236（+2.95%），spill slots仍30、最大frame仍48B。native bytes337592→515220（+52.62%），包含新增poll、完整flags工作与内联冷退出；fault metadata反而减少。不能把增加的native bytes解释为spill增加，也不能把cold代码字节等同于动态执行指令。

运行中位数，单位秒，包含初次JIT与清理；CoreMark另通过100000次迭代、至少10秒和CRC校验：

| case | scope | 旧版 | 最终版 | 变化 |
| --- | --- | ---: | ---: | ---: |
| fp | block | 0.202753 | 0.203090 | +0.17% |
| fp | function | 0.203261 | 0.205099 | +0.90% |
| call | block | 0.380316 | 0.386648 | +1.67% |
| call | function | 0.379868 | 0.385849 | +1.57% |
| coremark | function | 15.450449 | 16.655387 | +7.80% |

本轮没有新测FEX运行成绩或运行PMU，不能沿用旧FEX运行结果充当同轮比较。首版调用测试约+13%，移除热入口PC冗余后降至约+2%；CoreMark约+8%仍然存在，后续减少冷出口占位和恢复局部DeferTest资格应以该数据为基线，不弱化完整observer来换成绩。

## 7. 保留边界

1. RV64 constrained LR/SC内部不插pending load，不承诺内部区间的有界异步响应；ARM64非LSE/CAS128与LA64 reservation路径也未以本轮测试证明重试区间的有界延迟。指令前后可交付不等于内部每次失败边都能交付。
2. 单guest线程owner协议已落地；完整clone/process-group/timer路由、所有Linux syscall restart类别不属于本轮完成范围。
3. frame只覆盖当前公开CPU profile。SVE/SME、PKU、AVX-512和TF单步等未实现能力不能因增加frame或snapshot而宣告支持。
4. 已登记fault的精确恢复不等于任意host PC deopt；未知C++故障交宿主策略，不伪造guest现场。部分完成指令按已定义的资格处理，未逐类验收的类别保留限制。
5. 全套compile≤FEX2×尚未实现，CoreMark运行+7.80%也未消除。源码完成和功能通过不能代替性能门槛；本记录固定这些差距，后续优化无需再重建异常框架。

## 8. 证据与资源

仓库内 [证据索引](evidence/exception-recovery-20261109/README.md) 保存最终输入SHA、130源freeze、全部正式观察记录、诊断及测试摘要。完整原始stdout/stderr、脚本、早期失败、源码patch和压缩runner在：

`/Volumes/Data2/swiftvm-middle-results-relocated-20261025/exception-recovery-20261109`

手机只使用本轮命名空间 `/data/local/tmp/swiftvm-exception-recovery-20261109`，已在清理前后独立检查 lsof 与 df：回收目录已分配287791104B，df可用空间增加287887360B，原目录剩余0B，无活动文件或清理错误。不清其它项目、服务或设备频率状态。Data2的35205640B可再生成parent副本也已核对原gzip后回收。各QEMU均已退出，最终两轮initrd共回收54318151B；最终清理回执保存在证据目录。
