#pragma once

#include <span>
#include <unordered_map>

#include "runtime/compile/allocate/writer.h"

namespace swift::runtime::compile {

struct GPRRecolorSite {
    u32 instruction{};
    u32 fixed_clobbers{};
};

enum class RecolorFailure : u8 {
    None,
    InvalidWindow,
    HomeNotReserved,
    UnsupportedLocation,
    CapturedValue,
    FixedResidence,
    OutsideWindow,
    WidthOwner,
    PartialAliasGroup,
    ConsumerHome
};

// A local coalescing edit. Entries are captured on their first write, so an
// untouched value needs no storage and a block never copies the unit's maps.
// Pools, spill slots, numbering and target actions are outside this edit.
class AllocationEdit final : DeleteCopyAndMove {
public:
    explicit AllocationEdit(AllocationWriter& allocation);
    AllocationEdit(AllocationWriter& allocation, u32 local_begin, u32 local_limit);
    ~AllocationEdit();

    void Commit();
    void Rollback();
    // The caller proves scalar types, guest-state windows and target-specific
    // clobbers. This edits a complete physical component and its local masks;
    // scratch and required elision checks must pass before Commit.
    [[nodiscard]] RecolorFailure RecolorGPR(std::span<const u32> values,
                                            u16 home,
                                            bool fixed_class,
                                            std::span<const GPRRecolorSite> sites);
    [[nodiscard]] bool OriginalHostWrite(u32 id) const;
    [[nodiscard]] u32 OriginalWidthAnchor(u32 id) const;
    [[nodiscard]] size_t TouchedValueCount() const { return values.size(); }
    [[nodiscard]] size_t LocalIndexEntryCount() const { return local_slots.size(); }
    [[nodiscard]] size_t ExternalIndexEntryCount() const { return external_slots.size(); }

private:
    friend class AllocationWriter;
    struct ValueState {
        u32 id;
        AllocationWriter::Map mapping;
        AllocationWriter::WidthComponentOwner owner;
        u32 width_anchor;
        bool host_write;
        bool host_read;
    };

    void RecordValue(u32 id);
    void RecordPlacement(u32 id);
    void RecordPublication(u32 instruction);
    void RecordUse(u64 key);
    void RecordReloads(u32 instruction);
    [[nodiscard]] const ValueState* OriginalValue(u32 id) const;

    AllocationWriter& allocation;
    // Membership is allocated only on the first real change. Cross-window
    // owners stay sparse; the local table never spans the whole function.
    u32 local_begin{}, local_limit{};
    Vector<u32> local_slots;
    std::unordered_map<u32, u32> external_slots;
    Vector<ValueState> values;
    std::unordered_map<u32, AllocationWriter::GPRPlacement> placements;
    std::unordered_map<u32, std::optional<u32>> publications;
    std::unordered_map<u64, std::optional<AllocationWriter::GPRUse>> uses;
    std::unordered_map<u32, Vector<AllocationWriter::SpillReload>> reloads;
    bool active{true};
};

}  // namespace swift::runtime::compile
