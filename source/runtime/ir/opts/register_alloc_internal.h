#pragma once

#include "register_alloc_pass.h"

namespace swift::runtime::ir {

struct LiveInterval {
    Inst* inst{};
    u32 start{};
    u32 end{};

    bool operator<(const LiveInterval& other) const {
        if (start == other.start) {
            return end < other.end;
        } else {
            return start < other.start;
        }
    }
};

Value ResolveBitCastSource(Value value);
bool IsPinnedCoalesceTarget(u32 reg);
bool IsPinnedCoalesceProducer(OpCode op);
bool IsWidthChainRootProducer(const Inst* producer);
bool IsPinnedCoalesceObserver(OpCode op);
bool HasKnownWWrite(Value value);
void PlanWidthComponentOwners(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        const Vector<u32>& use_end);
bool ValidateWidthComponentTransaction(
        Block* lir_block,
        backend::RegAlloc* reg_alloc);
Vector<u32> CollectWidthChainUseEnds(Block* lir_block, u32 instr_count);
Vector<u8> CollectLongWidthChainBridges(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        u32 instr_count,
        const Vector<u32>& use_end);

struct RegisterAllocFamilyCallbacks {
    void* context{};
    bool (*check_instr)(void*, Inst*, u32, u32){};
    bool (*directly_feeds_memory)(void*, Inst*){};
};

void CoalesceWidthChainBridges(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        const Vector<u32>& use_end,
        const Vector<u8>& long_bridge,
        const Vector<u32>& fixed_gpr_clobbers,
        const RegisterAllocFamilyCallbacks& callbacks);
Vector<u32> CollectGuestGPRUseEnds(Block* lir_block, u32 instr_count);
void CoalesceGuestGPRReads(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const Vector<u32>& use_end);
bool GuestGPRMappedTo(Value value, u32 target, backend::RegAlloc* reg_alloc);
bool HasGuestGPRTargetConflict(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const Vector<u32>& use_end,
        Inst* producer,
        Inst* wrapper,
        Inst* store,
        u32 target);
void CoalesceGuestGPRWrites(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        const Vector<u32>& use_end,
        const Vector<u32>& fixed_gpr_clobbers,
        const RegisterAllocFamilyCallbacks& callbacks);
bool IsResidentFPRTarget(u32 reg);
bool IsScalarFPRBinaryProducer(OpCode op);
bool IsResidentFPRProducer(OpCode op, bool scalar_tie);
bool IsAesEncChainProducer(OpCode op);
Vector<u32> CollectGuestFPRUseEnds(Block* lir_block, u32 instr_count);
bool GuestFPRMappedTo(Value value, u32 target, backend::RegAlloc* reg_alloc);
bool TryCoalesceAesChain(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        const Vector<u32>& use_end,
        Inst& store,
        Value produced,
        u32 target);
void CoalesceGuestFPRWrites(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        const FeatureSet& features,
        u32 instr_count,
        bool scalar_insert);

struct ConstAddressCandidate {
    Inst* inst{};
    u32 use_id{};
};

std::optional<u64> RawConstAddressValue(Inst* inst);
std::optional<u64> ConstAddressValue(Inst* inst);
bool IsConstAddressBarrier(OpCode op);
void CacheConstantAddressesForBlock(
        Block* lir_block,
        backend::RegAlloc* reg_alloc,
        u64 unit_pc,
        bool audit,
        const RegisterAllocFamilyCallbacks& callbacks);

class VRegisterAllocator {
public:
    explicit VRegisterAllocator(Block* block);

    void AllocateRegisters();
    void ExpireOldIntervals(LiveInterval& current);
    bool IsFloatValue(Inst* inst);
    void GrowVRegs(u32 new_item_size);
    void AllocVReg(LiveInterval& interval);

private:
    void CollectLiveIntervals();

    Block* block;
    Vector<LiveInterval> live_interval;
    List<LiveInterval> active_lives;
    Vector<bool> active_v_regs{};
    u16 active_v_regs_cursor{0};
};

}  // namespace swift::runtime::ir
