#!/usr/bin/env python3
"""Render the fixed n=5 post-push anchor; never collect or replace a sample."""
from __future__ import annotations

import argparse
import json
from pathlib import Path


CASES = ("coremark", "int", "fp", "mem", "branch", "call", "func_tests", "x87_bench")
ARMS = ("block", "function", "fex")
SUBJECT = "390cf73d7d8db5687a8d33cc8be5cdd6aa38638f"
CODE = "7f1f403a9c429c664914867439ef6315ac9f3e3b"
SOURCE = "a78fdec20b3de2f00fe47bb1dbfa67c6664c0efa7cd1b7f30658edd8f1ce1e52"
STAGES = (
    ("formation_and_ir", "A：形成＋IR出生（扣纯byte parser）"),
    ("ir_optimization", "B：IR优化"),
    ("allocation_and_encoding", "C：分配＋编码"),
    ("transfer_and_publication", "复制／链接／发布"),
    ("release", "当前释放窗口"),
    ("other", "其余独占成本"),
)
RESOURCES = (
    ("compile_attempts", "全部编译尝试"),
    ("function_attempts", "Function尝试"),
    ("block_attempts", "Block尝试"),
    ("fallbacks", "Function→Block回退"),
    ("code_units_published", "发布的完整unit owner"),
    ("emitted_guest_instructions", "已发射guest指令出现（含拒绝前缀）"),
    ("emitted_guest_bytes", "已发射guest字节出现（含拒绝前缀）"),
    ("ir_nodes", "成功IR节点（不含拒绝前缀）"),
    ("operand_words", "成功operand words（不含拒绝前缀）"),
    ("native_bytes", "已发布native bytes"),
    ("spill_slots", "累计spill slots"),
    ("max_frame_bytes", "最大unit frame bytes"),
    ("peak_code_virtual_bytes", "code arena峰值虚拟bytes（含双别名）"),
    ("peak_cache_entries", "峰值ready PC keys"),
    ("peak_cache_occupied", "峰值occupied PC keys"),
    ("peak_cache_slots", "峰值目录slots"),
    ("cache_table_allocations", "目录分配次数"),
    ("fault_sites", "实际FaultSite数"),
    ("link_relocation_rows", "link relocation rows"),
    ("link_relocation_capacity", "link relocation容量"),
    ("cache_invalidations", "缓存失效次数"),
    ("unaligned_recoveries", "非对齐冷恢复次数"),
    ("unaligned_patches", "非对齐补丁次数"),
)


def load(root: Path, name: str):
    return json.loads((root / name).read_text())


def number(value, places=6):
    return "—" if value is None else f"{value:.{places}f}"


def integer(value):
    return "—" if value is None else f"{value:,}"


def table(headers, rows):
    return "\n".join(
        ["| " + " | ".join(headers) + " |", "| " + " | ".join("---" for _ in headers) + " |"]
        + ["| " + " | ".join(str(x) for x in row) + " |" for row in rows]
    )


def policy(root, name):
    path = root / name
    if not path.exists():
        return ["未记录"] * 4
    values = path.read_text().splitlines()
    return (values + ["未记录"] * 4)[:4]


def scene_value(rows, key):
    values = [row["record"][key] for row in rows if key in row["record"]]
    if not values:
        return "—"
    return integer(min(values)) if min(values) == max(values) else f"{integer(min(values))}–{integer(max(values))}"


def render(root: Path):
    plan = load(root, "plan.json")
    identity = load(root, "identity.json")
    inputs = load(root, "inputs.json")
    summary = load(root, "summary.json")
    breakdown = load(root, "compile-breakdown.json")
    runtime_records = load(root, "runtime-records.json")
    compile_records = load(root, "compile-records.json")
    smoke = load(root, "smoke-summary.json")
    modes = load(root, "mode-probe-summary.json")
    acceptance = summary["acceptance"]
    runtime = summary["runtime"]
    compile_ = summary["compile"]

    # Missing results cause a clean stop; this script neither polls nor emits
    # a partial report that could be confused with the registered full anchor.
    assert plan["subject_commit"] == identity["subject_commit"] == SUBJECT
    assert identity["code_commit"] == CODE and identity["source_digest"] == SOURCE
    assert plan["repetitions_per_case_arm"] == acceptance["n"] == 5
    expected = {(case, arm, rep) for case in CASES for arm in ARMS for rep in range(5)}
    for name, records in (("runtime", runtime_records), ("compile", compile_records)):
        assert len(records) == 120, f"{name}: expected 120 records, found {len(records)}"
        assert {(r["case"], r["arm"], r["round"]) for r in records} == expected
    assert acceptance["gated_checks"] == 48
    for case in CASES:
        for arm in ARMS:
            assert runtime[case][arm]["n"] == compile_[case][arm]["n"] == 5

    limits = acceptance["limits"]
    passed = acceptance["passed_checks"]
    failures = acceptance["failures"]
    valid_runtime = sum(bool(r["valid"]) for r in runtime_records)
    valid_compile = sum(bool(r["valid"]) for r in compile_records)
    conclusion = (
        "本轮固定输入的48项性能门槛全部通过。"
        if passed == 48
        else f"本轮固定输入通过{passed}/48项性能门槛，尚有{48 - passed}项未通过，不能宣称性能目标全部完成。"
    )
    lines = [
        "# 提交后全benchmark五轮锚定结果（2026-11-05）",
        "",
        f"{conclusion} 本次以已推送subject `{SUBJECT}` 为唯一候选，每项Block／Function／固定FEX各五次，共120条普通运行记录和120条独立编译记录；功能有效性分别为{valid_runtime}/120与{valid_compile}/120。所有预先登记样本均保留，未按门槛追加、替换或与旧结果混池。",
        "",
        "这是对已提交实现的差距锚定。本文中的‘通过’只覆盖固定八项输入、两种scope和本次设备条件；不代表完整x86 ISA、动态ELF或完整异常／信号兼容。",
        "",
        "## 1. 提交、输入与固定口径",
        "",
        table(["身份", "值"], [
            ("测量subject／已核远端提交", f"`{SUBJECT}`"),
            ("代码／测试提交", f"`{CODE}`"),
            ("分支", f"`{plan['remote_ref']}`"),
            ("118-file source digest", f"`{SOURCE}`"),
            ("candidate runner SHA256", f"`{inputs['candidate']['sha256']}`"),
            ("固定FEX revision", f"`{plan['FEX_revision']}`"),
            ("普通FEXLoader SHA256", f"`{inputs['FEXLoader']['sha256']}`"),
            ("byte-probe FEXLoader SHA256", f"`{inputs['FEXLoader-byte']['sha256']}`"),
            ("FEXServer SHA256", f"`{inputs['FEXServer']['sha256']}`"),
            ("FEX Config.json SHA256", f"`{inputs['fex-config/Config.json']['sha256']}`"),
        ]),
        "",
        "source digest是路径＋文件内容SHA的清单摘要，不是Git commit。subject包含已提交的说明与历史证据；本轮结果文档会在测量后另行提交，本文不虚构该未来commit。原始身份见[plan.json](plan.json)、[identity.json](identity.json)、[source-manifest.json](source-manifest.json)及[inputs.json](inputs.json)。",
        "",
        table(["固定guest ELF", "SHA256"], [
            (key, f"`{inputs[key]['sha256']}`")
            for key in ("coremark", "bench-suite", "func-tests", "x87-bench")
        ]),
        "",
        f"设备 `{plan['device']}`，CPU {plan['cpu']}，`taskset {plan['taskset']}`。策略min/max为{plan['min_khz']:,}/{plan['max_khz']:,} kHz；不修改频率策略或系统服务，不能称锁频。实测前后记录如下，温度列为原始sysfs整数：",
        "",
        table(["窗口", "min kHz", "max kHz", "current kHz", "temperature raw"], [
            (label, *policy(root, name)) for label, name in (
                ("compile前", "compile-policy-before.stdout"),
                ("compile后", "compile-policy-after.stdout"),
                ("runtime前", "runtime-policy-before.stdout"),
                ("runtime后", "runtime-policy-after.stdout"),
            )
        ]),
        "",
        "每样本启动新launcher，无AOT/object-code缓存，保留OS page cache；三arm按预先固定顺序循环。FEX维持同一Config、AVX关闭和TSO设置，未为本轮改变分母。普通运行与decode-timing编译分开启动；前者不使用诊断编译耗时作为运行成绩。用户态PMU包括同一child树的启动、冷JIT及wrapper；独立FEXServer位于该树外，未计入。",
        "",
        "CoreMark运行参数为`0 0 0 100000 7 1 2000`，CRC d340且loop计时须至少10秒；该行time使用guest loop ticks。其余七项使用whole-child simpleperf elapsed，包含启动和JIT，不能与CoreMark时间窗口混为统一评分。编译CoreMark只跑20迭代、CRC4983，短时官方validity warning按计划保留，不作为官方CoreMark分数。其他guest参数和checksum／exit oracle见[cases.json](cases.json)；func_tests保原stdout及exit101，x87_bench继承exit-only oracle，exit0不证明完整数值语义。",
        "",
        "## 2. 运行时间：各五次中位数",
        "",
        "单位ms；ratio为本scope中位数／同轮固定FEX五次中位数，越小越好。不取逐轮ratio的中位数，也不合并工作负载平均分。",
        "",
    ]
    lines.append(table(["输入", "Block ms", "Function ms", "FEX ms", "B/FEX", "F/FEX"], [
        (case, number(runtime[case]["block"]["seconds"] * 1000, 3),
         number(runtime[case]["function"]["seconds"] * 1000, 3),
         number(runtime[case]["fex"]["seconds"] * 1000, 3),
         number(runtime[case]["block"]["seconds"] / runtime[case]["fex"]["seconds"]),
         number(runtime[case]["function"]["seconds"] / runtime[case]["fex"]["seconds"]))
        for case in CASES
    ]))
    lines += [
        "", "## 3. 用户态PMU差距", "",
        "下表分别对cycles和instructions取五次中位数后相除。instructions和branch-misses用于解释差距，不新增为性能合格门，也不能仅凭计数变化给某条优化作因果归因。",
        "",
        table(["输入", "cycles B/FEX", "cycles F/FEX", "instructions B/FEX", "instructions F/FEX"], [
            (case, *[number(runtime[case][arm][metric] / runtime[case]["fex"][metric])
                     for metric in ("cycles", "instructions") for arm in ("block", "function")])
            for case in CASES
        ]),
        "",
        table(["输入", "Block branch-misses", "Function branch-misses", "FEX branch-misses"], [
            (case, *[integer(runtime[case][arm]["branch_misses"]) for arm in ARMS]) for case in CASES
        ]),
        "",
        "普通运行的全部原始计数、时间和stdout/oracle资格保存在[runtime-records.json](runtime-records.json)。cold JIT占比随输入时长不同；此表不是纯steady-state JIT指令质量实验，不能把整段指令差直接当作可删除指令。",
        "",
        "## 4. 编译总成本：只扣实际byte parser", "",
        "单位ms。SVM用compile总窗口减实际byte-parser子区间；FEX用闭合CompileBlock树减其实际byte_decode区间。保留形成、IR出生、失败前缀／Function回退、B、分配与编码、发布及当前释放窗口。decode-timing／byte probe会扰动编译，故只能作同协议的总预算比较；不能把整个前端或formation都扣掉。",
        "",
        table(["输入", "Block ms", "Function ms", "FEX ms", "B/FEX", "F/FEX"], [
            (case, *[number(compile_[case][arm]["ms"]) for arm in ARMS],
             number(compile_[case]["block"]["ms"] / compile_[case]["fex"]["ms"]),
             number(compile_[case]["function"]["ms"] / compile_[case]["fex"]["ms"])) for case in CASES
        ]),
        "",
        "FEX旧probe的logical工作buffer释放在窗口内，但最终native arena销毁不在该释放probe内；SVM当前code-owner释放计入总成本。这一owner-release不对称保留，未从候选扣除或给FEX补猜时长。组件规划预算及旧synthetic结果不能替代这里按scope独立的总编译≤2×资格。原始记录见[compile-records.json](compile-records.json)。",
        "",
        "## 5. 全部48项门槛的实际真值", "",
        f"每输入×scope检查time≤{limits['seconds']}×、user cycles≤{limits['cycles']}×、compile去byte≤{limits['compile']}×，共8×2×3＝48项。判定使用未四舍五入数值；表面显示值仅便于阅读。原始样本有效与性能门通过是两项不同事实。",
        "",
    ]
    gate_rows = []
    for case in CASES:
        for arm in ("block", "function"):
            ratios = (
                runtime[case][arm]["seconds"] / runtime[case]["fex"]["seconds"],
                runtime[case][arm]["cycles"] / runtime[case]["fex"]["cycles"],
                compile_[case][arm]["ms"] / compile_[case]["fex"]["ms"],
            )
            thresholds = (limits["seconds"], limits["cycles"], limits["compile"])
            gate_rows.append((case, arm, *[f"{number(v)} {'通过' if v <= cap else '**失败**'}"
                                           for v, cap in zip(ratios, thresholds)]))
    lines.append(table(["输入", "scope", "time ratio /真值", "cycles ratio /真值", "compile ratio /真值"], gate_rows))
    lines += ["", f"本轮结论：**{passed}/48**。"]
    if failures:
        lines += ["", "未过项（完整保留，不用旧轮次、额外复测或较好样本替换）：", "",
                  table(["输入", "scope", "指标", "实际ratio", "门槛"], [
                      (f["case"], f["arm"], f["metric"], number(f["ratio"]), number(f["limit"]))
                      for f in failures
                  ])]
    else:
        lines += ["", "没有未过项；这仍是预先固定五次的本轮锚定，不将一次通过外推为所有输入或环境的稳定上界。"]
    lines += [
        "",
        "未重跑凑门槛、未可选停止、未删除失败样本、未把Block与Function合并判定。完整判定见[summary.json](summary.json)的acceptance；原n3、A1／A2及x87七轮配对复核都只保留为历史，不混入本轮120＋120数据。",
        "",
        "## 6. CoreMark编译阶段与实际资源", "",
        "阶段表各arm只选择该arm总编译去byte排序的第3个实际样本（五次中位样本），所有分项来自同一次记录；不是独立分项中位数拼接。各列可闭合到该arm中位总值，单位ms。",
        "",
    ]
    core_stages = breakdown["cases"]["coremark"]
    stage_rows = []
    for key, label in STAGES:
        values = []
        for arm in ARMS:
            actual = ("logical_work_buffer_release" if arm == "fex" else "native_owner_release") if key == "release" else key
            values.append(number(core_stages[arm]["exclusive_ms"][actual]))
        stage_rows.append((label, *values))
    stage_rows.append(("总编译去byte", *[number(core_stages[arm]["total_ms"]) for arm in ARMS]))
    stage_rows.append(("所选round（零基）", *[core_stages[arm]["round"] for arm in ARMS]))
    stage_rows.append(("分项闭合误差ms", *[number(core_stages[arm]["closure_error_ms"], 9) for arm in ARMS]))
    lines.append(table(["阶段", "Block", "Function", "FEX"], stage_rows))
    lines += [
        "",
        "FEX分项由remaining_ticks独占归约：形成＝generate_ir/raw_decode/builder；B＝pass_manager/x87/dfe/validation/other_pass；C＝ra/backend/emit；发布＝tail_copy_cache/publish；释放＝logical_release。byte_decode单独排除；不把嵌套inclusive计时相加。SVM的C仍为原后端分配＋编码窗口，没有另做纯RA实验。所有八项的同样分解保存在[compile-breakdown.json](compile-breakdown.json)。",
        "",
        "资源表取本轮CoreMark20编译诊断的实际record；同一scope五次不同时显示最小–最大范围，不选择最有利值。SVM成功IR数明确不包含拒绝前缀，emitted guest数包含其实际已发射前缀，因此不能把前者称完整出生账。",
        "",
    ]
    core_records = {arm: [r for r in compile_records if r["case"] == "coremark" and r["arm"] == arm]
                    for arm in ("block", "function")}
    lines.append(table(["SVM实际资源", "Block", "Function"], [
        (label, *[scene_value(core_records[arm], key) for arm in ("block", "function")])
        for key, label in RESOURCES
    ]))
    selected_fex = next(r for r in compile_records if r["prefix"] == core_stages["fex"]["prefix"])["record"]
    lines += [
        "",
        f"该FEX代表样本CompileBlock root calls＝{integer(selected_fex.get('root_calls'))}。此root计数不是SVM的ready keys或完整unit owner数，原probe没有同口径资源字段时保留缺失，不补造FEX native bytes／spill。SVM多个外部PC共享一个unit；ready keys、owner数与动态访问次数不可互换。目录slot32B、NativeCode40B，CoreMark该scope若已列出多个ready key不表示复制native owner。",
        "",
        "dispatches／cache_hits只在诊断编译模式可用，静态链接绕过这些计数；本轮未另跑全入口census或采样，也不把它们当所有IR B的访问次数。",
        "",
        "## 7. 同source历史验证与当前范围", "",
        "本轮118-file digest与runner均未改变，因此没有重新做coverage trace。此前同source的[唯一覆盖审计](../continuation-reuse-20261105/audit-accepted-candidate/report.md)是历史结构验证：成功指令重复15889→191（−98.80%），全部实际出生87846→50856（−42.11%）；2757个额外入口＋681个root共享681个完整owner，逐entry核对为非空原始source B，非empty/stub。该独立CoreMark20诊断不是本轮重新测得的覆盖率，也不参与时间／cycles分母。唯一覆盖比OLD多123条及剩余191次重复仍保留，未将全部工作量差归因单一优化。",
        "",
        "A1的CALL后推测进入INT3、A2的LOCK共享入口／XEND等回退以及原n3的47/48和独立x87七轮复核均已归档。当前实现保留已成功形成source-B入口与stop-ready，取消CALL后推测队列、continuations和FC_INT特例；没有为了缩小编译成本新增ISA预扫、局部checkpoint回滚或overlap框架。历史结果不能与本轮中位数混池，旧拒绝候选也不能充当新锚定的通过证明。",
        "",
        f"本轮既有Android backend／pipeline＋八输入双scope smoke为{sum(bool(x['valid']) for x in smoke)}/{len(smoke)}；模式／SMC probe为{sum(bool(x['valid']) for x in modes)}/{len(modes)}。这些功能检查与性能记录分开，未为此次锚定新增测试矩阵。x87 exit-only、数值FP不自动累计MXCSR sticky／不新增IEEE traps、固定ELF／单执行线程等既定边界保持；不将这些限制藏在‘全benchmark’措辞中。",
        "",
        "## 8. 复查入口与关闭回执", "",
        "复查统计只需现成文件，无需再跑设备：先由本目录analyze.py归约完整记录，再运行`python3 render-report.py`。测量命令、顺序和参数见[run.py](run.py)、[commands.json](commands.json)、[plan.json](plan.json)；全部原stdout／stderr／PMU由record的prefix定位。本报告生成器只读取本目录，不修改源码、Git或设备，不触发新的测试、样本或计时。",
        "",
    ]
    closure = summary["source_closure"]
    cleanup = summary["phone_cleanup"]
    lines.append(table(["关闭／身份核对", "实际结果"], [
        ("current HEAD", f"`{closure['current_head']}`"),
        ("重新计算source digest", f"`{closure['digest_recomputed']}`"),
        ("manifest文件数", closure["files"]),
        ("current源码差异数", len(closure["current_changed"])),
        ("frozen源码差异数", len(closure["frozen_changed"])),
        ("自有设备namespace", f"`{cleanup['namespace']}`"),
        ("删除的自有allocated bytes", integer(cleanup["allocated_bytes_removed"])),
        ("df实际空闲增量bytes", integer(cleanup["df_actual_free_delta_bytes"])),
        ("自有namespace剩余bytes", integer(cleanup["remaining_bytes"])),
        ("残留active processes /open files /errors", f"{len(cleanup['active_processes'])} /{len(cleanup['open_files'])} /{len(cleanup['errors'])}"),
    ]))
    lines += [
        "",
        "allocated删除量与df实际空闲增量是不同观测，不相加成回收量。关闭仅涵盖本轮自有namespace；其他应用／owner未纳入。源closure、部署输入前后身份及cleanup资格由[summary.json](summary.json)保留完整真值；详细回执见[phone-cleanup-receipt.json](phone-cleanup-receipt.json)。",
        "",
        f"本轮归档判定为{passed}/48；后续是否继续缩小差距以这份固定锚定为基准。记录有效性、性能门槛与既定支持范围分别陈述，不用其中一项替代另一项。",
        "",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent)
    parser.add_argument("--output", type=Path, help="Defaults to ROOT/report-draft.md")
    args = parser.parse_args()
    root = args.root.resolve()
    document = render(root)
    output = args.output or root / "report-draft.md"
    output.write_text(document)
    print(output)


if __name__ == "__main__":
    main()
