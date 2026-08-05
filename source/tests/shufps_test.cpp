#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>
#include <sys/mman.h>
#include "runtime/common/svm_config.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/frontend/x86/decoder_internal.h"
#include "translator/x86/cpu.h"
#include "translator/x86/translator.h"

using namespace swift;
using namespace swift::translator::x86;

namespace {

using Vec128 = std::array<u8, 16>;

enum class SourceShape {
    Register,
    Memory,
    Alias,
};

const char* ShapeName(SourceShape shape) {
    switch (shape) {
        case SourceShape::Register:
            return "register";
        case SourceShape::Memory:
            return "memory";
        case SourceShape::Alias:
            return "dst-alias";
    }
    return "unknown";
}

std::vector<u8> ShufpsEncoding(SourceShape shape, u8 imm) {
    switch (shape) {
        case SourceShape::Register:
            return {0x0F, 0xC6, 0xC1, imm};  // shufps xmm0,xmm1,imm8
        case SourceShape::Memory:
            // REX.B + mod=01/rm=r13/disp8=0.
            return {0x41, 0x0F, 0xC6, 0x45, 0x00, imm};
        case SourceShape::Alias:
            return {0x0F, 0xC6, 0xC0, imm};  // shufps xmm0,xmm0,imm8
    }
    return {};
}

Vec128 Oracle(const Vec128& a, const Vec128& b, u8 imm) {
    u64 a_lo = 0;
    u64 a_hi = 0;
    u64 b_lo = 0;
    u64 b_hi = 0;
    std::memcpy(&a_lo, a.data(), 8);
    std::memcpy(&a_hi, a.data() + 8, 8);
    std::memcpy(&b_lo, b.data(), 8);
    std::memcpy(&b_hi, b.data() + 8, 8);
    const u64 lo = swift::x86::ShufpsHalf(a_lo, a_hi, imm);
    const u64 hi = swift::x86::ShufpsHalf(b_lo, b_hi, u64(imm) | 0x100);
    Vec128 result{};
    std::memcpy(result.data(), &lo, 8);
    std::memcpy(result.data() + 8, &hi, 8);
    return result;
}

std::string Hex(const Vec128& value) {
    std::string result;
    for (u8 byte : value) {
        result += fmt::format("{:02x}", byte);
    }
    return result;
}

}  // namespace

TEST_CASE("SHUFPS all imm8 values and operand shapes") {
    // Lane values deliberately include signed zero, infinities, signalling/
    // quiet NaNs and ordinary non-float bits. SHUFPS must preserve every bit.
    const std::array<u32, 4> a_lanes{0x80000000u, 0x7F800123u, 0x7FCABCDEu, 0x01234567u};
    const std::array<u32, 4> b_lanes{0x00000000u, 0xFF800000u, 0xFFC54321u, 0x89ABCDEFu};
    Vec128 a{};
    Vec128 b{};
    std::memcpy(a.data(), a_lanes.data(), a.size());
    std::memcpy(b.data(), b_lanes.data(), b.size());

    constexpr size_t kArenaSize = 0x100000;
    swift::runtime::backend::SmcTracker::SetEnabled(false);
    void* arena = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(arena != MAP_FAILED);
    const u64 base = reinterpret_cast<u64>(arena);
    const u64 stack = base + 0x80000;
    const u64 memory_source = base + 0xC0000;
    std::memcpy(reinterpret_cast<void*>(memory_source), b.data(), b.size());

    const char* old_jit = swift::runtime::GetRawSvmConfigEnvForTest("SVM_ENABLE_JIT");
    const bool had_old_jit = old_jit != nullptr;
    const std::string old_jit_value = old_jit ? old_jit : "";
    swift::runtime::SetSvmConfigEnvForTest("SVM_ENABLE_JIT", "1", 1);
    auto* jit_instance = X86Instance::Make();
    swift::runtime::SetSvmConfigEnvForTest("SVM_ENABLE_JIT", "0", 1);
    auto* interp_instance = X86Instance::Make();
    if (had_old_jit) {
        swift::runtime::SetSvmConfigEnvForTest("SVM_ENABLE_JIT", old_jit_value.c_str(), 1);
    } else {
        swift::runtime::UnsetSvmConfigEnvForTest("SVM_ENABLE_JIT");
    }
    auto* jit_core = X86Core::Make(jit_instance);
    auto* interp_core = X86Core::Make(interp_instance);

    size_t cursor = 0;
    size_t checked = 0;
    std::vector<std::string> problems;
    for (SourceShape shape : {SourceShape::Register, SourceShape::Memory, SourceShape::Alias}) {
        for (u32 raw_imm = 0; raw_imm < 256; ++raw_imm) {
            const u8 imm = static_cast<u8>(raw_imm);
            auto code = ShufpsEncoding(shape, imm);
            code.push_back(0xF4);  // hlt
            const u64 code_address = base + 0x1000 + cursor * 0x10;
            ++cursor;
            REQUIRE(code.size() <= 0x10);
            std::memcpy(reinterpret_cast<void*>(code_address), code.data(), code.size());

            const Vec128 expected = Oracle(a, shape == SourceShape::Alias ? a : b, imm);
            for (const auto& [backend, core] :
                 {std::pair{"jit", jit_core}, std::pair{"interpreter", interp_core}}) {
                auto& context = core->GetContext();
                std::memcpy(context.xmms[0].b, a.data(), a.size());
                std::memcpy(context.xmms[1].b, b.data(), b.size());
                context.r13.qword = memory_source;
                context.rsp.qword = stack;
                context.rip.qword = code_address;
                const int exit = int(core->Run());
                Vec128 actual{};
                std::memcpy(actual.data(), context.xmms[0].b, actual.size());
                ++checked;
                if (exit != int(swift::translator::None) || actual != expected) {
                    if (problems.size() < 20) {
                        problems.push_back(
                                fmt::format("{}/{} imm={:02x}: exit={} got={} expected={}",
                                            backend,
                                            ShapeName(shape),
                                            raw_imm,
                                            exit,
                                            Hex(actual),
                                            Hex(expected)));
                    }
                }
            }
        }
    }

    X86Core::Destroy(jit_core);
    X86Core::Destroy(interp_core);
    X86Instance::Destroy(jit_instance);
    X86Instance::Destroy(interp_instance);
    swift::runtime::backend::SmcTracker::SetEnabled(true);
    munmap(arena, kArenaSize);

    std::string joined;
    for (const auto& problem : problems) {
        joined += "\n  " + problem;
    }
    INFO("SHUFPS failures:" << joined);
    CHECK(checked == 256 * 3 * 2);
    CHECK(problems.empty());
}
