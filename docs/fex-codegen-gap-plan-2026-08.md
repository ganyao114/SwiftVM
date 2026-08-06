# FEX 代码质量差距机制归因与优化方案（2026-08-06)

数据锚点:baseline6(安静窗口,REPS=5,24/24 oracle)SVM/FEX 中位比——smallpt 2.498×、c-ray 2.640×、coremark 0.456、STREAM Scale 0.332/Add 0.455/Triad 0.454/Copy 0.707、zip7 0.662、osslaes 0.683;胜场 osslsha 2.458、sqlite 0.911。

本文基于四路并行研究(2026-08-06):A=FEX 热循环发码解剖、B=FEX SSE/FP 路径解剖、C=FEX 块间机制解剖、D=我方当前 master 热码实测归账(密度/动态探针 + top unit 反汇编手工归账)。四路独立取证,结论交叉互证一致。

## 0. 先划掉已死账目(勿再立项)

D 路在当前 master 实测确认:

- **NaN guard 动态账 = 0**(三基准 `nan_guard_dynamic=0`)——AFP 白名单(sse_afp_nan 默认 ON)已吃满,W76 口径的"每 FP op 5 条 guard"不再存在;
- **spill ≈ 0**(coremark 0.0000004%、smallpt 0.0015%、c-ray 0.11%,top 热 unit 全零);
- **直接链接与 FEX 打平**(已链接块到块双方都是 1 条 `b`);
- **GPR pin 数量不是差距**(L3 全 pin 实测 −10.18% 已关闭;W76 XMM 静态映射单独移植实测净负——见 §3 的组合性论证)。

另记方法论事故(探针使用者必读):**SVM_EXEC_PROF=1 会污染发码形态**——其 RecordExecCounter 的 4 条计数序列不被 hot-coalesce 窗口排除,且 FixedGPRClobbers 保留 ip0/ip1 改变 RA:coremark host_dynamic 虚高 40%、c-ray 虚高 24%、c-ray spill 虚高 15 倍。密度/归账测量一律不开 EXEC_PROF;它只在单独跑里用于出口/uniform 计数。

## 1. 我方当前残余账(D 路实测,占动态 host 指令 %)

| 类别 | coremark | smallpt | c-ray |
|---|---:|---:|---:|
| 真语义 work | 46.0 | 40.3 | 36.6 |
| move/宽度桥(host 反汇编口径,含 const 物化) | **38.8** | **29.6** | **30.4** |
| uniform/state 访问 | 4.2 | **16.8** | **18.3** |
| flags 操作 | 8.3 | 11.3 | 7.6 |
| 块边界(终态+link) | **17.9** | 11.9 | 12.1 |
| NaN / spill | ~0 | ~0 | ~0 |

关键构成事实:

- uniform 访问里 **XMM 占 88–94%**(smallpt 17.8G:1.1G、c-ray 76.3G:10.0G)——GPR 静态驻留有效,XMM 全部逐条往返 ThreadContext;
- coremark unit 平均仅 22.7 条动态指令却配 3+ 条出口序列,link_hit 仅 7.6%——**小块 + 边界税**是它的主形态;
- move/桥的实证形态:`lsr w?,w?,#0` 空桥、`mov w?,w?` 紧邻重复、`ubfx #0,#0x20` 与 mov 成串,集中在 32 位 guest 写的零扩展发布点(coremark 矩阵循环 45 条中 25 条是桥);
- smallpt/c-ray top unit 内 **rip 相对常量地址每迭代 movz+movk+mov 3 条重物化**,同一模式单 unit 重复 5 次(占 5–11%)。

## 2. FEX 侧机制事实(A/B/C 三路,file:line 证据见各路报告)

1. **SRA 全静态映射**:16 guest GPR + PF + AF = 18 个固定 host GPR(`Arm64Emitter.cpp:47-66`),16 XMM 全量映射 v16-v31(`:93-98`)。块边界**零**状态流量是设计公理——SpillStaticRegs 只出现在 dispatcher 出口(`Dispatcher.cpp:241/287`)。
2. **RA 合并器**:LoadRegister/StoreRegister 是 IR 注解,RA 反向亲和扫描把它们与 SRA 家合并,**合并成功 = 0 条指令**(`RegisterAllocationPass.cpp:184-234, 608-632`)。guest `add rax,0x10` 直接发 `add x4,x4,#16`。tie 不靠运气:Dst==Vector1 由映射恒成立(`VectorOps.cpp:312-320`)。
3. **flags 恒同步**:NZCV 永驻 host PSTATE(每个 flag 产生指令直接 `adds/subs`),PF/AF 驻留专用 host GPR r26/r27;块边界 flags 成本恒 0,jcc = 单条 `b.cond`。
4. **multiblock**:Jcc/jmp 链(跨函数、16KB 前向窗、5000 指令上限)并进单一编译单元(`Frontend.cpp:1125-1200`,默认 ON);内部边 = 裸 `b`/`b.cond`,回边仅 +2 条 suspend 轮询。
5. **间接分支块内联 L1**:6 条不写状态(`BranchOps.cpp:199-213`);shadow stack push 2 条/pop 3 条。FPCR 会话级一次设置(dispatcher 入口唯一设置点 `Dispatcher.cpp:124`)。

## 3. 核心论点:差距是三组**组合**机制,不是三件单点

四路证据共同指向:FEX 的优势由三个互相依赖的组合构成,拆零件移植拿不到数字(我方已有两次实测否决作证):

- **组合一:flags 寄存器化(NZCV 恒驻 + PF/AF 专用 GPR)× 边内部化**。单独做边内部化,我们实测动态净账 ≈0(`0311401` region edges 默认 OFF 的原因)——因为懒 flags 模型下每条被内部化的边仍要 MergeNZCV 4 条;FEX 的边便宜是因为 flags 本来就在架构寄存器里。**顺序必须是 flags 先行**。
- **组合二:XMM 静态驻留 × RA 合并器 × 块边界不落地**。W76 实测单独开静态映射净负——没有合并器,驻留值跨块仍要 fill/spill,白付搬运。D 路实测的 17–18% XMM state 账只有组合移植才能收割。
- **组合三:multiblock 单元内化 × 边界免税**。coremark 的 17.9% 边界账 × 22.7 条/entry 的小块形态,只有扩大单元粒度才能从根上摊掉。

## 4. 立项清单(按依赖序)

> 每项惯例:per-module 回退开关、密度账先行验证、orb 全门+双平台指纹。预估均为**动态指令账**,非墙钟承诺(教训:机械可删≠墙钟)。

### W-α RA 合并器:guest 寄存器读写的零指令化(先行,无依赖)
- 内容:仿 FEX LoadRegister/StoreRegister 合并——GetHostGPR/SetHostGPR/LoadUniform/StoreUniform 在 RA 阶段做反向亲和扫描,与 pin 家/静态家合并成功即消指令;重点消灭 32 位写零扩展发布点的 `lsr #0`/`ubfx`/`mov` 成串桥。
- 目标账:move/桥(coremark 38.8%、smallpt 29.6%、c-ray 30.4%)中的发布/桥接部分;预估可消 1/3–1/2。
- 风险:中。纯 RA 层,有既有 tie 基建(intwidth_tie/EA tie/induct tie 同族);sqlite 根修刚证明 tie 类优化的生命周期证明必须完整,验收须含全 oracle+指纹+sqlite 五态。
- 同时是 W-γ 的前置(合并器是组合二的腰)。

### W-β flags 寄存器化:PF/AF 专用 host GPR + NZCV 恒驻深化
- 内容:PF/AF 从 x26 packed 位域迁到两个专用 host GPR(FEX r26/r27 同构),块边界免物化;NZCV 在直接链接/内部化边间保持 host PSTATE 驻留,消灭 MergeNZCV 4 条/脏边。fault/signal/SMC 的 deopt 契约按 W79 审计的障碍清单逐条闭环。
- 目标账:flags 8–11% + 边界账中 MergeNZCV 部分。
- 风险:高。x26 是全仓 flags 枢纽,helper/signal/SMC 路径都要跟改;必须带 SVM_FLAGS_*=0 现场回退。

### W-γ XMM 组合移植:静态家 + 合并器 + 边界不落地
- 内容:16 XMM → v16-v31 固定家(或先证 8 个热家),跨块常驻;LoadUniform/StoreUniform(xmm) 经 W-α 合并器归零;dispatcher/helper 边界统一 fill/spill 点。
- 目标账:smallpt 16.8%/c-ray 18.3% 的 state 账(88–94% 是 XMM)+ move 账中 XMM 相关部分。2.5× 带的最大单项。
- 风险:高。**必须组合交付**,单独映射已证净负(W76);先交合并器(W-α)再交驻留。

### W-δ 单元内化转正:multiblock 形态的边内部化
- 内容:在 W-β 落地后重评 region edge 内部化(`0311401` 基建已在,默认 OFF):扩大前向窗、跨函数跟随 Jcc 链、条件边单 `b.cond` 化。
- 目标账:边界账 12–18%(coremark 最痛)。
- 风险:中。SMC/信号抢占契约(W79/W81 已有 latch 基建);转正前 A/B 裁定。

### W-ε 常量地址物化提升(独立,可立即做)
- 内容:rip 相对全局地址(movz+movk+mov 3 条)在同 unit/同循环内重复物化——做 unit 内常量地址缓存(占一个空闲 scratch 或复用 EA tie 基建),或 loop 不变量提升。
- 目标账:smallpt/c-ray top unit 的 5–11%。
- 风险:低-中。注意 ADRP/literal pool 两路线已实测否决(勿再走),只能走寄存器缓存路线。

### W-ζ 间接分支瘦身:块内联 L1 + shadow stack(独立)
- 内容:间接 exit 从"Str current_loc + Ret + trampoline L1/L2 ~16–18 条"改为块内联 L1 快查(~6 条);RSB push 6/pop 9 条对标 FEX shadow stack 2/3 条。
- 目标账:zip7 主残余(0.662),coremark 次要。
- 风险:中。dispatcher 契约改动面集中;与 W-δ 解耦。

### W-η 回边 safepoint 免税(独立,小)
- 内容:每自环边 Ldar+Cbnz 2 条 acquire 轮询 → FEX 式 fault page/dispatcher 层信号处理。W81 P0 latch 已证契约,需设计信号安全的 suspend 方案。
- 目标账:小循环固定 2 条/迭代。
- 风险:中(信号正确性)。

## 5. 路线图与预期

```
W-α(合并器) ──┬──> W-γ(XMM 组合) ──> smallpt/c-ray 2.5× 带主攻
              │
W-β(flags) ──┴──> W-δ(单元内化) ──> coremark/STREAM 中场主攻
W-ε(const)、W-ζ(间接)、W-η(safepoint) 独立并行 ──> zip7/osslaes 带
```

- 第一波(α+ε+ζ 并行):目标 coremark 0.456→0.55+、zip7 0.662→0.75+、smallpt/c-ray 各收 5–10% 账;
- 第二波(β→δ):coremark 边界+flags 合计 ~26% 账的主收割,目标 0.6+;
- 第三波(γ):smallpt/c-ray 的 state+move 近半账目,目标 2.5×→1.8× 量级。

全部以密度账+oracle+指纹为门,墙钟在安静窗口复测裁定;每项带 SVM_*=0 现场回退。

## 6. 明确不立项(已实测否决,防重复立项)

- NaN guard 任何后续(动态账已 0);
- spill 类(spill ≈0,evict/precise 已收割);
- GPR 全 pin level 3(−10.18%);
- XMM 静态映射单独移植(W76 净负;必须走 W-γ 组合);
- direct link 再优化(已打平);
- ADRP/literal pool 常量路线(修复点 G 已否决,序列化假设冲突);
- move coalescing 泛化审计(0.805–2.2% 池,低于 5% 门)——W-α 是**发布点定向**合并,与已否决的泛化路线不同构。
