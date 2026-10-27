#pragma once

#include <memory>
#include <optional>
#include <unordered_map>

#include "base/common_funcs.h"
#include "constraints.h"
#include "runtime/backend/gpr_value_facts.h"
#include "runtime/backend/register_mask.h"
#include "runtime/common/ra_shape_prof.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/args.h"
#include "runtime/ir/host_reg.h"

namespace swift::runtime::ir {
class Block;
class HIRFunction;
}  // namespace swift::runtime::ir

namespace swift::runtime::compile {
class AllocationInputs;

// Shared spill-area ABI, checked against State::spill_area by each backend.
inline constexpr u32 kMaxSpillSlots = 64;

class AllocationWriter;
class AllocationEdit;
class AllocationController;
using backend::FixedGPRValue;
using backend::FPRSMask;
using backend::GPRSMask;
using backend::GPRWidthFacts;

// Owns physical mappings, live masks, reload and preservation locations. It
// contains no target plan, encoder state or lazily populated analysis cache.
// Preparation writers are friends; consumers borrow only a const result.
class AllocationView;

// Physical types are shared by the owning result, its readonly view and the
// preparation writer. There is one definition and one location table.
struct AllocationTypes {
    enum Type : u16 { NONE, GPR, FPR, MEM, REF };

    enum class GPRView : u8 { Value, Low32, WholeValue, Definition };

    struct GPRLocation {
        Type kind{NONE};
        u16 slot{};
        u8 bits{};
        bool pinned{};
        u16 reload_register{UINT16_MAX};
        GPRWidthFacts extension{};
    };

    struct GPRPreservationAction {
        u32 value{};
        u16 source{};
        Type destination_kind{NONE};
        u16 destination{};
        u8 width{};
        u8 bit_offset{};
        u16 scratch{UINT16_MAX};
    };

    struct Map {
        Type type{NONE};
        u16 slot{};
        bool fixed_gpr{};
        bool publication_elided{};
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
};

class Allocation final : public AllocationTypes, DeleteCopyAndMove {
public:
    Allocation(u32 instr_size,
               const GPRSMask& gprs,
               const FPRSMask& fprs,
               const FeatureSet& features);
    ~Allocation() override { ASSERT(!writer_active); }

    [[nodiscard]] AllocationView View() const;

private:
    friend class AllocationView;
    friend class GPRPreservationIndex;
    friend class GPRHomeIndex;
    friend class AllocationWriter;
    friend class AllocationEdit;
    friend class AllocationController;
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
    Vector<GPRPlacement> gpr_placements{};
    std::unordered_map<u64, GPRUse> gpr_use_homes{};
    std::unordered_map<u32, u32> fixed_gpr_publications{};
    u32 gpr_location_epoch{1};
    u32 allocation_attempt{};
    bool gpr_locations_frozen{};
    bool fixed_gpr_constraints_ready{};
    bool writer_active{};
    std::unordered_map<u32, Vector<GPRPreservationAction>> gpr_preservation_actions{};
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
    GPRSMask gprs;
    FPRSMask fprs;
    const FeatureSet features;
    RAShapeUnitCounters ra_shape{};
};

// A borrowed view is one pointer. It owns no cache or preparation state; its
// owner must cover every consumer. Preparing a new result never relocates it.
class AllocationView : public AllocationTypes {
public:
    explicit AllocationView(const Allocation& result) : result(&result) {}
    [[nodiscard]] bool RefersToSameResult(AllocationView other) const {
        return result == other.result;
    }
    [[nodiscard]] const FixedGPRDefinition* FixedGPRDefinitionAt(u32 value_id) const;

    [[nodiscard]] bool HasFixedGPRDefinition(u32 value_id) const;

    [[nodiscard]] bool NeedsFixedGPRPublication(u32 instruction_id) const;

    [[nodiscard]] bool IsFixedGPRDefinitionElided(u32 value_id) const;

    [[nodiscard]] const FixedGPRDefinition* PreservedGPRDefinitionAt(u32 value_id) const;

    [[nodiscard]] const Vector<GPRPreservationAction>& GPRPreservationsBefore(
            u32 instruction) const;

    [[nodiscard]] bool HasGPRPreservationActions() const {
        return !result->gpr_preservation_actions.empty();
    }

    [[nodiscard]] bool FixedGPRHomeIntersects(
            ir::Value value, u16 home, u32 begin, u32 end, u32 ordinary_end) const;

    [[nodiscard]] bool FixedGPRPublicationOwns(u32 publication, u32 value_id) const;

    [[nodiscard]] bool FixedGPRConstraintsReady() const {
        return result->fixed_gpr_constraints_ready;
    }

    [[nodiscard]] GPRLocation GPRLocationAt(ir::Value value,
                                            u32 consumer,
                                            GPRView view = GPRView::Value) const;
    [[nodiscard]] GPRLocation GPRLocationAt(u32 id,
                                            ir::ValueType type,
                                            u32 consumer,
                                            GPRView view = GPRView::Value) const;

    [[nodiscard]] std::optional<u16> GPRValueHome(ir::Value value, u32 consumer) const;
    [[nodiscard]] std::optional<u16> GPRValueHome(u32 id, u32 consumer) const;

    [[nodiscard]] std::optional<u16> GPRLowView(u32 value_id) const;

    [[nodiscard]] bool HasGPRValueHome(u32 value_id) const;

    [[nodiscard]] bool GPRLocationsFrozen() const { return result->gpr_locations_frozen; }

    [[nodiscard]] u32 GPRLocationGeneration() const { return result->gpr_location_epoch; }

    [[nodiscard]] u32 AllocationAttempt() const { return result->allocation_attempt; }

    [[nodiscard]] std::optional<FixedGPRValue> GPRUseHome(u32 value_id,
                                                          u32 consumer,
                                                          bool selected_only = false) const;

    [[nodiscard]] u32 AdditionalGPRResidentUses(u32 value_id) const;

    [[nodiscard]] const GPRSMask& GetGprs() const;

    [[nodiscard]] const FPRSMask& GetFprs() const;

    [[nodiscard]] const FeatureSet& GetFeatures() const { return result->features; }

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

    [[nodiscard]] bool IsAesChainTied(u32 id) const;

    [[nodiscard]] u16 AesChainTarget(u32 id) const;

    [[nodiscard]] bool IsPshufdDirect(u32 id) const {
        return id < result->pshufd_direct.size() && result->pshufd_direct[id];
    }

    [[nodiscard]] bool SharesFPR(const ir::Value& left, const ir::Value& right) const;

    [[nodiscard]] bool IsFixedGPR(u32 id) const;

    [[nodiscard]] u32 LiveEnd(u32 id) const { return result->live_ends[ResolveId(id)]; }

    [[nodiscard]] bool ValueLiveAfter(const ir::Value& value, u32 instruction_id) const;

    ir::HostGPR ValueGPR(const ir::Value& value) const;

    ir::HostGPR ValueGPR(u32 id) const;

    ir::HostFPR ValueFPR(const ir::Value& value) const;

    ir::HostFPR ValueFPR(u32 id) const;

    ir::SpillSlot ValueMem(const ir::Value& value) const;

    ir::SpillSlot ValueMem(u32 id) const;

    [[nodiscard]] u32 AllocationId(const ir::Value& value) const;

    [[nodiscard]] const SpillReload* SpillReloadAt(u32 value_id, u32 instruction_id) const;

    [[nodiscard]] const SpillReload* SpillReloadAt(const ir::Value& value,
                                                   u32 instruction_id) const;

    [[nodiscard]] bool HasSpillReload(u32 value_id, u32 instruction_id) const;

    Type ValueType(const ir::Value& value) const;

    Type ValueType(u32 value_id) const;

    [[nodiscard]] GPRSMask DirtyGPR(u32 id) const { return result->alloc_result[id].dirty_gprs; }

    [[nodiscard]] FPRSMask DirtyFPR(u32 id) const { return result->alloc_result[id].dirty_fprs; }

    [[nodiscard]] u32 MapCount() const { return static_cast<u32>(result->alloc_result.size()); }

    [[nodiscard]] const Map& Mapping(u32 id) const { return result->alloc_result[id]; }

    [[nodiscard]] u32 SpillCount() const;

    [[nodiscard]] const RAShapeUnitCounters& RAShape() const { return result->ra_shape; }

private:
    friend class AllocationWriter;
    friend class AllocationEdit;
    friend class AllocationController;
    using GPRPlacement = Allocation::GPRPlacement;
    [[nodiscard]] u32 ResolveId(u32 id) const;
    [[nodiscard]] const Vector<SpillReload>* SpillReloadsAt(u32 instruction_id) const;
    [[nodiscard]] const GPRPlacement* CurrentGPRPlacement(u32 value_id) const;
    const Allocation* result;
};

inline AllocationView Allocation::View() const { return AllocationView{*this}; }
static_assert(sizeof(AllocationView) == sizeof(const Allocation*));
struct AllocationStatistics {
    u32 semantic_use_blocks{};
    u32 spill_use_builds{};
    u32 eviction_restarts{};
    u32 final_gpr_reserve{};
    u32 final_fpr_reserve{};
    bool fell_back_to_ladder{};
};

struct FunctionAllocationOptions {
    bool scalar_insert{};
    // No override follows the resolved FeatureSet. A local interval request
    // still has to pass entry, definition-order and cross-block qualification.
    std::optional<bool> block_intervals;
};

void AllocateBlock(const ir::Block* block,
                   AllocationWriter* allocation,
                   AllocationInputs inputs,
                   bool scalar_insert = false);
void AllocateFunction(const ir::HIRFunction* function,
                      AllocationWriter* allocation,
                      AllocationInputs inputs,
                      FunctionAllocationOptions options = {});

}  // namespace swift::runtime::compile
