//
// Created by 甘尧 on 2023/12/19.
//

#include "ir_assembler.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "runtime/common/svm_config.h"

namespace swift::runtime::ir {

HIRBuilder::ElseThen Assembler::If(const terminal::If& if_) {
    end_decode = true;
    if (uniform_builder) { UniformFlushRestamps(); uniform_restamps.clear(); UniformInvalidateAll(); uniform_labels.clear(); }
    if (hir_builder) {
        return hir_builder->If(if_);
    } else {
        ir_block->SetTerminal(terminal::Terminal{if_});
        return {};
    }
}

HIRBlock* Assembler::LinkBlock(const terminal::LinkBlock& block) {
    end_decode = true;
    if (uniform_builder) { UniformFlushRestamps(); uniform_restamps.clear(); UniformInvalidateAll(); uniform_labels.clear(); }
    if (hir_builder) {
        return hir_builder->LinkBlock(block);
    } else {
        ir_block->SetTerminal(terminal::Terminal{block});
        return {};
    }
}

void Assembler::ExternalLinkBlock(const terminal::ExternalLinkBlock& link) {
    end_decode = true;
    if (uniform_builder) { UniformFlushRestamps(); uniform_restamps.clear(); UniformInvalidateAll(); uniform_labels.clear(); }
    if (hir_builder) {
        hir_builder->ExternalLinkBlock(link);
    } else {
        ir_block->SetTerminal(terminal::Terminal{link});
    }
}

void Assembler::RegisterExternalDirectLink(Location target) {
    if (hir_builder) {
        hir_builder->RegisterExternalDirectLink(target);
    }
}

void Assembler::ReturnToDispatcher() {
    end_decode = true;
    if (uniform_builder) { UniformFlushRestamps(); uniform_restamps.clear(); UniformInvalidateAll(); uniform_labels.clear(); }
    if (hir_builder) {
        hir_builder->ReturnToDispatcher();
    } else {
        ir_block->SetTerminal(terminal::ReturnToDispatch{});
    }
}

void Assembler::ReturnToHost() {
    end_decode = true;
    if (uniform_builder) { UniformFlushRestamps(); uniform_restamps.clear(); UniformInvalidateAll(); uniform_labels.clear(); }
    if (hir_builder) {
        hir_builder->ReturnToHost();
    } else {
        ir_block->SetTerminal(terminal::ReturnToHost{});
    }
}

void Assembler::Return() {
    end_decode = true;
    if (uniform_builder) { UniformFlushRestamps(); uniform_restamps.clear(); UniformInvalidateAll(); uniform_labels.clear(); }
    if (hir_builder) {
        hir_builder->Return();
    } else {
        ir_block->SetTerminal(terminal::PopRSBHint{});
    }
}

void Assembler::RegisterCallReturn(Location location) {
    if (hir_builder) {
        hir_builder->RegisterCallReturn(location);
    }
}

bool Assembler::EndCommit() const {
    return end_decode;
}

bool Assembler::HasFunctionBlock(Location location) const {
    return hir_builder && hir_builder->HasCurrentFunctionBlock(location);
}

namespace {

// SB0 shadow census totals, printed under SVM_FOLDCENSUS.
unsigned long long g_builder_fold{};
unsigned long long g_builder_map_loads{};
unsigned long long g_builder_map_stores{};
unsigned long long g_foldfail_bytes{};
unsigned long long g_foldfail_continuity{};
unsigned long long g_foldfail_shape{};
unsigned long long g_store_mapped_fact{};
unsigned long long g_store_mapped_clear{};
unsigned long long g_store_unmapped{};
bool g_builder_census_registered{};

bool BuilderCensusEnabled() {
    static const bool enabled = std::getenv("SVM_FOLDCENSUS") != nullptr;
    if (enabled && !g_builder_census_registered) {
        g_builder_census_registered = true;
        std::atexit([] {
            std::fprintf(stderr,
                         "[svm-foldcensus] builder would_fold=%llu would_map_loads=%llu would_map_stores=%llu "
                         "foldfail bytes=%llu cont=%llu shape=%llu store_fact=%llu store_clear=%llu store_unmapped=%llu\n",
                         g_builder_fold, g_builder_map_loads, g_builder_map_stores,
                         g_foldfail_bytes, g_foldfail_continuity, g_foldfail_shape,
                         g_store_mapped_fact, g_store_mapped_clear, g_store_unmapped);
        });
    }
    return enabled;
}

bool ValidUniformEffects(const UniformEffectSet& effects, u32 uniform_size) {
    if (effects.count && !effects.ranges) {
        return false;
    }
    for (u8 i = 0; i < effects.count; ++i) {
        const auto& range = effects.ranges[i];
        if (range.offset > uniform_size || range.size > uniform_size - range.offset) {
            return false;
        }
    }
    return true;
}

}  // namespace

void Assembler::EnableUniformBuilder(const UniformInfo* info, FeatureSet features,
                                     bool bound_backend) {
    const bool forward = GetSvmConfig().builder_uniform;
    if (!info || !info->uniform_size || (!BuilderCensusEnabled() && !forward)) {
        return;
    }
    uniform_info = info;
    uniform_features = features;
    uniform_bytes.assign(info->uniform_size, UniformByteFact{});
    uniform_builder = true;
    uniform_forward = forward;
    uniform_bound = bound_backend && forward;
}

void Assembler::UniformInvalidateAll() {
    std::fill(uniform_bytes.begin(), uniform_bytes.end(), UniformByteFact{});
}

void Assembler::UniformInvalidateHelper(const Inst& inst) {
    const UniformEffectSet* effects = nullptr;
    switch (inst.GetOp()) {
        case OpCode::CallLambda:
        case OpCode::CallDynamic:
        case OpCode::CallLocation:
            effects = LookupUniformEffectSet(inst.GetArg<Lambda>(0).GetUniformEffectId());
            break;
        default:
            break;
    }
    if (!effects || !ValidUniformEffects(*effects, uniform_bytes.size())) {
        UniformInvalidateAll();
        return;
    }
    for (u8 i = 0; i < effects->count; ++i) {
        const auto& range = effects->ranges[i];
        std::fill(uniform_bytes.begin() + range.offset,
                  uniform_bytes.begin() + range.offset + range.size, UniformByteFact{});
    }
}

Assembler::UniformFold Assembler::UniformFoldFor(Uniform uniform) const {
    const auto uni_offset{uniform.GetOffset()};
    const auto uni_type{uniform.GetType()};
    const auto uni_size{GetValueSizeByte(uni_type)};
    const auto is_float{IsFloatValueType(uni_type)};
    if (!uniform_features.xmm_uniform_fwd &&
        uniform_info->IsXmmUniformRange(uni_offset, uni_size)) {
        return {};
    }
    if (uni_offset + uni_size > uniform_bytes.size()) {
        return {};
    }
    Value value_load{};
    u8 value_offset{0};
    for (u8 offset = 0; offset < uni_size; ++offset) {
        const auto& uni_value = uniform_bytes[uni_offset + offset];
        if (!uni_value.Defined()) {
            if (BuilderCensusEnabled()) ++g_foldfail_bytes;
            return {};
        }
        if (offset == 0) {
            value_load = uni_value.value;
            value_offset = uni_value.offset;
        } else if (value_load != uni_value.value ||
                   uni_value.offset != value_offset + offset) {
            if (BuilderCensusEnabled()) ++g_foldfail_continuity;
            return {};
        }
    }
    const auto value_size = value_load.Defined() ? GetValueSizeByte(value_load.Type()) : 0;
    const bool same_reg_class =
            value_load.Defined() && IsFloatValueType(value_load.Type()) == is_float;
    const bool full_value = same_reg_class && value_offset == 0 &&
                            uni_size == value_size && (is_float || uni_size == sizeof(u64));
    const bool low_fpr_view = uniform_features.xmm_narrow_fwd && same_reg_class &&
                              is_float && value_offset == 0 && uni_size < value_size;
    const bool scalar_extract = same_reg_class && !is_float &&
                                value_offset + uni_size <= value_size;
    if (full_value || low_fpr_view) {
        return {.ok = true, .extract = false, .value = value_load, .offset = value_offset};
    }
    if (scalar_extract) {
        return {.ok = true, .extract = true, .value = value_load, .offset = value_offset};
    }
    if (BuilderCensusEnabled()) ++g_foldfail_shape;
    return {};
}

void Assembler::UniformOnLoad(Inst* inst) {
    const auto uniform = inst->GetArg<Uniform>(0);
    const auto uni_offset{uniform.GetOffset()};
    const auto uni_size{GetValueSizeByte(uniform.GetType())};
    if (uni_offset + uni_size > uniform_bytes.size()) {
        return;
    }
    const bool xmm_forward_disabled = !uniform_features.xmm_uniform_fwd &&
            uniform_info->IsXmmUniformRange(uni_offset, uni_size);
    const auto uniform_register = uniform_info->uniform_regs_map.GetValueAt(uni_offset);
    const bool defer_resident_fpr = !uniform_register.Null() &&
            uniform_register.host_reg.is_fpr && GetSvmConfig().xmm_resident;
    if (!uniform_register.Null() && !defer_resident_fpr) {
        const bool pin_ext_gpr = !uniform_register.host_reg.is_fpr &&
                (uniform_register.host_reg.gpr.id <= 9 ||
                 uniform_register.host_reg.gpr.id == 22 ||
                 uniform_register.host_reg.gpr.id == 23 ||
                 uniform_register.host_reg.gpr.id == 29);
        if ((uniform_features.ir_uniform_range || pin_ext_gpr) &&
            !uniform_register.host_reg.is_fpr && UniformWouldFold(uniform)) {
            ++g_builder_fold;
            return;
        }
        ++g_builder_map_loads;
        if (pin_ext_gpr) {
            const Value loaded{inst};
            for (u8 byte = 0; byte < uni_size; ++byte) {
                uniform_bytes[uni_offset + byte] = {loaded, byte};
            }
        }
        return;
    }
    if (UniformWouldFold(uniform)) {
        ++g_builder_fold;
        return;
    }
    if (!xmm_forward_disabled && uniform_features.xmm_ssa_fwd2 &&
        uniform_info->IsXmmUniformRange(uni_offset, uni_size)) {
        const Value loaded{inst};
        for (u8 offset = 0; offset < uni_size; ++offset) {
            uniform_bytes[uni_offset + offset] = {loaded, offset};
        }
    }
}

void Assembler::UniformOnStore(Inst* inst) {
    UniformStoreFacts(inst->GetArg<Uniform>(0), inst->GetArg<Value>(1));
}

void Assembler::UniformStoreFacts(Uniform uniform, Value value) {
    const auto uni_offset{uniform.GetOffset()};
    const auto value_size{GetValueSizeByte(value.Type())};
    if (uni_offset + value_size > uniform_bytes.size()) {
        return;
    }
    const bool xmm_forward_disabled = !uniform_features.xmm_uniform_fwd &&
            uniform_info->IsXmmUniformRange(uni_offset, value_size);
    const auto uniform_register = uniform_info->uniform_regs_map.GetValueAt(uni_offset);
    const bool defer_resident_fpr = !uniform_register.Null() &&
            uniform_register.host_reg.is_fpr && GetSvmConfig().xmm_resident;
    if (!uniform_register.Null() && !defer_resident_fpr) {
        ++g_builder_map_stores;
        const bool mapped_fpr = uniform_register.host_reg.is_fpr;
        const auto reg_index = mapped_fpr ? uniform_register.host_reg.fpr.id
                                          : uniform_register.host_reg.gpr.id;
        const bool pin_ext_gpr = !mapped_fpr &&
                (reg_index <= 9 || reg_index == 22 || reg_index == 23 || reg_index == 29);
        const auto uni_reg_size = GetValueSizeByte(uniform_register.uniform.GetType());
        if ((uniform_features.ir_uniform_range || pin_ext_gpr) && !mapped_fpr &&
            uni_reg_size == sizeof(u64)) {
            if (BuilderCensusEnabled()) ++g_store_mapped_fact;
            for (u32 byte = 0; byte < value_size; ++byte) {
                uniform_bytes[uni_offset + byte] = {value, static_cast<u8>(byte)};
            }
        } else {
            if (BuilderCensusEnabled()) ++g_store_mapped_clear;
            UniformInvalidateAll();
        }
        return;
    }
    if (BuilderCensusEnabled()) ++g_store_unmapped;
    if (!xmm_forward_disabled) {
        for (u8 offset = 0; offset < value_size; ++offset) {
            uniform_bytes[uni_offset + offset] = {value, offset};
        }
    }
}

void Assembler::UniformAfterAppend(Inst* inst) {
    switch (inst->GetOp()) {
        case OpCode::LoadUniform:
            UniformOnLoad(inst);
            break;
        case OpCode::StoreUniform:
            UniformOnStore(inst);
            break;
        case OpCode::CallLambda:
        case OpCode::CallDynamic:
            if (uniform_features.ir_uniform_range) {
                UniformInvalidateHelper(*inst);
            } else {
                UniformInvalidateAll();
            }
            break;
        case OpCode::X87Op:
        case OpCode::MemoryCopy:
        case OpCode::MemoryCopyTSO:
        case OpCode::SetHostGPR:
        case OpCode::SetHostFPR:
        case OpCode::SetLocation:
            UniformInvalidateAll();
            break;
        case OpCode::Goto:
        case OpCode::NotGoto:
            if (!uniform_features.uniform_path_fwd) {
                UniformInvalidateAll();
            } else {
                uniform_labels[inst] = uniform_bytes;
            }
            break;
        case OpCode::BindLabel: {
            if (!uniform_features.uniform_path_fwd) {
                UniformInvalidateAll();
                break;
            }
            auto* branch = inst->GetArg<Value>(0).Def();
            auto target = uniform_labels.find(branch);
            if (target == uniform_labels.end() ||
                target->second.size() != uniform_bytes.size()) {
                UniformInvalidateAll();
                break;
            }
            for (size_t i = 0; i < uniform_bytes.size(); ++i) {
                const auto& taken = target->second[i];
                auto& fallthrough = uniform_bytes[i];
                const bool same = taken.Defined() && fallthrough.Defined() &&
                                  taken.value == fallthrough.value &&
                                  taken.offset == fallthrough.offset;
                if (!same) {
                    fallthrough = {};
                }
            }
            uniform_labels.erase(target);
            break;
        }
        default:
            break;
    }
}

Assembler::~Assembler() {
    if (uniform_forward) {
        UniformFlushRestamps();
    }
}

void Assembler::UniformFlushRestamps() {
    // Re-applied at every flush point for the whole block: the
    // decoder may SetType a folded load's Value at any later point
    // in the block (e.g. MulWithFlags stamps its operands at Mul
    // construction), and the pass's descriptor stamp is the final
    // word either way. The list is cleared at block boundaries
    // (terminal forwards), not here.
    for (auto& [inst, type] : uniform_restamps) {
        inst->SetReturn(type);
    }
}

Value Assembler::LoadUniform(Uniform uniform) {
    if (!uniform_forward) {
        Inst* inst = AppendRaw(OpCode::LoadUniform, uniform);
        if (uniform_builder && inst) {
            UniformOnLoad(inst);
        }
        return Value{inst};
    }
    // SB1: emit exactly the node UniformEliminationPass would have
    // rewritten this load into (fold alias/extract, or the mapped
    // GetHost* access), using the same byte-fact table the shadow
    // validated. Anything outside the mirrored conditions keeps
    // the raw LoadUniform so the pass still sees (and diagnoses)
    // it identically.
    const auto uni_offset{uniform.GetOffset()};
    const auto uni_type{uniform.GetType()};
    const auto uni_size{GetValueSizeByte(uni_type)};
    const auto is_float{IsFloatValueType(uni_type)};
    const auto append_fold = [&](const UniformFold& fold) {
        Inst* inst = fold.extract
                ? AppendRaw(OpCode::BitExtract, fold.value,
                            Imm(fold.offset * 8u), Imm(uni_size * 8u))
                : AppendRaw(OpCode::BitCast, fold.value);
        if (inst) {
            inst->SetReturn(uni_type);
            uniform_restamps.emplace_back(inst, uni_type);
        }
        return Value{inst};
    };
    if (uni_offset + uni_size <= uniform_bytes.size()) {
        const auto uniform_register = uniform_info->uniform_regs_map.GetValueAt(uni_offset);
        const bool defer_resident_fpr = !uniform_register.Null() &&
                uniform_register.host_reg.is_fpr && GetSvmConfig().xmm_resident;
        bool cross_mapped = false;
        if (uniform_register.Null()) {
            for (u8 offset = 1; offset < uni_size; ++offset) {
                if (!uniform_info->uniform_regs_map.GetValueAt(uni_offset + offset).Null()) {
                    cross_mapped = true;
                }
            }
        }
        if (!uniform_register.Null() && !defer_resident_fpr) {
            const auto uni_reg_offset{uniform_register.uniform.GetOffset()};
            const auto uni_reg_type{uniform_register.uniform.GetType()};
            const auto uni_reg_size{GetValueSizeByte(uni_reg_type)};
            const bool scalar_from_fpr =
                    IsFloatValueType(uni_reg_type) && !is_float && uni_size <= sizeof(u64);
            const bool eligible = (scalar_from_fpr ||
                                   IsFloatValueType(uni_reg_type) == is_float) &&
                    uni_offset >= uni_reg_offset &&
                    (uni_offset + uni_size) <= (uni_reg_offset + uni_reg_size) &&
                    uniform_info->uniform_regs_map.GetContinuousSizeFrom(uni_offset) >= uni_size;
            if (eligible) {
                const bool pin_ext_gpr = !uniform_register.host_reg.is_fpr &&
                        (uniform_register.host_reg.gpr.id <= 9 ||
                         uniform_register.host_reg.gpr.id == 22 ||
                         uniform_register.host_reg.gpr.id == 23 ||
                         uniform_register.host_reg.gpr.id == 29);
                if ((uniform_features.ir_uniform_range || pin_ext_gpr) &&
                    !uniform_register.host_reg.is_fpr) {
                    if (const auto fold = UniformFoldFor(uniform); fold.ok) {
                        auto result = append_fold(fold);
                        UniformFlushRestamps();
                        return result;
                    }
                }
                const bool mapped_fpr = uniform_register.host_reg.is_fpr;
                const auto reg_index = mapped_fpr ? uniform_register.host_reg.fpr.id
                                                  : uniform_register.host_reg.gpr.id;
                const Imm offset_in{static_cast<u8>(uni_offset - uni_reg_offset)};
                Inst* inst = mapped_fpr
                        ? AppendRaw(OpCode::GetHostFPR, HostRegIndex(reg_index), offset_in)
                        : AppendRaw(OpCode::GetHostGPR, HostRegIndex(reg_index), offset_in);
                if (inst) {
                    inst->SetReturn(uni_type);
                    uniform_restamps.emplace_back(inst, uni_type);
                    if (pin_ext_gpr) {
                        const Value loaded{inst};
                        for (u8 byte = 0; byte < uni_size; ++byte) {
                            uniform_bytes[uni_offset + byte] = {loaded, byte};
                        }
                    }
                }
                UniformFlushRestamps();
                return Value{inst};
            }
        } else if (!cross_mapped) {
            if (const auto fold = UniformFoldFor(uniform); fold.ok) {
                auto result = append_fold(fold);
                UniformFlushRestamps();
                return result;
            }
            Inst* inst = AppendRaw(OpCode::LoadUniform, uniform);
            if (inst) {
                UniformOnLoad(inst);
            }
            UniformFlushRestamps();
            return Value{inst};
        }
    }
    Inst* inst = AppendRaw(OpCode::LoadUniform, uniform);
    if (inst) {
        UniformOnLoad(inst);
    }
    UniformFlushRestamps();
    return Value{inst};
}

Void Assembler::StoreUniform(Uniform uniform, Value value) {
    if (uniform_bound) {
        // Bound-backend store mapping: mirror
        // UniformEliminationPass's StoreUniform -> SetHost*
        // conversion exactly. Anything outside the mirrored
        // eligibility (cross-boundary, deferred resident FPR,
        // kind mismatch) keeps the raw StoreUniform so the pass
        // still sees and diagnoses it identically.
        const auto uni_offset{uniform.GetOffset()};
        const auto value_type{value.Type()};
        const auto value_size{GetValueSizeByte(value_type)};
        const auto value_is_float{IsFloatValueType(value_type)};
        if (uni_offset + value_size <= uniform_bytes.size()) {
            const auto uniform_register =
                    uniform_info->uniform_regs_map.GetValueAt(uni_offset);
            const bool defer_resident_fpr = !uniform_register.Null() &&
                    uniform_register.host_reg.is_fpr && GetSvmConfig().xmm_resident;
            if (!uniform_register.Null() && !defer_resident_fpr) {
                const auto uni_reg_offset{uniform_register.uniform.GetOffset()};
                const auto uni_reg_type{uniform_register.uniform.GetType()};
                const auto uni_reg_size{GetValueSizeByte(uni_reg_type)};
                const bool scalar_to_fpr =
                        IsFloatValueType(uni_reg_type) && !value_is_float &&
                        value_size <= sizeof(u64);
                // Full-width only at decode: partial-width mapped
                // stores make the planner preserve upper bytes with
                // bitfield inserts, while the pass route proves them
                // dead from its byte facts. Narrow stores stay raw
                // for the pass to map with full context.
                const bool eligible = value_size == uni_reg_size &&
                        (scalar_to_fpr ||
                         IsFloatValueType(uni_reg_type) == value_is_float) &&
                        uni_offset >= uni_reg_offset &&
                        (uni_offset + value_size) <= (uni_reg_offset + uni_reg_size) &&
                        uniform_info->uniform_regs_map.GetContinuousSizeFrom(uni_offset) >= value_size;
                if (eligible) {
                    const bool mapped_fpr = uniform_register.host_reg.is_fpr;
                    const auto reg_index = mapped_fpr ? uniform_register.host_reg.fpr.id
                                                      : uniform_register.host_reg.gpr.id;
                    const Imm offset_in{static_cast<u8>(uni_offset - uni_reg_offset)};
                    Inst* inst = mapped_fpr
                            ? AppendRaw(OpCode::SetHostFPR, value, HostRegIndex(reg_index), offset_in)
                            : AppendRaw(OpCode::SetHostGPR, value, HostRegIndex(reg_index), offset_in);
                    if (inst) {
                        UniformStoreFacts(uniform, value);
                    }
                    UniformFlushRestamps();
                    return Void{inst};
                }
            }
        }
    }
    Inst* inst = AppendRaw(OpCode::StoreUniform, uniform, value);
    if (uniform_builder && inst) {
        UniformOnStore(inst);
    }
    if (uniform_forward) {
        UniformFlushRestamps();
    }
    return Void{inst};
}

namespace {
// Mirrored from ConstFoldingPass (FoldIntegerImmediates) so the
// decode-time fold accepts exactly the immediates the pass would.
constexpr u32 kBoundImmediateUseWindow = 5;

u64 BoundWidthMask(u32 bytes) {
    return bytes >= sizeof(u64) ? UINT64_MAX : ((u64{1} << (bytes * 8)) - 1);
}

bool BoundIsAddSubImmediate(s64 value) {
    if (value < 0) return false;
    const auto immediate = static_cast<u64>(value);
    return immediate <= 0xfff ||
           ((immediate & 0xfff) == 0 && (immediate >> 12) <= 0xfff);
}

u64 BoundRotateRightWidth(u64 value, u32 amount, u32 width) {
    const u64 mask = width == 64 ? UINT64_MAX : ((u64{1} << width) - 1);
    value &= mask;
    amount %= width;
    if (amount == 0) return value;
    return ((value >> amount) | (value << (width - amount))) & mask;
}

bool BoundIsLogicalImmediate(u64 value, u32 width) {
    const u64 width_mask = width == 64 ? UINT64_MAX : UINT32_MAX;
    value &= width_mask;
    if (value == 0 || value == width_mask) return false;
    for (u32 element_bits = 2; element_bits <= width; element_bits <<= 1) {
        const u64 element_mask = element_bits == 64
                ? UINT64_MAX
                : ((u64{1} << element_bits) - 1);
        const u64 element = value & element_mask;
        u64 replicated{};
        for (u32 offset = 0; offset < width; offset += element_bits) {
            replicated |= element << offset;
        }
        if (replicated != value) continue;
        for (u32 ones = 1; ones < element_bits; ++ones) {
            const u64 run = ones == 64 ? UINT64_MAX : ((u64{1} << ones) - 1);
            for (u32 rotation = 0; rotation < element_bits; ++rotation) {
                if (BoundRotateRightWidth(run, rotation, element_bits) == element) {
                    return true;
                }
            }
        }
    }
    return false;
}

std::optional<Imm> BoundAddSubImmediate(const Imm& source, ValueType type) {
    const u32 bytes = GetValueSizeByte(type);
    if (bytes == 0 || bytes > sizeof(u64) || IsFloatValueType(type)) {
        return std::nullopt;
    }
    const u32 reg_bits = bytes <= sizeof(u32) ? 32 : 64;
    const u64 raw = source.Get() & BoundWidthMask(bytes);
    const u64 widened = raw & (reg_bits == 32 ? UINT32_MAX : UINT64_MAX);
    if (BoundIsAddSubImmediate(static_cast<s64>(widened))) {
        return reg_bits == 32 ? Imm{static_cast<u32>(widened)} : Imm{widened};
    }
    const s64 signed_value = reg_bits == 32
            ? static_cast<s64>(static_cast<s32>(static_cast<u32>(widened)))
            : static_cast<s64>(widened);
    if (signed_value < 0 && signed_value != INT64_MIN &&
        BoundIsAddSubImmediate(-signed_value)) {
        return reg_bits == 32 ? Imm{static_cast<s32>(signed_value)}
                              : Imm{signed_value};
    }
    return std::nullopt;
}

std::optional<Imm> BoundLogicalImmediate(const Imm& source, ValueType type) {
    const u32 bytes = GetValueSizeByte(type);
    if (bytes == 0 || bytes > sizeof(u64) || IsFloatValueType(type)) {
        return std::nullopt;
    }
    const u32 reg_bits = bytes <= sizeof(u32) ? 32 : 64;
    const u64 raw = source.Get() & BoundWidthMask(bytes);
    const u64 widened = raw & (reg_bits == 32 ? UINT32_MAX : UINT64_MAX);
    if (!BoundIsLogicalImmediate(widened, reg_bits)) {
        return std::nullopt;
    }
    return reg_bits == 32 ? Imm{static_cast<u32>(widened)} : Imm{widened};
}
}  // namespace

void Assembler::BoundFoldImmediate(Inst* inst) {
    if (!uniform_features.int_imm_fold) {
        return;
    }
    const auto op = inst->GetOp();
    if (op != OpCode::Add && op != OpCode::Sub && op != OpCode::And &&
        op != OpCode::Or && op != OpCode::Xor) {
        return;
    }
    if (bound_index == 0) {
        return;
    }
    const u32 self_pos = bound_index - 1;
    const auto within_window = [&](Inst* producer) {
        const auto found = producer ? bound_positions.find(producer)
                                    : bound_positions.end();
        return found != bound_positions.end() &&
               self_pos - found->second <= kBoundImmediateUseWindow;
    };

    // `0 - x` unary canonical form, mirroring the pass. The pass
    // additionally requires the zero LoadImm to have a single use;
    // at decode time future uses are unknowable, but the rewrite
    // is value-identical either way (the zero stays live for any
    // later use), so only profitability is approximated.
    if (op == OpCode::Sub) {
        const auto left = inst->GetArg<Value>(0);
        const auto right = inst->GetArg<Operand>(1);
        auto* zero = left.Def();
        if (within_window(zero) && zero->GetOp() == OpCode::LoadImm &&
            zero->GetArg<Imm>(0).Get() == 0 &&
            right.GetRight().Null() && right.GetLeft().IsValue() &&
            right.GetOp() == OperandPlus) {
            const auto source = right.GetLeft().value;
            const auto type = inst->ReturnType();
            inst->DestroyArgs();
            inst->SetInst(OpCode::Neg, source);
            inst->SetReturn(type);
            return;
        }
    }

    const auto operand = inst->GetArg<Operand>(1);
    if (!operand.GetRight().Null() || !operand.GetLeft().IsValue() ||
        operand.GetOp() != OperandPlus) {
        return;
    }
    auto* load = operand.GetLeft().value.Def();
    if (!within_window(load) || load->GetOp() != OpCode::LoadImm) {
        return;
    }
    // Narrow Add/Sub: the pass skips these when NZCV is demanded,
    // but the flags pseudo is appended after the ALU op, so the
    // demand is unknowable at decode time. Leave narrow Add/Sub
    // to the pass.
    const u32 bytes = GetValueSizeByte(inst->ReturnType());
    if ((op == OpCode::Add || op == OpCode::Sub) && bytes <= 2) {
        return;
    }
    const auto source = load->GetArg<Imm>(0);
    const auto folded = (op == OpCode::Add || op == OpCode::Sub)
            ? BoundAddSubImmediate(source, inst->ReturnType())
            : BoundLogicalImmediate(source, inst->ReturnType());
    if (folded) {
        inst->SetArg(1, Operand{*folded});
    }
}

void Assembler::BoundFoldExtend(Inst* inst) {
    // Constant evaluation of width ops over LoadImm operands,
    // converted in place to LoadImm with the original return
    // type. The const-folding pass has no equivalent rule (it
    // only canonicalizes immediates into ALU operands), so this
    // only ever shortens code. Restricted to ops that are never
    // flags producers on any frontend (extends/extracts): a
    // SaveFlags pseudo naming this instruction would be appended
    // after it, so HasPseudoOperations() cannot guard at decode
    // time. Neg is deliberately excluded for that reason.
    if (inst->HasPseudoOperations()) {
        return;
    }
    const auto op = inst->GetOp();
    if (op != OpCode::ZeroExtend32 && op != OpCode::ZeroExtend32To64 &&
        op != OpCode::ZeroExtend64 && op != OpCode::SignExtend &&
        op != OpCode::BitExtract) {
        return;
    }
    const auto operand = inst->GetArg<Value>(0);
    auto* load = operand.Def();
    if (!load || load->GetOp() != OpCode::LoadImm) {
        return;
    }
    const ValueType result_type = inst->ReturnType();
    if (IsFloatValueType(result_type) || IsFloatValueType(operand.Type())) {
        return;
    }
    const u32 result_bytes = GetValueSizeByte(result_type);
    const u32 operand_bytes = GetValueSizeByte(operand.Type());
    if (result_bytes == 0 || result_bytes > sizeof(u64) ||
        operand_bytes == 0 || operand_bytes > sizeof(u64)) {
        return;
    }
    const u64 constant = load->GetArg<Imm>(0).Get() & BoundWidthMask(operand_bytes);
    u64 folded{};
    switch (op) {
        case OpCode::ZeroExtend32:
        case OpCode::ZeroExtend32To64:
            folded = constant & UINT32_MAX;
            break;
        case OpCode::ZeroExtend64:
            folded = constant;
            break;
        case OpCode::SignExtend: {
            const u32 src_bits = operand_bytes * 8;
            const s64 sign_extended = src_bits == 64
                    ? static_cast<s64>(constant)
                    : static_cast<s64>(constant << (64 - src_bits)) >> (64 - src_bits);
            folded = static_cast<u64>(sign_extended) & BoundWidthMask(result_bytes);
            break;
        }
        case OpCode::BitExtract: {
            const u64 left = inst->GetArg<Imm>(1).Get();
            const u64 bits = inst->GetArg<Imm>(2).Get();
            if (left >= 64 || bits == 0 || bits > 64) {
                return;
            }
            folded = (constant >> left) &
                     (bits == 64 ? UINT64_MAX : ((u64{1} << bits) - 1));
            folded &= BoundWidthMask(result_bytes);
            break;
        }
        default:
            return;
    }
    inst->DestroyArgs();
    if (result_bytes <= sizeof(u32)) {
        inst->SetInst(OpCode::LoadImm, Imm{static_cast<u32>(folded)});
    } else {
        inst->SetInst(OpCode::LoadImm, Imm{folded});
    }
    inst->SetReturn(result_type);
}

}
