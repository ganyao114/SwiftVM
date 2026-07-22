# SwiftVM 动态链接器与 Guest 程序支持

## 概述

SwiftVM 动态链接器是一个高级的动态链接系统，支持在 host 架构上运行 guest 架构的可执行程序。通过智能的符号解析和架构桥接机制，SwiftVM 可以：

- 区分 host 和 guest 架构的符号实现
- 自动为 guest 代码创建调用 host 函数的桥接代码
- 确保关键系统库（如 libc）使用 host 架构版本以获得最佳性能
- 支持多种架构组合（x86_64, ARM64, RISC-V）

## 核心特性

### 1. 符号解析策略

符号解析遵循以下优先级：
1. **Host 架构符号优先**：优先使用 host 库中的符号
2. **自动桥接创建**：如果找到 host 符号，自动为 guest 代码创建桥接
3. **Guest 符号回退**：如果没有 host 实现，直接使用 guest 库符号

### 2. 关键系统库强制 Host 版本

以下系统库始终使用 host 架构版本：
- `libc.so` - C 标准库
- `libpthread.so` - POSIX 线程库
- `libm.so` - 数学库
- `libdl.so` - 动态加载库
- `librt.so` - 实时扩展库
- `ld-linux.so` - 动态链接器

### 3. 多架构桥接支持

支持的架构组合：
- x86_64 guest → x86_64 host（直接调用）
- ARM64 guest → x86_64 host（桥接）
- RISC-V guest → x86_64 host（桥接）
- x86_64 guest → ARM64 host（桥接）
- 其他组合...

## 文件结构

```
source/translator/linux/linker/
├── dynlink.c                    # 核心动态链接器（修改自 musl libc）
├── linker_bridge.h/.cpp         # C 接口层，用于 C/C++ 桥接
├── linker.h/.cpp                # C++ 实现，管理 shadow libraries
├── CMakeLists.txt               # 构建配置
├── linker_config.h.in           # 配置头文件模板
│
├── ARCHITECTURE.md              # 架构设计文档
├── GUEST_PROGRAM_SUPPORT.md     # Guest 程序支持详细说明
├── CONFIGURATION.md             # 配置指南
├── README.md                    # 本文件
│
└── test_guest_support.sh        # 测试脚本
```

## 快速开始

### 编译

```bash
cd /path/to/SwiftVM
mkdir build && cd build
cmake .. -DBUILD_STANDALONE_LINKER=ON
make swiftvm-ld
```

### 运行 Guest 程序

```bash
# 基本用法
./swiftvm-ld /path/to/guest_program

# 查看依赖
./swiftvm-ld --list /path/to/guest_program

# 启用调试
LD_DEBUG=all ./swiftvm-ld /path/to/guest_program
```

### 环境配置

```bash
# 设置 guest 库路径
export LD_LIBRARY_PATH="/guest/lib:/guest/usr/lib"

# 加载配置
source ~/.swiftvmrc
```

## 主要组件

### 1. 核心数据结构

#### `ShadowSymbol` (linker.h)
```cpp
struct ShadowSymbol {
    void *bridge;       // guest 架构的跳板函数地址
    void *target;       // host 架构的目标函数地址
    Elf64_Sym sym;      // 符号信息
    bool has_host_impl; // 是否有 host 架构实现
};
```

#### `ShadowLib` (linker.h)
- 管理一个共享库的 host 和 guest 版本
- 维护符号缓存和跳板映射
- 提供架构无关的接口

### 2. 桥接生成器

#### `BridgeGenerator` 接口 (linker.h)
- 抽象的桥接函数生成器接口
- 支持多架构扩展（X86, X86_64, ARM64, RISC-V）
- 负责生成和管理桥接函数的生命周期

### 3. C 接口层

#### `linker_bridge.h/cpp`
- 提供 C 兼容的接口供动态链接器使用
- 错误处理和异常安全
- 线程安全的库管理

## 工作原理

### 符号解析流程

```
程序启动
    ↓
加载主程序和依赖库
    ↓
对每个未定义的符号：
    ↓
查找 host 库中的符号 (swift_linux_has_host_symbol)
    ↓
找到？─→ 是 ─→ 创建桥接函数 (swift_linux_create_bridge)
    │                        ↓
    │                  返回桥接函数地址
    │
    └─→ 否 ─→ 在 guest 库中查找符号
                        ↓
                  返回 guest 符号地址或错误
```

### 桥接机制

```c
// Guest 代码调用 printf
call printf_guest_stub

// printf_guest_stub 是生成的桥接代码
printf_guest_stub:
    // 1. 保存 guest 寄存器状态
    // 2. 转换调用约定（如果需要）
    // 3. 调用 host printf
    call printf@host
    // 4. 转换返回值
    // 5. 恢复 guest 寄存器
    ret
```

### 库加载策略

```c
加载库请求（例如 "libc.so"）
    ↓
检查是否为关键系统库？
    ↓
是 ─→ 强制加载 host 版本
    │     ↓
    │   swift_linux_load_so("libc.so")
    │     ↓
    │   创建 ShadowLib 对象
    │     ↓
    │   所有符号自动桥接
    │
否 ─→ 正常加载 guest 版本
        ↓
      符号按需桥接
```

## 关键 API

### C 接口 (linker_bridge.h)

```c
// 加载 host 架构的共享库
void* swift_linux_load_so(const char* so_name);

// 检查 host 库中是否有指定符号
bool swift_linux_has_host_symbol(void* handle, const char* sym_name);

// 为 guest 代码创建调用 host 符号的桥接
void* swift_linux_create_bridge(void* handle, const char* sym_name);

// 卸载 host 库
void swift_linux_unload_so(void* handle);
```

### C++ 接口 (linker.h)

```cpp
// Shadow Library 管理
class ShadowLib {
public:
    ShadowLib(const std::string& name);
    
    bool hasSymbol(const std::string& name);
    void* createBridge(const std::string& name);
    void* getSymbolAddress(const std::string& name);
};

// 桥接代码生成器接口
class BridgeGenerator {
public:
    virtual ~BridgeGenerator() = default;
    virtual void* generateBridge(void* hostFunc, const std::string& name) = 0;
};
```

## 配置选项

### 编译时选项

```cmake
# 构建独立链接器
-DBUILD_STANDALONE_LINKER=ON

# 启用测试
-DBUILD_LINKER_TESTS=ON

# 调试模式
-DSWIFTVM_DEBUG_MODE=ON
```

### 运行时环境变量

```bash
# 库路径
export LD_LIBRARY_PATH="/guest/lib:/usr/lib"

# 调试输出
export LD_DEBUG="libs,symbols,bindings"

# SwiftVM 特定选项
export SWIFTVM_BRIDGE_MODE="auto"
export SWIFTVM_SYMBOL_CACHE=1
export SWIFTVM_DEBUG_SYMBOLS=1
```

## 性能考虑

### 优化策略

1. **符号缓存**：缓存已解析的符号，避免重复查找
2. **JIT 桥接**：运行时编译桥接代码以获得最佳性能
3. **直接调用优化**：相同架构时避免桥接开销
4. **预加载关键库**：提前加载常用 host 库

### 性能测试

```bash
# 运行性能测试套件
./test_guest_support.sh

# 启用性能统计
export SWIFTVM_PERFORMANCE_STATS=1
./swiftvm-ld ./benchmark_program
```

## 调试指南

### 常见问题

**问题：符号未找到**
```bash
# 检查库路径
LD_DEBUG=libs ./swiftvm-ld ./program

# 验证符号存在
objdump -T /path/to/library.so | grep symbol_name
```

**问题：桥接失败**
```bash
# 启用桥接调试
export SWIFTVM_DEBUG_BRIDGE=1
./swiftvm-ld ./program
```

**问题：性能慢**
```bash
# 启用符号缓存
export SWIFTVM_SYMBOL_CACHE=1

# 检查桥接统计
export SWIFTVM_BRIDGE_STATS=1
```

### 调试工具

```bash
# 查看程序依赖
./swiftvm-ld --list ./program

# 验证符号解析
./swiftvm-ld --verify ./program

# 详细跟踪
LD_DEBUG=all ./swiftvm-ld ./program 2>&1 | less
```

## 测试

### 运行测试套件

```bash
# 基本功能测试
./test_guest_support.sh

# 性能测试
./test_guest_support.sh --performance

# 压力测试
./test_guest_support.sh --stress
```

### 编写自定义测试

```c
// test_custom.c
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main() {
    printf("Testing SwiftVM guest support\n");
    printf("sqrt(16) = %f\n", sqrt(16.0));
    
    void* ptr = malloc(100);
    printf("malloc succeeded: %p\n", ptr);
    free(ptr);
    
    return 0;
}
```

编译并测试：
```bash
gcc -o test_custom test_custom.c -lm
./swiftvm-ld ./test_custom
```

## 扩展开发

### 添加新架构支持

1. 在 `linker.cpp` 中添加新的 `BridgeGenerator` 实现：
```cpp
class MIPS64BridgeGenerator : public BridgeGenerator {
    void* generateBridge(void* hostFunc, const std::string& name) override {
        // 生成 MIPS64 桥接代码
    }
};
```

2. 在 `createBridgeGenerator()` 中注册：
```cpp
#ifdef SWIFTVM_HOST_MIPS64
    return std::make_unique<MIPS64BridgeGenerator>();
#endif
```

### 自定义符号拦截

```c
// 在 find_sym2() 中添加自定义逻辑
if (strcmp(s, "custom_function") == 0) {
    // 特殊处理
    return custom_implementation;
}
```

## 文档

- [架构设计文档](ARCHITECTURE.md) - 详细的系统架构说明
- [Guest 程序支持](GUEST_PROGRAM_SUPPORT.md) - Guest 程序加载详解
- [配置指南](CONFIGURATION.md) - 完整的配置选项说明

## 贡献

欢迎贡献代码！主要需求：

1. **架构特定桥接**：实现实际的机器码生成（当前为占位符）
2. **性能优化**：JIT 编译、缓存改进
3. **测试用例**：更多的测试场景
4. **文档改进**：使用示例、最佳实践

## 限制和注意事项

1. 当前桥接生成器是占位符实现，需要实现真实的机器码生成
2. 需要架构特定的调用约定处理
3. 某些复杂函数可能需要特殊处理
4. 内存管理需要考虑跨架构指针

## 许可证

遵循 SwiftVM 项目的许可证。
