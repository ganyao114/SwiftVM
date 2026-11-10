#include "state.h"

#include <algorithm>
#include <cstring>
#include "runtime/arch/x86/flags.h"

namespace swift::tests::correctness {
namespace {

std::uint32_t NumericControl(std::uint32_t control) {
    constexpr unsigned rounding[]{0, 2, 1, 3};
    return 0x1f80u | (rounding[(control >> 22) & 3] << 13) |
           ((control & (1u << 24)) ? 0x8040u : 0u);
}

}  // namespace

void Inject(const State& from, runtime::x86::GuestState& to) {
    to = {};
    std::copy_n(from.gpr.begin(), 16, to.gprs);
    to.pc = from.pc;
    to.packed_flags = runtime::x86::flags::FromRflags(from.flags);
    to.mxcsr = from.fp_control;
    to.fs_base = from.tls[0];
    to.gs_base = from.tls[1];
    for (unsigned i = 0; i != 16; ++i) {
        std::memcpy(to.xmm[i], from.vectors[i].data(), 16);
        std::memcpy(to.ymm_high[i], from.vectors[i].data() + 16, 16);
    }
    for (unsigned i = 0; i != 8; ++i) {
        std::memcpy(&to.x87.registers[i].significand, from.x87[i].data(), 8);
        std::memcpy(&to.x87.registers[i].sign_exp, from.x87[i].data() + 8, 2);
    }
    to.x87.control = from.x87_control;
    to.x87.status = from.x87_status;
    to.x87.tags = from.x87_tag;
    to.x87_opcode = from.x87_opcode;
    to.x87_ip = from.x87_ip;
    to.x87_dp = from.x87_dp;
}

State Project(const runtime::x86::GuestState& from) {
    State to;
    std::copy_n(from.gprs, 16, to.gpr.begin());
    to.pc = from.pc;
    to.flags = runtime::x86::flags::ToRflags(from.packed_flags);
    to.fp_control = from.mxcsr;
    to.tls = {from.fs_base, from.gs_base};
    for (unsigned i = 0; i != 16; ++i) {
        std::memcpy(to.vectors[i].data(), from.xmm[i], 16);
        std::memcpy(to.vectors[i].data() + 16, from.ymm_high[i], 16);
    }
    for (unsigned i = 0; i != 8; ++i) {
        std::memcpy(to.x87[i].data(), &from.x87.registers[i].significand, 8);
        std::memcpy(to.x87[i].data() + 8, &from.x87.registers[i].sign_exp, 2);
    }
    to.x87_control = from.x87.control;
    to.x87_status = from.x87.status;
    to.x87_tag = from.x87.tags;
    to.x87_opcode = from.x87_opcode;
    to.x87_ip = from.x87_ip;
    to.x87_dp = from.x87_dp;
    return to;
}

void Inject(const State& from, runtime::arm64::GuestState& to) {
    to = {};
    std::copy(from.gpr.begin(), from.gpr.end(), to.gprs);
    to.pc = from.pc;
    to.packed_flags = 1 | (from.flags & 0xf0000000);
    for (unsigned i = 0; i != 32; ++i) std::memcpy(to.q[i], from.vectors[i].data(), 16);
    to.fpcr = from.fp_control;
    to.fpsr = from.fp_status;
    to.numeric_control = NumericControl(from.fp_control);
    to.tpidr_el0 = from.tls[0];
}

State Project(const runtime::arm64::GuestState& from) {
    State to;
    std::copy_n(from.gprs, 32, to.gpr.begin());
    to.pc = from.pc;
    to.flags = from.packed_flags & 0xf0000000;
    for (unsigned i = 0; i != 32; ++i) std::memcpy(to.vectors[i].data(), from.q[i], 16);
    to.fp_control = from.fpcr;
    to.fp_status = from.fpsr;
    to.tls[0] = from.tpidr_el0;
    return to;
}

void Inject(const State& from, runtime::arm32::GuestState& to) {
    to = {};
    for (unsigned i = 0; i != 15; ++i) to.r[i] = std::uint32_t(from.gpr[i]);
    to.pc = std::uint32_t(from.pc);
    to.fpscr = from.fp_control;
    to.SetCpsr(from.flags);
    for (unsigned i = 0; i != 16; ++i) std::memcpy(to.q[i], from.vectors[i].data(), 16);
    to.numeric_control = NumericControl(from.fp_control);
    to.tpidrurw = from.tls[0];
    to.tpidruro = from.tls[1];
}

State Project(const runtime::arm32::GuestState& from) {
    State to;
    for (unsigned i = 0; i != 15; ++i) to.gpr[i] = std::uint32_t(from.r[i]);
    to.pc = std::uint32_t(from.pc);
    to.flags = from.Cpsr();
    for (unsigned i = 0; i != 16; ++i) std::memcpy(to.vectors[i].data(), from.q[i], 16);
    to.fp_control = from.fpscr;
    to.tls = {from.tpidrurw, from.tpidruro};
    return to;
}

}  // namespace swift::tests::correctness
