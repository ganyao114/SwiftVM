#include "save_state.h"
#include <cstring>

namespace swift::runtime::x86 {
namespace {
template <class T> void Put(std::span<std::uint8_t> out, std::size_t at, T value) {
    std::memcpy(out.data() + at, &value, sizeof value);
}

template <class T> T Get(std::span<const std::uint8_t> in, std::size_t at) {
    T value;
    std::memcpy(&value, in.data() + at, sizeof value);
    return value;
}
}  // namespace

void ExportLegacyState(const GuestState& state,
                       std::span<std::uint8_t, 512> out,
                       bool wide) noexcept {
    std::memset(out.data(), 0, out.size());
    Put(out, 0, state.x87.control);
    Put(out, 2, state.x87.status);
    out[4] = X87AbridgedTag(state.x87);
    Put(out, 6, state.x87_opcode);
    if (wide) {
        Put(out, 8, state.x87_ip);
        Put(out, 16, state.x87_dp);
    } else {
        Put(out, 8, static_cast<std::uint32_t>(state.x87_ip));
        Put(out, 16, static_cast<std::uint32_t>(state.x87_dp));
    }
    Put(out, 24, state.mxcsr);
    Put(out, 28, GuestMxcsrMask);
    const auto top = (state.x87.status >> 11) & 7;
    for (unsigned logical = 0; logical < 8; ++logical) {
        const auto& value = state.x87.registers[(top + logical) & 7];
        Put(out, 32 + logical * 16, value.significand);
        Put(out, 40 + logical * 16, value.sign_exp);
    }
    std::memcpy(out.data() + 160, state.xmm, sizeof state.xmm);
}

bool ImportLegacyState(GuestState& state,
                       std::span<const std::uint8_t, 512> in,
                       std::uint64_t components,
                       bool wide) noexcept {
    const auto mxcsr = Get<std::uint32_t>(in, 24);
    if ((components & 2) && (mxcsr & ~GuestMxcsrMask)) return false;
    if (components & 1) {
        state.x87.control = Get<std::uint16_t>(in, 0);
        state.x87.status = Get<std::uint16_t>(in, 2);
        state.x87_opcode = Get<std::uint16_t>(in, 6) & 0x7ff;
        state.x87_ip = wide ? Get<std::uint64_t>(in, 8) : Get<std::uint32_t>(in, 8);
        state.x87_dp = wide ? Get<std::uint64_t>(in, 16) : Get<std::uint32_t>(in, 16);
        const auto top = (state.x87.status >> 11) & 7;
        for (unsigned logical = 0; logical < 8; ++logical) {
            auto& value = state.x87.registers[(top + logical) & 7];
            value.significand = Get<std::uint64_t>(in, 32 + logical * 16);
            value.sign_exp = Get<std::uint16_t>(in, 40 + logical * 16);
            std::memset(value.reserved, 0, sizeof value.reserved);
        }
        X87RestoreAbridgedTags(state.x87, in[4]);
        state.x87.result = 0;
        state.x87.reserved = 0;
    }
    if (components & 2) {
        state.mxcsr = mxcsr;
        std::memcpy(state.xmm, in.data() + 160, sizeof state.xmm);
    }
    return true;
}
}  // namespace swift::runtime::x86
