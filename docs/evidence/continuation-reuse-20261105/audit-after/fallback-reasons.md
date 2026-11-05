# Stage1 回退原因审计

唯一诊断运行exit0、CRC4983，无重跑，时间不作QoR。与普通单观测存在工作量小差异：普通1333 attempts/802 Function/531 fallback；本诊断1339/805/534，不混为同一逐项计数。

全部534次失败都是形成成功后的前端Unsupported；formation、优化、backend失败均0。805成功发布=271 Function+534 Block fallback。

| 指令 / diStorm ID | 失败 | 已出生前缀N | 已发射前缀指令 | 非root源B | 位于该B首条 |
|---|---:|---:|---:|---:|---:|
| INT3 / 360 | 521 | 249954 | 81688 | 521 | 521 |
| XGETBV / 1787 | 3 | 173 | 129 | 3 | 0 |
| BTR / 912 | 4 | 545 | 174 | 4 | 0 |
| SHRD / 892 | 6 | 1269 | 449 | 6 | 0 |

原ir_nodes=48065不含失败前缀251941个节点，实际前端出生合计 **300006**。原guest=98267中失败前缀占82440条，成功unit仅15827条。单看成功IR变少会隐藏主要回退工作。失败词数无O(1)读口，仍为null。

源B归属直接由本次formation记录的[pc,end)与error_pc匹配；不通过猜测函数符号或CALL返回来分类。INT3全部在非root源B，且恰是该B第一条。它们对应额外续点形成进入trap/padding的现象；本审计未把每个前驱CALL都推断为noreturn。

拟议multi-entry限定的非request-PC FC_INT frontier能使这些trap不拖垮此前合法unit，同时真实以trap为root的请求仍须拒绝/服务；不能把非法指令一概吞掉，也无需增加Builder指令回滚或维护ISA支持表。SHRD/BTR/XGETBV保持现拒绝合同，未在本审计中扩ISA。

结构化JSON保存每次失败的root、errorPC、source B、prefix大小及后续成功Block retry。没有新建诊断runner、生产修改、构建或ADB操作。
