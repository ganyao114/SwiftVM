//
// Created by 甘尧 on 2023/9/7.
//

#pragma once

#include <cstdint>
#include <span>

namespace swift::runtime {

using LocationDescriptor = size_t;

struct UniformMapDesc {
    std::uint32_t offset;
    std::uint16_t size;
    std::uint16_t reg;
    bool is_float;
};

enum ISA : uint8_t {
    kNone = 0,
    kArm,
    kArm64,
    kX86,
    kX86_64,
    kRiscv32,
    kRiscv64,
    kLoongArch
};

class MemoryInterface {
public:
    virtual bool Read(void* dest, size_t offset, size_t size) = 0;
    virtual bool Write(void* src, size_t offset, size_t size) = 0;
    virtual void *GetPointer(void* src) = 0;

    template <typename T> T Read(size_t offset = 0) {
        T t;
        Read(&t, offset, sizeof(T));
        return std::move(t);
    }

    template <typename T> void Write(T& t, size_t offset) { Write(&t, offset, sizeof(T)); }
};

struct Config {
    LocationDescriptor loc_start;
    LocationDescriptor loc_end;
    bool enable_jit;
    bool enable_asm_interp;
    bool enable_rsb;
    bool has_local_operation;
    std::uint32_t uniform_buffer_size;
    ISA backend_isa;
    std::vector<UniformMapDesc> buffers_static_alloc; // 静态分配建议
    std::uint32_t stack_alignment;
    void *page_table;
    void *memory_base;
    MemoryInterface *memory;
};

}