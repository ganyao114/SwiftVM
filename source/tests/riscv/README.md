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

三个 CTest 分别验证默认函数模式、`SVM_FUNC_BASE=0` 单块模式和代码缓存地址复用。
最终三项均通过：前两项各 72,545 个检查，缓存复用 23 个检查。
原版 QEMU 缓存复用负对照返回 1；RV64 AOT 编译入口也按预期返回非零并明确拒绝。
主要检查包括：

- 240 个标量程序、23,040 次输入执行，对照未进行 ARM64 寄存器重写的 canonical IR interpreter。
- 16 种条件的全部 NZCV 组合，未显式标注类型的 flags 谓词及独立预期值。
- 标量/helper/V128 混合、三种 select 的完整 V128 结果、全部整数与浮点 callee-saved 寄存器、远分支及大栈/均匀区偏移。
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

额外执行的原生 `swift_aot_call_test` 有 4/6 用例失败：安装仅包含 6 个代码单元，
扫描器拒绝当前 ARM64 发码中的 ADR/ADRP 和离开单元的 PC-relative branch。
回退本次 `TestFlags` 返回类型修改、重新构建后，仍然得到同样的四项失败；恢复修改后再重新构建。
关闭 `SVM_REGION_EDGES` 的诊断运行同样失败。此项 ARM64 序列化兼容缺口仍待单独修复，
不能计作通过的回归。相关日志是
`/tmp/swiftvm-rv64-aot-constructor-control-test-20261007.log` 和
`/tmp/swiftvm-rv64-review-aot-diagnostics-20261006.log`。

构建/测试日志采用本次任务开始日期作为文件前缀：
`/tmp/swiftvm-riscv-backend-native-final-build-20261006.log`、
`/tmp/swiftvm-riscv-backend-native-test-20261006.log`、
`/tmp/swiftvm-riscv-backend-native-abi-ctest-20261006.log`、
`/tmp/swiftvm-riscv-backend-cross-build-20261006.log`、
`/tmp/swiftvm-riscv-backend-cross-ctest-20261006.log`。
真实 guest stdout/stderr 在 Ubuntu 的 `/tmp/swiftvm-rv64-guests-20261006/` 中。
最终构建的复跑结果为 `complete-results.json`，对应输出使用 `.complete.stdout` / `.complete.stderr` 后缀。
