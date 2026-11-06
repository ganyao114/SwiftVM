#include "patch.h"
#include "abi.h"
#include "encoding.h"
#include <algorithm>
#include <cstring>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
namespace swift::runtime::backend::arm64 {

void SynchronizeCode(std::uint8_t* writable,
                     const std::uint8_t* executable,
                     std::size_t bytes) noexcept {
    if (!bytes) return;
#if defined(__linux__) && defined(__aarch64__)
    // Arm's PoU sequence, with LLVM compiler-rt's CTR_EL0 IDC/DIC and line
    // sizes. Clean the alias actually written; invalidate the alias fetched.
    std::uint64_t ctr;
    asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
    if (!(ctr & (std::uint64_t{1} << 28))) {
        const auto line = std::uint64_t{4} << ((ctr >> 16) & 15);
        const auto begin = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(writable));
        const auto end = begin + bytes;
        for (auto at = begin & ~(line - 1); at < end; at += line)
            asm volatile("dc cvau, %0" : : "r"(at) : "memory");
    }
    asm volatile("dsb ish" : : : "memory");
    if (!(ctr & (std::uint64_t{1} << 29))) {
        const auto line = std::uint64_t{4} << (ctr & 15);
        const auto begin = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(executable));
        const auto end = begin + bytes;
        for (auto at = begin & ~(line - 1); at < end; at += line)
            asm volatile("ic ivau, %0" : : "r"(at) : "memory");
        asm volatile("dsb ish" : : : "memory");
    }
    asm volatile("isb" : : : "memory");
#else
    __builtin___clear_cache(reinterpret_cast<char*>(writable),
                            reinterpret_cast<char*>(writable + bytes));
    if (writable != executable)
        __builtin___clear_cache(
                reinterpret_cast<char*>(const_cast<std::uint8_t*>(executable)),
                reinterpret_cast<char*>(const_cast<std::uint8_t*>(executable + bytes)));
#endif
}

namespace {
bool Imm19Link(std::uint32_t instruction) noexcept { return encoding::Imm19(instruction); }

bool WriteLinkInstruction(std::uintptr_t* writable,
                          std::uint32_t back_words,
                          std::uint32_t instruction) noexcept {
    auto* write = reinterpret_cast<std::uint32_t*>(writable) - back_words;
    if (__atomic_load_n(write, __ATOMIC_RELAXED) == instruction) return false;
    __atomic_store_n(write, instruction, __ATOMIC_RELEASE);
    return true;
}
void SynchronizeLink(std::uintptr_t* writable,
                     const std::uintptr_t* executable,
                     std::uint32_t gate_back,
                     std::uint32_t head_back,
                     bool gate_changed,
                     bool head_changed) noexcept {
    if (!gate_changed && !head_changed) return;
    const auto first = std::max(gate_changed ? gate_back : 0, head_changed ? head_back : 0);
    const auto last =
            std::min(gate_changed ? gate_back : UINT32_MAX, head_changed ? head_back : UINT32_MAX);
    SynchronizeCode(reinterpret_cast<std::uint8_t*>(writable) - first * 4,
                    reinterpret_cast<const std::uint8_t*>(executable) - first * 4,
                    (first - last + 1) * 4);
}

void PatchLink(std::uintptr_t* writable,
               const std::uintptr_t* executable,
               std::uintptr_t target,
               std::uint32_t back_words,
               bool synchronize) noexcept {
    // Keep the far fallback ready before publishing a direct instruction.
    __atomic_store_n(writable, target, __ATOMIC_RELEASE);
    const auto head = __atomic_load_n(reinterpret_cast<std::uint32_t*>(writable) - back_words,
                                      __ATOMIC_RELAXED);
    const bool conditional = Imm19Link(head);
    const auto gate_back = conditional ? 2u : back_words;
    const auto from =
            reinterpret_cast<std::uintptr_t>(executable) - gate_back * sizeof(std::uint32_t);
    const auto delta = static_cast<std::int64_t>(target - from);
    const auto instruction =
            !(delta & 3) && delta >= -(INT64_C(1) << 27) && delta < (INT64_C(1) << 27)
                    ? encoding::Branch(delta / 4)
                    : encoding::LoadLiteral16(gate_back);
    const bool gate_changed = WriteLinkInstruction(writable, gate_back, instruction);
    bool head_changed{};
    if (conditional) {
        const auto head_from =
                reinterpret_cast<std::uintptr_t>(executable) - back_words * sizeof(std::uint32_t);
        const auto distance = static_cast<std::int64_t>(target - head_from);
        const auto words =
                !(distance & 3) && distance >= -(INT64_C(1) << 20) && distance < (INT64_C(1) << 20)
                        ? static_cast<std::uint32_t>(distance / 4)
                        : back_words - 2;
        head_changed = WriteLinkInstruction(
                writable, back_words, encoding::Retarget19(head, static_cast<std::int32_t>(words)));
    }
    // This mutation runs while native execution is stopped. Keep the target,
    // gate, head store order, then synchronize the actual changed range once.
    if (synchronize)
        SynchronizeLink(writable, executable, gate_back, back_words, gate_changed, head_changed);
}
}  // namespace

void PatchNativeLink(std::uintptr_t* writable,
                     const std::uintptr_t* executable,
                     std::uintptr_t target,
                     NativeOffset back_bytes) noexcept {
    PatchLink(writable, executable, target, back_bytes / 4, true);
}

void UnlinkNativeLink(std::uintptr_t* writable,
                      const std::uintptr_t* executable,
                      NativeOffset back_bytes) noexcept {
    const auto back_words = back_bytes / 4;
    __atomic_store_n(writable, reinterpret_cast<std::uintptr_t>(executable + 1), __ATOMIC_RELEASE);
    const auto head = __atomic_load_n(reinterpret_cast<std::uint32_t*>(writable) - back_words,
                                      __ATOMIC_RELAXED);
    const bool conditional = Imm19Link(head);
    const auto gate_back = conditional ? 2u : back_words;
    const bool gate_changed =
            WriteLinkInstruction(writable, gate_back, encoding::LoadLiteral16(gate_back));
    bool head_changed{};
    if (conditional)
        head_changed = WriteLinkInstruction(
                writable, back_words, encoding::Retarget19(head, back_words - 2));
    SynchronizeLink(writable, executable, gate_back, back_words, gate_changed, head_changed);
}

void ResetIndirectSite(std::uintptr_t* writable_array, const std::uintptr_t* fallback) noexcept {
    for (unsigned i = 0; i < abi::DynamicCacheEntries; ++i)
        __atomic_store_n(writable_array + i * 2 + 1,
                         reinterpret_cast<std::uintptr_t>(fallback),
                         __ATOMIC_RELEASE);
}

bool InitializeNativeLink(std::span<std::uint8_t> write,
                          std::uintptr_t execute,
                          const backend::LinkSite& site,
                          std::uintptr_t ready_target,
                          std::uintptr_t writable_alias) noexcept {
    const auto data = site.data_offset;
    if ((data & 7) || data > write.size()) return false;
    const auto fallback = site.fallback_offset;
    if ((fallback & 3) || fallback >= write.size()) return false;
    if (site.kind == LinkKind::Dynamic) {
        constexpr auto bytes = abi::DynamicCacheWords * 4u;
        if (bytes > write.size() - data) return false;
        for (unsigned i = 0; i < abi::DynamicCacheEntries; ++i) {
            const std::uintptr_t cold = execute + fallback;
            std::memcpy(write.data() + data + i * 16 + 8, &cold, 8);
        }
        const auto alias = writable_alias ? writable_alias + data : 0;
        std::memcpy(write.data() + data + bytes - 8, &alias, 8);
        return true;
    }
    const auto branch = site.branch_offset;
    if (8u > write.size() - data || (branch & 3) || branch >= data) return false;
    auto* pointer = reinterpret_cast<std::uintptr_t*>(write.data() + data);
    auto* executable = reinterpret_cast<const std::uintptr_t*>(execute + data);
    if (ready_target && writable_alias)
        PatchLink(pointer, executable, ready_target, (data - branch) / 4, false);
    else {
        const auto cold = execute + fallback;
        std::memcpy(pointer, &cold, 8);
    }
    return true;
}

bool PatchOrderedFallback(const backend::NativeCode& code,
                          NativeOffset fast,
                          NativeOffset fallback,
                          std::uintptr_t page_size) noexcept {
    if (!page_size || (fast & 3) || (fallback & 3) || fast > code.bytes || code.bytes - fast < 4 ||
        fallback > code.bytes || code.bytes - fallback < 12)
        return false;
    const auto fast_word = fast / 4, fallback_word = fallback / 4;
    auto* execute = reinterpret_cast<std::uint8_t*>(code.entry) + std::size_t{fast_word} * 4;
    const auto instruction =
            __atomic_load_n(reinterpret_cast<const std::uint32_t*>(execute), __ATOMIC_ACQUIRE);
    const bool ordered = encoding::OrderedAccess(instruction) != encoding::OrderedKind::None;
    // Inspect only this actual generated instruction. A later fault from an
    // already-patched plain cold access is fatal, not another alignment retry.
    const auto distance = static_cast<std::int64_t>(fallback_word) - fast_word;
    if (!ordered || distance < -(std::int64_t{1} << 25) || distance >= (std::int64_t{1} << 25))
        return false;
    const auto address = reinterpret_cast<std::uintptr_t>(execute);
    const auto page = address - address % page_size;
    const auto protect = [&](int permissions) {
#if defined(__linux__)
        return syscall(SYS_mprotect, reinterpret_cast<void*>(page), page_size, permissions);
#else
        return mprotect(reinterpret_cast<void*>(page), page_size, permissions);
#endif
    };
    if (!code.writable && protect(PROT_READ | PROT_WRITE)) return false;
    auto* write = code.writable ? code.writable + std::size_t{fast_word} * 4 : execute;
    const auto branch = encoding::Branch(distance);
    // The complete half-barrier stub was published with this same code owner.
    // Only the actual fast instruction changes, in one aligned atomic write.
    __atomic_store_n(reinterpret_cast<std::uint32_t*>(write), branch, __ATOMIC_RELEASE);
    if (code.writable) {
        SynchronizeCode(write, execute, 4);
        return true;
    }
    // Single-RX fallback retains its full synchronization before mprotect.
    __builtin___clear_cache(reinterpret_cast<char*>(write), reinterpret_cast<char*>(write + 4));
    if (protect(PROT_READ | PROT_EXEC)) return false;
    return true;
}
}  // namespace swift::runtime::backend::arm64
