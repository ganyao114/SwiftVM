# SwiftVM 与 FEX 代码生成质量差距复核

日期：2026-08-23

## 结论

SwiftVM 已经关闭 spill、NaN guard、直接链接、基础 region、NZCV 驻留和一部分
fixed-home 发布开销，但与 FEX 的剩余差距仍然主要来自三种结构性成本：

1. FEX 的 SRA 把 16 个 guest GPR、PF、AF 和 16 个 XMM 都绑定到固定 host 家，
   LoadRegister/StoreRegister 与这些家做反向亲和，成功后不发指令。SwiftVM 目前只覆盖
   部分 GPR/XMM 家和局部发布点，32 位 W 视图、跨发布快照和边界同步仍产生大量桥接。
2. SwiftVM 已把 NZCV 保留在 PSTATE，但 PF/AF 仍通过 `x26` 位域发布。FEX 用独立 GPR
   保存 PF/AF，不过当前 CoreMark 审计表明 SwiftVM 的 PF/AF 写没有可兑现的指令下界：
   AF 主要是一条必要的 clear，专用寄存器反而会增加 dispatcher/RSB recovery 成本。
3. FEX 默认 multiblock 可跨更大的前向窗口摊薄边界。SwiftVM 的 bounded-64 region 已明显
   降低 CoreMark 成本，但 SHA 等超块语料仍受函数边界、公开入口和 fault map 粒度限制。

8 月 14 日同 guest、RE=0 的历史静态 blow-up 表仍适合说明差距形状，但不能当成本轮精确
比值：之后 SwiftVM 已默认启用 FLAGS_REGS、扩大 region 窗口并连续落地两轮 fixed-home
合并。历史 SVM/FEX 比值为 CoreMark 2.305x、smallpt 2.223x、c-ray 2.423x、7-Zip
2.783x、SHA-256 3.030x；STREAM 为 0.925x，AES-GCM 为 1.213x。要刷新绝对比值，需要在
当前 FEX 与当前 SwiftVM 上重跑同一 guest PC 的静态块采集。

## 当前可复核账目

本次续轮在 Orb Linux 默认配置下禁用 JIT disk cache 和 `SVM_EXEC_PROF`，CoreMark 使用
`0x0 0x0 0x66 20000 7 1 2000`：

| 指标 | 相邻 low32 合并前 | 相邻合并后 | round-trip 后 | 同宽提取后 |
|---|---:|---:|---:|---:|
| host dynamic | 6,250,517,196 | 6,210,114,929 | 6,087,169,543 | 6,034,267,121 |
| move dynamic | 2,155,441,643 | 2,115,039,274 | 1,992,094,038 | 1,939,191,376 |
| move 占比 | 34.484% | 34.058% | 32.726% | 32.136% |
| spill dynamic | 0 | 0 | 0 | 0 |

相邻合并相对前一臂精确减少 40,402,351 条 host/move。新的 round-trip 消除再按共同
PC 的较小 entries 重算，125 个 PC 变小、5 个冷 PC 各增一条，host 净减 122,945,220，
move 净减 122,945,213；raw host 降 1.980%，spill 始终为零。两轮相对最初基线累计减少
约 163.35M host 指令。同宽 U32 提取消除再净减 52,902,689 条 common-PC host/move，
相对最初基线累计减少约 216.25M（3.46%）。

其他确定性语料的共同 PC 结果：

| workload | round-trip host | 同宽提取 host | 同宽提取变小/变大 PC |
|---|---:|---:|---:|
| STREAM | -5,854 | -512 | 86 / 2 |
| smallpt | -868,884 | -2,540,061 | 89 / 2 |
| c-ray | -1,283,611 | -402,107 | 467 / 6 |

smallpt 两臂 PPM 逐字一致，c-ray 两臂 PNG 的 IDAT 哈希一致。OpenSSL SHA 在进入有效
hashing 前于 `rip=0x62b930` 触发既有 PageFatal，因此没有拿失败路径的计数充当 SHA 性能
证据。当前 SQLite speedtest 二进制拒绝文档中的 `--size` 参数，亦不计入。

## 当前 SVM/FEX 静态比值

FEX 参照仍是同一份 `f2e35f3`、`disableavx` blockstats，因此 FEX 侧无需重编；SVM 侧在
`SVM_REGION_EDGES=0` 下重新采集，并按 W67 旧 TSV 的同 guest PC、guest instruction 数和
entries 重算。旧 PC 权重覆盖均超过 99.99997%：

| workload | 当前 SVM host/guest | FEX host/guest | 当前 SVM/FEX | 8 月 14 日 |
|---|---:|---:|---:|---:|
| CoreMark | 3.613 | 1.807 | 2.000× | 2.305× |
| STREAM | 2.161 | 3.336 | 0.648× | 0.925× |
| smallpt | 3.377 | 1.549 | 2.180× | 2.223× |

c-ray 当前运行只覆盖旧表 entries 的 74.47%，不把局部交集外推成全语料新比值。结果表明
CoreMark 的宽度桥连续优化有效地把静态差距压到约 2×，但 smallpt 基本仍是 2.18×；下一步
不应再把两者当成同一个整数宽度问题。

## 本轮优化

热点 `0x402de8` 的典型旧形态包含：

```text
lsr w8, w22, #0
lsr w6, w29, #0
mul w22, w8, w6
```

`w22` 是 guest fixed home 的 W 视图，旧值在 `BitExtract` 处最后使用，`mul` 的 U32
结果又已经被现有 W-alpha 证明可直接发布回同一个 fixed home。新合并只在以下条件同时
成立时把 `BitExtract` 的物理寄存器移交给结果家：

- 低 32 位 `BitExtract` 只有一个 U32 `Add/Sub/And/AndNot/Or/Xor/Mul` consumer；
- source 在 bridge 处最后使用，bridge 在 consumer 处最后使用；
- consumer 已由现有 fixed-home 写事务映射到同一 target，后续 `SetHostGPR` 已被证明为
  零发射发布；
- allocator 重新执行 bridge 与 consumer 的 scratch/active-register 校验；
- emitter 独立重放 source、consumer、publication、owner 和 clobber 证明。

这不是把 pinned X 误当成 high-zero X。省略 bridge 只表示 consumer 读取同一个 W view；
真正的 U32 consumer 仍会写 Wtarget，并在那一刻清零 X 的高 32 位。需要旧操作数计算 AF
的 `add` 等形态会被现有校验拒绝，本轮没有扩大 faulting producer 白名单，也没有新增开关、
日志或回退路径。

## 续轮优化

CoreMark 的剩余热点包含以下形态：

```text
lsr w6, w0, #0
mov w29, w6
```

对应 IR 是相邻的 `BitExtract(source, 0, 32) -> ZeroExtend32To64`。扩展值可能继续参与地址
计算，不能把整个链误当成单用途 `SetHostGPR` 发布。新合并只移除低 32 位 direct bridge，
保留扩展节点的一条 `mov Wdst, Wsrc` 和它的全部后续使用：

- bridge 是 U32、`lsb=0`、`bits=32`，只有一个使用且该使用就是紧邻的扩展节点；
- source 与扩展结果都在 GPR，物理寄存器不同，且两者都不属于既有 width component；
- allocator 在提交 reference mapping 前扩展 source 的 active mask，并重新验证 bridge 与
  wrapper 的 scratch 契约；
- emitter 独立重放 opcode、source id、相邻关系、共享寄存器和 active-mask 证明；
- `BitExtract` 零发射，`ZeroExtend32To64` 强制保留 W move，因此 source 不被破坏、Xdst 高位
  仍按 x86 32 位写语义清零，后续 publication/fault 时序不变。

同时补齐既有 full-width `LoadMemory` 直接发布的 emitter producer 复核；allocator 的 faulting
白名单没有扩大。宽度链证明中三处从不同临时 `GetValues()` 容器取 begin/end 的未定义行为
改为持有同一个 values 容器后遍历，长链测试不再受对象布局影响。

## round-trip 消除

剩余 BitExtract 的动态分类显示，最大的可证明冗余形态不是新的 RA width component，而是
同一 block 内的整数宽度往返：

```text
v32 -> ZeroExtend32To64 -> ... -> BitExtract(0, 32) -> U32 consumer
```

`BitExtract(ZeroExtend32To64(v32), 0, 32)` 与 `v32` 严格等价。新的
`IntegerWidthEliminationPass` 在 DCE 前把唯一的普通 consumer 直接改读原 U32 SSA；扩展和
提取在失去最后使用后由既有 DCE 删除。它不改 allocator、fixed home、fault publication 或
emitter 白名单，并严格拒绝窄提取、非 zero-extend 来源、多使用和 pseudo consumer。为避免
把短值变成 block-wide live range，只处理 128 条 IR 内的局部往返；CoreMark、STREAM、
smallpt、c-ray 的采样中没有更远的有效热候选。

该职责位于独立 pass 文件，并同时接入 block/function pipeline；没有新增环境开关、日志或
旧路径兜底。

## 同宽 U32 提取消除

第二轮把同一 pass 扩展到 `BitExtract(v32, 0, 32)`。输入可能是 U32 或 S32，但低 32 位
bit pattern 与 U32 结果完全相同；只要 consumer 明确以 W 宽读取，就可以直接改读原 SSA。
允许的 consumer 限于已复核的 U32 算术/shift/select、显式 extend、32 位 store/publication，
以及 `src_bits=32` 的 `VecFCvtIntToFloat`。

初始原型曾把相同规则泛化到 U8/U16 和 opaque consumer。固定种子全套件立即命中真实反例：
U16 `BitExtract` 向 `CallLambda` 传参时必须清理物理寄存器高位。最终实现因此拒绝所有非 U32
结果、pseudo、opaque call 和未列入白名单的 consumer；专项同时加入 U32 opaque-call 负例，
并保留既有 U16 helper 回归。收紧后失败计数恢复到前一阶段同集，CoreMark 的 52.90M 收益
全部保留。

## PF/AF 与 SHA 审计结论

- CoreMark 的 PF write 为 11,840,084 条动态指令，AF write 为 130,302,325 条；读侧接近零。
  AF 热写主要是逻辑 flags 的单条 clear，换成专用 GPR 仍需要这一条。审计测得可删动态指令
  为 0，而 dispatcher/RSBHit 的 cache/recovery 新成本为 67,754,766，因此当前专用 PF/AF
  GPR 方案判定 NO-GO。
- OpenSSL `speed -seconds 2 -evp sha256` 与 64 MiB `dgst -sha256` 都在
  `rip=0x62b930` 以 PageFatal reason 2 退出。故障发生在形成有效 SHA 热账之前；不绕过
  guest fault 语义，也不据此调整 region/fault-map 设计。

## 正确性收紧

- flags carry-test 折叠在读取 `And/Or` 参数前先验证 opcode，非预期 IR 形态直接拒绝折叠。
- region successor-cover 不再把 `CondSet`、`CondSelect`、`Adc`、`Sbb` 当成 flags-transparent；
  `SetOverflow` 也不再进入 transparent fallback。

曾验证过把 successor-cover 扩成完整 fault/AdvancePC 闸门；它使 CoreMark 从约 6.300B
回退到 6.551B（约 +4.0%），因此撤销，只保留上述精确 observer 修正。

## 验证

- Orb 全量构建通过。
- integer-width pass：2 cases / 14 assertions；连同 U16 CallLambda、low32、int-width、
  width-chain、GPR coalescing、resident-fault、W/X high-half 合跑为 9 cases / 482
  assertions，全部通过。
- 新 low32 copy 测试 6 assertions；GPR coalescing 353、width-chain 23、resident fault 21、
  W/X 高半部 17，全部通过。
- func_tests：FLAGS 0/1 × function/block/interpreter 六格均为 rc=101，checksum
  `9f52b7d59285dbe5`。
- helper-fault：38 passed，0 failed；clone futex/lock 在 FLAGS 0/1 下均 rc=0。
- 本轮 smallpt 两臂 PPM SHA-256 均为
  `fe96f7e48295b27c8df8236294052d138c3ed130b81d022739907fe6b2cde5aa`；c-ray 两臂
  PNG IDAT MD5 均为 `54256cb4b3c6313a65ea12ebb7b81e30`。
- function fingerprint 自一致性为 1664 units / 11 guests。相对本阶段精确旧二进制，
  各 guest 的 unit/decoded-block 数不变，汇总 IR 减少 1,046。
- 固定 `SWIFT_FUZZ_SEED=123456` 的 Orb 全套件：基线为 40 failed cases / 53 assertions，
  候选为 39 / 52，并新增 1 个通过的 test case。两条既有 width-chain 断言随临时容器 UB
  修复转绿；剩余差异仍是同类 VIXL 尾部反汇编自一致性抖动，没有新增语义失败类别。
- 同宽提取最终臂为 179 passed / 35 个既有 failed cases、1,047,523 / 45 assertions；与
  round-trip 阶段失败 case/assertion 数相同。泛化原型新增的 U16 helper 失败已被白名单收紧
  消除。

## 下一阶段

1. smallpt 当前仍约为 FEX 的 2.18×，而本轮整数宽度优化只减少 0.19% 左右的动态 host；
   下一轮应重做 smallpt/c-ray 的 FPR、state publication 与边界责任分解。
2. CoreMark 已降到约 FEX 2.00×；剩余 BitExtract 主要是 8/16 位真截断，不再把 raw
   BitExtract 数量当可删池，下一刀必须有 consumer 级物理高位证明。
3. SHA 必须先修复或绕开当前 guest 自身的合法 PageFatal 触发点，得到有效 hashing 热账后，
   才能判断 bounded-64、公开入口或 fault map 是否是主因。
4. PF/AF 专用 GPR 在当前 ABI 下为 NO-GO；只有出现新的 canonical park/recovery 载体，并且
   重新审计得到非零可删下界时才重开。

## 2026-08-24 补充

后续三项代码生成优化已落地：legacy scalar FPR 完整 publication、完整 NZCV merge
缩短一条、单指令 VecZip 结果直接发布到 resident home。正式 `smallpt_wh` 默认 region
从 1,321,651,162 降至 1,297,980,655 条 host 指令，累计减少 23,670,507（1.791%），
spill 仍为 0；PPM 与 FEX 逐字一致。新的同 harness RE=0 对比为 SVM 3.267832、FEX
1.549，即约 2.110×。由于 8 月 23 日表保留的 unit-formation artifact 与本次重采不同，
当前差距按 2.11–2.14× 报告，不直接混算两组绝对值。完整机制表、逐项 A/B、NO-GO
原型和验证见 `docs/codegen-fpr-flags-refresh-2026-08-24.md`。
