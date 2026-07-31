# W68：flags 极性归一化 DCE 现状核查与关闭结论

日期：2026-08-01  
基线：`5a19dcc`  
分支：`w68-flags-pol-dce`

## 1. 结论

W68 在实现前置门关闭，不新增 `SVM_FLAGS_POLARITY_DCE`。

W44 观察到的 7zip 热 unit `0x428140` 的 36 条形状，在当前 master 默认路径已被 W59 branch-only flags 完整覆盖：当前为 100 bytes / 25 条；关闭 `SVM_FLAGS_BRANCH_ONLY` 才恢复为 144 bytes / 36 条。11 条差额正是 PF、AF、carry polarity store 与 NZCV 合并物化，没有剩余可交给一个新 DCE 开关。

更重要的是，7zip 与 CoreMark 的 CFG liveness 诊断中 `reject_live=0`：所有能解析出两个后继、形状又匹配 terminal Jcc 的候选，要么已经被 W59 全部接受，要么不是 W68 所描述的“两个后继只消费部分 flags”形状。剩余 materialize 的 95.96%–98.14% 已落在 edge/fault/helper 强制边界；无法证明后继的 `reject_edge` 路径不能安全删除。

因此按任务要求“已覆盖或占比不足则直接回报并关闭”，不进入实现、正确性全矩阵或 CoreMark 新开关 A/B。

## 2. 核查方法

Orb `wine-ci`，master 对照二进制 `/tmp/svm-build/source/translator/linux/svm_translator_linux`，禁用 JIT disk cache：

- 7zip：`b -mmt1 -md=16m`，同时启用 `SVM_RA_SHAPE_PROF`、`SVM_DUMP_IR`、`SVM_DUMP_IR_POST`、`SVM_FLAGS_DEBUG`、`SVM_VIXL_HOST_DUMP`。
- 回滚对照：同一二进制、同一参数，仅加 `SVM_FLAGS_BRANCH_ONLY=0`。
- CoreMark：150,000 iterations；分别采集 RA shape flags 计数与 branch-only CFG 诊断。

7zip 两态均 rc=0；CoreMark 输出 `Correct operation validated`。

## 3. 热 unit `0x428140`

### 3.1 优化前 IR 形状

当前 master 在 flags pass 前仍可看到 W44 原始形状：

```text
@0  U8   = GetHostGPR #23
@1  U8   = GetHostGPR #1
@2  U8   = Sub @1
@3  VOID = SaveFlags @2, Flags<CF, OF, ZF, SF, AF, PF>
@4  U8   = LoadImm #1
@5  VOID = StoreUniform u[704], @4       # carry_inverted = 1
@6  VOID = AdvancePC #3
@7  VOID = BranchOnlyEdges
@8  U8   = LocalCondSet CC
If @8: LinkBlock 0x4280d0
Else:  LinkBlock 0x428145
```

W59 的当前诊断明确接受该块：

```text
[flags-branch-only] block 0x428140: ACCEPT condition=LocalCondSet
  successors=(0x4280d0,0x428145) removed=2
```

`removed=2` 即 `SaveFlags(All)` 与 polarity `StoreUniform`。主 producer 转成 `BranchOnlyFlags` pseudo，host NZCV 直接供 terminal branch。

### 3.2 当前默认 host：25 条

```text
ubfx x6, x23, #0, #8
uxtb w9, w6
lsl  w8, w1, #24
mov  w11, w9
subs w8, w8, w11, lsl #24
lsr  w8, w8, #24
b.hs ...
# 后接两个 LinkBlock/dispatcher 出口，共 18 条
```

完整 unit 为 100 bytes / 25 instructions，hash `3776ad67bd671165`。terminal `b.hs` 前没有 x26 写、PF/AF 计算或 polarity store。

### 3.3 关闭 W59 后：36 条

`SVM_FLAGS_BRANCH_ONLY=0` 时同一 unit 为 144 bytes / 36 instructions，hash `dbfc662c12dc367f`。新增的 11 条为：

```text
bfxil x26, x8, #0, #8               # PF raw byte
eor   x12, x1, x8                    # AF begin
eor   x12, x12, x11
ubfx  x12, x12, #4, #1
bfi   x26, x12, #26, #1             # AF end
mov   w6, #1                         # polarity begin
strb  w6, [x28, #1360]               # polarity end
mrs   x7, NZCV                       # packed NZCV merge begin
and   x26, x26, #0xffffffff0fffffff
and   x7, x7, #0xf0000000
orr   x26, x26, x7                   # packed NZCV merge end
```

这精确复现 W44 的 36 条形状，也证明其全部 11 条目标指令已经由 W59 删除；剩余 25 条的主要成本是两个 LinkBlock/dispatcher 出口，不在本任务允许修改的 linker 边界内。

## 4. 触发频率与全语料净账

### 4.1 7zip branch-only liveness

| 指标 | 数量 |
|---|---:|
| terminal candidates | 3,995 |
| accepted | 911 |
| reject_edge（后继不可证明） | 2,795 |
| reject_live（已知后继有部分/全部 flags live） | **0** |
| reject_shape | 289 |
| accepted 中 removed=2 | 669 |
| accepted 中 removed=3 | 242 |
| 删除 IR 节点合计 | 2,064 |

当前默认与 W59 回滚的 7zip host dump：

| 形态 | units | host bytes | host instructions |
|---|---:|---:|---:|
| 当前默认 | 6,944 | 1,110,576 | 277,644 |
| `SVM_FLAGS_BRANCH_ONLY=0` | 6,944 | 1,152,228 | 288,057 |
| 差额 | 0 | −41,652 | **−10,413** |

即 W44 候选 #2/W59 已在实际到达的 911 个 terminal unit 上吸收这类工作，平均每个接受点减少 11.43 条 host 指令；`0x428140` 是其中精确减少 11 条的代表。

flags 探针也给出一一对应的变化：

| 7zip 指标 | 当前默认 | branch-only OFF | 差额 |
|---|---:|---:|---:|
| flags units | 4,170 | 5,078 | −908 |
| PF producer/materialize | 4,379 | 5,290 | **−911** |
| AF producer/materialize | 2,448 | 3,117 | **−669** |

PF 差额精确等于 accepted 数，AF 差额精确等于 `removed=2` 的 669 个完整 AF producer，说明探针、IR 删除与 host 形状互相闭合。

### 4.2 当前剩余 materialize

| 语料 | PF materialize | PF force edge/helper/fault | 比率 | AF materialize | AF force edge/helper/fault | 比率 |
|---|---:|---:|---:|---:|---:|---:|
| 7zip | 4,379 | 3,153 + 6 + 1,045 = 4,204 | 96.00% | 2,448 | 1,654 + 4 + 691 = 2,349 | 95.96% |
| CoreMark | 1,129 | 848 + 0 + 260 = 1,108 | 98.14% | 564 | 402 + 0 + 148 = 550 | 97.52% |

这些 `force_*` 是 W67 对 source 延迟物化的保守边界计数。对本任务更直接的证据是 CFG 诊断：

- 7zip：3,995 candidates，911 accepted，`reject_live=0`。
- CoreMark：1,049 candidates，208 accepted，`reject_live=0`；accepted 删除 479 个 IR 节点（145 个 removed=2、63 个 removed=3）。

因此已知双后继路径不存在“W59 因部分 flags live 拒绝、W68 可删除其余位”的观测样本。剩余 2,795/740 个 `reject_edge` 没有可证明的双后继 liveness，涉及跨 unit、dispatcher、fault/helper 等可观察边界，不能把未知当死值。

## 5. 裁定与验收项状态

- 不新增 `SVM_FLAGS_POLARITY_DCE`；`kGetenvNames` 保持 54。
- 不改解释器、JIT、flags pass 或测试。
- OFF/ON 指纹、双平台两态全量、func_tests 六格与 CoreMark 五对 A/B 均不适用：不存在新开关或新行为，运行这些门只会比较同一 master 路径。
- 已执行的核查正确性：7zip default/branch-only rollback 均 rc=0；CoreMark 两次均 CRC validated。
- 不建议默认档位：本条直接关闭；继续使用已默认开启的 W59 branch-only flags。

工作树只新增本核查报告；未修改 `source/translator/linux/linker/`、`SwiftVM-bench/harness/run_matrix.sh` 或 golden，也未执行 commit/push/add/checkout/reset/stash。
