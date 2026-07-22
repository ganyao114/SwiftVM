//
// Created by 甘尧 on 2024/7/12.
//

#include "linker_bridge.h"
#include "linker.h"
#include <dlfcn.h>
#include <unordered_map>

namespace swift::linux {

static std::unordered_map<std::string, ShadowLib> shadow_libs;
static std::string last_error;

}

using namespace swift::linux;

extern "C" swift_linux_arch swift_linux_current_arch() {
    return ARCH_X86_64;
}

extern "C" swift_linux_host_so *swift_linux_load_so(const char *soname) {
    try {
        if (auto itr = shadow_libs.find(soname); itr != shadow_libs.end()) {
            return &itr->second;
        } else {
            // 加载新的 host 库
            auto [inserted_itr, success] = shadow_libs.emplace(soname, ShadowLib(soname));
            if (success) {
                return &inserted_itr->second;
            }
            last_error = "Failed to create ShadowLib for " + std::string(soname);
            return nullptr;
        }
    } catch (const std::exception& e) {
        last_error = "Exception loading library " + std::string(soname) + ": " + e.what();
        return nullptr;
    }
}

extern "C" void swift_linux_unload_so(swift_linux_host_so *dso) {
    // TODO: 实现库卸载逻辑
}

extern "C" Elf64_Sym *swift_linux_load_symbol(swift_linux_host_so *dso, const char *name) {
    if (!dso || !name) {
        last_error = "Invalid parameters to swift_linux_load_symbol";
        return nullptr;
    }
    
    try {
        ShadowLib *shadow_lib = static_cast<ShadowLib*>(dso);
        ShadowSymbol *shadow_sym = shadow_lib->LoadSymbol(name);
        
        return shadow_sym ? &shadow_sym->sym : nullptr;
    } catch (const std::exception& e) {
        last_error = "Exception loading symbol " + std::string(name) + ": " + e.what();
        return nullptr;
    }
}

extern "C" void *swift_linux_create_bridge(swift_linux_host_so *dso, const char *name, void *host_addr) {
    if (!dso || !name || !host_addr) {
        last_error = "Invalid parameters to swift_linux_create_bridge";
        return nullptr;
    }
    
    try {
        ShadowLib *shadow_lib = static_cast<ShadowLib*>(dso);
        return shadow_lib->CreateBridge(name, host_addr);
    } catch (const std::exception& e) {
        last_error = "Exception creating bridge for " + std::string(name) + ": " + e.what();
        return nullptr;
    }
}

extern "C" int swift_linux_has_host_symbol(swift_linux_host_so *dso, const char *name) {
    if (!dso || !name) return 0;
    
    try {
        ShadowLib *shadow_lib = static_cast<ShadowLib*>(dso);
        return shadow_lib->HasHostSymbol(name) ? 1 : 0;
    } catch (const std::exception& e) {
        last_error = "Exception checking symbol " + std::string(name) + ": " + e.what();
        return 0;
    }
}

extern "C" const char *swift_linux_get_last_error(void) {
    return last_error.c_str();
}
