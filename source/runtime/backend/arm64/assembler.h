#pragma once

#include "encoding.h"
#include <span>
#include <vector>

namespace swift::runtime::backend::arm64::detail {
using WordOffset = std::uint32_t;

// One code sink and one label/fixup owner. The exact encoder writes directly
// into the borrowed words; only actual branch uses grow these fixup vectors.
class Assembler {
private:
    WordOffset single_label{UINT32_MAX};
    std::vector<WordOffset> label_storage;
public:
    struct Fixup { WordOffset word; std::uint32_t target; };
    struct LocalFixup { std::uint32_t node; WordOffset word; };
    Assembler(std::vector<std::uint32_t>& words, std::size_t blocks)
            : label_storage(blocks == 1 ? 0 : blocks, UINT32_MAX), exact(words),
              labels(blocks == 1 ? std::span<WordOffset>{&single_label, 1} : std::span<WordOffset>{label_storage}),
              words(words) {}
    Assembler(const Assembler&) = delete;
    Assembler& operator=(const Assembler&) = delete;
    WordOffset Here() const { return static_cast<WordOffset>(words.size()); }
    void Data32(std::uint32_t value) { exact.dc32(value); }
    void Data64(std::uint64_t value) { exact.dc64(value); }
    void Patch(WordOffset from, WordOffset to);
    void Align8() { if (Here() & 1) exact.nop(); }
    vixl::aarch64::Assembler exact;
    std::span<WordOffset> labels;
    std::vector<Fixup> fixups;
    std::vector<LocalFixup> local_fixups;
private:
    std::vector<std::uint32_t>& words;
};
} // namespace swift::runtime::backend::arm64::detail
