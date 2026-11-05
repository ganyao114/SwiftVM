# FEX 差距优化结果 · 2026-11-05 UTC

本轮在原八项固定输入、Block/Function 两种 scope 上达到约定门槛：运行时间≤1.10×、user cycles≤1.10×、只扣 byte parser 的 compile≤2×FEX。三次中位数的最大比值分别为 **1.07339× / 1.08874× / 1.90291×**。数值校验和原退出要求均通过，共144条有效观察。

这是这组固定输入的测量结论。动态指令数仍未全面追平：例如 FP 为1.413/1.430×、call 为1.241/1.250×，Block CoreMark branch misses 为1.937×；完整原始计数保留在证据中。完整ISA、动态ELF及一般应用覆盖不由本轮推定。

当前代码为 `13ffa48f1d984cd7d600b5c93601301462b13570` 之后的未提交工作区，位于 `/Volumes/Data2/SwiftVM-submit-20261104`。原工作树仍冻结。118文件源码 digest：`9890a5f60446f0c57aadb34bbeea2d14088ffe9e87e613774ceeb9db5e11c8cc`；测量结束逐项核对未变化。runner SHA `5f98a8ca0fc9d9a804b0681b2453316079c3ffc984df3b148ebe74270195d7cd`，8892528B。本轮是在[覆盖完成版本](benchmark-completion-2026-11-05.md)上继续优化，没有更换原七项ELF、CoreMark或golden。

## 最终实现

| 文件 | 实际收敛 |
| --- | --- |
| `runtime/execution/runtime.cpp`、ARM64 `compile.h` | 软件DAZ/FTZ作为整个native unit的已知控制位；按实际使用的4种模式分别拥有PC目录/链接，LDMXCSR返回root后切换；默认只分配用到的模式，SMC统一使所有变体失效。未知控制位调用方保留动态读取。 |
| `runtime/backend/arm64/emit_vector.cpp` | 已知默认模式直接编码packed FOP，消除逐op控制读取、复制及DAZ/FTZ分支；非默认模式保留独立输入/输出flush，未知模式仍走原路径。 |
| `runtime/execution/x87.cpp`、SoftFloat两处roundPack及局部env | 从实际舍入点返回C1方向，去掉toward-zero第二次算术；保留ext80、PC/RC、sticky和unmask提交规则。局部env由4B变5B，公开x87状态布局不变。 |
| 根CMake、ARM64 `helper_calls.cpp`、`compile.cpp` | x87及其SoftFloat闭包按general-regs-only构建；桥保存144B GPR/NZCV，向量和FP环境保持；省去每op完整bank、pin发布和FP环境切换。返回路径只调用该整数闭包，已检查目标归档机器码及未定义符号。 |
| `runtime/frontend/x86/build_ir.cpp`、`runtime/ir/x87.h`、x87 helper | 同块精确组合FXCH×2及FLD1/FSQRT/FSTP0，保留真实TOP/tag/payload、masked效果及首PC的unmask停止；两个不可变NodeId游标复用状态地址和零payload，跨块清空，正常计入分配。补实际走到的addr32 LEA。 |
| ARM64 `abi.h`、`compile.*`、`execution/native_code.cpp`、Runtime | 每个实际动态出口264B固定16项full-PC/entry缓存，复用现有LinkSite和生命周期行。未知/碰撞转原目录，只回填ready；初始化和重置entry均为本站可调用目录fallback，热路径7词。单映射保留目录路径；诊断fallback撤回一次局部hit。 |
| `execution/native_code.cpp`、容器初始化 | RW/RX双别名相邻、统一一次munmap；保留W^X及失败回退。短解码先reserve16条记录，native输出初始至少1KiB，PC目录初始32槽；实际仍按需增长，发布和释放仍完整计费。 |

表内路径相对 `source/`。本轮没有新增中间IR、分析pass或ARM64源文件；Operation24B/Block24B/Operand8B/Home16B、NativeRunContext160B、GuestState576B不变，GPR/XMM仍用中立Uniform IR。FNINIT仅在实际Uniform地址来源上直接编码协议规定的状态尾部写入，其他来源保留helper；物理x87寄存器payload保留。

所有模式的静态链接与实际动态缓存行由原owner统一失效，code arena在原安全停点退休。相邻映射只用MAP_FIXED替换自身已保留的上半区，见[Linux mmap说明](https://man7.org/linux/man-pages/man2/mmap.2.html)。

## 运行（ms）

每臂3次独立launcher、轮转顺序，表为各指标独立中位数。CoreMark100000迭代使用内部loop timer且每次超过10秒；其他项为simpleperf整子进程elapsed，含启动/JIT/wrapper。不跨workload汇总两种口径。

| 项目 | Block | Function | FEX | Block/FEX | Function/FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 16147.000 | 15221.000 | 15043.000 | 1.073× | 1.012× |
| int | 305.357 | 304.099 | 315.226 | 0.969× | 0.965× |
| fp | 263.881 | 267.033 | 263.080 | 1.003× | 1.015× |
| mem | 2154.955 | 2157.188 | 2052.785 | 1.050× | 1.051× |
| branch | 258.441 | 265.915 | 268.539 | 0.962× | 0.990× |
| call | 479.963 | 493.068 | 460.699 | 1.042× | 1.070× |
| func_tests | 109.686 | 129.871 | 139.118 | 0.788× | 0.934× |
| x87_bench * | 557.856 | 555.246 | 866.999 | 0.643× | 0.640× |

## Compile（ms）

只扣真实byte parser；形成、IR构造、优化、分配、编码、失败/retry、发布和释放均计费，Function仍包括实际Block retry。独立侵入式byte probe与普通PMU分开；本轮先完成compile，随后普通运行。CoreMark compile用20迭代、final CRC4983；普通运行用100000迭代、final CRCd340。

| 项目 | Block | Function | FEX | Block/FEX | Function/FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 47.314 | 83.744 | 47.825 | 0.989× | 1.751× |
| int | 1.768 | 1.895 | 1.517 | 1.166× | 1.249× |
| fp | 2.511 | 2.608 | 1.742 | 1.442× | 1.497× |
| mem | 2.202 | 2.457 | 1.567 | 1.405× | 1.568× |
| branch | 1.827 | 1.926 | 1.531 | 1.193× | 1.258× |
| call | 1.814 | 1.960 | 1.713 | 1.059× | 1.144× |
| func_tests | 22.853 | 39.113 | 30.649 | 0.746× | 1.276× |
| x87_bench * | 0.547 | 0.491 | 0.288 | 1.903× | 1.708× |

CoreMark实际native单元（含DATA）为390348/647156B，覆盖完成轮为316908/511796B；增加的出口缓存数据属于实际运行资源。仍为一个双映射slab、2MiB虚拟保留；spill slots25/42、最大frame48/64B保持原值。Function实际705次尝试=700 Function+5 Block retry，未删失败成本。

## PMU与正确性范围

| 项目 | cycles/FEX（Block / Function） | instructions/FEX（Block / Function） |
| --- | ---: | ---: |
| coremark | 1.073 / 1.015 | 1.038 / 0.978 |
| int | 0.991 / 0.990 | 1.224 / 1.267 |
| fp | 1.054 / 1.054 | 1.413 / 1.430 |
| mem | 1.057 / 1.060 | 1.333 / 1.366 |
| branch | 1.003 / 1.041 | 1.138 / 1.158 |
| call | 1.078 / 1.089 | 1.241 / 1.250 |
| func_tests | 0.824 / 1.078 | 0.828 / 1.206 |
| x87_bench | 0.626 / 0.624 | 1.273 / 1.270 |

红魔NX729J/bf1359ff、CPU4/taskset10；min/max614400/1171200kHz，温度本轮25.8°C，未改频率或服务，OS页缓存保留、AOT/object cache关闭、TSO配置保留。PMU为目标子进程树的user cycles/instructions/branch misses，含cold JIT及相同wrapper；树外FEXServer不计入。硬件能力沿同boot的原auxv证据，AFP bit20为0。

固定FEX revision a156752fae3a4f65fefb1ed95af980bcdfac43d7，普通loader SHA8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175、byte probe SHAd2e4ce32b6c650bb9f7ce46a91aff8a098431ad3f5f52f52fd746d30b3500995，配置及部署文件前后相同。

Host五组CTest、Android264816后端checks、native pipeline均通过。有限真实guest验证同PC经历DAZ/FTZ四模式、回默认以及修改MULPD为ADDPD后的SMC全变体失效。浮点动态未知路径与已知模式均保留独立控制；x87组合与原序列做完整144B状态对照，C1检查包含PC53的nearest/upward结果。

* x87原benchmark只有exit0 oracle；额外有限精度/状态合同不等于完整x87 ISA验证，未屏蔽客体异常递送仍按原子集边界停止。SVM和固定FEX均保留ext80，没有改为reduced precision。

## 过程及证据

实测否决了单项缓存与永久退出策略，最终源码已删除其状态机和自修改缓存指令。中途版本保留在Data2供审计，最终成绩只来自当前冻结版本，没有拼接其他变体的快样本。

运行记录保存时遇到Data2 ENOSPC：前49条完整记录原值保留；一条已执行但未能保存的CoreMark Block样本没有进入成绩，随后仅补剩余23条。源码和手机14个输入SHA未变，频率边界未变。保存脚本随后改为原子替换JSON，见[disk-interruption-receipt](evidence/parity-gap-optimization-20261105/disk-interruption-receipt.json)。

[汇总与门槛](evidence/parity-gap-optimization-20261105/summary.json)、[runtime记录](evidence/parity-gap-optimization-20261105/runtime-records.json)、[compile原始分项](evidence/parity-gap-optimization-20261105/compile-records.json)、[原始输出](evidence/parity-gap-optimization-20261105/raw-output.jsonl)、[身份收据](evidence/parity-gap-optimization-20261105/receipt.json)。Data2完整源码快照、脚本和最终压缩runner保留于 `/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/parity-gap-optimization-20261105/attempt8`。

手机私有目录已确认无活动进程/打开文件后清空，allocated回收41333760B，df实际增加42582016B，remaining0且无错误。Data2清除了本项目已退役框架的旧中途二进制，allocated释放5334040576B，df独立实增5335826432B；保留原源码/Git/WIP、基线combined-02、末版combined-26及全部结论/日志，见[清理回执](evidence/parity-gap-optimization-20261105/disk-cleanup-receipt.json)。本轮没有提交或push。
