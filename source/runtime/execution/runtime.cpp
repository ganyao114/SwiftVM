#include "runtime.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/futex.h>
#include <sched.h>
#include <stdexcept>
#include <sys/auxv.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

#if defined(__aarch64__)
#include <asm/hwcap.h>
#endif

#include "fault.h"
#include "guest_cpu.h"
#include "dispatch.h"
#include "memory.h"
#include "native_code.h"
#include "runtime/backend/arm64/compile.h"
#include "runtime/compile/optimize.h"
#include "runtime/frontend/x86/build_ir.h"

namespace swift::runtime::execution {
namespace {
using u64 = std::uint64_t;
using s64 = std::int64_t;
using Clock = std::chrono::steady_clock;
namespace arm64 = backend::arm64;
constexpr std::uint8_t pinned_uniform_slots = 16;
struct Timer {
    u64& counter; Clock::time_point begin{Clock::now()};
    ~Timer() { counter += static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count()); }
};
s64 Result(s64 result) { return result < 0 ? -errno : result; }
const char* Name(ir::OpCode opcode) {
    switch (opcode) {
#define INST(name, ...) case ir::OpCode::name: return #name;
#include "runtime/ir/opcodes.inc"
#undef INST
        default: return "invalid";
    }
}
struct GuestStat {
    u64 device, inode, links;
    std::uint32_t mode, uid, gid, padding;
    u64 rdevice;
    s64 size, block_size, blocks, atime; u64 atime_nsec;
    s64 mtime; u64 mtime_nsec;
    s64 ctime; u64 ctime_nsec;
    s64 unused[3];
};
static_assert(sizeof(GuestStat) == 144);

// The sole PC directory, borrowed directly by native dispatch. Pending targets
// occupy this same table but never count as compiled entries.
class CodeCache {
public:
    explicit CodeCache(bool counted) noexcept
            : empty{.entry = reinterpret_cast<NativeCode::Function>(counted ? &SvmNativePendingCounted : &SvmNativePending)} {}
    NativeCode Find(u64 pc) const noexcept {
        if (slots.empty()) return {};
        auto at = Index(pc, slots.size() - 1);
        while (slots[at].occupied) {
            if (slots[at].pc == pc) return slots[at].code_id != UINT32_MAX ? codes[slots[at].code_id] : NativeCode{};
            at = (at + 1) & (slots.size() - 1);
        }
        return {};
    }
    const NativeCode* FindHost(std::uintptr_t pc) const noexcept {
        // Exceptional path only; native execution gates this borrow while
        // the resolver mutates/grows the one actual directory.
        for (const auto& code : codes) {
            const auto base = reinterpret_cast<std::uintptr_t>(code.entry);
            if (pc >= base && pc - base < code.bytes) return &code;
        }
        return nullptr;
    }
    void InsertReady(u64 pc, NativeCode code) {
        if (!code.entry) throw std::runtime_error("cannot cache an empty native entry");
        auto& slot = Reserve(pc);
        if (slot.code_id == UINT32_MAX) {
            if (codes.size() >= UINT32_MAX) throw std::length_error("native code metadata capacity overflow");
            const auto capacity = codes.capacity();
            codes.push_back(code);
            if (codes.capacity() != capacity) ++allocations;
            // A failed allocation leaves only an ordinary pending PC slot;
            // publish the numeric borrow and ready entry after it succeeds.
            slot.code_id = static_cast<std::uint32_t>(codes.size() - 1);
            ++entries;
        } else codes[slot.code_id] = code;
        slot.entry = code.entry;
        auto head = slot.pending_head;
        slot.pending_head = UINT32_MAX;
        if (head != UINT32_MAX) --pending_targets;
        while (head != UINT32_MAX) {
            const auto& link = links[head];
            Patch(link, code.entry);
            --pending_count;
            head = link.next;
        }
        // Cold resolver only: no native reader or retained slot reference.
        // Both probe paths remain occupied after the swap; only a pending
        // target moves deeper, with its original waiter head intact.
        const auto home = Index(pc, slots.size() - 1);
        if (&slot != &slots[home] && slots[home].occupied && slots[home].code_id == UINT32_MAX)
            std::swap(slot, slots[home]);
    }
    void AddLink(u64 target, std::uintptr_t* pointer, const std::uintptr_t* executable, std::uint32_t back_words) {
        auto& slot = Reserve(target);
        if (links.size() >= UINT32_MAX) throw std::length_error("native link-relocation capacity overflow");
        if (slot.code_id != UINT32_MAX) {
            links.push_back({pointer, executable, UINT32_MAX, back_words});
            Patch(links.back(), slot.entry); return;
        }
        // Publish membership only after the actual row has been allocated.
        // No scan of any other target's waiters is needed on insertion.
        links.push_back({pointer, executable, slot.pending_head, back_words});
        if (slot.pending_head == UINT32_MAX) ++pending_targets;
        slot.pending_head = static_cast<std::uint32_t>(links.size() - 1);
        ++pending_count;
    }
    void ResetLinks() noexcept {
        // Keep all original code/range metadata alive until the next cold
        // resolver. Undo each actual branch before any target can be retired.
        for (const auto& link : links)
            UnlinkNativeLink(link.writable_pointer, link.executable_pointer, link.back_words);
    }
    void Clear() noexcept {
        for (auto& slot : slots) slot = empty;
        codes.clear();
        links.clear();
        entries = occupied = pending_count = pending_targets = 0;
        // Retain the actual PC, cold-code and relocation buffers for reuse.
    }
    void Release() noexcept {
        std::vector<DispatchSlot>{}.swap(slots);
        std::vector<NativeCode>{}.swap(codes);
        std::vector<LinkRelocation>{}.swap(links);
        entries = occupied = pending_count = pending_targets = 0;
    }
    std::size_t Entries() const noexcept { return entries; }
    std::size_t Occupied() const noexcept { return occupied; }
    std::size_t Slots() const noexcept { return slots.size(); }
    DispatchSlot* Data() noexcept { return slots.empty() ? &empty : slots.data(); }
    DispatchSlot* EmptyData() noexcept { return &empty; }
    u64 Mask() const noexcept { return slots.empty() ? 0 : slots.size() - 1; }
    std::size_t Pending() const noexcept { return pending_count; }
    std::size_t PendingTargets() const noexcept { return pending_targets; }
    std::size_t LinkRows() const noexcept { return links.size(); }
    std::size_t LinkCapacity() const noexcept { return links.capacity(); }
    u64 Patched() const noexcept { return patched; }
    // Successful allocations of the actual PC table and cold-code buffer.
    u64 Allocations() const noexcept { return allocations; }

private:
    DispatchSlot empty;
    struct LinkRelocation {
        std::uintptr_t* writable_pointer;
        const std::uintptr_t* executable_pointer;
        std::uint32_t next;
        std::uint32_t back_words;
    };
    static_assert(sizeof(LinkRelocation) == 24);
    std::vector<DispatchSlot> slots;
    // Borrowed metadata only: no PC key or independent cache membership.
    // Native readers gate mutations; FindHost borrows a row only until the
    // next cold resolver, and Process keeps its original separate anchor.
    std::vector<NativeCode> codes;
    // The only relocation directory: ready links are retained here too. Only
    // unresolved rows participate in a target slot's pending_head chain.
    std::vector<LinkRelocation> links;
    std::size_t entries{}, occupied{}, pending_count{}, pending_targets{};
    u64 allocations{}, patched{};
    static std::size_t Index(u64 pc, std::size_t mask) noexcept {
        return static_cast<std::size_t>(pc ^ (pc >> arm64::abi::DispatchHashShift)) & mask;
    }
    static std::size_t EmptyOrMatching(const std::vector<DispatchSlot>& table, u64 pc) noexcept {
        auto at = Index(pc, table.size() - 1);
        while (table[at].occupied && table[at].pc != pc) at = (at + 1) & (table.size() - 1);
        return at;
    }
    DispatchSlot& Reserve(u64 pc) {
        if (!slots.empty()) {
            const auto at = EmptyOrMatching(slots, pc);
            if (slots[at].occupied) return slots[at];
        }
        // Native lookups dominate cold directory growth. A sparse directory
        // spends one slot array to reduce dependent probes on the warm path.
        if (slots.empty() || occupied == slots.size() / 8) Grow();
        auto& slot = slots[EmptyOrMatching(slots, pc)];
        slot.pc = pc; slot.occupied = true; ++occupied;
        return slot;
    }
    void Patch(const LinkRelocation& link, NativeCode::Function entry) noexcept {
        PatchNativeLink(link.writable_pointer, link.executable_pointer, reinterpret_cast<std::uintptr_t>(entry), link.back_words);
        ++patched;
    }
    void Grow() {
        if (slots.size() > slots.max_size() / 2) throw std::length_error("native PC cache capacity overflow");
        std::vector<DispatchSlot> next(slots.empty() ? 256 : slots.size() * 2, empty);
        for (const auto& slot : slots) if (slot.occupied) {
            const auto at = EmptyOrMatching(next, slot.pc);
            next[at] = slot;
            if (slot.code_id != UINT32_MAX) {
                const auto home = Index(slot.pc, next.size() - 1);
                if (at != home && next[home].occupied && next[home].code_id == UINT32_MAX)
                    std::swap(next[at], next[home]);
            }
        }
        slots.swap(next); ++allocations;
    }
};

class Process {
public:
    Process(RuntimeOptions options, RuntimeStats& stats) : options(options), stats(stats), cache(options.decode_timing) {
        native.state = &state; native.opaque = this; native.resolve = ResolveBridge;
        native.active = &faults.active; native.anchor = &anchor;
        native.pinned_uniform_offset = offsetof(GuestState, gprs);
        native.pinned_uniform_slots = pinned_uniform_slots;
        native.pinned_vector_offset = offsetof(GuestState, xmm);
        native.flags_offset = offsetof(GuestState, packed_flags); native.pc_offset = offsetof(GuestState, pc);
        native.fp_environment = 1;
        faults.lookup_native = LookupNative; faults.lookup_opaque = this;
        faults.recover_code_write = RecoverCodeWrite; faults.code_write_opaque = this;
        faults.pinned_uniform_offset = offsetof(GuestState, gprs);
        faults.pinned_uniform_slots = pinned_uniform_slots;
        faults.pinned_vector_offset = offsetof(GuestState, xmm);
        faults.flags_offset = offsetof(GuestState, packed_flags);
#if defined(__aarch64__)
        Timer capabilities{faults.service_ns};
        const auto hardware_caps = getauxval(AT_HWCAP);
        rcpc_loads = (hardware_caps & HWCAP_LRCPC) != 0;
        rcpc_imm9 = (hardware_caps & HWCAP_ILRCPC) != 0;
        native_nzcv = (hardware_caps & HWCAP_FLAGM) != 0;
        lse_atomics = (hardware_caps & HWCAP_ATOMICS) != 0;
        crc32c = (hardware_caps & HWCAP_CRC32) != 0;
        afp = (getauxval(AT_HWCAP2) & HWCAP2_AFP) != 0;
#endif
        native.native_nzcv = native_nzcv;
        faults.native_nzcv = native_nzcv;
        RefreshNative(native);
    }
    ~Process() {
        stats.fault_service_ns += faults.service_ns;
        stats.compile_request_ns += faults.service_ns;
        stats.unaligned_recoveries = faults.unaligned_recoveries.load(std::memory_order_relaxed);
        stats.unaligned_patches = faults.unaligned_patches.load(std::memory_order_relaxed);
        stats.smc_write_faults = faults.smc_write_faults.load(std::memory_order_relaxed);
        stats.dispatches = native.dispatcher_entries; stats.cache_hits = native.cache_hits;
        stats.directory_probe_steps = native.probe_steps;
        stats.patched_links = cache.Patched(); stats.pending_links = cache.Pending();
        stats.pending_targets = cache.PendingTargets(); stats.link_relocation_rows = cache.LinkRows();
        stats.link_relocation_capacity = cache.LinkCapacity();
        Timer total{stats.compile_request_ns}, release{stats.code_release_ns};
        cache.Release(); arena.Clear();
    }
    int Run(const std::string& path, std::span<const std::string> argv, std::span<const std::string> environment) {
#if !defined(__aarch64__)
        throw std::runtime_error("this runner executes AArch64 native code and requires an AArch64 host");
#endif
        const auto image = memory.LoadELF(path);
        state.pc = image.entry; state.gprs[RSP] = memory.InitialStack(image, argv, environment);
        UpdateFPEnvironment();
        {
            FaultScope registration{faults, state};
            stats.rcpc_loads = rcpc_loads;
            stats.rcpc_imm9 = rcpc_imm9;
            stats.fault_locations = stats.recover_unaligned = true;
            stats.private_uniforms = true;
            stats.pinned_uniform_slots = pinned_uniform_slots;
            stats.native_nzcv = native_nzcv;
            // The guard and image predate this one jump point. Mutable recovery
            // state belongs to Process. The naked native frames have no RAII
            // objects; cold C++ resolution clears active before constructing any.
            if (sigsetjmp(faults.jump, 1)) {
#if defined(__aarch64__)
                // siglongjmp bypasses NativeExit: restore before C++ diagnostics.
                asm volatile("msr fpcr, %0\nmsr fpsr, %1"
                             : : "r"(native.host_fpcr), "r"(native.host_fpsr) : "memory");
#endif
                char message[256];
                std::snprintf(message, sizeof message,
                        "guest memory/alignment fault signal=%d code=%d pc=0x%llx host_pc=0x%llx address=0x%llx",
                        static_cast<int>(faults.signal_number), static_cast<int>(faults.signal_code),
                        static_cast<unsigned long long>(state.pc), static_cast<unsigned long long>(faults.host_pc),
                        static_cast<unsigned long long>(faults.address));
                throw std::runtime_error(message);
            }
            while (!exited) {
                const auto reason = SvmRunNative(&state, &native);
                faults.active.store(nullptr);
                if (reason == 1) ServiceSyscall();
                else if (reason == 2) throw std::runtime_error(resolver_error ? resolver_error : diagnostic);
                else if (reason == 3) UpdateFPEnvironment(); // LDMXCSR takes a real root exit/reentry.
                else if (reason != 0) throw std::runtime_error("unknown native dispatch exit reason");
            }
        }
        if (faults.restore_error) throw std::runtime_error("cannot restore previous native fault handlers or signal mask");
        return exit_code;
    }

private:
    RuntimeOptions options;
    RuntimeStats& stats;
    GuestMemory memory;
    CodeArena arena;
    GuestState state;
    FaultContext faults;
    CodeCache cache;
    NativeCode anchor;
    NativeRunContext native;
    std::string diagnostic;
    const char* resolver_error{};
    bool exited{}, reported_unknown{}, rcpc_loads{}, rcpc_imm9{}, native_nzcv{}, lse_atomics{}, crc32c{}, afp{};
    int exit_code{};
    u64 clear_child_tid{};

    void UpdateFPEnvironment() {
        const u64 mxcsr = state.mxcsr;
        const auto daz = (mxcsr >> 6) & 1, ftz = (mxcsr >> 15) & 1;
        // Build from guest controls, never ambient host DN or trap enables.
        // x86 down/up RC bits cross into ARM RMode. Without AFP, leave FZ
        // disabled: scalar consumers apply the independent raw DAZ/FTZ bits.
        native.guest_fpcr = ((mxcsr >> 13) & 1) << 23 | ((mxcsr >> 14) & 1) << 22;
        if (afp) native.guest_fpcr |= 6 | daz | ftz << 24; // AH | NEP | FIZ | FZ.
    }

    static const NativeCode* LookupNative(void* opaque, std::uintptr_t host_pc) noexcept {
        return static_cast<Process*>(opaque)->cache.FindHost(host_pc);
    }
    static bool RecoverCodeWrite(void* opaque, std::uintptr_t address) noexcept {
        auto& process = *static_cast<Process*>(opaque);
        if (!process.memory.RecoverCodeWrite(address)) return false;
        process.faults.smc_pending = 1;
        __atomic_store_n(&process.native.mask, u64{0}, __ATOMIC_RELAXED);
        __atomic_store_n(&process.native.slots, process.cache.EmptyData(), __ATOMIC_RELEASE);
        process.cache.ResetLinks();
        return true;
    }
    std::uintptr_t Dispatcher() const noexcept {
        const auto dispatcher = native_nzcv
                ? (options.decode_timing ? &SvmNativeDispatchNZCVCounted : &SvmNativeDispatchNZCV)
                : (options.decode_timing ? &SvmNativeDispatchCounted : &SvmNativeDispatch);
        return reinterpret_cast<std::uintptr_t>(dispatcher);
    }
    void RefreshNative(NativeRunContext& context) noexcept {
        context.mask = faults.smc_pending ? 0 : cache.Mask();
        context.slots = faults.smc_pending ? cache.EmptyData() : cache.Data();
        context.anchor = &anchor;
    }
    void ObserveCache() noexcept {
        stats.peak_cache_entries = std::max(stats.peak_cache_entries, static_cast<u64>(cache.Entries()));
        stats.peak_cache_occupied = std::max(stats.peak_cache_occupied, static_cast<u64>(cache.Occupied()));
        stats.peak_cache_slots = std::max(stats.peak_cache_slots, static_cast<u64>(cache.Slots()));
        stats.cache_table_allocations = cache.Allocations();
    }
    static std::uintptr_t ResolveBridge(void* opaque, NativeRunContext* context) noexcept {
        auto& process = *static_cast<Process*>(opaque);
        try { return process.Resolve(*context); }
        catch (const std::exception& error) {
            // Even reporting bad_alloc must not let an exception cross the
            // naked native thunk. The fallback message has static lifetime.
            try { process.diagnostic = error.what(); process.resolver_error = nullptr; }
            catch (...) { process.resolver_error = "native resolver failed while recording its diagnostic"; }
        } catch (...) { process.resolver_error = "unknown native resolver failure"; }
        process.ObserveCache(); process.RefreshNative(*context);
        return 0;
    }
    std::uintptr_t Resolve(NativeRunContext& context) {
        // Native dispatch cleared active before calling us. All prior code,
        // fault ranges and relocation pointers were retained until this exit.
        if (faults.smc_pending) Invalidate();
        auto code = cache.Find(state.pc);
        if (!code.entry) {
            Timer total{stats.compile_request_ns};
            if (options.scope == ir::Scope::Function) {
                code = CompileUnit(ir::Scope::Function);
                if (!code.entry) { ++stats.fallbacks; code = CompileUnit(ir::Scope::Block); }
            } else code = CompileUnit(ir::Scope::Block);
            if (!code.entry) { ObserveCache(); RefreshNative(context); return 0; }
            { Timer publish{stats.publication_ns}; cache.InsertReady(state.pc, code); }
            ObserveCache();
        }
        anchor = code;
        resolver_error = nullptr;
        RefreshNative(context);
        return reinterpret_cast<std::uintptr_t>(code.entry);
    }

    void Invalidate() {
        Timer total{stats.compile_request_ns}, release{stats.code_release_ns};
        memory.ClearCodeWrites();
        faults.smc_pending = 0;
        cache.Clear(); anchor = {}; RefreshNative(native); arena.Clear(); ++stats.cache_invalidations;
    }
    NativeCode CompileUnit(ir::Scope scope) {
        ++stats.compile_attempts;
        scope == ir::Scope::Function ? ++stats.function_attempts : ++stats.block_attempts;
        x86::StateLayout layout;
        layout.gprs_offset = offsetof(GuestState, gprs); layout.pc_offset = offsetof(GuestState, pc); layout.stride = 8;
        layout.fprs_offset = offsetof(GuestState, xmm);
        layout.mxcsr_offset = offsetof(GuestState, mxcsr);
        layout.fs_base_offset = offsetof(GuestState, fs_base); layout.gs_base_offset = offsetof(GuestState, gs_base);
        layout.cpu_query = reinterpret_cast<std::uintptr_t>(&QueryGuestCpu);
        const bool private_guest_state = faults.registered && native.pinned_uniform_offset != UINT32_MAX;
        auto target = arm64::KnownTargetInfo();
        if (faults.registered) target.features |= static_cast<u64>(ir::TargetFeature::FaultLocations);
        if (private_guest_state) target.features |= static_cast<u64>(ir::TargetFeature::PrivateGPRState) |
                static_cast<u64>(ir::TargetFeature::PrivateVectorState);
        const bool mutable_entry = memory.CodeMutable(state.pc);
        const bool mutable_function = scope == ir::Scope::Function && mutable_entry;
        x86::BuildResult built;
        {
            Timer front{stats.frontend_ns};
            if (scope == ir::Scope::Function && !mutable_function) {
                std::vector<x86::BlockWindow> windows;
                for (const auto& region : memory.Regions())
                    if ((region.protection & (PROT_READ | PROT_EXEC)) == (PROT_READ | PROT_EXEC) &&
                        !memory.CodeMutable(region.address)) {
                        const auto bytes = memory.CodeAt(region.address);
                        if (!bytes.empty()) windows.push_back({region.address, bytes});
                    }
                built = x86::BuildFunctionIR(windows, state.pc, layout, options.function_limits, target,
                        nullptr, options.decode_timing ? &stats.compile_decode : nullptr);
            } else {
                if (mutable_function) ++stats.smc_function_single_block_attempts;
                const x86::BlockWindow window{state.pc, memory.CodeAt(state.pc)};
                built = x86::BuildIR({&window, 1}, scope, layout, target,
                        nullptr, options.decode_timing ? &stats.compile_decode : nullptr, mutable_entry);
            }
        }
        stats.emitted_instructions += built.instruction_count; stats.emitted_guest_bytes += built.decoded_bytes;
        if (!built) {
            char message[256];
            std::snprintf(message, sizeof(message), "x86 frontend rejected pc=0x%llx scope=%s error=%u ir_error=%u emitted=%zu bytes=%zu",
                    static_cast<unsigned long long>(built.error_pc), scope == ir::Scope::Function ? "function" : "block",
                    static_cast<unsigned>(built.error), static_cast<unsigned>(built.ir_error), built.instruction_count, built.decoded_bytes);
            diagnostic = message; return {};
        }
        if (options.decode_timing) {
            stats.ir_nodes += built.program.Operations().size();
            stats.operand_words += built.program.OperandArena().size();
        }
        if (options.optimize) {
            Timer optimize{stats.optimize_ns};
            auto optimized = compile::Optimize(std::move(built.program), std::numeric_limits<std::size_t>::max(),
                    compile::UniformPolicy{.flags = compile::UniformRange{offsetof(GuestState, packed_flags), sizeof(state.packed_flags)},
                                           .guest_memory_disjoint = private_guest_state});
            if (options.decode_timing) {
                stats.optimized_erased += optimized.stats.erased;
                stats.optimized_uniform_forwarded += optimized.stats.uniform_forwarded;
                stats.optimized_folded += optimized.stats.folded;
            }
            built.program = std::move(optimized.program);
            if (!optimized) { diagnostic = "IR optimization failed at the current compilation request"; return {}; }
        }
        arm64::CompileResult compiled;
        arm64::CompileOptions policy{offsetof(GuestState, packed_flags)};
        policy.rcpc_loads = rcpc_loads;
        policy.rcpc_imm9 = rcpc_imm9;
        policy.fault_locations = policy.recover_unaligned = faults.registered;
        policy.dispatcher = Dispatcher();
        policy.dispatch_mode = options.decode_timing ? arm64::DispatchMode::InlineCounted : arm64::DispatchMode::Inline;
        policy.callable_directory = true; // This Process borrows only its own callable CodeCache tuple.
        policy.exit_handler = reinterpret_cast<std::uintptr_t>(&SvmNativeExit);
        policy.pc_offset = offsetof(GuestState, pc); policy.pinned_uniform_offset = offsetof(GuestState, gprs);
        policy.pinned_uniform_slots = pinned_uniform_slots;
        policy.native_nzcv = native_nzcv;
        policy.lse_atomics = lse_atomics;
        policy.crc32c = crc32c;
        policy.native_fp_environment = true;
        if (!afp) policy.mxcsr_flush_offset = offsetof(GuestState, mxcsr);
        policy.private_uniforms = private_guest_state;
        policy.local_flags = private_guest_state && faults.registered && native_nzcv;
        policy.pinned_vector_offset = private_guest_state ? offsetof(GuestState, xmm) : UINT32_MAX;
        policy.deferred_pc = private_guest_state && faults.registered;
        policy.entry_pc = state.pc;
        { Timer backend{stats.backend_ns}; compiled = arm64::Compile(built.program, policy); }
        if (options.decode_timing) {
            stats.spill_slots += compiled.spill_slots;
            stats.max_frame_bytes = std::max(stats.max_frame_bytes, static_cast<u64>(compiled.frame_bytes));
        }
        if (!compiled) {
            const auto nodes = built.program.Operations();
            const char* opcode = compiled.node.value < nodes.size() ? Name(nodes[compiled.node.value].opcode) : "none";
            char message[256];
            std::snprintf(message, sizeof(message), "AArch64 compiler rejected pc=0x%llx scope=%s error=%u node=%u block=%u opcode=%s",
                    static_cast<unsigned long long>(state.pc), scope == ir::Scope::Function ? "function" : "block",
                    static_cast<unsigned>(compiled.error), compiled.node.value, compiled.block.value, opcode);
            diagnostic = message; return {};
        }
        NativeCode code;
        {
            Timer publish{stats.publication_ns};
            code = arena.Publish(compiled.words, std::move(compiled.faults), compiled.links);
            ++stats.code_units_published; stats.static_links += compiled.links.size();
            if (options.decode_timing) stats.fault_sites += code.faults.size();
            stats.native_bytes += code.bytes;
            stats.peak_code_bytes = std::max(stats.peak_code_bytes, static_cast<u64>(arena.ReservedBytes()));
            stats.peak_slabs = std::max(stats.peak_slabs, static_cast<u64>(arena.Slabs()));
            stats.peak_dual_slabs = std::max(stats.peak_dual_slabs, static_cast<u64>(arena.DualSlabs()));
            stats.code_wx_fallback |= arena.Slabs() != arena.DualSlabs();
            if (code.writable) for (const auto& link : compiled.links) {
                const auto offset = static_cast<u64>(link.pointer_word) * 4;
                cache.AddLink(link.target_pc, reinterpret_cast<std::uintptr_t*>(code.writable + offset),
                        reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<const std::uint8_t*>(code.entry) + offset),
                        link.pointer_word - link.branch_word);
            }
            // Function formation excludes mutable source regions, including RX
            // mappings with shared writers. Entry requests keep Function RA but
            // externalize every single-source-block edge.
            if (mutable_entry)
                memory.ArmCodeWrites(state.pc, built.decoded_bytes);
        }
        return code;
    }
    bool ReadString(u64 address, std::string& value) const {
        value.clear();
        for (std::size_t i = 0; i < 4096; ++i) {
            if (!memory.Contains(address + i, 1, PROT_READ)) return false;
            const auto byte = *reinterpret_cast<const char*>(address + i);
            if (!byte) return true;
            value += byte;
        }
        return false;
    }
    bool PrepareGuestWrite(u64 address, std::size_t size) {
        if (!memory.Contains(address, size, PROT_WRITE)) return false;
        // C++/kernel writes occur after NativeExit, outside a JIT fault site.
        // Remove protection and retire code before those writers touch pages.
        if (size && memory.WriteAffectsCode(address, size)) Invalidate();
        return true;
    }
    bool GuestWrite(u64 address, const void* source, std::size_t size) {
        return PrepareGuestWrite(address, size) && memory.CopyOut(address, source, size);
    }
    void FileChanged(int fd) {
        // NativeExit has already published state and cleared active. Update
        // bounds and retire executable observers before any guest resumes.
        if (memory.RefreshFileMappings(fd)) {
            ++stats.smc_file_write_invalidations;
            Invalidate();
        }
    }
    s64 Stat(int fd, u64 output, const char* path = nullptr, int flags = 0) {
        struct stat status{};
        const auto result = path ? fstatat(fd, path, &status, flags) : fstat(fd, &status);
        if (result) return -errno;
        const GuestStat guest{static_cast<u64>(status.st_dev), static_cast<u64>(status.st_ino), status.st_nlink,
                status.st_mode, status.st_uid, status.st_gid, 0, static_cast<u64>(status.st_rdev), status.st_size,
                status.st_blksize, status.st_blocks, status.st_atim.tv_sec, static_cast<u64>(status.st_atim.tv_nsec),
                status.st_mtim.tv_sec, static_cast<u64>(status.st_mtim.tv_nsec), status.st_ctim.tv_sec,
                static_cast<u64>(status.st_ctim.tv_nsec), {0, 0, 0}};
        return GuestWrite(output, &guest, sizeof(guest)) ? 0 : -EFAULT;
    }
    s64 Syscall(u64 number, const u64* args) {
        switch (number) {
            case 0:
                if (!PrepareGuestWrite(args[1], args[2])) return -EFAULT;
                return Result(read(static_cast<int>(args[0]), reinterpret_cast<void*>(args[1]), args[2]));
            case 1: {
                if (!memory.Contains(args[1], args[2], PROT_READ)) return -EFAULT;
                const auto count = write(static_cast<int>(args[0]), reinterpret_cast<const void*>(args[1]), args[2]);
                if (count > 0) FileChanged(static_cast<int>(args[0]));
                return Result(count);
            }
            case 3: return Result(close(static_cast<int>(args[0])));
            case 5: return Stat(static_cast<int>(args[0]), args[1]);
            case 8: return Result(lseek(static_cast<int>(args[0]), static_cast<off_t>(args[1]), static_cast<int>(args[2])));
            case 9: {
                constexpr u64 mapping_flags = MAP_SHARED | MAP_PRIVATE | MAP_FIXED | MAP_ANONYMOUS |
                        MAP_DENYWRITE | MAP_EXECUTABLE | MAP_NORESERVE;
                if ((args[2] & ~u64{7}) || (args[3] & ~mapping_flags)) return -EINVAL;
                const bool invalidate = (args[3] & MAP_FIXED) && memory.WriteAffectsCode(args[0], args[1]);
                const auto address = memory.Map(args[0], args[1], static_cast<int>(args[2]), static_cast<int>(args[3]),
                        static_cast<int>(args[4]), args[5]);
                if (address == UINT64_MAX) return -errno;
                if (invalidate || (args[2] & PROT_EXEC) ||
                    ((args[2] & PROT_WRITE) && memory.WriteAffectsCode(address, args[1]))) Invalidate();
                return static_cast<s64>(address);
            }
            case 10: {
                if (args[2] & ~u64{7}) return -EINVAL;
                const bool invalidate = memory.WriteAffectsCode(args[0], args[1]) || (args[2] & PROT_EXEC);
                const auto result = memory.Protect(args[0], args[1], static_cast<int>(args[2]));
                if (!result && invalidate) Invalidate(); return Result(result);
            }
            case 11: {
                const bool invalidate = memory.WriteAffectsCode(args[0], args[1]);
                const auto result = memory.Unmap(args[0], args[1]); if (!result && invalidate) Invalidate(); return Result(result);
            }
            case 12: {
                const auto previous = memory.Brk(0);
                if (args[0] && args[0] != previous &&
                    memory.WriteAffectsCode(std::min(previous, args[0]), std::max(previous, args[0]) - std::min(previous, args[0])))
                    Invalidate();
                return static_cast<s64>(memory.Brk(args[0]));
            }
            case 16: {
                if (args[1] == 0x5413) {
                    struct winsize size{}; if (ioctl(static_cast<int>(args[0]), TIOCGWINSZ, &size)) return -errno;
                    return GuestWrite(args[2], &size, sizeof(size)) ? 0 : -EFAULT;
                }
                if (args[1] == 0x541b) {
                    int bytes{}; if (ioctl(static_cast<int>(args[0]), FIONREAD, &bytes)) return -errno;
                    return GuestWrite(args[2], &bytes, sizeof(bytes)) ? 0 : -EFAULT;
                }
                break;
            }
            case 18: {
                if (!memory.Contains(args[1], args[2], PROT_READ)) return -EFAULT;
                if (args[3] > static_cast<u64>(std::numeric_limits<off_t>::max())) return -EINVAL;
                const auto count = pwrite(static_cast<int>(args[0]), reinterpret_cast<const void*>(args[1]), args[2],
                        static_cast<off_t>(args[3]));
                if (count > 0) FileChanged(static_cast<int>(args[0]));
                return Result(count);
            }
            case 20: {
                if (args[2] > 1024 || !memory.Contains(args[1], static_cast<std::size_t>(args[2]) * sizeof(iovec), PROT_READ)) return -EINVAL;
                const auto count = writev(static_cast<int>(args[0]), reinterpret_cast<const iovec*>(args[1]), static_cast<int>(args[2]));
                if (count > 0) FileChanged(static_cast<int>(args[0]));
                return Result(count);
            }
            case 24: return Result(sched_yield());
            case 28:
                if (!memory.Contains(args[0], args[1], 0)) return -ENOMEM;
                if (const auto result = madvise(reinterpret_cast<void*>(args[0]), args[1], static_cast<int>(args[2])); result) return -errno;
                if (memory.WriteAffectsCode(args[0], args[1])) Invalidate(); return 0;
            case 39: return getpid();
            case 60: case 231: {
                if (clear_child_tid) { const std::uint32_t zero{}; GuestWrite(clear_child_tid, &zero, sizeof(zero)); }
                exited = true; exit_code = static_cast<int>(args[0] & 255); return 0;
            }
            case 63: {
                struct utsname name{}; if (uname(&name)) return -errno;
                std::strcpy(name.machine, "x86_64");
                return GuestWrite(args[0], &name, sizeof(name)) ? 0 : -EFAULT;
            }
            case 72:
                if (args[1] >= F_GETFD && args[1] <= F_SETFL)
                    return Result(fcntl(static_cast<int>(args[0]), static_cast<int>(args[1]), args[2]));
                break;
            case 77: {
                if (args[1] > static_cast<u64>(std::numeric_limits<off_t>::max())) return -EINVAL;
                const auto result = ftruncate(static_cast<int>(args[0]), static_cast<off_t>(args[1]));
                if (!result) FileChanged(static_cast<int>(args[0]));
                return Result(result);
            }
            case 96: {
                timeval now{}; if (gettimeofday(&now, nullptr)) return -errno;
                const s64 value[2]{now.tv_sec, now.tv_usec}; const int timezone[2]{};
                if (args[0] && !GuestWrite(args[0], value, sizeof(value))) return -EFAULT;
                if (args[1] && !GuestWrite(args[1], timezone, sizeof(timezone))) return -EFAULT;
                return 0;
            }
            case 102: return getuid(); case 104: return getgid(); case 107: return geteuid(); case 108: return getegid();
            case 143: case 145: {
                const auto pid = static_cast<pid_t>(args[0]);
                if (number == 143 && !args[1]) return -EINVAL;
                // This guest has one execution thread. Its existing getpid /
                // gettid identities both refer to that thread, never another
                // task which merely shares the host's PID namespace.
                if (pid > 0 && pid != getpid() && pid != faults.thread_id) return -ESRCH;
                const auto thread = pid > 0 ? faults.thread_id : pid;
                if (number == 145) return Result(sched_getscheduler(thread));
                sched_param parameter{};
                if (sched_getparam(thread, &parameter)) return -errno;
                // Linux x86-64 sched_param is one signed 32-bit priority;
                // convert the field rather than copying the host structure.
                const std::int32_t priority = parameter.sched_priority;
                return GuestWrite(args[1], &priority, sizeof(priority)) ? 0 : -EFAULT;
            }
            case 157: {
                if (args[0] != PR_SET_VMA || args[1] != PR_SET_VMA_ANON_NAME) return -EINVAL;
                if (!memory.Contains(args[2], args[3], 0)) return -ENOMEM;
                std::string name;
                if (args[4] && !ReadString(args[4], name)) return -EFAULT;
                return Result(prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, args[2], args[3],
                        args[4] ? name.c_str() : nullptr));
            }
            case 158:
                if (args[0] == 0x1001 || args[0] == 0x1002) {
                    if (args[1] >= (u64{1} << 48)) return -EPERM;
                    (args[0] == 0x1002 ? state.fs_base : state.gs_base) = args[1]; return 0;
                }
                if (args[0] == 0x1003 || args[0] == 0x1004) {
                    const auto value = args[0] == 0x1003 ? state.fs_base : state.gs_base;
                    return GuestWrite(args[1], &value, sizeof(value)) ? 0 : -EFAULT;
                }
                return -EINVAL;
            case 186: return syscall(SYS_gettid);
            case 201: { const auto value = static_cast<s64>(time(nullptr)); if (args[0] && !GuestWrite(args[0], &value, sizeof(value))) return -EFAULT; return value; }
            case 202: {
                const auto operation = static_cast<int>(args[1]);
                const auto command = operation & FUTEX_CMD_MASK;
                const bool timed = command == FUTEX_WAIT || command == FUTEX_WAIT_BITSET;
                const bool requeue = command == FUTEX_REQUEUE || command == FUTEX_CMP_REQUEUE;
                if (!timed && !requeue && command != FUTEX_WAKE && command != FUTEX_WAKE_BITSET &&
                    command != FUTEX_WAKE_OP) break;
                if (!memory.Contains(args[0], sizeof(std::uint32_t), PROT_READ)) return -EFAULT;
                if (command == FUTEX_WAKE_OP) {
                    if (!PrepareGuestWrite(args[4], sizeof(std::uint32_t))) return -EFAULT;
                } else if (requeue && !memory.Contains(args[4], sizeof(std::uint32_t), PROT_READ)) return -EFAULT;
                // Both kernel ABIs use a 32-bit word and two signed 64-bit
                // timespec fields. Requeue's fourth argument is an integer,
                // whereas WAIT/WAIT_BITSET consume an optional time pointer.
                if (timed && args[3] && !memory.Contains(args[3], sizeof(timespec), PROT_READ)) return -EFAULT;
                return Result(syscall(SYS_futex, reinterpret_cast<std::uint32_t*>(args[0]), operation, args[2],
                        args[3], reinterpret_cast<std::uint32_t*>(args[4]), args[5]));
            }
            case 204:
#if defined(SYS_sched_getaffinity)
                if (!PrepareGuestWrite(args[2], args[1])) return -EFAULT;
                return Result(syscall(SYS_sched_getaffinity, static_cast<pid_t>(args[0]), args[1],
                        reinterpret_cast<void*>(args[2])));
#else
                break;
#endif
            case 218: clear_child_tid = args[0]; return syscall(SYS_gettid);
            case 228: {
                timespec now{}; if (clock_gettime(static_cast<clockid_t>(args[0]), &now)) return -errno;
                const s64 value[2]{now.tv_sec, now.tv_nsec}; return GuestWrite(args[1], value, sizeof(value)) ? 0 : -EFAULT;
            }
            case 2: case 21: case 89: case 257: case 262: case 269: {
                const u64 pointer = number == 257 || number == 262 || number == 269 ? args[1] : args[0];
                std::string path; if (!ReadString(pointer, path)) return -EFAULT;
                if (number == 2) {
                    const auto fd = open(path.c_str(), static_cast<int>(args[1]), static_cast<mode_t>(args[2]));
                    if (fd >= 0 && (args[1] & O_TRUNC)) FileChanged(fd);
                    return Result(fd);
                }
                if (number == 21) return Result(access(path.c_str(), static_cast<int>(args[1])));
                if (number == 269)
                    return Result(faccessat(static_cast<int>(args[0]), path.c_str(), static_cast<int>(args[2]), 0));
                if (number == 89) {
                    if (!PrepareGuestWrite(args[1], args[2])) return -EFAULT;
                    return Result(readlink(path.c_str(), reinterpret_cast<char*>(args[1]), args[2]));
                }
                if (number == 257) {
                    const auto fd = openat(static_cast<int>(args[0]), path.c_str(), static_cast<int>(args[2]), static_cast<mode_t>(args[3]));
                    if (fd >= 0 && (args[2] & O_TRUNC)) FileChanged(fd);
                    return Result(fd);
                }
                return Stat(static_cast<int>(args[0]), args[2], path.c_str(), static_cast<int>(args[3]));
            }
            case 302: {
                if (args[0] || args[2]) break; // Query-only; no host process-limit mutation.
                rlimit limit{}; if (getrlimit(static_cast<decltype(RLIMIT_STACK)>(args[1]), &limit)) return -errno;
                const u64 value[2]{limit.rlim_cur, limit.rlim_max}; return args[3] && !GuestWrite(args[3], value, sizeof(value)) ? -EFAULT : 0;
            }
            case 318:
#if defined(SYS_getrandom)
                if (!PrepareGuestWrite(args[0], args[1])) return -EFAULT;
                return Result(syscall(SYS_getrandom, reinterpret_cast<void*>(args[0]), args[1], args[2]));
#else
                break;
#endif
            default: break;
        }
        ++stats.unsupported_syscalls;
        if (options.decode_timing || !reported_unknown) {
            std::fprintf(stderr, "unsupported x86-64 syscall %llu at pc=0x%llx; returning ENOSYS\n",
                    static_cast<unsigned long long>(number), static_cast<unsigned long long>(state.pc)); reported_unknown = true;
        }
        return -ENOSYS;
    }
    void ServiceSyscall() {
        ++stats.syscalls;
        const u64 arguments[6]{state.gprs[RDI], state.gprs[RSI], state.gprs[RDX], state.gprs[R10], state.gprs[R8], state.gprs[R9]};
        state.gprs[RCX] = state.pc;
        const auto flags = state.packed_flags;
        const auto byte = static_cast<unsigned>(flags & 255);
        state.gprs[R11] = 0x202 | ((flags >> 29) & 1) | (u64{(std::popcount(byte) & 1) == 0} << 2) |
                (((flags >> 26) & 1) << 4) | (((flags >> 30) & 1) << 6) | (((flags >> 31) & 1) << 7) | (((flags >> 28) & 1) << 11);
        state.gprs[RAX] = static_cast<u64>(Syscall(state.gprs[RAX], arguments));
    }
};
} // namespace

RunResult RunGuest(const std::string& executable, std::span<const std::string> argv,
                   std::span<const std::string> environment, RuntimeOptions options) {
    RuntimeStats stats; int code = 125; std::string error;
    const auto begin = Clock::now();
    try { Process process(options, stats); code = process.Run(executable, argv, environment); }
    catch (const std::exception& failure) { error = failure.what(); }
    if (options.decode_timing) stats.compile_without_byte_decode_ns = stats.compile_request_ns - stats.compile_decode.nanoseconds;
    stats.run_lifetime_ns = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    return {code, stats, std::move(error)};
}

} // namespace swift::runtime::execution
