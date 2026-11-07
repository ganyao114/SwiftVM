#include "vm.h"
#include "timing.h"
#include "disk_cache.h"
#include "runtime/platform/linux/artifact_file.h"

#include <atomic>
#include <bit>
#include <algorithm>
#include <cerrno>
#include <elf.h>
#include "base/error.h"
#include "base/format.h"
#include <stdexcept>
#include <sys/mman.h>

#include "runtime/arch/x86/cpu.h"
#include "runtime/arch/arm64/numeric.h"

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

template <typename T> std::uint64_t AtomicModify(const front::AtomicRequest& request) noexcept {
    auto* pointer = reinterpret_cast<T*>(request.address);
    auto old = __atomic_load_n(pointer, __ATOMIC_SEQ_CST);
    for (;;) {
        T value{};
        const auto input = static_cast<T>(request.desired.low);
        switch (request.operation) {
            case front::AtomicOperation::CompareExchange:
                value = old == static_cast<T>(request.expected.low) ? input : old;
                break;
            case front::AtomicOperation::Exchange:
                value = input;
                break;
            case front::AtomicOperation::FetchAdd:
            case front::AtomicOperation::Add:
                value = static_cast<T>(old + input);
                break;
            case front::AtomicOperation::Sub:
                value = static_cast<T>(old - input);
                break;
            case front::AtomicOperation::And:
                value = static_cast<T>(old & input);
                break;
            case front::AtomicOperation::Or:
                value = static_cast<T>(old | input);
                break;
            case front::AtomicOperation::Xor:
                value = static_cast<T>(old ^ input);
                break;
            case front::AtomicOperation::Neg:
                value = static_cast<T>(T{0} - old);
                break;
            case front::AtomicOperation::AddCarry:
                value = static_cast<T>(old + input + request.carry);
                break;
            case front::AtomicOperation::SubBorrow:
                value = static_cast<T>(old - input - request.carry);
                break;
            case front::AtomicOperation::SignedMax:
                value = std::bit_cast<std::make_signed_t<T>>(old) >
                                        std::bit_cast<std::make_signed_t<T>>(input)
                                ? old
                                : input;
                break;
            case front::AtomicOperation::SignedMin:
                value = std::bit_cast<std::make_signed_t<T>>(old) <
                                        std::bit_cast<std::make_signed_t<T>>(input)
                                ? old
                                : input;
                break;
            case front::AtomicOperation::UnsignedMax:
                value = std::max(old, input);
                break;
            case front::AtomicOperation::UnsignedMin:
                value = std::min(old, input);
                break;
            default:
                __builtin_unreachable();  // Rejected before the first access.
        }
        const auto observed = old;
        // Even a failed guest CMPXCHG completes a real write cycle of old.
        if (__atomic_compare_exchange_n(
                    pointer, &old, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return observed;
    }
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
        , smc(*this) {
    auto& native = thread.native;
    auto& faults = thread.faults;
    native.opaque = this;
    native.resolve = ResolveBridge;
    native.prepare_atomic = PrepareAtomic;
    native.active = &faults.active;
    native.anchor = &thread.anchor;
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
    stats.dispatches = thread.native.dispatcher_entries;
    stats.cache_hits = thread.native.cache_hits;
    stats.directory_probe_steps = thread.native.probe_steps;
    for (const auto& cache : caches) {
        stats.patched_links += cache.Patched();
        stats.pending_links += cache.Pending();
        stats.pending_targets += cache.PendingTargets();
        stats.link_relocation_rows += cache.LinkRows();
        stats.link_relocation_capacity += cache.LinkCapacity();
    }
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
        compiler.Release();
        native = heap.Release();
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
    PC() = image.entry;
    SP() = platform::InitialStack(memory, image, argv, environment);
    UpdateFPEnvironment(true);
    if (guest_identity) {
        RuntimeTimer total{stats.compile_request_ns}, load{stats.cache_load_ns};
        image_key = MakeCodeImageKey(*guest_identity, image.load_bias, backend, options);
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
    if (write && memory.WriteAffectsCode(address, size)) Invalidate();
    return reinterpret_cast<void*>(address);
}

void VM::CallGuest(std::uint64_t pc, std::uint64_t stack_pointer, std::uint64_t stop_pc) {
    if (!prepared || closing || closed || host_call_active || thread.exited)
        swift::ThrowError("guest is not available for a host call", ErrorKind::InvalidState);
    if (!memory.CodeAt(stop_pc).empty())
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
    if (options.mode == ExecutionMode::InterpretOnly ||
        (options.mode == ExecutionMode::Mixed && !backend.compile)) {
        while (!thread.exited && !host_returned) {
            if (host_call_active && PC() == host_return_pc) {
                host_returned = true;
                break;
            }
            Interpret(true);
        }
        return;
    }
    if (!backend.compile)
        swift::ThrowError("native backend is unavailable on this host", ErrorKind::Unsupported);
    {
        FaultScope registration{thread.faults, uniform_data, PC()};
        stats.rcpc_loads = backend.capabilities.rcpc_loads;
        stats.rcpc_imm9 = backend.capabilities.rcpc_imm9;
        stats.fault_locations = true;
        stats.recover_unaligned = backend.patch_fault != nullptr;
        stats.private_uniforms = true;
        stats.pinned_uniform_slots = backend.pinned_uniform_slots;
        stats.native_nzcv = backend.capabilities.native_nzcv;
        stats.native_local_cache = backend.reset_indirect != nullptr;
        // All mutable recovery state is in GuestThread, before this jump point.
        // Native and its cold resolver end active before creating C++ owners.
        if (sigsetjmp(thread.faults.jump, 1)) {
            if (backend.restore_host_fp) backend.restore_host_fp(thread.native);
            if (thread.bridge_failure) throw swift::RuntimeError(thread.bridge_failure);
            swift::ThrowError(swift::Format("guest memory/alignment fault signal={} code={} "
                                            "pc={:#x} host_pc={:#x} address={:#x}",
                                            static_cast<int>(thread.faults.signal_number),
                                            static_cast<int>(thread.faults.signal_code),
                                            PC(),
                                            thread.faults.host_pc.load(),
                                            thread.faults.address.load()),
                              ErrorKind::GuestFault);
        }
        while (!thread.exited && !host_returned) {
            const auto reason = static_cast<NativeExit>(SvmRunNative(&thread.native));
            thread.faults.active.store(nullptr);
            if (thread.bridge_failure) throw swift::RuntimeError(thread.bridge_failure);
            if (reason == NativeExit::Syscall)
                syscalls.Service();
            else if (reason == NativeExit::ResolveFailure) {
                if (host_returned) break;
                if (thread.interpret_requested) {
                    thread.interpret_requested = false;
                    Interpret(true);
                } else
                    swift::ThrowError(compiler.DescribeFailure(),
                                      compiler.Failure() == CompileFailure::Unsupported
                                              ? ErrorKind::Unsupported
                                              : ErrorKind::InvalidState);
            } else if (reason == NativeExit::FPEnvironment)
                UpdateFPEnvironment(true);
            else if (reason == NativeExit::StateService)
                Interpret();
            else if (reason == NativeExit::X87Fault)
                swift::ThrowError("x87 unsupported command or unmasked exception");
            else if (reason == NativeExit::UnsupportedAtomic)
                swift::ThrowError("native atomic access is unsupported", ErrorKind::Unsupported);
            else if (reason == NativeExit::DivideFault)
                swift::ThrowError("guest integer division fault", ErrorKind::GuestFault);
            else if (reason == NativeExit::AlignmentFault)
                swift::ThrowError("guest memory alignment fault", ErrorKind::GuestFault);
            else if (reason == NativeExit::MemoryFault)
                swift::ThrowError("guest memory access fault", ErrorKind::GuestFault);
            else if (reason != NativeExit::Continue)
                swift::ThrowError("unknown native dispatch exit reason", ErrorKind::InvalidState);
        }
    }
    if (thread.faults.restore_error)
        swift::ThrowSystemError("restore native fault handlers or signal mask",
                                thread.faults.restore_error);
}

void VM::UpdateFPEnvironment(bool architectural_write) {
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
    const auto mxcsr = NumericControl();
    if (mxcsr & ~u64{0xffff})
        swift::ThrowError("guest MXCSR contains reserved bits", ErrorKind::GuestFault);
    const auto daz = (mxcsr >> 6) & 1, ftz = (mxcsr >> 15) & 1;
    thread.fp_mode = backend.specializes_fp_flush ? static_cast<std::uint8_t>(daz | (ftz << 1)) : 0;
    if (backend.update_fp) backend.update_fp(backend.context, thread.native, mxcsr);
    RefreshNative(thread.native);
}

void VM::Invalidate() { smc.Invalidate(); }

bool VM::PrepareGuestWrite(u64 address, std::size_t size) {
    if (!memory.Accessible(address, size, PROT_WRITE)) return false;
    if (size && memory.WriteAffectsCode(address, size)) Invalidate();
    return true;
}

bool VM::GuestWrite(u64 address, const void* source, std::size_t size) {
    return PrepareGuestWrite(address, size) && memory.CopyOut(address, source, size);
}

std::uint64_t VM::PrepareAtomic(void* opaque, u64 address, std::size_t size) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    if (!vm.memory.Accessible(address, size, PROT_READ | PROT_WRITE))
        return AtomicAccessValue(AtomicAccess::Fault);
    const auto end = address + size;
    for (const auto& region : vm.memory.Regions()) {
        if (region.address >= end) break;
        if (region.address + region.size > address && region.backing != BackingKind::Anonymous)
            return AtomicAccessValue(AtomicAccess::Unsupported);
    }
    // The single guest owns these stable anonymous mappings. Disarm actual
    // SMC watches before libatomic can enter a lock; retain the active unit
    // until its next dispatcher boundary rather than invalidating it here.
    return AtomicAccessValue(vm.smc.PrepareWriteRange(address, size) ? AtomicAccess::Allowed
                                                                     : AtomicAccess::Fault);
}

void VM::FileChanged(int fd) {
    if (memory.RefreshFileMappings(fd)) {
        ++stats.smc_file_write_invalidations;
        Invalidate();
    }
}

front::DirectServices VM::DirectServices() noexcept {
    front::DirectServices services{this, Fetch, Read, Write, Atomic, Cpu};
    services.state_image = StateImage;
    return services;
}

front::CodeReadResult VM::Fetch(void* opaque,
                                u64 pc,
                                std::span<std::uint8_t> destination) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    std::size_t copied{};
    while (copied < destination.size() && pc <= UINT64_MAX - copied) {
        const auto mode = vm.memory.ClassifySource(pc + copied);
        if (!mode) return {{front::AccessStatus::Fault, pc + copied}, copied};
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
    return {{copied == destination.size() ? front::AccessStatus::Completed
                                          : front::AccessStatus::Fault,
             pc + copied},
            copied};
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
            return {front::AccessStatus::Fault, address};
        }
        if (ordered) std::atomic_thread_fence(std::memory_order_seq_cst);
        return {front::AccessStatus::Completed, 0, changed};
    } catch (const std::exception& failure) {
        vm.thread.bridge_failure.Capture(failure);
        return {front::AccessStatus::Fault, address};
    } catch (...) {
        vm.thread.bridge_failure.Set(ErrorKind::Runtime, "unknown guest write service failure");
        return {front::AccessStatus::Fault, address};
    }
}

front::AccessResult VM::Atomic(void* opaque,
                               const front::AtomicRequest& request,
                               front::AtomicValue& observed) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    const bool wide =
            request.bits == 128 && request.operation == front::AtomicOperation::CompareExchange128;
    if ((!wide && request.bits != 8 && request.bits != 16 && request.bits != 32 &&
         request.bits != 64) ||
        (!wide && (request.operation == front::AtomicOperation::CompareExchange128 ||
                   request.operation > front::AtomicOperation::UnsignedMin)) ||
        (request.address & (request.bits / 8 - 1)))
        return {front::AccessStatus::UnsupportedBeforeAccess};
    const auto bytes = request.bits / 8;
    if (!vm.memory.Accessible(request.address, bytes, PROT_READ | PROT_WRITE))
        return {front::AccessStatus::Fault, request.address};
    // Under the current one-thread contract, owned anonymous pages cannot
    // disappear between this logical check and the instruction. File/device
    // atomics need a faultable platform primitive; never emulate them by a
    // read followed by a separate write or pretend a lock makes them atomic.
    for (const auto& region : vm.memory.Regions()) {
        if (region.address >= request.address + bytes) break;
        if (region.address + region.size > request.address &&
            region.backing != BackingKind::Anonymous)
            return {front::AccessStatus::UnsupportedBeforeAccess};
    }
    try {
        const bool changed = vm.memory.WriteAffectsCode(request.address, bytes);
        if (!vm.PrepareGuestWrite(request.address, bytes))
            return {front::AccessStatus::Fault, request.address};
        if (wide) {
            using Wide = unsigned __int128;
            auto* pointer = reinterpret_cast<Wide*>(request.address);
            const auto expected = (Wide{request.expected.high} << 64) | request.expected.low;
            const auto desired = (Wide{request.desired.high} << 64) | request.desired.low;
            auto old = __atomic_load_n(pointer, __ATOMIC_SEQ_CST);
            for (;;) {
                const auto before = old;
                const auto value = old == expected ? desired : old;
                if (__atomic_compare_exchange_n(
                            pointer, &old, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                    observed = {static_cast<u64>(before), static_cast<u64>(before >> 64)};
                    return {front::AccessStatus::Completed, 0, changed};
                }
            }
        }
        switch (request.bits) {
            case 8:
                observed.low = AtomicModify<std::uint8_t>(request);
                break;
            case 16:
                observed.low = AtomicModify<std::uint16_t>(request);
                break;
            case 32:
                observed.low = AtomicModify<std::uint32_t>(request);
                break;
            case 64:
                observed.low = AtomicModify<std::uint64_t>(request);
                break;
        }
        observed.high = 0;
        return {front::AccessStatus::Completed, 0, changed};
    } catch (const std::exception& failure) {
        vm.thread.bridge_failure.Capture(failure);
        return {front::AccessStatus::Fault, request.address};
    } catch (...) {
        vm.thread.bridge_failure.Set(ErrorKind::Runtime, "unknown guest atomic service failure");
        return {front::AccessStatus::Fault, request.address};
    }
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
                return (owner.host_call_active && pc == owner.host_return_pc) ||
                       bool(static_cast<VM*>(raw)->Cache().Find(pc));
            }};
    frontend::StepResult step;
    if (ISA() == GuestISA::Arm64) {
        const ::swift::arm64::DirectServices services{this, Fetch, Read, Write, Atomic};
        step = block ? ::swift::arm64::InterpretBlock(
                               thread.native.arm_state, services, 256, nullptr, ready)
                     : ::swift::arm64::InterpretOne(thread.native.arm_state, services);
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
            if (step.service == ExitValue(NativeExit::Syscall))
                syscalls.Service();
            else if (step.service == ExitValue(NativeExit::FPEnvironment))
                UpdateFPEnvironment(true);
            else if (step.service == ExitValue(NativeExit::X87Fault))
                swift::ThrowError("x87 unsupported command or unmasked exception");
            else
                swift::ThrowError("unknown interpreted service event", ErrorKind::InvalidState);
            return;
        default:
            break;
    }
    if (thread.bridge_failure) throw swift::RuntimeError(thread.bridge_failure);
    swift::ThrowError(
            swift::Format("{} direct execution rejected pc={:#x} kind={} address={:#x}",
                          ISA() == GuestISA::Arm64 ? "AArch64" : "x86",
                          step.pc,
                          static_cast<unsigned>(step.kind),
                          step.fault_address),
            step.kind == front::StepKind::Fault ? ErrorKind::GuestFault : ErrorKind::Unsupported);
}
}  // namespace swift::runtime::execution
