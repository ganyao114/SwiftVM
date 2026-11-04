#ifndef SWIFTVM_COREMARK_LINUX_PORT_H
#define SWIFTVM_COREMARK_LINUX_PORT_H

/* Linux syscall-only platform port. The official CoreMark files are unchanged.
 * HAS_FLOAT=0 selects the official integer timing/report path, not a kernel
 * substitution. -O2 and the compiler's normal vectorization remain enabled. */
#include <stddef.h>
#include <stdint.h>

#define HAS_FLOAT 0
#define HAS_TIME_H 0
#define HAS_STDIO 0
#define HAS_PRINTF 0
#define SEED_METHOD SEED_ARG
#define MEM_METHOD MEM_MALLOC
#define MULTITHREAD 1
#define MAIN_HAS_NOARGC 0
#define MAIN_HAS_NORETURN 0
#define COMPILER_VERSION __clang_version__
#define COMPILER_FLAGS "-O2 -ffreestanding -fno-stack-protector -fno-pie"
#define MEM_LOCATION "single-context static allocation arena (malloc port)"

typedef int16_t ee_s16;
typedef uint16_t ee_u16;
typedef int32_t ee_s32;
typedef uint32_t ee_u32;
typedef uint8_t ee_u8;
typedef double ee_f32;
typedef uintptr_t ee_ptr_int;
typedef size_t ee_size_t;
typedef uint64_t CORE_TICKS;
#define align_mem(x) ((void *)((((ee_ptr_int)(x)) + 3) & ~(ee_ptr_int)3))

typedef struct CORE_PORTABLE_S { ee_u8 portable_id; } core_portable;
extern ee_u32 default_num_contexts;
void portable_init(core_portable *, int *, char *[]);
void portable_fini(core_portable *);
int ee_printf(const char *, ...);

#endif
