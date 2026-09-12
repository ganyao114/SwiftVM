#include "resident_scalar_fpr_analysis.h"

#include <algorithm>
#include <array>

namespace swift::runtime::backend::arm64 {

namespace {

ir::Value ResolveBitCast(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<ir::Value>(0);
    }
    return value;
}

bool IsSelfXor(ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::VecXor || inst->ReturnType() != ir::ValueType::V128) {
        return false;
    }
    const auto left = ResolveBitCast(inst->GetArg<ir::Value>(0));
    const auto right = ResolveBitCast(inst->GetArg<ir::Value>(1));
    return left.Defined() && right.Defined() && left.Def() == right.Def();
}

bool IsOpaqueBarrier(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::CallLambda:
        case O::CallLocation:
        case O::CallDynamic:
        case O::X87Op:
        case O::Sse42Str:
        case O::GetUniformAddress:
        case O::UniformBarrier:
        case O::Goto:
        case O::NotGoto:
        case O::BindLabel:
            return true;
        default:
            return false;
    }
}

bool IsConversion(ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::VecFCvtIntToFloat) {
        return false;
    }
    const u32 dst_bits = inst->GetArg<ir::Imm>(2).Get();
    return (dst_bits == 32 && ir::GetValueSizeByte(inst->ReturnType()) == 4) ||
           (dst_bits == 64 && ir::GetValueSizeByte(inst->ReturnType()) == 8);
}

}  // namespace

void ResidentScalarFPRAnalysis::Analyze(ir::Block* block) {
    conversions.clear();
    memory_stores.clear();
    discarded.clear();
    AnalyzeConversions(block);
    AnalyzeMemoryStores(block);
    AnalyzeExtractStores(block);
}

void ResidentScalarFPRAnalysis::AnalyzeConversions(ir::Block* block) {
    std::array<bool, 32> zero_high{};
    for (auto& inst : block->GetInstList()) {
        if (IsOpaqueBarrier(inst.GetOp())) {
            zero_high.fill(false);
            continue;
        }
        if (inst.GetOp() != ir::OpCode::SetHostFPR) {
            continue;
        }

        const u32 target = inst.GetArg<ir::Imm>(1).Get();
        if (target < 16 || target >= zero_high.size()) {
            continue;
        }
        const u32 offset = inst.GetArg<ir::Imm>(2).Get();
        const auto value = inst.GetArg<ir::Value>(0);
        auto* producer = ResolveBitCast(value).Def();
        if (offset == 0 && value.Type() == ir::ValueType::V128 && IsSelfXor(producer)) {
            zero_high[target] = true;
            continue;
        }
        if (offset == 0 && value.Def() == producer && IsConversion(producer) && zero_high[target]) {
            bool export_result = producer->GetUses(false) > 1;
            if (export_result &&
                TryMapConversionStore(block, producer, &inst, static_cast<u16>(target))) {
                export_result = false;
            }
            conversions.emplace(producer,
                                ConversionRecipe{
                                        .target = static_cast<u16>(target),
                                        .export_result = export_result,
                                });
            discarded.insert(&inst);
            continue;
        }
        zero_high[target] = false;
    }
}

void ResidentScalarFPRAnalysis::AnalyzeMemoryStores(ir::Block* block) {
    auto& list = block->GetInstList();
    for (auto& read : list) {
        if (read.GetOp() != ir::OpCode::GetHostFPR || read.GetArg<ir::Imm>(1).Get() != 0 ||
            ir::IsFloatValueType(read.ReturnType()) ||
            (ir::GetValueSizeByte(read.ReturnType()) != 4 &&
             ir::GetValueSizeByte(read.ReturnType()) != 8) ||
            read.GetUses(false) != 1) {
            continue;
        }
        const u32 target = read.GetArg<ir::Imm>(0).Get();
        if (target < 16 || target > 31) {
            continue;
        }

        ir::Inst* source = &read;
        ir::Inst* extension = nullptr;
        for (auto& scan : list) {
            if (scan.Id() <= read.Id()) {
                continue;
            }
            if (IsOpaqueBarrier(scan.GetOp()) || (scan.GetOp() == ir::OpCode::SetHostFPR &&
                                                  scan.GetArg<ir::Imm>(1).Get() == target)) {
                break;
            }
            bool uses_source = false;
            for (const auto value : scan.GetValues()) {
                uses_source |= value.Def() == source;
            }
            if (!uses_source) {
                continue;
            }
            if (!extension && ir::GetValueSizeByte(read.ReturnType()) == sizeof(u64) &&
                scan.GetOp() == ir::OpCode::ZeroExtend32 &&
                scan.GetArg<ir::Value>(0).Def() == &read && scan.GetUses(false) == 1) {
                extension = &scan;
                source = extension;
                continue;
            }
            if (scan.GetOp() == ir::OpCode::StoreMemory &&
                scan.GetArg<ir::Value>(1).Def() == source &&
                ir::GetValueSizeByte(scan.GetArg<ir::Value>(1).Type()) ==
                        (extension ? sizeof(u32) : ir::GetValueSizeByte(read.ReturnType()))) {
                memory_stores.emplace(&scan, static_cast<u16>(target));
                discarded.insert(&read);
                if (extension) {
                    discarded.insert(extension);
                }
            }
            break;
        }
    }
}

void ResidentScalarFPRAnalysis::AnalyzeExtractStores(ir::Block* block) {
    auto& list = block->GetInstList();
    for (auto& extract : list) {
        if (extract.GetOp() != ir::OpCode::VecExtract64 ||
            extract.ReturnType() != ir::ValueType::U64 ||
            extract.GetArg<ir::Imm>(1).Get() != 0 || extract.GetUses(false) != 1) {
            continue;
        }
        const auto source = ResolveBitCast(extract.GetArg<ir::Value>(0));
        if (!source.Defined() || source.Type() != ir::ValueType::V128) {
            continue;
        }

        std::optional<u16> target;
        for (auto& scan : list) {
            if (scan.Id() <= extract.Id()) {
                continue;
            }
            if (IsOpaqueBarrier(scan.GetOp())) {
                break;
            }
            if (scan.GetOp() == ir::OpCode::SetHostFPR) {
                const auto written_target = static_cast<u16>(scan.GetArg<ir::Imm>(1).Get());
                const auto published = ResolveBitCast(scan.GetArg<ir::Value>(0));
                const bool exact_publication = scan.GetArg<ir::Imm>(2).Get() == 0 &&
                        published.Type() == ir::ValueType::V128 &&
                        published.Def() == source.Def();
                if (target && written_target == *target && !exact_publication) {
                    break;
                }
                if (exact_publication && written_target >= 16 && written_target <= 31) {
                    target = written_target;
                }
                continue;
            }

            const bool uses_extract = std::ranges::any_of(
                    scan.GetValues(), [&](const ir::Value value) { return value.Def() == &extract; });
            if (!uses_extract) {
                continue;
            }
            if (target && scan.GetOp() == ir::OpCode::StoreMemory &&
                scan.GetArg<ir::Value>(1).Def() == &extract &&
                ir::GetValueSizeByte(scan.GetArg<ir::Value>(1).Type()) == sizeof(u64)) {
                memory_stores.emplace(&scan, *target);
                discarded.insert(&extract);
            }
            break;
        }
    }
}

bool ResidentScalarFPRAnalysis::TryMapConversionStore(ir::Block* block,
                                                      ir::Inst* conversion,
                                                      ir::Inst* publication,
                                                      u16 target) {
    if (conversion->GetUses(false) != 2) {
        return false;
    }

    ir::Inst* store{};
    for (auto& inst : block->GetInstList()) {
        if (&inst == publication) {
            continue;
        }
        const bool uses_conversion = std::ranges::any_of(
                inst.GetValues(), [&](const ir::Value value) { return value.Def() == conversion; });
        if (!uses_conversion) {
            continue;
        }
        if (store || inst.GetOp() != ir::OpCode::StoreMemory ||
            inst.GetArg<ir::Value>(1).Def() != conversion) {
            return false;
        }
        store = &inst;
    }
    if (!store) {
        return false;
    }

    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= conversion->Id()) {
            continue;
        }
        if (&inst == publication) {
            continue;
        }
        if (IsOpaqueBarrier(inst.GetOp()) ||
            (inst.GetOp() == ir::OpCode::SetHostFPR && inst.GetArg<ir::Imm>(1).Get() == target)) {
            return false;
        }
        if (&inst == store) {
            memory_stores.emplace(store, target);
            return true;
        }
    }
    return false;
}

const ResidentScalarFPRAnalysis::ConversionRecipe* ResidentScalarFPRAnalysis::FindConversion(
        ir::Inst* conversion) const {
    const auto it = conversions.find(conversion);
    return it == conversions.end() ? nullptr : &it->second;
}

std::optional<u16> ResidentScalarFPRAnalysis::FindMemoryStore(ir::Inst* store) const {
    const auto it = memory_stores.find(store);
    return it == memory_stores.end() ? std::nullopt : std::optional<u16>{it->second};
}

bool ResidentScalarFPRAnalysis::IsDiscarded(ir::Inst* inst) const {
    return discarded.contains(inst);
}

}  // namespace swift::runtime::backend::arm64
