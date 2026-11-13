# ARM64 → x64 残余热点优化实施方案

日期为 2026-11-12。本文承接[机制优化实施结果](arm64-x64-mechanism-optimization-results-2026-11-12.md)，以提交 `98e4d829` 的实现为基线。目标是消除该实现在热路径上留下的浮点环境切换和 flags 快照往返，补齐复合条件、整数发码和浮点覆盖上的缺口。fault 处的 NZCV 与 FPSR 继续逐位精确，本方案没有放宽任何 guest 可观察语义。

批次 0—7 已实施，本文已按验证后的接口和边界条件修订。实施数据、编译成本和验收范围见[残余优化实施结果](arm64-x64-residual-optimization-results-2026-11-12.md)。冻结基线的生产代码对应 `98e4d829`，基准工具包含 `0297f745` 的诊断改动。

## 1. 基线与证据

### 1.1 与 Dynarmic 的剩余差距

数据取自上一轮结果，WSL `ProtonHM`、CPU2、Release／IPO、Function／pin0，单位 ms。

| 负载 | SwiftVM | Dynarmic | SwiftVM／Dynarmic |
| --- | ---: | ---: | ---: |
| 向量矩阵 | 0.769 | 0.127 | 6.06× |
| 标量矩阵 | 2.595 | 0.442 | 5.87× |
| 浮点 | 9.660 | 4.581 | 2.11× |
| 综合计算 | 8.602 | 4.262 | 2.02× |
| SHA | 6.572 | 3.248 | 2.02× |
| 排序 | 0.818 | 0.629 | 1.30× |
| 访存 | 19.238 | 16.476 | 1.17× |
| 移位 | 11.107 | 16.821 | 0.66× |
| 整数 | 8.244 | 13.693 | 0.60× |
| 浮点展开 ×8 | 1.615 | 2.692 | 0.60× |

### 1.2 采样归因

对上一轮归档的 CPU-clock IP 采样重新归类，每个负载合并 3 个进程。采样点记在被采 IP 的前一条指令上，用来抵消中断滞后，结果是近似值。[归因脚本](evidence/arm64-x64-residual-20261112/sample_attribution.py)、[完整输出](evidence/arm64-x64-residual-20261112/sample-attribution.txt)

| 负载 | MXCSR 存取 | flags 往返 | guest 状态读写 | 其余主要项 |
| --- | ---: | ---: | ---: | --- |
| 浮点 | 70.3% | — | 2.4% | MXCSR 序列使用的栈槽 14.6% |
| 向量矩阵 | 45.3% | 1.7% | 8.1% | 寄存器运算 20.9%，XMM 搬运 13.9% |
| 综合计算 | 4.5% | 25.1% | 19.0% | GPR 搬运 15.1% |
| SHA | — | 29.4% | 22.1% | GPR 搬运 15.8% |
| 访存 | — | 28.9% | 10.7% | guest 内存 42.1% |
| 排序 | — | 12.3% | 13.5% | 寄存器运算 54.6% |
| 整数 | — | — | 83.1% | |
| 移位 | — | — | 34.0% | 寄存器运算 48.3% |

Dynarmic 在浮点负载没有 MXCSR 存取，在访存和 SHA 的 flags 往返分别为 2.9% 和 3.5%。

热点窗口给出对应的代码形态。[热点窗口](evidence/arm64-x64-residual-20261112/hot-windows.txt)

- **浮点循环**每轮打开并关闭一次浮点区间。窗口内 2,394 个采样中约 2,200 个落在两组 `stmxcsr／ldmxcsr` 及其后续指令上。Dynarmic 的同一循环是 `addsd; ucomisd; jp`。
- **向量矩阵**热区的 64 条 packed 运算共 3,114 条宿主指令，平均每条约 49 条。其中 256 次逐 lane 指数检查约 2,000 条，其余 `movdqa／movdqu` 约 700 条，MXCSR 存取 52 条。Dynarmic 每条运算是 `mulpd; vcmppd; ptest; jnz`。[指令统计](evidence/arm64-x64-residual-20261112/matrix-mnemonics.txt)
- **SHA 轮循环**在 `sub r14,0x100` 之后执行 `pushf; pop r11; mov [rsp+0x48],r11`，经过约 25 条不产生 guest flags 的指令，再以 `push [rsp+0x48]; popf; jne` 结束。
- **访存循环**每轮在回边执行 `pushf; pop r11; mov [rsp],r11`，热路径上没有对应的恢复。
- **排序内层循环**的无符号比较分支被物化为两个 SETcc、一次 OR 和一次 TEST，共约 20 条指令，并触发一次 `pushf` 保存。

### 1.3 宿主指令成本

在同一台宿主 Ryzen 9 7950X3D 上，用独立内核测量各载体形态的单轮耗时。下表以基础循环 `add; sub; jne` 的 ns／轮归一化为 1，数值表示相对耗时。[源码](evidence/arm64-x64-residual-20261112/carrier_cost.S)、[结果](evidence/arm64-x64-residual-20261112/carrier-cost.txt)

| 形态 | 相对基础循环耗时 |
| --- | ---: |
| 基础循环 | 1.0 |
| 加 `pushfq; pop; mov` 保存 | 4.0 |
| 加 `pushfq` 保存与 `push; popfq` 恢复 | 24.5 |
| 加 `lahf; seto; mov` 保存 | 2.0 |
| 加 `lahf` 保存与 `add al,0x7f; sahf` 恢复 | 4.0 |
| 加 `setne`，分支前 `test` | 1.0 |
| 分支前对结果重新 `test` | 1.0 |
| `addsd` | 3.0 |
| `addsd` 加 NaN 与 MinNormal 结果检查 | 3.0 |
| 两输入指数准入加 `addsd` | 4.1 |
| MXCSR 区间开合加准入加 `addsd` | 39.6 |
| 4 lane 指数准入加 `addpd` | 7.1 |
| `addpd` 加 SSE4.1 结果检查 | 4.4 |

区间开合与 `popfq` 恢复在这组内核中占据主要耗时。SETcc 物化和结果后查在这组测量里没有可测的额外成本。

### 1.4 成因定位

| 现象 | 代码位置 | 成因 |
| --- | --- | --- |
| 区间开合 | `BeginArmFPRegion`、`EndArmFPRegion`、`PrepareArmFPRegion`，[emit_float.cpp](../source/runtime/backend/x64/emit_float.cpp) | 每个可能 fault 的访存、控制流终结和 helper 都结束区间，区间自带一组 MXCSR 保存与恢复 |
| ARM64 的状态位没有环境合同 | `VM::MergeFPStatus`，[runtime.cpp](../source/runtime/execution/runtime.cpp) | linked 入口已经装入 guest MXCSR，退出、helper 和 fault 也已保存它，状态位的合并只对 x86 guest 实现 |
| 逐 lane 准入 | `Handle(ArmFPBinary)` 的输入循环，[emit_float.cpp](../source/runtime/backend/x64/emit_float.cpp) | 以输入指数范围保证只产生 IXC，每个输入的每个 lane 一次检查 |
| 访存前必存快照 | `PrepareFlags` 的默认分支，[emit_flags.cpp](../source/runtime/backend/x64/emit_flags.cpp)；`RecordGuestAccess`，[emit.h](../source/runtime/backend/x64/emit.h) | 访存一律先保存再失效 native，recipe 只发射保存槽来源。恢复函数已支持 EFLAGS 来源 |
| 块入口固定走保存槽 | `Writer::Run` 的块入口状态、`BranchTo`，[emit.cpp](../source/runtime/backend/x64/emit.cpp) | 任何需要 flags 的后继都要求规范保存槽，回边因此每轮保存 |
| `popfq` 恢复 | `EnsureNativeFlags`，[emit_flags.cpp](../source/runtime/backend/x64/emit_flags.cpp) | 条件消费者与 producer 之间出现破坏 EFLAGS 的宿主指令时，从保存槽恢复整个 RFLAGS |
| 复合条件走布尔链 | `CondPassed`，[semantics_registers.inc](../source/runtime/frontend/arm64/semantics_registers.inc) | HI／LS／GE／LT／GT／LE 由多个 `TestFlags` 组合。后端已能把 `CondSet` 的 14 种条件直接映射为 Jcc |
| 32 位 ROR 展开 | `ShiftOperand` 的 32 位 ROR 分支，同上 | 生成 LSR、LSL、OR 三个节点。SHA 负载各单元的 IR 合计有 313 个 `LslImm`、5 个 `RorImm` |
| 恒等运算保留 | `Fold`，[fold.cpp](../source/runtime/ir/passes/fold.cpp) | 只在全部操作数为常量时折叠，`x\|0`、`x&全一` 保留为运算 |
| 多余归一化与中转 | `Logical`、`Arithmetic`，[emit_scalar.cpp](../source/runtime/backend/x64/emit_scalar.cpp)；`Uniform`，[emit_memory.cpp](../source/runtime/backend/x64/emit_memory.cpp) | 32 位逻辑运算用 64 位指令再 `mov r32,r32`，右操作数先进 RCX，V128 的 uniform 读写经 XMM0 |
| 重复装载 guest 寄存器 | `UniformCache`，[rewrite.cpp](../source/runtime/ir/passes/rewrite.cpp)；寄存器池，[allocate.cpp](../source/runtime/backend/x64/allocate.cpp) | 转发表 8 项，标量 home 6 个 |
| 浮点覆盖窄 | `TryNativeScalarFP` 等，[semantics_float.inc](../source/runtime/frontend/arm64/semantics_float.inc) | x64 目标只有 FADD／FSUB／FMUL 及其 NEON、indexed 形式进入快路径，其余经 `ExecuteNumeric` helper |
| 基准没有覆盖 FMA | [bench_dynarmic.py](../tools/bench_dynarmic.py) | guest 以 `-ffp-contract=off` 编译，FMADD 与 FMLA 不出现 |

## 2. 批次总览

| 批次 | 交付 | 主要受益负载 | 依赖 |
| --- | --- | --- | --- |
| 0 | 补充基准内核和 x86 guest 基线，冻结基线 runner，修复审查发现的问题 | 全部 | 无 |
| 1 | ARM64 guest 的 MXCSR 状态位并入 native 环境合同，删除区间开合 | 浮点、矩阵、综合 | 0 |
| 2 | FZ=0 域改为结果后查，packed 运算整向量检查 | 矩阵、浮点 | 1 |
| 3 | 复合条件和值测试直接生成 Jcc | 排序、通用分支 | 0 |
| 4 | flags 载体：条件物化、native 来源 recipe、热边入口约定、LAHF 快照、packed 权威路径 | SHA、访存、综合、x86 guest | 3 |
| 5 | 整数发码：ROR32、恒等折叠、32 位指令形式、操作数直达 | SHA、综合 | 0 |
| 6 | uniform 转发表扩容，先测量再定方案 | SHA、综合 | 5 |
| 7 | 扩大原生浮点覆盖 | 新增浮点内核 | 1、2 |

批次 1、3、5 互不依赖，可以并行开发。每个批次保留冻结 runner，按 §11 的协议与上一批次配对比较。

## 3. 批次 0：基准补充与基线冻结

现有五个微循环只包含 `subs; b.ne` 一种条件形态，浮点内核只有 FADD 和 FMUL。在 `tools/bench_dynarmic.py` 的内核集中增加以下输入，生成方式、输出校验和证据格式沿用现有内核。

| 内核 | 内容 | 覆盖的批次 |
| --- | --- | --- |
| `cond` | 每轮包含 `b.hi`、`b.lt`、`cbz`、`tbnz` 各一次 | 3 |
| `cmpsep` | `subs` 在循环开头，`b.ne` 在末尾，中间 8 条不置 flags 的 ALU 指令 | 4 |
| `fpmix` | 标量循环，含 `fcmp` 加 `b.gt`、`fdiv`、`scvtf`、`fcvtzs`、`fabs`、`fmadd` | 7 |
| `compute-fma`、`matrix-fma` | 现有 `compute` 与矩阵源码以 `-ffp-contract=on` 编译 | 7 |

以 `98e4d829` 构建的 `svm_arm64_compare_bench` 和固定 Dynarmic `9d4582339990d4eae53f1dc7160686920fc2075c` 作为冻结基线，记录全部内核的初始数值。`fpmix` 和两个 FMA 变体在基线上会大量进入 helper，其初始数值用于衡量批次 7。

### 3.1 x86 guest 的执行基线

上一轮只测量了 ARM guest 的执行性能，而原生 flags 对所有 guest ISA 生效。x86 guest 的全 flags producer 仍走 packed 路径，其后的条件消费现在要先把 RBX 导入保存槽再 `popfq`，见 §7.6。用 `benchmarks/app_inputs/build_coremark.py` 生成的 x86-64 CoreMark 比较 `52f04abd` 与 `98e4d829` 两个构建的热执行，结果决定 §7.6 在批次 4 内的先后。该脚本的工具链路径默认指向 macOS 的 NDK，在 WSL 中改用本机的 clang 与 lld。

### 3.2 代码审查发现的前置修复

对 `52f04abd..0297f745` 的代码审查给出以下正确性问题，R1、R2 已对照源码确认，R3—R6 在当前前端和 VM 的调用方式下不会触发。它们随批次 0 修复并各补一条回归。

| 编号 | 位置 | 问题 | 影响范围 | 处理 |
| --- | --- | --- | --- | --- |
| R1 | `Writer::Shift`，[emit_scalar.cpp](../source/runtime/backend/x64/emit_scalar.cpp) | `AsrImm` 的位移量掩码后为 0 时不发射移位，结果仍标记为已归一化，U32 home 留下 64 位符号扩展值 | x86 guest 的 `sar r32, 0` 一类形式。现有消费者多会再次归一化，不易观察 | 位移量为 0 的 ASR 不标记归一化 |
| R2 | `VM::UpdateFPEnvironment`，[runtime.cpp](../source/runtime/execution/runtime.cpp) | ARM64 分支去掉了 `architectural_write` 条件，每次调用都由 FPCR 重算 `NumericControl`，覆盖 `VM::Precompile` 为各 flush 域写入的值 | 具备 `specializes_fp_flush` 的宿主上，ARM64 guest 的 AOT 只预编译域 0。x64 宿主不受影响，本机无法执行该路径 | `Precompile` 的域循环直接选择域，不经 `NumericControl` 传递 |
| R3 | `BeginArmFPRegion` 的中途 FPCR 校验，[emit_float.cpp](../source/runtime/backend/x64/emit_float.cpp) | 退出时写入的 `guest_pc` 只由访存指令更新，flags 需求分析和写回消除也没有把 `ArmFPBinary` 视为可退出 | 潜在。ARM64 前端在 MSR FPCR 处结束单元，该校验目前不会失败 | 由 §4.3 取消中途退出 |
| R4 | x64 后端的 `update_fp` 与 `configure`，[runtime.cpp](../source/runtime/backend/x64/runtime.cpp) | `native_eflags` 和调度入口按后端全局选择，单次编译却可能因缺少 fault 位置而退回 packed flags | 潜在。VM 的编译请求总是满足条件 | 编译退回时返回错误 |
| R5 | `PrepareArmFPRegion`；rewrite 的不可缓存装载，[rewrite.cpp](../source/runtime/ir/passes/rewrite.cpp) | 访问宽度按 `TypeBits／8` 向下取整为 0，与 `UniformEffectsFor` 的向上取整不一致 | 潜在。当前前端不产生零宽或 Bool 类型的 uniform | 统一使用向上取整的范围查询 |
| R6 | `Compile` 新增的两个失败返回，[compile.cpp](../source/runtime/backend/x64/compile.cpp) | 没有执行已有失败路径的清理 | 诊断缺少节点和块信息 | 复用同一清理 |

## 4. 批次 1：浮点环境并入 native 合同

### 4.1 现有合同

linked 执行已经为 guest 维护一份 MXCSR。

- `SvmRunNative` 保存宿主 MXCSR，装入 `NativeRunContext::guest_fpcr`。`.Lcold` 与 `SvmNativeExit` 把当前 MXCSR 存回该字段。[entry_elf.S](../source/runtime/backend/x64/entry_elf.S)
- `PrepareCall` 在 helper 前执行 `stmxcsr [r12-40]` 和 `ldmxcsr [r12-56]`，分别对应 `guest_fpcr` 与 `host_fpcr`。`FinishCall` 重新装入 guest 值。[emit_calls.cpp](../source/runtime/backend/x64/emit_calls.cpp)
- fault 时 `RestoreSignalState` 把信号现场的 `fpregs->mxcsr` 写回 `guest_fpcr`。[x64_host.cpp](../source/runtime/platform/linux/x64_host.cpp)
- `VM::UpdateFPEnvironment` 为 ARM64 guest 计算控制字 `0x1f80 | RC(RMode) | (FZ ? 0x8040 : 0)`，由 `update_fp` 写入 `guest_fpcr`。

控制位这一半已经成立：编译到域 D 的单元运行时，MXCSR 的控制位就是 D 对应的常量。缺的是状态位。ARM64 guest 的 MXCSR 状态位目前无人合并，`ArmFPBinary` 因此自带区间来隔离和提交 IXC。

### 4.2 新增的状态位合同

对 x64 宿主上的 ARM64 guest 规定：guest 阶段内，MXCSR 低 6 位表示自上次收割以来原生浮点快路径产生、尚未并入 `GuestState::fpsr` 的异常。收割把这些位映射后 OR 进 FPSR，再清零。

| MXCSR | IE bit0 | ZE bit2 | OE bit3 | UE bit4 | PE bit5 | DE bit1 |
| --- | --- | --- | --- | --- | --- | --- |
| FPSR | IOC bit0 | DZC bit1 | OFC bit2 | UFC bit3 | IXC bit4 | 丢弃 |

映射为 `(x & 1) | ((x >> 1) & 0x1e)`。收割点如下。

| 收割点 | 实现 |
| --- | --- |
| 任何原因的 native 退出 | `VM::MergeFPStatus` 增加 ARM64 分支，在处理退出原因之前合并 `guest_fpcr & 0x3d` 并清除低 6 位。FPEnvironment 与 StateService 退出同样先合并 |
| fault 与 dispatch 中断 | 沿用 `RestoreSignalState` 回填的 `guest_fpcr`，随后由上一行合并 |
| helper 调用 | `PrepareCall` 在现有 `stmxcsr [r12-40]` 之后合并并清除保存值中的低 6 位，`FinishCall` 装回的环境不带状态位 |
| 原生访问 FPSR | `LoadUniform`、`StoreUniform` 的范围与 `fpsr` 重叠时，先发射收割序列。以 uniform 地址形式传给 helper 的访问由上一行覆盖 |
| 裸后端 | `Restore` 和非 linked 的 `PrepareCall` 发射同一收割序列 |

退出时先合并再执行体系结构写入，顺序与 guest 一致：MSR FPSR 覆盖已合并的位，MSR FPCR 保留它们。

生成代码中的收割序列如下，它使用 R10、R11，进入前保存仍然活跃的 native flags。检测包含 DE 的全部低 6 位，确保只有 DE 时同样清零。helper 返回值所在的 RAX 得以保留。该序列只出现在上表后三类位置。

```asm
stmxcsr dword [r12-40]
mov     r10d, [r12-40]
test    r10d, 0x3f
jz      done
mov     r11d, r10d
shr     r10d, 1
and     r10d, 0x1e
and     r11d, 1
or      r10d, r11d
or      dword [r12+fpsr], r10d
and     dword [r12-40], ~0x3f
ldmxcsr dword [r12-40]
done:
```

### 4.3 发码改动

- VM 显式设置 `arm_fpsr_offset`，`ArmFPBinary` 直接在入口装入的环境下执行。未提供该选项的裸后端调用者继续使用 `BeginArmFPRegion`、`EndArmFPRegion`、`PrepareArmFPRegion` 的区间协议，保留 `arm_fp_region` 与 stub 的 `region` 字段，以兼容现有调用方式。
- 单元入口的 FPCR 校验保持不变。取消单元中途的 FPCR 校验和由它产生的 FPEnvironment 退出：`allocation.fpcr_may_change` 为真的单元不发射快路径，其中的 `ArmFPBinary` 一律调用精确 helper 并在运行时读取 FPCR。快路径因此只存在于 FPCR 在单元内不变的代码中，R3 涉及的退出 PC、flags 发布和写回消除问题随之消失。当前 ARM64 前端在 MSR FPCR 处结束单元，其余 helper 都是保持 FPCR 的数值叶子，这类单元在实际负载中不出现。
- FZ=1 的域在环境中带有 DAZ 与 FTZ。现有准入保证输入和结果都是正常数或零，这两个控制位不会生效，数值与当前实现一致。
- VM 冷路径 stub 复用 native 环境，按调用点记录和恢复 flags 载体。裸后端区间分支仍参与 stub 身份。

### 4.4 验收

- 浮点循环与矩阵热路径的反汇编不含 `stmxcsr`、`ldmxcsr`。
- 定向回归：连续运算后读 FPSR；运算后 MSR FPSR 再读取；运算后 MSR FPCR 切域再读取；两次运算之间发生访存 fault，异常上下文的 FPSR 只含 fault 之前的位；运算之间经过冷路径 helper、cold resolve、事件退出、static link 和间接调度；全部 32 个域。
- 仅含整数与整数 NEON 的输入在退出时 `guest_fpcr` 低 6 位为零。该项检查其他原生指令的状态位污染。`ArmFPBinary` 与批次 7 新增的数值快路径共同遵守 pending 状态合同。
- 既有 27,648 次标量与 25,296 次 NEON 浮点差分逐位一致。

## 5. 批次 2：结果后查

### 5.1 语义依据

FZ=0、异常全部屏蔽时，加、减、乘在 x86 SSE 与 ARM 之间只有两类差异。

| 情形 | 数值 | 异常位 | 处理 |
| --- | --- | --- | --- |
| 结果为正常数、零、次正规数或无穷 | 相同 | PE／OE／UE 与 IXC／OFC／UFC 相同 | 快路径 |
| 结果恰为 ±MinNormal | 相同 | ARM 在舍入前判定 tiny，可能置 UFC。x86 在舍入后判定，不置 UE | 冷路径 |
| 结果为 NaN | NaN 的选择顺序、sNaN 优先级和默认 NaN 符号不同 | IE 与 IOC 相同 | 冷路径 |
| 输入为次正规数 | 相同 | x86 置 DE，FZ=0 时 ARM 没有对应位 | 收割时丢弃 DE |

tiny 判定的差异只出现在精确值略小于 MinNormal 且舍入到 MinNormal 的情形，交付结果必然是 ±MinNormal。进入冷路径的两类情形中，原生指令已置的状态位都是 ARM 正确结果的子集：NaN 情形只可能有 IE，MinNormal 情形只可能有 PE。冷路径的精确 helper 把完整结果 OR 进 FPSR，状态位是 sticky 的，重复置位没有影响。

DN=1 时 NaN 结果同样进入冷路径，由 helper 生成默认 NaN。四种舍入模式由 MXCSR 的 RC 直接实现。FZ16 不影响 F32 和 F64。因此 FZ=0 的 16 个域全部适用，FZ=1 的 16 个域保留现有输入准入。FZ=1 时 ARM 在冲刷次正规输入时置 IDC，冲刷 tiny 结果时只置 UFC，这两点与 DAZ／FTZ 的标志行为不同。

### 5.2 发码形态

SSE4.1 宿主上，标量与 packed 共用下面的形态，F32 换用 `pcmpeqd` 和 `cmpunordps`。常量放在单元冷区之后的常量池，以 RIP 相对寻址访问，不产生重定位。常量池按 16 字节对齐，并要求单元的装载地址保持 16 字节对齐。装载地址不保证对齐时，先用 `movdqu` 把常量取入 scratch 再运算。

```asm
movdqa     xmm0, lhs
mulpd      xmm0, rhs
movdqa     xmm14, xmm0
pand       xmm14, [rip+abs64]
pcmpeqq    xmm14, [rip+minnormal64]
movapd     xmm15, xmm0
cmpunordpd xmm15, xmm0
por        xmm14, xmm15
pmovmskb   ecx, xmm14
jrcxz      resume
jmp        cold
resume:
```

- 这段序列不改写 EFLAGS。`ArmFPBinary` 入口处的 `SaveNativeFlags(true)` 可以去掉，冷路径记录调用点的 flags 载体状态，返回前重建同一状态再合流。stub 键增加 native 与极性两位。
- F32 标量用 `movd` 清除高 96 位，F64 标量用 `movq` 清除高 64 位。packed 运算的活跃 lane 少于寄存器宽度时，同样清零非活跃 lane，避免其中的 sNaN 影响结果检查。
- 输入保存在各自的 home 中，结果在检查通过后才写入 home，冷路径可以重新读取输入。
- 没有 SSE4.1 的宿主按活跃 lane 提取整数位，清符号后比较 Infinity 和 MinNormal，识别 NaN 与 tiny 边界。该形态改写 EFLAGS，保留入口处的保存。

### 5.3 验收

- 边界语料覆盖 add／sub／mul、F32／F64、1／2／4 lane 和 16 个 FZ=0 域，逐项比较数值与 FPSR：精确的 MinNormal；从下方和上方舍入到 MinNormal；精确与不精确的次正规结果；下溢到零；四种舍入模式下溢出到无穷或最大有限数；qNaN、sNaN 分别位于各操作数位置及同时出现；`inf − inf` 与 `0 × inf`；次正规输入；四种舍入模式下的有符号零；非活跃 lane 含 NaN、次正规数和大数。语料生成沿用 [generate_arm64_fp_binary.py](../tools/correctness/generate_arm64_fp_binary.py)。
- 矩阵热区每条 packed 运算的宿主指令数从约 49 条降到 20 条以内。

## 6. 批次 3：条件直达

**复合条件。** `CondPassed` 在 x64 Known 目标上对 14 种条件各发射一个 `CondSet(cond)`，门控方式与 `X64FP()` 相同，其他目标保持现状。后端已有的三处逻辑直接生效：`AnalyzeFlags` 的条件读取掩码表，`InitializeHomes` 对块末 `CondSet` 的 `terminal_only` 判定，`Terminal` 经 `NativeCondition` 和 `NativeJump` 生成 Jcc。SUB 之后的 `InvertCarry` 使物理 CF 等于逻辑 C 的反，`b.hi`、`b.ls` 无需 `cmc` 即可使用 JA、JBE。非终结用途经 `Condition` 生成 SETcc。

**值测试。** `terminal_only` 的判定扩展到块末且单一用途的 `TestZero`、`TestNotZero`、`TestBit`，对应 CBZ、CBNZ、TBZ、TBNZ。

| 情形 | 发码 |
| --- | --- |
| 零测试，guest flags 当前驻留在 EFLAGS 且仍被需要 | `mov rcx, reg; jrcxz`，EFLAGS 不变 |
| 零测试，其他情形 | `test reg, reg; jz／jnz` |
| 位测试 | 按批次 4 的规则处理 flags 后 `test reg, imm` 或 `bt`，再 Jcc |

验收为 14 种条件乘 16 种 NZCV 输入，覆盖 B.cond、CCMP、CSET 的终结与非终结用途，以及 flags 活跃时穿过 CBZ、TBZ。排序内层循环的比较分支降到 4 条以内。

## 7. 批次 4：flags 载体

### 7.1 逐节点需求

`AnalyzeFlags` 目前只得到每个块入口的需求掩码。在块级不动点完成后再做一次块内反向遍历，为每个节点记录两个 4 位事实并存入 `Home`。

- `other_need`：该节点之后、被下一次覆盖之前，除本块终结条件以外的观察者需要的位。观察者的定义与现有分析相同，后继块的入口需求计入其中。
- `term_need`：本块终结条件读取、且来自当前版本的位。

第一次遍历把每个节点的读取位和输出位分别存入 `flag_reads`、`flag_writes` 两个字节，第二次遍历将两项需求打包到 `flags_need` 字节，只做位运算，不再查询 effects。

需求分析在 `ObserveUses` 完成跨块 SSA 使用统计之后执行。内部 `BindLabel` 合流清除路径上已物化的条件字节事实，由规范保存槽继续承载 flags，避免把仅在某一路径捕获的条件误用于其他路径。

### 7.2 破坏点的处理

`PrepareFlags` 在将要发射破坏 EFLAGS 的宿主指令、且 guest flags 驻留在 EFLAGS 时，按下表处理。

| 条件 | 动作 |
| --- | --- |
| `other_need` 与 `term_need` 均为零 | 直接丢弃，不发射任何指令 |
| `term_need` 等于终结条件的完整读取掩码 | 发射 `setcc byte [rsp+cond_slot]`，终结处用 `cmp byte [rsp+cond_slot], 0` 加 Jcc |
| `other_need` 非零 | 另外保存一次快照，可与上一行同时成立 |
| 终结条件只有部分位来自当前版本 | 保存快照，终结处从快照恢复 |

`cond_slot` 是帧内一个字节，每个单元一个，与 `flags_slot` 一起分配。SHA 轮循环由此变为一次 SETcc、一次快照保存和一次内存比较，`popfq` 消失。

### 7.3 EFLAGS 来源的 fault recipe

`RecordGuestAccess` 在访存指令执行时 guest flags 仍驻留于 EFLAGS 的情况下，写入 native 来源：`recovery[0] = 2 | (flags.carry << 2)`，`recovery[1] = 0x0f`。`RestoreGuestState` 和信号现场的 `REG_EFL` 传递已经支持这一来源，无需改动解码。

为使 EFLAGS 在访存处仍然有效，补两条保持 flags 的发码规则。

- `Memory` 的地址是寄存器 home，或可用 LEA 表达的 `Add`、`AddShift` 时，不保存也不失效 native。`Expression` 增加只用 LEA 的模式。
- guest flags 驻留在 EFLAGS 且仍被需要时，不产生 flags 的 `Add`、`Sub` 立即数形式和 `LslImm` 小移位改用 LEA。其余破坏 EFLAGS 的运算按 §7.2 处理。

### 7.4 循环头的入口约定

块入口约定增加 native 形式：EFLAGS 持有完整 NZCV，物理 CF 等于逻辑 C 的反。`AnalyzeFlags` 的块内遍历同时得出每个块的出口形态，取值为 native、保存槽或透传。循环头的所有回边前驱出口均为 native 时，该块采用 native 入口，回边不发射任何转换，极性不同的前驱补一条 `cmc`。入环的前向边和指向该块的外部入口桩负责从保存槽恢复一次。其他合流块保持保存槽约定。

访存循环由此不再有 flags 往返：回边保持 `sub` 的 EFLAGS，循环头的访存使用 native recipe，随后的 `add` 是破坏点，此时两项需求均为零。

### 7.5 LAHF 快照

保存槽内容由 raw RFLAGS 改为 16 位的 LAHF 映像，AH 为 `SF:ZF:0:AF:0:PF:1:CF`，AL 为 OF。

| 操作 | 发码 |
| --- | --- |
| 保存 | `mov r11, rax; lahf; seto al; mov word [slot], ax; mov rax, r11` |
| 恢复 | `mov r11, rax; movzx eax, word [slot]; add al, 0x7f; sahf; mov rax, r11` |
| 翻转保存值的 carry | `xor word [slot], 0x100` |
| ADC／SBB 取 carry | `bt word [slot], 8` |

N、Z、C、V 分别位于 bit15、bit14、bit8、bit0。`PackFlags`、部分更新的合并、`ImportPackedFlags`、汇编中的 `IMPORT_EFLAGS` 与 `PACK_EFLAGS`、`NativeRunContext::eflags` 的跨单元交换，以及 `RestoreGuestState` 读取保存槽的位序都改用这一映像。EFLAGS 来源的位序不变。

保存和恢复都经 R11 暂存 RAX，覆盖 `SelectValue`、部分 flags 更新和 helper 返回等结果仍在 RAX 的调用点。LAHF／SAHF 在 64 位模式下的可用性由 CPUID 检测并进入配置身份，缺失时保留 PUSHFQ 路径。`NativeRunContext::native_eflags` 为 0 时采用 packed 调度 ABI，为 1 时交换 raw RFLAGS，为 2 时交换 LAHF／OF 映像，信号恢复和汇编入口按相同格式判别。

### 7.6 packed 值权威时的路径

guest flags 的权威值在 RBX 时，当前实现仍把条件消费引向 EFLAGS，全 flags 的 packed producer 之前也先物化一次。这两条路径主要影响 x86 guest，以及 ARM guest 中 `fcmp` 和 helper 之后的条件。

| 位置 | 当前行为 | 改动 |
| --- | --- | --- |
| `EnsureNativeFlags` | `flags.dirty` 为假时先执行 `ImportPackedFlags`，约 17 条指令加一次存储，再 `push; popfq` | `flags.dirty` 为假时条件直接测试 RBX 中的位，沿用 `Condition` 和 `Terminal` 已有的 packed 分支 |
| `PrepareFlags` | 任何读写 flags 的非 native 节点之前都调用 `MaterializeFlags`，约 20 条指令 | 该节点覆盖全部 NZCV 时直接丢弃当前载体 |
| `AnalyzeFlags` | `VecLoadConst` 的 schema effect 为 Unknown，被计为完整的 flags 读取者 | 与 `PrepareFlags` 一致，按不读取 flags 处理 |

### 7.7 随批次整理的重复逻辑

审查同时指出三处需要手工保持一致的重复，在批次 4 改动同一批代码时收敛。

- fault recipe 的字节布局同时写在 `RecordGuestAccess`、`RestoreGuestState` 和 [code_image.cpp](../source/runtime/execution/code_image.cpp) 的镜像校验中。校验改为调用后端提供的函数。
- `AnalyzeFlags` 重新实现了 `ir/passes/liveness.cpp` 中逐 opcode 的 flags 读取分类。由 IR 层导出同一函数。
- `TestFlags` 掩码到条件的映射在 `Terminal`、`NativeFlagTest` 和 `PrepareFlags` 各有一份。改为一个返回可选条件的函数。

CoreMark 配对测量显示，x86 guest 的 mixed native-flags 路径仍有额外成本。VM 对 x86 guest 采用 packed flags 与 packed 调度 ABI，ARM32／ARM64 采用 native flags。packed 调度入口与裸后端 `SelectValue` 的非 local 分支保留，公共编译接口继续支持显式选择 local flags。

### 7.8 验收

- 16 种 NZCV 与两种极性的映像往返；部分更新合并；ADC／SBB 从映像取 carry；named snapshot。
- 循环头访存 fault 时由 EFLAGS 来源恢复上一轮的 flags，覆盖两种极性、保存槽来源和混合来源。
- 条件物化覆盖 14 种条件，以及终结条件部分位被后续 producer 覆盖的情形。
- static link、间接调度、cold resolve、SMC 失效和 L1 probe 处的 dispatch 中断使用新映像。
- x86 guest 的 1,278 次基线状态比较保持全字段一致，PF 来源、AF、DF 仍由 RBX 基值提供。x86-64 CoreMark 的热执行不低于 `52f04abd` 构建。
- SHA 轮循环和访存循环的反汇编不含 `pushf`、`popf`。

## 8. 批次 5：整数发码

| 改动 | 位置 | 内容 |
| --- | --- | --- |
| 32 位 ROR | `ShiftOperand` | 32 位分支改用已有的 `RorImm<Type::U32>` |
| 恒等折叠 | `Fold` 及 `RewriteValues` 的拷贝转发 | `x\|0`、`x^0`、`x+0`、`x-0`、零位移、`x&全一` 转发为 `x`，`x&0` 折叠为零。仅在结果类型与操作数类型一致时生效 |
| 32 位指令形式 | `Logical` | 宽度为 32 时使用 32 位操作数，省去随后的 `mov r32,r32` |
| 右操作数直达 | `Arithmetic`、`Logical`、`Mul` | 右操作数是驻留在寄存器 home 的单个值时直接作为源操作数，不经 RCX |
| V128 uniform 直达 | `Uniform` | 装载直接写入结果 home，存储直接读取源 home |
| 立即数存储 | `Uniform` 与 rewrite | 源为常量时生成 `mov [r12+off], imm32` |
| reservation 同值写 | uniform effects 与 rewrite | 同一块内已清零且期间没有可能置位的节点时，删除重复的清零 |

验收包括 ROR 位移量 1—31 的语义、恒等折叠的类型与视图规则、32 位运算结果的高位清零、源与目的 home 相同的情形。SHA 的 `gpr move` 采样占比和 `LslImm`、`Or` 节点数作为对照指标。

## 9. 批次 6：uniform 转发

SHA 负载的六个编译单元合计有 191 个 `LoadUniform`，其中一部分是 8 项转发表淘汰后的重复装载。扩大转发范围会延长 SSA 值的生命期，在 6 个标量 home 的条件下可能把一次装载换成一次 spill 存储加一次装载。因此分两步。

1. 把 `UniformCache` 改为按 `offset >> 3` 直接索引、以代数标记失效的表，查询和清空均为 O(1)。先统计 SHA、综合计算和完整程序在不同容量下被转发的装载数与 allocator 产生的真实 spill 数。
2. 统计显示 spill 增加时，为 allocator 增加由 uniform 槽支撑的值：某个值与一个 uniform 槽内容相同、且该槽在其最后一次使用前没有被覆盖时，淘汰它不产生 spill 存储，重新使用时从 `[r12+off]` 装载。

是否执行第 2 步由第 1 步的数据决定。

实施中采用 128 项直接映射表与独立的有效、pending epoch。测量确认扩容会增加 spill，因此同时实施 uniform 支撑。allocator 对单块内的装载和相邻写回的 U32／U64／V128 计算结果反向验证槽位稳定性，排除 pinned 与特殊状态字段、跨块生命期和不明副作用。计算结果与写回之间最多允许两个无元数据的 Nop，结果被淘汰时直接提交对应 guest 槽位，原 StoreUniform 复用这次提交。普通内存写在 `private_state` 未声明时构成别名屏障。

转发表的重叠查询覆盖前方 31 字节，使 portable IR 中 V256 pending store 的尾部观察同样生效。定向回归覆盖对齐与未对齐的 V256 写、尾部 U64／Bool 读取及后续完整覆盖。

## 10. 批次 7：扩大原生浮点覆盖

在批次 1 的环境合同和批次 2 的后查与冷路径之上，按出现频率扩展。每一项都以 helper 作为冷路径，数值与 FPSR 以现有 oracle 逐位比较。

| 顺序 | 指令 | 原生形态 | 冷路径条件 |
| --- | --- | --- | --- |
| 1 | FCMP、FCMPE、FCCMP | `ucomisd`／`comisd`，生成 NZCV nibble，经 `LSL 28` 与完整 `RestoreFlags` 发布 | FZ=1 的次正规输入 |
| 2 | FMADD 族、FMLA、FMLS | 宿主具备 FMA 时用 `vfmadd` 族 | NaN 结果、MinNormal 结果 |
| 3 | FABS、FNEG、FMOV | 位运算与寄存器搬运 | 无 |
| 4 | SCVTF、UCVTF、FCVTZS、FCVTZU | `cvtsi2ss／sd`、`cvttss／sd2si`，转换前分类并验证范围 | U64 输入高半区、NaN、越界与带符号差异 |
| 5 | FDIV、FSQRT | `divsd`、`sqrtsd` | NaN 结果、MinNormal 结果 |
| 6 | FCVT 精度转换 | `cvtss2sd`、`cvtsd2ss` | NaN 结果、MinNormal 结果，FZ=1 的 F64→F32 |

FMIN、FMAX 及其 NM 形式和 FRINT 族的语义差异较多，放在上表之后。各项的条件码映射和边界语料在批次 2 完成后细化。

新覆盖复用现有 `ExecuteNumeric` 精确叶子的指令编码和 effects，由 x64 emitter 识别匹配的状态布局和常量指令，生成原生热路径与逐点冷回退。FMA 还受宿主 FMA／OS 状态能力门控。FP 比较发布四位 NZCV，后继使用 packed 条件路径，当前实现没有另增浮点物理 flags 载体。

## 11. 验收与测量

**正确性。** 每个批次重跑 CTest，结果应保持 31 通过、7 项其他宿主 ISA 跳过。集成后重跑 57,168 次 ARM 差分、1,278 次 x86 基线状态比较、24 组完整程序以及磁盘缓存与 AOT。各批次的定向回归见对应小节。

**性能。** 协议沿用上一轮：CPU2、Function／pin0，冻结基线、候选与 Dynarmic 轮换并反转顺序，各 31 个新进程，每进程 3 次热执行取中位数，报告配对比值及四分位。PMU 检查 `enabled_ns == running_ns`。Block 与 pin2 另作各 9 个进程的扩展组。

| 负载 | 验收形态 | 按采样占比的估算 |
| --- | --- | --- |
| 浮点 | 热循环无 MXCSR 存取 | 9.66 ms 降到 2—3 ms |
| 向量矩阵 | 无 MXCSR 存取，每条 packed 运算 20 条以内 | 0.77 ms 降到 0.25 ms 左右 |
| SHA | 轮循环无 `pushf`、`popf`，32 位旋转为单条 ROR | 6.57 ms 降到 4—5 ms |
| 访存 | 每轮 15 条以内，无 flags 往返 | 19.2 ms 降到 14 ms 左右 |
| 排序 | 比较分支 4 条以内 | 0.82 ms 降到 0.6—0.7 ms |
| 整数、移位、浮点展开 | 配对比值不高于 1.02 | 持平 |

估算由 §1.2 的占比和 §1.3 的单项成本线性外推，以实测为准。任何负载的配对中位数退化超过 2% 时，先定位再继续下一批次。

**编译成本。** 沿用 ARM64 完整程序 31 组交替冷运行和 N、2N、4N 的规模 probe，分别报告 Frontend、Optimize、Backend、Publication 和代码字节。

| 批次 | 新增或减少的工作 |
| --- | --- |
| 1 | VM 热路径省去逐边界的区间开合，裸后端继续使用兼容路径 |
| 2 | 每条运算的检查指令减少，每单元增加至多 64 字节常量池 |
| 3 | 条件节点数减少，终结判定增加三个 opcode 比较 |
| 4 | 每块增加一次只含位运算的反向遍历，每节点三个 flags 事实字节 |
| 5 | 每节点常数次恒等判断，IR 节点数减少 |
| 6 | 转发表查询由 8 项扫描变为直接索引 |

验收线为完整程序冷编译总量不高于基线的 103%。

## 12. 代码修改清单

| 文件 | 批次 | 主要修改 |
| --- | --- | --- |
| `execution/runtime.cpp`、`execution/host_backend.h` | 1 | ARM64 状态位合并与清除，退出原因处理前调用 |
| `backend/x64/emit_float.cpp`、`emit.h` | 1、2 | native 环境合同，收割序列，结果后查，常量池，冷路径合流状态 |
| `backend/x64/emit_arm_float.cpp` | 7 | 识别精确数值叶子，新增原生浮点形式与冷回退 |
| `backend/x64/emit_calls.cpp`、`emit.cpp`、`emit_memory.cpp` | 1 | helper、裸后端出口和 FPSR uniform 访问处的收割 |
| `frontend/arm64/semantics_registers.inc`、`semantics.h` | 3、5 | `CondSet` 发射，32 位 ROR |
| `backend/x64/allocate.cpp`、`allocate.h` | 3、4 | 值测试的终结判定，逐节点需求，块出口形态，`cond_slot` |
| `backend/x64/emit_flags.cpp`、`emit.cpp`、`emit_control.cpp` | 3、4 | 破坏点处理，条件物化，入口约定，LAHF 映像 |
| `backend/x64/emit_operand.cpp`、`emit_scalar.cpp`、`emit_memory.cpp` | 4、5 | LEA 形式，32 位指令，操作数与 V128 直达 |
| `backend/x64/entry_elf.S`、`recovery.cpp`、`runtime.cpp` | 4 | 映像的导入、打包、恢复位序和 LAHF 能力检测 |
| `ir/passes/fold.cpp`、`rewrite.cpp`、`ir/uniform_effects.h` | 5、6 | 恒等折叠，同值写消除，转发表 |
| `frontend/arm64/semantics_float.inc` | 7 | 新增原生浮点形式的发射 |
| `tools/bench_dynarmic.py`、`benchmarks/` | 0 | 新内核、FMA 变体与 x86-64 CoreMark 对比 |
| `backend/x64/emit_scalar.cpp`、`compile.cpp`、`runtime.cpp`，`execution/offline_compile.cpp`、`code_image.cpp` | 0、4 | §3.2 的前置修复，recipe 校验改由后端提供 |
| `source/tests/rebuild` | 全部 | 各批次的定向回归 |

表中路径均位于 `source/runtime` 下，`tools`、`benchmarks` 和 `source/tests` 除外。生成代码的合同变化随源码进入 `CodeBuildId` 指纹，旧构建的缓存与 AOT 不会被新构建加载。`NativeRunContext` 布局不变。

## 13. 风险与未纳入事项

| 风险 | 控制 |
| --- | --- |
| 批次 1 依赖原生浮点快路径正确累计 MXCSR 状态位 | §4.4 的整数输入状态位为零检查，新增原生浮点形式时同步审计 |
| 批次 2 的语义等价性来自分析 | §5.3 的边界语料逐位比较，冷路径条件宁可多不可少 |
| 批次 4 的映像读写占用 RAX | 保存与恢复始终经 R11 保护 RAX，覆盖 CMOV、部分更新和 helper 返回回归 |
| 批次 4 同时改动跨单元交换格式 | static link、调度、cold resolve、SMC 与 dispatch 中断的回归先于性能测量 |
| 批次 6 可能增加 spill | 先测量，按数据决定第 2 步 |

以下三项有证据但改动面大，不在本方案内。

- **循环内 guest 寄存器驻留。** 整数循环 83.1% 的采样落在 guest 状态读写上。把循环携带的 guest 寄存器留在宿主寄存器需要跨块的值传递或按单元选择 pin，涉及 IR 与所有后端。
- **扩大标量寄存器池。** 16 个 GPR 中只有 6 个可分配，另有 7 个固定为 scratch。释放其中一部分需要逐个审计 handler 的寄存器用法。
- **延迟写回与 fault 恢复表。** 让 uniform 写回消除跨越访存，需要在 fault 位置记录 guest 寄存器到宿主位置的映射。

Frontend 占冷编译约 52%，SHA 负载的 IR 中 `Nop` 节点有 322 个。其耗时构成尚未剖析，建议在批次 5 之后单独测量。
