#include "gpr_inputs.h"

#include <algorithm>
#include "constraints.h"
#include "register_contract.h"
#include "runtime/backend/arm64/helper_call_contract.h"
#include "runtime/backend/gpr_coalescing_contract.h"
#include "runtime/compile/allocate/inputs.h"
#include "runtime/compile/analysis/block_index.h"
#include "runtime/compile/analysis/facts.h"
#include "runtime/ir/instr.h"
#include "runtime/ir/low32_copy.h"

namespace swift::runtime::backend::arm64 {
namespace {
using namespace ir;

bool ObservesHost(const Inst& inst, u32 host) {
    if ((inst.GetOp() == OpCode::GetHostGPR && inst.GetArg<Imm>(0).Get() == host) ||
        (inst.GetOp() == OpCode::SetHostGPR && inst.GetArg<Imm>(1).Get() == host))
        return true;
    bool observes = false;
    inst.ForEachValue([&](Value value) {
        value = ResolveBitCastSource(value);
        if (value.Def() && value.Def()->GetOp() == OpCode::GetHostGPR &&
            value.Def()->GetArg<Imm>(0).Get() == host)
            observes = true;
    });
    return observes;
}
bool IsPinnedWViewProducer(const Inst* inst) {
    if (!inst) return false;
    switch (inst->GetOp()) {
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::And:
        case OpCode::AndNot:
        case OpCode::Or:
        case OpCode::Xor:
        case OpCode::Mul:
            return GetValueSizeByte(inst->ReturnType()) == sizeof(u32);
        default:
            return false;
    }
}
Value LowExtractSource(Value value) {
    value = ResolveBitCastSource(value);
    const auto* def = value.Def();
    if (!def || def->GetOp() != OpCode::BitExtract ||
        GetValueSizeByte(def->ReturnType()) != sizeof(u32) || def->GetArg<Imm>(1).Get() != 0 ||
        def->GetArg<Imm>(2).Get() != 32)
        return {};
    return ResolveBitCastSource(def->GetArg<Value>(0));
}
}  // namespace

using namespace ir;

static bool IsWidthChainRootProducer(const Inst* producer) {
    if (!producer || GetValueSizeByte(producer->ReturnType()) != sizeof(u32)) {
        return false;
    }
    switch (producer->GetOp()) {
        case OpCode::GetHostGPR:
            return producer->GetArg<Imm>(1).Get() == 0;
        case OpCode::SignExtend:
        case OpCode::Mul:
        case OpCode::Add:
            // All three emit a real W destination for an U32 result.  This is
            // stronger than a W-view read: the physical X high half is known
            // zero after the producer.  Their inputs may have multiple uses;
            // the publication transaction proves every alias window instead
            // of substituting a last-use heuristic here.
            return true;
        default:
            return false;
    }
}

Value WWriteRoot(Value value, WWriteAliases aliases) {
    for (;;) {
        if (aliases == WWriteAliases::Resolved) value = ResolveBitCastSource(value);
        if (!value.Defined()) return value;
        const auto* def = value.Def();
        if ((def->GetOp() == OpCode::BitExtract &&
             GetValueSizeByte(def->ReturnType()) == sizeof(u32) && def->GetArg<Imm>(1).Get() == 0 &&
             def->GetArg<Imm>(2).Get() == 32) ||
            (def->GetOp() == OpCode::ZeroExtend32To64 &&
             GetValueSizeByte(def->GetArg<Value>(0).Type()) == sizeof(u32))) {
            value = def->GetArg<Value>(0);
            continue;
        }
        return value;
    }
}

bool HasKnownWWrite(Value value, WWriteAliases aliases) {
    value = WWriteRoot(value, aliases);
    if (!value.Defined()) {
        return false;
    }
    auto* def = value.Def();
    // These are aliases/reads, not physical W writes. In particular a
    // U32 GetHostGPR only selects the W view of a pinned X register; it
    // does not prove that X[63:32] is zero.
    if (def->GetOp() == OpCode::GetHostGPR || def->IsBitCastOperation()) {
        return false;
    }
    if (def->GetOp() == OpCode::LoadImm) {
        // A constant that fits in 32 bits materializes with the high
        // half zero under either the W or X encoding, so it is a known
        // W write even though its return type is U64.
        return (def->GetArg<Imm>(0).Get() >> 32) == 0;
    }
    // A narrow *unsigned* load materializes through a W-form load, which
    // zeroes bits [63:32] exactly like a 32-bit def.  Signed narrow loads
    // do not qualify: their register holds a sign-extended pattern, not
    // the zero-extended one.  This stays an opcode whitelist, not a size
    // test.
    if ((def->GetOp() == OpCode::LoadMemory || def->GetOp() == OpCode::LoadMemoryTSO) &&
        (def->ReturnType() == ValueType::U8 || def->ReturnType() == ValueType::U16)) {
        return true;
    }
    return GetValueSizeByte(def->ReturnType()) == sizeof(u32);
}

static void DescribeGPRPublication(const ir::Block* block,
                                   compile::BlockAnalysisIndex& index,
                                   size_t at,
                                   Vector<compile::FixedGPRPublication>& candidates) {
    using namespace ir;
    const auto instructions = index.Instructions();
    const auto* zext = instructions[at];
    ASSERT(zext->GetOp() == OpCode::ZeroExtend32To64);
    const auto source = zext->GetArg<Value>(0);
    if (!source.Def() || GetValueSizeByte(source.Type()) != sizeof(u32)) return;
    const auto extension = instructions.begin() + at;
    index.PrepareUses();
    const auto zext_users = index.Uses(zext);
    const auto first_use = std::find_if(zext_users.begin(), zext_users.end(), [&](const auto& use) {
        const auto position = index.InstructionPosition(use.consumer);
        return position && *position > at;
    });
    const auto store =
            first_use == zext_users.end()
                    ? instructions.end()
                    : instructions.begin() + *index.InstructionPosition(first_use->consumer);
    if (store == instructions.end() || (*store)->GetOp() != OpCode::SetHostGPR ||
        (*store)->GetArg<Value>(0).Def() != zext || (*store)->GetArg<Imm>(2).Get() != 0)
        return;
    const auto target = (*store)->GetArg<Imm>(1).Get();
    if (!(target <= 9 || target == 22 || target == 23 || target == 29)) return;

    const u32 source_uses = index.DirectUseCount(source.Def());
    const u32 zext_uses = index.DirectUseCount(zext);
    const auto next_write = [&](auto after, u32 home) {
        return std::find_if(std::next(after), instructions.end(), [&](const Inst* inst) {
            return inst->GetOp() == OpCode::SetHostGPR && inst->GetArg<Imm>(1).Get() == home;
        });
    };
    const auto target_write = next_write(store, target);
    const auto used_after = [&](const Inst* definition, auto write, bool inclusive) {
        if (write == instructions.end()) return false;
        const size_t boundary = write - instructions.begin();
        return std::ranges::any_of(index.Uses(definition), [&](const auto& use) {
            const auto position = index.InstructionPosition(use.consumer);
            return position && (*position > boundary || (inclusive && *position == boundary));
        });
    };
    // A write reads its own operands before replacing the target. The old
    // zext check was strict, while source capture checks included that write.
    const bool zext_used_after_overwrite = used_after(zext, target_write, false);
    // Counts remain raw SSA edges, including repeats and pseudo uses. Global
    // counts must match this block before admitting either home reuse.
    if (!zext_uses || zext_used_after_overwrite || zext->GetUses() != zext_uses ||
        source.Def()->GetUses() != source_uses)
        return;
    for (auto scan = std::next(extension); scan != store; ++scan)
        if (ObservesHost(**scan, target)) return;

    candidates.push_back({block, zext->Id(), (*store)->Id(), static_cast<u16>(target)});
    auto& candidate = candidates.back();
    const auto producer = std::find(instructions.begin(), extension, source.Def());
    if (producer == extension || (source_uses != 1 && (*producer)->GetOp() != OpCode::GetHostGPR))
        return;
    if (used_after(source.Def(), target_write, true)) return;
    switch ((*producer)->GetOp()) {
        case OpCode::GetHostGPR: {
            const auto source_host = (*producer)->GetArg<Imm>(0).Get();
            const auto source_write = next_write(producer, source_host);
            const bool source_written = source_write != instructions.end();
            const bool source_used_after_write = used_after(source.Def(), source_write, true);
            const bool safe_level2_capture = source_uses > 1 && source_host <= 9 &&
                                             (target == 22 || target == 23 || target == 29) &&
                                             !source_used_after_write;
            if (source_written && !safe_level2_capture) return;
            break;
        }
        case OpCode::Add:
        case OpCode::Sub:
        case OpCode::And:
        case OpCode::Or:
        case OpCode::Xor:
            break;
        default:
            return;
    }
    for (auto scan = std::next(producer); scan != extension; ++scan)
        if (ObservesHost(**scan, target)) return;
    candidate.source = (*producer)->Id();
}

GPRInputBuilder::GPRInputBuilder(const compile::AllocationInputs& input,
                                 const FeatureSet& physical_features,
                                 compile::BlockAnalysisIndex& uses,
                                 const compile::BlockUseEnds* ends,
                                 const ir::Block* block,
                                 std::unique_ptr<compile::GPRInputs>& data,
                                 std::unique_ptr<compile::WidthInputs>& width,
                                 std::unique_ptr<compile::GPRWriteInputs>& writes)
        : input(input)
        , physical_features(physical_features)
        , uses(uses)
        , ends(ends)
        , block(block)
        , data(data)
        , width(width)
        , writes(writes) {
    first_read = data ? data->reads.size() : 0;
    first_view = data ? data->views.size() : 0;
    first_w_publication = data ? data->w_publications.size() : 0;
    first_width = width ? width->publications.size() : 0;
    first_step = width ? width->steps.size() : 0;
    first_write = writes ? writes->writes.size() : 0;
    width_enabled = input.features.ra_width_chain || input.features.ra_width_chain_long;
    for (u32 pool = 0; pool < input.pools.size(); ++pool) {
        publications_enabled |= input.pools[pool].fixed_publications;
        coalescing_enabled |= input.pools[pool].gpr_coalescing;
        if (FixedGPRClassEnabled(input.pools[pool].pool, physical_features))
            fixed_class_pools |= 1u << pool;
    }
    ASSERT(!(coalescing_enabled || width_enabled) || ends);
}
compile::GPRInputs& GPRInputBuilder::Data() {
    if (!data) data = std::make_unique<compile::GPRInputs>();
    return *data;
}
compile::WidthInputs& GPRInputBuilder::WidthData() {
    if (!width) width = std::make_unique<compile::WidthInputs>();
    return *width;
}
compile::GPRWriteInputs& GPRInputBuilder::WriteData() {
    Data();
    if (!writes) writes = std::make_unique<compile::GPRWriteInputs>();
    return *writes;
}
void GPRInputBuilder::DescribeWrites() {
    using WriteInputs = compile::GPRWriteInputs;
    const auto nodes = uses.Instructions();
    for (const auto position : write_candidates) {
        const auto& store = *nodes[position];
        // A use view controls the architectural write. Resolving a BitCast
        // must not turn an 8/16-bit update into a whole-register publication.
        const auto publication_width = GetValueSizeByte(store.GetArg<Value>(0).Type());
        if (publication_width != sizeof(u32) && publication_width != sizeof(u64)) continue;
        const auto home = store.GetArg<Imm>(1).Get();
        const auto stored = ResolveBitCastSource(store.GetArg<Value>(0));
        const auto* wrapper = stored.Def();
        if (!wrapper || stored.Id() >= ends->size()) continue;
        const bool zero_extend = wrapper->GetOp() == OpCode::ZeroExtend32To64;
        const auto produced =
                zero_extend ? ResolveBitCastSource(wrapper->GetArg<Value>(0)) : stored;
        const auto* producer = produced.Def();
        if (!producer) continue;
        const bool known_w = HasKnownWWrite(produced);
        const bool root = IsWidthChainRootProducer(producer);
        const bool tentative_root = IsWidthChainRootProducer(wrapper) || (zero_extend && root);
        const auto admit = [&](bool width_mode) {
            const bool tentative = width_mode && tentative_root;
            const bool last = wrapper->GetUses() == 1 && (*ends)[stored.Id()] == store.Id();
            const bool live = input.features.ra_coalesce_live && !tentative &&
                              (*ends)[stored.Id()] >= store.Id() && !last;
            if (!tentative && !last && !live) return false;
            const bool width_root = width_mode && root;
            if (zero_extend) {
                if ((!known_w && !width_root) || produced.Id() >= ends->size()) return false;
                const bool inner_last =
                        producer->GetUses() == 1 && (*ends)[produced.Id()] == wrapper->Id();
                const bool inner_live = input.features.ra_coalesce_live && !width_root &&
                                        (*ends)[produced.Id()] >= wrapper->Id();
                if (!width_root && !inner_last && !inner_live) return false;
            }
            if (!backend::IsGPRPublicationProducer(producer->GetOp()) && !width_root) return false;
            const auto size = GetValueSizeByte(produced.Type());
            if ((size != sizeof(u32) && size != sizeof(u64)) ||
                ((size == sizeof(u32) || publication_width == sizeof(u32)) && !known_w &&
                 !width_root))
                return false;
            return !(width_root && producer->GetOp() == OpCode::GetHostGPR &&
                     producer->GetArg<Imm>(0).Get() == home);
        };
        u8 modes = admit(false) ? WriteInputs::Baseline : 0;
        if (input.features.ra_width_chain && admit(true)) {
            modes |= WriteInputs::Width;
            if (root) modes |= WriteInputs::WidthRoot;
        }
        if (!modes) continue;
        u32 live_end = store.Id();
        if (input.features.ra_coalesce_live) {
            live_end = std::max<u32>(live_end, (*ends)[stored.Id()]);
            if (zero_extend && produced.Id() < ends->size())
                live_end = std::max<u32>(live_end, (*ends)[produced.Id()]);
        }
        bool blocked = false;
        // The request FeatureSet owns helper semantics; observer rules below
        // deliberately retain the physical writer's FeatureSet instead.
        if (live_end > store.Id())
            for (size_t at = position + 1; at < nodes.size(); ++at) {
                const auto& scan = *nodes[at];
                if (scan.Id() > live_end) continue;
                if ((scan.GetOp() == OpCode::SetHostGPR && scan.GetArg<Imm>(1).Get() == home) ||
                    (home <= 9 &&
                     HelperCallContract::InstructionClobbersGPR(scan, home, input.features))) {
                    blocked = true;
                    break;
                }
            }
        if (blocked) continue;
        bool after_producer = false;
        for (const auto* scan : nodes) {
            if (scan == producer) {
                after_producer = true;
                continue;
            }
            if (!after_producer || scan == wrapper) continue;
            if (scan == &store) break;
            if ((scan->GetOp() != OpCode::SaveFlags &&
                 IsFixedGPRPublicationObserver(*scan, physical_features)) ||
                (scan->GetOp() == OpCode::GetHostGPR && scan->GetArg<Imm>(0).Get() == home) ||
                (scan->GetOp() == OpCode::SetHostGPR && scan->GetArg<Imm>(1).Get() == home)) {
                blocked = true;
                break;
            }
        }
        if (blocked) continue;
        u32 narrow_load = UINT32_MAX;
        if ((modes & WriteInputs::WidthRoot) && producer->GetOp() == OpCode::SignExtend) {
            const auto source = ResolveBitCastSource(producer->GetArg<Value>(0));
            if (source.Def() && source.Def()->GetOp() == OpCode::LoadMemory &&
                GetValueSizeByte(source.Type()) <= sizeof(u16) && source.Def()->GetUses() == 1 &&
                source.Id() < ends->size() && (*ends)[source.Id()] == producer->Id())
                narrow_load = source.Id();
        }
        if (zero_extend) modes |= WriteInputs::ZeroExtend;
        WriteData().writes.push_back({wrapper,
                                      producer,
                                      live_end,
                                      narrow_load,
                                      static_cast<u16>(position),
                                      static_cast<u16>(home),
                                      modes,
                                      fixed_class_pools});
    }
}
void GPRInputBuilder::DescribePublication(const Inst& store) {
    using WidthInputs = compile::WidthInputs;
    const auto stored = ResolveBitCastSource(store.GetArg<Value>(0));
    auto produced = stored;
    const auto home = store.GetArg<Imm>(1).Get();
    const auto publication_width = GetValueSizeByte(store.GetArg<Value>(0).Type());
    const bool whole = store.GetArg<Imm>(2).Get() == 0 &&
                       (publication_width == sizeof(u32) || publication_width == sizeof(u64));
    const bool zext = stored.Def() && stored.Def()->GetOp() == OpCode::ZeroExtend32To64;
    const bool zext32 =
            zext && GetValueSizeByte(stored.Def()->GetArg<Value>(0).Type()) == sizeof(u32);
    if (zext) produced = ResolveBitCastSource(stored.Def()->GetArg<Value>(0));
    if (coalescing_enabled && whole && backend::IsFixedGPRHome(home) &&
        IsPinnedWViewProducer(produced.Def()))
        Data().w_publications.push_back({store.Id(), produced.Id(), static_cast<u16>(home)});
    if (!width_enabled) return;
    u8 flags = 0;
    if (whole && backend::IsFixedGPRHome(home)) {
        const auto root = zext32 ? produced : stored;
        if (input.features.ra_width_chain && IsWidthChainRootProducer(root.Def()) &&
            root.Id() < ends->size() && root.Id() < store.Id()) {
            flags |= WidthInputs::Owner;
            if (zext32 || GetValueSizeByte(root.Type()) == sizeof(u32))
                flags |= WidthInputs::HighZero;
        }
        if (input.features.ra_width_chain_long && zext32 && produced.Def() &&
            (produced.Def()->GetOp() == OpCode::Add || produced.Def()->GetOp() == OpCode::Xor) &&
            GetValueSizeByte(produced.Def()->ReturnType()) == sizeof(u32))
            flags |= WidthInputs::LongStep;
    }
    // W-alpha's later proof names every raw publication, including partial
    // writes and non-fixed homes. Long-only mode needs just the long steps.
    if (input.features.ra_width_chain || (flags & WidthInputs::LongStep))
        WidthData().publications.push_back(
                {stored.Def(), produced.Def(), store.Id(), static_cast<u16>(home), flags});
}
void GPRInputBuilder::DescribeLongSteps() {
    if (!width || !input.features.ra_width_chain_long) return;
    const auto nodes = uses.Instructions();
    const auto closed = [&](const compile::WidthInputs::Publication& pub) {
        for (const auto* scan : nodes) {
            if (scan->Id() <= pub.produced->Id() || scan->Id() >= pub.instruction ||
                scan == pub.stored)
                continue;
            switch (scan->GetOp()) {
                case OpCode::LoadImm:
                case OpCode::ClearFlags:
                case OpCode::SaveFlags:
                    break;
                default:
                    return false;
            }
        }
        return true;
    };
    const auto safe_between = [&](u32 begin, u32 end) {
        for (const auto* scan : nodes) {
            if (scan->Id() <= begin || scan->Id() >= end) continue;
            switch (scan->GetOp()) {
                case OpCode::AdvancePC:
                case OpCode::BitExtract:
                case OpCode::LoadImm:
                case OpCode::ClearFlags:
                case OpCode::SaveFlags:
                    break;
                default:
                    return false;
            }
        }
        return true;
    };
    StackVector<u8, 32> closed_steps;
    for (u32 at = first_width; at < width->publications.size(); ++at) {
        const auto& current = width->publications[at];
        if (!(current.flags & compile::WidthInputs::LongStep)) continue;
        compile::WidthInputs::Step step{at,
                                        static_cast<u32>(width->links.size()),
                                        0,
                                        static_cast<u32>(width->views.size()),
                                        0};
        // Preserve every possible predecessor. Only the allocator knows which
        // steps survive mapping checks, so raw semantic adjacency is insufficient.
        const bool current_closed = closed(current);
        if (current_closed)
            for (u32 previous = first_step; previous < width->steps.size(); ++previous) {
                const auto& before = width->publications[width->steps[previous].publication];
                if (before.home != current.home || !closed_steps[previous - first_step]) continue;
                bool linked = false;
                current.produced->ForEachValue([&](Value value) {
                    linked |= LowExtractSource(value).Def() == before.stored;
                });
                if (linked && safe_between(before.instruction, current.produced->Id()))
                    width->links.push_back(previous);
            }
        current.produced->ForEachValue([&](Value value) {
            const auto* bridge = ResolveBitCastSource(value).Def();
            if (!bridge || !LowExtractSource(value).Defined() || bridge->GetUses() != 1 ||
                bridge->Id() >= ends->size() || (*ends)[bridge->Id()] != current.produced->Id() ||
                !data)
                return;
            for (u32 index = first_view; index < data->views.size(); ++index)
                if (nodes[data->views[index].position] == bridge) {
                    width->views.push_back(index);
                    break;
                }
        });
        step.link_count = width->links.size() - step.first_link;
        step.view_count = width->views.size() - step.first_view;
        width->steps.push_back(step);
        closed_steps.push_back(current_closed);
    }
}
void GPRInputBuilder::Describe(const ir::Inst& inst, size_t position) {
    using namespace ir;
    using GPRInputs = compile::GPRInputs;
    const auto op = inst.GetOp();
    if (publications_enabled) {
        if (op == OpCode::SetHostGPR) {
            has_publications = true;
            const auto value = inst.GetArg<Value>(0);
            full_exchange |= GetValueSizeByte(value.Type()) == sizeof(u64) &&
                             (!value.Def() || value.Def()->GetOp() != OpCode::ZeroExtend32To64);
        }
        if (op == OpCode::ZeroExtend32To64) publication_candidates.push_back(position);
    }
    if (!coalescing_enabled && !width_enabled) return;
    if (op == OpCode::SetHostGPR) DescribePublication(inst);
    if (coalescing_enabled) {
        if (op == OpCode::GetHostGPR)
            captures.push_back({inst.Id(), static_cast<u16>(inst.GetArg<Imm>(0).Get())});
        if (op == OpCode::SetHostGPR && inst.GetArg<Imm>(2).Get() == 0 &&
            backend::IsFixedGPRHome(inst.GetArg<Imm>(1).Get()))
            write_candidates.push_back(position);
    }
    if (coalescing_enabled && op == OpCode::GetHostGPR && inst.GetArg<Imm>(1).Get() == 0 &&
        GetValueSizeByte(inst.ReturnType()) == sizeof(u32)) {
        const auto home = inst.GetArg<Imm>(0).Get();
        if (backend::IsFixedGPRHome(home) && latest_store[home] &&
            !(blocked_homes & (1u << home)) && inst.Id() < ends->size()) {
            const auto* store = latest_store[home];
            const auto publication_width = GetValueSizeByte(store->GetArg<Value>(0).Type());
            const bool whole =
                    store->GetArg<Imm>(2).Get() == 0 &&
                    (publication_width == sizeof(u32) || publication_width == sizeof(u64));
            const auto published = ResolveBitCastSource(store->GetArg<Value>(0));
            const bool high_zero =
                    published.Def() &&
                    ((published.Def()->GetOp() == OpCode::ZeroExtend32To64 &&
                      GetValueSizeByte(published.Def()->GetArg<Value>(0).Type()) == sizeof(u32)) ||
                     (GetValueSizeByte(published.Type()) == sizeof(u32) &&
                      HasKnownWWrite(published)));
            if (whole)
                Data().reads.push_back({published.Def(),
                                        static_cast<u16>(position),
                                        store->Id(),
                                        static_cast<u16>(home),
                                        high_zero,
                                        fixed_class_pools});
        }
    }
    const bool low32 = op == OpCode::BitExtract &&
                       GetValueSizeByte(inst.ReturnType()) == sizeof(u32) &&
                       inst.GetArg<Imm>(1).Get() == 0 && inst.GetArg<Imm>(2).Get() == 32;
    if (low32 || (width_enabled && op == OpCode::ZeroExtend32To64 &&
                  GetValueSizeByte(inst.GetArg<Value>(0).Type()) == sizeof(u32))) {
        const auto source = ResolveBitCastSource(inst.GetArg<Value>(0));
        u32 wrapper = UINT32_MAX;
        const auto nodes = uses.Instructions();
        if (coalescing_enabled && low32 && inst.GetUses() == 1 && inst.Id() < ends->size() &&
            position + 1 < nodes.size()) {
            const auto* next = nodes[position + 1];
            if (next->GetOp() == OpCode::ZeroExtend32To64 &&
                next->GetArg<Value>(0).Def() == &inst && (*ends)[inst.Id()] == next->Id())
                wrapper = position + 1;
        }
        u8 flags = low32 && coalescing_enabled ? GPRInputs::Low32 : 0;
        if (width_enabled) flags |= GPRInputs::WidthBridge;
        if (width_enabled && HasKnownWWrite(source)) flags |= GPRInputs::KnownWWrite;
        if (source.Def() && source.Def()->GetOp() == OpCode::GetHostGPR &&
            GetValueSizeByte(source.Type()) == sizeof(u32))
            flags |= GPRInputs::CaptureU32;
        if (source.Def() && source.Def()->GetOp() != OpCode::GetHostGPR)
            flags |= GPRInputs::RecolorSource;
        Data().views.push_back(
                {source.Def(), wrapper, static_cast<u16>(position), flags, fixed_class_pools});
    }
    // These are site rules, not another consumer graph. Physical copy checks
    // borrow raw edges from the unique shared index.
    u8 flags = coalescing_enabled && IsReadOnlyLow32Consumer(op) ? GPRInputs::AllowNoResult : 0;
    if (coalescing_enabled && op == OpCode::AtomicExchange) flags |= GPRInputs::ForbidTransfer;
    if (coalescing_enabled && IsPinnedWViewProducer(&inst)) flags |= GPRInputs::PinnedWProducer;
    if (coalescing_enabled && IsFixedGPRPublicationObserver(inst, physical_features))
        flags |= GPRInputs::PublicationObserver;
    if (input.features.ra_width_chain &&
        (op == OpCode::BitExtract || op == OpCode::ZeroExtend32To64))
        flags |= GPRInputs::WidthEmissionSite;
    if (flags) consumer_rules.push_back({inst.Id(), flags});
    if (!coalescing_enabled) return;
    u32 mask = FixedGPRClobbers(inst, physical_features);
    if (op == OpCode::SetHostGPR) mask |= 1u << inst.GetArg<Imm>(1).Get();
    if (const auto helper = HelperCallContract::Resolve(inst, physical_features))
        for (u32 home = 0; home < 32; ++home)
            if (helper->ClobbersGPR(home)) mask |= 1u << home;
    if (mask) clobbers.push_back({inst.Id(), mask});
    // Resetting a publication wins for its own home, even if that opcode is
    // also classified as an observer for previously published homes.
    if (IsFixedGPRPublicationObserver(inst, physical_features)) blocked_homes = UINT32_MAX;
    if (op == OpCode::SetHostGPR) {
        const auto home = inst.GetArg<Imm>(1).Get();
        latest_store[home] = &inst;
        host_writes.push_back({static_cast<u16>(home), inst.Id()});
        blocked_homes &= ~(1u << home);
    }
}
void GPRInputBuilder::Finish() {
    if (has_publications && !full_exchange)
        for (const auto position : publication_candidates)
            DescribeGPRPublication(block, uses, position, Data().publications);
    DescribeLongSteps();
    DescribeWrites();
    if (!data && (!width || width->publications.size() == first_width)) return;
    Data();
    const auto nodes = uses.Instructions();
    auto& reads = data->reads;
    if (reads.size() > first_read) {
        ASSERT(ends);
        reads.erase(std::remove_if(reads.begin() + first_read,
                                   reads.end(),
                                   [&](const auto& read) {
                                       const auto id = nodes[read.position]->Id();
                                       // Preserve the old raw block scan: even locally unreachable
                                       // writes reject reuse, and a virtual end never includes the
                                       // next block.
                                       return std::any_of(host_writes.begin(),
                                                          host_writes.end(),
                                                          [&](const HostWrite& write) {
                                                              return write.home == read.home &&
                                                                     id < write.instruction &&
                                                                     write.instruction <=
                                                                             (*ends)[id];
                                                          });
                                   }),
                    reads.end());
    }
    const u32 read_count = reads.size() - first_read, view_count = data->views.size() - first_view;
    const u32 w_count = data->w_publications.size() - first_w_publication;
    const u32 width_count = width ? width->publications.size() - first_width : 0;
    const u32 step_count = width ? width->steps.size() - first_step : 0;
    const u32 write_count = writes ? writes->writes.size() - first_write : 0;
    if (!read_count && !view_count && !w_count && !width_count && !write_count) return;
    const u32 first_clobber = data->clobbers.size();
    if (view_count) {
        data->clobbers.insert(data->clobbers.end(), clobbers.begin(), clobbers.end());
    }
    if (view_count || write_count || width_count) {
        data->consumer_rules.insert(
                data->consumer_rules.end(), consumer_rules.begin(), consumer_rules.end());
    }
    const u32 first_capture = writes ? writes->captures.size() : 0;
    if (write_count)
        writes->captures.insert(writes->captures.end(), captures.begin(), captures.end());
    data->blocks.push_back({block,
                            first_read,
                            read_count,
                            first_view,
                            view_count,
                            first_clobber,
                            static_cast<u32>(view_count ? clobbers.size() : 0),
                            first_w_publication,
                            w_count,
                            first_width,
                            width_count,
                            first_step,
                            step_count,
                            first_write,
                            write_count,
                            first_capture,
                            static_cast<u32>(write_count ? captures.size() : 0)});
}
}  // namespace swift::runtime::backend::arm64
