# W66：FEX 形态寄存器分配重写的可行性调研与设计 spike

日期：2026-08-01  
性质：纯调研；未修改生产代码，未执行 commit/push/add/checkout/reset/stash  
本仓快照：`w66-ra-reshape`，`5f18ccf`  
FEX 快照：`f2e35f336f0b8bb0df979ffe10e7c6ffbd8af89c`（本机 `/Users/swift/CLionProjects/SwiftVM/.cache/FEX`）

## 0. 结论先行

这次精读不支持把“照搬 FEX 形态”作为一个整体项目立项，但支持拆出两个有严格前置门槛的设计方向：

1. **不能把 FEX 归纳成“动态 spill 直落 guest context”**。FEX 的普通 RA spill 是 block-local、furthest-first，实际落 host `sp` 下的 spill frame；落 `CpuStateFrame::State` 的是静态 guest GPR/XMM/PF/AF 以及 NZCV 在退出或 helper ABI 边界的同步。我们自己的动态 spill 反而已经直接落 `State::spill_area`，并且已做到块间无需全量 flush。因此问题 3a 所设想的主体已经存在，不能再次拿它计算静态消除收益。
2. **Clang `preserve_all` 不能让现有 pin snapshot 归零**。AArch64 上 x0–x8、x16–x18 仍是 caller-saved；我们的 level 2 pin 正好在 x0–x5，level 3 新增 x6–x9，所以 level 2 零收益，level 3 只额外保护 x9。更何况 x0–x7 还是 C 参数寄存器，参数装载本身就会覆盖 guest pin。`x18–x29` 不是正确的目标区：x18 在 Apple ABI 保留，x19–x23/x29 已由普通 AAPCS64 保护，x24–x28 被运行时固定占用。
3. **XMM 全驻留只能在“释放四个固定 cold FPR + helper ABI 分类”之后重测**。FEX 是 `v16–v31` 静态、`v2–v15` 动态、`v0/v1` 固定临时，即 14 个动态 FPR；我们开启 XMM pin 后，`v11–v14` 仍被 NaN cold ABI 固定占用，真实动态池只有 `v0–v10,v15` 共 12 个，且高压指令还必须保留 3–5 个 scratch。W13/W17 的负结果与这个结构一致。
4. **PF/AF 在本仓已经“寄存器驻留”**：它们不是普通内存状态，而是 packed 在 x26 中，PF 保存原始低字节、AF 保存单 bit，NZCV 另在 host PSTATE 中 lazy。另 pin 两个 GPR 不会消掉 PF 的 xor-fold 或 AF 的 bit4 计算，只会把 BFI/UBFX 换成跨寄存器操作，并进一步挤压已经告急的 GPR 池。因此 FEX 的 PF/AF 两个 fixed pseudo-register 不适合原样移植；可继续研究的是 unit-local flag-source SSA，而不是全局再占两个寄存器。
5. **建议顺序**：先加只读计数与 helper 分类证据；再做 cold-edge FPR 保存设计；只给经证明为 leaf 的 helper 试 `preserve_all`；最后才允许把 XMM static 重新送入 A/B。GPR 全 pin 和 PF/AF 双 pin 暂不立项。

## 1. 证据口径与上界算法

本报告直接采信题面已核实数据，并与 `docs/project-status.md`、当前源码和 FEX 源码逐项对照。性能上界只用于筛选，不作为预期收益：

- coremark 最新 CRC 内环 52 vs 22，差额 30 条。若把某类差额占比记作 `p`，理想静态上界为 `30p` 条；理想指令数降幅为 `30p/52`，忽略延迟、带宽、分支和 spill 后的理想加速为 `52/(52-30p)`。
- smallpt W41 站位为 SVM/FEX = 2.80，某类占“超额时间”比例为 `p` 时，理想新时间为 `2.80 - (2.80-1)p`，理想加速为 `2.80 / new_time`。
- W41 数据早于 W57 XMM fault sink 默认开启；因此它只能作为历史宽松上界，不能当作当前可实现空间。

按此计算：

| 类别 | 推导 | 理想上界 | 为什么不能用于立项 |
|---|---:|---:|---|
| coremark 全部 GPR state | `30×35.1%=10.53` 条 | 52→41.47，最多 1.254× | level 3 实测反而 −10.18%，spill 5425→14071 已推翻“静态消除即净收益” |
| coremark 全部 flags | `30×43.2%=12.96` 条 | 52→39.04，最多 1.332× | PF/AF 只是 flags 子集；W38 已拿走一部分且实测仅 +4.86% |
| smallpt 全部 XMM state（W41 历史） | `2.80-1.80×32.01%=2.2238` | 最多 1.259× | W57 后需重新分解；helper/FPR spill 成本未扣 |
| smallpt 全部 GPR+XMM state（W41 历史 45–56%） | 新时间 1.99–1.792 | 最多 1.407–1.563× | 混合了两个机制，且是已被后续优化侵蚀的总上界 |

## 2. FEX 形态精读

### 2.1 精确静态映射与 RA 的关系

FEX x86-64 非 ARM64EC 的固定映射不是笼统的“x4..x29 连续区”，而是：

| guest/pseudo | host |
|---|---|
| RAX, RCX, RDX, RBX | x4, x7, x5, x6 |
| RSP, RBP, RSI, RDI | x8, x9, x10, x11 |
| R8, R9, R10, R11 | x12, x13, x14, x15 |
| R12, R13, R14, R15 | x16, x17, x19, x29 |
| PF raw, AF raw | x26, x27 |
| XMM0–15 | v16–v31 |

证据：`FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.cpp:43-116`；普通非 ARM64EC 的 PF/AF 明确固定在 x26/x27，见 `Arm64Emitter.h:35-53`。

关键不在映射表本身，而在 FEX RA 把它建模为 `GPRFixed/FPRFixed`：

- `LoadRegister/StoreRegister/LoadPF/StorePF/LoadAF/StoreAF` 被解码为 fixed class，PF/AF 是 fixed GPR class 的最后两项（`RegisterAllocationPass.cpp:183-217`）。
- 反向 pass 记录 fixed affinity，检查中间 guest state 访问造成的 WAW/RAW hazard（`608-633`）。
- 正向 pass 在 fixed 访问点驱逐冲突值并搬移（`673-701`）。
- 同物理寄存器的 RA-only load/store/copy 可被删除（`766-775`）。

**移植判断：可移植但有前提。** 前提是本仓也把 guest fixed register 变成 RA 的一等 register class/affinity，而不是继续只在 translator 的 uniform map 上叠加更多 reserved descriptor。W60 已证明“只扩大静态 map、仍沿用当前线性扫描/动态池”净负。

### 2.2 动态池划分

FEX 的 x86-64 池为：

- 动态 GPR：x20–x24、x30、x18，共 7 个；前四个支持寄存器对分配（`Arm64Emitter.cpp:76-90`）。
- 动态 FPR：v2–v15，共 14 个；v0/v1 是 emitter 固定临时（`104-116`）。
- 静态 FPR：v16–v31，共 16 个。
- JIT 初始化时分别把动态/固定 GPR 和动态/固定 FPR 数量交给 RA（`JIT.cpp:627-633`）。

**移植判断：可移植但有前提。** 不能只复制“v16–v31 静态”这一半；必须同时给动态 FPR 保证至少 14 个、明确 fixed temp 的数量和 call/cold-edge clobber。我们当前开启 XMM static 后只有 12 个动态 FPR。

### 2.3 Spill：需要纠正的事实

FEX 的普通 RA spill 并不落 guest context：

- RA 每个 block 重置寄存器可用状态，spill 是 block-local（`RegisterAllocationPass.cpp:575-593`）。
- 选择 furthest-next-use，常量优先 rematerialize；spill slot 单调分配，源码仍有 “should colour spill slots” TODO（`280-345`）。
- JIT 入口按 `SpillSlots × MaxSpillSlotSize` 下调 `sp`，退出恢复（`JIT.cpp:794-813,1156-1169`）。
- `SpillRegister/FillRegister` 对 GPR/FPR 发射 `[sp + slot]` 的 `str/ldr`（`MemoryOps.cpp:371-547`）。

真正落 `CpuStateFrame::State` 的是 architectural SRA：NZCV 写 `State.flags[24]`，GPR 写 `State.gregs[]`，PF/AF 成对写 `pf_raw/af_raw`，XMM 写 `State.xmm`（`Arm64Emitter.cpp:681-779`），回填是对称路径（`781-890`）。

**移植判断：**

- “动态 spill 直落 context”：**不适用我们**，因为 FEX 本身不是这样，而本仓已经把动态 spill 放进 `State::spill_area`。
- “静态 architectural state 在退出/helper 边界按 mask 写 context”：**可移植，且本仓大体已有**；可借鉴的是更精细的 mask/ABI 分类，不是存储位置。

### 2.4 Helper ABI

FEX 同时保留普通 AAPCS64 和 `preserve_all` 两条路。其注释列出的 AArch64 `preserve_all` 约定为：

- caller-saved：x0–x8、x16–x18、x30，v0–v7；SVE256 时 v8–v31 的上 128 bit 也不保。
- callee-saved：x9–x15、x19–x29、低 128-bit v8–v31。

因此 FEX 的 preserve-all helper 前仍需：

- 把 7 个静态 GPR x4–x8/x16/x17 写回 context；
- 把动态 x18/x30 和 v2–v7 压 host stack；
- 128-bit 模式下不需保存静态 v16–v31；
- helper 返回后对称恢复。

见 `Arm64Emitter.h:203-233`、`Arm64Emitter.cpp:1025-1122`。这不是“任何 helper 都零保存”；普通 helper 仍走完整 `SpillStaticRegs + PushDynamicRegs`。FEX 通过 CMake 探测和 `FEXCORE_PRESERVE_ALL_ATTR` 标注其 fallback ABI 函数。

**移植判断：可移植但有严格前提。** 只适用于定义、所有声明、函数指针类型和 JIT call-site 都一致标注，且 hot helper 是 leaf 或整个向下调用图也采用相同 ABI。普通 C++/libc 调用会把成本移进被标注函数的 prologue。

### 2.5 Flags（NZCV、PF、AF）

FEX 的 PF/AF 是两个 fixed pseudo-GPR：IR 的 Load/StorePF/AF 参与 fixed affinity，映射相同时 move 可消失（`MemoryOps.cpp:160-220`）。PF/AF 在 context 中是相邻的 raw 32-bit 字段，边界成对 STP/LDP；host NZCV 则作为另一份静态状态在边界用 MRS/MSR 保存恢复（`Arm64Emitter.cpp:702-739,816-888`）。

**移植判断：表示思想可移植，两个物理 pin 不适用我们。** 我们已经用 x26 同时常驻 packed PF raw byte、AF bit 和已 materialize 的 NZCV bits；另占 x26/x27 式双寄存器并不会消掉 flags 算法，且 x27 已是 code cache 指针。

### 2.6 汇总移植矩阵

| FEX 机制 | 判定 | 前提/原因 |
|---|---|---|
| fixed GPR/FPR register class + affinity/coalescing | 可移植但有前提 | 必须重塑 RA、call clobber 和 scratch；不能只扩 translator map |
| 16 GPR 全驻留 | 可移植但有前提 | 先解决 helper、动态池和 partial-write bridge；W60 当前形态 −10.18% |
| 普通动态 spill 到 host stack | 不建议照搬 | 我们已有 State spill，换基址不减少 store/reload，反而增加 host-frame/异步边界审计 |
| architectural SRA 按 mask 写 context | 可移植 | 本仓 trampoline/host call 已有大部分基础；缺 helper-specific mask |
| `preserve_all` helper | 可移植但有前提 | 仅 leaf/闭合调用图；x0–x8 仍不保；ABI 类型必须端到端一致 |
| v16–v31 XMM + 14 动态 FPR + 2 fixed temp | 可移植但有前提 | 先把本仓 v11–v14 cold ABI 从全程保留改为 slow-edge 保存 |
| PF/AF 两个 fixed pin | 不适用原样移植 | 本仓 PF/AF 已在 x26；无两个空闲 GPR，算法收益也不足 |

## 3. 本仓到目标形态的模块差距清单

| 模块 | 当前形态 | 目标改动点 | 耦合/风险 |
|---|---|---|---|
| `register_alloc_pass.cpp` | function/block 线性扫描；fixed guest map 只是池外 reserved；每指令保留 scratch；spill slot recycling | 增加 fixed class/affinity、fixed access conflict eviction、同寄存器 load/store coalescing；call-site clobber mask；连续 FPR pair 约束或保留 fallback；输出 cold-edge live map | 最高风险；partial write、跨块 liveness、fault observation 都在这里交汇 |
| `reg_alloc.{h,cpp}` | `kMaxSpillSlots=64`；per-op scratch 3，峰值 GPR/FPR 5；fixed clobber x13 等 | 分离 value-pool、hot instruction scratch、cold-edge scratch、call clobber 四种契约；保留 spill reload headroom；计数每 op 的实际峰值 | 契约错一项会从性能问题变成 PANIC/错误码 |
| `jit_context.{h,cpp}` | spilled def 下一 TickIR 写 `State::spill_area`；use 按指令 memoized reload；无寄存器则 PANIC，但 RA verifier 应使其不可达 | 不必换 spill 位置；若释放 v11–v14，需要 slow-edge 保存/恢复 live FPR、并提供可证明的置换顺序；把 PANIC 继续作为断言而非回退策略 | cold edge 必须恢复 continuation 的所有 live SSA，不能只保 guest XMM |
| `EmitHostCall` | 保存当前 live caller GPR/FPR；x0–x9 static pin 强制并入；XMM static 会因 reserved mask 被当作 live Q 保存 | 给 Lambda/helper 增加 ABI/effect kind；按 ABI 的真实 clobber mask 求交；保留参数装载前快照、x29/x30、返回值槽；普通/间接函数必须回退 full AAPCS | 声明/定义/call-site 不一致是 ABI 错误；x0–x7 参数覆盖无法靠 preserve_all 消除 |
| helper 定义与构建系统 | 普通 C++ ABI；少数 asm/inline lowering | CMake/编译期探测；统一 attribute macro；禁止异常、RTTI unwind 和普通下游调用的 leaf 审计；对 asm helper 写显式 clobber contract | LTO、sanitizer、平台 Clang 差异都需独立门禁 |
| `trampolines.cpp` 入口/出口 | public `JitRun` 保存 host x19–x30、q8–q15；入口恢复 static uniforms；退出/CallHost 写回全部 static uniforms | public AAPCS 边界保持不变；若有 helper-specific state mask，不应污染总出口；新增 fixed class 后确保 descriptor 顺序和 STP/LDP 配对仍正确 | signal/fault/CallHost/XSAVE 是强观察点，不能延迟越过 |
| XMM 驻留 | v16–v31 static 默认 OFF；v11–v14 永久 scratch；XSAVE/XRSTOR 显式同步；AVX 上半仍在内存 | 先把 cold ABI 改成 slow-edge save；再恢复 v0–v15 动态池；helper ABI 分类；保留 TBL+TBX 非连续 fallback；静态低 128-bit 与 YMM high 的同步契约不变 | W13/W17 已三轮负；不能再做 top-N/per-unit eviction |
| flags 驻留 | x26 packed；NZCV lazy；PF raw byte bits 0–7；AF bit 26 | 若试验，只做 unit-local PF/AF source SSA 和 observation-time materialize；helper/terminal/fault 前合并；不新增全局 fixed pin | 与 W38/W47/W59 已有优化重叠，必须防双算收益 |
| `translator.cpp` pin map | level 2 默认 12 GPR，level 3 全 16；level 3 x0–x9 helper snapshot | 若重开全 pin，映射必须由 RA/ABI 联合设计，不应继续单独扩表；保留 env rollback | 当前 level 3 是明确负基线，不可原地翻默认 |

## 4. 机制 A：spill 直落 state 的独立净账

### 4.1 当前事实

本仓已经实现题目所描述的主要形态：

- `State::spill_area` 是 64 个对齐 u64 slot，位于 flexible uniform buffer 前（`source/runtime/backend/context.h:103-118`）。
- GPR/FPR spill 用 `[x28 + state_offset_spill_area + slot×8]` 的 LDR/STR；V128 占两个连续 slot（`jit_context.cpp:130-187`）。
- spilled def 的一次写回在下一条 `TickIR` 或 terminal 前完成；use 每条指令每值只 reload 一次（`189-199,648-665`）。
- slot 已按 live interval 回收；历史最高水位：AVX 3、饱和测试 28、block-mode x87 21，距离 64 尚远（`reg_alloc.h:94-121`）。

所以“块边界免 flush”也基本已经成立：没有一份寄存器 spill cache 需要在每个 block 尾全量刷回；只需把上一条 spilled def 的 pending 单次 store 落地。跨 unit 没有 SSA live value，不能靠换 spill 位置产生额外跨 unit 保持。

### 4.2 方案比较

| 方案 | 静态收益 | 新增边界成本 | 结论 |
|---|---:|---|---|
| A0 保持现状 | 基线 | 每个真正 spill 仍是 1 store + 每次重载 1 load；末指令最多补 pending store | 推荐 |
| A1 把固定上限从 64 提高 | 0 条 host 指令 | 每线程每 slot +8B；uniform buffer offset 后移但由 `offsetof` 派生 | 只有观测到高水位逼近 64 才做；这是可靠性改动，不是性能优化 |
| A2 按 unit 动态扩 State scratch | 0 条 host 指令 | 编译结果要携带 frame size；State 分配/复用、并发和 code cache ABI 增加元数据；大 offset 可能需地址 materialize | 当前 28/64 无必要 |
| A3 改落 `ThreadContext64` architectural fields | 表面上可复用 guest slot | arbitrary SSA 与 guest architectural value 无一一对应；helper/signal/fault 可观察到伪状态；partial write/alias 会破坏语义 | 否决 |
| A4 改成 FEX 式 host stack frame | 可能缩短某些 offset 编码，但当前 offset 可编码 | 每 unit 入口/出口调 `sp`；所有退出、异常、helper 嵌套和 unwind 路径必须还原；spill load/store 数不变 | 没有证据支持迁移 |

### 4.3 净账结论

本机制的可消除项为 **0**：存储位置不改变 RA 产生的 spill 数，也不改变 reload scratch 需求。当前 cap 也不是实测瓶颈。A1/A2 只有安全容量价值，预期墙钟收益为 **0%**；A3/A4 风险大于收益。数据充分，结论是**不作为性能阶段立项**。

## 5. 机制 B：helper ABI 自定义约定的独立净账

### 5.1 Clang 能力边界

官方 Clang 文档说明，AArch64 `preserve_all` 保留除 x0–x8、x16–x18 外的 GPR，并保留 v8–v31 的低 128 bit；参数和返回值仍按 C ABI。`preserve_most` 的 GPR 集相同，但不保 SIMD。两者目前都被标为 experimental，并建议 `preserve_all` 面向不再调用其它函数的 runtime leaf。参见 [Clang Attribute Reference](https://clang.llvm.org/docs/AttributeReference.html#preserve-all) 和 [AAPCS64](https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst)。

本机 Apple clang 17 探针结果：

- `preserve_some`：unknown attribute，被忽略；Clang 可用的是 `preserve_most`/`preserve_all`，没有“任选 x18–x29”这种源码级 calling convention。
- `preserve_all` leaf 只为实际 clobber 的寄存器保存；探针故意 clobber x9/x15/x19/x29/v8/v16/v31，生成 6 STP + 1 STR 及对称恢复。
- `preserve_all` 函数调用一个普通 C 函数时，生成 **480B frame、17 STP + 1 STR 及对称恢复**。
- `preserve_most` 调普通 C 函数为 **80B、4 STP + 1 STR**；`aarch64_vector_pcs` 为 **288B、10 STP**。

这验证了成本只会从 JIT caller 移进 helper callee，不会凭空消失。ABI 属性还必须在定义、声明和函数类型上匹配；间接函数或普通 C++ 互调不能靠 call-site 猜测。

### 5.2 对当前 pin map 的精确影响

| 配置 | 当前强制 snapshot | `preserve_all` 后仍须 snapshot | GPR 指令净减 |
|---|---:|---:|---:|
| pin level 2 | x0–x5：3 STP + 3 LDP | x0–x5 全部 | 0 |
| pin level 3 | x0–x9：5 STP + 5 LDP | x0–x8：通常 4 STP+1 STR 及对称恢复 | 通常仍是 5 store + 5 load；只少 8B 状态，不少指令 |
| XMM static | v16–v31：8 STP Q + 8 LDP Q | leaf preserve_all 可由 callee 保护 | caller 最多减 16 条内存指令/次 call；callee 成本取决于实际 clobber/下游调用 |

此外，x0–x7 是参数寄存器。即使 helper 本体不使用它们，`EmitHostCall` 的参数装载也会先覆盖其中的 guest pins，故不能取消旧值快照。若真要“pin snapshot 归零”，必须同时改变 helper 参数 ABI、guest pin map 和 C++ 入口方式；Clang 没有任意寄存器集合属性，最终只能靠汇编 thunk、LLVM backend 自定义 CC，或把参数放进固定 context。这已经是完整内部 ABI 重写，不是一个 attribute patch。

### 5.3 可落地的窄设计

若后续实施，IR/helper registry 至少需要四类：

1. `NormalAAPCS`：间接函数、可调用 libc/C++、可抛异常/进入 sanitizer 的 helper；保持当前保存集合。
2. `PreserveMostLeaf`：只改善部分动态/静态 GPR；对当前 level 2 pin 无收益，主要供未来 fixed-class map 评估。
3. `PreserveAllLeaf`：定义/声明/函数指针全标注，禁止普通下游调用；可保护 v16–v31，是 XMM static 的候选前置。
4. `AsmExact`：汇编 helper 明确列出 arg/result/clobber；JIT 按 exact mask 保存。它不允许再无声明地调用 C++。

每个 call site 的保存集合应为：`live ∩ abi_clobber`，再并入参数装载会覆盖的 static pins、x29/x30 和结果约束。不能只用一个全局“helper 都是自己编译的 C++”开关。

### 5.4 净账结论

- **GPR 全 pin：否定。** level 2 的机械收益为 0；level 3 通常连保存/恢复指令条数都不降。`x18–x29` 扩 callee-saved 不是有效方向。
- **XMM static：有条件成立。** 对每个 leaf helper，caller 理论上最多少 8 Q stores + 8 Q loads；但缺少当前各 benchmark 的动态 helper call 次数、call-site live FPR mask和 helper 下游调用图，无法换算墙钟收益。
- **数据不足项：** helper 动态调用频度、leaf 占比、实际 clobber 集、XMM static 下每 call 的 frame bytes。没有这些数据前，不给正向收益区间。

## 6. 机制 C：FPR 池重配 + XMM 默认驻留的独立净账

### 6.1 当前与 FEX 的真实预算差

| 形态 | XMM static | 固定 FPR scratch | 动态物理池 | 高压 op 保留后最多可承载 live value |
|---|---:|---:|---:|---:|
| 本仓 XMM OFF | 0 | v11–v14（4） | 28 | 峰值 scratch=5 时 23 |
| 本仓 XMM ON | v16–v31（16） | v11–v14（4） | v0–v10,v15（12） | 峰值 scratch=5 时 7 |
| FEX | v16–v31（16） | v0/v1（2） | v2–v15（14） | FEX 是不同 RA，不能直接套本仓 reserve 算法 |
| 设计目标 | v16–v31（16） | hot path 0；slow edge 临时借 4 | v0–v15（16） | 本仓峰值 scratch=5 时 11 |

W13 的“16 个静态 V 挤占动态池”描述需要补上这一层：当前并非还剩完整 v0–v15，而是 v11–v14 永久保留。W13 单开 XMM 的实测为 geomean 0.969、c-ray −26%；题面另给出 c-ray host +16.4%、向量栈流量 +256%。W17 的 top-N/XMM0–7 为 −0.9% 到 −9.0%，lazy-fill 很快摸满 16 XMM，per-unit 驱逐还出现 55 分钟 400% CPU 重翻译环路。这些结果共同否决再次搜索“部分 pin 甜点”。

### 6.2 释放 v11–v14 的可行设计

W37 的 cold handler 约定固定使用 v11–v14 保存两输入、硬件结果和 ordered mask，并用 x13 保存 continuation。它之所以全程保留四个 FPR，是 cold stub 在 block 末尾发射、返回到先前 site，不能随意破坏 continuation 的 live SSA。

可行替代不是删 scratch，而是**把保存成本搬到真正命中 NaN 的 slow edge**：

1. RA/JitContext 为每个 cold site 导出 v12–v15（或选定四个 ABI FPR）的 live/value map。
2. fast path 仍只有现有 NaN test + conditional branch，不加 store。
3. slow veneer 先把这四个物理寄存器中“continuation 仍 live 且不是结果”的值写入 `State` 的独立 cold scratch；先完成有环置换所需的源保存，再装入 cold ABI。
4. handler 修正结果；veneer 恢复 unrelated live value，结果寄存器写回，跳 continuation。
5. x13 continuation 契约保留；128-bit atomic 的 x12/x13 是 GPR 问题，与 FPR 池不冲突，但 fixed-clobber 表仍必须阻止重叠。

这是 cold-only 4×16B store/load 的最坏成本。W41 记录 smallpt 的 cold stub 三轮零命中，因此常见路径成本为零；NaN-heavy correctness 测试会变慢，但不应影响非 NaN benchmark。需要新增 64B–128B 独立 cold scratch，不能复用 architectural XMM 或普通 SSA spill slot，因为后者的生命周期/slot coloring不覆盖 block 末 cold stub。

### 6.3 三个已知爆点的逐项解法

1. **精确 NaN 修正**：用上述 slow-edge save/restore 释放永久 v11–v14；保持 x86 payload/SNaN/negative-indefinite bit exact，不依赖 `SVM_SSE_NAN_FAST`。必须覆盖 result 与 source alias、packed/scalar 32/64、多个 site 共享 handler和 continuation live value。
2. **TBL 连续寄存器对**：现有 `TryGetConsecutiveTmpV2` 已在有连续 pair 时用双表 TBL；碎片化时退成两个任意 scratch 的 `TBL(left)+TBX(right)`，语义等价（`translator_alu.cpp:1130-1158`）。保留此 fallback 即不会因 v0–v15 碎片化 PANIC；可选 pair-aware allocation 只作为码质优化，不是正确性前提。
3. **原子/128-bit scratch**：CAS128 的 observed pair 固定在 x12/x13，V128 结果通过 INS 组装；它消耗的是 GPR fixed clobber，不要求额外固定 FPR。需要验证的是 XMM static result/store coalescing和 fault/partial-commit，不应把 x12/x13 错算进 FPR 预算。

此外，当前每 op 峰值仍包括 scalar32 FP/VecFCvtPacked/VecFMulAdd 的 5 FPR，scalar64/crypto/SHA/SSE4.2 的 4 FPR。即使物理池恢复到 16，RA 在峰值 op 上也只能放 11 个 live value；因此必须采集“每指令 live FPR + scratch reserve + spill reload”直方图，不能只看池大小。

### 6.4 Helper 边界与净收益

XMM static 开启后，static v16–v31 作为 reserved bits 进入每条指令的 live FPR mask；当前 `EmitHostCall` 会保存所有这些 Q 寄存器。因此 XMM 默认驻留必须与第 5 节的 `PreserveAllLeaf`/exact-clobber helper 分类捆绑，否则 helper-heavy 单元每 call 新增最多 8 STP + 8 LDP，足以吃掉 uniform load/store 消除。

收益只能给历史上界：W41 的 XMM state 32.01% 对应 smallpt 理想最多 1.259×，但该分解早于 W57，且没有扣 helper和新增 spill。当前可实现收益 **数据不足，不能给正区间**。重新立项的硬门为：

- v0–v15 动态池后，XMM ON 的 FPR spill load/store 至少回落到 XMM OFF 的 1.25×以内；
- helper 动态边界新增内存指令净额不为正；
- smallpt/c-ray/STREAM/openssl GHASH 安静机交错 A/B geomean ≥1.03，任何单格不得低于 0.98；
- exact NaN、SHUFPS 1536 矩阵、XSAVE/XRSTOR、signal/fault、AVX low/high 同步全绿。

未过这些门，不翻默认，也不再尝试 W17 已否的 top-N、lazy-fill-only 或 per-unit eviction。

## 7. Flags 驻留扩展

### 7.1 当前表示不是“PF/AF 在内存”

本仓固定 x26 的布局为：N/Z/C/V 位 31–28，PF 原始结果字节在 0–7，AF 单 bit 在 26；host NZCV 是 lazy producer，`MergeNZCV` 才 MRS 并按 requested mask 合入 x26（`translator.h:32-52,265-273`；`translator_flags.cpp:10-25`）。

- 保存 PF：一条 `BFI x26, value, #0, #8`。
- 读取 PF：UBFX 后三次 xor-fold，再 AND/EOR 得 even parity。
- 保存 AF：`left ^ right ^ result` 后取 bit4，再 BFI x26。
- 读取 AF：一条 UBFX。

运行时入口/出口只对 x26 做一次 load/store。故 PF/AF 已经跨 unit 常驻在寄存器，并没有可由两个新 pin 消除的 state load/store。

### 7.2 另 pin PF/AF 的成本

- GPR 空间无合法双槽：x0–x9 是 level3 guest，x10 memory scratch，x11–x17 动态/implicit scratch，x18 Apple 保留，x19–x23/x29 guest，x24–x28 runtime，x30 LR。
- x25 只有关闭 RSB 时才空出一个，仍不够两个且会牺牲已验证的 RSB；x27 是 cache，不能照 FEX 使用。
- PF 的 xor-fold 与 AF 的 bit4 计算仍然存在；最多省少数 BFI/UBFX，却新增入口/出口/helper 保存、寄存器压力和与 packed flags 的同步。

因此全局双 pin 的预期收益为 **≤0，建议否决**。coremark “全部 flags”理想上界 1.332×不能分配给 PF/AF；PF/AF 子项的动态占比在 W38/W59 后没有独立计数，**数据不足，不能预估正收益**。

### 7.3 唯一值得保留的实验方向

做 unit-local `PFSource/AFSource` SSA：producer 保留 raw result/bit4 source，只有 LAHF/PUSHF、非 terminal consumer、helper、fault/terminal 观察点才 materialize 到 x26。它不占全局 fixed GPR，允许 RA 正常 spill，且与 W59 branch-only 的“证明全死则零物化”一致。

这必须用独立 env（建议 `SVM_FLAGS_PFAF_SOURCE=1`，默认 OFF），并先统计：PF/AF producer 数、直接 branch consumer 数、materialize 数、因 edge/helper/fault 被迫 materialize 数。若 `materialize/producer` 不能低于 0.5，不进入性能 A/B。

## 8. 分期路线图

| 期 | 内容与开关 | 独立/捆绑 | 收益区间与上限 | 风险 | 验收门 |
|---|---|---|---|---|---|
| P0 证据补齐 | 只读计数：spill high-water/读写、per-op FPR pressure、helper 动态次数/live mask/调用图、PF/AF materialize；建议总开关 `SVM_RA_SHAPE_PROF=1` | 独立 | 0%；不改变 codegen | 低 | OFF 字节指纹零变化；七语料拿到完整计数 |
| P1 spill 容量安全 | 仅当观测逼近 64，再做 `SVM_RA_SPILL_EXT=1` 或构建期容量实验 | 独立 | 预期 0%；上限 0%，只消除 PANIC | 低–中 | 高压合成用例 >64 不越界；普通语料 host bytes 不变 |
| P2 helper ABI 分类 | `SVM_HELPER_PRESERVE_ABI=1`，默认 OFF；只标 leaf，普通/间接回退 | 独立可测；是 P4 前置 | XMM OFF 时现有 pin GPR 上限 0；XMM ON 每 leaf call 静态上限减 16 条 Q 内存指令；墙钟数据不足 | 高 | 对每 helper 反汇编 prologue、符号类型/声明一致；禁止普通下游 call；sanitizer/LTO 两态 |
| P3 cold-edge FPR ABI | `SVM_FPR_COLD_SCRATCH=1`，默认 OFF；释放 v11–v14，动态池恢复 v0–v15 | 可独立于 XMM ON 验证；P4 前置 | 物理动态池 12→16（+33%）；峰值 reserve=5 时 value 容量 7→11（+57%）；墙钟数据不足 | 高 | NaN bit-exact、所有 alias/continuation live；FPR spill 不增；cold 零命中 fast bytes 不增或只在门槛内 |
| P4 XMM 全驻留重测 | 现有 `SVM_XMM_STATIC=1`，要求 P2+P3；默认 OFF | **必须捆绑 P2+P3** | smallpt 历史宽松上限 1.259×；当前预期数据不足。旧形态实测 geomean 0.969、c-ray −26% 是否决基线 | 很高 | 第 6.4 节四项门；W13/W17 负格全部翻正后才讨论默认 |
| P5 unit-local PF/AF source | `SVM_FLAGS_PFAF_SOURCE=1`，默认 OFF | 独立；不与全局 PF/AF pin 捆绑 | 全 flags 上限 1.332×仅供否定；PF/AF 当前可实现区间数据不足；W38 +4.86% 是已拿走收益参考 | 中–高 | materialize/producer <0.5；coremark/7zip 指令净减；跨 edge/fault/helper 全绿 |
| P6 fixed-class GPR RA spike | `SVM_RA_FIXED_CLASS=1` + 现有 `SVM_X86_PIN_EXT=3`，均默认 OFF | 必须在 P0/P2 证据充分后，与 scratch/call-clobber 重写捆绑 | coremark 理想上限 1.254×；当前实现实测 −10.18%，所以无正向预期区间 | 极高 | spill ops 不高于 level2 1.25×、host bytes 不增、helper边界净额≤0、coremark 安静机 ≥1.03；否则关闭轴 |

### 路线图裁定

- **可以独立落地**：P0；P1（仅安全需求）；P2 的 helper 分类基础；P3 的 cold-edge ABI；P5 的 unit-local source 实验。
- **必须捆绑**：XMM 默认驻留必须同时具备 P2 helper ABI 分类和 P3 动态池恢复；GPR fixed-class 必须与 call clobber/scratch 契约一起设计。
- **明确不做**：把 arbitrary SSA spill 写进 guest architectural fields；仅靠 `preserve_all` 翻 GPR level3；再次搜索部分 XMM pin/per-unit eviction；全局新增 PF/AF 两个 pin。

## 9. 验证矩阵

### 9.1 静态与计数门

- 每 unit：GPR/FPR pool、最大 live、scratch reserve、spill defs/loads/stores、高水位、连续 pair fallback 次数。
- 每 helper：ABI kind、动态 call 次数、caller save 指令/bytes、callee prologue/epilogue、普通下游 call 数。
- 每 flags unit：PF/AF producer、直接消费、materialize、edge/helper/fault 强制 materialize。
- host dump 对 CRC 热 unit、smallpt top units、c-ray、GHASH 做逐类指令归因；开关 OFF 必须 byte-identical。

### 9.2 正确性门

- `swift_test` OFF/单开/组合态；Unicorn fuzz；`func_tests` 三模式。
- exact NaN：scalar/packed 32/64、QNaN/SNaN、payload priority、negative indefinite、source/result alias、多 cold site。
- SHUFPS 256 imm × reg/mem/alias × JIT/interpreter；连续 TBL 和 TBX fallback 都强制覆盖。
- XSAVE/XSAVEC/XRSTOR、FXSAVE、AVX low/high、signal delivery/rt_sigreturn、host fault 恢复、PageFatal、CallHost。
- aligned/unaligned CAS128、原子 retry、partial commit；x87 helper fallback；SMC 与 JIT cache。
- helper ABI：直接/间接函数、普通 C++ 下游调用反例、asm helper clobber、sanitizer、LTO、Apple/Linux clang。

### 9.3 性能门

- 安静机交错 A/B，不使用受载单轮立项；至少 coremark、smallpt、c-ray、STREAM、7zip、openssl GHASH/AES。
- 同时报墙钟、host bytes、动态 spill memory ops、helper boundary memory ops，不能只报 uniform 消除数。
- 任何“收益上限”必须与新增边界项相减后再给预期；无法获得动态频度的项目明确标为数据不足。

## 10. 最终建议

1. 先做 P0 计数 spike；它是所有后续项目的共同门，不改变生产行为。
2. 把 P3“cold-edge 保存释放 v11–v14”作为唯一值得继续设计的 RA 基础设施；它直接回应 W13/W17 的确定性失败点。
3. P2 只面向经反汇编和调用图证明的 leaf helper；其价值主要是保护 XMM static，不是消灭 GPR pin snapshot。
4. 只有 P2+P3 同时过门，才重测现有 `SVM_XMM_STATIC=1`；在这之前维持默认 OFF。
5. PF/AF 只允许 unit-local source SSA 实验，不做全局双 pin。
6. GPR 全 fixed-class RA 暂不立项。若 P0 以后重开，必须以“spill ops ≤ level2 的 1.25×、helper 边界净额≤0”为前置，而不是以 35.1% 静态上限为理由。

本轮没有足够数据给出正向墙钟预估的项目：helper ABI、FPR pool 重配后的 XMM static、PF/AF source SSA。报告已分别列出缺失计数和进入 A/B 前的硬门；在这些数据补齐前，任何正收益数字都会违反 W30/W32/W17 的边界净账纪律。
