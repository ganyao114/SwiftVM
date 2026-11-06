# x87 Block compile：2026-11-06

[结果与完整合同](../../x87-block-compile-optimization-2026-11-06.md)。同批基线为上一轮生成质量优化快照，Git基底ebf55740；当前为新WIP源码digest `9a37021cd0cde4d04fa1bfaad358e44bbd99f3877d54f22b741660a2a8c662c0`，runner `e579c6fbdadad2902e59d0df447b03c2e8c8774f4d6de3edf1bba1f8f42953b1`。

- `summary.json`：完整五arm结果、门槛与绑定审计；`comparisons.json`：基线与当前变化。
- `compile-records.json`／`runtime-records.json`：全部200+200原始记录；`compile-breakdown.json`：每个实际样本的互斥分项，及与总量中位数匹配的单样本账本。
- `quality-comparison.json`／`resources.json`：实际IR、分配、发射和owner工作量，保留所有取值。
- `plan.json`／`identity.json`／`source-manifest.json`／`inputs.json`：冻结测量与输入身份；`plan.runner_sha256`是脚本指纹，`candidate_runner_sha256`是生产二进制指纹。
- `development-checkpoint-status.json`：保留前一compile-only开发点及未过线事实，不与最终样本混池。
- `phone-cleanup-receipt.json`及`functional-*`：测试和独立手机namespace清理证明。

原始命令、每次PMU/stdout/stderr、输入压缩包、源码archive和patch在Data2：`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/x87-block-compile-final-20261106`；前一开发点在同目录的`x87-block-compile-optimization-20261106`。二进制与大结果没有放入Git工作树。x87 workload仅有exit oracle；SVM完整native owner释放与FEX旧probe逻辑释放的边界差异保留，不能据此宣告严格两倍目标已完成。
