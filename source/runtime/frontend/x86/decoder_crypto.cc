#include <algorithm>
#include <cstring>
#include <limits>

#include <sys/types.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__aarch64__) && defined(__linux__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

#include "runtime/frontend/x86/decoder_internal.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

bool HostCryptoExtensions() {
#if defined(__aarch64__) && defined(__APPLE__)
    int value = 0;
    size_t size = sizeof(value);
    // FEAT_AES includes the AES round instructions.  PMULL is queried
    // separately: advertising PCLMULQDQ without it would make GCM select a
    // path we cannot lower natively.
    if (sysctlbyname("hw.optional.arm.FEAT_AES", &value, &size, nullptr, 0) != 0 || value == 0) {
        return false;
    }
    value = 0;
    size = sizeof(value);
    return sysctlbyname("hw.optional.arm.FEAT_PMULL", &value, &size, nullptr, 0) == 0 && value != 0;
#else
    return false;
#endif
}

bool HostSha256Extension() {
#if defined(__aarch64__) && defined(__APPLE__)
    int value = 0;
    size_t size = sizeof(value);
    return sysctlbyname("hw.optional.arm.FEAT_SHA256", &value, &size, nullptr, 0) == 0 &&
           value != 0;
#elif defined(__aarch64__) && defined(__linux__)
    return (getauxval(AT_HWCAP) & HWCAP_SHA2) != 0;
#else
    return false;
#endif
}

}  // namespace

bool X64Decoder::CryptoNiEnabled() {
    static const bool host_has_crypto = HostCryptoExtensions();
    const char* env = swift::runtime::PerfGetenv("SVM_X86_CRYPTO_NI");
    // Default ON on a host which can execute every instruction advertised by
    // this gate.  A process-level environment is deliberately read on every
    // query, matching the project's other SVM_* feature gates; env_hash keeps
    // cached translations separated.
    return host_has_crypto && (!env || std::strcmp(env, "0") != 0);
}

bool X64Decoder::ShaNiEnabled() {
    static const bool host_has_sha256 = HostSha256Extension();
    const char* crypto_env = swift::runtime::PerfGetenv("SVM_X86_CRYPTO_NI");
    const char* sha_env = swift::runtime::PerfGetenv("SVM_X86_CRYPTO_SHA");
    // SHA-NI rides on the AES-NI/PCLMUL bundle and is enabled by default
    // (SVM_X86_CRYPTO_SHA=0 opts out).  It stayed opt-in while the latent
    // signal-frame corruption (fixed in 32e7341) made OpenSSL's alarm-driven
    // speed runs crash; keep the CPUID advertisement and decoder gate
    // identical so guests see a consistent feature set.
    return host_has_sha256 && (!crypto_env || std::strcmp(crypto_env, "0") != 0) &&
           (!sha_env || std::strcmp(sha_env, "0") != 0);
}

void X64Decoder::DecodeAes(_DInst& insn, u32 kind) {
    if (!CryptoNiEnabled() || insn.ops[0].type != O_REG ||
        (insn.ops[0].index < R_XMM0 || insn.ops[0].index > R_XMM15)) {
        Interrupt(InterruptReason::ILL_CODE);
        return;
    }
    const auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    const auto source = LoadSrcVec(insn, insn.ops[1]);
    const auto destination = XmmRead(dst);
    const bool reuse_zero =
            kind < 4 && VecLoweringEnabled("SVM_AES_ZERO_REUSE");
    ir::Value zero;
    if (reuse_zero) {
        zero = __ VecSharedZero().SetType(ir::ValueType::V128);
    }
    ir::Value result;
    switch (kind) {
        case 0:
            result = reuse_zero ? __ VecAesEncFast(destination, source, zero)
                                : __ VecAesEnc(destination, source);
            break;
        case 1:
            result = reuse_zero ? __ VecAesEncLastFast(destination, source, zero)
                                : __ VecAesEncLast(destination, source);
            break;
        case 2:
            result = reuse_zero ? __ VecAesDecFast(destination, source, zero)
                                : __ VecAesDec(destination, source);
            break;
        case 3:
            result = reuse_zero ? __ VecAesDecLastFast(destination, source, zero)
                                : __ VecAesDecLast(destination, source);
            break;
        case 4:
            // AESKEYGENASSIST is register/m128 source plus imm8.  distorm
            // supplies it as the same two vector operands as the round ops.
            result = __ VecAesKeygenAssist(source, ir::Imm(insn.imm.byte));
            break;
        default: PANIC("invalid AES-NI kind");
    }
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodePclmul(_DInst& insn) {
    if (!CryptoNiEnabled() || insn.ops[0].type != O_REG ||
        (insn.ops[0].index < R_XMM0 || insn.ops[0].index > R_XMM15)) {
        Interrupt(InterruptReason::ILL_CODE);
        return;
    }
    const auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    auto result = __ VecPclMul(XmmRead(dst), LoadSrcVec(insn, insn.ops[1]),
                               ir::Imm(insn.imm.byte));
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

void X64Decoder::DecodeSha(_DInst& insn, u32 kind) {
    if (!ShaNiEnabled() || insn.ops[0].type != O_REG ||
        (insn.ops[0].index < R_XMM0 || insn.ops[0].index > R_XMM15)) {
        Interrupt(InterruptReason::ILL_CODE);
        return;
    }
    const auto dst = static_cast<_RegisterType>(insn.ops[0].index);
    const auto destination = XmmRead(dst);
    const auto source = LoadSrcVec(insn, insn.ops[1]);
    ir::Value result;
    switch (kind) {
        case 0: result = __ VecSha256Msg1(destination, source); break;
        case 1: result = __ VecSha256Msg2(destination, source); break;
        case 2: result = __ VecSha256Rnds2(destination, source, XmmRead(R_XMM0)); break;
        default: PANIC("invalid SHA-NI kind");
    }
    XmmWrite(dst, result.SetType(ir::ValueType::V128));
}

u32 X64Decoder::DecodeShaRaw(const u8* code, size_t available) {
    constexpr u32 kTruncated = std::numeric_limits<u32>::max();
    constexpr size_t kFetchWindow = 0x10;
    // distorm predates SHA-NI.  Preserve its mature ModRM/SIB/RIP-relative
    // operand decoder by replacing just the unknown opcode with a same-map,
    // same-operands surrogate (PSHUFB / PALIGNR), then emit the SHA IR.
    size_t at = 0;
    bool has_66 = false;
    for (u32 prefixes = 0; prefixes < 5 && at < available; ++prefixes) {
        const u8 byte = code[at];
        if (byte == 0x66) {
            has_66 = true;
            ++at;
        } else if (byte == 0x26 || byte == 0x2E || byte == 0x36 || byte == 0x3E ||
                   byte == 0x64 || byte == 0x65 || byte == 0x67) {
            ++at;
        } else {
            break;
        }
    }
    if (at < available && (code[at] & 0xF0) == 0x40) {
        ++at;
    }
    // SHA-NI has no mandatory 66 prefix (unlike AES-NI).  Treat an explicit
    // 66 as a non-match rather than silently accepting an architecturally
    // different encoding.
    if (has_66) return 0;
    if (at + 2 >= available) return kTruncated;
    if (code[at] != 0x0F || (code[at + 1] != 0x38 && code[at + 1] != 0x3A)) return 0;

    const bool map_0f38 = code[at + 1] == 0x38;
    const u8 opcode = code[at + 2];
    u32 kind;
    if (map_0f38) {
        // SHA256MSG1 / SHA256RNDS2 / SHA256MSG2. SHA-1 C8..CA remain
        // deliberately unadvertised until their five-operation IR arrives.
        switch (opcode) {
            case 0xCC: kind = 0; break;
            case 0xCD: kind = 1; break;
            case 0xCB: kind = 2; break;
            default: return 0;
        }
    } else {
        // SHA1RNDS4 is optional for this work item; do not consume it as a
        // SHA256 op just because its map has the same legacy prefix.
        return 0;
    }
    if (available < at + 4) return kTruncated; // opcode plus ModRM

    u8 surrogate[kFetchWindow]{};
    // PSHUFB is a 66-prefixed neighbour. Insert that byte solely for distorm
    // operand decoding, then remove it from the returned architectural
    // length; pc below remains based on the original SHA byte stream.
    const size_t copied = std::min(available, sizeof(surrogate) - 1);
    surrogate[0] = 0x66;
    std::memcpy(surrogate + 1, code, copied);
    surrogate[at + 3] = 0x00; // inserted 66 + 0F 38 00 /r
    auto insn = DisDecode(surrogate, sizeof(surrogate), is_64bit);
    if (insn.size <= 1) return 0;
    --insn.size;
    if (insn.size > available) return kTruncated;
    if (insn.ops[0].type != O_REG || insn.ops[1].type == O_NONE) return 0;
    pc += insn.size; // LoadSrcVec resolves RIP-relative memory from next RIP.
    DecodeSha(insn, kind);
    return insn.size;
}

}  // namespace swift::x86
