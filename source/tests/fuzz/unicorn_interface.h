#pragma once

#include <vector>
#include <cstdint>
#include <unicorn/unicorn.h>

namespace swift::test {

class UnicornInterface {
public:
    UnicornInterface(uc_arch arch, uc_mode mode);
    ~UnicornInterface();

    void MapMemory(uint64_t address, uint64_t size, uint32_t perms);
    void WriteMemory(uint64_t address, const std::vector<uint8_t>& data);
    std::vector<uint8_t> ReadMemory(uint64_t address, uint64_t size);

    void WriteRegister(int regid, uint64_t value);
    uint64_t ReadRegister(int regid);
    
    void WriteRegister128(int regid, const void* value);
    void ReadRegister128(int regid, void* value);

    void Run(uint64_t start_address, uint64_t end_address, uint64_t timeout = 0, size_t count = 0);

    uc_engine* GetEngine() { return uc; }

private:
    uc_engine* uc;
};

}
