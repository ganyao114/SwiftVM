# 按实际派发需求创建 L1：编译固定成本优化（2026-11-08）

后续构造与故障元数据所有权优化的实现、最终测量和未达项见[本轮报告](compile-shape-optimization-2026-11-08.md)。本文保留前一检查点的原始成绩。

本轮保留的优化是减少实际资源工作：纯静态 JIT 单元不创建线程 ready L1；只有实际生成动态派发出口时，才在发布前准备它。SMC 空 L1 在首次实际布防前创建。没有新增 IR、分析 pass、代码扫描或热路径判断，Block / Function 分配继续独立。

同机交替运行上一轮与当前二进制，x87 compile 排除实际 byte parser 后，Block 中位数 **0.7261→0.6468 ms（−10.9%）**，Function **0.6768→0.6437 ms（−4.9%）**。这是固定三轮的描述性结果，不代表已证明相同幅度的稳定收益。

完整最终矩阵中，x87 Block / Function 为 **0.6999 / 0.6233 ms**，同轮 FEX 为 **0.3119 ms**，倍率 **2.244× / 1.998×**。Function 处在两倍边缘，Block 仍未达标；不能宣告所有 compile 目标完成。

## 1. 根因与最终实现

x87 输入没有间接 guest 控制流，Function 只有一个单元、50 个 IR node、1732 B native code。此前进入 JIT 时仍固定创建 ready L1 和 SMC miss L1，退出时释放它们；它们各是 16 MiB 的按需分页匿名映射，各初始化一个 sentinel 页面。不存在对应规模的物理清零，但创建、页面触达和解除映射是真实工作。

现在，该不可变静态输入不创建这两个映射，少两次 mmap、两次 sentinel 页面触达和两次 munmap。CodeHeap 的双视图、W^X、故障恢复和完整 owner 回收继续保留。没有进程级资源池，也没有把初始化或释放移出完整 compile 窗口。

| 位置 | 责任 |
| --- | --- |
| `backend/code.h` | `CompileResult::has_indirect_dispatch` 记录实际生成的动态派发尾部，是瞬态 JIT 资源事实。 |
| `backend/arm64/emit_dispatch.cpp` | 在真实 `DynamicDispatch()` 发码入口置位，覆盖 terminal 和 PC store 提前提交的动态尾部。 |
| `backend/x64/emit.cpp`、`backend/riscv64/emit.cpp`、`backend/loongarch64/emit.cpp` | 在未知 PC 的 linked 动态尾跳处置位；静态边的冷 dispatcher fallback 不置位。 |
| 四后端 `compile.cpp` | `ClearCode()` 清零标记，覆盖复用输出、失败以及已有重发路径。 |
| `execution/compile_service.cpp` | 动态单元在 `CodeHeap::Publish` 前调用 `EnsureThreadDispatch()`；磁盘和 AOT 的 `Restore` 保守准备资源。 |
| `execution/runtime.cpp`、`vm.h` | 只在需要时创建真实线程 L1 并刷新 context；删除 JIT 启动时的固定创建。 |
| `execution/dispatch.cpp` | 冷 resolver 仅在真实 L1 已存在时填入 ready entry。 |
| `execution/smc.cpp` | 确认存在实际写监视候选后，在任何新增 mprotect 前创建 SMC miss view；信号处理路径不分配。 |
| `execution/code_link.cpp` | 首条实际 relocation 预留四行，避免小型 Block 的 1→2→4 连续扩容；之后保持几何增长。 |
| `execution/runtime.h`、`launcher/linux/report.cpp` | 释放前记录真实 ready L1 映射；entries / virtual_bytes 报实际值，纯静态用例为零。 |
| `tests/rebuild/dispatch_cache_test.cpp` | 在现有测试组中检查静态自修改无需 ready L1，动态派发和 FP 切域仍实际使用 L1。 |

源码路径相对 `source/runtime/`，launcher / tests 两行相对 `source/`。

这是“实际发码事实 → 发布前准备资源”的直接合同，不在 frontend 或 IR 中描述软件查表实现。GPR / 内存的 uniform IR、未知后端分支及现有分配策略没有改动。

### 正确性边界

- 四架构共享 dispatcher 原本就支持 null L1，直接转向 L2；L2 refill 也检查实际 slot 指针。初始入口和静态冷回退沿用该合同。
- ARM64 inline lookup 继续保持原来的无 null guard 形态。动态单元在任何 ready entry 或静态直链可达之前，L1 已存在。
- 从已有 native 单元冷调用编译服务时，资源准备和 context 刷新在返回新入口前完成。L1 一旦创建，就保留到 VM 关闭；FP 切域和 SMC 仅 Reset。
- SMC 的 pending 状态只能由命中已布防页的 Recover 设置。空 L1 在布防前准备，信号路径只替换指针和撤销链接。
- 磁盘 / AOT 不序列化或信任这个瞬态标记，统一在 Restore 中保守准备 L1，因此不改产物格式。离线预编译也沿用发布合同，保留同一 VM 随后执行已就绪单元的能力。
- 初始化仍计入 compile：动态单元的准备发生在 publication 内，恢复产物的准备发生在既有恢复窗口内。

## 2. 未保留的 FP 布局尝试

先尝试过把 packed DE 的完整分类放到单元尾部，让正常路径 fallthrough，并支持远分支。功能和长距离测试均通过，但同机交替对照的 FP cycles 仅变化 **−0.095% / −0.183%**，instructions 基本不变，未建立值得增加 fixup 和冷代码复杂度的收益。

这部分实现和新增长距离 fixture 已撤回。最终源码保留上一轮的 FP 双输入 admission 与精确 MXCSR 实现，没有新的 FP 冷路径队列或第二轮发码来源。

试验的源码、压缩二进制、144 条主观测、48 条交替观测及清理记录独立保存在 `cold-compile-fp-layout-20261108`，没有混入最终成绩或替换样本。最终版本的 paired FP cycles 变化为 **−0.043% / +0.032%**，没有测出新的 FP 运行收益。

## 3. 最终完整矩阵

红魔 `bf1359ff`，CPU4，固定三个重复、轮换引擎顺序。8 个原始输入和固定 Android FEX `a156752f` 沿用上一轮；不修改频率、服务或 SMC 形成策略。FEX 默认多基本块编译，保持既有 TSO 配置。

主矩阵 **72 条 compile + 72 条 runtime** 全部有效；另有 **48 条**上一轮 / 当前版本交替观测。保留全部样本和范围，没有按门槛重跑、取最快值或混合不同候选。

### 3.1 compile：只扣实际 byte parser

单位 ms，各自三次中位数。

| 输入 | Block | Function | FEX | B / FEX | F / FEX |
| --- | ---: | ---: | ---: | ---: | ---: |
| CoreMark | 42.0310 | 50.0690 | 45.9218 | 0.915× | 1.090× |
| int | 2.0377 | 1.9032 | 1.4263 | 1.429× | 1.334× |
| fp | 2.4163 | 2.8303 | 1.7242 | 1.401× | 1.642× |
| mem | 2.4483 | 2.3669 | 1.6889 | 1.450× | 1.402× |
| branch | 1.9816 | 2.0574 | 1.5979 | 1.240× | 1.288× |
| call | 2.2060 | 2.2913 | 1.6784 | 1.314× | 1.365× |
| func_tests | 20.8144 | 47.9586 | 29.7088 | 0.701× | 1.614× |
| x87 | 0.6999 | 0.6233 | 0.3119 | **2.244×** | **1.998×** |

当前 x87 Function 三次为 0.623334、0.605206、0.671042 ms；中位数刚好进入两倍以内，不等于每次或所有环境都满足两倍上限。Block 保持三个单元，Function 一个单元；FEX 的 CompileBlock 本身可以含多个基本块，不能把两种 SVM 分配层级混成同一条路径。

交替二进制对照的 x87 样本如下，单位 µs：

| scope | 上一轮三个值 | 当前三个值 | 中位数变化 |
| --- | --- | --- | ---: |
| Block | 764.790 / 726.090 / 713.074 | 646.304 / 646.821 / 657.815 | −10.9% |
| Function | 704.581 / 676.822 / 669.845 | 636.509 / 643.700 / 689.114 | −4.9% |

FP compile 的交替中位数为 Block 2.6834→2.6544 ms，Function 2.7959→2.8250 ms，约 −1.1% / +1.0%，范围重叠。

`func_tests` Function 主矩阵有 47.959 / 48.380 / 31.209 ms 的大波动；交替观测中，上一轮为 65.658 / 31.293 / 39.881 ms，当前为 31.811 / 32.017 / 31.348 ms。不能把主矩阵的上升或 paired 的下降直接归为代码收益。实际工作量、互斥阶段和所有墙钟样本均保留，不删慢样本。

FEX 现有计时仍不包括最终 native arena / lookup cache 析构，SVM 计入完整 native owner 回收。这是既有端点差异，不把 FEX 的约 2 µs logical release 当作完整回收成本，也没有从 SVM 窗口中扣掉回收以达标。

### 3.2 runtime 与代码质量

下表为 whole-child user PMU cycles 的 SVM / FEX 中位数比值。

| 输入 | Block | Function |
| --- | ---: | ---: |
| CoreMark | 1.066× | 1.027× |
| int | 0.690× | 0.689× |
| fp | 1.193× | 1.191× |
| mem | 1.048× | 1.050× |
| branch | 0.982× | 0.983× |
| call | 1.055× | 1.053× |
| func_tests | 0.763× | 0.954× |
| x87 | 0.680× | 0.677× |

CoreMark 100000 iterations，CRC d340、正式验证通过，guest loop 中位数为 16.097 / 15.494 / 15.140 s，分数 6212.34 / 6454.11 / 6605.02 iterations/s。其余 elapsed 为整进程时间，包含启动、JIT、调度与退出。FP Block 整进程耗时中位数为 FEX 的 1.352×，cycles 为 1.193×；该耗时项也按未达目标保留，不用 PMU 倍率代替实际 elapsed。

最终优化没有改动 guest 算术、IR 优化或生成指令形态。x87 仍为 Block 51 nodes / 1864 B、Function 50 nodes / 1732 B；FP instructions 仍为 FEX 的 2.854× / 2.858×。原来的精确 MXCSR 合同保留，FP cycles 约 19% 的差距尚未解决。

全部 SVM 样本 interpreted_steps=0。`func_tests` Function 仍有每次 15 次 Function→Block 编译回退，与上一轮相同。x87 workload 沿用 exit-only oracle，不将它写成完整数值验证。

## 4. 验证、剩余工作与证据

Android / host 构建和定点测试通过；Android 覆盖 FP 状态、链接、8 组 native runtime、持久化 / SMC 和 4 组 AVX。x64、RV64、LoongArch64 各用一次 QEMU system + 真实 Linux 内核完成对应 runtime、持久化和 guest JIT smoke。不是 QEMU-user 的信号行为验证，也不宣称实体跨架构性能。

新增资源断言验证了两种实际状态：纯静态自修改程序首次运行布防、修改已编译指令、撤销并重新编译，最终 exit42、SMC fault=1，真实 ready L1 全程为零；原动态派发、自修改与 FP 切域路径仍实际使用 16 MiB ready L1。磁盘 / AOT 恢复测试通过。

剩余重点是 x87 的首次 formation / IR 出生与 owner 固定成本。后续需要区分实际 CPU 工作、首次代码页触达和墙钟停顿，不能继续把差额主要归到 allocator，也不能凭函数体积就强改内联边界。FP 的正常分类成本需要新的实测依据；本轮已经排除了单纯移动冷路径这一方向的可确认收益。

工作树 `/Volumes/Data2/SwiftVM-submit-20261104`，基线 `cbd06547` 加既有 WIP；本轮相对上一轮稳定实现改动 18 个文件。最终冻结 71 个已改文件，构建 / 测量前后哈希匹配；47 个 C++ 文件格式检查和 `git diff --check` 通过，未提交或 push。

最终结果根：`/Volumes/Data2/swiftvm-middle-results-relocated-20261025/dispatch-demand-20261108`。包含 `benchmark/summary.json`、互斥阶段 `compile-breakdown.json`、`paired/`、各后端验证、源码压缩包和产品 gzip。当前 runner SHA256：`c8ac4ba9e29d3bbbc8ebcb4129140288bd42ff14bbc928fda3bc6df2c9e6effa`；稳定父版本为 `68f0116fa9514a3aa2bcf003cc42e0569cecaf1fc7e8edc50f98e76633f2b113`。

手机最终测试目录已清空：移除 allocated bytes 252,236,800，独立 df 空闲增量 253,693,952，无活跃进程、打开文件或清理错误。各 QEMU 的派生文件也已回收；大临时数据始终放 Data2，继续复用固定构建缓存。
