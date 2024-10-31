//
// Created by 甘尧 on 2023/9/7.
//

#pragma once

#include <cstdint>
#include <span>

namespace swift::runtime {

using LocationDescriptor = VAddr;

struct UniformMapDesc {
    std::uint32_t offset;
    std::uint16_t size;
    std::uint16_t reg;
    bool is_float;

    constexpr UniformMapDesc(uint32_t offset, uint32_t size, uint32_t reg, bool f)
            : offset(offset), size(size), reg(reg), is_float(f) {}
};

enum ISA : uint8_t { kNone = 0, kArm, kArm64, kX86, kX86_64, kRiscv32, kRiscv64, kLoongArch };

enum class Optimizations : std::uint32_t {
    None = 0,
    ReturnStackBuffer = 1 << 0,
    FunctionBaseCompile = 1 << 1,
    MemoryToRegister = 1 << 2,
    FlagElimination = 1 << 3,
    ConstantFolding = 1 << 4,
    StaticCode = 1 << 5,
    BlockLink = 1 << 6,
    DirectBlockLink = 1 << 7,
    StaticModuleOps = StaticCode | DirectBlockLink,
    ConstMemoryFolding = 1 << 8,
    UniformElimination = 1 << 9,
    LocalElimination = 1 << 10,
    DeadCodeRemove = 1 << 11,
    All = UINT32_MAX
};

DECLARE_ENUM_FLAG_OPERATORS(Optimizations)

enum class Arm64Features : std::uint32_t {
    None = 0,
    AES = 1 << 1,
    CRC32 = 1 << 2,
    SHA = 1 << 3,
    Atomics = 1 << 4,
    RNG = 1 << 5,
    AFP = 1 << 6,
    RCpc = 1 << 7,
    RCpcImm = 1 << 8,
    Pmull1Q = 1 << 9,
    CSSC = 1 << 10,
    Fcma = 1 << 11,
    FlagM = 1 << 12,
    AXFlag = 1 << 13,
    RPRES = 1 << 14
};

DECLARE_ENUM_FLAG_OPERATORS(Arm64Features)

class MemoryInterface {
public:
    virtual bool Read(void* dest, size_t addr, size_t size) = 0;
    virtual bool Write(void* src, size_t addr, size_t size) = 0;
    virtual void* GetPointer(void* src) = 0;

    template <typename T> T Read(size_t addr = 0) {
        T t;
        Read(&t, addr, sizeof(T));
        return std::move(t);
    }

    template <typename T> void Write(T& t, size_t addr) { Write(&t, addr, sizeof(T)); }
};

struct Config {
    LocationDescriptor loc_start;
    LocationDescriptor loc_end;
    bool enable_jit;
    bool enable_asm_interp;
    bool has_local_operation;
    ISA backend_isa;
    std::uint32_t uniform_buffer_size;
    std::span<UniformMapDesc> buffers_static_alloc;  // 静态分配
    bool static_program;
    Optimizations global_opts;
    Arm64Features arm64_features{Arm64Features::None};
    std::uint32_t stack_alignment;
    void* page_table;
    void* memory_base;
    MemoryInterface* memory;
};

}  // namespace swift::runtime