# RV64 执行测试入口

这里的原生 C++ emitter 直接链接 Biscuit，生成 RV64G 裸函数及输入/预期结果。
Python runner 将它们装入无 libc 的静态 Linux ELF，通过 `qemu-riscv64` 执行。
裸函数 smoke 验证 Biscuit 发码及测试入口；完整 IR 后端使用独立的 `swift_riscv_backend_test`，见下文。

## 构建与运行

在具备 Python 3.9+、C++20 编译器、Clang、LLD 和 `qemu-riscv64` 的 Linux 环境中：

```sh
cmake -S source/tests/riscv -B /tmp/swiftvm-riscv-tests \
  -DCMAKE_BUILD_TYPE=Release -DSVM_RISCV_QEMU_TESTS=ON
cmake --build /tmp/swiftvm-riscv-tests --target swift_riscv_emit --parallel 1
ctest --test-dir /tmp/swiftvm-riscv-tests -R '^swift_riscv_smoke$' \
  --no-tests=error --output-on-failure
```

这个独立构建不需要 SwiftVM 运行时、Boost、Unicorn 或 RISC-V C++ sysroot。
主工程的原生构建同样提供 `swift_riscv_emit` 目标；启用 `SVM_RISCV_QEMU_TESTS` 后注册上述 CTest。
交叉构建中显式启用此选项会报错，因为 runner 需要原生 emitter；请另建独立原生目标，或搬运 `--bundle`。
默认关闭执行测试，并在配置输出中明确说明。显式启用时，缺 Python、Clang 或 QEMU
会使配置失败；缺 LLD、交叉汇编能力或目标 ISA 则使执行测试失败。
`--no-tests=error` 防止测试未注册时得到零用例成功。

也可以直接运行，工具路径可通过 `--clang` 和 `--qemu` 指定：

```sh
python3 tools/run_riscv_smoke.py \
  --emitter /tmp/swiftvm-riscv-tests/swift_riscv_emit \
  --artifact-root /tmp/swiftvm-riscv-results
```

每次运行创建独立目录，保留 `result.json`、生成代码、汇编包装、ELF、工具版本、
完整命令和 stdout/stderr。成功需要正常用例全部执行，并通过故意改错预期值的负对照。
超时或缺工具都返回非零，不会被转换成跳过成功。

## macOS 发码、Ubuntu 执行

在 macOS 构建上述独立 emitter，然后生成可搬运的 bundle：

```sh
/tmp/swiftvm-riscv-tests/swift_riscv_emit /tmp/swiftvm-rv64-bundle
```

目录必须为空或尚不存在。bundle 使用相对文件名和架构值，不包含宿主函数地址。
在 Orb Ubuntu 中使用挂载路径执行，例如：

```sh
orb -m ubuntu python3 \
  /Users/swift/CLionProjects/SwiftVM/tools/run_riscv_smoke.py \
  --bundle /mnt/mac/private/tmp/swiftvm-rv64-bundle \
  --artifact-root /tmp/swiftvm-riscv-results
```

`--bundle` 只读取已有产物，适合跨宿主执行；`--emitter` 则在 runner 所在环境内发码。

## 当前检查与边界

- 64 位常量、返回、加减、32 位结果清高位、有符号/无符号分支、前向及循环回跳。
- 8/32 位有符号和无符号加载、带偏移的 64 位加载、32/64 位存储及相邻内存保留。
- 嵌套调用、`sp` 保持及 16 字节对齐、`s0`–`s11` 保持。
- 每个函数写入独立执行标记，防止未调用代码或调用错入口仍通过。
- 输入和预期结果由 C++ 的整数规则计算，runner 比较返回值和全部四个内存字。

函数协议为 `a0/a1` 输入、`a2` 指向四个 64 位内存字、`a0` 返回结果。
fixture 保留调用者临时寄存器 `t6` 用于函数入口标记；这是裸函数测试协议。Runtime 使用 `HaltReason block(State*)`。
默认 QEMU CPU 显式关闭 C、V、Zba/Zbb/Zbs/Zbc，检查 RV64G 基线。
例如 Biscuit 的 `ZEXTW` 使用 Zba 指令，基线中的 32 位结果清高位使用移位序列。

## 完整 SwiftVM 后端测试

[RV64 后端说明](../../runtime/backend/riscv64/README.md) 包含交叉编译和 CTest 命令。
完整后端程序在 RV64 进程内绑定 helper 和 IR 地址；macOS 仅编译此目标，不原生执行它。
QEMU 的动态 sysroot 保留 C 扩展，但关闭 V、Zba/Zbb/Zbs/Zbc。
双映射代码复用需要 [QEMU 测试补丁](../../../tools/qemu/README.md)，原版模拟器仍用于失败负对照。

十个 CTest 验证默认函数模式、`SVM_FUNC_BASE=0` 单块模式、代码缓存复用、Zbb、
标量加密扩展、VLEN=128/256 的 RVV、两个 VLEN 的向量加密扩展，以及 Zacas。
前三类基线与普通 RVV 使用补丁版 QEMU 8.0.4；向量加密和 Zacas 使用补丁版
QEMU 10.1.2，通过 `SVM_RISCV_QEMU_CRYPTO_EXECUTABLE` 显式指定。当前十项全部通过。
原版 QEMU 缓存复用负对照返回 1；RV64 AOT 编译入口也按预期返回非零并明确拒绝。

原生发码用例逐类断言无 interpreter 分派，并比较 cached/uncached 与独立参考结果。
覆盖全部标量宽度、flags 请求掩码、原子 RMW、向量 lane 宽度、全部 shuffle 控制、
浮点舍入/比较模式和 NaN/溢出边界；检查 LP64D callee-saved 寄存器及 caller FRM。
专门的 host-call 测试验证 0–8 参数、栈参数、动态目标、宽除法、成对结果，以及调用时
完全破坏 RVV 寄存器后的活跃浮点值恢复。CAS128 包括四线程同时更新的完整成对不变量。
memmove 用例覆盖重叠方向、全部字节对齐、零长度、边界故障和超过 VLEN 的复制。
AES 同时验证全部轮 IR 与 FIPS 197 的十轮已知答案。

`--fp`/`--rvv-fp`、`--calls`/`--rvv-calls`、`--memcopy`/`--rvv-memcopy`、
`--crypto`/`--scalar-crypto`/`--vector-crypto`、`--zacas` 可以分别运行对应用例。
`--structure`/`--rvv-structure` 检查逻辑 host GPR/FPR 绑定、全部部分字段、捕获语义、
回调更新与向量寄存器破坏，以及 Phi 的前向选择、循环交换、跨寄存器/栈槽复制环和窄立即数。
`--memory`/`--rvv-memory` 检查条件 ABI 帧、重复 oracle 回调、callee-saved/FRM 保持，
以及调用前、oracle 内和 oracle 返回后的故障恢复。
原生浮点基线已通过 99,176 项检查，普通 RVV 浮点通过 99,330 项；内存复制/CAS128
专测通过 10,599 项；向量加密专测通过 957 项，缓存复用通过 23 项。

[覆盖清单](../../../docs/riscv64-ir-coverage.md) 由 `ir.inc` 和实际注册的 emitter 生成。
`python3 tools/check_riscv_ir_coverage.py --check-document` 验证清单同步，
`--require-native` 返回 0：191 条 opcode 包括 188 条数据/状态 lowering 与 3 条局部控制流 lowering。
这仍是 opcode 清单；跨块 SSA、未拆分 V256 与缺少 uniform 绑定的 Host-register IR 明确拒绝。
源码 case 数量和 QEMU 时间都不能替代真机性能验收。用例对 RVV 指令序列设置预算，
并检查算术/AES 串联没有 SSA 栈流量；吞吐、延迟、跨 hart 原子顺序与 SMC 仍须在目标 CPU 测量。
host GPR 运算链无 SSA 栈访问，完整读写各用一条寄存器移动；归一化的 32 位发布也只用一条移动，
低字节发布用两条指令。标量和 RVV 循环 Phi 在寄存器充足时同样没有 SSA 栈访问。
叶块只保存实际使用的 GPR，不保存浮点 callee-saved 寄存器；条件 ABI 路径也使用精简入口/出口，
首次实际调用时补存其余寄存器并切换完整故障恢复入口。显式 host call 保留完整入口保存。

2026-10-07 的交叉构建还执行了 `func_tests_x86_64` 和 `func_tests_aarch64`，
全部 stdout 与既有基线逐字节一致，退出码分别为 101 和 25。原生 ARM64 的
462 个回归用例通过，共 1,126,860 次断言。
主要检查包括：

- 240 个标量程序、23,040 次输入执行，对照未进行 ARM64 寄存器重写的 canonical IR interpreter。
- 16 种条件的全部 NZCV 组合，未显式标注类型的 flags 谓词及独立预期值。
- 标量/helper/V128 混合、三种 select 的完整 V128 结果、全部整数与浮点 callee-saved 寄存器、远分支及大栈/均匀区偏移。
- 九个标量缓存寄存器的淘汰与重载、目标覆盖输入、窄结果、helper 隐式输入、成对返回值、
  scalar-to-V128 高半部、前向汇合及有限后向循环；同时执行关闭缓存的对照程序。
- CFG 边上的活跃值选择、落空路径保留缓存、Phi 的并行交换及寄存器不足时的复制环，
  host GPR/FPR 的部分写入、回调观察/更新与故障后最后一次发布状态。
- 有符号 12 位访存偏移的边界与超界路径，验证全部相邻字节，检查短偏移不生成额外地址指令。
- guest 范围末端、未对齐/TSO 访问、两个并发执行线程间对齐读写的完整性。
- L2 分派（包括最后一个 bucket）、中断、局部后向循环、嵌套 terminals。
- 生成代码与 C++ helper 的故障恢复、故障后 fallback 原子锁释放、helper 异常隔离及 Runtime 复用。
- 原子 exchange/fetch-add/CAS、IR 在 detach/回收之间的生命周期、HIR pool 释放后的函数内部入口，
  以及嵌套 terminal 中的跨块 SSA 拒绝检查。
- x86-64 与 AArch64 前端经 Runtime 到 syscall 的实际执行。

这些检查不是完整多 hart 内存序/SMC 或 FP 模式资格验证；QEMU 运行时间不作为性能成绩。

## 2026-10-06 验证记录

构建和执行均串行完成：

| 检查 | 结果 |
| --- | --- |
| macOS ARM64 独立构建及主工程 `swift_riscv_emit` 目标 | 通过 |
| macOS 发码、Orb Ubuntu 执行 | 19 个函数、49 个场景通过；负对照按预期返回 1 |
| Orb Ubuntu 独立构建及 `swift_riscv_smoke` CTest | 同样执行全部 49 个场景并通过负对照 |
| runner 防止零用例、部分执行和错误负对照的单元测试 | 3 个测试通过 |
| 指定不存在的 QEMU 工具 | 按预期返回 1，并留下失败记录 |
| 未注册执行测试时使用 `ctest --no-tests=error` | 按预期返回 8 |

Ubuntu 使用 Clang 16.0.6、QEMU 8.0.4，CPU 为文中默认基线配置。
日志为 `/tmp/swiftvm-riscv-smoke-crosshost-final-20261006.log`、
`/tmp/swiftvm-riscv-smoke-linux-ctest-20261006.log`；运行日志列出各自保存产物的目录。

## 2026-10-06/07 完整后端验证记录

完整 SwiftVM RV64 进程使用 GNU 13.2、`-march=rv64g -mabi=lp64d`、Release 的
`-O1 -DNDEBUG` 宿主构建，通过 Orb Ubuntu 中的补丁版 QEMU 8.0.4 执行。
之前的独立裸函数 smoke 记录仍使用原版 QEMU，二者验收范围不同。

真实 Linux guest 的输出和退出码对照仓库预期及原 ARM64 后端：

| Guest | 结果 |
| --- | --- |
| `loop_x86_64` | 输出 `5050`，退出 186 |
| `real_hello_x86_64` | 输出 `Hello, real glibc!`，退出 42 |
| `func_tests_x86_64` | 七行输出一致，checksum `9f52b7d59285dbe5`，退出 101 |
| `real_hello_aarch64` | 输出 `Hello, real glibc!`，退出 42 |
| `func_tests_aarch64` | 七行输出与原 ARM64 后端一致，checksum `7d907f8b01114299`，退出 25 |

最终构建重新执行两个 `func_tests` guest，stdout 逐字节一致，退出码仍分别为 101 和 25。

其中 AArch64 glibc 首次暴露了 `TestFlags` / `TestNotFlags` 无返回类型时丢弃结果的问题；
现在这两个 flags-only 谓词默认返回 U8。独立 NZCV 预期值检查和原生 interpreter 测试均覆盖它。
原生 macOS ARM64 `swift_test` 通过 460 个用例、1,126,818 个断言。
原生 guest-call CTest 也通过。

初次额外执行的原生 `swift_aot_call_test` 有 4/6 用例失败：安装仅包含 6 个代码单元，
扫描器拒绝当前 ARM64 发码中的 ADR/ADRP 和离开单元的 PC-relative branch。
回退本次 `TestFlags` 返回类型修改、重新构建后，仍然得到同样的四项失败；恢复修改后再重新构建。
关闭 `SVM_REGION_EDGES` 的诊断运行同样失败。初期诊断日志是
`/tmp/swiftvm-rv64-aot-constructor-control-test-20261007.log` 和
`/tmp/swiftvm-rv64-review-aot-diagnostics-20261006.log`。

2026-10-07 后续已修复此兼容缺口：离线发码不依赖共享 region trampoline，
收集器取完整 allocation，并随产物携带精确 fault/recovery 元数据。
扫描器允许单元内部 ADR，格式更新为 AOT version 3 / cache validity version 23。
原生回归通过 462 个用例、1,126,837 个断言；AOT 的 8 个用例、22,473 个断言全部通过，
guest-call 的 43 个用例也通过。AOT 端到端脚本 36 项检查全部通过，包含真实 guest、
损坏恢复地址拒绝、独立 ELF 解析和 SMC。实现说明见 [AOT 设计](../../../docs/aot-design.md)。
日志为 `/tmp/swiftvm-aot-native-full-20261007.log`、
`/tmp/swiftvm-aot-final-ctest-20261007.log` 和 `/tmp/swiftvm-aot-e2e-final-20261007.log`。
此修复后 RV64 三项执行测试再次通过（72,545 / 72,545 / 23），
RV64 AOT 入口仍以预期消息返回 1；此变更仅恢复 ARM64 AOT 支持。
复验日志为 `/tmp/swiftvm-aot-rv64-ctest-20261007.log`，
拒绝记录位于 Ubuntu 的 `/tmp/swiftvm-aot-rv64-rejection-20261007.log`。

构建/测试日志采用本次任务开始日期作为文件前缀：
`/tmp/swiftvm-riscv-backend-native-final-build-20261006.log`、
`/tmp/swiftvm-riscv-backend-native-test-20261006.log`、
`/tmp/swiftvm-riscv-backend-native-abi-ctest-20261006.log`、
`/tmp/swiftvm-riscv-backend-cross-build-20261006.log`、
`/tmp/swiftvm-riscv-backend-cross-ctest-20261006.log`。
真实 guest stdout/stderr 在 Ubuntu 的 `/tmp/swiftvm-rv64-guests-20261006/` 中。
最终构建的复跑结果为 `complete-results.json`，对应输出使用 `.complete.stdout` / `.complete.stderr` 后缀。

## 2026-10-07 标量缓存验证记录

直接标量结果使用 `s3`–`s11`，语义 helper 和局部控制流边界前写回标准槽。
发码保留可关闭缓存的内部构造参数，用于同一 IR 的执行与发码对照；Runtime 默认启用缓存。
对照程序同时使用本轮的短偏移访存和条件提取优化，下面的差异仅来自寄存器缓存。

48 次 U64 加法链的生成代码统计：

| 指标 | 关闭缓存 | 启用缓存 |
| --- | ---: | ---: |
| 完整块代码字节 | 1,132 | 1,096 |
| SSA 栈读取 | 49 | 0 |
| SSA 栈写回 | 49 | 40 |

SSA 统计不包含初始化清零、帧保存/恢复、uniform 或 guest 访存。该直线程序的 SSA 栈访问
减少约 59%；此结果衡量生成代码，不能推导真机运行时间。八种有符号/无符号宽度都执行
边界输入，并使用独立整数预期值检查缓存与关闭缓存的结果。

本轮串行验证结果：

- macOS 编译 `swift_riscv_backend_test` 通过；macOS 不执行 RV64 代码。
- RV64 GNU 交叉构建通过，三个 CTest 全部通过（72,879 / 72,879 / 23 个检查）。
- 新增寄存器压力、目标覆盖输入、helper 隐式输入、成对返回值、高半部清零、局部汇合、
  有限回跳及有符号访存偏移边界检查；原有 fault、异常、并发访存和 ABI 检查继续通过。
- 两个完整 `func_tests` guest 的七行 stdout 与先前基线逐字节一致，退出码仍为 101 / 25。
- 原版 QEMU 的缓存复用负对照返回 1，失败原因精确命中旧的双映射代码执行。

macOS 日志为 `/tmp/swiftvm-rv64-scalar-native-build-final-20261007.log`、
`/tmp/swiftvm-rv64-scalar-cross-build-final-20261007.log`、
`/tmp/swiftvm-rv64-scalar-ctest-20261007.log` 和
`/tmp/swiftvm-rv64-scalar-execution-20261007.log`。
完整 guest 的运行摘要为 `/tmp/swiftvm-rv64-scalar-guests-20261007.log`，
逐字节输出和 `results.json` 在 Ubuntu 的 `/tmp/swiftvm-rv64-scalar-guests-20261007/` 中。
负对照记录为 `/tmp/swiftvm-rv64-scalar-stock-qemu-20261007.log`。

## 2026-10-07 完整 IR 与 CFG 验证记录

`tools/check_riscv_ir_coverage.py --check-document --require-native` 通过：191 条 opcode，
其中 188 条数据/状态 IR 和 3 条局部控制流 IR。覆盖范围为块内 SSA、最大 V128；
跨块 SSA 和未拆分 V256 仍明确拒绝。逻辑 host-register IR 需要兼容的 uniform 绑定。

完整十组 CTest 串行通过：基线和函数块各 282,673 个检查、缓存复用 23、Zbb 15,046、
标量 crypto 956、RVV128/RVV256 各 142,140、向量 crypto 各 957、Zacas 10,599。
随后新增的 13 路向量 Phi 复制环超过常驻寄存器容量，分别通过基线 666、
RVV128/RVV256 各 667 个结构检查，包含破坏 RVV 寄存器的回调。
原生 ARM64 回归通过 462 个用例、1,126,838 个断言。
最终重新执行两个完整 guest，输出逐字节一致，x86-64/AArch64 退出码分别为 101/25。

当前生成代码预算：48 次 U64 加法链关闭/启用缓存为 752/376 字节，SSA 读写从各 49 次
降为零；RVV 向量链为 396 字节，其中 VecAdd 为 196 字节，SSA 读写为零。
完整寄存器复制使用一条指令，规范化的 32 位 host GPR 写入使用一条指令；
低字节写入使用两条指令。叶块只保存实际使用的 GPR，不保存浮点 callee-saved 寄存器。
这些是发码预算与访存统计，不能推导硬件吞吐或延迟。

日志为 `/tmp/swiftvm-rv64-complete-ir-final-ctest-20261007.log`、
`/tmp/swiftvm-rv64-phi-vector-spill-focused-20261007.log`、
`/tmp/swiftvm-rv64-complete-ir-arm-regression-20261007.log` 和
`/tmp/swiftvm-rv64-complete-ir-guests-20261007.log`。
guest 输出与结果位于 Ubuntu 的 `/tmp/swiftvm-rv64-complete-ir-guests-20261007/`。

## 2026-10-07 条件 ABI 帧与 uniform 字节字段补强

oracle、未对齐原子和宽除法的条件 ABI 调用使用共享冷路径，首次实际调用才补存未修改的
GPR 与 12 个 callee-saved FPR。入口/正常出口只保存恢复实际修改的 GPR；未调用 oracle
的内存路径不执行原有的 24 条 FPR 栈读写。显式 host call、基线大复制和软件 CAS128
保留入口完整保存，避免频繁调用反复检查。故障仍能恢复完整 LP64D 集合和调用者 FRM。

新增无 host binding 的故障回归在旧处理器下返回 1，精确命中 `flags=0 path=0`：
旧处理器把 guest flags 无条件写入未保存的 `s11`。现在只有保留 flags 缓存的生成块
从专用故障入口重载这个寄存器，未使用 `s11` 的块保持调用者值。
调用前、oracle 内和 oracle 返回后的故障，以及开启/关闭 flags 缓存的路径均通过 ABI 检查。

canonical uniform 允许不按 lane 对齐的字节字段。全部合法偏移、标量/向量类型及
缓存开关均使用独立字节数组验证；跨越 64 位两半的 GPR 字段直接拼接/合并，
RVV 字段使用字节移位与掩码合并，不通过发布/重载整个绑定绕过问题。
最终专测在 RV64G、RVV128、RVV256 各通过 200 个内存/故障检查，
结构检查分别为 1,258 / 1,259 / 1,259。

故障负对照日志为 `/tmp/swiftvm-rv64-unpinned-s11-negative-20261007.log`，
最终专测日志为 `/tmp/swiftvm-rv64-lazy-abi-recovery-focused-20261007.log`。

完成 `s11` 故障修复后的最终十组 CTest 再次全部通过：基线/单块模式各 283,407，
缓存复用 23，Zbb 15,046，标量 crypto 956，RVV128/RVV256 各 142,854，
向量 crypto 各 957，Zacas 10,599。日志为
`/tmp/swiftvm-rv64-complete-final-ctest-20261007.log`。
算术链发码预算保持为 U64 752/376 字节、RVV 396 字节；启用缓存的 SSA 读写均为零。
macOS 原生构建三个目标通过；ARM64 回归通过 462 个用例、1,126,857 个断言。
日志为 `/tmp/swiftvm-rv64-complete-final-native-build-20261007.log` 与
`/tmp/swiftvm-rv64-complete-final-arm-regression-20261007.log`。
最后重新执行 x86-64/AArch64 完整 guest，七行 stdout 逐字节匹配，退出码为 101/25。
摘要为 `/tmp/swiftvm-rv64-complete-final-guests-20261007.log`，输出与 JSON 结果位于 Ubuntu 的
`/tmp/swiftvm-rv64-complete-final-guests-20261007/`。

## 2026-10-07 跨 block SSA、V256 与混合宽度原子协议

同一 HIR 函数的跨 block 值、terminal 条件和 Phi 在一份 SSA 帧内原生执行。
Phi 根据 HIR 前驱重排到实际入边，保留循环携带值和并行复制环；支配关系不成立的输入
在发布代码前拒绝。依赖前驱 SSA 的内部 block 只从函数根入口进入。中断用例验证内部
guest 边先提交目标 PC，再检查请求；条件展开的合成标签不会形成错误的恢复边界。
canonical HIR、展开 IR 和 executable allocation 使用同一 QSBR 生命周期。

V256 拆成两个原生 V128 半部，覆盖访存、局部变量、逻辑 FPR、选择、Phi、整数/浮点运算、
密码指令和完整宽度重排。专测在 RV64G、VLEN=128 和 VLEN=256 各通过 1,461 项检查，
包括饱和/窄化、全部 packed 浮点转换、FMA 别名、跨半部 shift/extract/zip/unzip/gather、
Phi 交换、全部 32 字节访存对齐、越界前拒绝以及 LP64D。缓存开启的 40 次 V256 加法链
不产生 SSA 读写，VecAdd 发码不超过两个既有 V128 预算：RV64G 8,064 字节、RVV 392 字节。
32 字节访问只调用一次完整范围 oracle；TSO 只在整个访问两端设置屏障，读写组合相比普通
访存恰好增加 16 字节发码。portable interpreter 明确拒绝 V256，避免越过其 16 字节槽位。

普通 guest 访存、原生 AMO/LR-SC/Zacas 和软件原子操作参加共同的读者/写者协议。
Runtime 使用私有 cache line，每个生成块获取一次租约；ABI 调用前释放、返回后重新获取，
内部回边为待处理写者让出租约。软件 CAS128/未对齐原子关闭入口、等待读者退出后更新。
匿名 State、interpreter、复制 kernel 与 RV64 x87 内存 helper 使用匿名计数。
x87 的 80 位访问、环境保存/恢复和 FXSAVE/FXRSTOR 保持一次完整访问的读者所有权；
只操作寄存器的 x87 指令以及 ARM64 helper 不增加同步开销。入口先发布读者并执行全栅栏，再读取
写者 gate；成功后执行 R/RW acquire 栅栏。退出用 RW/W release 栅栏，故障恢复清理当前
线程的全部所有权。外部 callback 的直接 guest 指针访问需要自行参加协议。

并发专测验证各 1,000 次软件 CAS128 与重叠 AMO64、未对齐 U16 与 AMO32 的更新，
以及原生/interpreter 读取的一致性。新增 X87Op 专测用软件 CAS128 同时发布关联的
significand/exponent，至少读取 1,000 次 Float80 并检查两个字段属于同一次更新。
Zacas 组另外验证软件 CAS128 与 AMOCAS.Q 并发。
协议专测在 RV64G、RVV128、RVV256 各通过 12 项检查，Zacas 中通过 14 项；
跨 block SSA/Phi 专测在三个基础配置各通过 31 项检查。
这些是功能、ABI、发码预算和协议检查，真机吞吐、延迟和物理内存顺序验收仍待硬件。

本机三个构建目标通过；最终 ARM64 回归通过 462 个用例、1,126,852 个断言。
专测日志：`/tmp/swiftvm-rv64-wide-tso-final-20261007.log`、
`/tmp/swiftvm-rv64-rvv-wide-{128,256}-tso-final-20261007.log`、
`/tmp/swiftvm-rv64-{cfg,rvv-cfg-128,rvv-cfg-256,memory-protocol,zacas-protocol}-final-qualified-20261007.log`。
构建与 ARM64 回归日志：`/tmp/swiftvm-rv64-remaining-native-final-build-20261007.log`、
`/tmp/swiftvm-rv64-remaining-arm-regression-20261007.log`。

包含跨 block SSA、V256、混合宽度协议以及最后的 acquire/TSO 屏障修复后，十组 CTest
串行全部通过：基线和单块模式各 284,909、缓存复用 23、Zbb 15,046、标量 crypto 956、
RVV128/RVV256 各 144,346、向量 crypto 各 957、Zacas 10,611。
日志为 `/tmp/swiftvm-rv64-remaining-accepted-ctest-20261007.log`。
随后加入 x87 helper 同步，重新构建交叉及本机三个目标，并执行四种配置的并发协议、
完整 Zacas（10,613 项）、基线/RVV host-call ABI（698/700 项）及内存/故障恢复（200 项）专测。
日志为 `/tmp/swiftvm-rv64-x87-protocol-{baseline,rvv128,rvv256,zacas,calls,rvv-calls,memory}-20261007.log`、
`/tmp/swiftvm-rv64-x87-protocol-build-20261007.log`、
`/tmp/swiftvm-rv64-x87-native-build-20261007.log` 与
`/tmp/swiftvm-rv64-x87-arm-regression-20261007.log`。
最终 x86-64/AArch64 完整 guest 的七行 stdout 与基线逐字节相同，退出码分别为 101/25。
输出和结果位于 macOS 的 `/tmp/swiftvm-rv64-x87-final-guests-20261007/`。
