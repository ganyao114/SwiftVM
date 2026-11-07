#include "runtime/frontend/x86/interpreter.h"
#include "runtime/frontend/x86/build_ir.h"
#include "runtime/backend/arm64/compile.h"
#include "runtime/arch/x86/state.h"
#include "runtime/arch/x86/flags.h"
#include "runtime/arch/x86/cpu.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__aarch64__) && !defined(__APPLE__)
#include "runtime/execution/dispatch.h"
#include <asm/hwcap.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
namespace front = swift::x86;
namespace arch = swift::runtime::x86;
namespace ir = swift::runtime::ir;
namespace a64 = swift::runtime::backend::arm64;
using u64 = std::uint64_t;

void Check(bool condition, int line) {
    if (!condition)
        throw std::runtime_error("instruction coverage at line " + std::to_string(line));
}

#define CHECK(...) Check(bool(__VA_ARGS__), __LINE__)
constexpr u64 reserved = UINT64_C(0x8501000000000000);

front::StateLayout Layout() {
    front::StateLayout layout{offsetof(arch::GuestState, gprs), offsetof(arch::GuestState, pc), 8};
    layout.fprs_offset = offsetof(arch::GuestState, xmm);
    layout.fs_base_offset = offsetof(arch::GuestState, fs_base);
    layout.gs_base_offset = offsetof(arch::GuestState, gs_base);
    layout.mxcsr_offset = offsetof(arch::GuestState, mxcsr);
    layout.cpu_query = reinterpret_cast<std::uintptr_t>(&arch::QueryGuestCpu);
    layout.system_query = reinterpret_cast<std::uintptr_t>(&arch::QueryGuestSystem);
    return layout;
}

struct Compiled {
    a64::CompileResult plain, pinned;
};

Compiled Admission(std::span<const std::uint8_t> bytes, u64 pc, bool native) {
    const auto decoded = front::DecodeOne(bytes, pc);
    CHECK(decoded);
    const front::BlockWindow window{pc, bytes.first(decoded.instruction.View().Size())};
    auto built = front::BuildIR({&window, 1}, ir::Scope::Block, Layout(), a64::KnownTargetInfo());
    if (!built)
        std::fprintf(stderr,
                     "front error=%u pc=%llx detail=%u\n",
                     unsigned(built.error),
                     static_cast<unsigned long long>(built.error_pc),
                     unsigned(built.ir_error));
    if (!built) {
        for (const auto byte : window.bytes) std::fprintf(stderr, "%02x", byte);
        std::fputc('\n', stderr);
    }
    CHECK(built);
    a64::CompileOptions options;
    options.flags_offset = offsetof(arch::GuestState, packed_flags);
    auto code = a64::Compile(built.program, options);
    if (!code)
        std::fprintf(stderr, "backend error=%u node=%u\n", unsigned(code.error), code.node.value);
    if (!code) {
        for (const auto byte : window.bytes) std::fprintf(stderr, "%02x", byte);
        std::fputc('\n', stderr);
    }
    CHECK(code && !code.words.empty());
    Compiled result{std::move(code), {}};
#if defined(__aarch64__) && !defined(__APPLE__)
    if (native) {
        namespace execution = swift::runtime::execution;
        options.dispatcher = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatch);
        options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
        options.pc_offset = offsetof(arch::GuestState, pc);
        options.fault_locations = true;
        options.private_uniforms = true;
        options.pinned_uniform_offset = 0;
        options.pinned_uniform_slots = 16;
        options.pinned_vector_offset = offsetof(arch::GuestState, xmm);
        const auto caps = getauxval(AT_HWCAP);
        options.aes = (caps & HWCAP_AES) != 0;
        options.pmull = (caps & HWCAP_PMULL) != 0;
        options.sha256 = (caps & HWCAP_SHA2) != 0;
        // The packed linked ABI also exercises pin scratch8/9 on hosts that
        // have no FlagM. Crypto capabilities remain actual host facts.
        result.pinned = a64::Compile(built.program, options);
        CHECK(result.pinned);
    }
#else
    (void)native;
#endif
    return result;
}
#if defined(__aarch64__) && !defined(__APPLE__)
void Execute(const a64::CompileResult& compiled, arch::GuestState& state, bool pinned) {
    namespace execution = swift::runtime::execution;
    const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
    CHECK(page && page != static_cast<std::size_t>(-1));
    const auto bytes = compiled.words.size() * 4, allocation = (bytes + page - 1) / page * page;
    void* memory =
            mmap(nullptr, allocation, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(memory != MAP_FAILED);

    struct Release {
        void* memory;
        std::size_t size;

        ~Release() { munmap(memory, size); }
    } release{memory, allocation};

    std::memcpy(memory, compiled.words.data(), bytes);
    CHECK(mprotect(memory, allocation, PROT_READ | PROT_EXEC) == 0);
    __builtin___clear_cache(static_cast<char*>(memory), static_cast<char*>(memory) + bytes);
    if (!pinned) {
        CHECK(reinterpret_cast<u64 (*)(std::uint8_t*)>(memory)(
                      reinterpret_cast<std::uint8_t*>(&state)) == 0);
        return;
    }

    struct Scene {
        std::uintptr_t entry;
        unsigned calls{};
    } scene{reinterpret_cast<std::uintptr_t>(memory)};

    execution::NativeRunContext context;
    execution::NativeCode owner{
            reinterpret_cast<execution::NativeCode::Function>(memory), bytes, nullptr, {}};
    std::atomic<const execution::NativeCode*> active{};
    context.state = state;
    context.opaque = &scene;
    context.active = &active;
    context.anchor = &owner;
    context.pinned_uniform_offset = 0;
    context.pinned_uniform_slots = 16;
    context.pinned_vector_offset = offsetof(arch::GuestState, xmm);
    context.flags_offset = offsetof(arch::GuestState, packed_flags);
    context.pc_offset = offsetof(arch::GuestState, pc);
    context.resolve = [](void* opaque, execution::NativeRunContext*) noexcept -> std::uintptr_t {
        auto& current = *static_cast<Scene*>(opaque);
        return current.calls++ == 0 ? current.entry : 0;
    };
    CHECK(execution::SvmRunNative(&context) == 2 && scene.calls == 2);
    state = context.state;
}

void SameState(const arch::GuestState& expected, const arch::GuestState& actual) {
    CHECK(std::memcmp(expected.gprs, actual.gprs, sizeof(expected.gprs)) == 0);
    CHECK(expected.pc == actual.pc && arch::flags::ToRflags(expected.packed_flags) ==
                                              arch::flags::ToRflags(actual.packed_flags));
    CHECK((actual.packed_flags & reserved) == reserved);
    CHECK(std::memcmp(expected.xmm, actual.xmm, sizeof(expected.xmm)) == 0);
}
#endif
struct Machine {
    static constexpr u64 code_pc = 0x1000, memory_pc = 0x8000;
    arch::GuestState state;
    std::vector<std::uint8_t> code;
    std::array<std::uint8_t, 256> memory{};
    u64 denied_read{UINT64_MAX}, denied_write{UINT64_MAX};
    unsigned reads{}, writes{};

    Machine(std::initializer_list<std::uint8_t> bytes) : code(bytes) {
        state.pc = code_pc;
        state.gprs[arch::RSP] = memory_pc + 224;
        state.packed_flags = arch::flags::FromRflags(0x202, reserved);
    }

    static bool Contains(u64 address, std::size_t bytes) {
        return address >= memory_pc && address - memory_pc <= 256 &&
               bytes <= 256 - (address - memory_pc);
    }

    front::DirectServices Services() {
        front::DirectServices services;
        services.opaque = this;
        services.fetch = [](void* opaque,
                            u64 pc,
                            std::span<std::uint8_t> bytes) noexcept -> front::CodeReadResult {
            auto& m = *static_cast<Machine*>(opaque);
            if (pc < code_pc || pc - code_pc >= m.code.size())
                return {{front::AccessStatus::Fault, pc}, 0};
            const auto count =
                    std::min(bytes.size(), m.code.size() - static_cast<std::size_t>(pc - code_pc));
            std::memcpy(bytes.data(), m.code.data() + pc - code_pc, count);
            return {{}, count};
        };
        services.read = [](void* opaque,
                           u64 address,
                           std::span<std::uint8_t> bytes,
                           bool) noexcept -> front::AccessResult {
            auto& m = *static_cast<Machine*>(opaque);
            ++m.reads;
            if (!Contains(address, bytes.size()) || address == m.denied_read)
                return {front::AccessStatus::Fault, address};
            std::memcpy(bytes.data(), m.memory.data() + address - memory_pc, bytes.size());
            return {};
        };
        services.write = [](void* opaque,
                            u64 address,
                            std::span<const std::uint8_t> bytes,
                            bool) noexcept -> front::AccessResult {
            auto& m = *static_cast<Machine*>(opaque);
            ++m.writes;
            if (!Contains(address, bytes.size()) || address == m.denied_write)
                return {front::AccessStatus::Fault, address};
            std::memcpy(m.memory.data() + address - memory_pc, bytes.data(), bytes.size());
            return {};
        };
        return services;
    }

    front::StepResult Step(bool native = false) {
        CHECK(state.pc >= code_pc && state.pc - code_pc < code.size());
        const auto compiled = Admission(
                std::span<const std::uint8_t>{code}.subspan(state.pc - code_pc), state.pc, native);
        [[maybe_unused]] const auto initial = state;
        const auto services = Services();
        const auto step = front::InterpretOne(state, services);
#if defined(__aarch64__) && !defined(__APPLE__)
        if (native) {
            CHECK(step.kind == front::StepKind::Completed);
            auto plain = initial, pinned = initial;
            Execute(compiled.plain, plain, false);
            Execute(compiled.pinned, pinned, true);
            SameState(state, plain);
            SameState(state, pinned);
        }
#else
        (void)compiled;
#endif
        return step;
    }

    void Done(bool native = false) {
        CHECK(Step(native).kind == front::StepKind::Completed);
        CHECK((state.packed_flags & reserved) == reserved);
    }

    void SetVector(unsigned index, u64 low, u64 high = 0) {
        std::memcpy(state.xmm[index], &low, 8);
        std::memcpy(state.xmm[index] + 8, &high, 8);
    }

    u64 Vector(unsigned index, unsigned half = 0) const {
        u64 value{};
        std::memcpy(&value, state.xmm[index] + half * 8, 8);
        return value;
    }
};

void FlagsAndShifts() {
    Machine flags{0xf9,
                  0x9f,
                  0xf5,
                  0x9e,
                  0xfd,
                  0xfc,
                  0x9c,
                  0x9d};  // STC/LAHF/CMC/SAHF/STD/CLD/PUSHF/POPF.
    flags.state.packed_flags = arch::flags::FromRflags(0xad6, reserved);
    flags.Done(true);
    CHECK(arch::flags::GetCF(flags.state.packed_flags));
    flags.Done(true);
    CHECK(((flags.state.gprs[arch::RAX] >> 8) & 255) == 0xd7);
    flags.Done(true);
    CHECK(!arch::flags::GetCF(flags.state.packed_flags));
    flags.Done(true);
    CHECK(arch::flags::GetCF(flags.state.packed_flags) &&
          arch::flags::GetOF(flags.state.packed_flags));
    flags.Done(true);
    CHECK(arch::flags::GetDF(flags.state.packed_flags));
    flags.Done(true);
    CHECK(!arch::flags::GetDF(flags.state.packed_flags));
    const auto before = flags.state.packed_flags;
    flags.Done();
    flags.Done();
    CHECK(arch::flags::ToRflags(flags.state.packed_flags) == arch::flags::ToRflags(before));

    Machine shld{0x48, 0x0f, 0xa4, 0xc8, 4};
    shld.state.gprs[arch::RAX] = UINT64_C(0x8000000000000001);
    shld.state.gprs[arch::RCX] = UINT64_C(0xf000000000000000);
    shld.Done(true);
    CHECK(shld.state.gprs[arch::RAX] == 0x1f && !arch::flags::GetCF(shld.state.packed_flags));
    Machine shrd{0x48, 0x0f, 0xac, 0xc8, 1};
    shrd.state.gprs[arch::RAX] = shrd.state.gprs[arch::RCX] = 1;
    shrd.Done(true);
    CHECK(shrd.state.gprs[arch::RAX] == (u64{1} << 63) &&
          arch::flags::GetCF(shrd.state.packed_flags) &&
          arch::flags::GetOF(shrd.state.packed_flags));
    Machine rotate{0xd0, 0xd0, 0xd0, 0xd8};
    rotate.state.gprs[arch::RAX] = 0x80;
    rotate.state.packed_flags = arch::flags::FromRflags(0x203, reserved);
    rotate.Done(true);
    CHECK(rotate.state.gprs[arch::RAX] == 1 && arch::flags::GetCF(rotate.state.packed_flags) &&
          arch::flags::GetOF(rotate.state.packed_flags));
    rotate.Done(true);
    CHECK(rotate.state.gprs[arch::RAX] == 0x80 && arch::flags::GetCF(rotate.state.packed_flags));
}

void FaultAndAddress() {
    Machine bit{0x48, 0x0f, 0xab, 0x0b, 0x48, 0x0f, 0xb3, 0x0b};  // BTS/BTR [RBX],RCX, negative
                                                                  // bit-string offset.
    bit.state.gprs[arch::RBX] = Machine::memory_pc + 16;
    bit.state.gprs[arch::RCX] = UINT64_MAX;
    const auto flags = bit.state.packed_flags;
    bit.denied_write = Machine::memory_pc + 8;
    CHECK(bit.Step().kind == front::StepKind::Fault && bit.state.pc == Machine::code_pc &&
          bit.state.packed_flags == flags && bit.memory[15] == 0);
    bit.denied_write = UINT64_MAX;
    bit.Done();
    CHECK(bit.memory[15] == 128 && !arch::flags::GetCF(bit.state.packed_flags));
    bit.Done();
    CHECK(bit.memory[15] == 0 && arch::flags::GetCF(bit.state.packed_flags));

    Machine xlat{0x67, 0x64, 0xd7};
    xlat.state.gprs[arch::RBX] = UINT64_C(0x1234000000000004);
    xlat.state.gprs[arch::RAX] = 2;
    xlat.state.fs_base = Machine::memory_pc;
    xlat.memory[6] = 0xa5;
    xlat.Done();
    CHECK(xlat.state.gprs[arch::RAX] == 0xa5 && xlat.reads == 1);
    Machine movnti{0x48, 0x0f, 0xc3, 0x03};
    movnti.state.gprs[arch::RBX] = Machine::memory_pc;
    movnti.state.gprs[arch::RAX] = UINT64_C(0x8877665544332211);
    movnti.Done();
    CHECK(movnti.memory[0] == 0x11 && movnti.memory[7] == 0x88);
    Machine prefetch{0x0f, 0x0d, 0x08};  // PREFETCHW [RAX] is a non-faulting hint.
    prefetch.Done();
    CHECK(prefetch.reads == 0 && prefetch.writes == 0);
    Machine random{0x66, 0x0f, 0xc7, 0xf0};
    random.state.gprs[arch::RAX] = UINT64_C(0xabcd123400000000);
    random.state.packed_flags = arch::flags::FromRflags(0xad7, reserved);
    random.Done();
    CHECK((random.state.gprs[arch::RAX] & ~u64{0xffff}) == UINT64_C(0xabcd123400000000));
    CHECK(arch::flags::ToRflags(random.state.packed_flags) ==
          (arch::flags::GetCF(random.state.packed_flags) ? 0x203u : 0x202u));
    CHECK(arch::flags::GetCF(random.state.packed_flags) ||
          (random.state.gprs[arch::RAX] & 0xffff) == 0);
    Machine seed{0x0f, 0xc7, 0xf8};
    seed.state.gprs[arch::RAX] = UINT64_MAX;
    seed.Done();
    CHECK(seed.state.gprs[arch::RAX] <= UINT32_MAX &&
          (arch::flags::GetCF(seed.state.packed_flags) || seed.state.gprs[arch::RAX] == 0));
}

void ControlAndStrings() {
    Machine loop{0xe2, 0xfe};
    loop.state.gprs[arch::RCX] = 3;
    loop.Done();
    CHECK(loop.state.gprs[arch::RCX] == 2 && loop.state.pc == Machine::code_pc);
    loop.Done();
    loop.Done();
    CHECK(loop.state.gprs[arch::RCX] == 0 && loop.state.pc == Machine::code_pc + 2);
    Machine jecxz{0x67, 0xe3, 2};
    jecxz.state.gprs[arch::RCX] = u64{1} << 32;
    jecxz.Done();
    CHECK(jecxz.state.pc == Machine::code_pc + 5);
    Machine repeat{0x67, 0xf3, 0xa4};
    repeat.state.gprs[arch::RSI] = UINT64_C(0x120000008000);
    repeat.state.gprs[arch::RDI] = UINT64_C(0x340000008040);
    repeat.state.gprs[arch::RCX] = UINT64_C(0x560000000003);
    repeat.memory[0] = 1;
    repeat.memory[1] = 2;
    repeat.memory[2] = 3;
    repeat.Done();
    CHECK(repeat.state.gprs[arch::RSI] == 0x8001 && repeat.state.gprs[arch::RDI] == 0x8041 &&
          repeat.state.gprs[arch::RCX] == 2);
    repeat.denied_write = 0x8041;
    CHECK(repeat.Step().kind == front::StepKind::Fault && repeat.state.gprs[arch::RCX] == 2 &&
          repeat.state.gprs[arch::RSI] == 0x8001 && repeat.memory[65] == 0);
    repeat.denied_write = UINT64_MAX;
    repeat.Done();
    repeat.Done();
    CHECK(repeat.state.pc == Machine::code_pc + 3 && repeat.memory[64] == 1 &&
          repeat.memory[66] == 3);
    Machine zero{0xf3, 0xa4};
    zero.state.gprs[arch::RCX] = 0;
    CHECK(zero.Step().kind == front::StepKind::Completed && zero.reads == 0 && zero.writes == 0);
    Machine compare{0xf3, 0xa6};
    compare.state.gprs[arch::RSI] = 0x8000;
    compare.state.gprs[arch::RDI] = 0x8040;
    compare.state.gprs[arch::RCX] = 3;
    compare.memory[0] = compare.memory[64] = 1;
    compare.memory[1] = 2;
    compare.memory[65] = 3;
    compare.Done();
    CHECK(compare.state.pc == Machine::code_pc && arch::flags::GetZF(compare.state.packed_flags));
    compare.Done();
    CHECK(compare.state.pc == Machine::code_pc + 2 && compare.state.gprs[arch::RCX] == 1 &&
          !arch::flags::GetZF(compare.state.packed_flags));
}

void BmiAndAdx() {
    // Fixed VEX encodings use RCX as vvvv and RSI as r/m, matching the old
    // Rosetta-reference generator's low-register assignment.
    Machine andn{0xc4, 0xe2, 0xf0, 0xf2, 0xc6};
    andn.state.gprs[arch::RCX] = UINT64_C(0xaaaaaaaaaaaaaaaa);
    andn.state.gprs[arch::RSI] = UINT64_C(0x5555555555555555);
    andn.Done(true);
    CHECK(andn.state.gprs[arch::RAX] == UINT64_C(0x5555555555555555) &&
          !arch::flags::GetCF(andn.state.packed_flags));
    Machine extract{0xc4, 0xe2, 0xf0, 0xf7, 0xc6};
    extract.state.gprs[arch::RCX] = 0x0804;
    extract.state.gprs[arch::RSI] = UINT64_C(0xfedcba9876543210);
    extract.Done(true);
    CHECK(extract.state.gprs[arch::RAX] == 0x21);
    Machine bzhi{0xc4, 0xe2, 0x70, 0xf5, 0xc6};
    bzhi.state.gprs[arch::RCX] = 32;
    bzhi.state.gprs[arch::RSI] = UINT64_C(0xfacefeeddeadbeef);
    bzhi.Done(true);
    CHECK(bzhi.state.gprs[arch::RAX] == 0xdeadbeef && arch::flags::GetCF(bzhi.state.packed_flags));
    Machine deposit{0xc4, 0xe2, 0xf3, 0xf5, 0xc6};
    deposit.state.gprs[arch::RCX] = 0xb;
    deposit.state.gprs[arch::RSI] = 0x16a;
    deposit.Done(true);
    CHECK(deposit.state.gprs[arch::RAX] == 0x4a);
    Machine gather{0xc4, 0xe2, 0xf2, 0xf5, 0xc6};
    gather.state.gprs[arch::RCX] = 0x14a;
    gather.state.gprs[arch::RSI] = 0x16a;
    gather.Done(true);
    CHECK(gather.state.gprs[arch::RAX] == 0x1b);
    Machine mulx{0xc4, 0xe2, 0xe3, 0xf6, 0xc6};
    mulx.state.gprs[arch::RDX] = UINT64_MAX;
    mulx.state.gprs[arch::RSI] = 2;
    const auto flags = mulx.state.packed_flags;
    mulx.Done(true);
    CHECK(mulx.state.gprs[arch::RAX] == 1 && mulx.state.gprs[arch::RBX] == UINT64_MAX - 1 &&
          mulx.state.packed_flags == flags);
    Machine adx{0x66, 0x48, 0x0f, 0x38, 0xf6, 0xc1, 0xf3, 0x48, 0x0f, 0x38, 0xf6, 0xc1};
    adx.state.gprs[arch::RAX] = UINT64_MAX;
    adx.state.packed_flags = arch::flags::FromRflags(0xad7, reserved);
    adx.Done(true);
    CHECK(adx.state.gprs[arch::RAX] == 0 && arch::flags::GetCF(adx.state.packed_flags) &&
          arch::flags::GetOF(adx.state.packed_flags));
    adx.Done(true);
    CHECK(adx.state.gprs[arch::RAX] == 1 && arch::flags::GetCF(adx.state.packed_flags) &&
          !arch::flags::GetOF(adx.state.packed_flags));
}

void CryptoAndStringCompare() {
    Machine aes{0x66, 0x0f, 0x38, 0xdc, 0xc1};
    aes.Done(true);
    CHECK(aes.Vector(0) == UINT64_C(0x6363636363636363) &&
          aes.Vector(0, 1) == UINT64_C(0x6363636363636363));
    Machine decrypt{0x66, 0x0f, 0x38, 0xde, 0xc1};
    decrypt.Done(true);
    CHECK(decrypt.Vector(0) == UINT64_C(0x5252525252525252));
    Machine key{0x66, 0x0f, 0x3a, 0xdf, 0xc1, 1};
    key.Done(true);
    CHECK(key.Vector(0) == UINT64_C(0x6363636263636363) && key.Vector(0, 1) == key.Vector(0));
    Machine polynomial{0x66, 0x0f, 0x3a, 0x44, 0xc1, 0x10};
    polynomial.SetVector(0, 3, 0xaaaa);
    polynomial.SetVector(1, 11, 7);
    polynomial.Done(true);
    CHECK(polynomial.Vector(0) == 9 && polynomial.Vector(0, 1) == 0);
    // These nonzero SHA constants follow the Intel instruction equations:
    // sigma0(1)=02004000, sigma1(1)=0000a000, sigma1(a000)=44000028.
    Machine msg1{0x0f, 0x38, 0xcc, 0xc1};
    msg1.SetVector(1, 1);
    msg1.Done(true);
    CHECK(msg1.Vector(0) == 0 && msg1.Vector(0, 1) == UINT64_C(0x0200400000000000));
    Machine msg2{0x0f, 0x38, 0xcd, 0xc1};
    msg2.SetVector(1, 0, 1);
    msg2.Done(true);
    CHECK(msg2.Vector(0) == 0xa000 && msg2.Vector(0, 1) == 0x44000028);
    Machine rounds{0x0f, 0x38, 0xcb, 0xca};
    rounds.SetVector(0, 1);
    rounds.Done(true);
    CHECK(rounds.Vector(1) == UINT64_C(0x0420008000000001) &&
          rounds.Vector(1, 1) == UINT64_C(0x4428048000000001));

    const auto strings = [](Machine& machine) {
        machine.SetVector(0, 0x6261);
        machine.SetVector(1, 0x616278);
    };
    Machine index{0x66, 0x0f, 0x3a, 0x63, 0xc1, 0};
    strings(index);
    index.Done(true);
    CHECK(index.state.gprs[arch::RCX] == 1 &&
          arch::flags::ToRflags(index.state.packed_flags) == 0x2c3);
    Machine mask{0x66, 0x0f, 0x3a, 0x62, 0xc1, 0x40};
    strings(mask);
    mask.Done(true);
    CHECK(mask.Vector(0) == 0xffff00 && mask.Vector(0, 1) == 0);
    Machine explicit32{0x66, 0x0f, 0x3a, 0x61, 0xc1, 0};
    strings(explicit32);
    explicit32.state.gprs[arch::RAX] = UINT64_C(0x12340000fffffffe);
    explicit32.state.gprs[arch::RDX] = 3;
    explicit32.Done(true);
    CHECK(explicit32.state.gprs[arch::RCX] == 1 &&
          arch::flags::GetSF(explicit32.state.packed_flags));
    Machine explicit64{0x66, 0x48, 0x0f, 0x3a, 0x60, 0xc1, 0};
    strings(explicit64);
    explicit64.state.gprs[arch::RAX] = u64{1} << 63;
    explicit64.state.gprs[arch::RDX] = 3;
    explicit64.Done(true);
    CHECK(explicit64.Vector(0) == 6 && !arch::flags::GetSF(explicit64.state.packed_flags) &&
          arch::flags::GetZF(explicit64.state.packed_flags));
}
}  // namespace

int main() {
    try {
        FlagsAndShifts();
        FaultAndAddress();
        ControlAndStrings();
        BmiAndAdx();
        CryptoAndStringCompare();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    std::puts("instruction coverage contracts passed");
}
