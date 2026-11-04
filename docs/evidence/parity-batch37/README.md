# Batch37 小型证据索引

本目录逐字摘存本轮唯一冻结证据，不包含二进制、重复源码包或旧批测量。`receipt.json` 列出每份原文件的本地绝对路径、字节数及 SHA-256；新增索引与检查点单独标识。原始测量字节未修改。

| 证据 | 内容 |
| --- | --- |
| [compile-summary.json](compile-summary.json) | 三模式各 3 次 fresh cold compile 的全部原记录；仅扣纯 byte decoder 时间 |
| [runtime-summary.json](runtime-summary.json) | 两 scope 与固定 FEX 各一次有效 100000 迭代；耗时及 whole-child user PMU |
| [measurement-receipt.json](measurement-receipt.json)、[closeout-receipt.json](closeout-receipt.json) | 原综合判定、冻结身份及收尾；`measurement` SHA 保持原值 |
| [inputs.json](inputs.json)、[runner-identity.json](runner-identity.json)、[source-manifest.json](source-manifest.json) | guest、两种 FEX runner、候选 runner 和冻结 80 文件身份 |
| [commands.json](commands.json)、[environment.json](environment.json) | 原设备命令与 FEX 环境；不是新执行记录 |
| [host-batch37-ctest.log](host-batch37-ctest.log)、[backend-test.stdout](backend-test.stdout)、[pipeline-test.stdout](pipeline-test.stdout) | Host 5/5、Android backend 264579 checks、12+4170 pipeline 路径 |
| [native-quality-review.md](native-quality-review.md)、[native-quality-comparison.json](hot-actual/native-quality-comparison.json) | 四 actual unit 的离线 IR/Home/词流比较；明确未绑定运行 RX |
| [phone-cleanup-receipt.json](phone-cleanup-receipt.json) | 本轮 namespace 删除、实际 df 差额、无进程/打开文件/错误 |

另保留本轮 runtime 的原 stdout/stderr、formatter/SMC/EOF 合并场景输出、host/Android build 输出，以及 compile/runtime 前后 policy 的原小文件。policy 四行依次为 min/max/current kHz 与电池温度 0.1°C，含义来自 `commands.json`；范围 614400–1171200 kHz，采集时 current 1171200、温度 25.4°C，不声称锁频。runtime FEX 的 affinity warning 原样保留；原摘要分别核实实际 child CPU allowed 为 4。

`runtime-summary.json` 中 `goal_achieved:false` 是原 runtime-only 解析器的历史常量占位，未被修改。真实综合门槛按 `measurement-receipt.json` 的每 scope 数值及布尔项判断。短 20 迭代仅供 cold compile/CRC，不作有效 CoreMark 分数；长运行三模式均 exit0、CRC d340、有足够有效时长。

原证据根目录（本机定位，不是仓库相对链接）：
`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-parity-implementation-20261103/batch37`。
仓库中的一页结论见 [检查点](../../codegen-parity-checkpoint-2026-11-04.md)。
