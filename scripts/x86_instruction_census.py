#!/usr/bin/env python3
"""Audit distorm mnemonics against SwiftVM's x86 decoder.

The report is intentionally generated from the checked-out distorm enum and
the decoder switch cases.  The classification sets below are policy: they say
which currently-unhandled instructions belong to the advertised SSE2 baseline,
which are ordinary user-mode instructions, and which are deliberately outside
the current guest contract.
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import shutil
import subprocess
import sys


REPO = pathlib.Path(__file__).resolve().parents[1]
MNEMONICS = REPO / "source/runtime/externals/distorm/mnemonics.h"
FRONTEND = REPO / "source/runtime/frontend/x86"

DEFAULT_FIXTURES = (
    "real_hello_x86_64",
    "real_hello_musl_x86_64",
    "real_busy_x86_64",
    "real_busy_musl_x86_64",
)

# Legal user-mode instructions, but architecturally unavailable in 64-bit mode
# (or useful only to the unsupported 32-bit compatibility frontend).
LEGACY32 = {
    "I_AAA", "I_AAD", "I_AAM", "I_AAS", "I_ARPL", "I_BOUND", "I_CALL_FAR",
    "I_DAA", "I_DAS", "I_INTO", "I_JMP_FAR", "I_LDS", "I_LES", "I_SALC",
    "I_SYSENTER", "I_SYSEXIT",
}

# Ordinary user-mode/base-ISA holes.  These are implementation candidates even
# when a particular opcode is handled as a deliberate #UD.
USERLAND_BASIC = {
    "I_CLC", "I_CLD", "I_CMC", "I_INT", "I_INT1", "I_MOVBE", "I_POPF",
    "I_PREFETCHW", "I_PUSHF", "I_RCL", "I_RCR", "I_RDRAND", "I_RDTSC",
    "I_RDTSCP", "I_SHLD", "I_SHRD", "I_STC", "I_STD", "I_UD2", "I_XGETBV",
    "I_XLAT",
} | LEGACY32

# Missing SSE/SSE2 instructions while CPUID advertises SSE+SSE2.
# Predicate-specific CMP* names are real distorm opcodes, not presentation
# aliases, so every one must reach the decoder.
SSE2_BASELINE = {
    "I_ADDSD", "I_COMISD", "I_COMISS", "I_CVTDQ2PD", "I_CVTDQ2PS",
    "I_CVTPD2DQ", "I_CVTPD2PS", "I_CVTPS2DQ", "I_CVTPS2PD",
    "I_CVTSD2SI", "I_CVTSS2SI", "I_CVTTPD2DQ", "I_CVTTPS2DQ", "I_DIVSD",
    "I_MASKMOVDQU", "I_MAXPD", "I_MAXPS", "I_MAXSD", "I_MAXSS", "I_MINPD",
    "I_MINPS", "I_MINSD", "I_MINSS", "I_MOVAPD", "I_MOVNTI", "I_MOVNTPD",
    "I_MOVUPD", "I_MULSD", "I_PACKSSDW", "I_PACKSSWB", "I_PACKUSWB",
    "I_PADDSB", "I_PADDSW", "I_PADDUSB", "I_PADDUSW", "I_PMULHUW",
    "I_PMULHW", "I_PSUBSB", "I_PSUBSW", "I_PSUBUSB", "I_PSUBUSW",
    "I_RCPPS", "I_RCPSS", "I_RSQRTPS", "I_RSQRTSS", "I_SQRTPD",
    "I_SQRTPS", "I_SQRTSD", "I_SQRTSS", "I_SUBSD", "I_UNPCKHPD",
    "I_UNPCKHPS", "I_UNPCKLPD", "I_UNPCKLPS",
}
for kind in ("EQ", "LT", "LE", "UNORD", "NEQ", "NLT", "NLE", "ORD"):
    for shape in ("PD", "PS", "SD", "SS"):
        SSE2_BASELINE.add(f"I_CMP{kind}{shape}")

# These forms specifically read/write MMX registers.  SwiftVM does not expose
# CPUID MMX after this audit, so they are legal user-mode instructions but
# outside the advertised guest contract.  Their XMM counterparts remain in
# SSE2_BASELINE above.
MMX_ONLY = {
    "I_CVTPD2PI", "I_CVTPI2PD", "I_CVTPI2PS", "I_CVTPS2PI",
    "I_CVTTPD2PI", "I_CVTTPS2PI", "I_MASKMOVQ", "I_MOVDQ2Q",
    "I_MOVNTQ", "I_MOVQ2DQ", "I_PSHUFW",
}

# Instructions which require CPL0 (or a kernel-managed execution environment)
# in the guest model.  SGDT/SIDT/SLDT/SMSW/STR are included here because UMIP
# may make them privileged and SwiftVM exposes no descriptor-table model.
PRIVILEGED = {
    "I_CLGI", "I_CLI", "I_CLTS", "I_GETSEC", "I_IN", "I_INVD", "I_INVEPT",
    "I_INVLPG", "I_INVLPGA", "I_INVPCID", "I_INVVPID", "I_IRET", "I_LGDT",
    "I_LIDT", "I_LLDT", "I_LMSW", "I_LTR", "I_MONITOR", "I_MWAIT", "I_OUT",
    "I_OUTS", "I_RDMSR", "I_RDPMC", "I_RSM", "I_SGDT", "I_SIDT", "I_SKINIT",
    "I_SLDT", "I_SMSW", "I_STGI", "I_STI", "I_STR", "I_SWAPGS", "I_SYSRET",
    "I_WBINVD", "I_WRMSR", "I_XSETBV",
    "I_VMCALL", "I_VMCLEAR", "I_VMFUNC", "I_VMLAUNCH", "I_VMLOAD",
    "I_VMMCALL", "I_VMPTRLD", "I_VMPTRST", "I_VMREAD", "I_VMRESUME",
    "I_VMRUN", "I_VMSAVE", "I_VMWRITE", "I_VMXOFF", "I_VMXON",
}

X87_SCOPE = {
    "I_FBLD", "I_FBSTP", "I_FCMOVB", "I_FCMOVBE", "I_FCMOVE", "I_FCMOVNB",
    "I_FCMOVNBE", "I_FCMOVNE", "I_FCMOVNU", "I_FCMOVU", "I_FEDISI",
    "I_FEMMS", "I_FENI", "I_FNSAVE", "I_FRSTOR", "I_FSAVE", "I_FSETPM",
}

XSAVE_SCOPE = {
    "I_XRSTOR", "I_XRSTOR64", "I_XSAVE", "I_XSAVE64", "I_XSAVEOPT",
    "I_XSAVEOPT64",
}

CONCURRENT_SCOPE = {"I_CMPXCHG16B"}


def enum_mnemonics() -> set[str]:
    return set(re.findall(r"\b(I_[A-Z0-9_]+)\s*=", MNEMONICS.read_text()))


def handled_cases() -> set[str]:
    result: set[str] = set()
    for source in FRONTEND.glob("*.cc"):
        result.update(re.findall(r"\bcase\s+(I_[A-Z0-9_]+)\s*:", source.read_text()))
    return result


def classify(name: str, handled: set[str]) -> tuple[str, str]:
    if name in handled:
        return "handled", "keep"
    if name in PRIVILEGED:
        return "privileged/ring0", "skip: no guest kernel/system-state model"
    if name in MMX_ONLY:
        return "userland-exotic", "skip: MMX CPUID bit is hidden; XMM form is covered"
    if name in SSE2_BASELINE:
        return "SSE2-baseline-adjacent", "implement: advertised baseline"
    if name in USERLAND_BASIC:
        if name in LEGACY32:
            return "userland-basic", "N/A: invalid/compat-only; all callers decode 64-bit"
        return "userland-basic", "implement (or architecturally coherent #UD)"
    if name in CONCURRENT_SCOPE:
        return "userland-basic", "cross-branch TODO: do not touch here"
    if name in XSAVE_SCOPE:
        return "userland-exotic", "skip: CPUID hides XSAVE/OSXSAVE; state format is large"
    if name in X87_SCOPE:
        return "userland-exotic", "skip here: concurrent x87 scope"
    if name in {"I_RDFSBASE", "I_RDGSBASE", "I_WRFSBASE", "I_WRGSBASE"}:
        return "userland-exotic", "skip: CPUID hides FSGSBASE"
    if name in {"I_XABORT", "I_XBEGIN", "I_XEND"}:
        return "userland-exotic", "skip: CPUID hides TSX"
    if name in {"I_LAR", "I_LFS", "I_LGS", "I_LSL", "I_LSS", "I_VERR", "I_VERW"}:
        return "userland-exotic", "skip: obsolete segmentation/protection query"
    if name == "I_UNDEFINED":
        return "userland-exotic", "skip: distorm sentinel, not an instruction"
    if name.startswith("I_V"):
        return "userland-exotic", "skip: AVX/VEX state is outside SSE2 baseline"
    if name.startswith(("I_AES", "I_PCLMUL")):
        return "userland-exotic", "skip: CPUID hides AES/PCLMUL"
    if name.startswith(("I_PF", "I_PI2", "I__3DNOW", "I_PSWAP")):
        return "userland-exotic", "skip: 3DNow! vendor extension is not advertised"
    return "userland-exotic", "skip: post-SSE2 or vendor extension not advertised"


def objdump_path() -> str | None:
    candidates = (
        shutil.which("objdump"),
        shutil.which("llvm-objdump"),
        "/opt/homebrew/opt/binutils/bin/objdump",
    )
    return next((p for p in candidates if p and pathlib.Path(p).exists()), None)


def disassembly_counts(fixtures: list[pathlib.Path]) -> collections.Counter[str]:
    tool = objdump_path()
    if not tool:
        return collections.Counter()
    result: collections.Counter[str] = collections.Counter()
    prefixes = {
        "addr32", "bnd", "cs", "data16", "ds", "es", "fs", "gs", "lock",
        "notrack", "rep", "repe", "repne", "repnz", "repz", "ss",
    }
    for fixture in fixtures:
        text = subprocess.run(
            [tool, "-d", str(fixture)], check=True, text=True, capture_output=True
        ).stdout
        for line in text.splitlines():
            # GNU objdump prints address, bytes, and assembly in tab-separated
            # columns.  Long encodings have byte-only continuation lines;
            # requiring the third column avoids treating a trailing byte as a
            # mnemonic.
            columns = line.split("\t")
            if len(columns) < 3 or not re.match(r"^\s*[0-9a-f]+:", columns[0]):
                continue
            words = columns[2].strip().split()
            if not words:
                continue
            mnemonic = words[0].lower()
            index = 1
            while mnemonic in prefixes and index < len(words):
                mnemonic = words[index].lower()
                index += 1
            result[mnemonic] += 1
    return result


def normalize_objdump(mnemonic: str) -> str | None:
    aliases = {
        "cltd": "I_CDQ", "cltq": "I_CDQE", "cqto": "I_CQO", "movabs": "I_MOV",
        "endbr64": None, "endbr32": None, "incsspq": None, "incsspd": None,
        "rdsspq": None, "rdsspd": None,
        "je": "I_JZ", "jne": "I_JNZ", "cmove": "I_CMOVZ",
        "cmovne": "I_CMOVNZ", "sete": "I_SETZ", "setne": "I_SETNZ",
        "fildll": "I_FILD", "fistpll": "I_FISTP", "flds": "I_FLD",
        "fldt": "I_FLD", "fmuls": "I_FMUL", "fstpt": "I_FSTP",
        "fwait": "I_WAIT",
    }
    if mnemonic in aliases:
        return aliases[mnemonic]
    if mnemonic.startswith("nop"):
        return "I_NOP"
    if mnemonic.startswith("ret"):
        return "I_RET"
    if mnemonic.startswith("call"):
        return "I_CALL"
    if mnemonic in {"movsb", "movsw", "movsl", "movsq"}:
        return "I_MOVS"
    if re.fullmatch(r"mov[sz][bwlq]{2}", mnemonic):
        return "I_MOVSX" if mnemonic[3] == "s" else "I_MOVZX"
    if mnemonic == "movslq":
        return "I_MOVSXD"
    # AT&T size suffixes on ordinary integer instructions are not part of the
    # distorm mnemonic.
    if mnemonic[-1:] in "bwlq":
        base = mnemonic[:-1].upper()
        if f"I_{base}" in enum_mnemonics():
            return f"I_{base}"
    return f"I_{mnemonic.upper()}"


def write_report(path: pathlib.Path, fixtures: list[pathlib.Path]) -> None:
    all_names = enum_mnemonics()
    handled = handled_cases()
    counts = collections.Counter(classify(name, handled)[0] for name in all_names)
    disasm = disassembly_counts(fixtures)
    observed_missing: list[tuple[str, int, str]] = []
    decoder_blind: list[tuple[str, int]] = []
    for mnemonic, count in sorted(disasm.items()):
        normalized = normalize_objdump(mnemonic)
        if normalized is None:
            continue
        if normalized not in all_names:
            decoder_blind.append((mnemonic, count))
        elif normalized not in handled:
            observed_missing.append((mnemonic, count, normalized))

    lines = [
        "# x86 distorm instruction coverage census",
        "",
        "Generated by `scripts/x86_instruction_census.py` from the current worktree.",
        "",
        "## Summary",
        "",
        f"- distorm enum entries: {len(all_names)}",
        f"- decoder switch cases: {len(handled)}",
        f"- unhandled enum entries: {len(all_names - handled)}",
    ]
    for category in (
        "handled", "userland-basic", "SSE2-baseline-adjacent",
        "privileged/ring0", "userland-exotic",
    ):
        lines.append(f"- {category}: {counts[category]}")

    lines += [
        "",
        "The repository has a 32-bit ABI descriptor and mode-aware decoder code, but every",
        "constructor call in tests and translators passes `is_64bit=true`; legacy-only",
        "instructions are therefore marked N/A rather than implemented.",
        "",
        "A direct probe of the checked-in distorm confirms that AAA/AAD/AAM/AAS,",
        "BOUND, DAA/DAS, INTO, and SALC decode as `I_UNDEFINED` in 64-bit mode;",
        "opcode 63 decodes as MOVSXD rather than ARPL.  SYSENTER/SYSEXIT still decode",
        "in long mode, but SwiftVM has no 32-bit Linux guest entry path or compat vDSO.",
        "",
        "## CPUID coherence audit (before implementation)",
        "",
        "| advertised feature | current decoder state | finding |",
        "|---|---|---|",
        "| FPU (leaf 1 EDX.0) | core x87 is handled, 16 enum opcodes remain | "
        "landmine, but excluded here by the concurrent x87 scope |",
        "| CMOV (leaf 1 EDX.15) | handled | coherent |",
        "| MMX (leaf 1 EDX.23) | several pack/saturating/move opcodes missing | "
        "landmine; included in SSE2-baseline-adjacent |",
        "| FXSR (leaf 1 EDX.24) | FXSAVE/FXRSTOR handled | coherent |",
        "| SSE (leaf 1 EDX.25) | scalar compare/conversion/min/max/sqrt gaps | "
        "landmine; included in SSE2-baseline-adjacent |",
        "| SSE2 (leaf 1 EDX.26) | 97 combined MMX/SSE/SSE2 enum gaps | "
        "landmine; implement |",
        "| TSC (leaf 1 EDX.4) | not advertised; RDTSC missing | hidden coherently, "
        "but real fixtures contain RDTSC |",
        "| CX8 (leaf 1 EDX.8) | not advertised; CMPXCHG8B handled | safe "
        "under-advertising |",
        "| SEP (leaf 1 EDX.11) | not advertised; SYSENTER/SYSEXIT missing | "
        "coherent; 32-bit compat is not wired |",
        "| CX16 (leaf 1 ECX.13) | not advertised; CMPXCHG16B missing | "
        "cross-branch TODO only |",
        "| MOVBE (leaf 1 ECX.22) | not advertised; decoder missing | hidden "
        "coherently; implementation candidate |",
        "| XSAVE/OSXSAVE (leaf 1 ECX.26/.27) | not advertised; XGETBV and "
        "XSAVE family missing | coherent; keep hidden and make XGETBV #UD |",
        "| RDRAND (leaf 1 ECX.30) | not advertised; decoder missing | hidden "
        "coherently; advertise only after implementation |",
        "| SYSCALL (leaf 0x80000001 EDX.11) | not advertised; SYSCALL handled | "
        "safe under-advertising |",
        "| RDTSCP (leaf 0x80000001 EDX.27) | not advertised; decoder missing | "
        "hidden coherently; advertise only after implementation |",
        "",
        "`RDSEED` is not present in this distorm snapshot at all: `0F C7 /7` is",
        "returned as `I_UNDEFINED` of size 1 in both 32- and 64-bit decode.  Supporting",
        "it requires a raw-byte predecode (as used for CET) or a distorm update.",
        "",
        "## CPUID coherence audit (current worktree)",
        "",
        "| advertised feature | decoder/runtime state | result |",
        "|---|---|---|",
        "| TSC + RDTSCP | RDTSC/RDTSCP use one monotonic virtual 1 GHz counter; "
        "CPUID.15H reports 1 GHz and TSC_AUX is fixed at 0 | coherent |",
        "| CX8 | CMPXCHG8B already handled | coherent |",
        "| CMOV | handled | coherent |",
        "| FXSR | FXSAVE/FXRSTOR host helpers already handled | coherent |",
        "| SSE + SSE2 | all non-MMX entries classified as baseline-adjacent are "
        "handled | coherent within the declared baseline |",
        "| MOVBE | load/store decode through ByteSwap IR | coherent |",
        "| RDRAND + RDSEED | both decode; RDSEED has a raw-byte predecoder for "
        "the old distorm snapshot | coherent |",
        "| SYSCALL + NX + LM | SYSCALL/user-mode long-mode execution is handled; "
        "SYSRET remains guest-kernel-only | coherent for userland |",
        "| MMX | not advertised; MMX-register-only leftovers are explicitly "
        "out of scope | coherent |",
        "| XSAVE/OSXSAVE | not advertised; XGETBV raises #UD; XSAVE/XRSTOR skipped | "
        "coherent |",
        "| SEP | not advertised; no 32-bit compat frontend | coherent |",
        "| CX16 | not advertised | cross-branch TODO only; CMPXCHG16B untouched |",
        "",
        "## Test-fixture disassembly: mnemonics not handled by the decoder",
        "",
        "| objdump mnemonic | count | distorm enum |",
        "|---|---:|---|",
    ]
    lines.extend(f"| `{m}` | {n} | `{e}` |" for m, n, e in observed_missing)
    lines += [
        "",
        "This is a static whole-binary census, so AVX/TSX/XSAVE counts include",
        "glibc ifunc alternatives that current CPUID leaves make unreachable.",
        "",
        "## Test-fixture disassembly: decoder-version blind spots",
        "",
        "These mnemonics are emitted by the system objdump but have no enum in this old",
        "distorm snapshot (some are already recognized by SwiftVM's raw-byte predecoder).",
        "",
        "| objdump mnemonic | count |",
        "|---|---:|",
    ]
    lines.extend(f"| `{m}` | {n} |" for m, n in decoder_blind)
    lines += [
        "",
        "## Full enum census",
        "",
        "| mnemonic | decoder | classification | action |",
        "|---|---|---|---|",
    ]
    for name in sorted(all_names):
        category, action = classify(name, handled)
        lines.append(
            f"| `{name[2:]}` | {'handled' if name in handled else 'missing'} | "
            f"{category} | {action} |"
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--report", type=pathlib.Path,
        default=REPO / "docs/x86-instruction-census.md",
    )
    parser.add_argument("fixtures", nargs="*", type=pathlib.Path)
    args = parser.parse_args()
    fixtures = args.fixtures or [
        REPO / "source/translator/linux/tests" / name for name in DEFAULT_FIXTURES
    ]
    missing = [path for path in fixtures if not path.exists()]
    if missing:
        print("missing fixture(s): " + ", ".join(map(str, missing)), file=sys.stderr)
        return 2
    write_report(args.report, fixtures)
    print(args.report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
