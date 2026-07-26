//
// GuestCallEnv backed by the existing JIT path (translator::x86::X86Core).
//
// This is the "what is the entry" injection point named in docs/aot-design.md
// §6: everything above it (abi_sysv.h, GuestFn, ScopedGuestBuffer) is pure
// marshalling and does not know how the guest runs.  An AOT artifact would
// implement RunToSentinel() by jumping into compiled host code instead of
// calling X86Core::Run(); no marshalling code changes.
//
// The return path uses a SENTINEL PAGE: one guest page filled with `hlt`
// (0xF4).  Its address is pushed as the return address, so the guest's final
// `ret` lands on it, the x86 frontend raises InterruptReason::HLT and
// X86Core::Run() returns ExitReason::None with rip inside the page.  That
// makes "returned normally" a *positive* signal rather than the absence of an
// error, which is what lets a fault be distinguished from a return.
//

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include "guest_call.h"
#include "guest_space.h"

namespace swift::translator::x86 {
class X86Instance;
class X86Core;
}  // namespace swift::translator::x86

namespace swift::guest_call {

class JitGuestEnv : public GuestCallEnv {
public:
    struct Layout {
        std::uint64_t call_stack_base{0x60000000};
        std::uint64_t call_stack_size{1u << 20};
        std::uint64_t sentinel_page{0x60200000};
        std::uint64_t arena_base{0x60300000};
        std::uint64_t arena_size{4u << 20};
        std::uint64_t main_stack_top{0x7FF00000};
        std::uint64_t main_stack_size{8u << 20};
    };

    JitGuestEnv();
    ~JitGuestEnv() override;

    // Loads `elf_path` into a fresh guest window and brings up an X86Core.
    bool Init(const std::string& elf_path, std::string& error);

    // Runs the guest from its ELF entry point (a real Linux startup: initial
    // stack, auxv, a minimal syscall emulation) and stops on the first byte of
    // `stop_symbol`, which is patched to `hlt` before the run.  After this
    // returns, libc is fully initialized -- IRELATIVE ifunc relocations
    // applied, TLS installed, locale tables up -- so the call layer can call
    // ordinary libc functions.  The patched byte is intentionally NOT restored:
    // nothing calls `main` afterwards, and restoring it would mean writing to
    // a page the SMC tracker may have write-protected.
    bool RunStartupUntil(const std::string& stop_symbol, std::string& error);

    // GuestCallEnv ---------------------------------------------------------
    swift::x86::ThreadContext64& Context() override;
    void* HostPointer(std::uint64_t guest_addr, std::uint64_t size) override;
    [[nodiscard]] std::uint64_t SentinelAddress() const override { return layout_.sentinel_page; }
    GuestRunReport RunToSentinel() override;
    [[nodiscard]] std::uint64_t CallStackTop() override;
    std::uint64_t ScratchAlloc(std::uint64_t size, std::uint64_t align) override;
    void ScratchFree(std::uint64_t addr, std::uint64_t size) override;
    std::uint64_t LookupSymbol(const std::string& name) override;

    // The raw st_value of a symbol, WITHOUT ifunc resolution. Only useful to
    // show that resolution actually happened.
    [[nodiscard]] std::uint64_t RawSymbol(const std::string& name) const;
    [[nodiscard]] bool IsIfunc(const std::string& name) const {
        return image_.ifuncs.count(name) != 0;
    }

    // ----------------------------------------------------------------------
    GuestSpace& space() { return space_; }
    const GuestSpace::Image& image() const { return image_; }
    void SetLayout(const Layout& l) { layout_ = l; }
    [[nodiscard]] std::uint64_t exit_code() const { return exit_code_; }
    [[nodiscard]] bool exited() const { return exited_; }

    // Diagnostics: how many guest instructions' worth of syscalls were seen,
    // and the last unimplemented syscall number (0 = none).
    [[nodiscard]] std::uint64_t last_unknown_syscall() const { return unknown_syscall_; }

private:
    bool HandleSyscall();

    GuestSpace space_;
    GuestSpace::Image image_{};
    Layout layout_{};
    swift::translator::x86::X86Instance* instance_{};
    swift::translator::x86::X86Core* core_{};
    std::uint64_t brk_{};
    std::uint64_t brk_start_{};
    std::uint64_t mmap_next_{};
    std::uint64_t unknown_syscall_{};
    std::uint64_t exit_code_{};
    std::unordered_map<std::string, std::uint64_t> resolved_{};
    bool exited_{};
    bool started_{};
};

}  // namespace swift::guest_call
