# x87 Block compile 优化与同批验证：2026-11-06

后续对齐修正：FEX的多基本块CompileBlock对应SVM Function level。本文原始测量保持，新的同层级归因及优化结果见[Function compile对照](function-compile-optimization-2026-11-06.md)。

x87 Block 完整 compile（只扣实际字节 parser）从 **0.681927 ms 降到 0.565002 ms，减少 17.1%**。同批固定 FEX 为 **0.280833 ms**，当前倍率 **2.0119×**，仍比严格两倍预算多 **3.335 µs／0.59%**，不能据此宣告≤2×目标完成。五次当前 Block 范围为 545.417–578.541 µs；全部样本保留。

本轮基线是上一轮已测的生成质量优化快照 `ead9d016…`（runner `2539a519…`），不是把 Git HEAD 当成基线二进制。该批候选和基线在测量时均为 `ebf55740` 上尚未提交的不同实现，源码／runner指纹见末尾。[上一轮结果](codegen-quality-optimization-2026-11-06.md)保留原统计。

## 根因与实现

旧 Block 路径从入口先形成包含loop的unit，执行回边后又从loop leader独立构造整个loop。真实x87 guest的三个请求因此提交15+10+5条guest指令。新路径在现有decode记录中发现指向真实指令起点的内部直接回边，就在任何IR出生前截短原slice；第一段发布leader PC，下次以普通Block请求编译loop。因此提交量为5+10+5。没有增加第二个Block、CFG分析或中间pass；Function继续独立形成、独立分配。

| 既有文件 | 本轮职责与改动 |
| --- | --- |
| `frontend/x86/build_ir.cpp` | 复用当前decode slice完成late-backedge leader cut；短记录≤16条、单slice使用栈存储，长记录与多window正常扩容；所有用户只借用当前实际buffer |
| `frontend/x86/build_ir.h` | 明确Block入口切分与实际parser计费合同 |
| `execution/code_heap.h/.cpp` | 发布回调传本次真实RX base，使未公开self link在原完整同步中完成初始化 |
| `execution/compile_service.cpp` | 优先查已有ready entry；只有本请求root PC才能借用本次unit base，不提前公开ready、不猜extra-entry offset |
| `execution/code_link.cpp` | 把已有DATA相等去重统一到Patch，self BindReady免重复物理patch／同步；逻辑计数、relocation rows和SMC撤链仍由原owner管理 |
| `execution/code_cache.cpp` | 首次真实发布时预留现有目录的4个native rows，省去1→2→4两次中途分配与释放 |
| `tests/rebuild/x86_construction_test.cpp` | 覆盖真实5+10+5切分、Unknown、self-root／JMP／middle-byte／截断；一个长记录和多window场景覆盖两次buffer增长 |
| `tests/rebuild/arm64_compile_test.cpp` | 在原link测试中区分逻辑绑定数与真实patch回调，确认老incoming仍补丁、已预置self免补丁、两者都能撤链 |

源码路径以上均相对 `source/runtime/`，测试路径相对 `source/`。沿用Uniform IR、三个公共优化walk和原W^X／fault／SMC所有权。

内部回边切分只适用于Scope::Block、成功decode的直接Jcc／Jmp、严格内部且命中真实instruction PC的target。指向self-root、中间字节或外部地址沿用原行为。Function形成与watched Function单块分支保持原逻辑；watched Block只对实际提交prefix布防，后续loop由自己的请求管理。x87 lookahead受选中slice约束，不跨新leader融合。

切分会让某些原本一次提交的prefix+loop多一个冷请求；它减少重叠出生，不承诺所有输入的请求数和耗时都下降。例如CoreMark Block请求2041→2053，IR36575→35383、spill slots25→24；call请求45→46、IR673→677。下面同时报告收益和回退。

## 实际工作量

| x87 Block | 基线 | 当前 |
| --- | ---: | ---: |
| 编译请求／公开unit | 3 | 3 |
| 实际byte-parser调用 | 30 | 30 |
| 提交guest指令 | 30 | 20 |
| IR nodes | 65 | 51 |
| operand words | 182 | 131 |
| native bytes | 2532 | 1864 |
| 静态link rows | 4 | 3 |
| 目录／native-row实际分配次数 | 4 | 2 |
| fault sites | 1 | 1 |
| spill slots／max frame bytes | 0／0 | 0／0 |

固定FEX在该输入记录1个compile root，byte-parser probe调用22次；当前SVM Block仍是3个独立分配／提交的unit。两者形成粒度不同，以上分别给出实际工作量。

物理parser仍先读到原terminal，因此没有宣称decode调用减少。本任务排除的仅是实际parser区间，buffer操作、形成、IR出生仍全部收费。Function始终1个请求、20条guest、50N／126W、1732 native bytes；本批其compile 0.601356→0.600781 ms，基本持平，但相对本批FEX为 2.1393×，同样未过2×。

## Compile总量与完整分解

单位ms，固定五次中位数。B/F分开，不对不同workload求总平均。

| 输入 | Block前→后 | 变化 | Function前→后 | 变化 | 当前/FEX，B / F |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 42.1396→40.9727 | -2.8% | 51.2803→51.5435 | +0.5% | 0.866 / 1.089 |
| int | 1.8636→1.9048 | +2.2% | 1.9723→1.7955 | -9.0% | 1.250 / 1.178 |
| fp | 2.4934→2.2640 | -9.2% | 2.5876→2.6209 | +1.3% | 1.310 / 1.516 |
| mem | 2.2605→2.1891 | -3.2% | 2.2583→2.2766 | +0.8% | 1.383 / 1.439 |
| branch | 1.8666→2.0679 | +10.8% | 2.1486→1.9146 | -10.9% | 1.383 / 1.280 |
| call | 2.0959→1.8553 | -11.5% | 1.8677→1.8610 | -0.4% | 1.184 / 1.188 |
| func_tests | 20.9833→21.0595 | +0.4% | 33.2882→33.2767 | -0.0% | 0.689 / 1.089 |
| x87_bench | 0.6819→0.5650 | -17.1% | 0.6014→0.6008 | -0.1% | 2.012 / 2.139 |

下表取各arm总量恰为中位数的同一个样本，分项互斥并精确闭合；没有把独立分项中位数相加。单位µs。

| x87阶段 | 基线Block | 当前Block | FEX |
| --- | ---: | ---: | ---: |
| 形成与IR出生，已扣parser | 266.88 | 193.70 | 111.35 |
| IR优化 | 74.27 | 78.33 | 42.34 |
| 分配与发射 | 121.04 | 113.12 | 98.85 |
| 转交与发布 | 150.83 | 72.50 | 21.15 |
| 原生owner释放／FEX逻辑工作缓冲释放 | 49.95 | 87.71 | 2.29 |
| 其余收费工作 | 18.96 | 19.63 | 4.84 |
| 合计 | 681.927 | 565.002 | 280.833 |

SVM当前完整窗口包含native映射和实际owner释放；FEX沿用原probe，只计compile-root内逻辑工作缓冲释放，最终native arena物理销毁在其窗口之外。没有猜测补齐FEX，也没有从SVM删掉这项成本。当前与基线均只建一个1MiB dual slab，保留2MiB VA，以一次munmap释放两个alias；释放耗时变化不能解释为新添owner或系统调用。各分项时差是整体实测，不作为每个hunk的独立因果收益。

## 运行与生成质量

CoreMark为正式100000轮、CRC d340、验证成功且loop timer≥10秒；其余为whole-child elapsed，均包含冷启动与JIT。单位ms。

| 输入 | Block前→后 | 变化 | Function前→后 | 变化 | 当前/FEX，B / F |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 16129.000→16177.000 | +0.3% | 15777.000→15780.000 | +0.0% | 1.073 / 1.046 |
| int | 230.293→231.830 | +0.7% | 229.620→232.248 | +1.1% | 0.736 / 0.737 |
| fp | 249.831→249.424 | -0.2% | 250.388→248.562 | -0.7% | 0.943 / 0.939 |
| mem | 2145.775→2143.874 | -0.1% | 2142.397→2137.684 | -0.2% | 1.045 / 1.042 |
| branch | 253.784→259.254 | +2.2% | 254.236→253.222 | -0.4% | 0.959 / 0.936 |
| call | 467.499→468.260 | +0.2% | 468.393→468.152 | -0.1% | 1.024 / 1.024 |
| func_tests | 108.251→108.832 | +0.5% | 121.951→121.679 | -0.2% | 0.773 / 0.865 |
| x87_bench | 601.272→600.479 | -0.1% | 604.960→606.998 | +0.3% | 0.691 / 0.699 |

PMU计whole-child用户态，包括冷JIT；指令数变化不能单独当作稳态热体变化。

| 输入 | 指令变化 B / F | 指令/FEX，B / F | cycles/FEX，B / F |
| --- | ---: | ---: | ---: |
| coremark | +0.054% / +0.000% | 1.0384 / 1.0099 | 1.0724 / 1.0464 |
| int | -0.019% / -0.008% | 1.0159 / 1.0161 | 0.7041 / 0.7026 |
| fp | -0.326% / -0.016% | 0.9870 / 0.9901 | 0.9820 / 0.9821 |
| mem | -0.011% / -0.002% | 1.2547 / 1.2548 | 1.0526 / 1.0495 |
| branch | -0.023% / -0.006% | 1.0783 / 1.0785 | 1.0109 / 0.9805 |
| call | -0.001% / -0.002% | 1.2131 / 1.2131 | 1.0510 / 1.0500 |
| func_tests | -0.958% / -0.007% | 0.7810 / 1.0400 | 0.7781 / 0.9689 |
| x87_bench | -0.002% / -0.000% | 1.3753 / 1.3725 | 0.6765 / 0.6855 |

既定time／cycles≤1.10×、compile≤2×门槛，本批 46/48 通过，未过项：

- x87_bench／block／compile：2.0119×。
- x87_bench／function／compile：2.1393×。

## 验证、测量与保留

- 主机CTest9/9、ASan/UBSan9/9；红魔C++7/7、InterpretOnly／Block／Function状态图像3/3、workload smoke40/40通过。补充用例集中验证入口切分、溢出buffer与真实link撤销；没有新增验证pass。
- 红魔NX729J `bf1359ff`，CPU4/taskset10，既有min/max 614400／1171200 kHz不变；轮前后采样，保持系统服务原状。固定FEX `a156752fae… + original Android patch`、原guest、AVX-off／TSO配置。
- 五arm各五次，200个普通PMU运行和200个独立compile运行全部有效。新launcher、无AOT/object cache、保留OS page cache；独立FEXServer在PMU child树之外。x87 workload继承exit-only oracle，单独的ext80契约检查不等同于完整数值／ISA验证。
- 先前开发检查点完整200个compile样本、40个smoke已保留，x87Block0.613436ms／FEX0.295469ms＝2.076×；该点未跑原计划runtime，随后真实修改短buffer和初始目录容量。它是未完成campaign的开发检查点，没有与本批混池或替换样本；不能把两批时差归为SBO单项收益。最终snapshot只运行这一个固定完整campaign，没有为门槛重跑同一候选。
- x87异常位置marker同时供RaisedException的FIP、pending exception的当前servicePC、成功提交FIP使用。现有X87Execute已用满五个通用参数，本轮保留该合同；没有为约3µs预算扩全局schema或另建opcode包装层。

源码digest：`9a37021cd0cde4d04fa1bfaad358e44bbd99f3877d54f22b741660a2a8c662c0`（282项）；当前runner SHA256：`e579c6fbdadad2902e59d0df447b03c2e8c8774f4d6de3edf1bba1f8f42953b1`；测量plan SHA256：`5301273ea1ec73fdf31ae83df54a269fbc765208742f6b07b8162c80c0374b1f`。来源是 `ebf55740` 上的明确WIP快照，未伪称已提交版本。

完整原始launch／PMU／stdout／stderr、两版runner、源压缩包、patch、测试与审计位于 `/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/x87-block-compile-final-20261106`；前一开发检查点位于 `/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/x87-block-compile-optimization-20261106`。小型汇总和全部原始记录见 [evidence](evidence/x87-block-compile-20261106/README.md)。构建和大临时数据均复用Data2固定缓存；本轮functional与benchmark手机namespace已确认无活动进程／打开文件并删除，清理回执在evidence中。
