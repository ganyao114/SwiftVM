# Function compile 对照证据 2026-11-06

[完整结果与边界](../../function-compile-optimization-2026-11-06.md)。主结构比较为SVM Function对FEX多基本块CompileBlock，Block单列。基线9a37021c/e579c6fb，当前源码`166aeaed5d886c5da6d7c23eb3da663a4bfa788fc0957c884ae016ef943cc959`／runner`7b1799033060b8f00ecc6b437b363c26ee30a94f04f7fea68976fa549be5f86a`，Git基底ebf55740，明确WIP。

- summary/comparisons：固定全部样本的结果与门槛；compile-breakdown保同一个总量中位数样本分项和每个原样本。
- compile-records/runtime-records：全部200+200原始记录；x87-full-distribution另列中位数、均值、范围，保留高值。
- quality-comparison/resources/optimizer-work：生成工作量、实际末扫省略和复制转发次数；旧版没有新计数，未伪造旧值。
- plan/identity/source-manifest/inputs：源与部署身份；plan.runner_sha256为脚本，candidate_runner_sha256为生产ELF。
- development-checkpoint-status/intrusive-diagnostic-summary：保留未接受的第一点及侵入诊断边界，不与正式成绩混池。
- phone-cleanup及functional/diagnostic回执：各自namespace清理；测试日志与输入身份单列。

完整命令、PMU/stdout/stderr、二进制和源码压缩包、patch在Data2：`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/function-compile-final-20261106`。当前通过的是本批Function中位数门槛，均值/高值、Block未过项和0.5ms工程目标的差距均保留。

整理后代码提交为 `f3f236a5beb6b76581a3c81c028612c6fb436690`；[`submission.json`](submission.json)逐项核对最终已提交源码与被测快照。此前各阶段的WIP清单和二进制身份保留原样。
