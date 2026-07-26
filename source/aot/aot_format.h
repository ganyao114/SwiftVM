//
// AOT artifact: on-disk format and the runtime handshake.
//
// An artifact is a *legal* AArch64 ET_DYN ELF that carries three things:
//
//   1. the guest image, byte for byte, at its original guest virtual
//      addresses (docs/aot-design.md §1: compiled code encodes guest
//      addresses in every memory access, so guest data may not move);
//   2. the compiled ARM64 code for as many guest functions as the existing
//      function-mode path could translate, plus the relocation list produced
//      by backend/code_serial.h;
//   3. the original symbol table with every `STT_FUNC` redirected at the
//      compiled code (or at a loud-failure stub when a function could not be
//      compiled), and every `STT_OBJECT` untouched.
//
// ---------------------------------------------------------------------------
// Why the artifact is not dlopen()ed
// ---------------------------------------------------------------------------
// docs/aot-design.md §2 describes `swift_aot_init` as an entry point exported
// *by* the artifact. That shape presumes the host loader can map the artifact,
// which nothing can do here for two independent reasons: the development host
// is macOS (no ELF loader at all), and even on Linux `dlopen` would apply one
// load bias to the whole object, which is exactly what the guest-address
// constraint forbids — the guest half must land at its linked addresses while
// the host half must land wherever the code cache is.
//
// So the artifact is loaded by a SwiftVM-side loader (aot_load.h) and
// `swift_aot_init` lives in the SwiftVM image, taking the artifact path in
// SwiftAotRuntime. The contract that matters is unchanged: one call before
// any guest code runs, non-zero means "abandon this artifact".
//
// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
//   .svmaot.text  SHT_PROGBITS, ALLOC|EXECINSTR, sh_addr = kAotCodeVaddr
//                 All compiled units concatenated (16-byte aligned) followed
//                 by the failure stub. `st_value` of every rewritten
//                 `STT_FUNC` points in here.
//   .svmaot.info  SHT_PROGBITS, non-alloc. The metadata blob: validity key,
//                 guest image descriptors, unit table, relocation table and
//                 the L2 dispatch-slot assignment.
//   .svmaot.segN  SHT_PROGBITS, non-alloc. Verbatim file bytes of the guest
//                 ELF's Nth PT_LOAD. This is what the loader maps into the
//                 guest window; the copied original sections below are for
//                 symbol-table / third-party-parser fidelity and are checked
//                 against these bytes at compile time.
//   <originals>   Every section of the guest ELF, same names, same order,
//                 same sh_addr/sh_size/sh_type/sh_flags. `.symtab` has its
//                 `STT_FUNC` entries rewritten; nothing else changes.
//
#pragma once

#include <cstdint>

// ---------------------------------------------------------------------------
// C handshake
// ---------------------------------------------------------------------------
extern "C" {

// Everything the compiled code needs from the running SwiftVM. `struct_size`
// and `abi_version` are checked first so a stale caller is rejected rather
// than misread.
struct SwiftAotRuntime {
    std::uint32_t struct_size;   // must be sizeof(SwiftAotRuntime)
    std::uint32_t abi_version;   // must be kSwiftAotAbiVersion

    // swift::runtime::backend::AddressSpace* of the live instance. The
    // artifact's code is installed into its default module's code cache, so
    // it reaches host helpers, trampolines and the L2 dispatcher exactly the
    // way JIT-compiled code does.
    void* address_space;

    // Guest window: host = (guest & guest_addr_mask) + guest_memory_base.
    // Used to hash the guest bytes each unit was compiled from.
    void* guest_memory_base;
    std::uint64_t guest_addr_mask;

    // Path of the artifact ELF to install (see the header comment for why
    // this is not implicit).
    const char* artifact_path;
};

enum {
    kSwiftAotAbiVersion = 1,
};

// Install `rt->artifact_path` into `rt->address_space`.
//
// Returns 0 on success. Any non-zero value means the artifact was rejected
// and *nothing was installed*; the caller must fall back to plain JIT rather
// than run partially initialized code. Non-zero values are SwiftAotStatus.
int swift_aot_init(const SwiftAotRuntime* rt);

// Human-readable form of the last swift_aot_init() failure. Never null.
const char* swift_aot_last_error(void);

// Number of units installed by the last successful swift_aot_init().
unsigned long swift_aot_installed_units(void);

}  // extern "C"

namespace swift::aot {

enum SwiftAotStatus : int {
    kAotOk = 0,
    kAotBadArgument = 1,
    kAotUnreadable = 2,        // artifact missing / not an ELF / no AOT sections
    kAotBadFormat = 3,         // magic, version or payload checksum mismatch
    kAotKeyMismatch = 4,       // build id / config / env / guest image mismatch
    kAotGuestMismatch = 5,     // guest bytes at a unit's range are not what it saw
    kAotDispatchConflict = 6,  // L2 slot replay collision
    kAotRelocFailed = 7,       // ApplyRelocations refused a unit
    kAotNoRoom = 8,            // code cache exhausted
};

inline constexpr char kAotMagic[8] = {'S', 'V', 'M', 'A', 'O', 'T', '\0', '\1'};
inline constexpr std::uint64_t kAotFormatVersion = 1;

inline constexpr const char* kAotCodeSectionName = ".svmaot.text";
inline constexpr const char* kAotInfoSectionName = ".svmaot.info";
inline constexpr const char* kAotSegSectionPrefix = ".svmaot.seg";

// Nominal load address of `.svmaot.text` inside the artifact. It is above any
// guest window this build supports (SVM_GUEST_BITS caps at 47), so a reader
// can tell a rewritten `STT_FUNC` from an untouched `STT_OBJECT` by its value
// alone. The real host address is chosen by the code cache at load time;
// st_value is therefore an artifact-relative address, and
// swift::aot::LoadedArtifact::EntryOf() maps a symbol to the live pointer.
inline constexpr std::uint64_t kAotCodeVaddr = 0x8000'0000'0000ull;

// The failure stub every uncompiled STT_FUNC points at: `brk #0xA07`.
// Deliberately not a call into a diagnostic helper — a stub that needs the
// runtime to be wired would fail quietly when it is not.
inline constexpr std::uint32_t kAotStubInsn = 0xD42140E0u;  // brk #0xA07
inline constexpr std::uint32_t kAotStubSize = 8;            // brk + udf #0

}  // namespace swift::aot
