//
// Created by 甘尧 on 2024/7/18.
//

#include "linker.h"
#include "base/logging.h"
#include <dlfcn.h>
#include <cstring>
#include <memory>
#include <sys/mman.h>

namespace swift::linux {

// X86_64 跳板函数生成器实现
class X86_64BridgeGenerator : public BridgeGenerator {
public:
    void* CreateBridge(const char* symbol_name, void* host_addr) override {
        // TODO: 实现 x86_64 架构的跳板函数生成
        // 这里需要生成 guest 架构到 host 架构的调用桥接代码
        // 暂时返回 host 地址作为占位符
        swift::log::LogMessage(swift::log::Level::Debug, __FILE__, __LINE__, __FUNCTION__,
                              "Creating x86_64 bridge for symbol '{}' at host addr 0x{:x}",
                              symbol_name, reinterpret_cast<uintptr_t>(host_addr));
        return host_addr;
    }
    
    void DestroyBridge(void* bridge) override {
        // TODO: 实现跳板函数的清理
    }
};

// X86 跳板函数生成器实现
class X86BridgeGenerator : public BridgeGenerator {
public:
    void* CreateBridge(const char* symbol_name, void* host_addr) override {
        // TODO: 实现 x86 架构的跳板函数生成
        swift::log::LogMessage(swift::log::Level::Debug, __FILE__, __LINE__, __FUNCTION__,
                              "Creating x86 bridge for symbol '{}' at host addr 0x{:x}",
                              symbol_name, reinterpret_cast<uintptr_t>(host_addr));
        return host_addr;
    }
    
    void DestroyBridge(void* bridge) override {
        // TODO: 实现跳板函数的清理
    }
};

std::unique_ptr<BridgeGenerator> CreateBridgeGenerator(swift_linux_arch arch) {
    switch (arch) {
        case ARCH_X86_64:
            return std::make_unique<X86_64BridgeGenerator>();
        case ARCH_X86:
            return std::make_unique<X86BridgeGenerator>();
        default:
            swift::log::LogMessage(swift::log::Level::Error, __FILE__, __LINE__, __FUNCTION__,
                                  "Unsupported architecture: {}", static_cast<int>(arch));
            return nullptr;
    }
}

ShadowLib::ShadowLib(std::string path) : path(std::move(path)) {
    // 尝试加载 host 库
    host_ldso = dlopen(this->path.c_str(), RTLD_LAZY);
    if (host_ldso) {
        swift::log::LogMessage(swift::log::Level::Debug, __FILE__, __LINE__, __FUNCTION__,
                              "Successfully loaded host library: {}", this->path);
    } else {
        swift::log::LogMessage(swift::log::Level::Debug, __FILE__, __LINE__, __FUNCTION__,
                              "Failed to load host library: {}, error: {}", this->path, dlerror());
    }
    
    // 创建跳板生成器
    bridge_generator = CreateBridgeGenerator(swift_linux_current_arch());
}

ShadowLib::~ShadowLib() {
    // 清理所有跳板函数
    if (bridge_generator) {
        for (auto& [name, symbol] : shadow_symbols) {
            if (symbol.bridge && symbol.has_host_impl) {
                bridge_generator->DestroyBridge(symbol.bridge);
            }
        }
    }
    
    // 卸载 host 库
    if (host_ldso) {
        dlclose(host_ldso);
    }
}

ShadowSymbol* ShadowLib::LoadSymbol(const char* name) {
    // 首先检查缓存
    auto it = shadow_symbols.find(name);
    if (it != shadow_symbols.end()) {
        return &it->second;
    }
    
    // 创建新的 shadow symbol
    ShadowSymbol shadow_sym = {};
    shadow_sym.has_host_impl = false;
    shadow_sym.bridge = nullptr;
    shadow_sym.target = nullptr;
    
    // 检查是否有 host 实现
    if (host_ldso) {
        void *host_addr = dlsym(host_ldso, name);
        if (host_addr) {
            shadow_sym.has_host_impl = true;
            shadow_sym.target = host_addr;
            // 创建跳板函数
            shadow_sym.bridge = CreateBridge(name, host_addr);
            
            swift::log::LogMessage(swift::log::Level::Debug, __FILE__, __LINE__, __FUNCTION__,
                                  "Found host symbol '{}' in library '{}', created bridge at 0x{:x}",
                                  name, path, reinterpret_cast<uintptr_t>(shadow_sym.bridge));
        } else {
            swift::log::LogMessage(swift::log::Level::Debug, __FILE__, __LINE__, __FUNCTION__,
                                  "Host symbol '{}' not found in library '{}'", name, path);
        }
    }
    
    // 设置 ELF 符号信息
    shadow_sym.sym.st_name = 0; // 名称偏移，实际由调用者管理
    shadow_sym.sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
    shadow_sym.sym.st_other = 0;
    shadow_sym.sym.st_shndx = SHN_UNDEF; // 如果没有 host 实现则未定义
    shadow_sym.sym.st_value = shadow_sym.has_host_impl ? 
        reinterpret_cast<Elf64_Addr>(shadow_sym.bridge) : 0;
    shadow_sym.sym.st_size = 0;
    
    if (shadow_sym.has_host_impl) {
        shadow_sym.sym.st_shndx = 1; // 假设在第一个段中
    }
    
    // 缓存符号
    auto [inserted_it, success] = shadow_symbols.emplace(name, shadow_sym);
    return success ? &inserted_it->second : nullptr;
}

bool ShadowLib::HasHostSymbol(const char* name) {
    if (!host_ldso) return false;
    
    void *addr = dlsym(host_ldso, name);
    return addr != nullptr;
}

void* ShadowLib::CreateBridge(const char* name, void* host_addr) {
    if (!bridge_generator) {
        swift::log::LogMessage(swift::log::Level::Error, __FILE__, __LINE__, __FUNCTION__,
                              "Bridge generator not available for symbol '{}'", name);
        return nullptr;
    }
    
    return bridge_generator->CreateBridge(name, host_addr);
}

}
