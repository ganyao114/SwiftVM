# ARM64 → x64 机制优化实施方案

日期为 2026-11-12。本文记录机制优化的实现方案。实施已覆盖 NZCV → host EFLAGS、跨单元 ABI、完整受支持 FPCR 域、浮点区间、状态 effects、寄存器复用和共享冷调用。最终构建、验收与测量见[实施结果](arm64-x64-mechanism-optimization-results-2026-11-12.md)。

目标是在完整保留 guest 数值、状态观察与故障恢复语义的条件下，减少热路径中的 flags 转换、环境切换和状态搬运。各机制交付运行时代码和对应回归，集成版本交付相同 ELF 的测量记录，以冻结 runner、源码哈希和差分记录区分基线与实现。当前工作区已有上一轮四项优化，统一保留其修改。

## 1. 当前基线与实施优先级

实现基线采用已经完成四项优化的当前工作区及其冻结 runner，源码身份和二进制 SHA-256 见验收证据。Git HEAD `da0a6920` 对应此前的生产版本，当前未提交的四项实现需要一起纳入基线快照。参考引擎使用固定 Dynarmic `9d4582339990d4eae53f1dc7160686920fc2075c`。证据来自[四项优化验收记录](arm64-x64-optimization-results-2026-11-11.md)和[已核对的原始摘要](evidence/arm64-x64-opt4-20261111/summary.json)。

下表采用 11 月 12 日稳定复测，WSL `ProtonHM`、CPU2、Release/IPO、SwiftVM Function/pin0，单位为 ms。

| 负载 | 当前 SwiftVM | Dynarmic | 当前 SwiftVM／Dynarmic |
| --- | ---: | ---: | ---: |
| 整数 | 27.595 | 12.335 | 2.24× |
| 访存 | 28.597 | 15.743 | 1.82× |
| 移位 | 29.720 | 15.620 | 1.90× |
| 浮点 | 10.085 | 4.314 | 2.34× |
| 综合计算 | 9.954 | 3.725 | 2.67× |

本轮机制分析另做了每个引擎、每项负载 3 个进程，每进程 60 次热执行的 CPU-clock IP 采样，共 125,195 个样本，丢失数为零。综合程序 99.69% 的样本位于生成代码，数值 helper 约 0.04%，原生调度入口约 0.18%。矩阵的浮点算术、MXCSR 处理和 FPSR 提交区间占约 88% 的样本。区间同时包含算术与状态维护，具体单项收益由消融测量确认。诊断结果在 [build 中的分析摘要](../build/analysis/arm64-x64-residual-2026-11-12.json)，原始目录为 `/home/proton/.cache/SwiftVM/arm64-x64-residual-20261112-rb7a16lv/`。

| 批次 | 具体交付 | 主要依赖与验收 |
| --- | --- | --- |
| 1 | 固定当前基线，建立 flags 状态、保存槽和 fault recipe | 裸后端快照往返、真实 fault、保留位和寄存器恢复 |
| 2 | 函数内 EFLAGS 驻留、保留 flags 的事件检查、终结条件直达 Jcc | 原整数、访存、移位循环及事件退出 |
| 3 | 多块合流、复杂条件、ADC/SBB 与部分 flags 更新 | 全条件矩阵、CFG 合流、跨块读取、named snapshot |
| 4 | x64 跨编译单元原生 flags ABI | dispatcher、static link、entry、helper、cache 和 AOT |
| 5 | 按需生成 FPCR 专门化代码，再按区间管理浮点环境与异常状态 | 原 FP、向量矩阵、标量矩阵、FPCR/FPSR 边界与版本数量 |
| 6 | 精化状态 effects，消除局部被覆盖的 uniform 写回 | 矩阵、SHA、排序、精确 fault 与 helper 观察 |
| 7 | 结合真实指令约束复用最后一次使用的输入寄存器 | 向量、双操作数 ALU、spill 和源目的重叠 |
| 8 | 将浮点慢路径集中到冷区，再测量可共享的调用序列 | 代码布局、JIT 成本、I-cache 相关计数、重定位 |

批次 1—3 完成函数内 flags 优化，批次 4 扩展运行 ABI，实施中保留 flags 阶段的 runner。批次 5—8 合入集成版本进行完整验收。最终性能采用四项优化后的冻结基线、集成候选和固定 Dynarmic 三方比较，合成 IR 另行验证各机制的编译规模趋势。总收益按集成版本报告。

## 2. 沿用 ARM64 后端的状态合同

| 现有 ARM64 机制 | x64 对应实施位置 |
| --- | --- |
| `ObserveNZCV` 与 `Home::nzcv_clobber` | 在 x64 分配分析中计算 flags 的实际观察需求和允许覆盖位 |
| `PrepareNZCV/FinishNZCV` | 在实际宿主 clobber 前保护旧值，在 producer 后登记新值与 carry 极性 |
| `nzcv_matches/direct_carry` | 追踪 EFLAGS 有效位、快照有效位以及逻辑 C 与物理 CF 的对应关系 |
| `EnsureNZCV` 与 terminal condition | 读取条件所需位，直接生成 Jcc、SETcc 或 CMOVcc |
| `Shadow/PackNZCV` | 在 guest 可观察边界生成 portable packed flags |
| `SnapshotCompareNZCV/RestoreCompareNZCV` | 保护内部比较、选择和临时算术周围的 guest flags |
| `FaultRecovery` 与 signal 现场恢复 | 在每个真实 guest 访问处记录 flags 来源及 carry 极性 |

复用现有 `OperationMetadata`、`flag_outputs`、`FlagProfile`、`discard_omitted_flags`、用途信息和观察语义。flags 来源信息放在 x64 的实际分配／发码状态中，条件和值仍使用现有 IR。

实现参照位于 [ARM64 分配分析](../source/runtime/backend/arm64/allocation_selection.cpp)、[NZCV 生命周期](../source/runtime/backend/arm64/emit.h)、[flags 发布](../source/runtime/backend/arm64/emit_flags.cpp)和[故障恢复](../source/runtime/backend/arm64/recovery.cpp)。

## 3. NZCV、EFLAGS 与现有 IR 的语义

### 3.1 同宽整数运算的硬件关系

| guest 运算 | N | Z | V | C | 带 carry 输入 |
| --- | --- | --- | --- | --- | --- |
| ADD／ADCS | SF | ZF | OF | CF | ADC 使用 CF = 输入 C |
| SUB／CMP／SBCS | SF | ZF | OF | `!CF` | SBB 使用 CF = `!输入 C` |
| ARM 逻辑置 flags 指令 | 结果符号 | 结果为零 | 0 | 0 | 按该指令的 IR 定义生成 |

上述关系适用于相同运算位宽的整数操作。浮点比较、窄宽度运算和部分更新通过各自语义处理。硬件 carry 定义和快照指令依据 [Arm 指令手册](https://documentation-service.arm.com/static/6245c734b059dc5ff9a8bdab)与 [Intel 指令手册](https://cdrdv2-public.intel.com/782156/325383-sdm-vol-2abcd.pdf)。

### 3.2 发码器必须以 IR carry 为准

当前 ARM64 前端为 SUB 设置 `ArithmeticCanonicalCarry`，随后显式生成 `InvertCarry`。x64 应分别解释这两步。

| IR producer／profile | producer 后的逻辑 C |
| --- | --- |
| ADD／ADC，两种算术 profile | 宿主 CF |
| SUB／SBB／NEG，`ArithmeticCanonicalCarry` | 宿主 CF |
| SUB／SBB／NEG，`ArithmeticRaw` | `!宿主 CF` |
| `InvertCarry` | 翻转当前逻辑 C |

IR 的 `Sbb` 消费逻辑 borrow 输入。guest SBC 到 IR 的转换继续遵守前端已有的 carry 操作。发码器以 `logical_C = physical_CF XOR carry_xor` 表示当前关系，`InvertCarry` 可以通过更新该事实延迟到实际消费、规范化或发布时完成。

`LogicalZeroCVA`、`SetCarry`、`SetOverflow`、`ClearFlags`、`RestoreFlags` 和部分 `flag_outputs` 分别更新其声明的位。PF、AF、DF 及保留位由现有 portable 状态合同约束。现有 [前端 flags 元数据](../source/runtime/frontend/arm64/semantics.h)、[x64 profile 测试](../source/tests/rebuild/x64_compile_test.cpp)与[portable flags 定义](../source/runtime/arch/x86/flags.h)是逐项验收依据。

## 4. 首轮 flags 表示与分配

### 4.1 三种载体

首轮采用 packed 跨单元 ABI，函数内部允许当前 NZCV 驻留于 EFLAGS 或紧凑保存槽。

| 载体 | 内容与责任 |
| --- | --- |
| RBX | portable flags 的基值，包含 PF 来源、AF、DF 和保留位，以及最近一次物化的 NZCV |
| EFLAGS | 当前有效的 SF、ZF、OF、CF，carry 极性由发码事实解释 |
| 函数帧保存槽 | 8 字节 raw RFLAGS 快照，跨宿主 clobber 保存完整 NZCV |

`Writer::FlagsState` 记录 NZCV 是否 dirty、EFLAGS／保存槽是否有效，以及两份载体的 carry XOR。完整 producer 直接登记新版本，部分更新先与规范保存槽合并，形成完整 NZCV 版本。每次写入都使其他旧版本失效。

任何能够在下一次覆盖前被观察的位，都必须能从 RBX、EFLAGS 或保存槽恢复。纯编译期的源位置和极性信息由实际访问处的 fault recipe 固化。

下图表示一次完整 NZCV 版本的来源切换，RBX 同时保留其他 portable flags 字段。

```mermaid
flowchart LR
    P["RBX packed 基值"] -->|条件消费前恢复| N["EFLAGS 当前值"]
    N -->|宿主 clobber 前保存| S["帧内 raw RFLAGS 快照"]
    S -->|原生消费者前恢复| N
    N -->|观察边界物化| P
    S -->|观察边界物化| P
```

### 4.2 原生快照格式

实施采用 8 字节 raw RFLAGS 保存槽。NZCV 分别读取 bit7、bit6、bit11、bit0，carry XOR 单独描述逻辑 C。`PUSHFQ/POP R11` 捕获现场，通过帧内 MOV 保存。消费者需要原生条件时，用 `PUSH [slot]/POPFQ` 恢复。RAX 可继续承载算术结果、地址或返回值。

完整的同宽 NZCV producer 可以直接登记 EFLAGS。部分 NZCV producer 将所写位合并到规范保存槽，未写位保留原版本。PF 来源、AF、DF 和其他 portable 保留位始终具有 RBX 基值。命名快照和覆盖这些字段的 writer 使用完整 portable 合同。

保存／恢复使用基础 x64 指令，RAX 在快照期间保持完整。正常代码通过 MOV、Jcc 保留 EFLAGS，在真实 clobber 或观察边界使用快照。保存槽按 8 字节分配，函数帧继续遵守 16 字节对齐。

### 4.3 分配和入口选择

在 `CompileOptions` 增加与 ARM64 同类的 `local_flags` 控制，在 `Allocation` 中分配一个 flags 保存槽。`Runtime::configure` 可读取已有 `GuestLayout::isa`，并为满足请求状态／fault 合同的 Block 与 Function 启用原生载体。

VM 在 x64 后端且 `HostCompileRequest::private_state`、`fault_locations` 均已成立时启用。Known、Unknown、EarlyOff 三种前端 lowering 均采用一致的跨单元 ABI。ARM64、ARM32、x86 guest 的 VM 编译单元使用同一原生快照 ABI。裸后端调用通过 `CompileOptions::local_flags` 保持可选。`Compile` 向 allocator 传递 flags offset 与局部策略，分配接口的默认参数继续支持现有调用形式。

保存槽由帧布局统一管理，和 SSA spill、helper 区分别计数。所有可调用 entry wrapper 分配同一布局的函数帧。整数微循环预计新增一个对齐后的 16 字节帧，该成本单独记录。guest PF/AF 和其他输入仍通过现有完整 flags 合同验证。

## 5. 批次 1—2 的具体发码改动

### 5.1 状态保护与 producer

1. 在 x64 的 flags 分析中按块反向计算观察需求，复用现有 IR 的读写集合、metadata 和 uniform flags 范围判断。
2. 将真实 fault、helper、事件交付、退出、flags 字段读写、named snapshot 和未知 effects 纳入观察点。
3. handler 先确定实际发码形态，在其第一条可能破坏旧 flags 的宿主指令之前保存仍有需求的位。
4. `CaptureFlags` 对 `Type::U32/Type::U64` 的完整 NZCV producer 登记原生有效位、dirty 位和 profile 极性。其余形态先经 packed 合并路径建立完整状态。
5. `PublishFlags` 在实际观察处将有效载体覆盖到 RBX，再发布 guest 状态。转换过程消费已经保护的快照。

保存函数的调用位置需要覆盖 `Read/Mask/Sign/Expression` 的临时指令。宿主 clobber 由实际发码形态和 operand helper 共同确定，IR 的 `WriteFlags` 描述 guest 语义。`WriteResult` 的窄化、Bool 归一化以及浮点发码中的整数检查也纳入同一审计。

`Nop`、仅表达顺序约束的空 barrier、MOV／LEA 形式的宽值搬运具有保留 EFLAGS 的发码事实。回归同时检查空标记前后生成字节完全一致，防止优化器产生的空节点引入保存／恢复。

### 5.2 事件检查保持 EFLAGS

`GuestSafepoint` 的快路径使用 MOV 与 JRCXZ，事件交付路径发布完整 flags。下面是带标签的目标形态，实际计数器寄存器由分配结果决定。

```asm
sub counter, 1
mov rcx, [pending_events]
jrcxz check_branch
jmp event_exit
check_branch:
jne loop
```

`JRCXZ` 的短跳目标紧邻一个冷路径跳转，发码器据此保证位移可编码。RCX 属于当前 scratch 集，事件检查在终结分支处借用它。冷路径在调用发布逻辑之前仍持有 producer 的 EFLAGS。

编译冷分支时复制并恢复 `Writer` 的 flags、PC 和 fault 状态，做法参照 ARM64 `HandleGuestSafepointFamily`。两个运行分支各自具有正确的状态事实。

### 5.3 直接消费条件

| 条件 | EFLAGS 中有效的位 | 发码 |
| --- | --- | --- |
| EQ／NE | Z | JE／JNE |
| MI／PL | N | JS／JNS |
| VS／VC | V | JO／JNO |
| GE／LT／GT／LE | N、V，必要时 Z | JGE／JL／JG／JLE |
| CS／CC | C | 按 `carry_xor` 选择 JC／JNC |
| HI／LS | C、Z | 将物理 CF 规范为 `!逻辑 C`，使用 JA／JBE |

规范化 carry 时可使用 CMC，并同步更新 `carry_xor`。逻辑 C 因此保持相同含义。

沿用 `terminal_only` 的单消费者判断，扩展到合法的 `CondSet/LocalCondSet`。一般 Bool 使用 SETcc/MOVZX 生成完整的 0/1，并标记结果已经归一化。标量条件选择在操作数准备完成后使用对应 CMOVcc。

`TestFlags(mask)` 表示所选位中任意一位为真，`TestNotFlags(mask)` 表示所选位全为假。多位 mask 按其准确布尔公式匹配 Jcc，其余形态使用现有条件计算路径。

### 5.4 首批回边的证明范围

先处理能证明目标块 NZCV live-in 为零的函数内自回边。块内首次完整 producer 在任何观察前建立新状态，回边上的旧 NZCV 载体可结束生命周期。

其他边采用 raw RFLAGS 保存槽规范化入口。两个目标都需要 flags 时，先保存一次，再直接 Jcc 到目标。只有一个目标需要 flags 时，将转换放在通往该目标的路径上。退出边和事件边将最终 NZCV 物化，自回边进入下一轮完整 producer。

对当前 `ADD/EOR/SUBS/B.NE` 负载，验收应看到循环回边直接消费 ZF，循环中的逐轮 packed NZCV 转换消失。完整退出状态仍逐位与参考比较。

访存循环的入口 flags 通常在首次 load/store fault 时被观察，其 live-in 按实际分析保留。批次 3 用统一保存槽入口处理此类回边，并验收访存循环的指令收益。

## 6. fault、helper 与特殊 writer

### 6.1 x64 fault recipe

在 `backend/x64/recovery.h` 定义编码／解码函数，继续使用 `FaultSite::recovery` 的 8 字节后端描述。访问类型、宽度和全局 kind 的位置如下表。

| 字节 | 新模式的解释 |
| --- | --- |
| 0 | 低两位为模式，0 表示 RBX，1 表示状态已发布，2 表示本地载体。bit2/bit3 分别记录 native／saved 的 carry XOR |
| 1 | 低四位为 EFLAGS 来源 mask，高四位为保存槽来源 mask，位序采用现有 IR 的 C、V、Z、N |
| 2、3、7 | 保存槽相对 fault 现场 RSP 的 24-bit 字节偏移 |
| 4 | access |
| 5 | bytes |
| 6 | `FaultSiteKind` |

两组来源 mask 使用互斥位，其并集覆盖该访问处尚待物化的 NZCV。剩余位取自 RBX 基值。裸后端的保存槽超出编码范围时重新选择 portable 发码。跨单元 ABI 请求在该情况下返回 `FrameTooLarge`，运行时按现有 Function → Block 降级规则处理。

`platform/linux/x64_host.cpp` 将 `REG_EFL` 与现有 GPR 现场一起传给恢复函数。需要保存槽时，从现场 `REG_RSP + offset` 读取 raw RFLAGS snapshot。恢复函数按 mask 和 carry XOR 覆盖 portable NZCV，同时合并 RBX 中的其他 flags 位与现有 pinned 状态。

recipe 对应实际可能 fault 的访存指令起点。地址计算、对齐检测、访问前后的宿主 flags 来源分别由其实际指令位置决定。

### 6.2 helper 与原子路径

- `PrepareCall` 在进入 Helper phase 和任何宿主 clobber 前发布完整 guest flags。`FinishCall` 以 helper 返回后的 guest 状态建立新基值。
- `GetFlags`、命名 `SaveFlags`、`RestoreFlags` 和 flags uniform 的重叠访问使用其准确观察语义。命名快照继续保存完整的 portable 值，并在 producer 处记录其实际版本。
- `CMPXCHG16B` 借用 RBX 前发布状态，对访问处记录模式 1。其他原子发码根据实际 CMPXCHG／重试分支保护 guest flags。
- `ReadFPStatus`、浮点准入检查、`Select`、局部 Goto 和对齐检查的宿主比较按内部 clobber 保护。

## 7. 批次 3—4 的多块与 ABI 扩展

### 7.1 多块合流和部分 flags

为普通函数内合流建立统一的 raw RFLAGS 保存槽入口，保存完整 NZCV，并规范为 `physical_CF = !logical_C`。入口恢复得到确定的有效位和 carry 极性。首次实现由边上的转换序列完成规范化，后续根据所有前驱一致的事实消除转换。

在此基础上扩展部分 `flag_outputs`。保留位由 RBX／旧快照合并，新定义位登记其真实来源。`discard_omitted_flags` 按 IR 已授予的覆盖权限处理。ADC/SBB 先保护需要保留的旧位、准备操作数，再建立准确的 carry 输入。

每个块从已定义的入口事实开始发码。先在现有块遍历中计算四位 NZCV 的 gen/kill，再读取现有 terminal 边建立前驱索引，执行工作队列求解。每个事实位沿单调方向收敛，传播成本按 `O(4 × (B + E))` 计算。合流入口采用已定义的 raw RFLAGS 合同，指令发射执行一次。新增的快照覆盖旧版本时，需要先合并仍有效的旧字段。

### 7.2 跨编译单元原生 ABI

1. root entry 从 portable guest 状态导入 raw RFLAGS，建立 `physical_CF = !逻辑 C`。
2. static link 和 dispatcher 通过新增的 `NativeRunContext::eflags` 交换规范快照。`native_eflags` 表示实际 ABI 模式，汇编偏移由静态断言校验。
3. 就绪单元按 live-in 需求将 context 快照移入帧保存槽，在实际原生消费者处恢复 EFLAGS。序言／尾声的栈调整可以使用当前布局的 SUB/ADD，快照覆盖整个调整区间。
4. `SvmNativeDispatchNZCV` 及 counted 版本具有独立入口体。就绪 lookup 保留原生快照，cold resolve、helper、guest exit 和信号交付生成 portable 状态。
5. x86 与 ARM32 的 PF、AF 和部分 writer 继续遵守共享 IR 的完整 flags 合同，NZCV 之外的字段保存在 RBX 基值中。

所有 L1 probe 入口均参与 dispatch interrupt 识别，恢复时从 context 原生快照与 RBX 基值重建状态。pending thunk、static-link fallback 和 external entry wrapper 使用同一合同。SMC 失效清理所有已创建控制域的链接。

`NativeRunContext` 扩展为 1088 字节，已有字段偏移保留。`native_eflags` 和 `eflags` 分别位于 1072、1080。配置身份包含宿主原生 flags 能力与完整 FPCR 专门化能力。

## 8. 持久化与代码身份

本地载体、保存槽和新 recovery tag 的源码变化进入现有 `CodeBuildId.cmake` 指纹。raw RFLAGS 使用 x64 基础指令集，原生 flags 能力进入配置身份。若引入同一二进制内可切换的发码策略，其实际生效模式一起进入配置哈希。

8 字节 recipe 由 x64 解码器识别各模式，并校验 mask、极性和偏移编码。代码身份负责持久代码与当前恢复规则的匹配。跨单元 ABI 的实际模式也进入配置身份，序列化布局扩展时同步处理相应格式版本。

每批验证冷 JIT、磁盘缓存重载、helper 符号重定位和 AOT。重载检查命中与编译计数，AOT 检查载入单元、结果和原生执行计数。

## 9. 其余五项机制的具体实施

| 已识别的问题 | 对应方案 |
| --- | --- |
| 浮点环境按指令管理 | 批次 5b 在全部受支持 FPCR 域中形成直线浮点区间 |
| FPCR 编译专门化不足 | 批次 5a 按需生成 32 种完整语义域的版本 |
| 状态 effects 较粗，写回消除有限 | 批次 6a 定向失效 uniform 缓存，6b 跟踪局部覆盖写 |
| 寄存器分配与指令约束结合较少 | 批次 7 在现有分配循环中交接最后一次使用的输入 home |
| 慢路径逐条内嵌 | 批次 8a 集中冷区，8b 以规范调用合同共享 stub |

### 9.1 批次 5a 完整 FPCR 按需专门化

实现直接覆盖当前 `IsSupportedFPCR` 接受的 FZ16、RMode、FZ 和 DN，共 32 种语义组合。`FPCRDomain/FPCRForDomain` 在架构层提供统一压缩／展开。`fp_mode` 低两位继续承载需要该能力的后端的 DAZ/FTZ，bit2—6 承载 ARM64 FPCR key。

VM 保留 128 个可空的域指针，各域的 CodeCache 在首次进入时创建。每个 PC／语义域按实际请求编译，缓存目录及 static link 始终属于对应域。AOT 默认编译初始域，运行期间首次进入其他域时按同一规则生成。

`CompileOptions::arm_fpcr` 将实际控制传给发码器。含专门化二元 FP 运算的外部入口检查一次 FPCR，条件失效后复用 `NativeExit::FPEnvironment` 重选域。MSR FPCR 继续结束 decode 并通过现有状态服务切换。

allocator 在现有遍历中识别显式 FPCR store 和可能改写控制的 opaque helper。此类单元在区间入口重新检查 FPCR，失配时在当前 guest 指令执行前退出。架构精确数值叶子具有保持 FPCR 的合同，其共享 stub 复用原调用点的检查。

`ArmFPBinary` 对中央有限指数与零输入使用对应舍入模式的 SSE2 路径。该准入范围保证结果保持正常数，仅可能产生 IXC。特殊值、边界指数和 subnormal 进入带该 FPCR 常量的精确 helper。

持久单元保存完整 `fp_mode`，装载显式选择目标 cache，完成后恢复调用者域。序列化格式升级到版本 5，磁盘 key 哈希混合完整域字节。L1 在实际域变化时 reset，运行统计记录域创建数、切换数与 reset 耗时。稳定控制、全部 32 个组合、反向切回、特殊输入及版本复用由 native 测试覆盖。

### 9.2 批次 5b 在专门化控制域形成浮点区间

批次 5a 建立实际 FPCR 的编译事实，再形成区间。区间包含当前已具备精确准入检查的 F32/F64 ADD/SUB/MUL，以及能够证明保持控制状态的 uniform 搬运和普通操作。FPCR/FPSR 观察、真实 guest 访问、未知 helper、safepoint 和控制流边界结束区间。guest flags producer 前也结清区间，使其产生的 EFLAGS 直接供后续条件消费。

`Writer` 用固定大小状态记录区间是否活跃、原 MXCSR 保存位置、工作槽与异常累计方式。在已有发码遍历中遇到首条合格 FP 操作时进入区间，在边界发码前结束区间。

- 进入区间时保存原 MXCSR，并装入一次对应舍入模式的清洁环境。
- 区间内累计硬件 sticky exception，逐条提交 guest 数值结果，维持现有逐 lane 位分类。
- 离开区间时读取 MXCSR，将该准入范围内已证明等价的 IXC 合并到 FPSR，再恢复原环境。
- 特殊输入转慢路径时，先提交此前指令的 IXC，再调用精确数学叶子。返回后重置工作累计状态，使后续代码从确定的专门化环境继续。
- FPSR 写入先结清之前的累计状态，再建立写入后的状态。FPSR 读取、fault、helper 和事件观察前具有完整的异常结果。

慢路径与快路径返回同一个 join 时，环境、保存槽生命周期和 flags 来源具有一致合同。显式状态观察作为区间边界，保证累计贡献只归属正确的 guest 指令范围。

区间发现与发码合并为一次线性遍历。主要改动位于 `emit_float.cpp`、`emit_calls.cpp`、`Writer::Run` 与浮点观察边界。诊断记录区间数、长度、MXCSR 维护次数及慢路径命中。原 FP、向量矩阵和标量矩阵分别测量。

### 9.3 批次 6 定向 effects 与局部覆盖写

**6a 精化 uniform 访问范围。** 在现有 `RewriteValues` 中识别 `ArmFPBinary` 的固定五操作数格式，读取已有 FPCR/FPSR 偏移。其 uniform 读取范围为 FPCR、FPSR 各 4 字节，写入范围为 FPSR 的 4 字节。按真实重叠范围失效缓存，并继续保持异常贡献的可观察效果。

该规则复用当前八项 `UniformCache`。每条合格 FP 操作最多查询两个读取范围和一个写入范围，访问成本受固定表长约束。其他 effects 查询按已经验证的 helper 与观察合同逐项精化。

**6b 消除被后续写完整覆盖的 store。** 在同一张八项表中增加最近 store 的 NodeId，以及独立的待覆盖写标记。

1. 对受支持的 `StoreUniform` 记录地址、宽度和 NodeId。后续写完整覆盖旧范围且期间观察允许时，通过现有 Rewrite 接口移除旧 store，并更新 use rows 与 live 标记。
2. 对重叠的部分覆盖保留所需值，地址逸出与未知 effects 结束待覆盖写的跟踪。
3. 同范围 `LoadUniform` 已成功转发为 SSA 值时，延续相应跟踪。实际 uniform 读取和显式状态观察结束该范围的候选写。
4. MayFault、helper、事件和控制流边界清除待覆盖写标记。能够继续成立的 uniform 值缓存按现有读转发规则处理。
5. 被删除 store 的依赖经 `needs_final_cleanup` 进入已有最终 DCE，一次完成本轮清理。

首个例子是整数循环中 ADD 与 EOR 对同一 GPR 的写回。正确性测试覆盖转发读取、部分覆盖、重叠 FPSR 字段、helper 和 fault。矩阵、SHA、排序提供完整负载验证。reservation 清除由独立的有效性与 exclusive 序列分析处理。

本批的新增查询复用既有 rewrite 遍历。八项表溢出采用候选失效处理，编译状态大小保持常量。

### 9.4 批次 7 在现有分配循环中复用输入 home

首版只检查实际支持原位写入的操作的第一个输入，复用现有 `last_use` 与寄存器 bank。

1. 操作属于声明过的纯标量 ALU／SSE 双操作数集合，输入具有兼容位宽，`last_use == 当前 definition_position`。
2. 输入仍驻留在相应 bank，bank 的持有者与输入 NodeId 一致。
3. 在现有选寄存器循环中优先选择该槽，将其持有者交接给结果。输入的原 home 和 `spilled` 标记继续描述其此前所有使用。
4. 该交接使用专门的复用分支。现有 `spill(old_value)` 将值全局标记为 spilled，并安排从定义处保存，适用于真正淘汰寄存器的情形。
5. handler 在破坏输入前读取完需要的操作数，并将结果直接写入选定 home。具有 helper／慢路径的操作在逐一证明其约束后加入集合。

当前静态 home 模型需要这一显式交接。分配算法继续扫描最多 6 个 GPR 或 12 个 XMM，新增判断复用同一循环。复杂操作可以采用既有分配与发码路径。

诊断统计 MOV/MOVDQA、实际 `Home.spilled`、flags 保存槽和 helper 区。当前 `spill_slots` 包含帧内调用区，报告同时列出这些组成项。回归重点为结果后续被 spill、源目的别名、同一输入的多种 view、移位计数和窄读归一化。

### 9.5 批次 8 集中冷区并按调用合同共享

**8a 集中冷区。** 在 `Writer::Run` 主体之后发射浮点准入失败路径。热路径以条件跳转进入冷区，正常算术顺序落入下一条操作，冷区返回对应 join。

在已有 allocation 遍历中累计所需冷记录数量，据此一次预留记录容量。FP 调用点记录原 NodeId、入口、返回标签和共享 stub 索引，操作数直接查询现有 home。stub 键记录模式、状态偏移、保存寄存器位图、flags 与浮点区间合同，帧布局和 pin 策略属于当前 Writer。事件冷记录保存 flags 来源和 guest PC。live save 集合以物理寄存器位图记录。

**8b 共享调用 stub。** 完成布局测量后，对规范调用合同相同的记录建立固定字段的键。键包括运算模式、控制／状态偏移、寄存器保存位图、帧布局、pin 与浮点环境策略。记录在创建时由哈希表查找复用，处理量按预期 `O(F)` 计。

共享 stub 使用局部 CALL/RET 承接不同返回位置。SysV 对齐、CALL 新增的栈字和 helper 参数偏移一并设计，调用前的 flags 与浮点状态满足规范入口合同。原参数和结果缓冲由每个调用点提供，helper、恢复与 relocation 沿用现有符号合同。

8a 的验收包括热路径分支、布局、编译延迟和代码大小。8b 另外核对各调用点的返回与状态恢复，以及共享后的体积和额外调用成本。当前 68 条 `ArmFPBinary` 的 11,764 字节慢路径及调用边界作为静态比较数据。

## 10. 代码修改清单

| 文件或目录 | 主要修改 |
| --- | --- |
| `backend/x64/allocate.h`、`allocate.cpp` | flags 需求／允许覆盖事实、保存槽、terminal-only 条件与输入结果复用 |
| `backend/x64/compile.h`、`runtime.cpp`、`abi.h` | local flags 选择、GuestISA 请求适配、宿主能力检测与 feature 身份 |
| `backend/x64/emit.h`、`emit_flags.cpp` | 载体状态、快照保护、物化、carry 极性及原生条件消费 |
| `backend/x64/emit.cpp`、`emit_control.cpp` | edge 状态、事件检查、入口／尾声及直接终结条件 |
| `backend/x64/emit_operand.cpp`、`emit_scalar.cpp`、`emit_memory.cpp` | 操作数准备、归一化与真实访存处的 flags 事实 |
| `backend/x64/emit_calls.cpp`、`emit_atomic.cpp`、`emit_float.cpp` | helper、原子、内部比较与浮点区间状态 |
| `backend/x64/recovery.h`、`recovery.cpp` | 8 字节 recipe 与各来源的状态重建 |
| `platform/linux/x64_host.cpp` | 从 signal 现场提供 RFLAGS 与 RSP |
| `backend/x64/entry_elf.S`、`abi_layout.inc` | 跨单元原生 ABI、dispatcher 和冷出口 |
| `execution/code_identity.cpp`、`code_image.cpp` | 配置身份与恢复描述验证，按实际布局处理版本 |
| `execution/runtime.cpp`、`vm.h`、`host_backend.h`、`disk_cache.*`、`code_persistence.cpp`、`offline_compile.cpp` | 按需 FPCR 域、cache 选择、服务切换、持久化与 AOT |
| `ir/passes/rewrite.cpp` 及相关 effects 查询 | 精确范围的失效与局部写回消除 |
| `source/tests/rebuild` | 发码、语义、事件、helper、fault、cache 与 AOT 回归 |

表中 `backend`、`platform`、`execution`、`ir` 均位于 `source/runtime` 下。

## 11. 正确性验收

### 11.1 flags 状态和运算

- 穷举 16 种 NZCV 输入、16 个条件以及两种 carry 极性，覆盖 true/false 边和 Bool 数值用途。
- 对 32/64 位 ADD、SUB、ADC、SBB、NEG、逻辑操作比较两种算术 profile、溢出、借位、零和符号边界。
- 覆盖 `InvertCarry` 连续翻转、SBC／ADC 输入、guest 不置 flags 的算术、部分写和 `discard_omitted_flags`。
- 覆盖 named snapshot 的延后消费、多个消费者、结果末次使用、RAX/AH/AL 保存和源目的别名。
- 逐位检查 portable flags 的 PF 来源、AF、DF、保留位与最终 NZCV。

### 11.2 运行边界

- safepoint 快路径和真实事件交付，检查完整退出快照。
- flags 活跃时穿过 guest load/store、地址计算、未对齐访问、跨页失败和保护页 fault。
- helper 读取／修改 flags、慢路径返回、Select 和内部比较。
- pin0/pin2、Block/Function、多前驱合流、self-loop、static link、间接调度、cold resolve 和 SMC 失效。
- CAS128 的 RBX 借用与原子访问故障。
- 磁盘缓存重载、重定位、AOT 载入与完整程序结果。

### 11.3 既有回归矩阵

复用 `x64_compile_test.cpp`、`runtime_engine_test.cpp`、`exception_recovery_test.cpp`、ARM64 frontend 和 IR optimize 测试。新增用例聚焦原生 flags 载体引入的版本、clobber、极性和恢复问题。

集成版本重跑已有 4,224 次 ARM 基础差分、27,648 次标量 FP 和 25,296 次二元／indexed 差分。ARM64、ARM32、Thumb 完整程序覆盖上一轮的 24 组配置，x86 guest 运行现有 frontend、浮点、解释器与原生合约，并对既有 corpus 作完整状态的基线回归比较。

完整 CTest 记录实际执行与 skip。当前基线为 31 通过、7 跳过，后续记录对每个 skip 保留名称。

## 12. 性能测量与交付条件

### 12.1 测量协议

复用 `tools/bench_dynarmic.py` 和冻结的五个原始 ELF。比较四项优化后的 SwiftVM、新候选与固定 Dynarmic，使用 CPU2、Function/pin0，三组轮换顺序，各 31 个新进程，每进程 3 次热执行取中位数。五个控制负载采用同一协议，pin2 和 Block 另作各 9 个进程的扩展组。

另跑 PMU 和采样。记录 retired instructions、cycles、IPC、branches、branch misses、enabled/running 时间以及丢失样本。头条耗时来自普通 runner。报告逐轮配对分布，并保存出现高离散的完整测量轮次。

冷调用、进程初始化、compile request、代码大小和热执行分别报告。静态代码区间与采样区间使用实际发布代码的地址映射。

### 12.2 flags 批次的预期代码形态

| 验收对象 | 目标形态与指标 |
| --- | --- |
| 原整数自回边 | producer 的 ZF 直接进入 Jcc，事件快路径保留 EFLAGS |
| NZCV 发布 | 完整转换集中在实际观察／出口及需要规范化的边 |
| 条件消费 | 原生条件直接 Jcc/SETcc/CMOVcc，单消费者 terminal 使用现有用途事实 |
| fault | 现场 recipe 重建与精确参考的 guest PC、地址、flags 和已完成状态一致 |
| 指令计数 | 在相同 12,000,000 轮 ELF 上，整数低于当前约 37 条／轮，访存低于约 37 条／轮，移位低于约 40 条／轮 |
| 动态运行 | 目标负载获得重复热耗时改善，所有输出检查通过 |

指令数下降与热耗时分别验收。Dynarmic 的整数约 23 条／轮作为比较数据，具体可达到的数量由实现后的同路径反汇编与 PMU 核对。

### 12.3 集成交付内容

1. 运行时改动及实际启用范围。
2. 语义和边界回归结果，包含原生执行、fallback、skip 与输出校验。
3. 冻结基线／候选／Dynarmic 的热耗时、配对分布、JIT 成本和代码体积。
4. 原始 ELF、runner、源码身份、构建参数和结果 SHA-256。
5. 可定位的热路径反汇编、fault recipe，以及 flags／环境维护的代码形态和动态计数。

完整交付要求八项机制均具有实际代码和回归，最终候选通过完整正确性矩阵、普通 runner 测量、PMU、采样和编译规模验证。集成测量、实现取舍与验收中修复的问题一并写入结果记录。

## 13. JIT compile 成本审阅

### 13.1 已有编译时间分布

当前原始证据中，ARM64 完整程序 Function/JIT/pin0 的一次冷运行包含 12 次编译请求。其累计阶段耗时如下。该记录用于成本敏感性分析，各新批次的增量由候选实测获得。记录路径为归档内 `results/final-programs-summary.json` 的 `programs.arm64.runs["function/jit/pin0"]`。[原始归档](evidence/arm64-x64-opt4-20261111/raw-evidence.tar.gz)

| 阶段 | 累计 μs | 占 compile 总量 |
| --- | ---: | ---: |
| Frontend | 1,085.955 | 57.6% |
| Optimize | 221.215 | 11.7% |
| Backend | 434.130 | 23.0% |
| Publication | 73.460 | 3.9% |
| 其他请求内工作 | 70.782 | 3.8% |
| Compile 总量 | 1,885.542 | 100% |

例如，仅 backend 增加 20%，其余阶段和请求次数相同时，这条记录的 compile 总量约增加 4.6%。版本数量变化则会增加完整的编译请求，需要累加新增请求的实际耗时。

上一轮四项优化已测得综合负载 compile 增加约 9%—12%，同时生成代码增加 58.1%。本轮各项以该实现为比较基线，按新增分析、生成字节和实际版本数解释变化。

### 13.2 每项新增工作量

以下 N 为 IR 节点数，W 为操作数存储量，B/E 为 CFG 块／边数，F 为 ARM 浮点操作数，R 为分配器 bank 大小，U 为现有 uniform 表长。当前 R 最多为 12，U 为 8。

| 项目 | 新增工作与上界 | 主要编译代价 |
| --- | --- | --- |
| FPCR 按需专门化 | 域选择 O(1)，每个实际 key 编译一个版本 | 累计请求数和代码体积随实际控制集合增长 |
| 浮点区间 | 融入发码遍历，O(N + W)，固定大小区间状态 | 少量分类与边界处理，环境维护发码量随区间合并减少 |
| effects 与覆盖写 | 复用 rewrite，固定表查询 O(U × N + W) | 每节点常量查询，删除依赖进入已有最终 DCE |
| 寄存器复用 | 复用 `AssignRegisters`，O(R × N) | 首输入检查及同一 bank 循环中的选择 |
| 冷区集中 | O(F) 固定记录与单次延后发射 | 一次容量预留、固定字段写入、实际指令发射 |
| 调用 stub 共享 | 固定键哈希，预期 O(F) | 哈希、唯一 stub 与调用点发码 |
| NZCV 活性 | 块内 O(N + W)，四位事实传播 O(4 × (B + E)) | 已有 CFG 上的有限工作队列 |

这些局部优化可以保持单个代码版本的线性或固定因子扫描结构。实际增幅还受 IR 缩减、保存槽、代码量、缓存分配和函数规模影响。

### 13.3 本次审阅后确定的实现方式

1. FPCR 采用实际访问驱动的版本生成。单态默认控制负载通常只触发一个版本，双态及更多控制组合的累计成本按触发数记录。
2. 浮点区间在已有 `Writer` 发码遍历中形成，状态使用固定字段。
3. effects 与写回消除使用现有八项表、use rows 和最终 DCE。候选表容量固定，逐条处理地址重叠和观察边界。
4. 寄存器复用采用最后一次使用的首输入和 bank 所有权交接，直接使用当前静态 home 模型。真正的寄存器淘汰继续具有独立的 spill 语义。
5. 冷路径使用定长记录和物理寄存器位图，容量由已有 allocation 遍历的计数确定。布局集中和共享 stub 各自产生测量结果。
6. flags 合流先采用规范入口，数据流只传播所需的四位事实，代码在分析完成后发射一次。

### 13.4 测量与规模验证

集成测量沿用普通 runner 的基础计时，细分检查放在诊断版本和独立 probe。实际归档保留以下指标。

- 冷调用累计 `compile_request_ns`、`compile_attempts`、frontend／optimize／backend／publication 时间，以及单元的 PC 和控制域。
- 合成输入的实际 IR 节点、块数、生成字节、frame bytes、真实 SSA spill、消除的 store 数和共享 stub 数。flags 保存槽与 helper 区采用 allocator 的实际布局解释。
- N、2N、4N 的编译请求耗时、输出校验和共享数量，用规模曲线核对现有遍历、固定表与 CFG 工作队列的实现成本。
- 稳定默认、稳定非默认、两域交替和完整控制 key 序列下的版本数、驻留代码与累计编译量。同一控制序列的再次热执行检查复用情况。
- FPCR 域切换的 L1 reset／重新填充耗时，记录在执行与切换指标中。

规模测试用控制过的 N、2N、4N 代码单元核对耗时与分配趋势。分别构造直线 FP、密集 uniform 覆盖、寄存器压力、冷路径密集以及多块／循环合流的输入。使用相同源输入记录实际 IR 大小，输出一并校验。

出现可重复的 compile 增量时，结合阶段计时、分配、发码字节和新增版本请求定位，收敛实现后复测。交付同时给出热执行改善和实测编译代价。最终定量结果见[实施结果中的编译成本](arm64-x64-mechanism-optimization-results-2026-11-12.md#4-编译成本与规模)。
