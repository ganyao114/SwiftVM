#include "disk_cache.h"
#include "runtime/platform/linux/artifact_file.h"

#include <exception>
#include <utility>

namespace swift::runtime::execution {

std::size_t DiskCache::Hash::operator()(Key key) const noexcept {
    auto value = key.pc ^ (std::uint64_t{key.fp_mode} * UINT64_C(0x9e3779b97f4a7c15));
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    return static_cast<std::size_t>(value);
}

DiskCache::DiskCache(std::string directory, CodeImageKey key) {
    platform::EnsureCacheDirectory(directory);
    image.key = key;
    path = std::move(directory) + "/" + HexDigest(CodeImageKeyDigest(key)) + ".svmcache";
}

bool DiskCache::Load() noexcept {
    if (disabled || dirty) return false;
    try {
        CodeImage loaded;
        std::string error;
        const auto bytes = platform::ReadArtifactFile(path, true);
        if (!DecodeCodeImage(bytes, loaded, error) || loaded.key != image.key) return false;
        std::unordered_map<Key, std::size_t, Hash> ready;
        for (std::size_t i = 0; i < loaded.units.size(); ++i) {
            const auto& unit = loaded.units[i];
            ready.try_emplace(Key{unit.pc, unit.fp_mode}, i);
            for (const auto& entry : unit.code.entries)
                ready.try_emplace(Key{entry.pc, unit.fp_mode}, i);
        }
        image = std::move(loaded);
        index = std::move(ready);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

const CodeUnit* DiskCache::Find(std::uint64_t pc, std::uint8_t fp_mode) const noexcept {
    if (disabled) return nullptr;
    const auto found = index.find({pc, fp_mode});
    return found == index.end() ? nullptr : &image.units[found->second];
}

void DiskCache::Record(CodeUnit unit) {
    if (disabled || !unit.code.relocatable) return;
    const Key key{unit.pc, unit.fp_mode};
    const auto found = index.find(key);
    if (found != index.end()) {
        // A rejected source policy may be replaced by a fresh compilation.
        // Alias entries retain the same first-ready ownership as CodeCache.
        const auto position = found->second;
        auto& existing = image.units[position];
        if (existing.pc != unit.pc) return;
        for (const auto& entry : existing.code.entries) {
            const auto alias = index.find({entry.pc, existing.fp_mode});
            if (alias != index.end() && alias->second == position && alias->first != key)
                index.erase(alias);
        }
        existing = std::move(unit);
        for (const auto& entry : existing.code.entries)
            index.try_emplace(Key{entry.pc, existing.fp_mode}, position);
    } else {
        const auto position = image.units.size();
        image.units.push_back(std::move(unit));
        index.emplace(key, position);
        for (const auto& entry : image.units.back().code.entries)
            index.try_emplace(Key{entry.pc, key.fp_mode}, position);
    }
    dirty = true;
}

bool DiskCache::Flush() noexcept {
    if (disabled) return false;
    if (!dirty) return true;
    try {
        platform::WriteArtifactFile(path, EncodeCodeImage(image));
        dirty = false;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}
}  // namespace swift::runtime::execution
