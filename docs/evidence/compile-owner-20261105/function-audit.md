# Function 提交覆盖审计

本次只有一次 CoreMark20 诊断运行。`runtime_completed=true`、exit 0、`crcfinal=0x4983`；不足十秒时的 `Errors detected` 属预期，最初额外要求长跑 validation 文本的 host 断言未造成重跑。约 600ms 的带日志编译时间不进入 QoR。

诊断派生生产快照为 **842e4748e3263f6f50a28c53e1b8bdbb410bde70ae754940610ce43ed7eaed9b**，来自 `../buffers-publish/source-manifest.json`；比较基线为 fp-instruction final 的 **83d2985d2573a929ce4bbeca1ca611045ad563bdf68c3362cf72274ec5af70fe**。两者是容量复用/发布改动前后不同源码，下面只核实际工作量一致，不声称时间或机器字身份相同。

## 总数闭合

705 attempts = 700 Function attempts + 5 Block fallback attempts。成功发布为 **695 Function + 5 Block = 700**，decode/optimize/backend 错误均 0，5 个错误全部发生在前端发射。无缓存失效，所有 705 次 `fp_mode=0`，不存在跨 FP 模式的合理复制。

原统计全部闭合：guest instructions **32159**、guest bytes **136270**、IR nodes **87595**、arena words **153728**。其中：

| 统计范围 | 指令出生次数 | 字节出现次数 | 唯一指令 PC | 字节并集 |
|---|---:|---:|---:|---:|
| 695 个成功 Function | 31912 | 135397 | 16023 | 61880 |
| 加上 5 个成功 Block fallback | **31990** | **135659** | **16101** | **62142** |
| 5 个失败前缀，计入原 guest 统计 | 169 | 611 | 不作成功覆盖 | 不作成功覆盖 |

成功 unit 内没有重复指令 PC。跨 unit 的重复编译覆盖为 **15889 次指令出现、73517 次字节出现**；精确相同 slice 的重复出现为 **3803**。Block fallback 未输出 formation 行，但其单块连续 accepted 区间由 begin PC + guest bytes 得到，262B/78 条指令均是成功 Function 并集之外的新增覆盖。

计数方法：读取唯一诊断的实际 `[pc,end)`/count，使用固定 ELF 字节离线 Capstone 解码，所有范围的指令数、末端及重复 PC 的长度均完全匹配。没有重建 IR、执行 guest 或重新运行设备。Block 基线 12407 是总出现次数而非唯一 PC 集，不能把 Function 的全部重复量都当作两 scope 的可删除差额。

## 重复在哪里

主要是不同请求入口重新形成相交子图。例：`__vfprintf [0x25ce1e,0x25cf3c)` 的 54 条指令出现 14 次，多出 702 次；`[0x25d37f,0x25d466)` 的 50 条出现 14 次，多出 650 次。它们都是冷编译覆盖计数，不是运行时热点执行次数。

18 次后来请求的入口此前已作为旧 unit 的内部指令出现，且这 18 个新 unit 的整个 guest 覆盖均已出现过，其输入 IR 共 3590 节点。例如 seq315 从 `__vfprintf` 内 `0x25c1d1` 开始的 897 条指令，已全部在 seq314 中覆盖。这证明重复形成存在，但不同入口的 frame/home/flags 协议仍不同，不能直接跳进旧内部 label。

仅当目标是**实际已 ready 的合法请求入口**时，观测到 19 个 B 起点、直接覆盖 502 条指令；没有模拟其下游剪枝。现有 CodeCache 只登记请求入口，FEX 的形成也不查询旧 cache；FEX 的 CALL continuation 多入口有单独入口初始化。故本轮不增加 ready 截断回调或内部标签发布，不将 54% 字节重叠冒充可无损消除的工作。进一步去重需要独立设计多入口 ABI。

## 五个失败

均为 DecodeFunction 成功后发射 Unsupported；后面同 PC 的 Block fallback 全部成功。

| attempt / entry | 拒绝 PC / 指令 | 完整形成指令数 | 已发射前缀 | 已出生 IR | fallback 指令数 |
|---|---|---:|---:|---:|---:|
| 70 / 0x2699ff | 0x269acd XGETBV | 108 | 67 | 82 | 10 |
| 72 / 0x269a29 | 0x269acd XGETBV | 92 | 51 | 58 | 50 |
| 74 / 0x269adb | 0x269b01 XGETBV | 36 | 11 | 26 | 10 |
| 642 / 0x25f3b3 | 0x25f47f SHRD eax,r13d,cl | 126 | 21 | 44 | 2 |
| 644 / 0x25f3bc | 同上 | 111 | 19 | 41 | 6 |

合计失败 formation 473 条，169 条已发射、304 条尚未发射：**32463 decode calls = 31990 成功指令 + 473 失败形成指令**；**32159 原 guest 统计 = 31990 + 169**。失败前缀出生 **251 IR 节点**被原 `ir_nodes` 明确排除，因此全部实际前端出生为 **87846**；失败 arena words 没有 O(1) 读口，保留 null，未调用 CountWork。这部分重试不是 19752 条 scope 总差的主体，也不是 50944 个节点差的主体。

## 真实容量高水位

| 保留 owner | 实测最大字节 | 生产上限 |
|---|---:|---:|
| IR owner（包含 Storage/三 vector 容量） | 159856 | 8MiB |
| native words | 20512 | 4MiB |
| links | 1024 | 64KiB |

最大同时保留合计 **181392B**，最后一个 publish 也处于该值，均未触及上限。未将 CodeArena 长期保留的 native/fault 数据计入这些可复用工作缓冲。

结构化结果为 `analysis.json`；`unit-coverage.jsonl` 给出每个成功 `(pc,mode)` 的覆盖量和先前覆盖关系，原始 B 范围保留在唯一 stderr。`analyze.py` 可从这些不可变输入复算。本次解析未修改生产，未运行构建或操作设备；先前诊断派生构建单独记录于 `receipt.json`。
