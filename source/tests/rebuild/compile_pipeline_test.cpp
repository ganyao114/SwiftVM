#include "runtime/frontend/x86/detail/decoded.h"
#include "runtime/backend/arm64/compile.h"
#include "runtime/compile/optimize.h"
#include "runtime/frontend/x86/build_ir.h"
#include "runtime/arch/x86/state.h"
#include "function_fixture.h"

#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__aarch64__) && !defined(__APPLE__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
using namespace swift::runtime;
namespace a64 = backend::arm64;
void Check(bool condition, int line) {
    if (!condition) throw std::runtime_error("pipeline contract failed at " + std::to_string(line));
}
#define CHECK(x) Check(bool(x), __LINE__)

#if defined(__aarch64__) && !defined(__APPLE__)
constexpr bool executes_native = true;
#else
constexpr bool executes_native = false;
#endif
#if defined(__aarch64__) && !defined(__APPLE__) && defined(HWCAP_FLAGM)
const bool native_flags_available = (getauxval(AT_HWCAP) & HWCAP_FLAGM) != 0;
#else
constexpr bool native_flags_available = false;
#endif

std::uint64_t Execute(const a64::CompileResult& code, std::uint64_t* state) {
#if defined(__aarch64__) && !defined(__APPLE__)
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    CHECK(page && page != static_cast<std::size_t>(-1));
    const auto bytes = code.words.size() * sizeof(code.words[0]);
    const auto size = (bytes + page - 1) / page * page;
    void* mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    std::memcpy(mapping, code.words.data(), bytes);
    if (mprotect(mapping, size, PROT_READ | PROT_EXEC)) {
        munmap(mapping, size);
        CHECK(false);
    }
    __builtin___clear_cache(static_cast<char*>(mapping), static_cast<char*>(mapping) + bytes);
    const auto result = reinterpret_cast<std::uint64_t (*)(std::uint8_t*)>(mapping)(
            reinterpret_cast<std::uint8_t*>(state));
    CHECK(munmap(mapping, size) == 0);
    return result;
#else
    (void)code;
    (void)state;
    return 0;
#endif
}

swift::x86::BuildResult Build(std::span<const swift::x86::BlockWindow> windows,
                              ir::Scope scope,
                              ir::TargetInfo target,
                              bool decoded) {
    if (!decoded) return swift::x86::BuildIR(windows, scope, {0, 128, 8}, target);
    std::vector<_DInst> instructions;
    std::vector<swift::x86::DecodedBlock> blocks(windows.size());
    // A real decoder may discover blocks in a different order from guest PC.
    // Append the actual records in reverse block order, without a copy table.
    for (std::size_t index = windows.size(); index--;) {
        const auto& window = windows[index];
        const auto first = static_cast<std::uint32_t>(instructions.size());
        auto bytes = window.bytes;
        auto pc = window.pc;
        while (!bytes.empty()) {
            _CodeInfo code{};
            code.codeOffset = pc;
            code.code = bytes.data();
            code.codeLen = static_cast<int>(bytes.size());
            code.dt = Decode64Bits;
            _DInst instruction{};
            unsigned used{};
            (void)distorm_decompose(&code, &instruction, 1, &used);
            CHECK(used == 1 && instruction.size && instruction.size <= bytes.size());
            instructions.push_back(instruction);
            pc += instruction.size;
            bytes = bytes.subspan(instruction.size);
        }
        blocks[index] = {window.pc, first, static_cast<std::uint32_t>(instructions.size()) - first};
    }
    return swift::x86::BuildDecodedIR(swift::x86::detail::FixtureNormalize(instructions),
                                      blocks,
                                      windows,
                                      scope,
                                      {0, 128, 8},
                                      target);
}

a64::CompileResult Compile(swift::x86::BuildResult& built, bool optimize, bool flags) {
    if (!built)
        std::fprintf(stderr,
                     "frontend refused error=%u pc=%llx detail=%u\n",
                     unsigned(built.error),
                     static_cast<unsigned long long>(built.error_pc),
                     unsigned(built.ir_error));
    CHECK(built);
    const auto unit = built.program.UnitId();
    const auto* operations = built.program.Operations().data();
    if (optimize) {
        auto result = compile::Optimize(std::move(built.program));
        CHECK(result);
        CHECK(result.program.UnitId() == unit && result.program.Operations().data() == operations);
        built.program = std::move(result.program);
    }
    auto options = flags ? a64::CompileOptions{136} : a64::CompileOptions{};
    options.native_nzcv = flags && native_flags_available;
    auto code = a64::Compile(built.program, options);
    if (!code)
        std::fprintf(stderr,
                     "compile refused error=%u node=%u block=%u\n",
                     unsigned(code.error),
                     code.node.value,
                     code.block.value);
    CHECK(code && !code.words.empty());
    return code;
}

void RunPipeline(ir::Scope scope, ir::TargetMode mode, bool optimize) {
    alignas(16) std::array<std::uint32_t, 8> memory{};
    alignas(16) [[maybe_unused]] std::array<std::uint64_t, 17> state{};
    auto target = a64::KnownTargetInfo();
    target.features |= static_cast<std::uint64_t>(ir::TargetFeature::PrivateGPRState);
    target.mode = mode;
    std::vector<std::uint8_t> entry{0x49, 0xb8, 0x88, 0x77, 0x66, 0x55,
                                    0x44, 0x33, 0x22, 0x11,              // movabs r8,original
                                    0x4d, 0x89, 0xc1,                    // mov r9,r8
                                    0x41, 0xb8, 0xdd, 0xcc, 0xbb, 0xaa,  // mov r8d,0xaabbccdd
                                    0x4d, 0x89, 0xc2,                    // mov r10,r8
                                    0x41, 0xb2, 0xee,                    // mov r10b,0xee
                                    0x4d, 0x89, 0xd3,                    // mov r11,r10
                                    0x66, 0x41, 0xba, 0x34, 0x12,        // mov r10w,0x1234
                                    0x4d, 0x89, 0xd4,                    // mov r12,r10
                                    0x4d, 0x89, 0xcf,  // mov r15,r9 after r8 changed
                                    0x48, 0xb8};       // movabs rax,&memory
    const auto address = reinterpret_cast<std::uintptr_t>(memory.data());
    for (unsigned byte = 0; byte < 8; ++byte)
        entry.push_back(static_cast<std::uint8_t>(address >> (byte * 8)));
    for (const auto byte : {0xb9, 3,    0,    0,    0,     // mov ecx,3
                            0x48, 0x8d, 0x14, 0x88,        // lea rdx,[rax+rcx*4]
                            0x89, 0x0a,                    // mov [rdx],ecx
                            0x8b, 0x1a,                    // mov ebx,[rdx]
                            0x44, 0x8d, 0x6c, 0x88, 0xf0,  // lea r13d,[rax+rcx*4-16]
                            0x4d, 0x89, 0xee})  // mov r14,r13: consume the zero-high parent
        entry.push_back(static_cast<std::uint8_t>(byte));
    const auto displacement = 0x80 - entry.size() - 2;
    CHECK(displacement < 128);
    entry.push_back(0xeb);
    entry.push_back(static_cast<std::uint8_t>(displacement));
    const std::uint8_t tail[]{0x89, 0xdf, 0x90};  // mov edi,ebx; nop
    const swift::x86::BlockWindow windows[]{{0x1000, entry}, {0x1080, tail}};
    auto built = swift::x86::BuildIR(std::span{windows}.first(scope == ir::Scope::Block ? 1 : 2),
                                     scope,
                                     {0, 128, 8},
                                     target);
    CHECK(built);
    const auto unit = built.program.UnitId();
    const auto* operations = built.program.Operations().data();
    if (optimize) {
        auto optimized = compile::Optimize(std::move(built.program));
        CHECK(optimized);
        CHECK(optimized.program.UnitId() == unit);
        CHECK(optimized.program.Operations().data() == operations);
        built.program = std::move(optimized.program);
    }
    const auto code = a64::Compile(built.program);
    if (!code) {
        std::fprintf(stderr,
                     "compile refused: error=%u node=%u block=%u\n",
                     unsigned(code.error),
                     code.node.value,
                     code.block.value);
    }
    CHECK(code && !code.words.empty());
    const auto result = Execute(code, state.data());
    if constexpr (executes_native) {
        CHECK(result == 0 && memory[3] == 3);
        CHECK(state[0] == address && state[1] == 3 && state[2] == address + 12 && state[3] == 3);
        CHECK(state[7] == (scope == ir::Scope::Function ? 3 : 0));
        CHECK(state[16] == (scope == ir::Scope::Function ? 0x1083 : 0x1080));
        CHECK(state[8] == 0xaabbccdd && state[9] == 0x1122334455667788ULL);
        CHECK(state[10] == 0xaabb1234 && state[11] == 0xaabbccee && state[12] == 0xaabb1234);
        CHECK(state[15] == 0x1122334455667788ULL);
        CHECK(state[13] == static_cast<std::uint32_t>(address + 12 - 16) && state[14] == state[13]);
    }
}

using u64 = std::uint64_t;
enum class Alu { Add, Sub, Compare, And, Or, Xor, Test, Negate };
struct ExpectedFlags {
    u64 result{}, packed{};
    bool carry{}, overflow{}, zero{}, negative{}, parity{};
};
ExpectedFlags Expected(Alu operation, unsigned bits, u64 left, u64 right, u64 previous) {
    const auto mask = bits == 64 ? UINT64_MAX : u64{UINT32_MAX};
    const auto sign = u64{1} << (bits - 1);
    left &= mask;
    right &= mask;
    ExpectedFlags out;
    bool auxiliary{};
    switch (operation) {
        case Alu::Add:
            out.result = (left + right) & mask;
            out.carry = bits == 64 ? out.result < left : (left + right) > mask;
            out.overflow = (~(left ^ right) & (left ^ out.result) & sign) != 0;
            auxiliary = ((left ^ right ^ out.result) & 16) != 0;
            break;
        case Alu::Sub:
        case Alu::Compare:
            out.result = (left - right) & mask;
            out.carry = left < right;
            out.overflow = ((left ^ right) & (left ^ out.result) & sign) != 0;
            auxiliary = ((left ^ right ^ out.result) & 16) != 0;
            break;
        case Alu::Negate:
            out.result = (u64{} - left) & mask;
            out.carry = left != 0;
            out.overflow = left == sign;
            auxiliary = ((left ^ out.result) & 16) != 0;
            break;
        case Alu::And:
        case Alu::Test:
            out.result = left & right;
            break;
        case Alu::Or:
            out.result = left | right;
            break;
        case Alu::Xor:
            out.result = left ^ right;
            break;
    }
    out.zero = out.result == 0;
    out.negative = (out.result & sign) != 0;
    out.parity = (std::popcount(static_cast<std::uint8_t>(out.result)) & 1) == 0;
    constexpr u64 replaced = (u64{15} << 28) | (u64{1} << 26) | 255;
    out.packed = (previous & ~replaced) | (u64{out.negative} << 31) | (u64{out.zero} << 30) |
                 (u64{out.carry} << 29) | (u64{out.overflow} << 28) | (u64{auxiliary} << 26) |
                 (out.result & 255);
    return out;
}
bool BranchTaken(unsigned condition, const ExpectedFlags& f) {
    switch (condition) {
        case 0:
            return f.overflow;
        case 1:
            return !f.overflow;
        case 2:
            return f.carry;
        case 3:
            return !f.carry;
        case 4:
            return f.zero;
        case 5:
            return !f.zero;
        case 6:
            return f.carry || f.zero;
        case 7:
            return !f.carry && !f.zero;
        case 8:
            return f.negative;
        case 9:
            return !f.negative;
        case 10:
            return f.parity;
        case 11:
            return !f.parity;
        case 12:
            return f.negative != f.overflow;
        case 13:
            return f.negative == f.overflow;
        case 14:
            return f.zero || (f.negative != f.overflow);
        default:
            return !f.zero && (f.negative == f.overflow);
    }
}

void FlagBranch(Alu operation,
                unsigned bits,
                unsigned condition,
                u64 left,
                u64 right,
                ir::Scope scope,
                ir::TargetMode mode,
                bool optimize,
                bool decoded,
                bool immediate = false) {
    std::vector<std::uint8_t> bytes;
    if (bits == 64) bytes.push_back(0x48);
    const std::uint8_t opcodes[]{0x01, 0x29, 0x39, 0x21, 0x09, 0x31, 0x85, 0xf7};
    if (immediate) {
        if (operation == Alu::Test) {
            bytes.push_back(0xf7);
            bytes.push_back(0xc0);
            for (unsigned i = 0; i < 4; ++i) bytes.push_back(0xff);
        } else {
            const std::uint8_t group[]{0xc0, 0xe8, 0xf8, 0xe0, 0xc8, 0xf0};
            CHECK(unsigned(operation) < 6);
            bytes.push_back(0x83);
            bytes.push_back(group[unsigned(operation)]);
            bytes.push_back(0xff);  // sign-extended imm8 -1
        }
        CHECK(right == UINT64_MAX);
    } else {
        bytes.push_back(opcodes[unsigned(operation)]);
        bytes.push_back(0xd8);  // eax/rax, ebx/rbx; NEG /3
    }
    bytes.push_back(static_cast<std::uint8_t>(0x70 + condition));
    bytes.push_back(0x20);
    const u64 fallthrough = 0x2000 + bytes.size(), taken = fallthrough + 0x20;
    const std::uint8_t no[]{0xb9, 0x11, 0x11, 0, 0}, yes[]{0xb9, 0x22, 0x22, 0, 0};
    const swift::x86::BlockWindow windows[]{{0x2000, bytes}, {fallthrough, no}, {taken, yes}};
    auto target = a64::KnownTargetInfo();
    target.features |= static_cast<u64>(ir::TargetFeature::PrivateGPRState);
    target.mode = mode;
    auto built = Build(
            std::span{windows}.first(scope == ir::Scope::Block ? 1 : 3), scope, target, decoded);
    auto code = Compile(built, optimize, true);
    std::array<u64, 18> state{};
    state[0] = left;
    state[3] = right;
    constexpr u64 original = UINT64_C(0x5a5aa5a5110000a5);
    state[17] = original;
    const auto expected = Expected(operation, bits, left, right, original);
    const auto branch = BranchTaken(condition, expected);
    CHECK(Execute(code, state.data()) == 0);
    if constexpr (executes_native) {
        const bool writes = operation != Alu::Compare && operation != Alu::Test;
        if (state[0] != (writes ? expected.result : left) || state[17] != expected.packed ||
            state[16] != (branch ? taken : fallthrough) + (scope == ir::Scope::Function ? 5 : 0)) {
            std::fprintf(stderr,
                         "ALU mismatch op=%u bits=%u cc=%u a=%llx b=%llx flags=%llx expected=%llx "
                         "pc=%llx\n",
                         unsigned(operation),
                         bits,
                         condition,
                         static_cast<unsigned long long>(left),
                         static_cast<unsigned long long>(right),
                         static_cast<unsigned long long>(state[17]),
                         static_cast<unsigned long long>(expected.packed),
                         static_cast<unsigned long long>(state[16]));
            CHECK(false);
        }
        CHECK(state[3] == right);
        CHECK(state[1] == (scope == ir::Scope::Function ? (branch ? 0x2222 : 0x1111) : 0));
    }
}

std::size_t IntegerFlagBranches() {
    constexpr std::array<std::array<u64, 2>, 8> cases{{{{0, 0}},
                                                       {{1, 2}},
                                                       {{UINT64_MAX, 1}},
                                                       {{UINT32_MAX / 2, 1}},
                                                       {{u64{1} << 63, 1}},
                                                       {{16, 1}},
                                                       {{0x81, 0x80}},
                                                       {{UINT64_MAX / 2, 1}}}};
    std::size_t count{};
    for (const auto scope : {ir::Scope::Block, ir::Scope::Function})
        for (const auto bits : {32u, 64u})
            for (unsigned operation = 0; operation < 8; ++operation)
                for (unsigned condition = 0; condition < 16; ++condition)
                    for (const auto& pair : cases) {
                        FlagBranch(static_cast<Alu>(operation),
                                   bits,
                                   condition,
                                   pair[0],
                                   pair[1],
                                   scope,
                                   ir::TargetMode::Known,
                                   true,
                                   condition % 2 == 0);
                        ++count;
                    }
    for (const auto mode : {ir::TargetMode::EarlyOff, ir::TargetMode::Unknown})
        for (const auto scope : {ir::Scope::Block, ir::Scope::Function})
            for (const auto bits : {32u, 64u})
                for (const unsigned condition : {2u, 6u, 12u, 14u}) {
                    FlagBranch(Alu::Sub, bits, condition, 0, 1, scope, mode, false, true);
                    ++count;
                }
    for (const auto bits : {32u, 64u})
        for (unsigned operation = 0; operation < 7; ++operation)
            for (const unsigned condition : {0u, 2u, 7u}) {
                FlagBranch(static_cast<Alu>(operation),
                           bits,
                           condition,
                           1,
                           UINT64_MAX,
                           ir::Scope::Block,
                           ir::TargetMode::Known,
                           true,
                           true,
                           true);
                ++count;
            }
    return count;
}

void DeferredFlagsAndMemoryObservation() {
    for (const bool observe : {false, true}) {
        std::vector<std::uint8_t> bytes{0x01, 0xd8};  // add eax,ebx
        if (observe) {
            bytes.push_back(0x48);
            bytes.push_back(0x8b);
            bytes.push_back(0x16);
        }  // mov rdx,[rsi]
        bytes.push_back(0x31);
        bytes.push_back(0xf8);  // xor eax,edi
        const swift::x86::BlockWindow window{0x4000, bytes};
        auto built = Build({&window, 1}, ir::Scope::Block, a64::KnownTargetInfo(), true);
        CHECK(built);
        std::size_t producers{};
        for (const auto& op : built.program.Operations()) producers += op.flag_outputs != 0;
        CHECK(producers == (observe ? 2 : 1));
        auto code = Compile(built, true, true);
        std::array<u64, 18> state{};
        state[0] = UINT32_MAX;
        state[3] = 1;
        state[7] = 3;
        state[6] = reinterpret_cast<std::uintptr_t>(&state[17]);
        state[17] = 0;
        const auto sum = Expected(Alu::Add, 32, UINT32_MAX, 1, 0);
        const auto final = Expected(Alu::Xor, 32, sum.result, 3, sum.packed);
        CHECK(Execute(code, state.data()) == 0);
        if constexpr (executes_native) {
            CHECK(state[0] == 3 && state[17] == final.packed);
            if (observe) CHECK(state[2] == sum.packed);
        }
    }
    // Guest stores can alias the explicit flags field: committing the old
    // shadow before the store must not overwrite the newly written flags.
    const std::uint8_t alias[]{0x48, 0x89, 0x3e, 0x72, 0x20};  // mov [rsi],rdi; jb external
    const swift::x86::BlockWindow window{0x5000, alias};
    auto built = Build({&window, 1}, ir::Scope::Block, a64::KnownTargetInfo(), false);
    auto code = Compile(built, true, true);
    std::array<u64, 18> state{};
    state[6] = reinterpret_cast<std::uintptr_t>(&state[17]);
    state[7] = u64{1} << 29;
    state[17] = 0;
    CHECK(Execute(code, state.data()) == 0);
    if constexpr (executes_native) CHECK(state[17] == state[7] && state[16] == 0x5025);
}

void MultiplyObservation() {
    const std::uint8_t bytes[]{
            0x39, 0xc9,                    // cmp ecx,ecx: ZF must survive IMUL
            0x69, 0xc3, 2,    0,    0, 0,  // imul eax,ebx,2: observed overflow
            0x40, 0x0f, 0x94, 0xc6,        // setz sil
            0x0f, 0x90, 0xc1,              // seto cl
            0x0f, 0x92, 0xc2,              // setb dl
            0x69, 0xf8, 3,    0,    0, 0,  // imul edi,eax,3: flags overwritten
            0x83, 0xc7, 1,                 // add edi,1
    };
    const swift::x86::BlockWindow window{0x7000, bytes};
    auto target = a64::KnownTargetInfo();
    target.features |= static_cast<u64>(ir::TargetFeature::PrivateGPRState);
    auto built = Build({&window, 1}, ir::Scope::Function, target, false);
    auto code = Compile(built, true, true);
    std::array<u64, 18> state{};
    state[3] = 0x40000000;
    constexpr u64 original = UINT64_C(0x5a5aa5a5110000a5);
    state[17] = original;
    CHECK(Execute(code, state.data()) == 0);
    if constexpr (executes_native) {
        CHECK(state[0] == 0x80000000 && state[1] == 1 && state[2] == 1 && state[6] == 1 &&
              state[7] == 0x80000001);
        CHECK(state[17] == Expected(Alu::Add, 32, 0x80000000, 1, original).packed);
        CHECK(state[16] == window.pc + sizeof(bytes));
    }
}

void AutomaticFunctions() {
    for (const unsigned blocks : {1u, 4u, 32u}) {
        swift::tests::function_region::Fixture fixture{blocks};
        const auto window = fixture.Window();
        const auto limits = fixture.Limits();
        auto decoded = swift::x86::DecodeFunction({&window, 1}, fixture.entry, limits);
        CHECK(decoded && decoded.instructions.size() == blocks * 2 + 5);
        CHECK(decoded.decode_calls == decoded.instructions.size() && decoded.budget_exits == 0);
        CHECK(decoded.blocks.size() == blocks + 2 && decoded.entry_block > 0);
        CHECK(decoded.blocks[decoded.entry_block].pc == fixture.entry);
        for (const auto mode :
             {ir::TargetMode::Known, ir::TargetMode::EarlyOff, ir::TargetMode::Unknown})
            for (const bool supplied : {false, true}) {
                auto target = a64::KnownTargetInfo();
                target.features |= static_cast<u64>(ir::TargetFeature::PrivateGPRState);
                target.mode = mode;
                auto built =
                        supplied
                                ? swift::x86::BuildDecodedIR(decoded.instructions,
                                                             decoded.blocks,
                                                             {&window, 1},
                                                             ir::Scope::Function,
                                                             {0, 128, 8},
                                                             target,
                                                             nullptr,
                                                             decoded.entry_block)
                                : swift::x86::BuildFunctionIR(
                                          {&window, 1}, fixture.entry, {0, 128, 8}, limits, target);
                CHECK(built && built.program.EntryBlock().value == decoded.entry_block);
                auto code = Compile(built, true, true);
                std::array<u64, 18> state{};
                CHECK(Execute(code, state.data()) == 0);
                if constexpr (executes_native) {
                    CHECK(state[0] == blocks * fixture.cycles && state[1] == 0 &&
                          state[16] == fixture.exit);
                    CHECK((state[17] & (u64{15} << 28)) == (u64{1} << 30));
                }
            }
        // A bounded prefix exits at an actual undecoded instruction, keeping
        // the real entry instead of starting at the lower-PC loop body.
        auto bounded = swift::x86::BuildFunctionIR(
                {&window, 1}, fixture.entry, {0, 128, 8}, {2, limits.max_blocks});
        auto code = Compile(bounded, true, true);
        std::array<u64, 18> state{};
        state[0] = 99;
        CHECK(Execute(code, state.data()) == 0);
        if constexpr (executes_native)
            CHECK(state[0] == 0 && state[1] == fixture.cycles && state[16] == fixture.entry + 10);
    }
    // One stream is decoded straight through first; its terminal then names
    // a late entry at the middle SUB, which must split the existing slice.
    const std::uint8_t late[]{0x83, 0xc0, 1, 0x83, 0xe9, 1, 0x75, 0xfb, 0xeb, 0x20};
    const swift::x86::BlockWindow window{0x6000, late};
    const auto decoded = swift::x86::DecodeFunction({&window, 1}, window.pc);
    CHECK(decoded && decoded.splits == 1 && decoded.decode_calls == 4 &&
          decoded.instructions.size() == 4);
    auto built = swift::x86::BuildFunctionIR({&window, 1}, window.pc, {0, 128, 8});
    auto code = Compile(built, true, true);
    std::array<u64, 18> state{};
    state[0] = 7;
    state[1] = 3;
    CHECK(Execute(code, state.data()) == 0);
    if constexpr (executes_native) CHECK(state[0] == 8 && state[1] == 0 && state[16] == 0x602a);
}
void RepeatStorePipeline() {
    const std::uint8_t bytes[]{0xf3, 0x48, 0xab};  // REP STOSQ.
    const swift::x86::BlockWindow window{0x9000, bytes};
    constexpr u64 pattern = 0xfedcba9876543210ULL;
    for (const auto scope : {ir::Scope::Block, ir::Scope::Function}) {
        auto built = swift::x86::BuildIR({&window, 1}, scope, {0, 128, 8}, a64::KnownTargetInfo());
        auto code = Compile(built, true, true);
        if constexpr (executes_native) {
            swift::runtime::x86::GuestState empty;
            empty.gprs[swift::runtime::x86::RDI] =
                    1;  // Unmapped and unaligned: count0 must not touch it.
            empty.packed_flags = 0xa5a50000bc00003cULL;
            CHECK(Execute(code, reinterpret_cast<u64*>(&empty)) == 0 && empty.pc == 0x9003 &&
                  empty.gprs[swift::runtime::x86::RDI] == 1 &&
                  !empty.gprs[swift::runtime::x86::RCX] &&
                  empty.packed_flags == 0xa5a50000bc00003cULL);
            for (const bool reverse : {false, true}) {
                alignas(16) std::array<u64, 3> memory{};
                swift::runtime::x86::GuestState state;
                state.gprs[swift::runtime::x86::RAX] = pattern;
                state.gprs[swift::runtime::x86::RCX] = 3;
                const auto start =
                        reinterpret_cast<std::uintptr_t>(memory.data() + (reverse ? 2 : 0));
                state.gprs[swift::runtime::x86::RDI] = start;
                const auto flags =
                        (u64{0xa5a50000bc00003cULL} & ~u64{0x400}) | (reverse ? 0x400 : 0);
                state.packed_flags = flags;
                for (unsigned iteration = 0; iteration < 3; ++iteration) {
                    CHECK(Execute(code, reinterpret_cast<u64*>(&state)) == 0);
                    CHECK(state.gprs[swift::runtime::x86::RCX] == 2 - iteration &&
                          state.pc == (iteration == 2 ? 0x9003 : 0x9000));
                }
                CHECK(memory[0] == pattern && memory[1] == pattern && memory[2] == pattern &&
                      state.gprs[swift::runtime::x86::RDI] == (reverse ? start - 24 : start + 24) &&
                      state.packed_flags == flags);
            }
        }
    }
}

void ExtendedPrecisionPipeline() {
    using swift::runtime::x86::X87Command;
    swift::runtime::x86::X87State value;
    const auto step = [&](X87Command command, unsigned index = 0, u64 raw = 0) {
        return SvmX87Execute(&value, swift::runtime::x86::X87Encode(command, index), raw);
    };
    CHECK(step(X87Command::Init) == 0 && value.control == 0x037f && value.tags == 0xffff);
    CHECK(step(X87Command::PushOne) == 0);
    CHECK(step(X87Command::PushF64, 0, 0x3ff0000000000001ULL) == 0);
    CHECK(step(X87Command::Exchange, 1) == 0 && step(X87Command::Add, 1) == 0);
    CHECK(value.registers[6].sign_exp == 0x4000 &&
          value.registers[6].significand == 0x8000000000000400ULL && value.tags == 0x0fff);
    // Precision control is real: the same half-ULP result rounds at 53 bits.
    CHECK(step(X87Command::Init) == 0 && step(X87Command::SetControlWord, 0, 0x027f) == 0);
    CHECK(step(X87Command::PushOne) == 0 &&
          step(X87Command::PushF64, 0, 0x3ff0000000000001ULL) == 0 &&
          step(X87Command::Exchange, 1) == 0 && step(X87Command::Add, 1) == 0);
    CHECK(value.registers[6].significand == 0x8000000000000000ULL &&
          (value.status & 0x220) == 0x20);
    CHECK(step(X87Command::Init) == 0 && step(X87Command::SetControlWord, 0, 0x0a7f) == 0);
    CHECK(step(X87Command::PushOne) == 0 &&
          step(X87Command::PushF64, 0, 0x3ff0000000000001ULL) == 0 &&
          step(X87Command::Exchange, 1) == 0 && step(X87Command::Add, 1) == 0);
    CHECK(value.registers[6].significand == 0x8000000000000800ULL &&
          (value.status & 0x220) == 0x220);
    // Compare complete state, including physical payload and padding, against
    // the original instruction sequence for normal, masked and unmasked cases.
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        swift::runtime::x86::X87State original;
        original.tags = scenario == 3 ? 0xffff : 0;
        original.control = (scenario == 2 || scenario == 5) ? 0x037e : 0x037f;
        original.status = 0x0200;
        for (auto& reg : original.registers)
            reg = {0x8000000000000000ULL, 0x3fff, {1, 2, 3, 4, 5, 6}};
        if (scenario == 1 || scenario == 2) original.tags |= 3u << 2;
        auto combined = original;
        const bool exchange = scenario < 3;
        const auto grouped = exchange ? X87Command::ExchangeTwice : X87Command::PushOnePop;
        const auto index = exchange ? 1u : 0u;
        auto expected =
                SvmX87Execute(&original,
                              swift::runtime::x86::X87Encode(
                                      exchange ? X87Command::Exchange : X87Command::PushOne, index),
                              0);
        if (!expected && !exchange)
            expected =
                    SvmX87Execute(&original, swift::runtime::x86::X87Encode(X87Command::Sqrt), 0);
        if (!expected)
            expected = SvmX87Execute(
                    &original,
                    swift::runtime::x86::X87Encode(
                            exchange ? X87Command::Exchange : X87Command::StoreRegPop, index),
                    0);
        CHECK(SvmX87Execute(&combined, swift::runtime::x86::X87Encode(grouped, index), 0) ==
              expected);
        CHECK(std::memcmp(&original, &combined, sizeof original) == 0);
    }
    CHECK(step(X87Command::Init) == 0 && step(X87Command::SetControlWord, 0, 0x037e) == 0);
    CHECK(step(X87Command::Pop) ==
                  static_cast<u64>(swift::runtime::x86::X87Result::RaisedException) &&
          value.tags == 0xffff && (value.status & 0x8081) == 0x8081);

    alignas(8) const u64 factor = 0x3ff0000000000001ULL;
    for (const auto scope : {ir::Scope::Block, ir::Scope::Function}) {
        std::vector<std::uint8_t> bytes{0x48, 0xb8};  // movabs rax,&factor
        const auto address = reinterpret_cast<std::uintptr_t>(&factor);
        for (unsigned i = 0; i < 8; ++i)
            bytes.push_back(static_cast<std::uint8_t>(address >> (8 * i)));
        const std::uint8_t body[]{
                0xdb, 0xe3, 0xd9, 0xe8, 0xdd, 0x00, 0xd9, 0xc9, 0xd8, 0xc1, 0x0f, 0x05};
        bytes.insert(bytes.end(), std::begin(body), std::end(body));
        const swift::x86::BlockWindow window{0x8000, bytes};
        swift::x86::StateLayout layout{0, 128, 8};
        layout.x87_offset = offsetof(swift::runtime::x86::GuestState, x87);
        auto built = swift::x86::BuildIR({&window, 1}, scope, layout, a64::KnownTargetInfo());
        auto code = Compile(built, true, false);
        swift::runtime::x86::GuestState state;
        const auto reason = Execute(code, reinterpret_cast<u64*>(&state));
        if constexpr (executes_native) {
            CHECK(reason == 1 && state.gprs[swift::runtime::x86::RAX] == address);
            CHECK(state.x87.control == 0x037f && state.x87.tags == 0x0fff &&
                  state.x87.registers[6].sign_exp == 0x4000 &&
                  state.x87.registers[6].significand == 0x8000000000000400ULL);
        }
    }
}
}  // namespace

int main() {
    try {
        for (const auto scope : {ir::Scope::Block, ir::Scope::Function})
            for (const auto mode :
                 {ir::TargetMode::Known, ir::TargetMode::EarlyOff, ir::TargetMode::Unknown})
                for (const bool optimize : {false, true}) RunPipeline(scope, mode, optimize);
        const auto flag_paths = IntegerFlagBranches();
        DeferredFlagsAndMemoryObservation();
        MultiplyObservation();
        AutomaticFunctions();
        ExtendedPrecisionPipeline();
        RepeatStorePipeline();
#if defined(__aarch64__) && !defined(__APPLE__)
        std::printf(
                "12 base paths and %zu integer/flags/Jcc native pipeline paths passed; "
                "native_nzcv=%s\n",
                flag_paths,
                native_flags_available ? "true" : "false");
#else
        std::printf(
                "12 base paths and %zu integer/flags/Jcc pipeline paths passed; native execution "
                "requires ARM64 Linux/Android\n",
                flag_paths);
#endif
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
