# 线程 L1 改造后与固定 FEX 的常规 benchmark 对照（2026-11-08）

本轮保持已测试的线程 L1 / VM 共享 L2 产品二进制不变，补跑8项常规负载的同轮FEX对照。8×3引擎×3次，运行和compile分开，共144条有效主观测；固定交错顺序，保留全部样本和范围，不选最快值。

## 测量身份与边界

- 红魔NX729J `bf1359ff`，CPU4，min/max614400/1171200kHz；没有写频率、清系统页缓存或调整其他服务。
- SVM基线提交 `cbd06547a2d9e5259f65bc99a69e95c055aa8bcb` 加尚未提交的L1/L2实现；运行二进制SHA256：`0e53e1c3a0f528377740713197ae647d57939dc0b684607337e7a1464e807823`，与上轮实现验收完全相同。本轮没有修改产品代码。
- FEX固定Android参照为 `a156752fae3a4f65fefb1ed95af980bcdfac43d7` 加既有Android补丁；运行版SHA256 `8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175`。保留默认multiblock和TSO，关闭AVX/AOT/object cache；不能将CompileBlock直接等同SVM Block scope。
- guest ELF与golden延续既有8项输入，`inputs.json`登记实际路径/大小/SHA。bench-suite按真实x86 native checksum验收；func_tests按native完整stdout和exit101验收；x87继承exit-only输入，不冒称完整数值证明。
- 运行组为普通新进程，无decode时钟或派发计数。PMU user cycles/instructions/branch-misses同组，全部time_enabled=time_running；计数包括启动/JIT/回收和共同wrapper。counter PID为被simpleperf测量的父wrapper及其继承子树；记录实际guest PID和可捕获的CPU4亲缘证据。私有FEXServer在被测子树外。
- CoreMark100000使用guest循环timer，CRC d340且每次≥10秒；其他用例报告整进程墙钟，包含启动和编译。不同负载的时间口径不混合成总分。
- compile组使用SVM `--decode-timing`和另一份固定FEX byte/stage计时二进制；CoreMark仅20迭代，CRC4983但不足官方时长，只用于compile测量。排除双方实际byte parser时间，包含formation/IR/opt/RA/emit/publication及现有释放端点；探针本身扰动计时。
- FEX旧probe终点仅包含logical working-buffer release，不包含最终native arena析构；SVM包含native owner释放。此端点差异明确保留，不把它隐去。

bench-suite的int/fp/mem/branch/call使用同一个RWX PT_LOAD；这些Function请求全部触发SMC单块形成策略，仍使用Function分配，但没有形成通常的多块region。此处的Function标签不能解读成正常region优化上限。详见[差距原因分解](benchmark-gap-analysis-2026-11-08.md)。

## 运行结果

各项为固定3次中位数。时间单位秒；cycles与指令比均以本轮同输入FEX为1，小于1表示SVM成本较低。

| 负载 | Block秒 | Function秒 | FEX秒 | cycles B/F | 指令 B/F |
| --- | ---: | ---: | ---: | ---: | ---: |
| CoreMark 100000 | 16.0900 | 15.4170 | 15.0620 | 1.067× / 1.023× | 1.018× / 0.989× |
| 整数 | 0.3233 | 0.2425 | 0.3455 | 0.687× / 0.689× | 1.017× / 1.017× |
| packed FP | 0.7181 | 0.7163 | 0.3144 | 3.391× / 3.378× | 6.136× / 6.140× |
| 内存 | 2.1621 | 2.1707 | 2.1540 | 1.050× / 1.051× | 1.257× / 1.257× |
| 分支 | 0.2713 | 0.2505 | 0.2845 | 0.982× / 0.981× | 1.081× / 1.081× |
| CALL | 0.4914 | 0.4802 | 0.5292 | 1.055× / 1.056× | 1.215× / 1.215× |
| func_tests | 0.1201 | 0.1385 | 0.1808 | 0.769× / 0.958× | 0.747× / 1.019× |
| x87 | 0.6988 | 0.5972 | 0.8774 | 0.693× / 0.675× | 1.374× / 1.371× |

CoreMark分数（iterations/s，越大越好）：block 6215.04，function 6486.35，fex 6639.22。

短负载墙钟容易被调度放大，须结合PMU判断；原始范围均保留在summary.json。FEXServer启动不计入该子树，不能据短进程墙钟直接声称纯代码生成或热循环提升。

### packed FP差距

本轮最明显的代码生成差距是packed FP：两种scope的cycles约为FEX的3.4倍，指令约6.1倍；两种scope均零解释器回退。该循环主要执行packed SSE2加乘/比较，指令差距无法由启动时间或墙钟调度解释。

静态代码审阅显示，`backend/arm64/emit_float.cpp` 的 `HandleVecFAddFamily` 对两输入调用 `RecordDenormalInput`，额外发出lane/exponent分类、分支和必要的FP状态维护；这些来自先前 `658662cd` 的SSE/AVX sticky exception正确性修复。L1/L2这一轮没有修改该文件。测量已确认差距，额外检查占比仍需独立profile/发码审计，不能仅凭源码推导每一条PMU指令的归属，也不能直接删掉正确性合同。

追加固定3对父版本/候选Block交错测量（独立于主矩阵）：父版本cycles中位732820956，候选733011371（+0.026%）；指令分别1525494075与1525486760。该FP差距在L1改造前已经存在，不能归因于这次缓存替换。原始记录在fp-parent-attribution/，未与主批FEX比值混池。

## compile结果

单位ms，固定3次中位数。下表同时列原始含decode及排除byte parser的值；与FEX比值使用排除byte parser口径。

| 负载 | Block含decode | Function含decode | FEX含decode | Block排decode | Function排decode | FEX排decode | B/F相对FEX |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| CoreMark 20 | 45.275 | 72.069 | 55.239 | 40.963 | 66.602 | 47.195 | 0.868× / 1.411× |
| 整数 | 2.238 | 2.213 | 1.813 | 2.063 | 2.045 | 1.516 | 1.360× / 1.349× |
| packed FP | 2.966 | 3.205 | 2.074 | 2.674 | 2.936 | 1.741 | 1.536× / 1.687× |
| 内存 | 2.661 | 2.627 | 1.884 | 2.418 | 2.418 | 1.577 | 1.533× / 1.534× |
| 分支 | 2.165 | 2.156 | 1.893 | 2.002 | 1.993 | 1.599 | 1.252× / 1.247× |
| CALL | 2.444 | 2.415 | 2.038 | 2.282 | 2.249 | 1.574 | 1.450× / 1.429× |
| func_tests | 23.171 | 35.653 | 35.114 | 20.975 | 31.843 | 30.121 | 0.696× / 1.057× |
| x87 | 0.817 | 0.687 | 0.319 | 0.782 | 0.667 | 0.298 | 2.626× / 2.238× |

CoreMark的Function三次排decode值为 84.281 / 66.602 / 49.003 ms；仍可见明显调度波动，本轮按预登记3次中位数原样报告，不用之前线程CPU诊断替换。

x87 compile两种scope仍超过2×FEX；虽然绝对差约数百微秒，当前目标按比值定义，不能将其宣告达标。其他7项本轮compile比值小于2×。极短输入更易受映射初始化、分配和回收等固定成本影响；本轮未对这些成本另作因果拆分。

## 与既定目标的关系

继续使用原有时间≤1.10×、user cycles≤1.10×、compile排byte≤2.00×的比较线，不新增“指令数必须≤1.10×”一类未约定门槛。本轮未全部满足这些数值目标，超线项如下：

| 负载 | scope | 指标 | 本轮/FEX | 比较线 |
| --- | --- | --- | ---: | ---: |
| packed FP | block | seconds | 2.284× | 1.10× |
| packed FP | block | cycles | 3.391× | 1.10× |
| packed FP | function | seconds | 2.278× | 1.10× |
| packed FP | function | cycles | 3.378× | 1.10× |
| x87 | block | compile_excluding_byte | 2.626× | 2.00× |
| x87 | function | compile_excluding_byte | 2.238× | 2.00× |

三次观测不构成统计显著性、全ISA或所有应用的性能保证。checksum通过与数值目标是否满足分别记录。不能把旧2026-11-05的8项达标结论沿用到当前FP正确性修复后的代码。

## 间接派发专题与证据

另有上轮同一二进制的14个派发用例、126个正式热态PMU样本：单态/四目标JMP约与FEX同水平，64目标随机JMP的Block/Function约0.78×/0.95×FEX cycles；单态CALL+RET仍约1.19×/1.18×。这些是预热后的专题循环，不与上表含冷JIT的常规应用计数混合。详情见 [L1/L2实现报告](thread-dispatch-cache-2026-11-08.md)。

证据根目录：`/Volumes/Data2/swiftvm-middle-results-relocated-20261025/post-l1-benchmark-20261108`。主要入口为plan.json、inputs.json、runtime-records.json、compile-records.json、summary.json与compile-breakdown.json；保留所有stdout/stderr、实际PID、affinity、raw PMU、前后身份和源快照。

采样脚本有两项修正：PMU继承事件归属wrapper PID，首个有效CoreMark原样保留后修正解析；短进程可选live probe在/proc读取时退出，保留该点CSV/exit/PID并用独立capture-retry前缀补采一条，原因是stdout捕获失败，非按成绩选样。计划次数、已有效样本和产品二进制均未更改。

私有手机命名空间已在确认无活跃执行/打开文件后清理，释放87856128 B；唯一原始证据与可复用构建仍在Data2。本轮未提交/push代码。
