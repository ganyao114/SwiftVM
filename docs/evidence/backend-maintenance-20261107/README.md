# 后端整理验证证据

基线 `7c10a7dd`；实现与边界见[实施记录](../../backend-maintenance-2026-11-07.md)。依赖提交 `99d13b39`，实现提交 `0a037933`；精确源文件身份由source manifest绑定。

- `verification.json`：构建、各环境有效结果、设备状态与范围。
- `source-manifest.json`、`build-and-vendor-manifest.json`：232个自有C++/汇编文件及构建/依赖身份。
- `phone-tests-summary.json`、`phone-test-inputs.json`：红魔18项与真实输入SHA。
- `arm64-compile-summary.json`、`arm64-compile-inputs.json`：最终三组交替配对；含decode/发布/释放，无decode诊断时钟。
- `host-compile-summary.json`：四组合成结构/耗时检查，不作为FEX或应用性能结果。
- `rv-tests-summary.json`、`rv-console.log.gz`、`rv-*-inputs.json`：双hart完整Linux内核16项，原生资格与输入绑定。
- `host-tests-final.log.gz`保留原矩阵，包括当时新formatter的失败；`host-tests-fix.log.gz`为修正后formatter和测试异常包装的定点通过。其它已有/base合同已通过，合并后的有效状态为13通过、6按ISA跳过，没有未解决失败。
- `cleanup-receipts.json`、`rv-final-freeze.json`：两个手机namespace清空、QEMU退出、旧输入不变。

完整命令、编译日志、原始JSON、配对runner及可复现小guest保存在
`/Volumes/Data2/swiftvm-middle-results-relocated-20261025/backend-maintenance-20261107`。
RV只保留约8.9MB压缩输入和日志，没有派生展开目录。原始样本中的首次频率读取使用失效policy4路径，不作为最终性能锚点；最终CPU4直接路径的上限前后均1171200kHz。

这是职责/基础设施整理的正确性及compile回退检查；没有完整应用运行成绩、PMU或新FEX结论。
