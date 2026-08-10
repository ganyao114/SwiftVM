//
// Created by 甘尧 on 2024/2/23.
//
// IR interpreter: fallback execution engine when the JIT is unavailable and
// executable reference for the IR semantics. The authoritative semantic
// reference is the arm64 JIT (backend/arm64/jit/translator.cpp); wherever the
// two could disagree this implementation follows the JIT, including its
// quirks (documented inline).

#include <unordered_map>
#include <alloca.h>
#include "interpreter.h"
#include <bit>
#include "runtime/common/variant_util.h"
#include "runtime/frontend/x86/x87.h"

namespace swift::x86 {
extern "C" u64 SwiftSse42StrEvalImplicit(u64 a_lo,
                                         u64 a_hi,
                                         u64 b_lo,
                                         u64 b_hi,
                                         u64 imm8);
}

namespace swift::runtime::backend::interp {

using ir::ValueType;

#include "interpreter_internal.h"

namespace {

u64 MaskFor(ValueType type) { return MaskBits(TypeBits(type)); }


}  // namespace

Interpreter::Interpreter(State& state, ir::Block* block) : state(state), block(block) {}

// ---------------------------------------------------------------------------
// Value slot accessors
// ---------------------------------------------------------------------------

u64 Interpreter::ReadScalar(InterpStack& stack, ir::Value value) {
    return GetReg<u64>(stack, value);
}

u64 Interpreter::ReadScalarBits(InterpStack& stack, ir::Value value) {
    return ir::IsFloatValueType(value.Type()) ? static_cast<u64>(ReadVec(stack, value))
                                              : ReadScalar(stack, value);
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
// Block / terminal execution
// ---------------------------------------------------------------------------

HaltReason Interpreter::Run() {
    auto& insts = block->GetInstList();
    // Function-mode instruction ids are global across the RPO, while
    // Block::max_instr_id is only maintained by Block::ReIdInstr for flat
    // block translation. Size from the actual maximum so entering any block
    // in a function cannot index the interpreter stack out of bounds.
    u32 max_inst_id = 0;
    for (auto& inst : insts) {
        max_inst_id = std::max<u32>(max_inst_id, inst.Id());
    }
    const size_t slot_count = (size_t(max_inst_id) + 1) * kSlotStride;
    auto* raw = static_cast<u64*>(alloca(slot_count * sizeof(u64)));
    std::memset(raw, 0, slot_count * sizeof(u64));
    InterpStack stack{raw, slot_count};

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

void Interpreter::RunSse42Str(ir::Inst* inst, InterpStack& stack) {
    const u128 a = ReadVec(stack, inst->GetArg<ir::Value>(0));
    const u128 b = ReadVec(stack, inst->GetArg<ir::Value>(1));
    WriteScalar(stack,
                inst,
                swift::x86::SwiftSse42StrEvalImplicit(
                        static_cast<u64>(a),
                        static_cast<u64>(a >> 64),
                        static_cast<u64>(b),
                        static_cast<u64>(b >> 64),
                        inst->GetArg<ir::Imm>(2).Get()));
}

void Interpreter::RunX87Op(ir::Inst* inst, InterpStack& stack) {
    const auto context = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const auto command = inst->GetArg<ir::Imm>(1).Get();
    const auto address = ReadScalar(stack, inst->GetArg<ir::Value>(2));
    WriteScalar(stack, inst, swift::x86::X87Dispatch(context, command, address));
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

void Interpreter::RunNeg(ir::Inst* inst, InterpStack& stack) {
    WriteScalar(stack, inst, u64{0} - ReadScalar(stack, inst->GetArg<ir::Value>(0)));
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

void Interpreter::RunZeroExtend32To64(ir::Inst* inst, InterpStack& stack) {
    // Unlike ZeroExtend32, this result is U64-typed; apply the opcode's
    // 32-bit semantic mask explicitly before the U64 WriteScalar.
    WriteScalar(stack, inst, ReadScalar(stack, inst->GetArg<ir::Value>(0)) & UINT32_MAX);
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

void Interpreter::RunBranchOnlyFlags(ir::Inst* inst, InterpStack& stack) {
    // The interpreter has no ambient host NZCV lifetime to exploit. Preserve
    // the exact architectural result for the following LocalCondSet; the
    // function-level liveness proof guarantees no successor observes it.
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

void Interpreter::RunInvertCarry(ir::Inst* inst, InterpStack& stack) {
    state.host_cpu_flags ^= u64(1) << kHostFlagC;
}

void Interpreter::RunPublishFCmpFlags(ir::Inst* inst, InterpStack& stack) {
    const u64 packed = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const bool compact = inst->GetArg<ir::Imm>(1).Get() != 0;
    constexpr u64 replaced =
            (u64(1) << kHostFlagN) |
            (u64(1) << kHostFlagZ) |
            (u64(1) << kHostFlagC) |
            (u64(1) << kHostFlagV) |
            (u64(1) << kHostAF) |
            (u64(0xff) << kHostParityByte);
    u64& flags = state.host_cpu_flags;
    flags &= ~replaced;
    // The compact JIT path uses AXFLAG, whose C is the inverse of x86 CF.
    // Mirror that representation here because the decoder records the same
    // unit-local carry polarity for all downstream consumers.
    flags |= ((packed & 1) ^ u64(compact)) << kHostFlagC;
    flags |= ((packed >> 2) & 1) << kHostFlagZ;
    flags |= (((packed >> 1) & 1) ^ 1) << kHostParityByte;
}

void Interpreter::RunLocalCondSet(ir::Inst* inst, InterpStack& stack) {
    WriteScalar(stack, inst, EvalCondition(inst->GetArg<ir::Cond>(0)) ? 1 : 0);
}

void Interpreter::RunLocalParitySet(ir::Inst* inst, InterpStack& stack) {
    const u8 value = static_cast<u8>(ReadScalar(stack, inst->GetArg<ir::Value>(0)));
    bool even = (std::popcount(value) & 1u) == 0;
    if (inst->GetArg<ir::Imm>(1).Get() != 0) {
        even = !even;
    }
    WriteScalar(stack, inst, even ? 1 : 0);
}

void Interpreter::RunBranchOnlyEdges(ir::Inst* inst, InterpStack& stack) {}

void Interpreter::RunFCmpCondSet(ir::Inst* inst, InterpStack& stack) {
    const u64 packed = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const bool less = (packed & 7) == 1;
    const bool equal = (packed & 7) == 4;
    const bool unordered = (packed & 7) == 7;
    const bool n = less;
    const bool z = equal;
    const bool c = !less;
    const bool v = unordered;
    bool take = false;
    switch (inst->GetArg<ir::Cond>(1)) {
        case ir::Cond::EQ: take = z; break;
        case ir::Cond::NE: take = !z; break;
        case ir::Cond::CS: take = c; break;
        case ir::Cond::CC: take = !c; break;
        case ir::Cond::MI: take = n; break;
        case ir::Cond::PL: take = !n; break;
        case ir::Cond::VS: take = v; break;
        case ir::Cond::VC: take = !v; break;
        case ir::Cond::HI: take = c && !z; break;
        case ir::Cond::LS: take = !c || z; break;
        case ir::Cond::GE: take = n == v; break;
        case ir::Cond::LT: take = n != v; break;
        case ir::Cond::GT: take = !z && n == v; break;
        case ir::Cond::LE: take = z || n != v; break;
        case ir::Cond::AL:
        case ir::Cond::NV:
        default: take = true; break;
    }
    WriteScalar(stack, inst, take ? 1 : 0);
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
    WriteScalar(stack, inst, amount ? ((value >> amount) | (value << (bits - amount))) : value);
}

void Interpreter::RunRorValue(ir::Inst* inst, InterpStack& stack) {
    const u32 bits = TypeBits(inst->ReturnType());
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(0)) & MaskBits(bits);
    const u32 amount = ReadScalar(stack, inst->GetArg<ir::Value>(1)) & (bits - 1);
    WriteScalar(stack, inst, amount ? ((value >> amount) | (value << (bits - amount))) : value);
}

void Interpreter::RunByteSwap(ir::Inst* inst, InterpStack& stack) {
    const u64 value = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u32 width = inst->GetArg<ir::Imm>(1).Get();
    switch (width) {
        case 16:
            WriteScalar(stack, inst, __builtin_bswap16(static_cast<u16>(value)));
            break;
        case 32:
            WriteScalar(stack, inst, __builtin_bswap32(static_cast<u32>(value)));
            break;
        case 64:
            WriteScalar(stack, inst, __builtin_bswap64(value));
            break;
        default:
            PANIC("invalid byte-swap width {}", width);
    }
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



// Upper 64 bits of a 64x64 product; Imm selects signedness.  Kept in step with
// EmitMulHigh -- a one-sided change here produces a back end divergence, which
// is worse than the defect it would fix.
void Interpreter::RunMulHigh(ir::Inst* inst, InterpStack& stack) {
    const u64 l = ReadScalar(stack, inst->GetArg<ir::Value>(0));
    const u64 r = ReadScalar(stack, inst->GetArg<ir::Value>(1));
    u64 high;
    if (inst->GetArg<ir::Imm>(2).Get() != 0) {
        const auto product = static_cast<__int128>(static_cast<s64>(l)) *
                             static_cast<__int128>(static_cast<s64>(r));
        high = static_cast<u64>(static_cast<unsigned __int128>(product) >> 64);
    } else {
        high = static_cast<u64>(
                (static_cast<unsigned __int128>(l) * static_cast<unsigned __int128>(r)) >> 64);
    }
    WriteScalar(stack, inst, high);
}

// Condition -> 0/1.  This is RunCondSelect with the two operands frozen at 1
// and 0, and it must stay that way: EvalCondition is the single definition of
// what each ir::Cond means for this back end, so routing CondSet through it is
// what keeps the interpreter and EmitCondSet (CSET) from drifting apart.
void Interpreter::RunCondSet(ir::Inst* inst, InterpStack& stack) {
    // WriteScalar drops the store when the return type is VOID, and a Cond
    // argument gives Inst::SetArg nothing to infer one from -- so a front end
    // that forgets SetType would leave the condition reading as stack garbage
    // here while the JIT kept working.  Assert rather than silently diverge.
    ASSERT(inst->ReturnType() != ir::ValueType::VOID);
    WriteScalar(stack, inst, EvalCondition(inst->GetArg<ir::Cond>(0)) ? 1 : 0);
}

}  // namespace swift::runtime::backend::interp
