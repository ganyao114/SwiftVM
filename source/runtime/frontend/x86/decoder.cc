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
#include "runtime/frontend/x86/distorm_fast.h"
#include "runtime/frontend/x86/xsave.h"

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

static void FixupFsgsbaseOperand(_DInst& insn, const u8* code) {
    if (insn.opcode != I_RDFSBASE && insn.opcode != I_RDGSBASE &&
        insn.opcode != I_WRFSBASE && insn.opcode != I_WRGSBASE) {
        return;
    }
    // This snapshot has the right mnemonic/length table entries, but its
    // shared operand descriptor selects ModRM.reg (the /0../3 opcode selector)
    // instead of ModRM.r/m and always assigns a 64-bit GPR. Recover the actual
    // operand and REX.W width from the bytes before normal dispatch.
    u8 rex = 0;
    for (u32 i = 0; i + 2 < insn.size; ++i) {
        if ((code[i] & 0xF0) == 0x40) {
            rex = code[i];
        }
        if (code[i] != 0x0F || code[i + 1] != 0xAE) {
            continue;
        }
        const u8 modrm = code[i + 2];
        const u32 index = (modrm & 7) | ((rex & 1) ? 8 : 0);
        const bool wide = (rex & 8) != 0;
        insn.ops[0].type = O_REG;
        insn.ops[0].index = static_cast<u8>((wide ? R_RAX : R_EAX) + index);
        insn.ops[0].size = wide ? 64 : 32;
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

bool X64Decoder::VexTsoOrdered(const VexInsn& insn) const {
    // VEX encodings cannot carry LOCK, so only the AcqRel mode gate applies.
    if (GetTsoMode() != runtime::TsoMode::AcqRel) {
        return false;
    }

    // Mirror IsThreadPrivateAddress's stack/base-index relaxation. VexInsn
    // keeps architectural register numbers rather than distorm register
    // enums, where 4/5 are RSP/RBP (and ESP/EBP in 32-bit addressing).
    // VexInsn does not retain the accepted segment override, so an FS/GS VEX
    // access cannot be proven TLS-private here and remains conservatively
    // ordered.
    if (!insn.RmIsRegister()) {
        const auto stack_reg = [](u8 reg) { return reg == 4 || reg == 5; };
        if ((!insn.base_none && stack_reg(insn.base)) ||
            (!insn.index_none && stack_reg(insn.index))) {
            return false;
        }
    }
    return true;
}

ir::Value X64Decoder::MemLoad(const ir::Operand& addr, ir::ValueType type, bool tso) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::Memory};
    if (tso) {
        return assembler->LoadMemoryTSO(addr).SetType(type);
    }
    return assembler->LoadMemory(addr).SetType(type);
}

bool X64Decoder::ScalarVOperandsEnabled() {
    return swift::runtime::GetSvmConfig().sse_scalar_v_operands;
}

void X64Decoder::MemStore(const ir::Operand& addr, ir::Value value, bool tso) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::Memory};
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
                       bool is_64bit,
                       runtime::Arm64Features arm64_features,
                       bool sse_afp_nan,
                       bool identity_addressing)
        : start(start), pc(start), assembler(visitor), memory(memory), is_64bit(is_64bit),
          identity_addressing_(identity_addressing) {
    addr_mask = is_64bit ? UINT64_MAX : UINT32_MAX;
    flags_cfinv_supported_ =
            True(arm64_features & runtime::Arm64Features::FlagM);
    flags_fcmp_compact_ = runtime::GetSvmConfig().flags_fcmp_compact &&
                          True(arm64_features & runtime::Arm64Features::AXFlag);
    sse_afp_nan_ = sse_afp_nan;
    addr_ea_tie_ = runtime::GetSvmConfig().addr_ea_tie;
}

// x86-64's architectural maximum instruction length. Bounds every raw-byte
// read on the VEX path so a decode at the very end of a mapped page cannot run
// past it.
static constexpr size_t kMaxInsnBytes = 15;

// Bytes of guest code the decode loop is allowed to look at per instruction.
// DisDecode is handed 0x10 unconditionally, so that -- not kMaxInsnBytes -- is
// the window that must be proven readable.
static constexpr size_t kFetchWindow = 0x10;

#if defined(__GNUC__) || defined(__clang__)
#define SVM_DECODE_STAGE __attribute__((always_inline))
#else
#define SVM_DECODE_STAGE
#endif

class X64Decoder::DecodePipeline {
public:
    explicit DecodePipeline(X64Decoder& decoder) : decoder(decoder) {}

    void Run() {
        decoder.pc = decoder.start;
        swift::runtime::PerfDecodeRunEmptyMicrobench();
        swift::runtime::PerfLoweringRunEmptyMicrobench();
        decode_prof = swift::runtime::PerfDecodeDetailEnabled();
        fast_enabled = DistormFastEnabled();
        verify_fast = DistormFastVerifyEnabled();

        while (!decoder.end_decode) {
            swift::runtime::PerfDecodeScope2 perf_instruction{
                    swift::runtime::GetPerfStats2().decode_instruction_total};
            RecordDecodeAttempt();
            if (!FetchInstruction()) {
                break;
            }

            switch (PredispatchInstruction()) {
                case StepResult::Handled:
                    continue;
                case StepResult::Stop:
                    return;
                case StepResult::NotHandled:
                    break;
            }

            if (!DecodeDistormInstruction()) {
                break;
            }
        }
    }

private:
    enum class StepResult {
        NotHandled,
        Handled,
        Stop,
    };

    SVM_DECODE_STAGE void RecordDecodeAttempt() {
        if (!decode_prof) {
            return;
        }

        auto& stats = swift::runtime::GetPerfStats2();
        stats.decode_attempts.fetch_add(1, std::memory_order_relaxed);
        constexpr u64 kMappingPageShift = 14;
        const u64 page0 = decoder.pc >> kMappingPageShift;
        const u64 page1 = (decoder.pc + kFetchWindow - 1) >> kMappingPageShift;
        unsigned new_pages = 0;
        if (page0 != prior_fetch_page0 && page0 != prior_fetch_page1) {
            ++new_pages;
        }
        if (page1 != page0 && page1 != prior_fetch_page0 && page1 != prior_fetch_page1) {
            ++new_pages;
        }
        stats.decode_page_validation_calls.fetch_add(new_pages, std::memory_order_relaxed);
        prior_fetch_page0 = page0;
        prior_fetch_page1 = page1;
    }

    SVM_DECODE_STAGE bool FetchInstruction() {
        // Instruction fetch is the one guest access made from *host* code, so
        // it is the one that must never fault: runtime.cpp's HandleFault only
        // recovers faults whose host pc lies inside a JIT buffer. Validate both
        // ends of the 16-byte decoder window before any consumer reads it.
        swift::runtime::PerfDecodeScope2 perf_fetch{
                swift::runtime::GetPerfStats2().decode_fetch};
        if (decode_prof) {
            swift::runtime::GetPerfStats2().decode_fetch_getpointer_calls.fetch_add(
                    1, std::memory_order_relaxed);
        }
        code_ptr = reinterpret_cast<u8*>(
                decoder.memory->GetPointer(reinterpret_cast<void*>(decoder.pc)));
        if (!code_ptr) {
            decoder.Interrupt(InterruptReason::PAGE_FATAL);
            return false;
        }

        fetch_avail = kFetchWindow;
        if (decode_prof) {
            swift::runtime::GetPerfStats2().decode_fetch_getpointer_calls.fetch_add(
                    1, std::memory_order_relaxed);
        }
        if (decoder.memory->GetPointer(
                    reinterpret_cast<void*>(decoder.pc + kFetchWindow - 1))) {
            return true;
        }

        // A short window is copied byte-by-byte into zero padding. Every raw
        // recognizer below requires a nonzero final opcode byte, and all paths
        // reject decoded lengths beyond fetch_avail, so padding cannot create
        // an instruction across an unmapped page.
        if (decode_prof) {
            swift::runtime::GetPerfStats2().decode_fetch_short_windows.fetch_add(
                    1, std::memory_order_relaxed);
        }
        fetch_buffer.fill(0);
        fetch_avail = 0;
        for (size_t i = 0; i < kFetchWindow; ++i) {
            if (decode_prof) {
                auto& stats = swift::runtime::GetPerfStats2();
                stats.decode_fetch_getpointer_calls.fetch_add(1, std::memory_order_relaxed);
                stats.decode_fetch_bounce_calls.fetch_add(1, std::memory_order_relaxed);
            }
            const auto* byte = reinterpret_cast<const u8*>(
                    decoder.memory->GetPointer(reinterpret_cast<void*>(decoder.pc + i)));
            if (!byte) {
                break;
            }
            fetch_buffer[i] = *byte;
            ++fetch_avail;
        }
        code_ptr = fetch_buffer.data();
        return true;
    }

    SVM_DECODE_STAGE StepResult PredispatchInstruction() {
        swift::runtime::PerfDecodeScope2 perf_predispatch{
                swift::runtime::GetPerfStats2().decode_predispatch_inclusive};

        StepResult result = DecodeCetInstruction();
        if (result != StepResult::NotHandled) {
            return result;
        }
        result = DecodeRawExtension();
        if (result != StepResult::NotHandled) {
            return result;
        }
        result = DecodeVexInstruction();
        if (result != StepResult::NotHandled) {
            return result;
        }
        result = DecodeX87FreePopInstruction();
        if (result != StepResult::NotHandled) {
            return result;
        }
        result = DecodeXsavecInstruction();
        if (result != StepResult::NotHandled) {
            return result;
        }
        return DecodeRdseedInstruction();
    }

    SVM_DECODE_STAGE StepResult DecodeCetInstruction() {
        // CET endbr64 / endbr32 (F3 0F 1E FA/FB): distorm does not know them.
        if (code_ptr[0] == 0xF3 && code_ptr[1] == 0x0F && code_ptr[2] == 0x1E &&
            (code_ptr[3] == 0xFA || code_ptr[3] == 0xFB)) {
            {
                swift::runtime::PerfDecodeScope2 perf_raw{
                        swift::runtime::GetPerfStats2().decode_raw,
                        swift::runtime::PerfDecodePath2::Raw};
                decoder.assembler->Nop();
                decoder.pc += 4;
                decoder.assembler->AdvancePC(ir::Imm{4});
                decoder.end_decode = decoder.assembler->EndCommit();
            }
            RecordRawAccepted();
            return StepResult::Handled;
        }

        // CET shadow-stack ops: rdssp yields zero because SSP is not modelled;
        // incssp is a no-op. Both are only reachable on CET-enabled hosts.
        if (code_ptr[0] != 0xF3 || (code_ptr[1] & 0xF0) != 0x40 ||
            code_ptr[2] != 0x0F ||
            !((code_ptr[3] == 0x1E && (code_ptr[4] & 0xF8) == 0xC8) ||
              (code_ptr[3] == 0xAE && (code_ptr[4] & 0xF8) == 0xE8))) {
            return StepResult::NotHandled;
        }

        {
            swift::runtime::PerfDecodeScope2 perf_raw{
                    swift::runtime::GetPerfStats2().decode_raw,
                    swift::runtime::PerfDecodePath2::Raw};
            if (code_ptr[3] == 0x1E) {
                const u32 index = (code_ptr[4] & 7) | ((code_ptr[1] & 1) << 3);
                const auto reg = static_cast<_RegisterType>(
                        (code_ptr[1] & 8) ? (R_RAX + index) : (R_EAX + index));
                decoder.R(reg, decoder.assembler->LoadImm(ir::Imm(u64(0))));
            }
            decoder.assembler->Nop();
            decoder.pc += 5;
            decoder.assembler->AdvancePC(ir::Imm{5});
            decoder.end_decode = decoder.assembler->EndCommit();
        }
        RecordRawAccepted();
        return StepResult::Handled;
    }

    SVM_DECODE_STAGE StepResult DecodeRawExtension() {
        // SHA-NI, ADX and PKRU are newer than this distorm snapshot.
        u32 size{};
        {
            swift::runtime::PerfDecodeScope2 perf_raw{
                    swift::runtime::GetPerfStats2().decode_raw,
                    swift::runtime::PerfDecodePath2::Raw};
            size = decoder.DecodeShaRaw(code_ptr, fetch_avail);
            if (size != 0 && size != UINT32_MAX) {
                decoder.assembler->AdvancePC(ir::Imm{size});
                decoder.end_decode = decoder.assembler->EndCommit();
            }
        }
        StepResult result = FinishRawExtension(size);
        if (result != StepResult::NotHandled) {
            return result;
        }

        {
            swift::runtime::PerfDecodeScope2 perf_raw{
                    swift::runtime::GetPerfStats2().decode_raw,
                    swift::runtime::PerfDecodePath2::Raw};
            size = decoder.DecodeUserlandRaw(code_ptr, fetch_avail);
            if (size != 0 && size != UINT32_MAX) {
                decoder.assembler->AdvancePC(ir::Imm{size});
                decoder.end_decode = decoder.assembler->EndCommit();
            }
        }
        return FinishRawExtension(size);
    }

    SVM_DECODE_STAGE StepResult FinishRawExtension(u32 size) {
        if (size == UINT32_MAX) {
            decoder.Interrupt(InterruptReason::PAGE_FATAL);
            return StepResult::Stop;
        }
        if (size == 0) {
            return StepResult::NotHandled;
        }
        RecordRawAccepted();
        return StepResult::Handled;
    }

    SVM_DECODE_STAGE StepResult DecodeVexInstruction() {
        // This distorm snapshot cannot preserve all VEX.L/W/vvvv fields, so
        // AVX/AVX2 and BMI instructions take the raw VEX decoder first.
        const bool avx_on = decoder.AvxEnabled();
        const bool bmi_on = decoder.BmiEnabled();
        if ((!avx_on && !bmi_on) || !HasVexPrefix(code_ptr, kMaxInsnBytes)) {
            return StepResult::NotHandled;
        }

        swift::runtime::PerfDecodeScope2 perf_raw{
                swift::runtime::GetPerfStats2().decode_raw,
                swift::runtime::PerfDecodePath2::Vex};
        VexInsn vex;
        {
            swift::runtime::PerfDecodeScope2 perf_vex_core{
                    swift::runtime::GetPerfStats2().decode_vex_core};
            vex = DecodeVexInsn(code_ptr, kMaxInsnBytes);
        }
        if (vex.valid && vex.length > fetch_avail) {
            decoder.Interrupt(InterruptReason::PAGE_FATAL);
            return StepResult::Stop;
        }
        if (!vex.valid) {
            return StepResult::NotHandled;
        }

        const auto saved_pc = decoder.pc;
        // RIP-relative operands resolve against the end of the instruction.
        decoder.pc += vex.length;
        swift::runtime::PerfDecodeScope2 perf_vex_lowering{
                swift::runtime::GetPerfStats2().decode_lowering,
                swift::runtime::PerfDecodePath2::Vex};
        const unsigned operand_count =
                2u + unsigned(vex.vvvv_valid) + unsigned(vex.has_imm8);
        swift::runtime::PerfLoweringBegin(
                UINT32_MAX, operand_count, !vex.RmIsRegister(), true);
        const bool lowered =
                decoder.DecodeBmi(vex) ||
                (avx_on &&
                 (decoder.DecodeAvxMul(vex) || decoder.DecodeAvxFma(vex) ||
                  decoder.DecodeAvxInt(vex) || decoder.DecodeAvxFp(vex) ||
                  decoder.DecodeAvxHadd(vex) || decoder.DecodeAvxBlend(vex) ||
                  decoder.DecodeAvxGather(vex) || decoder.DecodeAvxMisc(vex) ||
                  decoder.DecodeAvxSse4(vex) || decoder.DecodeSse42StrVex(vex)));
        const auto lowering_ns = perf_vex_lowering.Stop();
        swift::runtime::PerfLoweringFinish(lowering_ns, lowered);
        if (!lowered) {
            decoder.pc = saved_pc;
            return StepResult::NotHandled;
        }

        decoder.assembler->AdvancePC(ir::Imm{vex.length});
        decoder.end_decode = decoder.assembler->EndCommit();
        const auto vex_ns = perf_raw.Stop();
        if (decode_prof) {
            auto& stats = swift::runtime::GetPerfStats2();
            stats.decode_raw_accepted.fetch_add(1, std::memory_order_relaxed);
            stats.decode_vex_accepted.fetch_add(1, std::memory_order_relaxed);
            stats.decode_vex.calls.fetch_add(1, std::memory_order_relaxed);
            stats.decode_vex.ns.fetch_add(vex_ns, std::memory_order_relaxed);
        }
        return StepResult::Handled;
    }

    SVM_DECODE_STAGE StepResult DecodeX87FreePopInstruction() {
        // distorm reports DF C0+i (FFREEP ST(i)) as undefined size one.
        if (code_ptr[0] != 0xDF || (code_ptr[1] & 0xF8) != 0xC0) {
            return StepResult::NotHandled;
        }
        {
            swift::runtime::PerfDecodeScope2 perf_raw{
                    swift::runtime::GetPerfStats2().decode_raw,
                    swift::runtime::PerfDecodePath2::Raw};
            decoder.DecodeX87FreePop(static_cast<u8>(code_ptr[1] & 7));
            decoder.pc += 2;
            decoder.assembler->AdvancePC(ir::Imm{2});
            decoder.end_decode = decoder.assembler->EndCommit();
        }
        RecordRawAccepted();
        return StepResult::Handled;
    }

    SVM_DECODE_STAGE StepResult DecodeXsavecInstruction() {
        // XSAVEC/XSAVEC64 is absent from this distorm snapshot. Substitute
        // XSAVE's opcode in a bounded copy, then use the shared XSAVE emitter.
        u32 opcode_offset = 0;
        while (opcode_offset < kMaxInsnBytes) {
            const u8 prefix = code_ptr[opcode_offset];
            const bool legacy =
                    prefix == 0x26 || prefix == 0x2E || prefix == 0x36 ||
                    prefix == 0x3E || prefix == 0x64 || prefix == 0x65 ||
                    prefix == 0x67;
            if (legacy ||
                (decoder.is_64bit && (prefix & 0xF0) == 0x40)) {
                ++opcode_offset;
                continue;
            }
            break;
        }
        if (opcode_offset + 2 >= kMaxInsnBytes ||
            code_ptr[opcode_offset] != 0x0F ||
            code_ptr[opcode_offset + 1] != 0xC7 ||
            (code_ptr[opcode_offset + 2] & 0x38) != 0x20 ||
            (code_ptr[opcode_offset + 2] & 0xC0) == 0xC0) {
            return StepResult::NotHandled;
        }

        std::array<u8, kFetchWindow> surrogate{};
        std::memcpy(surrogate.data(), code_ptr, surrogate.size());
        surrogate[opcode_offset + 1] = 0xAE;
        swift::runtime::PerfDecodeScope2 perf_distorm{
                swift::runtime::GetPerfStats2().decode_distorm};
        auto insn = DisDecode(
                surrogate.data(), surrogate.size(), decoder.is_64bit);
        const auto distorm_ns = perf_distorm.Stop();
        swift::runtime::PerfDecodeRecordOpcode(insn.opcode, distorm_ns);
        if ((insn.opcode != I_XSAVE && insn.opcode != I_XSAVE64) ||
            insn.size == 0) {
            return StepResult::NotHandled;
        }
        if (insn.size > fetch_avail) {
            decoder.Interrupt(InterruptReason::PAGE_FATAL);
            return StepResult::Stop;
        }

        {
            swift::runtime::PerfDecodeScope2 perf_raw{
                    swift::runtime::GetPerfStats2().decode_raw,
                    swift::runtime::PerfDecodePath2::Raw};
            decoder.insn_pc = decoder.pc;
            decoder.pc += insn.size;
            EmitXsavec(decoder.assembler,
                       decoder.FlatAddress(insn, insn.ops[0]),
                       decoder.pc,
                       decoder.insn_pc);
            decoder.assembler->AdvancePC(ir::Imm{insn.size});
            decoder.end_decode = decoder.assembler->EndCommit();
        }
        RecordRawAccepted();
        return StepResult::Handled;
    }

    SVM_DECODE_STAGE StepResult DecodeRdseedInstruction() {
        // RDSEED is also newer than the bundled distorm.
        if (!decoder.is_64bit) {
            return StepResult::NotHandled;
        }

        u32 offset = 0;
        bool operand16 = false;
        u8 rex = 0;
        if (code_ptr[offset] == 0x66) {
            operand16 = true;
            ++offset;
        }
        if ((code_ptr[offset] & 0xF0) == 0x40) {
            rex = code_ptr[offset++];
        }
        if (code_ptr[offset] != 0x0F || code_ptr[offset + 1] != 0xC7 ||
            (code_ptr[offset + 2] & 0xF8) != 0xF8) {
            return StepResult::NotHandled;
        }

        {
            swift::runtime::PerfDecodeScope2 perf_raw{
                    swift::runtime::GetPerfStats2().decode_raw,
                    swift::runtime::PerfDecodePath2::Raw};
            const u32 index = (code_ptr[offset + 2] & 7) | ((rex & 1) << 3);
            const u32 width = (rex & 8) ? 64 : (operand16 ? 16 : 32);
            const auto first =
                    width == 64 ? R_RAX : (width == 32 ? R_EAX : R_AX);
            decoder.DecodeRandomRegister(
                    static_cast<_RegisterType>(first + index), width);
            const u32 size = offset + 3;
            decoder.pc += size;
            decoder.assembler->AdvancePC(ir::Imm{size});
            decoder.end_decode = decoder.assembler->EndCommit();
        }
        RecordRawAccepted();
        return StepResult::Handled;
    }

    SVM_DECODE_STAGE bool DecodeDistormInstruction() {
        _DInst insn{};
        _DInst fast_insn{};
        bool fast_hit = false;
        if (fast_enabled || verify_fast) {
            swift::runtime::PerfDecodeScope2 perf_fast{
                    swift::runtime::GetPerfStats2().decode_raw,
                    swift::runtime::PerfDecodePath2::Raw};
            fast_hit = DecodeDistormFast(
                    code_ptr, fetch_avail, decoder.is_64bit, fast_insn);
            if (verify_fast || decode_prof) {
                DistormFastRecordAttempt(fast_hit);
            }
            if (fast_hit && decode_prof) {
                swift::runtime::GetPerfStats2().decode_raw_accepted.fetch_add(
                        1, std::memory_order_relaxed);
            }
        }

        if (!fast_hit || verify_fast || !fast_enabled) {
            swift::runtime::PerfDecodeScope2 perf_distorm{
                    swift::runtime::GetPerfStats2().decode_distorm};
            _DInst distorm_insn =
                    DisDecode(code_ptr, 0x10, decoder.is_64bit);
            const auto distorm_ns = perf_distorm.Stop();
            swift::runtime::PerfDecodeRecordOpcode(
                    distorm_insn.opcode, distorm_ns);
            if (fast_hit && verify_fast) {
                const bool match =
                        DistormFastEquivalent(fast_insn, distorm_insn, code_ptr);
                DistormFastRecordVerification(
                        match, fast_insn, distorm_insn, code_ptr);
                insn = match && fast_enabled ? fast_insn : distorm_insn;
            } else {
                insn = distorm_insn;
            }
        } else {
            insn = fast_insn;
        }

        swift::runtime::PerfDecodeScope2 perf_bookkeeping{
                swift::runtime::GetPerfStats2().decode_bookkeeping};
        FixupMovbeOperandSize(insn, code_ptr);
        FixupFsgsbaseOperand(insn, code_ptr);
        if (insn.opcode == UINT16_MAX || insn.size == 0) {
            decoder.Interrupt(InterruptReason::ILL_CODE);
            return false;
        }
        if (insn.size > fetch_avail) {
            decoder.Interrupt(InterruptReason::PAGE_FATAL);
            return false;
        }
        if (HasInvalidLockPrefix(insn)) {
            decoder.pc += insn.size;
            decoder.Interrupt(InterruptReason::ILL_CODE);
            decoder.assembler->AdvancePC(ir::Imm{insn.size});
            decoder.end_decode = decoder.assembler->EndCommit();
            return false;
        }

        decoder.insn_pc = decoder.pc;
        decoder.pc += insn.size;
        // Legacy VEX handlers re-read prefix bytes not represented by _DInst.
        decoder.insn_bytes = code_ptr;
        perf_bookkeeping.Stop();
        return LowerAndCommit(insn);
    }

    SVM_DECODE_STAGE bool HasInvalidLockPrefix(const _DInst& insn) const {
        for (u32 i = 0; i < insn.size && i < 16; ++i) {
            if ((insn.unusedPrefixesMask & (u16(1) << i)) != 0 &&
                code_ptr[i] == 0xF0) {
                return true;
            }
        }
        return false;
    }

    SVM_DECODE_STAGE bool LowerAndCommit(_DInst& insn) {
        swift::runtime::PerfDecodeScope2 perf_lowering{
                swift::runtime::GetPerfStats2().decode_lowering,
                swift::runtime::PerfDecodePath2::Lowering};
        unsigned operand_count = 0;
        bool has_memory = false;
        for (const auto& op : insn.ops) {
            if (op.type == O_NONE) {
                continue;
            }
            ++operand_count;
            has_memory = has_memory || op.type == O_SMEM || op.type == O_MEM ||
                         op.type == O_DISP;
        }
        swift::runtime::PerfLoweringBegin(
                insn.opcode, operand_count, has_memory);
        decoder.BeginStructuredAddressInstruction(insn.opcode);
        const bool lowered = decoder.DecodeSwitch(insn);
        const auto lowering_ns = perf_lowering.Stop();
        swift::runtime::PerfLoweringFinish(lowering_ns, lowered);
        if (!lowered) {
            decoder.Interrupt(InterruptReason::FALLBACK);
            return false;
        }

        swift::runtime::PerfDecodeScope2 perf_bookkeeping{
                swift::runtime::GetPerfStats2().decode_bookkeeping,
                swift::runtime::PerfDecodePath2::Bookkeeping};
        {
            swift::runtime::PerfDecodeScope2 perf_advance{
                    swift::runtime::GetPerfStats2().decode_advance_pc,
                    swift::runtime::PerfDecodePath2::Bookkeeping};
            decoder.assembler->AdvancePC(ir::Imm{insn.size});
        }
        {
            swift::runtime::PerfDecodeScope2 perf_end_commit{
                    swift::runtime::GetPerfStats2().decode_end_commit};
            decoder.end_decode = decoder.assembler->EndCommit();
        }
        return true;
    }

    SVM_DECODE_STAGE void RecordRawAccepted() const {
        if (decode_prof) {
            swift::runtime::GetPerfStats2().decode_raw_accepted.fetch_add(
                    1, std::memory_order_relaxed);
        }
    }

    X64Decoder& decoder;
    bool decode_prof{false};
    bool fast_enabled{false};
    bool verify_fast{false};
    u64 prior_fetch_page0{UINT64_MAX};
    u64 prior_fetch_page1{UINT64_MAX};
    std::array<u8, kFetchWindow> fetch_buffer{};
    u8* code_ptr{nullptr};
    size_t fetch_avail{0};
};

void X64Decoder::Decode() {
    DecodePipeline{*this}.Run();
}

#undef SVM_DECODE_STAGE


ir::Value X64Decoder::R(_RegisterType reg) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
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

static bool PinExtPartialWritesEnabled() {
    // Default ON after the flip A/B (PIN_EXT=2 bundle, coremark 5/5
    // pairs positive, median 1.22); =0 selects the RMW fallback as the
    // rollback.
    return swift::runtime::GetSvmConfig().x86_pin_ext >= 1;
}

bool X64Decoder::StructuredAddressModeEnabled() {
    return swift::runtime::GetSvmConfig().addrmode_struct;
}

bool X64Decoder::StructuredAddressChainOpcode(u16 opcode) {
    // Formal W39 promotion of the measured W33 STREAM chain. This list is
    // intentionally closed: adding an opcode means auditing that its lowering
    // neither writes a GPR behind R(reg,value) nor emits an intra-block label.
    switch (opcode) {
        case I_MOVAPS:
        case I_MOVAPD:
        case I_MOVNTPS:
        case I_MOVNTPD:
        case I_ADDPD:
        case I_MULPD:
            return true;
        default:
            return false;
    }
}

void X64Decoder::BeginStructuredAddressInstruction(u16 opcode) {
    structured_address_chain_active =
            StructuredAddressModeEnabled() && StructuredAddressChainOpcode(opcode);
    if (!structured_address_chain_active) {
        // This executes before lowering the non-whitelisted instruction.
        // Plain V128 memory inside that instruction may still carry a
        // structured Operand, but its state loads are not retained into the
        // next instruction.
        ClearStructuredAddressState();
    }
}

void X64Decoder::ClearStructuredAddressState() {
    structured_address_gprs.fill({});
}

void X64Decoder::InvalidateStructuredAddressReg(_RegisterType reg) {
    if (!StructuredAddressModeEnabled() || reg > _RegisterType::R_RIP) {
        return;
    }
    const auto& info = x86_regs_table[reg];
    if (info.index >= X86RegInfo::Rax && info.index <= X86RegInfo::R15) {
        // All aliases have the same X86RegInfo::index, including AH and every
        // 8/16/32-bit low view, so a partial write invalidates the full parent.
        structured_address_gprs[info.index - X86RegInfo::Rax] = {};
    }
}

ir::Value X64Decoder::AddressGprValue(_RegisterType reg) {
    if (!StructuredAddressModeEnabled() || !building_structured_address ||
        !structured_address_chain_active) {
        return R(reg);
    }
    ASSERT(reg <= _RegisterType::R_RIP);
    const auto& info = x86_regs_table[reg];
    if (info.high || info.type != ir::ValueType::U64 ||
        info.index < X86RegInfo::Rax || info.index > X86RegInfo::R15) {
        return R(reg);
    }
    auto& canonical = structured_address_gprs[info.index - X86RegInfo::Rax];
    if (!canonical.Defined()) {
        canonical = R(reg);
    }
    return canonical;
}

ir::Value X64Decoder::V(_RegisterType reg) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
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

// A 32-bit x86 GPR write replaces the full 64-bit architectural register
// with the zero-extended W result. The old lowering expressed that as a
// U32-producing ZeroExtend32 followed by a U64-producing ZeroExtend64. The
// folded opcode retains the U64 result type required by StoreUniform while
// keeping the operation's semantic truncation width explicitly 32 bits.
//
// SVM_GPR_ZEXT_COALESCE=0 restores the exact two-node pre-W19 lowering.
static bool GprZextCoalesceEnabled() {
    return swift::runtime::GetSvmConfig().gpr_zext_coalesce;
}

void X64Decoder::R(_RegisterType reg, ir::Value value) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    InvalidateStructuredAddressReg(reg);
    auto& info = x86_regs_table[reg];
    if (info.index >= X86RegInfo::Rax && info.index <= X86RegInfo::R15) {
        if (info.high) {
            if (PinExtPartialWritesEnabled() &&
                info.index >= X86RegInfo::Rax && info.index <= X86RegInfo::Rdx) {
                // AH/CH/DH are byte 1 of the newly pinned parent. Keep this as
                // a byte store: UniformElimination turns it into one BFI at
                // offset 8, while memory-backed/interpreter execution updates
                // exactly the same byte in ThreadContext64.
                auto offset = ToReg(info).GetOffset() + 1;
                __ StoreUniform(ir::Uniform{offset, ir::ValueType::U8},
                                NarrowTo(value, ir::ValueType::U8));
                return;
            }
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
            auto zext = GprZextCoalesceEnabled()
                                ? __ ZeroExtend32To64(value)
                                : __ ZeroExtend64(__ ZeroExtend32(value));
            __ StoreUniform(ir::Uniform{offset, ir::ValueType::U64}, zext);
            return;
        }
    }
    __ StoreUniform(ToReg(info), NarrowTo(value, info.type));
}

void X64Decoder::V(_RegisterType reg, ir::Value value) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    __ StoreUniform(ToVReg(x86_regs_table[reg]), value);
}

void X64Decoder::Interrupt(InterruptReason reason) {
    ir::Uniform uni_interrupt{offsetof(ThreadContext64, interrupt), ir::ValueType::U32};
    __ SetLocation(ir::Lambda{ir::Imm{pc}});
    __ StoreUniform(uni_interrupt, __ LoadImm(ir::Imm(static_cast<u32>(reason))));
    __ ReturnToHost();
}

ir::BOOL X64Decoder::CheckCond(Cond cond) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::Flags};
    if (auto local = TryLocalCondition(cond);
        local && local->fcmp.Def() && FlagsFcmpFuseEnabled()) {
        return __ FCmpCondSet(local->fcmp, local->arm).SetType(ir::ValueType::U8);
    }
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
    // Every other x86 condition is a pure NZCV function. Use CondSet, which
    // reads host NZCV directly (repeated TestFlags would go through Mrs/Tst
    // pairs and Tst clobbers host NZCV, degrading every subsequent read within
    // the block). The two x86 conditions with no single ARM equivalent (A and
    // BE) return early below, composed from polarity-aware pieces. CF involving
    // conditions honor the tracked carry polarity: after a sub-family op the
    // stored carry is the inverse of the x86 CF.
    ir::Cond arm;
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
    // One CondSet, not LoadImm(1) + LoadImm(0) + CondSelect: the fused form is
    // a `cset` on ARM64 and a `setcc` on x86, and LoadImm was the single most
    // numerous IR opcode in the corpus (15.15%), a large part of it these two
    // constants (docs/ir-expansion-attribution.md 2 and 3.4).
    //
    // SetType is REQUIRED, not decoration.  Inst::SetArg infers a default
    // return type from the first typed argument, and Cond is untyped -- an
    // opcode whose only argument is a Cond therefore keeps ValueType::VOID.
    // The JIT survives that (RegAlloc keys off the opcode's meta return type),
    // but Interpreter::WriteScalar early-returns on VOID and silently writes
    // nothing, so the condition would read as whatever was left in the slot.
    // Caught by run_helper_fault_tests.sh's SVM_ENABLE_JIT=0 shapes; the two
    // back ends now assert on it.  U8 is what the old LoadImm(u8) pair gave
    // CondSelect, so the register width is unchanged.
    return __ CondSet(arm).SetType(ir::ValueType::U8);
}

bool X64Decoder::FlagsNarrowAlignEnabled() {
    return runtime::GetSvmConfig().flags_narrow_align;
}

bool X64Decoder::FlagsCfinvEnabled() const {
    if (!runtime::GetSvmConfig().flags_cfinv) {
        return false;
    }
    return flags_cfinv_supported_;
}

bool X64Decoder::FlagsTerminalJccEnabled() {
    return runtime::GetSvmConfig().flags_terminal_jcc;
}

bool X64Decoder::FlagsBranchOnlyEnabled() {
    // Default ON after the flip A/B (7z Tot-MIPS 5/5 pairs positive, median
    // +5.0%); =0 keeps full EFLAGS materialization as the rollback.
    return runtime::GetSvmConfig().flags_branch_only;
}

bool X64Decoder::SuccessorFlagsDead(VAddr successor) const {
    // This is the block/lazy-function counterpart of the HIR CFG fixed point:
    // prove only a very small straight-line prefix. Any mapping boundary,
    // helper/control instruction, unknown opcode, or flag read is an immediate
    // conservative failure. MOV/LEA/NOP may precede the first full flag kill;
    // this matches the existing flags pass, where ordinary direct memory IR
    // is not a flags observer or helper boundary.
    constexpr u16 kArithmeticFlags = D_CF | D_PF | D_AF | D_ZF | D_SF | D_OF;
    u16 incoming = kArithmeticFlags;
    VAddr cursor = successor;
    for (u32 count = 0; count < 8; ++count) {
        std::array<u8, 16> bytes{};
        size_t available = 0;
        for (; available < bytes.size(); ++available) {
            const auto* byte = reinterpret_cast<const u8*>(
                    memory->GetPointer(reinterpret_cast<void*>(cursor + available)));
            if (!byte) {
                break;
            }
            bytes[available] = *byte;
        }
        if (available == 0) {
            return false;
        }
        auto insn = DisDecode(bytes.data(), bytes.size(), is_64bit);
        if (insn.opcode == UINT16_MAX || insn.size == 0 ||
            insn.size > available || META_GET_FC(insn.meta) != FC_NONE ||
            (insn.flags & (FLAG_LOCK | FLAG_REP | FLAG_REPNZ |
                           FLAG_PRIVILEGED_INSTRUCTION)) != 0) {
            return false;
        }
        if ((insn.testedFlagsMask & incoming) != 0) {
            return false;
        }
        incoming &= ~(insn.modifiedFlagsMask | insn.undefinedFlagsMask);
        if (incoming == 0) {
            return true;
        }
        if (insn.modifiedFlagsMask != 0 || insn.undefinedFlagsMask != 0) {
            // Partial writers such as INC preserve some incoming bits. Do not
            // reason through their frontend-specific carry handling.
            return false;
        }
        switch (insn.opcode) {
            case I_MOV:
            case I_MOVZX:
            case I_MOVSX:
            case I_MOVSXD:
            case I_LEA:
            case I_NOP:
                break;
            default:
                return false;
        }
        cursor += insn.size;
    }
    return false;
}

bool X64Decoder::FlagsFcmpFuseEnabled() {
    return runtime::GetSvmConfig().flags_fcmp_fuse;
}

void X64Decoder::MarkLocalNZCV(ir::Flags valid, ir::Value result) {
    local_nzcv_next_pc_ = pc;
    local_nzcv_valid_ = valid & (ir::Flags::NZCV | ir::Flags::Parity);
    local_flags_value_ = result;
}

void X64Decoder::PublishFCmpFlags(ir::Value packed) {
    const bool compact = FlagsFcmpCompactEnabled();
    __ PublishFCmpFlags(packed, ir::Imm(u32(compact)));
    carry_ = compact ? CarryPolarity::Inverted : CarryPolarity::Direct;
    StorePolarity(compact);
    local_fcmp_next_pc_ = pc;
    local_fcmp_value_ = packed;
}

std::optional<X64Decoder::LocalCondition> X64Decoder::TryLocalCondition(Cond cond) {
    // FCMP relations are deliberately mapped from IEEE outcomes, not from the
    // materialized x86 shadow.  These are exactly the single-condition sets:
    // less|unordered, greater|equal, greater, less|equal|unordered, unordered,
    // and ordered.  EQ/NE need two ARM conditions and stay on the old path.
    if (FlagsFcmpFuseEnabled() && local_fcmp_next_pc_ == insn_pc &&
        local_fcmp_value_.Def()) {
        switch (cond) {
            case Cond::CS:
            case Cond::BT:
                return LocalCondition{ir::Cond::LT, local_fcmp_value_, {}, false};
            case Cond::CC:
            case Cond::AE:
                return LocalCondition{ir::Cond::GE, local_fcmp_value_, {}, false};
            case Cond::HI:
            case Cond::AT:
                return LocalCondition{ir::Cond::GT, local_fcmp_value_, {}, false};
            case Cond::LS:
            case Cond::BE:
                return LocalCondition{ir::Cond::LE, local_fcmp_value_, {}, false};
            case Cond::PA:
                return LocalCondition{ir::Cond::VS, local_fcmp_value_, {}, false};
            case Cond::NP:
                return LocalCondition{ir::Cond::VC, local_fcmp_value_, {}, false};
            default:
                break;
        }
    }

    if (!FlagsTerminalJccEnabled() || local_nzcv_next_pc_ != insn_pc) {
        return std::nullopt;
    }

    if (FlagsBranchOnlyEnabled() && (cond == Cond::PA || cond == Cond::NP) &&
        True(local_nzcv_valid_ & ir::Flags::Parity) &&
        local_flags_value_.Def()) {
        return LocalCondition{
                {}, {}, local_flags_value_, cond == Cond::NP};
    }

    ir::Flags need{};
    ir::Cond arm{};
    switch (cond) {
        case Cond::EQ:
            need = ir::Flags::Zero;
            arm = ir::Cond::EQ;
            break;
        case Cond::NE:
            need = ir::Flags::Zero;
            arm = ir::Cond::NE;
            break;
        case Cond::MI:
        case Cond::SN:
            need = ir::Flags::Negate;
            arm = ir::Cond::MI;
            break;
        case Cond::PL:
        case Cond::NS:
            need = ir::Flags::Negate;
            arm = ir::Cond::PL;
            break;
        case Cond::VS:
            need = ir::Flags::Overflow;
            arm = ir::Cond::VS;
            break;
        case Cond::VC:
            need = ir::Flags::Overflow;
            arm = ir::Cond::VC;
            break;
        case Cond::GE:
            need = ir::Flags::Negate | ir::Flags::Overflow;
            arm = ir::Cond::GE;
            break;
        case Cond::LT:
            need = ir::Flags::Negate | ir::Flags::Overflow;
            arm = ir::Cond::LT;
            break;
        case Cond::GT:
            need = ir::Flags::Negate | ir::Flags::Overflow | ir::Flags::Zero;
            arm = ir::Cond::GT;
            break;
        case Cond::LE:
            need = ir::Flags::Negate | ir::Flags::Overflow | ir::Flags::Zero;
            arm = ir::Cond::LE;
            break;
        case Cond::CS:
        case Cond::BT:
            need = ir::Flags::Carry;
            if (carry_ == CarryPolarity::Direct) {
                arm = ir::Cond::CS;
            } else if (carry_ == CarryPolarity::Inverted) {
                arm = ir::Cond::CC;
            } else {
                return std::nullopt;
            }
            break;
        case Cond::CC:
        case Cond::AE:
            need = ir::Flags::Carry;
            if (carry_ == CarryPolarity::Direct) {
                arm = ir::Cond::CC;
            } else if (carry_ == CarryPolarity::Inverted) {
                arm = ir::Cond::CS;
            } else {
                return std::nullopt;
            }
            break;
        case Cond::HI:
        case Cond::AT:
            need = ir::Flags::Carry | ir::Flags::Zero;
            if (carry_ != CarryPolarity::Inverted) {
                return std::nullopt;
            }
            arm = ir::Cond::HI;
            break;
        case Cond::LS:
        case Cond::BE:
            need = ir::Flags::Carry | ir::Flags::Zero;
            if (carry_ != CarryPolarity::Inverted) {
                return std::nullopt;
            }
            arm = ir::Cond::LS;
            break;
        default:
            return std::nullopt;
    }
    if ((local_nzcv_valid_ & need) != need) {
        return std::nullopt;
    }
    return LocalCondition{arm, {}, {}, false};
}

ir::Value X64Decoder::CarryValue() {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::Flags};
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
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::Flags};
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
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
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
                if (PreserveMemoryEA(address_operand, size)) {
                    result = MemLoad(address_operand.ToIROperand(), size, false);
                } else {
                    auto address =
                            __ GetOperand(address_operand.ToIROperand())
                                    .SetType(is_64bit ? ir::ValueType::U64
                                                     : ir::ValueType::U32);
                    result = MemLoad(ir::Operand{address}, size, false);
                }
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
                if (PreserveMemoryEA(address, value.Type())) {
                    MemStore(address.ToIROperand(), value, false);
                } else {
                    auto folded =
                            __ GetOperand(address.ToIROperand())
                                    .SetType(is_64bit ? ir::ValueType::U64
                                                     : ir::ValueType::U32);
                    MemStore(ir::Operand{folded}, value, false);
                }
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

bool X64Decoder::PreserveMemoryEA(const X64Decoder::Operand& operand,
                                  ir::ValueType access_type) const {
    if (!addr_ea_tie_ || !identity_addressing_) {
        return false;
    }
    const auto ir_operand = operand.ToIROperand();
    if (!ir_operand.GetLeft().IsValue() || ir_operand.GetRight().Null() ||
        ir::GetValueSizeByte(ir_operand.GetLeft().value.Type()) != sizeof(u64)) {
        return false;
    }
    // 只保留现有 ARM64 memory emitter 已能直接编码的两类结构；其它地址
    // 仍先经 GetOperand 物化，避免把本开关扩成通用地址重写。
    if (ir_operand.GetOp() == ir::OperandOp::Plus &&
        ir_operand.GetRight().IsImm()) {
        return true;
    }
    if (ir_operand.GetOp() != ir::OperandOp::PlusExt ||
        !ir_operand.GetRight().IsValue() ||
        ir::GetValueSizeByte(ir_operand.GetRight().value.Type()) != sizeof(u64)) {
        return false;
    }
    const auto shift = ir_operand.GetOp().shift_ext;
    return shift < 4 && (u64{1} << shift) == ir::GetValueSizeByte(access_type);
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
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::Address};
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
                address_operand.left = AddressGprValue(static_cast<_RegisterType>(op.index));
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
                        address_operand.left = AddressGprValue(static_cast<_RegisterType>(insn.base));
                    }
                } else {
                    // Segment override combined with a base register: fold the base
                    // in arithmetically (segment scaling above stays dropped).
                    address_operand.left =
                            __ Add(address_operand.left.value,
                                   ir::Operand{AddressGprValue(static_cast<_RegisterType>(insn.base))});
                }
                if (op.index != R_NONE) {
                    address_operand.right = AddressGprValue(static_cast<_RegisterType>(op.index));
                }
            } else if (op.index != R_NONE) {
                address_operand.left = AddressGprValue(static_cast<_RegisterType>(op.index));
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
    return swift::runtime::GetSvmConfig().avx;
}

// True when a 0x66 operand-size prefix precedes the instruction's 0x0F escape
// byte. Only needed where distorm loses the distinction between an MMX encoding
// and its 66-prefixed SSE twin (see I_PADDQ in DecodeSwitch); everywhere else
// the operand register class already carries it.
bool X64Decoder::HasOperandSizePrefix() const {
    if (!insn_bytes) {
        return false;
    }
    // Legacy prefixes may appear in any order ahead of the escape byte, so scan
    // until 0x0F. The bound is x86's 4-prefix-group limit plus REX; anything
    // longer is not a two-byte-opcode encoding.
    for (u32 i = 0; i < 5; i++) {
        const u8 b = insn_bytes[i];
        if (b == 0x0F) {
            return false;
        }
        if (b == 0x66) {
            return true;
        }
    }
    return false;
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

ir::Value X64Decoder::YmmHighRead(u32 index) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    return __ LoadUniform(YmmHighUniform(index));
}

void X64Decoder::YmmHighWrite(u32 index, ir::Value value) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    __ StoreUniform(YmmHighUniform(index), value.SetType(ir::ValueType::V128));
}

ir::Value X64Decoder::YmmHighLo(u32 index) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    return __ LoadUniform(ir::Uniform{YmmHighUniform(index).GetOffset(), ir::ValueType::U64});
}

ir::Value X64Decoder::YmmHighHi(u32 index) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    return __ LoadUniform(ir::Uniform{YmmHighUniform(index).GetOffset() + 8, ir::ValueType::U64});
}

void X64Decoder::YmmHighLo(u32 index, ir::Value value) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    // NarrowTo normalizes untyped (CallLambda) values so the store has a width,
    // mirroring XmmLo/XmmHi.
    __ StoreUniform(ir::Uniform{YmmHighUniform(index).GetOffset(), ir::ValueType::U64},
                    NarrowTo(value, ir::ValueType::U64));
}

void X64Decoder::YmmHighHi(u32 index, ir::Value value) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
    __ StoreUniform(ir::Uniform{YmmHighUniform(index).GetOffset() + 8, ir::ValueType::U64},
                    NarrowTo(value, ir::ValueType::U64));
}

void X64Decoder::ZeroYmmHigh(u32 index) {
    swift::runtime::PerfLoweringPartScope2 perf{
            swift::runtime::PerfLoweringPart2::RegValue};
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
            v = MemLoad(ir::Operand{FlatAddress(insn, op1)},
                        ir::ValueType::V128,
                        TsoOrdered(insn));
        }
        XmmWrite(XmmOf(dst), v);
        ZeroYmmHigh(dst);
    } else {
        // Store form: m128, xmm. No destination register, so no upper half to
        // clear (vmovntdq degrades to a plain store, as the SSE path does).
        auto v = XmmRead(XmmOf(VecIndex(static_cast<_RegisterType>(op1.index))));
        MemStore(ir::Operand{FlatAddress(insn, op0)}, v, TsoOrdered(insn));
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
