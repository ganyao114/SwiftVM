#!/usr/bin/env python3
"""Mutate an AOT artifact, to prove the checks that are supposed to catch each
class of defect actually catch it (docs/aot-design.md §7.6).

Every mutation here is *size preserving*, so the ELF layout never has to be
recomputed and the mutation is the only difference between the two files.
Where a mutation would otherwise be caught by a checksum rather than by the
mechanism under test, the relevant hash is re-stamped so the artifact loads and
the *behaviour* is what fails.

  aot_mutate.py <in> <out> <mutation> [args]

Mutations:
  reloc-addend [delta]   every relocation's addend is shifted; the loader still
                         relocates, just to the wrong host address. Proves the
                         relocations are load bearing.
  code-byte [n]          flip a bit in .svmaot.text and re-stamp code_hash, so
                         the artifact loads. Proves the compiled AOT code is
                         what actually runs.
  code-byte-nostamp      flip a bit in .svmaot.text, leave code_hash alone.
                         Must be REJECTED.
  info-byte              flip a bit in the .svmaot.info payload, leave the
                         payload checksum alone. Must be REJECTED.
  move-data [index]      shift guest segment `index` by one page and re-stamp the
                         guest image hash, so the artifact loads with its data
                         at the wrong guest addresses. Proves the "guest data
                         must not move" constraint is under test.
  guest-byte             flip a bit of guest code inside a compiled unit and
                         re-stamp the guest image hash. Must be REJECTED by the
                         per-unit guest byte hash.
  sym-size [delta]       add `delta` to every rewritten STT_FUNC's st_size.
                         Must be caught by aot_elf_check.py.
"""
import struct
import sys

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1
MAGIC = b"SVMAOT\x00\x01"


def hash_bytes(data, seed):
    h = seed
    for b in data:
        h ^= b
        h = (h * FNV_PRIME) & MASK64
    return h


def hash_u64(v, seed):
    return hash_bytes(struct.pack("<Q", v & MASK64), seed)


class Reader:
    def __init__(self, buf, base=0):
        self.b = buf
        self.p = 0
        self.base = base

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def u16(self):
        v = struct.unpack_from("<H", self.b, self.p)[0]; self.p += 2; return v

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.p)[0]; self.p += 4; return v

    def u64(self):
        v = struct.unpack_from("<Q", self.b, self.p)[0]; self.p += 8; return v

    def skip(self, n):
        self.p += n


class Artifact:
    """Just enough ELF to find named sections, plus a field-offset map of the
    .svmaot.info payload."""

    def __init__(self, path):
        self.raw = bytearray(open(path, "rb").read())
        d = self.raw
        (self.e_type, self.e_machine, _, self.e_entry, self.e_phoff, self.e_shoff,
         _, _, _, self.e_phnum, self.e_shentsize, self.e_shnum,
         self.e_shstrndx) = struct.unpack_from("<HHIQQQIHHHHHH", d, 16)
        self.secs = []
        for i in range(self.e_shnum):
            o = self.e_shoff + i * self.e_shentsize
            nm, typ, flags, addr, off, size, link, info, align, ent = \
                struct.unpack_from("<IIQQQQIIQQ", d, o)
            self.secs.append(dict(nm=nm, type=typ, flags=flags, addr=addr, off=off,
                                  size=size, link=link, info=info, hdr=o, idx=i))
        sh = self.secs[self.e_shstrndx]
        blob = bytes(d[sh["off"]:sh["off"] + sh["size"]])
        for s in self.secs:
            s["name"] = blob[s["nm"]:blob.find(b"\0", s["nm"])].decode("latin1")
        self.info = self.sec(".svmaot.info")
        self.code = self.sec(".svmaot.text")
        self.parse_info()

    def sec(self, name):
        return next((s for s in self.secs if s["name"] == name), None)

    def sdata(self, s):
        return bytes(self.raw[s["off"]:s["off"] + s["size"]])

    # --- .svmaot.info ---------------------------------------------------
    def parse_info(self):
        base = self.info["off"]
        blob = self.sdata(self.info)
        assert blob[:8] == MAGIC, "bad magic"
        r = Reader(blob)
        r.skip(8)
        self.fmt_version = r.u64()
        self.off_key = base + r.p
        self.key = [r.u64() for _ in range(5)]  # format, build, config, env, guest
        self.off_payload_size = base + r.p
        self.payload_size = r.u64()
        self.off_payload_hash = base + r.p
        self.payload_hash = r.u64()
        self.payload_start = base + r.p

        r2 = Reader(blob, base)
        r2.p = r.p
        def at():
            return base + r2.p
        self.guest_entry_at = at(); self.guest_entry = r2.u64()
        r2.u64(); r2.u64(); r2.u64()          # phdr, phentsize, phnum
        r2.u64()                              # brk
        path_len = r2.u32(); r2.skip(path_len)
        self.stub_offset_at = at(); self.stub_offset = r2.u32()
        self.code_size = r2.u32()
        self.code_hash_at = at(); self.code_hash = r2.u64()
        r2.skip(9 * 4)                        # stats
        n_seg = r2.u32()
        self.segments = []
        for _ in range(n_seg):
            e = dict(vaddr_at=at())
            e["vaddr"] = r2.u64()
            e["memsz"] = r2.u64()
            e["flags"] = r2.u32()
            e["filesz"] = r2.u64()
            self.segments.append(e)
        n_slots = r2.u64()
        r2.skip(int(n_slots) * 12)
        n_units = r2.u64()
        self.units = []
        for _ in range(n_units):
            u = dict()
            u["guest_start"] = r2.u64()
            u["is_function"] = r2.u8()
            u["code_offset"] = r2.u32()
            u["code_size"] = r2.u32()
            nb = r2.u32()
            u["blocks"] = []
            for _ in range(nb):
                b = dict(guest_start=r2.u64(), guest_end=r2.u64())
                b["code_offset"] = r2.u32()
                b["hash_at"] = at()
                b["hash"] = r2.u64()
                u["blocks"].append(b)
            nr = r2.u32()
            u["relocs"] = []
            for _ in range(nr):
                rel = dict()
                r2.u32(); r2.u16(); r2.u16(); r2.u16(); r2.u16()
                rel["addend_at"] = at()
                rel["addend"] = r2.u64()
                rel["recorded"] = r2.u64()
                u["relocs"].append(rel)
            self.units.append(u)
        assert base + r2.p == base + len(blob), "payload did not consume the section"

    # --- re-stamping -----------------------------------------------------
    def restamp_payload(self):
        payload = bytes(self.raw[self.payload_start:
                                 self.payload_start + self.payload_size])
        struct.pack_into("<Q", self.raw, self.off_payload_hash,
                         hash_bytes(payload, FNV_OFFSET))

    def restamp_code_hash(self):
        code = self.sdata(self.code)
        struct.pack_into("<Q", self.raw, self.code_hash_at, hash_bytes(code, FNV_OFFSET))

    def guest_image_hash(self):
        """Mirror of swift::aot::HashGuestImage."""
        h = hash_u64(self.guest_entry, FNV_OFFSET)
        h = hash_u64(len(self.segments), h)
        for i, s in enumerate(self.segments):
            sec = self.sec(f".svmaot.seg{i}")
            data = self.sdata(sec)
            h = hash_u64(s["vaddr"], h)
            h = hash_u64(s["memsz"], h)
            h = hash_u64(s["flags"], h)
            h = hash_u64(len(data), h)
            h = hash_bytes(data, h)
        return h

    def restamp_guest_hash(self):
        struct.pack_into("<Q", self.raw, self.off_key + 4 * 8, self.guest_image_hash())

    def save(self, path):
        open(path, "wb").write(bytes(self.raw))

    # --- symbols ----------------------------------------------------------
    def symtab(self):
        return next((s for s in self.secs if s["type"] == 2), None)


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    src, dst, mutation = sys.argv[1], sys.argv[2], sys.argv[3]
    arg = sys.argv[4] if len(sys.argv) > 4 else None
    a = Artifact(src)

    if mutation == "reloc-addend":
        delta = int(arg or "0x10000", 0)
        n = 0
        for u in a.units:
            for rel in u["relocs"]:
                struct.pack_into("<Q", a.raw, rel["addend_at"],
                                 (rel["addend"] + delta) & MASK64)
                n += 1
        a.restamp_payload()
        print(f"perturbed {n} relocation addends by {delta:#x}")

    elif mutation in ("code-byte", "code-byte-nostamp"):
        # Land inside the first unit's code, not in padding.
        off = a.code["off"] + a.units[0]["code_offset"] + int(arg or 4)
        a.raw[off] ^= 0x01
        if mutation == "code-byte":
            a.restamp_code_hash()
            a.restamp_payload()
        print(f"flipped a bit of .svmaot.text at file offset {off:#x}"
              f"{' (code_hash re-stamped)' if mutation == 'code-byte' else ''}")

    elif mutation == "info-byte":
        a.raw[a.payload_start + 1] ^= 0x01
        print("flipped a bit of the .svmaot.info payload (checksum left stale)")

    elif mutation == "move-data":
        # A *middle* segment by default: moving the last one changes the image
        # span end, which the loader already rejects on a brk mismatch, and
        # that would let this mutation "pass" without ever demonstrating the
        # thing it exists to demonstrate (globals read from the wrong place).
        delta = 0x1000
        idx = int(arg) if arg is not None else max(0, len(a.segments) - 2)
        s = a.segments[idx]
        struct.pack_into("<Q", a.raw, s["vaddr_at"], s["vaddr"] + delta)
        s["vaddr"] += delta
        a.restamp_guest_hash()
        a.restamp_payload()
        print(f"moved the last guest segment from {s['vaddr'] - delta:#x} to "
              f"{s['vaddr']:#x} (guest hash re-stamped)")

    elif mutation == "guest-byte":
        # A byte of guest code covered by unit 0's first block.
        blk = a.units[0]["blocks"][0]
        target = blk["guest_start"]
        for i, s in enumerate(a.segments):
            if s["vaddr"] <= target < s["vaddr"] + s["filesz"]:
                sec = a.sec(f".svmaot.seg{i}")
                off = sec["off"] + (target - s["vaddr"])
                a.raw[off] ^= 0x01
                a.restamp_guest_hash()
                a.restamp_payload()
                print(f"flipped a bit of guest code at {target:#x} "
                      f"(guest hash re-stamped)")
                break
        else:
            print("could not locate the guest byte")
            return 1

    elif mutation == "sym-size":
        delta = int(arg or "8", 0)
        st = a.symtab()
        n = 0
        for i in range(st["size"] // 24):
            o = st["off"] + i * 24
            info = a.raw[o + 4]
            if info & 0xF != 2:  # STT_FUNC
                continue
            size = struct.unpack_from("<Q", a.raw, o + 16)[0]
            struct.pack_into("<Q", a.raw, o + 16, size + delta)
            n += 1
        print(f"grew {n} STT_FUNC st_size values by {delta}")

    else:
        print(f"unknown mutation {mutation}")
        return 2

    a.save(dst)
    return 0


if __name__ == "__main__":
    sys.exit(main())
