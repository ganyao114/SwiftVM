//
// docs/aot-design.md §8: what an AOT artifact does when the guest overwrites
// code the artifact compiled.
//
// The design said the artifact "should DETECT that the range was written and
// refuse to continue, rather than execute stale code".  This program is the
// case that decides between "refuse" and "retranslate", because it makes the
// difference observable:
//
//   Patchable() is compiled (and, under --aot, pre-compiled into the artifact
//   and registered with the SMC tracker).  It is called, then its immediate is
//   rewritten in place, then it is called again.  A run that executes stale
//   code prints the OLD value -- silently and plausibly.  A run that
//   retranslates prints the new one.  A run that refuses prints nothing.
//
// The immediate is located by scanning the function's own bytes for the
// pattern rather than by hard-coding an offset, so the test does not depend on
// which encoding the compiler chose.
//
// Build: clang --target=x86_64-unknown-linux-gnu -ffreestanding -nostdlib
// -fno-pic -fno-pie -mcmodel=small, linked by mkaotguest.py (which emits one
// RWX PT_LOAD, so the guest may write its own text).
//
typedef unsigned long u64;
typedef unsigned int u32;

#define OLD_IMM 0x11111111u
#define NEW_IMM 0x22222222u

static char out_buf[512];

__attribute__((noinline)) static u32 Patchable(void) { return OLD_IMM; }

// Through a volatile pointer so the compiler cannot fold the second call into
// the first, or into a constant.
static u32 (*volatile g_fn)(void) = Patchable;

static long Write(int fd, const char* buf, u64 len) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(1L), "D"((long)fd), "S"(buf), "d"(len)
                     : "rcx", "r11", "memory");
    return ret;
}

__attribute__((noreturn)) static void Exit(int code) {
    __asm__ volatile("syscall" : : "a"(60L), "D"((long)code) : "memory");
    __builtin_unreachable();
}

static u64 Hex(char* dst, u64 v) {
    static const char kDigits[] = "0123456789abcdef";
    for (int i = 7; i >= 0; --i) {
        dst[7 - i] = kDigits[(v >> (i * 4)) & 0xF];
    }
    return 8;
}

static u64 Emit(u64 pos, const char* label, u64 value) {
    while (*label) {
        out_buf[pos++] = *label++;
    }
    out_buf[pos++] = '=';
    pos += Hex(out_buf + pos, value);
    out_buf[pos++] = '\n';
    return pos;
}

// Rewrite the immediate inside Patchable's own body.  Returns the byte offset
// that was patched, or -1 when the pattern was not found (which would make the
// whole test meaningless, so it is reported rather than ignored).
__attribute__((noinline)) static long PatchSelf(void) {
    volatile unsigned char* code = (volatile unsigned char*)(unsigned long)&Patchable;
    for (long i = 0; i < 48; ++i) {
        const u32 word = (u32)code[i] | ((u32)code[i + 1] << 8) | ((u32)code[i + 2] << 16) |
                         ((u32)code[i + 3] << 24);
        if (word == OLD_IMM) {
            code[i + 0] = (unsigned char)(NEW_IMM & 0xFF);
            code[i + 1] = (unsigned char)((NEW_IMM >> 8) & 0xFF);
            code[i + 2] = (unsigned char)((NEW_IMM >> 16) & 0xFF);
            code[i + 3] = (unsigned char)((NEW_IMM >> 24) & 0xFF);
            return i;
        }
    }
    return -1;
}

void _start(void) {
    // Call it enough times that it is certainly translated (and, under --aot,
    // that the installed unit is the one running).
    u32 before = 0;
    for (int i = 0; i < 64; ++i) {
        before = g_fn();
    }

    const long offset = PatchSelf();
    __asm__ volatile("" ::: "memory");

    u32 after = 0;
    for (int i = 0; i < 64; ++i) {
        after = g_fn();
    }

    u64 pos = 0;
    pos = Emit(pos, "before", before);
    pos = Emit(pos, "patched_at", (u64)offset);
    pos = Emit(pos, "after", after);
    Write(1, out_buf, pos);

    if (offset < 0) {
        Exit(70);  // the pattern was never found: the test proved nothing
    }
    if (before != OLD_IMM) {
        Exit(71);
    }
    if (after == OLD_IMM) {
        Exit(72);  // STALE CODE: the write was not noticed
    }
    if (after != NEW_IMM) {
        Exit(73);
    }
    Exit(88);
}
