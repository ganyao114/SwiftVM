//
// Created by 甘尧 on 2024/6/20.
//

#include "uniform_elimination_pass.h"

namespace swift::runtime::ir {

struct UniformValue {
    Value value{};
    u8 offset{};

    [[nodiscard]] bool Null() const {
        return value.Defined();
    }
};

void UniformEliminationPass::Run(Block* block, const UniformInfo &info) {
    StackVector<UniformValue, 0x100> uniform_values{info.uniform_size};

    Map<u16, Value> new_values{};
    for (auto& inst : block->GetInstList()) {
        switch (inst.GetOp()) {
            case OpCode::LoadUniform: {
                auto uniform = inst.GetArg<Uniform>(0);
                auto uni_offset{uniform.GetOffset()};
                auto uni_type{uniform.GetType()};
                auto uni_size{GetValueSizeByte(uni_type)};
                auto is_float{IsFloatValueType(uni_type)};
                auto uniform_register = info.uniform_regs_map.GetValueAt(uniform.GetOffset());
                // static uniform load
                if (!uniform_register.Null()) {
                    auto uni_reg_offset{uniform_register.uniform.GetOffset()};
                    auto uni_reg_type{uniform_register.uniform.GetType()};
                    auto uni_reg_size{GetValueSizeByte(uni_reg_type)};
                    if ((uni_offset + uni_size) > (uni_reg_offset + uni_reg_size)) {
                        PANIC("Cross uniform load: {}", fmt::format("{}", inst));
                        break;
                    }
                    inst.Reset();
                    auto reg_index = is_float ? uniform_register.host_reg.fpr.id : uniform_register.host_reg.gpr.id;
                    Imm offset_in{static_cast<u8>(uni_offset - uni_reg_offset)};
                    if (is_float) {
                        inst.GetHostFPR(HostRegIndex(reg_index), offset_in).SetReturn(uni_type);
                    } else {
                        inst.GetHostGPR(HostRegIndex(reg_index), offset_in).SetReturn(uni_type);
                    }
                    break;
                }

                Value value_load{};
                u8 value_offset{0};
                for (u8 offset = 0; offset < uni_size; ++offset) {
                    auto &uni_value = uniform_values[uni_offset + offset];
                    if (uni_value.Null()) {
                        value_load = {};
                        break;
                    }
                    if (value_load != uni_value.value) {
                        value_load = {};
                        break;
                    }
                    value_load = uni_value.value;
                    if (offset == 0) {
                        value_offset = uni_value.offset;
                    }
                }

                if (value_load.Defined()) {
                    inst.Reset();
                    if (value_offset == 0) {
                        inst.BitCast(value_load).SetReturn(uni_type);
                    } else {
                        inst.BitExtract(value_load, Imm(value_offset * 8u), Imm(uni_size * 8u)).SetReturn(uni_type);
                    }
                }
                break;
            }
            case OpCode::StoreUniform: {
                auto uniform = inst.GetArg<Uniform>(0);
                auto value = inst.GetArg<Value>(1);
                auto uni_offset{uniform.GetOffset()};
                auto uni_type{uniform.GetType()};
                auto uni_size{GetValueSizeByte(uni_type)};
                auto is_float{IsFloatValueType(uni_type)};
                auto uniform_register = info.uniform_regs_map.GetValueAt(uniform.GetOffset());
                // static uniform load
                if (!uniform_register.Null()) {
                    auto uni_reg_offset{uniform_register.uniform.GetOffset()};
                    auto uni_reg_type{uniform_register.uniform.GetType()};
                    auto uni_reg_size{GetValueSizeByte(uni_reg_type)};
                    if ((uni_offset + uni_size) > (uni_reg_offset + uni_reg_size)) {
                        PANIC("Cross uniform store: {}", fmt::format("{}", inst));
                        break;
                    }
                    inst.Reset();
                    auto reg_index = is_float ? uniform_register.host_reg.fpr.id : uniform_register.host_reg.gpr.id;
                    Imm offset_in{static_cast<u8>(uni_offset - uni_reg_offset)};
                    if (is_float) {
                        inst.SetHostFPR(value, HostRegIndex(reg_index), offset_in);
                    } else {
                        inst.SetHostGPR(value, HostRegIndex(reg_index), offset_in);
                    }
                    break;
                }
                for (u8 offset = 0; offset < uni_size; ++offset) {
                    uniform_values[uni_offset + offset] = {value, offset};
                }
                break;
            }
            default:
                break;
        }
    }
}

void UniformEliminationPass::Run(HIRBuilder* hir_builder, const UniformInfo& info, bool mem_to_regs) {
    for (auto &func : hir_builder->GetHIRFunctions()) {
        Run(&func, info, mem_to_regs);
    }
}

void UniformEliminationPass::Run(HIRFunction* hir_func, const UniformInfo& info, bool mem_to_regs) {
}

}  // namespace swift::runtime::ir
