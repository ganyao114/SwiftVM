#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "code_cache.h"
#include "code_heap.h"
#include "code_image.h"
#include "compile_service.h"
#include "guest_thread.h"
#include "host_backend.h"
#include "memory.h"
#include "runtime.h"
#include "runtime/frontend/arm32/interpreter.h"
#include "runtime/frontend/arm64/interpreter.h"
#include "runtime/frontend/x86/interpreter.h"
#include "runtime/platform/linux/guest_signals.h"
#include "runtime/platform/linux/loader.h"
#include "runtime/platform/linux/syscalls.h"
#include "smc.h"

#if defined(SVM_TEST_BOUNDARY)
namespace swift::tests::correctness {
struct VMAccess;
}
#endif

namespace swift::runtime::execution {
class DiskCache;

class VM {
public:
    VM(RuntimeOptions, RuntimeStats&, const HostBackendOps&);
    ~VM();
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;
    int Run(const std::string&,
            std::span<const std::string> argv,
            std::span<const std::string> environment);
    void PrepareGuest(const std::string&,
                      std::span<const std::string> argv = {},
                      std::span<const std::string> environment = {});
    void PrepareGuest(std::span<const std::uint8_t>,
                      const std::string& display_path,
                      std::span<const std::string> argv = {},
                      std::span<const std::string> environment = {},
                      const CodeImage* native = nullptr);
    int RunPrepared();
    void CallGuest(std::uint64_t pc, std::uint64_t stack_pointer, std::uint64_t stop_pc);
    void RunUntilGuestStop(std::uint64_t stop_pc);

    x86::GuestState& State() noexcept { return thread.native.state; }

    arm64::GuestState& ArmState() noexcept { return thread.native.arm_state; }

    arm32::GuestState& Arm32State() noexcept { return thread.native.arm32_state; }

    GuestISA ISA() const noexcept { return layout.isa; }

    // Owner APIs and raw host borrows are not concurrent VM operations. Access
    // guest bytes while execution is stopped, or hold GuestExecutionLock for
    // the whole access, including any dereference after HostPointer returns.
    GuestMemory& Memory() noexcept { return memory; }

    void* HostPointer(std::uint64_t address, std::size_t size, bool write = false);
    CodeImage Precompile(const std::string&, std::span<const std::uint64_t> entries, bool sweep);
    CodeImage Precompile(std::span<const std::uint8_t>,
                         const std::string& display_path,
                         std::span<const std::uint64_t> entries,
                         bool sweep);
    // Call after Run (also on failure). Remaining mappings stay in this VM
    // when Release reports a failure; no resource is transferred to a registry.
    MappingRelease Close() noexcept;

    GuestStopReason StopReason() const noexcept { return thread.stop_reason; }

    int TerminationSignal() const noexcept { return thread.termination_signal; }

    std::unique_ptr<GuestExceptionSnapshot> CaptureException() const;

private:
    friend class CompileService;
    friend class Smc;
    friend class platform::LinuxSyscalls;
    friend class platform::GuestSignals;
#if defined(SVM_TEST_BOUNDARY)
    friend struct ::swift::tests::correctness::VMAccess;
#endif
    RuntimeOptions options;
    RuntimeStats& stats;
    HostBackendOps backend;
    GuestMemory memory;
    CodeHeap heap;
    std::array<std::unique_ptr<CodeCache>, 128> caches;
    DispatchCache empty_l1;
    DispatchInterrupt interrupt_l1;
    CompileService compiler;
    GuestThread thread;
    platform::LoadedImage image;
    platform::LinuxSyscalls syscalls;
    platform::GuestSignals signals;
    Smc smc;
    std::unique_ptr<DiskCache> disk_cache;
    CodeImageKey image_key;
    GuestLayout layout;
    void* uniform_data{};
    CodeImage* offline_output{};
    bool prepared{}, persistence_disabled{}, host_call_active{}, host_returned{};
    std::uint64_t host_return_pc{};
#if defined(SVM_TEST_BOUNDARY)
    bool test_boundary_active{};
    std::uint64_t test_boundary_service{}, test_boundary_native_status{};
#endif
    bool final_observed{}, closing{}, closed{};
    MappingRelease close_result;

    CodeCache& Cache() noexcept { return *caches[thread.fp_mode]; }

    void SelectFPMode(std::uint8_t);

    std::uint64_t& PC() noexcept {
        return *reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(uniform_data) +
                                                 layout.pc_offset);
    }

    std::uint64_t& SP() noexcept {
        return *reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(uniform_data) +
                                                 layout.stack_offset);
    }

    std::uint64_t& Flags() noexcept {
        return *reinterpret_cast<std::uint64_t*>(static_cast<std::uint8_t*>(uniform_data) +
                                                 layout.flags_offset);
    }

    std::uint32_t& NumericControl() noexcept {
        return *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(uniform_data) +
                                                 layout.numeric_control_offset);
    }

    void ConfigureGuest(GuestISA);

    void ObserveCache() noexcept;
    void ObserveFinal() noexcept;
    void EnsureThreadDispatch();
    void RefreshDispatchView() noexcept;
    void RefreshNative(NativeRunContext&) noexcept;
    void MergeFPStatus() noexcept;
    void UpdateFPEnvironment(bool architectural_write = false);

    void RestoreGuestFP() { UpdateFPEnvironment(true); }

    void DeliverException(const GuestExceptionInfo&);
    void DeliverPending();
    void RaiseSemanticException(NativeExit, std::uint64_t instruction_pc);
    void RaiseEncodedException(std::uint64_t);
    void PrepareLoaded(std::span<const std::string>,
                       std::span<const std::string>,
                       const Digest* guest_identity,
                       const CodeImage*);
    void Execute();
    void ExecuteHostCall(std::uint64_t stop_pc);

    bool IsHostReturn(std::uint64_t pc) const noexcept {
        if (!host_call_active) return false;
#if defined(SVM_TEST_BOUNDARY)
        if (test_boundary_active) return CodeAddress(ISA(), pc) == host_return_pc;
#endif
        return pc == host_return_pc;
    }

    void InstallImage(const CodeImage&);
    bool HashSource(SourceSpan, Digest&, std::uint8_t& mode) const;
    bool SourcesMatch(std::span<const SourceRecord>) const;
    bool CollectCode() const noexcept;
    void RecordUnit(ir::Scope,
                    const backend::CompileResult&,
                    NativeCode,
                    std::span<const SourceSpan>);
    void FlushDisk() noexcept;
    void DisablePersistence() noexcept;
    void Invalidate();
    std::uintptr_t Resolve(NativeRunContext&);
    static std::uintptr_t ResolveBridge(void*, NativeRunContext*) noexcept;
    static const NativeCode* LookupNative(void*, std::uintptr_t) noexcept;
    static bool RecoverCodeWrite(void*, std::uintptr_t) noexcept;
    static std::uint64_t AtomicBridge(void*, AtomicCall*) noexcept;

    void ClearReservation() noexcept {
        if (ISA() == GuestISA::Arm64)
            ArmState().reservation_valid = 0;
        else if (ISA() == GuestISA::Arm32)
            Arm32State().reservation_valid = 0;
    }

    bool HandleArmStateService();
    bool HandleArm32StateService();
    void ServiceArmState();
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
