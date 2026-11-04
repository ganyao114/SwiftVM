#pragma once

#include "allocate.h"

namespace swift::runtime::backend::arm64 {

// These capabilities describe accepted neutral IR forms, not optional CPU
// instructions. CompileOptions separately enables optional RCpc loads.
constexpr ir::TargetInfo KnownTargetInfo() noexcept {
    return {ir::TargetMode::Known, 0xa64,
            static_cast<std::uint64_t>(ir::TargetFeature::ShiftedAddress) |
            static_cast<std::uint64_t>(ir::TargetFeature::ConditionalUniformStore) |
            static_cast<std::uint64_t>(ir::TargetFeature::VectorSignedMulWidenLow)};
}

struct FaultSite {
    std::uint32_t first_word{}, last_word{}; // Actual native operation range, end exclusive.
    std::uint64_t guest_pc{};
    // Only an alignment fault at the actual fast instruction is recoverable.
    // Its cold fallback occupies [unaligned_fallback_word,+3): plain memory,
    // the TSO half-barrier and a branch back after the actual fast instruction.
    // Store fallbacks put the barrier before the plain store. This same site
    // supplies the guest-PC/TEST recipe for both its hot and cold ranges.
    std::uint32_t unaligned_word{UINT32_MAX}, unaligned_fallback_word{UINT32_MAX};
    // A single admitted Function loop may defer full TEST flags. When the
    // raw-AF tag is set, this actual register contains its entry snapshot.
    std::uint8_t test_bits{}, test_register{};
    // Block-local raw flags at the actual memory instruction. Zero PF source
    // means x1; otherwise it is an unchanged pinned result's low byte.
    // carry_direct means PSTATE.C already equals guest CF, without inversion.
    std::uint8_t parity_register{}, carry_direct{};
};
static_assert(sizeof(FaultSite) == 32);

struct LinkSite {
    std::uint32_t pointer_word{}; // Eight-byte aligned native DATA relocation.
    std::uint32_t branch_word{}; // Actual patchable transfer head; cold stub is pointer_word + 2.
    std::uint64_t target_pc{};
};
static_assert(sizeof(LinkSite) == 16);

struct CompileResult {
    CompileError error{CompileError::None};
    ir::NodeId node{};
    ir::BlockId block{};
    std::vector<std::uint32_t> words;
    std::uint32_t frame_bytes{}, spill_slots{}; // Default ABI includes its 16-byte x19/x30 save.
    explicit operator bool() const noexcept { return error == CompileError::None; }
    std::vector<FaultSite> faults;
    std::vector<LinkSite> links;
};

enum class DispatchMode : std::uint8_t { Callback, Inline, InlineCounted };

struct CompileOptions {
    // Optional packed flags field in guest_state. No field or shadow is used
    // by default. Enabled fields occupy eight bytes and may be unaligned.
    // NZCV occupy bits 31..28, AF bit 26, and the raw parity-result byte 7..0.
    // Unwritten bits are preserved. This is not an architectural x86 RFLAGS.
    std::uint32_t flags_offset{UINT32_MAX};
    bool rcpc_loads{}; // Caller has checked actual host FEAT_LRCPC support.
    bool fault_locations{};
    bool recover_unaligned{}; // Caller owns SIGBUS recovery and the fast-instruction branch-patch service.
    // Both callbacks enable the native ABI: x19=state, x1=flags,
    // x28=context. With native_nzcv, x29 carries raw AF (bit4); otherwise it
    // points to state.pc. Only the unit's spill area is adjusted.
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
    bool rcpc_imm9{}; // Caller has checked actual FEAT_LRCPC2 (HWCAP_ILRCPC).
    bool lse_atomics{}; // Caller has checked actual FEAT_LSE (HWCAP_ATOMICS).
    bool crc32c{}; // Caller has checked actual FEAT_CRC32 (HWCAP_CRC32).
    // Linked x28 points to the real NativeRunContext FP owner. Helpers run in
    // its saved host environment and return to guest_fpcr with clear FPSR.
    bool native_fp_environment{};
    // Actual raw32 MXCSR field when independent input/output flush controls
    // are unavailable in hardware. Default callers request no software flush.
    std::uint32_t mxcsr_flush_offset{UINT32_MAX};
    // Process-wide linked/private ABI with synchronous fault-PC recovery.
    // Resolved static edges and final dynamic tails defer PC publication.
    // Real observers materialize the entry PC; a cold dispatcher must publish
    // x0 before resolving. The publisher relocates every returned LinkSite.
    bool deferred_pc{};
    std::uint64_t entry_pc{}; // The actual request PC, not a guessed x0 input.
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
};

// Produces native words only; the caller owns W^X publication and cache flush.
// ABI: uint64_t function(uint8_t* guest_state). Program::EntryBlock is the entry.
// Definition dominance is an input producer precondition (see allocate.h).
// Return without a value and Dispatch return zero; Unreachable emits BRK.
// The optional fault table borrows no Program data. The caller supplies the
// actual signal recovery/publication service; this writer installs no handler.
CompileResult Compile(const ir::Program&, CompileOptions options = {});

} // namespace swift::runtime::backend::arm64
