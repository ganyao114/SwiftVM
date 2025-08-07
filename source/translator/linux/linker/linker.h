//
// Created by 甘尧 on 2024/7/18.
//

#pragma once

#include <unordered_map>
#include <elf.h>
#include "base/types.h"
#include "linker_bridge.h"

namespace swift::linux {

struct ShadowSymbol {
    void *bridge;
    void *target;
    Elf64_Sym sym;
};

struct ShadowLdso {

};

class ShadowLib : public swift_linux_host_so {
public:
    explicit ShadowLib(std::string path);

    ShadowSymbol *LoadSymbol(const char *name);

private:
    std::string path;
    void *host_ldso{};
    ShadowLdso shadow_ldso{};
    std::unordered_map<std::string, ShadowSymbol> shadow_symbols;
};

}
