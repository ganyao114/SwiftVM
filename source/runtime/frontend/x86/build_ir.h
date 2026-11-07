#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "runtime/ir/builder.h"
#include "runtime/frontend/build_result.h"
#include "region.h"

namespace swift::x86 {

struct StateLayout {
    std::uint32_t gprs_offset{}, pc_offset{}, stride{8};
    // Optional sixteen 16-byte XMM fields, in architectural register order.
    std::uint32_t fprs_offset{UINT32_MAX};
    // Optional U64 linear-address bases. Unset fields reject actual FS/GS
    // memory accesses; ignored prefixes on LEA/register-only forms need none.
    std::uint32_t fs_base_offset{UINT32_MAX}, gs_base_offset{UINT32_MAX};
    // Pure guest CPU query: {u64 lower,upper}(u64 leaf,u64 subleaf) noexcept.
    // Zero rejects actual CPUID; no host-CPU feature fallback is implied.
    std::uintptr_t cpu_query{};
    std::uintptr_t system_query{};
    bool runtime_state_services{};
    // Optional raw MXCSR word. Only actual LDMXCSR/STMXCSR require it.
    std::uint32_t mxcsr_offset{UINT32_MAX};
    // Optional architectural extended-precision state, described by arch/x86/x87.h.
    std::uint32_t x87_offset{UINT32_MAX};
    std::uint32_t x87_ip_offset{UINT32_MAX}, x87_dp_offset{UINT32_MAX},
            x87_opcode_offset{UINT32_MAX};
    std::uint32_t ymm_high_offset{UINT32_MAX};
};

// Native units return zero for ordinary dispatch, one for SYSCALL and three
// after LDMXCSR for the runtime to reload the guest FP environment. The runtime
// owns syscall execution and architectural RCX/R11 updates; no extra guest-state
// reason slot or per-dispatch state reset is needed.
using runtime::frontend::BuildError;
using runtime::frontend::BuildResult;

// Scalar x86-64 data movement/arithmetic, calls, stack operations and dispatch.
// Unimplemented forms are rejected. Windows are
// readable bounds, ordered by strictly increasing PC;
// decoding stops at the first terminal or the next declared entry/window end.
// Block IR stops at an interior direct-backedge leader before any IR is born;
// parser work through the original terminal is still included in DecodeTiming.
// Known targets may request a neutral shifted-address expression. EarlyOff and
// Unknown retain generic arithmetic. No physical registers enter the frontend.
// FaultLocations is enabled only with a real exception-location consumer;
// otherwise fault sites retain explicit GuestPC writes. Exits always write PC.
// No CFG discovery, old frontend or executable code is used.
// Allocation failures propagate; every reported error owns an empty Program.
// collected explicitly requests an O(N + W + B) qualification snapshot before
// owner release. Ordinary calls omit it and perform no work-count traversal.
// timing explicitly enables perturbed diagnostic clocks around actual byte
// decoder calls only; nullptr reads no such clock.
// external_edges makes every control transfer dispatch, including self edges,
// while preserving the requested IR scope and allocation path.
// Optional reusable is consumed only when IR construction starts. Its old
// borrows end, capacity transfers, and the new Program gets a fresh full ID.
[[nodiscard]] BuildResult BuildIR(
        std::span<const BlockWindow> windows,
        runtime::ir::Scope scope,
        StateLayout layout,
        runtime::ir::TargetInfo target = {},
        runtime::ir::WorkCounts* collected = nullptr,
        DecodeTiming* timing = nullptr,
        bool external_edges = false,
        runtime::ir::Program* reusable = nullptr,
        runtime::x86::GuestCpuModel model = runtime::x86::BaselineGuestCpu,
        std::vector<runtime::SourceSpan>* source_spans = nullptr,
        std::vector<std::uint64_t>* continuations = nullptr);

// The actual trusted-decoder records, with no second operand representation or
// re-decoding. Blocks select independently bounded slices in any physical
// buffer order and have increasing PCs. Each selected slice is contiguous in
// guest address and ends at its first terminal; unselected records are ignored.
// source_bytes stays immutable/alive for the call and supplies original bytes
// for prefix/form restrictions. Selected records must come from decoding those
// bytes; local size/PC/range checks do not authenticate a fabricated instruction.
// No Function discovery is implied: the caller declares every internal entry.
// CPUID and LDMXCSR are external exits and must end their selected slices.
// decoded_bytes/instruction_count count successfully emitted instructions, not
// every record supplied or its buffer materialization. A rejected operation may
// already have born a prefix; collected reports that actual abandoned work.
// Exact contiguous x87 register windows may share one semantic helper at their
// first PC; all original records retain validation and instruction/byte counts.
[[nodiscard]] BuildResult BuildDecodedIR(InstructionRange instructions,
                                         std::span<const DecodedBlock> blocks,
                                         std::span<const BlockWindow> source_bytes,
                                         runtime::ir::Scope scope,
                                         StateLayout layout,
                                         runtime::ir::TargetInfo target = {},
                                         runtime::ir::WorkCounts* collected = nullptr,
                                         std::uint32_t entry_block = 0,
                                         runtime::ir::Program* reusable = nullptr,
                                         std::vector<runtime::SourceSpan>* source_spans = nullptr,
                                         std::vector<std::uint64_t>* continuations = nullptr);

// Formation completes before any IR birth. Decode failures report zero emitted
// instruction/byte/work counts; DecodeFunction exposes their actual decode work.
[[nodiscard]] BuildResult BuildFunctionIR(
        std::span<const BlockWindow> readable,
        std::uint64_t entry_pc,
        StateLayout layout,
        DecodeLimits limits = {},
        runtime::ir::TargetInfo target = {},
        runtime::ir::WorkCounts* collected = nullptr,
        DecodeTiming* timing = nullptr,
        runtime::ir::Program* reusable = nullptr,
        ReadyEntryLookup ready = {},
        runtime::x86::GuestCpuModel model = runtime::x86::BaselineGuestCpu,
        std::vector<runtime::SourceSpan>* source_spans = nullptr,
        std::vector<std::uint64_t>* continuations = nullptr);

}  // namespace swift::x86
