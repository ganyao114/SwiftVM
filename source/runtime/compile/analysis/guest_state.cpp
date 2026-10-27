#include "guest_state.h"

#include <algorithm>
#include <functional>
#include <type_traits>
#include <unordered_set>

#include "effects.h"
#include "runtime/common/svm_config.h"
#include "runtime/ir/block.h"

namespace swift::runtime::compile {
using namespace ir;

GuestStateRead ClassifyGuestStateRead(Value source, u32 source_offset, ValueType type,
                                     GuestStateReadPolicy policy) {
    if (!source.Defined() || !IsValidValueType(type)) {
        return {};
    }
    const auto source_type = source.Type();
    if (!IsValidValueType(source_type)) {
        return {};
    }
    const u32 source_size = GetValueSizeByte(source_type);
    const u32 load_size = GetValueSizeByte(type);
    if (source_offset > source_size || load_size > source_size - source_offset) {
        return {};
    }
    const bool is_float = IsFloatValueType(type);
    const bool same_class = IsFloatValueType(source_type) == is_float;
    const auto result = [&](GuestStateReadKind kind) {
        return GuestStateRead{source, kind, static_cast<u8>(source_offset)};
    };
    if (policy == GuestStateReadPolicy::PendingStore) {
        // XmmLo/XmmHi are scalar views of the same architectural V128 slot.
        if (source_type == ValueType::V128 && type == ValueType::U64 &&
            (source_offset == 0 || source_offset == sizeof(u64))) {
            return result(GuestStateReadKind::VectorLane64);
        }
    }
    if (!same_class) {
        return {};
    }
    // A cast changes only the logical view. Narrow arithmetic and cast values
    // can retain dirty high bits in their physical container, so even a full
    // U8/U16/U32 view requires BitExtract before a wider consumer. Only full
    // 64-bit scalars and exact FPR values are zero-cost aliases.
    if (source_offset == 0 && load_size == source_size &&
        (is_float || load_size == sizeof(u64))) {
        return result(GuestStateReadKind::Alias);
    }
    if ((policy == GuestStateReadPolicy::ForwardNarrowFPR ||
         policy == GuestStateReadPolicy::PendingStore) && is_float &&
        source_offset == 0 && load_size < source_size) {
        return result(GuestStateReadKind::Alias);
    }
    if (!is_float) {
        return result(GuestStateReadKind::Extract);
    }
    // Nonzero-offset FPR views cannot be represented by a GPR BitExtract.
    return {};
}

GuestStateRead ReadGuestStateBytes(std::span<const GuestStateByteFact> bytes, Uniform uniform,
                                  GuestStateReadPolicy policy) {
    if (!IsValidValueType(uniform.GetType())) {
        return {};
    }
    const u32 offset = uniform.GetOffset();
    const u32 size = GetValueSizeByte(uniform.GetType());
    if (offset > bytes.size() || size > bytes.size() - offset) {
        return {};
    }
    const auto& first = bytes[offset];
    if (!first.Defined()) {
        return {};
    }
    for (u32 byte = 1; byte < size; ++byte) {
        const auto& fact = bytes[offset + byte];
        if (!fact.Defined() || fact.value != first.value || fact.offset != first.offset + byte) {
            return {};
        }
    }
    return ClassifyGuestStateRead(first.value, first.offset, uniform.GetType(), policy);
}

u32 MeetGuestStateBytes(std::span<GuestStateByteFact> fallthrough,
                        std::span<const GuestStateByteFact> taken) {
    ASSERT(fallthrough.size() == taken.size());
    u32 preserved{};
    for (size_t i = 0; i < fallthrough.size(); ++i) {
        auto& current = fallthrough[i];
        const auto& incoming = taken[i];
        if (current.Defined() && incoming.Defined() && current.value == incoming.value &&
            current.offset == incoming.offset) {
            ++preserved;
        } else {
            current = {};
        }
    }
    return preserved;
}

void GuestStateForwarding::Write(std::span<GuestStateByteFact> bytes, u32 offset, Value value) {
    const auto size = GetValueSizeByte(value.Type());
    ASSERT(value.Defined() && size && offset <= bytes.size() && size <= bytes.size() - offset);
    for (u32 byte = 0; byte < size; ++byte) {
        bytes[offset + byte] = {value, static_cast<u8>(byte)};
    }
}

void GuestStateForwarding::Invalidate(std::span<GuestStateByteFact> bytes) {
    std::fill(bytes.begin(), bytes.end(), GuestStateByteFact{});
}

GuestStateForwardUpdate GuestStateForwarding::Observe(std::span<GuestStateByteFact> bytes,
                                                       const Inst& inst,
                                                       bool range_writes, bool path_forward) {
    using E = GuestStateForwardEvent;
    switch (inst.GetOp()) {
        case OpCode::CallLambda:
        case OpCode::CallLocation:
        case OpCode::CallDynamic:
            if (range_writes) {
                if (const auto* effects = HelperUniformWrites(
                            inst, bytes.size(), UniformWritePolicy::Declared)) {
                    for (u8 i = 0; i < effects->count; ++i) {
                        const auto& range = effects->ranges[i];
                        std::fill(bytes.begin() + range.offset,
                                  bytes.begin() + range.offset + range.size, GuestStateByteFact{});
                    }
                    return {E::RangeInvalidation};
                }
            }
            break;
        case OpCode::X87Op:
        case OpCode::MemoryCopy:
        case OpCode::MemoryCopyTSO:
        case OpCode::SetHostGPR:
        case OpCode::SetHostFPR:
        case OpCode::SetLocation:
            break;
        case OpCode::Goto:
        case OpCode::NotGoto:
            if (path_forward) {
                Remember(&inst, bytes);
                return {E::Branch};
            }
            break;
        case OpCode::BindLabel:
            if (path_forward) {
                if (const auto preserved = Merge(inst.GetArg<Value>(0).Def(), bytes)) {
                    return {E::Merge, *preserved};
                }
            }
            break;
        default:
            return {};
    }
    Invalidate(bytes);
    return {E::FullInvalidation};
}

void GuestStateForwarding::Remember(const Inst* branch,
                                  std::span<const GuestStateByteFact> bytes) {
    auto& snapshot = snapshots[branch];
    snapshot.assign(bytes.begin(), bytes.end());
}

std::optional<u32> GuestStateForwarding::Merge(const Inst* branch,
                                             std::span<GuestStateByteFact> bytes) {
    const auto found = snapshots.find(branch);
    if (found == snapshots.end() || found->second.size() != bytes.size()) {
        return std::nullopt;
    }
    const auto preserved = MeetGuestStateBytes(bytes, found->second);
    snapshots.erase(found);
    return preserved;
}

GuestStateAccess ResolveGuestStateAccess(const GuestStateLayout& layout,
                                          u32 offset, ValueType type,
                                          bool defer_mapped_fpr) {
    if (!IsValidValueType(type) || type == ValueType::VOID) return {};
    const u32 size = GetValueSizeByte(type);
    if (offset > layout.uniform_size || size > layout.uniform_size - offset) return {};
    const auto binding = layout.uniform_regs_map.GetValueAt(offset);
    if (binding.Null()) {
        for (u32 byte = 1; byte < size; ++byte) {
            if (!layout.uniform_regs_map.GetValueAt(offset + byte).Null()) return {};
        }
        return {GuestStateAccessKind::Memory, binding};
    }
    if (defer_mapped_fpr && binding.host_reg.is_fpr) {
        return {GuestStateAccessKind::Memory, binding};
    }
    const auto mapped_type = binding.uniform.GetType();
    const u32 mapped_offset = binding.uniform.GetOffset();
    const u32 mapped_size = GetValueSizeByte(mapped_type);
    const bool scalar_fpr = IsFloatValueType(mapped_type) && !IsFloatValueType(type) &&
            size <= sizeof(u64);
    const bool contained = offset >= mapped_offset && offset - mapped_offset <= mapped_size &&
            size <= mapped_size - (offset - mapped_offset);
    const bool eligible = (scalar_fpr || IsFloatValueType(mapped_type) == IsFloatValueType(type)) &&
            contained && layout.uniform_regs_map.GetContinuousSizeFrom(offset) >= size;
    return {eligible ? GuestStateAccessKind::Register : GuestStateAccessKind::Invalid, binding};
}

GuestStateForwardAccess DescribeGuestStateForwardAccess(
        const GuestStateLayout& layout, const FeatureSet& features,
        u32 offset, ValueType type, bool defer_mapped_fpr) {
    GuestStateForwardAccess result{ResolveGuestStateAccess(layout, offset, type, defer_mapped_fpr)};
    const auto& binding = result.access.binding;
    if (result.access.kind == GuestStateAccessKind::Memory) {
        const bool xmm = layout.IsXmmRange(offset, GetValueSizeByte(type));
        result.read = !xmm || features.xmm_uniform_fwd;
        result.seed_load = result.read && xmm && features.xmm_ssa_fwd2;
        if (result.read) result.write = GuestStateForwardWrite::Write;
    } else if (result.access.kind == GuestStateAccessKind::Register) {
        const bool gpr = !binding.host_reg.is_fpr;
        const auto home = gpr ? binding.host_reg.gpr.id : 0;
        const bool seeded = gpr && home < 32 && layout.optimization.forwarded_gprs.Get(home);
        const bool forward = gpr && (features.ir_uniform_range || seeded);
        result.read = forward && (features.xmm_uniform_fwd ||
                !layout.IsXmmRange(offset, GetValueSizeByte(type)));
        result.seed_load = seeded;
        result.write = forward && GetValueSizeByte(binding.uniform.GetType()) == sizeof(u64)
                ? GuestStateForwardWrite::Write : GuestStateForwardWrite::Invalidate;
    }
    return result;
}

bool IsGuestStateObservationCandidate(OpCode op) {
    // Address exposure and a potentially helper-backed pure computation are
    // candidates even though their ordinary IR effect row is pure.
    if (op == OpCode::GetUniformAddress || op == OpCode::Sse42Str) return true;
    constexpr auto mask = IREffect::ReadGuestState | IREffect::WriteGuestState |
            IREffect::MayFault | IREffect::Call | IREffect::ControlFlow |
            IREffect::Barrier | IREffect::Unknown;
    return (GetIRMetaInfo(op).effects & mask) != IREffect::None;
}

std::span<const Inst* const> GuestStateObservationWindow(
        std::span<const Inst* const> observations, u32 after, u32 before) {
    if (before <= after || observations.empty()) return {};
    const auto first = std::upper_bound(observations.begin(), observations.end(), after,
            [](u32 id, const Inst* inst) { return id < inst->Id(); });
    const auto last = std::lower_bound(first, observations.end(), before,
            [](const Inst* inst, u32 id) { return inst->Id() < id; });
    return {first, last};
}

GuestStateRegisterAccesses::GuestStateRegisterAccesses(const Block* input) : block(input) {
    for (const auto& inst : block->GetInstList()) {
        const auto op = inst.GetOp();
        const bool fpr = op == OpCode::GetHostFPR || op == OpCode::SetHostFPR;
        const bool reads = op == OpCode::GetHostGPR || op == OpCode::GetHostFPR;
        if (reads || op == OpCode::SetHostGPR || op == OpCode::SetHostFPR) {
            const auto home = inst.GetArg<Imm>(reads ? 0 : 1).Get();
            if (home < 32) {
                const u32 prefix = (static_cast<u32>(fpr) * 64 +
                                    static_cast<u32>(home) * 2 + reads) << 16;
                accesses.push_back(prefix | static_cast<u16>(inst.Id()));
            }
        } else if (IsGuestStateGPRBarrierCandidate(inst.GetOp())) {
            barriers.push_back(&inst);
        }
    }
    std::sort(accesses.begin(), accesses.end());
}

std::optional<u32> GuestStateRegisterAccesses::FirstAccess(
        GuestStateRegisterFile file, u32 home, bool reads, u32 after,
        std::optional<u32> ignored) const {
    const auto range = AccessRange(file, home, reads, after, UINT32_MAX);
    auto next = range.begin();
    if (next != range.end() && ignored && static_cast<u16>(*next) == *ignored) ++next;
    return next == range.end() ? std::nullopt : std::optional<u32>{static_cast<u16>(*next)};
}

std::span<const u32> GuestStateRegisterAccesses::AccessRange(
        GuestStateRegisterFile file, u32 home, bool reads, u32 after, u32 before) const {
    if (home >= 32 || file > GuestStateRegisterFile::FPR || before <= after || accesses.empty())
        return {};
    const u32 prefix = (static_cast<u32>(file) * 64 + home * 2 + reads) << 16;
    const auto first = std::lower_bound(accesses.begin(), accesses.end(), prefix);
    const auto last = std::lower_bound(first, accesses.end(), prefix + (1u << 16));
    const auto begin = std::upper_bound(first, last, after, [](u32 id, u32 access) {
        return id < static_cast<u16>(access);
    });
    const auto end = before > UINT16_MAX ? last :
            std::lower_bound(begin, last, before, [](u32 access, u32 id) {
        return static_cast<u16>(access) < id;
    });
    return {begin, end};
}

const GuestStateRegisterAccesses* GuestStateFacts::FindRegisterAccesses(const Block* block) const {
    for (const auto& entry : register_accesses) {
        if (entry->block == block) return entry.get();
    }
    return nullptr;
}

const GuestStateRegisterAccesses& GuestStateFacts::PrepareRegisterAccesses(const Block* block) {
    if (const auto* found = FindRegisterAccesses(block)) return *found;
    // Construct completely before publishing; stable pointees outlive roster growth.
    register_accesses.push_back(std::unique_ptr<GuestStateRegisterAccesses>{new GuestStateRegisterAccesses{block}});
    return *register_accesses.back();
}

std::optional<std::span<const Inst* const>> GuestStateFacts::Find(
        const std::vector<BlockEntries>& entries, const Block* block) {
    for (const auto& record : entries) {
        if (record.block == block) return std::span<const Inst* const>{record.instructions};
    }
    return std::nullopt;
}

std::optional<std::span<const Inst* const>> GuestStateFacts::FindObservations(
        const Block* block) const {
    return Find(observations, block);
}

std::optional<std::span<const Inst* const>> GuestStateFacts::FindGPRDefinitions(
        const Block* block) const {
    return Find(gpr_definitions, block);
}

std::span<const Inst* const> GuestStateFacts::PrepareObservations(const Block* block) {
    if (const auto found = FindObservations(block)) return *found;
    std::vector<const Inst*> candidates;
    for (const auto& inst : block->GetInstList()) {
        if (IsGuestStateObservationCandidate(inst.GetOp())) candidates.push_back(&inst);
    }
    static_assert(std::is_nothrow_move_constructible_v<BlockEntries>);
    observations.push_back({block, std::move(candidates)});
    return observations.back().instructions;
}

std::span<const Inst* const> GuestStateFacts::PrepareGPRDefinitions(const Block* block) {
    if (const auto found = FindGPRDefinitions(block)) return *found;
    std::vector<const Inst*> definitions;
    const auto& instructions = block->GetInstList();
    if (!instructions.empty()) {
        // Numbered local definitions have unique u16 IDs. A bitset deduplicates
        // shared alias tails without one hash node per local definition. Sparse
        // spans use at most 8 KiB; ordinary short spans stay inline. Foreign
        // identities cannot share those bits, even if their numeric IDs collide.
        const u32 first = instructions.front().Id();
        const u32 last = instructions.back().Id();
        const u32 span = last >= first ? last - first + 1 : 0;
        StackVector<u64, 4> seen;
        seen.resize((span + 63) / 64);
        std::fill(seen.begin(), seen.end(), u64{});
        std::unordered_set<const Inst*> foreign;
        const auto append = [&](const Inst* definition) {
            while (definition) {
                const auto id = definition->Id();
                if (definition->GetBlock() == block && id >= first && id <= last) {
                    const u32 slot = id - first;
                    const u64 mask = u64{1} << (slot % 64);
                    auto& word = seen[slot / 64];
                    if (word & mask) break;
                    word |= mask;
                } else if (!foreign.insert(definition).second) {
                    break;
                }
                definitions.push_back(definition);
                const auto op = definition->GetOp();
                if (op != OpCode::ZeroExtend32 && op != OpCode::ZeroExtend32To64 &&
                    op != OpCode::SignExtend) break;
                definition = definition->GetArg<Value>(0).Def();
            }
        };
        for (const auto& inst : instructions) {
            if (inst.GetOp() == OpCode::GetHostGPR) append(&inst);
            else if (inst.GetOp() == OpCode::SetHostGPR) append(inst.GetArg<Value>(0).Def());
        }
        std::sort(definitions.begin(), definitions.end(), std::less<const Inst*>{});
    }
    // Do not publish a partially built catalog after an allocation failure.
    gpr_definitions.push_back({block, std::move(definitions)});
    return gpr_definitions.back().instructions;
}

}  // namespace swift::runtime::compile
