//
// Created by 甘尧 on 2024/7/18.
//

#pragma once

#include <unordered_map>
#include <elf.h>
#include "base/types.h"
#include "linker_bridge.h"

namespace swift::linux {

// 架构特定的跳板函数生成器接口
class BridgeGenerator {
public:
    virtual ~BridgeGenerator() = default;
    virtual void* CreateBridge(const char* symbol_name, void* host_addr) = 0;
    virtual void DestroyBridge(void* bridge) = 0;
};

// 工厂函数，根据当前架构创建相应的跳板生成器
std::unique_ptr<BridgeGenerator> CreateBridgeGenerator(swift_linux_arch arch);

struct ShadowSymbol {
    void *bridge;       // guest 架构的跳板函数地址
    void *target;       // host 架构的目标函数地址
    Elf64_Sym sym;      // 符号信息
    bool has_host_impl; // 是否有 host 架构实现
};

struct ShadowLdso {

};

class ShadowLib : public swift_linux_host_so {
public:
    explicit ShadowLib(std::string path);
    ~ShadowLib();

    ShadowSymbol *LoadSymbol(const char *name);
    bool HasHostSymbol(const char *name);
    void *CreateBridge(const char *name, void *host_addr);

private:
    std::string path;
    void *host_ldso{};
    ShadowLdso shadow_ldso{};
    std::unordered_map<std::string, ShadowSymbol> shadow_symbols;
    std::unique_ptr<BridgeGenerator> bridge_generator;
};

}
