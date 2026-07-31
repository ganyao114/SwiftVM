# SwiftVM 项目状态（2026-07-31）

本文档记录当前里程碑状态、已验证能力、性能快照与已知问题。架构细节见 [ARCHITECTURE.md](../ARCHITECTURE.md)，x86 指令覆盖清单见 [x86-instruction-census.md](x86-instruction-census.md)。

## 一句话状态

x86_64 guest → 自定义 IR → host ARM64 JIT(vixl) 的 DBT 主干在真实 glibc/musl 静态二进制上端到端验证通过；多线程 guest(clone/futex)、TSO 内存序、SMC 自修改代码（含 MT 安全回收）、SSE2 基线、x87(opt-in JIT）均已落地；FlagsElimination 放开 Carry Gate B（块内全路径覆盖死写删除）；翻译阶段七项分解探针（SVM_PROF2）、单块专用 RegAlloc 快路径（byte-identical）、UniformElim 早退剪枝、IR 构建消重（输出恒等）、decode 前端细分探针（SVM_DECODE_PROF）与 lowering 桶细分探针（SVM_LOW_PROF）、top-N distorm 快速通道、vixl EmissionCheckScope 快路径均已落地；syscall 面已覆盖七个主流 benchmark 语料（coremark/stream/smallpt/sqlite-speedtest1/c-ray/7zip/openssl-speed 全部跑通且输出与原生一致），FEX 对比基线已建立；执行侧优化落地：XMM0–15 静态驻留 + SSE NaN 快路径（W13 双开关默认 OFF，组合 geomean 1.205）、UniformElim 路径交汇（W14 默认 ON，coremark +8.6~8.9%）、x86 AES-NI/PCLMUL/SHA256 NI（W15，aes-128-gcm ~9×；SHA 随信号根治默认转正，sha256 7.5×）+ PCLMULQDQ imm=0x11 直发 PMULL2（W21）、ZExt32→64 单节点折叠（W19 默认 ON，32 位 GPR 写每处省一条 host mov）+ XMM uniform 转发开关化（W22 默认 ON ≡ 现状，审计量化既有收益）+ rt_sigreturn 内核语义重写（W20f，P0#3 根治：删 guest 栈私有帧，uc+fpstate ABI 恢复）+ 标量 SSE tied-destination/FEAT_AFP scalar-insert（W23 默认 ON，smallpt 1.38×）+ 标量 SSE 操作数 V 类化（W27 默认 ON，smallpt 再 1.23×）+ GHASH/AES FEX-style lowering（W28 默认 ON，GHASH 热 unit −31.9%）+ 窄 load 融合/GPR 常量 shift（W29 默认 ON，coremark +3.8%；项B 后经 baseline2 事故→根因修复→恢复默认，见 W34）+ XMM load→load SSA 转发（W31 默认 ON，GHASH 再 −7 条）+ 精确 NaN cold path（W37 默认 ON，smallpt 1.274×）+ unit-local flags 四件套（W38 默认 ON，coremark +4.86%）+ 结构化 AddressMode（W39 默认 ON，STREAM 几何 +3.6%）；x64 decode 入口重构（W40 纯重构，Decode 473→3 行、DecodeSwitch 六家族分文件，最大 McCabe 511→140）；SHUFPS 直降 NEON（W45）、SMC dirty hint（W46）、COMIS 紧凑 flags（W47）、JIT cache key 修 warm 全 miss（W48）均已落地（默认 OFF 开关，待翻盘）；GPR 全 pin 前置四件套 W50–W53 已合并（uniform 失效范围本地化、x87 TOP 专用寄存器退役、VIXL scratch 逐发射契约、Linux guest=host 恒等映射，全部默认 OFF）；pin 三阶段 W55/W56/W60 落地（16 GPR 全驻留能力交付，level 3 实测净负维持 opt-in）、XMM fault sink（W57）、SSE4.2 string 内联（W58）、branch-only flags（W59）、Linux 全量测试根修（W61）均已合并；**翻盘波次收官：SHUFPS_NEON/XMM_FAULT_SINK/FLAGS_BRANCH_ONLY/PIN_EXT=2+XPOOL 四项经受载 A/B 裁定翻为默认 ON（c-ray 1.44、smallpt 1.26、7z +5.0%、coremark 1.22 受载中位），六项维持 OFF；baseline4 受载全矩阵 SVM/FEX：coremark 0.403、sqlite 1.160、zip7 0.566、osslsha 2.494 胜、cray 因间歇停滞失真待 W64；W63 已根修 pin2/3 的 host fault 恢复缺陷（Linux 目标系统正确性门恢复全绿）；W64 已根修 c-ray 间歇停滞（退出中断竞态 → host 控制循环活锁，见下）；W65 落地 Linux x18 按 unit 条件化保留（spill 兜底 scratch，零 spill unit 逐位不变，全局保留方案实测 −3.6% 已否决）；Linux 全量 `all` 构建两处既有断裂已修（trampolines 死调用拖入 x86 前端未解析符号、jit_env 枚举与 SYS_* 宏冲突），orb 默认目标首次全量构建通过；W67 RA 形态只读探针 SVM_RA_SHAPE_PROF 落地（默认 OFF，OFF 指纹零 diff；七语料实测裁定 P1/P5 不立项、P3 静态推断确认、P2 收益面有限但真实，见下）**。master = `d9715ce`。

---

## 1. 已验证能力矩阵

| 子系统 | 状态 | 验证方式 |
|---|---|---|
| x86_64 前端（distorm） | 440/1107 mnemonic 已处理；52 特权级、598 异构明确跳过；16 个长模式非法项记录不实现 | 普查脚本 + fuzz |
| SSE2 基线 | packed int/float、scalar、cvt、ucomis 全 NEON 化 | 差分 fuzz + 定向断言 |
| x87 FPU | 默认 SoftFloat 位精确；opt-in JIT(SVM_X87_JIT=1)：栈管理/算术/比较/转换/FSQRT 内联 + TOP 虚拟化（再 opt-in) | 定向 2,049 断言 × 多模式 |
| LOCK 原子 | LOCK RMW、cmpxchg8b/16b(LDAXP/STLXP)；非对齐 LOCK→#GP(PageFatal)，非对齐无 LOCK→全局锁 | MT raw 测试 + Rosetta 仲裁 |
| 多线程 guest | clone/futex/每线程独立 Runtime + 共享 AddressSpace | clone_* 五件套 |
| TSO 内存序 | relaxed/acqrel(LRCPC 快路径 + REP 栅栏 + 栈/TLS 松弛） | litmus + 双模式矩阵 |
| SMC | 写保护 + 信号失效；MT 安全 QSBR epoch 回收（SVM_SMC_MT) | smc=99 + clone_smc_mt 512 次补丁 |
| 函数级编译 | FunctionBaseCompile 默认启用；CallLambda 函数级放开 | func_tests 3 模式 checksum |
| 静态寄存器映射 | RSP→x19、RBX→x20、RBP→x21 默认启用 | 矩阵 |
| 异常处理 | host 信号链（SMC→JIT fault→PageFatal)；非对齐 LOCK 语义按 Rosetta 仲裁 | bad_pointer=1 等 |

**9-binary 矩阵**（双 TSO 模式全过）:`hello=42 loop=186 real_hello=42 real_hello_musl=42 real_busy=0 real_busy_musl=0 bad_pointer=1 smc=99 clone_futex_smoke=0`
**func_tests**:function/block/interpreter 三模式 checksum `9f52b7d59285dbe5`
**TSO litmus**:relaxed `mp_bad>0`（检测器有效）,acqrel `mp_bad=0`（红线）
> 2026-07-27:relaxed 那一支曾**静默失效**——`mp_bad` 从 79–3173 掉到 0–1(实测 12/12
> 为 0),退出码由稳定 1 变成偶发 1。**红线没破,但红线也因此什么都不证明**:看不见
> 违例的检测器，对坏实现和好实现都报 0。根因是 `mp_data` 与 `mp_flag` 挤在同一个
> 8 字节粒度里，两条 store 在写缓冲里合并、消费者的核拿不到新 flag 而不同时拿到新
> data——窗口不是变窄，是被缓存协议关死了。修法是**每个共享字独占一条 cache line**
> (`.balign 128`)+ 四个 data 字 + 把 MP 阶段改成**按证据配额**(攒够 `MP_TARGET=64`
> 个违例就停,否则最多跑 `MP_ITERATIONS=4000000`)。修后实测:relaxed 40/40 恒
> `mp_bad=64`、退出 1(块模式 10/10 同),acqrel 40/40 恒 0、退出 0(块模式 10/10 同)。
> **不要把 relaxed 的期望改成 0** 来「修」这条测试——那是把死掉的检测器改成绿灯。

---

## 2. 性能快照（macOS arm64)

| 基准 | 数值 | 说明 |
|---|---|---|
| x87 bench 默认（SoftFloat 位精确） | 16,000,006 helper calls / ~1.63s | 基线 |
| x87 bench SVM_X87_JIT=1 | 2,000,001 calls / ~0.45s | FADD/FSUB 的 IXC 位精确守卫仍 bail |
| compare/sqrt 循环 opt-in | 40,038 → 6 helper calls | 中盘内联收益 |
| TOP 虚拟化 | stock bench 持平（0.45→0.46s) | bailout 主导，收益在 x87 密集内联段；默认 OFF |
| FlagsElimination Carry Gate B 放开（`f4d67be`） | 指纹 IR −4.37%（120,000→114,756，4400 unit 零变化零 fallback）；flag 密集 kernel host_bytes −7~8%；墙钟 branch −52%、int −32.5%、mem −10.6%、call −5.5%（bench_run.py 15 reps 交错，rel MAD <1%），fp/func_tests/x87 噪声内 | 块内被全路径覆盖的 C 写删除；`needed = Flags::All` 块出口全 live 不动，跨块保守性不变；在线删除与离线全路径分类对账（avx_real 402/402 精确） |
| 翻译阶段分解（`3f5e475` 探针实测，冷 cache func_tests） | pass 20.7%（UniformElim 单项 11.9%）/ 函数级固定 20.1%（RegAlloc 19.0%，CollectLiveIntervals 12.5%；RPO+IdByRPO×2 仅 1.14%）/ vixl 18.5% / 发布 15.9%（Disk RecordUnit 8.3%）/ IR 构建 11.8% / decode 11.4%，未归因 2.6% | SVM_PROF2 探针，17.17ns/边界扣除，默认关闭时墙钟差 +0.051%（噪声内）；发射中性双向验证（开/关指纹均零 diff） |
| 单块专用 RegAlloc 快路径（`88864e9`） | func_tests：collect_live −22.8%、regalloc −18.4%、translate −2.4~3.3%、墙钟 −2.75%；avx_real：regalloc −37.9%、translate −8.35%（11 轮交错冷 cache 中位数） | **byte-identical 硬约束**：同算法特化（dense 数组代有序 map、min-heap 代链表），`SVM_RA_1BLK=0/1` 含 host_bytes 逐行 diff 0 字节，默认开指纹零 diff；多块单元自动回退通用路径 |
| UniformElim 早退 + DSE 剪枝（`de89d99`） | func_tests：pass_uniform −24.5%、pass_total −14.7%、translate −3.2~4.2%、墙钟 −2.16%；DSE 扫描块数 −76.1%（3577→855）删除数不变 | 语料 68.94% 的块无 uniform 操作：无 uniform 块早退 + 少于两个 store 跳过 DSE，判定规则零改动；三路指纹（默认/`=0`/`=1`）均零 diff |
| IR 构建路径消重（`e15cc04`） | func_tests：ir_append −33.0%、translate −1~2.6%、墙钟 −2.0%；arena 高水位两路完全相同 | fresh 参数槽免 DestroyArg、构造后免重复 Validate、use 由模板实参直登记（免 metadata 二次扫描）；输出逐比特恒等，两路指纹零 diff |
| decode 前端分解（`7a3c5d6` 探针实测，15 语料 315 轮三状态轮换） | decode 桶内：**lowering 63.2%（188.9 ns/指令）**、distorm 14.7%（43.8 ns/次）、预分发 3.1%、外围簿记 3.5%、取指 1.9%、裸解码 0.9%、closure 12.8%；distorm top-20 opcode 覆盖 89.56%（MOV 31.02%）；VEX 裸解析 3.35 ns/次、AVX handler 143.5 ns/条 | SVM_DECODE_PROF 探针（SVM_PROF2 之上加开关），叶 scope 15.226ns/端到端 15.787ns 每边界扣除；探针开/关指纹均零 diff；结论：lowering 是唯一主体（≈翻译 7.2%），"换解码器"上限仅 ~1.7% 翻译，降为二级项目 |
| lowering 桶分解（`77d1993` 探针实测，10 语料三态轮换） | lowering 桶内：**寄存器读写/值物化 46.3%（≈翻译 3.3%）**、**flags 簿记 18.3%（≈1.3%）**、分发 10.6%、地址解析 5.8%、访存决策 3.7%、handler 剩余 15.4%；MOV 142.5 ns/条（频率 31% 成本 20%），CPUID 6518 ns/条但全语料仅 9 次；AVX 构成不同构（flags≈0、regvalue 54.6%） | SVM_LOW_PROF 逐指令 thread_local 记账 + 最外层 part 归属，26.4ns/边界回归扣除；探针开/关指纹均零 diff；结论：压缩对象是 regvalue+flags（输出恒等约束下），地址/访存路径无主体 |
| top-N distorm 快速通道（`4fb9ee1`） | distorm 调用 −75.6%（func_tests 5658→1379）；distorm 桶 −67~68%；decode 桶 −1.87~1.94%（两语料一致）；translate −0.2~0.6%（方向全正、亚 1%） | 保守裸解码 top-20 形状（MOV/PUSH/POP/RET/短 Jcc/ALU/LEA/CALL/JMP/MOVZX/SHL/SHR），不确定编码一律回退 distorm；`SVM_DISTORM_VERIFY=1` 双解码逐字段对账 52,024 次 + 109 万 SIB 扫描零不一致；指纹两态零 diff |
| vixl EmissionCheckScope 快路径（`7e2a9c5`） | vixl 桶分解：Emit* 外层 49% / 编码本体 34.4% / 码缓冲 14.7% / 池修正 1.8%；两语料 buffer 增长 0 次（增长策略路线数据不支持）；实测 codegen 桶 −3.8~8.1%、translate −0.05~4.16%（高负载下方向一致） | literal/veneer 池均空 + 缓冲充足时跳过 EnsureSpaceFor/pool 协议（该条件下原路径确定性无动作）；VIXL_DEBUG 恒走通用路径；lldb 禁 ASLR 1,145 unit 490KB host 文本两态 cmp=0；`SVM_VIXL_FAST=0` 回退、`SVM_VIXL_PROF` 细分探针 |

### FEX 对比基线（2026-07-31 baseline2，`SwiftVM-bench` harness，暖 cache，5 reps median，同一静态 x86_64 二进制三环境）

baseline2 = W23–W31 全部落地后的站位（baseline1 数字见 git 历史）。zip7/osslsha/osslaes 为本轮新接入的三格。

| benchmark | metric | SwiftVM | FEX(优化全开) | Rosetta | SVM/FEX | 胜者 |
|---|---|---:|---:|---:|---:|---|
| coremark 95k iters | iters/s ↑ | 7,220 | 25,970 | 30,203 | 0.278 | FEX |
| stream Copy | MB/s ↑ | 50,047 | 71,143 | 22,969 | 0.703 | FEX（**SVM 赢 Rosetta 2.18×**）|
| stream Scale / Add / Triad | MB/s ↑ | 19,153 / 25,934 / 21,058 | 73,708 / 78,959 / 79,294 | 62,911 / 71,893 / 80,877 | 0.26 / 0.33 / 0.27 | FEX |
| smallpt 64spp 320x240 | s ↓ | 24.57 | 5.43 | 5.56 | 4.52 | FEX（baseline1 为 7.43，已收窄）|
| sqlite speedtest1（文件模式） | s ↓ | 18.94 | 11.24 | 13.21 | 1.68 | FEX |
| c-ray scene 64spp 320x240 单线程 | s ↓ | 39.50 | 14.99 | 14.47 | 2.64 | FEX |
| 7zip b -mmt1 | Tot MIPS ↑ | 1,924 | 4,778 | 4,909 | 0.403 | FEX |
| openssl sha256 16k | kB/s ↑ | 455,955 | 180,538 | 390,269 | **2.526** | **SwiftVM** |
| openssl aes-128-gcm 16k | kB/s ↑ | 1,252,825 | 182,651 | 343,134 | **6.859** | **SwiftVM** |

**baseline2 事故与修复（W34，2026-07-31）**：baseline2 首轮暴露 W29 项B（窄 load 融合）正确性回归——stream/smallpt/sqlite 全部 PageFatal（故障 rip 均非访存指令，因为 fault table 是 unit 粒度，报的是 unit 入口）。env 二分坐实 `SVM_MEM_NARROW_FUSE` 单开关致灾 → 当天翻默认 OFF（`42d5dbd`）→ 根因修复（`80ec633`）：**GetOperand 剥除越过 RA 生命周期边界**——emitter 消费源 SSA 寄存器，但 RA 认为源已死亡把寄存器分给了后续值，store 拿 value 当地址。修复 = RA 新增 GetOperand→memory tie（源 interval 恰终止于 GetOperand 时转移寄存器所有权）+ emitter 两处 SharesGPR 硬守卫（无 tie 自动回退旧 transport）+ 回归用例。独立复验后恢复默认 ON（`6c590f9`，orchestrator 4 对交错 coremark **+2.78%**，4/4 正）。**流程教训（进门禁）：memory 类优化的合并门禁必须含 benchmark 正确性 cell（stream Validates + smallpt/sqlite 完成）——swift_test+func_tests+fuzz+指纹均未覆盖该形状。** 另：coremark svm cell 首轮"Errors detected"非代码问题——SVM 提速后 78k 迭代 9.8s 跑完低于 CoreMark 10s 门槛，`COREMARK_ITERS_SVM` 已升 95k。

**W33 STREAM AddressMode spike（2026-07-31，结论：立项，W39 已派出实施）**：STREAM 地址 SSA 链各自独立（CSE 不共享），窥孔路线不可行 → 结构化 AddressMode（decoder 保留 base/index/scale/disp 到 emitter）。原型实测：四热 unit 299→268 host（−10.4%），A/B 几何 1.048×，指纹 104 unit 全 decrease-only；4GB wrap 正确形式 = 最终 W+UXTW 收尾，不 prebias。**LDP/STP 配对否掉**（双 guest RIP fault attribution / partial commit / window wrap 三证缺一）。

**W35/W36 剩余差距分解（2026-07-31，指导 W37/W38 立项）**：
- **smallpt 4.5×**：最大单项 = **精确 x86 NaN 修正占动态样本 41.03%**（17 个标量 FP op 各 13 条修正；top unit 静态 48.5–61.3%）；guest `__sincos_sse2` 占 12.58% 但同为 codegen 膨胀非 ABI；块链接命中率 99.9997%、dispatcher 仅 1.13%——**调度轴已死**。→ W37 = 精确 NaN rare slow path（fast 直发 host FP + unit 内 cold stub 重建 payload/SNaN/负 indefinite，bit-exact 预估 1.15–1.28×；宽松旗标对 1.318× 已有但不作默认）。
- **coremark 3.6×**：CRC 内环 SVM 59 条 vs FEX 22 条稳态；差额 43.2% = flags 簿记（16 条/轮：MRS/MSR 6 + PF/AF/窄 CF 簿记 13 vs FEX 3）、35.1% = GPR state 访问（13 条——FEX 全 GPR pin 的真实结构收益，但 W32 已否我们的版本）、21.6% = move/宽度桥。BlockLink 仅 1.69%、function-unit 覆盖 100%（但 unit=单块）。→ W38 = unit-local flags pack（窄位宽对齐/cfinv/terminal Jcc 直消/VecFCmp 融合，预估 coremark +3~6%），**严禁跨 unit**（W30 雷区）。
- 共同结论：FEX 剩余结构优势 = 全 GPR pin + 本地 flags 表示；对应的跨 unit 方案已被 W30/W32 机械负账否决，只能做 unit 内瘦身，逐项逼近。

**W37 精确 NaN cold path（2026-07-31，`911a3c2`，`SVM_SSE_NAN_COLDPATH` 默认 ON）**：W35 立项落地——标量+packed VecFAdd/Sub/Mul/Div(32/64) 与 FSQRT 的快路径只剩 host FP 指令 + 一条结果自比较条件分支（NaN 身份判定）；命中 NaN 才跳 unit 本地共享 cold stub 重建完整 x86 语义（operand-1 payload 优先、SNaN quieting、负 indefinite QNaN、lane 选择）。cold ABI 用 RA 保留寄存器 ipv0–3(v11–v14) 与 atomic_pair_scratch(x13)；stub 在 Translate() 返回前发射计入 JitContext 尺寸。`=0` 逐字节恢复旧内联 lowering；`SVM_SSE_NAN_FAST`（宽松）仍优先。验证：swift_test 两态 114/126,903（含 Unicorn）、指纹两态 vs golden 零 diff、func_tests 三模式两态、smallpt 64spp 输出与 OFF 逐字节一致；orchestrator 安静机 5 对交错 smallpt A/B **1.274× 中位**（OFF≈18.78s ON≈14.77s，codex 受载机 1.200× 方向互证）。

**W38 unit-local flags 四件套（2026-07-31，`764b6f7`，四开关默认 ON）**：W36 立项落地——①`SVM_FLAGS_NARROW_ALIGN`：8/16 位 ALU 单 typed Add/Sub+SaveFlags，后端符号位对齐 W[31] 一次 adds/subs 后 lsr 截断，PF 复用结果；②`SVM_FLAGS_CFINV`：FEAT_FlagM 上已知极性 ADC/SBB 前一条 cfinv 归一化 host-C；③`SVM_FLAGS_TERMINAL_JCC`：紧邻 CMP/TEST + 唯一消费 branch/select 折叠为直接 B.cond/CSEL（CMPXCHG/INC-DEC/locked 排除）；④`SVM_FLAGS_FCMP_FUSE`：UCOMIS/COMIS 改 PublishFCmpFlags（非 flag-setting 整数指令发布 CF/PF/ZF、清 OF/SF/AF），FCMP 的 NZCV 直供紧邻消费；新 IR LocalCondSet/FCmpCondSet/InvertCarry/PublishFCmpFlags 含解释器等价实现，EQ/NE 回退。**合并期 orchestrator 抓获并修复一处 codex 未见的预算越界**：窄对齐最坏同时租 4 个 scratch（左右 tied 各一 + 立即数/移位右操作数一 + SaveAuxiliaryCarry 内部一），Add/Sub ScratchBudget 3→4——codex 构建无 Unicorn 跑不到触发它的 fuzz 用例。CRC 热 unit flags 簿记 19→12、静态 68→61。验证：swift_test 两态 114/128,352、func_tests 三模式两态、指纹 OFF vs golden 零 diff、CoreMark 两态 validated 且 CRC 一致；orchestrator 安静机 5 对交错 coremark A/B **+4.86% 中位**（落在 W36 预估 +3~6% 内）。

**W39 结构化 AddressMode（2026-07-31，`161f2fa` + 默认 ON 翻转 `0c8da57` + golden `29f9993`）**：W33 spike 正式化——普通非 TSO/atomic V128 load/store 以结构化 ir::Operand 携带地址（absolute/base/base+disp/index<<scale/base+index/base+(index<<scale)，三term 形状回退）；地址 GPR 前端值编号 + 闭集六 opcode（MOVAPS/MOVAPD/MOVNTPS/MOVNTPD/ADDPD/MULPD）跨指令复用，alias 写按 X86RegInfo::index 失效父 GPR，非白名单降前清空不能再播种；4GB 窗口严格 bias+((base+index) mod 2^32)（W+UXTW，无 prebias）。STREAM 四热 unit 281→268 host（W34 窄融合已先吸收 18 条），全语料 −510 IR/−365 host、104 unit 全减不增。三个新定向测试：4GB 回绕、跨页第二访问 fault、partial alias 失效。codex 默认 OFF 交付（受载 A/B 1.034× 建议安静机复测再翻）；orchestrator 安静机 6 对交错 STREAM A/B **四 kernel 24/24 全正**、几何 +3.6% → 当日翻默认 ON。指纹 golden 重生成：779 unit 全减不增、−4,821 IR（W38+W39 合计）。

**W40 x64 decode 入口重构（2026-07-31，`4261b1a`，用户点名的纯重构）**：`Decode()` 473 行/McCabe 57 → 三行委托 + 显式 DecodePipeline（fetch/predispatch/distorm/lowering 各成 stage，StepResult 枚举替代隐式 continue/break/return false）；1,225 行/503 case 的 DecodeSwitch 移入新文件 decoder_dispatch.cc 按 base/x87/SSE/SSE-float/SSE-misc/extended 六家族拆分，最大单函数 McCabe 511→140；家族分派与内部 stage always_inline 消除 +1.8% 函数边界成本。decoder.cc 3,044→1,913 行。零行为变化硬证据：指纹 vs golden 零 diff、swift_test 114/126,903、func_tests 三模式 checksum 不变、翻译墙钟 +0.36%（±1% 内）。

正确性：baseline2 全部 8 项 oracle 三环境签名一致（复跑后）。FEX 配置：`FEX_MULTIBLOCK=1 FEX_ABILOCALFLAGS=1 FEX_CACHEOBJECTCODECOMPILATION=1`（FEX-2405-222）。为跑通语料修的 ABI 缺陷：AT_FDCWD 零扩展识别（`927ba2e`）、pwrite64/rt_sigaction/rt_sigprocmask/time/sched_getaffinity/clock_nanosleep + 只读 MAP_SHARED 快照（`b086cf2`）、fsync/fdatasync/ftruncate（`6ec0114`）。

### FEX 对比基线（2026-07-31 baseline3，同 harness，暖 cache，5 reps median，24/24 oracle cell ok=1）

baseline3 = W37/W38/W39/W40 全部落地后的站位（当前权威基线）。全部 10 项相对 baseline2 提升。

| benchmark | metric | SwiftVM | FEX(优化全开) | Rosetta | SVM/FEX | 胜者 |
|---|---|---:|---:|---:|---:|---|
| coremark 95k iters | iters/s ↑ | 8,748 | 28,585 | 32,804 | 0.306 | FEX（baseline2 0.278）|
| stream Copy | MB/s ↑ | 56,748 | 82,126 | 26,664 | 0.691 | FEX（**SVM 赢 Rosetta 2.13×**）|
| stream Scale / Add / Triad | MB/s ↑ | 27,645 / 38,092 / 32,808 | 80,140 / 89,136 / 89,754 | 74,402 / 87,376 / 94,664 | 0.345 / 0.427 / 0.366 | FEX（三项相对 baseline2 +44/+47/+56%）|
| smallpt 64spp 320x240 | s ↓ | 14.94 | 5.34 | 5.59 | 2.798 | FEX（baseline2 4.52，W37 主贡献）|
| sqlite speedtest1（文件模式） | s ↓ | 15.19 | 10.84 | 13.64 | 1.401 | FEX（baseline2 1.68）|
| c-ray scene 64spp 320x240 单线程 | s ↓ | 19.00 | 13.22 | 13.62 | 1.437 | FEX（baseline2 2.64，近乎翻倍）|
| 7zip b -mmt1 | Tot MIPS ↑ | 2,131 | 4,476 | 4,949 | 0.476 | FEX（baseline2 0.403）|
| openssl sha256 16k | kB/s ↑ | 445,052 | 177,076 | 424,204 | **2.513** | **SwiftVM**（也超 Rosetta 1.049×）|
| openssl aes-128-gcm 16k | kB/s ↑ | 1,278,514 | 2,062,191 | 3,197,952 | 0.620 | FEX |

**osslaes baseline2 数字作废（测量诚信教训）**：baseline2 的"6.859× 胜"是假象——FEX 该轮 rep_1 正常（1,015,822 ops/8s ≈ 2.0 GB/s）但 rep_2–5 集体劣化 10–17×（59k–92k ops），median 恰好落在劣化样本上；baseline3 的 FEX 五 reps 全部稳定在 ~1.0M ops（2.0 GB/s），与 baseline2 rep_1 互证。**真实站位 = SVM 1.28 GB/s vs FEX 2.06 GB/s = 0.620（FEX 胜），Rosetta 3.2 GB/s 表明天花板还远。** GHASH/XMM-state 路径（W26 已标记）重新成为优化目标。教训进流程：跨轮倍率异常（尤其 >3× 的"胜"）必须逐 rep 审查原始输出再记账。

**W41–W44 四格差距分解（2026-07-31，baseline3 站位，全部 orchestrator 复核后立项）**：
- **W41 smallpt 2.80×**：W37 后主矛盾已从 NaN（41.03%→8.95%）转为 **XMM state 访问 32.01%**（+GPR 4.24%，合计占 gap 45–56%）；flags 9.99%（COMISD 非终端路径 **23 条 vs FEX ~3 条**：PublishFCmpFlags 单独 2.054%）；NaN 残余 8.95%（cold stub 三轮零命中，只剩结果自比较门槛）。`__sincos_sse2` 12.72% 与 W35 持平，是通用膨胀受害者不独立立项。候选：fault-aware XMM store sinking（6–11%，高风险）、FCMP 紧凑发布（1.4–2.1%，**已立 W47**）、NaN guard 合并（2.2–4.5%，极高风险暂缓）。
- **W42 c-ray 1.44×**：最大单块肉 = **SHUFPS 的 ShufpsHalf C++ helper——leaf 热点 13.13%**，top-15 内 64 个 call site × 29 条 ABI 外围；FEX 直降 DUP/ZIP2/EXT。transform 三元组 code-size 差 7–8× 全因 helper 出 JIT。SSE 标量 lane 保持已清空（INS/UMOV=0）、FCMP 融合覆盖 69% 终端分支、除法开方密度不成立。**已立 W45（SHUFPS NEON 直降 + VecHalfOp 家族审计，预估 12–18%）**。
- **W43 sqlite 1.40×（首次分解）**：时间大账 = guest JIT 9.07s / **syscall 4.28s（其中 4.00s 是真 host I/O，F_FULLFSYNC 语义不能动）** / 翻译 0.35s / 其它 1.49s。两个可立大项：**SMC CloseWriteWindow 每次 JIT 返回无条件取 mutex+扫 pages_ = 0.99s/6.5%（已立 W46 dirty hint）**；**JIT cache key 把全部 argv 纳入 hash → harness 每 rep 路径不同，warm cache 实为全 miss（已立 W48，预估 2.0–2.4%）**。guest 侧：move 38.7%（pcmpistri 8 guest→153 host、pcache1Fetch 除法 helper 78 条 move）、SSE4.2 string helper（3–6% 候选暂缓）、unit-local 窄 flags（2.3–4.9% 候选暂缓）。W16 的"rep movs 零命中"不可移植：sqlite memcmp+memmove+strcspn = 11.27%。
- **W44 7zip 0.476（首次专项）**：热区 = LZMA range decoder/Bt4 match finder/CRC slicing-by-12。样本加权 host 构成：**flags 簿记 27.8% + move/宽度 25.8% + GPR state 12.0%**（7.559 次 GPR uniform access/exit）。乘法已是单条 mul w（无专项）、间接分支 0.0018%（无肉）、memcpy/rep 假说再次零命中。byte-CMP `lsl/subs/lsr` 证明 W38 窄对齐已生效，但 terminal Jcc 之后仍完整物化 x26/AF/PF/carry 极性（FEX 同形状 4 条 vs 我们 36 条）。候选：unit-local GPR cache（8–18%，高风险，W32 边界需谨慎）、branch-only flags（4–9%，须证两后继 flags 全死，W30 边界）、flags polarity DCE（1–3%）、CL shift lazy flags（0.3–1.2%）。**暂缓，先消化 W45–W48。**
- 共同结构主题：state 访问（GPR/XMM uniform 往返）是四格共同第一大类，FEX 全 pin 的结构优势被四次独立量化；在 W30/W32 边界约束下只能做 unit-local 瘦身逐项逼近。

**W45–W48 四项落地（2026-07-31，均默认 OFF 交付，orchestrator 逐项独立复验合并）**：
- **W45 SHUFPS 直降 NEON（`2b1c437`，`SVM_VEC_SHUFPS_NEON`）**：新 IR VecShuffle32TwoSrc + 解释器等价；单指令特化（ZIP1/ZIP2/EXT/UZP1/UZP2 + alias DUP/EXT/ZIP/TRN/恒等/0xE0/0xE5 单 INS）、通用双表 TBL（新连续 V 寄存器对申请接口）、高压回退 TBL+TBX，全程不出 JIT。256 imm × 寄存器/内存/alias × JIT/解释器 = 1536 次定向执行（ShufpsHalf 作 oracle）。c-ray IDAT/smallpt/STREAM 两态一致，指纹零 diff。预估收益 12–18%（W42），待安静机 A/B 翻盘。
- **W46 SMC dirty hint（`246c8b9`，`SVM_SMC_DIRTY_HINT`）**：CloseWriteWindow 无待办免锁快速返回。不变量：HandleWriteFault 开页/置 dirty 前 seq_cst 发布 pending、RegisterNode 脏页补发、Retire 入队前发布；清除只能在 invalidation_mutex+metadata_lock 下确认全页非脏且 retired_ 空。双门控（hint+pending_count_），失误方向只会多走慢路径。sqlite 实测 close 100% 命中快路径、累计耗时 −96.97%；新增两组真实 tracker MT 测试。预估 sqlite 4.9–6.3%（W43），codex 受载 A/B +12%（待安静机复测翻盘）。
- **W47 COMIS 紧凑 flags（`40deaf3`，`SVM_FLAGS_FCMP_COMPACT`，需 FCMP_FUSE ON）**：通用路径 23→5 条（FCMP+CSET VC+BFI+BFC+AXFLAG）；AXFLAG 的 C 为 x86 CF 反相，复用 W38 carry polarity；FCmpCondSet 终端条件重映射；FlagM2 sysctl 探测写 Config、IR Imm 固化 unit 决策，无 FlagM2 自动回退。定向矩阵 4 变体×15 关系×JIT/解释器对账 + 16 Jcc/SETcc/CMOV + LAHF/PUSHFQ + ADC/SBB。1140 host unit 960 不变/180 减/0 增。预估 smallpt 1.4–2.1%（W41），待安静机 A/B 翻盘。
- **W48 JIT cache key（`ddfd262`，`SVM_JIT_CACHE_EXEC_ID`）**：=1 时 guest cache 身份 = 域标签 + argv[1] 文件身份，排除数据 argv。修掉 harness 每 rep 路径不同致 warm cache 全 miss：实测 OFF 跨路径 loaded=0 compiled=14721，ON loaded=14724 compiled=1。正确性边界不变（逐 block guest 字节 hash，替换二进制同路径同 inode/size/mtime 仍 reject 重编）。预估 sqlite 2.0–2.4%（翻译 0.35s 确定性消除），待翻盘。
- 合并期核查记录：W45 各 imm 特化形状逐一手算映射复核、ScratchBudget 最坏 3FPR+1GPR 恰好等于默认预算（无需加项）；W46 全部 dirty 生产者（唯一 `rec.dirty=true` 点 + 两个提前返回路径）与 close 调用方无"是否做事"依赖逐一核对；W47 的 AXFLAG 极性与 interpreter/decoder 三方一致性核对。perf_stats.h kGetenvNames 现 44 项。swift_test 全并后 120 cases / 131,889 assertions 两态全绿（含 Unicorn fuzz）。

**W49–W53 + W50：GPR 全 pin 前置四件套落地（2026-07-31，用户直接指令重开 W32 死区，均默认 OFF 交付，orchestrator 逐项独立复验合并）**：
- **W49 调研 spike（无代码合并）**：四点裁定——(1) uniform/context 已共用 x28，无可回收；(2) memory_base 消除 macOS 不可行（XNU 强制 4GB pagezero，自研 PoC 复现 KERN_INVALID_ADDRESS 与 pagezero0 SIGKILL rc=137）、Linux 可行（mmap_min_addr=65536）→ W53；(3) x16/x17 VIXL scratch 可按发射契约回收 → W52；(4) x22 x87 TOP 默认本就不占 → W51。目标 pin 布局 RAX→x22/RCX→x23/RDX→x29 + 现有 RSP/RBX/RBP→x19/20/21，临时池 x11–x17 共 7；全 pin PoC 实测 CRC inner loop 61→43 host 指令（orchestrator 自建复测两数一致）。W32 三杀手（全表失效 +18、partial-write 桥接 +48、隐藏 scratch）各获定向修复（W50/W51/W52）。
- **W50 uniform 失效范围本地化（`feb2ec2`，`SVM_IR_UNIFORM_RANGE`）**：mapped GPR store 全宽 U64 时按字节区间更新 facts（保留未覆盖字节及其它 uniform facts），取代全表失效；helper uniform 写副作用集机制（Lambda ArgType 高位置标 + 注册表），已标注 div/XSAVE/XSAVEC（pure）、RDTSC（rax/rdx 各 4B）、WRPKRU（interrupt+pkru）、XGETBV（rax/rdx/interrupt），未标注 helper（含 XRSTOR）保持全表失效；helper 反向 DSE 维持全 barrier（effect set 不描述读）。orchestrator 自建实测：全失效 1768→460（−74%）、range 失效 1308、保留字节 facts 21,998、DSE victims 884→916。swift_test 两态 122 cases/131,901 assertions（新增 2 测试用例）；func_tests 六格 PASS；指纹 OFF 零 diff（ON 70 unit 纯下降 −217 IR）。
- **W51 x87 TOP 专用寄存器退役（`1ef5c63`）**：删 449 行（CFG affine 分析 + 3 处 reservation）；TOP 改为指令局部提取（FSW 已载则 UBFX，否则 LDRH+UBFX），stack effect 并入 FSW 写回；新增 static descriptor 与 ABI/已有 descriptor 重叠的通用 ASSERT。多 x87 压力 unit 1324→1273 host 指令。x22 成为全 pin 候选。
- **W52 VIXL scratch 逐发射契约（`94cf351`，`SVM_JIT_SCRATCH_XPOOL`）**：TickIR 开 UseScratchRegisterScope 允许 x11–x17 减去 pinned/live/operand/fixed-clobber/leased；VIXL AcquireNextAvailable 挂钩（VIXL_CHECK 违约即炸，负向实验已验证能抓）；FixedGPRClobbers 表（atomics/CAS128/CallLambda 系/VecF* NaN-cold/块终端）+ RA 双重防护（IntervalFixedClobbers + RecordActiveRegs）；GetSharedTmpX OFF 态 byte-identical。4400 host unit 0 增 44 减 −240 指令；ON 态 swift_test 138,182 assertions（池宽敏感 spill 测试 +6,293）。
- **W53 Linux 恒等映射（`2b06ff4`，`SVM_MEM_IDENTITY`，仅 Linux）**：ET_EXEC PT_LOAD span 按链接地址 MAP_FIXED_NOREPLACE（严格返回地址校验拒绝 pre-4.17 内核），冲突 fatal 不做静默 bias 回退；释放 x24+x10（实测两寄存器 reservation 归零，dispatcher loc x9→x24）；coremark unit 126→110；smallpt 1693 unit 97851→97425 且 image SHA-256 不变；隔离损失已文档化（FEX 同模型），macOS 永不读该开关。捆绑 Linux bring-up 修复：code_cache RW/RX memfd 双映射（修 Linux SEGV_ACCERR）、glibc uc_mcontext、SYS_*/CLONE_* 宏冲突、GNU as bfc→and 等。
- **合并后 Linux 复验（orb Ubuntu）**：master `feb2ec2` 在 orb 重新构建，hello rc=42、func_tests 三模式 × SVM_MEM_IDENTITY 两态 × SVM_IR_UNIFORM_RANGE 两态全 PASS（对照 native.txt，rc=101）。W51+W52+W53+W50 交互在 Linux 首次验证。
- kGetenvNames 现 47 项。待翻盘开关队列：SVM_VEC_SHUFPS_NEON、SVM_SMC_DIRTY_HINT、SVM_FLAGS_FCMP_COMPACT、SVM_JIT_CACHE_EXEC_ID、SVM_JIT_SCRATCH_XPOOL、SVM_IR_UNIFORM_RANGE、SVM_MEM_IDENTITY（Linux 侧翻盘）。

**W54/W55：pin 战役第二批落地（2026-07-31，均默认 OFF，orchestrator 独立复验合并，orb Linux 复验全绿）**：
- **W54 helper ABI 纯值化（`8022dfb`，`SVM_X86_HELPER_VALUES`）**：RDTSC/RDTSCP、XGETBV、WRPKRU、RDRAND/RDSEED 不再读写 ThreadContext64，改参数/返回值交换；frontend 显式 StoreUniform 写 guest 寄存器，fault-only interrupt reason 经 NotGoto 局部分支写入，fault 时完整保留原 RAX/RDX/PKRU（codex 自查抓到 fault-preservation 缺陷并修复）；目标 helper site 全失效消除（random_smoke 定点 full 1→0）。XGETBV 高位提取 U64 producer + U32 cast view，避开 W 寄存器 UBFX 非法编码。
- **W55 pin RAX/RCX/RDX（`82f7e8d`，`SVM_X86_PIN_EXT`）**：W49 计划 B+G.2——x22/x23 callee-saved 零边界成本、x29 随 LR 保存；AH/CH/DH 单条 BFI、32 位写 W-write 自然零扩展；ZeroExtend32To64 单 pinned consumer 融合；窄 pinned read 无中间写时直接以 pinned W 参与 AND/XOR/ZeroExtend（SetHost 交叉检查保 snapshot）；uniform pass 对新 pin 启用 range-local facts + load-to-load forwarding、DSE 前置保死写删除；Add/Sub scratch 预算 ON +1。orchestrator 自建复验：4400 host unit 净 −3642（1639 减/403 增/2358 不变）、CRC 0x403880 61→62（pin→SSA snapshot materialization 抵消，留待阶段 2 闭合）、reg mask x22/23/29 reserved、allocatable 14→12；swift_test 四态（OFF/ON/ON+XPOOL/ON+URANGE）122 cases 全绿、func_tests 九格 PASS、sigreturn stale-frame 两态 PASS、指纹 OFF 零 diff；orb Linux 六格（含 PIN+XPOOL、PIN+IDENTITY）PASS。codex 受载 A/B coremark +13~14% 仅作方向。
- kGetenvNames 现 49 项。进行中：W56 pin 阶段 2（SVM_X86_PIN_EXT=2 追加 RSI/RDI/R8-R11→x0-x5，caller-saved helper 边界保存，CRC 硬目标 ≤52）+ W57 XMM fault sink（SVM_XMM_FAULT_SINK，独立新 pass，fault 注入矩阵为命门）。

**W57 XMM uniform store 延迟物化（2026-07-31，`50483de`，`SVM_XMM_FAULT_SINK` 默认 OFF，orchestrator 独立复验合并）**：新增独立 pass UniformStoreSinkPass（不动 uniform_elimination_pass.cpp，fault 策略可单独审计）——块内 XMM 区间的 StoreUniform 摘除为 pending SSA 值，仅在 fault/观察点前强制物化（全部 guest 访存/TSO/原子/对齐检查、CallLambda/Location/Dynamic、X87Op、GetUniformAddress、UniformBarrier、Get/SetHostGPR/FPR、Get/SetLocation、Goto/NotGoto/BindLabel、Push/PopRSB），块尾 flush_all 保证 signal 注入采样（unit 返回后）读到精确上下文；LoadUniform 全字节被 pending 覆盖时直接前递（同位宽/低 lane BitCast 视图、V128→XmmLo/Hi U64 VecExtract64），被后续选中写全字节覆盖的 pending 写删除但经 ChainWouldStrandASurvivor 保护（生产者链上有 LoadMemory/原子/call 等 DCE 幸存者时保留）。orchestrator 自建复验：swift_test 三态（OFF/ON/ON+XMM_STATIC）122 cases/131,901 assertions 全绿、func_tests 八格（含 ON+PINEXT 组合）PASS、sigreturn stale-frame 两态 PASS、helper-fault ON 38/38、指纹 OFF 零 diff、c-ray IDAT/smallpt md5 两态一致；EXEC_PROF 实测 smallpt xmm_uniform 计数 27.7 亿→21.2 亿（**−23.2%**，与 codex 报的 −23.41% 互证）；4400-unit 净账 0 变化（语料无 smallpt，热路径形态变化不进语料计数）。kGetenvNames 现 50 项。W56 仍在途。

**W58 SSE4.2 string 隐式长度家族内联（2026-07-31，`51c5695`，`SVM_SSE42_STRING_INLINE` 默认 OFF，orchestrator 独立复验合并，orb Linux 六格 PASS）**：W43 立项 #2 落地——新纯 IR `Sse42Str(V128, V128, imm8)` 承载 PCMPISTRI/PCMPISTRM 公共核心，ARM64 直发 NEON（隐式长度 Cmeq+窄化+RBIT+CLZ 定位首零元素；equal-any 运行时紧凑循环覆盖 strcspn 热形态；ranges/equal-each/equal-ordered 直线；IntRes1 位权重+ADDV 折叠；SDM 有效性覆盖聚合后标量代数；polarity/index/ZF/SF/CF/OF 按编译期 imm8 分派）；PCMPESTRI/M 显式长度保持 helper。解释器复用同一 exhaustive evaluator；OFF 保持原两段 CallLambda 逐位不变。orchestrator 复验：exhaustive（6104 行 Rosetta 实机参考、256 imm8×20 组操作数×implicit/17×17 explicit、约 149 万元组，JIT+解释器）两态 18,449 assertions 全绿、swift_test 两态 122 cases/131,903、func_tests 六格、指纹 OFF/ON 对 golden 均零 diff（语料无 PCMPISTR）、sqlite size100/main 两态 rc=0 输出一致、热 unit 0x506d74/0x506df8 153→148/155→153。**性能实话：sqlite 受载中位仅 +0.4~0.5%，远低于立项 3–6% 预估**（W43 把 84–85 条 move/save-restore 归给热 unit 的归因偏松，实测内联语义计算抵消了大部分静态收益）——按正确性基建+默认 OFF 合并，收益随翻盘波次复测。kGetenvNames 现 51 项。

**W56 pin 阶段 2（2026-07-31，`1833091`，`SVM_X86_PIN_EXT=2` level 语义，默认仍 OFF，orchestrator 独立复验合并）**：W49 计划 G.3——=1 保持 W55 逐位行为，=2 追加 RSI→x0、RDI→x1、R8-R11→x2-x5 六条 caller-saved pin（GPR descriptor 达 12 条）。命门边界：EmitHostCall 把 x0-x5 descriptor 显式并入保存集（每 helper call 固定 3 STP+3 LDP，实测 60/60 BLR 形状一致）；dispatcher halt-reason 探测改 w11、static uniform 回写后才发布 w0；level-2 无 XPOOL 时 x12/x13 按逐发射 scratch-only 出租、X87Op 走精确 SoftFloat helper（XPOOL 恢复 inline）；pool 谓词同查 env 级别与 descriptor 实际在位。新 FusePinnedWriteChains 把 ZExt32To64→SetHostGPR(pin) 链直映物理寄存器（全宽 exchange 整块保守放行；fuzz 抓获的 XCHG 双向快照与 helper U32 旧高位两类缺陷已关闭并回归）。**CRC 硬目标闭合：0x403880 62→52 条（orchestrator 自建 dump hash 与 codex 逐字节一致）**。orchestrator 复验：swift_test 四态 122 cases 全绿、func_tests 13 配置（含 =2+XMM_FAULT_SINK 跨 W57 组合）全 PASS、指纹 OFF 零 diff、sigreturn 两态 PASS、coremark =2 CRC validated。净账实话：4400 unit 静态 +2519（边界成本集中在 1.4% unit）、level-2 无 XPOOL spill 显著（func_tests 827，+XPOOL 回 24）、翻译期 reserve escalate 警告常见——codex 受载 CoreMark +2.2% 仅作方向，**level-2 翻盘必须与 XPOOL 捆绑评估**，安静机 A/B 定夺。kGetenvNames 不变（51 项）。

**W59 branch-only flags（2026-07-31，`d0120ac`，`SVM_FLAGS_BRANCH_ONLY` 默认 OFF，orchestrator 独立复验合并）**：W44 立项 #2——terminal Jcc 独占消费且两 successor flags-in 可证全死时，主 SaveFlags 转 BranchOnlyFlags pseudo：不写 x26 flags 字、不算 PF/AF、不写 polarity，host NZCV 直供 B.cond；JP/JNP 走新 IR LocalParitySet（低字节 xor 折叠）。安全性靠证明而非物化（与 W30 被否的 edge materialization 路线相反：只在零物化成本可证安全时转换）——多块 HIR backward fixed-point（空块/未知 edge 保守 All）+ lazy 单块 decoder 侧 successor 前 8 条保守扫描（BranchOnlyEdges 标记，pass 消费后清除）；producer region 内任何 flags 观察者/helper/局部分支即拒绝。orchestrator 复验：codex 新增定向矩阵（8 向量×16 Jcc + LAHF/PUSHF/SETcc/CMOV/ADC/跨 unit ADC 反例，orb x86 原生 oracle）6 配置全 PASS、swift_test 两态 122 cases、func_tests 六格（含 +PIN_EXT=2/+FAULT_SINK 组合）、指纹 OFF 零 diff、7z 两态 rc=0。净账：4400 unit host −2.33%（0 升/505 降）、ON IR −924；目标 unit 0x428140 36→25（我 dump hash 与 codex 一致）。**硬目标 ≤18/≤25 未闭合**：剩余 25 条中 LinkBlock×2+dispatcher 出口约占 18 条，flags 物化 11 条已全删——差距在出口地板（linker 禁改路径），如实记录。7z 受载方向正（中位 1.288×，含 loadavg 52 污染对），安静机复测定翻盘。kGetenvNames 现 52 项。

**W60 pin 阶段 3——全 GPR 静态驻留收官（2026-07-31，`1bbc39b`，`SVM_X86_PIN_EXT=3` 默认 OFF，orchestrator 独立复验合并）**：用户"通用寄存器优先 static 映射"指令的收官档——R12-R15 收编 x6-x9，16 条 guest GPR 全部静态驻留。level 3 自动捆绑 XPOOL（非 XPOOL 值池只剩 x14/x15 不可行；诊断区分 xpool_requested/effective/auto_level3），动态池 x11-x17 七寄存器；x10 不进值池、仅对 Add/Sub/FCvt/call 类高压 opcode 作第 8 个 instruction-local scratch；x87 固定走 exact helper fallback；page-table dispatcher 的 location scratch 仅 level 3 由 x9 移 x13（OFF/1/2 原路径不动）；helper 边界 5 STP+5 LDP；FusePinnedWriteChains 扩至 x0-x9，R12B-R15B 窄写 BFI(bfxil)；RA 加中间 reserve rung 防 7 寄存器池直跳 all-spill。**如实负账**：CRC 热 unit 0x403880 仍 52 条（208B，≤43 未达；codex 曾试到 42-43 但破坏 fuzz/glibc 主动撤回——正确性不让步的样本行为）；4400-unit host +3.89%、spill 内存操作 5425→14071（2.6×）；coremark level3 vs level2 受载五对 **−10.18%**——全 pin 的 spill+host 膨胀超过静态驻留收益（W30/W32"边界成本吃掉机械上限"同族教训；FEX 全 pin 成立因其 RA 为之塑形，我们的不是）。裁定：默认 OFF 能力交付、不翻盘；level 1/2 才是性能路径。orchestrator 复验全绿：swift_test 四态、func_tests 12 格+pin3×branch_only 交叉（W59×W60 首次共存）、指纹 OFF 零 diff、sigreturn/helper-fault/coremark =3 validated、level-3 CRC hash 与 codex 逐位一致（ec13e7172695d46a）。**合并方修正一处**：codex 的窄读融合新条款误伤 W56 已验证的 x22/23/29 U8/U16 And/Xor 直接 W 读（level 1/2 有 18/1683 unit 各 +2 指令，与其"逐位一致"声明不符）——已改为仅 level 3 保留该限制；修正后 level-2 全语料 size 零差异（残差字节差为 ASLR 地址噪声，在同二进制噪声底 30 以内）、level-3 codegen 不变（CRC hash 不变）。附：合并过程暴露 main_case.cpp 自 feb2ec2 起 Linux GCC `Imm{...ull}` 重载歧义（Darwin u64=unsigned long long 精确匹配，Linux LP64 下 ull 在 Imm(u64)/Imm(s64) 间同级歧义）——20 处字面量已修（`8c04f77`）；该编译修复首次让 Linux 跑出 W39 引入的测试对污染 bug（orb 全量套件第 20 例 SIGSEGV 中止、隔离单跑通过、macOS 全绿——疑似实例销毁后 fault handler 注册残留，W61 已立项根因调查）。orb Linux 门：func_tests OFF/=3/交叉、hello 全 PASS；全量 swift_test 待 W61。

**W61 Linux swift_test 全量中止根修（2026-07-31，`44a7fc6`，orchestrator 独立复验合并）**：W60 合并期修掉 main_case.cpp 的 Linux 编译歧义后首次暴露——orb 全量套件在第 20 例（结构化 V128 跨页 fault）SIGSEGV 中止、后续 102 例未执行；隔离单跑通过、与前一例同跑即复现。codex 根因（我逐项源码坐实）：SignalHandler 的 `g_installed` 一次性标记失真——Catch2 POSIX fatal 护栏每用例外围装卸 SIGSEGV（其列表含 SIGSEGV 不含 SIGBUS），首个构造 Runtime 的用例 teardown 把 SwiftVM action 冲掉，后续 Runtime 的 Install 被 `g_installed=true` 挡住不再重装，guest 页故障逃逸成裸 SIGSEGV；macOS 不崩因该访问在其上报 SIGBUS。修复 = 每次 Install 重新断言三个 sigaction（幂等，callback 注册本就独立 call_once）。附带：SHA2 host probe 在 Linux arm64 原恒 false，改 getauxval(AT_HWCAP)&HWCAP_SHA2（Linux 全量末段 CPUID 测试此前需 opt-out 绕过）。复验：orb 全量 122/122 连跑三轮全绿 + func_tests OFF/=3/交叉 + hello；macOS 两态 122 cases + func_tests + 指纹零 diff。

**翻盘波次（2026-07-31，用户指令受载直接跑，/tmp/flip_ab.sh 11 节 5 对交错中位，loadavg ~13；裁定口径：>2% 且 5/5 对同向才翻，全部幅度数字标记"受载中位"、以 baseline4 安静机复测为准）**：
- **翻 ON 四项**（各单独 commit、全门两态、oracle 全过）：① `SVM_VEC_SHUFPS_NEON`（`1bcfb39`，c-ray 1.4351）；② `SVM_XMM_FAULT_SINK`（`c64bbb5`，smallpt 1.2618，ppm md5 两态一致）；③ `SVM_FLAGS_BRANCH_ONLY`（`0ec5b97`，7z Tot-MIPS +5.0%；golden 重生成 4400 unit 集不变、up=0/down=394、−924 IR，与 W59 合并时 −924 精确互证）；④ `SVM_X86_PIN_EXT=2`+`SVM_JIT_SCRATCH_XPOOL` 捆绑（`c905307`，coremark 1.2207；150k 迭代 validated 12112 it/s vs baseline3 8748——95k 在新速度下跌破 CoreMark 10s 门槛的假 VALIDATION FAIL 非正确性问题，harness 已提 150k）。④的 golden 重生成首次遇到 IR 上升类（512 升/507 降/净 −330）：上升全部坐实为 pin 机制本身（uniform-elim mapped store/invalidation 节点，0x401db6 逐块统计 LoadUniform 52→0）——纯消除 pass 的 up=0 门对 pin 类翻改改为"上升类全部可解释+净降+无 func→block 回退"。
- **维持 OFF 六项**：FCMP_COMPACT 0.9986、SMC_DIRTY_HINT（sqlite 0.99/coremark 0.999）、CACHE_EXEC_ID 1.011、XPOOL 单独 1.0005、URANGE 1.011、SSE42 1.0199——全部 2% 噪声内或方向反。
- **翻盘 ④ 被一个潜伏测试 bug 阻塞并根修（`f11367d`）**：BRANCH_ONLY(ON) × PIN_EXT≥1 组合使 swift_test fuzz 段确定性致命——两特性各自的 A/B 与门禁从未覆盖该组合（PIN2 节跑在 BRANCH_ONLY 翻前二进制上）。codex 根因（我逐条源码坐实）："Fuzz x86 decode robustness" 的测试 MemIf `GetPointer` 恒等映射任意地址，BRANCH_ONLY 的 flags 死亡证明解码条件跳转后继时被随机 rel32 带到 arena 外取指——生产 MemIf 对未映射地址返回 nullptr 转 PAGE_FATAL（translator.cpp:220 的契约注释本就写明），测试 harness 对齐同一契约即可；是否踩中未映射页纯看进程地址空间布局，解释了 macOS pin1/2 必崩、level 3 与 Linux 免疫的假象组合。纯测试侧修复，生产码零改动。我独立复验：pin{0,1,2,3}×bronly{0,1} 八格固定种子全绿（此前两格必崩）、orb Linux 默认+pin1 全绿、默认断言数不变（131909/122）。
- 翻转后 master = `c905307`，orb Linux 门全绿（122/122 + hello + func_tests 两语料）；env 开关注册表 kGetenvNames 仍 52 项（翻盘不改名）。**下一锚点：baseline4 全矩阵（三环境），coremark SVM 侧 150k iters。**

**baseline4 全矩阵（2026-08-01，受载机 loadavg 10–20，24/24 oracle，coremark 150k iters；翻盘波次后首次站位）**：SVM/FEX 比值（>1 为胜；wall/render 类为慢几倍）：coremark **0.403**（baseline3 0.306）、stream Copy **0.823**（0.691）/Scale 0.393/Add 0.469/Triad 0.389、smallpt **2.660**（2.798）、zip7 **0.566**（0.476，BRANCH_ONLY 收益可见）、osslsha **2.494 保持胜场**、osslaes 0.699（0.620）、sqlite **1.160**（1.40，见下）、cray 受载失真不裁定（见下）。两个数据质量事故的根修：
- **sqlite/svm 格 n/a 事故**：regalloc 的 spill/scratch-escalation 诊断走 `LOG_WARNING` 直写 std::cout，随编译线程异步混入 guest stdout——sqlite speedtest 边跑边输出被打断行、交错位置逐次不同，5/5 rep 签名对不上 oracle 全部判 0。修为 `SVM_RA_DIAG` 开关默认 OFF（`2b212bf`，注册表 52→53）。修复后重跑 5/5 oracle_ok，且 svm 输出签名 `95723e06…` **与 FEX/Rosetta 逐字节一致**（baseline3 时代 svm 签名本就不同源，首次三环境同签名）。sqlite svm 中位 14.84s vs FEX 12.79s = 1.160。
- **cray render_s 解析 bug**：c-ray 渲染超 60s 输出 "Finished render in 1m 05s"，harness 原 awk 只取末字段得 5.0s——受载 svm 格（55–70s）被系统性低估，FEX/Rosetta（~14s）不受影响。已修（SwiftVM-bench `e3d96f3`）并离线重算：**cray svm 受载中位 ~59s vs FEX 14.88s ≈ 4.0×**。重点异常：①两轮共 3/10 个 rep 停滞到 900s CELL_TIMEOUT 被杀（非确定性，rep 序号不定，valid rep 55–69s）；②svm c-ray 对宿主负载超敏——baseline3 安静机 19s，受载 55–69s（~3×），同期 FEX 13.1→14.9（1.13×）。停滞已由 W64 根修（见下条）；负载超敏的 3× 受载膨胀与停滞不同源，cray 比值在安静机复测前不作裁定。**post-W64 受载复测（2026-08-01，cray-postw64，5 reps 暖 cache，oracle 3/3 一致）：svm 45.23s vs FEX 12.96s = 3.49×，5/5 干净完成零停滞（W64 修复在 harness 环境确认有效）；受载膨胀仍在（安静机 ~19s → 45s），超敏机制已立 W69 调查，安静机裁定待其数据。**
- **W64 已根修合并（`e41219d`+`b29ad68`，orchestrator 逐项独立复验）**：c-ray 间歇停滞根因是退出竞态——`SignalInterrupt()`（running=false + halt_reason=Signal）若落在两次 `Runtime::Run()` 之间，`Impl::Run()` 的 `while(running)` 一次不进入、hr 保持 None 返回，`X86Core::Run()` 对 None 一律重试 → 子线程 host 控制循环 100% CPU 活锁、主线程永久阻塞 JoinAll。停滞 rep 全部已完成 guest 工作并写出 142,312B PNG（翻译风暴/SMC/JIT cache/PNG 写入四假设均被取证否掉，见 `docs/w64-cray-stall-root-cause.md`；"3.1× 负载超敏"部分是 render_s 解析器 artifact，`1m 19s`→19s）。修法：空循环退出且 running 已清除时返回 Signal（SignalInterrupt 是 running 唯一清除者），CacheMiss/BlockLinkage 循环内语义不变；附两次 Run 间中断保留回归用例。同批根修 EXEC_PROF×XPOOL 探针撞穿 scratch 预算（计数器改固定 ip0/ip1 对并纳入 fixed-clobber 契约，profiling OFF 逐位不变）。我的复验：mac 139961/123、orb Linux 140792/123（同条件 master 140789/122，+3/+1 即新用例）、指纹 A/B 零 diff、func_tests 双平台六格 rc=101 checksum 一致、c-ray 退出竞态冒烟 20/20 干净退出（codex 高负载 50/50）。合并后 master 双平台重建复跑同数全绿。
- **W65 已合并（`4c84e46`，v1 被我实测否决后重做，orchestrator 逐项独立复验）**：用户点名利用 Linux 空闲的 x18。v1 全局永久保留方案被我的 orb coremark 交错 A/B 否掉（median 7.721→8.008s，ratio 0.9642，5/5 同向不重叠——x18 是高频 RA temp，全局退出动态池代价真实）。v2 改为按 unit 条件化：首 pass 证明 spill 才 ReserveGPRForUnit(18) 复位重跑，发射期每指令首个标量 spill reload/write-back 固定用 x18；零 spill unit 与 master 逐位一致；context.S 与 C++ 双份 host callee 保存例程各加一对 STP/LDP（每次 entry 一次，防宿主未来占用 x18）；JitContext 构造断言把契约变成不变量；Darwin/Android 逐位不变。我的复验：4400 unit host_bytes 精确 +0、指纹 A/B 零 diff、**我的 orb coremark A/B 5 对 ratio 1.0000**（7.847 vs 7.847）、mac 139958/122、orb 142568/122、spill 压测 pin 0–3 全过、func_tests 双平台六格。合并后 master：mac 139961/123、orb 142571/123（=W65 142568+W64 +3，算术吻合）、func_tests 六格；golden 对拍仍只剩 pre-existing real_busy 14 行漂移（与本次无关）。
- **W67 已合并（`d9715ce`，orchestrator 逐项独立复验）**：W66 路线图 P0 只读计数器 `SVM_RA_SHAPE_PROF`（默认 OFF；翻译期计数、FinalizeCode 后 relaxed atomics 合入、不进 JIT 热代码；env 注册表 53→54）。七语料 59,528 unit 实测裁定：**P1 不立项**（spill 仅 25 unit/0.042%，水位 10/64）；**P3 静态推断确认**（XMM ON 动态 FPR 池恒 12 非 16、live 峰值仅 3，释放 v11-v14 精确 +4，但语料未显示 FPR spill 是主要矛盾）；**P2 收益面有限但真实**（严格 leaf 229/884 site=25.9%，静态上限 3,648 条 snapshot 指令=12.07%，注意是翻译 site 数非运行频度）；**P5 不立项**（PF/AF 强制 materialize 下界 98.38%/97.90%，远超 <0.5 门槛）。数据全文 `docs/w67-ra-shape-prof.md`。我的复验：OFF 指纹 A/B 零 diff、mac 139961/123、orb 142571/123、func_tests 六格、探针 ON 的 coremark CRC 全对（0xe9f5/0xe714/0x1fd7/0x8e3a/0x25b5）且输出合理；合并后 master 双平台同数全绿。
- **Linux `all` 构建修复（`ff925b7`，orchestrator 独立复验）**：两处 master 既有断裂——①trampolines.cpp 的 BuildFunctionTrampoline 残留死调用 `x86::GetABIDescriptor64()`（2024 草稿，返回值从未使用），Linux 静态链接下 arm64_decode_smoke 失败（补链 fronted_x86 会成静态库环），删除死调用+头依赖；②jit_env.cpp 枚举与 Linux 系统头 `SYS_*` 宏同名冲突，机械改名 `kGuestSys_*`（数值不变）。我的复验：orb 干净目录默认 all 全量构建通过（此前只能按 target 构建）、smoke 双平台运行正常、orb 140792/123、mac 139961/123、指纹 A/B 零 diff。**新揭示（已立 #129）**：~~swift_guest_call_test 首次在 Linux 编出，stub_call_test.cpp:352 guest-fault 捕获用例 SIGSEGV（24/25）——此前无可运行基线，mac 基线待查。~~ **#129 已根修合并（`42af3d5`）**：非生产 bug——Catch2 每用例接管 SIGSEGV、结束恢复先前动作，共享 Runtime 跨用例存活导致 SwiftVM 处理器被替换；fixture 的 Vm() 激活边界重装（Install 文档本就声明幂等可重assert）。生产代码零改动。我的复验：orb 644/43 × 3 随机 seed、mac 644/43、双平台全量 142571/123 与 139961/123、指纹零 diff。
- **W63 已根修合并（`2a8f3d5`，orchestrator 逐项独立复验）**：复盘期 orb Linux 全量套件发现 "two structured V128 accesses fault on the second page after the first commits" 确定性失败/挂起。orchestrator 二分坐实唯一相关变量 `SVM_X86_PIN_EXT>=2`（pin0/1 过、pin2/3 挂，其余翻盘开关全无关；`c905307` 干净 clone 复现；翻盘波次的 orb 绿灯系 stale 构建所得）。codex 根因（我源码逐行坐实）：旧 host fault 恢复把 PageFatal 直写 ucontext x0 并直达 label_return_host——pin2/3 下 x0 是被驻留的 guest RSI，静态驻留回写把 PageFatal 落进 RSI 槽（guest 状态腐蚀），且该配置返回值由 `Mov w0, halt_reg(x11)` 发布而直达入口从未装载 x11（返回值垃圾）；pin0/1 免疫仅因 halt_reg=x0。修法：恢复改经 State 发布（halt_reason=PageFatal + 新增 label_fault_return_host 绑在既有 halt-reason load 之前，trampoline 零新增指令），删除 SetContextReturnValue；回归测试补 RSI 保持断言。我的复验：Linux pin0–3 四档定向 PASS + 全量 140789/122 + func_tests 六格 rc=101 输出一致 + 指纹 A/B vs c905307 零 diff；macOS 全量 139958/122。**遗留：Linux 侧指纹门对 golden 有 pre-existing 的 real_busy 漂移（c905307 基线同样失败，与 W63 无关）——golden 平台可移植性待查。**


**W12 执行侧分解（2026-07-30，探针已合 `c3d96a5`：SVM_EXEC_PROF/SVM_EXEC_MAP）**：(1) dispatch-slot 块链接 + RSB **早已默认启用**——dispatcher 仅占块退出 0.49%/1.13%/0.0003%，现有链接已贡献 1.453×，direct-B 再压上限 ≈0，此路线死亡；(2) GPR pin A/B 中性（SVM_STATIC_REGS=0/1 差 0.3% 噪声内，orchestrator 复测一致），全 16 pin 需整体 ABI 重做且 ceiling 仅 ~1.19×，不是 4–7× 的解释；(3) 真因是**指令级膨胀**——coremark 热块 14 guest→120 host（GPR state 16.7%、flags 簿记 12.5%、冗余 move）；STREAM Copy(80B/iter，95 host) vs Scale(16B/iter，60 host) 指令密度差 3.16× 与吞吐差 2.57× 吻合；单条 guest `mulpd` → 15 条 host（13 条 NaN payload 修正 + 2 条 XMM state 往返），XMM0–15 全驻留内存；`cmp+jne` flags 12 条；每次访存地址重算 5 条。路线排序：SIMD/XMM 驻留+NaN 快路径（W13，已落地 `599e6fb`）→ flags/跨指令值保持（W14）→ GPR ABI（须 A/B 证明）→ 块 direct patch（已否）。

**W13 执行侧第一波（2026-07-30，`599e6fb`，双开关默认 OFF）**：XMM0–15 静态驻留 v16–v31（`SVM_XMM_STATIC=1`；XmmLo/Hi 经 UMOV/INS 桥、分配器固定 FPR 快照语义、XSAVE/XRSTOR 边界显式同步、AVX 低半驻留）+ SSE/共享 AVX FP NaN 快路径（`SVM_SSE_NAN_FAST=1`，激进类；每 f32 算术 −9 条、f64 −12 条修正；偏离限于 NaN payload 优先级/符号位与 invalid indefinite 符号，vec_float_nan_pressure 按设计 rc=2）。orchestrator 独立 A/B（REPS=5/3 warm，oracle 全过）：单独 XMM 7 项 geomean **0.969**（Copy +10.6%/smallpt +3.8%，但 c-ray −26% 吞吐——NaN 修正密集码挤占动态 FPR 池，Scale/Add −3%）→ **默认 OFF**；组合旗标对 geomean **1.205**（Scale +29%/Add +25%/Triad +50%/smallpt 1.44×/Copy +8.6%，c-ray 恢复噪音级）——`SVM_XMM_STATIC=1 SVM_SSE_NAN_FAST=1` 为对 FEX 的推荐性能旗标对（与 FEX 需 env 旗标开优化对等）。EXEC_PROF 证实 xmm_uniform 流量 43.4 亿→0。合入时附带修掉指纹门 ASLR 假阳性（`c316570`：CallLambda 宿主机地址 imm 固定 4 指令料化——vixl 零块省略会让页对齐 helper（RepMovs）的料化长度按进程 1/4 概率摆动）。遗留：XMM 单独开启的 c-ray/Scale 回退根因（W16 已坐实：16 个静态 V 寄存挤占动态 FPR 池，NaN 修正暂存无处放 → spill/reload；配套 NAN_FAST 免暂存后恢复。候选后续：XMM lazy-fill / pressure-aware 驻留）。

**W14 执行侧第二波（2026-07-30，`b4f5c52` + golden `1e88fb4`）**：UniformElimination 局部路径交汇（`SVM_UNIFORM_PATH_FWD`，**默认 ON**）——Goto/NotGoto 前建立的 uniform 事实支配两条后继，BindLabel 处与 fallthrough 逐字节求交保留，反向 DSE 对称删除被双路径覆盖的 guarded 写；FlagsElimination 全位缩窄（`SVM_FLAG_FULL_ELIM`，**默认 OFF**——实测 76B 收益噪声级，保留为实验开关）。coremark 热块 0x402df8：GPR state 20→14、flags 簿记 19→13、host 120→111；动态 gpr_uniform −10.7%。性能（orchestrator 独立交错 A/B 9 轮，与 codex 两组数据互证）：coremark **+8.6~8.9%**，stream 四项交替 ±0.34% 内无回退。golden 重生成：201 unit IR 下降（合计 −1086）、unit 集不变、零增长；`SVM_UNIFORM_PATH_FWD=0` 与旧 golden 零 diff（回退精确性坐实）。W16 调研附带定论：地址重算瘦身对 smallpt/c-ray/7zip <1.03 不立项（访存已在 [x24, wN, uxtw] 折叠）；7zip 热点的 memcpy/rep movs 假说否掉（sample 零命中）；三项共同最大可压类别是 move/merge（18–39%，部分是 SSE 标量 lane 语义必需）。

**W15 x86 crypto NI（2026-07-30，`f6bd6a5`）**：AES-NI 五指令（AESENC/ENCLAST/DEC/DECLAST/KEYGENASSIST）+ PCLMULQDQ 落地，FEX 映射（AESE(0)→AESMC→XOR key；keygenassist TBL swizzle + RCON 在 byte 4/12；PMULL 需 0x00c00000 size 位）；SHA256RNDS2/MSG1/MSG2 按 FEX Zip/Rev64 state 映射实现。开关：`SVM_X86_CRYPTO_NI` **默认 ON**（host FEAT_AES+FEAT_PMULL 门控，env=0 精确回退 ILL_CODE）；`SVM_X86_CRYPTO_SHA=1` **opt-in 默认 OFF**（需 bundle ON + host FEAT_SHA256——默认 OFF 不是实现风险，是 master 潜伏信号损坏（见 P0 #3）在 SHA 路径暴露面更大，待根治后翻默认）。CPUID：leaf1 ECX bit1+bit25，leaf7 EBX bit29（仅 SHA 开启时报告）；OPENSSL_ia32cap 透传。解释器软件实现（GF S-box / 位循环 PCLMUL / NEON SHA256 intrinsics）与 JIT 语义一致。KAT（NIST SP 800-38A/38D、独立 bit-serial GHASH oracle、FIPS 动态 keygen、100MiB SHA-256 digest 对 host）默认 ON rc=0 / crypto=0 rc=1。性能（orchestrator 独立交错 A/B）：aes-128-gcm 16k 83.6k → ~700k kB/s（**~9×**，FEX 1774k 的 0.39，GHASH 路径差距待分解）；coremark 中性（1.00）。门禁：指纹默认/crypto=0 双态 vs golden 零 diff（未重生成）、func_tests 三模式、swift_test 126320 assertions、trio/smc/clone_smc_mt 全过。

**W19 ZExt 单节点折叠（2026-07-30，`fe9ee1d`，默认 ON）**：32 位 GPR 写的 lowering 由 `ZeroExtend64(ZeroExtend32(v))` 两节点折为新 opcode `ZeroExtend32To64`（U64 结果、显式 32 位截断语义；JIT 复用 ZExt32 的 W 写路径——arm64 写 W 即清零高半；解释器显式 `& UINT32_MAX`）。每个 32 位 GPR 写省一条 host mov。`SVM_GPR_ZEXT_COALESCE=0` 精确恢复两节点旧 lowering。全量指纹审计（orchestrator 独立 OFF/ON 发射）：4400 unit 集不变、**1832 unit IR 下降、零增长**、合计 −4328 IR——纯节点数削减，golden 重生成。性能（orchestrator 独立交错 A/B）：coremark **+10.9%**（4/4 对为正）、7zip Tot MIPS **+2.9%**（3/3 对为正）；codex 独立数据 zip7 +8.5%/coremark +2.8%/sqlite −4.2% wall，方向互证。

**W22 XMM uniform 转发开关化（2026-07-30，`e3d1280`，默认 ON ≡ 现状）**：审计定论——UniformElimination（W14 家族）**本已覆盖 V128 store→load BitCast 转发**；本变更把 XMM guest-state range 显式建模（`config.xmm_uniform_ranges` → `UniformInfo`，range 进 JIT cache config hash），`SVM_XMM_UNIFORM_FWD=0` 提供精确 OFF 审计路径（禁止该范围 SSA forwarding + DSE，GPR 路径不变）。不改变默认性能；价值 = 模块级回退开关 + 行为钉死单测 + 审计量化既有转发对 FEX 差距的实际贡献（OFF→ON：aes-128-gcm 2.04×、smallpt 1.33×、STREAM Copy +17.7%、coremark +2.2%；GHASH 热 unit uniform 访问 123→38）。附带查证：c-ray 输出 PNG 的 MD5 不稳定是 **tEXt 内嵌 RenderTime 墙钟**，像素逐字节确定——c-ray oracle 必须用像素 hash 而非文件 md5。

**W20f rt_sigreturn 私有帧竞态根治（2026-07-30，`32e7341`，P0 #3 关闭）**：根因——`GuestSignalPrivate` 帧写在 guest 栈、sigreturn 靠 magic+`ucontext_addr==rsp` 在 ±64B 窗口扫描找回；密集 SIGALRM 下相邻投递 rsp 有 ≤64B 抖动，已消费帧的 magic 残留栈中，扫描**先命中上一周期残帧**（宿主机镜像对照实测 `found_at` 比 `delivered_at` 低 0x20）；残帧校验槽被栈复写数据（saved rbp 恰好指向活跃 uc 区）骗过，`saved_context` 已被 guest 栈流量污染（fs_base=0），而 ucontext 完好 → gregs 正常 + fs_base=0 → 恢复执行后第一个 `fs:0x28` PageFatal。修法按真实 Linux 语义：内核从不保存/恢复 fs_base/gs_base，FPU 经 fpstate/xstate ABI 恢复——**删除私有帧+扫描**（不是修补）：sigreturn 从当前 ctx 出发只套 uc + 新增 `ApplySignalXState`（校验 MAGIC1/2）；顺带修掉旧路径用陈旧值覆盖 `interrupt` 的隐患。`SVM_SIGNAL_PRIVATE_FRAME=1` 保留旧路径（注明已知竞态，仅供二分）。回归 `sigreturn_stale_frame`（handler 持续校验 TLS + 交替 ±32B 栈深制造残帧窗口；legacy 路径如期复现 PageFatal 反证测试命中）。验收：openssl sha256 ×30 零 PageFatal（基线 ~10-15%）、SHA-NI ×15 零崩、指纹/func_tests/swift_test 全绿。
**SVM_X86_CRYPTO_SHA 默认转正（2026-07-30，`0a40a6b`）**：W15 拆 opt-in 的唯一理由就是 P0 #3（SHA+SIGALRM ~50% 崩），根治后翻转。openssl sha256 16k：60.6 → 456.1 MB/s（7.5×）；`=0` 精确回退软件路径。指纹零 diff（语料无 SHA）、KAT 双模式 rc=0（此前默认路径因 CPUID 不广告而空跑）。

**W23 标量 SSE tied-destination + FEAT_AFP scalar-insert（2026-07-30，`ac124f0`，默认 ON）**：x86 标量 SSE 语义是「只写 lane0、保留高 lane」，此前需要显式 merge；host FEAT_AFP 的 FPCR.NEP 让标量 AdvSIMD 天然只更新 lane0。三部分：①register_alloc `TryTieScalarInsert`——源 interval 恰在当前指令 start 结束时把物理寄存器转移给结果（从 active 移除但不释放）；②8 个标量 emitter 走 tied 快路径，NaN 优先级（left NaN→left quieted，否则 right，否则 indefinite）、minmax unordered/equal 选 src2、sqrt 负数→indefinite 均用 Fcmp+Bif/Bit 保持 x86 精确语义；③trampolines 在 runtime 入口保存 FPCR 并置 NEP，interp/host-call 边界恢复。config hash 纳入 AFP → JIT cache key 自动区分。独立交错 A/B（orchestrator 复测）：smallpt **1.383× 中位**（7 对，image.ppm oracle 每对一致）、c-ray 1.009×、ON 稳定性 0/10 异常——codex 曾以单个 c-ray 超时为由发成默认 OFF，复测证实该超时是噪声（OFF 侧同样偶发）。`SVM_SSE_SCALAR_INSERT=0` 精确回退。指纹对 golden 零 diff（IR 不变）、func_tests 三模式、swift_test 110 例全绿、smallpt 默认 ON 与 =0 输出逐字节相同。

**W27 标量 SSE 操作数 V 类化（2026-07-30，`928b365`，默认 ON）**：W18 #3 落地——标量浮点 op 的 RHS 不再经 GPR 往返：寄存器源直接 XmmRead（只读 lane0），内存源 LoadMemory 打 V32/V64 类型直发 Ldr S/D 进 FPR；`GetVecScalarOperand` 对非浮点类型保留 Fmov 桥；W23 tied 路径同步支持 V 型 RHS；解释器 `ReadScalarBits` 按类型分派。orchestrator 6 对交错 A/B：smallpt **1.228×**（oracle 每对一致）。`SVM_SSE_SCALAR_V_OPERANDS=0` 精确回退。指纹零 diff（IR 节点数不变，仅类型变化）、func_tests 三模式、swift_test 111 例、NaN 压力全绿。

**W28 GHASH/AES 的 FEX-style lowering（2026-07-30，`042455b`，默认 ON）**：W26 立项三件套——①`SVM_VEC_IMM_SHIFT`：PSLL/PSRL/PSRA W/D/Q 立即数形式直发 NEON SHL/USHR/SSHR（count≥width→零、算术移位饱和），VEX.128/256 同覆盖；②`SVM_VEC_CONST_CACHE`：PSHUFD 索引表 VecLoadConst+TBL + 块内 SSA 去重（literal pool 因 JitContext 尺寸生命周期不可用，用 PIC 安全物化）；③`SVM_VEC_BYTESHIFT_EXT`：PSLLDQ/PSRLDQ 共享零向量+单条 EXT，顺带补齐 VEX vpslldq/vpsrldq（此前无 handler）；④`SVM_AES_ZERO_REUSE`：AES 轮共享零 key + RA tie 免拷贝。GHASH 热 unit 实测 **185→126（−31.9%）**（orchestrator 用 W26 脚本在 master 构建上复核一致）。**墙钟实话：aes-128-gcm 16k 六对交错 OFF/ON=1.0001**（已贴近访存/stall 边界；codex 受载机 +36% 说法不可复现），按指令消除+codegen 简化落地。指纹 4 unit 变化全部可归因（PSHUFD/PSRLDQ 新形状，审计后重生成 golden）、AES KAT×3、func_tests、swift_test 111 例全绿。

**W29 窄 load 融合 + GPR 常量 shift 专门化（2026-07-30，`858fba2`，双开关默认 ON）**：W25 #2/#4——`SVM_MEM_NARROW_FUSE`：窄 LoadMemory 单用+RA tie 时直发 Ldrsb/Ldrsh（zext 由 Ldrb/Ldrh W 写天然满足）、单用 GetOperand 透传原始地址寄存器（fault 点不变）、复合 EA 直算进目标寄存器消 transport Mov；`SVM_SHIFT_IMM_FAST`：常量 count SHL/SHR/SAR 直发立即数移位（count==0 保持 x86 不动 flags，1..width-1 免 mask/guard，CF 立即数直算）。**合并期 orchestrator 抓获并修复 codex 一处 tie 越权**：ZeroExtend32To64→64 位立即数 shift 的 tie 会把 AsrImm 高 32 位符号扩展泄漏进 RDX（fuzz imul/cbw/mixed 三例 mismatch），修复=tie 仅限 32 位 W 写 shift。A/B（orchestrator，安静机）：项B coremark **+3.79%**（6/6 正），项A +0.26%（噪声级但方向正，hot unit −26 条实测，按 codegen 简化落地）。指纹 4 unit decrease-only（−44 IR，审计后重生成）、func_tests 三模式两态、swift_test 113 例全绿。

**W30 跨块 flag liveness + edge materialization（2026-07-30，否掉未合入）**：W25 预估的 58 条热路径消除在 PageFatal 红线 + 保守回退下只剩 **332→328**；edge materialization 使 ON 态 IR +23.3%、host bytes +25.5%；coremark A/B ON 反而 −0.95%。**轴关闭**：机械上限 1.07× 在安全物化成本下不可达，负证据数字存档，658 行实现已随 worktree 丢弃。

**W31 XMM uniform load→load SSA 转发（2026-07-30，`489b0bf`，默认 ON）**：W22 的 UniformElim 只从 StoreUniform 建 byte facts，首次 LoadUniform 不入表；补上后同地址重复 load 复用首载值（LoadUniform→BitCast→DCE，alias/barrier/label 规则全复用）。GHASH `0x634960` 123→116、AES `0x760790` 102→92、`0x761480` 206→201（合计 −22 条，orchestrator 复核一致）。`SVM_XMM_SSA_FWD2=0` 精确回退（OFF 逐字节一致）。指纹两态零 diff、func_tests 三模式两态、swift_test 113 例、KAT×3 全绿；受载 A/B GCM +0.4%/CBC +1.6% 方向正，按指令消除落地。

**W32 GPR direct-link trace forwarding（2026-07-30，否掉未合入）**：PoC 证实 link-hit 边界成本确实为 0，但新增静态 GPR 触发 uniform byte cache 全表失效（+18 条）+ partial-write/桥接（+48 条），20 条目标消除净剩 2 条；top-5 总 host 288→334（+16%）、全语料 IR +2.96%、coremark 净 −3.58%。**轴关闭**。至此跨 unit 结构轴三连灭（W30 flags / W32 GPR trace / W17 XMM 驻留），共同死因 = 边界/失效成本吃掉机械上限；此后跨 unit 立项必须先交边界成本净账。

**合并事故记录（2026-07-30，`d3cd7dd` 修复）**：W29 合并提交漏 add `jit_context.h/cpp`（SharesGPR），工作区残留使本地构建通过但干净检出编译失败（W31/W32 先后撞上）。流程补丁：合并提交后必须 `git status` 干净 + `git diff --stat HEAD` 对账。

### 已实测否掉的优化路线（2026-07-28，数字见各提交/记忆，勿重复立项）

- **W17 pressure-aware XMM 驻留（2026-07-30，三轮全负，轴关闭）**：部分 pin（top-N/XMM0–7）STREAM −0.9~−9.0%——少 pin 守不住收益、全 pin 挤爆 FPR 池，无中间甜点；lazy-fill 单独不够（c-ray 热 unit 很快摸满 16 XMM）；per-unit 可驱逐 pin（=3）在 c-ray 上触发 **55 分钟 400% CPU 死循环**（编译/重翻译环路，门禁语料覆盖不到），最终只能退成安全 no-op。结论：此轴关闭，W13 旗标对（XMM_STATIC=1 + NAN_FAST=1 捆绑）即最终形态。

- **函数单元做大 / 跨块优化**：`c0b5861` 已扫 1→64 块/单元，墙钟全在 MAD 内、发射字节单调涨；`SVM_FUNC_LAZY=1`（默认）下每单元一块，跨块没有主体。
- **x86 DirectBlockLink**：Mov+Br 无 backpatch 但跨 unit 仍非 SMC-safe（无 target→source incoming-link 表，ReclaimCode 释放后源块 Br 进已释放内存）；且开启即禁用整个 JIT disk cache（func_tests 热 cache 收益 ~11.4 ms vs 直接链接上界 ~0.01 ms）。
- **分派序列折叠（Mov+Ldr→单 Ldr）**：差分实测四条指令合计 0.305–0.314 ns/次，kernel_call 6400 万次仅 ~20 ms（10%）；折叠上界 ~1.2%。
- **ComputeRPO/IdByRPO 单独立项裁剪**：W1 分解实测合计仅占翻译 1.14%，全消也不值得；getenv 残余仅 0.068%，同样排除（2026-07-29）。
- **RecordUnit 批量化（W5，2026-07-29）**：前提证伪——不存在逐 unit 写盘（落盘本来就是退出时 2 次 fwrite、0 seek、0 fsync）。deferred-scan 候选（reloc scan 挪到退出 `Save()`）publish_disk −24.7% 但真实净省仅 0–101µs（0–0.6% translate），墙钟 func_tests 反而 +2.9%，且 pending_units 快照持有到 Save 改变长进程内存行为。最大头（publish_disk 的 53.1% = 1133 次 `/dev/null` guest 可读性探测 syscall）无法安全消除：`MemoryInterface::Read` 是裸 memcpy 返回无条件 true，`GetPointer` 只证明查询时刻可读（munmap 竞态）；真正的修法是 mmap/munmap 同步的 guarded-copy API，未立项。另：cache 跨构建加载天然不可能（build_id 每次 relink 都变，relocation 存 `host_image.base + addend`），build gate 是必要安全边界。
- **GetPointer 取指校验按页摊销（W7，2026-07-30）**：2 次 GetPointer 合计仅 5.47–5.82 ns/指令、占 decode 1.9–2.0%；按 16 KiB 页摊销理论上减少 ~90% 调用，但折合翻译总耗时上限仅 ~0.2%，还要维护页跨越/映射变化/SMC 安全证明，不单独立项。
- **优化 `DecodeVexInsn` 裸解析器（W7，2026-07-30）**：仅 3.35 ns/次；AVX 路径的成本在 handler/lowering（143.5 ns/条），不在字节解析，无主体。
- **distorm 整体替换（W7，2026-07-30 降级）**：实测 distorm 仅占 decode 14.7%，替换上限 ≈ 翻译总耗时 1.7%，工程风险与正确性面高于 top-N 通道，降为二级项目，不排在 lowering 之前。
- **lowering regvalue/flags 决策开销压缩（W9，2026-07-30）**：四个候选（R/V offset 预计算、NarrowTo 本地尺寸表、flags 条件表简化、GetValueSizeByte 移位）单项收益全部低于噪声，总账两语料方向相反（func_tests translate −0.83% / real_busy +1.24%，15–21 轮交错中位数），全部撤回、工作树零改动。结论：lowering 簿记开销在当前粒度压无可压，decode 线簿记层面封顶；剩余仅 top-N distorm 快速通道（~0.8%）与分发压扁（~0.2%）两个小项。

---

## 3. 环境变量开关

| 变量 | 取值 | 默认 | 说明 |
|---|---|---|---|
| SVM_TSO_MODE | relaxed\|acqrel\|hardware | relaxed | **字符串**，0/1 会静默落 relaxed |
| SVM_X87_JIT | 0/1 | 0 | x87 JIT opt-in；默认 SoftFloat 位精确 |
| SVM_X87_JIT_STATS | 0/1 | 0 | helper call 计数 |
| SVM_X87_TOPVIRT | 0/1 | 0 | TOP 虚拟化（需同时 SVM_X87_JIT=1) |
| SVM_SMC_MT | 0/1 | 1 | MT SMC 回收 kill switch |
| SVM_FUNC_BASE | 0/1 | 1 | 函数级编译 |
| SVM_ENABLE_JIT | 0/1 | 1 | 0=纯解释器 |
| SVM_STATIC_REGS | 0/1 | 1 | RSP→x19、RBX→x20、RBP→x21 |
| SVM_UNIFORM_ELIM | 0/1 | 1 | uniform 消除 pass |
| SVM_FLAG_CARRY_ELIM | 0/1 | 1 | Carry Gate B（块内全路径覆盖 C 写删除）；=0 精确恢复旧 Gate B 行为 |
| SVM_RA_1BLK | 0/1 | 1 | 单块专用 RegAlloc/LiveIntervals 快路径；=0 走通用路径（两路 byte-identical） |
| SVM_UNIFORM_FAST | 0/1 | 1 | UniformElim 早退 + DSE 剪枝；=0 走完整旧扫描（两路输出恒等） |
| SVM_IR_FAST | 0/1 | 1 | IR 构建路径消重；=0 走旧构造/校验/use 扫描（两路输出恒等） |
| SVM_PROF2 | 0/1 | 0 | 翻译阶段七项分解探针（decode/IR/pass/固定开销/vixl/发布/其他 + 逐 pass + getenv 计数）；发射中性，默认关闭 |
| SVM_DECODE_PROF | 0/1 | 0 | decode 前端细分探针（fetch/predispatch/raw/distorm/lowering/bookkeeping + opcode 直方图）；需同时 SVM_PROF2=1；发射中性，默认关闭 |
| SVM_LOW_PROF | 0/1 | 0 | lowering 桶细分探针（dispatch/address/memory/flags/regvalue part 归属 + opcode/group/kind 三套桶）；需同时 SVM_PROF2=1 SVM_DECODE_PROF=1；发射中性，默认关闭 |
| SVM_DISTORM_FAST | 0/1 | 1 | top-N distorm 快速通道；=0 全部回退 distorm（两路 IR 恒等） |
| SVM_DISTORM_VERIFY | 0/1 | 0 | 双解码逐字段对账（不一致强制采用 distorm 结果并计数），审计用 |
| SVM_VIXL_FAST | 0/1 | 1 | vixl EmissionCheckScope 快路径；=0 走原通用协议（两路 host 字节逐字节一致） |
| SVM_VIXL_PROF | 0/1 | 0 | vixl 发射细分探针（Emit 外层/编码本体/码缓冲/池修正）；发射中性，默认关闭 |
| SVM_SYSCALL_RT_SIGACTION | 0/1 | 1 | guest 信号 disposition 登记/查询；=0 恢复 ENOSYS |
| SVM_SYSCALL_RT_SIGPROCMASK | 0/1 | 1 | guest 每线程信号 mask；=0 恢复 ENOSYS |
| SVM_SYSCALL_MMAP_SHARED_READ | 0/1 | 1 | file-backed MAP_SHARED\|PROT_READ 只读快照；=0 恢复拒绝 |
| SVM_SIGNAL_DELIVERY | 0/1 | 1 | guest 信号投递框架（alarm 到期块边界注入 + 完整 signal frame + rt_sigreturn）；=0 恢复 ENOSYS |
| SVM_SIGNAL_TRACE | 0/1 | 0 | 信号注入/返回地址追踪 |
| SVM_XMM_STATIC | 0/1 | 0 | XMM0–15 静态驻留 v16–v31；=1 开启。单独开启当前净回退（W13 实测 geomean 0.969），须配合 SVM_SSE_NAN_FAST=1 使用（组合 1.205） |
| SVM_SSE_NAN_FAST | 0/1 | 0 | **激进类**：跳过 SSE/共享 AVX FP 的 x86 NaN payload/quiet 修正；偏离 NaN 位级语义（边界见 W13 段），五语料 oracle 无感 |
| SVM_UNIFORM_PATH_FWD | 0/1 | 1 | UniformElim 局部路径交汇（W14）；=0 精确恢复逐 label 清空旧行为（与 W14 前 golden 零 diff） |
| SVM_FLAG_FULL_ELIM | 0/1 | 0 | FlagsElimination 全 flag 位缩窄（W14 实验开关）；实测收益噪声级故默认 OFF，=1 开启 |
| SVM_X86_CRYPTO_NI | 0/1 | 1 | x86 AES-NI+PCLMULQDQ 硬加速（W15）；host 无 FEAT_AES/PMULL 时自动回退 ILL_CODE，=0 强制软件路径 |
| SVM_X86_CRYPTO_SHA | 0/1 | 1 | x86 SHA256 NI（W15，需 CRYPTO_NI 同开 + host FEAT_SHA256）；2026-07-30 随 P0 #3 根治默认转正（sha256 7.5×），=0 回退软件路径 |
| SVM_SSE_SCALAR_INSERT | 0/1 | 1 | 标量 SSE tied-destination + FEAT_AFP FPCR.NEP（W23，smallpt 1.383×）；host 无 FEAT_AFP 时自动关闭，=0 精确回退 merge 旧路径 |
| SVM_SSE_SCALAR_V_OPERANDS | 0/1 | 1 | 标量 SSE RHS 保持 V 类免 GPR→V 回填（W27，smallpt 再 1.228×）；=0 精确回退 GPR 桥 |
| SVM_VEC_IMM_SHIFT | 0/1 | 1 | PSLL/PSRL/PSRA 立即数直发 NEON 立即数移位（W28）；=0 回退可变计数路径 |
| SVM_VEC_CONST_CACHE | 0/1 | 1 | PSHUFD 索引表 VecLoadConst+块内去重（W28）；=0 回退逐次物化 |
| SVM_VEC_BYTESHIFT_EXT | 0/1 | 1 | PSLLDQ/PSRLDQ 共享零向量+EXT（W28，含 VEX 补覆盖）；=0 回退 lane 模拟 |
| SVM_AES_ZERO_REUSE | 0/1 | 1 | AES 轮共享零 key + RA tie 免拷贝（W28）；=0 回退逐轮 zero+copy |
| SVM_MEM_NARROW_FUSE | 0/1 | 1 | 窄 load 扩展融合 + GetOperand 透传/EA 直算（W29，coremark +3.8%）；=0 回退 load+extend 与 transport mov |
| SVM_SHIFT_IMM_FAST | 0/1 | 1 | GPR 常量 SHL/SHR/SAR 立即数专门化（W29）；=0 回退可变计数 guard 路径 |
| SVM_XMM_SSA_FWD2 | 0/1 | 1 | XMM uniform load→load SSA 转发（W31）；=0 精确回退仅 store→load 旧机制 |
| SVM_SSE_NAN_COLDPATH | 0/1 | 1 | 精确 x86 NaN 的 rare slow path（W37，smallpt 1.274×）：快路径 host FP + 一条结果自比较分支，NaN 跳 unit 内 cold stub 重建 payload/SNaN/indefinite；=0 逐字节恢复旧内联修正（SVM_SSE_NAN_FAST 仍优先） |
| SVM_FLAGS_NARROW_ALIGN | 0/1 | 1 | 8/16 位 ALU 符号位对齐 W[31] 单 adds/subs + lsr 截断，PF 复用结果（W38）；=0 回退加宽双操作旧 lowering |
| SVM_FLAGS_CFINV | 0/1 | 1 | FEAT_FlagM 上同 unit 已知极性 ADC/SBB 前 cfinv 归一化（W38）；无 FlagM 或极性未知自动走旧路径，=0 强制关闭 |
| SVM_FLAGS_TERMINAL_JCC | 0/1 | 1 | 紧邻 CMP/TEST + 唯一消费 branch/select 折叠直接 B.cond/CSEL（W38）；=0 恢复 cset+cbz 物化 |
| SVM_FLAGS_FCMP_FUSE | 0/1 | 1 | UCOMIS/COMIS PublishFCmpFlags + FCMP NZCV 直供紧邻 branch/select（W38）；EQ/NE 回退，=0 恢复关系打包旧路径 |
| SVM_ADDRMODE_STRUCT | 0/1 | 1 | 结构化 AddressMode（W39，STREAM 几何 +3.6%）：普通 V128 load/store 结构化 Operand 带地址 + 闭集六 opcode SSA 复用；=0 回退 FlatAddress 拍平 Add 链 |
| SVM_X86_GCM_PCLMUL2 | 0/1 | 1 | PCLMULQDQ imm=0x11 直发 PMULL2（W21，省 2×DUP，与 FEX 同形）；=0 回退 DUP+PMULL。实测性能中性（两组交错 A/B 同机噪声内），按 codegen 简化落地 |
| SVM_GPR_ZEXT_COALESCE | 0/1 | 1 | 32 位 GPR 写 ZExt32→ZExt64 两节点折为 ZeroExtend32To64 单节点（W19）；=0 精确恢复两节点旧 lowering（与 W19 前 golden 零 diff） |
| SVM_XMM_UNIFORM_FWD | 0/1 | 1 | XMM range 的 uniform store→load SSA 转发 + 死写删除（W22 开关化；行为与 W14 家族既有转发一致）。=0 精确禁止该范围转发/DSE，GPR 路径不变 |
| SVM_ARM64_LRCPC | 0/1 | 1 | TSO LRCPC 快路径 |
| SVM_EXEC_PROF | 0/1 | 0 | 执行侧探针：块退出分布、slot-link/RSB 命中、dispatcher L1/L2/miss、GPR uniform 访问计数；发射中性（W12 实测），默认关闭 |
| SVM_EXEC_MAP | 0/1 | 0 | JIT unit/trampoline 地址区间输出（配合 sample 分类 leaf PC） |
| SVM_BLOCK_LINK / SVM_DIRECT_BLOCK_LINK / SVM_EXEC_ACCESS_PAD | — | 诊断专用 | **勿用于生产**：链接关闭/direct-B baked/访存成本注入，仅测量用 |
| SVM_FORCE_FIXED_STACK | 0/1 | — | 诊断：强制 guest 栈 fixed/fallback(布局 flake repro) |
| SVM_GUEST_BITS | 0 或 20..47 | 32 | guest 地址窗口位宽。**0 = 无界（未隔离）**，需 `-DSWIFT_ALLOW_UNBOUNDED_GUEST=ON` 才编译进去，普通构建拒绝 |
| SVM_AVX / SVM_XSAVE | 0/1 | 0 | AVX/AVX2/FMA3 与 XSAVE/XSAVEC/XSAVEOPT 门控(联动广告) |
| SVM_SSE4 / SVM_SSE42STR / SVM_BMI | 0/1 | 1 | SSE3~SSE4.1 / SSE4.2 字符串 / BMI1/BMI2 |
| SVM_FSGSBASE | 0/1 | 0 | RD/WRFS/GSBASE(CPUID.7 EBX.0) |
| SVM_ADX | 0/1 | 0 | ADCX/ADOX 双进位链(CPUID.7 EBX.19) |
| SWIFT_FUZZ_SEED | u64 | 随机 | fuzz 定值复现 |

---

## 4. 测试与验证体系

- **本机（macOS arm64）跑通完整套件的前置条件**:系统缺 cmake 时可借用任一 3.21+ 的 cmake;vendored fmt 9.1.1 与 Apple clang 21 的 `consteval` 冲突，需 `-DCMAKE_CXX_FLAGS=-DFMT_CONSTEVAL=`;ANTLR 需 JDK,且 `find_program(JAVA ...)` 结果会被 CMake 缓存,换 JDK 后必须显式 `-DJAVA=<path>` 覆盖否则仍用旧值（而 "Antlr4 gen success" 是无条件打印的，不能作为成功依据）。
- **全套件基线**(2026-07-26,含上述全部修复):**逐用例独立进程**下非 fuzz 27 PASS / 0 FAIL、Unicorn 差分 fuzz 33 PASS / 0 FAIL。
- ⚠ **但「逐用例跑」会掩盖跨用例缺陷**。`Fuzz x86 decode robustness`(`x86_fuzz.cpp:2167`) 在**单进程全量**跑时 SIGABRT(`malloc: pointer being freed was not allocated`,堆破坏),单跑该用例则通过——需要前面若干用例先跑过才触发。已在 origin/master 的干净 worktree 上复现，**属既有缺陷而非近期引入**。
  §4 的 per-case 方法论是为绕开 Unicorn 偶发 SIGILL 而采用的，但它同时**交换掉了跨用例覆盖**。两种跑法都要做：per-case 用于定位与抗 flake,单进程全量用于暴露跨用例状态污染。仅凭 per-case 全绿宣称「套件通过」是不完整的。
- **构建陷阱**:逐提交验证（`git checkout <sha>` 循环）会让文件时间戳回退，make 据此误判无需重编，后续测试跑的是**陈旧二进制**。症状是「用例数变少但全绿」——覆盖面悄悄塌了却看着像胜利。切回分支后应 `touch` 相关源文件强制重建再测。
- **差分 fuzz**(`swift_test "Fuzz x86*"`,Unicorn oracle):32 族 per-case 运行。**已知 flake:Unicorn 自身偶发 SIGILL(rc=132,x86_fuzz.cpp:6023/6030)~10-30%/run，会 abort 整个 Catch2**——方法论：per-case 循环 + 每例 3 次重试；管道中 `$?` 是 grep 的码不是测试的码。
- **Rosetta 仲裁**(`arch -x86_64`):ISA 语义 ground truth（真实 x86 边界行为，如 FCOM IE、FIST 边界、非对齐 LOCK 信号形态）。
- **raw 测试二进制**(`source/translator/linux/tests/`,.S + 生成器 + 已检入 ELF):smoke/coverage/MT/TSO/x87 各家族；`build_real_tests.sh` 在 orb ubuntu-x64 VM 或 `clang -target x86_64-linux-gnu -nostdlib -static` 重建。
- **隔离/健壮性三件套**(均为「宿主必须活着」而非「guest 算得对」):
  `run_isolation_tests.sh` 21 例（地址空间窗口 + 无界模式的编译期门）、
  `run_malformed_guest_tests.sh` 33 例（畸形指令流）、
  `run_helper_fault_tests.sh` 38 例（**宿主 helper 帧里的 guest 故障必须变成 guest #PF**，
  13 形状 × 3 种 lowering；当前 HEAD **38/38 通过**）。
- **func_tests**:真实 C 多块函数，三模式 checksum 必须逐位一致。
- **MT/TSO**:clone_* 五件套 + litmus(mp/sb00 合法方差区间历史已知）。
- ⚠ **e2e 退出码对「函数模式错编译」是盲的**。函数编译整个包在 `try/catch` 里
  (`translator/x86/translator.cpp` 的 `catch (const std::exception&)`):一个单元
  错编译到触发断言，异常并不会让运行失败——它退到**块编译**兜底,guest 照样算出
  正确的退出码。于是 25 个 guest 的退出码矩阵全绿，而它本该考核的那条路径已经
  静默停跑了。这不是假设:一次针对函数路径的定向变异(M1)第一次就是凭这份证据被
  判成「存活」,只有每单元发射轨迹才看出它其实已经把全部函数单元踢进了块兜底。
  **实测(2026-07-27)**:注入一个让部分单元抛异常的变异后，25 个退出码与基线
  **逐行相同**,而 `run_func_fingerprint_tests.sh` 立刻红——修复 hermeticity 前的
  worktree 直启路径下，函数单元 4447 → 1855,
  `real_hello` 的 `ir_insts` 27718 → 11782。
- **函数编译发射指纹门禁**(`run_func_fingerprint_tests.sh`,常驻):11 个**单线程**
  guest 在 `SVM_PROF=2` 下的每单元 `[svm-unit] pc/ir` 列表 + `func_units /
  block_units / decoded_blocks / ir_insts` 总计,对拍检入的
  `func_fingerprint_golden.txt`（固定 staging 路径下 **4419 单元**）。三个门:
  ① 同一构建跑两遍必须逐字节一致(**这一档才带 `host_bytes`**);
  ② 单元总数 < 1000 直接判死(防「golden 由已经坏掉的构建生成」的自我一致陷阱);
  ③ 与 golden 对拍。
  **`host_bytes` 只在同一构建内比**——发射的 host 指针立即数长度随翻译器自身布局
  变化，跨构建会漂几百字节而代码其实一致;跨构建只比 `ir_insts` 与每单元列表。
  glibc guest（real_* 均为**静态**链接）的初始栈含 `argv[0]` 与 AT_EXECFN，过去直接从 checkout 启动会让路径字符串
  长度改变可达块和函数单元切分。门禁现将**所有** guest（含静态 guest）**复制**到
  固定的 `/tmp/svm_fp_guests/` 并从该路径启动；必须复制而非软链——loader 会
  realpath guest 路径并把解析后路径作为 AT_EXECFN 压进 guest 初始栈,软链会把
  真实 checkout 路径漏进栈里(实测:__memcpy_ssse3 路径选择漂移,±4 单元)。
  golden 已在固定路径下重生成,checkout/worktree 位置不再参与指纹。
  用法:`run_func_fingerprint_tests.sh <svm>` / `--update` / `--against <svm_B>`。
  同一构建的重复 check 仍先做带 `host_bytes` 的双跑确定性检查。

---

## 4b. AVX / AVX2 / FMA3（**已可用**，`SVM_AVX=1 SVM_XSAVE=1` 后 CPUID 如实上报）

> 本节 2026-07-27 重写。此前写的是「不是一个可用特性，是一层地基……没有任何
> 一条 VEX 指令被真正执行过」——那句话在写下时是诚实的，现在已经不成立。

**当前定级：可用。** 11 个真实 AVX2 kernel 与 x86-64 硬件**逐位一致**，
`run_avx_real_tests.sh` **18 项全通过、0 缺口、0 失败**。CPUID 在门控打开时上报
AVX / AVX2 / FMA / XSAVE / OSXSAVE，`XGETBV` 报告 XCR0[2:1]=11b，glibc 的
ifunc 会真正选中 AVX 路径。

| 族 | 状态 | 验证规模 |
|---|---|---|
| VEX.128 / VEX.256 搬移·位运算·整数·浮点 | 完成 | Rosetta 差分 + 定向自差分 |
| `vcmpps`/`vcmppd` 全 32 关系 | 完成 | predicate 重定义为关系集，见下 |
| FMA3 全 60 助记符（两个 VEX.L、寄存器与内存形式） | 完成 | Rosetta **3360 行**，7 次变异全杀 |
| gather 族 | 完成 | Rosetta 差分 |
| legacy SSE3 / SSSE3 / SSE4.1 共 64 条 | 完成 | Rosetta **4360 行**，26 次变异全杀 |
| SSE4.2 字符串族 `pcmpXstrY` + 四条 VEX 孪生 | 完成 | Rosetta **6104 行** + 独立 SDM 模型双 oracle，**51 次变异全杀**；另有 148 万种输入上三个求值器逐位一致 |

**CPUID bit 20 已开**（`76ccded`）。求值器从逐格矩阵改成位掩码代数后降到
4.1–5.4 ns/次（`52e59aa`），`pcmpistri` 端到端 9.47 ns/迭代，是 SSE2 的
`pcmpeqb+pmovmskb` 对的 6.8 倍，改动前是 17–45 倍。

**此前反对置位的理由前提是错的，值得记下来。** 当时判断「开了 bit 20 会让 glibc
把 `strlen`/`strstr` 换到 pcmpistri 上」。用 `llvm-nm` 查实际符号：这份 glibc 的
SSE4.2 字符串变体只有六个（`strcmp`/`strncmp`/`strcasecmp`/`strcasecmp_l`/
`strspn`/`strcspn`），**`strlen` 和 `strstr` 一个都没有**。census 里那 302 次
`pcmpistri` 是 ifunc 变体内部的**静态字节**，只有这一位置上才会被选中——这也解释
了为什么此前没有 glibc guest 死在它上面。**「census 计数」是静态出现次数，不是
动态执行次数**，把两者当一回事会得出反向的结论。

`pcmpXstrY` 那一族里有一处**硬件抓到的**语义 bug：SDM 有效性覆盖表的中间两行，
实现与测试模型**以同样的方式写反了**（`ABCDE` vs `ABCDEFGHIJ`，真机在索引 0
报匹配，写反的表报无匹配）。**模型没抓到，真机抓到了**——两个来源同源出错时，
只有第三方能否定。这是「单一 oracle 全绿不构成证据」最干净的一个实例。

**为什么 legacy SSE 属于 AVX 的收口条件**：真实 CPU 不存在「有 AVX 无 SSE4.2」
的组合，所以 advertise AVX 隐含承诺了整条 SSE 链。补这一族之前，一次 78-opcode
的 legacy 探针里 **67 条致命**（FALLBACK → IllegalCode → guest 死）。

**`VecFCmpMask` 的 predicate 已从「x86 imm8 编码」重定义为关系集**：
bit0=a&lt;b, bit1=a==b, bit2=a&gt;b, bit3=unordered。16 个子集全部合法，恰好覆盖
AVX 的 16 种关系；signaling 与否是异常行为不是比较结果，留在前端。这样 IR
不再背 x86 的编码表，legacy SSE 的 8 个 predicate 正是这张表的前八行。

### 关键发现：distorm 对 VEX.L 的静默丢失（本轮最有价值的产出）

vendored distorm 快照是 **AVX1-only**（2018 年生成，`insts.c` 里 `OT_YXMM`/`OT_VYXMM` 出现 0 次）。实测：

| 类别 | VEX.L=1 行为 |
|---|---|
| AVX1 float/搬移(`VMOVDQU/A`、`VMOVUPS`、`VADDPS`、`VBROADCASTSS`) | 正确，`index=R_YMM0` `size=256` |
| **AVX2 packed 整数**(`VPXOR`/`VPAND`/`VPADD*`/`VPCMP*`/`VPSHUFB`/`VPMOVMSKB`) | **静默按 128 位报告，且与 L=0 形式在 API 层完全无法区分** |
| AVX2-only 助记符(`VPBROADCAST*`/`VINSERTI128`/`VPERM2I128`/`VPERMD`/gather) | `I_UNDEFINED`,不可解码 |

实测对照（两条指令的 mnemonic id 与全部操作数**完全相同**）：
```
vpxor xmm0,xmm1,xmm2  C5 F1 EF C2 -> id=7009 ops[0] index=91(R_XMM0) size=128
vpxor ymm0,ymm1,ymm2  C5 F5 EF C2 -> id=7009 ops[0] index=91(R_XMM0) size=128
```
**后果**：任何按 distorm 操作数宽度判断位宽的实现，都会把 256 位 vpxor 当成 128 位——只算低半、并按 C3 把高半清零，静默算错。
**对策**：`DecodeVex()` 直接从原始指令字节解析 VEX 前缀，`VexInfo::l` 是位宽的**唯一**权威来源，任何路径都不得查询 distorm 的 size/寄存器类型。

### 设计契约
- **C1**:一个 YMM = **两个独立的 V128 IR 值**,永不创建 `ValueType::V256`。因为 ARM64 的 V 寄存器是 128 位而 `RegAlloc` 是一值一寄存器。结果是**零新增 IR opcode、零后端改动**——全部复用现有 V128 emitter 各调用两次。
- **C2**:`union Ymm`(low/high 重叠、`sizeof` 仅 16 字节，存不下 256 位）已删除,替换为只存高半的 `std::array<Xmm,16> ymm_high`。实测 `sizeof(ThreadContext64)`=864 及 `xmms`=160/`ymm_high`=416/`interrupt`=672/`x87_regs`=736 **逐字节不变**,FXSAVE 与全部硬编码 uniform 偏移不受影响。
- **C3**:VEX.128 写完 dst 必须清零 bits 255:128(与 legacy SSE 保留高半相反）;VEX.256 写满两半，**不适用** C3。
- **C4（已从"一律不报"演进为"必须与实现一致"）**:CPUID 永远不得承诺解码器会
  #UD 的能力——这是原始纪律；但门控打开后它**反过来也成立**：门控开启了
  handler，CPUID 就必须跟上，否则等于朝安全方向撒谎，guest 永远用不到已经写好
  的代码。现状：AVX/AVX2/FMA 随 `SVM_AVX && SVM_XSAVE` 联动（AVX 需要
  XCR0[2:1]=11b 的整套协议，缺 XSAVE 就不连贯）；SSE3/SSSE3/SSE4.1/POPCNT 随
  `SVM_SSE4`（默认开）；BMI1/BMI2 随 `SVM_BMI`。SSE4.2 随 `SVM_SSE42STR`（默认开）。
  至此 **x86-64-v2 特性集完整**。
- **C5**:AVX/BMI/XSAVE 挂在各自的 `SVM_*` 后，默认关闭；`SVM_SSE4` 是**默认开的
  逃生开关**，因为它替换的是「必然的 guest 死亡」而不是既有行为。
  一条教训：`haddps`/`hsubps` 曾被移入该文件，于是关掉开关反而比改动前更差
  ——它们现在被放在门控**之前**处理。**关闭态比被关掉的代码更差的开关不是逃生
  开关。**

### 已实现
- VEX.128:`VMOVDQA/U`、`VMOVAPS/UPS/APD/UPD`、`VMOVNTDQ/NTDQA/NTPS/NTPD`、`VLDDQU`、`VMOVD/Q`、`VPXOR/POR/PAND/PANDN`、`VPADD{B,W,D,Q}`、`VPSUB{B,W,D,Q}`、`VPCMPEQ{B,W,D}`、`VPCMPGT{B,W,D}`、`VZEROUPPER/VZEROALL`
- VEX.256(`decoder_avx.cc`):上述搬移/位运算/加减/比较，外加 `VPMINU{B,D}`/`VPMAXU{B,D}`、`VPMOVMSKB`(两半掩码按 `lo | hi<<16` 合并，是唯一两半不独立的)、`VPSHUFB`(AVX2 按 128 位 lane 独立，故每半一次 `VecTableLookup8` 精确)、`VBROADCASTSS`

### VEX.128 审计结论（2026-07-26,静态核对 + 定向自差分）

- **C3 高半清零：完整**。VEX.128 段内每一处向量目的写入（`DecodeVexBitwise`/`DecodeVexInt`/`DecodeVexMovVec` load 形式/`DecodeVexMovd`/`DecodeVexMovq` 的 xmm 目的）后面都紧跟 `ZeroYmmHigh`;`StoreMemory`(内存目的）与 `Dst(...)`(GPR/内存目的）正确地没有。已由主线独立复核。
- **不可交换运算的源顺序：正确**。`vpandn` → `VecAndNot(right,left)` 得 `src2 & ~src1`(Intel 定义);`vpsub*` → `src1-src2`;`vpcmpgt*` → `src1>src2`。定向测试用 `ref(a,b) != ref(b,a)` 的不对称性断言把顺序钉死，反了必红。
- **`vmovd`/`vmovq`:正确**,五个方向（GPR→xmm、xmm→GPR、mem↔xmm、xmm→xmm)均已执行验证，含零扩展。
- **`VexInfo` 的多数字段只写不读**:实际被消费的只有 `valid` 与 `l`;`vvvv`/`vvvv_unused`/`w`/`pp`/`mmmmm` 均无读取点。handler 是靠 distorm 的操作数**列表形状**区分 2/3 操作数形式，绕开了 vvvv==0 的歧义而非解决它。原注释宣称"vvvv 用作 cross-check"**不实，已订正**。保留字段本身合理（一旦某条编码 distorm 报得有歧义，原始前缀就是唯一权威——`l` 已经是这种情况）。
- **定向覆盖已补**:`TEST_CASE("x86 avx vex128 directed C3 zeroing and source order")`,68 个块 × JIT/解释器自差分 + 手算期望,不用 Unicorn。运行前对 `ymm_high` 逐寄存器逐字节投毒，并用一条 legacy `movdqu` 对照块证明投毒能存活于非 C3 写入——否则"观察到高半为零"不构成证据。

### Rosetta oracle 抓到的实现缺陷

**`VPMOVMSKB` 源寄存器解码错误（已修）。** 根因不在本仓库代码，在 bundled distorm：`externals/distorm/insts.c` 的 `II_V_66_0F_D7` 是其邻域里**唯一没有操作数描述符**的条目（`{{0x18e, 6563}, 0x40, 0, 0, 0, 0}` 对比 vpaddq/vpand/vpxor 的 `{{0x135, ...}, 0x0, 73, 0, 0, 0}`),于是 `ops[1]` 跟的是 **ModRM.reg 而非 ModRM.rm**,且目的被报成 64 位。distorm 自己的文本反汇编就露馅：`vpmovmskb ecx, ymm5`(`C4 E1 7D D7 CD`) 渲染成 `VPMOVMSKB RCX, XMM1`;legacy `66 0F D7` 解码正确。

后果：只要目的 GPR 编号 ≠ 源 ymm 编号就**掩错寄存器**——`vpmovmskb eax, ymm1` 返回 ymm0 的掩码。`vpmovmskb eax, ymm0` 恰好对（reg==rm),这大概就是它长期未被发现的原因。该指令在仓库 census 里出现 984 次。

修法沿用仓库既有的「原始字节预解码」先例（census 文档对 RDSEED 即如此）：新增 `X64Decoder::VexRmRegister()` 从原始编码取 ModRM.rm(含 VEX.B),`DecodeAvx256Pmovmskb` 改用它。**未改动 vendored distorm 表**——那会影响所有消费者且难以验证。修复后 12 处比较全部与 Rosetta 逐位一致。

**`VBROADCASTSS` 寄存器源形式已可解码。** bundled distorm 对 AVX2 的
`C4 E2 7D 18 C1` 仍会返回 `FLAG_NOT_DECODABLE`，但 VEX 指令现先走
`vex_decoder.cc` 的自包含原始字节解码；`0F38:18` 的寄存器源和 m32 源都复用
32-bit broadcast lowering，不再依赖 distorm 的旧表项。

### ⚠ FALLBACK 是致命的，不是优雅降级

前面几处描述把「handler decline → 块 FALLBACK」说成「拒绝翻译而非误执行」——**前半句对，后半句给人的印象错**。实际链路是：
```
InterruptReason::FALLBACK → translator/x86/translator.cpp 的 default 分支
                          → ExitReason::IllegalCode → guest 进程终止
```
**没有解释器兜底**（解释器消费同一份 IR,未解码的指令在那边同样未解码）。

这条决定了 CPUID 的开启策略：一旦 advertise AVX,glibc 的 IFUNC 会立刻选 AVX2 版的 memcpy/strlen 等，**撞上任何一条未实现指令就是进程崩溃**,而不是慢一点。所以「实现了一大批 AVX」并不等于「可以开 CPUID」——必须先确认目标二进制实际执行的指令集被完整覆盖。

初步取证（`llvm-objdump` 统计 `source/translator/linux/tests/` 下的真实二进制）：**musl 构建的 `real_hello`/`real_busy`/`func_tests` 里 VEX 指令数为 0**,而 glibc 构建的 `func_tests_x86_64` 有 **4525 条**。因此对 musl guest 开启的风险远低于 glibc guest。注意静态计数会包含运行时未必选中的 IFUNC 变体，「二进制中存在」不等于「一定执行」。

### 方法论：本轮反复出现的「绿色陷阱」

同一族问题在本轮出现了**四次**，共同特征是**失败会喊，退化不会**——测试变绿了，但覆盖面悄悄塌了：

| # | 表现 | 根因 |
|---|---|---|
| 1 | Unicorn 差分全绿 | oracle **静默算错**（忽略 VEX.vvvv、执行破坏性 legacy SSE） |
| 2 | 修复后测试通过，实际测的是旧代码 | `git checkout`/文件还原让 mtime 回退，make 误判无需重编 |
| 3 | per-case 逐用例全绿 | 每用例独立进程，**结构上**不可能触发跨用例状态污染（掩盖了 slab 堆破坏） |
| 4 | `ctest` 报 All tests passed (1 assertion) | 门控未设时用例 `SUCCEED()` 自跳过，而 `add_test` 没设门 |

**对策已固化**：①任何单一 oracle 的结论都要与第二 oracle 或 SDM 交叉核对；②任何还原/变异测试后 `touch` 源文件或删 .o 再构建；③per-case 与单进程全量**两种跑法都要跑**；④`add_test` 显式设置全部门控。

另有一条正向经验：**变异测试是唯一能证明断言有牙的手段**。本轮多处「全绿」在引入人为缺陷后确实变红，也有一次变异**没有**触发（`UZP1 t,x,x` 使 `UMULL2` 与 `UMULL` 语义等价），被如实报告为空结果而非计入战果——这个区分很重要。

### IR 扩展判据（多 ISA 中间表示）

该 IR 服务多个前端（ARM64/x86/Slang）与多个后端（ARM64/RISC-V64），新增 opcode 的标准是**语义合适 + 平台中性**。判断方法：**换一个 ISA 的前端还有意义吗？换一个后端还能自然实现吗？**

- ✅ `VecUnzip`(VecZip 的对偶,AArch64 `UZP1/UZP2`、RVV `vnsrl/vcompress`)、`VecMulWiden`（正是 SVE2 的 `UMULLB/SMULLB`)
- ❌ 被否决的 `VecMaddUbs`——「无符号×有符号、成对相加、饱和到 16 位」是 x86 特有组合，已拆解为前端用通用 op 组合
- ❌ 不要把 x86 编码约定（`vinsertps` 的三段式 imm8、FMA 的 132/213/231 编号、gather 执行后清零掩码的副作用）塞进 IR——那属编码层，留在前端 handler

参数化用中性形式（lane 宽度、是否有符号、是否饱和用 `Imm`），**不要为每个 x86 变体各开一个 opcode**——这也是 `ir.inc` 既有风格。

**中性 ≠ 该加**：blend 族审查后新增为零，因为现有 `VecOr/VecAnd/VecAndNot`（= AArch64 `BSL`、RVV `vmerge`）已能表达；`VecSelect` 虽是合法中性原语、能把三条降成一条，但不值得为此永久增加一个 opcode。

### oracle 缺陷模式（本轮累计四处）

| oracle | 缺陷 |
|---|---|
| Unicorn | VEX 三操作数形式忽略 vvvv,执行破坏性 legacy SSE(`vpaddw xmm0,xmm1,xmm2` 跑成 `xmm0=xmm0+xmm2`) |
| Unicorn | VEX.256 一律 `UC_ERR_INSN_INVALID`,根本不执行 |
| Rosetta | CPUID 谎报 AVX=0(需 `ROSETTA_ADVERTISE_AVX=1`),但指令照常正确执行 |
| Rosetta | `VPSLLVQ`/`VPSRLVQ` 的移位计数被截断成 32 位（Intel 用完整 qword)。实测：计数 `0x100000001` 时 Rosetta 给 2、SDM 应为 0;而计数 `0x100` 时 Rosetta 正确给 0,排除了「只读低字节」 |

**每一处都只有靠交叉验证（第二个 oracle 或规范本身）才发现，单一 oracle 全绿不构成正确性证据。** 这是本轮方法论上最值得留下的结论。

### 已知偏差与缺口
1. **32 字节访存的故障语义**:x86 上 32 字节访问是**单个不可分割**的架构操作。C1 拆成两次 16 字节后：跨页且仅第二页未映射时，load 会**先写入目的低半再故障**（硬件保持整个 YMM 不变），store 会**先提交前 16 字节再故障**（硬件不产生部分存储）；上半故障报告的地址是 `base+16` 而非 `base`。仅对 SIGSEGV 后继续执行的 guest 可见（用户态故障处理、JIT guard page、mmap 探测型分配器）。要精确修复需先探测两半再提交，或引入真正的 32 字节 IR 访存 op（后端工作）。
2. **32 字节对齐未强制**:`VMOVDQA`/`VMOVAPS`/`VMOVNT*` 应在未对齐时 #GP;沿用现有 SSE `MOVDQA` 同样忽略 16 字节规则的先例。可用机制是 `CheckMemoryAlignment(addr, Imm(31))`。
3. **非临时提示降级**为普通存储。
4. AVX2-only 指令族因 distorm 不解码而**受阻**，需更新 distorm 表或手工预解码。
5. 上述 VEX.128 之外的 mnemonic（`VPERM2I128`、`VINSERTI128`、`VBROADCASTF128`、`VPACK*`、`VPUNPCK*`、移位、全部 FP 算术、`VPTEST` 等）一律 decline → 块 FALLBACK。

### Unicorn 不能作为 AVX oracle（实测，2026-07-26）

在建 AVX 差分 fuzz 时实测 Unicorn 2.1.4，结果**否定了原定的验证路径**：

| 指令类别 | Unicorn 行为 |
|---|---|
| VEX.128 **搬移**(全部 16 种已实现形态：vmovdqu/dqa、vmovups/aps/upd/apd、vmovntps/ntpd/ntdq、vlddqu、vmovntdqa,以及 vmovd/vmovq 的 xmm↔m32/m64、xmm↔r32/r64、xmm↔xmm) | **正确**，可作 oracle |
| VEX.128 **三操作数形式**(vpaddb/vpaddw/vpxor … 全部 18 条) | **静默算错**:Unicorn **完全忽略 VEX.vvvv,执行破坏性的 legacy SSE 形式**——`vpaddw xmm0,xmm1,xmm2` 实际跑成 `xmm0 = xmm0 + xmm2`,**不报错**。用哨兵预置 xmm0 后取回 `哨兵 OP src2` 得证（xmm0 为零时表现为"结果 == src2",容易误判成另一种 bug）。与 AVX 使能无关 |
| VEX.256 全部 | `UC_ERR_INSN_INVALID`,**根本不执行** |

VEX.256 的拒绝在下列组合下均复现：CPU 模型 default / HASWELL / SKYLAKE_CLIENT × 设与不设 `CR4.OSXSAVE|OSFXSR` × 在被模拟代码里 `XSETBV` 把 XCR0 设为 3 或 7。

**后果**：AVX 的验证不能走差分 fuzz 的老路。可用组合是——搬移用 Unicorn 差分；ALU 运算用 **JIT vs 解释器自差分 + 手算定向期望**;**C3 的高半清零规则任何 oracle 都看不见**（观察高半需要 256 位存储，而 Unicorn 不执行），只能靠定向自差分覆盖。

### Rosetta 是 VEX.256 的可用 oracle（实测，本机 Darwin 27 / M4 Max）

原以为 VEX.256 无 oracle 可用，实测推翻了这个判断：

- **Rosetta 会执行 AVX/AVX2 的 256 位指令且结果正确**。`vmovdqu ymm`、`vpaddb ymm`、`vpshufb ymm`、`vpmovmskb r32,ymm`、`vpgatherdd ymm` 全部通过。
- **但默认下 CPUID 谎报**:`arch -x86_64` 里 leaf1 ECX.28(AVX)=0、ECX.27(OSXSAVE)=0、leaf7 EBX.5(AVX2)=0——**指令照样正确执行**。设 `ROSETTA_ADVERTISE_AVX=1` 后 CPUID 才如实报告（XCR0=0x7),执行行为两种情况相同。所以**能力判定必须靠实际执行，不能读 CPUID**。
- **负控成立**（否则上述结论无意义）:AVX-512 的 `vmovaps zmm0,zmm1` 与 `ud2` 都触发 SIGILL,证明确实在执行而非静默忽略。

已建 `TEST_CASE("x86 avx256 vs rosetta reference")`:522 次比较 × JIT/解释器双后端、264 条参考值、0 条 SKIP。参考数据生成器 `source/tests/fuzz/avx256_rosetta_ref.c`(x86_64 独立程序，不进 CMake)与测试**逐字共用** `avx256_ops.inc` 的指令表，两侧不可能漂移；它从表**运行时汇编**指令而非写 inline asm,且用「执行 `vpaddb ymm`」而非读 CPUID 判定能力。全部数值是 Rosetta 写进内存的字面字节，无一手算。

硬件判决了两条此前只能靠推理的语义：**`vpshufb ymm` 确为按 128 位 lane 独立**（per-lane 与 cross-lane 两种预测在 32 字节里差 30 字节，Rosetta 给的是 per-lane),**`vpmovmskb ymm` 确为 `lo | hi<<16`**（正反序可区分）。实现两条都对。

这也是一条方法论教训：**oracle 静默算错比 oracle 报错危险得多**。首次跑 AVX fuzz 得到 1308 处"不一致"，若不手算核对就会全部误判为本方实现的 bug。

### 已建立的 AVX 测试（三例，均在 `SVM_AVX=1` 下运行，未设时自跳过）

| 用例 | oracle | 覆盖 |
|---|---|---|
| `Fuzz x86 avx vex128` | **Unicorn 差分** | 仅**数据搬移**——7 种 load、9 种 store、两个方向的 reg-reg,以及 vmovd/vmovq 与 GPR/内存之间的全部形态。三个向量寄存器都回写，旁路 clobber 会被抓到 |
| `AVX VEX.128 packed integer directed edge vectors` | **手算字面量 + JIT/解释器自差分** | 18 条 ALU 运算。**双层**:第一层用 18 组手算的 16 字节字面量把参考模型钉死（否则模型与实现同错会一起变绿）;第二层 39 组边界操作数 × 6 种操作数形态（含 dst 别名到 src1/src2)在两种后端上跑，共 4212 次比较 |
| `x86 avx vex128 directed C3 zeroing and source order` | **`ymm_high` 投毒 + 自差分 + 手算** | **C3 高半清零**（无 oracle 可见，只能这样覆盖）+ 不可交换运算源顺序。用一条 legacy `movdqu` 对照块证明投毒能存活于非 C3 写入，否则"高半为零"不构成证据 |

两处**变异测试**证明断言有牙：把 `vpaddb` 的 opcode 改成 `vpaddw` → 手算层报错而自差分层**沉默**（两个后端一致地错，这正是纯自差分抓不到、手算层才能抓的情形）；改坏一个字面量字节 → 第一层报错。

### 下一步

①–⑤ 全部完成：AVX 差分 fuzz 已建；VEX.128 形式已补齐；**Rosetta 已实测可作
VEX.256 的 oracle**（本机 Darwin 27 / M4 Max，需 `ROSETTA_ADVERTISE_AVX=1`
才有诚实 CPUID，但无论如何都会执行 AVX——所以能力判定必须靠实际执行而非特性
位）；32 字节跨页访存 stage 0–4 已验证行为正确；CPUID + XCR0 已按 C4 打开。

**剩余**：
1. **`pcmpXstrY` 四条**——开 CPUID bit 20 的唯一前提。整族留白而非做一半：
   现在 guest 带着 IllegalCode 响亮地死，做一半会返回错误索引，让
   `strlen`/`strstr` 去读错误的内存。
2. **32 字节跨页访存的撕裂写问题仍未回答**（stage 5）。此前被「clone() worker
   死于 PageFatal 会带走宿主」挡住，该缺陷已修（见 §5），可以重做了。
3. MMX 形式的共享 opcode（`pshufb`/`palignr`/`pextrw`/`pmovmskb`/`pmuludq`/
   `psadbw`/`pminub` 的 no-66 编码）目前会被拿去操作 XMM 寄存器堆而不是陷入
   ——静默错数据。今天暴露面低（CPUID 不报 MMX），但是潜伏陷阱。

---

## 5. 已知问题与遗留

### ~~P-1 — 隔离性：guest 能读写宿主任意内存~~ **已修复**（`81fa5d6`，2026-07-27）

这是目前最严重的问题，级别高于本节其余全部条目：它不是「算错」，是**逃逸**。

guest 访存一律是 `host = guest + bias`，**全链路没有任何边界检查**（JIT 发射点
`backend/arm64/jit/translator.cpp:327` 的 `MemOperand{scratch, pt}`）。bias 加法
不等于隔离——当 `guest+bias` 恰好落在宿主已映射区域上，访问**静默成功**。

实证（lldb、关 ASLR、从 `SetGuestMemBias` 的 `$x0` 读 bias、就地改 guest imm64）：
- **读**：guest `mov eax,[0x14C000]` 拿到 `0xfeedfacf`，宿主自己的 Mach-O 头。
- **写**：宿主 `malloc` 的缓冲区在 `0x100895930`，guest 往 `buf − bias` 写，
  宿主 `exit` 断点处 `*(u64*)buf == 0x4141414141414141`。

同源的两处：① SMC 写保护的 `mprotect` 会落到 bias 指向的任意宿主映射，**有时
正是翻译器自己的 `__TEXT`**，于是它在自己脚下丢掉执行权限（现象 `fault addr ==
faulting pc`，约 2.5% 的野分支用例命中）；② x87/fxsave 与 `rep movs/stos` 系列
helper 在**宿主代码里**解引用 guest 地址，故障 pc 不在 JIT buffer 内，
`HandleFault` 恢复不了。②**不是三行能修的**：helper 的栈帧还在，
`SetContextPC(return_host)` 无效，需要 helper 内校验 + 故障返回路径，或
helper 感知的 unwind。

**已修复**：有界 2^32 guest 窗口 + 地址截断。**32 位下零开销**——arm64 本就用
寄存器偏移装载加 bias，`ldr xN,[base, pt]` 直接变成 `ldr xN,[pt, Wbase, UXTW]`，
截断折进了寻址模式；`mem` 负载反而快 3%（guest 现在住在一整块连续保留区里，
而不是散落的宿主 mmap）。保护页方案在正确性上不成立：guest 地址是无界的 64 位，
`+2^40` 不是任何有限保护带能挡住的。

回归套件 `run_isolation_tests.sh` 21 例：修后 21/21，`SVM_GUEST_BITS=0`（恢复
未隔离行为）下 10/20——十处逃逸全部失败。原始实证不可直接复现为测试（两个宿主
目标都随 ASLR 变位），所以套件断言**性质**：有窗口时 `[base]` 与
`[base + k·2^bits]` 必须别名，「没有别名」恰恰等于「这次访问触到了宿主内存」。

### ~~P-1b — 隔离性的另一半：helper 里的故障~~ **已修复**（2026-07-27）

上一轮遗留的「架构那一半」现已收口。三条独立的缺陷，逐条给证据：

**(1) clone worker 的缺页打死宿主——根因不是缺页处理。** 复现：
`SVM_AVX=1 svm_translator_linux avx_crosspage_x86_64 5` →
`unhandled host fault: SIGBUS at pc=... addr=0x7000400df4`。lldb 回溯定位到
`SyscallProcessState::StoreGuestU32` ← `X86GuestProcess::RunThread` ← 线程
teardown，`si_addr` 正是 guest 的 `&xp_ctid`。即：**CLONE_CHILD_CLEARTID 的
清零存储是宿主对 guest 内存的写**，而该 guest 页与代码同页、被 SMC 写保护，于是
在一个 `tls_active_runtime == nullptr` 的线程上吃到保护故障，`HandleSmcFault`
拒绝认领 → `DefaultHandler` 杀进程。对照实验：`SVM_SMC_MT=0`（解除全部写保护）
与 `SVM_ENABLE_JIT=0`（解释器不做 SMC 跟踪）下崩溃消失，确认是写保护而非缺页。
修法：`runtime.cpp` 增加线程本地 owner slot——「本线程拥有的 Runtime」，从构造
活到析构，覆盖 JitRun 之间的宿主代码（syscall 模拟、线程 teardown）。宿主写到
代码页语义上就是自修改代码，打开写窗口 + 失效翻译并重试正是正确处理。
该类缺陷不止 clone：任何把结果写回 guest 缓冲区的 syscall 都同形。

**(2) helper 内的 guest 故障必须变成 guest #PF。** 此前 x87/fxsave 的
`GuestPointer` 返回 `nullptr`、调用方一律 `if (!out) return;`——`fnstenv [bad]`
等成为**静默空操作**；`rep movs/stos/cmps/scas` 的 `ClampGuestWalk` 只把走查夹在
**窗口**内，对窗口内的未映射页毫无防护——实测（干净 HEAD）**直接打死宿主**，
比原以为的「静默少搬数据」更糟。
方案选型（三选一，选了 1）：
| 方案 | 结论 |
|---|---|
| helper 感知的 unwind | 需要 setjmp/longjmp 或 DWARF；每次调用有固定成本，且信号处理器里 longjmp 要手工恢复信号掩码 |
| 把 helper 访存改走 JIT 路径 | rep 走查本质是循环，无法用单条 IR 访存表达 |
| **helper 内校验 + 可见故障返回通道** | **选用**：无 unwind、无新后端 op |
返回通道的落点是**位 63**（`kX87GuestFault` / `kStringGuestFault`）：x87 helper
只回 FSW(16 位) 与 FCOMI 三位，`RepMovs` 原本返回 void，`RepStos*` 原本返回填充
末地址——已改由 IR 计算（走查连续，末地址是输入的纯函数），把返回值腾出来；
`RepCmps*/RepScas*` 回的是元素计数，被窗口夹断后 ≤ 2^32。发射侧复用既有的
`CheckMemoryAlignment(v, mask)`——它就是「`v & mask != 0` 就带 PageFatal 退出块」
的通用原语（`DecodeCmpxchg16b` 的非对齐 LOCK #GP 用的同一条），因此**后端零改动**。
`SetLocation(insn_pc)` 在前，保证 halt 报的是出错指令自己的 rip。
`rep cmps/scas` 只在「把夹断后的额度全部走完」时才报故障——提前因比较结果终止的
走查没有碰到洞，不欠故障。

**(3) 性能约束（上一位 agent 因此没做）已用数据解决。** 「每次调用一把锁 + 二分
查找」确实不可接受，所以没有沿用 `RangeIsMapped`：`GuestMemory` 新增
**页存在位图**（每 16 KiB 宿主页 1 bit，默认 32 位窗口下 32 KiB，calloc 惰性
落页），配合 `SignalHandler::SetGuestRangeProbe` 的**区间**探针——一次间接调用
回答整段走查，无锁。实测（独立 worktree、同一次进程内交错、11 rep、loadavg≈10）：

| 负载 | 中位数 | 说明 |
|---|---|---|
| `rep movsb` count=8 紧循环（合成最坏） | **+30%** | 每次调用 2 次探针，约 2.4 ns/探针 |
| `rep stosb` count=8 紧循环（合成最坏） | **+46%** | 每次调用 1 次探针 |
| `rep movsq` count=512 | +0.4% | 探针成本被搬运摊薄 |
| `func_tests`（真实 glibc，翻译密集） | −0.2% | 噪声内 |
| `mem` / `int` / `real_busy` | −0.1% / +0.9% / +1.5% | 噪声内 |

即：合成最坏情况有真实代价，**任何真实负载测不出差别**（glibc 的 memcpy 走
SSE/AVX，我们不上报 ERMS，`rep movsb` 不在热路径上）。位图同时让指令取指的
`GetPointer` 从「shared_lock + 二分」变成一次位读。

**回归套件**：`run_helper_fault_tests.sh` + `gen_helper_fault_guest_x86_64.py`，
38 例 = 13 形状 × 3 种 lowering（默认 / `SVM_X87_JIT=1` / `SVM_ENABLE_JIT=0`）。
断言三件事：宿主活着、guest halt 于 `reason 2`、**且 guest 没有打印 SURVIVED**
——每个形状在故障指令之后都继续写一个标记，所以「helper 跳过了访问、执行继续」
是显式可分辨的，而不是与「故障了」长得一样。
**当前 HEAD**：**38 通过 / 0 失败**。此前记录的 **9 通过 / 29 失败**来自当时
保留的**旧 clean binary**，用途是修复后的负向对照，不是当前 HEAD 的套件结果。
**变异测试**（7 个变异，逐个重编译后跑套件）：

| 变异 | 结果 |
|---|---|
| 去掉 `HandleSmcFault` 的 owner-slot 回退 | 杀死（clone_pf 红） |
| `X87Dispatch` 不 OR 故障位 | 杀死（10 红） |
| `CallX87` 不发射故障检查 | 杀死（10 红） |
| `ClampGuestWalk` 不上报映射不足 | 杀死（18 红） |
| `DecodeMovs` 去掉故障检查 | 杀死（6 红） |
| 位图不再更新 | 杀死（38 红） |
| `~Impl` 里的 `CloseWriteWindow` | **未杀死** → 该行已删除，见下 |

`~Impl` 的 `CloseWriteWindow` 是「线程退出前关掉宿主开的写窗口」的防御性调用，
变异测试分辨不出有无——而且它本就不必要：`RegisterNode` 的 `!rec.dirty` 守卫
与 `CloseWriteWindow` 的重试循环已经负责收集「别的线程开窗期间发布的翻译」。
**未经验证的防御性代码不留**，已删。

**`SVM_GUEST_BITS=0` 改为编译期门控**：普通构建直接拒绝并点名
`-DSWIFT_ALLOW_UNBOUNDED_GUEST=ON`（cmake option，默认 OFF）。
`run_isolation_tests.sh` 新增一条断言就是这个门（第 21 例），并在
`SVM_ISOLATION_EXPECT=broken` 下检测到门存在时 SKIP 并说明需要专用构建。
专用构建实测仍能演示缺陷：10 通过 / 10 失败。

**顺带答出了一个悬置的问题**：`run_avx_real_tests.sh` 的 stage 5 此前因为这条
崩溃报 BLOCKED。修好后它给出真实结论——**32 字节跨页存储在故障时确实留下撕裂
写**：落在已映射页里的 16 字节被提交，随后第二半缺页（`first_page_partial=1`、
`first_page_prefix_intact=1`、`observer_fault=1`）。这是契约 C1 的 2×16B 下降的
直接后果；x86 不承诺跨页原子性所以不算违规，但它**对另一个 guest 线程可见**，
且与当前 x86 硬件的实际行为不同。runner 现在断言机制（worker 死于故障、宿主存活、
未越界写）并把撕裂位作为 ANSWER 打印，而不是把它钉死成期望值——将来真做成硬件
忠实的下降不该因此变红。

**仍未解决**：故障后的寄存器状态是近似的。x86 在 `rep` 中途 #PF 时
RCX/RSI/RDI 指向出错元素；我们发射的更新在故障检查之后，故障路径根本走不到它们，
guest 也拿不到可恢复的 #PF（PageFatal 直接终结该 guest 线程），所以这个差异
目前不可观测——但如果将来实现了 guest 信号投递，它就变成一个真实缺口。

### P0 — 潜伏 bug（触发面已收窄但未根治）

1. ~~**防御性 spill 路径误编译**~~:**已根治**。根因不在 `JitContext` 的延迟回写协议，而在 `LinearScanAllocator::SpillAtInterval`——整数 grow 分支用 `slot = spill_slot_cursor`,而 cursor 存的是**上一次** grow 的索引（`GrowSpillStack` 先赋 cursor 再 resize),于是第 2、4、6…… 次 GPR spill 与前一次**共用同一个 slot**;同时 SIMD spill 只标记 `spill_slots[slot]` 而不标记 `slot+1`,整数分支的线性扫描会把上半 slot 再发给别人，两个 16 字节访问互相踩。两者都无断言保护，静默产生错误数据——这正是 f32 不定 NaN `0xffc00000` 能进入 `State::current_loc` 并被当作 guest PC 派发的路径。修复：grow 时取 `spill_slots.size()`,SIMD 两个 slot 都标记并按偶数下标对齐（保证 16 字节 `Ldr/Str q` 的缩放偏移可编码），删除诱发该错误的 `spill_slot_cursor` 成员。回归测试见 `main_case.cpp`"Register allocation gives every spilled value a private slot"——在修复前该测试红（标量场景 5 处碰撞），修复后绿。
   遗留（**不是**误编译，失败时响亮）:spill 的 interval 不入 `active_lives`(`register_alloc_pass.cpp:100-114`),导致 `ExpireOldIntervals` 的 MEM 分支与 `FreeSpill` 恒不可达,slot 只增不回收,压满 `kMaxSpillSlots=64` 时断言退出。
2. **vixl ip0/ip1 clobber 类**（最热点已消除，仍未全面根治）:vixl 的 `tmp_list_` = {x16,x17}(`macro-assembler-aarch64.cc:323`),而 `trampolines.cpp` 的保留清单**不含 x16/x17**。x86 配置下可分配 GPR 池共 16 个,`GetFirstClear` 低位优先,**x16/x17 恰是第 13/14 个**——只要有 13 个值同时活跃,linear scan 就会把 guest 值放进 x16。`ReserveTmpX` 只作用于 `GetTmpX`,对分配器给出的 guest 值**零保护**。
   已修：`SaveLogicalResultFlags`/`SaveNZ` 里的 `Bics(ip, ip, 0)`。vixl 会把 BIC 立即数取反成全 1,全 1 非合法逻辑立即数,故 LogicalMacro 必然物化进 x16——实测展开为 `mov x16, #0xffffffffffffffff` + `ands x11, x11, x16`。改用 `Tst(ip, ip)`(ANDS 的寄存器形式,NZCV 完全相同）后为单条 `tst`,不取 scratch。该路径由**每个 flag-setting 的 x86 OR/XOR**(含 `xor r,r` 清零惯用法）触达,是后端最热的暴露点。
   未修，按暴露面排序：①`EmitVecMovMask`(`translator_alu.cpp:838`) 的 `Movi(V16B,hi,lo)` 每次必取 x16;②`EmitOperand`(`translator.cpp:244`) 只用 `IsImmAddSub` 判定就返回立即数 Operand,但下游是**逻辑**宏,任何 ≤4095 却非合法位掩码的立即数（如 `and r,0x123`)都会物化进 x16;③`EmitX87Op` 内 57 处编译期常量比较/掩码（`Cmp(w,0x7FFF)`、`And(fsw,0xC5FF)` 等）——该函数已 `ReserveTmpX(ip0/ip1)`,故只威胁分配到 x16/x17 的 guest 值,不威胁自身临时;④`MergeLogicalFlagsNZ`/`EmitTestFlags` 的非连续 NZCV 子集掩码（常见形状均可编码）。
   **根治已落地**:x16/x17 加入 `trampolines.cpp` 的保留清单，池 16→14。该方案曾于 `96c6971` 全局保留、`5211e81` 因 SSE 高压回归而回退;**当年回归的真正原因很可能就是它把代码推上了当时静默损坏的 spill 路径**（见遗留 #1)——先修 spill 是重试它的前提。实测：非 fuzz 24 PASS/1 FAIL(仅既有 #10b),Unicorn 差分 fuzz **32 PASS/0 FAIL**;且最重的定向用例（glibc 72-block 函数、large lambda-free CFG、SSE batch B、x87)在池 14 下**spill 次数仍为 0**,即保留两个寄存器没有把任何现有 workload 推上 spill 路径。
   仍建议保留 §3 的 emitter 纪律：新增高压 emitter 时仍需数峰值临时数（现池=14),`ReserveTmpX` 对 x16/x17 已无必要但无害。
   **v31 已排除**:vixl 的 `fptmp_list_` = {d31} 同样未保留,但 `AllocFPR` 低位优先且空闲 FPR 有 28 个,v31 是最后一个才会发出;且经审计后端没有任何 emitter 会触发 vixl 取 FP scratch 的两条路径(`Fcmp` 立即数形式、mem-to-mem `Mov`)。实际不可达,无需处理。
3. ~~**SIGALRM × 长跑 → ~10–20% PageFatal**~~：**已根治（2026-07-30，`32e7341`）**。根因 = rt_sigreturn 的 `GuestSignalPrivate` 栈扫描命中上一信号周期的残留帧（magic 残留 + ucontext_addr 槽被 saved rbp 骗过），`saved_context` 被 guest 栈流量污染（fs_base=0）而 ucontext 完好。修复 = 按内核语义删私有帧：sigreturn 从当前 ctx 出发只套 uc + fpstate（fs_base/gs_base 从不随信号保存/恢复）。详见 §2 W20f 条目；`SVM_SIGNAL_PRIVATE_FRAME=1` 保留旧路径供二分。SHA-NI 已随此默认转正（`0a40a6b`）。
4. ~~**Fuzz x86 bit ops 随机 SIGILL**~~：**已根治（2026-07-30，`ad40395`），根因在 Unicorn 不在我们**。Unicorn 2.1.4 ARM64 JIT `tcg_out_rotl` 在 TCG 把 32 位 CL 循环计数常量折叠为 0 时发射未分配的 `extr Wd,Wn,Wm,#32`（种子 5070672046091958327：`xor rcx,rcx; rol cl,cl; lahf; seto r15b; hlt`）。我们侧的 rotate lowering 经新增专项回归（"Fuzz x86 bit ops SIGILL repro"，原始字节直跑 SwiftVM，断言 RCX=0/AH=0x46/OF 不变）验证正确；fuzz harness 在 `__aarch64__` 差分运行时把 CL 钉在 1..width-1 绕开 Unicorn 缺陷。

### P1 — 忠实度差异（默认路径冻结，不影响 Unicorn fuzz)

3. ~~**helper vs 真实 x86 两处**~~:**已修复**。
   ① **FIST/FISTP/FISTTP 的 C1**:`StoreInteger` 现按 SDM 语义置位——把整数结果加宽回 ext80 与源比较，结果**大于**源（向上舍入）才置 C1;inexact 标志只给大小、不给方向，所以必须比较。另发现一处连带缺陷：`Pop()` 无条件清 C1,而 FISTP 的 pop 属于同一条指令，会抹掉刚记录的舍入方向——已在 `StoreInt` 的分派处跨 pop 保留 C1（栈下溢时 `StackFault` 已把 C1 清零，该 0 同样被保留）。
   ② **m32/m64 load 的 IE**:`LoadMemoryValue` 曾在**局部** `state` 上做 `f32/f64_to_extF80` 后直接丢弃，SNaN 被 quiet 了但 IE 从未进状态字。现已 `RaiseSoftFloat`。加宽到 ext80 恒为精确，故不会引入伪 PE/UE。
   **实测结论与原判断不同**:Unicorn 差分**并未变红**（它根本不在 FIST/FLD 之后取 FSW,观察不到这两处），所以**不需要 fuzz 掩码**。真正变红的是定向断言 `x87 directed edge semantics`——它当时编码的正是旧的错误行为，其注释已明说"helper pre-quiets this SNaN without carrying IE"。该期望已订正，并新增一条正向断言把 C1 钉死：以 +1.5 走四种舍入模式，C1 必须恰好在结果为 2（向上舍入）的两种模式下置位。
   **随后被 Rosetta 仲裁纠正的判定式错误**：首版实现用「结果 > 精确源」判定 C1,**对所有负源都是反的**。真实 x86 判决：
   ```
   FIST m32(-1.5) RC=nearest → 存 -2, C1=1   但 -2 < -1.5，值比较给 0
   FIST m32(-1.5) RC=up      → 存 -1, C1=0   但 -1 > -1.5，值比较给 1
   ```
   C1 报告的是**有效数是否进位**，即 `|结果| > |精确源|`。首版的定向测试没抓到，因为它**只用了 +1.5**——正源下两种语义恰好一致，根本无法区分。教训：**验证舍入方向类语义必须带负源**，正负互为镜像才是自证的。现已抽出共用判定 `RoundedUpInMagnitude()`(`x87.cpp:298`,清符号位后比幅度）供全部落点使用。
3b. **x87 C1（舍入方向）全面排查**（2026-07-26,期望值均来自 Rosetta 实测）。
   已修：`StoreFloat`(FST/FSTP m32/m64,m80 恒 0——实测确认即使源在窄格式里 inexact 也报 C1=0/PE=0)、FSTP 的 pop 抹除、`StoreInteger` 的负源反向、`StoreReg`(FST/FSTP ST(i) 此前完全不碰 C1、留陈旧值)、`Unary::Round`(FRNDINT)、`Unary::Sqrt`(用 round-toward-zero 重算一次比对，仅 inexact 时触发)、以及 `translator_x87.cpp` 中盘 `StoreInt` 的两处（C1 恒 0、pop 掩码 `0xC5FF` 含 bit9 会抹掉 C1,改 `0xC7FF`)。**中盘那两处此前与 helper 背离**——`SVM_X87_JIT=1` 下定向断言本已是红的。
   定向测试新增约 500 条断言，正负源互为镜像;**负向对照**：回退实现只留测试，7 条 C1 断言全红（40 个失败断言），证明它们确实在揭错而非空转。三种模式（默认 / `SVM_X87_JIT=1` / 再加 `SVM_X87_TOPVIRT=1`)均 2557/2557 通过。
   **未修（有理由）**:`Binary`(FADD/FSUB/FMUL/FDIV) 无条件清 C1——判据已用 Rosetta 验证（以 round-toward-zero 重算，不等即进位），但这是 x87 最热的 helper 路径，inexact 时要多跑一次 SoftFloat,且 `translator_x87.cpp` 有对应的中盘孪生体，只修 helper 会重新制造上面那类背离。注意「RC=down/up/chop 三模式可由舍入模式+结果符号零开销得出、只有 nearest 需重算」这个优化——**半修（三模式对、nearest 错）比明确记为缺口更难排查**，故整体留待决策。`Scale`(FSCALE) 同理但价值更低（乘 2^n 恒精确）。中盘 FSQRT 拿不到第二次求值，且该路径本就是刻意的 f64 降精度管线,C1 保真无独立意义。
   **无法判定**：超越函数（FSIN/FCOS/FPTAN/FSINCOS/F2XM1/FYL2X/FYL2XP1/FPATAN)完全不碰 C1,但当前实现是「转 host double → libm → 加宽回 ext80」,结果本就不是正确舍入的,「舍入方向」在这个实现里没有可定义的答案。要修得先有正确舍入的 ext80 超越函数。

4. **x87 opt-in reduced 语义**:FMUL/FDIV 走 f64 受控精度（文档化 diverge),FADD/FSUB 以 IXC 守卫保位精确；FILD m64 守卫 |x|≤2^53。非架构 provenance 标记现在只有 **0xA5**，表示该值已获准走 reduced fast path；未标记值保留在 SoftFloat 路径。
5. **TOP 虚拟化默认 OFF**:stock bench 收益为零（bailout 主导），该模式 fuzz 覆盖薄。关键不变量已写入代码注释（pin 读取必经 TOP reload 的全寄存器 Ubfx 顺带清陈旧 mask)。

### P2 — 工程遗留

0. **PKRU 只有架构寄存器、无 enforcement**(2026-07-27 起）:RDPKRU/WRPKRU 解码并
   维护 `ThreadContext64::pkru`,但 **CPUID.PKU 不广告**、pkey_mprotect 与逐访问
   权限检查均未实现——无条件使用者看到一致寄存器,遵守 CPUID 的软件永不触发。
   同型分歧:XSAVEC 广告但写标准格式(XCOMP_BV=0,SDM §13.10 要求 compacted,
   已文档化;CPUID.0xD.1 EBX 报标准面积使按 EBX 分配的 guest 安全）。


6. **x87 14-byte legacy env**(FNSTENV/FLDENV 的 **16-bit 操作数形式**）未实现——`StoreEnvironment`/`LoadEnvironment`(`x87.cpp:1021`/`:1042`) 只写读 4 字节字段共 28 字节，即 32-bit 形式；16-bit 形式需 2 字节字段共 14 字节。FIP/FDP 非逐指令精确（helper 路径正确，内联路径是块级近似）。
7. **16 个长模式非法 mnemonic**（普查确认不可达，记录不实现）;32-bit guest 模式所有构造点均 is_64bit=true。
8. ~~**静态映射仅 RSP→x19**~~:已完成。RBX→x20、RBP→x21 扩展随 `a1f8292` 落地，映射表见 `source/translator/x86/translator.cpp:37`。
9. **CallLambda 多块函数 liveness** 保守回退（host-call 跨块活区间未证明）；>64 块+lambda 走块编译（任务 #43)。
10. **aarch64 解释器 PageFatal 缺口**（既有）。
10b. ~~**`Test runtime` 定向用例 SIGABRT**~~:**已修复**（两个缺陷叠加，修掉第一个后第二个才浮出）。
   ① `Module::Push` 经 `IntrusivePtrAddRef` 取得所有权，最终释放走 `Block::operator delete` → `SlabObject::TryFree`,而 TryFree 对不在 slab 内的指针调用 libc `free()`。用例把**栈上**的 Block 交进去，作用域结束时栈对象先析构、随后 module 释放引用 → 对栈地址 `free()`。改为堆分配（与 `runtime.cpp` 里 `TranslateIR` 喂给 Push 的方式一致）。
   ② 用例用 `loc_end = UINT64_MAX`,而 `AddressHashMap` 按每 1 MB 一个指针预留,2^64 的范围要 `2^44 × 8 = 128 TB`,mmap 拒绝后 `AllocateMemoryPages` 的 `ASSERT(base)` 触发。生产代码用 `1<<49`(x86)/`1<<48`(arm64),只有该用例是异类，已按其自身声明的 `backend_isa=kArm64` 对齐。
   **注意 `Module::Push(ir::AddressNode*)` 是「裸指针签名 + 取得所有权」的接口**——这是本次两个缺陷的共同诱因，未来若有新调用点应留意。
11. ~~**"Flag elimination" 定向断言失败**~~:已修复。根因是**测试陈旧**而非 pass 有 bug——测试（`7a909ba`,07-23）用 `Flags::All` 断言首个 SaveFlags 被删，但次日 `4003358` 引入的 carry 保护对任何含 C 的掩码一律不删。已把该场景改用 `Flags::NZ`（真正覆盖"死 pseudo 消除"），并新增一个 carry 掩码场景显式断言两个写都必须存活。
12. **信号 backpatch TSO 不做的结论已固化**:FEX 用它服务非对齐机制而非优化；我们编译期对齐证明+廉价分支已获同等快路径，成本（并发代码补丁+I-cache 同步+信号链耦合）不值。
13. **B3 — JIT 磁盘缓存的模块归属只在单模块实践下安全**：缓存单元以
    `guest_start` 为键，`RecordUnit` 当前忽略传入的 `module`，加载时又统一复活到
    `default_module`。今天 `AddressSpace::MapModule` 没有调用者，所以不存在地址相同
    但模块不同的碰撞；`config_hash`、`env_hash` 与逐块 guest-byte hash 也覆盖了现有
    单模块场景的漂移。若将来启用第二模块，这些键不足以表达 ownership，可能把单元
    归到错误模块。`MapModule` 已加高声量日志守卫：磁盘缓存 active 时一旦映射附加
    模块就明确报告 cache ownership ambiguous；真正支持多模块前必须把模块身份纳入
    持久化键与复活目标。
14. **M1 — Build-ID 是文件身份摘要，不是内容哈希**：`ComputeBuildId` 对可执行文件
    的 path、size、秒级 mtime、inode（再加 host image span）做哈希；若同一 inode
    被原地替换为同尺寸内容且 mtime 被保留，ID 可能不变。加载阶段仍会逐块校验 guest
    bytes，所以影响被限制在 build ID 未能隔离的 host-code cache 候选，而不是绕过
    guest 代码漂移检查。需要更强的发布/攻击模型时应改为内容或构建系统提供的 ID。
15. ~~**无动态链接 glibc guest 覆盖**~~:**已落地**(2026-07-28,`b269527`)。采用
    QEMU linux-user 模型——不实现链接语义，loader 解析 PT_INTERP、把 glibc 2.38
    ld.so 映射进 guest 窗口并以正确 auxv(AT_BASE/AT_ENTRY/AT_PHDR…)把控制权交给
    它，重定位/IRELATIVE/lazy binding 全部作为 guest 代码翻译执行。配套：
    `SVM_SYSROOT` 路径重定向(绝对路径优先 sysroot、不存在回退 host，未设置时
    逐字节旧行为)、MAP_FIXED 4KiB 亚页段加载(保留同 host 页相邻段字节)、
    `run_dynamic_tests.sh`(默认/AVX+XSAVE/lazy/eager 3 正例+缺 sysroot 负例，
    AVX+XSAVE 配置实测跨 `_dl_runtime_resolve_xsavec`)。静态 auxv 布局字节级
    冻结,fingerprint 4400 零 diff。三个记录在案的语义分歧：
    ① **亚页 mprotect 不强制**:glibc RELRO 是 4KiB 对齐但跨 16KiB host 页边界的
    范围，host 无法表达(扩大会误伤同页可写邻数据)——16KiB 可表达范围真
    Protect+SMC 失效，亚页范围返回成功不强制;仅对真实硬件上会 SIGSEGV 的
    guest 代码可观测(guest 窗口已被地址空间隔离兜底)。完整 enforcement 需软件
    guest 页权限表,列为后续。② **MMX ABI 标记**:glibc GNU property 把 MMX 计入
    x86-64-baseline,无此位 ld.so 拒绝加载任何 DSO——CPUID.1:EDX.MMX 经
    `SVM_X86_64_ABI_BASELINE` 仅限 PT_INTERP 启动广告,MMX 指令本身仍响亮 #UD;
    静态 guest CPUID 不变。③ munmap/mremap 亚页操作仍是 16KiB 粒度近似。
16. **XSAVE 语义分歧(已文档化,接受)**:XSAVE/XRSTOR 不强制 64 字节对齐 #GP、
    保留位/XCOMP/超 XCR0 位一律屏蔽不 #GP、MXCSR 非法位屏蔽;XGETBV ECX!=0 应
    #GP(0) 但无 #GP 出口类型,以 terminal IllegalCode 近似(decoder_xsave.cc)。

---

## 6. 开发工作流约定（本仓库现行）

- **codex 实现 + host 独立复核**:codex 沙箱无法运行 Unicorn(初始化即 SIGILL),fuzz 验收必须 host 侧做；codex 永不 git commit。历史上每轮独立复核都抓到过真 bug(2 个 x87 provenance、movbe decode、cpuid 测试自相矛盾、vixl ip0/ip1 类）。
- **分支隔离验证不足（三次验证）**:cmpxchg16b(x13)+指令覆盖（x30)+16-temp emitter 各自绿、合并才 PANIC;dlmalloc interposition 靠布局 oracle 潜伏。**合并后必须在 master 重跑全量**。
- **多 worktree 并行**:SwiftVM-{x87,x87mid,x87top,tso,static,sse2,fixstack,nanfix};master 只在主仓库检出。
- **调试布局 flake 的工具链**:SVM_FORCE_FIXED_STACK + macOS .ips 崩溃报告（~/Library/Logs/DiagnosticReports,python 两行式 json 解析）+ /cores(ulimit -c unlimited，系统会快速清理需立刻分析）+ lldb（注意会改变布局掩盖 flake)。
