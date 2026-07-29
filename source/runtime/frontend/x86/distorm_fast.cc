#include "runtime/frontend/x86/distorm_fast.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "mnemonics.h"

namespace swift::x86 {
namespace {

constexpr std::uint16_t kAddr64 = Decode64Bits << 10;
constexpr std::uint16_t kOp32 = Decode32Bits << 8;
constexpr std::uint16_t kOp64 = Decode64Bits << 8;
constexpr std::uint8_t kInteger = ISC_INTEGER << 3;

struct FastStats {
    std::atomic<unsigned long long> attempts{};
    std::atomic<unsigned long long> hits{};
    std::atomic<unsigned long long> verify_matches{};
    std::atomic<unsigned long long> verify_mismatches{};
};

FastStats& Stats() {
    static FastStats stats;
    return stats;
}

void DumpStats() {
    const auto& s = Stats();
    const auto attempts = s.attempts.load(std::memory_order_relaxed);
    const auto hits = s.hits.load(std::memory_order_relaxed);
    const auto matches = s.verify_matches.load(std::memory_order_relaxed);
    const auto mismatches = s.verify_mismatches.load(std::memory_order_relaxed);
    if (attempts == 0 && matches == 0 && mismatches == 0) return;
    std::fprintf(stderr,
                 "[svm-distorm-fast] enabled=%u verify=%u attempts=%llu hits=%llu "
                 "fallbacks=%llu verify_matches=%llu verify_mismatches=%llu\n",
                 unsigned(DistormFastEnabled()),
                 unsigned(DistormFastVerifyEnabled()),
                 attempts,
                 hits,
                 attempts - hits,
                 matches,
                 mismatches);
}

void EnsureDumpRegistered() {
    static const bool registered = [] {
        std::atexit(DumpStats);
        return true;
    }();
    (void)registered;
}

std::uint32_t RegisterMask(unsigned index) {
    return index < 8 ? (1u << index) : (0x4000u << (index - 8));
}

std::uint8_t Gpr(unsigned index, unsigned bits) {
    return static_cast<std::uint8_t>((bits == 64 ? R_RAX : R_EAX) + index);
}

void SetRegister(_DInst& insn, unsigned slot, unsigned index, unsigned bits) {
    insn.ops[slot].type = O_REG;
    insn.ops[slot].index = Gpr(index, bits);
    insn.ops[slot].size = static_cast<std::uint16_t>(bits);
    insn.usedRegistersMask |= RegisterMask(index);
}

void SetImmediate(_DInst& insn, unsigned slot, unsigned bits, std::int64_t value) {
    insn.ops[slot].type = O_IMM;
    insn.ops[slot].size = static_cast<std::uint16_t>(bits);
    insn.imm.sqword = value;
}

void SetPcRelative(_DInst& insn, unsigned bits, std::int64_t value) {
    insn.ops[0].type = O_PC;
    insn.ops[0].size = static_cast<std::uint16_t>(bits);
    insn.imm.addr = static_cast<_OffsetType>(value);
}

template <typename T>
bool Read(const std::uint8_t* code, std::size_t available, std::size_t& offset, T& value) {
    if (offset + sizeof(T) > available) return false;
    std::memcpy(&value, code + offset, sizeof(T));
    offset += sizeof(T);
    return true;
}

struct Prefix {
    std::size_t opcode_offset{};
    std::uint8_t rex{};
};

bool ParsePrefix(const std::uint8_t* code, std::size_t available, Prefix& prefix) {
    if (available == 0) return false;
    if ((code[0] & 0xf0) == 0x40) {
        if (available < 2 || (code[1] & 0xf0) == 0x40) return false;
        prefix.rex = code[0];
        prefix.opcode_offset = 1;
    }
    return true;
}

struct ModRm {
    std::uint8_t reg{};
    bool rm_is_register{};
    std::uint8_t rm_reg{};
    _Operand memory{};
    std::uint8_t base{R_NONE};
    std::uint8_t scale{};
    std::uint8_t disp_size{};
    std::uint64_t disp{};
    std::uint32_t used_mask{};
    bool rip_relative{};
    std::size_t end{};
};

bool ParseModRm(const std::uint8_t* code,
                std::size_t available,
                std::size_t offset,
                std::uint8_t rex,
                unsigned memory_bits,
                ModRm& out) {
    if (offset >= available) return false;
    const std::uint8_t modrm = code[offset++];
    const unsigned mod = modrm >> 6;
    const unsigned reg_field = (modrm >> 3) & 7;
    const unsigned rm_field = modrm & 7;
    out.reg = static_cast<std::uint8_t>(reg_field | ((rex & 4) ? 8 : 0));
    if (mod == 3) {
        out.rm_is_register = true;
        out.rm_reg = static_cast<std::uint8_t>(rm_field | ((rex & 1) ? 8 : 0));
        out.end = offset;
        return true;
    }

    out.memory.size = static_cast<std::uint16_t>(memory_bits);
    bool need_disp32 = false;
    if (rm_field == 4) {
        if (offset >= available) return false;
        const std::uint8_t sib = code[offset++];
        const unsigned scale_bits = sib >> 6;
        const unsigned index_field = (sib >> 3) & 7;
        const unsigned base_field = sib & 7;
        const bool have_index = index_field != 4 || (rex & 2);
        const bool have_base = mod != 0 || base_field != 5;
        const unsigned index = index_field | ((rex & 2) ? 8 : 0);
        const unsigned base = base_field | ((rex & 1) ? 8 : 0);
        need_disp32 = mod == 0 && !have_base;
        if (have_index) {
            out.memory.type = O_MEM;
            out.memory.index = Gpr(index, 64);
            out.base = have_base ? Gpr(base, 64) : R_NONE;
            out.scale = scale_bits == 0 ? 0 : static_cast<std::uint8_t>(1u << scale_bits);
            out.used_mask |= RegisterMask(index);
            if (have_base) out.used_mask |= RegisterMask(base);
        } else if (have_base) {
            out.memory.type = O_SMEM;
            out.memory.index = Gpr(base, 64);
            out.used_mask |= RegisterMask(base);
        } else {
            out.memory.type = O_DISP;
        }
    } else if (mod == 0 && rm_field == 5) {
        out.memory.type = O_SMEM;
        out.memory.index = R_RIP;
        out.rip_relative = true;
        need_disp32 = true;
    } else {
        const unsigned base = rm_field | ((rex & 1) ? 8 : 0);
        out.memory.type = O_SMEM;
        out.memory.index = Gpr(base, 64);
        out.used_mask |= RegisterMask(base);
    }

    if (mod == 1) {
        std::int8_t disp{};
        if (!Read(code, available, offset, disp)) return false;
        out.disp = static_cast<std::uint64_t>(static_cast<std::int64_t>(disp));
        out.disp_size = 8;
    } else if (mod == 2 || need_disp32) {
        std::int32_t disp{};
        if (!Read(code, available, offset, disp)) return false;
        out.disp = static_cast<std::uint64_t>(static_cast<std::int64_t>(disp));
        out.disp_size = 32;
    }
    out.end = offset;
    return true;
}

void ApplyMemory(_DInst& insn, unsigned slot, const ModRm& modrm) {
    insn.ops[slot] = modrm.memory;
    insn.base = modrm.base;
    insn.scale = modrm.scale;
    insn.dispSize = modrm.disp_size;
    insn.disp = modrm.disp;
    insn.usedRegistersMask |= modrm.used_mask;
    if (modrm.rip_relative) insn.flags |= FLAG_RIP_RELATIVE;
}

void SetRm(_DInst& insn, unsigned slot, const ModRm& modrm, unsigned bits) {
    if (modrm.rm_is_register) {
        SetRegister(insn, slot, modrm.rm_reg, bits);
    } else {
        ApplyMemory(insn, slot, modrm);
    }
}

_DInst MakeInsn(unsigned opcode, unsigned bits, std::uint16_t extra_flags = 0) {
    _DInst insn{};
    insn.opcode = static_cast<std::uint16_t>(opcode);
    insn.flags = static_cast<std::uint16_t>(kAddr64 | (bits == 64 ? kOp64 : kOp32) | extra_flags);
    insn.segment = R_NONE;
    insn.base = R_NONE;
    insn.meta = kInteger;
    return insn;
}

void SetAluFlagMasks(_DInst& insn) {
    switch (insn.opcode) {
        case I_ADD:
        case I_SUB:
        case I_CMP:
            insn.modifiedFlagsMask = 0x8d5;
            break;
        case I_AND:
            insn.modifiedFlagsMask = 0x8c5;
            insn.undefinedFlagsMask = D_AF;
            break;
        case I_OR:
            insn.modifiedFlagsMask = 0x0c4;
            insn.testedFlagsMask = D_AF;
            break;
        case I_XOR:
        case I_TEST:
            insn.modifiedFlagsMask = 0x0c4;
            insn.undefinedFlagsMask = D_AF;
            break;
        default:
            break;
    }
}

bool DecodeMov(const std::uint8_t* code,
               std::size_t available,
               const Prefix& prefix,
               _DInst& insn) {
    const std::size_t opoff = prefix.opcode_offset;
    const std::uint8_t opcode = code[opoff];
    const unsigned bits = (prefix.rex & 8) ? 64 : 32;
    // A non-W REX can be partly or wholly redundant depending on the exact
    // ModRM/SIB fields. distorm exposes that through unusedPrefixesMask.
    // Keep the trusted 32-bit set prefix-free instead of trying to reproduce
    // that subtle prefix-accounting state.
    if (bits == 32 && prefix.rex != 0) return false;
    if (opcode == 0x89 || opcode == 0x8b) {
        ModRm modrm;
        if (!ParseModRm(code, available, opoff + 1, prefix.rex, bits, modrm)) return false;
        insn = MakeInsn(I_MOV, bits, FLAG_DST_WR);
        if (opcode == 0x89) {
            SetRm(insn, 0, modrm, bits);
            SetRegister(insn, 1, modrm.reg, bits);
        } else {
            SetRegister(insn, 0, modrm.reg, bits);
            SetRm(insn, 1, modrm, bits);
        }
        insn.size = static_cast<std::uint8_t>(modrm.end);
        return true;
    }
    if ((opcode & 0xf8) == 0xb8) {
        const unsigned reg = (opcode & 7) | ((prefix.rex & 1) ? 8 : 0);
        std::size_t end = opoff + 1;
        insn = MakeInsn(I_MOV, bits, FLAG_DST_WR);
        SetRegister(insn, 0, reg, bits);
        if (bits == 64) {
            std::uint64_t imm{};
            if (!Read(code, available, end, imm)) return false;
            SetImmediate(insn, 1, 64, static_cast<std::int64_t>(imm));
        } else {
            std::int32_t imm{};
            if (!Read(code, available, end, imm)) return false;
            insn.flags |= FLAG_IMM_SIGNED;
            SetImmediate(insn, 1, 32, imm);
        }
        insn.size = static_cast<std::uint8_t>(end);
        return true;
    }
    if (opcode == 0xc7) {
        ModRm modrm;
        if (!ParseModRm(code, available, opoff + 1, prefix.rex, bits, modrm) ||
            (modrm.reg & 7) != 0) {
            return false;
        }
        std::size_t end = modrm.end;
        std::int32_t imm{};
        if (!Read(code, available, end, imm)) return false;
        insn = MakeInsn(I_MOV, bits, FLAG_DST_WR | FLAG_IMM_SIGNED);
        SetRm(insn, 0, modrm, bits);
        SetImmediate(insn, 1, 32, imm);
        insn.size = static_cast<std::uint8_t>(end);
        return true;
    }
    return false;
}

struct AluEncoding {
    std::uint8_t rm_reg;
    std::uint8_t reg_rm;
    std::uint16_t opcode;
};

constexpr std::array<AluEncoding, 7> kAluEncodings{{
        {0x01, 0x03, I_ADD},
        {0x09, 0x0b, I_OR},
        {0x21, 0x23, I_AND},
        {0x29, 0x2b, I_SUB},
        {0x31, 0x33, I_XOR},
        {0x39, 0x3b, I_CMP},
        {0x85, 0x85, I_TEST},
}};

bool DecodeAlu(const std::uint8_t* code,
               std::size_t available,
               const Prefix& prefix,
               _DInst& insn) {
    const std::size_t opoff = prefix.opcode_offset;
    const std::uint8_t raw_opcode = code[opoff];
    const unsigned bits = (prefix.rex & 8) ? 64 : 32;
    if (bits == 32 && prefix.rex != 0) return false;
    for (const auto& encoding : kAluEncodings) {
        if (raw_opcode != encoding.rm_reg && raw_opcode != encoding.reg_rm) continue;
        ModRm modrm;
        if (!ParseModRm(code, available, opoff + 1, prefix.rex, bits, modrm)) return false;
        const bool writes = encoding.opcode != I_CMP && encoding.opcode != I_TEST;
        insn = MakeInsn(encoding.opcode, bits, writes ? FLAG_DST_WR : 0);
        if (raw_opcode == encoding.rm_reg || encoding.opcode == I_TEST) {
            SetRm(insn, 0, modrm, bits);
            SetRegister(insn, 1, modrm.reg, bits);
        } else {
            SetRegister(insn, 0, modrm.reg, bits);
            SetRm(insn, 1, modrm, bits);
        }
        SetAluFlagMasks(insn);
        insn.size = static_cast<std::uint8_t>(modrm.end);
        return true;
    }

    if (raw_opcode != 0x81 && raw_opcode != 0x83 && raw_opcode != 0xf7) return false;
    ModRm modrm;
    if (!ParseModRm(code, available, opoff + 1, prefix.rex, bits, modrm)) return false;
    const unsigned selector = modrm.reg & 7;
    unsigned opcode = 0;
    if (raw_opcode == 0xf7) {
        if (selector != 0) return false;
        opcode = I_TEST;
    } else {
        constexpr std::array<unsigned, 8> kGroup1{I_ADD, I_OR, 0, 0, I_AND, I_SUB, I_XOR, I_CMP};
        opcode = kGroup1[selector];
        if (opcode == 0) return false;
    }
    const bool writes = opcode != I_CMP && opcode != I_TEST;
    insn = MakeInsn(
            opcode, bits, static_cast<std::uint16_t>(FLAG_IMM_SIGNED | (writes ? FLAG_DST_WR : 0)));
    SetRm(insn, 0, modrm, bits);
    std::size_t end = modrm.end;
    if (raw_opcode == 0x83) {
        std::int8_t imm{};
        if (!Read(code, available, end, imm)) return false;
        SetImmediate(insn, 1, 8, imm);
    } else {
        std::int32_t imm{};
        if (!Read(code, available, end, imm)) return false;
        SetImmediate(insn, 1, 32, imm);
    }
    SetAluFlagMasks(insn);
    insn.size = static_cast<std::uint8_t>(end);
    return true;
}

bool DecodeLea(const std::uint8_t* code,
               std::size_t available,
               const Prefix& prefix,
               _DInst& insn) {
    const std::size_t opoff = prefix.opcode_offset;
    if (code[opoff] != 0x8d) return false;
    const unsigned bits = (prefix.rex & 8) ? 64 : 32;
    if (bits == 32 && prefix.rex != 0) return false;
    ModRm modrm;
    if (!ParseModRm(code, available, opoff + 1, prefix.rex, 0, modrm) || modrm.rm_is_register) {
        return false;
    }
    insn = MakeInsn(I_LEA, bits, FLAG_DST_WR);
    SetRegister(insn, 0, modrm.reg, bits);
    ApplyMemory(insn, 1, modrm);
    insn.size = static_cast<std::uint8_t>(modrm.end);
    return true;
}

bool DecodeMovzx(const std::uint8_t* code,
                 std::size_t available,
                 const Prefix& prefix,
                 _DInst& insn) {
    const std::size_t opoff = prefix.opcode_offset;
    if (opoff + 1 >= available || code[opoff] != 0x0f ||
        (code[opoff + 1] != 0xb6 && code[opoff + 1] != 0xb7)) {
        return false;
    }
    const unsigned src_bits = code[opoff + 1] == 0xb6 ? 8 : 16;
    const unsigned dst_bits = (prefix.rex & 8) ? 64 : 32;
    if (dst_bits == 32 && prefix.rex != 0) return false;
    ModRm modrm;
    if (!ParseModRm(code, available, opoff + 2, prefix.rex, src_bits, modrm)) return false;
    // Without REX, ModRM r/m 4..7 names AH..BH. Keep those out of the hand
    // decoder so no high-8 register can be confused with SPL..DIL.
    if (src_bits == 8 && modrm.rm_is_register && prefix.rex == 0 && (modrm.rm_reg & 7) >= 4) {
        return false;
    }
    insn = MakeInsn(I_MOVZX, dst_bits, FLAG_DST_WR);
    SetRegister(insn, 0, modrm.reg, dst_bits);
    if (modrm.rm_is_register) {
        const unsigned index = modrm.rm_reg;
        insn.ops[1].type = O_REG;
        if (src_bits == 8 && prefix.rex != 0 && index >= 4 && index < 8) {
            insn.ops[1].index = static_cast<std::uint8_t>(R_SPL + index - 4);
        } else {
            insn.ops[1].index = static_cast<std::uint8_t>((src_bits == 8 ? R_AL : R_AX) + index);
        }
        insn.ops[1].size = static_cast<std::uint16_t>(src_bits);
        insn.usedRegistersMask |= RegisterMask(index);
    } else {
        ApplyMemory(insn, 1, modrm);
    }
    insn.size = static_cast<std::uint8_t>(modrm.end);
    return true;
}

bool DecodeShift(const std::uint8_t* code,
                 std::size_t available,
                 const Prefix& prefix,
                 _DInst& insn) {
    const std::size_t opoff = prefix.opcode_offset;
    const std::uint8_t raw_opcode = code[opoff];
    if (raw_opcode != 0xd1 && raw_opcode != 0xd3 && raw_opcode != 0xc1) return false;
    const unsigned bits = (prefix.rex & 8) ? 64 : 32;
    if (bits == 32 && prefix.rex != 0) return false;
    ModRm modrm;
    if (!ParseModRm(code, available, opoff + 1, prefix.rex, bits, modrm)) return false;
    const unsigned selector = modrm.reg & 7;
    if (selector != 4 && selector != 5) return false;
    insn = MakeInsn(selector == 4 ? I_SHL : I_SHR, bits, FLAG_DST_WR);
    SetRm(insn, 0, modrm, bits);
    std::size_t end = modrm.end;
    if (raw_opcode == 0xd3) {
        insn.ops[1].type = O_REG;
        insn.ops[1].index = R_CL;
        insn.ops[1].size = 8;
        insn.usedRegistersMask |= RM_CX;
    } else {
        std::uint8_t count = 1;
        if (raw_opcode == 0xc1 && !Read(code, available, end, count)) return false;
        SetImmediate(insn, 1, 8, count);
    }
    if (raw_opcode == 0xd1) {
        insn.modifiedFlagsMask = 0x8c5;
        insn.undefinedFlagsMask = D_AF;
    } else {
        insn.modifiedFlagsMask = 0x0c5;
        insn.undefinedFlagsMask = D_AF | D_OF;
    }
    insn.size = static_cast<std::uint8_t>(end);
    return true;
}

bool DecodeControlAndStack(const std::uint8_t* code,
                           std::size_t available,
                           const Prefix& prefix,
                           _DInst& insn) {
    const std::size_t opoff = prefix.opcode_offset;
    const std::uint8_t opcode = code[opoff];
    if ((opcode & 0xf8) == 0x50 || (opcode & 0xf8) == 0x58) {
        if (prefix.rex != 0 && prefix.rex != 0x41) return false;
        const bool pop = (opcode & 0xf8) == 0x58;
        const unsigned reg = (opcode & 7) | ((prefix.rex & 1) ? 8 : 0);
        insn = MakeInsn(pop ? I_POP : I_PUSH, 64, pop ? FLAG_DST_WR : 0);
        SetRegister(insn, 0, reg, 64);
        insn.size = static_cast<std::uint8_t>(opoff + 1);
        return true;
    }
    // REX is intentionally rejected for encodings whose semantics do not use
    // it; accepting an ignored prefix would broaden the trusted prefix set.
    if (prefix.rex != 0) return false;
    if (opcode == 0x6a || opcode == 0x68) {
        insn = MakeInsn(I_PUSH, 64, FLAG_IMM_SIGNED);
        std::size_t end = opoff + 1;
        if (opcode == 0x6a) {
            std::int8_t imm{};
            if (!Read(code, available, end, imm)) return false;
            SetImmediate(insn, 0, 8, imm);
        } else {
            std::int32_t imm{};
            if (!Read(code, available, end, imm)) return false;
            SetImmediate(insn, 0, 32, imm);
        }
        insn.size = static_cast<std::uint8_t>(end);
        return true;
    }
    if (opcode == 0xc3 || opcode == 0xc2) {
        insn = MakeInsn(I_RET, 64);
        insn.meta |= FC_RET;
        std::size_t end = opoff + 1;
        if (opcode == 0xc2) {
            std::uint16_t imm{};
            if (!Read(code, available, end, imm)) return false;
            SetImmediate(insn, 0, 16, imm);
        }
        insn.size = static_cast<std::uint8_t>(end);
        return true;
    }
    if (opcode == 0x74 || opcode == 0x75 || opcode == 0x77) {
        std::int8_t rel{};
        std::size_t end = opoff + 1;
        if (!Read(code, available, end, rel)) return false;
        const unsigned decoded = opcode == 0x74 ? I_JZ : (opcode == 0x75 ? I_JNZ : I_JA);
        insn = MakeInsn(decoded, 64);
        insn.meta |= FC_CND_BRANCH;
        insn.testedFlagsMask = opcode == 0x77 ? (D_CF | D_ZF) : D_ZF;
        SetPcRelative(insn, 8, rel);
        insn.size = static_cast<std::uint8_t>(end);
        return true;
    }
    if (opcode == 0xe8 || opcode == 0xe9 || opcode == 0xeb) {
        std::size_t end = opoff + 1;
        const bool short_jump = opcode == 0xeb;
        std::int64_t rel{};
        if (short_jump) {
            std::int8_t value{};
            if (!Read(code, available, end, value)) return false;
            rel = value;
        } else {
            std::int32_t value{};
            if (!Read(code, available, end, value)) return false;
            rel = value;
        }
        insn = MakeInsn(opcode == 0xe8 ? I_CALL : I_JMP, 64);
        insn.meta |= opcode == 0xe8 ? FC_CALL : FC_UNC_BRANCH;
        SetPcRelative(insn, short_jump ? 8 : 32, rel);
        insn.size = static_cast<std::uint8_t>(end);
        return true;
    }
    return false;
}

bool OperandEqual(const _Operand& a, const _Operand& b) {
    return a.type == b.type && a.index == b.index && a.size == b.size;
}

void PrintInsn(const char* name, const _DInst& insn) {
    std::fprintf(stderr,
                 "  %s opcode=%u size=%u flags=%#x unused=%#x used=%#x "
                 "imm=%#llx disp=%#llx segment=%u base=%u scale=%u dispSize=%u "
                 "meta=%#x modified=%#x tested=%#x undefined=%#x\n",
                 name,
                 insn.opcode,
                 insn.size,
                 insn.flags,
                 insn.unusedPrefixesMask,
                 insn.usedRegistersMask,
                 static_cast<unsigned long long>(insn.imm.qword),
                 static_cast<unsigned long long>(insn.disp),
                 insn.segment,
                 insn.base,
                 insn.scale,
                 insn.dispSize,
                 insn.meta,
                 insn.modifiedFlagsMask,
                 insn.testedFlagsMask,
                 insn.undefinedFlagsMask);
    for (unsigned i = 0; i < OPERANDS_NO; ++i) {
        std::fprintf(stderr,
                     "    op%u type=%u index=%u size=%u\n",
                     i,
                     insn.ops[i].type,
                     insn.ops[i].index,
                     insn.ops[i].size);
    }
}

}  // namespace

bool DistormFastEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SVM_DISTORM_FAST");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool DistormFastVerifyEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SVM_DISTORM_VERIFY");
        return value != nullptr && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

bool DecodeDistormFast(const std::uint8_t* code,
                       std::size_t available,
                       bool is_64bit,
                       _DInst& insn) {
    if (!is_64bit || available == 0) return false;
    Prefix prefix;
    if (!ParsePrefix(code, available, prefix) || prefix.opcode_offset >= available) return false;
    return DecodeMov(code, available, prefix, insn) ||
           DecodeControlAndStack(code, available, prefix, insn) ||
           DecodeAlu(code, available, prefix, insn) || DecodeLea(code, available, prefix, insn) ||
           DecodeMovzx(code, available, prefix, insn) || DecodeShift(code, available, prefix, insn);
}

bool DistormFastEquivalent(const _DInst& a, const _DInst& b, const std::uint8_t*) {
    if (a.imm.qword != b.imm.qword || a.disp != b.disp || a.addr != b.addr || a.flags != b.flags ||
        a.unusedPrefixesMask != b.unusedPrefixesMask ||
        a.usedRegistersMask != b.usedRegistersMask || a.opcode != b.opcode || a.size != b.size ||
        a.segment != b.segment || a.base != b.base || a.scale != b.scale ||
        a.dispSize != b.dispSize || a.meta != b.meta ||
        a.modifiedFlagsMask != b.modifiedFlagsMask || a.testedFlagsMask != b.testedFlagsMask ||
        a.undefinedFlagsMask != b.undefinedFlagsMask) {
        return false;
    }
    for (unsigned i = 0; i < OPERANDS_NO; ++i) {
        if (!OperandEqual(a.ops[i], b.ops[i])) return false;
    }
    return true;
}

void DistormFastRecordAttempt(bool hit) {
    EnsureDumpRegistered();
    auto& s = Stats();
    s.attempts.fetch_add(1, std::memory_order_relaxed);
    if (hit) s.hits.fetch_add(1, std::memory_order_relaxed);
}

void DistormFastRecordVerification(bool match,
                                   const _DInst& fast,
                                   const _DInst& distorm,
                                   const std::uint8_t* code) {
    EnsureDumpRegistered();
    auto& counter = match ? Stats().verify_matches : Stats().verify_mismatches;
    counter.fetch_add(1, std::memory_order_relaxed);
    if (match) return;
    std::fprintf(stderr, "[svm-distorm-verify] MISMATCH bytes=");
    const unsigned size = distorm.size == 0 ? 1 : distorm.size;
    for (unsigned i = 0; i < size && i < 15; ++i) std::fprintf(stderr, "%02x", code[i]);
    std::fputc('\n', stderr);
    PrintInsn("fast", fast);
    PrintInsn("distorm", distorm);
}

}  // namespace swift::x86
