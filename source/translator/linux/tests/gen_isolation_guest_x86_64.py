#!/usr/bin/env python3
"""Emit minimal guest ELFs that try to reach *host* memory.

TEST FIXTURE GENERATOR for run_isolation_tests.sh.

The property under test is address-space isolation: a guest memory access is
`host = guest + bias` with no bounds check, so a guest address large enough to
cover the distance to a host mapping reads or writes host memory directly.
Both shapes below were confirmed by hand (lldb, ASLR off) against the
unbounded build:

  ``alias``  the WRITE evidence, made self-verifying: stores a magic through
             ``base + delta`` and reads it back through ``base`` (delta a power
             of two >= 2^32).  With a bounded guest window every guest address
             truncates into the window, the two addresses alias, and the
             readback matches.  Without one the store lands `delta` bytes past
             the end of guest memory -- i.e. in host memory -- and the readback
             still shows the sentinel (or the host dies outright).  This is the
             same defect that let a hand-driven guest plant
             0x4141414141414141 in a host malloc buffer.

  ``load``   the READ evidence, same trick on the load side: compares
             ``[base + delta]`` with ``[base]``.  Escaping the window is what
             let a hand-driven guest read 0xfeedfacf -- the translator's own
             Mach-O header -- out of host memory.  The literal header read is
             not reproducible without disabling ASLR (its address moves per
             run), so the property is asserted instead of the address.

  ``read``   loads 4 bytes from an absolute guest address and writes them to
             *stderr* (the translator logs to stdout).  Diagnostic shape for
             driving a known host address by hand.

  ``wild``   dereferences extreme addresses (-1, 2^63, ...) that no
             translation can make valid.  Nothing is asserted about the guest;
             the host must merely survive.

Usage:
    gen_isolation_guest_x86_64.py read  <hex-guest-addr> -o out.elf
    gen_isolation_guest_x86_64.py alias <hex-delta>      -o out.elf
    gen_isolation_guest_x86_64.py load  <hex-delta>      -o out.elf
    gen_isolation_guest_x86_64.py wild  <hex-guest-addr> -o out.elf
"""

import argparse
import struct

VADDR = 0x400000
SCRATCH_OFF = 0x2000
EHSIZE, PHSIZE = 64, 56
CODE_OFF = EHSIZE + PHSIZE
SPAN = 0x3000


def _headers(entry, filesz, memsz):
    eh = bytearray(EHSIZE)
    eh[0:4] = b"\x7fELF"
    eh[4], eh[5], eh[6] = 2, 1, 1  # ELF64, little endian, version 1
    struct.pack_into("<HHI", eh, 16, 2, 0x3E, 1)  # ET_EXEC, EM_X86_64
    struct.pack_into("<QQQ", eh, 24, entry, EHSIZE, 0)
    struct.pack_into("<IHHHHHH", eh, 48, 0, EHSIZE, PHSIZE, 1, 0, 0, 0)
    ph = bytearray(PHSIZE)  # PT_LOAD, RWX, offset 0
    struct.pack_into("<IIQQQQQQ", ph, 0, 1, 7, 0, VADDR, VADDR, filesz, memsz, 0x1000)
    return bytes(eh), bytes(ph)


def _exit(code):
    return (b"\xb8\x3c\x00\x00\x00" +                      # mov eax, 60 (exit)
            b"\xbf" + struct.pack("<I", code) +            # mov edi, code
            b"\x0f\x05")                                   # syscall


def _write_scratch(nbytes, fd=2):
    """write(fd, scratch, nbytes) -- the probe bytes go to stderr because the
    translator's own log chatter goes to stdout, so `2>file` isolates them."""
    return (b"\xb8\x01\x00\x00\x00" +                      # mov eax, 1 (write)
            b"\xbf" + struct.pack("<I", fd) +                 # mov edi, fd
            b"\x48\xbe" + struct.pack("<Q", VADDR + SCRATCH_OFF) +  # movabs rsi, scratch
            b"\xba" + struct.pack("<I", nbytes) +          # mov edx, nbytes
            b"\x0f\x05")                                   # syscall


def build_read(addr):
    code = bytearray()
    code += b"\x48\xb8" + struct.pack("<Q", addr)          # movabs rax, addr
    code += b"\x8b\x00"                                    # mov eax, [rax]
    code += b"\x48\xbb" + struct.pack("<Q", VADDR + SCRATCH_OFF)  # movabs rbx, scratch
    code += b"\x89\x03"                                    # mov [rbx], eax
    code += _write_scratch(4)
    code += _exit(0)
    return code


def build_alias(delta):
    # scratch holds a known value; store a magic through scratch+delta and read
    # scratch back.  Aliased (windowed) => magic; unbounded => sentinel.
    magic = 0x4141414141414141
    sentinel = 0x5A5A5A5A5A5A5A5A
    code = bytearray()
    code += b"\x48\xbb" + struct.pack("<Q", VADDR + SCRATCH_OFF)  # movabs rbx, scratch
    code += b"\x48\xb8" + struct.pack("<Q", sentinel)      # movabs rax, sentinel
    code += b"\x48\x89\x03"                                # mov [rbx], rax
    code += b"\x48\xb9" + struct.pack("<Q", delta)         # movabs rcx, delta
    code += b"\x48\x01\xd9"                                # add rcx, rbx
    code += b"\x48\xb8" + struct.pack("<Q", magic)         # movabs rax, magic
    code += b"\x48\x89\x01"                                # mov [rcx], rax
    code += b"\x48\x8b\x03"                                # mov rax, [rbx]
    code += b"\x48\xba" + struct.pack("<Q", magic)         # movabs rdx, magic
    code += b"\x48\x39\xd0"                                # cmp rax, rdx
    code += b"\x75\x05"                                    # jne +5 -> exit(1)
    code += _exit(0)
    code += _exit(1)
    return code


def build_load(delta):
    # Store a magic at scratch, then load through scratch+delta.  Windowed =>
    # the two alias and the load returns the magic; unbounded => the load
    # reaches host memory (different value, or a fault).
    magic = 0x4141414141414141
    code = bytearray()
    code += b"\x48\xbb" + struct.pack("<Q", VADDR + SCRATCH_OFF)  # movabs rbx, scratch
    code += b"\x48\xb8" + struct.pack("<Q", magic)         # movabs rax, magic
    code += b"\x48\x89\x03"                                # mov [rbx], rax
    code += b"\x48\xb9" + struct.pack("<Q", delta)         # movabs rcx, delta
    code += b"\x48\x01\xd9"                                # add rcx, rbx
    code += b"\x48\x8b\x01"                                # mov rax, [rcx]
    code += b"\x48\xba" + struct.pack("<Q", magic)         # movabs rdx, magic
    code += b"\x48\x39\xd0"                                # cmp rax, rdx
    code += b"\x75\x05"                                    # jne +5 -> exit(1)
    code += _exit(0)
    code += _exit(1)
    return code


def build_wild(addr):
    code = bytearray()
    code += b"\x48\xb8" + struct.pack("<Q", addr)          # movabs rax, addr
    code += b"\x8b\x18"                                    # mov ebx, [rax]
    code += b"\xc7\x00\x41\x41\x41\x41"                    # mov dword [rax], 0x41414141
    code += _exit(0)
    return code


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shape", choices=["read", "alias", "load", "wild"])
    ap.add_argument("value", help="hex guest address (read/wild) or hex delta (alias/load)")
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()
    value = int(args.value, 16) & 0xFFFFFFFFFFFFFFFF

    code = {"read": build_read, "alias": build_alias, "load": build_load,
            "wild": build_wild}[args.shape](value)
    entry = VADDR + CODE_OFF
    body = bytes(code)
    filesz = CODE_OFF + len(body)
    eh, ph = _headers(entry, filesz, SPAN)
    with open(args.out, "wb") as f:
        f.write(eh + ph + body)


if __name__ == "__main__":
    main()
