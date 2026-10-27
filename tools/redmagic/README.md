# 红魔 PMU 对照

此目录实现[重构方案](../../docs/middle-pipeline-rebuild-plan-2026-10-21.md)
14.6 的设备采样与主机比较入口。设备固定为 NX729J、adb 序列号
`bf1359ff`，需要 root、CPU 4 online，以及已核对的 simpleperf。
它用于长期复用的性能验收，不修改 SVM/FEX，不插入 guest probe。

## 采样范围

[pmu-point.sh](pmu-point.sh) 每次运行一个新 launcher 进程：

```text
pmu-point.sh NEW_SAMPLE_DIR SIMPLEPERF EXPECTED_OUTPUT LAUNCHER GUEST [ARG...]
```

采样目录必须是新建私有目录下的 `samples/<id>`，父目录由调用方准备：
`/data/local/tmp/svm-middle-pmu-<batch>/samples/`。已有观察（包括失败）不能覆盖。
通过 `adb -s bf1359ff shell` 在 root 下以 `env -i PATH=/system/bin ...`
启动脚本，显式传入本批配置，避免继承诊断变量。
CoreMark 的 EXPECTED_OUTPUT 使用 `0x4983` 或 `0xd340`；现有 bench-suite
使用 `sha256:<摘要>`，对完整 stdout 字节（包括换行）检查 SHA-256。
两类检查均在 PMU 结束后执行，保存独立 target/profiler 退出码。

SVM 使用 `SVM_FUNC_BASE=1|0` 区分 Function/独立 Block，
`SVM_JIT_CACHE=`，`SVM_AVX=0 SVM_BMI=0 SVM_XSAVE=0`；
两种模式中基线/候选的其余环境必须相同。
FEX 的固定构建见 [../fex-android](../fex-android/README.md)。
设置私有 data/config/socket/tmp、`FEX_ROOTFS=/`、`FEX_HOSTFEATURES=disableavx`，
并设置 `FEX_PORTABLE=1`，隔离全局配置；显式关闭
`FEX_CACHEOBJECTCODECOMPILATION`、`FEX_AOTIRLOAD/CAPTURE/GENERATE` 和
`FEX_BLOCKJITNAMING/LIBRARYJITNAMING/GLOBALJITNAMING`。
配置文件、launcher、FEXServer、guest、脚本和 simpleperf 均登记哈希。

实际命令先将 simpleperf 绑到 CPU 4，再创建被测任务：

```sh
/system/bin/taskset 10 "$simpleperf" stat --csv --verbose --cpu 4 \
  --group cpu-cycles,instructions -o "$sample/pmu.csv" \
  /system/bin/sh -c "$wrapper" pmu-observation "$sample" "$child_wrapper" "$launcher" "$guest" \
  0x0 0x0 0x66 "$iterations" 7 1 2000
```

mask `10` 为十六进制，仅 CPU 4。父 wrapper 输出计数目标 PID，启动并等待
子 wrapper；子 wrapper 输出实际 PID/affinity 后 exec launcher。父 wrapper
独立写入 target-exit-code，脚本另存 profiler-exit-code。已观察到本设备
simpleperf 在 launcher 因 Aborted 终止时仍返回零，因此两种状态都必须检查。
两层 shell/查询开销在三条臂中均计入 PMU；此口径与早期单层 wrapper 分开采集。
taskset/profiler 自身的启动以及 adb 往返在 PMU 目标之外。
没有 user/kernel 过滤，保留 simpleperf 默认的线程/子进程继承。
instructions/cycles 放在同一组；只把每个原始计数器
`time running == time_enabled > 0` 的观察纳入有效批次。
普通 CSV 汇总不提供这些时间，不能由缺少百分比推定有效率 100%。
格式依据为 [AOSP cmd_stat.cpp](https://android.googlesource.com/platform/system/extras/+/52dfde3251129578413a54e67f723efe2799961b/simpleperf/cmd_stat.cpp)，
实际格式还须由本批冻结的 simpleperf 核对。

脚本保存 stdout/stderr、目标与 profiler 的 NUL 分隔 argv、环境、两种退出码、
前后输入哈希、UTC/uptime、fingerprint、CPU governor/频率上下限与当前值、
热区 31/35/41 的类型及温度、电池状态/温度/电量和 USB online。
这三处热区必须分别为 cpuss-0、cpu-1-0、cpu-1-6，否则拒绝执行。
不会改变性能模式、频率、散热、充电设置，也不会停止任何服务。

## 批次与服务

主批至少 36 点：两种 SVM 模式 × 基线/候选/FEX × 三轮 × 2000/12000。
轮转三条臂的先后次序，交错两端点；完整记录预定顺序，不事后丢弃慢样本。
每个端点 CRC 必须分别是 `0x4983`、`0xd340`。
短迭代不足十秒是已知限制，结果不作为官方 CoreMark 跑分。
同模式内两端点配对计算每轮斜率/截距，再报告中位数、范围与 MAD。
两点截距包含 loader、dispatcher、运行时、编译及 wrapper，不能叫纯编译成本。

主口径显式预启动同一私有 FEXServer，launcher/JIT 冷、服务已就绪；
Android 文件页缓存不清空，不把它称作整机冷启动。服务不在 launcher 的进程树内，
用另一组相同 PMU 事件单独采集服务整个寿命，原样报告启动、请求、空闲及关闭的
聚合计数；不能除以点数后冒充逐点服务成本。
另以每点全新 data/socket/tmp、不预启动服务的 FEX 运行保存端到端辅助观察，
默认继承覆盖 launcher 活跃期间自动启动的服务。launcher 退出后的服务尾部和
管理关闭在该观察范围外，必须注明。不要混入主三方比值。
使用相同私有环境执行 FEXServer -k，并等待自己的服务结束；不使用 killall。

## 主机比较

```sh
python3 tools/redmagic/compare.py /path/to/batch/manifest.json \
  --output /path/to/batch/comparison.json
```

manifest 的文件路径相对于它自身，绝对路径也可。字段为：

| 字段 | 内容 |
| --- | --- |
| schema / serial / repetitions | `1` / `bf1359ff` / 至少 `3` |
| arms | baseline/candidate/fex，各含 device_path、sha256 |
| guest / simpleperf / point_script | 各含 device_path、sha256 |
| artifacts | 每项含主机 path、sha256；部署文件另含 device_path。包含构建/源码与原始日志的 provenance |
| identity_before / identity_after | 本批所有部署路径到 SHA-256 的映射，与 artifacts 一致 |
| samples | 每项含 id、arm、mode（function/block）、round（从 0 起）、iterations、directory、device_directory、environment |
| service | 聚合服务 scope 的文字说明；pmu_path 指向单独服务 CSV。启动/关闭命令、PID、就绪与退出证据另存 artifacts |
| cold_service_samples | 可选辅助观察，使用同一 sample 字段，arm 为 fex；不参加主矩阵统计 |

### 分类负载

同一工具也支持已有的 `source/translator/linux/tests/bench_suite_x86_64`。
每个 kernel 独立一批，保持完整的两种模式 × 三臂 × 至少三轮 × 两端点，
以及相同的绑核、原始 PMU、产物身份和服务统计范围。不要混合不同 kernel
计算斜率，分别报告 `int`、`fp`（SSE2）、`mem`、`branch`、`call`。
这些样例本身不能证明高寄存器压力、任意 helper 或完整恢复路径已覆盖。

manifest 增加如下 workload；省略时仍表示上述固定 CoreMark 协议：

```json
{
  "workload": {
    "kind": "bench-suite",
    "kernel": "int",
    "points": [
      {"scale": 1, "stdout_path": "oracle/int-1.txt", "stdout_sha256": "<64 lowercase hex digits>"},
      {"scale": 2, "stdout_path": "oracle/int-2.txt", "stdout_sha256": "<64 lowercase hex digits>"}
    ]
  }
}
```

两个 scale 必须是不同正整数，比较器据此生成严格的 `<kernel> <scale>` argv。
每个 sample 的 `iterations` 字段在此表示 scale，斜率以 scale 差为分母。
stdout 文件必须登记在 artifacts；由现有 `bench_suite_native.c` 在真实
x86-64 或 macOS x86-64/Rosetta 下执行产生，保存源码、编译命令、oracle
二进制、stdout、stderr 和退出码。scale=1 还应核对仓库既有 golden；
禁止从 SVM/FEX 输出反向生成期望值。设备采样保存 expected-stdout-sha256，
主机再次检查完整输出、两类退出码和所有原始计数。

同一静态 guest 不使用 CPUID 分派，只能支持该固定工作负载的比较；
它不证明两个引擎所有可见 ISA 能力相等。分类批次同样不自动通过正式 FEX 门。

原始目录使用 adb pull 取回；先核对完整部署的前后哈希，再运行比较器。
它检查 argv、环境、CRC、退出码、PID/affinity、原始 PMU 与汇总的一致性及
完整主矩阵。硬件 fingerprint、governor、频率上下限、充电和 USB 条件改变时，
整批标为无效；当前频率与温度的变化保留用于判断波动，不挑样本补齐。
所有观察和负截距均保留，IPC 使用同组原始计数；斜率 IPC 另列。
`measurement_valid` 只表示主批采样协议通过，服务/辅助观察另列结果。
脚本始终把 formal_FEX_gate_passed/full_refactor_complete 留为 false：
固定输入的 CRC 和编译条目映射不能证明所有 guest CPUID 能力相等，
本工具也不覆盖完整正确性矩阵或其他负载的性能门。

## 编译统计离线解码

[compile_stats.py](compile_stats.py) 读取既有统计，输出一份带输入 SHA-256 的 JSON：

```sh
python3 tools/redmagic/compile_stats.py svm /path/to/svm-stderr.txt --output svm-stats.json
python3 tools/redmagic/compile_stats.py fex /path/to/fex-stats.raw \
  --clock-hz <CNTFRQ_HZ> --clock-source '<频率证据路径:行号及摘要>' --output fex-stats.json
```

SVM 只解析一份完整的 `[svm-prof]` coarse 汇总；多个进程的 dump 必须先分开。
`translate_ns` 包含查询、等待、编译、发布和收尾；`translate_wait_ns` 只覆盖
配置与编译范围锁。PROF2 行单列忽略数量，不相加嵌套计时，也不据日志推定运行环境。
单独测compile粗计时时只传`SVM_PROF=1`，完全不传`SVM_PROF2`；当前两个
开关按变量是否存在启用，`SVM_PROF2=0`仍会开启细粒度记录。普通PMU对照
不传这两个变量。compile粗计时点与上文无诊断的完整PMU矩阵分别保存和判断。

FEX 对应固定 `a156752f` 的 v2 ABI：64 字节头、48 字节线程槽和旧零 padding。
严格核对版本、Size/文件长度、全部 Head/Next 的范围与对齐，以及 Head 链循环。
全部物理槽分别保存在 `active`（TID 非零）与 `residual`（TID 为零，含空槽）中；
退出会摘链，复用会清零，不能把 residual 自动相加当进程累计。
`active`只是ABI字段分类，不证明对应OS线程仍存活，也不证明从未发生槽复用。
调用方负责保留 live 文件 FD、PID/starttime、线程复用及产物身份的证据。

`--clock-hz` 和来源必须显式提供；使用既有 metrics 输出的 `CNTFRQ_EL0`，或固定
FEX guest CPUID leaf 0x15 的 ECX，不能使用 CPU 当前频率或 PMU cpu-cycles。
JIT 与 Signal 分开输出原始 ticks 及向下取整的换算 ns。FEX 的 JIT 范围是完整
`CompileBlock` 服务，含锁、L3 命中和发布，未包含更早的线程/compiler 初始化；
它与 SVM coarse 范围仍有差别，解码结果不自动作性能或完整性判定。
