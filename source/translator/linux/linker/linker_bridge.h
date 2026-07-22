//
// Created by 甘尧 on 2024/7/12.
//

#pragma once

#include <elf.h>

enum swift_linux_arch {
    ARCH_X86,
    ARCH_X86_64
};

struct swift_linux_host_so {};

#ifdef __cplusplus
extern "C" {
#endif

enum swift_linux_arch swift_linux_current_arch();
struct swift_linux_host_so *swift_linux_load_so(const char *soname);
void swift_linux_unload_so(struct swift_linux_host_so *dso);
Elf64_Sym *swift_linux_load_symbol(struct swift_linux_host_so *dso, const char *name);
void *swift_linux_create_bridge(struct swift_linux_host_so *dso, const char *name, void *host_addr);
int swift_linux_has_host_symbol(struct swift_linux_host_so *dso, const char *name);
const char *swift_linux_get_last_error(void);

#ifdef __cplusplus
}
#endif

