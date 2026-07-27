# SwiftVM 项目状态（2026-07-26）

本文档记录当前里程碑状态、已验证能力、性能快照与已知问题。架构细节见 [ARCHITECTURE.md](../ARCHITECTURE.md)，x86 指令覆盖清单见 [x86-instruction-census.md](x86-instruction-census.md)。

## 一句话状态

x86_64 guest → 自定义 IR → host ARM64 JIT(vixl) 的 DBT 主干在真实 glibc/musl 静态二进制上端到端验证通过；多线程 guest(clone/futex)、TSO 内存序、SMC 自修改代码（含 MT 安全回收）、SSE2 基线、x87(opt-in JIT）均已落地。master = `76cdcd0`。

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
| SVM_ARM64_LRCPC | 0/1 | 1 | TSO LRCPC 快路径 |
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
