# RV64 backend

这个后端参考 ARM64 的 `JitContext` / `JitTranslator` / `TranslateIR` / trampoline 分层，
使用 Biscuit 发出 RV64G 指令。Linux RV64 构建中的 x86-64 和 ARM64 前端自动选择它。

## 执行协议

每个编译块使用标准 C ABI：`HaltReason block(State*)`。

- `a0` 传入 State，返回停止原因。
- `s0` 保存块的恢复帧，`s1` 保存 State，`s2` 保存 SSA 值区；入口保存、出口恢复。
- `s3`–`s11` 缓存直接发码的标量结果，避免每次使用都读取栈槽。
- 每个 SSA 值预留 16 字节的标准栈槽，低半部保存窄整数的零扩展结果；V128 使用完整槽。
- helper 与直接发码共享这些槽。调用 helper 前，所有值和 guest uniforms 已有有效的内存副本。
- 栈按 16 字节对齐；208 字节恢复帧保存全部整数和浮点 callee-saved 寄存器。
  helper 故障可能跳过 C++ 函数的恢复代码，因此不能只保存后端自己使用的寄存器。
  `gp`、`tp` 不参与分配。
- State 的虚拟 flags 使用现有 ARM64/interpreter 的 NZCV、parity-byte、AF 布局。
  IR 的 `FlagsBit` 是请求掩码，不能用作这个状态字的移位位号。

[RISC-V psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/) 定义了寄存器与栈约定。

## 发码与 Runtime

直接发码涵盖整数常量、uniform 访问、加减和进位、乘除及乘积高半部、逻辑运算、
扩展、移位和旋转、零/位检测、条件取值、select、标量访存，以及全部 terminal 类型。
不依赖 Zba/Zbb、压缩指令或 RVV。窄结果总是截断到 IR 类型宽度。
IR 的除零结果是零，因此不能直接采用 RISC-V DIV 的全一结果。

条件跳转只使用就近的反条件分支，远目标通过 AUIPC/ADDI/JALR 到达。
可编码的有符号 12 位偏移直接用于 load/store，超出范围时单独形成地址；汇编缓冲区按需增长。
条件判断只提取所需的 NZCV 位，AL/NV 直接生成真值；8 位结果使用 ANDI 清高位。
标量 guest 访存先验证 mask、范围、访问末端和映射 oracle；对齐地址使用单条原生 load/store，
保持对齐访问原子性，未对齐地址使用字节访问。未对齐普通访问不提供跨 cache line 的原子保证。
TSO 访问使用保守的 FENCE。其余指令调用现有 IR interpreter 的逐指令语义实现，
包括 flags、V128/浮点、原子和专用 frontend helper；这不是 ARM64 发码回退。

标量结果直接写入缓存寄存器，轮转淘汰时写回标准槽。目标寄存器可能仍保存当前输入，
所以淘汰和写回发生在读取操作数之前。读取不会改变缓存映射，select 和嵌套 terminal
的各个内部路径共享同一映射；对齐/未对齐加载在汇合后发布同一个结果寄存器。
语义 helper、Goto/NotGoto 和 BindLabel 前写回并清空缓存，保证 helper 的隐式操作数、
成对返回值和跳转到达路径都使用有效槽位。仅验证地址的 MemoryAddress 调用依赖 C ABI
保持缓存寄存器；它不访问 SSA 值区。块返回时直接丢弃私有缓存，独立块入口重新开始分配。

Runtime trampoline 循环调用块，通过现有 L2 表分派，使用 acquire fence 读取发布的表项。
无键返回 CodeMiss，空目标返回 CacheMiss，非零停止原因返回 host 并清除 State 的 halt_reason。
块入口、块间 dispatcher 和局部 label 会检查 Signal/SMC 请求。RV64 不使用 ARM64 的
寄存器/guard-page 中断协议。

内存故障从生成代码或语义 helper 进入块 epilogue，然后正常退出 dispatcher。
恢复地址与块帧使用 State spill_area 的最后两个槽；RV64 不使用 ARM64 的 spill allocator。
未对齐原子 helper 的故障恢复还会释放当前线程持有的 fallback 锁。host-call 参数保存在栈上，
避免跳过 C++ 析构时遗留参数 vector 的堆分配。
C++ helper 抛出的异常在 helper 边界捕获并转为 `IllegalCode`，不会穿过没有 unwind 信息的 JIT 帧。
canonical IR 的强引用按代码 allocation 保存在 Module 中，节点 detach 后仍保留，
直到 QSBR 允许 `ReclaimCode`，避免 helper 引用已释放的指令。
函数中的多个块共享一段 allocation；每个独立函数拥有独立 allocation。

代码缓存通过现有 RW/RX 映射发布并清理 I-cache。
[Linux RISC-V instruction synchronization](https://docs.kernel.org/6.15/arch/riscv/cmodx.html)
说明了线程迁移与 I-cache 同步要求；真机仍需要验证并发、SMC 和内存顺序。

## 构建与测试

Ubuntu 上安装 `g++-riscv64-linux-gnu`，并按 [测试模拟器说明](../../../../tools/qemu/README.md)
构建支持双映射缓存刷新的 QEMU 后（示例假定补丁版位于 `/tmp/swiftvm-qemu-riscv-8.0.4`）：

```sh
sh scripts/build-riscv64-linux.sh /tmp/swiftvm-rv64 \
  -DSVM_RISCV_QEMU_EXECUTABLE=/tmp/swiftvm-qemu-riscv-8.0.4/build-swiftvm/qemu-riscv64
ctest --test-dir /tmp/swiftvm-rv64 -R '^swift_riscv_backend' --no-tests=error --output-on-failure
```

工具链文件是 `source/cmake/riscv64-linux-gnu.cmake`。
QEMU 动态加载器的 sysroot 默认 `/usr/riscv64-linux-gnu`，可通过 `SVM_RISCV_SYSROOT` 修改。
CTest 启用 RV64 后端执行测试；macOS 构建可以编译同一目标，但不会把它当成本机可执行测试注册。
独立的 Biscuit 发码/跨宿主执行测试入口仍见 [测试说明](../../../tests/riscv/README.md)。
原版 QEMU 8.0.4 的缓存复用负对照必须失败，补丁版必须通过；SwiftVM 的 RW/RX 协议保持原样。

## 当前边界

这是可执行的基线后端，已有九个 GPR 的块内标量缓存，尚未实现活跃区间驱动的寄存器分配、
死值免写回、RVV 直接发码、direct linking、RSB 优化或
跨块 SSA/phi。函数内出现跨块 SSA 时编译明确拒绝，frontend 可重新解码为独立块。
ARM64 host-register rewritten IR 和未拆分的 V256 也明确拒绝。
函数编译保留 canonical uniform 访问和 flag producer，避免把 ARM64 特定优化契约带入 RV64。

语义 helper 内嵌本进程的函数地址和 IR 指针，因此 RV64 代码暂不写入磁盘 JIT cache，
也不支持 AOT 序列化，AOT 编译入口会明确拒绝 RV64。浮点和 SIMD 通过现有 interpreter
语义 helper 执行；完整 guest 舍入模式、FP 异常状态与所有边界指令仍需另行资格验证。
QEMU 的结果用于功能验证，不代表真机性能、原子顺序或多 hart SMC 保证。
