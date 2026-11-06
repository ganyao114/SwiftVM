# 代码整理与格式化验收：2026-11-06

功能整理提交为 `ea97d8b2`，覆盖运行时职责拆分、共享前端／解释器、受控 VIXL、用户态指令扩展及 compile 缓冲复用。随后单独提交格式化，方便区分行为修改和布局变化。

本轮按根目录 `.clang-format` 使用 NDK clang-format 21，覆盖 151 个自有 C++ 文件，保留 include 顺序。三个 opcode 表仍是一行一个宏；AVX 语义宏使用显式 StatementMacros 配置。一处带逐指令反汇编注释的测试字节表保留手工对齐。第三方源码、静态汇编和历史测量证据沿用原布局。

`scripts/format_runtime.py` 提供相同范围的格式化及 `--check`。Python 清点脚本使用 Ruff 0.17.4 格式化，前后 AST 一致。前端 901 个 mnemonic／语义入口、316 个 VEX 描述符，以及后端 typed／structural 入口和分派清点保持一致；描述符比较忽略表达式中的空白，源码 SHA 和行号不作为语义一致性判断。

| 检查 | 结果 |
| --- | --- |
| C++ 格式复查 | 151 个文件通过 |
| macOS ARM64 Release／IPO | 构建通过，CTest 9/9 |
| ASan + UBSan | 构建通过，CTest 9/9，无报告 |
| Android NDK29／API28／ThinLTO | 构建通过 |
| 红魔实际执行 | 7 个 C++ 检查 + 8 个 workload × Block/Function，共 23/23 |
| 状态图像 | InterpretOnly／Block／Function，3/3 |

CoreMark 只跑 20 次迭代检查 exit0 和 CRC `0x4983`，不作为正式跑分。初版汇总脚本误要求正式跑分的“至少 10 秒”验证消息，因此先把两条短 smoke 标为失败；已在原始 stdout 上按既有短 smoke 合同复核并修正。初始判定与修正原因均保留，没有重跑或替换样本。x87 workload 延用 exit-only smoke，数值与状态检查由专门的覆盖测试提供。

本轮没有新的 compile benchmark 或完整 runtime/PMU 成绩。[最近一次性能记录](compile-cost-optimization-2026-11-06.md)继续绑定其原有源码和 runner；其中 x87 Block 的严格 2× FEX 目标仍未关闭。README、架构说明及依赖说明已同步到当前职责和实际构建设置。

格式化后源码依赖清单为 host/Android 并集 282 项，digest：`8cfb6b743bf838545951d348413cd501e6e1a1612644095aadd43a0d3a020977`。Android runner SHA256：`83e10c8fa6cb94dab777ce2c07c8882e2d9634bbc3393728a70641f3f7961946`。

小型清单、日志、检查结果与手机清理收据见 [evidence](evidence/submission-20261106/README.md)。原始目录为 `/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/format-submit-20261106`。手机 namespace 已检查无活动进程和打开文件后清除，剩余 0B；实际释放约 148MB。所有构建复用 Data2 的固定缓存，冻结旧树保持只读。
