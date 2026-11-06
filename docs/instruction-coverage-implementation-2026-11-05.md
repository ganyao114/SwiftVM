# 用户态指令覆盖扩展：实现与验收

后续 compile 优化与最新测量见 [2026-11-06 成本优化记录](compile-cost-optimization-2026-11-06.md)；本文保留指令扩展完成时的原始快照。

本轮范围是旧版已经实现的用户态覆盖，以及基础整数、SSE、x87 缺口；EVEX/AVX-512 独立推进。旧代码仅作为编码规则、数值算法和历史 golden 的参考，没有恢复旧 decoder、IR、pass、分配器或运行时 owner。

## 已实现的指令族

| 范围 | 主要内容 |
| --- | --- |
| 基础整数与控制 | CLC/STC/CMC/CLD/STD、LAHF/SAHF、PUSHF/POPF 的用户态算术 flags/DF、BSWAP/MOVBE、LZCNT、BT/BTS/BTR/BTC、SHLD/SHRD、RCL/RCR、ENTER、LOOP/JCXZ 系列 |
| 字符串与地址 | MOVS/LODS/STOS/CMPS/SCAS、REP/REPE/REPNE、DF、部分完成与重试、67 地址和计数回绕、FS/GS 在线性地址阶段叠加 |
| BMI/ADX | ANDN、BLS 系列、BZHI/BEXTR、SHLX/SHRX/SARX/RORX、MULX、PDEP/PEXT、ADCX/ADOX，32/64 位分别执行 |
| SSE 整数与数据处理 | 饱和加减、平均、min/max、pack、扩展、高位乘法、shuffle/table、extract/insert、水平运算、blend、dot、SAD、PTEST、MASKMOV、PMADDUBSW 等 |
| SSE 浮点 | scalar single、packed/scalar sqrt/min/max/reciprocal、整数和浮点转换、round、水平运算、完整比较谓词入口；FMA 保留单次融合舍入和操作数 NaN 顺序 |
| Crypto 与字符串比较 | AES 轮运算/IMC/keygen、PCLMULQDQ、SHA256MSG1/MSG2/RNDS2；PCMPISTRI/M、PCMPESTRI/M 及其 VEX.128 形式，含 signed32/64 显式长度 |
| AVX/AVX2 | 自有 VEX 解码；128/256 位三操作数、转换、广播、跨 128 位 permute、每 lane 可变移位、FMA 三种编号、masked memory、VSIB gather |
| x87 | ext80/integer/m32/m64/m80、算术与比较、FCMOV 八种、控制/环境/FSAVE/FRSTOR、BCD、余数/缩放/拆分、超越函数；保留原 helper 命令 0–15 的编号 |
| MMX | 与 x87 物理寄存器共享真实 payload/tag/TOP，整数运算和 MMX/XMM 交叉转换，EMMS，不增加另一份 MMX 状态 |
| 用户态系统操作 | TSC、FS/GS base、XGETBV、FXSAVE/FXRSTOR、XSAVE/XSAVEOPT/XSAVEC/XRSTOR、RDRAND/RDSEED、CMPXCHG8B/16B、访存 fence 与 cache hint |

这张表表示实现范围，不把一个 mnemonic 等同所有前缀、宽度、内存边界或异常组合都已经穷尽验证。源清点、合法编码准入、固定数值 golden、实际 native 执行分别保留证据。

## 源码职责

```text
source/runtime/frontend/x86/
  instruction.h                    唯一 64B 自有指令记录与寄存器标识
  opcodes.inc                      自有 mnemonic；vendor 和自有扩展共用词汇
  decode.cpp                       唯一 DecodeOne 入口与 parser 选择
  detail/
    decoded.h                      legacy vendor 临时记录的一次适配与宽度修正
    modrm.h                        有界字节游标、前缀和 ModRM/SIB 地址解析
    extended_decode.cpp            ADX/SHA/随机数/WAIT 等 legacy 扩展
    vex_decode.cpp                 VEX 字段合法性与操作数归一化
    vex_opcodes.inc                VEX opcode/form 描述，非 IR 或 pass
    decode_timing.h                仅对实际选中的 parser 计时
  semantics*.inc                   同一个 Semantics<Sink> 的职责分段
    semantics_bmi.inc              BMI/ADX
    semantics_crypto.inc           x86 crypto 操作数与中立 vector op
    semantics_sse42.inc            SSE4.2 字符串结果和 flags 提交
    semantics_avx.inc              两半 YMM、NDS/FMA、masked/gather
    semantics_x87.inc              x87 访存、helper、提交与 metadata 顺序
    semantics_mmx.inc              MMX/x87 别名与 64 位 lane 语义
    semantics_system.inc           状态图像、TSC/base、宽 CAS
    semantics_user.inc             XLAT、MOVNTI、随机数
  interpreter*.inc                 同一个 DirectSink 的纯数值实现

source/runtime/arch/x86/
  x87*.cpp / x87_detail.h           ext80 数值、转换、余数和 quad 超越函数
  vector_crypto.*                  无 guest state 的 crypto 数值实现
  sse42_string.*                   无 TLS 的字符串比较数值实现
  bit_operations.h                 PDEP/PEXT 纯整数算法
  save_state.*                    标准状态图像的导入/导出
  state.h                         唯一 GuestState

source/runtime/backend/arm64/
  emit_*.cpp                      单 Writer 下的 typed Handle 实现
  emit_crypto.cpp                 实际 HWCAP 路径或数值 fallback
  emit_sse42.cpp                  字符串 helper 与 flags 提交
  emit_bits.cpp                   PDEP/PEXT helper 调用
  vector_helper.*                 有两个实际消费者的固定 helper ABI
  encoding.h / externals/vixl/     无隐式 scratch/pool 的 exact encoding

source/runtime/execution/state_service.cpp
                                  状态图像实际 guest 访存、SMC 和恢复提交
source/runtime/platform/system_query.cpp
                                  宿主时钟与 entropy 服务，脱离数值 helper 闭包
source/runtime/externals/cephes/   quad 多项式与许可证；SoftFloat 提供 f128 算术
```

这些文件不增加 pass。IrSink 和 DirectSink 消费同一个已解码记录和 `Semantics<Sink>`；Direct 执行不创建 Program。后端通过 opcode-tag `Handle` 派发，不引入第二份机器码 writer。

## 存储与提交合同

- 解码记录仍为 **64B**。VEX 的有效 16/32 字节长度占用原 reserved 字节；map/pp/W/vvvv 不作为另一份持久记录传入中间流程。
- YMM 使用两个 V128。低半沿用 XMM Uniform，高半使用 `ymm_high` Uniform；不扩展 V256 Home、RA bank 或持久指令数组。VEX.128 清高半，legacy SSE 保留高半。
- `GuestState` 从阶段一的 576B 扩为 **864B**，保留原字段偏移，增加真实 x87 FIP/FDP/FOP 和 16 个 YMM 高半。`X87State` 仍为 **144B**。
- 普通 256 位 load 和 masked load 在所需读取成功后提交目的寄存器。gather 按架构允许的部分完成逐 lane 提交数据并清对应 mask，重试不重复已完成 lane。
- x87 10B store 先准备数值，完成实际 guest store 后再提交 FSW/TOP/tag；guest store 失败不 pop。BCD 复用既有 query scratch 的有效空位，不增加状态镜像。
- `X87Execute` 明确携带 command/raw/DP/metadata。返回 2 表示旧 pending 异常，返回 3 表示本次新抬起异常；只有后者的失败出口写本指令 FIP/FOP/可选 FDP。metadata bit60 表示 helper 成功即为本指令的提交点；准备/查询/等待不设置该位，store 的 Commit 只在 guest 写成功后设置。前端不再为同一提交重复构造 FIP/FOP 常量和三个 Uniform store。
- x87 数值 helper 的 effects 是 guest state 读写及 External/Control 边界；异常以返回状态进入 service 4，没有 guest-memory 访存。纯元数据提交只写 guest state，二者均不再用 Unknown 暗示写所有 flags 或产生虚假 fault span。实际 guest load/store 仍保留原 fault 合同。
- legacy fused x87 窗口、INIT/FNSAVE 的末次元数据使用一个 `X87CommitMetadata` 操作提交，PC 为明确的实际指令地址。ARM64 与 Direct 分别复用同一个元数据写入函数。Wait 只可能返回成功或旧 pending，沿用三参数调用，不生成本次新异常的元数据分支。
- FX/XSAVE 通过同一个运行时 guest-memory 服务执行，保留 SMC 路径。XRSTOR 在实际需要的读取和图像检查成功后提交状态；FXSAVE 保留软件拥有的 464–511 字节。当前 XCR0 图像范围为 x87/SSE/YMM（7）。XSAVEC 重写 XSTATE_BV/XCOMP_BV，不读取旧 header；本模型的唯一非 legacy 组件为 YMM 高半，compact 与 standard 的该组件偏移均为 576。XRSTOR 依据图像格式分别处理 MXCSR、组件位图和保留位，先完成读取和合法性检查再提交。
- AES/PMULL/SHA256 根据实际宿主 HWCAP 选择，软件路径执行同一个纯数值合同。默认与 pin ABI 分别明确使用 v30/v31 和 v8/v9 临时寄存器。

## 验收与性能证据

最终构建、源指纹、手机输入 SHA、测试原始输出、编码准入和 compile 样本存放于：

`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/instruction-expansion-20261105`

最终源码指纹：`57db153a47dd80a107839598abc2a2dfc0f9137481ebc53e77b05fbef5eb7a66`，覆盖 host 与 Android 实际编译依赖的 282 个文件。手机 runner SHA256：`4e440cceaffe199b1d42fbc38d34715d3b211c9c5ed6bd68115c763634ca0783`。构建为 NDK29、arm64-v8a、API28、Release/O3、ThinLTO；x87/Cephes 数值闭包按既有 GPR-only 合同单独编译。对应源码压缩包、保留 runner、完整原始样本和设备清理收据都在上述目录。

| 检查 | 最终结果 |
| --- | --- |
| 主机 CTest | 9/9，通过 |
| ASan + UBSan CTest | 9/9，通过 |
| 红魔新增 scalar/SSE、AVX、x87/MMX 测试 | 3/3，通过；AVX 固定检查 1584 项 |
| 红魔实际 ARM64 执行 | 默认 ABI、GPR/XMM pin ABI 均执行新增覆盖；后端旧检查 264935 项及 pipeline 4170 条路径通过 |
| 状态图像 integration guest | InterpretOnly / Block / Function 三种执行全部 exit0；含标准与 compacted 保存恢复、AVX-only、空 mask 和 MXCSR 边界 |
| 原 workload smoke | 7 个 C++ 检查和 8 个 workload × 4 个 SVM arm，共 39/39，通过 |
| 有限 VEX 编码准入 | 316 个描述符生成 2600 个候选；Capstone 接受的 2169 个编码全部通过 DecodeOne → BuildIR → ARM64 Compile，名称不匹配 0 |
| compile 对比 | 8 workload × 5 arm × 5 轮，200/200 样本输出和计时闭合检查通过 |

词汇清点为 901 个自有 mnemonic，均找到共享语义入口；前端使用的 neutral IR 未发现缺 ARM64 typed/structural 入口。该清点不是 901 个指令的全 form 或异常覆盖证明。Capstone 没接纳的编码也不被冒充为该项准入成功；另有固定 golden 测试覆盖 VEX.W 的显式字符串长度等路径。

### Compile 结果

使用红魔 NX729J，serial `bf1359ff`，CPU4，既有频率策略 614400–1171200 kHz。每个样本都是新 launcher、无 AOT/object cache；五个 arm 轮换顺序。仅扣除真正字节 parser 的计时，适配、形成、IR 构建、优化、分配、发射、发布及释放仍计入。表中为全部五轮的中位数，单位 ms，FEX 倍率为各自中位数之比；完整 samples/min/max 在 `compile-summary.json`。这些原 workload 对比未使用新 AVX workload，固定 FEX 环境仍使用原来的 disableavx 设置。

| workload | 扩展前 Block | 当前 Block | 扩展前 Function | 当前 Function | FEX | 当前 / FEX，B / F |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| coremark | 45.0984 | 46.3457 | 51.8928 | 51.7937 | 47.5871 | 0.97 / 1.09 |
| int | 1.9960 | 1.8987 | 1.8390 | 2.0751 | 1.5302 | 1.24 / 1.36 |
| fp | 2.4761 | 2.6208 | 2.4186 | 2.6038 | 1.7189 | 1.52 / 1.51 |
| mem | 2.1623 | 2.3702 | 2.1348 | 2.4376 | 1.6169 | 1.47 / 1.51 |
| branch | 1.8291 | 2.0979 | 2.0074 | 1.9233 | 1.4824 | 1.42 / 1.30 |
| call | 1.8821 | 2.0603 | 1.9237 | 2.2037 | 1.6318 | 1.26 / 1.35 |
| func_tests | 22.6858 | 23.3656 | 32.8648 | 33.7896 | 30.3566 | 0.77 / 1.11 |
| x87_bench | 0.5823 | 0.6880 | 0.4762 | 0.5571 | 0.2871 | 2.40 / 1.94 |

CoreMark 相对扩展前为 Block +2.8%、Function −0.2%；func_tests 两种 scope 均约 +3%。小负载的变化约 −4.9% 到 +14.7%，不能据此声称 compile 成本完全没有增加。

**此前“所有负载最多慢 FEX 两倍”的目标仍未全部达到：x87 Block 为 2.40×，Function 为 1.94×。** x87 相对扩展前的中位数分别增加约 18.2% 和 17.0%，分别约 0.106 ms、0.081 ms。它必须继续作为 compile 优化的未关闭项，不能用大 workload 的成绩覆盖。

x87 的五个 Block 样本为 0.7059 / 0.6050 / 0.7214 / 0.6880 / 0.5901 ms，FEX 为 0.2951 / 0.2794 / 0.3129 / 0.2871 / 0.2816 ms。此次只保留固定五轮结果，没有用后续重跑替换慢样本。

### 已做的成本收敛与生成质量边界

- x87 metadata 从逐字段常量/store 收成实际提交；单条 FLD1/FXCH 由 helper 成功边提交，确实融合的窗口仍提交最后一条的真实 PC。没有添加 metadata 分析 pass 或生命周期表。
- x87 helper 与 metadata 改用真实 effects。最终 x87 Block/Function 的 fault site 都为 1，扩展前分别为 17/12；这一个 site 对应真实 guest 访存。
- x87 布局参数只在构造期间借用，环境图像构造保持独立函数，不把低频图像逻辑展开进普通寄存器算术的构造体。
- 最终 x87 IR 为 Block 65 节点/182 operand words、Function 50/126；扩展前为 59/106、46/75。元数据和异常合同仍有实际构造成本。更早未收敛版本为 124/91 节点；其原始结果保留在历史目录，未充当最终版本成绩。
- 五个 int/fp/mem/branch/call workload 的 native bytes 均比扩展前少 32B。CoreMark Block 为 389876→389900B，Function 为 393248→394016B；Function fallback 5→0，同时 spill slots 31→33，不能宣称所有代码质量指标完全一致或全都改善。
- x87 native bytes 为 Block 2364→2532B、Function 1532→1732B，包含新增的正确 metadata/异常提交。新增语义正确性已验证；本轮没有新的完整 runtime/PMU 比较，不复用旧 PMU 来宣称运行性能完全不回退。

中途编译预算不达标的批次保留在 `before-metadata-fusion/`、`after-checked-metadata-fusion/`、`after-single-metadata-commit/`、`after-precise-x87-effects/`，各自 runner 压缩保存。此前中途 source manifest 只取 host 依赖，未涵盖 Android-only execution/launcher 源码；该指纹口径不完整，已在 `source-manifest-correction.json` 明确更正，最终压缩包和指纹使用 host/Android 依赖并集，产品源码与二进制未因该证据更正改变。

状态图像扩展 fixture 只把本版三种 SVM 执行方式列为验收，没有宣称通过 FEX 对该 fixture 的交叉验证。手机本轮独立 namespace 已停止自身 FEXServer、检查无活动进程或 open file 后清除；不操作其它会话。原 `/Volumes/Data2/SwiftVM` 保持只读参考，本轮工作位于 `/Volumes/Data2/SwiftVM-submit-20261104`。本文指纹对应提交整理前的测量快照，格式化验证另行记录。


## 明确边界

- EVEX/AVX-512、guest kernel/特权指令和兼容模式不在本轮范围。旧版忽略 CS 的 RETF 近似、64 位模式非法的 PUSHA/POPA，以及完整 guest trap/异常投递属于执行模式合同，不计为本轮已补齐的数值 lowering。
- 没有照搬旧分支的近似入口：旧 RDPKRU/WRPKRU 只维护寄存器、没有 pkey 访存保护；按本轮范围确认，完整 PKU 单列后续，当前运行时不宣告 PKU，也不把这两个入口计为已完成。XSAVEC 则实现本模型 x87/SSE/YMM 的真正 compacted header，不能沿用旧分支写标准图像且 XCOMP_BV=0 的近似。
- 保留保守 CPUID 模型；实现若干扩展指令不自动打开整套 feature bit，例如三条 SHA256 指令不代表已经实现 SHA1。
- POPF 的合同是用户态算术 flags/DF；完整 TF/AC/ID trap 和 guest FP 异常投递仍需运行时信号模型。本轮 x87 未屏蔽异常通过已有 service 4 边界返回。
- ENTER 已实现实际 frame 链和 nesting；最终 RSP 所指内存的无写入权限探测尚无对应平台服务，未宣称这一边界已实现。

指令细节依据 Intel SDM；XSAVEC 和两种 XRSTOR 图像处理见 [卷一 §13.8、§13.10](https://cdrdv2-public.intel.com/843827/253665-sdm-vol-1-dec-24.pdf)，状态图像的软件保留区见 [Intel 指令手册](https://cdrdv2-public.intel.com/868140/253666-089-sdm-vol-2a.pdf)。Cephes/SoftFloat 的来源、许可证和文件 SHA 保留在 `x87-coverage-20261105` 的迁移收据中。
