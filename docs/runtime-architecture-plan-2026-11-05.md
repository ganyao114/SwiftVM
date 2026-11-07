# Runtime 架构整改方案：decode、解释执行、ARM64 编码与代码生命周期

日期：2026-11-05。状态：主范围已实施并完成测试；正式性能门槛47/48通过（x87 Block compile 2.0031×FEX略超2×），详见[交付记录](runtime-architecture-implementation-2026-11-05.md)。后续能力项目不由本轮验收授予。

本方案以 `74ac9bc9b6bb160c2d7d10bf9a37000f5ec0407a` 为实施前快照。历史性能基线是[提交后五轮锚定](post-push-benchmark-2026-11-05.md)：CoreMark Function compile 扣实际 byte parser 为 **51.463 ms**，固定 FEX 为 **47.700 ms**；该历史批次八项、两种 scope 的48项门槛均通过。新旧runner的本轮成对结果独立记录，不能沿用历史资格。架构整理继续承担形成、IR出生、失败／回退、分配、编码、发布、失效和释放成本。

旧实现分三种证据看待：旧分支提交 `660b6ba1`、`origin/master` 的 `42e9c234`，以及 `/Volumes/Data2/SwiftVM` 中冻结的旧工作目录。旧目录含原 WIP；其中 decoder、interpreter、address_space 等文件不能冒称全部等于 `660b6ba1`。本文旧文件的绝对路径链接指冻结工作目录，明确引用提交版本时另写 hash。冻结树只读。

## 1. 关键问题的结论

| 用户关切 | 实施前判断 | 设计落点 |
| --- | --- | --- |
| decode、assembler 原始，编码散落 | 成立；byte parser 重复，vendor 类型泄漏，A64 位域出现在选码、发布、fault 修补中 | 一个 DecodeOne；基于定制VIXL的typed exact encoder；一个借用原words、无隐式scratch／自动插码的Assembler |
| 旧架构较清晰，当前耦合 | 主要耦合在 Process、公共头依赖、flags/ABI 和真实资源角色，不能仅按文件长度判断 | 以数据所有者、执行边界和 ISA 边界拆分；保持当前直接编译骨架 |
| Code cache 与旧 MemMap | 旧 MemMap 是 host backing/views，不是 guest mmap；有可复用原则，也有空实现及版本差异 | GuestMemory 继续独占 guest 映射；MemMap 管 native backing/views；CodeHeap 管 slab 发布；CodeCache 管 unit/entry/link membership |
| dispatcher、block link、Function 编译 | 机制已有，但术语、接口和 sentinel 把不同概念混在一起 | 分清 source block、region、分配 scope、native unit、callable entry；区分内部边、静态链接和动态查找 |
| SMC 不清晰／未实现 | 已有受限实现；单注册线程、受管理源页／alias 和现有服务写入口有合同，通用并发与 guest 信号不完整 | 将检测、撤销、退休归为显式服务；先保留现有资格，再单独推进可写多块、并发和 guest 信号 |
| 前端兼作解释器 | 当前无解释器，旧解释器解释旧 IR，不能直接搬来执行 decoded 指令 | 最终共享 decode 和指令语义，静态实例化 IrSink、DirectSink；直接解释不必构造 Program |
| 旧 Runtime／Module／Function／Block 管理是否复用 | 共享地址空间与线程上下文分离、IR／native 分寿命等原则合理；旧 Module 又混合了多种职责，不能视为完整模块卸载模型 | VM／GuestThread 分清；保留 LoadedImage 的模块身份与冷策略职责；region／Program 短命，NativeUnit／Entry 独立管理 |

目标是让职责和实际机制对应。不会重新增加 MIR、actions、certificates、第二份指令计划或每条指令的虚调用。

补充的实现约束：源码按职责拆分，同一个实际 owner 可以跨多个 `.cpp` 实现，不为拆文件另建 manager 或流程层。Backend IR 使用命名 `Handle` 方法派发；case 只负责转交，指令实现归所属职责文件。只保留真实边界和后端支持范围所需的检查，不沿每层重复验证内部事实。

## 2. 当前实现中需要保留与需要修正的部分

### 2.1 保留的骨架

- 一份实际 decoded buffer；Function 的 block 是 16B slice，形成在 IR 出生前完成。
- 一个中立 Program，GPR/XMM 继续通过 Uniform 表达；Block 与 Function 分配路径独立。
- 通用优化的三次 N 遍历及一个 u64/N 工作表；后端一份 Home/N 和实际 B schedule。
- 一份 native words 输出，附真实 fault、link、entry 资源；直接选择并编码。
- 同一 native unit 的多个外部入口共享原范围与 fault base；不复制 unit，不把 body label 冒充完整入口。
- 已有容量复用、双映射 W^X、四个 FP specialization domain、pending 链和冷点退休。

### 2.2 实施前证据与实际替代位置

本节的问题指 `74ac9bc9` 的实施前快照；旧源码可用 `git show 74ac9bc9:<path>` 查看，已退役文件不再链接到当前不存在的路径。

| 实施前耦合 | 当前正式实现 |
| --- | --- |
| decode/build_ir 公开 `_DInst`，Block/Function 重复 parser 与 CET 修补 | [instruction.h](../source/runtime/frontend/x86/instruction.h) 自有 64B 记录和词汇；[decode.cpp](../source/runtime/frontend/x86/decode.cpp) 唯一解析；[region.cpp](../source/runtime/frontend/x86/region.cpp) 独立形成 |
| build_ir 的 Emitter 混合全部语义与 IR 出生细节 | [semantics.h](../source/runtime/frontend/x86/semantics.h) 及职责分组共享静态语义；IrSink 的缓存和 x87 窗口留在 [build_ir.cpp](../source/runtime/frontend/x86/build_ir.cpp)，数值执行归 [interpreter.cpp](../source/runtime/frontend/x86/interpreter.cpp) |
| compile/native_code/fault 各自解释 A64 编码与寄存器 | typed exact encoder 与单 Writer Handle；[patch.cpp](../source/runtime/backend/arm64/patch.cpp) 解释实际 link/fault payload；平台桥恢复 host 状态 |
| Process 内嵌 cache、compile、SMC 和 syscall | [vm.h](../source/runtime/execution/vm.h) 组合真实 owner；compile_service、code_cache/code_link、smc、Linux syscalls 按职责实现 |
| 公共 native/dispatch 头依赖 A64；GuestState 依赖 IR x87 | [backend/code.h](../source/runtime/backend/code.h)、[host_backend.h](../source/runtime/execution/host_backend.h) 是冷边界；唯一 guest state/x87 定义归 arch/x86 |
| syscall 自行拆 `packed_flags` | [flags.h](../source/runtime/arch/x86/flags.h) 定义 snapshot 与 RFLAGS 转换，A64 live flags 仅由 backend/platform 桥解释 |

## 3. 顶层边界与依赖

```text
Guest CPU model / GuestState / GuestMemory
                    |
x86 bytes -> DecodeOne
               |-- Block 流式构造 -------> ExecuteInstruction<IrSink> --|
               |-- Function region形成 -> ExecuteInstruction<IrSink> --+-> Program
               |                                                            |
               |                                                       common opt
               |                                                            |
               |                                                 Block/Function allocation
               |                                                            |
               |                                                ARM64 selection -> Assembler
               |                                                            |
               |                                                          words
               |                                                            |
               |                                                CodeHeap/CodeCache/SMC
               |
               +-- ExecuteInstruction<DirectSink> -> 直接执行/服务事件
```

两个 IrSink 路径消费同一份语义定义。Function 形成在 IR 出生前完成，Block 可以按现有方式流式出生；解释器不需要先形成 region 或构造 Program。

依赖约束：

- x86 byte decode 不依赖 IR、ARM64、CodeCache 或 mmap owner。
- x86 指令语义不出现 host 寄存器和 A64 编码；IR 选择提示只由 IrSink 消费。
- 中立 IR 与优化器不依赖 decoder、解释器、Linux signal 或 executable memory。
- ARM64 selector 只消费 Program、实际 Allocation 和一次绑定的 backend 配置；不访问 Process、ELF、CodeCache 对象。
- CodeHeap 不解读 guest opcode；CodeCache 不拼 A64 word；dispatcher 不分配寄存器。
- Linux 装配点绑定 frontend、backend、内存及执行服务。HostBackendOps 的冷调用和 signal 回调允许函数指针；JIT 每个 IR op 不通过虚接口。

## 4. Decode 前端与解释器共用

### 4.1 先收敛一个 DecodeOne

`DecodeOne(bytes, pc, guest_model, timing)` 只做实际 byte parser、已知 parser 差异适配和单条结果校验。Block 构造、region 形成、直接解释和诊断都消费它。CPUID、LDMXCSR、REP 的执行边界分类只定义一次，region 读取该分类。

- 保留一份实际记录。`instruction.h` 自有 64B `InstructionData`、Op、Register、operand 与 flow 词汇；`detail/decoded.h` 只做 parser 临时值到自有记录的单次适配。生产语义与 region 不再读取 vendor 字段或常量；测试的可信 raw fixture 可显式适配一次。
- `instruction.h` 给出 x86 自己的 Op、RegisterView、OperandView 和错误语义。寄存器视图表达 GPR/XMM、宽度、子字段与零扩展规则，不暴露 diStorm 的寄存器编号，也不出现 host 寄存器。
- 本轮不追求独立反汇编 SDK 的稳定二进制 ABI。内部访问器可内联读取唯一记录；不为了隐藏一个头增加每条指令 PImpl、heap object 或 virtual。
- 若以后更换 parser，只替换 adapter；不要求语义 consumer 跟随 vendor 布局变化。禁止长期同时保留 `_DInst[N]`、normalized instructions[N] 和 semantic actions[N]。
- raw bytes 继续借用稳定的 CodeWindow，Program 不保留 decoded/window 借用。直接解释的当前记录只活到该次 step／受控 REP 片段结束。
- CET 是否禁用来自 guest model；“当前 lowering 不支持某 prefix/form”属于语义支持范围，不能伪装成非法编码。
- byte timing 仍只包真实 parser 调用。适配、归一化、校验及 record 写入继续计入前端成本，不能放进被扣除的 parser 区间。

公共错误至少区分：字节不可解码／截断、region 形成限制、合法指令但语义未支持、IR 构造失败、backend 拒绝、guest 执行异常。合法 LOCK 共享 opcode 尾导致的 `OverlappingTarget` 是当前 region 限制，不是非法 x86。

### 4.2 Region 是形成策略，不能继续叫所有意义上的 function

`region.cpp` 保留当前 sparse worklist、唯一 D buffer 和 16B slices；接收 CodeSource、预算、SourceMode 及只读 ready 查询，不包含 IR emission。

- 默认 CALL 仍退出本区域，续点第一次实际请求再形成。
- ready 查询只影响形成边界，不进入 byte decoder。
- 空预算 frontier 与 synthetic exit 不成为 callable entry。
- Unknown/EarlyOff 保持通用 IR；可写源仍按当前单 source-block／外部边政策形成。
- 不重建旧 reset/redecode 循环，不增加全 opcode 支持扫描。

### 4.3 推荐终态：一份高层语义，两个静态 consumer

```cpp
template<class Sink>
StepResult ExecuteInstruction(Sink&, InstructionView);

// 两次静态实例化，不是每条指令选择一个虚对象。
// IrSink::Value     = ir::Value
// DirectSink::Value = 实际标量／向量值
```

共用的应是指令形式验证、guest operand 规则、访存／提交顺序、架构控制流和异常时机。IrSink 保留现有 NodeId、GPR/XMM 出生缓存、pending flags、地址合并与 Known-target 中立选码；DirectSink 持有实际 GuestState，通过 guest-memory service 访问内存。

这不是给 DirectSink 实现一个假的 `Emit<OpCode>`：

- `ReadGpr/WriteGpr` 共用 guest 子字段与 EAX 零高位规则，具体数值／符号值分别由 sink 处理。
- 算术 helper 共用 flags 规则；IR sink 可以延迟等价的 flags 生成，解释器按架构提交点更新 portable snapshot。
- REP 的零次旁路及逐次提交使用明确的结构化条件接口；IrSink 发条件路径，DirectSink 只执行选中的路径。不能机械调用两个分支的副作用。
- CPUID、syscall、MXCSR 切换等返回明确执行事件，由 runtime 服务处理。沿用现有出口 ABI 数值，避免再给 GuestState 增加一份 reason/reset 状态。
- 一个 opcode 分派源，按整数、控制／字符串、SIMD、x87 四组维护；不按单条指令建文件，不另建解释器支持表。

共享语义入口也只派发到各组的命名处理函数；具体 guest 行为写在这些函数中，不将整段整数／访存／控制语义塞进 switch case。IrSink 与 DirectSink 不分别复制一套指令分派实现。

当前 x87 的多指令精确窗口融合属于 JIT consumer 优化，留在 IrSink/其调用层：限定同一 source slice、逐条保留原记录验证和工作计数，再生成等价 helper。DirectSink 默认逐条执行，不因共用语义就越过中间 guest PC、异常或部分提交点。也不把 JIT 的 x87 window planner 复制成另一份 decoded actions。

**提交与故障合同必须显式保留：**

| 指令／事件 | 共用语义必须保证 |
| --- | --- |
| unsupported form | 结构支持检查在 guest 副作用前完成；解释器不能已经写内存后再要求 JIT 重跑 |
| CALL | 先读取间接目标，栈写成功后再提交 RSP 和目的 PC |
| RET | 栈读成功后才提交 RSP 和目标 PC |
| POP 的 RSP 相关 EA | 使用架构规定的新 SP 计算地址，但 fault 前不能提前提交 SP |
| REP STOS | 零计数不访问；每次成功 store 后更新 RCX/RDI，保留可恢复的部分完成 |
| x87 | 保留 ext80、TOP/tags/control/status，以及既定异常状态更新次序 |
| guest fault | 返回该指令／部分完成点的 guest PC 和状态；不退回另一个引擎重复已提交的动作 |

解释器访存必须新增明确的 safe-access 合同。现有 Native FaultSite 只能恢复已登记的 JIT 指令，不能替普通 C++ memcpy 提供 guest fault 语义。Memory service 负责 guest 逻辑权限、known EOF、SMC 写入口、顺序／atomic 要求以及返回故障；需要 faultable host access 时使用独立的窄 helper／platform recovery path，不能用未受保护的 C++ 访问假装已完成异常交付。

StepResult 至少区分完成／有进度、服务事件、guest fault 和未支持。REP 可保持同PC而减少剩余工作；guest fault 可以带已定义的部分完成，不能再解释成 Unsupported。解释值保留 consumer 的有效 view 和 BitCast/GetFlags 等 raw transport 规则，不能一律按自然 type 截断。解释器普通 C++ 栈也不能直接套用现有 native siglongjmp：恢复应在窄 access helper 内安全返回，再由语义层传播状态，避免跨越含 RAII 的栈帧。

### 4.4 为什么不直接搬旧 interpreter

旧 [interpreter.cpp](/Volumes/Data2/SwiftVM/source/runtime/backend/interp/interpreter.cpp:163) 消费旧 IR；旧 [Runtime](/Volumes/Data2/SwiftVM/source/runtime/backend/runtime.cpp:706) 还明确区分含 host-register 操作的 JIT IR 与可解释 IR。旧树没有现成的 `_DInst -> 直接执行` 引擎。

解释当前中立 Program 是可行的替代路线：能较快复用前端，但仍承担 IR 构造和 Submit。它可作为过渡或测试 oracle，不能被称为无 IR 直接解释。本方案推荐终态为 DirectSink，不强制先建设并长期维护第三套生产 IR interpreter。旧数值／flags 算法、值槽语义和 x87 helper 按当前类型及合同选择性移植。

### 4.5 Guest state 与 host live state 分开

`arch/x86/state.h`、`flags.h`、`x87.h` 是 guest 状态的共享定义。第一阶段保留现有字段布局与 snapshot 值格式，维持 GuestState 576B；不在架构搬家时改变所有 flags 生成。

- portable flags snapshot 明确不是原始 RFLAGS；其编码及 `ToRflags/GetCF/GetZF/...` 只定义在 `arch/x86/flags.h`。
- A64 的 NZCV、反向 C、x1 raw PF、x29 AF 和受限 TEST tag 是 backend live representation，仅由 ARM64 ABI／恢复桥解释。
- syscall 服务调用共享 flags codec，停止自行拆 A64 风格位域。
- x87 架构状态移出 IR namespace；IR 的 x87 操作引用共享协议，不能拥有另一套 guest x87 状态。
- guest model 决定 ISA／CET／CPUID；backend capability 只选等价中立 IR。保留 Known、EarlyOff、Unknown，GPR/XMM Uniform 不退役。

引擎切换只在完整状态边界同步一次：进入直接解释前发布 pins／portable flags，重新进入 native 前按当前 guest FP control 选择 domain 并安装环境。数值 FP 的 rounding、DAZ/FTZ、NaN、status／trap 范围沿用当前明确合同，不能因换了 consumer 悄悄改变。IrSink 仍发原中立访存 IR，由 backend 直接选码；只有 DirectSink 通过受控实际访存服务执行，不把 JIT 每次load/store改成解释器helper调用。

## 5. ARM64 encoder／assembler 的职责

机器编码本身必然含位域常量。整改目标是让这些常量有唯一位置，并让 selector、code cache 和通用 fault 服务停止各自解释它们。

### 5.1 基于定制 VIXL 的 exact encoder，保留一个输出 owner

按用户补充要求，采用受控的VIXL fork作为精确编码基础，并移除其黑盒scratch及自动操作入口。`encoding.h` 是SVM的薄typed门面：WReg/XReg/VReg、SP/ZR区分、Cond/Shift/Extend、合法immediate及patch格式委托同一编码实现；不再平行维护另一套完整手写编码器。

普通 Add、Ands、Ldapur、StrQ、BCond 等显式方法精确发一条指令；静态 inline，不能隐式申请 scratch、调用 helper 或创建 pool。单条架构alias可以保留，但必须有固定的一词编码及明确的寄存器、NZCV、writeback等效果。

热 exact 定义与 Operand/MemOperand 构造在 VIXL 自己的 `*-inl.h` 中保留单份实现，原 `.cc` 不重复定义。正式 Release／RelWithDebInfo 默认启用受工具链支持的 IPO（NDK Clang 为 ThinLTO），让职责文件的边界不成为宿主 JIT 的逐指令调用边界；`SVM_ENABLE_IPO=OFF` 可显式关闭，关闭后的性能单独看待。byte parser、`svm_x87` 和 SoftFloat ext80 helper 闭包保持独立编译；不能跨其 general-register-only ABI 导入 FP 代码。被裸汇编调用的 C++ helper 明确保留真实符号，不借此新增运行时检查或中间表示。此构建配置与实际 JIT 时间一同记录，不能将其收益冒充 IR／优化遍历数量下降。

`Assembler` 借用原 `CompileResult.words`，统一 Here、Bind、body/local label、实际 fixup、EmitU64 和 Align。它不保存指令对象列表。小函数内联，Finish 只处理实际 fixup，不再扫描所有 IR。

- selector 决定一次 opcode、宽度、flags 和 host feature；encoder 不再读 IR／Home 重新判断。
- MoveImmediate 等明确命名的多词 helper 由SVM保留有界展开，只写指定寄存器；需要额外临时寄存器的helper必须由调用方显式传入。
- labels 使用 B-sized body 位置和按真实分支增长的 fixup；不加 NodeId→label 的 N 表。
- Assembler 内部用 WordOffset；跨 backend 的真实资源坐标采用 NativeOffset。若统一成 byte offset，只在资源出生时换算一次，不增加最终转换遍历。
- 保留当前最多一次 long-forward 重发策略，沿原 owner clear/reuse；不为抽象新增 relaxation pass。
- native Root/Cold/Exit 继续静态汇编，不为“全部用同一个C++ assembler”而在 VM 启动时另造一套 dispatcher code heap。

### 5.2 VIXL 必须定制的范围

保留VIXL的typed寄存器、MemOperand、指令字段编码及branch range知识，固定fork来源并保留原许可。旧冻结工作目录的VIXL含原WIP，不能不经区分就当作干净上游；复用时逐项审核这些修改。

旧[MacroAssembler构造](/Volumes/Data2/SwiftVM/source/runtime/externals/vixl/aarch64/macro-assembler-aarch64.cc:337)默认建立`tmp_list_(ip_0, ip_1)`和`fptmp_list_(d31)`；[UseScratchRegisterScope](/Volumes/Data2/SwiftVM/source/runtime/externals/vixl/aarch64/macro-assembler-aarch64.h:3892)可在helper内部Acquire并归还寄存器。当前SVM又将XMM0..15固定到v16..v31，因此d31/v31同样不能被库偷偷使用。此问题不仅是x16/x17的寄存器压力，也影响结果正确性。

| VIXL机制 | 生产路径要求 |
| --- | --- |
| 默认GPR／FP scratch池、UseScratchRegisterScope、Acquire*及嵌套恢复 | 移除生产可用入口；不默认占用x16/x17/d31或其他寄存器，不保留第二个临时寄存器分配器 |
| 大immediate／访存offset／特殊operand的自动macro展开 | exact API不做隐式展开；SVM selector在实际选码点选择有界展开或返回CompileError，并显式提供临时寄存器 |
| literal pool及checkpoint自动插入 | 禁止；literal／DATA由SVM在明确位置Emit并登记实际资源 |
| veneer／远跳转自动修补或改写 | 禁止；范围失败回到既有显式分支选择／有界重发，不让库暗用x16/x17生成veneer |
| 隐式push/pop、spill/reload、SP调整、C++ helper调用 | 禁止；由现有frame与helper ABI代码显式发出 |
| 依host feature或代码距离自动选择另一组指令 | 由selector／显式patch策略决定，encoder只实现选定形式 |
| simulator／debug路径的额外代码插入 | 不进入生产发码接口 |
| exact单指令、固定架构alias、显式数据和对齐 | 保留；对齐和DATA也必须由调用者明确请求 |

不能只把scratch list暂时清空，或依赖ExactAssemblyScope／BlockPoolsScope和debug-only assert，就宣布消除了隐式行为。生产目标不提供隐式macro入口；调用到这类接口应在构建时暴露，不能在release版本偷偷恢复自动分配。旧WIP中的`svm_scratch_contract_*`只是在既有scratch机制上记账／许可，不作为新的常驻审计或certificate层搬回。

旧[CodeBuffer](/Volumes/Data2/SwiftVM/source/runtime/externals/vixl/code-buffer-vixl.cc:37)有owned／external buffer及managed-grow限制。fork还必须接入原words owner：编码直接写该sink，无另一块VIXL managed buffer再copy。允许同一个vector正常扩容，但不为此插入机器指令。标签由SVM Assembler持有唯一位置／fixup；不能同时维护SVM未解析表与VIXL pool/veneer修补链。

### 5.3 寄存器与展开的显式合同

去掉的是VIXL的黑盒寄存器获取。x16/x17不是库的私产；SVM可在明确的ABI／allocator规则下使用它们。当前dispatch、地址计算和helper桥已经有显式x16/x17用途，是否将其进一步纳入普通分配寄存器集合须另行修改对应ABI与lifetime，不能仅删除VIXL池就声称已释放。

```text
exact.Ldr(dst, base, encodable_offset)        # 选定形式，一条指令
EmitLoadLargeOffset(dst, base, offset, tmp)   # SVM显式展开，tmp由调用方提供
MaterializeConstant(dst, value)              # 有界展开，只修改显式dst
```

每个保留的helper必须能从签名和固定合同看出读取／写入寄存器、NZCV、base writeback、SP及内存效果；临时寄存器由现有分配／ABI事实授予，helper不再自动Acquire、borrow、spill或restore。已有live GPR、pinned XMM、PF/AF载体不能成为隐式牺牲品。

合法性与展开选择应在发码前确定。不能先部分发码，再以另一个fallback偷偷补scratch或改栈；拒绝时沿现有CompileResult清理当前草稿，保留原有有界重发。普通选码、range处理和patch仍只描述一次，不新增clobber计划表或N级验证pass。

### 5.4 选择、ABI、patch 分开

- `emit_scalar/memory/atomic/vector/float/state/flags`：按下述职责实现 IR 到具体 A64 指令的 Handle，保留现有局部相邻融合。
- `emit_control`：内部 CFG terminal、外部 static/dynamic 出口、frame/flags/PC 边界；没有 PC-directory 所有权。
- `patch`：解释本 backend 的实际 link/fault 修补格式，调用同一 encoding 算式；不决定 guest target 或 cache membership。
- `abi.h`、`abi_layout.inc` 与 Linux ARM64 bridge：唯一 pin、flags、FP、context offset 合同。静态汇编与动态发码从同一布局定义消费常量，并以 offsetof/static_assert 核对；不新增每边界状态复制。

### 5.5 单次 opcode 派发，Handle 按职责实现

实施前 `74ac9bc9` 的 Writer::Emit 先尝试 EmitVector，又可能转入 EmitState 的第二个 switch。当前 [emit.cpp](../source/runtime/backend/arm64/emit.cpp) 已收敛为**一次 opcode 派发 → 对应 Handle → exact assembler**。分配、flags、fault 和局部融合仍使用同一个 Writer 的实际状态。

推荐使用 C++20 的 opcode tag 重载 `Handle`，由现有中立 [opcodes.inc](../source/runtime/ir/opcodes.inc:1) 生成薄 switch。它已经是 OpCode／Schema 的唯一声明源；不另维护 backend opcode 注册表，也不把 ARM64 信息写回中立 opcode 表。示意如下，私有声明只列两个已支持操作：

```cpp
template<ir::OpCode Code>
using Op = std::integral_constant<ir::OpCode, Code>;

class Writer {
    struct OpContext {
        const ir::Operation& operation;
        // Cursor、Home等当前操作的局部借用字段省略，不是指令计划。
    };
    void Dispatch(OpContext&);

    void Handle(Op<ir::OpCode::Add>, OpContext&);         // emit_scalar.cpp
    void Handle(Op<ir::OpCode::LoadMemory>, OpContext&);  // emit_memory.cpp
    // 其余受支持的操作按同一方式声明，定义放进各职责文件。

    template<ir::OpCode Code>
    void Handle(Op<Code>, OpContext&) {
        Reject(CompileError::UnsupportedOpcode);
    }
    // 省略既有状态与工具声明。
};

void Writer::Dispatch(OpContext& ctx) {
    switch (ctx.operation.opcode) {
#define INST(name, ...) \
        case ir::OpCode::name: return Handle(Op<ir::OpCode::name>{}, ctx);
#include "runtime/ir/opcodes.inc"
#undef INST
        default: Reject(CompileError::UnsupportedOpcode);
    }
}
```

非模板重载匹配具体支持的 opcode；未实现的合法 opcode 统一拒绝，不能默认为 NOP 或尝试另一条语义。tag 是编译期类型，不创建运行时对象表。普通 `HandleAdd/HandleLoadMemory` 加同源宏生成映射也能达到目的，但本方案选定 tag 重载，避免混用多种派发框架。

实现规则：

- `emit.cpp` 保留 Writer 的一次 schedule 遍历、必要公共前后处理和薄派发。公共逻辑只处理实际共享的 flags/fault／结果写回，不在这里按 opcode 再实现选码，也不把每种操作都经过完整标量、向量和 helper 准备。
- `OpContext` 只活在当前迭代内，借用 Operation、Cursor、Home，以及当前用到的目的寄存器等局部事实；不分配、不复制 operand、无 N-sized 存储、不预先编码每条指令的动作。
- 每个 opcode 只有一个正式 Handle 入口。`LoadMemory`／`LoadUniform` 等同时有标量和 V128 形式时，在该 Handle 中按真实 type 直接调用具体小 helper；不再先 `EmitVector` 试探、失败后返回主 switch，也不二次按完整 opcode 派发。
- 同类 Add/Sub、TSO/普通 load 等可共用编译期参数化小 helper；宏只生成机械声明或 case→Handle 映射，不把选码、寄存器修改或错误恢复藏进宏体。模板只对 opcode／固定变体实例化，不按 VM 配置和组合 traits 复制整个 writer。
- `TerminalKind` 的少量派发同样只调用 `HandleReturn/HandleDispatch/HandleJump/HandleBranch/...`，复杂分支布局放在 `emit_control.cpp` 对应方法中。操作内部确有必要的宽度／条件枚举选择可以保留小 switch，不要求消灭所有 switch。
- 一个 Writer 保留共用流式状态；不按指令族新增 emitter 对象、独立 scratch 分配器、结果列表或动态注册。无 virtual、std::function、成员函数指针表或每指令 indirect call；生成的 switch 可以由编译器选择跳转表。跨 `.cpp` 是直接函数调用，不承诺所有 handler 都会被内联，也不为消掉调用而把大段实现搬回公共头。

### 5.6 检查放在事实建立或消费的唯一位置

不增加防御层来替代接口设计。检查按真实来源处理：

| 来源 | 检查位置与方式 |
| --- | --- |
| guest 字节、动态 guest 地址／权限、映射与系统调用结果 | 在 decode、实际访存／内存服务及资源操作边界处理真实失败；不能用 assert 代替 guest fault 或 OS 错误 |
| 稳定 backend／ABI 配置 | 在绑定或编译入口检查一次；不让每个 Handle 和 encoder 重验相同 offset、pin 与能力组合 |
| 该 backend 支持的 opcode、type、metadata、host feature 与具体编码范围 | 在实际选码或 fixup 的决策点判断；generic IR 合法不等于本 backend 必须支持 |
| 确已由 Builder／形成点保证的内部不变量 | 使用类型约束、布局 static_assert 或有必要的局部 debug assert；不在各层重复扫描 Program、operand 和 Home 来证明同一事实 |

不引入独立的前置全图 validator、逐节点 clobber audit、certificate 或发布后二次全流检查。不能把原来无保证的事实直接改成 assert；也不能为让检查通过而伪造 fault/SMC 能力。编码越界、真实 backend 拒绝、资源发布失败及 guest 异常保留既有结果边界，拒绝时清理原草稿，不附加多级 fallback／重试框架。测试仍按第11节的里程碑节奏，不为机械拆文件补一批只验证转发关系的测试。

## 6. Code memory、MemMap、CodeCache

### 6.1 先分清三个不同对象

| 对象 | 唯一职责与所有权 | 不负责 |
| --- | --- | --- |
| GuestMemory | guest VA、逻辑 protection、backing、dev/inode、offset、known size；Map/Unmap/Protect 后的唯一 region ledger | native JIT slab、entry membership |
| MemMap + CodeHeap | native backing 与 RW/RX views；slab 分配、复制、同步、fault-table buffer 和最终释放 | guest mmap 语义、guest PC 查找、guest opcode |
| CodeCache | unit 的注册描述、每 domain 的 PC membership、ready/pending、实际 relocation rows | 再拥有一份 native bytes／guest region ledger |

MemMap 是 CodeHeap 内 slab 的实际资源 owner，不是另一个 AddressSpace manager。GuestMemory 与 native MemMap 使用同类平台 mmap/protect/unmap 原语，但各自只记录自己真正拥有的映射；native code 不进入 guest region 集。

### 6.2 旧 MemMap 可复用的部分

旧 [mem_map.h](/Volumes/Data2/SwiftVM/source/runtime/backend/mem_map.h:12) 管 backing、多个 view 与权限意图。`660b6ba1` 中的“资源登记先于可能失败的后续操作、失败仍保 owner、quiescence 后释放”值得复用，见 [Map](/Volumes/Data2/SwiftVM/source/runtime/backend/mem_map.cpp:198)、[ReleaseAfterQuiescence](/Volumes/Data2/SwiftVM/source/runtime/backend/mem_map.cpp:261)。

但以下不能照搬：

- 旧 [MemMap::Protect](/Volumes/Data2/SwiftVM/source/runtime/backend/mem_map.cpp:412) 是空实现；`42e9c234` 中也有该缺口。新 API 必须对应真实保护操作并返回结果。
- 旧 guest mmap 曾用匿名页加 pread 捕获文件内容，并拒绝可写 MAP_SHARED，见 [syscalls](/Volumes/Data2/SwiftVM/source/translator/linux/syscalls.cpp:1399)。它不能替代当前真实 file-backed mapping 和 alias 身份。
- 旧 bias/window 地址空间不能直接接入当前 guest==host EA 快路径；这会改变访存和 fault 合同。多 VM 地址空间隔离另立支持阶段。
- 不原样恢复旧 Module 的持久IR／arena／地址路由混合容器、三套 TranslateTable、磁盘恢复层或全局 retained-owner 框架。模块身份及冷编译策略有实际用途，按第7.5～7.6节保留；此前一概“不恢复module”的表述过于绝对。

实施前 `74ac9bc9:source/runtime/execution/native_code.cpp` 的 `CodeArena::Clear` 会忽略 munmap 失败后清空 owner。当前 [MemMap::Release](../source/runtime/execution/mem_map.cpp) 仅在成功后遗忘对应 view，失败返回 VM close；不能把未知未释放资源宣布为空。析构保 best effort，正常关闭使用有结果的操作，不为该问题新增通用生命周期平台。

view 的失败保留与 fd close 分开：不能因 close 报错就重试一个可能已关闭、甚至已被复用的 fd。多个系统调用部分成功时，先记录已发生的真实结果，不在失败后仍声称全部旧映射存在。MemMap 不判断 unit 是否已不可达，也不能在 quiescence 前执行 Free/madvise 来回收 live code。

面向当前 Linux 的 fd owner 在发起 close 前先移出／置无效，close 只尝试一次并记录错误；未成功解除的 views 仍归原 VM。显式 `VM::Close()` 失败不把这些记录清空或转交全局队列，调用者仍持有该 VM 处理结果。CLI 便捷入口可以报告关闭失败后退出，但不能把它写成已完成全部资源释放。

### 6.3 代码输出与 published unit

- `CompileResult.words` 仍是唯一 codegen buffer。通用发布只借其 byte view，复制到 executable slab 一次。
- CodeHeap 继续拥有 slab 和 fault-table buffers；NativeUnit 的范围／fault view 是借用描述。第一阶段维持现 NativeCode 40B 的作用，不因多个 entry 增加元数据 owner。
- CodeCache 的 unit vector 可在冷点增长；native 活跃期禁止移动／清理被 fault lookup 借用的目录。保留当前稳定 anchor snapshot，而不是每 unit 新分配一个 shared_ptr。
- Entry 是 `(domain, guestPC) -> unit + native offset/callable address`。不能把 NativeCode.entry 改成内部入口，否则 fault base 和 writable offset 会错。
- 通用 `backend/code.h` 只描述真实 native ranges、entry、link、fault resource；A64 recovery payload 由 backend 回调解释。Cache 公共头不再 include ARM64 CompileOptions。

### 6.4 发布、撤销与退休的唯一顺序

```text
冷点：形成 -> IR -> optimize -> allocate/emit
    -> CodeHeap 分配/复制，解析实际 relocation，建立 fault owner
    -> 安装需要的源页写监视
    -> 注册 root/extra entries，drain 对应 pending
    -> refresh 当前 domain 的目录 view -> 恢复执行

失效：阻断当前 callable routes -> 撤 static links 与 local indirect entries
    -> 保留旧 native range/fault metadata
    -> 等当前执行到冷点且 active 清空
    -> 清 membership/借用描述 -> 释放或重用 native 映射
```

这由一个发布／失效入口编排，调用方不能任意漏掉 source protection 或 pending registration。只保留真正尚未发布的资源及现有 scratch，不创建 PublishPlan/PreparedUnit/Certificate 中间对象。

root／extra entry 注册与 pending patch 可能部分完成。该点之后发生异常，已经不能按“未发布slice”直接 free，更不能当普通编译拒绝再回退执行。首阶段保留当前 fatal close 路径；如果以后支持发布失败重试，必须先撤已登记entry／links，再在冷点退休。错误分类需把无guest副作用的编译拒绝与发布失败分开。

dual mapping 保持写 DATA→gate→conditional head 的顺序及真实 RW/RX 同步。single mapping 继续保冷链接，不假装存在可撤链 writable alias。既有 unaligned fallback 的临时保护切换仍由真实映射 owner 与 backend patch 协同。

## 7. Dispatcher、block link 与 Function level compile

### 7.1 五个概念必须分别命名

| 概念 | 定义 |
| --- | --- |
| guest/source block | 一段实际 guest 指令 slice；不等于 unit 或 callable entry |
| region | 一次预算内形成的可达区域；不是 ABI／symbol-defined function |
| allocation scope | Block 轻量分配，或 Function 的真实 RPO／跨块 lifetime；不由文件名猜测 |
| native unit | 一次编译结果，一个 frame 策略、一段范围与 fault base；可以含多个 source block |
| callable entry | unit 内满足完整入场合同的位置；不等于任意 body label |

CLI 可保 `--scope block/function`。公开文档继续写 Requested Function：可写源仍可选择 Function allocator，但实际只有一个 source block、所有边外部化。禁止把两个 scope 合并计成绩，或以“Function名字”证明已经使用多块图。

### 7.2 五条执行路径

1. **内部边**：同 unit 的 label jump/fallthrough；没有目录查找，不重复建立 frame。
2. **静态跨 unit link**：指向目标 callable entry；一条实际 relocation 持续拥有其 unlink 责任。
3. **动态出口局部 cache**：当前每实际 site 的 16 项 `(PC,entry)` hint；借 code 生命周期，不拥有 code，不接纳 pending thunk 为 ready target。
4. **domain PC directory**：当前域的唯一权威 membership；四个 FP specialization domain 分离，pending 与 ready 明确区分。
5. **cold resolver**：发布完整 guest 状态后做缓存失效、编译／解释选择及服务处理，再返回真实可调用入口。

`dispatch.cpp` 只做第5条的语义编排与出口事件。A64 Root/Cold/Exit 静态代码放 platform ARM64 bridge，内联快目录查找由 backend emitter 发出；它们共享 ABI/layout/hash 事实，不新增一个通用 per-lookup 虚调用。

### 7.3 具体 API 与 link 资源

```text
LookupReady(domain, pc) -> EntryView          # 含真实 entry + 完整 unit view
PublishEntries(unit, entries)                # first-ready wins
AddStaticLink(owner, targetPC, site)
AddIndirectSite(owner, array, fallback)
RevokeAllDomains() / RetireAtColdPoint()
```

CodeCache 是唯一 owner。`code_link.cpp` 可以实现同一个类的 link 方法，不另建持有第二张表的 LinkManager。

冷编译请求只需要 root PC、请求 scope、预算、domain 与 SourceMode。SourceMode 首期为 Immutable、WatchedBlock、Volatile：前两者分别使用当前不可写区域形成／受监视单块策略；Volatile 不授予持久 JIT cache，只有明确的 memory-service 资格才允许直接执行，否则报告当前 backing 不支持。可写多块资格完成后再加入独立模式，不把同一判定拆成多份可漂移的 policy/certificate。

实施前 LinkSite 以 `branch_word == UINT32_MAX` 改变 target_pc 含义，runtime relocation 又以 `back_words == 0` 区分动态 cache。当前 [LinkSite](../source/runtime/backend/code.h) 使用显式 LinkKind 与 byte offsets，static target 和 native fallback 分字段表达；LinkSite 为24B，真实 relocation 为32B。资源成本计入正式测量，不能用隐蔽 bit packing 换名义不变。

offset→实际映射地址只在发布时解析一次；源 target、patch kind 和 unit ownership 不经过 inputs→preparation→actions→certificates 再描述。

### 7.4 Domain、失效与升级

- domain 只包含影响该 native 版本的实际 specialization，先保当前四种软件 DAZ/FTZ 模式；不把所有 MXCSR 位都扩成 cache key。
- guest model、host features 和全局 memory policy 在 VM 内固定；变更时统一失效。
- scope 是编译策略，不自动成为另一张 PC 表。第一阶段保 first-ready；将来 tier upgrade 通过受控替换／撤链，不覆盖共享 unit descriptor。
- interpreter-only 模式无需创建 native CodeHeap。mixed 模式保留 Function 失败后从请求root尝试 Block 的既有策略；若再选择直接解释，必须从实际 GuestState.pc／已恢复的有效 continuation 执行。Function 的静态 error_pc 可能来自未执行的分支，不能作为解释器入口；IR 已出生的 prefix 也不是已执行的 guest prefix。
- DirectSink 在该条指令副作用前由同一个语义case确定支持度；开始执行后只返回完成／有进度、服务或 GuestFault，不能返回 Unsupported 让另一引擎重跑。REP 同PC且有进度也算成功。
- 若需要记忆不可 JIT 的入口，绑定 source 版本的 slow entry 进入同一个目录，不能冒充已编译 unit，也不另建失败PC哈希表。

### 7.5 旧 Runtime／Module／Function／Block：哪些合理，哪些不复用

旧实现不是 `Runtime → Module → Function → Block` 的单一所有权树。实际关系是：

```text
AddressSpace / Instance                         # 共享翻译与执行资源
  ├─ Module范围路由及默认Module
  │    ├─ IR地址索引 → Function 或 standalone Block 的强引用
  │    │                 └─ Function拥有子Block，Block拥有指令与terminal
  │    └─ 多个native CodeCache → MemMap / executable allocation
  └─ 共享入口目录、links、fault / SMC资源

Runtime                                         # 一个运行上下文
  ├─ State / uniform buffer / 线程局部执行状态
  └─ 借用AddressSpace；自身不拥有整份共享地址空间
```

这套设计在“共享资源与线程状态分离”“IR可先释放、机器码继续存在”“撤可达性与释放bytes分开”上合理；不能据此推导其全部容器、接口或关闭路径已经适合当前主线。

旧JIT也不是每次dispatch都走Module→Function→Block：native先查Runtime的L1／共享L2，冷查询 [GetCodeCache](/Volumes/Data2/SwiftVM/source/runtime/backend/address_space.cpp:190)在L2 miss后才回到Module／IR索引。需要评审的是这些状态与寿命如何维护，不能把冷查询层数直接算成每次执行成本；新方案也不把module／IR树叠加到ready目录前。

| 旧对象／机制 | 实际行为与评审 | 当前方案的处理 |
| --- | --- | --- |
| AddressSpace 与 Runtime | Runtime自有state storage并借AddressSpace，共享代码与线程状态分开，见 [Runtime::Impl](/Volumes/Data2/SwiftVM/source/runtime/backend/runtime.cpp:127)；Instance须活过所有Runtime | 复用这个分工；VM持共享资源，GuestThread持当前状态，首阶段只含一个线程 |
| Module | [MapModule](/Volumes/Data2/SwiftVM/source/runtime/backend/address_space.cpp:103)只建guest范围路由／策略容器；[loader调用](/Volumes/Data2/SwiftVM/source/translator/linux/main.cpp:115)传主image范围。Module还拥有IR索引及多个native arena，并非ELF image owner或guest mmap owner | 保留source image身份／冷策略用途，拆开实际映射、IR和native存储职责 |
| Function／HIRFunction | 持久Function拥有Block树，HIRFunction还表达准备中的region视图；形成受root、预算及边界限制，不是已证明的symbol／ABI函数 | 复用bounded region和scope概念，不保留跨编译的Function／HIRFunction对象树 |
| Block | 同时含guest范围、IR、锁、JitCache及生命周期关联；standalone和Function child的所有权形式不同 | 继续采用16B decoded slice、Program内24B BlockRecord；机器码入口和fault/link寿命不挂回IR块 |
| JitCache 与 native CodeCache | JitCache是状态／cache-id／offset描述；旧CodeCache才拥有native arena，名称与新PC CodeCache并不同义 | 保留entry与allocation分离；arena归CodeHeap，权威PC membership归CodeCache |
| Detach／Reclaim | [DetachNode](/Volumes/Data2/SwiftVM/source/runtime/backend/module.cpp:540)撤IR节点／版本可达性，[ReclaimCode](/Volumes/Data2/SwiftVM/source/runtime/backend/module.cpp:559)才移除fault资源并回收code；中间须有真实执行静止条件 | 沿用先撤入口及链、后冷点退休的原则；当前单线程资格不照搬旧多线程retirement平台 |

必须修正或不作为复用基础的细节：

1. **解除module路由不等于卸载。** `42e9c234`、`660b6ba1`及冻结WIP的 [UnmapModule](/Volumes/Data2/SwiftVM/source/runtime/backend/address_space.cpp:157)均只改RangeMap，不撤PC入口／links，也不释放native code；`660b6ba1`还由owned_modules保留owner至关闭。它不能承担新方案的卸载或SMC事务。
2. **节点存在不等于ready entry。** Module的 [GetNode](/Volumes/Data2/SwiftVM/source/runtime/backend/module.cpp:183)按exact start找Block／Function；GetNodeOrCreate能先挂未编译IR，GetJitCache另查Cached／可调用入口。不能把Module范围命中、找到Function、任意child body或allocation base直接当作可调用入口。新目录继续以真正发布的Entry为准。
3. **旧树成员迁移有结构成本。** [AddressNode](/Volumes/Data2/SwiftVM/source/runtime/ir/block.h:44)只有一个intrusive map hook，Function子块树与Module索引共用它；[BeginStandalonePublication](/Volumes/Data2/SwiftVM/source/runtime/ir/hir_builder.cpp:74)因此要先从Function Detach。新Program内ID／range无需继承这套迁移、引用计数和父子锁协议。旧Function释放指令但保留块／JIT描述的能力值得参考，具体持久对象不是保留该能力的必要条件。
4. **范围查询原语不能直接复用。** 旧 [Overlap](/Volumes/Data2/SwiftVM/source/runtime/ir/block.h:102)只看查询两端，漏掉查询完全包住节点，并把端点相接算重叠；[FindBlock(false)](/Volumes/Data2/SwiftVM/source/runtime/ir/function.cpp:124)也不能可靠查前驱包含块。未发现生产调用false，不能据此宣称现有SMC线上失效；但它们不适合作新模块／失效范围原语。新代码统一半开区间，使用GuestMemory的实际范围事实。
5. **旧容器和命名不能当性能目标。** Module的AddressHashMap按guest VA范围保留桶，Function／Block是指针树；这些形态不替代当前紧凑storage和唯一PC目录。旧call／pending-call表服务已发布的不同flags入口变体，pending-call不是待编译队列；新pending_head仅表示静态链接waiters，也不是异步编译器。
6. **旧解释循环有跨module边界。** [Interpreter](/Volumes/Data2/SwiftVM/source/runtime/backend/runtime.cpp:698)只在入场选择current_module，循环更新PC后仍查旧module，可能对另一module已有IR产生额外CodeMiss。外层可重新按PC选择，因此不能直接推断错误执行；新DirectSink不继承这份持久IR／module查找链。

版本资格也须保留：`42e9c234`原版signal fault lookup会取共享锁；`660b6ba1`已引入FaultCatalog、显式关闭和owned_modules；冻结WIP又增加CompileEnvironment、共享GuestStateBacking及Function-owner查询等。复用时逐项说明来源，不能把后加修正算作原版已有，也不把冻结WIP整体视为稳定库。

### 7.6 推荐的管理模型：共享VM、线程状态、短命IR和持久native产物

```text
VM
  ├─ GuestMemory                      # 唯一guest映射及backing事实
  ├─ LoadedImage元数据                 # image身份、load bias、entry；冷策略来源
  ├─ CodeHeap                         # 实际native slabs / views / fault buffers
  ├─ CodeCache[domain]                 # NativeUnit描述、真实Entry及link membership
  ├─ CompileService                   # 单次region → Program → native产物
  └─ GuestThread                      # 首阶段按值包含一个，尚不授多线程资格
       ├─ GuestState / 当前FP domain
       └─ NativeRunContext / fault恢复 / 当前active borrow
```

- **Module职责保留但不恢复大容器。** 当前只支持静态ELF，image身份、load bias、entry及必要的策略描述放在`platform/linux/loader`的LoadedImage元数据，不单建ModuleManager、全VA default Module或每module code heap。匿名代码使用VM默认编译策略；它是否能读取／执行仍由GuestMemory和既定CodeSource合同决定，默认策略不授映射能力。以后真正支持DSO／加载卸载及多module策略时，再独立source module服务；当前文件结构不预建空管理层。
- **映射事实只存一份。** image元数据不复制每次mmap/mprotect后的region ledger，不把最小／最大image包围区间当成全部有效代码。`read_only`或module声明不替代真实protection、MAP_SHARED alias和SourceMode；RWX benchmark资格不因此改变。
- **策略只在冷入口选择。** guest CPU model与host能力仍归VM；若以后存在module-specific编译策略，在形成前解析一次，region不跨不同策略／SourceMode的边界。改变影响产物的策略先失效已有代码，不给每条IR增加ModuleId／generation，也不恢复热路径的module→function→block查找。
- **Function和Block保编译含义。** 一次编译的region／Program是短命owner，Block是其中的slice／record；Block与Function仍分别决定分配策略。当前ARM64产物不借Program，编译完成后IR可释放或复用容量，只保真实fault／link／entry资源。未来backend若有helper确实借IR，必须为该实际借用明确owner，不要求所有backend默认保留IR。
- **NativeUnit和Entry独立于IR寿命。** 多个entry共用unit范围和fault base；多个unit可包含同一guest source，不能把PC当全局唯一IR block owner。撤销完整unit时，其root／所有extra entry及incoming links一起处理，不能只删Function root。共享slab上的其他unit也不能随一个module元数据析构而被误释放。
- **线程状态不再混进共享owner。** `runtime.h/.cpp`内把VM和GuestThread职责分清，当前按值组合，沿用GuestState576B与NativeRunContext160B。GuestThread停止／注销后才关闭其借用的VM资源；不为拆职责增加shared_ptr图、每thread一份code heap或未实现的QSBR／epoch。

模块卸载、源映射替换、策略变化统一经VM协调：先阻断已有callable routes并撤链接，在真实冷点结束active借用并退休完整unit，再执行或提交GuestMemory的映射变更／image元数据更新；操作失败保留其真实结果。当前全域撤销已能覆盖共享unit及backing alias，先用它完成合同。选择性失效需要时才增加按真实source ranges／pages连接unit的索引，不让Function树或Module范围路由代替依赖记录。此处定义未来卸载的必要顺序，不宣称本轮已经实现DSO卸载。

## 8. SMC 的当前资格与目标

### 8.1 已有与缺失

| 场景 | 当前资格／目标 |
| --- | --- |
| 匿名可写执行源，实际 JIT store 写入 | 已有页保护、signal 恢复实际 store、撤全部域 routes，冷点退休；保留当前单 source-block 边界 |
| 已登记普通文件／memfd 的 MAP_SHARED writer | 已按 backing 身份和文件范围覆盖 alias，含非 EXEC 的 RW alias |
| guest read 等 host CopyOut 写 guest buffer | 已经由服务入口先处理 code 影响，再执行 host 写入 |
| guest write/writev/pwrite/ftruncate/O_TRUNC 修改 backing | 已按成功结果和真实 fd 身份刷新已知 size、退休 executable observers |
| mmap/mprotect/munmap、known EOF | 已有映射／缓存协调及 decoder 可读范围限制；不等于完整 guest SIGBUS 交付 |
| 外部进程修改、未登记 host alias、设备 backing、外部截断 | 不属于当前监视资格；目标是显式拒绝／volatile source 策略，不能默默声明可持久缓存 |
| 可写源的多块内部边／回边 | 当前未授予；须单独证明 code write 后的内部执行退出与撤销合同 |
| 多个 guest 线程并发发布／失效 | 未完成一般资格，不能用现单 active 指针冒充并发回收协议 |
| guest 异步信号／完整 guest fault delivery | 当前不完整；host fault recovery 与 guest signal delivery 分开建设 |

以上“已有”仅指当前受管理路径和单注册执行线程，见 [memory.cpp:127](../source/runtime/execution/memory.cpp:127)、[runtime.cpp:399](../source/runtime/execution/runtime.cpp:399)、[fault.h](../source/runtime/execution/fault.h:14)。同块未来字节修改、外部写入竞态等未独立证明的边界不扩写为全SMC支持。

### 8.2 SMC 服务需要明确四项工作

1. **分类与布防**：查询 GuestMemory 的逻辑权限／backing，建立实际源页及 alias watch；临时 host 去W不改 guest 逻辑 protection。
2. **发现写入**：signal 窄回调或受控 host-service 写入口；signal 内不分配、不加普通锁、不 fstat、不抛异常、不销毁 code。
3. **撤销**：使目录快入口不可达，并重置四域 static links 和 local16；fault metadata 仍活着。
4. **退休**：当前 guest 线程退出 native 后，在 cold 点释放实际 owner。

GuestMemory 保唯一映射事实，SMC 只拥有当前 `armed` 等写监视状态和失效政策；不各存一份 region ledger。

native active 非空时，region、unit 目录和被借用的 fault buffers 保持稳定。信号可变状态继续放在 VM／线程 owner，不放在会被 longjmp 跨过的临时对象中。默认仍以全域撤销和冷点统一退休完成合同；这些条件不等同于多线程锁或 epoch 协议。

### 8.3 可写代码 Function 的后续工作

本轮五个 microbenchmark 的 ELF 是单段 RWE，其 Requested Function 全部走 SMC 单块路径，B/F native bytes 与IR工作量相同。没有实际写 fault 不等于可以把代码当 immutable；不能改 ELF 权限来改善正式成绩。

后续要在此类源上形成多块 region，必须先选择并实现以下一种真实合同：在受保护的 source-block 边界检查失效并完整退出，或者具备能在 code-write 后安全中断 region 的恢复路径。内部 fallthrough、回边、frame、flags、pin、fault-prefix 和重新布防都需要明确。不允许只删 `mutable_function` 判断。

初期继续全域失效，不引入 per-node 版本表。只有确实推进选择性失效时，才增加按真实 code pages／source ranges 连接 unit 的索引，并说明其实际所有权与冷编译成本。多线程 epoch/quiescence、guest 信号投递另立真实支持阶段。

## 9. 目标文件结构与各源文件责任

源码中的函数／方法定义之间由 `SeparateDefinitionBlocks: Always` 统一留空行，`scripts/format_runtime.py --check` 检查同一规则。普通 Handle 直接使用函数控制流，不保留旧 switch 的单次 `do/while(false)` 包装。

IR 优化仍按「首逆扫 → CFG 摘要传播及出口写入修正 → 唯一前向改写 → 按需末逆扫」执行；拆文件不增加全 IR 扫描、IR 副本、use 表或 pass 注册层。小型高频谓词继续内联，Release IPO 保持跨源文件优化。 本次源码整理的回归结果与小规模 compile 对照见 [验证记录](evidence/source-cleanup-20261106/README.md)。

以下是实际实施结构。`.h/.cpp` 配对属于同一个模块；按所有权／语义分组，不是一个文件等于一个 pass。IR 优化归属 `ir`，文件按分析、改写和局部折叠职责组织；tests 与 externals 未列部分保持当前布局。

```text
source/runtime/
  externals/vixl/                   # 固定来源的受控fork；精确编码／单sink，删除隐式macro入口
  arch/x86/
    state.h                         # 唯一 guest 状态布局
    flags.h                         # portable snapshot／RFLAGS codec
    cpu.h  cpu.cpp                  # guest CPU model、CPUID
    x87.h  x87.cpp                  # ext80 状态／共享数值服务

  frontend/x86/
    instruction.h                  # 唯一自有64B记录、InstructionView、operand／flow术语
    opcodes.inc                    # 自有x86指令词汇声明；供enum与adapter映射共用
    detail/decoded.h               # vendor到自有记录的一次适配；仅decoder与可信fixture使用
    decode.h  decode.cpp            # DecodeOne、parser 修补、诊断 timing
    region.h  region.cpp            # bounded formation、D buffer／16B slices
    semantics.h                    # 唯一静态语义分派与 sink contract
    semantics_dispatch.inc         # 唯一guest opcode派发到命名处理函数
    semantics_operands.inc         # guest operand／地址／宽度／前缀规则
    semantics_integer.inc          # 整数／flags 高层语义
    semantics_control.inc          # CALL/RET、条件、REP、架构提交次序
    semantics_vector.inc           # 当前 SIMD/FP 指令语义
    semantics_x87.inc              # x87 指令到共享数值协议
    build_ir.h  build_ir.cpp        # IrSink、出生缓存、中立选码、Program 输出
    interpreter.h interpreter.cpp  # DirectSink、step/run、执行事件

  ir/                              # 唯一 Program/Builder/CellRange，跨前后端的 IR 合同
    optimize.h                     # 通用 IR 优化入口、Uniform 语义策略和结果统计
    passes/
      optimize.cpp                 # 单 owner、arena 预算、错误收口和固定阶段顺序
      liveness.cpp                 # use/flags 活跃性、Function CFG 摘要传播与最终 DCE
      rewrite.cpp                  # 唯一前向改写，copy 与 Uniform 转发共用现有 use 表
      fold.cpp                     # 常量求值及有界 scalar/vector/address 局部模式
      detail.h                     # 私有共享事实、CFG 实际缓冲和小型热谓词

  backend/
    code.h                         # 真实 native range／entry／link／fault 契约
    dispatch_layout.inc            # 唯一目录hash／slot布局常量，供ABI与静态汇编消费
    arm64/
      encoding.h                   # 定制VIXL的typed exact门面／已知patch格式
      assembler.h  assembler.cpp   # 借 words、labels、fixups、data 定位
      abi.h                        # pins、flags、context layout 事实
      abi_layout.inc               # C++与静态汇编共用的布局常量
      allocate.h  allocate.cpp     # Block/Function Home 与实际 schedule
      compile.h  compile.cpp       # 一次主编译入口、实际资源输出、重发边界
      emit.h                       # 单 Writer 私有状态、Handle重载与必要小工具声明
      emit.cpp                     # 一次schedule遍历、公共前后处理、薄opcode派发
      emit_operand.cpp             # Cell/值视图读取、Home访问、显式地址／spill读取工具
      emit_scalar.cpp              # 标量整数／位运算Handle
      emit_memory.cpp              # 普通／TSO标量及向量访存Handle、实际fault点
      emit_atomic.cpp              # CAS/RMW等Handle、LSE与LL/SC显式选码
      emit_control.cpp             # 控制IR与terminal Handle、边／entry/exit／local16
      emit_state.cpp               # GPR/XMM Uniform/pin、PC与状态访问Handle
      emit_flags.cpp               # flags Handle、NZCV/PF/AF载体与物化工具
      emit_vector.cpp              # SIMD整数／位运算／shuffle等Handle
      emit_float.cpp               # SIMD/scalar FP与转换、x87原生快路径Handle
      helper_calls.cpp             # 真实 C++ helper ABI 桥
      patch.h  patch.cpp           # 本 ISA 的 link/unaligned 修补逻辑

  execution/
    runtime.h  runtime.cpp         # 公共运行API／VM执行循环／实际DirectServices
    vm.h                          # VM真实owner组合及私有服务声明
    guest_thread.h                # 单GuestThread状态、native／fault borrow
    host_backend.h                # 一次绑定的冷compile／patch／FP／恢复回调
    timing.h                      # 实际生命周期区间计时
    compile_service.h .cpp         # 冷编译：形成→IR→B→C→发布的唯一编排
    code_cache.h  code_cache.cpp   # domain 目录、unit 注册、ready/pending owner
    code_link.cpp                  # 同 CodeCache 的实际 relocation 方法
    dispatch.h  dispatch.cpp       # 冷 resolve／解释与native切换／出口事件
    memory.h  memory.cpp           # 唯一 guest mapping ledger、受控访问
    mem_map.h  mem_map.cpp         # native backing/views 的实际资源 owner
    code_heap.h  code_heap.cpp     # slab、发布、同步、fault buffer、释放
    smc.h  smc.cpp                 # write watch、撤销、cold retirement 政策
    fault.h  fault.cpp             # 通用故障事件与受限恢复合同

  platform/
    fp_environment.h              # 数值解释用host fenv隔离；guest模型不含host CSR
  platform/linux/
    loader.h  loader.cpp           # 静态ELF／初始栈／LoadedImage身份及冷策略元数据
    syscalls.h  syscalls.cpp       # Linux x86-64 guest ABI 到现有服务
    signals.cpp                    # sigaction/TLS/原handler链的窄适配
    safe_access.h .cpp             # 受控process_vm_readv/writev；失败返回，不跨C++栈longjmp
    arm64_runtime.h .cpp           # HostBackend绑定、HWCAP、FP／恢复桥
    arm64_entry.S                  # 静态 Root/Cold/Exit，消费同一ABI常量
```

文件职责补充：

- `backend/code.h` 不拥有第二份机器码，发布只借 backend words 的 byte view；fault 的 host recovery payload 留给 backend 解释。公共 code cache 仅理解地址、范围、资源种类和目标 PC。
- `externals/vixl`是编码依赖，不再拥有独立的scratch、pool或码流；其修改范围与版本固定，不接回旧MacroAssembler/JitContext/Actions链。
- `compile_service` 不产生 Prepared/Action；只协调现有 owner 和调用顺序。BackendOps 在 Linux 装配点绑定一次，编译器内部仍静态调用 encoder。
- `code_link.cpp` 不定义独立管理器，不拥有另一份 unit／PC 表。
- `mem_map.cpp` 是资源模块，可按当前平台选择实现；`signals.cpp` 和 syscall 的 Linux 细节不污染 encoder／decoder。
- `arm64_runtime` 是当前 concrete host bridge；解释器可在满足 memory service 的 host 上独立构建。当前方案不宣称同时完成 Windows、一般多VM地址空间或其他 JIT backend。
- 按职责拆分的 semantics `.inc` 仅供同一个静态语义模板包含；不是第二套指令流或生成期微操作表。
- `compile.cpp` 不承担逐条操作实现；`emit.cpp` 不积累指令族逻辑；`emit.h` 不成为另一份大实现文件。现有大 inline 编码移入 exact encoding，较复杂的 operand／状态逻辑归对应 `.cpp`，仅将必要的小访问器留在私有头。
- 上述各 `emit_*.cpp` 实现同一个 Writer 的方法，不是一组 pass／manager；除去一个过大的 switch，不产生新的指令遍历或中间表。`emit_float` 中只含 host 发码，ext80共享数值服务仍归 `arch/x86/x87`；helper ABI桥只负责真实调用约定，不成为所有复杂 opcode 的收容文件。
- 当前没有单独的`module.*`／`function_manager.*`／`block_manager.*`：source image元数据在loader，编译region在frontend，Program／BlockRecord在IR，持久NativeUnit／Entry在CodeCache。将来新增DSO服务时只增加实际image管理职责，不复制这些owner。

### 当前文件如何退出

| 当前实现 | 迁移后的去向 |
| --- | --- |
| decode.cpp 内形成＋BuildIR内第二份解析 | DecodeOne 到 decode；worklist 到 region；删除第二 parser 路径 |
| build_ir.cpp 大型语义与 Emitter | 高层语义到四组 semantics；IR-specific NodeId／缓存留 build_ir |
| compile.cpp 中 Run／raw encoding／scalar／terminal | 编码归 encoding/assembler；遍历与薄派发归emit.cpp；Handle归各职责文件；compile只保主编译与重发边界 |
| EmitVector／EmitState 二次switch与emit.h中的大实现 | 按职责改成直接Handle重载，复杂工具到operand／flags／state等.cpp；删除旧试探／二次派发入口 |
| dispatch.cpp A64 inline asm | 静态 bridge／entry.S；原通用名字用于冷 dispatcher |
| Process 内 CodeCache／CompileUnit | code_cache、code_link、compile_service；原内嵌实现删除 |
| 旧AddressSpace／Runtime／Module的可复用职责 | 共享资源归VM、线程状态归GuestThread；image身份／冷策略归loader元数据，不搬持久IR索引与每module arena |
| native_code.cpp | native view资源到 mem_map；发布／fault buffer到 code_heap；A64 patch到backend/arm64/patch |
| memory.cpp 的 ELF 与 code-write policy | ELF到loader；write-watch到SMC；guest mapping事实保memory |
| runtime.cpp 大型 syscall switch | linux/syscalls；VM只服务装配与执行循环 |
| guest_state、guest_cpu、ir/x87 state | arch/x86；不复制第二套状态 |

## 10. 防止再次膨胀的验收约束

1. **数据份数**：D一份、Program一份、u64/N优化表一份、Home/N一份、words一份。F/L/E只记录真实fault/link/entry，label/fixup只随实际B/branch增长。
2. **决策次数**：guest指令形式判定一个来源；backend配置接入一次；每IR op选码一次；每site实际地址解析一次；每unit发布一次。状态不在 inputs/preparation/actions/certificates 中反复描述。
3. **热路径**：无每opcode virtual/std::function、无新增全IR扫描、无second code buffer、无每边界通用状态reset。VIXL无隐式GPR/FP scratch或自动插码；临时寄存器与展开由SVM显式决定。CodeCache hot slot首阶段保持32B，GuestState576B、NativeRunContext160B先不改布局。
4. **职责拆分原则**：源码按语义和资源职责组织，同一owner的方法可分布于多个.cpp。Backend opcode／terminal的case只调用Handle；无“大switch搬家”或向量／状态二次试探派发。拆源文件不新增manager、缓存或独立状态机。
5. **接口合同**：默认／Unknown继续有用；只有真实安装的fault/SMC/native消费者才授相应能力。不能仅为了让检查通过而授事实。
6. **资源成本透明**：冷metadata尺寸若为清晰tag增加，记录实际字节与allocation；不为保持名义大小引入难审查的压缩协议。
7. **性能口径**：任何搬到VM初始化的分配仍计入cold生命周期，源码验证／解释前IR／normalization也不能隐入被扣除byte parser窗口。
8. **检查预算**：外部边界处理真实失败，内部已保证的事实不层层重证；无新增全图验证／常驻审计。稳定配置检查一次，opcode支持与编码范围在实际决策点处理。
9. **管理边界**：source module、region、allocation scope、NativeUnit和Entry不是一组嵌套持久对象；IR释放不影响可执行产物，撤路由不等于卸载，多entry必须沿完整unit退休。当前单GuestThread不靠旧多线程设施获得名义资格。

## 11. 推进顺序与阶段交付

| 阶段 | 交付 | 验证重点 |
| --- | --- | --- |
| P0 冻结与合同 | 固定74ac源码、输入／runner、模块owner及本轮范围；本方案就是入口 | 只读核对现有五轮数据，不重复跑基准 |
| P1 独立 decoder／定制VIXL exact assembler／Handle组织 | 一个DecodeOne；抽region；单Writer按职责拆源文件、同源薄派发到Handle；VIXL单sink、无隐式scratch/pool/veneer；同words typed encoder／labels | 以构建及现有少量编码／构造合同为主；显式operand之外的x16/x17/v31等保持；words与资源坐标等价；不扩大guest ISA、不新增验证pass |
| P2 代码内存与执行服务 | VM／单GuestThread及MemMap/CodeHeap/CodeCache/CompileService职责落定；LoadedImage元数据；typed link资源；Linux bridge私有化 | shared unit base及全部entry、pending、single/dual map、release结果；不再保旧内嵌实现，不恢复持久IR／module管理树 |
| P3 状态与SMC边界 | portable flags/x87定义、共同访存服务、统一写入通知及撤销／冷退休入口 | 已有SMC变体和file alias/EOF合同；保持单线程、可写单B资格 |
| P4 共用语义与直接解释 | 分指令族抽semantics、IrSink保持JIT、DirectSink逐步覆盖当前子集；InterpretOnly/Mixed事件接线 | CALL/RET/POP/REP/x87 fault-prefix与提交次序；unsupported前无副作用；引擎切换guest状态完整 |
| P5 统一锚定与清理 | 删除重复parser／raw encoding／旧公开类型／临时桥；更新实际支持矩阵 | 红魔相同ELF/FEX/配置固定五轮，分别验Block/Function与总compile窗口 |
| 后续能力项目 | 可写多B guard、选择性失效、多guest线程／guest signals、更多ISA／host | 单独建立真实资格，不从P5通过推导 |

P1 的 decoder 与 assembler 可独立施工；涉及 CodeCache/native ABI 的所有权接线由一个人整合，避免并行改出两个事实来源。P4 按指令族迁移，每族始终只有一个正式语义实现；临时对照路径在该族完成时删除。

测试节奏遵循此前约束：纯文件迁移以构建为主；有逻辑变化的边界使用已有少量合同，补真正新分支的测试；不每次改动都跑全Android／PMU矩阵。完整生成质量与五轮性能放在上述主要里程碑，过程版本允许有临时变化，不能拿过程结果当最终资格。

固定门槛继续为每输入／scope的time和user cycles≤1.10×FEX、compile扣实际byte≤2×FEX。结构整理的工程目标是让Core Function保持当前约51ms水平；建议对可重复的大案例compile中位数增加超过5%的结构开销先定位再推进，不把它自动接受成新基线。短案例保全部样本和波动范围，不按门槛补跑。解释器另报decode＋执行总成本，不与JIT compile指标互换。

## 12. 推荐实施边界

这次主线是**整理成可维护的编译／执行框架，并交付当前支持子集的直接解释能力**。第一轮保留当前优化质量和已验证SMC资格；用旧设计的资源／ABI分层改善边界，保留新数据形态和直接发码预算。

已落地的部分：自有 decoded 记录和 DecodeOne、region 独立、typed exact encoding、同 buffer Assembler、单 Writer Handle 派发、VM／GuestThread 与代码资源 owner、Linux syscall／ABI边界、共享 flags/x87 状态、统一 SMC 撤销退休，以及静态共享语义的 IrSink／DirectSink。Volatile backing 不授持久 JIT 或未经支持的直接 fetch。可写多块与并发 SMC 仍需要后续真实执行合同。

当前基准、历史回退及其证据继续保留：[五轮锚定](post-push-benchmark-2026-11-05.md)、[Function入口复用与拒绝尝试](function-entry-reuse-2026-11-05.md)、[原框架合同](compile-framework-contract-2026-11-02.md)。本轮实施已改变生产源码；固定 ELF、FEX 与重构前 runner 保留原身份，新结果单独记录，不覆盖历史成绩。
