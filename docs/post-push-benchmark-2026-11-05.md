# 提交后全benchmark五轮锚定结果（2026-11-05）

本轮固定输入的48项性能门槛全部通过。本次以已推送subject `390cf73d7d8db5687a8d33cc8be5cdd6aa38638f` 为唯一候选，每项Block／Function／固定FEX各五次，共120条普通运行记录和120条独立编译记录；功能有效性分别为120/120与120/120。所有预先登记样本均保留，未按门槛追加、替换或与旧结果混池。

这是对已提交实现的差距锚定。本文中的‘通过’只覆盖固定八项输入、两种scope和本次设备条件；不代表完整x86 ISA、动态ELF或完整异常／信号兼容。

## 1. 提交、输入与固定口径

| 身份 | 值 |
| --- | --- |
| 测量subject／已核远端提交 | `390cf73d7d8db5687a8d33cc8be5cdd6aa38638f` |
| 代码／测试提交 | `7f1f403a9c429c664914867439ef6315ac9f3e3b` |
| 分支 | `refs/heads/refactor/middle-pipeline-rebuild-parity` |
| 118-file source digest | `a78fdec20b3de2f00fe47bb1dbfa67c6664c0efa7cd1b7f30658edd8f1ce1e52` |
| candidate runner SHA256 | `13411690d78fa72df6fc84c61004bfe7c36a2060cc3a9a16cb22dcbfca9d77eb` |
| 固定FEX revision | `a156752fae3a4f65fefb1ed95af980bcdfac43d7 + original Android patch` |
| 普通FEXLoader SHA256 | `8632943c4736294ae8e76ab15c22a638e2f3db4959cdf5ca0e7c1d741024b175` |
| byte-probe FEXLoader SHA256 | `d2e4ce32b6c650bb9f7ce46a91aff8a098431ad3f5f52f52fd746d30b3500995` |
| FEXServer SHA256 | `9fe58a9946e9105c49911518356fe29a4ed3143c1fe90803b57afe112c225e8c` |
| FEX Config.json SHA256 | `275a50c60d9852ef3399bd9fd665d93cf46a06850a5dd15bd31dffe11e9cb6b0` |

source digest是路径＋文件内容SHA的清单摘要，不是Git commit。subject包含已提交的说明与历史证据；结果文档与证据另行提交；测量对象绑定上述subject，结果提交不会改变被测实现。原始身份见[plan.json](evidence/post-push-benchmark-20261105/plan.json)、[identity.json](evidence/post-push-benchmark-20261105/identity.json)、[source-manifest.json](evidence/post-push-benchmark-20261105/source-manifest.json)及[inputs.json](evidence/post-push-benchmark-20261105/inputs.json)。

| 固定guest ELF | SHA256 |
| --- | --- |
| coremark | `d92d8d77b15714c59cb028cf412017ce0ee4425299ffe97536c6ef4a6c0bcbba` |
| bench-suite | `df60aa3f57d40ed3eb0b44d124f2cb8c9d15899f90021b6d20e94be179240a6d` |
| func-tests | `4740aad183296f458756a6ebb5abf42a2affb17dddd3b74d115a8e2f3266ae60` |
| x87-bench | `60350fb2a0828e63934a3c8e4890302448d028ce8875a17dfa96b141f91bfabb` |

设备 `bf1359ff`，CPU 4，`taskset 10`。策略min/max为614,400/1,171,200 kHz；不修改频率策略或系统服务，不能称锁频。实测前后记录如下，温度列为原始sysfs整数：

| 窗口 | min kHz | max kHz | current kHz | temperature raw |
| --- | --- | --- | --- | --- |
| compile前 | 614400 | 1171200 | 1171200 | 260 |
| compile后 | 614400 | 1171200 | 1171200 | 261 |
| runtime前 | 614400 | 1171200 | 1171200 | 261 |
| runtime后 | 614400 | 1171200 | 1171200 | 264 |

120条wrapper状态均确认CPU4；其中103条另有运行中的目标进程CPU4核对，17条仅保留wrapper状态、实际PID／exit关联及子进程affinity继承证明，未补造live采样。完整边界见summary.validation.affinity。

每样本启动新launcher，无AOT/object-code缓存，保留OS page cache；三arm按预先固定顺序循环。FEX维持同一Config、AVX关闭和TSO设置，未为本轮改变分母。普通运行与decode-timing编译分开启动；前者不使用诊断编译耗时作为运行成绩。用户态PMU包括同一child树的启动、冷JIT及wrapper；独立FEXServer位于该树外，未计入。

CoreMark运行参数为`0 0 0 100000 7 1 2000`，CRC d340且loop计时须至少10秒；该行time使用guest loop ticks。其余七项使用whole-child simpleperf elapsed，包含启动和JIT，不能与CoreMark时间窗口混为统一评分。编译CoreMark只跑20迭代、CRC4983，短时官方validity warning按计划保留，不作为官方CoreMark分数。其他guest参数和checksum／exit oracle见[cases.json](evidence/post-push-benchmark-20261105/cases.json)；func_tests保原stdout及exit101，x87_bench继承exit-only oracle，exit0不证明完整数值语义。

## 2. 运行时间：各五次中位数

单位ms；ratio为本scope中位数／同轮固定FEX五次中位数，越小越好。不取逐轮ratio的中位数，也不合并工作负载平均分。

| 输入 | Block ms | Function ms | FEX ms | B/FEX | F/FEX |
| --- | --- | --- | --- | --- | --- |
| coremark | 16119.000 | 15627.000 | 15047.000 | 1.071243 | 1.038546 |
| int | 302.628 | 302.164 | 319.099 | 0.948383 | 0.946929 |
| fp | 247.220 | 250.067 | 263.165 | 0.939411 | 0.950229 |
| mem | 2160.671 | 2177.999 | 2050.361 | 1.053800 | 1.062251 |
| branch | 257.386 | 258.774 | 267.166 | 0.963394 | 0.968589 |
| call | 478.907 | 476.340 | 460.067 | 1.040951 | 1.035371 |
| func_tests | 110.294 | 120.421 | 138.470 | 0.796519 | 0.869654 |
| x87_bench | 556.393 | 552.645 | 862.285 | 0.645254 | 0.640908 |

## 3. 用户态PMU差距

下表分别对cycles和instructions取五次中位数后相除。instructions和branch-misses用于解释差距，不新增为性能合格门，也不能仅凭计数变化给某条优化作因果归因。

| 输入 | cycles B/FEX | cycles F/FEX | instructions B/FEX | instructions F/FEX |
| --- | --- | --- | --- | --- |
| coremark | 1.071600 | 1.038403 | 1.037899 | 1.009989 |
| int | 0.990041 | 0.990554 | 1.224240 | 1.266154 |
| fp | 0.981998 | 0.986483 | 0.990219 | 1.005909 |
| mem | 1.058587 | 1.066987 | 1.332670 | 1.366213 |
| branch | 1.001633 | 1.008696 | 1.137691 | 1.157643 |
| call | 1.077146 | 1.072170 | 1.240635 | 1.249902 |
| func_tests | 0.809168 | 0.965598 | 0.806196 | 1.041489 |
| x87_bench | 0.626508 | 0.624297 | 1.272632 | 1.269780 |

| 输入 | Block branch-misses | Function branch-misses | FEX branch-misses |
| --- | --- | --- | --- |
| coremark | 36,007,805 | 19,968,386 | 19,290,449 |
| int | 375,314 | 376,195 | 424,561 |
| fp | 383,999 | 384,149 | 425,806 |
| mem | 383,747 | 381,877 | 426,193 |
| branch | 376,246 | 376,019 | 425,413 |
| call | 375,315 | 375,370 | 426,019 |
| func_tests | 592,680 | 816,975 | 807,701 |
| x87_bench | 509,325 | 509,724 | 672,997 |

普通运行的全部原始计数、时间和stdout/oracle资格保存在[runtime-records.json](evidence/post-push-benchmark-20261105/runtime-records.json)。cold JIT占比随输入时长不同；此表不是纯steady-state JIT指令质量实验，不能把整段指令差直接当作可删除指令。

## 4. 编译总成本：只扣实际byte parser

单位ms。SVM用compile总窗口减实际byte-parser子区间；FEX用闭合CompileBlock树减其实际byte_decode区间。保留形成、IR出生、失败前缀／Function回退、B、分配与编码、发布及当前释放窗口。decode-timing／byte probe会扰动编译，故只能作同协议的总预算比较；不能把整个前端或formation都扣掉。

| 输入 | Block ms | Function ms | FEX ms | B/FEX | F/FEX |
| --- | --- | --- | --- | --- | --- |
| coremark | 45.688054 | 51.463000 | 47.699583 | 0.957829 | 1.078898 |
| int | 1.796358 | 1.823647 | 1.533177 | 1.171657 | 1.189456 |
| fp | 2.399743 | 2.585886 | 1.653385 | 1.451412 | 1.563995 |
| mem | 2.162390 | 2.134533 | 1.527865 | 1.415302 | 1.397069 |
| branch | 1.715833 | 1.792078 | 1.523438 | 1.126290 | 1.176338 |
| call | 1.760050 | 1.878809 | 1.589740 | 1.107131 | 1.181834 |
| func_tests | 22.606452 | 32.499146 | 30.488646 | 0.741471 | 1.065943 |
| x87_bench | 0.550678 | 0.529216 | 0.288646 | 1.907798 | 1.833444 |

FEX旧probe的logical工作buffer释放在窗口内，但最终native arena销毁不在该释放probe内；SVM当前code-owner释放计入总成本。这一owner-release不对称保留，未从候选扣除或给FEX补猜时长。组件规划预算及旧synthetic结果不能替代这里按scope独立的总编译≤2×资格。原始记录见[compile-records.json](evidence/post-push-benchmark-20261105/compile-records.json)。

## 5. 全部48项门槛的实际真值

每输入×scope检查time≤1.1×、user cycles≤1.1×、compile去byte≤2.0×，共8×2×3＝48项。判定使用未四舍五入数值；表面显示值仅便于阅读。原始样本有效与性能门通过是两项不同事实。

| 输入 | scope | time ratio /真值 | cycles ratio /真值 | compile ratio /真值 |
| --- | --- | --- | --- | --- |
| coremark | block | 1.071243 通过 | 1.071600 通过 | 0.957829 通过 |
| coremark | function | 1.038546 通过 | 1.038403 通过 | 1.078898 通过 |
| int | block | 0.948383 通过 | 0.990041 通过 | 1.171657 通过 |
| int | function | 0.946929 通过 | 0.990554 通过 | 1.189456 通过 |
| fp | block | 0.939411 通过 | 0.981998 通过 | 1.451412 通过 |
| fp | function | 0.950229 通过 | 0.986483 通过 | 1.563995 通过 |
| mem | block | 1.053800 通过 | 1.058587 通过 | 1.415302 通过 |
| mem | function | 1.062251 通过 | 1.066987 通过 | 1.397069 通过 |
| branch | block | 0.963394 通过 | 1.001633 通过 | 1.126290 通过 |
| branch | function | 0.968589 通过 | 1.008696 通过 | 1.176338 通过 |
| call | block | 1.040951 通过 | 1.077146 通过 | 1.107131 通过 |
| call | function | 1.035371 通过 | 1.072170 通过 | 1.181834 通过 |
| func_tests | block | 0.796519 通过 | 0.809168 通过 | 0.741471 通过 |
| func_tests | function | 0.869654 通过 | 0.965598 通过 | 1.065943 通过 |
| x87_bench | block | 0.645254 通过 | 0.626508 通过 | 1.907798 通过 |
| x87_bench | function | 0.640908 通过 | 0.624297 通过 | 1.833444 通过 |

本轮结论：**48/48**。

没有未过项；这仍是预先固定五次的本轮锚定，不将一次通过外推为所有输入或环境的稳定上界。

未重跑凑门槛、未可选停止、未删除失败样本、未把Block与Function合并判定。完整判定见[summary.json](evidence/post-push-benchmark-20261105/summary.json)的acceptance；原n3、A1／A2及x87七轮配对复核都只保留为历史，不混入本轮120＋120数据。

## 6. CoreMark编译阶段与实际资源

阶段表各arm只选择该arm总编译去byte排序的第3个实际样本（五次中位样本），所有分项来自同一次记录；不是独立分项中位数拼接。各列可闭合到该arm中位总值，单位ms。

| 阶段 | Block | Function | FEX |
| --- | --- | --- | --- |
| A：形成＋IR出生（扣纯byte parser） | 11.894416 | 16.592282 | 23.213281 |
| B：IR优化 | 9.119474 | 12.270046 | 6.272812 |
| C：分配＋编码 | 14.370116 | 17.262246 | 14.020521 |
| 复制／链接／发布 | 8.307871 | 4.425112 | 3.166146 |
| 当前释放窗口 | 0.431979 | 0.340521 | 0.316354 |
| 其余独占成本 | 1.564198 | 0.572793 | 0.710469 |
| 总编译去byte | 45.688054 | 51.463000 | 47.699583 |
| 所选round（零基） | 4 | 4 | 2 |
| 分项闭合误差ms | 0.000000000 | 0.000000000 | 0.000000000 |

FEX分项由remaining_ticks独占归约：形成＝generate_ir/raw_decode/builder；B＝pass_manager/x87/dfe/validation/other_pass；C＝ra/backend/emit；发布＝tail_copy_cache/publish；释放＝logical_release。byte_decode单独排除；不把嵌套inclusive计时相加。SVM的C仍为原后端分配＋编码窗口，没有另做纯RA实验。所有八项的同样分解保存在[compile-breakdown.json](evidence/post-push-benchmark-20261105/compile-breakdown.json)。

资源表取本轮CoreMark20编译诊断的实际record；同一scope五次不同时显示最小–最大范围，不选择最有利值。SVM成功IR数明确不包含拒绝前缀，emitted guest数包含其实际已发射前缀，因此不能把前者称完整出生账。

| SVM实际资源 | Block | Function |
| --- | --- | --- |
| 全部编译尝试 | 2,043 | 686 |
| Function尝试 | 0 | 681 |
| Block尝试 | 2,043 | 5 |
| Function→Block回退 | 0 | 5 |
| 发布的完整unit owner | 2,043 | 681 |
| 已发射guest指令出现（含拒绝前缀） | 12,407 | 16,584 |
| 已发射guest字节出现（含拒绝前缀） | 46,689 | 63,906 |
| 成功IR节点（不含拒绝前缀） | 36,651 | 50,591 |
| 成功operand words（不含拒绝前缀） | 65,841 | 84,624 |
| 已发布native bytes | 389,876 | 393,248 |
| 累计spill slots | 25 | 31 |
| 最大unit frame bytes | 48 | 64 |
| code arena峰值虚拟bytes（含双别名） | 2,097,152 | 2,097,152 |
| 峰值ready PC keys | 2,043 | 3,438 |
| 峰值occupied PC keys | 2,605 | 3,509 |
| 峰值目录slots | 32,768 | 32,768 |
| 目录分配次数 | 23 | 22 |
| 实际FaultSite数 | 5,075 | 6,762 |
| link relocation rows | 3,101 | 1,216 |
| link relocation容量 | 4,096 | 2,048 |
| 缓存失效次数 | 0 | 0 |
| 非对齐冷恢复次数 | 29 | 22 |
| 非对齐补丁次数 | 29 | 22 |

该FEX代表样本CompileBlock root calls＝255。此root计数不是SVM的ready keys或完整unit owner数，原probe没有同口径资源字段时保留缺失，不补造FEX native bytes／spill。SVM多个外部PC共享一个unit；ready keys、owner数与动态访问次数不可互换。目录slot32B、NativeCode40B，CoreMark该scope若已列出多个ready key不表示复制native owner。

dispatches／cache_hits只在诊断编译模式可用，静态链接绕过这些计数；本轮未另跑全入口census或采样，也不把它们当所有IR B的访问次数。

## 7. 同source历史验证与当前范围

本轮118-file digest与runner均未改变，因此没有重新做coverage trace。此前同source的[唯一覆盖审计](evidence/continuation-reuse-20261105/audit-accepted-candidate/report.md)是历史结构验证：成功指令重复15889→191（−98.80%），全部实际出生87846→50856（−42.11%）；2757个额外入口＋681个root共享681个完整owner，逐entry核对为非空原始source B，非empty/stub。该独立CoreMark20诊断不是本轮重新测得的覆盖率，也不参与时间／cycles分母。唯一覆盖比OLD多123条及剩余191次重复仍保留，未将全部工作量差归因单一优化。

A1的CALL后推测进入INT3、A2的LOCK共享入口／XEND等回退以及原n3的47/48和独立x87七轮复核均已归档。当前实现保留已成功形成source-B入口与stop-ready，取消CALL后推测队列、continuations和FC_INT特例；没有为了缩小编译成本新增ISA预扫、局部checkpoint回滚或overlap框架。历史结果不能与本轮中位数混池，旧拒绝候选也不能充当新锚定的通过证明。

本轮既有Android backend／pipeline＋八输入双scope smoke为18/18；模式／SMC probe为2/2。这些功能检查与性能记录分开，未为此次锚定新增测试矩阵。x87 exit-only、数值FP不自动累计MXCSR sticky／不新增IEEE traps、固定ELF／单执行线程等既定边界保持；不将这些限制藏在‘全benchmark’措辞中。

## 8. 复查入口与关闭回执

统计与完整命令已归档，重新归约原始记录无需再跑设备。`analyze.py`与`render-report.py`记录本次固定路径及subject校验；正式复算应使用对应subject／源快照环境，不能把它们当任意checkout的通用脚本。测量命令、顺序和参数见[run.py](evidence/post-push-benchmark-20261105/run.py)、[commands.json](evidence/post-push-benchmark-20261105/commands.json)、[plan.json](evidence/post-push-benchmark-20261105/plan.json)；全部原stdout／stderr／PMU由record的prefix定位。本报告生成器只读取本目录，不修改源码、Git或设备，不触发新的测试、样本或计时。

| 关闭／身份核对 | 实际结果 |
| --- | --- |
| 测量时HEAD | `390cf73d7d8db5687a8d33cc8be5cdd6aa38638f` |
| 重新计算source digest | `a78fdec20b3de2f00fe47bb1dbfa67c6664c0efa7cd1b7f30658edd8f1ce1e52` |
| manifest文件数 | 118 |
| current源码差异数 | 0 |
| frozen源码差异数 | 0 |
| 自有设备namespace | `/data/local/tmp/swiftvm-post-push-20261105` |
| 删除的自有allocated bytes | 42,435,584 |
| df实际空闲增量bytes | 44,470,272 |
| 自有namespace剩余bytes | 0 |
| 残留active processes /open files /errors | 0 /0 /0 |

allocated删除量与df实际空闲增量是不同观测，不相加成回收量。关闭仅涵盖本轮自有namespace；其他应用／owner未纳入。源closure、部署输入前后身份及cleanup资格由[summary.json](evidence/post-push-benchmark-20261105/summary.json)保留完整真值；详细回执见[phone-cleanup-receipt.json](evidence/post-push-benchmark-20261105/phone-cleanup-receipt.json)。

本轮归档判定为48/48；后续是否继续缩小差距以这份固定锚定为基准。记录有效性、性能门槛与既定支持范围分别陈述，不用其中一项替代另一项。

[本轮完整身份回执](evidence/post-push-benchmark-20261105/receipt.json)、[完整原始输出](evidence/post-push-benchmark-20261105/raw-output.jsonl)。先提交和push实现，再按预登记五轮完成测量；所有旧样本与未过项历史记录继续保留。
