# SwiftVM 项目架构文档

## 概述

SwiftVM 是一个多 ISA 动态二进制翻译与 JIT 编译框架，使用 C++20 编写。它能够将 ARM64/x86 guest 指令翻译为 host 原生代码执行，同时支持自定义语言 Slang 的编译执行。

### 核心能力

- 多 ISA 前端解码（ARM64、x86、Slang）
- 自定义中间表示（IR）及优化 Pass 管线
- 多后端 JIT 代码生成（ARM64、RISC-V64）+ 解释器 fallback
- Linux 动态链接器实现（用于 guest 程序加载）
- 跨平台支持（macOS、Linux、Windows、Android）

---

## 整体架构

```
┌─────────────────────────────────────────────────────┐
│                    Source Input                       │
│         (ARM64 Binary / x86 Binary / Slang)          │
└──────────────────────┬──────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────┐
│                    Frontend                          │
│   ┌──────────┐  ┌──────────┐  ┌──────────────────┐  │
│   │  ARM64   │  │   x86    │  │  Slang Compiler  │  │
│   │ Decoder  │  │ Decoder  │  │  (ANTLR4-based)  │  │
│   └────┬─────┘  └────┬─────┘  └────────┬─────────┘  │
└────────┼─────────────┼─────────────────┼────────────┘
         ▼             ▼                 ▼
┌─────────────────────────────────────────────────────┐
│                  IR (中间表示)                        │
│   Block → Inst → OpCode + Args (Value/Imm/Operand)  │
│   Module → HIRFunction → Block*                      │
└──────────────────────┬──────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────┐
│               Optimization Passes                    │
│   CFG Analysis │ Dataflow │ Const Folding │ RegAlloc │
│   Dead Code Elimination │ Flag Elimination │ ...     │
└──────────────────────┬──────────────────────────────┘
                       ▼
┌─────────────────────────────────────────────────────┐
│                    Backend                           │
│   ┌──────────┐  ┌───────────┐  ┌──────────────┐    │
│   │  ARM64   │  │  RISC-V64 │  │  Interpreter │    │
│   │   JIT    │  │    JIT    │  │  (fallback)  │    │
│   │  (VIXL)  │  │ (biscuit) │  │              │    │
│   └────┬─────┘  └─────┬─────┘  └──────┬───────┘    │
└────────┼──────────────┼───────────────┼─────────────┘
         ▼              ▼               ▼
┌─────────────────────────────────────────────────────┐
│                Runtime Execution                     │
│   Code Cache │ Address Space │ Memory Management     │
└─────────────────────────────────────────────────────┘
```

---

## 目录结构

```
source/
├── base/                   # 基础库（日志、文件IO、工具函数）
├── externals/              # 外部依赖（ANTLR4、Catch2、fmt）
├── runtime/                # 核心运行时
│   ├── include/            # 公共 API（sruntime.h, config.h）
│   ├── common/             # 通用工具（类型、内存分配、哈希等）
│   ├── ir/                 # 中间表示
│   │   ├── ir.inc          # IR 指令定义（X-Macro）
│   │   ├── opcodes.h       # OpCode 枚举
│   │   ├── ir_types.h      # 值类型与参数类型
│   │   ├── instr.h/cpp     # 指令节点
│   │   ├── block.h/cpp     # 基本块
│   │   ├── function.h/cpp  # HIR 函数
│   │   ├── module.h/cpp    # 模块
│   │   ├── hir_builder.h   # IR 构建器
│   │   └── opts/           # 优化 Pass
│   ├── frontend/           # 前端解码器
│   │   ├── arm64/          # ARM64 指令 → IR
│   │   ├── x86/            # x86 指令 → IR
│   │   └── slang/          # Slang 语言 → IR
│   ├── backend/            # 后端代码生成
│   │   ├── arm64/jit/      # ARM64 JIT（基于 VIXL）
│   │   ├── riscv64/jit/    # RISC-V64 JIT（基于 biscuit）
│   │   ├── interp/         # 解释器
│   │   ├── code_cache.h    # JIT 代码缓存
│   │   ├── address_space.h # 地址空间管理
│   │   └── runtime.h       # 运行时状态
│   └── externals/          # 运行时外部库
│       ├── vixl/           # ARM64 汇编器
│       ├── biscuit/        # RISC-V 汇编器
│       ├── dlmalloc/       # 内存分配器
│       └── distorm/        # x86 反汇编器
├── compiler/               # 语言编译器
│   ├── slang/              # Slang 编译器（ANTLR4）
│   └── clang/              # Clang 支持
├── translator/             # 二进制翻译
│   ├── arm64/              # ARM64 翻译器
│   ├── x86/                # x86 翻译器
│   └── linux/              # Linux 支持
│       ├── linker/         # 动态链接器（ldso）
│       └── libs/           # Linux 库模拟
└── tests/                  # 测试
    └── fuzz/               # 模糊测试（Unicorn 对比验证）
```

---

## 核心模块详解

### 1. IR 系统 (`source/runtime/ir/`)

IR 是整个系统的核心，所有前端产出和后端消费都围绕它展开。

**值类型** (`ValueType`):
- 整数: `U8`, `U16`, `U32`, `U64`, `S8`, `S16`, `S32`, `S64`
- 向量: `V8`, `V16`, `V32`, `V64`, `V128`, `V256`

**参数类型** (`ArgType`):
- `Value` — IR 值引用
- `Imm` — 立即数
- `Uniform` — Uniform buffer 引用（类似 GPU uniform）
- `Local` — 局部变量
- `Cond` / `Flags` — 条件码与标志位
- `Operand` — 复合操作数（寄存器+偏移等）
- `Lambda` — 回调/地址引用
- `Params` — 参数列表

**指令分类** (定义于 `ir.inc`):
| 类别 | 指令示例 |
|------|---------|
| 内存读写 | `LoadMemory`, `StoreMemory`, `LoadMemoryTSO`, `CompareAndSwap` |
| 算术运算 | `Add`, `Sub`, `Mul`, `Div`, `Adc`, `Sbb` |
| 位运算 | `And`, `Or`, `Xor`, `Not`, `AndNot` |
| 移位 | `LslImm`, `LsrImm`, `AsrImm`, `RorImm` + Value 变体 |
| 位操作 | `BitExtract`, `BitInsert`, `BitClear`, `TestBit` |
| 标志位 | `GetFlags`, `SaveFlags`, `TestFlags`, `ClearFlags` |
| 控制流 | `Goto`, `Select`, `CondSelect`, `CallLambda`, `CallLocation` |
| 向量 | `Vec4Add`, `Vec4Sub`, `Vec4Mul`, `Vec4And`, `Vec4Or` |
| 辅助 | `LoadImm`, `AdvancePC`, `SetLocation`, `PushRSB`, `PopRSB` |

**层次结构**: `Module` → `HIRFunction` → `Block` → `Inst`

### 2. 前端 (`source/runtime/frontend/`)

将 guest 指令解码为 IR：
- **ARM64 Frontend**: 解码 AArch64 指令，生成对应 IR
- **x86 Frontend**: 解码 x86/x86_64 指令（使用 distorm 辅助）
- **Slang Frontend**: 编译 Slang 语言源码到 IR

### 3. 优化管线 (`source/runtime/ir/opts/`)

可配置的优化 Pass（通过 `Config::global_opts` 位掩码控制）：

| 优化选项 | 说明 |
|---------|------|
| `ReturnStackBuffer` | 返回栈缓冲优化 |
| `FunctionBaseCompile` | 函数级编译 |
| `MemoryToRegister` | 内存到寄存器提升 |
| `FlagElimination` | 标志位消除 |
| `ConstantFolding` | 常量折叠 |
| `StaticCode` | 静态代码优化 |
| `BlockLink` / `DirectBlockLink` | 基本块链接 |
| `ConstMemoryFolding` | 常量内存折叠 |
| `UniformElimination` | Uniform 消除 |
| `LocalElimination` | 局部变量消除 |
| `DeadCodeRemove` | 死代码消除 |

### 4. 后端 (`source/runtime/backend/`)

**ARM64 JIT** (`arm64/jit/`):
- `JitTranslator`: 遍历 IR Block/Function，为每条 IR 指令调用 `Emit*` 方法
- 使用 VIXL `MacroAssembler` 生成 ARM64 机器码
- 包含完整的 NZCV 标志位映射（guest x86 flags → host ARM64 NZCV）
- `PseudoFlags` 机制延迟标志位计算

**RISC-V64 JIT** (`riscv64/jit/`):
- 类似架构，使用 biscuit 库生成 RV64 指令

**解释器** (`interp/`):
- 逐条解释执行 IR 指令，作为 JIT 不可用时的 fallback

### 5. 运行时 (`source/runtime/include/`)

**核心 API** (`sruntime.h`):
```cpp
class Runtime {
    HaltReason Run();           // 持续执行直到中断
    HaltReason Step();          // 单步执行
    void SignalInterrupt();     // 发送中断信号
    void SetLocation(LocationDescriptor);  // 设置 PC
    backend::State* GetState(); // 获取执行状态
};
```

**中断原因** (`HaltReason`):
- `Step` — 单步完成
- `Signal` — 外部中断
- `PageFatal` — 页错误
- `CodeMiss` / `CacheMiss` / `ModuleMiss` — 代码未翻译
- `BlockLinkage` — 块链接请求
- `IllegalCode` — 非法指令
- `CallHost` — 调用宿主函数

**配置** (`Config`):
- `backend_isa`: 目标后端 ISA（ARM64/RV64 等）
- `enable_jit`: 是否启用 JIT
- `global_opts`: 优化选项位掩码
- `arm64_features`: ARM64 特性检测（AES、CRC32、Atomics 等）
- `page_table` / `memory_base`: 内存映射配置

### 6. 二进制翻译器 (`source/translator/`)

负责加载和翻译完整的 guest 二进制程序：
- Linux 动态链接器实现（`ldso/dynlink.c`）
- Guest 程序加载与重定位
- 系统调用翻译

---

## 构建系统

- **CMake 3.21+**，C++20 标准
- 架构自动检测（`DetectArchitecture.cmake`）
- 主要构建产物：`swift_base`、`swift_runtime` 静态库
- 外部依赖：Boost、VIXL、biscuit、fmt、ANTLR4、dlmalloc、distorm、Catch2

### 支持的 ISA 枚举

```cpp
enum ISA : uint8_t {
    kNone, kArm, kArm64, kX86, kX86_64, kRiscv32, kRiscv64, kLoongArch
};
```

---

## 数据流示例

以 x86 → ARM64 JIT 翻译为例：

1. **x86 Frontend** 读取 guest x86 指令流
2. 解码每条指令，调用 `HirBuilder` 生成 IR（如 `Add`, `SaveFlags`, `StoreUniform`）
3. IR 经过优化 Pass（常量折叠、标志位消除、死代码消除等）
4. **ARM64 JitTranslator** 遍历优化后的 IR Block
5. 每条 IR 指令调用对应的 `Emit*` 方法，通过 VIXL 生成 ARM64 机器码
6. 生成的代码存入 **Code Cache**
7. **Runtime::Run()** 执行缓存中的原生代码，遇到未翻译代码时返回 `CodeMiss`，触发新一轮翻译
