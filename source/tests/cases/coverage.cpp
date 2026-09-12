#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "runtime/backend/smc_tracker.h"
#include "translator/linux/guest_memory.h"
#include "translator/x86/translator.h"

namespace {

struct ScopedSmcSetting {
    bool previous = swift::runtime::backend::SmcTracker::IsEnabled();
    ScopedSmcSetting() { swift::runtime::backend::SmcTracker::SetEnabled(false); }
    ~ScopedSmcSetting() { swift::runtime::backend::SmcTracker::SetEnabled(previous); }
};

struct SmallGuest {
    ScopedSmcSetting smc;
    swift::linux::GuestMemory memory;
    std::unique_ptr<swift::translator::x86::X86Instance,
                    decltype(&swift::translator::x86::X86Instance::Destroy)> instance{
            nullptr, swift::translator::x86::X86Instance::Destroy};
    std::unique_ptr<swift::translator::x86::X86Core,
                    decltype(&swift::translator::x86::X86Core::Destroy)> core{
            nullptr, swift::translator::x86::X86Core::Destroy};
    swift::u64 code_address{};
    swift::u64 data_address{};

    SmallGuest(swift::u32 window_bits, const std::vector<swift::u8>& code) {
        if (window_bits) REQUIRE(memory.ReserveWindow(window_bits));
        code_address = memory.MapAnywhere(2 * swift::linux::GuestMemory::kHostPageSize);
        REQUIRE(code_address != 0);
        data_address = code_address + swift::linux::GuestMemory::kHostPageSize;
        std::memcpy(memory.ToHost(code_address), code.data(), code.size());
        instance.reset(swift::translator::x86::X86Instance::Make(
                reinterpret_cast<void*>(memory.GetBias()), window_bits ? memory.Mask() : 0));
        instance->SetFunctionDecodeBudget(32);
        core.reset(swift::translator::x86::X86Core::Make(instance.get()));
    }
};

}  // namespace

TEST_CASE("bounded integer arithmetic publishes the half carry") {
    constexpr std::array<swift::u8, 6> operations{0x01, 0x29, 0x11, 0x19, 0xf7, 0x39};
    constexpr std::array<std::array<swift::u64, 2>, 5> pairs{{
            {0, 1}, {15, 1}, {0, 15}, {16, 1}, {15, 0}}};
    for (bool wide : {false, true}) {
        for (const auto operation : operations) {
            for (const auto& pair : pairs) {
                CAPTURE(wide, operation, pair[0], pair[1]);
                const bool carry = operation == 0x11 || operation == 0x19;
                const bool add = operation == 0x01 || operation == 0x11;
                std::vector<swift::u8> code{swift::u8(carry ? 0xf9 : 0xf8)};
                if (wide) code.push_back(0x48);
                code.insert(code.end(), {operation, swift::u8(operation == 0xf7 ? 0xd8 : 0xc8),
                                         0x9c, 0x5b, 0xf4});
                SmallGuest guest(20, code);
                auto& ctx = guest.core->GetContext();
                ctx.rip.qword = guest.code_address;
                ctx.rsp.qword = guest.data_address + 256;
                ctx.rax.qword = pair[0];
                ctx.rcx.qword = pair[1];
                const auto rhs = pair[1] + (carry ? 1 : 0);
                auto result = add ? pair[0] + rhs : pair[0] - rhs;
                bool half_carry = add ? (pair[0] & 15) + (pair[1] & 15) + carry > 15
                                      : (pair[0] & 15) < (pair[1] & 15) + carry;
                if (operation == 0xf7) {
                    result = 0 - pair[0];
                    half_carry = (pair[0] & 15) != 0;
                } else if (operation == 0x39) {
                    result = pair[0];
                }
                if (!wide) result &= 0xffffffffull;
                REQUIRE(guest.core->Run() == swift::translator::ExitReason::None);
                CHECK(ctx.rax.qword == result);
                CHECK(((ctx.rbx.qword >> 4) & 1) == half_carry);
            }
        }
    }
}

TEST_CASE("bounded scalar minmax preserves preceding integer flags") {
    for (bool single : {false, true}) {
        for (swift::u8 operation : {0x5d, 0x5f}) {
            for (bool equal : {false, true}) {
                CAPTURE(single, operation, equal);
                // CMP EAX,ECX; MIN/MAX xmm0,xmm1; SETB DL; SETE CL; PUSHFQ; POP RBX.
                const std::vector<swift::u8> code{
                        0x39, 0xc8,
                        swift::u8(single ? 0xf3 : 0xf2), 0x0f, operation, 0xc1,
                        0x0f, 0x92, 0xc2, 0x0f, 0x94, 0xc1, 0x9c, 0x5b, 0xf4};
                SmallGuest guest(20, code);
                auto& ctx = guest.core->GetContext();
                ctx.rip.qword = guest.code_address;
                ctx.rsp.qword = guest.data_address + 256;
                ctx.rax.qword = 0;
                ctx.rcx.qword = equal ? 0 : 1;
                ctx.rdx.qword = 0;
                ctx.xmm0.l[0] = single ? 0x40800000ull : 0x4010000000000000ull;
                ctx.xmm1.l[0] = single ? 0x41100000ull : 0x4022000000000000ull;
                REQUIRE(guest.core->Run() == swift::translator::ExitReason::None);
                CHECK((ctx.rdx.qword & 0xff) == (equal ? 0 : 1));
                CHECK((ctx.rcx.qword & 0xff) == (equal ? 1 : 0));
                CHECK((ctx.rbx.qword & 0x8d5) == (equal ? 0x44 : 0x95));
            }
        }
    }
}

TEST_CASE("bounded scalar arithmetic retains a live left operand") {
    constexpr std::array<swift::u8, 4> operations{0x58, 0x5c, 0x59, 0x5e};
    constexpr std::array<double, 4> answers{10.0, 6.0, 16.0, 4.0};
    for (bool single : {false, true}) {
        for (unsigned index = 0; index < operations.size(); ++index) {
            CAPTURE(single, index);
            // Preserve the left value in XMM2, calculate in XMM0, publish to
            // the old right register, then restore the still-live left value.
            const std::vector<swift::u8> code{
                    0x66, 0x0f, 0x28, 0xd0,
                    swift::u8(single ? 0xf3 : 0xf2), 0x0f, operations[index], 0xc1,
                    0x66, 0x0f, 0x28, 0xc8,
                    0x66, 0x0f, 0x28, 0xc2,
                    0xf4};
            SmallGuest guest(20, code);
            auto& ctx = guest.core->GetContext();
            ctx.rip.qword = guest.code_address;
            ctx.xmm0.l[0] = single ? 0x1122334441000000ull : 0x4020000000000000ull;
            ctx.xmm0.l[1] = 0x5566778899aabbccull;
            ctx.xmm1.l[0] = single ? 0xaabbccdd40000000ull : 0x4000000000000000ull;
            ctx.xmm1.l[1] = 0xddeeff0011223344ull;
            const auto left = ctx.xmm0;
            auto expected = left;
            if (single) {
                const float value = static_cast<float>(answers[index]);
                std::memcpy(&expected.i[0], &value, sizeof(value));
            } else {
                std::memcpy(&expected.l[0], &answers[index], sizeof(double));
            }
            REQUIRE(guest.core->Run() == swift::translator::ExitReason::None);
            CHECK(ctx.xmm0.l[0] == left.l[0]);
            CHECK(ctx.xmm0.l[1] == left.l[1]);
            CHECK(ctx.xmm1.l[0] == expected.l[0]);
            CHECK(ctx.xmm1.l[1] == expected.l[1]);
        }
    }
}

TEST_CASE("bounded scalar copy preserves the source guest register") {
    constexpr std::array<swift::u8, 4> operations{0x58, 0x5c, 0x59, 0x5e};
    constexpr std::array<double, 4> answers{10.0, 6.0, 16.0, 4.0};
    for (bool single : {false, true}) {
        for (unsigned index = 0; index < operations.size(); ++index) {
            CAPTURE(single, index);
            // MOVAPD xmm2,xmm0; ADD/SUB/MUL/DIV xmm2,xmm1; HLT.
            // XMM0 remains observable at the exit even without another IR read.
            const std::vector<swift::u8> code{
                    0x66, 0x0f, 0x28, 0xd0,
                    swift::u8(single ? 0xf3 : 0xf2), 0x0f, operations[index], 0xd1,
                    0xf4};
            SmallGuest guest(20, code);
            auto& ctx = guest.core->GetContext();
            ctx.rip.qword = guest.code_address;
            ctx.xmm0.l[0] = single ? 0x1122334441000000ull : 0x4020000000000000ull;
            ctx.xmm0.l[1] = 0x5566778899aabbccull;
            ctx.xmm1.l[0] = single ? 0xaabbccdd40000000ull : 0x4000000000000000ull;
            ctx.xmm1.l[1] = 0xddeeff0011223344ull;
            const auto left = ctx.xmm0;
            const auto right = ctx.xmm1;
            auto expected = left;
            if (single) {
                const float value = static_cast<float>(answers[index]);
                std::memcpy(&expected.i[0], &value, sizeof(value));
            } else {
                std::memcpy(&expected.l[0], &answers[index], sizeof(double));
            }
            REQUIRE(guest.core->Run() == swift::translator::ExitReason::None);
            CHECK(ctx.xmm0.l[0] == left.l[0]);
            CHECK(ctx.xmm0.l[1] == left.l[1]);
            CHECK(ctx.xmm1.l[0] == right.l[0]);
            CHECK(ctx.xmm1.l[1] == right.l[1]);
            CHECK(ctx.xmm2.l[0] == expected.l[0]);
            CHECK(ctx.xmm2.l[1] == expected.l[1]);
        }
    }
}

TEST_CASE("bounded floating comparisons preserve conditions and guest flags") {
    struct Pair {
        double left;
        double right;
        swift::u64 flags;
    };
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr std::array pairs{
            Pair{1.0, 2.0, 0x01}, Pair{2.0, 2.0, 0x40},
            Pair{3.0, 2.0, 0x00}, Pair{nan, 2.0, 0x45}, Pair{1.0, nan, 0x45}};
    for (bool single : {false, true}) {
        for (swift::u8 condition : {2, 3, 4, 5, 6, 7, 10, 11}) {
            for (unsigned consumer = 0; consumer < 3; ++consumer) {
                CAPTURE(single, condition, consumer);
                // EAX=0, ECX=1; COMIS xmm0,xmm1; Jcc, SETcc or CMOVcc.
                std::vector<swift::u8> code{
                        0xb8, 0, 0, 0, 0, 0xb9, 1, 0, 0, 0};
                if (!single) code.push_back(0x66);
                code.insert(code.end(), {0x0f, 0x2f, 0xc1});
                if (consumer == 0) {
                    code.insert(code.end(), {
                            swift::u8(0x70 + condition), 2, 0xeb, 5,
                            0xb8, 1, 0, 0, 0});
                } else {
                    // A flag-preserving vector copy also exercises the next
                    // guest instruction boundary before the condition is used.
                    code.insert(code.end(), {0x66, 0x0f, 0x28, 0xd3});
                    code.insert(code.end(), {
                            0x0f, swift::u8((consumer == 1 ? 0x90 : 0x40) + condition),
                            swift::u8(consumer == 1 ? 0xc0 : 0xc1)});
                }
                code.insert(code.end(), {0x9c, 0x5a, 0xf4});  // PUSHFQ; POP RDX; HLT
                SmallGuest guest(20, code);
                for (const auto& pair : pairs) {
                    CAPTURE(pair.left, pair.right, pair.flags);
                    auto& ctx = guest.core->GetContext();
                    ctx.rip.qword = guest.code_address;
                    ctx.rsp.qword = guest.data_address + 256;
                    std::memset(&ctx.xmms[0], 0, 2 * sizeof(ctx.xmms[0]));
                    if (single) {
                        const float left = static_cast<float>(pair.left);
                        const float right = static_cast<float>(pair.right);
                        std::memcpy(&ctx.xmms[0], &left, sizeof(left));
                        std::memcpy(&ctx.xmms[1], &right, sizeof(right));
                    } else {
                        std::memcpy(&ctx.xmms[0], &pair.left, sizeof(pair.left));
                        std::memcpy(&ctx.xmms[1], &pair.right, sizeof(pair.right));
                    }
                    const bool carry = pair.flags & 1;
                    const bool zero = pair.flags & 0x40;
                    const bool parity = pair.flags & 4;
                    bool expected{};
                    switch (condition) {
                        case 2: expected = carry; break;
                        case 3: expected = !carry; break;
                        case 4: expected = zero; break;
                        case 5: expected = !zero; break;
                        case 6: expected = carry || zero; break;
                        case 7: expected = !carry && !zero; break;
                        case 10: expected = parity; break;
                        case 11: expected = !parity; break;
                    }
                    REQUIRE(guest.core->Run() == swift::translator::ExitReason::None);
                    CHECK(ctx.rax.qword == static_cast<swift::u64>(expected));
                    CHECK((ctx.rdx.qword & 0x8d5) == pair.flags);
                }
            }
        }
    }
}

TEST_CASE("bounded byte-table loop keeps its odd index and byte stores") {
    // 7-Zip's recorded failing loop at 0x62a51a..0x62a565, followed by HLT.
    // Five iterations exercise partial-register writes, shifts, scaled
    // addresses and the loop-carried RCX/R12 values with a 256-byte table.
    const std::vector<swift::u8> code{
            0xb9, 0x01, 0x00, 0x00, 0x00, 0x48, 0x83, 0xe2, 0xfe,
            0x48, 0x83, 0xc2, 0x03, 0x0f, 0xb6, 0x44, 0x0d, 0x00,
            0x49, 0xc1, 0xe4, 0x03, 0x48, 0x89, 0xc6, 0x48, 0xc1, 0xe0, 0x03,
            0x4c, 0x29, 0xe6, 0x44, 0x0f, 0xb6, 0x64, 0x0d, 0x01,
            0x40, 0x0f, 0xb6, 0xf6, 0x88, 0x4c, 0x34, 0x10,
            0x4c, 0x89, 0xe6, 0x48, 0x29, 0xc6, 0x40, 0x0f, 0xb6, 0xc6,
            0x8d, 0x71, 0x01, 0x40, 0x88, 0x74, 0x04, 0x10,
            0x48, 0x89, 0xc8, 0x48, 0x83, 0xc1, 0x02,
            0x48, 0x39, 0xd1, 0x75, 0xc1, 0xf4,
    };
    for (swift::u32 window_bits : {0u, 20u, 32u}) {
        for (bool body_first : {false, true}) {
            CAPTURE(window_bits, body_first);
            SmallGuest guest(window_bits, code);
            auto* bytes = static_cast<swift::u8*>(guest.memory.ToHost(guest.data_address));
            constexpr std::array<swift::u8, 12> input{
                    0xff, 0x80, 0x7f, 0x01, 0xfe, 0x20, 0xc0, 0x04, 0xf0, 0x08, 0xa5, 0x11};
            std::memcpy(bytes, input.data(), input.size());
            auto* stack = bytes + 256;
            std::memset(stack, 0x5a, 288);
            std::array<swift::u8, 288> expected;
            expected.fill(0x5a);
            unsigned previous = input[0];
            for (unsigned index = 1; index < 11; index += 2) {
                expected[16 + ((input[index] - previous * 8) & 255)] = index;
                expected[16 + ((input[index + 1] - input[index] * 8) & 255)] = index + 1;
                previous = input[index + 1];
            }
            if (body_first) REQUIRE(guest.instance->CompileAt(guest.code_address + 13));
            auto& ctx = guest.core->GetContext();
            ctx.rip.qword = guest.code_address;
            ctx.rbp.qword = guest.data_address;
            ctx.rsp.qword = guest.data_address + 256;
            ctx.rdx.qword = 8;
            ctx.r12.qword = input[0];
            REQUIRE(guest.core->Run() == swift::translator::ExitReason::None);
            REQUIRE(ctx.rcx.qword == 11);
            REQUIRE(ctx.rdx.qword == 11);
            REQUIRE(ctx.r12.qword == input[10]);
            for (unsigned i = 0; i < expected.size(); ++i) {
                CAPTURE(i, expected[i], stack[i]);
                REQUIRE(stack[i] == expected[i]);
            }
        }
    }
}

TEST_CASE("bounded streaming kernels preserve every lane and terminate") {
    // Four SSE2 loops: c=a; b=3*c; a=b+c; a=b+3*c. Each loop handles
    // two doubles per iteration. Only 32 elements per array are needed to
    // exercise the arithmetic, scaled addresses and backward branches.
    const std::vector<swift::u8> code{
            0x45, 0x31, 0xc0, 0x66, 0x42, 0x0f, 0x10, 0x04, 0xc7, 0x66, 0x42, 0x0f,
            0x11, 0x04, 0xc2, 0x49, 0x83, 0xc0, 0x02, 0x49, 0x39, 0xc8, 0x72, 0xeb,
            0x45, 0x31, 0xc0, 0x66, 0x42, 0x0f, 0x10, 0x04, 0xc2, 0x66, 0x0f, 0x59,
            0xc3, 0x66, 0x42, 0x0f, 0x11, 0x04, 0xc6, 0x49, 0x83, 0xc0, 0x02, 0x49,
            0x39, 0xc8, 0x72, 0xe7, 0x45, 0x31, 0xc0, 0x66, 0x42, 0x0f, 0x10, 0x04,
            0xc6, 0x66, 0x42, 0x0f, 0x10, 0x0c, 0xc2, 0x66, 0x0f, 0x58, 0xc1, 0x66,
            0x42, 0x0f, 0x11, 0x04, 0xc7, 0x49, 0x83, 0xc0, 0x02, 0x49, 0x39, 0xc8,
            0x72, 0xe1, 0x45, 0x31, 0xc0, 0x66, 0x42, 0x0f, 0x10, 0x04, 0xc2, 0x66,
            0x42, 0x0f, 0x10, 0x0c, 0xc6, 0x66, 0x0f, 0x59, 0xc3, 0x66, 0x0f, 0x58,
            0xc1, 0x66, 0x42, 0x0f, 0x11, 0x04, 0xc7, 0x49, 0x83, 0xc0, 0x02, 0x49,
            0x39, 0xc8, 0x72, 0xdd, 0xf4,
    };
    for (swift::u32 window_bits : {0u, 20u, 32u}) {
        CAPTURE(window_bits);
        SmallGuest guest(window_bits, code);
        auto* data = static_cast<double*>(guest.memory.ToHost(guest.data_address));
        std::array<double, 96> expected{};
        for (unsigned i = 0; i < 32; ++i) {
            expected[i] = i + 0.5;
            expected[32 + i] = 2 * i;
        }
        std::memcpy(data, expected.data(), sizeof(expected));
        for (unsigned iteration = 0; iteration < 2; ++iteration) {
            for (unsigned i = 0; i < 32; ++i) {
                expected[64 + i] = expected[i];
                expected[32 + i] = 3 * expected[64 + i];
                expected[i] = expected[32 + i] + 3 * expected[64 + i];
            }
            auto& ctx = guest.core->GetContext();
            ctx.rip.qword = guest.code_address;
            ctx.rdi.qword = guest.data_address;
            ctx.rsi.qword = guest.data_address + 32 * sizeof(double);
            ctx.rdx.qword = guest.data_address + 64 * sizeof(double);
            ctx.rcx.qword = 32;
            const std::array<double, 2> factor{3, 3};
            std::memcpy(&ctx.xmm3, factor.data(), sizeof(factor));
            REQUIRE(guest.core->Run() == swift::translator::ExitReason::None);
            REQUIRE(ctx.r8.qword == 32);
            for (unsigned i = 0; i < expected.size(); ++i) {
                CAPTURE(iteration, i);
                REQUIRE(data[i] == expected[i]);
            }
        }
    }
}
