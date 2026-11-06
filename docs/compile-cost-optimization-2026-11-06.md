# Compile 成本优化与复测：2026-11-06

本轮已完成一批不改变指令选择和寄存器分配决策的通用成本优化。对比上一轮指令覆盖版本，同批 CoreMark Block compile 中位数下降 **9.4%**，Function 下降 **2.7%**。多数小负载改善，但 call Function 本批增加 **5.5%**，x87 Block 同批仅改善 **0.6%**，仍是 **2.25× FEX**。此前所有负载最多两倍的目标尚未完全达到。

代码位于 `/Volumes/Data2/SwiftVM-submit-20261104`；旧 `/Volumes/Data2/SwiftVM` 只读。本文指纹对应提交整理前的已测快照，格式化验证另行记录。该轮性能优化改动为现有 21 个文件，没有新生产文件、pass、N 级镜像或 Workspace 包装层。

## 已落实的收敛

| 原成本 | 本轮实现 | 保留的合同 |
| --- | --- | --- |
| 每次新建 Program::Storage，只移动旧 vector 容量 | 消费旧 owner 后原地 reset storage | 每轮新完整 unit ID、scope、target；清空 operations/operands/blocks、entry/active/unfinished。公共 Program metadata 仍只读，旧 Value 不因容量复用重新有效 |
| 优化器每请求分配一份 uses/flags rows | CompileService 持有同一工作数组，调用前 assign 清零 | 仍只有原来一个 u64/node 表、原三遍 walk，先分配再转移 Program；8 MiB 上限，释放计入原窗口 |
| 每请求新建 Allocation 的 homes/order | Arm64Runtime 持有同一 Allocation，增加借用式重载 | Home 16B、原 RA 算法和遍历不变；每轮清标量、homes.clear+resize、order.clear；失败清状态；长分支重发借同一 Allocation |
| 一个外部退出的单块 Function 仍建立 flags CFG、前驱与 DFS | 仅该真实形状省去图配置 | Scope 保持 Function；Jump/Branch，包括自回边，继续原图算法，Block 与 Function 分配仍独立 |
| 单 block label 分配 4B vector | Assembler 对象内存放单 label，多块仍用原数组 | 同一 label/fixup owner，span 不逃逸，Assembler 不可复制；没有额外全程序标签镜像 |
| 双别名各做一次完整 cache flush，conditional patch 每 word 各同步 | 一个共享 SynchronizeCode 清 RW、失效 RX；冷停止态多个指令写后一次同步实际变更范围 | target release-store → gate → head 顺序保留；SIGBUS 补丁返回前完成同步；SingleRX 原 full-sync → mprotect 时序不变 |

旧 Optimize、Allocate、Compile API 继续支持单次调用后回收；容量复用由已有编译 owner 选择。输出故障表仍移入 CodeHeap 保有，不能当作临时 scratch 回收。Frontend 的 Uniform GPR/XMM IR、Known/EarlyOff/Unknown 分支均未改变。

## 测量方法与完整结果

红魔 NX729J，serial `bf1359ff`，CPU4，既有策略 614400–1171200 kHz。本轮开始的 runner 为 baseline；优化版与它、固定 FEX 在同一批轮换五个 arm，每项五轮，共 **200 个样本**，输出和计时闭合检查全部通过。仅扣除实际字节 parser，保留适配、形成、IR、优化、分配、发射、发布和释放。全部样本保留，没有替换慢轮次。

表中是五轮中位数，单位 ms；变化与 FEX 倍率均为各自中位数之比。

| workload | Block 前 → 后 | 变化 | Function 前 → 后 | 变化 | 当前 / FEX，B / F |
| --- | ---: | ---: | ---: | ---: | ---: |
| coremark | 46.3705 → 41.9926 | -9.4% | 52.6132 → 51.1897 | -2.7% | 0.88 / 1.07 |
| int | 1.9847 → 1.8700 | -5.8% | 2.0094 → 1.8465 | -8.1% | 1.20 / 1.18 |
| fp | 2.6014 → 2.4207 | -6.9% | 2.6153 → 2.4667 | -5.7% | 1.45 / 1.48 |
| mem | 2.4696 → 2.2868 | -7.4% | 2.5600 → 2.2170 | -13.4% | 1.43 / 1.38 |
| branch | 1.9681 → 1.9250 | -2.2% | 2.0056 → 1.9314 | -3.7% | 1.27 / 1.28 |
| call | 2.0395 → 1.8440 | -9.6% | 2.0192 → 2.1307 | +5.5% | 1.13 / 1.30 |
| func_tests | 23.4494 → 21.1883 | -9.6% | 33.9802 → 33.1372 | -2.5% | 0.71 / 1.11 |
| x87_bench | 0.6463 → 0.6425 | -0.6% | 0.5859 → 0.5619 | -4.1% | 2.25 / 1.97 |

call Function 这一项未获总时间收益，仍保留回退结果；五轮不足以把冷启动/映射波动和实现影响完全区分。它的 IR 数、native bytes、spill/frame/fault/link 等生成指标与 baseline 一致，后续应独立复测，不能用整体均值覆盖。

CoreMark Block 的阶段中位数：frontend 16.123→15.509 ms、optimize 9.216→8.555 ms、backend 15.162→14.111 ms、publication 7.925→5.965 ms。阶段中位数不对应同一次样本，不能相加重建总时间或当作精确收益归因；原始同次记录均已保留。收益来自删除构造、分配、图配置和重复同步，不能归给单独一项。

## 机制证据与质量检查

- 同一个真实 IR 构建→优化→ARM64 发射微基准，8 次预热后 1000 次含 32 条 add 的小请求：Block 的 C++ operator-new 从 **5000 次降为 0**，单块 Function 从 **11000 次降为 0**。这只计该微基准中的 C++ 分配，完整 x86 前端、首次请求及需要持久保有的 fault/link 数据不在“零分配”结论内。
- 同进程分别走旧式 fresh scratch API 和复用 API，在大小 1/64/3/255/2 的请求间切换，发射机器字、spill、frame 完全一致；增加一次无效请求后再成功编译，也得到相同机器字。旧有 fresh identity/foreign Value 测试仍通过。
- 手机上同一双别名映射，5 个轮换批次、每批 2048 次 copy+sync：64B 从 **666.8→345.1 ns**，256B **754.3→358.6 ns**，1KiB **1508.3→621.6 ns**，4KiB **1893.0→980.1 ns**。每批执行最后生成的代码并检查返回值。这是同步 micro 的收益，不是整个 compile 的降幅。
- 同一实际 MemMap 的 20 轮 64KiB/1MiB 对比，Map 中位数为 12.162/11.641 µs，Release 为 8.072/10.990 µs。这个独立微基准没有显示足以解释 x87 差距的 slab 大小收益，因此本轮保留 1MiB slab，避免增加大程序的映射次数。
- 主机 CTest **9/9**，ASan/UBSan **9/9**；红魔 C++/workload smoke **39/39**；新增指令测试 **3/3**，状态图像 InterpretOnly/Block/Function **3/3**。已有 hotlink、SMC、真实故障恢复、两种 native ABI 路径均包含在本轮回归中。
- 五个确定性小负载与 CoreMark Function 的 native bytes、spill、frame、fault、link 等指标保持一致。CoreMark Block、func_tests Block 的实际请求/覆盖集合存在少量差异，已记录范围；不能把更小代码总量当作指令选择质量提升。本轮没有新的完整 runtime/PMU 成绩。

缓存同步依据 [Arm §9](https://documentation-service.arm.com/static/62a304f231ea212bb662321d) 与 [LLVM compiler-rt 21.1](https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-21.1.0/compiler-rt/lib/builtins/clear_cache.c)。Linux/AArch64 按 CTR 的独立 I/D line size 和 IDC/DIC 执行 RW DC CVAU → DSB → RX IC IVAU → DSB/ISB；其它宿主保留 builtin 路径。没有按某一台手机的 cache bit 写死。

## x87 剩余差距的具体来源

### 实际存在重叠翻译

手机统计为 Block **3 次请求、30 条客体指令、65 IR 节点**，Function **1 次请求、20 条客体指令、50 节点**。对真实 guest 用当前 BuildIR 单独复核三次入口，得到：

| Block 范围 | 指令数 | 字节数 |
| --- | ---: | ---: |
| 0x400078–0x40009d | 15 | 37 |
| 0x400089–0x40009d | 10 | 20 |
| 0x40009d–0x4000aa | 5 | 13 |

第一段已包含循环体，回跳到内部的 0x400089 时又编译第二段，重复提交 10 条指令、20B。Function 的 CFG 形成只需一份循环体；固定 FEX 记录也是一次 compile root。这里同时存在请求数量和真实提交工作量差异，不能全归因给一次 IR 操作太贵。

下一项值得验证的是：在 decode/region 形成阶段识别当前线性范围内的回跳 leader，尝试避免重叠区域的 IR 重建。优先复用已有 region 的 cut/leader 规则，保留 Block allocation，不把 Block 改成 Function。切分会改变入口和首次迭代路径，需要一起评估寄存器/flags 发布、链接与运行质量；本轮保持原形成策略，没有用未验证的切分换数字。

### 首次构造、发布与释放仍占明显比例

在当前 x87 五轮记录中，按同一次样本先求 frontend−byte + optimize + backend、再取中位数，Block 的核心编译成本为 **410.6 µs**，Function **406.8 µs**；FEX 的对应 formation/builder/pass-manager/backend 合计约 **269.4 µs**。该诊断口径约为 1.52×，不能替代包含生命周期的原总成本。

原总口径仍有一个已经存在的边界差异：SVM 计入宿主 binding 和 VM::Close 中的物理 native cache/buffer/slab 释放；FEX 探针计 compile root 内的逻辑 IR/临时缓冲释放，未计同等最终 native arena 销毁。当前 x87 Block 的物理释放中位数约 **50.0 µs**，FEX 的逻辑释放约 **2.2 µs**；二者不能直接当作同一阶段效率差。

发布中位数 SVM Block 约 **160.8 µs**、FEX 约 **7.4 µs**，还包含首次 lazy mapping、页触达、链接/元数据和不同发布粒度。单独同步 micro 已证实更快，但这一组冷启动发布没有随之稳定下降。需要继续把冷启动发布拆开量测，不能直接把 160.8 µs 全归给 flush 或 mmap，也不能通过把初始化/清理移出计时宣称达到两倍目标。

## 可复查证据

原始目录：

`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/compile-cost-optimization-20261105`

本仓库小型汇总见 [evidence](evidence/compile-cost-optimization-20261106/README.md)，本轮相对上一已测版本的 21 文件补丁也在其中。完整目录保留 200 个 compile 样本、阶段分解、原始输出、三个独立微基准、重叠翻译复核、源压缩包与最终 runner；手机独立 namespace 已清理，未操作其它会话。

最终源码依赖指纹：`1bf041a1e0167d41247d8c0cc31428be4aff93e032a31bc1e129131853fba322`（host/Android 并集 282 项）。Runner SHA256：`3862136f25ab0dffcac1051b4d6b66bbef0b23a0099fda795edc5677e3cb7f20`。Baseline SHA256：`4e440cceaffe199b1d42fbc38d34715d3b211c9c5ed6bd68115c763634ca0783`。
