# 后端职责与诊断基础设施整理

基线为 `7c10a7dd`，依赖提交 `99d13b39`，实现提交 `0a037933`。实现位于 `/Volumes/Data2/SwiftVM-submit-20261104`；冻结的 `/Volumes/Data2/SwiftVM` 仅作为旧库与设计参考，没有修改。本批没有新增优化 pass、IR 副本或逐节点决策表。默认仍为全部 JIT，Block / Function 分配入口保留。

## 文件与职责

```text
source/base/
  format.h                         类型安全的格式化入口
  error.h error.cpp                RuntimeError、固定Failure记录、errno交付
  logging.h logging.cpp            level/channel、可注入sink、stderr/Android输出
source/runtime/
  exit.h exit.inc                  C++与汇编共用的退出/原子资格窄合同
  ir/format.h format.cpp            独立可选formatter，直接读Program/CellRange
  backend/
    allocation_cfg.h .cpp          共用RPO与回边闭包数学
    arm64/
      allocate.h .cpp              入口、定义坐标、实际register/spill决策
      allocation_detail.h          私有借用上下文、块内状态和热谓词
      allocation_selection.cpp     原反向扫描中的use/pin/NZCV/PC选择
      emit.h emit_ops.inc          单Writer及171项静态typed Handle映射
      emit_scalar.cpp              标量选码，短driver与职责helper
      emit_calls.cpp               宽除法、CPU查询和x87调用族
      entry_elf.S                  ELF原生Root/Cold/Exit
      helper_bridge.S              明确的helper保存/恢复与返回约定
      runtime.h runtime.cpp        编译绑定/容量复用、寄存器/flags/FP恢复
    riscv64/
      allocate.h .cpp              独立GPR/pair/spill策略，借用共用CFG
      entry_elf.S                  ELF/LP64D原生入口
      runtime.h runtime.cpp        RV编译绑定与状态恢复
  platform/linux/
    arm64_host.h .cpp              HWCAP、ucontext、权限操作
    riscv64_host.h .cpp            ucontext、跨hart I-cache syscall
    signal_time.h                 信号路径的clock_gettime窄计时
source/launcher/linux/
  main.cpp                         接入、日志和进程退出
  options.h .cpp                   参数与帮助
  report.h .cpp                    既有83项统计JSON字段，键值相邻
source/tests/
  CMakeLists.txt                   测试目标和一致的CTest属性
  externals/catch/                 复用Catch2 3.2.1及许可证
  support/                        共享runner、薄fixture与native发布支持
  ir/format_test.cpp               formatter合同
  rebuild/arm64_*_test.cpp         scalar/vector/link/fault职责分组
```

`backend/*/runtime.cpp` 是执行集成层，编入 `svm_execution`；纯发码库不引入 Linux 头。`entry_elf.S` 按架构及宿主 ABI 选择，不能直接用于 Mach-O。ARM helper bridge 保留 Apple 符号适配及 `SVM_X87_GPR_ONLY`，保存的寄存器集合不变。

## 分配与发码

ARM 的 674 行 Allocate 大函数拆为初始化、一次反向选择和最终分配；`allocate.cpp` 为246行。Home仍16B，ARM三次/RV四次实际node walk保持。Block与无图Function不构建CFG；共用部分直接读真实terminal，不建CFG镜像。反向选择仍按PC观察、输入uses、NZCV、pending/pin的顺序执行，块内数组没有新增全量补零。

原426行ALU family拆成短入口与flags-only、窄比较、logical/arithmetic immediate、register fallback等helper。保留原成功条件、cursor消费及提交顺序；不建立试发码回滚。171项Handle映射集中到私有表，IR opcode/schema仍只有原声明源。

私有发码实现使用成对的 `#define __ masm.exact.` / `#undef __`。它只缩写exact encoder，不接回MacroAssembler、scratch allocator、pool或第二码流。关键逐节点小函数保留内联机会，Release IPO继续覆盖实际编译库。

## 错误、日志与执行边界

生产源码和benchmark中裸 `std::runtime_error` / `snprintf` 已清除；只有基础异常包装层继承标准异常。已有 BuildError / IR Error / CompileError 保留，CompileDiagnostic保存一次失败请求的事实，最后失败时才格式化；成功Function→Block回退不先构造诊断字符串。

noexcept桥通过固定Failure记录保存类别、文本与原始errno；信号路径仅保存固定操作名/errno，在普通C++边界生成系统错误文本。已格式化异常的what不重复渲染。复用记录只清状态，不在每次冷resolve重置整个文本数组。

Logger先过滤再求值格式化参数，支持level、channel与替换sink。日志格式化失败不抛出，直接写入与格式化入口均保留调用者errno。统计JSON独立于Logger，原字段、类型和可选性保留。

guest退出值与宿主失败分开：guest正常exit101仍是成功运行；guest先exit0但Close失败时，launcher返回125，报告保留guest值。ARM宽除法通过实际status返回除零/溢出，不再从C++ helper发SIGFPE/abort。原生故障停止后不会解释重放。

平台负责mprotect与进程级I-cache同步；后端负责具体指令修补。同步冷回调返回0或保存的errno，失败候选尚未发布。RV的FP缓存域策略显式绑定，避免用A64 AFP能力判断是否需要重复专门化。

## formatter与测试

`svm_ir_format` 由 `SVM_BUILD_IR_FORMAT` 控制，默认随 `BUILD_TESTING`。核心IR头不引入fmt，普通JIT无新增dump扫描。formatter显示natural type/consumer view、宽立即数、嵌套参数和Lambda child、完整flags metadata、entry与terminal；Void view按实际定义解析，而非显示成void。名称由中立opcode表生成，错误诊断共用该名称接口。

Catch2使用与消费者一致的C++20编译，测试依赖由BUILD_TESTING隔离。原152个命名场景保留；现有数值预期没有因迁移而放宽。成功Check保持原零分配行为，失败交给Catch报告，故Catch断言数不等于旧Check调用次数。ARM原5077行测试拆为四组与共享fixture，测试目录暂保留rebuild。

默认测试入口为compile/direct合同。`--native`显式选择真实原生执行，不支持的ISA返回77并由CTest标为跳过。共同native publisher保留真实W^X、映射寿命及RV跨hart刷新；没有用解释结果或返回零填充native结果。共享支持层不拥有另一份生产VM/cache。

## 验证与compile成本

三套构建通过：macOS ARM64、Android arm64-v8a/API28、RV64G/LP64D Linux。227个自有C++文件格式检查、Git空白检查通过。

- host：13项有效合同通过，6项native按ISA跳过；formatter输入及natural-view显示在新测试定位后修复并定点复测通过。
- 红魔NX729J（`bf1359ff`）：11个测试目标通过，其中6个使用显式native入口；另外B/F × JIT/Mixed除零停止、Func B/F七行oracle及exit101、缺失ELF/JSON错误出口通过，共18项。
- RV：一次双模拟hart、完整Linux内核中16/16通过，包含12个RV native Catch组、Mixed热转换、Func/Core20、SMC及访问/除零故障。不是物理RV性能测量，也不沿用旧23次native execution计数。

红魔使用改动前缓存runner与当前runner、相同CoreMark20输入、CPU4 affinity，各scope三组交替样本。下表compile包含decode、发布与释放，未打开扰动性的decode timing：

| scope | 改动前中位数 | 当前中位数 | 当前/改动前 |
| --- | ---: | ---: | ---: |
| Block | 41.4187 ms | 41.9671 ms | 1.0132 |
| Function | 51.6915 ms | 51.8474 ms | 1.0030 |

这轮没有明显compile劣化，也不据这点波动宣称提速。CPU4频率上限前后均1171200kHz，thermal_zone0原值27749/27744。初次读policy4失败的样本不作为最终锚点；最终改用CPU4自身cpufreq路径并要求前后一致。

host的scalar B/F与Function region automatic/provided-decoded四组中位数变化为+0.4%～+0.6%；相同实际IR工作、native words/spill/frame保持，region的机器码mix也相同。它们是合成结构检查，不代替应用/FEX成绩。

新增显式除法故障路径会增加发码。CoreMark观察到Block native bytes从369928到370288/370384，Function从377392到378008；不宣称所有应用机器码逐字不变。本轮没有完整runtime/PMU或FEX重测，旧正式对比保留原身份。

全部源/输入/二进制身份、结果与清理收据见[本轮证据](evidence/backend-maintenance-20261107/README.md)。手机两处namespace均已清空，无活动写入；QEMU退出，无展开目录。构建继续复用Data2固定缓存，较大数据不写系统tmp。
