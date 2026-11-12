# ARM64 → x64 性能优化方案（2026-11-11）

目标：在保留现有 ARM64 数值、状态观察与异常恢复语义的前提下，优先缩小与 Dynarmic 的热执行差距。首轮按 **原生浮点常见路径 → 条件分支融合 → 访存与状态搬运** 推进；每一步独立验证、独立提交。

四批首轮实现及验收已完成，结果见[优化验收记录](arm64-x64-optimization-results-2026-11-11.md)。下文保留实施时的设计范围和验收方法，P3b 的评估结论一并记入结果报告。

## 1. 基线与优先级

生产运行时基线为 SwiftVM `da0a692074dc00b56355bf160287b76f2fa5379c`，参考为未修改的 Dynarmic `9d4582339990d4eae53f1dc7160686920fc2075c`。分析依据为[差距分解](dynarmic-gap-analysis-2026-11-11.md)、[原始对比](dynarmic-benchmark-2026-11-11.md)及其证据目录。

下表采用差距分解中的 21 对新进程 PMU 基线：WSL、CPU2、Release/IPO，SwiftVM Function/pin0，相同 ARM64 ELF，单位 ms。

| 负载 | SwiftVM | Dynarmic | 主要证据 | 对应工作 |
| --- | ---: | ---: | --- | --- |
| FP 微循环 | 19.212 | 4.370 | 每轮 446 / 28 条宿主指令；约 75% 样本位于数值 helper 及调用边界 | P1 标量浮点 |
| 原始综合程序 | 18.134 | 4.001 | 独立矩阵分项 9.607 / 0.121；原程序包含 NEON binary 和 indexed multiply | P1 向量浮点 |
| 整数微循环 | 39.626 | 13.057 | 每轮 47 / 23 条；SwiftVM 中 flags 捕获及布尔条件链占 31 条 | P2 条件消费 |
| 访存微循环 | 39.537 | 15.510 | 展开 8 次后仍有约 2.63× 差距，存在固定寄存器中转和状态写回 | P3 访存与状态 |
| 移位微循环 | 39.992 | 15.270 | 同样受循环 flags 链影响，基本循环没有 spill | P2，随后复核 |

矩阵与 SHA 分别约占四个**独立重编译分项差值之和**的 66% 和 26%，只用于排序，不能作为原综合程序的精确耗时比例。CBNZ、展开循环、关闭向量化的 guest 均为诊断对照，正式收益必须在原 ELF 上重新测量。

当前 x64 实现支持 0/2 个 pinned uniform slots，默认 0，`runtime.cpp`、`emit.cpp` 和恢复代码均已有对应实现；后端 README 中“无 pin bank”的旧描述需要在后续相关提交中校正。主性能比较固定实际 pin0，pin2 用作正确性及次级性能检查。

## 2. 实施顺序与依赖

| 批次 | 交付结果 | 主要验收负载 | 复杂度与依赖 |
| --- | --- | --- | --- |
| P0 | 固定可重放基线、原 ELF、配置和结果校验 | 五个原负载及现有控制组 | 小；所有批次的前提 |
| P1a | ARM FP 状态合同、精确慢路径适配和 F32/F64 标量 ADD/SUB/MUL 快路径 | FP 原循环、展开 FP、真正标量矩阵 | 高；先解决状态正确性 |
| P1b | NEON binary 与 indexed FMUL 快路径 | 原始向量矩阵、原综合程序 | 中至高；依赖 P1a 的共同数值边界 |
| P2 | 条件直接用于终结分支，保留 packed NZCV | 原整数、访存、移位循环及 SHA | 中；仅依赖 P0，可独立于 P1 开发 |
| P3a | 直接使用访存地址、数据与分配结果，减少中转 | 展开访存、SHA、排序 | 中；仅依赖 P0 |
| P3b | 仅在可证明安全的局部范围消除中间状态写回 | 展开访存、SHA、排序、异常恢复 | 中至高；在 P3a 后重新归因再决定范围 |

建议串行完成 P1a、P1b、P2、P3a，每批结束后更新同一张性能表。P3b 与进一步的 lazy flags 都由新一轮证据决定，避免同时改变数值语义、flags 表示和故障恢复。

## 3. P0：固定测量合同

复用现有工具和证据，不新建 benchmark 框架：

- 保留普通 runner 和诊断 runner；头条耗时来自不启用采样、PMU 或编译计时的普通 runner。PMU、热点采样及编译诊断分别执行。
- 固定原始五个 ELF 的字节及哈希。原综合程序保留已经确认的向量化；真正标量版本另列。后续新构建 guest 保存实际 driver 参数与反汇编，不能只凭 `-fno-vectorize` 字样判断。
- 候选同时对比未优化的 SwiftVM 基线和固定版 Dynarmic，三者使用相同 ELF、结果检查和运行配置。记录请求配置与实际生效的 scope、pin slots、CPU 特性。
- 保留所有输出检查，要求热执行不新增编译、无解释执行、无 Function fallback。精确数值慢路径仍是合法原生执行的一部分，单独记录其命中情况，不能与解释回退混为一谈。
- 归档源码、构建参数、runner/ELF 哈希、原始样本、代码大小、spill 与 frame 信息；当前未提交的 benchmark、分析工具和证据与后续运行时改动分别整理。

交付为一次可复现的“基线／候选／Dynarmic”比较入口及清单；优先为现有脚本增加复用固定 ELF 的能力，防止重新编译 guest 改变问题本身。

## 4. P1：让常见浮点运算留在生成代码内

### 4.1 先解决状态与 IR 合同

相关位置：`frontend/arm64/semantics_float.inc`、`ir/opcodes.inc`、`arch/arm64/numeric.h`、`arch/arm64/fp_scalar.cpp`、`backend/x64/emit_float.cpp`、`emit_calls.cpp`，以及 `execution/runtime.cpp`、entry/fault 边界。

当前 `ArmScalarFP` 只在 ARM64 宿主生成，x64 明确拒绝它；x64 上的 ARM 数值指令仍走通用数值 helper。已有 `VecF*` 是另一套数值合同，不能直接替换 ARM 运算。

复用现有 Uniform/SSA IR：为支持的 x64 目标开放明确的 ARM FP 语义操作，审查 `ArmScalarFP` 的输入、结果及 effects；向量形式只增加必要的 kind、lane width、有效 lanes、indexed lane 描述，不引入第二套后端 IR。

先明确以下所有权，再启用快路径：

1. ARM FPCR/FPSR 是架构状态，`numeric_control` 和宿主 MXCSR 是执行环境。`UpdateFPEnvironment` 已映射舍入及 flush 控制，但 `MergeFPStatus` 当前只自动合并 x86 guest 的状态，不能直接承担新的 ARM FPSR 更新。
2. 每条 FP 运算的异常贡献必须精确累计，保留既有 FPSR sticky bits、QC 和无关位。首版逐条确认并提交结果与状态，先避免跨指令延迟提交；后续若边界成本成为热点，再单独设计分段累计。
3. 明确 FPCR 读取、FPSR 更新的 IR 可观察效果；常量折叠、DCE、CSE 和调度不得丢失或移动异常贡献，即使算术结果没有数值用户。
4. 经过 FPCR/FPSR 读写、helper、事件、退出、fault、snapshot 时，观察到的是该 guest 边界应有的状态。返回调用者后恢复完整宿主 MXCSR。
5. 沿用现有支持范围：已支持但不适合快路径的控制走精确慢路径；未支持控制保持原来的 Unsupported 行为。不能由初始 FPCR 推断整个可复用代码单元的运行条件。

### 4.2 P1a：保守的标量快路径

首轮范围为默认 FPCR 下的 F32/F64 `FADD/FSUB/FMUL`。FDIV、FMA、FP16、转换、比较和特殊扩展继续沿用精确实现，等第一轮收益和命中分布明确后再扩展。

每条受支持操作按以下流程执行：

1. 在运行时确认控制条件；对操作数做位级分类，首版仅放行已证明等价的零／有限正规数区域。
2. 在临时位置执行 SSE2 运算，保存原操作数，尚不提交 guest 结果或异常状态。
3. 对结果和本条运算的新异常做资格检查。NaN、Infinity、subnormal 及可能受舍入前 tininess 影响的边界转精确路径。**结果为 MinNorm 或零不自动证明安全**：可能由微小的精确结果舍入得到；需要保守覆盖这些边界。
4. 通过检查后，一次性提交结果与准确的 FPSR 增量，并按 ARM 标量规则清零目的向量高位。
5. 未通过时，丢弃试算结果及其宿主异常贡献，使用原操作数调用精确数值叶子。由薄适配调用已有 `EvaluateBinaryFP` 等数学函数，避免重新解码整条 guest 指令；按原顺序提交结果和 FPSR。

宿主 sticky 异常与历史状态必须可区分：不能把试算产生的异常或此前 helper 的宿主状态混入 ARM FPSR。不能仅凭 SSE 异常位直接复制得到 ARM 异常位；需要按准入区域证明映射，特别处理 IDC、UFC、IXC 和 tininess-before-rounding。

首版不为减少检查而放宽 NaN payload、sNaN、signed zero、subnormal、舍入或 sticky status 比较。如果检查成本吞掉收益，则先缩小快路径范围或改善发码，在证据更新前保留原路径。

**交付标准**：安全输入的生成代码不进入数值 helper；特殊输入进入精确数学路径且结果、状态完整一致。FP 原循环和展开循环均应出现可重复的热耗时下降，真正标量矩阵提供完整程序验证。

### 4.3 P1b：覆盖矩阵真正使用的向量形式

在 P1a 共同合同上依次实现：

- NEON F32/F64 ADD/SUB/MUL，首先覆盖原 ELF 的 `FADD V?.2D`、`FMUL V?.2D`，再覆盖同合同的 `.2S/.4S` 形式。
- indexed FMUL，首先覆盖 `FMUL ... V?.D[0]`，明确 lane 提取、广播、源／目的重叠和有效向量宽度。

整条向量指令先计算所有有效 lane，再统一提交。任一有效 lane 不满足资格检查时，整条指令进入精确路径，不能留下部分 lane 结果或试算异常。无效 lane 不参与算术或异常累计；64-bit 目的向量的高位清零等规则显式处理。

首版使用 SSE2 基线；可选 SSE4/FMA 形式由已有宿主特性检测控制。FMA 保持独立语义，禁止把 guest 的乘法、加法自动融合。

**交付标准**：原始向量矩阵和原综合程序都能观察到 helper 命中下降与稳定提速。只有标量微循环变快，不能视为浮点批次完成。

## 5. P2：直接消费条件，保留完整 NZCV

相关位置：`backend/x64/emit_flags.cpp`、`emit.cpp`、`emit.h` 及现有用途／活跃性事实。

第一步匹配单一终结分支消费者的 `TestFlags/TestNotFlags`，对合法的 `CondSet` 形式逐步扩展。由 terminal 直接测试 packed flags 并跳转，省去 `setcc → Bool 归一化 → 写 home → terminal 再读、mask、test`。多消费者、数值用途或不能证明安全的形态保留原发码。

首版仍执行既有 NZCV 捕获、打包和状态发布，RBX 继续保存 portable packed flags。事件检查、故障恢复、命名 snapshot、helper 和退出语义均保持原合同；不依赖宿主 EFLAGS 穿过这些边界继续有效。

复用现有用途信息做局部识别；若必须新增分析，先证明现有事实不足，避免为少数终结条件引入全局 pass 或第二套条件表示。

**交付标准**：原整数循环的条件不再往返 Bool home，退休指令数低于原来的每轮 47 条，原整数／访存／移位热耗时有可重复改善。最终 NZCV、事件退出和后继块读取均需正确；不能以替代 guest 的 CBNZ 输出合同验收。

若完成后 NZCV 捕获／打包仍是主要成本，再单独评估 `lahf/seto`、紧凑表示或按需物化。该后续批次必须先列出 ADD/SUB/ADC/SBB carry 极性、部分 flags 更新、x86 PF/AF、snapshot 和跨块观察规则，再决定表示，不与本批融合提交。

## 6. P3：减少访存与状态中转

### 6.1 P3a：访存结果直接进入分配位置

相关位置：`backend/x64/emit_memory.cpp`、`emit_operand.cpp`、结果写回、fault metadata 与 recovery。

- 简单合法的 base/index/offset 优先使用现有分配寄存器或 x64 地址形式，减少先复制到 RDI 的路径；复杂表达式沿用已有计算。
- Load 在无地址冲突且满足恢复合同的情况下直接写 scalar/XMM home，减少 RAX/XMM0 再复制；spill 仍走既有合法路径。
- Store 在位宽、来源归一化和别名均满足条件时直接使用源 home，减少固定 RAX/XMM0 staging。
- 精确记录实际可能 fault 的指令起点、访问宽度和地址恢复位置。目的寄存器覆盖地址、窄读、向量访问、跨页失败和 pinned 状态都要有明确规则。

**交付标准**：反汇编中可定位的地址／数据中转减少，展开访存出现稳定收益；实际 fault 的 guest PC、地址、访问类型、已完成状态与寄存器恢复全部不变。

### 6.2 P3b：在局部无观察区间消除中间写回

先对 P3a 后的 SHA、排序和展开访存重新归因，仅处理仍然热的写回：

- 复用现有 uniform 转发与死写消除；只在同一可证明无观察区间，消除同一范围内被覆盖的 StoreUniform。先核查是否已有 pass 完成该工作，再增加缺失的局部规则。
- helper、未知别名、uniform 地址逸出、可能 fault 的 guest 访问、safepoint、事件、atomic/exclusive、状态服务和控制流合流，默认作为边界。跨越这些边界需要现有 snapshot/recovery 合同的独立证明。
- reservation 清除单独审计。仅当进入区间时已证明失效、期间不能重新建立 reservation，且故障及观察语义完全相同时，才考虑去除重复清除。不能把后一次清除简单移到循环外，或把 reservation 优化混入普通 dead-store 规则。

**交付标准**：每类被删除的写回都有明确的覆盖／不可观察证明，且真实 VM 的 fault、helper 观察、exclusive 序列测试通过。没有可靠收益或证明不足时，停止在 P3a，保留现有状态维护。

## 7. 正确性与性能验收

### 7.1 正确性门槛

| 改动 | 新增或扩展的永久回归 |
| --- | --- |
| 标量 FP | F32/F64、±0、精确抵消、qNaN/sNaN 与 payload、Infinity、min/max normal、subnormal、MinNorm 舍入边界；全部已支持控制组合；既有与新增 FPSR bits；高 lanes 清零 |
| FP 运行边界 | 快／慢路径交替、FPCR/FPSR 读写、helper、事件／退出／fault、跨块链接及宿主 MXCSR 恢复；结果未使用时的异常贡献仍可见 |
| NEON/indexed | 每种有效 lane 宽度、混合正常／异常 lanes、整条回退、广播索引、源／目的重叠、无效 lanes 不产生异常 |
| 条件融合 | EQ/NE 等已启用条件、正反分支、多消费者不融合、跨块 NZCV 读取、事件退出、snapshot，以及部分 flags／carry 极性回归 |
| 访存／状态 | 窄宽度与扩展、地址和目的重叠、spill、pin0/pin2、未对齐／跨页／权限 fault、helper 可见状态、reservation 与 exclusive 序列 |

复用 `source/tests/rebuild/x64_compile_test.cpp`、ARM64 frontend、IR optimize、runtime engine 和 exception recovery 测试；裸 backend 测试与真实 VM 测试分别承担发码和运行边界验证。

每个运行时批次完成后执行现有 4,224 次基础差分矩阵；FP 批次重跑现有 27,648 次 FP 差分并扩展 NEON/indexed 用例。覆盖 Block/Function、Known/Unknown/EarlyOff、优化开关，pin0/pin2 另列实际覆盖。向量与 FPSR 边界须由独立参考约束，不能只用共享数学函数计算 expected。

完整程序使用 QEMU 和独立结果检查，至少保留上一轮 24 组 whole-program 配置，并加入原向量矩阵、真正标量矩阵和 FP 状态观察程序。触及共享 IR／运行时状态时回归 ARM32/Thumb 和 x86 guest。最终运行完整 CTest；记录真实 skip，不能计入执行成功。

新增 IR effects、helper 地址、entry/fault 状态或代码语义若影响持久缓存，更新相应代码身份／版本，并验证冷 JIT、磁盘缓存重载、重定位与 AOT。其他后端及 Unknown/EarlyOff 路径仍有合法的原实现，不能因只优化 x64 而失去支持。

### 7.2 性能测量与合入门槛

1. 在 WSL `ProtonHM` 的 x64 Release/IPO 构建中固定 CPU2；主组为实际 Function/pin0。基线、候选、Dynarmic 轮换顺序，新进程各 31 个样本，每进程 3 次热执行取中位数。
2. 输出每组热耗时、候选／基线比、候选／Dynarmic 比、逐轮配对比值分布和离散程度。若收益落在同环境噪声范围内，补一次独立测量并标为未确认。
3. 单列进程初始化、首次调用、后续新单元编译与热执行。Dynarmic 首次调用含全局解码器初始化，不能把 `cold - warm` 视为纯编译成本。
4. PMU 另跑相同工作量，检查 enabled/running 相等，比较 instructions、cycles、IPC、branches、branch misses；另保存 IR、发布代码、代码字节数、spill/frame 与 compile 诊断。
5. 每批验收原五个负载，再加入该批的展开／独立阶段控制。FP 同时报告准入、慢路径与 helper 命中情况，诊断计数不得进入头条计时版本。

硬门槛为正确性零新增差异、实际原生执行、无解释回退，且目标代码形态和热耗时均支持优化有效。其他原负载若重复出现超过 3% 的退化，或编译成本／代码大小明显增长，需要定位并缩小改动后重测，不能用综合平均值掩盖。

数值目标先由上述测量确认。本方案不承诺直接达到 Dynarmic，不将 66% 分项占比、CBNZ 替代 guest 的耗时或退休指令减少比例换算成总提速承诺。

## 8. 提交拆分与结束条件

建议按下列顺序形成可独立审阅和回退的提交；基础合同与其使用方若不能单独保持正确则合在同一提交：

1. 整理现有 benchmark／分析工具／证据，补齐固定 ELF 与实际配置的基线入口。
2. ARM FP 语义操作、状态合同、精确叶子适配及对应回归；可以先全部使用精确路径建立等价性。
3. x64 标量 F32/F64 ADD/SUB/MUL 保守快路径及边界验证。
4. NEON binary 快路径、完整 lanes／异常测试。
5. indexed FMUL 快路径及原矩阵／综合性能证据。
6. packed NZCV 的终结条件融合及真实观察边界测试。
7. 访存地址、来源与结果直达寄存器及故障恢复验证。
8. 仅在重新归因仍支持时，提交局部 uniform 死写规则；reservation 清除另成提交。

每批附上“修改前后同 ELF”的正确性与性能证据，保留前一批 runner 作为回退比较。遇到无法证明的 FP、flags 或 fault 边界，保留原精确路径／原发码并缩小适用范围。

首轮结束时，应能分别回答：FP helper 剩在哪些运算／数据；NZCV 打包与条件消费各剩多少成本；展开访存还剩哪些可观察状态写回。再据此决定扩展 FP 控制／FDIV/FMA、紧凑 flags 或进一步状态优化，不提前重写 dispatcher、寄存器分配器或扩充 pin bank。
