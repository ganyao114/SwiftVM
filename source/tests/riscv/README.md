# RV64 执行测试入口

这里的原生 C++ emitter 直接链接 Biscuit，生成 RV64G 裸函数及输入/预期结果。
Python runner 将它们装入无 libc 的静态 Linux ELF，通过 `qemu-riscv64` 执行。
当前验证 Biscuit 发码及测试入口；这些结果不计作尚未实现的 SwiftVM RISC-V 后端覆盖。

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
主工程同样提供 `swift_riscv_emit` 目标；启用 `SVM_RISCV_QEMU_TESTS` 后注册上述 CTest。
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
fixture 保留调用者临时寄存器 `t6` 用于函数入口标记；这是测试协议，尚非 Runtime ABI。
默认 QEMU CPU 显式关闭 C、V、Zba/Zbb/Zbs/Zbc，检查 RV64G 基线。
例如 Biscuit 的 `ZEXTW` 使用 Zba 指令，基线中的 32 位结果清高位使用移位序列。

下一阶段接入 IR 后端时应保留同一输入和执行结果对照，并使用独立的、尚未进行宿主
寄存器重写的 IR 运行解释器。helper 地址需要在 RISC-V 进程内绑定。
当前没有浮点/RVV、异常恢复、SMC、真实 Runtime/helper、原子和多线程内存序覆盖；
QEMU 运行时间也不作为性能成绩。真机或完整系统测试负责后续相关验收。

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
