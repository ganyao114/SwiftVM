# FPR publication、NZCV 与边界密度优化

日期：2026-08-24

基线：`fe0dbab`

FEX 参照：`f2e35f3`，`FEX_HOSTFEATURES=disableavx`

## 当前差距

正式 smallpt 使用 `smallpt_wh_x64 8 128 96`。在 Orb 默认 region、无
`SVM_EXEC_PROF` 的生产指令口径下，优化前为 1,321,651,162 条 host 指令，spill 为 0。
剔除 density 入口计数与 cold path 后，动态责任表闭合误差为 0.0002%：

| 类别 | host 指令 | 占比 |
|---|---:|---:|
| work | 499,336,597 | 37.781% |
| boundary | 315,449,440 | 23.868% |
| move/width | 296,127,192 | 22.406% |
| uniform | 138,886,309 | 10.509% |
| flags IR | 71,849,500 | 5.436% |

`boundary` 中 terminal 为 146,626,738（11.094%），link 为 171,798,869
（12.999%）。`move/width` 中 FPR state 为 72,854,485（5.512%），其中
`SetHostFPR` 为 64,757,147（4.900%）。主要单 op 为 `GetOperand` 8.036%、
`StoreUniform` 6.244%、`VecFMulScalar64` 5.969%、`LoadMemory` 5.801% 和
`SetHostFPR` 4.900%。spill 不是当前差距来源。

c-ray 同口径为 134,666,060 条 host 指令：work 43.354%、move/width 25.503%、
boundary 22.116%、uniform 6.522%、flags IR 2.506%；FPR state 为 5.484%，
`SetHostFPR` 为 5.055%。

8 月 23 日静态表中的 smallpt 为 SVM/FEX 2.180×。本轮同 harness、同旧 TSV
权重的 RE=0 重采从 SVM 3.335622 host/guest 降至 3.267832；加入 RSB 返回目标复用、
静态出口 direct-link、默认 return-L1、cycle successor layout、indirect-L1 状态成对加载、
live resident-FPR publication、scalar-load FPR fusion、scalar-sqrt resident publication、
legacy scalar-binary resident publication、direct absolute-address materialization 和 compact
FCMP PF/AF publication、trailing static-location cold publication、compact FCMP carrier
publication、semantic Nop elision、zero-register FPR lane publication 和 shared zero-store
materialization elision，再加入 simple CondSet saved-flags extraction 和 scaled memory
displacement encoding、direct single-bit TestFlags extraction 和 PSTATE-preserving zero
tests，再加入 live-PSTATE CSET materialization 和 negative GetOperand displacement lowering
以及 aligned L1 BFI address formation、register-offset memory EA preservation、
fault-exact stack-push pre-index store 和 same-page constant-address base reuse 后，按正式
host 权重折算约为 2.478306。
以未变的 FEX 1.549 为分母，对应 2.153×→1.600×。
旧表与这次重采的
unit 形成参数不完全相同，
因此不直接覆盖原表；按两种口径合看，当前正式 smallpt 距离 FEX 约 1.59–1.65×。

## 已落地

### Legacy scalar FPR publication

提交 `7110d20` 允许非 scalar-insert lowering 的完整 V128 scalar FP 结果直接拥有
resident XMM home。allocator 与 emitter 分别重证明；若 left 已占目标 home，则保留真实
publication，避免 legacy 多指令 lowering 覆盖仍需读取的高 lane。producer 与 publication
之间出现 memory/helper observer、冲突或额外存活值时仍拒绝。

这一项对正式 `smallpt_wh` 发码不变，但覆盖另一份固定 1024×768 smallpt：

- host 44,600,954,455→44,148,307,471，减少 452,646,984（1.015%）；
- 128 个共同 PC 只减不增，move 同量减少，spill 0→0；
- SwiftVM 两臂及 FEX PPM SHA-256 均为
  `fba9041fc8091204af5cccba5147d7fb84ac4d6abe2133b3e23ad2f2d1881a66`；
- c-ray 等 entries 口径减少 482,359 条，59 个共同 PC 只减不增，IDAT 不变。

### Full NZCV publication

提交 `7045d3b` 将完整 NZCV merge 从
`MRS + AND(flags) + AND(scratch) + ORR` 缩成三条。读取 `NZCV` 时除 [31:28]
外均为架构 RES0；完整请求下不再需要遮罩 scratch。部分请求继续走原四条路径。

| workload | 优化前 | 优化后 | 变化 |
|---|---:|---:|---:|
| CoreMark | 6,034,267,440 | 5,973,080,081 | −61,187,359（1.014%） |
| smallpt_wh | 1,321,651,162 | 1,303,939,990 | −17,711,172（1.340%） |
| c-ray 等 entries | — | — | −999,520 |

smallpt 有 1121 个共同 PC、c-ray 有 3909 个共同 PC 变小，均无增长 PC。move 与 spill
不变，说明收益精确来自 NZCV merge。

### VecZip resident publication

提交 `431be30` 把单条 `ZIP1/ZIP2` 纳入完整 resident FPR producer 集合。它与现有
`VecAnd/VecFAdd` 一样是单条三寄存器、完整 V128 写，沿用相同的存活、observer、冲突和
emitter 重证明。

- smallpt_wh 1,303,939,990→1,297,980,655，减少 5,959,335（0.457%）；
- 12 个共同 smallpt PC 只减不增；
- c-ray 等 entries 口径减少 124,308，29 个共同 PC 只减不增；
- CoreMark 仅运行尾部抖动，CRC final 保持 `0x382f`。

### Retained RSB return target

提交 `fa1768a` 让 dynamic `SetLocation` 穿过无副作用的 `PopRSB` 标记，并把仍驻留寄存器的
真实返回目标直接交给两种 RSB pop lowering。目标缺失、预测不符、空栈和 SMC 清空仍走原
dispatcher；终结段在 flags 合并前保留目标寄存器，避免 scratch 别名。

- smallpt_wh 1,297,980,655→1,296,969,640，减少 1,011,015（0.078%），328 个 PC
  只减不增，PPM 不变；
- `SVM_EXEC_PROF` 记录 8,080,989 次 RSB hit、248 次 miss，说明先前按静态块数估算的
  “仅 0.076% 上限”不是实际执行上限；
- CoreMark 等 entry 口径减少 25,442,605，327 个共同 PC 只减不增，CRC final 不变；
- call-dense workload 在 lean/default RSB frame 下均精确减少 64,000,000 条 host 指令，
  checksum 均为 `0xee79813b94536693`；
- c-ray 等 entry 口径减少 414,747，967 个共同 PC 只减不增，IDAT 不变。

### Static SetLocation direct link

提交 `729b826` 把 `SetLocation(imm) + ReturnToDispatch` 的同 module 静态出口接入现有
tracked direct-link。`JitContext` 只保留一份 site 发射与 flags-audit 逻辑；终结段继续在
成环边的 site 前发 acquire poll，并传递条件臂 `LinkSiteKind`。direct-link region 不可用、
BlockLink 关闭、自环或跨 module 时，仍走原 inline L2 或 dispatcher 路径。

- smallpt_wh 1,296,969,640→1,246,900,800，减少 50,068,840（3.860%）；998 个
  共同 PC 只减不增，entries 与 units 完全一致，PPM SHA-256 不变；
- smallpt 的 inline L2 `link_hit` 12,191,734→31,816、`link_miss` 227→0；direct、
  indirect、call、ret、RSB、dispatcher 和 region-edge 执行计数逐项不变；
- CoreMark 等 entry 减少 210,324,020，966 个共同 PC 只减不增，CRC final 为 `0x382f`；
- c-ray `scene.json -j 1 -s 64 -d 320x240` 等 entry 减少 574,050,830，4017 个
  共同 PC 只减不增，IDAT MD5 为 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 总量减少 18,740，859 个共同 PC 只减不增，`Solution Validates`；
- call-dense 3,344,000,774→3,104,000,764，减少 240,000,010，checksum 仍为
  `0xee79813b94536693`。

### Default returns through inline L1

提交 `24f9d49` 让 `indirect_l1` 模块的 call 不再生成 RSB frame，`ret` 直接复用已保留的
真实目标并走现有 inline L1。L1 快路径自带 `LDAR/TBNZ` signal safepoint，key mismatch
与 SMC-invalid value 仍退回 dispatcher/L2。若目标寄存器不可保留则直接 `Ret`；只有显式
`SVM_INDIRECT_L1=0` 的模块继续生成并消费 RSB frame，避免跨 module 留下 stale frame。

FEX `f2e35f3` 的两条 shadow-stack push 依赖 4 MiB call-ret mapping 和 guard-page fault
恢复；SwiftVM 当前只有 64-frame 普通数组，不能安全照抄删边界检查。默认 return-L1 则在
不引入异常恢复和 host-PC frame 的前提下移除整段 push，并缩短 pop。

- smallpt_wh 1,246,900,800→1,201,575,549，减少 45,325,251（3.635%）；881 个
  共同 PC 只减不增，entries 与 units 完全一致，PPM 不变；
- return 与普通间接出口合计 L1 profile 为 8,978,713 hit / 420 miss（99.9953%）；生产
  EXEC 中 RSB 8,080,989/248→0/0，仅增加 100 次 L2 hit/dispatcher，其他 exit、region 和
  guest-state 计数逐项不变；
- CoreMark 等 entry 减少 169,232,701（总量减少 169,232,746），892 个共同 PC
  只减不增，CRC final 为 `0x382f`；
- 64-spp c-ray 等 entry 减少 566,348,759（总量减少 566,459,798），3450 个共同 PC
  只减不增，IDAT 不变；
- STREAM 等 entry 减少 5,480、823 个共同 PC 只减不增，`Solution Validates`；
- call-dense 3,104,000,764→2,752,000,752，减少 352,000,012（11.340%），checksum
  不变；`SVM_INDIRECT_L1=0` 两臂均为 3,104,000,764，逐指令一致。

### Cycle-polled successor layout

提交 `b3998d5` 区分物理 successor layout 与真实 fallthrough。条件边的 then 目标若正好是
下一个 region block、同时承担 direct cycle poll，旧形态需要先跳到 then stub，并在非 then
臂再发一条无条件跳转。新形态反转条件直接跳向另一臂，随后保留原 `LDAR/CBNZ + B target`。
每块 cold stub 仍紧跟 hot body，因此 poll 后不得真实 fallthrough；signal、SMC 和冷出口均未
移动或删减。

- smallpt_wh 1,201,575,549→1,201,372,215，减少 203,334（0.0169%）；127 个共同 PC
  各少一条、0 个增长，entries 与 units 完全一致，PPM 不变；
- smallpt 静态 region local branch bytes 5,732→5,140，cycle edges/poll bytes 保持
  518/4,144；生产 EXEC 的 exit、region edge、cycle poll 和 fallthrough 逐项一致；
- CoreMark 等 entry 减少 51,081,278，122 个共同 PC 只减不增，CRC final 为 `0x382f`；
- 64-spp c-ray 等 entry 减少 328,719，424 个共同 PC 只减不增，IDAT 不变；
- STREAM 等 entry 减少 1,412，112 个共同 PC 只减不增，`Solution Validates`。

### Paired indirect-L1 state load

提交 `3ec9582` 将 `exit_request` 与 `indirect_l1_code_cache` 放在 `State` 的首个 16-byte
pair 中。production inline L1 用一条 `LDP` 同时取得请求字和 L1 基址，再用 `TBNZ`
筛出 signal；命中 signal 时先返回共享 trampoline，由其 offset-zero `LDAR` 确认请求并
返回 `Signal`。profile 路径保持原来的独立 L1 基址加载，不改变其计数语义。快路径从九条
降为八条，cache key/value 与 SMC-invalid 检查保持不变。

- smallpt_wh 1,201,372,215→1,199,466,420，减少 1,905,795（0.1586%）；376 个共同
  PC 各少一条、0 个增长，entries、units 和 PPM 均一致；
- CoreMark 等 entry 减少 31,825,037，373 个共同 PC 只减不增，CRC final 为 `0x382f`；
- 64-spp c-ray 等 entry 减少 91,110,798，1,137 个共同 PC 只减不增，IDAT 不变；
- STREAM 等 entry 减少 879，341 个共同 PC 只减不增，`Solution Validates`；
- call-dense 等 entry 精确减少 64,000,000，6 个共同 PC 只减不增，checksum 不变；
  scale-10 十次 wall-time 中位数 1.045108s→1.044901s，未出现 `LDAXP` 原型的退化。

### Live resident-FPR publication

提交 `e74e734` 允许完整 V128 producer 在 `SetHostFPR` 后仍有普通 SSA use 时继续占用
目标 resident home。publication 前仍拒绝 fault/helper observer、固定家读写和重叠 live
interval；publication 后若同一目标在 producer 最后 use 前再次写入也拒绝。一个 SSA 一旦
占用 resident home，不能再被后续 publication 改绑到另一个 home。emitter 独立复算同一
窗口，`SetHostFPR` 只在两侧证明一致时消失。

- smallpt_wh 1,199,466,420→1,187,471,711，减少 11,994,709（1.0000%）；79 个共同
  PC 缩短、0 个增长，entries、units、spill 和 PPM 均一致；
- 删除量主要来自完整 `LoadMemory` 4,704,551、`VecFMul` 1,550,057、`VecFAdd`
  1,390,954、scalar FP 1,661,934、`VecXor` 851,875、`VecZip` 833,261 和
  `LoadUniform` 696,744；
- 320×240、64-spp c-ray 等 entry 减少 136,043,687，174 个共同 PC 缩短、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 等 entry 减少 199，26 个共同 PC 缩短、0 个增长，`Solution Validates`；
  CoreMark 在当前口径基本中性，CRC final 保持 `0x382f`。

### Scalar-load FPR fusion

提交 `030f52d` 识别同一 resident home 的相邻 low-64 `LoadMemory` publication 与 high-64
`LoadImm(0)` publication。两侧 producer 都必须单用；从 load 到 publication 之间不得有
fault/helper、local control、目标 fixed-home 读写或重叠 FPR live interval。命中时原
`LDR X + MOV zero + 2×INS` 由原 fault site 上的一条 `LDR Dtarget` 代替；AArch64 D-register
load 同时清零 V[127:64]。其余形态继续逐条发射，且 load 点再次完整复证计划。

- 审计中的同目标配对为 16,092,695，14,574,717 次具有相邻 store；最终严格窗口命中
  6,285,771 次，每次精确减少三条；
- smallpt_wh 1,187,471,711→1,168,614,398，减少 18,857,313（1.588%）；64 个共同
  PC 缩短、0 个增长，所有 delta 均为三的倍数，entries、units、spill 和 PPM 均一致；
- 320×240、64-spp c-ray 等 entry 减少 115,072,803，27 个共同 PC 缩短、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 等 entry 减少 174，7 个共同 PC 缩短、0 个增长，`Solution Validates`；
  CoreMark 等 entry 为 0，CRC final 保持 `0x382f`。

### Scalar sqrt resident publication

提交 `97009a3` 允许 legacy scalar `VecFUnary(kind=sqrt)` 的完整 V128 结果直接继承已驻留的
merge home。merge 必须是同目标、offset 0 的已合并 `GetHostFPR`，且其最后使用必须精确落在
producer；producer 到 publication 之间仍沿用完整 observer、fixed-home 和 live-overlap 门。
allocator 与 emitter 分别复证。命中后 legacy lowering 省掉 merge 自复制，最终
`SetHostFPR` 也成为空发射；merge 仍存活的形态保持原路径。

- 残余归因中 `VecFUnary` 为 2,119,260 次；严格窗口命中 1,555,871 次，每次精确减少两条；
- smallpt_wh 1,168,614,398→1,165,502,656，减少 3,111,742（0.2663%）；20 个共同
  PC 均精确缩短两条、0 个增长，entries、units、spill 和 PPM 均一致；
- 320×240、64-spp c-ray 等 entry 减少 1,036,470，1 个共同 PC 缩短两条、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 与 CoreMark 等 entry 均为 0，分别保持 `Solution Validates` 与 CRC final
  `0x382f`。

### Legacy scalar-binary resident publication

提交 `f99eabf` 允许 legacy scalar Add/Sub/Mul/Div 在 left 是同目标、offset 0 的已合并
`GetHostFPR` 且最后使用精确落在 producer 时复用 resident home。32-bit lowering 在独立
scalar 临时值完成修复后直接插回 lane0；64-bit lowering 不能原地执行，否则会先破坏待保留
的高 lane，因此改用已保留的 NaN ABI 临时寄存器计算，再只插回 lane0。八个重复 emitter
收敛到一个 legacy scalar-binary 方法，没有增加 scratch 预算或产品开关。allocator 与
`SetHostFPR` emitter 仍分别复证全部 publication 窗口。

- 正式 smallpt 严格命中 2,482,103 次，每次删除最终完整 publication；
- smallpt_wh 1,165,502,656→1,163,020,553，减少 2,482,103（0.2130%）；31 个共同
  PC 缩短、0 个增长，entries、units、spill 和 PPM 均一致；
- 320×240、64-spp c-ray 等 entry 减少 107,640,218，83 个共同 PC 缩短、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 等 entry 仅减少 1，3 个静态 PC 缩短、0 个增长并保持 `Solution Validates`；
  CoreMark 等 entry 为 0，CRC final 保持 `0x382f`。

### Direct absolute-address materialization

提交 `b692fca` 把既有 `abs_const_mat` 机制翻为默认 ON：绝对 guest address 直接向
`GetOperand` 的分配结果物化，不再先占 scratch 再做一次 transport。语义、IR、fault site、
寄存器存活窗口和 cache feature hash 均不变；`SVM_ABS_CONST_MAT=0` 仍可回退旧代码形状。

- 当前 smallpt 中 21,753,480 次三指令 `GetOperand` 各删一条；
- smallpt_wh 1,163,020,553→1,141,267,073，减少 21,753,480（1.8704%）；794 个共同
  PC 缩短、0 个增长，entries、units、spill 和 PPM 均一致；
- 320×240、64-spp c-ray 等 entry 减少 243,851,552，1,924 个共同 PC 缩短、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 与 CoreMark 等 entry 分别减少 1,137 / 673，652 / 659 个共同 PC 缩短、0 个
  增长，分别保持 `Solution Validates` 与 CRC final `0x382f`。

### Compact FCMP non-NZCV publication

提交 `c15a712` 把 compact `PublishFCmpFlags` 的 parity-byte 写入和 AF 清零合为一条
27-bit `BFI`。`VecFCmp` 的 `CSET VC` 结果严格为 0/1；x26 bit0–7 是 raw parity byte、
bit26 是 AF，bit8–25 没有读者且本 opcode 已使 flags token 失效，因此合并不改变任何
可观察状态。原 `AXFLAG` 与 lazy host NZCV 契约保持不变，density 记为 shared PF/AF pack。

- 正式 smallpt 的 14,894,552 次 publication 各精确删除一条；
- smallpt_wh 1,141,267,073→1,126,372,521，减少 14,894,552（1.3051%）；144 个共同
  PC 缩短、0 个增长，entries、units、spill 和 PPM 均一致；
- 320×240、64-spp c-ray 等 entry 减少 88,615,708，118 个共同 PC 缩短、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 与 CoreMark 等 entry 分别减少 23 / 4，11 / 4 个共同 PC 缩短、0 个增长，
  分别保持 `Solution Validates` 与 CRC final `0x382f`。

### Trailing static-location cold publication

提交 `2f2fb88` 只延迟块内最后一条有效 IR 的常量 `SetLocation`。中块异常位置仍按原顺序
立即发布，动态目标仍直接写 `current_loc`；块尾常量目标则由 direct-link/L1 先尝试转发，
只在空 slot、不可链接或其他 terminal 返回 dispatcher 前发布。cycle signal stub 本身已经
携带对应目标，因此信号恢复语义不变。

- 正式 smallpt 的 10,013,768 次块尾常量发布各删除三条热指令；
- smallpt_wh 1,126,372,521→1,096,331,217，减少 30,041,304（2.6671%）；998 个共同
  PC 各缩短三条、0 个增长，entries、units、spill 和 PPM 均一致；
- 320×240、64-spp c-ray 等 entry 减少 344,393,514，4,151 个共同 PC 缩短、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 与 CoreMark 等 entry 分别减少 9,702 / 126,194,385，854 / 960 个已执行共同
  PC 缩短、0 个动态增长；CoreMark 仅一个 entries=0 的未执行版本因布局增长 9 条，
  动态贡献为 0。两者分别保持 `Solution Validates` 与 CRC final `0x382f`。

### Compact FCMP carrier publication

提交 `523d679` 在 consumer proof 成立时让 `VecFCmp` 的 `CSET VC` 直接写 x26：bit0
保留 ordered/raw-parity，W 写同时清 AF，后续 `PublishFCmpFlags` 只需 `AXFLAG`。证明只接受
紧邻的 compact publish，以及可选的 `LoadImm + StoreUniform + AdvancePC` 后
`FCmpCondSet`；其他消费者继续生成独立 ordered SSA。`FCmpCondSet` 的 ordered/unordered
路径从 x26 bit0 读取，通用 IR 语义不变。

- smallpt_wh 1,096,331,217→1,081,436,665，减少 14,894,552（1.3586%）；144 个共同
  PC 缩短、0 个增长，entries、units、spill 和 PPM 均一致；
- 320×240、64-spp c-ray 等 entry 减少 88,615,708，118 个共同 PC 缩短、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 等 entry 减少 23，11 个共同 PC 缩短、0 个增长并保持 `Solution Validates`；
- CoreMark 的四个 FCMP PC 合计稳定减少 4，CRC final 保持 `0x382f`。三组 A/B 中
  非 FCMP 冷块 `0x4668fd` 的一次 entry 偶发形成 9 条布局差异，第二组消失，按 opcode
  与重复样本隔离后不计入本阶段收益。

### Semantic Nop elision

提交 `e1257d6` 让 IR `Nop` 不再发射 ARM `NOP`。guest NOP、PAUSE、prefetch、fence 等
无状态提示仍经过 decode/translate 并保留 `dynamic_next_loc` 等编译期元数据效果；只删除
backend 中不可观察的 host 指令。代码池放置与对齐使用独立的 VIXL 路径，不受影响。

- 正式 smallpt 的 8,991,381 次 `Nop` 各精确删除一条，1,081,436,665→1,072,445,284，
  减少 8,991,381（0.8314%）；245 个共同 PC 缩短、0 个增长，entries、units、spill 和
  PPM 均一致；
- 320×240、64-spp c-ray 等 entry 减少 141,118,113，837 个共同 PC 缩短、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 与 CoreMark 等 entry 分别减少 1,390 / 57,725,341，221 / 280 个共同 PC
  缩短、0 个增长，分别保持 `Solution Validates` 与 CRC final `0x382f`。

### Zero-register FPR lane publication

提交 `b1e501e` 扩展既有默认开启的 `zero_store_zr`：单用、未 spill 的整数
`LoadImm(0)` 若唯一消费者是 `SetHostFPR`，不再先把零物化到普通 GPR；固定 FPR lane
直接从 `wzr/xzr` 写入。共享 helper 同时统一 StoreUniform、StoreMemory 和 SetHostFPR 的
资格判断；multi-use、pseudo observer、spill 和非零值继续走原路径，没有新增机制开关。

- 正式 smallpt 1,072,445,284→1,068,863,253，减少 3,582,031（0.3340%）；41 个共同
  PC 缩短、0 个增长，entries、units、spill 和 PPM 均一致；
- 320×240、64-spp c-ray 等 entry 减少 114,264,417，144 个共同 PC 缩短、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 等 entry 减少 46，5 个共同 PC 缩短、0 个增长并保持 `Solution Validates`；
  CoreMark 等 entry 为 -23，只有冷布局级的 6 缩短 / 1 增长，按中性处理，CRC final
  保持 `0x382f`。

### Shared zero-store materialization elision

提交 `215a059` 去掉 `zero_store_zr` 的单用限制：一个未 spill 的整数 `LoadImm(0)` 只有在
所有 use 都是同块 StoreUniform、StoreMemory 或 SetHostFPR 的 value operand 时才完全不
物化，每个消费者继续直接读取 `wzr/xzr`。证明用包含 pseudo 的全局 use count 与本块兼容
use 数精确闭合；跨块、地址复用、算术/pseudo observer、spill、浮点和非零值全部拒绝。

- 正式 smallpt 1,068,863,253→1,064,572,872，减少 4,290,381（0.4014%）；74 个共同
  PC 缩短、0 个增长，entries、units、spill 和 PPM 均一致；
- 320×240、64-spp c-ray 等 entry 减少 114,750,206，142 个共同 PC 缩短、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 等 entry 减少 5，14 个共同 PC 缩短、0 个增长并保持 `Solution Validates`；
  CoreMark 等 entry 为 0，CRC final 保持 `0x382f`。

### Simple CondSet saved-flags extraction

提交 `21000f1` 在 x26 为权威状态时直接提取 EQ/NE、CS/CC、MI/PL、VS/VC。正向条件用
一条 `UBFX`，反向条件追加一条 `EOR`；host PSTATE 仍存活或条件为 HI/LS、GE/LT、GT/LE
时继续使用既有 NZCV 恢复路径，没有新增开关或兼容兜底。

- 正式 smallpt 1,064,572,872→1,060,138,659，减少 4,434,213（0.4165%）；3,364 个
  unit 和 3,613 个 version 完全一致，101 个共同 PC 缩短、0 个增长，spill 0→0；
- 320×240、64-spp c-ray 等 entry 减少 21,411,778，293 个共同 PC 缩短、0 个增长，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM/CoreMark 等 entry 分别减少 2,168 / 2,210，均为 53 个共同 PC 缩短、0 个增长，
  并保持 `Solution Validates` / CRC final `0x382f`。

### Scaled memory displacement encoding

提交 `0fc245c` 让 direct-mode `[base + imm]` 同时接受 AArch64 unsigned scaled offset，
避免把已按访问宽度对齐的正位移先 `MOV` 到寄存器。pair、shift、writeback 和 bounded-bias
路径继续使用原判定，没有新增开关。

- 正式 smallpt 1,060,138,659→1,060,040,252，减少 98,407（0.0093%）；18 个共同 PC
  缩短、0 个增长，unit/version/entry 与 spill 均一致；
- c-ray、STREAM、CoreMark 等 entry 分别减少 93 / 8 / 8，全部只减不增，PPM、IDAT、
  `Solution Validates` 和 CRC final `0x382f` 保持一致。

### Direct single-bit TestFlags extraction

提交 `723ace5` 对单个 N/Z/C/V 的数值读取直接提取 host bit。x26 为权威状态时只发一条
`UBFX`；PSTATE 为权威状态时使用 `MRS + UBFX`，不再用会破坏 NZCV 的 `TST + CSET`
及恢复序列。PF/AF 和多 bit 测试继续使用原 lowering，没有新增开关。

- 正式 smallpt 1,060,040,252→1,052,965,418，减少 7,074,834（0.6674%）；81 个共同
  PC 缩短、0 个增长，unit/version/entry 与 spill 均一致；
- c-ray 等 entry 减少 32,621,346，221 个有执行的共同 PC 缩短；唯一增长的共同 PC
  entries 为 0，IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM/CoreMark 等 entry 分别减少 1,100 / 7,481,108，并保持 `Solution Validates` /
  CRC final `0x382f`。

### PSTATE-preserving zero tests

提交 `8ca4b0b` 在 pending guest flags 仍位于 PSTATE 且后续没有本地 Goto/NotGoto/BindLabel
时，以 `CLZ + LSR` 生成 zero 布尔值，nonzero 再追加一条 `EOR`。这样不再为随后的
`CMP + CSET` 预先发布 guest flags。无限制原型会使 zero-count rotate 的本地控制流丢失
ZF，既有 3-assertion repro 捕获了该问题；最终证明显式拒绝该形态。

- 正式 smallpt 1,052,965,418→1,047,125,252，减少 5,840,166（0.5546%）；净变化
  完全等入口，spill 0→0，PPM 不变；
- c-ray 等 entry 减少 21,374,140，增长布局的动态总量仅 278，IDAT MD5 保持
  `d0c71130abf3544a86b64417bc488c21`；
- STREAM 等 entry 减少 472；CoreMark 等 entry 增加 79,936（0.0015% 布局波动），
  `Solution Validates` 与 CRC final `0x382f` 保持一致。

### Live-PSTATE CSET materialization

提交 `aa7b83d` 将 PSTATE 为权威状态的单 bit `TestFlags` 从 `MRS + UBFX` 缩为一条
不会修改 NZCV 的 `CSET`；x26 为权威状态时仍使用单条 `UBFX`。

- 正式 smallpt 1,047,125,252→1,043,588,497，减少 3,536,755（0.3378%）；74 个共同
  PC 缩短、0 个增长，unit/version/entry 与 spill 均一致；
- c-ray 等 entry 减少 16,348,046，148 个共同 PC 缩短；唯一增长 PC 的 entries 为 0，
  IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM/CoreMark 等 entry 分别减少 30 / 3,740,015，并保持 `Solution Validates` /
  CRC final `0x382f`。

### Negative GetOperand displacement lowering

提交 `a505485` 对 AArch64 add/sub immediate 可编码的负 `GetOperand` 偏移直接发射 `SUB`，
不再先物化有符号立即数再执行 `ADD`。

- 正式 smallpt 1,043,588,497→1,042,807,357，减少 781,140（0.0749%）；等 entry
  减少 781,195，99 个共同 PC 缩短、0 个增长，spill 保持 0；
- c-ray 等 entry 减少 10,527,227，288 个共同 PC 缩短、0 个增长；
- STREAM/CoreMark 等 entry 分别减少 214 / 59；PPM、IDAT、STREAM validation 和
  CoreMark CRC 全部精确一致。

### Aligned L1 BFI address formation

提交 `d9eb980` 让 direct-hash L1 表按完整表跨度对齐，使 inline return 和 dispatcher
能以单条 `BFI` 形成 16-byte entry 地址，替代 `AND + ADD`。entry 数量、线性探测和
失效值契约不变。

- 正式 smallpt 1,042,807,357→1,040,901,562，减少 1,905,795（0.1828%）；377 个共同
  PC 缩短、0 个增长，unit/version/entry 不变，spill 保持 0；
- 正式 c-ray raw / 等 entry 分别减少 90,922,904 / 91,098,755，1,129 个等 entry PC
  缩短、0 个增长，IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM raw / 等 entry 分别减少 941 / 875；CoreMark raw / 等 entry 分别减少
  31,825,004 / 31,825,027；validation 与 CRC final `0x382f` 保持一致。

### Register-offset memory EA preservation

提交 `e2f9527` 让 direct 模式的 `[base + index]` 地址保持为 memory IR 的复合 operand，
由 ARM64 memory emitter 直接使用 register-offset encoding，不再先生成中间 `GetOperand`。

- 正式 smallpt 1,040,901,562→1,040,846,721，减少 54,841（0.0053%）；45 个共同 PC
  缩短、0 个增长，unit/version/entry 不变，spill 保持 0；
- 正式 c-ray raw / 等 entry 分别减少 5,506,465 / 5,469,864，200 个等 entry PC
  缩短、0 个增长，IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM 等 entry 减少 111；CoreMark 等 entry 减少 640,298，66 个 PC 缩短、0 个增长，
  validation 与 CRC final `0x382f` 保持一致。

### Fault-exact stack-push pre-index stores

提交 `4821182` 对连续的 `Sub(RSP,size) -> StoreMemory -> SetHostGPR(RSP)` 做严格后端证明，
在 direct 模式下以单条 AArch64 pre-index store 完成地址递减、存储和 RSP 发布。同步异常
发生在基址写回之前；biased-memory 和 base/data overlap（`push rsp`）继续走原路径。

- 正式 smallpt 1,040,846,721→1,001,905,579，减少 38,941,142（3.7413%）；228 个共同
  PC 缩短、0 个增长，unit/version/entry 不变，spill 保持 0；
- 正式 c-ray raw / 等 entry 分别减少 401,378,477 / 400,778,948，1,108 个等 entry PC
  缩短、0 个增长，IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM raw / 等 entry 分别减少 5,508 / 4,228；CoreMark raw / 等 entry 分别减少
  100,077,187 / 100,077,202；validation 与 CRC final `0x382f` 保持一致。

### Same-page constant-address base reuse

提交 `6b10c73` 把绝对地址缓存从“地址逐位相同”推广到“同一 4 KiB guest page”。一个通过
完整 idle-window 与 scratch 复证的 GPR 保存 page base，后续普通 memory operand 以 AArch64
scaled/unscaled offset 访问；biased-memory 路径逐次重物化精确 guest 地址。修正后的机制默认
ON，`SVM_CONST_ADDR_CACHE=0` 可回退。

- 正式 smallpt 1,001,905,579→984,381,707，减少 17,523,872（1.7491%）；47 个共同 PC
  缩短、0 个增长，unit/version/entry 不变，spill 保持 0；
- 正式 c-ray raw / 等 entry 分别减少 227,905,071 / 227,602,716，67 个等 entry PC
  缩短、0 个增长，IDAT MD5 保持 `d0c71130abf3544a86b64417bc488c21`；
- STREAM raw / 等 entry 分别减少 152 / 216；CoreMark raw / 等 entry 分别减少 147 / 216；
  validation 与 CRC final `0x382f` 保持一致。

二十九项合计使正式 smallpt 默认 region host 减少 337,269,455（25.5188%）。

### Pinned GPR direct consumers

默认 `SVM_X86_PIN_EXT=2` 固定 RAX/RCX/RDX/RBX/RSP/RBP/RSI/RDI/R8-R11，共 12 个
x86 GPR；R12-R15 只在 level 3 固定到 x6-x9。W60 已证明 level 3 会使 4,400-unit host
代码增加 3.89%、spill memory operation 从 5,425 增至 14,071，并使受载 CoreMark 比
level 2 下降 10.18%，因此不翻默认。

已有正式 smallpt 日志中，SetHostGPR 的 86,980,059 次加权 IR 有 72,003,699 次发码为零，
实际剩 14,976,360 条 host move；GetHostGPR 的 129,472,617 次加权 IR 有 123,911,206 次
发码为零，实际剩 5,561,411 条。提交 `e10fec4` 允许同一个 audited consumer 在多个 operand
位置直接读取 pinned W view，并让 callee-saved pinned U8/U16/U32 直接进入
`SXTB/SXTH/SXTW`。若同一快照还有后续 consumer，仍保留原 `UBFX`。

旧正式 entries 对 `test eax,eax` 和 pinned-W sign extension 两个已证明热块的权重分别为
2,054,236 / 706,866，机械上对应至少减少 2,761,102 条 host 指令（约为当前正式总量的
0.2805%）。遵照停止长时间压力测试的要求，本轮未重跑正式 smallpt，故不修改
984,381,707 和 1.600× 的正式标题数据。Mac 增量构建通过；3 个新用例与 3 个相邻 GPR
回归共 6 cases / 429 assertions 通过，未运行完整套件。

### Indexed-shuffle resident publication

提交 `2634fe4` 将 `VecShuffle32Indexed` 纳入完整 V128 resident-home publication。RA 与
ARM64 emitter 分别维护并复算同一 producer 集合；既有 observer、fixed-home、live-range
和冲突门不变。该 op 的普通 `TBL` 与已证明的 0x4e `EXT` lowering 均允许结果和 source
别名，因此 producer 可以直接写入目标 home，尾部 `ORR` publication 消失。

- bounded `4 8 6` 全写 census 在改动前观察到 267 个静态 copy、4,826 次加权执行；
  `VecShuffle32Indexed` 占 221 次，改动后该池全部消失；
- 严格 A/B 的 PC/version 均为 2,757 / 3,597，host/entry 与 top-20 覆盖 100%，PPM
  逐字一致、spill 为 0、无增长 PC，common host `580,841 -> 580,620`，减少 221
  （0.038048%）；
- root tracing 证明 `BitCast` 不是独立 producer 池：559 次中 558 次回溯到
  `VecFAddScalar64`，1 次回溯到 `GetHostFPR`。按根重算共有 4,582 次 scalar64 copy；
  其中 1,883 次链头来自同一 resident home，但精确两节点/sole-use 原型只兑现 384 次。

该阶段只运行短基准和定向测试，没有运行长 benchmark 或完整 suite；临时 census 已删除。

### Callee-saved pinned subtraction input

提交 `613dd12` 允许唯一 U8/U16/U32 consumer 为 `Sub` 时，x19–x29 callee-saved fixed home
直接提供 W view。既有扫描仍要求同一 read 的全部 named use 落在该 consumer，并在遇到
同 home 写入时停止，因此 capture、宽度和 helper-clobber 边界不变；没有扩展到其他 op。

- 严格 `4 8 6` A/B 的 2,757 PC / 3,597 version、100% host/entry 和 top-20 coverage、PPM
  与零 spill 全部保持；common host `580,620 -> 580,290`，减少 330（0.056836%），十个
  PC 缩短且无增长；
- pinned-GPR 六个 focused cases 通过 14 assertions；固定 seed 424242、各 256 iteration
  的 ALU/mixed fuzz 在两臂均保持既有 88 / 106 divergence；
- 同样放开 callee-saved `Add` 只减少 51（0.008789%），已完整删除。

该阶段没有新增开关，没有运行长 benchmark 或完整 suite。

### Narrow subtraction shifted-operand alignment

提交 `5f9ebac` 只处理窄 `Sub` 的实测 `LSL #0` 右操作数：原路径先把该 operand 复制到
临时 W 寄存器，再以 U8/U16 的 sign-bit shift 送入 `SUBS`；现在直接把既有寄存器编码成
`SUBS ..., LSL #24/#16`。非零 shift、immediate、composite 与 `Add` 均保持旧路径。

- 轻量 full short-run emitter census 在改动前将 56,852 条加权 host instruction 归到
  `Sub`，其中 span-4/5 热点由该准备 copy 主导；
- 严格 `4 8 6` A/B 的 shape、100% coverage、PPM、spill 与 no-growth 门全部保持，common
  host `580,290 -> 573,037`，减少 7,253（1.249892%）；十个 PC 各缩短一条；
- pinned-GPR focus 通过 16 assertions，flags elimination / SaveCV / CondSet 分别通过
  32 / 4 / 62；固定 seed 424242 的 ALU/mixed fuzz 在两臂均为既有 88 / 106 divergence。

### Sign-extension fixed-home publication

提交 `993acce` 把 `SignExtend` 纳入既有 last-use GPR publication producer 集。RA 与 emitter
仍独立维护同一分类，输入活跃、fixed-home 冲突、observer 和 publication 窗口证明不变；
`SXTB/SXTH/SXTW` 的目的与源允许别名，结果可直接写入 fixed home。

- 严格 `4 8 6` A/B 保持 2,757 PC / 3,597 version、100% coverage、PPM、零 spill 和无增长
  PC，common host `573,037 -> 569,108`，减少 3,929（0.685645%）；八个 PC 缩短；
- expanded GPR producer accepted/conflict/emission 矩阵通过 367 assertions，pinned-GPR
  focus 通过 16；固定 seed mov/extend 与 mixed fuzz 在两臂均保持 98 / 106 个既有差异；
- 同阶段 SetHostGPR 根 census 的 31,705 次实际发码中，13,002 次 `GetHostGPR` 根是不同
  guest home 之间的真实复制；`SignExtend` 的 3,944 次池已经关闭。

两个阶段都没有新增开关，没有运行长 benchmark 或完整 suite；临时 census 已删除。

### Narrow logical flag direct collapse

提交 `5a47163` 关闭了 smallpt 中成片出现的窄 `TEST reg,reg` 形态。前端对 U8/U16 同寄存器
自测只读取一次，不再生成无结果消费者的 `And`；后端对无 data use 的 `Or(value, 0)` 直接
从 `value` 发布逻辑标志。低位 `BitExtract` 只有在紧邻该精确 flag direct、只有这一处 use
且带 flags pseudo 时才允许与输入绑定。U8/U16 通过 `ADDS wzr, wzr, value, LSL #24/#16`
一次得到正确 N/Z 并清 C/V，U32/U64 直接 `TST`；需要急切物化 PF 的旧路径仍保留。

- bounded no-detail `4 8 6` host-dump A/B 两臂观察到相同 556 个 PC，其中 193 个缩短、0 个
  增长，合计减少 937 条静态 host 指令；热点 `0x419287` 从 260B 降到 240B，原
  `UXTB + UXTB + AND + MOV + SXTB + TST` 只剩一条 NZ producer；
- 与 retained formal log 做 exact-version 交集后覆盖 12.038773% formal host weight，191 个
  加权 PC 缩短、0 个增长，已经减少 43,721,205 条 formal-weighted 指令，占完整 formal
  host total 的 0.221854%。覆盖不足，以上只能视为保守下界，不能换算新的 FEX ratio；
- 两臂短 PPM SHA-256 均为
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`；logical shape 与
  flags/SaveCV/CondSet focus 共通过 120 assertions，固定 seed 424242 的 ALU/mixed fuzz
  保持既有 88 / 106 divergence；
- 严格 short collector 连续两次在 15 秒硬上限终止，均未生成 hot record。没有放宽超时，
  没有运行长 benchmark 或完整 suite，也没有加入开关或保留 check。

### NZCV publication and restore compaction

提交 `477947d`、`e389f9d`、`5b94050`、`183bf78` 和 `46197f6` 关闭了 flags 路径中五个同源
的大池。连续的部分 NZCV 发布统一为 `MRS + UBFX + BFI`，完整和非连续掩码由同一个 emitter
负责，region/backedge 不再维护重复实现。恢复时直接执行 `MSR NZCV, x26`；AArch64 NZCV
系统寄存器只接收 31:28，VIXL 的 `NZCVWriteIgnoreMask` 也明确把其余位标为 write-ignore。
region published veneer 复用现有 `TargetKillsIncomingFlags` 全覆盖证明，目标在观察或故障前
完整覆盖 incoming flags 时不再恢复 PSTATE。逻辑类同时清 C/V/AF 时，bits 26:29 的连续区间
由一条 `BFC` 清除；其他非连续组合保持原发码。

- partial 阶段的 556-PC 同形样本有 219 个缩短、0 增长，静态减少 561 条；正式权重
  交集减少 38,750,240 条。region 完整发布继续减少 16,608,202 条；
- 直接 `MSR` 阶段保持 558 PC / 558 version 完全一致，264 个缩短、0 增长，静态减少 2,068
  条，12.038818% 正式交集减少 179,793,723 条，占完整正式总量 0.912324%；
- dead published-entry restore 阶段同形，186 个缩短、0 增长，静态减少 673 条，正式交集减少
  42,596,165 条；C/V/AF 阶段同形，269 个缩短、0 增长，静态减少 668 条，正式交集减少
  52,706,706 条；
- 从本轮起点到最终样本，`0x47f6c0` 的冷 printf unit 分裂出 `0x47f8a8` / `0x47f8f0`。该
  PC 只有 240 entries、36,960 formal host weight，明确排除后，555 个共同 PC 中 329 个
  缩短、0 增长，静态减少 4,093 条；541-PC exact-version 子集覆盖 12.038585%，累计减少
  330,449,276 条 formal-weighted 指令，占完整 formal total 的 1.676794%。覆盖不足且有冷
  unit 成形变化，所以仍只作为保守下界，不换算新的 FEX ratio；
- 最终短 PPM SHA-256 仍为
  `a70375e511474ad45215f93df3e2c3db44af41afe40bb1c76e0f14d5528ea7b1`。flags codegen focus
  通过 10 cases / 142 assertions，region/trampoline/L1 focus 通过 5 cases / 224 assertions，
  辅助 region 集通过 3 cases / 63 assertions；固定 seed 424242 的 256-iteration ALU/mixed
  fuzz 保持既有 88 / 106 divergence。没有运行长 benchmark、stress 或完整 suite，没有
  新增开关或保留临时 check。

## 否决项

把所有窄 `TEST` 的 `And -> Or(0)` 扩成直接 `And` flags producer，并把两侧 low extract 都
绑定到原寄存器的原型没有通过 no-growth 门。8 秒上限下的 496-PC 共同样本中，29 个 PC
缩短但 31 个增长，静态净减少仅 13 条；retained formal exact-version 子集只减少 623 条
加权指令。IR 缩短扰动了 RA 排布，收益远低于复杂度，原型已完整回退。

直接放开 Linux AFP scalar insert 曾使 c-ray host 134,666,060→132,076,475，
但 smallpt 输出与 FEX 分离；单独关闭 scalar tie 后仍是同一错误输出，证明问题在 scalar
insert 契约而非 RA tie。该原型已完整删除。

`VecFUnary` 的通用 producer 白名单仍不成立：packed、RCP/RSQRT 以及 merge 非最后使用的
形态继续拒绝。本轮只批准 legacy scalar sqrt 的完整 merge-home 证明。

终端尾部回边轮询曾尝试把 `LDAR + CBNZ cold + B target` 改为 `LDAR + CBZ target`；正式
smallpt 两臂均为 1,168,614,398，证明现有 successor layout 已吸收所有安全命中。原型和
接口改动已完整删除。

同值 carry 极性发布去重在本地回归通过，但正式 smallpt 等 entry 仅减少 23,703
（0.0022%），同时 137 个 PC 增长、17 个缩短且 units/entries 改变，不具备稳定收益；
原型已完整删除。

把 legacy scalar-binary 的固定左源从精确 `GetHostFPR` 放宽到任意同 resident home 的
SSA 链后，正式 smallpt 两臂均为 1,072,445,284，3,364 个共同 PC 全部 byte-identical；
该条件不是剩余 publication 的限制来源，原型已完整删除。

新的 scalar-insert 链审计把 4,582 次 scalar64-root full copy 分成 1,883 次同 home 链和
其余真实跨 home/非 resident 链。精确两节点、sole-use 且 observer 安全的原型仅使短筛
`580,620 -> 580,236`，减少 384（0.066136%），但需要约 390 行 RA/emitter 双证明；收益与
复杂度不匹配，源码和专项测试已完整删除。`BitCast` 本身零发射，不另立优化池。

TestZero/TestNotZero 的 generated local condition 即使允许 FLAGS=1 下跨
`LoadImm + StoreUniform + AdvancePC`，正式 smallpt 也仅减少 919，收益不成比例；原型
已完整删除。`LoadImm(0) → SetHostGPR` 已被既有 GPR coalescer 吸收，smallpt / c-ray
等 entry 仅为 -1 / -22，同样完整删除。

透明 `BitCast` 零值存储图证明通过既有 537-assertion 矩阵，但正式 smallpt 的 3,364 个
unit 与 c-ray 的所有等 entry PC 均逐字不变，说明剩余零物化不受该透明节点限制；原型已
完整删除。

legacy scalar FP 的两条指令分别承担低 lane 运算和 x86 高 lane 保留，不是普通 copy；
单条路径依赖 AFP/NEP，而该路线已被 exact smallpt 输出否决。StoreUniform 侧的同块 DSE、
XMM fault sink 和 XMM1-11 驻留均已启用；XMM0 有既有墙钟回退证据，XMM12-15 会重新压缩
已关闭的动态 FPR 池，因此本轮不扩展驻留范围。

剩余 21.75M 个 left-immediate `GetOperand` 是必须分两段构造的绝对常量；ADRP/literal
替代不满足当前 relocation 与 mapping 契约，这条路线关闭。

saved-flags compound CondSet 的 `HI/LS` 与 `GE/LT` 两指令原型通过 88-assertion 结构门，
但在正式 smallpt、CoreMark 和 c-ray 审计样本中的动态命中均为 0；`GT/LE` 仍需三个输入。
原型与临时 cond 探针已经完整删除。

## 验证

- scalar-load fusion 的结构/fault 测试 2 cases / 12 assertions、新 live-publication 窗口
  测试 3 cases / 9 assertions；既有 FPR 责任测试 3 cases / 10
  assertions、resident XMM coalescing 794 assertions、scalar fixed-home tie 90 assertions、
  resident fault/capture 27 assertions，Mac 与 Orb 全部通过。
- func_tests：FLAGS 0/1 × function/block/interpreter 六格均 rc=101，checksum
  `9f52b7d59285dbe5`。
- helper-fault 38 passed / 0 failed；clone futex/lock 在 FLAGS 0/1 下均 rc=0。
- function fingerprint 对阶段基线保持 1664 units / 11 guests，自一致且逐项匹配。
- AVX VEX.128 move differential 在固定 seed 424242 下通过 Mac 与 Orb，覆盖 memory VMOVQ
  的 high-half zero 语义。
- scalar sqrt resident-publication 测试通过 2 个形态 / 6 assertions，覆盖精确两指令
  缩短和 merge 继续存活时的拒绝；既有 FPR focus 在 Mac 与 Orb 均为 8 cases /
  119 assertions。
- legacy scalar-binary resident-publication 将 FPR focus 扩为 9 cases / 125 assertions；
  64-case x86 NaN 真值矩阵在 Mac/Orb、`SVM_SSE_AFP_NAN=0`、cold path 0/1 四格均通过，
  覆盖新 64-bit 保高 lane 路径及两种精确 NaN 修复。
- absolute-address 三项定向门在 Mac/Orb 均通过 3 + 1 + 10 assertions；ON/OFF
  function fingerprint 为 1664 units / 11 guests 且逐项一致，func_tests 的 ON/OFF ×
  function/block/interpreter 六格均 rc=101、checksum `9f52b7d59285dbe5`、stdout hash 相同。
- COMIS compact flags 全消费者 JIT/interpreter 差分在 Mac/Orb 均通过 3,482 assertions；
  FLAGS 0/1 × function/block/interpreter 六格逐字一致，阶段 fingerprint 仍为
  1664 units / 11 guests。
- trailing static-location 的 Mac/Orb fallback focus 均通过 39 assertions，Orb cycle signal
  通过 30 assertions，反复 delink/relink 通过 142 assertions；固定 seed 全量保持
  191 passed / 35 个既有失败和相同的 45 个失败断言位置。FLAGS 0/1 ×
  function/block/interpreter 与前一版共十二格逐字一致，fingerprint 仍为 1664 units /
  11 guests。
- compact FCMP carrier 在 Mac/Orb 的全消费者差分均通过 3,482 assertions，flags focus
  均通过 46 assertions；固定 seed 全量保持 191 passed / 35 个既有失败和相同的 45 个
  失败断言位置。基线/候选 FLAGS 0/1 × function/block/interpreter 十二格逐字一致，
  fingerprint 仍为 1664 units / 11 guests。
- semantic Nop elision 的 x86 Nop family、RSB/indirect 结构、direct-link fallback 和 flags
  focus 在 Mac/Orb 分别通过 1、26、39、46 assertions；COMIS 全消费者差分通过 3,482
  assertions。固定 seed 全量保持 191 passed / 35 个既有失败和相同的 45 个失败断言位置；
  基线/候选 FLAGS 十二格逐字一致，clone futex/lock 四格均 rc=0，fingerprint 仍为
  1664 units / 11 guests。
- zero-register FPR lane publication 将既有 zero-store fail-closed 矩阵扩为 405 assertions，
  Mac 全部通过；FPR publication/fault focus 的 4 + 2 + 3 + 2 + 7 + 5 assertions 和 flags
  46 assertions 在 Mac/Orb 均通过，COMIS 两端均通过 3,482 assertions。基线/候选 FLAGS
  十二格逐字一致，fingerprint 保持 1664 units / 11 guests；Orb 固定 seed 全量仍为
  191 passed / 35 个既有 failed cases，当前为 44 个失败断言且没有新增失败类别。
- shared zero-store 证明把 Mac fail-closed 矩阵扩为 537 assertions，覆盖两个兼容 store
  共享零值和算术/pseudo/spill 拒绝。helper-fault 为 38/0，FPR/flags focus 与 COMIS
  3,482 assertions 在 Orb 通过；基线/候选 FLAGS 十二格、zero-store OFF/ON 六格和
  1664-unit/11-guest fingerprint 全部一致。Orb 固定 seed 全量保持 191 passed / 35 个
  既有 failed cases / 44 个失败断言。
- simple CondSet 结构矩阵在 Mac/Orb 均通过 62 assertions，flags focus、SaveCV 和 COMIS
  分别通过 46 / 4 / 3,482 assertions；基线/候选 FLAGS 十二格逐字一致，helper-fault 为
  38/0，1664-unit/11-guest fingerprint 全部一致。顶层 seed 424242 的重复运行保持
  191 passed / 35 个既有 failed cases；嵌套子进程 seed 会使既有 config/fuzz 失败断言在
  44–45 间波动，没有新增失败类别。
- 既有 address/host-base focus 在 Mac/Orb 均通过 39 assertions；基线/候选 FLAGS 十二格
  与 1664-unit/11-guest fingerprint 逐字一致。顶层 seed 424242 保持 191 passed / 35 个
  既有 failed cases / 45 个失败断言。
- flags focus 在 Mac/Orb 均通过 46 + 12 + 24 + 4 + 62 assertions，COMIS 两端均通过
  3,482 assertions；基线/候选 FLAGS 十二格、helper-fault 38/0 和
  1664-unit/11-guest fingerprint 一致。顶层 seed 424242 保持 191 passed / 35 个既有
  failed cases / 45 个失败断言。
- 既有 zero-rotate repro 在 Mac/Orb 均通过 3 assertions；flags focus、COMIS、FLAGS
  十二格、helper-fault 38/0 与 1664-unit/11-guest fingerprint 全部保持。顶层 seed
  424242 恢复为 191 passed / 35 个既有 failed cases / 45 个失败断言。
- flags focus、zero-rotate repro 与 COMIS 在 Mac/Orb 保持通过；基线/候选 FLAGS 十二格和
  1664-unit/11-guest fingerprint 一致。顶层 seed 424242 保持 191 passed / 35 个既有
  failed cases / 45 个失败断言。
- address/host-base focus 在 Mac/Orb 均通过 39 assertions，基线/候选 FLAGS 十二格逐字
  一致。候选自一致为 1,657 units / 11 guests；相对 1,664-unit 基线有 7 个 unit 合并，
  decoded-block 与 IR 总量同步下降，所有 guest 输出精确一致。顶层 seed 424242 为
  191 passed / 35 个既有 failed cases / 44 个失败断言。
- aligned-L1 阶段的七指令结构门在 Mac/Orb 均通过 26 assertions，inline-L1 signal/SMC
  invalidation 通过 13，trampoline 通过 154，排除既有 disk-cache 失败的 production
  direct-link 在 Mac/Orb 通过 393 / 349 assertions。基线/候选 FLAGS 十二格逐字一致，
  1,657-unit/11-guest fingerprint 匹配；顶层 seed 424242 为 191 passed / 35 个既有
  failed cases / 45 个失败断言，失败文件集合不变。
- register-offset EA 阶段的 address focus 在 Mac/Orb 均通过 84 assertions；基线/候选
  FLAGS 十二格逐字一致。候选自一致为 1,657 units / 11 guests；unit/decoded-block 总量
  不变，六个 guest 的 aggregate IR 合计减少 152。顶层 seed 424242 保持 191 passed /
  35 个既有 failed cases / 45 个失败断言，失败文件集合不变。
- stack-push 结构与 fault recovery 在 Mac/Orb 均通过 3 cases / 13 assertions；基线/候选
  FLAGS 十二格逐字一致，function fingerprint 为 1,657 units / 11 guests 且逐项匹配。
  排除新增三个专项 case 后，同 seed 基线/候选均为 191 passed / 35 个既有 failed cases /
  44 个失败断言，失败位置集合一致；既有 nested-child 波动仍为 44–45。
- constant-page cache 结构在 Mac/Orb 均通过 16 assertions，覆盖同址、同页不同址、scratch
  不足和 biased-memory 精确地址回退。Cache OFF/ON FLAGS 十二格与 bounded-bias func_tests
  逐字一致，function fingerprint 为 1,657 units / 11 guests 且逐项匹配。最终默认 ON 与
  rollback 套件均为 194 passed / 35 个既有 failed cases / 45 个失败断言，失败位置一致。
- RSB/indirect 结构测试 26 assertions，覆盖七指令 L1 快路径、无 push 和无目标
  dispatcher 路径；显式改写栈返回地址的临时 check 在默认、L1-off 两种 RSB frame、
  FLAGS-off 和 interpreter 下均 rc=0，check 已删除。
- 新增 production inline-L1 signal 测试 6 assertions；FLAGS=0 下 direct-link production
  全标签 11 cases / 395 assertions，默认 SMC 子集 5 cases / 268 assertions。静态
  SetLocation 的跨 module/BlockLink-off fallback 为 36 assertions，反复摘链/重编译为
  142 assertions，Mac 与 Orb 均通过。
- region edge、direct cycle signal 和 region flags 专项分别通过 42、30、46 assertions；
  cycle successor layout 的 function fingerprint 对基线保持 1664 units / 11 guests 一致。
- 200 轮 `smc_mt_stress` 为 host_fails/guest_lost/timeouts = 0/0/0。
- Catch 与 fuzz seed 同为 424242 时，最新 Orb tree 为 191 passed / 35 个既有 failed
  cases / 44 个失败断言；失败 case 数和类别不变，仍是既有配置敏感与 fuzz 类别。
- indexed-shuffle publication 的 resident-XMM accepted/conflict 矩阵通过 854 assertions，
  scalar fixed-home tie 通过 90，PSHUFD 全 immediate golden model 通过 6,914；VEX.128
  directed 通过 76，固定 seed 424242 的 256-iteration fuzz 通过。SSE batch-B directed
  通过 210，既有 JIT/interpreter differential 仍精确为 392 个 divergence。
- callee-saved pinned `Sub` 的 U8/U16/U32 结构门将 pinned-GPR focus 扩为 6 cases /
  16 assertions，并覆盖窄 `Sub` 不再产生 shifted-operand preparation；flags elimination、
  SaveCV、CondSet 分别通过 32 / 4 / 62。固定 seed 424242 的 256-iteration ALU/mixed fuzz 为
  88 / 106 个既有 divergence。scratch-price focus 的 U64 large-imm 项在两臂同样为
  24/25，属于既有过时期望，不是本阶段回归。
- `SignExtend` 扩展后的 GPR producer accepted/conflict/emission 矩阵通过 367 assertions；
  固定 seed mov/extend 与 mixed fuzz 在基线/候选均保持 98 / 106 个既有 divergence。
- smallpt_wh PPM SHA-256 两臂均为
  `fe96f7e48295b27c8df8236294052d138c3ed130b81d022739907fe6b2cde5aa`；
  原 equal-entry c-ray sample 的 IDAT MD5 两臂均为
  `54256cb4b3c6313a65ea12ebb7b81e30`，64-spp 正式 harness 两臂均为
  `d0c71130abf3544a86b64417bc488c21`；STREAM `Solution Validates`。
- Mac `swift_test` target 与 Orb 全目标构建通过。

## 下一步

当前 single-version opcode ledger 覆盖正式 smallpt host 执行的约 82.85%。剩余单 op host
责任最高的是 StoreUniform 82.41M、VecFMulScalar64 78.89M、LoadMemory 76.55M、GetOperand
65.87M、VecFAddScalar64 58.03M、LoadUniform 56.33M 和 StoreMemory 51.64M。

本轮已经关闭部分/完整 NZCV 掩码、`MSR` 前掩码和可证明死亡的 published-entry restore。
剩余高权重 full merge 与单条 `MSR` 在 committed x26 ABI 下已是局部指令下界；下一轮若继续
从 flags 大头推进，需要设计跨 unit pending-flags / dual-entry ABI，不再继续堆掩码 peephole。

1. GPR pin 应按实际 host bytes 而不是 IR 数量判断。level 2 保持 12/16 固定映射；level 3
   的 spill/host 膨胀已经否决。剩余 SetHost 热 move 多数是 guest 架构寄存器之间的真实复制，
   只能继续寻找 consumer 直接读取 fixed home 的形态，不能把真实 `mov` 当 publication 删除。
   ordinary StoreMemory、callee-saved `Sub`、窄 `Sub` 的 `LSL #0` preparation 和
   `SignExtend` publication 已关闭；同构 `Add` 仅 `-51`，不再扩池。当前 bounded census
   的 31,705 次实际 SetHostGPR 发码中，13,002 次 `GetHostGPR` 根是 guest home 间真复制。
   另有 5,157 次 `Sub` 根来自 Mac biased-memory 的栈更新；faulting StoreMemory 位于 Sub
   与 RSP publication 之间，不能提前覆盖 x19。Linux direct 的精确形态已由 pre-index
   store 合并，因此这不是剩余 FEX 对齐池。
2. 当前正式 smallpt 的已覆盖 link 约 6.6%。region/cycle link tail 约 2.1%，其中
   acquire poll 与跨本块 cold stub 的目标跳转不可直接删除；
   七条 return-L1 静态序列约 1.26%，地址形成已缩为 `BFI`，剩余
   `LDP + CMP + CSEL + BR` 没有明确的基础 ISA 融合机会。公开 host exit 仍仅 139 次。
   剩余 `SetLocation` 均为动态
   目标或后面仍有观察点，不能继承块尾常量证明。
3. 剩余 `SetHostFPR` 约为 30,193,585（2.848%），其中完整写约 8,317,804（0.785%）。
   low-64 `LoadMemory` 与 high-64 zero 分别余 10,641,609（1.004%）/ 10,093,484
   （0.952%）；所有 use 都兼容的 high-zero 常量物化已经消除，剩余数值是 publication IR 次数。
   相邻池中约
   8.29M 次因 fault/alias/fixed-home 门拒绝，不为继续扩池放宽精确状态边界。
   bounded short-run 全写 census 的 4,826 次 copy 中，`VecShuffle32Indexed` 的 221 已关闭；
   `BitCast` 559 次实际回溯为 558 次 `VecFAddScalar64` 加 1 次 `GetHostFPR`。按根统计的
   4,582 次 scalar64 copy 中，1,883 次同 home 链的严格原型也只兑现 384，且证明复杂度
   不成比例，已删除。没有新的 alias/observer 载体前不再扩池。
4. direct `[base+imm]`、`[base+index]` 与 access-size 匹配的 scaled index 已直接进入
   memory emitter。剩余复合 EA 涉及 bias/32-bit wrapping、shift 或 AArch64 不可编码的
   scale；只有同时给出 encoding 与 wrap 证明才扩展。
5. CoreMark 的 8/16-bit truncation 仍要求 consumer-specific 物理高位证明，并保留 U16
   CallLambda 回归门。
6. SHA 必须先换成能真正进入 hashing 的合法 workload；不使用当前 PageFatal 前的路径计数。
