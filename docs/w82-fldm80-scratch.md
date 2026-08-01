# W82：helper-fault `fld_m80` GPR scratch 契约根修

日期：2026-08-01

基线：`590bd35`

## 结论

这不是单纯的“预算声明漏 1”，而是 W52 XPOOL 改造后留下的两层契约漂移：

1. `fld_m80` 的 `LoadFloat(Float80)` 原实现实际连续取得 8 个动态 GPR。
   报错中的第 7 个是 `sign_exp`，其后还有第 8 个 `reg_address`；发射
   `0x7fff`、`0xc5ff` 等立即数时，VIXL 还可能临时取得 1 个寄存器。
2. W52 把 `StoreInt`/`Compare` 原先固定使用的 x12/x13 改成动态
   `GetTmpX()`，但把整个 `X87Op` 的 XPOOL 预算写成 6。两臂因此一度达到
   10 个动态临时，并可能把 VIXL 的 x11-x17 池完全占满。

因此根因分类是 **真实超用与声明漏记并存**。只把全局/整个 opcode 的 6 改成
7，只能越过第一处断言：随后会在 `Compare(Register)` 请求第 8 个，或在
VIXL immediate materialization 处因无可用寄存器中止。实验中的逐级现象是：

```text
budget=6: fld_m80 -> declared 6, asked for 7
budget=7: Compare(Register) -> declared 7, asked for 8
粗放放大预算: VIXL !available->IsEmpty / mac spill reload 容量不足
```

最终修复没有全局加 1，而是恢复并细化既有 scratch/fixed-clobber 契约：

- `fld_m80` 通过严格生命周期复用将动态峰值从 8 压到 6；
- `StoreInt` 将已固定的 x13 在转换前复用为 x87 槽地址，动态峰值为 7；
- `Compare` 用固定 x12/x13 承载两侧 bit pattern，按需重算右物理槽，动态峰值为 6；
- X87 发射期间固定保留 ip0，只把 ip0 交给 VIXL 做立即数物化；VIXL 不再与
  emitter 的动态池竞争；
- scratch budget 和 fixed clobber 新增 `Inst` 级查询，RA reserve、RA verify、
  fixed-live 排除和 JIT 发射断言全部消费同一份按 command 的契约。

helper fault 仍由原 W63/W57 路径处理。本次没有改 fault sink、fault
continuation、guest 状态发布或 helper 的 PageFatal 语义。

## 1. 复现与定位

Orb master 对照：

```sh
cd /private/tmp/w64/source/translator/linux/tests
./run_helper_fault_tests.sh \
  /tmp/svm-build/source/translator/linux/svm_translator_linux
```

当前对照实际是 37/38，而不是任务描述中的 36/38：

```text
PASS  fld_m80[default]
FAIL  fld_m80[SVM_X87_JIT=1] HOST killed by signal (rc=134)
PASS  fld_m80[SVM_ENABLE_JIT=0]
helper-fault suite: 37 passed, 1 failed
```

mac 主机没有 `/tmp/svm-build`；用复验期保留的 pre-fix `/tmp/w80-build`
重复同一命令，结果同样是 37/38，且唯一失败也是
`fld_m80[SVM_X87_JIT=1]`。因此这是双平台既有问题；当前二进制上未复现
default 格失败。

最小 guest 的异常原文：

```text
Check Failed! scratch GPR budget exceeded emitting opcode 7:
declared 6, asked for 7
```

`OpCode::Void=0`，所以 opcode 7 是 `X87Op`。沿 `EmitX87Op` 的
`LoadFloat/Float80` 分支逐项计数，原顺序为：

```text
1 fsw, 2 ftw, 3 top, 4 shift, 5 tag, 6 significand,
7 sign_exp, 8 reg_address
```

断言发生在翻译/发码期，guest 指令尚未进入 host fault recovery。由此排除
“fault sink、continuation 保存或 PageFatal 恢复额外占了第 7 个”的假说；
第 7 个就是 ext80 的 sign/exponent 临时。

历史定位：W52 `94cf351` 删除了 X87 emitter 对 ip0/ip1 的旧排除，并把
XPOOL 预算从 8 写成 6；同一提交还把 `StoreInt`/`Compare` 的固定 x12/x13
改成四个额外动态租赁点。源码的“six emitter temporaries”注释与实际所有
inline arm 不一致。

## 2. 修复内容与不变量

### 2.1 command 级预算与固定 clobber

`ScratchBudget(const ir::Inst&)` 在 XPOOL 生效时读取 X87 command：

| action | 动态 GPR 预算 | 固定 clobber |
|---|---:|---|
| `LoadFloat(Float80)` | 6 | ip0（VIXL only） |
| `Compare` | 6 | x12/x13 + ip0 |
| `StoreInt` | 7 | x12/x13 + ip0 |
| 其它 X87 inline arm | 保守上界 8 | ip0 |

opcode-only API 继续存在，供没有 `Inst` 的旧上下文使用；真正有指令的四条
契约链均改用 `Inst` 重载：

1. unit 最大 reserve 收集；
2. linear-scan 分配结果 verify；
3. interval fixed-clobber 排除；
4. `TickIR`/`GetTmpX`/`EndVixlScratch` 发射期断言。

固定寄存器由 RA 独立排除，不重复计入动态 scratch。X87 的 VIXL scope 只包含
ip0，避免先租到普通动态池寄存器；`EndVixlScratch` 只把非 fixed acquisition
计入动态预算。

### 2.2 emitter 生命周期复用

- `fld_m80`：输入 tag 检查发生在 guest significand load 之前，故先借用
  `significand`；旧 `shift` 在 tag 检查后改作 x87 slot address；分类完成后
  已死的 `sign_exp`/`significand` 分别改作 tag bit position/mask。guest 状态
  写入顺序、slow helper 分支和 fault 点均不变。
- `StoreInt`：固定 x13 (`converted`) 在真正 FCVT 写结果之前没有值，先用作
  x87 slot address，删除独立 `reg_address`。
- `Compare`：固定 x13/x12 保存 left/right bits；右物理 index 在 tag 检查和
  右 operand 转换前各自重算一次，删除长期存活的 `right_physical`、
  `right_address` 和动态 `left_bits`。

## 3. 验证结果

| 门 | 结果 |
|---|---|
| Orb helper-fault | PASS：38/38；default、`SVM_X87_JIT=1`、interpreter 全部通过 |
| mac helper-fault | PASS：38/38，同上 |
| x87 directed default | PASS：2,635 assertions（mac/Orb） |
| x87 directed `SVM_X87_JIT=1` | PASS：2,635 assertions（mac/Orb） |
| mac 全量 `swift_test` | PASS：139,975 assertions / 124 cases，等于基线 |
| Orb 全量 `swift_test` | PASS：142,585 assertions / 124 cases，等于基线 |
| func_tests 六格 | mac/Orb 均 PASS：function/block/interpreter × X87_JIT 0/1，全部 rc=101，checksum `9f52b7d59285dbe5` |
| func_tests stdout | 六格、双平台 SHA-256 均为 `9c3194ff498da03869bbabbd81241fd2ec771619281f969c4b4edc55577a6810` |
| x87 fuzz | seeds 101/202/303/404/505 × X87_JIT 0/1 × mac/Orb，共 20 次全过 |
| 指纹 A/B vs master | PASS：4,400 units，IR/totals 零 diff；额外保留 `host=` 原始字段后仍逐行零 diff |
| 指纹自一致 | PASS：新二进制两遍 4,400 units，含 host_bytes 一致 |
| 静态检查 | `git diff --check` PASS |

指纹语料的 11 个 guest 不含 x87 指令，所以 4,400-unit 门对 host_bytes 也是
零 diff，不能把它解释为 `fld_m80` 发码未变。用 helper-fault 生成器单独构造
`fld_m80` guest：master 在 unit 完成前即断言中止，没有可比较的旧
`[svm-unit]` 行；修复版成功发出：

```text
[svm-unit] pc=0x400078 ir=64 host=560
```

因此不存在可逐行列出的 master host diff；可归因的发码变化严格局限于
`LoadFloat(Float80)` 的临时寄存器复用，以及 `StoreInt`/`Compare` 恢复固定
x12/x13 并减少临时生命周期。IR 完全未改。

主要复验命令：

```sh
# 双平台分别运行（Orb 路径相同但置于 orb -m ubuntu 内）
/tmp/w82-build/source/tests/swift_test
SVM_X87_JIT=1 /tmp/w82-build/source/tests/swift_test \
  'x87 directed edge semantics'
SWIFT_FUZZ_SEED=101 SVM_X87_JIT=1 \
  /tmp/w82-build/source/tests/swift_test 'Fuzz x86 x87'

cd source/translator/linux/tests
./run_helper_fault_tests.sh \
  /tmp/w82-build/source/translator/linux/svm_translator_linux
./run_func_fingerprint_tests.sh \
  /tmp/w82-build/source/translator/linux/svm_translator_linux \
  --against /tmp/svm-build/source/translator/linux/svm_translator_linux
```

## 4. 改动文件

- `source/runtime/backend/reg_alloc.h`, `reg_alloc.cpp`：新增 Inst 级预算与
  fixed-clobber 契约，纠正 X87 账目。
- `source/runtime/ir/opts/register_alloc_pass.cpp`：reserve、verify、interval
  排除改用 Inst 级契约。
- `source/runtime/backend/arm64/jit/jit_context.cpp`：发射期预算/fixed 查询对齐；
  X87 VIXL 固定使用 ip0。
- `source/runtime/backend/arm64/jit/translator_x87.cpp`：`fld_m80`、StoreInt、
  Compare 临时生命周期收紧并恢复 x12/x13 固定使用。
- `source/tests/main_case.cpp`：现有 scratch-contract 验证改读 Inst 级预算；
  不新增 case/assertion，所以全量计数与基线一致。
- `docs/w82-fldm80-scratch.md`：本报告。

未修改 `source/translator/linux/linker/`、
`SwiftVM-bench/harness/run_matrix.sh` 或 golden；未执行 commit/add/checkout/
reset/stash/push，也未重新生成指纹 golden。
