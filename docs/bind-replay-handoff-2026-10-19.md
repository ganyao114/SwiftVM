# Bind 阶段跳过重播（ReplayAllocation）优化 — Handoff

日期：2026-10-19
仓库：`E:\Project\SwiftVM`（分支 master，工作树有大量未提交改动——这是常态，**不要 commit / stash / 清理，不要动无关改动**）

## 一句话结论

朴素的「当前分配已满足就跳过重播」不成立：实测变更绑定里字面已满足的是 **0 条**；实验性跳过（仅限最干净的一类）导致 swift_test 22 个用例失败。下一步不是直接实现跳过，而是先判定失败的那条不变量的性质，再选择健全的设计（最有希望的方向见 §6 的 A/B）。

## 背景与问题

函数级编译的固定成本是 SwiftVM 对 FEX 的主要劣势之一。分段计时（chroot 内 `svm_translator_linux` 编译 CoreMark）显示，目标规划阶段 `ARMTargetPlanner::BindDeclaredGPRHomes` 的成本几乎全在它尾部的 `allocation.ReplayAllocation()`——每次 Bind 只要有任何绑定「变更」，就把整个寄存器分配（RunVerified：收集活跃区间→线性扫描→合并→验证）重跑一遍：

- 300 次 Bind 调用中 203 次触发重播；
- 重播耗时约 119–142ms（不同插桩轮次有波动），Bind 自身的 facts/bind 计算只有个位数 ms。

目标问题：能不能在「当前分配已经满足这些绑定」时跳过重播？

## 普查结果（CoreMark -O2 静态 guest，300 次 Bind 调用口径，计数器为非原子静态变量，数值有 ±几条的轮次波动）

- `BindTargetDefinition` 候选 1141 条，**全部变更**（unchanged=0），且**全部**是 narrow `GetHostGPR`（返回值 ≤4 字节）的 low-view 绑定（selected 来自 `home.low`）；writer 类、GetOperand 类均为 0。
- 当前普通映射：1141 条**全部**是动态捕获寄存器（样本清一色 x9/x10），没有一条在目标 guest home 上；`GPRLocationAt(..., Definition)` 与 `GPRLocationAt(..., Value)` 在使用点也都不报 target home。也就是说按「字面已满足」口径，答案是 **0%**。
- 宽松口径（把 narrow 绑定视为已证明的 guest-home 读取驻留）：1114 条无固定区间冲突、无普通值占用目标 home、无外部使用；其中 1096 条 residence 覆盖全部指令使用。阻断项：固定区间冲突 27 条；squatter / unknown_use / no_uses 均为 0。
  - 注意：`residence_end` 的算法部分是同义反复（默认取 `requested_low == selected`，除非 selected use-home 改口），不能单独当作证明。
- 按调用计：203 次变更调用 = 153 次只有 BindTarget 变更 + 44 次两者都有 + 6 次只有 transfer。若 narrow 全覆盖类可安全跳过且不含 transfer，上限是 134/203 次调用可省重播。
- Transfer 绑定（U64 `GetHostGPR` 的 whole/begin>value 分支，单独 `PlanFixedGPRDefinition`）：98 条全部变更、exact 命中 0 条；**尚未做细目普查**，是独立子问题。

采样形态（前 60 条变更绑定一致）：`cat=narrow, src=low, kind=GPR, map=x9/x10, last_use ≈ id+1 或 id+2`。机制是：这些窄读在 pass1 里是被「捕获」到动态寄存器的普通 SSA 值（多半还带着 preserved 固定定义，后缀走普通映射）；Bind 要把它们改成 elided 的 guest-home 固定定义，重播让线性扫描从一开始就按预分配固定值处理（不占捕获区间、合并/验证都看到固定定义）。这就是重播在做实事、而非纯验证的原因。

## 跳过实验结果（仅 chroot 插桩副本，未进生产代码）

实验谓词：整次 Bind 调用中所有变更绑定均为 narrow_ok_full（narrow+low、无固定冲突、无占用、无外部使用、residence 覆盖全部使用）且无 transfer 时，跳过 `ReplayAllocation()` 与 `RestoreTargetDeclarations()`。

结果：**swift_test 612 个用例 590 通过、22 失败**（断言 1,107,440 中 23 个失败）；CoreMark 未跑（脚本在测试失败时退出）。失败几乎全部是同一条检查：`allocation.ValueGPR(ir::Value{&inst}).id == fixed->home`，出现于：

- `source/tests/pinned_gpr_read_test.cpp`（311/317/328/336/343/358/363/436/455/505）
- `source/tests/flags_merge_test.cpp:143`
- `source/tests/spill_forwarding_test.cpp:734`
- `source/tests/fuzz/sse42str_test.cpp`（432/943）、`source/tests/fuzz/userland_ext_test.cpp`（147/371）

## 下一位工人要做的事（按顺序）

1. **判定不变量性质**：grep 上述检查字符串，追到它的辅助函数与语义。回答：elided 固定定义是否真的要求普通映射 `ValueGPR == home`？发射器、溢出/重载、dirty mask、`VerifyFixedGPRConstraints` / `VerifyGPRAllocationPlan` 中谁会消费 elided 值的普通映射？给出代码证据链。
2. **单例追踪**：挑一个失败的 narrow 读，dump Bind 前/后、重播后三组状态（fixed def、placement(result/low/whole)、alloc_result、三种 `GPRView` 的 `GPRLocationAt`)，解释重播到底改变了哪些状态、哪些是后续阶段真正依赖的。也可顺带试「跳过 Replay 但仍调用 Restore」变体做隔离（预期无差别，请验证）。
3. **选设计**（候选，按推荐度）：
   - A. 把 narrow low 绑定前移到 pass1 之前的固定约束准备（`fixed_gpr_constraints.cpp` 一侧），让第一次分配就满足，Bind 自然无变更。需确认 PrepareNarrowReads 的证明在分配前是否成立。
   - B. Bind 内定向重映射：谓词成立时把捕获值的普通映射改写到 home，并逐项核对副作用（区间占用、dirty、spill reload、合并/width 状态），代替全量重播。
   - C. 证明后放宽不变量：仅当证明 elided 定义的普通映射不被任何消费者使用，才改检查与验证器；失败模式必须是断言而非静默误编译。
   - D. 保留重播，只优化重播本身（缓存活跃区间等）。上限较小但风险最低：collect 两遍合计约 44ms，省一遍 ≤ ~22ms；注意固定定义会改变区间收集对预分配值的处理，缓存要处理失效。
   - Transfer 绑定单独普查后再决定，不要顺手一起跳。
4. **实现与测试**：生产代码只改 `E:\Project\SwiftVM`；不要把 chroot 里的插桩搬回来。先写能复现跳过失败的回归测试再改。建议覆盖：带 preserved 捕获的 narrow 读、residence_end 之后还有使用的读、同 home 固定区间冲突、构造出的普通值占用、terminal 使用、函数多块跨块使用、Bind 在 Complete 重试回路中第二次被调用的幂等性。
5. **验收**：先跑上列失败测试文件，再跑全量 swift_test（612 用例）；CoreMark -O2 的 `crcfinal` 必须为 `0x988c`；报告新的 bindphase/replay 计时与改动前同口径对比。若分配行为有变（A/B 方向），还要对比稳态执行（宿主指令数/代码体积或 PMU 斜率），不能只看编译时间。旧轮次的成功结果不得给改过的代码背书；需要设备侧证据时按仓库既有 epoch 流程走。

## 关键代码指针（宿主仓库，行号以当前工作树为准、用前自查）

- `source/runtime/backend/arm64/alloc/target_allocation_planner.cpp`：`DeclareGPRBlock`（含 `gpr_builder.PrepareNarrowReads`）、`BindDeclaredGPRHomes`（BindingFacts、两类绑定的准入与 last 计算）、`Complete`（Bind 调用点及 FPR/常量被拒后的 Replay+重 Declare+重 Bind 回路）。
- `source/runtime/backend/reg_alloc_gpr_locations.cpp`：`SaveTargetDeclarations` / `RestoreTargetDeclarations`、`BindTargetDefinition`（unchanged 判据：旧固定定义同 home、同 elided 且 end 覆盖）、`ReplayAllocation`、`PlanFixedGPRDefinition`、`GPRTargetWriteConflict`（只查固定区间，不查普通分配）、`GPRLocationAt`（注意三种 view 对 low_view 的不同处理，及 preserved 定义的短路分支）。
- `source/runtime/backend/reg_alloc.h`：`TargetHome`/`TargetDeclarations`、`FixedGPRDefinition`、`GPRPlacement`、`Map`。
- `source/runtime/ir/opts/linear_scan_allocator.h`：`RunVerified`、`AllocateRegisters`、`CollectLiveIntervals`、`MapPreallocatedGPR`、固定寄存器试分配与合并诸 pass。
- `source/runtime/backend/arm64/alloc/fixed_gpr_constraints.cpp`、`gpr_plan_verifier.*`、guest_state_map / gpr_builder（自行 grep 文件名）。

## 实验环境与现状（复现用）

- 设备：Y700 Gen4 平板（TB322FC，adb serial `8BF1BCC0`；adb 在 `C:\Program Files\Software Fix\adb.exe`，不在 PATH）。Ubuntu 24.04 arm64 chroot：`/data/local/tmp/fexbench/`，**只能用**现成的 `enter.sh` 进入（它自带 `unshare -m` 隔离；历史上从普通 su 会话 mount 过导致 Binder 事故，切勿再犯）。`work/` 在 chroot 内是 `/work`。
- 推送+运行模式：`adb -s 8BF1BCC0 push <脚本> /data/local/tmp/fexbench/work/`，然后 `adb -s 8BF1BCC0 shell su -c "/data/local/tmp/fexbench/enter.sh /bin/bash /work/<runner>.sh"`。
- chroot 源码：`/work/swiftvm`，构建目录 `/work/swiftvm/build-bench`，guest：`/work/guests/coremark-O2-x86_64`，运行口径 `svm_translator_linux <guest> 0x0 0x0 0x66 100 7 1 2000`。
- ⚠️ chroot 里的 `target_allocation_planner.cpp` 现在叠了 bindphase + bindcensus 三层 + bindskip 实验四层插桩（补丁脚本在会话 scratchpad：`patch_bindcensus*.py`、`patch_bindskip_exp.py` 及对应 runner；同目录有 `.pre-bindcensus` 备份，是只有 bindphase 的状态）。要恢复干净基线请从宿主 `E:\Project\SwiftVM` 同路径同步，但同步单个文件前先 diff——chroot 源与宿主源在个别文件上可能有历史差异。
- 宿主仓库此刻也有与本主题无关的本地改动，绕开即可。

## 交付格式

一份短报告：(1) 不变量的判定结论与证据；(2) 选定设计及健全性论证（逐项对应证明义务）；(3) 测试与性能证据（命令、结果数字、运行环境与源码一致性说明）；(4) 剩余风险与未覆盖形状。
