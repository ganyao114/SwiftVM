//
// Created by 甘尧 on 2023/9/27.
//

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>
#include "aarch64/macro-assembler-aarch64.h"
#include "base/common_funcs.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/constant.h"
#include "runtime/backend/code_cache.h"
#include "runtime/backend/edge_flags_state.h"
#include "runtime/backend/reg_alloc.h"
#include "runtime/common/hot_coalesce_prof.h"
#include "runtime/common/types.h"
#include "runtime/include/config.h"
#include "runtime/ir/instr.h"
#include "runtime/ir/location.h"

namespace swift::runtime::backend::arm64 {

using namespace vixl::aarch64;

struct NoneReg {};
using CPUReg = boost::variant<NoneReg, Register, VRegister>;

struct DirectLinkFlagsBypass {
    u32 code_offset{UINT32_MAX};
    u32 resume_offset{UINT32_MAX};
    u32 linked_instruction{};
    u32 merge_branch_offset{UINT32_MAX};
    EdgeFlagsState edge_flags{};

    [[nodiscard]] bool Valid() const {
        return code_offset != UINT32_MAX && resume_offset != UINT32_MAX &&
               edge_flags.HasPendingPState();
    }
};

enum class FlagsMergeTrampolineKind : u8 {
    NZCV,
    NZCVToken,
    NZCVMask,
    NZCVMaskToken,
};

enum class CycleReasonTrampolineKind : u8 {
    Basic,
    NZCV,
    NZCVToken,
};

enum class ReturnTrampolineKind : u8 {
    Basic,
    NZCV,
    NZCVToken,
};

// Allocation-relative description retained after emission so disk cache
// can adjust every site without reading a concurrently patched code word.
struct DirectLinkSiteInfo {
    u32 code_offset{};
    u64 guest_target{};
    LinkSiteKind kind{LinkSiteKind::Unconditional};
    DirectLinkFlagsBypass flags_bypass{};
    u32 flags_bypass_instruction{};
};

class JitContext : DeleteCopyAndMove {
public:
    struct FaultRange {
        u32 begin{};
        u32 end{};
    };

    struct StaticForwardResult {
        bool emitted{};
        std::optional<FaultRange> poll_fault{};
    };

    struct IndirectCallForwardResult {
        FaultRange lookup_fault{};
        FaultRange target_fault{};
    };

    explicit JitContext(const std::shared_ptr<Module> &module,
                        RegAlloc& reg_alloc,
                        bool enable_direct_link = true);
    JitContext(const std::shared_ptr<Module>& module,
               RegAlloc& reg_alloc,
               MacroAssembler& assembler,
               bool enable_direct_link = true);

    void AbsorbEmissionState(JitContext& region);

    [[nodiscard]] CPUReg Get(const ir::Value& value);
    [[nodiscard]] bool HasAllocation(const ir::Value& value);
    [[nodiscard]] bool SharesGPR(const ir::Value& left, const ir::Value& right);
    [[nodiscard]] bool SharesFPR(const ir::Value& left, const ir::Value& right);
    [[nodiscard]] bool IsFPRMappedTo(const ir::Value& value, u16 target) {
        return reg_alloc.ValueType(value) == RegAlloc::FPR &&
               reg_alloc.ValueFPR(value).id == target;
    }
    [[nodiscard]] bool IsGPRMappedTo(const ir::Value& value, u16 target) {
        return reg_alloc.ValueType(value) == RegAlloc::GPR &&
               reg_alloc.ValueGPR(value).id == target;
    }
    [[nodiscard]] std::optional<u8> MappedGPRCode(const ir::Value& value) {
        if (reg_alloc.ValueType(value) != RegAlloc::GPR) {
            return std::nullopt;
        }
        return static_cast<u8>(reg_alloc.ValueGPR(value).id);
    }
    [[nodiscard]] Register R(const ir::Value& value, bool auto_cast = false);
    [[nodiscard]] Register RForWrite(const ir::Value& value);
    [[nodiscard]] XRegister X(const ir::Value& value);
    [[nodiscard]] WRegister W(const ir::Value& value);
    [[nodiscard]] VRegister V(const ir::Value& value);

    [[nodiscard]] XRegister GetTmpX();
    [[nodiscard]] Register GetTmpGPR(ir::ValueType type);
    [[nodiscard]] VRegister GetTmpV();
    // TBL with two tables encodes a consecutive register pair. Return false
    // without changing the scratch mask when pressure leaves no such pair.
    [[nodiscard]] bool TryGetConsecutiveTmpV2(VRegister& first, VRegister& second);
    // Registers unavailable for the current IR instruction: RegAlloc's live set
    // here (a conservative superset -- see register_alloc_pass.cpp), the
    // runtime's reserved registers, and every scratch GetTmpX/GetTmpV has
    // already handed out while emitting this instruction.  A register absent
    // from these holds no value the block will read again, which is what lets
    // EmitHostCall save a subset instead of everything.
    [[nodiscard]] const GPRSMask& GetLiveGPRs() const { return cur_dirty_gprs; }
    [[nodiscard]] const FPRSMask& GetLiveFPRs() const { return cur_dirty_fprs; }
    [[nodiscard]] bool ValueLiveAfter(const ir::Value& value) const {
        return cur_inst && reg_alloc.ValueLiveAfter(value, cur_inst->Id());
    }
    // Exclude a register from GetTmpX for the current IR instruction only.
    void ReserveTmpX(const XRegister& reg);
    // Reusable helper scratch for short, non-overlapping backend bookkeeping
    // (principally flag materialization). It is leased once per emission and
    // then reused, matching the old fixed x11 lifetime without globally
    // reserving x11.
    [[nodiscard]] XRegister GetSharedTmpX();

    // TickIR opens the per-opcode VIXL scratch contract. The translator closes
    // it immediately after the opcode emitter returns. Terminals get their own
    // contract because they share the last instruction's live mask.
    void EndInstructionScratch();
    [[nodiscard]] u32 LastInstructionScratchGPR() const {
        return last_instruction_scratch_gpr;
    }
    void BeginTerminalScratch();
    void EndTerminalScratch();
    void ReserveLevel3IndirectTarget(const ir::Value& value);
    // Cold NaN stubs resume an earlier instruction and therefore may not use
    // any implicit GPR scratch at all; their explicit x13 ABI is declared on
    // the originating opcode.
    void BeginColdScratch();
    void EndColdScratch();
    [[nodiscard]] bool ColdScratchActive() const { return auxiliary_scratch; }

    [[nodiscard]] std::optional<FaultRange>
    Forward(ir::Location location,
            Label* backedge_exit = nullptr,
            Label* self_target = nullptr,
            LinkSiteKind direct_link_kind = LinkSiteKind::Unconditional,
            DirectLinkFlagsBypass flags_bypass = {});
    [[nodiscard]] std::optional<FaultRange>
    ForwardLocal(ir::Location location,
                 Label* cycle_exit = nullptr,
                 bool fallthrough = false,
                 Label* local_target = nullptr);
    [[nodiscard]] std::optional<FaultRange>
    ForwardPublishedEntry(ir::Location location, Label* cycle_exit = nullptr);
    [[nodiscard]] bool CanBypassDispatcher(ir::Location location) const;
    [[nodiscard]] bool CanEmitDirectLink(ir::Location location) const;
    [[nodiscard]] bool CanUseRegionTrampoline() const { return direct_link_active; }
    [[nodiscard]] bool RequiresRegionTrampoline() const {
        return !pending_direct_link_sites.empty() || !pending_return_sites.empty() ||
               !flags_merge_sites.empty() || !cycle_reason_sites.empty();
    }
    [[nodiscard]] const std::vector<DirectLinkSiteInfo>& GetDirectLinkSites() const {
        return pending_direct_link_sites;
    }
    void EmitFlagsMergeBranch(FlagsMergeTrampolineKind kind, u8 mask = 0);
    [[nodiscard]] std::optional<FlagsMergeTrampolineKind>
    TakeFlagsMergeBranch(u32 code_offset);
    void EmitCycleReasonBranch();
    void EmitCycleFlagsMergeBranch(bool token);
    void EmitReturnFlagsMergeBranch(bool token);
    // Dispatch to a compile-time-constant guest location, for the
    // "SetLocation(imm) + ReturnToDispatch" shape a direct jmp/call decodes to.
    // Prefer a tracked direct-link site and retain the inline L2 lookup when the
    // region cannot host one. Emits nothing when the target is not linkable;
    // Commits state->current_loc only on a dispatcher fallback.
    [[nodiscard]] StaticForwardResult
    ForwardStatic(ir::Location location,
                  Label* cycle_exit = nullptr,
                  LinkSiteKind direct_link_kind = LinkSiteKind::Unconditional,
                  DirectLinkFlagsBypass flags_bypass = {});
    [[nodiscard]] FaultRange
    ForwardIndirectL1(const Register& location, Label* miss = nullptr);
    [[nodiscard]] FaultRange
    ForwardContinuation(const Register& location, Label* miss, Label* null_target);
    [[nodiscard]] IndirectCallForwardResult ForwardIndirectCall(const Register& location,
                                                                Label* miss,
                                                                bool pending_flags = false);
    void ReturnToDispatcher(const Register& location);
    void ReturnHost();
    [[nodiscard]] bool ContinuationActive() const {
        return cur_function && direct_link_active && FlagsRegsEnabled() &&
               features.indirect_l1 &&
               module->GetModuleConfig().HasOpt(
                       Optimizations::ReturnStackBuffer);
    }

    // --- Return Stack Buffer (RSB) emission --------------------------------
    // Called from the JitTranslator for PushRSB instructions and PopRSBHint
    // terminals when Optimizations::ReturnStackBuffer is enabled.
    //
    // Push: stores a 16-byte frame via pre-decrement of rsb_ptr (x25). The
    //   default format is (guest_return_addr, dispatch_index); lean-shadow
    //   stores dispatch_index twice to avoid materializing the address.
    // Pop: pops a frame, validates the predicted guest key against the retained
    //   return target when available (otherwise state->current_loc), and on a
    //   hit branches through the L2 value.
    //   A miss, empty slot, or underflow uses the normal dispatcher path.
    //
    // The guarded Runtime mapping resets rsb_ptr in the host fault context if
    // either direction leaves the usable stack.
    void EmitRSBPush(u64 guest_return_addr, u32 dispatch_index);
    void EmitRSBPop(std::optional<XRegister> actual_target = std::nullopt);

    // Reserves (GetOrPut) the L2 dispatch-table slot for a guest address and
    // returns its slot index (2*entry+1, pointing at the entry's value word).
    // Used by the translator to build RSB push frames at call sites.
    [[nodiscard]] u32 GetDispatchIndex(u64 guest_addr);

    void Finish();
    [[nodiscard]] u32 CurrentBufferSize();
    [[nodiscard]] u32 HotCheckBytesInRange(u32 begin, u32 end) const;
    [[nodiscard]] ptrdiff_t GetCodeOffset(LocationDescriptor location) const;
    // A linked canonical-state edge can bypass the published flags veneer.
    [[nodiscard]] ptrdiff_t GetDirectLinkCodeOffset(
            LocationDescriptor location) const;
    [[nodiscard]] ptrdiff_t GetPendingFlagsCodeOffset(
            LocationDescriptor location) const;
    [[nodiscard]] EdgeFlagsTargetContract GetPendingFlagsTargetContract(
            LocationDescriptor location) const;
    [[nodiscard]] ptrdiff_t GetCallCodeOffset(LocationDescriptor location) const;
    [[nodiscard]] ptrdiff_t GetCallPendingFlagsCodeOffset(
            LocationDescriptor location) const;
    void RecordDirectLinkEntry(LocationDescriptor location);
    void RecordPendingFlagsEntry(LocationDescriptor location,
                                 EdgeFlagsTargetContract contract);
    void EmitPendingFlagsCallEntry(LocationDescriptor location);
    [[nodiscard]] bool IsUniform(const Register& reg);
    [[nodiscard]] bool IsSpilled(const ir::Value& value) {
        return reg_alloc.ValueType(value) == RegAlloc::MEM;
    }
    [[nodiscard]] bool HasSpillReloadAtDefinition(ir::Inst* inst) const {
        return inst && reg_alloc.SpillReloadAt(ir::Value{inst}, inst->Id());
    }
    [[nodiscard]] bool HasPendingSpillWrites() const {
        return !pending_spill_writes.empty();
    }
    [[nodiscard]] std::optional<u32> PendingScalarSpillValue() const;
    u8* Flush(const CodeBuffer& code_cache);

    [[nodiscard]] MacroAssembler& GetMasm();

    // Address-space config (JIT needs it for the memory_base bias fast-path
    // decision in EmitMemOperand).
    [[nodiscard]] const Config& GetConfig() { return module->GetAddressSpace().GetConfig(); }
    [[nodiscard]] const FeatureSet& GetFeatures() const { return features; }
    [[nodiscard]] u64 GetPairCallTrampoline() const;
    [[nodiscard]] bool IsHostWriteCoalesced(u32 id) const {
        return reg_alloc.IsHostWriteCoalesced(id);
    }
    [[nodiscard]] bool IsHostReadCoalesced(u32 id) const {
        return reg_alloc.IsHostReadCoalesced(id);
    }
    [[nodiscard]] bool IsWidthChainCoalesced(u32 id) const {
        return reg_alloc.IsWidthChainCoalesced(id);
    }
    [[nodiscard]] u32 WidthChainAnchor(u32 id) const {
        return reg_alloc.WidthChainAnchor(id);
    }
    [[nodiscard]] bool IsLow32CopyCoalesced(u32 id) const {
        return reg_alloc.IsLow32CopyCoalesced(id);
    }
    [[nodiscard]] u32 Low32CopySource(u32 id) const {
        return reg_alloc.Low32CopySource(id);
    }
    [[nodiscard]] bool HasWidthComponentOwner(u32 anchor) const {
        return reg_alloc.HasWidthComponentOwner(anchor);
    }
    [[nodiscard]] bool WidthComponentOwnerCommitted(u32 anchor) const {
        return reg_alloc.WidthComponentOwnerCommitted(anchor);
    }
    [[nodiscard]] u16 WidthComponentOwnerTarget(u32 anchor) const {
        return reg_alloc.WidthComponentOwnerTarget(anchor);
    }
    [[nodiscard]] bool WidthComponentOwnerHighZero(u32 anchor) const {
        return reg_alloc.WidthComponentOwnerHighZero(anchor);
    }
    [[nodiscard]] bool IsConstAddressCached(u32 id) const {
        return reg_alloc.IsConstAddressCached(id);
    }
    [[nodiscard]] u32 ConstAddressCacheAnchor(u32 id) const {
        return reg_alloc.ConstAddressCacheAnchor(id);
    }
    [[nodiscard]] bool IsAesChainTied(u32 id) const {
        return reg_alloc.IsAesChainTied(id);
    }
    [[nodiscard]] u16 AesChainTarget(u32 id) const {
        return reg_alloc.AesChainTarget(id);
    }
    [[nodiscard]] bool IsPshufdDirect(u32 id) const {
        return reg_alloc.IsPshufdDirect(id);
    }
    [[nodiscard]] backend::GPRSMask DirtyGPR(u32 id) const {
        return reg_alloc.DirtyGPR(id);
    }
    [[nodiscard]] RAShapeUnitCounters& GetRAShapeCounters() {
        return reg_alloc.RAShape();
    }

    void SetCurrent(ir::Function *function);
    void SetCurrent(ir::Block *block, bool split_backedge_entry = false,
                    bool defer_published_entry = false);
    void BindInternalEntry(LocationDescriptor location);
    [[nodiscard]] vixl::aarch64::Label* GetCountedEntryLabel(
            LocationDescriptor location);
    // Completes a split block entry after the translator has emitted the
    // published-entry branch and bound the self-only body label.
    void BeginBackedgeBody();
    void TickIR(ir::Inst* instr,
                bool forward_spilled_width_input = false,
                bool adopt_pending_spill_write = false,
                std::optional<u32> forward_spilled_memory_input = std::nullopt,
                bool consumer_deferred = false);

    [[nodiscard]] vixl::aarch64::Label *GetLabel(LocationDescriptor loc);
    [[nodiscard]] vixl::aarch64::Label *GetInternalLabel(LocationDescriptor loc);

    [[nodiscard]] bool ExecProfileEnabled() const { return exec_profile_enabled; }
    void RecordExecCounter(u32 state_offset, u32 amount = 1);
    [[nodiscard]] bool ExecutionTraceEnabled() const { return execution_trace_enabled; }
    void RecordExecutionTrace(u64 guest_rip, const XRegister& guest_rsp);
    [[nodiscard]] bool HotCoalesceEnabled() const { return hot_coalesce_enabled; }
    void FinishHotCoalesceBlock();
    void BeginHotNaNGuard(u32 instruction_count);
    void EndHotNaNGuard();
    [[nodiscard]] bool DensityProfileEnabled() const { return density_profile_enabled; }
    [[nodiscard]] u32 DensityNaNBytes() const { return density_nan_bytes; }
    [[nodiscard]] bool FlagsRegsAuditEnabled() const {
        return flags_regs_audit_enabled;
    }
    struct DeferredFlagsRegsAudit {
        u32 slot{};
        HotCoalesceUnitStatic shape{};
    };
    [[nodiscard]] std::optional<DeferredFlagsRegsAudit>
    DeferFlagsRegsAudit();
    void ResumeFlagsRegsAudit(DeferredFlagsRegsAudit audit);
    void FinishDeferredFlagsRegsAudit();
    void RecordFlagsRegsAudit(FlagsRegsAuditMergeCause cause,
                              FlagsRegsAuditEdgeKind edge,
                              FlagsRegsAuditCost cost,
                              u32 amount,
                              bool site = false);
    void CommitFlagsRegsAudit();

private:
    void Initialize(bool enable_direct_link);
    void MaybeDumpHostBytes();
    void FlushLabels(VAddr target);
    void RecordHotCounter(HotCoalesceCounter counter, u32 amount = 1);
    void RecordHotSpillReload();
    void RecordHotSpillWriteback();
    [[nodiscard]] bool EmitDirectLink(ir::Location location,
                                      LinkSiteKind kind,
                                      DirectLinkFlagsBypass flags_bypass = {});

    // --- RegAlloc::MEM (spilled value) support ---------------------------
    // A value the linear scan could not keep in a host register lives in
    // State::spill_area (backend/context.h). Spilled defs compute into a
    // scratch register and are written back at the next instruction or block
    // boundary. A scalar def may stay resident for an adjacent direct consumer
    // when the same scratch is free and has no fixed role there.
    //
    // Platform and capacity constraints:
    //  - On desktop Linux, the first scalar spill reload/write-back may use
    //    x18 when it is free at that instruction. Further scalar reloads and
    //    x18 conflicts use the allocator's verified headroom. Other platforms
    //    forward an allocator-visible scratch only across one proven edge.
    //    SIMD spill scratch still comes from GetTmpV and PANICs loudly if that
    //    contract is ever violated.
    //  - The spill area holds kMaxSpillSlots u64 slots; the allocator
    //    PANICs beyond that rather than overrunning the uniform buffer.
    //  - A few block terminals (Invalid/ReturnToDispatch/ReturnToHost/
    //    PopRSBHint/Switch-fallthrough) Ret directly out of the translator
    //    without touching JitContext, so a pending write-back from the
    //    block's last instruction is skipped there. Harmless in block mode
    //    (spill slots are block-local); only a function-mode spill at the
    //    final instruction into such a terminal would be affected.
    struct PendingSpillWrite;
    [[nodiscard]] Register SpillGPR(const ir::Value& value, bool definition = false);
    [[nodiscard]] VRegister SpillFPR(const ir::Value& value);
    void FlushSpillWrites();
    [[nodiscard]] std::optional<u8> FlushSpillWrites(
            ir::Inst* consumer,
            bool forward_spilled_width_input = false,
            bool adopt_pending_spill_write = false,
            std::optional<u32> forward_spilled_memory_input = std::nullopt,
            bool consumer_deferred = false);
    [[nodiscard]] bool AdoptPendingSpillWrite(
            const PendingSpillWrite& write, ir::Inst* consumer);
    void EmitSpillWriteback(const PendingSpillWrite& write);
    [[nodiscard]] static bool IsFloatValue(const ir::Value& value);

    // Scratch handed to a spill reload rather than to the emitter. Budgeted
    // separately (backend::kSpillReloadHeadroom) because the linear scan pays
    // for it only in units that actually spilled.
    [[nodiscard]] XRegister GetSpillTmpX();
    [[nodiscard]] VRegister GetSpillTmpV();
    [[nodiscard]] backend::ScratchNeed CurrentBudget() const;
    void BeginVixlScratch(bool allow_pool);
    void EndVixlScratch();
    void ExcludeVixlScratch(const XRegister& reg);

    struct PendingSpillWrite {
        u32 value;
        u16 slot;    // spill slot index
        u8 reg;      // scratch register code holding the value
        bool is_fpr;
        bool required_backing{};
    };

    std::shared_ptr<Module> module;
    const FeatureSet features;
    ir::Function *cur_function{};
    ir::Block *cur_block{};
    ir::Inst *cur_inst{};
    RegAlloc& reg_alloc;
    std::unique_ptr<MacroAssembler> owned_masm;
    MacroAssembler& masm;
    LocationDescriptor unit_start{};
    bool unit_start_set{};
    bool host_bytes_dumped{};
    bool ra_shape_submitted{};
    bool exec_profile_enabled{};
    bool execution_trace_enabled{};
    bool hot_coalesce_enabled{};
    bool indirect_l1_prof_enabled{};
    bool flags_regs_audit_enabled{};
    bool hot_static_shape_enabled{};
    bool hot_counter_storage_enabled{};
    bool density_profile_enabled{};
    bool direct_link_active{};
    bool density_nan_open{};
    u32 density_nan_start{};
    u32 density_nan_bytes{};
    u32 exec_access_pad{};
    u32 hot_coalesce_slot{kHotCoalesceInvalidSlot};
    u32 flags_regs_audit_finished_slot{kHotCoalesceInvalidSlot};
    std::vector<u32> hot_coalesce_slots{};
    u32 hot_code_start{};
    u32 hot_nan_start{};
    u32 hot_nan_expected{};
    bool hot_collecting{};
    bool hot_nan_open{};
    HotCoalesceUnitStatic hot_shape{};
    struct HotCodeRange {
        u32 begin{};
        u32 end{};
    };
    std::vector<HotCodeRange> hot_check_ranges{};
    std::vector<HotCodeRange> hot_nan_ranges{};
    std::array<ir::HostGPR, ARM64_MAX_X_REGS> spilled_gprs;
    std::array<ir::HostGPR, ARM64_MAX_X_REGS> spilled_fprs;
    std::map<LocationDescriptor, Label> labels;
    std::map<LocationDescriptor, u32> direct_link_entry_offsets;
    std::map<LocationDescriptor, u32> pending_flags_entry_offsets;
    std::map<LocationDescriptor, EdgeFlagsTargetContract>
            pending_flags_target_contracts;
    std::map<LocationDescriptor, u32> call_entry_offsets;
    std::map<LocationDescriptor, u32> call_pending_flags_entry_offsets;
    std::map<LocationDescriptor, Label> internal_labels;
    // FLAGS_REGS L2 veneer lands here, immediately before the entry counter.
    // Internal taken edges still use internal_labels after the counter, matching
    // FLAGS=0.
    std::map<LocationDescriptor, Label> counted_entry_labels;
    // value id -> scratch reg code for the current instruction's spilled
    // def (repeated def accesses within one instruction must return the
    // same register); cleared at every TickIR.
    std::map<u32, u8> spill_def_scratch;
    // value id -> scratch reg code holding a reloaded *use* of a spilled
    // value, for the current instruction only; also cleared at every TickIR.
    // Memoizing reloads is not just a code-size win: it is what bounds the
    // reload demand of one instruction to the number of distinct values it
    // names, which is the bound backend::kSpillReloadHeadroom encodes.
    std::map<u32, u8> spill_use_scratch;
    // spill_use_scratch entries whose register holds a def value forwarded by
    // FlushSpillWrites: the slot was never written, so a use reached through
    // another emission path must reuse the register without re-loading.
    std::set<u32> forwarded_spill_use_scratch;
    std::vector<PendingSpillWrite> pending_spill_writes;
    std::vector<bool> active_spill_reload_regions;
    // Parallel to active_spill_reload_regions: the region's resident register
    // was filled by a slot reload, so a use reached through a different
    // emission path may reload it again. Def/adopt-established regions carry
    // a register-only value and must not reload.
    std::vector<bool> slot_backed_spill_reload_regions;
    std::vector<DirectLinkSiteInfo> pending_direct_link_sites;
    struct ReturnSiteInfo {
        u32 code_offset{};
        ReturnTrampolineKind kind{};
    };
    std::vector<ReturnSiteInfo> pending_return_sites;
    struct FlagsMergeSiteInfo {
        u32 code_offset{};
        FlagsMergeTrampolineKind kind{};
        u8 mask{};
    };
    std::vector<FlagsMergeSiteInfo> flags_merge_sites;
    struct CycleReasonSiteInfo {
        u32 code_offset{};
        CycleReasonTrampolineKind kind{};
    };
    std::vector<CycleReasonSiteInfo> cycle_reason_sites;

    GPRSMask cur_dirty_gprs{};
    GPRSMask cur_dirty_fprs{};
    // cur_dirty_* as of the last TickIR. The difference in marked bits is how
    // much scratch this instruction has consumed so far, which is what
    // backend::ScratchBudget bounds.
    GPRSMask tick_dirty_gprs{};
    FPRSMask tick_dirty_fprs{};
    u32 spill_tmp_gprs{};
    u32 spill_tmp_fprs{};
    u32 last_instruction_scratch_gpr{};
    int shared_tmp_gpr{-1};
    bool auxiliary_scratch{};
    bool vixl_scratch_contract_active{};
    std::unique_ptr<UseScratchRegisterScope> vixl_scratch_scope{};
};

}  // namespace swift::runtime::backend::arm64
