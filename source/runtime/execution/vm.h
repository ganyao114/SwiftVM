#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>

#include "code_cache.h"
#include "code_heap.h"
#include "compile_service.h"
#include "guest_thread.h"
#include "host_backend.h"
#include "memory.h"
#include "runtime.h"
#include "smc.h"
#include "runtime/frontend/x86/interpreter.h"
#include "runtime/platform/linux/loader.h"
#include "runtime/platform/linux/syscalls.h"

namespace swift::runtime::execution {

class VM {
public:
    VM(RuntimeOptions, RuntimeStats&, const HostBackendOps&);
    ~VM();
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;
    int Run(const std::string&,
            std::span<const std::string> argv,
            std::span<const std::string> environment);
    // Call after Run (also on failure). Remaining mappings stay in this VM
    // when Release reports a failure; no resource is transferred to a registry.
    MappingRelease Close() noexcept;

private:
    friend class CompileService;
    friend class Smc;
    friend class platform::LinuxSyscalls;
    RuntimeOptions options;
    RuntimeStats& stats;
    HostBackendOps backend;
    GuestMemory memory;
    CodeHeap heap;
    std::array<CodeCache, 4> caches;
    CompileService compiler;
    GuestThread thread;
    platform::LoadedImage image;
    platform::LinuxSyscalls syscalls;
    Smc smc;
    bool final_observed{}, closing{}, closed{};
    MappingRelease close_result;

    CodeCache& Cache() noexcept { return caches[thread.fp_mode]; }

    void ObserveCache() noexcept;
    void ObserveFinal() noexcept;
    void RefreshNative(NativeRunContext&) noexcept;
    void UpdateFPEnvironment();
    void Invalidate();
    std::uintptr_t Resolve(NativeRunContext&);
    static std::uintptr_t ResolveBridge(void*, NativeRunContext*) noexcept;
    static const NativeCode* LookupNative(void*, std::uintptr_t) noexcept;
    static bool RecoverCodeWrite(void*, std::uintptr_t) noexcept;
    bool PrepareGuestWrite(std::uint64_t, std::size_t);
    bool GuestWrite(std::uint64_t, const void*, std::size_t);
    void FileChanged(int);
    void Interpret(bool block = false);
    ::swift::x86::DirectServices DirectServices() noexcept;
    static ::swift::x86::CodeReadResult Fetch(void*,
                                              std::uint64_t,
                                              std::span<std::uint8_t>) noexcept;
    static ::swift::x86::AccessResult Read(void*,
                                           std::uint64_t,
                                           std::span<std::uint8_t>,
                                           bool) noexcept;
    static ::swift::x86::AccessResult Write(void*,
                                            std::uint64_t,
                                            std::span<const std::uint8_t>,
                                            bool) noexcept;
    static ::swift::x86::AccessResult Atomic(void*,
                                             const ::swift::x86::AtomicRequest&,
                                             ::swift::x86::AtomicValue&) noexcept;
    static ::swift::x86::CpuResult Cpu(void*, std::uint32_t, std::uint32_t) noexcept;
    static ::swift::x86::AccessResult StateImage(void*,
                                                 unsigned,
                                                 std::uint64_t,
                                                 std::uint64_t) noexcept;
};
}  // namespace swift::runtime::execution
