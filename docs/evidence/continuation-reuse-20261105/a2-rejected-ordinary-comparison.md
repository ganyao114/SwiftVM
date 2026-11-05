A2 rejected：8 项功能输出均有效，旧性能门槛仍有 1 项失败，因此本轮不接受；这不是未来 A3 的 final comparison。

新旧各有 72 条 runtime、72 条 compile 记录，全部 valid。旧门槛为各 case/各 scope 的 time、user-cycles≤1.10×同轮 FEX，去纯 byte decode 的 compile≤2.0×；旧轮48/48通过，A2为47/48。唯一失败是 **func_tests Function user-cycles=1.367144×**（旧1.068562×）。指令数与分支失误为诊断项，不临时新增或删除门槛。

旧源 `842e4748e3263f6f50a28c53e1b8bdbb410bde70ae754940610ce43ed7eaed9b`，runner `f7a345a603b1a9c16fd059504a09aeeb76df96f2da676828f723a734323ee548`；A2源 `8806171c19c9bbce1c60459d50dcfea80d6e262a21af4f7d710d3c34056a0ee8`，runner `79ccf232b02a4383a24ffa91c8f49ea02816aed71c860c7e0c4e9fe2ee4e10b6`。guest、ordinary FEX、byte-probe、simpleperf 和配置 SHA 全部一致。身份、原记录 SHA 和全部三次观测见[配套JSON](a2-rejected-ordinary-comparison.json)；[拒绝收据](attempt2/rejection-receipt.json)与原始记录均保持原样。

以下全部为三次中位数，括号为相对同轮 FEX 的比率。CoreMark time 使用100000 iterations 的 guest loop；其余 time 与全部 PMU 为整个子进程（含启动/冷JIT，排除外部FEXServer）。func_tests 仍按原门槛验收，不能因其功能测试性质而豁免。

**同轮 FEX 参照**

| case | time s | cycles G | instructions G | misses M | compile ms |
|---|---:|---:|---:|---:|---:|
| coremark | 15.072000 | 17.346615 | 62.013275 | 19.166196 | 46.894844 |
| int | 0.315286 | 0.282371 | 0.480618 | 0.424476 | 1.488906 |
| fp | 0.262564 | 0.223403 | 0.265174 | 0.428600 | 1.658385 |
| mem | 2.047452 | 2.260762 | 1.805293 | 0.428148 | 1.594583 |
| branch | 0.268569 | 0.228808 | 0.608631 | 0.424367 | 1.513437 |
| call | 0.458899 | 0.445577 | 1.744712 | 0.425851 | 1.579583 |
| func_tests | 0.139742 | 0.070572 | 0.151796 | 0.807374 | 30.345990 |
| x87_bench | 0.858774 | 0.901666 | 2.122587 | 0.671876 | 0.286406 |

**A2 Block**

| case | time s (×) | cycles G (×) | instructions G (×) | misses M (×) | compile ms (×) | gate |
|---|---:|---:|---:|---:|---:|---|
| coremark | 16.140000 (1.071) | 18.555022 (1.070) | 64.363443 (1.038) | 35.968102 (1.877) | 45.602955 (0.972) | PASS |
| int | 0.301281 (0.956) | 0.280104 (0.992) | 0.588388 (1.224) | 0.375091 (0.884) | 1.909994 (1.283) | PASS |
| fp | 0.248048 (0.945) | 0.219474 (0.982) | 0.262571 (0.990) | 0.383434 (0.895) | 2.369594 (1.429) | PASS |
| mem | 2.163703 (1.057) | 2.406200 (1.064) | 2.405852 (1.333) | 0.380610 (0.889) | 2.085464 (1.308) | PASS |
| branch | 0.259024 (0.964) | 0.229351 (1.002) | 0.692428 (1.138) | 0.376180 (0.886) | 1.745879 (1.154) | PASS |
| call | 0.479821 (1.046) | 0.482507 (1.083) | 2.164541 (1.241) | 0.373967 (0.878) | 1.883997 (1.193) | PASS |
| func_tests | 0.109967 (0.787) | 0.057212 (0.811) | 0.122484 (0.807) | 0.590056 (0.731) | 22.607643 (0.745) | PASS |
| x87_bench | 0.557096 (0.649) | 0.571906 (0.634) | 2.701264 (1.273) | 0.507628 (0.756) | 0.539222 (1.883) | PASS |

**A2 Function**

| case | time s (×) | cycles G (×) | instructions G (×) | misses M (×) | compile ms (×) | gate |
|---|---:|---:|---:|---:|---:|---|
| coremark | 15.735000 (1.044) | 18.099132 (1.043) | 62.798053 (1.013) | 21.245486 (1.108) | 58.224757 (1.242) | PASS |
| int | 0.302122 (0.958) | 0.280129 (0.992) | 0.608534 (1.266) | 0.376842 (0.888) | 2.004060 (1.346) | PASS |
| fp | 0.249080 (0.949) | 0.219979 (0.985) | 0.266735 (1.006) | 0.383785 (0.895) | 2.544958 (1.535) | PASS |
| mem | 2.171722 (1.061) | 2.414165 (1.068) | 2.466414 (1.366) | 0.383747 (0.896) | 2.152653 (1.350) | PASS |
| branch | 0.258933 (0.964) | 0.230987 (1.010) | 0.704573 (1.158) | 0.377649 (0.890) | 1.783330 (1.178) | PASS |
| call | 0.477296 (1.040) | 0.480165 (1.078) | 2.180711 (1.250) | 0.376283 (0.884) | 1.951767 (1.236) | PASS |
| func_tests | 0.147539 (1.056) | 0.096482 (1.367) | 0.237163 (1.562) | 1.111957 (1.377) | 54.583725 (1.799) | **FAIL cycles** |
| x87_bench | 0.556293 (0.648) | 0.572113 (0.635) | 2.695213 (1.270) | 0.511184 (0.761) | 0.496766 (1.734) | PASS |

**相对 OLD 的实际变化**（正数为增加；不同轮，不是独立 hunk 归因）

| case | B time Δ | Fn time Δ | B cycles Δ | Fn cycles Δ | B compile Δ | Fn compile Δ |
|---|---:|---:|---:|---:|---:|---:|
| coremark | +0.03% | +2.96% | -0.02% | +2.78% | +2.40% | -28.98% |
| int | -0.65% | -0.17% | -0.02% | +0.01% | +3.61% | +13.89% |
| fp | -0.89% | +0.36% | -0.19% | -0.01% | -3.02% | +4.70% |
| mem | +0.56% | -0.40% | +0.61% | -0.41% | -2.76% | -2.49% |
| branch | +0.56% | +0.11% | +0.10% | +0.02% | -8.99% | +0.87% |
| call | +0.24% | +0.14% | -0.03% | -0.07% | -8.39% | +5.18% |
| func_tests | +0.49% | +18.43% | +0.16% | +28.13% | +1.06% | +41.71% |
| x87_bench | -0.07% | +0.02% | +0.02% | +0.17% | -4.93% | -6.11% |

func_tests Function 的 whole-child time 与 user-cycles 均实际回退，compile中位38.516977→54.583725ms。冷JIT包含在该短进程测量中；诊断compile增加与回退相符，但来自不同进程，不能从ordinary cycles精确扣除，也不能撤掉门槛。该scope的attempts404→278、Function attempts388→230，而Block retries/fallbacks16→48；FEX与SVM的native stdout/exit101仍正确。拒绝收据记录48次失败中的30次合法LOCK/shared-tail形成冲突和18次其他Unsupported，不把失败算成0成本。

**CoreMark 实际工作/资源**：取各自compile total居中的同一个20-iteration诊断样本；以下census在本轮各三次均稳定。

| metric | OLD Block | A2 Block | OLD Function | A2 Function |
|---|---:|---:|---:|---:|
| compile attempts | 2,043 | 2,043 | 705 | 281 |
| Function attempts | 0 | 0 | 700 | 268 |
| Block retries/attempts | 2,043 | 2,043 | 5 | 13 |
| fallbacks | 0 | 0 | 5 | 13 |
| successful Function units | 0 | 0 | 695 | 255 |
| published unit owners | 2,043 | 2,043 | 700 | 268 |
| emitted guest instructions | 12,407 | 12,407 | 32,159 | 19,735 |
| emitted guest bytes | 46,689 | 46,689 | 136,270 | 76,586 |
| record IR N (reject prefixes excluded) | 36,651 | 36,651 | 87,595 | 59,022 |
| record arena words (reject prefixes excluded) | 65,841 | 65,841 | 153,728 | 97,848 |
| native bytes | 389,876 | 389,876 | 646,764 | 442,184 |
| ready PC entries | 2,043 | 2,043 | 700 | 4,258 |
| occupied keys incl pending | 2,605 | 2,605 | 789 | 4,439 |
| directory slots | 32,768 | 32,768 | 8,192 | 65,536 |
| slot element bytes | 1,048,576 | 1,048,576 | 262,144 | 2,097,152 |
| code virtual bytes | 2,097,152 | 2,097,152 | 2,097,152 | 2,097,152 |
| spill slots | 25 | 25 | 42 | 31 |
| max frame bytes | 48 | 48 | 64 | 64 |
| fault sites | 5,075 | 5,075 | 12,729 | 7,752 |
| static links | 2,882 | 2,882 | 1,386 | 1,125 |
| relocation rows | 3,101 | 3,101 | 1,787 | 1,399 |
| relocation capacity | 4,096 | 4,096 | 2,048 | 2,048 |

Function owners700→268（成功Function695→255，加Block fallback5→13），但ready PC700→4258，目录8192→65536槽，即slot元素空间256KiB→2MiB；不能把alias数当编译unit，也不能只报native bytes降低。peak code virtual仍2MiB、max frame仍64B，spill42→31未消除。N/words不包含失败IR前缀，缺失大小保持未知；时间包含这些失败与重试，不能将成功N下降当作全部前端出生量下降。短诊断dispatch计数不代表100000-iteration ordinary入口次数。

**Exclusive compile 成本**（ms，选各case/arm的total中位那次，分项不另取中位数）。全部8项的完整新旧闭合分组、样本round/prefix与FEX精确tick账在JSON。以下列CoreMark的直接比较：

| cohort / arm (round) | formation+IR | IR opt | allocation+encoding | transfer+publish | release | other | total |
|---|---:|---:|---:|---:|---:|---:|---:|
| old / block (1) | 11.407836 | 9.031221 | 14.533774 | 7.600464 | 0.419687 | 1.542343 | 44.535325 |
| old / function (0) | 28.603763 | 19.576054 | 28.554440 | 4.215056 | 0.435677 | 0.596771 | 81.981761 |
| old / fex (2) | 23.195990 | 6.427865 | 13.852813 | 3.233958 | 0.322760 | 0.716302 | 47.749688 |
| current / block (0) | 11.501096 | 9.214506 | 14.872854 | 7.924660 | 0.536042 | 1.553797 | 45.602955 |
| current / function (0) | 19.678330 | 13.009490 | 19.404583 | 5.432711 | 0.404375 | 0.295268 | 58.224757 |
| current / fex (0) | 22.522760 | 6.094583 | 14.256927 | 3.043646 | 0.311875 | 0.665052 | 46.894844 |

FEX用嵌套scope的remaining_ticks排除纯ByteDecode，独占ticks精确闭合；SVM formation减compile_decode，其余为原阶段计时，other为非负实测残差（含fault service/请求与目录等未单列工作）。SVM native-owner释放与FEX logical working-buffer释放范围不同，不混作等价阶段；两方compile探针都不是ordinary runtime。Function前端/优化/后端成本减少，但publication4.215056→5.432711ms增加；整体compile收益不等于运行质量收益。

正确性范围保持原样：CoreMark校验CRC；int/fp/mem/branch/call有native checksum；func_tests为native stdout+exit101；x87仅exit compatibility，无本bench数值oracle。已有ext80/CW/unmask pipeline验证不替代该bench数值输出，也不放宽其既有性能门槛。

CPU4与policy min/max614400–1171200kHz不变，未锁频；temperature raw旧256→258、新258→260。三样本与轻微跨轮环境差异不足以把小变化说成统计显著，不按采样份额外推退休指令归因，不计算跨workload平均分。A3尚无本报告数据；A2保持rejected。
