A3 candidate_fail：47/48项性能门通过，当前不接受。全部72 runtime与72 compile记录valid，不混入A2或后续配对复核样本。

沿用旧轮全部8固定项、两个scope的time与user-cycles≤1.10×同轮FEX、去纯byte decode compile≤2.0×门槛；instructions和branch-misses为诊断值。旧轮48/48通过；A3唯一失败为 **x87_bench Block compile=2.108549075×**。func_tests不豁免：其Function time/cycles已恢复通过。

A3源 `a78fdec20b3de2f00fe47bb1dbfa67c6664c0efa7cd1b7f30658edd8f1ce1e52`、runner `13411690d78fa72df6fc84c61004bfe7c36a2060cc3a9a16cb22dcbfca9d77eb`；OLD源 `842e4748e3263f6f50a28c53e1b8bdbb410bde70ae754940610ce43ed7eaed9b`、runner `f7a345a603b1a9c16fd059504a09aeeb76df96f2da676828f723a734323ee548`。guest、固定FEX与byte-probe、simpleperf及配置SHA一致。各原文件SHA、完整三样本、全部exclusive账和资源记录见[JSON](final-ordinary-comparison.json)。[A2 rejected报告](a2-rejected-ordinary-comparison.md)独立保留。

以下均为三次中位数；括号为SVM/同轮FEX比率。CoreMark time是100000-iteration guest loop；其他time和全部PMU是整个子进程，含启动/冷JIT，排除外部FEXServer。无跨case平均分。

**同轮FEX**

| case | time s | cycles G | instructions G | misses M | compile ms |
|---|---:|---:|---:|---:|---:|
| coremark | 15.033000 | 17.300795 | 62.013311 | 19.311517 | 48.025260 |
| int | 0.317518 | 0.282656 | 0.480620 | 0.423109 | 1.592552 |
| fp | 0.264063 | 0.223565 | 0.265174 | 0.426447 | 1.732552 |
| mem | 2.047394 | 2.260723 | 1.805294 | 0.426862 | 1.540000 |
| branch | 0.266538 | 0.228596 | 0.608633 | 0.424465 | 1.531719 |
| call | 0.459178 | 0.447101 | 1.744712 | 0.425614 | 1.640937 |
| func_tests | 0.138475 | 0.070844 | 0.151793 | 0.807961 | 30.286667 |
| x87_bench | 0.819708 | 0.863986 | 2.122587 | 0.718305 | 0.295573 |

**A3 Block**

| case | time s (×) | cycles G (×) | instructions G (×) | misses M (×) | compile ms (×) | gate |
|---|---:|---:|---:|---:|---:|---|
| coremark | 16.111000 (1.072) | 18.518537 (1.070) | 64.363385 (1.038) | 36.122353 (1.871) | 44.905649 (0.935) | PASS |
| int | 0.301029 (0.948) | 0.280198 (0.991) | 0.588387 (1.224) | 0.375332 (0.887) | 1.818940 (1.142) | PASS |
| fp | 0.252263 (0.955) | 0.219635 (0.982) | 0.262570 (0.990) | 0.383993 (0.900) | 2.396197 (1.383) | PASS |
| mem | 2.149787 (1.050) | 2.389788 (1.057) | 2.405851 (1.333) | 0.382379 (0.896) | 2.208391 (1.434) | PASS |
| branch | 0.261421 (0.981) | 0.229436 (1.004) | 0.692428 (1.138) | 0.376608 (0.887) | 1.762604 (1.151) | PASS |
| call | 0.477202 (1.039) | 0.482315 (1.079) | 2.164541 (1.241) | 0.374601 (0.880) | 1.900515 (1.158) | PASS |
| func_tests | 0.108749 (0.785) | 0.057106 (0.806) | 0.122482 (0.807) | 0.589010 (0.729) | 22.387070 (0.739) | PASS |
| x87_bench | 0.555797 (0.678) | 0.571037 (0.661) | 2.701265 (1.273) | 0.514527 (0.716) | 0.623230 (2.109) | **FAIL compile** |

**A3 Function**

| case | time s (×) | cycles G (×) | instructions G (×) | misses M (×) | compile ms (×) | gate |
|---|---:|---:|---:|---:|---:|---|
| coremark | 15.633000 (1.040) | 17.982269 (1.039) | 62.632713 (1.010) | 20.188250 (1.045) | 51.446873 (1.071) | PASS |
| int | 0.303149 (0.955) | 0.280312 (0.992) | 0.608531 (1.266) | 0.375482 (0.887) | 1.797454 (1.129) | PASS |
| fp | 0.250033 (0.947) | 0.220190 (0.985) | 0.266731 (1.006) | 0.384372 (0.901) | 2.430219 (1.403) | PASS |
| mem | 2.167412 (1.059) | 2.410038 (1.066) | 2.466407 (1.366) | 0.380619 (0.892) | 2.210524 (1.435) | PASS |
| branch | 0.259579 (0.974) | 0.230938 (1.010) | 0.704570 (1.158) | 0.377851 (0.890) | 1.920841 (1.254) | PASS |
| call | 0.476507 (1.038) | 0.480148 (1.074) | 2.180710 (1.250) | 0.374733 (0.880) | 1.852456 (1.129) | PASS |
| func_tests | 0.120647 (0.871) | 0.068128 (0.962) | 0.158097 (1.042) | 0.817234 (1.011) | 32.835098 (1.084) | PASS |
| x87_bench | 0.553002 (0.675) | 0.567885 (0.657) | 2.695210 (1.270) | 0.508867 (0.708) | 0.526250 (1.780) | PASS |

**相对OLD的实际变化**（正数为增加；不同轮，非独立hunk因果测量）

| case | B time Δ | Fn time Δ | B cycles Δ | Fn cycles Δ | B compile Δ | Fn compile Δ |
|---|---:|---:|---:|---:|---:|---:|
| coremark | -0.15% | +2.30% | -0.21% | +2.11% | +0.83% | -37.25% |
| int | -0.74% | +0.17% | +0.01% | +0.08% | -1.33% | +2.15% |
| fp | +0.79% | +0.74% | -0.12% | +0.08% | -1.93% | -0.02% |
| mem | -0.09% | -0.60% | -0.08% | -0.58% | +2.98% | +0.13% |
| branch | +1.49% | +0.36% | +0.14% | -0.00% | -8.12% | +8.65% |
| call | -0.31% | -0.03% | -0.07% | -0.08% | -7.59% | -0.17% |
| func_tests | -0.62% | -3.15% | -0.03% | -9.52% | +0.08% | -14.75% |
| x87_bench | -0.30% | -0.58% | -0.13% | -0.57% | +9.88% | -0.54% |

CoreMark Function compile 81.981761→51.446873ms，但time 15.282000→15.633000s仍有实际回退；通过门槛不等于比OLD更快。func_tests Function compile38.516977→32.835098ms、time/cycles相对FEX0.871255/0.961663，16次fallback与OLD相同；A2的48次fallback与cycles失败没有混入本轮。

**CoreMark工作/资源**：同各scope total中位那次20-iteration编译诊断；以下census在各自三次均稳定。

| metric | OLD Block | A3 Block | OLD Function | A3 Function |
|---|---:|---:|---:|---:|
| compile attempts | 2,043 | 2,043 | 705 | 686 |
| Function attempts | 0 | 0 | 700 | 681 |
| Block retries/attempts | 2,043 | 2,043 | 5 | 5 |
| fallbacks | 0 | 0 | 5 | 5 |
| successful Function units | 0 | 0 | 695 | 676 |
| published unit owners | 2,043 | 2,043 | 700 | 681 |
| emitted guest instructions | 12,407 | 12,407 | 32,159 | 16,584 |
| emitted guest bytes | 46,689 | 46,689 | 136,270 | 63,906 |
| record IR N; rejected prefixes excluded | 36,651 | 36,651 | 87,595 | 50,591 |
| record arena words; rejected prefixes excluded | 65,841 | 65,841 | 153,728 | 84,624 |
| native bytes | 389,876 | 389,876 | 646,764 | 393,248 |
| ready PC entries | 2,043 | 2,043 | 700 | 3,438 |
| occupied keys incl pending | 2,605 | 2,605 | 789 | 3,509 |
| directory slots | 32,768 | 32,768 | 8,192 | 32,768 |
| slot element bytes | 1,048,576 | 1,048,576 | 262,144 | 1,048,576 |
| code virtual bytes | 2,097,152 | 2,097,152 | 2,097,152 | 2,097,152 |
| spill slots | 25 | 25 | 42 | 31 |
| max frame bytes | 48 | 48 | 64 | 64 |
| fault sites | 5,075 | 5,075 | 12,729 | 6,762 |
| static links | 2,882 | 2,882 | 1,386 | 953 |
| relocation rows | 3,101 | 3,101 | 1,787 | 1,216 |
| relocation capacity | 4,096 | 4,096 | 2,048 | 2,048 |

Function unit owners700→681（成功Function695→676，加Block retry5→5），ready PC700→3438、slots8192→32768，slot元素256KiB→1MiB；不能把alias数当unit数。native646764→393248B，但peak code virtual仍2MiB，frame/spill见表而非宣称消除。成功IR N/words不包含失败前缀，其未知大小保持null；全部失败和重试仍计入总compile成本，不能将成功N减少当全部出生量减少。短诊断dispatch/probe计数不代表ordinary100000 iterations入口次数。

**Exclusive compile成本**（ms，选total中位样本；分项不各自取中位数）

| cohort / arm (round) | formation+IR | IR opt | allocation+encoding | transfer+publish | release | other | total |
|---|---:|---:|---:|---:|---:|---:|---:|
| old / block (1) | 11.407836 | 9.031221 | 14.533774 | 7.600464 | 0.419687 | 1.542343 | 44.535325 |
| old / function (0) | 28.603763 | 19.576054 | 28.554440 | 4.215056 | 0.435677 | 0.596771 | 81.981761 |
| old / fex (2) | 23.195990 | 6.427865 | 13.852813 | 3.233958 | 0.322760 | 0.716302 | 47.749688 |
| current / block (1) | 11.979617 | 9.088025 | 14.056878 | 7.591213 | 0.422448 | 1.767468 | 44.905649 |
| current / function (1) | 16.862134 | 12.056461 | 16.892473 | 4.685397 | 0.340209 | 0.610199 | 51.446873 |
| current / fex (0) | 22.940313 | 6.240938 | 14.659583 | 3.146823 | 0.351094 | 0.686510 | 48.025260 |

全部8项新旧exclusive分组均在JSON闭合。FEX remaining_ticks精确排除byte_decode子scope；SVM formation减byte decoder，其余原阶段计时，other为非负残差（含fault service等）。SVM native owner release与FEX逻辑工作缓冲释放范围不同。独立compile探针会扰动计时，不从ordinary PMU中扣除诊断compile成本。

**x87 Block失败的有限定位**：两轮total中位都为r2。total567.186→623.230µs，C104.270→206.251µs（+101.981），其余前端/优化/释放主要抵消，净+56.044µs；publication106.511→106.250µs、other16.251→16.717µs，不支持先归到cache BindReady。当前没有更细C子阶段计时，不能把源变化或entries.clear认定为根因，也不因小case直接归为噪声。

| cohort / round | total µs | formation µs | B µs | C µs | publication µs | release µs | other µs |
|---|---:|---:|---:|---:|---:|---:|---:|
| old / 0 | 553.749 | 179.270 | 44.844 | 112.812 | 119.428 | 80.469 | 16.926 |
| old / 1 | 608.701 | 166.200 | 43.385 | 96.614 | 203.436 | 83.020 | 16.046 |
| old / 2 | 567.186 | 170.831 | 70.260 | 104.270 | 106.511 | 99.063 | 16.251 |
| current / 0 | 597.501 | 188.647 | 48.020 | 133.593 | 104.115 | 103.437 | 19.689 |
| current / 1 | 658.179 | 195.004 | 50.312 | 140.051 | 128.699 | 122.552 | 21.561 |
| current / 2 | 623.230 | 166.981 | 45.990 | 206.251 | 106.250 | 81.041 | 16.717 |

x87 Block同中位样本work逐项相同：3attempts/3units，guest30inst/70B，N59/words106，native2364B，spill/frame0，fault17，ready/occupied3、slots32、4alloc/4static/4patched，pending0，relocation4/cap4，code virtual2MiB、slab/dual1，decode30、syscall1、unknown/invalidation/unaligned0。独立[预登记配对复核](x87-paired-confirmation/plan.json)现已完成，全部21次有效，完整数据另列如下，不替换原三轮。

正确性范围不扩大：CoreMark CRC；int/fp/mem/branch/call native checksum；func_tests native stdout+exit101，仍有原性能门；x87仅exit兼容、无本bench数值oracle，ext80/CW/unmask精度由独立pipeline覆盖。没有完整ISA/全部应用保证。

CPU4、policy min/max614400–1171200kHz不变，未锁频；温度原值见JSON。三样本不提供小差异显著性保证；报告实际差异而不作独立机制归因，不按采样份额外推绝对G。A2保留rejected，A3当前candidate_fail、accepted=false。

**x87 Block独立配对复核：完整两组数据，不混n**

原固定套件三轮，compile去byte decode(ms)：

| round | OLD baseline | OLD同轮FEX | A3 candidate | A3同轮FEX |
|---|---:|---:|---:|---:|
| 0 | 0.553749000 | 0.297604167 | 0.597501000 | 0.295572917 |
| 1 | 0.608701000 | 0.293541667 | 0.658179000 | 0.306354167 |
| 2 | 0.567186000 | 0.301562500 | 0.623230000 | 0.293697917 |

独立复核预先固定7轮、baseline/candidate/FEX三臂轮转，无提前停止或有效样本剔除；与上表原三轮完全分开。以下为全部21条(ms)，顺序取实际records：

| round | actual order | baseline | candidate A3 | FEX-byte |
|---|---|---:|---:|---:|
| 0 | B→C→F | 0.708075000 | 0.553332000 | 0.280000000 |
| 1 | C→F→B | 0.827448000 | 0.733070000 | 0.319375000 |
| 2 | F→B→C | 0.557966000 | 0.641043000 | 0.303020833 |
| 3 | B→C→F | 0.641197000 | 0.594011000 | 0.280885417 |
| 4 | C→F→B | 0.646247000 | 0.632346000 | 0.290208333 |
| 5 | F→B→C | 0.563022000 | 0.536773000 | 0.302135417 |
| 6 | B→C→F | 0.530575000 | 0.503699000 | 0.313697917 |
| median, n=7/arm | — | 0.641197000 | 0.594011000 | 0.302135417 |

独立复核的candidate/baseline=0.926409512，candidate/FEX=1.966042269，baseline/FEX=2.122217273。它不支持“本次候选导致x87 Block固有回退”：baseline在这一轮也超过2×，候选中位反而较低。但这不等于证明候选总更快或稳定通过2×，也不能据此把原固定三轮改成48/48。两组完整原始records、顺序、身份、summary及SHA均在JSON，未合成n=10或挑选较好样本。

复用功能实现本轮可收口；普通原三轮仍是47/48、accepted=false。功能输出、固定输入结果与这个有限复核均不授全应用/全ISA或稳定2×资格。
