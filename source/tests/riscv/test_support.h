#pragma once

#include <array>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include "runtime/backend/context.h"
#include "runtime/backend/riscv64/memory_protocol.h"
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
    rv::MemoryParticipant memory_participant;
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 4096> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    StateStorage() { state->spill_area[backend::kRiscvMemoryParticipantSlot] = reinterpret_cast<u64>(&memory_participant); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const {
        u64 result{}; std::memcpy(&result, state->uniform_buffer_begin + offset, 8); return result;
    }
};

inline Config TestConfig() {
    // The address-node index reserves eight bytes per MiB of guest range.
    // UINT64_MAX needs 128 TiB of contiguous VA, beyond native SV39 hosts.
    // A 48-bit test range covers the board's code pointers and reserves 2 GiB.
    return {.loc_start = 0, .loc_end = u64{1} << 48, .enable_jit = true,
            .backend_isa = kRiscv64, .uniform_buffer_size = 4096,
            .static_program = true, .global_opts = Optimizations::None,
            .stack_alignment = 16};
}

#if defined(__riscv) && __riscv_xlen == 64
extern "C" int SwiftRiscvCheckABI(BlockFn fn, backend::State* state);
extern "C" u64 SwiftRiscvFaultHelper(u64 address);
#endif

// Direct entry calls and every worker join must finish before this guard's scope ends.
class QuiescentRawCache final {
public:
    explicit QuiescentRawCache(backend::CodeCache& cache) : cache(cache) {}
    ~QuiescentRawCache() noexcept {
        if (!cache.ReleaseAfterQuiescence().Complete()) {
            std::fputs("FAIL: quiescent raw cache release incomplete\n", stderr);
            std::terminate();
        }
    }
    QuiescentRawCache(const QuiescentRawCache&) = delete;
    QuiescentRawCache& operator=(const QuiescentRawCache&) = delete;
    QuiescentRawCache(QuiescentRawCache&&) = delete;
    QuiescentRawCache& operator=(QuiescentRawCache&&) = delete;

private:
    backend::CodeCache& cache;
};

struct Compiled {
    Config config{TestConfig()};
    rv::JitContext context;
    rv::JitTranslator translator{context};
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    QuiescentRawCache release_cache{cache};
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
void NativeScalarShifts(bool zbb = false);
void ScalarShiftBenchmark();
void ScalarFeatureBenchmark();
void NativeFlags();
void NativeAtomics();
void NativeWide(bool vector = false);
void MemoryProtocol();
void NativeVectors(bool vector = false);
void NativeVectorInteger(bool vector = false);
void NativeVectorShuffle(bool vector = false);
void NativeLocals(bool vector = false);
void NativeVectorFloat(bool vector = false);
void NativeCalls(bool vector = false);
void NativeMemoryCopy(bool vector = false, bool zacas = false);
void NativeMemoryFrame(bool vector = false);
void NativeCrypto(bool vector = false, bool scalar_crypto = false, bool vector_crypto = false);
void NativeHostRegisters(bool vector = false);
void NativePhi(bool vector = false);

}  // namespace swift::tests::riscv
