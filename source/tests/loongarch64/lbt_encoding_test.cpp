#include "runtime/backend/loongarch64/assembler.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace la = swift::runtime::backend::loongarch64;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

namespace {
u64 checks{};
void Check(bool condition, const std::string& detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}
u32 Word(const la::Assembler& assembler) {
    Check(assembler.Size() == 4, "one LBT instruction occupies one word");
    u32 result{};
    std::memcpy(&result, assembler.Data(), sizeof(result));
    return result;
}
template <typename Exception, typename Function>
void RejectUnchanged(la::Assembler& assembler, Function&& function, const std::string& detail) {
    const auto before = assembler.Size();
    bool rejected{};
    try { function(); }
    catch (const Exception&) { rejected = true; }
    Check(rejected && assembler.Size() == before, detail);
}
la::HostFeatures Domain(bool arm) {
    return la::HostFeatures::FromHWCAP(u64{1} << (arm ? 11 : 10));
}

// Golden words were produced with GNU binutils 2.44, not SwiftVM's encoder:
//   loongarch64-unknown-linux-gnu-as reference.S -o reference.o
//   loongarch64-unknown-linux-gnu-objcopy -O binary -j .text reference.o reference.bin
// ARM operands are "$a0, $a1, 14", x86 operands "$a0, $a1";
// transfers use "$a0, 255", conditions "$a0, 15". The fixture contains
// every currently exposed LBT method. No emitted LBT instruction is executed.
struct ARM {
    const char* name;
    void (la::Assembler::*emit)(la::GPR, la::GPR, u32);
    u32 golden;
};
constexpr std::array<ARM, 7> arm_methods{{
    {"armadd.w", &la::Assembler::ARMADD_W, 0x0037149e},
    {"armsub.w", &la::Assembler::ARMSUB_W, 0x0037949e},
    {"armadc.w", &la::Assembler::ARMADC_W, 0x0038149e},
    {"armsbc.w", &la::Assembler::ARMSBC_W, 0x0038949e},
    {"armand.w", &la::Assembler::ARMAND_W, 0x0039149e},
    {"armor.w", &la::Assembler::ARMOR_W, 0x0039949e},
    {"armxor.w", &la::Assembler::ARMXOR_W, 0x003a149e},
}};
struct X86 {
    const char* name;
    void (la::Assembler::*emit)(la::GPR, la::GPR);
    u32 golden;
};
constexpr std::array<X86, 28> x86_methods{{
    {"x86add.b", &la::Assembler::X86ADD_B, 0x003f1484},
    {"x86add.h", &la::Assembler::X86ADD_H, 0x003f1485},
    {"x86add.w", &la::Assembler::X86ADD_W, 0x003f1486},
    {"x86add.d", &la::Assembler::X86ADD_D, 0x003f1487},
    {"x86sub.b", &la::Assembler::X86SUB_B, 0x003f1488},
    {"x86sub.h", &la::Assembler::X86SUB_H, 0x003f1489},
    {"x86sub.w", &la::Assembler::X86SUB_W, 0x003f148a},
    {"x86sub.d", &la::Assembler::X86SUB_D, 0x003f148b},
    {"x86adc.b", &la::Assembler::X86ADC_B, 0x003f148c},
    {"x86adc.h", &la::Assembler::X86ADC_H, 0x003f148d},
    {"x86adc.w", &la::Assembler::X86ADC_W, 0x003f148e},
    {"x86adc.d", &la::Assembler::X86ADC_D, 0x003f148f},
    {"x86sbc.b", &la::Assembler::X86SBC_B, 0x003f1490},
    {"x86sbc.h", &la::Assembler::X86SBC_H, 0x003f1491},
    {"x86sbc.w", &la::Assembler::X86SBC_W, 0x003f1492},
    {"x86sbc.d", &la::Assembler::X86SBC_D, 0x003f1493},
    {"x86and.b", &la::Assembler::X86AND_B, 0x003f9490},
    {"x86and.h", &la::Assembler::X86AND_H, 0x003f9491},
    {"x86and.w", &la::Assembler::X86AND_W, 0x003f9492},
    {"x86and.d", &la::Assembler::X86AND_D, 0x003f9493},
    {"x86or.b", &la::Assembler::X86OR_B, 0x003f9494},
    {"x86or.h", &la::Assembler::X86OR_H, 0x003f9495},
    {"x86or.w", &la::Assembler::X86OR_W, 0x003f9496},
    {"x86or.d", &la::Assembler::X86OR_D, 0x003f9497},
    {"x86xor.b", &la::Assembler::X86XOR_B, 0x003f9498},
    {"x86xor.h", &la::Assembler::X86XOR_H, 0x003f9499},
    {"x86xor.w", &la::Assembler::X86XOR_W, 0x003f949a},
    {"x86xor.d", &la::Assembler::X86XOR_D, 0x003f949b},
}};
struct Single {
    const char* name;
    void (la::Assembler::*emit)(la::GPR, u32);
    u32 golden;
    bool arm;
    u32 bits;
};
constexpr std::array<Single, 6> single_methods{{
    {"armmfflag", &la::Assembler::ARMMFFLAG, 0x005ffc44, true, 8},
    {"armmtflag", &la::Assembler::ARMMTFLAG, 0x005ffc64, true, 8},
    {"x86mfflag", &la::Assembler::X86MFFLAG, 0x005ffc04, false, 8},
    {"x86mtflag", &la::Assembler::X86MTFLAG, 0x005ffc24, false, 8},
    {"setarmj", &la::Assembler::SETARMJ, 0x0036fc04, true, 4},
    {"setx86j", &la::Assembler::SETX86J, 0x0036bc04, false, 4},
}};

void CompleteGoldenFixture(const char* reference_path) {
    la::Assembler assembler{4, la::HostFeatures::FromHWCAP((u64{1} << 10) | (u64{1} << 11))};
    size_t index{};
    const auto check = [&](u32 golden, const char* name) {
        u32 actual{};
        std::memcpy(&actual, assembler.Data() + (index++) * 4, 4);
        Check(actual == golden, std::string{name} + " matches GNU binutils 2.44");
    };
    for (const auto& method : arm_methods) {
        (assembler.*method.emit)(la::a0, la::a1, 14); check(method.golden, method.name);
    }
    for (const auto& method : x86_methods) {
        (assembler.*method.emit)(la::a0, la::a1); check(method.golden, method.name);
    }
    for (const auto& method : single_methods) {
        (assembler.*method.emit)(la::a0, (1u << method.bits) - 1); check(method.golden, method.name);
    }
    assembler.Finalize();
    Check(index == 41 && assembler.Size() == 164, "all 41 public LBT methods are covered");
    if (reference_path) {
        std::ifstream input{reference_path, std::ios::binary};
        Check(input.good(), "read the separately assembled GNU fixture");
        const std::vector<char> reference{std::istreambuf_iterator<char>{input}, {}};
        Check(reference.size() == assembler.Size() &&
              std::memcmp(reference.data(), assembler.Data(), reference.size()) == 0,
              "complete emitter buffer matches the separately assembled GNU fixture");
    }
}

void ArithmeticOperandsAndAdmission() {
    for (const auto& method : arm_methods) {
        for (u32 reg = 0; reg < 32; ++reg)
        for (u32 condition = 0; condition < 16; ++condition) {
            la::Assembler assembler{4, Domain(true)};
            (assembler.*method.emit)(la::GPR{reg}, la::GPR{31 - reg}, condition);
            const auto opcode = method.golden & ~u32(0x7fef);
            Check(Word(assembler) == (opcode | (reg << 5) | ((31 - reg) << 10) | condition),
                  std::string{method.name} + " preserves both register fields and every condition");
        }
        for (auto features : {la::HostFeatures{}, Domain(false), la::HostFeatures::FromHWCAP(u64{1} << 12)}) {
            la::Assembler assembler{4, features};
            RejectUnchanged<std::logic_error>(assembler, [&] { (assembler.*method.emit)(la::a0, la::a1, 14); },
                std::string{method.name} + " requires the ARM LBT domain");
        }
        la::Assembler assembler{4, Domain(true)};
        for (u32 invalid : {32u, UINT32_MAX}) {
            RejectUnchanged<std::invalid_argument>(assembler, [&] { (assembler.*method.emit)(la::GPR{invalid}, la::a1, 14); },
                std::string{method.name} + " rejects the left register without appending");
            RejectUnchanged<std::invalid_argument>(assembler, [&] { (assembler.*method.emit)(la::a0, la::GPR{invalid}, 14); },
                std::string{method.name} + " rejects the right register without appending");
        }
        for (u32 invalid : {16u, UINT32_MAX})
            RejectUnchanged<std::invalid_argument>(assembler, [&] { (assembler.*method.emit)(la::a0, la::a1, invalid); },
                std::string{method.name} + " rejects a condition outside four bits");
    }
    for (const auto& method : x86_methods) {
        for (u32 reg = 0; reg < 32; ++reg) {
            la::Assembler assembler{4, Domain(false)};
            (assembler.*method.emit)(la::GPR{reg}, la::GPR{31 - reg});
            Check(Word(assembler) == ((method.golden & ~u32(0x7fe0)) | (reg << 5) | ((31 - reg) << 10)),
                  std::string{method.name} + " preserves both register fields and the size selector");
        }
        for (auto features : {la::HostFeatures{}, Domain(true), la::HostFeatures::FromHWCAP(u64{1} << 12)}) {
            la::Assembler assembler{4, features};
            RejectUnchanged<std::logic_error>(assembler, [&] { (assembler.*method.emit)(la::a0, la::a1); },
                std::string{method.name} + " requires the x86 LBT domain");
        }
        la::Assembler assembler{4, Domain(false)};
        for (u32 invalid : {32u, UINT32_MAX}) {
            RejectUnchanged<std::invalid_argument>(assembler, [&] { (assembler.*method.emit)(la::GPR{invalid}, la::a1); },
                std::string{method.name} + " rejects the left register without appending");
            RejectUnchanged<std::invalid_argument>(assembler, [&] { (assembler.*method.emit)(la::a0, la::GPR{invalid}); },
                std::string{method.name} + " rejects the right register without appending");
        }
    }
}

void MasksConditionsAndAdmission() {
    for (const auto& method : single_methods) {
        const auto limit = 1u << method.bits;
        for (u32 immediate = 0; immediate < limit; ++immediate)
        for (u32 reg : {0u, 4u, 31u}) {
            la::Assembler assembler{4, Domain(method.arm)};
            (assembler.*method.emit)(la::GPR{reg}, immediate);
            const auto opcode = method.golden & ~u32(((limit - 1) << 10) | 31);
            Check(Word(assembler) == (opcode | (immediate << 10) | reg),
                  std::string{method.name} + " preserves every legal immediate and register edge");
        }
        for (auto features : {la::HostFeatures{}, Domain(!method.arm), la::HostFeatures::FromHWCAP(u64{1} << 12)}) {
            la::Assembler assembler{4, features};
            RejectUnchanged<std::logic_error>(assembler, [&] { (assembler.*method.emit)(la::a0, limit - 1); },
                std::string{method.name} + " rejects disabled and foreign domains");
        }
        la::Assembler assembler{4, Domain(method.arm)};
        for (u32 invalid : {limit, UINT32_MAX})
            RejectUnchanged<std::invalid_argument>(assembler, [&] { (assembler.*method.emit)(la::a0, invalid); },
                std::string{method.name} + " rejects an overflowing immediate without truncation");
        for (u32 invalid : {32u, UINT32_MAX})
            RejectUnchanged<std::invalid_argument>(assembler, [&] { (assembler.*method.emit)(la::GPR{invalid}, 0); },
                std::string{method.name} + " rejects an invalid register without appending");
    }
}
}  // namespace

int main(int argc, char** argv) {
    try {
        Check(argc == 1 || argc == 2, "optional argument names an independently assembled GNU binary fixture");
        CompleteGoldenFixture(argc == 2 ? argv[1] : nullptr);
        ArithmeticOperandsAndAdmission(); MasksConditionsAndAdmission();
        std::cout << "PASS all 41 LBT encodings, complete conditions/masks and domain rejection checks="
                  << checks << "; LBT encoded, not executed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL LBT encoding: " << error.what() << '\n'; return 1;
    }
}
