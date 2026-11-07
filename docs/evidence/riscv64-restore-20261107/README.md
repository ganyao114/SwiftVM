# RV64 恢复与回归验证

本批基于 `fb9c8e1f`；编码依赖提交为 `6a561d66`，功能实现提交为 `c0611141`，与已测源码清单一致。RV64G 后端直接消费当前 Uniform IR，与 ARM64 共用优化器、VM、CodeHeap、PC 目录、link 和 SMC 生命周期；分别保留 Block／Function 分配。默认仍为纯 JIT。文件职责及接受边界见 [后端说明](../../../source/runtime/backend/riscv64/README.md)。

| 检查 | 结果 |
| --- | --- |
| 格式 | 188 个自有 C++ 文件通过 |
| macOS ARM64 | 既有及新目标共 11/11；最终 RV 目标增量复查通过，明确不在 ARM 上执行 RV 指令 |
| RV64 交叉构建 | GCC 13／RV64G／LP64D／Release／LTO，runner 和两项测试构建通过 |
| RV64 生成代码 | 最终 23 个场景实际执行通过，含 flags、局部边前 spill、CFG／回边、pair／spill、整数极值、FP 四舍入模式／DAZ/FTZ、helper ABI |
| 完整 RV64 Linux 内核 | 2 个模拟 hart；runtime engine、23 native、Mixed B/F、Func B/F、CoreMark B/F，8 个场景一次通过 |
| RV64 workload | int／fp／mem／branch／call／x87 × B/F，共 12/12；前五项 checksum 与冻结原生 oracle 一致，x87 为 exit-only smoke |
| RV64 原子／SMC／故障 | 12/12，包括 CAS128 写监视、代码改写重编译、8/16/32/64 原子、访问故障和未对齐原子明确停止 |
| 红魔 ARM64 | 原有 9 项 native 回归全部通过 |

Mixed 阈值 8 时，Block／Function 均以 40 退出，只编译 1 次，分别完成 31／29 步解释。Func fixture 的成功合同为 7 行 checksum 全匹配、exit 101；没有误将非零退出码改作失败或换输入。CoreMark 仅跑 20 次迭代，exit 0、CRC `0x4983`；不足 10 秒的提示完整保留，不作为正式运行成绩。所有纯 JIT App 的解释步数均为 0。

原子支持自然对齐；CAS128 的 libatomic fallback 只在 VM 完整 RW Anonymous 资格及 SMC 解保护完成后加锁，失配保留真实写周期。预期故障场景以明确错误停止，不能作为 Mixed 解释重放。SMC 场景检查真实 cache invalidation，不只检查 exit。局部跳转回归另外断言 spill 在 taken／untaken 两条路径可读、汇合后不能错误沿用 PC 常量。

完整内核最终输入、最终 ELF、动态库及 SHA 绑定见 `rv-final-freeze.json`／`rv-kernel-inputs.json`，原始控制台为 `rv-kernel-console.log.gz`。当前 user-QEMU 10.0.13 没有实现 `process_vm_readv/writev`，初次 runtime engine 在该环境下的假 fault 记录保留于外部结果；生产 safe-access 未放宽，最终在已有 RV64 Linux 内核中验证。user-QEMU 的 RW/RX 刷新补丁仍沿用既有版本。仿真结果不代表物理 RV64 性能或多 hart 硬件证明。

红魔上以提交前 `fb9c8e1f` runner 为对照，CoreMark20 每 scope 各 3 组交替样本，均默认 JIT／相同 CPU affinity：

| compile 中位数，包含 decode／发布／释放 | 提交前 | 当前 | 当前／提交前 |
| --- | ---: | ---: | ---: |
| Block | 42.855 ms | 42.395 ms | 0.9893 |
| Function | 51.847 ms | 51.293 ms | 0.9893 |

样本只用于判断是否存在明显结构回退，不据此宣称提速或更新 FEX 成绩。原始样本、runner／guest SHA、设备状态与命令保存在外部结果目录；汇总见 `arm64-compile-summary.json`。Function native bytes 保持 377392；Block 为 369928／370240，保留全部路径／地址样本差异，不把这点波动作为质量收益。

`source-manifest.json`、`build-and-vendor-manifest.json` 记录最终源和 Biscuit word-sink 补丁身份。完整日志、可复现小型 guest、压缩源码和 initrd 都保存在 `verification.json` 所列 Data2 路径。三个内核测试阶段只使用独立派生 initrd，旧输入未改；最终 QEMU 正常退出，派生展开目录已清空。红魔两轮测试 namespace 也已清空，无活动写入或清理错误，实际释放量见清理收据。

本次未接回旧 RVV／可选 ISA 加速、旧 AOT／磁盘缓存，以及不属于当前前端合同的 legacy IR 形态；不以源码入口清点代替完整指令形式覆盖。
