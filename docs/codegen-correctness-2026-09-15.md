# 2026-09-15 后端正确性收敛

## 2026-10-06 review 后续修复

针对 `42a8ce2` 之后的 review，继续修复三个实现问题和两个性能缺口：

- low32 反向 recolor：原检查从 bridge 开始，遗漏父值定义到 bridge 之间新占用的寄存器区间。分配器现在检查这段前缀中的固定/helper/显式 GPR 破坏；发码复核也覆盖前缀，包含相邻扩展的快速路径。定向用例保留无破坏时的合并，并拒绝将父值迁入原子操作使用的临时寄存器。
- 已发布函数入口：split 前和空块入队前均检查已有代码，不再把 accepted 标记当作所有权证明。只有实际待解码的本地入口才触发 block-cap；回归覆盖 lazy/eager 下已经排入 HIR 的已发布入口，读取该入口会直接使测试失败。
- 相对文件路径：`getcwd` 返回与 `AT_FDCWD` 文件操作相同的宿主当前目录，保留 Linux 返回长度包含 NUL、`ERANGE` 和 `EFAULT` 语义。两个 guest ISA 都有实际创建相对文件的测试。
- POPCNT/CRC32：新增专用、无 helper 的 IR。POPCNT 使用 `FMOV/CNT/ADDV/UMOV`；CRC32 使用 OS 检出的 ARM64 CRC-32C 指令，缺少能力时使用不破坏 NZCV 的紧凑内联循环。回归还发现并修复了 distorm 表遗漏 CRC32 16 位源的 operand-size 支持。覆盖源宽度、输入/输出别名、保留输入、待发布 NZCV、JIT/解释器一致性以及生成代码中没有 `BLR`。原先借 POPCNT 触发 helper 的回归改用仍调用 helper 的 LZCNT。原有 resident helper 的 NZCV 用例补上 L2 入口发布和非零基线断言，避免未执行代码也通过。
- 冷编译：配置锁改为共享读锁；有界 region 以请求 PC 为中心保护 32 KiB 半径，互不重叠的请求可并行。lazy 解码及 block 回退在 8 KiB 后的指令边界退出；两个 region 的 regroup 范围包含在保护半径内。重叠请求、相同 PC 和无界 eager/AOT/解释器函数仍互斥。共享 fallback 状态、region membership、统计、slab 空闲链和代码缓存分配均有同步保护；编译观察者需支持并发回调。

`translate_ns` 现在包含等锁时间；新增 `translate_wait_ns` 和 PROF2 的 `translate_wait`。多线程计数是线程时间之和，不能用 `wall - translate` 推导执行时间。JIT 磁盘缓存版本提高到 22，避免使用修复前的发码。

本轮构建与测试串行执行，原生验证结果如下；下节的 449 个用例和 Linux 绝对路径结果属于此前提交。

| 验证 | 结果 | 日志 |
| --- | --- | --- |
| macOS ARM64 Release 默认全量 | 459 个用例、1,126,694 条断言通过 | `/tmp/swiftvm-reviewfix-full-default-20261006.log` |
| flags 回退：`SVM_FLAGS_REGS=0 SVM_FLAGS_CFINV=0 SVM_TSO_MODE=acqrel` | 25 个相关用例、1,929 条断言通过 | `/tmp/swiftvm-reviewfix-fallback-final-20261006.log` |
| pin3/fixed：`SVM_X86_PIN_EXT=3 SVM_RA_FIXED_CLASS=1 SVM_TSO_MODE=acqrel` | 25 个相关用例、1,929 条断言通过 | `/tmp/swiftvm-reviewfix-pin3-final-20261006.log` |
| SMC 开/关的冷编译与 slab 并发，开启 PROF2 和函数统计 | 3 个用例、240 条断言通过；等待计数非零，包含在总翻译时间内 | `/tmp/swiftvm-reviewfix-concurrency-prof-20261006.log` |
| 原始 low32 错误样例重新发码与 VIXL 执行 | 父值保留在 x6；对齐/非对齐原子操作之后均读到 `0x12345678`；已发布入口读取次数为 0 | `/tmp/swiftvm-reviewfix-probe-result-20261006.log` |
| 原生 AOT 目标 | `ninja -C cmake-build-release -j1 svm_aot` 构建通过 | `/tmp/swiftvm-reviewfix-aot-build-20261006.log` |

### 本轮 Linux 相对路径复核

Orb Ubuntu Release 启动器串行重建通过，日志为 `/tmp/swiftvm-reviewfix-linux-build-20261006.log`。随后串行执行 SQLite、CoreMark、smallpt 的 FEX、SwiftVM 直接映射和窗口映射，共 9 次运行，全部返回 0，输出与参考一致。FEX 使用本机 `/usr/bin/FEXInterpreter.f2e35f3`，不是对最新版本的比较；SwiftVM 使用上述重建的启动器。

三个引擎均固定 CPU 0、关闭磁盘代码缓存，SwiftVM 使用 `SVM_TSO_MODE=acqrel`。SQLite 输入为 `--size 1 --testset main --verify --output verify.data test.db`，数据库和验证输出都使用相对路径；CoreMark 输入为 `0x0 0x0 0x66 1000 7 1 2000`；smallpt 输入为 `4 8 6`。每次运行使用独立目录，30 秒超时，单文件上限 32 MiB。

| 负载 | 三个引擎一致的结果 |
| --- | --- |
| SQLite | Verification Hash 为 `111130 1e792c9db61996c477b8ab5ce2d690052e8dae74824a430a`；111,130 字节验证输出逐字节一致，SHA-256 为 `6db359a50bc1c3f9606b73a1dd92f29a814781c3e0be612b361d241e69969481` |
| CoreMark | 五项 CRC 为 `e9f5/e714/1fd7/8e3a/d340` |
| smallpt | 完整 8×6 PPM 逐字节一致，SHA-256 为 `e32ee42312d3fd9f643c6aa26f735254a4b6b6bc1111901c0193a69bf162ff5e` |

汇总日志为 `/tmp/swiftvm-reviewfix-linux-relative-20261006.log`；VM 中 `/tmp/swiftvm-linux-validation-reviewfix-20261006/{case}/{engine}/result.json` 保留启动器及客体文件摘要、完整命令、环境、返回码和输出位置。这批短运行验证正确性，不作为性能成绩或 Linux 多线程压力验证。

以上证明修复行为和生成代码形态，没有建立整体性能提升百分比。无界 eager 编译及相距不足保护范围的冷入口仍串行。

## 2026-10-06 续修结果

从工作区 Codex 历史恢复的最后停点是 low32 回归 SIGSEGV，以及尚未完成的 SQLite 验证。本次在保留已有改动的基础上完成续修，修复基于 `7d53d9e`，经授权整理为提交 `42a8ce2`。

修复了三个问题：

- 空 uniform 配置：`AddressSpace` 现在始终创建有效的 `UniformInfo`。没有 uniform buffer 时保留空映射，避免 `ReproveAdjacentLow32Copy` 经 `IsUniform` 读取空对象。原有同寄存器扩展回归恢复通过，并继续检查清除高 32 位所需的 W 写入。
- low32 所有权转交：延长父寄存器的使用区间时，必须检查整个区间中的 `SetHostGPR`、固定寄存器破坏和 helper 调用破坏。原实现只检查 XCHG 屏障，可能让后续消费者读到被重新发布的客体寄存器值。分配与发码复核均补上检查；新回归同时验证覆盖时拒绝合并、未覆盖时保留合并。
- 函数解码不收敛：外部入口候选不满足当前函数的解码条件时，原循环仍反复加入该候选，且不增加已解码块数。入队前现在过滤非本地入口及已有代码的入口。新回归用有界回调捕获旧实现的循环，并验证两类边界均不会被吸收。

### 最新原生验证

构建和测试均串行执行：

```sh
ninja -C cmake-build-release -j1 swift_test
cmake-build-release/source/tests/swift_test --reporter compact
```

macOS ARM64 Release 默认全量测试：**449 个用例、1,124,874 条断言全部通过**。日志：`/tmp/swiftvm-full-final-default-20261006.log`。

`'[frontier],*low32*'` 定向测试在默认配置，以及以下两组配置中均通过，每组 14 个用例、115 条断言：

```sh
SVM_FLAGS_REGS=0 SVM_FLAGS_CFINV=0 SVM_TSO_MODE=acqrel
SVM_X86_PIN_EXT=3 SVM_RA_FIXED_CLASS=1 SVM_TSO_MODE=acqrel
```

日志分别为 `/tmp/swiftvm-low32-flags-fallback-20261006.log` 和 `/tmp/swiftvm-low32-pin3-fixed-20261006.log`。临时隔离优化的改动已全部移除，没有新增生产诊断开关。

### Linux 客体复核

更新 Orb Ubuntu 中的 Release 启动器：

```sh
cmake --build /home/swift/.cache/swiftvm-review-fix-20260912 \
  --target svm_translator_linux --parallel 1
```

使用相同客体文件和输入，串行运行 FEX 参考、SwiftVM 直接映射及有界窗口，共 9 次探针；全部正常退出。SwiftVM 保持默认优化开启，使用 `SVM_TSO_MODE=acqrel`、关闭 JIT 磁盘缓存，每次运行绑定单核、限时 30 秒。FEX 参考为本机已有的 `/usr/bin/FEXInterpreter.f2e35f3`，不代表最新 FEX。

| 客体与输入 | 两种 SwiftVM 内存模式相对 FEX 的结果 |
| --- | --- |
| SQLite：`--size 1 --testset main --verify --output <绝对输出路径> <绝对数据库路径>` | Verification Hash 一致，111,130 字节验证文件逐字节一致 |
| CoreMark：`0x0 0x0 0x66 1000 7 1 2000` | seed、list、matrix、state、final 五项 CRC 均一致 |
| smallpt_wh：`4 8 6` | 完整 `image.ppm` 文件一致 |

SQLite 验证哈希为 `111130 1e792c9db61996c477b8ab5ce2d690052e8dae74824a430a`；验证文件 SHA-256 为 `6db359a50bc1c3f9606b73a1dd92f29a814781c3e0be612b361d241e69969481`。CoreMark 五项 CRC 为 `e9f5/e714/1fd7/8e3a/d340`。smallpt 文件 SHA-256 为 `e32ee42312d3fd9f643c6aa26f735254a4b6b6bc1111901c0193a69bf162ff5e`。

SQLite 使用绝对路径，是因为启动器现有 `SysGetcwd` 返回 `/`，而相对文件操作沿用宿主当前目录；本次未修改这项独立的路径语义。完整结果、二进制哈希、命令及输出保存在 Orb 的 `/tmp/swiftvm-linux-validation-release-20261006`；本机探针脚本为 `/tmp/swiftvm-linux-validate-20261006.py`。

这些是固定输入的正确性验证，不是性能评分，也不代表完整 AVX、其他配置或大型应用矩阵已经通过。以下 9 月 15 日记录保留为历史证据，其中 444 个用例的结果早于本次续修。

## 2026-09-15 历史记录

基于 `7d53d9e` 及当前未提交工作区。以下结果对应 macOS ARM64 Release 构建；构建使用 `ninja -C cmake-build-release -j1 swift_test`，测试串行执行。本轮没有提交或推送。

## 已修复与机制调整

- 内存 XCHG：窄存储与 LSE 交换只消费指定低位，删除两个路径中的多余窄提取。固定复现由 `No free temporary GPR` 恢复为通过，覆盖对齐及非对齐地址。
- 窄 Add/Sub：按实际发码寄存器判断输入是否被目标覆盖；符号对齐前不再重复提取简单寄存器操作数，ADD 的零位移寄存器右值直接参与对齐。scratch 预算改为操作数准备、NZCV 合并、AF 原值保留与折叠的明确成本，删除宽度相关的经验下限及临时添加的统一五寄存器下限。
- 窄 AND/BitExtract：后端不得跨非相邻 IR 推迟父值读取。可安全消除的窄掩码提取移到 `IntegerWidthEliminationPass`，使寄存器分配看到真实父值存活期。未优化路径保留提取，优化路径仍可只发出一条 AND。
- SIMD 可变移位：计数截断中的 CMP 会破坏 PSTATE；三种移位入口先合并待发布 NZCV，直接标志位消费者的破坏分析也包含这些操作。
- POPCNT：按源宽度计数，正确生成 ZF，清除 CF 后重置 carry 极性；使用计数结果判断 ZF，避免为标志位延长 helper 输入存活期。
- 标志位写入顺序：后来的 SaveFlags 取消同一窗口中更早的对应待清除位，修复急切打包路径中 ZF 被延迟 ClearFlags 覆盖的问题。
- 验证工具：四处 VIXL 反汇编循环改为按指令推进，避免字节错位解码及缓冲区尾部越读。
- 固定寄存器类验证：允许 XCHG 元数据标记描述客体 home 发布；普通发码 scratch 仍须与客体固定寄存器分离。

上述变更与工作区已有的 XCHG 发布屏障、高字节写入和窄移位修复共同接受验证。未添加生产调试输出或新的诊断开关。

## 验证证据

完整默认测试：

```sh
cmake-build-release/source/tests/swift_test --reporter compact
```

结果：444 个用例、1,124,784 条断言全部通过。原始日志保存在本机 `/tmp/swiftvm-full-derived-budget.log`。

新增固定回归包含内存 XCHG 压力、ADD 输入别名、四组 BT/BTR/BTC 存活期复现、三类 SIMD 移位的边界计数与直接标志位消费者、POPCNT 的 16/32/64 位及源目标重叠组合。

这些回归在默认配置、`SVM_FLAGS_REGS=0 SVM_FLAGS_CFINV=0` 和 `SVM_X86_PIN_EXT=3` 下通过；三个窄整数回归还在 `SVM_X86_PIN_EXT=3 SVM_RA_FIXED_CLASS=1` 下通过。scratch 定价用例的 39 条断言通过，实际发码消耗不超过声明预算。

可重复的历史失败种子：

| 测试 | 种子 | 迭代数 |
| --- | --- | --- |
| Fuzz x86 mov lea xchg extends | 1429665432088174214 | 5000 |
| Fuzz x86 bit ops | 7853176062513615565 | 2000 |
| Fuzz x86 alu | 15210798149460277694 | 4000 |
| Fuzz x86 shifts | 10579178568909756082 | 4000 |

通过 `SWIFT_FUZZ_SEED`、`SWIFT_FUZZ_ITERS` 指定种子和迭代数。固定回归已收录对应关键形态，不依赖随机测试再次抽中失败程序。

## 尚未完成

默认全量测试包含特性门控用例；AVX 未启用时的通过不代表完整 AVX 覆盖。其他配置只运行了上述定向回归，未声称全矩阵通过。

本轮没有刷新大型 Linux 客体的正确性矩阵，也没有测量当前 SwiftVM 与 FEX 的同输入动态工作量、执行速度或冷启动成本。因此这些修复不能作为整体追平 FEX 的证明。后续继续验证大型客体、建立可靠性能基线，再按热点处理编译单元、跨边状态和 helper 成本；参见 [FEX 差距清单](fex-gaps-2026-09-12.md)。
