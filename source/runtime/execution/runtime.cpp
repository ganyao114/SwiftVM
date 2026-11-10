#include "atomic_service.h"
#include "disk_cache.h"
#include "runtime/platform/linux/artifact_file.h"
#include "timing.h"
#include "vm.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cerrno>
#include <stdexcept>
#include <elf.h>
#include <sys/mman.h>
#include "base/error.h"
#include "base/format.h"

#include "runtime/arch/arm32/numeric.h"
#include "runtime/arch/arm64/numeric.h"
#include "runtime/arch/x86/cpu.h"
#include "runtime/platform/linux/abi_arm32.h"
#include "runtime/platform/linux/arm32_user_helpers.h"

namespace swift::runtime::execution {
namespace front = ::swift::x86;

namespace {
using u64 = std::uint64_t;

MappingRelease Merge(MappingRelease a, MappingRelease b) noexcept {
    a.remaining_views += b.remaining_views;
    if (!a.unmap_error) a.unmap_error = b.unmap_error;
    if (!a.close_error) a.close_error = b.close_error;
    return a;
}

}  // namespace

VM::VM(RuntimeOptions options, RuntimeStats& stats, const HostBackendOps& host)
        : options(options)
        , stats(stats)
        , backend(host)
        , heap(backend, memory.PageSize())
        , caches{CodeCache{reinterpret_cast<NativeCode::Function>(options.decode_timing
                                                                          ? &SvmNativePendingCounted
                                                                          : &SvmNativePending),
                           backend},
                 CodeCache{reinterpret_cast<NativeCode::Function>(options.decode_timing
                                                                          ? &SvmNativePendingCounted
                                                                          : &SvmNativePending),
                           backend},
                 CodeCache{reinterpret_cast<NativeCode::Function>(options.decode_timing
                                                                          ? &SvmNativePendingCounted
                                                                          : &SvmNativePending),
                           backend},
                 CodeCache{reinterpret_cast<NativeCode::Function>(options.decode_timing
                                                                          ? &SvmNativePendingCounted
                                                                          : &SvmNativePending),
                           backend}}
        , compiler(*this)
        , syscalls(*this)
        , signals(*this)
        , smc(*this) {
    stats.compile_decode.stages = options.decode_timing && options.compile_details;
    auto& native = thread.native;
    auto& faults = thread.faults;
    backend.target.features |= static_cast<u64>(ir::TargetFeature::GuestEvents);
    native.opaque = this;
    native.resolve = ResolveBridge;
    native.atomic_service = AtomicBridge;
    native.active = &faults.active;
    native.anchor = &thread.anchor;
    native.faults = &faults;
    faults.native = &native;
    native.pinned_uniform_slots = backend.pinned_uniform_slots;
    native.fp_environment = 1;
    native.native_nzcv = backend.capabilities.native_nzcv;
    faults.lookup_native = LookupNative;
    faults.host_failure = &thread.bridge_failure;
    faults.lookup_opaque = this;
    faults.recover_code_write = RecoverCodeWrite;
    faults.code_write_opaque = this;
    faults.patch_ordered = backend.patch_fault;
    faults.recover_native = backend.recover_native;
    faults.guest_fp_status = backend.floating_status ? &native.guest_fpsr : nullptr;
    faults.pinned_uniform_slots = backend.pinned_uniform_slots;
    faults.native_nzcv = backend.capabilities.native_nzcv;
    ConfigureGuest(GuestISA::X86_64);
    RefreshNative(native);
}

VM::~VM() { (void)Close(); }

void VM::ObserveFinal() noexcept {
    if (final_observed) return;
    final_observed = true;
    const auto& faults = thread.faults;
    stats.fault_service_ns += faults.service_ns;
    stats.compile_request_ns += faults.service_ns;
    stats.unaligned_recoveries = faults.unaligned_recoveries.load(std::memory_order_relaxed);
    stats.unaligned_patches = faults.unaligned_patches.load(std::memory_order_relaxed);
    stats.smc_write_faults = faults.smc_write_faults.load(std::memory_order_relaxed);
    stats.dispatch_interrupts = faults.dispatch_interrupts.load(std::memory_order_relaxed);
    stats.dispatches = thread.native.dispatcher_entries;
    stats.cache_hits = thread.native.cache_hits;
    stats.directory_probe_steps = thread.native.probe_steps;
    stats.thread_l1_bytes = thread.l1.Data() ? DispatchCache::Bytes : 0;
    stats.dispatch_interrupt_bytes = interrupt_l1.Data() ? DispatchCache::Bytes : 0;
    for (const auto& cache : caches) {
        stats.patched_links += cache.Patched();
        stats.pending_links += cache.Pending();
        stats.pending_targets += cache.PendingTargets();
        stats.link_relocation_rows += cache.LinkRows();
        stats.link_relocation_capacity += cache.LinkCapacity();
    }
}

void VM::EnsureThreadDispatch() {
    if (!backend.thread_dispatch_cache || thread.l1.Data()) return;
    thread.l1.Ensure();
    interrupt_l1.Ensure();
    thread.faults.dispatch_interrupt_bytes = DispatchCache::Bytes;
    thread.faults.dispatch_interrupt_base.store(
            reinterpret_cast<std::uintptr_t>(interrupt_l1.Data()), std::memory_order_release);
    stats.thread_dispatch_cache = true;
    RefreshNative(thread.native);
}

MappingRelease VM::Close() noexcept {
    if (closed) return close_result;
    if (thread.faults.registered || thread.faults.active.load()) return {1, EBUSY, 0};
    closing = true;
    FlushDisk();
    ObserveFinal();
    MappingRelease native;
    {
        RuntimeTimer total{stats.compile_request_ns}, release{stats.code_release_ns};
        // No executor or signal reader remains, including after partial entry
        // registration. All directory routes go away before any unit is freed.
        // Rewriting links in ranges retired together would add a useless L scan.
        for (auto& cache : caches) cache.Release();
        thread.anchor = {};
        RefreshNative(thread.native);
        thread.native.l1.store(nullptr, std::memory_order_relaxed);
        thread.faults.dispatch_interrupt_base.store(0, std::memory_order_relaxed);
        thread.faults.dispatch_interrupt_bytes = 0;
        thread.faults.dispatch_resume_l1.store(0, std::memory_order_relaxed);
        native = Merge(thread.l1.Release(), empty_l1.Release());
        native = Merge(native, interrupt_l1.Release());
        compiler.Release();
        native = Merge(native, heap.Release());
    }
    // Guest image/stack teardown belongs to run lifetime, as before. Native
    // cache/buffer release above remains in the complete compile window.
    close_result = Merge(native, memory.Release());
    closed = !close_result.remaining_views;
    return close_result;
}

int VM::Run(const std::string& path,
            std::span<const std::string> argv,
            std::span<const std::string> environment) {
    PrepareGuest(path, argv, environment);
    return RunPrepared();
}

void VM::PrepareGuest(const std::string& path,
                      std::span<const std::string> argv,
                      std::span<const std::string> environment) {
    if (!options.cache_directory.empty() || offline_output) {
        std::vector<std::uint8_t> bytes;
        try {
            RuntimeTimer total{stats.compile_request_ns}, load{stats.cache_load_ns};
            bytes = platform::ReadArtifactFile(path);
        } catch (const std::exception&) {
            if (offline_output) throw;
            ++stats.disk_cache_rejects;
            DisablePersistence();
        }
        if (!persistence_disabled) {
            PrepareGuest(bytes, path, argv, environment);
            return;
        }
    }
    if (closing || closed || prepared)
        swift::ThrowError("VM cannot load another guest image", ErrorKind::InvalidState);
    image = platform::LoadELF(memory, path);
    PrepareLoaded(argv, environment, nullptr, nullptr);
}

void VM::PrepareGuest(std::span<const std::uint8_t> bytes,
                      const std::string& display_path,
                      std::span<const std::string> argv,
                      std::span<const std::string> environment,
                      const CodeImage* native) {
    if (closing || closed || prepared)
        swift::ThrowError("VM cannot load another guest image", ErrorKind::InvalidState);
    image = platform::LoadELF(memory, bytes, display_path);
    Digest identity;
    {
        RuntimeTimer total{stats.compile_request_ns}, load{stats.cache_load_ns};
        identity = swift::HashBytes(bytes);
    }
    PrepareLoaded(argv, environment, &identity, native);
}

void VM::PrepareLoaded(std::span<const std::string> argv,
                       std::span<const std::string> environment,
                       const Digest* guest_identity,
                       const CodeImage* native) {
    ConfigureGuest(image.isa);
    PC() = image.isa == GuestISA::Arm32
                   ? arm32::Location::Make(image.entry & ~1u, image.entry & 1u).Key()
                   : image.entry;
    SP() = platform::InitialStack(
            memory,
            image,
            argv,
            environment,
            ISA() == GuestISA::Arm32 ? platform::abi32::HardwareCapabilities(options.arm32_profile)
                                     : 0);
    if (ISA() == GuestISA::Arm32) platform::InstallArm32UserHelpers(memory);
    UpdateFPEnvironment(true);
    if (guest_identity) {
        RuntimeTimer total{stats.compile_request_ns}, load{stats.cache_load_ns};
        image_key = MakeCodeImageKey(*guest_identity, image.load_bias, backend, options, ISA());
    }
    if (offline_output || native) {
        if (image.elf_type != ET_EXEC)
            swift::ThrowError("AOT supports static ET_EXEC guest images", ErrorKind::Unsupported);
        if (!backend.compile || !backend.relocate)
            swift::ThrowError("AOT requires an available native backend", ErrorKind::Unsupported);
    }
    if (!options.cache_directory.empty() && image.elf_type == ET_EXEC && backend.compile &&
        backend.relocate && !offline_output && !native && !persistence_disabled && guest_identity) {
        RuntimeTimer total{stats.compile_request_ns}, load{stats.cache_load_ns};
        try {
            disk_cache = std::make_unique<DiskCache>(options.cache_directory, image_key);
            (void)disk_cache->Load();
        } catch (const std::exception&) {
            ++stats.disk_cache_rejects;
            disk_cache.reset();
        }
    }
    if (native) {
        FaultScope registration{thread.faults, uniform_data, PC()};
        RuntimeTimer total{stats.compile_request_ns}, load{stats.cache_load_ns};
        InstallImage(*native);
    }
    prepared = true;
}

int VM::RunPrepared() {
    if (!prepared || closing || closed)
        swift::ThrowError("guest image is not available for execution", ErrorKind::InvalidState);
    host_returned = false;
    UpdateFPEnvironment(true);
    Execute();
    FlushDisk();
    return thread.exit_code;
}

void* VM::HostPointer(std::uint64_t address, std::size_t size, bool write) {
    if (!memory.Accessible(address, size, PROT_READ | (write ? PROT_WRITE : 0)))
        swift::ThrowError("guest call buffer is not accessible", ErrorKind::GuestFault);
    if (write) {
        ClearReservation();
        if (memory.WriteAffectsCode(address, size)) Invalidate();
    }
    return reinterpret_cast<void*>(address);
}

void VM::CallGuest(std::uint64_t pc, std::uint64_t stack_pointer, std::uint64_t stop_pc) {
    if (!prepared || closing || closed || host_call_active || thread.exited)
        swift::ThrowError("guest is not available for a host call", ErrorKind::InvalidState);
    if (ISA() == GuestISA::Arm32) {
        const auto key = [&](std::uint64_t value) {
            return value >> 32
                           ? value
                           : arm32::Location::Make(value & ~1u, value & 1u, 0, Arm32State().fpscr)
                                     .Key();
        };
        pc = key(pc);
        stop_pc = key(stop_pc);
        stack_pointer = std::uint32_t(stack_pointer);
    }
    if (!memory.CodeAt(CodeAddress(ISA(), stop_pc)).empty())
        swift::ThrowError("guest call return sentinel must not be executable",
                          ErrorKind::InvalidArgument);
    PC() = pc;
    SP() = stack_pointer;
    ExecuteHostCall(stop_pc);
}

void VM::RunUntilGuestStop(std::uint64_t stop_pc) {
    if (!prepared || closing || closed || host_call_active || thread.exited)
        swift::ThrowError("guest is not available for startup", ErrorKind::InvalidState);
    const auto source = memory.CodeAt(stop_pc);
    if (source.empty() || source.front() != 0xf4 ||
        std::any_of(caches.begin(), caches.end(), [stop_pc](const CodeCache& cache) {
            return bool(cache.Find(stop_pc));
        }))
        swift::ThrowError("guest startup requires an uncached explicit HLT stop",
                          ErrorKind::InvalidArgument);
    ExecuteHostCall(stop_pc);
}

void VM::ExecuteHostCall(std::uint64_t stop_pc) {
    host_return_pc = stop_pc;
    host_returned = false;
    host_call_active = true;
    try {
        Execute();
        host_call_active = false;
        if (!host_returned)
            swift::ThrowError("guest exited before returning to its host caller",
                              ErrorKind::GuestFault);
        FlushDisk();
    } catch (...) {
        host_call_active = false;
        throw;
    }
}

void VM::Execute() {
    UpdateFPEnvironment();
    {
        FaultScope registration{thread.faults, uniform_data, PC()};
        if (options.mode == ExecutionMode::InterpretOnly ||
            (options.mode == ExecutionMode::Mixed && !backend.compile)) {
            while (!thread.exited && !host_returned) {
                DeliverPending();
                if (thread.exited) break;
                if (IsHostReturn(PC())) {
                    host_returned = true;
                    break;
                }
                Interpret(true);
            }
        } else {
            if (!backend.compile)
                swift::ThrowError("native backend is unavailable on this host",
                                  ErrorKind::Unsupported);
            stats.rcpc_loads = backend.capabilities.rcpc_loads;
            stats.rcpc_imm9 = backend.capabilities.rcpc_imm9;
            stats.fault_locations = true;
            stats.recover_unaligned = backend.patch_fault != nullptr;
            stats.private_uniforms = true;
            stats.pinned_uniform_slots = backend.pinned_uniform_slots;
            stats.native_nzcv = backend.capabilities.native_nzcv;
            stats.thread_dispatch_cache = backend.thread_dispatch_cache;
            while (!thread.exited && !host_returned) {
                DeliverPending();
                if (thread.exited) break;
#if defined(SVM_TEST_BOUNDARY)
                // The direct numerical policy can return without entering the
                // resolver. Stop there before it fetches the test guard page.
                if (test_boundary_active && IsHostReturn(PC())) {
                    host_returned = true;
                    break;
                }
#endif
                // Unmasked SSE exceptions use the common direct numerical consumer.
                // Its instruction boundary aggregates every lane before committing.
                if (ISA() == GuestISA::X86_64 && (NumericControl() & 0x1f80) != 0x1f80) {
                    Interpret(true);
                    continue;
                }
                const auto status = SvmRunNative(&thread.native);
                const auto reason = static_cast<NativeExit>(status & 255);
#if defined(SVM_TEST_BOUNDARY)
                if (test_boundary_active) test_boundary_native_status = status;
#endif
                thread.native.phase.store(SVM_NATIVE_PHASE_RUNTIME, std::memory_order_release);
                if (reason != NativeExit::FPEnvironment) MergeFPStatus();
                thread.faults.active.store(nullptr);
                if (thread.bridge_failure) throw swift::RuntimeError(thread.bridge_failure);
                if (reason == NativeExit::Syscall) {
#if defined(SVM_TEST_BOUNDARY)
                    if (test_boundary_active) {
                        test_boundary_service = status;
                        host_returned = true;
                        break;
                    }
#endif
                    syscalls.Service();
                } else if (reason == NativeExit::GuestException)
                    DeliverException(thread.faults.exception);
                else if (reason == NativeExit::GuestEvents)
                    DeliverPending();
                else if (reason == NativeExit::SemanticException)
                    RaiseEncodedException(status);
                else if (reason == NativeExit::ResolveFailure) {
                    if (host_returned) break;
                    if ((ISA() == GuestISA::Arm64 && (PC() & 3)) ||
                        (ISA() == GuestISA::Arm32 &&
                         (PC() & (arm32::Location{PC()}.Thumb() ? 1u : 3u)))) {
                        RaiseSemanticException(NativeExit::AlignmentFault, PC());
                    } else if (thread.interpret_requested) {
                        thread.interpret_requested = false;
                        Interpret(true);
                    } else if (compiler.Diagnostic().stage == CompileStage::Frontend &&
                               compiler.Diagnostic().frontend_error ==
                                       frontend::BuildError::InvalidEncoding) {
                        PC() = compiler.Diagnostic().pc;
                        RaiseEncodedException(
                                SemanticExceptionExit(GuestExceptionKind::IllegalInstruction));
                    } else if (compiler.Diagnostic().stage == CompileStage::Source &&
                               compiler.Failure() == CompileFailure::InvalidInput) {
                        GuestExceptionInfo event;
                        event.isa = ISA();
                        event.origin = GuestExceptionOrigin::Semantic;
                        event.instruction_pc = event.address = CodeAddress(ISA(), PC());
                        event.address_valid = true;
                        event.kind = GuestExceptionKind::Memory;
                        event.access = GuestExceptionAccess::Execute;
                        event.signal_number = SIGSEGV;
                        DeliverException(event);
                    } else if (compiler.Diagnostic().stage == CompileStage::Frontend &&
                               compiler.Diagnostic().frontend_error ==
                                       frontend::BuildError::Truncated) {
                        GuestExceptionInfo event;
                        event.isa = ISA();
                        event.origin = GuestExceptionOrigin::Semantic;
                        event.instruction_pc = CodeAddress(ISA(), PC());
                        event.address =
                                event.instruction_pc + memory.CodeAt(event.instruction_pc).size();
                        event.address_valid = true;
                        event.kind = GuestExceptionKind::Memory;
                        event.access = GuestExceptionAccess::Execute;
                        event.signal_number = SIGSEGV;
                        if (ISA() == GuestISA::X86_64) event.exception_vector = 14;
                        DeliverException(event);
                    } else
                        swift::ThrowError(compiler.DescribeFailure(),
                                          compiler.Failure() == CompileFailure::Unsupported
                                                  ? ErrorKind::Unsupported
                                                  : ErrorKind::InvalidState);
                } else if (reason == NativeExit::FPEnvironment)
                    UpdateFPEnvironment(true);
                else if (reason == NativeExit::StateService) {
                    if (ISA() == GuestISA::Arm64 || ISA() == GuestISA::Arm32)
                        ServiceArmState();
                    else
                        Interpret();
                } else if (reason == NativeExit::X87Fault)
                    RaiseSemanticException(reason, PC());
                else if (reason == NativeExit::UnsupportedAtomic)
                    swift::ThrowError("native atomic access is unsupported",
                                      ErrorKind::Unsupported);
                else if (reason == NativeExit::DivideFault ||
                         reason == NativeExit::AlignmentFault ||
                         reason == NativeExit::MemoryFault || reason == NativeExit::FloatingFault)
                    RaiseSemanticException(reason, PC());
                else if (reason != NativeExit::Continue)
                    swift::ThrowError("unknown native dispatch exit reason",
                                      ErrorKind::InvalidState);
            }
        }
    }
    if (thread.faults.restore_error)
        swift::ThrowSystemError("restore native fault handlers or signal mask",
                                thread.faults.restore_error);
}

void VM::MergeFPStatus() noexcept {
    if (ISA() == GuestISA::X86_64 && backend.floating_status)
        NumericControl() |= backend.floating_status(thread.native);
}

void VM::UpdateFPEnvironment(bool architectural_write) {
    if (architectural_write) thread.native.guest_fpsr = 0;
    if (ISA() == GuestISA::Arm64) {
        const auto fpcr = thread.native.arm_state.fpcr;
        // Numeric leaves run only under the supported architectural profile.
        // Check at the existing FPCR service boundary, not at every FP operation.
        if (!arm64::IsSupportedFPCR(fpcr))
            swift::ThrowError("guest FPCR contains unsupported controls", ErrorKind::Unsupported);
        if (architectural_write) {
            constexpr unsigned rounding[]{0, 2, 1, 3};
            NumericControl() = 0x1f80u | (rounding[(fpcr >> 22) & 3] << 13) |
                               ((fpcr & (1u << 24)) ? 0x8040u : 0u);
        }
    }
    if (ISA() == GuestISA::Arm32 && architectural_write) {
        auto& state = thread.native.arm32_state;
        if (!arm32::IsSupportedFPSCR(state.fpscr))
            swift::ThrowError("ARM32 FPSCR exception enables are unsupported",
                              ErrorKind::Unsupported);
        state.RekeyFPSCR();
        constexpr unsigned rounding[]{0, 2, 1, 3};
        NumericControl() = 0x1f80u | (rounding[(state.fpscr >> 22) & 3] << 13) |
                           ((state.fpscr & (1u << 24)) ? 0x8040u : 0u);
    }
    const auto mxcsr = NumericControl();
    if (mxcsr & ~u64{0xffff})
        swift::ThrowError("guest MXCSR contains reserved bits", ErrorKind::GuestFault);
    const auto daz = (mxcsr >> 6) & 1, ftz = (mxcsr >> 15) & 1;
    const auto mode =
            backend.specializes_fp_flush ? static_cast<std::uint8_t>(daz | (ftz << 1)) : 0;
    if (mode != thread.fp_mode) {
        RuntimeTimer total{stats.compile_request_ns};
        thread.l1.Reset();
        thread.fp_mode = mode;
    }
    if (backend.update_fp) backend.update_fp(backend.context, thread.native, mxcsr);
    RefreshNative(thread.native);
}

void VM::Invalidate() { smc.Invalidate(); }

bool VM::PrepareGuestWrite(u64 address, std::size_t size) {
    ClearReservation();
    if (!memory.Accessible(address, size, PROT_WRITE)) return false;
    if (size && memory.WriteAffectsCode(address, size)) Invalidate();
    return true;
}

bool VM::GuestWrite(u64 address, const void* source, std::size_t size) {
    return PrepareGuestWrite(address, size) && memory.CopyOut(address, source, size);
}

std::uint64_t VM::AtomicBridge(void* opaque, AtomicCall* call) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    const auto status = ExecuteAtomic(vm.memory, vm.smc, *call);
    if (status != AtomicStatus::Unsupported) vm.ClearReservation();
    if (status == AtomicStatus::Fault) {
        RecordMemoryFault(vm.thread.faults,
                          call->fault_address,
                          GuestExceptionAccess::Atomic,
                          call->request.bits / 8);
    }
    return static_cast<std::uint64_t>(status);
}

void VM::FileChanged(int fd) {
    ClearReservation();
    if (memory.RefreshFileMappings(fd)) {
        ++stats.smc_file_write_invalidations;
        Invalidate();
    }
}

front::DirectServices VM::DirectServices() noexcept {
    front::DirectServices services{this, Fetch, Read, Write, Atomic, Cpu};
    services.state_image = StateImage;
    services.guest_events = [](void* raw) noexcept {
        return static_cast<VM*>(raw)->signals.Pending();
    };
    return services;
}

front::CodeReadResult VM::Fetch(void* opaque,
                                u64 pc,
                                std::span<std::uint8_t> destination) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    std::size_t copied{};
    const auto failure = [&](u64 address) {
        RecordMemoryFault(vm.thread.faults, address, GuestExceptionAccess::Execute);
        return front::CodeReadResult{{front::AccessStatus::Fault, address}, copied};
    };
    while (copied < destination.size() && pc <= UINT64_MAX - copied) {
        const auto mode = vm.memory.ClassifySource(pc + copied);
        if (!mode) return failure(pc + copied);
        if (*mode == SourceMode::Volatile)
            return {{front::AccessStatus::UnsupportedBeforeAccess, pc + copied}, copied};
        const auto source = vm.memory.CodeAt(pc + copied);
        if (source.empty()) break;
        const auto count = std::min(source.size(), destination.size() - copied);
        errno = 0;
        if (!vm.memory.CopyIn(destination.data() + copied, pc + copied, count)) {
            if (errno && errno != EFAULT)
                vm.thread.bridge_failure.Set(
                        ErrorKind::System, "read guest instruction bytes", errno);
            break;
        }
        copied += count;
    }
    if (copied != destination.size()) return failure(pc + copied);
    return {{front::AccessStatus::Completed, pc + copied}, copied};
}

front::AccessResult VM::Read(void* opaque,
                             u64 address,
                             std::span<std::uint8_t> bytes,
                             bool ordered) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    errno = 0;
    if (!vm.memory.CopyIn(bytes.data(), address, bytes.size())) {
        if (errno && errno != EFAULT)
            vm.thread.bridge_failure.Set(ErrorKind::System, "read guest bytes", errno);
        RecordMemoryFault(vm.thread.faults, address, GuestExceptionAccess::Read, bytes.size());
        return {front::AccessStatus::Fault, address};
    }
    if (ordered) std::atomic_thread_fence(std::memory_order_seq_cst);
    return {};
}

front::AccessResult VM::Write(void* opaque,
                              u64 address,
                              std::span<const std::uint8_t> bytes,
                              bool ordered) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    try {
        const bool changed = vm.memory.WriteAffectsCode(address, bytes.size());
        if (ordered) std::atomic_thread_fence(std::memory_order_seq_cst);
        errno = 0;
        if (!vm.GuestWrite(address, bytes.data(), bytes.size())) {
            if (errno && errno != EFAULT)
                vm.thread.bridge_failure.Set(ErrorKind::System, "write guest bytes", errno);
            RecordMemoryFault(vm.thread.faults, address, GuestExceptionAccess::Write, bytes.size());
            return {front::AccessStatus::Fault, address};
        }
        if (ordered) std::atomic_thread_fence(std::memory_order_seq_cst);
        return {front::AccessStatus::Completed, 0, changed};
    } catch (const std::exception& failure) {
        vm.thread.bridge_failure.Capture(failure);
        RecordMemoryFault(vm.thread.faults, address, GuestExceptionAccess::Write, bytes.size());
        return {front::AccessStatus::Fault, address};
    } catch (...) {
        vm.thread.bridge_failure.Set(ErrorKind::Runtime, "unknown guest write service failure");
        RecordMemoryFault(vm.thread.faults, address, GuestExceptionAccess::Write, bytes.size());
        return {front::AccessStatus::Fault, address};
    }
}

front::AccessResult VM::Atomic(void* opaque,
                               const front::AtomicRequest& request,
                               front::AtomicValue& observed) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    const bool changed = vm.memory.WriteAffectsCode(request.address, request.bits / 8);
    AtomicCall call{request};
    const auto status = static_cast<AtomicStatus>(AtomicBridge(opaque, &call));
    observed = call.observed;
    switch (status) {
        case AtomicStatus::Completed:
            return {front::AccessStatus::Completed, 0, changed};
        case AtomicStatus::Fault:
            return {front::AccessStatus::Fault, call.fault_address};
        case AtomicStatus::Unsupported:
            return {front::AccessStatus::UnsupportedBeforeAccess};
    }
    __builtin_unreachable();
}

front::CpuResult VM::Cpu(void*, std::uint32_t leaf, std::uint32_t subleaf) noexcept {
    const auto result = x86::QueryGuestCpu(leaf, subleaf);
    return {result.lower, result.upper};
}

void VM::Interpret(bool block) {
    thread.bridge_failure.Clear();
    const front::ReadyEntryLookup ready{
            this, [](void* raw, std::uint64_t pc) noexcept {
                const auto& owner = *static_cast<VM*>(raw);
                return owner.IsHostReturn(pc) ||
                       bool(static_cast<VM*>(raw)->Cache().Find(pc));
            }};
    frontend::StepResult step;
    if (ISA() == GuestISA::Arm64) {
        ::swift::arm64::DirectServices services{this, Fetch, Read, Write, Atomic};
        services.guest_events = [](void* raw) noexcept {
            return static_cast<VM*>(raw)->signals.Pending();
        };
        step = block ? ::swift::arm64::InterpretBlock(
                               thread.native.arm_state, services, 256, nullptr, ready)
                     : ::swift::arm64::InterpretOne(thread.native.arm_state, services);
    } else if (ISA() == GuestISA::Arm32) {
        ::swift::arm32::DirectServices services{this, Fetch, Read, Write, Atomic};
        services.guest_events = [](void* raw) noexcept {
            return static_cast<VM*>(raw)->signals.Pending();
        };
        step = block ? ::swift::arm32::InterpretBlock(thread.native.arm32_state,
                                                      services,
                                                      256,
                                                      nullptr,
                                                      ready,
                                                      options.arm32_profile)
                     : ::swift::arm32::InterpretOne(
                               thread.native.arm32_state, services, nullptr, options.arm32_profile);
    } else {
        step = block ? front::InterpretBlock(
                               thread.native.state, DirectServices(), 256, nullptr, ready)
                     : front::InterpretOne(thread.native.state, DirectServices());
    }
    stats.interpreted_steps += step.instructions;
    if (thread.bridge_failure) throw swift::RuntimeError(thread.bridge_failure);
    switch (step.kind) {
        case front::StepKind::Incomplete:
            swift::ThrowError("IR direct service unavailable after execution progress");
        case front::StepKind::Completed:
            return;
        case front::StepKind::Service:
            if ((step.service & 255) == ExitValue(NativeExit::SemanticException))
                RaiseEncodedException(step.service);
            else if (step.service == ExitValue(NativeExit::Syscall)) {
#if defined(SVM_TEST_BOUNDARY)
                if (test_boundary_active) {
                    test_boundary_service = step.service;
                    host_returned = true;
                    return;
                }
#endif
                syscalls.Service();
            } else if (step.service == ExitValue(NativeExit::FPEnvironment))
                UpdateFPEnvironment(true);
            else if (step.service == ExitValue(NativeExit::StateService) &&
                     (ISA() == GuestISA::Arm64 || ISA() == GuestISA::Arm32)) {
                ServiceArmState();
            } else if (step.service == ExitValue(NativeExit::GuestEvents))
                DeliverPending();
            else if (step.service == ExitValue(NativeExit::FloatingFault) ||
                     step.service == ExitValue(NativeExit::X87Fault) ||
                     step.service == ExitValue(NativeExit::DivideFault) ||
                     step.service == ExitValue(NativeExit::AlignmentFault))
                RaiseSemanticException(static_cast<NativeExit>(step.service), PC());
            else
                swift::ThrowError("unknown interpreted service event", ErrorKind::InvalidState);
            return;
        default:
            break;
    }
    if (thread.bridge_failure) throw swift::RuntimeError(thread.bridge_failure);
    if (step.kind == front::StepKind::Fault) {
        thread.faults.address.store(step.fault_address, std::memory_order_relaxed);
        RaiseSemanticException(
                step.service ? static_cast<NativeExit>(step.service) : NativeExit::MemoryFault,
                step.pc);
        return;
    }
    if (step.kind == front::StepKind::InvalidEncoding) {
        PC() = step.pc;
        RaiseEncodedException(SemanticExceptionExit(GuestExceptionKind::IllegalInstruction));
        return;
    }
    swift::ThrowError(
            swift::Format("{} direct execution rejected pc={:#x} kind={} address={:#x}",
                          ISA() == GuestISA::Arm64   ? "AArch64"
                          : ISA() == GuestISA::Arm32 ? "AArch32"
                                                     : "x86",
                          step.pc,
                          static_cast<unsigned>(step.kind),
                          step.fault_address),
            step.kind == front::StepKind::Fault ? ErrorKind::GuestFault : ErrorKind::Unsupported);
}
}  // namespace swift::runtime::execution
