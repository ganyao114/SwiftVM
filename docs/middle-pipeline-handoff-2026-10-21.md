# 中间流水线重构 Handoff（2026-10-21）

本文是给接手者的完整上下文：为什么改、改了什么、数字在哪、还剩什么、
从哪下手、有哪些坑。设计细节以文末文档地图为准，本文只做汇总与索引。

## 0. 一句话状态

编译中间流水线（decode → IR passes → 寄存器分配 → plan 层 → 发射）已完成
两轮瘦身：绑定后端 decode 期优化落地、三个过度复杂机制整删、verifier
审计降级为开关、FrozenGPRPlan 的 IR 签名删除。当前树是绿色检查点
（提交见 §8），套件 607 例与 DN2 基线失败集 IDENTICAL。未竟两件大事：
(1) 删 Bind/Replay（需先做 Declare 时刻定点 recolor 事务，设计已定，见 §6）；
(2) Step 5b 单遍 action lowering（UnitActions，未开工）。

## 1. 背景与目标

SVM（x86-64 → ARM64 JIT）与 FEX 的差距分解为三块：生成代码质量（稳态
斜率）、host 编译速度（截距）、初始化。中间流水线的 plan 层历史膨胀到
15+ 个 per-block plan + Bind/Replay 证明机器，是编译慢与维护成本的主因。
重构总方针（用户定）：保留 IR/HIR 不动；一切从简，复杂 plan 该删就删；
能在 decode 阶段（绑定后端时）提前做的优化提前做，后置 pass 保留作残余；
优化/分配逻辑只许在 RegAlloc/planning 层，发射层不许长分配逻辑。

## 2. 环境与门禁（照抄即可）

- 设备：红魔 NX729J，adb 序列号 `bf1359ff`（**不要用** Y700）。
  adb 路径 `C:\Program Files\Software Fix\adb.exe`，永远带 `-s bf1359ff`。
- 构建目录：`build-android-arm-opt-r1`（Android 构建，不要 chroot）。
  JDK17：`E:\Project\VMEngine\tools\vmengine-tools\jdk-17\bin` 先入 PATH。
  NDK：`E:/Project/VMEngine/tools/vmengine-tools/android-sdk/ndk/29.0.14206865`。
  构建命令（最多 -j3）：
  `cmake --build build-android-arm-opt-r1 --target swift_test svm_translator_linux -j 3`
- 部署：NDK `llvm-strip.exe` strip 后 push 到 `/data/local/tmp/`，改名带后缀
  （如 `swift_test_s5f`）避免覆盖基线二进制。设备测试一律 `taskset 10`。
- 基准：`coremark-O2-x86_64`（设备 /data/local/tmp 已有），参数固定
  `0x0 0x0 0x66 <iters> 7 1 2000`。双点斜率 iters=2000/12000，
  `simpleperf stat -e cpu-cycles,instructions`。CRC 门：`0x4983`@2000、
  `0xd340`@12000，错一个即停。
- 套件门：`swift_test` 全跑，与 DN2 基线日志比失败"用例名集合"
  （脚本 %TEMP%\failset_compare2.py，逻辑：解析 Catch2 失败块用例名求差集；
  注意 v1 有假 IDENTICAL bug，必须用 v2）。基线为 627 例/9 失败/67 断言；
  经三轮删除后当前 607 例/9 失败/67 断言，失败集须 IDENTICAL。
- 附加门（按需）：`SVM_VERIFY_PLAN=1` 全套件（审计路径）、
  `SVM_BOUND_DECODE=0` CRC+斜率（绑定关路径）、`SVM_BINDCENSUS=1`（Bind 普查）、
  `SVM_PROF2=1`（编译桶）、`SVM_VIXL_HOST_DUMP=1`（host 代码 dump，
  配合 %TEMP%\host_census_now.py 用 Capstone 分类）。
- 设备脚本一律写成 .sh 文件 push 执行，不要在 PowerShell 里拼带 `$` 的
  内联 shell（变量会被 PS 先展开，已踩坑）。

## 3. 当前数字（红魔，检查点 2a611ae）

| 指标 | SVM 当前 | SVM 本轮起点 | FEX（同机自建） |
|---|---|---|---|
| 斜率（kins/iter） | 1132.5 | 1658.8（会话初）/1156.4（Step 前） | 656.5 |
| 周期（k/iter） | ~339 | — | 169.4 |
| 截距指令 @2000 | ~5.89B | 6.198B | 0.157B |
| translate_ns@2000 | ~1669ms | — | — |

差距：稳态指令 1.72×、周期 2.0×、编译截距约 24×（截距受自适应重编译
计数耦合，跨轮比较用 per-unit 与固定 iters 双点斜率更可靠）。
编译桶（SVM_PROF2）：decode ~300ms、opt ~320（uniform 149 最大）、
regalloc ~450、codegen complete ~285（declare 119 / perblock 85 /
bind 34 / verify 20）、body+terminal+prologue ~150。
稳态构成：mov 18.5%、分支 16.5%、msr+mrs nzcv 6.2%、RCpc 访存 ~7.9%；
动态：72k exits/iter（direct 89%）、cycle polls 19.2k/iter、
direct-link 仅 472/1345 sites linked。块粒度细是结构性大头。

## 4. 已完成（按时间线，细节见 §8 文档）

前置轮（2026-10-19/20，已并入检查点）：pseudo-operation 链删除；
SB1 decode 期 uniform load 转发默认开；DN2/DN3 latch-only 轮询；
PK1-PK7 系列（store-sink dirty bytes、verifier dense owners、fixed 约束
二分、small-vector/dedup、REF 链扁平化、EA imm9 折叠）。
斜率 1658.8→1156.4、截距 6.198B→3.909B（当时口径）。

本轮（simplification 文档 Steps 1-6）：
- Step 1 标价：逐个关候选决策测斜率/截距。结论删三留三（用户确认）：
  删 induction tie（Δ−0.01%）、const-address cache（−0.03%）、
  funnel shift（−0.04%）；留 integer flags（+0.57%）、boolean select
  （+3.27%）、CFINV/raw-carry（+4.73%）。
- Step 2 绑定开关 `SVM_BOUND_DECODE`（默认开）：builder 全宽 store 映射
  （窄 store 必须留给 pass，否则 planner 出 bfxil 保高字节）、DSE 覆盖
  SetHost*（仅全宽 victim）、立即数折叠（窄 Add/Sub 跳过，NZCV 需求
  decode 时未知）、常量 extend/extract 折叠（Neg 排除，是 flags 生产者）。
  每刀过绑定开/关 host dump 等价对比（只许 removal-only 差异）。
- Step 3 普查关闭：共享事实多数已在 UnitFacts；GuestStateMap 多实例
  播种语义可观测地不同，不可 naive 合并；dense facts 并入 Step 5。
- Step 4a：`VerifyGPRAllocationPlan` 纯审计部分（fixed 复检/dominator/
  use-location）降级为 `SVM_VERIFY_PLAN` 开关，门槛在 SealPaths 调用点
  （target_allocation_planner.cpp），生产走 `VerifyGPRWidthElisionsOnly`
  只产宽度证书+flags-only spill。plan_verify 90→21ms。
- Step 4b：transfer 类预着色前移——PreparePinnedGPRValueTransfers 在配方
  处直接 PlanFixedGPRDefinition（TargetPlanning 门控），Bind 普查
  transfer 变更 107→0。
- Step 5a：删三项整删（文件+配置三线+测试）：funnel（622 例）、
  const-address（FeatureCount 71→70、env 171→170；616 例）、
  induction（70→69、170→169；610 例）。斜率不变，符合标价。
- Step 5b 第一刀：FrozenGPRPlan 的 IR 签名（每指令 ~20 token）整删，
  MatchesFrozenIR 收缩为 allocation generation+attempt 比较；region
  证书 unallocated_ir 同删。perblock_seal 51→30ms，translate −68ms。
  钉"plan 后改 IR 必须被拒"的测试已按新契约改写/删除（stale 触发改为
  ResetAllocations）。
- Step 6：三路全门（默认 / VERIFY_PLAN=1 / BOUND_DECODE=0）通过。

## 5. 事故记录（接手者必读）

2026-10-21 签名删除时，编辑脚本的 span 锚点命中 `VisitIRSignature` 的
前向声明，误删 `source/runtime/backend/arm64/alloc/pinned_gpr_allocation.cpp`
主体（536 行→85 行）。该文件主体是未提交在研代码，git 无恢复点。
已从会话日志的 read_file 记录逐行恢复 530/536 行，三处版本拼接缝
（audit_clobbers 计算、FindScratchFor/ScratchFor、文件尾函数组）以日志
原文补回，恢复后套件 IDENTICAL、CRC/斜率与误删前一致，并已逐区 review。
仍建议接手者对该文件做一次独立 review（重点：Prepare 头部、
BuildInstructionScratch、FrozenGPRPlan ctor 的 operands/scratch 捕获）。
教训已固化为工作习惯：span 删除必须用带函数体 `{` 的定义锚点，删后
先查行数与符号表；大文件编辑走 %TEMP% 下 Python 脚本 + 精确计数断言，
注意仓库内 CRLF/LF 混合（同一文件两种行尾都可能有，锚点要双试）。

另：历史上曾有 `git checkout --` 毁掉在研代码的事故。本仓库长期带
大量未提交在研改动是常态——永远不要对脏文件做 checkout/reset/clean，
回退单个实验只许回退自己刚改且已提交检查点的文件。

## 6. 剩余工作（按建议顺序，含下手点）

### 6.1 根治 B：Declare 时刻定点 recolor 事务（删 Bind 的前置，多日级）

背景结论（全部实测过，勿重走）：
- Bind 三类变更：transfer 已清零（4b）；narrow-preserved 6 个；
  writer 7 个。Bind 失败→Replay 实际触发 11/452 unit、约 26ms。
- writer 本质是 copy 配方的 transferred-use 生产者（Add×5 spilled、
  SelectZero×1 等），Bind 时刻 placed 率 0/7。早规划 uses==1 门必拒
  （普查：uses 拒绝 4640 次为各门之首）。
- 硬预着色两路皆否：自建软偏好通道 3170 次尝试 100% 被 active 拒绝
  （guest home 恒被固定区间占用）；复用 fixed_gpr_affinity 解耦到普通
  模式与 width-chain/low32 正面耦合（ReproveLow32Copy abort）。
- FixedGPRDefinition 字段两套语义（publication/end 为主）见
  simplification 文档 §9 的方向 1 设计条目，此处不重复。

施工设计（已定，未开工）：
1. RegAlloc 加单值 recolor 原语：改 mapping（MEM/GPR→目标 home）、
   清该值 spill_reloads 段、置 gpr_home_intervals_dirty 与
   gpr_preservation_ranges_dirty、处理 fixed_gpr 标志。要害是枚举
   所有缓存了旧 mapping 的结构（spill_reloads、home intervals、
   preservation ranges、width 证书尚未产出故无碍）。
2. copy 配方接入：MatchPinnedGPRCopy 成功后、AssignValueHome 前，
   若 producer 未在 target：以 [producer, last_transferred_use] 做
   GPRTargetWriteConflict + PublicationWindowSafe + FixedGPRClobbers
   检查，通过则 recolor（含 extend 链整链同寄存器，保 low32/width
   的 SharesGPR 前提），不过则放弃配方走普通发表。select/spilled
   配方同理（spilled 在 recolor 后不再 spilled，注意其 matcher 的
   MEM 前提要改判定时机）。
3. 每步用 SVM_BINDCENSUS 验证 writer_changed 与 fail_writer 同步下降，
   spill 总数对比基线（文档门：+2% 停），斜率容忍 1%。

### 6.2 根治 C：narrow-preserved 6 个清零

形状：DeclareCaptureVersions 给窄读规划 preserved 捕获（end=transfer=
读点），copy 配方又给紧邻的下一条消费者登记 selected use home，
Bind 算出 residence_end=val+1 > end=val，差 1 必重规划→replay。
候选修法（未验证）：copy 的 RegisterUseHome 对 preserved 值不登记
越过 transfer 点的 use（该消费者改读 displaced 副本，copy-out 本已
在 transfer 点发出）；或 DeclareCaptureVersions 在首个消费者紧邻且
home 存活时把 transfer 推到该消费者。注意 preservation 动作的
执行点相对 transfer 指令的先后（BuildGPRPreservationActions）。

### 6.3 根治 D：删 Bind/Replay

6.1+6.2 后 Bind 普查 changed_calls 应为 0，此时删：
BindDeclaredGPRHomes、BindTargetDefinition、Complete() 中 Bind 失败的
ReplayAllocation 分支（FPR coalescing 的 contract retry 是独立机制，
保留）、SaveTargetDeclarations/RestoreTargetDeclarations 及
TargetDeclarations 存档、RegAlloc 的 target_fixed_undo/target_publications_undo
中仅为 Bind 服务的部分。FixedGPRConstraintsHold 保留为
SVM_VERIFY_PLAN 审计路径（VerifyGPRAllocationPlan 内已调用）。

### 6.4 Step 5b 全量：单遍 action lowering

GPRBlockPlan 现有 11 个 shared_ptr 子 plan + 6 个 map（publications/
load_updates/preindex/biased/addresses/memory/narrow_bits/boolean_selects/
consumers/integer_branch/integer_flags/scalar_preparation/scalar_fpr/
flags_tokens、flags_only_spills、pinned）。目标：一遍 builder 输出按指令
id 索引的扁平 UnitActions（存 unit arena），逐个子 plan 搬完即删不留双轨。
已知最大桶 pass_uniform 149ms：builder 已映射全部 loads，pass 仍映射
5396 stores；per-block 整跳实测无效（几乎每块仍有原始 uniform 操作），
需 per-block 残余事实或从 forward 内部（fact 表/RangeMap 查询）优化。

### 6.5 稳态斜率

优先级判断：降 exit 数（72k/iter，direct-link 472/1345）> 缩 per-exit
序列（latch poll 已是 DN3 的 latch-only 形态）> 单指令微优化。
动手前先做动态热点定位（simpleperf record 的匿名 JIT 地址需与
SVM_VIXL_HOST_DUMP 的 unit 字节做地址相关，尚无现成脚本）。

## 7. 关键文件与开关索引

- 绑定 decode：`source/runtime/frontend/ir_assembler.{h,cpp}`
  （EnableUniformBuilder/BoundFoldImmediate/BoundFoldExtend/uniform_bound）、
  `source/translator/x86/translator.cpp`（BoundBackendDecode）、
  `source/runtime/ir/opts/uniform_elimination_pass.cpp`（残余 pass）。
- 配置：`source/runtime/common/svm_config.{h,cpp}`（SVM_BOUND_DECODE /
  SVM_VERIFY_PLAN / SVM_BUILDER_UNIFORM 等；新增 env 必须同步
  `source/tests/cases/config.cpp` 的计数与 back 名，现为 169/69）。
- 分配与 plan：`source/runtime/backend/arm64/alloc/` 下
  target_allocation_planner.cpp（Declare/Bind/Seal 总装）、
  fixed_gpr_constraints.cpp（分配前 fixed 规划+checker）、
  pinned_gpr_*.cpp（各配方）、gpr_plan_verifier.cpp（审计/width-only 分流）、
  frozen_gpr_plan.h、gpr_block_plan.h（UnitActions 的前身）。
- 分配器：`source/runtime/ir/opts/linear_scan_allocator.h`（tie 家族、
  fixed affinity——注意 gated 于 fixed_class/W-gamma、width chains）、
  register_alloc_coalesce_gpr.cpp（width 组件所有权事务）。
- 普查开关（env 门控、默认惰性、可保留）：SVM_BINDCENSUS（Bind+early
  两组计数、clause/narrow 子普查）、SVM_FOLDCENSUS、SVM_PROF2、
  SVM_VIXL_HOST_DUMP、SVM_EXEC_PROF、SVM_SKIP_PREP（逗号名单关配方，
  标价用）、SVM_RA_DIAG。

## 8. 文档与提交地图

- `docs/bind-replay-handoff-2026-10-19.md`：最初的 Bind/Replay 问题陈述。
- `docs/compile-stack-audit-2026-10-20.md`：编译全栈审计。
- `docs/middle-pipeline-redesign-2026-10-20.md`：整体重构方案（§7 为准）。
- `docs/middle-pipeline-simplification-2026-10-21.md`：简化执行文档，
  §9 是逐刀实测记录（标价表、每步数字、阴性结果、事故、设计修正），
  接手先读 §3 与 §9。
- 本分支提交（master 之上）：`2a611ae` 检查点（347 文件，含全部在研
  代码与上述四份文档）→ `60faa3c`/`fb2497b`/`0c709fc`/`20f2fc9`/
  `d35a54e`/`885ce04`/`dff09c4` 均为 docs 记录 → 本 handoff。
  其余 ~410 个未跟踪文件是 build 目录与更早的 docs，未纳入版本控制，
  与本重构无关，勿删。

## 9. 待-owner 决策（已部分表态）

- wrapped-Add 前移取舍：owner 已确认接受（方向 1）。但实测表明正确
  形态是 6.1 的 recolor 事务而非硬预着色，取舍对象随之变为"recolor
  失败时配方降级为普通发表"的局部质量损失，量级以施工时普查为准。
- 斜率方向（降 exit 数 vs 缩 per-exit 序列）未表态，见 6.5。
- ra_fixed_class（W-gamma）是否作为长期模式启用未表态；affinity 机制
  与之耦合，6.1 施工时勿顺手打开。

