#include "base/logging.h"
#pragma once

#include <fstream>
#include <charconv>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include "base/logging.h"
#include "runtime/common/svm_config.h"

namespace swift::runtime::backend::arm64 {

// Process-level layout experiment. Configuration is frozen and disk caching
// disabled by AddressSpace while enabled, so external input cannot reuse stale
// code. The default path remains compatible with existing experiment scripts.
class PlacementExperiment {
public:
    // Experiments can add at most 64 KiB at one point and cannot target an
    // offset beyond a 1 MiB compilation unit.
    static constexpr u32 kMaxPaddingBytes = 64 * 1024;
    static constexpr u32 kMaxUnitBytes = 1024 * 1024;
    PlacementExperiment(u32 mode, const std::string& path) : mode(mode) {
        if (mode != 1) return;
        std::ifstream input{path};
        if (!input) {
            LOG_WARNING("placement reference unavailable: {}", path);
            return;
        }
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream fields{line};
            std::string point, unit_text, pc_text, offset_text, extra;
            u32 offset{};
            if (!(fields >> point >> unit_text >> pc_text >> offset_text) || fields >> extra) continue;
            const auto parsed = std::from_chars(offset_text.data(),
                    offset_text.data() + offset_text.size(), offset);
            if (parsed.ec != std::errc{} || parsed.ptr != offset_text.data() + offset_text.size() ||
                unit_text.front() == '-' || pc_text.front() == '-') continue;
            try {
                size_t unit_end{}, pc_end{};
                const auto unit = std::stoull(unit_text, &unit_end, 0);
                const auto pc = std::stoull(pc_text, &pc_end, 0);
                if (unit_end != unit_text.size() || pc_end != pc_text.size() ||
                    offset % sizeof(u32) != 0) continue;
                if (offset > kMaxUnitBytes) {
                    LOG_WARNING("placement offset exceeds unit limit: {}", offset);
                    continue;
                }
                references.emplace(Key{point, unit, pc}, offset);
            } catch (const std::exception&) {
                LOG_WARNING("invalid placement reference row: {}", line);
            }
        }
    }

    u32 Padding(const char* kind, u64 unit_pc, u64 guest_pc, u32 before) const {
        if (mode == 0) return 0;
        if (mode == 2) {
            SVM_DIAG_FORMAT(Codegen,
                       "[svm-placement-reference] kind={} unit={:#x} pc={:#x} offset={}\n",
                       kind, unit_pc, guest_pc, before);
            return 0;
        }
        const auto it = references.find(Key{kind, unit_pc, guest_pc});
        if (it == references.end()) {
            SVM_DIAG_FORMAT(Codegen,
                       "[svm-placement-miss] kind={} unit={:#x} pc={:#x} before={}\n",
                       kind, unit_pc, guest_pc, before);
            return 0;
        }
        if (before > it->second) {
            SVM_DIAG_FORMAT(Codegen,
                       "[svm-placement-overrun] kind={} unit={:#x} pc={:#x} before={} target={}\n",
                       kind, unit_pc, guest_pc, before, it->second);
            return 0;
        }
        const auto padding = it->second - before;
        if (padding > kMaxPaddingBytes) {
            LOG_WARNING("placement padding exceeds point limit: {}", padding);
            return 0;
        }
        const u32 ops = padding / sizeof(u32);
        ASSERT_MSG(before + ops * sizeof(u32) == it->second,
                   "placement reference is not instruction aligned");
        if (ops != 0) {
            SVM_DIAG_FORMAT(Codegen,
                       "[svm-placement-pad] kind={} unit={:#x} pc={:#x} before={} after={} ops={}\n",
                       kind, unit_pc, guest_pc, before, before + ops * sizeof(u32), ops);
        }
        return ops;
    }

private:
    using Key = std::tuple<std::string, u64, u64>;
    const u32 mode;
    std::map<Key, u32> references;
};

inline const PlacementExperiment& GetPlacementExperiment() {
    static const PlacementExperiment experiment{GetSvmConfig().placement_pad,
                                                GetSvmConfig().placement_reference};
    return experiment;
}

} // namespace swift::runtime::backend::arm64
