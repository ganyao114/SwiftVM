#pragma once
#include "region.h"
#include "runtime/arch/arm64/state.h"
#include "runtime/frontend/direct.h"
#include "runtime/ir/direct/services.h"

namespace swift::arm64 {
using runtime::frontend::CodeReadResult;
using runtime::ir::direct::AccessResult;
using runtime::ir::direct::AccessStatus;
using runtime::ir::direct::AtomicRequest;
using runtime::ir::direct::AtomicValue;

struct DirectServices {
    void* opaque{};
    CodeReadResult (*fetch)(void*, std::uint64_t, std::span<std::uint8_t>) noexcept {};
    AccessResult (*read)(void*, std::uint64_t, std::span<std::uint8_t>, bool) noexcept {};
    AccessResult (*write)(void*, std::uint64_t, std::span<const std::uint8_t>, bool) noexcept {};
    AccessResult (*atomic)(void*, const AtomicRequest&, AtomicValue&) noexcept {};
    bool (*guest_events)(void*) noexcept {};
};

using runtime::frontend::StepKind;
using runtime::frontend::StepResult;
StepResult InterpretDecoded(const Instruction&,
                            std::span<const std::uint8_t>,
                            runtime::arm64::GuestState&,
                            const DirectServices&);
StepResult InterpretOne(runtime::arm64::GuestState&,
                        const DirectServices&,
                        DecodeTiming* = nullptr);
StepResult InterpretBlock(runtime::arm64::GuestState&,
                          const DirectServices&,
                          std::size_t max_instructions = 256,
                          DecodeTiming* = nullptr,
                          ReadyEntryLookup = {});
}  // namespace swift::arm64
