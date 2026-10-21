# 中间层简化重构（2026-10-21）

状态：设计已确认方向，待实施。前置文档：
`docs/middle-pipeline-redesign-2026-10-20.md`（下称原方案，其 §3
流水线形状继续有效）、`docs/compile-stack-audit-2026-10-20.md`。
本文记录原方案执行至今的实测结论，以及两点方向修正：
**plan 该删就删、一切从简；decode 前移做成绑定后端的条件
优化，后期 pass 保留为通用路径。**

## 1. 现状度量（红魔 NX729J，CoreMark 斜率/截距法）

| 指标 | SVM（当前） | SVM（会话起点） | FEX（同机自编译） |
|---|---|---|---|
| 稳态指令/iter | 1156.4k | 1658.8k | 656.5k |
| 稳态周期/iter | ~353k | — | 169.4k |
| 编译截距（host 指令） | 3.909B | 6.198B | 0.157B |
| translate 墙钟（432 unit） | 1.75s | ~2.4s | — |

编译分阶段（SVM_PROF2，当前）：decode 264ms、IR opts 307ms
（uniform+sink 约 145ms）、通用 regalloc 461ms、codegen 560ms
（其中目标规划 Complete 约 330ms：declare 121 / per-block
plans 110 / verify 90 / bind 21）、publish 46ms。采样显示
malloc/free 簇约占编译周期 17%，无单项函数热点超过 2%——
成本是弥散的，来自遍数与对象数量，不是某个算法。

关键定价实验（已落地，结论承重）：

- 关 uniform 映射（`SVM_UNIFORM_ELIM=0`，builder 仍开）：截距
  4.33B→2.70B（−37.7%），斜率 +27.6%、周期 +31%。分解：pass
  本身只占省下部分的小半，大半是 pinned IR 让 regalloc 与
  codegen 规划变贵。**成本在"规划 pinned IR 的证明机器"，
  不在"谁做映射"。**
- SB1 builder 开启后 uniform pass 的折叠数 = 0、load 映射 = 0，
  只剩 14104 次 store 映射。转发已被 decode 完全替代。
- Bind replay 实测仅 6.2ms（436 次调用中 7 次失败），删除
  Bind/Replay 本身不值风险；值钱的是它背后的整套证明分工。

## 2. 目标与非目标

目标：

- 截距 ≤2.0B（translate ≤0.95s），斜率不退超过 1%。
- `GPRBlockPlan` 子 plan 从 17 个降到 ≤5 个；生产路径删除
  Bind/Replay 与三组 verifier。
- decode 在绑定后端时完成 uniform（含 store）、常量、宽度、
  EA 的本地处理；后期 pass 保留，未绑定路径行为不变。

非目标：不改 IR/HIR 指令集与前端 lowering 形状（伪指令链删除
已完成，是唯一已批准的 IR 变更）；不改发射层结构；riscv64 与
解释器路径只做兼容适配，不做同等优化。

## 3. Plan 全量清单与处置

以 `GPRBlockPlan`（gpr_block_plan.h）与 Declare 阶段为准清点。

### 3.1 删除（证明类，随预着色落地整体消失）

| 对象 | 现职责 | 删除理由 |
|---|---|---|
| BindDeclaredGPRHomes + ReplayAllocation | 分配后绑定期望 home，不合就重分配 | 预着色后 home 是分配输入，无事后绑定 |
| VerifyFixedGPRConstraints / CheckFixedGPRConstraints | 验证 pinned 版本不被覆盖 | 预着色区间由构造保证；降为 `SVM_VERIFY_PLAN` debug 选项 |
| VerifyGPRAllocationPlan + width 证书 | 发射前全量复核 + 宽度省略证书 | 证书改为 N4 handler 的副产品（知道事实的地方直接产出） |
| FrozenGPRPlan 的 IR 签名 | 检测 plan 后 IR 被改 | 流程上 plan 后禁止改 IR，由断言点收缩为 generation 计数比较 |
| 声明存档/恢复（Save/RestoreTargetDeclarations） | Bind 的事务回滚 | 无 Bind 即无事务 |

### 3.2 合并进单遍（发射配方类，决策不变、换计算地点）

publications、load_updates/preindex/biased、addresses、memory
（writebacks/narrow_loads/offsets）、narrow_bits、funnels、
boolean_selects、consumers、integer_branch、integer_flags、
scalar_preparation、flags_tokens、scalar_fpr、constants。
共同点：都是"这条指令具体怎么发射"的局部决策，输入是分配
结果 + UnitFacts，输出是 action。合并为一遍走完的 builder，
输出一张按指令 id 索引的扁平 action 表（`UnitActions`），
存 unit arena，不再每 plan 一个 shared_ptr + map。

### 3.3 保留独立（unit 级或分配内禀）

region、backedge_flags（unit 级边事实）、fpr 分配计划（与
FPR 线性扫描同生）、pseudo_flags（已在 builder 侧增量构建）、
flags_only_spills（并入 verifier 替代物时顺带产出，或并入
N4 的 flags handler）。

### 3.4 先标价再定（复杂决策，删/留以测量为准）

induction、funnel shift、constant-address cache、narrow
compare（integer_flags 内）、boolean select 特殊形、scalar
raw-carry。这些是历史上为小幅稳态收益加的。用现有 skip 开关
逐个关掉测斜率/截距：斜率损失 <0.3% 的，**决策本身直接删**，
不搬进新结构；值钱的按 3.2 合并。标价表经确认后才是最终
删除清单——本节不预设结论。

标价结果（2026-10-21，同一轮内基线 1156.4 / 3.909B /
350.6k，关掉候选后的相对变化；正数 = 关掉变差 = 该决策
在挣钱）：

| 候选 | 开关 | Δ斜率 | Δ截距 | Δ周期 | 建议 |
|---|---|---|---|---|---|
| induction tie | SVM_INDUCT_TIE=0 | −0.01% | −0.27% | −0.61% | 删 |
| const-address cache | SVM_CONST_ADDR_CACHE=0 | −0.03% | −0.11% | −1.54% | 删 |
| funnel shift | SVM_SKIP_PREP=funnels | −0.04% | −0.05% | −0.15% | 删 |
| integer flags（含 narrow compare） | SVM_SKIP_PREP=intflags | +0.57% | −0.02% | −0.31% | 留（合并） |
| boolean select | SVM_SKIP_PREP=boolsel | +3.27% | +0.08% | +3.52% | 留（合并） |
| CFINV + raw carry | SVM_FLAGS_CFINV=0 | +4.73% | +6.17% | +5.59% | 留 |

注意：三项待删合计只值约 −0.4% 截距，删除收益不在数字
而在 N4 合并时少搬三套决策逻辑；且标价只覆盖 CoreMark
一个负载，地址/移位密集型负载上 const-address 与 funnel
可能有价值——删除前以本表为准、由人确认。周期列噪声带
约 ±1%，noconst 的 −1.54% 不单独采信，结论以斜率+截距
双向不挣钱为准。

## 4. 绑定后端与 decode 前移

### 4.1 开关与粒度

ModuleConfig 级标志（暂名 `bound_backend_decode`），默认在
`backend_isa == arm64 && enable_jit` 时开启，可用环境变量关
（沿用 svm_config 注册表规矩，记得同步 config 测试的计数）。
透传路径现成：`EnableUniformBuilder(uniform_info, features)`
已把 UniformInfo（pin 图 `uniform_regs_map`、`uni_gprs/fprs`)
和 FeatureSet 送进 Assembler。绑定额外允许 builder 使用后端
事实：立即数可编码性（arm64 imm 字段，小表）、scratch 与
clobber 约定、RCpc 可用性。未绑定时 builder 只做目标无关
折叠，与今天一致。

### 4.2 前移清单（绑定时，逐项独立成刀）

1. **uniform store 映射补完。** SB1 已做 load 转发；store 按
   pin 图直接生成 SetHostGPR/FPR。pass 保留，只收 builder
   看不见的残余：跨块 path merge、死存储的跨指令证明。
   预期 pass 的 mapped_stores 从 14104 降到只剩跨块部分。
2. **常量与恒等折叠。** builder 事实表记已知常量值，append
   时折 add/sub/logical/shift 的常量与恒等形。对应现 const
   pass 的本地部分；级联折叠留给 pass。
3. **宽度抑制。** 事实表记零/符号扩展provenance，已知结果
   事实时不生成 ZeroExtend/BitExtract（x86 的 W 写清零语义
   在事实里记账）。对应 integer-width pass 的本地部分。
4. **EA 规范化。** base+disp 在 decode 直接进 memory Operand，
   不再生成独立 EA SSA；PK7 的 planner 侧 EA 匹配在绑定模式
   下自然无的放矢（未绑定模式保留 PK7 路径）。
5. **立即数形式选择。** 有 Imm 操作数形式的 op 在操作数为
   已知常量时直接用立即数形式，省 LoadImm 物化。

明确不前移：DCE（decode 看不见未来 use）、loop hoist、
任何跨块分析、flags 的逆向需求判定（builder 只记录 demand，
生产者决策仍在后段）。

### 4.3 与后期 pass 的共存规则

沿用 SB1 已验证的模式：builder 保守（事实不足就生成原始
形式）、pass 幂等收尾（对已映射/已折叠的输入是 no-op 或
只做残余）。正确性不靠两边一致性假设，靠每刀的 host dump
等价对比兜底：绑定开/关两次编译的最终 host 代码差异只
允许"更少"（removal-only），出现新增或改写即停。

## 5. 预着色替换 Bind（删证明机器的核心一步）

UnitFacts 一遍建好每个 guest home 的驻留时间线。写 home 的
定义生成预着色区间（host 寄存器固定，跨驻留 span）；读 home
在事实证明驻留值仍有效时是固定寄存器 use，否则普通 load。
tie 家族（narrow load、width chain、memory operand、
induction）变成事实表里的硬/软 tie 边，分配时与现有
coalescing 同路消费。hoist 改成本模型单次决策（recipe 的
spill 估算来自事实表的区间端点），IR 至多改写一次、分配
恰好一次。FPR coalescing 与 const-address cache 的禁用决定
由事实在分配前算好，contract retry 消失。

发射契约不变：`GPRLocationAt`/`GPRUseHome`/`SpillReloadAt`/
`DirtyGPR` 等查询面保留语义，`GPRPlanViewFor` 改返回
UnitActions 视图。发射改动是机械改名级别。

## 6. 实施步骤与门禁

每步固定四门：全套件 627/618/9 且失败集与 DN2 基线
IDENTICAL；CRC `0x4983`@2000、`0xd340`@12000；双点斜率/
截距（斜率容忍 1%、截距只许降）；SVM_PROF2 复核目标桶。

1. **标价（只测不改）。** 逐个关 3.4 候选决策测斜率+截距，
   产出删/留表，经确认后定稿删除清单。
2. **绑定开关 + builder 扩围。** 按 4.2 清单逐项一刀；每刀
   额外过 host dump 等价对比（差异只许删除型）。
3. **UnitFacts 补完。** GuestStateMap 的 per-planner Analyze、
   宽度事实、consumer 分类收进一遍建成的 dense 表；下游
   改读事实表，删私自重算路径。
4. **预着色替换 Bind。** 最大一刀单独成步；额外列 spill
   总数与基线对比，上升 >2% 停下来商量；`SVM_VERIFY_PLAN`
   全开跑一遍套件，证明被删证明不再承重。
5. **单遍 action lowering。** 3.2 的 plan 逐个搬进一遍走完
   的 builder，`GPRBlockPlan` 收缩为 `UnitActions`；搬完
   一个删一个，不留双轨。
6. **收尾。** 全门复测 + host dump 抽查等价；本文与原方案
   §7 同步记最终数字、实际删除清单、与预期的偏差。

## 7. 风险与回滚

- 预着色改变分配形状，个别块 spill 可能上升：Step 4 的
  spill 对比门就是为此设的；超标即停，不硬推。
- 绑定模式双轨期 builder/pass 事实不一致是主要 bug 源：
  靠 dump 等价对比兜底，每刀可独立用开关关掉回滚。
- 标价结果可能显示某些复杂 plan 值 >1% 斜率：删除清单
  缩水、截距目标下调，以 Step 1 的确认结果为准。
- 每步都是原地替换且独立过门，任意一步可停在上一步的
  绿色状态；不保留长期双流水线（沿用原方案 §0 决策）。

## 8. 附录：关键证据索引

- 阶段成本与 plan 普查：SVM_PROF2 分桶（plan_declare 121 /
  plan_perblock 110 / plan_verify 90 / plan_bind 21 /
  perblock_seal 50 / builder_dead 31，单位 ms）。
- 映射定价：`SVM_UNIFORM_ELIM=0` A/B（截距 2.70B、斜率
  1500.9，盈亏平衡约 5000 iter）。
- builder 覆盖度：`SVM_FOLDCENSUS`（folded=0、
  mapped_loads=0、mapped_stores=14104）。
- Bind 现状：`SVM_BINDCENSUS`（436 调用、7 次 replay、
  target_replay 6.2ms）。
- 稳态形状：host dump 分类（mov 17.8%、RCpc 访存 9.2%、
  msr nzcv 4.5%）与 PK7 后残余模式普查（push 双用途 EA
  1656 条，手动 pre-index 因故障精确性否决，见原方案 §7）。
- 已落地切片全录：原方案 §7（Step1–5、SB0/SB1、DN1–DN3、
  PK1–PK8 含阴性结论）。

## 9. 实施进展

Step 1（2026-10-21）：标价完成，删三留三经确认（§3.4 表）。

Step 2（2026-10-21）：绑定开关 `SVM_BOUND_DECODE`（默认
arm64+JIT 开）落地。三刀：① store 全宽映射镜像 pass +
DSE 扩展覆盖 SetHost*（否则提前映射的死 store 成死发布，
dump 曾有 69 个 unit 变长；窄宽 store 仍留给 pass，否则
planner 用 bfxil 保高字节）；② 立即数折叠镜像
FoldIntegerImmediates（pass 残余 imm 3380→10；窄宽 Add/Sub
因 decode 时 NZCV 需求未知而跳过；positions 只记 LoadImm，
全量哈希曾让 decode +69ms）；③ 常量扩展/提取原地折
LoadImm（Neg 排除：它是 flags 生产者，伪操作后挂、decode
守卫无效）。结果：斜率 1156.4→1132.8（−2.0%）、周期
~353k→~339k、套件全程 IDENTICAL、dump 对比 longer=0。
pass mapped_stores 14104→5190（残余为窄宽/FPR defer/跨界）。
截距读数受自适应重编译计数耦合（bound 开时 452 vs 关时
436 次翻译），在 3.77B–4.04B 间摆动；per-unit translate
约 +2.8%（builder 检查的真实成本，待 Step 5 删 pass 时
回收）。dedup 前移经评估跳过：pass census 仅 182 次。
EA 规范化跳过：PK7 的 planner 匹配已覆盖，decode 侧需动
前端操作数构造，留待 Step 5 一并处理。

Step 3（2026-10-21，普查结论）：UnitFacts 共享面比预估
完整——BlockAnalysisIndex 已 per-block 单次构建并被
planner/builder/constraints 共用，PseudoFlagsPlan 在索引
构建时一并产出。GuestStateMap 的多实例 Analyze 经核查
不可直接共享：PrepareFixedGPRConstraints 用的是无函数级
seeding 的块本地语义（narrow probe 的 seeding_diff 计数
证明两者答案可观察地不同），planner 实例带 AnalyzeFunction
seeding。Analyze 本身廉价（惰性索引才是成本，且已 per-实例
缓存）。flags demand 存在三份计算（索引内 PseudoFlagsPlan、
Seal 的 FlagsDemand、verifier 的 spill-use），但形状各异
（packed 格式无 count 位），合并收益小。结论：Step 3 的
dense 事实表推迟到 Step 5 与 UnitActions 一并落地（事实
的最终居所在那边），本步不做无收益的所有权搬家。


Step 4a锛?026-10-21锛夛細verifier 绾璁￠檷绾т负 `SVM_VERIFY_PLAN` 寮€鍏炽€俙VerifyGPRAllocationPlan` 鐨?fixed-constraint 澶嶆銆乨ominator 鏋勫缓銆乽se-location 瀹¤浠呭湪寮€鍏虫墦寮€鏃惰繍琛岋紱鐢熶骇璺緞 `VerifyGPRWidthElisionsOnly` 鍙骇鍑哄彂 x岷?鎵块噸鐨勫搴﹁瘉涔︿笌 flags-only spill 璁″垝銆傞棬妲涙斁鍦?SealPaths 璋冪敤鐐癸紙鐩存帴璋冪敤 verifier 鐨勬祴璇曚繚鐣欏叏閲忚涔夛級锛涗袱涓粡 Seal 璺緞閽夋嫆缁濊涓虹殑娴嬭瘯鐢?ScopedVerifyPlan 鏄惧紡寮€瀹¤銆俻lan_verify 90ms鈫?1ms锛涢粯璁や笌鍏ㄥ紑濂椾欢鍧?IDENTICAL锛屾枩鐜?1132.9 涓嶅彉銆?
Step 4b锛?026-10-21锛夛細transfer 绫婚鐫€鑹插墠绉汇€俙PreparePinnedGPRValueTransfers` 鍦ㄩ厤鏂瑰凡鐭ュ鐩存帴瑙勫垝涓?Bind 绛変环鐨?fixed transfer 瀹氫箟锛坄TargetPlanning()` 闂ㄦ帶锛屼繚鎸?builder-only 璋冪敤鏂硅涔変笉鍙橈級锛孊ind 绛変环妫€鏌ラ殢鍗宠烦杩囷細鏅煡 transfer 鍙樻洿 107鈫?銆傚墿浣?Bind 鍙樻洿涓?narrow-preserved 6锛坮esidence_end 浠呮瘮 preserved 鎹曡幏鐐瑰 1锛孊indTargetDefinition 璺宠繃鏉′欢宸?1锛変笌 writer 4锛堝悗闅?induction 鍒犻櫎鍙樹负 7锛夛紝浜岃€呬负浠ｇ爜娉ㄩ噴鏄庣ず鐨?涓鸿川閲忔晠鎰忕暀鏅?鍐崇瓥锛圥repareEarlyResultHomes 瀵?wrapped Add 涓庣獎 W-clean 褰㈢姸鐨勬帓闄わ級锛氬己琛屽墠绉荤獎椹荤暀瀹氫箟缁忓疄娴嬭瘉浼紙CoreMark 宕╂簝銆佸浠?+8锛屽凡鍥為€€锛夈€侭ind/Replay 鍒犻櫎锛堝師 Step 4c锛夋殏缂擄紝replay 瀹為檯瑙﹀彂 11/452 unit銆乼arget_replay 绾?26ms銆?
Step 5a锛?026-10-21锛夛細鎵ц Step 1 纭鐨勫垹闄や笁椤癸紝閫愰」鍥涢棬銆俧unnel shift 鏁村垹锛坧lan+鍙戝皠鏂囦欢+5 涓祴璇曪紝622 渚?IDENTICAL锛夛紱const-address cache 鏁村垹锛堝垎閰嶅櫒閿氱偣+ConstantAddressPlan+retry bit+feature/env 涓ょ嚎锛宬FeatureCount 71鈫?0銆乪nv 171鈫?70锛?16 渚?IDENTICAL锛夛紱induction 鏁村垹锛坅llocator tie+InductionPlan+Config/feature/env 涓夌嚎锛?9/169锛?10 渚?IDENTICAL锛夈€備笁椤瑰悎璁℃枩鐜?1132.6锛堜笌鍒犻櫎鍓嶄竴鑷达紝绗﹀悎鏍囦环棰勬湡锛夈€?
Step 5b 绗竴鍒€锛?026-10-21锛屄?.1锛夛細FrozenGPRPlan 鐨?IR 绛惧悕鏀剁缉涓?generation 璁℃暟銆傚垹闄?frozen_ir 鎹曡幏锛圴isitIRSignature 鏁翠綋绉婚櫎锛宑tor 鏀规櫘閫氭寚浠ゅ惊鐜紝operands/scratch 鎹曡幏涓嶅彉锛夈€丆aptureIRSignature/MatchesIRSignature 闈欐€?API銆乺egion 璇佷功鐨?unallocated_ir锛圡atchesUnit 宸叉湁 plan 绾?generation 闂級锛汳atchesFrozenIR 鏀逛负 MatchesAllocation銆傞拤"plan 鍚庢敼 IR 蹇呴』琚嫆"鐨勬祴璇曟寜鏂板绾︽敼鍐欐垨鍒犻櫎锛坰tale 瑙﹀彂鏀逛负 allocation generation 鎺ㄨ繘锛夈€俻erblock_seal 51鈫?0ms锛宼ranslate 1737鈫?669ms锛屾埅璺濇寚浠?6.06B鈫?.89B@2000锛?07 渚?IDENTICAL锛孲VM_VERIFY_PLAN=1 IDENTICAL锛孲VM_BOUND_DECODE=0 鏃?CRC 鍙屽銆佹枩鐜?1134.5銆?
浜嬫晠璁板綍锛?026-10-21锛夛細绛惧悕鍒犻櫎鑴氭湰閿氱偣鍛戒腑 VisitIRSignature 鍓嶅悜澹版槑锛岃鍒?pinned_gpr_allocation.cpp 涓讳綋锛堣鏂囦欢涓讳綋涓烘湭鎻愪氦鍦ㄧ爺浠ｇ爜锛実it 鏃犳仮澶嶇偣锛夈€傚凡浠庢湰浼氳瘽鏃ュ織鐨?read_file 璁板綍閫愯鎭㈠ 530/536 琛屽苟淇涓夊鐗堟湰鎷兼帴缂濓紙audit_clobbers 璁＄畻銆丗indScratchFor銆佸熬閮ㄥ嚱鏁扮粍鍧囦互鏃ュ織鍘熸枃琛ュ洖锛夛紝鎭㈠鍚庡浠?IDENTICAL + CRC/鏂滅巼涓庤鍒犲墠涓€鑷淬€傛暀璁細span 鍒犻櫎蹇呴』浣跨敤甯﹀嚱鏁颁綋 `{` 鐨勫畾涔夐敋鐐瑰苟鍦ㄥ垹闄ゅ悗鍏堟煡琛屾暟/绗﹀彿琛ㄣ€?
褰撳墠鏁板瓧锛堢孩榄?bf1359ff锛?10鈫?07 渚嬶級锛氭枩鐜?1132.5 kins/iter銆佸懆鏈熺害 339k/iter銆乼ranslate 绾?1669ms@2000銆佹埅璺濇寚浠ょ害 5.89B銆傚 FEX锛堝悓鏈鸿嚜寤猴細656.5 / 169.4k / 0.157B锛夊樊璺濈害 1.72脳 鎸囦护銆?.0脳 鍛ㄦ湡銆佺紪璇戞埅璺濈害 24脳銆傛湭绔熼」锛歋tep 4c锛圔ind/Replay 鍒犻櫎锛屽緟 narrow/writer 鐨勫垎閰嶅墠鍐崇瓥璐ㄩ噺绛変环鏂规锛夈€丼tep 5b 鍏ㄩ噺锛埪?.2 瀛?plan 閫愪釜鎼繘鍗曢亶 builder 杈撳嚭 UnitActions锛涘綋鍓嶆渶澶ф《 pass_uniform 149ms鈥斺€攂uilder 宸叉槧灏勫叏閮?loads銆乸ass 浠嶆槧灏?5396 stores锛屾暣璺充笉鍙锛岄渶 per-block 娈嬩綑浜嬪疄锛夈€?
Step 5b 续探（2026-10-21，阴性）：uniform pass 残余跳过实测无效。给 forward 加"无 Load/StoreUniform 整跳、仅保留 DSE"后 pass_uniform 147.3ms 不变（forward 78.2/dse 49.3）——bound decode 后几乎每块仍含至少一条原始 uniform 操作（未映射偏移的 loads 与窄/FPR stores），per-block 跳过不成立；已回退。后续若攻此桶，应从 forward 内部下手（fact 表与 RangeMap 查询），或先扩大 builder 的 load 映射覆盖。

稳态构成复测（2026-10-21，检查点 2a611ae 后）：静态 host dump 普查（449 units/77863 条）mov 18.5%（reg,reg 10.5%）、分支 16.5%、msr+mrs nzcv 6.2%、RCpc 访存约 7.9%。动态计数（SVM_EXEC_PROF，2000 iter）：exits 144.9M（约 72k/iter，direct 89%）、region_edges 114.6M、cycle_polls 38.5M、fallthroughs 30.2M、gpr_uniform 10.4M、dispatch 仅 451。斜率 1132.5k/iter 对 72k exits/iter 约 15.6 条/exit——块粒度细、边界序列是结构性大头；direct-link 仅 linked 472/1345 sites。后续斜率工作应优先降 exit 数（区域增大/链接率）或 per-exit 序列长度，而非单条指令微优化。
