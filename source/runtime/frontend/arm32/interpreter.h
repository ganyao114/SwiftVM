#pragma once

#include "region.h"
#include "runtime/arch/arm32/state.h"
#include "runtime/frontend/direct.h"
#include "runtime/ir/direct/services.h"

namespace swift::arm32 {
using runtime::frontend::CodeReadResult;
using runtime::ir::direct::AccessResult;
using runtime::ir::direct::AccessStatus;
using runtime::ir::direct::AtomicRequest;
using runtime::ir::direct::AtomicValue;

struct DirectServices {
    void* opaque{};
    CodeReadResult (*fetch)(void*, u64, std::span<u8>) noexcept {};
    AccessResult (*read)(void*, u64, std::span<u8>, bool) noexcept {};
    AccessResult (*write)(void*, u64, std::span<const u8>, bool) noexcept {};
    AccessResult (*atomic)(void*, const AtomicRequest&, AtomicValue&) noexcept {};
    bool (*guest_events)(void*) noexcept {};
};

using runtime::frontend::StepKind;
using runtime::frontend::StepResult;
StepResult InterpretDecoded(const Instruction&,
                            std::span<const u8>,
                            runtime::arm32::GuestState&,
                            const DirectServices&,
                            Profile = {});
StepResult InterpretOne(runtime::arm32::GuestState&,
                        const DirectServices&,
                        DecodeTiming* = nullptr,
                        Profile = {});
StepResult InterpretBlock(runtime::arm32::GuestState&,
                          const DirectServices&,
                          std::size_t max_instructions = 256,
                          DecodeTiming* = nullptr,
                          ReadyEntryLookup = {},
                          Profile = {});
}  // namespace swift::arm32
