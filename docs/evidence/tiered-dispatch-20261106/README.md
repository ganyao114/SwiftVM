# 冷解释、热编译与 consumer 切换验证

基于 `420c3d46` 的未提交工作快照。本轮在独立 IR direct consumer 之上接入实际入口热度策略；同一 VM 选择解释或编译，共用 `Semantics<Sink>`、IR opcode 合同和唯一 GuestState。冷解释不构造 Program，JIT 继续使用原来的 Block／Function 分配流程。

Mixed 默认同一 PC／FP domain 的前 8 次冷入口请求解释，第 9 次尝试编译；CLI 为 `--engine mixed --hot-threshold 8`。ready 入口先于热度策略，native 热路径不新增计数或分支。热度及 unsupported 标记使用 32B DispatchSlot 原有空余空间，Grow／重排保留，源失效清除。不可 JIT 的入口继续解释，避免重复编译失败。

运行时默认仍是 `JitOnly`，冷解释与热晋升仅在显式 Mixed 下启用。全量回归后只调整了 launcher 帮助文字与文档，原回归快照保留。提交前重建最终 Android runner，未指定 engine 时同一循环的 Block／Function 都以 40 退出，解释步数均为 0；最终 runner 身份、快照差异与设备清理结果见 [提交收据](submission-receipt.json)。

`InterpretBlock` 在控制流、服务、代码改写、REP 单次迭代、预算或 ready 后继入口前结束。native 退出桥发布 pins／portable flags 并恢复宿主 FP，继续解释实际 PC；重新进入 native 复用现有状态与 FP domain。部分完成后返回 Incomplete，禁止切换 consumer 重放。

- 格式检查：159 个 C++ 文件，0 失败。
- 主机 CTest：10/10。
- 红魔 `bf1359ff` 原生回归：9/9。
- 新场景覆盖冷块零编译、Block／Function 热晋升、unsupported 禁重复尝试、目录扩容／重排及失效复位、ready 入口停止解释、未跳转 Jcc 边界、REP 和失败前缀保留。

同一 40 次循环在红魔 CLI 的实际结果如下，均正常以 40 退出：

| scope | 冷入口阈值 | 完成解释步 | 编译尝试 | native bytes |
| --- | ---: | ---: | ---: | ---: |
| Block | 255 | 124 | 0 | 0 |
| Block | 8 | 31 | 1 | 116 |
| Function | 8 | 29 | 1 | 160 |

两种热场景只编译一次，其余循环由 native 执行。Function 可把出口一同形成，Block 出口仍解释，因此解释步数不同。入口访问次数不是 guest 指令数；阈值 8 是可配置默认值，未据此宣称最优吞吐。

源文件与测试二进制 SHA、构建配置、原始 CLI JSON 及清理收据均在本目录。完整命令输出、压缩源码和 runner 保存在 Data2 的 `verification.json` 所列路径。两个设备测试目录已清空，无残留进程、打开文件或清理错误。

本轮没有新的 FEX 或 compile 时间对照；以上只证明动态交接和语义回归。未实现按时间降级、后台编译或外部抢占 native 热循环。
