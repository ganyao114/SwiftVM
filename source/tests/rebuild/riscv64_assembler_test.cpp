#include <biscuit/assembler.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

TEST_CASE("RV64 backward labels at a full code buffer", "[compile][riscv64-compile]") {
    std::vector<std::uint32_t> words;
    words.reserve(2);
    biscuit::Assembler assembler{words};
    assembler.NOP();
    assembler.NOP();
    biscuit::Label loop;
    assembler.Bind(&loop);
    // Both the bound label and the next instruction are at the old capacity.
    assembler.J(&loop);
    CHECK(words.back() == 0x6f);  // JAL x0, 0.
}

TEST_CASE("RV64 backward labels survive vector growth", "[compile][riscv64-compile]") {
    std::vector<std::uint32_t> words;
    words.reserve(1);
    biscuit::Assembler assembler{words};
    biscuit::Label loop;
    assembler.Bind(&loop);
    assembler.NOP();
    // Every power-of-two boundary reaches the old capacity before J grows it.
    for (unsigned i = 1; i != 257; ++i) {
        assembler.J(&loop);
        const auto word = words.back();
        const auto immediate = ((word >> 31) << 20) | (((word >> 12) & 255) << 12) |
                               (((word >> 20) & 1) << 11) | (((word >> 21) & 1023) << 1);
        const auto displacement = static_cast<std::int32_t>(immediate << 11) >> 11;
        CHECK(displacement == -static_cast<std::int32_t>(i * 4));
        CHECK((word & 0xfff) == 0x6f);  // JAL x0, displacement.
    }
}
