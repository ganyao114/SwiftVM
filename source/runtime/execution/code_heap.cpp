#include "code_heap.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include "base/error.h"

#include "host_backend.h"

namespace swift::runtime::execution {
namespace {
[[noreturn]] void Failed(const char* operation) { swift::ThrowSystemError(operation, errno); }
}  // namespace

CodeHeap::CodeHeap(const HostBackendOps& backend, std::size_t page_size) noexcept
        : backend(backend), page_size(page_size) {}

CodeHeap::~CodeHeap() { (void)Release(); }

std::size_t CodeHeap::Slabs() const noexcept {
    return static_cast<std::size_t>(std::count_if(slabs.begin(), slabs.end(), [](const auto& slab) {
        return slab.mapping.Writable() != nullptr;
    }));
}

std::size_t CodeHeap::DualSlabs() const noexcept {
    return static_cast<std::size_t>(std::count_if(slabs.begin(), slabs.end(), [](const auto& slab) {
        return slab.mapping.Writable() && slab.mapping.Dual();
    }));
}

std::size_t CodeHeap::ReservedBytes() const noexcept {
    std::size_t total{};
    for (const auto& slab : slabs) total += slab.mapping.ReservedBytes();
    return total;
}

MappingRelease CodeHeap::Release() noexcept {
    MappingRelease result;
    for (auto& slab : slabs) {
        const auto released = slab.mapping.Release();
        result.remaining_views += released.remaining_views;
        if (!result.unmap_error) result.unmap_error = released.unmap_error;
        if (!result.close_error) result.close_error = released.close_error;
    }
    if (!result.remaining_views) {
        std::vector<Slab>{}.swap(slabs);
        std::vector<std::vector<backend::FaultSite>>{}.swap(fault_tables);
    }
    return result;
}

NativeCode CodeHeap::Publish(std::span<const std::uint32_t> words,
                             std::vector<backend::FaultSite> faults,
                             std::span<const backend::LinkSite> links,
                             ReadyResolver ready,
                             void* opaque,
                             std::span<const backend::Relocation> relocations) {
    if (words.empty() || words.size() > (SIZE_MAX - 4096) / 4)
        swift::ThrowError("invalid native code size");
    const auto bytes = words.size_bytes();
    std::span<const backend::FaultSite> sites;
    if (!faults.empty()) {
        fault_tables.push_back(std::move(faults));
        sites = fault_tables.back();
    }
    if (slabs.empty() || !slabs.back().mapping.Writable() ||
        bytes > slabs.back().mapping.Size() - ((slabs.back().used + 15) & ~std::size_t{15})) {
        if (bytes > SIZE_MAX - page_size - 15)
            swift::ThrowError("native slab size overflow", ErrorKind::ResourceLimit);
        const auto size = (std::max(std::size_t{1} << 20, bytes + 15) + page_size - 1) / page_size *
                          page_size;
        // This row exists before any kernel mapping is acquired. A failed
        // second view/rollback remains represented until the VM closes it.
        slabs.emplace_back();
        slabs.back().mapping.MapNative(size);
    }
    auto& slab = slabs.back();
    const auto offset = (slab.used + 15) & ~std::size_t{15};
    if (!slab.mapping.SetWritable()) Failed("native slab writable");
    auto* write = slab.mapping.Writable() + offset;
    auto* execute = slab.mapping.Executable() + offset;
    std::memcpy(write, words.data(), bytes);
    if (!relocations.empty() &&
        (!backend.relocate || !backend.relocate(backend.context, {write, bytes}, relocations))) {
        if (!slab.mapping.SetExecutable()) Failed("native slab executable");
        swift::ThrowError("invalid native symbol relocation", ErrorKind::InvalidArgument);
    }
    for (const auto& link : links) {
        const auto target =
                link.kind == backend::LinkKind::Static && ready && slab.mapping.Dual()
                        ? ready(opaque, link.target_pc, reinterpret_cast<std::uintptr_t>(execute))
                        : 0;
        if (!backend.initialize_link(backend.context,
                                     {write, bytes},
                                     reinterpret_cast<std::uintptr_t>(execute),
                                     slab.mapping.Dual(),
                                     link,
                                     target)) {
            // Older units share this slab and must regain execute permission
            // even when this unpublished slice has an invalid relocation.
            if (!slab.mapping.SetExecutable()) Failed("native slab executable");
            swift::ThrowError("invalid native link relocation", ErrorKind::InvalidArgument);
        }
    }
    const bool synchronized_aliases = backend.synchronize_code != nullptr;
    if (synchronized_aliases) {
        const int error = backend.synchronize_code(write, execute, bytes);
        if (error) {
            // No entry or link can reach this candidate. The existing slab
            // owner retains its mappings for the ordinary VM close path.
            (void)slab.mapping.SetExecutable();
            swift::ThrowSystemError("synchronize native code", error);
        }
    } else
        __builtin___clear_cache(reinterpret_cast<char*>(write),
                                reinterpret_cast<char*>(write + bytes));
    if (!slab.mapping.SetExecutable()) Failed("native slab executable");
    if (write != execute && !synchronized_aliases)
        __builtin___clear_cache(reinterpret_cast<char*>(execute),
                                reinterpret_cast<char*>(execute + bytes));
    slab.used = offset + bytes;
    return {reinterpret_cast<NativeCode::Function>(execute),
            bytes,
            slab.mapping.Dual() ? write : nullptr,
            sites};
}
}  // namespace swift::runtime::execution
