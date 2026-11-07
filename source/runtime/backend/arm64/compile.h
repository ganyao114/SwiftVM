#pragma once

#include "runtime/backend/code.h"

namespace swift::runtime::ir {
class Program;
}

namespace swift::runtime::backend::arm64 {

struct Allocation;

// These capabilities describe accepted neutral IR forms, not optional CPU
// instructions. CompileOptions separately enables optional RCpc loads.
constexpr ir::TargetInfo KnownTargetInfo() noexcept {
    return {ir::TargetMode::Known,
            0xa64,
            static_cast<std::uint64_t>(ir::TargetFeature::ShiftedAddress) |
                    static_cast<std::uint64_t>(ir::TargetFeature::ConditionalUniformStore) |
                    static_cast<std::uint64_t>(ir::TargetFeature::VectorSignedMulWidenLow)};
}

using backend::CompileResult;
using backend::EntryPoint;
using backend::FaultSite;
using backend::LinkKind;
using backend::LinkSite;

enum class DispatchMode : std::uint8_t { Callback, Inline, InlineCounted };

struct CompileOptions {
    // Optional packed flags field in guest_state. No field or shadow is used
    // by default. Enabled fields occupy eight bytes and may be unaligned.
    // NZCV occupy bits 31..28, AF bit 26, and the raw parity-result byte 7..0.
    // Unwritten bits are preserved. This is not an architectural x86 RFLAGS.
    std::uint32_t flags_offset{UINT32_MAX};
    bool rcpc_loads{};  // Caller has checked actual host FEAT_LRCPC support.
    bool fault_locations{};
    bool recover_unaligned{};  // Caller owns SIGBUS recovery and the fast-instruction branch-patch
                               // service.
    // Both callbacks enable the native ABI: x19 points to the state embedded
    // in NativeRunContext; metadata is addressed at fixed negative offsets.
    // x1 carries flags; x28 is available for SSA values. With native_nzcv,
    // x29 carries raw AF (bit4); otherwise it points to state.pc.
    // Only the unit's spill area is adjusted.
    // Dispatcher tails pass the actual GuestPC in x0; deferred-PC tails
    // publish state.pc only at the cold dispatcher or a real observer.
    // exit_handler instead receives the unchanged Return reason in x0.
    std::uintptr_t dispatcher{}, exit_handler{};
    std::uint32_t pc_offset{UINT32_MAX}, pinned_uniform_offset{UINT32_MAX};
    // Runtime enforces guest memory/private state disjointness and publishes
    // x1 at cold resolution or fatal faults. Requires the linked pinned ABI.
    bool private_uniforms{};
    // Low eight slots use x20..x27. Twelve slots map 8..11 to x12..x15;
    // sixteen slots map 8..15 to x8..x15. The scalar SSA banks are x2..x15,
    // x2..x11 and x2..x7 respectively.
    std::uint8_t pinned_uniform_slots{8};
    // Caller has checked FEAT_FlagM. Hardware holds guest N/Z/V and inverted
    // guest C at Complete operation/edge boundaries. One qualified Function
    // self-edge can carry DeferredTestTag in x29 instead; its fault prefix
    // has a concrete register/width recipe and every observer/unit exit is
    // Complete. Linked x1 is raw PF (low result
    // byte); x29 holds the AF input xor, so
    // guest AF=(x29^x1).bit4. Reserved/high packed bits remain in flags_offset
    // memory. Entry/reload and publication translate that one real shadow.
    // Standalone native units retain their compatible packed x1/AF ABI.
    bool native_nzcv{};
    bool rcpc_imm9{};    // Caller has checked actual FEAT_LRCPC2 (HWCAP_ILRCPC).
    bool lse_atomics{};  // Caller has checked actual FEAT_LSE (HWCAP_ATOMICS).
    bool crc32c{};       // Caller has checked actual FEAT_CRC32 (HWCAP_CRC32).
    // Linked FP fields share x19's actual NativeRunContext prefix. Helpers
    // run in its saved host environment and return to guest_fpcr with clear FPSR.
    bool native_fp_environment{};
    // Actual raw32 MXCSR field when independent input/output flush controls
    // are unavailable in hardware. It is separate from private pinned Uniform
    // fields; these real control reads use its memory value. Default callers
    // request no software flush.
    std::uint32_t mxcsr_flush_offset{UINT32_MAX};
    // Process-wide linked/private ABI with synchronous fault-PC recovery.
    // Resolved static edges and final dynamic tails defer PC publication.
    // Real observers materialize the entry PC; a cold dispatcher must publish
    // x0 before resolving. The publisher relocates every returned LinkSite.
    bool deferred_pc{};
    std::uint64_t entry_pc{};  // The actual request PC, not a guessed x0 input.
    // Explicit opt-in to abi.h's one-directory NativeRunContext layout.
    // Custom callbacks/default callers keep their original tail transfer.
    DispatchMode dispatch_mode{DispatchMode::Callback};
    // Caller consumes the block-local raw PF/carry recipes in FaultSite.
    // Default callbacks retain the complete raw-flags ABI at every operation.
    bool local_flags{};
    // Optional linked/private XMM bank: sixteen complete V128 Uniform slots
    // map to v16..v31. The runtime owns root/helper/fatal synchronization.
    // The default memory ABI retains the original dynamic vector bank.
    std::uint32_t pinned_vector_offset{UINT32_MAX};
    // The actual directory owner guarantees a valid slots/mask tuple, even
    // when empty, and a callable native/cold payload in every slot. Public
    // nullable contexts retain their guards unless this fact is supplied.
    bool callable_directory{};
    // Optional invariant software flush mode for this entire native unit:
    // bit0=DAZ, bit1=FTZ. The caller owns cache separation and must exit the
    // unit before changing these controls. Unknown retains raw MXCSR reads.
    std::uint8_t mxcsr_flush_mode{UINT8_MAX};
    // Inline dynamic sites borrow the same directory owner and its reset
    // lifetime for one fixed local native-DATA array. Callback mode stays unchanged.
    bool local_dispatch_cache{};
    // Actual host Crypto Extension capabilities; all neutral crypto forms
    // retain their pure numerical helper fallback when the feature is absent.
    bool aes{}, pmull{}, sha256{};
    bool relocatable{};  // Explicit persistence recording; ordinary JIT stays unchanged.
};

// Produces native words only; the caller owns W^X publication and cache flush.
// ABI: uint64_t function(uint8_t* guest_state). Program::EntryBlock is the entry.
// Definition dominance is an input producer precondition (see allocate.h).
// Return without a value and Dispatch return zero; Unreachable emits BRK.
// The optional fault table borrows no Program data. The caller supplies the
// actual signal recovery/publication service; this writer installs no handler.
CompileResult Compile(const ir::Program&, CompileOptions options = {});
// Reuse this actual output's capacity. Fault tables transferred to a publisher
// remain there; no Program or previous result contents are borrowed on entry.
void Compile(const ir::Program&, CompileOptions, CompileResult& output);
// The same output reuse, with the caller's actual lifetime/location owner.
// The writer borrows it only for this call, including any branch re-emission.
void Compile(const ir::Program&, CompileOptions, CompileResult& output, Allocation& allocation);

}  // namespace swift::runtime::backend::arm64
