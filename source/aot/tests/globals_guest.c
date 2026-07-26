//
// docs/aot-design.md §7.2: a case that does nothing but read and write guest
// globals.
//
// The constraint being tested is that guest data stays at its *linked* guest
// virtual address after AOT compilation. If it moves, the symptom is not a
// crash — every register-only computation still works and only accesses
// through the baked-in absolute addresses go wrong, i.e. "mostly fine, reads
// garbage out of globals". So the checksum below deliberately depends on
// every data section separately, and the program reports which one broke
// rather than a single opaque number.
//
// Build: see build_globals_guest.sh (clang -target x86_64-unknown-linux-gnu
// -ffreestanding -nostdlib -fno-pic -mcmodel=small, linked by mkaotguest.py).
//
typedef unsigned long u64;
typedef unsigned int u32;

// --- .rodata --------------------------------------------------------------
static const u64 kRoTable[16] = {
        0x0000000000000001ull, 0x0000000000000102ull, 0x0000000000010203ull,
        0x0000000001020304ull, 0x0000000102030405ull, 0x0000010203040506ull,
        0x0001020304050607ull, 0x0102030405060708ull, 0x1122334455667788ull,
        0x99AABBCCDDEEFF00ull, 0xDEADBEEFCAFEBABEull, 0x0123456789ABCDEFull,
        0xFEDCBA9876543210ull, 0x5555555555555555ull, 0xAAAAAAAAAAAAAAAAull,
        0xFFFFFFFFFFFFFFFFull,
};

// --- .data ----------------------------------------------------------------
u64 g_seed = 0x243F6A8885A308D3ull;
u64 g_counter = 0;
u32 g_flags = 0xC0FFEEu;
// A pointer initialized to another global's address: this one only survives
// if BOTH objects kept their linked addresses.
const u64* g_table_ptr = kRoTable;

// --- .bss -----------------------------------------------------------------
u64 g_scratch[256];
u64 g_result_ro;
u64 g_result_data;
u64 g_result_bss;
u64 g_result_ptr;

static u64 Mix(u64 h, u64 v) {
    h ^= v;
    h *= 0x100000001B3ull;
    h ^= h >> 29;
    return h;
}

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

static char out_buf[512];

static u64 Emit(u64 pos, const char* label, u64 value) {
    for (const char* p = label; *p; ++p) {
        out_buf[pos++] = *p;
    }
    out_buf[pos++] = '=';
    for (int i = 15; i >= 0; --i) {
        const u64 nib = (value >> (i * 4)) & 0xF;
        out_buf[pos++] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
    out_buf[pos++] = '\n';
    return pos;
}

// Kept out of line so each becomes its own STT_FUNC and the AOT compiler has
// more than one function to find.
__attribute__((noinline)) static u64 SumRodata(void) {
    u64 h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 16; ++i) {
        h = Mix(h, kRoTable[i]);
    }
    return h;
}

__attribute__((noinline)) static u64 ChurnData(void) {
    u64 h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 64; ++i) {
        g_seed = g_seed * 6364136223846793005ull + 1442695040888963407ull;
        g_counter += (g_seed >> 33);
        g_flags ^= (u32)(g_seed >> 11);
        h = Mix(h, g_seed ^ g_counter ^ g_flags);
    }
    return Mix(h, g_counter);
}

__attribute__((noinline)) static u64 ChurnBss(void) {
    for (int i = 0; i < 256; ++i) {
        g_scratch[i] = (u64)i * 0x9E3779B97F4A7C15ull;
    }
    for (int round = 0; round < 4; ++round) {
        for (int i = 0; i < 256; ++i) {
            g_scratch[i] ^= g_scratch[(i * 7 + 1) & 255] + (u64)round;
        }
    }
    u64 h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 256; ++i) {
        h = Mix(h, g_scratch[i]);
    }
    return h;
}

__attribute__((noinline)) static u64 ThroughPointer(void) {
    u64 h = 0xCBF29CE484222325ull;
    for (int i = 0; i < 16; ++i) {
        h = Mix(h, g_table_ptr[i]);
    }
    // The pointer's own value: if .rodata moved, this differs even when the
    // contents happen to look the same.
    return Mix(h, (u64)(unsigned long)g_table_ptr);
}

void _start(void) {
    g_result_ro = SumRodata();
    g_result_data = ChurnData();
    g_result_bss = ChurnBss();
    g_result_ptr = ThroughPointer();

    u64 pos = 0;
    pos = Emit(pos, "rodata", g_result_ro);
    pos = Emit(pos, "data", g_result_data);
    pos = Emit(pos, "bss", g_result_bss);
    pos = Emit(pos, "ptr", g_result_ptr);
    pos = Emit(pos, "all",
               Mix(Mix(Mix(g_result_ro, g_result_data), g_result_bss), g_result_ptr));
    Write(1, out_buf, pos);
    Exit(77);
}
