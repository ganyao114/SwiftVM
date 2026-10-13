#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "runtime/backend/loongarch64/assembler.h"

namespace la = swift::runtime::backend::loongarch64;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

namespace {
u64 checks{};
void Check(bool condition, const std::string& description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}
u32 Word(const la::Assembler& assembler, size_t offset) {
    u32 word{};
    Check(offset + sizeof(word) <= assembler.Size(), "instruction is in the emitted buffer");
    std::memcpy(&word, assembler.Data() + offset, sizeof(word));
    return word;
}
std::int64_t Signed(u32 value, u32 bits) {
    const u64 sign = u64{1} << (bits - 1);
    return std::bit_cast<std::int64_t>((u64(value) ^ sign) - sign);
}

// Interpret only the architectural constant-building instructions. This is
// independent of LI's decisions about which sequence to use.
u64 EvaluateLI(const la::Assembler& assembler) {
    std::array<u64, 32> regs{};
    for (size_t offset = 0; offset < assembler.Size(); offset += 4) {
        const u32 word = Word(assembler, offset), rd = word & 31, rj = (word >> 5) & 31;
        const u32 imm12 = (word >> 10) & 4095, imm20 = (word >> 5) & 0xfffff;
        if ((word & 0xffc00000) == 0x02c00000) regs[rd] = regs[rj] + Signed(imm12, 12);
        else if ((word & 0xffc00000) == 0x03800000) regs[rd] = regs[rj] | imm12;
        else if ((word & 0xfe000000) == 0x14000000) regs[rd] = u64(Signed(imm20, 20)) << 12;
        else if ((word & 0xfe000000) == 0x16000000)
            regs[rd] = (regs[rd] & 0xffffffff) | (u64(Signed(imm20, 20)) << 32);
        else if ((word & 0xffc00000) == 0x03000000)
            regs[rd] = (regs[rj] & 0x000fffffffffffffULL) | (u64(imm12) << 52);
        else throw std::runtime_error("LI emitted an unmodeled instruction");
        regs[0] = 0;
    }
    return regs[4];
}

void ConstantEncodings() {
    const std::array<u64, 16> edges{0, 1, 2047, 2048, 4095, 4096, u64(-2048), u64(-2049),
        0x7fffffff, 0x80000000, 0xffffffff, 0x80000000000ULL, 0x8000000000000ULL,
        0x8000000000000000ULL, 0x0123456789abcdefULL, UINT64_MAX};
    u64 seed = 0xa55a4c413634ULL;
    for (u32 sample = 0; sample < 4096; ++sample) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        const u64 value = sample < edges.size() ? edges[sample] : seed;
        la::Assembler assembler{4};
        assembler.LI(la::a0, value); assembler.Finalize();
        Check(assembler.Size() <= 16 && EvaluateLI(assembler) == value,
              "independent LI encoding model: sample=" + std::to_string(sample));
    }
    la::Assembler assembler;
    assembler.add_d(la::a0, la::a1, la::a2);
    assembler.jirl(la::zero, la::ra, 0);
    assembler.Finalize();
    Check(Word(assembler, 0) == 0x001098a4 && Word(assembler, 4) == 0x4c000020,
          "fixed GNU assembler baseline add.d and return encodings");
}

void LabelsAndGrowth() {
    la::Assembler assembler{4};
    la::Label end;
    assembler.BEQ(la::a0, la::a1, &end);
    const auto address_offset = assembler.Size();
    assembler.LILabel(la::a2, &end);
    for (u32 i = 0; i < 40000; ++i) assembler.addi_d(la::zero, la::zero, 0);
    assembler.Bind(&end);
    const auto target_offset = assembler.Size();
    assembler.Finalize();
    const u32 branch = Word(assembler, 4);
    const u32 branch_imm = ((branch & 0x3ff) << 16) | ((branch >> 10) & 0xffff);
    Check(Signed(branch_imm, 26) * 4 == std::int64_t(target_offset) - 4,
          "conditional branch reaches beyond the short-branch range");
    Check(Signed((Word(assembler, address_offset) >> 5) & 0xfffff, 20) * 4 ==
          std::int64_t(target_offset) - std::int64_t(address_offset), "pcaddi label remains correct across autogrowth");
    const u32 first_word = Word(assembler, 0);
    la::Label after;
    assembler.J(&after); assembler.LI(la::a0, UINT64_MAX); assembler.Bind(&after);
    assembler.Finalize(); assembler.Finalize();
    Check(Word(assembler, 0) == first_word, "append and repeated Finalize preserve published unit encoding");
}

void LBTEncodingsAndAdmission() {
    const auto all = la::HostFeatures::FromHWCAP((u64{1} << 2) | (u64{1} << 4) | (u64{1} << 5) |
        (u64{1} << 6) | (u64{1} << 10) | (u64{1} << 11) | (u64{1} << 12));
    Check(all.ual && all.lsx && all.lasx && all.crc32 && all.lbt_arm && all.lbt_x86 && all.lbt_mips,
          "Linux HWCAP extension mapping");
    const auto none = la::HostFeatures::FromHWCAP(0);
    Check(!none.lsx && !none.lasx && !none.lbt_arm && !none.lbt_x86 && !none.lbt_mips,
          "baseline HWCAP admits no optional SIMD or translation extension");
    for (u32 policy = 0; policy < 8; ++policy) {
        const auto allowed = all.WithPolicy(policy & 1, policy & 2, policy & 4);
        Check(allowed.lsx == bool(policy & 1) && allowed.lasx == bool(policy & 2) &&
              allowed.crc32 == bool(policy & 4), "each optional feature obeys its independent policy bit");
        Check(allowed.ual && allowed.lbt_arm && allowed.lbt_x86 && allowed.lbt_mips,
              "SIMD/CRC policy preserves unrelated HWCAP capabilities");
        const auto absent = none.WithPolicy(policy & 1, policy & 2, policy & 4);
        Check(!absent.lsx && !absent.lasx && !absent.crc32,
              "feature policy cannot enable capabilities absent from HWCAP");
    }
    la::Assembler disabled;
    bool arm_rejected{}, x86_rejected{};
    try { disabled.ARMADD_W(la::a0, la::a1, 14); } catch (const std::logic_error&) { arm_rejected = true; }
    try { disabled.X86ADD_D(la::a0, la::a1); } catch (const std::logic_error&) { x86_rejected = true; }
    Check(arm_rejected && x86_rejected && disabled.Size() == 0, "LBT encoding is rejected before feature admission");
    la::Assembler admitted{4096, all};
    admitted.ARMADD_W(la::a0, la::a1, 14);
    admitted.ARMMFFLAG(la::a2, 15);
    admitted.SETARMJ(la::a3, 0);
    admitted.X86ADD_D(la::a4, la::a5);
    admitted.Finalize();
    const std::array<u32, 4> expected{0x0037149e, 0x005c3c46, 0x0036c007, 0x003f2507};
    for (u32 i = 0; i < expected.size(); ++i)
        Check(Word(admitted, i * 4) == expected[i], "fixed GNU binutils 2.44 LBT encoding");
    bool bad_condition{}, bad_mask{};
    try { admitted.ARMADD_W(la::a0, la::a1, 16); } catch (const std::invalid_argument&) { bad_condition = true; }
    try { admitted.X86MFFLAG(la::a0, 256); } catch (const std::invalid_argument&) { bad_mask = true; }
    Check(bad_condition && bad_mask && admitted.Size() == 16, "LBT immediate rejection leaves code unchanged");
    // QEMU TCG does not implement LBT. These assertions check encoding and
    // admission only; executing the hardware path remains a real-host test.
}

#if defined(__linux__) && defined(__loongarch__)
void ExecuteConstants() {
    const auto page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    u64 seed = 0x4a49544c413634ULL;
    for (u32 i = 0; i < 256; ++i) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        la::Assembler assembler;
        assembler.LI(la::a0, seed); assembler.jirl(la::zero, la::ra, 0); assembler.Finalize();
        auto* page = static_cast<char*>(mmap(nullptr, page_size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        Check(page != MAP_FAILED, "allocate assembler execution page");
        std::memcpy(page, assembler.Data(), assembler.Size());
        __builtin___clear_cache(page, page + assembler.Size());
        if (mprotect(page, page_size, PROT_READ | PROT_EXEC) != 0) {
            munmap(page, page_size); throw std::runtime_error("publish assembler execution page");
        }
        const auto observed = reinterpret_cast<u64 (*)()>(page)();
        munmap(page, page_size);
        Check(observed == seed, "executed constant builder obeys the LoongArch C ABI");
    }
}
#endif
}  // namespace

int main() {
    try {
        ConstantEncodings(); LabelsAndGrowth(); LBTEncodingsAndAdmission();
#if defined(__linux__) && defined(__loongarch__)
        ExecuteConstants();
        std::cout << "PASS native assembler execution; ";
#else
        std::cout << "PASS cross-host assembler encoding; ";
#endif
        std::cout << checks << " checks; LBT encoded, not executed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL assembler: " << error.what() << '\n'; return 1;
    }
}
