"""Run short correctness checks with a CPU duty cycle and bounded captured output."""

from __future__ import annotations

from dataclasses import dataclass
import math
import os
from pathlib import Path
import resource
import select
import signal
import subprocess
import sys
import time


@dataclass(frozen=True)
class RunLimits:
    wall_seconds: float = 30
    cpu_fraction: float = 0.1
    cpu_seconds: float = 4
    rss_mib: int = 192
    output_mib: int = 8

    def validate(self):
        if (not all(math.isfinite(value) for value in
                    (self.wall_seconds, self.cpu_fraction, self.cpu_seconds)) or
                self.wall_seconds <= 0 or self.cpu_seconds <= 0 or
                not 0 < self.cpu_fraction <= 1 or self.rss_mib <= 0 or self.output_mib <= 0):
            raise ValueError('invalid process resource limits')


@dataclass(frozen=True)
class RunResult:
    return_code: int
    elapsed_s: float
    sampled_peak_rss_kib: int
    stop_reason: str | None


def kernel_limits(group_file=Path('/proc/self/cgroup'), control_root=Path('/sys/fs/cgroup')):
    """Read effective cgroup v2 ceilings, including stricter parent groups."""
    try:
        membership = group_file.read_text().splitlines()
    except OSError:
        return {}
    group = next((line[3:] for line in membership if line.startswith('0::')), None)
    if group is None or not group.startswith('/') or '..' in Path(group).parts:
        return {}
    current = control_root / group.lstrip('/')
    if not current.is_dir():
        return {}
    result = {'source': 'cgroup_v2', 'cpu_fraction': None,
              'memory_bytes': None, 'swap_bytes': None}
    while True:
        for name, key in (('cpu.max', 'cpu_fraction'), ('memory.max', 'memory_bytes'),
                          ('memory.swap.max', 'swap_bytes')):
            try:
                fields = (current / name).read_text().split()
                if not fields or fields[0] == 'max':
                    continue
                value = int(fields[0])
                if name == 'cpu.max':
                    value /= int(fields[1])
                if value < 0:
                    continue
            except (OSError, ValueError, IndexError, ZeroDivisionError):
                continue
            result[key] = value if result[key] is None else min(value, result[key])
        if current == control_root:
            break
        current = current.parent
    return result


def group_rss(pgid: int) -> int:
    # A single child can create workers. Account for the whole process group,
    # and never select another task by executable name or a broad PID match.
    result = subprocess.run(['ps', '-axo', 'pgid=,rss='], capture_output=True,
                            text=True, timeout=2, check=True)
    total = 0
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) == 2 and int(fields[0]) == pgid:
            total += int(fields[1])
    return total


def signal_group(child: subprocess.Popen, sig: int):
    try:
        os.killpg(child.pid, sig)
    except ProcessLookupError:
        pass
    except PermissionError:
        # macOS can report EPERM for a group whose only member has exited
        # but has not yet been reaped. Reap and retry; live workers still
        # receive the signal, and a real permission error remains an error.
        if child.poll() is None:
            raise
        try:
            os.killpg(child.pid, sig)
        except ProcessLookupError:
            pass


def run_limited(command, *, stdout, stderr, env=None, cwd=None,
                limits=RunLimits()) -> RunResult:
    limits.validate()

    def child_limits():
        os.nice(15)
        cpu = math.ceil(limits.cpu_seconds)
        resource.setrlimit(resource.RLIMIT_CPU, (cpu, cpu))

    started = time.monotonic()
    # A process-wide file size limit also applies to Linux memfd storage used
    # by the JIT. Bound the captured streams without limiting its code cache.
    child = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             env=env, cwd=cwd, start_new_session=True, preexec_fn=child_limits)
    streams = {child.stdout: stdout if stdout is not None else sys.stdout.buffer,
               child.stderr: stderr if stderr is not None else sys.stderr.buffer}
    sizes = {stream: 0 for stream in streams}
    output_limit = limits.output_mib * 1024 * 1024

    def collect_output(wait_seconds=0):
        deadline = time.monotonic() + wait_seconds
        while streams:
            ready, _, _ = select.select(list(streams), [], [],
                                        max(0, deadline - time.monotonic()))
            if not ready:
                break
            for stream in ready:
                data = os.read(stream.fileno(), 65536)
                if not data:
                    del streams[stream]
                    continue
                available = max(0, output_limit - sizes[stream])
                streams[stream].write(data[:available])
                sizes[stream] += len(data)
                if sizes[stream] > output_limit:
                    return False
            if wait_seconds and time.monotonic() >= deadline:
                break
        remaining = deadline - time.monotonic()
        if not streams and remaining > 0 and child.poll() is None:
            time.sleep(remaining)
        return True
    peak = 0
    reason = None
    period = 0.2
    try:
        while child.poll() is None:
            signal_group(child, signal.SIGSTOP)
            if not collect_output():
                reason = 'output_limit'
                break
            peak = max(peak, group_rss(child.pid))
            remaining = limits.wall_seconds - (time.monotonic() - started)
            if peak > limits.rss_mib * 1024:
                reason = 'rss_limit'
                break
            if remaining <= 0:
                reason = 'wall_limit'
                break
            signal_group(child, signal.SIGCONT)
            if not collect_output(min(period * limits.cpu_fraction, remaining)):
                reason = 'output_limit'
                break
            signal_group(child, signal.SIGSTOP)
            if child.poll() is not None:
                break
            remaining = limits.wall_seconds - (time.monotonic() - started)
            if remaining > 0:
                time.sleep(min(period * (1 - limits.cpu_fraction), remaining))
    finally:
        # Also reap workers left behind by a parent that exited successfully.
        try:
            signal_group(child, signal.SIGKILL)
        finally:
            child.wait()
            try:
                if not collect_output():
                    reason = 'output_limit'
            finally:
                child.stdout.close()
                child.stderr.close()
    if reason is None and child.returncode in (-signal.SIGXCPU, -signal.SIGKILL):
        reason = 'cpu_limit_or_external_signal'
    elif reason is None and child.returncode == -signal.SIGXFSZ:
        reason = 'output_limit'
    return RunResult(child.returncode, time.monotonic() - started, peak, reason)
