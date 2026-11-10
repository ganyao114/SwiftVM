#include "catalog.h"

#include "base/error.h"

#include <algorithm>

namespace swift::tests::correctness {

Case Reduce(const Case& original,
            const std::function<bool(const Case&)>& predicate,
            std::uint32_t budget) {
    std::string reason;
    if (!ValidateCase(original, &reason))
        swift::ThrowError(reason, swift::ErrorKind::InvalidArgument);
    Case best = original;
    if (!budget || !predicate(best)) return best;
    --budget;
    const auto attempt = [&](Case candidate) {
        if (!budget || !ValidateCase(candidate)) return false;
        --budget;
        if (!predicate(candidate)) return false;
        best = std::move(candidate);
        return true;
    };
    // Keep the region/address/protection topology and the reviewed instruction
    // form. A fresh oracle is required by the caller's original failure predicate.
    for (std::size_t region = 0; region < best.memory.size() && budget; ++region) {
        for (std::size_t chunk = best.memory[region].bytes.size(); chunk && budget; chunk /= 2) {
            for (std::size_t begin = 0; begin < best.memory[region].bytes.size() && budget;
                 begin += chunk) {
                const auto end = std::min(begin + chunk, best.memory[region].bytes.size());
                if (std::all_of(best.memory[region].bytes.begin() + begin,
                                best.memory[region].bytes.begin() + end,
                                [](auto b) { return b == 0; }))
                    continue;
                auto candidate = best;
                std::fill(candidate.memory[region].bytes.begin() + begin,
                          candidate.memory[region].bytes.begin() + end,
                          0);
                attempt(std::move(candidate));
            }
        }
        auto candidate = best;
        auto& bytes = candidate.memory[region].bytes;
        while (!bytes.empty() && !bytes.back()) bytes.pop_back();
        if (bytes.size() != best.memory[region].bytes.size()) attempt(std::move(candidate));
    }
    const unsigned registers = best.isa == ISA::X86_64 ? 16 : best.isa == ISA::Arm64 ? 32 : 15;
    for (unsigned reg = 0; reg < registers && budget; ++reg) {
        constexpr std::uint64_t simple[]{0, 1, UINT64_MAX, UINT64_C(0x80000000)};
        for (auto value : simple) {
            if (!budget) break;
            if (best.isa == ISA::Arm32) value = std::uint32_t(value);
            if (best.initial.gpr[reg] == value) continue;
            auto candidate = best;
            candidate.initial.gpr[reg] = value;
            if (attempt(std::move(candidate))) break;
        }
    }
    const unsigned vectors = best.isa == ISA::Arm64 ? 32 : 16;
    const unsigned bytes = best.isa == ISA::X86_64 ? 32 : 16;
    for (unsigned reg = 0; reg < vectors && budget; ++reg) {
        auto candidate = best;
        std::fill_n(candidate.initial.vectors[reg].begin(), bytes, 0);
        if (candidate.initial.vectors[reg] != best.initial.vectors[reg] &&
            attempt(std::move(candidate)))
            continue;
        for (unsigned byte = 0; byte < bytes && budget; ++byte) {
            if (!best.initial.vectors[reg][byte]) continue;
            candidate = best;
            candidate.initial.vectors[reg][byte] = 0;
            attempt(std::move(candidate));
        }
    }
    for (unsigned bit = 0; bit < 32 && budget; ++bit) {
        if (!(best.initial.flags & (UINT64_C(1) << bit))) continue;
        auto candidate = best;
        candidate.initial.flags &= ~(UINT64_C(1) << bit);
        attempt(std::move(candidate));
    }
    if (budget) {
        auto candidate = best;
        candidate.initial.fp_status = 0;
        candidate.initial.fp_control = best.isa == ISA::X86_64 ? 0x1f80 : 0;
        if (candidate.initial.fp_status != best.initial.fp_status ||
            candidate.initial.fp_control != best.initial.fp_control)
            attempt(std::move(candidate));
    }
    return best;
}

}  // namespace swift::tests::correctness
