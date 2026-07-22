# SwiftVM Guest 程序启动支持

## 概述

SwiftVM 的动态链接器现在完全支持 guest 可执行程序的加载和执行，同时确保关键系统库（特别是 libc）使用 host 架构版本以获得最佳性能。

## 核心特性

### 1. Guest 程序启动流程

1. **阶段 1 (`_dlstart`)**: 基础自重定位
2. **阶段 2 (`__dls2`)**: 动态链接器初始化
   - 设置 ldso 使用 host libc
   - 解析 auxv 和程序头
3. **阶段 2b (`__dls2b`)**: TLS 设置
4. **阶段 3 (`__dls3`)**: 主程序加载和依赖解析
   - 初始化 guest 程序的 host 支持
   - 加载依赖库并设置跳板

### 2. Host 库优先级

#### 强制使用 Host 版本的库：
- `libc.so*` - C 标准库
- `libpthread.so*` - POSIX 线程库
- `libm.so*` - 数学库
- `libdl.so*` - 动态加载库
- `librt.so*` - 实时扩展库
- `ld-linux*` / `ld.so*` - 动态链接器

#### 检测和加载逻辑：
```c
static int is_critical_system_library(const char *name)
{
    // 检查库名称是否匹配关键系统库模式
    // 支持版本化和非版本化名称
}
```

### 3. 符号解析优先级

1. **Host 符号优先**: 对于关键系统库，优先查找 host 实现
2. **跳板创建**: 为 host 符号创建 guest 架构跳板
3. **Fallback**: 如果没有 host 实现，使用 guest 库符号

## 使用方式

### 启动 Guest 程序

```bash
# 直接执行 guest 程序
./guest_program args...

# 通过动态链接器执行
/path/to/swiftvm-ld.so ./guest_program args...

# 查看依赖关系
/path/to/swiftvm-ld.so --list ./guest_program
```

### 环境变量支持

```bash
# 设置库搜索路径
export LD_LIBRARY_PATH="/guest/lib:/guest/usr/lib"

# 预加载库
export LD_PRELOAD="guest_interceptor.so"

# 调试模式
export LD_DEBUG=all
```

## 调试和诊断

### 库加载跟踪

使用 `ldd` 模式查看库加载情况：

```bash
/path/to/swiftvm-ld.so --list ./guest_program
```

输出示例：
```
    libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x...) [host support available]
    libpthread.so.0 => /lib/x86_64-linux-gnu/libpthread.so.0 (0x...) [host support available]
    guest_specific.so => /guest/lib/guest_specific.so (0x...)
```

### 符号解析跟踪

在 `find_sym2` 中添加了详细的符号查找逻辑：

1. 检查是否有 host 实现
2. 创建跳板（如果需要）
3. Fallback 到 guest 符号

### 错误处理

- 关键库加载失败会产生警告但不会终止程序
- 符号未找到会产生详细的错误信息
- 跳板创建失败会有适当的错误报告

## 架构兼容性

### 当前支持

- **Host**: x86_64 Linux
- **Guest**: x86_64, x86, ARM64

### 跳板机制

```cpp
class BridgeGenerator {
public:
    virtual void* CreateBridge(const char* symbol_name, void* host_addr) = 0;
    virtual void DestroyBridge(void* bridge) = 0;
};
```

- X86_64 跳板生成器
- X86 跳板生成器  
- 可扩展到其他架构

## 性能优化

### 1. 符号缓存

- 每个库维护符号查找缓存
- 避免重复的 host 库查找
- 智能的跳板重用

### 2. 延迟加载

- 库按需加载
- 符号按需解析
- 跳板按需创建

### 3. 内存管理

- 高效的跳板内存分配
- 库卸载时的清理
- TLS 优化

## 限制和注意事项

### 1. 架构兼容性

- Host 和 Guest 必须是兼容的架构
- 调用约定必须匹配或可转换
- ABI 兼容性要求

### 2. 功能限制

- 某些底层系统调用可能需要特殊处理
- 信号处理需要架构感知
- 内存映射可能有限制

### 3. 调试限制

- 调试符号可能在跳板中丢失
- 性能分析需要考虑跳板开销
- 错误堆栈可能显示跳板地址

## 故障排除

### 常见问题

1. **库加载失败**
   ```
   Warning: Failed to load host version for critical library libfoo.so
   ```
   - 检查 host 系统是否安装了对应库
   - 验证库路径配置

2. **符号未找到**
   ```
   Symbol not found: some_function
   ```
   - 检查库依赖关系
   - 验证符号导出

3. **跳板创建失败**
   ```
   Exception creating bridge for symbol_name: details
   ```
   - 检查架构兼容性
   - 验证内存权限

### 调试技巧

1. 使用 `--list` 查看依赖
2. 设置详细错误报告
3. 检查 host 库可用性
4. 验证符号导出

## 未来改进

### 1. 性能优化

- JIT 跳板编译
- 内联跳板优化
- 批量符号解析

### 2. 功能扩展

- 更多架构支持
- 高级 ABI 转换
- 动态优化

### 3. 调试支持

- 更好的错误报告
- 调试符号保持
- 性能分析集成
