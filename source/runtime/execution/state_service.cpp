#include "vm.h"
#include "runtime/arch/x86/save_state.h"
#include <cstring>

namespace swift::runtime::execution {
namespace front = ::swift::x86;

front::AccessResult VM::StateImage(void* opaque,
                                   unsigned raw_kind,
                                   std::uint64_t address,
                                   std::uint64_t requested) noexcept {
    auto& vm = *static_cast<VM*>(opaque);
    using x86::StateImageKind;
    const auto kind = static_cast<StateImageKind>(raw_kind & 255);
    const bool wide = (raw_kind & 256) != 0;
    const bool compact_save = kind == StateImageKind::XSaveCompact;
    const bool extended = kind == StateImageKind::XSave || kind == StateImageKind::XSaveOpt ||
                          compact_save || kind == StateImageKind::XRestore;
    if (address & (extended ? 63 : 15)) return {front::AccessStatus::Fault, address};
    const auto selected = requested & x86::GuestXcr0;
    x86::StateImage image{};
    const auto read = [&](std::size_t first, std::size_t size) {
        return Read(&vm, address + first, {image.data() + first, size}, false);
    };
    bool changed{};
    const auto write = [&](std::size_t first, std::size_t size) {
        const auto result = Write(&vm, address + first, {image.data() + first, size}, false);
        changed |= result.code_changed;
        return result;
    };
    if (address > UINT64_MAX - (extended ? x86::XsaveBytes : 512))
        return {front::AccessStatus::Fault, address};
    if (kind == StateImageKind::FxSave) {
        x86::ExportLegacyState(
                vm.thread.native.state, std::span<std::uint8_t, 512>{image.data(), 512}, wide);
        return write(0, 464);  // FXSAVE leaves the software-owned bytes 464..511 untouched.
    }
    if (kind == StateImageKind::FxRestore) {
        const auto loaded = read(0, 512);
        if (loaded.status != front::AccessStatus::Completed) return loaded;
        if (!x86::ImportLegacyState(vm.thread.native.state,
                                    std::span<const std::uint8_t, 512>{image.data(), 512},
                                    3,
                                    wide))
            return {front::AccessStatus::Fault, address};
        return {};
    }
    if (kind == StateImageKind::XSave || kind == StateImageKind::XSaveOpt || compact_save) {
        // XINUSE may conservatively stay set even for an INIT component. No
        // tracking is needed: save every selected component. XSAVEC replaces
        // both bitmaps without reading the old header (SDM vol. 1, 13.10).
        front::AccessResult result;
        std::uint64_t old_bv{};
        if (!compact_save) {
            result = read(512, 8);
            if (result.status != front::AccessStatus::Completed) return result;
            std::memcpy(&old_bv, image.data() + 512, 8);
        }
        x86::ExportLegacyState(
                vm.thread.native.state, std::span<std::uint8_t, 512>{image.data(), 512}, wide);
        const auto bv = (old_bv & ~selected) | selected;
        std::memcpy(image.data() + 512, &bv, 8);
        if (compact_save) {
            const auto format = x86::XsaveCompactedFormat | selected;
            std::memcpy(image.data() + 520, &format, 8);
        }
        if (selected & 1) {
            result = write(0, 24);
            if (result.status != front::AccessStatus::Completed) return result;
            result = write(32, 128);
            if (result.status != front::AccessStatus::Completed) return result;
        }
        // Unlike XSAVE, compacted saves do not include MXCSR for AVX alone.
        if (selected & (compact_save ? 2 : 6)) {
            result = write(24, 8);
            if (result.status != front::AccessStatus::Completed) return result;
        }
        if (selected & 2) {
            result = write(160, 256);
            if (result.status != front::AccessStatus::Completed) return result;
        }
        if (selected & 4) {
            std::memcpy(image.data() + 576, vm.thread.native.state.ymm_high, 256);
            result = write(576, 256);
            if (result.status != front::AccessStatus::Completed) return result;
        }
        result = write(512, compact_save ? 16 : 8);
        result.code_changed |= changed;
        return result;
    }
    if (kind != StateImageKind::XRestore) return {front::AccessStatus::UnsupportedBeforeAccess};
    auto loaded = read(512, 64);
    if (loaded.status != front::AccessStatus::Completed) return loaded;
    std::uint64_t present{}, format{};
    std::memcpy(&present, image.data() + 512, 8);
    std::memcpy(&format, image.data() + 520, 8);
    if (present & ~x86::GuestXcr0) return {front::AccessStatus::Fault, address};
    const bool compact = (format & x86::XsaveCompactedFormat) != 0;
    if (compact) {
        if ((format & ~(x86::XsaveCompactedFormat | x86::GuestXcr0)) || (present & ~format))
            return {front::AccessStatus::Fault, address};
    } else if (format)
        return {front::AccessStatus::Fault, address};
    // Standard XRSTOR checks header bytes 8..23; compacted checks 16..63.
    for (unsigned i = 528; i < (compact ? 576 : 536); ++i)
        if (image[i]) return {front::AccessStatus::Fault, address};
    const auto fetch = selected & present;
    const auto load_mxcsr = compact ? (fetch & 2) : (selected & 6);
    if (fetch & 1) {
        loaded = read(0, 24);
        if (loaded.status != front::AccessStatus::Completed) return loaded;
        loaded = read(32, 128);
        if (loaded.status != front::AccessStatus::Completed) return loaded;
    }
    if (load_mxcsr) {
        loaded = read(24, 8);
        if (loaded.status != front::AccessStatus::Completed) return loaded;
    }
    if (fetch & 2) {
        loaded = read(160, 256);
        if (loaded.status != front::AccessStatus::Completed) return loaded;
    }
    if (fetch & 4) {
        loaded = read(576, 256);
        if (loaded.status != front::AccessStatus::Completed) return loaded;
    }
    // Complete every actual memory read and header validation before any
    // architectural restoration; INIT components do not read absent payload.
    auto restored = vm.thread.native.state;
    if (!x86::ImportLegacyState(
                restored, std::span<const std::uint8_t, 512>{image.data(), 512}, fetch & 3, wide))
        return {front::AccessStatus::Fault, address};
    if ((selected & 1) && !(present & 1)) {
        restored.x87 = {};
        restored.x87_ip = restored.x87_dp = 0;
        restored.x87_opcode = 0;
    }
    if ((selected & 2) && !(present & 2)) std::memset(restored.xmm, 0, sizeof restored.xmm);
    if (selected & 4) {
        if (present & 4)
            std::memcpy(restored.ymm_high, image.data() + 576, 256);
        else
            std::memset(restored.ymm_high, 0, sizeof restored.ymm_high);
    }
    if (selected & (compact ? 2 : 6)) {
        std::uint32_t mxcsr = 0x1f80;
        if (load_mxcsr) std::memcpy(&mxcsr, image.data() + 24, 4);
        if (mxcsr & ~x86::GuestMxcsrMask) return {front::AccessStatus::Fault, address};
        restored.mxcsr = mxcsr;
    }
    vm.thread.native.state = restored;
    return {};
}
}  // namespace swift::runtime::execution
