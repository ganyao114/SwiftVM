#pragma once

#include "case.h"

namespace swift::tests::correctness {

enum class Encoding : std::uint8_t {
    X86Register,
    X86ShiftCL,
    X86Memory,
    X86ScalarFP,
    X86Vector,
    A64Shifted,
    A64Register,
    A64Memory,
    A64ScalarFP,
    A64Vector,
    A32Register,
    A32ShiftRegister,
    A32Memory,
    A32ScalarFP,
    A32Vector,
    Thumb16Register,
    Thumb16Shift,
    Thumb32Register,
};

struct Form {
    std::string_view id;
    ISA isa;
    std::string_view category;
    Encoding encoding;
    std::uint32_t bits{}, mask{};
    std::uint8_t width{}, length{};
    std::uint32_t boundary_cases{32};
};

// These are reviewed forms, not a claim that the complete ISA is catalogued.
std::span<const Form> Catalog();
const Form* FindForm(std::string_view);
Case Generate(std::string_view form_id, std::uint64_t seed, std::uint64_t id);
bool ValidateCase(const Case&, std::string* reason = nullptr);

struct ComparisonContract {
    std::uint64_t flags_mask{};
    std::uint32_t fp_control_mask{}, fp_status_mask{};
};

// Derived only from independently reviewed form/input rules. It never consults
// the decoder, IR, optimizer, or the outputs being compared.
ComparisonContract Contract(const Case&);

}  // namespace swift::tests::correctness
