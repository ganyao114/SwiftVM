#include "coremark.h"
#include <stdarg.h>

/* Linux x86-64 ABI; no libc, loader, TLS, filesystem, or dynamic allocation. */
static long sys3(long number, long first, long second, long third) {
    long result;
    __asm__ volatile("syscall" : "=a"(result) : "a"(number), "D"(first), "S"(second), "d"(third)
                     : "rcx", "r11", "memory");
    return result;
}
static __attribute__((noreturn)) void fail(void) {
    (void)sys3(60, 2, 0, 0);
    __builtin_unreachable();
}
static void write_all(const char *text, size_t length) {
    while (length) {
        const long written = sys3(1, 1, (long)text, (long)length);
        if (written == -4) continue;
        if (written <= 0) fail();
        text += written;
        length -= (size_t)written;
    }
}

static uint64_t microseconds(void) {
    struct { long seconds, nanoseconds; } now;
    if (sys3(228, 1, (long)&now, 0) < 0) fail(); /* clock_gettime(CLOCK_MONOTONIC) */
    return (uint64_t)now.seconds * 1000000 + (uint64_t)now.nanoseconds / 1000;
}
static uint64_t started, stopped;
void start_time(void) { started = microseconds(); }
void stop_time(void) { stopped = microseconds(); }
CORE_TICKS get_time(void) { return stopped - started; }
secs_ret time_in_secs(CORE_TICKS ticks) { return (secs_ret)(ticks / 1000000); }

ee_u32 default_num_contexts = 1;
static _Alignas(16) unsigned char arena[65536];
static size_t allocated;
void *portable_malloc(ee_size_t size) {
    const size_t at = (allocated + 15) & ~(size_t)15;
    if (!size || at > sizeof(arena) || size > sizeof(arena) - at) fail();
    allocated = at + size;
    return arena + at;
}
void portable_free(void *pointer) { (void)pointer; }
void portable_init(core_portable *port, int *argc, char *argv[]) {
    (void)argc; (void)argv;
    allocated = 0;
    port->portable_id = 1;
}
void portable_fini(core_portable *port) { port->portable_id = 0; }

/* Compiler-generated structure initialization can call these without libc.
 * These are platform routines; all official benchmark files use identical O2. */
void *memcpy(void *destination, const void *source, size_t size) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < size; ++i) out[i] = in[i];
    return destination;
}
void *memset(void *destination, int value, size_t size) {
    unsigned char *out = destination;
    for (size_t i = 0; i < size; ++i) out[i] = (unsigned char)value;
    return destination;
}
void *memmove(void *destination, const void *source, size_t size) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    if ((uintptr_t)out < (uintptr_t)in) return memcpy(out, in, size);
    for (size_t i = size; i; --i) out[i - 1] = in[i - 1];
    return destination;
}

struct text_buffer { char bytes[768]; size_t used; int total; };
static void put(struct text_buffer *out, char value) {
    if (out->used == sizeof(out->bytes)) { write_all(out->bytes, out->used); out->used = 0; }
    out->bytes[out->used++] = value;
    ++out->total;
}
static void number(struct text_buffer *out, unsigned long long value, unsigned base,
                   unsigned width, char padding, int negative) {
    const char digits[] = "0123456789abcdef";
    char reversed[32]; unsigned count = 0;
    do { reversed[count++] = digits[value % base]; value /= base; } while (value);
    if (negative && padding == '0') { put(out, '-'); negative = 0; if (width) --width; }
    while (width > count + (unsigned)negative) { put(out, padding); --width; }
    if (negative) put(out, '-');
    while (count) put(out, reversed[--count]);
}
int ee_printf(const char *format, ...) {
    struct text_buffer out; out.used = 0; out.total = 0;
    va_list args; va_start(args, format);
    while (*format) {
        if (*format != '%') { put(&out, *format++); continue; }
        ++format;
        char padding = ' ';
        if (*format == '0') { padding = '0'; ++format; }
        unsigned width = 0;
        while (*format >= '0' && *format <= '9') width = width * 10 + (unsigned)(*format++ - '0');
        unsigned longs = 0;
        while (*format == 'l') { ++longs; ++format; }
        const char specifier = *format++;
        if (specifier == 's') {
            const char *text = va_arg(args, const char *);
            if (!text) text = "(null)";
            while (*text) put(&out, *text++);
        } else if (specifier == 'd' || specifier == 'i') {
            const long long value = longs > 1 ? va_arg(args, long long) : longs ? va_arg(args, long) : va_arg(args, int);
            const unsigned long long magnitude = value < 0 ? 0 - (unsigned long long)value : (unsigned long long)value;
            number(&out, magnitude, 10, width, padding, value < 0);
        } else if (specifier == 'u' || specifier == 'x') {
            const unsigned long long value = longs > 1 ? va_arg(args, unsigned long long) : longs ? va_arg(args, unsigned long) : va_arg(args, unsigned);
            number(&out, value, specifier == 'x' ? 16 : 10, width, padding, 0);
        } else if (specifier == '%') put(&out, '%');
        else fail(); /* No guessed output for an unimplemented format. */
    }
    va_end(args);
    write_all(out.bytes, out.used);
    return out.total;
}
