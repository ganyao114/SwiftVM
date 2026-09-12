#include "translator.h"
#include <numeric>
#include <cstring>
#include "aarch64/disasm-aarch64.h"
#include "runtime/backend/context.h"
#include "runtime/common/svm_config.h"
#include "translator/x86/cpu.h"
namespace swift::runtime::backend::arm64 {
void JitTranslator::ResetBoundaryDensity() {
    statistics.boundary_density_enabled = context.DensityProfileEnabled();
    statistics.boundary_terminal_open = false;
    statistics.boundary_terminal_link_bytes = 0;
    statistics.boundary_density_bytes.fill(0);
    for (auto& mnemonics : statistics.boundary_density_mnemonics) {
        mnemonics.clear();
    }
    statistics.boundary_terminal_link_mnemonics.clear();
    statistics.boundary_terminal_link_ranges.clear();
}

void JitTranslator::RecordBoundaryRange(BoundarySubsequence category,
                                        u32 begin,
                                        u32 end) {
    if (!statistics.boundary_density_enabled || begin == end) {
        return;
    }
    ASSERT(end > begin);
    ASSERT((end - begin) % vixl::aarch64::kInstructionSize == 0);
    const auto index = static_cast<size_t>(category);
    statistics.boundary_density_bytes[index] += end - begin;

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
        ++statistics.boundary_density_mnemonics[index][mnemonic];
        if (category == BoundarySubsequence::LinkTail && statistics.boundary_terminal_open) {
            ++statistics.boundary_terminal_link_mnemonics[mnemonic];
        }
    }
    if (category == BoundarySubsequence::LinkTail && statistics.boundary_terminal_open) {
        statistics.boundary_terminal_link_bytes += end - begin;
        statistics.boundary_terminal_link_ranges.emplace_back(begin, end);
    }
}

void JitTranslator::PrintBoundaryDensity(u64 guest_pc,
                                         u32 expected_boundary_bytes) {
    const auto terminal = static_cast<size_t>(BoundarySubsequence::TerminalMain);
    ASSERT(statistics.boundary_density_bytes[terminal] >= statistics.boundary_terminal_link_bytes);
    statistics.boundary_density_bytes[terminal] -= statistics.boundary_terminal_link_bytes;
    for (const auto& [mnemonic, count] : statistics.boundary_terminal_link_mnemonics) {
        auto it = statistics.boundary_density_mnemonics[terminal].find(mnemonic);
        ASSERT(it != statistics.boundary_density_mnemonics[terminal].end());
        ASSERT(it->second >= count);
        it->second -= count;
        if (it->second == 0) {
            statistics.boundary_density_mnemonics[terminal].erase(it);
        }
    }

    const u32 subtotal = std::accumulate(statistics.boundary_density_bytes.begin(),
                                         statistics.boundary_density_bytes.end(), 0u);
    // Strict unit-local commoning proof: two byte-identical final AArch64
    // instructions can be emitted once, with each duplicate site replaced by
    // a one-instruction B. Therefore each duplicate beyond the first has an
    // exact one-instruction (4-byte) removable ceiling. Restrict this to link
    // leaves inside the terminal; body-side RSB pushes and cold veneers cannot
    // accidentally enter the proof.
    std::map<u64, u32> common_suffixes;
    const auto* host_bytes = masm.GetBuffer()->GetStartAddress<const u8*>();
    for (const auto& [begin, end] : statistics.boundary_terminal_link_ranges) {
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
                 statistics.boundary_density_bytes[0], statistics.boundary_density_bytes[1],
                 statistics.boundary_density_bytes[2], statistics.boundary_density_bytes[3], subtotal,
                 expected_boundary_bytes, common2_groups, common2_ranges,
                 common2_saved);
    static constexpr std::array<const char*, 4> names{
            "prologue", "terminal", "link", "cold"};
    for (size_t i = 0; i < names.size(); ++i) {
        std::fprintf(stderr, " mn_%s=", names[i]);
        if (statistics.boundary_density_mnemonics[i].empty()) {
            std::fputc('-', stderr);
            continue;
        }
        bool first = true;
        for (const auto& [mnemonic, count] : statistics.boundary_density_mnemonics[i]) {
            std::fprintf(stderr, "%s%s:%u", first ? "" : ",",
                         mnemonic.c_str(), count);
            first = false;
        }
    }
    std::fputc('\n', stderr);
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
                     statistics.pfaf_density_bytes[0] & 0xffffu,
                     statistics.pfaf_density_bytes[1] & 0xffffu,
                     statistics.pfaf_density_bytes[2] & 0xffffu,
                     statistics.pfaf_density_bytes[3] & 0xffffu,
                     statistics.pfaf_density_bytes[4] & 0xffffu,
                     statistics.pfaf_density_bytes[5] & 0xffffu,
                     statistics.pfaf_density_bytes[0] >> 16,
                     statistics.pfaf_density_bytes[1] >> 16,
                     statistics.pfaf_density_bytes[2] >> 16,
                     statistics.pfaf_density_bytes[3] >> 16,
                     statistics.pfaf_density_bytes[4] >> 16,
                     statistics.pfaf_density_bytes[5] >> 16);
        PrintBoundaryDensity(block->GetStartLocation().Value(), density_bytes[4]);
    }
    if (region_edges_active && (density || context.ExecProfileEnabled())) {
        std::fprintf(stderr,
                     "[svm-region-edge] pc=0x%llx edges=%u cycles=%u "
                     "fallthrough=%u local_branch_bytes=%u poll_bytes=%u\n",
                     static_cast<unsigned long long>(
                             block->GetStartLocation().Value()),
                     statistics.region_block_edges,
                     statistics.region_block_cycles,
                     statistics.region_block_fallthroughs,
                     statistics.region_block_local_branch_bytes,
                     static_cast<u32>(statistics.region_block_cycles * 2u * sizeof(u32)));
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
}
