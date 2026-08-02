#pragma once

#include "translator.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <numeric>
#include <string_view>
#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/context.h"
#include "runtime/backend/arm64/defines.h"
#include "runtime/common/backedge_control.h"
#include "translator/x86/cpu.h"

namespace swift::runtime::backend::arm64 {

namespace {

bool UniformPairAuditEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("SVM_UNIFORM_PAIR_AUDIT");
        return value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

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

struct UniformAuditAccess {
    ir::Inst* inst{};
    bool store{};
    u32 offset{};
    u32 size{};
    ir::ValueType type{};

    [[nodiscard]] ir::Value DataValue() const {
        return store ? inst->GetArg<ir::Value>(1) : ir::Value{inst};
    }
};

struct UniformAuditCandidate {
    UniformAuditAccess left{};
    UniformAuditAccess right{};
    const char* kind{};
    bool pair_encoding{};
};

std::vector<UniformAuditCandidate> BuildUniformAuditCandidates(ir::Block* block) {
    std::vector<UniformAuditCandidate> out;
    std::vector<UniformAuditAccess> run;
    auto flush_run = [&] {
        for (size_t i = 0; i + 1 < run.size(); i += 2) {
            const auto& left = run[i];
            const auto& right = run[i + 1];
            const s64 effective = static_cast<s64>(state_offset_uniform_buffer) +
                                  static_cast<s64>(left.offset);
            const unsigned access_size_log2 = left.size == 4 ? 2 :
                                              left.size == 8 ? 3 : 4;
            const bool encoding =
                    vixl::aarch64::Assembler::IsImmLSPair(effective,
                                                          access_size_log2);
            out.push_back({left, right,
                           left.store ? "stp_store" : "ldp_load", encoding});
        }
        run.clear();
    };

    UniformAuditAccess previous{};
    bool have_previous = false;
    for (auto& inst : block->GetInstList()) {
        const auto op = inst.GetOp();
        if (op != ir::OpCode::LoadUniform && op != ir::OpCode::StoreUniform) {
            flush_run();
            have_previous = false;
            continue;
        }
        const auto uniform = inst.GetArg<ir::Uniform>(0);
        UniformAuditAccess current{&inst,
                                   op == ir::OpCode::StoreUniform,
                                   uniform.GetOffset(),
                                   ir::GetValueSizeByte(uniform.GetType()),
                                   uniform.GetType()};

        if (have_previous && previous.offset == current.offset &&
            previous.size == current.size) {
            bool safe = true;
            if (!previous.store && current.store) {
                safe = current.inst->GetArg<ir::Value>(1).Def() == previous.inst;
            }
            if (safe) {
                const char* kind = !previous.store && !current.store ? "same_ll" :
                                   previous.store && current.store ? "same_ss" :
                                   previous.store ? "same_sl" : "same_ls";
                out.push_back({previous, current, kind, false});
            }
        }

        const bool pairable_size = current.size == 4 || current.size == 8 ||
                                   current.size == 16;
        const bool extend = !run.empty() && current.store == run.back().store &&
                            current.size == run.back().size && pairable_size &&
                            current.offset == run.back().offset + run.back().size;
        if (!extend) {
            flush_run();
        }
        if (pairable_size) {
            run.push_back(current);
        }
        previous = current;
        have_previous = true;
    }
    flush_run();
    return out;
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

LinkSuffixCommonPlan::LinkSuffixCommonPlan(
        std::vector<std::optional<u64>> encodings) {
    sites.resize(encodings.size());
    canonical_labels.resize(encodings.size());
    canonical_emitted.resize(encodings.size());

    std::map<u64, std::vector<size_t>> groups;
    for (size_t i = 0; i < encodings.size(); ++i) {
        sites[i].encoding = encodings[i];
        sites[i].canonical = i;
        if (encodings[i]) {
            groups[*encodings[i]].push_back(i);
        }
    }
    for (const auto& [encoding, members] : groups) {
        (void)encoding;
        if (members.size() < 2) {
            continue;
        }
        const size_t canonical = members.front();
        canonical_labels[canonical] = std::make_unique<Label>();
        ++stats.groups;
        stats.ranges += static_cast<u32>(members.size());
        stats.saved_bytes += static_cast<u32>(members.size() - 1) *
                             vixl::aarch64::kInstructionSize;
        for (const size_t site : members) {
            sites[site].canonical = canonical;
            sites[site].shared = true;
        }
    }
}

bool LinkSuffixCommonPlan::TryEmit(size_t site,
                                   u64 actual_encoding,
                                   MacroAssembler& masm) {
    if (site >= sites.size()) {
        return false;
    }
    const auto& planned = sites[site];
    if (!planned.shared || planned.encoding != actual_encoding) {
        return false;
    }
    const size_t canonical = planned.canonical;
    ASSERT(canonical < sites.size());
    ASSERT(canonical_labels[canonical]);
    if (site == canonical) {
        masm.Bind(canonical_labels[canonical].get());
        canonical_emitted[canonical] = true;
        return false;
    }
    if (!canonical_emitted[canonical]) {
        return false;
    }
    masm.B(canonical_labels[canonical].get());
    return true;
}


JitTranslator::JitTranslator(JitContext& ctx) : context(ctx), masm(ctx.GetMasm()) {
    auto& config = ctx.GetConfig();
    use_memory_base = config.memory_base != nullptr || config.page_table != nullptr;
    guest_addr_mask = config.guest_addr_mask;
    window_uxtw = guest_addr_mask == 0xFFFFFFFFull;
    mem_hostbase_fold = config.mem_hostbase_fold;
    induct_tie = config.induct_tie;
    sse_scalar_insert = config.sse_scalar_insert;
    sse_afp_nan = config.sse_afp_nan;
    fpcr_tax_skip_switch = sse_afp_nan && FpcrTaxSkipSwitchEnabled();
    fpcr_tax_timing = sse_afp_nan && FpcrTaxTimingEnabled();
    xmm_pool_ext = True(config.global_opts & Optimizations::XmmPoolExt);
    if (const char* shift_fast = PerfGetenv("SVM_SHIFT_IMM_FAST")) {
        shift_imm_fast = std::strcmp(shift_fast, "0") != 0;
    }
    if (const char* mem_fuse = PerfGetenv("SVM_MEM_NARROW_FUSE")) {
        mem_narrow_fuse = std::strcmp(mem_fuse, "0") != 0;
    }
    if (const char* nan_fast = PerfGetenv("SVM_SSE_NAN_FAST")) {
        sse_nan_fast = std::strcmp(nan_fast, "0") != 0;
    }
    if (const char* nan_coldpath = PerfGetenv("SVM_SSE_NAN_COLDPATH")) {
        sse_nan_coldpath = std::strcmp(nan_coldpath, "0") != 0;
    }
    backedge_latch = BackedgeLatchEnabled();
    backedge_flags = BackedgeFlagsEnabled();
    if (const char* common = PerfGetenv("SVM_LINK_SUFFIX_COMMON")) {
        link_suffix_common = std::strcmp(common, "0") != 0;
    }
}

std::optional<u64> JitTranslator::MatchInductionImmediate(ir::Inst* inst) {
    if (!induct_tie || !inst || inst->GetOp() != ir::OpCode::Add ||
        ir::GetValueSizeByte(inst->ReturnType()) != sizeof(u64)) {
        return std::nullopt;
    }
    const auto right = inst->GetArg<ir::Operand>(1);
    if (!right.GetRight().Null() || !right.GetLeft().IsValue()) {
        return std::nullopt;
    }
    const auto immediate = right.GetLeft().value;
    if (!immediate.Def() || immediate.Def()->GetOp() != ir::OpCode::LoadImm ||
        immediate.Def()->GetUses() == 0 ||
        ir::GetValueSizeByte(immediate.Type()) != sizeof(u64)) {
        return std::nullopt;
    }
    const u64 value = immediate.Def()->GetArg<ir::Imm>(0).Get();
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
    bool after_add = false;
    for (auto& next : cur_block->GetInstList()) {
        if (&next == inst) {
            after_add = true;
            continue;
        }
        if (!after_add) {
            continue;
        }
        if (next.GetOp() == ir::OpCode::SetHostGPR) {
            const auto published = ResolveBitCastValue(next.GetArg<ir::Value>(0));
            if (published.Def() == inst && next.GetArg<ir::Imm>(1).Get() == source_host &&
                next.GetArg<ir::Imm>(2).Get() == 0) {
                return value;
            }
            return std::nullopt;
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
    return std::nullopt;
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
        const auto right = inst.GetArg<ir::Operand>(1).GetLeft().value;
        ++matched_uses[right.Def()];
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
    link_suffix_plan.reset();
    link_suffix_site = 0;
}

void JitTranslator::PlanLinkSuffixes(const ir::Terminal& terminal) {
    link_suffix_plan.reset();
    link_suffix_site = 0;
    if (!link_suffix_common || backedge_flags_plan) {
        return;
    }
    const bool direct_single_exit = VisitVariant<bool>(terminal, [this](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                      std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            return context.CanEmitDirectLinkV2(term.next);
        }
        return false;
    });
    if (direct_single_exit) {
        // P1's one-instruction leaf has no str+ret suffix to common. Nested
        // conditional exits deliberately remain on the P2/legacy planner.
        return;
    }

    std::vector<std::optional<u64>> encodings;
    std::function<void(const ir::Terminal&)> visit = [&](const ir::Terminal& item) {
        VisitVariant<void>(item, [&](const auto& term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                          std::is_same_v<T, ir::terminal::LinkBlockFast>) {
                encodings.push_back(context.PlanForwardSuffix(term.next));
            } else if constexpr (std::is_same_v<T, ir::terminal::If> ||
                                 std::is_same_v<T, ir::terminal::Condition>) {
                visit(term.then_);
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                for (const auto& case_ : term.cases) {
                    visit(case_.then);
                }
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
                visit(term.else_);
            }
        });
    };
    visit(terminal);
    link_suffix_plan =
            std::make_unique<LinkSuffixCommonPlan>(std::move(encodings));
}

JitContext::LinkSuffixEmitter JitTranslator::NextLinkSuffixEmitter() {
    if (!link_suffix_plan) {
        return {};
    }
    ASSERT(link_suffix_site < link_suffix_plan->SiteCount());
    const size_t site = link_suffix_site++;
    return [this, site](u64 actual_encoding) {
        return link_suffix_plan->TryEmit(site, actual_encoding, masm);
    };
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
    // Once enabled, duplicate sites no longer contain the original suffix, so
    // the post-emission scan cannot see the opportunity it just consumed. The
    // plan uses the same full 8-byte key and reports the pre-emission counts.
    if (link_suffix_common && link_suffix_plan) {
        const auto& stats = link_suffix_plan->GetStats();
        common2_groups = stats.groups;
        common2_ranges = stats.ranges;
        common2_saved = stats.saved_bytes;
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



void JitTranslator::Translate(ir::Block* block) {
    vixl::svm_vixl_prof::JitScope vixl_prof;
    ASSERT(vec_nan_cold_sites.empty());
    const bool density = context.DensityProfileEnabled();
    ResetBoundaryDensity();
    const bool uniform_pair_audit = UniformPairAuditEnabled();
    const auto uniform_candidates = uniform_pair_audit
            ? BuildUniformAuditCandidates(block)
            : std::vector<UniformAuditCandidate>{};
    if (uniform_pair_audit) {
        const auto legacy = HotCoalesceAnalyzeUniformSequences(block);
        if (legacy.saved_instructions != uniform_candidates.size()) {
            std::fprintf(stderr,
                         "[svm-uniform-mismatch]\tpc=0x%llx\tlegacy=%u\taudit=%zu\t"
                         "load_pairs=%u\tstore_pairs=%u\tsame_offset=%u\tblock=%s\n",
                         static_cast<unsigned long long>(
                                 block->GetStartLocation().Value()),
                         legacy.saved_instructions, uniform_candidates.size(),
                         legacy.load_pairs, legacy.store_pairs,
                         legacy.same_offset, block->ToString().c_str());
        }
    }
    std::vector<std::pair<u32, u32>> uniform_host_ranges;
    if (uniform_pair_audit) {
        u32 range_count = 0;
        for (const auto& inst : block->GetInstList()) {
            range_count = std::max(range_count, static_cast<u32>(inst.Id()) + 1);
        }
        uniform_host_ranges.resize(range_count);
    }
    const u32 density_start = density ? context.CurrentBufferSize() : 0;
    std::array<u32, static_cast<size_t>(DensityCategory::Count)> density_ops{};
    std::array<u32, static_cast<size_t>(DensityCategory::Count)> density_bytes{};
    u32 density_scalar_fp_ops = 0;
    PerfScope2 perf_prologue{GetPerfStats2().codegen_prologue};
    cur_block = block;
    cur_block_is_call = false;
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() == ir::OpCode::PushRSB) {
            cur_block_is_call = true;
            break;
        }
    }
    static_next_loc.reset();
    // Function mode keeps one function-sized suppression bitmap. Grow it
    // before the per-block backedge proof marks the two sunk IR instructions.
    disable_instructions.resize(
            std::max<size_t>(disable_instructions.size(), block->MaxInstrId()));
    backedge_flags_plan = PlanBackedgeFlags(block);
    if (backedge_flags_plan) {
        disable_instructions.set(backedge_flags_plan->polarity_load->Id());
        disable_instructions.set(backedge_flags_plan->polarity_store->Id());
    }
    backedge_exit_referenced = false;
    backedge_exit_label =
            backedge_latch && HasSelfEdge(block->GetTerminal())
                    ? std::make_unique<Label>()
                    : nullptr;
    context.SetCurrent(block, backedge_flags_plan != nullptr);
    PlanInductionTies(block);
    if (backedge_flags_plan) {
        // Every published/external entry takes the cold initializer below;
        // only the self edge targets local_entry. This makes host NZCV valid
        // before a pre-producer guest fault without charging the steady loop.
        __ B(backedge_flags_plan->external_entry.get());
        __ Bind(backedge_flags_plan->local_entry.get());
        context.BeginBackedgeBody();
        backedge_host_begin = context.CurrentBufferSize();
    }
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
        const u32 uniform_before = uniform_pair_audit ? context.CurrentBufferSize() : 0;
        const u32 nan_before = density ? context.DensityNaNBytes() : 0;
        Translate(&inst);
        if (uniform_pair_audit &&
            (inst.GetOp() == ir::OpCode::LoadUniform ||
             inst.GetOp() == ir::OpCode::StoreUniform)) {
            uniform_host_ranges[inst.Id()] =
                    {uniform_before, context.CurrentBufferSize()};
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
        }
    }
    perf_body.Stop();

    if (uniform_pair_audit) {
        for (const auto& candidate : uniform_candidates) {
            const auto left_value = candidate.left.DataValue();
            const auto right_value = candidate.right.DataValue();
            const bool copy_shape = std::strcmp(candidate.kind, "same_ll") == 0 ||
                                    std::strcmp(candidate.kind, "same_sl") == 0;
            const bool same_physical =
                    copy_shape && context.SharesPhysical(left_value, right_value);
            const auto [left_begin, left_end] =
                    uniform_host_ranges[candidate.left.inst->Id()];
            const auto [right_begin, right_end] =
                    uniform_host_ranges[candidate.right.inst->Id()];
            const auto left_ir = fmt::format("{}", *candidate.left.inst);
            const auto right_ir = fmt::format("{}", *candidate.right.inst);
            const auto left_host = context.DisassembleRange(left_begin, left_end);
            const auto right_host = context.DisassembleRange(right_begin, right_end);
            std::fprintf(stderr,
                         "[svm-uniform-pair]\tpc=0x%llx\tkind=%s\t"
                         "pair_encoding=%u\tcopy_shape=%u\tsame_physical=%u\t"
                         "id0=%u\tid1=%u\toff0=%u\toff1=%u\tsize0=%u\tsize1=%u\t"
                         "type0=%s\ttype1=%s\talloc0=%s\talloc1=%s\t"
                         "host_begin0=%u\thost_end0=%u\thost_begin1=%u\thost_end1=%u\t"
                         "ir0=%s\tir1=%s\thost0=%s\thost1=%s\n",
                         static_cast<unsigned long long>(
                                 block->GetStartLocation().Value()),
                         candidate.kind, candidate.pair_encoding,
                         copy_shape, same_physical,
                         candidate.left.inst->Id(), candidate.right.inst->Id(),
                         candidate.left.offset, candidate.right.offset,
                         candidate.left.size, candidate.right.size,
                         ir::ValueTypeString(candidate.left.type),
                         ir::ValueTypeString(candidate.right.type),
                         context.AllocationName(left_value).c_str(),
                         context.AllocationName(right_value).c_str(),
                         left_begin, left_end, right_begin, right_end,
                         left_ir.c_str(), right_ir.c_str(),
                         left_host.c_str(), right_host.c_str());
        }
    }

    PerfScope2 perf_terminal{GetPerfStats2().codegen_terminal};
    context.BeginTerminalScratch();
    const u32 flags_before = density ? context.CurrentBufferSize() : 0;
    FlushFlags();
    if (density) {
        density_bytes[static_cast<size_t>(DensityCategory::Flags)] +=
                context.CurrentBufferSize() - flags_before;
    }
    const u32 terminal_before = density ? context.CurrentBufferSize() : 0;
    boundary_terminal_open = density;
    PlanLinkSuffixes(block->GetTerminal());
    if (!EmitBackedgeFlagsTerminal(block->GetTerminal())) {
        EmitTerminal(block->GetTerminal(), true);
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
    if (density) {
        const u32 total_ops = std::accumulate(density_ops.begin(), density_ops.end(), 0u);
        const u32 total_bytes =
                std::accumulate(density_bytes.begin(), density_bytes.end(), 0u);
        std::fprintf(stderr,
                     "[svm-density] pc=0x%llx ops_flags=%u ops_uniform=%u "
                     "ops_move=%u ops_nan=%u ops_boundary=%u ops_work=%u "
                     "bytes_flags=%u bytes_uniform=%u bytes_move=%u bytes_nan=%u "
                     "bytes_boundary=%u bytes_work=%u ops_total=%u bytes_total=%u "
                     "ops_fp_scalar=%u\n",
                     static_cast<unsigned long long>(block->GetStartLocation().Value()),
                     density_ops[0], density_ops[1], density_ops[2], density_ops[3],
                     density_ops[4], density_ops[5], density_bytes[0], density_bytes[1],
                     density_bytes[2], density_bytes[3], density_bytes[4], density_bytes[5],
                     total_ops, total_bytes, density_scalar_fp_ops);
        PrintBoundaryDensity(block->GetStartLocation().Value(), density_bytes[4]);
    }
}

void JitTranslator::Translate(ir::HIRFunction* function) {
    vixl::svm_vixl_prof::JitScope vixl_prof;
    ASSERT(function);
    context.SetCurrent(function->GetFunction());
    disable_instructions.resize(function->MaxInstrCount());
    for (auto& hir_block : function->GetHIRBlocksRPO()) {
        // Undecoded successor left behind by lazy region compilation (and by
        // the pre-existing 128-block cap): no instructions and no terminal.
        // Emitting it would bind a label nobody branches to and then fall into
        // terminal::Invalid -> Ret without setting current_loc, which is a
        // dispatcher loop if it were ever entered.  Its guest address is
        // deliberately never published (TranslateIR skips empty blocks), so the
        // only way in is JitContext::Forward, which routes to it through the L2
        // dispatch slot instead.
        auto* block = hir_block.GetBlock();
        if (block->GetInstList().empty() && !block->HasTerminal()) {
            continue;
        }
        Translate(block);
    }
}

bool JitTranslator::PreservesHostNZCV(ir::OpCode op) {
    // Deliberately narrow emitter audit. These are the only pre-producer
    // operations admitted by the first spike; every listed ARM64 lowering is
    // flag-neutral (including its address arithmetic and CBNZ/TBZ guards).
    switch (op) {
        case ir::OpCode::LoadUniform:
        case ir::OpCode::StoreUniform:
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::GetHostGPR:
        case ir::OpCode::GetHostFPR:
        case ir::OpCode::SetHostGPR:
        case ir::OpCode::SetHostFPR:
        case ir::OpCode::LoadImm:
        case ir::OpCode::AdvancePC:
        case ir::OpCode::BitCast:
        case ir::OpCode::GetOperand:
        case ir::OpCode::Zero:
        case ir::OpCode::ZeroExtend32:
        case ir::OpCode::ZeroExtend32To64:
        case ir::OpCode::VecFAdd:
        case ir::OpCode::VecFSub:
        case ir::OpCode::VecFMul:
        // These lower to their non-S forms when no surviving SaveFlags pseudo
        // names them. The proof stops at the first producer that does have
        // such a pseudo, so earlier dead-flags pointer arithmetic is neutral.
        case ir::OpCode::Add:
        case ir::OpCode::Sub:
            return true;
        default:
            return false;
    }
}

bool JitTranslator::MayFaultOrObserve(ir::OpCode op) {
    switch (op) {
        case ir::OpCode::LoadMemory:
        case ir::OpCode::StoreMemory:
        case ir::OpCode::LoadMemoryTSO:
        case ir::OpCode::StoreMemoryTSO:
        case ir::OpCode::MemoryCopy:
        case ir::OpCode::MemoryCopyTSO:
        case ir::OpCode::CompareAndSwap:
        case ir::OpCode::CompareAndSwap128:
        case ir::OpCode::CheckMemoryAlignment:
        case ir::OpCode::AtomicExchange:
        case ir::OpCode::AtomicFetchAdd:
        case ir::OpCode::AtomicRMW:
        case ir::OpCode::CallLambda:
        case ir::OpCode::CallLocation:
        case ir::OpCode::CallDynamic:
        case ir::OpCode::X87Op:
        case ir::OpCode::Sse42Str:
            return true;
        default:
            return false;
    }
}

std::unique_ptr<JitTranslator::BackedgeFlagsPlan>
JitTranslator::PlanBackedgeFlags(ir::Block* block) {
    if (!backedge_flags || !block) {
        return nullptr;
    }

    std::optional<ir::Location> then_target;
    std::optional<ir::Location> else_target;
    ir::Value condition{};
    VisitVariant<void>(block->GetTerminal(), [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            condition = term.cond;
            auto direct = [](const ir::Terminal& edge) -> std::optional<ir::Location> {
                return VisitVariant<std::optional<ir::Location>>(
                        edge, [](const auto& target) -> std::optional<ir::Location> {
                            using E = std::decay_t<decltype(target)>;
                            if constexpr (std::is_same_v<E, ir::terminal::LinkBlock> ||
                                          std::is_same_v<E, ir::terminal::LinkBlockFast>) {
                                return target.next;
                            }
                            return std::nullopt;
                        });
            };
            then_target = direct(term.then_);
            else_target = direct(term.else_);
        }
    });
    if (!then_target || !else_target || !condition.Def() ||
        condition.Def()->GetOp() != ir::OpCode::LocalCondSet) {
        return nullptr;
    }
    const auto self = block->GetStartLocation();
    const bool then_self = *then_target == self;
    const bool else_self = *else_target == self;
    if (then_self == else_self) {
        return nullptr;
    }

    ir::Inst* first_producer = nullptr;
    ir::Inst* final_save = nullptr;
    ir::Flags final_requested{};
    for (auto& inst : block->GetInstList()) {
        if (inst.GetOp() != ir::OpCode::SaveFlags) {
            continue;
        }
        auto* producer = inst.GetArg<ir::Value>(0).Def();
        if (!producer) {
            return nullptr;
        }
        if (!first_producer || producer->Id() < first_producer->Id()) {
            first_producer = producer;
        }
        const auto requested = inst.GetArg<ir::Flags>(1);
        if (True(requested & ir::Flags::Parity) &&
            True(requested & ir::Flags::AuxiliaryCarry) &&
            True(requested & ir::Flags::NZCV)) {
            final_save = &inst;
            final_requested = requested;
        }
    }
    if (!first_producer || !final_save) {
        if (PerfGetenv("SVM_DUMP_IR")) {
            fmt::print(stderr, "[backedge-proof] {:#x} reject flags producer\n",
                       block->GetStartLocation().Value());
        }
        return nullptr;
    }

    ir::Inst* polarity_store = nullptr;
    ir::Inst* polarity_load = nullptr;
    u8 polarity = 0;
    constexpr u32 kCarryOffset = offsetof(swift::x86::ThreadContext64,
                                          carry_inverted);
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= final_save->Id() ||
            inst.GetOp() != ir::OpCode::StoreUniform) {
            continue;
        }
        const auto uniform = inst.GetArg<ir::Uniform>(0);
        if (uniform.GetOffset() != kCarryOffset ||
            uniform.GetType() != ir::ValueType::U8) {
            continue;
        }
        auto value = inst.GetArg<ir::Value>(1);
        auto* def = value.Def();
        if (!def || def->GetOp() != ir::OpCode::LoadImm ||
            value.Type() != ir::ValueType::U8 || def->GetUses() != 1) {
            if (PerfGetenv("SVM_DUMP_IR")) {
                fmt::print(stderr,
                           "[backedge-proof] {:#x} reject polarity value def={} op={} type={} uses={}\n",
                           block->GetStartLocation().Value(),
                           def != nullptr,
                           def ? static_cast<u32>(def->GetOp()) : UINT32_MAX,
                           static_cast<u32>(value.Type()),
                           def ? def->GetUses() : UINT32_MAX);
            }
            return nullptr;
        }
        const u64 immediate = def->GetArg<ir::Imm>(0).Get();
        if (immediate > 1) {
            return nullptr;
        }
        polarity_store = &inst;
        polarity_load = def;
        polarity = static_cast<u8>(immediate);
    }
    if (!polarity_store || !polarity_load ||
        polarity_store->Id() >= condition.Def()->Id()) {
        if (PerfGetenv("SVM_DUMP_IR")) {
            fmt::print(stderr,
                       "[backedge-proof] {:#x} reject polarity store={} load={} cond={}\n",
                       block->GetStartLocation().Value(),
                       polarity_store ? polarity_store->Id() : UINT32_MAX,
                       polarity_load ? polarity_load->Id() : UINT32_MAX,
                       condition.Def()->Id());
        }
        return nullptr;
    }

    // The old block-entry flags stay live until the first producer. A fault is
    // allowed in that prefix; the per-block veneer below reconstructs them.
    for (auto& inst : block->GetInstList()) {
        if (&inst == first_producer) {
            break;
        }
        if (!PreservesHostNZCV(inst.GetOp())) {
            if (PerfGetenv("SVM_DUMP_IR")) {
                fmt::print(stderr,
                           "[backedge-proof] {:#x} reject pre-producer op={} id={}\n",
                           block->GetStartLocation().Value(),
                           static_cast<u32>(inst.GetOp()), inst.Id());
            }
            return nullptr;
        }
    }
    // Once the next producer overwrites host NZCV there may be no synchronous
    // fault or architectural observer before the terminal safepoint.
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() > first_producer->Id() && MayFaultOrObserve(inst.GetOp())) {
            if (PerfGetenv("SVM_DUMP_IR")) {
                fmt::print(stderr,
                           "[backedge-proof] {:#x} reject post-producer fault op={} id={} producer={}\n",
                           block->GetStartLocation().Value(),
                           static_cast<u32>(inst.GetOp()), inst.Id(), first_producer->Id());
            }
            return nullptr;
        }
    }
    // The tail after the omitted store is intentionally tiny: advancing the
    // guest PC and consuming the already-live local condition only.
    ir::Inst* final_advance = nullptr;
    for (auto& inst : block->GetInstList()) {
        if (inst.Id() <= polarity_store->Id()) {
            continue;
        }
        if (inst.GetOp() != ir::OpCode::AdvancePC &&
            inst.GetOp() != ir::OpCode::LocalCondSet &&
            inst.GetOp() != ir::OpCode::ZeroExtend32 &&
            inst.GetOp() != ir::OpCode::ZeroExtend32To64 &&
            inst.GetOp() != ir::OpCode::SetHostGPR) {
            if (PerfGetenv("SVM_DUMP_IR")) {
                fmt::print(stderr,
                           "[backedge-proof] {:#x} reject tail op={} id={}\n",
                           block->GetStartLocation().Value(),
                           static_cast<u32>(inst.GetOp()), inst.Id());
            }
            return nullptr;
        }
        if (inst.GetOp() == ir::OpCode::AdvancePC) {
            if (final_advance) {
                return nullptr;
            }
            final_advance = &inst;
        }
    }
    if (!final_advance || final_advance->Id() >= condition.Def()->Id()) {
        return nullptr;
    }

    auto plan = std::make_unique<BackedgeFlagsPlan>();
    plan->self_is_then = then_self;
    plan->self_target = self;
    plan->cold_target = then_self ? *else_target : *then_target;
    plan->carry_inverted = polarity;
    plan->requested = GuestNZCVToHost(final_requested & ir::Flags::NZCV);
    plan->polarity_load = polarity_load;
    plan->polarity_store = polarity_store;
    plan->final_advance = final_advance;
    if (PerfGetenv("SVM_DUMP_IR")) {
        fmt::print(stderr, "[backedge-proof] {:#x} eligible\n",
                   block->GetStartLocation().Value());
    }
    return plan;
}

void JitTranslator::EmitBackedgeMaterialize(const BackedgeFlagsPlan& plan) {
    __ Mov(ipw1, plan.carry_inverted);
    __ Strb(ipw1,
            MemOperand(state,
                       state_offset_uniform_buffer +
                               offsetof(swift::x86::ThreadContext64,
                                        carry_inverted)));
    const u64 requested = static_cast<u64>(plan.requested);
    u64 keep = ~requested;
    __ Mrs(ip0, NZCV);
    __ And(flags, flags, ForceCast<s64>(keep));
    __ And(ip0, ip0, static_cast<u32>(requested));
    __ Orr(flags, flags, ip0);
}

bool JitTranslator::EmitBackedgeFlagsTerminal(const ir::Terminal& terminal) {
    if (!backedge_flags_plan) {
        return false;
    }
    auto& plan = *backedge_flags_plan;
    if (!save_in_nzcv || !nzcv_dirty || nzcv_requested != plan.requested) {
        if (PerfGetenv("SVM_DUMP_IR")) {
            fmt::print(stderr,
                       "[backedge-proof] {:#x} emitter fallback save={} dirty={} actual={:#x} expected={:#x}\n",
                       cur_block->GetStartLocation().Value(),
                       save_in_nzcv,
                       nzcv_dirty,
                       static_cast<u64>(nzcv_requested),
                       static_cast<u64>(plan.requested));
        }
        // Static proof and emitter state disagreed. Recreate the omitted
        // polarity write, commit through the ordinary path, and let the
        // generic terminal keep this block correct (but unoptimized).
        __ Mov(ipw1, plan.carry_inverted);
        __ Strb(ipw1,
                MemOperand(state,
                           state_offset_uniform_buffer +
                                   offsetof(swift::x86::ThreadContext64,
                                            carry_inverted)));
        MergeNZCV();
        plan.optimized = false;
        return false;
    }

    ir::Value condition{};
    VisitVariant<void>(terminal, [&](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::If>) {
            condition = term.cond;
        }
    });
    if (!condition.Def()) {
        return false;
    }
    if (auto local = LocalConditionFor(condition)) {
        const auto branch_to_cold = plan.self_is_then
                ? static_cast<Condition>(static_cast<u8>(*local) ^ 1)
                : *local;
        __ B(backedge_flags_plan->cold_exit.get(), branch_to_cold);
    } else if (plan.self_is_then) {
        __ Cbz(context.W(condition), backedge_flags_plan->cold_exit.get());
    } else {
        __ Cbnz(context.W(condition), backedge_flags_plan->cold_exit.get());
    }
    plan.cold_referenced = true;
    context.RecordExecCounter(exec_offset_exit_direct);
    backedge_exit_referenced = true;
    const u32 link_before = context.CurrentBufferSize();
    context.Forward(plan.self_target,
                    backedge_exit_label.get(),
                    plan.local_entry.get());
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
    return true;
}

void JitTranslator::EmitBackedgeColdPaths() {
    if (!backedge_flags_plan) {
        return;
    }
    auto& plan = *backedge_flags_plan;

    __ Bind(plan.external_entry.get());
    // All non-self entries begin with committed x26/State. Dispatcher lookup
    // clobbers NZCV. Normalize the committed carry representation to this
    // block's compile-time polarity before reconstructing host NZCV: a fault
    // before the first producer must see the same local ABI on an external
    // first iteration as it does after a self edge.
    Label polarity_ready;
    __ Ldrb(ipw0,
            MemOperand(state,
                       state_offset_uniform_buffer +
                               offsetof(swift::x86::ThreadContext64,
                                        carry_inverted)));
    __ Cmp(ipw0, plan.carry_inverted);
    __ B(&polarity_ready, eq);
    __ Eor(flags, flags, static_cast<u64>(HostFlags::C));
    __ Bind(&polarity_ready);
    __ Mov(ipw0, plan.carry_inverted);
    __ Strb(ipw0,
            MemOperand(state,
                       state_offset_uniform_buffer +
                               offsetof(swift::x86::ThreadContext64,
                                        carry_inverted)));
    __ And(ip0, flags, static_cast<u64>(HostFlags::NZCV));
    __ Msr(NZCV, ip0);
    __ B(plan.local_entry.get());

    if (plan.optimized && plan.cold_referenced) {
        __ Bind(plan.cold_exit.get());
        EmitBackedgeMaterialize(plan);
        context.RecordExecCounter(exec_offset_exit_direct);
        context.Forward(plan.cold_target);
    }

    u32 recovery_offset = 0;
    if (plan.optimized) {
        recovery_offset = context.CurrentBufferSize();
        __ Bind(plan.fault_recovery.get());
        EmitBackedgeMaterialize(plan);
        __ Ret();
    }
    backedge_block_metadata.push_back({cur_block->GetStartLocation().Value(),
                                       backedge_host_begin,
                                       backedge_host_end,
                                       recovery_offset});
    // The next emitted block always starts from the committed ABI. The local
    // state represented by this object has been materialized on every edge
    // that can reach it.
    nzcv_dirty = false;
    nzcv_requested = {};
    backedge_flags_plan.reset();
}

bool JitTranslator::IsSelfEdge(ir::Location target) const {
    return cur_block && target == cur_block->GetStartLocation();
}

bool JitTranslator::HasSelfEdge(const ir::Terminal& terminal) const {
    return VisitVariant<bool>(terminal, [this](const auto& term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::LinkBlock> ||
                      std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            return IsSelfEdge(term.next);
        } else if constexpr (std::is_same_v<T, ir::terminal::If> ||
                             std::is_same_v<T, ir::terminal::Condition>) {
            return HasSelfEdge(term.then_) || HasSelfEdge(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            return HasSelfEdge(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            return std::any_of(term.cases.begin(), term.cases.end(),
                               [this](const auto& item) {
                                   return HasSelfEdge(item.then);
                               });
        } else {
            return false;
        }
    });
}

void JitTranslator::EmitBackedgeExitStub() {
    if (!backedge_exit_label || !backedge_exit_referenced) {
        backedge_exit_label.reset();
        return;
    }
    Label signal;
    Label publish;
    __ Bind(backedge_exit_label.get());
    if (backedge_flags_plan && backedge_flags_plan->optimized) {
        EmitBackedgeMaterialize(*backedge_flags_plan);
    }
    __ Mov(ip1, cur_block->GetStartLocation().Value());
    __ Str(ip1, MemOperand(state, state_offset_current_loc));
    __ Tbnz(ip0, 63, &signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::CodeMiss));
    __ B(&publish);
    __ Bind(&signal);
    __ Mov(ipw1, static_cast<u32>(HaltReason::Signal));
    __ Bind(&publish);
    __ Str(ipw1, MemOperand(state, state_offset_halt_reason));
    __ Ret();
    backedge_exit_label.reset();
    backedge_exit_referenced = false;
}

void JitTranslator::EmitTerminal(const ir::Terminal& terminal,
                                 bool allow_direct_link_v2) {
    VisitVariant<void>(terminal, [this, allow_direct_link_v2](auto term) {
        using T = std::decay_t<decltype(term)>;
        if constexpr (std::is_same_v<T, ir::terminal::Invalid>) {
            // Flat decoded blocks have no explicit terminal: the next location was
            // already written to state->current_loc by a SetLocation instruction.
            MergeNZCV();
            context.RecordExecCounter(static_next_loc ? exec_offset_exit_direct
                                                      : exec_offset_exit_indirect);
            if (!EmitStaticForward()) {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToDispatch>) {
            MergeNZCV();
            context.RecordExecCounter(
                    cur_block_is_call ? exec_offset_exit_call
                                      : (static_next_loc ? exec_offset_exit_direct
                                                         : exec_offset_exit_indirect));
            if (!EmitStaticForward()) {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::ReturnToHost>) {
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_syscall);
            __ Mov(ipw, static_cast<u32>(HaltReason::CallHost));
            __ Str(ipw, MemOperand(state, state_offset_halt_reason));
            __ Ret();
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlock>) {
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_direct);
            auto* exit = IsSelfEdge(term.next) && backedge_exit_label
                    ? backedge_exit_label.get()
                    : nullptr;
            backedge_exit_referenced |= exit != nullptr;
            auto* self_target = IsSelfEdge(term.next) && backedge_flags_plan
                    ? backedge_flags_plan->local_entry.get()
                    : nullptr;
            const u32 link_before = context.CurrentBufferSize();
            context.Forward(term.next, exit, self_target,
                            NextLinkSuffixEmitter(), allow_direct_link_v2);
            RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                                context.CurrentBufferSize());
        } else if constexpr (std::is_same_v<T, ir::terminal::LinkBlockFast>) {
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_direct);
            auto* exit = IsSelfEdge(term.next) && backedge_exit_label
                    ? backedge_exit_label.get()
                    : nullptr;
            backedge_exit_referenced |= exit != nullptr;
            auto* self_target = IsSelfEdge(term.next) && backedge_flags_plan
                    ? backedge_flags_plan->local_entry.get()
                    : nullptr;
            const u32 link_before = context.CurrentBufferSize();
            context.Forward(term.next, exit, self_target,
                            NextLinkSuffixEmitter(), allow_direct_link_v2);
            RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                                context.CurrentBufferSize());
        } else if constexpr (std::is_same_v<T, ir::terminal::PopRSBHint>) {
            // Return Stack Buffer: this is the real pop+predict site. It must
            // run here (not at the PopRSB instruction) because guest flags have
            // just been committed by FlushFlags/MergeNZCV — the hit path
            // branches directly to the return target, which expects the flags
            // register to be current. EmitRSBPop ends in Br (hit) or Ret (miss/
            // underflow), so it fully terminates the block.
            MergeNZCV();
            context.RecordExecCounter(exec_offset_exit_ret);
            if (True(context.GetConfig().global_opts & Optimizations::ReturnStackBuffer)) {
                const u32 link_before = context.CurrentBufferSize();
                context.EmitRSBPop();
                RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                                    context.CurrentBufferSize());
            } else {
                __ Ret();
            }
        } else if constexpr (std::is_same_v<T, ir::terminal::If>) {
            Label else_label;
            if (auto local = LocalConditionFor(term.cond)) {
                __ B(&else_label,
                     static_cast<Condition>(static_cast<u8>(*local) ^ 1));
            } else {
                __ Cbz(context.W(term.cond), &else_label);
            }
            EmitTerminal(term.then_);
            __ Bind(&else_label);
            EmitTerminal(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
            Label else_label;
            auto host_cond = MapCond(term.cond);
            if (!(save_in_nzcv && nzcv_dirty)) {
                LoadNZCVFromFlags();
            }
            __ B(&else_label, static_cast<Condition>(static_cast<u8>(host_cond) ^ 1));
            EmitTerminal(term.then_);
            __ Bind(&else_label);
            EmitTerminal(term.else_);
        } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
            // Linear compare chain; each arm ends with its own terminal.
            MergeNZCV();
            auto value = context.R(term.value);
            for (auto& case_ : term.cases) {
                Label next_case;
                __ Mov(ip, case_.case_value.Get());
                __ Cmp(value, ip);
                __ B(&next_case, ne);
                EmitTerminal(case_.then);
                __ Bind(&next_case);
            }
            // No case matched: bail out to the dispatcher.
            context.RecordExecCounter(exec_offset_exit_indirect);
            __ Ret();
        } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
            Label no_halt;
            __ Ldr(ipw, MemOperand(state, state_offset_halt_reason));
            __ Cbz(ipw, &no_halt);
            MergeNZCV();
            __ Ret();
            __ Bind(&no_halt);
            EmitTerminal(term.else_);
        } else {
            PANIC("Unknown terminal!");
        }
    });
}

std::optional<Condition> JitTranslator::LocalConditionFor(ir::Value value) const {
    if (!value.Def()) {
        return std::nullopt;
    }
    if (auto it = local_conditions.find(value.Def()); it != local_conditions.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool JitTranslator::IsCompactFCmp(ir::Value value) {
    return value.Def() && value.Def()->GetOp() == ir::OpCode::VecFCmp &&
           value.Def()->GetArg<ir::Imm>(3).Get() != 0;
}

bool JitTranslator::RecordLocalCondition(ir::Inst* inst, ir::Cond cond) {
    if (inst->GetUses() != 1) {
        return false;
    }
    auto& list = cur_block->GetInstList();
    for (auto it = std::next(list.iterator_to(*inst)); it != list.end(); ++it) {
        bool names = false;
        for (auto value : it->GetValues()) {
            names = names || value.Def() == inst;
        }
        if (!names) {
            continue;
        }
        const bool supported =
                (it->GetOp() == ir::OpCode::Goto ||
                 it->GetOp() == ir::OpCode::NotGoto) &&
                        it->GetArg<ir::Value>(0).Def() == inst ||
                it->GetOp() == ir::OpCode::Select &&
                        it->GetArg<ir::Value>(0).Def() == inst;
        if (!supported) {
            return false;
        }
        local_conditions.emplace(inst, MapCond(cond));
        return true;
    }

    bool terminal_use = false;
    std::function<void(const ir::Terminal&)> visit = [&](const ir::Terminal& terminal) {
        VisitVariant<void>(terminal, [&](auto term) {
            using T = std::decay_t<decltype(term)>;
            if constexpr (std::is_same_v<T, ir::terminal::If>) {
                if (term.cond.Def() == inst) {
                    terminal_use = true;
                }
                visit(term.then_);
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Condition>) {
                visit(term.then_);
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::CheckHalt>) {
                visit(term.else_);
            } else if constexpr (std::is_same_v<T, ir::terminal::Switch>) {
                for (const auto& arm : term.cases) {
                    visit(arm.then);
                }
            }
        });
    };
    visit(cur_block->GetTerminal());
    if (terminal_use) {
        local_conditions.emplace(inst, MapCond(cond));
    }
    return terminal_use;
}

// A direct jmp/call decodes to SetLocation(imm) + ReturnToDispatcher, and the
// trampoline then re-reads state->current_loc and walks the L1 hash chain for
// a target that was already known when the code was emitted. The dispatch
// table indexed here is the same one the RSB pop and JitContext::Forward's
// BlockLink path already branch through, with the same safety property: SMC
// invalidation (SmcTracker::ClearDispatchSlots) zeroes the slot, so a stale
// translation degrades to the Cbz fallback rather than to a wild branch.
bool JitTranslator::EmitStaticForward() {
    if (!static_next_loc) {
        return false;
    }
    const u64 target = *static_next_loc;
    static_next_loc.reset();
    const u32 link_before = context.CurrentBufferSize();
    const bool emitted = context.ForwardStatic(ir::Location{target});
    RecordBoundaryRange(BoundarySubsequence::LinkTail, link_before,
                        context.CurrentBufferSize());
    return emitted;
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

Condition JitTranslator::MapCond(ir::Cond cond) {
    // ir::Cond values match the ARM condition encoding.
    return static_cast<Condition>(static_cast<u8>(cond) & 0xF);
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

MemOperand JitTranslator::EmitMemOperand(ir::Operand& ir_op,
                                         ir::ValueType type,
                                         bool pair,
                                         bool atomic,
                                         bool allow_writeback,
                                         bool structured_guest_ea) {
    auto access_size = ir::GetValueSizeByte(type);
    if (ir_op.GetRight().Null()) {
        if (ir_op.GetLeft().IsImm()) {
            auto imm = ir_op.GetLeft().imm.Get();
            auto imm_signed = ir_op.GetLeft().imm.GetSigned();
            if (use_memory_base) {
                // Absolute guest address: materialize it, then apply the pt
                // bias (guest addr + pt = host addr). With a bounded guest
                // window the truncation happens at translation time — the
                // immediate is a compile-time constant, so it is free.
                __ Mov(mem_scratch, guest_addr_mask ? (imm & guest_addr_mask) : imm);
                if (atomic) {
                    __ Add(mem_scratch, mem_scratch, pt);
                    return MemOperand{mem_scratch};
                }
                return MemOperand{mem_scratch, pt};
            }
            bool can_imm = pair ? __ IsImmLSPair(imm_signed, access_size) : __ IsImmLSUnscaled(imm_signed);
            if (can_imm) {
                return MemOperand{xzr, imm_signed};
            } else {
                auto tmp = context.GetTmpX();
                __ Mov(tmp, imm);
                return MemOperand{tmp};
            }
        } else {
            // Match Case: load store post/index & push/pop
            auto addr_value = ir_op.GetLeft().value;
            if (mem_narrow_fuse && addr_value.Def()->GetOp() == ir::OpCode::GetOperand &&
                addr_value.Def()->GetUses() == 1) {
                auto source_operand = addr_value.Def()->GetArg<ir::Operand>(0);
                auto source_left = source_operand.GetLeft();
                if (source_operand.GetRight().Null() && source_left.IsValue() &&
                    context.SharesGPR(addr_value, source_left.value)) {
                    // A simple EA does not need to be materialised in the
                    // GetOperand result register. The RA tie makes the result
                    // own that same register through the memory use, so
                    // consume the live result allocation here rather than
                    // extending the source SSA's lifetime in the emitter.
                    disable_instructions.set(addr_value.Def()->Id());
                    auto address_reg = context.R(addr_value, true);
                    if (use_memory_base) {
                        return BiasMem(address_reg, atomic);
                    }
                    return MemOperand{address_reg};
                }
            }
            auto& instr_list = cur_block->GetInstList();
            auto instr = addr_value.Def();
            // With the pt bias active, post-index forms cannot express
            // [base + pt] (+writeback), so the folding is disabled and the
            // address update executes as a normal Add/Sub.
            if (allow_writeback && !use_memory_base && addr_value.Def()->GetUses() == 2) {
                int search_times{0};
                for (auto itr = instr_list.iterator_to(*instr);
                     itr != instr_list.end() && search_times < 3;
                     itr++, search_times++) {
                    auto add_sub =
                            itr->GetOp() == ir::OpCode::Add || itr->GetOp() == ir::OpCode::Sub;
                    if (!add_sub) {
                        continue;
                    }
                    auto same_value = itr->GetArg<ir::Value>(0) == addr_value;
                    if (!same_value) {
                        continue;
                    }
                    auto operand = itr->GetArg<ir::Operand>(1);
                    auto no_right = operand.GetRight().Null();
                    if (!no_right) {
                        continue;
                    }
                    auto same_register = context.R(addr_value) == context.R(itr.operator->());
                    if (!same_register) {
                        continue;
                    }
                    auto left = operand.GetLeft();
                    if (left.IsImm()) {
                        auto imm = left.imm.GetSigned();
                        if (!pair && !__ IsImmLSUnscaled(imm)) {
                            continue;
                        }
                        if (pair && !__ IsImmLSPair(imm, access_size)) {
                            continue;
                        }
                        if (itr->GetOp() == ir::OpCode::Add) {
                            disable_instructions.set(itr->Id());
                            return MemOperand{context.R(addr_value), imm, PostIndex};
                        } else {
                            disable_instructions.set(itr->Id());
                            return MemOperand{context.R(addr_value), -imm, PostIndex};
                        }
                    } else {
                        if (itr->GetOp() == ir::OpCode::Add) {
                            disable_instructions.set(itr->Id());
                            return MemOperand{
                                    context.R(addr_value), context.R(left.value), PostIndex};
                        }
                    }
                }
            }
            if (use_memory_base) {
                return BiasMem(context.R(addr_value), atomic);
            }
            return MemOperand{context.R(addr_value)};
        }
    } else {
        Register left_reg;
        if (ir_op.GetLeft().IsImm()) {
            // Materialize an immediate left side (absolute address + offset
            // forms) into a scratch register first.
            auto tmp = context.GetTmpX();
            __ Mov(tmp, ir_op.GetLeft().imm.Get());
            left_reg = tmp;
        } else {
            left_reg = context.R(ir_op.GetLeft().value, true);
        }
        auto right = ir_op.GetRight();
        if (right.IsImm()) {
            auto imm = right.imm.GetSigned();
            bool can_imm = pair ? __ IsImmLSPair(imm, access_size) : __ IsImmLSUnscaled(imm);
            if (can_imm) {
                if (ir_op.GetOp() == ir::OperandOp::Plus) {
                    if (use_memory_base) {
                        return BiasMem(left_reg, imm, atomic);
                    }
                    return MemOperand{left_reg, imm};
                } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                    if (use_memory_base) {
                        __ Lsl(mem_scratch, left_reg, imm);
                        return BiasMem(mem_scratch, atomic);
                    }
                    auto tmp = context.GetTmpX();
                    __ Lsl(tmp, left_reg, imm);
                    return MemOperand{tmp};
                } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                    if (use_memory_base) {
                        __ Lsr(mem_scratch, left_reg, imm);
                        return BiasMem(mem_scratch, atomic);
                    }
                    auto tmp = context.GetTmpX();
                    __ Lsr(tmp, left_reg, imm);
                    return MemOperand{tmp};
                } else {
                    PANIC();
                }
            } else {
                if (use_memory_base) {
                    __ Mov(mem_scratch, imm);
                    if (ir_op.GetOp() == ir::OperandOp::Plus) {
                        __ Add(mem_scratch, left_reg, mem_scratch);
                    } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                        __ Lsl(mem_scratch, left_reg, mem_scratch);
                    } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                        __ Lsr(mem_scratch, left_reg, mem_scratch);
                    } else {
                        PANIC();
                    }
                    return BiasMem(mem_scratch, atomic);
                }
                auto tmp = context.GetTmpX();
                __ Mov(tmp, imm);
                if (ir_op.GetOp() == ir::OperandOp::Plus) {
                    return MemOperand{left_reg, tmp};
                } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                    return MemOperand{left_reg, tmp, LSL};
                } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                    return MemOperand{left_reg, tmp, LSR};
                } else {
                    PANIC();
                }
            }
        } else {
            auto right_reg = context.R(right.value, true);
            if (ir_op.GetOp() == ir::OperandOp::Plus) {
                if (use_memory_base) {
                    if (structured_guest_ea && window_uxtw) {
                        // Compute the guest EA in W form before applying the
                        // host bias: pt + ((base + index) mod 2^32).
                        // This deliberately is not a prebiased base.
                        __ Add(mem_scratch.W(), left_reg.W(), right_reg.W());
                    } else {
                        __ Add(mem_scratch, left_reg, right_reg);
                    }
                    return BiasMem(mem_scratch, atomic);
                }
                return MemOperand{left_reg, right_reg};
            } else if (ir_op.GetOp() == ir::OperandOp::LSL) {
                if (use_memory_base) {
                    __ Lsl(mem_scratch, left_reg, right_reg);
                    return BiasMem(mem_scratch, atomic);
                }
                return MemOperand{left_reg, right_reg, LSL};
            } else if (ir_op.GetOp() == ir::OperandOp::LSR) {
                if (use_memory_base) {
                    __ Lsr(mem_scratch, left_reg, right_reg);
                    return BiasMem(mem_scratch, atomic);
                }
                return MemOperand{left_reg, right_reg, LSR};
            } else if (ir_op.GetOp() == ir::OperandOp::PlusExt) {
                auto shift_amount = ir_op.GetOp().shift_ext;
                if (structured_guest_ea && use_memory_base) {
                    if (window_uxtw) {
                        // Keep base/index/scale in one wrapping W add.
                        // BiasMem supplies the final pt + Wguest, UXTW step.
                        __ Add(mem_scratch.W(),
                               left_reg.W(),
                               Operand{right_reg.W(), LSL, shift_amount});
                    } else {
                        __ Add(mem_scratch, left_reg, Operand{right_reg, LSL, shift_amount});
                    }
                    return BiasMem(mem_scratch, atomic);
                }
                if (ir::GetValueSizeByte(right.value.Type()) == shift_amount) {
                    if (use_memory_base) {
                        __ Add(mem_scratch,
                               left_reg,
                               Operand{right_reg, LSL, shift_amount});
                        return BiasMem(mem_scratch, atomic);
                    }
                    return MemOperand{left_reg, right_reg, LSL, shift_amount};
                } else {
                    if (use_memory_base) {
                        __ Lsl(mem_scratch, right_reg, shift_amount);
                        __ Add(mem_scratch, left_reg, mem_scratch);
                        return BiasMem(mem_scratch, atomic);
                    }
                    auto tmp = context.GetTmpX();
                    __ Lsl(tmp, right_reg, shift_amount);
                    return MemOperand{left_reg, tmp};
                }
            } else {
                PANIC();
            }
        }
        return {};
    }
}




























































#undef masm

}  // namespace swift::runtime::backend::arm64
