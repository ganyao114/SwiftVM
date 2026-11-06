#!/usr/bin/env python3
"""Inventory current owned x86 names, selected VEX encodings and semantic entries.

This is a source inventory, not an ISA/form coverage test or a CPUID proof.
The optional ELF census counts static disassembly, never executed instructions.
"""

from __future__ import annotations

import argparse
import collections
import hashlib
import pathlib
import re
import shutil
import subprocess
import sys


REPO = pathlib.Path(__file__).resolve().parents[1]
FRONTEND = REPO / "source/runtime/frontend/x86"
BACKEND = REPO / "source/runtime/backend/arm64"
MNEMONICS = REPO / "source/runtime/externals/distorm/mnemonics.h"
VOCABULARY = FRONTEND / "opcodes.inc"
VEX_TABLE = FRONTEND / "detail/vex_opcodes.inc"
IR_SCHEMA = REPO / "source/runtime/ir/opcodes.inc"
DEFAULT_FIXTURES = (
    "real_hello_x86_64",
    "real_hello_musl_x86_64",
    "real_busy_x86_64",
    "real_busy_musl_x86_64",
)

# Classify absent entries by execution scope, never by a hidden CPUID bit.
LEGACY32 = set(
    "AAA AAD AAM AAS ARPL BOUND CALL_FAR DAA DAS INTO JMP_FAR LDS LES SALC SYSENTER SYSEXIT".split()
)
TRAPS = set("UD2 INT INT1 INT3 ICEBP".split())
PRIVILEGED = set(
    """CLGI CLI CLTS GETSEC IN INVD INVEPT INVLPG INVLPGA INVPCID INVVPID IRET
LGDT LIDT LLDT LMSW LTR MONITOR MWAIT OUT OUTS RDMSR RDPMC RSM SGDT SIDT SKINIT SLDT SMSW
STGI STI STR SWAPGS SYSRET WBINVD WRMSR XSETBV HLT VMCALL VMCLEAR VMFUNC VMLAUNCH VMLOAD
VMMCALL VMPTRLD VMPTRST VMREAD VMRESUME VMRUN VMSAVE VMWRITE VMXOFF VMXON""".split()
)
MMX_NAMES = set(
    "EMMS CVTPD2PI CVTPI2PD CVTPI2PS CVTPS2PI CVTTPD2PI CVTTPS2PI MASKMOVQ MOVDQ2Q MOVNTQ MOVQ2DQ PSHUFW".split()
)
SSE2_BASELINE = set(
    """ADDSD COMISD COMISS CVTDQ2PD CVTDQ2PS CVTPD2DQ CVTPD2PS CVTPS2DQ CVTPS2PD
CVTSD2SI CVTSS2SI CVTTPD2DQ CVTTPS2DQ DIVSD MASKMOVDQU MAXPD MAXPS MAXSD MAXSS MINPD MINPS
MINSD MINSS MOVAPD MOVNTI MOVNTPD MOVUPD MULSD PACKSSDW PACKSSWB PACKUSWB PADDSB PADDSW
PADDUSB PADDUSW PMULHUW PMULHW PSUBSB PSUBSW PSUBUSB PSUBUSW RCPPS RCPSS RSQRTPS RSQRTSS
SQRTPD SQRTPS SQRTSD SQRTSS SUBSD UNPCKHPD UNPCKHPS UNPCKLPD UNPCKLPS""".split()
)
SSE2_BASELINE |= {
    f"CMP{relation}{shape}"
    for relation in ("EQ", "LT", "LE", "UNORD", "NEQ", "NLT", "NLE", "ORD")
    for shape in ("PD", "PS", "SD", "SS")
}


def source_label(path: pathlib.Path) -> str:
    return path.relative_to(REPO).as_posix()


def uncomment(text: str) -> str:
    # Leave line breaks intact so source locations remain useful.
    return re.sub(
        r"/\*.*?\*/|//[^\n]*", lambda m: "\n" * m[0].count("\n"), text, flags=re.S
    )


def expand_semantic_macros(text: str) -> str:
    """Expand this tree's function-like case macros, including FMA token paste.

    This deliberately does not evaluate C++ or preprocessor conditions. The
    report describes source entries, including guarded ones, not availability.
    """
    text = uncomment(re.sub(r"\\\r?\n", " ", text))
    definitions = {}
    for match in re.finditer(
        r"^[ \t]*#define\s+(\w+)\(([^)\n]*)\)[ \t]*(.*)$", text, re.M
    ):
        definitions[match[1]] = ([p.strip() for p in match[2].split(",")], match[3])
    text = re.sub(r"^[ \t]*#.*$", "", text, flags=re.M)
    if not definitions:
        return text
    pattern = re.compile(r"\b(" + "|".join(map(re.escape, definitions)) + r")\s*\(")
    for _ in range(10000):
        match = pattern.search(text)
        if not match:
            return text
        start = match.end()
        depth, end, part = 1, start, start
        arguments = []
        while end < len(text) and depth:
            char = text[end]
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            elif char == "," and depth == 1:
                arguments.append(text[part:end].strip())
                part = end + 1
            end += 1
        if depth:
            raise ValueError(f"unterminated semantic macro {match[1]}")
        arguments.append(text[part : end - 1].strip())
        parameters, body = definitions[match[1]]
        if len(parameters) != len(arguments):
            raise ValueError(f"semantic macro argument mismatch: {match[1]}")
        values = dict(zip(parameters, arguments))
        body = re.sub(
            r"\b(?:" + "|".join(map(re.escape, parameters)) + r")\b",
            lambda m: values[m[0]],
            body,
        )
        body = re.sub(r"\s*##\s*", "", body)
        text = text[: match.start()] + body + text[end:]
    raise ValueError("semantic macro expansion did not terminate")


def inventory() -> dict:
    paths = [MNEMONICS, VOCABULARY, VEX_TABLE, IR_SCHEMA, BACKEND / "emit.h"]
    paths += sorted(FRONTEND.glob("semantics*.inc"))
    paths += sorted(BACKEND.glob("emit*.cpp"))
    raw_sources = {path: path.read_bytes() for path in paths}
    sources = {path: value.decode() for path, value in raw_sources.items()}
    vendor = {
        name[2:] for name in re.findall(r"\b(I_[A-Z0-9_]+)\s*=", sources[MNEMONICS])
    }
    owned = {
        name: kind
        for kind, name in re.findall(
            r"^\s*(X86_OP|X86_EXTRA_OP)\(([A-Z0-9_]+)\)", sources[VOCABULARY], re.M
        )
    }
    entries: dict[str, set[str]] = collections.defaultdict(set)
    emitted_ir: set[str] = set()
    ir_names = set(re.findall(r"^\s*INST\((\w+)\s*,", sources[IR_SCHEMA], re.M))
    for path, original in sources.items():
        if (
            not path.name.startswith("semantics")
            or path.name == "semantics_operands.inc"
        ):
            continue  # Prefix admission/operand policy alone is not a semantic entry.
        expanded = expand_semantic_macros(original)
        for name in re.findall(r"\bOp::([A-Z][A-Z0-9_]*)\b", expanded):
            entries[name].add(source_label(path))
        emitted_ir.update(
            set(re.findall(r"\bbuilder(?:\.template)?\.(\w+)\s*\(", expanded))
            & ir_names
        )
        emitted_ir.update(set(re.findall(r"\bOpCode::(\w+)\b", expanded)) & ir_names)
    descriptors = []
    for line, value in enumerate(sources[VEX_TABLE].splitlines(), 1):
        match = re.match(r"\s*VEX\((.*)\)\s*(?://.*)?$", value)
        if not match:
            continue
        fields = [part.strip() for part in match[1].split(",")]
        if len(fields) != 8:
            raise ValueError(f"unexpected VEX descriptor at line {line}")
        descriptors.append((line, fields))
    selected = {
        name
        for _, fields in descriptors
        for name in fields[3:5]
        if name != "Unsupported"
    }
    handle_header = sources[BACKEND / "emit.h"]
    handlers = set(
        re.findall(r"\bHandle\s*\(\s*Op\s*<\s*OpCode::(\w+)\s*>", handle_header)
    )
    structural = set(
        re.findall(
            r"\boperation\.opcode\s*==\s*OpCode::(\w+)", sources[BACKEND / "emit.cpp"]
        )
    ) & {"Nop", "FaultLocation", "EntryPoint"}
    delegates = dict(
        re.findall(
            r"\bHandle\s*\(\s*Op\s*<\s*OpCode::(\w+)\s*>[^{};]*\{\s*(Handle\w+)\s*\(",
            handle_header,
        )
    )
    definitions = collections.defaultdict(set)
    for path, text in sources.items():
        if path.parent != BACKEND:
            continue
        for function in re.findall(r"\bvoid\s+Writer::(Handle\w+)\s*\(", text):
            definitions[function].add(source_label(path))
    return dict(
        vendor=vendor,
        owned=owned,
        entries=entries,
        descriptors=descriptors,
        selected=selected,
        ir_names=ir_names,
        emitted_ir=emitted_ir,
        handlers=handlers,
        structural=structural,
        delegates=delegates,
        definitions=definitions,
        hashes={
            source_label(p): hashlib.sha256(value).hexdigest()
            for p, value in raw_sources.items()
        },
    )


def classify(name: str, entries: dict) -> tuple[str, str]:
    if name in LEGACY32:
        return "legacy32/compat", "outside the current 64-bit guest entry contract"
    if name in PRIVILEGED:
        return (
            "privileged/system-model",
            "requires its real privilege/system-state policy; a name is not support",
        )
    if name in TRAPS:
        return (
            "trap/unsupported-stop",
            "architectural trap or explicit rejection; never count as ordinary completion",
        )
    if name in entries:
        return (
            "handled",
            "has semantic entry/selector; operand forms and runtime services remain conditional",
        )
    if name == "UNDEFINED":
        return "parser-sentinel", "not an executable instruction"
    if name.startswith("K") or name.startswith(
        ("VPTERNLOG", "VPCOMPRESS", "VPEXPAND", "VPMOVM2")
    ):
        return (
            "AVX512/opmask",
            "separate EVEX/AVX512 scope; VEX inventory does not cover it",
        )
    if name in MMX_NAMES:
        return "userland-MMX", "no semantic entry found; hidden CPUID is not a waiver"
    if name.startswith("F"):
        return "userland-x87", "no semantic entry found; inspect the actual x87 form"
    if name in SSE2_BASELINE:
        return (
            "SSE/SSE2-name-gap",
            "no semantic entry found; inspect actual baseline forms",
        )
    if name.startswith("V"):
        return (
            "VEX/vendor-name-gap",
            "not found in current semantics; distinguish VEX form from EVEX or vendor encoding",
        )
    return (
        "userland/extension-name-gap",
        "no semantic entry found; no CPUID or runtime-reachability conclusion",
    )


def objdump_path() -> str | None:
    candidates = (
        shutil.which("objdump"),
        shutil.which("llvm-objdump"),
        "/opt/homebrew/opt/binutils/bin/objdump",
    )
    return next((str(p) for p in candidates if p and pathlib.Path(p).exists()), None)


def disassembly_counts(
    fixtures: list[pathlib.Path],
) -> tuple[collections.Counter, list[str]]:
    counts = collections.Counter()
    if not fixtures:
        return counts, [
            "No ELF fixtures selected; this report contains source inventory only."
        ]
    tool = objdump_path()
    if not tool:
        return counts, [
            "objdump unavailable; requested ELF names were not inventoried."
        ]
    prefixes = set(
        "addr32 bnd cs data16 ds es fs gs lock notrack rep repe repne repnz repz ss".split()
    )
    notes = [f"Disassembler: `{tool}`."]
    for fixture in fixtures:
        result = subprocess.run(
            [tool, "-d", str(fixture)], text=True, capture_output=True
        )
        if result.returncode:
            notes.append(
                f"Disassembly failed for `{fixture}` (exit {result.returncode}): {result.stderr.strip()[:300]}"
            )
            continue
        for line in result.stdout.splitlines():
            # GNU/LLVM: address, byte group, assembly. Ignore byte-only lines.
            columns = line.split("\t")
            if len(columns) < 3 or not re.match(r"^\s*[0-9a-fA-F]+:", columns[0]):
                continue
            words = " ".join(columns[2:]).strip().split()
            if not words:
                continue
            while len(words) > 1 and words[0].lower() in prefixes:
                words.pop(0)
            counts[words[0].lower()] += 1
    return counts, notes


def adjust_objdump(mnemonic: str, names: set[str]) -> str | None:
    aliases = {
        "cltd": "CDQ",
        "cltq": "CDQE",
        "cqto": "CQO",
        "movabs": "MOV",
        "je": "JZ",
        "jne": "JNZ",
        "cmove": "CMOVZ",
        "cmovne": "CMOVNZ",
        "sete": "SETZ",
        "setne": "SETNZ",
        "fildll": "FILD",
        "fistpll": "FISTP",
        "flds": "FLD",
        "fldl": "FLD",
        "fldt": "FLD",
        "fsts": "FST",
        "fstl": "FST",
        "fstps": "FSTP",
        "fstpl": "FSTP",
        "fstpt": "FSTP",
        "fwait": "WAIT",
    }
    if mnemonic in aliases:
        return aliases[mnemonic]
    upper = mnemonic.upper()
    if upper in names:
        return upper
    if mnemonic.startswith("nop"):
        return "NOP"
    if mnemonic.startswith("ret"):
        return "RET"
    if mnemonic.startswith("call"):
        return "CALL"
    if mnemonic in {"movsb", "movsw", "movsl", "movsq"}:
        return "MOVS"
    if re.fullmatch(r"mov[sz][bwlq]{2}", mnemonic):
        return "MOVSX" if mnemonic[3] == "s" else "MOVZX"
    if mnemonic == "movslq":
        return "MOVSXD"
    if mnemonic[-1:] in "bwlq" and mnemonic[:-1].upper() in names:
        return mnemonic[:-1].upper()
    return upper


def write_report(
    path: pathlib.Path,
    fixtures: list[pathlib.Path],
    missing_defaults: list[pathlib.Path],
) -> None:
    data = inventory()
    vendor, owned, entries = data["vendor"], data["owned"], data["entries"]
    all_names = vendor | owned.keys()
    counts = collections.Counter(classify(name, entries)[0] for name in all_names)
    selected = data["selected"]
    lines = [
        "# x86 instruction source inventory",
        "",
        "Generated by `scripts/x86_instruction_census.py` from the current checked-out sources.",
        "",
        "## Meaning and limits",
        "",
        "`handled` means a current shared Semantics entry/selector exists, including expanded case macros. It does **not** mean every operand width, prefix, memory form, CPU model, service or backend shape is supported. Prefix admission alone is excluded.",
        "",
        "Owned vocabulary, parser names, selected VEX descriptors, Semantics entries and ARM64 typed IR handlers are reported independently. No enum/table count proves execution coverage, CPUID coherence or native/Direct equivalence. Static conditional source is not evaluated.",
        "",
        "EVEX/AVX512 is a separate scope. A VEX.128/.256 name such as VADDPS does not imply the same mnemonic's EVEX/masked/512-bit forms. Privileged/system-model, legacy32 and deliberate trap/rejection paths remain separate.",
        "",
        "## Summary",
        "",
        f"- distorm mnemonic names: {len(vendor)}",
        f"- owned X86_OP / X86_EXTRA_OP names: {len(owned)} ({sum(k == 'X86_OP' for k in owned.values())} / {sum(k == 'X86_EXTRA_OP' for k in owned.values())})",
        f"- owned names with a shared semantic entry/selector: {len(owned.keys() & entries.keys())}",
        f"- owned names without a found semantic entry: {len(owned.keys() - entries.keys())}",
        f"- selected VEX descriptor rows / endpoint names: {len(data['descriptors'])} / {len(selected)}",
        f"- selected VEX endpoints absent from owned vocabulary / semantics: {len(selected - owned.keys())} / {len(selected - entries.keys())}",
        f"- ARM64 explicit typed IR Handle registrations: {len(data['handlers'])}",
        f"- frontend-mentioned neutral IR names / without typed-or-structural ARM entry: {len(data['emitted_ir'])} / {len(data['emitted_ir'] - data['handlers'] - data['structural'])}",
        "",
    ]
    for category, count in sorted(counts.items()):
        lines.append(f"- {category}: {count}")
    lines += [
        "",
        "## Name gaps requiring inspection",
        "",
        "These are static lookup gaps, not automatically missing executable forms. MMX/x87/XSAVE/crypto are no longer waived merely because an older CPU model hid a feature.",
        "",
        "| set | names |",
        "|---|---|",
    ]
    for title, values in (
        ("Owned without semantic entry", owned.keys() - entries.keys()),
        ("VEX endpoint without owned name", selected - owned.keys()),
        ("VEX endpoint without semantic entry", selected - entries.keys()),
        (
            "Frontend IR without typed/structural ARM entry",
            data["emitted_ir"] - data["handlers"] - data["structural"],
        ),
    ):
        lines.append(
            f"| {title} | {', '.join(f'`{n}`' for n in sorted(values)) or 'none found'} |"
        )
    lines += [
        "",
        "## Selected VEX encoding descriptors",
        "",
        "Each row is the actual adapter selection, including W-selected endpoints and form/length guards. Further adapter normalization (for example shift groups) and shared semantics still validate operands. A row is not a full-form coverage claim.",
        "",
        "| line | map | pp | opcode | W0/narrow | W1/wide | form | lane bits | guards |",
        "|---:|---:|---:|---|---|---|---|---:|---|",
    ]
    for line, fields in data["descriptors"]:
        lines.append(f"| {line} | " + " | ".join(f"`{f}`" for f in fields) + " |")
    lines += [
        "",
        "## Shared semantics / ARM64 typed IR inventory",
        "",
        "These are neutral IR registration names, not guest instructions. A typed family may reject shapes at runtime; its source presence is not a proof for every argument. Structural nodes are consumed by Writer::Run rather than typed Handle.",
        "",
        "| frontend-mentioned IR | ARM entry | implementation source |",
        "|---|---|---|",
    ]
    for name in sorted(data["emitted_ir"]):
        family = data["delegates"].get(name, "")
        files = data["definitions"].get(family, set())
        status = (
            "typed Handle"
            if name in data["handlers"]
            else "Writer::Run structural"
            if name in data["structural"]
            else "not found"
        )
        lines.append(
            f"| `{name}` | {status} | {', '.join(f'`{n}`' for n in sorted(files)) or '`source/runtime/backend/arm64/emit.h`'} |"
        )
    disasm, notes = disassembly_counts(fixtures)
    lines += [
        "",
        "## Optional ELF static disassembly",
        "",
        "Whole-binary disassembly includes cold libraries and alternate paths. Counts are static instruction locations, not runtime frequencies. Objdump aliases and names alone do not identify all operand forms.",
        "",
    ]
    lines.extend(f"- {note}" for note in notes)
    lines.extend(
        f"- Retired default fixture absent (not scanned): `{fixture}`"
        for fixture in missing_defaults
    )
    lines += [
        "",
        "| objdump name | static count | normalized name | semantic entry | classification |",
        "|---|---:|---|---|---|",
    ]
    for mnemonic, count in sorted(disasm.items()):
        name = adjust_objdump(mnemonic, all_names)
        category = (
            classify(name, entries)[0]
            if name in all_names
            else "not in parser/owned name inventory"
        )
        lines.append(
            f"| `{mnemonic}` | {count} | `{name}` | {'present' if name in entries else 'not found'} | {category} |"
        )
    lines += [
        "",
        "## Execution coverage is separate",
        "",
        "This tool did not build or execute guest/native tests and does not read a source file as a passing test result. Central host/Android run receipts and numerical or fault assertions are the execution evidence. Relevant current test source files are listed only as pointers:",
        "",
    ]
    for test in sorted((REPO / "source/tests/rebuild").glob("*_test.cpp")):
        if test.name.startswith(("x86_", "x87_", "arm64_", "compile_pipeline")):
            lines.append(f"- `{source_label(test)}` (source existence only)")
    lines += [
        "",
        "## Full name inventory",
        "",
        "| name | parser | owned vocabulary | shared semantic entry source | classification | action/limit |",
        "|---|---|---|---|---|---|",
    ]
    for name in sorted(all_names):
        category, action = classify(name, entries)
        source = (
            ", ".join(f"`{value}`" for value in sorted(entries.get(name, set())))
            or "not found"
        )
        lines.append(
            f"| `{name}` | {'distorm' if name in vendor else 'extra/adapter'} | {owned.get(name, 'absent')} | {source} | {category} | {action} |"
        )
    lines += [
        "",
        "## Source identities",
        "",
        "Source contents read for this inventory (not a whole-build identity or passing-test receipt):",
        "",
        "| source | SHA256 |",
        "|---|---|",
    ]
    lines.extend(
        f"| `{name}` | `{digest}` |" for name, digest in sorted(data["hashes"].items())
    )
    lines.append(
        f"| `scripts/x86_instruction_census.py` | `{hashlib.sha256(pathlib.Path(__file__).read_bytes()).hexdigest()}` |"
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--report", type=pathlib.Path, default=REPO / "docs/x86-instruction-census.md"
    )
    parser.add_argument("fixtures", nargs="*", type=pathlib.Path)
    args = parser.parse_args()
    missing_defaults = []
    if args.fixtures:
        missing = [path for path in args.fixtures if not path.is_file()]
        if missing:
            print(
                "missing fixture(s): " + ", ".join(map(str, missing)), file=sys.stderr
            )
            return 2
        fixtures = args.fixtures
    else:
        defaults = [
            REPO / "source/translator/linux/tests" / name for name in DEFAULT_FIXTURES
        ]
        fixtures = [path for path in defaults if path.is_file()]
        missing_defaults = [path for path in defaults if not path.is_file()]
    try:
        write_report(args.report, fixtures, missing_defaults)
    except (OSError, ValueError) as error:
        print(f"census: {error}", file=sys.stderr)
        return 2
    print(args.report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
