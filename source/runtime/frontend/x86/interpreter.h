#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "region.h"
#include "runtime/ir/direct/services.h"
#include "runtime/frontend/direct.h"

namespace swift::runtime::x86 {
struct GuestState;
}

namespace swift::x86 {

// Memory/atomic results belong to the IR direct execution service. Fetch and
// architectural state-image events remain the x86 adapter's responsibility.
using runtime::ir::direct::AccessResult;
using runtime::ir::direct::AccessStatus;
using runtime::ir::direct::AtomicOperation;
using runtime::ir::direct::AtomicRequest;
using runtime::ir::direct::AtomicValue;
using runtime::ir::direct::CpuResult;

using runtime::frontend::CodeReadResult;

// Actual execution services, bound once by the VM. All accesses report failure
// without unwinding across guest code. Read/write buffers carry exactly the
// access width; ordered accesses and atomics retain their real memory semantics.
// UnsupportedBeforeAccess promises no memory or architectural side effect.
struct DirectServices {
    void* opaque{};
    CodeReadResult (*fetch)(void*, std::uint64_t pc, std::span<std::uint8_t>) noexcept {};
    AccessResult (*read)(void*, std::uint64_t, std::span<std::uint8_t>, bool ordered) noexcept {};
    AccessResult (*write)(void*,
                          std::uint64_t,
                          std::span<const std::uint8_t>,
                          bool ordered) noexcept {};
    AccessResult (*atomic)(void*, const AtomicRequest&, AtomicValue& observed) noexcept {};
    CpuResult (*cpuid)(void*, std::uint32_t leaf, std::uint32_t subleaf) noexcept {};
    runtime::x86::GuestCpuModel model{runtime::x86::BaselineGuestCpu};
    AccessResult (*state_image)(void*,
                                unsigned kind,
                                std::uint64_t address,
                                std::uint64_t requested) noexcept {};
};

using runtime::frontend::StepKind;
using runtime::frontend::StepResult;

// Consume the same owned DecodeOne record and original bytes accepted by the
// IR builder. This path executes neutral IR operations immediately: no fetch,
// re-decode, Builder or Program. Caller retains the same guest model and code
// version. PC/length/prefix admission occurs before executing the instruction.
[[nodiscard]] StepResult InterpretDecoded(const Instruction&,
                                          std::span<const std::uint8_t> source_bytes,
                                          runtime::x86::GuestState&,
                                          const DirectServices&);

// One architectural instruction, or one restartable REP iteration. No region,
// Builder, Program, allocation, native code or persistent decoded cache exists.
// All completed effects precede next_pc; faults retain the current instruction
// and its architecturally defined partial completion. Unsupported permits choosing another consumer
// only before observable effects; Incomplete and Fault retain partial progress and must never be
// replayed.
[[nodiscard]] StepResult InterpretOne(runtime::x86::GuestState&,
                                      const DirectServices&,
                                      DecodeTiming* timing = nullptr);

// Execute a linear fragment, stopping at the first control, service, fault,
// code-change or instruction-budget boundary. Counts include completed steps;
// pc identifies the stopping instruction, so a failed suffix never replays its
// completed prefix. One restartable REP iteration is always a boundary.
// After at least one completed instruction, a ready entry ends the fragment
// before fetching that instruction so the caller can enter its native code.
[[nodiscard]] StepResult InterpretBlock(runtime::x86::GuestState&,
                                        const DirectServices&,
                                        std::size_t max_instructions = 256,
                                        DecodeTiming* timing = nullptr,
                                        ReadyEntryLookup ready = {});

}  // namespace swift::x86
