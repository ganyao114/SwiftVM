# LoongArch64（LA664 / LBT / UAL / LSX）后端综合优化实施结果

日期：2026-11-14。实施基于 `dev` 分支提交 `f49f6fd8`，在 **Loongson-3A6000（LA664）** 真机上完成 `S0`—`S5` 全栈落地、硬件语义校准、全量回归测试与跨客体（`x86_64` / `AArch64` / `ARM32`）消融基准验收。参考前置文档：[LoongArch LBT 优化方案](loongarch-lbt-optimization-plan-2026-11-11.md)，原始测量证据归档：[ablation-summary.json](evidence/loongarch64-lbt-la664-20261114/ablation-summary.json)。

## 0. 核心结论摘要

- **端到端综合应用提速 1.97x–6.68x，专项微基准最高提速 11.66x**：相较冻结基线 `f49f6fd8`，全栈开启（`FullStack`）后，`x86_64` 综合负载提速 **5.29x**，`AArch64` 综合负载提速 **6.68x**，`ARM32` 综合负载提速 **1.97x**；非对齐 TSO 访存 + `CRC32C` 微基准提速 **11.66x**，128 位整数 SIMD + `POPCNT` 提速 **3.38x**，多基本块 ALU Flags + `ADC/SBB` 提速 **2.62x**。
- **发射原生代码体积缩减 35.8%–74.3%，后端编译耗时最高缩短 2.01x**：依托硬件 `LBT4.eflags`、`UAL` + `disp12` 寻址折叠、`CRCC.W.*.W`、`LSX` 单指令向量化与 PC 相对直接分支，`x86_64_composite_app` 原生代码从 `28,500 B` 降至 `9,624 B`（**-66.2%**，后端发码从 `0.268 ms` 降至 `0.140 ms`），`aarch64_composite_app` 从 `32,292 B` 降至 `8,648 B`（**-73.2%**，后端发码从 `0.307 ms` 降至 `0.153 ms`）。
- **无扩展回退路径同步受益（+8.0%–+49.7%）**：即便通过 `SVM_LA64_FEATURES=off` 禁用全部 `LBT/UAL/CRC32/LSX` 扩展，仅靠 PC 相对直接跳转、LA664 定向 `DBAR`、`ALSL.D` 移位寻址、单指令位域与当拍死亡输入寄存器复用，基础 ISA 路径亦获得 **1.08x–1.50x** 提速与 **10.5%–23.7%** 代码体积缩减。
- **真机实测发现并闭合两项关键硬件/后端隐患**：
  1. 定位 **Loongson-3A6000 `x86sbc.[b|h|w|d]` 的 `AF` 硬件硅片怪癖（Silicon Quirk）**，用 3 条标量指令完成零分支精确补偿；
  2. 修复 `Writer::Address` 在大偏移（超出 `disp12`）且 `destination == base` 时覆写基址寄存器的寄存器别名缺陷，并补齐单测回归。

---

## 1. 分阶段落地概览（S0—S5）

| 阶段 | 核心落地内容 | 关键文件 |
| --- | --- | --- |
| **S0** | 建立冻结基线 `f49f6fd8`；修复跨后端测试 `-latomic` 链接、128 位访存故障报告宽度 `access_bytes = bits / 8`（`16 B`）与 `pinned_uniform_slots` 契约 | [CMakeLists.txt](../CMakeLists.txt)、[emit_memory.cpp](../source/runtime/backend/loongarch64/emit_memory.cpp)、[runtime_engine_test.cpp](../source/tests/rebuild/runtime_engine_test.cpp) |
| **S1** | `AT_HWCAP` + `CPUCFG2` 双门控探测、`SVM_LA64_FEATURES` 策略门控；全套 LBT / CRC32C / LA664 汇编器编码（逐字对齐 GNU Binutils 2.44）；有效扩展位接入 `CodeImageKey::isa_features` 隔离缓存与 AOT | [host_features.h](../source/runtime/backend/loongarch64/host_features.h)、[assembler.h](../source/runtime/backend/loongarch64/assembler.h)、[compile.h](../source/runtime/backend/loongarch64/compile.h)、[runtime.cpp](../source/runtime/backend/loongarch64/runtime.cpp) |
| **S2** | `LBT4.eflags + carry_xor + lbt_dirty_mask` 全链路硬件标志引擎（算术、`Inc/Dec`、`Adc/Sbb`、逻辑、移位/循环移位、16 条件 `setx86j`、无分支标量 `CondSelect`）；3A6000 `x86sbc` `AF` 硬件怪癖修正；`ucontext_t` `lbt_context` 信号同步故障恢复 | [emit_flags.cpp](../source/runtime/backend/loongarch64/emit_flags.cpp)、[emit_scalar.cpp](../source/runtime/backend/loongarch64/emit_scalar.cpp)、[emit.cpp](../source/runtime/backend/loongarch64/emit.cpp)、[recovery.cpp](../source/runtime/backend/loongarch64/recovery.cpp)、[loongarch64_host.cpp](../source/runtime/platform/linux/loongarch64_host.cpp) |
| **S3** | `UAL` 硬件非对齐直访 + `disp12` 偏移折叠；LA664 定向 `dbar`（`0x14`/`0x12`/`0x15`/`0x1a`/`0x10`）与单侧 TSO 屏障；PC 相对 `B`/`BEQZ`/`BNEZ` 直接分支与 fallthrough 消除；`ALSL.D`、`BSTRPICK`/`EXT.W`、`ROTR.[B|H|W|D]` 与硬件 `CRCC.W.[B|H|W|D].W` | [emit_memory.cpp](../source/runtime/backend/loongarch64/emit_memory.cpp)、[emit_control.cpp](../source/runtime/backend/loongarch64/emit_control.cpp)、[emit_operand.cpp](../source/runtime/backend/loongarch64/emit_operand.cpp)、[entry_elf.S](../source/runtime/backend/loongarch64/entry_elf.S) |
| **S4** | 完整 LSX 128 位整数向量快路径（算术/饱和/比较、逐位逻辑、扩宽/窄化打包、字节混洗 `vshuf.b`/`vshuf4i.b`、`vmuh`/`vmaddwev`、`vabsd`、`vbsll.v`/`vbsrl.v`、`vmskltz.b`、`vpcnt.d`、`vld`/`vst` 溢出快路径、无分支 `V128 Select`）；`allocate.cpp` 当拍死亡输入寄存器与寄存器对安全复用 | [assembler.h](../source/runtime/backend/loongarch64/assembler.h)、[emit_vector.cpp](../source/runtime/backend/loongarch64/emit_vector.cpp)、[allocate.cpp](../source/runtime/backend/loongarch64/allocate.cpp) |
| **S5** | 修复 `Writer::Address` 大偏移 `destination == base` 覆写缺陷并补齐单测；完成 6 负载 × 5 配置真机消融基准与证据归档 | [emit_operand.cpp](../source/runtime/backend/loongarch64/emit_operand.cpp)、[loongarch64_compile_test.cpp](../source/tests/rebuild/loongarch64_compile_test.cpp)、[README.md](../source/runtime/backend/loongarch64/README.md) |

---

## 2. 真机硬件发现与关键工程细节

### 2.1 Loongson-3A6000（LA664）`x86sbc.[b|h|w|d]` 的 `AF` 硅片怪癖

在 S2 全位宽穷举差分测试中，3A6000 硬件实测暴露出一个未在公开手册注明的微架构缺陷：
- **触发条件**：执行 `x86sbc.b/h/w/d` 且输入 `CF_in == 1`、同时低 4 位半字节相等 `(lhs & 0xf) == (rhs & 0xf)` 时，x86 `SBB` 架构规范要求 `(lhs & 0xf) - (rhs & 0xf) - 1 < 0` 产生借位 `AF = 1`，但 3A6000 硬件的 4 位 `AF` 比较器漏计了 `-CF_in`，错误写入 `AF = 0`（其余 `CF/PF/ZF/SF/OF` 五个标志及 `x86adc.*` 的全部六个标志均完全精确）。
- **零分支补偿方案**：由于二进制减法恒等式 `(a - b - CF_in) ^ a ^ b` 的 bit 4 严格等于第 3 位向第 4 位的半字节借位（即精确的架构 `AF`），在计算差值 `res = a - b - CF_in` 后，仅用 3 条单周期指令（`XOR tmp, res, a` → `XOR tmp, tmp, b` → `X86MTFLAG tmp, 0x04`）即可精确覆盖 `LBT4.AF`，不污染其余标志且无需分支。

### 2.2 `LBT4.eflags` 惰性同步与 `ucontext_t` 信号异常恢复

- **热路径单源事实（Single Source of Truth）**：开启 `lbt_x86` 后，算术/逻辑/移位指令直接在硬件 `LBT4.eflags` 中维护 `{CF, PF, AF, ZF, SF, OF}`，并通过轻量元数据 `carry_xor`（适配 ARM 反极性进位 `C = ~borrow`，消除了热路径进位取反指令）与 `lbt_dirty_mask` 追踪脏位；`r31` packed flags 仅在跨基本块退出、C helper 调用与显式快照点按需通过 `x86mfflag` 导出。
- **同步故障精确恢复**：当访存指令触发 `SIGSEGV`/`SIGBUS` 进入信号处理器时，硬件标志尚驻留在被中断上下文的 `LBT4` 中。`loongarch64_host.cpp` 遍历 Linux LoongArch64 `ucontext_t::uc_mcontext.__extcontext` 扩展帧链表，定位 `LBT_CTX_MAGIC (0x42540001)` 的 `lbt_context::eflags`，与 `r31` 合并还原精确异常现场，避免热路径每条访存前提前写回 `r31` 的高昂开销。

### 2.3 LSX 操作数顺序陷阱与寄存器分配原地复用

- **LSX 反直觉操作数顺序校准**：与标量指令或 x86 SSE 相反，LoongArch LSX 中：
  - `vandn.v vd, vj, vk` 计算 `(~vj) & vk`（标量 `andn rd, rj, rk` 计算 `rj & ~rk`）；
  - `vilvl.* / vilvh.* / vpickev.* / vpickod.*` 将 **`vk` 置于偶/低通道，`vj` 置于奇/高通道**；
  - `vssrani.*.0 / vsrani.* / vssran.*` 将 **`vj` 打包至低 64 位、原 `vd` 打包至高 64 位**。
  上述指令均在 3A6000 上通过逐字节模式向量原生差分单测校准。
- **SSA 末次输入寄存器原地复用与别名防护**：将 `allocate.cpp` 死亡释放条件从 `< incoming.definition_position` 收紧至 `<=`，允许目标 SSA 直接复用当拍最后一次读取的输入 GPR / V128 寄存器对；同时排查并修复了三处写前读隐患：`GetOperand` 的立即数中间加载、窄位宽 `BitInsert` 的高低半字拷贝，以及 `Writer::Address` 在 `offset` 超出 `[-2048, +2047]` 且 `destination == base` 时 `LI` 提前破坏 `base` 的缺陷。

### 2.4 16 KiB 宿主页大小与客体 ELF 段对齐

Loongnix 25 内核页大小为 **16 KiB（`16384 B`）**。若跨架构客体静态 ELF 仅按默认的 4 KiB（`0x1000`）页对齐链接，`.text` 与紧随其后的 `.data/.bss` 会落入同一个 16 KiB 宿主物理页内，导致对 `.bss` 全局数组的正常写入持续命中代码页写保护（SMC 陷阱风暴）。测试客体统一使用 `-Wl,-z,max-page-size=0x10000`（64 KiB 段对齐）彻底隔离代码页与数据页。

---

## 3. 测试环境与正确性验收

### 3.1 真机测试平台

| 项目 | 配置详情 |
| --- | --- |
| 处理器 | Loongson-3A6000（LA664 微架构，4 核 8 线程），锁定 `2500.00 MHz`（`performance` 调频器，`taskset -c 0`） |
| 操作系统与内核 | Loongnix GNU/Linux 25，`Linux 6.6.52-loong64 loongarch64`，页大小 `16384 B`（16 KiB） |
| 硬件扩展特性 | `cpucfg lam ual fpu lsx lasx crc32 lvz lbt_x86 lbt_arm lbt_mips` |
| 宿主构建工具链 | `g++ 14.2.0`（Release `-O3`）、GNU Binutils `2.44`、CMake `3.31.6`、Ninja `1.12.1` |
| 客体交叉工具链 | Clang / LLD `19.1.7`（产出独立静态 `x86_64`、`aarch64`、`arm32` ELF 负载） |

### 3.2 正确性与回归矩阵

| 验证套件 | 覆盖范围与配置 | 结果 |
| --- | --- | --- |
| `svm_loongarch64_compile_test --compile-only` | 汇编器逐字编码（对照 Binutils 2.44）、`HostFeatures` 门控、`CodeImageKey` 隔离、S2/S3/S4/S5 代码形态断言 | **21/21 用例通过**（213 项断言） |
| `svm_loongarch64_compile_test --native` | 3A6000 原生差分执行：全位宽算术/`ADC`/`SBB`（含 `AF` 边界）、16 种 `setx86j`、信号 `lbt_context` 故障恢复、`UAL`+`TSO`+大偏移寻址、`CRC32C`、全套 LSX 128 位向量与 `vpcnt.d` | **20/20 用例通过**（214 项断言） |
| 全量 `ctest`（硬件默认全开） | 全部 38 个 CTest 目标（覆盖 `x86_64`、`ARM64`、`ARM32` 前端/中间端/后端/运行时/AOT/异常恢复） | **38/38 通过，0 失败** |
| 全量 `ctest`（`SVM_LA64_FEATURES=off`） | 强制关闭 `lbt_x86`、`ual`、`crc32`、`lsx`，验证纯基础 LA64 软件回退路径完整性 | **38/38 通过，0 失败** |
| S5 跨架构基准校验 | 6 个独立客体程序 × 5 种特性组合 × 6 次运行（共 180 次执行），逐次比对 64 位标准输出摘要与退出码 | **180/180 摘要逐位完全一致** |

---

## 4. 真机消融基准结果（Loongson-3A6000 @ 2.50 GHz）

### 4.1 消融档位定义

统一采用 `--scope function`，每组执行 1 次预热 + 5 次独立进程计时，取进程总耗时中位数（`median_ms`）：

1. **`Baseline (f49f6fd8)`**：未修改的 `dev` 冻结基线 Release 二进制；
2. **`BaseArch (features=off)`**：当前代码库运行于 `SVM_LA64_FEATURES=off`（仅启用基础 LA64 架构改进：PC 相对直接分支、定向 `dbar`、`ALSL.D`、单指令位域/旋转、寄存器原地复用）；
3. **`+LBT`**：`SVM_LA64_FEATURES=off,lbt`（在 `BaseArch` 基础上单独启用 `LBT4.eflags` 硬件标志引擎）；
4. **`+LBT+UAL+CRC32`**：`SVM_LA64_FEATURES=off,lbt,ual,crc32`（叠加硬件非对齐访存、`disp12` 折叠与硬件 `CRCC.W.*.W`）；
5. **`FullStack (all)`**：`SVM_LA64_FEATURES=all`（全栈启用 `LBT + UAL + CRC32C + LSX`）。

### 4.2 执行耗时与端到端加速比（中位数 ms，越小越快）

| 客体负载 | ISA | Baseline ms | BaseArch (`off`) | `+LBT` | `+LBT+UAL+CRC32` | `FullStack (all)` | 全栈提速比 |
| --- | :---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `x86_64_alu_flags_cfg`（25M 轮多分支 + 256-bit `ADC/SBB` + `SETcc`） | `x86_64` | 2819.93 | 1925.52 (`1.46x`) | 1116.34 (`2.53x`) | 1075.53 (`2.62x`) | **1075.51** | **2.622x** |
| `x86_64_unaligned_crc32c`（12M 轮奇偏移 TSO 访存 + `CRC32Q/L/W`） | `x86_64` | 5105.84 | 3981.06 (`1.28x`) | 3935.01 (`1.30x`) | 437.84 (`11.66x`) | **437.77** | **11.663x** |
| `x86_64_simd128_popcnt`（4M 轮 `paddsb/pshufb/pmaddwd/psadbw/pmovmskb` + `popcnt`） | `x86_64` | 873.78 | 583.52 (`1.50x`) | 537.92 (`1.62x`) | 546.48 (`1.60x`) | **258.23** | **3.384x** |
| `x86_64_composite_app`（5M 轮指针追逐 CFG + 非对齐 TSO + `CRC32Q` + `ADC` + SIMD） | `x86_64` | 1777.38 | 1246.61 (`1.43x`) | 1172.18 (`1.52x`) | 642.66 (`2.77x`) | **336.20** | **5.287x** |
| `aarch64_composite_app`（5M 轮 `CRC32CX` + `ADDS/ADC/SUBS/SBC` + 非对齐 + NEON `TBL/SQADD`） | `aarch64` | 2004.75 | 1855.44 (`1.08x`) | 1794.56 (`1.12x`) | 704.94 (`2.84x`) | **300.05** | **6.681x** |
| `arm32_composite_app`（12M 轮 `ADDS/ADC/SUBS/SBC` CPSR + 条件跳转 + 非对齐 + 旋转） | `arm32` | 1202.91 | 892.40 (`1.35x`) | 665.09 (`1.81x`) | 609.35 (`1.97x`) | **609.44** | **1.974x** |

**分项归因观察**：
- **基础架构层（`BaseArch` vs `Baseline`）**：PC 相对直接分支消除间接寄存器跳转、单侧精确 `dbar 0x14/0x12` 替代全屏障 `dbar 0`、`ALSL.D` 寻址融合与末次输入寄存器原地复用，直接带来 **1.08x–1.50x** 的普适加速。
- **LBT 标志引擎（`+LBT` vs `BaseArch`）**：在密集标志/带进位链负载 `x86_64_alu_flags_cfg` 上耗时从 `1925.52 ms` 骤降至 `1116.34 ms`（在 `BaseArch` 之上再提速 **1.72x**），同时依托 `carry_xor` 反极性进位适配，`arm32_composite_app` 的 CPSR `ADDS/ADC/SUBS/SBC` 链亦从 `892.40 ms` 降至 `665.09 ms`（再提速 **1.34x**）。
- **UAL + 硬件 CRC32C（`+LBT+UAL+CRC32` vs `+LBT`）**：消除了逐字节拼装非对齐读写与 C ABI `CRC32C` helper 调用，`x86_64_unaligned_crc32c` 耗时从 `3935.01 ms` 降至 `437.84 ms`（单层跃升 **8.99x**），`x86_64` 与 `aarch64` 综合负载分别再跃升 **1.82x** 与 **2.55x**。
- **LSX 128 位向量化 + `vpcnt.d`（`FullStack` vs `+LBT+UAL+CRC32`）**：将原本展开为数十条标量通道循环的 SSE/NEON 指令收敛为 1–3 条 LSX 硬件向量指令，`x86_64_simd128_popcnt` 从 `546.48 ms` 降至 `258.23 ms`（单层提速 **2.12x**），`x86_64_composite_app` 从 `642.66 ms` 降至 `336.20 ms`（单层提速 **1.91x**），`aarch64_composite_app` 从 `704.94 ms` 降至 `300.05 ms`（单层提速 **2.35x**）。

### 4.3 原生代码体积与后端编译耗时对比

| 客体负载 | Baseline 代码量 | BaseArch (`off`) | `+LBT` | `+LBT+UAL+CRC32` | `FullStack` 代码量 | 代码缩减幅度 | Backend 编译耗时（基线 → 全栈） |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| `x86_64_alu_flags_cfg` | 14,876 B | 13,316 B | 11,040 B | 8,712 B | **8,712 B** | **-41.4%** (`0.586x`) | `0.177 ms` → `0.130 ms` (**1.36x**) |
| `x86_64_unaligned_crc32c` | 25,028 B | 21,600 B | 20,652 B | 12,840 B | **6,440 B** | **-74.3%** (`0.257x`) | `0.215 ms` → `0.103 ms` (**2.09x**) |
| `x86_64_simd128_popcnt` | 15,696 B | 13,676 B | 12,524 B | 10,784 B | **6,652 B** | **-57.6%** (`0.424x`) | `0.170 ms` → `0.104 ms` (**1.63x**) |
| `x86_64_composite_app` | 28,500 B | 24,408 B | 23,276 B | 17,672 B | **9,624 B** | **-66.2%** (`0.338x`) | `0.268 ms` → `0.140 ms` (**1.91x**) |
| `aarch64_composite_app` | 32,292 B | 28,544 B | 27,748 B | 20,404 B | **8,648 B** | **-73.2%** (`0.268x`) | `0.307 ms` → `0.153 ms` (**2.01x**) |
| `arm32_composite_app` | 17,740 B | 13,540 B | 12,880 B | 11,388 B | **11,388 B** | **-35.8%** (`0.642x`) | `0.181 ms` → `0.153 ms` (**1.18x**) |

由于后端发射的机器指令字数量大幅下降（综合负载缩减约 2/3 至 3/4），不仅显著降低了 LA664 L1I 指令缓存压力，还同步将 JIT 后端代码生成耗时降低了 **18%–52%**（最高提速 **2.09x**）。

---

## 5. 后续可选演进方向

1. **`lbt_arm` 原生指令路径探索**：当前 `AArch64` / `ARM32` 客体已通过 `LBT4.eflags + carry_xor` 共享了 `lbt_x86` 的硬件进位借位与条件求值红利；后续可进一步评估 `armadd/armsub/armmfflag` 等 `lbt_arm` 专用指令在复杂 ARM 移位标志场景下的边际收益。
2. **浮点向量化与 LASX 256 位扩展**：本轮 `S4` 聚焦整数 128 位 SIMD 与 `VLD`/`VST` 通道；后续可将 `emit_vector.cpp` 中的 `F32x4` / `F64x2` 浮点向量运算按 IEEE-754 NaN/FZ 语义接入 LSX `vfadd/vfsub/vfmul/vfmadd`，并在未来引入 256 位 IR 类型时平滑扩展至 LASX（`xr0–xr31`）。
3. **x87 硬件栈顶（`LBT4.ftop`）评估**：若后续存在重度 32 位 x87 浮点负载，可进一步实测 `x86incftop/x86decftop` 相比当前编译期栈偏移折叠的实际收益。
