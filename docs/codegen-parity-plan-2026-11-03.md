# 生成质量追平 FEX：Uniform、寄存器驻留与优化流程方案

状态：第三十七批已通过本轮固定输入的量化性能验收。方案日期2026-11-03，进度更新于2026-11-04；实装范围、功能边界及完整结果见[实施进度](codegen-parity-progress-2026-11-03.md)。

本方案基于当前独立重建树与固定 FEX `a156752fae3a4f65fefb1ed95af980bcdfac43d7` 的实际代码及红魔测量。当前基线见 [生成质量对比](codegen-quality-comparison-2026-11-03.md)。原 `/Volumes/Data2/SwiftVM` 源码、Git/index/WIP 继续冻结。

本轮目标由“与 FEX 同数量级”提高为**正常执行的生成质量追平 FEX**，同时保持去纯字节解析的完整compile不超过FEX的2倍。第三十七批已在规定的原版Bionic/CoreMark输入、设备与配置下通过以下量化门限，依据是本轮实测及aggregate receipt，不用早期同数量级基线替代验收。

## 1. 目标与不可变边界

本方案起点的原版 Bionic/CoreMark 基线（最新测量见§4.17及实施进度）：

| 相对固定 FEX | Block | Function |
| --- | ---: | ---: |
| 完整 compile，扣纯 byte parser | 0.787× | 1.420× |
| 有效长跑耗时 | 2.448× | 1.937× |
| 用户态执行指令 | 2.754× | 2.075× |

本轮“追平”的工程验收口径为：相同完整输入、实际CPU条件和TSO配置下，**两种scope的运行时间、cycles及动态指令各≤FEX的1.10×，去byte完整compile各≤2×**。第三十七批八项均已实测通过，见§4.17与`batch37/measurement-receipt.json`。这是固定原版Bionic/CoreMark、红魔CPU4及记录配置下的量化验收，不外推全ISA或所有应用。精确故障合同的成本仍包含在整体测量内，没有性能豁免。

以下约束贯穿实施：

- **GPR 架构读写始终以 LoadUniform/StoreUniform 表达。**不增加 ReadPinnedGPR、WriteHostReg 等前端私有指令，不让后端依赖一份隐藏的前端 GPR 提交表。
- 允许语义等价的转发、合并和死写消除。保留 Uniform 接口不等于每条 guest 指令都必须留下冗余读写节点；所有剩余可观察状态提交仍能由中立 IR 解释。
- IR 继续为唯一 owner、24B Operation / 24B Block / 8B word；Submit 保持 O(1)。不恢复 HirValue/HirUse、MIR、actions、certificates 或第二份完整 IR。
- Block 维持轻分配；Function 保留真实 RPO、跨块值与回边生命周期闭包。不能把 Block 暗中改为 Function 再报告达标。
- 前端可按已知后端选择中立表达式，物理寄存器、指令编码、pin 亲和和真实 spill 决定归后端。Unknown/EarlyOff 路径保留。
- 继续保现有同步 fault 的 GPR/flags、TSO、W^X、撤链和 helper 边界合同。FEX 正常路径的省略不自动证明其满足 SVM 的故障状态合同。
- 不为了追平先增加测试平台、逐 opcode 证明、逐 helper PMU 或配置组合矩阵。按完整功能批实现，再集中验证。

## 2. 从 FEX 借鉴什么

| FEX 的实际做法 | SVM 采用的设计 | 不复制的部分 |
| --- | --- | --- |
| RegCache 保存逻辑值/宽度，最终产生状态读写 | 前端中立状态缓存，只管理逻辑版本和观察点 | 前端出现物理 GPR 或专门的 pin IR |
| Android 下 16 个 GPR SRA、PF/AF 固定载体、6 个动态 GPR | 普通 ARM64 native ABI 继续 pin16，优化亲和和覆盖时机 | 无证据先降 pin、再做 8/12/16 自动调参 |
| RFCE 按读位、覆盖写与 CFG 后继消 flags | 在现公共优化器中归约真实 flags 需求 | 新 PassManager、通用 Facts、按 producer 回放的历史图 |
| SRA affinity、释放 killed 输入、冲突前保存旧 SSA | 同一分配 owner 处理 Uniform 绑定、快照、tied destination | 多张 preparation/repair/action 表反复转换 |
| PF/AF 用 raw carrier，真实观察边界再打包 | ARM64 原生 flags 路径集中维护 raw 状态 | ARM64 载体编码进入中立 flags IR |
| 近直接分支、内联间接查表、cold miss 发布状态 | 原 links/PC 目录内收敛热边及发布 | 新 RET cache、第二套 PC cache 或链接 manager |

FEX 的动态临时 GPR 池与当前 SVM 同为六个。重点是减少值搬运、死旗和边界工作，而不是先改变寄存器数量。

## 3. 重构后的主流程

```text
decode / formation
  └─ x86 语义发射 + 有界逻辑寄存器缓存
       └─ 中立 Uniform IR，Known 可早选中立表达式
            ↓ 同一 Program owner
B：公共归约
  逆向活值/局部 flags → 必要的块域 flags 归约
  → 前向折叠/Uniform 转发与覆盖消除 → 最终逆向 flags 需求与 DCE
            ↓ 同一 Program owner
C：实际 ARM64 后端
  Block：last-use + Uniform 绑定窗口
  Function：RPO/跨块闭包 + 同一绑定/分配核心
  → 一份 Home → 直接选码、发射真实 spill/branch/fault 元数据
            ↓
D：原执行 owner
  发布 → 同一 PC 目录/links → native 执行
  cold/helper/syscall/fault 才物化相应状态
```

前端决定逻辑值，B 决定语义上哪些计算还需要，C 决定值实际放在哪里以及怎样编码，D 决定真实代码的发布和生命周期。一个阶段不为下一个阶段重新包装整份决定。

## 4. Uniform 合同及前端缓存

### 4.1 统一接口

```text
old = LoadUniform(RAX, U64)
sum = Add.u32(view<U32>(old), 1)
StoreUniform(field<RAX,U64>, view<U32>(sum))
```

最后一条表示完整 RAX 更新，值按 U32 读取后零扩展。不能替换为只写内存四字节的 StoreUniform<U32>，否则 generic backend 会保留旧高32位。

| x86 访问 | Uniform 语义 | ARM64 可用实现 |
| --- | --- | --- |
| RAX 读/写 | U64 全字段 | 借 pin、直接结果、必要 MOV 或 memory |
| EAX 读 | 低32位读 view | W 读口/UXTW，不强制先造一个临时寄存器 |
| EAX 写 | U64 字段 + U32 输入 view | W 结果天然零高位；generic backend 完整写 U64 |
| AL/AH 写 | U8 字段，offset 为父字段+0/+1 | BFI 或窄 memory store，保留其余位 |
| AX 写 | U16 局部字段 | 保父字段未写位 |

一份 LoadUniform 的 Value 是不可变快照。后续 StoreUniform 改写 pin 后，旧 Value 仍须保持旧值。读取旧快照跨越 pin 覆盖时，保留必要 MOV/栈位置；不能用“guest 寄存器当前在 pin”替代 SSA 语义。

### 4.2 前端缓存的保留与收敛

保留固定16槽、按块清理的逻辑版本缓存，避免先大量出生 Uniform 再交 B 删除。它记录 Value、逻辑宽度和未观察提交，既不记录 host register，也不产生“后端自行理解”的架构更新。

这里的逻辑宽度必须表示**对架构父值的覆盖范围**，不能只取 SSA 自然 type 或 dirty 状态。初始 RAX=`0x12340000_ffffffff` 时，读取 EAX 所得 U32 只覆盖低32位；后读 RAX 仍须取得原高位。写入 EAX 后的 U32 则描述完整、已零高位的 RAX，Publish 清 dirty 后这个事实仍成立。用固定16槽的完整父值资格/覆盖信息表达即可；窄读只产生 view/normalizer，不替换已有完整64位父缓存。

- 首次读取通过 LoadUniform；合法命中返回同一个逻辑值。
- 未观察的同字段完整覆盖可在出生前合并；实际 fault、helper、syscall、外部退出等观察点前仍通过 StoreUniform 提交。
- 不再为了换读 view 就强制“StoreUniform → LoadUniform”往返。能从现逻辑值取得的 view 使用中立截取/扩展，C 按消费者直接选码。
- AL/AH 等局部写保明确的窄 Uniform 语义。已知完整父值下的单项pending partial见§4.4；不建立字节级寄存器 SSA、通用 known-bits 图或部分写修复平台。
- 缓存合法性由实际状态的别名/观察合同决定。无 private-state 合同的调用者遇可能影响状态的操作就提交/失效；不能因为 backend 未知就改变 Uniform 的含义。

B 仍处理经过折叠或其他输入产生的残余 Uniform 冗余。前端缓存不重复维护 B 的 use 账；B 也不再建立一份前端寄存器版本图。

### 4.3 Known、Unknown 与运行 ABI

| 前端模式 | 后端实际绑定 | 行为 |
| --- | --- | --- |
| Known | ARM64 pin ABI | 紧凑的中立输入；Uniform 可落 pin |
| Unknown/EarlyOff | 最终选中的 ARM64 pin ABI | 普通中立输入；后端仍可对同一 Uniform 绑定 pin |
| 任意 | 无 pin binding | Uniform 落内存，使用与 memory state 匹配的入口/退出 ABI |

**前端不知道 backend，不等于最终 backend 不能 pin。**反过来，仅关闭 PinnedSlot 而沿用内存已陈旧的 pin16 执行入口是不正确的。无绑定路径复用现 standalone/memory 调用合同，不给普通 native 热路径增加模式判断。

### 4.4 已实施：完整父值与单项pending partial

第二十四批仅在Known/private的原16槽GPR缓存内，复用现parent/view/first记录并增加固定u16 pending标记。只有已知whole U32/U64父版本、写入源为精确同字段自然宽度的canonical U8/U16时才延迟局部合并；raw BitCast/GetResult/GetFlags及未知/外部/控制源仍走原路。同字段读可直接消费pending值，同字段新写可覆盖未观察旧写；不同局部字段/不兼容读先物化一条中立BitInsert。完整写覆盖可丢旧pending，真实fault/helper/终结等观察前仍统一物化并用完整U64 Uniform发布。U32父版本继续保持EAX写的完整零高覆盖；不把仅有低32快照当完整RAX。Unknown/EarlyOff保持原路径，不新增跨后端pin语义或N级状态。

C仅在BitInsert自然结果U32、count>0且字段完全落在低32内时选W-BFI，用实际W写完成清高；其余宽度、零count和越界形态保原检查/fallback。没有将BitInsert加入ReadsBeforeWrite；parent复制有实际用途，不能随同W选码盲删。

冻结24两CRC入口离线N164/272→147/250、words129/201→125/193，单B且frame/spill0。实际新BitInsert为2/1处，均为MOV parent加W-BFXIL；其余出生合并少的loads/stores不等于等量少机器访存，原部分Load本已借pin。Home和寿命有取舍，首CondSelect可能失去pin而用临时，byte XOR活到后续TEST。严格自然宽度门仍使U32容器产生的CL8写fallback，crcu8/crcu16分别残余6/7组RCX partial写，不能称全部partial已延迟。离线词流未与运行RX新增逐字绑定。

Host五项、Android1992及原合并检查集中通过。首次新增fixture用了未支持的address-size32 LEA，先导致Host新场景失败；仅将fixture的`67 8d 08`改成已支持的64位地址/32位结果`8d 08`，生产和runner ELF未变。旧fixture、manifest及失败日志完整保留在`batch24/attempt0`，不把它描述为生产修复。

有效长跑Block20.279s、Function18.786s、FEX15.102s，运行约1.342802×/1.243941×，cycles1.342324×/1.245317×，指令1.298374×/1.164807×；完整去byte compile46.633653/82.380712ms，对FEX47.521719ms为0.981312×/1.733538×。20次诊断的全应用spill slots23/40→25/42、最大frame32/48→48/64B，native bytes321692/534256→321708/534248，不能用两个离线frame0单元盖掉这些成本。两scope compile合格，运行/cycles仍未进入1.10×追平容差。完整身份/原始结果见实施进度和`batch24`；第二十五批宽源pending及其有限消费者归约见§4.5。数值carrier延迟归一化候选暂缓，未生产实现。

### 4.5 已实施：pending字段独立于源宽度，消费者归约与安全复用

第二十五批沿原固定16槽pending记录单独保存写字段宽度，允许合法canonical整数宽源提供U8/U16局部写；完整32/64父写仍须满足原精确语义。Read同partial时，源自然宽度不等于字段宽度就出生中立BitExtract，只保该字段低位；物化BitInsert也以字段宽度为准。宽源不会冒充完整父版本或已归一的窄缓存，raw BitCast/GetResult/GetFlags及未知/外部/控制来源保持原fallback。观察、全父覆盖与Uniform发布合同不变。

B在原forward fold增加有限嵌套clip归约：default、同B、sole-use/nonpin的BitExtractU32(first,length1..8)，被BitExtractU8(...,0,8)消费时，保原raw parent/view和field，外层直接取原字段，第三逆DCE退inner。没有新扫描、use表或范围分析，也不改变U32 shift count合同。

BitInsert加入原ReadsBeforeWrite名单；C在复制parent前，如insertion实际reg等于destination且parent不同，先用完整X17保存insertion，再执行copy/BFI。保Field/raw/width/partial/NZCV合同，W-BFI仍仅收§4.4的原有限范围。原samepoint最后读及pin真实publication窗口据此复用输入，不另造constraints/repair；保护所有width，但不放松原动态U32/U64复用门。既有压力合并用例包含满bank、parent仍活、RHS此点末用的X raw高位与W清高两种边界。

两个冻结输入离线N147/250→135/236、words125/193→107/173，frame/spill0。原6/7组RCX中间发布/回读消失；嵌套clip真实命中6/7处，MOV/BFXIL/UBFX各少6/7，crcu16另多1条alignment Nop。TST/CSEL/LSR/PSTATE维护不增加。新XOR输入已有canonical U8，直接适用原normalized_logical，不能把它称为deferred数值carrier。剩余2/1个BitInsert仍为parent copy加W-BFI，目的pin不等于RHS，没有新增X17保存，也不能用这两CRC认领同点复用收益。Home局部变化，离线结果未新增运行RX绑定。

Host五项、Android2100及原合并检查集中通过。有效长跑Block20.271s、Function18.649s、FEX15.079s，运行约1.344320×/1.236753×，cycles1.342960×/1.237836×，指令1.290989×/1.157408×；完整去byte compile46.632854/83.014875ms，对FEX48.220365ms为0.967078×/1.721573×。相对24，Block耗时实质持平、Function约少0.73%，指令少约0.57%/0.64%但误预测增加。全应用诊断native bytes321708/534248→321756/534004，spill仍25/42、最大frame仍48/64B，未收回24增量。整批三个改动不各自认领动态收益，compile合格但1.10×追平目标仍未达到。后续地址与真实消费者改进见§4.6；deferred XOR carrier和扩大PF目的位未实现，不扩张恢复历史或编译框架。

### 4.6 已实施：一个真实地址复用与实际consumer裁剪

第二十六批前端仅在Known/private、同B的内部pure Address构造口保留一个已有GetOperand NodeId；原actual cells不超过4且严格相同才复用，类型、Value/view、literal与expression等实际输入均不按guest寄存器名猜等价。metadata由该内部pure构造合同保证，不加另一层反复分类或全程序验证。B边界清理，非域保原出生；不是地址字典、第二份EA计划或N级缓存。复用同一不可变SSA地址会延长末用，原分配/发布继续计费。

C在窄Add/Sub已经需要aligned NZCV的8/16位bare Value/Immediate路径中，复用原低位读取：Value的min(view宽度,CarrierBits)覆盖结果宽度时直接取physical，由左UBFIZ W和右ADDS/SUBS W移位完成实际裁剪；更窄view（如Bool）或carrier仍走Read补零。AF仍在移位/输出写前只捕获bit4；metadata/legacy、Prepare/Finish、partial保留、完整numeric结果及PF合同不改。32/64、compound和非aligned路径保持原样，不新增数值carrier模式。

V128 TSO store的真实Add/Sub(base,ImmU64)、无shift地址，仅当有效disp非负、16整倍且<=65520时直接Read(base)并将byteOffset/16交STR Q scaled12；这是普通STR Q能力，不依赖RCpc imm9。负、不整16、超界或其它形态仍计算完整EA；原DMB、Shadow、实际FaultSite、scalar imm9及alignment恢复语义不变。

冻结26三入口离线：dot total words400→398、fast回边29→27；mul-const423→421、fast25→22（实际ADD少3但对齐NOP多1）；state157→157（实际UBFX少1、cold对齐NOP多1）。三单元均frame/spill0，不能替代全应用压力数据。矩阵复用地址末用确实延长；除对应ID映射/坐标外的可比打印Home位置字段未新增copy/spill，state IR/Home与25逐字节相同。原两道vector-store TSO屏障、guest访问和fault recipe保留。没有新增运行RX绑定，不按静态循环长度外推动态收益。

Host五项、Android2116及原合并检查集中通过。有效长跑19.972/18.467s、FEX15.039s，运行1.328014×/1.227941×，cycles1.326189×/1.229123×，指令1.261880×/1.134608×；完整去byte compile46.380543/83.198837ms，对FEX48.097813ms为0.964296×/1.729784×。相对25耗时约少1.48%/0.98%、指令少2.25%/1.97%，但误预测增加；不把整批结果拆认某一hunk。全应用出生N36638/86893、native bytes321308/533272，spill仍25/42、最大frame48/64B，未收回24增量。compile合格，1.10×运行/cycles追平目标仍开放。其后第二十七批窄flags-only CMP与zero-RHS的已测结果见§4.7；occupied查表变体、扩大PF目的位及noTouchCarrier等已排除候选未采用。

### 4.7 已实施：C/Z窄CMP与ALU有效零RHS

第二十七批C仅在NativeNZCV SubU8/U16、无数值consumer、非legacy、已支持算术metadata/非branchOnly，actual mask为Z或C|Z时，允许CMP W直接给出相同C/Z。两输入按实际view/width零扩展；Value RHS要求双方均为现Read可免cut的numeric输入，脏borrowed pin/窄raw容器保原aligned fallback。Immediate按min(view,width)归一且imm12/shift12合法才直编，不负数翻CMN。原Prepare/ArithmeticFlags(31)/CFINV保未写N/C/V及raw PF/AF，saved跳死数值spill；并不收回已经安排的其它spill/frame。

同一组Add/Sub/And/Or/Xor/AndNot的bare Immediate归一后有效低width为0时，仅在原fallback使用shifted-register Rm31=ZR，避免MOV scratch,#0；AF xor、结果cut、full/partial flags和原fallback不动，不全局改Read或地址0/SP。没有IR、alloc、N表、pass或新验证层。

离线crcu16 N120实际3→1：LSL+SUBS+UBFX改CMP W6,W4，words173→171，原CSEL、MOV21/TST14和NOP1保留；IR/Home相同，frame/spill0，fault recipe不变。state21c380完整157words相同，full95 CMP不退PF/AF。两单元zero-RHS目标0，不能认领它们的MOV消除。26的缺失CRC基线仅由已封薄ELF独立补发，不是运行RX；参见batch27/native-quality-review.md。

Host五项、Android2146及原合并路径通过后，原Bionic有效长跑19.985/18.167s、FEX15.114s：运行1.322283975×/1.201998147×、cycles1.320038194×/1.203067583×、指令1.260893168×/1.131652213×。完整去byte compile46.240713/82.477986ms，对FEX47.488645833ms为0.973721448×/1.736793807×。原始整数周期22,935,096,212/20,902,782,125/FEX17,374,570,153，指令78,152,064,874/70,141,515,103/FEX61,981,511,890，branch-misses97,861,966/45,485,830/FEX18,916,463保存在summary及进度表。Function较26时间约−1.62%，指令仅−0.26%、miss约−9.4%；Block绝对时间基本持平，不孤立归因单hunk。native bytes321220/532280、spill25/42、maxframe48/64B，完整压力/retry/发布/释放仍收费，1.10×目标未完成。

首次Host/phone fixture因不可表示窄literal在IR append处失败且未进性能；仅修fixture类型，不改production/schema，旧manifest/test/log/runner/cleanup留attempt0。最终27 source0c9edac17b07d59ffe2ef021edaae3e260506418470ae6bf7a2ff5341523ab97、runner1152f18a43cf3a6f8ebb9a73f152cc87b5eff14f966c40f0152a7b892f095056。最终cleanup du37,471,232B/df＋37,531,648B/remaining0无active或errors，不能加上attempt0独立回收数。测量后文档另存post-measurement-docs，不改as-built摘要。

第二十八批中立VecSignedMulWidenLow/capability的已测结果见§4.8。其它排除候选仍不采用；不扩ARM专用IR或分析平台。

### 4.8 已实施：中立连续低半有符号宽乘

第二十八批新增`VecSignedMulWidenLow(Value,Value,Imm src_lane_bits)->Explicit V128`：完整输出连续低半源lane的有符号全宽乘积，src8/16/32，flags不变；旧`VecMulWiden`的偶数lane语义不改。中立capability `VectorSignedMulWidenLow`用bit5，由KnownTargetInfo广告，实际target features63。Unknown/EarlyOff和缺能力的Known输入保持原图，不以ARM物理opcode或pin身份代替公共语义。

B仅识别同B/default metadata的`VecMadd16(VecExtend(a,16,32,0),VecExtend(b,16,32,0))`，要求自然/有效V128、至少一个Ext真实sole-use且非pin。零扩后16位lane为`[a0,0,a1,0,…]`，所以旧有符号pair-sum恰等于原父低四H的有符号32位乘积；不改成unsigned，也不融合后续Add。保旧Madd ID/type、原父read view和所有Add/Uniform消费者；多use左Ext及故障前完整XMM发布保留，sole右Ext由原第三DCE退。沿原u64 rows、forward统一Rewrite/use账执行；非tail的2→3编辑实际追加3words，以product和独占Ext的4words额度支付，原`oldW+2N`上限不变。scalar Fold不消费V128，没有新pass/N表/owner或布局变化，IR仍24/24/8、Home16B。

冻结28两真实Function入口离线：dot N428/B27不变，两个pair各5→2，热回边27→21，whole398→392；保留两USHLL及原Add/Uniform，USHLL、SMULL2和ADDP各少2条。原raw父last-use延长由实际新edge进入分配，本单元没有新增MOV/spill，frame0；不是一般保证。mul-const仍sign1扩展后VecMul32，不触本规则，421words及IR/Home逐字节不变。原fault guestPC/配方和link目标保持，机器坐标随真实布局变化；仅离线生成，没有新增运行RX绑定。报告`batch28/native-quality-review.md` SHA `4ac9e02d44f45962253e4154728faeaef94fabd4102c859e4aafccab195ebe2e`。

Host五项、Android2178及原合并检查集中通过。原Bionic有效长跑19.796/18.217s、FEX15.087s：运行1.312123020×/1.207463379×，cycles1.311559116×/1.208570686×，指令1.260545882×/1.131307186×；完整去byte compile47.028893/83.742158ms，对FEX48.503697917ms为0.969593970×/1.726510794×。周期整数22,732,665,722/20,947,613,471/FEX17,332,551,348，指令78,130,539,123/70,120,129,365/FEX61,981,511,513，miss96,613,315/43,959,824/FEX18,746,731均保在summary/进度。相对27全程指令各只少约21M（约0.03%），Block耗时−0.95%、Function反而＋0.28%，不能把局部22%少词当整体收益。全应用native bytes321196/532256、spill25/42、maxframe48/64B；未收回24的压力增量。

源码摘要`d8508058631d4db61ea8b4e8609d7eab1ecf51f21b3f2b21ba5bd49908bc2f4b`、runner `3bf74d5957deea6a57cf9af2ce60d5585da0d97d90776a7cf70e7f3800e7627f`已封存。cleanup du37,553,152B、df＋37,613,568B、remaining0，无active/openfiles/errors；测量后三文档独立归档，不重标as-built身份。完整compile继续只扣byte parser调用，形成/失败/retry/发布/释放收费；两scope均≤2×，1.10×运行/cycles目标未达到。

其后独立directory target census和冷layout重放已闭合，并据实际触达次数形成第二十九批固定hash与窄mask改动，见§4.9。诊断耗时不作普通PMU基线，不继续仅凭静态局部删词比例排序。

### 4.9 已实施：同目录固定shift13与窄mask消费

第二十九批仍只有一个PC目录、同一EOR mix和原capacity/occupied/ready-pending优先规则。原私有共享常量`DispatchHashShift`从11改13，C++ Index和inline Writer本已读同constexpr；四个shared naked dispatcher改用named immediate引用它，不增加hash mode/cache、Context、slot或热feature分支。实际NDK29 Android构建和unstripped ELF反汇编确认四shared symbol均为单条`ca403405 EOR X5,X0,X0,LSR #13`，没有常量materialization或新增指令。slot32、counter、链接撤销/SMC和生命周期不变。

依据是28隔离计数和唯一两次20输入的冷event，不是采样IP份额。Fn热target21c483有147,999,999次ready hit，原真实final slot187冷25a40f在slot188热21c483前；独立重放shift11的全部4654/1546冷event、每slot occupancy/PC/ready/code_id精确匹配。按v1长run ready频次加权v2 final layout，11→13模型slotreads为Block612,912,389→612,412,387、Function761,312,395→612,812,405，净少500,002/148,499,990，已计入Fn两项399,999次的1→2退化。pending全部参与Grow和ready抢位，未将同primary桶直接冒称实际链。该模型不是全程历史probe/周期重建，v2零频冷key与v1也有形成差异；全部正频权重均覆盖，不能据此直接认领绝对动态G或时间。

另有**真实20次诊断**：相同lookup125418/124098，probes由125889/154083变125740/124403，平均1.003755442/1.241623556→1.002567415/1.002457735；这包括冷/非ready尝试，域不同于上述final-ready模型。三次fresh记录一致，但byte时钟/计数有扰动，不作普通长跑耗时基线。两种证据及正式长跑分别保留。

同批C只在窄AND immediate可重复成32位logical mask时，保原Read归一后直接用W AND；不是让脏高位任意穿过numeric语义。B将精确U32低byte `BitInsert(parent,And(BitExtract(parent,0,8),mask),0,8)`改为同ID `And U32(parent,ffffff00|mask)`，保完整父bits8..31、原view/元数据和其它child消费者，沿原forward/Rewrite/use/末DCE，不加pass/表/预算。

六个真实target/scope各离线一次：21c483 B33→31/F170→168，两规则各少一word；输入UBFX8和完整AF/PF/CMP链仍在，仅outer BitInsert变AndU32及inner last-use4→3，实际reg/spill不变。21c473 B27、21c3c0 B31的IR/Home/words逐字节相同；两F仍164/170words且IR/Home相同，只有各3个hash EOR11→13。六个单元均frame/spill0，fault guestPC/配方和link target保持，只需21c483坐标前移2；没有新RX绑定，不用whole Function words乘hits。报告`batch29/native-quality-review.md` SHA `b4121647aa78eca14e11ec27b6eab2d6030c38c6e9ccdf58bf650d0c0cdfdfcb`。

Host五项、Android2209和原12/4170、formatter、SMC、EOF及合并故障/flags检查通过。原Bionic有效长跑19.902/17.492s、FEX15.036s：运行1.323623304×/1.163341314×，cycles1.322231413×/1.164423029×，指令1.253026079×/1.104876630×。周期整数22,848,587,260/20,121,607,249/FEX17,280,324,036，指令77,664,466,263/68,481,937,579/FEX61,981,524,204，miss101,256,504/33,969,965/FEX18,930,381。Function比28耗时约−3.98%，Block反而＋0.535%、没有运行收益；同批hash/两窄规则不拆认贡献。完整去byte compile46.678645/83.703110ms，对FEX48.390468750ms为0.964624774×/1.729743732×，两scope仍≤2×；运行/cycles仍未达1.10×。

20次出生N36638/86893、words65804/152552不变，folded50/117→52/126、erased634/1418不变，native bytes321196/532256→320892/531672。全应用spill仍25/42、maxframe48/64B，未解决24增量。完整形成、IR、失败/retry（Function722＋Block24）、发布和释放均收费，只扣实际byte parser调用。

80文件as-built摘要`406e095f868be786440684ab8aaa8e7d1948be1d8433104e624a9c1fc55f642f`、8,402,216B runner SHA `caeeb28a84e0ef824fdcaebccfda9d0d32904889eae42d2f8ced8c0995c40238`已封。cleanup du37,598,208B/df＋37,658,624B/remaining0，无active/openfiles/errors；post-measurement-docs另存，不修改80文件构建身份。29当时已封存，后续继续按实际剩余热点与执行次数决定，没有新增已承诺的生产机制；不采用排除的occupied/PF/noTouchCarrier等候选。

### 4.10 已测中间点30：完整窄CMP与前向条件边

RawAF模式的SubU8/U16、full All flags、无数值use、非legacy且bare immediate可编码时，保实际view/carrier裁剪，使用`SXTB/SXTH W16; MOV/MVN AF; SUBS/ADDS W1,#imm; SETF8/16`替原五词完整比较。W29保AF输入bit4且清TEST tag，W1低byte保PF；SETF给窄N/Z/V并保正确C，原ArithmeticFlags/profile仍消费。不是根据后续Jcc退PF/AF，generic/unsupported形态原路。原fixup行同时支持B26/imm19，前向B.cond/CBZ可直达实际label，保原需要restore/flip的路径；真正imm19超界时清未发布code/sites/links，用同Allocation/容量重新发长形式，费用仍计入compile。

同六入口IR/Home逐字不变，三个Block各fullCMP5→4；对齐使whole31→29、27→27、31→29。三个Function各三处fullCMP，另少5/5/7个forward B26，whole168→160、164→156、170→160。MRS3/RMIF3/CFINV2保留；这六份没有新增CFG CBZ命中，15个CBZ/CBNZ均在原inline目录。fault配方/实际访存和cold回hot-next、link target保持，全部局部frame0不替代全应用spill。报告`batch30/native-quality-review.md` SHA `d5c8f33c280c5ff268feb0c2d64d88ea0738082978b622c099673dd24828a07a`。

30有效长跑19.418/16.621s、FEX15.085s：运行1.287238979×/1.101823003×，cycles1.287297098×/1.103126393×，指令1.236497390×/1.069021426×。去byte compile46.835537/84.421017ms，对FEX47.787031250ms为0.980088860×/1.766609366×；Function的time/cycles此时仍未到1.10×。Host五项、Android264408累计checks及原合并路径通过；大fixture按Emit/Need/Constant累计，不是264K新语义case。首次build只因测试wrapper未限定Compile发生ADL歧义，未启动phone；仅改为`::Compile`，80文件manifest仅test一项变化、production不变。旧a83bcd71…身份及logs/test/status保`batch30/attempt0`；最终source `9714719ead011aae97dc24f44f48dad5e2142bbc15df0c750e4046598045e4aa`，runner `9cc7fc85ab1cd7d28d2092be51c0d862236553d4a3c2ddb8c8b847d723ca3434`（8,413,712B），cleanup du37,655,552/df＋37,715,968/remaining0无active/errors。30作为已测中间点保留，不单独重标文档as-built身份。

### 4.11 已实施31：实际branch位置与窄模运算

LinkSite仍16B，第二u32由旧fallback_word改为真实`branch_word`；fallback固定派生`pointer_word+2`。奇数尾把原热NOP移到BR后、对齐DATA前；近链按实际branch patch B，远链BR也跳过pad，DATA/冷stub大小不变。CodeArena/撤链/SMC仍同一owner，不能再由pointer−2猜branch。无flags的U8/U16 Add/Sub按模宽使用W运算及原末尾cut，较窄logical view/carrier仍Read fallback；不改IR、Home、bank或flags ABI。

同六入口IR/Home仍逐字同30：21c483 B29/F160→29/158，21c473 B27/F156→27/154，21c3c0 B29/F160→29/160。21c473首linked tail真实branch6→5，DATA8/fallback10不动；整单元词数/NOP不减，只让执行绕开pad。两个Function各一处AddU8#c6从MOV literal＋input UBFX＋ADD变SUB W#3a，原结果UBFX保留，净少2。其它挪动的pad是cold未链接尾，不认热收益。全部FaultSite配方、fast/cold访存与hot-next、link target保，坐标真实重算；局部frame0不覆盖全应用。报告`batch31/native-quality-review.md` SHA `38c4ceb2b35906cc752fdbb1f97c36ba4a1b16e0c4d1672d9d797306003367ac`。工具同步branch_word和派生fallback，并识别LDR＋8/＋12两种真实DATA格式，无新RX绑定，不将whole words乘entries。

31有效长跑19.386/16.428s、FEX15.057s：运行1.287507472×/1.091053995×，cycles1.285478844×/1.092661540×，指令1.192681451×/1.059333596×。周期22,255,808,306/18,917,515,352/FEX17,313,243,545，指令73,924,247,367/65,659,140,331/FEX61,981,551,993，miss101,087,334/21,242,893/FEX19,411,790。**Function本轮time/cycles/instructions进入1.10×，Block未达，两scope总目标仍开放。** Block较30指令少约2.716G但耗时仅−0.165%、基本平；Function耗时−1.161%，同批不拆认hunk收益。去byte完整compile46.855298/83.757012ms，对FEX47.809062500ms为0.980050550×/1.751906597×，两scope继续≤2×；失败/retry、形成、IR、分配、编码、发布/释放及真正范围fallback仍收费，仅扣实际byte parser。

Host五项、Android264431累计checks和原12/4170、formatter、SMC、EOF全部通过，非新增264K语义矩阵。N36638/86893、words65804/152552、folded52/126、erased634/1418保持，native bytes320708/519148；全应用spill25/42、maxframe48/64B依旧未解决。80文件final source `4b5913893c7e84e802613a8a8bf8dace7abd743d9e62404177d9237c13884eb2`，runner `a18350eadec631faaa836c579e191cacce9ab332fee982127b9e255141b0f068`（8,413,512B）；cleanup du37,680,128/df＋37,740,544/remaining0，无active/openfiles/errors。测量后三文档存`batch31/post-measurement-docs`，不改30/31冻结80文件摘要。

新的all-entry诊断与旧directory-ready计数不同域，静态链接也可进入前者。Block真实top含2197e0、21c3aa/3b3、21c4dc/3a0/4d0，已生成六个实际Block，不伪造同PC Function。2197e0离线LDR/BR不能当resolved warm：near self将LDR patch为B，非零热路是7条、末CBZ＋B，不是旧8条模板。现仅分域定位实际prefix/flags/conditional tails；诊断elapsed不作QoR，不因entry次数×整单元词数宣称收益。此为31的历史checkpoint；后续已测32/33见下文，未实施其它排除候选。

### 4.12 已测中间点32：实际条件self head与完整byte TEST

只沿真实LinkSite把合格self回边变为CBNZ head，gate位于DATA前的偶数位置。2197e0 near ready热路由31的七条（末CBZ＋B）变六条（前五条＋CBNZ直entry）；zero和self冷边仍先完成真实TEST，snapshot12及原两fault site保留。full byte TEST在无数值use时以SXTB X1＋MOV W29＋CMP形成精确PF/AF/NZCV，X1只是raw carrier，不给U8数值用户授脏高位。没有新IR、Home字段、恢复历史或目录。

32普通有效长跑19.087/16.429s、FEX15.237s，time1.252674411×/1.078230623×，cycles1.251072790×/1.080531939×；完整compile47.087707/83.528582ms对FEX48.122447917ms为0.978497750×/1.735750894×。Function仍在1.10×、Block未达；相对31 Block自身time−1.542%，Function几乎平，FEX自身time约+1.2%，不把分母变化当Function生成收益。完整整数表见进度§32。

仅四unique实际unit离线生成：B2197e0/B21c4d0及Fn21c380/Fn21c483，Known/a64/63、原guest/policy。前二IR/Home同31，词数39→40/29→27，不拿整单元词数代替热路径。Fn21c380无31同PC产物，只报actual prefix；Fn21c483 IR/Home/158words逐字同31，未扩scope。fault/zero/linktarget保，偏移按实际重算，局部frame0不泛化。报告`batch32/native-quality-review.md` SHA `b81fe4e2457b85f9e36170c082afcfe80051b4c8cbeaf8776b0fdbe091331a3f`，没有新RX逐字绑定。

Host5/Android264446累计checks和原合并全通过，源码`3ab60a40cbf7e2fb52db57ac3f2840fa1ac6b34122bb6628f26df96773087108`、runner`721e029f8ce0f41583c6a6de93caf1ef1e265c40d237c1726672acfa7898bb05`（8,417,720B）。三cold样本N36638/86893、words65804/152552、bytes320192/518324，spill25/42、maxframe48/64B未解。cleanup du37,696,512/df＋37,756,928/remaining0，无active/openfiles/errors；文档与33一次归档，不重写as-built身份。

### 4.13 已测33：真实条件边的单次near relocation

Block21c3aa/21c4dc/21c3a0分别用B.LS/B.NE/B.NE早期head记录主边，真实偶数late gate仍保留。ready且imm19可达时直接patch head到目标：主选中边原conditional＋patched B两分支变一条；未选中边保普通link，far/unresolved保gate。LinkSite16B和fallback=pointer+2不变，不能把head当gate，也不能把离线LDR/BR当已解析warm。

三个实际Block只各生成一次，IR/Home同31、full95 metadata/prologue不变，长度25/25/27、frame/spill0，均无fault site；其中两份未发布词流逐字同31，21c4dc仅条件方向/目标DATA顺序变化。新收益来自relocation位置，不是body删词。报告`batch33/native-quality-review.md` SHA `602cfbe5ff769ebd26d15a6fb5bf8ae25f1344ee6fba28370f44c1d09d4f88be`，五库身份闭合已释放，仅offline无新RX绑定，完整words×entry不作动态估算。

33有效长跑17.126/16.448s、FEX15.079s，time1.135751708×/1.090788514×，cycles1.134244255×/1.092157015×，instructions1.101186004×/1.058641344×。周期19,663,141,820/18,933,521,755/FEX17,335,897,221，指令68,253,163,902/65,616,181,953/FEX61,981,503,284，miss44,849,209/19,056,284/FEX18,740,247。去byte完整compile47.634095/83.521265ms，对FEX47.644322917ms为0.999785328×/1.753016097×。**Function本轮time/cycles仍过1.10×；Block未达，总目标开放，两scope compile≤2×。** 相对32 Block自身time−10.274%，Function+0.116%基本平；两批各自FEX分母保留，不拆认单hunk收益。

33三cold Block样本attempt2045/2042/2045、N36672/36638/36672、words65867/65804/65867、bytes320780/320192/320780，不能硬套旧2042规模；Function每次746attempt=722Function＋24Blockretry，N86893/words152552/bytes518324不变。失败/retry和所有形成、IR、分配、编码、发布/释放继续收费，仅扣纯byte库调用。spill25/42、maxframe48/64B仍未解决。

Host5/Android264468累计checks及原12/4170、formatter、SMC、EOF等全pass；不是新增264K语义case。源码`8d1b2d0a606ea32291c2a3060f9f3664c64a627e24558be3a4d9303fb0b3df86`、runner`8060f0d483d94029b47420480f333627606824bbeb7c37de7e0530ca49fdd67f`（8,418,352B）。cleanup du37,729,280/df＋37,789,696/remaining0，无active/openfiles/errors。33当时已封存；32/33三文档另存`batch33/post-measurement-docs`，不改变两个80文件已构建摘要及旧失败/重试证据。

### 4.14 已测34：真实callable目录省ready门

仅Runtime实际CodeCache提供`CompileOptions::callable_directory=true`：slots/mask始终有效，empty/pending/ready均有可调用的cold/native载荷；公开默认false继续保null guards。沿原Inline路径删CBZ slots和匹配后的CBZ entry，首槽ready10→8条；mismatch仍LDRB occupied＋CBZ empty。原entry快捷分支也删，额外ready碰撞循环8→9条，不能按每次probe省两条外推。仍为同一目录/owner、无新IR/pass/Home/bank，真实保留generic ABI。

只生成B21c3b3/B21c4e5/Fn21c483各一次，Known/a64/63、原guest/policy且JSON明确打印callable true。两有旧产物者IR/Home逐字相同，words33→29、158→148，fullflags/raw数值前缀保；B21c4e5无旧同PC仅报actual32words，未补编。三unit frame/spill0不覆盖全应用，fault guestPC/TEST/PF/carry recipe及actual fast/cold前两字保，布局重算，无新RX绑定。报告`batch34/native-quality-review.md` SHA `50a0824409c4927d7f505afff3b88d58f6372ed59d70393a6b9ef22507b298b1`，库身份闭合已释放。

34有效长跑16.885/16.058s、FEX15.097s：time1.118434126×/1.063655031×，cycles1.117177195×/1.065082166×，instructions1.081445141×/1.038905175×。去byte完整compile47.879109/84.502106ms对FEX48.133177083ms为0.994721560×/1.755589619×。**Function继续过1.10×，Block仍未达，总目标开放；两scope compile≤2×。** 相对33自身time−1.407%/−2.371%，FEX也变，不拆认hunk收益。完整整数见进度§34。

三cold Block样本均2045attempt/N36672/words65867/bytes318516；Function746attempt=722Function＋24Blockretry、N86893/words152552/bytes513740。spill25/42、maxframe48/64B未解决；失败/retry/形成/编码/发布/释放继续收费，只扣实际byte调用。Host5/Android264478累计checks＋原全部合并路径pass，不是264K新语义case。80source`c176c015f5d5ed0d79f0ed7b478def0869a40e93c66971ee324280c150a8eb58`、runner`365511fea7fa0fd58a1d9ce63f3cb7b5d81206cad6410fcd11e5c632a4453b19`（8,420,272B）；cleanup du37,749,760/df＋37,810,176/remaining0，无active/openfiles/errors。当前普通生产34冻结，测量后三docs独立归档`batch34/post-measurement-docs`，不改as-built80或旧证据。

### 4.15 已测35：raw PF消费与signed ordered load

C内只收敛实际消费：RawAF、全旗、clear-AF、8/16位逻辑结果，只有PF raw载体可符号扩展，普通数值的canonical截断保持；`SXTB/H X1,result`合并旧PF复制与NZ比较的符号扩展。result已在X1且仍有数值用户时保原路。实际B2197b0的X2 UBFX与后续字节BFXIL保留，少一条热运算；尾部不可达pad抵消whole words，因此仍34 words，不能只比较整块长度。

仅RcpcImm9原四门满足的LoadMemoryTSO U16→紧邻SignExtend U32、同块/defaultmeta/full16view/最后且唯一用户/双方不spill时，直接以`LDAPURSH W`写consumer既有Home。目标与deferredPF冲突则原路；原load.saved跳旧U16保存，只记一个精确consumer ID跳SXT，无IR/Home变化、无表或pass。成功load才提交目标pin，失败保旧pin及原load guestPC。冷recipe复用原site，改为`LDURSH W; DMB ISHLD; B fast+1`，Run与fault解码同认signed fast；不能沿unsigned load回补。B21b2d0四对实际命中，61→57 words；Function对照21c483仍148 words逐字同34。三份IR/Home相同，frame/spill局部0不代表全应用。

35有效长跑16.767/15.974s，FEX15.127s：time1.108415416×/1.055992596×，cycles1.107511747×/1.057213150×，instructions1.066771021×/1.024401665×。去byte完整compile47.996810/83.615719ms对FEX48.255989583ms，为0.994629069×/1.732753172×。**Block仍未达1.10×，Function继续达标；总目标开放，两scope compile≤2×。** 不将两hunk或局部字数分别认领整项收益。

Host5、Android264524累计checks与原12/4170、formatter、SMC及EOF预期125均通过；高check数主要来自已有大分支fixture的逐字Need，不是新增264K语义矩阵。20次cold全部失败/retry/发布/释放仍计费，只扣byte调用；全应用spill25/42、maxframe48/64B继续未解。80source `a40faa3165b9186754ccec697b024a8ca65f4db5639aae545c229fb5d14989cd`，runner `604424430e7265d139848c5af89700e1ab08796d1d141e4b0680c5e9d9bb6ef4`（8,423,456B）；quality SHA `62a539030513ab52f76f51b98c95d7b458fa5ef7aedad88c371deaf4bdf4f6a5`。cleanup du37,782,528/df＋37,842,944/remaining0，无active/openfiles/errors。测量后三docs独立归档`batch35/post-measurement-docs`，不改冻结80或旧证据。

### 4.16 已测36：受限低位消费、wide TEST与中立load地址

C仅在RawAF、XorU8/full95/LogicalZeroCVA、非legacy、不spill且目的非PF1，唯一数值用户为同B紧邻default StoreUniformU8、完整U8 view及真实pinned字段时，省结果UBFX。原SXTB X1取低8建立PF/NZ，BFXIL取低8提交并保父高位；普通数值、raw/wide及多use仍保canonical路径。U64同值full95 TEST无数值用户时，原Read规范化后用`SUBS X1,value,#0`，AFxor0、PF/NZ相同，硬件C1已是guest C0的反向。LogicalFlags仅增私有默认carry极性参数，旧调用等价；原DeferTest先行，2197e0的六条self热路及故障tag不改。

B只在原forward walk识别默认metadata的scalar LoadMemoryTSO裸U64地址：同B默认Add/Sub U64、实际sole-use且非pin，base自然及有效U64、literal U64，模64位位移在[-256,255]。保持原load ID/type/metadata，把既有Expression与原base/view/literal交consumer；Rewrite成功才迁use账，原末逆DCE退Add/Sub。每次最多追加4words，由load及独占地址producer的2N额度支付，`oldW+2N`硬上限不变；没有新pass、N表、IR opcode或Known专属语义。Unknown保同一中立表达式；RCPC关闭时C沿原Expression计算完整地址，故障顺序与guestPC保持。

冻结36仅四个指定offline单元各一次：B21979b full TEST热路少CFINV，whole30→28；B219790实际退地址ADD，LDAPURH/LDURH都直接带#2，Home据真实新寿命重算，whole35不变；B2197b0热路少UBFX，whole34→32；Fn21c483的IR/Home/148words/site/link逐字同35。三个局部改进不各自认领动态收益，未新增运行RX绑定，也不以wholewords乘entry次数。完整报告与实际坐标在`batch36/native-quality-review.md`。

36有效长跑16.575/15.845s，FEX14.998s：time1.105147353×/1.056474197×，cycles1.104393993×/1.058373914×，instructions1.043025581×/1.002199129×。去byte完整compile46.799072/82.989579ms对FEX47.452552083ms，为0.986228768×/1.748896010×。**两scope compile满足≤2×，Function继续进入1.10×time/cycles；Block仍未达，总目标开放。** 原始PMU整数及branch misses见实施进度第36批，不将单轮比值写成统计保证。

Host5、Android264542累计fixture检查及原12/4170、formatter、SMC、EOF预期125全通过；大Need累计不代表新增264K语义case。三cold Block均2045attempt/N36672/words65867/native317268；Function746attempt仍含722Function＋24Blockretry、N86893/words152552/native509492。所有失败/重试/发布/释放继续收费，只扣byte调用；全应用spill25/42、maxframe48/64B未解决。80source `87d1c870bf05ea0d69f2dcbb0ff25432d6ab89c185781abff737ac89d14b09d0`，runner `14de0114e2921a67388f098adeca46ca398d799fffff312c24ba1c72c7329739`（8,436,728B）；cleanup du37,831,680/df＋37,892,096/remaining0，无active/openfiles/errors。三docs测量后独立存`batch36/post-measurement-docs`，不重标as-built80，旧失败及所有批次历史保留。

### 4.17 已测37：MADD、byte-only消费及signed32 load；本轮门限通过

C仅在同B紧邻、默认metadata的U32 Mul→Add、product恰占Add一个Value输入、原last-use恰consumer定义位置、三源完整U32/Carrier≥32且结果/输入均不spill时，以原ReadW发MADD到consumer既有Home。deferredPF目标冲突、metadata/legacy SaveFlags（含可夹Clear）保原乘加；fullflags Add不融合。producer.saved避免旧Mul保存，只复用原单个`elided_consumer`跳精确下一节点，非新IR/Home/表或扫描。MADD读完三源后才写目标，数值按模32等价，旧flags不动。

原36 sole pinned-byte-store准入移到无状态helper；旧full RawAF XOR门保持，新默认U8 Add/Sub bare immediate且flag_mask0可同样省UBFX。原模8立即数正规化、较窄view的Read及non-S算术保持，BFXIL只提交低8并保父高位；额外/wide/raw数值消费者仍走旧路。原strict signed16→32 load分支仅增加U32→U64，沿原四门/真实marker、同B/default/fullview/sole-last-use/不spill/PF冲突边界，`LDAPURSW X`成功才写consumer。Run与handler均将19800000认作load，cold由fast XOR21000000得到`LDURSW X; DMB ISHLD; B fast+1`；Rn==Rt为非writeback寻址，失败保旧完整pin及原guestPC，cold再fault不二次救援。

四个冻结offline单元各一次，IR/Home均与既有基线逐字同：B21c3b3热少SXTW，whole29由cold pad抵消；B21c4b1的byte-only热少UBFX，whole31→29；B21b2d0仅第二组无旗Mul/Add融合，第一组fullflags/PF26/故障recipe保，whole57→55；Fn21c483实际148→146，来自byte cut与signed32各一条，未命中MADD。三个局部收益不拆认动态整批贡献，不以wholewords乘entry频次，无新增RX绑定。完整质量报告SHA `53c37434dccdbb678da7bd425eb533d98476cfe65bf12134c7b71405df8e1567`。

37有效运行16.302/15.493s、FEX15.072s，time1.081608280×/1.027932590×，cycles1.081087685×/1.030169884×，instructions1.027346518×/0.986518968×。去byte完整compile48.187132/84.762869ms对FEX47.3196875ms，为1.018331577×/1.791281251×。**两scope本轮time/cycles/instructions均≤1.10×，compile均≤2×，固定输入量化目标完成。** 汇总判定在`batch37/measurement-receipt.json`；runtime-only JSON的历史硬编码false占位保留原bytes，汇总按实际八项门限判true，不改测量数值。

Host5、Android264579累计Need及原12+4170、formatter/SMC/EOF预期125全pass。三个cold Block均2045attempt/N36672/words65867/native317132；Function746attempt仍含722Function＋24Blockretry、N86893/words152552/native509124，全部失败/重试/发布/释放计费。spill25/42、maxframe48/64B保持，未实施first-eviction/20B候选不是本轮验收前置。80source `79c8bf0c2046ab35ece932b5e74310b13d5de339b00ee01e4def117235cdd985`，runner `d927b24230bd4e9b80cb771394d8761dd1c382a59a0391f750321d9a9aa051aa`（8,447,952B）。cleanup du37,889,024/df＋37,949,440/remaining0、无active/open/errors；三docs测量后独立归档`batch37/post-measurement-docs`，不改as-built80、历史失败或原仓库/Git/index/WIP，未commit/push。

## 5. GPR pin 与分配：一次实际位置决定

### 5.1 保留 pin16，统一绑定来源

ARM64 的物理映射、PF/AF 角色、caller/callee 保存约定集中在一份私有 ABI 声明。运行时选择一次，分配、编码、dispatch、helper、fault 消费同一约定；前端和公共优化器不包含该头文件。

同一 Uniform offset/width 在 C 中绑定到 pin 或 memory。移除现分散的 adjacent publication、delayed publication、narrow borrow 等互相推导策略，将其收成现逆向使用遍历中的固定16槽读写窗口：

1. 从真实 Uniform 读写及 clobber 找每槽下一次覆盖/观察位置。
2. LoadUniform 只在其全部真实使用期间 pin 未变，或满足确切 read-before-write 时借 pin。
3. 对实际 StoreUniform 的输入，若能在不提前改变观察状态、不破坏旧快照的条件下直接产到目标 pin，就一次确定该位置。
4. 不能直接产到 pin 时采用普通 Home；到语义 StoreUniform 才提交。Store 偏好不等于可以提前提交。
5. 读取当条指令全部输入后，再释放确切最后读取的可复用源，选择目标。覆盖写、分阶段读及 scratch clobber 按真实 opcode 处理。

Pin 窗口是约束计算，Home 是唯一位置结果；不再输出 affinity plan、修正动作或逐节点资格表。Encoder 消费同一 Home，不重新推导上述决定。

### 5.2 优先消除的搬运

- 合法的 LoadUniform 借 pin，StoreUniform 输入直接在同 pin 完成，生成零条 MOV。
- 两地址 ALU 的首/第二输入在最后读取点复用；保留跨写仍活的旧 SSA 快照。
- W32、UXTW、CSEL W、窄 Add/Sub shifted RHS 直接消费现 view，减少单独截取和结果归零。
- LEA32 在前端直接出生为中立 U32 算术，base/index 按 U32 view 参与，RIP/absolute/位移按模2^32计算；真实访存地址及 LEA16/64 保原路径。C 的 GetOperand32/Select32 由实际 W 指令完成结果清高，不能全局删除同寄存器 MOV W，因为它可能承担唯一的架构清高。
- 同一 spilled Value 的同 view 重复读取复用已加载值；不同 raw/view 不按 NodeId 盲合并。
- PF raw 与数据结果可共享时使用同一实际位置选择；下一次 PF 覆盖前仍活的数值不能永久借 PF 载体。

Function 跨块值继续有稳定 Home 和回边闭包。不能照搬 FEX 的 per-CodeBlock resident 清空策略后丢掉跨块 SSA。

### 5.3 已实施：XMM 驻留，Uniform 语义保留

第十六批源码复核确认原 CoreMark 的真实矩阵循环包含 PADDW、PMULLD、PMADDWD；PrivateVectorState 只减少出生，故障边界仍产生 XMM Uniform 发布，C 仍落为 state STR Q。固定 FEX 的 x64 SRA 使用 v16..v31 保存十六个 XMM。当前静态案例每循环体有3/3/6次 XMM 发布；这是具体路径计数，不能外推全程指令数或预先认领运行收益。

本批沿原 linked/private ABI 绑定 XMM0..15 到 v16..v31；前端及 Uniform IR 不加物理寄存器信息。`CompileOptions::pinned_vector_offset` 默认 UINT32_MAX，原 memory ABI 保留。Runtime 同一 Process 的 Block、Function 和失败后的 Block retry 使用同一布局；Unknown 输入仍可由已知后端做晚期绑定，不能在同一驻留上下文中混入读取过期 backing memory 的单元。

- 原逆向 use/window 遍历统一固定32槽：GPR16×8B、XMM16×16B，以 producer Type 区分 X/V bank。Home 保持16B，无新N表或pass。仅完整槽首址V128可借pin/亲和；跨B SSA继续用动态home或16B栈锚。publication和borrow共用实际writer的同点终止规则，只对真实先读后写的单指令整数向量操作开放；复杂shuffle/FP保原快照语义。
- 完整V128 Uniform 直接为pin间copy，同pin零代码。标量、非槽首址和跨槽alias先发布实际交叠的一至两个Q，执行原精确访问；仅写后重载对应Q。读不伪造pin覆盖。原窗口把完整slot的真实alias写作为clobber，保护旧SSA快照。
- 物理预算明确为 pins v16..31、动态 v2..7和v10..15共12个、输入spill scratch v0/v1、复杂计算临时v8/v9、spill output v9。关闭绑定保原20个动态home及30/31临时。不能直接保留旧v30/v31使用，它们已是XMM14/15；也不能把spill output改v8而覆盖scalar FP尚未插入的低lane结果。
- root frame96→160B，原GPR偏移不动，额外保存host d8..15低64；首次入口先装16Q再进共享publisher，cold前发布、C++返回后在成功/失败分支前重载，Exit发布后恢复host。原helper stub统一576B，在原448B后保q30/31及q10..15完整Q，不逐call展开另一套保存逻辑。
- NativeRunContext仅尾加真实offset（152，size160）；FaultContext同一配置。fatal沿实际ucontext的baseline FPSIMD record复制v16..31完整128位；原SMC/BUS内部retry仍先返回，不改其保存现场。不建向量历史、逐FaultSite位置表或新状态owner。

实际修改限定于原abi/allocate/Writer、helper和root/fault/runtime接入，不增加文件或编译阶段。第十七批已通过一个合并场景，覆盖旧SSA、跨槽alias、helper、冷口及fatal完整XMM，并保留原检查。去byte完整compile为Block44.665ms、Function81.459ms，约FEX0.941×/1.716×；20次输入实际spill slots从24/41变为23/40，最大frame仍32/48B。有效长跑指令少约0.81%/0.71%，Function耗时基本持平，运行仍FEX1.513×/1.459×。这兑现了具体state往返优化，尚未解释或消除主要周期差距；下一步核对真实动态退出/return工作量，不能继续靠静态发布数或采样IP份额外推。

### 5.4 已实施：退役实际消费者的重复截断

第十九批对原CRC入口21c880/21c950作匹配源码的离线重编译，分别确认22/39个BitExtract在UBFX后又做同宽Narrow。其LSR实际为U32且已是一条，CMOV也已直接CSEL，不能把不存在的展开或bool往返列成收益。

只在C实际编码收敛：BitExtract先保留原Field合法性检查，再一次UBFX截取`min(count,result_width)`，由指令自身零扩展，省其后的重复Narrow；BitClear仍保必要正规化。输入RawScalar及默认raw transport语义不改。partial Uniform写入仅在Value的有效view和Home carrier均覆盖field时直接借实际物理低位，让BFXIL/STRB/STRH/STR W完成裁剪；更窄view/carrier写更宽field仍先Read补零，完整U64发布仍执行原Normalize。

这不全局放松U8/U16数值结果合同，也不删除XOR producer的Narrow。没有新增IR、Home位、分析或pass；复用原integer/raw/partial/spill检查，随本批合并验证，不为每个小helper建测试。

第二十批已测：两CRC入口words由199/311降至171/265，IR/Home/frame/spill不变；22/39次重复BitExtract及各7次partial-store前截断消失。与§7.5同批有效长跑为Block21.524s、Function19.942s，约FEX1.427×/1.322×；这是整批收益，不单独归因每一hunk，仍未达到追平目标。

### 5.5 已实施：直接消费TST掩码与借用carrier低位

第二十批CRC实际N16、N37尚有仅求Z的MOV mask→AND→signed cut→CMP序列。第二十一批沿原`NativeNZCV + And + Z或C|Z + 无数值消费者`这一个准入guard增加immediate RHS：掩码同时受结果宽度、RHS view、lhs view与Home carrier限制，非零且可编码才用原RawValue physical→PrepareNZCV→TST→FinishNZCV；零/不可编码保fallback，同Value原路保留。通用入口需要的N/C/V保留和C|Z的CFINV仍执行；不统一承诺一条，也不改PF/AF。

BitExtract仅在自身实际读取时直接取物理carrier，截取长度为`min(count,resultWidth,max(CarrierBits-first,0))`；借用pin语义上carrier以外为零，不先做MOV W/窄UBFX再提取低位。无narrow的carrier仍64位raw container，不能按自然type或view裁掉BitCast/GetResult高位。原Field/零count/Value-kind/view验证保留，BitClear/BitInsert与其它RawScalar口默认逻辑读取不变。

匹配第二十一批冻结源码/库的离线两CRC入口21c880/21c950，words由第二十批171/265降至137/217；N/B仍164/1、272/1，frame/spill均0。结果由实际词流核对，不宣称已与运行RX逐字绑定，也不把静态删词数外推全程收益。

无新IR/pass/N数据、模式或公共ABI，复用已有integer/raw/partial及Block合并检查。第二十一批仅改`compile.cpp`与`emit.h`上述写口，Host五项、Android1965 checks及原集中检查通过。完整去byte compile为45.543719/81.818327ms，对FEX47.298646ms为0.962897×/1.729824×；有效长跑20.728/19.135s，对FEX15.094s为1.373261×/1.267722×，cycles为1.372665×/1.268399×，指令为1.309510×/1.178007×。这是整批测量，不孤立归因两个hunk，不宣称追平。该批尚未实现extract-shift融合，后续实际归约见§5.6。

### 5.6 已实施：按forward真实中间态归约U8提取

第二十二批尝试将sole-use BitExtractU8与相邻LsrImmU32合并，但两真实CRC入口的IR/Home/words均与第二十一批逐字节相同，folded/erased/native bytes也未变。该批仅作为中途候选封存，不认领运行或选码收益。原因是原ForwardCopy在LSR处刚移除ZeroExtend32的use，其record仍持有inner的旧edge，尚未成为Nop：此时gap是零use的ZeroExtend32，inner有两条use；最终DCE后的图不能代替这个实际时点。

第二十三批保原三扫与oldW+2N预算，只收外层U32且effective input view严格U32、内层default BitExtractU8/count8/合法first/非pin、同B邻接或限定单条gap。邻接/default Nop时inner count必须1；精确default ZeroExtend32U32 gap须零use/非pin、单Value指向同inner且原view为合法scalar，此时inner count必须2。不提前erase或搜索其它pure-dead链。原外层ID就地改为BitExtractU32(parent,first+shift,8-shift)，保内层原parent/view；count仍先按U32归约，只收1..7。统一Rewrite成功后维护现use账，第三逆扫按旧edge依次退gap和inner。

同批将紧邻sole-use、非pin、default BitExtractU8(parent,0,8)向AndU8 immediate#1转发；只收LogicalZeroCVA、Z或C|Z、非branch_only，外lhs有效view必须U8。parent原view须为合法scalar，替换edge明确用U8读raw低位，保And原type/metadata/discard。复用ReplaceValueUse和同一use账，raw/wide/多use/其它形态保持原路径；没有新增通用alias规则、数据表、扫描或恢复合同。

离线两入口words137→129、217→201，各命中7次extract-shift，low-mask分别退1/8个UBFX，crcu16另少1条alignment Nop。birth目录N/B仍164/1与272/1，frame/spill均0；Home局部字段随真实edge重新分配，不称全部相同，未新增运行RX绑定。20次诊断输入folded从30/53到39/84（含局部代数），erased从604/1319到621/1383，native bytes从321756/534504到321692/534256。

第二十三批Host五项、Android1965及原合并检查通过。完整去byte compile为46.416828/82.417095ms，对FEX47.721354ms为0.972664×/1.727049×；有效长跑20.613/19.054s，对FEX15.128s为1.362573×/1.259519×，cycles为1.359872×/1.259721×，指令为1.302453×/1.170957×。这是两项归约的整批结果，不拆认动态贡献；compile合格但运行/cycles仍未进入1.10×追平容差。完整身份和原始结果见实施进度及`batch23`；后续第二十四批GPR partial缓存见§4.4。紧凑IR/唯一owner/三扫及两scope合同不变。

## 6. B：flags 与死写归约，保持三次 N 扫描

### 6.1 先消除已有确定浪费

已测矩阵 `IMUL → SHR → AND` 的11条额外溢出链必须退役。Shift 的 count=0、部分输出和保留位合同继续成立；不靠简单删除所有 Shift 前 Flush 实现。

公共 opcode 语义提供真实 flags read/write mask；普通“保留未写位”不算读取这些位。显式读旗、未知效果、外部调用、flags Uniform 别名及现同步 fault 观察按真实合同保留。尚不能分类的操作保守处理，不另建验证平台。

### 6.2 最终算法边界

1. **现第一逆向扫描**同时处理活值和块内 flags 覆盖；记录每块保守 gen/kill 及最多六个“仅因块出口而活”的最后 flag writer。已被本块覆盖且未观察的 setter 不再作为无条件 live root。
2. **Block** 出口 flags 需求为 All，不建立 CFG 数据流。**Function** 沿原 terminal 边对该保守 transfer 求六位 live-in/live-out；外部出口为 All，MayFault 的需求仍进入 gen。只需块域记录、实际 predecessor 边和有界队列，每个位单调传播，工作为 O(6×(B+Ecfg))。
3. 按出口 mask 原位修正至多6B个已记录 writer，提前释放相应 uses。清除 metadata 后，数值结果仍被使用的 ALU 保留；全死 Void setter 明确 Rewrite 为 Nop，维护现 use 账。这一步不独自承担整个 flags/DCE 闭包。
4. **现第二前向扫描**折叠和 Uniform 转发/完整覆盖写消除；观察点结束旧 Store 的可删除资格，value 转发资格单独处理。
5. **现第三逆向扫描同时重新归约最终块内 flags 需求、执行 DCE 并维护 use 账**：以各块出口需求开始，先判断当前数值/副作用/所需 flag 输出是否仍活，再决定是否传播其 flag 读取；清除死 metadata/setter，并释放当前 operands。这样前向死写消除后才死亡的 CondSet/GetFlags，以及删除 flags-only ADC 后不再需要的 SetCarry，能在同一逆扫中继续退役。不能沿用“凡 WriteFlags 都是 root”的旧规则。

仍为一份 u64/N 表、三次节点扫描；Function 额外数据按块/真实 CFG 边计，不产生逐节点 flags Facts。块域算法放在原 Optimize 内，不新建 PassManager 或全局 analysis cache。

首轮 gen/kill 对“数据只因出口 flags 存活、随后才被删除”的跨块读取可能仍是保守上界；上述算法不冒称与 FEX 完整 RFCE/DCE 固定点等价，也不隐藏第四次 N 扫描。先确保最终块内闭包消掉本轮已证实的死链。若最终热代码仍受这类跨块残留影响，应以真实案例细化块域 transfer，再重新量 compile；不能用实现了三扫或六位传播代替生成质量验收。

冷恢复尚未实现前，不能为了跨块删除 TEST 就把 MayFault 从 flags 观察点中拿掉。恢复表示应由 C 消费真实位置后决定，Uniform/flags IR 的语义不变。

### 6.3 省去死位保留需要明确 IR 合同

当前 `flag_outputs` 表示部分更新，其余位保持旧值。B 裁成仅 Z 后，C 继续发 MRS/RMIF 保留其它位符合这个合同；不能仅因下一条是 JNE 就删除。固定 FEX 的 TestZ 明确规定 N 未定义、C/V清零；其 InvalidateFlags 也直接声明未定义写。这些是 IR 语义，不能当作未写位保留来仿照。

下一步使用现 Operation.properties 的一个空位表达中立的 `discard_omitted_flags`，默认false，Operation仍24B。true明确表示：本操作写后，Flag::All中未请求的位允许取任意值，直到重新定义；这是未指定状态，不是UB或poison，reserved/非flags carrier位仍保真。后端可继续保留，也可不保留。它不是‘当前图已经证明dead’的缓存，不要求编辑epoch、验证证书或全局dirty。

B只在最后原逆扫，对已知整数metadata、非BranchOnly、仍有请求输出且并非All的活节点，在当前写后需求 `needed & ~keep == 0` 时赋予该合同。GetFlags whole-word、MayFault、真实alias、helper/未知及外部出口仍要求All，不能获得错误许可。冻结out的保守上界只会减少机会。泛化getter或新observer若后来加入，必须理解已经声明的未指定状态；清该位只选择保留本操作的旧输入，不恢复B此前已删除的原完整计算。

首批自动赋予范围为 `LogicalZeroCVA + keep=Zero`。后续已确认状态机的 C|Z 逻辑与算术比较也受遗漏位保留影响，因此沿同一末逆扫将许可推广到 `FlagWrites` 已确认的整数 metadata：`keep != 0 && keep != All && needed & ~keep == 0`，仍排除 BranchOnly。判定使用当前op之后的需求，随后才传播 ADC/SBB 等实际读取；即使keep未变化也需正确设置属性，keep清零时同时清属性/profile。全字观察和故障的All需求仍阻止许可，不能把实际输入Carry或真实故障所需的PF/AF当遗漏位删除。

C仅将该许可合入现Home的NZCV clobber nibble，不增加N表、块域结果owner、阶段或重新求跨块活性。默认frontend/未优化IR、generic/opaque/legacy合同不变；忽略许可的后端仍是正确实现。属性编码、Builder/Edit/Rewrite、metadata equality、effect分类与真实观察边界必须同批接通，保持前后端解耦。

## 7. C：原生 flags 表示与有限冷恢复

### 7.1 先统一普通原生表示

参考 FEX，用现有两个寄存器预算收敛 raw PF/AF，不再为每个完整算术结果立即拼成 packed word：

- Native NZCV 继续采用现已明确的 carry 约定。
- raw PF 保留低结果字节；raw AF 可保存输入 XOR，在真实读取时结合结果的 raw PF 取 bit4。
- packed flags 只在 GetFlags/SaveFlags、显式 Uniform 别名、helper/service、退出和故障等真实观察点形成。
- 原 packed word 的非 flags 位必须保留。可由原 GuestState 字段保存相应 shadow；这不改变中立 flags-word ABI，也不需要新的逐节点记录。
- PF 单独改变而 AF 保留时，必须补偿 raw AF 表示；部分 writer、shift0、MUL 仅写 CF/OF 等不能套用完整算术的更新公式。

实现使用同一原生状态模型，迁移 root/cold/helper/normal exit/fatal/fault 的最后一个 reader 后移除旧 native packed 拼装路径；无 FlagM/无 binding 的通用 packed 分支保留其真实硬件兼容用途。不给每条热指令增加 old/new 模式判断。

### 7.2 精确故障合同下的 TEST 恢复

FEX 的七条反转循环省掉 TEST 的 flags 写，普通访存不阻止其 DFE。SVM 需要另一个正确实现：IR 中 TEST 语义保留，C 可将仅在故障时需要的结果表示为现 FaultSite 的一个有界恢复描述。

首版只支持完整32/64位 `TEST x,x`，最多一份未物化 recipe；不支持任意算术历史、recipe 链或通用表达式 DAG。仅借实际仍有效的 pin、寄存器或既有 spill，不为这个优化延长大批值的生命周期。没有低成本来源就用同一个 materialize 入口恢复为普通原生状态。

候选资格并入 C 已有块/位置遍历，在发射前收口；Writer 绑定实际 site 来源并发出最终代码，不先发射一版再回退重编。该处没有资格时沿同一原生状态模型物化，不调用旧流水线。

恢复描述随真实 FaultSite 和原 CodeArena 生命周期保存，按故障点数量 F 计。可在现24B FaultSite尾增加一个明确的8B描述至32B；不加 N 级表、第二个 PC 索引或独立恢复 manager。实际输入位置在发射该 site 时从 Home/Uniform 当前版本确定，不把 NodeId 留给运行期猜值。

### 7.3 反转循环的有界 tag（初始稳态目标八条）

本次真实代码的首次入口来自 XOR zero（ZF=1），回边来自 TEST 非零 next（ZF=0）。load 与 store 共用的故障点不能挂一份无条件 TEST recipe；store 时 R10 还已被成功 load 改写。

本方案首选**一位后端内部 Test tag**，不先做循环 peeling 或 CFG/SSA 克隆：

- 在 RawAF 只使用低32位的约定下，以 x29.bit63 区分 Complete / DeferredTest；普通 AF writer 保证清该位。这一位不占 guest packed reserved 位，也不要求与 raw PF shadow 的改造同时完成。
- TEST 回边发一条 MOV tag，再用 CBNZ。当前 steady-state 目标为8条，对照 FEX7条；不宣称免费省到七条。
- 首次入口 tag=0，故障沿正常 PSTATE/PF/AF 恢复。回边 tag=1，本循环各真实 load/store site 绑定已有的 R12=current/address，它保存入口 TEST 值，避开已被 load 改写的 R10。
- **零值退出也会经过 MOV tag。**非 self 出口按此时真实 TEST 输入 R10=0 物化并清 tag；不能复用故障点的非零 R12。此成本发生在退出边，不加到每次非零回环。
- 仅支持一个可确定的 self-loop、非 self 入口 Complete、所有实际 fault site 有正确输入绑定的情形。Block 的实际 self-edge 仍走轻分配；不能跨普通 dispatcher/unit 携带未物化 tag。
- GetFlags/Shadow、flags Uniform 别名、partial writer、helper、syscall、不同来源 join、外部出口前必须 Complete。恢复后继续重试的未对齐/SMC 服务保留原 native tag；真正退出服务消费其恢复结果。
- 所有入口已具有等价 TEST 状态且来源有效的其它情形，可不发 tag、直接用静态 site recipe；这不是当前循环的默认资格。

此范围只覆盖现已承诺的实际同步 FaultSite。它不宣称已解决任意 host PC 上的完整异步 guest signal 状态。

### 7.4 已实施：仅块内的 PF 与 carry 静态故障状态

第十五批仍未达到周期目标。下一个实际案例是矩阵ADD之后只有访存、随后完整CMP覆盖：SVM为了fault先做PF搬运与CFINV，正常路径随后又覆盖它们。不能从IR删除这些旗；C可在单个B内延迟具体载体维护，实际FaultSite静态描述当时来源，无需再加运行时tag。

第十六批通过`CompileOptions::local_flags`由Runtime显式开启，默认public/callback合同不变；限linked/private/native、有实际fault服务的32/64位Add/Sub/Neg metadata。Writer维护一个pending PF实际pin及一个carry极性状态，不跨B/control/helper；另有两个当前operation的u8快照，只在实际标量memory发射点赋值，沿原F记录追加口写入。PF来源须是真实、未溢出的pinned数值结果；AF输入xor仍即时写W29。Logical、ADC/SBB及其它原型保现路径，不引入AF表达式恢复、算术历史、资格表或延长SSA寿命。

- 新metadata完整覆盖PF+AF且不读旧旗时可kill旧PF；其它真实PF/AF/whole读取、partial或opaque写、helper前统一完成。任何实际低字节pin覆盖前也要完成，包括coalesced destination及Read向目标pin的正规化。完整同pin发布且有效view≥8、或已知无代码的借pin读不改低字节，可保留来源。检查必须早于Read，不能等Uniform写后再修。
- CF仅延迟现FinishNZCV本来要发的CFINV；真实读旗、Pack/Shadow、显式/legacy旗口、helper与出口前恢复原inverted合同。下一纯CF writer可以kill旧状态；partial更新需要保留旧C时，在Prepare的MRS前完成，不能为每种reader另建多representation逻辑。
- Terminal及跳过Terminal的两条fused PC-store出口都须在分叉前完成；不能只在第一个TailJump上清C++状态而漏掉另一边。既有DeferredTest全写保持独立优先级，不能把新pending静默带过其tag边。
- 现32B FaultSite尾padding可容纳两个u8：`parity_register`（0为x1，否则实际pin）、`carry_direct`（1表示PSTATE.C已经等于guest CF，0为原inverted）。记录实际guest访存指令处的状态，不能盲抄Emit结束后的新状态；复杂MayFault/无有效marker先完成。无N级数据、额外IR扫描或第二fault索引。
- fatal普通分支选择该raw PF的完整低字节，同时用`(x29 ^ selectedPF).bit4`恢复AF，按carry_direct决定是否翻PSTATE.C；reserved仍来自原packed shadow。TEST recipe优先。BUS/SMC内部重试仍先返回、保持原ucontext，原冷热fallback共用同site。

文件职责不变：原Writer准备/算术/Uniform/终结口维护这两个状态，原FaultSite与fault owner恢复。复用一次实际Effects，不另扫IR或挂通用验证器。验收复用同一程序三次：ADD到RAX后fault1、MOV覆盖RAX后fault2、正常Return；核完整packed/reserved与实际site来源，并保原TEST/SMC合并检查。只有被后继覆盖且中途未真正物化的MOV/CFINV才算删除；单纯把同一条指令挪到出口不算收益。

第十六批上述真实故障场景及原合并检查通过，FaultSite仍32B。有效长跑指令减少约1%，Function耗时基本持平；因此不能把局部恢复已实现当作主要周期差距已解决，也不据此继续扩大成通用flags恢复引擎。后续源码复核定位到实际矩阵XMM state往返，当前先实施§5.3；FEX动态调度绝对工作量尚未准确计数，仍不能用sample-IP份额换算。

### 7.5 已实施：Block64自回边复用有界TEST恢复

当前实际Block2197e0离线热回边为9条，已有predicate直消费，剩余是TEST维护；不是早期18条。只在frame0、无entry-PC publication、linked/private/native及真实fault+unaligned服务下，沿原prefix状态机证明两site/snapshot，末4记录为TEST同next、previous发布、唯一CondSet NE、PC-select taken==本entry/另边不同，才启用Block64候选。Function原末3合同保留。

非零回边仍经原可撤销self Link，不改成裸B body；frame0保证entry与body一致。self冷stub仍为pointer+2（31起派生，不再占LinkSite字段），第一组从实际next pin发完整TEST（MOV x1,next；MOV w29,w1；CMP next,#0），再交原PC/dispatcher；zero边独立从已判零的next补同一完整TEST，再走原exit链接。固定发码不把cold分支的C++ flags bookkeeping当作另一边输入，无新状态表/ABI字段。常规eager PC语义也由原PublishPCAndJump保留。

SMC ResetLinks必须仍能迫使self下一跳进入冷stub并完成flags后退休旧arena；tag只准留在这个同owner的热self边，不能跨普通dispatcher/其它外部边。精确fault仍用实际R12入口snapshot，不能用已被load覆盖的R10。预计resolved self由9→7条；不是Function单CBNZ，也不由静态条数预先承诺时间收益。

一个新增Block合并场景覆盖首入口Complete、tagged load/store、zero全flags、已有alignment冷访问，以及Unlink/真实protected-store retry后的强制cold和flags交接；原Function七场景保留。不新增CFG/pass、IR副本、LinkSite或FaultSite字段。

第二十批上述真实场景及原检查通过，Android1965checks。冻结后离线生成确认resolved近self路径7条，2个site均64/R12、2个原Link；self冷与zero分别补完整TEST。未patch的LDR/BR镜像和无RWalias的cold路径不冒称7条；离线输出未与运行RX逐字绑定。完整compile45.532/81.195ms，对FEX46.863ms为0.972×/1.733×。

## 8. Block 终结、近链接与 PC 冷发布

本方案初始测量中的同一反转循环为 Block18、Function11、FEX7条；后续实现与实测见实施进度。Block 多出的条件物化和出口准备应沿真实终结消费者处理：

1. 当 CondSet/TestFlags 只有最后的 PC Uniform 选择消费者，C 直接消费原谓词；数值 bool 仍有其它使用时才物化。保 PF/AF/C|Z 等真实条件，不全套为一个 ARM 条件码。
2. Links 记录真实 branch relocation 类型与 target PC；范围内 B.cond/CBNZ/B26 直接进入已发布目标，远目标使用原 fallback。它是最终机器码重定位，不是新的动作表。
3. 未解析/失效边进入本边 cold stub，取得正确 target PC；resolved 热边跳过 PC 常量准备及 state.pc 热写。IR 的最后 StoreUniform(PC)仍保语义，由具备 fault-location 服务的 C/D 合同消费。
4. dispatcher 首槽比较用非 S SUB+CBNZ，减少热路径 NZCV 保存/恢复。cold publisher 在自身改变 flags 前取得正确状态。
5. 原 runtime links owner 负责 pending/解析/撤链；原 CodeArena 负责实际 patch、W^X 与 I-cache 发布。SMC 撤链后才安全退役，不能仅新增 B patch 而保留旧失效漏洞。

实施时使用同一 Process 的明确 deferred-PC ABI，并向每次真实 compile 传入 entry_pc；默认 public/generic 编译保留 eager 发布。目标单元若有 PC 字段重叠 Uniform 读取、非尾完整写、partial/vector 访问或实际 helper/Return 等观察，沿现分配 walk 记录单个单元属性，在入口物化 entry_pc。不能假设 resolved 入口的 x0 仍保存 target PC，也不能因现前端少见 PC LoadUniform 就省略其合同。

静态link保留LDR/BR/DATA的未解析gate，31允许BR后置不可达pad使DATA对齐，cold stub紧随DATA。LinkSite仍16B，现保存pointer_word/真实branch_word/target_pc，fallback由pointer+2派生；32/33的branch_word也可指早期CBNZ/B.cond head，其偶数late gate为pointer−2，普通奇数gate则可能为pointer−3，不能混淆head/gate或统一从pointer猜branch。ready且条件imm19可达才patch早期head；未选中边、far/unresolved及撤链保原gate/owner。实际L个初始指针重定位必须在CodeArena memcpy后、flush/mprotect前完成，Publish只借links span；近链patch/撤链均消费真实branch。撤链从RX DATA+8恢复本边stub，保留无RW alias的W^X路径。沿原实际use省去PC-only LoadImm实体，动态完整PC先真实view读到x0再退frame，cold dispatcher在复用x0前提交state.pc；默认public/generic及非尾写保持原合同。

TEST tag 首先可用现 Function 的真实 self-edge；Block 还需实际 entry_pc 与可撤销的 self relocation。在该 owner 接通前保 eager TEST，不能让 tag 穿普通 dispatcher，或增加绕过 SMC 撤链的本地回跳，也不能将 Function 的八条目标直接记作两scope都已实现。

普通内存 ABI、可被真实观察的 PC store、syscall/Return PC 及不满足服务合同的路径保原发布语义。FEX 的 call/return shadow stack 后置；当前先收敛同一 PC 目录，不再增加返回缓存。

### 8.1 已测 TSO 热布局：同单元冷 fallback

有实际FaultLocation及恢复服务的16/32/64位标量访问仅发一条有序访存，避免为原地修补在热路径占一个NOP。Writer在全部热body/links之后，只沿现FaultSite作F闭合，追加固定三字fallback：load为plain+DMB ISHLD+B，store为DMB ISH+plain+B，返回严格为fast+1；两向B26范围按实际偏移检查。未安装服务的guard、byte和向量合同保留。

同一32B FaultSite用`unaligned_fallback_word`定位三字冷范围，不复制site、不扩大热last_word覆盖其它IR，也不增加恢复manager/索引。原fault owner只在原fast的对齐故障上原子回补一字B，按原alias/I-cache与W^X合同发布；cold plain再fault仍消费同guest-PC/TEST recipe，SMC内部重试不提前物化状态，冷热代码一起由原unit退休。

固定FEX本身也使用NOP pair，这项选择是热/冷布局取舍，不是FEX缺少的工作。每site总静态代码约增加8B，已修补路径多两条B。第十四批指令少约7–8%，Function耗时基本持平；因此保留实现但不将其认作主要周期差距的解决方案，完整数据见实施进度。

### 8.2 已测动态位置内联同目录 probe

最终动态尾部在x0 target已读取、frame已退一次之后，直接消费原单一PC目录发完整非S查找循环。只用x2–x6/x16，保x0、raw PF/AF、NZCV及pins。每个source site的ready跳转拥有自己的BR；empty/pending/nil仍尾跳原dispatcher，允许冷路径重复查找并如实计费，不增第二cache、entry manager或Return cache。静态links/cold stubs及Return/service出口不内联。

这是ARM64物理ABI显式能力：`DispatchMode::Callback`为默认，Runtime明确选择`Inline/InlineCounted`。ABI常量留在后端私有abi.h，实际Runtime布局由现header断言；前端和公共IR不依赖该布局。计数模式只在编译时选择，普通路径无计数指令/判断；诊断记录每次真实native attempt/probe，含冷重复，不包括C++ Find。

同一CodeCache热槽改32B `{pc,entry,code_id,pending_head,occupied}`，原40B NativeCode在同owner的实际ready冷数组，不带PC key、不独立决定membership；实际映射仍由CodeArena拥有。新冷行分配成功后才发布ready；Grow/swap搬完整槽，SMC保留旧冷范围到原cold退休点。额外分配/复制/释放全部继续计入compile，不能把热槽减半称为整个cache减半。第十五批Function相对同轮旧镜像周期少约4.1%，误预测少21.6%，整批仍慢FEX约1.45×，不孤立归因单一feature。

### 8.3 已测：CRC散列的两scope取舍

第十七批隔离诊断闭合了原100000次输入：FEX有154,412,760次ReturnHint预测，全部命中；inline/shared L1合计457,600,120次。SVM两scope均约154,412,772次真实RET，但约612,014,000次native目录尝试；Function实际1,101,314,727次slot读取，平均1.79949。两端同域动态退出约612,013,000次，不能把查找次数差异说成guest执行量不同。

RET专属probe进一步确认：Function的154,412,772次RET只读199,713,375个inline slot，平均1.29337。扣除此域，其余约457.6M次尝试读取901.6M个slot，平均1.97028；约90.74%的额外probe来自该余量（含少量未分类shared冷重查）。因此先优化现有目录的散列；return predictor、多入口以及释放state基址寄存器的候选保留为独立后续项，不能把全局1.80平均数归到RET后承诺收益。

本批保唯一CodeCache、32B slot、half-load增长、pending/ready优先及原生命周期，只把原`(pc ^ (pc >> 11)) & mask`的mix在已确认CRC32+FlagM主机改为`CRC32C(seed=0, full64-bit pc) & mask`。CRC输入不做首尾xor，与实际CRC32CX W5,WZR,X0一致，后AND和probe不变。CodeCache仅持一个构造时配置的DispatchHash；C++查找/插入/rehash、CompileOptions和共享dispatcher从同一源消费，不各自推导或按命中动态切换。

inline与shared在生成时选一条CRC代原EOR，仍保匹配tag、nil/pending及SMC置空目录的原处理，不加热feature分支。默认public与缺所需能力的主机保FoldedPC；只加两个CRC NativeNZCV共享入口，不扩packed版本矩阵。无新IR/扫描、PC副本、二级cache、owner或每节点数据。

CRC延迟可能高于EOR，改善碰撞不等于已证明周期收益。按一个完整批次验证原合同、两scope有效长跑、PMU和去byte完整compile；新过程若仍未达到目标，继续按实际热点排序，不把目录模型的毛指令差当净可删量。计数源码、ELF与闭合记录在`batch17/{fex-counts,svm-counts,svm-counts-v2,native-exit-counts}`，诊断时长不作QoR证据。

第十八批已实测：Function20.773s，相对第十七批少5.05%；Block23.208s，反而多2.26%。20次输入平均slot读取分别从1.798降到1.493、从1.263升到1.417。CRC有真实Function收益，但不能全局采用，也不由这个输入推导按scope硬编码最优散列。本候选source/ELF/测量完整封存，生产回到FoldedPC并撤除新增CRC模式/两个thunk；下一批沿§8.4检验更少探测与低延迟mix的组合。

### 8.4 已实施：降低原目录装载率

只将原CodeCache的Grow阈值从occupied达到slot数1/2改为1/8，初始256、倍增、同槽PC/entry/冷索引及全部probe语义不变。保原FoldedPC两条mix/mask，不新增mode、表、owner、IR数据或扫描。8是同一目录的容量换探测预算，不是第二级查找或隐藏的可选pass；两scope与fallback同一规则。

相对原表预计增加数百KiB，具体按实际peak slots×32B报告，不能称为整个cache占用。额外zero/grow/copy/release继续计入去byte完整compile。目标是在原低延迟索引下减少实际dependent probe和分支误预测；内存/TLB代价和最终周期仍由下一完整批次实测决定。若不能改善两scope，不把更低probe或更大表本身当验收。

第十九批两scope均改善后保留：Block21.939s、Function20.520s、FEX15.086s，约1.454×/1.360×；去byte完整compile45.227/81.170ms，对FEX47.695ms为0.948×/1.702×。实际目录为1MiB/256KiB，相对旧表增加768/192KiB；code arena峰值不由此推断。20次诊断平均slot读取降到1.004/1.242，cold目录分配各多2次且已计费。不是追平验收，后续仍按当前实际生成质量细化。

## 9. Spill：保跨块 anchor，消除局部终身栈化

最新已测37仍使用唯一一份16B Home；本节首次eviction/fill方案尚未实施，仅为后续压力值候选，需证据与明确成本后决定，不是本轮达标前置。此前CoreMark直接SP访存采样低于0.1%，因此当时将此项排在已证实的死flags、Uniform亲和和热边之后；这个历史采样值不代替当前全应用spill25/42、maxframe48/64B的事实。

候选方案为**静态确定 slot 和首次 eviction，发射时才做真实 spill**：

- Allocation 的同一次前向决策确定首次 eviction 位置与 stack slot；frame 在 Prologue 发射前已知。
- 块内值在该位置之前使用原寄存器，只在首次真正被覆盖时 STR。之后用 canonical slot，fill 到可用 resident 后复用，避免每个 use 重载。
- Writer 不重新选 victim；它消费同一 Home 的时间位置及固定 resident 状态，当场发真实 STR/LDR。没有 spill actions、修正 pass 或第二份位置表。
- 跨块 SSA 仍使用稳定寄存器/定义处有效的 stack anchor，不假定某个动态路径一定执行首次局部 eviction。

若后续压力证据支持且成本明确，首次eviction信息拟用显式`first_eviction_position`，可能需Home从16B变为20B（增加4N字节）。**20B仅为未实施的备选成本；当前Home仍16B，本轮没有该字段或4N增量。**届时仍须保IR24/24/8，不通过复用definition/last_use隐藏含义，也不把第二张N表藏在preparation中。

不采用“emit到压力点才知道frame，再给所有尾部加占位NOP”的方案；不加入通用spill-slot coloring或全局split-live平台。本轮固定输入已达标，本候选保持未实施，后续按压力证据及明确成本决定；它不阻塞本轮既定验收，也不计作已交付机制。

## 10. 文件组织与唯一职责

目标布局如下。新增文件用于把同一 Writer 的实际实现集中到合适位置，不增加编译阶段、Program 包装对象或中间表示。

```text
source/runtime/
  ir/
    types.h, opcodes.inc, opcode.h
    program.h/.cpp
    builder.h, builder_impl.h, builder.cpp
  frontend/x86/
    decode.h/.cpp
    build_ir.h/.cpp
  compile/
    optimize.h/.cpp
  backend/arm64/
    abi.h                    # 新：唯一原生寄存器/flags/调用边界约定
    allocate.h/.cpp
    compile.h/.cpp
    emit.h                   # 新：私有 Writer 声明；不是新一层 IR
    emit_state.cpp           # 新：同 Writer 的 Uniform/flags 物化与冷恢复选码
    emit_vector.cpp          # 新：从现 compile.cpp 移出的实际 SIMD/FP 发射
    helper_calls.cpp
  execution/
    guest_state.h, guest_cpu.h/.cpp
    memory.h/.cpp
    native_code.h/.cpp
    dispatch.h/.cpp
    fault.h/.cpp
    runtime.h/.cpp
```

| 文件/组 | 最终负责的工作 |
| --- | --- |
| `ir/types.h`、`opcodes.inc`、`opcode.h` | 类型、Uniform/view、flag 读写语义、effect/schema；无物理 pin |
| `ir/program.*`、`builder*` | 唯一紧凑存储、局部追加/编辑、常数提交；无活性/分配 |
| `frontend/x86/decode.*` | 原字节解析与区域形成；不做物理寄存器分析 |
| `frontend/x86/build_ir.*` | x86语义、中立逻辑缓存、观察点提交及已知目标的中立表达式选择 |
| `compile/optimize.*` | 三次N扫描、有限块域flag归约、Uniform转发/死写，末逆扫耦合最终flags需求与DCE |
| `backend/arm64/abi.h` | 单一真实寄存器映射与native flags/调用布局；无每节点数据 |
| `backend/arm64/allocate.*` | 两scope生命周期、Uniform窗口/位置亲和、唯一Home、实际spill选择 |
| `backend/arm64/compile.h` | C的输入/结果、真正的fault/link relocation；不暴露plan包装 |
| `backend/arm64/compile.cpp` | C入口、同Writer主遍历、标量/访存/控制选码和实际分支收口 |
| `backend/arm64/emit.h` | 同一Writer对Program/Home/output的借用及少量固定状态，不建新owner |
| `backend/arm64/emit_state.cpp` | Uniform落地、raw flags、观察点物化、受限TEST恢复；直接写机器字 |
| `backend/arm64/emit_vector.cpp` | SIMD/标量FP实际编码、XMM Uniform绑定及alias同步、现控制模式及lane语义；无新状态owner |
| `backend/arm64/helper_calls.cpp` | 真实helper保存/参数/返回ABI，消费同一原生状态约定 |
| `execution/guest_state.h`、`guest_cpu.*` | 架构state与CPU模型；无临时分配状态 |
| `execution/memory.*` | 实际guest映射/文件与别名生命周期 |
| `execution/native_code.*` | 发布、alias、实际patch与代码/fault元数据持有 |
| `execution/dispatch.*` | root/warm/cold/exit交接，普通热查表不计数 |
| `execution/fault.*` | 实际同步故障恢复与有界recipe执行，复用原SMC/未对齐服务 |
| `execution/runtime.*` | 请求、重试、唯一PC目录、links、syscall及执行生命周期 |

不创建 `inputs/ preparation/ actions/ certificates/` 子目录，也不按每个优化再拆一对 pass 文件。文件拆分随负责逻辑迁移完成，不先做一轮纯搬家再复制旧实现。

## 11. 推进批次与旧机制退役

最新已测生产checkpoint为37：Block/Function time为1.081608280×/1.027932590×，cycles为1.081087685×/1.030169884×，instructions为1.027346518×/0.986518968×，均≤1.10×；去byte完整compile为1.018331577×/1.791281251×，均≤2×。本轮固定输入量化目标完成，依据aggregate receipt八项判定。spill25/42、maxframe48/64B保持，当前Home仍16B。directory计数/冷layout模型/真实probes与all-entry诊断继续分域；以下清单区分已实装与候选，候选后续按证据和明确成本决定，不计为本轮已测收益。

| 批次 | 完成内容 | 同批退役/收敛的旧逻辑 |
| --- | --- | --- |
| A：Uniform与位置 | 统一语义接口、混宽消费、pin窗口、真实tied destination | 分散的adjacent/delayed/narrow推导；为换view而发布再读 |
| B：死flags与选码 | 块内/块域需求归约、MUL溢出链DCE、UXTW/CSEL W/窄ALU、Block谓词直消费 | 无条件WriteFlags live-root；BOOL→COPY→CBZ尾链 |
| C：原生flags | raw PF/AF、最后reader迁移、统一pack/materialize边界 | 已迁移native路径的逐producer packed拼装与重复载体公式 |
| D：热边 | 非S查表、真实near relocation、cold PC发布、同owner撤链 | resolved边的PC准备/热STR及不必要LDR+BR |
| E：故障可见flags | 有界TEST恢复、当前self-loop tag及退出物化 | 合格路径的逐回环全flags维护；不新增通用lazy历史 |
| F：压力值（未实施候选） | 拟：首次eviction、同Home fill复用、跨块anchor保留 | 拟替代块内值“后来spill→从定义起一直在栈”的策略 |
| G：XMM驻留（已实施） | 同Uniform语义、原32槽窗口、固定pin ABI及真实冷/故障同步 | native完整XMM的state往返；不另造vector allocation/repair层 |

每批交付应是一条已接通的新路径并移除被替代逻辑，基线通过封存源码/镜像保留。硬件兼容的generic分支有实际用途，不与“保留旧native优化路径作开关”混在一起。

## 12. 成本、检查与验收

### 成本约束

- A 继续降低实际出生N；不为恢复Uniform接口先机械增加大量节点。
- B 保三次N扫描和一份u64/N，块域flags数据仅按B/实际边计。
- C 当前保唯一一份16B Home，物理选择直接成为最终编码输入；本轮没有新增N表或Home字段。§9的20B/4N仅是未实施的压力值备选，需证据与明确成本后决定，不计为当前成本或达标前置。
- 恢复与链接记录只按实际FaultSite/relocation产生；没有通用历史或每条IR证书。
- 两scope完整cold compile各≤2×同轮固定FEX，formation、IR、失败/retry、分配、编码、发布和释放继续计入，仅扣实际byte parser。

### 集中验证

开发中完成受影响部分的构建和必要合同检查，不逐opcode跑生成质量。复用现host合同、单合并guest和原Bionic输入；一个完整功能批结束才集中核实际语义/故障边界。只有新改动、失败或具体未决问题才增加定向检查。

最后在连接的红魔上统一比较两scope与固定FEX：原Bionic/CoreMark有效长跑、PMU总量及必要的一轮当前镜像热点；寄存器压力/跨pin快照用有限合并输入补足，不建立组合矩阵。FEX的fault flags不是SVM精确合同的oracle，故障字段按原语义检查；正常路径性能用同一输入与配置对照。

普通性能运行关闭byte时钟、命中计数和采样；compile用现显式诊断口径。Function失败与Block retry继续计费并报告，不以零fallback作为新硬门，也不能隐藏fallback把Function结果充作纯Function。

本轮固定原版Bionic/CoreMark输入在记录设备/配置下已实测通过：两scope time/cycles/instructions≤1.10×，去byte完整compile≤2×，集中功能/故障检查通过，aggregate receipt据实际八项门限判定完成。这个结论保持既定故障、TSO与成本边界，范围限本轮输入；全ISA、所有应用及未实施候选没有获得额外性能或功能保证。历史未达批次和原始占位字段继续保留，不将本轮通过追溯改写为旧批次达标。

## 13. 参考代码与设计核对

- FEX RegCache：`FEXCore/Source/Interface/Core/OpcodeDispatcher.h:1224–1268`。
- FEX SRA/RA寄存器池：`FEXCore/Source/Interface/Core/ArchHelpers/Arm64Emitter.cpp:74–82`。
- FEX SRA冲突/旧SSA保存：`FEXCore/Source/Interface/IR/Passes/RegisterAllocationPass.cpp:598–690`；kill与输出分配在`:730–753`。
- FEX flags归约：`FEXCore/Source/Interface/IR/Passes/RedundantFlagCalculationElimination.cpp`；信号恢复：`FEXCore/Source/Interface/Core/Core.cpp:208–247`。
- FEX真实分支/查表：`FEXCore/Source/Interface/Core/JIT/BranchOps.cpp:68–185`。
- 本轮三份设计复核及当前RX/PMU证据：`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-compare-20261103`。

全ISA、动态ELF、完整guest线程/信号平台不作为本轮新增前置条件；本方案优化当前已实现路径的生成质量与编译成本。
