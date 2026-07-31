# W71：热点 spill/copy 动态归因只读 spike

## 结论摘要

本轮只实现了默认关闭的计数探针 `SVM_RA_HOT_COALESCE`，没有实现任何优化。
最终版本在探针片段内用 host stack 保存/恢复 `ip0/ip1`，不改变 RA 可用寄存器池；
Orb Linux `-s 8` 下，独立的 `SVM_RA_SHAPE_PROF`（HOT=OFF）与 HOT=ON 都得到
`307 reload + 172 write-back = 479` 个静态 spill 访问，证明最终动态表没有被探针
制造的寄存器压力污染。

最重要的结论不是确认 W69 的 `0x402fed = 84 spill`，而是把它限定到了平台形态：

| 平台，`-s 8 160x120` | `0x402fed` entries | host/entry | spill/entry | dynamic spill | spill/host |
|---|---:|---:|---:|---:|---:|
| Orb Linux | 11,723,259 | 230 | **0** | **0** | **0%** |
| macOS | 11,723,259 | 302 | **84**（48 reload + 36 write-back） | 984,753,756 | 27.814570% |

macOS 的 302/84 与 W69 的手工反汇编值精确相同；Orb Linux 的默认 ABI/RA 形态
则没有这 84 个 spill。故 W69 的热点结论在 mac 成立，但不能外推到本任务指定的
Orb Linux。Orb 真正的 spill 热点是 `0x402e70`：每次入口 149 个 spill 访问，
3,278,291 次入口产生 488,465,359 次动态访问，占该块 entry-weighted host 指令
39.733333%；它几乎占 `-s 8` 全程序动态 spill 的全部，但全程序机械指令上限只有
1.820842%。

Orb `-s 8` 全程序的四类机械上限为：move/宽度桥 31.035161%，NaN guard
6.585859%，连续 state 合并 1.900909%，spill 1.820842%。这些都是“可归因或最多
可少发的 host 指令数 / entry-weighted host 指令数”，不是 cycle 占比，也不是净
加速承诺。

立项排序：

1. **move/tied-copy 可消除性设计 spike**：面积最大，值得在候选能被 def-use 证明
   可删后进入至少 7 对 c-ray A/B；当前 31.0% 包含大量必要 move，不能直接把它
   当收益。
2. **spill + tied-copy 联合实现 spike**：只针对 `0x402e70`，可以与第 1 项捆绑进入
   7 对 A/B；不建议以“消 `0x402fed` spill”为 Linux 目标，也不建议把 1.82% 机械
   上限写成正收益。
3. **proven-finite NaN proof spike**：表面积 6.59%，但必须先得到“可证明 finite 的
   动态覆盖率”；未得到该数据前不进入启发式实现。
4. **state LDP/STP coalesce**：全程序机械上限 1.90%，而 LDP/STP 还可能不减少
   load/store 吞吐或增加寄存器压力；不值得单独进入 7 对实现 A/B。

## 1. 改动与开关

### 1.1 开关和生命周期

- `PerfStats2::kGetenvNames` 从 54 增至 55，末项为 `SVM_RA_HOT_COALESCE`。
- `HotCoalesceProfEnabled()` 采用 function-local static 缓存环境变量。未设置或为
  `0` 时关闭；`1`/`stderr` 输出到 stderr；其他非零值作为输出文件路径。
- 每个 Runtime 持有私有的 `32768 × 4 × u64` 计数区；JIT 只写私有普通计数，
  Runtime 析构时才以 `memory_order_relaxed` 合并到进程原子计数器。
- 退出输出总表，以及按 guest entry 聚合重翻译版本后的 hot/spill/state top-20，
  前缀分别为 `[svm-hot-coalesce]`、`[svm-hot-coalesce-hot]`、
  `[svm-hot-coalesce-spill]`、`[svm-hot-coalesce-state]`。
- 探针代码内嵌进程本地 slot，故 HOT=ON 时禁用 JIT disk cache，避免下个进程加载
  没有对应 metadata 的缓存代码。

### 1.2 OFF 不进 JIT 热路径

`SetCurrent`、spill reload/write-back、NaN guard 和 block finish 的所有埋点都由
process-constant `hot_coalesce_enabled` 在翻译期裁掉。OFF 时不生成入口或分类计数
指令；最终 fingerprint 对 master 的 4,400 个 function units 完全一致，且比较包含
`host_bytes`。

ON 时每段计数指令先 `STP ip0, ip1, [sp, #-16]!`，结束后 LDP 恢复。这样不需要把
`ip0/ip1` 从 RA 池拿走。开发中曾试过全局保留这两个寄存器，结果把 Orb 静态主线
spill 放大到 693；该实现已删除，相关数据作废，没有进入本文表格。

### 1.3 文件清单

- 新增 `source/runtime/common/hot_coalesce_prof.{h,cpp}`：注册、原子聚合、分类、
  top-20 输出。
- `jit_context.{h,cpp}`、`translator.cpp`、`translator_alu.cpp`：入口、真实 spill
  load/store、NaN guard 和主线边界埋点；探针指令范围从 host 分类分母中剔除。
- `context.h`、`runtime.cpp`：保持原 `ExecProfileCounters` 位于 offset 0 的组合接口，
  以及 Runtime 私有计数区的创建/提交。
- `jit_cache.cpp`：HOT=ON 时拒绝 disk cache。
- `perf_stats.h`、runtime CMake、`main_case.cpp`：环境注册、编译接入和 14 个定向断言。

没有改 register allocator、IR 优化或任何生产优化逻辑。

## 2. 计数口径

### 2.1 精确运行期计数

- **entries**：每个已编译 block 的 guest entry 真正进入时加一；不同编译版本在
  进程退出时按 guest PC 聚合。
- **spill**：只在 JIT 实际发出的 `State::spill_area` reload LDR 和 write-back STR
  之后加一。因此 `spill_dynamic` 是实际走过的 spill 访问，不是翻译点乘猜测。
- **NaN guard**：在 `QueueVecNaNColdPath` 的正常热边上精确加该 guard 的 host
  指令数；scalar 为 2、packed-32 为 4、packed-64 为 5。cold correction stub 不进
  分母。

### 2.2 静态形态乘精确 entries

- **host denominator**：从入口探针之后到 terminal 结束的主线 host 指令数，减去
  所有探针指令；再乘该 block 的精确 entries。它不是 PMU retired-instruction 计数，
  若 block 内部有不总执行的分支，仍是 entry-weighted 机械上界。
- **move/宽度桥**：VIXL 反汇编主线后识别 `mov/fmov/umov/smov/ins/dup`、
  `uxt*/sxt*`、`ubfx/sbfx/bfi/bfxil/extr` 及 shift-by-zero；排除探针与已单列的
  NaN guard，再以静态数乘 entries。该分类比 W69 手工分类更宽，包含必要 ABI/value
  move，故 31% 不是可消除率。
- **state coalesce**：在最终 IR 中找连续 `LoadUniform` 或 `StoreUniform`。同方向、
  同 4/8/16-byte 宽度、相邻 offset 的两个访问计为一个 LDP/STP 机会，机械省 1 条；
  连续同 offset 的安全复用/DSE/forward 也机械省 1 条。遇到其他 IR 立即截断，因而
  不跨 helper/fault/barrier 猜测。`saved_static × entries` 为保守候选集合的机械上限，
  但尚未扣配对寄存器、编码范围、调度与新增 spill 成本。

各类别互斥于探针指令；spill 是 host denominator 的子集，move、NaN、state-saved
分别表达不同问题，不能相加后宣称净加速。

## 3. Orb Linux 测量

### 3.1 三档 c-ray 总表

均为 `-j 1`，图片采用 PNG IDAT payload MD5。ON 的 wall 只说明探针很重，不用于
性能结论。

| spp / dimensions | OFF wall | ON wall | PNG IDAT hash（ON=OFF） | host dynamic | spill | spill % | move % | NaN % | state saved % |
|---|---:|---:|---|---:|---:|---:|---:|---:|---:|
| 1 / 64x48 | 0.403178s | 6.104532s | `654b93c67e58d1f17e86afecc1e03592` | 2,357,440,260 | 10,409,371 | 0.441554% | 33.300000% | 4.252056% | 1.124555% |
| 4 / 128x96 | 0.828795s | 7.219775s | `d0348e6265ed38e5ddb39d5b549805b4` | 9,954,963,324 | 158,237,775 | 1.589536% | 31.362654% | 6.169067% | 1.766700% |
| 8 / 160x120 | 1.821716s | 9.226541s | `f9afd5bed804b5c2654303c9252d09c4` | 26,827,135,647 | 488,479,870 | 1.820842% | 31.035161% | 6.585859% | 1.900909% |

`-s 8` 的 `488,465,359 / 488,479,870 = 99.9970%` 动态 spill 来自
`0x402e70`。其余三个 spill block 合计仅 14,511 次，虽有很高块内比例但不热。

### 3.2 `-s 8` hot top-20 完整表

| rank | PC | entries | host/entry | host dynamic | move/entry | move % | NaN/entry | NaN % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | `0x402fed` | 11,723,259 | 230 | 2,696,349,570 | 82 | 35.652174% | 24 | 10.434783% |
| 2 | `0x403088` | 8,524,283 | 215 | 1,832,720,845 | 66 | 30.697674% | 24 | 11.162791% |
| 3 | `0x407450` | 3,486,673 | 473 | 1,649,196,329 | 157 | 33.192389% | 82 | 17.336152% |
| 4 | `0x402e70` | 3,278,291 | 375 | 1,229,359,125 | 79 | 21.066667% | 0 | 0% |
| 5 | `0x457ad0` | 4,968,099 | 174 | 864,449,226 | 34 | 19.540230% | 36 | 20.689655% |
| 6 | `0x403110` | 8,567,828 | 88 | 753,968,864 | 30 | 34.090909% | 0 | 0% |
| 7 | `0x40308c` | 3,198,976 | 214 | 684,580,864 | 66 | 30.841121% | 24 | 11.214953% |
| 8 | `0x457b70` | 3,562,209 | 160 | 569,953,440 | 33 | 20.625000% | 30 | 18.750000% |
| 9 | `0x402fc0` | 11,779,907 | 42 | 494,756,094 | 18 | 42.857143% | 0 | 0% |
| 10 | `0x402f62` | 3,278,288 | 134 | 439,290,592 | 54 | 40.298507% | 8 | 5.970149% |
| 11 | `0x407a40` | 1,512,697 | 240 | 363,047,280 | 78 | 32.500000% | 44 | 18.333333% |
| 12 | `0x41e8f0` | 5,086,142 | 70 | 356,029,940 | 37 | 52.857143% | 0 | 0% |
| 13 | `0x403132` | 5,855,155 | 58 | 339,598,990 | 21 | 36.206897% | 0 | 0% |
| 14 | `0x407638` | 3,193,593 | 104 | 332,133,672 | 36 | 34.615385% | 12 | 11.538462% |
| 15 | `0x4076d2` | 1,377,708 | 241 | 332,027,628 | 73 | 30.290456% | 44 | 18.257261% |
| 16 | `0x403114` | 3,155,431 | 87 | 274,522,497 | 30 | 34.482759% | 0 | 0% |
| 17 | `0x403188` | 5,868,104 | 46 | 269,932,784 | 15 | 32.608696% | 0 | 0% |
| 18 | `0x403201` | 3,334,939 | 80 | 266,795,120 | 23 | 28.750000% | 0 | 0% |
| 19 | `0x41bd50` | 1,895,621 | 131 | 248,326,351 | 24 | 18.320611% | 0 | 0% |
| 20 | `0x41e99d` | 2,894,533 | 85 | 246,035,305 | 30 | 35.294118% | 4 | 4.705882% |

### 3.3 `-s 8` spill 完整表

| rank | PC | entries | reload/write-back per entry | dynamic reload/write-back | dynamic total | block host dynamic | block spill % |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | `0x402e70` | 3,278,291 | 81 / 68 | 265,541,571 / 222,923,788 | 488,465,359 | 1,229,359,125 | 39.733333% |
| 2 | `0x506460` | 64 | 158 / 64 | 10,112 / 4,096 | 14,208 | 27,904 | 50.917431% |
| 3 | `0x404414` | 4 | 41 / 24 | 164 / 96 | 260 | 1,432 | 18.156425% |
| 4 | `0x408531` | 1 | 27 / 16 | 27 / 16 | 43 | 143 | 30.069930% |

不同工作量中 `0x402e70` 的 static spill 恒为 149；entries 从 69,788（s1）增至
1,061,916（s4）再到 3,278,291（s8），对应动态 spill 10,398,412、
158,225,484、488,465,359。它是真正随渲染工作放大的热点。

### 3.4 `-s 8` state coalesce top-20 完整表

`saved/entry = load_pairs + store_pairs + same_offset`。

| rank | PC | entries | sequences | load pairs | store pairs | same offset | saved/entry | saved dynamic | block % |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | `0x402fed` | 11,723,259 | 6 | 0 | 6 | 1 | 7 | 82,062,813 | 3.043478% |
| 2 | `0x403088` | 8,524,283 | 6 | 0 | 6 | 0 | 6 | 51,145,698 | 2.790698% |
| 3 | `0x457ad0` | 4,968,099 | 10 | 0 | 10 | 0 | 10 | 49,680,990 | 5.747126% |
| 4 | `0x407450` | 3,486,673 | 14 | 0 | 14 | 0 | 14 | 48,813,422 | 2.959831% |
| 5 | `0x457b70` | 3,562,209 | 11 | 1 | 10 | 0 | 11 | 39,184,299 | 6.875000% |
| 6 | `0x402e70` | 3,278,291 | 6 | 0 | 6 | 2 | 8 | 26,226,328 | 2.133333% |
| 7 | `0x403201` | 3,334,939 | 7 | 0 | 7 | 0 | 7 | 23,344,573 | 8.750000% |
| 8 | `0x402f62` | 3,278,288 | 5 | 0 | 5 | 1 | 6 | 19,669,728 | 4.477612% |
| 9 | `0x40308c` | 3,198,976 | 6 | 0 | 6 | 0 | 6 | 19,193,856 | 2.803738% |
| 10 | `0x407a40` | 1,512,697 | 9 | 0 | 9 | 0 | 9 | 13,614,273 | 3.750000% |
| 11 | `0x457c00` | 1,405,890 | 8 | 0 | 8 | 0 | 8 | 11,247,120 | 5.405405% |
| 12 | `0x403110` | 8,567,828 | 1 | 0 | 1 | 0 | 1 | 8,567,828 | 1.136364% |
| 13 | `0x41be2a` | 1,895,621 | 4 | 0 | 4 | 0 | 4 | 7,582,484 | 3.418803% |
| 14 | `0x40f07d` | 1,254,130 | 6 | 0 | 6 | 0 | 6 | 7,524,780 | 3.488372% |
| 15 | `0x402f46` | 3,278,289 | 2 | 0 | 2 | 0 | 2 | 6,556,578 | 2.702703% |
| 16 | `0x402f27` | 3,278,280 | 2 | 0 | 2 | 0 | 2 | 6,556,560 | 2.666667% |
| 17 | `0x403237` | 2,781,585 | 1 | 0 | 1 | 1 | 2 | 5,563,170 | 5.882353% |
| 18 | `0x412e10` | 2,577,948 | 1 | 0 | 2 | 0 | 2 | 5,155,896 | 6.666667% |
| 19 | `0x41e8f0` | 5,086,142 | 1 | 0 | 1 | 0 | 1 | 5,086,142 | 1.428571% |
| 20 | `0x40f030` | 1,254,130 | 4 | 0 | 4 | 0 | 4 | 5,016,520 | 4.494382% |

全程序 `state_saved_dynamic = 509,959,314`，除以 `host_dynamic =
26,827,135,647` 得 1.900909%。这是“每个 pair 从两条变一条、同 offset 安全消一条”
的指令数上限；不等于 load/store uop、带宽或 wall 上限。

## 4. 三个问题的回答

### 4.1 `0x402fed` 和 top hot unit 的 spill 成本

Orb Linux 的答案是 `0x402fed = 0%`，不是 27.8%。macOS 同址为 27.814570%，
证明 W69 数据本身没有数错，只是平台形态不同。Orb 的 `0x402e70` 为 39.733333%
块内占比并贡献 488,465,359 次；但把它放回全程序分母，只是 1.820788%，加上三个
冷块后为 1.820842%。因此不能再使用 W69 的“最热 `0x402fed` spill-only 4.20%
全程上限”批准 Linux spill 项目。

### 4.2 state coalesce 动态上限

Orb `-s 8` 的保守连续序列候选最多少发 509,959,314 条，占 entry-weighted host
1.900909%。其中 top 三个 W69 对齐块共 182,021,933 条，占它们 6,178,266,744 条
host 的 2.946166%。这个上限未扣 LDP/STP 的寄存器配对、调度、fault 精度和新增
spill；宽松上界只能用于否定：单独项目即使机械全吃满也不到 2%，不批准 standalone
实现 spike。

### 4.3 move / NaN 的真实动态占比

| 范围 | move | NaN guard |
|---|---:|---:|
| Orb 全程序，s1 | 33.300000% | 4.252056% |
| Orb 全程序，s4 | 31.362654% | 6.169067% |
| Orb 全程序，s8 | **31.035161%** | **6.585859%** |
| Orb 三个 W69 对齐块，s8 | **33.525868%** | **12.492957%** |
| W69 三块 SVM 静态 host 流（232/941、102/941） | 24.654623% | 10.839532% |
| W69 的 702 条 SVM-FEX 静态差额口径 | 22.8% | 14.5% |

用户给出的 22.8%/14.5% 是“占 SVM-FEX 静态差额”，而 W71 没有 FEX 动态分类，
所以不能计算严格同分母的动态 gap share。若只做字面比较，全程序 move 更高、NaN
更低，并不一致。若改用 SVM host 流作分母，三个对齐块的 NaN 12.49% 与 W69
10.84% 同量级，move 33.53% 明显更高；差异同时来自动态 entry 权重、Orb/mac RA
形态，以及 W71 自动 move 分类（305/918）比 W69 手工分类（232/941）更宽。故本轮
确认“move 面积很大”和“NaN 在对齐热块确实热”，但数据不足以把差值解释成纯动态
热度效应。

## 5. 验收结果

| 门 | 结果 |
|---|---|
| OFF fingerprint vs `/tmp/svm-build` | 通过；11 guests、4,400 function units，self-consistency 与 cross-build 均 OK，明确包含 `host_bytes` |
| c-ray ON/OFF oracle | 三档 PNG IDAT hash 逐档相同，见 3.1 |
| CoreMark ON | 150,000 iterations，`e9f5/e714/1fd7/8e3a/25b5`，`Correct operation validated` |
| `SVM_EXEC_PROF` + HOT 同开 | 定向 CoreMark smoke rc=0；既有 `[svm-exec]` 与新计数均正常输出，验证组合接口 offset 兼容 |
| Orb 全量构建 | 通过 |
| Orb `swift_test` | 142,585 assertions / 124 cases；基线 142,571/123，加的 14/1 全部来自新定向测试 |
| mac 全量构建 | 通过 |
| mac `swift_test` | 139,975 assertions / 124 cases；基线 139,961/123，加的 14/1 全部来自新定向测试 |
| ON suite 路径 | 未重复跑整套 ON；ON 只改变翻译出的探针代码，实际 JIT 路径已由 c-ray 三档与 CoreMark 150k 覆盖，静态分类由 14 个定向断言覆盖 |

独立扰动校验：Orb HOT=OFF + `SVM_RA_SHAPE_PROF` 得 `units=8307,
spill_units=4, spill_loads=307, spill_stores=172`；最终 HOT=ON 的四个 spill block
静态总和也是 307/172。

## 6. 风险、数据不足与下一步门

仍然无法从本轮数据预估以下净值：

- move 中有多少是语义/ABI 必需、多少是 tied operand、宽度桥或 RA copy 真能删除；
- 删除 `0x402e70` spill 后会新增多少 move、延长多少 live range、是否触发新的 spill、
  scratch escalation、terminal/cold-edge 保存或 host code growth；
- NaN guard 中有多少动态实例能由 finite provenance 正确证明；
- LDP/STP 在目标 CPU 上是否减少 cycle，以及配对寄存器压力和 fault 精度边界成本；
- W71 没有 PMU retired/cycle 分类，也没有 FEX 动态同口径，因此所有百分比都是 host
  指令机械面，不是 wall 或动态 gap 的因果份额。

若继续，建议下一阶段仍默认 OFF，并以 `SVM_RA_HOT_COALESCE` 的 PC 排序选择
`0x402e70` 和 move top blocks，先输出候选的 def/use、冲突图和“删除/新增”净静态账。
只有候选能同时满足“不新增 spill/scratch escalation、host bytes 净下降、oracle 与
fingerprint 不变”，才进入至少 7 对 c-ray A/B 95% CI。任何宽松上限都不单独作为
立项依据。

## 7. 证据位置与复现命令

本机临时证据：

- Orb：`/tmp/w71-final-{s1,s4,s8}.count`、`/tmp/w71-final-*-{on,off}/`、
  `/tmp/w71-final-coremark.count`、`/tmp/w71-ra-off.count`。
- mac：`/private/tmp/w71-mac-hot.count`。

OFF 指纹命令：

```sh
orb -m ubuntu sh -lc \
  '/mnt/mac/tmp/w66/source/translator/linux/tests/run_func_fingerprint_tests.sh \
   /tmp/w71-build/source/translator/linux/svm_translator_linux \
   --against /tmp/svm-build/source/translator/linux/svm_translator_linux'
```

探针用法：

```sh
SVM_RA_HOT_COALESCE=/tmp/hot.count svm_translator_linux GUEST [ARGS...]
```
