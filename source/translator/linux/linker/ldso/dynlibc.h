//
// Created by 甘尧 on 2024/7/12.
//

#ifndef SWIFTVM_DYNLIBC_H
#define SWIFTVM_DYNLIBC_H

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include "dynconfig.h"

struct tls_module {
    struct tls_module *next;
    void *image;
    size_t len, size, align, offset;
};

struct __libc {
    char can_do_threads;
    char threaded;
    char secure;
    volatile signed char need_locks;
    int threads_minus_1;
    size_t *auxv;
    struct tls_module *tls_head;
    size_t tls_size, tls_align, tls_cnt;
    size_t page_size;
};

#ifndef PAGE_SIZE
#define PAGE_SIZE libc.page_size
#endif

extern hidden struct __libc __libc;
#define libc __libc

hidden void __init_libc(char **, char *);
hidden void __init_tls(size_t *);
hidden void __init_ssp(void *);
hidden void __libc_start_init(void);
hidden void __funcs_on_exit(void);
hidden void __funcs_on_quick_exit(void);
hidden void __libc_exit_fini(void);
hidden void __fork_handler(int);

extern hidden size_t __hwcap;
extern hidden size_t __sysinfo;
extern char *__progname, *__progname_full;

extern hidden const char __libc_version[];

hidden void __synccall(void (*)(void *), void *);
hidden int __setxid(int, int, int, int);

#endif  // SWIFTVM_DYNLIBC_H
