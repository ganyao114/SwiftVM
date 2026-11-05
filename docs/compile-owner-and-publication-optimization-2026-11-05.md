# 编译容器复用、发布同步与 Function 覆盖审计 · 2026-11-05 UTC

本轮落实[上轮分析](fp-instruction-and-compile-cost-2026-11-05.md)的容量复用和新unit发布同步优化，并完成实际Function覆盖审计。CoreMark compile从 **47.125/83.862ms → 44.535/81.982ms**（Block/Function），当前FEX **47.750ms**；SVM自身降幅 **5.49%/2.24%**。这是固定开销改善，尚未消除大部分Function重复构造。

原8项固定输入、双scope、各臂3次中位数，72条runtime和72条compile有效记录全部保留；只扣真实byte parser，其他形成/IR/失败/retry/发布/释放成本仍在。最差runtime/user cycles/compile为 **1.07238× / 1.07925× / 1.90584×FEX**，全部保持原1.10×/1.10×/2×门槛。短案例有波动或回退，未挑样或据此宣称全面加速。

## 1. 实现与文件职责

| 文件 | 本轮改动 |
| --- | --- |
| [ir/builder.h](../source/runtime/ir/builder.h)、[builder.cpp](../source/runtime/ir/builder.cpp) | 新请求可消费已结束Program的三组vector容量。Storage本体仍重新创建，保留const scope/target/full-u64 unit ID；旧borrow结束后移动容量并clear逻辑内容，没有重建旧请求身份。每请求这一个Storage分配仍在，未声称全部分配消失。 |
| [frontend/x86/build_ir.h](../source/runtime/frontend/x86/build_ir.h)、[build_ir.cpp](../source/runtime/frontend/x86/build_ir.cpp) | 三个现有构造入口可选借用Program*作容量来源；仅在真正IR构造开始时消费，formation失败不消费。默认调用方仍独立拥有返回Program；拒绝路径仍返回空owner，不保留半成品可执行IR。 |
| [backend/arm64/compile.h](../source/runtime/backend/arm64/compile.h)、[compile.cpp](../source/runtime/backend/arm64/compile.cpp) | 现有Compile增加写入已有CompileResult的重载；每次先清错误、位置、frame/spill、words/faults/links逻辑内容，再执行原分配与编码。旧返回值API调用同一实现。长前向分支重发及Rejected仍清理当前草稿。 |
| [execution/runtime.cpp](../source/runtime/execution/runtime.cpp) | 一个Process直接持有ir_reuse和compile_output；源码Program最后borrow结束、native已复制/发布后保留实际容量。IR保留门8MiB、words4MiB、links64KiB，超大成功请求正常完成后放弃保留；最终明确在现compile/code_release Timer内释放。faults继续move给原CodeArena。 |
| [execution/native_code.h](../source/runtime/execution/native_code.h)、[native_code.cpp](../source/runtime/execution/native_code.cpp) | Publish可用只读ready-target回调；仅在有可撤链RWalias的双映射上，未公开写入期间把ready静态DATA/head补好，复用原最终整段双alias同步。公开旧代码的Patch/Unlink继续即时同步，动态16项缓存初始化不变。 |
| 同一Runtime的CodeCache::AddLink | 仍记录每条实际relocation及退休关系；若DATA已是ready entry，省去重复patch/sync，原解析链接计数仍记一次。pending waiter以后转ready时保持原即时补丁。单映射仍走原cold目录路径，避免创建无法SMC撤销的直连。 |

没有新源文件、IR形式、pass、按节点分析表或第二份IR。Operation24B/Block24B/Cell8B/Home16B、GuestState576B、NativeRunContext160B不变；Uniform GPR/XMM与Block/Function独立分配方式不变。容量门约束成功后的留存，不是单次请求峰值硬限；最终失败和回退的暂留草稿按原请求/Process生命周期释放。

当前CoreMark冷诊断观察到的真实最大留存为 **159856B IR +20512B words +1024B links =181392B**。这是该输入的实际值，不替代上述一般留存门。

## 2. Compile 全组结果（ms）

CoreMark compile用20迭代、CRC4983；运行用100000迭代、CRCd340。compile与普通PMU分开，每次独立launcher；没有把本轮独立覆盖诊断的计时拼入成绩。

| 项目 | Block | Function | FEX | Block/FEX | Function/FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 44.535 | 81.982 | 47.750 | 0.933× | 1.717× |
| int | 1.843 | 1.760 | 1.489 | 1.238× | 1.181× |
| fp | 2.443 | 2.431 | 1.682 | 1.452× | 1.445× |
| mem | 2.145 | 2.208 | 1.637 | 1.310× | 1.348× |
| branch | 1.918 | 1.768 | 1.673 | 1.146× | 1.057× |
| call | 2.057 | 1.856 | 1.611 | 1.276× | 1.152× |
| func_tests | 22.370 | 38.517 | 30.354 | 0.737× | 1.269× |
| x87_bench | 0.567 | 0.529 | 0.298 | 1.906× | 1.778× |

CoreMark分项使用每臂总时间处于中位数的同一条观察，FEX取exclusive remaining_ticks，避免父子stage或各自中位数重复加总。SVM“分配＋编码”包含RA，不能只与FEX backend字段直接相比。

| CoreMark阶段（ms） | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 形成＋IR，扣byte parser | 11.408 | 28.604 | 23.196 |
| IR优化 | 9.031 | 19.576 | 6.428 |
| 分配＋编码 | 14.534 | 28.554 | 13.853 |
| 传输＋发布/链接 | 7.600 | 4.215 | 3.234 |
| 各自生命周期阶段 * | 0.420 | 0.436 | 0.323 |
| 其余窗口内开销 | 1.542 | 0.597 | 0.716 |
| 合计 | 44.535 | 81.982 | 47.750 |

* SVM此处是native arena/cache及保留草稿owner实际释放；FEX是decoder/IR/temp working-buffer逻辑DelayedDisownBuffer，不是最终native arena munmap。本轮没有改变这个已声明的窗口差异，也没有将保留容量的释放藏到窗口外。FEX最终native arena销毁仍未独立测量。

## 3. Function 跨unit重复：实际数据

另派生两份只增冷日志的runtime/build_ir副本，链接正常缓存中的其他输入，生产源和普通runner不变。在手机上只运行一次CoreMark20，完整705次尝试日志与普通轮的700发布、32159 guest、87595返回节点/153728words闭合；全部FP mode0，无跨模式的合理复制。日志有明显扰动，其约600ms计时全部排除QoR。

| 指标 | 实测 |
| --- | ---: |
| 成功单元 | 695 Function +5 Block fallback =700 |
| 成功单元guest指令出现次数 | 31990 |
| 同mode去重后的guest指令 | 16101 |
| 跨unit重复指令出现次数 | 15889（约49.7%） |
| 成功字节出现次数/去重字节 | 135659 /62142 |
| 重复字节出现次数 | 73517（约54.2%） |
| 完全被先前unit覆盖的后续unit | 18（输入3590节点） |
| 五次失败已发射prefix | 169条指令、251个实际出生节点 |
| 总实际出生节点，含失败prefix | 87846（普通ir_nodes87595明确不含失败prefix） |

每个成功unit内部没有重复指令；重复来自不同请求入口形成相交子图，主要范围为`__vfprintf`。例如`0x25ce1e–0x25cf3c`的54条指令被14个unit构造，多出702次编译出现；这是compile重复，不是guest动态执行次数。离线Capstone5核对3433个唯一范围的count/end，所有边界与记录一致。

五次失败均发生在前端发射：3次`__cpu_indicator_init`里的XGETBV，2次`__dtoa`里的SHRD。formation均成功，未发生optimize/backend失败；失败形成473条指令，发射169条，所以32463 decode calls与32159 emitted之间的304也闭合。失败实际成本仍包含在普通compile Timer中；计数不证明其独占耗时，但169/251的规模不支持把主体膨胀全归给回退。

初次host诊断判定误用了长运行的“Correct operation validated”文本。20迭代原本会因不足10秒输出Errors detected，实际目标exit0/CRC4983正确；随后按原compile oracle核对，**没有重跑或改ELF**。原stdout/stderr和判定更正保留。

## 4. 后续结构性空间与当前入口边界

现有cache只持有合法完整unit入口。审计时已ready的实际B起点仅19处、直接涉及502条指令；这个数不是整图剪枝上限，也不能把15889次重复全部认作立即可删。内部标签可能依赖原frame、跨B SSA和flags状态，直接登记会绕过真实入口合同。本轮没有为少量ready边界新增截断回调、内部入口表或证明层，也没有收缩Function scope。

固定FEX确实提供更具体的入口组织参照：Frontend root与CALL之后的continuation组成候选集合，只对实际形成的B标EntryPoint；JIT为每个外部entry执行EmitEntryPoint，设置header/检查/同unit spill frame，内部边跳过序言；Core逐项登记CompiledCode.EntryPoints。它没有在形成中普遍查询已编译cache来剪图。对应固定源码位置：Frontend.cpp1037–1039/1271/1435，JIT/JIT.cpp803/899–926（实际emplace在913），Core.cpp908–910；RA每B重置动态寄存器集合、dispatcher每B重开flags/register cache。

因此下一步有价值的方向是**CALL continuation的合法多入口及其缓存登记**：先明确完整入口序言、frame/SSA/flags和fault状态合同，再复用同一个真实native unit/range owner。它应消除重复请求，而不是发布所有内部label或再造一套inputs/actions/certificates。当前没有FEX同口径覆盖trace，不能仅凭SVM重复量把两者全部compile差额归给它，或承诺除二收益。此入口合同扩展尚未实施。

## 5. 运行与生成质量复核

全部8项的compile尝试数、guest指令量、IR/words、native bytes、spill和frame与上轮逐项一致。CoreMark仍2043/705次尝试、native389876/646764B、spill25/42、最大frame48/64B。保留容量与发布时机没有改变指令选择或Uniform IR。

| 项目 | Block运行（ms） | Function运行（ms） | FEX运行（ms） |
| --- | ---: | ---: | ---: |
| coremark | 16135.000 | 15282.000 | 15046.000 |
| int | 303.258 | 302.637 | 315.156 |
| fp | 250.286 | 248.195 | 262.493 |
| mem | 2151.630 | 2180.471 | 2051.075 |
| branch | 257.590 | 258.639 | 267.950 |
| call | 478.686 | 476.649 | 458.777 |
| func_tests | 109.431 | 124.577 | 137.971 |
| x87_bench | 557.462 | 556.207 | 871.393 |

FP动态指令仍为FEX的0.990/1.006×。这只确认固定输入/现有数值合同，其他工作负载的指令数差距仍在；x87仍为ext80且原benchmark只提供exit-only oracle，不声称完整ISA或所有应用资格。

Host五组CTest通过；Android264823后端checks、12base+4170 native pipeline路径通过；原8ELF/golden、DAZ/FTZ四模式/回默认及SMC全变体检查均通过。新增场景集中验证容量复用后stale value/block拒绝、fresh完整unit身份、失败后输出不暴露旧代码，以及未公开ready链接/单映射撤链边界。

红魔bf1359ff/NX729J、CPU4/taskset10，min/max614400/1171200kHz、温度25.6→25.8°C；未改频率或服务。CoreMark使用内部loop timer且全部100000迭代运行超过10秒，其余使用simpleperf整子进程elapsed，含cold JIT/相同wrapper，不跨workload平均。固定FEX a156752fae3a4f65fefb1ed95af980bcdfac43d7及原Android patch/config、AOT/object cache关闭和TSO配置保留，普通/byte-probe loader分开。

最终118文件digest `842e4748e3263f6f50a28c53e1b8bdbb410bde70ae754940610ce43ed7eaed9b`，普通runner SHA `f7a345a603b1a9c16fd059504a09aeeb76df96f2da676828f723a734323ee548`。代码仍在`13ffa48f1d984cd7d600b5c93601301462b13570`之后的未提交工作区，未push。手机私有目录含诊断runner已清空，remaining0，无活动进程/打开文件/错误。Data2固定缓存复用，未触碰冻结树或其他项目。

[完整汇总](evidence/compile-owner-20261105/summary.json)、[exclusive成本分解](evidence/compile-owner-20261105/compile-breakdown.json)、[前后对比](evidence/compile-owner-20261105/comparison.json)、[Function覆盖审计](evidence/compile-owner-20261105/function-audit.json)、[本轮源补丁](evidence/compile-owner-20261105/changes-from-fp-optimization.patch)、[普通原始输出](evidence/compile-owner-20261105/raw-output.jsonl)、[身份回执](evidence/compile-owner-20261105/receipt.json)。完整源码、脚本和压缩runner位于Data2 `reconstruction-20261102/compile-owner-optimization-20261105`。
