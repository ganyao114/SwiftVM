#include "wide.h"

#include <memory>
#include <limits>
#include <stdexcept>
#include "runtime/common/variant_util.h"

namespace swift::runtime::backend::riscv64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;

bool Wide(ir::Value value) { return value.Type() == T::V256; }
bool Split(ir::Inst* inst) {
    if (inst->ReturnType() == T::V256) return true;
    if (inst->GetOp() == O::StoreUniform || inst->GetOp() == O::StoreMemory ||
        inst->GetOp() == O::StoreMemoryTSO || inst->GetOp() == O::StoreLocal)
        return Wide(inst->GetArg<ir::Value>(1));
    if (inst->GetOp() == O::SetHostFPR) return Wide(inst->GetArg<ir::Value>(0));
    return false;
}

void ValidateWideResult(ir::Inst* inst) {
    if (inst->ReturnType() != T::V256) return;
    const auto op = inst->GetOp();
    if ((op >= O::Vec4Add && op <= O::VecFRoundInt) &&
        op != O::VecExtract64 && op != O::VecExtract16 && op != O::VecMovMask && op != O::VecFCmp)
        return;
    switch (op) {
        case O::LoadLocal: case O::LoadUniform: case O::LoadMemory: case O::LoadMemoryTSO:
        case O::Zero: case O::LoadImm: case O::BitCast: case O::GetResult:
        case O::Select: case O::SelectZero: case O::CondSelect: case O::AddPhi:
        case O::GetHostFPR: case O::CompareAndSwap128:
        case O::CallLambda: case O::CallLocation: case O::CallDynamic:
        case O::X87Op: case O::Sse42Str: return;
        default: throw std::runtime_error("RV64 V256 result is invalid for this scalar opcode");
    }
}

class Builder {
public:
    explicit Builder(ir::Block* input) : input(input) {
        plan.body = new ir::Block(input->GetStartLocation());
        for (auto& inst : input->GetInstList()) {
            ValidateWideResult(&inst);
            const auto type = inst.ReturnType() == T::V256 ? T::V128 : inst.ReturnType();
            definitions[&inst] = {New(inst.GetOp(), type), Split(&inst) ? New(inst.GetOp(), type) : nullptr};
            plan.instructions[&inst] = definitions.at(&inst)[0];
        }
    }
    ~Builder() { for (auto& [inst, owner] : owned) inst->ReleaseArgs(); }
    WidePlan Run() {
        for (auto& inst : input->GetInstList()) Lower(&inst);
        plan.body->SetTerminal(Terminal(input->GetTerminal()));
        if (plan.body->GetInstList().size() > std::numeric_limits<u16>::max())
            throw std::runtime_error("RV64 V256 expansion exceeds the IR instruction ID limit");
        plan.body->ReIdInstr();
        return std::move(plan);
    }
private:
    ir::Inst* New(O op, T type) {
        auto value = std::make_unique<ir::Inst>(op); value->SetReturn(type);
        auto* ptr = value.get(); owned.emplace(ptr, std::move(value)); return ptr;
    }
    void Append(ir::Inst* inst) {
        plan.body->AppendInst(inst); owned.at(inst).release(); owned.erase(inst);
    }
    template<class... Args> ir::Value Emit(O op, T type, const Args&... args) {
        auto* inst = New(op, type); inst->SetArgs(args...); Append(inst); return ir::Value{inst};
    }
    ir::Value Part(ir::Value value, u32 part) const {
        auto found = definitions.find(value.Def());
        if (found == definitions.end()) throw std::runtime_error("RV64 wide input has no local definition");
        auto* inst = found->second[Wide(value) ? part : 0];
        if (!inst) throw std::runtime_error("RV64 wide cast cannot invent an undefined upper half");
        return ir::Value{inst}.SetCastType(Wide(value) ? T::V128 : value.Type());
    }
    ir::Value Zero() { return Emit(O::Zero, T::V128); }
    void Copy(ir::Inst* target, ir::Value source) {
        target->SetInst(O::BitCast, source); target->SetReturn(T::V128); Append(target);
    }
    void Clear(ir::Inst* target) { target->SetInst(O::Zero); target->SetReturn(T::V128); Append(target); }
    void Args(ir::Inst* target, ir::Inst* original, u32 part) {
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = original->ArgAt(slot);
            switch (arg.GetType()) {
                case ir::ArgType::Void: break;
                case ir::ArgType::Value: target->SetArg(slot, Part(arg.Get<ir::Value>(), part)); break;
                case ir::ArgType::Imm: target->SetArg(slot, arg.Get<ir::Imm>()); break;
                case ir::ArgType::Cond: target->SetArg(slot, arg.Get<ir::Cond>()); break;
                case ir::ArgType::Flags: target->SetArg(slot, arg.Get<ir::Flags>()); break;
                case ir::ArgType::Operand: target->SetArg(slot, arg.Get<ir::Operand::Op>()); break;
                case ir::ArgType::Local: {
                    auto local = arg.Get<ir::Local>();
                    if (local.type == T::V256) { local.type = T::V128; local.id += part * 2; }
                    target->SetArg(slot, local); break;
                }
                case ir::ArgType::Uniform: {
                    auto uniform = arg.Get<ir::Uniform>();
                    target->SetArg(slot, ir::Uniform{uniform.GetOffset() + part * 16,
                            uniform.GetType() == T::V256 ? T::V128 : uniform.GetType()}); break;
                }
                case ir::ArgType::Lambda: {
                    auto lambda = arg.Get<ir::Lambda>();
                    if (lambda.IsValue()) lambda.GetValue() = Part(lambda.GetValue(), part);
                    target->SetArg(slot, lambda); break;
                }
                case ir::ArgType::Params: {
                    ir::Params params;
                    for (const auto& param : arg.Get<ir::Params>()) {
                        if (param.data.IsValue()) params.Push(Part(param.data.value, part));
                        else params.Push(part ? ir::Imm{u64{0}} : param.data.imm);
                    }
                    target->SetArg(slot, params); break;
                }
            }
        }
    }
    ir::Terminal Terminal(const ir::Terminal& value) {
        return VisitVariant<ir::Terminal>(value, [&](const auto& term) -> ir::Terminal {
            using K = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<K, ir::terminal::If>)
                return ir::terminal::If{ir::BOOL{Part(term.cond, 0)}, Terminal(term.then_), Terminal(term.else_)};
            else if constexpr (std::is_same_v<K, ir::terminal::Condition>)
                return ir::terminal::Condition{term.cond, Terminal(term.then_), Terminal(term.else_)};
            else if constexpr (std::is_same_v<K, ir::terminal::Switch>) {
                auto cases = term.cases;
                for (auto& item : cases) item.then = Terminal(item.then);
                return ir::terminal::Switch{Part(term.value, 0), cases};
            } else if constexpr (std::is_same_v<K, ir::terminal::CheckHalt>)
                return ir::terminal::CheckHalt{Terminal(term.else_)};
            else return term;
        });
    }
    bool ScalarFloat(ir::Inst* inst) {
        const auto op = inst->GetOp();
        if (op >= O::VecFAddScalar32 && op <= O::VecFDivScalar64) return true;
        if (op == O::VecFUnary || op == O::VecFMinMax || op == O::VecFCmpMask || op == O::VecFRoundInt)
            return inst->GetArg<ir::Imm>(4).Get() != 0;
        return false;
    }
    void Lower(ir::Inst* original) {
        const auto op = original->GetOp();
        auto [low, high] = definitions.at(original);
        const auto source = [&](u32 argument, u32 part) { return Part(original->GetArg<ir::Value>(argument), part); };
        if (op == O::VecMovMask && Wide(original->GetArg<ir::Value>(0))) {
            const auto bits = original->GetArg<ir::Imm>(1);
            auto first = Emit(op, original->ReturnType(), source(0, 0), bits);
            auto second = Emit(op, original->ReturnType(), source(0, 1), bits);
            auto shifted = Emit(O::LslImm, original->ReturnType(), second, ir::Imm{u8(128 / bits.Get())});
            low->SetInst(O::Or, first, ir::Operand{shifted}); Append(low); return;
        }
        if ((op == O::VecExtract64 || op == O::VecExtract16) && Wide(original->GetArg<ir::Value>(0))) {
            const u32 lanes = op == O::VecExtract64 ? 2 : 8;
            const u32 lane = original->GetArg<ir::Imm>(1).Get();
            if (lane >= lanes * 2) throw std::runtime_error("invalid RV64 V256 extract lane");
            low->SetArgs(source(0, lane / lanes), ir::Imm{u8(lane % lanes)}); Append(low); return;
        }
        if (op == O::VecFCvtPacked && Wide(original->GetArg<ir::Value>(0))) {
            const u32 kind = original->GetArg<ir::Imm>(1).Get();
            if (kind == 4 || kind == 5 || kind == 7) {
                auto first = Emit(op, T::V128, source(0, 0), ir::Imm{u8(kind)});
                auto second = Emit(op, T::V128, source(0, 1), ir::Imm{u8(kind)});
                low->SetInst(O::VecZip, first, second, ir::Imm{u8{64}}, ir::Imm{u8{0}});
                Append(low); if (high) Clear(high); return;
            }
        }
        if (!high) { Args(low, original, 0); Append(low); return; }
        // Generic packed IR uses the complete value width. Explicit shuffle
        // controls and AES/SHA rounds act on their 128-bit lane groups.
        if (op == O::VecZip || op == O::VecPack) {
            for (u32 part = 0; part < 2; ++part) {
                auto* target = part ? high : low;
                if (op == O::VecZip) {
                    const u32 input_part = original->GetArg<ir::Imm>(3).Get() != 0;
                    target->SetArgs(source(0, input_part), source(1, input_part), original->GetArg<ir::Imm>(2), ir::Imm{u8(part)});
                } else target->SetArgs(source(part, 0), source(part, 1), original->GetArg<ir::Imm>(2), original->GetArg<ir::Imm>(3));
                Append(target);
            }
            return;
        }
        if (op == O::VecUnzip) {
            low->SetArgs(source(0, 0), source(0, 1), original->GetArg<ir::Imm>(2), original->GetArg<ir::Imm>(3));
            high->SetArgs(source(1, 0), source(1, 1), original->GetArg<ir::Imm>(2), original->GetArg<ir::Imm>(3));
            Append(low); Append(high); return;
        }
        if (op == O::VecInsert16) {
            const u32 lane = original->GetArg<ir::Imm>(2).Get();
            if (lane >= 16) throw std::runtime_error("invalid RV64 V256 insert lane");
            for (u32 part = 0; part < 2; ++part) {
                auto* target = part ? high : low;
                if (part != lane / 8) Copy(target, source(0, part));
                else { target->SetArgs(source(0, part), source(1, 0), ir::Imm{u8(lane % 8)}); Append(target); }
            }
            return;
        }
        if (op == O::VecByteShift || op == O::VecExtractBytes) {
            const u32 count = original->GetArg<ir::Imm>(2).Get();
            if (count > 32 || (op == O::VecExtractBytes && count >= 32))
                throw std::runtime_error("invalid RV64 V256 byte displacement");
            const bool left = op == O::VecByteShift && original->GetArg<ir::Imm>(3).Get() != 0;
            auto zero = Zero();
            const std::array<ir::Value, 4> parts{source(0, 0), source(0, 1),
                op == O::VecExtractBytes ? source(1, 0) : zero,
                op == O::VecExtractBytes ? source(1, 1) : zero};
            for (u32 part = 0; part < 2; ++part) {
                auto* target = part ? high : low;
                if (left) {
                    if (count == 32 || (count >= 16 && part == 0)) { Clear(target); continue; }
                    if (count == 0) { Copy(target, parts[part]); continue; }
                    if (count == 16) { Copy(target, parts[0]); continue; }
                    auto a = part == 0 || count > 16 ? zero : parts[0];
                    auto b = parts[count > 16 ? 0 : part];
                    target->SetInst(O::VecExtractBytes, a, b, ir::Imm{u8(16 - count % 16)});
                } else {
                    const u32 start = part + count / 16, offset = count % 16;
                    if (start >= 4 || (op == O::VecByteShift && start >= 2)) { Clear(target); continue; }
                    if (!offset) { Copy(target, parts[start]); continue; }
                    target->SetInst(O::VecExtractBytes, parts[start], parts[start + 1], ir::Imm{u8(offset)});
                }
                Append(target);
            }
            return;
        }
        if (op == O::VecShuffle32Indexed) {
            auto delta = Emit(O::VecLoadConst, T::V128, ir::Imm{u64{0x1010101010101010ULL}}, ir::Imm{u64{0x1010101010101010ULL}});
            for (u32 part = 0; part < 2; ++part) {
                auto index = source(1, part);
                auto adjusted = Emit(O::VecSub, T::V128, index, delta, ir::Imm{u8{8}});
                auto a = Emit(op, T::V128, source(0, 0), index);
                auto b = Emit(op, T::V128, source(0, 1), adjusted);
                auto* target = part ? high : low; target->SetInst(O::VecOr, a, b); Append(target);
            }
            return;
        }
        if (op == O::VecFCvtPacked) {
            const u32 kind = original->GetArg<ir::Imm>(1).Get();
            if (kind == 1 || kind == 6) {
                auto input = source(0, 0);
                auto upper = Emit(O::VecByteShift, T::V128, input, Zero(), ir::Imm{u8{8}}, ir::Imm{u8{0}});
                low->SetArgs(input, ir::Imm{u8(kind)}); high->SetArgs(upper, ir::Imm{u8(kind)});
                Append(low); Append(high); return;
            }
        }
        Args(low, original, 0);
        // Scalar floating operations merge the untouched upper half. Scalar
        // conversions and immediates zero it; packed operations lower twice.
        if (ScalarFloat(original)) {
            Append(low);
            const u32 merge = op == O::VecFUnary || op == O::VecFRoundInt ? 1 : 0;
            if (Wide(original->GetArg<ir::Value>(merge))) Copy(high, source(merge, 1)); else Clear(high);
            return;
        }
        if (op == O::LoadImm || op == O::VecLoadConst ||
            ((op == O::BitCast || op == O::GetResult) && !Wide(original->GetArg<ir::Value>(0))) ||
            op == O::VecFCvtScalar || op == O::VecFCvtIntToFloat || op == O::VecFCvtFloatToInt ||
            op == O::CompareAndSwap128 || op == O::CallLambda || op == O::CallLocation || op == O::CallDynamic ||
            op == O::Cpuid || op == O::CpuidUpper || op == O::X87Op || op == O::Sse42Str ||
            op == O::Div128 || op == O::Div128Remainder) {
            Append(low); Clear(high); return;
        }
        Args(high, original, 1);
        if (op == O::VecDup64) high->SetArg(0, source(0, 0));
        if (op == O::VecShiftLeft || op == O::VecShiftRight || op == O::VecShiftRightArithmetic)
            high->SetArg(1, source(1, 0));
        if (op == O::GetHostFPR) high->SetArg(1, ir::Imm{original->GetArg<ir::Imm>(1).Get() + 16});
        if (op == O::SetHostFPR) high->SetArg(2, ir::Imm{original->GetArg<ir::Imm>(2).Get() + 16});
        if (op == O::LoadMemory || op == O::LoadMemoryTSO || op == O::StoreMemory || op == O::StoreMemoryTSO) {
            plan.memory_extents[low] = 32; plan.memory_continuations.insert(high);
        }
        Append(low); Append(high);
    }
    ir::Block* input;
    WidePlan plan;
    std::unordered_map<ir::Inst*, std::array<ir::Inst*, 2>> definitions;
    std::unordered_map<ir::Inst*, std::unique_ptr<ir::Inst>> owned;
};
} // namespace

bool WidePlan::Needed(ir::Block* block) {
    for (auto& inst : block->GetInstList()) {
        if (Split(&inst)) return true;
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot)
            if (inst.ArgAt(slot).IsValue() && Wide(inst.ArgAt(slot).Get<ir::Value>())) return true;
    }
    return false;
}
WidePlan WidePlan::Build(ir::Block* block) { return Builder{block}.Run(); }
} // namespace swift::runtime::backend::riscv64
