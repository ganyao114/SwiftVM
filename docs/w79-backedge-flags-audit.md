# W79：self-backedge flags/state 去物化设计审计

日期：2026-08-01  
基线：`a94a984bd0413fd601374a1e801196f2521cc7cd`  
范围：只读审计；没有实现优化，没有改源码、golden 或 benchmark harness。

## 结论

W76 的原始形态不能原样进入实现：`Scale 36→25 / Triad 47→36` 假定热回边可以
完全跳过 11 条 flags/polarity 物化，同时没有计入抢占检查。当前 self-backedge 是
同一 JIT buffer 内的裸 `B(label)`，不会返回 trampoline；`SignalInterrupt()` 只写
`running=false` 和 `State::halt_reason=Signal`，而 trampoline 只在 block 返回后采样。
因此现状的长自环本来就不能及时响应 guest `alarm`/`exit_group`。本轮最小复现中，
x86 原生 1 秒退出，SwiftVM 5 秒后仍未退出（`timeout` rc=124）。W64 修的是“两次
`Runtime::Run()` 之间”的空循环竞态，不覆盖这个“仍在 JIT 自环内”的窗口。

同步 fault 也不能直接豁免。当前 fault table 每个编译 unit 只有一个
`{host_start, host_end, guest_loc}`，fault handler 一律跳到全局
`label_fault_return_host`；它不知道当前 block 的 pending-NZCV mask、carry polarity，
也没有 loop-local 值的恢复配方。W57 正因如此在每个 guest memory/helper/观察点前
flush，并在 terminal 再 flush。把 11 条全挪到冷边后，下一轮第一个 memory fault
会看到未提交的上一轮 flags；全局恢复入口无法补齐它。

但方向不是“收益面不存在”。七语料的 recurrent self-backedge block 覆盖面为：
STREAM **99.9978%**、CoreMark **38.2743%**、7zip **11.9271%**、OpenSSL SHA
**7.1781%**、c-ray **4.6212%**、SQLite **3.9085%**、smallpt **0.0112%**。
STREAM 三个算术环仍是 `399,999,980 : 20`，即每个冷出口前约两千万次回边。

最终决断是：

1. **关闭 W76 的“直接删 11 条、36→25/47→36”实现形态**；它缺失 fault、signal
   和 SMC 的 deopt/safepoint，不能用 liveness 证明替代精确性。
2. **不关闭更窄的 infra-first spike**：先增加统一 self-backedge exit latch、
   per-block fault recovery veneer 与双边证明，再只延迟 NZCV merge + 已知 carry
   polarity；PF/AF 仍热路径写入 x26。这个形态的诚实静态预估是
   `Scale 36→32`、`Add 39→35`、`Triad 47→43`，不是 W76 的 25/36。
3. full flags token / GPR/XMM state loop cache 暂不批准；它需要 loop-carried snapshot、
   external/internal 双入口和更细 fault map，已接近 region JIT，而不是小型 DCE。

## 1. 现状形态与 11 条的组成

W76 的当前 master dump 在 `0x401a70`（Scale）得到以下稳态尾部：

```text
add x7, x22, #16
mov x22, x7
mov/movk x6, #0x1312d000
subs x8, x7, x6
bfxil x26, x8, #0, #8       # PF source byte
eor/eor/ubfx/bfi             # 4 条，AF -> x26
mov w6, #1
strb w6, [x28, #1360]        # ThreadContext64::carry_inverted
mrs x7, nzcv
and x26, x26, keep_mask
and x7, x7, requested_mask
orr x26, x26, x7             # MergeNZCV，4 条
b.eq cold_exit
b self
```

`bfxil + 4×AF + mov/strb polarity + 4×MergeNZCV = 11`。完整 W71 block 还包含
一次性 9 条 cold link tail，故 `host_static=45/48/56`；去掉 cold tail 后正是 W76 的
Scale/Add/Triad `36/39/47`。

post-uniform IR 同样明确：最后的 `Sub + SaveFlags(All) + StoreUniform(U8,u[704])`
后，terminal 是 `LinkBlock(self)` / `LinkBlock(fallthrough)`。`u[704]` 加 State 的
uniform base 后即 host offset 1360，对应 `ThreadContext64::carry_inverted`。

复现 dump：

```sh
SVM_DUMP_IR=1 SVM_DUMP_IR_POST=1 SVM_VIXL_HOST_DUMP=1 \
  /tmp/w79-build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/stream_x64
```

本轮复用了 W76 已保存的逐字节证据：`/tmp/w76-off-dump/all.err`、
`/tmp/w76-off-401a70.asm`、`/tmp/w76-off-401b00.asm`、
`/tmp/w76-off-401ba0.asm`。

## 2. 契约链逐项审计

### 2.1 W57 fault/signal 精确上下文与 W64 抢占边界

当前链路是：

1. `Runtime::SignalInterrupt()` 写 `running=false` 和 `halt_reason=Signal`；它没有向
   正在执行 JIT 的 host thread 发送 POSIX signal。
2. 普通 block `Ret` 后，runtime trampoline 读取 `halt_reason`。非零时保存 x26、
   RSB 和全部 static uniforms，再返回 C++。
3. Linux `RunThread()` 收到 `ExitReason::Signal` 后才调用 `DeliverPendingSignal()`；
   注释明确承诺的是 translated block boundary。
4. `UniformStoreSinkPass` 在全部 memory/helper/control 观察点前 flush，并在 terminal
   `flush_all`，注释理由正是“signal injection is sampled only after a translated unit
   returns”。

self-backedge 例外：`JitContext::Forward(self)` 直接 `B(self_label)`，既不 `Ret`，也
不读 `halt_reason`。W64 在 `Runtime::Run()` 尾部补的 `running=false -> Signal` 只在
控制已经回到 C++、甚至下一次 Run 尚未进入时生效，不能抢占仍在 JIT 内的自环。

本轮实证程序：

```c
alarm(1);
while (!fired) {
    __asm__ volatile("" ::: "memory");
}
```

编译后内环是 `0x401798: load/test; 0x4017a0: je 0x401798`。复现：

```sh
# x86 原生 VM
gcc -O2 -static /private/tmp/w79-self-alarm.c -o /tmp/w79-self-alarm-x64
timeout 3 /tmp/w79-self-alarm-x64                     # rc=0

# Orb ARM64 / SwiftVM
x86_64-linux-gnu-gcc -O2 -static /private/tmp/w79-self-alarm.c \
  -o /tmp/w79-self-alarm-x64
timeout 5 /tmp/w79-build/source/translator/linux/svm_translator_linux \
  /tmp/w79-self-alarm-x64                              # rc=124
```

所以任何新设计都必须把 self-backedge 本身变成可抢占 block boundary。最低要求不是
“冷边 materialize” alone，而是热边 `exit_request` poll + 冷 deopt veneer。该 poll
是至少 2 条 host 指令（load-acquire/test branch）；因此 W76 的 36→25 上限本身就
少算了 safepoint 成本。

### 2.2 W30 全文残存记录与逐条对照

仓库没有保留 W30 的 658 行未合入实现；`git log --all` 也只有归档 commit
`7e1df71`。可全文重读的持久记录就是 `docs/project-status.md` 的 W30 条目：

- W25 预估热路径删 58 条；
- PageFatal 红线和保守回退后，目标块只从 332→328；
- edge materialization 令 ON 态 IR **+23.3%**、host bytes **+25.5%**；
- CoreMark A/B **−0.95%**；
- 机械上限 1.07× 在安全物化成本下不可达，轴关闭。

现行 `FlagsEliminationPass` 解释了这个保守边界为何必要：Return/dispatcher/helper/
check-halt/Condition 等离开已解码 CFG 的 terminal 一律 `Flags::All`；未知/空 successor
也一律 `All`。W59 只在两个 successor flags-in 都可证 dead 时转 BranchOnlyFlags，
从不向 edge 新增恢复代码，因此避开了 W30 的成本模型。

对本方向的适用性：

| W30 否决项 | 是否仍适用 | 本方向的差异/结论 |
|---|---|---|
| 未知 successor/helper/fault 必须保守 | **适用** | self target 已知仍不代表 fault/helper 不观察；必须有逐段 proof |
| PageFatal 前上下文必须可恢复 | **适用且是硬门** | 当前 fault entry 无 pending-flags 配方；不能直接删除 |
| edge 恢复造成 IR/host 静态膨胀 | **适用** | cold veneer 仍增加 bytes；可通过每 block 单 veneer/共享模板控制，但必须记净账 |
| 广泛跨 unit fixed-point 与多 edge 合流 | 部分不适用 | self + 单 cold exit 是闭合二边形，证明空间远小于 W30 任意 CFG |
| materialize 动态落在热 edge | 不适用 | 本方向明确把恢复放冷 exit；七语料频率证明部分 loop 极偏斜 |
| 58 条机械上限被保守回退吃光 | 不能直接外推 | STREAM 的 4 亿:20 仍有面积，但正确上限须扣 poll/fault recipe |

结论不是“W30 反向做就自动安全”，而是只有 self/single-exit 缩小了 CFG 问题；
PageFatal、未知观察点和静态膨胀三条完全保留。

### 2.3 fault map 与 SMC 写窗口

普通 host guest-memory fault：

- `Module::FaultEntry` 只有 unit host 范围和一个 `guest_loc`；function compilation 为
  整个 function buffer 登记一项 `func_start`，不是每 block/每 fault site。
- handler 将 `current_loc=guest_loc`、`halt_reason=PageFatal`，把 host PC 改到全局
  `label_fault_return_host`。
- trampoline 只能保存当前 x26/static pins；不能知道某 block 的 `nzcv_requested`、
  carry polarity 或 loop-local SSA。

因此有 memory 的 loop 若跨回边保留 pending NZCV，下一轮在新 producer 之前 fault，
全局入口会把旧 x26 当成当前 flags。把 merge 移到本轮首个 memory 前可以恢复正确性，
但 STREAM 的第一批操作就是 memory，等于每轮照付，收益归零。真正的 cold-fault
方案必须把 fault table 细化为每 emitted block（必要时更细）的 recovery veneer，
并证明 fault 前仍保留重建所需最小 live set。

SMC 是另一条链：write-protect fault handler 只开 RW window、清 L1/L2 slot，然后
sigreturn 重试原 store；真正 detach/reclaim 在当前 `JitRun` 返回后的
`CloseWriteWindow()`。self 内部 `B(label)` 不经过 slot，delink 也 patch 不到它。
所以活跃自环可能继续执行已经被清 slot 的 translation，且 write window 不能关闭。
这是现有自环已有的边界缺口；新优化若以“local state 可无限跨回边”为前提，会把它
变成显式正确性依赖，不能继续沿用。

推荐让同一个 backedge `exit_request` latch 同时承载 Signal 与 SMC invalidation：
SMC handler/跨线程 invalidator 发布 request，回边 acquire-load 命中后进 deopt veneer，
完整提交 block-entry state 并返回；C++ 随后按现有 epoch/CloseWriteWindow 流程 detach。
只 poll `halt_reason` 不够，因为当前 SMC handler 并不设置它，外部线程 invalidation
也没有目标 Runtime 的 `State*`。

### 2.4 LinkBlock/delink 的依赖

- self edge：同 buffer `B(label)`，不写 `current_loc`，不查 L2，也没有 incoming-link
  record；这是热度来源，也是 signal/SMC 不可见的原因。
- 非 self edge：默认走 L2 dispatch slot；slot 空则写 `current_loc` 后 `Ret`，非空则
  `Br(target)`。SMC 清 slot 可让未来 edge 退回 dispatcher。
- `SVM_DIRECT_BLOCK_LINK` 默认 OFF，源码明确标成缺 incoming-link invalidation、
  非 SMC-safe 的 measurement-only 模式。

因此 cold fallthrough 在调用 `Forward()` 前必须已提交所有跨 unit architectural state；
否则不论它命中 linked target 还是空 slot 返回 dispatcher，consumer 都会读到 stale
flags/state。新的 self veneer 不能改变外部 published entry；任何 external entry、RSB
target、cache miss 都必须从 fully materialized ABI 开始。

### 2.5 解释器/JIT 切换

解释器直接把 flags 存在 `State::host_cpu_flags`，布局与 x26 相同；它没有 JIT 的
host NZCV/local SSA。当前 `enable_jit=true` 时 dispatch miss 不回退解释器，而是返回
CodeMiss 重新编译；JIT/interpreter 模式也是 Instance 级配置，不在 self edge 动态切换。

所以优化可以是 JIT-only，但所有离开 JIT 的路径（Signal、PageFatal、SMC、helper、
cold exit、cache miss）都必须先把 local representation 合并回 x26/ThreadContext。
否则下一次重编译或 interpreter oracle 无法从同一状态继续。

## 3. 七语料 edge-frequency

### 3.1 口径

使用 W71 `SVM_RA_HOT_COALESCE` 的每 block `entries`。标准输出只列 top-20；本轮在
`DumpAtExit` 入口用 gdb 只读导出 Runtime 已提交的全部 counter slots，没有改源码：

```sh
orb -m wine-ci sh -lc '
  cmake -S /private/tmp/w65 -B /tmp/w79-build -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/w79-build --target svm_translator_linux -j8
  nm -S -a -C /tmp/w79-build/source/translator/linux/svm_translator_linux |
    grep "Counters()::counters"
'
```

本构建的 W71 `ProcessCounters` 是 PIE offset `0x4e3748`、size `0x280010`；每 slot
80 bytes（`HotCoalesceUnitStatic` 48 + 4×u64 dynamic），`next_slot` 在 `+0x280000`。
gdb 在 `hot_coalesce_prof.cpp::DumpAtExit`（offset `0x2294c0`）读取 slots，并按 guest
PC 聚合不同编译版本。当前复现脚本与原始证据在 Orb 的：

- `/private/tmp/w79-gdb.py`
- `/private/tmp/w79-run-edge.sh`
- `/private/tmp/w79-find-all-self.pl`
- `/private/tmp/w79-summarize-self.pl`
- `/tmp/w79-edge/{coremark150,stream,smallpt,sqlite,cray,zip7,openssl}/`

运行模板：

```sh
SVM_RA_HOT_COALESCE=/tmp/w79-edge/NAME/hot.txt \
gdb -q -batch -ex 'set pagination off' -ex starti \
  -ex 'source /private/tmp/w79-gdb.py' -ex continue --args \
  /tmp/w79-build/source/translator/linux/svm_translator_linux GUEST [ARGS...]
```

七次正式采样将下表参数代入同一个 wrapper，例如 CoreMark/STREAM 为：

```sh
/private/tmp/w79-run-edge.sh coremark150 \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/coremark_x64 \
  0 0 0x66 150000 7 1 2000
/private/tmp/w79-run-edge.sh stream \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/stream_x64
```

每次运行后用同一 guest ELF 反汇编识别 direct self-edge，再汇总：

```sh
/private/tmp/w79-find-all-self.pl GUEST /tmp/w79-edge/NAME/gdb.out \
  > /tmp/w79-edge/NAME/self-all.txt
/private/tmp/w79-summarize-self.pl \
  /tmp/w79-edge/NAME/self-all.txt /tmp/w79-edge/NAME/hot.txt
```

汇总脚本输出的原始字段就是下表的 `candidates/recurrent/source/exit_upper/`
`taken_lower_pct/ratio_median/ratio_max/host_coverage_pct`；例如 CoreMark 的原始行是
`59/49/3743634509/340417017/90.906777/8.000/213.692/38.274253`，STREAM 是
`32/23/1345000412/136/99.999990/18.000/19999999/99.997794`。其余语料的原始
`gdb.out`、`hot.txt`、`self-all.txt` 均在对应 `/tmp/w79-edge/NAME/` 目录。

识别条件是 guest direct conditional branch 的 target 等于当前 block entry，且
fallthrough PC 也有 entry counter。`source entries` 是 self terminal 执行数；
fallthrough block 可能还有别的 predecessor，所以 `exit entries` 只作为 exit 上界，
`1-exit/source` 是 taken 下界。只把 `source > exit_upper` 的样本列作 recurrent。
`host coverage` 是这些 block 的 `host_static × entries / 全程序 host_dynamic`；它是
entry-weighted 机械覆盖，不是 retired instruction 或可删比例。

### 3.2 参数与正确性

| 语料 | guest / 参数 | 结果 |
|---|---|---|
| coremark | `coremark_x64 0 0 0x66 150000 7 1 2000` | CRC `e9f5/e714/1fd7/8e3a/25b5`，validated |
| stream | `stream_x64` | `Solution Validates` |
| smallpt | `smallpt_wh_x64 4 64 48` | rc=0，`image.ppm` 非空 |
| sqlite | `sqlite_speedtest_x64 --size 1 --testset main test.db` | rc=0，DB 非空 |
| c-ray | `cray_x64 src/c-ray/input/scene.json -j 1 -s 4 -d 128x96 --no-sdl` | rc=0，PNG 非空 |
| 7zip | `7zip_x64 b -mmt1 -md=4m` | rc=0，`Tot: 1789/1790` |
| openssl-speed | `openssl_x64 speed -seconds 2 -evp sha256` | rc=0 |

### 3.3 分布

| 语料 | 可计数 self 候选 | recurrent | source entries | exit 上界 | taken 下界 | loop ratio 中位 / 最大 | host coverage |
|---|---:|---:|---:|---:|---:|---:|---:|
| coremark | 59 | 49 | 3,743,634,509 | 340,417,017 | 90.9068% | 8.000 / 213.692 | **38.2743%** |
| stream | 32 | 23 | 1,345,000,412 | 136 | 99.999990% | 18.000 / 19,999,999 | **99.9978%** |
| smallpt | 25 | 12 | 2,243 | 76 | 96.6117% | 8.377 / 1,535 | **0.0112%** |
| sqlite | 309 | 182 | 774,584 | 135,760 | 82.4732% | 4.500 / 51,258 | **3.9085%** |
| c-ray | 214 | 185 | 7,908,934 | 1,420,034 | 82.0452% | 8.500 / 31,734 | **4.6212%** |
| 7zip | 229 | 179 | 599,474,831 | 134,691,659 | 77.5317% | 7.000 / 1,055,138 | **11.9271%** |
| openssl SHA | 79 | 65 | 308,178,846 | 34,976,823 | 88.6505% | 9.185 / 157 | **7.1781%** |

代表点：

| 语料 | PC | source : exit 上界 | ratio | host/static |
|---|---:|---:|---:|---:|
| stream Scale | `0x401a70` | 399,999,980 : 20 | 19,999,999× | 45 |
| stream Triad | `0x401ba0` | 399,999,980 : 20 | 19,999,999× | 56 |
| coremark | `0x402680` | 887,400,000 : 30,600,000 | 29.0× | 28 |
| smallpt | `0x401cdd` | 1,535 : 1 | 1,535× | 35 |
| sqlite | `0x4adc94` | 128,410 : 8 | 16,051× | 48 |
| c-ray | `0x442ad0` | 1,089,077 : 547 | 1,991× | 168 |
| 7zip | `0x41e148` | 17,841,379 : 2,231 | 7,997× | 192 |
| openssl SHA | `0x62b975` | 270,895,788 : 26,215,694 | 10.33× | 32 |

回答收益面：STREAM 是极端但不是孤例；CoreMark 很宽，7zip/OpenSSL 有中等覆盖，
c-ray/SQLite 较窄，smallpt 基本没有。不能把 STREAM 的两千万比一外推成七语料统一
收益，也不能用 smallpt 关闭方向。

## 4. 设计选项

### 选项 A：原始“热边全不 materialize，冷边补 11 条”

- **静态**：Scale 36→25，Triad 47→36；不含 poll。
- **正确性依赖**：假定回边不会 signal/SMC/fault，或 State 不被观察。
- **fault/interrupt**：无方案；下一轮 fault 和 SIGALRM 反例已证伪。
- **失效回退**：只能把 11 条重新放回热边。
- **裁定**：拒绝，不进入实现。

### 选项 B：minimum-live-set + 统一 backedge deopt（推荐）

热路径保留 PF byte 和 AF 的 5 条写入 x26；只让以下 6 条跨 self edge 保持 local：

- `carry_inverted` 的 `mov + strb`；
- requested NZCV 的 `mrs + and + and + orr`。

在 self edge 前增加统一 `exit_request` acquire poll（按 2 条估算）。正常 taken edge
继续保留 host NZCV；cold fallthrough、Signal、SMC、fault 全进一个 per-block veneer，
先写 polarity/merge NZCV，再走现有 `Forward` 或 trampoline return。

- **静态预估**：Scale `36-6+2=32`，Add `39→35`，Triad `47→43`；净省 4 条，
  分别是稳态 −11.1% / −10.3% / −8.5%。cold veneer 和 fault metadata 另计 bytes。
- **双边证明**：self target 必须是同 block；唯一 cold direct successor；PF/AF 已在 x26
  current；carry polarity 编译期已知；从回边到下一 producer 之间 host NZCV 不被任何
  指令/helper clobber；从该 producer 到下次 safepoint 之间无 fault/observer；所有外部
  entry 从 committed ABI 进入。
- **fault 方案**：function buffer 改为至少 per-emitted-block fault subrange；eligible
  block 的 entry 指向该 block veneer。证明失败或 host PC 不在精确 subrange就用现状。
- **interrupt/SMC**：SignalInterrupt 和 SMC invalidation 都发布同一 request；veneer
  materialize 后返回。不能只复用当前非原子的 `halt_reason` 数据竞争写法；spike 应用
  明确的 atomic latch/epoch 和 acquire/release 次序。
- **失效回退**：逐 block 回退现有完整物化，不能函数级“部分相信”；disk-cache unit
  必须序列化 eligibility/recovery relocation，或开关 ON 时先禁 cache。

该设计的收益小于 W76 初估，但不需要保存 PF/AF 的源 operands，fault veneer 只依赖
x26、host NZCV 和编译期 polarity/mask，RA 风险最低。

### 选项 C：full flags token / state loop cache

把 PF/AF/NZCV/polarity，甚至 GPR/XMM uniform 都跨回边，外部 entry 初始化，内部
backedge 进入不读写 State；fault/signal 时按 live token 全量 deopt。

- **静态上限**：即使 11 条全删，加入 2 条 poll 也只是 Scale 36→27、Triad 47→38；
  W76 的 25/36 不再成立。若再删 state load/store，需另做净账。
- **正确性依赖**：loop-carried value 分配、external/internal 双入口、每个 fault site
  的 live snapshot、helper/partial-write alias、direct external entry、SMC epoch。
- **fault 方案**：当前 PF/AF 的源寄存器会在下一轮早期复用；要么保留 2–3 个 snapshot
  寄存器，要么 per-site recipe。前者缩池/增 spill，后者是完整 deoptimizer。
- **失效回退**：任何 site 无 recipe 则整个 loop 回退。
- **裁定**：不作为首个 spike；它已经越过小型 flags DCE 的风险边界。

## 5. 推荐 spike 的验收门草案

建议拆成两个默认 OFF 阶段，任何阶段失败即撤，不把 infra 与性能收益混成一次提交。

### P0：safepoint/deopt 基础设施（先求零优化）

1. self-backedge poll ON，但仍完整物化；OFF fingerprint/host bytes vs master 零 diff。
2. 上述 `alarm(1)` 自环：原生与 JIT 都在有界时间退出；加入 W64“两次 Run 之间”
   race 组合，至少 1,000 次无 hang。
3. exit_group 对另一个纯自环 guest thread：高负载下有界 join。
4. SMC：本线程自写、另一 guest thread 写、host syscall copy 写三类；poll 后必须执行
   CloseWriteWindow，旧 self branch 不得继续进入已 detach code。
5. per-block fault map：function-mode 多 block 每个 subrange 回到正确 guest entry；
   block mode、lazy function、disk cache load 三态一致。

P0 会在热路径增加 poll，不能默认 ON；只有 P1 的净收益覆盖它才讨论合并。

### P1：minimum-live-set 六条条件化 sink

正确性矩阵：

- Add/Sub/Cmp/Test 及 8/16/32/64 位；CF normal/inverted；PF/AF/N/Z/C/V 分别由
  `LAHF/PUSHF/SETcc/CMOV/ADC/SBB/Jcc` 在 self target 与 cold successor 首条消费；
- fault 注入在“回边后、首 producer 前”“producer 后、terminal 前”“cold veneer”
  三段，覆盖 load/store/跨页/对齐/atomic/helper；
- signal/SMC 与 branch decision 同时发生的竞态，双边状态必须对应 guest loop head
  或 cold successor，不能混合两边；
- external link/RSB/cache miss 直接进入 loop head，必须走 committed entry；
- JIT 与 interpreter 对账，pin 0–3、XMM fault sink 两态、function/block/lazy 三态。

项目门：

- OFF 指纹与 host bytes 对 master 零 diff；ON 自一致；不改 golden；
- mac/orb 全量、func_tests 六格、现有 signal/fault/SMC suite 全绿；
- STREAM 三热 unit 必须得到预期 `45→41`、`48→44`、`56→52` 的完整 block 净降
  （稳态即 36→32、39→35、47→43），且 cold bytes/metadata 单列；
- RA_SHAPE_PROF 两态 spill/high-water/scratch/helper snapshot 不增长；
- STREAM 四项和 CoreMark 各至少 7 对交错 A/B，95% CI 为正；7zip/OpenSSL/c-ray
  同跑防止较低覆盖语料被 poll 反噬；任何正式语料 >1% 回归即维持 OFF/撤回；
- interrupt latency 单列：自环 request 到 C++ return 不超过一次 guest iteration。

## 6. FEX 对照的边界

W76 实抓的 FEX STREAM loop 是 10/11/12 条，末尾以 `subs/cfinv/b.ne` 保持 flags local，
不在每次 backedge 写完整 State。FEX JIT 的 `CondJump` 可直接 `b.cond` host NZCV；
`ExitFunction` 的 direct branch/link 与 delinker有独立 incoming-link record。它的 signal/
state spill 体系也围绕 `CpuStateFrame`、static-register spill 和更成熟的 dispatcher/link
协议设计，不能只抄末尾三条而忽略整套 recovery contract。

这恰好支持本审计的结论：local flags 是正确方向，但它是 ABI/dispatcher/fault 的联合
设计，不是把 SwiftVM 的 terminal `MergeNZCV()` 删掉就完成。

## 7. 清洁度

- 工作树最终只新增本报告 `docs/w79-backedge-flags-audit.md`。
- 未修改任何 `source/`、golden、`source/translator/linux/linker/` 或
  `SwiftVM-bench/harness/run_matrix.sh`。
- 未执行 git commit/push/add/checkout/reset/stash。
- `git diff --check` 通过。
