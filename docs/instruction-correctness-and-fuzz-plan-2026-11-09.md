# 指令正确性测试与 Fuzz 框架方案（2026-11-09）

状态：框架已落地并通过 host sanitizer fuzz 首轮验证；独立 oracle 的逐 form 能力矩阵、设备执行矩阵和完整 form 覆盖仍按第 16 节推进。代码基线为 `dev@867843d1096fd5338aa3a138ee0a42e6fdc23d03`。现有 ARM32 验证记录见[实现与验收文档](arm32-frontend-implementation-2026-11-09.md)。

### 已落地范围（2026-11-09）

- `source/tests/correctness/`：Case/Observation codec、catalog、generate、compare、reduce、portable Direct 与 compile-only 执行、worker CLI。
- `source/tests/fuzz/`：x86/ARM64/ARM32 decode、structured pipeline、raw byte pipeline 五个 libFuzzer 目标；pipeline 支持 `SVM_FUZZ_FORM` 固定单个 catalog form，并在固定模式下把任何 Direct/后端拒绝视为失败。
- `tools/correctness/`：local/ADB/system-QEMU controller 与 Unicorn oracle 入口。
- runtime 只增加 `SVM_TEST_BOUNDARY` 测试边界，生产默认路径不变。

已验证：`svm_correctness selftest` 560 例通过；RV64 汇编器满缓冲回跳断言已修复，最小 5 字节输入可重放，新增 2 个边界回归；host fuzz 累计约 1.27 亿次执行（decode 约 1.19 亿次，structured/raw pipeline 约 436 万次，`SVM_FUZZ_FORM` 两轮 536 万次）无崩溃。逐 form 验证：35 个 form 各 8 例的 Direct 执行与 4 后端 × Block/Function × Known/EarlyOff/Unknown × opt 共 13,440 次编译全部成功。语义差分：host Direct 对结构化 280 例为 272 verified + 8 gaps（均为 x86 SSE sticky flag 能力缺口）；红魔 ARM64 JIT 在记账前为 280/280 verified。从 pinned fuzz 语料抽样的 280 例，在 host Direct、红魔 ARM64 JIT、QEMU system x64/RV64/LA64 六条执行路径上均为 270 verified + 10 gaps、零失败；QEMU 三架构 `native_executed=280/280`、block scope、`interpreted_steps=0`。10 个 gap = 8 个 x86 SSE sticky flag 用例 + 2 个跨边界 store（partial commit/fault 粒度）。独立 oracle 的 C++ 适配、逐 form 覆盖统计、真实 x86 FP flag 复核尚未完成。

## 1. 目标与边界

目标是系统验证 **x86-64、AArch64、ARM32 三个前端，经 ARM64、x64、RV64、LoongArch64 四个后端生成的代码，是否符合 guest ISA 的可观察行为**。ARM32 分别覆盖 ARM、Thumb16、Thumb-2；当前 x86 前端的范围是 x86-64，不能将其统计成已提供 x86-32 支持。

测试对象分为五层，但共用一份输入和重放协议：

| 层次 | 要回答的问题 | 主要方法 |
| --- | --- | --- |
| 编码与合法性 | 是否识别正确的 form、长度、模式、非法或截断输入？有没有误拒绝合法指令？ | 独立编码种子、有限字段穷举、raw-byte fuzz、独立 decoder 对照 |
| 指令语义 | 所有有定义的寄存器、flags、FP 状态、内存及异常是否正确？ | 单指令边界用例与独立 oracle 差分 |
| 编译流水线 | 提前优化、IR opt、选指令、分配与发码是否保持语义？ | 同 Case 的 Direct/JIT、优化开关、target 模式和分配 scope 对照 |
| 运行时 | pin、跨块链接、dispatcher、恢复、SMC、cache/AOT、执行模式切换是否正确？ | 有界序列、CFG 和有明确动作的运行时场景 |
| 并发与内存模型 | 原子性、exclusive、屏障和访存顺序是否满足 guest 模型？ | 无竞争语义测试，加独立的并发 litmus，分别验收 |

“所有指令”采用可审核的有限验收范围：**已声明支持的 encoding form × 必测边界类别 × 适用执行配置**。不能穷举全部 64/128 位状态，也不能用随机轮数代替覆盖率。合法但尚未实现、参考引擎不能比较、当前设备不能执行的格子必须保留并显示原因。

框架只进入测试目标。继续使用现有前端、IR、Direct sink、优化器、后端与 VM；不新增生产 IR、Program 解释器、正确性 pass、逐指令快照或常驻验证钩子。生产 compile 的遍历次数、提交形态和默认执行路径不因测试框架改变。

## 2. 当前代码可以复用什么，缺什么

| 现有部分 | 可复用内容 | 当前证据的限制 |
| --- | --- | --- |
| [frontend/build_result.h](../source/runtime/frontend/build_result.h) | 区分非法、截断、Unsupported、IRRejected、FormationRejected | 不能将这些拒绝统一过滤为“不测” |
| [frontend/direct.h](../source/runtime/frontend/direct.h) | Direct 的完成、服务、fault、模式变化及后继信息 | Direct 与 IR sink 共用前端语义，不是独立 oracle |
| `frontend/{x86,arm64,arm32}/build_ir.*` | Block/Function 构建和 target-aware 提交 | 构建通过不代表后端执行正确 |
| [ir/optimize.h](../source/runtime/ir/optimize.h) | 现有消费 `Program&&` 的优化接口 | 每种配置从同一 Case 重建，不增加 Program clone |
| [execution/host_backend.h](../source/runtime/execution/host_backend.h) | 四后端的编译、发布、重定位、FP 和恢复绑定 | 需要记录实际绑定能力，而非仅记录请求配置 |
| [tests/support/native_code.cpp](../source/tests/support/native_code.cpp) | 同 host ISA 的裸机器码微测试 | 没有完整 VM、FaultScope、dispatcher、pin ABI 与失效所有权，不能承接通用 native fuzz |
| [execution/vm.h](../source/runtime/execution/vm.h) | 真实 GuestMemory、CodeHeap、运行、异常、SMC、持久化 owner | 需要很薄的测试边界适配；不另写一套执行器 |
| `tests/rebuild/*coverage*`、ARM32/A64 等已有用例 | 固定回归、边界输入、部分真实后端执行 | 大量 expected 来自 Direct；并非所有 guest × backend × form 的独立验证 |

现有计数不能直接转换成指令正确性覆盖率：

- x86 `opcodes.inc` 是 mnemonic vocabulary，不能代表每种 opcode、宽度、前缀和 reg/mem form。
- A64 `instruction_families.inc` 是 visitor family，一个 family 仍包含不同 opcode、宽度、flags 和 shift 子类。
- ARM32 当前有 788 个 pattern rows、759 个去重 handler 入口；其中仍有 cond、S、寄存器重叠、移位、IT、Profile 等维度。这些数字不等于逐指令验证数量。
- 当前 `--compile-only` 与 native 不可用返回码 77 必须沿用为独立状态；不能计入执行通过。

默认 ARM32 支持范围继续是 ARMv7-A 小端用户态 Profile；历史版本门控、特权/银行寄存器、MMU、Jazelle、大端切换及浮点 trap 不自动纳入“已支持”。x86 已实现扩展路径与 CPUID 对外承诺也要分开登记：当前保守 CPUID 未宣告 AVX 等能力，不应因此跳过已存在的 VEX/AVX 组件语义测试，也不能将其通过解释成完整虚拟 CPU 已对外宣告该特性。

## 3. 从 Dynarmic 借鉴什么，修正什么

参考固定为 [`dynarmic-interp@5a76f6930935bc07a2aa5158f3ee515c34ca14fd`](https://github.com/ganyao114/dynarmic-interp/tree/5a76f6930935bc07a2aa5158f3ee515c34ca14fd)。该 fork 同时保留 native JIT 和模板解释器；`UserConfig::interpreter` 默认关闭，默认 A32 fuzz 没有将其打开。因此其默认比较是 JIT 与 Unicorn，不能把整个 fork 或其 fuzz 一概描述为模板解释器比较。

| 参考行为 | 本方案的取舍 |
| --- | --- |
| 按 decode bit pattern 固定位，随机其余字段；对 Thumb 宽度与 IT 专门处理 | 复用模板思路，增加独立合法性规则、独立汇编种子及 form 清单，防止漏掉整个编码族 |
| 单指令、5 条、100 条序列；FP 特殊值偏置 | 保留分层，再加入真实 CFG、分配压力和运行时场景 |
| 先用自身 translator 决定是否测试；拒绝 Interpret、部分终结和异常路径 | 不照搬。合法性由测试合同决定，DUT 误拒绝必须暴露 |
| 显式跳过部分 exclusive、异常、SMC 和存在 Unicorn 缺陷的数值指令 | 移到独立 campaign 或 oracle 能力缺口表，不从总体分母消失 |
| A32 比较屏蔽 T/E，FPSCR 只看部分高位；A64 FPSR 主要检查 QC | 按 ISA 合同比较全部有定义的状态，包括累计异常和模式 |
| 用 DUT 的 T 位调整参考 PC；PC 不同可额外推进 JIT 尝试追齐 | 禁止。停止边界、PC、T/IT 本身就是被测结果，不用 DUT 修理 oracle |
| 最终内存 byte map；非法访存可懒映射 | 普通 RAM 可比较最终字节；权限、fault、部分提交、顺序和 SMC 必须使用真实映射及专项合同 |
| generator/reader 输出并重放具体状态 | 保留完整 Case，再补自动 reducer、稳定种子派生和批次泄漏复现 |

主要源码依据：[`fuzz_util.cpp`](https://github.com/ganyao114/dynarmic-interp/blob/5a76f6930935bc07a2aa5158f3ee515c34ca14fd/tests/fuzz_util.cpp)、[`A32/fuzz_arm.cpp`](https://github.com/ganyao114/dynarmic-interp/blob/5a76f6930935bc07a2aa5158f3ee515c34ca14fd/tests/A32/fuzz_arm.cpp)、[`A64/fuzz_with_unicorn.cpp`](https://github.com/ganyao114/dynarmic-interp/blob/5a76f6930935bc07a2aa5158f3ee515c34ca14fd/tests/A64/fuzz_with_unicorn.cpp)、[`test_generator.cpp`](https://github.com/ganyao114/dynarmic-interp/blob/5a76f6930935bc07a2aa5158f3ee515c34ca14fd/tests/test_generator.cpp)、[`test_reader.cpp`](https://github.com/ganyao114/dynarmic-interp/blob/5a76f6930935bc07a2aa5158f3ee515c34ca14fd/tests/test_reader.cpp)。

该实现主要是结构化随机差分，不能直接当作已经提供 coverage-guided fuzz 和自动 shrink 的框架。

## 4. 总体框架：一份 Case，多种执行结果

```text
独立 form 清单 / 定值样例 / 结构化生成 / fuzz mutation
                         |
                  不可变 Case (.svmcase)
                         |
               具体地址布局 + RunConfig
                         |
       +-----------------+------------------+
       |                 |                  |
   独立 oracle       现有 Direct         真实 JIT / VM
       |                 |                  |
       +-----------------+------------------+
                         |
                 架构 Observation
                         |
              版本化 ComparisonContract
                         |
          coverage / mismatch / 完整重放与缩减
```

只有三个长期维护的领域对象：`Case`、`Observation`、`ComparisonContract`。`RunConfig` 是一次执行的配置，catalog 是静态规则与覆盖清单。不要再设计一串 CaseInputs → Preparation → Actions → Certificates，或为每个后端复制一份 expected 转换流程。

Direct/JIT 的对照提供定位能力；**独立 oracle 才为前端语义提供外部约束**。两类证据分别统计。选指令和分配仍处理真实 Program；测试只在已有冷入口组织执行与读取结果，不参与每条 IR 的生产提交。

### 4.1 Case：唯一可移植输入

采用版本化、显式小端字段的二进制 `.svmcase`，作为 CLI、worker、fuzzer、reducer 与回归 corpus 的共同格式。JSON 只作查看、manifest 和结果输出，不维护第二份输入真相；不直接序列化 C++ GuestState、指针、padding 或 enum ordinal。

| 字段 | 内容 |
| --- | --- |
| identity | schema 版本、Case ID、稳定 form ID、测试合同 ID/版本、生成器版本、master/case seed |
| guest | ISA、具体 Profile/虚拟 CPU 模型、ARM/Thumb 模式、端序及需要的扩展 |
| code | 实际机器码 bytes、代码区域、入口物理 PC、初始执行模式；编码 recipe 仅是生成时辅助 |
| state | 全部参与架构比较的初始 GPR、flags、向量、FP 控制/状态及 ISA 专属状态 |
| memory | region、实际初始字节、权限、别名关系、保护区、页面粒度与地址约束 |
| relocations | 显式的地址引用及修补位置，区分代码、初始寄存器和内存中的指针 |
| boundary | 终止地址或有限出口集合、服务/fault 合同、允许的终结类型、指令/REP/场景动作预算 |
| scenario | 仅运行时场景需要的有界动作：运行、改代码、改变映射、再次运行等 |

`CaseHash` 标识规范化输入；`InstanceHash` 还包含具体化的地址布局。oracle 缓存至少按 InstanceHash、Profile、停止规则、oracle binary/version 和合同版本索引。只按 code bytes 或 seed 缓存 expected 会漏掉地址、flags、FP 模式及版本差异。

`RunConfig` 单独保存 backend、实际 host features、分配 scope、opt、TargetMode、pin/ABI、engine、链接与持久化策略。它不修改 Case 的合法性或比较标准。fuzzer 不能通过篡改比较 mask、支持声明或“忽略字段”让失败消失。

固定随机算法和 `master_seed + case_id → case_seed` 的派生规则；不要依赖线程调度、全局 PRNG 消费顺序或 STL distribution 的跨版本行为。无论 seed 是否稳定，每次失败仍保存实际 Case。

### 4.2 Observation：原始结果与架构投影

每条执行路径输出相同逻辑结构：

- 结果分类与精确终止边界，物理 PC、完整模式，以及 service 的原指令位置和后继位置。
- 架构寄存器、flags、FP 状态、最终可观察内存及 canary；异常时保存合同要求的部分完成状态。
- exception kind、access kind/width、架构 fault address、guest PC；host signal/PC、native offset、Native/Semantic origin 作为诊断字段。
- 实际执行证据：请求/实际 scope、真实 guest source blocks 与 IR blocks 分别计数、成功发布的 native unit、fallback、interpreted steps、cache/AOT 命中、实际特征与 pin 配置。
- source revision、构建配置、DUT/oracle binary SHA256、worker OS/CPU/页大小、执行阶段与耗时。

不能 `memcmp(GuestState)`。投影时 x86 `packed_flags` 经现有 `ToRflags()` 还原；ARM32 完整 Location key 分解为物理 PC、T/IT/E/FPSCR 模式，另检查内部 key 与架构状态是否一致；x87 只比较 80 位有效载荷、tag/TOP/control/status，不比较槽内 padding。service packet、numeric scratch、reservation 实现字段等不作为普通架构输出。

### 4.3 ComparisonContract：由测试独立定义

合同由已审核 form 规则与输入条件选择，不能从 DUT 的 IR metadata、优化结果或 oracle 差异反向生成。合同至少规定：合法性、定义位/保留位、允许的架构结果集合、停止边界、异常与部分提交规则、近似数值误差规则，以及所需 oracle 能力。

明确区分：定义行为、架构 undefined、UNPREDICTABLE、implementation-defined、合法但未实现、超出当前支持 Profile。不能统一包装成“允许不同”。

普通 FP 按位比较，包括有定义的 NaN 传播、sNaN/qNaN、符号零和累计异常；只有 ISA 明确允许近似或多种合法输出的 form 才使用专属集合/误差合同。不能全局忽略 NaN payload、FP 异常或低几位。

## 5. 覆盖清单：从 encoding form 开始

### 5.1 稳定标识与两种支持视图

Form ID 使用稳定文本或显式 ID，不使用 enum ordinal。例如：

```text
x86_64/add/op01/rm32-r32/legacy
x86_64/add/op03/r32-rm32/legacy
a64/addsub_shifted/adds/w32
a64/addsub_shifted/subs/x64
a32/arm/adc_regshift/S1
a32/thumb16/add_reg_t1
```

每个 form 登记：encoding、字段约束、模式/feature、读写与 flags 合同、memory/fault 属性、必测 case 类别、oracle 能力及当前前后端支持情况。不同 cond/别名/边界值作为 case 轴；改变语义或编码规则的子类必须拆分 form。

清单保留两种视图：

1. **组件已实现面**：前端/后端实际存在的实现路径，包括未在外部 CPU 模型宣告的扩展。
2. **对外支持面**：当前 Profile/CPU 模型承诺接受的行为、功能位及明确拒绝规则。

两者不能互相替代。现有 A64 也不应凭 family 列表推定完整 ISA Profile；需要把已实现和未实现子类逐项落表。已知范围外的 ISA form 可登记为后续缺口，但不能伪造已通过记录。

### 5.2 清单来源与完备性检查

从 x86 opcode/VEX 描述、extended decoder 分支、A64 handler 子类、ARM32 原始 pattern/fields/Profile 提取候选，维护薄的人工补充规则与独立种子。生成报告检查：

- 已有 handler/pattern 是否关联 form，是否存在没有任何用例的实现入口。
- 清单中的合法 form 是否能被 DUT 接受；意外 Unsupported 是结果，不是生成过滤条件。
- 独立汇编器/decoder 提供的合法 form 是否落在清单或明确范围外，防止 DUT 表中不存在的整个编码族永远不被生成。
- 每个 form 的必测轴、适用后端和参考能力是否明确，不能用 mnemonic/family “碰到过一次”代替。

测试 inventory 可以复用生产表的编码事实；合法性、定义位和 expected 不应完全由同一份生产表推导。独立 decoder 只能证明长度/分类等编码事实，不是执行语义 oracle；复用同一个 VIXL decoder 也不是独立证据。

## 6. 用例生成：确定性边界优先，随机补组合

生成器同时支持严格合法语义输入和非法/异常输入，两者分开 campaign。不能把随机编码全部交给 DUT 后仅保留成功者。

### 6.1 通用边界轴

| 类别 | 必测输入 |
| --- | --- |
| 整数 | 0、1、-1、最小/最大有符号值、全 1、单 bit、交替 bit、进位链、溢出边界、高半乘法、除数 0 与商溢出 |
| flags | 所有小集合输入；carry/borrow；条件成立/不成立；部分更新与保留；后续分支/ADC 等依赖 |
| 移位/旋转 | 0、1、width-1、width、width+1、掩码边界和最大编码计数；立即数与寄存器计数分开 |
| 寄存器别名 | dst=src1/src2/base/index、所有操作数相同、特殊寄存器、部分寄存器与重叠向量 lane |
| SIMD | 每 lane 的独特非零初态、符号/饱和边界、lane 顺序、高半保留/清零、窄化/扩展、交叉 lane 与 shuffle |
| FP | ±0、最小/最大 subnormal、normal 边界、±Inf、qNaN/sNaN/payload、舍入边界、取消、overflow/underflow/inexact |
| 内存 | 对齐/不对齐、跨边界、权限、地址 wrap/非法地址、base/index 重叠、load/store 重叠及别名 |
| 控制流 | taken/untaken、直接/间接、call/return、前后跳、有界循环、不同模式入口和页末尾 |
| 编译压力 | flags/数值状态跨 helper、GPR/向量长 live range、CFG join、真实 spill/reload、固定寄存器冲突 |

未被指令写入的寄存器、lane、flags 和内存必须参与比较，初始值使用不同的非零图案，避免“都初始化为零”掩盖误清零。正常 RAM 通过最终字节、红区和 canary 检查越界写；这不等价于证明访问顺序或中间副作用正确。

小字段做有限穷举，例如 ARM cond/NZCV、8 位计数字段；Thumb16 的 65,536 个首半字可用于分类测试，识别为 Thumb-2 前缀时再补第二半字或预期截断，不能一律当作 T16。大空间采用边界集合、pairwise 和风险较高的三轴组合，随后结构化随机扩展。

### 6.2 各前端专项

| 前端 | 必须单列的风险轴 |
| --- | --- |
| x86-64 | legacy/REX/VEX、opcode 方向、8/16/32/64 位、AH 与 REX、32 位写入零扩展、ModRM/SIB/RIP-relative/addr32、立即数符号扩展、LOCK/REP、flags 定义位、MXCSR DAZ/FTZ/舍入与异常、x87 栈/TOP/tag/80 位、SIMD 上半保留或清零、FXSAVE/XSAVE 等已实现状态图像 form 的布局与 fault |
| A64 | W/X、SP/ZR 的 form 区别、extend/shift、NZCV 条件、pre/post-index 和 writeback 别名、literal/PC-relative、FPCR/FPSR、NEON lane/结构访存、exclusive/LSE 和 feature fallback |
| ARM32 | A32 cond 与 PC bias、S 与 PC/SP/LR 别名、RRX/寄存器 shift 边界、Thumb16/32 跨页、ITSTATE 推进及条件失败无副作用、ARM↔Thumb、S/D/Q 别名、Q/GE、FPSCR DN/FZ/AHP/舍入/异常、VMLA 与 VFMA、LDM/STM/双字/NEON 结构访存、位置键与 Profile 门控 |

FP 生成初始可采用约一半特殊 bit pattern、一半原始随机位，并单独组合控制状态；比例是调度参数，不是正确性条件。测试数值 expected 不能调用与 DUT 共用的 FP 数学叶子。

CPUID、时钟、随机数、TLS、系统状态和服务调用采用固定模型或受控输入合同。硬件 CPUID/时间戳的完整数值不应直接与虚拟 CPU 比较；对时钟仅能在明确合同下检查单调性等性质。没有实现的 feature admission gate 不应由测试凭空假设存在。

### 6.3 序列、CFG 与有定义的数据依赖

先做单指令，再做 2–8 条依赖序列，最后扩展至 32/128 条及有界 CFG；这些长度是初始配置，不复制参考实现的固定轮数作为目标。

序列生成要有意制造 carry/flags 链、同址读改写、部分寄存器依赖、FP 模式切换、跨块活跃值及 helper 前后状态。首版禁止读取上一步 undefined 输出，或插入明确重定义该状态的指令；仅在最终比较屏蔽 undefined flags 无法阻止它们通过 Jcc/ADC 影响后续结果。无需为此先造完整 ISA 污点解释器。

同一个 payload 保留真实 Block/Function 编译边界。主测试不逐指令插 trap、snapshot 或 trace，避免破坏 DCE、flags liveness、RA 和跨块优化。失败诊断可以另跑逐步轨迹，但原始失败必须能在原配置复现。

单指令用例负责让每种 form 的结果可观察；长序列末态相同不能证明其中被覆盖的每条中间结果都正确。生成器不得把“出现某条指令”直接计成该 form 已完成独立语义验证。

## 7. 执行与观察边界

### 7.1 复用真实执行路径

普通 native 用例由测试 worker 创建最小 guest image，经现有 `PrepareGuest`/GuestMemory、CompileService、CodeHeap、FaultScope 和 native entry 执行。代码 RX、数据 RW；显式 SMC 用例才使用对应可写代码策略。Direct 使用同一初始内存与现有 `InterpretOne/InterpretBlock`，不再构造一份 Program 来解释。

裸 `NativeCode::Run` 只用于不依赖 runtime 的纯叶子 IR/assembler 微测试。凡涉及 guest fault、pin、helper ABI、dispatcher、link、信号和持久化，必须使用对应真实 owner。

源边界必须对应真实映射：[GuestMemory::CodeAt](../source/runtime/execution/memory.cpp) 返回 RX region 的剩余可读字节，ELF 同页 padding/零填充也在其中。缩短 Case 逻辑长度或 ELF `p_filesz` 不能阻止形成器继续读取。

- P0/P1 的精确单指令/末态用例优先将 payload 实际末尾放在 RX region 页末，后接非 RX guard，terminal 为下一页首地址。先写入再保护，保持相邻区域权限不同，让现有 source window 自然结束；不包装或修改 `CodeAt` 来伪造长度。
- 控制流指令按预先声明的 taken/untaken/间接出口集合截停，比较原始目标、返回地址和模式，不能再执行一个 tail jump 覆盖这些输出。
- 普通序列可使用显式 tail B/JMP 导向 terminal，但它必须作为 Case 中真实执行的指令让 DUT/oracle 共同执行，并纳入 PC、IT 推进、条件和预算合同。不能统一称 trailer 对全部架构状态无影响，也不能用它代替单指令精确末态验证。
- 地址敏感、跨页、未结束 IT、截断/取指故障用例按自身布局和合同生成；无法满足具体布局的 worker 报能力缺口，不能随意移动 payload 或追加 trailer。

### 7.2 精确、无寄存器污染的停止合同

现有 `CallGuest` 按完整 PC key 判断返回；ARM32 payload 改 FPSCR/IT/T 后，不能假定物理停止地址相同就会匹配。`RunUntilGuestStop` 目前要求未缓存的 x86 HLT，也不能直接作为跨 ISA 通用入口。

新增 **测试专用的冷 `RunToBoundary` 适配**，复用上述执行循环：

1. 在 payload 可执行区域之外指定物理 terminal address，不把 terminal 编译、缓存或链接成 native guest unit，也不让 Function 形成器吞入 terminal。
2. 在已有冷 Resolve 和 Direct 返回循环的停止判断处，按测试合同检查物理地址；命中后先 flush/合并架构状态，再捕获完整 PC key、T/IT/FPSCR 等供比较。错误模式仍然是失败，不用于修正 oracle。
3. Direct 的块执行通过现有 ready-entry lookup 在 terminal 前返回，再由外层停止判断捕获；不能让 `InterpretBlock` 先尝试 fetch terminal，事后把 fault 改成正常终止。首次入口已经是 terminal 的情况在 Direct fragment 冷入口/外层先判断；包括 JitOnly 因数值策略进入解释、暂不经过 native Resolve 的情况。
4. 需要观察 SVC/syscall 等时，在已有 service exit 边界、Linux 服务真正执行之前捕获原指令 PC、后继模式及参数。测试 stop 不执行随机 host syscall。
5. 不修改普通指令语义，不在热路径增加 callback；适配仅编入独立测试 runtime 目标，生产 target 不包含测试分支和符号。适配代码按职责保留在测试目录。

测试状态在 image 准备完成后注入；入口、SP、FP 控制和完整 ARM32 key 均来自 Case。不要调用会重新选择模式或覆盖 SP 的便利接口后仍声称执行了原始输入。适配只为现有 VM 提供测试访问与冷边界策略，不复制 `Execute`、`Resolve`、异常分发或发布代码；确需条件编译的停止判断集中于已有检查点。

各架构 oracle 独立实现相同边界。Unicorn 通过经自测的执行/取指 hook 或有明确语义的计数接口停机；硬件 adapter 优先通过独立 probe 子进程与 kernel regset/ptrace 接口注入、捕获完整状态。对于 guard terminal，可将精确预定地址上的取指 fault 识别为 observer stop，按独立验证的 kernel 保存规则还原边界；不得将其他 fault 吞掉，也不得私自将 terminal 页改为 RX 插断点后仍声称同一 Instance。

先用固定自测证明寄存器、flags、FP、模式和停止语义没有被 probe 污染；包含 x86 RF、A32 IT 等 trap/fault 保存差异。归一规则来自独立架构/probe 合同，不读取 DUT 输出。不能依赖一个会占用 guest scratch 寄存器的 C 调用 trampoline，也不能假定单步/TF 对所有指令无副作用。不能精确还原的 form/状态不具有该 oracle 的确认资格。

terminal 不得覆盖 RAX、R7、flags 等被测输出。UD2/BRK/BKPT 等终止方式只有在对应模式与 IT 条件下验证不污染观察结果后才可用，不能先假定有一套通用 trap。oracle 无法准确停机时记 `oracle-inconclusive`，不额外推进 DUT 追齐，也不读取 DUT 状态调整参考模式。

原始截断/页末取指测试不追加 terminal bytes；否则本来不完整的编码可能被追加数据补成合法指令。该类用例按真实可读 span 或 guard page 观察拒绝/fault。

### 7.3 “一步”与预算

边界类型至少区分 `instruction_completed`、`before_service`、`fault`、`test_terminal`。x86 `InterpretOne` 对 REP 可以只执行一次 iteration；oracle 的 count=1 与它不必相同。需要分别记录和限制架构指令、REP iteration、场景动作，不能按 C++ 调用次数对齐。

普通有界 CFG 通过结构约束保证到达 terminal，外部 watchdog 防止错误代码死循环。实际 native 路径没有免费的精确 retired guest instruction 计数；首版不为计数增加逐指令 instrumentation。诊断单步预算和静态已知预算与真实计数分开记录。墙钟超时先归类 hang/timeout，经固定输入重放确认，不能直接宣称语义不一致。

## 8. 独立 oracle 与参考能力

| 参考来源 | 用途 | 限制与证据等级 |
| --- | --- | --- |
| 同 guest ISA 真机 probe | 高价值边界和争议样例复核 | 必须检测 feature、regset、异常和精确停止能力；只确认该 CPU/Profile 的行为 |
| Unicorn | x86-64/A64/A32 普通单指令和短序列高吞吐差分 | 基于 QEMU，有已知缺陷；异常细节、特殊状态与扩展能力按版本自测 |
| 独立 QEMU probe | 支持 Unicorn 缺口、运行时/故障或系统环境对照 | 与 Unicorn 同一家族，不能作为两票独立证据；锁定版本/CPU 模型 |
| Dynarmic native JIT | ARM/Thumb/A64 的补充对照，辅助定位模式和优化问题 | SVM 已参考/移植部分语义，存在共同来源；不能独立证明同源语义 |
| 独立规范定值/数学模型 | flags、边界、FP、地址及异常小集合 goldens | 不调用 SVM 同源实现；不一开始重写一个全 ISA 模拟器 |
| 旧版 SVM / 当前 Direct | 回归与流水线归因 | 共享代码/语义，不是最终独立裁判 |

[Unicorn 官方明确其基于 QEMU](https://www.unicorn-engine.org/)。报告记录 `oracle_family/version/profile/capabilities`，不采用“多数引擎一致所以正确”的投票规则。争议样例保留全部结果，转硬件或独立规范定值裁决；无法裁决则为 inconclusive。

每个 oracle adapter 启动前执行少量固定自测：寄存器/lane 布局、端序、flags、FP sticky 状态、PC/mode、terminal、映射/权限、故障前现场。能力缺失以明确的 form/状态/故障合同表示，不静默归零不支持的寄存器。

已确认的 oracle defect 必须有版本范围、最小复现、issue/依据和替代验证方式；不直接永久跳过整个指令族。只有已知不可靠的比较字段不能满足严格合同，则该 oracle 不具有该用例的确认资格，不能通过屏蔽差异计为 pass。

已记录的实现限制（2026-11-09）：

- Unicorn 2.1.4 不建模 x86 SSE 的 MXCSR sticky exception flags（IE/DE/ZE/OE/UE/PE，bits 0-5）。oracle 在 Observation 中输出 `fp_control_verified_mask=0xffffffc0`，比较器只在双方掩码内比较；控制位/RC/FZ/DAZ 仍验证，异常标志由 `x86_floating_test` 的规范定值覆盖，后续硬件 oracle 复核。
- Unicorn 2.1.4 的 ARM32 `TPIDRURW`（C13_C0_2）是 no-op。oracle 保留声明的输入值并输出 `arm32_tls_rw=unavailable`；任何写该寄存器的 form 会因保留输入而暴露为差异，不会静默通过。

红魔首先承担 ARM64 后端真实机器码执行和 A64 硬件 oracle；是否能运行 A32 原生 probe 需检测，不能因 CPU 为 ARM64 就假设支持 32 位 EL0。x86 真机 oracle 按可用硬件接入；没有真机的范围仍可由版本固定的模拟参考验证，但报告显式标记证据等级。

## 9. 内存、异常与部分提交

### 9.1 统一 GuestMemory 模型

每个 Case 具体化成真实 arena；Direct 的回调、native 的地址和 oracle 的 guest 映射使用相同初态与布局。不能将现有逻辑地址 0x8000 加 `std::array` fixture 直接当作 native 可解引用内存。

优先给 worker 与其 oracle 使用相同 guest VA。固定地址冲突记 infrastructure unavailable。允许重定位时只能修补 Case 中显式声明的引用，不能把所有“看起来落在 arena 的整数”自动当指针归一化。每个后端与自己的参考比较同一已具体化 Instance，再汇总逻辑 Case；地址敏感用例必须满足原地址约束。

记录 guest/host 页大小与权限粒度。通用跨 host 用例可以采用所有 worker 兼容的较粗边界，例如 64 KiB；指定 4 KiB 权限边界的用例只有在真正支持该粒度的环境运行才算覆盖。不能扩成大页后仍声称验证了 4 KiB 边界。

### 9.2 故障合同

沿用 `RuntimeOptions.exception_handler` 返回 Stop，在 Linux guest signal frame 修改 SP、栈和 PC 之前捕获现场，见 [execution/exception.cpp](../source/runtime/execution/exception.cpp)。host 自身崩溃、未归属 guest 的 fault 与架构 guest exception 分开处理。

比较架构 exception kind/vector、访问种类/宽度、物理 PC、fault address，以及该 form 有定义的已提交状态。不能只比较“都收到 SIGSEGV”，也不能统一将 native/helper/Direct fault 回滚到指令前。

fault address 的“访问首地址/真正故障字节”等口径按 ISA 与故障合同规定；参考不能提供时显示能力缺口，不能使用一个全局模糊地址 mask。

首批故障输入包括：未映射、只读、不可执行、页末取指、跨页 load/store、架构对齐约束、x86 非规范地址、除零/商溢出、非法编码，以及当前支持的状态写入拒绝。REP、LDM/STM、双字、NEON 结构访存单列 partial commit/restart，明确 fault 前哪些寄存器/内存允许或必须已改变。

跨区域/跨页的单个 store 先按以下合同处理：Direct 与 JIT 统一报告首个不可访问字节作为 fault address，并保持全有或全无提交；Unicorn 可能已提交映射前缀，并报告不同的 fault byte/width。比较器只对跨边界写屏蔽映射前缀的 partial commit 差异，并把 fault address/width 记为 gap；其他字节、异常 kind/vector 与寄存器仍严格比较。REP/LDM/STM/NEON 结构访存不使用该宽松规则。

最终内存相同不足以证明访问顺序。Direct/Unicorn access trace 可用于失败诊断；native 顺序由别名、保护页、故障、原子和 litmus 场景约束，不能为了日志在主 fuzz 中替换真实 load/store。[QEMU TCG plugin 文档](https://www.qemu.org/docs/master/devel/tcg-plugins.html)也明确：翻译回调不等于实际执行，memory callback 发生于成功访问之后，faulting access 不由该回调报告。

### 9.3 原子与并发的实际能力边界

当前 VM 拥有一个 guest thread，`GuestExecutionLock` 是进程级递归锁；两个 VM 的调用并行不等于 guest 指令并发。ARM32 exclusive 还受 private-anonymous/外部写者边界限制，见 [ARM32 实现边界](arm32-frontend-implementation-2026-11-09.md#实现范围与边界)。

先验收无竞争 RMW、exclusive load/store、宽度与地址、monitor 清除、失败 SC 不写内存、fault 和当前 128 位锁路径。对允许 spurious failure 的 store-exclusive 不要求第一次成功，采用有界重试与最终合法结果判断。

真正的内存顺序和原子性 litmus 使用单列的多 host 线程后端微核入口，避开全局 VM 串行锁，并保留真实后端访存/屏障路径；它证明的是编译映射，不等价于当前 runtime 已支持多 guest thread。对观测值判断是否属于 guest 模型允许集合，不要求弱序结果必须出现，也不能将宿主结果集合直接当 guest 允许集合。

这部分要有独立并发能力与 guest memory model 合同；当前无法提供该能力的格子明确列为缺口，不能凭单线程随机测试宣告完整原子/TSO 正确性。

## 10. 真实执行矩阵与覆盖记账

### 10.1 基础矩阵

| guest | ARM64 后端 | x64 后端 | RV64 后端 | LoongArch64 后端 |
| --- | --- | --- | --- | --- |
| x86-64 | 红魔 / ARM64 Linux | x64 Linux | RV64 Linux | LA64 Linux |
| A64 | 红魔 / ARM64 Linux | x64 Linux | RV64 Linux | LA64 Linux |
| A32 / T16 / T32 | 红魔 / ARM64 Linux | x64 Linux | RV64 Linux | LA64 Linux |

基础矩阵按 `form × 必测 case 类别 × 适用配置` 登记；配置部分至少包括：

```text
4 backend × 2 allocation scope × 2 optimize × 3 TargetMode
= 48 个基础 native 配置

allocation scope: Block / Function
TargetMode: Known / EarlyOff / Unknown
另加 Direct 与独立 oracle 结果
```

每个 mandatory form-case 类别要在全部适用基础配置中有实际用例；同一输入可覆盖多个类别。这不要求每个扩展随机样例全笛卡尔积。随机探索按覆盖缺口调度，并按周期重放完整回归矩阵；每条永久失败样例必须重放原失败配置。pin、feature、engine、link/cache 等额外轴使用重点组合及专项 campaign；不支持的组合明确说明原因。

**分配 scope 与形成范围分开记账。** Function scope 的单指令测试只证明 Function allocator 路径；不能证明形成了多个 guest 基本块。IR 内部条件/出口块也不能算作多个 guest source blocks。CFG 专项记录真实形成的非空 guest block PC/key 集合、IR block 数及实际 native unit，利用输出可观察的多路径 CFG 确认已形成并执行多块 region。RWX source policy 造成 Function 单块时，如实标记，普通多块用例使用 RX code/RW data。

测试通过冷 `HostBackendOps.compile` 包装读取已有 Program/CompileResult，记录实际 scope、IR blocks、frame/spill、特征等。guest source block 证据优先使用已有入口/源记录；不足时只在测试变体的现有 Function 形成返回边界读取已经生成的 DecodedBlock 摘要，不重新 formation/decode，不扫描全 IR 或增加 pass。无法证明多 guest block 的用例不能算完成多块格。统计开启与普通性能测量分开。

### 10.2 不允许“假 native 通过”

- 请求 Function 后回退 Block，架构结果可以正确，但 Function 那一格仍未覆盖。
- 请求 JIT 后出现 Direct/service policy fallback，不能计为目标 form 的 native 验证；记录 interpreted steps 与具体 fallback 原因。当前个别数值模式即使 JitOnly 也可能受策略解释，测试要暴露事实。
- 成功编译但没有执行 native 入口/抵达预期边界，只记 compiled。
- pin 测试必须匹配完整 CompileOptions、NativeRunContext 和 entry ABI，不能只改 pinned slot 数。首次覆盖 pin=0 与实际默认配置；ARM64 其他 bank 数仅在完整绑定可用时加入。
- 可选 LSE/RCpc/AFP、x64 特征、RV Zb/RVV、LA LSX 等只在实际环境支持时执行；已有 fallback 能力可通过真实配置禁用后测试，不能伪造 host capability。
- spill 压力用例需有实际 frame/spill 与跨调用/跨块活跃证据；名为 pressure 不等于真的触发了 spill。

### 10.3 覆盖事实与结果分类

分开记录 `catalogued / declared_supported / generated / decoded / built / compiled / native_executed / independently_verified`。每个 mandatory form-case-config 单元保留最后结果及证据引用，不把前一级自动升级到后一级。

结果至少包括：

| 结果 | 含义 |
| --- | --- |
| verified | 在同一 Instance、边界和有效合同下，目标路径实际运行，并被合格独立 oracle 确认 |
| pipeline_agreement | Direct/JIT 或不同配置一致，尚无独立参考确认 |
| semantic_mismatch / exception_mismatch | 有定义的架构输出或异常不同 |
| expected_rejection / expected_fault | 与预先规定的非法或异常合同一致；与正常语义 coverage 分栏 |
| unexpected_rejection | 当前支持面内的合法 form 被 decode/build/compile 拒绝 |
| policy_fallback | 请求路径未实际执行，记录真实替代路径 |
| oracle_unsupported / oracle_inconclusive | 参考能力不足或无法裁决，不计通过 |
| out_of_scope / architecturally_unconstrained | 有独立依据的范围外或无法严格差分输入，不能吞掉支持面内的缺口 |
| host_crash / sanitizer_failure / timeout | DUT 或 oracle 的阶段、失败类型分别保存 |
| infrastructure_unavailable / infrastructure_failure | 地址冲突、设备不可达、页粒度、二进制或 adapter 能力问题 |

报告给出固定分母、已验证数、pipeline-only 数、未执行/不支持/待裁决数和失败数。任何过滤器、oracle 缺陷或 timeout 都不能缩小已冻结的支持面分母。

## 11. Fuzz 引擎、隔离与失败归因

### 11.1 两条互补执行方式

**进程内 fast fuzz** 使用 libFuzzer 驱动 C++ decode/build/opt/compile-only/Direct：

- raw bytes target：每个 ISA 独立目标，覆盖任意字节、截断、长度、拒绝与解码安全性。
- structured pipeline target：读取同一 Case，使用结构化 mutator 保持多数输入合法，覆盖构建、优化、发码和 Direct；非法输入保留独立 campaign。
- ASan/UBSan 用于被 C++ 编译器插桩的代码；不得把 sanitizer 通过误认为 JIT 生成的访存也受到插桩保护。

**native differential fuzz** 由 controller 发送完整 Case 至隔离 worker，执行真实后端与 oracle，并按 form/操作数/CFG/feature 覆盖反馈调度。最初使用结构化生成和语义覆盖，不先造跨进程 C++ edge feedback 协议。

libFuzzer 支持 corpus、定制 mutation 和进程隔离模式，见[官方文档](https://llvm.org/docs/LibFuzzer.html)。SanitizerCoverage 是编译器插入的函数/basic-block/edge 回调，见[Clang 文档](https://clang.llvm.org/docs/SanitizerCoverage.html)。据此，手写 assembler 运行时生成的机器码不会自动获得这类 coverage；父进程仅 RPC 到普通 worker，也不会自动收到 worker 内的 C++ 覆盖。

若后续采用 libFuzzer fork 模式驱动 native 编译，必须让实际执行编译路径的子进程链接并运行插桩代码。C++ edge coverage、ISA form coverage 和后端实际执行 coverage 保持三份统计，不互相冒充。

测试不得破坏已有 helper ABI：对需要 general-regs-only/nosanitize 等约束的桥接代码沿用生产构建规则。sanitized C++ lane 与真实 production-ABI native lane 分开，避免 instrumentation 自身破坏 pinned/FP 寄存器造成假误编译。

### 11.2 结构化 mutation

mutation 共用 Case codec 与 form 合同，不为 libFuzzer 另建一套 testcase AST。优先变异：opcode 同族、宽度、寄存器别名、立即数边界、初始 flags/FP bit pattern、访存偏移/权限/别名、短序列依赖、CFG 边和循环次数。

opcode/宽度变异跨越 form 时，必须按独立 catalog 规则重新分类并验证 bytes 与 Form ID/合同匹配，不能继续沿用旧 ID 串记覆盖。合同重新选择仍受可信规则约束，不能由 fuzz 输入任意放宽。

合法语义 campaign 的变异后分类独立于 DUT；非法 encoding campaign 则有意破坏保留位、前缀、长度和 Profile。生成器生成不了合法输入属于 generator gap，不能把尝试次数记为已测试。

跨后端及 opt/target 差分是 metamorphic 辅助证据，不取代独立 oracle。首版不对任意指令做可能改变 undefined/FP/异常语义的“等价重写”。局部 IR/backend 微测试可复用现有 IR 和原语 Direct，无需增加完整 Program 解释器或独立 IR fuzz 语言。

### 11.3 worker、预算与归因

native 执行使用独立子进程、guard pages 和外部 watchdog。分别记录 decode/build/opt/compile/publish/execute/oracle 阶段；预算包含 bytes、IR size、内存、CFG、序列/REP 次数、墙钟与输出量。错误代码卡死时只终止自己启动的 worker/process group。

初版每例新建 VM、进程可有界复用，例如 64 例一批；批次结束重启 worker 进程，保存当前完整 batch journal，成功批次回收。每例执行前先提交 Case 和配置到 journal，再进入可能崩溃的代码，避免只留下 crash PC。

失败先在全新 worker 重放。若只在批次内出现，保留完整前缀 Case 与顺序，归类状态/资源泄漏并进行批次前缀缩减；不以 fresh replay 通过就丢为 flaky。warm cache/SMC 等有意跨执行状态则写在同一场景 Case 内。

按现有流水线进行差异定位：

| 现象 | 优先定位方向，不能替代最终证明 |
| --- | --- |
| Direct 和所有 JIT 以相同方式偏离独立 oracle | 共同前端语义、状态投影、输入/停止合同或 oracle 本身 |
| Direct 与 oracle 一致，四后端 JIT 均错 | IR 提交、共同优化/target 合同、共用数值叶子或测试 ABI |
| 只有某 backend 错 | 该后端选指令、约束、分配、发码、FP/访存映射或恢复 |
| 只在 opt-on / Known / Function / pin 错 | 对应优化、提前提交、CFG/分配或状态协议 |
| 只在 warm/link/cache/AOT/SMC 错 | 身份键、发布、重定位、失效、所有权或生命周期 |
| 只有某 oracle 不同 | 先保存争议，核对能力、合同与 probe，再用独立来源复核 |

## 12. 复现、缩减与回归

每个首个失败保存：原始 `.svmcase`、RunConfig、全部相关 Observation、版本/hash manifest、差异字段与原始结果分类。IR dump、guest/host disassembly、recovery metadata 和诊断 trace 按失败重放生成，正常用例不保存全量日志。

reducer 与首次 runner 使用同一执行/比较函数，failure predicate 固定为原 backend、Profile、scope/opt/target/pin、参考能力及原差异类别。不能把语义 mismatch 缩成另一个非法编码、不同 fault 或基础设施错误。

缩减顺序：

1. 运行时动作/批次前缀、CFG 节点和无关基本块；保持必要的 warm-up、模式切换和失效关系。
2. 序列做分块删除与单条删除，修复显式分支/地址引用，但不改变原始模式与观测合同。
3. 简化寄存器、立即数、别名图和地址偏移，保留触发所需宽度、Thumb16/32、IT 合法性和页边界。
4. 简化初始寄存器/FP bit pattern/内存及权限区域；保留定义性、原异常和部分提交条件。
5. 全新 worker 重放缩减结果，再用原配置验证。诊断单步模式通过不能取代原 Function/opt 配置的失败复现。

缩减只保证在预算内找到更小样例，不承诺全局最小。保留原始与缩减版及其关联；不覆盖唯一原始证据。去重依据 form、阶段、架构差异字段、backend/config 与缩减特征，不能只按 host crash PC 合并。

确认并修复后，将小型 `.svmcase` 放入永久 corpus；需要时补一个能清楚说明根因的 Catch 回归。回归重放走原失败后端与配置，并保留独立 expected 来源，避免修复后又退化成 Direct-only expected。

## 13. 运行时专项场景

场景仍使用 Case 内的有界动作，不引入第二套编译框架。

| 专项 | 场景与必须观察的事实 |
| --- | --- |
| Block/Function、链接与 dispatcher | 多块 RX 区域、taken/untaken、间接目标冲突、call/return、L1/L2 cold/warm、真实 native link 命中后状态、pin flush |
| Mixed 与热块晋升 | 同一入口先 Direct 后 JIT、热度按完整位置键、跨路径状态/异常一致；默认 JIT 行为保持 |
| 翻译身份 | 同物理地址的 ARM/Thumb、IT/FPSCR/FP 环境不同 key；状态变化后不能复用错误 native unit |
| SMC/映射 | guest/host 写代码、相关文件别名、权限变化、解除映射/重映射、当前支持的文件变更；失效影响所有相关 key 与 link/cache |
| fault/restart | 部分提交、恢复后再次执行、REP、LDM/STM、helper/native fault、原 guest PC/mode；先捕获现场再测试 Linux frame |
| 同步信号与异步暂停 | frame/sigreturn/restart、L1 指针中断、恢复；异步时验证可达的合法指令前缀状态，不能要求固定 nondeterministic delivery cycle |
| disk cache/AOT | cold/warm、真实命中与 compile=0、source/profile/hash 不符拒绝、独立进程/ASLR 重定位、嵌入 guest、不同位置键的条目 |

ARM guest SMC 用例必须包含 Profile 要求的 D/I cache maintenance、barrier 和执行同步序列；host 写代码则走明确的 runtime 写入/失效 API 合同。参考 adapter 在合同指定的维护/重建边界同步其译码缓存。没有维护的 ARM write→execute 不能将 QEMU/Unicorn 即时 TB invalidation 当作唯一 ISA expected；runtime 若额外承诺更强一致性，另按该承诺验收。

cache/image 元数据可以单独做 parser fuzz，只验证解析、边界、拒绝与资源清理；**不能直接执行随机变异后的 native code section**。缓存格式校验不是机器码安全验证器。语义 native fuzz 的机器码必须由当前编译器从受控 Case 产生。

线程暂停/异步信号专项不能拿某一个固定终态冒充完整中断证明；需要根据受限 payload 的合法前缀集合判断 PC、架构状态和恢复一致性。不可用的当前机制明确记录，不通过添加生产逐指令检查点补测试便利性。

## 14. 文件组织与职责

按阶段创建实际需要的文件，不先生成空 provider/工厂层。下面是完整目标布局；`arch` 文件初始只处理薄适配，若数值/编码规则长大，再按真实职责拆分，不能变成新的巨型 switch。

```text
source/tests/correctness/
  CMakeLists.txt
  case.h / case_io.cpp
  catalog.h / catalog.cpp
  generate.cpp
  execute.h / execute.cpp
  execute_vm.cpp
  compare.cpp
  reduce.cpp
  main.cpp
  arch/
    x86.cpp
    arm64.cpp
    arm32.cpp
  oracle/
    oracle.h
    unicorn.cpp
    native_probe.cpp
    qemu.cpp
    dynarmic.cpp
  corpus/
    regressions/*.svmcase

source/tests/fuzz/
  CMakeLists.txt
  decode.cpp
  pipeline.cpp

tools/correctness/
  run.py
  workers.example.json
```

| 文件 | 唯一主要职责 |
| --- | --- |
| `case.h` | Case、Observation、RunConfig 和结果枚举的稳定逻辑结构；与生产 GuestState 解耦 |
| `case_io.cpp` | 唯一二进制 codec、hash、JSON view 与协议版本；输入有界检查集中在这里 |
| `catalog.h/.cpp` | Form ID、支持面、独立 ComparisonContract、必测轴和覆盖事实汇总；不执行指令 |
| `generate.cpp` | 稳定 PRNG、通用边界组合、结构化 mutation、序列/CFG recipe；持久化产物只有 Case |
| `execute.h/.cpp` | 公共执行协议、地址具体化规则、portable/Direct 编排和 Observation 输出；不无条件包含 `vm.h` 或链接 Linux-only runtime |
| `execute_vm.cpp` | Linux/Android 的真实 GuestMemory/VM materialization、冷 RunToBoundary/compile evidence 适配；复用生产 owner，仅 native worker 编入 |
| `compare.cpp` | 定义位、合法结果集合、内存/fault 对照、结构化差异；不根据 DUT 输出动态放宽规则 |
| `reduce.cpp` | 重用原 executor/comparator 的 failure predicate、delta 与操作数/状态缩减 |
| `main.cpp` | generate/sweep/replay/reduce/report 命令及 worker 入口；不放 ISA 语义 |
| `arch/{x86,arm64,arm32}.cpp` | 各 ISA 编码模板、合法性/状态规则、GuestState 注入与架构投影；使用按 form 的 handler/表派发。规则增多时就地拆为 ISA 目录中的 catalog data、生成规则和 state 文件；中央 catalog 仅聚合 |
| `oracle/oracle.h` | 冷粒度参考执行协议、能力/版本/来源关系；不建立 per-op 虚函数体系 |
| `oracle/unicorn.cpp` | Unicorn 状态与映射适配、精确 stop、能力自测；不在 invalid-memory hook 自动修好 fault |
| `oracle/native_probe.cpp` | 独立硬件 probe 的状态协议、regset/异常/停止能力自测；不链接 DUT 语义 |
| `oracle/qemu.cpp` | 独立 QEMU probe 通信和结果读取，不实现另一套 ISA |
| `oracle/dynarmic.cpp` | 可选补充参考，记录共同来源和 JIT/Interp 实际模式 |
| `fuzz/decode.cpp` | 按编译目标选择 ISA 的薄 raw-byte libFuzzer 入口 |
| `fuzz/pipeline.cpp` | 复用 Case codec/mutator 和现有 pipeline/Direct 的薄入口 |
| `run.py` | 分片、worker 批次、ADB/system-QEMU 传输、watchdog、证据/配额与清理；不包含指令 expected |
| `workers.example.json` | worker 能力、命令、地址/页粒度、scratch/cache 根配置示例，不提交本机隐私凭据 |

对外初始只需两个普通测试 executable：`svm_correctness`（含 worker 子命令）和独立 oracle probe；进程隔离靠启动子进程，不为每种 ISA/backend/config 再写一套 CLI。内部 `svm_correctness_core` 提供共用逻辑，测试 worker 只链接对应 host backend 的真实运行入口。

`svm_correctness_core` 仅包含 codec/catalog/generate/compare/reduce、ISA 薄适配及 portable 执行编排。Linux/Android 的 CLI 另加入 `execute_vm.cpp`，链接由同一生产 source list 构建的 `svm_execution_test` 变体；测试边界及必要的冷 formation 观测仅在对应测试构建启用，不能复制 runtime/前端实现，也不能在同一进程重复链接生产和测试两套符号。macOS CLI 对 VM/native 请求返回 infrastructure unavailable；libFuzzer 目标只链接所需 portable core/frontend/codewriter，不携带测试 VM。

若 native probe 需要少量汇编，按 guest ISA 放在 `oracle/probe/`，用于状态注入/捕获和受控停止，代码与 DUT 后端 assembler 独立。跨进程协议复用 Case/Observation；probe 仅共编 codec/协议代码，不能链接携带 frontend/numeric 依赖的整个 correctness core，也不能调用 SVM 语义库计算 expected。

现有 `tests/rebuild/*` 不全量搬迁。保留测试入口，将有价值的固定编码/预期逐步注册到 catalog/corpus；不复制到新目录后长期双份维护。

## 15. 构建、运行与资源控制

### 15.1 测试依赖与构建

新增可选测试开关，例如 `SVM_BUILD_CORRECTNESS`、`SVM_BUILD_FUZZERS`、`SVM_TEST_WITH_UNICORN`、`SVM_TEST_WITH_DYNARMIC`。依赖锁定版本，仅测试 target 链接；不进入生产库，不在普通构建中自动下载大型工具链/引擎。复用上游模板/测试代码时保留其许可、来源与固定版本。具体开关以实施时现有 CMake 命名风格统一。

sanitized host fuzz、production-ABI correctness worker、production benchmark 使用不同 build identity 和缓存输出；不能把插桩变体混进 compile 性能基线。框架本身只做少量 codec roundtrip、比较规则、oracle/边界自测和 reducer 重放验证，不为每层包装再堆一套镜像测试。

以下命令说明目标交互方式，**目前尚不存在**：

```sh
# 固定边界集、生成完整 Case、同配置重放及缩减
svm_correctness sweep --guest arm32 --campaign single --config matrix.json
svm_correctness generate --guest x86_64 --seed 1234 --cases 10000 --out corpus
svm_correctness replay failure.svmcase --config original-run.json
svm_correctness reduce failure.svmcase --config original-run.json --out minimized.svmcase
svm_correctness report results --catalog-version <version>

# 本地 libFuzzer；具体目标按 guest 编译
svm_fuzz_decode_arm32 decode-corpus -max_len=64 -max_total_time=300

# controller 使用配置中的真实设备、系统镜像和固定缓存
python3 tools/correctness/run.py --workers workers.json --campaign mandatory --seed 1234
```

### 15.2 实际 worker 与磁盘约束

- ARM64 native：用户连接的红魔 `bf1359ff`，通过 ADB 运行批次。正确性测试不需要 PMU，不改频率或系统调度；只在自有任务目录部署/回收。
- x64/RV64/LA64：复用已有 Linux system-QEMU 内核和固定构建缓存，实际执行对应后端机器码。TCG 结果属于功能验证，不是对应真机性能或全部硬件行为的证明。

2026-11-09 的跨后端实测：在 OrbStack Ubuntu 中用 `x86_64-linux-gnu-gcc`、`riscv64-linux-gnu-gcc` 和 loongarch64 cross-tools 重建 `submit-{x64,riscv64,loongarch64}/svm_correctness`，再用 QEMU system（真实 Linux kernel、2 vCPU、TCG；x64 q35/max、RV64 virt、LA64 virt+EFI）和自定义 initramfs 执行 pinned fuzz 抽样 280 例。三架构均为 270 agreement + 10 gaps、零失败，`native_executed=280/280`、block scope、`interpreted_steps=0`；证据在 `/Volumes/Data2/swiftvm-correctness-results/qemu-cross-20261109/`。RV64/LA64 的 unaligned byte fallback 曾把 fault width 记为 1，已改为逻辑访问宽度。

### 15.3 前端 pattern inventory（2026-11-10，全量完成）

- ARM32：从 `decode/generated_tables.inc` 生成 788 个 pattern（759 个 opcode；Arm 261、Thumb16 79、Thumb32 239、VFP 62、NEON 147）的 synthetic case，4 个后端各 788/788 编译通过。修正 Thumb32 半字顺序并按合同更新比较器后的 Direct 全量矩阵：**540 terminal、201 illegal、13 budget（控制流 one-step）、29 memory fault、2 breakpoint、2 service（SVC）、1 rejected（FPSCR exception enable）**。Direct 已把 LDREX/STREX-family 的 state service 接到 arena monitor 上：LDREX/LDREXB/LDREXH/LDREXD 及 Thumb32 STREX/STREXB/STREXH/STREXD、LDREX 家族均 terminal；LDA/STL/LDAEX/STLEX 是 ARMv8-only，在默认 armv7 profile 下由 `InstructionEnabled` 正确判为 undefined，不是缺口。SWP/SWPB、x86 CMPXCHG 等原子服务已可执行。报告见 `/Volumes/Data2/swiftvm-correctness-results/arm32-inventory-20261109/coverage-report.json`。
- ARM64/x86：83 个 VIXL family、544 个 Op；下一步从 decode corpus/family 提取代表编码，跑同样的编译+执行矩阵。
- ARM32 后端执行（QEMU x64/RV64/LA64，669 个有效页末 case，三后端结果一致）：**637 agreement + 19 agreement+gap + 2 profile_gap + 1 oracle_cpsr_gap + 10 oracle_inconclusive、零 exception/semantic/rejection 失败**；`native_ok=652/669`。修正 Thumb32 半字顺序后，原 246 exception 降至 15；按合同修正比较器后，15 个 fault_width 2/4 vs 0 的 VFP/coprocessor/停止边界用例归为 execute-fault width gap（instruction-fetch width 不是架构宽度），1 个 `gpr[0] 0x10/0x30` 为 ORACLE CPSR/T-bit 投影缺口，2 个 FPSCR rejection 为 profile gap（FPSCR exception enable、big-endian 明确拒绝）。QEMU 证据在 `/Volumes/Data2/swiftvm-correctness-results/qemu-arm32-vm/`。红魔 223 例 sample：**211 verified + 5 verified_with_gap（原 5 exception，同为 execute-fault width gap）+ 2 verified_with_gap + 3 oracle unsupported + 1 oracle_cpsr_gap + 1 profile_gap、零失败**；原执行时的 5 个 `policy_fallback` 是 run.py 把 fetch fault 误判为未 native 执行，已改为只在 terminal/budget/service 时要求 `native_executed`，5 例定向重跑为 verified_with_gap（`/Volumes/Data2/swiftvm-correctness-results/20261110-042610-targeted/`）。
- 全量 ARM32（669 个有效页末 case，2026-11-10）：红魔 ARM64 **637 verified + 19 verified_with_gap + 10 oracle_unsupported + 2 profile_gap + 1 oracle_cpsr_gap，零失败**，与 QEMU x64/RV64/LA64 的 637 agreement 分布一致；ARM32 全量在三 VM 后端与真机 ARM64 上闭合。证据：`/Volumes/Data2/swiftvm-correctness-results/20261110-arm32-full-redmagic/`。
- Direct 二次筛查：159 个 ARM32 pattern 在 canonical 操作数下 illegal/rejected，其中 82 个通过 16 个 valid variant 恢复（含 SWP/SWPB 的原子交换）；77 个在 16 个 variant 后仍 illegal，主要是 UDF/特权（RFE/SRS/MCR/MRC/CRC32）、canonical NEON/VFP 约束形式，以及 ARMv8-only 的 acquire/release（LDA/STL/LDAEX/STLEX，armv7 profile 下按 undefined 处理）。Direct 已补上 atomic 服务和 exclusive state service，SWP/SWPB、LDREX/STREX 家族和 x86 CMPXCHG 等已可执行。

### 15.4 ARM64/x86 代表编码 inventory（2026-11-10，全量完成）

- 从 decode/raw-pipeline 语料提取代表编码：**x86 725/904 个 Op、1741 个编码；ARM64 80/82 个 family、292 个编码**，共 2033 个 synthetic case（805 个唯一 key）。
- 编译矩阵：**1941/2033 在 4 个后端全部通过，0 个跨后端差异**；92 个在 4 后端都被 frontend 拒绝（x86 68、A64 24；含控制流/系统/前缀策略等单指令边界外形式，以及 A64 保留字段/无效编码）。先前 9 个跨后端差异已修复：CRC32C 在编译矩阵中启用目标 FEAT_CRC32（VM 执行仍按探测到的 host 能力门控）；0x67 地址覆盖下 x87 DP 操作数在前端零扩展为 U64（修复 FLD/FNSTSW 的 x87 内存形式）；ARM64 后端补上 V128 `Select`（`cset`+`dup`+`bsl`）。红魔定向验证：x86 CRC32、A64 FNEG FP16 均 verified，FLD 原生执行成功（仅 x87 state 为 oracle gap），证据在 `/Volumes/Data2/swiftvm-correctness-results/20261110-043500-item3/`。
- Direct 执行：x86 1363 terminal、40 budget、1 service、153 memory fault、101 alignment fault、7 divide、1 breakpoint、2 illegal、73 rejected；ARM64 235 terminal、7 budget、1 service、15 illegal、1 breakpoint、10 memory、23 rejected。
- Host 与 Unicorn 差分（合同记账后）：x86 **803 agreement、111 semantic（111/111 由 native 参考裁决为 Unicorn 错误）、0 exception、187 inconclusive、100 oracle_alignment_gap、1 oracle_gp_gap、1 oracle_service_gap、8 prefix_policy_gap、20 unsupported_guest_feature_gap、43 control、29 nondeterministic、438 x87/MMX gap**；ARM64 **244 agreement、6 control、31 inconclusive、7 unpredictable_encoding_gap、4 unsupported_guest_feature_gap、0 unexpected_rejection**。fault 投影差异按规则归因：legacy SSE 对齐检查、128-bit 访问宽度、32 位以上 fault address 截断、空/unknown access、divide width、instruction-fetch vector；A64 scalar FP16 misc 的未分配编码按 unpredictable gap 处理。VM 测试边界要求 terminal 位于可执行源之外，后端执行使用代码置于 `0x04010000-len` 的页末副本；host 差分使用页首副本。
- Priority-2 前端修复：`LDDQU`/`MOVSLDUP`/`MOVSHDUP`/`MOVNTDQA` 缺失或错误的 128-bit 内存宽度（`MOVDDUP` 保持 64-bit）；`BLENDVPD`/`BLENDVPS`/`PBLENDVB` 的隐式 XMM0 第三操作数；`CVTPD2DQ` 缺失 mandatory F2 前缀；`CVTPD2PI`/`CVTTPD2PI` 被 distorm 错报为 XMM 目标。Direct 失败现会报告最后未支持的 IR opcode（`runtime/ir/direct` 诊断，不进入热路径）。
- 独立 x86-64 native 参考（item 4）：新增 `tools/correctness/native_probe.{c,S}` 与 `native_probe.py`，把 case 的代码/内存/寄存器/YMM/MXCSR 映射到宿主真实 CPU，执行单条指令后回读完整状态。**其中不带 66 前缀的 MMX `PADDQ` 被 distorm 错报为 128-bit XMM 形式，已在 frontend `CorrectWidths` 中按无 mandatory 66 修正为 MMX/64-bit（真实 DUT bug）**。对与 Unicorn 的 semantic 差异逐例执行 native 参考：**114/114 与 DUT 一致**（后续合同更新后为 111 例，仍 **111/111** 一致，证据 `native-x86-20261110/semantic-final/`），包括 AVX 内存操作数、LAHF、SAHF、POPF、BLSI 和 LOCK NEG；Unicorn 的 AVX 内存投影、LAHF AH bit1、NEG PF/SF 等结果被真实 CPU 否定。另有 8 例此前差异为 BSF/BSR 源 0（目标架构未定义）与 MMX 投影，均已按 gap 处理。证据：`/Volumes/Data2/swiftvm-correctness-results/native-x86-20261110/`（114 个 native observation、对应 DUT observation 与 `compare-native-vs-dut.json`，114/114 agreement；可用 `tools/correctness/native_probe.py` 重放）。
- native 参考已扩展到 x87（FXSAVE）：输入/输出携带 FCW/FSW/FTW/FOP/FIP/FDP/ST0-7，对 Rosetta 的一比特 tag word、逻辑/物理寄存器旋转做归一化，FOP/FIP/FDP 与 stack-fault 副作用按 gap 报告。`FLD/FLDZ/FLD1` 等基础形式与 DUT 一致（仅投影 gap）。x86 inventory 的初始 x87 栈为全空，59 个已探测 x87 case 中 55 个立即 stack fault（按 `native_x87_fault_gap`），只有 4 个真正执行 x87 语义；**x86 inventory 生成器仍需为 x87/MMX 形式初始化有效栈/寄存器**。MMX 仍为 x87/MMX gap（Rosetta FXSAVE 不投影 MMX tag/exponent）。
- 控制器与 campaign 集成：`run.py --native-dir` 对 Unicorn 的 semantic/exception 差异追加 native 复核，一致则记 `native_verified_gap`；`adjudicate.py` 对已产生的结果做同样重分类。**A64/x86 250 例后端 sample（QEMU x64/RV64/LA64 + 红魔，使用修复后二进制）**：QEMU 三后端 124/127 agreement + 10/7 agreement+gap + **10 native_verified_gap/后端、0 semantic、0 exception** + 11 alignment + 59 x87 + 25 inconclusive + 4 nondeterministic + 3 unpredictable + 2 unsupported feature + 1 service + 1 prefix policy；`native_ok=238/250`。红魔最终 `verified 124 + verified_with_gap 10 + native_verified_gap 10 + alignment 11 + x87 59 + nondeterministic 4 + unsupported 23 + inconclusive 2 + unsupported_feature 2 + unpredictable 3 + prefix 1 + service 1`，**零 failures**。证据：`/Volumes/Data2/swiftvm-correctness-results/qemu-a64x86-20261110/`、`.../20261110-native-sample250b/`。
- 真实 x86 硬件参考（priority-4）：新增 `.github/workflows/native-x86-reference.yml`，在 `ubuntu-latest` 上构建并运行 native probe，对 repo 内置的 424 个初始化 x87/MMX case（`tools/correctness/native-corpus/x86-x87-mmx/`）产出 422 个 observation（2 个超时）。这解决了 Rosetta 不执行 MMX 的限制：**MMX 形式 202 agreement + 2 native_probe_gap + 4 inconclusive（0 semantic/exception）**；x87 为 **122 agreement + 45 native_x87_fault_gap + 28 x87_state_gap + 12 x87_estimate_gap + 6 native_probe_gap + 1 inconclusive（0 semantic/exception）**。关键修正：guest 映射不得与 probe 栈重叠（ASLR 下 `setarch -R` + 显式检查）、SIGSEGV 使用 `sigaltstack` 避免信号帧写入 guest 栈、FXSAVE tag word 按一比特 valid mask 编码、FXSAVE 逻辑 ST 槽按 TOP 旋转到物理寄存器、代码页整页按可读零填充。x87 state save/restore 编码与超越函数精度按显式 gap 处理。证据：`/Volumes/Data2/swiftvm-correctness-results/native-ci-20261110/`（CI run 42844450784），比较见 `compare-final.json`。
- ARM32 v8 profile（priority-4，2026-11-10 完成）：harness 接受 `armv8-user` 并传入 Direct、IR 构建与 **VM RuntimeOptions**，Unicorn oracle 使用 `UC_CPU_ARM_MAX`。从 ARM32 inventory + valid variants 生成 16 个 LDA/STL/LDAEX/STLEX case（含手工构造的合法 STLEXD 编码 `0xe1a52e94`），Direct 全部执行完成、STLEX/STLEXB/STLEXH 返回无 reservation 的 status=1；**16/16 与 Unicorn agreement**，4 个后端 **16/16 编译通过**。VM 路径此前未把 `arm32_profile` 传入 RuntimeOptions，JIT 按 armv7 编译 v8 指令并报 illegal instruction fault（16/16 fault）；修复后 **QEMU x64/RV64/LA64 各 16/16 agreement（`native_ok=16/16`）、红魔 ARM64 16/16 verified**。证据：`/Volumes/Data2/swiftvm-correctness-results/arm32-v8-20261110/`、`.../qemu-arm32-v8-20261110/`、`.../20261110-arm32-v8-redmagic/`。
- 性能回归检查：用 baseline `4b44ba96` 与当前 HEAD 在相同 Release/Ninja/编译器/`SVM_ENABLE_IPO=OFF` 配置下分别构建 `svm_ir_build_submit_bench`，交错 3 轮（每轮 7 samples）得到 `ns_per_attempt` 中位数均为 **3552.7 ns**（baseline 3721/3553/3547，current 3618/3548/3553），无可测回归。`svm_x86_build_submit_bench` 的固定负例自检在 baseline 与 current 上均失败（既有 fixture 问题，与本轮改动无关），因此未采用其数值。
- 全拒绝归因：92 个全后端 build 拒绝（x86 68、A64 24）中，控制流（CALL/RET/Jxx）、FXSAVE/XRSTOR、RDRAND、MOVBE、System/PAC/exclusive 等为单指令边界外或系统特性；带 LOCK/冗余 F2/F3 前缀的形式由 `AllowedPrefixes` 策略拒绝；A64 保留字段/写回别名编码为架构未定义。Direct 侧 `unexpected_rejection` 已从 57 降到 **0**：A64 `NEONScalar2RegMiscFP16` 的编码（0x7ef9a828）经与 VIXL `NEON2RegMiscFP16Op` 表比对确认不在已分配操作表中，按 unpredictable gap 处理。跨后端差异保持为 0。
- 后端执行（250 例抽样，per-case worker）：QEMU x64 155 agreement + 25 exception + 12 semantic + 7 rejection + 23 inconclusive + 27 x87 + 1 nondeterministic；RV64/LA64 各 158 agreement + 22 exception + 12 semantic + 7 rejection + 23 inconclusive + 27 x87 + 1 nondeterministic；`native_ok=237/250`。红魔 ARM64 JIT 153 verified + 24 exception + 13 semantic + 8 rejection + 23 oracle unsupported + 27 x87 + 1 nondeterministic（另 1 infrastructure failure）。三 QEMU 后端共享同一批 AVX/x87/BSF/LAHF 差异，没有新的后端专属语义失败；batch worker 已改为 per-case，单例 VM 失败不会阻塞后续用例。
- 全量后端执行（2033 例，2026-11-10）：QEMU x64 **1003 agreement + 76 agreement+gap + 224 inconclusive + 111 semantic + 3 exception + 24 unsupported_guest_feature + 438 x87 + 100 alignment + 29 nondeterministic + 8 control_one_step + 8 prefix + 7 unpredictable + 1 oracle_service + 1 oracle_gp，`native_ok=1903/2033`**；RV64/LA64 各 **1027 agreement + 52 agreement+gap + 224 inconclusive + 111 semantic + 3 exception + 24 unsupported + 438 x87 + 100 alignment + 29 nondeterministic + 8 control_one_step + 8 prefix + 7 unpredictable + 1 service + 1 gp，`native_ok=1903/1904`**；三后端均 0 rejection、0 timeout、0 parse/host failure。红魔 ARM64 **1002 verified + 76 verified_with_gap + 100 alignment + 222 oracle_unsupported + 26 nondeterministic + 8 prefix + 8 control_one_step + 437 x87 + 6 inconclusive + 24 unsupported_guest_feature + 1 service + 1 oracle_gp + 7 unpredictable**，其余 115 个 x86 semantic/exception 经 host CPU 复核为 **113 native_verified_gap + 2 verified_with_gap**，零剩余差异、零 timeout/host failure。证据：`/Volumes/Data2/swiftvm-correctness-results/qemu-a64x86-full-20261110/`（x64 的 `results.json.gz` 与各 arch 目录）、`.../20261110-full-a64x86-final/`。
- 全量差分 native 裁决（2026-11-10）：AVX/SSE 全部形式、LAHF/SAHF/POPF、BLSI、BTC、XCHG、LOOPNZ、SQRTSS 等 115 个 x86 差异全部与真实 x86 CPU 一致（Rosetta/FXSAVE 参考），Unicorn 是唯一偏差方；QEMU 三后端对同一批 115 例的 113 个 Unicorn 差异也全部 native_equal（其中 2 例带 NaN/fault 投影 gap）。证据：`/Volumes/Data2/swiftvm-correctness-results/20261110-a64x86-adjudication/native-final/`、`native-adjudication-final.json`、`.../qemu-adjudication-20261110/`。
- 第三方 FEX 复核（2026-11-11）：在 OrbStack arm64 Ubuntu 中用 `FEXInterpreter` 执行 Linux 静态 x86-64 probe（`native_probe.py --exec-prefix "orb -m ubuntu /usr/bin/FEXInterpreter"`），对 676 个缺口/差异 case 得到 667 个观测（5 个 FEX 自身崩溃：FXSAVE [r8]、POP FS、0F 00 组）。**AVX/SSE/legacy 112/112 与 DUT 一致**，独立再次证明 Unicorn 是错误方；XSAVE/XRSTOR 家族 FEX 可执行，证实 DUT 的 unsupported 归类属实；legacy SSE 对齐 100 例中 FEX 与 Unicorn 都不建模 #GP，DUT 的 #GP 经 ADDPD/ADDPS/HADDPD/MOVNTPD 有效地址抽查符合 ISA（Rosetta 仅 28/100 报 fault）；x87 inventory 258/436 可比一致，其余差异集中在 FEX 的 FXSAVE/x87 状态投影与 FCOMI unordered 标志（DUT 0x45=ZF|PF|CF 符合 ISA，FEX 0x40），未发现 DUT 偏差；BTC/XCHG 两例 FEX fault 地址比 DUT 与 Rosetta 偏 1，属 FEX 上报差异。证据：`/Volumes/Data2/swiftvm-correctness-results/fex-ref/`（`summary.json`、`diffs-swapped.json`、`rosetta-align/`）。
- 执行器与比较器修正（2026-11-10）：per-case VM worker 增加 8s 执行看门狗，guest 自循环（`b .`/`loop $`/`jmp $`）不再无限占用 worker，oracle 预算耗尽的用例统一归 `oracle_inconclusive`；QEMU init 每例回收 `/out`，修复 2033 个 observation 撑爆 guest tmpfs 导致的 621 例空输出；看门狗短于 run.py 的 10s worker 超时。比较器新增：x86 quiet-NaN sign/payload 按 IR 合同计 gap（SQRTSS(-x) 不再误报）、XSAVE/XRSTOR 状态组件与保留位 MXCSR 状态按 `unsupported_guest_feature_gap`（Unicorn 与 Rosetta 都不投影相应 #GP）、native 参考作为 expected 时的 fault access/vector/width 投影 gap。
- macOS host：portable decode/build/Direct、codec/比较自测和 sanitizer fuzz。当前裸 native support 是 Linux/Android，不将 macOS compile-only 统计成 native 通过。
- 大 scratch 统一放 Data2，例如 `/Volumes/Data2/swiftvm-correctness-results`；构建复用 `/Volumes/Data2/swiftvm-middle-build-cache` 下独立测试子目录，不再把大结果写系统 `/tmp`。
- 默认总 scratch 配额可先设 8 GiB、单 worker 临时配额 512 MiB，结合实际剩余空间下调；固定系统镜像/构建缓存独立计量。达到阈值停止 campaign 并报告，不删除未归档失败。
- 正常样例保留聚合计数与有限 novel corpus，成功 batch journal 及时回收。唯一失败保留原始 Case、缩减版和必要证据，不保存每例二进制副本、完整 RAM 或每轮 initrd。
- 清理只针对 controller 记录的本次 PID 和自有目录，失败/中断后生成清理 receipt。跨系统重放复用固定镜像和批量协议，避免每个 Case 重建/推送整套运行场。

分片由稳定 Case ID 决定，worker 数变化不改变生成输入；固定版本下可重复分发。先做好本机/ADB/QEMU 的同一协议，再接现有 CI，不在第一阶段额外建设服务端调度平台或自动定时任务。

## 16. 实施顺序与阶段验收

不先铺满所有 oracle 和 runtime 场景；先证明一个完整输入确实能在独立参考与四后端之间正确重放。复现与缩减从第一阶段就存在。

| 阶段 | 实现内容 | 完成条件 |
| --- | --- | --- |
| P0：冻结合同与最小闭环 | 三前端 form inventory 初版、Case/Observation codec、比较投影、RunToBoundary、worker/journal、最小 reducer | 三 ISA 各有固定整数/flags/FP/内存用例，注入已知差异能准确报错；终止不污染状态；崩溃可保存并重放 |
| P1：单指令正确性 | 独立 oracle 接入、自测/能力表、各 form 边界集、raw-byte decode fuzz；优先 ARM32 新实现及 carry/shift/部分寄存器/FP/alias 高风险，再按清单补全三个前端 | 所有已声明 form 均有生成/合同归属；每个 form 的必测格可查询，四后端执行和独立确认分开列出；未知拒绝不能被过滤 |
| P2：流水线与压力 | 依赖短序列、真实多块 CFG、Block/Function 分配、opt/target/pin、helper/flags/spill、structured/native fuzz | mandatory 基础矩阵覆盖，实际 scope/CFG/压力有证据；只在回退路径成功的格仍列缺口 |
| P3：故障与生命周期 | partial commit/restart、信号/暂停、SMC、link/cache、Mixed、disk/AOT、跨进程重定位、元数据 parser fuzz | 每个场景有独立边界和证据；故障前现场与 Linux frame 分开；回归可在全新 worker 与规定 warm 场景重放 |
| P4：并发与扩展硬件 | backend litmus、guest model 合同、真实 host 并发、可选指令硬件与参考扩展 | 不出现 guest 模型禁止结果；runtime 多 guest thread 未提供的能力独立标记；真机/TCG 证据不混合 |

P0 先选少量用例打通基础矩阵，不先随机烧机；P1/P2 才扩大覆盖与时长。每个阶段修复时用最小重放和受影响 mandatory 集验证，在阶段收口集中跑完整矩阵，不在每次源码编辑后重复整轮 benchmark 或所有 fuzz。

建议初始调度预算：

- 每次提交：永久失败 corpus、改动 form 的固定边界、当前 host 可执行的代表配置；缺少设备的格显示未执行。
- 阶段收口/夜间人工或现有 CI 触发：三个前端、四个后端的 mandatory 矩阵与轮转随机 campaign，按 shard 可续跑。
- 较长 campaign：raw decode、FP、CFG/压力、fault、runtime 分开预算和统计；争议 oracle 样例优先硬件复核。

不在本文预先承诺“跑若干小时就完整正确”。验收必须输出 catalog 版本、固定分母、每个必测 case class/config 的状态、oracle 来源及剩余缺口；例如合法 form 无参考、Function 回退或某后端未执行，都属于尚未完成的工作。

## 17. 必须一直保留的设计约束

1. 相同 Case、相同具体输入、相同架构合同；oracle 与 DUT 独立到达边界。
2. 正确性标准来自 ISA/已声明 Profile，不能由 DUT 当前能翻译什么决定。
3. Direct/JIT 一致、编译成功、TCG 执行、真机独立确认分别报告。
4. Block 与 Function 分配分别验证；Function scope 不自动等于多基本块 region。
5. 失败原始输入先保存，缩减保持原失败谓词，任何排除都有记录且不缩分母。
6. 测试框架复用现有执行 owner，不把 fuzz 便利性变成生产新 pass、逐指令检查或另一套 IR。
