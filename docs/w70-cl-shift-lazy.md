# W70：CL shift lazy flags 现状核查与关闭结论

日期：2026-08-01  
基线：`2e7bb8f`  
分支：`w70-clshift-lazy`

## 1. 结论

W70 在实测门关闭，不新增 `SVM_FLAGS_CL_LAZY`，`kGetenvNames` 保持 54。

W38/W59 与当前 FlagsElimination 已经删掉 terminal CL shift 的大部分 flags 写。
剩余可机械消除的是 count=0 本地 guard：flag body 已空时仍有 `cmp+cset+cbz`
三条，body live 时可折成一条 `CBZ`。一个 RA 安全的 emitter 原型把 7zip host
code 降了 1,424 bytes / 356 个 instruction slots（仅 0.1282%），但最终交错 A/B
为负：

- Tot MIPS 中位数 2997 -> 2943，ON/OFF = **0.9820**；
- 墙钟中位数 32.338s -> 32.749s，OFF/ON = **0.9875**；
- 2/3 对同向负。

这既不足 0.3% 门槛，也没有正收益。按任务要求“残余不足或噪声内则关闭、不硬做”，
原型与定向测试均已撤回，工作树只保留本核查报告。

## 2. 现状核查

### 2.1 方法与触发频率

Orb `wine-ci`，`/tmp/w70-build`，禁用 JIT disk cache，运行：

```text
7zip_x64 b -mmt1 -md=16m
SVM_DUMP_IR=1 SVM_DUMP_IR_POST=1 SVM_FLAGS_DEBUG=1
SVM_VIXL_HOST_DUMP=1 SVM_RA_SHAPE_PROF=<path>
```

运行 rc=0；共 6,944 units，当前 master host 合计 1,110,576 bytes / 277,644
个 4-byte instruction slots。

诊断记录到 125 次 CL shift 翻译、114 个唯一 guest site：

| 形态 | 唯一 site |
|---|---:|
| SHL 32/64 | 38 / 10 |
| SHR 32/64 | 58 / 2 |
| SAR 32/64 | 1 / 5 |

7zip 热路径没有 8/16 位样本。FlagsElimination 后的 125 个 guard 中，104 个
（83.2%）body 已清空；其余 21 个仍保留 flags 写，body 大小分布为 9 条 IR ×4、
11 条 ×2、13 条 ×15。

### 2.2 当前形状

LZMA 热区 unit `0x42c480`、guest PC `0x42c49e` 为 `shl %cl,%eax`。pass 前是：

```text
count = And(RCX, 0x1f)
result = LslValue(EAX, count)
cond = TestNotZero(count)
NotGoto(cond, done)
SaveFlags(result, N/Z/P)
SetCarry(...)
SetOverflow(...)
StorePolarity(Direct)
BindLabel(done)
```

flags pass 对所在 block 的统计为 `SaveFlags 4 -> 1`、`SetC/V 4 -> 0`；post-IR
只剩 `LslValue; TestNotZero; NotGoto; BindLabel`，guard body 已空。master host 中
对应片段是：

```text
and  w9, w23, #0x1f
lsl  w8, w7, w9
cmp  w9, #0
cset w7, ne
cbz  w7, next_instruction
mov  w7, w8
```

因此 W44 所说的“每次都完整物化 flags”在当前 master 已不成立；这个热点只剩
三条空 guard。另一个 `0x42c9d5` 的 flag body 仍 live，因为 flags 要带出 unit；
它不是可以跨 W30 edge/helper/fault 契约随意删除的死写。

### 2.3 W38/W59 覆盖判断

- W38 窄 flags 对齐不覆盖本次观测到的 32/64 位热点，也不负责零 count 的本地
  控制流。
- W59 terminal branch-only 与一般 FlagsElimination 已吸收主体：83.2% 观测点
  的 flag body 已经清空。
- 剩余 21 个 live body 若改成跨 edge lazy source，需要新造跨块机制，正确性成本
  远高于本任务的小项；本次只评估了可证明安全的 local guard 折叠。

## 3. count=0 语义与原型边界

当前 frontend 先按 x86 规则把 count 掩成 5 bit（64 位操作数为 6 bit）。
`NotGoto(TestNotZero(count))` 在零 count 时跳过 `SaveFlags`、CF/OF 与 polarity 的
全部写入；frontend 随后把 carry polarity 标为 Unknown，消费者按运行时 polarity
字节归一化。因此 count=0 会保留旧 x26、PF/AF、carry 与其极性。

原型保持同一 IR，只在 ARM64 emitter 折叠 guard：

- body 空：删除 `cmp+cset+cbz`；
- body live：在 `TestNotZero` 位置直发 `CBZ masked_count,label`，原 flag body 不动；
- `CBZ` 不写 host NZCV，guard 内没有 guest memory、helper 或 fault 点；
- 8/16 位 frontend 在 shift 后多一个窄 mask，原型只接受该精确形状；
- count>width 的 8/16 位仍为非零并执行原 body；32/64 位的 width 值经 5/6-bit
  mask 变成零并保留旧 flags。

代码审查还否决了第一个“在 NotGoto 位置读 count”的版本：IR 认为 count 的 last-use
在 `TestNotZero`，到 `NotGoto` 时物理寄存器可能已被复用。最终计入 A/B 的安全版把
`CBZ` 放在 `TestNotZero` 发射点；后续 `NotGoto` 只抑制旧分支。早期版本虽在低压
测试中通过，但违反 RA 生命周期，不能作为可合并实现或性能证据。

## 4. 原型静态净账

RA 安全版与 OFF 的 7zip host dump：

| 状态 | units | host bytes | instruction slots |
|---|---:|---:|---:|
| OFF | 6,944 | 1,110,576 | 277,644 |
| ON | 6,944 | 1,109,152 | 277,288 |
| 差额 | 0 | **-1,424** | **-356** |

125 个诊断点的实际 guard 指令删除为 104×3 + 21×2 = 354；总 slots 额外下降 2
来自 unit 尾部对齐。总 host code 只降 0.1282%。

代表 unit：

- `0x42c480`（空 body）：440 -> 428 bytes，删除 3 条；hash
  `05e69a34cc5ea253 -> ce38b7b8e16536f4`。
- `0x42c9d5`（live body）：276 -> 268 bytes，删除 2 条；hash
  `0e956bea77756819 -> a646e5155d1db6bf`。

## 5. 7zip A/B

最终 RA 安全版，单线程 `-md=16m`、JIT cache 关闭、三对交错，每次 rc=0：

| pair | OFF wall / MIPS | ON wall / MIPS | ON MIPS / OFF |
|---|---:|---:|---:|
| 1 | 33.047s / 2915 | 33.284s / 2892 | 0.9921 |
| 2 | 32.338s / 2997 | 32.749s / 2943 | 0.9820 |
| 3 | 32.130s / 3009 | 32.085s / 3016 | 1.0023 |
| median | **32.338s / 2997** | **32.749s / 2943** | **0.9820** |

墙钟中位 ratio（OFF/ON）为 0.9875，即 ON 约 -1.25%；MIPS 中位为 -1.80%。

另有一轮早期五对曾显示 MIPS +1.03%、墙钟 +0.50%，但该轮使用的是随后因越过
RA last-use 被否决的版本，不能用于落地判断。安全版的最终数据优先，且与仅
0.1282% 的静态上限相符。

## 6. 原型验证记录

这些门用于确认原型没有暴露功能错误；由于原型最终撤回，它们不是新增代码的落地门：

| 门 | 原型结果 |
|---|---|
| OFF 指纹 vs master | PASS；4,400 function units，零 diff；同 build 两遍含 host_bytes 自一致 |
| ON 指纹自一致 | PASS；4,400 units；同一 ON binary A/B 含 host_bytes 一致 |
| CL shift 定向 OFF/ON | PASS；每态 434 assertions；SHL/SHR/SAR × 8/16/32/64 × count 0/1/width-1/width/width+1 |
| func_tests 六格 | PASS；function/block/interpreter × OFF/ON 均 rc=101、checksum `9f52b7d59285dbe5` |
| macOS 全量 OFF/ON | PASS；原型加定向断言后两态均 140,177 / 123；master 记录为 139,961 / 123 |
| Orb Linux 全量 OFF/ON | PASS；原型加定向断言后两态均 142,787 / 123 |
| Orb master 同条件 | PASS；142,571 / 123；原型的 +216 assertions 全来自临时 CL 定向覆盖 |

一次误带 CMake AVX/XSAVE 可选门的 Linux 运行扩大到 265,604 assertions，并命中
既有的进程内 XGETBV 环境互扰；它不是项目记录的 142,571 同条件门，已用 W70 与
`/tmp/svm-build` 默认环境各自重跑并全绿，未计作验收结果。

## 7. 最终工作树与纪律

- 只新增 `docs/w70-cl-shift-lazy.md`。
- 不新增 `SVM_FLAGS_CL_LAZY`；不改 emitter、frontend、解释器、flags pass 或测试；
  `kGetenvNames` 保持 54。
- 未修改 `source/translator/linux/linker/`、`SwiftVM-bench/harness/run_matrix.sh` 或
  `func_fingerprint_golden.txt`。
- 未执行 commit/push/add/checkout/reset/stash。

建议：关闭 W44 候选 #4。若未来已有其他 pass 顺带改变 CL guard 形状，可把
“空 guard 清理”作为通用 control-flow canonicalization 的附带项重新测量；不建议
为当前 0.1282% 静态残余单独维护开关或专用 matcher。
