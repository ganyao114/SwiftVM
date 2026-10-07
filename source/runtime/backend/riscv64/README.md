# RV64 backend

这个后端参考 ARM64 的 `JitContext` / `JitTranslator` / `TranslateIR` / trampoline 分层，
使用 Biscuit 发出 RV64G 指令。Linux RV64 构建中的 x86-64 和 ARM64 前端自动选择它。

## 执行协议

每个编译块使用标准 C ABI：`HaltReason block(State*)`。

- `a0` 传入 State，返回停止原因。
- `s0` 保存块的恢复帧，`s1` 保存 State，`s2` 保存 SSA 值区；入口保存、出口恢复。
- 不使用 flags 的块以 `s3`–`s11` 缓存 SSA；使用 flags 的块以 `s3`–`s10` 缓存 SSA，
  `s11` 保存 guest flags。RV64G 的 V128 值使用两个 GPR；RVV 值缓存于 `v8`–`v23`。
- CFG 活跃性分析按最后一次使用释放死亡值；局部跳转只写回目标入口仍需要的 SSA。
  条件跳转的落空路径保留现有缓存，不重复 spill/reload。
- Phi 在进入目标 CFG 节点的边上并行赋值，优先常驻 GPR/RVV；复制环用两个临时
  GPR 打破，不分配临时栈。寄存器不足的 Phi 才使用标准槽。
- `Config::buffers_static_alloc` 为 `Get/SetHostGPR/FPR` 提供逻辑寄存器与 uniform 范围
  的绑定。后端按访问频率选择常驻寄存器，逻辑编号不直接对应 RV64 物理编号。
  部分写入保留未触及字节，读取保持捕获语义；回调前发布、返回后重新载入。
- 支配分析仅初始化可能绕过定义而被读取的槽位；直线运算链不清空全部 SSA 栈。
- 每个 SSA 值预留 16 字节的标准栈槽，低半部保存窄整数的零扩展结果；V128 使用完整槽。
- V256 在发码前拆成两个 V128 SSA，保持四个 GPR 或两个 RVV 寄存器常驻。
  整数运算链复用死亡输入的寄存器对，避免因拆分增加 SSA spill；完整宽度的
  zip/unzip、pack、字节位移、抽取、movmask 和浮点转换显式处理半部之间的数据流。
- helper 与直接发码共享这些槽。调用 helper 前，所有值和 guest uniforms 已有有效的内存副本。
- 栈按 16 字节对齐。叶块使用 112 字节恢复帧，只保存实际使用的整数寄存器，
  不保存浮点 callee-saved 寄存器；标量浮点只使用 caller-saved 临时寄存器。
  存在 ABI 调用的块使用 208 字节帧。显式 host call 等频繁调用在入口保存完整 ABI 集合；
  只有 oracle、未对齐原子或宽除法 slow path 的块在入口仅保存实际修改的 GPR，
  首次执行 ABI 调用时再通过共享冷路径保存其余 GPR 和全部浮点 callee-saved 寄存器。
  正常返回保留精简出口，helper 故障通过专用恢复入口恢复完整集合，
  因为故障可能跳过 C++ 函数的恢复代码。
  `gp`、`tp` 不参与分配。
- State 的虚拟 flags 使用现有 ARM64/interpreter 的 NZCV、parity-byte、AF 布局。
  IR 的 `FlagsBit` 是请求掩码，不能用作这个状态字的移位位号。

[RISC-V psABI](https://riscv-non-isa.github.io/riscv-elf-psabi-doc/) 定义了寄存器与栈约定。

## 发码与 Runtime

直接发码涵盖整数常量、uniform 访问、加减和进位、乘除及乘积高半部、逻辑运算、
扩展、移位和旋转、bitfield、byte swap、popcount、CLZ/CTZ、CRC32C、全部 flags IR、
条件取值、select、标量/V128 访存，以及全部 terminal 类型。
基线要求 RV64G；Zbb 的计数/byte swap/rotate/ANDN 和 RVV 整数向量使用检测到的扩展。
普通标量结果截断到 IR 类型宽度；BitCast/GetResult 按语义保留完整原始槽。
IR 的除零结果是零，因此不能直接采用 RISC-V DIV 的全一结果。

条件跳转只使用就近的反条件分支，远目标通过 AUIPC/ADDI/JALR 到达。
可编码的有符号 12 位偏移直接用于 load/store，超出范围时单独形成地址；汇编缓冲区按需增长。
条件判断只提取所需的 NZCV 位，AL/NV 直接生成真值；8 位结果使用 ANDI 清高位。
标量 guest 访存先验证 mask、范围、访问末端和映射 oracle；对齐地址使用单条原生 load/store，
保持对齐访问原子性，未对齐地址使用字节访问。未对齐普通访问不提供跨 cache line 的原子保证。
TSO 访问使用保守的 FENCE。地址 mask、limit、末端溢出检查直接发码，只有存在映射
oracle 时才调用回调边界。对齐的 32/64 位交换、加减、AND/OR/XOR 使用 AMO；
CAS/NEG 和 8/16 位更新使用 LR/SC，保持目标 word 的相邻位。未对齐更新使用专用
加锁 kernel，保持既有 fallback 协议，并由故障恢复释放本线程的锁。
普通访存、原生 AMO/LR-SC/Zacas 与软件原子 kernel 使用共同的读者/写者协议。
Runtime 在私有 cache line 上注册读者；生成块在入口获得访问租约，出口和 ABI 调用前释放，
内部回边遇到待处理写者时让出租约。软件 CAS128/未对齐更新关闭入口并等待现有读者退出，
再执行其完整更新。正常路径每块发布一次读者状态，不为每次 guest 访问增加全局原子 RMW。
没有 Runtime 注册节点的直接 State 调用，以及 interpreter/libc 复制 kernel 使用匿名读者计数。
RV64 的 x87 内存 helper 同样获取读者，包括完整的 80 位访问、环境保存/恢复和
FXSAVE/FXRSTOR；只操作 x87 寄存器的指令不增加读者同步，其他后端保持原有 helper 路径。
故障恢复清理当前 State 的租约、kernel 计数和软件原子锁。
入口的 store/load 全栅栏与写者的全栅栏关闭注册/扫描竞争，入场成功后的 R/RW 栅栏
获取写者退出前的更新；出口的 RW/W 栅栏使 guest
访问先于读者退出发布，依据 [RVWMO 的 FENCE 顺序规则](https://docs.riscv.org/reference/isa/v20260120/unpriv/mm-eplan.html)。
这一协议使软件原子更新与参与协议的访问互斥；普通宽向量访存本身仍由多条指令实现，
不承诺与并发原生 Zacas 之间的整个向量读取原子性。直接操作 guest 指针的外部 host callback
须自行参加该协议；ABI 调用期间生成块不持有租约。

V128 的数据访问、选择、原始 bitcast、位运算、整数加减、平均、移位、乘法、比较、
min/max、饱和运算和窄化使用原生发码。无 V 时以 GPR 对及 SWAR 运算执行；有 V 时
使用向量寄存器缓存，连续运算避免 SSA 栈访问。RVV 的 VL 仅覆盖 128 位，spill 使用
VL=2 的 VSE64，不能使用会随 VLEN 扩大的 whole-register store。
canonical uniform 的非 lane 对齐字段通过字节 slide 和掩码合并访问；slide-down 的源索引
以 VLMAX 为界，VL 限制目标写入，尾部保持策略保留窄结果的零高位，遵循
[RVV slide 规范](https://docs.riscv.org/reference/isa/v20260120/unpriv/v-st-ext.html)。
RVV 固定点舍入只在所需模式变化或 ABI 调用后重设；所有线程必须允许已发布代码使用的 V。
向量重排、横向运算、局部变量及全部浮点 IR 均直接发码。浮点结果显式修复 x86 NaN、
有符号零、无序比较及转换溢出语义；RVV 运算使用 RNE，ABI 回调与出口恢复调用者 FRM。
AES 使用 Zkne/Zknd 或 Zvkned，SHA 使用 Zknh/Zvknha/Zvknhb，PCLMUL 使用 Zbc/Zbkc/Zvbc；
没有这些扩展时直接生成 GPR/RVV 指令，AES 轮使用预计算 T-table。
短 memmove 内联，较大复制采用 LMUL=8 的可伸缩 RVV 分块或直接 libc memmove，
保留双向重叠语义并验证完整地址范围。CAS128 在 Zacas 上使用 AMOCAS.Q；其余平台
通过固定加锁 kernel 执行。host call、CPUID、x87、SSE 字符串和宽除法使用专用 ABI
边界，无 IR 分派；64 位可表达的 Div128 直接使用 DIV/REM。
完整清单见 [覆盖报告](../../../../docs/riscv64-ir-coverage.md)；
interpreter 路径不计为原生性能覆盖。

结果直接写入缓存寄存器，优先使用空闲寄存器，活跃值淘汰时写回标准槽。目标寄存器可能仍保存当前输入，
所以淘汰和写回发生在读取操作数之前。读取不会改变缓存映射，select 和嵌套 terminal
的各个内部路径共享同一映射；对齐/未对齐加载在汇合后发布同一个结果寄存器。
语义 helper 前发布所有必要标准槽；跳转和 label 汇合只写回目标活跃值，Phi 直接进行边复制。
这保证 helper 的隐式操作数、成对返回值和跳转到达路径都使用有效数据。
flags 与常驻 uniforms 在 helper、跳转和潜在故障前发布，
ABI 调用后重新读取；故障放弃 C++ 帧时从 State 恢复 `s11`。oracle 和原子 slow kernel
调用前保存活跃 RVV 值，调用后原位恢复并重设 vtype，普通内存热路径不为此写回。
块返回时直接丢弃私有 SSA 缓存，独立块入口重新开始分配。

具有跨 block SSA 的 HIR 函数在一份共享 SSA 帧内编译，内部 terminal 直接连接原生 CFG，
Phi 按 HIR 前驱顺序重排到实际入边，并使用同一并行复制机制。原始 IR 保持不变；
Module 将展开后的 IR 保留到代码 allocation 的 QSBR 回收。只有函数根入口发布为 C ABI 入口，
依赖前驱 SSA 的内部块不能单独进入；frontend 可以从相应 guest 地址重新解码独立块。
内部 guest 边在目标中断检查前提交 current_loc/prev_loc，条件展开的合成标签不作为中断边界。
32 字节访存先验证完整范围，然后访问两个半部，只调用一次映射 oracle。
TSO 屏障只置于完整访问的两端，拆分后的半部之间不重复设置。

Runtime trampoline 循环调用块，通过现有 L2 表分派，使用 acquire fence 读取发布的表项。
无键返回 CodeMiss，空目标返回 CacheMiss，非零停止原因返回 host 并清除 State 的 halt_reason。
块入口、块间 dispatcher 和 guest CFG label 会检查 Signal/SMC 请求。
单条 guest 指令内部的局部标签不作为退出点，避免 SMC 重放部分指令。
RV64 不使用 ARM64 的
寄存器/guard-page 中断协议。

内存故障从生成代码或语义 helper 进入块 epilogue，然后正常退出 dispatcher。
恢复地址与块帧使用 State spill_area 的最后两个槽，读者租约使用前两个预留槽；
RV64 不使用 ARM64 的 spill allocator，也不改变 State 的布局。
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

目前 `ir.inc` 的 191 条 opcode 均有原生 lowering：188 条数据/状态 IR 与 3 条局部控制流 IR，
`--require-native` 覆盖门禁通过。已有 CFG 活跃性、局部及同一函数跨块 Phi、V256 lowering、
GPR/RVV 缓存和逻辑宿主寄存器绑定。
Phi 参数按入边源指令顺序排列，同一源的落空边在跳转边之前；宿主入口边排在最前且要求立即数。
Phi 必须位于 CFG 入口的连续 Phi 序列中，值输入须支配相应入边，宽度须与结果一致。
Host-register IR 缺少兼容 uniform 绑定时明确拒绝。
portable interpreter 的向量槽位仍限定为 V128，遇到 V256 会返回 IllegalCode；
V256 使用 RV64 原生 legalization 执行。
跨块 SSA 必须属于同一 HIR 函数，并支配其使用点或 Phi 入边；非法标量 opcode 的 V256
结果明确拒绝。展开后的指令数不得超出 IR 的 16 位 ID 范围。
allocation 之间已支持 direct linking：近目标使用单条 `JAL x0`，超出 ±1 MiB
时使用本 allocation 的固定 L2 value 槽叶子。冷链接经共享 LinkManager 注册，
SMC 在回收目标前原子恢复原始 JAL；热路径不调用 C++ 链接器、不重新哈希目标。
RSB 使用带边界的返回栈，保存 guest 返回 PC 与稳定的 L2 value-word 索引。
命中后恢复当前块的 LP64D 帧并尾跳到返回入口；空栈、预测不匹配和空槽退回普通分派。
函数编译保留 canonical uniform 访问和 flag producer，避免把 ARM64 特定优化契约带入 RV64。

RV64 已接入 `SVM_JIT_CACHE=<目录>` 和 `svm_aot compile/run`。
Linux launcher 的磁盘缓存使用有界 bias window，需要同时设置 `SVM_MEM_DIRECT=0`；
目录须预先创建。默认 direct memory 模式沿用现有缓存限制，会明确禁用磁盘缓存。
固定宿主地址通过 `AUIPC/LD` 读取标记过的本地 literal pool，加载时校验并重定位全部
host-image 指针。磁盘缓存和 AOT 都保留原始冷 JAL，加载后重新注册链接元数据，
并绑定当前进程的目标。
共享 SSA 函数仅发布根 ABI 入口，内部块和解码依赖的 guest 字节参与哈希与 SMC。
包含进程私有 IR/堆指针或宿主镜像外指针的单元会被拒绝持久化，并继续走 JIT。
AOT 产物使用 `EM_RISCV`、LP64D `e_flags=4`；宿主 ISA 和探测到的扩展能力进入有效性校验。
产物格式为 AOT 4 / cache 24，旧产物需要重建。
完整 guest 舍入模式、FP 异常状态与所有边界指令仍需另行资格验证。
QEMU 的结果用于功能验证，不代表真机性能、原子顺序或多 hart SMC 保证。
