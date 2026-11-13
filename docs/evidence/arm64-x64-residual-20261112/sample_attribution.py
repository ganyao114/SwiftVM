#!/usr/bin/env python3
"""Attribute archived CPU-clock IP samples to host instruction classes.

Input is the `diagnostics/samples` directory extracted from
docs/evidence/arm64-x64-mechanisms-20261112/raw-evidence.tar.gz:

    <root>/<workload>/<engine>/<process>/{*.asm, samples.tsv}

Commands
    classes   ROOT [WORKLOAD...]            class shares per workload and engine
    window    ROOT WORKLOAD ENGINE PROC N   densest N-instruction window
    mnemonics ROOT WORKLOAD ENGINE PROC LO HI   mnemonic histogram, hex addresses

`classes` charges each sample to the instruction that precedes the sampled IP,
because the interrupt is taken after the expensive instruction retires. `window`
prints the raw per-IP counts. Both are approximations of where time is spent.
"""
import bisect
import collections
import glob
import re
import sys

LINE = re.compile(r"\s*([0-9a-f]{8,16})[:\s]\s*(.*)$")
STATE_REGISTER = {"swiftvm": "r12", "dynarmic": "r15"}
MXCSR = {"stmxcsr", "ldmxcsr", "vstmxcsr", "vldmxcsr"}
FLAGS = {"pushf", "popf", "pushfq", "popfq", "lahf", "sahf", "seto", "cmc"}
XMM_MOVES = {"movdqa", "movaps", "movapd", "vmovaps", "vmovdqa", "movq", "movd", "vmovq"}


def load_asm(directory):
    text, order = {}, []
    for path in sorted(glob.glob(directory + "*.asm")):
        with open(path, errors="replace") as source:
            for line in source:
                match = LINE.match(line)
                if not match or not match.group(2).strip():
                    continue
                address = int(match.group(1), 16)
                text[address] = match.group(2).strip()
                order.append(address)
    order.sort()
    return text, order


def load_samples(directory):
    counts = collections.Counter()
    with open(directory + "samples.tsv") as source:
        for line in source:
            address, count = line.split()
            counts[int(address, 16)] += int(count)
    return counts


def locate(order, address):
    index = bisect.bisect_right(order, address) - 1
    return index if index >= 0 and address - order[index] <= 16 else -1


def classify(text, engine):
    mnemonic = text.split()[0]
    if mnemonic in MXCSR:
        return "mxcsr"
    if mnemonic in FLAGS:
        return "flags transfer"
    if mnemonic.startswith("j") or mnemonic in ("call", "ret"):
        return "branch"
    if re.search(r"\[%s[+\]]" % STATE_REGISTER[engine], text):
        return "guest state memory"
    if re.search(r"\[rsp", text):
        return "frame memory"
    if "[" in text:
        return "other memory"
    if mnemonic in XMM_MOVES:
        return "xmm move"
    if mnemonic == "mov":
        return "gpr move"
    if mnemonic[:3] in ("add", "sub", "mul", "div") and mnemonic[-2:] in ("pd", "ps", "sd", "ss"):
        return "fp arithmetic"
    return "other register op"


def classes(root, workloads):
    for workload in workloads:
        for engine in ("swiftvm", "dynarmic"):
            totals, samples = collections.Counter(), 0
            for process in sorted(glob.glob(f"{root}/{workload}/{engine}/*/")):
                text, order = load_asm(process)
                for address, count in load_samples(process).items():
                    samples += count
                    index = locate(order, address)
                    if index < 0:
                        totals["outside generated code"] += count
                    else:
                        totals[classify(text[order[max(index - 1, 0)]], engine)] += count
            print(f"{workload} / {engine}: {samples} samples")
            for name, count in totals.most_common():
                print(f"  {count / samples * 100:5.1f}%  {name}")


def window(root, workload, engine, process, size):
    directory = f"{root}/{workload}/{engine}/{process}/"
    text, order = load_asm(directory)
    per = [0] * len(order)
    total = 0
    for address, count in load_samples(directory).items():
        total += count
        index = locate(order, address)
        if index >= 0:
            per[index] += count
    best, start, running = -1, 0, sum(per[:size])
    for index in range(len(order) - size):
        if running > best:
            best, start = running, index
        running += per[index + size] - per[index]
    print(f"{workload} / {engine} / process {process}: {total} samples, "
          f"densest {size}-instruction window holds {best} ({best / total * 100:.1f}%)")
    for index in range(start, min(len(order), start + size)):
        address = order[index]
        print(f"{per[index]:5d}  {address & 0xfffff:05x}  {text[address][:84]}")


def mnemonics(root, workload, engine, process, low, high):
    text, order = load_asm(f"{root}/{workload}/{engine}/{process}/")
    selected = [text[a].split()[0] for a in order if low <= a <= high]
    print(f"{workload} / {engine} / process {process}: {len(selected)} instructions "
          f"in [{low:x}, {high:x}]")
    for name, count in collections.Counter(selected).most_common():
        print(f"{count:6d}  {name}")


def main(argv):
    if len(argv) >= 3 and argv[1] == "classes":
        names = argv[3:] or ["integer", "memory", "shift", "fp", "compute",
                             "compute-matrix", "compute-sha", "compute-sort"]
        classes(argv[2], names)
    elif len(argv) == 7 and argv[1] == "window":
        window(argv[2], argv[3], argv[4], argv[5], int(argv[6]))
    elif len(argv) == 8 and argv[1] == "mnemonics":
        mnemonics(argv[2], argv[3], argv[4], argv[5], int(argv[6], 16), int(argv[7], 16))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
