# 生成质量追平 FEX：实施进度

本轮固定输入量化目标已完成。当前已测checkpoint为第三十七批：Block/Function运行对FEX约1.081608×/1.027933×，cycles约1.081088×/1.030170×，指令约1.027347×/0.986519×，均≤1.10×；去byte完整compile约1.018332×/1.791281×，均≤2×。集中功能/故障检查通过，aggregate receipt八项全真。该结论限原版Bionic/CoreMark、记录红魔CPU4和配置；spill25/42、maxframe48/64B保持，first-eviction/20B候选未实施且不是达标前置。普通PMU、byte-clock诊断与offline词流分域，旧批次历史保留。

## 第一批已完成

- GPR 保持 Uniform IR。固定缓存记录完整父值覆盖和最近派生 view，区别低32快照与 EAX 完整零高写。
- 统一实际 Uniform→pin 窗口、裸 RHS 最后读取复用；直接选 UXTW、CSEL W、窄 Add/Sub shifted RHS。
- 原三次扫描/u64N 内消块内死 flags，末逆扫联动需求、DCE 和 use 账；跨块出口仍 All。
- dispatcher 非 S 比较，只有 cold miss 捕获 NZCV。
- 原 links owner 对近目标 patch B26，远目标保 LDR/BR；SMC 撤链恢复指令，原 CodeArena 负责 alias/I-cache。

## 实测

原版 Android Bionic/CoreMark，100000次有效长跑，三者 CRC=`0xd340`，实际 PID 均 CPU4。去 byte compile 为3次 fresh中位，失败/retry/发布/释放仍收费。

| 指标 | 原基线 Block / Function | 首批 Block | 首批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: | ---: |
| 去 byte compile | 37.163 / 67.080 ms | 40.450 ms | 72.181 ms | 47.883 ms |
| Compile / FEX | 0.787 / 1.420× | 0.845× | 1.507× | 1× |
| 运行 | 36.943 / 29.236 s | 31.392 s | 26.905 s | 15.009 s |
| 运行 / FEX | 2.448 / 1.937× | 2.092× | 1.793× | 1× |
| 用户态指令 | 170.722 / 128.624 G | 151.133 G | 115.977 G | 61.982 G |
| 指令 / FEX | 2.754 / 2.075× | 2.438× | 1.871× | 1× |

原基线是上一轮独立有效测量。首批 Block/Function 耗时减少约15.0%/8.0%，指令减少约11.5%/9.8%。compile增加约8%，仍小于2×；公共优化耗时约6.65→8.84、13.46→18.03ms，下一批收敛重复分类。

选定矩阵 continuation 离线发射412→352 words，三处11条 signed32 multiply overflow链均已消失，低32乘积保留。它不是全应用周期收益。Function的722 Function+24局部Block重试继续计费。

## 集中检查与身份

host五项通过；手机后端1521 checks、12+4170 pipeline路径通过。原合并guest两scope exit0；匿名SMC两scope均3次写fault、5次失效并exit0。

文件别名EOF负例两scope均完成22次syscall、17次失效、3次写fault，按原合同125/取指拒绝。脚本初次误期待0，已修正判定并复用原Block结果。它仍有原pc=0取指拒绝限制，不是完整guest SIGBUS交付。

77输入摘要 `c0d9eeea736c5cabc23bac21a525e3868af0e2e405eb6644771b11e3f82ea199`；runner SHA `3f4aebcb5531c2b6c3117206854958f1bb4e992f062d294df64b3340b82bc158`。源码快照、patch、runner.gz、原始命令/结果/PMU/CPU mask及回执位于：

`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-parity-implementation-20261103/batch1`

手机namespace已检查无进程/打开文件后清理，回收35,382,272字节，剩余零。原仓库继续冻结。

## 第二批：原生 PF/AF 与实际消费口

Linked NativeNZCV 现在以 x1 保存 raw PF、W29 保存 AF 输入 xor，实际 AF 为 `(x29 ^ x1).bit4`。reserved 位留在原 packed 字段；GetFlags、显式别名、helper、Return、cold resolver 和 fatal 出口按真实合同物化。ADC/SBB 四种宽度及普通 Select/条件边使用原生运算；generic/standalone 保留 packed ABI。Home 仍16B，在原分配逆扫内用 spare bits 记录局部未观察 NZCV 的 clobber 许可。

优化器减少重复 effects/flags 分类；单 use ZeroExtend32 只向明确的 W 数值消费者转发。三次 N 扫描和一份 u64/N 不变。

| 指标 | 第二批 Block | 第二批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去 byte compile，3次 fresh 中位 | 41.518 ms | 71.919 ms | 48.029 ms |
| Compile / FEX | 0.864× | 1.497× | 1× |
| 有效100000次运行，首轮 | 38.298 s | 24.467 s | 15.086 s |
| 首轮用户态指令 | 142.525 G | 105.688 G | 61.982 G |
| Block 定向有效复核 | 29.331 s / 142.524 G | — | — |

Block 两次指令工作量相同，周期分别为44.315G、33.687G，运行波动原因尚未闭合；两次原始结果均保留，不能据较快一次宣称稳定收益。Function 本轮为 FEX 的1.622×，仍未追平。两scope compile 均保持小于2×；B阶段中位较首批降至7.54/15.60ms，backend/publication成本增加抵消部分收益。分段中位数不可直接相加。

为定位 Block 周期异常，只补了一轮当前镜像 Block 周期采样并保存 RX。周期归类近似：dispatcher 17.34%、copy/extract 30.46%、raw flags/PSTATE合计12.25%、PC发布3.68%；采样落点不能直接当可删除成本。30000次采样只作热点诊断，9.425s未达CoreMark有效计时要求，不能替代上述有效100000次结果。

host五项、手机后端1548 checks、原12+4170 pipeline路径及formatter/SMC合并检查均通过；linked合并用例实际覆盖PF-only/AF-only、reserved、ADC8、SBB32与旧SSA跨snapshot保活。77输入摘要 `cd6401b420879fb1cf01da21ea627870282dac6fac0e5fcd46b33e156ccd5cce`，runner SHA `cc546b9cca43f68d0b1175f4914b175f8a671c62df62f450f27486664d6aadb6`。证据在同目录 `batch2`。手机目录已清理，回收38,728,704字节。

## 第三批与当前工作

第三批已接通 Block 最终 PC 选择的唯一 predicate 直消费，复用 Home::terminal_only，与 Function 共用条件准备/恢复；额外 use、flags/state 写入保留原路径。Function 在首逆扫归约每B的gen/kill与最多六个出口writer，沿真实terminal边求保守需求；末逆扫用冻结out重新联动DCE/use账。TestFlags/condition/ADC/SBB读取需求已细化；GetFlags、故障、helper、未知、别名及BranchOnly仍保守。新增数据按B/真实边收费，Block无块域表；它不声称RFCE+DCE全局闭包。

第三批host五项、手机后端1617 checks、原pipeline及formatter/SMC两scope通过。原Bionic短输入的3次fresh去byte compile中位为Block40.197ms、Function73.675ms、同轮FEX47.169ms，即0.852×/1.562×；CRC一致。源码摘要 `d1e58ccedfd07623d0e038b871d969020d585b14fa2f65a2cb4ec4d2240373ff`，runner SHA `a9116c30c8763d12e8cddd6e984cbeb3466aa3ccd81fdcf59a31d9f53bb0a66d`；证据在 `batch3`。完整运行对比尚未测量。

## 第四批：静态 links 冷 PC 发布

Process 统一采用明确的 deferred-PC ABI，真实 compile 提供 entry_pc；实际 PC Uniform 重叠、partial/vector 访问、helper/Return 等观察单元在入口物化。动态目标和默认 public 编译保原发布。原分配 walk 内识别 PC-only LoadImm，保真实 use 与额外 use fallback，不新增 N 级表。

静态 links 保持16B记录，outer LDR/BR/DATA 后接本边 cold stub。CodeArena 在可写阶段单轮执行 L 个重定位与必要边界检查，无 RW alias 的 W^X 路径也正确。近 B/远 LDR 已解析边跳过 PC 准备；原 owner 撤链恢复到 RX DATA+8 的 stub，按原 active/退休合同回收。没有新的链接 cache/manager。

| 指标 | 第四批 Block | 第四批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去 byte compile，3次 fresh 中位 | 42.356 ms | 76.524 ms | 51.275 ms |
| Compile / FEX | 0.826× | 1.492× | 1× |
| 有效100000次运行 | 24.895 s | 24.460 s | 15.049 s |
| 运行 / FEX | 1.654× | 1.625× | 1× |
| 用户态指令 | 102.229 G | 99.885 G | 61.982 G |
| 指令 / FEX | 1.649× | 1.612× | 1× |

CRC均为0xd340，实际CPU4。与第二批相比，Block指令减少约28.3%，Function约5.5%；Function有效运行时间基本持平，不能按指令减少比例宣称周期收益。与最初基线相比，两scope运行约减少32.6%/16.3%，仍未追平FEX。静态cold stubs使短输入native字节较第三批约增加50.5KB/23.5KB，这是需要继续收敛的实际体积成本。

host五项、手机后端1650 checks、原pipeline、formatter/SMC两scope及原EOF负例通过；现linked合并用例覆盖ready direct patch、目标真实PC Uniform观察、unlink后原dispatcher重跑。77输入摘要 `1f810975e42c1713fb85de44757cb3caf487e5d9ed803bd6c374d6c55a223569`，runner SHA `c40649f3a496798d3276e80d6467fb080ee871b451f9903023b856206a8cc345`；证据在 `batch4`。

## 第五批：有限 Function TEST 恢复

Function限定的真实self-loop已实现一位TEST tag，实际FaultSite绑定现存snapshot寄存器/宽度；首次Complete与回边Deferred分开，zero出口完成TESTzero。Block仍eager，tag不穿普通dispatcher，也没有新增绕过SMC撤链的本地回跳。FaultSite24→32B，Home16B、NativeCode40B不变。内层dispatcher不可变literal在同Writer的imm19范围内复用，outer links仍各自拥有原DATA/stub。

host五项、手机后端1689 checks及原合并路径通过；一个Function场景组覆盖首次Complete load fault、tagged load fault、load已覆current pin后的store fault、zero exit。原0x219700单元的离线发射与当前RX均确认反转loop为8条，两个site为64bit/reg12。它没有解释整项主要差距：

| 指标 | 第五批 Block | 第五批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去 byte compile，3次 fresh 中位 | 42.475 ms | 76.038 ms | 47.799 ms |
| Compile / FEX | 0.889× | 1.591× | 1× |
| 有效100000次运行 | 24.929 s | 24.654 s | 15.086 s |
| 用户态指令 | 102.229 G | 98.669 G | 61.982 G |

Function指令较第四批减少约1.2%，运行基本持平，不能把局部八条记作整项追平。只补了一轮当前Function周期采样/RX：dispatcher21.28%、copy/extract20.12%、PSTATE/carrier形状合计12.16%；均为周期落点近似，不能直接认作可删成本或probe概率。profile只作诊断，保留独立有效长跑为性能口径。

第五批源码摘要 `f3a570873477343f85d33bc9a3152a0f78f8daac751717172693c412b3a8f16b`，runner SHA `52d9398d622424b3ee1a87894cf42d649a8d1fb11a532ed2dfebf2dda255b571`，证据在 `batch5`。

## 第六批职责收口与当前工作

TEST recurrence matcher已从分配器退役，Allocation没有semantic候选记录。Writer在原发射过程收一个固定prefix状态，And处读取最终Home及两条尾记录后才决定；只对两个实际site写物理recipe，失败照原Emit，无回滚、新N表或额外N扫描。Home/lifetime算法不变。host五项、手机后端1688 checks、原pipeline、四个真实TEST场景及formatter/SMC通过；未为职责调整另跑完整QoR。

第六批源码摘要 `a4884be275b619103b5f1c4707fa55c4686f9af903bea5d659244542c81c4159`，runner SHA `8fed1184faad5bd632a2f239ec17bae16c7c68a6209b0b8d533cea635a3ffe2e`，证据在 `batch6`。

## 第七批：eager 窄选码与同目录收敛

仅需Zero的窄self-AND且无数值use时直接TST低位，保原Uniform快照、Prepare/Finish和未写flags；窄bare位运算的两个输入已归一时省结果重复裁剪。单PC目录只允许ready与home上的pending交换，Grow沿原单轮rehash同规则；原hash/容量/owner不变。dispatcher复用已加载entry判ready occupancy，ready-mismatch少一次byte load，pending/empty多一个条件branch。

| 指标 | 第七批 Block | 第七批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去 byte compile，3次 fresh 中位 | 42.122 ms | 76.136 ms | 47.143 ms |
| Compile / FEX | 0.893× | 1.615× | 1× |
| 有效100000次运行 | 24.317 s | 24.209 s | 15.032 s |
| 运行 / FEX | 1.618× | 1.610× | 1× |
| 用户态指令 | 100.488 G | 96.198 G | 61.982 G |
| 指令 / FEX | 1.621× | 1.552× | 1× |

CRC一致，CPU4。较第五批整批耗时约降2.5%/1.8%，仍远未追平；不能把本批收益拆认给单个feature。host五项、Android1708 checks、原pipeline、4个TEST场景、formatter/SMC及EOF负例通过。源码摘要 `aafe29e6a5dd457dd8dd8274d482eb06fb40041ea349f017788622213a1b20e1`，runner SHA `194265c850b86d9bf8be5c0fc5fe164c2405ed50a4fff1dd700e601b996fa44b`；证据在 `batch7`。

对照固定FEX，当前ELF文本落在同一个1MiB guest范围，FEX低20位、16B entry的L1在这份文本内无索引冲突；整表为16MiB，不能称其总表更小。SVM20次短输入Function census是722 ready+96pending/2048个64B槽，不偷当长采样的动态keyset。现数据不能把21%dispatcher周期全归因于hash或probe深度，故没有盲扩表/换hash/新建cache。

## 第八批文件职责与当前工作

已按方案落地同一个detail::Writer：compile.cpp1153行保入口/主遍历/标量/访存/控制，emit.h367行保私有声明、固定状态及小编码inline，emit_state.cpp591行收Uniform/raw flags/有限TEST，emit_vector.cpp375行收SIMD/FP。原定义已移除；CMake只在原svm_arm64增加两个cpp。没有新owner、阶段或中间表。

host五项、Android1708 checks、原pipeline/4fault/formatter/SMC通过。3次fresh去byte compile为Block42.776ms、Function76.561ms、FEX47.213ms，即0.906×/1.622×，与拆分前基本持平；不为机械布局重复完整QoR。80输入摘要 `60318156750f4023f3073ca7f6cb250126f1f95b2e6012286d2fb398fae3e38b`，runner SHA `57fff1ab204cdca601db4e67cb3f8ddb8c41f3f2e121fb0a05ff3ad6664749c7`；证据在 `batch8`。

## 第九批：low8/16/32 借位读取

借pin位宽已明确为low8/16/32，Home仍16B、寿命/写窗口沿原规则；RawValue/Read、UXTW门与低位consumer同步更新，完整发布仍真实正规化。不授AH或跨B借用，不增加use-permission表。host五项及Android1744 checks、原合并路径全部通过。

有效长跑Block24.652s、Function24.223s、FEX15.040s；指令100.187G/95.895G/FEX61.981G。较第七批只少约0.3%指令，没有整项速度收益，不能将挪到消费者的mask算作省指令。去byte compile42.706/76.837/FEX47.432ms，即0.900×/1.620×。源码 `3644ab29f51060367c3b9f2ba8ca20b7fe53768452986b0bed61a64901bdf483`、runner `3a2f2943d4a189d41fb7402f0f701a4d50476ab9bade65e95c22b07febbf3184`；证据在 `batch9`。

## 第十批：明确可不定义的 flags

`discard_omitted_flags`以properties bit3实现，Operation/Block/word仍24/24/8。默认false；true声明写后未请求的Flag::All位允许未指定结果，不是UB/poison或活性证明缓存，reserved保持定义。三处Builder写口及getter/equality完整往返，true+outputs0拒绝。B只在末逆扫描对当前写后不再读取其它位的LogicalZero+Zero输出授予；GetFlags whole-word/fault/alias/unknown/external需求不放宽。C只并入既有Home NZCV clobber，generic可以忽略许可、继续保留。host五项、Android1744 checks、原pipeline/真实fault/formatter/SMC全部通过。

| 指标 | 第十批 Block | 第十批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去 byte compile，3次 fresh 中位 | 43.281 ms | 78.610 ms | 47.619 ms |
| Compile / FEX | 0.909× | 1.651× | 1× |
| 有效100000次运行 | 24.100 s | 23.939 s | 15.172 s |
| 运行 / FEX | 1.588× | 1.578× | 1× |
| 用户态指令 | 99.343 G | 94.706 G | 61.982 G |
| 指令 / FEX | 1.603× | 1.528× | 1× |

有小幅整批收益，仍未追平。源码 `8e1748f398de2a00b188ee750ef978a115a6e6c5be9c29718e01cff5173e6fec`、runner `33744c16aeef913d71b80ee9a27610ca4400db1c245b291f303f9621febbcac7`，证据在 `batch10`。

只补一轮同镜像Function+FEX retired-instruction诊断采样，SVM dispatcher/FEX相关模板的sample-IP权重分别24.23%/22.07%；FEX不是零成本，当前热点大头为状态机计算跳转。**曾将这些权重乘长跑总量换算为22.946G/13.680G、差9.267G/解释28.3%；后续第十一批精确计数已否定这种绝对换算，全部撤回。**不能继续用这些数值宣称收益、占总差比例或据此重构目录。采样仍仅作定位线索，当前IR/Home/RX按实际机器字绑定。

## 第十一批诊断与当前工作

为验证实际probe，NativeRunContext只在末尾加一个u64计数（144→152B，旧0..136偏移不动）；Counted dispatcher在实际slot读取前非S累加，普通入口参数为空、无额外热指令。JSON `directory_probe_steps`仅decode_timing可用。相关host两项、Android原合并检查通过；一次原版100000长诊断CRC一致，得到：

| Scope | lookup | 实际slot读取 | 平均probe |
| --- | ---: | ---: | ---: |
| Function | 612,013,090 | 1,101,313,890 | 1.799494 |
| Block | 612,014,408 | 763,016,861 | 1.246730 |

按ordinary真实模板（首ready10条、额外ready probe8条），Function约10.034G；即便把所有mismatch再按pending路径加2条，上界约11.013G，明显低于采样权重换算22.946G。已核14个SVM采样PC均在封存ELF真实dispatcher范围并逐word一致，没有符号越界误归；两份实际事件均为hardware instructions/freq4000，但precise_ip=0。这轮sample-IP分组存在显著归因偏，不能当动态次数，也不能按同一个系数校正FEX。全程stat总量和有效耗时保留。完整范围/事件/校准见证据目录 `batch10/profile-instructions/matched-dispatch-attribution.json`。没有PC级表、新cache或epoch，也不以计数诊断时间与普通性能混算。第十一批手机namespace已检查无进程/打开文件后删除，du回收36,751,360字节、df实际增加36,806,656字节，无清理错误。

热点review排除了一处错误删除机会：0x21c3b3的UBFX对应真实MOV r9d,eax，在后续MOVSXD故障前必须提交架构R9，不能以UXTW替换名义抹掉。新的恢复与选码只基于实际语义边界推进。原仓库继续冻结，[完整方案](codegen-parity-plan-2026-11-03.md)的追平目标不变。

## 第十二批：32位出生宽度与实际部分 flags 消费

LEA32的base/index、加减/移位、Known AddShift及常量直接按中立U32表达，仍以Uniform完整槽提交零高父寄存器。Memory/TLS/stack与LEA16/64保原路径；C对实际GetOperand32、Select32使用W指令，保留宽shift和窄view的原裁剪，不全局省略MOV W。B沿原末逆扫将`discard_omitted_flags`授予`FlagWrites`已确认、写后需求只含keep的部分metadata；实际Carry输入、全字观察、fault和未知效果需求不放宽。C的flags-only self-AND支持C|Z，TST后按native carry约定反相。

离线重发射的矩阵continuation总词流322→298，23B不变、节点412→408、frame/spill均0。其中已与第十批RX绑定的旧B7为44条，新离线B7为39条；两LEA少4条，另一读视图少1条。其余4条PF/AF/carry工作在后续故障前仍须保留。state公共TEST/CMP尾9→3条；整个两个state单元各少6条。新词流没有重新捕获RX，不能把离线匹配写成新runtime逐字核验。

| 指标 | 第十二批 Block | 第十二批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 43.971 ms | 78.706 ms | 47.440 ms |
| Compile / FEX | 0.927× | 1.659× | 1× |
| 有效100000次运行 | 23.965 s | 22.955 s | 15.085 s |
| 运行 / FEX | 1.589× | 1.522× | 1× |
| 用户态指令 | 97.141 G | 89.332 G | 61.982 G |
| 指令 / FEX | 1.567× | 1.441× | 1× |

Function较第十批指令减少约5.7%、耗时减少约4.1%；Block指令少约2.2%，耗时基本持平。不能将整批收益拆认给单一改动，追平目标仍未达到。三方CRC均0xd340、实际CPU4，频率上限/当前读数1.1712GHz不变。host五项、Android1762 checks、原12+4170 pipeline/四个TEST故障场景、formatter/SMC及EOF负例合并通过。

80输入摘要`cc702c4f8f9b79fbbfe46d0dc679f0e13355d231234989f237ef5fc23f7d6d0e`、runner SHA`ff8a30b5016d24053713e0c5b52943a76497db216c2d1c9a66ebc05364a8c96d`；证据在`batch12`，逐模板说明为`flags-copy-review.md`。手机namespace已检查无活动进程/打开文件后删除，du回收36,787,200字节、df实际增加36,847,616字节，剩余零，无清理错误。下一候选为TSO未对齐修补占位的热路径成本，先核对原fault/SMC/W^X owner，不新增恢复平台。

## 第十三、十四批：同 owner 内的 TSO 冷 fallback

可恢复标量16/32/64位访存热路径只发实际有序访问；Writer末尾沿原FaultSite作一次F闭合，发固定三字fallback（load为plain+DMB ISHLD+B，store为DMB ISH+plain+B），返回原fast之后的下一字。FaultSite仍32B/同一行，`unaligned_fallback_word`替代原resume字段，冷热范围共用原guest-PC/TEST recipe。SIGBUS只在原fast处回补一条原子B；cold plain再故障按原fatal/SMC服务处理。默认无恢复、byte、V128路径不变；无新增owner、cache、N表或中间表示。

第十三批host五项通过，手机检查在新增的‘两次patch’断言停止：`head+1`没有触发预期的两次修补。第十四批只将该检查地址改为`head+15`跨16字节边界，实际确认load/store各patch一次，再验证同代码cold load与cold store的tagged致命故障及R12快照。生产镜像与第十三批完全相同，未将一次测试地址修正记为生产优化。原失败日志和源码封存保留。

| 指标 | 第十四批 Block | 第十四批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 43.423 ms | 78.590 ms | 47.887 ms |
| Compile / FEX | 0.907× | 1.641× | 1× |
| 有效100000次运行 | 23.511 s | 22.943 s | 15.035 s |
| 运行 / FEX | 1.564× | 1.526× | 1× |
| 用户态指令 | 90.327 G | 82.226 G | 61.982 G |
| 指令 / FEX | 1.457× | 1.327× | 1× |

与第十二批比，指令少约7.0%/8.0%，Block周期/耗时少约1.9%，Function周期/耗时基本持平。保留这一热布局收敛，但不能称主要周期差距已解决。固定FEX也使用NOP原地回补；这不是SVM独有成本的解释。代价是每个eligible site总静态代码增加约8B，已修补路径多两条B；本轮矩阵单元词流298→363、state单元82→98，frame/spill及IR均不变。compile仍小于2×，不能拿指令下降比例代替运行时间收益。

三方CRC均0xd340、实际CPU4、CoreMark validation通过。第十四批Android1771 checks、原12+4170 pipeline/扩展原TEST场景、formatter/SMC及EOF负例通过；host五项对应同一生产实现。80输入摘要`5c8434a4b4faf4fe0743423e521352625074685e436170f514bad4ddd77e78e7`、runner SHA`e9fa95395fadad570ed658b628c7d801bffac3187c6275bdf64881e1be0379da`。证据在`batch13`及`batch14`；两个手机namespace均检查无进程/打开文件后删除，剩余零、无清理错误，第十四批du回收36,799,488字节、df实际增加36,859,904字节。

下一步只读审查同一PC目录的热槽布局与动态尾PC发布。当前动态末尾PC写已合并Dispatch，不再多一次LDR；能省的是一次STR，且必须在dispatcher冷miss、覆盖x0之前提交目标PC。不会按‘两条’虚计收益，也不在完整批次测量中途插入实现。原仓库继续冻结。

## 第十五批：动态尾部内联同目录查找

动态完整PC写的实际消费口先读target到x0、只退一次frame，再直接发原目录的完整非S probe。各source site拥有各自的BR entry，ready不再先尾跳共享dispatcher。hash/容量/probe/ready优先规则不变；nil/pending/empty仍跳原共享dispatcher，冷路径在复用x0之前写回实际state.pc。静态links/cold stubs、Return及service出口保持原路。未知/custom ABI默认Callback；Runtime通过`DispatchMode::Inline/InlineCounted`明确选择本物理ABI，前端/GPR仍只有中立Uniform IR。

同一CodeCache的热槽64→32B，保存pc/entry/code_id/pending_head/occupied；原NativeCode移到同owner的实际ready冷行，没有第二张PC查找表或映射owner。冷分配先完成才发布ready，新增分配/复制仍在原请求计时中，原SMC active/anchor/撤链/退休顺序保持。CLI布局按sizeof输出，allocations明确包含PC槽与ready冷行。热数组减半不代表全部cache内存减半。

| 指标 | 第十五批 Block | 第十五批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 42.976 ms | 79.213 ms | 47.756 ms |
| Compile / FEX | 0.900× | 1.659× | 1× |
| 有效100000次运行 | 22.964 s | 21.946 s | 15.110 s |
| 运行 / FEX | 1.520× | 1.452× | 1× |
| 用户态指令 | 88.421 G | 80.206 G | 61.982 G |
| 指令 / FEX | 1.427× | 1.294× | 1× |
| 用户态branch-misses | 110.083 M | 63.810 M | 20.548 M |

为核对分支变化，本轮额外重跑已封存第十四批普通Function一次，仍CPU4/100000有效/CRC0xd340：22.899s、26.330G cycles、82.226G instructions、81.398M branch-misses。新Function相对同轮旧镜像耗时/周期约少4.2%/4.1%，指令少2.5%，误预测少21.6%。这是内联、布局与PC发布整批共同收益，不能据此孤立归因各feature；也不能把误预测比例直接换算为总耗时收益。代价是同轮普通Function的native_bytes从509,612增至533,308（+23,696B），峰值code映射仍2MiB。设备未提供branch-instructions事件，因此只使用实际公布的cycles/instructions/branch-misses。前后CPU4 min/max/cur为614400/1171200/1171200kHz，电池26.4→26.5°C，没有频率写入。

host五项、Android1813 checks、原12+4170 pipeline/TEST故障场景、formatter/SMC及EOF负例合并通过；新增同一动态场景覆盖packed/NativeNZCV、pending碰撞、cold→ready、真实PC观察、完整flags/reserved及Return42。三方及旧Function均validation/CRC通过。诊断统计包含inline与shared的每次真实native lookup/probe；冷重复查找如实计数，不含C++ Find内部probe，也不拿新attempt数冒充guest间接分支次数。

80输入摘要`da7191424811a542ae1cdcabf3a968469cf15a1854c021a458e0743e60a6ef9c`、runner SHA`fb46f538cd580f919decec3a425f570c9217fd992bd8b130cd76b0f1d76489ff`；证据在`batch15`。手机namespace已确认无活动进程/打开文件后删除，du回收45,118,464字节、df实际增加45,182,976字节，剩余零、无错误。目标仍未完成，下一候选为同B内PF/carry的静态故障恢复，不建立通用表达式历史或逐节点资格平台。

## 第十六批：块内 PF/carry 的实际 fault recipe

Runtime明确开启`local_flags`，默认调用者保原路。32/64位Add/Sub/Neg的实际pinned结果可以暂缓MOV到x1，原需CFINV处可保直接carry；AF输入xor仍即时保存在W29。真实读旗、partial/opaque/legacy producer、helper、物理pin覆盖及所有实际出口前统一完成。相同pin完整发布且有效view≥8、无代码借pin读不强制复制；两个fused PC出口在分叉前完成。未跨CFG维护状态，没有新tag/历史、N表或扫描。

原32B FaultSite使用尾padding保存`parity_register/carry_direct`，在实际scalar memory前记录；complex/vector或无marker MayFault先完成。fatal普通分支用选定raw PF低字节同时恢复PF/AF，按carry极性解释PSTATE.C；reserved保真，原TEST recipe优先，BUS/SMC内部重试仍不物化。原Run将一次Effects传给Emit复用，未新增节点遍历。legacy_mask也作为producer前观察口，不能等到后面的SaveFlags才补。

| 指标 | 第十六批 Block | 第十六批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 45.150 ms | 80.193 ms | 47.744 ms |
| Compile / FEX | 0.946× | 1.680× | 1× |
| 有效100000次运行 | 22.799 s | 21.897 s | 15.074 s |
| 运行 / FEX | 1.512× | 1.453× | 1× |
| 用户态指令 | 87.466 G | 79.467 G | 61.982 G |
| 指令 / FEX | 1.411× | 1.282× | 1× |

相对第十五批，指令少约1.1%/0.9%，耗时少约0.7%/0.2%；Function相对FEX仍约1.45×，不能称显著运行收益。这一局部机制兑现了fault合同下的延迟维护，但不据此扩大恢复范围或把主要时间差归咎于这几条旗指令。下一步先查固定FEX是否已有准确runtime lookup/return telemetry，替代已被否定的采样份额推算。

host五项、Android1840 checks及原12+4170 pipeline/TEST、formatter/SMC、EOF负例合并通过。新增同一程序三次真实执行：ADD后fault1为PF pin20/CF direct；MOV覆盖RAX后fault2为PF x1/CF direct；正常Return42；均按原ReferenceArithmetic核全packed/reserved、PC和RAX。三方长跑均validation/CRC0xd340、实际CPU4；前后min/max/cur614400/1171200/1171200kHz、电池26.5°C。

80输入摘要`b3988d0f95b3122a094207e392b85eee28221663116e22e6de44e3fa97aaa6be`、runner SHA`b9644785dc722881934e9d8cdc362845dd1bf44de237e080800d11a76fd4fa32`，证据在`batch16`。手机namespace已核无进程/打开文件后删除，du回收36,934,656字节、df实际增加36,995,072字节，剩余零、无清理错误。原仓库/Git/WIP继续冻结，目标保持开放。

## 第十七批：同一 Uniform IR 的 XMM 固定驻留

XMM0..15映射v16..31；前端及IR保持中立，晚期由`pinned_vector_offset`实际绑定。默认memory ABI保原路。现有逆扫统一32个固定slot，typed bank区分同号X/V，publication/borrow共用安全同点终点；Home仍16B，无新pass/N表。跨B SSA保动态或栈锚；完整slot V128直接pin copy，同pin零代码。标量、非首址和跨槽alias先发布实际重叠Q、精确访存，只有写后重载，读不伪造覆盖。

明确物理预算为12个动态V（2..7、10..15），输入spill0/1、复杂temporary8/9、spill output9。原v30/v31向量临时已迁移，GPR30和scalar d0语义保留。Root160B额外保存host d8..15低64；首次先load16Q，cold callback成功/失败均先reload，Exit发布。统一helper576B保存全部live Q；fatal读取实际baseline FPSIMD的v16..31完整256B，原SMC/BUS retry仍先返回、不改现场。NativeRunContext尾加offset152、size160；没有新的状态owner或逐site向量表。

| 指标 | 第十七批 Block | 第十七批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 44.665 ms | 81.459 ms | 47.460 ms |
| Compile / FEX | 0.941× | 1.716× | 1× |
| 有效100000次运行 | 22.695 s | 21.877 s | 14.996 s |
| 运行 / FEX | 1.513× | 1.459× | 1× |
| 用户态周期 | 26.050 G | 25.156 G | 17.240 G |
| 用户态指令 | 86.756 G | 78.901 G | 61.982 G |
| 指令 / FEX | 1.400× | 1.273× | 1× |
| branch-misses | 110.174 M | 66.639 M | 18.858 M |

相对第十六批，指令减少0.81%/0.71%，周期减少0.57%/0.06%，耗时减少0.46%/0.09%；Function耗时基本持平。这批消除了已定位的XMM state往返，但没有显著缩小主要周期差距，不能将静态publication次数当作动态贡献。20次诊断输入的实际spill slots从24/41到23/40，最大frame仍32/48B；native_bytes从325,124/541,632到323,276/537,792。普通长跑机器码为315,424/526,596B，诊断镜像的inline计数指令使其体积不同，不混用两组数据。Function仍为722次Function+24次Block retry，全部计费；普通长跑为728+24。

host五项、Android1912 checks、原12+4170 pipeline/TEST、formatter/SMC及EOF负例合并通过。只新增一个Function合并场景三次执行：跨B旧XMM SSA、partial及跨槽alias、pin14/15、8个live Q跨真实C++ helper clobber、实际PROT_NONE fatal、cold成功及失败；逐比全部XMM和128位scratch结果。三方长跑均validation/CRC0xd340且实际CPU4。前后min/max/cur614400/1171200/1171200kHz，电池26.5→26.6°C，无频率写入。

离线以本批冻结头/库重编译三个实际矩阵入口21ac80/21ae30/21b0b0，均frame0/spill0；三个循环原3/3/6个V128发布都为同pin零copy，完整unit均不再访问state XMM backing memory。原guest访存、TSO屏障和shuffle保留。这只是带准确policy的offline recompile，不宣称已与运行RX逐字绑定，也不外推动态G或耗时收益。证据在`batch17/vector-native-review.md`及原IR/Home/words。

80输入摘要`4ae23247dc0eb8c9135fad99375008499cafc2e9304e1ae4fcf739ff16f634a3`、runner SHA`b2f2c092b598458e9be52dde2fb92037e56b253132ec11d35f9d468937b5e88c`。手机namespace已确认无活动进程/打开文件后清理，du回收37,131,264字节、df实际增加37,191,680字节，剩余零，无错误。普通fixed FEX、原仓库/Git/WIP仍冻结。

后续转向真实动态退出/return与编译单元形成差异。20次输入固定FEX只有255次root compile，SVM requested Function为746次attempt；FEX会在同owner编译CALL后continuation并登记多入口，且ReturnHint predictor命中绕过L1。两个compile计数域不等价，不能据此认领动态收益。目前缺少FEX实际lookup/predictor次数，正用隔离的诊断source/build计数，普通FEX时长/PMU参考不变；不会用采样IP份额换算绝对指令。

## 第十七批后的闭合归因：动态退出和RET专属probe

隔离FEX六个LSE native计数及SVM两个真实CALL/RET tail计数已闭合原100000输入：FEX154,412,760次返回预测全部命中，inline/shared L1合计457,600,120次；SVM两scope约154,412,772次RET、612,014,000次native目录尝试。FEX动态operand退出总数612,012,846，两端约6.12亿的实际退出量接近，查找量差异来自机制。SVM计数含冷重复查找，不冒充严格guest分支总数。

Function额外一个RET inline probe计数取得199,713,375次读取，平均1.293373；全量slot读取1,101,314,727，扣除RET inline后的其余尝试平均1.970279，并承担90.7418%的extra probes（余量含少量未按hint分类的shared冷重查）。V2全量CALL/RET/attempt/probe与V1完全相同，均validation/CRC0xd340/CPU4。FEX只有一个exit_group_current最终记录，无live snapshot。诊断timing不用于QoR。

该数据促使下一批先改善原目录，不先扩返回栈/多入口框架。普通ready模板的结构估算毛差约6.214G，其中RET部分约1.289G；cold/pending和新增CALL push等未计，不能把它说成净节省或耗时收益，也不能恢复旧sample-IP份额推算。完整计数、域、身份和限制见`batch17/native-exit-review.md`。诊断phone namespace已清理：du24,155,136B、df24,186,880B、剩余0、无活动/错误；固定FEX和普通SVM镜像未被诊断改写。

## 第十八批候选：CRC索引使两scope出现取舍，已撤除新增模式

候选保原单一目录/32B slot/原half-load和probe，仅用raw seed0/完整64-bit PC的CRC32C代原FoldedPC。C++冷索引、inline和shared从同一CodeCache mode消费；只增加两个NativeNZCV CRC共享入口，不做热feature分支。实际CPU能力确认后选择；public默认保FoldedPC。原功能检查及Bionic长跑均通过。

| 指标 | Block | Function | 同轮FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile | 44.546 ms | 81.139 ms | 47.882 ms |
| Compile / FEX | 0.930× | 1.695× | 1× |
| 有效100000次运行 | 23.208 s | 20.773 s | 15.187 s |
| 运行 / FEX | 1.528× | 1.368× | 1× |
| 用户态指令 | 87.534 G | 77.405 G | 61.982 G |
| branch-misses | 108.872 M | 40.172 M | 19.012 M |

相对第十七批，Function耗时少5.05%、周期少5.05%、指令少1.90%，而Block耗时/周期多约2.26%、指令多0.90%。20次诊断平均slot从1.798→1.493、1.263→1.417，方向随scope不同。不能把CRC理论mix质量或Function收益当作全局结论，也不据此按一个输入硬编码scope专用hash。候选完整封存后，六个hash相关源恢复第十七批，新增enum/option/mode/两个thunk一并撤除，guest CRC32C指令实现仍沿原合同。

80输入digest`4454f50f550f5f992afc490e40d204b597562ed8e10a0d58d037b4866b95d8d2`；runner SHA`41762d3b9b6a7b66eb8ff6caa504bd0359098ab25006381961ed39f6e9d1303f`，证据`batch18`。Host五项、Android1912及原pipeline/formatter/SMC/EOF通过。CPU4 min/max/cur614400/1171200/1171200kHz，电池26.5°C。phone namespace已清理，du37,139,456B、df37,199,872B、剩余0、无活动/错误。

## 第十九批：保留低延迟索引，降低同一目录装载率

生产仅在第十七批基础上将原Grow阈值occupied/slots从1/2改为1/8。初始256及倍增、Slot32、ready抢pending、所有索引/probe/SMC撤回/退休规则均保留。没有新增IR/分析/pass、模式或PC副本；全部extra zero/grow/copy/release继续计费。

| 指标 | 第十九批 Block | 第十九批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 45.227 ms | 81.170 ms | 47.695 ms |
| Compile / FEX | 0.948× | 1.702× | 1× |
| 有效100000次运行 | 21.939 s | 20.520 s | 15.086 s |
| 运行 / FEX | 1.454× | 1.360× | 1× |
| 用户态周期 | 25.183 G | 23.595 G | 17.350 G |
| 用户态指令 | 85.555 G | 76.181 G | 61.982 G |
| 指令 / FEX | 1.380× | 1.229× | 1× |
| branch-misses | 95.735 M | 47.970 M | 18.938 M |

相对第十七批，两scope耗时少3.33%/6.20%、周期少3.33%/6.21%、指令少1.38%/3.45%；相对第十八批，两scope耗时也都改善。20次诊断平均slot读取为1.003755/1.241624。普通长跑实际目录从8192/2048到32768/8192槽，即256/64KiB到1MiB/256KiB，增加768/192KiB；只报告slot数组，不能冒充整个cache。cold cache/table分配计数各多2次（20/17），已含实际ready冷行分配，非全进程malloc次数。

Host五项、Android1912、原12+4170 pipeline/TEST、formatter/SMC及EOF负例集中通过，无新增测试。三方有效长跑/CRC0xd340/CPU4通过；min/max/cur614400/1171200/1171200kHz，电池26.1→26.2°C。Function诊断仍722次Function+24次Block retry，全部计费；普通长跑728+24。Block普通长跑2044个unit、native_bytes315,920，Function752个unit、526,596B；少量startup/计时打印路径差异保留，不筛选掉。

80输入digest`280dd463abf6d539e5ca565d01e95b01ac37045de9e3a45e2093ba5d08de82fc`，runner SHA`31bde30d20c74c166d65e9a12d05550677de0c86729f443dc088a146a1ff2d91`；完整源码/ELF/命令/原始输出在`batch19`。phone namespace已清理：du37,131,264B、df37,191,680B、剩余0、无活动/错误。原Git/WIP、固定FEX及Oct25协调清理收据均未改。目标仍开放；下步从当前真实Block自边与CRC窄操作继续定位，不用已撤除的CRC模式充当新框架。

## 第二十批：受限Block self TEST与实际截断消费收敛

沿原Writer两site prefix，在真实Block64/frame0/无入口PC物化且末4为TEST、snapshot发布、NE和taken=self的PC选择时启用既有tag。保原可撤销self Link；self冷fallback第一组从实际next pin补完整TEST，zero边独立补同一完整TEST后再退frame/exit。固定发码不把cold bookkeeping污染另一边，Function末3合同保留；无新增状态表/ABI/IR字段。真实SMC撤链仍能迫使下一self进入cold、完成flags后退休旧码。

同时退役两个实际截断重复：BitExtract一次UBFX(min(count,resultWidth))，BitClear仍保Narrow；partial Uniform仅在view/carrier覆盖field时由BFXIL/STRB/H/W裁剪，窄view/carrier写宽field仍Read补零，完整U64仍原Normalize。没有全局放松numeric/raw合同，没有IR/分配扫描改变。

| 指标 | 第二十批 Block | 第二十批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 45.532 ms | 81.195 ms | 46.863 ms |
| Compile / FEX | 0.972× | 1.733× | 1× |
| 有效100000次运行 | 21.524 s | 19.942 s | 15.082 s |
| 运行 / FEX | 1.427× | 1.322× | 1× |
| 用户态周期 | 24.697 G | 22.928 G | 17.334 G |
| 用户态指令 | 82.567 G | 74.445 G | 61.982 G |
| 指令 / FEX | 1.332× | 1.201× | 1× |
| branch-misses | 96.899 M | 43.297 M | 18.766 M |

相对第十九批，耗时少1.89%/2.82%、周期少1.93%/2.82%、指令少3.49%/2.28%；Block误预测反而多1.22%，Function少9.74%。这是整批共同收益，不将各hunk孤立归因或据此宣称已追平。完整compile仍合格，Function失败/24次Block retry继续计费。

Host五项、Android1965 checks、原12+4170 pipeline/Function TEST、formatter/SMC/EOF集中通过。仅新增一个Block合并场景：首入口Complete fault、tagged load/store的R12 snapshot、zero whole flags/reserved、双alignment cold patch、Unlink强制cold、真实RO-store回写权限后撤self/slots=null再重试；第二resolver确认完整TEST并停止旧arena。原Function七场景未改。

冻结后的三个实际入口离线各生成一次：Block2197e0在快速访存及ready近self时7条，2FaultSite均64/R12、2原Link，self冷及zero各有完整MOV1/MOVW29/CMP；未patch的LDR/BR和无RWalias不冒称7条。CRC21c880/21c950 words199→171、311→265，22/39个重复BitExtract和各7个partial-store前截断全部退役；UBFX66→37、103→57。三份IR/Home逐字节与第十九批相同，frame/spill0。薄工具/五库SHA和文本在`batch20/native-quality-review.md`；未绑定运行RX，不外推动G。

80输入digest`c051bc66dc4135ab26cd3c32fb275bd687b7d732a698770db939e9da8f125696`，runner SHA`1f37b262c8ec8c1b02853e98bff6706b3ea18320a7309bf509711d4e6095b9b7`。三方长跑validation/CRC0xd340/实际CPU4通过，min/max/cur614400/1171200/1171200kHz、电池26.1°C。phone namespace已清理：du37,225,472B、df37,285,888B、剩余0、无活动/错误。原Git/WIP及固定FEX不动，目标仍开放。

## 第二十一批：TST立即数与BitExtract低carrier直接消费

生产仅改两个既有C写口：`compile.cpp`在原NativeNZCV、仅Z或C|Z、无数值消费者的And路径直接消费可编码immediate mask，保原Prepare/Finish、CFINV及fallback；`emit.h`让BitExtract在实际读取时借pin carrier低位，按carrier边界限制截取，非narrow来源仍读raw64。Field/view/零长度与numeric/raw合同保持，没有新IR、表、pass、扫描或ABI；没有实现extract-shift融合。

| 指标 | 第二十一批 Block | 第二十一批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 45.543719 ms | 81.818327 ms | 47.298646 ms |
| Compile / FEX | 0.962897× | 1.729824× | 1× |
| 有效100000次运行 | 20.728 s | 19.135 s | 15.094 s |
| 运行 / FEX | 1.373261× | 1.267722× | 1× |
| 用户态周期 | 23,810,451,501 | 22,001,848,525 | 17,346,152,789 |
| 周期 / FEX | 1.372665× | 1.268399× | 1× |
| 用户态指令 | 81,165,432,992 | 73,014,684,660 | 61,981,522,003 |
| 指令 / FEX | 1.309510× | 1.178007× | 1× |
| branch-misses | 98,875,659 | 43,902,249 | 19,773,626 |

相对第二十批，耗时少3.70%/4.05%、周期少3.59%/4.04%、指令少1.70%/1.92%，但branch-misses多2.04%/1.40%。这是整批同输入测量，不拆认某一hunk的动态收益；没有新增当前RX绑定或采样归因。去byte完整compile仍小于2×，Function失败和24次Block retry继续计费；两scope的运行/cycles仍未达追平目标。普通长跑PMU是whole-child用户态总量，不含byte clocks或诊断dispatch counters，不能直接等同热循环本体。

Host五项、Android1965 checks及原pipeline/Function/Block TEST、formatter、SMC、EOF检查集中通过，不另建测试矩阵。三方长跑validation/CRC0xd340及实际CPU4通过；CPU4 min/max/cur为614400/1171200/1171200kHz，未锁频。原始compile/runtime汇总、命令、identity和CPU现场保留在`batch21`，不改历史批次。

80输入digest`04bf740296e3ed29bedb4fb30b2a1e2373cdec6081f17884dceaf16ea7a755dd`，runner SHA`9065b3a56a895a5eac8ec44e2c1accf3d92fea14b4b30cdaf9457eaf4e050151`。测量后这三份文档及独立hash归档为`batch21/post-measurement-docs`，不重新标为已构建源码摘要。phone namespace已清理：du37,237,760B、df实际增加37,298,176B、剩余0，无活动进程、打开文件或错误。原Git/WIP、固定FEX及生产21冻结源码不动，继续下一步。

## 第二十二批：有界融合候选未命中，不认领收益

本批只在现第二forward walk加入extract→LSR归约和有限合同场景；完整建测与原输入测量保留。两CRC入口的IR/Home/words与第二十一批逐字节相同，仍137/217 words，frame/spill0；20次输入folded仍30/53、erased604/1319、native bytes321756/534504。普通长跑指令也基本不变，不能把时长的小差或预计删词数算成算法收益。

| 指标 | 第二十二批 Block | 第二十二批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 46.081704 ms | 82.668092 ms | 48.389010 ms |
| Compile / FEX | 0.952318× | 1.708406× | 1× |
| 有效100000次运行 | 20.777 s | 19.134 s | 15.087 s |
| 运行 / FEX | 1.377146× | 1.268244× | 1× |
| 用户态周期 | 23,837,741,113 | 21,972,908,735 | 17,339,293,326 |
| 周期 / FEX | 1.374782× | 1.267232× | 1× |
| 用户态指令 | 81,165,451,060 | 73,014,176,569 | 61,981,553,120 |
| 指令 / FEX | 1.309510× | 1.177998× | 1× |
| branch-misses | 98,493,301 | 43,139,322 | 19,157,591 |

离线核对确认不是源码/库漂移。真实forward时点，ForwardCopy刚把LSR的ZeroExtend32输入改为inner，gap已零use但record仍持有inner旧edge；因此inner count为2，gap尚不是Nop。原matcher同时要求count1和gap Nop，实际没有命中。最终DCE后的Nop不能倒推为第二遍输入；下一批只精确识别这个中间态，不增加扫描或提前erase。原因、库身份和逐字节对照在`batch22/native-quality-review.md`，判定在`measurement-decision.json`。

已构建源码摘要`fb42653eb5ef4dbf86b286edcf02468ae44b7124f68257915bf80ae9a0955810`，runner SHA`0bd01fdce2a204bd06cc50a1e7b2d9baeb91af745c8c344aa667e5c60284fad5`。三方有效长跑/CRC0xd340/实际CPU4通过；完整compile/runtime原始摘要在`batch22`。phone namespace已清理：du37,258,240B、df实际增加37,318,656B、剩余0，无活动进程、打开文件或错误。封存为中途候选，不修改原结果或补认收益。

## 第二十三批：真实中间态融合与相邻low-mask归约

保原三次N扫描、唯一u64/N账和oldW+2N预算。extract-shift只扩精确零use/非pin、单Value指向同inner的default ZeroExtend32U32 gap；inner当前count须2，邻接/default Nop仍须1。原外U32/effective viewU32、内U8/count8/合法first等门不放松，原统一Rewrite后维护use，第三逆扫依次退gap和inner。另一个专用ForwardCopy入口仅把相邻sole-use/nonpin、default BitExtractU8(parent,0,8)向AndU8 immediate#1转发；只收LogicalZeroCVA的Z或C|Z、非branch_only，outer effective view必须U8，parent原view合法scalar后改为显式U8 edge。And type/metadata/discard全保，沿ReplaceValueUse及原DCE，不新增通用raw alias规则、pass、表或恢复合同。

| 指标 | 第二十三批 Block | 第二十三批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 46.416828 ms | 82.417095 ms | 47.721354 ms |
| Compile / FEX | 0.972664× | 1.727049× | 1× |
| 有效100000次运行 | 20.613 s | 19.054 s | 15.128 s |
| 运行 / FEX | 1.362573× | 1.259519× | 1× |
| 用户态周期 | 23,652,712,158 | 21,910,748,111 | 17,393,333,207 |
| 周期 / FEX | 1.359872× | 1.259721× | 1× |
| 用户态指令 | 80,728,027,417 | 72,577,660,187 | 61,981,508,769 |
| 指令 / FEX | 1.302453× | 1.170957× | 1× |
| branch-misses | 96,147,888 | 47,149,103 | 19,084,768 |

20次诊断folded从30/53到39/84（现含局部代数），erased从604/1319到621/1383，native bytes从321756/534504到321692/534256；spill slots仍23/40、最大frame32/48B。Function的722次Function和24次Block retry继续全部计费。两scope完整compile均小于2×，但有效运行/cycles仍高于1.10×，目标保持开放。这是两项归约的整批测量，不单独归因动态贡献；误预测也非两scope同向改善。PMU为普通whole-child用户态总量，不含byte clocks、采样或诊断dispatch counters。

冻结23头/库对原CRC入口各离线发射一次：21c880 words137→129，21c950 words217→201。两者各合并7处extract-shift，low-mask分别去1/8个UBFX，crcu16另少1条alignment Nop；LSR15/31→8/24，TST/CSEL及原Uniform提交不增加。birth目录N/B仍164/1、272/1，frame/spill0；parent末用及部分寄存器Home随新edge改变，不能称IR/Home全部相同。实际词流、库身份和局部Home差异保留于`batch23/native-quality-review.md`与`native-quality-comparison.json`。这些是offline recompile，未与运行RX新增逐字绑定，不外推动态G或时长。

Host五项、Android1965及原pipeline/Function/Block TEST、formatter、SMC、EOF合并检查集中通过，不另建测试矩阵。三方100000次validation/CRC0xd340和实际CPU4通过。已构建源码摘要`91128ccc2989b359d51bb3c61853df12e287ae4e27ef2ad49f73d181b3ea6b8c`，runner SHA`50a74168958b62077b31695ce93ef897319fcc24877eef5ab7fd05b7afa999a3`；完整命令/原始结果/身份在`batch23`。本次三份测量后文档与hash独立归档到`batch23/post-measurement-docs`，不改已构建源码摘要。

phone namespace已清理：du37,270,528B、df实际增加37,330,944B、剩余0，无活动进程、打开文件或错误。原Git/WIP、固定FEX及历史证据不动。后续第二十四批GPR partial缓存见下节；紧凑IR、唯一owner、三扫和两scope框架不变。

## 第二十四批：完整父值与精确同宽pending partial

Known/private前端复用原16槽GPR parent/view/first和一个固定u16 pending标记；只在已有完整U32/U64父版本、源为字段精确自然U8/U16的canonical值时保留一个未观察partial。同字段后读复用该值，不同field/不兼容读先出生中立BitInsert；完整覆盖可丢未观察partial，真实fault/helper/终结等观察前物化并完整Uniform发布。原raw/unknown及Unknown/EarlyOff路径不放宽，U32父版本保持EAX写的全父零高覆盖，不把低32快照当完整RAX。

C仅在BitInsert结果U32、count>0且字段完全位于低32时选W-BFI，由W指令清高；其它形态保原检查/fallback，没有修改ReadsBeforeWrite或授予新的输入输出别名。框架、Home16B和原三次扫描/u64N不变，没有每节点pending表或恢复平台。

| 指标 | 第二十四批 Block | 第二十四批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 46.633653 ms | 82.380712 ms | 47.521719 ms |
| Compile / FEX | 0.981312× | 1.733538× | 1× |
| 有效100000次运行 | 20.279 s | 18.786 s | 15.102 s |
| 运行 / FEX | 1.342802× | 1.243941× | 1× |
| 用户态周期 | 23,285,233,462 | 21,602,456,627 | 17,346,951,881 |
| 周期 / FEX | 1.342324× | 1.245317× | 1× |
| 用户态指令 | 80,475,182,674 | 72,196,513,985 | 61,981,522,002 |
| 指令 / FEX | 1.298374× | 1.164807× | 1× |
| branch-misses | 93,753,484 | 45,240,498 | 18,788,289 |

20次诊断的IR出生N从36751/87134到36725/87038，operand words65838/152673到65829/152604；folded39/84、erased621/1383不变，归约来自前端少出生。全应用native bytes321692/534256→321708/534248，spill slots23/40→25/42、最大frame32/48→48/64B，说明延长SSA寿命确有成本。不能用局部少节点直接认领等量少机器访存，也不能只列两个CRC的frame0而隐藏全输入压力代价。Function仍722次Function+24次Block retry，完整收费；两scopecompile保持小于2×，运行/cycles目标仍未完成。普通PMU为whole-child用户态总量，不含byte clocks、采样或diagnostic counters。

冻结24头/库对两真实CRC入口各离线发射一次：21c880 N164→147、words129→125；21c950 N272→250、words201→193。各仍单B、frame0/spill0，新BitInsert2/1处均为MOV parent加W-BFXIL，完整发布可与pin合并。Home有取舍：首CSEL失去原pin而用临时，byte XOR活到下一次TEST，最后物化仍付parent copy。两处TST/CSEL/LSR数不变；少的LoadUniform原本可能只是零指令borrow，不按节点数推算动态指令。

当前pending门严格要求源自然宽度与写字段相同；byte shift仍用U32容器保x86 count modulo32，故crcu8/crcu16的CL8路径仍分别有6/7组RCX partial写走发布/fallback，不能称全partial已defer。完整词流、Home取舍、真实capabilities和库身份见`batch24/native-quality-review.md`及`native-quality-comparison.json`；只属offline recompile，未新增运行RX逐字绑定，不外推动态G或时长。

首次新增`PendingPartialWritesKeepWholeParents`场景用了未支持的address-size32 LEA（`67 8d 08`），Host新场景先失败。随后**只把该fixture改成`8d 08`**，使用已支持的64位地址、32位结果；未修改生产或runner ELF，也没有拿失败fixture作手机测量。原test、旧manifest、build/test失败日志和原因保留在`batch24/attempt0`。修正后Host五项、Android1992及原pipeline/Function/Block TEST、formatter、SMC、EOF合并检查集中通过，不扩检查矩阵。三方100000次validation/CRC0xd340和实际CPU4通过。

最终已构建源码摘要`485c41f69ba5ae53ee9715f99f8cdb2611d34eb4be7f8e19e957ff937dd85036`，runner SHA`a4fd3895f81ba7a5b9f6d3ede61303d263c6658bb1044a046fec6beb887495dd`；旧fixture原摘要`1b02da67da402a2b76d03c106c79df016b819bae4939e02b1b17d8539a49cf3b`保留，二者的生产代码/ELF相同，仅test改动。原始compile/runtime/清理收据在`batch24`；这三份测量后文档独立归档到`batch24/post-measurement-docs`，不重新计算已构建源码摘要。

phone namespace已清理：du37,331,968B、df实际增加37,392,384B、剩余0，无活动进程、打开文件或错误。原Git/WIP、固定FEX及历史证据不动。后续第二十五批宽源pending及有限消费者改动见下节。延迟数值carrier归一化候选暂缓未实现；紧凑IR/唯一owner/三扫及两scope框架保持不变。

## 第二十五批：宽源partial、嵌套clip与安全BitInsert复用

三项改动沿同一紧凑IR/固定缓存/原三扫和Home落地：前端pending单独记录字段宽度，允许合法canonical宽整数提供byte/word写，窄读以BitExtract表达，不把宽源误当已截断field或完整父值；B在原forward fold将sole-use、同B的有限BitExtractU32字段与外BitExtractU8剪低位合并，保raw parent/view和第三DCE；C与分配给BitInsert原samepoint最后读复用，若目的覆盖RHS，先完整MOV X17保存，再copy parent及BFI。原pin窗口、Field/raw/flags/width、完整发布和fault观察合同不变，不加pass、表或repair层。

| 指标 | 第二十五批 Block | 第二十五批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 46.632854 ms | 83.014875 ms | 48.220365 ms |
| Compile / FEX | 0.967078× | 1.721573× | 1× |
| 有效100000次运行 | 20.271 s | 18.649 s | 15.079 s |
| 运行 / FEX | 1.344320× | 1.236753× | 1× |
| 用户态周期 | 23,272,937,682 | 21,451,194,005 | 17,329,589,384 |
| 周期 / FEX | 1.342960× | 1.237836× | 1× |
| 用户态指令 | 80,017,427,986 | 71,737,889,588 | 61,981,509,541 |
| 指令 / FEX | 1.290989× | 1.157408× | 1× |
| branch-misses | 96,692,385 | 48,816,136 | 18,653,216 |

相对24，Block20.279→20.271s实质持平，Function18.786→18.649s约少0.73%；指令约少0.57%/0.64%，误预测却增加。只保该整批结果，不将宽源、clip或reuse各自认领动态收益；同轮FEX分母也独立保留。两scopecompile仍小于2×，1.10×运行/cycles目标继续开放。普通长跑PMU为whole-child用户态总量，不含byte clocks、采样或诊断dispatch计数。

20次诊断出生N36725/87038→36657/86916，operand words65829/152604→65861/152621；folded39/84→48/115（含局部代数），erased621/1383→632/1416。native bytes321708/534248→321756/534004，Block反而多48B、Function少244B；spill仍25/42、最大frame仍48/64B，没有收回24的+2槽/+16B增量。Function仍722次Function+24次Block retry，所有失败/重试/发布/释放计费，不能只用两个CRC的静态减词或零spill代表全应用。

冻结25头与五个host库仅重链现薄工具自身，两入口各Function生成一次，实际caps为Known/0xa64/features31。crcu8 N147→135、words125→107；crcu16 N250→236、words193→173，仍各单B/frame0/spill0。原RCX U8 stores6/7→0，同槽U64 stores7/8→1，U8 loads6/7→0；宽partial确实退出这些中间发布回读。嵌套clip真实命中6/7处，外U8 extract直接取原parent字段，内层由第三DCE退出；两个XOR输入已有canonical U8，自然命中原normalized_logical，不是新增deferred carrier。

两单位MOV/BFXIL/UBFX各少6/7，TST/CSEL/LSR/EOR和PSTATE维护数量不变；crcu16因alignment多1条NOP，故有效指令−21、总words−20，crcu8总words−18。残留BitInsert2/1处仍有MOV parent+W-BFXIL，目的pin均不等于RHS，未发新增X17保护，不能将本输入算成reuse已获益。Home位置和寿命随真实图改变。逐词/IR/Home/库身份在`batch25/native-quality-review.md`和`native-quality-comparison.json`；这些是offline recompile，未新增运行RX绑定，不外推动态G或耗时。

Host五项、Android2100及原pipeline/Function/Block TEST、formatter、SMC、EOF集中检查通过；压力合并场景覆盖满bank、parent仍活/RHS末用的X raw高位保存与U32 W清高，没有新检查矩阵。三方100000次validation/CRC0xd340和实际CPU4通过。已构建源码摘要`bd4ae07371084d6ba57d6c695837c7b3a8edfd896db120882a618472a0ec3789`，runner SHA`fd54301ff534e3b02ccb80fc222a9bcd18f20722f1d1183608b597a20e5a9260`。原始compile/runtime、完整命令/身份、清理收据在`batch25`；测量后三份文档及hash归档到`batch25/post-measurement-docs`，不重新标为已构建源码摘要。

phone namespace已清理：du37,401,600B、df实际增加37,462,016B、剩余0，无活动进程、打开文件或错误。原Git/WIP、固定FEX及历史证据不动。deferred XOR carrier与PF目的位扩展未实现，不把既有canonical XOR少cut描述成新lazy机制；后续地址/实际消费者改进见第二十六批，框架和最终目标保持不变。

## 第二十六批：单地址复用、窄aligned消费者与vector scaled store

前端只在Known/private的同B内部pure构造口保留一个真实地址NodeId，原cells<=4且严格匹配才复用，不按guest寄存器名猜等价。复用的是不可变SSA结果，实际末用延长仍由原分配计费；不加地址字典、N表、EA计划或重复metadata验证。内部pure metadata由构造合同保证，Unknown/EarlyOff及非域保持原出生。

C在已经aligned的8/16位Add/Sub、bare RHS Value/Immediate中让UBFIZ W和ADDS/SUBS W移位消费直接裁剪输入；min(view,CarrierBits)不足仍Read补零，AF提前捕获bit4、Prepare/Finish、numeric结果和全部flags合同保持。V128 TSO store仅对无shift Add/Sub(base,ImmU64)、有效非负16整disp<=65520，用STR Q scaled12直接消费offset，不依赖RCpc；原负/非整/超界/其它fallback、DMB、Shadow与实际fault语义保留。无新IR/alloc/恢复平台或数值carrier模式。

| 指标 | 第二十六批 Block | 第二十六批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 46.380543 ms | 83.198837 ms | 48.097813 ms |
| Compile / FEX | 0.964296× | 1.729784× | 1× |
| 有效100000次运行 | 19.972 s | 18.467 s | 15.039 s |
| 运行 / FEX | 1.328014× | 1.227941× | 1× |
| 用户态周期 | 22,925,689,969 | 21,247,716,045 | 17,286,897,458 |
| 周期 / FEX | 1.326189× | 1.229123× | 1× |
| 用户态指令 | 78,213,270,563 | 70,324,765,604 | 61,981,529,016 |
| 指令 / FEX | 1.261880× | 1.134608× | 1× |
| branch-misses | 96,894,481 | 50,220,369 | 18,769,758 |

相对25，耗时约少1.48%/0.98%、指令少2.25%/1.97%，误预测仍增加。三项同批改动不拆认各自动态贡献，也不以某个热循环静态短了几条外推整轮G/耗时。两scope完整去byte compile合格，1.10×运行/cycles目标未达到；Function失败、24次局部Block retry和真实形成/分配/发布/释放继续收费。普通PMU仍是whole-child用户态总量，不带byte clocks、采样或诊断计数。

20次诊断出生N36657/86916→36638/86893，operand words65861/152621→65804/152552；folded48/115、erased632/1416不变。native bytes321756/534004→321308/533272，spill仍25/42、最大frame仍48/64B，未收回24的压力增量；不能用三个离线单元的frame0代替全应用账。

冻结26头和五库各生成一次三个实际Function入口，caps为Known/0xa64/31：

| 单元 | N/B 25→26 | total words 25→26 | fast回边指令 | 局部frame/spill |
| --- | ---: | ---: | ---: | ---: |
| dot `21b0b0` | 430/27→428/27 | 400→398 | 29→27 | 0/0 |
| mul-const `21ae30` | 428/26→426/26 | 423→421 | 25→22 | 0/0 |
| state `21c380` | 83/10→83/10 | 157→157 | 不将完整单元当单一热环 | 0/0 |

dot实际退两条重复EA ADD；mul-const退两条重复EA ADD和一条store前ADD#16，保两道DMB与两次原STR Q，实际快路径少3条，但对齐NOP多1，所以全单元只少2words。state实际CMP输入UBFX少1，左W移位已做低8裁剪；cold literal前多1条NOP抵回，总157不变。状态机IR/Home与25逐字节相同；矩阵地址末用延长，例如dot首地址7/8→7/13、mul首地址5/6→5/11（def/last），Home可比打印位置字段经ID/alias映射未新增copy/spill。fault guestPC/recipe及Link目标保留，相对位置随真实词流收口。全部细节/五库身份在`batch26/native-quality-review.md`；只重链薄工具自身，不是编译吞吐测量，未新增运行RX绑定。

Host五项、Android2116及原pipeline/Function/Block TEST、formatter、SMC、EOF集中通过，不新增矩阵。原合并case补了脏pin窄算术/Bool fallback、vector +16直消费与−16 fallback，并保whole flags/普通site；三方100000次validation/CRC0xd340、实际CPU4通过。已构建源码摘要`f0783b61bcd3c7e4477c5ba89f975e19b0117f0dcdeca9c568c80004cb3e7475`，runner SHA`1bfe2c054b3bbb6c4b28c31e764508d2768e4e5947d34e8da04bedf689920df5`；原始compile/runtime、命令/身份和清理收据在`batch26`。测量后三份文档/hash独立归档`batch26/post-measurement-docs`，不重标已构建源码摘要。

phone namespace已清理：du37,426,176B、df实际增加37,486,592B、剩余0，无活动进程、打开文件或错误。原Git/WIP、固定FEX及历史证据不动。其后的第二十七批窄flags-only CMP/zero-RHS验收与测量见下节。occupied查表变体、扩大PF目的位及noTouchCarrier等已排除候选未采用；紧凑IR、唯一owner、三扫和两scope框架不变。

## 第二十七批：窄CMP与有效零RHS的实际消费

仅C作两个局部选择，不改IR、分配、前端、Field或框架。NativeNZCV SubU8/U16、无数值consumer、非legacy、已支持算术metadata且非branchOnly，仅actual mask为Z或C|Z时，以W比较得到相同C/Z；LHS仍按logical view/width正规化。Value RHS只收两端均符合现Read的numeric免cut路径，避免给脏借pin补两UBFX反而增码；Immediate按min(view,width)取低位且imm12/shift12合法才直接CMP，不能把负literal翻CMN。原Prepare/ArithmeticFlags/CFINV和PF/AF/未写N/V保留，跳过死数值save/spill。其它路径保原fallback。

同组Add/Sub/And/Or/Xor/AndNot的bare RHS在view/result-width归一后为0时，局部用Rm31=ZR，省原scratch literal装载；不把普通Read、地址0或SP语义改成ZR。AF输入xor、NZCV、结果Narrow和所有原边界保持，没有新增table/pass/helper框架。

| 指标 | 第二十七批 Block | 第二十七批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 46.240713 ms | 82.477986 ms | 47.488645833 ms |
| Compile / FEX | 0.973721448× | 1.736793807× | 1× |
| 有效100000次运行 | 19.985 s | 18.167 s | 15.114 s |
| 运行 / FEX | 1.322283975× | 1.201998147× | 1× |
| 用户态周期 | 22,935,096,212 | 20,902,782,125 | 17,374,570,153 |
| 周期 / FEX | 1.320038194× | 1.203067583× | 1× |
| 用户态指令 | 78,152,064,874 | 70,141,515,103 | 61,981,511,890 |
| 指令 / FEX | 1.260893168× | 1.131652213× | 1× |
| branch-misses | 97,861,966 | 45,485,830 | 18,916,463 |

相对26，Block19.972→19.985s绝对时间基本持平；Function18.467→18.167s约少1.62%，但指令仅少约0.26%，误预测少约9.4%。这是整批普通PMU/有效运行事实，不能拆归单hunk或把静态删词比例外推全程G/时间，也不以删检查解释收益。同轮FEX自身变化保留在分母中。两scope完整去byte compile仍≤2×，Function的722次Function＋24次局部Block retry及形成/发布/释放全计费；1.10×运行/cycles目标仍未达到。

20次代表性诊断的出生N36638/86893、operand words65804/152552、folded48/115和erased632/1416与26保持；native bytes321308/533272→321220/532280。全应用spill仍25/42、最大frame48/64B，未收回24的压力增量。少量fresh Block记录会随实际clock/执行路径出现不同出生数，原各样本账留在compile-summary；这里的代表性计数不冒充所有进程完全一致。

冻结27头与五库，各实际Function入口仅生成一次，Program.Target实印Known/0xa64/caps31。crcu16的N120 SubU8/Z、last=def、两numeric输入在真实word69–71从LSL+SUBS+UBFX变为CMP W6,W4；CSEL原样，总173→171。打印IR/Home逐字节相同，frame/spill0；NOP仍1条，MOV21/TST14/CFINV0保持。唯一fault PC21caef、recipe/恢复资格不变，fast146→144/fallback170→168仅因布局前移。

state21c380的IR/Home和157words全部逐字节不变；其full95窄CMP仍保AF/PF和完整故障可见flags，不能因后续NE把它算作flags退役。两个单元都没有effective-zero bare RHS，所以这两份词流的zero-RHS去MOV为0。W26原先没有crcu16产物，经授权只用原已封26薄ELF在W27独立补发baseline26-crcu16一次，不重链26或覆盖旧证据。详细原词流/metadata/五库前后SHA见batch27/native-quality-review.md及comparison JSON；全部offline、无新RX绑定，不能替代普通长跑归因。

首次新增fixture把0x100标U8、0x10000标U16、wide0x100001000标U16，违反literal自身可表示范围；负literal应使用合法S8符号扩展。Host及phone backend场景在IR append失败处停止，未进入compile/runtime性能模式。只将fixture改用合法U16/U32/U64/S8（AndNot用合法zero），仍测试消费者低width截断；production compile.cpp保持da31de60da6591f738016491582a34234c16d4be7fd53e6bce6050f3046d42de，未为fixture放宽schema。旧manifest/test/log/runner、手机失败与首次独立cleanup全留在batch27/attempt0，不把失败尝试写成测量结果。

修正后Host五项、Android2146及原12＋4170 pipeline、Function/Block TEST、formatter、SMC、EOF与合并raw/borrow/Bool/fullflags场景集中通过，没有扩矩阵。三方100000次validation/CRC0xd340与实际CPU4通过。最终已构建源码摘要0c9edac17b07d59ffe2ef021edaae3e260506418470ae6bf7a2ff5341523ab97，runner SHA1152f18a43cf3a6f8ebb9a73f152cc87b5eff14f966c40f0152a7b892f095056；原始summary、命令和身份在batch27。测量后三份文档/hash独立归档batch27/post-measurement-docs，不重新标为已构建源码摘要。

最终phone namespace已清理：du37,471,232B、df实际增加37,531,648B、剩余0，无active/openfiles/errors。attempt0首次cleanup保独立收据，不与最终值相加冒称单轮回收。原Git/WIP、固定FEX、旧证据保持不动。

第二十八批中立VecSignedMulWidenLow/capability的完整测量见下节；紧凑IR、唯一owner、三扫与两scope框架不变。

## 第二十八批：连续低半有符号宽乘的实际命中

新增中立`VecSignedMulWidenLow(Value,Value,Imm src_lane_bits)->Explicit V128`，定义连续低半src8/16/32 lane的有符号全宽乘积，完整V128输出、flags不变。旧`VecMulWiden`仍取偶数lane，未改合同。TargetFeature::VectorSignedMulWidenLow为bit5，KnownTargetInfo广告，实际Program为Known/0xa64/features63；Unknown/EarlyOff及缺cap的Known仍走原图。

B只在同B、default metadata、自然/有效V128的`VecMadd16`双`VecExtend(raw,16,32,0)`上匹配。至少一个Ext真实sole-use且非pin；非tail 2→3的Rewrite实际追加3word，以product和独占Ext两节点的4word预算支付，不增加`oldW+2N`。vector draft与scalar Fold分开，V128不会进入Mask128。新真实父edge保原view、延寿进入实际分配；原Madd ID/type及Add/Uniform消费者保持，多use左Ext和故障前完整XMM publication保留，sole右Ext由原第三DCE退。沿现u64 rows/forward/统一Rewrite/use账执行，无新pass、N表、owner、IR层或恢复历史，IR仍24/24/8B、Home16B。

| 指标 | 第二十八批 Block | 第二十八批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 47.028893 ms | 83.742158 ms | 48.503697917 ms |
| Compile / FEX | 0.969593970× | 1.726510794× | 1× |
| 有效100000次运行 | 19.796 s | 18.217 s | 15.087 s |
| 运行 / FEX | 1.312123020× | 1.207463379× | 1× |
| 用户态周期 | 22,732,665,722 | 20,947,613,471 | 17,332,551,348 |
| 周期 / FEX | 1.311559116× | 1.208570686× | 1× |
| 用户态指令 | 78,130,539,123 | 70,120,129,365 | 61,981,511,513 |
| 指令 / FEX | 1.260545882× | 1.131307186× | 1× |
| branch-misses | 96,613,315 | 43,959,824 | 18,746,731 |

相对27，Block19.985→19.796s约−0.95%，Function18.167→18.217s约＋0.28%，未改善。两scope全程指令分别仅少21,525,751/21,385,738条（各约0.03%），不能用局部回边27→21、约22%少词认领全应用收益；同轮FEX变化继续保留在分母。两scope完整去byte compile仍≤2×，1.10×运行/cycles目标尚未达到。compile只扣真实byte parser调用，形成、IR、分配、编码、发布、释放和失败/retry均收费；Function仍722次Function＋24次局部Block重试。普通长跑PMU是whole-child用户态总量，不含byte clocks、采样或诊断计数。

20次代表性诊断出生N36638/86893、operand words65804/152552与27相同，folded48/115→50/117、erased632/1416→634/1418；native bytes321220/532280→321196/532256。全应用spill仍25/42、最大frame48/64B，未收回24的压力增量。两个离线单元frame0不能替代该全应用账；不同fresh进程各自原始计数保在compile-summary，不将代表值冒充全部进程完全一致。

冻结28头和五库各真实Function入口离线只生成一次，另用封27薄工具补同入口对照；27两份IR/words与既有26产物逐字节相同，未靠“前端不改”猜基线。dot `21b0b0`仍N428/B27，whole398→392 words，非DATA指令394→388，NOP仍2，热回边27→21。N200/N209右侧Ext变Nop，原N201/N210变新宽乘；N187/N192及N188/N193发布、N203/N212 Add和N205/N218结果发布全部保留。每pair两USHLL＋三乘加词变保留一USHLL＋一SMULL，两个pair合计USHLL、SMULL2、ADDP各少2；SMULL仍2、MOV/DUP不增。原raw父N186/N191的last-use10→24/15→33，真实home随之变化，但本单元未新增copy/spill；这是本次分配事实，不是一般保证。

mul-const `21ae30`为sign1扩展后VecMul32，不属于该等式，新op0、融合0，IR/Home/机器字节均不变、仍421words。两unit fault guestPC/TEST与PF/carry配方、link目标保持，只按实际发射调整word位置；没有新增运行RX逐字绑定。全部细节见`batch28/native-quality-review.md`，SHA `4ac9e02d44f45962253e4154728faeaef94fabd4102c859e4aafccab195ebe2e`，该离线选择事实不外推动态G或整轮周期。

Host五项、Android2178及原12＋4170 pipeline、Function/Block TEST、formatter、SMC、EOF与合并flags/alias/原故障检查集中通过；有限IR场景核cap fallback/同ID/多consumer/左发布/右DCE/2N，既有backend合并场景核src8/16/32真实数值。三方100000次validation/CRC0xd340和实际CPU4通过。源码摘要`d8508058631d4db61ea8b4e8609d7eab1ecf51f21b3f2b21ba5bd49908bc2f4b`，runner SHA `3bf74d5957deea6a57cf9af2ce60d5585da0d97d90776a7cf70e7f3800e7627f`；原始summary、命令/身份与报告在`batch28`。测量后三份文档/hash独立归档`batch28/post-measurement-docs`，不重标已构建源码摘要。

phone namespace清理du37,553,152B、df实际增加37,613,568B、剩余0，无active/openfiles/errors。原仓库Git/WIP、固定FEX及旧证据不动；27和24的fixture-only失败仍保各自attempt0，未以本批通过掩盖历史失败。

随后独立diagnostic clone的实际directory target census已闭合，结果见下节；再以完整冷layout核实探测链后形成29，不是仅凭static微切或初始同桶猜总体差距。诊断耗时不替代普通PMU基线。deferred XOR carrier、扩大PF目的位及已排除occupied/noTouchCarrier等仍未采用。


## 独立目录目标计数：28诊断已闭合

从28复制的独立诊断只在原32B slot尾部padding使用u32计数；Counted ready-hit实际分支以非S指令累加，原目录、迁移及冷归属保留。两100000输入各只运行一次，CRC均d340、实际CPU4、invalidations0；slot之和与直接native.cache_hits精确相等：Block612,012,388、Function612,012,385，总数小于2^32。它只覆盖实际目录ready-hit，不覆盖静态links或全部native块，也不是运行时间/PMU归因。原始结果及独立源/镜像身份在`batch28/directory-target-census`。

两scope前十PC次数相同，覆盖94.2157%的目录命中；core_state_transition目标共457,599,993次（74.7697%），core_bench_state共108,399,983次（17.7121%）。前三目标21c483、21c473、21c3c0分别147,999,999、131,199,999、93,599,999次。符号归属不能自动等同RET或CALL类别。

初始待核点是Block实际probe减lookup900,291、Function149,300,133，而Function最热21c483与冷ready目标25a40f初始桶同为0xbb。同桶本身不足以证明先后/完整链，此阶段未改生产；随后独立v2全occupied/Reserve/Grow/Ready冷event核实了真实布局，才形成29的固定shift13，见下节。没有改容量或编译流程。

诊断namespace已单独清理：du2,945,024B、df实际增加2,953,216B、剩余0，无活动进程/打开文件/错误。生产28的原始普通测量身份保持不变。

## 第二十九批：实证同目录碰撞与窄mask消费

生产仍是一个CodeCache/32B slot、原容量/ready-pending优先/链接撤销和SMC生命周期；只把共享`DispatchHashShift`11改13。C++ Index和inline Writer已消费constexpr，四shared naked wrapper也以named immediate引用它，未加mode、二级cache、Context/counter字段或热feature分支。当前Android unstripped ELF中四shared的EOR均实为`ca403405 / LSR #13`，没有额外materialization指令；构建已验证Clang接受该常量输入。

B在原forward把精确U32低byte `BitInsert(parent,And(BitExtract(parent,0,8),mask),0,8)`改为同ID whole-parent `And U32(parent,ffffff00|mask)`，保父bits8..31、原view和其他child消费者；C对可重复成32位logical immediate的窄AND，先保原numeric Read再单条W AND。原三扫/u64 rows/Rewrite/use/DCE及预算不变，IR24/24/8、Home16、Uniform/唯一owner保持。

| 指标 | 第二十九批 Block | 第二十九批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 46.678645 ms | 83.703110 ms | 48.390468750 ms |
| Compile / FEX | 0.964624774× | 1.729743732× | 1× |
| 有效100000次运行 | 19.902 s | 17.492 s | 15.036 s |
| 运行 / FEX | 1.323623304× | 1.163341314× | 1× |
| 用户态周期 | 22,848,587,260 | 20,121,607,249 | 17,280,324,036 |
| 周期 / FEX | 1.322231413× | 1.164423029× | 1× |
| 用户态指令 | 77,664,466,263 | 68,481,937,579 | 61,981,524,204 |
| 指令 / FEX | 1.253026079× | 1.104876630× | 1× |
| branch-misses | 101,256,504 | 33,969,965 | 18,930,381 |

相比28，Function18.217→17.492s约−3.98%，Block19.796→19.902s约＋0.535%，Block没有运行收益。全程指令分别少466,072,860/1,638,191,786条，但不把同批hash和两窄规则拆认贡献，不把某个static单位乘命中次数当测量。两scope compile仍≤2×；运行/cycles仍超1.10×，Function指令约1.104877×也未进1.10×。普通PMU为whole-child用户态，不含byte clocks、采样或诊断计数；失败/retry、形成、IR、分配、编码、发布和释放全部收费，只扣实际byte parser调用。

### 三种目录证据的不同口径

28 v1记录两scope各约612M实际ready hits，静态links不计。独立v2仅每scope20一次，含全部occupied/pending和有序Reserve/Grow/Ready/Clear。shift11重放逐event和最终每slot occupancy/PC/ready/code_id精确匹配：Block4654event/2604occupied，Function1546event/818occupied；Grow按旧physical顺序、ready抢pending完整复现。原Fn热21c483确在冷25a40f后面，final slot187/188，而不是仅因同primary桶猜先后。

**Final-layout模型**用v1正频ready visits加权v2的最终表：shift11→13为Block612,912,389→612,412,387（−500,002）、Function761,312,395→612,812,405（−148,499,990）。全部正频PC覆盖，包含Fn两项399,999次1→2退化；前10仅Fn21c483由2→1，其余1→1。v2形成与v1有零频冷key差异，频次未按Grow epoch分段，pending_head/visits逐次变更也没日志，因此该模型不是100000全历史probes或周期重建。脚本/changed JSON/报告在`batch28/directory-layout-v2`，不以模型净值认领绝对G指令或时间。

**真实20次诊断**的lookup仍125418/124098，slot probes125889/154083→125740/124403；平均由1.003755442/1.241623556降为1.002567415/1.002457735。三次fresh同值。该probe域含冷/非ready尝试，不能与final-ready模型混成同一计数；byte timing/计数扰动的诊断elapsed不替代上表普通长跑。当前测量与模型方向相符，实际收益只以上表有效输入为准。

### 六个离线实际入口与完整成本

冻结29头、当前五host库和原guest/配置，只重链原薄inspector且六unique unit各生成一次，实际cap Known/a64/features63。五库生成前/后SHA相同，已释放读锁。21c483 B33→31/F170→168，两规则各少1word：`MOV #df; AND X`变`AND W,#dfdfdfdf`，`MOV parent; BFXIL W`变`AND W,#ffffffdf`；原UBFX8输入、完整AF/PF/CMP和原pub保持。仅原BitInsert ID变AndU32和inner last-use4→3，其余Home位置不变。21c473 B27、21c3c0 B31的words与28逐字节同；两Function仍164/170，IR/Home同28，只有各3处hash EOR11→13。

各Function仍10个faultsites，各Block仍2个static links；guestPC/配方/实际fast opcode和target不变，21c483坐标随少词前移2。六个单元frame/spill0，不代表全应用；没有新增运行RX绑定，也不把whole Function词数乘ready hits。详细报告`batch29/native-quality-review.md` SHA `b4121647aa78eca14e11ec27b6eab2d6030c38c6e9ccdf58bf650d0c0cdfdfcb`。实际ELF四shared EOR地址2fed8/300d0/302d4/3050c，命令和反汇编在`hot-state/shared-dispatch-review.json`。

20次诊断统计的IR N36638/86893、operand words65804/152552不变，folded50/117→52/126、erased634/1418不变，native bytes321196/532256→320892/531672。IR计数保原成功请求口径，不宣称覆盖全部失败前缀出生；完整compile收费仍包含失败/retry。全应用spill25/42、maxframe48/64B未变，24引入的压力增量未解决；Function仍722次Function＋24次局部Block重试，不能以局部六单元frame0盖掉。

Host五项、Android2209及原12＋4170 pipeline、Function/Block TEST、formatter、SMC、EOF和合并flags/alias/fault checks全部集中通过；三方100000有效运行CRC0xd340、实际CPU4相符。80文件source-as-built摘要`406e095f868be786440684ab8aaa8e7d1948be1d8433104e624a9c1fc55f642f`，8,402,216B runner SHA `caeeb28a84e0ef824fdcaebccfda9d0d32904889eae42d2f8ced8c0995c40238`；原始summary/命令/身份在`batch29`。三文档另归档`batch29/post-measurement-docs`，不更改80文件as-built摘要。

最终namespace cleanup du37,598,208B、df实际增加37,658,624B、remaining0，无active/openfiles/errors。原树Git/WIP、固定FEX和旧证据不动，24/27 fixture-only失败继续保留。29当时冻结，后续已测30/31见下文。deferred XOR carrier、扩大PF目的位与排除的occupied/noTouchCarrier等仍未采用。

## 第三十批中间点：full窄CMP与前向条件边

RawAF、SubU8/U16、full All flags、无数值use、非legacy且实际immediate可编码时，以SXTB/SXTH、AF MOV/MVN、SUBS/ADDS、SETF8/16四词取代原五词。W29保输入xor bit4并清tag，W1低byte保PF；SETF给窄N/Z/V并保算术C，原ArithmeticFlags/profile仍消费。数值仍被实际view/carrier约束，不因后继Jcc省PF/AF。前向B.cond/CBZ用原fixup行直达实际label；保restore/flip，真正imm19越界只重发未发布的旧长形式，使用同Allocation/容量且费用收费，没有新IR/pass/N表。

| 指标 | 第三十批 Block | 第三十批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 46.835537 ms | 84.421017 ms | 47.787031250 ms |
| Compile / FEX | 0.980088860× | 1.766609366× | 1× |
| 有效100000次运行 | 19.418 s | 16.621 s | 15.085 s |
| 运行 / FEX | 1.287238979× | 1.101823003× | 1× |
| 用户态周期 | 22,319,447,448 | 19,126,254,214 | 17,338,225,554 |
| 周期 / FEX | 1.287297098× | 1.103126393× | 1× |
| 用户态指令 | 76,639,968,584 | 66,259,556,330 | 61,981,504,514 |
| 指令 / FEX | 1.236497390× | 1.069021426× | 1× |
| branch-misses | 88,963,255 | 21,913,319 | 19,432,127 |

Function此时time/cycles仍略高于1.10×，不能把指令先达标或四舍五入叫总目标完成。两scope compile仍≤2×。N36638/86893、words65804/152552、folded52/126、erased634/1418不变，native bytes320748/519188；全应用spill25/42、maxframe48/64B未解决。Function722＋Block24失败/retry及形成/分配/发布/释放继续计费，仅扣实际byte parser调用；诊断IR计数不冒称覆盖全部拒绝前缀。

同六入口各离线一次，IR/Home逐字同29；每Block一处fullCMP5→4，对齐使whole31→29、27→27、31→29。每Function三处fullCMP＋5/5/7个forward B26退役，whole168→160、164→156、170→160；条件数不变，MRS3/RMIF3/CFINV2不变。原restore/carry-flip不是删掉的工作；这六份15个CBZ/CBNZ均属inline目录，不冒称新CFG CBZ命中。fast/cold访存、fault配方/hot-next和link target保，局部frame0不替代全应用账，无新RX绑定，不按wholewords乘entry visits。报告`batch30/native-quality-review.md` SHA `d5c8f33c280c5ff268feb0c2d64d88ea0738082978b622c099673dd24828a07a`。

首次Host/Android build在测试wrapper的未限定Compile调用处ADL歧义，未启动phone；仅改`::Compile`。两个80文件manifest比较仅`source/tests/rebuild/arm64_compile_test.cpp`变化，production相同。原失败manifest `a83bcd711380773d275291b756abb89b19289554a2cf8db016ee7209a59468a1`、test/build logs/status保`batch30/attempt0`；不当成功或最终身份。最终source `9714719ead011aae97dc24f44f48dad5e2142bbc15df0c750e4046598045e4aa`，runner `9cc7fc85ab1cd7d28d2092be51c0d862236553d4a3c2ddb8c8b847d723ca3434`（8,413,712B）。Host五项、Android264408累计checks＋原12/4170、formatter、SMC、EOF通过；大fixture按Emit/Need/Constant累计，不是264K新语义case。三方CRC0xd340/有效CPU4长跑相符。cleanup du37,655,552B/df＋37,715,968B/remaining0，无active/openfiles/errors。30以中间点合并到31文档，不改其冻结源/镜像身份。

## 第三十一批：真实link位置与窄模运算

LinkSite仍16B，原第二u32从fallback_word改为实际branch_word，fallback固定派生pointer_word＋2。奇数tail把热NOP移到BR后、对齐DATA前；near B patch和撤链消费实际branch，far BR也跳过pad，DATA/冷stub大小不变。无flags窄Add/Sub使用W模运算及原末尾正规化，较窄view/carrier仍Read fallback，不改flags、Home、bank、IR或分析流程。

| 指标 | 第三十一批 Block | 第三十一批 Function | 同轮 FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 46.855298 ms | 83.757012 ms | 47.809062500 ms |
| Compile / FEX | 0.980050550× | 1.751906597× | 1× |
| 有效100000次运行 | 19.386 s | 16.428 s | 15.057 s |
| 运行 / FEX | 1.287507472× | 1.091053995× | 1× |
| 用户态周期 | 22,255,808,306 | 18,917,515,352 | 17,313,243,545 |
| 周期 / FEX | 1.285478844× | 1.092661540× | 1× |
| 用户态指令 | 73,924,247,367 | 65,659,140,331 | 61,981,551,993 |
| 指令 / FEX | 1.192681451× | 1.059333596× | 1× |
| branch-misses | 101,087,334 | 21,242,893 | 19,411,790 |

**Function本轮time/cycles/instructions均进1.10×；Block仍未达，两scope总目标不完成。** 相比30，Block指令少2,715,721,217条、约2.716G，但time只−0.165%、基本平；Function指令少600,415,999条、time−1.161%。这是整批普通实测，不拆归单hunk，也不把static pad或离线完整词数乘entry counts当动态G。两scope compile仍≤2×，完整失败/retry、形成、IR、分配、编码、发布/释放及范围fallback继续收费，仅排除实际byte parser；PMU是whole-child用户态，不带诊断/byte clocks。

20次诊断N36638/86893、words65804/152552、folded52/126、erased634/1418保持，native bytes320748/519188→320708/519148；全应用spill仍25/42、maxframe48/64B，未解决24增量。请求Function仍722次Function＋24局部Block retry，保原计费和IR计数口径。

同六unique入口只离线一次，Known/a64/features63、guest/policy不变，五hostlibs前后SHA一致已释放。IR/Home六份逐字同30；words为21c483 B29/F160→29/158，21c473 B27/F156→27/154，21c3c0 B29/F160→29/160。21c473首linked branch真6→5，原hot NOP移到BR后word7，DATA仍8/fallback10，whole words/NOP不减；其他挪pad处为cold未链接尾，不当热收益。两个Function各一处U8 Add#c6由MOV literal＋input UBFX＋ADD变SUB W#3a，原result UBFX保，净少2；较窄读取仍fallback，现flags state不变。所有fault配方、fast/cold访存及hot-next、link target保，实际偏移重算。工具显式适配branch_word/派生fallback及LDR＋8/＋12两模板，防把odd DATA错当指令。局部frame0不替代全应用；无新增RX绑定。报告`batch31/native-quality-review.md` SHA `38c4ceb2b35906cc752fdbb1f97c36ba4a1b16e0c4d1672d9d797306003367ac`。

新的all-entry诊断与directory-ready域不同，静态links也可进入。Block已报告真实高频2197e0、21c3aa/3b3、21c4dc/3a0/4d0，另以原31薄工具各生成一次实际Block，没有盲造同PC Function。2197e0的离线LDR/BR并非resolved warm两条：近self将LDR patch为B，非零实际7条、末CBZ＋B，未patch/far才8条；snapshot12和两个site/zero completion保持。其余真实prefix区分三条32bit fullCMP、五条byte fullCMP、jump-table动态PC和fullbyte TEST，完整33/39等词数不乘entry visits。短报在`batch31/hot-all-entries/native-review.md`；后续全量计数/符号由独立诊断分域收口，诊断elapsed不作QoR，本轮不扩候选或新架构。

Host五项、Android264431累计checks＋原12/4170、formatter、SMC、EOF与原合并fault/flags等全通过；不是新增264K语义矩阵。三方100000有效运行CRC0xd340、实际CPU4相符。80文件as-built source `4b5913893c7e84e802613a8a8bf8dace7abd743d9e62404177d9237c13884eb2`，runner `a18350eadec631faaa836c579e191cacce9ab332fee982127b9e255141b0f068`（8,413,512B）。三文档另存`batch31/post-measurement-docs`，不改30/31冻结80文件摘要或旧失败身份。

最终namespace cleanup du37,680,128B、df实际增加37,740,544B、remaining0，无active/openfiles/errors。普通31生产冻结，原Git/WIP、固定FEX、全部旧证据不动。当前总目标开放，未解Block差距和spill；没有把Function本轮达标扩成两scope/整个框架已验收，也不将诊断时长作为新baseline。


## 第三十二批：CBNZ self head与完整byte TEST收口（中间点）

真实self LinkSite记录早期CBNZ head，仍保late even gate和可撤销cold fallback。B2197e0 near ready非零路由31的七条变六条：MOV snapshot12、load next、store old、MOV tag、MOV old，CBNZ next直entry；zero路径及self cold fallback先完成真实TEST。原snapshot12和两fault site保留，不跨普通dispatcher漏tag。另一full byte TEST仅在无数值use时由五字改为SXTB X1＋MOV W29＋CMP，raw PF carrier可符号扩展，不给numeric U8用户授脏高位；精确PF/AF/NZCV、reserved和原默认路径保持。

| 指标 | Block | 请求Function（含retry） | FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 47.087707000 ms | 83.528582000 ms | 48.122447917 ms |
| Compile / FEX | 0.978497750× | 1.735750894× | 1.000000000× |
| 有效100000次运行 | 19.087 s | 16.429 s | 15.237 s |
| 运行 / FEX | 1.252674411× | 1.078230623× | 1.000000000× |
| 用户态周期 | 21,907,017,203 | 18,920,747,029 | 17,510,585,614 |
| 周期 / FEX | 1.251072790× | 1.080531939× | 1.000000000× |
| 用户态指令 | 72,187,100,419 | 65,616,176,375 | 61,981,524,907 |
| 指令 / FEX | 1.164655121× | 1.058640885× | 1.000000000× |
| branch-misses | 96,491,110 | 18,399,398 | 19,263,158 |

相对31，Block自身time−1.542%，Function16.428→16.429s近乎平；同批FEX15.057→15.237s约+1.2%，不能因Function比值下降就认其代码加速。Function本轮time/cycles在1.10×内，Block仍未达；两scope compile≤2×，总目标不完成。去byte口径保形成、IR、优化、分配、编码、发布/释放、失败/retry，只扣实际byte库调用；普通whole-child PMU不带byte clocks/诊断counter。

原guest/policy、Known/a64/63，只四unique实际入口各离线一次，五库前后hash一致已释放。B2197e0/B21c4d0 IR/Home同31，whole words39→40/29→27；前者whole增长不抵消真实warm6条，也不能把offline LDR/BR当warm。Fn21c380是首次同PC产物，只报152words实际prefix不声称比31改善；Fn21c483的IR/Home和158words逐字同31，未扩scope。fault/zero/linktarget保持，布局偏移真实重算，四单元frame0不替代全应用。报告`batch32/native-quality-review.md` SHA `b81fe4e2457b85f9e36170c082afcfe80051b4c8cbeaf8776b0fdbe091331a3f`；无新运行RX绑定，不做wholewords×entry估算。

三cold诊断样本均Block2042attempt/N36638/words65804/native bytes320192；请求Function746attempt=722Function＋24Blockretry、N86893/words152552/native bytes518324。folded52/126、erased634/1418保持，全应用spill25/42、maxframe48/64B未解决。

Host5/Android264446累计checks、原12/4170、formatter、SMC、EOF及原合并fault/flags均通过；检查数为fixture累计，非264K语义case。80文件source `3ab60a40cbf7e2fb52db57ac3f2840fa1ac6b34122bb6628f26df96773087108`，runner `721e029f8ce0f41583c6a6de93caf1ef1e265c40d237c1726672acfa7898bb05`（8,417,720B）。cleanup du37,696,512B、df＋37,756,928B、remaining0，无active/openfiles/errors。32保独立summary和as-built身份，三docs与33合并一次归档。

## 第三十三批：条件主边near head直接链接（当前checkpoint）

仅真实Block条件尾沿已有LinkSite把primary目标的早期B.cond记录为branch_word，gate保在DATA前偶数位置。21c3aa为B.LS、21c4dc/21c3a0为B.NE；ready且imm19可达时patch head直目标，选中primary由conditional→gate→patched B的两条分支变一条。未选中边仍head未取＋普通link两分支，far/unresolved保真实LDR/BR gate；不能把离线模板称warm，或把近条件head当late gate。fallback仍pointer_word+2，LinkSite16B、FaultSite32B、IR24/24/8、Home16和原SMC/撤链owner不变。

| 指标 | Block | 请求Function（含retry） | FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 47.634095000 ms | 83.521265000 ms | 47.644322917 ms |
| Compile / FEX | 0.999785328× | 1.753016097× | 1.000000000× |
| 有效100000次运行 | 17.126 s | 16.448 s | 15.079 s |
| 运行 / FEX | 1.135751708× | 1.090788514× | 1.000000000× |
| 用户态周期 | 19,663,141,820 | 18,933,521,755 | 17,335,897,221 |
| 周期 / FEX | 1.134244255× | 1.092157015× | 1.000000000× |
| 用户态指令 | 68,253,163,902 | 65,616,181,953 | 61,981,503,284 |
| 指令 / FEX | 1.101186004× | 1.058641344× | 1.000000000× |
| branch-misses | 44,849,209 | 19,056,284 | 18,740,247 |

**Function本轮time/cycles仍进1.10×；Block运行1.135751708×、cycles1.134244255×仍未达，总目标开放。** 两scope compile≤2×。相对32，Block自身time19.087→17.126s、−10.274%，Function16.429→16.448s、+0.116%基本平；FEX15.237→15.079s也变化，不跨批挑分母或拆认某hunk。原普通whole-child PMU整数与运行validation均保留，不由离线词数/entry频次推动态指令收益。

三actual Block21c3aa/21c4dc/21c3a0仅各离线生成一次，Known/a64/63、原guest/policy，五hostlibs三阶段hash一致已释放。IR/Home逐字同31，N4/B1、无prologue、full95 flags metadata保持，25/25/27words不减，frame/spill0，均无fault site；21c3aa和21c3a0未发布words甚至逐字同31，21c4dc仅条件方向和两个target DATA顺序互换。primary head/gate/pointer/fallback分别2/16/18/20、2/16/18/20、4/18/20/22；其它普通link target保。主收益在实际patch位置，不能以whole-unit词数差证实或否定它；无新RX逐字绑定。报告`batch33/native-quality-review.md` SHA `602cfbe5ff769ebd26d15a6fb5bf8ae25f1344ee6fba28370f44c1d09d4f88be`，comparison JSON SHA `1ef5b0cca0edfa4c893844a014bb2cf628492d8a13bca1f160726591ff06e9a5`。

33 Block三cold样本实为2045/2042/2045attempt、N36672/36638/36672、words65867/65804/65867、native bytes320780/320192/320780；first sample与旧2042/N36638不同，不能全部套旧规模，也不把一次first代表所有记录。Function三次均746attempt=722Function＋24Blockretry，N86893/words152552/native bytes518324，全部失败/重试和release继续计费。folded52/126、erased634/1418保持，spill25/42、maxframe48/64B仍未解决；局部frame0不扩为全应用新增无spill。

Host5/Android264468累计checks＋原12/4170、formatter、SMC、EOF、原fault/flags路径全pass；大型Emit/Need/Constant fixture累计不能写成新增264K语义case。三方100000次validation/CRC0xd340、实际CPU4相符。80文件as-built source `8d1b2d0a606ea32291c2a3060f9f3664c64a627e24558be3a4d9303fb0b3df86`，runner `8060f0d483d94029b47420480f333627606824bbeb7c37de7e0530ca49fdd67f`（8,418,352B）。32/33本次三docs与hash另存`batch33/post-measurement-docs`，不改变两个冻结80文件摘要；24/27旧fixture失败及30 attempt0 test-only ADL失败身份仍原样保留。

最终namespace cleanup du37,729,280B、df实际＋37,789,696B、remaining0，无active/openfiles/errors。生产33继续冻结，原仓库/Git/WIP、固定FEX、旧证据不动。目录final模型、20次真实probes、all-entry census与普通长跑保持分域；没有新诊断耗时baseline、新IR/表/pass或未授权后续机制，总目标仍开放。


## 第三十四批：真实callable目录的ready门收口（当前checkpoint）

由Runtime实际owner提供`callable_directory=true`，保证有效slots/mask和每槽可调用cold/native entry，公开默认false仍保null guards。Inline首槽ready删除CBZ slots和CBZ entry，两门10→8条；mismatch统一读取occupied，empty进cold、occupied继续probe，原ready-entry快捷分支也删。故额外ready碰撞的局部循环旧8→新9条，不是所有probe都省两条；不从离线模板推测全应用净周期。单一目录/ready-pending语义/CodeArena退休与SMC仍原owner，无新IR/pass/Home字段或bank。

| 指标 | Block | 请求Function（含retry） | FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 47.879109000 ms | 84.502106000 ms | 48.133177083 ms |
| Compile / FEX | 0.994721560× | 1.755589619× | 1.000000000× |
| 有效100000次运行 | 16.885 s | 16.058 s | 15.097 s |
| 运行 / FEX | 1.118434126× | 1.063655031× | 1.000000000× |
| 用户态周期 | 19,386,747,792 | 18,482,725,400 | 17,353,332,914 |
| 周期 / FEX | 1.117177195× | 1.065082166× | 1.000000000× |
| 用户态指令 | 67,029,624,149 | 64,392,931,982 | 61,981,529,713 |
| 指令 / FEX | 1.081445141× | 1.038905175× | 1.000000000× |
| branch-misses | 43,606,343 | 13,836,358 | 18,471,891 |

**Function本轮time/cycles继续在1.10×内，Block仍未达，总目标开放；两scope完整compile≤2×。** 相比33自身time，Block17.126→16.885s（−1.407%）、Function16.448→16.058s（−2.371%），FEX15.079→15.097s也变；保各自分母，不拆认单hunk收益。whole-child用户态PMU来自普通长跑、不带byte clocks/诊断counter；只在compile诊断扣实际byte库调用，全部失败、retry、形成、IR/优化、分配/编码、发布/释放继续收费。

只3unique实际单元各一次离线生成：B21c3b3/B21c4e5/Fn21c483，Known/a64/features63、原guest/policy，三JSON显式打印实际callable true，五hostlibs重链/生成前后hash一致已释放。B21c3b3与31 IR/Home逐字同，words33→29（每site三guard及一个cold DATA pad）；Fn21c483与32 IR/Home同，158→148（三site九guard及一个pad）。B21c4e5无旧同PC产物，仅报actual32words，不补旧。三者frame/spill0，actual Raw.NativeCalc/精确flags保留；fault guestPC/TEST/PF/carry配方与fast/cold前两字保，hot/cold偏移真实重算，全0LinkSite。无新运行RX逐字绑定，不以wholewords×entry认动态G。短报`batch34/native-quality-review.md` SHA `50a0824409c4927d7f505afff3b88d58f6372ed59d70393a6b9ef22507b298b1`，JSON SHA `99d598fe28def4d63c952b4fb7b1a4f1a31e22d5a0cd34e5363536e14efc7292`。

三cold Block诊断样本均2045attempt/N36672/words65867/native bytes318516；Function均746attempt=722Function＋24Blockretry，N86893/words152552/native bytes513740；folded52/126、erased634/1418保持。全应用spill25/42、maxframe48/64B仍未回收，不能拿选中三unit的frame0替代这一事实。

Host5、Android264478累计checks和原12/4170、formatter、SMC、EOF及原fault/flags检查全部pass；Emit/Need/Constant fixture累计不等于264K新语义case。三方100000validation/CRC0xd340、实际CPU4相符。80文件as-built source `c176c015f5d5ed0d79f0ed7b478def0869a40e93c66971ee324280c150a8eb58`，runner `365511fea7fa0fd58a1d9ce63f3cb7b5d81206cad6410fcd11e5c632a4453b19`（8,420,272B）。三个测量后文档及before/after/patch/hash收据单存`batch34/post-measurement-docs`，不重标80文件已构建身份；旧失败和32/33中间点均保留。

最终namespace cleanup du37,749,760B、df实际＋37,810,176B、remaining0，无active/openfiles/errors。当前普通生产34冻结，原仓库/Git/WIP、固定FEX和全部旧证据不动；此次文档更新没有追加生成、试验或未测候选，总目标仍开放。


## 第三十五批：PF raw消费与signed load

完整批次保Uniform IR、三遍B、一份Home及原生命周期；没有新pass/表/IR opcode。RawAF全旗8/16位逻辑结果以SXT到rawPF1合并复制和符号比较，普通numeric canonical截断保持，live结果在1时保原路。RcpcImm9原四门、同块紧邻U16 load→U32 SignExtend、full16view、sole-use/last-use、无spill且不碰deferredPF时，LDAPURSH W直接写consumer既有Home，仅精确一consumer跳SXT；load自身旧save跳过。原site/guestPC/flags/旧pin故障合同保，冷fallback为LDURSH W+DMB ISHLD+B回fast+1，解码与修补保持signed。

| 指标 | Block | Requested Function | 同轮FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 47.996810 ms | 83.615719 ms | 48.255989583 ms |
| Compile/FEX | 0.994629069× | 1.732753172× | 1× |
| 有效100000次运行 | 16.767 s | 15.974 s | 15.127 s |
| Runtime/FEX | 1.108415416× | 1.055992596× | 1× |
| user cycles | 19,258,684,603 | 18,384,034,904 | 17,389,147,023 |
| Cycles/FEX | 1.107511747× | 1.057213150× | 1× |
| user instructions | 66,120,093,537 | 63,493,976,319 | 61,981,523,906 |
| Instructions/FEX | 1.066771021× | 1.024401665× | 1× |
| user branch misses | 45,101,485 | 17,619,539 | 18,681,326 |

三者CRC d340/有效≥10s，实际PID CPU4；min/max 614400/1171200kHz，未写频率。**Block还高于1.10×，总目标仍开放；Function与两scope compile达标。** 相对34自身runtime−0.699%/−0.523%、instructions约−1.357%/−1.396%，FEX时间也变，不能独立归因各hunk。需再省约0.759%的Block自身耗时才到本轮FEX的1.10×，此数不是预测收益。

两scope所有cold失败/重试/发布/释放计费，requested Function仍722 Function＋24 Block retry，不是纯Function无fallback。首Block N36672/operand65867/native318284，三cold样本明细保留；全应用spill25/42、maxframe48/64B未解决。Host5、手机后端264524累计checks、原12base＋4170 native pipeline、formatter/匿名SMC/文件别名EOF（125）全部通过；264K主要是既有远分支fixture逐字Need，不能当语义矩阵数。

仅三个指定offline单元各一次：B2197b0 IR/Home同34，whole34但热PF少1，尾部cold pad抵消；B21b2d0 IR/Home同、61→57 words，四条LDAPURSH W22/W2取代Hload+SXT，四原PC/recipe保且cold对应signed；Fn21c483 IR/Home/148words/site/link逐字同34。局部frame/spill0不覆盖全应用。`native-quality-review.md` SHA `62a539030513ab52f76f51b98c95d7b458fa5ef7aedad88c371deaf4bdf4f6a5`，comparison `4f753901e6912c9bfe79b7210446a21637643d1f1cdac178dcff2539cb48fc44`，均无RX绑定或局部收益外推。

80source摘要 `a40faa3165b9186754ccec697b024a8ca65f4db5639aae545c229fb5d14989cd`；runner SHA `604424430e7265d139848c5af89700e1ab08796d1d141e4b0680c5e9d9bb6ef4`，8,423,456B。证据在`qor-parity-implementation-20261103/batch35`，原始命令、policy、PID、stdout/PMU、compile记录与signed/PF分阶段patch均保留。手机namespace已检查own ps/lsof后清空：du37,782,528B，df实际＋37,842,944B，remaining0，无active/openfiles/errors。三docs测量后独立存`batch35/post-measurement-docs`，不变更as-built80，原仓库/Git/WIP仍冻结、未提交或push。

## 第三十六批：受限byte发布、wide TEST与load地址表达式

C省去UBFX仅限RawAF XorU8/full95、LogicalZeroCVA、非legacy、不spill、结果非PF1，唯一数值用户为同B紧邻default pinned U8 Store及完整8位view。35的SXTB1与原BFXIL分别截取其实际低8，数值/raw/wide/多use路径保持。Full95 U64 self TEST仅numericdead时用SUBS X1,source,#0；原Read保宽度语义，PF低byte/AF0/NZ不变，硬件C1已正确而不再CFINV。私有LogicalFlags尾参数默认保旧调用，DeferTest先行，既有self/fault/tag合同不变。

B只在原forward识别同B/default Add/Sub U64的sole-use非pin地址，base自然及有效U64、literal full64，模64位位移[-256,255]，consumer限default scalar LoadMemoryTSO。原load ID/type/故障顺序保留，既有Expression交C；成功Rewrite后迁实际use账，末逆DCE退地址producer。追加≤4words由load和独占producer覆盖，原oldW+2N硬上限、三次walk、一张u64N与唯一owner保持；Unknown中立语义与RCPC-off完整EA fallback保留，没有新IR/pass/表。

| 指标 | Block | Requested Function | 同轮FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 46.799072 ms | 82.989579 ms | 47.452552083 ms |
| Compile/FEX | 0.986228768× | 1.748896010× | 1× |
| 有效100000次运行 | 16.575 s | 15.845 s | 14.998 s |
| Runtime/FEX | 1.105147353× | 1.056474197× | 1× |
| user cycles | 19,034,878,099 | 18,241,695,053 | 17,235,586,414 |
| Cycles/FEX | 1.104393993× | 1.058373914× | 1× |
| user instructions | 64,648,262,642 | 62,117,779,015 | 61,981,473,747 |
| Instructions/FEX | 1.043025581× | 1.002199129× | 1× |
| user branch misses | 44,263,899 | 18,257,864 | 18,754,341 |

三者CRC d340、validation有效且运行≥10s，实际PID CPU4；原CPU4 min/max614400/1171200kHz条件保留，未锁频。**Function继续满足本轮1.10×time/cycles，两scope compile≤2×；Block时间及cycles仍高于门限，总目标未完成。** 相对35自身time约−1.145%/−0.808%，instructions约−2.226%/−2.167%；同轮FEX也变化，不拆认三个hunk贡献。Block距本轮时间门限约差自身耗时0.466%，只是门限算术，不是承诺收益或统计保证。

三cold Block均2045attempt/N36672/operand65867/native317268，Function仍746attempt=722Function＋24Blockretry、N86893/words152552/native509492；全应用spill25/42、maxframe48/64B未解决。folded142/304、erased724/1596。实际失败/重试/形成/IR/B/分配/发码/发布/释放均计费，只扣真实byte-library调用；三次fresh诊断中位不替代ordinary长跑。Host5、Android264542累计Need/fixture检查、原12base＋4170 native pipeline、formatter/匿名SMC/文件别名及EOF预期125集中通过，高Need总量不当成独立语义case数。

仅四个指定offline单元各一次，原guest/Known63/callable true及policy由实际JSON打印：B21979b IR/Home同35，SUBS替ANDS/CFINV，热少一条、whole30→28；B219790退N5地址Add、保N6 load，base寿命5→6及Home3→2按新图真实收费，LDAPURH W2/LDURH W2都用[X10,#2]，热少ADD但whole35不变；B2197b0 IR/Home同，省byte输出UBFX，热少一条、whole34→32；Fn21c483 IR/Home/148words/十site/零link逐字同35。四者frame/spill0不覆盖全应用压力，原guestPC/TEST/PF/carry recipe及cold回hot+1保持；没有新RX绑定、采样或wholewords×entry外推。质量报告SHA `3f81daff9ff34dda9eb3013118f9a827d957068a199c4103079416c31923ab32`，comparison JSON SHA `e6760820f9e678095a4c615d1f82f8c3fb89194a639ea1d8997ff7ca814a9507`。

80文件source-as-built摘要 `87d1c870bf05ea0d69f2dcbb0ff25432d6ab89c185781abff737ac89d14b09d0`，runner SHA `14de0114e2921a67388f098adeca46ca398d799fffff312c24ba1c72c7329739`（8,436,728B）。原始compile/runtime JSON、命令、PID、PMU与两个阶段patch保留在`qor-parity-implementation-20261103/batch36`。手机namespace清理du37,831,680B、df实际＋37,892,096B，remaining0、无active/openfiles/errors。三docs以35归档after为before，测量后独立归档`batch36/post-measurement-docs/{before,after,source.patch,receipt.json}`；不改已构建80文件身份，原24/27 fixture及30 attempt0失败等全部历史、原仓库/Git/index/WIP继续保留冻结。

## 第三十七批：实际标量consumer融合；本轮固定输入量化验收通过

完整批次只在原C actual consumer收敛三处。MADD限同B紧邻default U32 Mul→无旗Add，product恰占Add一个Value输入、原last-use恰consumer定义位置、三源完整U32/Carrier≥32且输入/结果不spill，PF目标冲突回退。沿原ReadW直接写consumer既有Home，producer.saved不保存旧Mul，复用同一个`elided_consumer`精确跳Add；Legacy SaveFlags/可夹Clear、metadata fullflags及其它形态保原乘加。读后写保原pin，non-S结果按模32等价且旧flags保持。

36 sole pinned-byte-store检查移成无状态helper，旧full RawAF XOR准入保持；新默认U8 Add/Sub bare immediate、flag_mask0共享同B紧邻U8/fullview/唯一数据用户的实际BFXIL字段。原模8正规化、较窄view Read、旧flags与父高位保持，普通multi-use/raw/wide路径不变。Signed ordered-load仅在原strict16→32门内增加U32→U64：RcpcImm9原四门/真实marker、相邻同B/default/fullview/sole-last-use、双方不spill且不覆盖deferredPF。LDAPURSW X成功才提交consumer Home；Run与handler同认19800000，cold XOR21000000得到LDURSW X+DMB ISHLD+B回fast+1，非writeback Rn==Rt安全、abort保旧pin/guestPC。原IR/Home/ABI、三次B、一张u64N、16B Home与所有权不增层。

| 指标 | Block | Requested Function | 同轮FEX |
| --- | ---: | ---: | ---: |
| 去byte完整compile，3次fresh中位 | 48.187132 ms | 84.762869 ms | 47.3196875 ms |
| Compile/FEX | 1.018331577× | 1.791281251× | 1× |
| 有效100000次运行 | 16.302 s | 15.493 s | 15.072 s |
| Runtime/FEX | 1.081608280× | 1.027932590× | 1× |
| user cycles | 18,728,836,155 | 17,846,732,735 | 17,324,067,619 |
| Cycles/FEX | 1.081087685× | 1.030169884× | 1× |
| user instructions | 63,676,531,817 | 61,145,976,904 | 61,981,552,180 |
| Instructions/FEX | 1.027346518× | 0.986518968× | 1× |
| user branch misses | 43,130,249 | 14,228,779 | 18,729,668 |

三方CRC0xd340、CoreMark validation有效且运行≥10s，实际PID均CPU4。前后CPU4 min/max614400/1171200kHz与温度读数254一致，未锁频或改频率。**两scope本轮time/cycles/instructions均≤1.10×，compile均≤2×，固定输入量化目标完成。** 判定见[aggregate measurement receipt](/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-parity-implementation-20261103/batch37/measurement-receipt.json)，SHA `fd0ebb07db6dd9237f5534ab538a0e568f766b4bfe7a317b5473c73f3fd14a4b`。原runtime-summary的`goal_achieved=false`是历史硬编码占位，原bytes保留；汇总按两scope四门八项实测、功能/真审/身份闭合判true，原数字未改。相对36自身time约−1.647%/−2.222%，instructions约−1.503%/−1.564%；FEX分母也变化，不孤立认领三个hunk动态收益。

三cold Block均2045attempt/N36672/operand65867/native317132；Function746attempt仍完整计722Function＋24Blockretry、N86893/words152552/native509124。folded142/304、erased724/1596保持。全部实际失败/重试/形成/IR/优化/分配/编码/发布/释放继续收费，只扣真实byte-library调用；ordinary长跑无byte时钟、计数或采样。全应用spill25/42、maxframe48/64B未解，候选first-eviction/20B未实施且不是本轮通过的前提。Host5、Android264579累计Need/fixture、原12base＋4170 native pipeline、formatter/SMC/文件别名及EOF预期125全部通过；264K累计数不称独立语义矩阵。

仅四个指定offline单元各一次，冻结工具/五库及Known63/callable true/原guest-policy闭合。所有IR/Home均同既有基线：B21c3b3对34热少SXTW、whole29因cold pad抵消，signed32 cold是LDURSW X2,[X2]；B21c4b1对36省byte UBFX、31→29；B21b2d0对35仅第二组无旗pair用MADD，第一组fullflags/故障PF26保，57→55；Fn21c483对36实际148→146，byte UBFX与signed32各少一条，无MADD。whole差值含真实cold对齐pad，不能按entry频次外推整Function执行数。原guestPC/TEST/PF/carry、冷回fast+1及link目标保持，实际坐标按布局重算；没有新RX绑定或FEX生成。质量报告SHA `53c37434dccdbb678da7bd425eb533d98476cfe65bf12134c7b71405df8e1567`，comparison JSON SHA `54b4951a9bba6f738f0100e1cc5ee60a197bbfbb434e5ba232f50c5a693c0542`。

80source-as-built摘要 `79c8bf0c2046ab35ece932b5e74310b13d5de339b00ee01e4def117235cdd985`，runner SHA `d927b24230bd4e9b80cb771394d8761dd1c382a59a0391f750321d9a9aa051aa`（8,447,952B），原始命令/records/PMU/PID/实际patch及完整汇总保留在`qor-parity-implementation-20261103/batch37`。cleanup du37,889,024B、df实际＋37,949,440B，remaining0、无active/openfiles/errors。三docs以36 correction-after plan及原36after另外两份为before，测量后单独归档`batch37/post-measurement-docs/{before,after,source.patch,receipt.json}`，不改变as-built80。原仓库/Git/index/WIP继续冻结、未commit/push。验收范围是本轮固定输入及已实现路径；完整guest线程/信号、动态ELF、全ISA和所有应用普遍性能保证仍在既有功能边界之外，没有扩成新的目标或审批流程。
