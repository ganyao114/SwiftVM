# RV64 JIT 测试用 QEMU

SwiftVM 通过 RW/RX 双映射发布代码，在 RV64 Linux 上使用
`__builtin___clear_cache` / `riscv_flush_icache` 同步指令缓存。
QEMU 8.0.4 的 linux-user 实现把这个 syscall 当作空操作；写 RW 别名不会使 RX
别名已有的 translation block 失效。本工作区的缓存地址复用测试在原版 QEMU 下失败，
应用 [riscv-flush-icache.patch](riscv-flush-icache.patch) 后通过。

补丁仅用于测试模拟器，在收到 syscall 时调用 `tb_flush`，不修改 SwiftVM 的内存映射协议。
它会清空所有 translation block，测试耗时不能用于评价后端性能。
原实现见 [QEMU v8.0.4 cpu_loop.c](https://github.com/qemu/qemu/blob/v8.0.4/linux-user/riscv/cpu_loop.c)。

## Ubuntu 构建

下面使用 QEMU v8.0.4，提交 `83a9cdbd65ceb4a443630aed011a00ef217ed408`。
在仓库根目录执行：

```sh
sudo apt-get install git build-essential pkg-config ninja-build meson python3 \
  libglib2.0-dev libpixman-1-dev g++-riscv64-linux-gnu qemu-user

task_repo=$PWD
task_qemu=/tmp/swiftvm-qemu-riscv-8.0.4
git clone --depth 1 --branch v8.0.4 https://github.com/qemu/qemu.git "$task_qemu"
git -C "$task_qemu" rev-parse HEAD
git -C "$task_qemu" apply "$task_repo/tools/qemu/riscv-flush-icache.patch"
git -C "$task_qemu" apply "$task_repo/tools/qemu/riscv-vector-shift.patch"
# 用命令级 URL 重写访问 QEMU 的 GitHub 子模块镜像，不修改全局 Git 配置。
git -C "$task_qemu" -c url.https://github.com/qemu/.insteadOf=https://gitlab.com/qemu-project/ \
  submodule update --init --depth 1 \
  ui/keycodemapdb tests/fp/berkeley-softfloat-3 tests/fp/berkeley-testfloat-3
mkdir "$task_qemu/build-swiftvm"
cd "$task_qemu/build-swiftvm"
../configure --target-list=riscv64-linux-user --disable-system --disable-tools \
  --disable-docs --disable-werror --disable-debug-info --disable-fdt \
  --meson=/usr/bin/meson --with-git-submodules=ignore --extra-cflags=-O0
ninja -j1 qemu-riscv64
cd "$task_repo"
sh scripts/build-riscv64-linux.sh /tmp/swiftvm-rv64 \
  -DSVM_RISCV_QEMU_EXECUTABLE="$task_qemu/build-swiftvm/qemu-riscv64" \
  "-DCMAKE_CROSSCOMPILING_EMULATOR=$task_qemu/build-swiftvm/qemu-riscv64;-L;/usr/riscv64-linux-gnu;-cpu;rv64,v=false,zba=false,zbb=false,zbs=false,zbc=false"
ctest --test-dir /tmp/swiftvm-rv64 -R '^swift_riscv_backend' \
  --no-tests=error --output-on-failure
```

显式传入 `CMAKE_CROSSCOMPILING_EMULATOR` 可以覆盖旧构建目录缓存的模拟器路径。
动态 sysroot 的 libc 使用 RV64GC，因此完整运行时测试保留 C 扩展；SwiftVM 自身的代码
和构建目标仅要求 RV64G，CPU 关闭 V、Zba/Zbb/Zbs/Zbc。

## 原版模拟器负对照

```sh
/usr/bin/qemu-riscv64 -L /usr/riscv64-linux-gnu \
  -cpu rv64,v=false,zba=false,zbb=false,zbs=false,zbc=false \
  /tmp/swiftvm-rv64/source/tests/riscv/swift_riscv_backend_test --cache-reuse
```

在已复现此问题的 QEMU 8.0.4 中，此命令应返回非零并指出双映射代码复用错误。
不要把这个失败改为 skip；如果使用另一版本的模拟器，应重新运行这个用例确认其缓存语义。
裸函数 smoke 测试不复用动态代码缓存，仍可使用原版 QEMU。

## 向量加密和 Zacas

Zvkned、Zvknha、Zvbc 和 AMOCAS.Q 的执行验证使用 QEMU v10.1.2，提交
`ccaea6b2656ec6eab966585f7b16438208f98de7`。该版本的替代 vDSO
`__vdso_flush_icache` 仍直接返回零；仅修改 syscall 入口不足以修复双映射缓存复用。
[版本专用补丁](riscv-flush-icache-qemu10.patch) 同时修复两个入口。

```sh
sudo apt-get install python3-venv
task_repo=$PWD
task_qemu=/tmp/swiftvm-qemu-riscv-10.1.2
git clone --depth 1 --branch v10.1.2 https://github.com/qemu/qemu.git "$task_qemu"
git -C "$task_qemu" apply "$task_repo/tools/qemu/riscv-flush-icache-qemu10.patch"
cd "$task_qemu/linux-user/riscv"
# QEMU 带有预生成 vDSO；修改源文件后必须先重新生成，不能只运行 ninja。
riscv64-linux-gnu-gcc -nostdlib -shared -fpic -Wl,-h,linux-vdso.so.1 \
  -Wl,--build-id=sha1 -Wl,--hash-style=both -Wl,-T,vdso.ld \
  -mabi=lp64d -march=rv64g -o vdso-64.so vdso.S
riscv64-linux-gnu-gcc -nostdlib -shared -fpic -Wl,-h,linux-vdso.so.1 \
  -Wl,--build-id=sha1 -Wl,--hash-style=both -Wl,-T,vdso.ld \
  -mabi=ilp32d -march=rv32g -o vdso-32.so vdso.S
mkdir "$task_qemu/build-swiftvm"
cd "$task_qemu/build-swiftvm"
../configure --target-list=riscv64-linux-user --disable-system --disable-tools \
  --disable-docs --disable-werror --disable-debug-info --disable-fdt --extra-cflags=-O0
ninja -j1 qemu-riscv64
cd "$task_repo"
cmake -S . -B /tmp/swiftvm-rv64 \
  -DSVM_RISCV_QEMU_CRYPTO_EXECUTABLE="$task_qemu/build-swiftvm/qemu-riscv64"
ctest --test-dir /tmp/swiftvm-rv64 -R '^swift_riscv_backend_(vector_crypto|zacas)' \
  --no-tests=error --output-on-failure
"$task_qemu/build-swiftvm/qemu-riscv64" -L /usr/riscv64-linux-gnu \
  /tmp/swiftvm-rv64/source/tests/riscv/swift_riscv_backend_test --cache-reuse
```

VLEN=128/256 的向量加密及 Zacas 用例均通过。仅 syscall 补丁时，缓存复用负对照
仍失败；同步重建 vDSO 后通过全部 23 项复用检查。模拟器的全局 TB 刷新成本不属于
生成代码的性能，不用于吞吐或延迟结论。

## RVV 算术右移的独立模拟器回归

在本工作区的 AArch64-host QEMU 8.0.4 中，`VL == VLMAX` 时的 `vsra.vx` gvec
路径会错误处理在同一 TB 中赋值的移位寄存器。独立于 SwiftVM/JIT 的
[vector_shift_probe.c](vector_shift_probe.c) / [汇编](vector_shift_probe.S)
复现了这个问题：两个 `0x8000000000000000` 输入按 8 位 lane 算术右移 7，
原版的 `.vx` 输出保持原样，`.vi` 输出正确的 `0xff00000000000000`。
同一测试在 VLEN=256 的 helper 路径正确。

[riscv-vector-shift.patch](riscv-vector-shift.patch) 让测试模拟器的 `vsra.vx` 使用现有
架构语义 helper。SwiftVM 仍生成原生 VSRA.VX；补丁没有为模拟器修改后端指令或降低性能预算。
QEMU 上的耗时不用于真机性能验收。升级模拟器后，应先重新执行这项独立回归再去掉补丁。

```sh
riscv64-linux-gnu-gcc -march=rv64g -mabi=lp64d \
  tools/qemu/vector_shift_probe.c tools/qemu/vector_shift_probe.S -o /tmp/swiftvm-vector-shift-probe
"$task_qemu/build-swiftvm/qemu-riscv64" -L /usr/riscv64-linux-gnu \
  -cpu rv64,v=true,vlen=128,elen=64 /tmp/swiftvm-vector-shift-probe
```

两行输出都必须是 `ff00000000000000 ff00000000000000`，退出码必须为 0。
后端 RVV 回归分别使用 VLEN=128 和 256，并用 `SVM_RV64_TEST_OP=127` 定位算术右移。
