# FP 指令优化与 compile 成本分析 · 2026-11-05 UTC

本轮先确认并解决 FP 动态指令差距，再分析 compile。最终仍用原8项固定输入、Block/Function双scope，各臂3次中位数；72条runtime和72条compile记录全部有效。运行/user cycles/compile最大比值为 **1.06629× / 1.08761× / 1.92737× FEX**，保持此前1.10×/1.10×/2×门槛。

## 1. FP 根因与修复

实际guest热循环 `0x400ff0–0x4010cc` 有14条packed MUL/ADD、4条LT比较及AND/ANDNOT/OR掩码组合。当前SVM默认模式和固定FEX的packed算术/比较均直接发单条NEON；不依赖AFP。此前剩余差距来自数值FP的状态合同和分配规则，不是加乘编码本体。

1. [opcodes.inc](../source/runtime/ir/opcodes.inc) 把数值FP标为Unknown，EffectsFor会展开成全部effects。结果是每个FP被当成任意状态/flags/memory观察者：清Uniform缓存、取消延迟pin发布，并让入场PC保持可见。它与已有“读active FP环境、不建模MXCSR sticky/trap、不读写guest整数flags”的数值合同不符。
2. [allocate.cpp](../source/runtime/backend/arm64/allocate.cpp) 的vector read-before-write白名单遗漏packed加减乘除、已支持的scalar64算术和比较掩码，阻止旧输入pin在最后一次读取处安全复用。
3. 修复为已有 `Effect::ReadHostState`，保留对caller FP环境的依赖；补实际安全的读完再写白名单。没有增加effect位、pass、IR形式或按节点的分析表。未知软件DAZ/FTZ仍在实际读取口动态取控制字；其独立内存字段与private pins分离的既有绑定要求已写明。

[离线词流](evidence/fp-instruction-compile-cost-20261105/loop-code-summary.json) 保留完整相同guest窗口：IR仍72节点、无spill；Block native含cold/DATA从97词降为69词，首条件分支前76→48词，其中25条向量MOV→0，并去掉3条多余入场PC发布。这个prefix计数不代替动态测量，也不包含条件分支后的实际transfer。算术仍为14条独立FOP，没有用FMA改变两次舍入；未引入只针对该kernel的掩码重写。

| FP 指标 | 之前 Block / Function | 最终 Block / Function | 最终 FEX |
| --- | ---: | ---: | ---: |
| 动态指令数/FEX | 1.413 / 1.430× | 0.991 / 1.006× | 1× |
| 动态指令数（百万） | 374.708 / 379.173 | 262.713 / 266.874 | 265.171 |
| 运行（ms） | 263.881 / 267.033 | 248.749 / 250.087 | 262.898 |
| user cycles/FEX | 1.054 / 1.054× | 0.983 / 0.986× | 1× |

之前列来自[上一完整轮](parity-gap-optimization-2026-11-05.md)，最终列全部来自本轮冻结源码，不拼接快样本。4百万次guest循环少28条/次可解释约1.12亿条动态指令减少；原始PMU含相同cold JIT/wrapper，保留其实际开销。耗时降幅约6%，远小于指令降幅；FP依赖链、访存及TSO barrier仍在，当前数据只支持这些仍可能限制吞吐，不把IPC或总指令数当作耗时的单独原因。

## 2. 已顺手消除的 compile 重复构造

[build_ir.cpp](../source/runtime/frontend/x86/build_ir.cpp) 原来为Function Jcc的两个外部PC各建一个 `LoadImm + StoreUniform + Dispatch` 退出块，随后交给优化、CFG/RA调度和编码处理。小suite的真实PT_LOAD为RWX，Runtime为SMC把实际边全部外部化，所以这里没有内部CFG需要保留。

Known target已接受ConditionalUniformStore，且taken/fallthrough确实都external时，现直接构造已有 `StoreUniformSelectImm + Dispatch`；Program仍是Function，仍走AllocateFunction。任一边internal、Unknown/EarlyOff保持原Function图。后端仅移除实际PC-tail准入的scope门，继续检查真实末op、Dispatch、PC字段、same-block earlier predicate及真实use/barrier。SMC、动态出口及旧owner失效方式不变。

| Requested Function | IR节点：之前→最终 | operand words：之前→最终 |
| --- | ---: | ---: |
| int | 691→601 | 1159→1099 |
| fp | 1521→1419 | 2773→2705 |
| branch | 691→601 | 1184→1124 |
| call | 743→659 | 1229→1173 |

这是消除真实中间工作，不是把Function计为Block。CoreMark大多数实际CFG不符合该条件：Function节点87622→87595，总compile仍约84ms；本轮没有宣称该小改动解决全部compile差距。

## 3. 最新 compile 对比

只扣真实byte parser；形成、IR构造、优化、分配、编码、失败/retry、发布和释放都按原窗口计费。CoreMark compile固定20迭代/CRC4983，普通运行100000迭代/CRCd340。

| 项目 | Block（ms） | Function（ms） | FEX（ms） | Block/FEX | Function/FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 47.125 | 83.862 | 47.043 | 1.002× | 1.783× |
| int | 1.790 | 1.880 | 1.512 | 1.184× | 1.243× |
| fp | 2.468 | 2.468 | 1.634 | 1.510× | 1.510× |
| mem | 2.267 | 2.302 | 1.580 | 1.435× | 1.457× |
| branch | 1.911 | 1.789 | 1.478 | 1.293× | 1.210× |
| call | 1.914 | 1.827 | 1.562 | 1.225× | 1.170× |
| func_tests | 23.976 | 39.716 | 30.108 | 0.796× | 1.319× |
| x87_bench | 0.553 | 0.477 | 0.287 | 1.927× | 1.664× |

不能拿SVM backend直接除以FEX backend：前者含RA，后者将RA放在pass manager中。下面每臂选择“总compile为中位数”的那一条观察做可加总分解，不把各stage独立中位数相加。FEX使用原始remaining_ticks（exclusive），全部阶段闭合到root减byte_decode。

| CoreMark阶段（ms） | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| 形成＋IR，扣byte parser | 11.965 | 29.619 | 21.987 |
| IR优化 | 9.571 | 19.591 | 6.822 |
| 分配＋编码 | 14.541 | 28.640 | 14.165 |
| 传输＋发布/链接 | 8.108 | 4.662 | 3.043 |
| 当前生命周期阶段 * | 0.408 | 0.368 | 0.293 |
| 其余窗口内开销 | 2.532 | 0.981 | 0.732 |
| 合计 | 47.125 | 83.862 | 47.043 |

* 两者当前活动不同：SVM包含native arena/cache owner实际释放；FEX该stage是decoder/IR/temp working-buffer的DelayedDisownBuffer，不代表最终native arena munmap。FEX逻辑release已从所有父stage的exclusive分解中扣开，没有重复计入形成或tail copy。其native arena最终销毁没有独立测量，不能把这一列的差全解释成相同工作效率差。

Function实际705次尝试=700 Function+5 Block retry，提交guest指令32159，IR87595节点；Block为2043次、12407条guest指令、36651节点。Function虽发布unit少，总提交guest工作约2.59×、节点约2.39×；它的总成本不能只归咎于RA或编码器。当前计数证明工作量增长，尚不能单凭总数证明多少范围重叠是可消除的，不能为降低统计值直接收缩Function覆盖。

## 4. 剩余优化空间与优先级

| 优先级 | 具体工作 | 代码/FEX依据 | 必须保留的边界 |
| --- | --- | --- | --- |
| 1 | 复用现有owner容量，先words/links，再考虑IR三组vector。实际替换逐请求new/grow/free，不能只加reserve。 | Builder每次新建Storage及operations/operands/blocks；CompileResult words/links也随请求销毁。FEX Core的ReownOrClaimBuffer/ResetWorkingList、IREmitter游标复位、JIT TempAllocator和RA成员容器复用容量。CoreMark Block2043个非空成功unit，仅Storage及三组IR容量就至少有8172个分配起点，不是8172次系统调用；分配耗时尚未细分。 | 每次仍换full-u64 unit身份；旧borrow先结束，faults仍交CodeArena持有，单Process/线程一个实际scratch owner；限制高水位内存。不要为复用再建第二份IR/封装层。 |
| 2 | 把新unit尚未公开期间的ready outgoing-link补丁放在最终整段cache同步之前，合并重复维护。 | CodeArena Publish先整段双alias clear_cache，Runtime AddLink随即对ready target逐word双alias clear_cache；FEX在temp emission/迁移后做最终ClearICache。当前Block发布约8.11ms vs FEX3.04ms，值得测量，但并非全部差值都属于cache维护。 | 最终sync完成后才能InsertReady/首branch；旧已公开unit的pending补丁和SMC unlink继续即时同步；保W^X、单映射fallback及原生命周期owner。需数实际符合条件的outgoing补丁，不能拿全部patched_links当收益。 |
| 3 | 单独审计Function实际覆盖与重复提交，再决定形成策略。 | 当前guest/IR总工作显著大于Block；形成＋优化＋C几乎占满Function预算。FEX编译单元形成和寄存器缓存值得借鉴。 | 先用实际范围/已有形成记录证实重叠，保留Function代码质量与Block独立分配。不能根据attempt数或一项benchmark直接缩小scope、跳过真实失败成本。 |

IR优化也有9.57ms vs FEX6.82ms的实际空间，但现三次walk分别处理需求/改写/最终DCE，不应为了减少“pass数量”强行合并。应先少构造无用IR、复用容量，再按真实热点看操作数解码/Uniform cache工作。上述剩余候选本轮尚未实施或细分计时，不承诺倍数收益。FEX也先写临时代码再memcpy迁移，SVM的一次words拷贝不能单独定为差距根因。

## 5. 验证、范围与证据

Host五组CTest、Android264816后端checks、12base+4170 native pipeline路径通过；原八项ELF和golden未变。真实guest仍通过四DAZ/FTZ模式、回默认及SMC全变体失效。已有未知模式路径保留动态读取，数值FP未新增sticky/trap语义；x87依旧ext80。x87原benchmark仍只有exit-only oracle，有限补充合同不构成完整ISA或一般应用资格。

红魔bf1359ff/NX729J、CPU4/taskset10、user PMU，min/max614400/1171200kHz、温度25.6→25.7°C，未改频率或服务。CoreMark用guest loop timer，其余用整子进程elapsed，含cold JIT和相同wrapper；不跨workload平均。固定FEX a156752fae3a4f65fefb1ed95af980bcdfac43d7及原Android patch/config不变，AOT/object cache关闭、TSO配置保留。当前boot与上轮一致、AFP=false；本轮结果不依赖AFP。

当前工作区仍在 `13ffa48f1d984cd7d600b5c93601301462b13570` 后，未commit/push。最终118文件digest `83d2985d2573a929ce4bbeca1ca611045ad563bdf68c3362cf72274ec5af70fe`；runner SHA `59c8a277505638b751cd754ea55ea02484c8f0130f3fd5393111802610e87151`。源码、普通/byte FEX loader及14个部署输入均保留身份核对，手机私有测量目录已安全清空（remaining0，无活动写入/错误）。原冻结树和其他项目未动。

[汇总](evidence/fp-instruction-compile-cost-20261105/summary.json)、[独立compile分解](evidence/fp-instruction-compile-cost-20261105/compile-breakdown.json)、[实际工作量差](evidence/fp-instruction-compile-cost-20261105/work-diff.json)、[原始输出](evidence/fp-instruction-compile-cost-20261105/raw-output.jsonl)、[身份回执](evidence/fp-instruction-compile-cost-20261105/receipt.json)。完整脚本、源码快照及压缩runner位于Data2的 `reconstruction-20261102/fp-instruction-optimization-20261105/final`；上一FP专项观察单独保存在同级 `fp-stage`，没有拼入最终成绩。
