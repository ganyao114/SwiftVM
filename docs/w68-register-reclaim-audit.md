# host GPR 基建回收与全 pin 池预算审计

基线：指挥官给定的 `origin/master` 33bdf53 工作树。审计与验证期间没有执行任何
git 命令。

## 1. 裁决

**Phase A：GO；Phase B：本基线没有新的“便宜且风险隔离”候选，因此不落机制
代码、不新增开关。**

原因不是“没有寄存器可收”，而是任务预期的三类便宜回收已经存在于当前 master：

- x16/x17 已由 `SVM_JIT_SCRATCH_XPOOL` 纳入 value/scratch pool，并由逐指令
  VIXL scratch contract 防止隐式租用覆盖 live value；
- desktop Linux 的 x18 已按 host 条件化，零 spill unit 在池内，只有首轮发生
  spill 的 unit 才重跑并专用 x18；Darwin 仍保留 x18；
- x25、x24、x10 都已按实际功能条件保留：没有 RSB/local 时 x25 可用，identity
  时 x24 可用，没有 guest-address bias 时 x10 可用。

给这些现状再包一层默认 OFF 的同义开关，ON/OFF 不产生不同发码，不能构成有效
spike；把现有默认行为反向绑到新开关又会破坏 OFF 默认态 byte-identical。真正仍
可增加池容量的 x27、活动 RSB 下的 x25、以及“spilling unit 也把 x18 当 value”
均需要新增发码/溢出契约，不能归入便宜隔离项。

更正后的 full-pin（16 个 guest GPR 固定 home）池结论是：

| host / address 形态 | 首轮可见池 | 已 spill unit 重跑池 | 说明 |
|---|---:|---:|---|
| Linux desktop, identity | **10** | **9** | x10–x18 + x24；重跑时专用 x18 |
| Linux desktop, bias | **8** | **7** | x10=mem scratch、x24=pt |
| Darwin, identity | **9** | **9** | x18 平台保留 |
| Darwin, bias（本机实测形态） | **7** | **7** | x18、x10、x24 均保留 |

因此 fixed-class spike 的 pool 7 是 **Darwin + bias** 图，不是 Linux full-pin 的
物理上限。保持 x26/x28/x30 不动时，Linux identity 的严格结构上限是 **12**：
当前 10 + 关闭/迁出活动 RSB 的 x25 + unpin cache-base x27。所谓 13 还必须再迁出
x26、x28、x30 之一，或少 pin 一个 guest GPR；均超出本任务边界。

## 2. 逐寄存器总表

统一坐标与契约来源：

- full-pin 映射：`source/translator/x86/translator.cpp:47-104`，选择逻辑
  `:545-568`；
- JIT 固定别名：`source/runtime/backend/arm64/defines.h:25-82`；
- 基础保留图：`source/runtime/backend/arm64/trampolines.cpp:55-88,111-132`；
- helper caller-save 边界：
  `source/runtime/backend/arm64/jit/translator_control.cpp:174-230,297-306,317-367`；
- fixed-clobber 表：`source/runtime/backend/reg_alloc.cpp:193-301`；
- `SVM_JIT_SCRATCH_XPOOL` 逐 unit 收紧：
  `source/runtime/backend/reg_alloc.cpp:460-476`。

“硬约束”只表示不能在本 JIT ABI 下直接作为普通 value；AAPCS
caller/callee-save 本身不是“不可用”，但决定 helper 边界的保存成本。

| reg | 当前角色（当前坐标） | 类别 | 回收路径与代价 | 开关/裁决 |
|---|---|---|---|---|
| x0 | L2/L3 guest RSI；AAPCS arg/result | 自家 full-pin + ABI caller-clobber | 16-pin 目标内不可收；helper 必须 snapshot | 既有 `SVM_X86_PIN_EXT` |
| x1 | L2/L3 guest RDI；AAPCS arg1 | 同上 | 同 x0 | 同上 |
| x2 | L2/L3 guest R8；AAPCS arg2 | 同上 | 同 x0 | 同上 |
| x3 | L2/L3 guest R9；AAPCS arg3 | 同上 | 同 x0 | 同上 |
| x4 | L2/L3 guest R10；AAPCS arg4 | 同上 | 同 x0 | 同上 |
| x5 | L2/L3 guest R11；AAPCS arg5 | 同上 | 同 x0 | 同上 |
| x6 | L3 guest R12；L0–L2 为普通 pool | 自家 full-pin + ABI caller-clobber | 只有降低 pin level 才归还 | `SVM_X86_PIN_EXT=3` |
| x7 | L3 guest R13；L0–L2 为普通 pool | 同上 | 同 x6 | 同上 |
| x8 | L3 guest R14；L0–L2 为普通 pool | 同上 | 同 x6 | 同上 |
| x9 | L3 guest R15；非 L3 bias dispatcher 可作 `ip6` | 自家复用 + ABI caller-clobber | L3 下 dispatcher loc 已改用 x13，不能再收 guest home | `SVM_X86_PIN_EXT`; `trampolines.cpp:195-217` |
| x10 | `mem_scratch`/dispatcher `forward`；identity 下普通 pool；L3 bias 可窄租给纯 ALU | 自家可优化选择 | 当前已条件回收；活动 bias 时常态入 value pool 会重现 VOID store clobber 风险 | 无需新开关；`defines.h:59-69`; `reg_alloc.cpp:85-92`; `jit_context.cpp:1143-1151` |
| x11 | `ip`；terminal/call target、exclusive status；XPOOL 下普通 pool | 自家 fixed clobber | 已按 opcode/terminal 排除，不需全局保留 | `SVM_JIT_SCRATCH_XPOOL`; `reg_alloc.cpp:215-242` |
| x12 | atomic surviving value；CAS/x87 fixed clobber；其余为 pool | 自家 fixed clobber | 已逐 opcode 回收 | 同上；`reg_alloc.cpp:216-233,264-276` |
| x13 | CAS128 second observed、NaN cold link、L3 bias dispatcher loc；其余为 pool | 自家 fixed clobber | 已逐 opcode/路径回收 | 同上；`reg_alloc.cpp:223-258` |
| x14 | `ip2`，普通 value/scratch pool；RSB terminal 临时 code ptr | 自家可优化选择 | 已在池；无需再收 | XPOOL 不关闭它；`defines.h:77-80`; `jit_context.cpp:823-838` |
| x15 | `ip3`，普通 value/scratch pool | 自家可优化选择 | 已在池 | 无需新开关 |
| x16 | VIXL `ip0`、显式 helper/region scratch；XPOOL 下 value/scratch pool | ABI IP 名称 + 自家动态契约 | 已回收；每条 emission 只把非 dirty 的 x11–x17 交给 VIXL | `SVM_JIT_SCRATCH_XPOOL`; `jit_context.cpp:1155-1225` |
| x17 | VIXL `ip1`、显式 helper/region scratch；XPOOL 下 value/scratch pool | 同 x16 | 已回收 | 同 x16 |
| x18 | Darwin 平台保留；Linux desktop 零-spill unit 普通 pool、spilling unit reload/writeback scratch；Android 不启用 | 平台条件 + 自家 spill 正确性契约 | 当前便宜形态已经实现；要让 spilling unit 也分配 value，必须重写首枚 spill scratch/fail-closed 契约 | compile-time host 图；`trampolines.cpp:60-63`; `register_alloc_verified.inc:53-73`; `jit_context.cpp:405-423` |
| x19 | guest RSP fixed home | 自家 full-pin + ABI callee-save | 16-pin 内不可收 | `SVM_X86_PIN_EXT`；映射 `translator.cpp:61-104` |
| x20 | guest RBX fixed home | 同上 | 同 x19 | 同上 |
| x21 | guest RBP；legacy asm-interpreter `handle`（x86 JIT 路径互斥） | 同上 | 同 x19；不是空闲寄存器 | `defines.h:37-47` |
| x22 | L1+ guest RAX；legacy `arg` 路径互斥 | 同上 | 同 x19 | `SVM_X86_PIN_EXT>=1` |
| x23 | L1+ guest RCX；legacy `args` 路径互斥 | 同上 | 同 x19 | 同上 |
| x24 | bias 时 permanent pt；identity dispatcher 间作 `loc`，guest emission 内可入池 | 自家可优化选择 | 当前已条件回收；bias 下 unpin 会给每个 guest memory operand 增加 base 获取/活区间 | 由 `Config::memory_base/page_table` 决定；`trampolines.cpp:79-84,195-217` |
| x25 | 默认 RSB pointer；或 local buffer；两者都关闭时普通 pool | 自家可优化选择 | 当前已条件回收；活动 RSB 下 unpin 需每次 push/pop load/store pointer，且跨直接边保持一致 | 既有 RSB/global opts；`trampolines.cpp:76-87`; `jit_context.cpp:726-860` |
| x26 | packed guest flags 枢纽，PF raw byte + AF bit26 + NZCV committed bits | 自家 JIT ABI | W-β lazy/split 已封存；本任务不碰 | 固定保留；`translator_flags.cpp:52-121,183-212,591-652` |
| x27 | L2 code-cache base，runtime entry 一次加载，indirect/RSB terminal 使用 | 自家性能选择 | 可 unpin，代价是受影响边 reload；必须用“少 spill/bridge”抵消 reload 税 | 本任务只算账不实施；`trampolines.cpp:263-265`; `jit_context.cpp:823-833` |
| x28 | permanent `State*`，所有 State/uniform/fault/helper 路径的基址 | 自家 JIT ABI + ABI callee-save | 理论可改为按需传递/加载，但会污染几乎每类 emitter 和 fault contract，不是便宜回收 | 固定保留；`defines.h:25-36`; `region_link_trampoline.cpp:132-148` |
| x29 | L1+ guest RDX fixed home；helper/region slow path与 x30 成对保存；非 level1 仍不进动态池 | 自家 full-pin；不使用 host frame chain | 16-pin 内不可收；低 pin level 可另案归还，但会改变现有 trampoline/RA 图 | `SVM_X86_PIN_EXT>=1`; `trampolines.cpp:66-73,111-132`; `translator_control.cpp:174-177,306,395` |
| x30 | LR、dispatcher/block continuation、helper BLR 返回 | 架构/ABI | 只有全 JIT 改成显式 continuation spill/另一寄存器才能收，成本和风险高 | 固定保留；`trampolines.cpp:70-73`; `jit_context.cpp:712-723` |

x31/SP 不属于 x0–x30 枚举，但预算中必须排除；基础图在
`trampolines.cpp:65` 固定保留，不能作为普通 value/scratch。

## 3. x16/x17：全树隐式 scratch 审计

### 3.1 “已无任何隐式 AcquireX”不成立

当前树的 VIXL 默认 scratch list 仍是 `{x16,x17}`：
`source/runtime/externals/vixl/aarch64/macro-assembler-aarch64.cc:323-364`。
全树逐调用搜索（排除注释示例）仍有：

- 9 个 `AcquireX` 调用：同文件 `:1091,1132,2458,2471,2644,2692,2704,2712`
  以及 `macro-assembler-aarch64.h:4099`；
- 3 个 `AcquireW` 调用：`macro-assembler-aarch64.cc:980,1058,1596`。

所以可入池的证明不能写成“VIXL 不再隐式 AcquireX”。正确证明是：SwiftVM 在
`UseScratchRegisterScope::AcquireNextAvailable()` 内记录每次租用
(`macro-assembler-aarch64.cc:3035-3041`)，并由
`SvmBeginScratchContract/SvmEndScratchContract`
(`macro-assembler-aarch64.h:3520-3557`) 检查允许集合。

### 3.2 当前安全机理

`JitContext::BeginVixlScratch()` 在 XPOOL 开启时先清空 VIXL 原 scratch list，再只
放回本指令 `cur_dirty_gprs` 未占用的 x11–x17；显式 `GetTmpX()` 随即从 scope
排除该寄存器。结束时把显式 scratch 与 VIXL 实际租用相加并校验不超过 opcode
预算（`jit_context.cpp:1155-1232`）。RA 同时把 CAS、call、NaN cold、x87、terminal
的固定寄存器写进 per-instruction clobber mask
(`register_alloc_pass.cpp:1082-1121`)。

linker veneer、region-link slow trampoline、runtime helper/terminal 仍有显式 x16/x17
使用，例如 `region_link_trampoline.cpp:132-165`。这些路径或在 allocator unit
之外保存 static homes，或位于已 snapshot/fixed-clobber 的边界；它们不是未登记的
普通 emitter 覆盖。

**结论：x16/x17 已安全入 pool/scratch 梯，但依据是动态租用审计，不是隐式
ip0/ip1 已消失。** 本批无需新开关。

## 4. x18、x25/x28 与 x10 的定点结论

### 4.1 x18

当前 host 图已经是要求的 compile-time 双图：

- `__APPLE__` 在 trampoline 基础图直接 mark x18；
- desktop Linux 首轮不 mark x18；若该 unit 首轮 `SpillCount()!=0`，RA mark x18、
  清空映射并完整重跑；
- emission 对 spilling unit 断言 x18 已保留，首枚 scalar spill reload/writeback
  固定使用 x18；
- Android 不走该优化，避免 bionic/SCS 平台用途。

这比运行时 host 判定更干净：同一产物只有一张 ABI 图，Darwin 开发回路没有变化。
进一步“spilling unit 也保留 x18 给 value”不是便宜项，因为会直接破坏
`jit_context.cpp:31-34,405-423` 的 final reload scratch 保证。安全重开条件是先为
首枚 spill 建立另一根永不与 live value 重叠的 scratch，或让无法满足 reserve 的
unit fail-closed；不能只删 `ReserveGPRForUnit(18)`。

### 4.2 x25 与 x28 实际用途

x25 不是闲置 callee-save：默认 x86 config 在
`translator.cpp:590-599` 开启 `ReturnStackBuffer`，entry/exit 在
`trampolines.cpp:275-277,373-375,399-425` 同步 pointer，guest call/ret 在
`jit_context.cpp:740-860` 对它 pre-decrement/post-increment。关闭 RSB 且没有 local
operation 时基础 mask 已不 mark x25，因此“可收形态”已经实现；活动 RSB 下回收
会把每次 push/pop 的 pointer load/store 和跨边同步加入热路径。

x28 是 permanent `State*`，不仅是普通 context cache。runtime entry 把 x0 搬入 x28
(`trampolines.cpp:245-261`)，flags、memory、fault/signal、helper、region link 都以它
寻址；region link slow path还明确把它作为 helper x1
(`region_link_trampoline.cpp:132-148`)。回收 x28 等价于改 JIT ABI，不进入 Phase B。

### 4.3 x10

x10 的三个生命周期已经互斥：dispatcher 间的 `forward`、bias guest code 中的
`mem_scratch`、identity guest code 的普通 pool。level3+bias 仅对
Add/Sub/Or/Select/VecFCvt/Call 等白名单 lease x10 作 instruction scratch
(`reg_alloc.cpp:85-92`; `jit_context.cpp:1143-1151`)，不把它变成跨指令 value home。

常态化入池在 bias 模式不安全：`defines.h:60-68` 记录了 VOID Store 没有 value
interval、旧 dirty mask 可能把 live value 当空闲 scratch 的已知机理。x16/x17 的
解放增加的是受 RA/VIXL contract 管理的梯子，不会自动解除 x10 的 address ABI。

## 5. x27 unpin 权衡账

收益与成本必须按同一动态分母计算：

```text
Net(x27 unpin) = saved_spill + saved_bridge + saved_host_move
                 - cache_base_reload - extra_address_materialization
```

任务说明把 4f8c192 的数字概括为约 0.68% CoreMark。当前 checkout 中可核的
`w-beta-b0-report.md:138-159` 给出更细的原始账：cache-base reload 模型为
1,016,545,472 条，在该报告的 §7.2 分母上是 **1.726969% 新成本**；
**0.679715% 是 PF/AF + strict Merge savings 再扣全部新成本后的净下界**，不是
纯 reload 税。两种表述不能混用。

本报告据此采用更保守的门：即便按任务给定的 0.68% 当 reload 成本下界，只有当
“池 +1”的 RE=0 实测动态 spill/bridge 降幅严格大于该数，x27 unpin 才有净收益；
按 checkout B0 原始项裁决时门槛更高。Phase C 应同一进程逐 PC 对照，不可把
CoreMark fixed-class pool7 的 267×直接当成 pool+1 的收益。

## 6. 池上限终算

full-pin 固定 home 为：

```text
x0-x9  = RSI,RDI,R8,R9,R10,R11,R12,R13,R14,R15
x19-x23 = RSP,RBX,RBP,RAX,RCX
x29 = RDX
```

共 16 根。x0–x30 剩 15 根中，当前 JIT ABI 固定 x25(RSB)、x26(flags)、
x27(cache)、x28(State)、x30(LR)，留下 x10–x18 与 x24，共 10 根。这 10 已包含：

- x16/x17 的 XPOOL 回收；
- Linux 零-spill unit 的 x18；
- identity 下的 x10/x24。

候选阶梯：

| 配置 | Linux identity pool | 可行性/成本 |
|---|---:|---|
| 当前 full-pin | **10**（spilling unit 9） | 已有机制，无新增代码 |
| + x25（RSB 关闭） | 11（10） | 已有条件形态；默认 RSB 性能能力丢失 |
| + x27 unpin | 11（10） | cache reload 税，必须密度胜出 |
| + x25 + x27 | **12（11）** | 保持 x26/x28/x30 时的严格上限 |
| 再到 13 | 13（12） | 必须动 x26/x28/x30 或减少 pin；本任务禁止/否决 |

bias 形态从上述数字再减 x10、x24；Darwin 再减 x18。由此解释本机
Darwin+bias full-pin 为 7。

## 7. 活区间实测与“16 pin + 新池”spill 预估

所有密度 run 都显式 `SVM_REGION_EDGES=0`，并用 `env -u SVM_EXEC_PROF`；只开现成
`SVM_RA_SHAPE_PROF`，不发 guest counter。墙钟只记录正确完成，不作为性能裁决。

### 7.1 当前两端实测

CoreMark 命令：

```text
coremark_x64 0 0 0x66 150000 7 1 2000
```

两态 CRC 都是 `e9f5/e714/1fd7/8e3a/25b5`，均有
`Correct operation validated`。

| CoreMark | L2 / pool11 | L3 fixed / pool7 |
|---|---:|---:|
| units | 1,702 | 1,693 |
| spill units | 2 | 104 |
| spill defs / loads / stores | 2 / 2 / 2 | 319 / 557 / 290 |
| host bytes | 191,776 | 192,992 |
| max-live GPR | 0:34,1:158,2:615,3:628,4:161,5:77,6:20,7:7,9:2 | 0:34,1:178,2:626,3:611,4:140,5:75,6:20,7:7,9:2 |

SQLite 命令：`sqlite_speedtest_x64 --size 1 --testset main`。两态都执行到
`PRAGMA integrity_check`、`ANALYZE` 和 `TOTAL`，exit 0。

| SQLite | L2 / pool11 | L3 fixed / pool7 |
|---|---:|---:|
| units | 14,135 | 14,135 |
| spill units | 25 | 543 |
| spill defs / loads / stores | 136 / 148 / 132 | 4,933 / 6,709 / 4,609 |
| host bytes | 1,535,616 | 1,561,180 |
| max-live GPR（L3） | — | 0:345,1:1047,2:4792,3:6213,4:1195,5:414,6:89,7:12,8:4,9:22,12:2 |

pool7 的 104/543 spill-unit 恰好对应“默认 scratch reserve=3 后 max-live >4”的
直方图尾部，说明探针口径与 allocator 约束一致。

### 7.2 从实测活区间投影 Linux identity pool

默认普通 reserve 是 3。下表只做可审的 interval-pressure 下界，不把 peak excess
臆造为 spill load/store 次数；后者还依赖 use 数、fixed clobber 和 eviction。
Linux x18 机制只重跑首轮已经 spill 的 unit，所以重跑缩池不会把首轮未 spill 的
unit 追溯加入。

| 语料 / 首轮池 | 普通 value 容量 | 仅按 max-live 可证明至少触发 | x18 重跑池/普通容量 | 这些 unit 的 peak excess 总下界 |
|---|---:|---:|---:|---:|
| CoreMark / 10（当前 Linux identity） | 7 | **2**（max-live 9） | 9 / 6 | **6** |
| CoreMark / 11（回收 x25 或 x27） | 8 | **2** | 10 / 7 | **4** |
| CoreMark / 12（回收 x25+x27） | 9 | **0** | 无重跑 | **0** |
| SQLite / 10 | 7 | **28**（8:4, 9:22, 12:2） | 9 / 6 | **86** |
| SQLite / 11 | 8 | **24**（9:22, 12:2） | 10 / 7 | **54** |
| SQLite / 12 | 9 | **2**（12:2） | 11 / 8 | **8** |
| SQLite / 13（仅数学外推，非当前可达） | 10 | **2** | 12 / 9 | **6** |

少数 opcode 的 reserve 大于 3，现有 aggregate probe 没输出 `(max-live,reserve)`
联合分布；不能假装边际直方图已经给出精确 unit 数。实测 reserve 尾部是 CoreMark
18 个 unit（4:10, 5:8），SQLite 185 个（4:51, 5:87, 6:9, 7:38）。把这些 unit
全部按“可能额外触发”计入，得到保守区间：

| 语料 | pool10 | pool11 | pool12 | pool13（数学外推） |
|---|---:|---:|---:|---:|
| CoreMark spill-unit 预估区间 | **2–20** | **2–20** | **0–18** | 0–18 |
| SQLite spill-unit 预估区间 | **28–213** | **24–209** | **2–187** | 2–187 |

这就是当前可交付的、不拍脑袋的 spill 预估：Linux identity pool10 的 CoreMark
interval 下界是 2、SQLite 下界是 28，且即使用最悲观的边际组合也显著小于本机
pool7 实测的 104/543。pool12 能清除 CoreMark 的普通-reserve interval pressure，
并把 SQLite 的普通-reserve 压到 2 个超宽 unit；它不能在缺少联合分布时被宣称为
“确定零 spill”。精确 spill ops/动态 bridge 必须在 Phase C 用真实 pool 配置跑
allocator，不能从本表虚构。

## 8. Phase B 开关裁决

| 预期项 | 当前 master | 新开关裁决 |
|---|---|---|
| x16/x17 入 pool/scratch 梯 | 已由 `SVM_JIT_SCRATCH_XPOOL` 实现，默认 ON；L3 即使请求 OFF 也为正确性自动 ON (`reg_alloc.cpp:20-36`) | 不新增同义开关 |
| x18 host 条件化 | 已由 `__APPLE__` / desktop Linux / Android 编译图 + per-unit spill 重跑实现 | 不新增；“spilling unit 也入池”不是便宜项 |
| x25 | RSB/local 都未启用时已经在池 | 不新增；活动 RSB unpin 另案 |
| x28 | permanent State ABI | NO-GO |

因此本批没有 ON 态新发码，也没有合法的“每项 ON 密度”可跑。新增一个中央
`SVM_CONFIG_FIELDS` 项还会要求同步 `main_case.cpp` 的 env/FeatureSet 数量断言；该
文件又被本任务明确封锁。没有候选时绕过中央配置表或制造无效开关都不合理。

## 9. 门禁证据

因为 Phase B 没有机制代码，改前与交付源码完全相同；唯一 tracked 交付是本报告。

- clean rebuild：`cmake --build build --clean-first -j8`，exit 0；现有第三方/宏重定义
  warning 集合保留，**新增 warning 集合为空**；
- 默认全套件：209/209 cases，1,053,302 assertions，全绿；
- 五 gate：209 cases 中 207 pass / 2 fail，严格只有 4 个既有断言：

```text
xsave_test.cpp:467
xsave_test.cpp:468
xsave_test.cpp:843
xsave_test.cpp:1045
```

- 指纹串行、未使用 `--update`：
  - `SVM_FP_GOLDEN_PROFILE=darwin-flagm`：1,786 units / 11 guests，
    self-consistency OK，golden byte-identical；
  - `SVM_FLAGS_CFINV=0 SVM_FP_GOLDEN_PROFILE=darwin-noflagm`：同为
    1,786 units / 11 guests，self-consistency OK，golden byte-identical；
- CoreMark/SQLite 的 RE=0 shape run 正确完成，见 §7；没有启用 EXEC_PROF；
- 没有修改 `docs/`、golden、linker、harness、`svm_config.h`、
  `main_case.cpp` 或 x26/lazy-flags 代码。

## 10. 给 Phase C 的量化前置

Phase C 不应再以 pool7 的 267×/104-unit/543-unit 结果外推 Linux。放行前应具备：

1. 在 Linux desktop + identity 上直接确认 full-pin 首轮 `gpr_pool=10`，并分别输出
   x18 重跑前/后的 pool；
2. 若试 x27，单独默认 OFF gate，逐 PC 交付 cache reload 与 spill/bridge 动态净账；
3. 若试 x25，必须同时量化 RSB hit/miss、push/pop 指令和 dispatcher 回退，不能把
   “关 RSB”伪装成免费池；
4. P6 四门仍按真实配置裁决：spill ops、helper boundary、host bytes、动态
   move/bridge；本报告的 interval 表只能筛配置，不能替代实测。

在这些前置下，首个值得复测的配置是 **16 pin + Linux identity pool10**；pool11/12
分别是带一个/两个有成本结构迁移的后续臂，不是本批可默认合入的“便宜回收”。
