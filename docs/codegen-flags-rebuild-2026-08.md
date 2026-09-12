# Flags 重构方案：机制上限对齐 FEX

日期:2026-08-18
承接:docs/codegen-p0b-flags-repr-2026-08.md、docs/codegen-opt-recipe-2026-08.md §2.2、
docs/w79-backedge-flags-audit.md
范围:**设计**。不写发射、不翻 `SVM_FLAGS_REGS`、不改 State 布局，直到 §7 门过。

目标不是「再抠一版 Merge」，而是让 **flags 相关机制的每 guest 指令 host 上限**
与 FEX 同形态：热 ALU = 1 条置 NZCV，Jcc = `b.cond`，PF/AF 只在消费点出现，
发布只发生在观察/deopt。整程序 FEX parity 仍要 P1 发布模型 + P2 超块，不在本文。

## 0. 上限怎么对齐

Linux direct 上 coremark **4.04 vs 1.81 h/g**。alu+xport **1.93 vs 0.88（+1.05）**。
纸门 1：`Sub+And+Or` 的 pack 占三项 **89.8%**、占程序 host **15.4%**。
FEX 同址热块是一条 `subs`/`ands` + `b.cond`，没有 PF/AF/`mrs` 串。

对齐 flags **机制上限** 的意思：

| 机制 | FEX | 我们今天 | 重构后上限 |
| --- | --- | --- | --- |
| 热 ALU（要 flags） | 1 条置 NZCV | 1 ALU + PF `bfi` + AF 3–4 + 块尾 Merge 4 | **1 条置 NZCV** |
| Jcc | `b.cond` / `cbz` | `TestFlags`→x26 或 `Load+Tst+Cset` | **直接 `b.cond`** |
| PF / AF | 消费点或根本不算 | 挂在每个生产者上 | **JP/PUSHF/deopt 才算** |
| 块内后继 | PSTATE 活着 | AdvancePC 当 flush | **PSTATE 活着，AdvancePC 零 flags** |
| 跨 unit / L2 / helper | 超块摊薄；出 JIT 才物化 | 每条边 Merge，否则下一 unit PSTATE 是垃圾 | **边 = `b`（零 flags 税）；出 JIT 才 publish** |

15.4% pack + ~8% AdvancePC Merge 是这条轴的利润池。吃干净后 coremark 大约
4.04→**~3.2 h/g**，相对 FEX 1.81 仍有 ~1.4，那是 SetHost / 边界 / 超块，不是 flags。

已证 **达不到** 该上限的做法（禁止再立项）：

- 把 pack 挪到 `AdvancePC`（分类归零、`host_static` 不降）
- 只懒 NZCV、PF/AF 仍写生产者（RE=0 −0.22%，RE=1 +6%）
- trampoline 从 **x26 再 `msr NZCV`** 却假定每块 PSTATE 有效（RE=0 SIGABRT，RE=1 +20%）
- 双入口 veneer 插在 body 前且 fallthrough 落到 Load（绕环）
- `FLAGS_REGS` 隐含 `BACKEDGE_LATCH`（latch 单独已过正确性，叠加 token 翻不过密度门）
- W-β.1 换 `needed=All`（RE=0 每单元 1 块，后继 unknown）

## 1. 三层表示（核心合同）

一层一个寿命，禁止混用。

```
          热路径（unit 内 + 已证明的内部边）
          ┌─────────────────────────────────────┐
          │  NZCV     = 主机 PSTATE             │
          │  last_result / AF[63] = x12         │
          │  不写 x26                           │
          └─────────────────────────────────────┘
                         │ publish / park
                         ▼
          已发布 ABI（永远可被解释器 / 信号读）
          ┌─────────────────────────────────────┐
          │  x26 = State::host_cpu_flags        │
          │  布局不变                           │
          └─────────────────────────────────────┘
                         │ 仅进出 JIT
                         ▼
          停泊（过 dispatcher / C++ 仍活）
          ┌─────────────────────────────────────┐
          │  State.nzcv_park    ← PSTATE        │
          │  State.result_park  ← x12           │
          └─────────────────────────────────────┘
```

- **热**：和 FEX 一样，ALU 只碰 PSTATE；结果在 x12（已选、spill 扫描已过）。
- **已发布**：x26。观察点、解释器、SMC、PUSHF **只认这一层**。
- **停泊**：新加 **两个 u64**，专给「C++ 会打死、x26 又还是旧打包字」的东西。
  不是第二份 RFLAGS。x26 继续存完整打包字；停泊存 **还没打进 x26 的热态**。

`=0` 时不停泊、不读这两槽，发码与今天 x26 急切 **字节相同**。

### 1.1 为什么必须有停泊，不能只用 x26

x26 是 callee-saved，进出 trampoline 已经 `str/ldr`。PSTATE 和 x12 不是。
直连 `b` 能带着热态走；`Ret` / `JitRun` / 多数 L2 不能。

若只在入口用 x26 做 `msr NZCV`：那是「已发布字 → PSTATE」，**丢掉**
「上次 ALU 之后还没 publish 的 PSTATE」。热态和 x26 一旦分叉（懒路径的常态），
从 x26 恢复就是错的。RE=0 SIGABRT 就是这个洞。

停泊在 **离开 JIT 之前** 写下当前 PSTATE 和 x12（无论 x26 新不新），
**进入 JIT 之后** 先 `unpark` 再跑 body。这样下一 unit 的热态接得上。

### 1.2 State 布局（未实施）

在 `spill_area` **之前** 加字段会平移 uniform，禁。

只许两种放法，施工前锁一种：

1. **占用 spill_area[0]、[1]**，FLAGS_REGS 时 RA 少两个 spill 槽。
   不改 `offsetof(uniform)`。须再跑一遍 spill 扫描（x12 钉过是 0，再少 2 槽）。
2. **State 末尾、`uniform_buffer_begin` 之前** 加 `u64 nzcv_park; u64 result_park`。
   平移 uniform，**disk cache 全废**（FLAGS_REGS 本就拒 cache）。只在 `=1` 进程用。

推荐 1：不动 uniform。`=0` 不碰这两槽。

禁止：别名进 `host_cpu_flags`、别名 x26、再钉第三枚 GPR。

## 2. 控制流契约（上次两刀为什么炸）

四种进块方式，flags 寿命不同：

| 入口 | 谁跳过来 | 热态 |
| --- | --- | --- |
| A 直连 `b` / 区内 fallthrough | 上一块 body | PSTATE+x12 **还在** |
| B `JitRun` / trampoline `blr` | C++ 刚回来 | 必须 **unpark** |
| C L2 表里的 host 地址 | 可能不经 trampoline | 必须落到 **published** 入口 |
| D 自环 `b` | 同块 | 与 A 相同 |

因此每个 **对外可见** 的 guest PC 要两个 host 地址：

```
published:                    # GetLabel / L2 / JitRun / 跨 unit 直连
    ldp  x12, ip, [state, #park]
    msr  NZCV, ip
    b    internal
internal:                     # GetInternalLabel / 区内 ForwardLocal / fallthrough
    ; body，nzcv_dirty=true
```

硬规则：

1. **顺序 fallthrough 的第一个字节是 `internal`**，不是 published。
   veneer 放在 **块尾或 unit 尾**。上次把 Load 插在 body 前，fallthrough 落到
   Load/`b`，和内部边拧成环。
2. `SetCurrent` 在 FLAGS+region 下 **不要** 在 body 起点绑 `GetLabel`。
   函数入口的 `GetLabel` 绑在 **第一条 published veneer** 上。
3. 区内边：`Merge` **不发**（PSTATE 还在），`b internal` 或 fallthrough。
4. 出 JIT（`Ret`、CallHost、CodeMiss、Signal 可见出口）：
   `park`（mrs+str x12）**并且** `publish`→x26（信号/解释器）。
5. 跨 unit 直连 `b published`：对方 unpark；本侧 **只 park、可不 publish**
   （x26 可以旧，热态在停泊里）。若目标是 `=0` 单元或 cache 旧码：必须 publish，
   走 committed ABI。
6. scratch 只用 `ip`/`ip0`。禁止 `GetSharedTmpX()`：TickIR 没开时会砸 x0–x5。

`SVM_FLAGS_REGS=1` **不** 隐含 latch。Signal 看见的是出口 publish 过的 x26。
自环若要在 SIGALRM 中点交出原生 RFLAGS，用 **已有** `BACKEDGE_LATCH` veneer
调 `publish`，另开、另测，不绑进 FLAGS 默认。

## 3. 热路径与观察点

### 3.1 生产者（先 Sub/And/Or/Xor/Add/Neg/Cmp/Test）

1. 一条置 NZCV 的 ALU，结果在 x12（或与 dest pin 同一家且 last_result 转发到 x12
   的证明——第一刀不做转发，dest 就写 x12 再 SetHost，或 dest 在 pin、再
   `mov x12, pin`；密度刀再消这条 mov）。
2. 不调用 `SaveParity` / `SaveAuxiliaryCarry`。
3. `SaveHostFlags` 只标 `nzcv_dirty`。
4. AF 走 p0b §3.3.A：x12[63]，生产者 0–1 条，禁止在生产者上算完 4 条再藏分类。

`pack_b` 必须是 0。`alu_b` 只计那条 ALU（+合法窄宽 lsl/lsr）。

### 3.2 消费者

| 消费者 | 做法 |
| --- | --- |
| Jcc NZCV（JO/JC/JZ/JS/JA 的 CF 部分…） | `b.cond`，**禁止**先 Merge 再 Tst |
| JP/JNP/SETP | 对 x12 低 8 位 eor-reduce，不读 x26 |
| JA/JBE（CF∧ZF） | `b.ls` / `b.hi` 吃 **同一次** 比较的 PSTATE；禁止 `And(TestFlags(CF), CondSet(NE))` 那种先 Tst 再砸 ZF |
| ADC/SBB | host C；只在 `FLAGS_REGS && !branch_only` 时当生产者 |
| PUSHF/LAHF/GetFlags | publish，再读 x26 |
| helper / 原子 / fault | publish（原子本就会打 x12，先 publish） |

`TestFlags` 不再是「先 Merge 再 Tst」的万金油。能映射到 host cond 的，直接 cond。
必须测 x26 里 PF 等非 NZCV 位的，先 publish。

### 3.3 AdvancePC

零 flags。不是 flush。`FlushFlags` 只清 `ClearFlags` 挂起位。

## 4. 和现有开关的关系

| 开关 | 重构后 |
| --- | --- |
| `SVM_FLAGS_REGS` | A 类。`=0` 字节等于今天急切 x26。`=1` = 三层 ABI。默认 OFF 直到 §7。 |
| `SVM_FLAGS_REGS_AUDIT` | 仍只读。 |
| `SVM_BACKEDGE_LATCH` | 独立。FLAGS 胜时 `BackedgeFlagsEnabled()` 仍假。 |
| `SVM_RA_COALESCE_LIVE` | 保留。last_result 在 x12 时，向 pin 的发布仍走 W-α。 |
| disk cache | `=1` 拒绝，直到 SerialBlock 写下「停泊+token 家」。 |

`kFeatureCount` 不动。

## 5. 施工顺序（一刀一扇门）

每刀默认 OFF 或只在 `FLAGS_REGS=1` 后可见。失败整刀回滚，禁止半开 ABI。
每步 orb 烟测 **必须 `timeout`**（func_tests 45s，coremark 90s）。

**刀 0 — 合同测试（零发码）**
- 单测：四种入口分别落到 published / internal。
- 文档锁死 fallthrough 第一字节 = internal。
- 不过不写 trampoline。

**刀 1 — 停泊槽 + trampoline park/unpark**(已落,`72ef3bb`+本刀)
- 停泊在 `spill_area[0..1]`。`nzcv_park` bit0 = valid；第一次
  `JitRun` 见 0 则跳过 unpark。
- scratch 用 **x17/`ip1`**。x16/`ip` 在 `return_host` 上会毁掉
  PIN_EXT=2 的 halt/loc 别名，func_tests 会 rc=0 秒退。
- `=1` 热路径仍急切 PF/AF。func_tests 双 RE 校验和已过。
- 密度允许持平(RE=1 仍是旧 FLAGS +6%，不是停泊引入的)。

**刀 2 — 热 ALU 去 pack**(生产者 PF/AF 已关;`=1` RE=0 host −2.9%;RE=1 仍正,不翻)
- 生产者跳过 PF/AF；`AdvancePC` 不 Merge。
- 观察点 `EmitSplitFlagsPublish`（PSTATE+x12→x26）。
- 出口：park **且** publish（信号仍读 x26）。
- 区内边：不 Merge、不 publish；`b internal`。
- 跨 unit 直连：`b published`（对方 unpark）。
- 门：`pack_b→0`；**RE=0 与 RE=1 的 entries 加权 host 都下降**；spill 不回涨。
- JA/JBE 必须走 host cond，回归 `switch_worker`/`func_tests` flags 组。

**刀 3 — 双入口落地**
- 刀 0 的布局真正绑上 `GetLabel` / `GetInternalLabel`。
- L2 只填 published。
- 函数入口 = 首块 published veneer，不是 body。
- 门：func_tests 双 RE；默认 RE **不得挂、不得环**；host 相对刀 2 再降或持平。

**刀 4 — Jcc 本地化**
- 能映射的 Jcc 全改 `b.cond`，删 `LoadNZCV`+`TestFlags` 热路径。
- 门：coremark/zip7 flags 相关 opcode 的 host 接近 FEX 同 PC（±1 条 mov 到 x12）。

**刀 5 — 翻默认讨论**
- 刀 2–4 的密度门全绿。
- `=0` 对金标 IR/host 零差（或先重锚与 flags 无关的旧 IR 漂）。
- 定向：alarm 自环、PageFault+PUSHF、SMC 自写、helper+JP；解释器/JIT RFLAGS 一致。
- 仍不隐含 latch。不过门不翻。

## 6. 密度预期（诚实）

相对当前默认（LIVE 已 ON、FLAGS OFF）：

| 阶段 | coremark RE=0 | coremark RE=1 | 说明 |
| --- | ---: | ---: | --- |
| 今 | 基准 | 基准 | 已含 W-α −2% 量级 |
| 刀 1 | ~0 | ~0 | 只买 ABI |
| 刀 2 | **−8%～−15%** | 须为负 | 吃 pack + AdvancePC Merge；RE=1 靠区内不 Merge |
| 刀 3–4 | 再收 Jcc/Load | 再收 | 上限是 FEX 那条 ALU+`b.cond` |
| 整程序 vs FEX | 仍约 1.7× | 同 | 剩下 P1/P2 |

若刀 2 的 RE=1 不降：停在刀 1，**不翻**。说明区内边仍在付 publish，回去查
fallthrough/L2 有没有误走 published。

## 7. 正确性清单（W79 更新）

| 出口 | 配方 |
| --- | --- |
| `Ret` / trampoline | park + publish |
| 区内 `b` / fallthrough | 什么都不做 |
| 跨 unit `b published` | 本侧 park；目标 unpark |
| Signal / SMC | 读 x26（已 publish）；自环中点另开 latch |
| PageFault | recovery veneer publish；无 map 的块禁止懒 |
| helper / 原子 | 前 publish（x12 是原子划痕） |
| PUSHF / GetFlags | publish |
| `=0` 单元 / 旧 cache | 入边前 publish，按 committed 进 |
| 解释器 | 只碰 x26 |

## 8. 不做什么

- 不把 pack 记到 AdvancePC 上。
- 不删 SetHost、不开 level 3、不动 x25/x27。
- 不把 last_result 别名进 x26。
- 不在 `GetSharedTmpX` 上做入口 Load。
- 不把 FLAGS 和 latch 捆成一个默认。
- 不在无 `timeout` 的脚本里跑 `FLAGS=1` 默认 RE。

## 9. 第一刀落地前要锁的两个选择

1. 停泊放 **spill[0..1]** 还是 **uniform 前新字段**（推荐 spill）。
2. 跨 unit 直连是 **一律 `b published`**（实现简单、每边 +2 unpark）还是
   **同 ABI 走 `b internal`**（FEX 上限、要证明目标已是 token ABI）。

刀 2 可以用「一律 published」先过密度下降门；刀 4 再收同 ABI 直连，才碰到
FEX「边 = 一条 `b`」的天花板。
