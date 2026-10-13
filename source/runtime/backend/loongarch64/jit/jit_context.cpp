#include "jit_context.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include "runtime/backend/cache_clear.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/module.h"

namespace swift::runtime::backend::loongarch64 {

static_assert(SparseHomesPlan::kStride == kValueStride);

JitContext::JitContext(bool cache_values_enabled, HostFeatures host_features,
                     std::span<const UniformMapDesc> uniforms, ScalarAllocationMode allocation)
        : masm(4096, host_features), features(host_features),
          cache_values(cache_values_enabled && allocation == ScalarAllocationMode::LocalCache),
          allocation_mode(cache_values_enabled ? allocation : ScalarAllocationMode::CanonicalHomes) {
    (void)uniforms;
}

u32 JitContext::CurrentBufferSize() const { return static_cast<u32>(masm.Size()); }

void JitContext::ConfigureModule(Module* owner) {
    module = owner;
    if (!module) return;
    const auto policy = ResolveFeatureSet(module->GetModuleConfig());
    features = features.WithPolicy(policy.loongarch_lsx, policy.loongarch_lasx, policy.loongarch_crc32);
    SetFlagsPublication(policy.loongarch_flags_publication);
    SetFlagsDemand(policy.loongarch_flags_demand);
    SetUniformForwarding(policy.loongarch_uniform_forwarding);
    SetScalarRematerialization(policy.loongarch_rematerialization);
    SetScalarSplitting(policy.loongarch_scalar_splitting);
    SetHomeReuse(policy.loongarch_home_reuse);
    SetVectorAllocation(policy.loongarch_vector_allocation);
    SetVectorCFGAllocation(policy.loongarch_vector_cfg_allocation);
    uniform_extent = module->GetAddressSpace().GetConfig().uniform_buffer_size;
    if (!policy.loongarch_cfg_allocation && allocation_mode == ScalarAllocationMode::GlobalCFG) {
        allocation_mode = ScalarAllocationMode::LocalCache;
        cache_values = true;
    }
}

void JitContext::Flush(const CodeBuffer& buffer) {
    masm.Finalize();
    if (buffer.size < masm.Size()) throw std::runtime_error("LoongArch code allocation is too small");
    std::memcpy(buffer.rw_data, masm.Data(), masm.Size());
    ClearDCache(buffer.rw_data, masm.Size());
    ClearICache(buffer.exec_data, masm.Size());
}

void JitContext::ConfigureBlock(ir::Block* block) {
    if (!block) throw std::invalid_argument("LoongArch homes need an IR block");
    DiscardValues();
    register_plan.reset(); scalar_allocation.reset(); current_inst = nullptr; scalar_baseline_placement = false;
    uniform_plan.reset(); uniform_stats = {};
    sparse_homes.reset(); home_members.clear(); home_stats = {};
    flags_plan.reset(); flags_dirty = ir::Flags::All;
    slot_count = 1;
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() == ir::Inst::invalid_id)
            throw std::invalid_argument("LoongArch dense homes require valid instruction IDs");
        home_members.insert(&inst);
        slot_count = std::max(slot_count, u32(inst.Id()) + 1);
    }
    home_stats.instructions = home_stats.required_homes = home_members.size();
    home_stats.home_count = slot_count;
    home_stats.dense_bytes = home_stats.compact_bytes = u64(slot_count) * kValueStride;
}

void JitContext::ConfigureUniforms(ir::Block* block, const RegisterPlan::Exits& exits) {
    if (!UsesUniformForwarding()) return;
    if (register_plan || scalar_allocation || sparse_homes)
        throw std::logic_error("LoongArch uniform forwarding must precede allocation");
    uniform_plan = UniformForwardPlan::Build(block, targets, stops, exits, uniform_extent);
    if (uniform_plan->Stats().candidates) {
        const auto baseline_live = RegisterPlan::Build(block, targets, stops, {}, exits);
        const auto baseline = ScalarAllocationPlan::Build(block, baseline_live, targets,
                ScalarAllocationPlan::kRegisterCount, scalar_rematerialization, scalar_splitting, stops, exits);
        uniform_plan->SetBaselineSpills(baseline.Stats().spilled_values);
    }
    uniform_plan->Apply();
}

void JitContext::ConfigureRegisters(ir::Block* block, const RegisterPlan::Exits& exits) {
    // Forwarding can extend source ranges. Prune candidates against the final
    // complete CFG coloring; the baseline remains an architectural state load.
    for (u32 round = 0;; ++round) {
        register_plan = RegisterPlan::Build(block, targets, stops, {}, exits);
        if (allocation_mode != ScalarAllocationMode::GlobalCFG) return;
        scalar_allocation = ScalarAllocationPlan::Build(block, *register_plan, targets,
                ScalarAllocationPlan::kRegisterCount, scalar_rematerialization, scalar_splitting, stops, exits);
        if (!uniform_plan) return;
        uniform_plan->RecordAllocationRound();
        uniform_plan->RecordAllocatedSpills(scalar_allocation->Stats().spilled_values);
        if (!uniform_plan->RejectSpilledSources(*scalar_allocation)) {
            if (uniform_plan->RejectSpillGrowth(scalar_allocation->Stats().spilled_values)) {
                register_plan = RegisterPlan::Build(block, targets, stops, {}, exits);
                scalar_allocation = ScalarAllocationPlan::Build(block, *register_plan, targets,
                        ScalarAllocationPlan::kRegisterCount, scalar_rematerialization, scalar_splitting, stops, exits);
                uniform_plan->RecordAllocatedSpills(scalar_allocation->Stats().spilled_values);
            }
            return;
        }
        if (round == 7) {
            uniform_plan->RejectAll();
            register_plan = RegisterPlan::Build(block, targets, stops, {}, exits);
            scalar_allocation = ScalarAllocationPlan::Build(block, *register_plan, targets,
                    ScalarAllocationPlan::kRegisterCount, scalar_rematerialization, scalar_splitting, stops, exits);
            uniform_plan->RecordAllocatedSpills(scalar_allocation->Stats().spilled_values);
            return;
        }
    }
}

void JitContext::ConfigureHomes(ir::Block* block, const RegisterPlan::Exits& exits,
                               std::span<const WideVectorPair> wide) {
    if (!UsesSparseHomes()) return;
    if (!block || !scalar_allocation || !register_plan)
        throw std::logic_error("LoongArch sparse homes require a configured CFG allocation");
    if (sparse_homes) throw std::logic_error("LoongArch sparse IDs are already installed");
    if (block->GetInstList().size() != home_members.size())
        throw std::logic_error("LoongArch sparse block differs from configured homes");
    for (auto& inst : block->GetInstList()) {
        if (!home_members.contains(&inst))
            throw std::logic_error("LoongArch sparse block has foreign instructions");
        (void)register_plan->Position(&inst);
    }
    // Build performs all checks before any ID or frame-size mutation. A
    // rejected layout leaves the dense clone intact for the caller's fallback.
    auto plan = SparseHomesPlan::Build(block, *scalar_allocation, exits, nullptr, home_reuse, targets, stops, wide);
    const auto dense_bytes = home_stats.dense_bytes;
    sparse_homes.emplace(std::move(plan));
    for (auto& inst : block->GetInstList()) inst.SetId(sparse_homes->HomeId(&inst).value_or(ir::Inst::invalid_id));
    slot_count = sparse_homes->HomeCount();
    home_stats = sparse_homes->Stats();
    home_stats.dense_bytes = dense_bytes;
}

u32 JitContext::HomeOffset(ir::Inst* inst, u32 part) const {
    if (!inst || !home_members.contains(inst) || part > 1)
        throw std::invalid_argument("foreign or invalid LoongArch SSA home access");
    if (sparse_homes) {
        if (auto id = sparse_homes->HomeId(inst); id && inst->Id() != *id)
            throw std::logic_error("LoongArch sparse SSA home ID changed after placement");
        return sparse_homes->HomeOffset(inst, part);
    }
    if (inst->Id() == ir::Inst::invalid_id || u32(inst->Id()) >= slot_count)
        throw std::out_of_range("invalid LoongArch dense SSA home ID");
    return u32(inst->Id()) * kValueStride + part * sizeof(u64);
}

void JitContext::ConfigureFlags(ir::Block* block, const FlagsPlan::Exits& exits,
                              const FlagsPlan::Instructions& halt_checks) {
    flags_plan = FlagsPlan::Build(block, targets, stops, exits, halt_checks);
}

void JitContext::BeginInstruction(ir::Inst* inst) {
    current_inst = inst;
    if (scalar_allocation) {
        const auto moves = scalar_allocation->MovesBefore(inst);
        // Stage every old source before changing any destination register.
        // The home planner keeps these simultaneously live staging slots
        // distinct, including register swaps and narrowing raw aliases.
        for (const auto& move : moves) if (move.from) {
            Store(scalar_registers[*move.from], values, HomeOffset(move.value));
            ++stats.stores; ++stats.split_stores;
            if (!move.to) ++stats.spills;
        }
        for (const auto& move : moves) if (move.to) {
            Load(scalar_registers[*move.to], values, HomeOffset(move.value));
            ++stats.loads; ++stats.split_loads;
        }
    }
    // Restore the incoming CFG union, not the previous lexical emitter's
    // transient state. A guarded helper can reload only one runtime path.
    flags_dirty = flags_plan ? flags_plan->At(inst) : ir::Flags::All;
}

std::optional<u32> JitContext::ScalarRegister(ir::Inst* value) const {
    if (!scalar_allocation) return std::nullopt;
    return scalar_baseline_placement ? scalar_allocation->Register(value) :
                                     scalar_allocation->RegisterAt(value, current_inst);
}

bool JitContext::IsLive(ir::Inst* inst) const {
    // Emitters can reread operands after writing a result (for flags, pair
    // results or vector halves), so keep both sides of this instruction live.
    return !register_plan || !current_inst || register_plan->At(current_inst).contains(inst) ||
           register_plan->After(current_inst).contains(inst);
}

void JitContext::ReleaseDead(const LiveSet& live) {
    for (auto& value : cache) if (value.inst && !live.contains(value.inst)) {
        value = {}; ++stats.dead_values;
    }
}

void JitContext::FinishInstruction() {
    if (cache_values && register_plan && current_inst) ReleaseDead(register_plan->After(current_inst));
}

void JitContext::ConfigureFlow(std::unordered_map<ir::Inst*, ir::Inst*> new_targets,
                              std::unordered_set<ir::Inst*> new_stops,
                              std::unordered_map<ir::Inst*, ir::Location> new_locations,
                              std::unordered_set<ir::Inst*> synthetic) {
    targets = std::move(new_targets); stops = std::move(new_stops);
    locations = std::move(new_locations); synthetic_labels = std::move(synthetic);
}

void JitContext::RemapFlow(const std::unordered_map<ir::Inst*, ir::Inst*>& instructions) {
    decltype(targets) remapped_targets;
    decltype(stops) remapped_stops, remapped_synthetic;
    decltype(locations) remapped_locations;
    for (auto [from, to] : targets) remapped_targets.emplace(instructions.at(from), instructions.at(to));
    for (auto* inst : stops) remapped_stops.insert(instructions.at(inst));
    for (auto* inst : synthetic_labels) remapped_synthetic.insert(instructions.at(inst));
    for (auto [inst, loc] : locations) remapped_locations.emplace(instructions.at(inst), loc);
    ConfigureFlow(std::move(remapped_targets), std::move(remapped_stops),
                  std::move(remapped_locations), std::move(remapped_synthetic));
}

ir::Inst* JitContext::BranchTarget(ir::Inst* branch) const {
    if (auto found = targets.find(branch); found != targets.end()) return found->second;
    return branch;
}

std::optional<ir::Location> JitContext::LocationAt(ir::Inst* inst) const {
    if (auto found = locations.find(inst); found != locations.end()) return found->second;
    return std::nullopt;
}

void JitContext::Address(GPR result, GPR base, s64 offset) {
    if (offset >= -2048 && offset <= 2047) masm.addi_d(result, base, static_cast<s32>(offset));
    else {
        if (base == t8) throw std::logic_error("LoongArch address scratch aliases its base");
        masm.LI(t8, static_cast<u64>(offset)); masm.add_d(result, base, t8);
    }
}

void JitContext::Load(GPR result, GPR base, s64 offset, u32 size) {
    if (offset < -2048 || offset > 2047) { Address(t8, base, offset); base = t8; offset = 0; }
    const auto immediate = static_cast<s32>(offset);
    switch (size) {
        case 1: masm.ld_bu(result, base, immediate); break;
        case 2: masm.ld_hu(result, base, immediate); break;
        case 4: masm.ld_wu(result, base, immediate); break;
        case 8: masm.ld_d(result, base, immediate); break;
        default: throw std::invalid_argument("invalid LoongArch scalar load size");
    }
}

void JitContext::Store(GPR source, GPR base, s64 offset, u32 size) {
    if (source == t8) throw std::logic_error("LoongArch store source aliases address scratch");
    if (offset < -2048 || offset > 2047) { Address(t8, base, offset); base = t8; offset = 0; }
    const auto immediate = static_cast<s32>(offset);
    switch (size) {
        case 1: masm.st_b(source, base, immediate); break;
        case 2: masm.st_h(source, base, immediate); break;
        case 4: masm.st_w(source, base, immediate); break;
        case 8: masm.st_d(source, base, immediate); break;
        default: throw std::invalid_argument("invalid LoongArch scalar store size");
    }
}

void JitContext::ReadPart(GPR result, ir::Value value, u32 part) {
    if (part > 1) throw std::invalid_argument("LoongArch value part exceeds canonical home");
    const auto normalize = [&] {
        if (part != 0) return;
        u32 size = ir::GetValueSizeByte(value.Type());
        const auto producer = value.Def()->ReturnType();
        if (producer >= ir::ValueType::U8 && producer <= ir::ValueType::S64 &&
            value.Type() >= ir::ValueType::U8 && value.Type() <= ir::ValueType::S64)
            size = std::min(size, u32(ir::GetValueSizeByte(producer)));
        if (size && size < 8) Mask(result, size * 8);
    };
    if (scalar_allocation) if (auto immediate = scalar_allocation->Immediate(value.Def())) {
        masm.LI(result, part ? u64{0} : *immediate); ++stats.rematerializations;
        normalize(); return;
    }
    if (part == 0) if (auto reg = ScalarRegister(value.Def())) {
        masm.MV(result, scalar_registers[*reg]); ++stats.register_reads;
        normalize(); return;
    }
    for (size_t i = 0; i < cache.size(); ++i) {
        if (cache[i].inst == value.Def() && cache[i].part == part) {
            masm.MV(result, scalar_registers[i]); ++stats.cache_hits;
            normalize();
            return;
        }
    }
    Load(result, values, HomeOffset(value.Def(), part));
    ++stats.loads;
    normalize();
}

void JitContext::WritePart(ir::Inst* inst, GPR value, u32 part) {
    if (inst->ReturnType() == ir::ValueType::VOID) return;
    if (part > 1) throw std::invalid_argument("LoongArch value part exceeds canonical home");
    if (Rematerialized(inst)) { ++stats.omitted_constant_stores; return; }
    if (part == 0) if (auto reg = ScalarRegister(inst)) {
        masm.MV(scalar_registers[*reg], value);
        const auto size = ir::GetValueSizeByte(inst->ReturnType());
        if (size < 8) Mask(scalar_registers[*reg], size * 8);
        return;
    }
    if (!cache_values) {
        if (scalar_allocation && part == 0 && ScalarAllocationPlan::Eligible(inst)) ++stats.spills;
        const u32 size = ir::GetValueSizeByte(inst->ReturnType());
        if (part == 0 && size < 8) {
            Store(zero, values, HomeOffset(inst));
            Store(value, values, HomeOffset(inst), size);
        } else Store(value, values, HomeOffset(inst, part));
        ++stats.stores;
        return;
    }
    size_t index = cache.size();
    for (size_t i = 0; i < cache.size(); ++i)
        if (cache[i].inst == inst && cache[i].part == part) { index = i; break; }
    if (index == cache.size()) {
        for (size_t i = 0; i < cache.size(); ++i) if (!cache[i].inst) { index = i; break; }
    }
    if (index == cache.size()) {
        index = next_register++ % cache.size();
        if (register_plan && current_inst) {
            u32 furthest{};
            for (size_t i = 0; i < cache.size(); ++i) {
                if (!IsLive(cache[i].inst)) { index = i; break; }
                const auto next = register_plan->NextUse(cache[i].inst, current_inst);
                if (next >= furthest) { index = i; furthest = next; }
            }
        }
        const auto& previous = cache[index];
        if (previous.dirty && IsLive(previous.inst)) {
            Store(scalar_registers[index], values, HomeOffset(previous.inst, previous.part));
            ++stats.stores; ++stats.spills;
        } else if (!IsLive(previous.inst)) ++stats.dead_values;
    }
    masm.MV(scalar_registers[index], value);
    if (part == 0 && ir::GetValueSizeByte(inst->ReturnType()) < 8)
        Mask(scalar_registers[index], ir::GetValueSizeByte(inst->ReturnType()) * 8);
    cache[index] = {inst, part, true};
}

void JitContext::ReadRawPart(GPR result, ir::Value value, u32 part) {
    if (part > 1) throw std::invalid_argument("LoongArch raw value part exceeds canonical home");
    if (scalar_allocation) if (auto immediate = scalar_allocation->Immediate(value.Def())) {
        masm.LI(result, part ? u64{0} : *immediate); ++stats.rematerializations; return;
    }
    if (part == 0) if (auto reg = ScalarRegister(value.Def())) {
        masm.MV(result, scalar_registers[*reg]); ++stats.register_reads; return;
    }
    for (size_t i = 0; i < cache.size(); ++i) if (cache[i].inst == value.Def() && cache[i].part == part) {
        masm.MV(result, scalar_registers[i]); ++stats.cache_hits; return;
    }
    Load(result, values, HomeOffset(value.Def(), part)); ++stats.loads;
}

void JitContext::WriteRawPart(ir::Inst* inst, GPR value, u32 part) {
    if (inst->ReturnType() == ir::ValueType::VOID) return;
    if (part > 1) throw std::invalid_argument("LoongArch raw value part exceeds canonical home");
    if (part == 0) if (auto reg = ScalarRegister(inst)) {
        masm.MV(scalar_registers[*reg], value); return;
    }
    for (auto& cached : cache) if (cached.inst == inst && cached.part == part) cached = {};
    Store(value, values, HomeOffset(inst, part)); ++stats.stores;
    if (scalar_allocation && part == 0 && ScalarAllocationPlan::Eligible(inst) && !Rematerialized(inst)) ++stats.spills;
}

void JitContext::PublishInput(ir::Inst* value) {
    if (!scalar_allocation) { FlushValues(); return; }
    if (auto immediate = scalar_allocation->Immediate(value)) {
        masm.LI(t0, *immediate); Store(t0, values, HomeOffset(value)); ++stats.stores;
        Store(zero, values, HomeOffset(value, 1)); ++stats.stores;
        return;
    }
    if (auto reg = ScalarRegister(value)) {
        Store(scalar_registers[*reg], values, HomeOffset(value)); ++stats.stores;
    }
}

void JitContext::PublishInputs(ir::Inst* inst) {
    if (!scalar_allocation) { FlushValues(); return; }
    for (auto* value : scalar_allocation->Inputs(inst)) PublishInput(value);
}

void JitContext::ReloadDefinition(ir::Inst* value) {
    if (auto reg = ScalarRegister(value)) {
        Load(scalar_registers[*reg], values, HomeOffset(value)); ++stats.loads;
    }
}

void JitContext::ReloadDefinitions(ir::Inst* inst) {
    if (scalar_allocation) for (auto* value : scalar_allocation->Definitions(inst)) ReloadDefinition(value);
}

ir::Flags JitContext::RequiredFlagWrites(ir::Inst* inst, ir::Flags requested) const {
    if (flags_demand && flags_plan && flags_plan->Reachable(inst))
        return requested & flags_plan->NeededAfter(inst);
    return requested;
}

void JitContext::Operand(GPR result, const ir::Operand& operand) {
    const auto data = [&](GPR dest, const ir::DataClass& source) {
        if (source.IsValue()) Read(dest, source.value);
        else if (source.IsImm()) masm.LI(dest, source.imm.Get());
        else masm.MV(dest, zero);
    };
    if (result == t7 || result == t8) throw std::logic_error("LoongArch operand aliases reserved scratch");
    data(result, operand.GetLeft());
    if (operand.GetRight().Null() || operand.GetOp().type == ir::OperandOp::None) return;
    data(t7, operand.GetRight());
    switch (operand.GetOp().type) {
        case ir::OperandOp::Plus: masm.add_d(result, result, t7); break;
        case ir::OperandOp::PlusExt:
            masm.slli_d(t7, t7, operand.GetOp().shift_ext); masm.add_d(result, result, t7); break;
        case ir::OperandOp::LSL: masm.sll_d(result, result, t7); break;
        case ir::OperandOp::LSR:
            if (operand.GetLeft().IsValue()) Mask(result, ir::GetValueSizeByte(operand.GetLeft().value.Type()) * 8);
            masm.srl_d(result, result, t7); break;
        default: throw std::invalid_argument("invalid LoongArch operand operation");
    }
}

void JitContext::Lambda(GPR result, const ir::Lambda& lambda) {
    if (lambda.IsValue()) Read(result, lambda.GetValue()); else masm.LI(result, lambda.GetImm().Get());
}

void JitContext::Mask(GPR result, u32 bits) {
    if (!bits || bits > 64) throw std::invalid_argument("invalid LoongArch scalar width");
    if (bits < 64) masm.bstrpick_d(result, result, bits - 1, 0);
}

void JitContext::Sign(GPR result, u32 bits) {
    if (!bits || bits > 64) throw std::invalid_argument("invalid LoongArch scalar width");
    if (bits < 64) { masm.slli_d(result, result, 64 - bits); masm.srai_d(result, result, 64 - bits); }
}

void JitContext::ReadFlags(GPR result) { masm.MV(result, flags); }
void JitContext::WriteFlags(GPR source) {
    masm.MV(flags, source);
    // Emitters can publish within an instruction before the next plan entry.
    // Mark all bits conservatively; the next instruction restores its exact
    // static dirty mask, including partial updates.
    flags_dirty = ir::Flags::All;
}
void JitContext::PublishFlags(bool force) {
    if (!force && flags_publication && flags_dirty == ir::Flags::None) {
        ++flag_stats.skipped_stores;
        return;
    }
    Store(flags, state, state_offset_host_flags);
    ++flag_stats.stores;
    flags_dirty = ir::Flags::None;
}
void JitContext::ReloadFlags() {
    Load(flags, state, state_offset_host_flags);
    ++flag_stats.loads;
    flags_dirty = ir::Flags::None;
}

void JitContext::FlushValues() {
    for (size_t i = 0; i < cache.size(); ++i) if (cache[i].inst && cache[i].dirty) {
        if (cache_values && !IsLive(cache[i].inst)) {
            cache[i] = {}; ++stats.dead_values; continue;
        }
        Store(scalar_registers[i], values, HomeOffset(cache[i].inst, cache[i].part));
        cache[i].dirty = false; ++stats.stores;
    }
}
void JitContext::DiscardValues() { cache.fill({}); next_register = 0; }
void JitContext::BranchZero(GPR value, Label& label, bool zero_test) {
    if (zero_test) masm.BEQ(value, zero, &label); else masm.BNE(value, zero, &label);
}
}  // namespace swift::runtime::backend::loongarch64
