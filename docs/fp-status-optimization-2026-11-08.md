# FP 状态与编译发布优化实测（2026-11-08）

本轮按 `benchmark-gap-analysis-2026-11-08.md` 的结论优化产品代码：合并 packed FP 异常检测，消除静态链接发布与 ready entry 的重复工作，复用 Function 源窗口容器。红魔上的旧、新二进制交替对照显示，FP 的 Block / Function cycles 分别下降 **64.95% / 64.75%**，instructions 下降 **53.49% / 53.46%**。完整新矩阵中，FP cycles 为 FEX 的 **1.191× / 1.192×**，此前为 3.391× / 3.378×。

仍未达到所有既定目标：FP cycles 仍高于 FEX 约 19%；x87 compile 排除 byte parser 后仍为 FEX 的 **2.801× / 2.470×**。发布优化减少了重复工作，但尚未根治首单元的形成、发布和完整生命周期固定成本。

工作目录为 `/Volumes/Data2/SwiftVM-submit-20261104`，基线提交 `cbd06547a2d9e5259f65bc99a69e95c055aa8bcb`；包含此前尚未提交的线程 L1 / 共享目录改造。本轮没有提交或 push，冻结参考目录 `/Volumes/Data2/SwiftVM` 未改动。

## 1. 实现与职责

| 文件 | 本轮职责与改动 |
| --- | --- |
| `source/runtime/backend/arm64/emit_fp_status.cpp` | 新增 FP 状态、denormal / NaN 分类与控制辅助实现；包含同一 consumer 的双输入正常路径和罕见路径。 |
| `source/runtime/backend/arm64/emit_float.cpp` | 保留浮点指令的 Handle 实现，调用状态辅助；按 predicate 与实际 FP profile 选择比较指令和状态修正。 |
| `source/runtime/backend/arm64/emit.h` | 声明 Writer 状态辅助接口，不新增 IR 或中间表。 |
| `source/runtime/execution/code_link.cpp`、`code_cache.h` | `AddStaticLink` 接收已准备好的 ready target，保留解除链接所需的 relocation row，省去第二次 Reserve / 查表 / 比较与修补。 |
| `source/runtime/execution/compile_service.cpp` | CodeHeap 发布成功后读取已经初始化的目标 literal，向链接登记传递 ready target；Release 释放复用窗口容量。 |
| `source/runtime/execution/code_cache.cpp` | ready 入口去重时避免 canonical PC 被再次绑定；额外入口 offset 校验仍先执行。 |
| `source/runtime/execution/compile_service.h`、`frontend_service.cpp` | `region_windows` 归实际编译工作区所有；每个请求先 clear，再根据当前映射重建内容。 |
| `source/tests/rebuild/arm64_vector_test.cpp`、`arm64_link_test.cpp` | 覆盖 FP 状态与链接发布合同。 |
| `source/tests/rebuild/avx_coverage_test.cpp` | 修正旧 AVX 测试对 sticky 状态的过时预期，并补齐独立 native fixture 的 FPSR 观察。 |
| `benchmarks/app_inputs/split_bench_suite.py` | 从固定原始 ELF 派生独立 RX / RW 输入，仅用于区分单元形成策略。 |

没有增加 pass、第二份 IR、证明表或 actions / certificates 包装。Block 与 Function 的分配路径继续独立；本轮没有改动 allocator。

### 1.1 FP 正常路径

此前每个 packed 算术 consumer 对两个 operand 分别执行 DE 检测，比较又逐 lane 扫描 NaN，并保存、恢复 FPSR。本循环共 14 次 packed 加乘、4 次 packed LT 比较，每轮正常路径额外约 320 条 host 指令。

现在两个输入共用一次 admission：2D 用 UZP2 收集四个 high word、去符号后取最小值，4S 对两个输入去符号后合并最小值，再检查指数。两条正常路径分别为 6 / 7 条指令。仅当至少一 lane 的指数为零时进入逐 lane 分类，继续保持同 lane NaN / invalid、零除数、负 sqrt 与 FMA 的异常优先级。

当 guest predicate 与该 FP profile 下的 native quiet / signaling 行为一致时，直接保留 native 比较产生的状态；不匹配的 predicate 继续修正。常真、常假 predicate 也保留显式异常分类。标量路径继续使用原本的两个单输入检测，避免 packed admission 给 scalar 正常值引入不必要的罕见分支。

只使用明确保留的 scratch，选择向量 scratch 时排除实际输入寄存器；不改变 pin bank、guest NZCV 或 context 寄存器。MXCSR 的 DE、IE、sticky、DAZ / FTZ 合同继续保留。

本循环正常路径模型降为每轮 116 条额外指令，减少 `204 × 4,000,000 = 816M`。交替二进制实测指令差与其一致；这不是对所有 FP 输入的动态指令数承诺。

### 1.2 发布和工作区

静态链接的 prepared 路径只在 CodeHeap 发布成功后使用，且排除 self / cold target。提前向目录插入链接会在 relocation、restore 或 cache sync 失败后留下悬空记录，因此没有采用。prepared 路径仍登记原有 unlink row 并保持统计含义，pending / self 路径按原逻辑执行。

Function 源窗口只复用 vector 容量；映射与权限分类每次重建，不缓存旧权限或 IR。这会减少后续请求的分配，不能消除第一次请求的容器分配。

## 2. 测量方法与证据

结果根目录：`/Volumes/Data2/swiftvm-middle-results-relocated-20261025/fp-status-optimization-20261108`。

- 红魔设备 `bf1359ff`，CPU4；保留原频率策略，测量前后 min / max 为 614400 / 1171200 kHz，未修改服务或频率。
- 固定 8 个原始输入，Block / Function / FEX 轮换顺序，各 3 次。主矩阵共 **72 条 runtime + 72 条 compile**，全部有效，保留所有样本与范围，不按阈值重跑或挑最快值。
- runtime 使用普通产品 ELF，关闭 byte 计时与热态 dispatch 诊断计数。PMU 为整个子进程树的 user cycles / instructions / branch-misses，同组、不 multiplex；私有 FEXServer 在测量树之外预热。
- CoreMark runtime 为 100000 iterations，CRC d340，guest loop 均超过 10 秒。其余耗时包含 fresh process、启动、JIT 和退出，短任务耗时受调度影响。
- compile 使用独立诊断运行，双方只扣实际 byte parser；CoreMark 使用 20 iterations、CRC 4983，不将短运行当正式 CoreMark 分数。
- FEX 固定 `a156752fae3a4f65fefb1ed95af980bcdfac43d7` 加既有 Android patch，默认 multiblock；显式保留 scalar / vector / memcpyset / half-barrier TSO，关闭 TSO 自动迁移、AVX、AOT / object cache。
- 产品 runner SHA256：`68f0116fa9514a3aa2bcf003cc42e0569cecaf1fc7e8edc50f98e76633f2b113`。旧 runner SHA256：`0e53e1c3a0f528377740713197ae647d57939dc0b684607337e7a1464e807823`。

`benchmark/summary.json` 为主矩阵，`compile-breakdown.json` 的每行均来自一个真实样本，互斥阶段之和闭合。`paired/` 是另行保留的旧、新二进制对照，`rx-rw/` 是独立形成诊断，不与主矩阵混合。

### 2.1 FP 同机交替对照

固定 3 轮，Block / Function 各运行旧、新版本，交替次序，共 12 条完整 PMU 观测。

| scope | 旧版 cycles | 新版 cycles | 变化 | 旧版 instructions | 新版 instructions | 变化 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Block | 732,488,067 | 256,746,960 | −64.95% | 1,525,490,213 | 709,456,637 | −53.49% |
| Function | 729,721,116 | 257,250,241 | −64.75% | 1,526,422,244 | 710,356,803 | −53.46% |

### 2.2 完整 runtime 矩阵

比值为 SVM / 同轮 FEX 的三次中位数，cycles 越低越好。

| 用例 | Block cycles | Function cycles | Block instructions | Function instructions |
| --- | ---: | ---: | ---: | ---: |
| CoreMark | 1.071× | 1.027× | 1.018× | 0.989× |
| int | 0.690× | 0.690× | 1.017× | 1.017× |
| fp | 1.191× | 1.192× | 2.854× | 2.858× |
| mem | 1.051× | 1.056× | 1.257× | 1.257× |
| branch | 0.979× | 0.980× | 1.081× | 1.081× |
| call | 1.057× | 1.057× | 1.215× | 1.215× |
| func_tests | 0.749× | 0.933× | 0.747× | 1.017× |
| x87 | 0.670× | 0.673× | 1.374× | 1.371× |

CoreMark 的实际 guest loop 中位数为 Block 16.105 s / 6209.25 iterations/s、Function 15.453 s / 6471.24 iterations/s、FEX 15.026 s / 6655.13 iterations/s。

所有 SVM runtime 样本 interpreted_steps=0。`func_tests` Function 每次有 15 次 Function→Block 编译回退，旧版同样为 15；其余样本为零。不能将这项统计写成“完全没有编译回退”。x87 沿用 exit-only fixture，本轮不宣称完成独立的完整数值 oracle。

原来的 1.10× 运行门槛仍有未达项：FP cycles；call Block 的短任务整进程耗时中位数为 FEX 的 1.115×，cycles 为 1.057×。不据这个短任务的耗时差单独推断代码质量。

### 2.3 compile，排除实际 byte parser

单位 ms，仍计入已有编译窗口的其他全部工作。

| 用例 | Block | Function | FEX | Block / FEX | Function / FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| CoreMark | 41.9435 | 50.2284 | 46.8098 | 0.896× | 1.073× |
| int | 2.0150 | 2.1206 | 1.4897 | 1.353× | 1.423× |
| fp | 2.4639 | 2.7061 | 1.7452 | 1.412× | 1.551× |
| mem | 2.3442 | 2.4892 | 1.6279 | 1.440× | 1.529× |
| branch | 1.9532 | 1.9341 | 1.5122 | 1.292× | 1.279× |
| call | 2.1276 | 2.4014 | 1.5597 | 1.364× | 1.540× |
| func_tests | 21.0671 | 31.8790 | 46.4951 | 0.453× | 0.686× |
| x87 | 0.8154 | 0.7192 | 0.2911 | **2.801×** | **2.470×** |

另有固定 12 条 x87 旧、新交替 compile 对照：Block 0.8116→0.7303 ms，Function 0.6561→0.6342 ms，描述性下降约 10.0% / 3.3%；样本范围重叠，不作为稳定收益幅度的证明。也不把不同批次 CoreMark 或 func_tests 的墙钟变化全部归功于源码改动。

SVM 仍计入 native owner 的完整回收；FEX 现有 probe 只计工作缓冲逻辑释放，最终 native arena / lookup cache 析构不在其中。此端点差异保留在报告中，没有把 SVM 回收移出窗口以达标。

以下是 x87 各自总量中位数所对应的**同一条实际样本**，单位 µs：

| 阶段 | Block | Function | FEX |
| --- | ---: | ---: | ---: |
| formation / IR，排 byte parser | 267.6 | 247.7 | 116.4 |
| IR 优化 | 67.0 | 64.1 | 46.0 |
| 分配与发码 | 138.0 | 102.5 | 99.5 |
| transfer / publication | 112.0 | 122.8 | 21.6 |
| SVM owner 回收 / FEX 逻辑缓冲释放 | 166.6 | 130.2 | 2.0 |
| 其他已计窗口 | 64.2 | 52.0 | 5.7 |
| 合计 | **815.4** | **719.2** | **291.1** |

后续 x87 应优先对齐 FEX 完整生命周期观察、剖分首单元形成与发布；现有数据不支持把主要差额继续归给 allocator，也不支持删除正确性合同。CALL / RET 专门的影子返回机制及可写代码多块形成未在本轮实现。

## 3. RX / RW 单独诊断

原始 tiny suite 为单个 RWX PT_LOAD。脚本只接受固定 SHA256 `df60aa3f57d40ed3eb0b44d124f2cb8c9d15899f90021b6d20e94be179240a6d`，将 header table 追加到文件尾，保留入口以后的全部原始指令 / 常量字节、guest PC 与 BSS 地址；输出独立 RX code / RW data ELF，SHA256 `335e0025b79d6b16e0fd15722eed1a744e19e1778effab9478df98a1aead0b4b`。

5 个 kernel × 3 个引擎，各 1 次编译诊断，共 15 条，checksum 均正确：

| 用例 | 原 RWX 的 Function 请求数 | RX/RW 的 Block 请求数 | RX/RW 的 Function 请求数 | RX/RW 的 FEX CompileBlock 调用数 |
| --- | ---: | ---: | ---: | ---: |
| int | 38 | 39 | 6 | 3 |
| fp | 42 | 43 | 6 | 3 |
| mem | 45 | 46 | 6 | 3 |
| branch | 38 | 39 | 6 | 3 |
| call | 45 | 46 | 14 | 7 |

RX / RW 的 SVM interpreted_steps、编译回退、SMC 强制单块次数均为零。这验证了源权限对形成策略的影响；它不是原 RWX 负载的性能提升，单次诊断也不承担速度验收。产品对可写源的 SMC 单块限制保持原状。

## 4. 验证与回收

Android 与 host 构建通过。红魔原生验证覆盖 PackedFPStatus / PackedFloatingArithmetic、链接发布 / 动态 PC、FP runtime / dispatch cache、持久化 / SMC 和 relocation。PackedFPStatus 对 17 组向量分别覆盖 32 / 64 bit、已知软件 / 动态软件 / AFP profile；AFP profile 原生执行受实际硬件支持约束，本机 AFP=false。

额外 AVX 检查最初暴露旧 fixture 的“MXCSR sticky 不变”预期：先前精确 FP 实现已允许产生异常状态，但 fixture 尚未更新。该 fused near-one 输入确实产生 PE，独立预期改为 0x1fa0；独立 native fixture 清除并读取 FPSR，再映射全部六个异常位与已有 MXCSR 合并，没有屏蔽异常位。修正后 compile-only 与全部 4 个 native AVX 测试组通过，产品 ELF 未改变。原失败 stdout / stderr 与修正后的结果均保留。

x64、RV64、LoongArch64 使用增量固定构建缓存，在 QEMU system 的真实 Linux 内核下验证共享发布、链接、FP domain、SMC 和持久化；原生组与 guest JIT Block / Function smoke 全部通过。它们是功能验证，不代表实体设备性能。各自 console、source hash 与清理回执保留在结果根目录。

40 个已改 C++ 文件按仓库 clang-format 配置检查通过，`git diff --check` 通过。

本轮手机目录 `/data/local/tmp/swiftvm-fp-status-20261108` 已清空，移除 allocated bytes **289,952,768**，独立 df 空闲增量 **291,418,112**；没有活跃进程、打开文件或清理错误。QEMU 派生 initrd / 临时 rootfs 已回收。保留测量脚本、原始文本 / JSON、源码快照和压缩产品二进制在 Data2；继续复用固定构建缓存。
