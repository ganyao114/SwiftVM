#include <catch2/catch_test_macros.hpp>
#include <array>
#include <bit>
#include <cstring>
#include <sys/mman.h>
#include <vector>
#include "runtime/backend/smc_tracker.h"
#include "translator/x86/translator.h"

using namespace swift;

TEST_CASE("unsigned condition codes preserve carry with either flags ABI", "[flags][regression]") {
    // cmp ecx,esi; seta r9b; setbe r10b; hlt.
    const std::array<u8, 12> bytes{0x39, 0xf1, 0x41, 0x0f, 0x97, 0xc1,
                                  0x41, 0x0f, 0x96, 0xc2, 0xf4, 0x90};
    void* code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(code != MAP_FAILED);
    std::memcpy(code, bytes.data(), bytes.size());
    runtime::backend::SmcTracker::SetEnabled(false);
    auto* instance = translator::x86::X86Instance::Make();
    auto* core = translator::x86::X86Core::Make(instance);
    auto& context = core->GetContext();
    bool matched = true;
    for (u32 left : {0u, 1u, 0x7fffffffu, 0x80000000u, UINT32_MAX}) {
        for (u32 right : {0u, 1u, 0x7fffffffu, 0x80000000u, UINT32_MAX}) {
            context.rcx.qword = left;
            context.rsi.qword = right;
            context.rip.qword = reinterpret_cast<VAddr>(code);
            core->Run();
            matched &= (context.r9.qword & 0xff) == u64(left > right);
            matched &= (context.r10.qword & 0xff) == u64(left <= right);
        }
    }
    translator::x86::X86Core::Destroy(core);
    translator::x86::X86Instance::Destroy(instance);
    runtime::backend::SmcTracker::SetEnabled(true);
    munmap(code, 4096);
    REQUIRE(matched);
}

TEST_CASE("narrow ADC SBB preserve all architectural flags at carry boundaries",
          "[ir][x86][regression][flags]") {
    for (u32 width : {8u, 16u}) {
        for (bool subtract : {false, true}) {
            for (u64 carry : {u64{0}, u64{1}}) {
              for (unsigned form : {0u, 1u, 2u, 3u}) {
                if (width == 16 && form == 1) continue;
                std::vector<u8> bytes{static_cast<u8>(carry ? 0xf9 : 0xf8)};
                if (width == 16) bytes.push_back(0x66);
                bytes.push_back(form == 3 ? (width == 8 ? 0x80 : 0x83)
                    : static_cast<u8>(subtract ? (width == 8 ? 0x18 : 0x19)
                                              : (width == 8 ? 0x10 : 0x11)));
                bytes.push_back(form == 3 ? (subtract ? 0xd8 : 0xd0)
                                        : form == 2 ? 0x1f : form == 1 ? 0xfc : 0xd8);
                if (form == 3) bytes.push_back(0xff);  // sign-extended -1 for word arithmetic
                if (form == 2)
                    bytes.insert(bytes.end(), {0x44, 0x0f, static_cast<u8>(width == 8 ? 0xb6 : 0xb7), 0x07});
                else
                    bytes.insert(bytes.end(), {0x49, 0x89, 0xc0});
                bytes.insert(bytes.end(), {0x9f, 0x0f, 0x90, 0xc2, 0xf4});
                void* code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANON, -1, 0);
                REQUIRE(code != MAP_FAILED);
                std::memcpy(code, bytes.data(), bytes.size());
                runtime::backend::SmcTracker::SetEnabled(false);
                auto* instance = translator::x86::X86Instance::Make();
                auto* core = translator::x86::X86Core::Make(instance);
                auto& context = core->GetContext();
                const u64 mask = (u64{1} << width) - 1;
                const u64 sign = u64{1} << (width - 1);
                bool matched = true;
                u64 bad_a{}, bad_b{}, actual{}, expected{}, actual_flags{}, expected_flags{};
                const auto check = [&](u64 a, u64 b) {
                    const u64 result = (subtract ? a - b - carry : a + b + carry) & mask;
                    const bool cf = subtract ? a < b + carry : a + b + carry > mask;
                    const bool of = ((subtract ? (a ^ b) : ~(a ^ b)) & (a ^ result) & sign) != 0;
                    const bool af = ((a ^ b ^ result) & 0x10) != 0;
                    const bool pf = (std::popcount(static_cast<unsigned>(result & 0xff)) & 1) == 0;
                    const u64 flags = u64(cf) | (u64(pf) << 2) | (u64(af) << 4) |
                                      (u64(result == 0) << 6) | (u64((result & sign) != 0) << 7);
                    u64 memory = (0xa5a5a5a5a5a5a5a5ull & ~mask) | a;
                    context.rax.qword = form == 1 ? 0x11223344556600a5ull | (a << 8) : a;
                    context.rbx.qword = form == 1 ? b << 8 : b;
                    context.rdi.qword = reinterpret_cast<VAddr>(&memory);
                    context.rdx.qword = 0;
                    context.rip.qword = reinterpret_cast<VAddr>(code);
                    core->Run();
                    const u64 got = (context.r8.qword >> (form == 1 ? 8 : 0)) & mask;
                    const u64 got_flags = (context.rax.qword >> 8) & 0xd5;
                    const bool intact = form == 2
                            ? (memory & ~mask) == (0xa5a5a5a5a5a5a5a5ull & ~mask)
                            : form != 1 || (context.r8.qword & ~u64{0xff00}) == 0x11223344556600a5ull;
                    if (!intact || got != result || got_flags != flags || (context.rdx.qword & 1) != u64(of)) {
                        matched = false;
                        bad_a = a; bad_b = b; actual = got; expected = result;
                        actual_flags = got_flags | ((context.rdx.qword & 1) << 11);
                        expected_flags = flags | (u64(of) << 11);
                        return false;
                    }
                    return true;
                };
                if (form == 3) {
                    for (u64 a = 0; a <= mask && matched; ++a) check(a, mask);
                } else if (width == 8) {
                    for (u64 a = 0; a <= mask && matched; ++a)
                        for (u64 b = 0; b <= mask && matched; ++b) check(a, b);
                } else {
                    constexpr std::array<u64, 12> edges{0, 1, 0x0e, 0x0f, 0x10, 0x7ffe,
                                                       0x7fff, 0x8000, 0x8001, 0xffef, 0xfffe, 0xffff};
                    for (auto a : edges) for (auto b : edges) if (matched) check(a, b);
                    u64 random = 0x9e3779b97f4a7c15;
                    for (u32 i = 0; i < 4096 && matched; ++i) {
                        random ^= random << 7; random ^= random >> 9;
                        const auto a = random & mask;
                        random ^= random << 8;
                        check(a, random & mask);
                    }
                }
                translator::x86::X86Core::Destroy(core);
                translator::x86::X86Instance::Destroy(instance);
                runtime::backend::SmcTracker::SetEnabled(true);
                munmap(code, 4096);
                INFO("width=" << width << " subtract=" << subtract << " carry=" << carry
                     << " form=" << form
                     << " a=" << bad_a << " b=" << bad_b << " result=" << actual
                     << '/' << expected << " flags=" << actual_flags << '/' << expected_flags);
                REQUIRE(matched);
              }
            }
        }
    }
}
