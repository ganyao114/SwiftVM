// Native x86-64 reference: executes one guest instruction on the host CPU
// with the case's architectural state and reports the resulting state.
// Built ad hoc by native_probe.py; not part of the shipped runtime.
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdalign.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#include <unistd.h>

extern void svm_native_run(void* context, void* entry);

typedef struct {
    uint64_t gpr[16];
    uint64_t flags;
    uint32_t mxcsr;
    uint32_t reserved;
    uint8_t ymm[16][32];
    alignas(16) uint8_t fxsave[512];
} Context;

typedef struct {
    uint64_t address;
    uint64_t size;
    uint32_t protection;
    uint32_t reserved;
} Region;

enum { kHeaderBytes = 1216, kMaxCode = 16 };

static sigjmp_buf fault_jump;
static volatile int fault_signal;
static volatile uint64_t fault_address;

static void OnFault(int signal, siginfo_t* info, void*) {
    fault_signal = signal;
    fault_address = (uint64_t)(uintptr_t)info->si_addr;
    siglongjmp(fault_jump, 1);
}

static uint64_t LoadU64(const uint8_t* p) {
    uint64_t value;
    memcpy(&value, p, sizeof value);
    return value;
}

static uint32_t LoadU32(const uint8_t* p) {
    uint32_t value;
    memcpy(&value, p, sizeof value);
    return value;
}

static void StoreU64(uint8_t* p, uint64_t value) { memcpy(p, &value, sizeof value); }
static void StoreU32(uint8_t* p, uint32_t value) { memcpy(p, &value, sizeof value); }

static int UnderRosetta(void) {
#if defined(__APPLE__)
    int translated = 0;
    size_t size = sizeof translated;
    if (sysctlbyname("sysctl.proc_translated", &translated, &size, NULL, 0) == 0)
        return translated;
#endif
    return 0;
}

// Rosetta's FXSAVE tag word is one validity bit per physical register rather
// than the architectural abridged encoding. Convert in both directions so the
// probe still consumes and reports the case's x87 tag word.
static void FtwToRosetta(uint8_t* area) {
    uint16_t tag;
    memcpy(&tag, area + 4, 2);
    uint16_t bits = 0;
    for (int i = 0; i != 8; ++i)
        if (((tag >> (2 * i)) & 3) != 3) bits |= (uint16_t)(1u << i);
    memcpy(area + 4, &bits, 2);
}

static void RotateFxsave(uint8_t* area) {
    uint16_t status;
    memcpy(&status, area + 2, 2);
    const unsigned top = (status >> 11) & 7;
    if (!top) return;
    uint8_t saved[128];
    memcpy(saved, area + 32, sizeof saved);
    for (unsigned i = 0; i != 8; ++i)
        memcpy(area + 32 + 16 * ((top + i) & 7), saved + 16 * i, 16);
}

static void FtwFromRosetta(uint8_t* area) {
    uint16_t bits;
    memcpy(&bits, area + 4, 2);
    uint16_t tag = 0xffff;
    for (int i = 0; i != 8; ++i) {
        if (!(bits & (1u << i))) continue;
        uint64_t significand;
        uint16_t exponent;
        memcpy(&significand, area + 32 + 16 * i, 8);
        memcpy(&exponent, area + 40 + 16 * i, 2);
        const uint16_t kind = significand == 0 && exponent == 0 ? 1 : 0;
        tag = (uint16_t)((tag & ~(3u << (2 * i))) | (kind << (2 * i)));
    }
    memcpy(area + 4, &tag, 2);
}

static void* MapAt(uint64_t address, uint64_t size) {
    const long page = sysconf(_SC_PAGESIZE);
    const uint64_t base = address & ~(uint64_t)(page - 1);
    const uint64_t end = (address + size + page - 1) & ~(uint64_t)(page - 1);
    void* p = mmap((void*)(uintptr_t)base, end - base, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s input.bin output.bin\n", argv[0]);
        return 2;
    }
    FILE* input = fopen(argv[1], "rb");
    if (!input) {
        perror("input");
        return 2;
    }
    fseek(input, 0, SEEK_END);
    const long input_size = ftell(input);
    fseek(input, 0, SEEK_SET);
    uint8_t* bytes = malloc((size_t)input_size);
    if (!bytes || fread(bytes, 1, (size_t)input_size, input) != (size_t)input_size) {
        fprintf(stderr, "cannot read input\n");
        return 2;
    }
    fclose(input);
    if (input_size < kHeaderBytes || memcmp(bytes, "SVNPROB1", 8)) {
        fprintf(stderr, "bad input header\n");
        return 2;
    }
    const uint64_t code_address = LoadU64(bytes + 8);
    const uint64_t terminal_pc = LoadU64(bytes + 16);
    const uint32_t code_size = LoadU32(bytes + 24);
    const uint32_t region_count = LoadU32(bytes + 28);
    if (code_size > kMaxCode) {
        fprintf(stderr, "code too long\n");
        return 2;
    }
    Context context;
    memcpy(context.gpr, bytes + 48, sizeof context.gpr);
    context.flags = LoadU64(bytes + 176);
    context.mxcsr = LoadU32(bytes + 184);
    memcpy(context.ymm, bytes + 192, sizeof context.ymm);
    memcpy(context.fxsave, bytes + 704, sizeof context.fxsave);

    const uint8_t* cursor = bytes + kHeaderBytes;
    Region* regions = calloc(region_count, sizeof(Region));
    const uint8_t** contents = calloc(region_count, sizeof(uint8_t*));
    for (uint32_t i = 0; i != region_count; ++i) {
        regions[i].address = LoadU64(cursor);
        regions[i].size = LoadU64(cursor + 8);
        regions[i].protection = LoadU32(cursor + 16);
        const uint32_t stored = LoadU32(cursor + 20);
        cursor += 24;
        contents[i] = cursor;
        cursor += stored;
        if (cursor > bytes + input_size || stored > regions[i].size) {
            fprintf(stderr, "truncated region\n");
            return 2;
        }
        if (!MapAt(regions[i].address, regions[i].size)) {
            perror("mmap region");
            return 2;
        }
        memset((void*)(uintptr_t)regions[i].address, 0, regions[i].size);
        memcpy((void*)(uintptr_t)regions[i].address, contents[i], stored);
        regions[i].reserved = stored;
    }

    // The code bytes live outside the data regions; map their page and place
    // an absolute jump to the probe resume label right after the instruction.
    const long page = sysconf(_SC_PAGESIZE);
    const uint64_t code_page = code_address & ~(uint64_t)(page - 1);
    const uint64_t terminal_page = terminal_pc & ~(uint64_t)(page - 1);
    if (!MapAt(code_page, page) || (terminal_page != code_page && !MapAt(terminal_page, page))) {
        perror("mmap code");
        return 2;
    }
    memcpy((void*)(uintptr_t)code_address, bytes + 32, code_size);

    extern void svm_native_resume(void);
    uint8_t trampoline[14] = {0xff, 0x25, 0, 0, 0, 0};
    const uint64_t resume = (uint64_t)(uintptr_t)&svm_native_resume;
    memcpy(trampoline + 6, &resume, 8);
    memcpy((void*)(uintptr_t)terminal_pc, trampoline, sizeof trampoline);

    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = OnFault;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);

    const int rosetta = UnderRosetta();
    // FXSAVE stores an eight-bit valid mask, not the architectural tag word.
    FtwToRosetta(context.fxsave);
    uint32_t status = 0;
    uint64_t fault = 0;
    if (!sigsetjmp(fault_jump, 1)) {
        svm_native_run(&context, (void*)(uintptr_t)code_address);
    } else {
        status = 1;
        fault = fault_address;
    }
    if (rosetta) RotateFxsave(context.fxsave);
    FtwFromRosetta(context.fxsave);

    uint8_t* output = calloc(1, 1200);
    memcpy(output, "SVNOUT01", 8);
    StoreU32(output + 8, status);
    StoreU64(output + 16, fault);
    memcpy(output + 24, context.gpr, sizeof context.gpr);
    StoreU64(output + 152, context.flags);
    StoreU32(output + 160, context.mxcsr);
    memcpy(output + 168, context.ymm, sizeof context.ymm);
    StoreU32(output + 680, region_count);
    memcpy(output + 688, context.fxsave, sizeof context.fxsave);
    FILE* out = fopen(argv[2], "wb");
    if (!out) {
        perror("output");
        return 2;
    }
    fwrite(output, 1, 1200, out);
    for (uint32_t i = 0; i != region_count; ++i) {
        Region header = regions[i];
        fwrite(&header, sizeof header, 1, out);
        fwrite((void*)(uintptr_t)header.address, 1, header.size, out);
    }
    fclose(out);
    return 0;
}
