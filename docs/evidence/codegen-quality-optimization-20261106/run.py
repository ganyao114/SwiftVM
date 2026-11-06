from pathlib import Path
import datetime, hashlib, json, re, shlex, statistics, subprocess, sys, time

REPETITIONS = 5
ARMS = ["baseline-block", "baseline-function", "block", "function", "fex"]
ORDERS = [ARMS[n:] + ARMS[:n] for n in range(REPETITIONS)]
P = Path(__file__).resolve().parent
R = Path("/Volumes/Data2/SwiftVM-submit-20261104")
T = Path("/Volumes/Data2/SwiftVM/source/translator/linux/tests")
ADB = ["/opt/homebrew/bin/adb", "-s", "bf1359ff"]
NS = "/data/local/tmp/swiftvm-codegen-quality-20261106"
Q = Path(
    "/Volumes/Data2/swiftvm-middle-preserved-20261025/reconstruction-20261102/qor-direct-emit-20261103"
)
env = json.loads((R / "docs/evidence/parity-batch37/environment.json").read_text())
env = {
    k: v.replace("/data/local/tmp/swiftvm-parity-batch37-20261103", NS)
    for k, v in env.items()
}
ENV = shlex.join(["env", "-i"] + [k + "=" + v for k, v in env.items()])
POLICY = "cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_min_freq /sys/devices/system/cpu/cpu4/cpufreq/scaling_max_freq /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq /sys/class/power_supply/battery/temp"
CASES = [
    {
        "name": "coremark",
        "guest": "coremark",
        "args": ["0", "0", "0", "100000", "7", "1", "2000"],
        "exit": 0,
    }
]
gold = {
    s.split()[0]: s.split()[1]
    for s in (T / "bench_suite_x86_64.native.txt").read_text().splitlines()
    if s and not s.startswith("#") and len(s.split()) == 2
}
for name in ["int", "fp", "mem", "branch", "call"]:
    CASES.append(
        {
            "name": name,
            "guest": "bench-suite",
            "args": [name, "1"],
            "exit": 0,
            "checksum": gold[name],
        }
    )
CASES += [
    {"name": "func_tests", "guest": "func-tests", "args": [], "exit": 101},
    {"name": "x87_bench", "guest": "x87-bench", "args": [], "exit": 0},
]


def save(name, value):
    target = P / name
    temporary = target.with_name(target.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    temporary.replace(target)


def run(label, command, expected=None, timeout=30):
    a = ADB + (["shell", command] if isinstance(command, str) else command)
    r = subprocess.run(a, capture_output=True, text=True, timeout=timeout)
    (P / (label + ".stdout")).write_text(r.stdout)
    (P / (label + ".stderr")).write_text(r.stderr)
    c = (
        json.loads((P / "commands.json").read_text())
        if (P / "commands.json").exists()
        else []
    )
    c.append({"label": label, "argv": a, "exit": r.returncode})
    save("commands.json", c)
    if expected is not None and r.returncode != expected:
        raise RuntimeError(label + ": " + r.stderr[-1000:])
    return r


def policy(label):
    r = run(label, POLICY, 0)
    vals = r.stdout.split()
    assert vals[:2] == ["614400", "1171200"], vals
    return vals


def app(arm, diagnostic=False):
    if arm == "fex":
        return (
            ENV
            + (" FEX_COMPILE_STAGE_PROBE=1 " if diagnostic else " ")
            + NS
            + ("/FEXLoader-byte" if diagnostic else "/FEXLoader")
        )
    return (
        "env -i PATH=/system/bin "
        + NS
        + ("/baseline" if arm.startswith("baseline-") else "/candidate")
        + (" --decode-timing" if diagnostic else "")
        + " --scope "
        + arm.removeprefix("baseline-")
    )


def check_output(case, out, code):
    if code != case["exit"]:
        return False, "exit " + str(code) + ", expected " + str(case["exit"])
    if case["name"] == "coremark":
        valid = (
            "Correct operation validated." in out
            and re.search(r"crcfinal\s*:\s*0xd340", out) is not None
        )
        return bool(valid), "CoreMark validation/CRC"
    if "checksum" in case:
        m = re.search(r"^" + case["name"] + r"\s+(0x[0-9a-f]+)\s*$", out, re.M)
        return bool(m and m[1] == case["checksum"]), "native checksum"
    if case["name"] == "func_tests":
        expected = "".join(
            line
            for line in (T / "func_tests_x86_64.native.txt")
            .read_text()
            .splitlines(keepends=True)
            if not line.startswith("exit=")
        )
        return out == expected, "native stdout and exit101"
    return True, "exit-only compatibility; no numerical oracle"


mode = sys.argv[1]
if mode in ("prepare", "refresh"):
    assert (
        subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=R, text=True).strip()
        == "ebf557408e14aef165b28bd7f0f3ebfc0ae3e918"
    )
    m = json.loads((P / "source-manifest.json").read_text())
    assert all(
        hashlib.sha256((R / x["path"]).read_bytes()).hexdigest() == x["sha256"]
        for x in m["files"]
    )
    if mode == "prepare":
        run("namespace-absent", "test ! -e " + NS, 0)
    else:
        own = run("namespace-idle", "ps -A -o PID,PPID,ARGS", 0).stdout
        assert not [
            line for line in own.splitlines() if NS in line and " sh -c " not in line
        ]
    run(
        "mkdir",
        "mkdir -p "
        + shlex.join(
            [NS, NS + "/samples", NS + "/fex-data", NS + "/fex-config", NS + "/fex-tmp"]
        ),
        0,
    )
    inputs = json.loads((R / "docs/evidence/parity-batch37/inputs.json").read_text())
    files = {
        k: Path(inputs[k]["path"])
        for k in ["FEXLoader", "FEXLoader-byte", "FEXServer", "simpleperf"]
    }
    files.update(
        {
            "baseline": P / "baseline-svm",
            "candidate": Path(
                "/Volumes/Data2/swiftvm-middle-build-cache/reconstruction/submit-android/svm_rebuild_linux"
            ),
            "coremark": Path(inputs["coremark-bionic"]["path"]),
            "bench-suite": T / "bench_suite_x86_64",
            "func-tests": T / "func_tests_x86_64",
            "x87-bench": T / "x87_bench_x86_64",
            "fex-config/Config.json": Q / "Config.json",
        }
    )
    (P / "launch.sh").write_text(
        '#!/system/bin/sh\nprefix=$1\nshift\ncat /proc/$$/status > "$prefix.wrapper-status"\n"$@" &\nbenchmark_pid=$!\nprintf "%s\\n" "$benchmark_pid" > "$prefix.pid"\nwait "$benchmark_pid"\nbenchmark_exit=$?\nprintf "%s\\n" "$benchmark_exit" > "$prefix.exit"\nexit "$benchmark_exit"\n'
    )
    files["launch.sh"] = P / "launch.sh"
    identities = {}
    for name, path in files.items():
        identities[name] = {
            "path": str(path),
            "bytes": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        }
        run("push-" + name.replace("/", "-"), ["push", str(path), NS + "/" + name], 0)
    run(
        "chmod",
        "chmod 700 " + shlex.join(NS + "/" + n for n in files if "/" not in n),
        0,
    )
    run(
        "remote-identities-before",
        "sha256sum " + shlex.join(NS + "/" + n for n in files),
        0,
    )
    run(
        "device-identity",
        "getprop ro.product.model; getprop ro.build.fingerprint; uname -a; cat /proc/sys/kernel/random/boot_id; df -k /data/local/tmp",
        0,
    )
    save("inputs.json", identities)
    save("environment.json", env)
    save("cases.json", CASES)
    save(
        "identity.json",
        {
            "base_commit": "ebf557408e14aef165b28bd7f0f3ebfc0ae3e918",
            "working_tree_modified": True,
            "source_digest": m["digest"],
            "device": "bf1359ff",
            "cpu": 4,
            "repetitions": REPETITIONS,
            "PMU": "Whole child user events, including cold JIT and same wrapper; FEXServer outside target tree is excluded. Fresh launcher each sample, no AOT/object cache, OS page cache retained.",
            "time": "CoreMark loop timer; other cases whole-child simpleperf elapsed including startup/JIT. No cross-workload score aggregation.",
        },
    )
    identity = json.loads((P / "identity.json").read_text())
    identity.update(
        subject_commit="ebf557408e14aef165b28bd7f0f3ebfc0ae3e918",
        code_commit="ea97d8b2b8edd86840c9f9dd4df81f7fc1ef590c",
        remote_commit_verified="ebf557408e14aef165b28bd7f0f3ebfc0ae3e918",
        measurement_plan_sha256=hashlib.sha256(
            (P / "plan.json").read_bytes()
        ).hexdigest(),
        no_optional_stopping=True,
        prior_samples_not_pooled=True,
    )
    save("identity.json", identity)
    policy("prepare-policy")
    print("prepared", len(files), "inputs", flush=True)
elif mode == "smoke":
    policy("smoke-policy-before")
    results = []
    for case in CASES:
        for arm in ARMS:
            args = (
                case["args"]
                if case["name"] != "coremark"
                else ["0", "0", "0", "20", "7", "1", "2000"]
            )
            label = "smoke-" + case["name"] + "-" + arm
            rr = run(
                label,
                "taskset 10 timeout -k 2 120 "
                + app(arm)
                + " "
                + shlex.join([NS + "/" + case["guest"]] + args),
                timeout=130,
            )
            good = (
                (rr.returncode == 0 and "crcfinal      : 0x4983" in rr.stdout)
                if case["name"] == "coremark"
                else check_output(case, rr.stdout, rr.returncode)[0]
            )
            results.append(
                {
                    "case": case["name"],
                    "arm": arm,
                    "exit": rr.returncode,
                    "valid": good,
                    "prefix": label,
                }
            )
            save("smoke-summary.json", results)
            print(label, rr.returncode, good, flush=True)
    save("smoke-summary.json", results)
    policy("smoke-policy-after")
elif mode in ("runtime", "runtime-resume"):
    rows = (
        json.loads((P / "runtime-records.json").read_text())
        if mode == "runtime-resume"
        else []
    )
    if mode == "runtime-resume":
        (P / "runtime-records-before-disk-resume.json").write_bytes(
            (P / "runtime-records.json").read_bytes()
        )
        for row in rows:
            if row["case"] == "func_tests":
                case = next(c for c in CASES if c["name"] == row["case"])
                valid, reason = check_output(
                    case, (P / (row["prefix"] + ".stdout")).read_text(), row["exit"]
                )
                if row["valid"] != valid:
                    row["initial_harness_valid"] = row["valid"]
                    row["qualification_note"] = (
                        "Native golden exit=101 line is shell metadata, checked against target exit, not guest stdout."
                    )
                row["valid"] = valid
                row["check"] = reason
    failed = {(r["case"], r["arm"]) for r in rows if not r["valid"]}
    done = {(r["case"], r["arm"], r["round"]) for r in rows}
    policy(
        "runtime-policy-before" if mode == "runtime" else "runtime-resume-policy-before"
    )
    for rep, order in enumerate(ORDERS):
        policy("runtime-policy-round-" + str(rep) + "-before")
        for case in CASES:
            for arm in order:
                if (case["name"], arm, rep) in done:
                    continue
                label = f"runtime-{case['name']}-{arm}-{rep}"
                remote = NS + "/samples/" + label
                args = shlex.join([NS + "/" + case["guest"]] + case["args"])
                command = f"{NS}/simpleperf stat -o {remote}.pmu -e cpu-cycles:u,instructions:u,branch-misses:u -- taskset 10 timeout -k 2 120 sh {NS}/launch.sh {remote} {app(arm)} {args}"
                start = time.monotonic()
                proc = subprocess.Popen(
                    ADB + ["shell", command],
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
                time.sleep(0.15)
                live = run(
                    label + "-live",
                    f'p=$(cat {remote}.pid 2>/dev/null); if [ -n "$p" ] && [ -d /proc/$p ]; then readlink /proc/$p/exe; cat /proc/$p/status; fi',
                )
                out, err = proc.communicate(timeout=135)
                (P / (label + ".stdout")).write_text(out)
                (P / (label + ".stderr")).write_text(err)
                pmu = run(label + "-pmu", "cat " + remote + ".pmu", 0).stdout
                meta = run(
                    label + "-meta",
                    "cat "
                    + remote
                    + ".exit "
                    + remote
                    + ".pid "
                    + remote
                    + ".wrapper-status",
                    0,
                ).stdout
                actual_exit = int(meta.splitlines()[0])
                allowed = re.search(r"^Cpus_allowed_list:\s*(.+)$", meta, re.M)[
                    1
                ].strip()
                assert allowed == "4"
                if "Cpus_allowed_list:" in live.stdout:
                    assert (
                        re.search(r"^Cpus_allowed_list:\s*(.+)$", live.stdout, re.M)[
                            1
                        ].strip()
                        == "4"
                    )
                valid, reason = check_output(case, out, actual_exit)
                counters = {
                    event: int(v.replace(",", ""))
                    for v, event in re.findall(
                        r"^\s*([\d,]+)\s+(cpu-cycles:u|instructions:u|branch-misses:u)\b",
                        pmu,
                        re.M,
                    )
                }
                elapsed = re.search(r"Total test time:\s*([\d.]+)\s*seconds", pmu)
                assert elapsed and len(counters) == 3, pmu
                seconds = float(elapsed[1])
                score = None
                if case["name"] == "coremark" and valid:
                    ticks = int(re.search(r"Total ticks\s*:\s*(\d+)", out)[1])
                    seconds = ticks / 1000
                    valid = seconds >= 10
                    score = 100000 / seconds
                row = {
                    "case": case["name"],
                    "arm": arm,
                    "round": rep,
                    "exit": actual_exit,
                    "profiler_exit": proc.returncode,
                    "valid": valid,
                    "check": reason,
                    "seconds": seconds,
                    "whole_child_seconds": float(elapsed[1]),
                    "pmu": counters,
                    "coremark_iterations_per_second": score,
                    "cpu_allowed_list": allowed,
                    "live_pid_verified": ("Cpus_allowed_list:" in live.stdout),
                    "command": command,
                    "host_elapsed_s_diagnostic": time.monotonic() - start,
                    "prefix": label,
                }
                rows.append(row)
                save("runtime-records.json", rows)
                print(
                    label,
                    "valid" if valid else "FAILED",
                    actual_exit,
                    round(seconds, 6),
                    flush=True,
                )
                if not valid:
                    failed.add((case["name"], arm))
        policy("runtime-policy-round-" + str(rep) + "-after")
    policy(
        "runtime-policy-after" if mode == "runtime" else "runtime-resume-policy-after"
    )
    print("runtime complete", len(rows), "samples", flush=True)
elif mode == "compile":
    rows = []
    policy("compile-policy-before")
    assert all(r["valid"] for r in json.loads((P / "smoke-summary.json").read_text()))
    eligible = {(c["name"], a) for c in CASES for a in ARMS}
    for rep, order in enumerate(list(reversed(ORDERS))):
        policy("compile-policy-round-" + str(rep) + "-before")
        for case in CASES:
            for arm in order:
                if (case["name"], arm) not in eligible:
                    continue
                label = f"compile-{case['name']}-{arm}-{rep}"
                args = (
                    case["args"]
                    if case["name"] != "coremark"
                    else ["0", "0", "0", "20", "7", "1", "2000"]
                )
                command = (
                    "taskset 10 timeout -k 2 120 "
                    + app(arm, True)
                    + " "
                    + shlex.join([NS + "/" + case["guest"]] + args)
                )
                r = run(label, command, timeout=130)
                cc = dict(case)
                if case["name"] == "coremark":
                    good = r.returncode == 0 and "crcfinal      : 0x4983" in r.stdout
                else:
                    good = check_output(cc, r.stdout, r.returncode)[0]
                if arm == "fex":
                    text = next(
                        (
                            s.split("FEX_COMPILE_STAGE_PROBE ", 1)[1]
                            for s in r.stderr.splitlines()
                            if "FEX_COMPILE_STAGE_PROBE " in s
                        ),
                        None,
                    )
                    rec = json.loads(text) if text else None
                    good = bool(good and rec and rec["qualified"] and rec["closed"])
                    ns = (
                        (
                            rec["root_ticks"]
                            - next(
                                s["ticks"]
                                for s in rec["stages"]
                                if s["name"] == "byte_decode"
                            )
                        )
                        * 1e9
                        / rec["cntfrq_hz"]
                        if good
                        else None
                    )
                else:
                    text = next(
                        (s for s in r.stderr.splitlines() if s.startswith("{")), None
                    )
                    rec = json.loads(text) if text else None
                    good = bool(good and rec and rec["runtime_completed"])
                    ns = rec["compile_without_byte_decode_ns"] if good else None
                rows.append(
                    {
                        "case": case["name"],
                        "arm": arm,
                        "round": rep,
                        "valid": good,
                        "compile_excluding_byte_ns": ns,
                        "record": rec,
                        "prefix": label,
                    }
                )
                save("compile-records.json", rows)
                print(
                    label,
                    "valid" if good else "FAILED",
                    None if ns is None else round(ns / 1e6, 4),
                    flush=True,
                )
        policy("compile-policy-round-" + str(rep) + "-after")
    policy("compile-policy-after")
elif mode == "cleanup":
    run("stop-own-server", ENV + " " + NS + "/FEXServer -k")
    ids = json.loads((P / "inputs.json").read_text())
    run(
        "remote-identities-after",
        "sha256sum " + shlex.join(NS + "/" + n for n in ids),
        0,
    )
    before = run("cleanup-ps", "ps -A -o PID,PPID,ARGS", 0).stdout
    active = [
        s
        for s in before.splitlines()
        if NS in s and not any(x in s for x in [" sh -c ", "ps -A", "FEXServer -k"])
    ]
    opened = run("cleanup-lsof", "lsof").stdout
    own_open = [s for s in opened.splitlines() if NS in s]
    assert not active and not own_open, (active, own_open)
    du = run("cleanup-du", "du -sk " + NS, 0).stdout
    df1 = run("cleanup-df-before", "df -k /data/local/tmp", 0).stdout
    run("cleanup-remove", "rm -rf -- " + NS, 0)
    run("cleanup-absent", "test ! -e " + NS, 0)
    df2 = run("cleanup-df-after", "df -k /data/local/tmp", 0).stdout
    save(
        "phone-cleanup-receipt.json",
        {
            "utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "namespace": NS,
            "allocated_bytes_removed": int(du.split()[0]) * 1024,
            "df_actual_free_delta_bytes": (
                int(df2.splitlines()[-1].split()[3])
                - int(df1.splitlines()[-1].split()[3])
            )
            * 1024,
            "remaining_bytes": 0,
            "active_processes": active,
            "open_files": own_open,
            "errors": [],
        },
    )
    print("cleanup complete", flush=True)
else:
    raise SystemExit("prepare|runtime|compile|cleanup")
