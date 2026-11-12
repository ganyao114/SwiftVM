# LoongArch LBT 优化方案

状态：方案（未实施）。当前 LA64 后端仍走基础 ISA 的软件 flags 路径；本文给出基于 `lbt_x86` 的分阶段落地方案、验证门和回退约束。基线提交 `71a9b2c0`。

## 0. 结论摘要

- 基础 LoongArch64 没有 ALU flags 寄存器（只有浮点条件码 `$fcc[0-7]`），也没有标量 PCNT；x86 EFLAGS 目前由 LA64 后端用 GPR `r31` 的 packed flags 软件模拟。
- LBT（Loongson Binary Translation）扩展提供一整套 x86 EFLAGS/条件/ADC 辅助指令：`x86add/sub/adc/sbc/inc/dec/and/or/xor/shift/rotate`、`x86mfflag/x86mtflag`、`SETJ`、x87 TOP/tag 等。LBT4 低 32 位是 eflags，高 32 位是 ftop；LBT0–3 是 scratch。
- Box64 的 LA64 后端在 `cpuext.lbt` 为真时已经使用这套指令，并保留软件 flags 回退；它没有使用 `movgr2scr/jiscr`、非对齐 `ldl/ldr` 或 x87 TOP（TOP 只有 emitter 定义，未接入）。
- SwiftVM 的 LA64 `HostFeatures` 已能解码 HWCAP `lbt_x86`（bit 10），但 baseline 明确不依赖 LBT；目前没有 LBT 执行路径。
- **最大阻塞**：上游 QEMU 的 LoongArch decode 没有任何 LBT 指令，本地 `qemu-loongarch64 8.0.4` 也不能执行；当前没有 LA64 真机。P1 起必须有 3A5000/3A6000（LBT 内核）或支持 LBT 的 QEMU fork 才能验证语义和性能。
- 建议：先做 P0（特性门控、assembler 编码、编译期验证），P1+ 等验证载体到位后再合入执行路径；无 LBT 时保持现有软件路径和基础 ISA 优化。

## 1. 背景与证据

### 1.1 基础 ISA

- LoongArch Vol.1 的非特权整数指令没有 ALU flags 寄存器；分支使用 `BEQ/BNE/BLT/BGE/BLTU/BGEU`，浮点比较写 `$fcc`。
- 标量指令没有 `pcnt.d/w`；GNU as 2.41 和 LLVM 21 均拒绝，只有 LSX/LASX 的 `vpcnt/xvpcnt`。POPCNT/parity 继续用 SWAR。
- 现有 LA64 flags 实现：`r31` packed flags，PF 源字节 `[7:0]`、DF `10`、AF `26`、OF `28`、CF `29`、ZF `30`、SF `31`；`emit_flags.cpp` 负责合并/读取，`emit_scalar.cpp` 负责算术/逻辑 flags 生成。

### 1.2 LBT

GNU as 2.41 已支持 LBT，本机可汇编并抓取编码。例如：

| 指令 | 编码 |
| --- | --- |
| `x86add.w $t1,$t2` | `0x003f39a6` |
| `x86add.wu $t1,$t2` | `0x003f39a0` |
| `x86adc.w $t1,$t2` | `0x003f39ae` |
| `x86and.w $t1,$t2` | `0x003fb9b2` |
| `x86mfflag $t0,0` / `x86mtflag $t0,0` | `0x005c000c` / `0x005c002c` |
| `x86inc.w $t1` / `x86dec.w $t1` | `0x000081a2` / `0x000081a6` |

LBT 的 x86 算术/逻辑/移位指令是两寄存器（或单寄存器 inc/dec）形式，标志-only，结果仍由基础指令计算；`x86mfflag/x86mtflag` 的 imm8 是标志掩码（CF/PF/AF/ZF/SF/OF），不是寄存器选择子。LBT4 是独立的 64 位状态（低 32 eflags、高 32 ftop）；DF 不在 LBT4 中。

### 1.3 Box64 参考实现

Box64 commit `e4d0022` 的 `src/dynarec/la64/`：

- 检测：`src/os/hostext_linux.c:289` 读 CPUCFG2 bit 18；`BOX64_DYNAREC_NOHOSTEXT=lbt` 可关闭；`src/libtools/sigtools.c:824` 处理信号帧中的 LBT eflags。
- flags 生成：`dynarec_la64_emit_math.c:40` 的 ADD/SUB、`:1051` 的 ADC、`:701` 的 SBC；`emit_logic.c:33`；`emit_shift.c:37/110/209`。
- 条件消费：`X64_SETJ`（`la64_emitter.h:2531`）在 `dynarec_la64_0f.c:1001/1594/1647`、`660f.c:1380`、`f30f.c:231` 使用，直接读 OF/SF/ZF/CF/PF 得到 0/1。
- 边角修正：`X64_GET_EFLAGS/SET_EFLAGS`（`la64_emitter.h:2527`）用于移位 OF/AF、x87 等场景。
- 实际使用：x86 算术/逻辑/移位、ADC/SBB、SETJ、`ROTRI_B/ROTRI_H`；x87 TOP/stack-mode 只有 emitter 定义，未在 x87 dispatch 中调用。
- 未使用：`movgr2scr/movscr2gr`、`jiscr0/jiscr1`、`ldl/ldr/stl/str` 只出现在 `la64_printer.c` 反汇编表，不在 codegen。

### 1.4 当前 SwiftVM 状态

- `source/runtime/backend/loongarch64/host_features.h:39` 已解码 HWCAP `lbt_x86`（bit 10）、`lbt_arm`（11）、`lbt_mips`（12），但仅用于能力记录。
- `source/runtime/backend/loongarch64/README.md:17` 明确：baseline 不要求/不宣称 LSX、LASX、LBT。
- 当前 flags 路径：`emit_flags.cpp` 的 packed flags 合并/读取，`emit_scalar.cpp` 的 IntegerArithmetic/LogicalArithmetic，以及 `emit_control.cpp` 的 Select/CondSelect。
- 已落地的基础 ISA 优化（提交 `71a9b2c0`）：BSTRPICK/BSTRINS、MASKEQZ/MASKNEZ、ROTRI、单 bit TestBit、无分支 Select；这是无 LBT host 的长期回退。

## 2. 目标与非目标

### 2.1 目标

- 在 `lbt_x86` 可用时，用 LBT 指令直接产生/消费 x86 EFLAGS，替代当前约 20–30 条软件 flags 序列。
- `cmp/test + Jcc/SETcc/CMOV` 的条件求值压到 1 条 `SETJ` + 分支/选择。
- 保留完整软件回退；LBT 能力进入 code image 和 AOT artifact key。
- 先覆盖 x86 guest；首期优先算术/逻辑/移位、ADC/SBB、Jcc/SETcc/CMOV；x87 TOP/通用 LBT 作为后续可选。

### 2.2 非目标（首期）

- `movgr2scr/movscr2gr`、`jiscr0/jiscr1`：LAT 翻译器运行时专用，Box64 未用，语义未公开，跳过。
- LBT 非对齐 `ldl/ldr/stl/str`：Box64 也未用；作为独立后续项评估，不进入 EFLAGS 主线。
- `lbt_arm`/`lbt_mips`：另立项目；本方案只针对 x86 guest。
- x87 TOP/stack-mode：P3 可选；先保证 x87 正确性，性能收益需实测。
- 不改变基础 ISA 回退路径的语义和现有测试。

## 3. 设计

### 3.1 特性探测与门控

新增运行时能力 `lbt_x86`，建议同时检查：

1. **HWCAP bit 10**：内核声明 LBT 上下文切换/信号帧支持；
2. **CPUCFG2 bit 18**：CPU 声明 LBT 指令支持；
3. 显式开关：`--lbt auto|on|off`（默认 `auto`）与环境变量（例如 `SVM_LBT=0`）用于调试/规避；
4. 把有效 LBT 能力写入 `CodeImageKey`、磁盘缓存 key 和 AOT artifact key，跨能力加载必须失配拒绝。

说明：Box64 只用 CPUCFG2；SwiftVM 的 HWCAP 检测语义更保守，建议两者都满足才启用。若内核不保存 LBT 状态，绝不能执行 LBT。

### 3.2 flags 状态模型

- **LBT 路径以 LBT4.eflags 为快速路径的规范 flags**；`r31` packed flags 保留给 DF、快照和跨边界导出，不再在每条算术指令后同步两份 flags。
- 生产者：
  - `Add/Sub/Neg/Inc/Dec/And/Or/Xor`：LBT `x86*` flags-only + 基础结果指令；
  - `Adc/Sbb`：优先使用 LBT 三操作数 `ADC/SBC`（结果 + flags，消费 LBT4.CF）；若目标 CPU 不支持该形式，退回 flags-only + 基础带进位序列；
  - shifts/rotates：LBT `x86sll/srl/sra/rotl/rotr/rcl/rcr`；OF/AF 按 Box64 方式用基础指令计算后 `SET_EFLAGS(mask)` 修正（仅在需要时）；
  - `MUL/IMUL` 的 CF/OF 可后续接入 LBT `x86mul`。
- 消费者：
  - `CondSet`/`LocalCondSet`/`CondSelect`/Jcc/SETcc/CMOV：`SETJ(rd, cond)` 直接得到 0/1；16 种条件全覆盖；
  - `TestFlags/TestNotFlags` 的标准条件映射 `SETJ`；非标准掩码用 `GET_EFLAGS(mask)` + AND/SNEZ；
  - `GetFlags/SaveFlags/RestoreFlags/BranchOnlyFlags/ClearFlags/InvertCarry`：`GET/SET_EFLAGS(mask)`；
  - PUSHF/POPF/LAHF/SAHF：掩码 GET/SET + DF 由 packed flags 处理。
- 边界转换：helper ABI、异常/信号、快照/auxiliary home、跨 unit 入口/退出处，先 `GET_EFLAGS(full)` 导出到 packed flags；进入 LBT 路径前 `SET_EFLAGS(mask)` 导入。热路径不做双状态同步。

### 3.3 IR/后端映射

- 初期不新增 IR opcode；LA64 后端在 `Writer` 内根据 `options.lbt_x86` 选择 LBT 低层发射。
- `IntegerArithmetic`：LBT 下改走 `x86add/sub` + 基础 ADD/SUB；不再调用完整 packed flags 计算。
- `LogicalArithmetic`：LBT 下改走 `x86and/or/xor` + 基础逻辑结果。
- `SetCarry/SetOverflow`：能由 LBT 生产者直接产生的，省略；不能覆盖的（如部分移位 OF/AF）用 `GET/SET_EFLAGS`。
- `Condition`/`CondSet`：改用 `SETJ`。
- `SaveProducerFlags`/`MergeFlags`：LBT 模式下保留为边界导出；不参与热路径。
- `SelectValue`：现有无分支 MASKEQZ 路径保留；CondSelect 可用 `SETJ` + MASKEQZ/MASKNEZ，按测量取舍。

### 3.4 assembler

- xbyak 没有 LBT 编码；在 LA64 `Assembler` 增加 `Emit32` 包装或补 xbyak 表项，覆盖：
  - `x86add/sub/adc/sbc/inc/dec/and/or/xor` 及 `.b/.h/.w/.d/.wu/.du` 变体；
  - `x86sll/srl/sra/rotl/rotr/rcl/rcr` + 立即数形式；
  - `x86mfflag/x86mtflag`、`SETJ`、`ROTRI_B/H`、ADC/SBC 三操作数形式；
  - 后续按需加 x87 TOP/stack-mode。
- 编码单测：与 GNU as 2.41 / Box64 emitter 的 word 表逐字比较；覆盖 16 个 SETJ 条件、GET/SET mask 组合和位宽后缀。
- 需要同时验证 `--lbt off` 时 assembler 不发出任何 LBT 指令。

### 3.5 AOT、缓存与上下文

- `RuntimeOptions`/`CodeImageKey`/AOT artifact 增加 LBT 能力位；LBT 产物不能跨到非 LBT 设备，反之亦然。
- LBT4 是 per-thread 状态；signal/fault frame 必须保存/恢复 eflags（必要时 ftop）；参考 Box64 `sigtools.c`。
- 线程切换、fork/exec、guest 异常返回、AOT 进入/退出、helper 调用前后，检查 flags 一致性。
- 若内核未提供 LBT context（例如非 LBT 内核），强制 `lbt_x86 = false`。

## 4. 实施阶段

| 阶段 | 内容 | 验收 |
| --- | --- | --- |
| **P0 规范与基础设施** | 确认 LBT 语义（Vol3/真机/Box64）；补 assembler 编码与单测；HWCAP+CPUCFG 探测、CLI/env、cache/AOT key；编译期可生成但默认不执行 | 编码与 GNU as/Box64 逐字一致；`--lbt off`/非 LBT host 零行为变化；构建通过 |
| **P1 Add/Sub/Jcc 试点** | Add/Sub/Neg + CondSet/Jcc 走 LBT；其余仍软件路径（混合） | 真机 `cmp/add + Jcc` 与软件路径/Unicorn 完全一致；指令数从 ~30 降到 ~4–6；程序套件无回归 |
| **P2 全量 EFLAGS** | Inc/Dec、And/Or/Xor、ADC/SBB、shifts/rotates + OF/AF 修正；SETcc/CMOV；PUSHF/LAHF/SAHF；快照/ABI 边界 | 定向 EFLAGS corpus（位宽 × 标志 × 条件）全通过；信号/异常/线程上下文通过 |
| **P3 x87 与通用 LBT** | x87 TOP/stack-mode（若收益明确）、`ROTRI_B/H`；可选评估 `ldl/ldr` | 定向 x87/8/16 位旋转通过；仅真机有收益时启用 |
| **P4 收尾** | AOT/disk cache 能力绑定、性能调优、文档/证据 | AOT 跨能力失配拒绝；目标基准达标；回退路径保持 |

## 5. 验证矩阵

- **差分**：同一 Case 分别 `--lbt off/on` 执行，比较完整 GuestState（GPR/flags/内存/异常）；oracle 继续用 Unicorn（x86 语义）。
- **定向 corpus**：ADD/SUB/ADC/SBB/INC/DEC/AND/OR/XOR/SHL/SHR/SAR/ROL/ROR/RCL/RCR × 8/16/32/64 位 × CF/PF/AF/ZF/SF/OF 边界 × 消费端（Jcc 16 条件、SETcc、CMOV、PUSHF/POPF、LAHF/SAHF、ADC 链）。
- **上下文**：信号投递/返回、SIGSEGV 恢复、clone/thread、fork、exec、AOT 进入/退出、helper 调用前后 flags。
- **程序级**：hello/compute/fp/bench_int/bench_fp、x86 busybox 常用命令；native vs LBT on/off。
- **非 LBT host**：QEMU 和 no-LBT CPU 强制软件路径，现有测试全绿；不得出现 SIGILL。

## 6. 性能测量

- 指令计数/代码尺寸：`cmp+jz`、`add+js`、`adc` 链、`setcc`、`cmov`、`shl+jc` 的 `native_bytes`/word 数，LBT on/off。
- 真机 wall time：同一程序 LBT on/off，重复取中位数；目标先定“Jcc 序列 ≥5× 指令减少”，程序级至少不劣化、分支密集程序有可测提升。
- QEMU TCG 不作为 LBT 性能结论，只用于非 LBT 回退验证。
- 保留现有基础 ISA 优化（BSTRINS/BSTRPICK/MASKEQZ/ROTRI、branch-only flags）作为无 LBT host 的对照和回退。

## 7. 风险与回退

- **验证载体缺失**：这是首要阻塞。没有 LBT 真机或 QEMU fork，P1 之后不能合入执行路径。
- **语义未完全公开**：SETJ 条件表、GET/SET mask、ADC/SBC 三操作数、AF/OF 边角以真机/Box64 为准；不能只靠专利推断。官方 Vol.3 文档仍是 TBD。
- **双 flags 状态**：LBT4 与 packed flags 的转换点若设计不好会吃掉收益；坚持“热路径只用 LBT4，边界才转换”。
- **DF/快照/信号**：DF 不在 LBT4；PUSHF/POPF、异常恢复、线程切换必须显式导出/导入。
- **CPU/内核差异**：LBT 可选；HWCAP+CPUCFG 双门控 + 显式关闭；非 LBT 内核禁止执行。
- **AOT 失配**：LBT 能力必须进入 artifact key，跨能力加载直接拒绝。
- **回退**：任何 LBT 语义不确定或测试失败，立即回退到现有软件 flags 路径；不保留半启用状态。

## 8. 待确认问题

1. 官方 LBT 卷三的 eflags 布局、SETJ 条件表、GET/SET mask、ADC/SBC 三操作数语义。
2. LBT4 是否包含 DF；AF/PF 是否在所有 x86* 指令中与 x86 完全一致。
3. 内核 HWCAP 与 CPUCFG 的 LBT 声明是否一致；信号帧/线程切换的 LBT 状态保存范围。
4. 是否有可用的 LBT QEMU fork；否则需要 3A5000/3A6000 真机（lbt_x86）或等价测试载体。
5. `ldl/ldr` 非对齐 LBT 的 fault/宽度语义；是否值得替代现有 byte fallback。
6. x87 TOP/stack-mode 的实际收益（Box64 目前未接入）。

## 9. 参考

- 本地代码：
  - `source/runtime/backend/loongarch64/host_features.h`
  - `source/runtime/backend/loongarch64/README.md`
  - `source/runtime/backend/loongarch64/emit_flags.cpp`
  - `source/runtime/backend/loongarch64/emit_scalar.cpp`
  - `source/runtime/backend/loongarch64/emit_control.cpp`
  - `source/runtime/ir/opcodes.inc`（flags IR）
- Box64 `e4d0022`：`src/dynarec/la64/la64_emitter.h:2495+`、`dynarec_la64_emit_math.c:40/1051`、`emit_logic.c:33`、`emit_shift.c:37/209`、`dynarec_la64_0f.c:1001`、`src/os/hostext_linux.c:289`、`src/libtools/sigtools.c:824`。
- [LoongArch Reference Manual Vol.1](https://loongson.github.io/LoongArch-Documentation/LoongArch-Vol1-EN.html)
- [loongarch-opcodes LBT list](https://github.com/loongson-community/loongarch-opcodes)
- [GNU Binutils 2.41 LoongArch LBT support](https://lists.gnu.org/archive/html/info-gnu/2023-07/msg00009.html)
- [QEMU target/loongarch/insns.decode（无 LBT 指令）](https://github.com/qemu/qemu/blob/master/target/loongarch/insns.decode)
- [LoongArch Documentation（Vol.3 TBD）](https://github.com/loongson/LoongArch-Documentation)
- [Old World / New World low-level details（LBT sc_scr/eflags/ftop）](https://areweloongyet.com/en/docs/world-compat-details/)
