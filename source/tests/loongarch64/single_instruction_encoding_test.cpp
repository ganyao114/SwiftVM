#include "test_support.h"
#include "runtime/backend/loongarch64/guest_access.h"

#include <array>
#include <vector>

namespace swift::tests::loongarch64 {

void SingleInstructionEncodings() {
    const auto before = checks;
    la::Assembler as;
    std::vector<la::GuestAccessRange> sites;
    const auto add = [&](auto emit, u32 width, la::GuestAccessKind kind, u8 base, s32 displacement) {
        const auto begin = static_cast<u32>(as.Size());
        emit();
        sites.push_back({begin, static_cast<u32>(as.Size()), width, kind, base, displacement});
    };
    for (u8 base : {u8{16}, u8{18}}) for (s32 offset : {-2048, -1, 0, 7, 2047}) {
        const la::GPR address{base};
        add([&] { as.ld_bu(la::t0, address, offset); }, 1, la::GuestAccessKind::Read, base, offset);
        add([&] { as.ld_hu(la::t0, address, offset); }, 2, la::GuestAccessKind::Read, base, offset);
        add([&] { as.ld_wu(la::t0, address, offset); }, 4, la::GuestAccessKind::Read, base, offset);
        add([&] { as.ld_d(la::t0, address, offset); }, 8, la::GuestAccessKind::Read, base, offset);
        add([&] { as.st_b(la::t0, address, offset); }, 1, la::GuestAccessKind::Write, base, offset);
        add([&] { as.st_h(la::t0, address, offset); }, 2, la::GuestAccessKind::Write, base, offset);
        add([&] { as.st_w(la::t0, address, offset); }, 4, la::GuestAccessKind::Write, base, offset);
        add([&] { as.st_d(la::t0, address, offset); }, 8, la::GuestAccessKind::Write, base, offset);
    }
    for (s32 offset : {-32768, -4, 0, 4, 32764}) {
        add([&] { as.ll_w(la::t0, la::t4, offset); }, 4, la::GuestAccessKind::Read, 16, offset);
        add([&] { as.ll_d(la::t0, la::t4, offset); }, 8, la::GuestAccessKind::Read, 16, offset);
        add([&] { as.sc_w(la::t3, la::t4, offset); }, 4, la::GuestAccessKind::Write, 16, offset);
        add([&] { as.sc_d(la::t3, la::t4, offset); }, 8, la::GuestAccessKind::Write, 16, offset);
    }
#define ATOMIC(word, wide) \
    add([&] { as.word(la::t0, la::t1, la::t4); }, 4, la::GuestAccessKind::ReadWrite, 16, 0); \
    add([&] { as.wide(la::t0, la::t1, la::t4); }, 8, la::GuestAccessKind::ReadWrite, 16, 0)
    ATOMIC(amswap_db_w, amswap_db_d);
    ATOMIC(amadd_db_w, amadd_db_d);
    ATOMIC(amand_db_w, amand_db_d);
    ATOMIC(amor_db_w, amor_db_d);
    ATOMIC(amxor_db_w, amxor_db_d);
    ATOMIC(ammax_db_w, ammax_db_d);
    ATOMIC(ammin_db_w, ammin_db_d);
    ATOMIC(ammax_db_wu, ammax_db_du);
    ATOMIC(ammin_db_wu, ammin_db_du);
#undef ATOMIC
    const auto fake_begin = static_cast<u32>(as.Size());
    as.add_d(la::t0, la::t6, la::t1);
    const auto state_begin = static_cast<u32>(as.Size());
    as.ld_d(la::t0, la::state, 0);
    as.Finalize();
    const auto bytes = std::span<const u8>{as.Data(), as.Size()};
    // GNU binutils 2.44: the complete scalar/LLSC/AMO fixture uses the same
    // registers and offsets as above. These words come from GNU as/objcopy,
    // independently of both SwiftVM's encoder and the metadata decoder.
    constexpr std::array<u32, 120> native_golden{
        0x2a20020cu, 0x2a60020cu, 0x2aa0020cu, 0x28e0020cu, 0x2920020cu, 0x2960020cu, 0x29a0020cu, 0x29e0020cu,
        0x2a3ffe0cu, 0x2a7ffe0cu, 0x2abffe0cu, 0x28fffe0cu, 0x293ffe0cu, 0x297ffe0cu, 0x29bffe0cu, 0x29fffe0cu,
        0x2a00020cu, 0x2a40020cu, 0x2a80020cu, 0x28c0020cu, 0x2900020cu, 0x2940020cu, 0x2980020cu, 0x29c0020cu,
        0x2a001e0cu, 0x2a401e0cu, 0x2a801e0cu, 0x28c01e0cu, 0x29001e0cu, 0x29401e0cu, 0x29801e0cu, 0x29c01e0cu,
        0x2a1ffe0cu, 0x2a5ffe0cu, 0x2a9ffe0cu, 0x28dffe0cu, 0x291ffe0cu, 0x295ffe0cu, 0x299ffe0cu, 0x29dffe0cu,
        0x2a20024cu, 0x2a60024cu, 0x2aa0024cu, 0x28e0024cu, 0x2920024cu, 0x2960024cu, 0x29a0024cu, 0x29e0024cu,
        0x2a3ffe4cu, 0x2a7ffe4cu, 0x2abffe4cu, 0x28fffe4cu, 0x293ffe4cu, 0x297ffe4cu, 0x29bffe4cu, 0x29fffe4cu,
        0x2a00024cu, 0x2a40024cu, 0x2a80024cu, 0x28c0024cu, 0x2900024cu, 0x2940024cu, 0x2980024cu, 0x29c0024cu,
        0x2a001e4cu, 0x2a401e4cu, 0x2a801e4cu, 0x28c01e4cu, 0x29001e4cu, 0x29401e4cu, 0x29801e4cu, 0x29c01e4cu,
        0x2a1ffe4cu, 0x2a5ffe4cu, 0x2a9ffe4cu, 0x28dffe4cu, 0x291ffe4cu, 0x295ffe4cu, 0x299ffe4cu, 0x29dffe4cu,
        0x2080020cu, 0x2280020cu, 0x2180020fu, 0x2380020fu, 0x20fffe0cu, 0x22fffe0cu, 0x21fffe0fu, 0x23fffe0fu,
        0x2000020cu, 0x2200020cu, 0x2100020fu, 0x2300020fu, 0x2000060cu, 0x2200060cu, 0x2100060fu, 0x2300060fu,
        0x207ffe0cu, 0x227ffe0cu, 0x217ffe0fu, 0x237ffe0fu, 0x3869360cu, 0x3869b60cu, 0x386a360cu, 0x386ab60cu,
        0x386b360cu, 0x386bb60cu, 0x386c360cu, 0x386cb60cu, 0x386d360cu, 0x386db60cu, 0x386e360cu, 0x386eb60cu,
        0x386f360cu, 0x386fb60cu, 0x3870360cu, 0x3870b60cu, 0x3871360cu, 0x3871b60cu, 0x0010b64cu, 0x28c002ecu,
    };
    Check(bytes.size() == sizeof(native_golden), "GNU fixture covers every emitted native metadata instruction");
    for (u32 index = 0; index < native_golden.size(); ++index) {
        u32 actual{};
        std::memcpy(&actual, bytes.data() + index * 4, 4);
        Check(actual == native_golden[index], "complete scalar, LLSC and AMO encoding matches GNU binutils");
    }
    for (const auto& valid : sites) {
        Check(la::GuestAccessEncodingValid(bytes, valid), "native site matches its independently decoded word");
        for (u32 mutation = 0; mutation < 6; ++mutation) {
            auto invalid = valid;
            switch (mutation) {
                case 0: invalid.end_offset += 4; break;
                case 1: ++invalid.begin_offset; break;
                case 2: invalid.size = valid.size == 8 ? 4 : 8; break;
                case 3: invalid.address_gpr = valid.address_gpr == 16 ? 18 : 16; break;
                case 4: invalid.kind = valid.kind == la::GuestAccessKind::Read ?
                                la::GuestAccessKind::Write : la::GuestAccessKind::Read; break;
                case 5: ++invalid.displacement; break;
            }
            Check(!la::GuestAccessEncodingValid(bytes, invalid), "malformed native site fails closed");
        }
    }
    Check(!la::GuestAccessEncodingValid(bytes, {fake_begin, fake_begin + 4, 8,
                  la::GuestAccessKind::Read, 18, 0}), "ALU instruction cannot be declared a guest access");
    Check(!la::GuestAccessEncodingValid(bytes, {state_begin, state_begin + 4, 8,
                  la::GuestAccessKind::Read, 23, 0}), "State anchor cannot be declared guest memory");
    Check(!la::GuestAccessEncodingValid(bytes, {UINT32_MAX - 3, 0, 8,
                  la::GuestAccessKind::Read, 18, 0}), "native offset overflow fails before reading code");
    // Golden words from GNU binutils, independent of both Xbyak and the decoder.
    const std::array<u32, 4> golden{0x28c0024c, 0x2980224c, 0x2000020c, 0x21000213};
    const std::array<la::GuestAccessRange, 4> golden_sites{{
        {0, 4, 8, la::GuestAccessKind::Read, 18, 0},
        {4, 8, 4, la::GuestAccessKind::Write, 18, 8},
        {8, 12, 4, la::GuestAccessKind::Read, 16, 0},
        {12, 16, 8, la::GuestAccessKind::Write, 16, 0},
    }};
    // SC.D's opcode is 0x23; intentionally reject the SC.W word mislabeled D.
    const auto golden_bytes = std::span<const u8>{reinterpret_cast<const u8*>(golden.data()), sizeof(golden)};
    for (u32 index = 0; index < golden_sites.size(); ++index)
        Check(la::GuestAccessEncodingValid(golden_bytes, golden_sites[index]) == (index != 3),
              "GNU golden instruction words enforce native width metadata");
    std::cout << "PASS private native-access encoding " << checks - before << " checks\n";
}

} // namespace swift::tests::loongarch64
