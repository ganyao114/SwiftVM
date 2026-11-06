#include "vm.h"
#include "timing.h"

#include <atomic>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <stdexcept>
#include <sys/mman.h>

#include "runtime/arch/x86/cpu.h"

namespace swift::runtime::execution {
namespace front = ::swift::x86;
namespace {
using u64 = std::uint64_t;
constexpr std::uint8_t pinned_uniform_slots = 16;
MappingRelease Merge(MappingRelease a, MappingRelease b) noexcept {
    a.remaining_views += b.remaining_views;
    if (!a.unmap_error) a.unmap_error = b.unmap_error;
    if (!a.close_error) a.close_error = b.close_error;
    return a;
}
template<typename T>
std::uint64_t AtomicModify(const front::AtomicRequest& request) noexcept {
    auto* pointer = reinterpret_cast<T*>(request.address);
    auto old = __atomic_load_n(pointer, __ATOMIC_SEQ_CST);
    for (;;) {
        T value{};
        const auto input = static_cast<T>(request.desired.low);
        switch (request.operation) {
            case front::AtomicOperation::CompareExchange:
                value = old == static_cast<T>(request.expected.low) ? input : old; break;
            case front::AtomicOperation::Exchange: value = input; break;
            case front::AtomicOperation::FetchAdd:
            case front::AtomicOperation::Add: value = static_cast<T>(old + input); break;
            case front::AtomicOperation::Sub: value = static_cast<T>(old - input); break;
            case front::AtomicOperation::And: value = static_cast<T>(old & input); break;
            case front::AtomicOperation::Or: value = static_cast<T>(old | input); break;
            case front::AtomicOperation::Xor: value = static_cast<T>(old ^ input); break;
            case front::AtomicOperation::Neg: value = static_cast<T>(T{0} - old); break;
            case front::AtomicOperation::AddCarry: value = static_cast<T>(old + input + request.carry); break;
            case front::AtomicOperation::SubBorrow: value = static_cast<T>(old - input - request.carry); break;
            default: __builtin_unreachable(); // Rejected before the first access.
        }
        const auto observed = old;
        // Even a failed guest CMPXCHG completes a real write cycle of old.
        if (__atomic_compare_exchange_n(pointer, &old, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return observed;
    }
}
}
VM::VM(RuntimeOptions options, RuntimeStats& stats, const HostBackendOps& host)
        : options(options), stats(stats), backend(host), heap(backend, memory.PageSize()),
          caches{CodeCache{reinterpret_cast<NativeCode::Function>(options.decode_timing ? &SvmNativePendingCounted : &SvmNativePending), backend},
                 CodeCache{reinterpret_cast<NativeCode::Function>(options.decode_timing ? &SvmNativePendingCounted : &SvmNativePending), backend},
                 CodeCache{reinterpret_cast<NativeCode::Function>(options.decode_timing ? &SvmNativePendingCounted : &SvmNativePending), backend},
                 CodeCache{reinterpret_cast<NativeCode::Function>(options.decode_timing ? &SvmNativePendingCounted : &SvmNativePending), backend}},
          compiler(*this), syscalls(*this), smc(*this) {
    auto& native = thread.native;
    auto& faults = thread.faults;
    native.state = &thread.state; native.opaque = this; native.resolve = ResolveBridge;
    native.active = &faults.active; native.anchor = &thread.anchor;
    native.pinned_uniform_offset = offsetof(x86::GuestState, gprs);
    native.pinned_uniform_slots = pinned_uniform_slots;
    native.pinned_vector_offset = offsetof(x86::GuestState, xmm);
    native.flags_offset = offsetof(x86::GuestState, packed_flags); native.pc_offset = offsetof(x86::GuestState, pc);
    native.fp_environment = 1;
    native.native_nzcv = backend.capabilities.native_nzcv;
    faults.lookup_native = LookupNative; faults.lookup_opaque = this;
    faults.recover_code_write = RecoverCodeWrite; faults.code_write_opaque = this;
    faults.patch_ordered = backend.patch_fault;
    faults.recover_native = backend.recover_native;
    faults.pinned_uniform_offset = offsetof(x86::GuestState, gprs);
    faults.pinned_uniform_slots = pinned_uniform_slots;
    faults.pinned_vector_offset = offsetof(x86::GuestState, xmm);
    faults.flags_offset = offsetof(x86::GuestState, packed_flags);
    faults.native_nzcv = backend.capabilities.native_nzcv;
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
    stats.dispatches = thread.native.dispatcher_entries; stats.cache_hits = thread.native.cache_hits;
    stats.directory_probe_steps = thread.native.probe_steps;
    for (const auto& cache : caches) {
        stats.patched_links += cache.Patched(); stats.pending_links += cache.Pending();
        stats.pending_targets += cache.PendingTargets(); stats.link_relocation_rows += cache.LinkRows();
        stats.link_relocation_capacity += cache.LinkCapacity();
    }
}
MappingRelease VM::Close() noexcept {
    if (closed) return close_result;
    if (thread.faults.registered || thread.faults.active.load()) return {1, EBUSY, 0};
    closing = true;
    ObserveFinal();
    MappingRelease native;
    {
        RuntimeTimer total{stats.compile_request_ns}, release{stats.code_release_ns};
        // No executor or signal reader remains, including after partial entry
        // registration. All directory routes go away before any unit is freed.
        // Rewriting links in ranges retired together would add a useless L scan.
        for (auto& cache : caches) cache.Release();
        thread.anchor = {}; RefreshNative(thread.native);
        compiler.Release();
        native = heap.Release();
    }
    // Guest image/stack teardown belongs to run lifetime, as before. Native
    // cache/buffer release above remains in the complete compile window.
    close_result = Merge(native, memory.Release());
    closed = !close_result.remaining_views;
    return close_result;
}
int VM::Run(const std::string& path, std::span<const std::string> argv, std::span<const std::string> environment) {
    if (closing || closed) throw std::runtime_error("VM has been closed");
    image = platform::LoadELF(memory, path);
    thread.state.pc = image.entry;
    thread.state.gprs[x86::RSP] = platform::InitialStack(memory, image, argv, environment);
    UpdateFPEnvironment();
    if (options.mode == ExecutionMode::InterpretOnly ||
        (options.mode == ExecutionMode::Mixed && !backend.compile)) {
        while (!thread.exited) Interpret();
        return thread.exit_code;
    }
    if (!backend.compile) throw std::runtime_error("native backend is unavailable on this host");
    {
        FaultScope registration{thread.faults, thread.state};
        stats.rcpc_loads = backend.capabilities.rcpc_loads;
        stats.rcpc_imm9 = backend.capabilities.rcpc_imm9;
        stats.fault_locations = stats.recover_unaligned = true;
        stats.private_uniforms = true;
        stats.pinned_uniform_slots = pinned_uniform_slots;
        stats.native_nzcv = backend.capabilities.native_nzcv;
        // All mutable recovery state is in GuestThread, before this jump point.
        // Native and its cold resolver end active before creating C++ owners.
        if (sigsetjmp(thread.faults.jump, 1)) {
            if (backend.restore_host_fp) backend.restore_host_fp(thread.native);
            char message[256];
            std::snprintf(message, sizeof(message),
                    "guest memory/alignment fault signal=%d code=%d pc=0x%llx host_pc=0x%llx address=0x%llx",
                    static_cast<int>(thread.faults.signal_number), static_cast<int>(thread.faults.signal_code),
                    static_cast<unsigned long long>(thread.state.pc),
                    static_cast<unsigned long long>(thread.faults.host_pc),
                    static_cast<unsigned long long>(thread.faults.address));
            throw std::runtime_error(message);
        }
        while (!thread.exited) {
            const auto reason = SvmRunNative(&thread.state, &thread.native);
            thread.faults.active.store(nullptr);
            if (reason == 1) syscalls.Service();
            else if (reason == 2) {
                if (thread.interpret_requested) { thread.interpret_requested = false; Interpret(); }
                else throw std::runtime_error(thread.resolver_error ? thread.resolver_error : thread.diagnostic);
            } else if (reason == 3) UpdateFPEnvironment();
            else if (reason == 5) Interpret();
            else if (reason == 4) throw std::runtime_error("x87 unsupported command or unmasked exception");
            else if (reason != 0) throw std::runtime_error("unknown native dispatch exit reason");
        }
    }
    if (thread.faults.restore_error) throw std::runtime_error("cannot restore previous native fault handlers or signal mask");
    return thread.exit_code;
}
void VM::UpdateFPEnvironment() {
    const auto mxcsr = thread.state.mxcsr;
    if (mxcsr & ~u64{0xffff}) throw std::runtime_error("guest MXCSR contains reserved bits");
    const auto daz = (mxcsr >> 6) & 1, ftz = (mxcsr >> 15) & 1;
    thread.fp_mode = backend.capabilities.afp ? 0 : static_cast<std::uint8_t>(daz | (ftz << 1));
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
void VM::FileChanged(int fd) {
    if (memory.RefreshFileMappings(fd)) { ++stats.smc_file_write_invalidations; Invalidate(); }
}
front::DirectServices VM::DirectServices() noexcept {
    front::DirectServices services{this, Fetch, Read, Write, Atomic, Cpu};
    services.state_image = StateImage; return services;
}
front::CodeReadResult VM::Fetch(void* opaque, u64 pc, std::span<std::uint8_t> destination) noexcept {
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
        if (!vm.memory.CopyIn(destination.data() + copied, pc + copied, count)) break;
        copied += count;
    }
    return {{copied == destination.size() ? front::AccessStatus::Completed : front::AccessStatus::Fault, pc + copied}, copied};
}
front::AccessResult VM::Read(void* opaque, u64 address, std::span<std::uint8_t> bytes, bool ordered) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    if (!vm.memory.CopyIn(bytes.data(), address, bytes.size())) return {front::AccessStatus::Fault, address};
    if (ordered) std::atomic_thread_fence(std::memory_order_seq_cst);
    return {};
}
front::AccessResult VM::Write(void* opaque, u64 address, std::span<const std::uint8_t> bytes, bool ordered) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    try {
        const bool changed = vm.memory.WriteAffectsCode(address, bytes.size());
        if (ordered) std::atomic_thread_fence(std::memory_order_seq_cst);
        if (!vm.GuestWrite(address, bytes.data(), bytes.size())) return {front::AccessStatus::Fault, address};
        if (ordered) std::atomic_thread_fence(std::memory_order_seq_cst);
        return {front::AccessStatus::Completed, 0, changed};
    } catch (...) {
        vm.thread.access_error = "guest write failed while retiring native code";
        return {front::AccessStatus::Fault, address};
    }
}
front::AccessResult VM::Atomic(void* opaque, const front::AtomicRequest& request, front::AtomicValue& observed) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    const bool wide = request.bits == 128 && request.operation == front::AtomicOperation::CompareExchange128;
    if ((!wide && request.bits != 8 && request.bits != 16 && request.bits != 32 && request.bits != 64) ||
        (!wide && request.operation > front::AtomicOperation::SubBorrow) ||
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
        if (region.address + region.size > request.address && region.backing != BackingKind::Anonymous)
            return {front::AccessStatus::UnsupportedBeforeAccess};
    }
    try {
        const bool changed = vm.memory.WriteAffectsCode(request.address, bytes);
        if (!vm.PrepareGuestWrite(request.address, bytes)) return {front::AccessStatus::Fault, request.address};
        if (wide) {
            using Wide = unsigned __int128;
            auto* pointer = reinterpret_cast<Wide*>(request.address);
            const auto expected = (Wide{request.expected.high} << 64) | request.expected.low;
            const auto desired = (Wide{request.desired.high} << 64) | request.desired.low;
            auto old = __atomic_load_n(pointer, __ATOMIC_SEQ_CST);
            for (;;) {
                const auto before = old;
                const auto value = old == expected ? desired : old;
                if (__atomic_compare_exchange_n(pointer, &old, value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                    observed = {static_cast<u64>(before), static_cast<u64>(before >> 64)};
                    return {front::AccessStatus::Completed, 0, changed};
                }
            }
        }
        switch (request.bits) {
            case 8: observed.low = AtomicModify<std::uint8_t>(request); break;
            case 16: observed.low = AtomicModify<std::uint16_t>(request); break;
            case 32: observed.low = AtomicModify<std::uint32_t>(request); break;
            case 64: observed.low = AtomicModify<std::uint64_t>(request); break;
        }
        observed.high = 0;
        return {front::AccessStatus::Completed, 0, changed};
    } catch (...) {
        vm.thread.access_error = "guest atomic write failed while retiring native code";
        return {front::AccessStatus::Fault, request.address};
    }
}
front::CpuResult VM::Cpu(void*, std::uint32_t leaf, std::uint32_t subleaf) noexcept {
    const auto result = x86::QueryGuestCpu(leaf, subleaf);
    return {result.lower, result.upper};
}
void VM::Interpret() {
    thread.access_error = nullptr;
    const auto step = front::InterpretOne(thread.state, DirectServices());
    switch (step.kind) {
        case front::StepKind::Completed: return;
        case front::StepKind::Service:
            if (step.service == 1) syscalls.Service();
            else if (step.service == 3) UpdateFPEnvironment();
            else if (step.service == 4) throw std::runtime_error("x87 unsupported command or unmasked exception");
            else throw std::runtime_error("unknown interpreted service event");
            return;
        default: break;
    }
    if (thread.access_error) throw std::runtime_error(thread.access_error);
    char message[192];
    std::snprintf(message, sizeof(message), "x86 direct execution rejected pc=0x%llx kind=%u address=0x%llx",
            static_cast<unsigned long long>(step.pc), static_cast<unsigned>(step.kind),
            static_cast<unsigned long long>(step.fault_address));
    throw std::runtime_error(message);
}
} // namespace swift::runtime::execution
