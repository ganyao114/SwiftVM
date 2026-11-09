#include "dispatch_cache.h"

#include <cerrno>
#include <sys/mman.h>

#include "base/error.h"

namespace swift::runtime::execution {

DispatchCache::~DispatchCache() { (void)Release(); }

void DispatchCache::Ensure() {
    if (data) return;
    auto* mapping =
            mmap(nullptr, Bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) swift::ThrowSystemError("map thread dispatch cache", errno);
    data = static_cast<DispatchEntry*>(mapping);
    // Every other empty tag is zero, which indexes slot zero rather than its
    // own slot. UINT64_MAX indexes the last slot, so slot zero cannot false-hit.
    data[0].pc = UINT64_MAX;
}

void DispatchCache::Reset() {
    if (!data) return;
    if (madvise(data, Bytes, MADV_DONTNEED))
        swift::ThrowSystemError("reset thread dispatch cache", errno);
    data[0].pc = UINT64_MAX;
}

void DispatchCache::Fill(std::uint64_t pc, backend::NativeCode::Function entry) noexcept {
    data[Index(pc)] = {pc, entry};
}

void DispatchCache::ClearKey(std::uint64_t pc) noexcept {
    const auto index = Index(pc);
    data[index] = {index ? 0 : UINT64_MAX, nullptr};
}

MappingRelease DispatchCache::Release() noexcept {
    if (!data) return {};
    if (munmap(data, Bytes)) return {1, errno, 0};
    data = nullptr;
    return {};
}

DispatchInterrupt::~DispatchInterrupt() { (void)Release(); }

void DispatchInterrupt::Ensure() {
    if (data) return;
    auto* mapping =
            mmap(nullptr, DispatchCache::Bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) swift::ThrowSystemError("map interrupt dispatch view", errno);
    data = static_cast<DispatchEntry*>(mapping);
}

MappingRelease DispatchInterrupt::Release() noexcept {
    if (!data) return {};
    if (munmap(data, DispatchCache::Bytes)) return {1, errno, 0};
    data = nullptr;
    return {};
}

}  // namespace swift::runtime::execution
