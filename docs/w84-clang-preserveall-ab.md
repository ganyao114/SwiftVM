# W84：Clang Linux 构建与 W73 preserve_all 真实 A/B

## 1. 结论

W73 的 `preserve_all` 路径在 Linux/AArch64 Clang 17 下确实激活，GCC 13 下确实
惰性。`func_tests` 的 `DirectPreserveAll` 计数为 Clang-ON `4`，而
Clang-OFF/GCC-ON 都为 `0`。具体的 `bsf eax, edx` unit `0x41169a` 中，Clang-ON
删去了 x10/x11 和 q11-q14 的 save/restore，unit 从 252 B 降到 228 B；GCC-ON
与 Clang-OFF 均保留完整快照。

正确性门全部通过，未发现 preserve_all 契约破坏。性能上只有 7-Zip 得到方向一致、
CI 排除 1 的小收益：median `1.007439`，即约 `+0.744%`；c-ray 与 CoreMark
基本持平，OpenSSL 噪声很大且 CI 跨 1。四语料不构成普遍收益，唯一显著项又不足
1%，因此**不翻盘，`SVM_HELPER_LEAF_ABI` 继续默认 OFF**。

## 2. Clang 构建

### 2.1 工具链

- Orb：Ubuntu 23.10，AArch64，16 vCPU。
- VM 原有 `/usr/bin/clang`：Ubuntu Clang `16.0.6 (15)`。
- Clang 16 能在前端识别 `__attribute__((preserve_all))`，但 AArch64 后端在编译
  `decoder_sse42str.cc` 和 `decoder_alu.cc` 时崩溃：
  `fatal error: error in backend: Unsupported calling convention.`
- apt 仓库可用的下一版为 Clang `17.0.2 (1~exp1ubuntu2.1)`；安装命令：

```sh
orb -m ubuntu bash -lc '
  sudo apt-get update
  sudo apt-get install -y clang-17
  clang-17 --version
  clang++-17 --version
'
```

最终构建命令：

```sh
orb -m ubuntu bash -lc '
  rm -rf /tmp/w84-build-clang
  cmake -S /private/tmp/w64 -B /tmp/w84-build-clang \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang-17 \
    -DCMAKE_CXX_COMPILER=clang++-17
  cmake --build /tmp/w84-build-clang -j$(nproc)
'
```

配置、默认 `all` 目标均成功。没有项目源码构建修复；Clang 输出的第三方/既有 warning
没有按 warning-as-error 处理。没有改动 `jit_context.cpp` 或 `trampolines.cpp`。
Clang 16 的失败是工具链后端能力边界，不是以运行时行为修改绕过的项目错误。

## 3. preserve_all 激活证据

### 3.1 编译期守卫与计数

三次都运行同一个 `func_tests_x86_64`，禁用 disk JIT cache，并启用
`SVM_RA_SHAPE_PROF`：

```sh
Q=/private/tmp/w64/source/translator/linux/tests/func_tests_x86_64
C=/tmp/w84-build-clang/source/translator/linux/svm_translator_linux
G=/tmp/svm-build/source/translator/linux/svm_translator_linux

SVM_JIT_CACHE= SVM_HELPER_LEAF_ABI=0 SVM_RA_SHAPE_PROF=/tmp/clang-off.shape "$C" "$Q"
SVM_JIT_CACHE= SVM_HELPER_LEAF_ABI=1 SVM_RA_SHAPE_PROF=/tmp/clang-on.shape  "$C" "$Q"
SVM_JIT_CACHE= SVM_HELPER_LEAF_ABI=1 SVM_RA_SHAPE_PROF=/tmp/gcc-on.shape    "$G" "$Q"
```

三次 guest 都是 rc `101`，stdout SHA-256 都为
`9c3194ff498da03869bbabbd81241fd2ec771619281f969c4b4edc55577a6810`。

| 构建/开关 | direct_aapcs calls | DirectPreserveAll calls | snapshot instructions | snapshot code bytes | snapshot memory bytes |
|---|---:|---:|---:|---:|---:|
| GCC-ON | 16 | 0 | 288 | 1,152 | 5,536 |
| Clang-OFF | 16 | 0 | 288 | 1,152 | 5,536 |
| Clang-ON | 12 | 4 | 264 | 1,056 | 4,896 |

Clang-ON 的最后三列是 `direct_aapcs` 与 `direct_preserve_all` 两类之和。严格 leaf
调用仍需保存 x0-x8 中的活值和 JIT 自己的 x29/x30 link pair，因此计数不是零；被删除
的是 preserve_all 合同覆盖的 x9-x15 与 v8-v31 活值。

GCC-ON 与 Clang-OFF 完全同形，证明 `SVM_HAS_HELPER_PRESERVE_ALL` 在 GCC 下没有
错误放行；Clang-ON 非零，证明开关在 Clang 构建中不是惰性开关。

### 3.2 具体 unit 三方发码

`func_tests_x86_64` 的 guest PC `0x41169a` 为 glibc `__strlen_sse2` 中的：

```text
41169a: 0f bc c2    bsf eax, edx
41169d: c3          ret
```

用 `SVM_VIXL_HOST_DUMP=1` 取 raw host bytes，再以
`objdump -D -b binary -m aarch64` 反汇编。三方调用外围如下；helper 地址的四条
MOV/MOVK 立即数随二进制装载地址不同，不属于指令选择差异。

| 项 | GCC-ON | Clang-OFF | Clang-ON |
|---|---:|---:|---:|
| unit size | 252 B | 252 B | 228 B |
| stack frame | `0xb0` | `0xb0` | `0x60` |
| `stp/ldp x10,x11` | 有 | 有 | 无 |
| `stp/ldp q11,q12` | 有 | 有 | 无 |
| `stp/ldp q13,q14` | 有 | 有 | 无 |
| x0-x7、x29/x30 save/restore | 有 | 有 | 有 |

关键片段：

```text
GCC-ON / Clang-OFF:
  sub sp, sp, #0xb0
  stp x0, x1, [sp]
  ...
  stp x10, x11, [sp, #64]
  stp x29, x30, [sp, #80]
  stp q11, q12, [sp, #112]
  stp q13, q14, [sp, #144]
  ... blr x11 ...
  ldp q11, q12, [sp, #112]
  ldp q13, q14, [sp, #144]
  ...
  ldp x10, x11, [sp, #64]
  ldp x29, x30, [sp, #80]
  add sp, sp, #0xb0

Clang-ON:
  sub sp, sp, #0x60
  stp x0, x1, [sp]
  ...
  stp x29, x30, [sp, #64]
  ... blr x11 ...
  ...
  ldp x29, x30, [sp, #64]
  add sp, sp, #0x60
```

正好少 6 条 host 指令、24 B，与 unit size 差一致。这是调用点真实走
`DirectPreserveAll` 的直接发码证据。

## 4. 正确性与指纹门

| 门 | 结果 |
|---|---|
| Clang Release 默认 `all` 构建 | PASS |
| `swift_test`, leaf OFF | PASS，142,585 assertions / 124 cases |
| `swift_test`, leaf ON | PASS，142,585 assertions / 124 cases |
| func_tests 六格 | PASS；function/block/interpreter × OFF/ON 全部 rc=101，checksum `9f52b7d59285dbe5`，stdout SHA-256 全同 |
| helper-fault, leaf OFF | PASS，38/38 |
| helper-fault, leaf ON | PASS，38/38 |
| 固定五 seed fuzz | PASS；`101,202,303,404,505`，每个 `Fuzz x86*` 均 66 assertions / 34 cases |
| OFF 官方指纹 vs GCC master | PASS；4,400 units，零 diff |
| OFF 保留 `host=` 的额外逐行对拍 | PASS；4,400 units，零 diff，双方 713,784 host bytes |
| ON 指纹自一致 | PASS；4,400 units，两遍含 `host=` 自一致 |
| ON 静态 host bytes | 713,496，较 OFF -288 B（-0.04035%） |

主要复现命令：

```sh
T=/tmp/w84-build-clang/source/tests/swift_test
SVM_HELPER_LEAF_ABI=0 "$T"
SVM_HELPER_LEAF_ABI=1 "$T"

cd /private/tmp/w64/source/translator/linux/tests
SVM_HELPER_LEAF_ABI=0 ./run_helper_fault_tests.sh \
  /tmp/w84-build-clang/source/translator/linux/svm_translator_linux
SVM_HELPER_LEAF_ABI=1 ./run_helper_fault_tests.sh \
  /tmp/w84-build-clang/source/translator/linux/svm_translator_linux

SVM_HELPER_LEAF_ABI=0 ./run_func_fingerprint_tests.sh \
  /tmp/w84-build-clang/source/translator/linux/svm_translator_linux \
  --against /tmp/svm-build/source/translator/linux/svm_translator_linux

for seed in 101 202 303 404 505; do
  SWIFT_FUZZ_SEED=$seed SVM_HELPER_LEAF_ABI=1 "$T" 'Fuzz x86*'
done
```

func_tests 六格分别固定 `SVM_FUNC_BASE=1`、`SVM_FUNC_BASE=0`、
`SVM_ENABLE_JIT=0`，每格再交叉 `SVM_HELPER_LEAF_ABI=0/1`，且始终清空
`SVM_JIT_CACHE`。

## 5. Clang 内 A/B

### 5.1 方法

- 同一个 Clang 17 Release 二进制，只切换 `SVM_HELPER_LEAF_ABI=0/1`。
- 每个 benchmark 7 对；奇数对 OFF→ON，偶数对 ON→OFF。
- 每次均 `SVM_JIT_CACHE=`，避免 disk cache 改变翻译覆盖。
- 起跑时 Orb load average 为 `2.72 / 5.11 / 4.25`，16 vCPU。
- c-ray：`scene.json -j 1 -s 8 -d 160x120 --no-sdl`，指标为 render 秒，
  ratio=`OFF/ON`；14 次 PNG IDAT MD5 都为
  `f9afd5bed804b5c2654303c9252d09c4`。
- CoreMark：`0x0 0x0 0x66 150000 7 1 2000`，指标 Iterations/Sec，
  ratio=`ON/OFF`；14 次均 `Correct operation validated` 且 CRC oracle 同一。
- 7-Zip：`b -mmt1 -md=16m`，指标 Tot MIPS，ratio=`ON/OFF`；14 次结构 oracle
  同一。
- OpenSSL：`speed -seconds 8 -evp sha256`，指标 16 KiB 列 kB/s，
  ratio=`ON/OFF`；14 次 algorithm/version oracle 同一。
- CI：对 7 个 paired ratio 做 100,000 次有放回 bootstrap，固定随机种子
  `840073`，统计量为 median，报告 percentile 2.5%/97.5%。所有 ratio 都以
  `>1` 表示 ON 更快。

### 5.2 逐对数据

每格格式为 `OFF -> ON (ratio)`。

| pair / order | c-ray s | CoreMark iter/s | 7-Zip Tot MIPS | OpenSSL SHA-256 kB/s |
|---|---:|---:|---:|---:|
| 1 / OFF→ON | 1.670→1.680 (`0.994048`) | 12294.074→12326.403 (`1.002630`) | 2774→2785 (`1.003965`) | 458854.27→434470.91 (`0.946860`) |
| 2 / ON→OFF | 1.600→1.640 (`0.975610`) | 12278.978→12301.132 (`1.001804`) | 2841→2873 (`1.011264`) | 424040.45→447068.43 (`1.054306`) |
| 3 / OFF→ON | 1.640→1.640 (`1.000000`) | 12400.794→12359.921 (`0.996704`) | 2801→2816 (`1.005355`) | 451940.67→465604.23 (`1.030233`) |
| 4 / ON→OFF | 1.660→1.640 (`1.012195`) | 12331.470→12333.498 (`1.000164`) | 2823→2844 (`1.007439`) | 453630.21→452020.22 (`0.996451`) |
| 5 / OFF→ON | 1.640→1.640 (`1.000000`) | 12283.000→12390.550 (`1.008756`) | 2796→2842 (`1.016452`) | 453203.97→447047.68 (`0.986416`) |
| 6 / ON→OFF | 1.640→1.640 (`1.000000`) | 12417.219→12420.303 (`1.000248`) | 2807→2858 (`1.018169`) | 442183.68→453084.08 (`1.024651`) |
| 7 / OFF→ON | 1.620→1.690 (`0.958580`) | 12313.249→12299.114 (`0.998852`) | 2780→2792 (`1.004317`) | 443233.34→452814.08 (`1.021616`) |

注意第 2 对表格仍按 `OFF -> ON` 展示数值，`order` 列才是实际执行顺序。

### 5.3 汇总

| benchmark | median ratio | bootstrap 95% CI | 逐对符号（正/平/负） | 判定 |
|---|---:|---:|---:|---|
| c-ray | `1.000000` | `[0.975610, 1.000000]` | 1 / 3 / 3 | 无收益；0.01s 输出粒度产生 3 个平值 |
| CoreMark | `1.000248` | `[0.998852, 1.002630]` | 5 / 0 / 2 | +0.025%，噪声内 |
| 7-Zip | `1.007439` | `[1.004317, 1.016452]` | 7 / 0 / 0 | 方向稳定，但 median 仅 +0.744% |
| OpenSSL SHA-256 | `1.021616` | `[0.986416, 1.030233]` | 4 / 0 / 3 | CI 跨 1；首两对随先后顺序约 -5.3%/+5.4%，宿主漂移主导 |

7-Zip 证明 W73 的真实 Clang 路径存在小幅运行时收益；它没有把 W73 的收益面从
“有限但真实”改写为全局默认候选。c-ray 没有改善，CoreMark 为千分之一级，OpenSSL
无法从噪声中分离。按“至少 1% 且多语料稳定、CI 支持”的翻盘标准，本轮不满足。

## 6. 改动与纪律

工作树只新增本报告 `docs/w84-clang-preserveall-ab.md`；没有生产源码构建修复，
没有修改 golden、Linux linker、benchmark harness、`jit_context.cpp` 或
`trampolines.cpp`，也没有执行 commit/push/add/checkout/reset/stash。
