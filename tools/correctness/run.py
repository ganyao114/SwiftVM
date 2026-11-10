#!/usr/bin/env python3
"""Bounded differential controller for local, ADB and system-QEMU workers.

The DUT's `compare` command owns comparison contracts. Unicorn runs in a fresh
independent process; unavailable reference capabilities never become passes.
System-QEMU workers use SSH into a fixed, reusable VM, optionally started by
boot_command. The controller never builds kernels, initrds or disk images.
"""

from __future__ import annotations

import argparse
import itertools
import json
import os
from pathlib import Path
import selectors
import shlex
import shutil
import signal
import subprocess
import sys
import time
import uuid

import oracle


DEFAULT_ROOT = Path("/Volumes/Data2/swiftvm-correctness-results")
FAILURES = {"semantic_mismatch", "exception_mismatch", "unexpected_rejection",
            "host_crash", "sanitizer_failure", "timeout", "infrastructure_failure"}
REPLAY_FAILURES = FAILURES - {"infrastructure_failure"}
GAPS = {"control_one_step_gap", "state_service_gap", "oracle_unsupported",
        "oracle_inconclusive", "x87_gap", "nondeterministic_gap", "profile_gap", "oracle_cpsr_gap"}


class InfrastructureError(RuntimeError):
    pass


class QuotaExceeded(RuntimeError):
    pass


class InfrastructureStop(RuntimeError):
    pass


def load_json(path: Path, maximum: int = 128 * 1024 * 1024) -> dict:
    if path.stat().st_size > maximum:
        raise InfrastructureError(f"oversized JSON result: {path}")
    with path.open() as stream:
        return json.load(stream)


def tree_bytes(root: Path) -> int:
    return sum(entry.stat(follow_symlinks=False).st_size
               for directory, _, files in os.walk(root)
               for name in files if (entry := Path(directory) / name).exists())


def kill_owned(process: subprocess.Popen):
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
    except ProcessLookupError:
        process.wait()


def bounded_run(command: list[str], log: Path, timeout: float, output_limit: int,
                cwd: Path | None = None) -> dict:
    """Drain both pipes with a byte limit; terminate only this process group."""
    started = time.monotonic()
    environment = os.environ.copy()
    temporary = log.parent / "tmp"
    temporary.mkdir(exist_ok=True)
    environment.update(TMPDIR=str(temporary), PYTHONDONTWRITEBYTECODE="1")
    total, reason = 0, ""
    try:
        process = subprocess.Popen(command, cwd=cwd, env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, start_new_session=True)
    except OSError as error:
        log.write_text(str(error) + "\n")
        return {"returncode": None, "reason": "spawn_failure", "error": str(error),
                "elapsed_ms": (time.monotonic() - started) * 1000}
    selector = selectors.DefaultSelector()
    for stream in (process.stdout, process.stderr):
        os.set_blocking(stream.fileno(), False)
        selector.register(stream, selectors.EVENT_READ)
    with log.open("wb") as output:
        try:
            while selector.get_map():
                if time.monotonic() - started > timeout:
                    reason = "timeout"
                    kill_owned(process)
                    break
                for key, _ in selector.select(timeout=0.05):
                    block = os.read(key.fileobj.fileno(), 16384)
                    if not block:
                        selector.unregister(key.fileobj)
                        key.fileobj.close()
                        continue
                    output.write(block[:max(0, output_limit - total)])
                    total += len(block)
                    if total > output_limit:
                        reason = "output_limit"
                        kill_owned(process)
                        break
                if reason:
                    break
            if process.poll() is None:
                process.wait(timeout=max(0.01, timeout - (time.monotonic() - started)))
        except subprocess.TimeoutExpired:
            reason = "timeout"
            kill_owned(process)
        except BaseException:
            kill_owned(process)
            raise
        finally:
            selector.close()
            for stream in (process.stdout, process.stderr):
                if not stream.closed:
                    stream.close()
    return {"returncode": process.returncode, "reason": reason,
            "output_bytes": total, "elapsed_ms": (time.monotonic() - started) * 1000}


def checked(command: list[str], log: Path, timeout: float = 15, output_limit: int = 262144) -> dict:
    result = bounded_run(command, log, timeout, output_limit)
    if result["returncode"] != 0 or result["reason"]:
        raise InfrastructureError(f"command failed ({result}): {shlex.join(command)}; log={log}")
    return result


class Worker:
    def __init__(self, specification: dict, run: Path, timeout: float, output_limit: int):
        self.spec, self.run = specification, run
        self.name, self.transport = specification["name"], specification.get("transport", "local")
        if not self.name or any(c not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_." for c in self.name):
            raise InfrastructureError("worker name must be a path-safe identifier")
        self.timeout = specification.get("timeout_seconds", timeout)
        self.output_limit, self.vm = output_limit, None
        self.remote_root = specification.get("remote_root", "")
        if self.transport != "local" and (not self.remote_root.startswith("/") or ".." in Path(self.remote_root).parts):
            raise InfrastructureError("remote_root must be an absolute task directory")
        self.prepared, self.binary_hash = False, ""

    def _ssh(self) -> list[str]:
        ssh = self.spec.get("ssh", {})
        result = [ssh.get("program", "ssh"), "-p", str(ssh.get("port", 22)),
                  "-o", "BatchMode=yes", "-o", "ConnectTimeout=3"]
        if ssh.get("identity"):
            result += ["-i", ssh["identity"]]
        return result + [f"{ssh.get('user', 'root')}@{ssh.get('host', '127.0.0.1')}"]

    def remote(self, arguments: list[str], log: Path, timeout: float = 15) -> dict:
        if self.transport == "adb":
            command = [self.spec.get("adb", "adb"), "-s", self.spec["serial"], "shell", shlex.join(arguments)]
        else:
            command = self._ssh() + [shlex.join(arguments)]
        return bounded_run(command, log, timeout, self.output_limit)

    def remote_checked(self, arguments: list[str], log: Path, timeout: float = 15):
        result = self.remote(arguments, log, timeout)
        if result["returncode"] != 0 or result["reason"]:
            raise InfrastructureError(f"remote command failed ({result}); log={log}")
        return result

    def transfer(self, local: Path, remote: str, log: Path, upload: bool):
        if self.transport == "adb":
            command = [self.spec.get("adb", "adb"), "-s", self.spec["serial"],
                       "push" if upload else "pull"]
            command += [str(local), remote] if upload else [remote, str(local)]
        else:
            ssh = self.spec.get("ssh", {})
            peer = f"{ssh.get('user', 'root')}@{ssh.get('host', '127.0.0.1')}:{remote}"
            command = [ssh.get("scp", "scp"), "-O", "-P", str(ssh.get("port", 22)),
                       "-o", "BatchMode=yes", "-o", "ConnectTimeout=3"]
            if ssh.get("identity"):
                command += ["-i", ssh["identity"]]
            command += [str(local), peer] if upload else [peer, str(local)]
        checked(command, log, 30, self.output_limit)

    def prepare(self, directory: Path):
        if self.prepared:
            return
        if self.transport == "system-qemu" and self.spec.get("command"):
            # An existing console/serial bridge can use the same file protocol.
            # Its guest binary identity must be supplied by the build/deploy owner.
            self.binary_hash = self.spec.get("guest_binary_sha256", "unavailable")
            self.prepared = True
            return
        if self.transport == "system-qemu":
            probe = self.remote(["true"], directory / "qemu-probe.log", 5)
            if probe["returncode"] != 0 or probe["reason"]:
                boot = self.spec.get("boot_command")
                if not boot:
                    raise InfrastructureError("system-QEMU SSH endpoint unavailable and no boot_command configured")
                environment = os.environ.copy()
                environment["TMPDIR"] = str(directory / "tmp")
                (directory / "tmp").mkdir(exist_ok=True)
                self.vm = subprocess.Popen(boot, env=environment, start_new_session=True,
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                deadline = time.monotonic() + self.spec.get("boot_timeout_seconds", 90)
                while time.monotonic() < deadline and self.vm.poll() is None:
                    probe = self.remote(["true"], directory / "qemu-probe.log", 5)
                    if probe["returncode"] == 0 and not probe["reason"]:
                        break
                    time.sleep(0.5)
                else:
                    raise InfrastructureError("owned system-QEMU did not reach its SSH endpoint")
        if self.transport not in ("local", "adb", "ssh", "system-qemu"):
            raise InfrastructureError("unknown worker transport: " + self.transport)
        if self.transport != "local":
            self.remote_checked(["mkdir", "-p", self.remote_root], directory / "remote-root.log")
            if self.spec.get("host_binary"):
                host_binary = Path(self.spec["host_binary"])
                self.transfer(host_binary, self.spec["binary"], directory / "deploy.log", True)
                self.remote_checked(["chmod", "700", self.spec["binary"]], directory / "chmod.log")
            self.remote_checked(["sha256sum", self.spec["binary"]], directory / "binary-hash.log")
            self.binary_hash = (directory / "binary-hash.log").read_text().split()[0]
        self.prepared = True

    def execute(self, dut: Path, case: Path, config: Path, out: Path, directory: Path) -> dict:
        self.prepare(directory)
        if self.transport == "local" or (self.transport == "system-qemu" and self.spec.get("command")):
            binary = Path(self.spec.get("binary", str(dut)))
            if self.transport == "local":
                self.binary_hash = oracle.sha256(binary)
            values = {"binary": str(binary), "dut": str(dut), "case": str(case),
                      "config": str(config), "out": str(out), "job": str(directory)}
            template = self.spec.get("command", ["{binary}", "worker", "--case", "{case}",
                                                 "--config", "{config}", "--out", "{out}"])
            command = [value.format_map(values) for value in template]
            return bounded_run(command, directory / "worker.log", self.timeout, self.output_limit)
        remote_directory = self.remote_root.rstrip("/") + "/" + directory.name
        self.remote_checked(["mkdir", remote_directory], directory / "remote-mkdir.log")
        error = None
        try:
            self.transfer(case, remote_directory + "/case.svmcase", directory / "push-case.log", True)
            self.transfer(config, remote_directory + "/config.json", directory / "push-config.log", True)
            command = [self.spec["binary"], "worker", "--case", remote_directory + "/case.svmcase",
                       "--config", remote_directory + "/config.json", "--out", remote_directory + "/observation.json"]
            inner = "echo $$ > " + shlex.quote(remote_directory + "/pid") + "; exec " + shlex.join(command)
            # timeout runs in the guest too: killing adb/ssh alone leaves a runaway DUT.
            result = self.remote(["timeout", "-s", "KILL", str(max(1, int(self.timeout))) + "s",
                                  "sh", "-c", inner], directory / "worker.log", self.timeout + 5)
            if result["returncode"] == 0 and not result["reason"]:
                self.transfer(out, remote_directory + "/observation.json", directory / "pull.log", False)
            elif result["returncode"] in (124, 137):
                result["reason"] = "timeout"
            return result
        finally:
            # Only kill a recorded process whose argv still names this exact Case.
            pid_log = directory / "remote-pid.log"
            status = self.remote(["cat", remote_directory + "/pid"], pid_log)
            if status["returncode"] == 0:
                pid = pid_log.read_text().strip()
                if pid.isdecimal():
                    args_log = directory / "remote-argv.log"
                    status = self.remote(["cat", f"/proc/{pid}/cmdline"], args_log)
                    if status["returncode"] == 0 and (remote_directory + "/case.svmcase").encode() in args_log.read_bytes().split(b"\0"):
                        status = self.remote(["kill", "-KILL", pid], directory / "remote-kill.log")
                        if status["returncode"] != 0:
                            error = "could not terminate owned remote worker"
            status = self.remote(["rm", "-rf", remote_directory], directory / "remote-cleanup.log")
            if status["returncode"] != 0 or status["reason"] or error:
                oracle.write_json(directory / "remote-cleanup-error.json", {"error": error, "result": status})
                raise InfrastructureError("remote task cleanup failed; retained remote-cleanup-error.json")

    def close(self):
        if self.vm:
            kill_owned(self.vm)


def failure_key(result: dict) -> tuple:
    comparison = result.get("comparison", {})
    # Comparison diagnostics name the field before the first colon.
    return result["classification"], comparison.get("category", ""), comparison.get("detail", "").split(":", 1)[0]


class Controller:
    def __init__(self, settings: dict, dut: Path, run: Path, oracle_python: str | None = None):
        self.settings, self.dut, self.run = settings, dut.resolve(), run
        self.root = run.parent
        self.quota = settings.get("quota_bytes", 8 * 1024**3)
        self.worker_quota = settings.get("worker_quota_bytes", 512 * 1024**2)
        self.min_free = settings.get("min_free_bytes", 4 * 1024**3)
        self.timeout = settings.get("timeout_seconds", 10)
        self.output_limit = settings.get("output_limit_bytes", 262144)
        self.oracle_python = oracle_python or settings.get("oracle", {}).get("python", sys.executable)
        self.workers = [Worker(value, run, self.timeout, self.output_limit)
                        for value in settings["workers"] if value.get("enabled", True)]
        self.counts, self.failures, self.gaps, self.infrastructure_streaks = {}, [], [], {}

    def check_quota(self, directory: Path | None = None):
        if shutil.disk_usage(self.root).free < self.min_free:
            raise QuotaExceeded("minimum free space threshold reached")
        if tree_bytes(self.root) > self.quota:
            raise QuotaExceeded("scratch quota reached; retained evidence was not removed")
        if directory and tree_bytes(directory) > self.worker_quota:
            raise QuotaExceeded("per-worker quota reached")

    def evaluate(self, case_path: Path, config: dict, worker: Worker, directory: Path) -> dict:
        directory.mkdir(parents=True)
        self.check_quota()
        shutil.copyfile(case_path, directory / "case.svmcase")
        oracle.write_json(directory / "config.json", config)
        manifest = {"protocol_version": 1, "case_sha256": oracle.sha256(case_path),
                    "run_config": config, "worker": worker.spec,
                    "dut_binary": str(self.dut), "dut_sha256": oracle.sha256(self.dut),
                    "controller_sha256": oracle.sha256(Path(__file__)),
                    "oracle_adapter_sha256": oracle.sha256(Path(oracle.__file__)),
                    "oracle_python": self.oracle_python, "created_unix": time.time(),
                    "fresh_process": True}
        oracle.write_json(directory / "manifest.json", manifest)
        # Commit the Case/config before entering either potentially crashing engine.
        oracle_timeout = self.settings.get("oracle", {}).get("timeout_seconds", 5)
        probe = bounded_run([self.oracle_python, str(Path(oracle.__file__).resolve()),
                             "--case", str(directory / "case.svmcase"), "--out", str(directory / "oracle.json"),
                             "--timeout-ms", str(max(1, int(oracle_timeout * 800)))],
                            directory / "oracle.log", oracle_timeout, self.output_limit)
        result = {"case_sha256": manifest["case_sha256"], "worker": worker.name,
                  "config": config, "processes": {"oracle": probe}}
        if probe["reason"] or probe["returncode"] != 0:
            result["classification"] = "oracle_inconclusive"
            result["detail"] = probe
        else:
            expected = load_json(directory / "oracle.json")
            worker_result = worker.execute(self.dut, directory / "case.svmcase", directory / "config.json",
                                           directory / "actual.json", directory)
            result["processes"]["worker"] = worker_result
            manifest["worker_binary_sha256"] = worker.binary_hash
            oracle.write_json(directory / "manifest.json", manifest)
            self.check_quota(directory)
            if worker_result["reason"] == "timeout":
                result["classification"] = "timeout"
            elif worker_result["reason"] or worker_result["returncode"] not in (0, 77):
                result["classification"] = "host_crash" if isinstance(worker_result["returncode"], int) and worker_result["returncode"] < 0 else "infrastructure_failure"
            elif expected["outcome"] == "unavailable":
                result["classification"] = "oracle_unsupported" if expected.get("error", "").startswith("oracle_unsupported:") else "oracle_inconclusive"
                result["detail"] = expected.get("error", "")
            else:
                actual = load_json(directory / "actual.json")
                if actual["outcome"] == "unavailable":
                    result["classification"] = "infrastructure_unavailable"
                    result["detail"] = actual.get("error", "")
                    oracle.write_json(directory / "result.json", result)
                    return result
                if actual["outcome"] == "compiled" and config.get("compile_only", False):
                    result["classification"] = "compiled"
                    oracle.write_json(directory / "result.json", result)
                    return result
                comparison_path = directory / "comparison.json"
                comparison_process = bounded_run([str(self.dut), "compare", "--case", str(directory / "case.svmcase"),
                                                   "--expected", str(directory / "oracle.json"),
                                                   "--actual", str(directory / "actual.json"), "--out", str(comparison_path)],
                                                  directory / "compare.log", self.timeout, self.output_limit)
                result["processes"]["compare"] = comparison_process
                if comparison_process["reason"] or not comparison_path.exists():
                    result["classification"] = "infrastructure_failure"
                else:
                    comparison = load_json(comparison_path)
                    result["comparison"] = comparison
                    result["classification"] = comparison["category"]
                    if comparison["equal"] and comparison["comparable"]:
                        if config.get("compile_only", False):
                            result["classification"] = "compiled"
                        elif (config["backend"] != "direct" and
                              actual.get("outcome") in ("terminal", "budget", "service") and
                              not actual.get("native_executed", False)):
                            result["classification"] = "policy_fallback"
                        elif config["backend"] != "direct" and actual.get("actual_scope") != config["scope"]:
                            result["classification"] = "policy_fallback"
                            result["detail"] = "requested allocation scope was not executed"
                        elif comparison.get("gaps"):
                            result["classification"] = "verified_with_gap"
                        else:
                            result["classification"] = "verified"
        oracle.write_json(directory / "result.json", result)
        return result

    def record_infrastructure(self, worker: Worker, result: dict):
        if result["classification"] != "infrastructure_failure":
            self.infrastructure_streaks.pop(worker.name, None)
            return
        failed = tuple((stage, value.get("reason", ""), value.get("returncode"))
                       for stage, value in result.get("processes", {}).items()
                       if value.get("reason") or value.get("returncode") not in (0, 77))
        # Keep varying journal paths out of the signature; this is a stage failure.
        signature = failed or (result.get("detail", "").split(":", 1)[0],)
        previous, count = self.infrastructure_streaks.get(worker.name, (None, 0))
        count = count + 1 if signature == previous else 1
        self.infrastructure_streaks[worker.name] = signature, count
        if count >= 3:
            oracle.write_json(self.run / "infrastructure-stop.json", {
                "worker": worker.name, "consecutive_failures": count,
                "signature": signature, "reason": "repeated infrastructure failure; campaign stopped"})
            raise InfrastructureStop(worker.name + ": the same infrastructure failure repeated three times")

    def reduce(self, failure: Path, worker: Worker, config: dict, result: dict, budget: int):
        predicate = failure / "predicate.json"
        oracle.write_json(predicate, {"settings": self.settings, "dut": str(self.dut),
                                     "run": str(self.run), "worker": worker.spec,
                                     "config": config, "oracle_python": self.oracle_python,
                                     "failure_key": failure_key(result)})
        wrapper = failure / "predicate.sh"
        wrapper.write_text("#!/bin/sh\nexec " + shlex.join([sys.executable, str(Path(__file__).resolve()),
                                                             "_predicate", str(predicate)]) + ' "$1"\n')
        wrapper.chmod(0o700)
        candidate = failure / "minimized.svmcase"
        reduction = bounded_run([str(self.dut), "reduce", "--case", str(failure / "original.svmcase"),
                                 "--predicate", str(wrapper), "--out", str(candidate), "--budget", str(budget)],
                                failure / "reduce.log", max(self.timeout, budget * (self.timeout + 5)), self.output_limit)
        oracle.write_json(failure / "reduction.json", reduction)
        if reduction["returncode"] == 0 and candidate.exists():
            verification = self.run / "jobs" / ("reduced-" + uuid.uuid4().hex)
            reduced_result = self.evaluate(candidate, config, worker, verification)
            if failure_key(reduced_result) == failure_key(result):
                shutil.move(str(verification), str(failure / "minimized-replay"))
            else:
                candidate.rename(failure / "unconfirmed-candidate.svmcase")
                shutil.move(str(verification), str(failure / "unconfirmed-replay"))

    def close(self, receipt: bool = True):
        for worker in self.workers:
            worker.close()
        if receipt:
            errors = [str(path) for path in self.run.rglob("remote-cleanup-error.json")]
            oracle.write_json(self.run / "cleanup-receipt.json", {"finished_unix": time.time(),
                              "owned_vm_processes_stopped": True, "scratch_bytes": tree_bytes(self.run),
                              "failures_retained": self.failures, "errors": errors})


def predicate_main(arguments: list[str]) -> int:
    manifest, candidate = Path(arguments[0]), Path(arguments[1])
    value = load_json(manifest)
    run = Path(value["run"])
    settings = dict(value["settings"], workers=[dict(value["worker"], enabled=True)])
    controller = Controller(settings, Path(value["dut"]), run, value["oracle_python"])
    directory = run / "jobs" / ("predicate-" + uuid.uuid4().hex)
    try:
        result = controller.evaluate(candidate, value["config"], controller.workers[0], directory)
        return 0 if failure_key(result) == tuple(value["failure_key"]) else 1
    except (InfrastructureError, QuotaExceeded, ValueError, OSError):
        return 1
    finally:
        controller.close(receipt=False)
        shutil.rmtree(directory, ignore_errors=True)


def configs(worker: Worker, base: dict, matrix: bool):
    base = dict(base, **worker.spec.get("config", {}))
    base.setdefault("backend", worker.spec.get("backend", "direct"))
    for key, value in (("scope", "block"), ("target", "known"), ("optimize", True),
                       ("pin", True), ("compile_only", False)):
        base.setdefault(key, value)
    if matrix and base["backend"] != "direct":
        for scope, target, optimize in itertools.product(("block", "function"),
                                                        ("known", "early-off", "unknown"), (False, True)):
            yield dict(base, scope=scope, target=target, optimize=optimize)
    else:
        yield base


def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "_predicate":
        return predicate_main(sys.argv[2:])
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dut", type=Path, required=True)
    parser.add_argument("--workers", type=Path, required=True)
    parser.add_argument("--case", type=Path, action="append", default=[])
    parser.add_argument("--input-dir", type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--worker", action="append")
    parser.add_argument("--results", type=Path)
    parser.add_argument("--oracle-python")
    parser.add_argument("--matrix", action="store_true")
    parser.add_argument("--shard-count", type=int, default=1)
    parser.add_argument("--shard-index", type=int, default=0)
    parser.add_argument("--reduce", action="store_true")
    parser.add_argument("--reduce-budget", type=int, default=64)
    parser.add_argument("--progress-every", type=int, default=25)
    args = parser.parse_args()
    if args.shard_count <= 0 or not 0 <= args.shard_index < args.shard_count:
        parser.error("invalid shard")
    settings = load_json(args.workers)
    root = (args.results or Path(settings.get("scratch_root", DEFAULT_ROOT))).resolve()
    root.mkdir(parents=True, exist_ok=True)
    run = root / (time.strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex[:8])
    (run / "jobs").mkdir(parents=True)
    (run / "failures").mkdir()
    (run / "gaps").mkdir()
    oracle.write_json(run / "workers.json", settings)
    oracle.write_json(run / "owner.json", {"pid": os.getpid(), "run_id": run.name, "created_unix": time.time()})
    base = load_json(args.config) if args.config else {}
    controller = Controller(settings, args.dut, run, args.oracle_python)
    if args.worker:
        controller.workers = [worker for worker in controller.workers if worker.name in args.worker]
    cases = list(args.case)
    if args.input_dir:
        cases += sorted(args.input_dir.rglob("*.svmcase"))
    if not cases or not controller.workers:
        parser.error("provide cases and at least one enabled worker")
    exit_code = 0
    try:
        for case_path in cases:
            case_path = case_path.resolve()
            case_hash = oracle.sha256(case_path)
            if int(case_hash[:16], 16) % args.shard_count != args.shard_index:
                continue
            decoded = oracle.read_case(case_path)
            guest = oracle.ISA_NAMES[decoded["isa"]]
            for worker in controller.workers:
                if guest not in worker.spec.get("guests", ["x86_64", "arm64", "arm32"]):
                    continue
                for config in configs(worker, base, args.matrix):
                    controller.check_quota()
                    directory = run / "jobs" / uuid.uuid4().hex
                    try:
                        result = controller.evaluate(case_path, config, worker, directory)
                    except (InfrastructureError, ValueError, OSError) as error:
                        directory.mkdir(exist_ok=True)
                        result = {"classification": "infrastructure_failure", "detail": str(error),
                                  "worker": worker.name, "config": config, "case_sha256": case_hash}
                        oracle.write_json(directory / "result.json", result)
                    classification = result["classification"]
                    controller.counts[classification] = controller.counts.get(classification, 0) + 1
                    completed = sum(controller.counts.values())
                    if classification not in ("verified", "verified_with_gap", "compiled", "expected_rejection", "expected_fault") or completed % max(1, args.progress_every) == 0:
                        print(json.dumps({"case": case_hash, "worker": worker.name, "result": classification,
                                          "completed": completed}), flush=True)
                    # Always retain capability gaps; they are evidence, never verification.
                    if classification in GAPS:
                        gap = run / "gaps" / directory.name
                        directory.rename(gap)
                        if (gap / "case.svmcase").exists():
                            (gap / "case.svmcase").rename(gap / "original.svmcase")
                        else:
                            shutil.copyfile(case_path, gap / "original.svmcase")
                        controller.gaps.append(str(gap))
                    elif classification not in ("verified", "verified_with_gap", "compiled", "expected_rejection", "expected_fault"):
                        failure = run / "failures" / directory.name
                        directory.rename(failure)
                        if (failure / "case.svmcase").exists():
                            (failure / "case.svmcase").rename(failure / "original.svmcase")
                        else:
                            shutil.copyfile(case_path, failure / "original.svmcase")
                        controller.failures.append(str(failure))
                        if classification in FAILURES:
                            exit_code = 1
                            if classification in REPLAY_FAILURES:
                                replay_dir = run / "jobs" / ("fresh-" + uuid.uuid4().hex)
                                try:
                                    fresh = controller.evaluate(failure / "original.svmcase", config, worker, replay_dir)
                                    shutil.move(str(replay_dir), str(failure / "fresh-replay"))
                                    if args.reduce and failure_key(fresh) == failure_key(result):
                                        controller.reduce(failure, worker, config, result, args.reduce_budget)
                                except (InfrastructureError, ValueError, OSError) as error:
                                    oracle.write_json(failure / "replay-error.json", {"error": str(error)})
                        elif exit_code == 0:
                            exit_code = 2
                    else:
                        shutil.rmtree(directory)
                    oracle.write_json(run / "summary.json", {"counts": controller.counts,
                                      "cases_supplied": len(cases), "shard_index": args.shard_index,
                                      "shard_count": args.shard_count, "failures": controller.failures, "gaps": controller.gaps})
                    controller.record_infrastructure(worker, result)
    except QuotaExceeded as error:
        oracle.write_json(run / "quota-stop.json", {"error": str(error)})
        exit_code = 2
    except InfrastructureStop:
        exit_code = 2
    except KeyboardInterrupt:
        exit_code = 130
    finally:
        controller.close()
        print(f"Evidence: {run}", flush=True)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
