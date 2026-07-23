//
// Created by 甘尧 on 2024/2/23.
//
// IR interpreter: fallback execution engine when the JIT is unavailable and
// executable reference for the IR semantics. The authoritative semantic
// reference is the arm64 JIT (backend/arm64/jit/translator.cpp); wherever the
// two could disagree this implementation follows the JIT, including its
// quirks (documented inline).

#include <alloca.h>
#include <cstring>
#include <unordered_map>
#include "interpreter.h"
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::interp {

using ir::ValueType;

namespace {

// ---------------------------------------------------------------------------
// Host-side layout of the virtual flags word (state.host_cpu_flags).
//
// This mirrors the JIT flags register (x26) layout bit-for-bit so that blocks
// executed by the interpreter and by the JIT observe identical flag state:
//  - guest N/Z/C/V live at the ARM64 host NZCV bit positions (31/30/29/28),
//    so JITed code can merge them with Mrs/Msr (GuestNZCVToHost);
//  - bits 7..0 hold the low byte of the last result whose SaveFlags pseudo
//    requested Parity (x86 PF is derived from it on test);
//  - bit 26 holds x86 AF exactly (carry/borrow into bit 4), matching the JIT's
//    HostFlagsBit::AuxiliaryCarry single-bit representation.
// See JitTranslator::SaveParity / SaveAuxiliaryCarry / HostFlagsBit.
// ---------------------------------------------------------------------------
constexpr u32 kHostFlagN = 31;
constexpr u32 kHostFlagZ = 30;
constexpr u32 kHostFlagC = 29;
constexpr u32 kHostFlagV = 28;
constexpr u32 kHostAF = 26;
constexpr u32 kHostParityByte = 0;   // width 8

u32 TypeBits(ValueType type) { return ir::GetValueSizeByte(type) * 8; }

bool IsVector(ValueType type) { return type >= ValueType::V8 && type <= ValueType::V256; }

u64 MaskBits(u32 bits) { return bits >= 64 ? ~u64(0) : ((u64(1) << bits) - 1); }

u64 MaskFor(ValueType type) { return MaskBits(TypeBits(type)); }

u64 SignExtendTo(u64 value, u32 bits) {
    if (bits >= 64) {
        return value;
    }
    const u64 sign = u64(1) << (bits - 1);
    return (value ^ sign) - sign;
}

}  // namespace

Interpreter::Interpreter(State& state, ir::Block* block) : state(state), block(block) {}

// ---------------------------------------------------------------------------
// Value slot accessors
// ---------------------------------------------------------------------------

u64 Interpreter::ReadScalar(InterpStack& stack, ir::Value value) {
    return GetReg<u64>(stack, value);
}

Interpreter::u128 Interpreter::ReadVec(InterpStack& stack, ir::Value value) {
    return GetReg<u128>(stack, value);
}

void Interpreter::WriteScalar(InterpStack& stack, ir::Inst* inst, u64 value) {
    const auto type = inst->ReturnType();
    if (type == ValueType::VOID) {
        return;
    }
    if (IsVector(type)) {
        WriteVec(stack, inst, static_cast<u128>(value));
        return;
    }
    // Slots always hold the value zero-extended / masked to its type width, so
    // plain u64 compares (TestZero, terminals, Select) behave like the JIT's
    // width-correct register compares.
    value &= MaskFor(type);
    SetReg(stack, ir::Value{inst}, value);
}

void Interpreter::WriteVec(InterpStack& stack, ir::Inst* inst, u128 value) {
    SetReg(stack, ir::Value{inst}, value);
}

// ---------------------------------------------------------------------------
// Operand / immediate evaluation
// ---------------------------------------------------------------------------

u64 Interpreter::EvalDataClass(InterpStack& stack, const ir::DataClass& data) {
    if (data.IsValue()) {
        return ReadScalar(stack, data.value);
    }
    if (data.IsImm()) {
        // The JIT materializes immediates with Mov(imm.Get()) (zero-extending
        // narrow signed imms) except in width-specific add/sub encodings.
        // Because every consumer masks to the operation width afterwards,
        // Get() vs GetSigned() is observationally equivalent here.
        return data.imm.Get();
    }
    return 0;
}

u64 Interpreter::EvalOperand(InterpStack& stack, const ir::Operand& operand) {
    const u64 left = EvalDataClass(stack, operand.GetLeft());
    const auto& right = operand.GetRight();
    if (right.Null()) {
        return left;
    }
    const u64 rval = EvalDataClass(stack, right);
    switch (operand.GetOp().type) {
        case ir::OperandOp::None:
            // Single-sided operand emitted as {left, Imm 0, OperandNone}
            // (see A64Decoder::SingleOperand).
            return left;
        case ir::OperandOp::Plus:
            return left + rval;
        case ir::OperandOp::LSL:
            // Width-correct for 32-bit ops too: the result is masked to the
            // destination width on write, matching W-register shifts.
            return left << (rval & 63);
        case ir::OperandOp::LSR:
            return (left & MaskFor(operand.GetLeft().IsValue() ? operand.GetLeft().value.Type()
                                                                : ValueType::U64)) >>
                   (rval & 63);
        case ir::OperandOp::PlusExt:
            return left + (rval << operand.GetOp().shift_ext);
        default:
            return left;
    }
}

u64 Interpreter::EvalLambda(InterpStack& stack, ir::Lambda& lambda) {
    return lambda.IsValue() ? ReadScalar(stack, lambda.GetValue()) : lambda.GetImm().Get();
}

// ---------------------------------------------------------------------------
// Host calls (CallLambda / CallDynamic / CallLocation / CallHost)
// ---------------------------------------------------------------------------

u64 Interpreter::CallHostFunc(InterpStack& stack,
                              ir::Lambda& lambda,
                              const std::vector<ir::DataClass>& args) {
    // Host C-ABI call with up to 8 u64 arguments, u64 result. The frontends
    // use this for helpers the IR cannot express (x86 128-bit multiply high
    // half, 128-bit dividends, REP MOVS); see decoder.cc MulHiU64/DivQU64/...
    u64 argv[8]{};
    for (size_t i = 0; i < args.size() && i < 8; ++i) {
        argv[i] = EvalDataClass(stack, args[i]);
    }
    const u64 addr = EvalLambda(stack, lambda);
    using HostFn = u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64);
    const auto fn = reinterpret_cast<HostFn>(static_cast<uintptr_t>(addr));
    return fn(argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]);
}

// ---------------------------------------------------------------------------
// Guest flags
// ---------------------------------------------------------------------------

void Interpreter::SaveGuestFlags(InterpStack& stack, ir::Inst* def, ir::Flags f) {
    const auto type = def->ReturnType();
    const u32 bits = TypeBits(type);
    if (bits == 0 || bits > 64) {
        return;
    }
    const u64 result = ReadScalar(stack, ir::Value{def});
    u64& fl = state.host_cpu_flags;
    auto set = [&](u32 bit, bool on) {
        fl = on ? (fl | (u64(1) << bit)) : (fl & ~(u64(1) << bit));
    };

    if (True(f & ir::Flags::Negate)) {
        set(kHostFlagN, (result >> (bits - 1)) & 1);
    }
    if (True(f & ir::Flags::Zero)) {
        set(kHostFlagZ, result == 0);
    }

    if (True(f & (ir::Flags::Carry | ir::Flags::Overflow))) {
        bool have{false}, carry{false}, overflow{false};
        switch (def->GetOp()) {
            case ir::OpCode::Add:
            case ir::OpCode::Adc: {
                // The incoming carry must be read before this save updates C.
                const u64 cin =
                        def->GetOp() == ir::OpCode::Adc ? ((fl >> kHostFlagC) & 1) : 0;
                const u64 l = ReadScalar(stack, def->GetArg<ir::Value>(0)) & MaskBits(bits);
                const u64 r = EvalOperand(stack, def->GetArg<ir::Operand>(1)) & MaskBits(bits);
                const unsigned __int128 wide = static_cast<unsigned __int128>(l) + r + cin;
                carry = (wide >> bits) & 1;
                const u64 sl = SignExtendTo(l, bits);
                const u64 sr = SignExtendTo(r, bits);
                const u64 sres = SignExtendTo(result, bits);
                overflow = ((~(sl ^ sr) & (sl ^ sres)) >> (bits - 1)) & 1;
                have = true;
                break;
            }
            case ir::OpCode::Sub:
            case ir::OpCode::Sbb: {
                // ARM semantics (matching the JIT): C = NOT borrow.
                const u64 bin =
                        def->GetOp() == ir::OpCode::Sbb ? (1 - ((fl >> kHostFlagC) & 1)) : 0;
                const u64 l = ReadScalar(stack, def->GetArg<ir::Value>(0)) & MaskBits(bits);
                const u64 r = EvalOperand(stack, def->GetArg<ir::Operand>(1)) & MaskBits(bits);
                carry = static_cast<unsigned __int128>(l) >=
                        static_cast<unsigned __int128>(r) + bin;
                const u64 sl = SignExtendTo(l, bits);
                const u64 sr = SignExtendTo(r, bits);
                const u64 sres = SignExtendTo(result, bits);
                overflow = (((sl ^ sr) & (sl ^ sres)) >> (bits - 1)) & 1;
                have = true;
                break;
            }
            case ir::OpCode::And:
            case ir::OpCode::Or:
            case ir::OpCode::Xor:
            case ir::OpCode::AndNot:
                // Logical ops: C = V = 0 (host ANDS/BICS behaviour, which the
                // arm64 guest relies on; the x86 frontend additionally clears
                // C/V via a ClearFlags pseudo).
                carry = false;
                overflow = false;
                have = true;
                break;
            case ir::OpCode::Mul: {
                if (bits < 64) {
                    // Mirrors the JIT's want_cv path: widen the multiply and
                    // check the upper half. 64-bit Mul leaves C/V untouched
                    // (JitTranslator::SaveCV returns early for U64).
                    const u64 l = ReadScalar(stack, def->GetArg<ir::Value>(0)) & MaskBits(bits);
                    const u64 r =
                            EvalOperand(stack, def->GetArg<ir::Operand>(1)) & MaskBits(bits);
                    if (ir::IsSignValueType(def->GetArg<ir::Value>(0).Type())) {
                        const __int128 wide = static_cast<__int128>(
                                                      static_cast<s64>(SignExtendTo(l, bits))) *
                                              static_cast<s64>(SignExtendTo(r, bits));
                        overflow = carry =
                                wide != static_cast<__int128>(static_cast<s64>(
                                                SignExtendTo(result, bits)));
                    } else {
                        const unsigned __int128 wide =
                                static_cast<unsigned __int128>(l) * r;
                        overflow = carry = (wide >> bits) != 0;
                    }
                    have = true;
                }
                break;
            }
            default:
                // Other ops do not define C/V; leave them untouched.
                break;
        }
        if (have) {
            if (True(f & ir::Flags::Carry)) {
                set(kHostFlagC, carry);
            }
            if (True(f & ir::Flags::Overflow)) {
                set(kHostFlagV, overflow);
            }
        }
    }

    if (True(f & ir::Flags::Parity)) {
        // JIT SaveParity: stash the low byte of the result; PF is derived
        // from it lazily in TestFlags.
        fl = (fl & ~(u64(0xFF) << kHostParityByte)) |
             ((result & 0xFF) << kHostParityByte);
    }
    if (True(f & ir::Flags::AuxiliaryCarry)) {
        switch (def->GetOp()) {
            case ir::OpCode::Add:
            case ir::OpCode::Adc:
            case ir::OpCode::Sub:
            case ir::OpCode::Sbb: {
                // JIT SaveAuxiliaryCarry: AF = bit4(left) ^ bit4(right) ^
                // bit4(result) (carry/borrow into bit 4), stored as one bit.
                const u64 l = ReadScalar(stack, def->GetArg<ir::Value>(0));
                const u64 r = EvalOperand(stack, def->GetArg<ir::Operand>(1));
                set(kHostAF, ((l ^ r ^ result) >> 4) & 1);
                break;
            }
            default:
                // The JIT never snapshots AF operands for non add/sub ops.
                break;
        }
    }
}

bool Interpreter::TestGuestFlags(ir::Flags f) {
    const u64 fl = state.host_cpu_flags;
    bool result{false};
    bool first{true};

    u64 nzcv{0};
    if (True(f & ir::Flags::Negate)) {
        nzcv |= u64(1) << kHostFlagN;
    }
    if (True(f & ir::Flags::Zero)) {
        nzcv |= u64(1) << kHostFlagZ;
    }
    if (True(f & ir::Flags::Carry)) {
        nzcv |= u64(1) << kHostFlagC;
    }
    if (True(f & ir::Flags::Overflow)) {
        nzcv |= u64(1) << kHostFlagV;
    }
    if (nzcv) {
        // JIT: Tst(flags, mask); Cset ne -> "any of the tested bits set".
        result = (fl & nzcv) != 0;
        first = false;
    }
    if (True(f & ir::Flags::Parity)) {
        // Mirrors JitTranslator::TestParityFlag: fold the saved byte, x86 PF
        // is set on even parity.
        u64 b = (fl >> kHostParityByte) & 0xFF;
        b ^= b >> 4;
        b ^= b >> 2;
        b ^= b >> 1;
        const bool pf = ((b & 1) ^ 1) != 0;
        result = first ? pf : (result && pf);
        first = false;
    }
    if (True(f & ir::Flags::AuxiliaryCarry)) {
        // Mirrors JitTranslator::TestAuxiliaryCarry: AF is a single bit.
        const bool af = (fl >> kHostAF) & 1;
        result = first ? af : (result && af);
        first = false;
    }
    // Empty mask -> false, like the JIT's `Mov(result, 0)` fallback.
    return result;
}

void Interpreter::ClearGuestFlags(ir::Flags f) {
    u64& fl = state.host_cpu_flags;
    if (True(f & ir::Flags::Negate)) {
        fl &= ~(u64(1) << kHostFlagN);
    }
    if (True(f & ir::Flags::Zero)) {
        fl &= ~(u64(1) << kHostFlagZ);
    }
    if (True(f & ir::Flags::Carry)) {
        fl &= ~(u64(1) << kHostFlagC);
    }
    if (True(f & ir::Flags::Overflow)) {
        fl &= ~(u64(1) << kHostFlagV);
    }
    if (True(f & ir::Flags::Parity)) {
        // JIT quirk (JitTranslator::ClearFlags): clearing Parity writes 1
        // into the saved parity byte rather than zeroing it.
        fl = (fl & ~(u64(0xFF) << kHostParityByte)) | (u64(1) << kHostParityByte);
    }
    if (True(f & ir::Flags::AuxiliaryCarry)) {
        fl &= ~(u64(1) << kHostAF);
    }
}

bool Interpreter::EvalCondition(ir::Cond cond) {
    const u64 fl = state.host_cpu_flags;
    const bool n = (fl >> kHostFlagN) & 1;
    const bool z = (fl >> kHostFlagZ) & 1;
    const bool c = (fl >> kHostFlagC) & 1;
    const bool v = (fl >> kHostFlagV) & 1;
    // ir::Cond values match the ARM condition encoding (JitTranslator::MapCond).
    switch (cond) {
        case ir::Cond::EQ:
            return z;
        case ir::Cond::NE:
            return !z;
        case ir::Cond::CS:
            return c;
        case ir::Cond::CC:
            return !c;
        case ir::Cond::MI:
            return n;
        case ir::Cond::PL:
            return !n;
        case ir::Cond::VS:
            return v;
        case ir::Cond::VC:
            return !v;
        case ir::Cond::HI:
            return c && !z;
        case ir::Cond::LS:
            return !c || z;
        case ir::Cond::GE:
            return n == v;
        case ir::Cond::LT:
            return n != v;
        case ir::Cond::GT:
            return !z && (n == v);
        case ir::Cond::LE:
            return z || (n != v);
        case ir::Cond::AL:
        case ir::Cond::NV:
        default:
            return true;
    }
}

// ---------------------------------------------------------------------------
// Block / terminal execution
// ---------------------------------------------------------------------------

HaltReason Interpreter::Run() {
    const size_t slot_count = size_t(block->MaxInstrId()) * kSlotStride + kSlotStride;
    auto* raw = static_cast<u64*>(alloca(slot_count * sizeof(u64)));
    std::memset(raw, 0, slot_count * sizeof(u64));
    InterpStack stack{raw, slot_count};

    auto& insts = block->GetInstList();

    // Block-local labels for Goto / NotGoto: the JIT lowers them to local
    // labels + Cbz/Cbnz where BindLabel(arg = the Goto's value) marks the
    // jump target (JitTranslator::EmitGoto / EmitBindLabel).
    std::unordered_map<ir::Inst*, ir::Inst*> labels;
    for (auto& inst : insts) {
        if (inst.GetOp() == ir::OpCode::BindLabel) {
            labels[inst.GetArg<ir::Value>(0).Def()] = &inst;
        }
    }

    for (auto it = insts.begin(); it != insts.end(); ++it) {
        const auto op = it->GetOp();
        if (op == ir::OpCode::Goto || op == ir::OpCode::NotGoto) {
            const bool cond = GetReg<u64>(stack, it->GetArg<ir::Value>(0)) != 0;
            const bool jump = (op == ir::OpCode::Goto) == cond;
            if (jump) {
                if (auto target = labels.find(&*it); target != labels.end()) {
                    it = insts.iterator_to(*target->second);
                }
            }
            continue;
        }
        Run(&*it, stack);
        // Wild-pointer guard (RunLoadMemory/RunStoreMemory) and any future
        // instruction-level fault sets state.halt_reason; stop the block
        // immediately so the halt propagates to the dispatcher instead of
        // continuing to execute past the faulting instruction.
        if (state.halt_reason != HaltReason::None) {
            return state.halt_reason;
        }
    }
    return RunTerminal(block->GetTerminal(), stack);
}

HaltReason Interpreter::Run(ir::Inst* inst, InterpStack& stack) {
    switch (inst->GetOp()) {
#define INST(name, ...)                                                                            \
    case ir::OpCode::name:                                                                         \
        Run##name(inst, stack);                                                                    \
        break;
#include "runtime/ir/ir.inc"
#undef INST
        default:
            break;
    }
    // Individual instructions never halt; only terminals produce a HaltReason.
    return HaltReason::None;
}

HaltReason Interpreter::RunTerminal(const ir::Terminal& terminal, InterpStack& stack) {
    HaltReason result{HaltReason::None};
    VisitVariant<void>(terminal, [&](auto term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::Invalid>) {
            // Flat decoded blocks already wrote the next location via a
            // SetLocation instruction; just return to the dispatcher.
            result = HaltReason::None;
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToDispatch>) {
            result = HaltReason::None;
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost>) {
            state.halt_reason = HaltReason::CallHost;
            result = HaltReason::CallHost;
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlock>) {
            state.prev_loc = state.current_loc;
            state.current_loc = term.next;
            result = HaltReason::None;
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            state.prev_loc = state.current_loc;
            state.current_loc = term.next;
            result = HaltReason::None;
        } else if constexpr (std::is_same_v<T, ir::terminal::PopRSBHint>) {
            // RSB is not modelled (same as the JIT); behave like
            // ReturnToDispatch. current_loc was set by a SetLocation inst.
            result = HaltReason::None;
        } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
            const bool cond = GetReg<u64>(stack, term.cond) != 0;
            result = RunTerminal(cond ? term.then_ : term.else_, stack);
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            result = RunTerminal(EvalCondition(term.cond) ? term.then_ : term.else_, stack);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            const u64 value = ReadScalar(stack, term.value);
            for (auto& case_ : term.cases) {
                if (value == case_.case_value.Get()) {
                    result = RunTerminal(case_.then, stack);
                    return;
                }
            }
            // No case matched: bail out to the dispatcher (same as the JIT).
            result = HaltReason::None;
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            if (state.halt_reason != HaltReason::None) {
                result = state.halt_reason;
            } else {
                result = RunTerminal(term.else_, stack);
            }
        } else {
            PANIC("Unknown terminal!");
        }
    });
    return result;
}

// ---------------------------------------------------------------------------
// Instructions
// ---------------------------------------------------------------------------

void Interpreter::RunDefineLocal(ir::Inst* inst, InterpStack& stack) {}

void Interpreter::RunLoadLocal(ir::Inst* inst, InterpStack& stack) {
    // Locals are modelled as 8-byte slots indexed by Local::id inside
    // state.local_buffer (assumption: no current frontend emits locals; the
    // arm64 translator config sets has_local_operation=false and the JIT
    // PANICs on these).
    const auto local = inst->GetArg<ir::Local>(0);
    const auto* base = static_cast<const u8*>(state.local_buffer);
    u64 value{0};
    if (base) {
        std::memcpy(&value, base + size_t(local.id) * 8, ir::GetValueSizeByte(local.type));
    }
    WriteScalar(stack, inst, value);
}

void Interpreter::RunStoreLocal(ir::Inst* inst, InterpStack& stack) {
    const auto local = inst->GetArg<ir::Local>(0);
    auto* base = static_cast<u8*>(state.local_buffer);
    if (!base) {
        return;
    }
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(1));
    std::memcpy(base + size_t(local.id) * 8, &value, ir::GetValueSizeByte(local.type));
}

void Interpreter::RunLoadUniform(ir::Inst* inst, InterpStack& stack) {
    const auto uni = inst->GetArg<ir::Uniform>(0);
    const auto type =
            inst->ReturnType() == ValueType::VOID ? uni.GetType() : inst->ReturnType();
    const auto* base = &state.uniform_buffer_begin[uni.GetOffset()];
    if (IsVector(type)) {
        u128 value{};
        std::memcpy(&value, base, ir::GetValueSizeByte(type));
        WriteVec(stack, inst, value);
    } else {
        // Zero-extending load, matching the JIT's Ldrb/Ldrh/Ldr W.
        u64 value{0};
        std::memcpy(&value, base, ir::GetValueSizeByte(type));
        WriteScalar(stack, inst, value);
    }
}

void Interpreter::RunStoreUniform(ir::Inst* inst, InterpStack& stack) {
    const auto uni = inst->GetArg<ir::Uniform>(0);
    const auto value = inst->GetArg<ir::Value>(1);
    // Frontends always type their values; fall back to the uniform's declared
    // type for hand-built IR with an untyped value.
    const auto type = value.Type() == ValueType::VOID ? uni.GetType() : value.Type();
    auto* base = &state.uniform_buffer_begin[uni.GetOffset()];
    if (IsVector(type)) {
        const u128 v = ReadVec(stack, value);
        std::memcpy(base, &v, ir::GetValueSizeByte(type));
    } else {
        const u64 v = ReadScalar(stack, value);
        std::memcpy(base, &v, ir::GetValueSizeByte(type));
    }
}

void Interpreter::RunLoadMemory(ir::Inst* inst, InterpStack& stack) {
    const auto operand = inst->GetArg<ir::Operand>(0);
    const auto type = inst->ReturnType();
    const u64 guest_addr = EvalOperand(stack, operand);
    // Wild-pointer guard: a guest address at or beyond the guest address-space
    // limit (Config::loc_end) is definitionally invalid. Raise PageFatal instead
    // of letting the host dereference a bad pointer (SIGSEGV). The JIT path
    // relies on the host signal handler for this; the interpreter has none.
    const u64 access_size = ir::GetValueSizeByte(type);
    if (guest_addr >= state.guest_addr_limit ||
        guest_addr + access_size > state.guest_addr_limit ||
        (state.interp_range_check &&
         !state.interp_range_check(state.interp_range_check_ctx, guest_addr, access_size))) {
        state.halt_reason = HaltReason::PageFatal;
        return;
    }
    // Guest address virtualization: state.pt carries the guest->host bias
    // (host = guest + bias); it is 0 for identity mapping.
    const auto* ptr = reinterpret_cast<const void*>(guest_addr +
                                                    reinterpret_cast<uintptr_t>(state.pt));
    if (IsVector(type)) {
        u128 value{};
        std::memcpy(&value, ptr, ir::GetValueSizeByte(type));
        WriteVec(stack, inst, value);
    } else {
        // All scalar loads zero-extend (Ldrb/Ldrh/Ldr W/Ldr X in the JIT);
        // signed loads are expressed with a separate SignExtend instruction.
        u64 value{0};
        std::memcpy(&value, ptr, ir::GetValueSizeByte(type));
        WriteScalar(stack, inst, value);
    }
}

void Interpreter::RunStoreMemory(ir::Inst* inst, InterpStack& stack) {
    const auto operand = inst->GetArg<ir::Operand>(0);
    const auto value = inst->GetArg<ir::Value>(1);
    const auto type = value.Type();
    const u64 guest_addr = EvalOperand(stack, operand);
    // Wild-pointer guard: see RunLoadMemory for the rationale.
    const u64 access_size = ir::GetValueSizeByte(type);
    if (guest_addr >= state.guest_addr_limit ||
        guest_addr + access_size > state.guest_addr_limit ||
        (state.interp_range_check &&
         !state.interp_range_check(state.interp_range_check_ctx, guest_addr, access_size))) {
        state.halt_reason = HaltReason::PageFatal;
        return;
    }
    auto* ptr = reinterpret_cast<void*>(guest_addr +
                                        reinterpret_cast<uintptr_t>(state.pt));
    if (IsVector(type)) {
        const u128 v = ReadVec(stack, value);
        std::memcpy(ptr, &v, ir::GetValueSizeByte(type));
    } else {
        const u64 v = ReadScalar(stack, value);
        std::memcpy(ptr, &v, ir::GetValueSizeByte(type));
    }
}

void Interpreter::RunLoadMemoryTSO(ir::Inst* inst, InterpStack& stack) {
    // TSO ordering is only observable with multiple concurrent guest threads;
    // the interpreter executes one guest thread on one host thread, so a TSO
    // load is semantically identical to a plain load here (the JIT provides
    // the ordering with plain load + dmb ishld — see
    // JitTranslator::EmitLoadMemoryTSO).
    RunLoadMemory(inst, stack);
}

void Interpreter::RunStoreMemoryTSO(ir::Inst* inst, InterpStack& stack) {
    // See RunLoadMemoryTSO: single-threaded execution makes the release
    // ordering unobservable, so TSO stores degrade to plain stores.
    RunStoreMemory(inst, stack);
}

void Interpreter::RunMemoryCopy(ir::Inst* inst, InterpStack& stack) {
    auto dst = inst->GetArg<ir::Lambda>(0);
    auto src = inst->GetArg<ir::Lambda>(1);
    const u64 size = inst->GetArg<ir::Imm>(2).Get();
    // The lambdas evaluate to guest addresses; apply the pt bias (0 for
    // identity mapping).
    const auto bias = reinterpret_cast<uintptr_t>(state.pt);
    std::memmove(reinterpret_cast<void*>(EvalLambda(stack, dst) + bias),
                 reinterpret_cast<const void*>(EvalLambda(stack, src) + bias),
                 size);
}

void Interpreter::RunMemoryCopyTSO(ir::Inst* inst, InterpStack& stack) {
    RunMemoryCopy(inst, stack);
}

void Interpreter::RunCompareAndSwap(ir::Inst* inst, InterpStack& stack) {
    // Args: (address, expected, desired); returns the old value.
    // Single-threaded semantics: no retry loop, no exclusives (the JIT uses
    // Ldaxr/Stlxr).
    const u64 addr = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const auto expected = inst->GetArg<ir::Value>(1);
    const auto desired = inst->GetArg<ir::Value>(2);
    const u32 bits = TypeBits(expected.Type());
    const u64 mask = MaskBits(bits);
    auto* ptr = reinterpret_cast<void*>(addr + reinterpret_cast<uintptr_t>(state.pt));
    u64 old{0};
    std::memcpy(&old, ptr, bits / 8);
    old &= mask;
    if (old == (ReadScalar(stack, expected) & mask)) {
        const u64 d = ReadScalar(stack, desired) & mask;
        std::memcpy(ptr, &d, bits / 8);
    }
    WriteScalar(stack, inst, old);
}

void Interpreter::RunUniformBarrier(ir::Inst* inst, InterpStack& stack) {
    // Compiler barrier only (same as the JIT); no runtime effect.
}

void Interpreter::RunGoto(ir::Inst* inst, InterpStack& stack) {
    // Handled in Interpreter::Run's instruction loop (block-local jump).
}

void Interpreter::RunNotGoto(ir::Inst* inst, InterpStack& stack) {
    // Handled in Interpreter::Run's instruction loop (block-local jump).
}

void Interpreter::RunSelect(ir::Inst* inst, InterpStack& stack) {
    const bool cond = ReadScalar(stack, inst->GetArg<ir::Value>(0)) != 0;
    WriteScalar(stack, inst, ReadScalar(stack, inst->GetArg<ir::Value>(cond ? 1 : 2)));
}

void Interpreter::RunCondSelect(ir::Inst* inst, InterpStack& stack) {
    const bool cond = EvalCondition(inst->GetArg<ir::Cond>(0));
    WriteScalar(stack, inst, ReadScalar(stack, inst->GetArg<ir::Value>(cond ? 1 : 2)));
}

void Interpreter::RunBindLabel(ir::Inst* inst, InterpStack& stack) {
    // Marker only; resolved up-front in Interpreter::Run.
}

void Interpreter::RunCallLambda(ir::Inst* inst, InterpStack& stack) {
    auto lambda = inst->GetArg<ir::Lambda>(0);
    std::vector<ir::DataClass> args;
    for (int i = 1; i < 4; i++) {
        if (inst->ArgAt(i).IsValue()) {
            args.emplace_back(inst->GetArg<ir::Value>(i));
        } else if (inst->ArgAt(i).IsImm()) {
            args.emplace_back(inst->GetArg<ir::Imm>(i));
        }
    }
    WriteScalar(stack, inst, CallHostFunc(stack, lambda, args));
}

void Interpreter::RunCallLocation(ir::Inst* inst, InterpStack& stack) {
    // Same as the JIT: treated as a host C-ABI call with params.
    auto lambda = inst->GetArg<ir::Lambda>(0);
    std::vector<ir::DataClass> args;
    for (auto& param : inst->GetArg<ir::Params>(1)) {
        args.emplace_back(param.data);
    }
    WriteScalar(stack, inst, CallHostFunc(stack, lambda, args));
}

void Interpreter::RunCallDynamic(ir::Inst* inst, InterpStack& stack) {
    auto lambda = inst->GetArg<ir::Lambda>(0);
    std::vector<ir::DataClass> args;
    for (auto& param : inst->GetArg<ir::Params>(1)) {
        args.emplace_back(param.data);
    }
    WriteScalar(stack, inst, CallHostFunc(stack, lambda, args));
}

void Interpreter::RunAddPhi(ir::Inst* inst, InterpStack& stack) {
    // HIR phi nodes never appear in the flat decoded blocks the interpreter
    // executes (the JIT's EmitAddPhi is empty as well).
}

void Interpreter::RunNop(ir::Inst* inst, InterpStack& stack) {}

void Interpreter::RunAdvancePC(ir::Inst* inst, InterpStack& stack) {
    // Decode-progress marker; the guest PC is tracked through SetLocation and
    // block terminals, so there is nothing to do (the JIT only flushes its
    // lazy flag state here, which the interpreter keeps eagerly coherent).
}

void Interpreter::RunSetLocation(ir::Inst* inst, InterpStack& stack) {
    auto location = inst->GetArg<ir::Lambda>(0);
    state.current_loc = EvalLambda(stack, location);
}

void Interpreter::RunGetLocation(ir::Inst* inst, InterpStack& stack) {
    WriteScalar(stack, inst, state.current_loc.Value());
}

void Interpreter::RunZero(ir::Inst* inst, InterpStack& stack) {
    if (IsVector(inst->ReturnType())) {
        WriteVec(stack, inst, 0);
    } else {
        WriteScalar(stack, inst, 0);
    }
}

void Interpreter::RunGetHostGPR(ir::Inst* inst, InterpStack& stack) {
    // No static host-register allocation on this configuration
    // (Config::buffers_static_alloc is empty; the JIT also no-ops when the
    // offset arg is 0). Yield 0.
    WriteScalar(stack, inst, 0);
}

void Interpreter::RunGetHostFPR(ir::Inst* inst, InterpStack& stack) {
    // See RunGetHostGPR.
    WriteScalar(stack, inst, 0);
}

void Interpreter::RunSetHostGPR(ir::Inst* inst, InterpStack& stack) {
    // No-op: no static host-register mapping (see RunGetHostGPR).
}

void Interpreter::RunSetHostFPR(ir::Inst* inst, InterpStack& stack) {
    // No-op: no static host-register mapping (see RunGetHostGPR).
}

void Interpreter::RunBitCast(ir::Inst* inst, InterpStack& stack) {
    // Pure re-interpretation: copy the raw 16-byte slot.
    WriteVec(stack, inst, ReadVec(stack, inst->GetArg<ir::Value>(0)));
}

void Interpreter::RunGetOperand(ir::Inst* inst, InterpStack& stack) {
    WriteScalar(stack, inst, EvalOperand(stack, inst->GetArg<ir::Operand>(0)));
}

void Interpreter::RunGetResult(ir::Inst* inst, InterpStack& stack) {
    // Pseudo op chained on arg0's def: copy the referenced value (the JIT
    // moves the allocated register; raw slot copy covers vectors too).
    WriteVec(stack, inst, ReadVec(stack, inst->GetArg<ir::Value>(0)));
}

void Interpreter::RunLoadImm(ir::Inst* inst, InterpStack& stack) {
    // Matches the JIT's Mov(result, imm.Get()).
    WriteScalar(stack, inst, inst->GetArg<ir::Imm>(0).Get());
}

void Interpreter::RunPushRSB(ir::Inst* inst, InterpStack& stack) {
    // TODO: ReturnStackBuffer support; safe to ignore while the optimization
    // is off (same as the JIT).
}

void Interpreter::RunPopRSB(ir::Inst* inst, InterpStack& stack) {
    // TODO: ReturnStackBuffer support; safe to ignore (same as the JIT).
}

void Interpreter::RunAdd(ir::Inst* inst, InterpStack& stack) {
    const u64 l = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 r = EvalOperand(stack, inst->GetArg<ir::Operand>(1));
    WriteScalar(stack, inst, l + r);
}

void Interpreter::RunSub(ir::Inst* inst, InterpStack& stack) {
    const u64 l = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 r = EvalOperand(stack, inst->GetArg<ir::Operand>(1));
    WriteScalar(stack, inst, l - r);
}

void Interpreter::RunAdc(ir::Inst* inst, InterpStack& stack) {
    const u64 l = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 r = EvalOperand(stack, inst->GetArg<ir::Operand>(1));
    const u64 cin = (state.host_cpu_flags >> kHostFlagC) & 1;
    WriteScalar(stack, inst, l + r + cin);
}

void Interpreter::RunSbb(ir::Inst* inst, InterpStack& stack) {
    const u64 l = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 r = EvalOperand(stack, inst->GetArg<ir::Operand>(1));
    // Carry is stored with host (ARM) semantics: C = NOT borrow.
    const u64 bin = 1 - ((state.host_cpu_flags >> kHostFlagC) & 1);
    WriteScalar(stack, inst, l - r - bin);
}

void Interpreter::RunMul(ir::Inst* inst, InterpStack& stack) {
    const u64 l = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 r = EvalOperand(stack, inst->GetArg<ir::Operand>(1));
    WriteScalar(stack, inst, l * r);
}

void Interpreter::RunDiv(ir::Inst* inst, InterpStack& stack) {
    const auto left = inst->GetArg<ir::Value>(0);
    const auto type = left.Type();
    const u32 bits = TypeBits(type);
    const u64 l = ReadScalar(stack, left) & MaskBits(bits);
    const u64 r = EvalOperand(stack, inst->GetArg<ir::Operand>(1)) & MaskBits(bits);
    // Division by zero follows ARM64 host semantics (result = 0, no trap),
    // same as the JIT; x86 guest #DE behaviour is not modelled.
    u64 result{0};
    if (r != 0) {
        if (ir::IsSignValueType(type)) {
            const s64 sl = static_cast<s64>(SignExtendTo(l, bits));
            const s64 sr = static_cast<s64>(SignExtendTo(r, bits));
            if (sr == -1) {
                // Avoid the INT_MIN / -1 UB; ARM64 Sdiv wraps.
                result = (u64(0) - static_cast<u64>(sl)) & MaskBits(bits);
            } else {
                result = static_cast<u64>(sl / sr);
            }
        } else {
            result = l / r;
        }
    }
    WriteScalar(stack, inst, result);
}

void Interpreter::RunZeroExtend32(ir::Inst* inst, InterpStack& stack) {
    // Slots already hold zero-extended values; the write masks to U32.
    WriteScalar(stack, inst, ReadScalar(stack, inst->GetArg<ir::Value>(0)));
}

void Interpreter::RunZeroExtend64(ir::Inst* inst, InterpStack& stack) {
    WriteScalar(stack, inst, ReadScalar(stack, inst->GetArg<ir::Value>(0)));
}

void Interpreter::RunSignExtend(ir::Inst* inst, InterpStack& stack) {
    const auto value = inst->GetArg<ir::Value>(0);
    const u32 bits = TypeBits(value.Type());
    WriteScalar(stack, inst, SignExtendTo(ReadScalar(stack, value), bits));
}

void Interpreter::RunAnd(ir::Inst* inst, InterpStack& stack) {
    const u64 l = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 r = EvalOperand(stack, inst->GetArg<ir::Operand>(1));
    WriteScalar(stack, inst, l & r);
}

void Interpreter::RunOr(ir::Inst* inst, InterpStack& stack) {
    const u64 l = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 r = EvalOperand(stack, inst->GetArg<ir::Operand>(1));
    WriteScalar(stack, inst, l | r);
}

void Interpreter::RunXor(ir::Inst* inst, InterpStack& stack) {
    const u64 l = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 r = EvalOperand(stack, inst->GetArg<ir::Operand>(1));
    WriteScalar(stack, inst, l ^ r);
}

void Interpreter::RunNot(ir::Inst* inst, InterpStack& stack) {
    if (inst->ArgAt(1).IsVoid()) {
        // Unary form: logical not (used for zero checks), result is 0/1.
        WriteScalar(stack, inst, ReadScalar(stack, inst->GetArg<ir::Value>(0)) == 0 ? 1 : 0);
    } else {
        WriteScalar(stack, inst, ~EvalOperand(stack, inst->GetArg<ir::Operand>(1)));
    }
}

void Interpreter::RunAndNot(ir::Inst* inst, InterpStack& stack) {
    const u64 l = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 r = EvalOperand(stack, inst->GetArg<ir::Operand>(1));
    WriteScalar(stack, inst, l & ~r);
}

void Interpreter::RunGetFlags(ir::Inst* inst, InterpStack& stack) {
    // The JIT moves the whole flags register, ignoring the mask argument.
    WriteScalar(stack, inst, state.host_cpu_flags);
}

void Interpreter::RunSaveFlags(ir::Inst* inst, InterpStack& stack) {
    // Pseudo op executed at its position in the instruction list: capture the
    // requested guest flags from the referenced def's operation.
    SaveGuestFlags(stack, inst->GetArg<ir::Value>(0).Def(), inst->GetArg<ir::Flags>(1));
}

void Interpreter::RunTestFlags(ir::Inst* inst, InterpStack& stack) {
    WriteScalar(stack, inst, TestGuestFlags(inst->GetArg<ir::Flags>(0)) ? 1 : 0);
}

void Interpreter::RunTestNotFlags(ir::Inst* inst, InterpStack& stack) {
    WriteScalar(stack, inst, TestGuestFlags(inst->GetArg<ir::Flags>(0)) ? 0 : 1);
}

void Interpreter::RunClearFlags(ir::Inst* inst, InterpStack& stack) {
    ClearGuestFlags(inst->GetArg<ir::Flags>(0));
}

void Interpreter::RunSetCarry(ir::Inst* inst, InterpStack& stack) {
    // Mirrors JitTranslator::EmitSetCarry: set guest CF from a computed bit.
    const bool on = ReadScalar(stack, inst->GetArg<ir::Value>(0)) & 1;
    u64& fl = state.host_cpu_flags;
    fl = on ? (fl | (u64(1) << kHostFlagC)) : (fl & ~(u64(1) << kHostFlagC));
}

void Interpreter::RunSetOverflow(ir::Inst* inst, InterpStack& stack) {
    const bool on = ReadScalar(stack, inst->GetArg<ir::Value>(0)) & 1;
    u64& fl = state.host_cpu_flags;
    fl = on ? (fl | (u64(1) << kHostFlagV)) : (fl & ~(u64(1) << kHostFlagV));
}

void Interpreter::RunLslImm(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = TypeBits(inst->ReturnType());
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(0)) & MaskBits(bits);
    const u64 amount = inst->GetArg<ir::Imm>(1).Get() & (bits - 1);
    WriteScalar(stack, inst, value << amount);
}

void Interpreter::RunLslValue(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = TypeBits(inst->ReturnType());
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(0)) & MaskBits(bits);
    const u64 amount = ReadScalar(stack, inst->GetArg<ir::Value>(1)) & (bits - 1);
    WriteScalar(stack, inst, value << amount);
}

void Interpreter::RunLsrImm(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = TypeBits(inst->ReturnType());
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(0)) & MaskBits(bits);
    const u64 amount = inst->GetArg<ir::Imm>(1).Get() & (bits - 1);
    WriteScalar(stack, inst, value >> amount);
}

void Interpreter::RunLsrValue(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = TypeBits(inst->ReturnType());
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(0)) & MaskBits(bits);
    const u64 amount = ReadScalar(stack, inst->GetArg<ir::Value>(1)) & (bits - 1);
    WriteScalar(stack, inst, value >> amount);
}

void Interpreter::RunAsrImm(ir::Inst* inst, InterpStack& stack) {
    const auto value = inst->GetArg<ir::Value>(0);
    const u32 bits = TypeBits(inst->ReturnType());
    const u64 amount = inst->GetArg<ir::Imm>(1).Get() & (bits - 1);
    const auto extended =
            static_cast<s64>(SignExtendTo(ReadScalar(stack, value), TypeBits(value.Type())));
    WriteScalar(stack, inst, static_cast<u64>(extended >> amount));
}

void Interpreter::RunAsrValue(ir::Inst* inst, InterpStack& stack) {
    const auto value = inst->GetArg<ir::Value>(0);
    const u32 bits = TypeBits(inst->ReturnType());
    const u64 amount = ReadScalar(stack, inst->GetArg<ir::Value>(1)) & (bits - 1);
    const auto extended =
            static_cast<s64>(SignExtendTo(ReadScalar(stack, value), TypeBits(value.Type())));
    WriteScalar(stack, inst, static_cast<u64>(extended >> amount));
}

void Interpreter::RunRorImm(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = TypeBits(inst->ReturnType());
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(0)) & MaskBits(bits);
    const u32 amount = inst->GetArg<ir::Imm>(1).Get() & (bits - 1);
    WriteScalar(stack,
                inst,
                amount ? ((value >> amount) | (value << (bits - amount))) : value);
}

void Interpreter::RunRorValue(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = TypeBits(inst->ReturnType());
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(0)) & MaskBits(bits);
    const u32 amount = ReadScalar(stack, inst->GetArg<ir::Value>(1)) & (bits - 1);
    WriteScalar(stack,
                inst,
                amount ? ((value >> amount) | (value << (bits - amount))) : value);
}

void Interpreter::RunBitExtract(ir::Inst* inst, InterpStack& stack) {
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 lsb = inst->GetArg<ir::Imm>(1).Get();
    const u64 width = inst->GetArg<ir::Imm>(2).Get();
    WriteScalar(stack, inst, (value >> lsb) & MaskBits(u32(width)));
}

void Interpreter::RunBitInsert(ir::Inst* inst, InterpStack& stack) {
    const u64 dest = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 src = ReadScalar(stack, inst->GetArg<ir::Value>(1));
    const u64 lsb = inst->GetArg<ir::Imm>(2).Get();
    const u64 width = inst->GetArg<ir::Imm>(3).Get();
    const u64 mask = MaskBits(u32(width)) << lsb;
    WriteScalar(stack, inst, (dest & ~mask) | ((src << lsb) & mask));
}

void Interpreter::RunBitClear(ir::Inst* inst, InterpStack& stack) {
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 lsb = inst->GetArg<ir::Imm>(1).Get();
    const u64 width = inst->GetArg<ir::Imm>(2).Get();
    WriteScalar(stack, inst, value & ~(MaskBits(u32(width)) << lsb));
}

void Interpreter::RunTestBit(ir::Inst* inst, InterpStack& stack) {
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 bit = inst->GetArg<ir::Imm>(1).Get();
    WriteScalar(stack, inst, (value >> bit) & 1);
}

void Interpreter::RunTestZero(ir::Inst* inst, InterpStack& stack) {
    WriteScalar(stack, inst, ReadScalar(stack, inst->GetArg<ir::Value>(0)) == 0 ? 1 : 0);
}

void Interpreter::RunTestNotZero(ir::Inst* inst, InterpStack& stack) {
    WriteScalar(stack, inst, ReadScalar(stack, inst->GetArg<ir::Value>(0)) != 0 ? 1 : 0);
}

namespace {

template <typename Op>
unsigned __int128 Vec4Binary(unsigned __int128 a, unsigned __int128 b, Op op) {
    unsigned __int128 result = 0;
    for (int i = 0; i < 4; ++i) {
        const u32 la = static_cast<u32>(a >> (i * 32));
        const u32 lb = static_cast<u32>(b >> (i * 32));
        result |= static_cast<unsigned __int128>(op(la, lb)) << (i * 32);
    }
    return result;
}

}  // namespace

void Interpreter::RunVec4Add(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             Vec4Binary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                        ReadVec(stack, inst->GetArg<ir::Value>(1)),
                        [](u32 a, u32 b) { return a + b; }));
}

void Interpreter::RunVec4Sub(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             Vec4Binary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                        ReadVec(stack, inst->GetArg<ir::Value>(1)),
                        [](u32 a, u32 b) { return a - b; }));
}

void Interpreter::RunVec4Mul(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             Vec4Binary(ReadVec(stack, inst->GetArg<ir::Value>(0)),
                        ReadVec(stack, inst->GetArg<ir::Value>(1)),
                        [](u32 a, u32 b) { return a * b; }));
}

void Interpreter::RunVec4And(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             ReadVec(stack, inst->GetArg<ir::Value>(0)) &
                     ReadVec(stack, inst->GetArg<ir::Value>(1)));
}

void Interpreter::RunVec4Or(ir::Inst* inst, InterpStack& stack) {
    WriteVec(stack,
             inst,
             ReadVec(stack, inst->GetArg<ir::Value>(0)) |
                     ReadVec(stack, inst->GetArg<ir::Value>(1)));
}

}  // namespace swift::runtime::backend::interp
