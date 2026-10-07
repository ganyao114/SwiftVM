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
