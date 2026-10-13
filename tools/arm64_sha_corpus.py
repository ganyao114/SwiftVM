"""Bounded ARM SHA corpus; scalar FIPS 180 rounds are the independent oracle."""

import argparse
import hashlib
import random
import struct

MASK = 0xFFFFFFFF
BASES = {"sha1c": 0x5E000000, "sha1p": 0x5E001000, "sha1m": 0x5E002000,
         "sha1su0": 0x5E003000, "sha256h": 0x5E004000, "sha256h2": 0x5E005000,
         "sha256su1": 0x5E006000, "sha1h": 0x5E280800,
         "sha1su1": 0x5E281800, "sha256su0": 0x5E282800}
INITIAL1 = (0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0)
INITIAL256 = (0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
              0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19)
# FIPS 180-4 section 4.2.2, first 32 bits of cube-root fractional parts.
K256 = tuple(int(value, 16) for value in (
    "428a2f98 71374491 b5c0fbcf e9b5dba5 3956c25b 59f111f1 923f82a4 ab1c5ed5 "
    "d807aa98 12835b01 243185be 550c7dc3 72be5d74 80deb1fe 9bdc06a7 c19bf174 "
    "e49b69c1 efbe4786 0fc19dc6 240ca1cc 2de92c6f 4a7484aa 5cb0a9dc 76f988da "
    "983e5152 a831c66d b00327c8 bf597fc7 c6e00bf3 d5a79147 06ca6351 14292967 "
    "27b70a85 2e1b2138 4d2c6dfc 53380d13 650a7354 766a0abb 81c2c92e 92722c85 "
    "a2bfe8a1 a81a664b c24b8b70 c76c51a3 d192e819 d6990624 f40e3585 106aa070 "
    "19a4c116 1e376c08 2748774c 34b0bcb5 391c0cb3 4ed8aa4a 5b9cca4f 682e6ff3 "
    "748f82ee 78a5636f 84c87814 8cc70208 90befffa a4506ceb bef9a3f7 c67178f2").split())


def rol(value, count):
    count %= 32
    return ((value << count) | (value >> ((32 - count) % 32))) & MASK


def sigma(value, kind):
    if kind == 0:
        return rol(value, 25) ^ rol(value, 14) ^ (value >> 3)
    if kind == 1:
        return rol(value, 15) ^ rol(value, 13) ^ (value >> 10)
    if kind == 2:
        return rol(value, 30) ^ rol(value, 19) ^ rol(value, 10)
    return rol(value, 26) ^ rol(value, 21) ^ rol(value, 7)


def operation(op, destination, source, words):
    d, n, m = list(destination), list(source), list(words)
    if op == "sha1h":
        return [rol(n[0], 30), 0, 0, 0]
    if op == "sha1su0":
        stream = d + n + m
        return [stream[i] ^ stream[i + 2] ^ stream[i + 8] for i in range(4)]
    if op == "sha1su1":
        result = [rol(d[i] ^ n[i + 1], 1) for i in range(3)]
        return result + [rol(d[3] ^ result[0], 1)]
    if op == "sha256su0":
        stream = d + n
        return [(stream[i] + sigma(stream[i + 1], 0)) & MASK for i in range(4)]
    if op == "sha256su1":
        # Extend a conceptual schedule using W[t-7] and already-generated W[t-2].
        older = n[1:] + [m[0]]
        previous = m[2:]
        result = []
        for i in range(4):
            result.append((d[i] + older[i] + sigma((previous + result)[i], 1)) & MASK)
        return result
    if op.startswith("sha1"):
        a, b, c, fourth = d
        e = n[0]
        for word in m:
            if op == "sha1c":
                f = (b & c) ^ ((MASK ^ b) & fourth)
            elif op == "sha1p":
                f = b ^ c ^ fourth
            else:
                f = (b & c) ^ (b & fourth) ^ (c & fourth)
            a, b, c, fourth, e = (rol(a, 5) + f + e + word) & MASK, a, rol(b, 30), c, fourth
        return [a, b, c, fourth]
    state = d + n if op == "sha256h" else n + d
    for word in m:
        a, b, c, fourth, e, f, g, h = state
        temporary = (h + sigma(e, 3) + ((e & f) ^ ((MASK ^ e) & g)) + word) & MASK
        state = [(temporary + sigma(a, 2) + ((a & b) ^ (a & c) ^ (b & c))) & MASK,
                 a, b, c, (fourth + temporary) & MASK, e, f, g]
    return state[:4] if op == "sha256h" else state[4:]


def schedule(message, sha256):
    block = message + b"\x80" + bytes(55 - len(message)) + (8 * len(message)).to_bytes(8, "big")
    words = list(struct.unpack(">16I", block))
    for i in range(16, 64 if sha256 else 80):
        words.append((sigma(words[i - 2], 1) + words[i - 7] + sigma(words[i - 15], 0) + words[i - 16]) & MASK
                     if sha256 else rol(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1))
    return words


def corpus(case_type):
    import arm64_differential as differential
    cases = []
    controls = (0xD51B443B, 0xD51B441C)
    capture = (0xD53B443B, 0xD53B441C) + tuple(0x3D8003A0 | (reg << 10) | reg for reg in range(4))
    initialize = tuple(word for reg in range(4) for word in
                       (0x9E670000 | ((reg * 2 + 1) << 5) | reg,
                        0x9EAF0000 | ((reg * 2 + 2) << 5) | reg))
    aliases = ((0, 1, 2), (0, 0, 2), (0, 1, 0), (0, 0, 0),
               (0, 1, 1), (1, 0, 2), (2, 1, 0), (3, 2, 1))
    rng = random.Random(0x5A1802026)

    def seed(vectors):
        result = {2 * reg + half + 1: int.from_bytes(struct.pack("<4I", *vector)[half * 8:half * 8 + 8], "little")
                  for reg, vector in enumerate(vectors) for half in range(2)}
        result.update({27: 0x0800009F, 28: 0x01400000})
        return result

    def add(op, profile, vectors, regs):
        rd, rn, rm = regs
        word = BASES[op] | (rn << 5) | rd
        if op not in ("sha1h", "sha1su1", "sha256su0"):
            word |= rm << 16
        name = f"sha_{op}_{profile}_r{rd}_{rn}_{rm}"
        cases.append(case_type(name, controls + initialize + (word,) + capture,
                               name, seed(vectors), 0xB0000000, "sha"))

    patterns = ([0] * 4, [MASK] * 4, [0x80000000] * 4, [0x7FFFFFFF] * 4,
                [0xAAAAAAAA, 0x55555555] * 2, [0x01234567, 0x89ABCDEF, 0xFEDCBA98, 0x76543210])
    for op in BASES:
        for bit in range(128):
            # Put each bit in a different operand on alternating cases, with
            # nonuniform other operands to expose permutations and carries.
            vectors = [[0x10203040 + reg * 0x11111111 + lane * 0x01010101 for lane in range(4)]
                       for reg in range(4)]
            source = 1 if op == "sha1h" else bit % 3
            vectors[source] = [0] * 4
            vectors[source][bit // 32] = 1 << (bit % 32)
            add(op, f"bit{bit}", vectors, aliases[0])
        for i, pattern in enumerate(patterns):
            vectors = [list(pattern), list(reversed(pattern)), [value ^ MASK for value in pattern], [0xA5A5A5A5] * 4]
            for regs in aliases:
                add(op, f"pattern{i}", vectors, regs)
        for i in range(8):
            vectors = [[rng.getrandbits(32) for _ in range(4)] for _ in range(4)]
            for regs in aliases:
                add(op, f"random{i}", vectors, regs)
        for i, edge in enumerate((0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, MASK)):
            add(op, f"boundary{i}", [[edge] * 4, [MASK - edge] * 4, [1] * 4, [0xDEADBEEF] * 4], aliases[0])

    for sha256 in (False, True):
        for message in (b"", b"abc"):
            initial = INITIAL256 if sha256 else INITIAL1
            vectors = [list(initial[:4]), list(initial[4:]) + ([0] * 3 if not sha256 else []), [0] * 4, [0] * 4]
            words, rounds = [], schedule(message, sha256)
            current_e = 1
            for group in range(16 if sha256 else 20):
                keys = [(rounds[4 * group + i] + (K256[4 * group + i] if sha256 else
                         (0x5A827999, 0x6ED9EBA1, 0x8F1BBCDC, 0xCA62C1D6)[group // 5])) & MASK for i in range(4)]
                key = struct.pack("<4I", *keys)
                words += differential.mov64(9, int.from_bytes(key[:8], "little"))
                words += differential.mov64(10, int.from_bytes(key[8:], "little"))
                words += [0x9E670123, 0x9EAF0143]  # Load V3 round words.
                if sha256:
                    words += [0x4EA01C04, BASES["sha256h"] | (3 << 16) | (1 << 5),
                              BASES["sha256h2"] | (3 << 16) | (4 << 5) | 1]
                else:
                    next_e = 3 - current_e
                    words += [BASES["sha1h"] | next_e]
                    op = "sha1c" if group < 5 else "sha1p" if group < 10 or group >= 15 else "sha1m"
                    words += [BASES[op] | (3 << 16) | (current_e << 5)]
                    current_e = next_e
            # Restore temporaries so snapshots preserve the complete fixture.
            words += list(initialize[4:])
            name = f"sha_chain_{'sha256' if sha256 else 'sha1'}_{'abc' if message else 'empty'}"
            cases.append(case_type(name, controls + initialize + tuple(words) + capture,
                                   name, seed(vectors), 0xB0000000, "sha"))
    if len(cases) != 2464:
        raise ValueError(f"SHA coverage plan changed: expected 2464, got {len(cases)}")
    return cases


def expected(case):
    vectors = [list(struct.unpack("<4I", struct.pack("<QQ", case.seeds[n], case.seeds[n + 1])))
               for n in (1, 3, 5, 7)]
    if case.name.startswith("sha_chain_"):
        sha256 = "_sha256_" in case.name
        message = b"abc" if case.name.endswith("_abc") else b""
        digest = (hashlib.sha256 if sha256 else hashlib.sha1)(message).digest()
        # Public digest includes feed-forward; instructions perform compression.
        initial = INITIAL256 if sha256 else INITIAL1
        state = [(value - initial[i]) & MASK for i, value in enumerate(struct.unpack(">8I" if sha256 else ">5I", digest))]
        vectors[0], vectors[1] = state[:4], state[4:] + ([] if sha256 else [0] * 3)
    else:
        _, op, rest = case.name.split("_", 2)
        rd, rn, rm = map(int, rest.rsplit("_r", 1)[1].split("_"))
        vectors[rd] = operation(op, vectors[rd], vectors[rn], vectors[rm])
    return {"memory": b"".join(struct.pack("<4I", *vector) for vector in vectors),
            "fpsr": case.seeds[27], "fpcr": case.seeds[28]}


def verify():
    import arm64_differential as differential
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    import unicorn.arm64_const as registers
    decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    cases = corpus(differential.Case)
    for case in cases:
        code = b"".join(word.to_bytes(4, "little") for word in case.instructions)
        if len(list(decoder.disasm(code, 0x400000))) * 4 != len(code):
            raise ValueError(f"{case.name}: Capstone rejected code")
        cpu = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        cpu.ctl_set_cpu_model(registers.UC_CPU_ARM64_MAX)
        cpu.mem_map(0x400000, 4096)
        cpu.mem_map(differential.MEMORY_BASE & ~4095, 4096)
        cpu.mem_write(0x400000, code)
        for n in range(29):
            cpu.reg_write(getattr(registers, f"UC_ARM64_REG_X{n}"), case.seeds.get(n, 0x100 + n))
        cpu.reg_write(registers.UC_ARM64_REG_X29, differential.MEMORY_BASE)
        cpu.reg_write(registers.UC_ARM64_REG_NZCV, case.nzcv)
        cpu.emu_start(0x400000, 0x400000 + len(code))
        wanted = expected(case)
        if (bytes(cpu.mem_read(differential.MEMORY_BASE, 64)) != wanted["memory"] or
                cpu.reg_read(registers.UC_ARM64_REG_X27) != wanted["fpsr"] or
                cpu.reg_read(registers.UC_ARM64_REG_X28) != wanted["fpcr"] or
                cpu.reg_read(registers.UC_ARM64_REG_NZCV) != case.nzcv):
            raise ValueError(f"{case.name}: scalar FIPS/hashlib reference disagrees with Unicorn")
    print(f"PASS {len(cases)} SHA independent scalar/FIPS/hashlib/Capstone/Unicorn cases")
    return 0


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify", action="store_true", required=True)
    parser.parse_args()
    raise SystemExit(verify())
