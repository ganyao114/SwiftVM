#include "vm.h"
#include "disk_cache.h"
#include "timing.h"

#include "base/error.h"

#include <algorithm>
#include <array>

namespace swift::runtime::execution {

bool VM::HashSource(SourceSpan span, Digest& result, std::uint8_t& mode) const {
    const auto classified = memory.ClassifySource(span.pc);
    if (!span.size || !classified || *classified == SourceMode::Volatile ||
        span.size > memory.CodeAt(span.pc).size())
        return false;
    mode = static_cast<std::uint8_t>(*classified);
    // Hash exactly the bytes consumed by the frontend, through the same safe
    // memory service as direct execution. No decoder or IR walk is repeated.
    Sha256 hash;
    std::array<std::uint8_t, 4096> bytes;
    for (std::uint64_t offset = 0; offset < span.size;) {
        const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(bytes.size(), span.size - offset));
        if (!memory.CopyIn(bytes.data(), span.pc + offset, count)) return false;
        hash.Update({bytes.data(), count});
        offset += count;
    }
    result = hash.Finish();
    return true;
}

bool VM::SourcesMatch(std::span<const SourceRecord> sources) const {
    if (sources.empty()) return false;
    for (const auto& source : sources) {
        Digest actual;
        std::uint8_t mode;
        if (!HashSource(source.span, actual, mode) || mode != source.mode ||
            actual != source.digest)
            return false;
    }
    return true;
}

bool VM::CollectCode() const noexcept {
    return offline_output || (disk_cache && disk_cache->Enabled());
}

void VM::RecordUnit(ir::Scope scope,
                    const backend::CompileResult& compiled,
                    NativeCode code,
                    std::span<const SourceSpan> sources) {
    if (!compiled.relocatable || sources.empty()) return;
    const auto capture = [&] {
        CodeUnit unit;
        unit.pc = PC();
        unit.fp_mode = thread.fp_mode;
        unit.scope = scope;
        unit.code = compiled;
        // Publication owns the moved recovery table. Persist its real metadata,
        // while retaining canonical words rather than observing patched RX code.
        unit.code.faults.assign(code.faults.begin(), code.faults.end());
        unit.sources.reserve(sources.size());
        for (const auto span : sources) {
            SourceRecord source;
            source.span = span;
            if (!HashSource(span, source.digest, source.mode)) {
                if (offline_output)
                    swift::ThrowError("cannot capture AOT source bytes", ErrorKind::GuestFault);
                ++stats.disk_cache_rejects;
                DisablePersistence();
                return;
            }
            unit.sources.push_back(source);
        }
        if (offline_output)
            offline_output->units.push_back(std::move(unit));
        else if (disk_cache && disk_cache->Enabled())
            disk_cache->Record(std::move(unit));
    };
    if (offline_output)
        capture();
    else {
        // Optional storage ownership never becomes a reason to discard an
        // already published, valid native unit.
        try {
            capture();
        } catch (const std::exception&) {
            ++stats.disk_cache_rejects;
            DisablePersistence();
        }
    }
}

void VM::FlushDisk() noexcept {
    if (thread.faults.smc_pending || persistence_disabled) {
        DisablePersistence();
        return;
    }
    if (!disk_cache || !disk_cache->Enabled() || !disk_cache->Dirty()) return;
    RuntimeTimer write{stats.cache_write_ns};
    if (disk_cache->Flush())
        ++stats.disk_cache_writes;
    else {
        ++stats.disk_cache_rejects;
        DisablePersistence();
    }
}

void VM::DisablePersistence() noexcept {
    persistence_disabled = true;
    if (disk_cache) disk_cache->Disable();
}

void VM::InstallImage(const CodeImage& native) {
    if (native.key != image_key)
        swift::ThrowError("AOT build, guest image or native configuration mismatch",
                          ErrorKind::InvalidArgument);
    for (const auto& unit : native.units) {
        if ((!backend.specializes_fp_flush && unit.fp_mode) || unit.fp_mode > 3 ||
            !SourcesMatch(unit.sources))
            swift::ThrowError("AOT source bytes or source policy mismatch",
                              ErrorKind::InvalidArgument);
    }
    const auto initial_pc = PC();
    const auto initial_mxcsr = NumericControl();
    for (const auto& unit : native.units) {
        PC() = unit.pc;
        NumericControl() = (initial_mxcsr & ~UINT32_C(0x8040)) | ((unit.fp_mode & 1u) << 6) |
                           ((unit.fp_mode >> 1) << 15);
        UpdateFPEnvironment();
        auto code = compiler.Restore(unit, true);
        Cache().InsertReady(unit.pc, code, unit.code.entries);
        ++stats.aot_units_loaded;
    }
    PC() = initial_pc;
    NumericControl() = initial_mxcsr;
    UpdateFPEnvironment();
    ObserveCache();
}

}  // namespace swift::runtime::execution
