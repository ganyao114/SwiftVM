# Runtime 架构实施验收

本目录存放固定测量脚本、身份与精简结果；完整原始输出保留在：

`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/runtime-architecture-20261105`

实施工作树为 `/Volumes/Data2/SwiftVM-submit-20261104`，基于 `74ac9bc9` 的 WIP。冻结旧目录 `/Volumes/Data2/SwiftVM` 只提供历史源码和固定 guest 输入，没有修改。最终源码清单与压缩快照按 SHA256 保存，编译产物另记身份。

## 固定口径

- 红魔 NX729J，ADB `bf1359ff`，CPU4／`taskset 10`，原 min/max 614400/1171200 kHz；不改手机频率策略。
- 重构前固定 runner、最终 runner 各测 Block／Function，加固定 FEX，共五臂。
- 八个输入，每臂五次；runtime 200 次、compile 另起进程 200 次，固定轮换顺序，全部保留。
- CoreMark runtime 用 100000 iterations／CRC d340，guest loop 至少10秒；其他 runtime 是包含启动及 JIT 的 whole-child simpleperf 时间。user PMU 含当前子进程冷 JIT，FEXServer 不在目标树内。
- compile 用 CoreMark20／CRC4983；仅扣真实 byte parser 区间，计入适配、形成、IR、失败／回退、优化、分配、编码、发布与 native 释放。沿用既有 FEX arena 最终析构不完全对称的限定。
- 最终 Release 启用 IPO（NDK ThinLTO）；重构前 runner 保留原无 IPO 构建。byte parser 和 ext80 helper 闭包没有改为 IPO。结果体现正式构建整体成本，不将其归因为减少了 IR pass。
- x87 benchmark 沿用 exit-only oracle；不得写成完整数值 ISA 认证。可写 microbenchmark ELF 原样保留，Requested Function 仍是实际单 source-block 的 SMC 路径。

## 运行

使用 Data2 上三个固定构建缓存，分别为 Release host、Release NDK29 arm64-v8a/android28 和 Debug ASan+UBSan。Release 默认 `SVM_ENABLE_IPO=ON`；Debug 不开 IPO。构建设置随结果保存为 `*-CMakeCache.txt`。

`run.py prepare` 创建独占手机目录并验证输入／源码身份；之后依次执行 `smoke`、`mode-probe`、`smc-probe`、`compile`、`direct-smoke`、`runtime`，最后 `analyze.py` 和 `run.py cleanup`。每项只操作 `/data/local/tmp/swiftvm-runtime-architecture-20261105` 及其独占 FEX 环境。路径与源身份被刻意固定，重测其他版本需另建测量身份，不能覆盖本轮样本。

`smoke` 检查后端、native pipeline、直接语义、实际 engine，以及八项应用两份 runner 的两个 scope。`direct-smoke` 只跑 CoreMark20 和功能集，补直接引擎的端到端 oracle；不对解释器重复全部长时间吞吐循环。页边界、CALL/RET/POP/REP、atomic-before-access、CET、FP 环境与 x87 状态由定点合同覆盖。

## 保留的过程版本

完整原始证据目录下分别保留以下版本，均不与最终样本混合：

- `functional-attempt-a3f641a148c1`：移植中的 RMIF mask 错误，修复后重验，无性能样本。
- `functional-pre-owned-record-2d192706bdb7`：行为通过，但语义仍借 vendor vocabulary，已退役，无性能样本。
- `compile-regression-ded140b16097`：首轮200个 compile 样本，大案例增加6–10%，已保留 runner 与采样。
- `compile-call-boundaries-2f1dee3562af`：hot inline 后200个 compile 样本仍有约7–8%开销；因此继续验证 Release IPO，保留该版本 runner／源码快照。
- 单次 `ipo-preflight` 只用于构建配置实验，不进入最终中位数或门槛。

Android 内核拒绝 `/dev/zero` RX 映射（EACCES），所以实际设备上无法运行 Volatile RX 分支。测试明确报告不可用；该分支的源码准入审查、上层 Unsupported-before-access 合同与匿名／普通文件实际路径分别记录，不能声称整条设备代码执行链已动态覆盖。
