# 前端 IR 膨胀：逐 guest-opcode 归因（2026-07-27）

`docs/perf-baseline.md` §5.5 把「前端 IR 膨胀」列为候选 #5，并写明「方向确凿但**着力点
未知**，需先做逐 opcode 的 IR/host 字节归因」。本文就是那份归因，外加按表做的三处优化。

被测提交：`5b5cf19`（在 detached worktree 中构建、测量，主工作树有其他并行改动）。
构建：`CMAKE_BUILD_TYPE=Release`，Apple clang 21，`-DFMT_CONSTEVAL=`。

完整表（564 行，按 mnemonic×操作数形态×寻址模式×宽度拆分）：
**`docs/ir-expansion-attribution.tsv`**。本文只摘前 25 行与结论。

---

## 0. 怎么得到这些数字

插桩**只存在于独立 worktree**，主工作区无残留（`git grep -n "SVM_IRPROF\|guest_tag\|ir_prof"`
应无输出）。三个计数点：

1. `X64Decoder::Decode()` 循环体：每条 guest 指令记一次 `(mnemonic, 操作数形态,
   寻址模式, 操作数宽度)`，同时记该指令在 decode 期间新建的 IR 条数（**优化前**）。
2. `ir::Inst` 加一个 `guest_tag` 字段，构造时从线程局部变量取值，**穿过全部优化 pass**。
3. `JitTranslator::Translate(ir::Inst*)`：`CurrentBufferSize()` 前后差 = 这条 IR 发射了
   多少 host 字节，按 `guest_tag` 与 IR opcode 双向累加（**优化后**）。

**分母的坑**：一开始用 IR 里的 `AdvancePC` 计数 guest 指令，结果 `RET` 算出 51.8 条
IR/指令、`JZ` 算出 16.07。原因是 `HIRBuilder::AppendInst` 在函数级终结符之后会丢弃
`AdvancePC`（hir_builder.h:412 的注释写明了），于是 RET/JMP/CALL/SYSCALL 的分母只剩
10%–29%。改用 decode 侧计数后 `adv/dec` 对非终结类指令是 0.94–1.00，对分支类是
0.10–0.52 —— 这个比值本身就是那个缺陷的证据。**全表用 decode 侧分母。**

语料：`source/translator/linux/tests/` 的 25 个 e2e guest + `bench_suite_x86_64` 五个
kernel，共 **45 684 条已翻译 guest 指令 / 276 985 条 IR / 409 216 条 host 指令**。
30 个程序的退出码在插桩构建下与基线逐行一致。

**动态频次**用 IR 解释器（`SVM_ENABLE_JIT=0`）跑六个真实 glibc/musl 程序统计
（func_tests、func_tests_musl、real_busy(_musl)、real_hello(_musl)），
共 650 398 条动态 guest 指令。注意解释器路径与 JIT 不完全等价（`smc` 与 `func_tests`
在解释器下退出码不同），所以动态列是**加权用的近似**，不是行为基线。
`loop_x86_64` 被排除：它是一条 `dec` 自循环，会独占 89% 的动态计数。

---

## 1. 归因表（静态，按总 IR 贡献排序）

`n` = 被翻译次数；`ir/g`、`hostI/g` = 每条该 guest 指令平均产生的 IR 条数 / host 指令数；
`%IR`、`%host` = 占全部 IR / 全部 host 指令的比例；
`dyn%`、`dyn_host%` = 真实 glibc 程序里的动态执行占比 / 动态加权 host 代码占比。

| mnemonic | n | %n | ir/g | hostI/g | %IR | %host | dyn% | dyn_host% |
|---|---|---|---|---|---|---|---|---|
| MOV | 11492 | 25.16 | 4.44 | 3.77 | **18.42** | 10.60 | 20.52 | 8.89 |
| CMP | 3480 | 7.62 | 9.18 | **16.36** | **11.53** | **13.91** | 11.36 | **21.35** |
| TEST | 2233 | 4.89 | 8.36 | 13.15 | 6.74 | 7.18 | 5.71 | 8.62 |
| RET | 1858 | 4.07 | 6.54 | 4.55 | 4.39 | 2.06 | — | — |
| XOR | 1138 | 2.49 | 9.92 | 13.63 | 4.08 | 3.79 | 1.65 | 2.59 |
| CPUID | 62 | 0.14 | 175.00 | 217.00 | 3.92 | 3.29 | 0.01 | 0.05 |
| ADD | 1231 | 2.69 | 8.04 | 14.41 | 3.57 | 4.33 | 5.81 | 9.61 |
| JZ | 2955 | 6.47 | 3.30 | 5.03 | 3.52 | 3.63 | 5.00 | 2.89 |
| AND | 862 | 1.89 | 11.12 | 14.39 | 3.46 | 3.03 | 3.57 | 5.91 |
| CALL | 1192 | 2.61 | 7.68 | 16.17 | 3.30 | 4.71 | 1.25 | 2.33 |
| INC | 680 | 1.49 | 12.56 | **41.74** | 3.08 | 6.94 | 0.01 | 0.02 |
| LEA | 2270 | 4.97 | 3.61 | 3.73 | 2.96 | 2.07 | 5.74 | 2.46 |
| SUB | 890 | 1.95 | 8.56 | 15.18 | 2.75 | 3.30 | 4.09 | 7.13 |
| POP | 1206 | 2.64 | 5.91 | 4.37 | 2.57 | 1.29 | 0.25 | 0.05 |
| JNZ | 1761 | 3.85 | 3.36 | 5.06 | 2.14 | 2.18 | 1.16 | 0.67 |
| SHL | 296 | 0.65 | 19.35 | 21.80 | 2.07 | 1.58 | 0.20 | 0.28 |
| PUSH | 958 | 2.10 | 5.89 | 3.91 | 2.04 | 0.92 | 0.25 | 0.05 |
| SHR | 343 | 0.75 | 15.42 | 17.43 | 1.91 | 1.46 | 0.94 | 1.89 |
| MOVZX | 516 | 1.13 | 7.69 | 6.91 | 1.43 | 0.87 | 2.29 | 1.82 |
| OR | 406 | 0.89 | 8.82 | 11.00 | 1.29 | 1.09 | 0.10 | 0.15 |
| JMP | 2473 | 5.41 | 1.26 | 3.05 | 1.13 | 1.84 | 1.10 | 0.40 |
| PAUSE | 1445 | 3.16 | 2.00 | 1.00 | 1.04 | 0.35 | 0.00 | 0.00 |
| IMUL | 130 | 0.28 | 18.16 | 24.14 | 0.85 | 0.77 | 1.21 | 3.35 |
| DIV | 30 | 0.07 | 10.70 | 61.63 | 0.12 | 0.45 | 0.32 | 2.26 |
| MOVAPS | 165 | 0.36 | 4.38 | 4.19 | 0.30 | 0.17 | 10.15 | 4.88 |

（`RET` 等终结类指令没有动态数：解释器同样丢弃它们的 `AdvancePC`，
所以 `dyn%` 只覆盖非终结指令，各列以其自身总和归一。）

### 同一 mnemonic 的形态差异（全表里更有用的一层）

| mnemonic | form | mem | bits | n | ir/g | hostI/g |
|---|---|---|---|---|---|---|
| CMP | r,m | `[rip]` | 32 | 1029 | 8.00 | 17.00 |
| CMP | r,i | — | 32 | 445 | 6.91 | 14.22 |
| CMP | r,r | — | 64 | 534 | 5.66 | 10.79 |
| CMP | r,i | — | 8 | 159 | 17.75 | 25.02 |
| CMP | m,i | `[b]` | 8 | 130 | 20.00 | 24.78 |
| MOV | r,r | — | 64 | 2346 | 2.84 | 1.53 |
| MOV | r,i | — | 32 | 2075 | 4.91 | 4.01 |
| MOV | r,m | `[b+d]` | 64 | 957 | 4.62 | 3.98 |
| INC | m | `[rip]` | 32 | 514 | 13.00 | **49.00** |

8/16 位算术贵一倍：`ArithWithFlags` 走「左移进 32 位容器再算 NZCV」的路径
（decoder_alu.cc:225 起），一条 8 位 CMP 要 17.75 条 IR。

---

## 2. 逐 IR-opcode 的 host 成本（同一份数据的另一个切面）

| IR opcode | 条数 | %IR | %host 字节 | host 指令/条 |
|---|---|---|---|---|
| LoadImm | 41972 | **15.15** | **11.12** | 1.08 |
| AdvancePC | 34802 | **12.56** | **7.66** | 0.90 |
| StoreUniform | 31613 | 11.41 | 7.73 | 1.00 |
| LoadUniform | 16263 | 5.87 | 3.97 | 1.00 |
| GetHostGPR | 11527 | 4.16 | 0.21 | 0.07 |
| GetOperand | 11519 | 4.16 | **6.78** | **2.41** |
| SaveFlags | 11366 | 4.10 | 0.00 | 0.00 |
| ZeroExtend32 | 11129 | 4.02 | 2.72 | 1.00 |
| ZeroExtend64 | 9710 | 3.51 | 2.37 | 1.00 |
| LoadMemory | 9407 | 3.40 | 2.43 | 1.06 |
| Sub | 8440 | 3.05 | **7.77** | **3.77** |
| Add | 6153 | 2.22 | 4.21 | 2.80 |
| And | 6598 | 2.38 | 4.30 | 2.67 |
| CondSelect | 5747 | 2.07 | 4.21 | 3.00 |
| Or | 2110 | 0.76 | 2.96 | **5.74** |

---

## 3. 结论：着力点在哪

1. **x86 的 EFLAGS 语义就是主成本。** CMP / TEST / ADD / SUB / AND / XOR / INC / SHL /
   SHR / IMUL / OR 加上消费标志的 Jcc 合计 **≈48% 的 IR** 与 **≈53% 的 host 指令**；
   按真实 glibc 的动态执行加权，这一族占 **60.5% 的动态 host 代码**。
   排第一的 CMP 一条要 **16.36 条 ARM64 指令**。
2. **贵的不是 NZCV，是 PF 与 AF。** 后端对每条算术指令发
   `SaveParity`（1 条 `BFI`）与 `SaveAuxiliaryCarry`（`EOR`/`EOR`/`UBFX`/`BFI` 4 条
   + 一个临时 GPR），而 PF/AF 只有 `JP/JNP/SETP`、`LAHF`、`PUSHF`、BCD 会读。
   `FlagsEliminationPass` 从不碰它们：ALU 用 `Flags::All`，其中含 Carry，
   而该 pass 对任何含 Carry 的 SaveFlags 直接 `break`。
3. **`AdvancePC` 不是标记，是每条 guest 指令一次的 NZCV 落地。**
   `EmitAdvancePC` → `MergeNZCV()` 在 NZCV 脏时发 `Mrs/And/And/Orr` 四条。
   实测 0.90 host 指令/条 × 34 802 条 = **7.66% 的全部生成代码**。
4. **常量重复物化。** `LoadImm` 是条数最多的 IR：`CheckCond` 每次条件求值都新建一对
   `LoadImm(1)`/`LoadImm(0)` 喂给 `CondSelect`（ARM64 上本可以是一条 `cset`），
   每条算术指令又新建一个「进位极性字节」常量。
5. **guest 上下文的死写从来没人删。** `UniformEliminationPass` 只做 load 前推，
   不做 store 消除；`StorePolarity` 每条算术指令写一次、几乎立刻被下一条覆盖。
6. **寻址模式展开确实有成本但不是第一档**：`GetOperand` 2.41 条 host 指令/条、
   占 6.78% 字节。基线里怀疑的「地址重复计算」在数据里排第五，不是第一。
7. **后端 `host/ir = 1.48`**（本轮口径，按 decode 侧分母）——基线说的
   「后端每条 IR 落 2 条 ARM64 是合理的」成立，膨胀确实在前端。

---

## 4. 按表做的三处优化

全部落在 `source/runtime/ir/opts/`，两个后端共用（没有新增 IR opcode）。

| # | 改动 | 对应表里的哪一行 |
|---|---|---|
| A | `UniformEliminationPass`：新增**反向的 uniform 死写消除** | StoreUniform 11.41% IR / 7.73% host |
| B | `FlagsEliminationPass`：在 SaveFlags 掩码里**只收窄 PF/AF** | §3.2，CMP/ADD/SUB/AND 一族 |
| C | `ConstFoldingPass`（原本是空壳）：**块内窗口化常量 CSE** | LoadImm 15.15% IR / 11.12% host |

配套两处：
* `DeadCodeEliminationPass` 不再删除无人使用的 `LoadMemory`/`LoadMemoryTSO`
  ——guest 读会 fault，fault 本身就是副作用。这是 A 暴露出来的**既有缺陷**：
  A 的第一版让 `bad_pointer_x86_64` 从 1（PageFatal）变成 42（「读成功了」）。
* `PassPipeline` 补上 `ConstantFolding` 的 function-pass 条目（与 `d42bb4f` 修
  FlagElimination 的是同一个接线漏洞）。

三处各有 env 逃生开关便于二分：`SVM_UNIFORM_DSE=0`、`SVM_FLAG_NARROW=0`、
`SVM_CONST_CSE=0`。

### 确定性指标（30 个程序，与宿主负载无关）

| | ir/guest | host 指令/guest |
|---|---|---|
| `5b5cf19` | 6.063 | 8.958 |
| +A+B+C | **5.721**（−5.6%） | **8.400**（−6.2%） |

### 墙钟：**测不到差别**

同一个二进制、同一次调用内交错、31 reps、中位数（宿主 loadavg 7.40–7.91）：

| workload | 三项全开 | 三项全关 |
|---|---|---|
| int | 0.1701 | 0.1700 |
| branch | 0.2925 | 0.2924 |
| func_tests | 0.0318 | 0.0319 |

七个 workload 全部落在 MAD 之内。这本身是个结论：**「IR 少 5.6%」不自动等于「跑得快」**
——微基准是延迟链主导，被删掉的 AF 计算和极性存储不在关键路径上，M4 的乱序窗口把它们
吸收了；`func_tests` 是翻译主导，省下的代码量与多跑两遍分析的开销相抵。
省下的 6% 是**代码体积**，对 i-cache 压力大的长命真实程序可能有用，本轮没有这样的负载可测。
