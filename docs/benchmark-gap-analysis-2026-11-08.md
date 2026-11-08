# benchmark 与固定 FEX 的差距分解（2026-11-08）

本分析基于[本轮8项benchmark](post-l1-benchmark-2026-11-08.md)的144条有效主样本、6条FP父版本对照，以及同一产品二进制的126条热态派发样本。没有修改产品代码、重新跑全矩阵或更换输入；本轮新增了一次只读设备auxv核对。SVM仍为`cbd06547`加尚未提交的线程L1/L2实现，FEX仍为固定Android `a156752f`。

差距分为几种不同成本：FP异常状态的逐指令实现、可写代码的单元形成限制、首次发布与完整回收、CALL/RET语义信息，以及尚未量化的热循环布局/依赖。不能用同一个“IR太慢”或“dispatcher太慢”解释。

## 1. packed FP：额外指令的来源已基本闭合

实测Block/Function分别为FEX的3.391×/3.378×cycles、6.136×/6.140×instructions。无解释器回退，编译诊断spill=0；相邻父版本与L1版本的独立FP对照仅差0.026%cycles。这项差距在L1改造前已存在。

红魔实际`AT_HWCAP2=0x26181`，AFP bit20=false。默认MXCSR为0x1f80，实际软件专用域为DAZ0/FTZ0。因此本例没有逐op读取MXCSR，也没有逐op执行软件归零，当前主要成本是denormal/NaN分类和比较状态修正。

guest的4,000,000轮热循环包含14条packed加乘和4条packed LT比较。按当前正常输入路径计数：

| guest操作 | 每次额外host指令 | 每轮次数 | 每轮额外 |
| --- | ---: | ---: | ---: |
| packed double加乘 | 12：两个operand各两lane的UMOV/UBFX/CBNZ | 8 | 96 |
| packed single加乘 | 12：两个operand各一次指数reduction及跳转 | 6 | 72 |
| CMPLTPD | 30：12条DE分类、16条NaN分类、2条FPSR保存恢复 | 2 | 60 |
| CMPLTPS | 46：12条DE分类、32条NaN分类、2条FPSR保存恢复 | 2 | 92 |
| 合计 | | | **320** |

PMU相减为Block每轮多319.222条、Function多319.455条，与320条的源码正常路径模型相差不足0.25%。这基本锁定了额外instructions的主来源；它仍不是逐周期或所有输入的动态轨迹分解。

直接位置：[`RecordDenormalInput`](../source/runtime/backend/arm64/emit_float.cpp:33)、[`RecordNaNInput`](../source/runtime/backend/arm64/emit_float.cpp:188)、[`HandleVecFAddFamily`](../source/runtime/backend/arm64/emit_float.cpp:372)、[`HandleVecFCmpMaskFamily`](../source/runtime/backend/arm64/emit_float.cpp:524)。异常分类对两个operand分别执行，比较随后再次遍历NaN并保存/恢复FPSR。

固定FEX的对应packed算术和LT通常各发一条NEON指令，但其`GetMXCSR`/`RestoreMXCSRState`显式屏蔽低6位sticky异常状态，STMXCSR也使用这一结果。本benchmark只校验数值checksum，不校验MXCSR，两边checksum相同不能证明这个状态合同相同。不能直接删除SVM的异常语义来追平该版本。

保留语义的优化顺序：

1. **按predicate精确匹配host比较的异常行为。** 本循环的legacy LT是signaling；若对应profile的host比较行为匹配，可让native比较直接维护IE，省去重复NaN分类及FPSR保存恢复。该循环的源码上限约可省104条/轮，即416M instructions；这不是已实现收益或cycles承诺。quiet、复合predicate及不同AFP profile仍要独立处理。
2. **将同一个consumer的两个operand DE检查合并为normal入口和rare修正。** rare路径保留同lane NaN/invalid优先级及sticky状态；正常路径避免double逐lane搬到GPR、single两次独立reduction。
3. **共用DE/NaN所需的指数分类结果，利用确知常量。** 未知内存输入和运算结果不能仅因本benchmark通常normal就省略检查。不增加第二份IR、全局证明表或每条指令的新pass。

AFP=true也不自动解决问题：当前代码仍可能逐operand读取FPCR.FIZ，compare仍有分类/状态操作。它需要按真实后端能力和现有FP控制边界单独优化。

6.14×instructions只对应3.39×cycles：由同组计数计算，SVM的IPC更高，新增大量整数分类和可预测跳转有机会与原FP依赖链重叠。具体端口、reduction、SIMD→GPR和系统寄存器延迟份额需要profile，不能按320条乘固定单指令周期。

## 2. 5个tiny负载：Function实际上受SMC单块形成策略限制

固定`bench_suite_x86_64`只有一个PT_LOAD，flags=7（RWX），file size6272B，memory size4259840B。int/fp/mem/branch/call均使用它。

SVM在[`BuildFrontend`](../source/runtime/execution/frontend_service.cpp:16)中只对不可变源形成Function region；可写代码保留Function分配方式，但只形成一个source block。数据完全对应：

| 用例 | Block编译单元数 | Function请求数 / SMC单块形成次数 | FEX实际生成IR次数 |
| --- | ---: | ---: | ---: |
| int | 39 | 38 / 38 | 3 |
| fp | 43 | 42 / 42 | 3 |
| mem | 46 | 45 / 45 | 3 |
| branch | 39 | 38 / 38 | 3 |
| call | 46 | 45 / 45 | 7 |

FEX保留默认multiblock；它的CompileBlock可含多个基本块。这些数字不是Function allocator和Block allocator等价，也不是Function优化无效，更不是这些循环反复发生SMC。其实际SMC fault/失效为0；限制发生在形成时。

因此SVM承担更多独立entry/terminal、pass启动、分配启动、单元复制/ICache同步、目录和静态链接登记。tiny的publication约0.31–0.40ms，FEX约0.07–0.11ms；该段内没有细分每个系统调用，不能把它全部算成mmap。

处理方向应分开：

- 基准诊断：增加独立的RX代码/RW数据输入，保留原RWX成绩及SHA，才可观察正常Function region的潜力。
- 产品能力：若要在可写代码中形成多块region，必须给整个源范围保留页依赖、写监视、入口撤销和冷退休合同。不能简单删掉`!mutable_entry`。
- 保持现有Block/Function分配两条路径；形成策略与分配scope是两个维度。

## 3. x87 compile：发布和owner固定成本占比高，后端并非主要差额

x87输入为RX，单块SMC计数为0。SVM Block生成3个unit，Function生成1个unit；分别只有51/50个IR node，native约1.86/1.73KiB，spill=0。FEX实际生成1次IR。

下表为各自**总量中位数对应的真实样本**：Block round1、Function round0、FEX round1。使用同一条样本的互斥阶段，避免把各阶段独立中位数拼成不存在的总量。单位µs：

| 阶段 | SVM Block | SVM Function | FEX |
| --- | ---: | ---: | ---: |
| formation/IR，排byte parser | 237.6 | 229.6 | 114.8 |
| IR优化 | 81.9 | 55.4 | 41.4 |
| 分配+发码 | 125.8 | 87.5 | 118.4 |
| transfer/publication | 138.6 | 110.6 | 15.9 |
| SVM完整owner回收 / FEX逻辑缓冲释放 | 145.6 | 134.4 | 2.4 |
| 其余已计窗口 | 52.9 | 49.5 | 5.0 |
| 合计 | **782.4** | **667.0** | **298.0** |

Function后端三轮都低于FEX。总差额主要落在前端、首次发布和回收；仅优化allocator/emit不能根治这一项。

### 测量端点必须单列

SVM的[`Close`](../source/runtime/execution/runtime.cpp:107)计入cache、两个L1映射、compiler工作缓冲和native heap释放。FEX现有probe只计decoder/IR/temp allocator的logical buffer release，最终native arena/lookup cache析构不在其中，部分初始化也在CompileBlock根窗口之前。

Function所选样本中，完整release与FEX logical reset的账面差为约132µs，占所示总差约36%；这不是“FEX物理释放只需2µs”的证据。应补齐FEX对应生命周期再比较，不能把SVM的工作移出计时来达标。即便仅作诊断扣去SVM release，前端和首次发布仍有真实的已测差额。

首次发布的实际路径是[`CodeHeap::Publish`](../source/runtime/execution/code_heap.cpp:53)与[`MemMap::MapNative`](../source/runtime/execution/mem_map.cpp:17)：第一个unit按至少1MiB code slab建立双视图，复制代码、处理静态link、同步ICache，再登记共享目录和ready entry。对只有约1.7KiB代码的输入，这些固定工作占比较大。mapping大小是虚拟容量，不能称为分配并清零了1MiB物理内存。

另外，不能据本轮数据重讲已经不存在的成本：

- 这些tiny都只有一个dual-mapped slab，`SetWritable/SetExecutable`不会逐unit执行mprotect。
- L1是两个按页需求分配的匿名映射，没有启动时遍历清零32MiB。
- Program、optimizer rows、allocation homes及output已有容量复用，不存在每IR节点单独堆对象的旧结构。
- 所选x87 Function的other共49.5µs，其中约34.8µs已归fault service注册/恢复，剩余包含其他初始化；不能再虚构0.1ms的L1初始化子项。

优化顺序应是：对齐完整生命周期观察、缩减首单元真实固定工作、收敛同一static link/PC的重复查找和登记，然后再按实测热点减少局部容器增长或SMC登记。保留W^X、代码所有权和失效正确性。

## 4. CoreMark：运行剩余差距与compile墙钟波动分开

CoreMark为正常region形成，Function单块SMC计数为0。运行时Block/Function cycles为FEX的1.067×/1.023×，instructions为1.018×/0.989×。Function已经用更少总指令完成，但cycles仍略多，说明剩余差距不能只按指令总量解释。

同轮branch misses：Block约43.46M，Function约20.45M，FEX约18.66M。它支持继续审查内部边、静态循环、布局与返回预测；不能直接将miss差乘一个固定代价来宣称解释了全部cycles差额。具体依赖链和cache stall尚无硬件采样份额。

compile的Function三次为84.281/66.602/49.003ms（均排byte parser）；相同工作量下，优化阶段为27.533/27.017/10.663ms，formation也有33.603/16.935/16.364ms的跨度。不能把某个中位样本中27ms的opt全部视为稳定算法成本，也不能把前面线程CPU诊断替换成本轮正式墙钟成绩。

静态工作量确有差别：Block/Function的IR node为35383/50739，生成guest指令计数为11761/16449，Function生成更多静态内容；请求数从2053降至676。额外region覆盖、较大图的优化/分配工作都应计入。仅凭计数不能证明重复编译；要进一步区分有益覆盖、冷分支和重叠，需要实际region/PC工作账本。

## 5. mem与CALL：小幅cycles差不能按额外指令等比例归因

mem的instructions多约25.7%，cycles多约5%；CALL的instructions多约21.5%，cycles多约5.5%。两者实际Function请求均为45次SMC单块形成，因此本轮不能用它们证明正常Function region的收益上限。

两端请求的TSO策略已核实：实际environment显式设置`TSOAUTOMIGRATION=0`、scalar/vector/memcpyset/halfbarrier TSO为1，不能用FEX默认配置解释差距。相应scalar TSO分支都能用LDAPR/LDAPUR/STLR类指令，FEX非byte某些快路径还预留NOP，不能笼统说SVM每次多一个屏障。

mem的连续写、stride读改写、依赖pointer chase可使部分额外整数工作与等待重叠；具体地址构造、宽度转换、flags/fault状态维护仍是候选，尚未逐热PC量化。此输入compile只有约2.4ms、完整运行约2.16s，也没有重复SMC，compile不是这项运行5%差距的主要解释。

CALL有明确结构差异：SVM前端保留真实guest栈读写后，以普通Dispatch表示CALL/RET；后端看不到Call/Return提示，RET也走L1+BR。FEX保留控制流提示、guest/host return pair，并在匹配时用native RET。可参考[`semantics_control.inc`](../source/runtime/frontend/x86/semantics_control.inc:313)和[`emit_dispatch.cpp`](../source/runtime/backend/arm64/emit_dispatch.cpp:75)。

专题direct CALL/RET为SVM36条、FEX28条，间接单态为43/34条；纯间接JMP的L1已接近或优于FEX。但共享RET配对样例中SVM Block也可快于FEX，所以不能把全部CALL差额都归给缺shadow return。

合理方向是保留Uniform和真实guest栈，只补中立的Call/Return意图；后端可选shadow快返回，目标不匹配、异常返回、失效等继续使用普通L1/L2。不能用单个CALL微基准替代完整应用收益验证。

## 6. 推进优先级

| 顺位 | 工作 | 证据强度 | 保留边界 |
| --- | --- | --- | --- |
| 1 | FP按predicate消除重复状态修正，合并DE/NaN normal/rare入口 | 指令模型与PMU差额高度吻合 | 完整MXCSR语义、未知值、不同AFP profile |
| 2 | 明确RWX单块形成；独立RX/RW benchmark与产品mutable region设计分开 | ELF flags和所有SMC单块计数闭合 | Block/Function分配独立、源页依赖/撤销 |
| 3 | 首次发布、目录/link登记、完整回收的固定成本 | 阶段定位确定，内部各调用份额未量化 | W^X、唯一owner、不移动计时端点 |
| 4 | Call/Return意图及后端快返回；静态边/布局 | 机制差异确定，整个应用cycles份额未知 | Uniform、真实guest栈和fallback |
| 5 | CoreMark依赖链、mem地址/flags具体形态 | 仅候选，需定点profile | 不先做泛化IR改造 |

保持当前IR owner、容器和三次有界优化框架；后续修改应针对上述真实消费者与生命周期成本，避免再引入多层描述表、包装pass或重复全图验证。

详细证据位于`/Volumes/Data2/swiftvm-middle-results-relocated-20261025/post-l1-benchmark-20261108/`：

- `fp-mechanism-review.md`：逐操作指令模型、固定FEX的MXCSR合同和源码行号。
- `costs-review.md`：compile真实样本、RWX形成与生命周期端点。
- `runtime-mechanism-review.md`：CoreMark、TSO、mem/CALL机制及推断边界。
- `gap-analysis/selected-ledgers.json`：所选真实中位样本及可加差额。
- `gap-analysis/host-features.json`、`host-auxv.raw`：只读设备能力核对。
