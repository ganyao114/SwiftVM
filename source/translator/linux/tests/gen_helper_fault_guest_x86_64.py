#!/usr/bin/env python3
"""Emit minimal guest ELFs whose guest memory faults happen inside HOST helpers.

TEST FIXTURE GENERATOR for run_helper_fault_tests.sh.

Two properties are under test; both are about *where* a guest fault is taken,
not about what the guest computes.

1. HOST SURVIVAL ON A CLONE WORKER (`clone_pf`).
   A clone() worker that dies of a guest page fault must leave the host alive.
   It used not to: the worker's CLONE_CHILD_CLEARTID teardown store is a HOST
   write into guest memory, and when the ctid word shares a guest page with
   translated code that page is SMC write-protected, so the store took a host
   SIGBUS on a thread where no Runtime was active -- process dead.  The shape
   here puts ctid in the same page as the code deliberately (one RWX PT_LOAD,
   which is also what every freestanding guest in this tree looks like).

2. A HELPER'S GUEST FAULT MUST BE A GUEST FAULT (`fnstenv`, `fldenv`,
   `fxsave`, `fxrstor`, `rep_movs`, `rep_stos`, `rep_scas`, `rep_cmps`).
   These instructions are lowered to host helper calls that dereference guest
   memory from a live host frame, which runtime.cpp's HandleFault cannot
   unwind.  The old behaviour was to validate and then silently skip (x87 /
   fxsave) or to clamp the walk and move fewer bytes (rep), i.e. to compute the
   wrong answer quietly.  The required behaviour is ExitReason::PageFatal.
   Each shape aims the instruction at an address the guest never mapped and
   then, if it survives, WRITES A MARKER and exits 0 -- so "the guest kept
   running" is distinguishable from "the guest faulted".

Usage:
    gen_helper_fault_guest_x86_64.py <shape> -o out.elf
"""

import argparse
import struct

VADDR = 0x400000
SPAN = 0x8000            # mapped guest image: [0x400000, 0x408000)
SCRATCH_OFF = 0x2000     # inside the image
CTID_OFF = 0x40          # in the SAME page as the code: that is the point
STACK_OFF = 0x7000       # clone worker stack top, also inside the image
# Far outside the image but inside the default 32-bit guest window, so the
# access is a plain unmapped-page fault rather than a window escape.
UNMAPPED = 0x30000000

EHSIZE, PHSIZE = 64, 56
CODE_OFF = EHSIZE + PHSIZE


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
    return (b"\xb8\x3c\x00\x00\x00" +                  # mov eax, 60 (exit)
            b"\xbf" + struct.pack("<I", code) +        # mov edi, code
            b"\x0f\x05")                               # syscall


def _survived():
    """The instruction under test did NOT fault: say so out loud on stderr and
    exit 0.  Without this, "no fault" and "fault" would both just be a dead
    guest and the test would pass for the wrong reason."""
    msg = b"SURVIVED\n"
    return (b"\x48\xbb" + struct.pack("<Q", VADDR + SCRATCH_OFF) +   # movabs rbx, scratch
            b"".join(b"\xc6\x83" + struct.pack("<i", i) + bytes([c])  # mov byte [rbx+i], c
                     for i, c in enumerate(msg)) +
            b"\xb8\x01\x00\x00\x00" +                                # mov eax, 1 (write)
            b"\xbf\x02\x00\x00\x00" +                                # mov edi, 2 (stderr)
            b"\x48\x89\xde" +                                        # mov rsi, rbx
            b"\xba" + struct.pack("<I", len(msg)) +                  # mov edx, len
            b"\x0f\x05" +                                            # syscall
            _exit(0))


# --- 1. clone worker whose page fault must not take the host down ----------

def build_clone_pf():
    """clone() a worker that dereferences an unmapped guest address.

    The worker dies of PageFatal.  Its CLONE_CHILD_CLEARTID teardown then
    stores 0 to ctid -- a host write to a guest page that also holds code, so
    it is SMC write-protected.  Before the fix that store killed the host with
    an unhandled SIGBUS; now the SMC handler claims it.  The leader waits for
    ctid to be zeroed (which only happens on that teardown path) and exits 0.
    """
    ctid = VADDR + CTID_OFF
    code = bytearray()
    # ctid = 1
    code += b"\x48\xb8" + struct.pack("<Q", ctid)          # movabs rax, ctid
    code += b"\xc7\x00\x01\x00\x00\x00"                    # mov dword [rax], 1
    # clone(CLONE_VM|FS|FILES|SIGHAND|THREAD|SYSVSEM|CHILD_CLEARTID,
    #       child_stack, ptid=0, ctid, tls=0)
    code += b"\xbf\x00\x0f\x25\x00"                        # mov edi, 0x250f00
    code += b"\x48\xbe" + struct.pack("<Q", VADDR + STACK_OFF)  # movabs rsi, stack top
    code += b"\x31\xd2"                                    # xor edx, edx (ptid)
    code += b"\x49\xba" + struct.pack("<Q", ctid)          # movabs r10, ctid
    code += b"\x45\x31\xc0"                                # xor r8d, r8d (tls)
    code += b"\xb8\x38\x00\x00\x00"                        # mov eax, 56 (clone)
    code += b"\x0f\x05"                                    # syscall
    code += b"\x85\xc0"                                    # test eax, eax
    parent = bytearray()
    # --- parent: spin on ctid until the worker's teardown zeroes it ---------
    #  rcx is a spin budget; exhausting it exits 3 ("the worker never died
    #  cleanly"), which the runner reports as its own kind of failure.
    #    L:  cmp dword [rax], 0      3
    #        je  DONE                6
    #        dec rcx                 3
    #        jne L                   6   -> rel32 = -18
    #        exit(3)                12
    #  DONE: exit(0)                12   -> je rel32 = 3 + 6 + 12 = 21
    parent += b"\x48\xb8" + struct.pack("<Q", ctid)        # movabs rax, ctid
    parent += b"\x48\xc7\xc1" + struct.pack("<i", 0x1000000)  # mov rcx, budget
    parent += b"\x83\x38\x00"                              # cmp dword [rax], 0
    parent += b"\x0f\x84" + struct.pack("<i", 21)          # je DONE
    parent += b"\x48\xff\xc9"                              # dec rcx
    parent += b"\x0f\x85" + struct.pack("<i", -18)         # jne L
    parent += _exit(3)                                     # timed out
    parent += _exit(0)
    # --- child: touch an address that was never mapped ----------------------
    child = bytearray()
    child += b"\x48\xb8" + struct.pack("<Q", UNMAPPED)     # movabs rax, unmapped
    child += b"\x8b\x18"                                   # mov ebx, [rax]
    child += _exit(0)                                      # unreachable if it faults
    code += b"\x0f\x84" + struct.pack("<i", len(parent))   # jz -> child
    code += parent
    code += child
    return code


# --- 2. helper-resident guest faults ---------------------------------------

def _load_unmapped_into(reg_bytes):
    return b"\x48\xb8" + struct.pack("<Q", UNMAPPED) + reg_bytes


def build_fnstenv():
    # fnstenv [rax] writes 28 bytes through the x87 helper.
    code = bytearray()
    code += b"\x48\xb8" + struct.pack("<Q", UNMAPPED)      # movabs rax, unmapped
    code += b"\xd9\x30"                                    # fnstenv [rax]
    code += _survived()
    return code


def build_fldenv():
    # fldenv [rax] reads 28 bytes through the x87 helper (LoadEnvironment).
    # FRSTOR is deliberately not used: it is not implemented at all and dies
    # with IllegalCode, which would test the decoder rather than the helper.
    code = bytearray()
    code += b"\x48\xb8" + struct.pack("<Q", UNMAPPED)      # movabs rax, unmapped
    code += b"\xd9\x20"                                    # fldenv [rax]
    code += _survived()
    return code


def build_fnstcw():
    code = bytearray()
    code += b"\x48\xb8" + struct.pack("<Q", UNMAPPED)      # movabs rax, unmapped
    code += b"\xd9\x38"                                    # fnstcw [rax]
    code += _survived()
    return code


def build_fld_m80():
    code = bytearray()
    code += b"\x48\xb8" + struct.pack("<Q", UNMAPPED)      # movabs rax, unmapped
    code += b"\xdb\x28"                                    # fld tbyte [rax]
    code += _survived()
    return code


def build_fxsave():
    code = bytearray()
    code += b"\x48\xb8" + struct.pack("<Q", UNMAPPED)      # movabs rax, unmapped
    code += b"\x0f\xae\x00"                                # fxsave [rax]
    code += _survived()
    return code


def build_fxrstor():
    code = bytearray()
    code += b"\x48\xb8" + struct.pack("<Q", UNMAPPED)      # movabs rax, unmapped
    code += b"\x0f\xae\x08"                                # fxrstor [rax]
    code += _survived()
    return code


def build_rep_movs():
    # rep movsb from mapped scratch into unmapped memory, 64 bytes.
    code = bytearray()
    code += b"\x48\xbe" + struct.pack("<Q", VADDR + SCRATCH_OFF)  # movabs rsi, scratch
    code += b"\x48\xbf" + struct.pack("<Q", UNMAPPED)      # movabs rdi, unmapped
    code += b"\x48\xc7\xc1\x40\x00\x00\x00"                # mov rcx, 64
    code += b"\xfc"                                        # cld
    code += b"\xf3\xa4"                                    # rep movsb
    code += _survived()
    return code


def build_rep_movs_partial():
    # The interesting shape: the walk STARTS in mapped memory and runs off the
    # end of the image.  A clamping implementation copies the mapped prefix and
    # returns as if nothing happened; the architectural answer is #PF.
    start = VADDR + SPAN - 64
    code = bytearray()
    code += b"\x48\xbe" + struct.pack("<Q", VADDR + SCRATCH_OFF)  # movabs rsi, scratch
    code += b"\x48\xbf" + struct.pack("<Q", start)         # movabs rdi, image end - 64
    code += b"\x48\xc7\xc1\x00\x02\x00\x00"                # mov rcx, 512
    code += b"\xfc"                                        # cld
    code += b"\xf3\xa4"                                    # rep movsb
    code += _survived()
    return code


def build_rep_stos():
    code = bytearray()
    code += b"\x48\xbf" + struct.pack("<Q", UNMAPPED)      # movabs rdi, unmapped
    code += b"\x48\xc7\xc1\x40\x00\x00\x00"                # mov rcx, 64
    code += b"\x31\xc0"                                    # xor eax, eax
    code += b"\xfc"                                        # cld
    code += b"\xf3\xaa"                                    # rep stosb
    code += _survived()
    return code


def build_rep_stos_partial():
    start = VADDR + SPAN - 64
    code = bytearray()
    code += b"\x48\xbf" + struct.pack("<Q", start)         # movabs rdi, image end - 64
    code += b"\x48\xc7\xc1\x00\x02\x00\x00"                # mov rcx, 512
    code += b"\x31\xc0"                                    # xor eax, eax
    code += b"\xfc"                                        # cld
    code += b"\xf3\xaa"                                    # rep stosb
    code += _survived()
    return code


def build_rep_scas():
    # scasb for a byte that never appears, so the loop cannot stop early and
    # must reach the unmapped page.
    code = bytearray()
    code += b"\x48\xbf" + struct.pack("<Q", VADDR + SPAN - 64)  # movabs rdi, image end - 64
    code += b"\x48\xc7\xc1\x00\x02\x00\x00"                # mov rcx, 512
    code += b"\xb0\xa5"                                    # mov al, 0xA5 (never stored)
    code += b"\xfc"                                        # cld
    code += b"\xf2\xae"                                    # repne scasb
    code += _survived()
    return code


def build_rep_cmps():
    code = bytearray()
    code += b"\x48\xbe" + struct.pack("<Q", VADDR + SCRATCH_OFF)  # movabs rsi, scratch
    code += b"\x48\xbf" + struct.pack("<Q", VADDR + SPAN - 64)    # movabs rdi, image end - 64
    code += b"\x48\xc7\xc1\x00\x02\x00\x00"                # mov rcx, 512
    code += b"\xfc"                                        # cld
    code += b"\xf3\xa6"                                    # repe cmpsb  (all zero == zero)
    code += _survived()
    return code


SHAPES = {
    "clone_pf": build_clone_pf,
    "fnstenv": build_fnstenv,
    "fldenv": build_fldenv,
    "fnstcw": build_fnstcw,
    "fld_m80": build_fld_m80,
    "fxsave": build_fxsave,
    "fxrstor": build_fxrstor,
    "rep_movs": build_rep_movs,
    "rep_movs_partial": build_rep_movs_partial,
    "rep_stos": build_rep_stos,
    "rep_stos_partial": build_rep_stos_partial,
    "rep_scas": build_rep_scas,
    "rep_cmps": build_rep_cmps,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shape", choices=sorted(SHAPES))
    ap.add_argument("-o", "--out", required=True)
    args = ap.parse_args()
    body = bytes(SHAPES[args.shape]())
    entry = VADDR + CODE_OFF
    filesz = CODE_OFF + len(body)
    assert filesz < SCRATCH_OFF, "code overran the scratch area"
    eh, ph = _headers(entry, filesz, SPAN)
    with open(args.out, "wb") as f:
        f.write(eh + ph + body)


if __name__ == "__main__":
    main()
