#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "runtime/ir/builder.h"
#include "decode.h"

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
    // Optional raw MXCSR word. Only actual LDMXCSR/STMXCSR require it.
    std::uint32_t mxcsr_offset{UINT32_MAX};
};
// Native units return zero for ordinary dispatch, one for SYSCALL and three
// after LDMXCSR for the runtime to reload the guest FP environment. The runtime
// owns syscall execution and architectural RCX/R11 updates; no extra guest-state
// reason slot or per-dispatch state reset is needed.
enum class BuildError : std::uint8_t {
    None, InvalidInput, InvalidEncoding, Truncated, Unsupported, IRRejected,
};
struct BuildResult {
    BuildError error{BuildError::None};
    std::uint64_t error_pc{};
    std::size_t decoded_bytes{}, instruction_count{};
    runtime::ir::Error ir_error{runtime::ir::Error::None};
    runtime::ir::Program program;
    explicit operator bool() const noexcept {
        return error == BuildError::None && bool(program);
    }
};

// Scalar x86-64 data movement/arithmetic, calls, stack operations and dispatch.
// Unimplemented forms are rejected. Windows are
// readable bounds, ordered by strictly increasing PC;
// decoding stops at the first terminal or the next declared entry/window end.
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
[[nodiscard]] BuildResult BuildIR(std::span<const BlockWindow> windows,
                                  runtime::ir::Scope scope,
                                  StateLayout layout,
                                  runtime::ir::TargetInfo target = {},
                                  runtime::ir::WorkCounts* collected = nullptr,
                                  DecodeTiming* timing = nullptr,
                                  bool external_edges = false);

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
[[nodiscard]] BuildResult BuildDecodedIR(std::span<const _DInst> instructions,
                                         std::span<const DecodedBlock> blocks,
                                         std::span<const BlockWindow> source_bytes,
                                         runtime::ir::Scope scope,
                                         StateLayout layout,
                                         runtime::ir::TargetInfo target = {},
                                         runtime::ir::WorkCounts* collected = nullptr,
                                         std::uint32_t entry_block = 0);

// Formation completes before any IR birth. Decode failures report zero emitted
// instruction/byte/work counts; DecodeFunction exposes their actual decode work.
[[nodiscard]] BuildResult BuildFunctionIR(std::span<const BlockWindow> readable,
                                          std::uint64_t entry_pc,
                                          StateLayout layout,
                                          DecodeLimits limits = {},
                                          runtime::ir::TargetInfo target = {},
                                          runtime::ir::WorkCounts* collected = nullptr,
                                          DecodeTiming* timing = nullptr);

}  // namespace swift::x86
