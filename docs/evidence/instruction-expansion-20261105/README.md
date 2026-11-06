# 指令扩展验收证据

实现和边界见 [详细文档](../../instruction-coverage-implementation-2026-11-05.md)。本目录保留最终小型汇总；原始输出、200 个固定 compile 样本、完整编码准入记录、最终 runner 与源压缩包保存在：

`/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/instruction-expansion-20261105`

以 `receipt.json`、完整 host/Android `source-manifest.json` 和 runner SHA 为当前版本身份。中途 host-only manifest 的缺漏已在 correction 中记录。Compile 的两倍 FEX 目标仍有 x87 Block 一项未满足，不能仅用测试通过表示全部性能达标。AVX-512 与 PKU 按用户选择独立推进。
