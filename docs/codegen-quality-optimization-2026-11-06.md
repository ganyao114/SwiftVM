# ARM64 生成质量优化与同批验证：2026-11-06

本轮在 `ebf55740` 上修改五个既有 ARM64 后端文件，并扩展少量原有行为测试。Uniform IR、三个公共优化 walk、Block／Function 独立分配、Home 布局和运行时 owner 均沿用现有实现。该轮测量绑定当时尚未提交的代码快照，原始源码与二进制指纹保留。后续优化及最终代码提交关联见[Function compile结果](function-compile-optimization-2026-11-06.md)。

整数 Block／Function 的动态指令分别减少17.0%／19.7%，耗时减少22.6%／23.9%；当前整数指令数约为FEX的1.016×。内存指令减少5.8%／8.1%，但本批耗时仅改善0.3%／2.1%；调用指令减少2.2%／2.9%，耗时改善2.4%／2.0%。这些差异没有等比例转化为运行时间。

CoreMark 本批基本持平，Function耗时+0.8%；func_tests Function+0.9%，x87 Block／Function+2.8%／+1.2%，均如实保留。内存和调用的指令数仍分别高于FEX约25.5%和21.3%，CoreMark Block分支差距仍待后续有针对性的分析。

## 实现与根因

| 根因 | 已实现的收敛 | 合同边界 |
| --- | --- | --- |
| 整数热体把移位后逻辑分成两条 | 同块紧邻、sole-use 的 U64 移位并入 XOR／AND／OR 的 shifted operand | 原 Read 保逻辑视图；producer／consumer 默认 metadata，排除 legacy flags、spill、窄 consumer view和重复引用；复用 elided_consumer |
| 乘法 CF 发布有两次反转 | NativeNZCV 下紧邻 TestNotZero→SetCarry 直接把零谓词写入 native inverted CF，省 EOR 和 CFINV | 先完成原 flags 再 Read；保 NZV、PF、AF 和 reserved；packed／多消费者保原路径 |
| Function 外部条件尾多走一次 B26 | 两种 scope 复用既有可重定位 conditional head，primary ready 边直接条件跳转 | 调用点已完成 flags并退出spill frame；restore／flip和真实内部CFG保原行为；未改变Block专用 deferred-test |
| 向量对齐 mask 先装临时寄存器 | 可编码 mask 直接用非 S 的 AND immediate | NZCV 不变，CBZ／真实 fault及不可编码 mask原路径保留 |
| 大地址偏移先 MOVZ+MOVK 再 ADD | 无 flags 的适合24位偏移用两条 immediate ADD／SUB | 覆盖普通 Add／Sub与复合 Expression；只取比现 constant materialization更短的形态，保 W/X模宽和flags |

现有 Typed Handle 派发继续负责选码，没有新增 pass、IR opcode、N级表或生命周期包装。实际代码位于 `emit_scalar.cpp`、`emit_flags.cpp`、`emit_control.cpp`、`emit_operand.cpp` 和 `emit_memory.cpp`。

## 静态与功能证据

- 真实 guest 的整数热体，五组移位逻辑合为五条 shifted EOR；整 unit 47→43 words，差值包含对齐 padding，不能把 word 数直接当作执行条数。
- 调用叶子的 ASR→XOR及CF链共少三条实际指令，整 unit 123→121 words，同样包含 DATA／对齐差异。
- 向量 streaming 热体的两次对齐检查及第二个地址共少三条实际指令，整 unit 59→55 words。
- FP 两种scope的算术前缀保持相同，Function resolved primary tail 省一条每圈 B26；整个输出 word 数可因冷边和对齐而不变。
- 最终 host CTest 9/9、ASan/UBSan 9/9；红魔七个 C++ 检查、InterpretOnly／Block／Function 状态图像3/3通过；五arm workload smoke40/40通过。测试复用既有 linked flags、整数与条件近链／远链／撤链场景。

离线选码按当前生产入口和真实 guest 字节重建；它不是对运行 RX 的逐字绑定。以下同批 PMU 记录才用于判断真实动态收益，不用局部删词比例外推全程性能。

## 固定测量方法

红魔 NX729J `bf1359ff`，CPU4／taskset10，既有 min/max 614400／1171200 kHz保持。固定五arm为上一已推送版本的 Block/Function、优化版的 Block/Function、固定 FEX；每输入每arm五次，共200普通运行和200独立compile样本。全部保留，没有凑门槛追加、替换或跨批混池。FEX继续使用 `a156752fae3a4f65fefb1ed95af980bcdfac43d7 + original Android patch`、同一guest、AVX-off／TSO配置。

每次新launcher，无AOT/object cache，保留OS page cache。CoreMark100000、CRC d340、正式验证且loop timer≥10秒；其它输入使用whole-child simpleperf elapsed。PMU为用户态child树计数，包含启动和冷JIT，独立FEXServer在树外。x87继承exit-only workload oracle，不据此宣称完整数值／ISA覆盖。

Compile仅扣实际字节parser；形成、IR出生、失败／回退、优化、分配、发射、发布和原释放窗口都收费。SVM计最终native owner物理释放，FEX旧probe只计compile-root逻辑工作缓冲释放，未补猜或扣除这项边界差异。

## 运行时间：同批上一版 → 优化版

单位ms，五次中位数；变化与FEX倍率均为中位数之比。B/F分别独立报告。

| 输入 | Block 前→后 | 变化 | Function 前→后 | 变化 | 当前/FEX，B / F |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 16118.000→16112.000 | -0.0% | 15663.000→15786.000 | +0.8% | 1.0712 / 1.0495 |
| int | 301.724→233.436 | -22.6% | 302.857→230.381 | -23.9% | 0.7406 / 0.7309 |
| fp | 250.655→249.941 | -0.3% | 250.776→249.111 | -0.7% | 0.9471 / 0.9439 |
| mem | 2149.607→2142.269 | -0.3% | 2179.583→2134.359 | -2.1% | 1.0441 / 1.0402 |
| branch | 257.503→253.696 | -1.5% | 260.130→254.400 | -2.2% | 0.9462 / 0.9488 |
| call | 479.978→468.633 | -2.4% | 477.840→468.349 | -2.0% | 1.0232 / 1.0226 |
| func_tests | 109.405→108.437 | -0.9% | 120.187→121.277 | +0.9% | 0.7706 / 0.8618 |
| x87_bench | 591.326→607.976 | +2.8% | 591.324→598.357 | +1.2% | 0.7072 / 0.6960 |

## 动态指令与cycles

这些计数包含冷JIT和启动，不能单凭它们判定稳态每条指令质量；也不能把同一批多个实现改动的整体收益归给单独一个hunk。

| 输入 | 指令变化 B | 指令变化 F | 指令/FEX，B / F | cycles/FEX，B / F |
| --- | ---: | ---: | ---: | ---: |
| coremark | -0.00% | -0.00% | 1.0378 / 1.0099 | 1.0708 / 1.0492 |
| int | -17.00% | -19.72% | 1.0161 / 1.0161 | 0.7029 / 0.7030 |
| fp | -0.00% | -1.50% | 0.9902 / 0.9902 | 0.9834 / 0.9831 |
| mem | -5.84% | -8.14% | 1.2549 / 1.2549 | 1.0514 / 1.0465 |
| branch | -5.20% | -6.81% | 1.0785 / 1.0785 | 0.9820 / 0.9820 |
| call | -2.22% | -2.94% | 1.2131 / 1.2131 | 1.0456 / 1.0458 |
| func_tests | +0.01% | +0.03% | 0.7886 / 1.0402 | 0.7838 / 0.9706 |
| x87_bench | -0.00% | -0.00% | 1.3754 / 1.3725 | 0.6945 / 0.6838 |

## Compile：同批上一版 → 优化版

| 输入 | Block 前→后 ms | 变化 | Function 前→后 ms | 变化 | 当前/FEX，B / F |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 42.1323→42.4353 | +0.7% | 51.3259→51.9458 | +1.2% | 0.8938 / 1.0941 |
| int | 1.9962→1.9093 | -4.4% | 1.8592→1.9532 | +5.1% | 1.2701 / 1.2992 |
| fp | 2.5681→2.4506 | -4.6% | 2.4878→2.6326 | +5.8% | 1.4753 / 1.5848 |
| mem | 2.2431→2.2333 | -0.4% | 2.3959→2.3623 | -1.4% | 1.4176 / 1.4995 |
| branch | 1.8593→1.9333 | +4.0% | 1.9268→1.9497 | +1.2% | 1.2898 / 1.3008 |
| call | 1.9113→2.0609 | +7.8% | 1.8821→2.0092 | +6.8% | 1.2261 / 1.1954 |
| func_tests | 21.0236→21.4120 | +1.8% | 33.1232→33.3801 | +0.8% | 0.7092 / 1.1056 |
| x87_bench | 0.6679→0.6773 | +1.4% | 0.5858→0.5663 | -3.3% | 2.2842 / 1.9099 |

既定运行time／cycles≤1.10×、compile≤2×的门槛，本轮 47/48通过。未过项：

- x87_bench／block／compile。

## 生成统计的范围

16组 workload／scope 的 spill slots 和 max frame 取值集合均与基线一致。func_tests Block 的实际请求范围有小幅差异：基线IR节点16906或16929、operand words28906或28949，优化版为16906／28906。该项少量IR总量和时间变化不当作公共IR优化或单一选码改动的独立收益；完整取值见 quality-comparison.json。整数等固定热体的IR／Home保持原样，选码直接减少实际指令。

## 身份与保留

优化版源码依赖digest：`ead9d0166558dc4ef37915bc09cc7d1b62fb7b7e40ae242c0e22f6d828c0e074`（282项）；runner SHA256：`2539a519d83743b3dde0c2aa10b14fec34792221de601b573b484876f231077d`。基线runner SHA256：`83e10c8fa6cb94dab777ce2c07c8882e2d9634bbc3393728a70641f3f7961946`，绑定上一已推送 `ebf55740`。本轮没有把新的工作树指纹标成旧已提交源码。

完整原始结果、前后静态词流、测试、源压缩包和两版runner在 `/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/codegen-quality-optimization-20261106`。小型汇总与原始记录见 [evidence](evidence/codegen-quality-optimization-20261106/README.md)。手机本轮functional与benchmark独立namespace均在无活动进程／打开文件后清除；所有构建复用Data2固定缓存。
