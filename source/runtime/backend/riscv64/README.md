# RV64 后端

当前后端直接消费重建后的 `ir::Program`，与 ARM64 共用前端语义、Uniform IR、优化器、CodeHeap、PC 目录、link／fault 数据结构和 SMC 生命周期。默认 `JitOnly`；显式 Mixed 才按冷入口解释、热入口编译。没有恢复旧 IR/HIR、Phi 展开链、JitContext、独立 code cache 或持久化管理层。

基线为 RV64G／LP64D（不强制要求 V、Zbb 或 Crypto 扩展），同时通过 Linux `SYS_riscv_hwprobe` 动态探测并启用 `Zba`、`Zbb`、`Zbs`、`Zicond`、`Zbc`/`Zbkc`、`Ztso` 及快速非对齐标量访存（`ual`）硬件快路径（可通过 `SVM_RV64_FEATURES` 覆盖，特性掩码绑定至代码缓存键）。整数与 FP 原始值使用 GPR home，V128 使用两个 GPR 或 16B spill；FP 运算由 F/D 指令执行，整数向量优先走 64-bit SWAR 并行快路径，其余按 lane 生成 GPR 指令。Crypto、x87、宽除法等沿用数值 helper 或标准 ABI callback，不调用 IR direct，也不在运行时分派 IR opcode。

## 文件职责

| 文件 | 职责 |
| --- | --- |
| `host_features.h` | 基于 `SYS_riscv_hwprobe` 的无信号宿主扩展探测、`SVM_RV64_FEATURES` 策略与缓存特性掩码 |
| `compile.h/.cpp` | 中立目标能力（含 `ShiftedAddress`）、CompileOptions、短跳自适应重试编译、复用输出／allocation |
| `allocate.h/.cpp` | 独立 Block／Function 分配、实际 CFG/RPO、回边存活区间、10 个 GPR（`s0`、`s2..s10`，5 对 V128）与 spill home |
| `emit.h` | 一个 Writer 的内部状态、冷访存桩队列、typed Handle 声明和零拷贝操作数读取工具 |
| `emit.cpp` | 一次 schedule 遍历、薄 opcode 派发、入口／出口、短跳回填（`PatchJumps`）、冷桩刷出与局部控制 |
| `scalar_ops.inc`、`vector_ops.inc` | 按职责生成 Handle 声明；IR opcode／schema 仍仅定义于 `ir/opcodes.inc` |
| `emit_operand.cpp` | 窄位宽零扩展 SSA 不变量、零拷贝 `ReadRef`、立即数／移位寻址表达式与 `Zba`/`Zbb` 整数转换 |
| `emit_scalar.cpp` | 标量算术、立即数折叠、移位／位域、`Zbb`/`Zbs` 快路径、无分支 SWAR 计数与 `Zbc` `CLMUL` CRC32C |
| `emit_flags.cpp` | `s11` 按需局部标志求值、算术 producer 快照、`Zbs`/`Zbb` 条件及显式 flags 操作 |
| `emit_memory.cpp` | Uniform、`[base+simm12]` 访存、`ual` 直通／`!ual` 函数尾冷桩非对齐回退、单边／`Ztso` 屏障和条件写入 |
| `emit_atomic.cpp` | AMO／LR-SC、子字原子、CAS128 资格入口与 ABI 调用 |
| `atomic_helpers.h/.cpp` | libatomic CAS128 kernel；不含 IR、前端或缓存策略 |
| `emit_control.cpp` | `Zicond` 无分支标量／V128 select、PC、marker 及明确不接收的旧形态 |
| `emit_calls.cpp` | CPUID／系统查询、SSE4.2、x87、宽除法和有限 scalar Lambda ABI |
| `call_helpers.h/.cpp` | 无 IR 依赖的宽除法结果／错误返回及 x87 metadata 提交 |
| `emit_vector.cpp` | GPR-pair 整数 64-bit SWAR 向量（含 `PMOVMSKB`）、`Zbb` 向量快路径、排列、饱和、扩展和 Crypto 服务 |
| `emit_float.cpp` | F/D 运算、转换、比较、rounding、`FCLASS` 非规格化快检、非 NaN 旁路、`Zicond` min/max 与软件 DAZ/FTZ |
| `patch.h/.cpp` | 真实 LinkSite 初始化和 DATA target 的原子发布／撤销 |
| `abi_layout.inc` | C++／静态汇编共享的 NativeRunContext 偏移 |

`externals/biscuit` 沿用旧版固定来源，只增加 CompileResult word-vector sink；字流直接写入输出，不另建 code buffer。未启用压缩优化、literal pool 或自动 scratch 选择。s1 是唯一 state 基址，s11 保存 portable flags，`s0` 与 `s2..s10` 分配 SSA（共 10 个 GPR / 5 对 V128）；a/t 寄存器由各 handler 显式使用。

## 编译和执行合同

- Block／Function 使用同一类 home，但分别分配。Function 保留真实内部 CFG；额外完整入口有独立 prologue，内部边直接进入 body。
- 常规 flags metadata 只更新 live s11，不分配逐 producer 栈快照。只有实际 SaveFlags／BranchOnlyFlags 消费或 CPUID／宽除法第二结果才申请辅助槽。
- Goto／NotGoto 的数值结果在两条路径都可用，spill 在分支前发布；BindLabel 不继承只在分支体中成立的 PC 常量。
- 静态出口使用 `{target,cold_fallback}` 两个 DATA 槽，release 更新／撤销；热边不进入 C++。动态边查唯一 32B PC 目录。当前没有每站点动态 local cache。
- 根桥保存 LP64D callee 状态和完整 FCSR，冷边界发布 flags／PC 并恢复 host FCSR；guest 四种舍入映射为 RNE/RDN/RUP/RTZ。数值 helper 在 host FP 环境调用，返回后恢复 guest 环境。
- 故障恢复只匹配实际 native FaultSite；普通访存在 `ual` 下直接发射硬件读写，非 `ual` 下采用 3 指令内联对齐快路径加函数尾冷桩逐字节回退。原子要求自然对齐，未满足时在访问前明确停止，不以锁加 memcpy 模拟与硬件原子互斥。
- CAS128 的 libatomic fallback 在持锁前由 VM 检查完整 RW Anonymous 范围，并解除实际 SMC 写监视；其它 backing 明确不接受。失配也执行 old→old 写周期。该资格检查不回收当前 unit，撤链后在冷 dispatcher 安全退休。
- NativeRunContext 利用原 offset 152 的空余空间保存这一窄 atomic callback；state 仍在 offset 160，完整对象仍为 1024B。GuestState 私有性来自 VM 映射合同，与物理 pin 无关；RV 当前不 pin guest GPR/XMM。

当前 210 个中立 opcode 中有 193 个 lowering 入口，其中 85 个为向量／FP。这是 IR 入口清点，不是完整 x86 ISA 或所有 type／operand 组合覆盖。旧 local／Phi／host-register／RSB、MemoryCopy 和 LoadMemoryExtend 形态明确拒绝；CallLocation／CallDynamic 的 Params 也未接回。当前 x86 前端不依赖这些入口：guest CALL/RET 仍由栈访存和 dispatch 执行，AVX/AVX2 仍分解为 V128 IR。`Zba`、`Zbb`、`Zbs`、`Zicond`、`Zbc`/`Zbkc`、`ual` 与 `Ztso` 已通过 `host_features.h` 动态接入；RVV、向量 Crypto 与 Zacas 仍在后续范围。2026-11-07 已通过当前 native-unit 格式恢复磁盘缓存与 AOT，包含跨进程重定位、SMC 失效和命名调用；见 [AOT／缓存合同](../../../../docs/aot-code-cache-2026-11-07.md)，未接回旧持久化实现。

## 构建与验证

在有 RV64 Linux GCC 13 和 sysroot 的 Linux 主机执行：

```sh
scripts/build-riscv64-linux.sh /path/to/rv-build \
  -DSVM_RISCV_QEMU_EXECUTABLE=/path/to/qemu-riscv64
ctest --test-dir /path/to/rv-build -R riscv64_compile_contract --output-on-failure
```

默认编译目标为 RV64G／LP64D；target libc 可能使用 C 扩展，模拟器不强制关闭 C。可显式传 `CMAKE_CROSSCOMPILING_EMULATOR` 或配置 QEMU/sysroot/CPU。

运行时 safe-access 需要真实 `process_vm_readv/writev`。当前测试所用 user-QEMU 10.0.13 缺少这两个 syscall，Mixed／InterpretOnly 在完整 RV64 Linux 内核下验收；生产代码没有为仿真器改成无保护 memcpy。RW/RX 指令缓存复用也要求正确实现的 `riscv_flush_icache(flags=0)`，见 [模拟器说明](../../../../tools/qemu/README.md)。QEMU 用于功能验证，不代表物理 RV64 性能或多 hart 硬件证明。

最终编译／运行结果及 ARM64 compile 回归对照见 [恢复记录](../../../../docs/evidence/riscv64-restore-20261107/README.md)。
