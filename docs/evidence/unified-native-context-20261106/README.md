# 单一 native context 基址与 x28 回收

本次快照基于 `15834ec4`，参考旧版 `660b6ba1:source/runtime/backend/context.h` 的统一 State／Uniform owner，以及 `backend/runtime.cpp` 同一 allocation 的前缀布局；保持当前独立的 guest 架构定义与前后端合同。

`NativeRunContext` 移除 state 指针，直接内嵌一个 `GuestState`。元数据占 152B，state 对齐后位于 offset 160，完整对象 1024B。Native 只常驻 `x19 = &context.state`；元数据用固定负偏移访问，Uniform 字段仍使用原 state 相对偏移。Slots/mask 仍是一条 LDP。`SvmRunNative` 只接受这个 owner；`x28` 进入 linked SSA bank，pin16 的动态标量寄存器由六个增加为七个，standalone 原 ABI 保留。

`verification-receipt.json` 与 `source-manifest.json` 固定本次 158 个活动源／汇编文件：格式检查 155/155，主机 CTest 9/9，红魔原生回归 8/8。新增的 B/F 真实寄存器压力场景覆盖第七个 live value、x28 read-before-write 复用、实际返回值、pin／context 元数据和 standalone 入口；原有 fault、cold link、FP、AVX、x87 和解释器回归沿用。

`plan.json`、`inputs.json`、`compile-records.json` 保留固定三轮、两个 workload、四个 arm 的全部 24 个 compile 样本，只排除真实 byte parser，包含实际编译与发布／回收窗口。基线 runner 对应已提交 `15834ec4`，身份见 `baseline-identity.json`。

| 完整 compile 中位数 | 基线 ms | 候选 ms | 变化 |
| --- | ---: | ---: | ---: |
| x87 Block | 0.761671 | 0.619998 | −18.6% |
| x87 Function | 0.542341 | 0.560521 | +3.4% |
| CoreMark20 Block | 39.639071 | 39.852372 | +0.5% |
| CoreMark20 Function | 48.570565 | 48.589095 | +0.04% |

三轮只是小规模回归检查，不是新一轮 FEX 锚定或最坏情况耗时保证。x87 Block 的 IR、spill、native bytes 均未变；其较低中位数不能单独证明此改动带来 18.6% 的 compile 收益。CoreMark 两种模式的 IR 和请求工作量一致，Block spill slots 24→22、最大 frame 48→32B、native bytes 382328→382280；Function spill slots 33→30、最大 frame 64→48B、native bytes 392784→392728。这里的字节变化包括寄存器分配与 context 选码，未将每个字节拆归单个机制。

设备本轮临时目录已释放 173555712 字节，独立 df 空闲增加 173604864 字节，剩余 0，无活跃写入／打开文件或清理错误。大二进制与原始日志保留在 Data2；本目录只提交少量记录。

代码提交为 `55a3d768`；已逐文件核对提交中的 158 个活动源／汇编文件与测量快照一致，关联见 `submission.json`。原始记录保留测量时的 WIP 身份。
