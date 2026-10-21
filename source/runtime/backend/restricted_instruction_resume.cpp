#include "restricted_instruction_resume.h"
#include "runtime/backend/module.h"
#include "runtime/common/variant_util.h"
#include "runtime/ir/block.h"
#include "runtime/backend/riscv64/jit/jit_context.h"
#include <algorithm>
#include <limits>

namespace swift::runtime::backend {

InstructionCodePin::~InstructionCodePin() {
    if (module) module->ReleaseInstructionPin(allocation, allocation_id);
}

bool ValidateInstructionResumeHomes(const InstructionResumeDescriptor& d) {
    if (!d.original || !d.emitted || d.completion_pc != d.emitted->GetEndLocation().Value()) return false;
    auto& insts = d.emitted->GetInstList();
    const auto find = [&](u32 id) -> ir::Inst* {
        ir::Inst* result{};
        for (auto& inst : insts) if (inst.Id() == id) {
            if (result) return nullptr;
            result = &inst;
        }
        return result;
    };
    auto* head = find(d.head_id);
    if (!head || head->GetOp() != ir::OpCode::BindLabel ||
        std::count_if(insts.begin(), insts.end(), [](const auto& inst) {
            return inst.GetOp() == ir::OpCode::AdvancePC;
        }) != 1) return false;
    u32 payload_parts{};
    std::vector<u32> seen;
    for (const auto& home : d.live) {
        if ((home.type != ir::ValueType::U64 && home.type != ir::ValueType::V32 &&
             home.type != ir::ValueType::V64 && home.type != ir::ValueType::V128) ||
            home.value_id >= d.head_id ||
            home.value_id > std::numeric_limits<u32>::max() / riscv64::kValueStride ||
            home.offset != home.value_id * riscv64::kValueStride ||
            home.bytes != ir::GetValueSizeByte(home.type) ||
            home.payload_slot != payload_parts || home.parts > kRestrictedResumeParts - payload_parts ||
            std::find(seen.begin(), seen.end(), home.value_id) != seen.end()) return false;
        auto* value = find(home.value_id);
        if (!value || value->ReturnType() != home.type) return false;
        // ZeroHigh follows BitCast/GetResult producers. Prove that chain stays
        // inside the emitted IR and terminates before calling the actual rule.
        auto* producer = value;
        size_t depth{};
        while (producer->GetOp() == ir::OpCode::BitCast || producer->GetOp() == ir::OpCode::GetResult) {
            if (++depth > insts.size() || !producer->ArgAt(0).IsValue()) return false;
            producer = producer->GetArg<ir::Value>(0).Def();
            if (!producer || find(producer->Id()) != producer) return false;
        }
        if (home.parts != (riscv64::JitContext::ZeroHigh(value) ? 1u : 2u)) return false;
        payload_parts += home.parts;
        seen.push_back(home.value_id);
    }
    return payload_parts == d.payload_parts;
}

u64 InstructionIRIdentity(ir::Block& block) {
    u64 hash = 1469598103934665603ULL;
    const auto mix = [&](u64 value) { for (u32 i = 0; i < 8; ++i) { hash = (hash ^ (value & 255)) * 1099511628211ULL; value >>= 8; } };
    mix(block.GetStartLocation().Value());
    mix(block.GetEndLocation().Value());
    if (!block.HasTerminal() || !VisitVariant<bool>(block.GetTerminal(), [](const auto& terminal) {
        return std::is_same_v<std::decay_t<decltype(terminal)>, ir::terminal::ReturnToHost>;
    })) return 0;
    for (auto& inst : block.GetInstList()) {
        mix(inst.Id()); mix(static_cast<u64>(inst.GetOp())); mix(static_cast<u64>(inst.ReturnType()));
        for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
            auto& arg = inst.ArgAt(slot);
            mix(static_cast<u64>(arg.GetType()));
            const auto value = [&](ir::Value v) { mix(reinterpret_cast<u64>(v.Def())); mix(static_cast<u64>(v.Type())); };
            const auto imm = [&](ir::Imm v) { mix(v.Get()); mix(static_cast<u64>(v.GetType())); };
            switch (arg.GetType()) {
                case ir::ArgType::Void: break;
                case ir::ArgType::Value: value(arg.Get<ir::Value>()); break;
                case ir::ArgType::Imm: imm(arg.Get<ir::Imm>()); break;
                case ir::ArgType::Uniform: { auto v = arg.Get<ir::Uniform>(); mix(v.GetOffset()); mix(static_cast<u64>(v.GetType())); break; }
                case ir::ArgType::Local: { auto v = arg.Get<ir::Local>(); mix(v.id); mix(static_cast<u64>(v.type)); break; }
                case ir::ArgType::Cond: mix(static_cast<u64>(arg.Get<ir::Cond>())); break;
                case ir::ArgType::Flags: mix(static_cast<u64>(arg.Get<ir::Flags>())); break;
                case ir::ArgType::Operand: { auto v = arg.Get<ir::OperandOp>(); mix(v.type); mix(v.shift_ext); break; }
                case ir::ArgType::Lambda: {
                    auto v = arg.Get<ir::Lambda>(); mix(v.IsValue());
                    if (v.IsValue()) value(v.GetValue()); else imm(v.GetImm());
                    mix(static_cast<u64>(v.GetUniformEffectId())); mix(static_cast<u64>(v.GetHelperABI()));
                    mix(static_cast<u64>(v.GetHostFpEffect())); mix(static_cast<u64>(v.GetHostRegisterEffect()));
                    mix(static_cast<u64>(v.GetHelperGuestStateEffect())); mix(static_cast<u64>(v.GetHelperFaultEffect()));
                    mix(static_cast<u64>(v.GetHelperReentryEffect())); mix(static_cast<u64>(v.GetHostFlagsEffect())); break;
                }
                case ir::ArgType::Params: return 0;
            }
        }
    }
    return hash;
}
}  // namespace swift::runtime::backend
