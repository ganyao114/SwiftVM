# AOT／磁盘缓存验证：2026-11-07

实现说明见 [AOT／缓存合同](../../aot-code-cache-2026-11-07.md)。基线为已提交的 `f9e8e5bd5887325143113f92f502efa15f55197c`，本轮为其上的工作区改动。[源码身份](source-inputs.json)记录实际文件摘要，[构建身份](build-identities.json)记录三种构建的 native-code key。

## 功能验证

| 环境 | 实测结果 |
| --- | --- |
| macOS ARM64 | 16 项有效 portable contracts 全过，7 项 Linux native contracts 按 ISA／平台跳过；serializer 修复后再聚焦跑 3 项持久化合同，全过 |
| 红魔 NX729J，`bf1359ff` | 32 项记录检查通过，包含 12 次 Catch 进程；另外通过两 scope 的 compile／info／6 次命名 call CLI |
| RV64G，真实 Linux 6.12.113，QEMU 两 hart，ASLR 开启 | 完整首轮 25 步、39 Catch cases 全过；最终 key 对影响范围聚焦复核 18 步、7 native cases，全过 |

红魔覆盖 Block／Function、JIT／Mixed 的独立新进程缓存写入／读取，暖进程 `compile_attempts=0`、`disk_cache_hits>0`、`interpreted_steps=0`。完整静态 glibc 函数测试两 scope 均保持输出和退出码 101，暖缓存无需编译。测试还覆盖源码 policy mismatch、真实 SMC、native fault、损坏缓存回退及损坏／错误 scope 的显式 AOT 拒绝。

AOT 删除原 ELF 后由另一个宿主可执行文件 `svm_rebuild_linux` 运行；两 scope 均退出 42，加载现成 native units，无 JIT 或解释。独立 CLI `sum7=28`、IFUNC `choose=9`、CPUID `cpu=7`。部分覆盖产物实际加载 4 个 FP-domain units，随后只对未覆盖 continuation 发起 1 次 JIT，正常退出 42。

命名 API 的 native cases 覆盖 7 个整数参数及栈传参、FP、mixed／memory aggregate、varargs、typed GuestPtr、global 持久化、IFUNC、CPUID、guest fault 后恢复，以及预编译 startup stop。另对真实 glibc guest 做 `patch main → compile → 删除两份 source → startup → strlen`：加载 20,828 个 units，startup／调用补充 31 次 JIT，无解释执行，正确返回 ELF header 第一处 NUL 的偏移 **8**。该额外 harness 最初将期望写成 7；已直接用原始 ELF 和捕获输出修正 oracle，没有重跑 guest 来制造通过结果，说明保留在 [原始摘要](glibc-summary.json)。

RV64 的最终摘要在 [rv-final-summary.json](rv-final-summary.json)。首轮构建 key 与最终 key 不同：中间修复了 ARM64 Dynamic LinkSite 使用 `NoNativeOffset` 的合法 branch 字段，RV64 该合同不变；保留两轮各自源码／二进制身份，没有把首轮数据冒充最终全量重跑。

## 默认 JIT 的 compile 回归

红魔 CPU4，`taskset 10`，CoreMark 20 iterations，CRC `0x4983`；基线／本轮交替执行，各 3 个新进程。采样前后 CPU4 上限均为 **1,171,200 kHz**。默认关闭缓存与 AOT，以下为同口径 `compile_ns` 的中位数，包含 decode，不构成新的完整 FEX 或正式 CoreMark 吞吐比较。

| Scope | 基线 ms | 本轮 ms | 变化 |
| --- | ---: | ---: | ---: |
| Block | 41.3900 | 41.6206 | +0.6% |
| Function | 50.4686 | 51.6165 | +2.3% |

Function native bytes 均为 378,008。Block 基线出现 370,384／370,696 两种实际输出，本轮为 370,384；没有把该 fresh-process 工作量差异表述为逐字节一致。结果足以排除明显 compile 劣化，不支持从 3 轮小样本推导微小速度差异。

宿主构建／选码微基准每组 15 个样本保持相同工作量和生成形态：scalar Block +1.7%、scalar Function +0.6%、自动 Function region −1.2%、已解码 Function −2.7%。详见 [手机采样](phone-compile-summary.json)、[宿主采样](host-compile-summary.json) 与 [二进制身份](phone-compile-inputs.json)。

## 缓存成本的实际边界

以下为完整函数 guest 的单次功能检查值，用来说明成本归属，不作为稳定性能分数。

| Scope | 冷 cache compile ms | 冷 cache 写回 ms | 暖 cache compile／加载发布 ms | 暖 cache 编译次数 |
| --- | ---: | ---: | ---: | ---: |
| Block | 38.186 | 26.698 | 23.990 | 0 |
| Function | 51.312 | 37.051 | 26.881 | 0 |

`compile_ns` 包含读取、hash、重绑定与发布；`cache_write_ns` 单列最终 fsync／原子写回，首次持久化总预算需相加。暖缓存的 0 次编译仍有文件摘要、source span 核验、映射、链接和回收成本。当前不宣称 mmap 零拷贝加载、PIE 跨 load-bias 复用或 0 成本 AOT。

## 证据与回收

完整 raw stdout／stderr、命令、辅助 harness、产物和 RV console 均位于 `/Volumes/Data2/swiftvm-middle-results-relocated-20261025/aot-cache-20261107`。仓库只保留少量摘要、输入摘要与合同日志。

三组红魔命名空间已分别检查无活跃进程、无打开文件后回收，剩余 0 bytes；RV 自有 QEMU 均退出，临时 initrd 已删除。固定构建缓存继续复用 Data2，未修改冻结的 `/Volumes/Data2/SwiftVM` 或其他项目。清理凭据见 [功能测试](phone-cleanup-receipt.json)、[compile 采样](phone-compile-cleanup-receipt.json) 和 [glibc 调用](glibc-cleanup-receipt.json)。
