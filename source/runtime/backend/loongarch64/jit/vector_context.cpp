#include "vector_context.h"

#include <algorithm>
#include <stdexcept>
#include "runtime/backend/context.h"

namespace swift::runtime::backend::loongarch64 {
namespace {
using O = ir::OpCode;
constexpr u32 kGatherScratch = 3;

u32 LaneIndex(u64 bits) {
    switch (bits) {
        case 8: return 0;
        case 16: return 1;
        case 32: return 2;
        case 64: return 3;
        default: throw std::invalid_argument("invalid LoongArch LASX lane width");
    }
}

// The positive list in WidePlan owns provenance. This dispatch selects one
// original lane-local V256 operation, never two unrelated V128 instructions.
void WideBinary(Assembler& as, ir::Inst* inst, XVReg result, XVReg left, XVReg right) {
    const auto op = inst->GetOp();
    switch (op) {
        case O::Vec4And: case O::VecAnd: as.xvand_v(result, left, right); return;
        case O::Vec4Or: case O::VecOr: as.xvor_v(result, left, right); return;
        case O::VecXor: as.xvxor_v(result, left, right); return;
        // LASX ANDN has the same ~left & right semantics as LSX.
        case O::VecAndNot: as.xvandn_v(result, right, left); return;
        case O::VecMulHigh16:
            if (inst->GetArg<ir::Imm>(2).Get()) as.xvmuh_h(result, left, right);
            else as.xvmuh_hu(result, left, right);
            return;
        default: break;
    }
    const u64 bits = op == O::Vec4Add || op == O::Vec4Sub || op == O::Vec4Mul ? 32 :
                     inst->GetArg<ir::Imm>(2).Get();
    const auto lane = LaneIndex(bits);
#define LANES(name) \
    switch (lane) { \
        case 0: as.name##_b(result, left, right); break; \
        case 1: as.name##_h(result, left, right); break; \
        case 2: as.name##_w(result, left, right); break; \
        case 3: as.name##_d(result, left, right); break; \
    }
#define UNSIGNED_LANES(name) \
    switch (lane) { \
        case 0: as.name##_bu(result, left, right); break; \
        case 1: as.name##_hu(result, left, right); break; \
        case 2: as.name##_wu(result, left, right); break; \
        case 3: as.name##_du(result, left, right); break; \
    }
    switch (op) {
        case O::Vec4Add: case O::VecAdd: LANES(xvadd); break;
        case O::Vec4Sub: case O::VecSub: LANES(xvsub); break;
        case O::Vec4Mul: case O::VecMul: LANES(xvmul); break;
        case O::VecCmpEq: LANES(xvseq); break;
        case O::VecCmpGt: std::swap(left, right); LANES(xvslt); break;
        case O::VecAvg:
            if (bits != 8 && bits != 16)
                throw std::invalid_argument("LoongArch VecAvg requires byte/halfword lanes");
            UNSIGNED_LANES(xvavgr); break;
        case O::VecMin:
            if (inst->GetArg<ir::Imm>(3).Get()) { LANES(xvmin); }
            else { UNSIGNED_LANES(xvmin); }
            break;
        case O::VecMax:
            if (inst->GetArg<ir::Imm>(3).Get()) { LANES(xvmax); }
            else { UNSIGNED_LANES(xvmax); }
            break;
        case O::VecSatAdd:
            if (inst->GetArg<ir::Imm>(3).Get()) { LANES(xvsadd); }
            else { UNSIGNED_LANES(xvsadd); }
            break;
        case O::VecSatSub:
            if (inst->GetArg<ir::Imm>(3).Get()) { LANES(xvssub); }
            else { UNSIGNED_LANES(xvssub); }
            break;
        default: throw std::invalid_argument("unsupported LoongArch native wide vector operation");
    }
#undef UNSIGNED_LANES
#undef LANES
}
} // namespace

void VectorContext::Configure(ir::Block* block, std::span<const WideVectorPair> wide, bool enabled,
        const RegisterPlan::Targets& targets, const RegisterPlan::Stops& stops,
        const RegisterPlan::Exits& exits, bool cfg_enabled) {
    plan = VectorAllocationPlan::Build(block, context.Features(), wide, targets, stops, exits,
                                     enabled && cfg_enabled && context.Features().lsx);
    allocation = enabled && context.Features().lsx;
    global_cfg = allocation && cfg_enabled && plan->CFGNodes() > 1;
    publication.reset();
    if (global_cfg) publication = VectorPublicationPlan::Build(block, *plan, targets, stops, exits);
    instruction_active = canonical_boundary = false;
    cache.fill({}); pinned.fill(false); current = nullptr; stats = {}; completed_tails.clear();
    stats.global_cfg = global_cfg; stats.cfg_nodes = plan->CFGNodes();
    if (global_cfg) {
        const auto& assigned = plan->AllocationStats();
        stats.assigned_values = assigned.register_values; stats.spilled_values = assigned.spilled_values;
        stats.interference_edges = assigned.interference_edges;
        stats.publication_iterations = publication->Stats().iterations;
    }
}

bool VectorContext::Live(const Entry& entry, const LiveSet& live) const {
    return entry.low && (live.contains(entry.low) || (entry.high && live.contains(entry.high)));
}

void VectorContext::BeginInstruction(ir::Inst* inst) {
    current = inst;
    pinned.fill(false);
    instruction_active = true; canonical_boundary = false;
    // Exit snapshots read mapped VR/XVR values only on the cold branch.
    // Do not publish or change cache cleanliness on the continuation.
    if (inst->GetOp() == O::Checkpoint) return;
    if (global_cfg) {
        if (plan->WideTail(inst)) return;
        if (plan->RegionInstruction(inst)) {
            // Variable lane shifts consume their count through the GPR/raw
            // view, even if that SSA annotation itself owns a mapped vector.
            if (inst->GetOp() == O::VecShiftLeft || inst->GetOp() == O::VecShiftRight ||
                inst->GetOp() == O::VecShiftRightArithmetic) PublishMapped(inst->GetArg<ir::Value>(1).Def());
            return;
        }
        if (VectorAllocationPlan::PreservesRegisters(inst)) {
            // These emitters use GPR/raw homes without clobbering the vector
            // bank. Publish their actual inputs, keeping unrelated SSA values
            // resident across counters, retirement markers and CFG edges.
            PublishCanonicalInputs(inst);
            if (inst->GetOp() == O::Goto || inst->GetOp() == O::NotGoto || inst->GetOp() == O::BindLabel)
                for (auto* value : plan->After(inst)) if (plan->Register(value)) ++stats.edge_preserved;
        } else {
            PublishLive(plan->At(inst)); canonical_boundary = true;
        }
        return;
    }
    // All raw containers, FP emitters, calls, guest memory and CFG entries use
    // canonical homes. Publish before any cold branch is emitted, so a hot
    // path cannot inherit compile-time facts from a helper path it did not run.
    if (!plan || !plan->RegionInstruction(inst)) Barrier();
    else if (inst->GetOp() == O::VecShiftLeft || inst->GetOp() == O::VecShiftRight ||
             inst->GetOp() == O::VecShiftRightArithmetic) {
        const auto* count = inst->GetArg<ir::Value>(1).Def();
        const auto before = stats.store_bytes;
        for (u32 index = 0; index < cache.size(); ++index)
            if (cache[index].low == count || cache[index].high == count) Publish(index);
        stats.boundary_store_bytes += stats.store_bytes - before;
    }
}

void VectorContext::FinishInstruction() {
    pinned.fill(false);
    if (!plan || !current) return;
    if (global_cfg) {
        if (canonical_boundary) {
            // The continuation is after the entire emitter, including FP
            // hot/cold joins and the last memory/atomic ABI call.
            ReloadLive(plan->After(current));
        } else if (!plan->RegionInstruction(current) && current->GetOp() != O::AddPhi &&
                   current->GetOp() != O::BindLabel) {
            ReloadMapped(current);
        }
        instruction_active = false;
        return;
    }
    const auto& live = plan->After(current);
    for (auto& entry : cache) if (entry.low && !Live(entry, live)) {
        stats.dead_values += entry.high ? 2 : 1;
        entry = {};
    }
    instruction_active = false;
}

void VectorContext::PublishMapped(ir::Inst* inst) {
    const auto reg = global_cfg ? plan->Register(inst) : std::nullopt;
    if (!reg) return;
    // These facts come from a CFG fixed point, never from the last emitted
    // path. An inactive terminal/fallthrough phi uses the previous emitter's
    // output state, including any fresh native definition in that instruction.
    if (publication && current && !(instruction_active ? publication->DirtyAt(current, inst) :
                                                       publication->DirtyAfter(current, inst))) {
        ++stats.skipped_publications; stats.skipped_publication_bytes += 16;
        return;
    }
    Store({inst, nullptr, true}, VectorAllocationPlan::kFirstRegister + *reg);
    stats.boundary_store_bytes += 16;
}

void VectorContext::ReloadMapped(ir::Inst* inst) {
    const auto reg = global_cfg ? plan->Register(inst) : std::nullopt;
    if (!reg) return;
    // LocalCache may have just produced one or both parts through GPRs.
    // Make that definition's raw home current before loading the LSX view.
    context.PublishInput(inst);
    LoadHome(VReg{VectorAllocationPlan::kFirstRegister + *reg}, inst);
    stats.boundary_load_bytes += 16;
}

void VectorContext::PublishLive(const LiveSet& live) {
    // Iterate physical slots for deterministic code emission; liveness sets
    // and graph identities otherwise contain process-specific pointer order.
    std::array<ir::Inst*, VectorAllocationPlan::kRegisterCount> assigned{};
    for (auto* value : live) if (auto reg = plan->Register(value)) assigned[*reg] = value;
    for (auto* value : assigned) if (value) PublishMapped(value);
}

void VectorContext::PublishCanonicalInputs(ir::Inst* inst) {
    if (inst->GetOp() == O::AddPhi || inst->GetOp() == O::BindLabel) return;
    if (inst->GetOp() == O::StoreUniform && UniformSourceRegister(inst)) return;
    LiveSet inputs;
    for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
        auto& arg = inst->ArgAt(slot);
        if (arg.IsValue()) inputs.insert(arg.Get<ir::Value>().Def());
        else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) inputs.insert(arg.Get<ir::Lambda>().GetValue().Def());
        else if (arg.IsParams()) for (const auto& param : arg.Get<ir::Params>())
            if (param.data.IsValue()) inputs.insert(param.data.value.Def());
    }
    PublishLive(inputs);
}

void VectorContext::ReloadLive(const LiveSet& live) {
    std::array<ir::Inst*, VectorAllocationPlan::kRegisterCount> assigned{};
    for (auto* value : live) if (auto reg = plan->Register(value)) assigned[*reg] = value;
    for (auto* value : assigned) if (value) ReloadMapped(value);
}

void VectorContext::PublishPhiInput(ir::Inst* inst) { PublishMapped(inst); }
void VectorContext::ReloadPhiDefinition(ir::Inst* inst) { ReloadMapped(inst); }

void VectorContext::ReadCheckpointPart(GPR result, ir::Value value, u32 part) {
    auto& as = context.GetMasm();
    if (global_cfg) {
        if (auto reg = plan->Register(value.Def())) {
            as.vpickve2gr_d(result, VReg{VectorAllocationPlan::kFirstRegister + *reg}, part);
            return;
        }
    } else {
        for (u32 i = 0; i < cache.size(); ++i) {
            const auto& entry = cache[i];
            if (entry.low == value.Def()) {
                as.vpickve2gr_d(result, VReg{VectorAllocationPlan::kFirstRegister + i}, part);
                return;
            }
            if (entry.high == value.Def()) {
                as.xvpickve2gr_d(result, XVReg{VectorAllocationPlan::kFirstRegister + i}, part + 2);
                return;
            }
        }
    }
    context.ReadPart(result, value, part);
}

std::optional<u32> VectorContext::UniformSourceRegister(ir::Inst* inst) const {
    if (!global_cfg) return std::nullopt;
    const auto value = inst->GetArg<ir::Value>(1);
    if (value.Type() != ir::ValueType::V128) return std::nullopt;
    return plan->Register(value.Def());
}

bool VectorContext::StoreUniform(ir::Inst* inst) {
    const auto reg = UniformSourceRegister(inst);
    if (!reg) return false;
    // Preserve the existing pair of 64-bit State stores and their exact
    // offsets. The bridge reads the fixed VR instead of first spilling it to
    // a home, and requires no vector alignment or additional memory access.
    const VReg source{VectorAllocationPlan::kFirstRegister + *reg};
    auto& as = context.GetMasm();
    const auto offset = state_offset_uniform_buffer + inst->GetArg<ir::Uniform>(0).GetOffset();
    as.vpickve2gr_d(t0, source, 0); context.Store(t0, state, offset);
    as.vpickve2gr_d(t0, source, 1); context.Store(t0, state, offset + 8);
    ++stats.register_reads;
    return true;
}

void VectorContext::LoadHome(VReg destination, ir::Inst* value) {
    auto& as = context.GetMasm();
    const auto offset = context.HomeOffset(value);
    // LSX home accesses use byte-granular signed 12-bit offsets. A large or
    // sparse frame keeps the existing full-width address formation.
    if (offset <= 2047) as.vld(destination, values, s32(offset));
    else { context.Address(t0, values, offset); as.vld(destination, t0, 0); }
    ++stats.loads; stats.load_bytes += 16;
}

void VectorContext::StoreHome(VReg source, ir::Inst* value) {
    auto& as = context.GetMasm();
    const auto offset = context.HomeOffset(value);
    if (offset <= 2047) as.vst(source, values, s32(offset));
    else { context.Address(t0, values, offset); as.vst(source, t0, 0); }
    ++stats.stores; stats.store_bytes += 16;
}

void VectorContext::Store(const Entry& entry, u32 reg) {
    auto& as = context.GetMasm();
    if (!entry.high) {
        StoreHome(VReg{reg}, entry.low);
    } else if (context.HomeOffset(entry.high) == context.HomeOffset(entry.low) + 16) {
        context.Address(t0, values, context.HomeOffset(entry.low));
        as.xvst(XVReg{reg}, t0, 0); ++stats.stores; stats.store_bytes += 32;
    } else {
        // A pair can have non-contiguous sparse/dense homes. A full xvst at
        // the low home would overwrite another SSA definition in this case.
        StoreHome(VReg{reg}, entry.low);
        as.xvpermi_q(XVReg{kGatherScratch}, XVReg{reg}, 0x11);
        StoreHome(VReg{kGatherScratch}, entry.high);
        ++stats.scatters; ++stats.permutations;
    }
    context.ReloadDefinition(entry.low);
    if (entry.high) context.ReloadDefinition(entry.high);
}

void VectorContext::Publish(u32 index) {
    auto& entry = cache.at(index);
    if (!entry.low || !entry.dirty) return;
    Store(entry, VectorAllocationPlan::kFirstRegister + index);
    entry.dirty = false;
}

void VectorContext::Barrier() {
    if (global_cfg) {
        if (current && (!instruction_active || !canonical_boundary)) {
            PublishLive(instruction_active ? plan->At(current) : plan->After(current));
            if (instruction_active) canonical_boundary = true;
            ++stats.barriers;
        }
        return;
    }
    pinned.fill(false);
    bool populated{};
    const auto before = stats.store_bytes;
    for (u32 index = 0; index < cache.size(); ++index) if (cache[index].low) {
        populated = true; Publish(index); cache[index] = {};
    }
    if (populated) ++stats.barriers;
    stats.boundary_store_bytes += stats.store_bytes - before;
}

u32 VectorContext::Allocate(ir::Inst* low, ir::Inst* high) {
    // ReadOperand can expose a resident low half. Keep its entire physical
    // slot intact while later operand reads and the result allocate entries.
    // LASX gathers and raw high-half reads still copy to independent scratch.
    for (u32 index = 0; index < cache.size(); ++index) if (!pinned[index] && !cache[index].low) {
        cache[index] = {low, high, false}; return index;
    }
    u32 victim = VectorAllocationPlan::kRegisterCount;
    u32 farthest{};
    for (u32 index = 0; index < cache.size(); ++index) {
        if (pinned[index]) continue;
        const auto& entry = cache[index];
        if (plan && current && !Live(entry, plan->At(current)) && !Live(entry, plan->After(current))) {
            stats.dead_values += entry.high ? 2 : 1;
            cache[index] = {low, high, false}; return index;
        }
        u32 next = plan && current ? plan->NextUse(entry.low, current) : 0;
        if (entry.high && plan && current) next = std::min(next, plan->NextUse(entry.high, current));
        if (victim == cache.size() || next >= farthest) { farthest = next; victim = index; }
    }
    if (victim == cache.size()) throw std::logic_error("all LoongArch vector cache slots are pinned");
    if (cache[victim].dirty) { Publish(victim); ++stats.spills; }
    cache[victim] = {low, high, false};
    return victim;
}

VReg VectorContext::Read(ir::Value value, VReg scratch) {
    auto* def = value.Def();
    auto& as = context.GetMasm();
    if (global_cfg) {
        if (auto reg = plan->Register(def)) {
            const VReg source{VectorAllocationPlan::kFirstRegister + *reg};
            as.vor_v(scratch, source, source); ++stats.register_reads; ++stats.moves;
        } else {
            context.PublishInput(def);
            LoadHome(scratch, def);
        }
        return scratch;
    }
    for (u32 index = 0; index < cache.size(); ++index) {
        const auto& entry = cache[index];
        if (entry.low == def) {
            as.vor_v(scratch, VReg{VectorAllocationPlan::kFirstRegister + index},
                     VReg{VectorAllocationPlan::kFirstRegister + index});
            ++stats.cache_hits; ++stats.moves; return scratch;
        }
        if (entry.high == def) {
            as.xvpermi_q(XVReg{scratch.getIdx()},
                         XVReg{VectorAllocationPlan::kFirstRegister + index}, 0x11);
            ++stats.cache_hits; ++stats.permutations; return scratch;
        }
    }
    context.PublishInput(def);
    const auto index = allocation && plan && current && plan->RegionInstruction(current) ?
                       std::optional{Allocate(def)} : std::nullopt;
    LoadHome(scratch, def);
    if (index) {
        as.vor_v(VReg{VectorAllocationPlan::kFirstRegister + *index}, scratch, scratch);
        ++stats.moves;
    }
    return scratch;
}

VReg VectorContext::ReadOperand(ir::Value value, VReg scratch) {
    if (global_cfg) if (auto reg = plan->Register(value.Def())) {
        ++stats.register_reads; ++stats.direct_reads;
        return VReg{VectorAllocationPlan::kFirstRegister + *reg};
    }
    if (!global_cfg && allocation && instruction_active && plan && plan->RegionInstruction(current)) {
        for (u32 index = 0; index < cache.size(); ++index) if (cache[index].low == value.Def()) {
            pinned[index] = true;
            ++stats.cache_hits; ++stats.direct_reads;
            return VReg{VectorAllocationPlan::kFirstRegister + index};
        }
    }
    return Read(value, scratch);
}

VReg VectorContext::Destination(ir::Inst* inst, VReg scratch) const {
    if (global_cfg && !canonical_boundary) if (auto reg = plan->Register(inst))
        return VReg{VectorAllocationPlan::kFirstRegister + *reg};
    return scratch;
}

void VectorContext::Write(ir::Inst* inst, VReg source) {
    // Local scalar cache entries are already flushed by the emitter. A
    // narrow scalar annotation can also own a whole-CFG GPR; publish its full
    // raw128 container immediately before reloading that scalar definition.
    context.DiscardValues();
    const bool narrow = ScalarAllocationPlan::Eligible(inst);
    if (global_cfg) {
        if (auto reg = plan->Register(inst)) {
            if (canonical_boundary) Store({inst, nullptr, true}, source.getIdx());
            const VReg destination{VectorAllocationPlan::kFirstRegister + *reg};
            if (destination.getIdx() == source.getIdx()) ++stats.direct_writes;
            else { context.GetMasm().vor_v(destination, source, source); ++stats.moves; }
        } else Store({inst, nullptr, true}, source.getIdx());
        return;
    }
    const bool resident = allocation && plan && current && plan->RegionInstruction(current);
    if (!resident || narrow) Store({inst, nullptr, true}, source.getIdx());
    if (resident) {
        const auto index = Allocate(inst);
        auto& as = context.GetMasm();
        as.vor_v(VReg{VectorAllocationPlan::kFirstRegister + index}, source, source);
        cache[index].dirty = !narrow; ++stats.moves;
    }
}

XVReg VectorContext::ReadWide(const std::array<ir::Inst*, 2>& pair, XVReg scratch) {
    auto& as = context.GetMasm();
    if (global_cfg) {
        Read(ir::Value{pair[0]}, VReg{scratch.getIdx()});
        const auto high = ReadOperand(ir::Value{pair[1]}, VReg{kGatherScratch});
        // Only source.Q0 is selected; a mapped LSX value's upper 128 bits
        // are unspecified and must never contribute to the concatenation.
        as.xvpermi_q(scratch, XVReg{high.getIdx()}, 0x02);
        ++stats.gathers; ++stats.permutations;
        return scratch;
    }
    bool halves_cached{};
    for (u32 index = 0; index < cache.size(); ++index) {
        const auto& entry = cache[index];
        if (entry.low == pair[0] && entry.high == pair[1]) {
            as.xvor_v(scratch, XVReg{VectorAllocationPlan::kFirstRegister + index},
                      XVReg{VectorAllocationPlan::kFirstRegister + index});
            stats.cache_hits += 2; ++stats.moves; return scratch;
        }
        halves_cached |= entry.low && (entry.low == pair[0] || entry.low == pair[1] ||
                                       entry.high == pair[0] || entry.high == pair[1]);
    }
    if (halves_cached) {
        Read(ir::Value{pair[0]}, VReg{scratch.getIdx()});
        Read(ir::Value{pair[1]}, VReg{kGatherScratch});
        // imm low nibble 2 selects old destination.Q0, high nibble 0
        // selects source.Q0. The upper bits written by LSX are irrelevant.
        as.xvpermi_q(scratch, XVReg{kGatherScratch}, 0x02);
        ++stats.gathers; ++stats.permutations;
        return scratch;
    }
    context.PublishInput(pair[0]); context.PublishInput(pair[1]);
    const auto index = allocation ? std::optional{Allocate(pair[0], pair[1])} : std::nullopt;
    if (context.HomeOffset(pair[1]) == context.HomeOffset(pair[0]) + 16) {
        context.Address(t0, values, context.HomeOffset(pair[0]));
        as.xvld(scratch, t0, 0); ++stats.loads; stats.load_bytes += 32;
    } else {
        LoadHome(VReg{scratch.getIdx()}, pair[0]);
        LoadHome(VReg{kGatherScratch}, pair[1]);
        as.xvpermi_q(scratch, XVReg{kGatherScratch}, 0x02);
        ++stats.gathers; ++stats.permutations;
    }
    if (index) {
        as.xvor_v(XVReg{VectorAllocationPlan::kFirstRegister + *index}, scratch, scratch);
        ++stats.moves;
    }
    return scratch;
}

void VectorContext::WriteWide(const WideVectorPair& pair, XVReg source) {
    context.DiscardValues();
    if (global_cfg) {
        Write(pair.low, VReg{source.getIdx()});
        const auto high = Destination(pair.high, VReg{kGatherScratch});
        context.GetMasm().xvpermi_q(XVReg{high.getIdx()}, source, 0x11);
        Write(pair.high, high);
        ++stats.scatters; ++stats.permutations;
        return;
    }
    if (!allocation) { Store({pair.low, pair.high, true}, source.getIdx()); return; }
    const auto index = Allocate(pair.low, pair.high);
    context.GetMasm().xvor_v(XVReg{VectorAllocationPlan::kFirstRegister + index}, source, source);
    cache[index].dirty = true; ++stats.moves;
}

bool VectorContext::EmitWide(ir::Inst* inst) {
    if (completed_tails.contains(inst)) return true;
    const auto* pair = plan ? plan->WideHead(inst) : nullptr;
    if (!pair) return false;
    context.FlushValues();
    const auto left = ReadWide(pair->left, XVReg{0});
    const auto right = ReadWide(pair->right, XVReg{1});
    WideBinary(context.GetMasm(), inst, XVReg{2}, left, right);
    WriteWide(*pair, XVReg{2});
    completed_tails.insert(pair->high);
    ++stats.lasx_fusions; ++stats.lasx_operations;
    return true;
}

} // namespace swift::runtime::backend::loongarch64
