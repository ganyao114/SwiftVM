# Runtime 架构实施与验收记录

后续指令扩展、864B GuestState 和新的验收见 [用户态指令覆盖扩展](instruction-coverage-implementation-2026-11-05.md)。本文的测量和源指纹对应扩展前的阶段一快照。

日期：2026-11-05。对应[设计方案](runtime-architecture-plan-2026-11-05.md)。主范围生产实现与测试已完成；正式性能门槛47/48通过，x87 Block compile略超严格2×FEX门槛，详见下文。

实施树：`/Volumes/Data2/SwiftVM-submit-20261104`，基于 `74ac9bc9b6bb160c2d7d10bf9a37000f5ec0407a` 的 WIP。冻结旧树 `/Volumes/Data2/SwiftVM` 保留原代码／WIP，仅只读引用。最终测量的186项源码清单 digest：`eb0503feacb8c784b7091ded3e573673a92d596c15596ec0f8a6a95f329f1329`。

## 1. 实际交付

| 设计边界 | 已落地实现与职责 |
| --- | --- |
| 一个 decoder、自有指令记录 | `frontend/x86/instruction.h` 持有64B自有记录及 Op/Register/operand/flow；`decode.cpp` 是唯一 parser/CET/错误适配入口；vendor 仅在 `detail/decoded.h` 与 decoder 中出现 |
| 独立 region 形成 | `region.*` 保留 sparse worklist、唯一 D buffer、16B block slices、预算和 ready cuts；CALL 默认退出，不加第二份 normalization/actions[N] |
| 共享指令语义 | `semantics.h` 与 operands/integer/control/vector/x87/dispatch 分组是唯一高层语义；IrSink 留在 `build_ir.cpp`，DirectSink 在 `interpreter.cpp`；IR 目标提示只由 IrSink 决定 |
| 前端可直接执行 | `InterpretOne` 直接对数值和 GuestState 执行，无 Program／NodeId 表／持久指令缓存；共享子字段、提交次序、REP、flags 与 x87 协议 |
| 中立 IR 与分配 | 保留一个 Program、24B Operation/BlockRecord、u64/N 优化工作表、Home/N 与实际 B schedule；Block 与 Function 分配继续独立，GPR/XMM Uniform 与 Known/EarlyOff/Unknown 继续保留 |
| 精确 ARM64 编码 | 受控 VIXL fork、唯一 borrowed words；无 MacroAssembler／UseScratch／默认 x16/x17/d31 池、literal/veneer 自动插码或第二码流；SVM 显式授临时寄存器与展开 |
| 后端派发与文件组织 | `compile.cpp` 只主编译／分配／重发；`emit.cpp` 一次薄派发到同一 Writer 的 Handle；operand/scalar/memory/atomic/control/state/flags/vector/float 分职责实现，无向量／状态试探二次派发 |
| 通用代码资源 | `backend/code.h` 表达 byte offsets 与真实 F/L/E；LinkKind 显式区分 static/dynamic；A64 patch 和 recovery payload 仅 backend/platform 解释 |
| VM／线程分工 | `execution/vm.h` 组合共享实际 owner，`guest_thread.h` 按值持当前线程状态；不恢复持久 Module→Function→Block／IR索引树 |
| 代码映射与目录 | `MemMap` 独占 native backing/views，`CodeHeap` 独占 slab/fault buffers，`CodeCache[4]` 独占 PC ready/pending、unit/entry/link membership，`code_link.cpp` 是同 owner 的方法 |
| 冷编译和 dispatcher | `CompileService` 直接协调形成→IR→opt→backend→发布；`dispatch.cpp` 冷解析／切换引擎；`HostBackendOps` 只做一次绑定的冷回调，不加每IR虚调用 |
| Linux 与 ARM64 bridge | loader、syscalls、signals、safe_access、arm64_runtime、静态 arm64_entry.S 按职责分开；执行公共头不依赖 A64 CompileOptions／寄存器布局 |
| 架构状态 | `arch/x86/state/flags/cpu/x87` 是唯一 guest 定义；syscall 使用同一 ToRflags，ext80 状态不再归 IR；GuestState576B、NativeRunContext160B、热slot32B保持 |
| SMC 生命周期 | `smc.*` 负责 watch、signal窄回调、四域撤目录／static links／local16、冷点退休；GuestMemory 保唯一映射／backing事实 |
| SourceMode | GuestMemory 单次分类 Immutable／WatchedBlock／Volatile；Other backing 不授持久 JIT，CodeAt 不暴露其 raw bytes，未经支持的直接 fetch 在读前拒绝 |

完整实际文件树及逐文件职责见方案第9节。旧 `execution/native_code.*`、guest_state/guest_cpu、execution/x87 与 ir/x87 定义已删除，没有保留转发兼容层或第二实现。

资源尺寸透明记录：Instruction64B、DecodedBlock16B、Operation24B、BlockRecord24B、Home16B；FaultSite32B、LinkSite24B、EntryPoint16B、NativeCode40B；真实 relocation 从24B增加为32B，用显式 kind 换掉旧 sentinel 协议。全部成本计入测量。

静态 Root/Cold/Exit 的8个实际符号由同一 ABI 常量生成；将汇编按职责收为命名宏后，`.text` 仍为3544B，SHA256 `0eecd3b4ef1712e537f6c3a8c685bdffcf4cc1233653680a655471dd437c9cd7`。没有为静态入口再建立启动 code heap。

## 2. 实际执行合同与新增测试

CLI 新增 `--engine jit|interpreter|mixed`；默认 JIT。InterpretOnly 不请求编译／native slab；Mixed 只对支持范围拒绝选择直接执行，从实际 GuestState.pc 继续，错误 Program 不作为 fallback。Function 失败后仍从请求 root 尝试 Block。

Direct memory 经实际服务处理逻辑权限、known EOF、SMC 通知、ordered access 和 atomic。Linux 使用 process_vm_readv/writev 安全返回失败；不以普通 C++ memcpy 加 native siglongjmp 跨 RAII 栈。aligned 匿名 atomic/CAS128 是当前资格，文件／设备／未对齐 atomic 在副作用前拒绝。数值 FP 通过平台 fenv guard 隔离调用者环境。

关键合同有独立行为测试：

- EAX 零高位、算术 flags、INC 保 CF、SETcc。
- CALL 栈写成功后才提交 RSP/PC；RET 读成功后才提交；POP 使用新 SP 计算 EA，但 fault 前不提交 SP。
- REP 零次数不访问；每次成功 store 后提交 RCX/RDI，fault 保留已完成前缀。
- 不支持的 segment MOV/POP 与 atomic 在 guest 访问前拒绝；x87 未屏蔽状态更新返回 service，不变成可重跑的 Unsupported。
- packed/scalar FP lane、guest RC、host fenv 恢复与默认NaN控制隔离；CET baseline 与启用未支持模型的区分。
- 完整短指令不因取15B时遇不可读尾页而 fault；真正缺 opcode/REX/immediate 尾字节时保留故障地址；非法 long-mode opcode 与15B prefix流保持 InvalidEncoding。
- 实际 VM 的 InterpretOnly、Mixed 后端拒绝恢复、InvalidProgram 不 fallback、映射所有者关闭。

diStorm 只增加 opt-in 的真实字节短缺报告，默认调用行为保留；adapter 不做补零重试或第二次 parser。原 parser 未暴露的 VEX-prefix 短缺事实不靠猜测补齐，不宣称新增 AVX 支持。

最终版本验证：

| 项目 | 结果 |
| --- | --- |
| macOS ARM64 Release／IPO | 构建成功；6/6合同通过；此处只做跨平台构造及编码测试，不宣称native Linux执行 |
| Debug ASan+UBSan | 构建成功；6/6合同通过，无 sanitizer 报告；Debug不启IPO |
| Android NDK29 arm64-v8a/android28 Release／ThinLTO | 构建成功；实际手机后端264,935项检查通过 |
| Android native pipeline | 12条base路径与4,170条整数／flags／Jcc路径通过 |
| Android Direct／engine | 定点合同通过；CoreMark20 CRC4983、功能集完整stdout与exit101通过；compile attempts/native bytes/slabs均为0 |
| JIT应用 smoke | 固定八项输入×前后两个runner×Block/Function，共32项结果校验通过 |
| FP domain／SMC | 四个前后runner/scope roundtrip通过；alias JIT Block/Function各3次native write fault、9次file-write invalidation，Direct alias亦通过 |
| known EOF | JIT／Direct均按当前合同拒绝（exit125），保留诊断，不发生host裸访问崩溃 |

手机内核以 EACCES 拒绝 `/dev/zero` RX mapping；测试明确报告这条 Volatile RX 动态分支不可用。它的准入源码、上层 before-access 拒绝与实际匿名／普通文件路径分别核对，不把主机拒绝当作已执行了该设备代码路径。

## 3. Compile 性能

最终正式测量为五臂、八输入、每臂五次，共200样本。仅扣真实 byte parser；适配、形成、IR、拒绝／回退、分配、编码、发布与native释放都计入。手机是红魔 NX729J `bf1359ff`，CPU4／taskset10，原频率min/max614400/1171200kHz保持。与固定FEX使用相同guest ELF与环境，不修改RWE权限。

CoreMark Block：重构前 **44.7964 ms** → 当前 **45.0604 ms**（**+0.59%**）；Function：**51.1011 ms → 51.4487 ms**（**+0.68%**）。功能集 Block **+0.89%**、Function **+0.84%**，大案例没有出现显著回退。短案例全样本／范围保留，不以补样本消除波动。

| 输入 | 旧Block ms | 当前Block ms | 旧Function ms | 当前Function ms | 固定FEX ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| CoreMark20 | 44.7964 | 45.0604 | 51.1011 | 51.4487 | 47.6496 |
| int | 1.7627 | 1.8980 | 1.7688 | 1.9082 | 1.4785 |
| fp | 2.3697 | 2.3922 | 2.5577 | 2.4424 | 1.7463 |
| mem | 2.1028 | 2.3008 | 2.1220 | 2.2325 | 1.5625 |
| branch | 1.7834 | 1.7648 | 1.8760 | 1.8242 | 1.4917 |
| call | 1.7407 | 1.8236 | 1.8687 | 1.8170 | 1.6101 |
| func_tests | 22.5736 | 22.7738 | 32.6825 | 32.9581 | 30.4778 |
| x87_bench | 0.5514 | 0.5734 | 0.4933 | 0.4824 | 0.2863 |

具体无舍入值以 `compile-summary.json` 为准。CoreMark当前B/F分别为FEX **0.946×／1.080×**。严格2× compile门槛有一条未通过：x87 Block **2.0031×**，超过约0.15%；其相对旧runner为+3.99%，是约0.57ms的短案例。本轮保留此失败，不以补跑、改门槛或混入历史样本宣布全部通过。

### 开销定位与最终构建配置

首轮机械拆分后，Core B/F增加约7%，功能集增加6–10%。采样及目标文件显示 adapter 每条记录有多次register/opcode转换调用，exact encoder及Handle也增加了宿主调用边界。先将热定义移入单份inl、收窄实际映射并内联共享dispatch入口；第二轮大案例仍增加7–8%，因此没有直接接受为新基线。

正式构建启用工具链支持的 Release IPO（NDK ThinLTO），允许跨职责文件消除宿主调用开销。byte parser、svm_x87及SoftFloat ext80闭包保持非IPO编译，保留general-register-only ABI；被裸汇编调用的SvmArm64WideDivide明确标记used/noinline。未添加运行时验证、第二encoder、unity源码或IR pass。

这项收益来自宿主构建优化，不能宣称是IR遍历下降。前两轮各200个compile样本及runner／source快照单独保留；单次IPO预检不进入最终统计。关闭 `SVM_ENABLE_IPO` 后的性能不继承本轮正式资格。

## 4. 生成质量与运行性能

最终五轮共200个runtime／PMU样本全部通过结果校验。CoreMark以下为guest loop秒数；其余为包含启动／JIT的whole-child秒数，不能跨输入合成总分。

| 输入 | 旧Block s | 当前Block s | 旧Function s | 当前Function s | FEX s | 当前B/F cycles÷FEX |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| coremark | 16.097000 | 16.096000 | 15.647000 | 15.645000 | 15.035000 | 1.0704／1.0404 |
| int | 0.301734 | 0.301631 | 0.301358 | 0.302450 | 0.315721 | 0.9918／0.9920 |
| fp | 0.250427 | 0.251584 | 0.247987 | 0.249991 | 0.262316 | 0.9850／0.9860 |
| mem | 2.147944 | 2.151391 | 2.157273 | 2.169060 | 2.056437 | 1.0523／1.0615 |
| branch | 0.257255 | 0.259570 | 0.259642 | 0.260644 | 0.268114 | 1.0031／1.0094 |
| call | 0.478089 | 0.480196 | 0.475587 | 0.479491 | 0.458200 | 1.0787／1.0734 |
| func_tests | 0.108200 | 0.110585 | 0.119984 | 0.121431 | 0.141167 | 0.8204／0.9789 |
| x87_bench | 0.553600 | 0.557743 | 0.554127 | 0.556637 | 0.857454 | 0.6340／0.6336 |

CoreMark Block **16.097 → 16.096 s**，Function **15.647 → 15.645 s**，与重构前持平；当前B/F时间为FEX **1.0706×／1.0406×**，user cycles为 **1.0704×／1.0404×**。其余输入相对旧runner的time中位数变化为约−0.03%至+2.20%，无显著运行性能退化。PMU指令数包含冷JIT；CoreMark当前B/F为FEX **1.0379×／1.0100×**，不得将此窗口全部写成guest动态指令。

**严格门槛合计47/48通过**：16项time、16项user cycles全部通过≤1.10×FEX；compile为15/16通过≤2×FEX，唯一未通过项是前述x87 Block 2.0031×。这不等同于完整48项通过，未追加样本消除边界失败。


CoreMark 前后IR工作量、native bytes、spill与frame保持：Function 50,591个IR节点、393,248 native bytes、31 spill slots、64B最大frame；没有通过减少guest工作或放松原ELF的SMC政策换取compile结果。功能集Block的基线本身有一条extra unit的轮间变化，候选落在其已观测工作量范围；详见quality-comparison.json，不声称每个原始计数逐轮完全相同。

## 5. 明确的范围与证据

当前资格仍是单GuestThread、受管理代码页／alias及既有服务写入口；Requested Function对可写源维持单source-block。可写多块guard、选择性失效、多guest线程／epoch、DSO卸载、完整guest signal delivery、外部进程／设备backing写入监视没有由本轮授予。x87基准沿用exit-only oracle，数值FP也不宣称新增完整MXCSR sticky/trap语义。

脚本、身份及精简结果见[证据入口](evidence/runtime-architecture-20261105/README.md)。完整原始文件、前两轮过程版本、最终runner、186项源码清单和压缩快照均在Data2保留目录；大型数据没有搬回系统盘。手机独占目录及本轮FEXServer已清理，分配空间回收114,619,392字节，df实测可用空间增加118,030,336字节；目录剩余0、活跃进程0、打开文件0、清理错误0。原始receipt及清理前后identity/ps/lsof/du/df保留。
