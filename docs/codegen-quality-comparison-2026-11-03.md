# 重建框架初始基线与 FEX 的质量对比

本文件封存优化开始前的 formatter 检查点；表格不是当前实现的最新结果。后续各批源码、镜像与测量见[实施进度](codegen-parity-progress-2026-11-03.md)。

2026-11-03，原版 Android Bionic/CoreMark，红魔 NX729J。生产代码保持最终 formatter 检查点不变。本轮只做代码审查、一次三模式定向采样和少量离线单 unit 重编译定位，没有实施优化或重跑测试矩阵。

## 1. 结论与总量

当前主要差距是 **flags 过早实化及死写保留、Block 终结边过重、窄操作数和结果位置的选码开销**。当前 CoreMark 的证据不支持优先重建 spill/分配修正框架。Function 已明显改善动态代码质量，仍与 FEX 有约一倍的指令差距。

主基线沿用已完成的 100000 次有效长跑；三个实际 workload PID 均核到 CPU4，CRC 相同、运行超过 10 秒并通过 CoreMark validation：

| 指标 | SVM Block | SVM Function | 固定 FEX |
| --- | ---: | ---: | ---: |
| Guest 计时 | 36.943 s | 29.236 s | 15.093 s |
| 耗时 / FEX | 2.448× | 1.937× | 1× |
| 全 child 用户态指令 | 170.722 G | 128.624 G | 61.982 G |
| 指令 / FEX | 2.754× | 2.075× | 1× |
| 全 child 用户态周期 | 42.344 G | 33.554 G | 17.344 G |
| IPC | 4.032 | 3.833 | 3.574 |

Function 相比 Block 少执行约 24.7% 指令，耗时减少约 20.9%。SVM 的额外指令中有很多寄存器移动、位操作和标志维护，平均 IPC 更高，所以指令倍数高于耗时倍数；IPC 高本身没有消除额外工作。

计数包含 child setup 和 cold JIT。最终 compile 去纯字节解析仍是 37.163/67.080/FEX47.231 ms；后续质量优化继续保留两 scope 各不超过 2× 的 compile 目标。

## 2. 本轮采样与真实代码对应

候选 runner SHA 为 `3e32e04a97ad3635e2a288aa874fd3ed344c5db4443a5127141bacf98fe9bc1e`，源摘要 `0aaf165c7df7c9c099b499ed520ed719629b3140e6703951b1f054cd94a6e8a3`。原 guest SHA 为 `d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba`。FEX 固定 `a156752fae3a4f65fefb1ed95af980bcdfac43d7` 与原 Android 补丁、TSO/cache 配置。

本轮 Block/Function/FEX 各采一次 30000 次运行，`cpu-cycles:u`、4000 Hz，无 decode 时钟或 dispatch 命中计数。实际 PID 的 exe 与 CPU4 均核对；频率 min/max/cur 前后为 614400/1171200/1171200 kHz，电池 26.7°C，没有锁频。三个 final CRC 均为 `0x5275`。此次只用于定位；100000 次有效 stat 仍是上表的性能来源。

按实际 workload PID 和 sample.period 加权，样本为 48199/37621/19633；record 均报告 lost=0。SVM 两份 RX 各 1 MiB，FEX 只抓具体命名与 dispatcher 覆盖的约 252 KiB 页面，不抓整个预留区。快照为运行中顺序读取，非原子；本次采到的 JIT word 都有对应字节并成功解码。采样 IP 可承接相邻指令依赖停顿，以下百分比是代码形态权重，不能直接当作可删除时间。

| 代码形态，占各自 PID period | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| flags carrier 与 PSTATE 指令形态 | 13.019% | 16.715% | 3.460% |
| MOV/截取/插入/扩展形态 | 28.994% | 23.420% | 13.481% |
| 共享 dispatcher / FEX 内联查表及返回预测 | 14.212% | 19.473% | 16.919% |
| 直接 SP 栈访存形态 | 0.060% | 0.069% | 未采到 |

FEX 的主要查表工作在 JIT 内联代码中。将实际 `LDR → UBFIZ → ADD → LDP → SUB → CBZ → BR/RET` 及返回预测归集后为 16.919%。不能拿 SVM 的 19.473% 与 FEX 独立 dispatcher 的零样本对比；这两个份额也不表示两边绝对周期相同。

没有增加生产 PC-map 钩子。现有 host 静态库离线重编 selected unit 后，只排除编译器明确返回的地址 DATA relocation，保留所有 opcode，比对实际 RX：

| Guest 入口 / scope | 实际 RX unit 起点 | 全 unit 指令匹配 |
| --- | --- | --- |
| `0x219700` / Function | `0x7318544110` | 842 words，全部非地址 DATA 字节一致 |
| `0x2197e0` / Block | `0x793e088f00` | 28 words，全部非地址 DATA 字节一致 |
| `0x21b45e` / Function | `0x7318548830` | 412 words，全部非地址 DATA 字节一致 |

`0x21ab30` 的独立重编译未取得全 unit 匹配，未用它命名 SVM 热代码。矩阵热点使用已匹配的 `0x21b45e` continuation。FEX 则从实际 4B header、40B tail 和 vl64pair RIP anchors 恢复区域；重叠 perf-map 名称只用于选择抓取页面。

## 3. 同一条链表反转循环：Block 18，Function 11，FEX 7 条

原 guest `0x2197e0..0x2197f2` 位于 `core_bench_list`，六条 x86 指令完成指针反转及非零回环。下列数量是已链接、非零回边命中的单次动态路径，含有序访存恢复槽：

| 路径 | A64 指令数 | 实际组成 |
| --- | ---: | --- |
| SVM Block | 18 | 数据/有序访存、TEST 全旗、CSET+COPY+CBZ、PC 常量与写回、LDR literal+BR |
| SVM Function | 11 | 数据/有序访存、TEST 全旗、B.NE |
| FEX | 7 | 数据/有序访存、CBNZ |

Function 的实际热段 `RX+0x41308..0x41334`：

```text
MOV current, next
LDAPUR next, [current]
NOP
NOP
STLUR previous, [current]
ANDS tmp, next, next
BFXIL flags, tmp, #0, #8
MOV rawAF, XZR
CFINV
MOV previous, current
B.NE loop
```

FEX `0x7a503bc488..0x7a503bc4a4` 保留相同数据/访存序列，以 CBNZ 回环。Block 对同一条件另外物化 bool，准备/写 PC，再通过已修好的 DATA 指针跳回本块；这一热路径没有回 C++，也没有探共享 hash。

Block 的七条额外终结边工作是明确的结构差距。可以沿现 PC-tail 消费真实条件、收敛热 PC 发布、支持范围内近分支，保留原链接与撤链 owner；无须恢复 MIR、actions 或证明表。Function 内部边已经绕过这些工作。

### 四条 TEST flags 差异涉及现有正确性合同

FEX RFCE 不把普通可能 fault 的 load/store 当作 flags 观察点，正常后继无需求时把 TEST/Jcc 化为 CBNZ。其 JIT 信号恢复读取当时 PSTATE/PF/AF；RIP delta 表恢复 PC，不重新计算被删除 TEST 的 flags。因此下一轮读坏指针时，不能从该优化推导它仍提供 TEST 后的精确 flags。

SVM `execution/fault.cpp:194` 从真实 PSTATE、x1、x29 还原当前 flags；现合同保访存 fault 处的状态。这里直接删除另外四条会破坏这个合同。先保现语义，不能将“FEX 少四条”全部算作 SVM 可直接删除的浪费。

## 4. 矩阵热点存在可安全删除的死 flags 链

原 guest `0x21b511` 起执行 `IMUL → MOV → SHR 2 → AND 15 → SHR 5 → AND 127 → IMUL → ADD`。两次真正访存之间没有 EFLAGS 消费。SVM 已匹配 unit 的 fault metadata 将前一 load 精确定位到 `0x21b50b`；FEX 对应 RIP anchor 同为该 PC。

SVM `0x7318548900` 的真正低32位 MUL 后，还发出：

```text
SXTW a, left; SXTW b, right; MUL wide, a, b
SXTW expected, low; EOR different, wide, expected
CLZ predicate, different; UBFX predicate, predicate, #6, #1
EOR predicate, predicate, #1
RMIF ...CF; CFINV; RMIF ...OF
```

**这 11 条仅计算/发布 IMUL 的 CF/OF，随后在没有 fault、分支或 flags 读的区间内被 AND 覆盖。**FEX 对应位置直接保留低32位 MUL，再执行数据移位/AND，没有这条溢出链。

根因已落到具体实现：

1. `frontend/x86/build_ir.cpp:1953` 对 shift/rotate 无条件 `pending.Flush()`，触发 `PublishMultiply:886`，提前出生宽乘法与 SetCarry/SetOverflow。
2. 后继 AND 会覆盖新的 pending recipe，却无法撤回已发布的 MUL 写旗。
3. `compile/optimize.cpp:19` 的 Observable 将 metadata / WriteFlags 一律当作 live root；原 DCE 只看到 Value use，不能判断 flags 已被覆盖。

修复应先在已有首个逆向 walk 中做**块内** flags 覆盖消除：块出口 needed=All；MayFault、外部、未知效果及真实 flags 读保观察边界；只处理已定义的 metadata mask 与具体 setter。死 metadata 原位清除，已证明死的 Void setter 明确改为 Nop，再由原 DCE 删除其输入计算。维持当前三轮和一张 u64/N 表，只需每块一个 flags-needed 标量，不加 CFG flags pass 或逐节点事实表。

不能简单删 shift 前的 Flush：count=0、部分写位、AF/OF 保留策略仍需保真。这一项与上一节跨 fault 的 TEST 删除不同，它有明确的无观察覆盖区间。

## 5. 后端仍有低复杂度选码空间

| 当前形态 | 可收敛处 | 边界 |
| --- | --- | --- |
| 32位 view 先 UBFX/MOV，再 ADD X LSL | 真实地址消费者直接 ADD/SUB X,W,UXTW | 只消费现 view；架构 pin 的必要归零写保留 |
| U32 CondSelect 用 CSEL X 后 MOV W | 直接 CSEL W，按32位读口取值 | 8/16位及 raw 宽读保原合同 |
| 窄 Add/Sub 分别截取、左移两输入 | shifted RHS 的 ADDS/SUBS W，保结果提取 | AF/PF、carry profile 和覆盖前的输入仍正确 |
| dispatcher CMP+B.NE，热路径 MRS/MSR | 非 S SUB+CBNZ，可免两次 NZCV 访问 | cold publication 在自身改 flags 前取到原值 |
| 已 linked 的 LDR literal+BR | 近范围直接 B，远目标留原 thunk | 原 SMC 撤链、W^X、I-cache 发布归原 owner |

SVM 现有满 bank 首输入 last-use 复用已经存在。固定 FEX Android 的动态 GPR 池同样为六个，不能把当前差距归结为它有更大的临时寄存器池。SVM 固定 Home 确实可能使被选 spill 的值从定义起 STR、每 use LDR；FEX 可在压力点 spill、fill 后 remap。但当前热点没有给出优先重建这一体系的依据。

## 6. 不应删掉的共有成本及静态大小口径

双方 LRCPC2 标量读均有 `LDAPUR+NOP`，写均有 `NOP+STLUR`；这些槽供实际未对齐 half-barrier 回补。Vector TSO 的读后/写前 barrier 也存在。FEX 的 NOP period 更高，结合最热位置可以看到访存依赖的采样偏移，不能将 NOP 的采样权重当作删指令收益。

同轮 30000 次诊断的已发布 RX payload：Block 261664 B，Function 467948 B，FEX 350384 B。FEX 从 final perf-map 去除重叠、还原255个 fragment，216个已抓 header/tail 大小逐一相符。此 payload 含 FEX 内嵌 header/tail/RIP 表及双方 DATA；SVM 外置 fault/link 元数据不在这个数字内。它描述发布布局与区域覆盖，动态质量仍按真实热路径与执行指令判断，不能按 mmap 的预留大小排名。

## 7. 建议推进顺序

1. **现三轮内消除无观察区间的死 flags**，优先退役已证实的 MUL 溢出链，保全部 fault 边界。
2. **收敛 Block 条件终结及窄消费者选码**，减少 bool、截取和结果拷贝；保 Block/Function 独立分配。
3. **局部收敛 dispatcher，再处理热 PC 发布/near link**。不增第二缓存、链接 manager 或中间计划层。
4. 其它压力型工作负载出现真实热点后再决定 spill 改造；不以静态槽数先建 split-live/修正框架。

先完成一个连贯功能批，再集中看选定应用的质量和 compile 端点。此次未实施上述改动，不宣称预计节省等同于实测加速。

本轮证据与工具位于 `/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-compare-20261103`：原 `.data`、三份 RX、精确身份、period 汇总、FEX 内联查表、RIP anchors、单 unit 字节匹配、源码审查均保留。手机本线 namespace 与唯一自建 perf-map 已清理，剩余为零；Data2 只回收了已确认闲置、可再生成的本线构建 `.o`，代码/ELF/唯一结果均保留。
