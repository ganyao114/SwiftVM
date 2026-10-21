//
// Created by 甘尧 on 2023/10/13.
//

#pragma once

#include <bit>
#include <optional>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include "runtime/backend/gpr_value_facts.h"
#include "base/common_funcs.h"
#include "runtime/common/types.h"
#include "runtime/common/ra_shape_prof.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/block.h"
#include "runtime/ir/host_reg.h"

namespace swift::runtime::ir { class HIRFunction; }

namespace swift::runtime::backend {
namespace arm64 { struct GPRBlockPlan; class UnitFacts; }
class Module;

template<typename T = u32>
class RegisterMask {
public:

    explicit RegisterMask() : mask() {}

    explicit RegisterMask(T mask) : mask(mask) {}

    auto GetFirstMarked() {
        return std::countr_zero(mask);
    }

    auto GetFirstClear() {
        constexpr auto bit_count = static_cast<int>(sizeof(T) * 8);
        auto idx = std::countr_one(mask);
        // All bits marked → no clear register; countr_one returns the
        // bit-width, which is not a valid register index.
        return idx < bit_count ? idx : -1;
    }

    auto GetMarkedCount() {
        return std::popcount(mask);
    }

    auto GetClearCount() {
        return GetAllCount() - std::popcount(mask);
    }

    auto GetAllCount() {
        return sizeof(T) * 8;
    }

    [[nodiscard]] bool Null() const {
        return mask == 0;
    }

    [[nodiscard]] bool Get(u32 bit) const {
        return mask & (T(1) << bit);
    }

    bool operator==(const RegisterMask&) const = default;

    void Mark(u32 bit) {
        mask |= (T(1) << bit);
    }

    void Clear(u32 bit) {
        mask &= ~(T(1) << bit);
    }

    void Reset(T value) {
        mask = value;
    }

private:
    T mask;
};

using GPRSMask = RegisterMask<u32>;
using FPRSMask = RegisterMask<u32>;

// Raw XPOOL request and effective state. Pin level 3 forces the effective state
// on because pinning x6-x9 would otherwise leave only x14/x15 as value GPRs.
[[nodiscard]] bool ScratchXPoolRequested(const FeatureSet& features);
[[nodiscard]] bool ScratchXPoolAutoEnabled(const FeatureSet& features);
[[nodiscard]] bool X86PinExtLevel3Requested();

// True only for an actual level-2/3 x86 pool: the environment level alone is
// insufficient because swift_test and non-x86 frontends can construct an
// AddressSpace without the expected static descriptors while inheriting vars.
[[nodiscard]] bool X86PinExtLevel2Enabled(const GPRSMask& pool);
[[nodiscard]] bool X86PinExtLevel3Enabled(const GPRSMask& pool);
[[nodiscard]] bool X86PinExtScratchOnlyEnabled(const GPRSMask& pool,
                                               const FeatureSet& features);
// Level 3 can lease the bias-mode x10 reservation to pure high-pressure ALU
// lowering.
// It never enters the value pool and memory opcodes never see it as scratch.
[[nodiscard]] bool X86PinExtLevel3AluScratchEnabled(const GPRSMask& pool,
                                                    ir::OpCode op);
[[nodiscard]] bool X86PinExtLevel3AluScratchEnabled(bool available, ir::OpCode op);

// W-gamma fixed-class gate.  The class is meaningful only with the complete
// sixteen-register level-3 architectural map; a sparse map must keep the
// established allocator unchanged instead of silently creating a hybrid ABI.
inline constexpr u32 kX86FixedGPRHomes =
        (0x3ffu << 0) | (0x1fu << 19) | (1u << 29);
[[nodiscard]] bool FixedGPRClassEnabled(const GPRSMask& pool,
                                        const FeatureSet& features);
[[nodiscard]] bool IsFixedGPRHome(u32 code);

// Number of u64 spill slots reserved in State::spill_area (context.h).
// The linear-scan pass panics instead of handing out a slot beyond this:
// an out-of-range slot would silently overwrite the uniform buffer that
// follows the spill area. Kept in sync with State::spill_area by a
// static_assert in arm64/jit/jit_context.cpp.
//
// This is a budget for spills LIVE AT ONCE, not for spills performed. It used
// to be the latter -- the allocator never returned a slot -- which made a unit
// that spilled repeatedly abort even though it needed a handful of slots at any
// instant. With recycling (register_alloc_pass.cpp, ExpireOldIntervals) the
// measured high-water marks are: avx_real_x86_64 under SVM_AVX=1 fell from 50
// slots to 3 (57 spills either way), swift_test's saturation blocks from 55 to
// 28, and the block-mode x87 guests are unchanged at 8 and 21 -- those really
// do hold that many spilled values simultaneously.
//
// What can still reach 64: the ARM64 pools are 20 allocatable GPRs and 28 FPRs,
// less the scratch reserve (3/3 by default), so a compilation unit needs more
// than ~81 scalar values -- or ~57 V128 values, which take two slots each --
// live at one instruction. No guest in the corpus comes close; the closest is
// 21. Raising the ceiling is cheap if that ever changes: spill_area sits
// immediately before State's flexible uniform buffer, every uniform offset is
// derived from offsetof(State, uniform_buffer_begin) rather than hardcoded, and
// ThreadContext64 -- whose byte layout the FXSAVE path and contract C2 depend
// on -- lives inside that buffer and does not move. The cost is 8 bytes of
// per-thread State per slot; the JIT's scaled-immediate addressing has room to
// spare (32760 bytes for the u64 form, 65520 for the 16-byte one). It is left
// at 64 because the assert now fires only on a shape nothing produces, and it
// names the unit when it does.
static constexpr u32 kMaxSpillSlots = 64;

// --- Scratch-register budget -------------------------------------------
// While the JIT emits ONE IR instruction it may take host registers that hold
// no IR value (JitContext::GetTmpX / GetTmpV / ReserveTmpX). Those come from
// whatever the register allocator left unassigned at that instruction, and
// they are only recycled at the next instruction boundary (TickIR) -- so the
// number that matters is how many an emitter holds at once, not how many it
// uses in sequence.
//
// This table is the contract between the two halves:
//   * the linear scan (register_alloc_pass.cpp) keeps at least this many
//     registers unassigned at every instruction of a compilation unit, so the
//     emitter is guaranteed to find them;
//   * JitContext asserts that no emitter exceeds its declared budget, so an
//     emitter that grows past it fails loudly and by name instead of silently
//     stealing a live value (or panicking only under high pressure, which is
//     what used to happen).
//
// Values are the measured peaks over the whole test corpus (every guest under
// every gating configuration plus swift_test); see the report accompanying
// this change. Adding an opcode needs no table edit -- the default covers the
// ordinary emitter shapes -- and if a new emitter needs more, the assert in
// JitContext::GetTmpX names it on the first emission.
struct ScratchNeed {
    u8 gpr;
    u8 fpr;
};

// Flags demand of one value-producing instruction, summarised from
// its SaveFlags/BranchOnlyFlags pseudo uses. Each phase computes it
// from its own use index (allocator demand table, planner flags
// plan, or a block scan) and passes it to the precise scratch
// budgets, replacing pseudo-chain walks (un-chaining step 3.4).
struct FlagsDemand {
    ir::Flags requested{};
    u8 pseudo_count{};
    bool branch_only{};
    bool has_save{};
};

// Exact demand for `owner` by scanning one block. For call sites
// without a use index (standalone preparation, tests).
[[nodiscard]] FlagsDemand ScanFlagsDemand(const ir::Block& block, const ir::Inst& owner);

// Four independent GPR budgets.  `value_pool` is a count; the remaining
// fields are contracts for one emission site and never contribute registers
// to that pool.  Keeping fixed clobbers as a mask prevents the x12/x13 CAS and
// cold-continuation ABIs from being mistaken for ordinary scratch capacity.
struct GPRClassContract {
    u8 value_pool{};
    u8 hot_instruction_scratch{};
    u8 cold_edge_scratch{};
    u32 call_clobber_mask{};
    u32 fixed_clobber_mask{};
};

[[nodiscard]] GPRClassContract ClassifyGPRContract(
        const GPRSMask& pool,
        const ir::Inst& inst,
        const FeatureSet& features,
        FlagsDemand demand);

// W52 opt-in scratch-pool contract. OFF preserves the historical global
// reservation byte-for-byte. ON lets the allocator use x11-x17 and protects
// only the registers an opcode explicitly clobbers.
[[nodiscard]] bool ScratchXPoolEnabled(const FeatureSet& features);

// Bit mask of architecturally fixed GPR clobbers for one opcode. These are
// direct SwiftVM uses (atomic status/value registers, host-call/terminal
// register, cold-path return register), not VIXL's dynamically leased scratch
// registers.
[[nodiscard]] u32 FixedGPRClobbers(ir::OpCode op, const FeatureSet& features,
                                   bool scratch_only = false);
[[nodiscard]] u32 FixedGPRClobbers(const ir::Inst& inst,
                                   const FeatureSet& features,
                                   bool scratch_only = false);
inline constexpr u32 kTerminalFixedGPRClobbers = 1u << 11;

// Ordinary emitters: the widest generic shape is a 3-GPR ALU sequence
// (Add/Sub/And/Or/Xor/Mul with a folded operand) and a 3-FPR vector sequence.
inline constexpr u8 kDefaultScratchGPR = 3;
inline constexpr u8 kDefaultScratchFPR = 3;

// Every *use* of a spilled value reloads it into a scratch register
// (JitContext::SpillGPR/SpillFPR) on top of the emitting opcode's own budget,
// and a spilled destination needs one too. Reloads are memoized per
// (instruction, value), so an instruction's extra demand is exactly the number
// of distinct spilled values it names -- which the allocator counts directly
// rather than guessing at.

// Conservative budget by opcode, plus the precise instruction-level contract
// used whenever the caller has the full IR instruction. Total functions;
// never fail.
[[nodiscard]] ScratchNeed ScratchBudget(ir::OpCode op, const FeatureSet& features);
[[nodiscard]] ScratchNeed ScratchBudget(const ir::Inst& inst,
                                        const FeatureSet& features,
                                        FlagsDemand demand);
[[nodiscard]] ScratchNeed PreciseAddSubScratchBudget(const ir::Inst& inst, FlagsDemand demand);
[[nodiscard]] bool ScratchPreciseRequested(const FeatureSet& features);

class RegAlloc : DeleteCopyAndMove {
public:

    explicit RegAlloc(u32 instr_size, const GPRSMask& gprs, const FPRSMask& fprs,
                      const FeatureSet& features, bool afp_nan = false);
    ~RegAlloc();
    // Shared per-attempt analysis facts (middle-pipeline redesign
    // step 1). Lazily created; valid for this allocation attempt only.
    [[nodiscard]] arm64::UnitFacts& Facts();
    void SetAllocationUnit(ir::Block* block, ir::HIRFunction* function, std::function<void()> replay);
    void CompleteGPRAllocation(const std::shared_ptr<Module>& module);
    void BeginTargetPlanning();
    void EndTargetPlanning();
    void AbortTargetPlanning() noexcept;
    [[nodiscard]] bool TargetPlanning() const { return target_planning; }
    [[nodiscard]] bool GPRAllocationComplete() const { return target_complete; }
    [[nodiscard]] u32 TargetReplayCount() const { return target_replays; }
    [[nodiscard]] bool FPRCoalescingDisabled() const { return fpr_coalescing_disabled; }
    void DisableFPRCoalescing() { ASSERT(target_planning && !gpr_locations_frozen); fpr_coalescing_disabled = true; }
    void CertifyWidthElision(u32 id, bool low32) {
        ASSERT(target_planning && !gpr_locations_frozen && id < alloc_result.size());
        if (verified_width_elisions.empty()) verified_width_elisions.resize(alloc_result.size());
        verified_width_elisions[id] |= low32 ? 2 : 1;
    }
    [[nodiscard]] bool WidthElisionVerified(u32 id) const {
        return id < verified_width_elisions.size() && (verified_width_elisions[id] & 1);
    }
    void CertifyHostRead(u32 id) {
        ASSERT(target_planning && !gpr_locations_frozen && id < alloc_result.size());
        if (verified_width_elisions.empty()) verified_width_elisions.resize(alloc_result.size());
        verified_width_elisions[id] |= 4;
    }
    [[nodiscard]] bool HostReadVerified(u32 id) const {
        return id < verified_width_elisions.size() && (verified_width_elisions[id] & 4);
    }
    void CertifyZeroStore(u32 id) {
        ASSERT(target_planning && !gpr_locations_frozen && id < alloc_result.size());
        if (verified_width_elisions.empty()) verified_width_elisions.resize(alloc_result.size());
        verified_width_elisions[id] |= 8;
    }
    [[nodiscard]] bool ZeroStoreVerified(u32 id) const {
        return id < verified_width_elisions.size() && (verified_width_elisions[id] & 8);
    }
    [[nodiscard]] bool Low32ElisionVerified(u32 id) const {
        return id < verified_width_elisions.size() && (verified_width_elisions[id] & 2);
    }
    void StoreGPRPlan(const ir::Block* block, std::shared_ptr<const arm64::GPRBlockPlan> plan) { target_plans[block] = std::move(plan); }
    [[nodiscard]] std::shared_ptr<const arm64::GPRBlockPlan> GPRPlanFor(const ir::Block* block) const;
    // Synchronous readonly view; do not retain it across plan replacement,
    // replay/reset, or this allocator's lifetime. Facts belong to this attempt.
    // To own the GPRBlockPlan beyond this read, use GPRPlanFor instead.
    [[nodiscard]] const arm64::GPRBlockPlan* GPRPlanViewFor(const ir::Block* block) const;
    struct TargetHome {
        u32 value{};
        u16 result{UINT16_MAX}, low{UINT16_MAX}, whole{UINT16_MAX};
        u32 begin{}, end{UINT32_MAX}, additional_uses{};
        bool has_use_homes{};
    };
    struct TargetUse { u32 value{}, consumer{}; FixedGPRValue home{}; bool selected{}; };
    struct TargetDeclarations { std::vector<TargetHome> homes; std::vector<TargetUse> uses; };
    [[nodiscard]] TargetDeclarations SaveTargetDeclarations() const;
    void RestoreTargetDeclarations(const TargetDeclarations& declarations);
    bool BindTargetDefinition(u32 value, u16 home, u8 width, bool elided, u32 last_use, u32 publication = UINT32_MAX);
    void ReplayAllocation();
    void DeclareElidedPublication(u32 id) { ASSERT(target_planning); target_elided_publications.insert(id); }

    enum Type : u16 {
        NONE,
        GPR,
        FPR,
        MEM,
        REF
    };

    enum class GPRView : u8 { Value, Low32, WholeValue, Definition };
    struct GPRLocation {
        Type kind{NONE};
        u16 slot{};
        u8 bits{};
        bool pinned{};
        u16 reload_register{UINT16_MAX};
        GPRWidthFacts extension{};
    };
    struct FixedGPRDefinition {
        u16 home{UINT16_MAX};
        u16 transferred_home{UINT16_MAX};
        u32 end{};
        u32 transfer{UINT32_MAX};
        u32 last_use{};
        u32 publication{UINT32_MAX};
        u32 owner{};
        u8 width{};
        u8 bit_offset{};
        bool elided{};
        // The guest home owns the prefix; the ordinary mapping owns the
        // suffix. transfer is a preservation point, before that IR executes.
        bool preserved{};
        bool version{};
    };
    // A constraint is proven before ordinary live intervals are assigned. It
    // remains intact across scratch-reserve retries and is verified against
    // the completed allocation before the emitter can consume it.
    void PlanFixedGPRDefinition(u32 value_id, FixedGPRDefinition definition);
    [[nodiscard]] const FixedGPRDefinition* FixedGPRDefinitionAt(u32 value_id) const;
    [[nodiscard]] bool HasFixedGPRDefinition(u32 value_id) const;
    [[nodiscard]] bool NeedsFixedGPRPublication(u32 instruction_id) const;
    [[nodiscard]] bool IsFixedGPRDefinitionElided(u32 value_id) const;
    [[nodiscard]] const FixedGPRDefinition* PreservedGPRDefinitionAt(u32 value_id) const;
    [[nodiscard]] bool GPRPreservationConflict(u16 home, u32 begin, u32 end) const;
    [[nodiscard]] bool GPRTargetWriteConflict(u16 home, u32 begin, u32 end,
                                               u32 ignored = UINT32_MAX, u32 ignored_second = UINT32_MAX) const;
    struct GPRPreservationAction {
        u32 value{};
        u16 source{};
        Type destination_kind{NONE};
        u16 destination{};
        u8 width{};
        u8 bit_offset{};
        u16 scratch{UINT16_MAX};
    };
    void BuildGPRPreservationActions();
    [[nodiscard]] const Vector<GPRPreservationAction>& GPRPreservationsBefore(u32 instruction) const;
    [[nodiscard]] bool HasGPRPreservationActions() const { return !gpr_preservation_actions.empty(); }
    [[nodiscard]] bool FixedGPRHomeIntersects(ir::Value value, u16 home,
                                            u32 begin, u32 end, u32 ordinary_end) const;
    [[nodiscard]] bool FixedGPRPublicationOwns(u32 publication, u32 value_id) const;
    [[nodiscard]] bool FixedGPRConstraintsReady() const { return fixed_gpr_constraints_ready; }
    void FinishFixedGPRConstraints() {
        ASSERT(!gpr_locations_frozen);
        fixed_gpr_constraints_ready = true;
    }

    // This is the single location table for preallocated homes, target views,
    // inferred copies and ordinary allocations/reload recipes. Querying it
    // never emits a load or reserves scratch.
    [[nodiscard]] GPRLocation GPRLocationAt(ir::Value value, u32 consumer,
                                           GPRView view = GPRView::Value) const;
    [[nodiscard]] std::optional<u16> GPRValueHome(ir::Value value, u32 consumer) const;
    [[nodiscard]] std::optional<u16> GPRLowView(u32 value_id) const;
    [[nodiscard]] bool HasGPRValueHome(u32 value_id) const;
    void BeginGPRLocationAnalysis();
    void FreezeGPRLocations();
    [[nodiscard]] bool GPRLocationsFrozen() const { return gpr_locations_frozen; }
    [[nodiscard]] u32 GPRLocationGeneration() const { return gpr_location_epoch; }
    [[nodiscard]] u32 AllocationAttempt() const { return allocation_attempt; }
    void AssignGPRValueHome(u32 value_id, u16 home);
    void AssignGPRTransferredHome(u32 value_id, u16 home, u32 begin, u32 end);
    void AssignGPRLowView(u32 value_id, u16 home);
    void RegisterGPRUseHome(u32 value_id, u32 consumer, FixedGPRValue value,
                            bool selected);
    [[nodiscard]] std::optional<FixedGPRValue> GPRUseHome(u32 value_id, u32 consumer,
                                                       bool selected_only = false) const;
    [[nodiscard]] u32 AdditionalGPRResidentUses(u32 value_id) const;

    struct Map {
        Type type{NONE};
        u16 slot{};
        bool fixed_gpr{};
        GPRSMask dirty_gprs{0};
        FPRSMask dirty_fprs{0};

        bool operator==(const Map&) const = default;
    };

    struct WidthComponentOwner {
        u16 target{UINT16_MAX};
        bool high_zero{};
        bool committed{};

        [[nodiscard]] bool Valid() const { return target != UINT16_MAX; }
        bool operator==(const WidthComponentOwner&) const = default;
    };

    struct SpillReload {
        u32 region{};
        u32 value{};
        u16 reg{};
        bool owns_all_uses{};
        bool fault_backed{};
    };

    // The GPR coalescer stages one whole block and publishes it only after the
    // write/read/width proofs agree.  This capture is deliberately limited to
    // state that family can mutate; pools, spills and scratch contracts remain
    // allocator construction state and cannot be touched by the transaction.
    struct GPRCoalesceState {
        Vector<Map> mappings;
        Vector<bool> host_writes;
        Vector<bool> host_reads;
        Vector<u32> width_anchors;
        Vector<WidthComponentOwner> width_owners;
    };

    [[nodiscard]] const GPRSMask& GetGprs() const;
    [[nodiscard]] const FPRSMask& GetFprs() const;
    [[nodiscard]] const FeatureSet& GetFeatures() const { return features; }
    void DeclareGuestGPR(u16 home) {
        ASSERT(!gpr_locations_frozen && home < 31);
        gprs.Mark(home);
    }

    void MapRegister(u32 id, ir::HostGPR gpr);
    void MapFixedRegister(u32 id, ir::HostGPR gpr);
    void MapRegister(u32 id, ir::HostFPR fpr);
    void MapMemSpill(u32 id, ir::SpillSlot slot);
    void MapSpillReload(u32 value_id, u32 first_use, u32 last_use,
                        ir::HostGPR reg, bool owns_all_uses = false,
                        bool fault_backed = false);
    void MapReference(u32 from, u32 to);
    void MarkHostWriteCoalesced(u32 id);
    void MarkHostReadCoalesced(u32 id);
    void MarkWidthChainCoalesced(u32 id, u32 anchor_id);
    void MarkLow32CopyCoalesced(u32 id, u32 source_id);
    bool FreezeWidthComponentOwner(u32 anchor_id, u16 target, bool high_zero);
    bool CommitWidthComponentOwner(u32 anchor_id, u16 target);
    void MarkAesChainTied(u32 id, u16 target);
    void MarkPshufdDirect(u32 id);
    [[nodiscard]] bool IsHostWriteCoalesced(u32 id) const;
    [[nodiscard]] bool IsHostReadCoalesced(u32 id) const;
    [[nodiscard]] bool IsWidthChainCoalesced(u32 id) const;
    [[nodiscard]] u32 WidthChainAnchor(u32 id) const;
    [[nodiscard]] bool IsLow32CopyCoalesced(u32 id) const;
    [[nodiscard]] u32 Low32CopySource(u32 id) const;
    [[nodiscard]] bool HasWidthComponentOwner(u32 anchor_id) const;
    [[nodiscard]] bool WidthComponentOwnerCommitted(u32 anchor_id) const;
    [[nodiscard]] u16 WidthComponentOwnerTarget(u32 anchor_id) const;
    [[nodiscard]] bool WidthComponentOwnerHighZero(u32 anchor_id) const;
    [[nodiscard]] GPRCoalesceState SaveGPRCoalesceState() const;
    void RestoreGPRCoalesceState(GPRCoalesceState state);
    [[nodiscard]] bool IsAesChainTied(u32 id) const;
    [[nodiscard]] u16 AesChainTarget(u32 id) const;
    [[nodiscard]] bool IsPshufdDirect(u32 id) const {
        return id < pshufd_direct.size() && pshufd_direct[id];
    }
    [[nodiscard]] bool SharesFPR(const ir::Value& left, const ir::Value& right) const;
    [[nodiscard]] bool IsFixedGPR(u32 id) const;
    void SetLiveEnd(u32 id, u32 end);
    [[nodiscard]] bool ValueLiveAfter(const ir::Value& value,
                                      u32 instruction_id) const;
    void SetActiveRegs(u32 id, GPRSMask &gprs, FPRSMask &fprs);
    // Experimental placement check: rename only the symmetric dynamic homes
    // after allocation and verification, so allocation/coalescing decisions
    // and therefore instruction count remain unchanged.
    void PermutePlacementCheckGPRHomes();

    ir::HostGPR ValueGPR(const ir::Value &value) const;
    ir::HostFPR ValueFPR(const ir::Value &value) const;
    ir::SpillSlot ValueMem(const ir::Value &value) const;
    ir::HostGPR ValueGPR(u32 id) const;
    ir::HostFPR ValueFPR(u32 id) const;
    ir::SpillSlot ValueMem(u32 id) const;
    [[nodiscard]] u32 AllocationId(const ir::Value& value) const;
    [[nodiscard]] const SpillReload* SpillReloadAt(
            u32 value_id, u32 instruction_id) const;
    [[nodiscard]] const SpillReload* SpillReloadAt(
            const ir::Value& value, u32 instruction_id) const;
    [[nodiscard]] bool HasSpillReload(u32 value_id, u32 instruction_id) const;
    // Resolves REF (bitcast alias) entries, so the result is the underlying
    // GPR/FPR/MEM allocation rather than the alias itself.
    Type ValueType(const ir::Value &value) const;

    [[nodiscard]] GPRSMask GetDirtyGPR() const;
    [[nodiscard]] FPRSMask GetDirtyFPR() const;
    // Same, for an arbitrary instruction id: the allocation pass verifies its
    // own output against every instruction, not just the current one.
    [[nodiscard]] GPRSMask DirtyGPR(u32 id) const { return alloc_result[id].dirty_gprs; }
    [[nodiscard]] FPRSMask DirtyFPR(u32 id) const { return alloc_result[id].dirty_fprs; }
    [[nodiscard]] u32 MapCount() const { return static_cast<u32>(alloc_result.size()); }
    [[nodiscard]] const Map& Mapping(u32 id) const { return alloc_result[id]; }
    [[nodiscard]] u32 SpillCount() const;
    [[nodiscard]] RAShapeUnitCounters& RAShape() { return ra_shape; }
    [[nodiscard]] const RAShapeUnitCounters& RAShape() const { return ra_shape; }

    void SetCurrent(ir::Inst *inst);

    // Drops every mapping so the linear scan can be re-run over the same unit
    // with a larger scratch reserve (see RegisterAllocPass::Run). Only the
    // allocation results are cleared; the pool masks are construction state.
    void ResetAllocations();

private:
    // Follows REF chains (MapReference) to the id holding the real
    // GPR/FPR/MEM allocation.
    [[nodiscard]] u32 ResolveId(u32 id) const;
    // Point every REF entry straight at its final allocation so
    // ResolveId (called by every allocation query) is O(1). Safe
    // once an allocation attempt is complete; results of ResolveId
    // are unchanged.
    void FlattenRefChains();
    [[nodiscard]] const Vector<SpillReload>* SpillReloadsAt(u32 instruction_id) const;

    struct GPRPlacement {
        u16 result{UINT16_MAX};
        u16 low_view{UINT16_MAX};
        u16 whole_value{UINT16_MAX};
        u32 begin{};
        u32 end{UINT32_MAX};
        u32 epoch{};
        u32 additional_uses{};
        bool has_use_homes{};
        FixedGPRDefinition fixed{};
    };
    struct GPRUse {
        FixedGPRValue value{};
        u32 epoch{};
        bool selected{};
    };
    GPRPlacement& GPRPlacementFor(u32 value_id);
    [[nodiscard]] const GPRPlacement* CurrentGPRPlacement(u32 value_id) const;
    Vector<GPRPlacement> gpr_placements{};
    std::unordered_map<u64, GPRUse> gpr_use_homes{};
    std::unordered_map<u32, u32> fixed_gpr_publications{};
    std::optional<std::unordered_map<u32, u32>> target_publications_undo;
    u32 gpr_location_epoch{1};
    u32 allocation_attempt{};
    bool gpr_locations_frozen{};
    bool fixed_gpr_constraints_ready{};
    ir::Block* allocation_block{};
    ir::HIRFunction* allocation_function{};
    std::function<void()> allocation_replay{};
    std::unique_ptr<arm64::UnitFacts> unit_facts{};
    bool target_planning{}, target_complete{};
    std::unordered_map<const ir::Block*, std::shared_ptr<const arm64::GPRBlockPlan>> target_plans{};
    std::unordered_map<u32, FixedGPRDefinition> target_fixed_undo;
    u32 target_replays{};
    bool fpr_coalescing_disabled{}, target_fpr_disabled_undo{};
    std::vector<u8> verified_width_elisions;
    std::unordered_set<u32> target_elided_publications{};
    std::unordered_map<u32, Vector<GPRPreservationAction>> gpr_preservation_actions{};
    mutable std::array<Vector<std::pair<u32, u32>>, 32> gpr_preservation_ranges{};
    mutable bool gpr_preservation_ranges_dirty{true};
    struct HomeInterval { u32 owner{}, begin{}, end{}; };
    mutable std::array<std::vector<HomeInterval>, 32> gpr_home_intervals{};
    mutable std::array<std::vector<u32>, 32> gpr_home_prefix_ends{};
    mutable bool gpr_home_intervals_dirty{true};

    Vector<Map> alloc_result;
    Vector<u32> live_ends;
    Vector<Vector<SpillReload>> spill_reloads{};
    u32 spill_reload_region_count{};
    Vector<bool> coalesced_host_writes{};
    Vector<bool> coalesced_host_reads{};
    Vector<u32> width_chain_anchors{};
    Vector<WidthComponentOwner> width_component_owners{};
    Vector<u32> low32_copy_sources{};
    Vector<u16> aes_chain_targets{};
    Vector<bool> pshufd_direct{};
    u32 stack_size{};
    ir::Inst *current_ir{};
    GPRSMask gprs;
    FPRSMask fprs;
    const FeatureSet features;
    RAShapeUnitCounters ra_shape{};
};

}
