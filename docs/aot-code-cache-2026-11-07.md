# AOT 与磁盘 code cache 恢复

本轮让当前 ARM64／RV64 编译流程支持跨进程复用 native code，并恢复按符号名调用 guest 函数的接口。采用旧版「同一份 native unit 支撑 AOT 与磁盘缓存」的原则，编译仍走现有 CompileService，不恢复旧 IR／HIR、固定 dispatcher 槽位或旧 AddressSpace。

## 代码组织与职责

| 位置 | 职责 |
| --- | --- |
| `base/hash.{h,cpp}` | 流式 SHA256 与摘要文本；仅持久化入口调用 |
| `backend/relocation.h` | 稳定 HostSymbol、RelocationKind 和 16B relocation row |
| `backend/{arm64,riscv64}/relocate.{h,cpp}` | 在实际地址出生处录制；加载时解析本进程的 helper／dispatcher 地址，修补已记录槽位 |
| `execution/code_image.{h,cpp}` | 架构无关的 native unit 数据格式；inline words 和外置 `.svmaot.text` 共享元数据 |
| `execution/code_identity.cpp`、`cmake/CodeBuildId.cmake` | guest 内容、编译器／源码、配置和 native ABI 的兼容性 key |
| `execution/disk_cache.{h,cpp}` | 文件中的 canonical unit 集合及 PC／FP domain 查询；不拥有可执行映射 |
| `execution/code_persistence.cpp` | VM 的源码依赖核验、结果捕获、AOT 安装、SMC 后禁用存储与最终 flush |
| `execution/offline_compile.{h,cpp}` | 离线候选入口及静态后继 sweep；调用现有 decode／优化／后端／Publish |
| `platform/linux/artifact_file.{h,cpp}` | 有界文件读取、私有 cache 目录和同目录临时文件原子替换 |
| `aot/elf.{h,cpp}` | 本功能所需的 ELF64 读取结构；不承担 guest 映射或执行 |
| `aot/artifact.{h,cpp}` | 自带 guest 的 host ELF 容器、符号重写、alias／IFUNC／stub 与元数据核验 |
| `aot/compile.{h,cpp}` | 符号候选选择及离线编译 API |
| `aot/abi_sysv.h`、`guest_ptr.h`、`guest_call.{h,cpp}` | SysV 参数／返回值分类、GuestFn、独立调用栈和 guest buffer |
| `aot/call.{h,cpp}` | 将命名调用接入同一个 VM；保留 globals、解析 IFUNC、报告 guest fault |
| `aot/startup.cpp` | 在编译前按符号定位 executable PT_LOAD 字节，生成带明确 startup stop 的 guest 副本 |
| `launcher/linux/aot_main.cpp` | `compile`／`run`／`info`／整数便利 `call` 命令 |

文件按持久化边界和所有权拆分，不新增 IR pass、IR 副本或独立执行缓存。CodeHeap 仍独占 native mappings，CodeCache 仍独占 ready entry／link membership；AOT 与磁盘加载都先经现有发布入口。

## 原始发码与跨进程重绑定

默认 JIT 不录制 relocation，不收集 source spans，不读取磁盘或计算内容摘要。仅启用 cache／离线 AOT 时记录实际使用的外部地址。ARM64 使用固定 MOVZ／MOVK 槽或真正的 DATA64，RV64 使用单地址的 AUIPC／LD literal。普通 guest immediate 不参与地址猜测。

持久化记录使用语义 HostSymbol，不保存宿主可执行文件偏移；runner、离线工具和调用测试可分别链接、在各自 ASLR 地址上解析。未知自定义 callback 保留正常 JIT 能力，但其结果不可持久化。

保存的是后端完成发码、尚未被动态链接和 fault 修补改写的 canonical words。FaultSite 在 Publish 转移给 CodeHeap 后，从真实发布描述符捕获。加载顺序为复制 words、重定位、初始化链接、同步 I-cache、设置执行权限、注册 ready entries。保存过的 native 地址与旧进程的动态局部缓存不复用。

## Key、源码依赖和失效

兼容性 key 包含完整 guest ELF 的 SHA256、构建源码／工具链内容标识、实际 host ISA／能力、代码生成选项、状态／dispatch ABI、请求 scope、预算和 load bias。argv／environment／日志配置不改变 key；`decode_timing` 会改变 counted dispatch，必须纳入。

每个 unit 保存实际 decode 过程中收集的各段 `(PC,size,digest,SourceMode)`。Function 的不连续源码区间分别记录；Block 回边裁剪前已消费的字节也记录。不通过 `entry + decoded_bytes` 猜测地址范围，不额外 decode 或遍历 IR。

磁盘 key miss、坏文件、源码／policy 不匹配和可选存储 I/O 失败回退正常 JIT。显式指定的不兼容 AOT 在执行前报错。真正的可执行映射、权限或 I-cache 同步失败仍报告运行时错误。原始 guest 的错误退出码不会被误当作 host failure。

发生 guest 代码改写／SMC 时沿现有机制失效 native code，并禁用当前 VM 后续的磁盘读取和写回，避免从文件复活旧代码。保留磁盘中的原始未修改图像，供下一次 fresh guest 使用。SourceMode 校验保留 Immutable 与 WatchedBlock 的差别；可写代码不借旧 Function 资格跨块复用。

磁盘文件在 VM 结束时一次写回，通过 `0600` 同目录临时文件、完整写入、fsync 和 rename 发布；不逐 unit fsync 或重写整个文件。并发进程最后一次写入可覆盖另一进程新增的缓存命中机会，不影响 guest 语义。文件当前以 256 MiB 为上限，不自动选择全局目录或后台清理其他用户数据。

当前 JSON 中 `compile_ns` 包含 cache 读取、hash、重绑定与发布；最终存储写回单列为 `cache_write_ns`。比较首次持久化总成本时需将两者相加，完整 `run_lifetime_ns` 已覆盖这两部分。暖缓存 `compile_attempts=0` 不意味着零核验、发布或 I/O。

## AOT 的运行与调用

`svm_aot compile guest -o output.aot --scope function` 默认收集 guest entry 与函数符号，并沿真实静态 LinkSite 和 decode 产生的调用／服务 continuation 扩展候选。`--no-symbols`、`--no-sweep`、`--entry`、`--max` 可限制初始候选；`--max` 不限制 sweep 总量。存在软件 DAZ／FTZ specialization 时分别生成所需 FP domain。

产物是 host ELF，包含一份完整 `.svmaot.guest`、一份 `.svmaot.text` 和 `.svmaot.info`。原 guest sections 保留，函数符号改写到真实可调用入口；未编译函数指向显式失败 stub，objects 保留 guest VA，IFUNC 保留 resolver 含义。读取时从嵌入的原始 guest 独立推导并核验 rewritten symbol table。

```sh
./svm_aot compile guest.elf -o guest.aot --scope function
./svm_aot info guest.aot
./svm_aot run --aot guest.aot --scope function
./svm_rebuild_linux --aot guest.aot --scope function
./svm_aot call guest.aot sum7 1 2 3 4 5 6 7
./svm_aot patch guest.elf main -o guest-call.elf
./svm_aot compile guest-call.elf -o guest-call.aot --scope function
./svm_aot call guest-call.aot strlen --startup main 4194304
```

产物自带 guest，不要求原文件仍存在。`run` 可省略 guest 路径；若提供参数，首项作为 argv0。没有 AOT 覆盖的合法目标沿同一 JIT 路径编译，计数明确反映新增编译。显式 artifact 的 scope、优化和 diagnostic 配置需要匹配；Function artifact 的 CLI／C++ 命名调用自动采用产物 scope。

```cpp
swift::aot::AotGuestEnv env;
std::string error;
if (!env.Init("guest.aot", error)) swift::ThrowError(error);
auto sum = env.Lookup<std::uint64_t(std::uint64_t, std::uint64_t)>("sum");
auto result = sum(20, 22);
auto release = env.Close();
```

CLI `call` 提供最多 8 个整数参数；完整 C++ GuestFn 支持独立 GPR／SSE 参数银行、溢出栈参数、FP、aggregate／sret、varargs 和 GuestPtr。guest pointer 不等同于任意 host pointer；需要通过 ScopedGuestBuffer 与 VM memory service 建立可访问的 guest buffer。

调用经当前 guest entry／dispatcher 执行，返回地址为独立的不可执行 sentinel；不会将符号中的 host code VA 强转成普通 C 函数调用。每次调用恢复调用前的架构寄存器，保留 guest 全局数据与映射；IFUNC resolver 也通过 GuestFn 执行并缓存选择结果。

依赖 libc／constructors 初始化的调用先使用显式 startup 流程：`PatchGuestStopByte(input,"main",patched,error)` 生成副本，再从该副本 compile artifact；`Init` 后调用 `env.RunStartupUntil("main",error)`。guest 从正常 ELF entry 与初始 Linux stack 执行，停在已预设的 F4 函数入口之前，之后的命名调用保留初始化后的 globals／TLS／映射。stop 函数的首字节保留为 F4，不继续调用其原实现。该 stop 的 source hash 已包含在产物中，不修改已校验的 guest bytes，不赋予普通 guest HLT 新语义；`patch_stop=true` 在 AOT 环境明确报错，要求先生成并编译副本。上例的字符串地址需按实际 guest 映射选择，C++ 调用可使用 ScopedGuestBuffer 传入字符串。

## 边界

- 首期持久化仅支持 static x86-64 ET_EXEC。静态 PIE 的普通 JIT 行为保留；当前 guest PC／RIP 常量没有完整重定位 provenance，不能把不同 load bias 的代码假装为可复用。
- 保持一 guest thread 和现有 guest==host memory 合同，不宣称完整多 VM 地址空间、动态 loader 或 guest signal 支持。
- 构建 key 采取保守失效；源码、编译器、host 能力或发码配置变化后需要重编译 artifact／cache。
- 磁盘命中是跳过 decode／优化／分配／发码，仍需内容核验、发布、重绑定和链接。compile 统计包括这些实际成本，不能将 `compile_attempts == 0` 表述为零加载成本。

测试、源码／二进制身份和 compile 回归数据见 [验证记录](evidence/aot-cache-20261107/README.md)。
