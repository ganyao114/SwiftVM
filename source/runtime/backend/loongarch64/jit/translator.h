#pragma once

#include <map>
#include <vector>
#include "runtime/backend/loongarch64/guest_access.h"
#include "runtime/backend/loongarch64/jit/jit_context.h"
#include "runtime/backend/loongarch64/jit/vector_context.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/block.h"

namespace swift::runtime::backend::loongarch64 {

enum class ExecutionMode : u8 { Ordinary, SingleInstruction };

struct EmissionStats {
    u32 direct{}, helpers{}, guarded_native{}, frame_size{}, saved_gprs{}, saved_fprs{};
    // Static flag bit computations, not dynamic execution counts.
    u32 flag_bits_requested{}, flag_bits_computed{}, flag_writes_elided{};
    u32 checkpoints{}, cold_uniform_stores{}, pc_stores{}, hot_pc_stores{};
    std::array<u32, static_cast<size_t>(ir::OpCode::BASE_COUNT)> bytes{};
};

class JitTranslator {
public:
    explicit JitTranslator(JitContext& context, ExecutionMode mode = ExecutionMode::Ordinary)
            : context(context), execution_mode(mode) {}
    void Translate(ir::Block* block);
    void SetFunctionExits(std::unordered_map<ir::Inst*, ir::Terminal> exits,
                          std::unordered_set<ir::Inst*> halt_checks) {
        function_exits = std::move(exits); function_halt_checks = std::move(halt_checks);
    }
    ir::Block* EmittedIR() const { return block; }
    u32 RecoveryOffset() const { return recovery_offset; }
    u32 HomeBytes() const { return context.SlotCount() * kValueStride; }
    std::span<const GuestAccessRange> GuestAccesses() const { return guest_accesses; }
    GuestAccessCoverage GuestFaultCoverage() const { return guest_access_coverage; }
    bool AuditedTypedGuestAccesses() const { return typed_guest_accesses; }
    const EmissionStats& Stats() const { return stats; }
    const VectorValueStats& VectorStats() const { return vectors.Stats(); }

private:
    void Emit(ir::Inst* inst);
#define INST(name, ...) void Emit##name(ir::Inst* inst);
#include "runtime/ir/ir.inc"
#undef INST
    void EmitHelper(ir::Inst* inst);
    void EmitCondition(ir::Cond cond, GPR result);
    ir::Flags RequestedFlags(ir::Inst* inst, ir::Flags requested);
    void EmitTerminal(const ir::Terminal& terminal);
    void EmitPrologue();
    void EmitEpilogue();
    void EmitRestoreFrame();
    void EmitSingleRetirement();
    void RecordGuestAccess(u32 begin, u32 size, GuestAccessKind kind, GPR address, s32 displacement = 0);
    void Poll();
    void CheckHalt();
    void PublishPC(bool commit = true);
    void Return(HaltReason reason);
    void EmitMemory(ir::Inst* inst, bool store, bool ordered);
    void EmitAtomic(ir::Inst* inst);
    void EmitFloat(ir::Inst* inst, bool scalar, u32 bits);
    void ConfigurePhis();
    struct PhiMove { ir::Inst* destination; ir::DataClass source; };
    void EmitPhiMoves(const std::vector<PhiMove>& moves);
    void Call(GPR target);

    JitContext& context;
    VectorContext vectors{context};
    std::vector<WideVectorPair> wide_pairs;
    ir::Block* block{};
    IntrusivePtr<ir::Block> wide_ir;
    ir::Inst* current_inst{};
    EmissionStats stats{};
    u32 recovery_offset{};
    bool guest_instructions{};
    u64 pc_delta{}, previous_pc_delta{};
    bool pc_pending{};
    ExecutionMode execution_mode{ExecutionMode::Ordinary};
    GuestAccessCoverage guest_access_coverage{GuestAccessCoverage::Precise};
    std::vector<GuestAccessRange> guest_accesses;
    bool typed_guest_accesses{};
    Label epilogue, fault_recovery, single_retirement;
    std::map<ir::Inst*, Label> labels;
    std::vector<PhiMove> entry_phis;
    std::unordered_map<ir::Inst*, std::vector<PhiMove>> taken_phis, fallthrough_phis;
    std::unordered_map<ir::Inst*, ir::Terminal> function_exits;
    std::unordered_set<ir::Inst*> function_halt_checks;
    std::unordered_map<ir::Inst*, u32> memory_extents;
    std::unordered_set<ir::Inst*> memory_continuations;
};
}  // namespace swift::runtime::backend::loongarch64
