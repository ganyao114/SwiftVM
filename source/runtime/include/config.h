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
    // Bit 2 stays reserved: global_opts is hashed into persisted JIT cache and
    // AOT validity keys, so reusing it would change the meaning of old keys.
    FlagElimination = 1 << 3,
    ConstantFolding = 1 << 4,
    StaticCode = 1 << 5,
    BlockLink = 1 << 6,
    DirectBlockLink = 1 << 7,
    StaticModuleOps = StaticCode | DirectBlockLink,
    // Bit 8 stays reserved: global_opts is hashed into persisted JIT cache and
    // AOT validity keys, so reusing it would change the meaning of old keys.
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

// Guest memory ordering model: how strictly guest loads/stores are ordered on
// a (weakly ordered) host.
//   Relaxed:  no ordering — correct for single-threaded guests (default).
//   AcqRel:   every guest load acquires / every store releases (TSO
//             compatible); the frontend emits LoadMemoryTSO/StoreMemoryTSO
//             and the ARM64 backend surrounds plain accesses with barriers.
//   Hardware: the host enforces TSO itself (e.g. Apple silicon TSO mode,
//             Linux PR_SET_MEM_MODEL_TSO); codegen matches Relaxed.
enum class TsoMode : std::uint8_t { Relaxed, AcqRel, Hardware };

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
    // Memory ordering for guest accesses (frontends read this and install it
    // via their mode hook, e.g. x86::SetTsoMode). Hardware mode is a promise
    // from the embedder that the host already runs in a TSO memory model.
    TsoMode tso_mode{TsoMode::Relaxed};
    std::uint32_t stack_alignment;
    void* page_table;
    void* memory_base;
    // Bounded guest window: every guest address is truncated to this mask
    // *before* memory_base is added, so `guest & mask + memory_base` can only
    // ever name the embedder's own [memory_base, memory_base + mask + 1)
    // reservation — a wild guest pointer aliases inside the window instead of
    // reaching host memory. Must be 2^n - 1. 0 = disabled (unbounded bias;
    // the guest can then address arbitrary host memory and it is the
    // embedder's job to prove it will not).
    std::uint64_t guest_addr_mask{0};
    MemoryInterface* memory;
};

}  // namespace swift::runtime
