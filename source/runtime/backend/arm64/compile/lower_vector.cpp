#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include "lower_vector.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/facts.h"
#include "runtime/compile/workspace.h"

namespace swift::runtime::backend::arm64 {

namespace {
struct Publication {
    const ir::Inst* low_store{};
    const ir::Inst* high_store{};
    const ir::Inst* zero{};
    const ir::Inst* load_extension{};
    const ir::Inst* load{};
    u16 target{};
};
bool IsFusionBarrier(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::LoadMemory:
        case O::StoreMemory:
        case O::LoadMemoryTSO:
        case O::StoreMemoryTSO:
        case O::MemoryCopy:
        case O::MemoryCopyTSO:
        case O::CompareAndSwap:
        case O::CompareAndSwap128:
        case O::CheckMemoryAlignment:
        case O::AtomicExchange:
        case O::AtomicFetchAdd:
        case O::AtomicRMW:
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
bool PublicationShape(const Publication& p) {
    // A typed edge may narrow its producer. Fusion clears whole lanes, so
    // both writes must cover the complete 64-bit halves they replace.
    return p.low_store && p.high_store && p.zero && p.target >= 16 && p.target <= 31 &&
           p.low_store->GetOp() == ir::OpCode::SetHostFPR &&
           p.high_store->GetOp() == ir::OpCode::SetHostFPR &&
           p.low_store->GetArg<ir::Value>(0).Defined() &&
           !ir::IsFloatValueType(p.low_store->GetArg<ir::Value>(0).Type()) &&
           ir::GetValueSizeByte(p.low_store->GetArg<ir::Value>(0).Type()) == 8 &&
           p.high_store->GetArg<ir::Value>(0).Type() == ir::ValueType::U64 &&
           p.zero->GetOp() == ir::OpCode::LoadImm && p.zero->ReturnType() == ir::ValueType::U64 &&
           p.zero->GetArg<ir::Imm>(0).Get() == 0 &&
           p.low_store->GetArg<ir::Imm>(1).Get() == p.target &&
           p.high_store->GetArg<ir::Imm>(1).Get() == p.target &&
           p.low_store->GetArg<ir::Imm>(2).Get() == 0 &&
           p.high_store->GetArg<ir::Imm>(2).Get() == sizeof(u64) &&
           p.high_store->GetArg<ir::Value>(0).Def() == p.zero &&
           p.low_store->Id() + 1 == p.high_store->Id();
}
Publication DescribePublication(const ir::Inst* low, const ir::Inst* high) {
    Publication p{.low_store = low,
                  .high_store = high,
                  .zero = high->GetArg<ir::Value>(0).Def(),
                  .target = static_cast<u16>(low->GetArg<ir::Imm>(1).Get())};
    p.load = low->GetArg<ir::Value>(0).Def();
    if (p.load && p.load->GetOp() == ir::OpCode::ZeroExtend64) {
        p.load_extension = p.load;
        p.load = p.load->GetArg<ir::Value>(0).Def();
    }
    return p;
}
}  // namespace

namespace detail {
u16 SelectScalarPublication(const ir::Inst& low_inst,
                            const ir::Inst* high,
                            const compile::BlockAnalysisIndex& index) {
    const auto* low = &low_inst;
    if (low->GetOp() != ir::OpCode::SetHostFPR || low->GetArg<ir::Imm>(2).Get() != 0 || !high ||
        high->GetOp() != ir::OpCode::SetHostFPR)
        return 0;
    const auto p = DescribePublication(low, high);
    if (!PublicationShape(p)) return 0;
    const auto* load = p.load;
    bool load_ok = load && load->GetOp() == ir::OpCode::LoadMemory && load->GetUses(false) == 1 &&
                   load->Id() < low->Id();
    if (load_ok) {
        load_ok = p.load_extension ? load->ReturnType() == ir::ValueType::U32 &&
                                             p.load_extension->GetArg<ir::Value>(0).Type() ==
                                                     ir::ValueType::U32 &&
                                             p.load_extension->GetUses(false) == 1 &&
                                             p.load_extension->GetArg<ir::Value>(0).Def() == load &&
                                             low->GetArg<ir::Value>(0).Def() == p.load_extension &&
                                             load->Id() < p.load_extension->Id() &&
                                             p.load_extension->Id() < low->Id()
                                   : load->ReturnType() == ir::ValueType::U64 &&
                                             low->GetArg<ir::Value>(0).Def() == load;
    }
    if (load_ok) {
        for (auto* node : index.Between(load->Id(), high->Id() + 1)) {
            if (node == low || node == high || node == p.zero || node == p.load_extension) continue;
            if (IsFusionBarrier(node->GetOp()) ||
                ((node->GetOp() == ir::OpCode::GetHostFPR ||
                  node->GetOp() == ir::OpCode::SetHostFPR) &&
                 node->GetArg<ir::Imm>(node->GetOp() == ir::OpCode::GetHostFPR ? 0 : 1).Get() ==
                         p.target)) {
                load_ok = false;
                break;
            }
        }
    }
    return Publication | (load_ok ? PublicationLoad : 0);
}

ScalarFPRActions LowerScalarFPRPublications(const compile::BlockAnalysisIndex& index,
                                            std::span<const compile::LoweringCandidate> candidates,
                                            std::span<const ScalarFPROwner> owners) {
    ScalarFPRActions lowered;
    std::map<const ir::Inst*, u32> zero_uses;
    const auto nodes = index.Instructions();
    for (const auto& candidate : candidates) {
        if (!(candidate.roles & Publication)) continue;
        ASSERT(size_t{candidate.position} + 1 < nodes.size());
        const auto p =
                DescribePublication(nodes[candidate.position], nodes[candidate.position + 1]);
        ASSERT(PublicationShape(p));
        const auto* low = p.low_store;
        const auto* high = p.high_store;
        const auto* load = p.load;
        bool load_ok = candidate.roles & PublicationLoad;
        if (load_ok) {
            const auto first = std::lower_bound(
                    owners.begin(), owners.end(), p.target, [](const auto& owner, u16 home) {
                        return owner.home < home;
                    });
            for (auto next = first; next != owners.end() && next->home == p.target; ++next) {
                const auto& owner = *next;
                if (owner.external_uses || (owner.birth <= high->Id() && owner.end > load->Id()))
                    load_ok = false;
            }
        }
        if (load_ok) {
            lowered.actions.push_back(
                    {load->Id(),
                     static_cast<u8>(p.target),
                     p.load_extension ? ScalarFPRAction::Load32 : ScalarFPRAction::Load64});
            if (p.load_extension) lowered.covered.push_back(p.load_extension->Id());
            lowered.covered.push_back(low->Id());
            lowered.covered.push_back(high->Id());
        } else {
            lowered.actions.push_back(
                    {low->Id(), static_cast<u8>(p.target), ScalarFPRAction::Value});
            lowered.covered.push_back(high->Id());
        }
        ++zero_uses[p.zero];
    }
    for (const auto& [zero, uses] : zero_uses)
        if (zero->GetUses(false) == uses) lowered.covered.push_back(zero->Id());
    return lowered;
}
}  // namespace detail

namespace {
bool IsSelfXor(const ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::VecXor || inst->ReturnType() != ir::ValueType::V128) {
        return false;
    }
    const auto left = ir::ResolveBitCastSource(inst->GetArg<ir::Value>(0));
    const auto right = ir::ResolveBitCastSource(inst->GetArg<ir::Value>(1));
    return left.Defined() && right.Defined() && left.Def() == right.Def();
}

bool IsConversion(const ir::Inst* inst) {
    if (!inst || inst->GetOp() != ir::OpCode::VecFCvtIntToFloat) {
        return false;
    }
    const u32 dst_bits = inst->GetArg<ir::Imm>(2).Get();
    return (dst_bits == 32 && ir::GetValueSizeByte(inst->ReturnType()) == 4) ||
           (dst_bits == 64 && ir::GetValueSizeByte(inst->ReturnType()) == 8);
}

}  // namespace
namespace detail {
bool SelectResidentConversion(const ir::Inst& inst,
                              std::array<bool, 32>& zero_high,
                              const compile::BlockAnalysisIndex& index) {
    const u32 target = inst.GetArg<ir::Imm>(1).Get();
    if (target < 16 || target >= zero_high.size()) return false;
    const u32 offset = inst.GetArg<ir::Imm>(2).Get();
    const auto value = inst.GetArg<ir::Value>(0);
    auto* producer = ir::ResolveBitCastSource(value).Def();
    if (offset == 0 && value.Type() == ir::ValueType::V128 && IsSelfXor(producer)) {
        zero_high[target] = true;
        return false;
    }
    if (offset == 0 && value.Def() == producer && IsConversion(producer) &&
        index.Contains(producer) && zero_high[target])
        return true;
    zero_high[target] = false;
    return false;
}
bool IsResidentMemoryRead(const ir::Inst& read) {
    if (read.GetOp() != ir::OpCode::GetHostFPR || read.GetArg<ir::Imm>(1).Get() != 0 ||
        ir::IsFloatValueType(read.ReturnType()) ||
        (ir::GetValueSizeByte(read.ReturnType()) != 4 &&
         ir::GetValueSizeByte(read.ReturnType()) != 8) ||
        read.GetUses(false) != 1)
        return false;
    const auto target = read.GetArg<ir::Imm>(0).Get();
    return target >= 16 && target <= 31;
}
bool IsResidentExtract(const ir::Inst& extract) {
    return extract.GetOp() == ir::OpCode::VecExtract64 &&
           extract.ReturnType() == ir::ValueType::U64 && extract.GetArg<ir::Imm>(1).Get() == 0 &&
           extract.GetUses(false) == 1;
}
}  // namespace detail
namespace {
struct ResidentLowering final {
    const compile::BlockAnalysisIndex& index;
    std::span<const compile::LoweringCandidate> candidates;
    ScalarFPRActions lowered;

    void Add(const ir::Inst* inst, u32 target, ScalarFPRAction::Kind kind) {
        ASSERT(index.Contains(inst) && inst->Id() < UINT16_MAX && target >= 16 && target <= 31);
        if (!lowered.Find(inst->Id()))
            lowered.actions.push_back({inst->Id(), static_cast<u8>(target), kind});
    }
    void Store(const ir::Inst* inst, u16 target) { Add(inst, target, ScalarFPRAction::Store); }
    void Cover(const ir::Inst* inst) { lowered.covered.push_back(inst->Id()); }
    void AnalyzeConversions();
    void AnalyzeMemoryStores();
    void AnalyzeExtractStores();
    bool TryMapConversionStore(const ir::Inst*, const ir::Inst*, u16);
};

void ResidentLowering::AnalyzeConversions() {
    const auto nodes = index.Instructions();
    for (const auto& candidate : candidates) {
        if (!(candidate.roles & detail::Conversion)) continue;
        const auto& inst = *nodes[candidate.position];
        auto* producer = inst.GetArg<ir::Value>(0).Def();
        const auto target = static_cast<u16>(inst.GetArg<ir::Imm>(1).Get());
        bool export_result = producer->GetUses(false) > 1;
        if (export_result && TryMapConversionStore(producer, &inst, target)) export_result = false;
        Add(producer,
            target,
            export_result ? ScalarFPRAction::ConversionExport : ScalarFPRAction::Conversion);
        Cover(&inst);
    }
}

void ResidentLowering::AnalyzeMemoryStores() {
    const auto nodes = index.Instructions();
    for (const auto& candidate : candidates) {
        if (!(candidate.roles & detail::MemoryRead)) continue;
        const auto& read = *nodes[candidate.position];
        const u32 target = read.GetArg<ir::Imm>(0).Get();
        if (target < 16 || target > 31) {
            continue;
        }

        const ir::Inst* source = &read;
        const ir::Inst* extension = nullptr;
        for (const auto* node : index.Between(read.Id(), UINT32_MAX)) {
            const auto& scan = *node;
            if (detail::IsVectorOpaqueBarrier(scan.GetOp()) ||
                (scan.GetOp() == ir::OpCode::SetHostFPR &&
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
                Store(&scan, static_cast<u16>(target));
                Cover(&read);
                if (extension) {
                    Cover(extension);
                }
            }
            break;
        }
    }
}

void ResidentLowering::AnalyzeExtractStores() {
    const auto nodes = index.Instructions();
    for (const auto& candidate : candidates) {
        if (!(candidate.roles & detail::ExtractStore)) continue;
        const auto& extract = *nodes[candidate.position];
        const auto source = ir::ResolveBitCastSource(extract.GetArg<ir::Value>(0));
        if (!source.Defined() || source.Type() != ir::ValueType::V128) {
            continue;
        }

        std::optional<u16> target;
        for (const auto* node : index.Between(extract.Id(), UINT32_MAX)) {
            const auto& scan = *node;
            if (detail::IsVectorOpaqueBarrier(scan.GetOp())) {
                break;
            }
            if (scan.GetOp() == ir::OpCode::SetHostFPR) {
                const auto written_target = static_cast<u16>(scan.GetArg<ir::Imm>(1).Get());
                const auto published = ir::ResolveBitCastSource(scan.GetArg<ir::Value>(0));
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
                    scan.GetValues(),
                    [&](const ir::Value value) { return value.Def() == &extract; });
            if (!uses_extract) {
                continue;
            }
            if (target && scan.GetOp() == ir::OpCode::StoreMemory &&
                scan.GetArg<ir::Value>(1).Def() == &extract &&
                ir::GetValueSizeByte(scan.GetArg<ir::Value>(1).Type()) == sizeof(u64)) {
                Store(&scan, *target);
                Cover(&extract);
            }
            break;
        }
    }
}

bool ResidentLowering::TryMapConversionStore(const ir::Inst* conversion,
                                             const ir::Inst* publication,
                                             u16 target) {
    if (conversion->GetUses(false) != 2) {
        return false;
    }

    const ir::Inst* store{};
    ASSERT(index.UsesReady());
    for (const auto& use : index.Uses(conversion)) {
        const auto* inst = use.consumer;
        if (inst == publication) continue;
        if (store || inst->GetOp() != ir::OpCode::StoreMemory ||
            inst->GetArg<ir::Value>(1).Def() != conversion) {
            return false;
        }
        store = inst;
    }
    if (!store) {
        return false;
    }

    for (const auto* node : index.Between(conversion->Id(), UINT32_MAX)) {
        const auto& inst = *node;
        if (&inst == publication) {
            continue;
        }
        if (detail::IsVectorOpaqueBarrier(inst.GetOp()) ||
            (inst.GetOp() == ir::OpCode::SetHostFPR && inst.GetArg<ir::Imm>(1).Get() == target)) {
            return false;
        }
        if (&inst == store) {
            Store(store, target);
            return true;
        }
    }
    return false;
}

}  // namespace

const ScalarFPRAction* ScalarFPRActions::Find(u32 id) const {
    const auto found = std::find_if(actions.begin(), actions.end(), [id](const auto& row) {
        return row.instruction == id;
    });
    return found == actions.end() ? nullptr : &*found;
}

bool ScalarFPRActions::Covers(u32 id) const {
    return std::ranges::find(covered, id) != covered.end();
}

namespace detail {
ScalarFPRActions LowerResidentScalarFPR(const compile::BlockAnalysisIndex& index,
                                        std::span<const compile::LoweringCandidate> candidates) {
    ResidentLowering builder{index, candidates, {}};
    builder.AnalyzeConversions();
    builder.AnalyzeMemoryStores();
    builder.AnalyzeExtractStores();
    std::sort(builder.lowered.covered.begin(), builder.lowered.covered.end());
    builder.lowered.covered.erase(
            std::unique(builder.lowered.covered.begin(), builder.lowered.covered.end()),
            builder.lowered.covered.end());
    return std::move(builder.lowered);
}
}  // namespace detail
}  // namespace swift::runtime::backend::arm64
