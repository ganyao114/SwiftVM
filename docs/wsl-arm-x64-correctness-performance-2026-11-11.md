# WSL ARM guest → x64：正确性与性能记录（2026-11-11）

基线为 `dev` 的 `47d6fa47`。本轮使用 WSL2 / Ubuntu 24.04、x86-64、GCC 13.3、CMake 3.28.3、Release/IPO；普通构建没有开启 sanitizer。本记录覆盖已完成的本轮实现、验证和明确的边界，以下结果只确认列出的配置与输入，不宣称完整 ISA 覆盖。

## 已实现的修复

1. `VM::UpdateFPEnvironment` 只在 ARM64 guest **且宿主后端为 ARM64** 时使用原始 FPCR。此前所有宿主都把已转换的浮点控制覆盖为 ARM FPCR；x64 的 `ldmxcsr` 因保留位直接使宿主进程 SIGSEGV。160 个 ARM64 基础差分用例中，原版为 10 verified / 150 host_crash；修复后为 160 verified。这里没有把崩溃改成解释回退。
2. x64 的 pin 能力从残留的 4-slot 改为实际支持的 2-slot，默认仍为 0。大于能力上限的请求由原有配置路径夹到 2；诊断统计改为记录 `layout.pinned_uniform_slots`，避免把后端容量写成实际使用量。
3. Direct 正确性执行器对非法指令、断点的地址有效性和 `unknown` access 使用与生产 VM 相同的异常合同；divide/FP 异常也使用明确的 `unknown`。没有修改比较器或放宽比较掩码。增加 x86 UD2/INT3、A64 invalid/BRK、A32 UDF、Thumb UDF/BKPT 共 7 个协议回归，并与真实 VM 的观察比较。

## 局部性能改动

`x64::Writer::PackFlags` 将相邻的 ZF/SF 一起搬运，用 32 位运算生成全部可移植算术标志和 parity 源字节。保留现有 producer snapshot、carry 极性和 reserved bits 协议；固定发射序列从 27 条缩到 18 条。

没有新增 IR pass、节点分析表、缓存或后端表示。ARM 前端的 FP 数值语义与 helper 路径保持原合同。`x64_compile_test` 增加四种位宽、ADD/SUB/ADC/SBB、两种 carry profile、边界操作数与输入 carry 的 5,184 组原生检查，独立计算预期 NZCV/PF/AF 与保留位。

## 验证范围

- ARM64 FP 回归：16 种 RMode/FZ/DN 组合 × Block/Function × JIT/Interpreter/Mixed × pin 0/2，共 192 次。检查舍入中点、IXC、目的向量高位清零、FPCR 保留及实际原生执行。
- ARM32 pin 回归：请求 0/2/4 × Block/Function，经过实际 ARM/Thumb interworking，检查退出值、有效 pin 数和零解释步数。
- 基础差分矩阵：ARM64 160 例、ARM32 192 例，分别执行 Block/Function × Known/Unknown/EarlyOff × opt on/off。**24 个配置、4,224 次比较全部 agreement，4,224 次均有 native_executed，零失败。** Oracle 为仓库固定版本的 Unicorn 适配器，先通过其 self-test。矩阵使用已提供的 batch worker；初始基线另用每例独立进程的 controller 执行。
- 第一批构建的 CTest：30 passed / 7 skipped / 1 failed；skipped 为本宿主不能执行的其他架构 native 目标。随后修复并复测了 `aot_call_native` 的同步 guest fault 复入语义，完整 CTest 为 38 项中 31 passed / 7 skipped / 0 failed。
- C++ 改动用 clang-format 21.1.8 格式化；`git diff --check` 和 Python 语法检查通过。

ARM32 canonical inventory 的 824 例另行完整执行，结果未改写为全绿：

| 分类 | 数量 |
| --- | ---: |
| verified | 595 |
| verified_with_gap | 15 |
| oracle_unsupported | 140 |
| oracle_cpsr_gap | 1 |
| profile_gap | 1 |
| exception_mismatch | 72 |

72 例与 Unicorn 的异常差异均保留。Direct 和 VM 对这些输入都返回 fault；修复上述观察元数据后，两者 **72/72 agreement**。这只能说明该组差异并非 x64 独有的算术执行偏差，不能证明 Unicorn 或 DUT 哪一方符合 ISA。输入包含同名寄存器、写回与目标重叠、版本门控以及 fault-width 等情况，仍需合法操作数组合和独立架构参考继续裁决。

## 性能与 compile 成本

`tools/bench_arm_x64.py` 用 Clang/LLD 构建无需 guest libc 的静态 A64/A32 程序。整数、访存各 3,000,000 次，FP 100,000 次；每次对完整 32/64 位输出或 double 位模式检查独立预期，要求所请求 scope 成功原生执行、无 fallback、无解释步数。固定 WSL CPU2，基线/候选交错，普通 wall time 与 `--decode-timing` 分开测量，各 31 个新进程样本，报告中位数。

为隔离优化，性能基线已包含 FPCR 正确性修复，尚未包含标志位优化；两边都显式 `--pin-slots 0`。该基线诊断原本仍把容量 4 误报成实际 pin 数，真实配置与编译参数为 0；候选已修正报告。每份结果绑定 runner 和 guest ELF 的 SHA256。

| Guest / workload | Scope | 普通 wall time 变化 | compile_ns 变化 | native bytes 前→后 |
| --- | --- | ---: | ---: | ---: |
| A64 integer | Block | -7.69% | -3.17% | 1272→1224 |
| A64 integer | Function | -6.66% | -2.56% | 732→716 |
| A64 memory | Block | -12.23% | -3.10% | 1320→1264 |
| A64 memory | Function | -8.00% | -2.54% | 748→732 |
| A64 FP | Block | -1.68% | -4.45% | 1508→1452 |
| A64 FP | Function | -1.56% | +0.14% | 896→864 |
| A32 integer | Block | -7.99% | -14.96% | 1492→1436 |
| A32 integer | Function | -5.91% | -14.91% | 924→892 |
| A32 memory | Block | -6.21% | -18.25% | 1700→1644 |
| A32 memory | Function | -6.85% | -17.21% | 1004→972 |
| A32 FP | Block | -1.49% | -17.14% | 1928→1872 |
| A32 FP | Function | -1.82% | -15.93% | 1248→1216 |

首轮 11 样本的整数/访存收益方向相同，compile 中位数变化范围约 -2.6% 至 +4.2%。这两轮均未观察到明显 compile 退化；不把亚毫秒级 compile 的波动全部归因于编码优化。FP 主体仍是数值 helper，本轮的约 1%–2% wall time 变化不代表 FP 算术吞吐已有显著提升。这些是固定微基准，不是完整应用或 PMU 结论。

## 复现和证据位置

仓库位于 Windows `D:\Dev\SwiftVM`，WSL 对应 `/mnt/d/Dev/SwiftVM`。

```sh
cmake -S . -B build/host -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_STANDARD_LIBRARIES=-latomic
cmake --build build/host -j12
SVM_TEST_SCRATCH=/tmp ctest --test-dir build/host --output-on-failure

python3 tools/bench_arm_x64.py \
  --baseline /path/to/fixed-baseline/svm_rebuild_linux \
  --candidate build/host/svm_rebuild_linux \
  --out build/wsl-arm-x64/reproduce --cpu 2 --samples 31
```

本机完整构建需要显式链接 libatomic：否则 LoongArch64 portable test 的 `__atomic_compare_exchange_16` 未解析。该参数只在本地 CMake cache，未改仓库构建逻辑。Windows 挂载目录不能满足缓存目录 `0700` 检查，因此测试临时文件使用 Linux `/tmp`；未放宽生产缓存权限要求。

本机证据在 `build/wsl-arm-x64/`：

- `matrix-summary.json` / `matrix.log`：4,224 次矩阵汇总；详细 observations 位于下述 WSL cache。
- `bench-final/summary.json`：31 样本原始观测、时间中位数、输入与二进制 hash；`bench-flags/summary.json` 为前一轮 11 样本。
- `final-ctest.xml`：完整 CTest 结果；`inventory-summary.json` / `inventory-review.json`：824 例分类及 72 例复核。
- `baseline/`、`fixed/`：修复前及仅修复 FPCR 后的二进制。

详细 corpus、每例观察及原始独立进程结果保留在 `/home/proton/.cache/SwiftVM/wsl-arm-x64-20261111/`。`baseline-arm64`、`baseline-arm32`、`fixed-arm64`、`fixed-arm32-inventory` 为 controller 结果；`candidate-matrix` 为 24 个配置的实际观察；`inventory-review` 保留修复前后 Direct 元数据。

上述为第一批结果。以下记录后续工作，仍不能把有限矩阵当作完整 ISA 覆盖。

## 第二批：hint、浮点舍入与数值 helper

### 正确性修复

- ARMv7 的 A32/Thumb16/Thumb32 SEVL 编码位于 hint 空间，旧版本应保持 NOP 行为。原实现把它们与 v8 的 acquire/release 指令共同门控到 v8，导致 v7 用户程序收到非法指令。将这三项独立门控为 v7+，保留用户态既有 hint sink。新增三种编码 × v7/v8 的状态不变回归；QEMU Cortex-A15 与 Unicorn 均确认 v7 执行到后继 PC。
- ARM 数值 helper 一直用 `softfloat_round_odd` 保存 binary128 中间结果的微小修正，但 SoftFloat target 没有启用 `SOFTFLOAT_ROUND_ODD`，且冻结子集省略了 F128 `packReturn` 标签。现启用该原有选项、恢复上游标签；不增加浮点状态或 ABI。恢复位置另与 Berkeley SoftFloat 上游文件核对，本机留存参考文件 SHA256 `a00544e1a8b5282614b6a9ee92c711489bfa55824bb9509bbaa29dccebeb79e6`。
- 可复现的旧错误：F32 `1.0 + 最小正非正规数`、向正无穷舍入、FZ=0，原版返回 `0x3f800000`，正确结果为 `0x3f800001`，IXC 均应置位。QEMU Cortex-A15 和 Unicorn 对正确值一致。新增 Add/Sub、FMA 的极小修正、精确抵消、正负零、FZ 边界及 FCVTXN jam-to-odd 回归；FCVTXN 验证固定舍入不随 FPCR.RMode 改变。

### 数值优化与复杂度

`EvaluateBinaryFP` 对 F32/F64 的 MUL/DIV/ADD/SUB/负乘改为直接调用已包含的 stateful SoftFloat 同精度操作，避免先扩展到 binary128 再缩回。ARM 的输入 flush、NaN 选择、DN、舍入和 FPSR 累计继续由同一数学叶子拥有。FZ 使用舍入前 tininess，同时处理精确 subnormal 与“微小结果舍入成 MinNorm”的边界，UFC 不额外带入 IXC。F16 保留原来的 binary128 路径。

这是现有纯数值函数中的固定分支，约 54 行；没有增加 IR pass、每节点数据、发码分析、运行时注册表或指令编码 helper。JIT 提交的 IR 与生成代码大小保持相同。A32 与 A64 共用该叶子，因此无需复制前端或后端浮点路径。

### 验证结果

1. **2,051,840 组数学对照：零差异。** 对比新 F32/F64 操作与启用 round-to-odd 后的原 binary128 算法，覆盖 32/64 位、四种基本运算及负乘、16 种控制组合、边界值与固定随机流。比较返回位模式与全部异常位；这是算法对照，独立架构参考见下一项。
2. 新增 `tools/correctness/generate_arm_fp.py`，生成 **9,216 个合法单指令用例**：A64、A32、Thumb VFP，32/64 位，ADD/SUB/MUL/DIV/负乘/FMA，16 种 RMode/FZ/DN 组合及随机位模式。输入保留非零目的向量高位与已有 QC/UFC，验证独立保留规则；UFC 初始为零的精确异常边界由永久数值回归和上一项全异常位对照覆盖。与固定 Unicorn 参考比较，Block+opt、Function+opt、Block+no-opt 共 **27,648/27,648 agreement**，全部有 `native_executed`，零解释执行、零失败。没有修改比较规则、掩码或失败分类。
3. 原有 24 配置基础矩阵在第二批生产改动后重跑，**4,224/4,224 agreement**，全部实际原生执行。
4. 把 SHA-256、插入排序、整数混合、非内联调用、浮点矩阵计算编译成无 libc 的 AArch64、A32、Thumb 静态 ELF。三种 ISA × Block/Function × JIT/Mixed/Interpreter 的 **18 次完整程序执行**均与独立 QEMU 输出的完整 56 字节一致；SHA-256 再与 Python hashlib 单独核对。JIT 无解释步数、无 Function fallback。它覆盖编译器生成的控制流、实际栈/访存与向量代码，补充单指令测试。
5. 三个综合 ELF 的磁盘缓存热启动分别命中 12/14/14 个 unit，均 `compile_attempts=0`；有界 AOT 分别装载 6 个 unit，其余目标由普通 JIT 补齐，没有伪称全量离线编译。移走原 ELF 后三者仍由嵌入 guest 正确执行，测试结束恢复原文件。
6. 第二批开始时整体 CTest 为 **30 passed / 7 skipped / 1 个既有 `aot_call_native` failed**；该失败已在后续边界修复中解决。新增 FCVTXN 所在的 ARM64 frontend target 与 correctness protocol 又单独通过。

### 第二批性能与 compile 门

基线为第一批完整改动后的 runner，候选为本批数值修复和优化后的 runner；使用相同 guest ELF、pin=0、CPU2、交错新进程，普通运行与 intrusive compile 诊断分开，各 31 样本。

| FP workload | Scope | 候选 / 基线普通 wall time | 候选 / 基线 compile_ns |
| --- | --- | ---: | ---: |
| A64 FP | Block | 0.8443× | 0.9470× |
| A64 FP | Function | 0.8385× | 0.9516× |
| A32 FP | Block | 0.8236× | 0.7983× |
| A32 FP | Function | 0.8145× | 0.8306× |

浮点微基准 wall time 下降 **15.6%–18.6%**。整数与访存组候选/基线为 0.990–0.996，基本持平；所有组生成代码大小、compile attempts、spill 数保持不变。本轮 compile 中位数均未增加；冷启动的亚毫秒数值不应直接解读成优化器或 allocator 加速。

另外对综合 C 程序执行 11 样本 compile 诊断：A64 Block/Function 的 compile 中位数分别 2.296→2.244 ms、2.062→1.965 ms；A32 为 2.085→1.970 ms、1.638→1.564 ms；Thumb 为 2.084→1.988 ms、1.782→1.629 ms。该较大输入也未观察到明显 compile 退化。仍不把固定输入或 WSL wall time 当作完整应用性能排名。

### inventory 差异裁决进度

对第一批 72 个失败输入，用任务缓存内的 QEMU 8.2.2 Cortex-A15，通过 GDB remote 注入原始寄存器、CPSR、VFP 状态与内存，再单步观察。原始 canonical 用例全部保留；其中三个 SEVL 问题已修复，重放结果为 **3 agreement / 69 exception_mismatch**。

为避免只检查全零、同名寄存器，把原失败涉及的 SWP、MSR、半字/双字访存、长乘/累加、VFP 双 GPR 搬运、NEON transpose/zip/unzip 改用 Clang 汇编的合法操作数组合，增加 172 个实际用例。其中 **168 与 Unicorn agreement**；其余 4 例涉及双字访存：两例逻辑 8 字节访问与 Unicorn 分拆的 4 字节 fault-width 不同，另外两例未对齐访问 DUT 报 alignment、QEMU 同样停在总线异常，而 Unicorn 完成访问。没有据此放宽比较器。

QEMU 对部分 constrained-unpredictable/保留编码选择继续执行，不足以证明 DUT 的拒绝策略错误。当时尚未裁决的 69 例和 typed-call 故障复入问题，后续处理分别记录于第三、第四批。

### 第二批复现与本机证据

```sh
python3 tools/correctness/generate_arm_fp.py --out /path/on/linux/fp-corpus
# 用已有 correctness controller 或 batch worker 执行生成的完整 Case。
# 生产构建、oracle 版本与资源限制沿用前文及 correctness 文档。
```

`build/wsl-arm-x64/` 下新增：

- `fp-validation-summary.json`、`post-fp-matrix-summary.json`：第二批差分汇总，详细 case/observation 仍在 WSL cache 的 `fp-corpus`、`fp-validation`、`post-fp-matrix`。
- `fp-leaf/round-odd-comparison.log`：2,051,840 组完整异常位与结果比较；旁边保存原算法、当前编译输入和上游参考文件。
- `bench-direct-fp/summary.json`：31 样本与 runner/guest SHA256。
- `compiled-programs/`：综合 C 源码、三种 guest ELF、QEMU 原始参考输出、18 次执行汇总、`compile-benchmark.json`、`persistence.json`。
- `qemu-inventory-review-summary.json`、`inventory-post-fp-summary.json`、`arm32-legal-summary.json`、`arm32-legal-qemu.json`：异常复核与合法操作数测试。详细 QEMU 寄存器、代码和中间 ELF 在 WSL cache 的 `qemu-inventory-review`、`arm32-legal-operands`。
- `fp-ctest.xml`：本批全套 CTest 的真实结果；原始 evidence 与剩余失败保留。

## 第三批：AOT 同步 fault 的可恢复调用边界

`AotGuestEnv::RunToSentinel` 原先把同步 guest fault 交给默认 Linux signal policy；空指针读取因此将 AOT VM 的唯一 guest thread 标记为已退出，`GuestFn::Try` 虽然返回 `GuestFault`，后续合法调用却被 `VM::CallGuest` 拒绝。修复在没有显式 `exception_handler` 时安装一个仅针对同步事件的 AOT 边界处理器：把异常转换为本次调用的 `GuestFault`，由 `CallFrame` 恢复调用前的架构状态，保留 guest 内存与已提交的全局写入；异步信号仍走 guest OS 路径。显式处理器的 `GuestOS`／`Stop` 策略，以及 guest 主动 `exit` 后不可复入，都保留并加入回归测试。

专门的 `aot_call_native` 覆盖 Block/Function 两种 scope、连续两次 fault→成功复入、显式终止策略和主动退出边界。随后在 WSL 重新构建并运行完整测试集：**38 项中 31 passed / 7 skipped / 0 failed**。本轮没有改变 ARM guest 的普通执行、信号传递或 AOT artifact 格式。

对应证据：`build/wsl-arm-x64/final-aot-build.log`、`build/wsl-arm-x64/final-aot-ctest.log`，以及 `source/aot/call.cpp`、`source/tests/rebuild/aot_call_test.cpp` 中对应的实现与回归。

## 第四批：双 word 访问、Unknown target 故障与 NEON UNKNOWN 数据

### 架构合同与修复

- ARM/Thumb 的 LDRD、STRD 在地址双字对齐时保留原来的 64 位访问；在地址仅 word 对齐、最低三位为 `100` 时，按架构规定分两次 32 位访问。第二次访问使用 32 位地址加法，第一 word 的成功提交和最终 base writeback 分属实际顺序。修复前一次 64 位访问会丢失第二 word fault 前的第一 word 提交，且 fault-width 错报为 8。所有 literal、immediate、register、pre/post-index 形式共用 `LoadDual`／`StoreDual`；替换重复的 pack/unpack 代码后，该语义文件净减少约 30 行。使用既有本地 Goto 和 bit-test，无新 IR opcode、pass、每节点分析或运行时注册表。先尝试的“一概拆成两 word”方案因会丢失双字对齐的 64 位访问合同而未保留。
- x64 在 Unknown/EarlyOff target 下只有显式 guest PC store，旧 encoder 只识别 FaultLocation，导致合法指令的坏地址访问使 host SIGSEGV。现从原有的显式常量 PC 识别点和 SetLocation 同步更新现有 fault-location 字段；改动为 9 行，不增加发射指令或额外搜索。旧的第二批 runner 在相同 fault case 下独立进程退出 `-11`；新构建捕获为 guest memory fault。ARM64/x86 guest 的 Unknown/EarlyOff/已知 target、Block/Function、pin=0/2 另有永久回归，ARM32 由下述完整访问矩阵覆盖。
- VTRN/VUZP/VZIP 的两目标相同时，ARMv7 定义目的数据为 UNKNOWN，未授权把该情况转换为非法指令。原实现错误地使用 `UnpredictableInstruction`。现选取确定性的原数据作为 UNKNOWN 结果，不触发异常；A32/Thumb、D/Q 目标的纯语义回归和 x64 原生执行均验证其余寄存器、flags、FPSCR 继续保留。

本轮逐编码参考为 Arm 官方 `ARM Architecture Reference Manual ARMv7-A and ARMv7-R edition`，`DDI0406C.d / ID040418`。本机 PDF SHA256：`294668ae6480133b32d85e9567cc77c5eb0e1232decdf42cac7ab480e884f6e0`。双 word 见 A8.8.73–75、A8.8.211–212；UNKNOWN 向量数据见 A8.8.421、423、424；逐编码的寄存器重叠、保留位和版本条款写入 `docs/evidence/wsl-arm-x64-20261111/arm32-exception-contracts.json`，包含原始 form、case hash 和编码字。

### 当前二进制验证

- 新增 84 例覆盖七种 ARM/Thumb addressing 形式 × load/store × 正常对齐、第一/第二访问失败和 alignment fault。12 个 x64 配置（Block/Function × Known/Unknown/EarlyOff × opt on/off）共 **1,008 次实际原生执行**，以及 84 次 Direct，全部符合独立的结果、部分提交、写回及访问元数据预期。
- QEMU Cortex-A15 单步复核 56 例，目标寄存器、base writeback 和 stop signal 全部一致；不把 QEMU 未直接导出的 access-width 当作已验证。
- 永久 ARM32 runtime 回归对每种形式、两种对齐路径和 fault 边界运行三种 target policy、两种 scope 与 JIT/Interpreter；包含对第二 word 故障前的 load/store 第一 word 提交、故障时不写回 base 的检查。
- 最终构建重新执行基础矩阵 **4,224/4,224 agreement**，浮点矩阵 **27,648/27,648 agreement**，均实际原生执行，无解释或失败。
- 完整 CTest 的 JUnit 记录为 **38 total / 31 passed / 7 skipped / 0 failed**；这更正了此前把 CTest 的“38 total”误写为“38 passed”的文字。跳过条目仍是本宿主不能执行的其他架构 native 项。证据为 `boundary-final-build.log`、`boundary-ctest.log` 与 `boundary-ctest.xml`。

### 原始 inventory 差异的最终裁决

原始 72 个失败输入的当前严格重放仍明确报告 **4 agreement / 65 exception_mismatch / 3 semantic_mismatch**，未改变比较器、掩码或原始分类。

最终二进制也完整重放了全部 824 个 ARM32 inventory 输入：599 verified、15 verified_with_gap、140 oracle_unsupported、65 exception_mismatch、1 oracle_cpsr_gap、1 profile_gap、3 semantic_mismatch。所有原始差异仍可在 `boundary-full-inventory-summary.json` 追溯；不把不可比较的 oracle 项或 UNKNOWN 数据计为数值通过。

其中 4 agreement 是已修复的三种 SEVL 和一个 LDRD literal word-boundary fault。65 个异常差异逐项审查为 64 个有明确 UNPREDICTABLE 条款的重叠/零 mask 编码，以及一个 SSAT16 空间的保留位编码；VM 的拒绝策略保留。三个 NEON 同目标编码现在正常执行，但 Oracle 选择的 UNKNOWN 位模式与本项目选择不同，因此严格比较器仍保留 semantic_mismatch，不能据此伪称数值全一致。这些审查结论只适用于 JSON 中列出的具体编码，不把整个指令族标为通过。

本机详情位于 `build/wsl-arm-x64/boundary-inventory-summary.json`、`dualword-summary.json`、`dualword-qemu.json`、`boundary-basic-summary.json`、`boundary-fp-summary.json`；完整 observations 保存在 WSL cache 的 `dualword-validation`、`boundary-inventory-replay`、`boundary-basic-matrix` 和 `boundary-fp-validation`。

### 第四批性能与 compile 门

性能基线为第二批完成后的 `direct-fp-candidate`，候选为第四批最终构建。两份 runner 都复制到同一 Linux 文件系统，固定 CPU2、pin=0、交错新进程，每组 31 个样本；普通 wall time 与 compile 诊断仍分开。把候选放在 `/mnt/d`、基线放在 Linux cache 的试跑存在重复取页干扰，已判为不可比较，相关数据留为 `invalid-drvfs-summary.json`，不使用其性能结论。

六种原有整数／访存／浮点微基准 × 两种 scope 的候选／基线普通 wall time 为 **0.979–0.993**，compile 中位数为 **0.688–0.962**，生成代码大小和 spill 不变。小幅变化包含二进制布局与测量波动，不把它们全部当成本批优化的直接收益。

LDRD/STRD 专门循环每次读两 word、修改数据、再写两 word，重复 3,000,000 次。完整输出先通过独立整数计算及 QEMU 检查。修复必须保存按地址选择的两种访问合同，因此有额外的本地分支和冷路径；此组明确存在运行时间与代码大小的代价，不能用通用微基准掩盖：

| Guest / alignment | Scope | 普通 wall time 候选／基线 | compile_ns 候选／基线 | native bytes 前→后 |
| --- | --- | ---: | ---: | ---: |
| ARM / 8 | Block | 1.202× | 0.838× | 1988→2700 |
| ARM / 8 | Function | 1.212× | 0.782× | 1084→1452 |
| ARM / 4 | Block | 1.230× | 0.810× | 1988→2700 |
| ARM / 4 | Function | 1.237× | 0.808× | 1084→1452 |
| Thumb / 8 | Block | 1.214× | 0.792× | 1972→2684 |
| Thumb / 8 | Function | 1.219× | 0.771× | 1080→1448 |
| Thumb / 4 | Block | 1.245× | 0.841× | 1972→2684 |
| Thumb / 4 | Function | 1.245× | 0.790× | 1080→1448 |

将最初的 AND＋非零测试改为既有 TestBit，减少一个 IR 数值操作后，这个必要正确性路径的专门循环额外时间由最初的约 27%–31% 降为约 **20%–25%**。这里不宣称 LDRD/STRD 算术吞吐获得提升，也未增加仅为抵消该成本的分析表或 pass。

综合 C 程序在当前构建上再次完成 A64/A32/Thumb × Block/Function × JIT/Mixed/Interpreter 共 **18/18 次完整输出匹配**。Thumb Interpreter 首次超过脚本的 60 秒上限，确认旧进程已因 timeout 结束后单独重跑；重跑与原版均正常完成相同的 23,425,546 条 guest 指令，未把 timeout 计为通过。最终输出和退出状态另有完整记录。

较大程序的 compile 中位数（同样 31 个交错样本）：

| Guest | Block 前→后 | Function 前→后 |
| --- | ---: | ---: |
| A64 | 2.282→2.196 ms | 2.014→1.943 ms |
| A32 | 2.117→2.049 ms | 1.732→1.656 ms |
| Thumb | 1.979→2.031 ms | 1.609→1.616 ms |

未观察到明显 compile 退化；Thumb 的 +2.6%／+0.4% 未超出本机冷启动诊断的波动范围。Thumb native bytes 因双 word 冷路径增加约 1.8%／2.5%；另外两个 ISA 的综合程序大小未变。此前第二批 FP 和第一批 flags 的隔离性能结果仍绑定各自的 runner，不与本批比例直接相加。

本机记录为 `bench-boundary/summary.json`、`bench-dualword/summary.json` 和 `boundary-programs/summary.json`。`boundary-final-audit.json` 校验该次构建的 runner、correctness binary、源码身份及各验证 summary；本系列的性能与差分证据绑定该次构建。

可随源码审阅的精简证据见 `docs/evidence/wsl-arm-x64-20261111/verification-summary.json` 和同目录的 `arm32-exception-contracts.json`。当前构建完成全部已运行测试、有限差分矩阵、综合程序及 compile 门；已知的不可预测／保留编码与 UNKNOWN 数据差异已逐项裁决。双 word 严格访存的运行与代码体积代价保留为明确限制，未以更多分析机制换取微基准数字。

## 提交前的远端同步与集成检查

整理提交时，远端 `dev` 已新增 `71a9b2c0`（LoongArch64 bitfield、mask-select 和 rotate 优化）。该提交与本系列没有文件重叠，已先快进合入，再在 WSL 完成构建与完整 CTest：**38 total / 31 passed / 7 skipped / 0 failed**。上述证据记录的 19 个源码哈希与合入后的文件全部一致。

集成构建的二进制哈希和测试结果另存于 `verification-summary.json` 的 `integration_check`，本机日志为 `push-integration-build.log`、`push-integration-ctest.xml` 和 `push-integration-summary.json`。前文差分矩阵和性能测量仍绑定原二进制哈希；此次集成检查没有重新运行这些矩阵或性能采样。

## 后续：ARM64 → x64 按需标志位与常量移位优化

本轮单独以已提交的 `67768e23` 为基线，在 WSL 的 AMD Ryzen 9 7950X3D 上测量。基线与候选 runner 均复制到同一 Linux 文件系统，使用相同 guest ELF、CPU2、pin=0、Release/IPO 配置。每组先预热，再交错运行 31 对新进程，普通 wall time 与 `--decode-timing` 分开采样。整数、访存与新增移位循环各 12,000,000 次；FP 循环仍为 100,000 次。所有结果均检查完整输出、无 fallback 和无解释步数。

### 实现与正确性

- `PackFlags` 直接使用已有 `flag_outputs`，只打包需要的标志位。ARM 的 NZCV 更新不再生成 PF 源字节和 AF；只要求 ZF 时也不计算 CF、OF、SF。存在命名 snapshot 的 producer 仍保存全部算术标志，后续 `SaveFlags` 不受影响；未选中的标志和保留位继续由原来的合并路径保留。
- `Shift` 对立即数计数直接发射 x64 immediate shift/rotate；计数按原位宽截断，零计数不发射移位。寄存器计数仍使用原来的 CL 路径；输入截断、符号扩展和结果截断保持原合同。
- 两处均为现有 x64 emitter 内的局部选择，没有新增 IR pass、分析表、宿主特性要求或 guest 浮点语义。

永久回归覆盖 51,840 组算术结果和选定标志位检查，以及 5,376 组 immediate/variable shift、四种位宽、两种 scope、边界计数和 guest flags 保留检查。另专门验证 producer 当时只写 ZF、后续仍可恢复完整命名 snapshot。WSL 完整构建与 CTest 为 **38 total / 31 passed / 7 skipped / 0 failed**。

严格差分使用相同输入的既有独立 Unicorn 参考观察，重新运行 A64/A32 × Block/Function × Known/Unknown/EarlyOff × opt on/off：**4,224/4,224 agreement**，且每次均实际原生执行。未改比较器、掩码或 oracle 分类。四个正式 ARM64 微基准的完整输出另与 QEMU 一致；新增 shift workload 的 A64/A32 短输入也分别通过 QEMU 与两种 scope。上述为有限覆盖，不代表整个 ISA 的证明。

### 最终性能

表中的比例为候选中位数／基线中位数；小于 1 表示耗时下降。

| 工作负载 | Scope | wall time 比例 | compile_ns 比例 | native bytes 前→后 |
| --- | --- | ---: | ---: | ---: |
| A64 integer | Block | 0.892 | 0.923 | 1224→1144 |
| A64 integer | Function | 0.867 | 0.916 | 716→668 |
| A64 memory | Block | 0.901 | 0.936 | 1264→1208 |
| A64 memory | Function | 0.888 | 0.916 | 732→700 |
| A64 shift | Block | 0.867 | 0.935 | 1272→1184 |
| A64 shift | Function | 0.848 | 0.920 | 732→684 |
| A64 FP | Block | 0.989 | 0.953 | 1452→1404 |
| A64 FP | Function | 0.989 | 0.946 | 864→832 |
| A64 综合编译程序 | Block | 0.943 | 0.947 | 72664→66636 |
| A64 综合编译程序 | Function | 0.939 | 0.960 | 49332→43872 |

综合程序沿用前文的 freestanding C：SHA256、排序、整数混合和 FP 矩阵，由 Clang/LLD 编译成完整静态 ELF。基线与候选每次输出的全部 56 字节均与 QEMU 一致，SHA256 字段还与 Python 独立计算一致。Block wall time 为 32.334→30.502 ms，Function 为 32.219→30.259 ms；31 对样本中分别有 29 对和 27 对候选更快。所有 workload 的 spill slots 未增加，综合程序生成代码减少约 8.3%／11.1%。

为辨别改动贡献，先单独测量 flags 优化：3,000,000 次循环、11 对样本中，整数／访存 wall time 比例为 0.906–0.923。随后在 flags 候选上单独加入 immediate shift，shift 循环的额外比例为 0.966–0.968。它们绑定各自输入和构建，不与最终比例相加。FP helper 没有修改，约 1% 的 wall time 差异不作为 FP 算术加速结论。compile 中位数均未退化，但不把编译计时差异全部归因于 emitter；没有使用 PMU，也不将这些 WSL 结果外推为一般应用吞吐。

### 复现与证据

基准工具新增 `--isa arm64|arm32|both` 和可重复的 `--workload`；默认仍运行原有两种 guest 的 integer/memory/FP 三项。新增 shift 预期由 Python 的定宽整数运算独立计算。

```sh
WORK="$HOME/.cache/SwiftVM/arm64-x64-perf-20261111"
python3 tools/bench_arm_x64.py \
  --baseline "$WORK/baseline/svm_rebuild_linux" \
  --candidate "$WORK/candidate/svm_rebuild_linux" \
  --out build/arm64-x64-perf/reproduce --isa arm64 --cpu 2 \
  --samples 31 --iterations 12000000 \
  --workload integer --workload memory --workload fp --workload shift
```

可审阅的精简证据为 `docs/evidence/arm64-x64-perf-20261111/summary.json`，包含基线／候选哈希、源码哈希、各组中位数、配对样本分布、代码体积、差分配置和限制。全部逐样本记录在上述 Linux cache 的 `final-benchmark/summary.json`、`flags-screen/summary.json`、`shift-screen/summary.json`、`compiled-program/summary.json`；完整 case observations 位于 `correctness/`。综合程序源码与 ELF 保留在 `compiled-program/`；其构建和测量脚本为本机 `build/arm64-x64-perf/bench_programs.py`。CTest 日志与 JUnit 位于 `build/arm64-x64-perf/final-ctest.log` 和 `final-ctest.xml`。
