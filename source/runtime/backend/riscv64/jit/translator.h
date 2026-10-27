#pragma once

#include <map>
#include <deque>
#include "runtime/backend/riscv64/jit/jit_context.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/block.h"
#include "runtime/backend/restricted_instruction_resume.h"

namespace swift::runtime::backend::riscv64 {

struct EmissionStats {
    u32 direct{};
    u32 helpers{};
    u32 frame_size{}, saved_gprs{}, saved_fprs{}, lazy_saved_gprs{}, lazy_saved_fprs{};
    std::array<u32, static_cast<size_t>(ir::OpCode::BASE_COUNT)> bytes{};
};

class JitTranslator {
public:
    explicit JitTranslator(JitContext& context) : context(context) {}
    void Translate(ir::Block* block);
    void SetFunctionExits(std::unordered_map<const ir::Inst*, ir::Terminal> exits,
                          std::unordered_set<const ir::Inst*> halt_checks) {
        function_exits = std::move(exits);
        function_halt_checks = std::move(halt_checks);
    }
    ir::Block* EmittedIR() const { return block; }
    [[nodiscard]] u32 RecoveryOffset() const { return recovery_offset; }
    [[nodiscard]] const EmissionStats& Stats() const { return stats; }
    [[nodiscard]] std::shared_ptr<const InstructionResumeDescriptor> InstructionResume() const { return instruction_resume; }

private:
    struct PhiMove {
        const ir::Inst* destination;
        ir::DataClass source;
    };
    void ConfigurePhis();
    bool EmitPhi(const ir::Inst* inst);
    void EmitPhiMoves(const std::vector<PhiMove>& moves);
    bool EmitVectorHostRegisters(const ir::Inst* inst);
    bool EmitVectorCrypto(const ir::Inst* inst);
    bool EmitMemoryCopy(const ir::Inst* inst);
    bool EmitVectorShuffle(const ir::Inst* inst);
    bool EmitVectorLocal(const ir::Inst* inst);
    bool EmitVectorFloat(const ir::Inst* inst);
    bool EmitNativeCall(const ir::Inst* inst);
    bool EmitVectorInteger(const ir::Inst* inst);
    bool EmitVector(const ir::Inst* inst);
    bool EmitAtomic(const ir::Inst* inst);
    bool EmitScalar(const ir::Inst* inst);
    void EmitIntegerShift(const ir::Inst* inst, ir::IntegerShiftKind kind);
    void EmitIntegerPredicate(const ir::Inst* inst, ir::IntegerPredicateKind kind);
    bool EmitScalarBits(const ir::Inst* inst);
    bool EmitFlags(const ir::Inst* inst);
    void EmitHelper(const ir::Inst* inst);
    void EmitPrologue(u32 entry_start, bool prepend = true);
    void EmitAddress(u64 size);
    void EmitMemory(const ir::Inst* inst, bool store, bool ordered, biscuit::GPR result);
    void EmitTerminal(const ir::Terminal& terminal);
    void Poll();
    void Return(HaltReason reason);
    bool EmitControl(const ir::Inst* inst);
    void EmitTailCode(u32 saved_mask);
    void Link(u64 guest);
    bool PopRSB();

    JitContext& context;
    ir::Block* block{};
    IntrusivePtr<ir::Block> wide_ir;
    const ir::Inst* current_inst{};
    std::unordered_map<const ir::Inst*, u32> memory_extents;
    std::unordered_set<const ir::Inst*> memory_continuations;
    u32 slot_count{};
    u32 recovery_offset{};
    EmissionStats stats{};
    biscuit::Label epilogue, fault_recovery;
    biscuit::Label tail_epilogue, link_cold;
    struct TailSite { u64 guest; biscuit::Label label; };
    std::deque<TailSite> tail_sites;
    bool tail_used{};
    biscuit::Label abi_recovery, abi_save;
    std::map<const ir::Inst*, biscuit::Label> labels;
    std::vector<PhiMove> entry_phis;
    std::unordered_map<const ir::Inst*, std::vector<PhiMove>> taken_phis, fallthrough_phis;
    std::unordered_map<const ir::Inst*, ir::Terminal> function_exits;
    std::unordered_set<const ir::Inst*> function_halt_checks;
    std::shared_ptr<InstructionResumeDescriptor> instruction_resume;
};

}  // namespace swift::runtime::backend::riscv64
