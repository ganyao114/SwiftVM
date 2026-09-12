#include "translator.h"

namespace swift::runtime::backend::arm64 {

namespace {

constexpr u64 kConstAddressPageOffsetMask = 0xfff;
constexpr u64 kConstAddressPageMask = ~kConstAddressPageOffsetMask;

std::optional<u64> GetConstAddress(ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::GetOperand ||
        inst->ReturnType() != ir::ValueType::U64) {
        return std::nullopt;
    }
    const auto operand = inst->GetArg<ir::Operand>(0);
    if (!operand.GetLeft().IsImm()) {
        return std::nullopt;
    }
    if (operand.GetRight().Null()) {
        return operand.GetLeft().imm.Get();
    }
    if (operand.GetOp() == ir::OperandOp::Plus &&
        operand.GetRight().IsImm() && operand.GetRight().imm.Get() == 0) {
        return operand.GetLeft().imm.Get();
    }
    return std::nullopt;
}

bool CanUseConstPageOffset(ir::Inst& memory, u64 address) {
    ir::ValueType type{};
    if (memory.GetOp() == ir::OpCode::LoadMemory) {
        type = memory.ReturnType();
    } else if (memory.GetOp() == ir::OpCode::StoreMemory) {
        type = memory.GetArg<ir::Value>(1).Type();
    } else {
        return false;
    }
    const u64 size = ir::GetValueSizeByte(type);
    const u64 offset = address & kConstAddressPageOffsetMask;
    return size != 0 && (offset <= 255 || offset % size == 0);
}

}  // namespace

#define __ masm.

bool JitTranslator::ReproveCachedConstAddress(ir::Inst* inst) const {
    if (!inst || inst->GetOp() != ir::OpCode::GetOperand ||
        !context.IsConstAddressCached(inst->Id())) {
        return false;
    }
    const u32 anchor_id = context.ConstAddressCacheAnchor(inst->Id());
    const auto current_address = GetConstAddress(inst);
    if (!current_address) {
        return false;
    }
    ir::Inst* anchor = nullptr;
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() == anchor_id) {
            anchor = &scan;
            break;
        }
    }
    if (!anchor || !context.IsConstAddressCached(anchor_id) ||
        context.ConstAddressCacheAnchor(anchor_id) != anchor_id) {
        return false;
    }
    const auto anchor_address = GetConstAddress(anchor);
    if (!anchor_address ||
        (*anchor_address & kConstAddressPageMask) !=
                (*current_address & kConstAddressPageMask)) {
        return false;
    }
    const auto target = context.X(ir::Value{inst}).GetCode();
    if (context.X(ir::Value{anchor}).GetCode() != target) {
        return false;
    }
    u32 use_id = inst->Id();
    ir::Inst* memory_use = nullptr;
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() <= inst->Id()) {
            continue;
        }
        if (scan.GetOp() == ir::OpCode::Goto ||
            scan.GetOp() == ir::OpCode::NotGoto ||
            scan.GetOp() == ir::OpCode::BindLabel) {
            return false;
        }
        bool names = false;
        for (auto value : scan.GetValues()) {
            names |= value.Def() == inst;
        }
        if (!names) {
            continue;
        }
        memory_use = &scan;
        use_id = scan.Id();
        break;
    }
    if (!memory_use || !CanUseConstPageOffset(*memory_use, *current_address)) {
        return false;
    }
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() >= anchor_id && scan.Id() <= use_id &&
            !context.DirtyGPR(scan.Id()).Get(target)) {
            return false;
        }
    }
    return true;
}

bool JitTranslator::CanReuseExactCachedConstAddress(ir::Inst* inst) const {
    const u32 anchor_id = context.ConstAddressCacheAnchor(inst->Id());
    if (anchor_id == inst->Id()) {
        return false;
    }
    const auto target = context.X(ir::Value{inst}).GetCode();
    ir::Inst* previous = nullptr;
    for (auto& scan : cur_block->GetInstList()) {
        if (scan.Id() >= inst->Id()) {
            break;
        }
        if (!context.IsConstAddressCached(scan.Id()) ||
            context.ConstAddressCacheAnchor(scan.Id()) != anchor_id ||
            context.X(ir::Value{&scan}).GetCode() != target) {
            continue;
        }
        previous = &scan;
    }
    return previous && GetConstAddress(previous) == GetConstAddress(inst);
}

std::optional<u64> JitTranslator::CachedConstAddressOffset(ir::Inst* inst) const {
    if (memory_state.use_memory_base || !ReproveCachedConstAddress(inst)) {
        return std::nullopt;
    }
    const auto address = GetConstAddress(inst);
    return address ? std::optional<u64>{*address & kConstAddressPageOffsetMask}
                   : std::nullopt;
}

bool JitTranslator::EmitCachedConstAddress(ir::Inst* inst,
                                           const Register& result) {
    if (!context.IsConstAddressCached(inst->Id())) {
        return false;
    }
    ASSERT_MSG(ReproveCachedConstAddress(inst),
               "constant-address cache proof failed at IR {}", inst->Id());
    const auto address = GetConstAddress(inst);
    ASSERT(address);
    if (memory_state.use_memory_base) {
        if (!CanReuseExactCachedConstAddress(inst)) {
            __ Mov(result, *address);
        }
    } else if (context.ConstAddressCacheAnchor(inst->Id()) == inst->Id()) {
        __ Mov(result, *address & kConstAddressPageMask);
    }
    return true;
}

#undef __

}  // namespace swift::runtime::backend::arm64
