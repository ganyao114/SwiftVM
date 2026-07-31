// User-mode extension instructions whose decode support is absent or incomplete
// in the vendored distorm snapshot: ADX and PKRU use raw bytes; FSGSBASE uses
// distorm's existing F3 0F AE /0../3 table entries.

#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include "runtime/frontend/x86/decoder_internal.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

#define __ assembler->

namespace {

bool EnvOn(const char* name) {
    const char* value = swift::runtime::PerfGetenv(name);
    return value != nullptr && std::strcmp(value, "0") != 0;
}

_RegisterType Gpr(u32 index, u32 width) {
    return static_cast<_RegisterType>((width == 64 ? R_RAX : R_EAX) + index);
}

_RegisterType AddressGpr(u32 index, bool address32) {
    return static_cast<_RegisterType>((address32 ? R_EAX : R_RAX) + index);
}

_RegisterType SegmentForPrefix(u8 prefix) {
    switch (prefix) {
        case 0x26:
            return R_ES;
        case 0x2E:
            return R_CS;
        case 0x36:
            return R_SS;
        case 0x3E:
            return R_DS;
        case 0x64:
            return R_FS;
        case 0x65:
            return R_GS;
        default:
            return static_cast<_RegisterType>(R_NONE);
    }
}

// WRPKRU's reserved input registers are a #GP condition. SwiftVM has no
// general-protection delivery path, so match XGETBV's established convention:
// record ILL_CODE and terminate the block. The architectural write is atomic
// with respect to this check -- a fault leaves PKRU unchanged.
u64 WritePkru(u64 context) {
    auto& ctx = *reinterpret_cast<ThreadContext64*>(context);
    if (ctx.rcx.low.dword != 0 || ctx.rdx.low.dword != 0) {
        ctx.interrupt = InterruptReason::ILL_CODE;
        return 1;
    }
    ctx.pkru = ctx.rax.low.dword;
    return 0;
}

ir::UniformEffectId WritePkruEffects() {
    static constexpr std::array ranges{
            ir::UniformEffectRange{offsetof(ThreadContext64, interrupt),
                                   sizeof(InterruptReason)},
            ir::UniformEffectRange{offsetof(ThreadContext64, pkru), sizeof(u32)},
    };
    static constexpr ir::UniformEffectSet effects{ranges.data(), ranges.size()};
    static const auto id = ir::RegisterUniformEffectSet(&effects);
    return id;
}

}  // namespace

bool X64Decoder::FsgsbaseEnabled() { return EnvOn("SVM_FSGSBASE"); }

bool X64Decoder::AdxEnabled() { return EnvOn("SVM_ADX"); }

void X64Decoder::DecodeFsgsbase(_DInst& insn, bool write, bool gs) {
    // CPUID.7.0:EBX.FSGSBASE stands in for CR4.FSGSBASE, as the XSAVE gate's
    // OSXSAVE bit stands in for CR4.OSXSAVE. With the gate off the instruction
    // is unavailable even though distorm can name it.
    if (!FsgsbaseEnabled()) {
        Interrupt(InterruptReason::ILL_CODE);
        return;
    }

    auto& operand = insn.ops[0];
    const ir::Uniform base{
            static_cast<u32>(gs ? offsetof(ThreadContext64, gs_base)
                                : offsetof(ThreadContext64, fs_base)),
            ir::ValueType::U64};
    if (write) {
        auto value = ToValue(Src(insn, operand));
        // The 32-bit form writes a zero-extended E-register. The 64-bit form
        // writes the full R-register. Both update the exact storage used by
        // SegmentBase for FS:/GS: address generation.
        value = operand.size == 64 ? value.SetType(ir::ValueType::U64)
                                   : __ ZeroExtend64(__ ZeroExtend32(value));
        __ StoreUniform(base, value);
    } else {
        Dst(insn, operand, __ LoadUniform(base));
    }
}

u32 X64Decoder::DecodeUserlandRaw(const u8* code, size_t available) {
    constexpr u32 kTruncated = std::numeric_limits<u32>::max();

    // RDPKRU / WRPKRU are exact three-byte, register-only encodings. PKU is
    // deliberately NOT advertised in CPUID.7.0:ECX[3]: this models only the
    // architectural register, not pkey_mprotect or per-access enforcement.
    // Consequently PKRU changes have no protection effect. The implementation
    // exists solely so an unconditional user gets a benign coherent register
    // instead of #UD; CPUID-respecting software will never issue the opcodes.
    if (available >= 2 && code[0] == 0x0F && code[1] == 0x01) {
        if (available < 3) {
            return kTruncated;
        }
        if (code[2] == 0xEE || code[2] == 0xEF) {
            pc += 3;
            if (code[2] == 0xEE) {
                const ir::Uniform pkru{
                        static_cast<u32>(offsetof(ThreadContext64, pkru)), ir::ValueType::U32};
                R(R_EAX, __ LoadUniform(pkru));
                R(R_EDX, __ LoadImm(ir::Imm(u32(0))));
            } else {
                auto context = __ GetUniformAddress(ir::Imm(0)).SetType(ir::ValueType::U64);
                auto faulted =
                        __ CallHostWithUniformEffects(WritePkruEffects(), &WritePkru, context)
                                .SetType(ir::ValueType::U64);
                __ SetLocation(ir::Lambda{ir::Imm{pc}});
                __ If(ir::terminal::If{__ TestNotZero(faulted),
                                       ir::terminal::ReturnToHost{},
                                       ir::terminal::LinkBlock{pc}});
            }
            return 3;
        }
    }

    // ADCX = 66 0F 38 F6 /r, ADOX = F3 0F 38 F6 /r. Legacy segment and
    // address-size prefixes are accepted before the mandatory prefix; REX,
    // when present, must be the final prefix. The snapshot has no ADCX/ADOX
    // enum at all, so construct the same _DInst operand shape that ordinary
    // ALU Src/Dst uses rather than teaching distorm a private opcode.
    size_t at = 0;
    u8 mandatory = 0;
    u8 segment_prefix = 0;
    bool address32 = false;
    for (u32 prefixes = 0; prefixes < 5 && at < available; ++prefixes) {
        const u8 byte = code[at];
        if (byte == 0x66 || byte == 0xF3) {
            if (mandatory != 0 && mandatory != byte) {
                return 0;
            }
            mandatory = byte;
            ++at;
        } else if (byte == 0x67) {
            address32 = true;
            ++at;
        } else if (SegmentForPrefix(byte) != R_NONE) {
            segment_prefix = byte;
            ++at;
        } else {
            break;
        }
    }
    u8 rex = 0;
    if (at < available && (code[at] & 0xF0) == 0x40) {
        rex = code[at++];
    }
    if (mandatory == 0) {
        return 0;
    }
    constexpr u8 kAdxOpcode[] = {0x0F, 0x38, 0xF6};
    for (u32 i = 0; i < 3; ++i) {
        if (at + i >= available) {
            return kTruncated;
        }
        if (code[at + i] != kAdxOpcode[i]) {
            return 0;
        }
    }
    at += 3;
    if (at >= available) {
        return kTruncated;
    }

    _DInst insn{};
    insn.base = R_NONE;
    insn.segment = segment_prefix ? static_cast<u8>(SegmentForPrefix(segment_prefix))
                                  : static_cast<u8>(R_DS | SEGMENT_DEFAULT);
    const u32 width = (rex & 8) != 0 ? 64 : 32;
    const u8 modrm = code[at++];
    const u32 mod = modrm >> 6;
    const u32 reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
    const u32 rm_low = modrm & 7;
    const u32 rm = rm_low | ((rex & 1) ? 8 : 0);
    insn.ops[0] = _Operand{O_REG, static_cast<u8>(Gpr(reg, width)), static_cast<u16>(width)};
    insn.ops[1].size = static_cast<u16>(width);

    const auto read_disp = [&](u32 bytes, s64* out) {
        if (at + bytes > available) {
            return false;
        }
        if (bytes == 1) {
            *out = static_cast<s8>(code[at]);
        } else {
            const u32 raw = u32(code[at]) | (u32(code[at + 1]) << 8) |
                            (u32(code[at + 2]) << 16) | (u32(code[at + 3]) << 24);
            *out = static_cast<s32>(raw);
        }
        at += bytes;
        return true;
    };
    const auto set_disp = [&](u32 bytes) {
        s64 displacement{};
        if (!read_disp(bytes, &displacement)) {
            return false;
        }
        insn.disp = static_cast<u64>(displacement);
        insn.dispSize = static_cast<u8>(bytes * 8);
        return true;
    };

    if (mod == 3) {
        insn.ops[1] =
                _Operand{O_REG, static_cast<u8>(Gpr(rm, width)), static_cast<u16>(width)};
    } else if (rm_low != 4) {
        if (mod == 0 && rm_low == 5) {
            if (address32) {
                insn.ops[1].type = O_DISP;
                insn.ops[1].index = R_NONE;
            } else {
                insn.ops[1].type = O_SMEM;
                insn.ops[1].index = R_RIP;
                insn.flags |= FLAG_RIP_RELATIVE;
            }
            if (!set_disp(4)) {
                return kTruncated;
            }
        } else {
            insn.ops[1].type = O_SMEM;
            insn.ops[1].index = static_cast<u8>(AddressGpr(rm, address32));
            if (mod == 1 && !set_disp(1)) {
                return kTruncated;
            }
            if (mod == 2 && !set_disp(4)) {
                return kTruncated;
            }
        }
    } else {
        if (at >= available) {
            return kTruncated;
        }
        const u8 sib = code[at++];
        const u32 scale_bits = sib >> 6;
        const u32 index_low = (sib >> 3) & 7;
        const u32 base_low = sib & 7;
        const u32 index = index_low | ((rex & 2) ? 8 : 0);
        const u32 base = base_low | ((rex & 1) ? 8 : 0);
        insn.ops[1].type = O_MEM;
        insn.ops[1].index =
                (index_low == 4 && (rex & 2) == 0)
                ? R_NONE
                : static_cast<u8>(AddressGpr(index, address32));
        insn.scale = scale_bits == 0 ? 0 : static_cast<u8>(1u << scale_bits);
        if (mod == 0 && base_low == 5) {
            insn.base = R_NONE;
            if (!set_disp(4)) {
                return kTruncated;
            }
        } else {
            insn.base = static_cast<u8>(AddressGpr(base, address32));
            if (mod == 1 && !set_disp(1)) {
                return kTruncated;
            }
            if (mod == 2 && !set_disp(4)) {
                return kTruncated;
            }
        }
    }

    if (at > 15) {
        // The opcode was recognized, but an overlength x86 instruction is #UD.
        pc += at;
        Interrupt(InterruptReason::ILL_CODE);
        return static_cast<u32>(at);
    }
    insn.size = static_cast<u8>(at);
    pc += insn.size;  // GetAddress resolves RIP-relative sources from next RIP.

    if (!AdxEnabled()) {
        Interrupt(InterruptReason::ILL_CODE);
        return insn.size;
    }

    auto left = R(static_cast<_RegisterType>(insn.ops[0].index));
    auto right = ToValue(Src(insn, insn.ops[1]));
    ir::Value result;
    if (mandatory == 0x66) {
        // ADCX consumes architectural CF. Normalize it into the host-C slot,
        // perform one existing IR Adc, and save ONLY C. OF/N/Z/PF/AF never
        // enter the save mask, so their already-materialized or lazy values
        // survive unchanged.
        auto cf = CarryValue();
        __ SetCarry(cf);
        carry_ = CarryPolarity::Direct;
        result = __ Adc(left, ir::Operand{right}).SetType(GetSize(width));
        __ SaveFlags(result, ir::Flags::Carry);
        StorePolarity(false);
    } else {
        // ADOX uses OF as an independent unsigned carry bit. Materialize OF
        // and the old architectural CF before borrowing host C for Adc. After
        // the carry-out is materialized, restore CF and write ONLY OF. This
        // deliberately converts the preserved CF representation to Direct,
        // updating carry_inverted with it; no other flag bit is rewritten.
        auto old_of = __ TestFlags(ir::Flags::Overflow).SetType(ir::ValueType::U8);
        auto old_cf = CarryValue();
        __ SetCarry(old_of);
        carry_ = CarryPolarity::Direct;
        result = __ Adc(left, ir::Operand{right}).SetType(GetSize(width));
        __ SaveFlags(result, ir::Flags::Carry);
        auto carry_out = __ TestFlags(ir::Flags::Carry).SetType(ir::ValueType::U8);
        __ SetCarry(old_cf);
        __ SetOverflow(carry_out);
        StorePolarity(false);
    }
    carry_ = CarryPolarity::Direct;
    Dst(insn, insn.ops[0], result);
    return insn.size;
}

#undef __

}  // namespace swift::x86
