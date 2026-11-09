# ARM32 前端实现与验收（2026-11-09）

已按[接入方案](arm32-frontend-plan-2026-11-09.md)实现默认 ARMv7-A 小端用户态范围。代码基线为 `2f7ae360`，译码模式与指令语义参考固定的 `dynarmic-interp 5a76f6930935bc07a2aa5158f3ee515c34ca14fd`，保留其 0BSD 版权与来源说明。

## 实现范围与边界

| 部分 | 实际落地 |
| --- | --- |
| 前端 | A32、Thumb16、Thumb-2；条件码、IT、PC bias/interworking、整数/DSP、scalar/multiple/双字访存、用户态状态与屏障 |
| 数值 | S/D/Q 共用存储；VFP、NEON、QC、FPSCR、比较 flags；舍入、DN/FZ、AHP、异常位累计；区分 VMLA 与 VFMA |
| 两种提交 | 同一 `Semantics<Sink>` 供 IR 与 Direct 使用；默认 JIT；Mixed 按完整位置键统计热度并切换 |
| 后端 | 沿用 ARM64、x64、RV64、LoongArch64 的选指令、分配、发码和恢复；GPR 读写仍是 Uniform IR |
| Linux | ELF32、有界低地址分配、32 位栈/auxv、EABI 编号及结构体转换、TLS、kuser v5 |
| 信号 | 普通 752 B 帧/`sigreturn=119`、880 B RT 帧/`rt_sigreturn=173`、VFP 记录、默认/自带 restorer、Thumb SVC restart |
| 所有权 | 原有 dispatch/L1/L2、异常恢复、SMC、CodeHeap/GuestMemory owner；完整 key 用于入口，物理地址用于 source/mapping |
| 持久化 | CodeImage v4；guest ISA/Profile/location version；ELF32 guest 嵌入 host ELF64 AOT；数值叶子使用稳定重定位符号 |

默认 Profile 为 v7，启用 VFP/NEON、FP16 转换、VFPv4 和整数 divide。ARMv8 CRC/crypto 默认关闭。预留的 v3–v8 配置和部分版本门控不代表所有历史版本的最低指令合法性已经分别验收；本轮对外确认的是上述 v7 用户态 Profile。

方案原有边界保持：不提供特权/银行寄存器/MMU/Jazelle、大端/SETEND 切换、PT_INTERP 或完整 Android linker/bionic 环境。FPSCR 浮点异常使能位在写入边界明确拒绝，未宣告浮点 trap 协议。exclusive 保持现有 private-anonymous/单 guest 线程及外部写者边界。高级 `Lookup<Sig>` 的 AAPCS32 分类未纳入本轮；已有 ARM32 typed context 和原始寄存器调用入口。

## 框架与文件职责

完整文件树已同步至[方案第 7 节](arm32-frontend-plan-2026-11-09.md#7-实装文件组织)。主要入口为：

- `source/runtime/frontend/arm32/{decode,region,build_ir,interpreter}.{h,cpp}`：译码、Function 形成、提交和直接执行。
- `semantics_{integer,multiply,dsp,shift_flags,registers,memory,control,system,vfp,neon_integer,neon_float,neon_memory}.inc`：按语义职责组织实现。生成的 switch 只提取参数并调用对应成员。
- `source/runtime/arch/arm32/{profile,location,state,service}.h`：翻译身份和架构状态；PC/flags/Q 偏移为 128/136/144，state 448 B，NativeRunContext 仍为 1072 B。
- `numeric.{h,cpp}`：窄数学叶子和 FPSCR；`numeric_callbacks.inc`、`numeric_registry.*`：地址与持久化数学 ID 的对应。复用现有 `arch/arm64/fp_*` 的纯数学入口，不传入 A32 编码或复制整份 A64 state。
- `platform/linux/{abi_arm32,syscalls_arm32,signal_frame_arm32,arm32_user_helpers}*`：Linux ABI 边界，复用共用 syscall operation、队列、内存与原子 owner。

`Block` 直接流式 decode→提交，不保存一份中间 instruction 向量；`Function` 在有界区域内缓存一次译码。直接执行不构造 Program、不缓存模板解释块。没有增加新的全 IR pass、每指令 CPSR 快照、运行时 matcher 容器或整指令 helper。

位置键保留 T/E/IT/FPSCR 模式；SVC 只在自身边界保存原 `service_pc`，restart 同时保留原位置与精确后继。普通指令没有新增历史记录。FMA 使用 state 尾部的 8 B 数值 addend 槽，未扩大 state。

kuser 页安装内容来自实际 guest 汇编。生成器读取 ELF 符号的 `.type/.size` 生成 span，防止安装表与函数长度分离；32/64 位 CAS、TLS 和屏障均经过实际调用。

## 指令覆盖与验证口径

| 模式表 | 模式行数 |
| --- | ---: |
| ARM | 261 |
| Thumb16 | 79 |
| Thumb32 | 239 |
| VFP | 62 |
| NEON | 147 |
| 合计 | 788 |

这些模式对应 759 个去重语义入口，均参与 IRSink/DirectSink 实例化。生成表可由 `tools/arm32/generate_decode.py` 重建，重复生成无字节变化。模式数不等于独立指令数，也不等于逐编码测试数量；Profile 开关、操作数组合合法性与目标后端能力仍分别处理。

四个永久测试源为 `arm32_frontend_test.cpp`、`arm32_semantics_test.cpp`、`arm32_numeric_test.cpp`、`arm32_runtime_test.cpp`。集中覆盖谓词副作用、IT、移位 0/32/>32/RRX、ADC/SBC、跨页取指、变长拆块、寄存器别名、FP 状态、native/Direct 一致性、两类信号帧及 restart。数值叶子另有独立数学预期。

| 实际执行环境 | 前端/数值 portable | 生成代码 native | runtime portable | runtime native | kuser ELF |
| --- | ---: | ---: | ---: | ---: | --- |
| 红魔 NX729J / ARM64 | 18 cases | 2 cases | 2 cases | 3 cases | Block/Function × 三种 engine |
| x64 Linux kernel / QEMU system | 18 | 2 | 2 | 3 | Function JIT 通过 |
| RV64 Linux kernel / QEMU system | 18 | 2 | 2 | 3 | Function JIT 通过 |
| LoongArch64 Linux kernel / QEMU system | 18 | 2 | 2 | 3 | Function JIT 通过 |

跨后端为功能验证，不作为真机性能结论。RV 可选 Zb/RVV 和 LA LSX/LASX 关闭；x64 ASLR 为 mode 1，RV/LA 为 mode 2。构建、打包与运行的二进制 SHA256 均有绑定。

额外保留了同一份 C 程序的 ARM 与 Thumb ELF，由独立 QEMU ARM 执行得到 `1ef52f03\n`。红魔上的两种 scope × JIT/Direct/Mixed 均输出相同；kuser 独立预期为 `arm32 kuser ok\n`。未假定红魔能直接运行原生 32 位 Linux ELF。

红魔共用 runtime 原有回归同时通过：11 个 portable cases、23 个 native cases及 4 个 persistence cases。主机的 ARM32、ARM64、code image、AOT portable targets 通过；格式检查和 `git diff --check` 通过。

## 磁盘缓存与 AOT

- Thumb 混合程序的 Block/Function 冷启动均写入缓存；热启动分别命中 17/10 个 unit，`compile_attempts=0`。
- ARM 与 Thumb ELF32 均可产生 Function AOT，并在移走原 guest 文件后依靠嵌入 guest 运行，输出正确；两次均加载 48 个 unit，`compile_attempts=0`。
- VFP/NEON 数值叶子的持久化记录使用稳定数学 ID，未把跨进程函数地址原样缓存。四后端 smoke 检查对应重定位；ELF32 Thumb 符号的解析、host ELF64 符号重写与完整 key source 投影有专门回归。

## Compile 与生成质量观察

红魔 CPU4，频率上限 1.1712 GHz，前后上下限未变化。基线为任务开始前保留的 launcher，候选为当前实现；两者使用同一个 x86-64 CoreMark ELF、相同参数/环境、scope、optimize 与诊断计时，三轮交错运行。以下是三轮中位数，`compile_ns` 维持原有端点，包含实际 byte decode、发布及回收等既定成本；这不是全机冷启动或 PMU cycles 口径。

| x86 CoreMark | 原 launcher | 当前实现 | 变化 |
| --- | ---: | ---: | ---: |
| Block compile | 12.544 ms | 12.616 ms | +0.58% |
| Function compile | 11.935 ms | 12.100 ms | +1.38% |
| Block 扣实际 byte decode | 11.362 ms | 11.476 ms | +1.01% |
| Function 扣实际 byte decode | 10.735 ms | 10.941 ms | +1.92% |

对应生成观察保持相同：Block 为 87,032 native bytes / 2 spill slots，Function 为 70,604 native bytes / 3 spill slots；提交的 guest 指令数也分别保持 2,838 / 3,150。该小批对照未观察到明显 compile 回退，不能据此宣布所有 workload 均无回退。

新 ARM/Thumb 混合小程序的 Function JIT 观察分别为 1.989/2.164 ms，生成 9,920/9,828 B、spill 为 0；属于固定小输入的单点成本，不是完整 benchmark 排名。完整分解、反例/中途失败及原始结果保存在 Data2，本仓库保留最终摘要。

**ARM32 与参考 A32 JIT 的同口径 compile 倍数尚未测量。** 方案中同数量级、Function 不超过 2× 的目标保留，未改写为已达标；不能以 FEX 的 x86 输入或参考模板解释块构建时间代替这项验证。

## 使用和复现

```sh
# 默认全部 JIT；Block/Function 的分配和形成范围分别保留。
svm_rebuild_linux --scope function arm32-static.elf
svm_rebuild_linux --engine interpreter arm32-static.elf
svm_rebuild_linux --engine mixed --hot-threshold 8 arm32-static.elf
svm_rebuild_linux --cache-dir ./native-cache arm32-static.elf
svm_aot compile arm32-static.elf -o arm32.aot --scope function
svm_aot run --aot arm32.aot --scope function

# 普通构建使用已检入的表；更新模式后离线重生成。
python3 tools/arm32/generate_decode.py
# helper 生成：obj 输出放在 Data2 固定构建目录。
clang --target=armv7-linux-gnueabi -c \
  source/runtime/platform/linux/arm32_user_helpers.S -o "$task_build/arm32-user-helpers.o"
python3 tools/arm32/generate_user_helpers.py "$task_build/arm32-user-helpers.o"
```

[最终证据 manifest](evidence/arm32-frontend-implementation-20261109/manifest.json)包含二进制/源码绑定、独立 oracle 命令及原始结果路径；[手机摘要](evidence/arm32-frontend-implementation-20261109/phone-summary.json)和[跨后端摘要](evidence/arm32-frontend-implementation-20261109/cross-summary.json)记录实际结果。构建缓存复用 Data2 目录，临时 kernel initrd 已回收，未把大量产物写回系统盘。
