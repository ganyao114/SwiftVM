#include "runtime/arch/x86/flags.h"
#include "runtime/arch/x86/state.h"
#include "runtime/frontend/x86/interpreter.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#if defined(__aarch64__) && !defined(__APPLE__)
#include "runtime/backend/arm64/compile.h"
#include "runtime/compile/optimize.h"
#include "runtime/execution/dispatch.h"
#include "runtime/frontend/x86/build_ir.h"
#include <asm/hwcap.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
namespace arch = swift::runtime::x86;
namespace front = swift::x86;
using u64 = std::uint64_t;

void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

u64 Call(arch::X87State& state, arch::X87Command command, unsigned index = 0, u64 raw = 0) {
    return SvmX87Execute(&state, arch::X87Encode(command, index), raw);
}

unsigned Top(const arch::X87State& state) { return (state.status >> 11) & 7; }

const arch::X87Value& St(const arch::X87State& state, unsigned index = 0) {
    return state.registers[(Top(state) + index) & 7];
}

bool SameRegisters(const arch::X87State& a, const arch::X87State& b) {
    for (unsigned i = 0; i < 8; ++i)
        if (a.registers[i].significand != b.registers[i].significand ||
            a.registers[i].sign_exp != b.registers[i].sign_exp)
            return false;
    return true;
}

struct Machine {
    static constexpr u64 pc = 0x1000, data = 0x8000;
    arch::GuestState state;
    std::vector<std::uint8_t> code;
    std::array<std::uint8_t, 256> memory{};
    u64 denied_read{UINT64_MAX}, denied_write{UINT64_MAX};
    unsigned reads{}, writes{};

    explicit Machine(std::initializer_list<std::uint8_t> bytes) : code(bytes) { state.pc = pc; }

    static bool Contains(u64 address, std::size_t bytes) {
        return address >= data && address - data <= 256 && bytes <= 256 - (address - data);
    }

    front::DirectServices Services() {
        return {this,
                [](void* opaque,
                   u64 address,
                   std::span<std::uint8_t> out) noexcept -> front::CodeReadResult {
                    const auto& m = *static_cast<Machine*>(opaque);
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
                    if (!Contains(address, out.size()) || address == m.denied_read)
                        return {front::AccessStatus::Fault, address};
                    std::memcpy(out.data(), m.memory.data() + address - data, out.size());
                    return {};
                },
                [](void* opaque, u64 address, std::span<const std::uint8_t> in, bool) noexcept
                        -> front::AccessResult {
                    auto& m = *static_cast<Machine*>(opaque);
                    ++m.writes;
                    if (!Contains(address, in.size()) || address == m.denied_write)
                        return {front::AccessStatus::Fault, address};
                    std::memcpy(m.memory.data() + address - data, in.data(), in.size());
                    return {};
                },
                nullptr,
                nullptr};
    }

    front::StepResult Step() { return front::InterpretOne(state, Services()); }

    template <class T> void Put(unsigned offset, T value) {
        std::memcpy(memory.data() + offset, &value, sizeof value);
    }

    template <class T> T Get(unsigned offset) const {
        T value;
        std::memcpy(&value, memory.data() + offset, sizeof value);
        return value;
    }
};

void ExactArithmeticAndTransactions() {
    arch::X87State state;
    Check(Call(state, arch::X87Command::PushI64, 0, UINT64_C(9007199254740993)) == 0 &&
                  St(state).significand == UINT64_C(0x8000000000000400) &&
                  St(state).sign_exp == 0x4034,
          "integer load must retain 2^53+1 in ext80");
    Call(state, arch::X87Command::Init);
    Call(state, arch::X87Command::PushI64, 0, 5);
    Call(state, arch::X87Command::PushI16, 0, 2);
    const auto command = arch::X87EncodeOperation(arch::X87Command::Binary,
                                                  1,
                                                  arch::X87Format::Register,
                                                  arch::X87Binary::Sub,
                                                  arch::X87DestIndex | arch::X87Pop);
    Check(SvmX87Execute(&state, command, 0) == 0 && Top(state) == 7 &&
                  St(state).significand == UINT64_C(0xc000000000000000) &&
                  St(state).sign_exp == 0x4000,
          "destination-index subtraction must pop only after producing exact three");
    const auto old_status = state.status;
    Check(Call(state, arch::X87Command::PrepareF32) == 0 && state.result == 0x40400000 &&
                  state.status == old_status,
          "float-store prepare cannot commit architectural status");
    Check(Call(state, arch::X87Command::CommitPrepared, 0, 1) == 0 && state.tags == 0xffff &&
                  Top(state) == 0,
          "successful float-store continuation commits one pop");

    Call(state, arch::X87Command::Init);
    Call(state, arch::X87Command::PushF64, 0, UINT64_C(0x3ff8000000000000));  // 1.5
    const auto before = state.status;
    Check(Call(state, arch::X87Command::PrepareI16) == 0 && state.result == 2 &&
                  state.status == before,
          "nearest integer-store prepare rounds 1.5 to two without committing");
    Call(state, arch::X87Command::CommitPrepared, 0, 1);
    Check((state.status & 0x0220) == 0x0220 && Top(state) == 0,
          "integer-store continuation retains actual magnitude round-up C1 and precision sticky");

    Call(state, arch::X87Command::Init);
    state.control &= ~4u;
    Call(state, arch::X87Command::PushF64);
    Call(state, arch::X87Command::PushOne);
    const auto original = state;
    Check(Call(state, arch::X87Command::Div, 1) ==
                          static_cast<u64>(arch::X87Result::RaisedException) &&
                  SameRegisters(state, original) && state.tags == original.tags &&
                  Top(state) == Top(original) && (state.status & 0x8084) == 0x8084,
          "a newly unmasked divide-by-zero sets sticky summary without committing stack data");
    Check(Call(state, arch::X87Command::Wait) == static_cast<u64>(arch::X87Result::UnmaskedPending),
          "a later wait distinguishes an older pending exception from a newly raised one");

    Call(state, arch::X87Command::Init);
    state.control = 0x077f;
    Call(state, arch::X87Command::PushConstant, 0, static_cast<u64>(arch::X87Constant::Pi));
    Check(St(state).significand == UINT64_C(0xc90fdaa22168c234) && !(state.status & 32),
          "FLDPI round-down chooses the lower ext80 neighbor without precision exception");
}

void MemoryPrecisionAndFault() {
    // FLD m80; FSTP m80. The second write is deliberately denied first.
    Machine m{0xdb, 0x28, 0xdb, 0x3f};
    m.state.gprs[arch::RAX] = Machine::data;
    m.state.gprs[arch::RDI] = Machine::data + 128;
    m.Put<u64>(0, UINT64_C(0xc000000000000001));
    m.Put<std::uint16_t>(8, 0x4000);
    Check(m.Step().kind == front::StepKind::Completed &&
                  St(m.state.x87).significand == UINT64_C(0xc000000000000001),
          "FLD m80 keeps bits below binary64 precision");
    const auto before = m.state.x87;
    const auto old_ip = m.state.x87_ip, old_dp = m.state.x87_dp;
    m.denied_write = Machine::data + 136;
    Check(m.Step().kind == front::StepKind::Fault && m.state.pc == Machine::pc + 2 &&
                  m.state.x87.status == before.status && m.state.x87.tags == before.tags &&
                  SameRegisters(m.state.x87, before) && m.state.x87_ip == old_ip &&
                  m.state.x87_dp == old_dp,
          "a high-piece m80 store fault cannot pop or commit x87 status/metadata");
    m.denied_write = UINT64_MAX;
    Check(m.Step().kind == front::StepKind::Completed &&
                  m.Get<u64>(128) == UINT64_C(0xc000000000000001) &&
                  m.Get<std::uint16_t>(136) == 0x4000 && m.state.x87.tags == 0xffff &&
                  m.state.x87_ip == Machine::pc + 2 && m.state.x87_dp == Machine::data + 128,
          "successful retried m80 store commits its actual payload, pop and metadata");

    Machine load_fault{0xdb, 0x28};
    load_fault.state.gprs[arch::RAX] = Machine::data;
    load_fault.Put<u64>(0, UINT64_C(0x8000000000000000));
    load_fault.Put<std::uint16_t>(8, 0x3fff);
    load_fault.denied_read = Machine::data + 8;
    load_fault.state.x87_ip = 0x777;
    const auto original = load_fault.state.x87;
    Check(load_fault.Step().kind == front::StepKind::Fault && load_fault.state.x87_ip == 0x777 &&
                  load_fault.state.x87.tags == original.tags &&
                  SameRegisters(load_fault.state.x87, original),
          "both m80 load pieces precede any architectural stack commit");

    // FLD m32; FISTTP m16. Signed truncation is independent of guest RC.
    Machine narrow{0xd9, 0x00, 0xdf, 0x0f};
    narrow.state.gprs[arch::RAX] = Machine::data;
    narrow.state.gprs[arch::RDI] = Machine::data + 32;
    narrow.state.x87.control = 0x0b7f;
    narrow.Put<std::uint32_t>(0, 0xc0600000);  // -3.5
    Check(narrow.Step().kind == front::StepKind::Completed &&
                  narrow.Step().kind == front::StepKind::Completed &&
                  narrow.Get<std::uint16_t>(32) == 0xfffd && narrow.state.x87.tags == 0xffff,
          "FISTTP m16 truncates negative binary32 and pops only after its store");
}

void EnvironmentAndCompare() {
    Machine m{0xdd, 0x30, 0xdd, 0x20};  // FNSAVE [RAX]; FRSTOR [RAX]
    m.state.gprs[arch::RAX] = Machine::data;
    m.state.x87.control = 0x027f;
    m.state.x87.status = 0x1900;
    m.state.x87.tags = 0xfc3f;
    m.state.x87.registers[3] = {UINT64_C(0x8000000000000000), 0x3fff, {}};
    m.state.x87.registers[4] = {UINT64_C(0xc000000000000000), 0x4000, {}};
    m.state.x87_ip = 0x12345678;
    m.state.x87_dp = 0x76543210;
    m.state.x87_opcode = 0x345;
    const auto original = m.state.x87;
    Check(m.Step().kind == front::StepKind::Completed && m.Get<std::uint32_t>(0) == 0x027f &&
                  m.Get<std::uint32_t>(4) == 0x1900 && m.Get<std::uint32_t>(8) == 0xfc3f &&
                  m.Get<u64>(28) == UINT64_C(0x8000000000000000) &&
                  m.Get<std::uint16_t>(36) == 0x3fff && m.state.x87.control == 0x037f &&
                  m.state.x87.tags == 0xffff && !m.state.x87_ip,
          "FNSAVE stores the 28-byte environment and logical stack before real FNINIT");
    Check(m.Step().kind == front::StepKind::Completed && m.state.x87.control == original.control &&
                  m.state.x87.status == original.status && m.state.x87.tags == original.tags &&
                  SameRegisters(m.state.x87, original) && m.state.x87_ip == 0x12345678 &&
                  m.state.x87_dp == 0x76543210 && m.state.x87_opcode == 0x345,
          "FRSTOR restores physical payloads under the imported TOP and full tags");

    Machine compare{0xdb, 0xf1, 0xde, 0xd9};  // FCOMI ST0,ST1; FCOMPP
    compare.state.x87.tags = 0xfff0;
    compare.state.x87.registers[0] = {UINT64_C(0x8000000000000000), 0x3fff, {}};
    compare.state.x87.registers[1] = {UINT64_C(0x8000000000000000), 0x4000, {}};
    compare.state.packed_flags = arch::flags::FromRflags(0xad7);
    Check(compare.Step().kind == front::StepKind::Completed &&
                  (arch::flags::ToRflags(compare.state.packed_flags) & 0x8d5) == 1 &&
                  Top(compare.state.x87) == 0,
          "FCOMI publishes less in CF and clears OF/SF/AF without a stack pop");
    Check(compare.Step().kind == front::StepKind::Completed && compare.state.x87.tags == 0xffff &&
                  Top(compare.state.x87) == 2 && (compare.state.x87.status & 0x4500) == 0x0100,
          "FCOMPP performs its actual less comparison and two pops");

    Machine unmasked{0xd9, 0xfa, 0x9b};  // FSQRT on empty; WAIT
    unmasked.state.x87.control &= ~1u;
    unmasked.state.x87_ip = 0x777;
    unmasked.state.x87_dp = 0x888;
    const auto failure = unmasked.Step();
    Check(failure.kind == front::StepKind::Service && failure.service == 4 &&
                  unmasked.state.x87_ip == Machine::pc && unmasked.state.x87_opcode == 0x1fa &&
                  unmasked.state.x87_dp == 0x888,
          "new unmasked helper exceptions commit actual instruction metadata");
    unmasked.state.pc = Machine::pc + 2;
    const auto pending = unmasked.Step();
    Check(pending.kind == front::StepKind::Service && pending.service == 4 &&
                  unmasked.state.x87_ip == Machine::pc,
          "older pending exception on WAIT preserves the original x87 instruction pointer");
}

void QuadRange() {
    arch::X87State state;
    state.tags = 0xfffc;
    state.registers[0] = {UINT64_C(0x8000000000000000), 0x403b, {}};  // 2^60
    const auto command = arch::X87Encode(arch::X87Command::Transcendental);
    Check(SvmX87Execute(&state, command, static_cast<u64>(arch::X87Transcendental::Sin)) == 0 &&
                  St(state).significand == UINT64_C(0xd4a56d583332e140) &&
                  St(state).sign_exp == 0xbffe && !(state.status & 0x400),
          "large accepted trig argument must use real reduction, not the vendor 2^55 zero "
          "fallback");
    // Oracle: 115-digit Decimal Machin pi and independent Taylor sine;
    // sin(2^60)=-0.83064921763725465057528179558151030107616947443429...
    // rounded to nearest ext80 is bffe:d4a56d583332e140.
    state.registers[0] = {UINT64_C(0x8000000000000000), 0x403e, {}};  // 2^63
    const auto before = state;
    Check(SvmX87Execute(&state, command, static_cast<u64>(arch::X87Transcendental::SinCos)) == 0 &&
                  SameRegisters(state, before) && state.tags == before.tags &&
                  Top(state) == Top(before) && (state.status & 0x400),
          "out-of-range FSINCOS sets C2 without changing stack depth or payload");
}

void MmxAlias() {
    Machine m{0x0f, 0x6f, 0x00, 0x0f, 0x68, 0xc1, 0x0f, 0x77, 0xd9, 0xe8};
    m.state.gprs[arch::RAX] = Machine::data;
    m.Put<u64>(0, UINT64_C(0x0807060504030201));
    m.state.x87.status = 0x2800;
    m.state.x87.registers[1].significand = UINT64_C(0x1716151413121110);
    const auto flags = m.state.packed_flags;
    Check(m.Step().kind == front::StepKind::Completed &&
                  m.state.x87.registers[0].sign_exp == 0xffff && m.state.x87.tags == 0 &&
                  Top(m.state.x87) == 0,
          "MMX writes the real physical x87 payload and enters TOP0/tag-valid state");
    Check(m.Step().kind == front::StepKind::Completed &&
                  m.state.x87.registers[0].significand == UINT64_C(0x1708160715061405) &&
                  m.state.packed_flags == flags,
          "MMX high-unpack uses the upper half of 64 bits and preserves EFLAGS");
    Check(m.Step().kind == front::StepKind::Completed && m.state.x87.tags == 0xffff &&
                  m.Step().kind == front::StepKind::Completed && Top(m.state.x87) == 7 &&
                  St(m.state.x87).significand == UINT64_C(0x8000000000000000) &&
                  St(m.state.x87).sign_exp == 0x3fff,
          "EMMS empties the aliased stack so following FLD1 uses the actual x87 top");

    Machine pack{0x0f, 0x63, 0xc1};
    pack.state.x87.registers[0].significand = UINT64_C(0x0080007fff80ff7f);
    pack.state.x87.registers[1].significand = UINT64_C(0x0004000300020001);
    Check(pack.Step().kind == front::StepKind::Completed &&
                  pack.state.x87.registers[0].significand == UINT64_C(0x040302017f7f8080),
          "MMX PACKSSWB concatenates four narrowed lanes from each input");

    Machine memory_conversion{0x66, 0x0f, 0x2a, 0x00};  // CVTPI2PD xmm0,m64, no MMX operand.
    memory_conversion.state.gprs[arch::RAX] = Machine::data;
    memory_conversion.Put<u64>(0, UINT64_C(0xfffffffe00000001));
    memory_conversion.state.x87.control &= ~1u;
    memory_conversion.state.x87.status = 0x9881;
    const auto old = memory_conversion.state.x87;
    Check(memory_conversion.Step().kind == front::StepKind::Completed &&
                  memory_conversion.state.x87.status == old.status &&
                  memory_conversion.state.x87.tags == old.tags,
          "CVTPI2PD m64 neither enters MMX state nor observes a pending x87 exception");
    u64 doubles[2]{};
    std::memcpy(doubles, memory_conversion.state.xmm[0], sizeof doubles);
    Check(doubles[0] == UINT64_C(0x3ff0000000000000) && doubles[1] == UINT64_C(0xc000000000000000),
          "CVTPI2PD memory source converts the two actual signed dwords");

    Machine masked{0x0f, 0xf7, 0xc1};  // MASKMOVQ mm0,mm1 with every byte masked off.
    masked.state.gprs[arch::RDI] = 0;
    masked.state.x87.status = 0x2800;
    masked.state.x87.registers[0].significand = UINT64_MAX;
    Check(masked.Step().kind == front::StepKind::Completed && masked.writes == 0 &&
                  Top(masked.state.x87) == 0 && masked.state.x87.tags == 0,
          "zero MASKMOVQ mask touches no guest pointer while still entering MMX mode");
}

void ConditionalMoves() {
    Machine m{0xda, 0xc1, 0xdb, 0xd1, 0xda, 0xd9};  // B false; NBE true; U true.
    m.state.x87.tags = 0xfff0;
    m.state.x87.status = 0x0200;
    m.state.x87.registers[0] = {UINT64_C(0x8000000000000000), 0x3fff, {}};
    m.state.x87.registers[1] = {UINT64_C(0x8000000000000000), 0x4000, {}};
    m.state.packed_flags = arch::flags::FromRflags(2);
    Check(m.Step().kind == front::StepKind::Completed && St(m.state.x87).sign_exp == 0x3fff &&
                  (m.state.x87.status & 0x0200),
          "false FCMOV preserves the old value and normal C1");
    Check(m.Step().kind == front::StepKind::Completed && St(m.state.x87).sign_exp == 0x4000 &&
                  (m.state.x87.status & 0x0200),
          "FCMOVNBE consumes explicit not(CF|ZF) without changing EFLAGS or C1");
    m.state.packed_flags = arch::flags::FromRflags(6);
    m.state.x87.registers[1] = {UINT64_C(0xc000000000000000), 0x4000, {}};
    Check(m.Step().kind == front::StepKind::Completed &&
                  St(m.state.x87).significand == UINT64_C(0xc000000000000000) &&
                  m.state.packed_flags == arch::flags::FromRflags(6),
          "FCMOVU reads guest parity and retains the full ext80 source");

    Machine empty{0xda, 0xc1};
    empty.state.x87.tags = 0xfffc;
    empty.state.x87.status = 0x0200;
    empty.state.x87.registers[0] = {UINT64_C(0x8000000000000000), 0x3fff, {}};
    empty.state.packed_flags = arch::flags::FromRflags(2);  // False still reads both stack tags.
    Check(empty.Step().kind == front::StepKind::Completed &&
                  (empty.state.x87.status & 0x0241) == 0x0041 &&
                  St(empty.state.x87).significand == UINT64_C(0xc000000000000000) &&
                  St(empty.state.x87).sign_exp == 0xffff && (empty.state.x87.tags & 15) == 14 &&
                  Top(empty.state.x87) == 0,
          "false FCMOV still detects masked source-stack underflow without populating the source "
          "or popping");
    Machine unmasked{0xda, 0xc1};
    unmasked.state.x87.tags = 0xfffc;
    unmasked.state.x87.control &= ~1u;
    unmasked.state.x87.registers[0] = {UINT64_C(0x8000000000000000), 0x3fff, {}};
    unmasked.state.packed_flags = arch::flags::FromRflags(2);
    const auto result = unmasked.Step();
    Check(result.kind == front::StepKind::Service && result.service == 4 &&
                  St(unmasked.state.x87).sign_exp == 0x3fff && unmasked.state.x87.tags == 0xfffc,
          "unmasked false FCMOV stack exception does not overwrite its destination");

    Machine save{0x9b, 0xdd, 0x30};  // FSAVE is an actual WAIT followed by FNSAVE.
    save.state.gprs[arch::RAX] = Machine::data;
    save.state.x87.control &= ~1u;
    save.state.x87.status = 1;
    Check(save.Step().kind == front::StepKind::Service && save.writes == 0 &&
                  save.state.x87.control == 0x037e,
          "FSAVE waiting prefix delivers pending x87 exception before saving or resetting state");
}

void BcdTransactions() {
    // The 18 decimal digits are fixed goldens, not a helper round trip oracle.
    Machine m{0xdf, 0x20, 0xdf, 0x37};  // FBLD [RAX]; FBSTP [RDI].
    m.state.gprs[arch::RAX] = Machine::data;
    m.state.gprs[arch::RDI] = Machine::data + 128;
    m.Put<u64>(0, UINT64_C(0x3456789012345678));
    m.Put<std::uint16_t>(8, 0x8012);
    Check(m.Step().kind == front::StepKind::Completed &&
                  St(m.state.x87).significand == UINT64_C(0xdb4da5d31879a700) &&
                  St(m.state.x87).sign_exp == 0xc037,
          "FBLD loads exact negative 123456789012345678 without a binary64 detour");
    const auto before = m.state.x87;
    const auto old_ip = m.state.x87_ip, old_dp = m.state.x87_dp;
    m.denied_write = Machine::data + 136;
    const auto fault = m.Step();
    if (fault.kind != front::StepKind::Fault)
        std::fprintf(stderr,
                     "BCD expected high-store fault: kind=%u writes=%u fsw=%04x/%04x "
                     "tags=%04x/%04x ip=%llx/%llx dp=%llx/%llx\n",
                     unsigned(fault.kind),
                     m.writes,
                     m.state.x87.status,
                     before.status,
                     m.state.x87.tags,
                     before.tags,
                     static_cast<unsigned long long>(m.state.x87_ip),
                     static_cast<unsigned long long>(old_ip),
                     static_cast<unsigned long long>(m.state.x87_dp),
                     static_cast<unsigned long long>(old_dp));
    Check(fault.kind == front::StepKind::Fault && m.state.x87.status == before.status &&
                  m.state.x87.tags == before.tags && SameRegisters(m.state.x87, before) &&
                  m.state.x87_ip == old_ip && m.state.x87_dp == old_dp,
          "BCD high-piece store fault leaves architectural FSW/TOP/tags and metadata uncommitted");
    m.denied_write = UINT64_MAX;
    Check(m.Step().kind == front::StepKind::Completed &&
                  m.Get<u64>(128) == UINT64_C(0x3456789012345678) &&
                  m.Get<std::uint16_t>(136) == 0x8012 && m.state.x87.tags == 0xffff,
          "successful BCD continuation commits exact 18 digits, sign and one pop");

    Machine rounded{0xdf, 0x30};
    rounded.state.gprs[arch::RAX] = Machine::data;
    rounded.state.x87.tags = 0xfffc;
    rounded.state.x87.status = 1;  // Old masked IE must not forge an indefinite.
    rounded.state.x87.registers[0] = {UINT64_C(0xe000000000000000), 0x4000, {}};  // +3.5 -> +4.
    Check(rounded.Step().kind == front::StepKind::Completed && rounded.Get<u64>(0) == 4 &&
                  !rounded.Get<std::uint16_t>(8) && (rounded.state.x87.status & 0x0221) == 0x0221 &&
                  rounded.state.x87.tags == 0xffff,
          "FBSTP honors CW nearest rounding, keeps old IE and commits new PE/C1 independently of "
          "its payload");
    Machine negative_zero{0xdf, 0x30};
    negative_zero.state.gprs[arch::RAX] = Machine::data;
    negative_zero.state.x87.tags = 0xfffc;
    negative_zero.state.x87.control = 0x0b7f;
    negative_zero.state.x87.registers[0] = {
            UINT64_C(0x8000000000000000), 0xbffe, {}};  // -0.5, RC up.
    Check(negative_zero.Step().kind == front::StepKind::Completed &&
                  negative_zero.Get<u64>(0) == 0 && negative_zero.Get<std::uint16_t>(8) == 0x8000 &&
                  (negative_zero.state.x87.status & 0x0220) == 0x20,
          "BCD rounding to zero retains the original negative sign and actual magnitude-rounding "
          "direction");
    Machine indefinite{0xdf, 0x30};
    indefinite.state.gprs[arch::RAX] = Machine::data;
    indefinite.state.x87.tags = 0xfffe;
    indefinite.state.x87.registers[0] = {UINT64_C(0xc000000000000000), 0x7fff, {}};
    Check(indefinite.Step().kind == front::StepKind::Completed &&
                  indefinite.Get<u64>(0) == UINT64_C(0xc000000000000000) &&
                  indefinite.Get<std::uint16_t>(8) == 0xffff && (indefinite.state.x87.status & 1) &&
                  indefinite.state.x87.tags == 0xffff,
          "masked invalid BCD store writes the SDM FFFFC000000000000000 indefinite and pops");
    Machine overflow{0xdf, 0x30};
    overflow.state.gprs[arch::RAX] = Machine::data;
    overflow.state.x87.tags = 0xfffc;
    overflow.state.x87.control &= ~1u;
    overflow.state.x87.registers[0] = {
            UINT64_C(0xde0b6b3a76400000), 0x403a, {}};  // 10^18, one digit too many.
    const auto error = overflow.Step();
    Check(error.kind == front::StepKind::Service && error.service == 4 && overflow.writes == 0 &&
                  overflow.state.x87.tags == 0xfffc && Top(overflow.state.x87) == 0 &&
                  (overflow.state.x87.status & 1),
          "unmasked over-18-digit BCD conversion cannot store or pop");
}

void NativeHelperABI() {
#if defined(__aarch64__) && !defined(__APPLE__)
    namespace a64 = swift::runtime::backend::arm64;
    namespace ir = swift::runtime::ir;
    namespace execution = swift::runtime::execution;
    const std::uint8_t bytes[]{
            0xd9, 0xfe,              // FSIN on exact 2^60 in the input stack.
            0xf2, 0x0f, 0xd6, 0xd0,  // MOVDQ2Q MM2,XMM0.
            0xf3, 0x0f, 0xd6, 0xda,  // MOVQ2DQ XMM3,MM2.
            0x0f, 0x6f, 0x20,        // MOVQ MM4,[RAX].
            0x0f, 0x68, 0xe1,        // PUNPCKHBW MM4,MM1.
            0x0f, 0x77,              // EMMS.
            0xdf, 0x60, 0x10,        // FBLD 18 exact digits from [RAX+16].
            0xda, 0xc0,              // FCMOVB ST0,ST0 consumes the real guest CF.
            0xdf, 0x70, 0x20,        // FBSTP [RAX+32] through its real 10B transaction.
            0x0f, 0x05};             // Actual root service return1.
    front::StateLayout layout{offsetof(arch::GuestState, gprs), offsetof(arch::GuestState, pc), 8};
    layout.fprs_offset = offsetof(arch::GuestState, xmm);
    layout.x87_offset = offsetof(arch::GuestState, x87);
    layout.mxcsr_offset = offsetof(arch::GuestState, mxcsr);
    layout.x87_ip_offset = offsetof(arch::GuestState, x87_ip);
    layout.x87_dp_offset = offsetof(arch::GuestState, x87_dp);
    layout.x87_opcode_offset = offsetof(arch::GuestState, x87_opcode);
    auto target = a64::KnownTargetInfo();
    target.features |= static_cast<u64>(ir::TargetFeature::PrivateGPRState);
    const front::BlockWindow window{Machine::pc, bytes};
    for (const bool pinned : {false, true}) {
        auto built = front::BuildIR({&window, 1}, ir::Scope::Block, layout, target);
        if (!built) {
            std::fprintf(stderr,
                         "native x87/MMX BuildIR: pinned=%u error=%u error_pc=0x%llx ir_error=%u "
                         "instructions=%zu decoded_bytes=%zu source_bytes=",
                         unsigned(pinned),
                         unsigned(built.error),
                         static_cast<unsigned long long>(built.error_pc),
                         unsigned(built.ir_error),
                         built.instruction_count,
                         built.decoded_bytes);
            for (const auto byte : bytes) std::fprintf(stderr, "%02x", unsigned(byte));
            std::fputc('\n', stderr);
        }
        Check(bool(built), "native x87/MMX combined input forms real neutral IR");
        auto optimized = swift::runtime::compile::Optimize(std::move(built.program));
        Check(bool(optimized), "native x87/MMX input retains the normal optimizer contract");
        a64::CompileOptions options{};
        options.flags_offset = offsetof(arch::GuestState, packed_flags);
        options.pc_offset = offsetof(arch::GuestState, pc);
        options.entry_pc = Machine::pc;
        options.fault_locations = true;
        options.mxcsr_flush_offset = offsetof(arch::GuestState, mxcsr);
        if (pinned) {
            options.dispatcher = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeDispatch);
            options.exit_handler = reinterpret_cast<std::uintptr_t>(&execution::SvmNativeExit);
            options.pinned_uniform_offset = offsetof(arch::GuestState, gprs);
            options.pinned_uniform_slots = 16;
            options.pinned_vector_offset = offsetof(arch::GuestState, xmm);
            options.private_uniforms = true;
#ifdef HWCAP_FLAGM
            options.native_nzcv = (getauxval(AT_HWCAP) & HWCAP_FLAGM) != 0;
#endif
        }
        const auto compiled = a64::Compile(optimized.program, options);
        if (!compiled)
            std::fprintf(
                    stderr,
                    "x87 native compile pin=%u error=%u node=%u block=%u opcode=%u\n",
                    unsigned(pinned),
                    unsigned(compiled.error),
                    compiled.node.value,
                    compiled.block.value,
                    compiled.node
                            ? unsigned(optimized.program.Operations()[compiled.node.value].opcode)
                            : 0);
        Check(bool(compiled), "native x87/MMX combined input compiles in default and pinned ABI");
        const auto page = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
        const auto code_bytes = compiled.words.size() * sizeof(std::uint32_t),
                   size = (code_bytes + page - 1) / page * page;
        void* mapping =
                mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Check(mapping != MAP_FAILED, "native code RW allocation");
        std::memcpy(mapping, compiled.words.data(), code_bytes);
        if (mprotect(mapping, size, PROT_READ | PROT_EXEC)) {
            munmap(mapping, size);
            Check(false, "native code RX publication");
        }
        __builtin___clear_cache(static_cast<char*>(mapping),
                                static_cast<char*>(mapping) + code_bytes);
        arch::GuestState state;
        alignas(16) std::array<std::uint8_t, 48> memory{};
        const u64 mmx_source = UINT64_C(0x0807060504030201),
                  bcd_source = UINT64_C(0x3456789012345678);
        const std::uint16_t bcd_high = 0x0012;
        std::memcpy(memory.data(), &mmx_source, 8);
        std::memcpy(memory.data() + 16, &bcd_source, 8);
        std::memcpy(memory.data() + 24, &bcd_high, 2);
        state.gprs[arch::RAX] = reinterpret_cast<std::uintptr_t>(memory.data());
        state.gprs[arch::R12] = UINT64_C(0x123456789abcdef0);
        state.x87.tags = 0xfffc;
        state.x87.registers[0] = {UINT64_C(0x8000000000000000), 0x403b, {}};
        state.x87.registers[1].significand = UINT64_C(0x1716151413121110);
        const u64 xmm[2]{UINT64_C(0xabcdef0123456789), UINT64_C(0x76543210fedcba98)};
        std::memcpy(state.xmm[0], xmm, sizeof xmm);
        state.packed_flags = arch::flags::FromRflags(0xad7);
        state.pc = Machine::pc;
        const auto function =
                reinterpret_cast<swift::runtime::backend::NativeCode::Function>(mapping);
        u64 host_fpcr{}, host_fpsr{};
        asm volatile("mrs %0, fpcr\n mrs %1, fpsr" : "=r"(host_fpcr), "=r"(host_fpsr));
        const auto run = [&]() -> u64 {
            if (!pinned) return function(reinterpret_cast<std::uint8_t*>(&state));
            swift::runtime::backend::NativeCode code{
                    function, code_bytes, nullptr, compiled.faults};
            std::atomic<const swift::runtime::backend::NativeCode*> active{};
            execution::NativeRunContext context{};
            context.state = &state;
            context.opaque = mapping;
            context.resolve = [](void* entry, execution::NativeRunContext*) noexcept {
                return reinterpret_cast<std::uintptr_t>(entry);
            };
            context.active = &active;
            context.anchor = &code;
            context.pinned_uniform_offset = options.pinned_uniform_offset;
            context.pinned_uniform_slots = 16;
            context.pinned_vector_offset = options.pinned_vector_offset;
            context.native_nzcv = options.native_nzcv;
            context.flags_offset = options.flags_offset;
            context.pc_offset = options.pc_offset;
            return execution::SvmRunNative(&state, &context);
        };
        const auto result = run();
        u64 after_fpcr{}, after_fpsr{};
        asm volatile("mrs %0, fpcr\n mrs %1, fpsr" : "=r"(after_fpcr), "=r"(after_fpsr));
        u64 observed[2]{};
        std::memcpy(observed, state.xmm[3], sizeof observed);
        u64 bcd_output{};
        std::uint16_t bcd_output_high{};
        std::memcpy(&bcd_output, memory.data() + 32, 8);
        std::memcpy(&bcd_output_high, memory.data() + 40, 2);
        const bool correct_state =
                state.x87.registers[0].significand == UINT64_C(0xd4a56d583332e140) &&
                state.x87.registers[0].sign_exp == 0xbffe &&
                state.x87.registers[2].significand == xmm[0] &&
                state.x87.registers[2].sign_exp == 0xffff &&
                state.x87.registers[4].significand == UINT64_C(0x1708160715061405) &&
                state.x87.tags == 0xffff && observed[0] == xmm[0] && observed[1] == 0 &&
                state.gprs[arch::R12] == UINT64_C(0x123456789abcdef0) && bcd_output == bcd_source &&
                bcd_output_high == bcd_high;
        state = arch::GuestState{};
        state.pc = Machine::pc;
        state.x87.control &= ~1u;
        state.x87_ip = 0x777;
        state.x87_dp = 0x999;
        const auto failure = run();
        const bool correct_error = failure == 4 && state.x87_ip == Machine::pc &&
                                   state.x87_dp == 0x999 && state.x87_opcode == 0x1fe &&
                                   (state.x87.status & 0x8041) == 0x8041;
        Check(munmap(mapping, size) == 0, "native code release");
        Check(result == 1 && host_fpcr == after_fpcr && host_fpsr == after_fpsr,
              "general-register-only ext80/Cephes helper preserves the real host FP environment");
        Check(correct_state,
              "real native helper retains full GPR/XMM banks and commits MMX physical aliases");
        Check(correct_error,
              "native new unmasked status3 returns reason4 with actual FIP/FOP and preserved DP");
    }
#endif
}
}  // namespace

int main() {
    try {
        ExactArithmeticAndTransactions();
        MemoryPrecisionAndFault();
        EnvironmentAndCompare();
        QuadRange();
        MmxAlias();
        ConditionalMoves();
        BcdTransactions();
        NativeHelperABI();
        std::puts("x87/MMX coverage passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "x87/MMX coverage: %s\n", error.what());
        return 1;
    }
}
