#include "runtime/backend/loongarch64/assembler.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace la = swift::runtime::backend::loongarch64;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

namespace {
u64 checks{};
void Check(bool condition, const std::string& detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}
u32 Word(const la::Assembler& assembler, size_t offset) {
    u32 word{};
    std::memcpy(&word, assembler.Data() + offset, sizeof(word));
    return word;
}
template <typename Function>
bool Rejected(Function&& function) {
    try { function(); }
    catch (const std::exception&) { return true; }
    return false;
}

void AllBranchInverses() {
    using Branch = void (la::Assembler::*)(la::GPR, la::GPR, la::Label*);
    const std::array<Branch, 6> branches{&la::Assembler::BEQ, &la::Assembler::BNE,
        &la::Assembler::BLT, &la::Assembler::BGE, &la::Assembler::BLTU, &la::Assembler::BGEU};
    // Fixed GNU binutils 2.44 encodings for inverse short branches with
    // rj=a0/rd=a1, byte displacement 8, and a long branch to the next PC.
    const std::array<u32, 6> expected{0x5c000885, 0x58000885, 0x64000885,
                                    0x60000885, 0x6c000885, 0x68000885};
    for (size_t index = 0; index < branches.size(); ++index) {
        la::Assembler assembler{4};
        la::Label target;
        (assembler.*branches[index])(la::a0, la::a1, &target);
        assembler.Bind(&target); assembler.Finalize();
        Check(assembler.Size() == 8 && Word(assembler, 0) == expected[index] &&
              Word(assembler, 4) == 0x50000400, "all conditional jumps use the correct inverse and local skip");
        const auto size = assembler.Size();
        Check(Rejected([&] { (assembler.*branches[index])(la::a0, la::a1, nullptr); }) &&
              assembler.Size() == size, "null branch target leaves the buffer unchanged");
        Check(Rejected([&] { (assembler.*branches[index])(la::GPR{32}, la::a1, &target); }) &&
              assembler.Size() == size, "invalid conditional register leaves the buffer unchanged");
    }
}

void PCRelativeRangeChecks() {
    constexpr u32 range = 1u << 21;
    for (bool negative : {false, true}) {
        for (bool beyond : {false, true}) {
            const bool rejected = Rejected([&] {
                la::Assembler assembler{4};
                la::Label target;
                if (negative) assembler.Bind(&target);
                else assembler.LILabel(la::a0, &target);
                const u32 displacement = negative ? range + (beyond ? 4 : 0) : range - (beyond ? 0 : 4);
                while (assembler.Size() < displacement) assembler.dd(0x03400000);
                if (negative) assembler.LILabel(la::a0, &target);
                else assembler.Bind(&target);
                assembler.Finalize();
                const auto instruction_offset = negative ? displacement : 0;
                const auto word = Word(assembler, instruction_offset);
                const auto raw = u64((word >> 5) & 0xfffff);
                const auto sign = u64{1} << 19;
                const auto immediate = static_cast<std::int64_t>((raw ^ sign) - sign);
                Check(immediate * 4 == (negative ? -std::int64_t(displacement) : std::int64_t(displacement)),
                      "pcaddi preserves the exact displacement at its signed range boundary");
            });
            Check(rejected == beyond, "pcaddi fails closed outside its signed +/-2 MiB encoding range");
        }
    }
    la::Assembler assembler;
    la::Label missing;
    assembler.J(&missing);
    Check(Rejected([&] { assembler.Finalize(); }), "unbound labels cannot be finalized");
}

void IndependentLBTDomains() {
    auto arm = la::HostFeatures::FromHWCAP(u64{1} << 11);
    auto x86 = la::HostFeatures::FromHWCAP(u64{1} << 10);
    la::Assembler arm_only{4, arm}, x86_only{4, x86};
    Check(Rejected([&] { arm_only.X86ADD_D(la::a0, la::a1); }) && arm_only.Size() == 0,
          "ARM LBT capability does not admit x86 LBT lowering");
    Check(Rejected([&] { x86_only.ARMADD_W(la::a0, la::a1, 14); }) && x86_only.Size() == 0,
          "x86 LBT capability does not admit ARM LBT lowering");
    Check(Rejected([&] { arm_only.ARMADD_W(la::GPR{32}, la::a1, 14); }) && arm_only.Size() == 0,
          "LBT register validation rejects an unencodable operand");
    Check(Rejected([&] { x86_only.SETX86J(la::a0, 16); }) && x86_only.Size() == 0,
          "LBT condition validation rejects truncation");
}
}  // namespace

int main() {
    try {
        AllBranchInverses(); PCRelativeRangeChecks(); IndependentLBTDomains();
        std::cout << "PASS LoongArch assembler range and admission checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL LoongArch assembler: " << error.what() << '\n';
        return 1;
    }
}
