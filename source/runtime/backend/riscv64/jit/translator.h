#pragma once

#include <map>
#include <deque>
#include "runtime/backend/riscv64/jit/jit_context.h"
#include "runtime/include/sruntime.h"
#include "runtime/ir/block.h"

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
    void SetFunctionExits(std::unordered_map<ir::Inst*, ir::Terminal> exits,
                          std::unordered_set<ir::Inst*> halt_checks) {
        function_exits = std::move(exits); function_halt_checks = std::move(halt_checks);
    }
    ir::Block* EmittedIR() const { return block; }
    [[nodiscard]] u32 RecoveryOffset() const { return recovery_offset; }
    [[nodiscard]] const EmissionStats& Stats() const { return stats; }

private:
    struct PhiMove { ir::Inst* destination; ir::DataClass source; };
    void ConfigurePhis();
    bool EmitPhi(ir::Inst* inst);
    void EmitPhiMoves(const std::vector<PhiMove>& moves);
    bool EmitVectorHostRegisters(ir::Inst* inst);
    bool EmitVectorCrypto(ir::Inst* inst);
    bool EmitMemoryCopy(ir::Inst* inst);
    bool EmitVectorShuffle(ir::Inst* inst);
    bool EmitVectorLocal(ir::Inst* inst);
    bool EmitVectorFloat(ir::Inst* inst);
    bool EmitNativeCall(ir::Inst* inst);
    bool EmitVectorInteger(ir::Inst* inst);
    bool EmitVector(ir::Inst* inst);
    bool EmitAtomic(ir::Inst* inst);
    bool EmitScalar(ir::Inst* inst);
    bool EmitScalarBits(ir::Inst* inst);
    bool EmitFlags(ir::Inst* inst);
    void EmitHelper(ir::Inst* inst);
    void EmitPrologue(u32 entry_start);
    void EmitAddress(u64 size);
    void EmitMemory(ir::Inst* inst, bool store, bool ordered, biscuit::GPR result);
    void EmitTerminal(const ir::Terminal& terminal);
    void Poll();
    void Return(HaltReason reason);
    bool EmitControl(ir::Inst* inst);
    void EmitTailCode(u32 saved_mask);
    void Link(u64 guest);
    bool PopRSB();

    JitContext& context;
    ir::Block* block{};
    IntrusivePtr<ir::Block> wide_ir;
    ir::Inst* current_inst{};
    std::unordered_map<ir::Inst*, u32> memory_extents;
    std::unordered_set<ir::Inst*> memory_continuations;
    u32 slot_count{};
    u32 recovery_offset{};
    EmissionStats stats{};
    biscuit::Label epilogue, fault_recovery;
    biscuit::Label tail_epilogue, link_cold;
    struct TailSite { u64 guest; biscuit::Label label; };
    std::deque<TailSite> tail_sites;
    bool tail_used{};
    biscuit::Label abi_recovery, abi_save;
    std::map<ir::Inst*, biscuit::Label> labels;
    std::vector<PhiMove> entry_phis;
    std::unordered_map<ir::Inst*, std::vector<PhiMove>> taken_phis, fallthrough_phis;
    std::unordered_map<ir::Inst*, ir::Terminal> function_exits;
    std::unordered_set<ir::Inst*> function_halt_checks;
};

}  // namespace swift::runtime::backend::riscv64
