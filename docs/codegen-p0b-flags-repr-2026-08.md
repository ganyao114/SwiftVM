# P0-B flags 表示(纸门 2–4)

日期:2026-08-18
承接:docs/codegen-opt-recipe-2026-08.md §2.2
范围:设计已落。x12 预留 + Linux spill 扫描已过。施工第三刀默认 OFF 落地 token +
`EmitSplitFlagsPublish` + 隐含 latch。

纸门 1 已过:coremark / zip7 的 `Sub`/`And`/`Or` 里 flags 打包分别占
**89.82% / 75.07%**(entries 加权 host)。本文回答另外三扇门:怎样让这笔
pack 在密度账上消失、意外出口的载体是什么、开关怎么回退。

## 0. 裁定

| 门 | 结论 |
| --- | --- |
| 2 密度归零 | **有设计**。热路径生产者只留 1 条置 NZCV 的 ALU,PF/AF/Merge 不挂在该 IR 上。 |
| 3 deopt | **载体半齐**。W81 `SVM_BACKEDGE_LATCH` 已能把 Signal/SMC 拉出回边;`FaultEntry` 已有 per-block subrange。缺的是「从 lazy token 填回 x26」的配方,不是再做一套 latch。 |
| 4 开关 | **合同可写**。新 `SVM_FLAGS_REGS` 默认 OFF;`=0` 精确回到今天的 x26 急切打包。 |

**允许的下一刀**(仍默认 OFF):W-β.2 lazy token spike。
**禁止当成主刀**:W-β.1「只用 `ComputeFunctionLiveIn` 换掉 `needed=All`」。
在战役分子 `SVM_REGION_EDGES=0` 上那是空操作,见 §2。

纸面首选 last_result = **x12**(`atomic_scratch`):原子路径本就是观察点,必须先
publish,与 token 存活区间不重叠;`FLAGS_REGS=1` 时把它从 XPOOL 可分配集拿掉。
禁止动 x25/x27 与已封 pin 家。Linux 池扫描只为证 spill 不回涨,不另找家。

## 1. 今天为什么 pack 坐在 ALU 上

`EmitSub`/`EmitAnd`/`EmitOr` 在非 `branch_only` 时,把下面整串算进**同一条**
IR 的 host bytes(`production_emitted` → `svm-gap-op`):

| 步骤 | 大约 | 函数 |
| --- | ---: | --- |
| 置位 ALU(`subs`/`ands`/`orrs`,窄宽还要 lsl/lsr) | 1–3 | `translator_alu_scalar.cpp` |
| PF:`bfi x26, result, #ParityByte, #8` | 1 | `SaveParity` |
| AF:`eor/eor/ubfx/bfi` + scratch | 3–4 | `SaveAuxiliaryCarry` |
| NZCV:多数只标 `nzcv_dirty`(0 条);否则 `mrs+orr` | 0–2 | `SaveHostFlags` |

块尾 `AdvancePC` 再付 `MergeNZCV` 4 条(`mrs/and/and/orr`)。B0
`SVM_FLAGS_REGS_AUDIT` 只给 Merge/SaveFlags 记账,所以从未看见这 15% 程序
host——它藏在 `Sub 0.626` 这种 alu 桶里。

`FlagsEliminationPass` 块内反向活性是真的,也已经会删被后继生产者盖掉的
`SaveFlags`。但块出口写死:

```text
Flags needed = Flags::All;  // flags_elimination_pass.cpp
```

`ComputeFunctionLiveIn` 只喂给 `TryBranchOnly`(两端后继 PF/AF 都死的
`If`)。默认 `SVM_FLAGS_BRANCH_ONLY` 已吃掉那一类。纸门 1 剩下的 89% pack,
是「后继未知 ⇒ 出口必须交出完整 x26」的税,不是 elim 漏删。

## 2. 为什么 β.1(换 live-out)过不了战役分子

`SVM_FUNC_LAZY` 缺省 1。`SVM_REGION_EDGES=0` 时每个编译单元就是 **1 个
guest 块**(见 `translator/x86/translator.cpp` LazyFuncBudget)。注释写明:

- 函数模式下也没有跨块 SSA;
- 每个块尾 `FlushFlags`;
- 后继走 L2,常常尚未解码。

单块 HIR 里 `CollectTerminalTargets` 看到的不是同函数后继,就是 unknown。
`ComputeFunctionLiveIn` 对 unknown 置 `Flags::All`。把
`needed = Flags::All` 换成该函数 live-out,**RE=0 密度表一行都不会动**。

RE=1(16 块窗)上 β.1 能收区内边,那是生产缺省的附带账,不是本方案分子,
也消不掉单元边界上那一次 All 发布。P2 要摊 AdvancePC,靠的也不是这个。

自环 `cmp; jne self` 已由 BranchOnly 覆盖。再收这一刀不是 15% 的来源。

## 3. 纸门 2:lazy token(W-β.2)

FEX 不把 PF/AF 打进内存 flags 字,CMP/TEST 就是一条 host 置位 ALU。要对齐
那本账,发布 ABI 必须改,不能只挪指令到 `AdvancePC`(分类归零、`host_static`
不降,算作弊)。

### 3.1 热路径

每个 flags 生产者(先只切 `Sub`/`And`/`Or`/`Xor`/`Add`,与纸门 1 同族):

1. 发 **一条** 置 NZCV 的 ALU。禁止 `subs wzr`:结果低 8 位是 PF 的源,
   CMP 也要写进 last_result。
2. 不调用 `SaveParity` / `SaveAuxiliaryCarry`。
3. `SaveHostFlags` 只标 `nzcv_dirty`,不 `mrs` 进 x26。
4. 记下 token:`op`、宽度、last_result、AF 需要的左右源(见下)。

`svm-gap-op` 分类:该 IR 的 `pack_b=0`,`alu_b=4`(窄宽对齐的 lsl/lsr 仍算
alu,不是 pack)。纸门 2 的验收就是 coremark/zip7 三 opcode 的 pack% → 0,
且这 15.38% 不得原样出现在 `AdvancePC`/`SaveFlags` 的 bytes 里。

### 3.2 token 内容

| 字段 | 热家 | 更新 | 谁读 |
| --- | --- | --- | --- |
| NZCV | host PSTATE | ALU 自己 | `b.cond` / `cset` / ADC 的 host C |
| last_result | 专用 GPR | 每条 flags ALU 的 dest | PF 消费;GetFlags/PUSHF;deopt |
| AF src | 见 §3.3 | 可选 | AF 消费;deopt |

x26 仍是 **已发布 ABI**(与解释器 `State::host_cpu_flags` 同布局)。热路径
不写 x26。观察点才把 token 打回 x26。

PF 消费(`JP`/`JNP`/`SETP`/`LocalParitySet`):对 last_result 低 8 位做现成的
`GetParityFlag` eor-reduce,不读 x26。

### 3.3 AF

coremark/zip7 几乎不读 AF。两条路,spike 只许选一条并用开关切开:

- **A(推荐)**:AF 也懒。token 再保留 left/right 的 bit4(两个 1-bit,或一条
  `eor` 之后的 1-bit)。生产者多 0–1 条,不是今天的 4 条。
- **B**:AF 仍急切写入 x26。纸门 2 对 AF 那 3–4 条不算过(pack 不能归零)。

禁止「生产者上算完 AF 却记到 ALU 头上再宣称 pack=0」。

### 3.4 观察点才打包

下列路径必须先 `EmitSplitFlagsPublish()`(PSTATE NZCV + last_result→PF +
AF bit → x26,清 `nzcv_dirty`)再离开 token ABI:

- `GetFlags` / LAHF / PUSHF / PUSHFQ
- helper / `CallLambda` / `CallLocation` / x87
- PageFatal / alignment / atomic 观察点
- Signal / SMC / host 出口
- 解释器切换与 CodeMiss(JIT-only 优化,离开前必须可被解释器读)
- 跨编译单元、目标 live-in 未知的边(RE=0 的 L2 缺省就是这种)

**同单元、且目标 `ComputeFunctionLiveIn` 不含 PF/AF 的边**可以不打包,
token 寄存器原样进入后继(只在 RE=1 窗内有意义)。

这样 RE=0 的块界不再付 PF/AF/Merge,**除非**后继尚未编译(未知 ⇒ 发布)。
未知边若仍强制 All,RE=0 又变净零。所以 β.2 的发布 ABI 是 token 寄存器
**穿过 dispatcher**,不是「未知就打回 x26」。L2/`Br(slot)`/RSB 必须按 callee-saved
或固定 pin 保存 last_result(+ AF bit)。这是相对今天唯一的 ABI 增量。

## 4. 纸门 3:W79 清单 × 现成载体

W81 已落地、默认 OFF,结论仍有效:P1 六条 sink 补不回 poll,不翻盘。β.2
**复用 P0 latch,不复用 P1 配方**(P1 假定 x26 已是 current)。

| W79 出口 | 已有载体 | β.2 还要做的 |
| --- | --- | --- |
| 普通块 `Ret` + trampoline | 已提交 x26 | `Ret` 前 `EmitSplitFlagsPublish` |
| self-backedge 裸 `B` | `SVM_BACKEDGE_LATCH`:`LDAR+CBNZ` + veneer | veneer 改调 publish,不调 `EmitBackedgeMaterialize` 的 x26 假设 |
| Signal / `exit_group` | latch bit63 release;OFF 态仍是块界才看见 | ON 态 spike 必须同时开 latch,否则自环里的 token 对 handler 不可见 |
| SMC 清 slot | latch 低 63 位 generation | 同上;publish 后退回 CloseWriteWindow |
| PageFatal | per-block `FaultEntry` + 可选 recovery | recovery = publish;无 recipe 的块回全局入口(该块不得省略 PF/AF) |
| helper / 观察点 | `UniformStoreSink` 已在这些点 flush | flush 改为 publish |
| direct-link `b target` | 已发布入口走 committed ABI | 目标与源同一 token ABI 则不打包;跨 ABI(OFF 单元、cache revive)先 publish |
| disk cache | W81 FLAGS ON 禁 cache | `SVM_FLAGS_REGS=1` 同样禁,直到 `SerialBlock` 能写下 token 家 |

解释器布局不改。任何 JIT→解释器/重编译入口都先 publish。

**β.2 对 latch 的依赖**:RE=0 热自环若要在 SIGALRM 时交出与原生一致的
RFLAGS,必须能在回边进 veneer。spike 允许 `FLAGS_REGS=1` 隐含
`BACKEDGE_LATCH=1`(文档写清;仍都默认 OFF)。不把 latch 翻成全局默认。

没有 per-site operand capture。token 就是最小 live set:last_result +
可选 AF bit + 编译期 op/width。RA 不得在 token 存活区间重用 last_result
家;下一条 flags ALU 才能覆盖。

## 5. last_result 家(未决,挡发射)

封存项不可动:x25/x27、selector 级 SRA、GPR 全 pin level 3、XMM 静态。

| 候选 | 利 | 弊 |
| --- | --- | --- |
| **x12 `atomic_scratch`(推荐)** | 已是 scratch 类,非 pin 家;原子 RMW 前必须 publish,与 token 区间不交;`XPOOL=0` 时本就不在 value pool | `FLAGS_REGS=1` 要从 XPOOL 集拿掉 x12,等价池 −1,须证 spill 不回涨 |
| 另从可分配池钉 1 枚 | 同上 sticky 家 | 没有比 x12 更干净的空号;禁 x25/x27 |
| 块尾把 last_result 写进 State 新槽 | 不缩池 | RE=0 每块一次 st,吃回纸门 2 的利润 |
| 借用 x26 存 last_result | 零新 pin | 与已发布 flags 字别名,GetFlags/解释器/故障恢复全要改语义,否 |

AF bit 若走 §3.3.A,优先塞进 last_result 家的高位或 x26 里今天 AF 那个 bit
(热路径仍不维护完整 x26,只维护那 1 bit)。不再单独钉第三枚。

未做「钉哪一枚」的 Linux 池扫描之前,不写 `EmitSub` 改动。扫描只读,复用
`SVM_RA_HOT_COALESCE` 的 allocatable 压力,不新开 pass。

## 6. 纸门 4:开关合同

| 变量 | 默认 | 语义 |
| --- | --- | --- |
| `SVM_FLAGS_REGS` | OFF | 1 = β.2 token ABI |
| `SVM_FLAGS_REGS=0` | — | 精确回到 x26 急切;`SaveParity`/`SaveAuxiliaryCarry`/`needed=All` 全在 |
| `SVM_FLAGS_REGS_AUDIT` | OFF | 仍只读;不改 |

隐含:`FLAGS_REGS=1` ⇒ 本进程 `BACKEDGE_LATCH=1`,disk cache 拒绝带 token
的 unit。`BACKEDGE_FLAGS`(W81 P1)与 β.2 互斥,同时开则 FLAGS_REGS 胜、P1
配方不走。

验收(过了才允许讨论翻默认,本波不翻):

1. orb Linux,禁 `EXEC_PROF`,冷 cache;`RE=0` 与默认 RE 两态。
2. coremark/zip7:`Sub+And+Or` 的 `pack_b` 合计 / 三项合计 → **0**(允许
   分类噪声 <1%)。
3. 同两次跑的 `host_static`(entries 加权)必须下降,coremark 方向是把
   15.38% pack 从程序 host 里抠掉一截;若只是挪到 `AdvancePC`,判失败。
4. spill 不回涨。sqlite 指纹 OFF 零差;ON 只许 IR 收缩类重生 golden。
5. 定向:alarm 自环有界退出、PageFault 后 PUSHF、SMC 自写、helper 往返后
   JP,解释器/JIT 对同一 RFLAGS 观察点一致。

## 7. 明确不做

- 不把 pack 改记到 `AdvancePC` 来「过」纸门 2。
- 不删 SetHost/GetHost,不碰 fault recipe 家族以外的 P1。
- 不在无 token 家扫描时改 `translator_alu_scalar.cpp`。
- 不把 `SVM_BACKEDGE_FLAGS` 翻 ON。
- 不把 SHA/XMM/SRA 绑进这一刀。

## 8. 下一步(施工序)

1. ~~只读选家~~:已裁定 x12。A 类 `SVM_FLAGS_REGS` 默认 OFF 已接线。
2. ~~Linux direct 池扫描~~:coremark/zip7 spill 不回涨,`max_live_gpr` 9/11 vs 池 13。
3. ~~默认 OFF token + publish + latch 隐含~~:热路径 `Sub`/`And`/`Or`/`Xor`/`Add` 不再 `SaveParity`/`SaveAuxiliaryCarry`;观察点 `MergeNZCV` 顺带把 last_result/AF 打回 x26;AdvancePC 与同 unit 自环/region 边保持 lazy;`FLAGS_REGS=1` 隐含 latch。默认仍 OFF。
4. §6 正确性已过(func_tests 双 RE、coremark CRC、sqlite smoke)。
   热路径 PF/AF 仍写 x26,只懒 NZCV + 跳过 And/Or MergeLogical。
   coremark RE=0 host_dynamic −0.22%;**默认 RE=1 +6.0%**(双入口要 Load)。
   未翻默认。
