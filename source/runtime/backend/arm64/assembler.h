#pragma once

#include "encoding.h"
#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace swift::runtime::backend::arm64::detail {
using WordOffset = std::uint32_t;

// One code sink and one label/fixup owner. The exact encoder writes directly
// into the borrowed words; only actual branch uses grow these fixup vectors.
class Assembler {
private:
    static constexpr std::size_t inline_labels = 64;
    std::array<WordOffset, inline_labels> local_labels;
    std::vector<WordOffset> label_storage;

public:
    struct Fixup {
        WordOffset word;
        std::uint32_t target;
    };

    struct LocalFixup {
        std::uint32_t node;
        WordOffset word;
    };

    Assembler(std::vector<std::uint32_t>& words, std::size_t blocks)
            : label_storage(blocks <= inline_labels ? 0 : blocks, UINT32_MAX)
            , exact(words)
            , labels(blocks <= inline_labels ? std::span<WordOffset>{local_labels.data(), blocks}
                                             : std::span<WordOffset>{label_storage})
            , words(words) {
        if (blocks <= inline_labels) std::fill_n(local_labels.data(), blocks, UINT32_MAX);
    }

    Assembler(const Assembler&) = delete;
    Assembler& operator=(const Assembler&) = delete;

    WordOffset Here() const { return static_cast<WordOffset>(words.size()); }

    void Data32(std::uint32_t value) { exact.dc32(value); }

    void Data64(std::uint64_t value) { exact.dc64(value); }

    void Patch(WordOffset from, WordOffset to);

    void Align8() {
        if (Here() & 1) exact.nop();
    }

    vixl::aarch64::Assembler exact;
    std::span<WordOffset> labels;
    std::vector<Fixup> fixups;
    std::vector<LocalFixup> local_fixups;
    WordOffset last_branch_target{};

private:
    std::vector<std::uint32_t>& words;
};
}  // namespace swift::runtime::backend::arm64::detail
