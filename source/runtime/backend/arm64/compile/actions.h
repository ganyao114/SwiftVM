#pragma once

#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "base/common_funcs.h"
#include "runtime/backend/arm64/compile/admission.h"
#include "runtime/backend/arm64/compile/edges.h"
#include "runtime/backend/arm64/compile/scratch.h"
#include "runtime/common/types.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/analysis/pseudo_flags.h"

namespace swift::runtime::compile {
class Allocation;
}
namespace swift::runtime::ir {
class Block;
class Inst;
class Value;
class RegisterAllocTestSupport;
enum class Cond : u8;
enum class Flags : u64;
}  // namespace swift::runtime::ir

namespace swift::runtime::backend::arm64 {

// Stable registration identity and compact block offsets. All final ownership
// resides in UnitActions; block registration carries no shared plan owner.
struct BlockActions final {
public:
    [[nodiscard]] bool PushesReturnAddress() const { return pushes_return_address; }

private:
    friend class UnitActions;
    friend class AllocationPreparation;
    u32 flags_offset{UINT32_MAX};
    u16 instruction_first{UINT16_MAX}, instruction_last{};
    u16 backedge{UINT16_MAX};
    bool has_scratch{};
    bool pushes_return_address{};
};

struct GPRResidenceAction final {
    enum Flag : u8 { ZeroExtend = 1, SignExtend = 2, Resident = 4 };
    u16 instruction;
    u8 bits{};
    u8 result_home{UINT8_MAX};
    [[nodiscard]] bool Has(Flag flag) const { return bits & flag; }
    [[nodiscard]] std::optional<u16> ResultHome() const {
        return result_home == UINT8_MAX ? std::nullopt : std::optional<u16>{result_home};
    }
};

struct FlagsOnlySpillAction final {
    u16 consumer, producer;
    [[nodiscard]] u32 Key() const { return (u32{consumer} << 16) | producer; }
    bool operator==(const FlagsOnlySpillAction&) const = default;
};

struct PublicationAction final {
    enum Kind : u8 { Skip, Copy, Transfer, BiasedUpdate, Value };
    enum Property : u8 { ClearHigh = 1, AdjustResident = 2, AdoptsSpill = 4, FusedSource = 8 };
    u16 instruction;
    // Width/bit offset for a value or copy; decrement for a biased update.
    u16 parameter{};
    u8 target{};
    u8 source{UINT8_MAX};
    u8 properties{};
    Kind kind{Skip};
    [[nodiscard]] bool Has(Property bit) const { return properties & bit; }
    [[nodiscard]] u8 Width() const { return parameter & 0xff; }
    [[nodiscard]] u8 BitOffset() const { return parameter >> 8; }
    void SetView(u8 width, u8 offset = 0) { parameter = width | (u16{offset} << 8); }
    [[nodiscard]] std::optional<u16> SourceHome() const {
        return source == UINT8_MAX ? std::nullopt : std::optional<u16>{source};
    }
    [[nodiscard]] ir::Value Source(const ir::Inst& publication) const;
    bool operator==(const PublicationAction&) const = default;
};

struct MemoryTransferAction final {
    enum Kind : u8 { Load, LoadUpdate, PreIndex, Biased, Value };
    u16 instruction;
    // Load->update, update->publication/access, or extract->store, by kind.
    u16 related{UINT16_MAX};
    s16 offset{};
    u8 home{};
    Kind kind;
    [[nodiscard]] bool MatchesMemory(const ir::Inst& access) const;
    [[nodiscard]] bool MatchesValue(const ir::Inst& store, ir::Value) const;
    bool operator==(const MemoryTransferAction&) const = default;
};

struct AddressAction final {
    s64 offset{};
    u16 instruction;
    u16 memory;
    u8 home;
    bool operator==(const AddressAction&) const = default;
};

// One row per selected extract/add/sub. The u32 payload is an immediate for
// compares, or the exact receiving instruction ID for an input projection.
struct IntegerFlagsAction final {
    enum Kind : u8 { Input, InputLow, CarryLeft, CarryRight, CompareImmediate, CompareValue };
    u32 argument{};
    u16 instruction;
    Kind kind;
    // Compare width in the upper bits; bit zero selects the sealed extract edge.
    u8 options{};
    [[nodiscard]] bool IsInput() const { return kind == Input || kind == InputLow; }
    [[nodiscard]] bool IsCarry() const { return kind == CarryLeft || kind == CarryRight; }
    [[nodiscard]] bool IsCompare() const {
        return kind == CompareImmediate || kind == CompareValue;
    }
    [[nodiscard]] u8 Width() const { return options >> 1; }
    [[nodiscard]] bool MatchesInput(const ir::Inst& consumer, ir::Value) const;
    [[nodiscard]] ir::Value Source(const ir::Inst& consumer) const;
    [[nodiscard]] std::optional<ir::Value> Right(const ir::Inst& consumer) const;
    bool operator==(const IntegerFlagsAction&) const = default;
};

// At most one selected terminal branch per block. Coverage is shared with the
// other actions; no condition/producer pointers or discarded sets survive.
struct IntegerBranchAction final {
    u32 immediate{};
    u16 producer;
    u16 condition;
    ir::Cond raw_condition;
    u8 required_mask{};
    u8 zero_target{UINT8_MAX};
    u8 width{};
    [[nodiscard]] ir::Flags Required() const { return static_cast<ir::Flags>(required_mask); }
    [[nodiscard]] bool IsNarrow() const { return width == 1 || width == 2; }
    [[nodiscard]] std::optional<u16> ZeroTarget() const {
        return zero_target == UINT8_MAX ? std::nullopt : std::optional<u16>{zero_target};
    }
    bool operator==(const IntegerBranchAction&) const = default;
};

enum class ConsumerFlag : u16 {
    Boolean = 1,
    LaterControl = 2,
    Last = 4,
    LocalCondition = 8,
    LaterPstate = 16,
    Pair = 32,
    FCmp = 64,
    CompactCarrier = 128,
    PackedRelation = 256,
    RawSingle = 512,
    RawReader = 1024,
    OddCarry = 2048,
    IncomingPacked = 4096,
    CompoundClear = 8192,
    String = 16384,
    IncomingZero = 32768
};
struct ConsumerAction final {
    u16 instruction;
    // Opcode-disjoint links: Boolean user, pair secondary, compound producer,
    // or the beginning of a compact comparison's inversion window.
    u16 related{UINT16_MAX};
    u16 properties{};
    u8 pstate_mask{};
    // SSE string demand or the one certified incoming-zero operand slot.
    u8 auxiliary{};
    [[nodiscard]] bool Has(ConsumerFlag bit) const { return properties & static_cast<u16>(bit); }
    void Mark(ConsumerFlag bit) { properties |= static_cast<u16>(bit); }
    [[nodiscard]] ir::Flags Pstate() const { return static_cast<ir::Flags>(pstate_mask); }
    [[nodiscard]] ir::Flags StringDemand() const { return static_cast<ir::Flags>(auxiliary); }
    [[nodiscard]] bool MatchesZeroInput(const ir::Inst&, ir::Value) const;
    // The pending inversion is current-block emission state. Preparation
    // counted every InvertCarry between this producer and reader, so its
    // membership needs only these frozen bounds, even for an even count.
    [[nodiscard]] bool CarryInverted(u32 pending = UINT32_MAX) const {
        const bool skipped = related != UINT16_MAX && related < pending && pending < instruction;
        return !Has(ConsumerFlag::OddCarry) != skipped;
    }
    bool operator==(const ConsumerAction&) const = default;
};
struct AluSplitFacts final {
    u32 value_uses{}, external_users{};
    u8 family_count{};
    bool family_capped{}, saw_address{}, saw_value{};
    bool operator==(const AluSplitFacts&) const = default;
};
struct AluSplitAction final {
    u16 instruction;
    AluSplitFacts facts;
    bool operator==(const AluSplitAction&) const = default;
};
enum class FPRAction : u8 {
    Read = 1,
    Write = 2,
    ScalarTie = 4,
    AesTie = 8,
    ShuffleTie = 16,
    Pshufd = 32
};

struct ScalarFPRAction final {
    enum Kind : u8 { Load64, Load32, Value, Conversion, ConversionExport, Store };
    u16 instruction;
    u8 target;
    Kind kind;
    [[nodiscard]] bool IsLoad() const { return kind == Load64 || kind == Load32; }
    [[nodiscard]] bool IsConversion() const {
        return kind == Conversion || kind == ConversionExport;
    }
    [[nodiscard]] bool ExportsResult() const { return kind == ConversionExport; }
    bool operator==(const ScalarFPRAction&) const = default;
};

struct CarryAction final {
    u16 instruction;
    u16 combine;
    u16 invert{UINT16_MAX};
    ir::Cond packed_condition;
    ir::Cond raw_condition{static_cast<ir::Cond>(UINT8_MAX)};
    [[nodiscard]] std::optional<ir::Cond> RawCondition() const {
        return invert == UINT16_MAX ? std::nullopt : std::optional{raw_condition};
    }
    bool operator==(const CarryAction&) const = default;
};

struct MemoryAction final {
    enum Flag : u8 { Narrow = 1, Direct = 2, Sign = 4, Wide = 8, PostIndex = 16, Offset = 32 };
    u16 instruction;
    s16 offset{};
    u8 base{UINT8_MAX};
    u8 target{UINT8_MAX};
    u8 access_bytes{};
    u8 bits{};
    [[nodiscard]] bool Has(Flag flag) const { return bits & flag; }
    [[nodiscard]] u8 ResultBytes() const { return Has(Wide) ? 8 : 4; }
    // Read the exact EA operand edge certified during lowering. No matching or
    // use-graph traversal occurs during emission; its type view is preserved.
    [[nodiscard]] ir::Value OffsetBase(const ir::Inst& access) const;
    bool operator==(const MemoryAction&) const = default;
};

struct SelectAction final {
    u16 instruction;
    bool boolean;
    ir::Cond condition{static_cast<ir::Cond>(UINT8_MAX)};
    [[nodiscard]] std::optional<ir::Cond> DirectCondition() const {
        return static_cast<u8>(condition) == UINT8_MAX ? std::nullopt : std::optional{condition};
    }
    bool operator==(const SelectAction&) const = default;
};

struct FlagsTokenAction final {
    u32 homes;
    u16 producer;
    // The invalid instruction ID denotes a block-wide retention certificate.
    u16 point{UINT16_MAX};
    [[nodiscard]] static bool Retains(std::span<const FlagsTokenAction>,
                                      u32 producer,
                                      u32 home,
                                      u32 point);
    bool operator==(const FlagsTokenAction&) const = default;
};

enum class ScalarFlag : u8 {
    SelfXor = 1,
    CopyInputDiscarded = 2,
    BitInputCovered = 4,
    TiedZeroExtend = 8
};
struct NarrowAction final {
    enum Kind : u8 { Extend, DeferredExtend, ExtractShift, Mask, ShiftMask };
    u16 instruction;
    Kind kind;
    u8 parameter;
    [[nodiscard]] u8 Width() const { return parameter & 0x7f; }
    [[nodiscard]] bool SourceHighZero() const { return parameter & 0x80; }
    // Read only the two/three sealed operand edges proven during lowering.
    // The original use-edge type view survives; no graph or rematching occurs.
    [[nodiscard]] ir::Value Source(const ir::Inst& consumer) const;
    bool operator==(const NarrowAction&) const = default;
};

// Target decisions and their proofs belong to the prepared result, separately
// from physical locations. Only the preparation builder can change them.
class UnitActions final {
public:
    explicit UnitActions(compile::AllocationView allocation) : allocation(allocation) {}
    UnitActions(const UnitActions&) = delete;
    UnitActions& operator=(const UnitActions&) = delete;
    UnitActions(UnitActions&&) = delete;
    UnitActions& operator=(UnitActions&&) = delete;
    [[nodiscard]] bool Ready() const { return MatchesAllocation(allocation); }
    [[nodiscard]] bool MatchesAllocation(compile::AllocationView allocation) const;
    [[nodiscard]] bool MatchesPreparation(compile::AllocationView allocation) const;
    [[nodiscard]] const BlockActions* ForBlock(const ir::Block* block) const;
    [[nodiscard]] const UnitCertificate* Certificate() const {
        return building || Ready() ? certificate.get() : nullptr;
    }
    [[nodiscard]] const EdgeActions& Edges() const { return edges; }
    [[nodiscard]] const AtomicActions& Atomics() const { return atomics; }
    [[nodiscard]] std::span<const u64> SharedHostCallTargets() const {
        return building || Ready()
                       ? std::span<const u64>{shared_host_calls.get(), shared_host_call_count}
                       : std::span<const u64>{};
    }
    [[nodiscard]] bool DefersTerminalLocation(const ir::Inst*) const;
    [[nodiscard]] const BackedgeAction* BackedgeFor(const ir::Block*) const;
    [[nodiscard]] const PFAFAction* PFAFFor(const ir::Block*) const;
    [[nodiscard]] const AtomicCohort* AtomicFor(const ir::Inst*) const;
    [[nodiscard]] bool MatchesTargetInputs(compile::AllocationView,
                                           const Config&,
                                           const FeatureSet&) const;
    [[nodiscard]] compile::PseudoFlagsView PseudoFlagsForBlock(const ir::Block*) const;
    [[nodiscard]] bool DiscardsFlagsOnlySpill(const ir::Inst* consumer, u32 producer) const;
    [[nodiscard]] bool HasPreparedScratch(const ir::Block*) const;
    [[nodiscard]] bool MatchesScratchInputs(const ir::Block*,
                                            const GPRSMask&,
                                            const FeatureSet&) const;
    [[nodiscard]] const UnitScratchFacts& UnitScratch() const { return unit_scratch; }
    [[nodiscard]] InstructionScratchView ScratchForBlock(const ir::Block*) const;
    [[nodiscard]] const InstructionScratchFacts* ScratchFor(const ir::Inst*) const;
    [[nodiscard]] SpillInputFacts SpillInputFor(const ir::Inst* consumer, u32 producer) const;
    [[nodiscard]] const GPRResidenceAction* Residence(const ir::Inst*) const;
    [[nodiscard]] bool WidthElisionVerified(u32 id) const { return HasProof(id, Width); }
    [[nodiscard]] bool Low32ElisionVerified(u32 id) const { return HasProof(id, Low32); }
    [[nodiscard]] bool HostReadVerified(u32 id) const { return HasProof(id, HostRead); }
    [[nodiscard]] bool ZeroStoreVerified(u32 id) const { return HasProof(id, ZeroStore); }
    [[nodiscard]] bool FPRVerified(u32 id, FPRAction action) const;
    [[nodiscard]] std::optional<u8> PshufdControl(u32 id) const;
    [[nodiscard]] bool InstructionCovered(u32 id) const { return HasProof(id, Covered); }
    [[nodiscard]] const ScalarFPRAction* ScalarFPRLoad(u32 id) const;
    [[nodiscard]] const ScalarFPRAction* ScalarFPRValue(u32 id) const;
    [[nodiscard]] const ScalarFPRAction* ScalarFPRConversion(u32 id) const;
    [[nodiscard]] const ScalarFPRAction* ScalarFPRStore(u32 id) const;
    [[nodiscard]] const CarryAction* CarryForTest(u32 id) const;
    [[nodiscard]] const MemoryAction* Memory(u32 id) const;
    [[nodiscard]] const ConsumerAction* Consumer(u32 id) const;
    [[nodiscard]] const AluSplitFacts* AluSplit(u32 id) const;
    [[nodiscard]] u32 ConsumerRevision() const { return consumer_revision; }
    [[nodiscard]] const IntegerFlagsAction* IntegerFlags(u32 id) const;
    [[nodiscard]] const IntegerFlagsAction* FlagsInput(u32 id) const;
    [[nodiscard]] const IntegerFlagsAction* NarrowCarry(u32 id) const;
    [[nodiscard]] const IntegerFlagsAction* NarrowCompare(u32 id) const;
    [[nodiscard]] const IntegerBranchAction* IntegerBranch(const ir::Block*) const;
    [[nodiscard]] const PublicationAction* Publication(u32 id) const;
    [[nodiscard]] const MemoryTransferAction* MemoryTransfer(u32 id) const;
    [[nodiscard]] const AddressAction* Address(u32 id) const;
    [[nodiscard]] bool SuppressesCarryInvert(u32 id) const { return HasProof(id, CarryInvert); }
    [[nodiscard]] const SelectAction* Select(u32 id) const;
    [[nodiscard]] bool RetainsFlagsToken(u32 producer, u32 home, u32 point) const;
    [[nodiscard]] const NarrowAction* Narrow(u32 id) const;
    [[nodiscard]] bool HasScalar(u32 id, ScalarFlag flag) const {
        return HasProof(id, static_cast<Proof>(static_cast<u16>(flag) << 11));
    }

private:
    friend class AllocationPreparation;
    friend class ir::RegisterAllocTestSupport;
    enum Proof : u16 {
        Width = 1,
        Low32 = 2,
        HostRead = 4,
        ZeroStore = 8,
        Covered = 1u << 10,
        CarryInvert = 1u << 15
    };
    void Certify(u32 id, Proof proof, u32 capacity);
    void CertifyFPR(u32 id, FPRAction, u8 control, u32 capacity);
    void SealPayloads();
    [[nodiscard]] const ScalarFPRAction* FindScalarFPR(u32 id) const;
    [[nodiscard]] bool HasProof(u32 id, Proof proof) const {
        return (building || Ready()) && id < proofs.size() && (proofs[id] & proof);
    }

    compile::AllocationView allocation;
    u32 generation{}, attempt{};
    bool ready{}, building{};
    u16 shared_host_call_count{};
    u16 deferred_location_count{};
    std::vector<u16> proofs;
    struct ShuffleControl {
        u16 instruction;
        u8 control;
    };
    std::vector<ShuffleControl> shuffle_controls;
    std::vector<ScalarFPRAction> scalar_fpr;
    std::vector<SelectAction> selects;
    std::vector<FlagsTokenAction> flags_tokens;
    std::vector<NarrowAction> narrow;
    std::vector<CarryAction> carry;
    std::vector<MemoryAction> memory;
    std::vector<ConsumerAction> consumers;
    std::vector<AluSplitAction> alu_split;
    std::vector<IntegerFlagsAction> integer_flags;
    std::vector<IntegerBranchAction> integer_branches;
    std::vector<PublicationAction> publications;
    std::vector<MemoryTransferAction> memory_transfers;
    std::vector<AddressAction> addresses;
    std::vector<u16> pseudo_flags;
    std::vector<FlagsOnlySpillAction> flags_only_spills;
    UnitScratchFacts unit_scratch;
    std::vector<ScratchRange> scratch_ranges;
    std::vector<SpillInputAction> spill_inputs;
    std::vector<GPRResidenceAction> residences;
    std::unique_ptr<const UnitCertificate> certificate;
    std::unique_ptr<const u64[]> shared_host_calls;
    std::unique_ptr<const u16[]> deferred_locations;
    EdgeActions edges;
    AtomicActions atomics;
    std::vector<BackedgeAction> backedges;
    std::vector<PFAFAction> pfaf;
    u32 consumer_revision{};
    std::unordered_map<const ir::Block*, BlockActions> blocks;
};

}  // namespace swift::runtime::backend::arm64
