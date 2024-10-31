//
// Created by 甘尧 on 2024/7/18.
//

#pragma once

#include <unordered_map>
#include "base/types.h"
#include "linker_bridge.h"

namespace swift::linux {

struct ShadowSymbol {
    void *bridge;
    void *target;
};

class ShadowLib : public swift_linux_host_so {
public:
    explicit ShadowLib(std::string path);

private:
    std::string path;
    void *host_ldso{};
    std::unordered_map<std::string, ShadowSymbol> shadow_symbols;
};

}
