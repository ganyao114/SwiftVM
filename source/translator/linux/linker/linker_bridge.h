//
// Created by 甘尧 on 2024/7/12.
//

#pragma once

enum swift_linux_arch {
    ARCH_X86,
    ARCH_X86_64
};

struct swift_linux_host_so {};

extern "C" swift_linux_arch swift_linux_current_arch();
extern "C" swift_linux_host_so *swift_linux_load_so(const char *soname);
extern "C" void swift_linux_unload_so(swift_linux_host_so *dso);
extern "C" void* swift_linux_load_symbol(swift_linux_host_so *dso, const char *name);

