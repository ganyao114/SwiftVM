#pragma once

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include "runtime/backend/context.h"
#include "runtime/backend/riscv64/jit/translator.h"

namespace swift::tests::riscv {

using namespace swift::runtime;
namespace rv = swift::runtime::backend::riscv64;
using BlockFn = HaltReason (*)(backend::State*);
inline u64 checks{};
inline void Check(bool condition, const std::string& detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}

struct StateStorage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 4096> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const {
        u64 result{}; std::memcpy(&result, state->uniform_buffer_begin + offset, 8); return result;
    }
};

inline Config TestConfig() {
    return {.loc_start = 0, .loc_end = UINT64_MAX, .enable_jit = true,
            .backend_isa = kRiscv64, .uniform_buffer_size = 4096,
            .static_program = true, .global_opts = Optimizations::None,
            .stack_alignment = 16};
}

#if defined(__riscv) && __riscv_xlen == 64
extern "C" int SwiftRiscvCheckABI(BlockFn fn, backend::State* state);
extern "C" u64 SwiftRiscvFaultHelper(u64 address);
#endif

struct Compiled {
    Config config{TestConfig()};
    rv::JitContext context;
    rv::JitTranslator translator{context};
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    BlockFn fn{};
    explicit Compiled(ir::Block* block, bool cache_scalars = true,
                      rv::HostFeatures features = rv::HostFeatures::Detect(),
                      std::span<const UniformMapDesc> bindings = {}) : context(cache_scalars, features, bindings) {
        translator.Translate(block);
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate RV64 test code");
        context.Flush(*buffer);
        fn = reinterpret_cast<BlockFn>(buffer->exec_data);
    }
};

void NativeScalarBits(bool zbb = false);
void NativeScalarALU(bool zbb = false);
void NativeFlags();
void NativeAtomics();
void NativeVectors(bool vector = false);
void NativeVectorInteger(bool vector = false);
void NativeVectorShuffle(bool vector = false);
void NativeLocals(bool vector = false);
void NativeVectorFloat(bool vector = false);
void NativeCalls(bool vector = false);
void NativeMemoryCopy(bool vector = false, bool zacas = false);
void NativeCrypto(bool vector = false, bool scalar_crypto = false, bool vector_crypto = false);
void NativeHostRegisters(bool vector = false);
void NativePhi(bool vector = false);

}  // namespace swift::tests::riscv
