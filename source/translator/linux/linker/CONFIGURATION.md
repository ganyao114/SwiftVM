# SwiftVM Guest 程序支持配置说明

## 概述

SwiftVM 现在支持加载和执行 guest 可执行程序，同时确保关键系统库（如 libc）使用 host 架构版本以获得最佳性能和兼容性。

## 快速开始

### 1. 基本配置

```bash
# 设置环境变量
export SWIFTVM_ROOT="/path/to/swiftvm"
export LD_LIBRARY_PATH="/guest/lib:/guest/usr/lib:$LD_LIBRARY_PATH"

# 运行 guest 程序
$SWIFTVM_ROOT/ld.so ./my_guest_program
```

### 2. 自动 host 库使用

以下系统库会自动使用 host 架构版本：
- libc.so（标准C库）
- libpthread.so（线程支持）
- libm.so（数学库）
- libdl.so（动态加载）
- librt.so（实时扩展）
- ld-linux.so（动态链接器）

### 3. 测试配置

```bash
# 运行测试套件
chmod +x test_guest_support.sh
./test_guest_support.sh
```

## 详细配置

### 库路径配置

```bash
# 标准配置
export LD_LIBRARY_PATH="/guest/lib:/guest/usr/lib:/usr/local/guest/lib"

# 调试配置（启用详细输出）
export LD_DEBUG="libs,symbols,bindings"

# 性能配置（禁用某些检查）
export LD_BIND_NOW=1  # 立即绑定所有符号
```

### 预加载库

```bash
# 加载 guest 特定的拦截库
export LD_PRELOAD="guest_interceptor.so:guest_compat.so"

# 用于性能分析
export LD_PRELOAD="guest_profiler.so"
```

### 架构特定配置

#### x86_64 Guest on x86_64 Host
```bash
# 最优性能配置
export SWIFTVM_BRIDGE_MODE="direct"
export SWIFTVM_HOST_LIBS="/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu"
```

#### ARM64 Guest on x86_64 Host
```bash
# 需要桥接配置
export SWIFTVM_BRIDGE_MODE="full"
export SWIFTVM_GUEST_LIBS="/guest/lib/aarch64-linux-gnu"
export SWIFTVM_HOST_LIBS="/lib/x86_64-linux-gnu"
```

#### RISC-V Guest on x86_64 Host
```bash
# 完整仿真配置
export SWIFTVM_BRIDGE_MODE="emulation"
export SWIFTVM_GUEST_LIBS="/guest/lib/riscv64-linux-gnu"
```

## 性能优化

### 符号缓存

```bash
# 启用符号缓存（推荐）
export SWIFTVM_SYMBOL_CACHE=1
export SWIFTVM_CACHE_SIZE=10000

# 预热缓存
export SWIFTVM_PRELOAD_SYMBOLS="libc,libm,libpthread"
```

### 桥接优化

```bash
# JIT 桥接编译（如果支持）
export SWIFTVM_JIT_BRIDGES=1

# 桥接缓存大小
export SWIFTVM_BRIDGE_CACHE_SIZE=1000

# 直接调用优化
export SWIFTVM_OPTIMIZE_DIRECT_CALLS=1
```

### 内存管理

```bash
# 堆栈大小配置
export SWIFTVM_STACK_SIZE=8388608  # 8MB

# 共享内存优化
export SWIFTVM_SHARED_MEMORY=1

# 内存映射优化
export SWIFTVM_MMAP_THRESHOLD=131072  # 128KB
```

## 调试配置

### 基本调试

```bash
# 启用所有调试输出
export LD_DEBUG="all"

# 只显示库加载
export LD_DEBUG="libs"

# 只显示符号解析
export LD_DEBUG="symbols"

# 只显示重定位
export LD_DEBUG="reloc"
```

### SwiftVM 特定调试

```bash
# 桥接调试
export SWIFTVM_DEBUG_BRIDGE=1

# 符号解析调试
export SWIFTVM_DEBUG_SYMBOLS=1

# 库加载调试
export SWIFTVM_DEBUG_LOADING=1

# 性能统计
export SWIFTVM_PERFORMANCE_STATS=1
```

### 日志配置

```bash
# 日志级别
export SWIFTVM_LOG_LEVEL="debug"  # error, warn, info, debug

# 日志文件
export SWIFTVM_LOG_FILE="/tmp/swiftvm-guest.log"

# 日志格式
export SWIFTVM_LOG_FORMAT="timestamp,level,component,message"
```

## 故障排除

### 常见问题

#### 1. 符号未找到错误
```bash
# 检查库路径
LD_DEBUG=libs ./my_program

# 验证符号存在
objdump -T /path/to/library.so | grep symbol_name

# 检查架构匹配
file /path/to/library.so
```

#### 2. 桥接失败
```bash
# 检查桥接支持
export SWIFTVM_DEBUG_BRIDGE=1
export SWIFTVM_BRIDGE_MODE="debug"

# 验证架构兼容性
uname -m  # 检查 host 架构
```

#### 3. 性能问题
```bash
# 启用性能分析
export SWIFTVM_PERFORMANCE_STATS=1

# 检查桥接开销
export SWIFTVM_BRIDGE_PROFILING=1

# 优化符号缓存
export SWIFTVM_SYMBOL_CACHE=1
```

#### 4. 内存问题
```bash
# 检查内存使用
export SWIFTVM_MEMORY_DEBUG=1

# 减少缓存大小
export SWIFTVM_CACHE_SIZE=1000

# 启用内存压缩
export SWIFTVM_MEMORY_COMPRESS=1
```

### 诊断工具

#### 检查依赖
```bash
# 使用 SwiftVM ldd
$SWIFTVM_ROOT/ld.so --list ./program

# 检查缺失符号
$SWIFTVM_ROOT/ld.so --verify ./program
```

#### 性能分析
```bash
# 符号解析统计
export SWIFTVM_SYMBOL_STATS=1

# 桥接统计
export SWIFTVM_BRIDGE_STATS=1

# 内存统计
export SWIFTVM_MEMORY_STATS=1
```

## 示例配置文件

### ~/.swiftvmrc
```bash
# SwiftVM Guest 程序默认配置

# 基本设置
export SWIFTVM_ROOT="/opt/swiftvm"
export SWIFTVM_BRIDGE_MODE="auto"

# 库路径
export LD_LIBRARY_PATH="/guest/lib:/guest/usr/lib:$LD_LIBRARY_PATH"

# 性能优化
export SWIFTVM_SYMBOL_CACHE=1
export SWIFTVM_JIT_BRIDGES=1
export SWIFTVM_OPTIMIZE_DIRECT_CALLS=1

# 调试（可选，仅在需要时启用）
# export SWIFTVM_DEBUG_SYMBOLS=1
# export LD_DEBUG="libs"

# 架构特定设置
case "$(uname -m)" in
    x86_64)
        export SWIFTVM_HOST_LIBS="/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu"
        ;;
    aarch64)
        export SWIFTVM_HOST_LIBS="/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu"
        ;;
esac

# 函数：快速启动 guest 程序
run_guest() {
    $SWIFTVM_ROOT/ld.so "$@"
}

# 函数：调试 guest 程序
debug_guest() {
    LD_DEBUG=all $SWIFTVM_ROOT/ld.so "$@"
}
```

### 使用示例
```bash
# 加载配置
source ~/.swiftvmrc

# 运行程序
run_guest ./my_program

# 调试程序
debug_guest ./my_program

# 检查依赖
$SWIFTVM_ROOT/ld.so --list ./my_program
```

## 高级功能

### 多架构支持

```bash
# 为不同架构设置不同的库路径
export SWIFTVM_X86_64_LIBS="/guest/lib/x86_64"
export SWIFTVM_ARM64_LIBS="/guest/lib/aarch64"
export SWIFTVM_RISCV64_LIBS="/guest/lib/riscv64"

# 自动架构检测
export SWIFTVM_AUTO_ARCH_DETECT=1
```

### 符号拦截

```bash
# 拦截特定符号
export SWIFTVM_INTERCEPT_SYMBOLS="malloc,free,printf"

# 拦截库
export SWIFTVM_INTERCEPT_LIBS="libc.so,libpthread.so"

# 自定义拦截器
export LD_PRELOAD="custom_interceptor.so"
```

### 安全配置

```bash
# 禁用某些不安全的功能
export SWIFTVM_DISABLE_EXEC=1  # 禁用可执行内存分配
export SWIFTVM_DISABLE_PTRACE=1  # 禁用 ptrace
export SWIFTVM_SANDBOX_MODE=1  # 启用沙箱模式

# 限制库加载路径
export SWIFTVM_SECURE_LIBRARY_PATH="/usr/lib:/lib"
```

这些配置提供了 SwiftVM guest 程序支持的完整配置选项。根据具体需求选择合适的配置组合。
