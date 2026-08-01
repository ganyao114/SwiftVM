# W90：FPCR 边界税削减 spike

## 1. 结论

归因与最小实现均已完成。`SVM_SSE_AFP_NAN` 仍默认 OFF；OFF 不发射缓存或探针代码。

实现前，c-ray/smallpt 的 AFP 边界机械静态税分别有 99.7810% / 99.1869% 来自 direct
helper。实现后，两项真实语料的 helper 返回缓存查询均为 **100% hit**：c-ray
4,808,127 / 4,808,127，smallpt 311,205 / 311,205；完整 MXCSR→FPCR 重建仅剩每次
JitRun 的入口初始化，分别为 9,209 / 2,211 次。探针机械动态估算分别下降
46.036579% / 45.724362%。

这证明“MXCSR 未变时跳过构造链”命中了 W89 回退的主要表面积。上述是静态指令数乘
事件数的机械估算，不是墙钟收益证据；是否让 c-ray/smallpt 回到 CI 跨零，仍以 Orb
交错 A/B 为准。

## 2. 测量探针

新增默认 OFF 的 `SVM_FPCR_TAX_PROF=<path>`，作为第 62 个环境变量。它不复用
`SVM_EXEC_PROF`，也不向 RegAlloc 增加固定 clobber：JIT 计数序列在栈上保存/恢复
`ip0/ip1`，避免改变待测 RA 形态。

计数存于每个 `RuntimeProfileInterface` 尾部的 Runtime 私有数组，Runtime 析构时才用
relaxed atomic 汇总到进程计数。探针开启时禁用磁盘 JIT cache，避免含进程私有计数
地址契约的代码跨进程复用。普通 dispatcher entry 也计数，作为“unit 调度本身不交
FPCR 税”的对照。

实现后探针还记录：

- `cache_lookups`：所有 guest-FPCR 恢复点；
- `cache_hits` / `cache_misses`：当前 `context.mxcsr` 与栈缓存 source 的比较结果；
- `rebuild_executed`：JitRun 入口初始化加所有 miss 的完整重建数。

探针自身的保存、计数和恢复指令不计入机械税估算。

## 3. 实现前归因

Release 构建：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

c-ray：

```sh
cd /private/tmp/w90-profile/cray
SVM_SSE_AFP_NAN=1 \
SVM_FPCR_TAX_PROF=/private/tmp/w90-profile/cray/fpcr.count \
/tmp/w64/build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/cray_x64 \
  /Users/swift/CLionProjects/SwiftVM-bench/src/c-ray/input/scene.json \
  -j 1 -s 8 -d 160x120 -o rendered.png --no-sdl
```

smallpt：

```sh
cd /private/tmp/w90-profile/smallpt
SVM_SSE_AFP_NAN=1 \
SVM_FPCR_TAX_PROF=/private/tmp/w90-profile/smallpt/fpcr.count \
/tmp/w64/build/source/translator/linux/svm_translator_linux \
  /Users/swift/CLionProjects/SwiftVM-bench/bin/smallpt_wh_x64 64 320 240
```

两项均 rc=0 并生成非空图像。实现前原始计数：

| 边界 | c-ray events | c-ray dynamic est / 占比 | smallpt events | smallpt dynamic est / 占比 |
|---|---:|---:|---:|---:|
| runtime entry | 9,145 | 118,885 / 0.1898% | 2,211 | 28,743 / 0.7047% |
| runtime return | 9,145 | 18,290 / 0.0292% | 2,211 | 4,422 / 0.1084% |
| dispatcher entry | 20,744,556 | 0 / 0% | 36,147,516 | 0 / 0% |
| asm interpreter | 0 | 0 / 0% | 0 | 0 / 0% |
| trampoline CallHost | 0 | 0 / 0% | 0 | 0 / 0% |
| direct helper | 4,808,130 | 62,505,690 / **99.7810%** | 311,205 | 4,045,665 / **99.1869%** |
| MemoryCopy | 0 | 0 / 0% | 0 | 0 / 0% |
| StoreUniform(mxcsr) | 0 | 0 / 0% | 0 | 0 / 0% |
| **合计** | — | **62,642,865** | — | **4,078,830** |

dispatcher/runtime-entry 比分别为 2,268.404 和 16,348.944。普通 unit 返回在
`halt_reason == 0` 时直接回 `code_dispatcher`，不会离开 JitRun，也不会恢复/重装
FPCR。因此 W89 的 small-unit 回退并非“每 unit 一次税”，而是 helper 边界密度高。

## 4. 缓存机制

### 4.1 每 JitRun 独立栈帧

AFP runtime-entry 帧由 16 B 扩为 32 B：

| offset | 内容 |
|---:|---|
| `+0` | `host_fpcr`，进入 JitRun 时 `MRS FPCR` 保存 |
| `+8` | `guest_fpcr`，由 MXCSR 完整构造的缓存值 |
| `+16` | `source_mxcsr`，构造上述值时使用的 MXCSR |
| `+24` | 对齐/保留 |

入口从 `context.mxcsr` 完整构造一次 guest FPCR，写入后两项缓存再 `MSR FPCR`。退出
JitRun 时恢复 `host_fpcr` 并一次性弹出 32 B。缓存不跨 Runtime、不跨线程，也不跨
JitRun。

### 4.2 所有恢复点统一比较

`EmitSseAFPRestoreGuestFPCRCached` 是唯一的 host→guest AFP 恢复路径，覆盖：

- asm interpreter 返回；
- trampoline `CallHost` 返回；
- `EmitHostCall` direct helper 返回；
- `EmitMemoryCopy` 返回；
- `StoreUniform(mxcsr)` 即时同步。

每处均执行同一协议，无 helper/MemoryCopy 清洁性特例：

1. 从当前 JitRun 栈帧加载 `{cached_guest_fpcr, source_mxcsr}`；
2. 从 `context.mxcsr` 重新加载当前值并按 32 位比较；
3. 相等：直接 `MSR FPCR, cached_guest_fpcr`；
4. 不等：从已加载的当前 MXCSR 完整构造 guest FPCR，同步写回两项缓存，再 `MSR`。

`StoreUniform(mxcsr)` 使用 allocator 可见的三个临时 GPR，避免 XPOOL 与固定 ip clobber
冲突；它也走同一比较协议，写入新值时自然 miss 并更新缓存。signal 路径保持 W89 的
现有原子副本契约，完全不访问 JitRun 栈缓存。

### 4.3 静态净账

| 边界 | 实现前 AFP 专属指令 | 实现后 hit | miss |
|---|---:|---:|---:|
| runtime entry | 13 | 14（入口总是初始化） | — |
| runtime return | 2 | 2 | — |
| interpreter / CallHost / direct helper / MemoryCopy | 13 | 7 | 18 |
| StoreUniform(mxcsr) | 11 | 5 | 16 |

helper 热路径从 13→7，单次机械减少 46.153846%。miss 比旧路径多 5 条，代价来自比较、
缓存写回和控制流；因此必须由动态 hit 率证明，而不能只看单点静态形状。

## 5. 实现后复测

复现命令与 §3 相同，仅把输出路径改为 `fpcr-after.count`。结果：

| 指标 | c-ray | smallpt |
|---|---:|---:|
| runtime entry / return | 9,209 / 9,209 | 2,211 / 2,211 |
| dispatcher entry | 20,744,594 | 36,147,516 |
| direct helper | 4,808,127 | 311,205 |
| cache lookup | 4,808,127 | 311,205 |
| cache hit | 4,808,127 | 311,205 |
| cache miss | **0** | **0** |
| cache hit rate | **100.000000%** | **100.000000%** |
| rebuild executed | 9,209 | 2,211 |
| 机械动态估算 | 33,804,233 | 2,213,811 |
| 相对实现前 | **−46.036579%** | **−45.724362%** |

两轮独立运行的 c-ray entry 数有 9,145→9,209 的轻微运行间差异，helper 数仅差 3；
结论不依赖逐事件完全相同，因为实现后所有 4,808,127 次 helper lookup 都命中，且
`rebuild_executed == runtime_entry`。smallpt 两轮 entry/helper 数完全一致。

## 6. 定向正确性

扩展 `AFP guest FPCR is rebuilt from MXCSR across host-call boundaries`：

- 普通只读 helper 返回走 cache hit，helper 观察到精确 host FPCR；
- 第二个 helper 在 host FPCR 下直接把 `context.mxcsr` 从 round-up 改为
  round-to-nearest，返回后的 SSE add 得到 `0x3f800000`，证明统一比较捕获变化并走
  miss/rebuild；
- 随后的 DAZ、FTZ `StoreUniform(mxcsr)` 仍分别得到零结果；
- JitRun 返回后 host FPCR 逐位恢复。

用探针运行该用例得到 2 次 direct helper、3 次 StoreUniform、5 次 lookup、1 次 hit、
4 次 miss、5 次 rebuild（4 个 miss + 1 次入口），与用例结构精确相符。用例结果为
9 assertions / 1 case PASS。

语料输出 OFF/ON 对账：

| 语料 | 对账口径 | OFF | ON |
|---|---|---|---|
| smallpt | PPM MD5 | `e5b19a8c9ed08153f862f8eb11afe3b3` | 同左 |
| c-ray | PNG IDAT MD5（排除时间元数据） | `f9afd5bed804b5c2654303c9252d09c4` | 同左 |

## 7. macOS 验证

| 状态 | 结果 |
|---|---|
| 默认 OFF 全量 `swift_test` | PASS，140,252 assertions / 133 cases |
| `SVM_SSE_AFP_NAN=1` 全量 `swift_test` | PASS，140,252 assertions / 133 cases |
| 定向 helper-MXCSR cache miss | PASS，9 assertions / 1 case |
| `git diff --check` | PASS |

当前 Codex 文件沙箱仍拒绝 Unicorn 2.1.4 的 `hw.cachelinesize` sysctl，后者会退到
`MRS CTR_EL0` 并在测试进程内 SIGILL。双态全量都使用 build 目录内、不进 git 的
DYLD interpose，只为 `hw.cachelinesize` 返回本机 128，并为 AFP capability probe
返回 1；两态条件完全相同。该测试环境绕行不在产品 diff 中。

## 8. 改动清单

- `source/runtime/backend/arm64/fpcr_mode.h`：32 B 帧布局、拆分完整构造器、统一缓存恢复器；
- `source/runtime/backend/arm64/trampolines.cpp`：入口初始化、退出恢复、interpreter/
  CallHost 缓存路径与探针；
- `source/runtime/backend/arm64/jit/translator_control.cpp`：direct helper 统一缓存路径；
- `source/runtime/backend/arm64/jit/translator_mem.cpp`：MemoryCopy 与 MXCSR store 统一缓存路径；
- `source/runtime/backend/arm64/jit/jit_context.{h,cpp}`：不改变 RA 形态的 JIT 私有计数；
- `source/runtime/common/fpcr_tax_prof.{h,cpp}`、`source/runtime/backend/context.h`、
  `runtime.cpp`、`jit_cache.cpp`、`CMakeLists.txt`：探针注册、私有汇总和 cache 隔离；
- `source/runtime/common/perf_stats.h`、`source/tests/main_case.cpp`：env 61→62 与定向回归；
- `docs/w90-fpcr-tax.md`：本报告。

未改 signal handler、默认开关、linker、harness、golden 或 `docs/project-status.md`。

## 9. 待 orchestrator 执行

Orb 正确性门脚本已写到 `/private/tmp/w90-orb.sh`，语法检查通过。它执行：双态全量
suite、双态 helper-fault、func_tests 六格、OFF 对 `/tmp/svm-build` fingerprint A/B、
ON 固定五 seed fuzz，以及 helper 改 MXCSR 的定向回归。

仍待回填：

- Orb 双态全量断言/用例数、helper-fault 38/38、func checksum、fingerprint 和 fuzz；
- c-ray/smallpt/STREAM 同二进制 env 切换交错 A/B。

本轮只省掉 MXCSR→FPCR 构造链；每次 helper 边界的 host restore 与 guest install
`MSR` 仍保留。若 c-ray/smallpt 仍有显著残税，下一步才是带 helper 级 FP 使用标注的
整段切换豁免；本实现没有预埋或暗含该豁免。
