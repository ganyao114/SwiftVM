# W64 c-ray/svm 间歇性 900s 停滞根因调查

日期：2026-08-01  
代码：`5f18ccf53edf9aeb024922baba6fee1fd6c6188c`  
范围：纯调研；未修改 harness；临时修法探针已撤销，生产源码无遗留 diff。

## 结论摘要

1. **900s 停滞的根因已定位**：`exit_group` 对其他 guest core 发中断时，若目标 core 恰好处于两次 `Runtime::Run()` 之间，`SignalInterrupt()` 会把 `running` 置为 `false`；下一次 `Runtime::Run()` 的 `while (running)` 一次也不进入并返回 `HaltReason::None`。`X86Core::Run()` 对 `None` 在内部无限重试，因此永远回不到 `RunThread()` 外层观察 `process->IsExiting()`。主线程则永久阻塞在 `JoinAll()/pthread_join`。这是宿主侧 busy-loop 活锁，不是 guest 渲染慢、翻译风暴、SMC invalidation 或文件 I/O 阻塞。
2. **“安静机 19s → 受载机 55–69s，约 3.1×”不成立**：`baseline3` 原始 c-ray 日志是 `1m 19s`/`1m 20s`，旧 parser 只取末字段，误记成 19/20 秒。对应 `.status` wall 是 79.316–80.629 秒。正确比较是 baseline3 的约 79.8 秒与 baseline4/baseline4fix 正常样本的约 58.9/65.8 秒，不存在 3.1× 负载退化。
3. 高宿主负载仍可能提高**停滞命中概率**：调度抢占会放大“两次 `Runtime::Run()` 之间”的竞态窗口；但这与被错误解析出来的 3.1× 渲染时间不是同一个现象。
4. 最小修法是在 `Runtime::Run()` 发现 `running == false` 且没有其他 halt reason 时返回 `HaltReason::Signal`，不能返回 `None`。临时探针采用这一语义后，在当前高负载下 c-ray smoke 的 50 个 timed rep 全部正常退出（50/50，0 timeout）；探针随后已撤销。

## 一、停滞现场

### 1. 历史样本都已完成 guest 工作

磁盘上实际的超时格为：

| 结果 | rep | wall | guest 最后阶段 | PNG |
|---|---:|---:|---|---|
| baseline4 | 2 | 900.037s | 55.51s 完成渲染，随后 `Render finished, exiting.` | 142,312 B |
| baseline4fix | 2 | 900.132s | 2m18s 完成渲染，随后 `Render finished, exiting.` | 142,312 B |
| baseline4fix | 5 | 900.719s | 55.48s 完成渲染，随后 `Render finished, exiting.` | 142,312 B |

注意：当前 on-disk `baseline4/matrix.csv` 标记的是 rep2 超时、rep3 成功；这与任务描述中的 rep3 不一致。后续判断以原始 `.status` 和 `.out` 为准。

三个超时样本均已输出：

- `Renderer exiting`
- `Finished render in ...`
- `Render finished, exiting.`

并写出完整大小的 `rendered_0000.png`。因此超时发生在 guest 计算和 PNG 写出之后。

### 2. 本轮复现

复现标签：`w64probe-stall1`；复现时 loadavg 约 23–30。

- 01:46:36 启动 prime。
- 01:47:58 guest 到 100%，输出 `Renderer exiting`、`Finished render in 1m 22s`、`Render finished, exiting.`，并写出 142,312 B PNG。
- 01:48:32 translator 仍存活，进程瞬时 CPU 100%，elapsed 1:56、累计 CPU 1:44。
- 连续两个 5 秒 `sample` 都得到同一现场。

### 3. sample 栈

两个 sample 分别采到 4181 和 4201 个样本：

- main thread：全部在 `X86GuestProcess::JoinAll()` → `std::thread::join()` → `_pthread_join()` → `__ulock_wait`。
- 唯一剩余 child thread：全部在 `X86GuestProcess::RunThread()` → `X86Core::Run()`；热点在反复调用的 `Runtime::Run()`、`GetUniformBuffer()`、`GetLocation()` 和 `InstallThreadAltStack()`。
- child thread 没有停在 guest JIT 地址、`futex`、write/fsync、SMC lock、mprotect 或翻译函数；它在宿主控制循环中持续空转。

证据文件：

- `results/w64probe-stall1/probes/prime_sample2.txt`
- `results/w64probe-stall1/probes/prime_sample3.txt`
- `results/w64probe-stall1/work/cray_svm/prime/prime.out`

## 二、具体竞态机制

正常退出路径：

1. guest leader 执行 `exit_group`。
2. `SyscallProcessState::RequestExitGroup()` 设置 process-wide `exiting=true`。
3. leader 的 `RunThread()` 调用 `InterruptAll()`，对其他 core 调用 `SignalInterrupt()`。
4. child core 应从 `core->Run()` 返回 `ExitReason::Signal`；外层 `RunThread()` 调用 `ClearInterrupt()`，下一轮在顶部观察 `process->IsExiting()` 并退出。

竞态路径：

1. child core 正好处于 `Runtime::Run()` 调用之间。
2. `SignalInterrupt()` 执行 `running.store(false)` 并设置 `halt_reason=Signal`。
3. child 再进入 `Runtime::Run()`：局部 `hr` 初始化为 `None`，`while (running)` 因 false 完全跳过，函数直接返回 `None`。
4. `X86Core::Run()` 的内部 `while (hr == None)` 立即再次调用 `Runtime::Run()`；`running` 一直为 false，所以永久返回 `None`。
5. 控制永远回不到 `RunThread()` 的 `ExitReason::Signal` 分支，无法执行 `ClearInterrupt()`，也无法再次检查 `process->IsExiting()`。
6. leader 已退出并在 `JoinAll()` 等该 child；一线程 wait、一线程 100% host busy-loop，直到 harness 900s kill。

这也解释了：

- 非确定性：取决于 interrupt 落在 JIT 内还是两次 Run 调用之间。
- rep 序号不固定：与输入/缓存内容无确定关联。
- 高负载可能提高命中率：抢占扩大 host-side gap。
- guest 输出总是在正常完成后停止：活锁发生在 process teardown。

## 三、“负载超敏”复核

### 1. baseline3 的 19 秒是 parser 假象

`baseline3` 的有效 SVM rep：

| rep | matrix `render_s` | `.status` wall | 原始日志 |
|---:|---:|---:|---|
| 2 | 19 | 79.749s | `Finished render in 1m 19s` |
| 3 | 20 | 80.629s | `Finished render in 1m 20s` |
| 4 | 19 | 79.806s | `Finished render in 1m 19s` |
| 5 | 19 | 79.316s | `Finished render in 1m 19s` |

rep1 也在 1m20s 完成 guest 输出，随后命中同类 900s 退出停滞。

旧 parser 对 `1m 19s` 仅取 `$NF`，故得到 19。当前 harness 已按分钟字段换算成 79 秒；本调查未修改 harness。

类似证据在 baseline4 仍可见：rep4 wall 72.880s，原始日志 `1m 12s`，旧 metric 却是 `render_s=12`。baseline4fix rep1/rep3 也分别把 1m05s/1m08s 记成 5/8 秒。

按 `.status` wall 计算：

- baseline3 正常 SVM：79.316, 79.749, 79.806, 80.629s，中位约 79.78s。
- baseline4 正常 SVM：46.964, 57.824, 59.888, 72.880s，中位约 58.86s。
- baseline4fix 正常 SVM：54.908, 65.811, 68.876s，中位 65.81s。

因此没有“19 → 55–69 秒”的 3.1× 回退。

### 2. 执行/翻译/SMC 分解

默认 `SVM_EXEC_PROF=1` 当前会触发独立的 scratch-budget 断言：`opcode 33: declared 7, asked for 8`。为取得分解数据，探针跑使用 `SVM_JIT_SCRATCH_XPOOL=0`；这会改变代码生成，故绝对性能不与默认配置直接比较，只用于时间归因。

受载（loadavg 约 30）、320x240、8 samples、cold cache 的 timed rep：

| 项 | 数值 | 占 19.444s wall |
|---|---:|---:|
| 最长 runtime elapsed | 19.329s | 99.4% |
| 全部翻译 `translate_total` | 0.594s / 8,343 calls | 3.1%（与执行有启动重叠） |
| SMC close | 13.70ms / 10,545 calls | 0.07% |
| JIT cache | loaded=0, compiled=8,303, stored=8,123 | 冷跑预期 |

同一场景 oracle 为 22.986s runtime、0.599s 翻译、22.79ms SMC。翻译量约 8.3k units，和完整 64-sample 复现的 8,308 个映射同量级；增加 samples 主要增加同一组已翻译热块的执行次数，不会把固定约 0.6s 翻译放大成几十秒。

结论：受载 wall 主体是 guest JIT 执行获得的 CPU 时间；没有 3× 异常路径需要解释。相对 FEX/Rosetta 的总体性能差距属于生成代码效率/正常调度份额问题，不是本 W64 停滞根因。

## 四、候选假说裁定

| 假说 | 裁定 | 证据 |
|---|---|---|
| guest 仍在计算但极慢 | 否 | guest 已 100%、完成 PNG 和退出日志；sample 不在 guest JIT |
| 翻译风暴/反复失效重编 | 否 | sample 无 Translate；映射数量有限；翻译约 0.6s/8.3k units |
| dispatcher/invalidation 活锁 | 部分接近但需精确改名 | 是 host control-loop 活锁；不是 dispatcher 或 invalidation，而是 `running=false` + `None` 返回契约错误 |
| syscall 模拟阻塞、PNG write/fsync | 否 | 文件已写完；sample 无 syscall/write/fsync；停滞发生在退出后 |
| 编译线程与执行线程竞争 | 否 | 无后台编译线程造成几十秒增长；翻译总量约 0.6s |
| SMC write-window/mprotect 受压 | 否 | SMC close 13.7–22.8ms，只占约 0.07–0.10% |
| JIT cache 抖动 | 否 | 即使 cold、0 loaded、全量编译也仅约 0.6s |
| guest 自旋被调度放大 | 否（渲染期）；是（退出后宿主空转） | 正常渲染进度持续；停滞 sample 是宿主 `Runtime::Run()` 空循环 |
| 19→55–69s 的负载超敏 | 否 | `1m 19s` 被旧 parser 误记为 19s |

## 五、修法建议

### P0：修正 `Runtime::Run()` 的 stop 返回契约

最小语义：当 `running == false` 导致循环未进入/退出，且没有更具体的 halt reason 时，必须返回 `HaltReason::Signal`，不能返回 `None`。

可在现有 return 前做等价处理：

```cpp
if (hr == HaltReason::None && !running.load(std::memory_order_acquire)) {
    return HaltReason::Signal;
}
return hr;
```

这个修法不依赖非原子的 `state->halt_reason` 是否已被 trampoline 消费；下一层会稳定得到 `ExitReason::Signal`，外层执行 `ClearInterrupt()` 并观察 process-wide exit。

临时探针验证：当前高负载下 c-ray smoke 50/50 timed rep 正常退出，0 timeout；每 rep wall 约 0.47–0.50s。探针代码已撤销，未留在工作树。

建议补两类回归：

1. runtime 单元测试：在两次 `Runtime::Run()` 之间调用 `SignalInterrupt()`，断言下一次 Run 返回 Signal 而非 None。
2. Linux guest 多线程退出压力：反复 clone + `exit_group`，扩大调度扰动，断言所有 host thread 有界 join。

预估收益：消除该 900s timeout 类别；对正常渲染吞吐应近似零影响，只改变已有 interrupt 的退出路径。

### P1：修复 `SVM_EXEC_PROF` 与默认 XPOOL 的 scratch 契约

这不是 W64 根因，但会阻碍后续性能测量。应让 exec counter 使用的两个 GPR 纳入 `ScratchBudget`/固定 clobber 契约，或改为不会与 XPOOL 分配冲突的保留寄存器路径。修后需验证 probe on/off 的 guest 语义一致，并明确 profiler overhead，不应把 profiler 数字当默认配置绝对性能。

### 临时缓解

- 保留 cell timeout 只能避免无限占用，不能修复丢失的退出通知。
- 没有证据支持通过关闭 SMC、清 JIT cache、改 PNG 写路径或提高 timeout 来缓解；这些方向不应进入默认方案。

## 六、产物与清洁度

- 停滞现场：`/Users/swift/CLionProjects/SwiftVM-bench/results/w64probe-stall1/`
- 分解探针：`/Users/swift/CLionProjects/SwiftVM-bench/results/w64probe-prof-s8/`
- 修法压力测试：`/Users/swift/CLionProjects/SwiftVM-bench/results/w64probe-fix-smoke50/`
- harness 未修改。
- `source/translator/linux/linker/` 未修改。
- 未执行 git add/commit/push/checkout/reset/stash。
- 最终 `git diff` 为空；仅本报告和本地 build/result 产物为未跟踪调查产物。
