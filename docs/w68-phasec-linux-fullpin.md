# Phase C：Linux identity pool10 全 pin 复测报告

## 0. 裁决

**NO-GO。** Linux/aarch64 identity 图上，`SVM_X86_PIN_EXT=3` +
`SVM_RA_FIXED_CLASS=1` 的首轮 GPR pool 确为 **10**；发生 spill 的 unit 按既有
x18 契约以 pool **9** 重跑。这个实测结果推翻了把 Darwin+bias 的 pool7 当成
物理上限的旧前提，也显著缓和了原 spike 的容量灾难，但尚不足以翻盘：

| P6 门 | CoreMark region 默认态 | CoreMark `RE=0` | 裁决 |
|---|---:|---:|---|
| spill ops ≤ A × 1.25 | 静态 `0 → 106`；动态 `0 → 72` | 静态 `0 → 106`；动态 `0 → 72` | FAIL |
| helper 边界净值 ≤ 0 | caller-save inst `+210` | caller-save inst `+196` | FAIL |
| host bytes 不增 | `445,520 → 445,792`（`+272`） | `246,860 → 247,300`（`+440`） | FAIL |
| CoreMark 动态密度改善 ≥ 1.03 | A/B=`1.009824480` | A/B=`1.001134324` | FAIL |

此外，fixed-class 的动态 move/桥接条目没有净减：CoreMark region 增加
`710,299,064`（`+3.526066%`），`RE=0` 增加 `2,879,776,687`
（`+3.802596%`）。因此本批不建议改变默认开关，也不建议把 Linux full-pin
作为生产 bundle。

## 1. 范围、环境与构建

### 1.1 环境

- host 入口：`orb -m ubuntu sh -c '...'`；所有 VM 持久产物均位于
  `/home/swift/svm-phasec/`。
- VM：Ubuntu/aarch64，15 CPU；内核 `7.0.14-orbstack-00291-g1b252bd85841`。
- 源树：`/home/swift/svm-phasec/SwiftVM`，指挥官预置的 `33bdf53` 干净 clone。
- 构建树：`/home/swift/svm-phasec/build`；最终配置为 `RelWithDebInfo`，与 w68
  现有构建口径一致。
- 密度测量显式清除 `SVM_EXEC_PROF`，生产口径保留 region 默认值，块态显式
  `SVM_REGION_EDGES=0`；所有 benchmark 均关闭 JIT cache。

### 1.2 构建结果与缺口

首次按 Release 配置完成 599 个 Ninja target；发现 w68 现有构建为
`RelWithDebInfo` 后，在同一 build tree 重配并完整重建，最终同样完成
`[599/599] Linking CXX executable source/tests/swift_test`。没有需要修复的
host-Linux 编译缺口，也没有改动源码。

最终构建日志为
`/home/swift/svm-phasec/build-relwithdebinfo.log`。GCC 输出 5,086 个
warning occurrence；与此前 Release 完整构建计数相同，均来自现有源码、宏和
third-party 路径。本批零源码改动，故新增 warning 集合为空。

## 2. Linux identity 与 pool10 的直接证据

运行命令的关键部分为：

```text
env -u SVM_EXEC_PROF SVM_JIT_CACHE= \
  SVM_X86_PIN_EXT=3 SVM_RA_FIXED_CLASS=1 \
  SVM_VIXL_HOST_DUMP=1 SVM_RA_SHAPE_PROF=<log> \
  svm_translator_linux hello_x86_64
```

程序正常以 guest 约定值 42 退出。运行时打印：

```text
Linux identity memory mode: guest addresses map directly onto the host address space...
[svm-reg-mask] memory_base=0 page_table=0 x24_reserved=0 x10_reserved=0
dispatcher_loc=x24 pin_ext=1 pin_ext_level2=1 x0_x5_reserved=1
pin_ext_level3=1 x6_x9_reserved=1 xpool_requested=1 xpool_effective=1
xpool_auto_level3=0 x22_reserved=1 x23_reserved=1 x18_reserved=0
x18_spill_conditional=1 x29_reserved=1 allocatable_gprs=10
```

同次 shape 记录为 `units=2 spill_units=0 host_bytes=136`，GPR pool histogram
为 `10:2`。这同时证明：

1. Linux 默认运行时是 identity，而不是 bias：`memory_base=0`、
   `page_table=0`，x24/x10 都未因 bias 被 mark；
2. level3 + XPOOL 的首轮 value pool 是 10；
3. x18 在无 spill 的 Linux unit 中进入 pool，只有 spill unit 才条件保留并重跑。

benchmark 的直接证据进一步覆盖重跑路径：

| 语料/口径 | B 臂 GPR pool histogram | spill unit |
|---|---:|---:|
| CoreMark region | `9:2, 10:621` | 2 |
| CoreMark `RE=0` | `9:2, 10:1691` | 2 |
| SQLite region | `9:28, 10:4831` | 28 |
| SQLite `RE=0` | `9:29, 10:14684` | 29 |

代码契约也与运行时输出一致：translator 仅在 `memory_base` 或 `page_table`
存在时启用 bias；寄存器 mask 按该状态标记 x24/x10，Linux x18 则走
spilling-unit conditional reserve/retry，而非 Darwin 的全局保留。

## 3. 测量口径

两臂均在 Linux/aarch64 原生 host 上串行运行：

- 臂 A：`SVM_X86_PIN_EXT=2 SVM_RA_FIXED_CLASS=0`；
- 臂 B：`SVM_X86_PIN_EXT=3 SVM_RA_FIXED_CLASS=1`；
- region：清除 `SVM_REGION_EDGES`，使用生产默认态；
- `RE=0`：`SVM_REGION_EDGES=0`，块态 digit-exact；
- CoreMark：`coremark_x64 0 0 0x66 150000 7 1 2000`；
- SQLite：`sqlite_speedtest_x64 --size 1 --testset main <fresh.db>`，每臂/口径
  使用独立 fresh database；
- 计数：`SVM_RA_SHAPE_PROF` 加既有 hot-coalesce 统计；`SVM_EXEC_PROF`
  明确禁用；
- 动态密度改善因子定义为 `A host_dynamic / B host_dynamic`，门槛 1.03；
- 墙钟仅随原始日志保存，不参与裁决。

全部四组 CoreMark CRC 都为 `e9f5/e714/1fd7/8e3a/25b5`；四组 SQLite
均返回 0 并打印 `TOTAL`。

## 4. P6 四门：CoreMark

### 4.1 生产 region 默认态

| 指标 | A：L2 | B：L3 + fixed | B−A / 比值 |
|---|---:|---:|---:|
| units | 623 | 623 | 0 |
| GPR pool | `14:623` | `9:2, 10:621` | — |
| spill units | 0 | 2 | +2 |
| spill defs / loads / stores | `0 / 0 / 0` | `34 / 38 / 34` | 静态 ops `0 → 106` |
| host bytes | 445,520 | 445,792 | **+272 / +0.061052%** |
| executed versions | 1,010 | 1,014 | +4 |
| weighted entries | 2,656,710,078 | 2,656,710,079 | +1 |
| host dynamic inst | 56,652,222,403 | 56,101,058,698 | −551,163,705 / −0.972890% |
| weighted spill reload/store | 0 | 72 | **+72** |
| weighted move/bridge | 20,144,236,137 | 20,854,535,201 | **+710,299,064 / +3.526066%** |
| helper dynamic calls | 227 | 203 | −24 |
| helper caller-save inst | 5,650 | 5,860 | **+210** |
| helper code bytes | 22,600 | 23,440 | +840 |
| helper memory bytes | 124,384 | 124,112 | −272 |

密度改善因子：

```text
56,652,222,403 / 56,101,058,698 = 1.009824480
```

只改善 0.972890%，未达到 1.03。按 guest-PC 公共集逐 unit 对照：2,887 个
公共 PC 中 124 个增大、462 个减小、2,301 个不变，公共集净增 1,252 bytes；
因此 aggregate `+272` 不是版本数单独造成的假象。

### 4.2 `RE=0` 块态

| 指标 | A：L2 | B：L3 + fixed | B−A / 比值 |
|---|---:|---:|---:|
| units | 1,702 | 1,693 | −9 |
| GPR pool | `14:1702` | `9:2, 10:1691` | — |
| spill units | 0 | 2 | +2 |
| spill defs / loads / stores | `0 / 0 / 0` | `34 / 38 / 34` | 静态 ops `0 → 106` |
| host bytes | 246,860 | 247,300 | **+440 / +0.178239%** |
| weighted entries | 9,774,083,577 | 9,774,083,959 | +382 |
| host dynamic inst | 205,284,062,103 | 205,051,467,323 | −232,594,780 / −0.113304% |
| weighted spill reload/store | 0 | 72 | **+72** |
| weighted move/bridge | 75,731,853,755 | 78,611,630,442 | **+2,879,776,687 / +3.802596%** |
| helper dynamic calls | 151 | 137 | −14 |
| helper caller-save inst | 3,704 | 3,900 | **+196** |
| helper code bytes | 14,816 | 15,600 | +784 |
| helper memory bytes | 81,536 | 82,544 | +1,008 |

密度改善因子：

```text
205,284,062,103 / 205,051,467,323 = 1.001134324
```

只改善 0.113304%。公共 PC 集为 1,692 个：73 个增大、304 个减小、
1,315 个不变，公共集净增 1,400 bytes。

## 5. SQLite 对照

SQLite 不是 CoreMark 1.03 主门，但它暴露了 pool10 下仍然存在的压力集中区。

### 5.1 生产 region 默认态

| 指标 | A：L2 | B：L3 + fixed | B−A / 比值 |
|---|---:|---:|---:|
| units | 4,857 | 4,859 | +2 |
| GPR pool | `13:1, 14:4856` | `9:28, 10:4831` | — |
| spill units | 2 | 28 | +26 |
| spill defs / loads / stores | `4 / 6 / 2` | `882 / 997 / 791` | ops `12 → 2,670`，**222.5×** |
| host bytes | 3,364,920 | 3,415,256 | **+50,336 / +1.495905%** |
| host dynamic inst | 285,221,705 | 277,088,340 | −8,133,365 |
| weighted spill reload/store | 8 | 1,191,200 | **148,900×** |
| weighted move/bridge | 87,507,559 | 92,264,481 | **+4,756,922 / +5.436013%** |
| helper caller-save inst | 49,235,282 | 56,834,268 | **+7,598,986** |
| helper code bytes | 196,941,128 | 227,337,072 | +30,395,944 |
| helper memory bytes | 1,075,764,240 | 1,197,348,416 | +121,584,176 |

密度改善因子为 `1.029352967`，改善 2.851594%，仍比 1.03 少约
0.000647。公共 PC 集净增 50,256 bytes（22,048 个公共 PC 中 881 增、
5,247 减、15,920 不变）。

### 5.2 `RE=0` 块态

| 指标 | A：L2 | B：L3 + fixed | B−A / 比值 |
|---|---:|---:|---:|
| units | 14,711 | 14,713 | +2 |
| GPR pool | `13:1, 14:14710` | `9:29, 10:14684` | — |
| spill units | 3 | 29 | +26 |
| spill defs / loads / stores | `4 / 6 / 2` | `421 / 489 / 388` | ops `12 → 1,298`，**108.166667×** |
| host bytes | 2,086,580 | 2,102,456 | **+15,876 / +0.760862%** |
| host dynamic inst | 531,423,946 | 520,138,055 | −11,285,891 |
| weighted spill reload/store | 8 | 1,190,125 | **148,765.625×** |
| weighted move/bridge | 160,317,205 | 170,217,118 | **+9,899,913 / +6.175203%** |
| helper caller-save inst | 3,856,784 | 4,473,550 | **+616,766** |
| helper code bytes | 15,427,136 | 17,894,200 | +2,467,064 |
| helper memory bytes | 84,542,320 | 94,410,272 | +9,867,952 |

密度改善因子为 `1.021697876`，改善 2.123708%。公共 PC 集净增
15,492 bytes（14,711 个公共 PC 中 604 增、3,844 减、10,263 不变）。

## 6. 套件与指纹自检

### 6.1 指纹 self-consistency

仓内脚本固定使用 `/tmp/svm_fp_guests`，与“VM 只在 `~/svm-phasec/` 下活动”
冲突，因此没有直接调用脚本。改为在 evidence 子目录逐条复刻脚本的 guest
staging、两次生成和 byte-for-byte `cmp`；没有读写 golden，也没有
`--update`。

| 配置 | guest | units | 两遍 byte compare |
|---|---:|---:|---|
| 默认 L2 | 11 | 1,792 | PASS |
| L3 + fixed | 11 | 1,792 | PASS |

证据分别在
`/home/swift/svm-phasec/evidence/fingerprint/status.txt` 与
`/home/swift/svm-phasec/evidence/fingerprint-l3-fixed/status.txt`，均为
`rc=0 units=1792 guests=11`。

### 6.2 Linux 全套件与五 gate

最终 RelWithDebInfo 二进制的结果：

| 配置 | cases | assertions | 结果 |
|---|---:|---:|---|
| 默认 L2 | 209；206 pass / 3 fail | 1,050,418；1,050,412 pass / 6 fail | FAIL |
| L3 + fixed | 209；206 pass / 3 fail | 1,050,413；1,050,406 pass / 7 fail | FAIL |
| 默认 L2 + 五 gate | 209；204 pass / 5 fail | 1,173,237；1,173,226 pass / 11 fail | FAIL |

五 gate 包含要求的四项且逐字一致：

```text
xsave_test.cpp:467
xsave_test.cpp:468
xsave_test.cpp:843
xsave_test.cpp:1045
```

但还包含 Linux/GCC 下既有的 `main_case.cpp` 失败，故不能宣称“严格同集”。
默认 L2 的额外断言位于 `main_case.cpp:2566/2692/2727/2735/4067/4135`；
L3/five-gate 的某些运行还出现 `main_case.cpp:2522`。这些额外失败在 A、B
和 five-gate baseline 均可见，且与本批零代码改动相符；它们仍然是验收阻塞，
不能豁免成通过。

### 6.3 Linux 套件缺口定因（未修改受限文件）

1. `main_case.cpp:2496-2500` 与 `2650-2654` 把 buffer 转成
   `vixl::aarch64::Instruction*` 后使用 `++instruction`。该 VIXL
   `Instruction` 是无数据成员的 decoder facade，GCC 下 `sizeof` 为 1；日志显示
   每条有效 4-byte instruction 后跟三个 `unallocated`，尾部还会越过 cursor
   解三次。比较文本因此包含越界/未初始化内容，失败数会随 allocator 内容波动。
2. `main_case.cpp:2735` 的 force-spill 测试在 RA 后手工调用
   `MapMemSpill`，绕过 Linux production path 的“发现 spill 后保留 x18 并重跑”
   契约，最终触发 `spilling unit reached emission without conditional x18 reservation`。
3. `main_case.cpp:3787-3794` 的 width-chain 自建 GPR mask 保留 x18 可用；Linux
   `RunVerified` 对 spilling unit 以 x18 reserved 图重跑，改变该测试的人工分配
   预期，导致 `4067/4135` 的 platform-sensitive 断言失败。

正确修复面都在明确禁止改动的 `main_case.cpp`：按 4-byte instruction size
迭代、让 force-spill 使用 production retry/显式 x18 reservation、让自建 mask
显式匹配被测 host 图。给 production 机制增加特判只会掩盖测试夹具问题，因此
本批没有写补丁。Release 与 RelWithDebInfo 均复现，也排除了构建类型归因。

## 7. 量化死因与重开前置

### 7.1 死因

pool10 确实消除了 pool7 的全局容量坍塌，但 fixed-class 当前的收益与成本分布
仍不对称：大多数 unit 不 spill，少数高压 unit 进入 pool9 retry；同时 fixed
home affinity 在 helper/clobber 边界增加 snapshot，copy/coalescing 收益又未抵消
新增 bridge。结果是：动态 host instruction 略降，但静态 host bytes、helper
边界和 move/bridge 都增长；SQLite 的少数热点 spill 被 entry 权重放大到约
119 万条。

### 7.2 量化重开条件

再次考虑翻盘前，至少应同时满足：

1. CoreMark 两口径的 B 臂 spill ops 回到 A 的门槛。当前 A 为 0，故必须消除
   这 2 个 spill unit、106 个静态 spill ops 和 72 个动态 spill ops，或先由另案
   合法改变 A 基线；
2. helper caller-save 指令净额在两口径均 ≤0；需分别消除当前 `+210`、
   `+196`，且 helper memory bytes 不增；
3. aggregate host bytes 与 common-PC 净额均 ≤0；至少消除 CoreMark
   `+272/+440`，并避免以新版本掩盖单 unit 增长；
4. CoreMark region 与 `RE=0` 的 A/B 动态密度因子都达到 ≥1.03；当前缺口
   分别为 `0.020175520` 与 `0.028865676`；
5. move/桥接动态条目必须净减，而不是当前 `+3.526066%/+3.802596%`；
6. SQLite 至少消除 26 个新增 spill unit，并把 region/`RE=0` 的动态 spill
   从 `1,191,200/1,190,125` 降至 A×1.25；helper 与 host-byte 净额同时 ≤0；
7. 另行获准修正 Linux `main_case.cpp` 测试夹具后，默认态、L3+fixed 全套件
   必须全绿，five-gate 必须只剩四项 xsave 既有红灯。

在上述条件达成前，回退口径保持：`SVM_X86_PIN_EXT=2`、
`SVM_RA_FIXED_CLASS=0`。本批没有改动任何机制代码、配置、测试、golden、
linker 或 harness。

## 8. 证据索引

VM 内全部原始产物位于 `/home/swift/svm-phasec/evidence/`：

- `/home/swift/svm-phasec/build-relwithdebinfo.log`：最终构建；
- `pool-l3-fixed.{out,err,rc,shape}`：identity 与 `allocatable_gprs=10`；
- `density/{region,re0}-{a,b}-{coremark,sqlite}/`：四组 shape、hot、stdout、
  stderr 与 fresh SQLite DB；
- `fingerprint/`、`fingerprint-l3-fixed/`：两臂两遍 self-consistency；
- `swift-test-default-relwithdebinfo.log`、`swift-test-l3-fixed.log`、
  `swift-test-five-gates.log`：默认 L2、L3+fixed、五 gate 的完整 Catch2 输出。

报告文件是本批唯一 tracked 候选改动。
