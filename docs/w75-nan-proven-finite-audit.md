# W75：proven-finite NaN guard 消除可行性核查

## 结论摘要

本轮只读核查基于 `w75-nan-audit` / `23c9b74`，没有实现优化，也没有改源码、
golden、linker 或 harness。

结论是：**关闭该候选，不批准实现 spike**。

- Orb Linux c-ray `-j 1 -s 8 -d 160x120` 复测得到
  `1,766,797,595 / 26,827,294,354 = 6.585821%` NaN guard 机械占比，复现
  W71 的 6.59%。smallpt `8 160 120` 为
  `956,165,789 / 8,789,802,698 = 10.878126%`。
- W71 hot top-20 覆盖 c-ray guard 的 **75.710247%**。逐块反汇编后，143 个
  guarded FP site、332 条静态 guard 与探针逐块完全相等；其中没有一个能用当前
  unit-local IR 保守证明结果非 NaN。源码/工作量特化地放宽到跨 unit 范围证明，
  `0x41e99d` 的一个 `mulss` 是唯一候选，也只值
  `5,789,066 / 26,827,294,354 = 0.021579%`。
- 在当前“unit 可独立入场、无 speculation/deopt”的可实现证明模型下，top-20 其余
  site 的入口 XMM/动态内存事实都是 Unknown。即使再把 top-20 以外尚未逐 opcode
  审计的 **全部** guard 都当成可删，并加上上述不可用于通用 DBT 的乐观候选，
  c-ray 上限仍只有
  `(429,150,771 + 5,789,066) / 26,827,294,354 = 1.621259%`，低于 2% 门槛。
  这是一条对未知尾部极度有利的上界，因此无需冒险外推。
- smallpt top-20 中有一个真实的位构造候选：glibc `erand48_r` 先构造 `[1,2)` 的
  IEEE-754 double，再减只读常数 1.0；其一个 `subsd` guard 值
  `6,421,970 / 8,789,802,698 = 0.073062%`。它不能改变 c-ray 的否决结果。
- 正确实现需要 known-FP-class、整数 known-bits、只读映射证明、跨 architectural
  XMM store/load 的事实保持，以及 MXCSR、fault/signal、调用和类型 pun 失效规则。
  对当前已证明收益面，这个成本和“错误时静默吞 NaN”的风险不成比例。

本文中的“动态”均为 W71 定义的 `block entries × 主线静态 host 指令数`，不是 PMU
retired instructions 或 cycle；收益数字都是机械指令上界，不是 wall-time 承诺。

## 1. 复现口径与原始数据

### 1.1 构建与运行

Orb 构建目录严格使用 `/tmp/w75-build`：

```sh
cmake -S /private/tmp/w64 -B /tmp/w75-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/w75-build --target svm_translator_linux -j4
```

计数结果保存在 Orb 的 `/tmp/w75-audit/{cray,smallpt}/hot.count`。复现命令：

```sh
mkdir -p /tmp/w75-audit/cray /tmp/w75-audit/smallpt

cd /tmp/w75-audit/cray
SVM_JIT_CACHE= \
SVM_RA_HOT_COALESCE=/tmp/w75-audit/cray/hot.count \
/tmp/w75-build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/cray_x64 \
  /Users/swift/CLionProjects/SwiftVM-bench/src/c-ray/input/scene.json \
  -j 1 -s 8 -d 160x120 -o out.png --no-sdl

cd /tmp/w75-audit/smallpt
SVM_JIT_CACHE= \
SVM_RA_HOT_COALESCE=/tmp/w75-audit/smallpt/hot.count \
/tmp/w75-build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/smallpt_wh_x64 8 160 120
```

总表直接取 `hot.count` 第一行：

| workload | units / executed | entries | host dynamic | NaN guard dynamic | NaN % |
|---|---:|---:|---:|---:|---:|
| c-ray | 8,289 / 8,289 | 341,795,712 | 26,827,294,354 | 1,766,797,595 | **6.585821%** |
| smallpt | 1,750 / 1,750 | 105,833,707 | 8,789,802,698 | 956,165,789 | **10.878126%** |

计算式均为 `nan_guard_dynamic / host_dynamic × 100`。c-ray 输出 PNG SHA-256 为
`229ec52c3209ead0fcc27aced83a5b2bf5a33a9d2f35cec5abdf19042faf19fb`；smallpt
PPM 为 `eb590f3cc93b41be030444d484eb482caa9936607b2323a3836ff87b6935d859`，可用
`sha256sum rendered_0000.png image.ppm` 重取。

### 1.2 guard 的实际形态

`QueueVecNaNColdPath` 当前在热边计数并发射：

| result 形态 | host guard | 每 site 条数 |
|---|---|---:|
| scalar f32/f64 | `FCMP result,result; B.vs cold` | 2 |
| packed f32 | `FCMEQ; UMINV; FMOV; CBZ` | 4 |
| packed f64 | `FCMEQ; UMOV; UMOV; AND; CBZ` | 5 |

guard 只属于 `VecFAdd/Sub/Mul/Div` 和 `VecFUnary(kind=sqrt)`。MIN/MAX 由
`VecFMinMax` 的比较选择序列实现 x86 operand-2 规则，不进入 W37 guard；因此下表中
MIN/MAX 是“热块里存在但 guard=0”，不能把它们计入可删面积。

### 1.3 opcode 归因方法和覆盖范围

W71 文件只输出全程序总数和按 `host_dynamic` 排序的 hot top-20，不输出每个 unit 的
完整表。为保持只读，本轮对两份 guest ELF 的 top-20 PC 用 `llvm-objdump-16 -d
--x86-asm-syntax=intel --start-address=<pc>` 反汇编到第一个 terminal，并按上表给每个
x86 FP opcode 加权。校验条件是每个 block 都满足：

```text
sum(opcode sites × scalar/packed guard width) == hot.count 的 nan_static
```

c-ray 20/20、smallpt 20/20 均相等，无不匹配 block。下表只归因 top-20；全程序减去
top-20 的残余明确列为 `unclassified residual`，没有冒充完整 opcode 分布。

以下是本轮实际使用的只读归因脚本；它直接重现两个 opcode 表的 site、动态数和
top-20 覆盖率，并把逐 block 不相等项打印到 `bad`：

```python
import re, subprocess
from collections import Counter

weights = {
    "addss": 2, "subss": 2, "mulss": 2, "divss": 2,
    "addsd": 2, "subsd": 2, "mulsd": 2, "divsd": 2,
    "sqrtss": 2, "sqrtsd": 2,
    "addps": 4, "subps": 4, "mulps": 4, "divps": 4, "sqrtps": 4,
    "addpd": 5, "subpd": 5, "mulpd": 5, "divpd": 5, "sqrtpd": 5,
}
root = "/Users/swift/CLionProjects/SwiftVM-bench/bin"
for name, elf in (("cray", f"{root}/cray_x64"),
                  ("smallpt", f"{root}/smallpt_wh_x64")):
    lines = open(f"/tmp/w75-audit/{name}/hot.count").read().splitlines()
    rows = []
    for line in lines:
        if line.startswith("[svm-hot-coalesce-hot]"):
            d = dict(re.findall(r"(\w+)=([^ ]+)", line))
            rows.append((int(d["pc"], 16), int(d["entries"]),
                         int(d["nan_static"])))
    sites, dynamic, minmax_sites, minmax_dynamic, bad = (
        Counter(), Counter(), Counter(), Counter(), [])
    for pc, entries, expected in rows:
        text = subprocess.check_output([
            "llvm-objdump-16", "-d", "--x86-asm-syntax=intel",
            f"--start-address={pc}", f"--stop-address={pc + 2048}", elf
        ], text=True)
        got = 0
        for line in text.splitlines():
            m = re.match(r"\s*[0-9a-f]+:\s+(?:[0-9a-f]{2}\s+)+\s*([a-z0-9]+)", line)
            if not m:
                continue
            op = m.group(1)
            if op in weights:
                sites[op] += 1
                dynamic[op] += entries * weights[op]
                got += weights[op]
            if op in {"minss", "maxss", "minsd", "maxsd",
                      "minps", "maxps", "minpd", "maxpd"}:
                minmax_sites[op] += 1
                minmax_dynamic[op] += entries
            if op.startswith(("j", "ret", "call")):
                break
        if got != expected:
            bad.append((hex(pc), expected, got))
    total = dict(re.findall(r"(\w+)=([^ ]+)", lines[0]))
    full, host = int(total["nan_guard_dynamic"]), int(total["host_dynamic"])
    top = sum(dynamic.values())
    print(name, sum(sites.values()), sum(sites[o] * weights[o] for o in sites),
          top, top / full * 100, "bad=", bad)
    for op, value in dynamic.most_common():
        print(op, sites[op], value, value / top * 100,
              value / full * 100, value / host * 100)
    print("minmax", dict(minmax_sites), dict(minmax_dynamic))
```

## 2. 热块 opcode 分布

### 2.1 c-ray

c-ray top-20 内共有 143 个 guarded guest site、332 条静态 host guard；动态
`1,337,646,824` 条，占全程序 guard **75.710247%**。

| opcode | top-20 静态 site | 每 site guard | 动态 guard | top-20 guard | 全程序 guard | 全程序 host |
|---|---:|---:|---:|---:|---:|---:|
| `mulss` | 66 | 2 | 544,010,410 | 40.669211% | 30.790760% | 2.027824% |
| `subss` | 29 | 2 | 341,733,258 | 25.547346% | 19.341959% | 1.273827% |
| `mulps` | 11 | 4 | 146,789,576 | 10.973717% | 8.308228% | 0.547165% |
| `addss` | 23 | 2 | 122,713,674 | 9.173847% | 6.945542% | 0.457421% |
| `addps` | 7 | 4 | 99,136,524 | 7.411263% | 5.611086% | 0.369536% |
| `subps` | 5 | 4 | 69,733,460 | 5.213144% | 3.946884% | 0.259935% |
| `divss` | 2 | 2 | 13,529,922 | 1.011472% | 0.765788% | 0.050433% |
| `sqrt*` | 0 | — | 0 | 0% | 0% | 0% |
| unclassified residual | — | — | 429,150,771 | — | 24.289753% | 1.599680% |

MIN/MAX 旁证：top-20 有 13 个 `minss` block-site（entry-weighted 执行
73,234,087 次）和 8 个 `maxss` block-site（46,893,036 次），二者 guard 都为 0。

动态 guard 最大的 block：

| PC | 符号/源码 | entries | guard/entry | dynamic guard |
|---|---|---:|---:|---:|
| `0x407450` | `rayIntersectsWithPolygon`, `poly.c:17` | 3,486,673 | 82 | 285,907,186 |
| `0x402fed` | `traverse_bvh_generic`, `bvh.c:507` | 11,723,259 | 24 | 281,358,216 |
| `0x403088` | `robust_max` in `traverse_bvh_generic` | 8,524,283 | 24 | 204,582,792 |
| `0x457ad0` | `tform_point`, `transforms.c:59` | 4,968,099 | 36 | 178,851,564 |
| `0x457b70` | `tform_vector`, `transforms.c:67` | 3,562,209 | 30 | 106,866,270 |
| `0x40308c` | `robust_min` in `traverse_bvh_generic` | 3,198,976 | 24 | 76,775,424 |
| `0x407a40` | sphere `intersect`, `sphere.c:16` | 1,512,697 | 44 | 66,558,668 |
| `0x4076d2` | `rayIntersectsWithPolygon`, `poly.c:41` | 1,377,708 | 44 | 60,619,152 |
| `0x407638` | inlined `vec_dot`, `vector.h:97` | 3,193,593 | 12 | 38,323,116 |
| `0x402f62` | inlined `safe_inverse`, `bvh.c:103` | 3,278,288 | 8 | 26,226,304 |

PC→符号/行号可用 `addr2line -Cfipe cray_x64 <pc>` 重取；entries、guard/entry 和
dynamic guard 直接取 `hot.count` 的 `[svm-hot-coalesce-hot]` 行。

### 2.2 smallpt

smallpt top-20 内有 237 个 guarded guest site、552 条静态 host guard；动态
`598,930,810` 条，占全程序 guard **62.638804%**。

| opcode | top-20 静态 site | 每 site guard | 动态 guard | top-20 guard | 全程序 guard | 全程序 host |
|---|---:|---:|---:|---:|---:|---:|
| `mulsd` | 97 | 2 | 220,561,202 | 36.825823% | 23.067255% | 2.509285% |
| `addsd` | 57 | 2 | 132,478,336 | 22.119139% | 13.855164% | 1.507182% |
| `subsd` | 51 | 2 | 113,182,178 | 18.897371% | 11.837087% | 1.287653% |
| `mulpd` | 17 | 5 | 76,494,995 | 12.771925% | 8.000181% | 0.870270% |
| `addpd` | 5 | 5 | 26,068,250 | 4.352464% | 2.726332% | 0.296574% |
| `subpd` | 4 | 5 | 19,550,065 | 3.264161% | 2.044631% | 0.222418% |
| `sqrtsd` | 3 | 2 | 5,297,892 | 0.884558% | 0.554077% | 0.060273% |
| `divsd` | 3 | 2 | 5,297,892 | 0.884558% | 0.554077% | 0.060273% |
| unclassified residual | — | — | 357,234,979 | — | 37.361196% | 4.064198% |

top-20 另有 5 个 `minsd` block-site、entry-weighted 6,512,405 次，guard=0。

动态 guard 最大的 block：

| PC | 符号 | entries | guard/entry | dynamic guard |
|---|---|---:|---:|---:|
| `0x404662` | `radiance` | 1,105,112 | 84 | 92,829,408 |
| `0x404e75` | `radiance` | 949,157 | 70 | 66,440,990 |
| `0x403e90` | `radiance` | 1,302,481 | 34 | 44,284,354 |
| `0x404591` | `radiance` | 1,302,481 | 34 | 44,284,354 |
| `0x404269` | `radiance` | 1,302,481 | 34 | 44,284,354 |
| `0x404333` | `radiance` | 1,302,481 | 34 | 44,284,354 |
| `0x4043fd` | `radiance` | 1,302,481 | 34 | 44,284,354 |
| `0x4044c7` | `radiance` | 1,302,481 | 34 | 44,284,354 |
| `0x4041b3` | `radiance` | 1,250,499 | 34 | 42,516,966 |
| `0x4040e9` | `radiance` | 1,173,202 | 34 | 39,888,868 |

## 3. IR 事实来源与命中核查

### 3.1 证明目标不是一个布尔 `finite`

删除当前 site 的 guard，只需证明**该次运算结果不可能为 NaN**；但如果结果继续
传播，不能把“本次非 NaN”错误提升为 `Finite`：

- finite + finite、finite − finite、finite × finite 可能溢出成 infinity；本次仍非
  NaN，但后续 `inf-inf`、`0*inf` 会生成 NaN。
- finite / finite 在 `0/0` 时仍为 NaN；必须额外证明除数非零，或证明操作数组合
  不属于 invalid 类。
- sqrt 必须证明输入不是 NaN 且不小于零；`-0.0` 合法并且符号可观察。
- packed 操作必须逐 lane 成立，不能用“至少一 lane finite”代替全 lane 证明。

因此可行的数据结构至少是“可能 FP class 的位集合 + 符号/零信息”，类似 LLVM 的
[`KnownFPClass`](https://llvm.org/doxygen/structllvm_1_1KnownFPClass.html)，而不是一个
会在溢出后继续传播的 `is_finite` bit。LLVM 的 `nnan` 属于允许把 NaN 结果视为
poison 的 fast-math 承诺；guest 没有给 DBT 这种承诺，不能照搬
[`nnan`](https://llvm.org/docs/LangRef.html#fast-math-flags) 作为输入假设。

### 3.2 dump 观察

IR dump 用较小但走同一翻译路径的 workload 获取：

```sh
SVM_JIT_CACHE= SVM_DUMP_IR=1 SVM_DUMP_IR_POST=1 \
  /tmp/w75-build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/cray_x64 \
  /Users/swift/CLionProjects/SwiftVM-bench/src/c-ray/input/scene.json \
  -j 1 -s 1 -d 64x48 -o out.png --no-sdl \
  2>/tmp/w75-audit/cray-dump/ir.log

SVM_JIT_CACHE= SVM_DUMP_IR=1 SVM_DUMP_IR_POST=1 \
  /tmp/w75-build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/smallpt_wh_x64 \
  4 64 48 2>/tmp/w75-audit/smallpt-dump/ir.log
```

两份 log 分别为 23,778,762 和 5,673,696 bytes（`wc -c`）。c-ray `0x402fed`
的 guarded `VecFMulScalar32` 操作数来自 `LoadUniform`；其他主要几何块同样由
architectural XMM state 或 `LoadMemory` 起源。smallpt `radiance` 热块也以
`LoadUniform`/`LoadMemory` 为主。也就是说，C/C++ 源码里的“向量”“颜色”“平方和”
类型信息到此已经不存在，未知 guest state 不能默认为 finite。

即使 guest 指令流在同一 unit 内构造了已知位型，当前每条 guest 指令间的
StoreUniform/LoadUniform 形态也可能让普通 SSA transfer 看不到它。例如
`erand48_r` 的最终 IR 是从 architectural XMM slot 重载再做 `VecFSubScalar64`；要
命中它，事实必须跟随 uniform byte range，而不只是跟随 SSA value。

### 3.3 四类来源与命中数

下表统计范围是上述 hot top-20，动态数是“命中 site 的 W37 guard width × block
entries”。“生产可接受”只允许从 guest 指令/映射权限本身推出、对所有输入成立的
事实；不接受本次 benchmark 恰好输入较小。

| finite/non-NaN 来源 | c-ray 生产可接受命中 | c-ray 乐观人工上界 | smallpt 生产可接受候选 | 退出条件 |
|---|---:|---:|---:|---|
| a. FP 常数、只读数据 | 0 site / 0 dynamic | 同左 | 0 个完整 site | 单个 finite 常数不足以证明另一操作数；只有 loader 证明不可写的映射可建事实，普通 `.data`、未知别名、`mprotect`/SMC 后立即失效 |
| b. 前序 FP 运算/范围格 | 0 / 0 | 1 `mulss` / 5,789,066 | 0 / 0 | CFG join 取可能类并集；循环需不动点；溢出后只能保留 non-NaN 而非 finite；未知 load/call 退出 |
| c. 整数/逻辑/已知位构造 | 0 / 0 | 同左 | 1 `subsd` / 6,421,970 | 普通 bitcast/type pun 可直接制造 NaN；必须由 known-bits 证明指数/尾数类别，不能把所有整数向量/逻辑结果一概当 finite |
| d. NaN 结果不可观察 | 0 / 0 | 0 / 0 | 0 / 0 | 初版禁用；architectural XMM 写、guest store、fault/helper/signal/unit terminal 都是观察边界 |

对应静态覆盖：c-ray 为 0/143 个生产可接受 site；即便接受 workload-specific 的
跨 unit 范围假设也只有 1/143，guard 指令为 2/332。smallpt 为 1/237，guard
指令为 2/552。

#### c-ray 的唯一乐观候选为什么不能进入通用证明

`0x41e99d`（`radical_inverse_b_13`）为：

```text
pxor xmm1,xmm1
cvtsi2ss xmm1,rdx
mulss xmm1,xmm3
minss xmm1,[one_minus_epsilon]
addss xmm0,xmm1
```

在本次渲染的 sample-index 范围内，可人工证明 `rdx` 是非负 reversed digits，
`xmm3` 是反复乘 `1/13` 的 `[0,1]` 值，故 `mulss` 结果 finite；删它的两条 guard
给出 `2 × 2,894,533 = 5,789,066`。但 unit 从 `0x41e99d` 入场时 `xmm3` 已是
`LoadUniform`，而函数参数本身也没有 x86 ABI 级的非负/上界契约。把 benchmark
实际输入范围当成静态语言事实会对别的合法调用出错。因此它只作为极乐观上界，
**生产命中计为 0**；后面的 `addss` 仍因 `xmm0` 未知而不能删。

反汇编和源定位复现：

```sh
llvm-objdump-16 -d --x86-asm-syntax=intel \
  --start-address=0x41e99d --stop-address=0x41ea10 \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/cray_x64
addr2line -Cfipe /Users/swift/CLionProjects/SwiftVM-bench/bin/cray_x64 0x41e99d
```

#### smallpt 的位构造候选

`0x41ef69` 的 glibc `erand48_r` 用 48-bit state 和整数 OR 构造 exponent=`0x3ff`
的 double，故位型严格在 `[1,2)`；随后 `subsd` 一个 RIP-relative 只读 1.0，结果
严格在 `[0,1)`。这一证明不依赖运行输入值，理论上可删除两条 guard：

```text
2 × 3,210,985 = 6,421,970
6,421,970 / 8,789,802,698 = 0.073061594%
```

但它要求新增整数 known-bits、bitcast→FP-class、uniform byte-fact 和 ELF 只读映射
四段传递，不能由一个局部 `VecFSubScalar64` peephole 安全得到。

反汇编复现：

```sh
llvm-objdump-16 -d --x86-asm-syntax=intel \
  --start-address=0x41ef50 --stop-address=0x41efc0 \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/smallpt_wh_x64
```

### 3.4 FEX/其他编译器参照

核查时浅克隆 FEX HEAD `2fdbff3d1c0058b5851b84ae9db2c00d88f02b60`。其默认
pass manager 当前只有 x87 stack optimization、dead-flag elimination 和 RA，未见
可直接复用的全局 known-FP-class pass；其 ARM64 MIN/MAX lowering 仍按 opcode
显式处理 NaN/operand-2 选择，而不是以“通常是 finite”为前提。对应源码：

- [FEX PassManager.cpp](https://github.com/FEX-Emu/FEX/blob/2fdbff3d1c0058b5851b84ae9db2c00d88f02b60/FEXCore/Source/Interface/IR/PassManager.cpp#L70-L84)
- [FEX VectorOps.cpp](https://github.com/FEX-Emu/FEX/blob/2fdbff3d1c0058b5851b84ae9db2c00d88f02b60/FEXCore/Source/Interface/Core/JIT/VectorOps.cpp#L1391-L1445)

可借鉴的是 LLVM 的“可能类别集合”表示，不是把 FEX 某条 host lowering 当成 x86
NaN payload 精确性的证明。

## 4. 证明链风险与强制保守边界

证明错误不会稳定崩溃，而会让 NaN payload、符号或 invalid indefinite 静默错误，
属于必须默认 OFF、可模块回退的高风险优化。完整风险如下：

1. **qNaN/SNaN**：quiet bit、sign、payload、左右操作数优先级和 generated indefinite
   都是 W37 cold path 修复的对象；“结果是 NaN 但不关心哪一个”不成立。
2. **`-0.0`**：sqrt、div、MIN/MAX 和后续 copysign/位读取能观察符号；不能用非负或
   nonzero 的粗糙布尔值抹掉 `-0.0`。
3. **infinity 与溢出**：finite 输入不保证 finite 输出；若错误传播 `Finite`，后续
   `inf-inf`、`0*inf`、`inf/inf` 会绕过真正需要的 guard。
4. **FTZ/DAZ、MXCSR 与 FPCR**：subnormal 输入/输出、rounding mode 和 exception
   mask 会改变分类/结果；事实和 JIT cache key 必须绑定控制模式，`LDMXCSR` 等写入
   必须清除相关事实。
5. **packed/scalar 形态**：packed 必须逐 lane；scalar 指令还保留上层 lane，不能用
   lane 0 的事实覆盖完整 XMM。
6. **type pun 与 shuffle**：MOVD/MOVQ、AND/OR/XOR、insert/extract、shuffle 以及 guest
   memory 都可构造任意 FP 位型；只有精确 known-bits 足以重新分类。
7. **只读性和别名**：RIP-relative 不等于不可写。只有 loader 映射权限和地址空间
   生命周期共同证明的 RO 数据可用；`mprotect`、SMC、共享映射、未知别名写入必须按
   epoch 失效。
8. **CFG/循环**：join 取可能类并集，loop 做收敛不动点；不能使用只在某条热路径
   成立的 profile 事实证明语义。
9. **helper/call/syscall**：未知 helper、间接调用、syscall、CallLambda/Location/
   Dynamic、GetUniformAddress 和 ABI 返回值都清除可能被触及的 facts。
10. **x87/SSE 交互**：ext80 的范围、舍入和 NaN 格式不同；经 memory、FXSAVE/
    XRSTOR 或类型 pun 进入 SSE 时必须退回 Unknown。
11. **fault/signal 精确上下文**：W57 的契约要求在 guest memory、helper、控制流和
    unit terminal 前物化精确 XMM state。不能仅因 SSA 看似死值就假定 NaN
    不可观察；异步 signal/fault context、handler 修改和 sigreturn 都是边界。
12. **宿主 feature**：AFP/AH、FPCR 默认 NaN/alternate handling 及宿主差异必须进入
    lowering 条件和 cache key，不能由一台 Orb 的行为外推。

如果未来有新数据重新立项，最低安全外壳应是模块级
`SVM_SSE_NAN_PROVEN_FINITE`（名字仅建议）默认 OFF；证明失败或遇到任一边界就保留
现有 W37 guard，不允许 speculative “大概 finite”。这不是本轮批准的实现计划。

重新立项的验证矩阵必须至少包括：f32/f64、scalar/packed；每种 qNaN/SNaN 的符号、
payload 和左右操作数顺序；`inf + -inf`、`0*inf`、`0/0`、负数 sqrt；`±0`、subnormal、
`±inf`；FTZ/DAZ 四组合；JIT/解释器/真实 x86 对拍；x87↔SSE、bitcast、call/helper、
fault/signal W57 context；并做 **NaN payload 位级 fuzz**，不能只比十进制输出或
`isnan()`。

## 5. 决断与可复核上界

c-ray 的 top-20 外残余为：

```text
1,766,797,595 - 1,337,646,824 = 429,150,771
429,150,771 / 26,827,294,354 = 1.599679660%
```

这是本轮未逐 opcode 分类的全部 guard。在当前 unit-independent、无 deopt 的证明
模型下，把它 100% 当作可删除，再加 c-ray top-20 唯一的 workload-specific 乐观
site：

```text
(429,150,771 + 5,789,066) / 26,827,294,354
  = 1.621258675%
```

仍低于用户指定的 2% 动态占比门。真实的生产可接受 top-20 命中为零，所以这个
1.621259% 已明显高估。任意 whole-program CFG/range specialization 可能建立更多事实，
但它需要改变“任意 guest PC 可独立入场”的翻译契约或引入 guard+deopt，已经不是
低风险 guard-elimination spike；证明链成本/风险门同样否决该方向。

**最终裁定：W75 关闭，不新增开关，不实现 proven-finite guard elimination。** 若以后
W71 能输出全 unit PC/entries 且新 workload 显示生产可证明覆盖超过 2%，再以第 4 节
的默认 OFF 外壳和 payload-level fuzz 门重新审议。

## 6. 范围纪律

- 仓库唯一新增文件：`docs/w75-nan-proven-finite-audit.md`。
- 未修改任何源码、测试、golden、`source/translator/linux/linker/` 或
  `SwiftVM-bench/harness/run_matrix.sh`。
- 未执行 commit/push/add/checkout/reset/stash，也未重新生成 fingerprint golden。
- `/tmp/w75-build`、`/tmp/w75-audit` 和 `/tmp/w75-fex-audit` 只是 Orb 临时构建/证据
  目录，不属于仓库改动。
