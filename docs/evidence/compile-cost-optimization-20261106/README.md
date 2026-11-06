# Compile 成本优化验收

具体实现、完整表格、阶段口径与剩余问题见 [2026-11-06 记录](../../compile-cost-optimization-2026-11-06.md)。

本目录只保存小型汇总及相对上一已测指令扩展版本的真实21文件补丁；原始200样本、独立微基准、最终runner与282项实际源码依赖压缩包保存在：

`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/compile-cost-optimization-20261105`

x87 Block 原完整口径仍为2.25×FEX，两倍目标没有关闭。Call Function本批总耗时+5.5%也未被从表中排除。源码未提交/push，手机独立namespace已清理。
