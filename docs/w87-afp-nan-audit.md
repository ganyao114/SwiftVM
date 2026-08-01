# W87：FEAT_AFP NaN guard 消除 audit-first 筛选

日期：2026-08-01  
代码基线：`9859c07`  
FEX 取证版本：`f2e35f336f0b8bb0df979ffe10e7c6ffbd8af89c`

## 0. 结论

**全部 audit 门通过，批准进入默认 OFF 的实现 spike。**

结论分成两层，不能混写：

1. **语义与面积门通过。**在真实 Arm 硬件上，`FPCR.AH=1, NEP=1, DN=0` 对本项目
   W37 覆盖的 `FADD/FSUB/FMUL/FDIV/FSQRT`，在 scalar/packed × f32/f64 上位级复现
   x86 SSE 默认 MXCSR 的 NaN 选择、SNaN quieting、payload/sign、invalid-op 负 indefinite；
   64 个二元 NaN 组合原始输出零 diff。`FIZ`/`FZ` 也分别对应 x86 `DAZ`/`FTZ`。
2. **动态可删面积过 5% 门。**本轮临时探针在 macOS 七语料得到 STREAM 12.530542%、
   smallpt 10.543499%、c-ray 6.554911%；归档的 W71 Orb Linux c-ray `-s 8`
   是 6.585859%，方向和量级一致。这些是“entry-weighted 可删 host 指令机械上限”，
   不是 cycle 占比或净加速承诺。

指定的 Linux 目标系统门也已签字：orchestrator 在 Orb ubuntu/aarch64 上以
`gcc -O2` 编译同一 `/private/tmp/w87-afp-arm.c` 并运行，将原始输出与 macOS 的
`/private/tmp/w87-afp-arm-mac.out` 逐字节比较，`diff` 为空（0 行差异）。Linux 原始
输出由 orchestrator 归档为 `/private/tmp/w87-afp-arm-linux.out`。因此本文不是用
macOS 结果替代 Linux 门，而是两套真实 Arm 环境对同一 175 行原始输出互证；x86_64
Rosetta 路径继续作为 x86 指令真值。

首个 spike 只允许删 W37 的二元 Add/Sub/Mul/Div 与 SQRT guard。FMA、MIN/MAX、
COMIS、RCP/RSQRT 不纳入第一阶段；它们要么不是 W37 guard 来源，要么还有三输入
优先级、异常状态或 estimate precision 的独立合同。

## 1. 证据范围与方法

### 1.1 当前会话实际完成

- 只读审计 SwiftVM 当前 lowering、trampoline FPCR 边界、interpreter 与 MXCSR 路径。
- 精读本机 FEX clone，固定到上述 commit；没有用网页搜索结果替代源码。
- 临时 AArch64 guest：`/private/tmp/w87-afp-arm.c`；直接读写 FPCR 并输出结果位模式。
- 临时 x86_64 guest：`/private/tmp/w87-afp-x86.c`；直接读写 MXCSR，并通过 Rosetta
  执行 x86 指令作为同机参考。
- 临时扩展 `SVM_RA_HOT_COALESCE`，增加 NaN op-family 与 MXCSR-write 计数；跑完七
  语料后已完整撤回，patch 留在 `/private/tmp/w87-temp-probe.patch`。
- orchestrator 在 Orb ubuntu/aarch64 上用 `gcc -O2` 重跑同一 Arm guest；Linux 与
  macOS 原始输出逐字节相同，`diff` 0 行。
- 最终工作树不保留探针、guest 或 benchmark 产物。

### 1.2 计数口径

W71 的 `nan_guard_dynamic` 是每个 block 精确入口次数乘该 block 最终发出的 W37
guard 指令数：scalar 2、packed-f32 4、packed-f64 5。分母 `host_dynamic` 同样是
entry-weighted 主线 host 指令数，不是 PMU retired instructions。临时扩展把同一计数
按 IR opcode 分成 Add/Sub/Mul/Div/Sqrt；没有把 cold stub、FMA inline repair 或
MIN/MAX 算进去。因此百分比是“若 AFP 允许整类 guard 消失，最多可少发多少主线指令”
的静态×动态机械上限。

## 2. FEX 机制取证

### 2.1 feature 检测与 FPCR 生命周期

| 机制 | FEX 源码位置 | 结论 |
|---|---|---|
| 检测 AFP | `Source/Common/HostFeatures.cpp:294,491-502`，`FetchHostFeatures` | 从 `ID_AA64MMFR1_EL1.AFP` 进入 `HostFeatures.SupportsAFP`；RPRES 独立检测。 |
| 进入 JIT | `FEXCore/.../ArchHelpers/Arm64Emitter.cpp:637-660`，`FillSpecialRegs` | AFP 存在时设置 `FPCR.NEP[2] | AH[1]`；若 guest MXCSR.DAZ=1，再把 bit 6 写入 `FIZ[0]`。 |
| 离开 JIT/进 host | 同文件 `SpillStaticRegs:681-699` | 清 `NEP/AH/FIZ`，避免改变 native/C++ 浮点环境。 |
| 动态 MXCSR | `FEXCore/.../JIT/MiscOps.cpp:120-148`，`SetRoundingMode` | guest RC 写 `RMode[23:22]`，FTZ 写 `FZ[24]`，AFP 下 DAZ 写 `FIZ[0]`。 |

FEX 不是只借 AFP 做 lane preserve。它在 JIT guest 区间同时打开 `AH`，普通
`VFAdd/VFSub/VFMul/VFDiv/VFSqrt` 直接发 host FP 指令，无 NaN guard；`AH` 使这些
指令的 NaN 选择和 invalid default NaN 恰好落到 x86 规则。

### 2.2 覆盖族与 fallback

| 指令族 | FEX AFP 路径 | 无 AFP fallback | 精度判断 |
|---|---|---|---|
| Add/Sub/Mul | `VectorOps.cpp:248-258` 宏直接 `fadd/fsub/fmul` | 仍直接 host FP | AFP host 上位级精确；无 AFP 路径不重建 x86 payload，合同较 SwiftVM 宽松。 |
| Div | `VectorOps.cpp:1335-1388` 直接 `fdiv` | 仍直接 host FP | 同上。 |
| scalar lane 保留 | `VFScalarOperation:296-439` | temp 计算 + `ins` | AFP 依赖 `NEP` 直接写 tied destination。 |
| Sqrt | `VFSqrt`/`VFSqrtScalarInsert` 直接 `fsqrt` | 直接 host FP，必要时 temp+insert | AFP host 上 AH 处理 NaN/invalid；无 AFP 不做 SwiftVM 式精确修复。 |
| MIN/MAX | `VFMin/VFMax:1391-1520` 与 scalar 版本 `442-493` | `fcmp/fcsel` 或 compare+`bif/bit` | AFP 下明确注释“AH lets fmin/fmax behave like x86”；无 AFP 有精确选择 fallback。 |
| FMA | `VFScalarFMAOperation:265-290` 等 | temp+insert | 源码主要以 NEP/tied 为条件；三输入 NaN 次序未见软件重建。不能照抄为 SwiftVM 首期证明。 |
| RCP/RSQRT | AH 与 RPRES 共同影响 estimate 语义/精度 | 依 host/软件序列 | `SpillStaticRegs` 注释明确 AH+RPRES 把 estimate mantissa 从 8-bit 改到 12-bit；需独立 audit。 |

所以答案是：**FEX 确实用 AFP 免除软件 NaN guard；但 FEX 在无 AFP 的普通二元算术
上并不提供与 SwiftVM W37 同等级的 payload 精确 fallback。**我们只能移植 AFP
硬件事实，不能照抄 FEX 的非 AFP 行为。SwiftVM 无 AFP 或 proof 失败时必须保留 W37。

## 3. Arm 语义逐位映射

章节引用以 Arm ARM DDI0487 的以下章节为合同：A1.5 “Floating-point support”、
A1.5.4 “Flushing denormalized numbers to zero”、A1.5.5 “NaN handling and the
Default NaN”，以及每条指令调用的共享伪代码 `FPProcessNaNs`/`FPProcessNaNs3`。
[Arm A-profile Architecture Reference Manual](https://developer.arm.com/documentation/ddi0487/latest)
与 [A64 instruction descriptions](https://developer.arm.com/documentation/ddi0602/latest)
是本表的规范入口。Linux 公布 AFP 的 ABI 位为 `AT_HWCAP2` bit 20，见
[Linux arm64 UAPI hwcap.h](https://github.com/torvalds/linux/blob/master/arch/arm64/include/uapi/asm/hwcap.h)。

| FPCR 位 | AFP/Arm 行为 | x86 MXCSR 对应 | 本项目结论 |
|---|---|---|---|
| `AH[1]` | Alternative Handling：多源 NaN 按寄存器操作数次序传播，不再让 SNaN 抢优先；invalid 无 NaN 输入时产生 sign=1 的 default NaN；MIN/MAX 改成第二操作数选择语义；也改变部分 estimate/异常细节。 | x86 二元 SSE 的 operand-1 NaN 优先、SNaN quiet、negative indefinite；MIN/MAX unordered 选 operand 2。 | W37 五族的核心位；必须开。 |
| `NEP[2]` | scalar Advanced SIMD 只改 lane 0，不清/污染其余 lane。 | legacy scalar SSE 保留 destination 上层 lanes。 | 与当前 W23 一致；必须开，但它本身不决定 NaN。 |
| `FIZ[0]` | input denormal flush to signed zero，与 output flush 分离。 | `MXCSR.DAZ[6]`。默认 0。 | 动态映射 DAZ，不能常开。 |
| `FZ[24]` | AH=1 时只控制输出 denormal flush；FIZ 单独控制输入。 | `MXCSR.FTZ[15]`。默认 0。 | 动态映射 FTZ，不能常开。 |
| `DN[25]` | 强制生成 default NaN，覆盖 payload 传播选择。 | x86 无等价开关；默认必须保留 payload。 | guest 区间必须清 0，不能继承 host FPCR。 |
| `RMode[23:22]` | IEEE rounding mode。 | `MXCSR.RC[14:13]`，编码次序需转换。 | spike 同 FEX 映射；W37 guard 删除本身不应改变 RC。 |
| exception enable/status | FPCR trap enable 与 FPSR sticky status。AH 会改变部分异常处理细节。 | MXCSR mask/status。 | SwiftVM 当前未完整建模 SSE sticky/trap；AFP spike 不得宣称修复它，也不得扩大既有合同。 |

Arm 官方 feature 列表把 FEAT_AFP 定义为把 input/output flush 分开、增加 alternative
NaN/default-NaN 行为、NEP 与异常相关控制。最新 Arm 指令文档也会对不参与 AH 的
指令明确写“as if FPCR.AH is 0”，说明 AH 是逐指令规范语义而非实现偶然。

## 4. 指令族语义筛选

| Arm 指令 | scalar/packed × f32/f64 | 与 x86 的位级结果 | 是否能删 W37 guard |
|---|---|---|---|
| FADD/FSUB/FMUL/FDIV | 全覆盖 | `AH=1, DN=0` 下 Q/S 所有排列、payload/sign、invalid indefinite 全匹配。 | **是，首期 allowlist。** |
| FSQRT | 全覆盖 | SNaN quiet+payload；负有限数输出 `ffc00000`/`fff8000000000000`，匹配 x86。 | **是，首期 allowlist。** |
| FMIN/FMAX | packed f32/f64 实测 | NaN 时第二 operand 原位返回，连 SNaN raw bits 也不 quiet，匹配 SSE MIN/MAX。 | 语义匹配，但当前不是 W37 guard；另案。 |
| FCMP/FCMPE | f32 定向 | unordered 条件一致；q+s 时 Arm NZCV=`3`，x86 LAHF=`47`；两边 invalid sticky 均置位。 | 无 W37 guard；不计收益。异常状态仍属既有缺口。 |
| FRECPE/FRSQRTE | f32/f64 NaN 定向 | NaN payload/quieting样本匹配；正常数 estimate precision 未证明等同 x86 RCP/RSQRT。 | **否。**无 W37 guard，且需 RPRES/误差合同 audit。 |
| FMA/FMS | 本轮未做完整三输入排列 | AH 的 `FPProcessNaNs3` 次序可能可用，但 SwiftVM 当前按 a,b,c 重建，且有 `Inf*0 + NaN` 特例。 | **否，首期保留现有 inline correction。** |

## 5. 定向 guest 原始输出

### 5.1 环境与比较规则

Arm 输出来自 macOS 实机，并由 Orb ubuntu/aarch64 独立复现：

```text
Darwin ... RELEASE_ARM64_T6031 arm64
mode: FPCR.AH=1, NEP=1, DN=0, FIZ=0, FZ=0
Orb: gcc -O2; uname -m=aarch64; Linux output vs macOS output: 0 differing lines
```

x86 输出来自同机 `arch -x86_64`，`MXCSR=0x00001f80`。`q/s/n` 分别为 quiet NaN、
signalling NaN、normal；第一/第二字符是 operand 1/2。探针使用不同 sign/payload：
f32 `q1=7fc01234, s1=7f801111, q2=ffc05678, s2=ff802222`；f64 对应
`7ff8...1234, 7ff0...1111, fff8...5678, fff0...2222`。

以下 64 项是 Arm `AH+NEP` 的原始输出；与 x86 BASE 对应文件逐行 `diff -u` 无输出：

```text
c32-add qq=7fc01234 qs=7fc01234 sq=7fc01111 ss=7fc01111 qn=7fc01234 nq=ffc05678 sn=7fc01111 ns=ffc02222
c32-sub qq=7fc01234 qs=7fc01234 sq=7fc01111 ss=7fc01111 qn=7fc01234 nq=ffc05678 sn=7fc01111 ns=ffc02222
c32-mul qq=7fc01234 qs=7fc01234 sq=7fc01111 ss=7fc01111 qn=7fc01234 nq=ffc05678 sn=7fc01111 ns=ffc02222
c32-div qq=7fc01234 qs=7fc01234 sq=7fc01111 ss=7fc01111 qn=7fc01234 nq=ffc05678 sn=7fc01111 ns=ffc02222
c64-add qq=7ff8000000001234 qs=7ff8000000001234 sq=7ff8000000001111 ss=7ff8000000001111 qn=7ff8000000001234 nq=fff8000000005678 sn=7ff8000000001111 ns=fff8000000002222
c64-sub qq=7ff8000000001234 qs=7ff8000000001234 sq=7ff8000000001111 ss=7ff8000000001111 qn=7ff8000000001234 nq=fff8000000005678 sn=7ff8000000001111 ns=fff8000000002222
c64-mul qq=7ff8000000001234 qs=7ff8000000001234 sq=7ff8000000001111 ss=7ff8000000001111 qn=7ff8000000001234 nq=fff8000000005678 sn=7ff8000000001111 ns=fff8000000002222
c64-div qq=7ff8000000001234 qs=7ff8000000001234 sq=7ff8000000001111 ss=7ff8000000001111 qn=7ff8000000001234 nq=fff8000000005678 sn=7ff8000000001111 ns=fff8000000002222
```

这里直接证明三件事：operand 1 优先；SNaN 只置 quiet bit且保留 sign/payload；没有
NaN 输入的 invalid operation 使用负 indefinite。

### 5.2 packed、invalid、SQRT、MIN/MAX 与 compare 原始输出

Arm `AH+NEP` 与 x86 BASE 的共同结果：

```text
b32 invalid=ffc00000,ffc00000,ffc00000,ffc00000
b64 invalid=fff8000000000000,fff8000000000000,fff8000000000000,fff8000000000000
p32-add=7fc01234,7fc01111,ffc00000,7f800000
p64-add=7ff8000000001234,7ff8000000001111
p32-sub=7fc01234,7fc01111,7f800000,ff800000
p32-mul=7fc01234,7fc01111,ff800000,ffc00000
p32-div=7fc01234,7fc01111,ffc00000,00000000
p32-min=ff802222,ffc05678,ff800000,00000000
p32-max=ff802222,ffc05678,7f800000,7f800000
unary32 sqrt-s=7fc01111 recpe-q=7fc01234 recpe-s=7fc01111 rsqrt-s=7fc01111
unary64 sqrt-s=7ff8000000001111
scalar-upper=40400000,11223344,55667788,99aabbcc
```

compare 输出因 flags 格式不同而分别记录：

```text
Arm: fcmp(q1,s2)=nzcv:3 fpsr:00000001
Arm: fcmpe(q1,s2)=nzcv:3 fpsr:00000001
x86: ucomiss(q1,s2)=lahf:47 mxcsr:00001f81
x86: comiss(q1,s2)=lahf:47 mxcsr:00001f81
```

### 5.3 AH 是必要条件

相同输入在 Arm baseline（AH=0）与 AH=1 的差异：

```text
AH=0: add(q1,s2)=ffc02222   invalid=7fc00000
AH=1: add(q1,s2)=7fc01234   invalid=ffc00000
x86 : add(q1,s2)=7fc01234   invalid=ffc00000
```

所以不能只开 NEP；当前 SwiftVM trampoline 刻意保持 AH clear 的 W23 形态不足以删 W37。

### 5.4 DAZ/FTZ 原始输出

```text
Arm AH           : denorm32 in=00000001 out=00400000 denorm64 in=0000000000000001 out=0008000000000000
x86 BASE         : denorm32 in=00000001 out=00400000 denorm64 in=0000000000000001 out=0008000000000000
Arm AH+FIZ       : denorm32 in=00000000 out=00400000 denorm64 in=0000000000000000 out=0008000000000000
x86 DAZ          : denorm32 in=00000000 out=00400000 denorm64 in=0000000000000000 out=0008000000000000
Arm AH+FZ        : denorm32 in=00000000 out=00000000 denorm64 in=0000000000000000 out=0000000000000000
x86 FTZ          : denorm32 in=00000000 out=00000000 denorm64 in=0000000000000000 out=0000000000000000
Arm AH+FIZ+FZ    : denorm32 in=00000000 out=00000000 denorm64 in=0000000000000000 out=0000000000000000
x86 DAZ+FTZ      : denorm32 in=00000000 out=00000000 denorm64 in=0000000000000000 out=0000000000000000
```

`in` 本身也是一次 FP 运算的结果，所以 FZ 模式下打印为 0；这不表示 FZ 被误当成
input flush。FIZ 与 FZ 分开切换的四格结果仍逐格对应 DAZ 与 FTZ。

完整原始输出保存在 `/private/tmp/w87-afp-arm-mac.out`、orchestrator 归档的
`/private/tmp/w87-afp-arm-linux.out` 与 `/private/tmp/w87-afp-x86-mac.out`；临时
guest 源保留在 `/private/tmp/w87-afp-*.c`。Linux/macOS 两份 Arm 输出均为 175 行，
逐字节 diff 为空。

## 6. SwiftVM 当前合同盘点

### 6.1 W37 guard 与 cold repair

`source/runtime/backend/arm64/jit/translator_alu.cpp:1612-1777`：

| 形态 | hot guard | 每 site host 指令 |
|---|---|---:|
| scalar f32/f64 | `FCMP result,result; B.VS slow` | 2 |
| packed f32 | `FCMEQ; UMINV; FMOV; CBZ slow` | 4 |
| packed f64 | `FCMEQ; UMOV; UMOV; AND; CBZ slow` | 5 |

覆盖 `VecFAdd/Sub/Mul/Div` 与 `VecFUnary(kind=sqrt)` 的 scalar/packed f32/f64。
guard 检测 result 是否 NaN；对四种二元运算，这等价于“任一输入 NaN或运算本身
invalid”，对 SQRT 等价于“输入 NaN或负有限输入”。cold handler 再完成：

- operand 1 NaN 优先；否则 operand 2；
- SNaN 原 sign/payload 上置 quiet bit；
- 无 NaN 输入的 invalid 生成 `0xffc00000` / `0xfff8000000000000`；
- packed 逐 lane 修，scalar 只替换 lane 0并保留上层 lane。

`SVM_SSE_NAN_FAST=1` 是显式宽松模式，优先跳过精确修复；AFP spike 必须保持它与
`SVM_SSE_NAN_COLDPATH` 的既有开关语义，不能借新开关暗改旧组合。

### 6.2 FMA 与非 W37 路径

`translator_alu.cpp:3319-3415` 的 FMA/FMS 使用始终在 hot path 的三输入 vector
修正，按 a,b,c 优先级并处理 `Inf*0 + NaN`。W71 `nan_guard_dynamic` 不含它。
MIN/MAX、COMIS、rounding、RCP/RSQRT 也走独立 lowering。首期若把“AFP host”当作
全局免修证明，会把未审计族误删，必须采用逐 opcode allowlist。

### 6.3 interpreter

`source/runtime/backend/interp/interpreter.cpp:1620-1690,2428-2465` 先用 C++ host FP
算数值，再按 raw bits 显式实现 operand-1 priority、quiet bit 与 negative indefinite。
因此 interpreter 的 NaN bits 不依赖 host 自然传播规则；但 C++ 运算本身仍会观察
当前线程 FPCR 的 rounding/flush 环境。

正确边界不是让 AH 泄漏给 interpreter，而是继续沿用 trampoline 现有做法：进入
interpreter/C++/host helper 前恢复 caller FPCR，返回 JIT 后重建 guest FPCR。这样
interpreter 保持既有 bit normalization，翻译器/runtime 自身的 C++ FP 也不受污染。

### 6.4 当前 trampoline 与 Linux feature 缺口

`source/runtime/backend/arm64/trampolines.cpp:190-247,327-402` 已有一半基础设施：

- AFP 被 Config 打开时，保存原 FPCR 到 host stack；
- guest 区间只 OR `NEP[2]`，明确故意不设 AH；
- interpreter、CallHost 与返回 C++ 前恢复原 FPCR；回来后重开 NEP。

但不能原样扩展为 `OR AH`：它从 caller FPCR 起步，会继承 host `DN/FZ/RMode`；AFP
精确模式必须从保存值中清除 guest-owned fields，再由 `ThreadContext64::mxcsr`
构造 AH/NEP/FIZ/FZ/RMode。

另外，`source/translator/x86/translator.cpp:265-311` 的 Linux feature probe 当前只读
FlagM/FlagM2，**没有把 `getauxval(AT_HWCAP2) & HWCAP2_AFP` 写进
`Arm64Features::AFP`**。即使 Orb `/proc/cpuinfo` advertise `afp`，现有 Linux Config
仍看不到它。实现 spike 的 P0 必须先补 bit 20 检测；旧内核头无宏时按 Linux UAPI
定义 `1UL << 20`。`ComputeConfigHash` 已在 `code_serial.cpp:710-749` 哈希
`config.arm64_features`，所有 `SVM_*` 原值也进入 env hash，因此 feature 与新开关会
自然隔离 JIT/AOT cache key，无需另造 cache version。

## 7. MXCSR 动态交互

### 7.1 当前行为

`ThreadContext64::mxcsr` 默认 `0x1f80`。`LDMXCSR` 与 `FXRSTOR` 在
`decoder_sse.cc:1397-1447` 只 StoreUniform；XRSTOR helper 也只更新 context。
普通 SSE arithmetic 当前没有同步 FPCR.FIZ/FZ/RMode，项目注释亦承认 DAZ/FTZ
尚未全局建模。AFP 不能靠继承 host FPCR 扩大这个缺口。

### 7.2 建议的精确映射

每个 guest thread 的 FPCR 是线程局部架构状态；不需要进程全局锁。建议：

1. runtime entry 保存完整 host FPCR。
2. 构造 guest FPCR：强制 `AH=1, NEP=1, DN=0`；`FIZ=MXCSR.DAZ`，
   `FZ=MXCSR.FTZ`，`RMode=decode(MXCSR.RC)`；其余 host trap/status 控制保持已定义的
   runtime 合同，不盲目继承。
3. LDMXCSR/FXRSTOR/XRSTOR 后发一个明确的 FP-mode sync IR，或保守地终止 unit 返回
   dispatcher 重装 FPCR。首期优先“终止并重装”：它把 fault/partial restore 顺序交给
   现有边界，且本轮未发现热 MXCSR 写。
4. interpreter、host helper、signal/fault recovery进入 C++ 前恢复 host FPCR；返回
   guest 时从**当前** context MXCSR 重建，而不是从 saved host FPCR 仅 OR 三个位。

如果 RC/exception mode 的全合同本轮无法一次补齐，允许首期采用“默认 MXCSR fast
mode”：entry 比较 control bits，默认时用 AFP code；任何 LDMXCSR/restore 把 thread
标成 fallback 并退出，后续保留 W37 guard。不能只在翻译期假设默认，因为同一 unit
可在 MXCSR 改写后再次进入。

### 7.3 MXCSR 写频率证据

临时探针在最终 IR 的 `StoreUniform(mxcsr)` 处按 block entry 加权。本轮 coremark、
STREAM、smallpt、sqlite、c-ray、OpenSSL 均为 **0**；7zip 输出来自扩展计数格式前的
一次运行，没有该字段，记为“未采集”，不填 0。故“写时退出/重装”的边界成本在六个
已测语料为零，但不能外推到游戏、多媒体或主动切 DAZ/FTZ 的应用。

## 8. 七语料动态账目

以下是本轮 macOS 临时扩展 W71 的原始总表；Orb W71 历史值单列，不混入本轮实测。

| 语料/参数 | host dynamic | guard dynamic | guard % | Add | Sub | Mul | Div | Sqrt | MXCSR writes |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| coremark 1k | 2,344,765,645 | 10 | 0.000000% | 0 | 0 | 0 | 10 | 0 | 0 |
| STREAM full | 68,951,540,979 | 8,640,001,770 | **12.530542%** | 4,340,000,772 | 300,000,338 | 4,000,000,630 | 30 | 0 | 0 |
| smallpt `4 64 48` | 725,642,953 | 76,508,159 | **10.543499%** | 20,616,374 | 16,632,189 | 35,832,642 | 787,994 | 2,638,960 | 0 |
| sqlite `--size 1 --testset main` | 835,509,742 | 660 | 0.000079% | 0 | 0 | 0 | 660 | 0 | 0 |
| c-ray `-j1 -s8 -d160x120` | 26,953,821,561 | 1,766,798,895 | **6.554911%** | 366,469,114 | 437,416,038 | 900,675,357 | 52,685,696 | 9,552,690 | 0 |
| 7zip `b -mmt1 -md=4m` | 184,234,243,088 | 76 | 0.000000% | 0 | 0 | 38 | 38 | 0 | 未采集 |
| OpenSSL sha256 2s | 141,266,292,041 | 96 | 0.000000% | 0 | 24 | 36 | 36 | 0 | 0 |

按分母折算的主要族：

| 语料 | Add | Sub | Mul | Div | Sqrt | 合计 |
|---|---:|---:|---:|---:|---:|---:|
| STREAM | 6.294277% | 0.435089% | 5.801177% | ~0 | 0 | 12.530542% |
| smallpt | 2.841118% | 2.292062% | 4.938054% | 0.108593% | 0.363672% | 10.543499% |
| c-ray | 1.359618% | 1.622835% | 3.341550% | 0.195467% | 0.035441% | 6.554911% |

归档 W71 Orb Linux c-ray `-s8 160x120`：`host_dynamic=26,827,135,647`，
`nan_guard_dynamic/host=6.585859%`；STREAM Triad `0x401ba0` 热窗口的已知值为
`nan_static=10, nan_dynamic=17.86%`，Scale/Add 各 `nan_static=5`。本轮 c-ray 与
归档 Orb 只差 0.030948 个百分点，说明 NaN 面积结论不是 mac 独有；本轮当前 raw
matrix 又由 orchestrator 在 Orb 重跑并与 macOS 得到逐字节零 diff，平台语义门闭环。

oracle：smallpt PPM MD5 `8e22dc7bffca6dae5ad8964a29f8c030`；c-ray PNG MD5
`fe0fce0b5dfd4d2b3343e4dada134611`。CoreMark 1k 的四个 workload CRC 为
`e9f5/e714/1fd7/8e3a`；1k 不是标准 validation iteration，故不把 final CRC 写成
150k 的 `25b5`，也不声称 validated。

## 9. 边界成本净账

### 9.1 可删上限

机械上限推导链为：

```text
每 block W37 guard 静态条数
× block 精确 entry 次数
= guard_dynamic
guard_dynamic / Σ(block host_static × entries)
= 可删主线 host 指令上限
```

所以 STREAM/smallpt/c-ray 的上限分别是 12.53%/10.54%/6.55%。这只能批准 spike，
不能转换成同百分比 wall speedup；guard 包含 branch/compare，实际 cycle 权重取决于
前端、分支预测、发射与 FP latency overlap。

### 9.2 新增成本

| 边界 | 成本 | 本轮证据/约束 |
|---|---|---|
| JIT runtime entry | 读 MXCSR + 构造/写 FPCR，固定每次 Run 一次；现有 NEP 已有 Mrs/Stp/Orr/Msr，AFP 增量可并入构造。 | 不能按 FP op 收费；需 A/B 单列 dispatch-heavy 语料。 |
| interpreter/host helper | 离 guest 前恢复 host FPCR、回来后从 context 重建。现有路径已有两次 Msr；增量是重建 FIZ/FZ/RMode。 | helper-heavy 7zip/OpenSSL 必须防回归。 |
| MXCSR write | sync IR 或 unit exit/re-entry。 | 六个已测语料动态为 0；7zip未知；不能宣称普遍为 0。 |
| signal/fault | OS 保存线程 FP state，但 C++ handler不得在 guest FPCR 下做 host FP。恢复入口必须统一走 host-FPCR restore。 | 属正确性硬门，未实测；实现期必须注入。 |
| cold code | W37 cold veneer与 metadata可不发，减少 code bytes；但 fallback translation仍需保留。 | on/off fingerprint 应只出现 allowlist site 的 guard/veneer差异。 |
| cache | AFP feature 已进 ConfigHash；新 env 自动进 EnvHash。 | 无额外 per-unit metadata。 |

净加速必须由至少 7 对交错 A/B 给出 95% CI；本文只批准试验，不承诺正收益。

## 10. 默认 OFF 实现 spike 方案

建议开关：`SVM_SSE_AFP_NAN=0/1`，默认 OFF。

### P0：feature 与 FPCR 边界，不删 guard

1. Linux `DetectArm64Features` 增加静态缓存的
   `getauxval(AT_HWCAP2) & HWCAP2_AFP`；头文件缺宏时按 UAPI `1UL<<20`。
2. trampoline 保存 host FPCR，按 context MXCSR 构造 guest FPCR；C++/interp/helper/
   return/fault 全边界恢复 host FPCR。
3. LDMXCSR、FXRSTOR、XRSTOR 后正确重装 guest FPCR；首期可保守 unit exit。
4. P0 开关 ON 但 guard 仍完整保留，先跑 bit matrix、signal/fault/helper、七语料 oracle。

### P1：逐 opcode 免 guard

仅在以下条件同时成立时，`EmitVecFloatNaNFixup`/SQRT site 不排 cold veneer：

- host `Arm64Features::AFP`；
- `SVM_SSE_AFP_NAN=1`；
- guest 区间已证明 `AH=1, NEP=1, DN=0`，且 MXCSR control 已同步；
- opcode 是 Add/Sub/Mul/Div/Sqrt，lane 是 f32/f64，形态为当前 W37 scalar/packed；
- `SVM_SSE_NAN_FAST` 与 `SVM_SSE_NAN_COLDPATH` 的既有优先级保持不变。

proof 任一点失败，逐 site 回退现有 W37 guard+cold handler；不做函数级“部分相信”。
FMA/MIN/MAX/COMIS/RCP/RSQRT 明确不匹配 allowlist。

### 验收矩阵

- Orb 原样运行本文 AArch64 guest；64 个二元组合 + packed/invalid/sqrt/minmax/compare/
  DAZ/FTZ 必须与 x86 raw hex 零 diff。
- JIT/interpreter × scalar/packed × 32/64 × 全 Q/S/normal/invalid 组合；AFP ON/OFF。
- MXCSR default/DAZ/FTZ/DAZ+FTZ/四种 RC；LDMXCSR/FXRSTOR/XRSTOR 后同 unit 首条 FP。
- helper、interpreter fallback、signal、fault 三种返回路径检查 host FPCR 不泄漏；两个
  guest threads 使用不同 MXCSR 交错执行，验证 thread-local。
- OFF fingerprint/host bytes vs master 零 diff；ON diff 只允许 allowlist guard与对应
  cold veneer/relocation减少；disk cache cold/warm 分 feature/env 隔离。
- 七语料 correctness；STREAM/smallpt/c-ray 至少 7 对交错 A/B，95% CI；7zip/OpenSSL/
  sqlite 防固定入口/host-call 成本反噬。任一正式语料 >1% 回归则保持 OFF。

## 11. 门裁定与数据不足

| 门 | 裁定 | 理由 |
|---|---|---|
| 位级语义 | **通过（真实 Arm + x86 同机参考）** | W37 五族覆盖的组合零 diff；DAZ/FTZ映射也匹配。 |
| 动态可删 ≥5% | **通过** | STREAM 12.53%、smallpt 10.54%、c-ray 6.55%；Orb W71 c-ray 6.59%互证。 |
| 默认 OFF spike 立项 | **批准** | 采用 P0/P1 分期与逐 opcode fallback，不把 FEX 宽松 fallback移植进来。 |
| 指定 Orb 当前复测 | **通过（orchestrator 执行）** | Orb ubuntu/aarch64，`gcc -O2` 编译同一 guest；Linux/macOS 原始输出 175 行逐字节一致，diff 0 行。 |

仍无法从本轮数据预估：

- AFP guard 删除对 wall time 的净收益与 95% CI；
- entry/helper FPCR 重建在 dispatch/helper-heavy workload 的真实成本；
- 7zip 的动态 MXCSR write 次数；
- FMA 三输入全部 NaN/invalid 排列、RCP/RSQRT 的 RPRES 精度合同；
- MXCSR sticky exception/trap 的完整 x86 合同（这是当前已有语义缺口，不由 AFP
  guard spike 顺带宣称解决）；

这些不足不否定 default-OFF spike，但阻止默认翻转和收益承诺。

## 12. 外部规范与源码链接

- [Arm A-profile Architecture Reference Manual (DDI0487)](https://developer.arm.com/documentation/ddi0487/latest)
- [Arm A64 instruction set descriptions (DDI0602)](https://developer.arm.com/documentation/ddi0602/latest)
- [Linux arm64 HWCAP UAPI：HWCAP2_AFP bit 20](https://github.com/torvalds/linux/blob/master/arch/arm64/include/uapi/asm/hwcap.h)
- [FEX upstream repository](https://github.com/FEX-Emu/FEX)
