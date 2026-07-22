#include "unicorn_interface.h"
#include <stdexcept>
#include <string>

namespace swift::test {

UnicornInterface::UnicornInterface(uc_arch arch, uc_mode mode) {
    uc_err err = uc_open(arch, mode, &uc);
    if (err != UC_ERR_OK) {
        throw std::runtime_error("Failed on uc_open() with error returned: " + std::to_string(err));
    }
}

UnicornInterface::~UnicornInterface() {
    uc_close(uc);
}

void UnicornInterface::MapMemory(uint64_t address, uint64_t size, uint32_t perms) {
    uc_err err = uc_mem_map(uc, address, size, perms);
    if (err != UC_ERR_OK) {
        throw std::runtime_error("Failed on uc_mem_map() with error returned: " + std::to_string(err));
    }
}

void UnicornInterface::WriteMemory(uint64_t address, const std::vector<uint8_t>& data) {
    uc_err err = uc_mem_write(uc, address, data.data(), data.size());
    if (err != UC_ERR_OK) {
        throw std::runtime_error("Failed on uc_mem_write() with error returned: " + std::to_string(err));
    }
}

std::vector<uint8_t> UnicornInterface::ReadMemory(uint64_t address, uint64_t size) {
    std::vector<uint8_t> data(size);
    uc_err err = uc_mem_read(uc, address, data.data(), size);
    if (err != UC_ERR_OK) {
        throw std::runtime_error("Failed on uc_mem_read() with error returned: " + std::to_string(err));
    }
    return data;
}

void UnicornInterface::WriteRegister(int regid, uint64_t value) {
    uc_err err = uc_reg_write(uc, regid, &value);
    if (err != UC_ERR_OK) {
        throw std::runtime_error("Failed on uc_reg_write() with error returned: " + std::to_string(err));
    }
}

uint64_t UnicornInterface::ReadRegister(int regid) {
    uint64_t value;
    uc_err err = uc_reg_read(uc, regid, &value);
    if (err != UC_ERR_OK) {
        throw std::runtime_error("Failed on uc_reg_read() with error returned: " + std::to_string(err));
    }
    return value;
}

void UnicornInterface::WriteRegister128(int regid, const void* value) {
    uc_err err = uc_reg_write(uc, regid, value);
    if (err != UC_ERR_OK) {
        throw std::runtime_error("Failed on uc_reg_write() with error returned: " + std::to_string(err));
    }
}

void UnicornInterface::ReadRegister128(int regid, void* value) {
    uc_err err = uc_reg_read(uc, regid, value);
    if (err != UC_ERR_OK) {
        throw std::runtime_error("Failed on uc_reg_read() with error returned: " + std::to_string(err));
    }
}

void UnicornInterface::Run(uint64_t start_address, uint64_t end_address, uint64_t timeout, size_t count) {
    uc_err err = uc_emu_start(uc, start_address, end_address, timeout, count);
    if (err != UC_ERR_OK) {
        throw std::runtime_error("Failed on uc_emu_start() with error returned: " + std::to_string(err));
    }
}

}
