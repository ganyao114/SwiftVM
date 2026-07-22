//
// Created by SwiftGan on 2021/1/1.
//

#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include "runtime/frontend/x86/cpu.h"
#include "runtime/frontend/x86/decoder.h"

namespace swift::x86 {

using namespace swift::runtime::frontend;

static std::array<ABIRegUniform, 1> general_return_x86{{offsetof(ThreadContext64, rax), 4}};

static std::array<ABIRegUniform, 1> float_return_x86{{offsetof(ThreadContext64, xmm0), 16}};

static std::array<ABIRegUniform, 8> general_params_x64{
        ABIRegUniform{offsetof(ThreadContext64, rdi), 8},
        ABIRegUniform{offsetof(ThreadContext64, rsi), 8},
        ABIRegUniform{offsetof(ThreadContext64, rdx), 8},
        ABIRegUniform{offsetof(ThreadContext64, rcx), 8},
        ABIRegUniform{offsetof(ThreadContext64, r8), 8},
        ABIRegUniform{offsetof(ThreadContext64, r9), 8},
        ABIRegUniform{offsetof(ThreadContext64, rax), 8},
        ABIRegUniform{offsetof(ThreadContext64, rbx), 8}};

static std::array<ABIRegUniform, 8> float_params_x64{
        ABIRegUniform{offsetof(ThreadContext64, xmm0), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm1), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm2), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm3), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm4), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm5), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm6), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm7), 16}};

static std::array<ABIRegUniform, 2> general_return_x64{
        ABIRegUniform{offsetof(ThreadContext64, rdi), 8},
        ABIRegUniform{offsetof(ThreadContext64, rsi), 8}};

static std::array<ABIRegUniform, 2> float_return_x64{
        ABIRegUniform{offsetof(ThreadContext64, xmm0), 16},
        ABIRegUniform{offsetof(ThreadContext64, xmm1), 16}};

ABIDescriptor GetABIDescriptor32() {
    return {{}, {}, general_return_x86, float_return_x86};
}

ABIDescriptor GetABIDescriptor64() {
    return {general_params_x64, float_params_x64, general_return_x64, float_return_x64};
}

void FromHost(backend::State* state, ThreadContext64* ctx) {
    ctx->pc.qword = *state->current_loc;
    // TODO Flags
}

void ToHost(backend::State* state, ThreadContext64* ctx) {
    state->current_loc = ctx->pc.qword;
    // TODO Flags
}

// Host helpers for operations the IR cannot express directly (128-bit products,
// 128-bit dividends). Invoked through CallHost. Divide-by-zero / overflow do NOT
// raise #DE here yet (TODO), they just produce 0.
static u64 MulHiU64(u64 a, u64 b) {
    return static_cast<u64>((static_cast<unsigned __int128>(a) * b) >> 64);
}

static u64 MulHiS64(u64 a, u64 b) {
    return static_cast<u64>(
            (static_cast<__int128>(static_cast<s64>(a)) * static_cast<s64>(b)) >> 64);
}

static u64 DivQU64(u64 hi, u64 lo, u64 den) {
    if (!den) {
        return 0;
    }
    auto num = (static_cast<unsigned __int128>(hi) << 64) | lo;
    return static_cast<u64>(num / den);
}

static u64 DivRU64(u64 hi, u64 lo, u64 den) {
    if (!den) {
        return 0;
    }
    auto num = (static_cast<unsigned __int128>(hi) << 64) | lo;
    return static_cast<u64>(num % den);
}

static u64 DivQS64(u64 hi, u64 lo, u64 den) {
    auto sden = static_cast<s64>(den);
    if (!sden) {
        return 0;
    }
    auto num = static_cast<__int128>((static_cast<unsigned __int128>(hi) << 64) | lo);
    if (sden == -1 && num == (-static_cast<__int128>(1) << 127)) {
        return static_cast<u64>(static_cast<s64>(num));
    }
    return static_cast<u64>(num / sden);
}

namespace {
std::atomic<u64> g_guest_mem_bias{0};
}

void SetGuestMemBias(u64 bias) { g_guest_mem_bias.store(bias, std::memory_order_relaxed); }
u64 GetGuestMemBias() { return g_guest_mem_bias.load(std::memory_order_relaxed); }

static void RepMovs(u64 dst, u64 src, u64 bytes) {
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    std::memmove(reinterpret_cast<void*>(dst + bias), reinterpret_cast<const void*>(src + bias), bytes);
}

// rep stos fill helpers, one per element size (CallHost takes 3 args max).
// They return the end address: the call result feeds the RDI update, which
// keeps the host call alive in the JIT pipeline.
static u64 RepStos1(u64 dst, u64 value, u64 count) {
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    std::memset(reinterpret_cast<void*>(dst + bias), int(value & 0xFF), count);
    return dst + count;
}
static u64 RepStos2(u64 dst, u64 value, u64 count) {
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    auto* p = reinterpret_cast<u8*>(dst + bias);
    for (u64 i = 0; i < count; ++i) {
        std::memcpy(p + i * 2, &value, 2);
    }
    return dst + count * 2;
}
static u64 RepStos4(u64 dst, u64 value, u64 count) {
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    auto* p = reinterpret_cast<u8*>(dst + bias);
    for (u64 i = 0; i < count; ++i) {
        std::memcpy(p + i * 4, &value, 4);
    }
    return dst + count * 4;
}
static u64 RepStos8(u64 dst, u64 value, u64 count) {
    const u64 bias = g_guest_mem_bias.load(std::memory_order_relaxed);
    auto* p = reinterpret_cast<u8*>(dst + bias);
    for (u64 i = 0; i < count; ++i) {
        std::memcpy(p + i * 8, &value, 8);
    }
    return dst + count * 8;
}

static u64 DivRS64(u64 hi, u64 lo, u64 den) {
    auto sden = static_cast<s64>(den);
    if (!sden) {
        return 0;
    }
    auto num = static_cast<__int128>((static_cast<unsigned __int128>(hi) << 64) | lo);
    if (sden == -1 && num == (-static_cast<__int128>(1) << 127)) {
        return 0;
    }
    return static_cast<u64>(num % sden);
}

#define __ assembler->

static ir::ValueType GetSize(u32 bits) {
    switch (bits) {
        case 0:
            return ir::ValueType::VOID;
        case 8:
            return ir::ValueType::U8;
        case 16:
            return ir::ValueType::U16;
        case 32:
            return ir::ValueType::U32;
        case 64:
            return ir::ValueType::U64;
        default:
            PANIC();
            return ir::ValueType::VOID;
    }
}

static ir::ValueType GetSignedSize(u32 bits) {
    switch (bits) {
        case 0:
            return ir::ValueType::VOID;
        case 8:
            return ir::ValueType::S8;
        case 16:
            return ir::ValueType::S16;
        case 32:
            return ir::ValueType::S32;
        case 64:
            return ir::ValueType::S64;
        default:
            PANIC();
            return ir::ValueType::VOID;
    }
}

// 64 bit signed values must be typed U64: the backend's context.R only
// promotes U64 to X registers, an S64 value would silently get a W register
// (32 bit truncation).
static ir::ValueType GetSignedContainer(u32 bits) {
    return bits == 64 ? ir::ValueType::U64 : GetSignedSize(bits);
}

static ir::ValueType GetVecSize(u32 bits) {
    switch (bits) {
        case 0:
            return ir::ValueType::VOID;
        case 8:
            return ir::ValueType::V8;
        case 16:
            return ir::ValueType::V16;
        case 32:
            return ir::ValueType::V32;
        case 64:
            return ir::ValueType::V64;
        case 128:
            return ir::ValueType::V128;
        case 256:
            return ir::ValueType::V256;
        default:
            PANIC();
            return ir::ValueType::VOID;
    }
}

ir::Uniform ToReg(const X86RegInfo& info) {
    u32 offset{};
    if (info.index >= X86RegInfo::Rax && info.index <= X86RegInfo::R15) {
        // gprs
        offset = offsetof(ThreadContext64, regs) + (info.index - X86RegInfo::Rax) * sizeof(Reg);
    } else if (info.index >= X86RegInfo::ES && info.index <= X86RegInfo::GS) {
        // segments
        offset = offsetof(ThreadContext64, segs) + (info.index - X86RegInfo::ES) * sizeof(Seg);
    } else if (info.index == X86RegInfo::Rip) {
        // pc
        offset = offsetof(ThreadContext64, pc);
    } else {
        PANIC("Invalid GPR {}!", info.index);
    }
    return ir::Uniform{offset, info.type};
}

ir::Uniform ToVReg(const X86RegInfo& info) {
    u32 offset{};
    if (info.index >= X86RegInfo::Xmm0 && info.index <= X86RegInfo::Xmm15) {
        offset = offsetof(ThreadContext64, xmms) + (info.index - X86RegInfo::Xmm0) * sizeof(Xmm);
    } else if (info.index >= X86RegInfo::Ymm0 && info.index <= X86RegInfo::Ymm15) {
        // AVX Regs
        offset = offsetof(ThreadContext64, xmms) + (info.index - X86RegInfo::Ymm0) * sizeof(Ymm);
    } else {
        PANIC("Invalid FPR {}!", info.index);
    }
    return ir::Uniform{offset, info.type};
}

X64Decoder::X64Decoder(VAddr start,
                       runtime::MemoryInterface* memory,
                       ir::Assembler* visitor,
                       bool is_64bit)
        : start(start), pc(start), assembler(visitor), memory(memory), is_64bit(is_64bit) {
    addr_mask = is_64bit ? UINT64_MAX : UINT32_MAX;
}

void X64Decoder::Decode() {
    pc = start;
    while (!end_decode) {
        auto code_ptr = reinterpret_cast<u8*>(memory->GetPointer(reinterpret_cast<void*>(pc)));
        if (!code_ptr) {
            Interrupt(InterruptReason::PAGE_FATAL);
            break;
        }
        // CET endbr64 / endbr32 (F3 0F 1E FA/FB): distorm doesn't know them,
        // treat as NOP (real binaries start with endbr64).
        if (code_ptr[0] == 0xF3 && code_ptr[1] == 0x0F && code_ptr[2] == 0x1E &&
            (code_ptr[3] == 0xFA || code_ptr[3] == 0xFB)) {
            __ Nop();
            pc += 4;
            assembler->AdvancePC(ir::Imm{4});
            end_decode = assembler->EndCommit();
            continue;
        }
        _DInst insn = DisDecode(code_ptr, 0x10, is_64bit);
        if (insn.opcode == UINT16_MAX || insn.size == 0) {
            // size == 0 would loop forever at the same pc.
            Interrupt(InterruptReason::ILL_CODE);
            break;
        }
        pc += insn.size;
        if (!DecodeSwitch(insn)) {
            Interrupt(InterruptReason::FALLBACK);
            break;
        }
        assembler->AdvancePC(ir::Imm{insn.size});
        end_decode = assembler->EndCommit();
    }
}

bool X64Decoder::DecodeSwitch(_DInst& insn) {
    // Control / debug register moves are not modelled: trap gracefully
    // instead of panicking on the unknown register class.
    for (auto& op : insn.ops) {
        if (op.type == O_REG && ((op.index >= R_CR0 && op.index <= R_CR8) ||
                                 (op.index >= R_DR0 && op.index <= R_DR7))) {
            Interrupt(InterruptReason::ILL_CODE);
            return true;
        }
    }
    switch (insn.opcode) {
        case I_NOP:
            __ Nop();
            break;
        case I_HLT:
            Interrupt(InterruptReason::HLT);
            break;
        case I_INT_3:
            Interrupt(InterruptReason::BRK);
            break;
        case I_SYSCALL:
            Interrupt(InterruptReason::SVC);
            break;
        case I_CPUID:
            DecodeCpuid(insn);
            break;
        case I_CALL: {
            auto ret_type = is_64bit ? ir::ValueType::U64 : ir::ValueType::U32;
            Push(__ LoadImm(ir::Imm(pc)), ret_type);
            __ PushRSB(ir::Lambda(ir::Imm{pc}));
            DecodeCondJump(insn, Cond::AL);
            break;
        }
        case I_RET: {
            auto ret_addr = Pop(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32);
            // ret imm16: also drop stack args
            if (insn.ops[0].type == O_IMM) {
                auto sp = R(_RegisterType::R_RSP);
                R(_RegisterType::R_RSP,
                  __ Add(sp, ir::Operand{ir::Imm(u64(insn.imm.word))}));
            }
            __ SetLocation(ir::Lambda{ret_addr});
            __ PopRSB();
            __ Return();
            break;
        }
        case I_RETF: {
            auto ret_addr = Pop(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32);
            __ SetLocation(ir::Lambda{ret_addr});
            __ PopRSB();
            __ Return();
            break;
        }
        case I_LEAVE: {
            R(_RegisterType::R_RSP, R(_RegisterType::R_RBP));
            auto rbp = Pop(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32);
            R(is_64bit ? _RegisterType::R_RBP : _RegisterType::R_EBP, rbp);
            break;
        }
        case I_LEA:
            DecodeLea(insn);
            break;
        case I_JMP:
            DecodeCondJump(insn, Cond::AL);
            break;
        case I_JA:
            DecodeCondJump(insn, Cond::AT);
            break;
        case I_JAE:
            DecodeCondJump(insn, Cond::AE);
            break;
        case I_JB:
            DecodeCondJump(insn, Cond::BT);
            break;
        case I_JBE:
            DecodeCondJump(insn, Cond::BE);
            break;
        case I_JZ:
            DecodeCondJump(insn, Cond::EQ);
            break;
        case I_JNZ:
            DecodeCondJump(insn, Cond::NE);
            break;
        case I_JG:
            DecodeCondJump(insn, Cond::GT);
            break;
        case I_JGE:
            DecodeCondJump(insn, Cond::GE);
            break;
        case I_JL:
            DecodeCondJump(insn, Cond::LT);
            break;
        case I_JLE:
            DecodeCondJump(insn, Cond::LE);
            break;
        case I_JS:
            DecodeCondJump(insn, Cond::SN);
            break;
        case I_JNS:
            DecodeCondJump(insn, Cond::NS);
            break;
        case I_JP:
            DecodeCondJump(insn, Cond::PA);
            break;
        case I_JO:
            DecodeCondJump(insn, Cond::OF);
            break;
        case I_JNO:
            DecodeCondJump(insn, Cond::NO);
            break;
        case I_JNP:
            DecodeCondJump(insn, Cond::NP);
            break;
        case I_JCXZ:
            DecodeZeroCheckJump(insn, _RegisterType::R_CX);
            break;
        case I_JECXZ:
            DecodeZeroCheckJump(insn, _RegisterType::R_ECX);
            break;
        case I_JRCXZ:
            DecodeZeroCheckJump(insn, _RegisterType::R_RCX);
            break;
        case I_MOV:
            DecodeMov(insn);
            break;
        case I_MOVZX:
            DecodeMovzx(insn);
            break;
        case I_MOVSX:
        case I_MOVSXD:
            DecodeMovsx(insn);
            break;
        case I_MOVS:
            DecodeMovs(insn);
            break;
        case I_STOS:
            DecodeStos(insn);
            break;
        case I_CMOVA:
            DecodeCondMov(insn, Cond::AT);
            break;
        case I_CMOVAE:
            DecodeCondMov(insn, Cond::AE);
            break;
        case I_CMOVB:
            DecodeCondMov(insn, Cond::BT);
            break;
        case I_CMOVBE:
            DecodeCondMov(insn, Cond::BE);
            break;
        case I_CMOVZ:
            DecodeCondMov(insn, Cond::EQ);
            break;
        case I_CMOVG:
            DecodeCondMov(insn, Cond::GT);
            break;
        case I_CMOVGE:
            DecodeCondMov(insn, Cond::GE);
            break;
        case I_CMOVL:
            DecodeCondMov(insn, Cond::LT);
            break;
        case I_CMOVLE:
            DecodeCondMov(insn, Cond::LE);
            break;
        case I_CMOVNZ:
            DecodeCondMov(insn, Cond::NE);
            break;
        case I_CMOVNO:
            DecodeCondMov(insn, Cond::NO);
            break;
        case I_CMOVO:
            DecodeCondMov(insn, Cond::OF);
            break;
        case I_CMOVP:
            DecodeCondMov(insn, Cond::PA);
            break;
        case I_CMOVNP:
            DecodeCondMov(insn, Cond::NP);
            break;
        case I_CMOVS:
            DecodeCondMov(insn, Cond::SN);
            break;
        case I_CMOVNS:
            DecodeCondMov(insn, Cond::NS);
            break;
        case I_SETA:
            DecodeSetCC(insn, Cond::AT);
            break;
        case I_SETAE:
            DecodeSetCC(insn, Cond::AE);
            break;
        case I_SETB:
            DecodeSetCC(insn, Cond::BT);
            break;
        case I_SETBE:
            DecodeSetCC(insn, Cond::BE);
            break;
        case I_SETG:
            DecodeSetCC(insn, Cond::GT);
            break;
        case I_SETGE:
            DecodeSetCC(insn, Cond::GE);
            break;
        case I_SETL:
            DecodeSetCC(insn, Cond::LT);
            break;
        case I_SETLE:
            DecodeSetCC(insn, Cond::LE);
            break;
        case I_SETNO:
            DecodeSetCC(insn, Cond::NO);
            break;
        case I_SETNP:
            DecodeSetCC(insn, Cond::NP);
            break;
        case I_SETNS:
            DecodeSetCC(insn, Cond::NS);
            break;
        case I_SETNZ:
            DecodeSetCC(insn, Cond::NE);
            break;
        case I_SETO:
            DecodeSetCC(insn, Cond::OF);
            break;
        case I_SETP:
            DecodeSetCC(insn, Cond::PA);
            break;
        case I_SETS:
            DecodeSetCC(insn, Cond::SN);
            break;
        case I_SETZ:
            DecodeSetCC(insn, Cond::EQ);
            break;
        case I_ADD:
            DecodeAddSub(insn, false);
            break;
        case I_XADD:
            DecodeAddSub(insn, false, true, true);
            break;
        case I_SUB:
            DecodeAddSub(insn, true);
            break;
        case I_CMP:
            DecodeAddSub(insn, true, false);
            break;
        case I_ADC:
            DecodeAddSubWithCarry(insn, false);
            break;
        case I_SBB:
            DecodeAddSubWithCarry(insn, true);
            break;
        case I_INC:
            DecodeIncAndDec(insn, false);
            break;
        case I_DEC:
            DecodeIncAndDec(insn, true);
            break;
        case I_NEG:
            DecodeNeg(insn);
            break;
        case I_NOT:
            DecodeNot(insn);
            break;
        case I_XCHG:
            DecodeXchg(insn);
            break;
        case I_MUL:
            DecodeMulOneOperand(insn, false);
            break;
        case I_IMUL:
            DecodeIMul(insn);
            break;
        case I_DIV:
            DecodeDiv(insn, false);
            break;
        case I_IDIV:
            DecodeDiv(insn, true);
            break;
        case I_OR:
            DecodeOr(insn);
            break;
        case I_AND:
            DecodeAnd(insn, true);
            break;
        case I_TEST:
            DecodeAnd(insn, false);
            break;
        case I_XOR:
            DecodeXor(insn);
            break;
        case I_SHL:
        case I_SAL:
            DecodeShlShr(insn, false);
            break;
        case I_SHR:
            DecodeShlShr(insn, true);
            break;
        case I_SAR:
            DecodeSar(insn);
            break;
        case I_PUSH:
            DecodePush(insn);
            break;
        case I_POP:
            DecodePop(insn);
            break;
        case I_PUSHA:
            DecodePushA(insn);
            break;
        case I_POPA:
            DecodePopA(insn);
            break;
        case I_CBW: {
            auto al = R(_RegisterType::R_AL);
            R(_RegisterType::R_AX, __ SignExtend(al).SetType(ir::ValueType::S16));
            break;
        }
        case I_LAHF: {
            // AH = SF:ZF:0:AF:0:PF:1:CF
            auto cf = CheckCond(Cond::BT);
            auto pf = CheckCond(Cond::PA);
            auto af = __ TestFlags(ir::Flags::AuxiliaryCarry).SetType(ir::ValueType::U8);
            auto zf = CheckCond(Cond::EQ);
            auto sf = CheckCond(Cond::MI);
            auto lo = __ Or(cf, ir::Operand{__ LslImm(pf, ir::Imm(2u))});
            auto mid = __ Or(__ LslImm(af, ir::Imm(4u)), ir::Operand{__ LslImm(zf, ir::Imm(6u))});
            auto ah = __ Or(__ Or(lo, ir::Operand{mid}),
                            ir::Operand{__ Or(__ LslImm(sf, ir::Imm(7u)),
                                              ir::Operand{ir::Imm(u64(2))})});
            R(_RegisterType::R_AH, ah);
            break;
        }
        case I_CWDE: {
            auto ax = R(_RegisterType::R_AX);
            R(_RegisterType::R_EAX, __ SignExtend(ax).SetType(ir::ValueType::S32));
            break;
        }
        case I_CDQE: {
            auto eax = R(_RegisterType::R_EAX);
            R(_RegisterType::R_RAX, __ SignExtend(eax).SetType(ir::ValueType::U64));
            break;
        }
        case I_CWD: {
            auto ax = __ SignExtend(R(_RegisterType::R_AX)).SetType(ir::ValueType::S32);
            R(_RegisterType::R_DX, __ AsrImm(ax, ir::Imm(15u)));
            break;
        }
        case I_CDQ: {
            auto eax = __ SignExtend(R(_RegisterType::R_EAX)).SetType(ir::ValueType::U64);
            R(_RegisterType::R_EDX, __ AsrImm(eax, ir::Imm(31u)));
            break;
        }
        case I_CQO: {
            auto rax = R(_RegisterType::R_RAX).SetType(ir::ValueType::U64);
            R(_RegisterType::R_RDX, __ AsrImm(rax, ir::Imm(63u)));
            break;
        }
        default:
            return false;
    }
    return true;
}

ir::Value X64Decoder::R(_RegisterType reg) {
    ASSERT(reg <= _RegisterType::R_RIP);
    auto& info = x86_regs_table[reg];
    if (info.high && info.index >= X86RegInfo::Rax && info.index <= X86RegInfo::R15) {
        // AH / CH / DH / BH: bits [15:8] of the parent register.
        auto offset = ToReg(info).GetOffset();
        auto parent = __ LoadUniform(ir::Uniform{offset, ir::ValueType::U16});
        auto shifted = __ LsrImm(parent, ir::Imm(8u));
        return __ And(shifted, ir::Operand{ir::Imm(0xFFu)}).SetType(ir::ValueType::U8);
    }
    return __ LoadUniform(ToReg(info));
}

ir::Value X64Decoder::V(_RegisterType reg) {
    ASSERT(reg <= _RegisterType::R_YMM15);
    return __ LoadUniform(ToVReg(x86_regs_table[reg]));
}

ir::Value X64Decoder::NarrowTo(ir::Value value, ir::ValueType type) {
    auto want = ir::GetValueSizeByte(type);
    // Untyped (VOID) values are register-width containers: treat as 64 bit
    // (the backend cannot size a VOID value either).
    if (value.Type() == ir::ValueType::VOID) {
        value = value.SetCastType(ir::ValueType::U64);
    }
    auto have = ir::GetValueSizeByte(value.Type());
    if (want == have) {
        return value;
    }
    if (want == 8) {
        return __ ZeroExtend64(value);
    }
    // W-normalize first (safe for any input width), then a cast-type
    // adjustment for sub-32 destinations. (SetType would mutate the producing
    // instruction's own width and still leave the wrapper's cast unchanged —
    // the store width follows the wrapper.)
    value = __ ZeroExtend32(value);
    if (want < 4) {
        value = value.SetCastType(type);
    }
    return value;
}

void X64Decoder::R(_RegisterType reg, ir::Value value) {
    auto& info = x86_regs_table[reg];
    if (info.index >= X86RegInfo::Rax && info.index <= X86RegInfo::R15) {
        if (info.high) {
            // AH / CH / DH / BH: read-modify-write bits [15:8], keep the rest.
            auto offset = ToReg(info).GetOffset();
            auto parent = __ LoadUniform(ir::Uniform{offset, ir::ValueType::U64});
            auto cleared = __ And(parent, ir::Operand{ir::Imm(~u64(0xFF00))});
            auto byte = __ And(value, ir::Operand{ir::Imm(0xFFu)});
            auto inserted = __ LslImm(__ ZeroExtend64(byte), ir::Imm(8u));
            __ StoreUniform(ir::Uniform{offset, ir::ValueType::U64},
                            __ Or(cleared, ir::Operand{inserted}));
            return;
        }
        if (is_64bit && info.type == ir::ValueType::U32) {
            // x86-64: 32 bit GPR writes zero the upper 32 bits.
            auto offset = ToReg(info).GetOffset();
            auto zext = __ ZeroExtend64(__ ZeroExtend32(value));
            __ StoreUniform(ir::Uniform{offset, ir::ValueType::U64}, zext);
            return;
        }
    }
    __ StoreUniform(ToReg(info), NarrowTo(value, info.type));
}

void X64Decoder::V(_RegisterType reg, ir::Value value) {
    __ StoreUniform(ToVReg(x86_regs_table[reg]), value);
}

void X64Decoder::Interrupt(InterruptReason reason) {
    ir::Uniform uni_interrupt{offsetof(ThreadContext64, interrupt), ir::ValueType::U32};
    __ SetLocation(ir::Lambda{ir::Imm{pc}});
    __ StoreUniform(uni_interrupt, __ LoadImm(ir::Imm(static_cast<u32>(reason))));
    __ ReturnToHost();
}

ir::BOOL X64Decoder::CheckCond(Cond cond) {
    switch (cond) {
        case Cond::AL:
            return __ LoadImm(ir::Imm(true));
        case Cond::NV:
            return __ LoadImm(ir::Imm(false));
        case Cond::PA:
            return __ TestFlags(ir::Flags::Parity).SetType(ir::ValueType::U8);
        case Cond::NP:
            return __ TestNotFlags(ir::Flags::Parity).SetType(ir::ValueType::U8);
        default:
            break;
    }
    // Every other x86 condition is a pure NZCV function. Use CondSelect, which
    // reads host NZCV directly (repeated TestFlags would go through Mrs/Tst
    // pairs and Tst clobbers host NZCV, degrading every subsequent read within
    // the block). x86 conditions without an ARM equivalent are expressed as the
    // inverse condition with swapped select operands. CF involving conditions
    // honor the tracked carry polarity: after a sub-family op the stored carry
    // is the inverse of the x86 CF.
    bool direct = carry_ != CarryPolarity::Inverted;
    ir::Cond arm;
    bool inv = false;
    switch (cond) {
        case Cond::EQ: arm = ir::Cond::EQ; break;
        case Cond::NE: arm = ir::Cond::NE; break;
        case Cond::MI: case Cond::SN: arm = ir::Cond::MI; break;
        case Cond::PL: case Cond::NS: arm = ir::Cond::PL; break;
        case Cond::VS: arm = ir::Cond::VS; break;
        case Cond::VC: arm = ir::Cond::VC; break;
        case Cond::GE: arm = ir::Cond::GE; break;
        case Cond::LT: arm = ir::Cond::LT; break;
        case Cond::GT: arm = ir::Cond::GT; break;
        case Cond::LE: arm = ir::Cond::LE; break;
        // CF == 1
        case Cond::CS: case Cond::BT:
            arm = direct ? ir::Cond::CS : ir::Cond::CC;
            break;
        // CF == 0
        case Cond::CC: case Cond::AE:
            arm = direct ? ir::Cond::CC : ir::Cond::CS;
            break;
        // JA: CF == 0 && ZF == 0
        case Cond::HI: case Cond::AT:
            // Not expressible as a single ARM condition under Direct carry
            // polarity (x86 A = !CF && !ZF while the stored C equals CF), so
            // compose it from polarity-aware pieces.
            return __ And(CheckCond(Cond::CC), ir::Operand{CheckCond(Cond::NE)});
        // JBE: CF == 1 || ZF == 1
        case Cond::LS: case Cond::BE:
            return __ Or(CheckCond(Cond::CS), ir::Operand{CheckCond(Cond::EQ)});
        default: PANIC();
    }
    auto one = __ LoadImm(ir::Imm(u8(1)));
    auto zero = __ LoadImm(ir::Imm(u8(0)));
    return inv ? __ CondSelect(arm, zero, one) : __ CondSelect(arm, one, zero);
}

ir::Value X64Decoder::CarryValue() {
    auto raw = __ TestFlags(ir::Flags::Carry).SetType(ir::ValueType::U8);
    if (carry_ == CarryPolarity::Inverted) {
        return __ Xor(raw, ir::Operand{ir::Imm(u64(1))});
    }
    return raw;
}

void X64Decoder::CondGoto(ir::BOOL cond, ir::Lambda then_, ir::Location else_) {
    if (then_.IsValue()) {
        auto label = __ NotGoto(cond);
        __ SetLocation(then_);
        __ BindLabel(label);
        __ If(ir::terminal::If{
                cond, ir::terminal::ReturnToDispatch{}, ir::terminal::LinkBlock{else_}});
    } else {
        __ If(ir::terminal::If{cond,
                               ir::terminal::LinkBlock{then_.GetImm().Get()},
                               ir::terminal::LinkBlock{else_}});
    }
}

ir::Value X64Decoder::ToValue(const ir::DataClass& data) {
    return data.IsImm() ? __ LoadImm(data.imm) : data.value;
}

ir::DataClass X64Decoder::Src(_DInst& insn, _Operand& op) {
    ir::DataClass result{};
    switch (op.type) {
        case O_PC:
            result = ir::Imm(pc + insn.imm.sqword);
            break;
        case O_REG:
            if (op.index == R_RIP) {
                result = ir::Imm((pc + insn.imm.qword) & addr_mask);
            } else if (IsV(static_cast<_RegisterType>(op.index))) {
                result = V(static_cast<_RegisterType>(op.index));
            } else {
                result = R(static_cast<_RegisterType>(op.index));
            }
            break;
        case O_IMM: {
            if (insn.flags & FLAG_IMM_SIGNED) {
                // distorm already sign extended the immediate to 64 bits.
                // Type it u64: an s64 typed LoadImm would get a 32 bit
                // register in the backend and truncate.
                result = ir::Imm{static_cast<u64>(insn.imm.sqword)};
            } else if (op.size == 64) {
                result = ir::Imm{insn.imm.qword};
            } else if (op.size == 16) {
                result = ir::Imm{insn.imm.word};
            } else if (op.size == 8) {
                result = ir::Imm{insn.imm.byte};
            } else {
                result = ir::Imm{insn.imm.dword};
            }
            break;
        }
        case O_IMM1:
            result = ir::Imm{insn.imm.ex.i1};
            break;
        case O_IMM2:
            result = ir::Imm{insn.imm.ex.i2};
            break;
        case O_SMEM:
        case O_MEM:
        case O_DISP: {
            auto size = GetSize(op.size);
            auto address_operand = GetAddress(insn, op);
            // The backend's TSO loads/stores (ldar/stlr) only encode [base]:
            // any offset/index in the addressing operand would be silently
            // dropped, so the address is always folded into a single value.
            // SetType (not just cast): EmitGetOperand sizes the result from
            // the instruction's own return type.
            auto address = __ GetOperand(address_operand.ToIROperand())
                                   .SetType(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32);
            result = __ LoadMemoryTSO(ir::Operand{address}).SetType(size);
            break;
        }
        case O_PTR: {
            auto mem_segment = insn.imm.ptr.seg;
            auto seg_offset = insn.imm.ptr.off;
            auto address = (u32(mem_segment) << 4) + seg_offset;
            result = ir::Imm{address};
            break;
        }
        default:
            PANIC();
    }

    return result;
}

void X64Decoder::Dst(_DInst& insn, _Operand& operand, const ir::DataClass& data) {
    auto value = ToValue(data);
    switch (operand.type) {
        case O_REG:
            if (IsV(static_cast<_RegisterType>(operand.index))) {
                V(static_cast<_RegisterType>(operand.index), value);
            } else {
                R(static_cast<_RegisterType>(operand.index), value);
            }
            break;
        case O_DISP:
        case O_SMEM:
        case O_MEM: {
            auto address = GetAddress(insn, operand);
            if (operand.size) {
                // The store width comes from the operand, not the value (e.g.
                // sign extended immediates are wider than the destination).
                value = NarrowTo(value, GetSize(operand.size));
            }
            // See Src(): TSO stores only encode [base], fold the address.
            auto folded = __ GetOperand(address.ToIROperand())
                                  .SetType(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32);
            __ StoreMemoryTSO(ir::Operand{folded}, value);
            break;
        }
        default:
            PANIC();
    }
}

bool X64Decoder::IsV(_RegisterType reg) { return reg >= R_ST0; }

ir::DataClass X64Decoder::GetOperand(const X64Decoder::Operand& operand) {
    if (operand.OnlyLeft()) {
        return operand.Left();
    } else {
        return __ GetOperand(operand.ToIROperand());
    }
}

ir::Value X64Decoder::SegmentBase(_RegisterType segment) {
    if (segment == _RegisterType::R_FS) {
        return __ LoadUniform(ir::Uniform{offsetof(ThreadContext64, fs_base), ir::ValueType::U64});
    }
    if (segment == _RegisterType::R_GS) {
        return __ LoadUniform(ir::Uniform{offsetof(ThreadContext64, gs_base), ir::ValueType::U64});
    }
    // Other segments keep the legacy selector * 16 model (flat in 64 bit).
    return __ LslImm(R(segment), ir::Imm(4u));
}

X64Decoder::Operand X64Decoder::GetAddress(_DInst& insn, _Operand& op) {
    Operand address_operand{};
    switch (op.type) {
        case O_SMEM: {
            auto segment = SEGMENT_GET(insn.segment);
            bool is_default = SEGMENT_IS_DEFAULT(insn.segment);
            switch (insn.opcode) {
                case I_MOVS:
                    is_default = false;
                    if (&op == &insn.ops[0]) segment = R_ES;
                    break;
                case I_CMPS:
                    is_default = false;
                    if (&op == &insn.ops[1]) segment = R_ES;
                    break;
                case I_INS:
                case I_LODS:
                case I_STOS:
                case I_SCAS:
                    is_default = false;
                    break;
            }

            if (op.index == R_RIP) {
                // RIP relative: pc already points at the next instruction.
                address_operand.left = ir::Imm(pc & addr_mask);
            } else {
                address_operand.left = R(static_cast<_RegisterType>(op.index));
            }

            if (!is_default && (segment != R_NONE)) {
                // FS/GS use the 64-bit bases from the context; other segments
                // keep the legacy selector * 16 model.
                auto seg_base = SegmentBase(static_cast<_RegisterType>(segment));
                if (address_operand.left.Null()) {
                    address_operand.left = seg_base;
                } else {
                    address_operand.left =
                            __ Add(seg_base, ir::Operand{address_operand.left});
                }
            }

            if (insn.dispSize) {
                s64 disp = ForceCast<s64>(insn.disp);
                if (address_operand.right.Null() && !address_operand.ext) {
                    if (address_operand.left.IsImm()) {
                        address_operand.left = ir::Imm((address_operand.left.imm.Get() + disp) &
                                                       addr_mask);
                    } else {
                        address_operand.right = ir::Imm(disp & addr_mask);
                    }
                } else if (disp) {
                    if (address_operand.left.IsImm()) {
                        // RIP-relative base plus a segment offset: fold the
                        // displacement instead of dereferencing the imm.
                        address_operand.left =
                                ir::Imm((address_operand.left.imm.Get() + disp) & addr_mask);
                    } else {
                        ir::Imm imm{std::abs<s64>(disp) & addr_mask};
                        address_operand.left =
                                disp > 0 ? __ Add(address_operand.left.value, ir::Operand{imm})
                                         : __ Sub(address_operand.left.value, ir::Operand{imm});
                    }
                }
            }
            break;
        }
        case O_MEM: {
            if ((SEGMENT_GET(insn.segment) != R_NONE) && !SEGMENT_IS_DEFAULT(insn.segment)) {
                // FS/GS use the 64-bit bases from the context; other segments
                // keep the legacy selector * 16 model.
                address_operand.left = SegmentBase(
                        static_cast<_RegisterType>(SEGMENT_GET(insn.segment)));
            }
            if (insn.base != R_NONE) {
                if (address_operand.left.Null()) {
                    if (insn.base == R_RIP) {
                        // pc already points at the next instruction.
                        address_operand.left = ir::Imm(pc & addr_mask);
                    } else {
                        address_operand.left = R(static_cast<_RegisterType>(insn.base));
                    }
                } else {
                    // Segment override combined with a base register: fold the base
                    // in arithmetically (segment scaling above stays dropped).
                    address_operand.left =
                            __ Add(address_operand.left.value,
                                   ir::Operand{R(static_cast<_RegisterType>(insn.base))});
                }
                if (op.index != R_NONE) {
                    address_operand.right = R(static_cast<_RegisterType>(op.index));
                }
            } else if (op.index != R_NONE) {
                address_operand.left = R(static_cast<_RegisterType>(op.index));
            }
            if (insn.scale != 0) {
                if (insn.scale == 2)
                    address_operand.ext = 1;
                else if (insn.scale == 4)
                    address_operand.ext = 2;
                else if (insn.scale == 8)
                    address_operand.ext = 3;
                else {
                    PANIC("Invalid scale");
                }
                if (!address_operand.right.Null()) {
                    // base + index * scale
                    address_operand.op_type = ir::OperandOp::PlusExt;
                }
            }
            if (insn.dispSize) {
                s64 disp = ForceCast<s64>(insn.disp);
                if (address_operand.left.Null()) {
                    address_operand.left = ir::Imm(disp & addr_mask);
                } else if (address_operand.right.Null() && !address_operand.ext) {
                    if (address_operand.left.IsImm()) {
                        // Fold constant bases (e.g. RIP relative).
                        address_operand.left = ir::Imm((address_operand.left.imm.Get() + disp) &
                                                       addr_mask);
                    } else {
                        address_operand.right = ir::Imm(disp & addr_mask);
                    }
                } else if (disp) {
                    if (address_operand.left.IsImm()) {
                        // Constant (e.g. RIP-relative) base combined with an
                        // index or segment: fold the displacement.
                        address_operand.left =
                                ir::Imm((address_operand.left.imm.Get() + disp) & addr_mask);
                    } else {
                        ir::Imm imm{std::abs<s64>(disp) & addr_mask};
                        address_operand.left =
                                disp > 0 ? __ Add(address_operand.left.value, ir::Operand{imm})
                                         : __ Sub(address_operand.left.value, ir::Operand{imm});
                    }
                }
            }
            if (address_operand.left.Null()) {
                address_operand.left = ir::Imm(u64(0));
            }
            break;
        }
        case O_DISP: {
            if ((SEGMENT_GET(insn.segment) != R_NONE) && !SEGMENT_IS_DEFAULT(insn.segment)) {
                // FS/GS use the 64-bit bases from the context; other segments
                // keep the legacy selector * 16 model.
                auto seg_base = SegmentBase(
                        static_cast<_RegisterType>(SEGMENT_GET(insn.segment)));
                address_operand.left =
                        __ Add(seg_base, ir::Operand{ir::Imm(insn.disp & addr_mask)});
            } else {
                address_operand.left = ir::Imm(insn.disp & addr_mask);
            }
            break;
        }
        default:
            PANIC();
    }
    return address_operand;
}

ir::Value X64Decoder::Extend(ir::Value value, ir::ValueType type, bool sign) {
    if (sign) {
        return __ SignExtend(value).SetType(type);
    }
    switch (ir::GetValueSizeByte(type)) {
        case 1:
            return __ And(value, ir::Operand{ir::Imm(0xFFu)}).SetType(ir::ValueType::U8);
        case 2:
            return __ And(value, ir::Operand{ir::Imm(0xFFFFu)}).SetType(ir::ValueType::U16);
        case 4:
            return __ ZeroExtend32(value);
        case 8:
            return __ ZeroExtend64(value);
        default:
            PANIC();
    }
}

void X64Decoder::DecodeMov(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    auto src = Src(insn, op1);
    Dst(insn, op0, src);
}

void X64Decoder::DecodeMovzx(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    auto src = ToValue(Src(insn, op1));
    ir::Value result;
    if (op0.size == 64) {
        result = __ ZeroExtend64(src);
    } else {
        // 32 bit destinations also zero the upper half of the 64 bit register
        // (handled by the register write path); 16 bit ones store the low half.
        result = __ ZeroExtend32(src);
    }
    Dst(insn, op0, result);
}

void X64Decoder::DecodeMovsx(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    auto src = ToValue(Src(insn, op1));
    auto result = __ SignExtend(src).SetType(GetSignedContainer(op0.size));
    Dst(insn, op0, result);
}

void X64Decoder::DecodeMovs(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    if ((insn.flags & FLAG_REP) || (insn.flags & FLAG_REPNZ)) {
        // TODO: DF (direction flag) and segment overrides are not modelled,
        // assume DF == 0 and default segments. The IR MemoryCopyTSO op takes an
        // immediate count, so a dynamic RCX count goes through a host call.
        auto size = GetSize(op0.size);
        auto src_reg = is_64bit ? _RegisterType::R_RSI : _RegisterType::R_ESI;
        auto dst_reg = is_64bit ? _RegisterType::R_RDI : _RegisterType::R_EDI;
        auto cnt_reg = is_64bit ? _RegisterType::R_RCX : _RegisterType::R_ECX;
        auto src_addr = R(src_reg);
        auto dst_addr = R(dst_reg);
        auto count = __ ZeroExtend64(R(cnt_reg));
        auto bytes =
                __ Mul(count, ir::Operand{ir::Imm(u64(ir::GetValueSizeByte(size)))});
        __ CallHost(&RepMovs, dst_addr, src_addr, bytes);
        R(src_reg, __ Add(src_addr, ir::Operand{bytes}));
        R(dst_reg, __ Add(dst_addr, ir::Operand{bytes}));
        R(cnt_reg, __ LoadImm(ir::Imm(u64(0))));
    } else {
        // TODO: DF (direction flag) is not modelled, assume DF == 0.
        auto size = GetSize(op0.size);
        auto src_reg = is_64bit ? _RegisterType::R_RSI : _RegisterType::R_ESI;
        auto dst_reg = is_64bit ? _RegisterType::R_RDI : _RegisterType::R_EDI;
        auto src_addr = R(src_reg);
        auto dst_addr = R(dst_reg);
        auto value = __ LoadMemoryTSO(ir::Operand{src_addr}).SetType(size);
        __ StoreMemoryTSO(ir::Operand{dst_addr}, value);
        auto step = ir::Imm(u64(ir::GetValueSizeByte(size)));
        R(src_reg, __ Add(src_addr, ir::Operand{step}));
        R(dst_reg, __ Add(dst_addr, ir::Operand{step}));
    }
}

void X64Decoder::DecodeCpuid(_DInst& insn) {
    (void)insn;
    // Conservative feature set: SSE2 baseline only. AVX2 / AVX-512 / BMI /
    // ERMS are deliberately NOT reported so glibc's ifunc dispatch selects
    // the baseline SSE2 string routines (the EVEX implementations are out of
    // scope for this translator).
    static constexpr u32 kSse2Edx = (1u << 0)   // FPU
                                    | (1u << 15)  // CMOV
                                    | (1u << 23)  // MMX
                                    | (1u << 24)  // FXSR
                                    | (1u << 25)  // SSE
                                    | (1u << 26); // SSE2
    static constexpr u32 kExtEdx = (1u << 20)   // NX
                                   | (1u << 29);  // LM (required by 64 bit guests)
    auto leaf = __ ZeroExtend64(R(_RegisterType::R_EAX));
    auto is_leaf = [&](u32 n) {
        return __ TestZero(__ Xor(leaf, ir::Operand{ir::Imm(u64(n))}));
    };
    // Per-output-register leaf values {eax, ebx, ecx, edx}; unlisted leaves
    // and subleaves yield zeros.
    auto emit = [&](u32 for_leaf, std::array<u32, 4> vals) {
        auto cond = is_leaf(for_leaf);
        auto pick = [&](_RegisterType reg, u32 v) {
            R(reg, __ Select(cond, __ LoadImm(ir::Imm(u64(v))), R(reg))
                       .SetType(ir::ValueType::U32));
        };
        pick(_RegisterType::R_EAX, vals[0]);
        pick(_RegisterType::R_EBX, vals[1]);
        pick(_RegisterType::R_ECX, vals[2]);
        pick(_RegisterType::R_EDX, vals[3]);
    };
    // Start from zeros, then fold each supported leaf in.
    R(_RegisterType::R_EAX, __ LoadImm(ir::Imm(u64(0))));
    R(_RegisterType::R_EBX, __ LoadImm(ir::Imm(u64(0))));
    R(_RegisterType::R_ECX, __ LoadImm(ir::Imm(u64(0))));
    R(_RegisterType::R_EDX, __ LoadImm(ir::Imm(u64(0))));
    emit(0x80000000, {0x80000004, 0, 0, 0});  // max extended leaf
    emit(0x80000001, {0, 0, 0, kExtEdx});
    emit(7, {0, 0, 0, 0});                     // no BMI / AVX2 / AVX-512 / ERMS
    emit(1, {0x000306C3, 0, 0, kSse2Edx});     // Haswell-ish model, SSE2 only
    // "GenuineIntel" + max basic leaf.
    emit(0, {7, 0x756E6547, 0x6C65746E, 0x49656E69});
}

void X64Decoder::DecodeStos(_DInst& insn) {
    auto& op0 = insn.ops[0];
    const auto size = GetSize(op0.size);
    auto dst_reg = is_64bit ? _RegisterType::R_RDI : _RegisterType::R_EDI;
    auto cnt_reg = is_64bit ? _RegisterType::R_RCX : _RegisterType::R_ECX;
    auto acc = [this, size] {
        switch (ir::GetValueSizeByte(size)) {
            case 1: return R(_RegisterType::R_AL);
            case 2: return R(_RegisterType::R_AX);
            case 4: return R(_RegisterType::R_EAX);
            default: return R(_RegisterType::R_RAX);
        }
    }();
    auto dst_addr = R(dst_reg);

    if ((insn.flags & FLAG_REP) || (insn.flags & FLAG_REPNZ)) {
        // TODO: DF (direction flag) and segment overrides are not modelled,
        // assume DF == 0 and default segments.
        auto count = __ ZeroExtend64(R(cnt_reg));
        // Widen the accumulator: a narrow-typed value passed straight into a
        // host call gets a spill allocation the JIT cannot produce.
        auto acc64 = __ ZeroExtend64(acc);
        ir::Value end;
        switch (ir::GetValueSizeByte(size)) {
            case 1: end = __ CallHost(&RepStos1, dst_addr, acc64, count); break;
            case 2: end = __ CallHost(&RepStos2, dst_addr, acc64, count); break;
            case 4: end = __ CallHost(&RepStos4, dst_addr, acc64, count); break;
            default: end = __ CallHost(&RepStos8, dst_addr, acc64, count); break;
        }
        // The helper returns the fill end address, keeping the call alive.
        R(dst_reg, end);
        R(cnt_reg, __ LoadImm(ir::Imm(u64(0))));
    } else {
        // TODO: DF (direction flag) is not modelled, assume DF == 0.
        __ StoreMemoryTSO(ir::Operand{dst_addr}, acc.SetType(size));
        auto step = ir::Imm(u64(ir::GetValueSizeByte(size)));
        R(dst_reg, __ Add(dst_addr, ir::Operand{step}));
    }
}

ir::Value X64Decoder::ArithWithFlags(ir::Value left, ir::Value right, ArithOp op, u32 width,
                                     ir::Flags flag_mask) {
    const bool sub = op == ArithOp::Sub || op == ArithOp::Sbb;
    const bool use_carry = op == ArithOp::Adc || op == ArithOp::Sbb;
    // The native host adc/sbc consumes the stored carry directly, which is only
    // valid when its polarity matches: Adc wants Direct, Sbb wants Inverted.
    bool carry_native = op == ArithOp::Adc ? carry_ != CarryPolarity::Inverted
                        : op == ArithOp::Sbb ? carry_ == CarryPolarity::Inverted
                                             : true;
    bool native = width == 64 || width == 32;
    if (native) {
        if (use_carry && !carry_native) {
            // Normalize the stored host carry to the polarity the native
            // adc/sbc consumes. Materialize the x86 CF as a value, then run
            // a carry-defining op that reproduces it with the required
            // polarity, saving only C:
            //   Adc wants host C == x86 CF:      MAX + cin carries iff cin.
            //   Sbb wants host C == NOT x86 CF:  0 - cin borrows iff cin.
            auto cin = CarryValue();
            if (op == ArithOp::Adc) {
                auto norm = __ Add(__ LoadImm(ir::Imm(~u64(0))), ir::Operand{cin});
                __ SaveFlags(norm, ir::Flags::Carry);
            } else {
                auto norm = __ Sub(__ LoadImm(ir::Imm(u64(0))), ir::Operand{cin});
                __ SaveFlags(norm, ir::Flags::Carry);
            }
        }
        ir::Value result;
        switch (op) {
            case ArithOp::Add:
                result = __ Add(left, ir::Operand{right});
                break;
            case ArithOp::Adc:
                result = __ Adc(left, ir::Operand{right});
                break;
            case ArithOp::Sub:
                result = __ Sub(left, ir::Operand{right});
                break;
            case ArithOp::Sbb:
                result = __ Sbb(left, ir::Operand{right});
                break;
        }
        __ SaveFlags(result, flag_mask);
        // INC / DEC call this with Carry excluded from flag_mask: the
        // stored carry (and its polarity) must stay untouched then.
        if (True(flag_mask & ir::Flags::Carry)) {
            carry_ = sub ? CarryPolarity::Inverted : CarryPolarity::Direct;
        }
        return result;
    }
    // 8 / 16 bit: host flag computation is only exact at the host register
    // width, so the NZCV-defining op runs in a 32 bit container on operands
    // shifted left.
    const u32 container = 32;
    const u64 mask = (u64(1) << width) - 1;
    const u32 shift = container - width;
    ir::Value a_c = __ ZeroExtend32(left);
    ir::Value b_c = __ ZeroExtend32(right);
    // Carry / borrow in as a value, read before anything clobbers host NZCV.
    // x86 ADC adds CF and SBB subtracts CF (the flag value itself).
    ir::Value cin;
    if (use_carry) {
        cin = CarryValue();
    }
    // Unshifted add producing the result value plus PF/AF. For subtractions the
    // subtrahend is negated and masked, keeping the value in [0, 2^container):
    // its host N/C/V are always 0 and its Z is only set when the true Z is
    // set, so merging it into the sticky flags register can never set a wrong
    // bit.
    {
        ir::Value rhs;
        if (!sub) {
            rhs = use_carry ? __ Add(b_c, ir::Operand{cin}) : b_c;
        } else {
            auto subtrahend = use_carry ? __ Add(b_c, ir::Operand{cin}) : b_c;
            auto zero = __ LoadImm(ir::Imm(u64(0)));
            rhs = __ And(__ Sub(zero, ir::Operand{subtrahend}), ir::Operand{ir::Imm(mask)});
        }
        auto value = __ Add(a_c, ir::Operand{rhs});
        __ SaveFlags(value, flag_mask & (ir::Flags::Parity | ir::Flags::AuxiliaryCarry));
        // Exact NZCV from the shifted op: the carry-in is folded into the
        // shifted subtrahend BEFORE the shift, so N/Z/V at the container's
        // top bit always match the narrow operation (the wrap of b + cin to
        // 2^width shifts out of the result but keeps result parity with the
        // narrow computation). Known residue: C is wrong in the single edge
        // b == mask && cin == 1, where the true borrow/carry cannot be
        // represented by any single 32 bit host op (documented; would need
        // backend support to fix).
        auto sa = __ LslImm(a_c, ir::Imm(shift));
        ir::Value sb;
        if (use_carry) {
            sb = __ LslImm(__ Add(b_c, ir::Operand{cin}), ir::Imm(shift));
        } else {
            sb = __ LslImm(b_c, ir::Imm(shift));
        }
        auto flagged = sub ? __ Sub(sa, ir::Operand{sb}) : __ Add(sa, ir::Operand{sb});
        __ SaveFlags(flagged, flag_mask & ir::Flags::NZCV);
        // INC / DEC call this with Carry excluded from flag_mask: the
        // stored carry (and its polarity) must stay untouched then.
        if (True(flag_mask & ir::Flags::Carry)) {
            carry_ = sub ? CarryPolarity::Inverted : CarryPolarity::Direct;
        }
        // The store width follows the value's type (EmitStoreUniform), so the
        // result must carry the guest width type (32 -> 16/8 is W-safe).
        return value.SetCastType(GetSize(width));
    }
}

void X64Decoder::DecodeAddSub(_DInst& insn, bool sub, bool save_res, bool exchange) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = ToValue(Src(insn, op1));

    // ADD / SUB / CMP update CF, OF, ZF, SF, PF and AF.
    auto result = ArithWithFlags(left, right, sub ? ArithOp::Sub : ArithOp::Add, op0.size,
                                 ir::Flags::All);

    if (exchange) {
        Dst(insn, op1, left);
    }

    if (save_res) {
        Dst(insn, op0, result);
    }
}

void X64Decoder::DecodeCondJump(_DInst& insn, Cond cond) {
    auto& op0 = insn.ops[0];

    auto address = ir::Lambda{Src(insn, op0)};

    if (cond == Cond::AL) {
        // Direct: constant target, indirect (reg / mem): value target. Both
        // terminate the block and hand the target back to the dispatcher.
        __ SetLocation(address);
        __ ReturnToDispatcher();
    } else {
        auto check_result = CheckCond(cond);
        CondGoto(check_result, address, pc);
    }
}

void X64Decoder::DecodeZeroCheckJump(_DInst& insn, _RegisterType reg) {
    auto& op0 = insn.ops[0];
    auto value_check = R(reg);
    auto address = Src(insn, op0);

    CondGoto(__ TestZero(value_check), address, pc);
}

void X64Decoder::DecodeAddSubWithCarry(_DInst& insn, bool sub) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = ToValue(Src(insn, op1));

    auto result = ArithWithFlags(left, right, sub ? ArithOp::Sbb : ArithOp::Adc, op0.size,
                                 ir::Flags::All);

    Dst(insn, op0, result);
}

void X64Decoder::DecodeIncAndDec(_DInst& insn, bool dec) {
    auto& op0 = insn.ops[0];
    auto src = ToValue(Src(insn, op0));
    auto one = __ LoadImm(ir::Imm(u64(1), GetSize(op0.size)));

    // INC / DEC update OF, SF, ZF, AF and PF, but preserve CF. Note the backend
    // cannot preserve CF across a flag-setting add (its NZCV liveness is not
    // per-bit), so CF after INC / DEC is currently the carry of the increment
    // itself (see report).
    auto result = ArithWithFlags(src, one, dec ? ArithOp::Sub : ArithOp::Add, op0.size,
                                 ir::Flags::Overflow | ir::Flags::Negate | ir::Flags::Parity |
                                         ir::Flags::Zero | ir::Flags::AuxiliaryCarry);

    Dst(insn, op0, result);
}

void X64Decoder::DecodeNeg(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto src = ToValue(Src(insn, op0));

    // NEG updates all status flags; CF is set iff the operand was non zero,
    // which matches the borrow of 0 - src.
    auto zero = __ LoadImm(ir::Imm(u64(0), GetSize(op0.size)));
    auto result = ArithWithFlags(zero, src, ArithOp::Sub, op0.size, ir::Flags::All);

    Dst(insn, op0, result);
}

void X64Decoder::DecodeNot(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto src = ToValue(Src(insn, op0));

    // NOT does not modify any flags.
    auto result = __ Xor(src, ir::Operand{ir::Imm(UINT64_MAX)});

    Dst(insn, op0, result);
}

void X64Decoder::DecodeXchg(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = Src(insn, op0);
    auto right = Src(insn, op1);

    Dst(insn, op0, right);
    Dst(insn, op1, left);
}

void X64Decoder::DecodeSetCC(_DInst& insn, Cond cond) {
    auto check_result = CheckCond(cond);
    auto one = __ LoadImm(ir::Imm(u8(1)));
    auto zero = __ LoadImm(ir::Imm(u8(0)));
    auto result = __ Select(check_result, one, zero);
    Dst(insn, insn.ops[0], result);
}

// Compute the product of a and b at the given width and return the low half.
// If out_hi is non null the upper half is stored there. Flags: CF/OF are set
// exactly (product does not fit in `width` bits); PF comes from the low
// result. SF/ZF/AF are architecturally undefined after mul/imul and are left
// alone (the fuzz harness masks them). The CF/OF boolean is stored through a
// single flag-defining op because the backend merges NZCV wholesale per flag
// window: t = bad << 63; t + t carries and overflows exactly when bad.
static ir::Value MulWithFlags(ir::Assembler* assembler, ir::Value a, ir::Value b, u32 width,
                              bool sign, ir::Value* out_hi = nullptr) {
    ir::Value lo;
    ir::Value bad;  // nonzero iff the product does not fit in `width` bits
    if (width == 64) {
        lo = assembler->Mul(a, ir::Operand{b});
        auto hi = sign ? assembler->CallHost(&MulHiS64, a, b)
                       : assembler->CallHost(&MulHiU64, a, b);
        if (out_hi) {
            *out_hi = hi;
        }
        if (sign) {
            // Valid iff hi is the sign extension of lo's top bit.
            bad = assembler->Xor(hi, ir::Operand{assembler->AsrImm(lo, ir::Imm(63u))});
        } else {
            bad = hi;
        }
    } else {
        // Full double width product in a 64 bit container for the high half /
        // fit check (no flag side effects).
        auto aw = sign ? assembler->SignExtend(a).SetType(ir::ValueType::U64)
                       : assembler->ZeroExtend64(a);
        auto bw = sign ? assembler->SignExtend(b).SetType(ir::ValueType::U64)
                       : assembler->ZeroExtend64(b);
        auto wide = assembler->Mul(aw, ir::Operand{bw});
        if (out_hi) {
            // SetType: an untyped shift instruction would get a W register in
            // the backend, making a 32+ bit shift amount unallocated.
            *out_hi = sign ? assembler->AsrImm(wide, ir::Imm(width)).SetType(ir::ValueType::U64)
                           : assembler->LsrImm(wide, ir::Imm(width)).SetType(ir::ValueType::U64);
        }
        if (sign) {
            // Sign-extend the low `width` bits with shift pairs: a narrow-typed
            // consumer of `wide` would make the register allocator hand out a
            // W register for it and silently break the 64 bit uses.
            auto shl = assembler->LslImm(wide, ir::Imm(64 - width)).SetType(ir::ValueType::U64);
            auto sext_lo = assembler->AsrImm(shl, ir::Imm(64 - width)).SetType(ir::ValueType::U64);
            bad = assembler->Sub(wide, ir::Operand{sext_lo});
        } else {
            bad = assembler->LsrImm(wide, ir::Imm(width));
        }
        // The store path keeps the historical shape (W-register-safe types).
        if (sign && width < 32) {
            auto an = assembler->SignExtend(a).SetType(ir::ValueType::S32);
            auto bn = assembler->SignExtend(b).SetType(ir::ValueType::S32);
            lo = assembler->Mul(an, ir::Operand{bn});
            lo = lo.SetCastType(GetSize(width));
        } else {
            auto type = sign ? GetSignedContainer(width) : GetSize(width);
            lo = assembler->Mul(a.SetType(type), ir::Operand{b.SetType(type)});
        }
    }
    // PF from the low result (byte path, does not touch NZCV); AF cleared for
    // determinism (undefined per spec).
    auto flagged = assembler->Or(lo, ir::Operand{ir::Imm(u64(0))});
    assembler->ClearFlags(ir::Flags::AuxiliaryCarry);
    assembler->SaveFlags(flagged, ir::Flags::Parity);
    // Exact CF/OF via the t + t producer (see header comment).
    auto bad01 = assembler->ZeroExtend64(
            ir::Value{assembler->TestNotZero(bad.SetType(ir::ValueType::U64))});
    auto t = assembler->LslImm(bad01, ir::Imm(63u));
    auto cv = assembler->Add(t, ir::Operand{t});
    assembler->SaveFlags(cv, ir::Flags::Carry | ir::Flags::Overflow);
    return lo;
}

void X64Decoder::DecodeMulOneOperand(_DInst& insn, bool sign) {
    auto& op0 = insn.ops[0];
    auto src = ToValue(Src(insn, op0));

    switch (op0.size) {
        case 8: {
            ir::Value hi;
            auto product = MulWithFlags(assembler, R(_RegisterType::R_AL), src, 8, sign, &hi);
            R(_RegisterType::R_AL, product);
            R(_RegisterType::R_AH, hi);
            break;
        }
        case 16: {
            ir::Value hi;
            auto product = MulWithFlags(assembler, R(_RegisterType::R_AX), src, 16, sign, &hi);
            R(_RegisterType::R_AX, product);
            R(_RegisterType::R_DX, hi);
            break;
        }
        case 32: {
            ir::Value hi;
            auto product = MulWithFlags(assembler, R(_RegisterType::R_EAX), src, 32, sign, &hi);
            R(_RegisterType::R_EAX, product);
            R(_RegisterType::R_EDX, hi);
            break;
        }
        case 64: {
            ir::Value hi;
            auto lo = MulWithFlags(assembler, R(_RegisterType::R_RAX), src, 64, sign, &hi);
            R(_RegisterType::R_RAX, lo);
            R(_RegisterType::R_RDX, hi);
            break;
        }
        default:
            PANIC();
    }
    carry_ = CarryPolarity::Direct;  // CF == 0 (approximation), same either way
}

void X64Decoder::DecodeIMul(_DInst& insn) {
    if (insn.ops[1].type == O_NONE) {
        // One operand form: (R)DX:(R)AX = (R)AX * src.
        DecodeMulOneOperand(insn, true);
        return;
    }

    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    ir::DataClass left_data;
    ir::DataClass right_data;
    if (insn.ops[2].type != O_NONE) {
        // Three operand form: dst = src1 * imm. The immediate is always sign
        // extended to the destination width (distorm may report a narrower
        // operand size than the encoded sign extension).
        left_data = Src(insn, op1);
        if (insn.ops[2].type == O_IMM) {
            auto raw = insn.imm.sqword;
            auto bits = insn.ops[2].size;
            if (bits < 64) {
                raw = (s64(raw) << (64 - bits)) >> (64 - bits);
            }
            right_data = ir::Imm{u64(raw)};
        } else {
            right_data = Src(insn, insn.ops[2]);
        }
    } else {
        // Two operand form: dst = dst * src.
        left_data = Src(insn, op0);
        right_data = Src(insn, op1);
    }

    auto width = op0.size;
    auto left = ToValue(left_data);
    auto right = ToValue(right_data);
    auto product = MulWithFlags(assembler, left, right, width, true);
    Dst(insn, op0, product);
    carry_ = CarryPolarity::Direct;  // CF == 0 (approximation), same either way
}

void X64Decoder::DecodeDiv(_DInst& insn, bool sign) {
    auto& op0 = insn.ops[0];
    auto src = ToValue(Src(insn, op0));

    auto div_q = sign ? &DivQS64 : &DivQU64;
    auto div_r = sign ? &DivRS64 : &DivRU64;

    // Divide the 2*width dividend (composed into a 128 bit hi:lo pair) by the
    // sign/zero extended divisor; quotient goes to (R)AX, remainder to (R)DX.
    switch (op0.size) {
        case 8: {
            auto ax = R(_RegisterType::R_AX);
            auto num = sign ? __ SignExtend(ax).SetType(ir::ValueType::U64)
                            : __ ZeroExtend64(__ ZeroExtend32(ax));
            auto hi = sign ? __ AsrImm(num, ir::Imm(63u)) : __ LoadImm(ir::Imm(u64(0)));
            auto den = Extend(src, ir::ValueType::U64, sign);
            auto quot = __ CallHost(div_q, hi, num, den);
            auto rem = __ CallHost(div_r, hi, num, den);
            R(_RegisterType::R_AL, quot);
            R(_RegisterType::R_AH, rem);
            break;
        }
        case 16: {
            auto lo = __ ZeroExtend32(R(_RegisterType::R_AX));
            auto hi16 = __ ZeroExtend32(R(_RegisterType::R_DX));
            auto num32 = __ Or(__ LslImm(hi16, ir::Imm(16u)), ir::Operand{lo});
            auto num = sign ? __ SignExtend(num32).SetType(ir::ValueType::U64)
                            : __ ZeroExtend64(num32);
            auto hi = sign ? __ AsrImm(num, ir::Imm(63u)) : __ LoadImm(ir::Imm(u64(0)));
            auto den = Extend(src, ir::ValueType::U64, sign);
            auto quot = __ CallHost(div_q, hi, num, den);
            auto rem = __ CallHost(div_r, hi, num, den);
            R(_RegisterType::R_AX, quot);
            R(_RegisterType::R_DX, rem);
            break;
        }
        case 32: {
            auto lo = __ ZeroExtend64(R(_RegisterType::R_EAX));
            auto hi32 = __ ZeroExtend64(R(_RegisterType::R_EDX));
            auto num = __ Or(__ LslImm(hi32, ir::Imm(32u)), ir::Operand{lo});
            auto hi = sign ? __ AsrImm(num.SetType(ir::ValueType::U64), ir::Imm(63u))
                           : __ LoadImm(ir::Imm(u64(0)));
            auto den = Extend(src, ir::ValueType::U64, sign);
            auto quot = __ CallHost(div_q, hi, num, den);
            auto rem = __ CallHost(div_r, hi, num, den);
            R(_RegisterType::R_EAX, quot);
            R(_RegisterType::R_EDX, rem);
            break;
        }
        case 64: {
            auto lo = R(_RegisterType::R_RAX);
            auto hi = R(_RegisterType::R_RDX);
            auto den = Extend(src, ir::ValueType::U64, sign);
            auto quot = __ CallHost(div_q, hi, lo, den);
            auto rem = __ CallHost(div_r, hi, lo, den);
            R(_RegisterType::R_RAX, quot);
            R(_RegisterType::R_RDX, rem);
            break;
        }
        default:
            PANIC();
    }
    // TODO: divide errors (#DE) are not raised; flags are undefined per spec.
}

void X64Decoder::DecodeLea(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto address = GetAddress(insn, op1);
    // SetType (mutation): EmitGetOperand sizes the result from the
    // instruction's own return type, untyped would truncate to 32 bits.
    Dst(insn, op0,
        __ GetOperand(address.ToIROperand())
                .SetType(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32));
}

void X64Decoder::DecodeCondMov(_DInst& insn, Cond cond) {
    // 32 bit cmov clears the upper half of the destination even when the
    // condition is false (modern x86 behaviour, and what Unicorn models), so
    // this cannot be a skip-around branch: select between source and the
    // current destination and let Dst apply the usual width rules.
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    auto check_result = CheckCond(cond);
    auto dst_val = ToValue(Src(insn, op0));
    auto src_val = ToValue(Src(insn, op1));
    auto result = __ Select(check_result, src_val, dst_val).SetType(GetSize(op0.size));
    Dst(insn, op0, result);
}

void X64Decoder::SaveLogicFlags(ir::Value result, u32 width) {
    // AND / OR / XOR / TEST: CF = OF = 0, AF undefined (cleared here),
    // SF / ZF / PF from the result.
    if (width < 32) {
        // The backend's flag-setting logical ops compute N/Z at the host
        // register width (32 bits), which is wrong for narrow results; a
        // separate flag-only op derives them at the guest width.
        auto flagged = __ Or(result, ir::Operand{ir::Imm(u64(0))});
        __ ClearFlags(ir::Flags::Carry | ir::Flags::Overflow | ir::Flags::AuxiliaryCarry);
        __ SaveFlags(flagged, ir::Flags::Negate | ir::Flags::Zero | ir::Flags::Parity);
    } else {
        __ ClearFlags(ir::Flags::Carry | ir::Flags::Overflow | ir::Flags::AuxiliaryCarry);
        __ SaveFlags(result, ir::Flags::Negate | ir::Flags::Parity | ir::Flags::Zero);
    }
    carry_ = CarryPolarity::Direct;  // CF == 0, same under either polarity
}

void X64Decoder::DecodeAnd(_DInst& insn, bool save_result) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = Src(insn, op1);

    auto result = __ And(left, ir::Operand{right});

    SaveLogicFlags(result, op0.size);

    if (save_result) {
        Dst(insn, op0, result);
    }
}

ir::Value X64Decoder::Pop(ir::ValueType type) {
    auto size_byte = ir::GetValueSizeByte(type);
    auto sp = _RegisterType::R_RSP;
    auto address = R(sp);
    auto value = __ LoadMemory(ir::Operand{address}).SetType(type);
    R(sp, __ Add(address, ir::Operand{ir::Imm(u64(size_byte))}));
    return value;
}

void X64Decoder::Push(ir::Value value, ir::ValueType type) {
    auto size_byte = ir::GetValueSizeByte(type);
    auto sp = _RegisterType::R_RSP;
    auto address = R(sp);
    auto new_sp = __ Sub(address, ir::Operand{ir::Imm(u64(size_byte))});
    __ StoreMemory(ir::Operand{new_sp}, value.SetType(type));
    R(sp, new_sp);
}

void X64Decoder::DecodeOr(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = Src(insn, op0);
    auto right = Src(insn, op1);

    auto result = __ Or(ToValue(left), ir::Operand{right});
    SaveLogicFlags(result, op0.size);

    Dst(insn, op0, result);
}

void X64Decoder::DecodeXor(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = Src(insn, op0);
    auto right = Src(insn, op1);

    auto result = __ Xor(ToValue(left), ir::Operand{right});
    SaveLogicFlags(result, op0.size);

    Dst(insn, op0, result);
}

void X64Decoder::DecodePush(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto width = op0.size == 16 ? 16 : (is_64bit ? 64 : 32);
    auto type = GetSize(width);
    if (op0.type == O_IMM || op0.type == O_IMM1) {
        // distorm keeps the immediate sign extended in imm.sqword.
        auto value = __ LoadImm(ir::Imm(insn.imm.sqword)).SetType(GetSignedContainer(width));
        Push(value, type);
        return;
    }
    auto value = ToValue(Src(insn, op0));
    Push(value, type);
}

void X64Decoder::DecodePop(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto width = op0.size == 16 ? 16 : (is_64bit ? 64 : 32);
    auto value = Pop(GetSize(width));
    Dst(insn, op0, value);
}

void X64Decoder::DecodePushA(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto type = GetSize(op0.size);
    ASSERT(type == runtime::ir::ValueType::U32);
    auto sp = R(_RegisterType::R_ESP);
    __ StoreMemory(ir::Operand{sp, -4, ir::OperandPlus}, R(R_EAX));
    __ StoreMemory(ir::Operand{sp, -8, ir::OperandPlus}, R(R_ECX));
    __ StoreMemory(ir::Operand{sp, -12, ir::OperandPlus}, R(R_EDX));
    __ StoreMemory(ir::Operand{sp, -16, ir::OperandPlus}, R(R_EBX));
    __ StoreMemory(ir::Operand{sp, -20, ir::OperandPlus}, R(R_ESP));
    __ StoreMemory(ir::Operand{sp, -24, ir::OperandPlus}, R(R_EBP));
    __ StoreMemory(ir::Operand{sp, -28, ir::OperandPlus}, R(R_RSI));
    __ StoreMemory(ir::Operand{sp, -32, ir::OperandPlus}, R(R_RDI));
    auto new_sp = __ Sub(sp, ir::Operand{32u});
    R(_RegisterType::R_ESP, new_sp);
}

void X64Decoder::DecodePopA(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto type = GetSize(op0.size);
    ASSERT(type == runtime::ir::ValueType::U32);
    auto sp = R(_RegisterType::R_ESP);
    auto edi = __ LoadMemory(ir::Operand{sp});
    R(R_EDI, edi);
    auto esi = __ LoadMemory(ir::Operand{sp, 4u});
    R(R_ESI, esi);
    auto ebp = __ LoadMemory(ir::Operand{sp, 8u});
    R(R_EBP, ebp);
    auto ebx = __ LoadMemory(ir::Operand{sp, 16u});
    R(R_EBX, ebx);
    auto edx = __ LoadMemory(ir::Operand{sp, 20u});
    R(R_EDX, edx);
    auto ecx = __ LoadMemory(ir::Operand{sp, 24u});
    R(R_ECX, ecx);
    auto eax = __ LoadMemory(ir::Operand{sp, 28u});
    R(R_EAX, eax);
    auto new_sp = __ Add(sp, ir::Operand{32u});
    R(_RegisterType::R_ESP, new_sp);
}

void X64Decoder::DecodeShlShr(_DInst& insn, bool shr) { DecodeShift(insn, shr ? 1 : 0); }

void X64Decoder::DecodeSar(_DInst& insn) { DecodeShift(insn, 2); }

void X64Decoder::DecodeShift(_DInst& insn, int kind) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto width = op0.size;
    auto left = ToValue(Src(insn, op0));
    auto count_raw = ToValue(Src(insn, op1));

    // x86 masks the shift count to 5 bits (6 bits for 64 bit operands),
    // regardless of the operand width; the backend shift ops mask to their
    // own width, so narrow shifts must run in a 32 bit container.
    auto count_mask = width == 64 ? ir::Imm(0x3Fu) : ir::Imm(0x1Fu);
    auto count = __ And(count_raw, ir::Operand{count_mask});
    ir::Value shifted = width < 32 ? __ ZeroExtend32(left) : left;

    ir::Value result;
    if (kind == 0) {
        result = __ LslValue(shifted, count);
    } else if (kind == 1) {
        result = __ LsrValue(shifted, count);
    } else {
        // Narrow SAR must sign extend to 32 bits first: the backend shift is a
        // 32/64 bit op, an unsigned narrow value would shift in zeros.
        auto ext = width < 32 ? __ SignExtend(left).SetType(ir::ValueType::S32)
                              : left.SetType(GetSignedContainer(width));
        result = __ AsrValue(ext, count);
    }
    result = result.SetCastType(GetSize(width));
    // For narrow shifts the flag-defining op must be typed at the guest
    // width: the backends derive SF/ZF from the operation width, which the
    // 32 bit container would get wrong.
    ir::Value flag_value = width < 32
            ? __ And(result, ir::Operand{ir::Imm((u64(1) << width) - 1)}).SetType(GetSize(width))
            : result;

    // A zero shift count leaves the flags untouched; skip the flag update.
    auto skip_flags = __ NotGoto(__ TestNotZero(count));
    // SF / ZF / PF from the result via a flag-setting logical op. CF / OF are
    // cleared as an approximation (CF should be the last bit shifted out and OF
    // is only defined for count == 1; the backend cannot express that partial
    // update, see report).
    auto flagged = __ Or(flag_value, ir::Operand{ir::Imm(u64(0))});
    __ SaveFlags(flagged, ir::Flags::Negate | ir::Flags::Zero | ir::Flags::Parity);
    __ BindLabel(skip_flags);
    carry_ = CarryPolarity::Direct;  // CF == 0 (approximation), same either way

    Dst(insn, op0, result);
}

void X64Decoder::DecodeCmp(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = Src(insn, op1);

    auto result = __ Sub(left, ir::Operand{right});
    __ SaveFlags(result, ir::Flags::All);
}

void X64Decoder::DecodeAndNot(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto left = ToValue(Src(insn, op0));
    auto right = Src(insn, op1);

    auto result = __ AndNot(left, ir::Operand{right});
    SaveLogicFlags(result, op0.size);

    Dst(insn, op0, result);
}

}  // namespace swift::x86
