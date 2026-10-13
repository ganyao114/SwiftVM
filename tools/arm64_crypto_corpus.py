"""ARM AES instruction corpus with FIPS 197 tables and GF(256) expectations.

The reference never uses the runtime's AES IR sequence or native crypto APIs.
"""

import argparse
import random
import re
import struct


BASES = {"aese": 0x4E284800, "aesd": 0x4E285800,
         "aesmc": 0x4E286800, "aesimc": 0x4E287800}
# FIPS 197 Table 4, in hexadecimal input order.
SBOX = bytes.fromhex(
    "637c777bf26b6fc53001672bfed7ab76ca82c97dfa5947f0add4a2af9ca472c0"
    "b7fd9326363ff7cc34a5e5f171d8311504c723c31896059a071280e2eb27b275"
    "09832c1a1b6e5aa0523bd6b329e32f8453d100ed20fcb15b6acbbe394a4c58cf"
    "d0efaafb434d338545f9027f503c9fa851a3408f929d38f5bcb6da2110fff3d2"
    "cd0c13ec5f974417c4a77e3d645d197360814fdc222a908846eeb814de5e0bdb"
    "e0323a0a4906245cc2d3ac629195e479e7c8376d8dd54ea96c56f4ea657aae08"
    "ba78252e1ca6b4c6e8dd741f4bbd8b8a703eb5664803f60e613557b986c11d9e"
    "e1f8981169d98e949b1e87e9ce5528df8ca1890dbfe6426841992d0fb054bb16")
INVERSE_SBOX = bytes(SBOX.index(value) for value in range(256))
PLAINTEXT = bytes.fromhex("00112233445566778899aabbccddeeff")
KEY = bytes.fromhex("000102030405060708090a0b0c0d0e0f")
CIPHERTEXT = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")


def multiply(a, b):
    """Shift-and-add in GF(2^8), reducing each step by AES polynomial 0x11b."""
    result = 0
    while b:
        if b & 1:
            result ^= a
        a = ((a << 1) ^ (0x11B if a & 0x80 else 0)) & 255
        b >>= 1
    return result


def mix_columns(data, inverse=False):
    first_row = (14, 11, 13, 9) if inverse else (2, 3, 1, 1)
    result = bytearray(16)
    for column in range(4):
        for row in range(4):
            for term in range(4):
                result[4 * column + row] ^= multiply(data[4 * column + term],
                                                     first_row[(term - row) % 4])
    return bytes(result)


def round_result(old, key, inverse=False):
    substituted = bytes((INVERSE_SBOX if inverse else SBOX)[a ^ b]
                        for a, b in zip(old, key))
    return bytes(substituted[4 * ((column + (-row if inverse else row)) % 4) + row]
                 for column in range(4) for row in range(4))


def round_keys(key):
    """FIPS 197 AES-128 word recurrence; used only to construct full-block code."""
    words = [list(key[offset:offset + 4]) for offset in range(0, 16, 4)]
    rcon = 1
    for index in range(4, 44):
        term = words[-1][:]
        if index % 4 == 0:
            term = [SBOX[value] for value in term[1:] + term[:1]]
            term[0] ^= rcon
            rcon = multiply(rcon, 2)
        words.append([a ^ b for a, b in zip(words[index - 4], term)])
    return [bytes(value for word in words[offset:offset + 4] for value in word)
            for offset in range(0, 44, 4)]


def corpus(case_type):
    import arm64_differential as differential
    cases = []
    init = tuple(word for reg in range(4) for word in
                 (0x9E670000 | ((reg * 2 + 1) << 5) | reg,
                  0x9EAF0000 | ((reg * 2 + 2) << 5) | reg))
    stores = tuple(0x3D8003A0 | (reg << 10) | reg for reg in range(4))
    control_init = (0xD51B443B, 0xD51B441C)  # MSR FPSR/X27, FPCR/X28
    control_read = (0xD53B443B, 0xD53B441C)
    untouched = (bytes.fromhex("0123456789abcdef1032547698badcfe"),
                 bytes.fromhex("f1e2d3c4b5a69788796a5b4c3d2e1f00"))
    rng = random.Random(0xA35F197)
    profiles = [(f"uniform{value:02x}", bytes([value]) * 16) for value in range(256)]
    profiles += [(f"ramp{start:02x}", bytes(range(start, start + 16)))
                 for start in range(0, 256, 16)]
    profiles += [(f"single{lane}_{bit}", bytes((1 << bit) if index == lane else 0
                                               for index in range(16)))
                 for lane in range(16) for bit in range(8)]
    profiles += [(f"random{n}", bytes(rng.randrange(256) for _ in range(16)))
                 for n in range(16)]

    def seeds_for(vectors):
        result = {2 * reg + half + 1: int.from_bytes(vector[half * 8:half * 8 + 8], "little")
                  for reg, vector in enumerate(vectors) for half in range(2)}
        result.update({27: 0x0800009F, 28: 0x01400000})
        return result

    for op, base in BASES.items():
        for profile, data in profiles:
            keys = [("zero", bytes(16)), ("ones", bytes([255]) * 16),
                    ("random", bytes(rng.randrange(256) for _ in range(16)))]
            if op in ("aesmc", "aesimc"):
                keys = [("none", data)]
            for key_name, key in keys:
                for alias in (False, True):
                    old = data if op in ("aese", "aesd") or alias else bytes.fromhex(
                        "aabbccddeeff00112233445566778899")
                    source = key if op in ("aese", "aesd") else data
                    vectors = [old, source, *untouched]
                    word = base | (0 if alias else 1 << 5)
                    name = f"crypto_{op}_{profile}_{key_name}_{'alias' if alias else 'distinct'}"
                    cases.append(case_type(name, control_init + init + (word,) + control_read + stores,
                                           name, seeds_for(vectors), 0xB0000000, "crypto"))

    expanded = round_keys(KEY)
    for inverse in (False, True):
        old = CIPHERTEXT if inverse else PLAINTEXT
        vectors = [old, bytes(16), *untouched]
        words = []
        rounds = [expanded[10]] + [mix_columns(expanded[n], True) for n in range(9, 0, -1)] if inverse else expanded[:10]
        for index, key in enumerate(rounds):
            for reg, chunk in ((9, key[:8]), (10, key[8:])):
                words += differential.mov64(reg, int.from_bytes(chunk, "little"))
            words += [0x9E670121, 0x9EAF0141]  # FMOV D1/X9, V1.D[1]/X10
            words += [BASES["aesd" if inverse else "aese"] | (1 << 5)]
            if index != 9:
                words += [BASES["aesimc" if inverse else "aesmc"]]
        key = expanded[0 if inverse else 10]
        words += differential.mov64(9, int.from_bytes(key[:8], "little"))
        words += differential.mov64(10, int.from_bytes(key[8:], "little"))
        words += [0x9E670121, 0x9EAF0141, 0x6E211C00]  # EOR V0.16B,V0.16B,V1.16B
        name = f"crypto_nist_{'decrypt' if inverse else 'encrypt'}"
        cases.append(case_type(name, control_init + init + tuple(words) + control_read + stores,
                               name, seeds_for(vectors), 0xB0000000, "crypto"))
    return cases


def expected(case):
    vectors = [struct.pack("<QQ", case.seeds[n], case.seeds[n + 1]) for n in (1, 3, 5, 7)]
    if case.name == "crypto_nist_encrypt":
        vectors[0], vectors[1] = CIPHERTEXT, round_keys(KEY)[10]
    elif case.name == "crypto_nist_decrypt":
        vectors[0], vectors[1] = PLAINTEXT, KEY
    else:
        match = re.fullmatch(r"crypto_(aese|aesd|aesmc|aesimc)_.+_(alias|distinct)", case.name)
        if not match:
            raise ValueError(f"Unknown AES case: {case.name}")
        op, alias = match.groups()
        source = vectors[0 if alias == "alias" else 1]
        vectors[0] = (round_result(vectors[0], source, op == "aesd") if op in ("aese", "aesd")
                      else mix_columns(source, op == "aesimc"))
    return {"memory": b"".join(vectors), "fpsr": case.seeds[27], "fpcr": case.seeds[28]}


def verify():
    import arm64_differential as differential
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_INTR
    import unicorn.arm64_const as registers
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = corpus(differential.Case)
    for case in cases:
        elf = differential.make_elf(case)
        code_segment = struct.unpack_from("<IIQQQQQQ", elf, 64)
        data_segment = struct.unpack_from("<IIQQQQQQ", elf, 120)
        code = elf[4096:code_segment[5]]
        if len(list(decoder.disasm(code, differential.CODE_BASE + 4096))) * 4 != len(code):
            raise ValueError(f"{case.name}: Capstone rejected code")
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.ctl_set_cpu_model(registers.UC_CPU_ARM64_MAX)
        cpu.mem_map(differential.CODE_BASE, 65536)
        cpu.mem_map(differential.DATA_BASE, 65536)
        cpu.mem_write(differential.CODE_BASE, elf[:code_segment[5]])
        cpu.mem_write(differential.DATA_BASE, elf[data_segment[2]:data_segment[2] + data_segment[5]])
        snapshots = []
        exited = []

        def interrupt(emulator, number, _):
            syscall = emulator.reg_read(registers.UC_ARM64_REG_X8)
            if syscall == 64:
                snapshots.append(bytes(emulator.mem_read(emulator.reg_read(registers.UC_ARM64_REG_X1),
                                                        emulator.reg_read(registers.UC_ARM64_REG_X2))))
                emulator.reg_write(registers.UC_ARM64_REG_X0, differential.SNAPSHOT_SIZE)
            elif syscall == 93:
                if emulator.reg_read(registers.UC_ARM64_REG_X0):
                    raise ValueError(f"{case.name}: snapshot write failed")
                exited.append(True)
                emulator.emu_stop()
            else:
                raise ValueError(f"{case.name}: unexpected syscall {syscall}")

        cpu.hook_add(UC_HOOK_INTR, interrupt)
        cpu.emu_start(differential.CODE_BASE + 4096, 0, count=2000)
        if len(snapshots) != 1 or not exited:
            raise ValueError(f"{case.name}: incomplete harness execution")
        actual, wanted = differential.decode_snapshot(snapshots[0], case), expected(case)
        if (bytes.fromhex(actual["memory"]) != wanted["memory"] or
                int(actual["registers"]["x27"], 16) != wanted["fpsr"] or
                int(actual["registers"]["x28"], 16) != wanted["fpcr"] or
                int(actual["nzcv"], 16) != case.nzcv):
            raise ValueError(f"{case.name}: independent FIPS/GF math differs from Unicorn: {actual}, {wanted}")
    print(f"PASS {len(cases)} AES independent FIPS/GF math/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
