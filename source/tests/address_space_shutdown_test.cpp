
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "base/scope_exit.h"
#include "runtime/compile/analysis/guest_state.h"
#ifdef __linux__
#include <sys/uio.h>
#endif
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "aot/aot_artifact.h"
#include "aot/aot_load.h"
#include "elfio/elfio.hpp"
#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/trampolines.h"
#include "runtime/backend/context.h"
#include "runtime/backend/jit_cache.h"
#include "runtime/backend/riscv64/trampolines.h"
#include "runtime/backend/runtime.h"
#include "runtime/common/backedge_control.h"
#include "runtime/common/perf_stats.h"
#include "runtime/ir/hir_builder.h"

namespace {
thread_local bool deny_allocations{};
thread_local int fail_aot_allocation = -1;
thread_local unsigned aot_allocation_index{};
thread_local bool aot_failure_hit{}, aot_was_published{}, aot_published_at_failure{};
thread_local bool fail_after_aot_push{}, inject_aot_cleanup{}, fail_cleanup_allocation{};
thread_local unsigned post_push_failure_index{};
thread_local unsigned cleanup_skip_queries{};
thread_local bool arm_after_next_read_unit{};
thread_local unsigned private_failure_index{};
thread_local bool deny_guest_read{};
thread_local unsigned interrupt_guest_read{}, partial_guest_read{};
thread_local bool aot_protection_failure_hit{}, cleanup_query_observed{}, cleanup_failure_hit{};
thread_local bool inside_watched_purge{};
thread_local bool count_retirement_allocations{}, retirement_failure_hit{};
thread_local int fail_retirement_allocation = -1;
thread_local unsigned retirement_allocation_index{};
bool track_maps{}, fail_build_allocation{}, fail_uniform_allocation{}, saw_trampoline_rx{};
bool fail_construction_cleanup{};
void* uniform_failure_caller{};
struct TrackedView { void* address{}; size_t size{}; bool live{}; };
std::array<TrackedView, 128> tracked_views{};
size_t tracked_view_count{};
void* second_failing_unmap{};
unsigned second_unmap_budget{};
void (*during_unmap)(){};
void* failing_unmap{};
void* failing_unprotect{};
void* failing_reprotect{};
unsigned reprotect_budget{};
unsigned unmap_budget{}, unprotect_budget{}, close_budget{};
unsigned arena_destroys{}, checks{};
unsigned close_calls{}, rename_calls{}, deny_arena_budget{}, deny_rx_map_budget{}, deny_truncate_budget{};
std::atomic<void*> watched_code_free{};
std::atomic<unsigned> watched_code_frees{};
std::atomic<const void*> watched_purge_allocation{};
std::atomic<bool> purge_wait_observed{}, purge_completed{}, free_before_purge{};
std::atomic<bool> linked_executor_inside{}, linked_executor_release{};
std::atomic<unsigned> dual_executor_inside{}, dual_executor_released{};
std::atomic<void*> pause_protection{};
std::atomic<bool> protection_paused{}, finish_protection{};
bool fail_aot_protect{}, watch_aot_allocation{};
bool capture_published_lifetime{};
std::shared_ptr<swift::runtime::backend::CodeLifetime> published_failure_lifetime;
size_t watched_aot_size{};
unsigned multi_oom_pushes{}, multi_protect_pushes{}, multi_cleanup_queries{};
unsigned multi_post_push_failure_index{};
bool multi_disarm_after_failing_unit{};
unsigned multi_fail_allocation_ordinal{};
bool capture_multi_lifetimes{};
bool watch_multi_purge{};
bool capture_code_buffers{};
std::array<void*, 8> captured_code_exec{};
std::array<void*, 8> captured_code_rw{};
unsigned captured_code_buffer_count{};
std::array<std::shared_ptr<swift::runtime::backend::CodeLifetime>, 8> multi_lifetimes{};
unsigned multi_lifetime_count{};
bool watch_multi_allocations{};
std::array<void*, 8> multi_allocations{};
std::array<unsigned, 8> multi_allocation_frees{};
unsigned multi_allocation_count{};
struct BlockEmissionAllocationWatch {
    swift::runtime::backend::Module* module{};
    swift::runtime::ir::Block* block{};
    bool reject_first{};
    bool rejected{};
    bool unpublished{true};
    unsigned calls{};
    std::array<bool, 2> required_region{};
    std::array<swift::u32, 2> sizes{};
    std::array<swift::u64, 2> ir_signature{};
    std::array<std::array<unsigned long long, 4>, 2> phases{};
};
thread_local BlockEmissionAllocationWatch* block_emission_allocation_watch{};
}

void* operator new(std::size_t size) {
    if (count_retirement_allocations) {
        const auto index = retirement_allocation_index++;
        if (fail_retirement_allocation >= 0 && index == static_cast<unsigned>(fail_retirement_allocation)) {
            retirement_failure_hit = true;
            throw std::bad_alloc{};
        }
    }
    if (fail_cleanup_allocation) {
        fail_cleanup_allocation = false;
        cleanup_failure_hit = true;
        throw std::bad_alloc{};
    }
    if (fail_aot_allocation >= 0 && aot_allocation_index++ == static_cast<unsigned>(fail_aot_allocation)) {
        aot_failure_hit = true;
        aot_published_at_failure = aot_was_published;
        fail_aot_allocation = -1;
        throw std::bad_alloc{};
    }
    const bool uniform_failure = fail_uniform_allocation && saw_trampoline_rx &&
                                 size == sizeof(swift::runtime::compile::GuestStateLayout);
    if (uniform_failure) uniform_failure_caller = __builtin_return_address(0);
    if (deny_allocations || (fail_build_allocation && saw_trampoline_rx) || uniform_failure) {
        if (fail_construction_cleanup) deny_allocations = true;
        throw std::bad_alloc{};
    }
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

extern "C" int __real_munmap(void*, size_t);
extern "C" int __wrap_munmap(void* memory, size_t size) {
    if (during_unmap) during_unmap();
    if (memory == failing_unmap && unmap_budget) {
        --unmap_budget; errno = EIO; return -1;
    }
    if (memory == second_failing_unmap && second_unmap_budget) {
        --second_unmap_budget; errno = EIO; return -1;
    }
    const int result = __real_munmap(memory, size);
    if (result == 0) for (auto& view : tracked_views)
        if (view.address == memory && view.size == size) view.live = false;
    return result;
}
extern "C" void* __real_mmap(void*, size_t, int, int, int, off_t);
extern "C" void* __wrap_mmap(void* memory, size_t size, int protection, int flags, int descriptor, off_t offset) {
    if ((protection & PROT_EXEC) && deny_rx_map_budget) {
        --deny_rx_map_budget; errno = ENOMEM; return MAP_FAILED;
    }
    void* result = __real_mmap(memory, size, protection, flags, descriptor, offset);
    if (track_maps && result != MAP_FAILED) {
        if (tracked_view_count == tracked_views.size()) std::abort();
        tracked_views[tracked_view_count++] = {result, size, true};
        if (size == 4 * 1024 * 1024 && (protection & PROT_EXEC)) {
            saw_trampoline_rx = true;
            if (fail_construction_cleanup) { failing_unmap = result; unmap_budget = 3; }
        }
    }
    return result;
}
extern "C" int __real_ftruncate(int, off_t);
extern "C" int __wrap_ftruncate(int descriptor, off_t size) {
    if (deny_truncate_budget) { --deny_truncate_budget; errno = ENOSPC; return -1; }
    return __real_ftruncate(descriptor, size);
}
extern "C" int __real_mprotect(void*, size_t, int);
extern "C" int __wrap_mprotect(void* memory, size_t size, int protection) {
    if (memory == failing_reprotect && protection == PROT_READ && reprotect_budget) {
        --reprotect_budget;
        aot_protection_failure_hit = true;
        errno = EACCES; return -1;
    }
    if (fail_aot_protect && protection == PROT_READ) {
        fail_aot_protect = false;
        aot_protection_failure_hit = true;
        errno = EACCES; return -1;
    }
    if (memory == failing_unprotect && (protection & PROT_WRITE) && unprotect_budget) {
        --unprotect_budget; errno = EACCES; return -1;
    }
    const int result = __real_mprotect(memory, size, protection);
    if (result == 0 && protection == PROT_READ && memory == pause_protection.load()) {
        protection_paused = true;
        while (!finish_protection.load()) std::this_thread::yield();
    }
    return result;
}
extern "C" int __real_close(int);
#ifdef __linux__
extern "C" ssize_t __real_process_vm_readv(pid_t, const iovec*, unsigned long, const iovec*, unsigned long, unsigned long);
extern "C" ssize_t __wrap_process_vm_readv(pid_t pid, const iovec* local, unsigned long local_count,
        const iovec* remote, unsigned long remote_count, unsigned long flags) {
    if (deny_guest_read) { errno = EPERM; return -1; }
    if (interrupt_guest_read) { --interrupt_guest_read; errno = EINTR; return -1; }
    if (partial_guest_read) { --partial_guest_read; return 0; }
    return __real_process_vm_readv(pid, local, local_count, remote, remote_count, flags);
}
#endif
extern "C" int __wrap_close(int descriptor) {
    ++close_calls;
    const int result = __real_close(descriptor);
    if (close_budget) { --close_budget; errno = EIO; return -1; }
    return result;
}
extern "C" int __real_rename(const char*, const char*);
extern "C" int __wrap_rename(const char* source, const char* target) {
    ++rename_calls;
    return __real_rename(source, target);
}
extern "C" void* __real_create_mspace_with_base(void*, size_t, int);
extern "C" void* __wrap_create_mspace_with_base(void* memory, size_t size, int locked) {
    if (deny_arena_budget) { --deny_arena_budget; return nullptr; }
    return __real_create_mspace_with_base(memory, size, locked);
}
extern "C" size_t __real_destroy_mspace(void*);
extern "C" size_t __wrap_destroy_mspace(void* arena) {
    ++arena_destroys;
    return __real_destroy_mspace(arena);
}
extern "C" void __real_mspace_free(void*, void*);
extern "C" void __wrap_mspace_free(void* arena, void* memory) {
    if (watch_multi_allocations) {
        for (unsigned i = 0; i < multi_allocation_count; ++i)
            if (memory == multi_allocations[i]) ++multi_allocation_frees[i];
    }
    if (memory == watched_code_free.load()) {
        if (watched_purge_allocation && !purge_completed) free_before_purge = true;
        ++watched_code_frees;
    }
    __real_mspace_free(arena, memory);
}
extern "C" void* __real_mspace_memalign(void*, size_t, size_t);
extern "C" void* __wrap_mspace_memalign(void* arena, size_t alignment, size_t size) {
    auto* result = __real_mspace_memalign(arena, alignment, size);
    if (watch_aot_allocation && result && size == watched_aot_size) {
        watched_code_free = result;
        watched_code_frees = 0;
    }
    if (watch_multi_allocations && result && size == watched_aot_size) {
        if (multi_allocation_count >= multi_allocations.size()) std::abort();
        const unsigned ordinal = multi_allocation_count;
        multi_allocations[multi_allocation_count++] = result;
        if (multi_disarm_after_failing_unit && ordinal > multi_fail_allocation_ordinal &&
            fail_aot_allocation >= 0 && !aot_failure_hit) {
            fail_aot_allocation = -1;
            multi_disarm_after_failing_unit = false;
        }
    }
    return result;
}
extern "C" bool __real__ZN5swift7runtime7backend6Module4PushEPNS0_2ir11AddressNodeE(
        swift::runtime::backend::Module*, swift::runtime::ir::AddressNode*);
extern "C" bool __wrap__ZN5swift7runtime7backend6Module4PushEPNS0_2ir11AddressNodeE(
        swift::runtime::backend::Module* module, swift::runtime::ir::AddressNode* node) {
    const bool result = __real__ZN5swift7runtime7backend6Module4PushEPNS0_2ir11AddressNodeE(module, node);
    if (watch_aot_allocation && result) {
        aot_was_published = true;
        if (capture_published_lifetime) published_failure_lifetime = node->code_lifetime;
        if (capture_multi_lifetimes && multi_lifetime_count < multi_lifetimes.size())
            multi_lifetimes[multi_lifetime_count++] = node->code_lifetime;
        if (fail_after_aot_push) {
            fail_after_aot_push = false;
            fail_aot_allocation = static_cast<int>(post_push_failure_index);
            aot_allocation_index = 0;
        }
    }
    if (result && multi_oom_pushes && --multi_oom_pushes == 0) {
        fail_aot_allocation = static_cast<int>(multi_post_push_failure_index);
        aot_allocation_index = 0;
    }
    if (result && multi_protect_pushes && --multi_protect_pushes == 0) fail_aot_protect = true;
    return result;
}
std::pair<swift::u16, swift::runtime::backend::CodeBuffer> RealAllocCodeCache(
        swift::runtime::backend::Module*, swift::u32, bool)
        asm("__real__ZN5swift7runtime7backend6Module14AllocCodeCacheEjb");
std::pair<swift::u16, swift::runtime::backend::CodeBuffer> WrappedAllocCodeCache(
        swift::runtime::backend::Module*, swift::u32, bool)
        asm("__wrap__ZN5swift7runtime7backend6Module14AllocCodeCacheEjb");
std::pair<swift::u16, swift::runtime::backend::CodeBuffer> WrappedAllocCodeCache(
        swift::runtime::backend::Module* module, swift::u32 size, bool require_direct_link_region) {
    if (auto* watch = block_emission_allocation_watch; watch && watch->module == module) {
        const auto call = watch->calls++;
        if (call >= watch->required_region.size()) std::abort();
        watch->required_region[call] = require_direct_link_region;
        watch->sizes[call] = size;
        auto signature = swift::u64{1469598103934665603ull};
        for (const auto& instruction : watch->block->GetInstList()) {
            signature = (signature ^ reinterpret_cast<uintptr_t>(&instruction)) * 1099511628211ull;
            signature = (signature ^ instruction.Id()) * 1099511628211ull;
            signature = (signature ^ static_cast<swift::u64>(instruction.GetOp())) * 1099511628211ull;
        }
        watch->ir_signature[call] = signature;
        const auto& stats = swift::runtime::GetPerfStats2();
        watch->phases[call] = {
                stats.pass_total.calls.load(std::memory_order_relaxed),
                stats.allocate_block.calls.load(std::memory_order_relaxed),
                stats.allocate_function.calls.load(std::memory_order_relaxed),
                stats.codegen_complete.calls.load(std::memory_order_relaxed)};
        watch->unpublished &= watch->block->GetJitCache().jit_state !=
                              swift::runtime::backend::JitState::Cached;
        watch->unpublished &= !module->GetAddressSpace().GetLinkManager().QueryTarget(
                watch->block->GetStartLocation().Value());
        if (watch->reject_first && call == 0) {
            watch->rejected = true;
            return {swift::runtime::backend::INVALID_CACHE_ID,
                    swift::runtime::backend::CodeBuffer{nullptr, nullptr, 0, 0}};
        }
    }
    if (multi_disarm_after_failing_unit && captured_code_buffer_count > multi_fail_allocation_ordinal &&
        fail_aot_allocation >= 0 && !aot_failure_hit) {
        fail_aot_allocation = -1;
        multi_disarm_after_failing_unit = false;
    }
    auto result = RealAllocCodeCache(module, size, require_direct_link_region);
    if (capture_code_buffers && result.second.exec_data &&
        (watched_aot_size == 0 || size == watched_aot_size)) {
        if (captured_code_buffer_count >= captured_code_exec.size()) std::abort();
        captured_code_exec[captured_code_buffer_count] = result.second.exec_data;
        captured_code_rw[captured_code_buffer_count] = result.second.rw_data;
        ++captured_code_buffer_count;
        if (watch_multi_allocations) {
            if (multi_allocation_count >= multi_allocations.size()) std::abort();
            multi_allocations[multi_allocation_count++] = result.second.rw_data;
        }
    }
    return result;
}
std::vector<swift::u64> RealQueryTargets(
        const swift::runtime::backend::LinkManager*, swift::runtime::backend::LinkSourceOwner)
        asm("__real__ZNK5swift7runtime7backend11LinkManager12QueryTargetsENS1_15LinkSourceOwnerE");
std::vector<swift::u64> WrappedQueryTargets(
        const swift::runtime::backend::LinkManager*, swift::runtime::backend::LinkSourceOwner)
        asm("__wrap__ZNK5swift7runtime7backend11LinkManager12QueryTargetsENS1_15LinkSourceOwnerE");
std::vector<swift::u64> WrappedQueryTargets(
        const swift::runtime::backend::LinkManager* manager, swift::runtime::backend::LinkSourceOwner owner) {
    if (multi_cleanup_queries && --multi_cleanup_queries == 0) {
        cleanup_query_observed = true;
        fail_cleanup_allocation = true;
    }
    if (inject_aot_cleanup && aot_was_published && (aot_failure_hit || aot_protection_failure_hit)) {
        if (cleanup_skip_queries) {
            --cleanup_skip_queries;
        } else {
            inject_aot_cleanup = false;
            cleanup_query_observed = true;
            fail_cleanup_allocation = true;
        }
    }
    return RealQueryTargets(manager, owner);
}
extern "C" size_t __real__ZN5swift7runtime7backend11LinkManager11PurgeSourceENS1_15LinkSourceOwnerE(
        swift::runtime::backend::LinkManager*, swift::runtime::backend::LinkSourceOwner);
extern "C" size_t __wrap__ZN5swift7runtime7backend11LinkManager11PurgeSourceENS1_15LinkSourceOwnerE(
        swift::runtime::backend::LinkManager* manager, swift::runtime::backend::LinkSourceOwner owner) {
    bool watched = owner.allocation == watched_purge_allocation.load();
    if (!watched && watch_multi_purge) {
        for (unsigned i = 0; i < multi_lifetime_count; ++i) {
            if (multi_lifetimes[i] && owner.allocation == multi_lifetimes[i]->base) { watched = true; break; }
        }
    }
    inside_watched_purge = watched;
    const auto reset = swift::ScopeExit([] { inside_watched_purge = false; });
    const auto result = __real__ZN5swift7runtime7backend11LinkManager11PurgeSourceENS1_15LinkSourceOwnerE(manager, owner);
    if (watched) purge_completed = true;
    return result;
}
extern "C" int __real_sched_yield();
extern "C" int __wrap_sched_yield() {
    if (inside_watched_purge) purge_wait_observed = true;
    return __real_sched_yield();
}
bool RealReadUnit(swift::runtime::backend::BlobReader&, swift::runtime::backend::SerialUnit&)
        asm("__real__ZN5swift7runtime7backend8ReadUnitERNS1_10BlobReaderERNS1_10SerialUnitE");
bool WrappedReadUnit(swift::runtime::backend::BlobReader&, swift::runtime::backend::SerialUnit&)
        asm("__wrap__ZN5swift7runtime7backend8ReadUnitERNS1_10BlobReaderERNS1_10SerialUnitE");
bool WrappedReadUnit(swift::runtime::backend::BlobReader& reader, swift::runtime::backend::SerialUnit& unit) {
    if (multi_disarm_after_failing_unit && multi_lifetime_count > multi_fail_allocation_ordinal &&
        fail_aot_allocation >= 0 && !aot_failure_hit) {
        fail_aot_allocation = -1;
        multi_disarm_after_failing_unit = false;
    }
    const bool result = RealReadUnit(reader, unit);
    if (result && arm_after_next_read_unit) {
        arm_after_next_read_unit = false;
        fail_aot_allocation = static_cast<int>(private_failure_index);
        aot_allocation_index = 0;
    }
    return result;
}

namespace {
using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::backend;
using namespace swift::runtime::ir;
namespace aot = swift::aot;

void Check(bool condition, const char* detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}

void CheckCallHalt(HaltReason actual, HaltReason expected, VAddr location, const char* detail) {
    std::array<char, 256> diagnostic{};
    std::snprintf(diagnostic.data(),
                  diagnostic.size(),
                  "%s: actual=0x%x expected=0x%x location=0x%llx",
                  detail,
                  static_cast<unsigned>(actual),
                  static_cast<unsigned>(expected),
                  static_cast<unsigned long long>(location));
    Check(actual == expected, diagnostic.data());
}

constexpr u32 kCallCallbackOffset = 32;
constexpr u32 kReturningCallUniformSize = kCallCallbackOffset + sizeof(u64);

Config TestConfig(void* guest = nullptr) {
    Config config{};
    config.loc_start = 0;
    config.loc_end = 8 * static_cast<size_t>(getpagesize());
    config.enable_jit = true;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
    config.memory_base = guest;
    config.guest_addr_mask = config.loc_end - 1;
#if defined(__riscv)
    config.backend_isa = kRiscv64;
#else
    config.backend_isa = kArm64;
#endif
    return config;
}

class GuestWindow {
public:
    const size_t size = 8 * static_cast<size_t>(getpagesize());
    void* const memory = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    GuestWindow() { Check(memory != MAP_FAILED, "allocate guest window"); }
    ~GuestWindow() { if (memory != MAP_FAILED) __real_munmap(memory, size); }
};

bool Mapped(const void* pointer) {
    const auto page = static_cast<uintptr_t>(getpagesize());
    unsigned char resident{};
    errno = 0;
    if (mincore(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pointer) & ~(page - 1)),
                page, &resident) == 0) return true;
    if (errno == ENOMEM) return false;
    throw std::runtime_error("mincore failed without proving an absent mapping");
}

struct CounterAlias final : TerminalAliasOwner {
    unsigned calls{};
    void Revoke() noexcept override { ++calls; }
};

RetirementControl::Ticket Terminal(AddressSpace& space) {
    auto ticket = RetirementControl::RequestDrain(space.GetRetirementControl(), true);
    Check(space.GetRetirementControl()->SealPublications(ticket), "seal actual terminal ticket");
    return ticket;
}

void Gates() {
    AddressSpace space{TestConfig()};
    auto alias = std::make_shared<CounterAlias>();
    Check(space.RegisterTerminalAlias(alias), "register terminal alias owner");
    auto module = space.GetDefaultModule();
    const auto [id, code] = module->AllocCodeCache(64);
    auto ordinary = RetirementControl::RequestDrain(space.GetRetirementControl());
    Check(space.GetRetirementControl()->SealPublications(ordinary), "seal ordinary drain");
    Check(!space.TryShutdown(ordinary).Complete() && alias->calls == 0 && Mapped(code.exec_data),
          "ordinary drain neither revokes nor unmaps");
    Check(space.GetRetirementControl()->Resume(ordinary), "ordinary drain still resumes");
    {
        AddressSpace other{TestConfig()};
        const auto ticket = Terminal(other);
        Check(!space.TryShutdown(ticket).Complete() && alias->calls == 0 && Mapped(code.exec_data),
              "another address-space ticket cannot release this code");
    }
    u64 request{};
    auto owner = std::make_unique<RetirementControl::ExitRegistration>(space.GetRetirementControl(), request);
    RetirementControl::Ticket ticket;
    {
        RetirementControl::Lease execution{space.GetRetirementControl(), RetirementControl::Kind::Execution};
        ticket = RetirementControl::RequestDrain(space.GetRetirementControl(), true);
        Check(!space.TryShutdown(ticket).Complete() && alias->calls == 0 && Mapped(code.exec_data),
              "self-active execution prevents every irreversible stage");
    }
    Check(space.GetRetirementControl()->SealPublications(ticket), "seal after execution exit");
    Check(!space.TryShutdown(ticket).Complete() && alias->calls == 0 && Mapped(code.exec_data),
          "idle Runtime owner prevents every irreversible stage");
    owner.reset();
    Check(space.TryShutdown(ticket).Complete() && alias->calls == 1 && !Mapped(code.exec_data),
          "final owner exit authorizes actual release");
    Check(!space.MapModule(0x800, 0x900, ModuleConfig{}), "late Module creation is refused");
    Check(space.TryShutdown(ticket).Complete() && alias->calls == 1, "completed shutdown is idempotent");
}

void Tokens() {
    AddressSpace space{TestConfig()};
    auto module = space.GetDefaultModule();
    const auto [id, code] = module->AllocCodeCache(64);
    auto alias = std::make_shared<CounterAlias>();
    Check(space.RegisterTerminalAlias(alias), "register low-level-token alias");
    TranslateTable l1;
    u64 request{};
    auto token = space.GetSmcTracker().RegisterRuntime(l1, &request);
    const auto ticket = Terminal(space);
    Check(space.GetRetirementControl()->CheckShutdown(ticket) == RetirementControl::DrainStatus::Ready,
          "low-level token is deliberately independent of Runtime attachment admission");
    for (unsigned i = 0; i < 2; ++i) {
        const auto result = space.TryShutdown(ticket);
        Check(result.stage == AddressSpace::ShutdownStage::Smc && result.smc.runtimes_attached &&
              alias->calls == 0 && Mapped(code.exec_data), "low-level token blocks every irreversible stage");
    }
    space.GetSmcTracker().UnregisterRuntime(token);
    Check(space.TryShutdown(ticket).Complete() && alias->calls == 1 && !Mapped(code.exec_data),
          "unregistering the last token permits full physical shutdown");
    bool rejected{};
    try { (void)space.GetSmcTracker().RegisterRuntime(l1, &request); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "late low-level registration cannot reopen terminal shutdown");
}

struct RunningContext {
    AddressSpace* space{};
    CounterAlias* alias{};
    void* entry{};
    void* trampoline{};
    RetirementControl::Ticket ticket;
    unsigned callbacks{};
    bool failed{};
};
thread_local RunningContext* running_context{};

u64 RequestShutdownFromCode() noexcept {
    auto& state = *running_context;
    ++state.callbacks;
    try {
        state.ticket = RetirementControl::RequestDrain(state.space->GetRetirementControl(), true);
        const auto result = state.space->TryShutdown(state.ticket);
        Check(!result.Complete() && state.alias->calls == 0 && Mapped(state.entry) && Mapped(state.trampoline),
              "actual executing helper return address prevents revocation and physical release");
    } catch (...) { state.failed = true; }
    return 84;
}

void Running() {
    AddressSpace space{TestConfig()};
    auto module = space.GetDefaultModule();
    auto alias = std::make_shared<CounterAlias>();
    Check(space.RegisterTerminalAlias(alias), "register actual-running shutdown alias");
    HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{0x100}, Location{0x104});
    const auto callback = static_cast<u64>(reinterpret_cast<uintptr_t>(
            FptrCast(static_cast<u64 (*)()>(&RequestShutdownFromCode))));
    const auto value = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{callback}});
    function->StoreUniform(Uniform{0, ValueType::U64}, value);
    function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
    void* entry = TranslateIR(module, function);
    void* trampoline = reinterpret_cast<void*>(space.GetTrampolines().GetRuntimeEntry());
    Check(entry && Mapped(entry) && Mapped(trampoline), "publish real native callback code and entry trampoline");
    RunningContext state{&space, alias.get(), entry, trampoline};
    running_context = &state;
    auto runtime = std::make_unique<Runtime>(&space);
    runtime->SetLocation(0x100);
    const auto reason = runtime->Run();
    running_context = nullptr;
    Check((reason == HaltReason::CallHost || reason == HaltReason::Retirement) &&
          state.callbacks == 1 && !state.failed, "actual generated code reaches and returns from the shutdown request");
    Check(space.GetRetirementControl()->SealPublications(state.ticket), "seal after the generated execution exits");
    Check(!space.TryShutdown(state.ticket).Complete() && alias->calls == 0 && Mapped(entry) && Mapped(trampoline),
          "stopped Runtime attachment still pins actual code before detach");
    runtime.reset();
    Check(space.TryShutdown(state.ticket).Complete() && alias->calls == 1 && !Mapped(entry) && !Mapped(trampoline),
          "actual execution exit and Runtime detach permit final physical code and trampoline release");
}

size_t DescriptorCount() {
    auto* directory = opendir("/proc/self/fd");
    Check(directory != nullptr, "open descriptor inventory");
    size_t count{};
    while (auto* entry = readdir(directory)) if (entry->d_name[0] != '.') ++count;
    closedir(directory);
    return count;
}

void StartTracking() {
    Check(MemMap::GetRetainedReleaseState().Complete(), "construction starts without retained failures");
    tracked_views = {}; tracked_view_count = 0; saw_trampoline_rx = false; track_maps = true;
}

void VerifyConstructionRelease(size_t descriptors) {
    track_maps = false;
    Check(tracked_view_count != 0, "construction actually acquired backing mappings");
    for (size_t i = 0; i < tracked_view_count; ++i)
        Check(!tracked_views[i].live && !Mapped(tracked_views[i].address), "constructor rollback actually unmaps each view");
    Check(DescriptorCount() == descriptors && MemMap::GetRetainedReleaseState().Complete(),
          "successful construction rollback returns descriptors and owners to baseline");
}

void Construction() {
    const auto descriptors = DescriptorCount();
    const auto config = TestConfig();
    StartTracking();
    fail_build_allocation = true;
    bool failed{};
    try { AddressSpace space{config}; } catch (const std::bad_alloc&) { failed = true; }
    fail_build_allocation = false;
    Check(failed && saw_trampoline_rx, "inject derived Build allocation failure after arena acquisition");
    VerifyConstructionRelease(descriptors);

    StartTracking();
    fail_uniform_allocation = true; failed = false;
    try { AddressSpace space{config}; } catch (const std::bad_alloc&) { failed = true; }
    fail_uniform_allocation = false;
    Check(failed && saw_trampoline_rx, "inject AddressSpace uniform metadata failure after trampoline allocation");
    Dl_info origin{};
    Check(uniform_failure_caller && dladdr(uniform_failure_caller, &origin) && origin.dli_fbase,
          "bind the uniform failure's actual caller to this executable");
    std::printf("uniform_failure_image_offset=%#llx\n", static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(uniform_failure_caller) - reinterpret_cast<uintptr_t>(origin.dli_fbase)));
    VerifyConstructionRelease(descriptors);

    StartTracking(); fail_build_allocation = fail_construction_cleanup = true; failed = false;
    const auto destroys = arena_destroys;
    try { AddressSpace space{config}; } catch (const std::bad_alloc&) { failed = true; }
    fail_build_allocation = fail_construction_cleanup = false;
    const auto retained = MemMap::GetRetainedReleaseState();
    deny_allocations = false;
    Check(failed && saw_trampoline_rx && retained.owners == 1 && retained.first_failure.unmap_error == EIO &&
          Mapped(failing_unmap) && arena_destroys == destroys + 1,
          "constructor bad_alloc retains its failed live Impl without allocating and destroys the arena once");
    deny_allocations = true;
    const auto pending = MemMap::RetryRetainedReleases();
    deny_allocations = false;
    Check(pending.owners == 1 && pending.first_failure.unmap_error == EIO && Mapped(failing_unmap),
          "constructor cleanup failure persists through a subsequent allocation-free retry");
    Check(MemMap::RetryRetainedReleases().Complete() && arena_destroys == destroys + 1,
          "constructor cleanup retry completes without destroying its arena again");
    failing_unmap = nullptr;
    VerifyConstructionRelease(descriptors);

    StartTracking();
    auto invalid = config; invalid.backend_isa = kRiscv64; invalid.enable_asm_interp = true;
    failed = false;
    try { riscv64::TrampolinesRiscv64 trampoline{invalid}; }
    catch (const std::invalid_argument&) { failed = true; }
    Check(failed, "RV derived construction rejects interpreter after base arena construction");
    VerifyConstructionRelease(descriptors);

    StartTracking(); deny_rx_map_budget = 1; failed = false;
    try { CodeCache cache{config, 4096, FeatureSet{}}; } catch (const std::bad_alloc&) { failed = true; }
    Check(failed, "inject CodeCache RX acquisition failure");
    VerifyConstructionRelease(descriptors);

    for (bool data : {false, true}) {
        StartTracking(); deny_arena_budget = 1; failed = false;
        try {
            if (data) { DataAllocator allocator{4096}; }
            else { CodeCache cache{config, 4096, FeatureSet{}}; }
        } catch (const std::bad_alloc&) { failed = true; }
        Check(failed, "inject allocator arena construction failure");
        VerifyConstructionRelease(descriptors);
    }
    deny_truncate_budget = 1; failed = false;
    try { MemMap memory{4096, true}; } catch (const std::bad_alloc&) { failed = true; }
    Check(failed && DescriptorCount() == descriptors && MemMap::GetRetainedReleaseState().Complete(),
          "Impl initialization failure releases its already acquired descriptor");
}

std::unique_ptr<MemMap> interleaved_owner;
size_t observed_inflight{};
void EnqueueDuringRetry() {
    during_unmap = nullptr;
    observed_inflight = MemMap::GetRetainedReleaseState().owners;
    (void)interleaved_owner->ReleaseAfterQuiescence();
    interleaved_owner.reset();
}

void Retained() {
    const auto descriptors = DescriptorCount();
    auto owner = std::make_unique<MemMap>(4096, true);
    void* rw = owner->GetMemory();
    void* rx = owner->Map(4096, 0, MemMap::ReadExe);
    failing_unmap = rx; unmap_budget = 4;
    deny_allocations = true;
    const auto first = owner->ReleaseAfterQuiescence();
    owner.reset();
    const auto retained = MemMap::GetRetainedReleaseState();
    deny_allocations = false;
    Check(first.unmap_error == EIO && retained.owners == 1 && retained.first_failure.unmap_error == EIO &&
          Mapped(rx) && Mapped(rw), "failed release transfers the original live Impl without allocating");
    for (unsigned i = 0; i < 2; ++i) {
        deny_allocations = true;
        const auto retry = MemMap::RetryRetainedReleases();
        deny_allocations = false;
        Check(retry.owners == 1 && retry.first_failure.unmap_error == EIO && Mapped(rx),
              "persistent unmap failure survives one bounded retry per invocation");
    }
    interleaved_owner = std::make_unique<MemMap>(4096, false);
    second_failing_unmap = interleaved_owner->GetMemory(); second_unmap_budget = 3;
    during_unmap = EnqueueDuringRetry;
    deny_allocations = true;
    const auto interleaved = MemMap::RetryRetainedReleases();
    deny_allocations = false;
    Check(observed_inflight == 1 && interleaved.owners == 1 && !Mapped(rx) && !Mapped(rw) &&
          Mapped(second_failing_unmap), "in-flight owner stays counted and newly queued owner waits for the next retry");
    Check(!MemMap::RetryRetainedReleases().Complete() && Mapped(second_failing_unmap),
          "newly queued failure is retained across its next retry");
    Check(MemMap::RetryRetainedReleases().Complete() && !Mapped(second_failing_unmap) &&
          DescriptorCount() == descriptors, "final checked retry returns real mappings and descriptors to baseline");
    failing_unmap = second_failing_unmap = nullptr;

    const pid_t child = fork();
    Check(child >= 0, "fork sticky retained-close diagnostic");
    if (child == 0) {
        auto failed = std::make_unique<MemMap>(4096, false);
        close_budget = 1;
        const auto result = failed->ReleaseAfterQuiescence();
        const auto calls = close_calls;
        deny_allocations = true;
        failed.reset();
        const auto a = MemMap::RetryRetainedReleases();
        const auto b = MemMap::RetryRetainedReleases();
        _exit(result.close_error == EIO && a.owners == 1 && b.owners == 1 &&
              b.first_failure.close_error == EIO && !b.first_failure.descriptor_open && close_calls == calls ? 0 : 91);
    }
    int status{};
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "consumed close error remains owned and no retry closes the old descriptor number");
}

void Owners() {
    std::shared_ptr<Module> retained;
    std::array<void*, 8> addresses{};
    std::optional<NodeRef> external_function;
    // Function owns constituent blocks by deletion. Observe a block under the
    // retained parent pin; an independent NodeRef would delete a linked child.
    Block* external_block{};
    {
        AddressSpace space{TestConfig()};
        auto default_module = space.GetDefaultModule();
        auto unmapped = space.MapModule(0x1000, 0x2000, ModuleConfig{});
        auto covered = space.MapModule(0x3000, 0x4000, ModuleConfig{});
        retained = space.MapModule(0x3000, 0x4000, ModuleConfig{});
        space.UnmapModule(0x1000, 0x2000);
        Check(space.GetOwnedModuleCount() == 4 && space.GetMappedModules().size() == 1,
              "terminal registry includes default/unmapped/covered owners independently of routing");
        const std::array modules{default_module, unmapped, covered, retained};
        for (size_t i = 0; i < modules.size(); ++i) {
            const auto [id, code] = modules[i]->AllocCodeCache(64);
            addresses[i * 2] = code.exec_data; addresses[i * 2 + 1] = code.rw_data;
            modules[i]->AddFaultEntry(code.exec_data, code.exec_data + code.size, 0x100 + i);
            Check(id != INVALID_CACHE_ID && Mapped(code.exec_data) && Mapped(code.rw_data),
                  "each owner has actual executable and writable views");
            if (i == 3) {
                auto* function = new Function(Location{0x3100});
                auto* block = new Block(Location{0x3100});
                function->AddBlock(block);
                function->GetJitCache().jit_state = JitState::Queue;
                function->GetJitCache().cache_size = 31;
                block->GetJitCache().jit_state = JitState::Cached;
                block->GetJitCache().cache_id = id;
                block->GetJitCache().offset_in = code.offset;
                block->GetJitCache().cache_size = code.size;
                external_function.emplace(function); external_block = block;
                Check(modules[i]->Push(function), "retain function and constituent block publication");
                modules[i]->RetainCodeIR(code.exec_data, function);
                space.PushCodeCache(Location{0x3100}, code.exec_data);
                space.PushCallCodeCache(Location{0x3100}, code.exec_data);
                space.PushPendingCallCodeCache(Location{0x3100}, code.exec_data);
            }
        }
        const auto ticket = Terminal(space);
        deny_allocations = true;
        const auto result = space.TryShutdown(ticket);
        deny_allocations = false;
        Check(result.Complete() && space.GetOwnedModuleCount() == 0, "allocation-free full shutdown releases every owner");
        for (const auto pointer : addresses) Check(!Mapped(pointer), "all owner RX/RW views are actually absent");
        Check(space.GetCodeCacheTable().Lookup(0x3100) == 0 && space.GetCallCodeCacheTable().Lookup(0x3100) == 0 &&
              space.GetPendingCallCodeCacheTable().Lookup(0x3100) == 0, "dispatch/call/pending no longer route to code");
        auto* function = static_cast<Function*>(external_function->Get());
        auto* block = external_block;
        Check(function->GetJitCache().jit_state.get<JitState>() == JitState::None &&
              *function->GetJitCache().cache_size == 0 &&
              block->GetJitCache().jit_state.get<JitState>() == JitState::None &&
              *block->GetJitCache().cache_size == 0, "external function and block caches are inert in every state");
    }
    Check(!retained->IsDirectLinkConfigured() && retained->GetJitCache(Location{0x3100}) == nullptr &&
          !retained->GetCodeRegion(static_cast<u8*>(addresses[6])) &&
          retained->GetCodeCache(static_cast<u8*>(addresses[6])) == nullptr,
          "retained Module exposes no callable address after AddressSpace destruction");
    bool rejected{};
    try { (void)retained->GetDispatchIndex(Location{0x3100}); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "retained Module refuses borrowed AddressSpace access");
    retained->DiscardLinkSource(addresses[6]);
}

void Readers() {
    AddressSpace space{TestConfig()};
    auto module = space.GetDefaultModule();
    const auto [id, code] = module->AllocCodeCache(64);
    module->AddFaultEntry(code.exec_data, code.exec_data + 64, 0x1000);
    auto alias = std::make_shared<CounterAlias>();
    Check(space.RegisterTerminalAlias(alias), "register reader-grace alias");
    std::optional<LinkManager::SignalReader> link{std::in_place, space.GetLinkManager()};
    std::optional<FaultCatalog::Reader> fault{std::in_place, *space.GetFaultCatalog()};
    const auto ticket = Terminal(space);
    auto result = space.TryShutdown(ticket);
    Check(result.stage == AddressSpace::ShutdownStage::LinkReaders && alias->calls == 1 && Mapped(code.exec_data),
          "link reader prevents any physical mapping release");
    link.reset();
    result = space.TryShutdown(ticket);
    Check(result.stage == AddressSpace::ShutdownStage::FaultReaders && alias->calls == 1 && Mapped(code.exec_data),
          "fault reader prevents release after link grace without repeating aliases");
    fault.reset();
    Check(space.TryShutdown(ticket).Complete() && alias->calls == 1 && !Mapped(code.exec_data),
          "reader exits permit actual physical release");
}

void Retry(bool writable) {
    AddressSpace space{TestConfig()};
    auto module = space.GetDefaultModule();
    const auto [id, code] = module->AllocCodeCache(64);
    const auto region = *module->GetCodeRegion(code.exec_data);
    auto alias = std::make_shared<CounterAlias>();
    Check(space.RegisterTerminalAlias(alias), "register release retry alias");
    failing_unmap = writable ? region.rw_base : region.rx_base;
    unmap_budget = 2;
    const auto ticket = Terminal(space);
    const unsigned before = arena_destroys;
    auto result = space.TryShutdown(ticket);
    Check(result.stage == AddressSpace::ShutdownStage::Modules && result.mapping.unmap_error == EIO &&
          result.failed_module == module.get() && space.GetOwnedModuleCount() == 1 && alias->calls == 1,
          "first failed view remains attached to the original owner");
    const unsigned once = arena_destroys;
    Check(once == before + 1 && Mapped(failing_unmap) && !module->GetCodeRegion(code.exec_data),
          "arena is destroyed once and failed Module is inert");
    result = space.TryShutdown(ticket);
    Check(!result.Complete() && result.mapping.unmap_error == EIO && arena_destroys == once && Mapped(failing_unmap),
          "persistent failed view is retried without rebuilding or re-destroying arena");
    Check(space.TryShutdown(ticket).Complete() && !Mapped(region.rx_base) && !Mapped(region.rw_base) &&
          alias->calls == 1, "final retry releases both views and clears registry");
    failing_unmap = nullptr;
}

void Smc() {
    GuestWindow guest;
    AddressSpace space{TestConfig(guest.memory)};
    auto module = space.GetDefaultModule();
    const auto [id, code] = module->AllocCodeCache(64);
    auto* block = new Block(Location{0});
    NodeRef external{block};
    block->SetEndLocation(Location{2 * static_cast<u64>(getpagesize())});
    Check(module->Push(block), "publish protected guest node");
    space.GetSmcTracker().RegisterNode(module, block, 0, 2 * static_cast<u64>(getpagesize()));
    auto alias = std::make_shared<CounterAlias>();
    Check(space.RegisterTerminalAlias(alias), "register unprotect gate alias");
    failing_unprotect = static_cast<u8*>(guest.memory) + getpagesize();
    unprotect_budget = 2;
    const auto ticket = Terminal(space);
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const auto result = space.TryShutdown(ticket);
        Check(result.stage == AddressSpace::ShutdownStage::Smc && result.smc.protect_error == EACCES &&
              result.smc.remaining_pages == 1 && alias->calls == 0 && Mapped(code.exec_data),
              "failed unprotect stops alias revocation and all mapping release");
    }
    Check(space.TryShutdown(ticket).Complete() && alias->calls == 1 && !Mapped(code.exec_data),
          "unprotect retry permits the remaining shutdown transaction");
    static_cast<u8*>(guest.memory)[getpagesize()] = 17;
    Check(static_cast<u8*>(guest.memory)[getpagesize()] == 17, "guest page is actually writable after release");
    failing_unprotect = nullptr;
}

void CloseFailure() {
    const pid_t child = fork();
    Check(child >= 0, "fork persistent-close diagnostic");
    if (child == 0) {
        try {
            AddressSpace space{TestConfig()};
            auto module = space.GetDefaultModule();
            (void)module->AllocCodeCache(64);
            const auto ticket = Terminal(space);
            close_budget = 1;
            const auto first = space.TryShutdown(ticket);
            const auto retry = space.TryShutdown(ticket);
            if (first.stage != AddressSpace::ShutdownStage::Modules || first.mapping.close_error != EIO ||
                retry.mapping.close_error != EIO || retry.Complete() || space.GetOwnedModuleCount() != 1) _exit(93);
            std::set_terminate([] { _exit(77); });
        } catch (...) { _exit(94); }
        _exit(95);
    }
    int status{};
    Check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77,
          "sticky close failure remains owned and destructor refuses false success");
}

aot::AotImage Artifact(const Config& config, void* guest, bool complete_entries = false) {
    aot::AotImage image{};
    image.host_isa = config.backend_isa;
    image.guest.entry = 0x100;
    image.key = {kCacheFormatVersion, ComputeBuildId(), ComputeConfigHash(config), ComputeEnvHash(),
                 aot::HashGuestImage(image.segments, image.guest.entry)};
    AddressSpace source{config};
    source.SetSerializableCodeEmission();
    HIRBuilder builder{1, true, ResolveFeatureSet(source.GetDefaultModule()->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{0x100}, Location{0x104});
    if (complete_entries) {
        const auto value = function->Add(ValueType::U64,
                                         function->LoadUniform(Uniform{8, ValueType::U64}),
                                         Operand{function->LoadImm(ValueType::U64, Imm{u64{37}})});
        function->SaveFlags(value, Flags::All);
        function->StoreUniform(Uniform{0, ValueType::U64}, value);
    } else {
        function->StoreUniform(Uniform{0, ValueType::U64},
                               function->LoadImm(ValueType::U64, Imm{u64{37}}));
    }
    function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
    auto* entry = static_cast<u8*>(TranslateIR(source.GetDefaultModule(), function));
    FaultEntry allocation;
    std::vector<FaultEntry> faults;
    Check(entry && source.GetDefaultModule()->LookupCodeAllocation(entry, allocation, &faults),
          "capture actual serializable production code");
    image.code.assign(allocation.host_start, allocation.host_end);
    aot::AotUnit unit{};
    unit.guest_start = 0x100; unit.code_size = image.code.size();
    for (const auto& fault : faults) unit.fault_sites.push_back({fault.guest_loc,
        static_cast<u32>(fault.host_start - allocation.host_start),
        static_cast<u32>(fault.host_end - allocation.host_start),
        fault.recovery ? static_cast<u32>(fault.recovery - allocation.host_start) : UINT32_MAX,
        static_cast<u8>(fault.recovery_kind)});
    if (complete_entries) {
        const auto cache = source.GetDefaultModule()->GetCodeCache(allocation.host_start);
        Check(cache && FreezeRegionBranches(*cache, allocation.host_start, image.code, unit.region_branches),
              "freeze actual shared region branches before scanning the linked entry fixture");
    }
    const auto scan = ScanCodeUnit(image.code,
                                   GetHostImage(),
                                   config.loc_end,
                                   {},
                                   config.backend_isa,
                                   unit.region_branches);
    Check(scan.ok, "actual production artifact has valid relocations");
    unit.relocs = scan.relocs;
    unit.blocks.push_back({0x100, 0x104, static_cast<u32>(entry - allocation.host_start),
        HashBytes(static_cast<const u8*>(guest) + 0x100, 4, 0xCBF29CE484222325ull)});
    if (complete_entries) {
        const auto target = source.GetLinkManager().QueryTarget(0x100);
        Check(target && target->host_pc == source.GetCodeCache(Location{0x100}),
              "capture the real canonical linked entry before admission testing");
        const auto offset = [&](const void* pointer) -> u32 {
            if (!pointer) return UINT32_MAX;
            const auto* address = static_cast<const u8*>(pointer);
            Check(address >= allocation.host_start && address < allocation.host_end,
                  "each captured entry belongs to the actual production allocation");
            return static_cast<u32>(address - allocation.host_start);
        };
        auto& block = unit.blocks.front();
        block.code_offset = offset(target->host_pc);
        block.direct_code_offset = offset(target->direct_host_pc);
        block.pending_flags_code_offset = offset(target->pending_flags_host_pc);
        block.continuation_code_offset = offset(target->call_host_pc);
        block.pending_flags_continuation_code_offset = offset(target->call_pending_flags_host_pc);
        block.pending_flags_contract = target->pending_flags_contract;
        unit.is_function = 1;
        Check(block.ValidEntryContract(unit.code_size), "captured linked entries have valid persistence contracts");
    }
    image.units.push_back(std::move(unit));
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    return image;
}

u64 DualExecutorCallback() noexcept;
u64 LinkedExecutorCallback() noexcept;

aot::AotImage BlockingTargetArtifact(const Config& config, void* guest) {
    aot::AotImage image{};
    image.host_isa = config.backend_isa;
    image.guest.entry = 0x100;
    image.key = {kCacheFormatVersion, ComputeBuildId(), ComputeConfigHash(config), ComputeEnvHash(),
                 aot::HashGuestImage(image.segments, image.guest.entry)};
    AddressSpace source{config};
    source.SetSerializableCodeEmission();
    HIRBuilder builder{1, true, ResolveFeatureSet(source.GetDefaultModule()->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{0x100}, Location{0x104});
    const auto callback = static_cast<u64>(reinterpret_cast<uintptr_t>(
            FptrCast(static_cast<u64 (*)()>(&DualExecutorCallback))));
    const auto called = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{callback}});
    function->StoreUniform(Uniform{24, ValueType::U64}, called);
    const auto value = function->Add(ValueType::U64,
                                     function->LoadUniform(Uniform{8, ValueType::U64}),
                                     Operand{function->LoadImm(ValueType::U64, Imm{u64{37}})});
    function->SaveFlags(value, Flags::All);
    function->StoreUniform(Uniform{0, ValueType::U64}, value);
    function->EndBlock(terminal::ReturnToHost{}); function->EndFunction();
    auto* entry = static_cast<u8*>(TranslateIR(source.GetDefaultModule(), function));
    FaultEntry allocation;
    std::vector<FaultEntry> faults;
    Check(entry && source.GetDefaultModule()->LookupCodeAllocation(entry, allocation, &faults),
          "capture actual serializable blocking target code");
    image.code.assign(allocation.host_start, allocation.host_end);
    aot::AotUnit unit{};
    unit.guest_start = 0x100; unit.code_size = image.code.size();
    for (const auto& fault : faults) unit.fault_sites.push_back({fault.guest_loc,
        static_cast<u32>(fault.host_start - allocation.host_start),
        static_cast<u32>(fault.host_end - allocation.host_start),
        fault.recovery ? static_cast<u32>(fault.recovery - allocation.host_start) : UINT32_MAX,
        static_cast<u8>(fault.recovery_kind)});
    const auto cache = source.GetDefaultModule()->GetCodeCache(allocation.host_start);
    Check(cache && FreezeRegionBranches(*cache, allocation.host_start, image.code, unit.region_branches),
          "freeze actual shared region branches before scanning the blocking target fixture");
    const auto scan = ScanCodeUnit(image.code,
                                   GetHostImage(),
                                   config.loc_end,
                                   {},
                                   config.backend_isa,
                                   unit.region_branches);
    Check(scan.ok, "actual blocking target artifact has valid relocations");
    unit.relocs = scan.relocs;
    unit.blocks.push_back({0x100, 0x104, static_cast<u32>(entry - allocation.host_start),
        HashBytes(static_cast<const u8*>(guest) + 0x100, 4, 0xCBF29CE484222325ull)});
    const auto target = source.GetLinkManager().QueryTarget(0x100);
    Check(target && target->host_pc == source.GetCodeCache(Location{0x100}),
          "capture the real canonical blocking target entry");
    const auto offset = [&](const void* pointer) -> u32 {
        if (!pointer) return UINT32_MAX;
        const auto* address = static_cast<const u8*>(pointer);
        Check(address >= allocation.host_start && address < allocation.host_end,
              "each captured blocking target entry belongs to the actual production allocation");
        return static_cast<u32>(address - allocation.host_start);
    };
    auto& block = unit.blocks.front();
    block.code_offset = offset(target->host_pc);
    block.direct_code_offset = offset(target->direct_host_pc);
    block.pending_flags_code_offset = offset(target->pending_flags_host_pc);
    block.continuation_code_offset = offset(target->call_host_pc);
    block.pending_flags_continuation_code_offset = offset(target->call_pending_flags_host_pc);
    block.pending_flags_contract = target->pending_flags_contract;
    unit.is_function = 1;
    Check(block.ValidEntryContract(unit.code_size), "captured blocking target entries have valid persistence contracts");
    image.units.push_back(std::move(unit));
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    return image;
}

aot::AotImage ReturningTargetArtifact(const Config& config, void* guest) {
    aot::AotImage image{};
    image.host_isa = config.backend_isa;
    image.guest.entry = 0x100;
    image.key = {kCacheFormatVersion, ComputeBuildId(), ComputeConfigHash(config), ComputeEnvHash(),
                 aot::HashGuestImage(image.segments, image.guest.entry)};
    AddressSpace source{config};
    source.SetSerializableCodeEmission();
    HIRBuilder builder{1, true, ResolveFeatureSet(source.GetDefaultModule()->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{0x100}, Location{0x104});
    const auto value = function->Add(ValueType::U64,
                                     function->LoadUniform(Uniform{8, ValueType::U64}),
                                     Operand{function->LoadImm(ValueType::U64, Imm{u64{37}})});
    function->SaveFlags(value, Flags::All);
    function->StoreUniform(Uniform{0, ValueType::U64}, value);
    function->AdvancePC(Imm{u64{1}});
    auto* tail = builder.LinkBlock(terminal::LinkBlock{Location{0x102}});
    builder.SetCurBlock(tail);
    const auto resume = function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{16, ValueType::U64});
    function->SetLocation(Lambda{resume});
    function->EndBlock(terminal::PopRSBHint{});
    function->EndFunction();
    auto* entry = static_cast<u8*>(TranslateIR(source.GetDefaultModule(), function));
    FaultEntry allocation;
    std::vector<FaultEntry> faults;
    Check(entry && source.GetDefaultModule()->LookupCodeAllocation(entry, allocation, &faults),
          "capture actual serializable returning target code");
    image.code.assign(allocation.host_start, allocation.host_end);
    aot::AotUnit unit{};
    unit.guest_start = 0x100; unit.code_size = image.code.size();
    for (const auto& fault : faults) unit.fault_sites.push_back({fault.guest_loc,
        static_cast<u32>(fault.host_start - allocation.host_start),
        static_cast<u32>(fault.host_end - allocation.host_start),
        fault.recovery ? static_cast<u32>(fault.recovery - allocation.host_start) : UINT32_MAX,
        static_cast<u8>(fault.recovery_kind)});
    const auto cache = source.GetDefaultModule()->GetCodeCache(allocation.host_start);
    Check(cache && FreezeRegionBranches(*cache, allocation.host_start, image.code, unit.region_branches),
          "freeze actual shared region branches before scanning the returning target fixture");
    const auto scan = ScanCodeUnit(image.code,
                                   GetHostImage(),
                                   config.loc_end,
                                   {},
                                   config.backend_isa,
                                   unit.region_branches);
    Check(scan.ok, "actual returning target artifact has valid relocations");
    unit.relocs = scan.relocs;
    unit.blocks.push_back({0x100, 0x104, static_cast<u32>(entry - allocation.host_start),
        HashBytes(static_cast<const u8*>(guest) + 0x100, 4, 0xCBF29CE484222325ull)});
    const auto target = source.GetLinkManager().QueryTarget(0x100);
    Check(target && target->host_pc == source.GetCodeCache(Location{0x100}),
          "capture the real canonical returning target entry");
    const auto offset = [&](const void* pointer) -> u32 {
        if (!pointer) return UINT32_MAX;
        const auto* address = static_cast<const u8*>(pointer);
        Check(address >= allocation.host_start && address < allocation.host_end,
              "each captured returning target entry belongs to the actual production allocation");
        return static_cast<u32>(address - allocation.host_start);
    };
    auto& block = unit.blocks.front();
    block.code_offset = offset(target->host_pc);
    block.direct_code_offset = offset(target->direct_host_pc);
    block.pending_flags_code_offset = offset(target->pending_flags_host_pc);
    block.continuation_code_offset = offset(target->call_host_pc);
    block.pending_flags_continuation_code_offset = offset(target->call_pending_flags_host_pc);
    block.pending_flags_contract = target->pending_flags_contract;
    unit.is_function = 1;
    Check(block.ValidEntryContract(unit.code_size), "captured returning target entries have valid persistence contracts");
    image.units.push_back(std::move(unit));
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    return image;
}

aot::AotImage BlockingReturningTargetArtifact(const Config& config, void* guest) {
    Check(config.uniform_buffer_size >= kReturningCallUniformSize,
          "blocking returning target has space for its callback result at offset 32");
    aot::AotImage image{};
    image.host_isa = config.backend_isa;
    image.guest.entry = 0x100;
    image.key = {kCacheFormatVersion, ComputeBuildId(), ComputeConfigHash(config), ComputeEnvHash(),
                 aot::HashGuestImage(image.segments, image.guest.entry)};
    AddressSpace source{config};
    source.SetSerializableCodeEmission();
    HIRBuilder builder{1, true, ResolveFeatureSet(source.GetDefaultModule()->GetModuleConfig())};
    auto* function = builder.AppendFunction(Location{0x100}, Location{0x104});
    const auto callback = static_cast<u64>(reinterpret_cast<uintptr_t>(
            FptrCast(static_cast<u64 (*)()>(&LinkedExecutorCallback))));
    const auto called = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{callback}});
    function->StoreUniform(Uniform{kCallCallbackOffset, ValueType::U64}, called);
    const auto value = function->Add(ValueType::U64,
                                     function->LoadUniform(Uniform{8, ValueType::U64}),
                                     Operand{function->LoadImm(ValueType::U64, Imm{u64{37}})});
    function->SaveFlags(value, Flags::All);
    function->StoreUniform(Uniform{0, ValueType::U64}, value);
    function->AdvancePC(Imm{u64{1}});
    auto* tail = builder.LinkBlock(terminal::LinkBlock{Location{0x102}});
    builder.SetCurBlock(tail);
    const auto resume = function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{16, ValueType::U64});
    function->SetLocation(Lambda{resume});
    function->EndBlock(terminal::PopRSBHint{});
    function->EndFunction();
    auto* entry = static_cast<u8*>(TranslateIR(source.GetDefaultModule(), function));
    FaultEntry allocation;
    std::vector<FaultEntry> faults;
    Check(entry && source.GetDefaultModule()->LookupCodeAllocation(entry, allocation, &faults),
          "capture actual serializable blocking returning target code");
    image.code.assign(allocation.host_start, allocation.host_end);
    aot::AotUnit unit{};
    unit.guest_start = 0x100; unit.code_size = image.code.size();
    for (const auto& fault : faults) unit.fault_sites.push_back({fault.guest_loc,
        static_cast<u32>(fault.host_start - allocation.host_start),
        static_cast<u32>(fault.host_end - allocation.host_start),
        fault.recovery ? static_cast<u32>(fault.recovery - allocation.host_start) : UINT32_MAX,
        static_cast<u8>(fault.recovery_kind)});
    const auto cache = source.GetDefaultModule()->GetCodeCache(allocation.host_start);
    Check(cache && FreezeRegionBranches(*cache, allocation.host_start, image.code, unit.region_branches),
          "freeze actual shared region branches before scanning the blocking returning target fixture");
    const auto scan = ScanCodeUnit(image.code,
                                   GetHostImage(),
                                   config.loc_end,
                                   {},
                                   config.backend_isa,
                                   unit.region_branches);
    Check(scan.ok, "actual blocking returning target artifact has valid relocations");
    unit.relocs = scan.relocs;
    unit.blocks.push_back({0x100, 0x104, static_cast<u32>(entry - allocation.host_start),
        HashBytes(static_cast<const u8*>(guest) + 0x100, 4, 0xCBF29CE484222325ull)});
    const auto target = source.GetLinkManager().QueryTarget(0x100);
    Check(target && target->host_pc == source.GetCodeCache(Location{0x100}),
          "capture the real canonical blocking returning target entry");
    const auto offset = [&](const void* pointer) -> u32 {
        if (!pointer) return UINT32_MAX;
        const auto* address = static_cast<const u8*>(pointer);
        Check(address >= allocation.host_start && address < allocation.host_end,
              "each captured blocking returning target entry belongs to the actual production allocation");
        return static_cast<u32>(address - allocation.host_start);
    };
    auto& block = unit.blocks.front();
    block.code_offset = offset(target->host_pc);
    block.direct_code_offset = offset(target->direct_host_pc);
    block.pending_flags_code_offset = offset(target->pending_flags_host_pc);
    block.continuation_code_offset = offset(target->call_host_pc);
    block.pending_flags_continuation_code_offset = offset(target->call_pending_flags_host_pc);
    block.pending_flags_contract = target->pending_flags_contract;
    unit.is_function = 1;
    Check(block.ValidEntryContract(unit.code_size), "captured blocking returning target entries have valid persistence contracts");
    image.units.push_back(std::move(unit));
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    return image;
}

std::pair<aot::AotImage, SerialUnit> RestoredCallFixture(const Config& config, void* guest,
        const aot::AotImage& target_image, VAddr target_pc, VAddr source_pc, VAddr return_pc) {
    auto image = target_image;
    SerialUnit captured;
    {
        AddressSpace source{config};
        source.SetSerializableCodeEmission();
        const auto module = source.GetDefaultModule();
        {
            HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
            auto* function = builder.AppendFunction(Location{target_pc}, Location{target_pc + 4});
            const auto value =
                    function->Add(ValueType::U64,
                                  function->LoadUniform(Uniform{8, ValueType::U64}),
                                  Operand{function->LoadImm(ValueType::U64, Imm{u64{37}})});
            function->SaveFlags(value, Flags::All);
            function->StoreUniform(Uniform{0, ValueType::U64}, value);
            function->AdvancePC(Imm{u64{1}});
            auto* tail = builder.LinkBlock(terminal::LinkBlock{Location{target_pc + 2}});
            builder.SetCurBlock(tail);
            const auto resume = function->LoadUniform<TypedValue<ValueType::U64>>(Uniform{16, ValueType::U64});
            function->SetLocation(Lambda{resume});
            function->EndBlock(terminal::PopRSBHint{});
            function->EndFunction();
            Check(TranslateIR(module, function) != nullptr, "compile the returning target in the Call capture space");
        }
        HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
        auto* function = builder.AppendFunction(Location{source_pc}, Location{return_pc + 4});
        function->StoreUniform(Uniform{24, ValueType::U64},
                               function->LoadImm(ValueType::U64, Imm{u64{0}}));
        const auto left = function->LoadImm(ValueType::U64, Imm{~u64{0}});
        const auto one = function->LoadImm(ValueType::U64, Imm{u64{1}});
        const auto sum = function->Add(ValueType::U64, left, Operand{one});
        function->SaveFlags(sum, Flags::All);
        const auto ret = function->LoadImm(ValueType::U64, Imm{return_pc});
        function->CallReturn(ret, Imm{return_pc}, Lambda{Imm{target_pc}});
        function->SetLocation(Lambda{Imm{target_pc}});
        builder.RegisterCallReturn(Location{return_pc});
        function->EndBlock(terminal::ReturnToDispatch{});
        builder.SetCurBlock(Location{return_pc});
        function->StoreUniform(Uniform{24, ValueType::U64},
                               function->LoadImm(ValueType::U64, Imm{u64{0x51ab}}));
        function->AdvancePC(Imm{u64{1}});
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        auto* entry = static_cast<u8*>(TranslateIR(module, function));
        FaultEntry extent;
        Check(entry && module->LookupCodeAllocation(entry, extent), "compile the actual outgoing Call source allocation");
        std::string error;
        Check(CaptureSerialCode(*module, extent.host_start, u32(extent.host_end - extent.host_start),
              GetHostImage(), config.loc_end, captured, &error), error.c_str());
        Check(captured.link_sites.size() == 1 &&
              captured.link_sites.front().kind == u8(FlagsRegsEnabled() ? LinkSiteKind::Call : LinkSiteKind::Unconditional),
              "capture a real cross-unit static Call site");
        const auto node = module->GetNode(Location{source_pc});
        Check(IsFunction(node), "retain the captured Call function owner");
        const auto compiled = GetFunction(node);
        const auto offset = [&](const void* pointer) -> u32 {
            if (!pointer) return UINT32_MAX;
            const auto* address = static_cast<const u8*>(pointer);
            Check(address >= extent.host_start && address < extent.host_end, "captured Call entry belongs to its allocation");
            return u32(address - extent.host_start);
        };
        for (const auto& member : compiled->GetBlocks()) {
            const auto start = member.GetStartLocation().Value();
            const auto end = std::max(start + 1, member.GetEndLocation().Value());
            SerialBlock block{start, end, UINT32_MAX,
                HashBytes(static_cast<const u8*>(guest) + start, end - start, 0xCBF29CE484222325ull)};
            if (const auto member_target = source.GetLinkManager().QueryTarget(start)) {
                block.code_offset = offset(member_target->host_pc);
                block.direct_code_offset = offset(member_target->direct_host_pc);
                block.pending_flags_code_offset = offset(member_target->pending_flags_host_pc);
                block.pending_flags_contract = member_target->pending_flags_contract;
                block.continuation_code_offset = offset(member_target->call_host_pc);
                block.pending_flags_continuation_code_offset = offset(member_target->call_pending_flags_host_pc);
            } else if (auto* block_entry = source.GetCodeCache(Location{start})) {
                block.code_offset = offset(block_entry);
            } else block.entry_flags = SerialBlock::MetadataOnly;
            Check(block.ValidEntryContract(captured.code.size()), "capture every actual Call entry contract");
            captured.blocks.push_back(block);
        }
        source.GetCodeCacheTable().ForEachEntry([&](u32 index, size_t key, size_t) {
            image.dispatch_slots.emplace_back(u64(key), index);
        });
    }
    captured.guest_start = source_pc; captured.is_function = 1;
    captured.fault_sites.push_back({source_pc, 0, u32(captured.code.size())});
    aot::AotUnit unit{};
    unit.guest_start = source_pc; unit.code_size = captured.code.size(); unit.is_function = 1;
    unit.blocks = captured.blocks; unit.relocs = captured.relocs; unit.fault_sites = captured.fault_sites;
    unit.link_sites = captured.link_sites; unit.region_branches = captured.region_branches;
    image.units = {unit}; image.code = captured.code;
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    return {std::move(image), std::move(captured)};
}

void AotOwners() {
    GuestWindow guest, second_guest;
    const auto config = TestConfig(guest.memory);
    const auto second_config = TestConfig(second_guest.memory);
    const auto image = Artifact(config, guest.memory);
    const auto second_image = Artifact(second_config, second_guest.memory);
    auto first = std::make_unique<AddressSpace>(config);
    auto second = std::make_unique<AddressSpace>(second_config);
    aot::LoadReport a, b;
    std::string error;
    Check(aot::InstallArtifact(*first, image, guest.memory, config.guest_addr_mask, a, error) == aot::kAotOk,
          "install actual artifact in first owner");
    Check(aot::InstallArtifact(*second, second_image, second_guest.memory, second_config.guest_addr_mask, b, error) == aot::kAotOk,
          "install same offsets in second owner");
    auto* host_a = aot::ResolveCodeOffset(a.mapping, 0);
    auto* host_b = aot::ResolveCodeOffset(b.mapping, 0);
    Check(host_a && host_b && host_a != host_b && aot::ResolveCodeOffset(0) == host_b,
          "explicit owner maps remain distinct while compatibility lookup selects latest");
    for (auto* owner : {first.get(), second.get()}) {
        Runtime runtime{owner};
        runtime.SetLocation(0x100);
        (void)runtime.Run();
        u64 value{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
        Check(value == 37, "each independent owner executes actual restored artifact code");
    }
    auto stale = second_image;
    stale.key.format_version = 25;
    aot::LoadReport rejected;
    Check(aot::InstallArtifact(*second, stale, second_guest.memory, second_config.guest_addr_mask, rejected, error) == aot::kAotKeyMismatch &&
          aot::ResolveCodeOffset(0) == host_b && aot::ResolveCodeOffset(a.mapping, 0) == host_a,
          "rejected artifact preserves both accepted installation maps");
    first.reset();
    Check(!aot::ResolveCodeOffset(a.mapping, 0) && !Mapped(host_a) &&
          aot::ResolveCodeOffset(b.mapping, 0) == host_b && aot::ResolveCodeOffset(0) == host_b,
          "first-owner teardown cannot revoke second-owner offsets");
    second.reset();
    Check(!aot::ResolveCodeOffset(b.mapping, 0) && !aot::ResolveCodeOffset(0) && !Mapped(host_b),
          "retained maps cannot expose released code and compatibility map never falls back");
}

struct SymbolArtifact {
    std::string directory, guest_path, artifact_path;
    aot::AotImage image;

    explicit SymbolArtifact(aot::AotImage compiled) {
#ifdef ANDROID
        char name[] = "/data/local/tmp/swiftvm-shutdown-symbols-XXXXXX";
#else
        char name[] = "/tmp/swiftvm-shutdown-symbols-XXXXXX";
#endif
        Check(mkdtemp(name) != nullptr, "create an owned symbol-artifact directory");
        directory = name;
        guest_path = directory + "/guest.elf";
        artifact_path = directory + "/guest.aot";
        try {
            using namespace ELFIO;
            elfio guest;
            guest.create(ELFCLASS64, ELFDATA2LSB);
            guest.set_os_abi(ELFOSABI_LINUX);
            guest.set_type(ET_EXEC);
            guest.set_machine(EM_X86_64);
            auto* text = guest.sections.add(".text");
            text->set_type(SHT_PROGBITS);
            text->set_flags(SHF_ALLOC | SHF_EXECINSTR);
            text->set_address(0x100);
            const char bytes[4]{};
            text->set_data(bytes, sizeof(bytes));
            auto* strings = guest.sections.add(".strtab");
            strings->set_type(SHT_STRTAB);
            auto* table = guest.sections.add(".symtab");
            table->set_type(SHT_SYMTAB);
            table->set_link(strings->get_index());
            table->set_entry_size(guest.get_default_entry_size(SHT_SYMTAB));
            table->set_addr_align(8);
            table->set_info(1);
            string_section_accessor names{strings};
            symbol_section_accessor symbols{guest, table};
            for (const char* symbol : {"entry", "alias"})
                symbols.add_symbol(names, symbol, 0x100, 4, STB_GLOBAL, STT_FUNC, 0, text->get_index());
            constexpr unsigned char kGnuIfunc = 10;  // ELF STT_GNU_IFUNC.
            symbols.add_symbol(names, "ifunc", 0x100, 4, STB_GLOBAL, kGnuIfunc, 0, text->get_index());
            symbols.add_symbol(names, "object", 0x100, 4, STB_GLOBAL, STT_OBJECT, 0, text->get_index());
            Check(guest.save(guest_path), "write a real guest symbol table");
            compiled.stub_offset = static_cast<u32>(compiled.code.size());
            const u32 stub[2] = {compiled.host_isa == kRiscv64 ? 0x00100073u : aot::kAotStubInsn, 0u};
            const auto* stub_bytes = reinterpret_cast<const u8*>(stub);
            compiled.code.insert(compiled.code.end(), stub_bytes, stub_bytes + sizeof(stub));
            compiled.code_hash = HashBytes(compiled.code.data(), compiled.code.size(), 0xCBF29CE484222325ull);
            std::string error;
            Check(aot::WriteArtifact(guest_path, compiled, artifact_path, error),
                  "production writer rewrites the guest symbol table");
            Check(aot::ReadArtifact(artifact_path, image, error), error.c_str());
            Check(image.code == compiled.code &&
                  image.units.size() == compiled.units.size() && image.stub_offset == compiled.stub_offset,
                  "production reader accepts the complete serialized artifact");
        } catch (...) {
            Remove();
            throw;
        }
    }
    void Remove() noexcept {
        if (!guest_path.empty()) unlink(guest_path.c_str());
        if (!artifact_path.empty()) unlink(artifact_path.c_str());
        if (!directory.empty()) rmdir(directory.c_str());
    }
    ~SymbolArtifact() { Remove(); }
};

void Symbols() {
    GuestWindow guest, second_guest;
    const auto config = TestConfig(guest.memory), second_config = TestConfig(second_guest.memory);
    SymbolArtifact first_artifact{Artifact(config, guest.memory)};
    SymbolArtifact second_artifact{Artifact(second_config, second_guest.memory)};
    auto first = std::make_unique<AddressSpace>(config);
    auto second = std::make_unique<AddressSpace>(second_config);
    aot::LoadReport a, b;
    std::string error;
    Check(aot::InstallArtifact(*first, first_artifact.image, guest.memory, config.guest_addr_mask, a, error) == aot::kAotOk &&
          aot::InstallArtifact(*second, second_artifact.image, second_guest.memory, second_config.guest_addr_mask, b, error) == aot::kAotOk,
          "install two production-written and read symbol artifacts");
    aot::SymbolIndex first_index, second_index, compatibility;
    Check(first_index.Build(first_artifact.artifact_path, first_artifact.image, error, a.mapping) &&
          second_index.Build(second_artifact.artifact_path, second_artifact.image, error, b.mapping) &&
          compatibility.Build(second_artifact.artifact_path, second_artifact.image, error),
          "build actual SymbolIndex objects with explicit and compatibility owner maps");
    const auto retained = first_index;
    auto* host_a = first_index.Lookup("entry");
    auto* host_b = second_index.Lookup("entry");
    Check(first_index.Size() == 3 && second_index.Size() == 3 && host_a && host_b && host_a != host_b &&
          host_a == aot::ResolveCodeOffset(a.mapping, 0) && host_b == aot::ResolveCodeOffset(b.mapping, 0),
          "rewritten symbols resolve to the exact independent installed entries");
    for (const char* name : {"entry", "alias", "ifunc"})
        Check(first_index.Lookup(name) == host_a && retained.Lookup(name) == host_a &&
              second_index.Lookup(name) == host_b && compatibility.Lookup(name) == host_b,
              "STT_FUNC aliases and rewritten IFUNC retain their selected owner");
    Check(!first_index.Lookup("object") && !first_index.Lookup("missing"),
          "non-function and absent symbols expose no code");
    Check(!first_index.Build(first_artifact.directory + "/missing.elf", first_artifact.image, error, b.mapping) &&
          first_index.Size() == 3 && first_index.Lookup("entry") == host_a,
          "failed symbol rebuild preserves the previously accepted owner binding");
    for (const auto& owner : {std::pair{first.get(), host_a}, std::pair{second.get(), host_b}}) {
        Check(owner.first->GetCodeCacheTable().Lookup(0x100) == reinterpret_cast<size_t>(owner.second),
              "SymbolIndex names the actual Runtime dispatch entry");
        Runtime runtime{owner.first};
        runtime.SetLocation(0x100);
        (void)runtime.Run();
        u64 value{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
        Check(value == 37, "the symbol-bound dispatch entry executes actual restored code");
    }
    std::optional<LinkManager::SignalReader> reader{std::in_place, first->GetLinkManager()};
    const auto ticket = Terminal(*first);
    Check(first->TryShutdown(ticket).stage == AddressSpace::ShutdownStage::LinkReaders && Mapped(host_a),
          "reader grace retains the physical arena after alias revocation");
    for (const char* name : {"entry", "alias", "ifunc"})
        Check(!first_index.Lookup(name) && !retained.Lookup(name) && second_index.Lookup(name) == host_b &&
              compatibility.Lookup(name) == host_b,
              "retained SymbolIndex copies cannot expose revoked code during reader grace");
    aot::SymbolIndex rebuilt;
    Check(rebuilt.Build(first_artifact.artifact_path, first_artifact.image, error, a.mapping) &&
          rebuilt.Size() == 3 && !rebuilt.Lookup("entry"),
          "rebuilding metadata against a retired map cannot revive code");
    reader.reset();
    Check(first->TryShutdown(ticket).Complete() && !Mapped(host_a) && !retained.Lookup("entry") &&
          second_index.Lookup("entry") == host_b && Mapped(host_b),
          "first-owner physical release preserves the other retained index");
    first.reset();
    second.reset();
    Check(!Mapped(host_b) && !first_index.Lookup("entry") && !second_index.Lookup("entry") &&
          !compatibility.Lookup("entry") && !aot::ResolveCodeOffset(0),
          "last-owner release leaves retained symbol indices inert without fallback");
}

bool GuestPageWritable(const void* pointer) {
    auto* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) throw std::runtime_error("open actual process page permissions");
    const auto close = ScopeExit([&] { std::fclose(maps); });
    std::array<char, 1024> line{};
    while (std::fgets(line.data(), line.size(), maps)) {
        unsigned long begin{}, end{};
        std::array<char, 5> permissions{};
        if (std::sscanf(line.data(), "%lx-%lx %4s", &begin, &end, permissions.data()) != 3) continue;
        const auto address = reinterpret_cast<uintptr_t>(pointer);
        if (begin <= address && address < end) return permissions[1] == 'w';
    }
    throw std::runtime_error("guest page missing from actual process maps");
}

void RangeUnprotectRetry(bool terminal) {
    for (unsigned failed_page : {0u, 1u}) {
        GuestWindow guest;
        const auto config = TestConfig(guest.memory);
        const auto page = static_cast<u64>(getpagesize());
        auto image = Artifact(config, guest.memory);
        auto neighbor = image.units.front();
        neighbor.guest_start += 3 * page;
        neighbor.code_offset = image.code.size();
        for (auto& block : neighbor.blocks) { block.guest_start += 3 * page; block.guest_end += 3 * page; }
        for (auto& fault : neighbor.fault_sites) fault.guest_start += 3 * page;
        const auto code = image.code;
        image.code.insert(image.code.end(), code.begin(), code.end());
        SerialBlock dependency{page + 0x100, page + 0x104, UINT32_MAX,
            HashBytes(static_cast<const u8*>(guest.memory) + page + 0x100, 4, 0xCBF29CE484222325ull)};
        dependency.entry_flags = SerialBlock::DependencyOnly;
        image.units.front().blocks.push_back(dependency);
        image.units.push_back(neighbor);
        image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        aot::LoadReport report;
        std::string error;
        Check(aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error) == aot::kAotOk,
              "install two units with actual multi-page code dependency");
        auto* allocation = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, 0));
        auto* adjacent = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, neighbor.code_offset));
        const auto region = module->GetCodeRegion(allocation);
        Check(allocation && adjacent && region, "bind exact source and unaffected neighbor allocations");
        auto* failed_address = static_cast<u8*>(guest.memory) + failed_page * page;
        auto* other_address = static_cast<u8*>(guest.memory) + (1 - failed_page) * page;
        Check(!GuestPageWritable(failed_address) && !GuestPageWritable(other_address),
              "both real dependency pages are protected before range retirement");
        TranslateTable l1{8};
        auto& smc = space.GetSmcTracker();
        auto token = smc.RegisterRuntime(l1);
        const auto unregister = ScopeExit([&] { if (token) smc.UnregisterRuntime(token); });
        SmcTracker::ExecutionScope active{smc, token};
        watched_code_free = SiteRxToRw(*region, allocation);
        watched_code_frees = 0;
        failing_unprotect = failed_address;
        unprotect_budget = 2;
        const auto reset = ScopeExit([] {
            watched_code_free = failing_unprotect = nullptr;
            unprotect_budget = 0;
        });
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            smc.InvalidateRange(space, &l1, 0x100, page + 0x104);
            Check(unprotect_budget == 1 - attempt, "range retry reaches retained protected page");
            Check(!GuestPageWritable(failed_address) && GuestPageWritable(other_address),
                  "failed page stays protected while successful page unprotects independently");
            Check(!aot::ResolveCodeOffset(report.mapping, 0) &&
                  aot::ResolveCodeOffset(report.mapping, neighbor.code_offset) == adjacent,
                  "failed unprotect still revokes affected aliases and preserves neighbor");
            Check(watched_code_frees == 0, "active executor retains retired allocation during unprotect retries");
        }
        active.End();
        Check(watched_code_frees == 1, "executor exit frees retired allocation exactly once");
        smc.UnregisterRuntime(token);
        token.reset();
        if (terminal) {
            const auto ticket = Terminal(space);
            Check(space.TryShutdown(ticket).Complete(), "terminal cleanup retries failed ordinary unprotect");
        } else {
            smc.InvalidateRange(space, &l1, 0x100, page + 0x104);
            Check(aot::ResolveCodeOffset(report.mapping, neighbor.code_offset) == adjacent,
                  "successful range retry preserves adjacent live allocation");
            Runtime runtime{&space};
            runtime.SetLocation(0x100 + 3 * page);
            Check(runtime.Run() == HaltReason::CallHost, "neighbor still executes after range unprotect retry");
            u64 value{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
            Check(value == 37, "neighbor native result survives partial range cleanup");
        }
        Check(GuestPageWritable(failed_address) && GuestPageWritable(other_address),
              "retry actually makes both guest dependency pages writable");
        *failed_address = 17;
        Check(*failed_address == 17 && watched_code_frees == 1, "actual guest write succeeds without double free");
    }
}

void RangeRepublish(bool protection_failure) {
    for (const bool replace_mapping : {false, true}) for (unsigned failed_page : {0u, 1u}) {
        GuestWindow guest;
        const auto config = TestConfig(guest.memory);
        const auto page = static_cast<u64>(getpagesize());
        auto image = Artifact(config, guest.memory);
        auto neighbor = image.units.front();
        neighbor.guest_start += 3 * page;
        neighbor.code_offset = image.code.size();
        for (auto& block : neighbor.blocks) { block.guest_start += 3 * page; block.guest_end += 3 * page; }
        for (auto& fault : neighbor.fault_sites) fault.guest_start += 3 * page;
        const auto code = image.code;
        image.code.insert(image.code.end(), code.begin(), code.end());
        SerialBlock dependency{page + 0x100, page + 0x104, UINT32_MAX,
            HashBytes(static_cast<const u8*>(guest.memory) + page + 0x100, 4, 0xCBF29CE484222325ull)};
        dependency.entry_flags = SerialBlock::DependencyOnly;
        image.units.front().blocks.push_back(dependency);
        image.units.push_back(neighbor);
        image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
        auto replacement = image;
        replacement.units.pop_back();
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        aot::LoadReport old_report;
        std::string error;
        Check(aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, old_report, error) == aot::kAotOk,
              "install real old generation and unaffected neighbor before remap");
        auto* old_allocation = static_cast<u8*>(aot::ResolveCodeOffset(old_report.mapping, 0));
        auto* adjacent = aot::ResolveCodeOffset(old_report.mapping, neighbor.code_offset);
        const auto region = module->GetCodeRegion(old_allocation);
        Check(old_allocation && adjacent && region, "bind old generation and neighbor allocations before remap");
        auto* failed_address = static_cast<u8*>(guest.memory) + failed_page * page;
        auto* other_address = static_cast<u8*>(guest.memory) + (1 - failed_page) * page;
        TranslateTable l1{8};
        auto& smc = space.GetSmcTracker();
        auto token = smc.RegisterRuntime(l1);
        const auto unregister = ScopeExit([&] { if (token) smc.UnregisterRuntime(token); });
        SmcTracker::ExecutionScope active{smc, token};
        watched_code_free = SiteRxToRw(*region, old_allocation);
        watched_code_frees = 0;
        failing_unprotect = failed_address;
        unprotect_budget = 1;
        const auto reset = ScopeExit([] {
            watched_code_free = failing_unprotect = failing_reprotect = nullptr;
            unprotect_budget = reprotect_budget = 0;
        });
        smc.InvalidateRange(space, &l1, 0x100, page + 0x104);
        Check(unprotect_budget == 0 && !GuestPageWritable(failed_address) && GuestPageWritable(other_address),
              "retain exactly one failed dependency page before external replacement");
        Check(!aot::ResolveCodeOffset(old_report.mapping, 0) &&
              aot::ResolveCodeOffset(old_report.mapping, neighbor.code_offset) == adjacent && watched_code_frees == 0,
              "retired generation is revoked and executor-pinned while neighbor stays live");
        if (replace_mapping) {
            Check(mmap(failed_address, page, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == failed_address,
                  "replace retained protected guest mapping with actual writable MAP_FIXED page");
        } else {
            Check(mprotect(failed_address, page, PROT_READ | PROT_WRITE) == 0,
                  "externally change retained guest mapping to actual writable protection");
        }
        Check(GuestPageWritable(failed_address), "external replacement really invalidates cached protection state");
        if (protection_failure) {
            failing_reprotect = failed_address;
            reprotect_budget = 1;
            aot_protection_failure_hit = false;
            aot::LoadReport rejected;
            bool rejected_protection{};
            try {
                (void)aot::InstallArtifact(space, replacement, guest.memory, config.guest_addr_mask, rejected, error);
            } catch (const std::runtime_error& failure) {
                rejected_protection = std::strcmp(failure.what(), "AOT guest write window is open") == 0;
            }
            Check(rejected_protection && reprotect_budget == 0 && aot_protection_failure_hit,
                  "actual failed reprotection prevents new generation publication");
            Check(!rejected.mapping && rejected.units_installed == 0 &&
                  !module->GetJitCache(Location{0x100}) && !space.GetCodeCacheTable().Lookup(0x100),
                  "failed reprotection leaves no callable new generation or dispatch entry");
            Check(GuestPageWritable(failed_address) && !aot::ResolveCodeOffset(old_report.mapping, 0) &&
                  aot::ResolveCodeOffset(old_report.mapping, neighbor.code_offset) == adjacent && watched_code_frees == 0,
                  "reprotection rejection preserves real permissions, old grace and neighbor identity");
        }
        aot::LoadReport current;
        Check(aot::InstallArtifact(space, replacement, guest.memory, config.guest_addr_mask, current, error) == aot::kAotOk,
              "publish replacement generation after external guest mapping change");
        auto* new_allocation = aot::ResolveCodeOffset(current.mapping, 0);
        Check(new_allocation && new_allocation != old_allocation && watched_code_frees == 0,
              "new generation uses distinct storage while old generation is executor-pinned");
        Check(!GuestPageWritable(failed_address) && !GuestPageWritable(other_address),
              "republishing re-protects actual remapped guest dependency pages");
        Check(!aot::ResolveCodeOffset(old_report.mapping, 0) &&
              aot::ResolveCodeOffset(old_report.mapping, neighbor.code_offset) == adjacent,
              "old generation aliases stay revoked after fresh generation commits");
        active.End();
        Check(watched_code_frees == 1, "old generation frees exactly once after executor grace");
        smc.UnregisterRuntime(token);
        token.reset();
        {
            Runtime runtime{&space};
            runtime.SetLocation(0x100);
            Check(runtime.Run() == HaltReason::CallHost, "remapped-page replacement executes actual native code");
            u64 value{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
            Check(value == 37, "replacement generation retains correct native result");
        }
        smc.InvalidateRange(space, &l1, 0x100, page + 0x104);
        smc.InvalidateRange(space, &l1, 0x100, page + 0x104);
        Check(!aot::ResolveCodeOffset(current.mapping, 0) &&
              aot::ResolveCodeOffset(old_report.mapping, neighbor.code_offset) == adjacent,
              "repeated range cleanup revokes replacement without touching neighbor");
        Check(GuestPageWritable(failed_address) && GuestPageWritable(other_address),
              "replacement cleanup restores actual writable dependency pages");
        *failed_address = 23;
        Check(*failed_address == 23 && watched_code_frees == 1,
              "actual write after replacement cleanup succeeds without freeing old generation twice");
    }
}

void RetirementFailureSweep(std::string_view path) {
    GuestWindow guest;
    const auto config = TestConfig(guest.memory);
    const auto image = Artifact(config, guest.memory);
    unsigned injected{};
    const auto attempt = [&](int fail_index) {
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        aot::LoadReport report;
        std::string error;
        Check(aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error) == aot::kAotOk,
              "install real unit for retirement allocation sweep");
        auto* allocation = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, 0));
        const auto region = module->GetCodeRegion(allocation);
        const auto node = module->GetNode(Location{0x100});
        Check(allocation && region && IsFunction(node), "bind actual allocation and retained node");
        TranslateTable l1{8};
        auto& smc = space.GetSmcTracker();
        const auto token = smc.RegisterRuntime(l1);
        const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
        SmcTracker::ExecutionScope active{smc, token};
        watched_code_free = SiteRxToRw(*region, allocation);
        watched_code_frees = 0;
        const auto reset = ScopeExit([] {
            count_retirement_allocations = false;
            fail_retirement_allocation = -1;
            watched_code_free = nullptr;
        });
        const bool write = path == "retire-oom-write";
        if (write)
            Check(smc.HandleWriteFault(space, l1, reinterpret_cast<uintptr_t>(guest.memory) + 0x100),
                  "open real write window before injected close");
        const auto retire = [&] {
            if (path == "retire-oom-node") return smc.RetireNode(space, &l1, module, GetFunction(node).get());
            if (write) smc.CloseWriteWindow(space, l1);
            else smc.InvalidateRange(space, &l1, 0x100, 0x104);
            return true;
        };
        retirement_allocation_index = 0;
        retirement_failure_hit = false;
        fail_retirement_allocation = fail_index;
        count_retirement_allocations = true;
        bool failed{}, retired{};
        try { retired = retire(); }
        catch (const std::bad_alloc&) { failed = true; }
        count_retirement_allocations = false;
        fail_retirement_allocation = -1;
        const auto allocations = retirement_allocation_index;
        Check(failed == retirement_failure_hit, "only the selected allocation failure interrupts retirement");
        if (failed) {
            ++injected;
            Check(watched_code_frees == 0, "failed transaction retains allocation until executor exits");
            retired = retire();
        }
        Check(retired && IsEmpty(module->GetNode(Location{0x100})), "same-path retry detaches the original owner");
        Check(!aot::ResolveCodeOffset(report.mapping, 0) && !space.GetCodeCacheTable().Lookup(0x100),
              "successful retirement revokes retained offsets and dispatch");
        Check(watched_code_frees == 0, "active executor still owns the retired allocation");
        active.End();
        Check(watched_code_frees == 1, "executor exit frees the exact allocation once after failure and retry");
        FaultEntry absent;
        Check(!module->LookupCodeAllocation(allocation, absent), "successful retry purges physical fault ownership");
        (void)retire();
        Check(watched_code_frees == 1, "repeated retirement cannot free twice");
        return allocations;
    };
    const auto allocation_count = attempt(-1);
    Check(allocation_count > 0, "measure actual retirement allocations before sweeping");
    for (unsigned index = 0; index < allocation_count; ++index) attempt(static_cast<int>(index));
    Check(injected == allocation_count, "every observed retirement allocation failure was reached");
    std::printf("retirement_oom path=%s allocations=%u injected=%u retry_frees=%u\n",
                path.data(), allocation_count, injected, injected + 1);
}

void SmcSymbols(std::string_view path) {
    GuestWindow guest;
    const auto config = TestConfig(guest.memory);
    auto image = Artifact(config, guest.memory);
    // RV units require a root at offset zero; the child modes cover their
    // nonzero published entries. Keep a separate interior alias probe here.
    const u32 prefix = config.backend_isa == kRiscv64 ? 0 : 16;
    const u32 probe_offset = prefix ? prefix : sizeof(u32);
    image.code.insert(image.code.begin(), prefix, 0);
    auto& unit = image.units.front();
    unit.code_size += prefix;
    for (auto& block : unit.blocks) if (block.IsPublished()) block.code_offset += prefix;
    for (auto& reloc : unit.relocs) reloc.code_offset += prefix;
    unit.fault_sites = {{unit.guest_start, 0, unit.code_size, UINT32_MAX,
                         static_cast<u8>(FaultRecoveryKind::GuestFault)}};
    auto neighbor = unit;
    const auto page = static_cast<u64>(getpagesize());
    neighbor.guest_start += page;
    neighbor.code_offset = image.code.size();
    for (auto& block : neighbor.blocks) { block.guest_start += page; block.guest_end += page; }
    for (auto& site : neighbor.fault_sites) site.guest_start += page;
    const auto code = image.code;
    image.code.insert(image.code.end(), code.begin(), code.end());
    image.units.push_back(neighbor);
    SymbolArtifact artifact{std::move(image)};
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    aot::LoadReport report;
    std::string error;
    const auto installed = aot::InstallArtifact(space, artifact.image, guest.memory,
                                               config.guest_addr_mask, report, error);
    Check(installed == aot::kAotOk, error.empty() ? "install SMC symbol artifact" : error.c_str());
    aot::SymbolIndex index;
    Check(index.Build(artifact.artifact_path, artifact.image, error, report.mapping),
          "build retained SMC symbol index");
    const auto retained = index;
    auto* allocation = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, 0));
    auto* adjacent = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, neighbor.code_offset));
    const auto region = module->GetCodeRegion(allocation);
    const auto root_offset = artifact.image.units.front().blocks.front().code_offset;
    Check(allocation && adjacent && region && root_offset == prefix &&
          index.Lookup("entry") == allocation + root_offset &&
          module->GetCodeRegion(adjacent)->id == region->id,
          "bind the architecture-valid entry and two distinct allocations in the same arena");
    {
        Runtime runtime{&space};
        runtime.SetLocation(0x100);
        (void)runtime.Run();
        u64 value{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
        Check(value == 37, "execute restored code before ordinary retirement");
    }
    module->RemoveFaultEntries(allocation);
    FaultEntry absent;
    Check(!module->LookupCodeAllocation(allocation + probe_offset, absent),
          "retirement cannot recover the canonical allocation from removed fault metadata");
    TranslateTable l1{8};
    auto& smc = space.GetSmcTracker();
    auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    SmcTracker::ExecutionScope active{smc, token};
    watched_code_free = SiteRxToRw(*region, allocation);
    watched_code_frees = 0;
    const auto stop_watching = ScopeExit([] { watched_code_free = nullptr; });
    const auto node = module->GetNode(Location{0x100});
    Check(IsFunction(node), "bind the actual installed node for ordinary retirement");
    if (path == "symbols-smc") {
        Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()),
              "retire the actual installed node under ordinary SMC");
        Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()),
              "repeated node retirement cannot release twice");
    } else if (path == "symbols-range") {
        smc.InvalidateRange(space, &l1, 0x100, 0x104);
    } else {
        Check(smc.HandleWriteFault(space, l1, reinterpret_cast<uintptr_t>(guest.memory) + 0x100),
              "open the actual guest-page write window");
        Check(!retained.Lookup("entry") && !aot::ResolveCodeOffset(report.mapping, probe_offset),
              "write-fault path revokes aliases before deferred node detach");
        smc.CloseWriteWindow(space, l1);
    }
    Check(!index.Lookup("entry") && !retained.Lookup("entry") &&
          !aot::ResolveCodeOffset(report.mapping, 0) && !aot::ResolveCodeOffset(0),
          "ordinary SMC makes every retained symbol and offset alias inert");
    Check(!aot::ResolveCodeOffset(report.mapping, probe_offset) &&
          !aot::ResolveCodeOffset(report.mapping, code.size() - 1) &&
          aot::ResolveCodeOffset(report.mapping, neighbor.code_offset) == adjacent &&
          aot::ResolveCodeOffset(report.mapping, neighbor.code_offset + probe_offset) == adjacent + probe_offset,
          "all retired interior offsets are inert and the adjacent unit remains live");
    Check(IsEmpty(module->GetNode(Location{0x100})) &&
          !space.GetCodeCacheTable().Lookup(0x100) && watched_code_frees == 0,
          "logical detach precedes allocation free while an SMC executor is active");
    active.End();
    Check(watched_code_frees == 1, "SMC executor exit frees the exact allocation once");
    {
        Runtime runtime{&space};
        runtime.SetLocation(0x100 + page);
        (void)runtime.Run();
        u64 value{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
        Check(value == 37, "the adjacent unit still executes after the affected unit is freed");
    }
    auto replacement = artifact.image;
    replacement.units.resize(1);
    aot::LoadReport fresh;
    Check(aot::InstallArtifact(space, replacement, guest.memory, config.guest_addr_mask,
                               fresh, error) == aot::kAotOk, "reinstall a fresh generation at the same guest location");
    auto* reused = static_cast<u8*>(aot::ResolveCodeOffset(fresh.mapping, 0));
    Check(reused == allocation && !aot::ResolveCodeOffset(report.mapping, probe_offset) &&
          aot::ResolveCodeOffset(fresh.mapping, probe_offset) == reused + probe_offset && !retained.Lookup("entry"),
          "actual allocator address reuse cannot revive a retained old-generation alias");
}

void LatestSmcSymbols() {
    GuestWindow guest, other_guest;
    const auto config = TestConfig(guest.memory), other_config = TestConfig(other_guest.memory);
    SymbolArtifact artifact{Artifact(config, guest.memory)};
    SymbolArtifact other_artifact{Artifact(other_config, other_guest.memory)};
    AddressSpace first{config}, latest{other_config};
    aot::LoadReport a, b;
    std::string error;
    Check(aot::InstallArtifact(first, artifact.image, guest.memory, config.guest_addr_mask, a, error) == aot::kAotOk &&
          aot::InstallArtifact(latest, other_artifact.image, other_guest.memory, other_config.guest_addr_mask,
                               b, error) == aot::kAotOk, "install two independent ordinary-retirement owners");
    aot::SymbolIndex old, newest, compatibility;
    Check(old.Build(artifact.artifact_path, artifact.image, error, a.mapping) &&
          newest.Build(other_artifact.artifact_path, other_artifact.image, error, b.mapping) &&
          compatibility.Build(other_artifact.artifact_path, other_artifact.image, error),
          "retain explicit and latest-owner symbol bindings before ordinary retirement");
    auto* host = old.Lookup("entry");
    auto module = latest.GetDefaultModule();
    const auto node = module->GetNode(Location{0x100});
    Check(IsFunction(node) && latest.GetSmcTracker().RetireNode(latest, nullptr, module, GetFunction(node).get()),
          "retire only the newest owner while the older owner remains live");
    Check(host && old.Lookup("entry") == host && aot::ResolveCodeOffset(a.mapping, 0) &&
          !newest.Lookup("entry") && !compatibility.Lookup("entry") && !aot::ResolveCodeOffset(0),
          "compatibility lookup never falls back from a retired latest owner to an older live owner");
}

void PublicationSmcSymbols() {
    GuestWindow guest;
    const auto config = TestConfig(guest.memory);
    SymbolArtifact artifact{Artifact(config, guest.memory)};
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    aot::LoadReport report;
    std::string error;
    int status = -1;
    std::exception_ptr install_error, invalidate_error;
    std::thread publisher, invalidator;
    pause_protection = guest.memory;
    protection_paused = finish_protection = false;
    const auto cleanup = ScopeExit([&] {
        finish_protection = true;
        if (publisher.joinable()) publisher.join();
        if (invalidator.joinable()) invalidator.join();
        pause_protection = nullptr;
        watched_code_free = nullptr;
    });
    publisher = std::thread([&] {
        try { status = aot::InstallArtifact(space, artifact.image, guest.memory,
                                            config.guest_addr_mask, report, error); }
        catch (...) { install_error = std::current_exception(); }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!protection_paused && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    Check(protection_paused, "pause the actual installer between node publication and dispatch commit");
    const auto node = module->GetNode(Location{0x100});
    Check(IsFunction(node), "the paused installer has published its actual node");
    const auto lifetime = GetFunction(node)->code_lifetime;
    Check(lifetime && !lifetime->Live() && !module->GetJitCache(Location{0x100}),
          "a pending allocation is invisible to module lookup before commit");
    const auto region = module->GetCodeRegion(lifetime->base);
    Check(region.has_value(), "bind paused allocation to its real arena");
    watched_code_free = SiteRxToRw(*region, lifetime->base);
    watched_code_frees = 0;
    std::atomic<bool> invalidation_requested{};
    invalidator = std::thread([&] {
        invalidation_requested = true;
        try { space.GetSmcTracker().InvalidateRange(space, nullptr, 0x100, 0x104); }
        catch (...) { invalidate_error = std::current_exception(); }
    });
    while (!invalidation_requested) std::this_thread::yield();
    finish_protection = true;
    publisher.join(); invalidator.join();
    if (install_error) std::rethrow_exception(install_error);
    if (invalidate_error) std::rethrow_exception(invalidate_error);
    Check(status == aot::kAotOk && lifetime->Revoked() && !aot::ResolveCodeOffset(report.mapping, 0) &&
          !space.GetCodeCacheTable().Lookup(0x100) && IsEmpty(module->GetNode(Location{0x100})) &&
          watched_code_frees == 1,
          "a racing range invalidation consumes the whole installation and cannot be followed by late publication");
    Check(!lifetime->Commit() && !lifetime->Live(), "a revoked pending/live token cannot be committed again");
}

void RollbackSmcSymbols() {
    GuestWindow guest;
    const auto config = TestConfig(guest.memory);
    SymbolArtifact artifact{Artifact(config, guest.memory)};
    unsigned rejected{}, private_releases{}, deferred_releases{};
    bool reached_success{};
    unsigned allocation_count{};
    {
        AddressSpace space{config};
        TranslateTable l1{8};
        auto& smc = space.GetSmcTracker();
        const auto token = smc.RegisterRuntime(l1);
        const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
        SmcTracker::ExecutionScope active{smc, token};
        aot::LoadReport report;
        std::string error;
        aot_failure_hit = false;
        aot_allocation_index = 0;
        // Count a successful transaction with the existing injector. This
        // safety ceiling must not fire and is not the measured sweep bound.
        fail_aot_allocation = 4096;
        const auto reset = ScopeExit([] { fail_aot_allocation = -1; });
        const auto status = aot::InstallArtifact(space, artifact.image, guest.memory,
                                                config.guest_addr_mask, report, error);
        allocation_count = aot_allocation_index;
        fail_aot_allocation = -1;
        Check(status == aot::kAotOk && !aot_failure_hit && report.units_installed == 1 &&
              allocation_count > 10 && allocation_count < 4096,
              "measure a complete successful installation before enumerating allocation failures");
    }
    for (int failure = 0; failure <= static_cast<int>(allocation_count); ++failure) {
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        TranslateTable l1{8};
        auto& smc = space.GetSmcTracker();
        auto token = smc.RegisterRuntime(l1);
        const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
        SmcTracker::ExecutionScope active{smc, token};
        aot::LoadReport report;
        std::string error;
        watched_code_free = nullptr;
        watched_code_frees = 0;
        watched_aot_size = artifact.image.units.front().code_size;
        watch_aot_allocation = true;
        aot_failure_hit = aot_was_published = aot_published_at_failure = false;
        aot_allocation_index = 0;
        fail_aot_allocation = failure;
        bool threw{};
        int status = -1;
        try { status = aot::InstallArtifact(space, artifact.image, guest.memory,
                                            config.guest_addr_mask, report, error); }
        catch (const std::bad_alloc&) { threw = true; }
        fail_aot_allocation = -1;
        watch_aot_allocation = false;
        const auto reset = ScopeExit([] { watched_code_free = nullptr; });
        if (!threw) {
            Check(!aot_failure_hit && status == aot::kAotOk && report.units_installed == 1,
                  "allocation-failure sweep reaches an unchanged successful installation");
            reached_success = true;
            active.End();
            break;
        }
        ++rejected;
        Check(aot_failure_hit, "each rejected sweep iteration actually consumes its selected allocation failure");
        Check(!space.GetCodeCacheTable().Lookup(0x100) && IsEmpty(module->GetNode(Location{0x100})),
              "every one-shot allocation failure removes all node and slot visibility");
        if (watched_code_free) {
            if (aot_published_at_failure) {
                Check(watched_code_frees == 0, "an allocation already published at the failure cannot free before executor exit");
                ++deferred_releases;
            } else {
                Check(watched_code_frees == 1, "private rollback frees its canonical allocation once");
                ++private_releases;
            }
        }
        active.End();
        if (watched_code_free) Check(watched_code_frees == 1,
              "published allocation rollback waits for the active executor and then frees once");
    }
    std::printf("rollback_sweep baseline=%u reached=%u rejected=%u private=%u deferred=%u allocations=%u\n",
                allocation_count, reached_success ? 1u : 0u, rejected, private_releases,
                deferred_releases, aot_allocation_index);
    Check(reached_success && rejected > 10 && private_releases && deferred_releases,
          "fault sweep covers both private and visible transaction rollback");
    {
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        TranslateTable l1{8};
        auto& smc = space.GetSmcTracker();
        auto token = smc.RegisterRuntime(l1);
        const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
        SmcTracker::ExecutionScope active{smc, token};
        aot::LoadReport report;
        std::string error;
        watched_code_free = nullptr;
        watched_aot_size = artifact.image.units.front().code_size;
        watch_aot_allocation = fail_aot_protect = true;
        bool rejected_protection{};
        try { (void)aot::InstallArtifact(space, artifact.image, guest.memory,
                                         config.guest_addr_mask, report, error); }
        catch (const std::runtime_error& error) {
            rejected_protection = std::strcmp(error.what(), "AOT guest write window is open") == 0;
        }
        const bool protection_failure_consumed = !fail_aot_protect;
        watch_aot_allocation = fail_aot_protect = false;
        const auto reset = ScopeExit([] { watched_code_free = nullptr; });
        Check(protection_failure_consumed && rejected_protection && watched_code_free && watched_code_frees == 0 &&
              !space.GetCodeCacheTable().Lookup(0x100) && IsEmpty(module->GetNode(Location{0x100})),
              "failed page protection rejects publication and retains visible allocation through grace");
        active.End();
        Check(watched_code_frees == 1, "failed page protection frees only after executor exit");
    }
}

void DiskDispatchConflict() {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-disk-admission-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-disk-admission-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "enable isolated production disk cache before constructing address spaces");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    const auto config = TestConfig(guest.memory);
    const auto artifact = Artifact(config, guest.memory);
    for (unsigned invalid = 0; invalid < 10; ++invalid) {
        AddressSpace space{config};
        auto* disk = space.GetJitDiskCache();
        Check(disk && disk->Enabled(), "actual production disk loader admits test configuration");
        aot::LoadReport previous;
        std::string error;
        Check(aot::InstallArtifact(space, artifact, guest.memory, config.guest_addr_mask, previous, error) == aot::kAotOk,
              "publish independent accepted native code before disk replay");
        auto* prior = space.GetCodeCache(Location{0x100});
        auto* prior_offset = aot::ResolveCodeOffset(previous.mapping, 0);
        auto& table = space.GetCodeCacheTable();
        std::vector<std::array<size_t, 3>> before;
        table.ForEachEntry([&](u32 index, size_t key, size_t value) { before.push_back({index, key, value}); });
        const auto found = std::find_if(before.begin(), before.end(), [](const auto& item) { return item[1] == 0x100; });
        Check(prior && prior_offset && found != before.end(), "bind actual prior disk-replay target");
        const u32 occupied = u32((*found)[0]), valid_empty = table.Hash(0x2100);
        u32 empty{};
        while (std::any_of(before.begin(), before.end(), [&](const auto& item) { return item[0] == empty; })) ++empty;
        Check(std::none_of(before.begin(), before.end(), [&](const auto& item) { return item[0] == valid_empty; }),
              "choose actually empty and hash-reachable replay prefix");
        std::vector<std::pair<u64, u32>> assignments;
        if (invalid == 0) assignments = {{0x2100, valid_empty}, {0x1100, occupied}};
        if (invalid == 1) assignments = {{0x2100, valid_empty}, {0x2200, valid_empty}};
        if (invalid == 2) assignments = {{0x2100, valid_empty}, {0x2100, valid_empty + 1}};
        if (invalid == 3) assignments = {{0, empty}};
        if (invalid == 4) assignments = {{0x2100, UINT32_MAX}};
        if (invalid == 5) assignments = {{0x2100, empty}};
        if (invalid == 6) assignments = {{0x100, empty}};
        if (invalid == 7) assignments = {{0x2100, valid_empty}};
        if (invalid == 9) assignments = {{0x100, occupied}, {0x100, occupied}, {0x2100, valid_empty}};
        BlobWriter payload;
        payload.U64(invalid == 8 ? 2 : assignments.size());
        for (const auto& [key, index] : assignments) { payload.U64(key); payload.U32(index); }
        if (invalid == 8) payload.U64(0x2100); // Truncated assignment table, with a valid outer checksum.
        else if (invalid != 7) payload.U64(0);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, space.GetDefaultModule()->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)})
            file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> path{};
        std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(path.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write correctly keyed and checksummed own production cache fixture");
        }
        space.LoadJitCache();
        std::vector<std::array<size_t, 3>> after;
        table.ForEachEntry([&](u32 index, size_t slot_key, size_t value) { after.push_back({index, slot_key, value}); });
        const bool accepted = invalid == 9;
        if (accepted) {
            before.push_back({valid_empty, 0x2100, 0});
            std::sort(before.begin(), before.end());
        }
        Check(disk->Stats().reject_header.load() == (accepted ? 0u : 1u) &&
              disk->Stats().dispatch_slots.load() == (accepted ? 3u : 0u) && disk->Stats().units_loaded.load() == 0 &&
              before == after && space.GetCodeCache(Location{0x100}) == prior &&
              aot::ResolveCodeOffset(previous.mapping, 0) == prior_offset && aot::ResolveCodeOffset(0) == prior_offset,
              "disk rejection preserves existing entries and publishes no partial replay prefix");
        Runtime runtime{&space};
        runtime.SetLocation(0x100);
        Check(runtime.Run() == HaltReason::CallHost, "prior accepted native entry executes after production disk replay");
        u64 answer{};
        std::memcpy(&answer, runtime.GetUniformBuffer().data(), sizeof(answer));
        Check(answer == 37, "prior result survives conflicting or truncated disk cache");
        Check(unlink(path.data()) == 0, "remove only the owned disk replay fixture");
    }
    std::printf("disk_dispatch rejected=9 valid_merges=1 prior_executions=10 result=37\n");
}

void DiskRevivalFailure(bool cleanup_failure, bool terminal, bool sweep) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-disk-revival-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-disk-revival-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "enable an isolated production disk revival cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    const auto config = TestConfig(guest.memory);
    const auto artifact = Artifact(config, guest.memory);
    unsigned failures{}, cleanup_failures{}, successes{};
    for (const bool function : {false, true}) for (const bool primary_oom : {true, false}) {
        if (sweep && !primary_oom) continue;
        bool completed{};
        for (unsigned index = 0; index < (sweep ? 256u : 1u); ++index) {
            AddressSpace space{config};
            auto module = space.GetDefaultModule();
            auto& smc = space.GetSmcTracker();
            auto* disk = space.GetJitDiskCache();
            Check(disk && disk->Enabled(), "exercise the actual production disk unit loader");
            SerialUnit unit{};
            unit.guest_start = 0x100; unit.is_function = function;
            unit.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
            unit.code = artifact.code; unit.blocks = artifact.units.front().blocks;
            unit.relocs = artifact.units.front().relocs;
            for (const auto& fault : artifact.units.front().fault_sites)
                if (config.backend_isa == kRiscv64 || fault.host_end == fault.host_begin + 4)
                    unit.fault_sites.push_back(fault);
            // Include a real precise fault range even when the source only
            // needed an allocation-wide fallback for this simple function.
            if (unit.fault_sites.empty()) unit.fault_sites.push_back({0x100, 0, 4});
            BlobWriter payload;
            payload.U64(0); payload.U64(1); WriteUnit(payload, unit);
            const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
                ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
            BlobWriter file;
            constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
            file.Bytes(magic, sizeof(magic));
            for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                    u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)})
                file.U64(value);
            file.Bytes(payload.Data().data(), payload.Size());
            auto name = HashU64(key.build_id, key.guest_id);
            name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
            std::array<char, 512> path{};
            std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
            {
                std::ofstream output(path.data(), std::ios::binary | std::ios::trunc);
                output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
                Check(output.good(), "write a correctly keyed and checksummed native disk unit");
            }
            TranslateTable l1{8};
            const auto token = smc.RegisterRuntime(l1);
            bool registered = true;
            const auto unregister = ScopeExit([&] { if (registered) smc.UnregisterRuntime(token); });
            SmcTracker::ExecutionScope active{smc, token};
            watched_code_free = nullptr; watched_code_frees = 0;
            watched_aot_size = unit.code.size(); post_push_failure_index = index;
            aot_failure_hit = aot_was_published = aot_published_at_failure = false;
            aot_protection_failure_hit = cleanup_query_observed = cleanup_failure_hit = false;
            watch_aot_allocation = true; inject_aot_cleanup = cleanup_failure;
            fail_after_aot_push = primary_oom; fail_aot_protect = !primary_oom;
            const auto reset = ScopeExit([] {
                watched_code_free = nullptr; post_push_failure_index = 0;
                watch_aot_allocation = fail_after_aot_push = fail_aot_protect = false;
                inject_aot_cleanup = fail_cleanup_allocation = false;
                fail_aot_allocation = -1;
            });
            bool escaped{};
            try { space.LoadJitCache(); } catch (...) { escaped = true; }
            watch_aot_allocation = fail_after_aot_push = fail_aot_protect = false;
            fail_aot_allocation = -1;
            const bool hit = primary_oom ? aot_failure_hit : aot_protection_failure_hit;
            Check(aot_was_published && watched_code_free && disk->Stats().feature_match == 1,
                  "a valid native unit reached real Module publication and allocation");
            if (!hit) {
                Check(sweep && !escaped && disk->Stats().units_loaded == 1 && module->GetJitCache(Location{0x100}),
                      "the post-publication OOM sweep ends only at a successful complete revival");
                active.End();
                Runtime runtime{&space}; runtime.SetLocation(0x100);
                Check(runtime.Run() == HaltReason::CallHost, "revived native code executes after all failure points");
                u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
                Check(value == 37, "successful disk revival preserves the actual native result");
                ++successes; completed = true; break;
            }
            ++failures;
            Check(disk->Stats().units_loaded == 0 && !module->GetJitCache(Location{0x100}) &&
                  !space.GetCodeCacheTable().Lookup(0x100),
                  "failed disk revival leaves no cached node or reachable native entry");
            Check(!escaped && watched_code_frees == 0,
                  "disk failure degrades to JIT and preserves the exact allocation through executor grace");
            const auto node = module->GetNode(Location{0x100});
            if (cleanup_failure) {
                Check(cleanup_query_observed && cleanup_failure_hit && !fail_cleanup_allocation && !inject_aot_cleanup &&
                      !IsEmpty(node), "a distinct cleanup OOM retains actual Module ownership");
                auto* retained = function ? static_cast<AddressNode*>(GetFunction(node).get())
                                          : static_cast<AddressNode*>(GetBlock(node).get());
                const auto lifetime = retained->code_lifetime;
                const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
                Check(lifetime && lifetime->Revoked() && !lifetime->Commit() && region,
                      "failed disk cleanup is inert and cannot recommit");
                auto* rx = lifetime->base; auto* rw = SiteRxToRw(*region, rx);
                Check(watched_code_free == rw && Mapped(rx) && Mapped(rw),
                      "both actual aliases remain owned after compound failure");
                active.End();
                Check(watched_code_frees == 0, "unsubmitted cleanup is retained after executor exit");
                if (terminal) {
                    smc.UnregisterRuntime(token); registered = false;
                    const auto ticket = Terminal(space);
                    Check(space.TryShutdown(ticket).Complete() && !Mapped(rx) && !Mapped(rw) && watched_code_frees == 0,
                          "terminal shutdown consumes failed disk cleanup through its arena");
                } else {
                    Check(smc.RetireNode(space, &l1, module, retained) && watched_code_frees == 1 &&
                          IsEmpty(module->GetNode(Location{0x100})) && !smc.RetireNode(space, &l1, module, retained),
                          "ordinary retry releases failed disk cleanup exactly once");
                }
                ++cleanup_failures;
            } else {
                Check(IsEmpty(node), "ordinary rollback detaches the failed disk node");
                active.End();
                Check(watched_code_frees == 1, "disk allocation is freed exactly once after executor exit");
            }
            Check(unlink(path.data()) == 0, "remove the owned failure fixture before teardown");
            if (!sweep) completed = true;
        }
        Check(completed, "every disk revival failure sweep reaches its terminal case");
    }
    std::printf("disk_revival failures=%u cleanup_failures=%u terminal=%u sweep=%u successes=%u shapes=2\n",
                failures, cleanup_failures, terminal, sweep, successes);
}

void GuestReadability(bool aot_mode) {
    unsigned accepted{}, rejected{};
    for (unsigned shape = 0; shape < 6; ++shape) {
        const auto child = fork();
        Check(child >= 0, "fork an isolated actual guest-range admission process");
        if (child == 0) {
            const int result = [&] {
            try {
#ifdef ANDROID
                char directory[] = "/data/local/tmp/swiftvm-guest-readable-XXXXXX";
#else
                char directory[] = "/tmp/swiftvm-guest-readable-XXXXXX";
#endif
                Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
                      "enable isolated correctly keyed disk guest-range fixture");
                const auto cleanup = ScopeExit([&] {
                    auto* files = opendir(directory);
                    if (files) {
                        while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                            unlink((std::string(directory) + '/' + entry->d_name).c_str());
                        closedir(files);
                    }
                    rmdir(directory);
                });
                GuestWindow guest;
                const auto page = static_cast<size_t>(getpagesize());
                auto config = TestConfig(guest.memory);
                auto artifact = Artifact(config, guest.memory);
                const bool partial = shape == 2 || shape == 3;
                const VAddr start = partial ? page - 2 : 0x100;
                auto& source = artifact.units.front();
                source.guest_start = start;
                source.blocks.front().guest_start = start;
                source.blocks.front().guest_end = start + 4;
                source.blocks.front().guest_bytes_hash = HashBytes(static_cast<u8*>(guest.memory) + start, 4, 0xCBF29CE484222325ull);
                for (auto& fault : source.fault_sites) fault.guest_start = start;
                AddressSpace space{config};
                auto module = space.GetDefaultModule();
                auto* disk = space.GetJitDiskCache();
                Check(disk && disk->Enabled(), "production disk loader accepts the range fixture configuration");
                std::array<char, 512> path{};
                if (!aot_mode) {
                    SerialUnit unit{};
                    unit.guest_start = start; unit.is_function = 1;
                    unit.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
                    unit.code = artifact.code; unit.blocks = source.blocks; unit.relocs = source.relocs;
                    BlobWriter payload;
                    payload.U64(0); payload.U64(1); WriteUnit(payload, unit);
                    const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
                        ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
                    BlobWriter file;
                    constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
                    file.Bytes(magic, sizeof(magic));
                    for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                            u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)})
                        file.U64(value);
                    file.Bytes(payload.Data().data(), payload.Size());
                    auto name = HashU64(key.build_id, key.guest_id);
                    name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
                    std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
                    std::ofstream output(path.data(), std::ios::binary | std::ios::trunc);
                    output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
                    Check(output.good(), "write an authentic disk unit before revoking guest readability");
                }
                auto* revoked_page = static_cast<u8*>(guest.memory) + (partial ? page : 0);
                if (shape == 0 || shape == 2)
                    Check(__real_mprotect(revoked_page, page, PROT_NONE) == 0, "make actual guest bytes inaccessible");
                else if (shape == 1 || shape == 3)
                    Check(__real_munmap(revoked_page, page) == 0, "unmap actual guest bytes");
                else if (shape == 4)
                    Check(__real_mprotect(revoked_page, page, PROT_READ) == 0, "keep valid read-only guest code admissible");
                else
                    static_cast<u8*>(guest.memory)[start] ^= 1;
                aot::LoadReport report;
                std::string error;
                int status{};
                if (aot_mode) status = aot::InstallArtifact(space, artifact, guest.memory, config.guest_addr_mask, report, error);
                else space.LoadJitCache();
                const bool valid = shape == 4;
                if (valid) {
                    Check(aot_mode ? status == aot::kAotOk && report.units_installed == 1 : disk->Stats().units_loaded == 1,
                          "safe guest hashing keeps readable production code installable");
                    Runtime runtime{&space}; runtime.SetLocation(start);
                    Check(runtime.Run() == HaltReason::CallHost, "admitted read-only native guest code executes");
                    u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
                    Check(value == 37, "safe readable admission preserves the native result");
                } else {
                    Check(aot_mode ? status == aot::kAotGuestMismatch && report.units_installed == 0 :
                                     disk->Stats().units_loaded == 0 && disk->Stats().reject_guest_bytes == 1,
                          "unreadable or changed actual guest range rejects before publication");
                    Check(IsEmpty(module->GetNode(Location{start})) && !space.GetCodeCacheTable().Lookup(start) &&
                          !space.GetCallCodeCacheTable().Lookup(start) && !space.GetPendingCallCodeCacheTable().Lookup(start),
                          "guest range rejection publishes no node or canonical/Call alias");
                }
                if (shape <= 3)
                    Check(mmap(revoked_page, page, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == revoked_page,
                          "restore only the owned guest page before teardown");
                if (!aot_mode) Check(unlink(path.data()) == 0, "remove the isolated range fixture");
                std::printf("guest_readability backend=%s shape=%u accepted=%u result=%u checks=%u\n",
                            aot_mode ? "aot" : "disk", shape, valid, valid ? 37 : 0, checks);
                std::fflush(stdout);
                return 0;
            } catch (const std::exception& failure) {
                std::fprintf(stderr, "guest_readability child failed: %s\n", failure.what());
                return 1;
            }
            }();
            _exit(result);
        }
        int status{};
        Check(waitpid(child, &status, 0) == child, "join the actual guest-range admission process");
        std::printf("guest_readability_child backend=%s shape=%u exit=%d signal=%d\n", aot_mode ? "aot" : "disk", shape,
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        Check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "actual guest-range admission completes without a native memory fault");
        if (shape == 4) ++accepted; else ++rejected;
    }
    std::printf("guest_readability backend=%s children=6 rejected=%u native_successes=%u\n",
                aot_mode ? "aot" : "disk", rejected, accepted);
}

void GuestHashRead() {
    GuestWindow guest;
    const auto page = static_cast<size_t>(getpagesize());
    std::memset(guest.memory, 0xa5, 2 * page);
    const auto mask = guest.size - 1;
    const auto reset = ScopeExit([] { deny_guest_read = false; interrupt_guest_read = partial_guest_read = 0; });
    unsigned race_reads{}, fallback_cases{};
    for (const bool fallback : {false, true}) {
        deny_guest_read = fallback;
        const auto descriptors = DescriptorCount();
        for (const auto size : {size_t{1}, size_t{4}, size_t{512}, page + 17}) {
            u64 hash{};
            Check(HashGuestMemory(guest.memory, mask, 0, size, hash) &&
                  hash == HashBytes(guest.memory, size, 0xCBF29CE484222325ull),
                  "native and denied-syscall guest reads produce the original byte hash");
            if (fallback) ++fallback_cases;
        }
        Check(__real_mprotect(static_cast<u8*>(guest.memory) + page, page, PROT_NONE) == 0,
              "protect the actual second guest page for short-copy rejection");
        for (const auto start : {VAddr{page}, VAddr{page - 2}}) {
            u64 hash = 0x12345678;
            Check(!HashGuestMemory(guest.memory, mask, start, start + 4, hash) && hash == 0x12345678,
                  "inaccessible and partially readable copies reject without changing the output hash");
            if (fallback) ++fallback_cases;
        }
        Check(__real_mprotect(static_cast<u8*>(guest.memory) + page, page, PROT_READ | PROT_WRITE) == 0 &&
              DescriptorCount() == descriptors, "fallback guest reads retain no pipe descriptors");
    }
    deny_guest_read = false;
    u64 hash = 0x12345678;
    interrupt_guest_read = 1;
    Check(HashGuestMemory(guest.memory, mask, 0, 4, hash) && interrupt_guest_read == 0 &&
          hash == HashBytes(guest.memory, 4, 0xCBF29CE484222325ull), "interrupted kernel reads retry safely");
    partial_guest_read = 1; hash = 0x12345678;
    Check(!HashGuestMemory(guest.memory, mask, 0, 4, hash) && partial_guest_read == 0 && hash == 0x12345678,
          "short kernel reads reject rather than hashing uninitialized bytes");
    Check(!HashGuestMemory(guest.memory, mask, 4, 4, hash) &&
          !HashGuestMemory(guest.memory, mask, 5, 4, hash) &&
          !HashGuestMemory(guest.memory, 0, 0, (1u << 20) + 1, hash) &&
          !HashGuestMemory(guest.memory, mask, mask - 1, mask + 2, hash) &&
          !HashGuestMemory(reinterpret_cast<void*>(UINTPTR_MAX - 16), 0, 32, 36, hash) &&
          !HashGuestMemory(reinterpret_cast<void*>(UINTPTR_MAX - 2), 0, 0, 4, hash) && hash == 0x12345678,
          "empty, wrapped, oversized, masked-crossing and biased-overflow ranges reject");
    std::atomic<bool> start{}, stop{}, protect_ok{true};
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        while (!stop.load()) {
            if (__real_mprotect(guest.memory, page, PROT_NONE) != 0 ||
                __real_mprotect(guest.memory, page, PROT_READ) != 0) protect_ok = false;
        }
    });
    const auto finish = ScopeExit([&] { stop = true; writer.join(); });
    start = true;
    const auto expected = HashBytes(static_cast<u8*>(guest.memory) + page, 64, 0xCBF29CE484222325ull);
    for (unsigned attempt = 0; attempt < 1024; ++attempt) {
        hash = 0x12345678;
        const bool readable = HashGuestMemory(guest.memory, mask, 0, 64, hash);
        Check(hash == (readable ? expected : 0x12345678), "concurrent protection returns exact bytes or safe rejection");
        ++race_reads;
    }
    stop = true;
    Check(protect_ok, "all concurrent native protection changes succeeded");
    std::printf("guest_hash fallback_cases=%u race_reads=%u interrupted=1 short_copy=1 range_guards=6\n", fallback_cases, race_reads);
}

void EntryAdmission(bool aot_mode, std::string_view retirement = {}) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-entry-admission-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-entry-admission-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "enable an isolated actual disk loader for entry ownership tests");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    auto original = Artifact(config, guest.memory, true);
    const auto page = static_cast<u64>(getpagesize());
    // Each published guest entry has a distinct physical body, including on
    // latch-enabled ARM where fault ranges must distinguish the entries.
    const auto body_size = static_cast<u32>(original.code.size());
    const auto body = original.code;
    const auto metadata = original.units.front();
    original.code.insert(original.code.end(), body.begin(), body.end());
    original.units.front().code_size = original.code.size();
    for (auto reloc : metadata.relocs) {
        reloc.code_offset += body_size;
        original.units.front().relocs.push_back(reloc);
    }
    for (auto branch : metadata.region_branches) {
        branch.code_offset += body_size;
        original.units.front().region_branches.push_back(branch);
    }
    for (auto fault : metadata.fault_sites) {
        fault.host_begin += body_size; fault.host_end += body_size;
        if (fault.recovery_offset != UINT32_MAX) fault.recovery_offset += body_size;
        fault.guest_start = page + 0x100;
        original.units.front().fault_sites.push_back(fault);
    }
    original.units.front().fault_sites.push_back({0x100, 0, static_cast<u32>(original.code.size())});
    original.code_hash = HashBytes(original.code.data(), original.code.size(), 0xCBF29CE484222325ull);
    const auto at = [&](VAddr location, bool secondary = false) {
        auto block = original.units.front().blocks.front();
        block.guest_start = location; block.guest_end = location + 4;
        block.guest_bytes_hash = HashBytes(static_cast<u8*>(guest.memory) + location, 4, 0xCBF29CE484222325ull);
        if (secondary) for (auto* offset : {&block.code_offset, &block.direct_code_offset, &block.pending_flags_code_offset,
                                           &block.continuation_code_offset, &block.pending_flags_continuation_code_offset})
            if (*offset != UINT32_MAX) *offset += body_size;
        return block;
    };
    original.units.front().blocks.push_back(at(page + 0x100, true));
    unsigned rejections{}, accepted{}, prefixes{}, executions{};
    for (unsigned shape = 0; shape < 8; ++shape) for (const bool reverse : {false, true}) {
        if (!retirement.empty() && shape != 4 && shape != 5) continue;
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        aot::LoadReport previous;
        std::string error;
        Check(aot::InstallArtifact(space, original, guest.memory, config.guest_addr_mask, previous, error) == aot::kAotOk,
              "install an independent native unit with root and secondary entry");
        const std::array<VAddr, 2> prior_locations{0x100, page + 0x100};
        std::array<std::array<size_t, 3>, 2> prior_entries{};
        const auto first_target = space.GetLinkManager().QueryTarget(prior_locations[0]);
        const auto second_target = space.GetLinkManager().QueryTarget(prior_locations[1]);
        Check(first_target && second_target, "both prior entries own actual link targets");
        const auto prior_alias = aot::ResolveCodeOffset(previous.mapping, 0);
        for (unsigned i = 0; i < prior_locations.size(); ++i) {
            const auto loc = prior_locations[i];
            prior_entries[i] = {space.GetCodeCacheTable().Lookup(loc), space.GetCallCodeCacheTable().Lookup(loc),
                                space.GetPendingCallCodeCacheTable().Lookup(loc)};
            Check(prior_entries[i][0] != 0, "prior canonical entry is live");
        }
        auto incoming = original;
        auto& candidate = incoming.units.front();
        const auto root = shape == 0 ? prior_locations[0] : shape == 2 ? prior_locations[1] : 2 * page + 0x100;
        const auto alias = shape == 1 || shape == 4 || shape == 5 ? prior_locations[0] :
                           shape == 3 ? prior_locations[1] : 4 * page + 0x100;
        candidate.guest_start = root; candidate.blocks = {at(root), at(alias, true)};
        for (auto& fault : candidate.fault_sites) fault.guest_start = root;
        if (shape == 4 || shape == 5) {
            auto& metadata = candidate.blocks.back();
            metadata.entry_flags = shape == 4 ? SerialBlock::MetadataOnly : SerialBlock::DependencyOnly;
            metadata.code_offset = metadata.direct_code_offset = metadata.pending_flags_code_offset =
                metadata.continuation_code_offset = metadata.pending_flags_continuation_code_offset = UINT32_MAX;
            metadata.pending_flags_contract = {};
        }
        if (shape >= 6) {
            auto second = candidate;
            second.guest_start = shape == 7 ? root : 3 * page + 0x100;
            second.blocks.front() = at(second.guest_start);
            for (auto& fault : second.fault_sites) fault.guest_start = second.guest_start;
            second.code_offset = incoming.code.size();
            incoming.code.insert(incoming.code.end(), original.code.begin(), original.code.end());
            incoming.units.push_back(std::move(second));
        }
        if (reverse) for (auto& unit : incoming.units) std::reverse(unit.blocks.begin(), unit.blocks.end());
        incoming.code_hash = HashBytes(incoming.code.data(), incoming.code.size(), 0xCBF29CE484222325ull);
        aot::LoadReport report;
        const bool valid = shape == 4 || shape == 5;
        const bool prefix = !aot_mode && shape >= 6;
        if (aot_mode) {
            const auto status = aot::InstallArtifact(space, incoming, guest.memory, config.guest_addr_mask, report, error);
            std::printf("entry_attempt backend=aot shape=%u reverse=%u status=%d units=%u\n", shape, reverse, status, report.units_installed);
            Check(status == (valid ? aot::kAotOk : aot::kAotBadFormat) && report.units_installed == unsigned(valid),
                  "AOT rejects every cross-owner entry before installing any unit");
            if (!valid) Check(!report.mapping && !report.dispatch_slots && aot::ResolveCodeOffset(0) == prior_alias,
                              "rejected ownership cannot replace the latest independent alias");
        } else {
            BlobWriter payload;
            payload.U64(0); payload.U64(incoming.units.size());
            for (const auto& source : incoming.units) {
                SerialUnit unit{};
                unit.guest_start = source.guest_start; unit.is_function = 1;
                unit.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
                unit.code.assign(incoming.code.begin() + source.code_offset,
                                 incoming.code.begin() + source.code_offset + source.code_size);
                unit.blocks = source.blocks; unit.relocs = source.relocs;
                unit.region_branches = source.region_branches;
                WriteUnit(payload, unit);
            }
            const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
                ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
            BlobWriter file;
            constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
            file.Bytes(magic, sizeof(magic));
            for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                    u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)})
                file.U64(value);
            file.Bytes(payload.Data().data(), payload.Size());
            auto name = HashU64(key.build_id, key.guest_id);
            name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
            std::array<char, 512> path{};
            std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
            {
                std::ofstream output(path.data(), std::ios::binary | std::ios::trunc);
                output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
                Check(output.good(), "write the real keyed multi-entry disk fixture");
            }
            space.LoadJitCache();
            auto& stats = space.GetJitDiskCache()->Stats();
            std::printf("entry_attempt backend=disk shape=%u reverse=%u rejected=%llu loaded=%llu\n", shape, reverse,
                        static_cast<unsigned long long>(stats.reject_reloc.load()), static_cast<unsigned long long>(stats.units_loaded.load()));
            Check(stats.reject_reloc == unsigned(!valid) && stats.units_loaded == unsigned(valid || prefix),
                  "disk revival rejects the conflicting unit while preserving any accepted independent prefix");
            Check(unlink(path.data()) == 0, "remove only this owned disk fixture");
        }
        for (unsigned i = 0; i < prior_locations.size(); ++i) {
            const auto loc = prior_locations[i];
            const auto target = space.GetLinkManager().QueryTarget(loc);
            const auto& before = i == 0 ? first_target : second_target;
            Check(target && target->generation == before->generation && target->host_pc == before->host_pc &&
                  target->direct_host_pc == before->direct_host_pc && target->pending_flags_host_pc == before->pending_flags_host_pc &&
                  target->call_host_pc == before->call_host_pc && target->call_pending_flags_host_pc == before->call_pending_flags_host_pc,
                  "rejected unit preserves all previously owned link entry variants and generation");
            Check(prior_entries[i] == std::array<size_t, 3>{space.GetCodeCacheTable().Lookup(loc),
                  space.GetCallCodeCacheTable().Lookup(loc), space.GetPendingCallCodeCacheTable().Lookup(loc)} &&
                  aot::ResolveCodeOffset(previous.mapping, 0) == prior_alias,
                  "canonical, Call, pending Call and explicit aliases retain the independent owner");
            Runtime runtime{&space}; runtime.SetLocation(loc);
            Check(runtime.Run() == HaltReason::CallHost, "the preserved prior entry still executes native code");
            u64 answer{}; std::memcpy(&answer, runtime.GetUniformBuffer().data(), sizeof(answer));
            Check(answer == 37, "preserved root and secondary entries retain their real result");
            ++executions;
        }
        if (!valid && !prefix && root != prior_locations[0] && root != prior_locations[1])
            Check(!space.GetCodeCache(Location{root}) && IsEmpty(module->GetNode(Location{root})),
                  "rejected new root has no partial node or dispatcher publication");
        if (prefix) {
            Runtime runtime{&space}; runtime.SetLocation(root);
            Check(runtime.Run() == HaltReason::CallHost, "accepted disk prefix still executes after later entry rejection");
            u64 answer{}; std::memcpy(&answer, runtime.GetUniformBuffer().data(), sizeof(answer));
            Check(answer == 37, "accepted disk prefix retains its production result");
            ++prefixes;
        }
        if (valid) ++accepted; else ++rejections;
        if (!retirement.empty()) {
            auto owner = module->GetNode(Location{root});
            Check(IsFunction(owner), "retain the actual restored metadata/dependency owning function");
            const auto function = GetFunction(owner);
            const auto lifetime = function->code_lifetime;
            const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
            Check(lifetime && lifetime->Live() && region, "bind exact restored allocation and both aliases before retirement");
            auto& smc = space.GetSmcTracker();
            TranslateTable l1{8};
            const auto token = smc.RegisterRuntime(l1);
            const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
            SmcTracker::ExecutionScope active{smc, token};
            watched_code_free = SiteRxToRw(*region, lifetime->base); watched_code_frees = 0;
            const auto reset_watch = ScopeExit([] { watched_code_free = nullptr; });
            if (retirement == "node") Check(smc.RetireNode(space, &l1, module, function.get()),
                                              "retire the actual restored node without retiring its dependencies");
            else if (retirement == "range") smc.InvalidateRange(space, &l1, root, root + 4);
            else {
                Check(smc.HandleWriteFault(space, l1, reinterpret_cast<uintptr_t>(guest.memory) + root),
                      "invalidate the restored root through a real guest write window");
                smc.CloseWriteWindow(space, l1);
            }
            Check(lifetime->Revoked() && !space.GetCodeCacheTable().Lookup(root) &&
                  !space.GetCallCodeCacheTable().Lookup(root) && !space.GetPendingCallCodeCacheTable().Lookup(root) &&
                  !space.GetLinkManager().QueryTarget(root) && IsEmpty(module->GetNode(Location{root})) && watched_code_frees == 0,
                  "retirement withdraws the restored owner's entries and waits for executor grace");
            for (unsigned i = 0; i < prior_locations.size(); ++i) {
                const auto loc = prior_locations[i];
                const auto target = space.GetLinkManager().QueryTarget(loc);
                const auto& before = i == 0 ? first_target : second_target;
                Check(prior_entries[i] == std::array<size_t, 3>{space.GetCodeCacheTable().Lookup(loc),
                      space.GetCallCodeCacheTable().Lookup(loc), space.GetPendingCallCodeCacheTable().Lookup(loc)} &&
                      target && target->generation == before->generation && aot::ResolveCodeOffset(previous.mapping, 0) == prior_alias,
                      "retiring metadata or a dependency preserves the independently owned entry surfaces");
                Runtime runtime{&space}; runtime.SetLocation(loc);
                Check(runtime.Run() == HaltReason::CallHost, "the independent entry executes during another owner's retirement");
                u64 answer{}; std::memcpy(&answer, runtime.GetUniformBuffer().data(), sizeof(answer));
                Check(answer == 37, "independent native execution survives metadata/dependency retirement");
            }
            active.End();
            Check(watched_code_frees == 1 && !smc.RetireNode(space, &l1, module, function.get()),
                  "retired metadata/dependency owner releases once and repeated retirement is inert");
            std::printf("entry_retirement backend=%s path=%.*s metadata=%u reverse=%u retained_entries=2 result=37 frees=1\n",
                        aot_mode ? "aot" : "disk", int(retirement.size()), retirement.data(), shape, reverse);
        }
    }
    std::printf("entry_admission backend=%s rejected=%u accepted=%u preserved_executions=%u prefix_executions=%u\n",
                aot_mode ? "aot" : "disk", rejections, accepted, executions, prefixes);
}

void AotDispatchConflict() {
    GuestWindow guest;
    const auto config = TestConfig(guest.memory);
    const auto original = Artifact(config, guest.memory);
    auto incoming = original;
    const auto shift = static_cast<u64>(getpagesize());
    incoming.units.front().guest_start += shift;
    for (auto& block : incoming.units.front().blocks) { block.guest_start += shift; block.guest_end += shift; }
    for (auto& fault : incoming.units.front().fault_sites) fault.guest_start += shift;
    for (unsigned invalid = 0; invalid < 7; ++invalid) {
        AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        aot::LoadReport previous;
        std::string error;
        Check(aot::InstallArtifact(space, original, guest.memory, config.guest_addr_mask, previous, error) == aot::kAotOk,
              "install independent accepted artifact before dispatch conflict");
        auto* const prior = space.GetCodeCache(Location{0x100});
        auto* const prior_offset = aot::ResolveCodeOffset(previous.mapping, 0);
        Check(prior && prior_offset && aot::ResolveCodeOffset(0) == prior_offset,
              "bind exact prior dispatch and alias before conflict");
        auto& table = space.GetCodeCacheTable();
        std::vector<std::array<size_t, 3>> before;
        table.ForEachEntry([&](u32 index, size_t key, size_t value) { before.push_back({index, key, value}); });
        const auto found = std::find_if(before.begin(), before.end(), [](const auto& item) { return item[1] == 0x100; });
        Check(found != before.end(), "locate actual occupied dispatch assignment");
        const u32 occupied = u32((*found)[0]);
        u32 empty{};
        while (std::any_of(before.begin(), before.end(), [&](const auto& item) { return item[0] == empty; })) ++empty;
        auto candidate = incoming;
        if (invalid == 0) candidate.dispatch_slots = {{0x2100, empty}, {0x1100, occupied}};
        if (invalid == 1) candidate.dispatch_slots = {{0x2100, empty}, {0x2200, empty}};
        if (invalid == 2) candidate.dispatch_slots = {{0x2100, empty}, {0x2100, empty + 1}};
        if (invalid == 3) candidate.dispatch_slots = {{0, empty}};
        if (invalid == 4) candidate.dispatch_slots = {{0x2100, UINT32_MAX}};
        if (invalid == 5) candidate.dispatch_slots = {{0x2100, empty}}; // Unreachable through its hash probe chain.
        if (invalid == 6) candidate.dispatch_slots = {{0x100, empty}};
        aot::LoadReport rejected;
        Check(aot::InstallArtifact(space, candidate, guest.memory, config.guest_addr_mask, rejected, error) == aot::kAotDispatchConflict,
              "reject whole malformed or conflicting dispatch assignment");
        std::vector<std::array<size_t, 3>> after;
        table.ForEachEntry([&](u32 index, size_t key, size_t value) { after.push_back({index, key, value}); });
        Check(before == after && space.GetCodeCache(Location{0x100}) == prior &&
              aot::ResolveCodeOffset(previous.mapping, 0) == prior_offset && aot::ResolveCodeOffset(0) == prior_offset &&
              !space.GetCodeCache(Location{0x1100}) && IsEmpty(module->GetNode(Location{0x1100})) &&
              !rejected.units_installed && !rejected.dispatch_slots && !rejected.mapping,
              "dispatch conflict preserves every existing assignment and publishes no prefix");
        Runtime runtime{&space};
        runtime.SetLocation(0x100);
        Check(runtime.Run() == HaltReason::CallHost, "prior accepted native entry executes after dispatch rejection");
        u64 answer{};
        std::memcpy(&answer, runtime.GetUniformBuffer().data(), sizeof(answer));
        Check(answer == 37, "prior native result survives dispatch conflict");
        auto valid = incoming;
        valid.dispatch_slots = {{0x100, occupied}, {0x100, occupied}};
        aot::LoadReport accepted;
        Check(aot::InstallArtifact(space, valid, guest.memory, config.guest_addr_mask, accepted, error) == aot::kAotOk &&
              accepted.units_installed == 1 && space.GetCodeCache(Location{0x100}) == prior &&
              aot::ResolveCodeOffset(previous.mapping, 0) == prior_offset && space.GetCodeCache(Location{0x1100}),
              "matching repeated replay preserves old entry values and admits independent new code");
    }
    std::printf("aot_dispatch invalid_cases=7 unchanged_assignments=7 prior_executions=7 valid_merges=7 result=37\n");
}

void MultiUnitInstallRollback(bool cleanup_failure, bool terminal) {
    GuestWindow guest;
    const auto config = TestConfig(guest.memory);
    const auto page = static_cast<u64>(getpagesize());
    const auto prototype = Artifact(config, guest.memory);
    const auto clone = [&](const std::vector<u64>& locations) {
        auto image = prototype;
        image.units.clear(); image.code.clear();
        for (const auto location : locations) {
            auto unit = prototype.units.front();
            const auto shift = location - unit.guest_start;
            unit.guest_start = location;
            unit.code_offset = u32(image.code.size());
            for (auto& block : unit.blocks) { block.guest_start += shift; block.guest_end += shift; }
            for (auto& fault : unit.fault_sites) fault.guest_start += shift;
            image.code.insert(image.code.end(), prototype.code.begin(), prototype.code.end());
            image.units.push_back(std::move(unit));
        }
        image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
        return image;
    };
    const std::vector<u64> locations{0x100, page + 0x100, 2 * page + 0x100};
    const auto image = clone(locations);
    const u64 neighbor = 3 * page + 0x100;
    const auto independent = clone({neighbor});
    for (const bool primary_oom : {true, false}) {
        AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        auto& smc = space.GetSmcTracker();
        aot::LoadReport previous;
        std::string error;
        Check(aot::InstallArtifact(space, independent, guest.memory, config.guest_addr_mask, previous, error) == aot::kAotOk,
              "install unrelated accepted unit before multi-unit transaction");
        auto* prior = space.GetCodeCache(Location{neighbor});
        Check(prior && aot::ResolveCodeOffset(previous.mapping, 0) && aot::ResolveCodeOffset(0),
              "bind unaffected earlier installation and latest alias");
        auto* prior_offset = aot::ResolveCodeOffset(previous.mapping, 0);
        TranslateTable l1{8};
        const auto token = smc.RegisterRuntime(l1);
        bool registered = true;
        const auto unregister = ScopeExit([&] { if (registered) smc.UnregisterRuntime(token); });
        SmcTracker::ExecutionScope active{smc, token};
        watched_aot_size = image.units.front().code_size;
        multi_allocations.fill(nullptr); multi_allocation_frees.fill(0); multi_allocation_count = 0;
        watch_multi_allocations = true;
        aot_failure_hit = aot_protection_failure_hit = false;
        multi_oom_pushes = primary_oom ? 2 : 0;
        multi_protect_pushes = primary_oom ? 0 : 2;
        multi_cleanup_queries = cleanup_failure ? 2 : 0;
        cleanup_query_observed = cleanup_failure_hit = false;
        const auto reset = ScopeExit([] {
            watch_multi_allocations = false;
            multi_oom_pushes = multi_protect_pushes = multi_cleanup_queries = 0;
            fail_aot_allocation = -1; fail_aot_protect = false;
            fail_cleanup_allocation = false;
        });
        aot::LoadReport report;
        bool rejected{};
        try { (void)aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error); }
        catch (const std::bad_alloc&) { rejected = primary_oom; }
        catch (const std::runtime_error& failure) {
            rejected = !primary_oom && std::strcmp(failure.what(), "AOT guest write window is open") == 0;
        }
        fail_aot_allocation = -1; fail_aot_protect = false;
        Check(rejected && (primary_oom ? aot_failure_hit : aot_protection_failure_hit) && multi_allocation_count == 2,
              "actual second-unit primary failure occurs after one unit committed");
        const auto retained_node = module->GetNode(Location{locations.front()});
        Check(cleanup_failure ? cleanup_query_observed && cleanup_failure_hit && IsFunction(retained_node) : IsEmpty(retained_node),
              "separate cleanup failure retains only the first committed owner for explicit retry");
        for (const auto location : locations) Check(!space.GetCodeCacheTable().Lookup(location) &&
              !space.GetCallCodeCacheTable().Lookup(location) && !space.GetPendingCallCodeCacheTable().Lookup(location) &&
              (cleanup_failure && location == locations.front() || IsEmpty(module->GetNode(Location{location}))) &&
              !space.GetLinkManager().QueryTarget(location),
              "later-unit failure revokes every unit from the failed installation");
        Check(!report.units_installed && !report.code_bytes && !report.dispatch_slots && !report.mapping,
              "failed multi-unit transaction reports zero committed installation");
        Check(space.GetCodeCache(Location{neighbor}) == prior && aot::ResolveCodeOffset(previous.mapping, 0) == prior_offset &&
              aot::ResolveCodeOffset(0) == prior_offset, "multi-unit rollback preserves independent accepted publication and latest alias");
        for (unsigned i = 0; i < multi_allocation_count; ++i) Check(!multi_allocation_frees[i],
              "both visible failed-installation allocations wait for executor grace");
        active.End();
        for (unsigned i = 0; i < multi_allocation_count; ++i) Check(multi_allocation_frees[i] == (cleanup_failure && i == 0 ? 0u : 1u),
              "queued allocation frees once while separately failed cleanup retains its storage");
        {
            Runtime runtime{&space};
            runtime.SetLocation(neighbor);
            Check(runtime.Run() == HaltReason::CallHost, "independent prior unit still runs after complete multi-unit rollback");
            u64 answer{};
            std::memcpy(&answer, runtime.GetUniformBuffer().data(), sizeof(answer));
            Check(answer == 37, "independent accepted native result survives later-unit failure");
        }
        if (cleanup_failure) {
            const auto lifetime = GetFunction(retained_node)->code_lifetime;
            const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
            Check(lifetime && lifetime->Revoked() && region && !lifetime->Commit() &&
                  SiteRxToRw(*region, lifetime->base) == multi_allocations[0] && multi_allocation_frees[0] == 0,
                  "compound multi-unit failure retains exact inert allocation after grace");
            if (terminal) {
                auto* rx = lifetime->base;
                auto* rw = multi_allocations[0];
                smc.UnregisterRuntime(token); registered = false;
                const auto ticket = Terminal(space);
                Check(space.TryShutdown(ticket).Complete() && !Mapped(rx) && !Mapped(rw) &&
                      multi_allocation_frees[0] == 0 && multi_allocation_frees[1] == 1,
                      "terminal cleanup releases compound multi-unit arenas without duplicate allocation frees");
                std::printf("multi_install failure=%s cleanup_oom=1 terminal=1 failed_unit=2 committed_before=1 allocations=2 ordinary_frees=1 arena_release=1\n",
                            primary_oom ? "oom" : "protect");
                continue;
            }
            Check(smc.RetireNode(space, &l1, module, GetFunction(retained_node).get()) && multi_allocation_frees[0] == 1 &&
                  IsEmpty(module->GetNode(Location{locations.front()})), "ordinary retry frees separately retained earlier unit exactly once");
        }
        watch_multi_allocations = false;
        aot::LoadReport retry;
        Check(aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, retry, error) == aot::kAotOk &&
              retry.units_installed == 3, "same full multi-unit installation succeeds after one-shot failure");
        for (const auto location : locations) {
            Runtime again{&space};
            again.SetLocation(location);
            Check(again.Run() == HaltReason::CallHost, "each retried installation unit executes");
        }
        std::printf("multi_install failure=%s cleanup_oom=%u terminal=0 failed_unit=2 committed_before=1 allocations=2 frees=2 retry_units=3 neighbor=37\n",
                    primary_oom ? "oom" : "protect", cleanup_failure);
    }
}

void MultiUnitCallRollback(bool cleanup_failure, bool terminal) {
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const auto target_image = ReturningTargetArtifact(config, guest.memory);
    const auto [fixture_image, fixture_captured] = RestoredCallFixture(
            config, guest.memory, target_image, 0x100, 2 * page + 0x100, 2 * page + 0x180);
    (void)fixture_captured;
    const auto& prototype = fixture_image;
    const auto clone = [&](const std::vector<u64>& locations) {
        auto image = prototype;
        image.units.clear(); image.code.clear();
        for (const auto location : locations) {
            auto unit = prototype.units.front();
            const auto shift = location - unit.guest_start;
            unit.guest_start = location;
            unit.code_offset = u32(image.code.size());
            for (auto& block : unit.blocks) { block.guest_start += shift; block.guest_end += shift; }
            for (auto& fault : unit.fault_sites) fault.guest_start += shift;
            image.code.insert(image.code.end(), prototype.code.begin(), prototype.code.end());
            image.units.push_back(std::move(unit));
        }
        image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
        return image;
    };
    const std::vector<u64> locations{2 * page + 0x100, 3 * page + 0x100, 4 * page + 0x100};
    const auto image = clone(locations);
    const u64 neighbor = 0x100;
    const auto& independent = target_image;
    for (const bool primary_oom : {true, false}) {
        AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        auto& smc = space.GetSmcTracker();
        aot::LoadReport previous;
        std::string error;
        Check(aot::InstallArtifact(space, independent, guest.memory, config.guest_addr_mask, previous, error) == aot::kAotOk,
              "install unrelated accepted unit before multi-unit transaction");
        auto* prior = space.GetCodeCache(Location{neighbor});
        Check(prior && aot::ResolveCodeOffset(previous.mapping, 0) && aot::ResolveCodeOffset(0),
              "bind unaffected earlier installation and latest alias");
        auto* prior_offset = aot::ResolveCodeOffset(previous.mapping, 0);
        TranslateTable l1{8};
        const auto token = smc.RegisterRuntime(l1);
        bool registered = true;
        const auto unregister = ScopeExit([&] { if (registered) smc.UnregisterRuntime(token); });
        SmcTracker::ExecutionScope active{smc, token};
        watched_aot_size = image.units.front().code_size;
        multi_allocations.fill(nullptr); multi_allocation_frees.fill(0); multi_allocation_count = 0;
        watch_multi_allocations = true;
        aot_failure_hit = aot_protection_failure_hit = false;
        multi_oom_pushes = primary_oom ? 2 : 0;
        multi_protect_pushes = primary_oom ? 0 : 2;
        multi_cleanup_queries = cleanup_failure ? 2 : 0;
        cleanup_query_observed = cleanup_failure_hit = false;
        const auto reset = ScopeExit([] {
            watch_multi_allocations = false;
            multi_oom_pushes = multi_protect_pushes = multi_cleanup_queries = 0;
            fail_aot_allocation = -1; fail_aot_protect = false;
            fail_cleanup_allocation = false;
        });
        aot::LoadReport report;
        bool rejected{};
        try { (void)aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error); }
        catch (const std::bad_alloc&) { rejected = primary_oom; }
        catch (const std::runtime_error& failure) {
            rejected = !primary_oom && std::strcmp(failure.what(), "AOT guest write window is open") == 0;
        }
        fail_aot_allocation = -1; fail_aot_protect = false;
        Check(rejected && (primary_oom ? aot_failure_hit : aot_protection_failure_hit) && multi_allocation_count == 2,
              "actual second-unit primary failure occurs after one unit committed");
        const auto retained_node = module->GetNode(Location{locations.front()});
        Check(cleanup_failure ? cleanup_query_observed && cleanup_failure_hit && IsFunction(retained_node) : IsEmpty(retained_node),
              "separate cleanup failure retains only the first committed owner for explicit retry");
        for (const auto location : locations) Check(!space.GetCodeCacheTable().Lookup(location) &&
              !space.GetCallCodeCacheTable().Lookup(location) && !space.GetPendingCallCodeCacheTable().Lookup(location) &&
              (cleanup_failure && location == locations.front() || IsEmpty(module->GetNode(Location{location}))) &&
              !space.GetLinkManager().QueryTarget(location),
              "later-unit failure revokes every unit from the failed installation");
        Check(!report.units_installed && !report.code_bytes && !report.dispatch_slots && !report.mapping,
              "failed multi-unit transaction reports zero committed installation");
        Check(space.GetCodeCache(Location{neighbor}) == prior && aot::ResolveCodeOffset(previous.mapping, 0) == prior_offset &&
              aot::ResolveCodeOffset(0) == prior_offset, "multi-unit rollback preserves independent accepted publication and latest alias");
        for (unsigned i = 0; i < multi_allocation_count; ++i) Check(!multi_allocation_frees[i],
              "both visible failed-installation allocations wait for executor grace");
        active.End();
        for (unsigned i = 0; i < multi_allocation_count; ++i) Check(multi_allocation_frees[i] == (cleanup_failure && i == 0 ? 0u : 1u),
              "queued allocation frees once while separately failed cleanup retains its storage");
        {
            const auto current = space.GetLinkManager().QueryTarget(neighbor);
            Check(current && space.GetCodeCache(Location{neighbor}) == prior &&
                  aot::ResolveCodeOffset(previous.mapping, 0) == prior_offset,
                  "independent returning target remains published after complete multi-unit rollback");
        }
        if (cleanup_failure) {
            const auto lifetime = GetFunction(retained_node)->code_lifetime;
            const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
            Check(lifetime && lifetime->Revoked() && region && !lifetime->Commit() &&
                  SiteRxToRw(*region, lifetime->base) == multi_allocations[0] && multi_allocation_frees[0] == 0,
                  "compound multi-unit failure retains exact inert allocation after grace");
            if (terminal) {
                auto* rx = lifetime->base;
                auto* rw = multi_allocations[0];
                smc.UnregisterRuntime(token); registered = false;
                const auto ticket = Terminal(space);
                Check(space.TryShutdown(ticket).Complete() && !Mapped(rx) && !Mapped(rw) &&
                      multi_allocation_frees[0] == 0 && multi_allocation_frees[1] == 1,
                      "terminal cleanup releases compound multi-unit arenas without duplicate allocation frees");
                std::printf("multi_call_install failure=%s cleanup_oom=1 terminal=1 failed_unit=2 committed_before=1 allocations=2 ordinary_frees=1 arena_release=1\n",
                            primary_oom ? "oom" : "protect");
                continue;
            }
            Check(smc.RetireNode(space, &l1, module, GetFunction(retained_node).get()) && multi_allocation_frees[0] == 1 &&
                  IsEmpty(module->GetNode(Location{locations.front()})), "ordinary retry frees separately retained earlier unit exactly once");
        }
        watch_multi_allocations = false;
        aot::LoadReport retry;
        Check(aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, retry, error) == aot::kAotOk &&
              retry.units_installed == 3, "same full multi-unit installation succeeds after one-shot failure");
        for (const auto location : locations) {
            Runtime again{&space};
            const u64 resume = location + 0x80;
            std::memcpy(again.GetUniformBuffer().data() + 16, &resume, 8);
            again.SetLocation(location);
            Check(again.Run() == HaltReason::CallHost, "each retried Call unit executes through the returning target");
            u64 value{}, witness{};
            std::memcpy(&value, again.GetUniformBuffer().data(), 8);
            std::memcpy(&witness, again.GetUniformBuffer().data() + 24, 8);
            Check(value == 37 && witness == 0x51ab, "retried Call units preserve target and return-body witnesses");
            const auto node = module->GetNode(Location{location});
            const auto sites = space.GetLinkManager().QuerySourceSites(
                    {module.get(), GetFunction(node)->code_lifetime->base});
            Check(sites.size() == 1 &&
                  sites.front().kind == (FlagsRegsEnabled() ? LinkSiteKind::Call : LinkSiteKind::Unconditional) &&
                  sites.front().state == LinkSiteState::Linked, "each retried Call unit links its real Call site");
        }
        std::printf("multi_call_install failure=%s cleanup_oom=%u terminal=0 failed_unit=2 committed_before=1 allocations=2 frees=2 retry_units=3 neighbor=37\n",
                    primary_oom ? "oom" : "protect", cleanup_failure);
    }
}


void CompoundSmcSymbols() {
    GuestWindow guest;
    const auto config = TestConfig(guest.memory);
    SymbolArtifact artifact{Artifact(config, guest.memory)};
    for (const bool primary_oom : {false, true}) for (const bool terminal : {false, true}) {
        AddressSpace space{config};
        auto module = space.GetDefaultModule();
        auto& smc = space.GetSmcTracker();
        TranslateTable l1{8};
        const auto token = smc.RegisterRuntime(l1);
        bool registered = true;
        const auto unregister = ScopeExit([&] { if (registered) smc.UnregisterRuntime(token); });
        SmcTracker::ExecutionScope active{smc, token};
        aot::LoadReport report;
        std::string error;
        watched_code_free = nullptr;
        watched_code_frees = 0;
        watched_aot_size = artifact.image.units.front().code_size;
        aot_failure_hit = aot_was_published = aot_published_at_failure = false;
        aot_protection_failure_hit = cleanup_query_observed = cleanup_failure_hit = false;
        watch_aot_allocation = inject_aot_cleanup = true;
        fail_after_aot_push = primary_oom;
        fail_aot_protect = !primary_oom;
        const auto reset = ScopeExit([] {
            watched_code_free = nullptr;
            watch_aot_allocation = fail_after_aot_push = fail_aot_protect = false;
            inject_aot_cleanup = fail_cleanup_allocation = false;
            fail_aot_allocation = -1;
        });
        bool original_error{};
        try {
            (void)aot::InstallArtifact(space, artifact.image, guest.memory,
                                      config.guest_addr_mask, report, error);
        } catch (const std::bad_alloc&) {
            original_error = primary_oom;
        } catch (const std::runtime_error& failure) {
            original_error = !primary_oom &&
                    std::strcmp(failure.what(), "AOT guest write window is open") == 0;
        }
        watch_aot_allocation = false;
        Check(original_error && aot_was_published &&
              (primary_oom ? aot_failure_hit && aot_published_at_failure : aot_protection_failure_hit) &&
              cleanup_query_observed && cleanup_failure_hit && !fail_cleanup_allocation && !inject_aot_cleanup,
              "actual published primary failure survives a separately consumed cleanup allocation failure");
        const auto node = module->GetNode(Location{0x100});
        Check(IsFunction(node), "failed cleanup retains its actual node and Module ownership");
        auto* function = GetFunction(node).get();
        const auto lifetime = function->code_lifetime;
        const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
        Check(lifetime && lifetime->Revoked() && region && watched_code_free && watched_code_frees == 0 &&
              !lifetime->Commit() && !module->GetJitCache(Location{0x100}) &&
              !space.GetCodeCacheTable().Lookup(0x100) && report.units_installed == 0 && !report.mapping,
              "compound failure leaves all publication inert and keeps the exact allocation owned");
        auto* const rx = lifetime->base;
        auto* const rw = SiteRxToRw(*region, rx);
        Check(watched_code_free == rw && Mapped(rx) && Mapped(rw),
              "failed cleanup retains both real aliases rather than freeing visible code");
        if (!primary_oom) {
            FaultEntry fault;
            Check(module->LookupCodeAllocation(rx, fault),
                  "failed post-protection cleanup retains fault metadata with its allocation");
        }
        active.End();
        Check(watched_code_frees == 0, "executor exit cannot reclaim storage that cleanup never queued");
        if (terminal) {
            smc.UnregisterRuntime(token);
            registered = false;
            const auto ticket = Terminal(space);
            Check(space.TryShutdown(ticket).Complete() && !Mapped(rx) && !Mapped(rw) &&
                  watched_code_frees == 0 && !module->GetJitCache(Location{0x100}),
                  "terminal arena release safely consumes compound-failure storage without a duplicate free");
        } else {
            Check(smc.RetireNode(space, &l1, module, function) && watched_code_frees == 1 &&
                  IsEmpty(module->GetNode(Location{0x100})) &&
                  !smc.RetireNode(space, &l1, module, function),
                  "ordinary cleanup retry releases the retained allocation exactly once");
        }
    }
}

void OrdinarySourceReaders(bool range) {
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink;
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    const u64 source_pc = 0x100, target_pc = static_cast<u64>(getpagesize()) + source_pc;
    const auto compile = [&](bool source) {
        HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
        const u64 pc = source ? source_pc : target_pc;
        auto* function = builder.AppendFunction(Location{pc}, Location{pc + 4});
        function->StoreUniform(Uniform{source ? 8u : 0u, ValueType::U64},
                               function->LoadImm(ValueType::U64, Imm{u64{37}}));
        if (source)
            function->EndBlock(terminal::LinkBlock{Location{target_pc}});
        else function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        return static_cast<u8*>(TranslateIR(module, function));
    };
    auto* target = compile(false);
    auto* source = compile(true);
    Check(source && target && module->IsDirectLinkConfigured(), "compile real source and target owners");
    {
        Runtime runtime{&space};
        for (unsigned warm = 0; warm < 2; ++warm) {
            runtime.SetLocation(source_pc);
            (void)runtime.Run();
            u64 value{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
            Check(value == 37, "actual source branch executes the neighboring target body");
        }
    }
    FaultEntry allocation;
    Check(module->LookupCodeAllocation(source, allocation), "bind the actual source allocation and faults");
    const LinkSourceOwner owner{module.get(), allocation.host_start};
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 1 && sites.front().state == LinkSiteState::Linked,
          "production compilation and execution create one actually linked outgoing site");
    const auto site = sites.front().site;
    const auto region = module->GetCodeRegion(site.region_id);
    Check(region.has_value(), "bind the actual outgoing site's RX and RW region");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::atomic<bool> executing{}, end_executor{};
    std::exception_ptr executor_error;
    std::thread executor;
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    watched_code_free = SiteRxToRw(*region, allocation.host_start);
    watched_code_frees = 0;
    watched_purge_allocation = allocation.host_start;
    purge_wait_observed = purge_completed = free_before_purge = false;
    const auto finish = ScopeExit([&] {
        end_executor = true;
        reader.reset();
        if (executor.joinable()) executor.join();
        watched_purge_allocation = nullptr;
        watched_code_free = nullptr;
    });
    executor = std::thread([&] {
        try {
            SmcTracker::ExecutionScope active{smc, token};
            executing = true;
            while (!end_executor) std::this_thread::yield();
            active.End();
        } catch (...) { executor_error = std::current_exception(); executing = true; }
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    Check(wait_until(executing), "enter actual SMC executor before ordinary retirement");
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the exact normally compiled source node");
    if (range) smc.InvalidateRange(space, &l1, source_pc, source_pc + 4);
    else Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()), "retire actual outgoing source node");
    const auto retired = links.QuerySite(site);
    Check(retired && retired->state == LinkSiteState::Retiring && watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && module->LookupCodeAllocation(source, allocation),
          "ordinary detach keeps the real outgoing owner and fault metadata throughout executor grace");
    const auto invalidated = links.SignalInvalidateTarget(target_pc);
    u32 instruction{};
    std::memcpy(&instruction, region->rx_base + site.offset, sizeof(instruction));
    Check(invalidated.found && invalidated.linked_sites == 1 && instruction == sites.front().unlinked_instruction,
          "signal invalidation can still restore the real detached source's branch during executor grace");
    end_executor = true;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge && watched_code_frees == 0 &&
          Mapped(source) && Mapped(watched_code_free.load()),
          "actual PurgeSource reaches its reader wait before the exact source allocation can be freed");
    reader.reset();
    executor.join();
    if (executor_error) std::rethrow_exception(executor_error);
    Check(purge_completed && !free_before_purge && watched_code_frees == 1 &&
          !links.QuerySite(site) && links.QuerySourceSites(owner).empty() &&
          !module->LookupCodeAllocation(source, allocation),
          "reader exit completes ordinary source purge before its one exact allocation free");
    watched_purge_allocation = nullptr;
    Runtime runtime{&space};
    runtime.SetLocation(target_pc);
    (void)runtime.Run();
    u64 value{};
    std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
    Check(value == 37 && module->GetJitCache(Location{target_pc}) == target,
          "the neighboring target remains executable after ordinary source reclamation");
}

u64 DualExecutorCallback() noexcept {
    const auto ticket = dual_executor_inside.fetch_add(1);
    while (dual_executor_released.load() <= ticket) std::this_thread::yield();
    return 84;
}

std::pair<aot::AotImage, SerialUnit> RestoredSourceFixture(const Config& config, void* guest,
        const aot::AotImage& target_image, VAddr target_pc, VAddr source_pc, VAddr secondary_pc, u64 witness = 91) {
    auto image = target_image;
    SerialUnit captured;
    {
        AddressSpace source{config};
        source.SetSerializableCodeEmission();
        const auto module = source.GetDefaultModule();
        HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
        auto* function = builder.AppendFunction(Location{source_pc}, Location{source_pc + 4});
        function->StoreUniform(Uniform{16, ValueType::U64},
                               function->LoadImm(ValueType::U64, Imm{witness}));
        function->EndBlock(terminal::LinkBlock{Location{target_pc}});
        function->EndFunction();
        auto* entry = static_cast<u8*>(TranslateIR(module, function));
        FaultEntry extent;
        Check(entry && module->LookupCodeAllocation(entry, extent), "compile the actual outgoing source allocation");
        std::string error;
        Check(CaptureSerialCode(*module, extent.host_start, u32(extent.host_end - extent.host_start),
              GetHostImage(), config.loc_end, captured, &error), error.c_str());
        const auto target = source.GetLinkManager().QueryTarget(source_pc);
        Check(target && captured.link_sites.size() == 1, "capture a real cross-unit branch and complete source entries");
        const auto offset = [&](const void* pointer) -> u32 {
            if (!pointer) return UINT32_MAX;
            const auto* address = static_cast<const u8*>(pointer);
            Check(address >= extent.host_start && address < extent.host_end, "captured source entry belongs to its allocation");
            return u32(address - extent.host_start);
        };
        SerialBlock block{source_pc, source_pc + 4, offset(target->host_pc),
            HashBytes(static_cast<u8*>(guest) + source_pc, 4, 0xCBF29CE484222325ull),
            offset(target->direct_host_pc), offset(target->pending_flags_host_pc), target->pending_flags_contract};
        block.continuation_code_offset = offset(target->call_host_pc);
        block.pending_flags_continuation_code_offset = offset(target->call_pending_flags_host_pc);
        captured.blocks = {block};
        source.GetCodeCacheTable().ForEachEntry([&](u32 index, size_t key, size_t) {
            image.dispatch_slots.emplace_back(u64(key), index);
        });
    }
    const auto body_size = u32(captured.code.size());
    const auto first_body = captured;
    captured.code.insert(captured.code.end(), first_body.code.begin(), first_body.code.end());
    for (auto reloc : first_body.relocs) { reloc.code_offset += body_size; captured.relocs.push_back(reloc); }
    for (auto branch : first_body.region_branches) { branch.code_offset += body_size; captured.region_branches.push_back(branch); }
    for (auto site : first_body.link_sites) {
        site.code_offset += body_size;
        for (auto* offset : {&site.flags_bypass_offset, &site.flags_bypass_resume_offset, &site.flags_merge_branch_offset})
            if (*offset != UINT32_MAX) *offset += body_size;
        captured.link_sites.push_back(site);
    }
    for (auto fault : first_body.fault_sites) {
        fault.host_begin += body_size; fault.host_end += body_size;
        if (fault.recovery_offset != UINT32_MAX) fault.recovery_offset += body_size;
        fault.guest_start = secondary_pc;
        captured.fault_sites.push_back(fault);
    }
    auto secondary = captured.blocks.front();
    secondary.guest_start = secondary_pc; secondary.guest_end = secondary_pc + 4;
    for (auto* offset : {&secondary.code_offset, &secondary.direct_code_offset, &secondary.pending_flags_code_offset,
                        &secondary.continuation_code_offset, &secondary.pending_flags_continuation_code_offset})
        if (*offset != UINT32_MAX) *offset += body_size;
    captured.blocks.push_back(secondary);
    captured.guest_start = source_pc; captured.is_function = 1;
    captured.fault_sites.push_back({source_pc, 0, u32(captured.code.size())});
    aot::AotUnit unit{};
    unit.guest_start = source_pc; unit.code_size = captured.code.size(); unit.is_function = 1;
    unit.blocks = captured.blocks; unit.relocs = captured.relocs; unit.fault_sites = captured.fault_sites;
    unit.link_sites = captured.link_sites; unit.region_branches = captured.region_branches;
    image.units = {unit}; image.code = captured.code;
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    return {std::move(image), std::move(captured)};
}

void RestoredSourceReaders(bool aot_mode, std::string_view path) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-restored-readers-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-restored-readers-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the actual restored linked-source disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const auto target_image = Artifact(config, guest.memory, true);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    unsigned cases{};
    for (const auto metadata : {SerialBlock::MetadataOnly, SerialBlock::DependencyOnly}) for (const bool reverse : {false, true}) {
        AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        aot::LoadReport target_report, report;
        std::string error;
        Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
              "install the independent target before restoring the two-entry source");
        auto incoming = image;
        incoming.units.front().blocks.push_back({.guest_start = target_pc, .guest_end = target_pc + 4,
            .code_offset = UINT32_MAX,
            .guest_bytes_hash = HashBytes(static_cast<u8*>(guest.memory) + target_pc, 4, 0xCBF29CE484222325ull),
            .entry_flags = u8(metadata)});
        if (reverse) std::reverse(incoming.units.front().blocks.begin(), incoming.units.front().blocks.end());
        if (aot_mode) {
            const auto status = aot::InstallArtifact(space, incoming, guest.memory, config.guest_addr_mask, report, error);
            Check(status == aot::kAotOk && report.units_installed == 1,
                  error.empty() ? "restore the actual linked two-entry AOT source" : error.c_str());
        } else {
            auto disk = captured;
            disk.blocks = incoming.units.front().blocks;
            disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
            // Disk revival rebuilds its allocation owner; ARM serial faults
            // carry only precise instruction ranges, unlike AOT's owner span.
            if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
                return site.host_end != site.host_begin + sizeof(u32);
            });
            BlobWriter payload;
            payload.U64(incoming.dispatch_slots.size());
            for (const auto& [key, index] : incoming.dispatch_slots) { payload.U64(key); payload.U32(index); }
            payload.U64(1); WriteUnit(payload, disk);
            const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
                ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
            BlobWriter file;
            constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
            file.Bytes(magic, sizeof(magic));
            for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                    u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
            file.Bytes(payload.Data().data(), payload.Size());
            auto name = HashU64(key.build_id, key.guest_id);
            name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
            std::array<char, 512> filename{};
            std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
            {
                std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
                output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
                Check(output.good(), "write the actual linked two-entry disk fixture");
            }
            space.LoadJitCache();
            Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the linked source");
            Check(unlink(filename.data()) == 0, "remove the owned linked-source disk fixture");
        }
        auto& links = space.GetLinkManager();
        auto& smc = space.GetSmcTracker();
        const auto node = module->GetNode(Location{source_pc});
        Check(IsFunction(node), "retain the exact restored source node");
        const auto lifetime = GetFunction(node)->code_lifetime;
        const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
        Check(lifetime && region && lifetime->entries.size() == 2, "bind both published entries to one restored allocation");
        const LinkSourceOwner owner{module.get(), lifetime->base};
        const auto target = links.QueryTarget(target_pc);
        const auto target_entry = space.GetCodeCacheTable().Lookup(target_pc);
        for (const auto pc : {source_pc, secondary_pc}) {
            Runtime runtime{&space};
            for (unsigned warm = 0; warm < 2; ++warm) {
                runtime.SetLocation(pc);
                Check(runtime.Run() == HaltReason::CallHost, "execute each restored source entry through the real target");
                u64 value{}, witness{};
                std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
                std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
                Check(value == 37 && witness == 91, "both physical source bodies and the target execute");
            }
        }
        const auto sites = links.QuerySourceSites(owner);
        Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
            return site.state == LinkSiteState::Linked;
        }), "both restored physical branches become actually linked");
        TranslateTable l1{8}, other_l1{8};
        const auto token = smc.RegisterRuntime(l1), other_token = smc.RegisterRuntime(other_l1);
        const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(other_token); smc.UnregisterRuntime(token); });
        for (const auto pc : {target_pc, source_pc, secondary_pc}) {
            const auto entry = space.GetCodeCacheTable().Lookup(pc);
            l1.Put(pc, entry); other_l1.Put(pc, entry);
        }
        std::atomic<bool> executing{}, end_executor{};
        std::exception_ptr executor_error;
        std::thread executor;
        std::optional<LinkManager::SignalReader> reader{std::in_place, links};
        watched_code_free = SiteRxToRw(*region, lifetime->base); watched_code_frees = 0;
        watched_purge_allocation = lifetime->base;
        purge_wait_observed = purge_completed = free_before_purge = false;
        const auto finish = ScopeExit([&] {
            end_executor = true; reader.reset();
            if (executor.joinable()) executor.join();
            watched_purge_allocation = nullptr; watched_code_free = nullptr;
        });
        executor = std::thread([&] {
            try {
                SmcTracker::ExecutionScope active{smc, token};
                executing = true;
                while (!end_executor) std::this_thread::yield();
                active.End();
            } catch (...) { executor_error = std::current_exception(); executing = true; }
        });
        const auto wait_until = [](const std::atomic<bool>& flag) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            return flag.load();
        };
        Check(wait_until(executing), "enter executor grace before restored-source retirement");
        if (path == "node") Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()), "retire the linked restored source node");
        else if (path == "range") smc.InvalidateRange(space, &l1, secondary_pc, secondary_pc + 4);
        else {
            Check(smc.HandleWriteFault(space, l1, reinterpret_cast<uintptr_t>(guest.memory) + secondary_pc),
                  "open a real write window on the restored secondary entry");
            smc.CloseWriteWindow(space, l1);
        }
        Check(lifetime->Revoked() && watched_code_frees == 0 && IsEmpty(module->GetNode(Location{source_pc})),
              "restored source detaches and keeps its allocation throughout executor grace");
        for (const auto pc : {source_pc, secondary_pc}) Check(!space.GetCodeCacheTable().Lookup(pc) &&
            !space.GetCallCodeCacheTable().Lookup(pc) && !space.GetPendingCallCodeCacheTable().Lookup(pc) &&
            !l1.Lookup(pc) && !other_l1.Lookup(pc) && !links.QueryTarget(pc), "all restored entry surfaces and registered L1s withdraw");
        const auto preserved = links.QueryTarget(target_pc);
        Check(target && preserved && preserved->generation == target->generation &&
              space.GetCodeCacheTable().Lookup(target_pc) == target_entry && l1.Lookup(target_pc) == target_entry &&
              other_l1.Lookup(target_pc) == target_entry, "metadata/dependency overlap preserves the independent target and both L1s");
        const auto invalidated = links.SignalInvalidateTarget(target_pc);
        Check(invalidated.found && invalidated.linked_sites == 2, "signal invalidation still visits both detached physical branches");
        for (const auto& site : sites) {
            u32 instruction{}; std::memcpy(&instruction, region->rx_base + site.site.offset, sizeof(instruction));
            Check(instruction == site.unlinked_instruction, "signal reader restores the exact detached branch instruction");
        }
        end_executor = true;
        Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge && watched_code_frees == 0 &&
              Mapped(lifetime->base) && Mapped(watched_code_free.load()), "real purge waits for signal-reader grace with both code views retained");
        reader.reset(); executor.join();
        if (executor_error) std::rethrow_exception(executor_error);
        FaultEntry absent;
        Check(purge_completed && !free_before_purge && watched_code_frees == 1 && links.QuerySourceSites(owner).empty() &&
              !module->LookupCodeAllocation(lifetime->base, absent), "signal-reader exit purges both sources before one exact allocation free");
        Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()), "repeated restored-source retirement cannot free twice");
        Runtime runtime{&space}; runtime.SetLocation(target_pc);
        Check(runtime.Run() == HaltReason::CallHost && space.GetCodeCacheTable().Lookup(target_pc) == target_entry,
              "the independent target still executes after restored-source reclamation");
        u64 answer{}; std::memcpy(&answer, runtime.GetUniformBuffer().data(), 8);
        Check(answer == 37, "the preserved target retains its production result");
        ++cases;
    }
    std::printf("restored_readers backend=%s path=%.*s cases=%u entries=2 linked_sites=2 executor_wait=1 reader_wait=1 frees=%u\n",
                aot_mode ? "aot" : "disk", int(path.size()), path.data(), cases, cases);
}

void LinkedRevivalFailure(bool aot_mode, std::string_view mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-linked-failure-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-linked-failure-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate real linked multi-entry restoration failures");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const auto target_image = Artifact(config, guest.memory, true);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    const bool sweep = mode != "protect", terminal = mode == "terminal";
    const bool compound = mode == "cleanup" || terminal;
    unsigned failures{}, cleanup_failures{}, successes{}, reader_waits{}, terminal_releases{};
    for (const auto metadata : {SerialBlock::MetadataOnly, SerialBlock::DependencyOnly}) for (const bool reverse : {false, true}) {
        for (const bool primary_oom : {true, false}) {
            if ((mode == "oom" && !primary_oom) || (mode == "protect" && primary_oom)) continue;
            bool completed{};
            unsigned shape_failures{};
            for (unsigned index = 0; index < (sweep && primary_oom ? 256u : 1u); ++index) {
                AddressSpace space{config};
                const auto module = space.GetDefaultModule();
                auto& smc = space.GetSmcTracker();
                auto& links = space.GetLinkManager();
                aot::LoadReport target_report, report;
                std::string error;
                Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
                      "install the independent target before each linked failure attempt");
                const auto target = links.QueryTarget(target_pc);
                const std::array<size_t, 3> target_entries{space.GetCodeCacheTable().Lookup(target_pc),
                    space.GetCallCodeCacheTable().Lookup(target_pc), space.GetPendingCallCodeCacheTable().Lookup(target_pc)};
                auto* target_alias = aot::ResolveCodeOffset(target_report.mapping, 0);
                Check(target && target_entries[0] && target_alias, "bind the independent target's native ownership surfaces");
                auto incoming = image;
                incoming.units.front().blocks.push_back({.guest_start = target_pc, .guest_end = target_pc + 4,
                    .code_offset = UINT32_MAX,
                    .guest_bytes_hash = HashBytes(static_cast<u8*>(guest.memory) + target_pc, 4, 0xCBF29CE484222325ull),
                    .entry_flags = u8(metadata)});
                if (reverse) std::reverse(incoming.units.front().blocks.begin(), incoming.units.front().blocks.end());
                std::string filename;
                if (!aot_mode) {
                    auto disk = captured;
                    disk.blocks = incoming.units.front().blocks;
                    disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
                    if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
                        return site.host_end != site.host_begin + sizeof(u32);
                    });
                    BlobWriter payload;
                    payload.U64(incoming.dispatch_slots.size());
                    for (const auto& [key, slot] : incoming.dispatch_slots) { payload.U64(key); payload.U32(slot); }
                    payload.U64(1); WriteUnit(payload, disk);
                    const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
                        ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
                    BlobWriter file;
                    constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
                    file.Bytes(magic, sizeof(magic));
                    for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                            u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
                    file.Bytes(payload.Data().data(), payload.Size());
                    auto name = HashU64(key.build_id, key.guest_id);
                    name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
                    std::array<char, 512> path{};
                    std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
                    filename = path.data();
                    std::ofstream output(filename, std::ios::binary | std::ios::trunc);
                    output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
                    Check(output.good(), "write the valid linked two-entry failure fixture");
                }
                TranslateTable l1{8}, other_l1{8};
                const auto token = smc.RegisterRuntime(l1), other_token = smc.RegisterRuntime(other_l1);
                bool registered = true;
                const auto unregister = ScopeExit([&] { if (registered) { smc.UnregisterRuntime(other_token); smc.UnregisterRuntime(token); } });
                l1.Put(target_pc, target_entries[0]); other_l1.Put(target_pc, target_entries[0]);
                std::atomic<bool> executing{}, end_executor{};
                std::exception_ptr executor_error, retry_error;
                std::thread executor, retry;
                std::optional<LinkManager::SignalReader> reader{std::in_place, links};
                watched_code_free = nullptr; watched_code_frees = 0;
                watched_purge_allocation = nullptr;
                purge_wait_observed = purge_completed = free_before_purge = false;
                const auto finish = ScopeExit([&] {
                    end_executor = true; reader.reset();
                    if (executor.joinable()) executor.join();
                    if (retry.joinable()) retry.join();
                    watched_code_free = nullptr; watched_purge_allocation = nullptr;
                    watch_aot_allocation = fail_after_aot_push = fail_aot_protect = false;
                    capture_published_lifetime = inject_aot_cleanup = fail_cleanup_allocation = false;
                    published_failure_lifetime.reset(); fail_aot_allocation = -1; post_push_failure_index = 0;
                });
                const auto wait_until = [](const std::atomic<bool>& flag) {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
                    return flag.load();
                };
                executor = std::thread([&] {
                    try {
                        SmcTracker::ExecutionScope active{smc, token};
                        executing = true;
                        while (!end_executor) std::this_thread::yield();
                        active.End();
                    } catch (...) { executor_error = std::current_exception(); executing = true; }
                });
                Check(wait_until(executing), "admit a real executor before restoration fault injection");
                watched_aot_size = incoming.code.size(); post_push_failure_index = index;
                aot_failure_hit = aot_was_published = aot_published_at_failure = false;
                aot_protection_failure_hit = cleanup_query_observed = cleanup_failure_hit = false;
                capture_published_lifetime = watch_aot_allocation = true;
                inject_aot_cleanup = compound; fail_after_aot_push = primary_oom; fail_aot_protect = !primary_oom;
                bool escaped{};
                int status = -1;
                try {
                    if (aot_mode) status = aot::InstallArtifact(space, incoming, guest.memory, config.guest_addr_mask, report, error);
                    else space.LoadJitCache();
                } catch (...) { escaped = true; }
                watch_aot_allocation = capture_published_lifetime = fail_after_aot_push = fail_aot_protect = false;
                fail_aot_allocation = -1;
                const bool hit = primary_oom ? aot_failure_hit : aot_protection_failure_hit;
                const auto lifetime = published_failure_lifetime;
                Check(aot_was_published && lifetime && watched_code_free,
                      "the valid linked source reached actual Module publication and exact native allocation");
                const auto region = module->GetCodeRegion(lifetime->base);
                Check(region && SiteRxToRw(*region, lifetime->base) == watched_code_free,
                      "bind both aliases of the exact linked restoration allocation");
                const LinkSourceOwner owner{module.get(), lifetime->base};
                watched_purge_allocation = lifetime->base;
                const auto preserve_target = [&] {
                    const auto current = links.QueryTarget(target_pc);
                    Check(current && current->generation == target->generation && current->host_pc == target->host_pc &&
                          target_entries == std::array<size_t, 3>{space.GetCodeCacheTable().Lookup(target_pc),
                              space.GetCallCodeCacheTable().Lookup(target_pc), space.GetPendingCallCodeCacheTable().Lookup(target_pc)} &&
                          l1.Lookup(target_pc) == target_entries[0] && other_l1.Lookup(target_pc) == target_entries[0] &&
                          aot::ResolveCodeOffset(target_report.mapping, 0) == target_alias,
                          "linked restoration failure preserves the independently owned entries, L1s, generation and alias");
                };
                if (!hit) {
                    Check(sweep && !escaped && lifetime->Live() && (aot_mode ? status == aot::kAotOk && report.units_installed == 1
                          : space.GetJitDiskCache()->Stats().units_loaded == 1), "the complete linked post-Push OOM sweep reaches a committed source");
                    preserve_target();
                    end_executor = true; executor.join(); reader.reset();
                    if (executor_error) std::rethrow_exception(executor_error);
                    for (const auto pc : {source_pc, secondary_pc}) {
                        Runtime runtime{&space}; runtime.SetLocation(pc);
                        Check(runtime.Run() == HaltReason::CallHost, "the final sweep case executes each physical restored source body");
                        u64 value{}, witness{};
                        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
                        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
                        Check(value == 37 && witness == 91, "successful multi-entry revival preserves source and target witnesses");
                    }
                    Check(smc.RetireNode(space, &l1, module, GetFunction(module->GetNode(Location{source_pc})).get()) &&
                          watched_code_frees == 1, "successful sweep endpoint retains ordinary one-time reclamation");
                    ++successes; completed = true;
                } else {
                    ++failures; ++shape_failures;
                    std::printf("linked_failure_point backend=%s mode=%.*s metadata=%u reverse=%u primary=%s index=%u\n",
                                aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), unsigned(metadata), reverse,
                                primary_oom ? "oom" : "protect", index);
                    Check((aot_mode ? (escaped || status != aot::kAotOk) && !report.mapping : !escaped && space.GetJitDiskCache()->Stats().units_loaded == 0) &&
                          lifetime->Revoked() && !lifetime->Commit() && watched_code_frees == 0,
                          "failed multi-entry restoration is inert and cannot free before executor exit");
                    for (const auto pc : {source_pc, secondary_pc}) Check(!space.GetCodeCacheTable().Lookup(pc) &&
                        !space.GetCallCodeCacheTable().Lookup(pc) && !space.GetPendingCallCodeCacheTable().Lookup(pc) && !links.QueryTarget(pc),
                        "rollback withdraws every new canonical, Call, pending Call and link entry");
                    preserve_target();
                    const auto node = module->GetNode(Location{source_pc});
                    if (compound) {
                        Check(cleanup_query_observed && cleanup_failure_hit && !inject_aot_cleanup && !fail_cleanup_allocation &&
                              IsFunction(node) && GetFunction(node)->code_lifetime == lifetime && links.QuerySourceSites(owner).size() == 2,
                              "independent cleanup OOM retains both real outgoing sites with the inert original owner");
                        ++cleanup_failures;
                        end_executor = true; executor.join();
                        if (executor_error) std::rethrow_exception(executor_error);
                        Check(watched_code_frees == 0, "executor exit cannot consume unsubmitted compound cleanup");
                    } else Check(IsEmpty(node), "ordinary linked rollback detaches its failed published source");
                    if (terminal) {
                        reader.reset();
                        smc.UnregisterRuntime(other_token); smc.UnregisterRuntime(token); registered = false;
                        auto* rx = lifetime->base; auto* rw = watched_code_free.load();
                        const auto ticket = Terminal(space);
                        Check(space.TryShutdown(ticket).Complete() && !Mapped(rx) && !Mapped(rw) && watched_code_frees == 0,
                              "terminal cleanup releases the retained linked source through both actual arena views");
                        ++terminal_releases;
                    } else {
                        bool retired{};
                        if (compound) retry = std::thread([&] {
                            try { retired = smc.RetireNode(space, &l1, module, GetFunction(node).get()); }
                            catch (...) { retry_error = std::current_exception(); }
                        });
                        else end_executor = true;
                        Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge && watched_code_frees == 0 &&
                              Mapped(lifetime->base) && Mapped(watched_code_free.load()),
                              "failed linked-source reclamation waits for the real signal reader before either allocation alias is reused");
                        reader.reset();
                        if (retry.joinable()) retry.join();
                        if (executor.joinable()) executor.join();
                        if (executor_error) std::rethrow_exception(executor_error);
                        if (retry_error) std::rethrow_exception(retry_error);
                        FaultEntry absent;
                        Check((!compound || retired) && purge_completed && !free_before_purge && watched_code_frees == 1 &&
                              links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent) &&
                              IsEmpty(module->GetNode(Location{source_pc})), "reader exit purges both failed source sites before one exact free");
                        ++reader_waits;
                        preserve_target();
                        Runtime runtime{&space}; runtime.SetLocation(target_pc);
                        Check(runtime.Run() == HaltReason::CallHost, "the independent target executes after linked compound cleanup");
                        u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
                        Check(value == 37, "linked failure preserves the original native target result");
                    }
                    if (!sweep || !primary_oom) completed = true;
                }
                if (!filename.empty()) Check(unlink(filename.c_str()) == 0, "remove this owned linked failure fixture");
                if (completed) break;
            }
            Check(completed, "each linked failure shape reaches its exact final case");
            std::printf("linked_failure_shape backend=%s mode=%.*s metadata=%u reverse=%u primary=%s failures=%u\n",
                        aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), unsigned(metadata), reverse,
                        primary_oom ? "oom" : "protect", shape_failures);
        }
    }
    std::printf("linked_failure backend=%s mode=%.*s failures=%u cleanup_failures=%u successes=%u reader_waits=%u terminal_releases=%u entries=2 sites=2\n",
                aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), failures, cleanup_failures, successes, reader_waits, terminal_releases);
}

void LinkedCallFailure(bool aot_mode, std::string_view mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-linked-call-failure-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-linked-call-failure-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate real linked multi-entry restoration failures");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, return_pc = source_pc + 0x80;
    const auto target_image = ReturningTargetArtifact(config, guest.memory);
    const auto [image, captured] = RestoredCallFixture(config, guest.memory, target_image, target_pc, source_pc, return_pc);
    const bool sweep = mode != "protect", terminal = mode == "terminal";
    const bool compound = mode == "cleanup" || terminal;
    unsigned failures{}, cleanup_failures{}, successes{}, reader_waits{}, terminal_releases{};
    for (const auto metadata : {SerialBlock::MetadataOnly, SerialBlock::DependencyOnly}) for (const bool reverse : {false, true}) {
        for (const bool primary_oom : {true, false}) {
            if ((mode == "oom" && !primary_oom) || (mode == "protect" && primary_oom)) continue;
            bool completed{};
            unsigned shape_failures{};
            for (unsigned index = 0; index < (sweep && primary_oom ? 256u : 1u); ++index) {
                AddressSpace space{config};
                const auto module = space.GetDefaultModule();
                auto& smc = space.GetSmcTracker();
                auto& links = space.GetLinkManager();
                aot::LoadReport target_report, report;
                std::string error;
                Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
                      "install the independent target before each linked failure attempt");
                const auto target = links.QueryTarget(target_pc);
                const std::array<size_t, 3> target_entries{space.GetCodeCacheTable().Lookup(target_pc),
                    space.GetCallCodeCacheTable().Lookup(target_pc), space.GetPendingCallCodeCacheTable().Lookup(target_pc)};
                auto* target_alias = aot::ResolveCodeOffset(target_report.mapping, 0);
                Check(target && target_entries[0] && target_alias, "bind the independent target's native ownership surfaces");
                auto incoming = image;
                incoming.units.front().blocks.push_back({.guest_start = target_pc, .guest_end = target_pc + 4,
                    .code_offset = UINT32_MAX,
                    .guest_bytes_hash = HashBytes(static_cast<u8*>(guest.memory) + target_pc, 4, 0xCBF29CE484222325ull),
                    .entry_flags = u8(metadata)});
                if (reverse) std::reverse(incoming.units.front().blocks.begin(), incoming.units.front().blocks.end());
                std::string filename;
                if (!aot_mode) {
                    auto disk = captured;
                    disk.blocks = incoming.units.front().blocks;
                    disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
                    if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
                        return site.host_end != site.host_begin + sizeof(u32);
                    });
                    BlobWriter payload;
                    payload.U64(incoming.dispatch_slots.size());
                    for (const auto& [key, slot] : incoming.dispatch_slots) { payload.U64(key); payload.U32(slot); }
                    payload.U64(1); WriteUnit(payload, disk);
                    const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
                        ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
                    BlobWriter file;
                    constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
                    file.Bytes(magic, sizeof(magic));
                    for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                            u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
                    file.Bytes(payload.Data().data(), payload.Size());
                    auto name = HashU64(key.build_id, key.guest_id);
                    name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
                    std::array<char, 512> path{};
                    std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
                    filename = path.data();
                    std::ofstream output(filename, std::ios::binary | std::ios::trunc);
                    output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
                    Check(output.good(), "write the valid linked two-entry failure fixture");
                }
                TranslateTable l1{8}, other_l1{8};
                const auto token = smc.RegisterRuntime(l1), other_token = smc.RegisterRuntime(other_l1);
                bool registered = true;
                const auto unregister = ScopeExit([&] { if (registered) { smc.UnregisterRuntime(other_token); smc.UnregisterRuntime(token); } });
                l1.Put(target_pc, target_entries[0]); other_l1.Put(target_pc, target_entries[0]);
                std::atomic<bool> executing{}, end_executor{};
                std::exception_ptr executor_error, retry_error;
                std::thread executor, retry;
                std::optional<LinkManager::SignalReader> reader{std::in_place, links};
                watched_code_free = nullptr; watched_code_frees = 0;
                watched_purge_allocation = nullptr;
                purge_wait_observed = purge_completed = free_before_purge = false;
                const auto finish = ScopeExit([&] {
                    end_executor = true; reader.reset();
                    if (executor.joinable()) executor.join();
                    if (retry.joinable()) retry.join();
                    watched_code_free = nullptr; watched_purge_allocation = nullptr;
                    watch_aot_allocation = fail_after_aot_push = fail_aot_protect = false;
                    capture_published_lifetime = inject_aot_cleanup = fail_cleanup_allocation = false;
                    published_failure_lifetime.reset(); fail_aot_allocation = -1; post_push_failure_index = 0;
                });
                const auto wait_until = [](const std::atomic<bool>& flag) {
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
                    return flag.load();
                };
                executor = std::thread([&] {
                    try {
                        SmcTracker::ExecutionScope active{smc, token};
                        executing = true;
                        while (!end_executor) std::this_thread::yield();
                        active.End();
                    } catch (...) { executor_error = std::current_exception(); executing = true; }
                });
                Check(wait_until(executing), "admit a real executor before restoration fault injection");
                watched_aot_size = incoming.code.size(); post_push_failure_index = index;
                aot_failure_hit = aot_was_published = aot_published_at_failure = false;
                aot_protection_failure_hit = cleanup_query_observed = cleanup_failure_hit = false;
                capture_published_lifetime = watch_aot_allocation = true;
                inject_aot_cleanup = compound; fail_after_aot_push = primary_oom; fail_aot_protect = !primary_oom;
                bool escaped{};
                int status = -1;
                try {
                    if (aot_mode) status = aot::InstallArtifact(space, incoming, guest.memory, config.guest_addr_mask, report, error);
                    else space.LoadJitCache();
                } catch (...) { escaped = true; }
                watch_aot_allocation = capture_published_lifetime = fail_after_aot_push = fail_aot_protect = false;
                fail_aot_allocation = -1;
                const bool hit = primary_oom ? aot_failure_hit : aot_protection_failure_hit;
                const auto lifetime = published_failure_lifetime;
                Check(aot_was_published && lifetime && watched_code_free,
                      "the valid linked source reached actual Module publication and exact native allocation");
                const auto region = module->GetCodeRegion(lifetime->base);
                Check(region && SiteRxToRw(*region, lifetime->base) == watched_code_free,
                      "bind both aliases of the exact linked restoration allocation");
                const LinkSourceOwner owner{module.get(), lifetime->base};
                watched_purge_allocation = lifetime->base;
                const auto preserve_target = [&] {
                    const auto current = links.QueryTarget(target_pc);
                    Check(current && current->generation == target->generation && current->host_pc == target->host_pc &&
                          target_entries == std::array<size_t, 3>{space.GetCodeCacheTable().Lookup(target_pc),
                              space.GetCallCodeCacheTable().Lookup(target_pc), space.GetPendingCallCodeCacheTable().Lookup(target_pc)} &&
                          l1.Lookup(target_pc) == target_entries[0] && other_l1.Lookup(target_pc) == target_entries[0] &&
                          aot::ResolveCodeOffset(target_report.mapping, 0) == target_alias,
                          "linked restoration failure preserves the independently owned entries, L1s, generation and alias");
                };
                if (!hit) {
                    Check(sweep && !escaped && lifetime->Live() && (aot_mode ? status == aot::kAotOk && report.units_installed == 1
                          : space.GetJitDiskCache()->Stats().units_loaded == 1), "the complete linked post-Push OOM sweep reaches a committed source");
                    preserve_target();
                    end_executor = true; executor.join(); reader.reset();
                    if (executor_error) std::rethrow_exception(executor_error);
                    {
                        Runtime runtime{&space};
                        const u64 resume = return_pc;
                        std::memcpy(runtime.GetUniformBuffer().data() + 16, &resume, 8);
                        runtime.SetLocation(source_pc);
                        Check(runtime.Run() == HaltReason::CallHost, "the final sweep case executes the restored Call source and returning target");
                        u64 value{}, witness{};
                        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
                        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 24, 8);
                        Check(value == 37 && witness == 0x51ab, "successful Call revival preserves target and return-body witnesses");
                        const auto sites = links.QuerySourceSites(owner);
                        Check(sites.size() == 1 &&
                              sites.front().kind == (FlagsRegsEnabled() ? LinkSiteKind::Call : LinkSiteKind::Unconditional) &&
                              sites.front().state == LinkSiteState::Linked,
                              "successful Call revival links its real static Call site");
                    }
                    Check(smc.RetireNode(space, &l1, module, GetFunction(module->GetNode(Location{source_pc})).get()) &&
                          watched_code_frees == 1, "successful sweep endpoint retains ordinary one-time reclamation");
                    ++successes; completed = true;
                } else {
                    ++failures; ++shape_failures;
                    std::printf("linked_call_failure_point backend=%s mode=%.*s metadata=%u reverse=%u primary=%s index=%u\n",
                                aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), unsigned(metadata), reverse,
                                primary_oom ? "oom" : "protect", index);
                    Check((aot_mode ? (escaped || status != aot::kAotOk) && !report.mapping : !escaped && space.GetJitDiskCache()->Stats().units_loaded == 0) &&
                          lifetime->Revoked() && !lifetime->Commit() && watched_code_frees == 0,
                          "failed multi-entry restoration is inert and cannot free before executor exit");
                    for (const auto pc : {source_pc, return_pc}) Check(!space.GetCodeCacheTable().Lookup(pc) &&
                        !space.GetCallCodeCacheTable().Lookup(pc) && !space.GetPendingCallCodeCacheTable().Lookup(pc) && !links.QueryTarget(pc),
                        "rollback withdraws every new canonical, Call, pending Call and link entry");
                    preserve_target();
                    const auto node = module->GetNode(Location{source_pc});
                    if (compound) {
                        Check(cleanup_query_observed && cleanup_failure_hit && !inject_aot_cleanup && !fail_cleanup_allocation &&
                              IsFunction(node) && GetFunction(node)->code_lifetime == lifetime && links.QuerySourceSites(owner).size() == 1,
                              "independent cleanup OOM retains both real outgoing sites with the inert original owner");
                        ++cleanup_failures;
                        end_executor = true; executor.join();
                        if (executor_error) std::rethrow_exception(executor_error);
                        Check(watched_code_frees == 0, "executor exit cannot consume unsubmitted compound cleanup");
                    } else Check(IsEmpty(node), "ordinary linked rollback detaches its failed published source");
                    if (terminal) {
                        reader.reset();
                        smc.UnregisterRuntime(other_token); smc.UnregisterRuntime(token); registered = false;
                        auto* rx = lifetime->base; auto* rw = watched_code_free.load();
                        const auto ticket = Terminal(space);
                        Check(space.TryShutdown(ticket).Complete() && !Mapped(rx) && !Mapped(rw) && watched_code_frees == 0,
                              "terminal cleanup releases the retained linked source through both actual arena views");
                        ++terminal_releases;
                    } else {
                        bool retired{};
                        if (compound) retry = std::thread([&] {
                            try { retired = smc.RetireNode(space, &l1, module, GetFunction(node).get()); }
                            catch (...) { retry_error = std::current_exception(); }
                        });
                        else end_executor = true;
                        Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge && watched_code_frees == 0 &&
                              Mapped(lifetime->base) && Mapped(watched_code_free.load()),
                              "failed linked-source reclamation waits for the real signal reader before either allocation alias is reused");
                        reader.reset();
                        if (retry.joinable()) retry.join();
                        if (executor.joinable()) executor.join();
                        if (executor_error) std::rethrow_exception(executor_error);
                        if (retry_error) std::rethrow_exception(retry_error);
                        FaultEntry absent;
                        Check((!compound || retired) && purge_completed && !free_before_purge && watched_code_frees == 1 &&
                              links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent) &&
                              IsEmpty(module->GetNode(Location{source_pc})), "reader exit purges both failed source sites before one exact free");
                        ++reader_waits;
                        preserve_target();
                    }
                    if (!sweep || !primary_oom) completed = true;
                }
                if (!filename.empty()) Check(unlink(filename.c_str()) == 0, "remove this owned linked failure fixture");
                if (completed) break;
            }
            Check(completed, "each linked failure shape reaches its exact final case");
            std::printf("linked_call_failure_shape backend=%s mode=%.*s metadata=%u reverse=%u primary=%s failures=%u\n",
                        aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), unsigned(metadata), reverse,
                        primary_oom ? "oom" : "protect", shape_failures);
        }
    }
    std::printf("linked_call_failure backend=%s mode=%.*s failures=%u cleanup_failures=%u successes=%u reader_waits=%u terminal_releases=%u entries=2 sites=1\n",
                aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), failures, cleanup_failures, successes, reader_waits, terminal_releases);
}


struct LinkedMultiUnitFixture {
    aot::AotImage image;
    std::vector<SerialUnit> disk_units;
    std::array<std::pair<VAddr, VAddr>, 3> entries{};
};

LinkedMultiUnitFixture MakeLinkedMultiUnitFixture(const Config& config, void* guest,
        const aot::AotImage& target_image, VAddr target_pc) {
    const auto page = static_cast<u64>(getpagesize());
    const std::array<VAddr, 3> roots{2 * page + 0x100, 4 * page + 0x100, 6 * page + 0x100};
    const auto [prototype_image, prototype_unit] = RestoredSourceFixture(
            config, guest, target_image, target_pc, roots.front(), roots.front() + page);
    LinkedMultiUnitFixture fixture;
    fixture.image = prototype_image;
    fixture.image.units.clear();
    fixture.image.code.clear();
    for (unsigned i = 0; i < roots.size(); ++i) {
        const auto shift = roots[i] - roots.front();
        auto unit = prototype_image.units.front();
        unit.guest_start += shift;
        unit.code_offset = static_cast<u32>(fixture.image.code.size());
        for (auto& block : unit.blocks) { block.guest_start += shift; block.guest_end += shift; }
        for (auto& fault : unit.fault_sites) fault.guest_start += shift;
        fixture.image.code.insert(fixture.image.code.end(), prototype_image.code.begin(), prototype_image.code.end());
        fixture.image.units.push_back(std::move(unit));

        auto disk = prototype_unit;
        disk.guest_start += shift;
        for (auto& block : disk.blocks) { block.guest_start += shift; block.guest_end += shift; }
        for (auto& fault : disk.fault_sites) fault.guest_start += shift;
        fixture.disk_units.push_back(std::move(disk));
        fixture.entries[i] = {roots[i], roots[i] + page};
    }
    fixture.image.code_hash = HashBytes(fixture.image.code.data(), fixture.image.code.size(), 0xCBF29CE484222325ull);
    return fixture;
}

struct LinkedChainFixture {
    aot::AotImage image;
    std::vector<SerialUnit> disk_units;
    std::array<std::vector<VAddr>, 3> entries;
    std::array<unsigned, 3> site_counts{2, 2, 0};
};

LinkedChainFixture MakeLinkedChainFixture(const Config& config, void* guest,
        const aot::AotImage& target_image) {
    const auto page = static_cast<u64>(getpagesize());
    const std::array<VAddr, 3> roots{2 * page + 0x100, 4 * page + 0x100, 6 * page + 0x100};
    const auto [a_image, a_captured] = RestoredSourceFixture(
            config, guest, target_image, roots[1], roots[0], roots[0] + page, 91);
    const auto [b_image, b_captured] = RestoredSourceFixture(
            config, guest, target_image, roots[2], roots[1], roots[1] + page, 92);

    auto c_unit = target_image.units.front();
    const auto c_shift = roots[2] - 0x100;
    c_unit.guest_start += c_shift;
    for (auto& block : c_unit.blocks) { block.guest_start += c_shift; block.guest_end += c_shift; }
    for (auto& fault : c_unit.fault_sites) fault.guest_start += c_shift;
    SerialUnit c_disk{};
    c_disk.guest_start = roots[2];
    c_disk.is_function = c_unit.is_function;
    c_disk.code = target_image.code;
    c_disk.blocks = c_unit.blocks;
    c_disk.relocs = c_unit.relocs;
    c_disk.fault_sites = c_unit.fault_sites;
    c_disk.region_branches = c_unit.region_branches;

    LinkedChainFixture fixture;
    fixture.image = target_image;
    fixture.image.units.clear();
    fixture.image.code.clear();
    fixture.image.dispatch_slots.clear();
    const auto add_slots = [&](const auto& slots) {
        for (const auto& slot : slots) {
            bool present = false;
            for (const auto& existing : fixture.image.dispatch_slots) present = present || existing == slot;
            if (!present) fixture.image.dispatch_slots.push_back(slot);
        }
    };
    add_slots(a_image.dispatch_slots);
    add_slots(b_image.dispatch_slots);
    const std::array<const std::vector<u8>*, 3> codes{&a_image.code, &b_image.code, &target_image.code};
    std::array<aot::AotUnit, 3> units{a_image.units.front(), b_image.units.front(), c_unit};
    for (unsigned i = 0; i < units.size(); ++i) {
        units[i].code_offset = static_cast<u32>(fixture.image.code.size());
        fixture.image.code.insert(fixture.image.code.end(), codes[i]->begin(), codes[i]->end());
        fixture.image.units.push_back(std::move(units[i]));
    }
    fixture.disk_units = {a_captured, b_captured, c_disk};
    fixture.entries = {{{roots[0], roots[0] + page}, {roots[1], roots[1] + page}, {roots[2]}}};
    fixture.image.code_hash = HashBytes(fixture.image.code.data(), fixture.image.code.size(), 0xCBF29CE484222325ull);
    return fixture;
}

void LinkedMultiUnitFailure(bool aot_mode, std::string_view mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-linked-multi-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-linked-multi-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate real multi-unit linked restoration failures");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const VAddr target_pc = 0x100;
    const auto target_image = Artifact(config, guest.memory, true);
    const auto fixture = MakeLinkedMultiUnitFixture(config, guest.memory, target_image, target_pc);
    const bool terminal = mode == "terminal";
    const bool compound = mode == "cleanup" || terminal;
    unsigned failures{}, cleanup_attempts{}, successes{}, reader_waits{}, terminal_releases{}, retained_owners{};

    struct AttemptResult {
        bool primary_hit{};
        bool cleanup_hit{};
        std::array<bool, 3> retained{};
    };

    const auto run_attempt = [&](unsigned fail_position, bool primary_oom, unsigned index,
            SerialBlock::EntryFlags metadata, bool reverse_blocks, bool reverse_order,
            unsigned cleanup_skip, bool require_primary_hit) -> AttemptResult {
        std::array<unsigned, 3> order{0, 1, 2};
        if (reverse_order) std::reverse(order.begin(), order.end());

        auto incoming = fixture.image;
        incoming.units.clear();
        incoming.code.clear();
        std::vector<SerialUnit> incoming_disk;
        for (unsigned position = 0; position < order.size(); ++position) {
            const unsigned unit_index = order[position];
            auto unit = fixture.image.units[unit_index];
            const auto source_offset = unit.code_offset;
            unit.code_offset = static_cast<u32>(incoming.code.size());
            incoming.code.insert(incoming.code.end(),
                    fixture.image.code.begin() + source_offset,
                    fixture.image.code.begin() + source_offset + unit.code_size);
            unit.blocks.push_back({.guest_start = target_pc, .guest_end = target_pc + 4,
                .code_offset = UINT32_MAX,
                .guest_bytes_hash = HashBytes(static_cast<u8*>(guest.memory) + target_pc, 4, 0xCBF29CE484222325ull),
                .entry_flags = u8(metadata)});
            if (reverse_blocks) std::reverse(unit.blocks.begin(), unit.blocks.end());
            incoming.units.push_back(unit);
            auto disk = fixture.disk_units[unit_index];
            disk.blocks = incoming.units.back().blocks;
            incoming_disk.push_back(std::move(disk));
        }
        incoming.code_hash = HashBytes(incoming.code.data(), incoming.code.size(), 0xCBF29CE484222325ull);

        AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        auto& smc = space.GetSmcTracker();
        auto& links = space.GetLinkManager();
        aot::LoadReport target_report, report;
        std::string error;
        Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
              "install the independent target before each multi-unit linked attempt");
        const auto target = links.QueryTarget(target_pc);
        const std::array<size_t, 3> target_entries{space.GetCodeCacheTable().Lookup(target_pc),
            space.GetCallCodeCacheTable().Lookup(target_pc), space.GetPendingCallCodeCacheTable().Lookup(target_pc)};
        auto* target_alias = aot::ResolveCodeOffset(target_report.mapping, 0);
        Check(target && target_entries[0] && target_alias, "bind the independent target before multi-unit failure");

        std::string filename;
        if (!aot_mode) {
            for (auto& disk : incoming_disk) {
                disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
                if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
                    return site.host_end != site.host_begin + sizeof(u32);
                });
            }
            BlobWriter payload;
            payload.U64(incoming.dispatch_slots.size());
            for (const auto& [key, slot] : incoming.dispatch_slots) { payload.U64(key); payload.U32(slot); }
            payload.U64(incoming_disk.size());
            for (const auto& disk : incoming_disk) WriteUnit(payload, disk);
            const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
                ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
            BlobWriter file;
            constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
            file.Bytes(magic, sizeof(magic));
            for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                    u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
            file.Bytes(payload.Data().data(), payload.Size());
            auto name = HashU64(key.build_id, key.guest_id);
            name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
            std::array<char, 512> path{};
            std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
            filename = path.data();
            std::ofstream output(filename, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the valid three-unit linked disk fixture");
        }

        TranslateTable l1{8}, other_l1{8};
        const auto token = smc.RegisterRuntime(l1), other_token = smc.RegisterRuntime(other_l1);
        bool registered = true;
        const auto unregister = ScopeExit([&] { if (registered) { smc.UnregisterRuntime(other_token); smc.UnregisterRuntime(token); } });
        l1.Put(target_pc, target_entries[0]); other_l1.Put(target_pc, target_entries[0]);
        std::atomic<bool> executing{}, end_executor{};
        std::exception_ptr executor_error;
        std::thread executor;
        std::optional<LinkManager::SignalReader> reader{std::in_place, links};
        multi_allocations.fill(nullptr); multi_allocation_frees.fill(0); multi_allocation_count = 0;
        multi_lifetimes.fill({}); multi_lifetime_count = 0;
        watched_code_free = nullptr; watched_code_frees = 0;
        watched_purge_allocation = nullptr;
        purge_wait_observed = purge_completed = free_before_purge = false;
        const auto finish = ScopeExit([&] {
            end_executor = true; reader.reset();
            if (executor.joinable()) executor.join();
            watched_code_free = nullptr; watched_purge_allocation = nullptr; watch_multi_purge = false;
            watch_aot_allocation = watch_multi_allocations = capture_multi_lifetimes = false;
            capture_published_lifetime = false; published_failure_lifetime.reset();
            multi_oom_pushes = multi_protect_pushes = multi_post_push_failure_index = 0;
            multi_disarm_after_failing_unit = false; multi_fail_allocation_ordinal = 0;
            fail_after_aot_push = fail_aot_protect = false;
            inject_aot_cleanup = fail_cleanup_allocation = false; cleanup_skip_queries = 0;
            fail_aot_allocation = -1;
        });
        const auto wait_until = [](const std::atomic<bool>& flag) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            return flag.load();
        };
        executor = std::thread([&] {
            try {
                SmcTracker::ExecutionScope active{smc, token};
                executing = true;
                while (!end_executor) std::this_thread::yield();
                active.End();
            } catch (...) { executor_error = std::current_exception(); executing = true; }
        });
        Check(wait_until(executing), "admit a real executor before multi-unit fault injection");

        watched_aot_size = fixture.image.units.front().code_size;
        aot_failure_hit = aot_was_published = aot_published_at_failure = false;
        aot_protection_failure_hit = cleanup_query_observed = cleanup_failure_hit = false;
        watch_aot_allocation = watch_multi_allocations = capture_multi_lifetimes = true;
        multi_oom_pushes = primary_oom ? fail_position + 1 : 0;
        multi_protect_pushes = primary_oom ? 0 : fail_position + 1;
        multi_post_push_failure_index = index;
        multi_fail_allocation_ordinal = fail_position;
        multi_disarm_after_failing_unit = primary_oom;
        inject_aot_cleanup = compound; cleanup_skip_queries = cleanup_skip;
        fail_after_aot_push = false; fail_aot_protect = false; fail_aot_allocation = -1;

        bool escaped{};
        int status = -1;
        try {
            if (aot_mode) status = aot::InstallArtifact(space, incoming, guest.memory, config.guest_addr_mask, report, error);
            else space.LoadJitCache();
        } catch (...) { escaped = true; }
        watch_aot_allocation = capture_multi_lifetimes = false;
        multi_oom_pushes = multi_protect_pushes = 0;
        multi_disarm_after_failing_unit = false;
        inject_aot_cleanup = false; cleanup_skip_queries = 0; fail_cleanup_allocation = false;
        fail_aot_allocation = -1; fail_aot_protect = false;

        AttemptResult result{.primary_hit = primary_oom ? aot_failure_hit : aot_protection_failure_hit,
                             .cleanup_hit = cleanup_failure_hit};
        Check(!require_primary_hit || result.primary_hit, "the targeted multi-unit primary failure was consumed");
        const unsigned expected_pushed = (!result.primary_hit || !aot_mode) ? 3u : fail_position + 1;
        if (multi_lifetime_count != expected_pushed || multi_allocation_count != expected_pushed)
            std::printf("linked_multi_track backend=%s mode=%.*s position=%u primary=%s index=%u hit=%u expected=%u lifetimes=%u allocations=%u\n",
                        aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), fail_position,
                        primary_oom ? "oom" : "protect", index, result.primary_hit, expected_pushed,
                        multi_lifetime_count, multi_allocation_count);
        Check(multi_lifetime_count == expected_pushed,
              "track every pushed multi-unit linked lifetime in installation order");
        Check(multi_allocation_count == expected_pushed,
              "track every pushed multi-unit linked allocation in installation order");
        for (unsigned position = 0; position < expected_pushed; ++position) {
            const auto lifetime = multi_lifetimes[position];
            const auto region = lifetime ? module->GetCodeRegion(lifetime->base) : std::nullopt;
            Check(lifetime && lifetime->entries.size() == 2 && region &&
                  SiteRxToRw(*region, lifetime->base) == multi_allocations[position],
                  "bind both aliases and both entries of each pushed linked unit");
        }
        const auto preserve_target = [&] {
            const auto current = links.QueryTarget(target_pc);
            Check(current && current->generation == target->generation && current->host_pc == target->host_pc &&
                  target_entries == std::array<size_t, 3>{space.GetCodeCacheTable().Lookup(target_pc),
                      space.GetCallCodeCacheTable().Lookup(target_pc), space.GetPendingCallCodeCacheTable().Lookup(target_pc)} &&
                  l1.Lookup(target_pc) == target_entries[0] && other_l1.Lookup(target_pc) == target_entries[0] &&
                  aot::ResolveCodeOffset(target_report.mapping, 0) == target_alias,
                  "multi-unit linked failure preserves the independent target entries, L1s, generation and alias");
        };
        const auto execute_entry = [&](VAddr pc, bool expect_witness = true) {
            Runtime runtime{&space};
            runtime.SetLocation(pc);
            Check(runtime.Run() == HaltReason::CallHost, "execute an accepted linked entry through the independent target");
            u64 value{}, witness{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
            Check(value == 37 && (!expect_witness || witness == 91),
                  "accepted linked source and target preserve both witnesses");
        };
        const auto finish_accepted_disk_units = [&] {
            if (aot_mode) return;
            for (unsigned position = 0; position < order.size(); ++position) {
                if (position == fail_position) continue;
                const auto lifetime = multi_lifetimes[position];
                const LinkSourceOwner owner{module.get(), lifetime->base};
                for (const auto pc : {fixture.entries[order[position]].first, fixture.entries[order[position]].second})
                    for (unsigned warm = 0; warm < 2; ++warm) execute_entry(pc);
                const auto sites = links.QuerySourceSites(owner);
                Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
                    return site.state == LinkSiteState::Linked;
                }), "disk units around the failed prefix remain actually linked");
                const auto node = module->GetNode(Location{fixture.entries[order[position]].first});
                FaultEntry absent;
                Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
                      multi_allocation_frees[position] == 1 && links.QuerySourceSites(owner).empty() &&
                      !module->LookupCodeAllocation(lifetime->base, absent),
                      "accepted disk units retain ordinary exact reclamation after a neighboring failure");
            }
        };

        if (!result.primary_hit) {
            Check(!require_primary_hit && !escaped &&
                  (aot_mode ? status == aot::kAotOk && report.units_installed == 3
                            : space.GetJitDiskCache()->Stats().units_loaded == 3),
                  "the complete multi-unit post-Push sweep reaches a committed three-unit installation");
            for (unsigned position = 0; position < 3; ++position)
                Check(multi_lifetimes[position]->Live(), "each endpoint linked unit remains live before execution");
            preserve_target();
            end_executor = true; executor.join(); reader.reset();
            if (executor_error) std::rethrow_exception(executor_error);
            for (unsigned position = 0; position < order.size(); ++position) {
                for (const auto pc : {fixture.entries[order[position]].first, fixture.entries[order[position]].second})
                    for (unsigned warm = 0; warm < 2; ++warm) execute_entry(pc);
                const LinkSourceOwner owner{module.get(), multi_lifetimes[position]->base};
                const auto sites = links.QuerySourceSites(owner);
                Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
                    return site.state == LinkSiteState::Linked;
                }), "each successful multi-unit endpoint links both physical branches");
            }
            for (unsigned position = 0; position < order.size(); ++position) {
                const auto node = module->GetNode(Location{fixture.entries[order[position]].first});
                FaultEntry absent;
                Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
                      multi_allocation_frees[position] == 1 &&
                      links.QuerySourceSites({module.get(), multi_lifetimes[position]->base}).empty() &&
                      !module->LookupCodeAllocation(multi_lifetimes[position]->base, absent),
                      "successful multi-unit endpoint retains one exact free per unit");
            }
            ++successes;
            if (!filename.empty()) Check(unlink(filename.c_str()) == 0, "remove this owned multi-unit disk fixture");
            return result;
        }

        ++failures;
        if (result.cleanup_hit) ++cleanup_attempts;
        std::printf("linked_multi_failure_point backend=%s mode=%.*s position=%u unit=%u primary=%s index=%u cleanup_skip=%u\n",
                    aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), fail_position, order[fail_position],
                    primary_oom ? "oom" : "protect", index, cleanup_skip);
        if (aot_mode) {
            Check((escaped || status != aot::kAotOk) && !report.units_installed && !report.code_bytes &&
                  !report.dispatch_slots && !report.mapping,
                  "failed multi-unit AOT installation reports zero committed units");
        } else {
            Check(!escaped && space.GetJitDiskCache()->Stats().units_loaded == 2,
                  "failed disk unit leaves exactly the two neighboring accepted units loaded");
        }
        for (unsigned position = 0; position < order.size(); ++position) {
            const bool accepted = !aot_mode && position != fail_position;
            if (position < expected_pushed) {
                const auto lifetime = multi_lifetimes[position];
                Check(accepted ? lifetime->Live() : lifetime->Revoked() && !lifetime->Commit(),
                      "multi-unit failure separates accepted disk lifetimes from inert failed lifetimes");
                Check(multi_allocation_frees[position] == 0, "no pushed multi-unit allocation frees during executor grace");
            }
            for (const auto pc : {fixture.entries[order[position]].first, fixture.entries[order[position]].second}) {
                if (accepted) {
                    Check(space.GetCodeCacheTable().Lookup(pc) && links.QueryTarget(pc),
                          "accepted disk entries around the failure remain published");
                } else {
                    Check(!space.GetCodeCacheTable().Lookup(pc) && !space.GetCallCodeCacheTable().Lookup(pc) &&
                          !space.GetPendingCallCodeCacheTable().Lookup(pc) && !links.QueryTarget(pc),
                          "failed multi-unit rollback withdraws every failed canonical, Call, pending Call and link entry");
                }
            }
        }
        preserve_target();

        for (unsigned position = 0; position < order.size(); ++position) {
            const auto root = fixture.entries[order[position]].first;
            const auto node = module->GetNode(Location{root});
            const bool failed = aot_mode ? position <= fail_position : position == fail_position;
            if (!failed) {
                Check(aot_mode ? IsEmpty(node) : IsFunction(node), "multi-unit attempt separates unattempted AOT and accepted disk nodes");
                continue;
            }
            if (IsFunction(node)) {
                result.retained[position] = true;
                const auto lifetime = multi_lifetimes[position];
                Check(GetFunction(node)->code_lifetime == lifetime &&
                      links.QuerySourceSites({module.get(), lifetime->base}).size() == 2,
                      "cleanup failure retains the exact inert linked owner and both outgoing sites");
            } else {
                Check(IsEmpty(node), "ordinary multi-unit rollback detaches each non-retained failed node");
            }
        }
        for (const bool retained : result.retained) if (retained) ++retained_owners;
        if (compound && result.cleanup_hit) {
            bool any_retained = false;
            for (const bool retained : result.retained) any_retained = any_retained || retained;
            Check(cleanup_query_observed && any_retained,
                  "independent multi-unit cleanup OOM is consumed and retains an inert owner");
        }

        if (!compound || !result.cleanup_hit) {
            watched_purge_allocation = multi_lifetimes[fail_position]->base;
            watch_multi_purge = true;
            end_executor = true;
            Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge,
                  "multi-unit failed-source purge reaches a real signal-reader wait");
            for (unsigned position = 0; position < expected_pushed; ++position)
                Check(multi_allocation_frees[position] == 0 && Mapped(multi_lifetimes[position]->base) &&
                      Mapped(multi_allocations[position]), "all pushed multi-unit aliases survive reader grace");
            reader.reset(); executor.join(); watch_multi_purge = false;
            if (executor_error) std::rethrow_exception(executor_error);
            Check(purge_completed && !free_before_purge, "multi-unit purge completes only after signal-reader exit");
            ++reader_waits;
            for (unsigned position = 0; position < order.size(); ++position) {
                const bool failed = aot_mode ? position <= fail_position : position == fail_position;
                if (!failed) continue;
                FaultEntry absent;
                Check(multi_allocation_frees[position] == 1,
                      "ordinary multi-unit rollback frees each failed allocation exactly once");
                Check(links.QuerySourceSites({module.get(), multi_lifetimes[position]->base}).empty(),
                      "ordinary multi-unit rollback purges each failed source's outgoing sites");
                Check(!module->LookupCodeAllocation(multi_lifetimes[position]->base, absent),
                      "ordinary multi-unit rollback removes each failed allocation's fault ownership");
                Check(IsEmpty(module->GetNode(Location{fixture.entries[order[position]].first})),
                      "ordinary multi-unit rollback detaches each failed node");
            }
            preserve_target();
            execute_entry(target_pc, false);
            finish_accepted_disk_units();
        } else {
            end_executor = true; reader.reset(); executor.join();
            if (executor_error) std::rethrow_exception(executor_error);
            for (unsigned position = 0; position < expected_pushed; ++position) {
                const bool failed = aot_mode ? position <= fail_position : position == fail_position;
                if (failed) Check(multi_allocation_frees[position] == (result.retained[position] ? 0u : 1u),
                      "compound multi-unit cleanup distinguishes queued frees from retained owners after grace");
            }
            if (terminal) {
                smc.UnregisterRuntime(other_token); smc.UnregisterRuntime(token); registered = false;
                const auto ticket = Terminal(space);
                Check(space.TryShutdown(ticket).Complete(), "terminal shutdown releases retained multi-unit linked owners");
                for (unsigned position = 0; position < expected_pushed; ++position) {
                    Check(!Mapped(multi_lifetimes[position]->base) && !Mapped(multi_allocations[position]),
                          "terminal multi-unit cleanup releases both aliases of every pushed allocation");
                    if (result.retained[position]) {
                        Check(multi_allocation_frees[position] == 0, "terminal retained allocation has no duplicate ordinary free");
                        ++terminal_releases;
                    }
                }
            } else {
                for (unsigned position = 0; position < order.size(); ++position) {
                    if (!result.retained[position]) continue;
                    const auto root = fixture.entries[order[position]].first;
                    const auto node = module->GetNode(Location{root});
                    auto* function = GetFunction(node).get();
                    const auto lifetime = multi_lifetimes[position];
                    watched_purge_allocation = lifetime->base;
                    purge_wait_observed = purge_completed = free_before_purge = false;
                    reader.emplace(links);
                    bool retired{};
                    std::exception_ptr retry_error;
                    std::thread retry([&] {
                        try { retired = smc.RetireNode(space, &l1, module, function); }
                        catch (...) { retry_error = std::current_exception(); }
                    });
                    Check(wait_until(purge_wait_observed) && !purge_completed && multi_allocation_frees[position] == 0 &&
                          Mapped(lifetime->base) && Mapped(multi_allocations[position]),
                          "retained multi-unit owner retry waits for a real signal reader");
                    reader.reset(); retry.join();
                    if (retry_error) std::rethrow_exception(retry_error);
                    FaultEntry absent;
                    Check(retired && purge_completed && multi_allocation_frees[position] == 1 &&
                          links.QuerySourceSites({module.get(), lifetime->base}).empty() &&
                          !module->LookupCodeAllocation(lifetime->base, absent) &&
                          IsEmpty(module->GetNode(Location{root})),
                          "retained multi-unit owner retry purges both sites before one exact free");
                    ++reader_waits;
                }
                preserve_target();
                execute_entry(target_pc, false);
                finish_accepted_disk_units();
            }
        }
        if (!filename.empty()) Check(unlink(filename.c_str()) == 0, "remove this owned multi-unit disk fixture");
        return result;
    };

    if (mode != "protect") {
        for (unsigned fail_position = 0; fail_position < 3; ++fail_position) {
            bool completed = false;
            unsigned shape_failures = 0;
            for (unsigned index = 0; index < 256; ++index) {
                const auto result = run_attempt(fail_position, true, index, SerialBlock::MetadataOnly,
                                                false, false, 0, false);
                if (!result.primary_hit) { completed = true; break; }
                ++shape_failures;
            }
            Check(completed, "each multi-unit prefix allocation sweep reaches its successful endpoint");
            std::printf("linked_multi_failure_shape backend=%s mode=%.*s position=%u primary=oom failures=%u\n",
                        aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), fail_position, shape_failures);
        }
    } else {
        for (const auto metadata : {SerialBlock::MetadataOnly, SerialBlock::DependencyOnly})
        for (const bool reverse_blocks : {false, true})
        for (const bool reverse_order : {false, true}) {
            unsigned shape_failures = 0;
            for (unsigned fail_position = 0; fail_position < 3; ++fail_position) {
                const auto result = run_attempt(fail_position, false, 0, metadata, reverse_blocks, reverse_order, 0, true);
                Check(result.primary_hit, "each multi-unit protection shape consumes its targeted failure");
                ++shape_failures;
            }
            std::printf("linked_multi_failure_shape backend=%s mode=protect metadata=%u reverse=%u order_reverse=%u failures=%u\n",
                        aot_mode ? "aot" : "disk", unsigned(metadata), reverse_blocks, reverse_order, shape_failures);
        }
    }

    if (compound) {
        for (unsigned fail_position = 0; fail_position < 3; ++fail_position) {
            bool endpoint = false, saw_cleanup = false, saw_failing = false, saw_earlier = false;
            for (unsigned cleanup_skip = 0; cleanup_skip < 16; ++cleanup_skip) {
                const auto result = run_attempt(fail_position, false, 0, SerialBlock::MetadataOnly,
                                                false, false, cleanup_skip, true);
                if (!result.cleanup_hit) { endpoint = true; break; }
                saw_cleanup = true;
                saw_failing = saw_failing || result.retained[fail_position];
                for (unsigned position = 0; position < fail_position; ++position)
                    saw_earlier = saw_earlier || result.retained[position];
            }
            Check(endpoint && saw_cleanup && saw_failing,
                  "cleanup-query sweep reaches the failing owner and then an ordinary endpoint");
            if (aot_mode && fail_position) Check(saw_earlier,
                  "cleanup-query sweep also retains an earlier committed AOT prefix owner");
            std::printf("linked_multi_cleanup_shape backend=%s mode=%.*s position=%u failing_retained=%u earlier_retained=%u\n",
                        aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), fail_position,
                        saw_failing, saw_earlier);
        }
    }

    std::printf("linked_multi_failure backend=%s mode=%.*s failures=%u cleanup_attempts=%u retained_owners=%u successes=%u reader_waits=%u terminal_releases=%u units=3 entries=6 sites=6\n",
                aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), failures, cleanup_attempts,
                retained_owners, successes, reader_waits, terminal_releases);
}

void LinkedPrivateFailure(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-linked-private-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-linked-private-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate real linked private restoration failures");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const auto target_image = Artifact(config, guest.memory, true);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    unsigned failures{}, prealloc{}, private_releases{}, deferred{}, successes{};
    for (unsigned index = 0; index < 256; ++index) {
        AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        auto& smc = space.GetSmcTracker();
        auto& links = space.GetLinkManager();
        aot::LoadReport target_report, report;
        std::string error;
        Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
              "install the independent target before each linked private attempt");
        const auto target = links.QueryTarget(target_pc);
        const std::array<size_t, 3> target_entries{space.GetCodeCacheTable().Lookup(target_pc),
            space.GetCallCodeCacheTable().Lookup(target_pc), space.GetPendingCallCodeCacheTable().Lookup(target_pc)};
        auto* target_alias = aot::ResolveCodeOffset(target_report.mapping, 0);
        Check(target && target_entries[0] && target_alias, "bind the independent target before linked private failure");

        auto incoming = image;
        incoming.units.front().blocks.push_back({.guest_start = target_pc, .guest_end = target_pc + 4,
            .code_offset = UINT32_MAX,
            .guest_bytes_hash = HashBytes(static_cast<u8*>(guest.memory) + target_pc, 4, 0xCBF29CE484222325ull),
            .entry_flags = u8(SerialBlock::MetadataOnly)});
        std::string filename;
        if (!aot_mode) {
            auto disk = captured;
            disk.blocks = incoming.units.front().blocks;
            disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
            if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
                return site.host_end != site.host_begin + sizeof(u32);
            });
            BlobWriter payload;
            payload.U64(incoming.dispatch_slots.size());
            for (const auto& [key, slot] : incoming.dispatch_slots) { payload.U64(key); payload.U32(slot); }
            payload.U64(1); WriteUnit(payload, disk);
            const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
                ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
            BlobWriter file;
            constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
            file.Bytes(magic, sizeof(magic));
            for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                    u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
            file.Bytes(payload.Data().data(), payload.Size());
            auto name = HashU64(key.build_id, key.guest_id);
            name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
            std::array<char, 512> path{};
            std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
            filename = path.data();
            std::ofstream output(filename, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the valid linked private disk fixture");
        }

        TranslateTable l1{8};
        const auto token = smc.RegisterRuntime(l1);
        const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
        SmcTracker::ExecutionScope active{smc, token};
        captured_code_exec.fill(nullptr); captured_code_rw.fill(nullptr); captured_code_buffer_count = 0;
        watched_code_free = nullptr; watched_code_frees = 0;
        watched_aot_size = captured.code.size();
        aot_failure_hit = aot_was_published = aot_published_at_failure = false;
        published_failure_lifetime.reset();
        const auto reset = ScopeExit([] {
            watch_aot_allocation = capture_code_buffers = capture_published_lifetime = false;
            fail_aot_allocation = -1; arm_after_next_read_unit = false; private_failure_index = 0;
            watched_code_free = nullptr; published_failure_lifetime.reset();
        });
        watch_aot_allocation = capture_code_buffers = capture_published_lifetime = true;
        if (aot_mode) {
            fail_aot_allocation = static_cast<int>(index);
            aot_allocation_index = 0;
            arm_after_next_read_unit = false;
        } else {
            fail_aot_allocation = -1;
            private_failure_index = index;
            arm_after_next_read_unit = true;
        }
        bool escaped{};
        int status = -1;
        try {
            if (aot_mode) status = aot::InstallArtifact(space, incoming, guest.memory, config.guest_addr_mask, report, error);
            else space.LoadJitCache();
        } catch (...) { escaped = true; }
        watch_aot_allocation = capture_code_buffers = capture_published_lifetime = false;
        fail_aot_allocation = -1; arm_after_next_read_unit = false;
        const bool hit = aot_failure_hit;
        const auto preserve_target = [&] {
            const auto current = links.QueryTarget(target_pc);
            Check(current && current->generation == target->generation && current->host_pc == target->host_pc &&
                  target_entries == std::array<size_t, 3>{space.GetCodeCacheTable().Lookup(target_pc),
                      space.GetCallCodeCacheTable().Lookup(target_pc), space.GetPendingCallCodeCacheTable().Lookup(target_pc)} &&
                  aot::ResolveCodeOffset(target_report.mapping, 0) == target_alias,
                  "linked private failure preserves the independent target generation, entries and alias");
        };
        const auto execute_target = [&] {
            Runtime runtime{&space};
            runtime.SetLocation(target_pc);
            Check(runtime.Run() == HaltReason::CallHost, "independent target executes after linked private failure");
            u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            Check(value == 37, "linked private failure preserves the independent target result");
        };

        if (!hit) {
            const auto lifetime = published_failure_lifetime;
            Check(!escaped && captured_code_buffer_count == 1 && lifetime && lifetime->Live() &&
                  (aot_mode ? status == aot::kAotOk && report.units_installed == 1
                            : space.GetJitDiskCache()->Stats().units_loaded == 1),
                  "the linked private allocation sweep reaches a committed source");
            preserve_target();
            active.End();
            for (const auto pc : {source_pc, secondary_pc}) for (unsigned warm = 0; warm < 2; ++warm) {
                Runtime runtime{&space};
                runtime.SetLocation(pc);
                Check(runtime.Run() == HaltReason::CallHost, "successful private sweep endpoint executes each restored entry");
                u64 value{}, witness{};
                std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
                std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
                Check(value == 37 && witness == 91, "successful private sweep endpoint preserves source and target witnesses");
            }
            const LinkSourceOwner owner{module.get(), lifetime->base};
            const auto sites = links.QuerySourceSites(owner);
            Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
                return site.state == LinkSiteState::Linked;
            }), "successful private sweep endpoint links both physical branches");
            FaultEntry absent;
            Check(smc.RetireNode(space, &l1, module, GetFunction(module->GetNode(Location{source_pc})).get()) &&
                  watched_code_frees == 1 && links.QuerySourceSites(owner).empty() &&
                  !module->LookupCodeAllocation(lifetime->base, absent),
                  "successful private sweep endpoint retains ordinary one-time reclamation");
            ++successes;
            if (!filename.empty()) Check(unlink(filename.c_str()) == 0, "remove this owned linked private fixture");
            break;
        }

        ++failures;
        const char* stage = captured_code_buffer_count == 0 ? "prealloc" : aot_published_at_failure ? "published" : "private";
        std::printf("linked_private_failure_point backend=%s stage=%s index=%u\n",
                    aot_mode ? "aot" : "disk", stage, index);
        if (aot_mode) {
            Check((escaped || status != aot::kAotOk) && !report.units_installed && !report.mapping,
                  "failed linked private AOT installation commits no unit");
        } else {
            Check(space.GetJitDiskCache()->Stats().units_loaded == 0,
                  "failed linked private disk revival loads no unit");
        }
        for (const auto pc : {source_pc, secondary_pc}) Check(!space.GetCodeCacheTable().Lookup(pc) &&
            !space.GetCallCodeCacheTable().Lookup(pc) && !space.GetPendingCallCodeCacheTable().Lookup(pc) &&
            !links.QueryTarget(pc), "linked private failure publishes no source entry or link target");
        Check(IsEmpty(module->GetNode(Location{source_pc})), "linked private failure leaves no source node");
        preserve_target();

        if (captured_code_buffer_count == 0) {
            Check(!watched_code_free && !aot_was_published && !published_failure_lifetime,
                  "pre-allocation linked failure acquires no code allocation or lifetime");
            ++prealloc;
        } else {
            Check(captured_code_buffer_count == 1, "linked private sweep tracks exactly one code allocation");
            auto* exec = static_cast<u8*>(captured_code_exec[0]);
            auto* rw = static_cast<u8*>(captured_code_rw[0]);
            Check(exec && rw && watched_code_free == rw, "linked private sweep binds both code aliases");
            const LinkSourceOwner owner{module.get(), exec};
            FaultEntry absent;
            if (!aot_published_at_failure) {
                Check(!aot_was_published && !published_failure_lifetime &&
                      links.QuerySourceSites(owner).empty() &&
                      !module->LookupCodeAllocation(exec, absent) && watched_code_frees == 1,
                      "private linked discard purges registered sites and frees once before executor exit");
                ++private_releases;
            } else {
                Check(published_failure_lifetime && published_failure_lifetime->base == exec && watched_code_frees == 0,
                      "published linked allocation remains deferred during executor grace");
                ++deferred;
            }
            active.End();
            if (aot_published_at_failure) {
                Check(watched_code_frees == 1 && links.QuerySourceSites(owner).empty() &&
                      !module->LookupCodeAllocation(exec, absent),
                      "published linked allocation purges and frees once after executor exit");
            }
        }
        if (captured_code_buffer_count == 0) active.End();
        execute_target();
        if (!filename.empty()) Check(unlink(filename.c_str()) == 0, "remove this owned linked private fixture");
    }
    Check(successes == 1 && prealloc && private_releases && deferred,
          "linked private sweep covers pre-allocation, private and published rollback");
    std::printf("linked_private_failure backend=%s failures=%u prealloc=%u private_releases=%u deferred=%u successes=%u entries=2 sites=2\n",
                aot_mode ? "aot" : "disk", failures, prealloc, private_releases, deferred, successes);
}

void LinkedChainFailure(bool aot_mode, std::string_view mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-linked-chain-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-linked-chain-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate real same-artifact linked chain failures");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const VAddr target_pc = 0x100;
    const auto target_image = Artifact(config, guest.memory, true);
    const auto fixture = MakeLinkedChainFixture(config, guest.memory, target_image);
    const bool terminal = mode == "terminal";
    const bool compound = mode == "cleanup" || terminal;
    unsigned failures{}, cleanup_attempts{}, successes{}, reader_waits{}, terminal_releases{}, retained_owners{};

    struct AttemptResult {
        bool primary_hit{};
        bool cleanup_hit{};
        std::array<bool, 3> retained{};
    };

    const auto run_attempt = [&](unsigned fail_position, bool primary_oom, unsigned index,
            SerialBlock::EntryFlags metadata, bool reverse_blocks, bool reverse_order,
            unsigned cleanup_skip, bool require_primary_hit) -> AttemptResult {
        std::array<unsigned, 3> order{0, 1, 2};
        if (reverse_order) std::reverse(order.begin(), order.end());

        auto incoming = fixture.image;
        incoming.units.clear();
        incoming.code.clear();
        std::vector<SerialUnit> incoming_disk;
        for (unsigned position = 0; position < order.size(); ++position) {
            const unsigned unit_index = order[position];
            auto unit = fixture.image.units[unit_index];
            const auto source_offset = unit.code_offset;
            unit.code_offset = static_cast<u32>(incoming.code.size());
            incoming.code.insert(incoming.code.end(),
                    fixture.image.code.begin() + source_offset,
                    fixture.image.code.begin() + source_offset + unit.code_size);
            unit.blocks.push_back({.guest_start = target_pc, .guest_end = target_pc + 4,
                .code_offset = UINT32_MAX,
                .guest_bytes_hash = HashBytes(static_cast<u8*>(guest.memory) + target_pc, 4, 0xCBF29CE484222325ull),
                .entry_flags = u8(metadata)});
            if (reverse_blocks) std::reverse(unit.blocks.begin(), unit.blocks.end());
            incoming.units.push_back(unit);
            auto disk = fixture.disk_units[unit_index];
            disk.blocks = incoming.units.back().blocks;
            incoming_disk.push_back(std::move(disk));
        }
        incoming.code_hash = HashBytes(incoming.code.data(), incoming.code.size(), 0xCBF29CE484222325ull);

        AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        auto& smc = space.GetSmcTracker();
        auto& links = space.GetLinkManager();
        aot::LoadReport target_report, report;
        std::string error;
        Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
              "install the independent target before each linked chain attempt");
        const auto target = links.QueryTarget(target_pc);
        const std::array<size_t, 3> target_entries{space.GetCodeCacheTable().Lookup(target_pc),
            space.GetCallCodeCacheTable().Lookup(target_pc), space.GetPendingCallCodeCacheTable().Lookup(target_pc)};
        auto* target_alias = aot::ResolveCodeOffset(target_report.mapping, 0);
        Check(target && target_entries[0] && target_alias, "bind the independent target before linked chain failure");

        std::string filename;
        if (!aot_mode) {
            for (auto& disk : incoming_disk) {
                disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
                if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
                    return site.host_end != site.host_begin + sizeof(u32);
                });
            }
            BlobWriter payload;
            payload.U64(incoming.dispatch_slots.size());
            for (const auto& [key, slot] : incoming.dispatch_slots) { payload.U64(key); payload.U32(slot); }
            payload.U64(incoming_disk.size());
            for (const auto& disk : incoming_disk) WriteUnit(payload, disk);
            const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
                ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
            BlobWriter file;
            constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
            file.Bytes(magic, sizeof(magic));
            for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                    u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
            file.Bytes(payload.Data().data(), payload.Size());
            auto name = HashU64(key.build_id, key.guest_id);
            name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
            std::array<char, 512> path{};
            std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
            filename = path.data();
            std::ofstream output(filename, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the valid three-unit linked chain disk fixture");
        }

        TranslateTable l1{8}, other_l1{8};
        const auto token = smc.RegisterRuntime(l1), other_token = smc.RegisterRuntime(other_l1);
        bool registered = true;
        const auto unregister = ScopeExit([&] { if (registered) { smc.UnregisterRuntime(other_token); smc.UnregisterRuntime(token); } });
        l1.Put(target_pc, target_entries[0]); other_l1.Put(target_pc, target_entries[0]);
        std::atomic<bool> executing{}, end_executor{};
        std::exception_ptr executor_error;
        std::thread executor, retry;
        std::optional<LinkManager::SignalReader> reader{std::in_place, links};
        multi_allocations.fill(nullptr); multi_allocation_frees.fill(0); multi_allocation_count = 0;
        multi_lifetimes.fill({}); multi_lifetime_count = 0;
        captured_code_exec.fill(nullptr); captured_code_rw.fill(nullptr); captured_code_buffer_count = 0;
        watched_code_free = nullptr; watched_code_frees = 0;
        watched_purge_allocation = nullptr;
        purge_wait_observed = purge_completed = free_before_purge = false;
        const auto finish = ScopeExit([&] {
            end_executor = true; reader.reset();
            if (executor.joinable()) executor.join();
            if (retry.joinable()) retry.join();
            watched_code_free = nullptr; watched_purge_allocation = nullptr; watch_multi_purge = false;
            watch_aot_allocation = watch_multi_allocations = capture_multi_lifetimes = capture_code_buffers = false;
            multi_oom_pushes = multi_protect_pushes = multi_post_push_failure_index = 0;
            multi_disarm_after_failing_unit = false; multi_fail_allocation_ordinal = 0;
            fail_after_aot_push = fail_aot_protect = false; fail_aot_allocation = -1;
            inject_aot_cleanup = fail_cleanup_allocation = false; cleanup_skip_queries = 0;
        });
        const auto wait_until = [](const std::atomic<bool>& flag) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            return flag.load();
        };
        executor = std::thread([&] {
            try {
                SmcTracker::ExecutionScope active{smc, token};
                executing = true;
                while (!end_executor) std::this_thread::yield();
                active.End();
            } catch (...) { executor_error = std::current_exception(); executing = true; }
        });
        Check(wait_until(executing), "admit a real executor before linked chain fault injection");

        watched_aot_size = 0;
        aot_failure_hit = aot_was_published = aot_published_at_failure = false;
        aot_protection_failure_hit = cleanup_query_observed = cleanup_failure_hit = false;
        watch_aot_allocation = watch_multi_allocations = capture_multi_lifetimes = capture_code_buffers = true;
        multi_oom_pushes = primary_oom ? fail_position + 1 : 0;
        multi_protect_pushes = primary_oom ? 0 : fail_position + 1;
        multi_post_push_failure_index = index;
        multi_fail_allocation_ordinal = fail_position;
        multi_disarm_after_failing_unit = primary_oom;
        inject_aot_cleanup = compound; cleanup_skip_queries = cleanup_skip;
        fail_after_aot_push = false; fail_aot_protect = false; fail_aot_allocation = -1;

        bool escaped{};
        int status = -1;
        try {
            if (aot_mode) status = aot::InstallArtifact(space, incoming, guest.memory, config.guest_addr_mask, report, error);
            else space.LoadJitCache();
        } catch (...) { escaped = true; }
        watch_aot_allocation = capture_multi_lifetimes = capture_code_buffers = false;
        multi_oom_pushes = multi_protect_pushes = 0;
        multi_disarm_after_failing_unit = false;
        inject_aot_cleanup = false; cleanup_skip_queries = 0; fail_cleanup_allocation = false;
        fail_aot_allocation = -1; fail_aot_protect = false;

        AttemptResult result{.primary_hit = primary_oom ? aot_failure_hit : aot_protection_failure_hit,
                             .cleanup_hit = cleanup_failure_hit};
        const bool& hit = result.primary_hit;
        Check(!require_primary_hit || hit, "the targeted linked chain primary failure was consumed");
        const unsigned expected_pushed = (!hit || !aot_mode) ? 3u : fail_position + 1;
        if (multi_lifetime_count != expected_pushed || captured_code_buffer_count != expected_pushed ||
            multi_allocation_count != expected_pushed)
            std::printf("linked_chain_track backend=%s mode=%.*s position=%u primary=%s index=%u hit=%u expected=%u lifetimes=%u buffers=%u allocations=%u\n",
                        aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), fail_position,
                        primary_oom ? "oom" : "protect", index, hit, expected_pushed,
                        multi_lifetime_count, captured_code_buffer_count, multi_allocation_count);
        Check(multi_lifetime_count == expected_pushed,
              "track every pushed linked chain lifetime in installation order");
        Check(captured_code_buffer_count == expected_pushed,
              "track every pushed linked chain code buffer in installation order");
        Check(multi_allocation_count == expected_pushed,
              "track every pushed linked chain allocation in installation order");
        for (unsigned position = 0; position < expected_pushed; ++position) {
            const auto lifetime = multi_lifetimes[position];
            const auto region = lifetime ? module->GetCodeRegion(lifetime->base) : std::nullopt;
            Check(lifetime && lifetime->entries.size() == fixture.entries[order[position]].size() && region &&
                  lifetime->base == captured_code_exec[position] &&
                  SiteRxToRw(*region, lifetime->base) == captured_code_rw[position] &&
                  multi_allocations[position] == captured_code_rw[position],
                  "bind both aliases and entries of each pushed linked chain unit");
        }
        const auto preserve_target = [&] {
            const auto current = links.QueryTarget(target_pc);
            Check(current && current->generation == target->generation && current->host_pc == target->host_pc &&
                  target_entries == std::array<size_t, 3>{space.GetCodeCacheTable().Lookup(target_pc),
                      space.GetCallCodeCacheTable().Lookup(target_pc), space.GetPendingCallCodeCacheTable().Lookup(target_pc)} &&
                  l1.Lookup(target_pc) == target_entries[0] && other_l1.Lookup(target_pc) == target_entries[0] &&
                  aot::ResolveCodeOffset(target_report.mapping, 0) == target_alias,
                  "linked chain failure preserves the independent target entries, L1s, generation and alias");
        };
        const auto execute_unit = [&](unsigned unit_index, VAddr pc) {
            Runtime runtime{&space};
            runtime.SetLocation(pc);
            Check(runtime.Run() == HaltReason::CallHost, "execute an accepted linked chain entry");
            u64 value{}, witness{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
            Check(value == 37 && witness == (unit_index == 2 ? 0 : 92),
                  "accepted linked chain preserves downstream and target witnesses");
        };
        const auto execute_target = [&] {
            Runtime runtime{&space};
            runtime.SetLocation(target_pc);
            Check(runtime.Run() == HaltReason::CallHost, "independent target executes after linked chain failure");
            u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            Check(value == 37, "linked chain failure preserves the independent target result");
        };
        const auto retire_position = [&](unsigned position) {
            const unsigned unit_index = order[position];
            const auto node = module->GetNode(Location{fixture.entries[unit_index].front()});
            const auto lifetime = multi_lifetimes[position];
            FaultEntry absent;
            Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
                  multi_allocation_frees[position] == 1 &&
                  links.QuerySourceSites({module.get(), lifetime->base}).empty() &&
                  !module->LookupCodeAllocation(lifetime->base, absent) && IsEmpty(node) == false,
                  "accepted linked chain unit retains ordinary exact reclamation");
            Check(IsEmpty(module->GetNode(Location{fixture.entries[unit_index].front()})),
                  "accepted linked chain retirement detaches its node");
        };
        const auto finish_accepted_disk_units = [&] {
            if (aot_mode) return;
            const auto accepted_unit = [&](unsigned unit_index) {
                for (unsigned position = 0; position < 3; ++position)
                    if (order[position] == unit_index) return position != fail_position;
                return false;
            };
            if (accepted_unit(2)) execute_unit(2, fixture.entries[2].front());
            if (accepted_unit(1) && accepted_unit(2)) {
                for (const auto pc : fixture.entries[1]) for (unsigned warm = 0; warm < 2; ++warm) execute_unit(1, pc);
                unsigned b_position = 0;
                while (order[b_position] != 1) ++b_position;
                const auto sites = links.QuerySourceSites({module.get(), multi_lifetimes[b_position]->base});
                Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
                    return site.state == LinkSiteState::Linked;
                }), "accepted downstream linked chain segment remains actually linked");
            }
            for (int position = 2; position >= 0; --position)
                if (unsigned(position) != fail_position) retire_position(unsigned(position));
        };

        if (!hit) {
            Check(!escaped && (aot_mode ? status == aot::kAotOk && report.units_installed == 3
                                         : space.GetJitDiskCache()->Stats().units_loaded == 3),
                  "the linked chain sweep reaches a committed three-unit installation");
            for (unsigned position = 0; position < 3; ++position)
                Check(multi_lifetimes[position]->Live(), "each linked chain endpoint unit remains live before execution");
            preserve_target();
            end_executor = true; executor.join(); reader.reset();
            if (executor_error) std::rethrow_exception(executor_error);
            for (unsigned unit_index = 0; unit_index < 3; ++unit_index)
                for (const auto pc : fixture.entries[unit_index])
                    for (unsigned warm = 0; warm < 2; ++warm) execute_unit(unit_index, pc);
            for (unsigned position = 0; position < 3; ++position) {
                const auto sites = links.QuerySourceSites({module.get(), multi_lifetimes[position]->base});
                Check(sites.size() == fixture.site_counts[order[position]] &&
                      std::all_of(sites.begin(), sites.end(), [](const auto& site) {
                          return site.state == LinkSiteState::Linked;
                      }), "successful linked chain links every same-artifact branch");
            }
            for (unsigned position = 0; position < 3; ++position) retire_position(position);
            ++successes;
            if (!filename.empty()) Check(unlink(filename.c_str()) == 0, "remove this owned linked chain disk fixture");
            return result;
        }

        ++failures;
        std::printf("linked_chain_failure_point backend=%s mode=%.*s position=%u unit=%u primary=%s index=%u cleanup_skip=%u\n",
                    aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), fail_position, order[fail_position],
                    primary_oom ? "oom" : "protect", index, cleanup_skip);
        if (aot_mode) {
            Check((escaped || status != aot::kAotOk) && !report.units_installed && !report.code_bytes &&
                  !report.dispatch_slots && !report.mapping,
                  "failed linked chain AOT installation reports zero committed units");
        } else {
            Check(!escaped && space.GetJitDiskCache()->Stats().units_loaded == 2,
                  "failed linked chain disk unit leaves exactly its two neighbors loaded");
        }
        for (unsigned position = 0; position < order.size(); ++position) {
            const bool accepted = !aot_mode && position != fail_position;
            if (position < expected_pushed) {
                Check(multi_allocation_frees[position] == 0, "no pushed linked chain allocation frees during executor grace");
                Check(accepted ? multi_lifetimes[position]->Live()
                               : multi_lifetimes[position]->Revoked() && !multi_lifetimes[position]->Commit(),
                      "linked chain failure separates accepted disk lifetimes from inert failed lifetimes");
            }
            for (const auto pc : fixture.entries[order[position]]) {
                if (accepted) {
                    Check(space.GetCodeCacheTable().Lookup(pc) && links.QueryTarget(pc),
                          "accepted linked chain disk entries remain published");
                } else {
                    Check(!space.GetCodeCacheTable().Lookup(pc) && !space.GetCallCodeCacheTable().Lookup(pc) &&
                          !space.GetPendingCallCodeCacheTable().Lookup(pc) && !links.QueryTarget(pc),
                          "failed linked chain rollback withdraws every failed entry");
                }
            }
            const auto node = module->GetNode(Location{fixture.entries[order[position]].front()});
            if (!accepted && IsFunction(node)) {
                result.retained[position] = true;
                const auto lifetime = multi_lifetimes[position];
                Check(GetFunction(node)->code_lifetime == lifetime &&
                      links.QuerySourceSites({module.get(), lifetime->base}).size() == fixture.site_counts[order[position]],
                      "cleanup failure retains the exact inert linked chain owner and outgoing sites");
            } else {
                Check(accepted ? IsFunction(node) : IsEmpty(node), "linked chain failure separates accepted and failed nodes");
            }
        }
        preserve_target();
        for (const bool retained : result.retained) if (retained) ++retained_owners;
        if (result.cleanup_hit) ++cleanup_attempts;
        if (compound && result.cleanup_hit) {
            bool any_retained = false;
            for (const bool retained : result.retained) any_retained = any_retained || retained;
            Check(cleanup_query_observed && any_retained,
                  "independent linked chain cleanup OOM is consumed and retains an inert owner");
        }

        if (!compound || !result.cleanup_hit) {
        watched_purge_allocation = multi_lifetimes[fail_position]->base;
        watch_multi_purge = true;
        bool expect_reader_wait = false;
        for (unsigned position = 0; position < expected_pushed; ++position) {
            const bool failed = aot_mode ? position <= fail_position : position == fail_position;
            expect_reader_wait = expect_reader_wait || (failed && fixture.site_counts[order[position]] != 0);
        }
        end_executor = true;
        if (expect_reader_wait) {
            Check(wait_until(purge_wait_observed) && !free_before_purge,
                  "linked chain failed-source purge reaches a real signal-reader wait");
            for (unsigned position = 0; position < expected_pushed; ++position) {
                if (fixture.site_counts[order[position]])
                    Check(multi_allocation_frees[position] == 0, "site-bearing linked chain aliases survive reader grace");
                Check(Mapped(multi_lifetimes[position]->base) && Mapped(multi_allocations[position]),
                      "all pushed linked chain aliases remain mapped during reader grace");
            }
        }
        reader.reset(); executor.join(); watch_multi_purge = false;
        if (executor_error) std::rethrow_exception(executor_error);
        Check(!free_before_purge && (!expect_reader_wait || purge_completed),
              "linked chain purge completes only after signal-reader exit");
        if (expect_reader_wait) ++reader_waits;
        for (unsigned position = 0; position < order.size(); ++position) {
            const bool failed = aot_mode ? position <= fail_position : position == fail_position;
            if (!failed) continue;
            FaultEntry absent;
            Check(multi_allocation_frees[position] == 1,
                  "ordinary linked chain rollback frees each failed allocation exactly once");
            Check(links.QuerySourceSites({module.get(), multi_lifetimes[position]->base}).empty(),
                  "ordinary linked chain rollback purges each failed source");
            Check(!module->LookupCodeAllocation(multi_lifetimes[position]->base, absent),
                  "ordinary linked chain rollback removes each failed fault allocation");
            Check(IsEmpty(module->GetNode(Location{fixture.entries[order[position]].front()})),
                  "ordinary linked chain rollback detaches each failed node");
        }
        preserve_target();
        execute_target();
        finish_accepted_disk_units();
        } else {
            end_executor = true; reader.reset(); executor.join();
            if (executor_error) std::rethrow_exception(executor_error);
            for (unsigned position = 0; position < expected_pushed; ++position) {
                const bool failed = aot_mode ? position <= fail_position : position == fail_position;
                if (failed) Check(multi_allocation_frees[position] == (result.retained[position] ? 0u : 1u),
                      "compound linked chain cleanup distinguishes queued frees from retained owners after grace");
            }
            if (terminal) {
                smc.UnregisterRuntime(other_token); smc.UnregisterRuntime(token); registered = false;
                const auto ticket = Terminal(space);
                Check(space.TryShutdown(ticket).Complete(), "terminal shutdown releases retained linked chain owners");
                for (unsigned position = 0; position < expected_pushed; ++position) {
                    Check(!Mapped(multi_lifetimes[position]->base) && !Mapped(multi_allocations[position]),
                          "terminal linked chain cleanup releases both aliases of every pushed allocation");
                    if (result.retained[position]) {
                        Check(multi_allocation_frees[position] == 0, "terminal retained linked chain allocation has no duplicate ordinary free");
                        ++terminal_releases;
                    }
                }
            } else {
                for (unsigned position = 0; position < order.size(); ++position) {
                    if (!result.retained[position]) continue;
                    const auto root = fixture.entries[order[position]].front();
                    const auto node = module->GetNode(Location{root});
                    auto* function = GetFunction(node).get();
                    const auto lifetime = multi_lifetimes[position];
                    watched_purge_allocation = lifetime->base;
                    purge_wait_observed = purge_completed = free_before_purge = false;
                    reader.emplace(links);
                    bool retired{};
                    std::exception_ptr retry_error;
                    std::atomic<bool> retry_done{};
                    retry = std::thread([&] {
                        try { retired = smc.RetireNode(space, &l1, module, function); }
                        catch (...) { retry_error = std::current_exception(); }
                        retry_done = true;
                    });
                    bool retry_wait_observed = false;
                    {
                        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                        while (!retry_done && std::chrono::steady_clock::now() < deadline) {
                            if (purge_wait_observed) { retry_wait_observed = true; break; }
                            std::this_thread::yield();
                        }
                    }
                    if (retry_wait_observed) {
                        Check(!purge_completed && multi_allocation_frees[position] == 0 &&
                              Mapped(lifetime->base) && Mapped(multi_allocations[position]),
                              "retained linked chain owner retry waits for a real signal reader");
                        ++reader_waits;
                    }
                    reader.reset(); retry.join();
                    if (retry_error) std::rethrow_exception(retry_error);
                    FaultEntry absent;
                    Check(retired && purge_completed && multi_allocation_frees[position] == 1 &&
                          links.QuerySourceSites({module.get(), lifetime->base}).empty() &&
                          !module->LookupCodeAllocation(lifetime->base, absent) &&
                          IsEmpty(module->GetNode(Location{root})),
                          "retained linked chain owner retry purges its sites before one exact free");
                }
                preserve_target();
                execute_target();
                finish_accepted_disk_units();
            }
        }
        if (!filename.empty()) Check(unlink(filename.c_str()) == 0, "remove this owned linked chain disk fixture");
        return result;
    };

    if (mode != "protect") {
        for (unsigned fail_position = 0; fail_position < 3; ++fail_position) {
            bool completed = false;
            unsigned shape_failures = 0;
            for (unsigned index = 0; index < 256; ++index) {
                if (!run_attempt(fail_position, true, index, SerialBlock::MetadataOnly, false, false, 0, false).primary_hit) {
                    completed = true;
                    break;
                }
                ++shape_failures;
            }
            Check(completed, "each linked chain prefix allocation sweep reaches its successful endpoint");
            std::printf("linked_chain_failure_shape backend=%s mode=%.*s position=%u primary=oom failures=%u\n",
                        aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), fail_position, shape_failures);
        }
    } else {
        for (const auto metadata : {SerialBlock::MetadataOnly, SerialBlock::DependencyOnly})
        for (const bool reverse_blocks : {false, true})
        for (const bool reverse_order : {false, true}) {
            unsigned shape_failures = 0;
            for (unsigned fail_position = 0; fail_position < 3; ++fail_position) {
                Check(run_attempt(fail_position, false, 0, metadata, reverse_blocks, reverse_order, 0, true).primary_hit,
                      "each linked chain protection shape consumes its targeted failure");
                ++shape_failures;
            }
            std::printf("linked_chain_failure_shape backend=%s mode=protect metadata=%u reverse=%u order_reverse=%u failures=%u\n",
                        aot_mode ? "aot" : "disk", unsigned(metadata), reverse_blocks, reverse_order, shape_failures);
        }
    }

    if (compound) {
        for (unsigned fail_position = 0; fail_position < 3; ++fail_position) {
            bool endpoint = false, saw_cleanup = false, saw_failing = false, saw_earlier = false;
            for (unsigned cleanup_skip = 0; cleanup_skip < 16; ++cleanup_skip) {
                const auto result = run_attempt(fail_position, false, 0, SerialBlock::MetadataOnly,
                                                false, false, cleanup_skip, true);
                if (!result.cleanup_hit) { endpoint = true; break; }
                saw_cleanup = true;
                saw_failing = saw_failing || result.retained[fail_position];
                for (unsigned position = 0; position < fail_position; ++position)
                    saw_earlier = saw_earlier || result.retained[position];
            }
            Check(endpoint && saw_cleanup && saw_failing,
                  "linked chain cleanup-query sweep reaches the failing owner and then an ordinary endpoint");
            if (aot_mode && fail_position) Check(saw_earlier,
                  "linked chain cleanup-query sweep also retains an earlier committed AOT prefix owner");
            std::printf("linked_chain_cleanup_shape backend=%s mode=%.*s position=%u failing_retained=%u earlier_retained=%u\n",
                        aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), fail_position,
                        saw_failing, saw_earlier);
        }
    }

    if (compound) {
        std::printf("linked_chain_failure backend=%s mode=%.*s failures=%u cleanup_attempts=%u retained_owners=%u successes=%u reader_waits=%u terminal_releases=%u units=3 entries=5 sites=4\n",
                    aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), failures, cleanup_attempts,
                    retained_owners, successes, reader_waits, terminal_releases);
    } else {
        std::printf("linked_chain_failure backend=%s mode=%.*s failures=%u successes=%u reader_waits=%u units=3 entries=5 sites=4\n",
                    aot_mode ? "aot" : "disk", int(mode.size()), mode.data(), failures, successes, reader_waits);
    }
}

void LinkedPublicationRace(bool aot_mode, std::string_view race) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-linked-race-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-linked-race-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate real linked publication races");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const auto target_image = Artifact(config, guest.memory, true);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the independent target before linked publication race");
    const auto target = links.QueryTarget(target_pc);
    const auto target_entry = space.GetCodeCacheTable().Lookup(target_pc);
    auto* target_alias = aot::ResolveCodeOffset(target_report.mapping, 0);
    Check(target && target_entry && target_alias, "bind independent target before linked publication race");

    auto incoming = image;
    std::string filename;
    if (!aot_mode) {
        auto disk = captured;
        disk.blocks = incoming.units.front().blocks;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(incoming.dispatch_slots.size());
        for (const auto& [key, slot] : incoming.dispatch_slots) { payload.U64(key); payload.U32(slot); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> path{};
        std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        filename = path.data();
        std::ofstream output(filename, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
        Check(output.good(), "write the valid linked publication race disk fixture");
    }
    const auto preserve_target = [&] {
        const auto current = links.QueryTarget(target_pc);
        Check(current && current->generation == target->generation && current->host_pc == target->host_pc &&
              space.GetCodeCacheTable().Lookup(target_pc) == target_entry &&
              aot::ResolveCodeOffset(target_report.mapping, 0) == target_alias,
              "linked publication race preserves the independent target");
    };
    const auto execute_target = [&] {
        Runtime runtime{&space};
        runtime.SetLocation(target_pc);
        Check(runtime.Run() == HaltReason::CallHost, "independent target executes after linked publication race");
        u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        Check(value == 37, "linked publication race preserves the independent target result");
    };

    watched_aot_size = captured.code.size();
    watched_code_free = nullptr; watched_code_frees = 0;
    aot_was_published = false; published_failure_lifetime.reset();
    const auto reset_watch = ScopeExit([] {
        watch_aot_allocation = capture_published_lifetime = false;
        watched_code_free = nullptr; published_failure_lifetime.reset();
        pause_protection = nullptr; finish_protection = true;
    });
    watch_aot_allocation = capture_published_lifetime = true;

    if (race == "duplicate") {
        Check(aot_mode, "duplicate publication race is AOT-only");
        std::atomic<bool> go{};
        std::array<int, 2> statuses{-1, -1};
        std::array<aot::LoadReport, 2> reports;
        std::array<std::exception_ptr, 2> errors{};
        std::array<std::string, 2> messages;
        std::array<std::thread, 2> publishers;
        for (unsigned i = 0; i < publishers.size(); ++i) publishers[i] = std::thread([&, i] {
            while (!go) std::this_thread::yield();
            try {
                statuses[i] = aot::InstallArtifact(space, incoming, guest.memory,
                                                   config.guest_addr_mask, reports[i], messages[i]);
            } catch (...) { errors[i] = std::current_exception(); }
        });
        go = true;
        for (auto& publisher : publishers) publisher.join();
        watch_aot_allocation = capture_published_lifetime = false;
        for (const auto& failure : errors) if (failure) std::rethrow_exception(failure);
        const unsigned successes = (statuses[0] == aot::kAotOk) + (statuses[1] == aot::kAotOk);
        Check(successes == 1 && published_failure_lifetime && watched_code_free,
              "concurrent duplicate linked installs admit exactly one owner");
        preserve_target();
        for (const auto pc : {source_pc, secondary_pc}) for (unsigned warm = 0; warm < 2; ++warm) {
            Runtime runtime{&space};
            runtime.SetLocation(pc);
            Check(runtime.Run() == HaltReason::CallHost, "winning duplicate linked source executes");
            u64 value{}, witness{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
            Check(value == 37 && witness == 91, "winning duplicate linked source preserves witnesses");
        }
        const auto lifetime = published_failure_lifetime;
        const auto sites = links.QuerySourceSites({module.get(), lifetime->base});
        Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
            return site.state == LinkSiteState::Linked;
        }), "winning duplicate linked source links both branches once");
        FaultEntry absent;
        Check(smc.RetireNode(space, nullptr, module, GetFunction(module->GetNode(Location{source_pc})).get()) &&
              watched_code_frees == 1 && links.QuerySourceSites({module.get(), lifetime->base}).empty() &&
              !module->LookupCodeAllocation(lifetime->base, absent),
              "winning duplicate linked source retains one exact reclamation");
        execute_target();
        std::printf("linked_race backend=aot race=duplicate successes=1 rejected=1 entries=2 sites=2 frees=1\n");
        return;
    }

    const bool source_race = race == "source";
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::atomic<bool> executing{}, end_executor{};
    std::exception_ptr executor_error;
    std::thread executor;
    std::optional<LinkManager::SignalReader> reader;
    if (source_race) {
        reader.emplace(links);
        executor = std::thread([&] {
            try {
                SmcTracker::ExecutionScope active{smc, token};
                executing = true;
                while (!end_executor) std::this_thread::yield();
                active.End();
            } catch (...) { executor_error = std::current_exception(); executing = true; }
        });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!executing && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        Check(executing, "admit executor before linked source publication race");
    }
    const auto finish_threads = ScopeExit([&] {
        end_executor = true; reader.reset();
        if (executor.joinable()) executor.join();
    });

    pause_protection = static_cast<u8*>(guest.memory) + 2 * page;
    protection_paused = finish_protection = false;
    aot::LoadReport report;
    int status = -1;
    std::exception_ptr install_error, invalidate_error;
    std::thread publisher([&] {
        try {
            if (aot_mode) status = aot::InstallArtifact(space, incoming, guest.memory, config.guest_addr_mask, report, error);
            else space.LoadJitCache();
        } catch (...) { install_error = std::current_exception(); }
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!protection_paused && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    Check(protection_paused, "pause the linked installer between node publication and entry commit");
    const auto pending_node = module->GetNode(Location{source_pc});
    Check(IsFunction(pending_node), "paused linked installer has published its actual node");
    const auto lifetime = GetFunction(pending_node)->code_lifetime;
    Check(lifetime && !lifetime->Live() && !space.GetCodeCacheTable().Lookup(source_pc),
          "paused linked allocation is pending and invisible before commit");
    const auto region = module->GetCodeRegion(lifetime->base);
    Check(region && SiteRxToRw(*region, lifetime->base) == watched_code_free,
          "paused linked allocation binds both real aliases");
    std::atomic<bool> invalidation_requested{};
    const VAddr invalidated = source_race ? source_pc : target_pc;
    std::thread invalidator([&] {
        invalidation_requested = true;
        try { smc.InvalidateRange(space, nullptr, invalidated, invalidated + 4); }
        catch (...) { invalidate_error = std::current_exception(); }
    });
    while (!invalidation_requested) std::this_thread::yield();
    finish_protection = true;
    publisher.join(); invalidator.join();
    pause_protection = nullptr;
    watch_aot_allocation = capture_published_lifetime = false;
    if (install_error) std::rethrow_exception(install_error);
    if (invalidate_error) std::rethrow_exception(invalidate_error);
    Check(aot_mode ? status == aot::kAotOk && report.units_installed == 1
                   : space.GetJitDiskCache()->Stats().units_loaded == 1,
          "racing invalidation does not corrupt the linked installation result");

    if (source_race) {
        Check(lifetime->Revoked() && !lifetime->Commit() &&
              !space.GetCodeCacheTable().Lookup(source_pc) && !space.GetCodeCacheTable().Lookup(secondary_pc) &&
              IsEmpty(module->GetNode(Location{source_pc})) && watched_code_frees == 0,
              "racing source invalidation consumes the linked installation before executor exit");
        preserve_target();
        watched_purge_allocation = lifetime->base;
        purge_wait_observed = purge_completed = free_before_purge = false;
        end_executor = true;
        const auto purge_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!purge_wait_observed && std::chrono::steady_clock::now() < purge_deadline) std::this_thread::yield();
        Check(purge_wait_observed && !purge_completed && !free_before_purge && watched_code_frees == 0 &&
              Mapped(lifetime->base) && Mapped(watched_code_free.load()),
              "racing source purge waits for the real signal reader");
        reader.reset(); executor.join();
        if (executor_error) std::rethrow_exception(executor_error);
        FaultEntry absent;
        Check(purge_completed && watched_code_frees == 1 &&
              links.QuerySourceSites({module.get(), lifetime->base}).empty() &&
              !module->LookupCodeAllocation(lifetime->base, absent),
              "racing source invalidation purges both sites before one exact free");
        execute_target();
        std::printf("linked_race backend=%s race=source install_ok=1 consumed=1 reader_wait=1 frees=1 entries=2 sites=2\n",
                    aot_mode ? "aot" : "disk");
    } else {
        Check(lifetime->Live() && !links.QueryTarget(target_pc) &&
              IsEmpty(module->GetNode(Location{target_pc})) &&
              space.GetCodeCacheTable().Lookup(source_pc) && space.GetCodeCacheTable().Lookup(secondary_pc),
              "racing target invalidation removes only the independent target after source commit");
        const auto sites = links.QuerySourceSites({module.get(), lifetime->base});
        Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
            return site.state == LinkSiteState::Unlinked;
        }), "racing target invalidation restores both linked branches to unlinked");
        FaultEntry absent;
        Check(smc.RetireNode(space, nullptr, module, GetFunction(module->GetNode(Location{source_pc})).get()) &&
              watched_code_frees == 1 && links.QuerySourceSites({module.get(), lifetime->base}).empty() &&
              !module->LookupCodeAllocation(lifetime->base, absent),
              "source survives target race until ordinary one-time retirement");
        std::printf("linked_race backend=%s race=target install_ok=1 target_retired=1 source_live=1 sites=2 frees=1\n",
                    aot_mode ? "aot" : "disk");
    }
    if (!filename.empty()) Check(unlink(filename.c_str()) == 0, "remove this owned linked race disk fixture");
}

void LinkedReuse(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-linked-reuse-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-linked-reuse-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate real linked allocation reuse");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const VAddr target_pc = 0x100;
    const auto target_image = Artifact(config, guest.memory, true);
    const auto fixture = MakeLinkedMultiUnitFixture(config, guest.memory, target_image, target_pc);

    auto incoming = fixture.image;
    for (auto& unit : incoming.units) {
        unit.blocks.push_back({.guest_start = target_pc, .guest_end = target_pc + 4,
            .code_offset = UINT32_MAX,
            .guest_bytes_hash = HashBytes(static_cast<u8*>(guest.memory) + target_pc, 4, 0xCBF29CE484222325ull),
            .entry_flags = u8(SerialBlock::MetadataOnly)});
    }
    std::vector<SerialUnit> incoming_disk;
    for (unsigned i = 0; i < incoming.units.size(); ++i) {
        auto disk = fixture.disk_units[i];
        disk.blocks = incoming.units[i].blocks;
        incoming_disk.push_back(std::move(disk));
    }
    auto replacement = target_image;
    replacement.units = {incoming.units[1]};
    replacement.units.front().code_offset = 0;
    replacement.code = std::vector<u8>(incoming.code.begin() + fixture.image.units[1].code_offset,
                                       incoming.code.begin() + fixture.image.units[1].code_offset +
                                       fixture.image.units[1].code_size);
    replacement.dispatch_slots = incoming.dispatch_slots;
    replacement.code_hash = HashBytes(replacement.code.data(), replacement.code.size(), 0xCBF29CE484222325ull);

    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the independent target before linked reuse");
    const auto target = links.QueryTarget(target_pc);
    const auto target_entry = space.GetCodeCacheTable().Lookup(target_pc);
    auto* target_alias = aot::ResolveCodeOffset(target_report.mapping, 0);
    Check(target && target_entry && target_alias, "bind the independent target before linked reuse");

    std::string filename;
    if (!aot_mode) {
        for (auto& disk : incoming_disk) {
            disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
            if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
                return site.host_end != site.host_begin + sizeof(u32);
            });
        }
        BlobWriter payload;
        payload.U64(incoming.dispatch_slots.size());
        for (const auto& [key, slot] : incoming.dispatch_slots) { payload.U64(key); payload.U32(slot); }
        payload.U64(incoming_disk.size());
        for (const auto& disk : incoming_disk) WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> path{};
        std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        filename = path.data();
        std::ofstream output(filename, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
        Check(output.good(), "write the valid linked reuse disk fixture");
    }

    TranslateTable l1{8}, other_l1{8};
    const auto token = smc.RegisterRuntime(l1), other_token = smc.RegisterRuntime(other_l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(other_token); smc.UnregisterRuntime(token); });
    l1.Put(target_pc, target_entry); other_l1.Put(target_pc, target_entry);
    std::atomic<bool> executing{}, end_executor{};
    std::exception_ptr executor_error;
    std::thread executor;
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    multi_allocations.fill(nullptr); multi_allocation_frees.fill(0); multi_allocation_count = 0;
    multi_lifetimes.fill({}); multi_lifetime_count = 0;
    watched_code_free = nullptr; watched_code_frees = 0;
    watched_purge_allocation = nullptr;
    purge_wait_observed = purge_completed = free_before_purge = false;
    const auto finish = ScopeExit([&] {
        end_executor = true; reader.reset();
        if (executor.joinable()) executor.join();
        watched_code_free = nullptr; watched_purge_allocation = nullptr; watch_multi_purge = false;
        watch_aot_allocation = watch_multi_allocations = capture_multi_lifetimes = false;
        capture_published_lifetime = false; published_failure_lifetime.reset();
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    executor = std::thread([&] {
        try {
            SmcTracker::ExecutionScope active{smc, token};
            executing = true;
            while (!end_executor) std::this_thread::yield();
            active.End();
        } catch (...) { executor_error = std::current_exception(); executing = true; }
    });
    Check(wait_until(executing), "admit a real executor before linked reuse installation");

    watched_aot_size = fixture.image.units.front().code_size;
    watch_aot_allocation = watch_multi_allocations = capture_multi_lifetimes = true;
    int status = -1;
    if (aot_mode) status = aot::InstallArtifact(space, incoming, guest.memory, config.guest_addr_mask, report, error);
    else space.LoadJitCache();
    watch_aot_allocation = capture_multi_lifetimes = false;
    Check(aot_mode ? status == aot::kAotOk && report.units_installed == 3
                   : space.GetJitDiskCache()->Stats().units_loaded == 3,
          "install three linked units before exact-address reuse");
    Check(multi_lifetime_count == 3 && multi_allocation_count == 3, "track all three linked reuse allocations");
    const auto preserve_target = [&] {
        const auto current = links.QueryTarget(target_pc);
        Check(current && current->generation == target->generation && current->host_pc == target->host_pc &&
              space.GetCodeCacheTable().Lookup(target_pc) == target_entry &&
              l1.Lookup(target_pc) == target_entry && other_l1.Lookup(target_pc) == target_entry &&
              aot::ResolveCodeOffset(target_report.mapping, 0) == target_alias,
              "linked reuse preserves the independent target generation, entries, L1s and alias");
    };
    const auto execute_entry = [&](VAddr pc, u64 witness) {
        Runtime runtime{&space};
        runtime.SetLocation(pc);
        Check(runtime.Run() == HaltReason::CallHost, "execute a linked reuse entry through the independent target");
        u64 value{}, observed{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&observed, runtime.GetUniformBuffer().data() + 16, 8);
        Check(value == 37 && observed == witness, "linked reuse preserves source and target witnesses");
    };
    for (unsigned unit_index = 0; unit_index < 3; ++unit_index) {
        const auto lifetime = multi_lifetimes[unit_index];
        const auto region = lifetime ? module->GetCodeRegion(lifetime->base) : std::nullopt;
        Check(lifetime && lifetime->Live() && lifetime->entries.size() == 2 && region &&
              SiteRxToRw(*region, lifetime->base) == multi_allocations[unit_index],
              "bind both aliases of each linked reuse allocation");
        for (const auto pc : std::array<VAddr, 2>{fixture.entries[unit_index].first, fixture.entries[unit_index].second})
            for (unsigned warm = 0; warm < 2; ++warm) execute_entry(pc, 91);
        const auto sites = links.QuerySourceSites({module.get(), lifetime->base});
        Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
            return site.state == LinkSiteState::Linked;
        }), "all linked reuse units link both physical branches before retirement");
    }
    preserve_target();

    constexpr unsigned middle = 1;
    const auto old_lifetime = multi_lifetimes[middle];
    auto* const old_rx = old_lifetime->base;
    void* const old_rw = multi_allocations[middle];
    const auto middle_node = module->GetNode(Location{fixture.entries[middle].first});
    Check(smc.RetireNode(space, &l1, module, GetFunction(middle_node).get()) &&
          old_lifetime->Revoked() && multi_allocation_frees[middle] == 0,
          "retire only the middle linked unit under executor grace");
    for (const auto pc : std::array<VAddr, 2>{fixture.entries[middle].first, fixture.entries[middle].second})
        Check(!space.GetCodeCacheTable().Lookup(pc) && !links.QueryTarget(pc),
              "retired middle linked entries withdraw before address reuse");
    watched_purge_allocation = old_lifetime->base;
    watch_multi_purge = true;
    end_executor = true;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge &&
          multi_allocation_frees[middle] == 0 && Mapped(old_rx) && Mapped(old_rw),
          "middle linked purge waits for the real signal reader");
    reader.reset(); executor.join(); watch_multi_purge = false;
    if (executor_error) std::rethrow_exception(executor_error);
    FaultEntry absent;
    Check(purge_completed && multi_allocation_frees[middle] == 1 &&
          links.QuerySourceSites({module.get(), old_lifetime->base}).empty() &&
          !module->LookupCodeAllocation(old_lifetime->base, absent) &&
          IsEmpty(module->GetNode(Location{fixture.entries[middle].first})),
          "middle linked allocation purges and frees exactly once");
    for (const unsigned unit_index : {0u, 2u}) {
        const auto lifetime = multi_lifetimes[unit_index];
        Check(lifetime->Live() && multi_allocation_frees[unit_index] == 0 &&
              space.GetCodeCacheTable().Lookup(fixture.entries[unit_index].first),
              "neighboring linked units remain live after partial allocation release");
        const auto sites = links.QuerySourceSites({module.get(), lifetime->base});
        Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
            return site.state == LinkSiteState::Linked;
        }), "neighboring linked branches remain linked after partial release");
    }
    preserve_target();

    // The old RW pointer is now free. Stop counting it as the middle slot so
    // the replacement's eventual free at the same address counts only once.
    multi_allocations[middle] = nullptr;
    watch_aot_allocation = capture_multi_lifetimes = true;
    aot::LoadReport replacement_report;
    const auto replacement_status = aot::InstallArtifact(space, replacement, guest.memory,
                                                         config.guest_addr_mask, replacement_report, error);
    watch_aot_allocation = capture_multi_lifetimes = false;
    Check(replacement_status == aot::kAotOk && replacement_report.units_installed == 1 &&
          multi_lifetime_count == 4 && multi_allocation_count == 4,
          "install a fresh linked generation in the released allocation");
    const auto new_lifetime = multi_lifetimes[3];
    Check(new_lifetime && new_lifetime != old_lifetime && new_lifetime->base == old_rx &&
          multi_allocations[3] == old_rw && old_lifetime->Revoked() && !old_lifetime->Live() && new_lifetime->Live(),
          "allocator address reuse binds only the fresh linked lifetime");
    Check(!aot::ResolveCodeOffset(report.mapping, fixture.image.units[1].code_offset) &&
          aot::ResolveCodeOffset(replacement_report.mapping, 0) == new_lifetime->base,
          "retained old artifact mapping cannot revive the reused address");
    for (const auto pc : std::array<VAddr, 2>{fixture.entries[middle].first, fixture.entries[middle].second})
        for (unsigned warm = 0; warm < 2; ++warm) execute_entry(pc, 91);
    for (const unsigned unit_index : {0u, 2u}) execute_entry(fixture.entries[unit_index].first, 91);
    const auto new_sites = links.QuerySourceSites({module.get(), new_lifetime->base});
    Check(new_sites.size() == 2 && std::all_of(new_sites.begin(), new_sites.end(), [](const auto& site) {
        return site.state == LinkSiteState::Linked;
    }), "fresh generation links both branches at the reused address");
    preserve_target();

    const auto retire_live = [&](unsigned lifetime_index, unsigned unit_index) {
        const auto lifetime = multi_lifetimes[lifetime_index];
        const auto node = module->GetNode(Location{fixture.entries[unit_index].first});
        FaultEntry gone;
        Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
              multi_allocation_frees[lifetime_index] == 1 &&
              links.QuerySourceSites({module.get(), lifetime->base}).empty() &&
              !module->LookupCodeAllocation(lifetime->base, gone) &&
              IsEmpty(module->GetNode(Location{fixture.entries[unit_index].first})),
              "linked reuse generations and neighbors each retire exactly once");
    };
    retire_live(3, middle);
    retire_live(0, 0);
    retire_live(2, 2);
    Check(multi_allocation_frees[middle] == 1, "old middle generation free count is not double-counted at the reused address");
    preserve_target();
    Runtime runtime{&space};
    runtime.SetLocation(target_pc);
    Check(runtime.Run() == HaltReason::CallHost, "independent target executes after linked reuse");
    u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
    Check(value == 37, "linked reuse preserves the independent target result");
    if (!filename.empty()) Check(unlink(filename.c_str()) == 0, "remove this owned linked reuse disk fixture");
    std::printf("linked_reuse backend=%s retired=1 reused=1 neighbors=2 old_frees=1 new_frees=1 entries=6 sites=6\n",
                aot_mode ? "aot" : "disk");
}

void LinkedPartialViewRelease(bool aot_mode, bool writable) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-linked-partial-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-linked-partial-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate real linked partial-view release");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const auto target_image = Artifact(config, guest.memory, true);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the independent target before linked partial release");
    std::string filename;
    if (!aot_mode) {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, slot] : image.dispatch_slots) { payload.U64(key); payload.U32(slot); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> path{};
        std::snprintf(path.data(), path.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        filename = path.data();
        std::ofstream output(filename, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
        Check(output.good(), "write the valid linked partial-release disk fixture");
    }
    if (aot_mode) {
        Check(aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error) == aot::kAotOk &&
              report.units_installed == 1, "install the linked source before partial view release");
    } else {
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "revive the linked source before partial view release");
    }
    for (const auto pc : {source_pc, secondary_pc}) for (unsigned warm = 0; warm < 2; ++warm) {
        Runtime runtime{&space};
        runtime.SetLocation(pc);
        Check(runtime.Run() == HaltReason::CallHost, "execute linked source before partial view release");
        u64 value{}, witness{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
        Check(value == 37 && witness == 91, "linked partial release preserves execution witnesses before shutdown");
    }
    const auto source_node = module->GetNode(Location{source_pc});
    const auto lifetime = GetFunction(source_node)->code_lifetime;
    const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
    Check(lifetime && region && links.QuerySourceSites({module.get(), lifetime->base}).size() == 2,
          "bind the linked source arena before partial view release");
    auto alias = std::make_shared<CounterAlias>();
    Check(space.RegisterTerminalAlias(alias), "register linked partial-release terminal alias");
    auto* const failed_view = writable ? static_cast<void*>(region->rw_base) : static_cast<void*>(region->rx_base);
    failing_unmap = failed_view;
    unmap_budget = 2;
    const auto reset_unmap = ScopeExit([] {
        failing_unmap = nullptr;
        unmap_budget = 0;
    });
    const unsigned destroys_before = arena_destroys;
    const auto ticket = Terminal(space);
    const auto first = space.TryShutdown(ticket);
    const unsigned destroys_after_first = arena_destroys;
    Check(!first.Complete() && first.stage == AddressSpace::ShutdownStage::Modules &&
          first.mapping.unmap_error == EIO && first.failed_module == module.get() &&
          alias->calls == 1 && !aot::ResolveCodeOffset(target_report.mapping, 0) &&
          (aot_mode ? !aot::ResolveCodeOffset(report.mapping, 0) : true),
          "first partial linked view release is incomplete after alias revocation");
    Check(Mapped(failed_view), "the failed linked arena view remains mapped after first shutdown attempt");
    if (writable) {
        Check(!Mapped(region->rx_base), "successful RX alias is already unmapped while RW release is retained");
    } else {
        Check(Mapped(region->rw_base), "RW alias is not attempted before the failed RX alias");
    }
    const auto second = space.TryShutdown(ticket);
    Check(!second.Complete() && second.stage == AddressSpace::ShutdownStage::Modules &&
          second.mapping.unmap_error == EIO && second.failed_module == module.get() &&
          alias->calls == 1 && arena_destroys == destroys_after_first && Mapped(failed_view),
          "second partial linked view release retries without revoking or destroying again");
    const auto third = space.TryShutdown(ticket);
    Check(third.Complete() && !Mapped(region->rx_base) && !Mapped(region->rw_base) && alias->calls == 1,
          "third partial linked view release completes both arena views exactly once");
    Check(destroys_after_first > destroys_before, "partial linked release actually reached arena destruction");
    if (!filename.empty()) Check(unlink(filename.c_str()) == 0, "remove this owned linked partial-release disk fixture");
    std::printf("linked_partial backend=%s view=%s incomplete_attempts=2 arena_destroyed_once=1 views_released=2\n",
                aot_mode ? "aot" : "disk", writable ? "rw" : "rx");
}

u64 LinkedExecutorCallback() noexcept {
    linked_executor_inside = true;
    while (!linked_executor_release) std::this_thread::yield();
    return 84;
}

void LinkedExecutorRetire() {
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink;
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    const u64 source_pc = 0x100, target_pc = static_cast<u64>(getpagesize()) + source_pc;
    const auto compile = [&](bool source) {
        HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
        const u64 pc = source ? source_pc : target_pc;
        auto* function = builder.AppendFunction(Location{pc}, Location{pc + 4});
        if (source) {
            const auto callback = static_cast<u64>(reinterpret_cast<uintptr_t>(
                    FptrCast(static_cast<u64 (*)()>(&LinkedExecutorCallback))));
            const auto result = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{callback}});
            function->StoreUniform(Uniform{24, ValueType::U64}, result);
            function->StoreUniform(Uniform{16, ValueType::U64},
                                   function->LoadImm(ValueType::U64, Imm{u64{91}}));
            function->EndBlock(terminal::LinkBlock{Location{target_pc}});
        } else {
            function->StoreUniform(Uniform{0, ValueType::U64},
                                   function->LoadImm(ValueType::U64, Imm{u64{37}}));
            function->EndBlock(terminal::ReturnToHost{});
        }
        function->EndFunction();
        return static_cast<u8*>(TranslateIR(module, function));
    };
    auto* target = compile(false);
    auto* source = compile(true);
    Check(source && target && module->IsDirectLinkConfigured(), "compile real linked executor source and target");
    linked_executor_inside = false;
    linked_executor_release = true;
    for (unsigned warm = 0; warm < 2; ++warm) {
        Runtime runtime{&space};
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == HaltReason::CallHost, "warm the real linked executor source");
        u64 value{}, witness{}, callback_result{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
        std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + 24, 8);
        Check(value == 37 && witness == 91 && callback_result == 84,
              "real linked executor warmup preserves callback, source and target witnesses");
    }
    FaultEntry allocation;
    Check(module->LookupCodeAllocation(source, allocation), "bind the real linked executor allocation");
    const LinkSourceOwner owner{module.get(), allocation.host_start};
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 1 && sites.front().state == LinkSiteState::Linked,
          "real linked executor source is actually linked before retirement");
    const auto site = sites.front().site;
    const auto region = module->GetCodeRegion(site.region_id);
    Check(region.has_value(), "bind the real linked executor source region");
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the real linked executor source node");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runner;
    const auto finish = ScopeExit([&] {
        linked_executor_release = true;
        reader.reset();
        if (runner.joinable()) runner.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
    });
    watched_code_free = SiteRxToRw(*region, allocation.host_start);
    watched_code_frees = 0;
    watched_purge_allocation = allocation.host_start;
    purge_wait_observed = purge_completed = free_before_purge = false;
    linked_executor_inside = linked_executor_release = false;
    std::exception_ptr runner_error;
    HaltReason run_reason{};
    u64 run_value{}, run_witness{}, run_callback{};
    runner = std::thread([&] {
        try {
            Runtime runtime{&space};
            runtime.SetLocation(source_pc);
            run_reason = runtime.Run();
            std::memcpy(&run_value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&run_witness, runtime.GetUniformBuffer().data() + 16, 8);
            std::memcpy(&run_callback, runtime.GetUniformBuffer().data() + 24, 8);
        } catch (...) { runner_error = std::current_exception(); }
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    Check(wait_until(linked_executor_inside), "real linked executor reaches its blocking callback");
    Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
          watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
          "retire a linked source while its real executor is inside the allocation");
    Check(links.QueryTarget(target_pc) && space.GetCodeCacheTable().Lookup(target_pc),
          "linked executor retirement preserves the independent target");
    linked_executor_release = true;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge &&
          watched_code_frees == 0 && Mapped(allocation.host_start) && Mapped(watched_code_free.load()),
          "real linked executor purge waits for the signal reader after execution");
    reader.reset();
    runner.join();
    if (runner_error) std::rethrow_exception(runner_error);
    FaultEntry absent;
    Check(run_reason == HaltReason::CallHost && run_value == 37 && run_witness == 91 && run_callback == 84 &&
          purge_completed && watched_code_frees == 1 &&
          links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(allocation.host_start, absent),
          "real linked executor completes through the target before one exact free");
    Runtime runtime{&space};
    runtime.SetLocation(target_pc);
    Check(runtime.Run() == HaltReason::CallHost, "independent target executes after real linked executor retirement");
    u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
    Check(value == 37, "real linked executor retirement preserves the target result");
    std::printf("linked_executor backend=jit retired=1 reader_wait=1 frees=1 entries=1 sites=1\n");
}

void LinkedRestoredExecutorRetire(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-restored-executor-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-restored-executor-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the restored executor disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100;
    const auto target_image = Artifact(config, guest.memory, true);
    auto image = target_image;
    SerialUnit captured;
    {
        AddressSpace source{config};
        source.SetSerializableCodeEmission();
        const auto module = source.GetDefaultModule();
        HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
        auto* function = builder.AppendFunction(Location{source_pc}, Location{source_pc + 4});
        const auto callback = static_cast<u64>(reinterpret_cast<uintptr_t>(
                FptrCast(static_cast<u64 (*)()>(&LinkedExecutorCallback))));
        const auto result = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{callback}});
        function->StoreUniform(Uniform{24, ValueType::U64}, result);
        function->StoreUniform(Uniform{16, ValueType::U64},
                               function->LoadImm(ValueType::U64, Imm{u64{91}}));
        function->EndBlock(terminal::LinkBlock{Location{target_pc}});
        function->EndFunction();
        auto* entry = static_cast<u8*>(TranslateIR(module, function));
        FaultEntry extent;
        Check(entry && module->LookupCodeAllocation(entry, extent), "compile the restored executor source allocation");
        std::string error;
        Check(CaptureSerialCode(*module, extent.host_start, u32(extent.host_end - extent.host_start),
              GetHostImage(), config.loc_end, captured, &error), error.c_str());
        const auto target = source.GetLinkManager().QueryTarget(source_pc);
        Check(target && captured.link_sites.size() == 1, "capture the restored executor branch and complete source entry");
        const auto offset = [&](const void* pointer) -> u32 {
            if (!pointer) return UINT32_MAX;
            const auto* address = static_cast<const u8*>(pointer);
            Check(address >= extent.host_start && address < extent.host_end, "captured restored executor entry belongs to its allocation");
            return u32(address - extent.host_start);
        };
        SerialBlock block{source_pc, source_pc + 4, offset(target->host_pc),
            HashBytes(static_cast<u8*>(guest.memory) + source_pc, 4, 0xCBF29CE484222325ull),
            offset(target->direct_host_pc), offset(target->pending_flags_host_pc), target->pending_flags_contract};
        block.continuation_code_offset = offset(target->call_host_pc);
        block.pending_flags_continuation_code_offset = offset(target->call_pending_flags_host_pc);
        captured.blocks = {block};
        source.GetCodeCacheTable().ForEachEntry([&](u32 index, size_t key, size_t) {
            image.dispatch_slots.emplace_back(u64(key), index);
        });
    }
    captured.guest_start = source_pc; captured.is_function = 1;
    captured.fault_sites.push_back({source_pc, 0, u32(captured.code.size())});
    aot::AotUnit unit{};
    unit.guest_start = source_pc; unit.code_size = captured.code.size(); unit.is_function = 1;
    unit.blocks = captured.blocks; unit.relocs = captured.relocs; unit.fault_sites = captured.fault_sites;
    unit.link_sites = captured.link_sites; unit.region_branches = captured.region_branches;
    image.units = {unit}; image.code = captured.code;
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);

    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the independent target before restoring the executor source");
    if (aot_mode) {
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "restore the actual linked AOT executor source" : error.c_str());
    } else {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, index] : image.dispatch_slots) { payload.U64(key); payload.U32(index); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> filename{};
        std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the actual linked executor disk fixture");
        }
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the executor source");
        Check(unlink(filename.data()) == 0, "remove the owned linked executor disk fixture");
    }
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the exact restored executor source node");
    const auto lifetime = GetFunction(node)->code_lifetime;
    const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
    Check(lifetime && region && lifetime->entries.size() == 1, "bind the published entry to one restored executor allocation");
    const LinkSourceOwner owner{module.get(), lifetime->base};
    const auto target_entry = space.GetCodeCacheTable().Lookup(target_pc);
    linked_executor_inside = false;
    linked_executor_release = true;
    for (unsigned warm = 0; warm < 2; ++warm) {
        Runtime runtime{&space};
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == HaltReason::CallHost, "warm the restored linked executor source");
        u64 value{}, witness{}, callback_result{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
        std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + 24, 8);
        Check(value == 37 && witness == 91 && callback_result == 84,
              "restored linked executor warmup preserves callback, source and target witnesses");
    }
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 1 && sites.front().state == LinkSiteState::Linked,
          "restored linked executor source is actually linked before retirement");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runner;
    const auto finish = ScopeExit([&] {
        linked_executor_release = true;
        reader.reset();
        if (runner.joinable()) runner.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
    });
    watched_code_free = SiteRxToRw(*region, lifetime->base);
    watched_code_frees = 0;
    watched_purge_allocation = lifetime->base;
    purge_wait_observed = purge_completed = free_before_purge = false;
    linked_executor_inside = linked_executor_release = false;
    std::exception_ptr runner_error;
    HaltReason miss_reason{}, run_reason{};
    u64 resume_location{}, run_value{}, run_witness{}, run_callback{};
    runner = std::thread([&] {
        try {
            Runtime runtime{&space};
            runtime.SetLocation(source_pc);
            miss_reason = runtime.Run();
            resume_location = runtime.GetLocation();
            run_reason = runtime.Run();
            std::memcpy(&run_value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&run_witness, runtime.GetUniformBuffer().data() + 16, 8);
            std::memcpy(&run_callback, runtime.GetUniformBuffer().data() + 24, 8);
        } catch (...) { runner_error = std::current_exception(); }
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    Check(wait_until(linked_executor_inside), "restored linked executor reaches its blocking callback");
    Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
          lifetime->Revoked() && watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
          "retire a restored linked source while its real executor is inside the allocation");
    Check(links.QueryTarget(target_pc) && space.GetCodeCacheTable().Lookup(target_pc) == target_entry,
          "restored linked executor retirement preserves the independent target");
    linked_executor_release = true;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge &&
          watched_code_frees == 0 && Mapped(lifetime->base) && Mapped(watched_code_free.load()),
          "restored linked executor purge waits for the signal reader after execution");
    reader.reset();
    runner.join();
    if (runner_error) std::rethrow_exception(runner_error);
    FaultEntry absent;
    Check(miss_reason == HaltReason::CodeMiss && resume_location == target_pc,
          "restored linked executor exits its retired allocation at the link edge with a precise resume location");
    Check(run_reason == HaltReason::CallHost && run_value == 37 && run_witness == 91 && run_callback == 84 &&
          purge_completed && watched_code_frees == 1 &&
          links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent),
          "restored linked executor completes through the target before one exact free");
    Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()),
          "repeated restored executor retirement cannot free twice");
    Runtime runtime{&space};
    runtime.SetLocation(target_pc);
    Check(runtime.Run() == HaltReason::CallHost, "independent target executes after restored linked executor retirement");
    u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
    Check(value == 37, "restored linked executor retirement preserves the target result");
    std::printf("linked_restored_executor backend=%s retired=1 code_miss=1 resumed=1 reader_wait=1 frees=1 entries=1 sites=1\n",
                aot_mode ? "aot" : "disk");
}

void LinkedCallExecutorRetire(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-call-executor-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-call-executor-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the Call executor disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.uniform_buffer_size = kReturningCallUniformSize;
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, return_pc = 2 * page + 0x180;
    const auto target_image = BlockingReturningTargetArtifact(config, guest.memory);
    const auto [image, captured] = RestoredCallFixture(
            config, guest.memory, target_image, target_pc, source_pc, return_pc);

    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the independent blocking returning target before restoring the Call source");
    if (aot_mode) {
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "restore the actual linked AOT Call source" : error.c_str());
    } else {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, index] : image.dispatch_slots) { payload.U64(key); payload.U32(index); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> filename{};
        std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the actual linked Call executor disk fixture");
        }
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the Call source");
        Check(unlink(filename.data()) == 0, "remove the owned linked Call executor disk fixture");
    }
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the exact restored Call source node");
    const auto lifetime = GetFunction(node)->code_lifetime;
    const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
    Check(lifetime && region && !lifetime->entries.empty(), "bind the published entries to one restored Call allocation");
    const auto entry_count = lifetime->entries.size();
    const LinkSourceOwner owner{module.get(), lifetime->base};
    const auto target_entry = space.GetCodeCacheTable().Lookup(target_pc);
    const auto prime = [&](Runtime& runtime) {
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &return_pc, 8);
    };
    linked_executor_inside = false;
    linked_executor_release = true;
    for (unsigned warm = 0; warm < 2; ++warm) {
        Runtime runtime{&space};
        prime(runtime);
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == HaltReason::CallHost, "warm the restored linked Call source");
        u64 value{}, witness{}, callback_result{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 24, 8);
        std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + kCallCallbackOffset, 8);
        Check(value == 37 && witness == 0x51ab && callback_result == 84,
              "restored linked Call warmup preserves target value, return witness and callback result");
        Check(runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty,
              "restored linked Call warmup leaves the return stack buffer empty");
    }
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 1 && sites.front().state == LinkSiteState::Linked,
          "restored linked Call source is actually linked before retirement");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runner;
    const auto finish = ScopeExit([&] {
        linked_executor_release = true;
        reader.reset();
        if (runner.joinable()) runner.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
    });
    watched_code_free = SiteRxToRw(*region, lifetime->base);
    watched_code_frees = 0;
    watched_purge_allocation = lifetime->base;
    purge_wait_observed = purge_completed = free_before_purge = false;
    linked_executor_inside = linked_executor_release = false;
    std::exception_ptr runner_error;
    HaltReason run_reason{}, retry_reason{};
    u64 run_value{}, run_witness{}, run_callback{}, run_location{}, run_flags{};
    u64 retry_location{}, retry_witness{}, retry_flags{};
    bool run_rsb_empty{}, retry_rsb_empty{};
    State* run_state{};
    runner = std::thread([&] {
        try {
            Runtime runtime{&space};
            run_state = runtime.GetState();
            prime(runtime);
            runtime.SetLocation(source_pc);
            run_reason = runtime.Run();
            run_location = runtime.GetLocation();
            std::memcpy(&run_value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&run_witness, runtime.GetUniformBuffer().data() + 24, 8);
            std::memcpy(&run_callback, runtime.GetUniformBuffer().data() + kCallCallbackOffset, 8);
            run_rsb_empty = runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty;
            run_flags = runtime.GetState()->host_cpu_flags & 0xf0000000u;
            // A new host entry must observe the revoked return alias, even
            // though the first execution retained its source through grace.
            retry_reason = runtime.Run();
            retry_location = runtime.GetLocation();
            std::memcpy(&retry_witness, runtime.GetUniformBuffer().data() + 24, 8);
            retry_flags = runtime.GetState()->host_cpu_flags & 0xf0000000u;
            retry_rsb_empty = runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty;
        } catch (...) { runner_error = std::current_exception(); }
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    Check(wait_until(linked_executor_inside), "restored linked Call executor reaches the blocking callee");
    Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
          lifetime->Revoked() && watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
          "retire a restored linked Call source while its executor is inside the callee");
    Check(links.QueryTarget(target_pc) && space.GetCodeCacheTable().Lookup(target_pc) == target_entry,
          "restored linked Call retirement preserves the independent callee");
    Check(run_state != nullptr && space.ExitLatchEnabled(),
          "the blocked callee has a live Runtime with region exit requests enabled");
    const auto request =
            std::atomic_ref<u64>(run_state->exit_request).load(std::memory_order_acquire);
    Check((request & kBackedgeSmcRequestMask) != 0 &&
                  (request & (kBackedgeSignalRequest | kRetirementExitRequest)) == 0,
          "source retirement publishes an SMC generation before the callee is released");
    Check(!space.GetCodeCacheTable().Lookup(return_pc) &&
                  !space.GetCallCodeCacheTable().Lookup(return_pc) &&
                  !space.GetPendingCallCodeCacheTable().Lookup(return_pc),
          "source retirement revokes the canonical and both Call return aliases");
    linked_executor_release = true;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge &&
          watched_code_frees == 0 && Mapped(lifetime->base) && Mapped(watched_code_free.load()),
          "restored linked Call purge waits for the signal reader after the return");
    reader.reset();
    runner.join();
    if (runner_error) std::rethrow_exception(runner_error);
    FaultEntry absent;
    // The explicit region configuration publishes an SMC request in both
    // flags modes. The callee's return poll consumes its frame and ejects at
    // the guest return before invoking a retained native continuation.
    CheckCallHalt(run_reason,
                  HaltReason::CodeMiss,
                  run_location,
                  "restored linked Call executor completes or ejects at the retired return");
    Check(run_location == return_pc,
          "restored linked Call ejection names the retired return location");
    Check(run_value == 37 && run_callback == 84, "restored linked Call executor preserves callee witnesses");
    Check(run_witness == 0,
          "the retired return body never executes through a retained continuation");
    CheckCallHalt(retry_reason,
                  HaltReason::CodeMiss,
                  retry_location,
                  "a second entry cannot reuse the retired return code");
    Check(retry_location == return_pc && retry_witness == 0 && retry_rsb_empty,
          "retry keeps the exact return PC, untouched return witness and empty frame stack");
    Check(run_flags == 0 && retry_flags == run_flags,
          "the SMC return and retry preserve the callee's committed NZCV");
    Check(run_rsb_empty, "restored linked Call executor balances the return stack buffer");
    Check(purge_completed && watched_code_frees == 1 &&
          links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent),
          "restored linked Call executor frees the retired source exactly once");
    Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()),
          "repeated restored Call retirement cannot free twice");
    Check(links.QueryTarget(target_pc) && space.GetCodeCacheTable().Lookup(target_pc) == target_entry &&
          IsFunction(module->GetNode(Location{target_pc})),
          "independent callee remains installed after restored linked Call retirement");
    std::printf(
            "linked_call_executor backend=%s retired=1 returned=0 code_miss=1 retry_miss=1 "
            "reader_wait=1 frees=1 entries=%zu sites=1\n",
            aot_mode ? "aot" : "disk",
            entry_count);
}

void LinkedCallTargetInvalidate(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-call-invalidate-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-call-invalidate-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the Call invalidate disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.uniform_buffer_size = kReturningCallUniformSize;
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, return_pc = 2 * page + 0x180;
    const auto target_image = BlockingReturningTargetArtifact(config, guest.memory);
    const auto [image, captured] = RestoredCallFixture(
            config, guest.memory, target_image, target_pc, source_pc, return_pc);

    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the independent blocking returning target before restoring the Call source");
    if (aot_mode) {
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "restore the actual linked AOT Call source" : error.c_str());
    } else {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, index] : image.dispatch_slots) { payload.U64(key); payload.U32(index); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> filename{};
        std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the actual linked Call invalidate disk fixture");
        }
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the Call source");
        Check(unlink(filename.data()) == 0, "remove the owned linked Call invalidate disk fixture");
    }
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the exact restored Call source node");
    const auto lifetime = GetFunction(node)->code_lifetime;
    const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
    Check(lifetime && region && !lifetime->entries.empty(), "bind the published entries to one restored Call allocation");
    const auto entry_count = lifetime->entries.size();
    const LinkSourceOwner owner{module.get(), lifetime->base};
    const auto target = links.QueryTarget(target_pc);
    Check(target && target->host_pc, "bind the blocking returning target before Call invalidation");
    const auto prime = [&](Runtime& runtime) {
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &return_pc, 8);
    };
    linked_executor_inside = false;
    linked_executor_release = true;
    for (unsigned warm = 0; warm < 2; ++warm) {
        Runtime runtime{&space};
        prime(runtime);
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == HaltReason::CallHost, "warm the restored linked Call source");
        u64 value{}, witness{}, callback_result{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 24, 8);
        std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + kCallCallbackOffset, 8);
        Check(value == 37 && witness == 0x51ab && callback_result == 84,
              "restored linked Call warmup preserves target value, return witness and callback result");
    }
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 1 && sites.front().state == LinkSiteState::Linked,
          "restored linked Call source is actually linked before invalidation");
    const auto site = sites.front();
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runner;
    const auto finish = ScopeExit([&] {
        linked_executor_release = true;
        reader.reset();
        if (runner.joinable()) runner.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
    });
    watched_code_free = SiteRxToRw(*region, lifetime->base);
    watched_code_frees = 0;
    watched_purge_allocation = lifetime->base;
    purge_wait_observed = purge_completed = free_before_purge = false;
    linked_executor_inside = linked_executor_release = false;
    std::exception_ptr runner_error;
    HaltReason run_reason{}, retry_reason{};
    u64 run_value{}, run_witness{}, run_callback{}, run_location{}, run_flags{};
    u64 retry_location{}, retry_witness{}, retry_flags{};
    bool run_rsb_empty{}, retry_rsb_empty{};
    State* run_state{};
    runner = std::thread([&] {
        try {
            Runtime runtime{&space};
            run_state = runtime.GetState();
            prime(runtime);
            runtime.SetLocation(source_pc);
            run_reason = runtime.Run();
            run_location = runtime.GetLocation();
            std::memcpy(&run_value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&run_witness, runtime.GetUniformBuffer().data() + 24, 8);
            std::memcpy(&run_callback, runtime.GetUniformBuffer().data() + kCallCallbackOffset, 8);
            run_rsb_empty = runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty;
            run_flags = runtime.GetState()->host_cpu_flags & 0xf0000000u;
            // A new host entry must observe the revoked return alias, even
            // though the first execution retained its source through grace.
            retry_reason = runtime.Run();
            retry_location = runtime.GetLocation();
            std::memcpy(&retry_witness, runtime.GetUniformBuffer().data() + 24, 8);
            retry_flags = runtime.GetState()->host_cpu_flags & 0xf0000000u;
            retry_rsb_empty = runtime.GetState()->rsb_pointer == runtime.GetState()->rsb_empty;
        } catch (...) { runner_error = std::current_exception(); }
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    Check(wait_until(linked_executor_inside), "restored linked Call executor reaches the blocking callee");
    const auto invalidated = links.SignalInvalidateTarget(target_pc);
    Check(invalidated.found && invalidated.linked_sites == 1 && watched_code_frees == 0 &&
          Mapped(lifetime->base) && Mapped(target->host_pc),
          "signal invalidation visits the linked Call site while the executor is inside the callee");
    {
        u32 instruction{}; std::memcpy(&instruction, region->rx_base + site.site.offset, sizeof(instruction));
        Check(instruction == site.unlinked_instruction, "signal invalidation restores the exact Call instruction under a live callee executor");
    }
    const auto unlinked = links.QuerySourceSites(owner);
    Check(unlinked.size() == 1 && unlinked.front().state == LinkSiteState::Linked &&
          unlinked.front().target_generation == sites.front().target_generation,
          "lock-free Call invalidation keeps the site record while the executor remains inside the callee");
    Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
          lifetime->Revoked() && watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
          "retire the invalidated restored Call source while its executor is inside the callee");
    Check(Mapped(target->host_pc) && IsFunction(module->GetNode(Location{target_pc})),
          "Call invalidation and retirement preserve the independent callee allocation");
    Check(run_state != nullptr && space.ExitLatchEnabled(),
          "the blocked callee has a live Runtime with region exit requests enabled");
    const auto request =
            std::atomic_ref<u64>(run_state->exit_request).load(std::memory_order_acquire);
    Check((request & kBackedgeSmcRequestMask) != 0 &&
                  (request & (kBackedgeSignalRequest | kRetirementExitRequest)) == 0,
          "source retirement publishes an SMC generation before the callee is released");
    Check(!space.GetCodeCacheTable().Lookup(return_pc) &&
                  !space.GetCallCodeCacheTable().Lookup(return_pc) &&
                  !space.GetPendingCallCodeCacheTable().Lookup(return_pc),
          "source retirement revokes the canonical and both Call return aliases");
    linked_executor_release = true;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge &&
          watched_code_frees == 0 && Mapped(lifetime->base) && Mapped(watched_code_free.load()),
          "invalidated Call source purge waits for the signal reader after the return");
    reader.reset();
    runner.join();
    if (runner_error) std::rethrow_exception(runner_error);
    FaultEntry absent;
    // The explicit region configuration publishes an SMC request in both
    // flags modes. The callee's return poll consumes its frame and ejects at
    // the guest return before invoking a retained native continuation.
    CheckCallHalt(run_reason,
                  HaltReason::CodeMiss,
                  run_location,
                  "invalidated Call executor completes or ejects at the retired return");
    Check(run_location == return_pc, "invalidated Call ejection names the retired return location");
    Check(run_value == 37 && run_callback == 84, "invalidated Call executor preserves callee witnesses");
    Check(run_witness == 0,
          "the invalidated source return body never executes through a retained continuation");
    CheckCallHalt(retry_reason,
                  HaltReason::CodeMiss,
                  retry_location,
                  "a second entry cannot reuse the retired return code");
    Check(retry_location == return_pc && retry_witness == 0 && retry_rsb_empty,
          "retry keeps the exact return PC, untouched return witness and empty frame stack");
    Check(run_flags == 0 && retry_flags == run_flags,
          "the SMC return and retry preserve the callee's committed NZCV");
    Check(run_rsb_empty, "invalidated Call executor balances the return stack buffer");
    Check(purge_completed && watched_code_frees == 1 &&
          links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent),
          "invalidated Call source frees exactly once after grace");
    Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()),
          "repeated invalidated Call retirement cannot free twice");
    std::printf(
            "linked_call_invalidate backend=%s invalidated=1 retired=1 returned=0 code_miss=1 "
            "retry_miss=1 reader_wait=1 frees=1 entries=%zu sites=1\n",
            aot_mode ? "aot" : "disk",
            entry_count);
}

void LinkedConcurrentDoubleRetire(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-double-retire-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-double-retire-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the double-retire disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, first_pc = 2 * page + 0x100, first_secondary = 3 * page + 0x100;
    const VAddr second_pc = 4 * page + 0x100, second_secondary = 5 * page + 0x100;
    const auto target_image = BlockingTargetArtifact(config, guest.memory);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, first_pc, first_secondary);
    const auto [second_image, second_captured] = RestoredSourceFixture(
            config, guest.memory, target_image, target_pc, second_pc, second_secondary, 92);
    (void)second_captured;
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report, second_report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the blocking target before restoring both sources");
    if (aot_mode) {
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "restore the first actual linked AOT source" : error.c_str());
    } else {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, index] : image.dispatch_slots) { payload.U64(key); payload.U32(index); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> filename{};
        std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the actual linked double-retire disk fixture");
        }
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the first source");
        Check(unlink(filename.data()) == 0, "remove the owned linked double-retire disk fixture");
    }
    const auto second_status = aot::InstallArtifact(space, second_image, guest.memory, config.guest_addr_mask, second_report, error);
    Check(second_status == aot::kAotOk && second_report.units_installed == 1,
          error.empty() ? "restore the second actual linked AOT source" : error.c_str());
    const auto first_node = module->GetNode(Location{first_pc});
    const auto second_node = module->GetNode(Location{second_pc});
    Check(IsFunction(first_node) && IsFunction(second_node), "retain both restored source nodes");
    const auto first_lifetime = GetFunction(first_node)->code_lifetime;
    const auto second_lifetime = GetFunction(second_node)->code_lifetime;
    const auto first_region = module->GetCodeRegion(first_lifetime ? first_lifetime->base : nullptr);
    Check(first_lifetime && second_lifetime && first_region && first_lifetime->base != second_lifetime->base &&
          first_lifetime->entries.size() == 2 && second_lifetime->entries.size() == 2,
          "bind both restored sources to distinct lifetime-backed allocations");
    const LinkSourceOwner first_owner{module.get(), first_lifetime->base};
    const LinkSourceOwner second_owner{module.get(), second_lifetime->base};
    const auto target_entry = space.GetCodeCacheTable().Lookup(target_pc);
    dual_executor_inside = 0;
    dual_executor_released = 1'000'000;
    for (const auto [pc, expected_witness] : {std::pair{first_pc, u64{91}}, {first_secondary, u64{91}},
                                              {second_pc, u64{92}}, {second_secondary, u64{92}}}) {
        Runtime runtime{&space};
        for (unsigned warm = 0; warm < 2; ++warm) {
            runtime.SetLocation(pc);
            Check(runtime.Run() == HaltReason::CallHost, "warm each restored source through the blocking target");
            u64 value{}, witness{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
            Check(value == 37 && witness == expected_witness, "each source body and the shared target execute");
        }
    }
    for (const auto& owner : {first_owner, second_owner}) {
        const auto sites = links.QuerySourceSites(owner);
        Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
            return site.state == LinkSiteState::Linked;
        }), "both restored sources are actually linked before concurrent retirement");
    }
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runner, retire_first, retire_second;
    const auto finish = ScopeExit([&] {
        dual_executor_released = 1'000'000;
        reader.reset();
        if (runner.joinable()) runner.join();
        if (retire_first.joinable()) retire_first.join();
        if (retire_second.joinable()) retire_second.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
        watch_multi_purge = false;
        multi_lifetime_count = 0;
    });
    watched_code_free = SiteRxToRw(*first_region, first_lifetime->base);
    watched_code_frees = 0;
    watched_purge_allocation = first_lifetime->base;
    multi_lifetimes[0] = first_lifetime;
    multi_lifetimes[1] = second_lifetime;
    multi_lifetime_count = 2;
    watch_multi_purge = true;
    purge_wait_observed = purge_completed = free_before_purge = false;
    dual_executor_inside = 0;
    dual_executor_released = 0;
    std::exception_ptr runner_error, first_error, second_error;
    HaltReason run_reason{};
    u64 run_value{}, run_witness{}, run_callback{};
    runner = std::thread([&] {
        try {
            Runtime runtime{&space};
            runtime.SetLocation(first_pc);
            run_reason = runtime.Run();
            std::memcpy(&run_value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&run_witness, runtime.GetUniformBuffer().data() + 16, 8);
            std::memcpy(&run_callback, runtime.GetUniformBuffer().data() + 24, 8);
        } catch (...) { runner_error = std::current_exception(); }
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    const auto wait_count = [](const std::atomic<unsigned>& counter, unsigned expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (counter.load() < expected && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return counter.load() >= expected;
    };
    Check(wait_count(dual_executor_inside, 1), "the real executor reaches the blocking target through the first source");
    std::atomic<bool> retire_go{};
    std::atomic<unsigned> retire_done{};
    bool first_retired{}, second_retired{};
    retire_first = std::thread([&] {
        try {
            while (!retire_go) std::this_thread::yield();
            first_retired = smc.RetireNode(space, &l1, module, GetFunction(first_node).get());
        } catch (...) { first_error = std::current_exception(); }
        ++retire_done;
    });
    retire_second = std::thread([&] {
        try {
            while (!retire_go) std::this_thread::yield();
            second_retired = smc.RetireNode(space, &l1, module, GetFunction(second_node).get());
        } catch (...) { second_error = std::current_exception(); }
        ++retire_done;
    });
    retire_go = true;
    Check(wait_count(retire_done, 2), "both concurrent retirements finish");
    if (first_error) std::rethrow_exception(first_error);
    if (second_error) std::rethrow_exception(second_error);
    Check(first_retired && second_retired && first_lifetime->Revoked() && second_lifetime->Revoked() &&
          watched_code_frees == 0 && !space.GetCodeCacheTable().Lookup(first_pc) &&
          !space.GetCodeCacheTable().Lookup(second_pc) &&
          IsEmpty(module->GetNode(Location{first_pc})) && IsEmpty(module->GetNode(Location{second_pc})),
          "concurrent retirement revokes both restored sources under the live executor");
    Check(links.QueryTarget(target_pc) && space.GetCodeCacheTable().Lookup(target_pc) == target_entry,
          "concurrent double retirement preserves the shared target");
    dual_executor_released = 1;
    Check(wait_until(purge_wait_observed), "first source purge reaches its grace wait after concurrent retirement");
    Check(!purge_completed && !free_before_purge &&
          watched_code_frees == 0 && Mapped(first_lifetime->base) && Mapped(watched_code_free.load()),
          "first source purge waits for the signal reader after concurrent retirement");
    reader.reset();
    runner.join();
    retire_first.join();
    retire_second.join();
    if (runner_error) std::rethrow_exception(runner_error);
    FaultEntry absent;
    Check(run_reason == HaltReason::CallHost && run_value == 37 && run_witness == 91 && run_callback == 84,
          "executor inside the shared target completes during concurrent double retirement");
    Check(purge_completed && watched_code_frees == 1 &&
          links.QuerySourceSites(first_owner).empty() && links.QuerySourceSites(second_owner).empty() &&
          !module->LookupCodeAllocation(first_lifetime->base, absent) &&
          !module->LookupCodeAllocation(second_lifetime->base, absent),
          "concurrent double retirement frees both allocations exactly once after grace");
    Check(!smc.RetireNode(space, &l1, module, GetFunction(first_node).get()) &&
          !smc.RetireNode(space, &l1, module, GetFunction(second_node).get()),
          "repeated concurrent retirement of either source cannot free twice");
    std::printf("linked_double_retire backend=%s retired=2 executor_completed=1 reader_wait=1 frees=1 entries=2 sites=4\n",
                aot_mode ? "aot" : "disk");
}

void LinkedRestoredDualExecutorRetire(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-restored-dual-executor-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-restored-dual-executor-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the restored dual-executor disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100;
    const auto target_image = Artifact(config, guest.memory, true);
    auto image = target_image;
    SerialUnit captured;
    {
        AddressSpace source{config};
        source.SetSerializableCodeEmission();
        const auto module = source.GetDefaultModule();
        HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
        auto* function = builder.AppendFunction(Location{source_pc}, Location{source_pc + 4});
        const auto callback = static_cast<u64>(reinterpret_cast<uintptr_t>(
                FptrCast(static_cast<u64 (*)()>(&DualExecutorCallback))));
        const auto result = function->CallLambda<TypedValue<ValueType::U64>>(Lambda{Imm{callback}});
        function->StoreUniform(Uniform{24, ValueType::U64}, result);
        function->StoreUniform(Uniform{16, ValueType::U64},
                               function->LoadImm(ValueType::U64, Imm{u64{91}}));
        function->EndBlock(terminal::LinkBlock{Location{target_pc}});
        function->EndFunction();
        auto* entry = static_cast<u8*>(TranslateIR(module, function));
        FaultEntry extent;
        Check(entry && module->LookupCodeAllocation(entry, extent), "compile the restored dual-executor source allocation");
        std::string error;
        Check(CaptureSerialCode(*module, extent.host_start, u32(extent.host_end - extent.host_start),
              GetHostImage(), config.loc_end, captured, &error), error.c_str());
        const auto target = source.GetLinkManager().QueryTarget(source_pc);
        Check(target && captured.link_sites.size() == 1, "capture the restored dual-executor branch and complete source entry");
        const auto offset = [&](const void* pointer) -> u32 {
            if (!pointer) return UINT32_MAX;
            const auto* address = static_cast<const u8*>(pointer);
            Check(address >= extent.host_start && address < extent.host_end, "captured restored dual-executor entry belongs to its allocation");
            return u32(address - extent.host_start);
        };
        SerialBlock block{source_pc, source_pc + 4, offset(target->host_pc),
            HashBytes(static_cast<u8*>(guest.memory) + source_pc, 4, 0xCBF29CE484222325ull),
            offset(target->direct_host_pc), offset(target->pending_flags_host_pc), target->pending_flags_contract};
        block.continuation_code_offset = offset(target->call_host_pc);
        block.pending_flags_continuation_code_offset = offset(target->call_pending_flags_host_pc);
        captured.blocks = {block};
        source.GetCodeCacheTable().ForEachEntry([&](u32 index, size_t key, size_t) {
            image.dispatch_slots.emplace_back(u64(key), index);
        });
    }
    captured.guest_start = source_pc; captured.is_function = 1;
    captured.fault_sites.push_back({source_pc, 0, u32(captured.code.size())});
    aot::AotUnit unit{};
    unit.guest_start = source_pc; unit.code_size = captured.code.size(); unit.is_function = 1;
    unit.blocks = captured.blocks; unit.relocs = captured.relocs; unit.fault_sites = captured.fault_sites;
    unit.link_sites = captured.link_sites; unit.region_branches = captured.region_branches;
    image.units = {unit}; image.code = captured.code;
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);

    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the independent target before restoring the dual-executor source");
    if (aot_mode) {
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "restore the actual linked AOT dual-executor source" : error.c_str());
    } else {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, index] : image.dispatch_slots) { payload.U64(key); payload.U32(index); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> filename{};
        std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the actual linked dual-executor disk fixture");
        }
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the dual-executor source");
        Check(unlink(filename.data()) == 0, "remove the owned linked dual-executor disk fixture");
    }
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the exact restored dual-executor source node");
    const auto lifetime = GetFunction(node)->code_lifetime;
    const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
    Check(lifetime && region && lifetime->entries.size() == 1, "bind the published entry to one restored dual-executor allocation");
    const LinkSourceOwner owner{module.get(), lifetime->base};
    const auto target_entry = space.GetCodeCacheTable().Lookup(target_pc);
    dual_executor_inside = 0;
    dual_executor_released = 1'000'000;
    for (unsigned warm = 0; warm < 2; ++warm) {
        Runtime runtime{&space};
        runtime.SetLocation(source_pc);
        Check(runtime.Run() == HaltReason::CallHost, "warm the restored linked dual-executor source");
        u64 value{}, witness{}, callback_result{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
        std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + 24, 8);
        Check(value == 37 && witness == 91 && callback_result == 84,
              "restored dual-executor warmup preserves callback, source and target witnesses");
    }
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 1 && sites.front().state == LinkSiteState::Linked,
          "restored linked dual-executor source is actually linked before retirement");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runners[2];
    const auto finish = ScopeExit([&] {
        dual_executor_released = 1'000'000;
        reader.reset();
        for (auto& runner : runners) if (runner.joinable()) runner.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
    });
    watched_code_free = SiteRxToRw(*region, lifetime->base);
    watched_code_frees = 0;
    watched_purge_allocation = lifetime->base;
    purge_wait_observed = purge_completed = free_before_purge = false;
    dual_executor_inside = 0;
    dual_executor_released = 0;
    std::atomic<unsigned> runners_done{};
    std::exception_ptr runner_errors[2];
    HaltReason miss_reasons[2]{}, run_reasons[2]{};
    u64 resume_locations[2]{}, run_values[2]{}, run_witnesses[2]{}, run_callbacks[2]{};
    for (unsigned i = 0; i < 2; ++i) {
        runners[i] = std::thread([&, i] {
            try {
                Runtime runtime{&space};
                runtime.SetLocation(source_pc);
                miss_reasons[i] = runtime.Run();
                resume_locations[i] = runtime.GetLocation();
                run_reasons[i] = runtime.Run();
                std::memcpy(&run_values[i], runtime.GetUniformBuffer().data(), 8);
                std::memcpy(&run_witnesses[i], runtime.GetUniformBuffer().data() + 16, 8);
                std::memcpy(&run_callbacks[i], runtime.GetUniformBuffer().data() + 24, 8);
            } catch (...) { runner_errors[i] = std::current_exception(); }
            ++runners_done;
        });
    }
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    const auto wait_count = [](const std::atomic<unsigned>& counter, unsigned expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (counter.load() < expected && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return counter.load() >= expected;
    };
    Check(wait_count(dual_executor_inside, 2), "both restored dual executors reach their blocking callbacks");
    Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
          lifetime->Revoked() && watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
          "retire a restored linked source while both real executors are inside the allocation");
    Check(links.QueryTarget(target_pc) && space.GetCodeCacheTable().Lookup(target_pc) == target_entry,
          "restored dual-executor retirement preserves the independent target");
    dual_executor_released = 1;
    Check(wait_count(runners_done, 1) && watched_code_frees == 0 && !purge_completed &&
          Mapped(lifetime->base) && Mapped(watched_code_free.load()),
          "one finished dual executor cannot free the allocation while the second remains inside");
    dual_executor_released = 2;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge &&
          watched_code_frees == 0 && Mapped(lifetime->base) && Mapped(watched_code_free.load()),
          "restored dual-executor purge waits for the signal reader after both executions");
    reader.reset();
    for (auto& runner : runners) runner.join();
    for (const auto& runner_error : runner_errors) if (runner_error) std::rethrow_exception(runner_error);
    FaultEntry absent;
    Check(miss_reasons[0] == HaltReason::CodeMiss && miss_reasons[1] == HaltReason::CodeMiss &&
          resume_locations[0] == target_pc && resume_locations[1] == target_pc,
          "both restored dual executors exit the retired allocation at the link edge with precise resume locations");
    Check(run_reasons[0] == HaltReason::CallHost && run_reasons[1] == HaltReason::CallHost &&
          run_values[0] == 37 && run_values[1] == 37 && run_witnesses[0] == 91 && run_witnesses[1] == 91 &&
          run_callbacks[0] == 84 && run_callbacks[1] == 84 &&
          purge_completed && watched_code_frees == 1 &&
          links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent),
          "both restored dual executors complete through the target before one exact free");
    Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()),
          "repeated restored dual-executor retirement cannot free twice");
    Runtime runtime{&space};
    runtime.SetLocation(target_pc);
    Check(runtime.Run() == HaltReason::CallHost, "independent target executes after restored dual-executor retirement");
    u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
    Check(value == 37, "restored dual-executor retirement preserves the target result");
    std::printf("linked_restored_dual_executor backend=%s retired=1 executors=2 code_miss=2 resumed=2 reader_wait=1 frees=1 entries=1 sites=1\n",
                aot_mode ? "aot" : "disk");
}

void LinkedTargetExecutorInvalidate(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-target-executor-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-target-executor-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the target-executor disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const auto target_image = BlockingTargetArtifact(config, guest.memory);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the blocking target before restoring the two-entry source");
    if (aot_mode) {
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "restore the actual linked two-entry AOT source against the blocking target" : error.c_str());
    } else {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, index] : image.dispatch_slots) { payload.U64(key); payload.U32(index); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> filename{};
        std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the actual linked target-executor disk fixture");
        }
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the target-executor source");
        Check(unlink(filename.data()) == 0, "remove the owned linked target-executor disk fixture");
    }
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the exact restored source node against the blocking target");
    const auto lifetime = GetFunction(node)->code_lifetime;
    const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
    Check(lifetime && region && lifetime->entries.size() == 2, "bind both published entries to one restored allocation against the blocking target");
    const LinkSourceOwner owner{module.get(), lifetime->base};
    const auto target = links.QueryTarget(target_pc);
    Check(target && target->host_pc, "bind the blocking target before executor invalidation");
    dual_executor_inside = 0;
    dual_executor_released = 1'000'000;
    for (const auto pc : {source_pc, secondary_pc}) {
        Runtime runtime{&space};
        for (unsigned warm = 0; warm < 2; ++warm) {
            runtime.SetLocation(pc);
            Check(runtime.Run() == HaltReason::CallHost, "execute each restored source entry through the blocking target");
            u64 value{}, witness{}, callback_result{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
            std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + 24, 8);
            Check(value == 37 && witness == 91 && callback_result == 84, "both source bodies and the blocking target execute");
        }
    }
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
        return site.state == LinkSiteState::Linked;
    }), "both restored physical branches become actually linked to the blocking target");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runner;
    const auto finish = ScopeExit([&] {
        dual_executor_released = 1'000'000;
        reader.reset();
        if (runner.joinable()) runner.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
    });
    watched_code_free = SiteRxToRw(*region, lifetime->base);
    watched_code_frees = 0;
    watched_purge_allocation = lifetime->base;
    purge_wait_observed = purge_completed = free_before_purge = false;
    dual_executor_inside = 0;
    dual_executor_released = 0;
    std::exception_ptr runner_error;
    HaltReason run_reason{};
    u64 run_value{}, run_witness{}, run_callback{};
    runner = std::thread([&] {
        try {
            Runtime runtime{&space};
            runtime.SetLocation(source_pc);
            run_reason = runtime.Run();
            std::memcpy(&run_value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&run_witness, runtime.GetUniformBuffer().data() + 16, 8);
            std::memcpy(&run_callback, runtime.GetUniformBuffer().data() + 24, 8);
        } catch (...) { runner_error = std::current_exception(); }
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    const auto wait_count = [](const std::atomic<unsigned>& counter, unsigned expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (counter.load() < expected && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return counter.load() >= expected;
    };
    Check(wait_count(dual_executor_inside, 1), "the real executor reaches the blocking target callback through the linked branch");
    const auto invalidated = links.SignalInvalidateTarget(target_pc);
    Check(invalidated.found && invalidated.linked_sites == 2 && watched_code_frees == 0 &&
          Mapped(lifetime->base) && Mapped(target->host_pc),
          "signal invalidation visits both linked branches while the executor is inside the target");
    for (const auto& site : sites) {
        u32 instruction{}; std::memcpy(&instruction, region->rx_base + site.site.offset, sizeof(instruction));
        Check(instruction == site.unlinked_instruction, "signal invalidation restores the exact branch instruction under a live target executor");
    }
    const auto unlinked = links.QuerySourceSites(owner);
    Check(unlinked.size() == 2 && std::all_of(unlinked.begin(), unlinked.end(), [&](const auto& site) {
        return site.state == LinkSiteState::Linked && site.target_generation == sites.front().target_generation;
    }), "lock-free signal invalidation keeps both site records while the executor remains inside the target");
    Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
          lifetime->Revoked() && watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
          "retire the unlinked restored source while the executor remains inside the target");
    dual_executor_released = 1;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge &&
          watched_code_frees == 0 && Mapped(lifetime->base) && Mapped(watched_code_free.load()),
          "target-executor source purge waits for the signal reader");
    reader.reset();
    runner.join();
    if (runner_error) std::rethrow_exception(runner_error);
    Check(run_reason == HaltReason::CallHost && run_value == 37 && run_witness == 91 && run_callback == 84,
          "the executor inside the invalidated target completes with source, callback and target witnesses");
    FaultEntry absent;
    Check(purge_completed && !free_before_purge && watched_code_frees == 1 &&
          links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent),
          "signal-reader exit purges both unlinked sources before one exact allocation free");
    Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()), "repeated target-executor source retirement cannot free twice");
    std::printf("linked_target_executor backend=%s invalidated=2 executor_completed=1 reader_wait=1 frees=1 entries=2 sites=2\n",
                aot_mode ? "aot" : "disk");
}

void LinkedConcurrentInstall(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-concurrent-install-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-concurrent-install-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the concurrent-install disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const VAddr neighbor_pc = 4 * page + 0x100, neighbor_secondary_pc = 5 * page + 0x100;
    const auto target_image = BlockingTargetArtifact(config, guest.memory);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    const auto [neighbor_image, neighbor_captured] = RestoredSourceFixture(
            config, guest.memory, target_image, target_pc, neighbor_pc, neighbor_secondary_pc, 92);
    (void)neighbor_captured;
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the blocking target before restoring the executing source");
    if (aot_mode) {
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "restore the actual linked two-entry AOT source before concurrent install" : error.c_str());
    } else {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, index] : image.dispatch_slots) { payload.U64(key); payload.U32(index); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> filename{};
        std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the actual linked concurrent-install disk fixture");
        }
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the executing source");
        Check(unlink(filename.data()) == 0, "remove the owned linked concurrent-install disk fixture");
    }
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the exact restored source node before concurrent install");
    const auto lifetime = GetFunction(node)->code_lifetime;
    const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
    Check(lifetime && region && lifetime->entries.size() == 2, "bind both published entries to one restored allocation before concurrent install");
    const LinkSourceOwner owner{module.get(), lifetime->base};
    const auto run_entries = [&](VAddr first, VAddr second, u64 witness) {
        for (const auto pc : {first, second}) {
            Runtime runtime{&space};
            for (unsigned warm = 0; warm < 2; ++warm) {
                runtime.SetLocation(pc);
                Check(runtime.Run() == HaltReason::CallHost, "execute each restored entry through the blocking target");
                u64 value{}, seen{}, callback_result{};
                std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
                std::memcpy(&seen, runtime.GetUniformBuffer().data() + 16, 8);
                std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + 24, 8);
                Check(value == 37 && seen == witness && callback_result == 84, "each restored body and the blocking target execute");
            }
        }
    };
    dual_executor_inside = 0;
    dual_executor_released = 1'000'000;
    run_entries(source_pc, secondary_pc, 91);
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
        return site.state == LinkSiteState::Linked;
    }), "both restored physical branches become actually linked before concurrent install");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runner, installer;
    const auto finish = ScopeExit([&] {
        dual_executor_released = 1'000'000;
        reader.reset();
        if (installer.joinable()) installer.join();
        if (runner.joinable()) runner.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
    });
    watched_code_free = SiteRxToRw(*region, lifetime->base);
    watched_code_frees = 0;
    watched_purge_allocation = lifetime->base;
    purge_wait_observed = purge_completed = free_before_purge = false;
    dual_executor_inside = 0;
    dual_executor_released = 0;
    std::atomic<bool> runner_done{};
    std::exception_ptr runner_error, installer_error;
    HaltReason run_reason{};
    u64 run_value{}, run_witness{}, run_callback{};
    runner = std::thread([&] {
        try {
            Runtime runtime{&space};
            runtime.SetLocation(source_pc);
            run_reason = runtime.Run();
            std::memcpy(&run_value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&run_witness, runtime.GetUniformBuffer().data() + 16, 8);
            std::memcpy(&run_callback, runtime.GetUniformBuffer().data() + 24, 8);
        } catch (...) { runner_error = std::current_exception(); }
        runner_done = true;
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    const auto wait_count = [](const std::atomic<unsigned>& counter, unsigned expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (counter.load() < expected && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return counter.load() >= expected;
    };
    Check(wait_count(dual_executor_inside, 1), "the real executor reaches the blocking target before concurrent install");
    aot::LoadReport neighbor_report;
    std::string neighbor_error;
    int neighbor_status{-1};
    installer = std::thread([&] {
        try {
            neighbor_status = aot::InstallArtifact(space, neighbor_image, guest.memory, config.guest_addr_mask, neighbor_report, neighbor_error);
        } catch (...) { installer_error = std::current_exception(); }
    });
    installer.join();
    if (installer_error) std::rethrow_exception(installer_error);
    Check(neighbor_status == aot::kAotOk && neighbor_report.units_installed == 1 && !runner_done &&
          watched_code_frees == 0 && Mapped(lifetime->base),
          neighbor_error.empty() ? "a disjoint linked unit installs while the executor remains inside the target" : neighbor_error.c_str());
    const auto neighbor_node = module->GetNode(Location{neighbor_pc});
    Check(IsFunction(neighbor_node), "retain the concurrently installed neighbor node");
    const auto neighbor_lifetime = GetFunction(neighbor_node)->code_lifetime;
    Check(neighbor_lifetime && neighbor_lifetime->entries.size() == 2 && neighbor_lifetime->base != lifetime->base,
          "the concurrently installed neighbor owns a disjoint two-entry allocation");
    const LinkSourceOwner neighbor_owner{module.get(), neighbor_lifetime->base};
    Check(links.QuerySourceSites(neighbor_owner).size() == 2, "the concurrently installed neighbor registers both outgoing sites");
    Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
          lifetime->Revoked() && watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
          "retire the executing source across the concurrent install while its executor remains inside the target");
    dual_executor_released = 1;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge &&
          watched_code_frees == 0 && Mapped(lifetime->base) && Mapped(watched_code_free.load()),
          "concurrent-install source purge waits for the signal reader");
    reader.reset();
    runner.join();
    if (runner_error) std::rethrow_exception(runner_error);
    Check(run_reason == HaltReason::CallHost && run_value == 37 && run_witness == 91 && run_callback == 84,
          "the executor completes through the target across the concurrent install");
    FaultEntry absent;
    Check(purge_completed && !free_before_purge && watched_code_frees == 1 &&
          links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent),
          "signal-reader exit purges the executing source before one exact allocation free");
    Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()), "repeated concurrent-install source retirement cannot free twice");
    dual_executor_released = 1'000'000;
    run_entries(neighbor_pc, neighbor_secondary_pc, 92);
    const auto neighbor_sites = links.QuerySourceSites(neighbor_owner);
    Check(neighbor_sites.size() == 2 && std::all_of(neighbor_sites.begin(), neighbor_sites.end(), [](const auto& site) {
        return site.state == LinkSiteState::Linked;
    }), "both concurrently installed neighbor branches become actually linked after the source purge");
    run_entries(neighbor_pc, neighbor_secondary_pc, 92);
    std::printf("linked_concurrent_install backend=%s installed_during_execution=1 executor_completed=1 neighbor_entries=2 neighbor_sites=2 reader_wait=1 frees=1\n",
                aot_mode ? "aot" : "disk");
}

void LinkedConcurrentInstallRetire(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-concurrent-install-retire-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-concurrent-install-retire-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the concurrent install-retire disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const VAddr neighbor_pc = 4 * page + 0x100, neighbor_secondary_pc = 5 * page + 0x100;
    const auto target_image = BlockingTargetArtifact(config, guest.memory);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    const auto [neighbor_image, neighbor_captured] = RestoredSourceFixture(
            config, guest.memory, target_image, target_pc, neighbor_pc, neighbor_secondary_pc, 92);
    (void)neighbor_captured;
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the blocking target before restoring the executing source");
    if (aot_mode) {
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "restore the actual linked two-entry AOT source before concurrent install-retire" : error.c_str());
    } else {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, index] : image.dispatch_slots) { payload.U64(key); payload.U32(index); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> filename{};
        std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the actual linked concurrent install-retire disk fixture");
        }
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the executing source");
        Check(unlink(filename.data()) == 0, "remove the owned linked concurrent install-retire disk fixture");
    }
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the exact restored source node before concurrent install-retire");
    const auto lifetime = GetFunction(node)->code_lifetime;
    const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
    Check(lifetime && region && lifetime->entries.size() == 2, "bind both published entries to one restored allocation before concurrent install-retire");
    const LinkSourceOwner owner{module.get(), lifetime->base};
    const auto run_entries = [&](VAddr first, VAddr second, u64 witness) {
        for (const auto pc : {first, second}) {
            Runtime runtime{&space};
            for (unsigned warm = 0; warm < 2; ++warm) {
                runtime.SetLocation(pc);
                Check(runtime.Run() == HaltReason::CallHost, "execute each restored entry through the blocking target");
                u64 value{}, seen{}, callback_result{};
                std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
                std::memcpy(&seen, runtime.GetUniformBuffer().data() + 16, 8);
                std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + 24, 8);
                Check(value == 37 && seen == witness && callback_result == 84, "each restored body and the blocking target execute");
            }
        }
    };
    dual_executor_inside = 0;
    dual_executor_released = 1'000'000;
    run_entries(source_pc, secondary_pc, 91);
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
        return site.state == LinkSiteState::Linked;
    }), "both restored physical branches become actually linked before concurrent install-retire");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runner, installer, retirer;
    const auto finish = ScopeExit([&] {
        dual_executor_released = 1'000'000;
        reader.reset();
        if (installer.joinable()) installer.join();
        if (retirer.joinable()) retirer.join();
        if (runner.joinable()) runner.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
    });
    watched_code_free = SiteRxToRw(*region, lifetime->base);
    watched_code_frees = 0;
    watched_purge_allocation = lifetime->base;
    purge_wait_observed = purge_completed = free_before_purge = false;
    dual_executor_inside = 0;
    dual_executor_released = 0;
    std::atomic<bool> runner_done{};
    std::exception_ptr runner_error, installer_error, retirer_error;
    HaltReason run_reason{};
    u64 run_value{}, run_witness{}, run_callback{};
    runner = std::thread([&] {
        try {
            Runtime runtime{&space};
            runtime.SetLocation(source_pc);
            run_reason = runtime.Run();
            std::memcpy(&run_value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&run_witness, runtime.GetUniformBuffer().data() + 16, 8);
            std::memcpy(&run_callback, runtime.GetUniformBuffer().data() + 24, 8);
        } catch (...) { runner_error = std::current_exception(); }
        runner_done = true;
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    const auto wait_count = [](const std::atomic<unsigned>& counter, unsigned expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (counter.load() < expected && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return counter.load() >= expected;
    };
    Check(wait_count(dual_executor_inside, 1), "the real executor reaches the blocking target before concurrent install-retire");
    std::atomic<bool> race_go{};
    std::atomic<unsigned> race_done{};
    aot::LoadReport neighbor_report;
    std::string neighbor_error;
    int neighbor_status{-1};
    bool source_retired{};
    installer = std::thread([&] {
        try {
            while (!race_go) std::this_thread::yield();
            neighbor_status = aot::InstallArtifact(space, neighbor_image, guest.memory, config.guest_addr_mask, neighbor_report, neighbor_error);
        } catch (...) { installer_error = std::current_exception(); }
        ++race_done;
    });
    retirer = std::thread([&] {
        try {
            while (!race_go) std::this_thread::yield();
            source_retired = smc.RetireNode(space, &l1, module, GetFunction(node).get());
        } catch (...) { retirer_error = std::current_exception(); }
        ++race_done;
    });
    race_go = true;
    Check(wait_count(race_done, 2), "concurrent install and retirement both finish under the live executor");
    if (installer_error) std::rethrow_exception(installer_error);
    if (retirer_error) std::rethrow_exception(retirer_error);
    Check(neighbor_status == aot::kAotOk && neighbor_report.units_installed == 1 && source_retired && !runner_done &&
          lifetime->Revoked() && watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
          neighbor_error.empty() ? "install and retirement race to completion while the executor remains inside the target" : neighbor_error.c_str());
    const auto neighbor_node = module->GetNode(Location{neighbor_pc});
    Check(IsFunction(neighbor_node), "retain the concurrently installed neighbor node");
    const auto neighbor_lifetime = GetFunction(neighbor_node)->code_lifetime;
    Check(neighbor_lifetime && neighbor_lifetime->entries.size() == 2 && neighbor_lifetime->base != lifetime->base,
          "the concurrently installed neighbor owns a disjoint two-entry allocation");
    const LinkSourceOwner neighbor_owner{module.get(), neighbor_lifetime->base};
    Check(links.QuerySourceSites(neighbor_owner).size() == 2, "the concurrently installed neighbor registers both outgoing sites");
    dual_executor_released = 1;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge &&
          watched_code_frees == 0 && Mapped(lifetime->base) && Mapped(watched_code_free.load()),
          "concurrent install-retire source purge waits for the signal reader");
    reader.reset();
    runner.join();
    installer.join();
    retirer.join();
    if (runner_error) std::rethrow_exception(runner_error);
    Check(run_reason == HaltReason::CallHost && run_value == 37 && run_witness == 91 && run_callback == 84,
          "the executor completes through the target across the install-retire race");
    FaultEntry absent;
    Check(purge_completed && !free_before_purge && watched_code_frees == 1 &&
          links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent),
          "signal-reader exit purges the raced source before one exact allocation free");
    Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()), "repeated raced source retirement cannot free twice");
    dual_executor_released = 1'000'000;
    run_entries(neighbor_pc, neighbor_secondary_pc, 92);
    const auto neighbor_sites = links.QuerySourceSites(neighbor_owner);
    Check(neighbor_sites.size() == 2 && std::all_of(neighbor_sites.begin(), neighbor_sites.end(), [](const auto& site) {
        return site.state == LinkSiteState::Linked;
    }), "both raced neighbor branches become actually linked after the source purge");
    std::printf("linked_install_retire backend=%s raced=1 executor_completed=1 neighbor_entries=2 neighbor_sites=2 reader_wait=1 frees=1\n",
                aot_mode ? "aot" : "disk");
}

void LinkedConcurrentInstallFailure(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-concurrent-install-failure-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-concurrent-install-failure-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the concurrent install-failure disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const VAddr neighbor_pc = 4 * page + 0x100, neighbor_secondary_pc = 5 * page + 0x100;
    const auto target_image = BlockingTargetArtifact(config, guest.memory);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    const auto [neighbor_image, neighbor_captured] = RestoredSourceFixture(
            config, guest.memory, target_image, target_pc, neighbor_pc, neighbor_secondary_pc, 92);
    (void)neighbor_captured;
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the blocking target before restoring the executing source");
    if (aot_mode) {
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "restore the actual linked two-entry AOT source before failed concurrent install" : error.c_str());
    } else {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, index] : image.dispatch_slots) { payload.U64(key); payload.U32(index); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> filename{};
        std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the actual linked failed-install disk fixture");
        }
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the executing source");
        Check(unlink(filename.data()) == 0, "remove the owned linked failed-install disk fixture");
    }
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the exact restored source node before failed concurrent install");
    const auto lifetime = GetFunction(node)->code_lifetime;
    const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
    Check(lifetime && region && lifetime->entries.size() == 2, "bind both published entries to one restored allocation before failed concurrent install");
    const LinkSourceOwner owner{module.get(), lifetime->base};
    const auto target_before = links.QueryTarget(target_pc);
    Check(target_before && target_before->host_pc, "bind the shared target before failed concurrent install");
    const auto run_entries = [&](VAddr first, VAddr second, u64 witness) {
        for (const auto pc : {first, second}) {
            Runtime runtime{&space};
            for (unsigned warm = 0; warm < 2; ++warm) {
                runtime.SetLocation(pc);
                Check(runtime.Run() == HaltReason::CallHost, "execute each restored entry through the blocking target");
                u64 value{}, seen{}, callback_result{};
                std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
                std::memcpy(&seen, runtime.GetUniformBuffer().data() + 16, 8);
                std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + 24, 8);
                Check(value == 37 && seen == witness && callback_result == 84, "each restored body and the blocking target execute");
            }
        }
    };
    dual_executor_inside = 0;
    dual_executor_released = 1'000'000;
    run_entries(source_pc, secondary_pc, 91);
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
        return site.state == LinkSiteState::Linked;
    }), "both restored physical branches become actually linked before failed concurrent install");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runner, installer, retirer;
    const auto finish = ScopeExit([&] {
        dual_executor_released = 1'000'000;
        reader.reset();
        if (installer.joinable()) installer.join();
        if (retirer.joinable()) retirer.join();
        if (runner.joinable()) runner.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
        watch_aot_allocation = fail_after_aot_push = fail_aot_protect = false;
        fail_aot_allocation = -1;
    });
    watched_code_free = SiteRxToRw(*region, lifetime->base);
    watched_code_frees = 0;
    watched_purge_allocation = lifetime->base;
    purge_wait_observed = purge_completed = free_before_purge = false;
    dual_executor_inside = 0;
    dual_executor_released = 0;
    std::atomic<bool> runner_done{};
    std::exception_ptr runner_error, retirer_error;
    HaltReason run_reason{};
    u64 run_value{}, run_witness{}, run_callback{};
    runner = std::thread([&] {
        try {
            Runtime runtime{&space};
            runtime.SetLocation(source_pc);
            run_reason = runtime.Run();
            std::memcpy(&run_value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&run_witness, runtime.GetUniformBuffer().data() + 16, 8);
            std::memcpy(&run_callback, runtime.GetUniformBuffer().data() + 24, 8);
        } catch (...) { runner_error = std::current_exception(); }
        runner_done = true;
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    const auto wait_count = [](const std::atomic<unsigned>& counter, unsigned expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (counter.load() < expected && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return counter.load() >= expected;
    };
    Check(wait_count(dual_executor_inside, 1), "the real executor reaches the blocking target before failed concurrent install");
    std::atomic<bool> race_go{};
    std::atomic<unsigned> race_done{};
    aot::LoadReport neighbor_report;
    std::string neighbor_error;
    int neighbor_status{-1};
    bool install_escaped{};
    bool install_failure_hit{};
    bool source_retired{};
    installer = std::thread([&] {
        while (!race_go) std::this_thread::yield();
        // Keep the memalign watch from reassigning the source's watched free
        // pointer to the neighbor allocation of the same size class.
        watched_aot_size = ~size_t{0};
        post_push_failure_index = 0;
        aot_failure_hit = aot_was_published = aot_published_at_failure = false;
        capture_published_lifetime = watch_aot_allocation = true;
        fail_after_aot_push = true;
        try {
            neighbor_status = aot::InstallArtifact(space, neighbor_image, guest.memory, config.guest_addr_mask, neighbor_report, neighbor_error);
        } catch (...) { install_escaped = true; }
        install_failure_hit = aot_failure_hit;
        watch_aot_allocation = capture_published_lifetime = fail_after_aot_push = false;
        fail_aot_allocation = -1;
        ++race_done;
    });
    retirer = std::thread([&] {
        try {
            while (!race_go) std::this_thread::yield();
            source_retired = smc.RetireNode(space, &l1, module, GetFunction(node).get());
        } catch (...) { retirer_error = std::current_exception(); }
        ++race_done;
    });
    race_go = true;
    Check(wait_count(race_done, 2), "failed install and retirement both finish under the live executor");
    if (retirer_error) std::rethrow_exception(retirer_error);
    Check(install_failure_hit, "the injected post-publication allocation failure is consumed");
    Check(install_escaped || neighbor_status != aot::kAotOk, "the raced install is rejected");
    Check(source_retired && !runner_done && lifetime->Revoked() && watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
          "retirement completes under the live executor across the failed install");
    Check(IsEmpty(module->GetNode(Location{neighbor_pc})) && !space.GetCodeCacheTable().Lookup(neighbor_pc) &&
          !space.GetCodeCacheTable().Lookup(neighbor_secondary_pc) &&
          links.QueryTarget(target_pc) && links.QueryTarget(target_pc)->generation == target_before->generation &&
          links.QueryTarget(target_pc)->host_pc == target_before->host_pc,
          "the failed raced install publishes no neighbor state and preserves the shared target");
    dual_executor_released = 1;
    // With two purges in flight the watched one may block on the manager
    // mutex instead of reaching its yield wait, so observe the deferral
    // itself: the allocation must stay mapped and unfreed while the
    // signal reader is held, across a bounded stability window.
    const auto deferral_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    bool deferred = true;
    while (std::chrono::steady_clock::now() < deferral_deadline) {
        deferred &= !purge_completed && !free_before_purge && watched_code_frees == 0 &&
                    Mapped(lifetime->base) && Mapped(watched_code_free.load());
        std::this_thread::yield();
    }
    Check(deferred, "failed-install source purge remains deferred while the signal reader is held");
    reader.reset();
    runner.join();
    installer.join();
    retirer.join();
    if (runner_error) std::rethrow_exception(runner_error);
    Check(run_reason == HaltReason::CallHost && run_value == 37 && run_witness == 91 && run_callback == 84,
          "the executor completes through the target across the failed install race");
    FaultEntry absent;
    // The deferred purge may need a scheduling quantum after the last join;
    // waiting for its completion witness is the same grace contract the
    // sibling oracles observe through the yield hook.
    Check(wait_count(watched_code_frees, 1), "failed-install source purge completes after reader exit");
    Check(purge_completed && !free_before_purge && watched_code_frees == 1 &&
          links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent),
          "signal-reader exit purges the raced source before one exact allocation free");
    Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()), "repeated raced source retirement cannot free twice");
    aot::LoadReport retry_report;
    std::string retry_error;
    const auto retry_status = aot::InstallArtifact(space, neighbor_image, guest.memory, config.guest_addr_mask, retry_report, retry_error);
    Check(retry_status == aot::kAotOk && retry_report.units_installed == 1,
          retry_error.empty() ? "the same neighbor installs cleanly on retry after the raced failure" : retry_error.c_str());
    const auto neighbor_node = module->GetNode(Location{neighbor_pc});
    Check(IsFunction(neighbor_node), "retain the retried neighbor node");
    const auto neighbor_lifetime = GetFunction(neighbor_node)->code_lifetime;
    Check(neighbor_lifetime && neighbor_lifetime->entries.size() == 2,
          "the retried neighbor owns a two-entry allocation");
    const LinkSourceOwner neighbor_owner{module.get(), neighbor_lifetime->base};
    dual_executor_released = 1'000'000;
    run_entries(neighbor_pc, neighbor_secondary_pc, 92);
    const auto neighbor_sites = links.QuerySourceSites(neighbor_owner);
    Check(neighbor_sites.size() == 2 && std::all_of(neighbor_sites.begin(), neighbor_sites.end(), [](const auto& site) {
        return site.state == LinkSiteState::Linked;
    }), "both retried neighbor branches become actually linked after the source purge");
    std::printf("linked_install_failure backend=%s raced=1 rejected=1 retried=1 executor_completed=1 neighbor_entries=2 neighbor_sites=2 reader_wait=1 frees=1\n",
                aot_mode ? "aot" : "disk");
}

void LinkedConcurrentCompile(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-concurrent-compile-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-concurrent-compile-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the concurrent-compile disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const VAddr fresh_pc = 4 * page + 0x100;
    const auto target_image = BlockingTargetArtifact(config, guest.memory);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the blocking target before restoring the executing source");
    if (aot_mode) {
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "restore the actual linked two-entry AOT source before concurrent compile" : error.c_str());
    } else {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, index] : image.dispatch_slots) { payload.U64(key); payload.U32(index); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> filename{};
        std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the actual linked concurrent-compile disk fixture");
        }
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the executing source");
        Check(unlink(filename.data()) == 0, "remove the owned linked concurrent-compile disk fixture");
    }
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the exact restored source node before concurrent compile");
    const auto lifetime = GetFunction(node)->code_lifetime;
    const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
    Check(lifetime && region && lifetime->entries.size() == 2, "bind both published entries to one restored allocation before concurrent compile");
    const LinkSourceOwner owner{module.get(), lifetime->base};
    const auto run_entries = [&](VAddr first, VAddr second, u64 witness) {
        for (const auto pc : {first, second}) {
            Runtime runtime{&space};
            for (unsigned warm = 0; warm < 2; ++warm) {
                runtime.SetLocation(pc);
                Check(runtime.Run() == HaltReason::CallHost, "execute each restored entry through the blocking target");
                u64 value{}, seen{}, callback_result{};
                std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
                std::memcpy(&seen, runtime.GetUniformBuffer().data() + 16, 8);
                std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + 24, 8);
                Check(value == 37 && seen == witness && callback_result == 84, "each restored body and the blocking target execute");
            }
        }
    };
    dual_executor_inside = 0;
    dual_executor_released = 1'000'000;
    run_entries(source_pc, secondary_pc, 91);
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
        return site.state == LinkSiteState::Linked;
    }), "both restored physical branches become actually linked before concurrent compile");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runner, compiler;
    const auto finish = ScopeExit([&] {
        dual_executor_released = 1'000'000;
        reader.reset();
        if (compiler.joinable()) compiler.join();
        if (runner.joinable()) runner.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
    });
    watched_code_free = SiteRxToRw(*region, lifetime->base);
    watched_code_frees = 0;
    watched_purge_allocation = lifetime->base;
    purge_wait_observed = purge_completed = free_before_purge = false;
    dual_executor_inside = 0;
    dual_executor_released = 0;
    std::atomic<bool> runner_done{};
    std::exception_ptr runner_error, compiler_error;
    HaltReason run_reason{};
    u64 run_value{}, run_witness{}, run_callback{};
    runner = std::thread([&] {
        try {
            Runtime runtime{&space};
            runtime.SetLocation(source_pc);
            run_reason = runtime.Run();
            std::memcpy(&run_value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&run_witness, runtime.GetUniformBuffer().data() + 16, 8);
            std::memcpy(&run_callback, runtime.GetUniformBuffer().data() + 24, 8);
        } catch (...) { runner_error = std::current_exception(); }
        runner_done = true;
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    const auto wait_count = [](const std::atomic<unsigned>& counter, unsigned expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (counter.load() < expected && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return counter.load() >= expected;
    };
    Check(wait_count(dual_executor_inside, 1), "the real executor reaches the blocking target before concurrent compile");
    u8* fresh_entry{};
    compiler = std::thread([&] {
        try {
            HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
            auto* function = builder.AppendFunction(Location{fresh_pc}, Location{fresh_pc + 4});
            function->StoreUniform(Uniform{16, ValueType::U64},
                                   function->LoadImm(ValueType::U64, Imm{u64{93}}));
            function->EndBlock(terminal::LinkBlock{Location{target_pc}});
            function->EndFunction();
            fresh_entry = static_cast<u8*>(TranslateIR(module, function));
        } catch (...) { compiler_error = std::current_exception(); }
    });
    compiler.join();
    if (compiler_error) std::rethrow_exception(compiler_error);
    FaultEntry fresh_allocation;
    Check(fresh_entry && module->LookupCodeAllocation(fresh_entry, fresh_allocation) && !runner_done &&
          watched_code_frees == 0 && Mapped(lifetime->base),
          "a fresh linked function compiles while the executor remains inside the target");
    const LinkSourceOwner fresh_owner{module.get(), fresh_allocation.host_start};
    Check(links.QuerySourceSites(fresh_owner).size() == 1, "the concurrently compiled function registers its outgoing site");
    Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()) &&
          lifetime->Revoked() && watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
          "retire the executing source across the concurrent compile while its executor remains inside the target");
    dual_executor_released = 1;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge &&
          watched_code_frees == 0 && Mapped(lifetime->base) && Mapped(watched_code_free.load()),
          "concurrent-compile source purge waits for the signal reader");
    reader.reset();
    runner.join();
    if (runner_error) std::rethrow_exception(runner_error);
    Check(run_reason == HaltReason::CallHost && run_value == 37 && run_witness == 91 && run_callback == 84,
          "the executor completes through the target across the concurrent compile");
    FaultEntry absent;
    Check(purge_completed && !free_before_purge && watched_code_frees == 1 &&
          links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent),
          "signal-reader exit purges the executing source before one exact allocation free");
    Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()), "repeated concurrent-compile source retirement cannot free twice");
    dual_executor_released = 1'000'000;
    for (unsigned warm = 0; warm < 2; ++warm) {
        Runtime runtime{&space};
        runtime.SetLocation(fresh_pc);
        Check(runtime.Run() == HaltReason::CallHost, "execute the concurrently compiled function through the blocking target");
        u64 value{}, seen{}, callback_result{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&seen, runtime.GetUniformBuffer().data() + 16, 8);
        std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + 24, 8);
        Check(value == 37 && seen == 93 && callback_result == 84, "the concurrently compiled body and the blocking target execute");
    }
    const auto fresh_sites = links.QuerySourceSites(fresh_owner);
    Check(fresh_sites.size() == 1 && fresh_sites.front().state == LinkSiteState::Linked,
          "the concurrently compiled branch becomes actually linked after the source purge");
    std::printf("linked_concurrent_compile backend=%s compiled_during_execution=1 executor_completed=1 fresh_linked=1 reader_wait=1 frees=1\n",
                aot_mode ? "aot" : "disk");
}

void LinkedConcurrentCompileRetire(bool aot_mode) {
#ifdef ANDROID
    char directory[] = "/data/local/tmp/swiftvm-concurrent-compile-retire-XXXXXX";
#else
    char directory[] = "/tmp/swiftvm-concurrent-compile-retire-XXXXXX";
#endif
    Check(mkdtemp(directory) && SetSvmConfigEnvForTest("SVM_JIT_CACHE", directory, 1) == 0,
          "isolate the concurrent compile-retire disk cache");
    const auto cleanup = ScopeExit([&] {
        auto* files = opendir(directory);
        if (files) {
            while (auto* entry = readdir(files)) if (entry->d_name[0] != '.')
                unlink((std::string(directory) + '/' + entry->d_name).c_str());
            closedir(files);
        }
        rmdir(directory);
    });
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const VAddr fresh_pc = 4 * page + 0x100;
    const auto target_image = BlockingTargetArtifact(config, guest.memory);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report, report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the blocking target before restoring the executing source");
    if (aot_mode) {
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "restore the actual linked two-entry AOT source before concurrent compile-retire" : error.c_str());
    } else {
        auto disk = captured;
        disk.feature_hash = HashFeatureSet(ResolveFeatureSet(module->GetModuleConfig()));
        if (config.backend_isa == kArm64) std::erase_if(disk.fault_sites, [](const auto& site) {
            return site.host_end != site.host_begin + sizeof(u32);
        });
        BlobWriter payload;
        payload.U64(image.dispatch_slots.size());
        for (const auto& [key, index] : image.dispatch_slots) { payload.U64(key); payload.U32(index); }
        payload.U64(1); WriteUnit(payload, disk);
        const ValidityKey key{kCacheFormatVersion, ComputeBuildId(),
            ComputeConfigHash(config, module->GetModuleConfig(), {}), ComputeEnvHash(), ComputeGuestId()};
        BlobWriter file;
        constexpr char magic[8] = {'S', 'V', 'M', 'J', 'I', 'T', 'C', '\1'};
        file.Bytes(magic, sizeof(magic));
        for (const auto value : {key.format_version, key.build_id, key.config_hash, key.env_hash, key.guest_id,
                                u64(payload.Size()), HashBytes(payload.Data().data(), payload.Size(), 0xCBF29CE484222325ull)}) file.U64(value);
        file.Bytes(payload.Data().data(), payload.Size());
        auto name = HashU64(key.build_id, key.guest_id);
        name = HashU64(key.config_hash, name); name = HashU64(key.env_hash, name);
        std::array<char, 512> filename{};
        std::snprintf(filename.data(), filename.size(), "%s/svmjit-%016llx.bin", directory, static_cast<unsigned long long>(name));
        {
            std::ofstream output(filename.data(), std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(file.Data().data()), file.Size());
            Check(output.good(), "write the actual linked concurrent compile-retire disk fixture");
        }
        space.LoadJitCache();
        Check(space.GetJitDiskCache()->Stats().units_loaded == 1, "the production disk loader revives the executing source");
        Check(unlink(filename.data()) == 0, "remove the owned linked concurrent compile-retire disk fixture");
    }
    const auto node = module->GetNode(Location{source_pc});
    Check(IsFunction(node), "retain the exact restored source node before concurrent compile-retire");
    const auto lifetime = GetFunction(node)->code_lifetime;
    const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
    Check(lifetime && region && lifetime->entries.size() == 2, "bind both published entries to one restored allocation before concurrent compile-retire");
    const LinkSourceOwner owner{module.get(), lifetime->base};
    const auto run_entries = [&](VAddr first, VAddr second, u64 witness) {
        for (const auto pc : {first, second}) {
            Runtime runtime{&space};
            for (unsigned warm = 0; warm < 2; ++warm) {
                runtime.SetLocation(pc);
                Check(runtime.Run() == HaltReason::CallHost, "execute each restored entry through the blocking target");
                u64 value{}, seen{}, callback_result{};
                std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
                std::memcpy(&seen, runtime.GetUniformBuffer().data() + 16, 8);
                std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + 24, 8);
                Check(value == 37 && seen == witness && callback_result == 84, "each restored body and the blocking target execute");
            }
        }
    };
    dual_executor_inside = 0;
    dual_executor_released = 1'000'000;
    run_entries(source_pc, secondary_pc, 91);
    const auto sites = links.QuerySourceSites(owner);
    Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
        return site.state == LinkSiteState::Linked;
    }), "both restored physical branches become actually linked before concurrent compile-retire");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    std::optional<LinkManager::SignalReader> reader{std::in_place, links};
    std::thread runner, compiler, retirer;
    const auto finish = ScopeExit([&] {
        dual_executor_released = 1'000'000;
        reader.reset();
        if (compiler.joinable()) compiler.join();
        if (retirer.joinable()) retirer.join();
        if (runner.joinable()) runner.join();
        watched_code_free = nullptr;
        watched_purge_allocation = nullptr;
    });
    watched_code_free = SiteRxToRw(*region, lifetime->base);
    watched_code_frees = 0;
    watched_purge_allocation = lifetime->base;
    purge_wait_observed = purge_completed = free_before_purge = false;
    dual_executor_inside = 0;
    dual_executor_released = 0;
    std::atomic<bool> runner_done{};
    std::exception_ptr runner_error, compiler_error, retirer_error;
    HaltReason run_reason{};
    u64 run_value{}, run_witness{}, run_callback{};
    runner = std::thread([&] {
        try {
            Runtime runtime{&space};
            runtime.SetLocation(source_pc);
            run_reason = runtime.Run();
            std::memcpy(&run_value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&run_witness, runtime.GetUniformBuffer().data() + 16, 8);
            std::memcpy(&run_callback, runtime.GetUniformBuffer().data() + 24, 8);
        } catch (...) { runner_error = std::current_exception(); }
        runner_done = true;
    });
    const auto wait_until = [](const std::atomic<bool>& flag) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!flag && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return flag.load();
    };
    const auto wait_count = [](const std::atomic<unsigned>& counter, unsigned expected) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (counter.load() < expected && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return counter.load() >= expected;
    };
    Check(wait_count(dual_executor_inside, 1), "the real executor reaches the blocking target before concurrent compile-retire");
    std::atomic<bool> race_go{};
    std::atomic<unsigned> race_done{};
    u8* fresh_entry{};
    bool source_retired{};
    compiler = std::thread([&] {
        try {
            while (!race_go) std::this_thread::yield();
            HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
            auto* function = builder.AppendFunction(Location{fresh_pc}, Location{fresh_pc + 4});
            function->StoreUniform(Uniform{16, ValueType::U64},
                                   function->LoadImm(ValueType::U64, Imm{u64{93}}));
            function->EndBlock(terminal::LinkBlock{Location{target_pc}});
            function->EndFunction();
            fresh_entry = static_cast<u8*>(TranslateIR(module, function));
        } catch (...) { compiler_error = std::current_exception(); }
        ++race_done;
    });
    retirer = std::thread([&] {
        try {
            while (!race_go) std::this_thread::yield();
            source_retired = smc.RetireNode(space, &l1, module, GetFunction(node).get());
        } catch (...) { retirer_error = std::current_exception(); }
        ++race_done;
    });
    race_go = true;
    Check(wait_count(race_done, 2), "concurrent compile and retirement both finish under the live executor");
    if (compiler_error) std::rethrow_exception(compiler_error);
    if (retirer_error) std::rethrow_exception(retirer_error);
    FaultEntry fresh_allocation;
    Check(fresh_entry && module->LookupCodeAllocation(fresh_entry, fresh_allocation) && source_retired && !runner_done &&
          lifetime->Revoked() && watched_code_frees == 0 &&
          !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
          "compile and retirement race to completion while the executor remains inside the target");
    const LinkSourceOwner fresh_owner{module.get(), fresh_allocation.host_start};
    Check(links.QuerySourceSites(fresh_owner).size() == 1, "the raced-in compiled function registers its outgoing site");
    dual_executor_released = 1;
    Check(wait_until(purge_wait_observed) && !purge_completed && !free_before_purge &&
          watched_code_frees == 0 && Mapped(lifetime->base) && Mapped(watched_code_free.load()),
          "concurrent compile-retire source purge waits for the signal reader");
    reader.reset();
    runner.join();
    compiler.join();
    retirer.join();
    if (runner_error) std::rethrow_exception(runner_error);
    Check(run_reason == HaltReason::CallHost && run_value == 37 && run_witness == 91 && run_callback == 84,
          "the executor completes through the target across the compile-retire race");
    FaultEntry absent;
    Check(purge_completed && !free_before_purge && watched_code_frees == 1 &&
          links.QuerySourceSites(owner).empty() && !module->LookupCodeAllocation(lifetime->base, absent),
          "signal-reader exit purges the raced source before one exact allocation free");
    Check(!smc.RetireNode(space, &l1, module, GetFunction(node).get()), "repeated raced source retirement cannot free twice");
    dual_executor_released = 1'000'000;
    for (unsigned warm = 0; warm < 2; ++warm) {
        Runtime runtime{&space};
        runtime.SetLocation(fresh_pc);
        Check(runtime.Run() == HaltReason::CallHost, "execute the raced-in compiled function through the blocking target");
        u64 value{}, seen{}, callback_result{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
        std::memcpy(&seen, runtime.GetUniformBuffer().data() + 16, 8);
        std::memcpy(&callback_result, runtime.GetUniformBuffer().data() + 24, 8);
        Check(value == 37 && seen == 93 && callback_result == 84, "the raced-in compiled body and the blocking target execute");
    }
    const auto fresh_sites = links.QuerySourceSites(fresh_owner);
    Check(fresh_sites.size() == 1 && fresh_sites.front().state == LinkSiteState::Linked,
          "the raced-in compiled branch becomes actually linked after the source purge");
    std::printf("linked_compile_retire backend=%s raced=1 executor_completed=1 fresh_linked=1 reader_wait=1 frees=1\n",
                aot_mode ? "aot" : "disk");
}

void DirtySmcSymbols() {
    GuestWindow guest;
    const auto config = TestConfig(guest.memory);
    const auto original = Artifact(config, guest.memory);
    auto replacement = original;
    replacement.units.front().guest_start = 0x200;
    for (auto& block : replacement.units.front().blocks) {
        block.guest_start += 0x100;
        block.guest_end += 0x100;
    }
    for (auto& site : replacement.units.front().fault_sites) site.guest_start += 0x100;
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    aot::LoadReport first, rejected, retry;
    std::string error;
    Check(aot::InstallArtifact(space, original, guest.memory, config.guest_addr_mask, first, error) == aot::kAotOk,
          "install the first unit on a protected guest page");
    TranslateTable l1{8};
    auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    SmcTracker::ExecutionScope active{smc, token};
    Check(smc.HandleWriteFault(space, l1, reinterpret_cast<uintptr_t>(guest.memory) + 0x100) &&
          !aot::ResolveCodeOffset(first.mapping, 0), "open dirty write window and revoke its old aliases");
    watched_code_free = nullptr;
    watched_aot_size = replacement.units.front().code_size;
    watch_aot_allocation = true;
    bool cancelled{};
    try { (void)aot::InstallArtifact(space, replacement, guest.memory, config.guest_addr_mask, rejected, error); }
    catch (const std::runtime_error&) { cancelled = true; }
    watch_aot_allocation = false;
    const auto reset = ScopeExit([] { watched_code_free = nullptr; });
    Check(cancelled && watched_code_free && watched_code_frees == 0 &&
          IsEmpty(module->GetNode(Location{0x200})) && !space.GetCodeCacheTable().Lookup(0x200),
          "a new unit cannot commit into an existing open write window");
    smc.CloseWriteWindow(space, l1);
    Check(watched_code_frees == 0, "cancelled visible allocation remains pinned through old executor grace");
    active.End();
    Check(watched_code_frees == 1 && IsEmpty(module->GetNode(Location{0x100})),
          "closing the window and exiting the executor releases the cancelled allocation once");
    Check(aot::InstallArtifact(space, replacement, guest.memory, config.guest_addr_mask, retry, error) == aot::kAotOk &&
          aot::ResolveCodeOffset(retry.mapping, 0) && !aot::ResolveCodeOffset(first.mapping, 0),
          "retry after window closure publishes a fresh generation without reviving the original");
}

void ChildSmcSymbols(std::string_view mode) {
    GuestWindow guest;
    const auto config = TestConfig(guest.memory);
    auto image = Artifact(config, guest.memory);
    auto& unit = image.units.front();
    const auto span = unit.code_size;
    const auto original_code = image.code;
    image.code.insert(image.code.end(), original_code.begin(), original_code.end());
    auto child_entry = unit.blocks.front();
    child_entry.guest_start += 0x100;
    child_entry.guest_end += 0x100;
    child_entry.code_offset += span;
    unit.blocks.push_back(child_entry);
    const auto original_relocs = unit.relocs;
    for (auto relocation : original_relocs) {
        relocation.code_offset += span;
        unit.relocs.push_back(relocation);
    }
    const auto original_faults = unit.fault_sites;
    for (auto fault : original_faults) {
        fault.guest_start += 0x100;
        fault.host_begin += span;
        fault.host_end += span;
        if (fault.recovery_offset != UINT32_MAX) fault.recovery_offset += span;
        unit.fault_sites.push_back(fault);
    }
    unit.code_size += span;
    unit.fault_sites.push_back({unit.guest_start, 0, unit.code_size, UINT32_MAX,
                               static_cast<u8>(FaultRecoveryKind::GuestFault)});
    Check(unit.ValidFaultSites(), "the two-entry fixture retains its complete allocation-owner fault range");
    image.code_hash = HashBytes(image.code.data(), image.code.size(), 0xCBF29CE484222325ull);
    AddressSpace space{config};
    auto module = space.GetDefaultModule();
    aot::LoadReport report;
    std::string error;
    Check(aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error) == aot::kAotOk,
          "install two actual published child entries in one function allocation");
    const auto node = module->GetNode(Location{0x100});
    Check(IsFunction(node), "retain the installed child-owning function");
    auto function = GetFunction(node);
    const std::array<ir::Block*, 2> children{
        function->FindBlock(Location{0x100}), function->FindBlock(Location{0x200})};
    Check(children[0] && children[1], "borrow child descriptors through the retained owning Function");
    if (config.backend_isa == kArm64) {
        // ARM does not publish child caches; reproduce the RV descriptor shape
        // here to exercise the shared detach path on the ARM test device.
        for (size_t i = 0; i < children.size(); ++i) {
            auto guard = children[i]->LockWrite();
            auto& cache = children[i]->GetJitCache();
            cache = function->GetJitCache();
            cache.offset_in = *cache.offset_in + i * span;
            cache.cache_size = *cache.cache_size - i * span;
        }
    }
    auto* const allocation = static_cast<u8*>(aot::ResolveCodeOffset(report.mapping, 0));
    const auto region = module->GetCodeRegion(allocation);
    Check(allocation && region && module->GetJitCache(children[0]->GetJitCache()) &&
          module->GetJitCache(children[1]->GetJitCache()), "both retained child descriptors resolve while live");
    for (const u64 pc : {u64{0x100}, u64{0x200}}) {
        Runtime runtime{&space};
        runtime.SetLocation(pc);
        (void)runtime.Run();
        u64 value{};
        std::memcpy(&value, runtime.GetUniformBuffer().data(), sizeof(value));
        Check(value == 37, "each actual published child entry executes its expected body");
    }
    TranslateTable l1{8};
    auto& smc = space.GetSmcTracker();
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    SmcTracker::ExecutionScope active{smc, token};
    watched_code_free = SiteRxToRw(*region, allocation);
    watched_code_frees = 0;
    const auto reset = ScopeExit([] { watched_code_free = nullptr; });
    if (mode == "symbols-child-node") {
        Check(smc.RetireNode(space, &l1, module, function.get()), "retire the child-owning function");
    } else if (mode == "symbols-child-range") {
        smc.InvalidateRange(space, &l1, 0x200, 0x204);
    } else {
        Check(smc.HandleWriteFault(space, l1, reinterpret_cast<uintptr_t>(guest.memory) + 0x200),
              "open the actual child guest-page write window");
        smc.CloseWriteWindow(space, l1);
    }
    Check(!module->GetJitCache(children[0]->GetJitCache()) && !module->GetJitCache(children[1]->GetJitCache()),
          "ordinary function retirement clears retained child cache descriptors");
    Check(children[0]->code_lifetime == function->code_lifetime &&
          children[1]->code_lifetime == function->code_lifetime && function->code_lifetime->Revoked() &&
          !aot::ResolveCodeOffset(report.mapping, 0) && !aot::ResolveCodeOffset(report.mapping, span) &&
          !space.GetCodeCacheTable().Lookup(0x100) && !space.GetCodeCacheTable().Lookup(0x200) &&
          IsEmpty(module->GetNode(Location{0x100})) && watched_code_frees == 0,
          "every child shares the revoked allocation identity and executor grace retains its storage");
    active.End();
    Check(watched_code_frees == 1 && !smc.RetireNode(space, &l1, module, function.get()),
          "one function allocation is freed once after all child descriptors become inert");
}

void InstructionPinLifetime(std::string_view mode) {
    const bool sweep = mode == "instruction-pin-failure";
    unsigned failures{};
    bool succeeded{};
    for (int failure = 0; failure < (sweep ? 8 : 1); ++failure) {
        AddressSpace space{TestConfig()};
        auto module = space.GetDefaultModule();
        const auto [cache_id, code] = module->AllocCodeCache(64);
        Check(cache_id != INVALID_CACHE_ID && code.exec_data && code.rw_data,
              "allocate a real independent code owner for the host pin lifecycle");
        // This descriptor exercises host ownership only. It is not published
        // or executed as a resumable instruction, and makes no semantic claim.
        ir::NodeRef ir_owner{new ir::Block(Location{0x500})};
        module->RetainCodeIR(code.exec_data, ir_owner.Get());
        auto descriptor = std::make_shared<InstructionResumeDescriptor>();
        descriptor->original = descriptor->emitted = static_cast<ir::Block*>(ir_owner.Get());
        module->RegisterInstructionResume(code.exec_data, descriptor);
        std::unique_ptr<InstructionCodePin> pin;
        bool rejected{};
        aot_failure_hit = false;
        aot_allocation_index = 0;
        fail_aot_allocation = sweep ? failure : -1;
        try { pin = module->PinInstructionCode(module, code.exec_data); }
        catch (const std::bad_alloc&) { rejected = true; }
        fail_aot_allocation = -1;
        const auto reset = ScopeExit([] { fail_aot_allocation = -1; watched_code_free = nullptr; });
        if (rejected) {
            ++failures;
            Check(aot_failure_hit && module->InspectInstructionPin(code.exec_data).pins == 0,
                  "consumed pin allocation failure never increments a phantom pin");
            const auto ticket = Terminal(space);
            Check(space.TryShutdown(ticket).Complete() && !Mapped(code.exec_data) && !Mapped(code.rw_data),
                  "failed pin construction leaves no terminal registration or retained mapping");
            continue;
        }
        Check(pin && (!sweep || !aot_failure_hit) && module->InspectInstructionPin(code.exec_data).pins == 1,
              "pin acquires its exact allocation and independent terminal owner");
        succeeded = true;
        watched_code_free = code.rw_data;
        watched_code_frees = 0;
        if (mode == "instruction-pin-reclaim") {
            // No generated caller has ever entered this allocation; the caller
            // supplies quiescence before requesting its ordinary reclamation.
            module->ReclaimCode(code.exec_data);
            const auto state = module->InspectInstructionPin(code.exec_data);
            Check(state.pins == 1 && state.revoked && state.reclaim_pending && watched_code_frees == 0,
                  "ordinary reclaim marks the live external pin pending without freeing it");
            pin.reset();
            Check(watched_code_frees == 1 && module->InspectInstructionPin(code.exec_data).pins == 0,
                  "last independent pin releases the pending allocation exactly once");
        } else {
            auto ordinary = RetirementControl::RequestDrain(space.GetRetirementControl());
            Check(space.GetRetirementControl()->CheckDrain(ordinary) == RetirementControl::DrainStatus::Ready &&
                  space.GetRetirementControl()->SealPublications(ordinary) &&
                  space.GetRetirementControl()->Resume(ordinary),
                  "an independent pin permits ordinary drain and resume generations");
            const auto ticket = Terminal(space);
            Check(space.GetRetirementControl()->CheckShutdown(ticket) == RetirementControl::DrainStatus::Pending &&
                  !space.TryShutdown(ticket).Complete() && Mapped(code.exec_data) && Mapped(code.rw_data) &&
                  watched_code_frees == 0,
                  "a pin without any Runtime prevents terminal physical release");
            Check(!module->PinInstructionCode(module, code.exec_data) &&
                  module->InspectInstructionPin(code.exec_data).pins == 1,
                  "sealed terminal admission rejects a second external pin without throwing");
            pin.reset();
            Check(space.GetRetirementControl()->CheckShutdown(ticket) == RetirementControl::DrainStatus::Ready &&
                  space.TryShutdown(ticket).Complete() && !Mapped(code.exec_data) && !Mapped(code.rw_data),
                  "terminal release completes after the independent pin leaves");
        }
        break;
    }
    Check(succeeded && (!sweep || failures >= 2), "pin allocation sweep reaches success after all actual failure points");
}

void Persistence() {
    // Process-level configuration may be cached before main. Install the
    // unique directory in the environment, then replace this same process so
    // cache enablement is part of startup, just as in a production invocation.
    if (!std::getenv("SVM_SHUTDOWN_SAVE_CHILD")) {
#ifdef ANDROID
        char directory[] = "/data/local/tmp/swiftvm-r104-save-XXXXXX";
#else
        char directory[] = "/tmp/swiftvm-r104-save-XXXXXX";
#endif
        Check(mkdtemp(directory) != nullptr && setenv("SVM_JIT_CACHE", directory, 1) == 0 &&
              setenv("SVM_SHUTDOWN_SAVE_CHILD", "1", 1) == 0,
              "enable real disk cache in a unique directory before process startup");
        char name[] = "swift_address_space_shutdown_test", mode[] = "save";
        char* arguments[] = {name, mode, nullptr};
        execv("/proc/self/exe", arguments);
        throw std::runtime_error("cannot restart persistence test with startup configuration");
    }
    const char* directory = std::getenv("SVM_JIT_CACHE");
    Check(directory != nullptr, "restarted persistence test retains its unique directory");
    {
        GuestWindow guest;
        AddressSpace space{TestConfig(guest.memory)};
        auto* disk = space.GetJitDiskCache();
        Check(disk && disk->Enabled(), "persistence test requires the actual enabled cache contract");
        space.SetSerializableCodeEmission();
        auto module = space.GetDefaultModule();
        HIRBuilder builder{1, true, ResolveFeatureSet(module->GetModuleConfig())};
        auto* function = builder.AppendFunction(Location{0x100}, Location{0x104});
        function->StoreUniform(Uniform{0, ValueType::U64},
                               function->LoadImm(ValueType::U64, Imm{u64{37}}));
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        auto* entry = TranslateIR(module, function);
        Check(entry && space.PushCodeCache(Location{0x100}, entry), "publish a real persistable production unit and slot");
        // RV CompileFunctions records its own unit. ARM's standalone test
        // entry needs the driver-side hook supplied before SaveAfterDrain.
        if (space.GetConfig().backend_isa == kArm64) {
            FaultEntry allocation;
            std::vector<FaultEntry> faults;
            Check(module->LookupCodeAllocation(static_cast<u8*>(entry), allocation, &faults),
                  "bind persistence input to the actual published allocation");
            const auto region = module->GetCodeRegion(allocation.host_start);
            Check(region.has_value(), "bind persistence RW alias to the original arena");
            std::vector<SerialFaultSite> sites;
            for (const auto& fault : faults) sites.push_back({fault.guest_loc,
                static_cast<u32>(fault.host_start - allocation.host_start),
                static_cast<u32>(fault.host_end - allocation.host_start),
                fault.recovery ? static_cast<u32>(fault.recovery - allocation.host_start) : UINT32_MAX,
                static_cast<u8>(fault.recovery_kind)});
            const std::vector<SerialBlock> blocks{{.guest_start = 0x100, .guest_end = 0x104,
                .code_offset = static_cast<u32>(static_cast<u8*>(entry) - allocation.host_start)}};
            disk->RecordUnit(module, 0x100, true, allocation.host_start, SiteRxToRw(*region, allocation.host_start),
                             static_cast<u32>(allocation.host_end - allocation.host_start), blocks, {}, sites);
        }
        Check(disk->Stats().units_compiled == 1 && disk->Stats().reject_scan == 0,
              "actual production scanner accepts the persisted unit before shutdown");
        auto alias = std::make_shared<CounterAlias>();
        Check(space.RegisterTerminalAlias(alias), "register persistence-order alias");
        std::optional<LinkManager::SignalReader> reader{std::in_place, space.GetLinkManager()};
        TranslateTable l1;
        u64 request{};
        auto token = space.GetSmcTracker().RegisterRuntime(l1, &request);
        const auto ticket = Terminal(space);
        const auto pending = space.TryShutdown(ticket);
        Check(pending.smc.runtimes_attached && disk->Stats().units_stored == 0 && rename_calls == 0 &&
              alias->calls == 0 && Mapped(entry), "held low-level token prevents actual disk publication and revocation");
        space.GetSmcTracker().UnregisterRuntime(token);
        bool failed{};
        deny_allocations = true;
        try { (void)space.TryShutdown(ticket); } catch (const std::bad_alloc&) { failed = true; }
        deny_allocations = false;
        Check(failed && alias->calls == 0 && rename_calls == 0 && Mapped(entry) &&
              space.GetCodeCacheTable().Lookup(0x100) == reinterpret_cast<size_t>(entry),
              "save OOM preserves callable publications and permits retry");
        bool sealed{};
        try { (void)space.GetSmcTracker().RegisterRuntime(l1, &request); }
        catch (const std::runtime_error&) { sealed = true; }
        Check(sealed, "successful preflight seals late registration even when saving throws");
        const auto saved = space.TryShutdown(ticket);
        Check(saved.stage == AddressSpace::ShutdownStage::LinkReaders && disk->Stats().units_stored == 1 &&
              rename_calls == 1 && alias->calls == 1 && Mapped(entry),
              "real disk save finishes before reader grace and retains live code");
        Check(space.TryShutdown(ticket).stage == AddressSpace::ShutdownStage::LinkReaders && rename_calls == 1,
              "reader-grace retry never saves cleared routing tables again");
        reader.reset();
        Check(space.TryShutdown(ticket).Complete() && !Mapped(entry) && rename_calls == 1,
              "final shutdown releases code after exactly one actual disk publication");
    }
    auto* files = opendir(directory);
    Check(files != nullptr, "inspect actual persisted cache file");
    size_t count{};
    while (auto* file = readdir(files)) {
        if (file->d_name[0] == '.') continue;
        ++count;
        const auto path = std::string(directory) + '/' + file->d_name;
        Check(unlink(path.c_str()) == 0, "remove only this test's persisted file");
    }
    closedir(files);
    Check(count == 1 && rmdir(directory) == 0, "exactly one cache file was published in the unique directory");
}

void BlockEmissionFallback() {
#if defined(__aarch64__)
    std::optional<std::string> old_profile, old_cache;
    if (const auto* value = GetRawSvmConfigEnvForTest("SVM_PROF2")) old_profile = value;
    if (const auto* value = GetRawSvmConfigEnvForTest("SVM_JIT_CACHE")) old_cache = value;
    const auto restore = ScopeExit([&] {
        block_emission_allocation_watch = nullptr;
        if (old_profile) SetSvmConfigEnvForTest("SVM_PROF2", old_profile->c_str(), 1);
        else UnsetSvmConfigEnvForTest("SVM_PROF2");
        if (old_cache) SetSvmConfigEnvForTest("SVM_JIT_CACHE", old_cache->c_str(), 1);
        else UnsetSvmConfigEnvForTest("SVM_JIT_CACHE");
    });
    Check(SetSvmConfigEnvForTest("SVM_PROF2", "1", 1) == 0 &&
          SetSvmConfigEnvForTest("SVM_JIT_CACHE", "", 1) == 0 && Perf2Enabled(),
          "enable existing phase counters and isolate Block fallback cache");
    for (const bool hir : {false, true}) for (const bool fallback : {false, true}) {
        GuestWindow guest;
        auto config = TestConfig(guest.memory);
        config.global_opts = Optimizations::BlockLink;
        AddressSpace space{config};
        const auto module = space.GetDefaultModule();
        const VAddr target_pc = 0x100;
        const VAddr source_pc = 2 * static_cast<VAddr>(getpagesize()) + 0x100;
        IntrusivePtr<Block> target{new Block(0, Location{target_pc})};
        target->SetEndLocation(Location{target_pc + 1});
        const auto result = target->LoadImm(ValueType::U64, Imm{u64{0x85}});
        target->StoreUniform(Uniform{0, ValueType::U64}, result);
        target->SetTerminal(terminal::ReturnToHost{});
        target->ReIdInstr();
        Check(module->Push(target.get()), "register the independent fallback target with its Module");
        auto* target_code = static_cast<u8*>(TranslateIR(module, target));
        Check(target_code != nullptr, "compile the real Block fallback target");
        space.PushCodeCache(Location{target_pc}, target_code);

        IntrusivePtr<Block> source;
        std::optional<HIRBuilder> builder;
        HIRBlock* source_hir{};
        if (hir) {
            builder.emplace(1, true, ResolveFeatureSet(module->GetModuleConfig()));
            auto* function = builder->AppendFunction(Location{source_pc}, Location{source_pc + 1});
            source_hir = function->CreateOrGetBlock(Location{source_pc});
            const auto marker = function->LoadImm(ValueType::U64, Imm{u64{0x37}});
            function->StoreUniform(Uniform{8, ValueType::U64}, marker);
            function->EndBlock(terminal::LinkBlock{Location{target_pc}});
            function->EndFunction();
            function->ComputeRPO();
            function->IdByRPO();
            source.reset(source_hir->GetBlock());
            source->SetEndLocation(Location{source_pc + 1});
            u32 position{};
            for (const auto& instruction : source->GetInstList())
                Check(instruction.Id() == position++, "single real HIR block already has local dense IDs");
            const auto* value_record = function->GetHIRValue(marker);
            Check(value_record && value_record->GetUses().size() == 1,
                  "retain the HIR marker's real StoreUniform use before final Block metadata");
            const auto* use_record = &value_record->GetUses().front();
            const auto* user = use_record->inst;
            const auto argument = use_record->arg_idx;
            const auto uses = marker.Def()->GetUses();
            // Function RPO numbering does not fill Block::MaxInstrId. This
            // single-block fixture keeps the same IDs while finalizing it.
            source->ReIdInstr();
            Check(function->GetHIRValue(marker) == value_record &&
                  value_record->GetUses().size() == 1 &&
                  &value_record->GetUses().front() == use_record &&
                  use_record->inst == user && use_record->arg_idx == argument &&
                  marker.Def()->GetUses() == uses,
                  "final Block numbering preserves the HIR value and original use-chain nodes");
            Check(IsEmpty(module->GetNode(Location{source_pc})),
                  "HIR source is left for TranslateIR's standalone detach-and-publish path");
        } else {
            source.reset(new Block(0, Location{source_pc}));
            source->SetEndLocation(Location{source_pc + 1});
            const auto marker = source->LoadImm(ValueType::U64, Imm{u64{0x37}});
            source->StoreUniform(Uniform{8, ValueType::U64}, marker);
            source->SetTerminal(terminal::LinkBlock{Location{target_pc}});
            source->ReIdInstr();
            Check(module->Push(source.get()), "register the independent fallback source with its Module");
        }
        Check(source->GetInstList().size() == source->MaxInstrId(),
              "both final Block fixtures have complete instruction-count metadata");
        const auto& stats = GetPerfStats2();
        const std::array before{
                stats.pass_total.calls.load(std::memory_order_relaxed),
                stats.allocate_block.calls.load(std::memory_order_relaxed),
                stats.allocate_function.calls.load(std::memory_order_relaxed),
                stats.codegen_complete.calls.load(std::memory_order_relaxed)};
        BlockEmissionAllocationWatch watch{.module = module.get(), .block = source.get(),
                                            .reject_first = fallback};
        block_emission_allocation_watch = &watch;
        const auto clear_watch = ScopeExit([&] { block_emission_allocation_watch = nullptr; });
        auto* entry = static_cast<u8*>(hir ? TranslateIR(module, source_hir)
                                           : TranslateIR(module, source));
        block_emission_allocation_watch = nullptr;
        Check(entry != nullptr, hir ? "HIRBlock TranslateIR accepts the final dense fixture"
                                    : "independent Block TranslateIR accepts the registered fixture");
        Check(watch.calls == (fallback ? 2u : 1u),
              fallback ? "forced fallback reaches both allocation attempts"
                       : "ordinary emission reaches exactly one allocation attempt");
        Check(watch.required_region[0], "first emission explicitly requests a direct-link region");
        Check(watch.rejected == fallback, "only the forced-fallback case injects the first rejection");
        Check(watch.unpublished, "no allocation attempt sees an already published source cache or target");
        const auto source_node = module->GetNode(Location{source_pc});
        Check(IsBlock(source_node) && GetBlock(source_node).get() == source.get(),
              "both APIs retain the exact source Block in the Module after publication");
        Check(watch.phases[0][0] == before[0] + (hir ? 0u : 1u) &&
              watch.phases[0][1] == before[1] + 1 && watch.phases[0][2] == before[2] &&
              watch.phases[0][3] == before[3] + 1,
              "both Block APIs prepare once through the Block allocation entry");
        if (fallback) {
            Check(!watch.required_region[1] && watch.sizes[0] && watch.sizes[1] &&
                  watch.phases[0] == watch.phases[1] &&
                  watch.ir_signature[0] == watch.ir_signature[1],
                  "fallback re-emits unchanged prepared IR without optimization or allocation");
        }
        Check(source->GetJitCache().jit_state == JitState::Cached &&
              module->GetJitCache(source->GetJitCache()) == entry,
              "the final successful allocation is the sole Block cache entry");
        FaultEntry fault;
        Check(module->LookupFault(entry, fault) && fault.guest_loc == source_pc &&
              fault.Contains(entry), "the selected emission publishes its source fault range");
        const auto published = space.GetLinkManager().QueryTarget(source_pc);
        Check(published && published->host_pc == entry,
              "the selected emission publishes the final source link target");
        Check(space.GetLinkManager().GetStats().sites == (fallback ? 0u : 1u),
              "disabled-region re-emission drops the rejected buffer's direct-link sites");
        space.PushCodeCache(Location{source_pc}, entry);
        Runtime runtime{&space};
        for (unsigned run = 0; run < 2; ++run) {
            std::memset(runtime.GetUniformBuffer().data(), 0, 16);
            runtime.SetLocation(source_pc);
            Check(runtime.Run() == HaltReason::CallHost,
                  "the selected Block emission executes through the actual target");
            u64 observed{}, marker{};
            std::memcpy(&observed, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&marker, runtime.GetUniformBuffer().data() + 8, 8);
            Check(observed == 0x85 && marker == 0x37,
                  "normal and rebuilt emissions preserve both source and target effects");
        }
        const auto sites = space.GetLinkManager().QuerySourceSites({module.get(), entry});
        Check(fallback ? sites.empty() : sites.size() == 1 && sites.front().state == LinkSiteState::Linked,
              "only the ordinary region emission becomes a live direct source link");
        Check(stats.allocate_block.calls.load(std::memory_order_relaxed) == before[1] + 1 &&
              stats.allocate_function.calls.load(std::memory_order_relaxed) == before[2],
              "warm execution reuses the final cache without another allocation");
    }
#endif
}

void Cycles() {
    BlockEmissionFallback();
    for (unsigned i = 0; i < 16; ++i) {
        void* rx{}; void* rw{}; void* entry{};
        std::shared_ptr<Module> retained;
        {
            AddressSpace space{TestConfig()};
            retained = space.MapModule(0x100, 0x400, ModuleConfig{});
            const auto [id, code] = retained->AllocCodeCache(64);
            rx = code.exec_data; rw = code.rw_data;
            entry = reinterpret_cast<void*>(space.GetTrampolines().GetRuntimeEntry());
            Check(Mapped(rx) && Mapped(rw) && Mapped(entry), "cycle owns executable and trampoline mappings");
        }
        Check(!Mapped(rx) && !Mapped(rw) && !Mapped(entry), "ordinary AddressSpace destructor physically releases all code");
        Check(!retained->GetCodeRegion(static_cast<u8*>(rx)), "external Module handle is inert after ordinary destructor");
    }
}

void ResourceCycles() {
    GuestWindow guest;
    auto config = TestConfig(guest.memory);
    config.global_opts = Optimizations::BlockLink | Optimizations::ReturnStackBuffer;
    config.region_edges = true;
    const auto page = static_cast<u64>(getpagesize());
    const VAddr target_pc = 0x100, source_pc = 2 * page + 0x100, secondary_pc = 3 * page + 0x100;
    const auto target_image = Artifact(config, guest.memory, true);
    const auto [image, captured] = RestoredSourceFixture(config, guest.memory, target_image, target_pc, source_pc, secondary_pc);
    (void)captured;
    AddressSpace space{config};
    const auto module = space.GetDefaultModule();
    auto& smc = space.GetSmcTracker();
    auto& links = space.GetLinkManager();
    aot::LoadReport target_report;
    std::string error;
    Check(aot::InstallArtifact(space, target_image, guest.memory, config.guest_addr_mask, target_report, error) == aot::kAotOk,
          "install the shared target for resource cycles");
    const auto target_before = links.QueryTarget(target_pc);
    Check(target_before && target_before->host_pc, "bind the shared target before resource cycles");
    TranslateTable l1{8};
    const auto token = smc.RegisterRuntime(l1);
    const auto unregister = ScopeExit([&] { smc.UnregisterRuntime(token); });
    const auto finish_watch = ScopeExit([] { watched_code_free = nullptr; watched_purge_allocation = nullptr; });
    constexpr unsigned kCycles = 6;
    for (unsigned cycle = 0; cycle < kCycles; ++cycle) {
        aot::LoadReport report;
        const auto status = aot::InstallArtifact(space, image, guest.memory, config.guest_addr_mask, report, error);
        Check(status == aot::kAotOk && report.units_installed == 1,
              error.empty() ? "install the cycle source" : error.c_str());
        const auto node = module->GetNode(Location{source_pc});
        Check(IsFunction(node), "retain the cycle source node");
        const auto lifetime = GetFunction(node)->code_lifetime;
        const auto region = module->GetCodeRegion(lifetime ? lifetime->base : nullptr);
        Check(lifetime && region && lifetime->entries.size() == 2, "bind the cycle allocation");
        for (const auto pc : {source_pc, secondary_pc}) {
            Runtime runtime{&space};
            runtime.SetLocation(pc);
            Check(runtime.Run() == HaltReason::CallHost, "execute the cycle source through the shared target");
            u64 value{}, witness{};
            std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
            std::memcpy(&witness, runtime.GetUniformBuffer().data() + 16, 8);
            Check(value == 37 && witness == 91, "cycle executes source and target witnesses");
        }
        const LinkSourceOwner owner{module.get(), lifetime->base};
        const auto sites = links.QuerySourceSites(owner);
        Check(sites.size() == 2 && std::all_of(sites.begin(), sites.end(), [](const auto& site) {
            return site.state == LinkSiteState::Linked;
        }), "cycle source links both branches before retirement");
        watched_code_free = SiteRxToRw(*region, lifetime->base);
        watched_code_frees = 0;
        Check(smc.RetireNode(space, &l1, module, GetFunction(node).get()), "retire the cycle source");
        FaultEntry absent;
        Check(watched_code_frees == 1 && links.QuerySourceSites(owner).empty() &&
              !module->LookupCodeAllocation(lifetime->base, absent) &&
              !space.GetCodeCacheTable().Lookup(source_pc) && IsEmpty(module->GetNode(Location{source_pc})),
              "each cycle frees its allocation exactly once and leaves no publication");
        Check(links.QueryTarget(target_pc) && links.QueryTarget(target_pc)->host_pc == target_before->host_pc,
              "the shared target survives every cycle unchanged");
    }
    Runtime runtime{&space};
    runtime.SetLocation(target_pc);
    Check(runtime.Run() == HaltReason::CallHost, "shared target still executes after all cycles");
    u64 value{}; std::memcpy(&value, runtime.GetUniformBuffer().data(), 8);
    Check(value == 37, "shared target result survives all cycles");
    std::printf("resource_cycles cycles=%u installs=%u retirements=%u frees_per_cycle=1 target_stable=1\n", kCycles, kCycles, kCycles);
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        const std::string_view mode{argv[1]};
        if (mode == "gates") Gates();
        else if (mode == "tokens") Tokens();
        else if (mode == "running") Running();
        else if (mode == "construction") Construction();
        else if (mode == "retained") Retained();
        else if (mode == "save") Persistence();
        else if (mode == "owners") Owners();
        else if (mode == "readers") Readers();
        else if (mode == "retry-rx") Retry(false);
        else if (mode == "retry-rw") Retry(true);
        else if (mode == "smc") Smc();
        else if (mode == "close") CloseFailure();
        else if (mode == "aot") AotOwners();
        else if (mode == "symbols") Symbols();
        else if (mode == "retire-oom-node" || mode == "retire-oom-range" || mode == "retire-oom-write")
            RetirementFailureSweep(mode);
        else if (mode == "range-unprotect" || mode == "range-unprotect-terminal")
            RangeUnprotectRetry(mode == "range-unprotect-terminal");
        else if (mode == "range-republish" || mode == "range-republish-protect-fail")
            RangeRepublish(mode == "range-republish-protect-fail");
        else if (mode == "symbols-smc" || mode == "symbols-range" || mode == "symbols-write") SmcSymbols(mode);
        else if (mode == "symbols-latest") LatestSmcSymbols();
        else if (mode == "symbols-publication") PublicationSmcSymbols();
        else if (mode == "symbols-rollback") RollbackSmcSymbols();
        else if (mode == "symbols-compound") CompoundSmcSymbols();
        else if (mode == "multi-install" || mode == "multi-install-cleanup" || mode == "multi-install-cleanup-terminal")
            MultiUnitInstallRollback(mode != "multi-install", mode == "multi-install-cleanup-terminal");
        else if (mode == "multi-call-install" || mode == "multi-call-install-cleanup" || mode == "multi-call-install-cleanup-terminal")
            MultiUnitCallRollback(mode != "multi-call-install", mode == "multi-call-install-cleanup-terminal");
        else if (mode == "aot-dispatch") AotDispatchConflict();
        else if (mode == "disk-dispatch") DiskDispatchConflict();
        else if (mode == "disk-revival" || mode == "disk-revival-cleanup" || mode == "disk-revival-cleanup-terminal" || mode == "disk-revival-oom")
            DiskRevivalFailure(mode == "disk-revival-cleanup" || mode == "disk-revival-cleanup-terminal",
                               mode == "disk-revival-cleanup-terminal", mode == "disk-revival-oom");
        else if (mode == "disk-readability" || mode == "aot-readability") GuestReadability(mode == "aot-readability");
        else if (mode == "guest-hash") GuestHashRead();
        else if (mode == "aot-entry" || mode == "disk-entry") EntryAdmission(mode == "aot-entry");
        else if (mode == "aot-entry-node" || mode == "disk-entry-node") EntryAdmission(mode.starts_with("aot"), "node");
        else if (mode == "aot-entry-range" || mode == "disk-entry-range") EntryAdmission(mode.starts_with("aot"), "range");
        else if (mode == "aot-entry-write" || mode == "disk-entry-write") EntryAdmission(mode.starts_with("aot"), "write");
        else if (mode == "aot-reader-node" || mode == "disk-reader-node") RestoredSourceReaders(mode.starts_with("aot"), "node");
        else if (mode == "aot-reader-range" || mode == "disk-reader-range") RestoredSourceReaders(mode.starts_with("aot"), "range");
        else if (mode == "aot-reader-write" || mode == "disk-reader-write") RestoredSourceReaders(mode.starts_with("aot"), "write");
        else if (mode == "aot-linked-oom" || mode == "disk-linked-oom") LinkedRevivalFailure(mode.starts_with("aot"), "oom");
        else if (mode == "aot-linked-protect" || mode == "disk-linked-protect") LinkedRevivalFailure(mode.starts_with("aot"), "protect");
        else if (mode == "aot-linked-cleanup" || mode == "disk-linked-cleanup") LinkedRevivalFailure(mode.starts_with("aot"), "cleanup");
        else if (mode == "aot-linked-terminal" || mode == "disk-linked-terminal") LinkedRevivalFailure(mode.starts_with("aot"), "terminal");
        else if (mode == "aot-linked-call-oom" || mode == "disk-linked-call-oom") LinkedCallFailure(mode.starts_with("aot"), "oom");
        else if (mode == "aot-linked-call-protect" || mode == "disk-linked-call-protect") LinkedCallFailure(mode.starts_with("aot"), "protect");
        else if (mode == "aot-linked-call-cleanup" || mode == "disk-linked-call-cleanup") LinkedCallFailure(mode.starts_with("aot"), "cleanup");
        else if (mode == "aot-linked-call-terminal" || mode == "disk-linked-call-terminal") LinkedCallFailure(mode.starts_with("aot"), "terminal");
        else if (mode == "aot-linked-multi-oom" || mode == "disk-linked-multi-oom") LinkedMultiUnitFailure(mode.starts_with("aot"), "oom");
        else if (mode == "aot-linked-multi-protect" || mode == "disk-linked-multi-protect") LinkedMultiUnitFailure(mode.starts_with("aot"), "protect");
        else if (mode == "aot-linked-multi-cleanup" || mode == "disk-linked-multi-cleanup") LinkedMultiUnitFailure(mode.starts_with("aot"), "cleanup");
        else if (mode == "aot-linked-multi-terminal" || mode == "disk-linked-multi-terminal") LinkedMultiUnitFailure(mode.starts_with("aot"), "terminal");
        else if (mode == "aot-linked-private-oom" || mode == "disk-linked-private-oom") LinkedPrivateFailure(mode.starts_with("aot"));
        else if (mode == "aot-linked-chain-oom" || mode == "disk-linked-chain-oom") LinkedChainFailure(mode.starts_with("aot"), "oom");
        else if (mode == "aot-linked-chain-protect" || mode == "disk-linked-chain-protect") LinkedChainFailure(mode.starts_with("aot"), "protect");
        else if (mode == "aot-linked-chain-cleanup" || mode == "disk-linked-chain-cleanup") LinkedChainFailure(mode.starts_with("aot"), "cleanup");
        else if (mode == "aot-linked-chain-terminal" || mode == "disk-linked-chain-terminal") LinkedChainFailure(mode.starts_with("aot"), "terminal");
        else if (mode == "aot-linked-reuse" || mode == "disk-linked-reuse") LinkedReuse(mode.starts_with("aot"));
        else if (mode == "linked-executor-retire") LinkedExecutorRetire();
        else if (mode == "aot-linked-executor-retire" || mode == "disk-linked-executor-retire")
            LinkedRestoredExecutorRetire(mode.starts_with("aot"));
        else if (mode == "aot-linked-call-executor" || mode == "disk-linked-call-executor")
            LinkedCallExecutorRetire(mode.starts_with("aot"));
        else if (mode == "aot-linked-call-invalidate" || mode == "disk-linked-call-invalidate")
            LinkedCallTargetInvalidate(mode.starts_with("aot"));
        else if (mode == "aot-linked-double-retire" || mode == "disk-linked-double-retire")
            LinkedConcurrentDoubleRetire(mode.starts_with("aot"));
        else if (mode == "aot-linked-compile-retire" || mode == "disk-linked-compile-retire")
            LinkedConcurrentCompileRetire(mode.starts_with("aot"));
        else if (mode == "aot-linked-install-retire" || mode == "disk-linked-install-retire")
            LinkedConcurrentInstallRetire(mode.starts_with("aot"));
        else if (mode == "aot-linked-install-failure" || mode == "disk-linked-install-failure")
            LinkedConcurrentInstallFailure(mode.starts_with("aot"));
        else if (mode == "aot-linked-dual-executor-retire" || mode == "disk-linked-dual-executor-retire")
            LinkedRestoredDualExecutorRetire(mode.starts_with("aot"));
        else if (mode == "aot-linked-target-executor" || mode == "disk-linked-target-executor")
            LinkedTargetExecutorInvalidate(mode.starts_with("aot"));
        else if (mode == "aot-linked-concurrent-install" || mode == "disk-linked-concurrent-install")
            LinkedConcurrentInstall(mode.starts_with("aot"));
        else if (mode == "aot-linked-concurrent-compile" || mode == "disk-linked-concurrent-compile")
            LinkedConcurrentCompile(mode.starts_with("aot"));
        else if (mode == "aot-linked-partial-rx" || mode == "disk-linked-partial-rx") LinkedPartialViewRelease(mode.starts_with("aot"), false);
        else if (mode == "aot-linked-partial-rw" || mode == "disk-linked-partial-rw") LinkedPartialViewRelease(mode.starts_with("aot"), true);
        else if (mode == "aot-linked-race-source" || mode == "disk-linked-race-source") LinkedPublicationRace(mode.starts_with("aot"), "source");
        else if (mode == "aot-linked-race-target" || mode == "disk-linked-race-target") LinkedPublicationRace(mode.starts_with("aot"), "target");
        else if (mode == "aot-linked-race-duplicate") LinkedPublicationRace(true, "duplicate");
        else if (mode == "symbols-reader-node" || mode == "symbols-reader-range")
            OrdinarySourceReaders(mode == "symbols-reader-range");
        else if (mode == "symbols-dirty") DirtySmcSymbols();
        else if (mode == "symbols-child-node" || mode == "symbols-child-range" || mode == "symbols-child-write")
            ChildSmcSymbols(mode);
        else if (mode == "instruction-pin-terminal" || mode == "instruction-pin-reclaim" || mode == "instruction-pin-failure")
            InstructionPinLifetime(mode);
        else if (mode == "cycles") Cycles();
        else if (mode == "resource-cycles") ResourceCycles();
        else return 2;
        std::printf("mode=%s checks=%u passed\n", argv[1], checks);
        return 0;
    } catch (const std::exception& error) {
        deny_allocations = false;
        fail_build_allocation = fail_uniform_allocation = false;
        std::fprintf(stderr, "failure: %s\n", error.what());
        return 1;
    }
}
