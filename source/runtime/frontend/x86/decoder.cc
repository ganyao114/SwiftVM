//
// Created by SwiftGan on 2021/1/1.
//

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "runtime/frontend/x86/cpu.h"
#include "runtime/frontend/x86/decoder_internal.h"

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

static bool IsGuestStackRegister(unsigned reg) {
    switch (static_cast<_RegisterType>(reg)) {
        case _RegisterType::R_RSP:
        case _RegisterType::R_ESP:
        case _RegisterType::R_SP:
        case _RegisterType::R_RBP:
        case _RegisterType::R_EBP:
        case _RegisterType::R_BP:
            return true;
        default:
            return false;
    }
}

static bool IsThreadPrivateAddress(const _DInst& insn) {
    const auto segment = static_cast<_RegisterType>(SEGMENT_GET(insn.segment));
    if (segment == _RegisterType::R_FS || segment == _RegisterType::R_GS) {
        return true;
    }
    if (IsGuestStackRegister(insn.base)) {
        return true;
    }
    for (const auto& op : insn.ops) {
        if ((op.type == O_SMEM || op.type == O_MEM) &&
            IsGuestStackRegister(op.index)) {
            return true;
        }
    }
    return false;
}

static void FixupMovbeOperandSize(_DInst& insn, const u8* code) {
    if (insn.opcode != I_MOVBE || FLAG_GET_OPSIZE(insn.flags) != Decode32Bits) {
        return;
    }

    // This distorm snapshot treats MOVBE's 66H operand-size override as an
    // unused prefix: both F0 (reg <- mem) and F1 (mem <- reg) are returned
    // with 32-bit operands/register IDs. Recover the architectural 16-bit
    // form from the original bytes before generic Src/Dst processing.
    for (u32 i = 0; i < insn.size; ++i) {
        if (code[i] != 0x66 || (insn.unusedPrefixesMask & (u16(1) << i)) == 0) {
            continue;
        }
        for (auto& op : insn.ops) {
            if (op.type == O_NONE) break;
            op.size = 16;
            if (op.type == O_REG && op.index >= REGS32_BASE &&
                op.index < REGS32_BASE + 16) {
                op.index = static_cast<u8>(op.index + (REGS16_BASE - REGS32_BASE));
            }
        }
        insn.flags &= ~(u16(3) << 8);
        auto* normalized = &insn;
        FLAG_SET_OPSIZE(normalized, Decode16Bits);
        insn.unusedPrefixesMask &= ~(u16(1) << i);
        return;
    }
}

ABIDescriptor GetABIDescriptor32() { return {{}, {}, general_return_x86, float_return_x86}; }

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

bool X64Decoder::TsoOrdered(const _DInst& insn) const {
    if (GetTsoMode() == runtime::TsoMode::AcqRel) {
        // ABI-owned stack and TLS locations are private to this guest thread.
        // Shared acquire/release accesses still order surrounding plain
        // accesses, while LOCK must retain its full ordered/atomic semantics.
        if (!(insn.flags & FLAG_LOCK) && IsThreadPrivateAddress(insn)) {
            return false;
        }
        return true;
    }
    // LOCK-prefixed instructions are full fences on x86; order their accesses
    // even in Relaxed mode.
    return (insn.flags & FLAG_LOCK) != 0;
}

ir::Value X64Decoder::MemLoad(const ir::Operand& addr, ir::ValueType type, bool tso) {
    if (tso) {
        return assembler->LoadMemoryTSO(addr).SetType(type);
    }
    return assembler->LoadMemory(addr).SetType(type);
}

void X64Decoder::MemStore(const ir::Operand& addr, ir::Value value, bool tso) {
    if (tso) {
        assembler->StoreMemoryTSO(addr, value);
    } else {
        assembler->StoreMemory(addr, value);
    }
}

#define __ assembler->

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
        // A YMM entry names the HIGH half only; the low half is the matching
        // Xmm entry, which is where every SSE handler already reads and writes.
        offset = offsetof(ThreadContext64, ymm_high) +
                 (info.index - X86RegInfo::Ymm0) * sizeof(Xmm);
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

// x86-64's architectural maximum instruction length. Bounds every raw-byte
// read on the VEX path so a decode at the very end of a mapped page cannot run
// past it.
static constexpr size_t kMaxInsnBytes = 15;

// Bytes of guest code the decode loop is allowed to look at per instruction.
// DisDecode is handed 0x10 unconditionally, so that -- not kMaxInsnBytes -- is
// the window that must be proven readable.
static constexpr size_t kFetchWindow = 0x10;

void X64Decoder::Decode() {
    pc = start;
    while (!end_decode) {
        // Instruction fetch is the one guest access made from *host* code, so
        // it is the one that must never fault: runtime.cpp's HandleFault only
        // recovers faults whose host pc lies inside a JIT buffer, so a fault
        // raised here kills the host process instead of the guest. Two things
        // are therefore checked before any byte is touched:
        //   1. pc itself is backed by a guest mapping (GetPointer validates),
        //   2. the whole 16-byte window every consumer below reads is backed
        //      too -- DisDecode is handed 0x10 bytes unconditionally and the
        //      raw-byte special cases peek at code_ptr[1..4] before any length
        //      is known, so a fetch starting in the last bytes of a mapping
        //      would run off its end.
        // When the window is short, decode from a zero-padded bounce buffer
        // and reject anything whose real length needs the missing bytes: an
        // instruction straddling into an unmapped page is a guest #PF. The
        // padding cannot fake a match in the raw-byte special cases below --
        // every one of them requires a nonzero byte at its last position.
        u8 fetch_buf[kFetchWindow];
        auto code_ptr = reinterpret_cast<u8*>(memory->GetPointer(reinterpret_cast<void*>(pc)));
        if (!code_ptr) {
            Interrupt(InterruptReason::PAGE_FATAL);
            break;
        }
        size_t fetch_avail = kFetchWindow;
        if (!memory->GetPointer(reinterpret_cast<void*>(pc + kFetchWindow - 1))) {
            std::memset(fetch_buf, 0, sizeof(fetch_buf));
            fetch_avail = 0;
            for (size_t i = 0; i < kFetchWindow; ++i) {
                const auto* byte = reinterpret_cast<const u8*>(
                        memory->GetPointer(reinterpret_cast<void*>(pc + i)));
                if (!byte) break;
                fetch_buf[i] = *byte;
                ++fetch_avail;
            }
            code_ptr = fetch_buf;
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
        // CET shadow-stack ops distorm doesn't know, both only reachable on
        // CET-enabled hosts (glibc's _dl_shadow_stack paths): rdsspq/rdsspd
        // (F3 [REX] 0F 1E /1) yields 0 (no shadow stack here), incsspq/incsspd
        // (F3 [REX] 0F AE /5) is a no-op (SSP is not modelled).
        if (code_ptr[0] == 0xF3 && (code_ptr[1] & 0xF0) == 0x40 && code_ptr[2] == 0x0F &&
            ((code_ptr[3] == 0x1E && (code_ptr[4] & 0xF8) == 0xC8) ||
             (code_ptr[3] == 0xAE && (code_ptr[4] & 0xF8) == 0xE8))) {
            if (code_ptr[3] == 0x1E) {
                // rdssp: dst = 0
                u32 idx = (code_ptr[4] & 7) | ((code_ptr[1] & 1) << 3);
                auto reg = static_cast<_RegisterType>((code_ptr[1] & 8) ? (R_RAX + idx)
                                                                        : (R_EAX + idx));
                R(reg, __ LoadImm(ir::Imm(u64(0))));
            }
            __ Nop();
            pc += 5;
            assembler->AdvancePC(ir::Imm{5});
            end_decode = assembler->EndCommit();
            continue;
        }
        // VEX (AVX/AVX2). This distorm snapshot cannot carry AVX: of 117 probed
        // encodings, 40 come back I_UNDEFINED and another 38 silently drop
        // VEX.L, reporting a 256-bit encoding as 128-bit XMM operands. So VEX
        // instructions are decoded from the raw bytes here (vex_decoder.h)
        // rather than handed to distorm.
        //
        // Families still living on the legacy distorm path fall through: when a
        // handler declines, pc is restored and the normal decode runs exactly as
        // before. That keeps this migration incremental instead of a flag day.
        // BMI1/BMI2 share the VEX prefix but are GPR instructions, so they are
        // gated separately: glibc's ifunc needs AVX2 and BMI2 together before
        // it picks an AVX2 string variant, and each of those contains BMI.
        const bool avx_on = AvxEnabled();
        const bool bmi_on = BmiEnabled();
        if ((avx_on || bmi_on) && HasVexPrefix(code_ptr, kMaxInsnBytes)) {
            const auto vex = DecodeVexInsn(code_ptr, kMaxInsnBytes);
            // vex.length beyond the fetch window means the encoding was
            // completed out of the zero padding: it runs into an unmapped
            // page, so the guest faults rather than executing it.
            if (vex.valid && vex.length > fetch_avail) {
                Interrupt(InterruptReason::PAGE_FATAL);
                break;
            }
            if (vex.valid) {
                const auto saved_pc = pc;
                // Handlers resolve RIP-relative operands against the END of the
                // instruction, matching GetAddress's convention, so advance
                // first and restore only if nobody claims it.
                pc += vex.length;
                // No opcode collision: BMI lives on 0F38 F2/F3/F5/F6/F7 and
                // 0F3A F0, while the AVX handlers' F2..F6 cases are all on the
                // 0F map.
                if (DecodeBmi(vex) ||
                    (avx_on &&
                     (DecodeAvxMul(vex) || DecodeAvxFma(vex) || DecodeAvxInt(vex) ||
                      DecodeAvxFp(vex) || DecodeAvxHadd(vex) || DecodeAvxBlend(vex) ||
                      DecodeAvxGather(vex) || DecodeAvxMisc(vex) ||
                      DecodeAvxSse4(vex)))) {
                    assembler->AdvancePC(ir::Imm{vex.length});
                    end_decode = assembler->EndCommit();
                    continue;
                }
                pc = saved_pc;
            }
        }
        // distorm reports DF C0+i (FFREEP ST(i)) as an undefined one-byte
        // instruction. Decode it here so the architectural free+pop operation
        // remains available and the stream advances by its real length.
        if (code_ptr[0] == 0xDF && (code_ptr[1] & 0xF8) == 0xC0) {
            DecodeX87FreePop(static_cast<u8>(code_ptr[1] & 7));
            pc += 2;
            assembler->AdvancePC(ir::Imm{2});
            end_decode = assembler->EndCommit();
            continue;
        }
        // This distorm snapshot predates RDSEED and decodes 0F C7 /7 as a
        // one-byte undefined instruction.  Recognize the register-only
        // encoding here (optional 66 and REX prefixes) so the stream advances
        // by its architectural length.
        if (is_64bit) {
            u32 seed_offset = 0;
            bool operand16 = false;
            u8 rex = 0;
            if (code_ptr[seed_offset] == 0x66) {
                operand16 = true;
                ++seed_offset;
            }
            if ((code_ptr[seed_offset] & 0xF0) == 0x40) {
                rex = code_ptr[seed_offset++];
            }
            if (code_ptr[seed_offset] == 0x0F && code_ptr[seed_offset + 1] == 0xC7 &&
                (code_ptr[seed_offset + 2] & 0xF8) == 0xF8) {
                const u32 index = (code_ptr[seed_offset + 2] & 7) | ((rex & 1) << 3);
                const u32 width = (rex & 8) ? 64 : (operand16 ? 16 : 32);
                const auto first = width == 64 ? R_RAX : (width == 32 ? R_EAX : R_AX);
                DecodeRandomRegister(static_cast<_RegisterType>(first + index), width);
                const u32 size = seed_offset + 3;
                pc += size;
                assembler->AdvancePC(ir::Imm{size});
                end_decode = assembler->EndCommit();
                continue;
            }
        }
        _DInst insn = DisDecode(code_ptr, 0x10, is_64bit);
        FixupMovbeOperandSize(insn, code_ptr);
        if (insn.opcode == UINT16_MAX || insn.size == 0) {
            // size == 0 would loop forever at the same pc.
            Interrupt(InterruptReason::ILL_CODE);
            break;
        }
        if (insn.size > fetch_avail) {
            // Decoded out of the zero padding: the real encoding runs into an
            // unmapped page, which architecturally faults.
            Interrupt(InterruptReason::PAGE_FATAL);
            break;
        }
        // distorm leaves an architecturally invalid LOCK prefix in the unused
        // prefix mask instead of rejecting the instruction.  Do not silently
        // execute the unlocked opcode (notably LOCK IMUL and register-dest
        // forms): x86 raises #UD.
        bool invalid_lock = false;
        for (u32 i = 0; i < insn.size && i < 16; ++i) {
            if ((insn.unusedPrefixesMask & (u16(1) << i)) != 0 && code_ptr[i] == 0xF0) {
                invalid_lock = true;
                break;
            }
        }
        if (invalid_lock) {
            pc += insn.size;
            Interrupt(InterruptReason::ILL_CODE);
            assembler->AdvancePC(ir::Imm{insn.size});
            end_decode = assembler->EndCommit();
            break;
        }
        pc += insn.size;
        // VEX handlers re-read the prefix bytes (see DecodeVex): distorm does
        // not expose VEX.L/W/vvvv and mis-sizes the AVX2 integer forms.
        insn_bytes = code_ptr;
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
        case I_INT1:
        case I_INT:
            // With no guest IDT/kernel, software and debug interrupts surface
            // through the runtime's breakpoint/trap path.
            Interrupt(InterruptReason::BRK);
            break;
        case I_SYSCALL:
            // syscall: RCX = next RIP (pc already advanced), R11 = RFLAGS.
            R(_RegisterType::R_RCX, __ LoadImm(ir::Imm(pc)));
            R(_RegisterType::R_R11, __ LoadImm(ir::Imm(u64(0x202))));
            Interrupt(InterruptReason::SVC);
            break;
        case I_CPUID:
            DecodeCpuid(insn);
            break;
        case I_RDTSC:
            DecodeTimestamp(false);
            break;
        case I_RDTSCP:
            DecodeTimestamp(true);
            break;
        case I_RDRAND:
            DecodeRandom(insn);
            break;
        case I_MOVBE:
            DecodeMovbe(insn);
            break;
        case I_MOVNTI:
            DecodeMovnti(insn);
            break;
        case I_XLAT:
            DecodeXlat(insn);
            break;
        case I_XGETBV:
            // XSAVE/OSXSAVE are deliberately absent from CPUID, therefore
            // XGETBV is architecturally unavailable and must #UD.
            Interrupt(InterruptReason::ILL_CODE);
            break;
        case I_UD2:
            Interrupt(InterruptReason::ILL_CODE);
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
                R(_RegisterType::R_RSP, __ Add(sp, ir::Operand{ir::Imm(u64(insn.imm.word))}));
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
        case I_LODS:
            DecodeLods(insn);
            break;
        case I_CMPS:
            DecodeCmps(insn);
            break;
        case I_SCAS:
            DecodeScas(insn);
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
        case I_SHLD:
            DecodeDoubleShift(insn, false);
            break;
        case I_SHRD:
            DecodeDoubleShift(insn, true);
            break;
        case I_RCL:
            DecodeRotateCarry(insn, true);
            break;
        case I_RCR:
            DecodeRotateCarry(insn, false);
            break;
        case I_CLC:
            __ SetCarry(__ LoadImm(ir::Imm(u8(0))));
            carry_ = CarryPolarity::Direct;
            StorePolarity(false);
            break;
        case I_STC:
            __ SetCarry(__ LoadImm(ir::Imm(u8(1))));
            carry_ = CarryPolarity::Direct;
            StorePolarity(false);
            break;
        case I_CMC:
            __ SetCarry(__ Xor(CarryValue(), ir::Operand{ir::Imm(u8(1))}));
            carry_ = CarryPolarity::Direct;
            StorePolarity(false);
            break;
        case I_CLD:
            StoreDirection(false);
            break;
        case I_STD:
            StoreDirection(true);
            break;
        case I_PUSH:
            DecodePush(insn);
            break;
        case I_POP:
            DecodePop(insn);
            break;
        case I_PUSHF:
            DecodePushf(insn);
            break;
        case I_POPF:
            DecodePopf(insn);
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
            // TestFlags reads the guest flag shadow directly.  CondSelect
            // would observe whatever host NZCV the preceding PF/AF query
            // happened to leave behind (notably after UCOMIS*).
            auto zf = __ TestFlags(ir::Flags::Zero).SetType(ir::ValueType::U8);
            auto sf = __ TestFlags(ir::Flags::Negate).SetType(ir::ValueType::U8);
            auto lo = __ Or(cf, ir::Operand{__ LslImm(pf, ir::Imm(2u))});
            auto mid = __ Or(__ LslImm(af, ir::Imm(4u)), ir::Operand{__ LslImm(zf, ir::Imm(6u))});
            auto ah = __ Or(
                    __ Or(lo, ir::Operand{mid}),
                    ir::Operand{__ Or(__ LslImm(sf, ir::Imm(7u)), ir::Operand{ir::Imm(u64(2))})});
            R(_RegisterType::R_AH, ah);
            break;
        }
        case I_SAHF: {
            // SF:ZF:0:AF:0:PF:1:CF <- AH.  Save each independently because
            // SAHF permits combinations (notably SF+ZF) that no single ALU
            // result can represent.
            auto ah = R(_RegisterType::R_AH).SetType(ir::ValueType::U64);
            auto bit = [&](u32 position) {
                return __ And(__ LsrImm(ah, ir::Imm(position)),
                              ir::Operand{ir::Imm(u64(1))});
            };
            auto one = __ LoadImm(ir::Imm(u64(1)));
            auto zero = __ LoadImm(ir::Imm(u64(0)));

            auto pf = bit(2);
            auto parity_value = __ Select(__ TestNotZero(pf), zero, one);
            __ SaveFlags(__ Or(parity_value, ir::Operand{ir::Imm(u64(0))}),
                         ir::Flags::Parity);

            auto zf = bit(6);
            auto zero_value = __ Select(__ TestNotZero(zf), zero, one);
            __ SaveFlags(__ Or(zero_value, ir::Operand{ir::Imm(u64(0))}),
                         ir::Flags::Zero);

            auto sf = bit(7);
            auto sign_value = __ LslImm(sf, ir::Imm(63u)).SetType(ir::ValueType::U64);
            __ SaveFlags(__ Or(sign_value, ir::Operand{ir::Imm(u64(0))}),
                         ir::Flags::Negate);

            // 0xF + AFbit has an auxiliary carry exactly when AFbit is one.
            auto auxiliary_value =
                    __ Add(__ LoadImm(ir::Imm(u64(0xF))), ir::Operand{bit(4)});
            __ SaveFlags(auxiliary_value, ir::Flags::AuxiliaryCarry);

            __ SetCarry(bit(0));
            carry_ = CarryPolarity::Direct;
            StorePolarity(false);
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
        // ---- x87 floating-point stack ----------------------------------
        case I_FLD:
        case I_FST:
        case I_FSTP:
        case I_FILD:
        case I_FIST:
        case I_FISTP:
        case I_FISTTP:
        case I_FADD:
        case I_FADDP:
        case I_FIADD:
        case I_FMUL:
        case I_FMULP:
        case I_FIMUL:
        case I_FSUB:
        case I_FSUBR:
        case I_FSUBP:
        case I_FSUBRP:
        case I_FISUB:
        case I_FISUBR:
        case I_FDIV:
        case I_FDIVR:
        case I_FDIVP:
        case I_FDIVRP:
        case I_FIDIV:
        case I_FIDIVR:
        case I_FCOM:
        case I_FCOMP:
        case I_FCOMPP:
        case I_FUCOM:
        case I_FUCOMP:
        case I_FUCOMPP:
        case I_FICOM:
        case I_FICOMP:
        case I_FCOMI:
        case I_FCOMIP:
        case I_FUCOMI:
        case I_FUCOMIP:
        case I_FCHS:
        case I_FABS:
        case I_FTST:
        case I_FXAM:
        case I_FSQRT:
        case I_FRNDINT:
        case I_FPREM:
        case I_FPREM1:
        case I_FSCALE:
        case I_FXTRACT:
        case I_FSIN:
        case I_FCOS:
        case I_FSINCOS:
        case I_FPTAN:
        case I_FPATAN:
        case I_FYL2X:
        case I_FYL2XP1:
        case I_F2XM1:
        case I_FLD1:
        case I_FLDL2T:
        case I_FLDL2E:
        case I_FLDPI:
        case I_FLDLG2:
        case I_FLDLN2:
        case I_FLDZ:
        case I_FXCH:
        case I_FFREE:
        case I_FINCSTP:
        case I_FDECSTP:
        case I_FNSTCW:
        case I_FSTCW:
        case I_FLDCW:
        case I_FNSTSW:
        case I_FSTSW:
        case I_FNINIT:
        case I_FINIT:
        case I_FNCLEX:
        case I_FCLEX:
        case I_FNSTENV:
        case I_FSTENV:
        case I_FLDENV:
        case I_FNOP:
        case I_WAIT:
            DecodeX87(insn);
            break;
        // ---- SSE subset (glibc baseline SSE2 string routines) ----
        case I_MOVD:
            DecodeMovd(insn);
            break;
        case I_MOVQ:
            DecodeMovq(insn);
            break;
        case I_MOVDQA:
        case I_MOVDQU:
        case I_MOVAPS:
        case I_MOVUPS:
        case I_MOVAPD:
        case I_MOVUPD:
        case I_MOVNTDQ:
        case I_MOVNTPS:
        case I_MOVNTPD:
        case I_LDDQU:
            DecodeMovVec(insn);
            break;
        case I_MOVSD:
            DecodeMovsd(insn);
            break;
        case I_MOVSS:
            DecodeMovss(insn);
            break;
        case I_MOVLPD:
        case I_MOVLPS:
            DecodeMovHalf(insn, false);
            break;
        case I_MOVHPD:
        case I_MOVHPS:
            DecodeMovHalf(insn, true);
            break;
        case I_MOVHLPS:
            DecodeMovhlps(insn, false);
            break;
        case I_MOVLHPS:
            DecodeMovhlps(insn, true);
            break;
        case I_MOVMSKPS:
            DecodeMovmsk(insn, false);
            break;
        case I_MOVMSKPD:
            DecodeMovmsk(insn, true);
            break;
        case I_PXOR:
        case I_XORPS:
        case I_XORPD:
            DecodeVecBitwise(insn, VecBitwiseOp::Xor);
            break;
        case I_POR:
        case I_ORPS:
        case I_ORPD:
            DecodeVecBitwise(insn, VecBitwiseOp::Or);
            break;
        case I_PAND:
        case I_ANDPS:
        case I_ANDPD:
            DecodeVecBitwise(insn, VecBitwiseOp::And);
            break;
        case I_PANDN:
        case I_ANDNPS:
        case I_ANDNPD:
            DecodeVecBitwise(insn, VecBitwiseOp::AndNot);
            break;
        case I_PADDQ:
            DecodeVecInt(insn, VecIntOp::Add, 64);
            break;
        case I_PSUBQ:
            DecodeVecInt(insn, VecIntOp::Sub, 64);
            break;
        case I_PUNPCKLDQ:
            DecodeVecZip(insn, 32, false);
            break;
        case I_PUNPCKHDQ:
            DecodeVecZip(insn, 32, true);
            break;
        case I_PUNPCKLQDQ:
            DecodeVecZip(insn, 64, false);
            break;
        case I_PUNPCKHQDQ:
            DecodeVecZip(insn, 64, true);
            break;
        case I_PMULUDQ:
            DecodeSseMulWiden(insn, false);
            break;
        case I_PMULDQ:
            DecodeSseMulWiden(insn, true);
            break;
        case I_PADDB:
            DecodeVecInt(insn, VecIntOp::Add, 8);
            break;
        case I_PSUBB:
            DecodeVecInt(insn, VecIntOp::Sub, 8);
            break;
        case I_PADDW:
            DecodeVecInt(insn, VecIntOp::Add, 16);
            break;
        case I_PSUBW:
            DecodeVecInt(insn, VecIntOp::Sub, 16);
            break;
        case I_PADDD:
            DecodeVec4Add(insn);
            break;
        case I_PSUBD:
            DecodeVecInt(insn, VecIntOp::Sub, 32);
            break;
        case I_PADDSB: DecodeVecSat(insn, false, 8, true); break;
        case I_PADDSW: DecodeVecSat(insn, false, 16, true); break;
        case I_PADDUSB: DecodeVecSat(insn, false, 8, false); break;
        case I_PADDUSW: DecodeVecSat(insn, false, 16, false); break;
        case I_PSUBSB: DecodeVecSat(insn, true, 8, true); break;
        case I_PSUBSW: DecodeVecSat(insn, true, 16, true); break;
        case I_PSUBUSB: DecodeVecSat(insn, true, 8, false); break;
        case I_PSUBUSW: DecodeVecSat(insn, true, 16, false); break;
        case I_PACKSSWB: DecodeVecPack(insn, 16, false); break;
        case I_PACKSSDW: DecodeVecPack(insn, 32, false); break;
        case I_PACKUSWB: DecodeVecPack(insn, 16, true); break;
        case I_PCMPEQB:
            DecodeVecInt(insn, VecIntOp::CmpEq, 8);
            break;
        case I_PCMPEQW:
            DecodeVecInt(insn, VecIntOp::CmpEq, 16);
            break;
        case I_PCMPEQD:
            DecodeVecInt(insn, VecIntOp::CmpEq, 32);
            break;
        case I_PCMPGTB:
            DecodeVecInt(insn, VecIntOp::CmpGt, 8);
            break;
        case I_PCMPGTW:
            DecodeVecInt(insn, VecIntOp::CmpGt, 16);
            break;
        case I_PCMPGTD:
            DecodeVecInt(insn, VecIntOp::CmpGt, 32);
            break;
        case I_PMINUB:
            DecodeVecMinMax(insn, false, 8, false);
            break;
        case I_PMAXUB:
            DecodeVecMinMax(insn, true, 8, false);
            break;
        case I_PMINUD:
            DecodeVecMinMax(insn, false, 32, false);
            break;
        case I_PMAXUD:
            DecodeVecMinMax(insn, true, 32, false);
            break;
        case I_PMINSW:
            DecodeVecMinMax(insn, false, 16, true);
            break;
        case I_PMAXSW:
            DecodeVecMinMax(insn, true, 16, true);
            break;
        case I_PAVGB:
            DecodeVecAvg(insn, 8);
            break;
        case I_PAVGW:
            DecodeVecAvg(insn, 16);
            break;
        case I_PSADBW:
            DecodeVecAbsDiffSum8(insn);
            break;
        case I_PUNPCKLBW:
            DecodeVecZip(insn, 8, false);
            break;
        case I_PUNPCKHBW:
            DecodeVecZip(insn, 8, true);
            break;
        case I_PUNPCKLWD:
            DecodeVecZip(insn, 16, false);
            break;
        case I_PUNPCKHWD:
            DecodeVecZip(insn, 16, true);
            break;
        case I_UNPCKLPS:
            DecodeVecZip(insn, 32, false);
            break;
        case I_UNPCKHPS:
            DecodeVecZip(insn, 32, true);
            break;
        case I_UNPCKLPD:
            DecodeVecZip(insn, 64, false);
            break;
        case I_UNPCKHPD:
            DecodeVecZip(insn, 64, true);
            break;
        case I_PSHUFD:
            DecodePshufd(insn);
            break;
        case I_SHUFPS:
            DecodeShufps(insn, false);
            break;
        case I_SHUFPD:
            DecodeShufps(insn, true);
            break;
        case I_PSLLDQ:
            DecodePshiftDQ(insn, true);
            break;
        case I_PSRLDQ:
            DecodePshiftDQ(insn, false);
            break;
        case I_PSLLW:
            DecodePshift(insn, true, 0);
            break;
        case I_PSLLD:
            DecodePshift(insn, true, 1);
            break;
        case I_PSLLQ:
            DecodePshift(insn, true, 2);
            break;
        case I_PSRLW:
            DecodePshift(insn, false, 0);
            break;
        case I_PSRLD:
            DecodePshift(insn, false, 1);
            break;
        case I_PSRLQ:
            DecodePshift(insn, false, 2);
            break;
        case I_PSRAW:
            DecodePshiftA(insn, 0);
            break;
        case I_PSRAD:
            DecodePshiftA(insn, 1);
            break;
        case I_ADDPS:
            DecodePackedFloatOp(insn, VecFloatOp::Add, 32);
            break;
        case I_ADDPD:
            DecodePackedFloatOp(insn, VecFloatOp::Add, 64);
            break;
        case I_SUBPS:
            DecodePackedFloatOp(insn, VecFloatOp::Sub, 32);
            break;
        case I_SUBPD:
            DecodePackedFloatOp(insn, VecFloatOp::Sub, 64);
            break;
        case I_MULPS:
            DecodePackedFloatOp(insn, VecFloatOp::Mul, 32);
            break;
        case I_MULPD:
            DecodePackedFloatOp(insn, VecFloatOp::Mul, 64);
            break;
        case I_DIVPS:
            DecodePackedFloatOp(insn, VecFloatOp::Div, 32);
            break;
        case I_DIVPD:
            DecodePackedFloatOp(insn, VecFloatOp::Div, 64);
            break;
        case I_ADDSS:
            DecodeScalarFloatOp(insn, VecFloatOp::Add);
            break;
        case I_SUBSS:
            DecodeScalarFloatOp(insn, VecFloatOp::Sub);
            break;
        case I_MULSS:
            DecodeScalarFloatOp(insn, VecFloatOp::Mul);
            break;
        case I_DIVSS:
            DecodeScalarFloatOp(insn, VecFloatOp::Div);
            break;
        case I_ADDSD:
            DecodeScalarFloatOp(insn, VecFloatOp::Add, 64);
            break;
        case I_SUBSD:
            DecodeScalarFloatOp(insn, VecFloatOp::Sub, 64);
            break;
        case I_MULSD:
            DecodeScalarFloatOp(insn, VecFloatOp::Mul, 64);
            break;
        case I_DIVSD:
            DecodeScalarFloatOp(insn, VecFloatOp::Div, 64);
            break;
        case I_CMPEQPS: DecodeFloatCompareMask(insn, 32, 0, false); break;
        case I_CMPLTPS: DecodeFloatCompareMask(insn, 32, 1, false); break;
        case I_CMPLEPS: DecodeFloatCompareMask(insn, 32, 2, false); break;
        case I_CMPUNORDPS: DecodeFloatCompareMask(insn, 32, 3, false); break;
        case I_CMPNEQPS: DecodeFloatCompareMask(insn, 32, 4, false); break;
        case I_CMPNLTPS: DecodeFloatCompareMask(insn, 32, 5, false); break;
        case I_CMPNLEPS: DecodeFloatCompareMask(insn, 32, 6, false); break;
        case I_CMPORDPS: DecodeFloatCompareMask(insn, 32, 7, false); break;
        case I_CMPEQPD: DecodeFloatCompareMask(insn, 64, 0, false); break;
        case I_CMPLTPD: DecodeFloatCompareMask(insn, 64, 1, false); break;
        case I_CMPLEPD: DecodeFloatCompareMask(insn, 64, 2, false); break;
        case I_CMPUNORDPD: DecodeFloatCompareMask(insn, 64, 3, false); break;
        case I_CMPNEQPD: DecodeFloatCompareMask(insn, 64, 4, false); break;
        case I_CMPNLTPD: DecodeFloatCompareMask(insn, 64, 5, false); break;
        case I_CMPNLEPD: DecodeFloatCompareMask(insn, 64, 6, false); break;
        case I_CMPORDPD: DecodeFloatCompareMask(insn, 64, 7, false); break;
        case I_CMPEQSS: DecodeFloatCompareMask(insn, 32, 0, true); break;
        case I_CMPLTSS: DecodeFloatCompareMask(insn, 32, 1, true); break;
        case I_CMPLESS: DecodeFloatCompareMask(insn, 32, 2, true); break;
        case I_CMPUNORDSS: DecodeFloatCompareMask(insn, 32, 3, true); break;
        case I_CMPNEQSS: DecodeFloatCompareMask(insn, 32, 4, true); break;
        case I_CMPNLTSS: DecodeFloatCompareMask(insn, 32, 5, true); break;
        case I_CMPNLESS: DecodeFloatCompareMask(insn, 32, 6, true); break;
        case I_CMPORDSS: DecodeFloatCompareMask(insn, 32, 7, true); break;
        case I_CMPEQSD: DecodeFloatCompareMask(insn, 64, 0, true); break;
        case I_CMPLTSD: DecodeFloatCompareMask(insn, 64, 1, true); break;
        case I_CMPLESD: DecodeFloatCompareMask(insn, 64, 2, true); break;
        case I_CMPUNORDSD: DecodeFloatCompareMask(insn, 64, 3, true); break;
        case I_CMPNEQSD: DecodeFloatCompareMask(insn, 64, 4, true); break;
        case I_CMPNLTSD: DecodeFloatCompareMask(insn, 64, 5, true); break;
        case I_CMPNLESD: DecodeFloatCompareMask(insn, 64, 6, true); break;
        case I_CMPORDSD: DecodeFloatCompareMask(insn, 64, 7, true); break;
        case I_MINPS: DecodeFloatMinMax(insn, 32, false, false); break;
        case I_MAXPS: DecodeFloatMinMax(insn, 32, true, false); break;
        case I_MINPD: DecodeFloatMinMax(insn, 64, false, false); break;
        case I_MAXPD: DecodeFloatMinMax(insn, 64, true, false); break;
        case I_MINSS: DecodeFloatMinMax(insn, 32, false, true); break;
        case I_MAXSS: DecodeFloatMinMax(insn, 32, true, true); break;
        case I_MINSD: DecodeFloatMinMax(insn, 64, false, true); break;
        case I_MAXSD: DecodeFloatMinMax(insn, 64, true, true); break;
        case I_SQRTPS: DecodeFloatUnary(insn, 32, 0, false); break;
        case I_SQRTPD: DecodeFloatUnary(insn, 64, 0, false); break;
        case I_SQRTSS: DecodeFloatUnary(insn, 32, 0, true); break;
        case I_SQRTSD: DecodeFloatUnary(insn, 64, 0, true); break;
        case I_RCPPS: DecodeFloatUnary(insn, 32, 1, false); break;
        case I_RCPSS: DecodeFloatUnary(insn, 32, 1, true); break;
        case I_RSQRTPS: DecodeFloatUnary(insn, 32, 2, false); break;
        case I_RSQRTSS: DecodeFloatUnary(insn, 32, 2, true); break;
        case I_PMADDWD:
            DecodeVecMadd16(insn);
            break;
        case I_MOVSHDUP:
            DecodeVecDupPairs32(insn, true);
            break;
        case I_MOVSLDUP:
            DecodeVecDupPairs32(insn, false);
            break;
        case I_MOVDDUP:
            DecodeMovddup(insn);
            break;
        // I_HADDPS / I_HSUBPS are deliberately NOT listed here: they fall
        // through to `default:` -> DecodeSse4, alongside haddpd/hsubpd. The
        // host-lambda implementation that used to sit here flipped the sign
        // bit of a NaN result (clang lowers the pair of float subtractions to
        // FNEG + FADDP at -O2), so it was replaced by the pure-IR horizontal
        // path in decoder_sse4.cc rather than patched.
        case I_PEXTRW:
            DecodePextrw(insn);
            break;
        case I_PINSRW:
            DecodePinsrw(insn);
            break;
        case I_PMULLW:
            DecodeVecMul(insn, 16);
            break;
        case I_PMULHW:
            DecodeVecMulHigh16(insn, true);
            break;
        case I_PMULHUW:
            DecodeVecMulHigh16(insn, false);
            break;
        case I_MASKMOVDQU:
            DecodeMaskmovdqu(insn);
            break;
        case I_PSHUFLW:
            DecodePshufw(insn, false);
            break;
        case I_PSHUFHW:
            DecodePshufw(insn, true);
            break;
        case I_CVTSI2SS:
            DecodeCvtsi2ss(insn);
            break;
        case I_CVTSI2SD:
            DecodeCvtsi2sd(insn);
            break;
        case I_CVTTSS2SI:
            DecodeCvttss2si(insn);
            break;
        case I_CVTTSD2SI:
            DecodeCvttsd2si(insn);
            break;
        case I_CVTSS2SI:
            DecodeCvtFloatToInt(insn, 32);
            break;
        case I_CVTSD2SI:
            DecodeCvtFloatToInt(insn, 64);
            break;
        case I_CVTDQ2PS: DecodePackedConvert(insn, 0); break;
        case I_CVTDQ2PD: DecodePackedConvert(insn, 1); break;
        case I_CVTPS2DQ: DecodePackedConvert(insn, 2); break;
        case I_CVTTPS2DQ: DecodePackedConvert(insn, 3); break;
        case I_CVTPD2DQ: DecodePackedConvert(insn, 4); break;
        case I_CVTTPD2DQ: DecodePackedConvert(insn, 5); break;
        case I_CVTPS2PD: DecodePackedConvert(insn, 6); break;
        case I_CVTPD2PS: DecodePackedConvert(insn, 7); break;
        case I_CVTSD2SS:
            DecodeCvtsd2ss(insn);
            break;
        case I_CVTSS2SD:
            DecodeCvtss2sd(insn);
            break;
        case I_POPCNT:
            DecodePopcnt(insn);
            break;
        case I_BSWAP:
            DecodeBswap(insn);
            break;
        case I_LZCNT:
            // With the BMI gate on this adds the CF that the plain LZCNT path
            // omits; with it off it falls through to today's behaviour.
            DecodeLzcntBmi(insn);
            break;
        case I_CRC32:
            DecodeCrc32(insn);
            break;
        case I_LOOP:
        case I_LOOPZ:
        case I_LOOPNZ:
            DecodeLoop(insn);
            break;
        case I_ENTER:
            DecodeEnter(insn);
            break;
        case I_CMPXCHG8B:
            DecodeCmpxchg8b(insn);
            break;
        case I_CMPXCHG16B:
            DecodeCmpxchg16b(insn);
            break;
        case I_PALIGNR:
            DecodePalignr(insn);
            break;
        case I_PSHUFB:
            DecodePshufb(insn);
            break;
        case I_PMOVMSKB:
            DecodePmovmskb(insn);
            break;
        case I_STMXCSR:
            DecodeMxcsr(insn, false);
            break;
        case I_LDMXCSR:
            DecodeMxcsr(insn, true);
            break;
        case I_FXSAVE:
        case I_FXSAVE64:
            DecodeFxsave(insn, false);
            break;
        case I_FXRSTOR:
        case I_FXRSTOR64:
            DecodeFxsave(insn, true);
            break;
        case I_UCOMISD:
        case I_COMISD:
            DecodeUcomisd(insn);
            break;
        case I_UCOMISS:
        case I_COMISS:
            DecodeUcomis(insn, 32);
            break;
        case I_BSF:
            DecodeBitScan(insn, false);
            break;
        case I_TZCNT:
            // TZCNT and BSF differ in their zero-source behaviour and in CF/ZF,
            // so they cannot share a handler once BMI1 is advertised. With the
            // gate off DecodeTzcnt reproduces the old aliasing: tzcnt executes
            // as bsf on a CPU that hides BMI1.
            DecodeTzcnt(insn);
            break;
        case I_BSR:
            DecodeBitScan(insn, true);
            break;
        case I_CMPXCHG:
            DecodeCmpxchg(insn);
            break;
        case I_ROL:
            DecodeRotate(insn, true);
            break;
        case I_ROR:
            DecodeRotate(insn, false);
            break;
        case I_BT:
            DecodeBt(insn, 0);
            break;
        case I_BTS:
            DecodeBt(insn, 1);
            break;
        case I_BTR:
            DecodeBt(insn, 2);
            break;
        case I_BTC:
            DecodeBt(insn, 3);
            break;
        case I_PAUSE:
        case I_PREFETCHT0:
        case I_PREFETCHT1:
        case I_PREFETCHT2:
        case I_PREFETCHNTA:
        case I_PREFETCH:
        case I_PREFETCHW:
        case I_LFENCE:
        case I_MFENCE:
        case I_SFENCE:
        case I_EMMS:
        case I_CLFLUSH:
            // Timing / ordering hints only: no observable state in this
            // single-threaded model.
            __ Nop();
            break;
        // ---- AVX: VEX-encoded forms ----
        // One entry point so the SVM_AVX gate and the VEX.L / operand-shape
        // checks are written once; see DecodeAvx.
        case I_VMOVDQA:
        case I_VMOVDQU:
        case I_VMOVAPS:
        case I_VMOVUPS:
        case I_VMOVAPD:
        case I_VMOVUPD:
        case I_VMOVNTDQ:
        case I_VMOVNTDQA:
        case I_VMOVNTPS:
        case I_VMOVNTPD:
        case I_VLDDQU:
        case I_VMOVD:
        case I_VMOVQ:
        case I_VPXOR:
        case I_VPOR:
        case I_VPAND:
        case I_VPANDN:
        case I_VPADDB:
        case I_VPADDW:
        case I_VPADDD:
        case I_VPADDQ:
        case I_VPSUBB:
        case I_VPSUBW:
        case I_VPSUBD:
        case I_VPSUBQ:
        case I_VPCMPEQB:
        case I_VPCMPEQW:
        case I_VPCMPEQD:
        case I_VPCMPGTB:
        case I_VPCMPGTW:
        case I_VPCMPGTD:
        // Routed for their VEX.256 handlers (decoder_avx.cc). The VEX.128
        // forms have no handler yet; DecodeAvx's L=0 path declines them, which
        // traps the block as FALLBACK — the same outcome as before they were
        // listed here, never a mis-execution.
        case I_VXORPS:
        case I_VXORPD:
        case I_VORPS:
        case I_VORPD:
        case I_VANDPS:
        case I_VANDPD:
        case I_VANDNPS:
        case I_VANDNPD:
        case I_VPMINUB:
        case I_VPMINUD:
        case I_VPMAXUB:
        case I_VPMAXUD:
        case I_VPMOVMSKB:
        case I_VPSHUFB:
        case I_VBROADCASTSS:
        case I_VZEROUPPER:
        case I_VZEROALL:
            return DecodeAvx(insn);
        default:
            // Legacy SSE3 / SSSE3 / SSE4.1 / SSE4.2 (decoder_sse4.cc). Routed
            // from `default` rather than from sixty `case` labels: an opcode
            // any case above claims never reaches here, and this arm therefore
            // cannot collide with a case being added elsewhere in the switch.
            // DecodeSse4 returns false for anything it does not claim, which
            // preserves the previous FALLBACK behaviour exactly.
            // The SSE4.2 string family (decoder_sse42str.cc) rides the same
            // arm for the same reason.
            return DecodeSse4(insn);
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
    ir::Cond arm;
    bool inv = false;
    switch (cond) {
        case Cond::EQ:
            arm = ir::Cond::EQ;
            break;
        case Cond::NE:
            arm = ir::Cond::NE;
            break;
        case Cond::MI:
        case Cond::SN:
            arm = ir::Cond::MI;
            break;
        case Cond::PL:
        case Cond::NS:
            arm = ir::Cond::PL;
            break;
        case Cond::VS:
            arm = ir::Cond::VS;
            break;
        case Cond::VC:
            arm = ir::Cond::VC;
            break;
        case Cond::GE:
            arm = ir::Cond::GE;
            break;
        case Cond::LT:
            arm = ir::Cond::LT;
            break;
        case Cond::GT:
            arm = ir::Cond::GT;
            break;
        case Cond::LE:
            arm = ir::Cond::LE;
            break;
            // CF == 1 / CF == 0: value-based, honoring the polarity byte, so the
            // result is exact even when the carry was produced in another block.
        case Cond::CS:
        case Cond::BT:
            return __ TestNotZero(CarryValue());
        case Cond::CC:
        case Cond::AE:
            return __ TestZero(CarryValue());
        // JA: CF == 0 && ZF == 0
        case Cond::HI:
        case Cond::AT:
            // Not expressible as a single ARM condition under Direct carry
            // polarity (x86 A = !CF && !ZF while the stored C equals CF), so
            // compose it from polarity-aware pieces.
            return __ And(CheckCond(Cond::CC), ir::Operand{CheckCond(Cond::NE)});
        // JBE: CF == 1 || ZF == 1
        case Cond::LS:
        case Cond::BE:
            return __ Or(CheckCond(Cond::CS), ir::Operand{CheckCond(Cond::EQ)});
        default:
            PANIC();
    }
    auto one = __ LoadImm(ir::Imm(u8(1)));
    auto zero = __ LoadImm(ir::Imm(u8(0)));
    return inv ? __ CondSelect(arm, zero, one) : __ CondSelect(arm, one, zero);
}

ir::Value X64Decoder::CarryValue() {
    auto raw = __ TestFlags(ir::Flags::Carry).SetType(ir::ValueType::U8);
    switch (carry_) {
        case CarryPolarity::Inverted:
            return __ Xor(raw, ir::Operand{ir::Imm(u64(1))});
        case CarryPolarity::Direct:
            return raw;
        default:
            // Unknown (block entry): recover the architectural CF through the
            // runtime polarity byte (ThreadContext64::carry_inverted).
            return __ Xor(raw, ir::Operand{__ LoadUniform(PolarityUniform())});
    }
}

void X64Decoder::StorePolarity(bool inverted) {
    // StoreUniform uses the value's width, not the Uniform declaration's
    // width. Keep this byte-typed so it cannot overwrite the adjacent DF byte.
    __ StoreUniform(PolarityUniform(), __ LoadImm(ir::Imm(u8(inverted ? 1 : 0))));
}

ir::Value X64Decoder::DirectionValue() {
    return __ LoadUniform(DirectionUniform()).SetType(ir::ValueType::U8);
}

void X64Decoder::StoreDirection(bool backward) {
    __ StoreUniform(DirectionUniform(), __ LoadImm(ir::Imm(u8(backward ? 1 : 0))));
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

ir::DataClass X64Decoder::Src(_DInst& insn, _Operand& op, bool force_tso) {
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
            const bool tso = force_tso || TsoOrdered(insn);
            if (tso) {
                // Preserve a constant/RIP-relative address through the TSO IR.
                // The backend can then prove natural alignment at compile time
                // and omit its dynamic LDAPR/STLR alignment branch.
                result = MemLoad(address_operand.ToIROperand(), size, true);
            } else {
                auto address =
                        __ GetOperand(address_operand.ToIROperand())
                                .SetType(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32);
                result = MemLoad(ir::Operand{address}, size, false);
            }
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

void X64Decoder::Dst(_DInst& insn, _Operand& operand, const ir::DataClass& data, bool force_tso) {
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
            const bool tso = force_tso || TsoOrdered(insn);
            if (tso) {
                // Keep constant addresses visible for the aligned TSO fast
                // path; register-derived addresses are checked dynamically.
                MemStore(address.ToIROperand(), value, true);
            } else {
                auto folded =
                        __ GetOperand(address.ToIROperand())
                                .SetType(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32);
                MemStore(ir::Operand{folded}, value, false);
            }
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
                    address_operand.left = __ Add(seg_base, ir::Operand{address_operand.left});
                }
            }

            if (insn.dispSize) {
                s64 disp = ForceCast<s64>(insn.disp);
                if (address_operand.right.Null() && !address_operand.ext) {
                    if (address_operand.left.IsImm()) {
                        address_operand.left =
                                ir::Imm((address_operand.left.imm.Get() + disp) & addr_mask);
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
                address_operand.left =
                        SegmentBase(static_cast<_RegisterType>(SEGMENT_GET(insn.segment)));
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
                } else if (!address_operand.left.IsImm()) {
                    // Scaled index with NO base (mod=00, SIB.base=101; the
                    // address sits in the disp32). With `right` empty the
                    // scale can only travel in `ext`, and ToIROperand then
                    // emits `left << ext` — but the displacement fold below
                    // adds the disp INTO `left`, yielding
                    //     (index + disp) << scale
                    // instead of the architectural
                    //     (index << scale) + disp.
                    // Materialise the shift here so the disp fold sees a
                    // plain value and takes the `right = Imm(disp)` branch.
                    // X64Decoder::VexAddress already does it in this order,
                    // which is why the VEX path was correct and this one was
                    // not — clang emits this form for any static array with a
                    // 2/4/8-byte element under -fno-pic, so the blast radius
                    // was every SSE and scalar access of that shape.
                    address_operand.left =
                            __ LslImm(ToValue(address_operand.left),
                                      ir::Imm(u64(address_operand.ext)));
                    address_operand.ext = 0;
                }
            }
            if (insn.dispSize) {
                s64 disp = ForceCast<s64>(insn.disp);
                if (address_operand.left.Null()) {
                    address_operand.left = ir::Imm(disp & addr_mask);
                } else if (address_operand.right.Null() && !address_operand.ext) {
                    if (address_operand.left.IsImm()) {
                        // Fold constant bases (e.g. RIP relative).
                        address_operand.left =
                                ir::Imm((address_operand.left.imm.Get() + disp) & addr_mask);
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
                auto seg_base = SegmentBase(static_cast<_RegisterType>(SEGMENT_GET(insn.segment)));
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

void X64Decoder::DecodeLea(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];

    auto address = GetAddress(insn, op1);
    // SetType (mutation): EmitGetOperand sizes the result from the
    // instruction's own return type, untyped would truncate to 32 bits.
    Dst(insn,
        op0,
        __ GetOperand(address.ToIROperand())
                .SetType(is_64bit ? ir::ValueType::U64 : ir::ValueType::U32));
}

// ---------------------------------------------------------------------------
// SSE decode implementations
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// AVX / VEX infrastructure
// ---------------------------------------------------------------------------

bool X64Decoder::AvxEnabled() {
    // Read once: DecodeSwitch consults this per instruction and getenv is not
    // required to be cheap (or thread-safe against setenv).
    static const bool enabled = [] {
        const char* env = std::getenv("SVM_AVX");
        return env && std::strcmp(env, "0") != 0;
    }();
    return enabled;
}

X64Decoder::VexInfo X64Decoder::DecodeVex() const {
    VexInfo vex{};
    if (!insn_bytes) {
        return vex;
    }
    // A VEX prefix must immediately precede the opcode; only segment and
    // address-size overrides may come before it (66/F2/F3/REX before VEX is
    // #UD, and distorm would not have produced a V* mnemonic in that case).
    u32 i = 0;
    while (i < 4) {
        const u8 b = insn_bytes[i];
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65 ||
            b == 0x67) {
            ++i;
            continue;
        }
        break;
    }
    if (insn_bytes[i] == 0xC5) {
        // 2-byte VEX: [R vvvv L pp]. Implies mmmmm=1 (0F map) and W=0.
        const u8 b1 = insn_bytes[i + 1];
        const u8 raw_vvvv = static_cast<u8>((b1 >> 3) & 0xF);
        vex.valid = true;
        vex.vvvv = static_cast<u8>(~raw_vvvv & 0xF);
        vex.vvvv_unused = raw_vvvv == 0xF;
        vex.l = (b1 & 0x04) != 0;
        vex.pp = static_cast<u8>(b1 & 0x03);
        vex.mmmmm = 1;
        vex.w = false;
    } else if (insn_bytes[i] == 0xC4) {
        // 3-byte VEX: [R X B mmmmm][W vvvv L pp].
        const u8 b1 = insn_bytes[i + 1];
        const u8 b2 = insn_bytes[i + 2];
        const u8 raw_vvvv = static_cast<u8>((b2 >> 3) & 0xF);
        vex.valid = true;
        vex.mmmmm = static_cast<u8>(b1 & 0x1F);
        vex.w = (b2 & 0x80) != 0;
        vex.vvvv = static_cast<u8>(~raw_vvvv & 0xF);
        vex.vvvv_unused = raw_vvvv == 0xF;
        vex.l = (b2 & 0x04) != 0;
        vex.pp = static_cast<u8>(b2 & 0x03);
    }
    return vex;
}

bool X64Decoder::IsVex128(const VexInfo& vex) const { return vex.valid && !vex.l; }

u32 X64Decoder::VexRmRegister() const {
    if (!insn_bytes) {
        return UINT32_MAX;
    }
    // Same prefix scan as DecodeVex: only segment / address-size overrides may
    // precede a VEX prefix.
    u32 i = 0;
    while (i < 4) {
        const u8 b = insn_bytes[i];
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65 ||
            b == 0x67) {
            ++i;
            continue;
        }
        break;
    }
    u32 modrm_at;
    u8 b_bit;
    if (insn_bytes[i] == 0xC5) {
        // 2-byte VEX implies B=0 (it has no B field), then opcode, then ModRM.
        b_bit = 0;
        modrm_at = i + 3;
    } else if (insn_bytes[i] == 0xC4) {
        // 3-byte VEX stores B inverted in byte1 bit 5.
        b_bit = static_cast<u8>((insn_bytes[i + 1] & 0x20) ? 0 : 1);
        modrm_at = i + 4;
    } else {
        return UINT32_MAX;
    }
    const u8 modrm = insn_bytes[modrm_at];
    return static_cast<u32>((modrm & 0x07) | (b_bit << 3));
}

u32 X64Decoder::VecIndex(_RegisterType reg) {
    if (reg >= R_YMM0 && reg <= R_YMM15) {
        return static_cast<u32>(reg - R_YMM0);
    }
    ASSERT(reg >= R_XMM0 && reg <= R_XMM15);
    return static_cast<u32>(reg - R_XMM0);
}

_RegisterType X64Decoder::XmmOf(u32 index) {
    ASSERT(index < 16);
    return static_cast<_RegisterType>(R_XMM0 + index);
}

ir::Uniform X64Decoder::YmmHighUniform(u32 index) {
    ASSERT(index < 16);
    return ir::Uniform{static_cast<u32>(offsetof(ThreadContext64, ymm_high) + index * sizeof(Xmm)),
                       ir::ValueType::V128};
}

ir::Value X64Decoder::YmmHighRead(u32 index) { return __ LoadUniform(YmmHighUniform(index)); }

void X64Decoder::YmmHighWrite(u32 index, ir::Value value) {
    __ StoreUniform(YmmHighUniform(index), value.SetType(ir::ValueType::V128));
}

ir::Value X64Decoder::YmmHighLo(u32 index) {
    return __ LoadUniform(ir::Uniform{YmmHighUniform(index).GetOffset(), ir::ValueType::U64});
}

ir::Value X64Decoder::YmmHighHi(u32 index) {
    return __ LoadUniform(ir::Uniform{YmmHighUniform(index).GetOffset() + 8, ir::ValueType::U64});
}

void X64Decoder::YmmHighLo(u32 index, ir::Value value) {
    // NarrowTo normalizes untyped (CallLambda) values so the store has a width,
    // mirroring XmmLo/XmmHi.
    __ StoreUniform(ir::Uniform{YmmHighUniform(index).GetOffset(), ir::ValueType::U64},
                    NarrowTo(value, ir::ValueType::U64));
}

void X64Decoder::YmmHighHi(u32 index, ir::Value value) {
    __ StoreUniform(ir::Uniform{YmmHighUniform(index).GetOffset() + 8, ir::ValueType::U64},
                    NarrowTo(value, ir::ValueType::U64));
}

void X64Decoder::ZeroYmmHigh(u32 index) {
    // Two U64 zero stores rather than one V128: the vector IR has no
    // "materialize zero vector" op, and LoadImm produces a scalar.
    auto zero = __ LoadImm(ir::Imm(u64(0)));
    __ StoreUniform(ir::Uniform{YmmHighUniform(index).GetOffset(), ir::ValueType::U64}, zero);
    __ StoreUniform(ir::Uniform{YmmHighUniform(index).GetOffset() + 8, ir::ValueType::U64}, zero);
}

bool X64Decoder::DecodeAvx(_DInst& insn) {
    // Returning false traps the block as FALLBACK. That is the correct answer
    // for every shape we do not model: a VEX instruction executed with the
    // wrong width or operand order silently produces wrong data, which is far
    // worse than refusing to translate the block.
    if (!AvxEnabled()) {
        return false;
    }
    // VZEROUPPER/VZEROALL take no operands and are distinguished by mnemonic
    // (VZEROALL is the L=1 encoding), so they need neither check below.
    if (insn.opcode == I_VZEROUPPER || insn.opcode == I_VZEROALL) {
        DecodeVzero(insn.opcode == I_VZEROALL);
        return true;
    }
    const auto vex = DecodeVex();
    // L is read from the raw prefix, never from distorm's operand sizes: for
    // the AVX2 packed-integer opcodes this distorm snapshot has no 256-bit
    // table entry and reports 128-bit XMM operands for an L=1 encoding without
    // any error. Gating on the real L bit is what keeps that misdecode from
    // reaching a handler. L=1 belongs to the 256-bit handlers.
    if (!IsVex128(vex)) {
        // DecodeAvx256 re-checks vex.valid/vex.l, so a non-VEX encoding still
        // declines here and the block traps as FALLBACK.
        return DecodeAvx256(insn, vex);
    }
    // Normalize any YMM operand code to its XMM twin. distorm already reports
    // XMM for the L=0 encodings we accept, but a YMM code reaching one of the
    // SSE helpers below would silently read/write ymm_high instead of xmms —
    // the wrong 128-bit half, with no diagnostic. One cheap guard here covers
    // every handler.
    for (auto& op : insn.ops) {
        if (op.type == O_REG && op.index >= R_YMM0 && op.index <= R_YMM15) {
            op.index = static_cast<u8>(XmmOf(VecIndex(static_cast<_RegisterType>(op.index))));
        }
    }
    switch (insn.opcode) {
        case I_VMOVDQA:
        case I_VMOVDQU:
        case I_VMOVAPS:
        case I_VMOVUPS:
        case I_VMOVAPD:
        case I_VMOVUPD:
        // Non-temporal hints carry no extra semantics in this model, so they
        // degrade to plain moves exactly as the SSE dispatch does.
        case I_VMOVNTDQ:
        case I_VMOVNTDQA:
        case I_VMOVNTPS:
        case I_VMOVNTPD:
        case I_VLDDQU:
            DecodeVexMovVec(insn);
            return true;
        case I_VMOVD:
            DecodeVexMovd(insn);
            return true;
        case I_VMOVQ:
            DecodeVexMovq(insn);
            return true;
        default:
            break;
    }
    // Everything remaining is a 3-operand non-destructive form. Verify the
    // operand shape distorm produced rather than trusting it: a 2-operand
    // result here would make ops[1] the r/m operand and ops[2] garbage.
    if (insn.ops[0].type != O_REG || insn.ops[1].type != O_REG ||
        insn.ops[2].type == O_NONE) {
        return false;
    }
    switch (insn.opcode) {
        case I_VPXOR:
            DecodeVexBitwise(insn, VecBitwiseOp::Xor);
            return true;
        case I_VPOR:
            DecodeVexBitwise(insn, VecBitwiseOp::Or);
            return true;
        case I_VPAND:
            DecodeVexBitwise(insn, VecBitwiseOp::And);
            return true;
        case I_VPANDN:
            DecodeVexBitwise(insn, VecBitwiseOp::AndNot);
            return true;
        case I_VPADDB:
            DecodeVexInt(insn, VecIntOp::Add, 8);
            return true;
        case I_VPADDW:
            DecodeVexInt(insn, VecIntOp::Add, 16);
            return true;
        case I_VPADDD:
            DecodeVexInt(insn, VecIntOp::Add, 32);
            return true;
        case I_VPADDQ:
            DecodeVexInt(insn, VecIntOp::Add, 64);
            return true;
        case I_VPSUBB:
            DecodeVexInt(insn, VecIntOp::Sub, 8);
            return true;
        case I_VPSUBW:
            DecodeVexInt(insn, VecIntOp::Sub, 16);
            return true;
        case I_VPSUBD:
            DecodeVexInt(insn, VecIntOp::Sub, 32);
            return true;
        case I_VPSUBQ:
            DecodeVexInt(insn, VecIntOp::Sub, 64);
            return true;
        case I_VPCMPEQB:
            DecodeVexInt(insn, VecIntOp::CmpEq, 8);
            return true;
        case I_VPCMPEQW:
            DecodeVexInt(insn, VecIntOp::CmpEq, 16);
            return true;
        case I_VPCMPEQD:
            DecodeVexInt(insn, VecIntOp::CmpEq, 32);
            return true;
        case I_VPCMPGTB:
            DecodeVexInt(insn, VecIntOp::CmpGt, 8);
            return true;
        case I_VPCMPGTW:
            DecodeVexInt(insn, VecIntOp::CmpGt, 16);
            return true;
        case I_VPCMPGTD:
            DecodeVexInt(insn, VecIntOp::CmpGt, 32);
            return true;
        default:
            return false;
    }
}

_RegisterType X64Decoder::VexSrc1(const _DInst& insn) {
    // Non-destructive source: `vpxor dst, src1, src2` keeps dst intact, so
    // handlers must read ops[1] where the SSE version reads ops[0].
    ASSERT(insn.ops[1].type == O_REG);
    return static_cast<_RegisterType>(insn.ops[1].index);
}

void X64Decoder::DecodeVexBitwise(_DInst& insn, VecBitwiseOp op) {
    const auto dst = VecIndex(static_cast<_RegisterType>(insn.ops[0].index));
    auto left = XmmRead(VexSrc1(insn));
    auto right = LoadSrcVec(insn, insn.ops[2]);
    ir::Value result;
    switch (op) {
        case VecBitwiseOp::Xor:
            result = __ VecXor(left, right);
            break;
        case VecBitwiseOp::Or:
            result = __ VecOr(left, right);
            break;
        case VecBitwiseOp::And:
            result = __ VecAnd(left, right);
            break;
        case VecBitwiseOp::AndNot:
            // VPANDN: dst = (NOT src1) AND src2; VecAndNot(x, y) = x AND NOT y.
            result = __ VecAndNot(right, left);
            break;
    }
    XmmWrite(XmmOf(dst), result.SetType(ir::ValueType::V128));
    ZeroYmmHigh(dst);
}

void X64Decoder::DecodeVexInt(_DInst& insn, VecIntOp op, u32 lane_bits) {
    const auto dst = VecIndex(static_cast<_RegisterType>(insn.ops[0].index));
    auto left = XmmRead(VexSrc1(insn));
    auto right = LoadSrcVec(insn, insn.ops[2]);
    ir::Value result;
    switch (op) {
        case VecIntOp::Add:
            result = __ VecAdd(left, right, ir::Imm(lane_bits));
            break;
        case VecIntOp::Sub:
            result = __ VecSub(left, right, ir::Imm(lane_bits));
            break;
        case VecIntOp::CmpEq:
            result = __ VecCmpEq(left, right, ir::Imm(lane_bits));
            break;
        case VecIntOp::CmpGt:
            result = __ VecCmpGt(left, right, ir::Imm(lane_bits));
            break;
    }
    XmmWrite(XmmOf(dst), result.SetType(ir::ValueType::V128));
    ZeroYmmHigh(dst);
}

void X64Decoder::DecodeVexMovVec(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    if (op0.type == O_REG) {
        // Load form: xmm, xmm/m128.
        const auto dst = VecIndex(static_cast<_RegisterType>(op0.index));
        ir::Value v;
        if (op1.type == O_REG) {
            v = XmmRead(XmmOf(VecIndex(static_cast<_RegisterType>(op1.index))));
        } else {
            v = __ LoadMemory(ir::Operand{FlatAddress(insn, op1)}).SetType(ir::ValueType::V128);
        }
        XmmWrite(XmmOf(dst), v);
        ZeroYmmHigh(dst);
    } else {
        // Store form: m128, xmm. No destination register, so no upper half to
        // clear (vmovntdq degrades to a plain store, as the SSE path does).
        auto v = XmmRead(XmmOf(VecIndex(static_cast<_RegisterType>(op1.index))));
        __ StoreMemory(ir::Operand{FlatAddress(insn, op0)}, v);
    }
}

void X64Decoder::DecodeVexMovd(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    // VEX.W forms (vmovq via VEX.128.66.0F.W1 6E) are 64-bit: same path as
    // vmovq, exactly as DecodeMovd defers to DecodeMovq.
    if (op0.size == 64 || op1.size == 64) {
        DecodeVexMovq(insn);
        return;
    }
    if (op0.type == O_REG && IsV(static_cast<_RegisterType>(op0.index))) {
        // vmovd xmm, r/m32: low dword = src, bits 255:32 all zeroed.
        const auto dst = VecIndex(static_cast<_RegisterType>(op0.index));
        auto src = ToValue(Src(insn, op1));
        XmmLo(XmmOf(dst), __ ZeroExtend64(src));
        XmmHi(XmmOf(dst), __ LoadImm(ir::Imm(u64(0))));
        ZeroYmmHigh(dst);
    } else {
        // vmovd r/m32, xmm: no vector destination.
        Dst(insn, op0, XmmLo(XmmOf(VecIndex(static_cast<_RegisterType>(op1.index)))));
    }
}

void X64Decoder::DecodeVexMovq(_DInst& insn) {
    auto& op0 = insn.ops[0];
    auto& op1 = insn.ops[1];
    if (op0.type == O_REG && IsV(static_cast<_RegisterType>(op0.index))) {
        // vmovq xmm, xmm/r64/m64: low qword = src, bits 255:64 zeroed.
        const auto dst = VecIndex(static_cast<_RegisterType>(op0.index));
        ir::Value v;
        if (op1.type == O_REG && IsV(static_cast<_RegisterType>(op1.index))) {
            v = XmmLo(XmmOf(VecIndex(static_cast<_RegisterType>(op1.index))));
        } else {
            v = ToValue(Src(insn, op1));
        }
        XmmLo(XmmOf(dst), v);
        XmmHi(XmmOf(dst), __ LoadImm(ir::Imm(u64(0))));
        ZeroYmmHigh(dst);
    } else {
        Dst(insn, op0, XmmLo(XmmOf(VecIndex(static_cast<_RegisterType>(op1.index)))));
    }
}

void X64Decoder::DecodeVzero(bool all) {
    // Outside 64-bit mode only YMM0-7 exist; touching YMM8-15 there would
    // corrupt state a 32-bit guest can still observe through a later mode
    // switch, so follow the architectural register count.
    const u32 count = is_64bit ? 16 : 8;
    auto zero = __ LoadImm(ir::Imm(u64(0)));
    for (u32 i = 0; i < count; ++i) {
        if (all) {
            // VZEROALL clears the FULL registers, low half included.
            const auto low = ToVReg(x86_regs_table[XmmOf(i)]).GetOffset();
            __ StoreUniform(ir::Uniform{low, ir::ValueType::U64}, zero);
            __ StoreUniform(ir::Uniform{low + 8, ir::ValueType::U64}, zero);
        }
        ZeroYmmHigh(i);
    }
}

}  // namespace swift::x86
