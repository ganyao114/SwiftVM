#pragma once

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include "runtime/backend/code_cache.h"
#include "runtime/backend/context.h"
#include "runtime/backend/loongarch64/jit/translator.h"

namespace swift::tests::loongarch64 {

using namespace swift::runtime;
namespace la = swift::runtime::backend::loongarch64;
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
        u64 result{};
        std::memcpy(&result, state->uniform_buffer_begin + offset, 8);
        return result;
    }
};

inline Config TestConfig() {
    return {.loc_start = 0, .loc_end = 1u << 20, .enable_jit = true,
            .backend_isa = kLoongArch, .uniform_buffer_size = 4096,
            .static_program = true, .global_opts = Optimizations::None,
            .stack_alignment = 16};
}

extern "C" int SwiftLoongArchCheckABI(BlockFn fn, backend::State* state);
extern "C" u64 SwiftLoongArchFaultHelper(u64 address);

inline int CheckABI(BlockFn fn, backend::State* state) {
    // Direct block calls need the fresh-entry preparation normally done by
    // Runtime::Run; otherwise the previous CallHost exit bypasses the body.
    if (state) state->halt_reason = HaltReason::None;
    return SwiftLoongArchCheckABI(fn, state);
}

struct Compiled {
    Config config{TestConfig()};
    la::JitContext context;
    la::JitTranslator translator{context};
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    BlockFn fn{};
    explicit Compiled(ir::Block* block, bool cache_values = true,
                      la::HostFeatures features = la::HostFeatures::Detect(),
                      backend::Module* owner = nullptr,
                      la::ScalarAllocationMode allocation = la::ScalarAllocationMode::GlobalCFG,
                      bool flags_publication = true)
            : context(cache_values, features, {}, allocation) {
        context.SetFlagsPublication(flags_publication);
        if (owner) context.ConfigureModule(owner);
        translator.Translate(block);
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate LoongArch test code");
        context.Flush(*buffer);
        fn = reinterpret_cast<BlockFn>(buffer->exec_data);
    }
};

void ScalarDifferential();
void ScalarBits();
void HelpersABI();
void FlagsDifferential();
void ControlFlow();
void FunctionCFG();
void GuestMemory();
void GuestAtomics();
void RuntimeFaults();
void WideMemory();
void Arm64Guest();
void VectorDifferential();
void FloatHelpers();
void RegisterAllocationPlan();
void ScalarAllocationPlan();
void ScalarAllocationExecution();
void SparseHomesPlan();
void SparseHomesExecution();
void FlagsPlans();
void FlagsPublications();
void RuntimeLifecycle();
void RuntimeSteps();
void SingleInstructionAdmissions();
void SingleInstructionEncodings();
void PrivateSingleInstructionCodes();
void Benchmark();
void CompileContracts();

}  // namespace swift::tests::loongarch64
