#pragma once

#include <optional>
#include <unordered_set>
#include <vector>

#include "runtime/backend/edge_flags_state.h"
#include "runtime/ir/function_entry_provenance.h"

namespace swift::runtime::ir {
class Block;
class HIRBlock;
class HIRFunction;
}

namespace swift::runtime::backend {

class Module;

inline constexpr u32 kInvalidFunctionEntryOffset = UINT32_MAX;

enum class FunctionEntryKind : u8 {
    Canonical,
    DirectLink,
    PendingFlags,
    Continuation,
    PendingFlagsContinuation,
    Internal,
};

enum class FunctionEntryFlagsRequirement : u8 {
    Canonical,
    Pending,
};

enum class FunctionEntryContinuationRequirement : u8 {
    None,
    PublishedFrame,
};

enum class FunctionEntryGuestStateRequirement : u8 {
    CanonicalFixedHomes,
};

enum class FunctionEntryOrigin : u8 {
    FunctionRoot,
    DecodedBlock,
    AcceptedSplit,
    RejectedSplit,
    SerializedCodeObject,
};

struct FunctionEntryCodeOffsets {
    u32 canonical{kInvalidFunctionEntryOffset};
    u32 direct_link{kInvalidFunctionEntryOffset};
    u32 pending_flags{kInvalidFunctionEntryOffset};
    u32 continuation{kInvalidFunctionEntryOffset};
    u32 pending_flags_continuation{kInvalidFunctionEntryOffset};
};

struct FunctionEntryPointContract {
    FunctionEntryKind kind{FunctionEntryKind::Internal};
    FunctionEntryFlagsRequirement flags{
            FunctionEntryFlagsRequirement::Canonical};
    FunctionEntryContinuationRequirement continuation{
            FunctionEntryContinuationRequirement::None};
    FunctionEntryGuestStateRequirement guest_state{
            FunctionEntryGuestStateRequirement::CanonicalFixedHomes};
    u32 code_offset{kInvalidFunctionEntryOffset};

    [[nodiscard]] bool Present() const {
        return code_offset != kInvalidFunctionEntryOffset;
    }
};

struct FunctionEntryDependency {
    ir::Location start{};
    ir::Location end{};

    bool operator==(const FunctionEntryDependency&) const = default;
};

class FunctionEntryContract final {
public:
    [[nodiscard]] static FunctionEntryContract Build(const ir::HIRFunction& function,
                                                     const ir::Block& block,
                                                     FunctionEntryCodeOffsets offsets,
                                                     EdgeFlagsTargetContract pending_flags_contract,
                                                     bool canonical_terminal = false);
    [[nodiscard]] static std::unordered_set<u64> AnalyzeCanonicalTerminalEntries(
            const ir::HIRFunction& function);
    [[nodiscard]] static FunctionEntryContract Restore(
            ir::Location guest,
            ir::Location guest_end,
            FunctionEntryCodeOffsets offsets,
            EdgeFlagsTargetContract pending_flags_contract,
            bool linkable = true);

    [[nodiscard]] bool IsWellFormed(u32 allocation_size) const;
    [[nodiscard]] const FunctionEntryPointContract& Canonical() const {
        return canonical;
    }
    [[nodiscard]] const FunctionEntryPointContract& DirectLink() const {
        return direct_link;
    }
    [[nodiscard]] const FunctionEntryPointContract& PendingFlags() const {
        return pending_flags;
    }
    [[nodiscard]] const FunctionEntryPointContract& Continuation() const {
        return continuation;
    }
    [[nodiscard]] const FunctionEntryPointContract&
    PendingFlagsContinuation() const {
        return pending_flags_continuation;
    }
    [[nodiscard]] ir::Location Guest() const { return guest; }
    [[nodiscard]] ir::Location GuestEnd() const { return guest_end; }
    [[nodiscard]] FunctionEntryOrigin Origin() const { return origin; }
    [[nodiscard]] bool Linkable() const { return linkable; }
    [[nodiscard]] const std::vector<FunctionEntryDependency>& Dependencies()
            const {
        return dependencies;
    }
    [[nodiscard]] const std::optional<ir::FunctionEntryProvenance>& Provenance()
            const {
        return provenance;
    }
    [[nodiscard]] EdgeFlagsTargetContract PendingFlagsContract() const {
        return pending_flags_contract;
    }

private:
    ir::Location guest{};
    ir::Location guest_end{};
    FunctionEntryOrigin origin{FunctionEntryOrigin::DecodedBlock};
    FunctionEntryPointContract canonical{};
    FunctionEntryPointContract direct_link{};
    FunctionEntryPointContract pending_flags{};
    FunctionEntryPointContract continuation{};
    FunctionEntryPointContract pending_flags_continuation{};
    std::vector<FunctionEntryDependency> dependencies{};
    std::optional<ir::FunctionEntryProvenance> provenance{};
    EdgeFlagsTargetContract pending_flags_contract{};
    bool linkable{true};
};

struct PublishedFunctionEntry {
    ir::Location guest{};
    void* canonical_host_pc{};
    u64 generation{};
};

class FunctionEntryPublisher final {
public:
    FunctionEntryPublisher(Module& module, u8* allocation, u32 allocation_size)
            : module(module), allocation(allocation), allocation_size(allocation_size) {}

    [[nodiscard]] PublishedFunctionEntry Publish(
            const FunctionEntryContract& contract) const;

private:
    [[nodiscard]] void* HostPC(
            const FunctionEntryPointContract& entry) const;

    Module& module;
    u8* allocation{};
    u32 allocation_size{};
};

}  // namespace swift::runtime::backend
