#pragma once

#include <biscuit/assembler.hpp>
#include <unordered_map>
#include <vector>
#include <span>
#include "runtime/backend/code_cache.h"
#include "runtime/backend/riscv64/defines.h"
#include "runtime/backend/riscv64/host_features.h"
#include "runtime/ir/args.h"

namespace swift::runtime::ir { class Block; }

namespace swift::runtime::backend::riscv64 {

struct ScalarValueStats {
    u32 loads{};
    u32 stores{};
    u32 cache_hits{};
    u32 spills{};
    u32 dead_values{};
    u32 initializations{};
};

// Each SSA value has a canonical 16-byte home. Direct scalar lowering defers
// writes while values reside in callee-saved GPRs or RVV registers. Integer
// vectors use GPR pairs without V. Helpers and joins require canonical homes;
// callbacks preserve live caller-saved vectors. No ARM64 register rewrite runs.
class JitContext {
public:
    explicit JitContext(bool cache_scalars = true, HostFeatures features = HostFeatures::Detect())
            : cache_scalars(cache_scalars), features(features) {}
    biscuit::Assembler& GetMasm() { return masm; }
    const HostFeatures& Features() const { return features; }
    void EnsureSpace();
    void Address(biscuit::GPR result, biscuit::GPR base, s64 offset);
    void Load(biscuit::GPR result, biscuit::GPR base, s64 offset, u32 size = 8);
    void Store(biscuit::GPR value, biscuit::GPR base, s64 offset, u32 size = 8);
    void Read(biscuit::GPR result, ir::Value value);
    void ReadPart(biscuit::GPR result, ir::Value value, u32 part);
    biscuit::GPR SourcePart(ir::Value value, u32 part, biscuit::GPR scratch);
    static bool ZeroHigh(ir::Inst* inst);
    std::array<biscuit::GPR, 2> ResultPair(ir::Inst* inst);
    void WritePair(ir::Inst* inst, const std::array<biscuit::GPR, 2>& registers);
    void SetVectorType(u32 bits, u32 lanes);
    void ResetVectorType() { vector_bits = vector_lanes = 0; vector_round_mode = ~u32{0}; }
    void SetVectorRounding(u32 mode);
    biscuit::Vec ResultVector(ir::Inst* inst);
    biscuit::Vec SourceVector(ir::Value value, biscuit::Vec scratch);
    void WriteVector(ir::Inst* inst, biscuit::Vec value);
    void SaveVectorsForCall(bool abi_call = true);
    void RestoreVectorsAfterCall();
    void EnableVectorFloat(bool enabled) { vector_float = enabled; }
    bool VectorFloatEnabled() const { return vector_float; }
    void EnterFloatMode();
    void LeaveFloatMode();
    void BeginBlock() { abi_calls = false; used_gprs = 0; float_mode_offset = 200; }
    void MarkABICall() { abi_calls = true; }
    bool CallsABI() const { return abi_calls; }
    u32 UsedGPRs() const { return used_gprs | (flags_enabled ? 1u << 8 : 0); }
    void FinalizeFrame() { float_mode_offset = abi_calls ? 200 : 104; }
    u32 FloatModeOffset() const { return float_mode_offset; }
    void Prepend(u32 start, std::span<const u8> code);
    biscuit::GPR SourceRegister(ir::Value value, biscuit::GPR scratch);
    void Data(biscuit::GPR result, const ir::DataClass& data);
    void Operand(biscuit::GPR result, const ir::Operand& operand);
    void Mask(biscuit::GPR value, u32 bits);
    void SignExtend(biscuit::GPR value, u32 bits);
    // Reserve/evict before any operands are read; Write publishes the result.
    biscuit::GPR ResultRegister(ir::Inst* inst);
    void Write(ir::Inst* inst, biscuit::GPR value, bool normalized = false);
    // Blocks that consume/publish flags reserve s11; other blocks retain all
    // nine SSA registers. ABI calls publish/reload the architectural word.
    void EnableFlagsCache(bool enabled) { flags_enabled = enabled; flags_dirty = false; }
    bool FlagsEnabled() const { return flags_enabled; }
    void MarkFlagsDirty() { ASSERT(flags_enabled); flags_dirty = true; }
    void PublishFlags();
    void ReloadFlags();
    void ConfigureLiveness(ir::Block* block);
    void ConfigureInitialization(ir::Block* block);
    void InitializeValues();
    void ReleaseDeadValues(u32 position);
    void FlushValues();
    void DiscardValues();
    [[nodiscard]] const ScalarValueStats& ValueStats() const { return value_stats; }
    void Jump(biscuit::Label& label);
    void BranchZero(biscuit::GPR value, biscuit::Label& label, bool zero = true);
    void Condition(biscuit::GPR result, ir::Cond condition);
    [[nodiscard]] u32 CurrentBufferSize();
    void Flush(const CodeBuffer& buffer);

private:
    biscuit::GPR ReserveRegister(biscuit::GPR excluded = biscuit::x0);
    void SpillVector(size_t index);
    biscuit::Assembler masm{};
    const bool cache_scalars;
    const HostFeatures features;
    std::array<ir::Inst*, scalar_registers.size()> cached_values{};
    std::array<u32, scalar_registers.size()> cached_parts{};
    std::array<ir::Inst*, 16> cached_vectors{};
    size_t next_vector{};
    u32 vector_bits{}, vector_lanes{};
    u32 vector_round_mode{~u32{0}};
    size_t next_register{};
    bool flags_enabled{};
    bool vector_float{};
    bool abi_calls{};
    u32 used_gprs{}, float_mode_offset{200};
    bool flags_dirty{};
    std::unordered_map<ir::Inst*, u32> last_uses;
    std::vector<ir::Inst*> initial_values;
    ScalarValueStats value_stats{};
};

}  // namespace swift::runtime::backend::riscv64
