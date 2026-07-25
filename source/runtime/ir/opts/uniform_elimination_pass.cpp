//
// Created by 甘尧 on 2024/6/20.
//

#include "uniform_elimination_pass.h"

namespace swift::runtime::ir {

struct UniformValue {
    Value value{};
    u8 offset{};

    [[nodiscard]] bool Defined() const {
        return value.Defined();
    }
};

void UniformEliminationPass::Run(Block* block, const UniformInfo &info) {
    StackVector<UniformValue, 0x100> uniform_values{info.uniform_size};
    u32 load_count{};
    u32 folded_load_count{};
    u32 mapped_load_count{};
    u32 mapped_store_count{};
    u32 invalidation_count{};

    auto invalidate_uniform_values = [&] {
        std::fill(uniform_values.begin(), uniform_values.end(), UniformValue{});
        invalidation_count++;
    };

    for (auto& inst : block->GetInstList()) {
        switch (inst.GetOp()) {
            case OpCode::LoadUniform: {
                load_count++;
                auto uniform = inst.GetArg<Uniform>(0);
                auto uni_offset{uniform.GetOffset()};
                auto uni_type{uniform.GetType()};
                auto uni_size{GetValueSizeByte(uni_type)};
                auto is_float{IsFloatValueType(uni_type)};
                ASSERT_MSG(uni_offset + uni_size <= uniform_values.size(),
                           "uniform load [{}, {}) exceeds uniform buffer size {}",
                           uni_offset, uni_offset + uni_size, uniform_values.size());
                auto uniform_register = info.uniform_regs_map.GetValueAt(uniform.GetOffset());
                if (uniform_register.Null()) {
                    for (u8 offset = 1; offset < uni_size; ++offset) {
                        if (!info.uniform_regs_map.GetValueAt(uni_offset + offset).Null()) {
                            PANIC("Cross uniform load: {}", fmt::format("{}", inst));
                        }
                    }
                }
                // static uniform load
                if (!uniform_register.Null()) {
                    auto uni_reg_offset{uniform_register.uniform.GetOffset()};
                    auto uni_reg_type{uniform_register.uniform.GetType()};
                    auto uni_reg_size{GetValueSizeByte(uni_reg_type)};
                    if (IsFloatValueType(uni_reg_type) != is_float ||
                        uni_offset < uni_reg_offset ||
                        (uni_offset + uni_size) > (uni_reg_offset + uni_reg_size) ||
                        info.uniform_regs_map.GetContinuousSizeFrom(uni_offset) < uni_size) {
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
                    mapped_load_count++;
                    break;
                }

                Value value_load{};
                u8 value_offset{0};
                for (u8 offset = 0; offset < uni_size; ++offset) {
                    auto &uni_value = uniform_values[uni_offset + offset];
                    if (!uni_value.Defined()) {
                        value_load = {};
                        break;
                    }
                    if (offset == 0) {
                        value_load = uni_value.value;
                        value_offset = uni_value.offset;
                    } else if (value_load != uni_value.value ||
                               uni_value.offset != value_offset + offset) {
                        value_load = {};
                        break;
                    }
                }

                // BitCast is a zero-cost register alias; it does NOT implement
                // the zero-extension of a narrow uniform load. Use it only
                // when the load consumes the complete stored value. A partial
                // scalar load, including one at byte offset zero, needs a real
                // BitExtract/UBFX so upper source bits cannot leak into users
                // such as a host-call argument. BitExtract is GPR-only in the
                // current backend, so partial vector/FPR folds stay disabled.
                const auto value_size = value_load.Defined()
                                              ? GetValueSizeByte(value_load.Type())
                                              : 0;
                const bool same_reg_class =
                        value_load.Defined() &&
                        IsFloatValueType(value_load.Type()) == is_float;
                // Narrow arithmetic producers use a W/X-register container and
                // are not required to clear bits above their logical width.
                // A Value cast wrapper changes Value::Type(), but does not emit
                // any narrowing instruction. Therefore even an apparently
                // same-width U8/U16/U32 store/load pair must extract explicitly:
                // a U32 wrapper may still name a U64 producer, and CallLambda
                // consumes its X register. Only a complete U64 scalar or exact
                // FPR value is guaranteed safe as a zero-cost alias.
                const bool full_value =
                        same_reg_class && value_offset == 0 &&
                        uni_size == value_size && (is_float || uni_size == sizeof(u64));
                const bool scalar_extract =
                        same_reg_class && !is_float &&
                        value_offset + uni_size <= value_size;
                if (full_value || scalar_extract) {
                    inst.Reset();
                    if (full_value) {
                        inst.BitCast(value_load).SetReturn(uni_type);
                    } else {
                        inst.BitExtract(value_load, Imm(value_offset * 8u), Imm(uni_size * 8u)).SetReturn(uni_type);
                    }
                    folded_load_count++;
                }
                break;
            }
            case OpCode::StoreUniform: {
                auto uniform = inst.GetArg<Uniform>(0);
                auto value = inst.GetArg<Value>(1);
                auto uni_offset{uniform.GetOffset()};
                auto value_type{value.Type()};
                auto value_size{GetValueSizeByte(value_type)};
                auto value_is_float{IsFloatValueType(value_type)};
                ASSERT_MSG(uni_offset + value_size <= uniform_values.size(),
                           "uniform store [{}, {}) exceeds uniform buffer size {}",
                           uni_offset, uni_offset + value_size, uniform_values.size());
                auto uniform_register = info.uniform_regs_map.GetValueAt(uniform.GetOffset());
                if (uniform_register.Null()) {
                    for (u8 offset = 1; offset < value_size; ++offset) {
                        if (!info.uniform_regs_map.GetValueAt(uni_offset + offset).Null()) {
                            PANIC("Cross uniform store: {}", fmt::format("{}", inst));
                        }
                    }
                }
                // Static uniform store. The whole access must be contained in
                // one mapping; a cross-boundary access cannot be represented by
                // a single SetHost* operation.
                if (!uniform_register.Null()) {
                    auto uni_reg_offset{uniform_register.uniform.GetOffset()};
                    auto uni_reg_type{uniform_register.uniform.GetType()};
                    auto uni_reg_size{GetValueSizeByte(uni_reg_type)};
                    if (IsFloatValueType(uni_reg_type) != value_is_float ||
                        uni_offset < uni_reg_offset ||
                        (uni_offset + value_size) > (uni_reg_offset + uni_reg_size) ||
                        info.uniform_regs_map.GetContinuousSizeFrom(uni_offset) < value_size) {
                        PANIC("Cross uniform store: {}", fmt::format("{}", inst));
                        break;
                    }
                    inst.Reset();
                    auto reg_index = value_is_float ? uniform_register.host_reg.fpr.id
                                                    : uniform_register.host_reg.gpr.id;
                    Imm offset_in{static_cast<u8>(uni_offset - uni_reg_offset)};
                    if (value_is_float) {
                        inst.SetHostFPR(value, HostRegIndex(reg_index), offset_in);
                    } else {
                        inst.SetHostGPR(value, HostRegIndex(reg_index), offset_in);
                    }
                    mapped_store_count++;
                    // A mapped store mutates the live host register directly.
                    // Treat it like SetHostGPR for the path-insensitive cache:
                    // a later load of another uniform must not fold through a
                    // value captured before this write. This matters when a
                    // static RSP store (ENTER/LEAVE) is followed by a load of
                    // RBP in a function-compiled block; otherwise the load can
                    // become a BitCast of the old pinned x19 value.
                    invalidate_uniform_values();
                    break;
                }
                // The backend stores according to the Value type, not the
                // Uniform descriptor type. Track those actual bytes so a
                // mismatched descriptor cannot make untouched bytes look
                // overwritten. Loads spanning old and new values fail the
                // value/offset-continuity check above.
                for (u8 offset = 0; offset < value_size; ++offset) {
                    uniform_values[uni_offset + offset] = {value, offset};
                }
                break;
            }
            case OpCode::CallLambda:
            case OpCode::CallDynamic:
            case OpCode::X87Op:
            case OpCode::MemoryCopy:
            case OpCode::MemoryCopyTSO:
            case OpCode::SetHostGPR:
            case OpCode::SetHostFPR:
            case OpCode::SetLocation:
            case OpCode::Goto:
            case OpCode::NotGoto:
            case OpCode::BindLabel:
                // Calls are conservatively opaque even though today's x86
                // helpers do not receive the uniform buffer. MemoryCopy uses
                // guest addresses today, but is kept opaque at this generic IR
                // layer. SetLocation covers interrupt/syscall exits; terminals
                // end the block and therefore have no following load to fold.
                // Direct SetHost* operations mutate statically mapped state
                // outside the byte cache. Intra-block branches are barriers as
                // well: the pass is deliberately path-insensitive, so a store
                // between Goto/NotGoto and BindLabel is not known to execute on
                // every path reaching the label. BindLabel is the essential
                // merge barrier; clearing at the branch too keeps facts scoped
                // to one straight-line region.
                invalidate_uniform_values();
                break;
            default:
                break;
        }
    }

    if (std::getenv("SVM_DUMP_IR")) {
        fmt::print(stderr,
                   "[uniform-elim] block {:#x}: LoadUniform {} -> {} "
                   "(folded {}, mapped {}), mapped stores {}, invalidations {}\n",
                   block->GetStartLocation().Value(), load_count,
                   load_count - folded_load_count - mapped_load_count,
                   folded_load_count, mapped_load_count, mapped_store_count,
                   invalidation_count);
        if (std::getenv("SVM_DUMP_IR_POST")) {
            fmt::print(stderr, "--- post-uniform block {:#x} ---\n{}\n",
                       block->GetStartLocation().Value(), block->ToString());
        }
    }
}

void UniformEliminationPass::Run(HIRBuilder* hir_builder, const UniformInfo& info, bool mem_to_regs) {
    for (auto &func : hir_builder->GetHIRFunctions()) {
        Run(&func, info, mem_to_regs);
    }
}

void UniformEliminationPass::Run(HIRFunction* hir_func, const UniformInfo& info, bool mem_to_regs) {
    (void)mem_to_regs;
    for (auto* hir_block : hir_func->GetHIRBlocks()) {
        if (hir_block) {
            Run(hir_block->GetBlock(), info);
        }
    }
}

}  // namespace swift::runtime::ir
