//
// Created by 甘尧 on 2024/7/12.
//

#include "linker_bridge.h"
#include "linker.h"

namespace swift::linux {

static std::unordered_map<std::string, ShadowLib> shadow_libs;

}

using namespace swift::linux;

extern "C" swift_linux_arch swift_linux_current_arch() {
    return ARCH_X86_64;
}

extern "C" swift_linux_host_so *swift_linux_load_so(const char *soname) {
    if (auto itr = shadow_libs.find(soname); itr != shadow_libs.end()) {
        return &itr->second;
    } else {
        // Do load

    }
}

extern "C" void swift_linux_unload_so(swift_linux_host_so *dso) {

}

extern "C" void* swift_linux_load_symbol(swift_linux_host_so *dso, const char *name) {

}
