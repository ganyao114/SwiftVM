# W83：function fingerprint golden 平台/feature 分片

日期：2026-08-01  
基线：`590bd3584833e6e5c69913b9aac2a2ce8dc6ed9a`  
分支：`w83-golden-shard`

## 结论

选择 **平台 + 有效 FlagM/CFINV 分片**，不采用 preload 归一化。

指纹是 execution-driven gate：宿主 `fstat` 元数据会改变 guest glibc 实际走到的
basic block，宿主 lowering feature 也会合法改变 IR。把这些输入编码进 fixture identity
比在测试进程里注入一层 libc hook 更直接，也不改变生产 syscall 语义。新机制的 profile
为：

```text
darwin-flagm      darwin-noflagm
linux-flagm       linux-noflagm
```

这里的 `flagm` 指“本次运行 CFINV 实际可用”：宿主报告 FEAT_FlagM，且没有设置
`SVM_FLAGS_CFINV=0`。FlagM2 当前没有已证明会改变这套 corpus IR 的使用点，因此没有
先制造一层空分片；以后若 AXFlag 开始改变指纹，可按同一 profile 机制扩展。

正式 `func_fingerprint_golden.txt` 没有修改。四份 candidate 均只在：

```text
/private/tmp/w83-candidates/
```

由 orchestrator 决定哪些 candidate 成为正式 shard。

## 1. W78 后现状复验

Orb `wine-ci` 从本工作树新建 `/tmp/w83-build`：

```sh
orb -m wine-ci bash -lc '
  cmake -S /private/tmp/w66 -B /tmp/w83-build -DCMAKE_BUILD_TYPE=Release
  cmake --build /tmp/w83-build --target svm_translator_linux -j8
  /private/tmp/w66/source/translator/linux/tests/run_func_fingerprint_tests.sh \
    /tmp/w83-build/source/translator/linux/svm_translator_linux
'
```

旧门结果为 rc=1、4400 units self-consistency OK。完整 diff 是 **18 条 content line**，
而且只剩 W77 的 `st_blksize` 族：

| guest | Linux 选中 | Darwin golden 选中 | totals |
|---|---|---|---|
| `real_hello` | `0x438cb5 ir=10`, `0x438cbd ir=12` | `0x438d00 ir=13`, `0x438d0d ir=12` | `21681→21678` |
| `real_busy` | `0x4415b5 ir=10`, `0x4415bd ir=12` | `0x441600 ir=13`, `0x44160d ir=12` | `27602→27599` |
| `func_tests` | `0x459705 ir=10`, `0x45970d ir=12` | `0x459750 ir=13`, `0x45975d ir=12` | `26987→26984` |

计数为每组 `2 additions + 2 removals + 1 totals addition + 1 totals removal = 6`，
三组共 18。W77 的两个 FlagM same-PC unit 已完全消失：W78 后
`real_busy:0x439970`、`func_tests:0x419c10` 都回到 45 IR。原先被 `+3/-3` 互消的
`real_busy`、`func_tests` totals 现在各显现 `-3`，与 W78 commit 说明一致。

原始证据位于 Orb：

```text
/tmp/w83-baseline-fingerprint.log
/tmp/w83-linux-current.txt
/tmp/w83-linux-current.diff
```

## 2. 方案比较

| 维度 | 平台/feature 分片 | harness preload/包装归一化 |
|---|---|---|
| 生产语义 | 完全不碰 | 只要严格限定在子进程也可不碰 |
| 双平台实现 | shell 选择 reference | Linux 需要 ELF `LD_PRELOAD`，mac 需要 Mach-O/DYLD 对应实现 |
| fixture 身份 | 明确记录真实平台与有效 feature | 隐藏真实平台差异，仍需另解 FlagM feature |
| `--against` | 不参与 reference 选择，零侵入 | 必须保证 A/B 两进程注入完全一致 |
| 长期维护 | 新环境显式新增 profile | 维护两套 hook ABI、编译产物和 libc symbol 覆盖 |
| 失效形态 | 缺 profile 时 setup fail | hook 未加载时可能静默退回宿主值 |

W77 的 preload 是很好的因果探针，但不适合作为长期 fixture：它只解决
`st_blksize`，不解决真实 feature 差异，还引入双平台动态加载机制。选择平台分片也
符合 W77 已归档的推荐处置，并保留每条 unit 记录的回归检测能力；没有使用 PC ignore
list。

## 3. 实现

只修改 `source/translator/linux/tests/run_func_fingerprint_tests.sh`：

1. 通过 `uname -s` 得到 `darwin/linux`。
2. Darwin 用 `sysctl hw.optional.arm.FEAT_FlagM`，Linux 用 `/proc/cpuinfo` 的独立
   `flagm` token；`SVM_FLAGS_CFINV=0` 强制选择 `noflagm`。
3. shard 命名为 `func_fingerprint_golden.<profile>.txt`。
4. `SVM_FP_GOLDEN_DIR` 可把 fixture 定向到 `/tmp` candidate 目录；
   `SVM_FP_GOLDEN_PROFILE` 可显式指定 profile。profile 只接受
   `[A-Za-z0-9._-]`，避免路径注入。
5. 选择顺序：显式 override → 当前 profile 的已安装 shard → 若已经进入 shard 时代但
   当前 profile 缺失则 setup fail → 尚未安装任何 shard时才用 legacy golden。
6. 没安装 shard、没设 override 时继续使用原
   `func_fingerprint_golden.txt`；历史 check/`--update` 行为保持不变。
7. `--against` 仍只比较 A/B 两个临时 fingerprint，不读取、创建或打印 golden shard。

candidate 的生成方式不会触碰正式 golden：

```sh
SVM_FP_GOLDEN_DIR=/private/tmp/w83-candidates \
  run_func_fingerprint_tests.sh SVM --update

SVM_FLAGS_CFINV=0 SVM_FP_GOLDEN_DIR=/private/tmp/w83-candidates \
  run_func_fingerprint_tests.sh SVM --update
```

## 4. 双平台 candidate 与 diff 收敛

四份 candidate 都是 4411 行、4400 unit：

| profile | SHA-256 |
|---|---|
| `darwin-flagm` | `ad6723e7204a557fa310022a7cbf926dae88196bab8a9c35158b60d34fb9be5f` |
| `darwin-noflagm` | `fab16f27f28132945827ffdf1dbebe2a482f213070203d727b99ed02815a1af9` |
| `linux-flagm` | `eff80f190f851479ecfc909fd466117f366b8da27bd791693c78d0af7f6a0a6e` |
| `linux-noflagm` | `81177e7257317e077595fea77b7744eac69abb5d648955a9c2c3529772f96edb` |

默认 feature 的正式检查：

```sh
SVM_FP_GOLDEN_DIR=/private/tmp/w83-candidates \
  source/translator/linux/tests/run_func_fingerprint_tests.sh \
  /tmp/w83-build/source/translator/linux/svm_translator_linux
```

结果：

| host | 自动 profile | self-consistency | candidate diff |
|---|---|---|---|
| mac | `darwin-flagm` | 4400 units，OK | **0** |
| Orb wine-ci | `linux-flagm` | 4400 units，OK | **0** |

强制 `SVM_FLAGS_CFINV=0` 后分别自动选择 `darwin-noflagm`、`linux-noflagm`，两端
再次都是 4400 units、candidate diff **0**。同平台 `flagm` 与 `noflagm` candidate
各相差 8 条 content line，正是两个 FlagM unit 及其 totals；feature 维度实际生效。

平台维度同样精确：

- `darwin-flagm` 与原正式 golden 字节相同；
- `linux-flagm` 对原正式 golden 恰好是上述 18 条 content diff；
- 没有额外、无法解释的记录。

## 5. 向后兼容证据

不设置新变量：

| 检查 | mac | Orb |
|---|---|---|
| 旧 `script SVM` | rc=0，仍匹配 legacy golden | rc=1，仍是原 18-line pre-existing drift |
| `script SVM --against SVM` | rc=0 | rc=0 |
| shard profile 输出 | 无 | 无 |

这证明脚本没有在 legacy 调用里暗中归一化或换 reference；只有显式 candidate 目录或
以后安装的正式 shard 才启用新选择逻辑。

负向检查也符合约定：显式空 candidate 目录返回 rc=2 并指出缺少
`func_fingerprint_golden.darwin-flagm.txt`；含 `/` 的非法 profile 返回 rc=2，未访问
任何 reference。

正式 `func_fingerprint_golden.txt` 的 SHA-256 在实现前后均为：

```text
ad6723e7204a557fa310022a7cbf926dae88196bab8a9c35158b60d34fb9be5f
```

## 6. 全量验证

构建与运行：

```sh
cmake --build /tmp/w83-build --target swift_test -j8
/tmp/w83-build/source/tests/swift_test
```

| 平台 | 结果 |
|---|---|
| mac | **139975 assertions / 124 cases，全部通过** |
| Orb wine-ci | **142585 assertions / 124 cases，全部通过** |

日志：mac `/tmp/w83-mac-swift_test.log`；Orb `/tmp/w83-orb-swift_test.log`。

## 7. 改动与交付边界

改动文件：

- `source/translator/linux/tests/run_func_fingerprint_tests.sh`
- `docs/w83-fingerprint-golden-shards.md`

未修改：

- `source/translator/linux/tests/func_fingerprint_golden.txt`
- `source/translator/linux/linker/`
- `SwiftVM-bench/harness/run_matrix.sh`
- 任何生产 syscall/JIT 源码

未执行 git commit/push/add/checkout/reset/stash。candidate 仅在
`/private/tmp/w83-candidates/`，不属于工作树。
