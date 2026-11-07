#include "runtime/frontend/x86/interpreter.h"
#include "runtime/frontend/x86/build_ir.h"
#include "runtime/arch/x86/state.h"
#include "runtime/arch/x86/flags.h"
#include "runtime/backend/arm64/compile.h"
#include "runtime/ir/optimize.h"
#include "runtime/execution/dispatch.h"
#include "runtime/platform/fp_environment.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#if defined(__aarch64__) && !defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
namespace front = swift::x86;
namespace runtime = swift::runtime;
namespace arch = runtime::x86;
namespace a64 = runtime::backend::arm64;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using Bytes = std::array<std::uint8_t, 32>;
unsigned checks{};

void Check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

std::vector<std::uint8_t> Hex(const char* text) {
    std::vector<std::uint8_t> out;
    const auto digit = [](char c) {
        return c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0';
    };
    for (std::size_t i = 0; text[i]; i += 2)
        out.push_back(static_cast<std::uint8_t>(digit(text[i]) * 16 + digit(text[i + 1])));
    return out;
}

Bytes Words(std::initializer_list<u32> words) {
    Bytes out{};
    std::memcpy(out.data(), words.begin(), words.size() * sizeof(u32));
    return out;
}

void Set(arch::GuestState& state, unsigned reg, const Bytes& bytes) {
    std::memcpy(state.xmm[reg], bytes.data(), 16);
    std::memcpy(state.ymm_high[reg], bytes.data() + 16, 16);
}

void SetHex(arch::GuestState& state, unsigned reg, const char* text) {
    const auto bytes = Hex(text);
    Check(bytes.size() == 32, "fixed golden vector is 256 bits");
    std::memcpy(state.xmm[reg], bytes.data(), 16);
    std::memcpy(state.ymm_high[reg], bytes.data() + 16, 16);
}

Bytes Get(const arch::GuestState& state, unsigned reg) {
    Bytes out{};
    std::memcpy(out.data(), state.xmm[reg], 16);
    std::memcpy(out.data() + 16, state.ymm_high[reg], 16);
    return out;
}

struct Machine {
    static constexpr u64 pc = 0x1000, memory_pc = 0x8000;
    arch::GuestState state;
    std::vector<std::uint8_t> code;
    alignas(32) std::array<std::uint8_t, 128> memory{};
    u64 memory_address{memory_pc};
    u64 denied{UINT64_MAX};
    unsigned reads{}, writes{};

    explicit Machine(const char* bytes) : code(Hex(bytes)) {
        state.pc = pc;
        state.packed_flags = UINT64_C(0x123456789abc00d5);
        Set(state,
            0,
            Words({0xdecafbad,
                   0xdecafbad,
                   0xdecafbad,
                   0xdecafbad,
                   0xdecafbad,
                   0xdecafbad,
                   0xdecafbad,
                   0xdecafbad}));
        for (unsigned i = 0; i < 32; ++i) {
            const u32 word = 100 + i;
            std::memcpy(memory.data() + i * 4, &word, 4);
        }
    }

    front::DirectServices Services() {
        return {this,
                [](void* opaque,
                   u64 address,
                   std::span<std::uint8_t> out) noexcept -> front::CodeReadResult {
                    auto& m = *static_cast<Machine*>(opaque);
                    if (address < pc || address - pc >= m.code.size())
                        return {{front::AccessStatus::Fault, address}, 0};
                    const auto count = std::min(
                            out.size(), m.code.size() - static_cast<std::size_t>(address - pc));
                    std::memcpy(out.data(), m.code.data() + address - pc, count);
                    return {{}, count};
                },
                [](void* opaque, u64 address, std::span<std::uint8_t> out, bool) noexcept
                        -> front::AccessResult {
                    auto& m = *static_cast<Machine*>(opaque);
                    ++m.reads;
                    if (address == m.denied || address < m.memory_address ||
                        address - m.memory_address > m.memory.size() ||
                        out.size() > m.memory.size() - (address - m.memory_address))
                        return {front::AccessStatus::Fault, address};
                    std::memcpy(
                            out.data(), m.memory.data() + address - m.memory_address, out.size());
                    return {};
                },
                [](void* opaque, u64 address, std::span<const std::uint8_t> data, bool) noexcept
                        -> front::AccessResult {
                    auto& m = *static_cast<Machine*>(opaque);
                    ++m.writes;
                    if (address == m.denied || address < m.memory_address ||
                        address - m.memory_address > m.memory.size() ||
                        data.size() > m.memory.size() - (address - m.memory_address))
                        return {front::AccessStatus::Fault, address};
                    std::memcpy(
                            m.memory.data() + address - m.memory_address, data.data(), data.size());
                    return {};
                },
                nullptr,
                nullptr};
    }

    front::StepResult Step() { return front::InterpretOne(state, Services()); }
};

front::StateLayout Layout() {
    front::StateLayout layout;
    layout.gprs_offset = offsetof(arch::GuestState, gprs);
    layout.pc_offset = offsetof(arch::GuestState, pc);
    layout.fprs_offset = offsetof(arch::GuestState, xmm);
    layout.mxcsr_offset = offsetof(arch::GuestState, mxcsr);
    layout.ymm_high_offset = offsetof(arch::GuestState, ymm_high);
    return layout;
}

a64::CompileResult Compile(const Machine& m, bool pinned) {
    auto target = a64::KnownTargetInfo();
    if (pinned)
        target.features |= static_cast<u64>(runtime::ir::TargetFeature::PrivateGPRState) |
                           static_cast<u64>(runtime::ir::TargetFeature::PrivateVectorState) |
                           static_cast<u64>(runtime::ir::TargetFeature::FaultLocations);
    const front::BlockWindow window{Machine::pc, m.code};
    auto built = front::BuildIR({&window, 1}, runtime::ir::Scope::Block, Layout(), target);
    if (!built) {
        std::fprintf(stderr,
                     "AVX frontend error=%u pc=%llx detail=%u bytes=",
                     unsigned(built.error),
                     static_cast<unsigned long long>(built.error_pc),
                     unsigned(built.ir_error));
        for (const auto byte : m.code) std::fprintf(stderr, "%02x", byte);
        std::fputc('\n', stderr);
    }
    Check(bool(built), "fixed AVX instruction builds a real program");
    auto optimized = runtime::ir::Optimize(
            std::move(built.program),
            SIZE_MAX,
            {{runtime::ir::UniformRange{offsetof(arch::GuestState, packed_flags), 8}}, true});
    Check(bool(optimized), "AVX conditional loads survive the actual three-walk optimizer");
    a64::CompileOptions options;
    options.flags_offset = offsetof(arch::GuestState, packed_flags);
    options.pc_offset = offsetof(arch::GuestState, pc);
    options.mxcsr_flush_offset = offsetof(arch::GuestState, mxcsr);
    if (pinned) {
        options.private_uniforms = true;
        options.fault_locations = true;
#if defined(__aarch64__) && !defined(__APPLE__)
        options.dispatcher =
                reinterpret_cast<std::uintptr_t>(runtime::execution::SvmNativeDispatch);
        options.exit_handler = reinterpret_cast<std::uintptr_t>(runtime::execution::SvmNativeExit);
#else
        // Host qualification checks linked emission without executing A64.
        options.dispatcher = 0x1000;
        options.exit_handler = 0x2000;
#endif
        options.pinned_uniform_offset = offsetof(arch::GuestState, gprs);
        options.pinned_uniform_slots = 16;
        options.pinned_vector_offset = offsetof(arch::GuestState, xmm);
    }
    auto code = a64::Compile(optimized.program, options);
    if (!code) {
        std::fprintf(stderr,
                     "AVX backend error=%u node=%u block=%u bytes=",
                     unsigned(code.error),
                     code.node.value,
                     code.block.value);
        for (const auto byte : m.code) std::fprintf(stderr, "%02x", byte);
        std::fputc('\n', stderr);
    }
    Check(bool(code), "AVX program has a real ARM64 lowering");
    return code;
}

void Same(const arch::GuestState& actual, const arch::GuestState& expected) {
    Check(actual.pc == expected.pc, "AVX native PC commit");
    Check(actual.packed_flags == expected.packed_flags,
          "AVX preserves whole packed flags and reserved fields");
    Check(std::memcmp(actual.gprs, expected.gprs, sizeof(actual.gprs)) == 0,
          "AVX preserves unrelated GPRs");
    Check(actual.mxcsr == expected.mxcsr, "AVX does not synthesize MXCSR sticky state");
    for (unsigned reg = 0; reg < 16; ++reg)
        Check(Get(actual, reg) == Get(expected, reg), "AVX native full low/high vector state");
}

void Native(const Machine& input, const arch::GuestState& expected, bool pinned) {
    const auto code = Compile(input, pinned);
#if defined(__aarch64__) && !defined(__APPLE__)
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    const auto size = (code.words.size() * 4 + page - 1) / page * page;
    void* address = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(address != MAP_FAILED, "AVX native mapping");
    std::memcpy(address, code.words.data(), code.words.size() * 4);
    Check(mprotect(address, size, PROT_READ | PROT_EXEC) == 0, "AVX native W-to-X publication");
    __builtin___clear_cache(static_cast<char*>(address), static_cast<char*>(address) + size);
    auto state = input.state;
    runtime::platform::HostFloatScope environment(
            static_cast<runtime::platform::FloatRounding>((state.mxcsr >> 13) & 3));
    if (!pinned)
        Check(reinterpret_cast<runtime::backend::NativeCode::Function>(address)(
                      reinterpret_cast<std::uint8_t*>(&state)) == 0,
              "standalone AVX dispatch result");
    else {
        struct Resolver {
            std::uintptr_t entry;
            unsigned calls{};
        } resolver{reinterpret_cast<std::uintptr_t>(address)};

        const runtime::backend::NativeCode owner{
                reinterpret_cast<runtime::backend::NativeCode::Function>(address),
                code.words.size() * sizeof(code.words[0]),
                nullptr,
                code.faults};
        std::atomic<const runtime::backend::NativeCode*> active{};
        runtime::execution::NativeRunContext context;
        context.state = state;
        context.opaque = &resolver;
        context.active = &active;
        context.anchor = &owner;
        context.pinned_uniform_offset = offsetof(arch::GuestState, gprs);
        context.pinned_uniform_slots = 16;
        context.pinned_vector_offset = offsetof(arch::GuestState, xmm);
        context.flags_offset = offsetof(arch::GuestState, packed_flags);
        context.pc_offset = offsetof(arch::GuestState, pc);
        context.resolve =
                [](void* opaque,
                   runtime::execution::NativeRunContext* current) noexcept -> std::uintptr_t {
            auto& r = *static_cast<Resolver*>(opaque);
            ++r.calls;
            return current->state.pc == Machine::pc ? r.entry : 0;
        };
        const auto returned = runtime::execution::SvmRunNative(&context);
        state = context.state;
        if (returned != 2 || resolver.calls != 2)
            std::fprintf(stderr,
                         "AVX native pin=1 returned=%llu resolver_calls=%u\n",
                         static_cast<unsigned long long>(returned),
                         resolver.calls);
        Check(returned == 2 && resolver.calls == 2,
              "linked AVX pin16 leaves through the real cold boundary");
    }
    Same(state, expected);
    Check(munmap(address, size) == 0, "AVX native mapping release");
#else
    (void)expected;
#endif
}

void Golden(Machine input, const Bytes& answer, unsigned reads = 0) {
    auto direct = input;
    const auto result = direct.Step();
    Check(result.kind == front::StepKind::Completed, "AVX Direct execution accepts fixed bytes");
    Check(direct.reads == reads && direct.writes == 0,
          "AVX fixture performs only its enabled guest accesses");
    Check(Get(direct.state, 0) == answer, "AVX output matches a fixed independent answer");
    auto expected = input.state;
    Set(expected, 0, answer);
    expected.pc += input.code.size();
    Same(direct.state, expected);
    Native(input, expected, false);
    Native(input, expected, true);
}

void NdsAndPermutes() {
    for (const bool wide : {false, true}) {
        Machine m(wide ? "c5f5fec2" : "c5f1fec2");
        Set(m.state, 1, Words({1, 2, 3, 4, 5, 6, 7, 8}));
        Set(m.state, 2, Words({10, 20, 30, 40, 50, 60, 70, 80}));
        Golden(m, wide ? Words({11, 22, 33, 44, 55, 66, 77, 88}) : Words({11, 22, 33, 44}));
    }
    Machine shifts("c4e27145c2");  // vpsrlvd xmm0,xmm1,xmm2
    Set(shifts.state, 1, Words({0x80000000, 0xffffffff, 1, 0x80000000}));
    Set(shifts.state, 2, Words({31, 32, 0, 0xffffffff}));
    Golden(shifts, Words({1, 0, 1, 0}));
    Machine permute("c4e37546c231");  // vperm2i128 ymm0,ymm1,ymm2,31h
    Set(permute.state, 1, Words({1, 2, 3, 4, 5, 6, 7, 8}));
    Set(permute.state, 2, Words({10, 20, 30, 40, 50, 60, 70, 80}));
    Golden(permute, Words({5, 6, 7, 8, 50, 60, 70, 80}));
    Machine variable("c4e27536c2");  // vpermd ymm0,ymm1,ymm2
    Set(variable.state, 1, Words({7, 0, 5, 2, 6, 1, 4, 3}));
    Set(variable.state, 2, Words({10, 20, 30, 40, 50, 60, 70, 80}));
    Golden(variable, Words({80, 10, 60, 30, 70, 20, 50, 40}));
}

void FusedGolden() {
    // Literal byte/result rows from the pre-existing Rosetta hardware oracle:
    // source/tests/fuzz/avx_fma_rosetta_ref.inc in the frozen original tree.
    constexpr const char* a = "0000004000004040000080400000a0400000c0400000e0400000004100001041";
    constexpr const char* b = "000040400000a0400000e0400000104100003041000050410000704100008841";
    constexpr const char* c = "0000a04000000040000080bf000080c00000e0c0000020c1000050c1000080c1";

    const struct {
        const char* bytes;
        const char* answer;
    } order[] = {
            {"c4e27198c2", "0000884100005041000040c00000f8c100000000000000000000000000000000"},
            {"c4e271a8c2", "000050410000304100004040000030c100000000000000000000000000000000"},
            {"c4e275b8c2", "00003041000088410000d8410000244200006c420000a2420000d64200000943"},
    };

    for (const auto& row : order) {
        Machine m(row.bytes);
        SetHex(m.state, 0, c);
        SetHex(m.state, 1, a);
        SetHex(m.state, 2, b);
        const auto answer = Hex(row.answer);
        Bytes bytes{};
        std::copy(answer.begin(), answer.end(), bytes.begin());
        Golden(m, bytes);
    }
    Machine fused("c4e271b8c2");
    SetHex(fused.state, 1, "0008803f0010803f0008803f0004803f0002803f0008803f0004803f0010803f");
    SetHex(fused.state, 2, "0008803f0004803f0004803f0002803f0008803f0002803f0008803f0002803f");
    SetHex(fused.state, 0, "000480bf0008803f000280bf0008803f000480bf0010803f000280bf0004803f");
    Golden(fused, Words({0x39c00800, 0x40000e00, 0x39a00400, 0x40000700}));
    Machine scalar("c4e271b9c2");
    SetHex(scalar.state, 1, "1101c07f1101c07f000000401101c07f4404807f1101c07f0000807f1101c07f");
    SetHex(scalar.state, 2, "2202c0ff000000402202c0ff2202c0ff2202c0ff5505807f000000000000807f");
    SetHex(scalar.state, 0, "000040403303c07f3303c07f3303c07f00004040000040403303c07f000080ff");
    Golden(scalar, Words({0x7fc00111, 0x7fc00333, 0x7fc00333, 0x7fc00333}));
}

void VexStrings() {
    // Equal-any over {'a','b'} and {'x','b','a'} has IntRes2=6.
    // W0 takes signed EAX=-2, while W1 distinguishes the positive full
    // RAX=0x00000000fffffffe (clamped to 16), including the 'x' at A[2].
    const struct {
        const char* code;
        bool explicit_lengths, mask, unit_mask, full_a;
        u64 length_a;
        unsigned index;
    } cases[] = {
            {"c4e37960ca00", true, true, false, false, UINT64_C(0xfffffffe), 0},
            {"c4e37961ca00", true, false, false, false, UINT64_C(0xfffffffe), 1},
            {"c4e37962ca40", false, true, true, false, 0, 0},
            {"c4e37963ca40", false, false, false, false, 0, 2},
            {"c4e3f961ca00", true, false, false, true, UINT64_C(0xfffffffe), 0},
            {"c4e3f960ca40", true, true, true, false, UINT64_MAX - 1, 0},
    };

    const auto flags_for = [](u64 previous, bool full_a) {
        return (previous & ~arch::flags::Arithmetic) | arch::flags::Carry | arch::flags::Zero |
               (full_a ? arch::flags::Overflow : arch::flags::Sign) | u64{1};
    };
    for (const auto& row : cases) {
        Machine m(row.code);
        Bytes a{}, b{};
        a[0] = 'a';
        a[1] = 'b';
        if (row.explicit_lengths) {
            a[2] = 'x';
            std::fill(a.begin() + 3, a.begin() + 16, 'z');
        }
        b[0] = 'x';
        b[1] = 'b';
        b[2] = 'a';
        Set(m.state, 1, a);
        Set(m.state, 2, b);
        m.state.gprs[arch::RAX] = row.length_a;
        m.state.gprs[arch::RDX] = 3;
        m.state.gprs[arch::RCX] = UINT64_MAX;
        auto expected = m.state;
        expected.pc += m.code.size();
        expected.packed_flags = flags_for(expected.packed_flags, row.full_a);
        if (row.mask) {
            Bytes mask{};
            if (row.unit_mask)
                mask[1] = mask[2] = 255;
            else
                mask[0] = 6;
            Set(expected, 0, mask);  // VEX mask forms also clear YMM0[255:128].
        } else
            expected.gprs[arch::RCX] = row.index;  // Every index form zeroes RCX[63:32].
        auto direct = m;
        Check(direct.Step().kind == front::StepKind::Completed && direct.reads == 0,
              "four VEX string compares execute");
        Same(direct.state, expected);
        Native(m, expected, false);
        Native(m, expected, true);
    }
    Machine memory(
            "c4e379630f00");  // vpcmpistri xmm1,[rdi],0; m128 explicitly permits misalignment.
    Bytes a{};
    a[0] = 'a';
    a[1] = 'b';
    Set(memory.state, 1, a);
    std::fill(memory.memory.begin() + 1, memory.memory.begin() + 17, 0);
    memory.memory[1] = 'x';
    memory.memory[2] = 'b';
    memory.memory[3] = 'a';
    memory.memory_address = reinterpret_cast<std::uintptr_t>(memory.memory.data());
    memory.state.gprs[arch::RDI] = memory.memory_address + 1;
    memory.state.gprs[arch::RCX] = UINT64_MAX;
    auto expected = memory.state;
    expected.pc += memory.code.size();
    expected.gprs[arch::RCX] = 1;
    expected.packed_flags = flags_for(expected.packed_flags, false);
    auto direct = memory;
    Check(direct.Step().kind == front::StepKind::Completed && direct.reads == 1,
          "VEX string m128 reads once without alignment restriction");
    Same(direct.state, expected);
    Native(memory, expected, false);
    Native(memory, expected, true);
    for (const auto* invalid : {"c4e37d60ca00", "c4e37163ca00"}) {
        const auto bytes = Hex(invalid);
        const auto decoded = front::DecodeOne(bytes, Machine::pc);
        Check(!decoded && decoded.error == front::DecodeError::InvalidEncoding,
              "VEX string encodings reject L=1 and non-reserved vvvv");
    }
}

void MaskAndGatherFaults() {
    Machine mask("c4e26d2c07");  // vmaskmovps ymm0,ymm2,[rdi]
    mask.state.gprs[arch::RDI] = Machine::memory_pc;
    Set(mask.state, 2, Words({0x80000000, 0x80000000, 0, 0, 0, 0, 0, 0}));
    const auto before = mask.state;
    mask.denied = Machine::memory_pc + 4;
    const auto failed = mask.Step();
    Check(failed.kind == front::StepKind::Fault && failed.fault_address == mask.denied &&
                  mask.reads == 2,
          "masked load accesses exactly the enabled prefix");
    Same(mask.state, before);  // No destination commit until all loads succeed.
    (void)Compile(mask, false);
    (void)Compile(mask, true);
    Machine zero("c4e26d2c07");
    zero.state.gprs[arch::RDI] = 1;
    Set(zero.state, 2, Words({0x7fffffff, 0, 1, 0, 0, 0, 0, 0}));
    Golden(zero, {});
    Machine enabled("c4e26d2c07");
    enabled.memory_address = reinterpret_cast<std::uintptr_t>(enabled.memory.data());
    enabled.state.gprs[arch::RDI] = enabled.memory_address;
    Set(enabled.state, 2, Words({0x80000000, 0, 0, 0, 0, 0x80000000, 0, 0}));
    Golden(enabled, Words({100, 0, 0, 0, 0, 105, 0, 0}), 2);
    Machine gather("c4e26d90048e");  // vpgatherdd ymm0,[rsi+ymm1*4],ymm2
    gather.state.gprs[arch::RSI] = Machine::memory_pc;
    Set(gather.state, 1, Words({0, 1, 2, 3, 4, 5, 6, 7}));
    Set(gather.state,
        2,
        Words({0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000}));
    const auto incoming_flags = gather.state.packed_flags;
    gather.denied = Machine::memory_pc + 4;
    Check(gather.Step().kind == front::StepKind::Fault && gather.reads == 2 &&
                  gather.state.pc == Machine::pc,
          "gather stops at its first enabled fault");
    Check(Get(gather.state, 0) == Words({100,
                                         0xdecafbad,
                                         0xdecafbad,
                                         0xdecafbad,
                                         0xdecafbad,
                                         0xdecafbad,
                                         0xdecafbad,
                                         0xdecafbad}) &&
                  Get(gather.state, 2) == Words({0,
                                                 0x80000000,
                                                 0x80000000,
                                                 0x80000000,
                                                 0x80000000,
                                                 0x80000000,
                                                 0x80000000,
                                                 0x80000000}) &&
                  gather.state.packed_flags == incoming_flags,
          "gather commits only completed elements and clears their mask lanes");
    (void)Compile(gather, false);
    (void)Compile(gather, true);
    gather.denied = UINT64_MAX;
    Check(gather.Step().kind == front::StepKind::Completed &&
                  Get(gather.state, 0) == Words({100, 101, 102, 103, 104, 105, 106, 107}) &&
                  Get(gather.state, 2) == Bytes{} && gather.reads == 9,
          "gather restart skips the completed lane and clears its final mask");
    Machine successful("c4e26d90048e");
    successful.memory_address = reinterpret_cast<std::uintptr_t>(successful.memory.data());
    successful.state.gprs[arch::RSI] = successful.memory_address;
    Set(successful.state, 1, Words({0, 1, 2, 3, 4, 5, 6, 7}));
    Set(successful.state,
        2,
        Words({0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000}));
    auto expected = successful.state;
    expected.pc += successful.code.size();
    Set(expected, 0, Words({100, 101, 102, 103, 104, 105, 106, 107}));
    Set(expected, 2, {});
    auto direct = successful;
    Check(direct.Step().kind == front::StepKind::Completed && direct.reads == 8,
          "gather all active lanes succeed");
    Same(direct.state, expected);
    Native(successful, expected, false);
    Native(successful, expected, true);
    Machine address32("67c4e26d90048e");
    address32.state.gprs[arch::RSI] = UINT64_C(0x123400008004);
    Set(address32.state, 1, Words({0xffffffff, 0, 1, 2, 3, 4, 5, 6}));
    Set(address32.state,
        2,
        Words({0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000,
               0x80000000}));
    Check(address32.Step().kind == front::StepKind::Completed &&
                  Get(address32.state, 0) == Words({100, 101, 102, 103, 104, 105, 106, 107}) &&
                  Get(address32.state, 2) == Bytes{} &&
                  address32.state.gprs[arch::RSI] == UINT64_C(0x123400008004),
          "VSIB32 sign-extends each index then wraps the complete EA without changing the base "
          "GPR");
    (void)Compile(address32, false);
    (void)Compile(address32, true);
}
}  // namespace

int main() {
    try {
        NdsAndPermutes();
        FusedGolden();
        VexStrings();
        MaskAndGatherFaults();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "AVX coverage: %s\n", error.what());
        return 1;
    }
    std::printf("AVX fixed coverage passed (%u checks)\n", checks);
}
