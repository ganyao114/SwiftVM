# 独立 IR direct consumer 验证

本次基于 `420c3d46` 的工作快照。`source-manifest.json` 记录 162 个活动源／汇编文件，构建配置另见 `build-config.json`。IR direct 数值实现从 x86 前端移到 `ir/direct`，同一份 `Semantics<Sink>` 通过统一 `Emit<OpCode>` 接口选择 Builder 或立即数值执行；没有 Program 构造／遍历或另一份 guest opcode 分派。

新增 `InterpretDecoded` 接受与 `BuildDecodedIR` 相同的 owned decoder record 和源字节。调用方在 decode／完整提交边界选择 consumer，`InterpretOne` 只作 fetch → DecodeOne → InterpretDecoded 包装。现有 Mixed 交接保留；未新增外部抢占 JIT 热循环协议，也未声称消除既有编译失败回退中的再次取码。

- 格式检查：159 个文件，0 失败。
- 主机 CTest：10/10，通过独立 IR direct 数值／Uniform、metadata 快照、raw view、服务／故障及原有覆盖。
- 红魔 `bf1359ff`：9/9，包括 IR direct、x86 interpreter、runtime engine、ARM64、AVX、x87 等现有原生回归。
- 同一 decoded 记录分别提交 Builder 与 IR direct，后者在 fetch 服务缺席时执行成功。
- ENTER 已完成一次栈写后，后续服务拒绝会返回 Incomplete，保留进度并禁止换 consumer 重放；未加事务日志或回滚表。
- 数值 TU 保留 `-frounding-math`，FP 环境及 Producer 完整数值快照合同沿用。

原生测试输入 SHA 见 `inputs.json`。本轮未运行新的 FEX 或性能对照；结果用于接口与语义回归，不作性能声明。设备临时目录已清空，进程／打开文件与实际释放量见 `phone-cleanup-receipt.json`；大构建与压缩源码／runner 保留在 Data2 固定缓存及结果目录。

RISC-V 仅完成只读探索，用户纠正方向后已停止；本次没有接入 RISC-V 后端。
