#!/usr/bin/env python3
"""Independent Unicorn probe for the versioned .svmcase protocol.

This process imports no SVM library. Install the pinned test-only dependency with
`python -m pip install unicorn==2.1.4`; run.py starts a fresh probe for every case.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys
import time


UNICORN_VERSION = "2.1.4"
MAX_FILE = 1024 * 1024
MAX_MEMORY = 16 * 1024 * 1024
ARENA_PAGE = 65536
ISA_NAMES = {1: "x86_64", 2: "arm64", 3: "arm32"}


class ProtocolError(ValueError):
    pass


class OracleUnsupported(ValueError):
    pass


def initial_state() -> dict:
    return {
        "gpr": [0] * 32, "pc": 0, "flags": 0,
        "vectors": [[0] * 32 for _ in range(32)],
        "fp_control": 0, "fp_status": 0, "tls": [0, 0],
        "x87": [[0] * 10 for _ in range(8)],
        "x87_control": 0x37F, "x87_status": 0, "x87_tag": 0xFFFF,
        "x87_opcode": 0, "x87_ip": 0, "x87_dp": 0,
    }


class Reader:
    def __init__(self, data: bytes):
        if len(data) > MAX_FILE:
            raise ProtocolError("case exceeds 1 MiB")
        self.data, self.offset = data, 0

    def raw(self, size: int) -> bytes:
        if size > len(self.data) - self.offset:
            raise ProtocolError("truncated case")
        result = self.data[self.offset:self.offset + size]
        self.offset += size
        return result

    def number(self, fmt: str) -> int:
        return struct.unpack("<" + fmt, self.raw(struct.calcsize(fmt)))[0]

    def blob(self, maximum: int = MAX_FILE) -> bytes:
        size = self.number("I")
        if size > maximum:
            raise ProtocolError("oversized field")
        return self.raw(size)

    def text(self) -> str:
        return self.blob().decode("utf-8")


def decode_case(data: bytes) -> dict:
    r = Reader(data)
    if r.raw(8) != b"SVMCASE\0" or r.number("I") != 1:
        raise ProtocolError("unsupported case schema")
    case = {"isa": r.number("B"), "profile": r.text(), "form_id": r.text()}
    if case["isa"] not in ISA_NAMES:
        raise ProtocolError("invalid ISA")
    for key in ("seed", "id", "code_address", "terminal_pc"):
        case[key] = r.number("Q")
    case["max_steps"] = r.number("I")
    case["code"] = r.blob(4096)
    state = initial_state()
    state["gpr"] = [r.number("Q") for _ in range(32)]
    state["pc"], state["flags"] = r.number("Q"), r.number("Q")
    state["vectors"] = [list(r.raw(32)) for _ in range(32)]
    state["fp_control"], state["fp_status"] = r.number("I"), r.number("I")
    state["tls"] = [r.number("Q"), r.number("Q")]
    state["x87"] = [list(r.raw(10)) for _ in range(8)]
    for key in ("x87_control", "x87_status", "x87_tag", "x87_opcode"):
        state[key] = r.number("H")
    state["x87_ip"], state["x87_dp"] = r.number("Q"), r.number("Q")
    case["initial"] = state
    count = r.number("I")
    if count > 16:
        raise ProtocolError("too many memory regions")
    case["memory"] = []
    for _ in range(count):
        region = {"address": r.number("Q"), "size": r.number("I"),
                  "protection": r.number("B"), "bytes": r.blob()}
        if len(region["bytes"]) > region["size"] or region["protection"] > 7:
            raise ProtocolError("invalid memory region")
        case["memory"].append(region)
    if sum(m["size"] for m in case["memory"]) > MAX_MEMORY:
        raise ProtocolError("memory exceeds 16 MiB")
    if r.offset != len(data):
        raise ProtocolError("trailing case data")
    if not case["max_steps"] or case["max_steps"] > 1000000:
        raise ProtocolError("invalid instruction budget")
    return case


def encode_case(case: dict) -> bytes:
    parts = [b"SVMCASE\0", struct.pack("<IB", 1, case["isa"])]

    def number(fmt: str, value: int):
        parts.append(struct.pack("<" + fmt, value))

    def blob(value):
        value = bytes(value)
        number("I", len(value))
        parts.append(value)

    for key in ("profile", "form_id"):
        blob(case[key].encode("utf-8"))
    for key in ("seed", "id", "code_address", "terminal_pc"):
        number("Q", case[key])
    number("I", case["max_steps"])
    blob(case["code"])
    state = case["initial"]
    for value in state["gpr"]:
        number("Q", value)
    for key in ("pc", "flags"):
        number("Q", state[key])
    for value in state["vectors"]:
        parts.append(bytes(value))
    for key in ("fp_control", "fp_status"):
        number("I", state[key])
    for value in state["tls"]:
        number("Q", value)
    for value in state["x87"]:
        parts.append(bytes(value))
    for key in ("x87_control", "x87_status", "x87_tag", "x87_opcode"):
        number("H", state[key])
    for key in ("x87_ip", "x87_dp"):
        number("Q", state[key])
    number("I", len(case["memory"]))
    for region in case["memory"]:
        number("Q", region["address"])
        number("I", region["size"])
        number("B", region["protection"])
        blob(region["bytes"])
    result = b"".join(parts)
    decode_case(result)
    return result


def read_case(path: str | Path) -> dict:
    with open(path, "rb") as stream:
        return decode_case(stream.read(MAX_FILE + 1))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_json(path: str | Path, value: dict):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".part")
    temporary.write_text(json.dumps(value, separators=(",", ":")) + "\n")
    temporary.replace(path)


def observation(outcome: str = "host_failure", error: str = "") -> dict:
    return {"protocol_version": 1, "outcome": outcome, "stage": "oracle",
            "error": error, "backend": "unicorn", "state": initial_state(),
            "memory": [], "native_executed": False, "steps": 0,
            "fault_kind": "", "fault_access": "unknown", "fault_pc": 0,
            "fault_address": 0, "fault_address_valid": False,
            "fault_width": 0, "fault_vector": -1}


def _arm32_tls_rw_supported(uc) -> bool:
    """Unicorn 2.1.4 maps TPIDRURW to a deprecated no-op register."""
    from unicorn import arm_const as t
    probe = 0x5A5A5A5A
    try:
        uc.reg_write(t.UC_ARM_REG_C13_C0_2, probe)
        return uc.reg_read(t.UC_ARM_REG_C13_C0_2) == probe
    except Exception:
        return False


def _registers(uc, case: dict, load: bool, arm32_tls_rw: bool = True) -> dict:
    from unicorn import x86_const as x, arm64_const as a, arm_const as t

    isa, source, result = case["isa"], case["initial"], initial_state()

    def register(reg, value):
        if load:
            uc.reg_write(reg, value)
            return value
        return uc.reg_read(reg)

    if isa == 1:
        names = ["RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI"]
        names += [f"R{i}" for i in range(8, 16)]
        for i, name in enumerate(names):
            result["gpr"][i] = register(getattr(x, "UC_X86_REG_" + name), source["gpr"][i])
        result["pc"] = register(x.UC_X86_REG_RIP, source["pc"])
        result["flags"] = register(x.UC_X86_REG_RFLAGS, source["flags"])
        result["fp_control"] = register(x.UC_X86_REG_MXCSR, source["fp_control"])
        for i in range(16):
            value = register(getattr(x, f"UC_X86_REG_YMM{i}"),
                             int.from_bytes(bytes(source["vectors"][i]), "little"))
            result["vectors"][i] = list(value.to_bytes(32, "little"))
        for i, name in enumerate(("FS_BASE", "GS_BASE")):
            result["tls"][i] = register(getattr(x, "UC_X86_REG_" + name), source["tls"][i])
        for key, name in (("x87_control", "FPCW"), ("x87_status", "FPSW"),
                          ("x87_tag", "FPTAG"), ("x87_opcode", "FOP"),
                          ("x87_ip", "FIP"), ("x87_dp", "FDP")):
            result[key] = register(getattr(x, "UC_X86_REG_" + name), source[key])
        for i in range(8):
            raw = bytes(source["x87"][i])
            value = register(getattr(x, f"UC_X86_REG_FP{i}"),
                             (int.from_bytes(raw[:8], "little"), int.from_bytes(raw[8:], "little")))
            result["x87"][i] = list(value[0].to_bytes(8, "little") + value[1].to_bytes(2, "little"))
    elif isa == 2:
        for i in range(31):
            result["gpr"][i] = register(getattr(a, f"UC_ARM64_REG_X{i}"), source["gpr"][i])
        result["gpr"][31] = register(a.UC_ARM64_REG_SP, source["gpr"][31])
        for key, name in (("pc", "PC"), ("flags", "NZCV"),
                          ("fp_control", "FPCR"), ("fp_status", "FPSR")):
            result[key] = register(getattr(a, "UC_ARM64_REG_" + name), source[key])
        for i in range(32):
            value = register(getattr(a, f"UC_ARM64_REG_Q{i}"),
                             int.from_bytes(bytes(source["vectors"][i][:16]), "little"))
            result["vectors"][i][:16] = value.to_bytes(16, "little")
        result["tls"][0] = register(a.UC_ARM64_REG_TPIDR_EL0, source["tls"][0])
    else:
        # CPSR selects the bank and execution mode before loading SP/LR/PC.
        result["flags"] = register(t.UC_ARM_REG_CPSR, source["flags"])
        for i in range(15):
            result["gpr"][i] = register(getattr(t, f"UC_ARM_REG_R{i}"), source["gpr"][i])
        result["pc"] = register(t.UC_ARM_REG_PC, source["pc"])
        result["fp_control"] = register(t.UC_ARM_REG_FPSCR, source["fp_control"])
        for i in range(16):
            value = register(getattr(t, f"UC_ARM_REG_Q{i}"),
                             int.from_bytes(bytes(source["vectors"][i][:16]), "little"))
            result["vectors"][i][:16] = value.to_bytes(16, "little")
        for i, name in enumerate(("C13_C0_2", "C13_C0_3")):
            if i == 0 and not arm32_tls_rw:
                # Preserve the declared input; any instruction that writes this
                # register will therefore surface as a mismatch, not a pass.
                result["tls"][i] = source["tls"][i]
            else:
                result["tls"][i] = register(getattr(t, "UC_ARM_REG_" + name), source["tls"][i])
    return result


def execute(case: dict, timeout_ms: int = 2000) -> dict:
    import unicorn as u
    from unicorn import x86_const as x, arm64_const as a, arm_const as t

    if u.__version__ != UNICORN_VERSION:
        raise OracleUnsupported(f"requires Unicorn {UNICORN_VERSION}; found {u.__version__}")
    expected_profile = {1: "x86_64-user", 2: "a64-user", 3: "armv7-user"}
    if case["profile"] != expected_profile[case["isa"]]:
        raise OracleUnsupported("unknown guest profile: " + case["profile"])
    if case["isa"] == 3 and (case["initial"]["flags"] & 0x21F) != 0x10:
        raise OracleUnsupported("only little-endian ARMv7 user mode is supported")
    isa = case["isa"]
    thumb = isa == 3 and bool(case["initial"]["flags"] & 0x20)
    arch, mode = {1: (u.UC_ARCH_X86, u.UC_MODE_64),
                  2: (u.UC_ARCH_ARM64, u.UC_MODE_ARM),
                  3: (u.UC_ARCH_ARM, u.UC_MODE_THUMB if thumb else u.UC_MODE_ARM)}[isa]
    uc = u.Uc(arch, mode)
    model = {1: x.UC_CPU_X86_HASWELL, 2: a.UC_CPU_ARM64_MAX, 3: t.UC_CPU_ARM_CORTEX_A15}[isa]
    uc.ctl_set_cpu_model(model)
    arm32_tls_rw = _arm32_tls_rw_supported(uc) if isa == 3 else True
    if isa == 2:
        uc.reg_write(a.UC_ARM64_REG_CPACR_EL1, 3 << 20)
    elif isa == 3:
        uc.reg_write(t.UC_ARM_REG_C1_C0_2, 0xF << 20)
        uc.reg_write(t.UC_ARM_REG_FPEXC, 1 << 30)

    page = uc.ctl_get_page_size()
    code_start = case["code_address"] & -ARENA_PAGE
    code_end = (case["code_address"] + len(case["code"]) + ARENA_PAGE - 1) & -ARENA_PAGE
    if code_end <= code_start or code_end - code_start > 2 * ARENA_PAGE:
        raise ProtocolError("invalid code arena")
    mappings = [(code_start, code_end)]
    uc.mem_map(code_start, code_end - code_start, u.UC_PROT_ALL)
    uc.mem_write(case["code_address"], case["code"])
    uc.mem_protect(code_start, code_end - code_start, u.UC_PROT_READ | u.UC_PROT_EXEC)
    for region in case["memory"]:
        start, end = region["address"], region["address"] + region["size"]
        if not region["size"] or start % page or end % page:
            raise OracleUnsupported("memory mapping does not match Unicorn page granularity")
        if any(start < prior_end and end > prior_start for prior_start, prior_end in mappings):
            raise OracleUnsupported("overlapping regions require an explicit alias contract")
        uc.mem_map(start, region["size"], u.UC_PROT_ALL)
        uc.mem_write(start, bytes(region["bytes"]))
        uc.mem_protect(start, region["size"], region["protection"])
        mappings.append((start, end))
    _registers(uc, case, True, arm32_tls_rw)
    result = observation()
    count = 0
    pc_reg = {1: x.UC_X86_REG_RIP, 2: a.UC_ARM64_REG_PC, 3: t.UC_ARM_REG_PC}[isa]

    def before_instruction(engine, address, _size, _data):
        nonlocal count
        if address == case["terminal_pc"]:
            result["outcome"] = "terminal"
            engine.emu_stop()
        elif count >= case["max_steps"]:
            result["outcome"] = "budget"
            result["error"] = "oracle instruction-hook budget exhausted"
            engine.emu_stop()
        else:
            count += 1

    access_names = {
        u.UC_MEM_READ_UNMAPPED: "read", u.UC_MEM_READ_PROT: "read",
        u.UC_MEM_WRITE_UNMAPPED: "write", u.UC_MEM_WRITE_PROT: "write",
        u.UC_MEM_FETCH_UNMAPPED: "execute", u.UC_MEM_FETCH_PROT: "execute",
    }

    def invalid_memory(engine, access, address, size, _value, _data):
        pc = engine.reg_read(pc_reg)
        if access_names[access] == "execute" and address == case["terminal_pc"] and pc == address:
            result["outcome"] = "terminal"
        else:
            result.update(outcome="fault", fault_kind="memory", fault_access=access_names[access],
                          fault_pc=pc, fault_address=address, fault_address_valid=True,
                          fault_width=size, fault_vector=14 if isa == 1 else -1)
        return False

    def interrupt(engine, vector, _data):
        if isa == 1 and vector in (0, 6, 13, 14, 16, 19):
            kind = {0: "divide", 6: "illegal_instruction", 13: "memory", 14: "memory",
                    16: "floating_point", 19: "floating_point"}[vector]
            result.update(outcome="fault", fault_kind=kind, fault_pc=engine.reg_read(pc_reg),
                          fault_vector=vector)
        else:
            result.update(outcome="unavailable", error=f"oracle_unsupported: interrupt {vector} has no exact exception/service adapter")
        engine.emu_stop()

    uc.hook_add(u.UC_HOOK_CODE, before_instruction)
    uc.hook_add(u.UC_HOOK_MEM_INVALID, invalid_memory)
    uc.hook_add(u.UC_HOOK_INTR, interrupt)
    started = time.monotonic()
    try:
        if case["initial"]["pc"] == case["terminal_pc"]:
            result["outcome"] = "terminal"
        else:
            uc.emu_start(case["initial"]["pc"] | thumb, case["terminal_pc"], timeout=timeout_ms * 1000)
            if result["outcome"] == "host_failure":
                if uc.reg_read(pc_reg) == case["terminal_pc"]:
                    result["outcome"] = "terminal"
                elif uc.query(u.UC_QUERY_TIMEOUT):
                    result.update(outcome="budget", error="oracle wall-clock budget exhausted")
                else:
                    result.update(outcome="unavailable", error="oracle_inconclusive: engine stopped outside the declared boundary")
    except u.UcError as error:
        if result["outcome"] == "host_failure":
            result.update(outcome="unavailable", error="oracle_unsupported: " + str(error))
    result["state"] = _registers(uc, case, False, arm32_tls_rw)
    result["memory"] = [{"address": region["address"], "size": region["size"],
                         "protection": region["protection"],
                         "bytes": list(uc.mem_read(region["address"], region["size"]))}
                        for region in case["memory"]]
    result["steps"] = count
    library = Path(u.__file__).parent / "lib" / "libunicorn.2.dylib"
    if not library.exists():
        library = Path(u.__file__).parent / "lib" / "libunicorn.so.2"
    result["oracle"] = {
        "family": "qemu-unicorn", "version": u.__version__, "cpu_model": model,
        "isa": ISA_NAMES[isa], "thumb": thumb, "page_size": page,
        "adapter_sha256": sha256(Path(__file__)),
        "library_sha256": sha256(library) if library.exists() else "unavailable",
        "hook_count_is_retired_instruction_count": False,
        "arm32_tls_rw": "verified" if arm32_tls_rw else
                        "unavailable: Unicorn 2.1.4 TPIDRURW is a no-op; declared input preserved",
        "elapsed_ms": (time.monotonic() - started) * 1000,
    }
    return result


def self_test() -> dict:
    results = []
    samples = [(1, False, "4801d8", 3), (2, False, "0000018b", 1),
               (3, False, "010080e0", 1), (3, True, "4018", 1)]
    for isa, thumb, hex_code, right in samples:
        code = bytes.fromhex(hex_code)
        case = {"isa": isa, "profile": {1: "x86_64-user", 2: "a64-user", 3: "armv7-user"}[isa],
                "form_id": "oracle-self-test", "seed": 0, "id": len(results),
                "code_address": 0x20000 - len(code), "terminal_pc": 0x20000,
                "code": code, "initial": initial_state(), "memory": [], "max_steps": 8}
        state = case["initial"]
        state.update(pc=case["code_address"], flags=2 if isa == 1 else (0x30 if thumb else 0x10) if isa == 3 else 0xA0000000)
        state["gpr"][0], state["gpr"][right] = 7, 5
        state["fp_control"] = {1: 0xFFC9, 2: 0x3400000, 3: 0xFB00009F}[isa]
        state["fp_status"] = 0x0800009F if isa == 2 else 0
        state["tls"] = [0x12345678, 0 if isa == 2 else 0x76543210]
        state["vectors"][0][:16] = range(16)
        if isa == 1:
            state["vectors"][0][16:] = range(16, 32)
            state["x87"][0] = list(bytes.fromhex("0000000000000080ff3f"))
            state["x87_status"] = 3 << 11
        if encode_case(decode_case(encode_case(case))) != encode_case(case):
            raise AssertionError("case roundtrip changed bytes")
        actual = execute(case)
        if actual["outcome"] != "terminal" or actual["state"]["gpr"][0] != 12 or actual["state"]["pc"] != case["terminal_pc"]:
            raise AssertionError(f"{ISA_NAMES[isa]} terminal/arithmetic self-test: {actual}")
        tls_limited = isa == 3 and actual["oracle"]["arm32_tls_rw"] != "verified"
        for key in ("vectors", "fp_control", "fp_status", "tls", "x87", "x87_control", "x87_status", "x87_tag"):
            if key == "tls" and tls_limited:
                if actual["state"]["tls"][1] != state["tls"][1]:
                    raise AssertionError(f"{ISA_NAMES[isa]} register projection self-test: tls[1]")
                continue
            if actual["state"][key] != state[key]:
                raise AssertionError(f"{ISA_NAMES[isa]} register projection self-test: {key}")
        if isa == 3 and bool(actual["state"]["flags"] & 0x20) != thumb:
            raise AssertionError("Thumb mode was lost at boundary")
        results.append({"isa": ISA_NAMES[isa], "thumb": thumb, "outcome": "passed",
                        "arm32_tls_rw": actual["oracle"].get("arm32_tls_rw", "not_applicable")})
    # A faulting load must remain a fault; the adapter must not lazy-map it.
    fault = copy.deepcopy(case)
    fault["code"] = bytes.fromhex("0868")
    fault["initial"]["gpr"][1] = 0x700000
    actual = execute(fault)
    if actual["outcome"] != "fault" or actual["fault_address"] != 0x700000 or actual["fault_access"] != "read":
        raise AssertionError("unmapped memory self-test failed")
    it = copy.deepcopy(case)
    it["code"] = bytes.fromhex("0120")  # MOVS r0,#1 under an initial false EQ IT.
    it["initial"].update(flags=0x830)
    it["initial"]["gpr"][0] = 7
    actual = execute(it)
    if actual["outcome"] != "terminal" or actual["state"]["gpr"][0] != 7 or actual["state"]["flags"] != 0x30:
        raise AssertionError("initial Thumb IT state/boundary self-test failed")
    return {"protocol_version": 1, "unicorn_version": UNICORN_VERSION, "tests": results,
            "unmapped_memory": "passed", "initial_thumb_it": "passed"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--timeout-ms", type=int, default=2000)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        value = self_test()
    elif args.case:
        try:
            value = execute(read_case(args.case), args.timeout_ms)
        except (ImportError, OracleUnsupported) as error:
            value = observation("unavailable", "oracle_unsupported: " + str(error))
        except (ProtocolError, UnicodeError, struct.error) as error:
            value = observation("host_failure", "case_protocol: " + str(error))
        except Exception as error:
            value = observation("host_failure", type(error).__name__ + ": " + str(error))
    else:
        parser.error("provide --case or --self-test")
    if args.out:
        write_json(args.out, value)
    else:
        print(json.dumps(value, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
