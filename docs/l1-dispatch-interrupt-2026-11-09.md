# 线程 L1 中断视图：实现与验收（2026-11-09）

本轮实现按线程切换 L1 指针的内部事件出口，降低 ARM64 动态间接派发的重复检查成本。基线为 `a5a2b93c`，实现位于 `/Volumes/Data2/SwiftVM-submit-20261104`，未修改冻结参考树 `/Volumes/Data2/SwiftVM`。实现提交为 `c750c99e`。测量时使用未提交工作树，冻结的48个源文件与该提交一致。

## 实际行为

```mermaid
flowchart LR
    Q[目标线程收到可交付事件] --> A[context.l1 选择 PROT_NONE 视图]
    A --> L[下次动态 L1 lookup]
    L --> F[精确 probe + 所属保护范围匹配]
    F --> R[恢复完整状态和已完成转移的目标 PC]
    R --> E[host-only GuestEvents 出口]
    E --> D[VM 选取并交付事件]
    D --> N[恢复正常或 SMC 空 L1 后重新进入]
```

保护视图覆盖整个 `2^20 × 16 B = 16 MiB` 索引范围，匿名映射只保留虚拟地址，不初始化表项、不触及数据页。首次实际动态单元发布前与普通 L1 一起建立；纯静态路径不因此创建映射。磁盘/AOT 恢复沿用已有保守准备路径。每次请求只切换指针，不调用 mprotect，也不改写共享 native code。

不能将指针设为 null：`nullptr + index * 16` 的访问覆盖低地址 16 MiB，可能访问已经映射的 guest 内存；保护整段索引范围才能保证所有索引均故障。SMC 的空 L1 也不能充当中断视图，空表会 miss 到 L2，仍可能继续执行。

`NativeRunContext::l1` 原位改成 lock-free atomic 指针，offset1032 和 context1072 B 均不变，不新增永久寄存器。ARM64 的 x19 仍指向 embedded state，L1 相对偏移为872。

## 职责与文件

| 源码位置 | 本轮职责 |
| --- | --- |
| `execution/dispatch_cache.{h,cpp}` | `DispatchInterrupt` 拥有唯一 PROT_NONE 映射，沿用既有失败释放重试合同；不增加新的 owner 管理层 |
| `execution/dispatch.{h,cpp}` | 固定 ABI 和信号安全的视图选择；正常服务刷新 L2 时缓存恢复用 L1 指针 |
| `execution/fault.{h,cpp}` | 保存保护范围、恢复指针和慢路计数；地址资格检查不读取可变目录 |
| `execution/runtime.cpp`、`smc.cpp` | 按需建立/回收视图，SMC 选择空表作为正常恢复目标，中断视图优先 |
| `platform/linux/guest_signals.cpp` | ready request 更新时选择视图；在受管 ingress 屏蔽区选取、取消或空队列 drain 后恢复正常视图 |
| `backend/code.h` | `FaultSiteKind` 区分 guest memory 和内部 dispatch interrupt，借用原 recovery[6]；FaultSite仍32 B、NativeCode仍40 B |
| `frontend/{x86,arm64}`、`ir/opcodes.inc` | 提交中立的 `GuestDispatchSafepoint`，与原观察点保留相同完整状态/flags/control 合同 |
| `ir/direct/scalar.inc`、`ir/passes/rewrite.cpp` | direct consumer 和现有优化流程识别新观察点；解释器仍在指令前交付 |
| `backend/arm64/emit_dispatch.cpp`、`emit_control.cpp` | 在现有发码中判断实际动态尾，去掉合格的前置 poll，并为真实 LDP 记录精确恢复点 |
| `backend/arm64/{emit.cpp,allocation_selection.cpp}` | 延续完整 observer 与冷恢复 PC 合同，不重新发布热入口 PC |
| `backend/*/entry_elf.S` | 共享查表的精确 probe 标签，独立 `SvmNativeEventExit(12)` 共用 host-only 恢复尾 |
| `platform/linux/*_host.cpp`、`signals.cpp` | 原始 kernel frame 恢复、PC/phase/保护范围联合资格、内部事件与 guest fault 分流 |
| `execution/code_image.{h,cpp}` | 持久格式2→3，现有加载校验识别 site kind，旧 key 不复用 |
| `launcher/linux/report.cpp`、`execution/runtime.h` | 报告内部中断次数和保护视图虚拟容量，不为普通 lookup 增加计数器 |

文件职责在既有边界内扩展，没有新增 IR 分析 pass、MIR、任意 host PC snapshot、per-block owner、全表遍历或证明包装层。

## 哪些检查被省去

| 路径 | 本轮实现 |
| --- | --- |
| ARM64 Inline / InlineCounted，最终 PC store 是真实动态目标 | 省去 `GuestDispatchSafepoint` 的前置 load+CBZ，复用原 L1 LDP；正常 lookup 不增加指令 |
| 间接语义被常量折叠为静态直链，或双常量 Select | 保留普通前置 poll，避免绕过 L1 后失去响应 |
| Callback 或没有 fault locations | 保留前置 poll |
| x64 / RV64 / LoongArch64 | 共享 L1 中断与恢复已接通；新 marker 暂按原 poll 发码 |
| 直接向后边、Function 内循环、预算/4 KiB 窗口和 REP 元素边界 | 继续使用原观察点 |
| HostWait | 继续原有唤醒/取消 stub，切换 L1 不会唤醒阻塞的 syscall |

ARM64 的资格判断只检查最终 store 与直接 producer，复用 ConstantPC 的静态尾规则，工作量固定；不扫描整块、不创建额外分析表。新 marker 仍是完整 observer，不能因为省去两条机器指令而让优化器丢弃应恢复的 flags 或延后 architectural store。

## 恢复、竞态与生命周期

1. 内部故障同时要求 native root 有效、所属保护范围、SIGSEGV/SEGV_ACCERR 和精确查表 PC。inline 使用随发码生成的单条 LDP 范围；共享 dispatcher 使用汇编标签，涵盖 counted 与 NZCV 变体，不依赖它位于 CodeHeap 内。
2. 普通 guest memory 故障仍要求 Guest phase。共享 lookup 可处于 Entering，尤其 x64 首次 root probe，此时 active 可以为空；不为正常派发增加 phase store。
3. 从原始 signal frame 恢复 pins、vectors、flags 和 FP 状态。ARM64 使用 x0、x64 使用 RDI、RV/LA 使用 a0 恢复**已经完成转移后的目标 PC**，不回到 CALL/RET 前重新执行。恢复后走内部 exit12，不产生 guest SIGSEGV，不调用 RecordGuestFault，也不覆盖原 guest exception。
4. `SvmNativeEventExit` 恢复 host root frame；handler 恢复出的 guest state 不再经普通发布尾覆盖。root_sp 清空、phase 回到 Runtime，VM 原有事件选择和恢复流程继续执行。
5. 信号 ingress 只操作 lock-free atomic 和提前缓存的恢复指针，不读取正在 grow 的 L2 目录。正常刷新在选择后再读一次 pending，避免被 ingress 打断后用旧正常指针覆盖新请求。SMC 的恢复目标始终是空 L1，中断视图的优先级高于它。
6. 请求取消后，在途 lookup 可能已经持有保护指针；不能额外要求 pending 非零，也不能要求 context.l1 仍指向保护表。这样的精确故障仍安全返回 VM；空队列 drain 同样恢复正常视图，避免反复 exit12。本轮收口审阅补上了这条空队列恢复路径，并增加真实 VM 断言。
7. 保护映射先完成，再发布范围 base 和可执行入口。Close 在 native/signal borrows 结束后撤销范围与指针，再释放映射；handler 不 mmap、不分配、不回收代码。

当前 VM 仍只有一个 guest thread，进程级 GuestExecutionLock 的既有串行执行合同未改变。两 context 的指针独立性已验证，但没有宣称 clone、多 guest thread 并行和跨线程 SMC 停机协议已经实现。本轮接通现有 GuestSignals 的 ready 事件，没有新增公开 Pause/Resume 调度 API。响应边界由下一次 L1、现存循环检查和阻塞唤醒协议共同提供，不能宣称任意长 helper 或 LR/SC retry 已具备严格有界延迟。

## 验证范围

- Host 的 x86 construction、ARM64 frontend、IR direct 三项合同通过；ARM64 发码与 code image 两项合同通过。发码测试覆盖动态 L1、counted、静态折叠/Select/Callback/no-metadata fallback，以及完整 pin observer。
- 红魔 `bf1359ff` 最终产品：L1 中断2、原异常/API7、signals5、dispatch cache3、persistence4，共21个用例通过。包含真实异步信号打断 Block/Function 间接循环、目标 PC 恢复、pending0 的在途查表、空队列恢复、FP 域和 SMC。
- x64/RV64/LA64 的真实 Linux kernel（QEMU system）各通过 L1 中断2 + exception API2 + signals5，共27项；空队列修正后，另按新源码重建并各补验同一 L1 中断2项。不是 qemu-user，也不是硬件性能结果。
- 首/尾索引访问均验证为 EFAULT；共享 probe 测试同时覆盖原 active 为空的入口和从保护表恢复到普通 resolver。两 context 仅切换目标 owner 的指针。
- 所有改动的 C++ 源文件按仓库配置格式化，`git diff --check` 通过。测试集中在本轮合同和相关已有能力，没有重复运行全指令覆盖或全套 FEX 应用矩阵。

## 最终固定样本测量

红魔 CPU4，taskset10；频率 policy 未修改，min/max614400/1171200 kHz。每 arm 固定3次交错取中位数，保留全部样本，无最快值筛选；最终84条观测为48条热态 + 36条 compile。使用相同 ELF/checksum，全部完成且零解释器回退。

热态复用原有两轮 guest 和 PMU controller，只测第二个已经充分预热的同 PC 区域；cycles、instructions、branch-misses 为指定 TID 的 user event group，time_enabled=time_running，无 multiplex。下表每迭代包括 target 选择和校验等共同工作，CALL 包含 RET；不是孤立 lookup 的周期数。

| 用例 | Scope | 时间 ns：旧→新 | cycles 变化 | instructions：旧→新 |
| --- | --- | ---: | ---: | ---: |
| call-mono | block | 8.544→8.214 | -6.54% | 49.008→45.008 |
| call-mono | function | 8.514→8.209 | -6.37% | 49.008→45.006 |
| call-64-permuted | block | 17.246→15.299 | -13.85% | 49.008→45.008 |
| call-64-permuted | function | 17.207→15.810 | -8.26% | 49.008→45.008 |
| jump-mono | block | 6.097→5.860 | -3.18% | 35.008→33.008 |
| jump-mono | function | 6.009→5.812 | -2.36% | 34.008→32.008 |
| jump-64-permuted | block | 17.112→14.829 | -11.90% | 35.006→33.008 |
| jump-64-permuted | function | 16.616→17.193 | -3.39% | 35.977→33.977 |

CALL+RET 每迭代约少4条 host 指令，JMP 少2条，符合省去重复 poll 的发码合同；cycles 中位数均下降，但不声称全部变化仅由 lookup 本身产生。jump-64-permuted Function 的真实墙钟中位数仍增加约3.47%，即使 cycles下降；全部时间分散和原始计数保留，不能用 cycles 改善替代这一耗时回退。

### Compile

统计完整既有 compile 窗口，包括初始化/映射/发布及完整 native owner 回收，只扣实际 byte parser。CoreMark20仅验证编译和CRC4983，不是正式CoreMark成绩；x87沿用退出码 oracle，不冒称完整数值验证。本轮没有重测FEX，不能宣称达到 ≤2× 的绝对目标。

| 用例 | Scope | 旧中位 ms〔min,max〕 | 新中位 ms〔min,max〕 | 新/旧 |
| --- | --- | ---: | ---: | ---: |
| call | block | 2.3674〔2.2794,2.4356〕 | 2.1376〔2.1117,2.3524〕 | 0.903× |
| call | function | 2.2048〔2.1341,2.3367〕 | 2.4374〔2.1845,2.5116〕 | 1.106× |
| coremark20 | block | 40.6056〔40.2211,74.7630〕 | 39.3661〔38.8225,73.3096〕 | 0.969× |
| coremark20 | function | 66.0311〔50.0668,67.1989〕 | 68.3541〔50.7776,83.4958〕 | 1.035× |
| x87 | block | 0.7225〔0.6965,0.8038〕 | 0.7055〔0.6729,0.8344〕 | 0.976× |
| x87 | function | 0.6619〔0.6401,0.6903〕 | 0.7446〔0.6702,0.9750〕 | 1.125× |

本轮没有证明 compile 稳定改善。CALL Function 和 x87 Function 中位数分别增加约10.55%和12.50%；范围重叠、每 arm 仅3次，不能据此断言稳定回退幅度，也不能忽略这些样本。CoreMark Function 范围很宽，不把某次调度等待当作编译工作量，更不拿它掩盖小用例的变化。

下表列出全部实际工作量集合，spill保持不变。CoreMark Block 的一条 candidate 观测多生成2个unit，IR节点36285→36341；保留这项工作差异，不将其视作严格相同编译工作的CPU比较。其它项的IR节点集合相同。新增故障记录与保护映射是真实成本；x87没有动态L1/保护映射、产物形态不变，其墙钟上升不能归因于新映射，但具体来源未经CPU诊断确认。

| 用例/Scope | IR节点：旧→新 | spill slots（前后相同） | native bytes：旧→新 | FaultSite：旧→新 |
| --- | ---: | ---: | ---: | ---: |
| call/block | 688→688 | 0 | 7080→6576 | 48→53 |
| call/function | 684→684 | 0 | 7016→6512 | 48→53 |
| coremark20/block | 36285→36285,36341 | 22 | 425408→403504,404860 | 4734→4953,4955 |
| coremark20/function | 52236→52236 | 30 | 515220→488820 | 6445→6708 |
| x87/block | 52→52 | 0 | 2136→2136 | 1→1 |
| x87/function | 51→51 | 0 | 2004→2004 | 1→1 |

直链/Function内部循环的 poll、完整 observer 对局部 flags 融合的影响仍在。本轮没有重测正式 CoreMark 的运行差距，也没有证明此前 DeferTest 资格退化和约7.8%的运行回退已经消除。

## 证据与清理

完整结果：`/Volumes/Data2/swiftvm-middle-results-relocated-20261025/l1-dispatch-interrupt-20261109`。仓库保留 [证据索引](evidence/l1-dispatch-interrupt-20261109/README.md)。最终产品、源码 SHA/补丁、ELF gzip、全部原始 stdout/stderr 与跨端内核记录保存在 Data2。

`final/` 是空队列修正及最终格式化后的唯一最终样本集。根目录的首批以及 `checkpoints/formatted-before-drain-fix/` 均作为独立历史检查点保留；未混合、替换或按门槛选择。最终冻结后未再修改产品源码，源码/产品/实测二进制一致性单独记录。

本轮手机仅使用 `/data/local/tmp/swiftvm-l1-interrupt-20261109`，测量后保留证据并回收，未改频率和其他服务；跨端临时initrd均已回收。清理结果见 `phone-cleanup-receipt.json` 和跨端两份 summary。构建继续复用固定 Data2 caches。
