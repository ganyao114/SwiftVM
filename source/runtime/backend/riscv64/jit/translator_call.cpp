#include "translator.h"

#include <utility>
#include "runtime/backend/context.h"
#include "runtime/common/div128.h"
#include "runtime/common/host_pair_result.h"
#include "runtime/frontend/x86/sse42str_helper.h"
#include "runtime/frontend/x86/x87.h"

namespace swift::runtime::backend::riscv64 {

using namespace biscuit;
using O = ir::OpCode;

namespace {

template <typename Sequence> struct HostCall;
template <size_t... I> struct HostCall<std::index_sequence<I...>> {
    static u64 Invoke(State* state, u64 target, decltype((void)I, u64{})... input) noexcept {
        try {
            std::array<u64, 8> args{};
            ((args[I] = input), ...);
            using Function = u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64);
            return reinterpret_cast<Function>(target)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
        } catch (...) {
            state->halt_reason = HaltReason::IllegalCode;
            return 0;
        }
    }
};

template <size_t... I> auto HostCalls(std::index_sequence<I...>) {
    return std::array<u64, sizeof...(I)>{reinterpret_cast<u64>(&HostCall<std::make_index_sequence<I>>::Invoke)...};
}

HostPairResult QueryPair(State* state, u64 target, u64 first, u64 second, u64 features) noexcept {
    try { return reinterpret_cast<HostPairResult (*)(u64, u64, u64)>(target)(first, second, features); }
    catch (...) { state->halt_reason = HaltReason::IllegalCode; return {}; }
}

}  // namespace

bool JitTranslator::EmitNativeCall(ir::Inst* inst) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::CallLambda: case O::CallLocation: case O::CallDynamic:
        case O::X87Op: case O::Sse42Str: case O::Cpuid: case O::CpuidUpper:
        case O::Div128: case O::Div128Remainder: break;
        default: return false;
    }
    if (op == O::CpuidUpper || op == O::Div128Remainder) return true;
    auto& as = context.GetMasm();
    context.PublishFlags();
    const bool paired = op == O::Cpuid || op == O::Div128;
    const bool vector_return = ir::IsFloatValueType(inst->ReturnType());
    const auto pair = paired || vector_return ? context.ResultPair(inst) : std::array<GPR, 2>{x0, x0};
    const auto result = paired || vector_return ? pair[0] : context.ResultRegister(inst);
    if (paired) {
        ir::Inst* secondary{};
        const auto secondary_op = op == O::Cpuid ? O::CpuidUpper : O::Div128Remainder;
        const auto pseudos = inst->GetPseudoOperations(secondary_op);
        if (!pseudos.empty()) secondary = pseudos.front();
        if (op == O::Cpuid) {
            context.SaveVectorsForCall();
            as.MV(a0, state); context.HostAddress(a1, inst->GetArg<ir::Imm>(3).Get());
            context.Read(a2, inst->GetArg<ir::Value>(0)); context.Read(a3, inst->GetArg<ir::Value>(1));
            as.LI(a4, inst->GetArg<ir::Imm>(2).Get());
            context.HostAddress(t0, reinterpret_cast<u64>(&QueryPair)); context.LeaveFloatMode(); as.JALR(t0); context.EnterFloatMode();
            as.MV(pair[0], a0); as.MV(pair[1], a1);
            context.ReloadFlags(); context.RestoreVectorsAfterCall();
        } else {
            const bool sign = inst->GetArg<ir::Imm>(3).Get() != 0;
            context.Read(a0, inst->GetArg<ir::Value>(0)); context.Read(a1, inst->GetArg<ir::Value>(1));
            context.Read(a2, inst->GetArg<ir::Value>(2));
            Label zero, slow, done;
            as.BEQ(a2, x0, &zero);
            if (sign) { as.SRAI(t2, a1, 63); as.BNE(a0, t2, &slow); }
            else as.BNE(a0, x0, &slow);
            if (sign) { as.DIV(pair[0], a1, a2); as.REM(pair[1], a1, a2); }
            else { as.DIVU(pair[0], a1, a2); as.REMU(pair[1], a1, a2); }
            as.J(&done); as.Bind(&slow);
            context.SaveVectorsForCall();
            context.HostAddress(t0, reinterpret_cast<u64>(sign ? &DivideSigned128 : &DivideUnsigned128));
            context.LeaveFloatMode(); as.JALR(t0); context.EnterFloatMode();
            as.MV(pair[0], a0); as.MV(pair[1], a1);
            context.ReloadFlags(); context.RestoreVectorsAfterCall(); as.J(&done);
            as.Bind(&zero); as.MV(pair[0], x0); as.MV(pair[1], x0); as.Bind(&done);
            context.ResetVectorType();
        }
        context.Write(inst, pair[0]);
        if (secondary) context.Write(secondary, pair[1]);
    } else {
        std::array<ir::DataClass, 8> args{};
        u32 count{};
        const auto append = [&](const ir::DataClass& arg) { if (!arg.Null() && count < args.size()) args[count++] = arg; };
        context.SaveVectorsForCall();
        if (op == O::CallLambda || op == O::CallLocation || op == O::CallDynamic) {
            const auto target = inst->GetArg<ir::Lambda>(0);
            if (target.IsValue()) context.Read(a1, target.GetValue()); else context.HostAddress(a1, target.GetImm().Get());
            if (op == O::CallLambda) {
                for (u32 arg = 1; arg < 4; ++arg) {
                    if (inst->ArgAt(arg).IsValue()) append(inst->GetArg<ir::Value>(arg));
                    else if (inst->ArgAt(arg).IsImm()) append(inst->GetArg<ir::Imm>(arg));
                }
            } else for (const auto& param : inst->GetArg<ir::Params>(1)) append(param.data);
        } else if (op == O::X87Op) {
            context.HostAddress(a1, reinterpret_cast<u64>(&swift::x86::X87Dispatch));
            append(inst->GetArg<ir::Value>(0)); append(inst->GetArg<ir::Imm>(1)); append(inst->GetArg<ir::Value>(2));
        } else {
            context.HostAddress(a1, reinterpret_cast<u64>(&swift::x86::SwiftSse42StrEvalImplicit));
            // A vector argument occupies two integer C-ABI parameters here.
            context.ReadPart(a2, inst->GetArg<ir::Value>(0), 0); context.ReadPart(a3, inst->GetArg<ir::Value>(0), 1);
            context.ReadPart(a4, inst->GetArg<ir::Value>(1), 0); context.ReadPart(a5, inst->GetArg<ir::Value>(1), 1);
            as.LI(a6, inst->GetArg<ir::Imm>(2).Get()); count = 5;
        }
        constexpr std::array registers{a2, a3, a4, a5, a6, a7};
        if (count > 6) as.ADDI(sp, sp, -16);
        if (op != O::Sse42Str) for (u32 arg = 0; arg < count; ++arg) {
            if (arg < registers.size()) context.Data(registers[arg], args[arg]);
            else { context.Data(t2, args[arg]); as.SD(t2, (arg - 6) * 8, sp); }
        }
        static const auto calls = HostCalls(std::make_index_sequence<9>{});
        as.MV(a0, state); context.HostAddress(t0, calls[count]);
        context.LeaveFloatMode(); as.JALR(t0); context.EnterFloatMode();
        if (count > 6) as.ADDI(sp, sp, 16);
        if (result != a0) as.MV(result, a0);
        context.ReloadFlags(); context.RestoreVectorsAfterCall();
        if (vector_return) { as.MV(pair[1], x0); context.WritePair(inst, pair); }
        else context.Write(inst, result);
    }
    context.Load(t0, state, state_offset_halt_reason, 4);
    context.BranchZero(t0, epilogue, false);
    return true;
}

}  // namespace swift::runtime::backend::riscv64
