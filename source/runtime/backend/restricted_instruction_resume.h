#pragma once

#include <array>
#include <memory>
#include <optional>
#include <vector>
#include "runtime/backend/retirement_control.h"
#include "runtime/ir/function.h"

namespace swift::runtime::backend {
class AddressSpace;
class Module;

constexpr u32 kRestrictedResumeParts = 64;
constexpr u64 kRestrictedResumeFflagsMask = 31;
[[nodiscard]] constexpr bool ValidInstructionStickyFlags(u64 flags) {
    return (flags & ~kRestrictedResumeFflagsMask) == 0;
}

struct InstructionResumeDescriptor {
    struct Home {
        u32 value_id{}, offset{};
        ir::ValueType type{};
        u32 bytes{}, parts{}, payload_slot{};
    };
    ir::Block* original{};
    ir::Block* emitted{};
    u64 ir_identity{};
    u64 original_identity{};
    u64 guest_pc{};
    u64 completion_pc{};
    u32 head_id{}, head_offset{}, resume_offset{};
    std::vector<Home> live;
    u32 payload_parts{};
};

// An allocator pin, with real intrusive IR ownership, held by one Runtime.
class InstructionCodePin {
public:
    ~InstructionCodePin();
    InstructionCodePin(const InstructionCodePin&) = delete;
    InstructionCodePin& operator=(const InstructionCodePin&) = delete;
    std::shared_ptr<Module> module;
    std::shared_ptr<const InstructionResumeDescriptor> descriptor;
    u8* allocation{};
    u64 allocation_id{};
    std::vector<ir::NodeRef> ir_owners;
private:
    friend class Module;
    InstructionCodePin() = default;
    // A public pin can outlive its Runtime. This separate host owner blocks
    // terminal release while allowing ordinary drain/resume generations.
    u64 terminal_request_{};
    std::optional<RetirementControl::ExitRegistration> terminal_owner_;
};

// Generated code only accesses this preallocated capture payload.
struct InstructionCapture {
    enum Status : u64 { Empty, Armed, Suspended, Resuming, Abandoned };
    u64 status{};
    u64 head_id{};
    u64 guest_pc{};
    u64 guest_flags{};
    u64 sticky_fflags{};
    std::array<u64, kRestrictedResumeParts> payload{};
};

[[nodiscard]] u64 InstructionIRIdentity(ir::Block& block);
// Checks emitted SSA provenance and the backend's real physical home contract.
[[nodiscard]] bool ValidateInstructionResumeHomes(const InstructionResumeDescriptor& descriptor);
}  // namespace swift::runtime::backend
