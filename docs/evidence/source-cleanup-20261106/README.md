# IR 优化职责拆分与源码整理验证

验证对象是以 `a13b68c5` 为基线的本次完整源码快照。格式化与结构调整分开提交；`source-manifest.json` 记录全部 155 个活动 C++ 文件的 SHA-256，原始测量记录保持当时的 WIP 身份。

- `verification-receipt.json`：格式检查 155/155、主机 CTest 9/9、红魔手机回归 6/6，以及此次整理保留的优化阶段顺序。
- `phone-tests.json`、`host-ctest.log`：现有回归结果；未为格式化重复增加测试。
- `plan.json`、`inputs.json`：固定三轮的小规模 compile 对照范围与二进制身份。基线是已提交源码对应的 Android runner，SHA-256 为 `7b1799033060b8f00ecc6b437b363c26ee30a94f04f7fea68976fa549be5f86a`。
- `compile-records.json`：2 个 workload × Block/Function × 基线/候选 × 3 轮，共 24 个完整样本；只扣除实际 byte parser，保留所有样本。
- `compile-comparison.json`：本批中位数约增加 1.0%～2.8%。这只是本次整理的回归检查，不是新一轮 FEX 差距锚定，也不证明稳定的最坏耗时上界。

CoreMark Block 候选有一轮实际编译 2049 个 unit，另外两轮及基线三轮均为 2053；相关 IR/native 总量差异未计作优化收益。其余三组工作量一致，各组 spill/frame 统计均保持一致。

`phone-cleanup-receipt.json` 确认本轮设备临时目录已释放 145264640 字节，剩余 0，无活跃进程／打开文件或清理错误。大二进制未纳入本目录；原始执行日志及固定构建缓存留在 Data2。
