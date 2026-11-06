# Function compile 优化与 FEX 同层对照 2026-11-06

本轮按 **SVM Function level 对 FEX 的多基本块 CompileBlock** 对齐。固定五次同批中位数，x87 Function完整compile从 **0.640780 ms降到0.592396 ms，减少7.6%**；FEX为0.305781ms，当前 **1.9373×**。达到本批预设的2×中位数门槛，约0.5ms的工程目标尚未达到。

波动同样保留：Function五次为 0.834165, 0.592396, 0.516512, 0.607814, 0.563699 ms，范围0.516512–0.834165ms；均值0.622917ms，相对FEX均值为 **2.0417×**。因此不能把这批中位数通过表述为已证明稳定／最坏情况≤2×。没有删除0.834ms高值、追加或替换样本。该高值的optimize_ns为371979，IR规模与省末扫计数未增，现记录尚不能区分其中的调度、缺页或计算成本。

Block继续独立报告端到端运行模式成本：x87 Block 0.600835→0.625779ms，变化+4.2%，当前/FEX=2.0465×，仍未过2×；不再用“三个Block对一个FEX root”解释同层级编译效率。

## 已实现的收敛

| 既有文件，相对source/runtime | 工作与合同 |
| --- | --- |
| frontend/x86/region.cpp | 原有有序map和pending使用2KiB局部PMR，超出正常回落；实际返回records/blocks仍独立vector。records首次预留最多32条，保预算、ready frontier、late split和队列次序 |
| frontend/x86/build_ir.cpp | 仅本次成功形成的Function走同一Emitter内部结构可信分支；公共decoded/Block保持原结构校验，prefix/form/Unknown/x87窗口与提交照旧；已形成至少16条记录的Function，IR初始提示64N/192W，减少首次迁移 |
| ir/builder_impl.h、builder.cpp | 内联实际Cell写口，保已验证kind的传播；容量增长共享到三个冷函数，仅不足时调用。Schema、unit/type/view/literal、wide/header、标准vector写入和失败回滚保持 |
| compile/optimize.cpp/.h | 首扫、CFG和forward顺手聚合局部cleanup事实；全图稳定才走原Finish省末扫，所有变图／窄flags／newdiscard／非默认Nop仍走原末扫；默认FaultLocation保留但省无值无旗分析 |
| compile/optimize.cpp | 小Function原4份B/edge工作缓冲改为8/9/8/16局部数组及活动span，超8B保原vectors；初始化、flags固定点和队列不变，小图实际缓冲容量456B如实计数，无CFG仍0 |
| backend/arm64/emit_float.cpp | 按既有Home实际使用情况，省无人消费的成功status装载接口及死spill保存；Init架构state.result清零、helper、pending/raised、精确PC、成功metadata及真实结果用户保持 |
| execution/code_cache.cpp、compile_service.cpp | 复用未增长时的目录探测位置，合并链接统计／登记遍历，复用slab统计；SingleRX计数／冷链接及SMC策略保持 |
| execution/runtime.h、launcher/linux/main.cpp | 仅诊断输出已有copy_forwarded及新final_walk_skipped聚合计数，普通运行不新增逐节点时钟 |

当前x87 Function仍1次请求、20条guest、50N/126W；末扫省略计数为1。Block仍3次请求、20条guest、51N/131W，末扫省略计数为3。返回值省装载的源码路径已经覆盖，但本例总native bytes仍为Function1732／Block1864，未将它宣称为已测得的机器码减少。

IR仍是原24B Operation／8B Cell，Uniform语义、Block/Function独立分配、五参数schema、Handle派发、native映射与SMC owner保持。没有新增中间pass、N级镜像、公共Fast入口、unchecked Builder或整程序证明层。

## Compile完整对照

单位ms，五次中位数。Function作为主结构对照，Block单列；不对不同输入求平均。

| 输入 | Function前→后 | 变化 | 当前F/FEX | Block前→后 | 变化 | 当前B/FEX |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| coremark | 51.1596→48.9009 | -4.4% | 1.032 | 40.8363→39.8454 | -2.4% | 0.841 |
| int | 1.8875→1.7808 | -5.7% | 1.188 | 1.8626→1.7715 | -4.9% | 1.182 |
| fp | 2.5723→2.4502 | -4.7% | 1.463 | 2.2678→2.2537 | -0.6% | 1.345 |
| mem | 2.2862→2.1827 | -4.5% | 1.387 | 2.2162→2.3101 | +4.2% | 1.467 |
| branch | 1.8943→1.8609 | -1.8% | 1.172 | 1.8330→1.8433 | +0.6% | 1.161 |
| call | 1.9680→1.8900 | -4.0% | 1.198 | 2.0269→1.9317 | -4.7% | 1.224 |
| func_tests | 33.1391→31.5892 | -4.7% | 1.044 | 20.8562→20.0276 | -4.0% | 0.662 |
| x87_bench | 0.6408→0.5924 | -7.6% | 1.937 | 0.6008→0.6258 | +4.2% | 2.046 |

以下单位µs，分别取总量中位数对应的同一个样本；所有互斥分项闭合，未把独立分项中位数相加。

| x87阶段 | 基线Function | 当前Function | FEX |
| --- | ---: | ---: | ---: |
| 形成与IR出生，扣实际parser | 278.18 | 244.90 | 119.95 |
| IR优化 | 81.51 | 82.86 | 40.16 |
| 分配与发射 | 80.52 | 94.48 | 118.07 |
| 转交与发布 | 107.76 | 73.54 | 20.31 |
| 原生owner释放／FEX逻辑归还 | 77.92 | 80.57 | 2.24 |
| 其余收费工作 | 14.89 | 16.04 | 5.05 |
| 合计 | 640.780 | 592.396 | 305.781 |

SVM计首次native双映射与最终实际owner释放；固定FEX在backend初始化时预建RWX代码区，其旧probe的释放阶段主要计逻辑归还，native arena最终销毁在窗口之外。双方均有临时发码与尾部copy。原计时范围保留，未提前Map、延后Release或扣掉SVM成本宣告过线。当前Function已经一个unit、一次完整发布同步、无静态link；未以链接同步合并预收本例收益。

## 实际代码体积与诊断所得

同身份生产ELF `.text` 为 1443088→1422376B，减少20712B／1.44%。原公共 `StoreOperand<false>` 为740B、363个静态直接调用站点；当前独立符号消失，Grow<Cell/Operation/BlockRecord>分别212/240/240B。未强制内联整个AppendImpl，未添加raw vector写或assume。代码体积变化是静态证据，不直接换算成compile微秒。

首个实现快照369e023c／runner bd88463b的完整200个compile样本已保留：Function基线578.645→候选590.158µs，约+2.0%，没有将其接受为性能优化，也未运行原计划runtime。之后用两版临时诊断ELF各5次，分开形成、IR出生和优化器内部阶段；诊断有额外clocks/getrusage/stderr，不能作为正式总成本或与当前样本混池。

该诊断发现，原末扫约4.7µs、稳定返回约1.5µs，省末扫承担不了先前约30µs的预算；初始化约19µs及IR出生仍占较大费用。因此继续减少小CFG堆分配、IR首次迁移，并依据实际反汇编分离公共word写口的冷增长。所有临时include／时钟均已撤回，最终只测当前一个完整固定campaign。两批差额不当作某个hunk独立因果收益。

## 运行与生成质量

16组workload/scope的spill slots和max frame取值集合均与基线相同；其中15组的IR nodes、operand words和native bytes取值集合也相同。func_tests Block基线1131或1132次请求，候选1131次；IR 16836或16859→16836、operand words 28754或28797→28754、native bytes 220380或220600→220380。这项实际请求路径差异不作为IR压缩或选码改进的独立收益，全部取值保留于quality-comparison.json。 这类统计不能替代逐指令或完整ISA等价证明；本轮结合既有功能契约和状态图像验证，保留x87精度、异常及metadata合同。

单位ms；CoreMark为正式100000轮、CRC d340且loop timer≥10秒，其余whole-child elapsed包含冷启动与JIT。

| 输入 | Function前→后 | 变化 | Block前→后 | 变化 | 当前/FEX，F / B |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 15760.000→15745.000 | -0.1% | 16182.000→16185.000 | +0.0% | 1.044 / 1.073 |
| int | 229.897→230.359 | +0.2% | 229.123→230.058 | +0.4% | 0.731 / 0.731 |
| fp | 249.211→250.577 | +0.5% | 248.082→248.213 | +0.1% | 0.947 / 0.938 |
| mem | 2140.718→2148.094 | +0.3% | 2138.341→2140.048 | +0.1% | 1.048 / 1.044 |
| branch | 253.486→254.653 | +0.5% | 252.522→253.795 | +0.5% | 0.943 / 0.939 |
| call | 467.653→467.922 | +0.1% | 466.993→468.383 | +0.3% | 1.020 / 1.021 |
| func_tests | 122.413→120.993 | -1.2% | 107.709→107.993 | +0.3% | 0.860 / 0.767 |
| x87_bench | 597.395→595.703 | -0.3% | 600.362→595.597 | -0.8% | 0.686 / 0.686 |

PMU为whole-child用户态，包含冷JIT，不把微小总指令变化当成稳态热体改善。

| 输入 | 指令变化 F / B | cycles/FEX，F / B |
| --- | ---: | ---: |
| coremark | -0.011% / -0.006% | 1.0439 / 1.0723 |
| int | -0.006% / -0.007% | 0.7025 / 0.7014 |
| fp | -0.029% / -0.028% | 0.9824 / 0.9814 |
| mem | -0.001% / -0.002% | 1.0506 / 1.0498 |
| branch | -0.004% / -0.005% | 0.9807 / 0.9798 |
| call | -0.002% / -0.002% | 1.0510 / 1.0520 |
| func_tests | -2.777% / -1.704% | 0.9451 / 0.7674 |
| x87_bench | -0.344% / -0.343% | 0.6696 / 0.6731 |

既定time／cycles≤1.10×、compile≤2×，本批47/48项中位数门槛通过，未过项：

- x87_bench／block／compile：2.0465×。

## 验证与身份

- 主机CTest9/9、ASan/UBSan9/9；红魔C++7/7、InterpretOnly／Block／Function状态图像3/3；workload smoke40/40通过。既有用例补稳定门与必要反例、9B/129B回落、内部形成/外部decoded、helper真实/死返回值检查，没有生产双跑或验证pass。
- 红魔NX729J bf1359ff、CPU4/taskset10，原min/max614400／1171200kHz及服务保持。固定FEX a156752fae…+Android patch、同guest、AVX-off／TSO、无AOT/object cache、保留OS page cache。每次fresh launcher；FEXServer在PMU child树之外。x87 workload继承exit-only oracle，精度契约测试单列。
- 全部200普通运行+200compile样本有效；基线为9a37021c／runner e579c6fb，而非把Git HEAD当被测源码。当前源码digest `166aeaed5d886c5da6d7c23eb3da663a4bfa788fc0957c884ae016ef943cc959`（282项），runner `7b1799033060b8f00ecc6b437b363c26ee30a94f04f7fea68976fa549be5f86a`，测量时为ebf55740上的明确WIP快照。整理后的代码提交为`f3f236a5`，其282项依赖文件与测量快照逐项一致；关联见[submission.json](evidence/function-compile-20261106/submission.json)，原冻结清单保持不变。
- 原始结果、源/runner压缩包、patch及审计：`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/function-compile-final-20261106`。首个未接受开发点与临时诊断位于同级`function-compile-optimization-20261106`。所有构建和大临时数据使用Data2固定缓存，手机本轮functional/benchmark/diagnostic namespace均已检查无活动写入并清理。

[小型汇总、完整原始记录与清理回执](evidence/function-compile-20261106/README.md)。
