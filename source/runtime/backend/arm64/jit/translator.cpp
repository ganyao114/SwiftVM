#pragma once

#include "translator.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <numeric>
#include <sstream>
#include <string_view>
#include <tuple>
#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/common/backedge_control.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {

namespace {

ir::Value ResolveBitCastValue(ir::Value value) {
    while (value.Defined() && value.Def()->IsBitCastOperation()) {
        value = value.Def()->GetArg<ir::Value>(0);
    }
    return value;
}

bool IsA64AddImmediate(u64 value) {
    return value <= 0xfff ||
           ((value & 0xfff) == 0 && (value >> 12) <= 0xfff);
}

bool IsScalarFPRBinary(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::VecFAddScalar32:
        case O::VecFSubScalar32:
        case O::VecFMulScalar32:
        case O::VecFDivScalar32:
        case O::VecFAddScalar64:
        case O::VecFSubScalar64:
        case O::VecFMulScalar64:
        case O::VecFDivScalar64:
            return true;
        default:
            return false;
    }
}

enum class DensityCategory : size_t {
    Flags,
    Uniform,
    MoveWidth,
    NaN,
    Boundary,
    Work,
    Count,
};

// Audit-only, mutually exclusive IR taxonomy. SVM_DENSITY_PROF is default OFF;
// the emitter-window accounting below does not add instructions to guest code.
DensityCategory DensityClass(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::GetFlags:
        case O::SaveFlags:
        case O::BranchOnlyFlags:
        case O::TestFlags:
        case O::TestNotFlags:
        case O::ClearFlags:
        case O::SetCarry:
        case O::SetOverflow:
        case O::InvertCarry:
        case O::PublishFCmpFlags:
        case O::LocalCondSet:
        case O::LocalParitySet:
        case O::FCmpCondSet:
            return DensityCategory::Flags;
        case O::LoadUniform:
        case O::StoreUniform:
        case O::GetUniformAddress:
        case O::UniformBarrier:
            return DensityCategory::Uniform;
        case O::DefineLocal:
        case O::LoadLocal:
        case O::StoreLocal:
        case O::GetHostGPR:
        case O::GetHostFPR:
        case O::SetHostGPR:
        case O::SetHostFPR:
        case O::BitCast:
        case O::GetOperand:
        case O::GetResult:
        case O::LoadImm:
        case O::Zero:
        case O::ZeroExtend32:
        case O::ZeroExtend32To64:
        case O::ZeroExtend64:
        case O::SignExtend:
        case O::VecLoadConst:
        case O::VecSharedZero:
        case O::VecShuffle32:
        case O::VecShuffle32TwoSrc:
        case O::VecShuffle32Indexed:
        case O::VecShuffle16:
        case O::VecZip:
        case O::VecUnzip:
        case O::VecDupPairs32:
        case O::VecDup64:
        case O::VecExtract64:
        case O::VecExtract16:
        case O::VecInsert16:
        case O::VecTableLookup8:
            return DensityCategory::MoveWidth;
        case O::VecFAddScalar32:
        case O::VecFSubScalar32:
        case O::VecFMulScalar32:
        case O::VecFDivScalar32:
        case O::VecFAddScalar64:
        case O::VecFSubScalar64:
        case O::VecFMulScalar64:
        case O::VecFDivScalar64:
        case O::VecFAdd:
        case O::VecFSub:
        case O::VecFMul:
        case O::VecFDiv:
        case O::VecFUnary:
            return DensityCategory::NaN;
        case O::Goto:
        case O::NotGoto:
        case O::BindLabel:
        case O::Nop:
        case O::AdvancePC:
        case O::SetLocation:
        case O::GetLocation:
        case O::PushRSB:
        case O::PopRSB:
        case O::AddPhi:
        case O::BranchOnlyEdges:
            return DensityCategory::Boundary;
        default:
            return DensityCategory::Work;
    }
}

bool DensityScalarFP(ir::OpCode op) {
    using O = ir::OpCode;
    switch (op) {
        case O::VecFAddScalar32:
        case O::VecFSubScalar32:
        case O::VecFMulScalar32:
        case O::VecFDivScalar32:
        case O::VecFAddScalar64:
        case O::VecFSubScalar64:
        case O::VecFMulScalar64:
        case O::VecFDivScalar64:
            return true;
        default:
            return false;
    }
}

}  // namespace

#define __ masm.

JitTranslator::JitTranslator(JitContext& ctx) : context(ctx), masm(ctx.GetMasm()) {
    auto& config = ctx.GetConfig();
    use_memory_base = config.memory_base != nullptr || config.page_table != nullptr;
    guest_addr_mask = config.guest_addr_mask;
    window_uxtw = guest_addr_mask == 0xFFFFFFFFull;
    mem_hostbase_fold = config.mem_hostbase_fold;
    induct_tie = config.induct_tie;
    sse_scalar_insert = config.sse_scalar_insert;
    sse_afp_nan = config.sse_afp_nan;
    const auto& features = ctx.GetFeatures();
    sse_scalar_tie = features.sse_scalar_tie;
    sse_shufps_imm = features.sse_shufps_imm;
    sse_afp_minmax = features.sse_afp_minmax && sse_afp_nan;
    shift_imm_fast = features.shift_imm_fast;
    mem_narrow_fuse = features.mem_narrow_fuse;
    addr_ea_tie = features.addr_ea_tie;
    abs_const_mat = features.abs_const_mat;
    sse_nan_coldpath = features.sse_nan_coldpath;
    direct_cycle_latch = BackedgeLatchEnabled() || config.region_edges;
    backedge_latch = direct_cycle_latch || config.region_edges;
    // The legacy process-wide switch and the unit FeatureSet select the same
    // proven representation.  No lazy state may exist without a cycle
    // observer: RE=0/LATCH=0 therefore remains the ordinary committed path.
    backedge_flags = backedge_latch &&
                     (GetSvmConfig().backedge_flags || features.flags_loop_lazy);
    region_branch_flags = features.flags_region_branch;
    execution_trace_enabled = context.ExecutionTraceEnabled();
    if (execution_trace_enabled) {
        for (const auto& desc : config.buffers_static_alloc) {
            if (!desc.is_float && desc.size == sizeof(u64) &&
                desc.offset == offsetof(swift::x86::ThreadContext64, rsp)) {
                execution_trace_rsp_reg = desc.reg;
                break;
            }
        }
        // 当前探针只定义了 x86/x86-64 的 RSP 语义；其他前端不发探针码。
        execution_trace_enabled = execution_trace_rsp_reg >= 0;
    }
}

void JitTranslator::EmitExecutionTrace(u64 guest_rip) {
    if (!execution_trace_enabled) return;
    context.RecordExecutionTrace(guest_rip,
                                 XRegister(execution_trace_rsp_reg));
}



std::optional<u64> JitTranslator::MatchInductionImmediate(ir::Inst* inst) {
    if (!induct_tie || !inst || inst->GetOp() != ir::OpCode::Add ||
        ir::GetValueSizeByte(inst->ReturnType()) != sizeof(u64)) {
        return std::nullopt;
    }
    const auto right = inst->GetArg<ir::Operand>(1);
    if (!right.GetRight().Null()) {
        return std::nullopt;
    }
    u64 value{};
    if (right.GetLeft().IsImm() && context.GetFeatures().int_imm_fold) {
        value = right.GetLeft().imm.Get();
    } else if (right.GetLeft().IsValue()) {
        const auto immediate = right.GetLeft().value;
        if (!immediate.Def() || immediate.Def()->GetOp() != ir::OpCode::LoadImm ||
            immediate.Def()->GetUses() == 0 ||
            ir::GetValueSizeByte(immediate.Type()) != sizeof(u64)) {
            return std::nullopt;
        }
        value = immediate.Def()->GetArg<ir::Imm>(0).Get();
    } else {
        return std::nullopt;
    }
    if (!IsA64AddImmediate(value)) {
        return std::nullopt;
    }

    const auto source = ResolveBitCastValue(inst->GetArg<ir::Value>(0));
    if (!source.Def() || source.Def()->GetOp() != ir::OpCode::GetHostGPR ||
        !context.SharesGPR(source, ir::Value{inst})) {
        return std::nullopt;
    }

    // Hard emitter guard: re-prove that the in-place result is published to
    // the same architectural pin before any observer/faulting instruction.
    const u64 source_host = source.Def()->GetArg<ir::Imm>(0).Get();
    u32 result_end = inst->Id();
    for (auto& next : cur_block->GetInstList()) {
        for (auto value : next.GetValues()) {
            if (ResolveBitCastValue(value).Def() == inst) {
                result_end = std::max<u32>(result_end, next.Id());
            }
        }
    }
    bool after_add = false;
    bool published = false;
    for (auto& next : cur_block->GetInstList()) {
        if (&next == inst) {
            after_add = true;
            continue;
        }
        if (!after_add) {
            continue;
        }
        if (next.GetOp() == ir::OpCode::SetHostGPR) {
            const auto next_value = ResolveBitCastValue(next.GetArg<ir::Value>(0));
            if (!published) {
                if (next_value.Def() != inst ||
                    next.GetArg<ir::Imm>(1).Get() != source_host ||
                    next.GetArg<ir::Imm>(2).Get() != 0) {
                    return std::nullopt;
                }
                published = true;
                continue;
            }
            if (next.Id() <= result_end &&
                next.GetArg<ir::Imm>(1).Get() == source_host) {
                return std::nullopt;
            }
            continue;
        }
        if (published) {
            continue;
        }
        if (next.GetOp() == ir::OpCode::SaveFlags) {
            if (next.GetArg<ir::Flags>(1) != ir::Flags::None) {
                return std::nullopt;
            }
            continue;
        }
        if (next.GetOp() != ir::OpCode::LoadImm && !next.IsBitCastOperation()) {
            return std::nullopt;
        }
    }
    return published ? std::optional<u64>{value} : std::nullopt;
}

void JitTranslator::PlanInductionTies(ir::Block* block) {
    if (!induct_tie) {
        return;
    }
    std::map<ir::Inst*, u32> matched_uses;
    for (auto& inst : block->GetInstList()) {
        if (!MatchInductionImmediate(&inst)) {
            continue;
        }
        const auto right = inst.GetArg<ir::Operand>(1).GetLeft();
        // Canonical immediates have no LoadImm instruction to suppress.  The
        // match still carries the in-place ownership proof into EmitAdd.
        if (right.IsValue()) {
            ++matched_uses[right.value.Def()];
        }
    }
    for (const auto& [load, uses] : matched_uses) {
        // A CSE'd induction constant (Copy's shared #80) is removable only
        // when every SSA consumer is a guarded in-place immediate site.
        if (uses == load->GetUses()) {
            disable_instructions.set(load->Id());
        }
    }
}

void JitTranslator::ResetBoundaryDensity() {
    boundary_density_enabled = context.DensityProfileEnabled();
    boundary_terminal_open = false;
    boundary_terminal_link_bytes = 0;
    boundary_density_bytes.fill(0);
    for (auto& mnemonics : boundary_density_mnemonics) {
        mnemonics.clear();
    }
    boundary_terminal_link_mnemonics.clear();
    boundary_terminal_link_ranges.clear();
}

void JitTranslator::RecordBoundaryRange(BoundarySubsequence category,
                                        u32 begin,
                                        u32 end) {
    if (!boundary_density_enabled || begin == end) {
        return;
    }
    ASSERT(end > begin);
    ASSERT((end - begin) % vixl::aarch64::kInstructionSize == 0);
    const auto index = static_cast<size_t>(category);
    boundary_density_bytes[index] += end - begin;

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    const auto* bytes = masm.GetBuffer()->GetStartAddress<const u8*>();
    for (u32 offset = begin; offset < end;
         offset += vixl::aarch64::kInstructionSize) {
        const auto* instruction =
                reinterpret_cast<const vixl::aarch64::Instruction*>(bytes + offset);
        decoder.Decode(instruction);
        std::string_view text{disassembler.GetOutput()};
        const auto first = text.find_first_not_of(" \t");
        if (first == std::string_view::npos) {
            continue;
        }
        text.remove_prefix(first);
        const auto last = text.find_first_of(" \t");
        const std::string mnemonic{text.substr(0, last)};
        ++boundary_density_mnemonics[index][mnemonic];
        if (category == BoundarySubsequence::LinkTail && boundary_terminal_open) {
            ++boundary_terminal_link_mnemonics[mnemonic];
        }
    }
    if (category == BoundarySubsequence::LinkTail && boundary_terminal_open) {
        boundary_terminal_link_bytes += end - begin;
        boundary_terminal_link_ranges.emplace_back(begin, end);
    }
}

void JitTranslator::PrintBoundaryDensity(u64 guest_pc,
                                         u32 expected_boundary_bytes) {
    const auto terminal = static_cast<size_t>(BoundarySubsequence::TerminalMain);
    ASSERT(boundary_density_bytes[terminal] >= boundary_terminal_link_bytes);
    boundary_density_bytes[terminal] -= boundary_terminal_link_bytes;
    for (const auto& [mnemonic, count] : boundary_terminal_link_mnemonics) {
        auto it = boundary_density_mnemonics[terminal].find(mnemonic);
        ASSERT(it != boundary_density_mnemonics[terminal].end());
        ASSERT(it->second >= count);
        it->second -= count;
        if (it->second == 0) {
            boundary_density_mnemonics[terminal].erase(it);
        }
    }

    const u32 subtotal = std::accumulate(boundary_density_bytes.begin(),
                                         boundary_density_bytes.end(), 0u);
    // Strict unit-local commoning proof: two byte-identical final AArch64
    // instructions can be emitted once, with each duplicate site replaced by
    // a one-instruction B. Therefore each duplicate beyond the first has an
    // exact one-instruction (4-byte) removable ceiling. Restrict this to link
    // leaves inside the terminal; body-side RSB pushes and cold veneers cannot
    // accidentally enter the proof.
    std::map<u64, u32> common_suffixes;
    const auto* host_bytes = masm.GetBuffer()->GetStartAddress<const u8*>();
    for (const auto& [begin, end] : boundary_terminal_link_ranges) {
        if (end - begin < 2 * vixl::aarch64::kInstructionSize) {
            continue;
        }
        u64 suffix{};
        std::memcpy(&suffix, host_bytes + end - sizeof(suffix), sizeof(suffix));
        ++common_suffixes[suffix];
    }
    u32 common2_groups = 0;
    u32 common2_ranges = 0;
    u32 common2_saved = 0;
    for (const auto& [suffix, count] : common_suffixes) {
        if (count < 2) {
            continue;
        }
        ++common2_groups;
        common2_ranges += count;
        common2_saved +=
                (count - 1) * vixl::aarch64::kInstructionSize;
    }
    std::fprintf(stderr,
                 "[svm-boundary] pc=0x%llx bytes_prologue=%u "
                 "bytes_terminal=%u bytes_link=%u bytes_cold=%u "
                 "bytes_total=%u expected_boundary=%u "
                 "common2_groups=%u common2_ranges=%u common2_saved=%u",
                 static_cast<unsigned long long>(guest_pc),
                 boundary_density_bytes[0], boundary_density_bytes[1],
                 boundary_density_bytes[2], boundary_density_bytes[3], subtotal,
                 expected_boundary_bytes, common2_groups, common2_ranges,
                 common2_saved);
    static constexpr std::array<const char*, 4> names{
            "prologue", "terminal", "link", "cold"};
    for (size_t i = 0; i < names.size(); ++i) {
        std::fprintf(stderr, " mn_%s=", names[i]);
        if (boundary_density_mnemonics[i].empty()) {
            std::fputc('-', stderr);
            continue;
        }
        bool first = true;
        for (const auto& [mnemonic, count] : boundary_density_mnemonics[i]) {
            std::fprintf(stderr, "%s%s:%u", first ? "" : ",",
                         mnemonic.c_str(), count);
            first = false;
        }
    }
    std::fputc('\n', stderr);
}



JitTranslator::BlockTranslateState
JitTranslator::PrepareBlockState(ir::Block* block) {
    cur_block = block;
    const auto& loop_hoist = block->GetLoopHoistMetadata();
    loop_hoist_body_entry = loop_hoist.prefix_end
            ? std::make_unique<Label>()
            : nullptr;
    u32 loop_hoist_prefix_begin = 0;
    u32 loop_hoist_prefix_ops = 0;
    ASSERT_MSG(!loop_hoist_body_entry || HasSelfEdge(block->GetTerminal()),
               "loop-hoist prefix without a self edge at {:#x}",
               block->GetStartLocation().Value());
    ASSERT(direct_cycle_exits.empty());
    direct_cycle_cut_edges = 0;
    region_block_edges = 0;
    region_block_cycles = 0;
    region_block_fallthroughs = 0;
    region_block_local_branch_bytes = 0;
    cur_block_is_call = false;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() == ir::OpCode::PushRSB) {
            cur_block_is_call = true;
            break;
        }
    }
    static_next_loc.reset();
    dynamic_next_loc.reset();
    // Function mode keeps one function-sized suppression bitmap. Grow it
    // before the per-block backedge proof marks the two sunk IR instructions.
    disable_instructions.resize(
            std::max<size_t>(disable_instructions.size(), block->MaxInstrId()));
    backedge_flags_plan = PlanBackedgeFlags(block);
    if (backedge_flags_plan) {
        if (backedge_flags_plan->polarity_load) {
            disable_instructions.set(backedge_flags_plan->polarity_load->Id());
        }
        disable_instructions.set(backedge_flags_plan->polarity_store->Id());
    }
    backedge_exit_referenced = false;
    backedge_exit_label =
            backedge_latch &&
                            (HasSelfEdge(block->GetTerminal()) ||
                             HasRegionCycleEdgeFromCurrent() ||
                             (backedge_flags_plan &&
                              backedge_flags_plan->dead_successor &&
                              IsDirectCycleCutEdge(
                                      backedge_flags_plan->self_target)))
                    ? std::make_unique<Label>()
                    : nullptr;
    const bool split_flags_entry = backedge_flags_plan &&
                                   !backedge_flags_plan->dead_successor;
    context.SetCurrent(block, split_flags_entry);
    if (region_edges_active) {
        context.BindInternalEntry(block->GetStartLocation().Value());
    }
    PlanInductionTies(block);
    return {&loop_hoist,
            loop_hoist_prefix_begin,
            loop_hoist_prefix_ops,
            split_flags_entry};
}

void JitTranslator::PlacementPoint(const char* kind, u64 guest_pc) {
    const u32 mode = GetSvmConfig().placement_pad;
    if (mode == 0) return;
    const u32 before = context.CurrentBufferSize();
    if (mode == 2) {
        std::fprintf(stderr,
                     "[svm-placement-reference] kind=%s unit=0x%llx pc=0x%llx offset=%u\n",
                     kind, static_cast<unsigned long long>(placement_unit_pc),
                     static_cast<unsigned long long>(guest_pc), before);
        return;
    }
    using Key = std::tuple<std::string, u64, u64>;
    static const std::map<Key, u32> references = [] {
        std::map<Key, u32> result;
        std::ifstream input{"/private/tmp/svm-placement-pad-reference.tsv"};
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream fields{line};
            std::string point;
            std::string unit_text;
            std::string pc_text;
            u32 offset{};
            if (!(fields >> point >> unit_text >> pc_text >> offset)) continue;
            result.emplace(Key{point, std::stoull(unit_text, nullptr, 0),
                               std::stoull(pc_text, nullptr, 0)}, offset);
        }
        return result;
    }();
    const auto it = references.find(Key{kind, placement_unit_pc, guest_pc});
    if (it == references.end()) {
        std::fprintf(stderr,
                     "[svm-placement-miss] kind=%s unit=0x%llx pc=0x%llx before=%u\n",
                     kind, static_cast<unsigned long long>(placement_unit_pc),
                     static_cast<unsigned long long>(guest_pc), before);
        return;
    }
    if (before > it->second) {
        std::fprintf(stderr,
                     "[svm-placement-overrun] kind=%s unit=0x%llx pc=0x%llx before=%u target=%u\n",
                     kind, static_cast<unsigned long long>(placement_unit_pc),
                     static_cast<unsigned long long>(guest_pc), before, it->second);
        return;
    }
    const u32 ops = (it->second - before) / sizeof(u32);
    ASSERT_MSG(before + ops * sizeof(u32) == it->second,
               "placement reference is not instruction aligned");
    for (u32 i = 0; i < ops; ++i) __ Nop();
    if (ops != 0) {
        std::fprintf(stderr,
                     "[svm-placement-pad] kind=%s unit=0x%llx pc=0x%llx before=%u after=%u ops=%u\n",
                     kind, static_cast<unsigned long long>(placement_unit_pc),
                     static_cast<unsigned long long>(guest_pc), before,
                     context.CurrentBufferSize(), ops);
    }
}

JitTranslator::UniformDensityCounts
JitTranslator::CollectUniformDensity(ir::Block* block, bool density) {
    // Count the remaining x86 GPR uniform-buffer traffic dynamically without
    // instrumenting each access: add the block's static emitted access count
    // once on entry. ThreadContext64 begins with the 16 8-byte GPRs, so the
    // first 128 uniform bytes are exactly the register-residency region.
    u32 gpr_uniform_accesses = 0;
    u32 xmm_uniform_accesses = 0;
    constexpr u32 kXmmBegin = offsetof(swift::x86::ThreadContext64, xmms);
    constexpr u32 kXmmEnd = kXmmBegin + sizeof(swift::x86::ThreadContext64::xmms);
    for (auto& inst : block->GetInstList()) {
        if ((inst.GetOp() == ir::OpCode::LoadUniform ||
             inst.GetOp() == ir::OpCode::StoreUniform)) {
            const u32 offset = inst.GetArg<ir::Uniform>(0).GetOffset();
            if (offset < 16 * sizeof(u64)) {
                ++gpr_uniform_accesses;
            } else if (offset >= kXmmBegin && offset < kXmmEnd) {
                ++xmm_uniform_accesses;
            }
        }
    }
    if (density) {
        std::array<u32, 16> xmm_loads{};
        std::array<u32, 16> xmm_stores{};
        std::array<u32, 5> xmm_load_widths{};
        std::array<u32, 5> xmm_store_widths{};
        u32 xmm_load_gpr{};
        u32 xmm_load_fpr{};
        u32 xmm_store_gpr{};
        u32 xmm_store_fpr{};
        auto width_slot = [](u32 size) -> std::optional<size_t> {
            switch (size) {
                case 1: return 0;
                case 2: return 1;
                case 4: return 2;
                case 8: return 3;
                case 16: return 4;
                default: return std::nullopt;
            }
        };
        for (auto& inst : block->GetInstList()) {
            const bool load = inst.GetOp() == ir::OpCode::LoadUniform;
            const bool store = inst.GetOp() == ir::OpCode::StoreUniform;
            if (!load && !store) {
                continue;
            }
            const auto uniform = inst.GetArg<ir::Uniform>(0);
            const u32 offset = uniform.GetOffset();
            const u32 size = load
                    ? ir::GetValueSizeByte(inst.ReturnType())
                    : ir::GetValueSizeByte(inst.GetArg<ir::Value>(1).Type());
            if (offset < kXmmBegin || offset + size > kXmmEnd) {
                continue;
            }
            const u32 relative = offset - kXmmBegin;
            const u32 index = relative / sizeof(swift::x86::Xmm);
            if (index >= 16 || relative + size > (index + 1) * sizeof(swift::x86::Xmm)) {
                continue;
            }
            if (load) {
                ++xmm_loads[index];
                if (ir::IsFloatValueType(inst.ReturnType())) {
                    ++xmm_load_fpr;
                } else {
                    ++xmm_load_gpr;
                }
            } else {
                ++xmm_stores[index];
                if (ir::IsFloatValueType(inst.GetArg<ir::Value>(1).Type())) {
                    ++xmm_store_fpr;
                } else {
                    ++xmm_store_gpr;
                }
            }
            if (const auto slot = width_slot(size)) {
                ++(load ? xmm_load_widths[*slot] : xmm_store_widths[*slot]);
            }
        }
        const u32 load_total = std::accumulate(xmm_loads.begin(), xmm_loads.end(), 0u);
        const u32 store_total = std::accumulate(xmm_stores.begin(), xmm_stores.end(), 0u);
        if (load_total || store_total) {
            std::fprintf(stderr,
                         "[svm-xmm-state] pc=0x%llx loads=%u stores=%u "
                         "load_gpr=%u load_fpr=%u store_gpr=%u store_fpr=%u "
                         "load_w1=%u load_w2=%u load_w4=%u load_w8=%u load_w16=%u "
                         "store_w1=%u store_w2=%u store_w4=%u store_w8=%u store_w16=%u\n",
                         static_cast<unsigned long long>(block->GetStartLocation().Value()),
                         load_total, store_total,
                         xmm_load_gpr, xmm_load_fpr, xmm_store_gpr, xmm_store_fpr,
                         xmm_load_widths[0], xmm_load_widths[1], xmm_load_widths[2],
                         xmm_load_widths[3], xmm_load_widths[4],
                         xmm_store_widths[0], xmm_store_widths[1], xmm_store_widths[2],
                         xmm_store_widths[3], xmm_store_widths[4]);
            for (u32 index = 0; index < 16; ++index) {
                if (!xmm_loads[index] && !xmm_stores[index]) {
                    continue;
                }
                std::fprintf(stderr,
                             "[svm-xmm-reg] pc=0x%llx index=%u loads=%u stores=%u\n",
                             static_cast<unsigned long long>(block->GetStartLocation().Value()),
                             index, xmm_loads[index], xmm_stores[index]);
            }
        }
    }
    return {gpr_uniform_accesses, xmm_uniform_accesses};
}

void JitTranslator::TranslateBlockInstructions(
        ir::Block* block,
        const ir::LoopHoistMetadata& loop_hoist,
        bool density,
        bool gap_audit,
        std::span<u32> density_ops,
        std::span<u32> density_bytes,
        u32& density_scalar_fp_ops,
        u32& loop_hoist_prefix_begin,
        u32& loop_hoist_prefix_ops) {
    loop_hoist_prefix_begin = context.CurrentBufferSize();
    VAddr audit_guest_pc = block->GetStartLocation().Value();
    for (auto& inst : block->GetInstList()) {
        auto category = DensityCategory::Work;
        if (density) {
            category = DensityClass(inst.GetOp());
            ++density_ops[static_cast<size_t>(category)];
            density_scalar_fp_ops += DensityScalarFP(inst.GetOp());
        }
        cur_instr = &inst;
        if (inst.Id() < disable_instructions.size() && disable_instructions.test(inst.Id())) {
            continue;
        }
        const u32 before = density ? context.CurrentBufferSize() : 0;
        const u32 nan_before = density ? context.DensityNaNBytes() : 0;
        const bool audit_advance = gap_audit &&
                inst.GetOp() == ir::OpCode::AdvancePC;
        const bool audit_nzcv_dirty = audit_advance && save_in_nzcv && nzcv_dirty;
        const u64 audit_nzcv_requested = audit_advance
                ? static_cast<u64>(nzcv_requested)
                : 0;
        Translate(&inst);
        if (loop_hoist_body_entry && &inst == loop_hoist.prefix_end) {
            loop_hoist_prefix_ops =
                    (context.CurrentBufferSize() - loop_hoist_prefix_begin) /
                    sizeof(u32);
            PlacementPoint("body", block->GetStartLocation().Value());
            __ Bind(loop_hoist_body_entry.get());
        }
        if (density) {
            const u32 emitted = context.CurrentBufferSize() - before;
            const u32 nan_emitted = context.DensityNaNBytes() - nan_before;
            density_bytes[static_cast<size_t>(DensityCategory::NaN)] += nan_emitted;
            const auto emitted_category =
                    category == DensityCategory::NaN
                            ? DensityCategory::Work
                            : (inst.GetOp() == ir::OpCode::AdvancePC
                                       ? DensityCategory::Flags
                                       : category);
            density_bytes[static_cast<size_t>(emitted_category)] +=
                    emitted - nan_emitted;
            if (category == DensityCategory::Boundary &&
                inst.GetOp() != ir::OpCode::AdvancePC) {
                RecordBoundaryRange(inst.GetOp() == ir::OpCode::PushRSB
                                            ? BoundarySubsequence::LinkTail
                                            : BoundarySubsequence::TerminalMain,
                                    before, context.CurrentBufferSize());
            }
            if (gap_audit) {
                const u32 production_emitted =
                        emitted - context.HotProbeBytesInRange(before,
                                                               context.CurrentBufferSize());
                const bool scalar_binary = IsScalarFPRBinary(inst.GetOp());
                const bool scalar_tied = scalar_binary && context.SharesFPR(
                        ir::Value{&inst}, inst.GetArg<ir::Value>(0));
                const bool shufps = inst.GetOp() == ir::OpCode::VecShuffle32TwoSrc;
                const u32 shufps_imm = shufps ? inst.GetArg<ir::Imm>(2).Get() & 0xffu : 0;
                const bool shufps_alias = shufps &&
                        inst.GetArg<ir::Value>(0).Id() == inst.GetArg<ir::Value>(1).Id();
                const bool shufps_left_tied = shufps && context.SharesFPR(
                        ir::Value{&inst}, inst.GetArg<ir::Value>(0));
                const bool shufps_left_fixed = shufps && context.IsHostReadCoalesced(
                        ResolveBitCastValue(inst.GetArg<ir::Value>(0)).Id());
                std::fprintf(stderr,
                             "[svm-gap-op] block=0x%llx guest_pc=0x%llx id=%u "
                             "op=%s bytes=%u host_offset=%u scalar_binary=%u scalar_tied=%u "
                             "shufps=%u shufps_imm=%u shufps_alias=%u "
                             "shufps_left_tied=%u shufps_left_fixed=%u "
                             "advpc_nzcv_dirty=%u advpc_nzcv_requested=0x%llx\n",
                             static_cast<unsigned long long>(
                                     block->GetStartLocation().Value()),
                             static_cast<unsigned long long>(audit_guest_pc),
                             inst.Id(), ir::GetIRMetaInfo(inst.GetOp()).name,
                             production_emitted, before,
                             scalar_binary ? 1u : 0u, scalar_tied ? 1u : 0u,
                             shufps ? 1u : 0u, shufps_imm,
                             shufps_alias ? 1u : 0u, shufps_left_tied ? 1u : 0u,
                             shufps_left_fixed ? 1u : 0u,
                             audit_nzcv_dirty ? 1u : 0u,
                             static_cast<unsigned long long>(audit_nzcv_requested));
            }
        }
        if (inst.GetOp() == ir::OpCode::AdvancePC) {
            audit_guest_pc += inst.GetArg<ir::Imm>(0).Get();
        }
    }
}

void JitTranslator::EmitBlockTerminalAndColdPaths(
        ir::Block* block,
        bool density,
        std::span<u32> density_bytes) {
    context.BeginTerminalScratch();
    const u32 flags_before = density ? context.CurrentBufferSize() : 0;
    FlushFlags();
    if (density) {
        density_bytes[static_cast<size_t>(DensityCategory::Flags)] +=
                context.CurrentBufferSize() - flags_before;
    }
    const u32 terminal_before = density ? context.CurrentBufferSize() : 0;
    boundary_terminal_open = density;
    if (!EmitBackedgeFlagsTerminal(block->GetTerminal())) {
        EmitTerminal(block->GetTerminal());
    }
    boundary_terminal_open = false;
    context.EndTerminalScratch();
    if (density) {
        RecordBoundaryRange(BoundarySubsequence::TerminalMain, terminal_before,
                            context.CurrentBufferSize());
        density_bytes[static_cast<size_t>(DensityCategory::Boundary)] +=
                context.CurrentBufferSize() - terminal_before;
    }
    if (backedge_flags_plan) {
        backedge_host_end = context.CurrentBufferSize();
    }
    // Close the W71 accounting window before out-of-line NaN repair stubs.
    // The hot guard remains in the block; cold handlers are not executed on
    // the normal path and therefore do not belong in static x entry counts.
    context.FinishHotCoalesceBlock();
    context.BeginColdScratch();
    const u32 boundary_cold_before = density ? context.CurrentBufferSize() : 0;
    EmitBackedgeExitStub();
    EmitDirectCycleExitStubs();
    EmitBackedgeColdPaths();
    if (density) {
        RecordBoundaryRange(BoundarySubsequence::ColdTail, boundary_cold_before,
                            context.CurrentBufferSize());
        density_bytes[static_cast<size_t>(DensityCategory::Boundary)] +=
                context.CurrentBufferSize() - boundary_cold_before;
    }
    const u32 nan_cold_before = density ? context.CurrentBufferSize() : 0;
    EmitVecNaNColdPaths();
    if (density) {
        density_bytes[static_cast<size_t>(DensityCategory::NaN)] +=
                context.CurrentBufferSize() - nan_cold_before;
    }
    context.EndColdScratch();
}

void JitTranslator::PrintBlockDensity(
        ir::Block* block,
        bool density,
        std::span<const u32> density_ops,
        std::span<const u32> density_bytes,
        u32 density_scalar_fp_ops,
        const ir::LoopHoistMetadata& loop_hoist,
        u32 loop_hoist_prefix_ops) {
    if (density) {
        const u32 total_ops = std::accumulate(density_ops.begin(), density_ops.end(), 0u);
        const u32 total_bytes =
                std::accumulate(density_bytes.begin(), density_bytes.end(), 0u);
        std::fprintf(stderr,
                     "[svm-density] pc=0x%llx ops_flags=%u ops_uniform=%u "
                     "ops_move=%u ops_nan=%u ops_boundary=%u ops_work=%u "
                     "bytes_flags=%u bytes_uniform=%u bytes_move=%u bytes_nan=%u "
                     "bytes_boundary=%u bytes_work=%u ops_total=%u bytes_total=%u "
                     "ops_fp_scalar=%u pf_write_bytes=%u pf_read_bytes=%u "
                     "af_write_bytes=%u af_read_bytes=%u pfaf_shared_bytes=%u "
                     "whole_flags_bytes=%u pf_write_sites=%u pf_read_sites=%u "
                     "af_write_sites=%u af_read_sites=%u pfaf_shared_sites=%u "
                     "whole_flags_sites=%u\n",
                     static_cast<unsigned long long>(block->GetStartLocation().Value()),
                     density_ops[0], density_ops[1], density_ops[2], density_ops[3],
                     density_ops[4], density_ops[5], density_bytes[0], density_bytes[1],
                     density_bytes[2], density_bytes[3], density_bytes[4], density_bytes[5],
                     total_ops, total_bytes, density_scalar_fp_ops,
                     pfaf_density_bytes[0] & 0xffffu,
                     pfaf_density_bytes[1] & 0xffffu,
                     pfaf_density_bytes[2] & 0xffffu,
                     pfaf_density_bytes[3] & 0xffffu,
                     pfaf_density_bytes[4] & 0xffffu,
                     pfaf_density_bytes[5] & 0xffffu,
                     pfaf_density_bytes[0] >> 16,
                     pfaf_density_bytes[1] >> 16,
                     pfaf_density_bytes[2] >> 16,
                     pfaf_density_bytes[3] >> 16,
                     pfaf_density_bytes[4] >> 16,
                     pfaf_density_bytes[5] >> 16);
        PrintBoundaryDensity(block->GetStartLocation().Value(), density_bytes[4]);
    }
    if (region_edges_active && (density || context.ExecProfileEnabled())) {
        std::fprintf(stderr,
                     "[svm-region-edge] pc=0x%llx edges=%u cycles=%u "
                     "fallthrough=%u local_branch_bytes=%u poll_bytes=%u\n",
                     static_cast<unsigned long long>(
                             block->GetStartLocation().Value()),
                     region_block_edges,
                     region_block_cycles,
                     region_block_fallthroughs,
                     region_block_local_branch_bytes,
                     static_cast<u32>(region_block_cycles * 2u * sizeof(u32)));
    }
    if (density && direct_cycle_cut_edges) {
        std::fprintf(stderr,
                     "[svm-direct-cycle-cover] pc=0x%llx edges=%u poll_bytes=%u\n",
                     static_cast<unsigned long long>(
                             block->GetStartLocation().Value()),
                     direct_cycle_cut_edges,
                     static_cast<u32>(direct_cycle_cut_edges * 2u * sizeof(u32)));
    }
    if (density && loop_hoist.prefix_end) {
        std::fprintf(stderr,
                     "[svm-loop-hoist] pc=0x%llx gpr=%u const=%u prefix_ops=%u\n",
                     static_cast<unsigned long long>(
                             block->GetStartLocation().Value()),
                     loop_hoist.gpr_count, loop_hoist.const_count,
                     loop_hoist_prefix_ops);
    }
    loop_hoist_body_entry.reset();
}

void JitTranslator::Translate(ir::Block* block) {
    vixl::svm_vixl_prof::JitScope vixl_prof{context.GetFeatures().vixl_fast};
    ASSERT(vec_nan_cold_sites.empty());
    if (!translating_function) {
        placement_unit_pc = block->GetStartLocation().Value();
    }
    // Keep entry padding outside the block density/hot accounting window.  It
    // is reached only on the first fallthrough; every self edge targets the
    // label bound after it.
    PlacementPoint("block", block->GetStartLocation().Value());
    const bool density = context.DensityProfileEnabled();
    const bool gap_audit = density && GetSvmConfig().ra_hot_coalesce_all;
    ResetBoundaryDensity();
    const u32 density_start = density ? context.CurrentBufferSize() : 0;
    std::array<u32, static_cast<size_t>(DensityCategory::Count)> density_ops{};
    std::array<u32, static_cast<size_t>(DensityCategory::Count)> density_bytes{};
    if (density) {
        pfaf_density_bytes.fill(0);
    }
    u32 density_scalar_fp_ops = 0;
    PerfScope2 perf_prologue{GetPerfStats2().codegen_prologue};
    auto block_state = PrepareBlockState(block);
    const auto& loop_hoist = *block_state.loop_hoist;
    u32& loop_hoist_prefix_begin = block_state.loop_hoist_prefix_begin;
    u32& loop_hoist_prefix_ops = block_state.loop_hoist_prefix_ops;
    const bool split_flags_entry = block_state.split_flags_entry;
    if (split_flags_entry) {
        // Every published/external entry takes the cold initializer below;
        // only the self edge targets local_entry. This makes host NZCV valid
        // before a pre-producer guest fault without charging the steady loop.
        __ B(backedge_flags_plan->external_entry.get());
        PlacementPoint("flags", block->GetStartLocation().Value());
        __ Bind(backedge_flags_plan->local_entry.get());
        context.BeginBackedgeBody();
        backedge_host_begin = context.CurrentBufferSize();
    } else if (backedge_flags_plan) {
        backedge_host_begin = context.CurrentBufferSize();
    }
    EmitExecutionTrace(block->GetStartLocation().Value());
    const auto uniform_density = CollectUniformDensity(block, density);
    const u32 gpr_uniform_accesses =
            uniform_density.gpr_uniform_accesses;
    const u32 xmm_uniform_accesses =
            uniform_density.xmm_uniform_accesses;
    context.RecordExecCounter(exec_offset_gpr_uniform_accesses,
                              gpr_uniform_accesses);
    context.RecordExecCounter(exec_offset_xmm_uniform_accesses,
                              xmm_uniform_accesses);
    if (density) {
        RecordBoundaryRange(BoundarySubsequence::Prologue, density_start,
                            context.CurrentBufferSize());
        density_bytes[static_cast<size_t>(DensityCategory::Boundary)] +=
                context.CurrentBufferSize() - density_start;
    }
    perf_prologue.Stop();
    PerfScope2 perf_body{GetPerfStats2().codegen_body};
    TranslateBlockInstructions(block,
                               loop_hoist,
                               density,
                               gap_audit,
                               density_ops,
                               density_bytes,
                               density_scalar_fp_ops,
                               loop_hoist_prefix_begin,
                               loop_hoist_prefix_ops);
    perf_body.Stop();

    PerfScope2 perf_terminal{GetPerfStats2().codegen_terminal};
    EmitBlockTerminalAndColdPaths(block, density, density_bytes);
    PrintBlockDensity(block,
                      density,
                      density_ops,
                      density_bytes,
                      density_scalar_fp_ops,
                      loop_hoist,
                      loop_hoist_prefix_ops);
    if (!translating_function) {
        PlacementPoint("unit", block->GetStartLocation().Value());
    }
}

void JitTranslator::Translate(ir::HIRFunction* function) {
    vixl::svm_vixl_prof::JitScope vixl_prof{context.GetFeatures().vixl_fast};
    ASSERT(function);
    placement_unit_pc = function->GetFunction()->GetStartLocation().Value();
    context.SetCurrent(function->GetFunction());
    disable_instructions.resize(function->MaxInstrCount());
    PrepareRegionEdges(function);
    std::vector<ir::Block*> emitted_blocks;
    for (auto& hir_block : function->GetHIRBlocksRPO()) {
        auto* block = hir_block.GetBlock();
        if (block->GetInstList().empty() && !block->HasTerminal()) {
            continue;
        }
        emitted_blocks.push_back(block);
    }
    translating_function = true;
    for (size_t i = 0; i < emitted_blocks.size(); ++i) {
        // Undecoded successor left behind by lazy region compilation (and by
        // the pre-existing 128-block cap): no instructions and no terminal.
        // Emitting it would bind a label nobody branches to and then fall into
        // terminal::Invalid -> Ret without setting current_loc, which is a
        // dispatcher loop if it were ever entered.  Its guest address is
        // deliberately never published (TranslateIR skips empty blocks), so the
        // only way in is JitContext::Forward, which routes to it through the L2
        // dispatch slot instead.
        auto* block = emitted_blocks[i];
        next_region_block = i + 1 < emitted_blocks.size()
                ? std::optional<u64>{emitted_blocks[i + 1]
                                             ->GetStartLocation()
                                             .Value()}
                : std::nullopt;
        Translate(block);
    }
    translating_function = false;
    PlacementPoint("unit", placement_unit_pc);
    next_region_block.reset();
}


Label* JitTranslator::GetLocalLabel(ir::Inst* inst) {
    if (auto itr = local_labels.find(inst); itr != local_labels.end()) {
        return &itr->second;
    }
    return &local_labels.try_emplace(inst).first->second;
}

HostFlags JitTranslator::GuestNZCVToHost(ir::Flags guest) {
    HostFlags host{};
    if (True(guest & ir::Flags::Negate)) {
        host |= HostFlags::N;
    }
    if (True(guest & ir::Flags::Zero)) {
        host |= HostFlags::Z;
    }
    if (True(guest & ir::Flags::Carry)) {
        host |= HostFlags::C;
    }
    if (True(guest & ir::Flags::Overflow)) {
        host |= HostFlags::V;
    }
    return host;
}

Register JitTranslator::MaterializeOperand(const Operand& operand, ir::ValueType type) {
    auto tmp = context.GetTmpGPR(type);
    __ Mov(tmp, operand);
    return tmp;
}

void JitTranslator::Translate(ir::Inst* inst) {
    ASSERT(inst);
    context.TickIR(inst);
    if (inst->GetOp() != ir::OpCode::SetLocation) {
        static_next_loc.reset();
        dynamic_next_loc.reset();
    }

#define INST(name, ...)                                                                            \
    case ir::OpCode::name:                                                                         \
        Emit##name(inst);                                                                          \
        break;

    switch (inst->GetOp()) {
#include "runtime/ir/ir.inc"
        default:
            ASSERT_MSG(false, "Instr unk op: {}", inst->GetOp());
    }

#undef INST
    context.EndInstructionScratch();
}

bool JitTranslator::MatchMemoryOffsetCase(ir::Inst* inst) { return false; }
































Operand JitTranslator::EmitOperand(ir::Operand& ir_op) {
    if (ir_op.GetRight().Null()) {
        if (ir_op.GetLeft().IsImm()) {
            auto imm = ir_op.GetLeft().imm.Get();
            auto imm_signed = ir_op.GetLeft().imm.GetSigned();
            bool can_imm = __ IsImmAddSub(imm_signed);
            if (can_imm) {
                return Operand{imm_signed};
            } else {
                auto tmp = context.GetTmpX();
                __ Mov(tmp, imm);
                return Operand{tmp};
            }
        } else {
            return Operand{context.R(ir_op.GetLeft().value, true)};
        }
    } else {
        Register left_reg;
        ir::ValueType left_type{ir::ValueType::U64};
        if (ir_op.GetLeft().IsImm()) {
            // Materialize an immediate left side (constant-based composite
            // operand) into a scratch register first.
            auto tmp = context.GetTmpX();
            __ Mov(tmp, ir_op.GetLeft().imm.Get());
            left_reg = tmp;
        } else {
            auto left_value = ir_op.GetLeft().value;
            left_type = left_value.Type();
            left_reg = context.R(left_value, true);
        }
        auto right = ir_op.GetRight();
        if (right.IsImm()) {
            auto imm = right.imm.GetSigned();
            auto is_lsl = ir_op.GetOp() == ir::OperandOp::LSL;
            auto is_lsr = ir_op.GetOp() == ir::OperandOp::LSR;
            if (is_lsl || is_lsr) {
                if ((left_reg.Is64Bits() || (imm < kWRegSize)) || (left_reg.Is32Bits() || (imm < kXRegSize))) {
                    return Operand{left_reg, is_lsl ? LSL : LSR, static_cast<u8>(imm)};
                } else {
                    PANIC();
                }
            } else if (ir_op.GetOp() == ir::OperandOp::Plus) {
                auto tmp = context.GetTmpGPR(left_type);
                bool can_imm = __ IsImmAddSub(imm);
                if (can_imm) {
                    __ Add(tmp, left_reg, imm);
                } else {
                    __ Mov(tmp, imm);
                    __ Add(tmp, left_reg, tmp);
                }
                return Operand{tmp};
            } else {
                PANIC();
            }
        } else {
            auto right_reg = context.R(right.value, true);
            auto tmp = context.GetTmpGPR(left_type);
            if (ir_op.GetOp() == ir::OperandOp::Plus) {
                __ Add(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                __ Lsl(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                __ Lsr(tmp, left_reg, right_reg);
                return Operand{tmp};
            } else if (ir_op.GetOp() == ir::OperandOp::PlusExt) {
                auto shift_amount = ir_op.GetOp().shift_ext;
                ASSERT(right_reg.Is64Bits() || (shift_amount < kWRegSize));
                ASSERT(right_reg.Is32Bits() || (shift_amount < kXRegSize));
                __ Add(tmp, left_reg, Operand{right_reg, LSL, shift_amount});
                return Operand{tmp};
            } else {
                PANIC();
            }
        }
        return {};
    }
}





























































#undef masm

}  // namespace swift::runtime::backend::arm64
