#include "pinned_gpr_allocation.h"

#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace swift::runtime::backend::arm64 {

namespace {
// Env-gated clause histogram (SVM_BINDCENSUS) for the narrow matcher:
// early calls pass allow_widen_extend=true, late calls false. Clauses:
// 0=ok 1=basic 2=use-shape 3=widened-view 4=order/survives 5=consumer.
unsigned long long g_clause[2][6]{};
bool g_clause_suppress{};
// Per-read early clause for reads the late matcher later accepts.
// Keyed by Inst address (arena-stable while the unit is alive) and
// validated by id/home/width to shrug off address reuse.
struct EarlyRecord { int clause; u32 id; u32 home; u32 width; };
std::unordered_map<const ir::Inst*, EarlyRecord> g_early_record;
unsigned long long g_lateok_early[8]{};  // 0..5 clauses, 6=no-record, 7=mismatch
bool g_clause_registered{};
bool ClauseCensusEnabled() {
    static const bool enabled = std::getenv("SVM_BINDCENSUS") != nullptr;
    if (enabled && !g_clause_registered) {
        g_clause_registered = true;
        std::atexit([] {
            for (int side = 0; side < 2; ++side)
                std::fprintf(stderr, "[svm-clauseprobe] %s ok=%llu basic=%llu shape=%llu widen=%llu survives=%llu consumer=%llu\n",
                             side ? "late " : "early", g_clause[side][0], g_clause[side][1],
                             g_clause[side][2], g_clause[side][3], g_clause[side][4], g_clause[side][5]);
            std::fprintf(stderr, "[svm-clauseprobe] late-ok early-clause ok=%llu basic=%llu shape=%llu widen=%llu survives=%llu consumer=%llu norecord=%llu mismatch=%llu\n",
                         g_lateok_early[0], g_lateok_early[1], g_lateok_early[2], g_lateok_early[3],
                         g_lateok_early[4], g_lateok_early[5], g_lateok_early[6], g_lateok_early[7]);
        });
    }
    return enabled;
}
}  // namespace

std::optional<u16> MatchNarrowFixedGPRRead(
        ir::Inst& read, const BlockAnalysisIndex& block_analysis,
        const GuestStateMap& guest_state_map,
        const std::function<ir::Flags(ir::Inst*)>& set_flags, bool allow_widen_extend) {
    if (read.GetOp() != ir::OpCode::GetHostGPR) return std::nullopt;
    const bool census = ClauseCensusEnabled() && !g_clause_suppress;
    const int side = allow_widen_extend ? 0 : 1;
    const u32 home = read.GetArg<ir::Imm>(0).Get();
    const u32 width = ir::GetValueSizeByte(read.ReturnType());
    const auto fail = [&](int clause) {
        if (census) {
            ++g_clause[side][clause];
            if (side == 0) g_early_record[&read] = {clause, read.Id(), home, width};
        }
        return std::nullopt;
    };
    if (read.GetArg<ir::Imm>(1).Get() != 0 || !IsFixedGPRHome(home) ||
        read.GetUses() == 0 || width > sizeof(u32)) {
        return fail(1);
    }
    const auto uses = block_analysis.Uses(&read);
    if (uses.size() != 1 || uses.front().count != read.GetUses()) return fail(2);
    auto* consumer = uses.front().consumer;
    for (auto value : consumer->GetValues()) {
        if (value.Def() == &read && ir::GetValueSizeByte(value.Type()) > width)
            return fail(3);  // A dirty X register cannot supply a widened W capture.
    }
    if (consumer->Id() <= read.Id() ||
        !guest_state_map.FixedHomeSurvives(home, read.Id(), consumer->Id())) {
        return fail(4);
    }
    const auto op = consumer->GetOp();
    const u32 result_width = consumer->HasValue() && consumer->ReturnType() != ir::ValueType::VOID
            ? ir::GetValueSizeByte(consumer->ReturnType()) : 0;
    const bool direct_alu =
            (op == ir::OpCode::And || op == ir::OpCode::Xor) &&
            result_width <= sizeof(u32) &&
            (home <= 5 || width == sizeof(u32) ||
             (home >= 22 && !X86PinExtLevel3Requested()));
    const bool direct_or = op == ir::OpCode::Or &&
            width == sizeof(u32) && result_width == sizeof(u32);
    const bool add_sub = op == ir::OpCode::Add || op == ir::OpCode::Sub;
    const bool caller_alu = home <= 9 && add_sub &&
            result_width <= sizeof(u32) && (home <= 5 || width == sizeof(u32));
    const bool adjacent_flags = home >= 6 && home <= 9 &&
            (width == sizeof(u8) || width == sizeof(u16)) &&
            consumer->Id() == read.Id() + 1 && add_sub && result_width == width &&
            True(set_flags(consumer) & ir::Flags::NZCV);
    const bool callee_sub = home >= 19 && op == ir::OpCode::Sub &&
            result_width <= sizeof(u32);
    const bool zero_extend = (width == sizeof(u8) || width == sizeof(u16)) &&
            op == ir::OpCode::ZeroExtend32 && consumer->GetArg<ir::Value>(0).Def() == &read;
    const bool sign_extend = home >= 19 && op == ir::OpCode::SignExtend &&
            consumer->GetArg<ir::Value>(0).Def() == &read;
    // Early-only: a U32 read widened to U64 reads the same W home. Width
    // coalescing still owns the bridge and FinalizeFixedGPRReadConstraints
    // drops this constraint when a width chain adopts the read.
    const bool widen_extend = allow_widen_extend && width == sizeof(u32) &&
            op == ir::OpCode::ZeroExtend32To64 && consumer->GetArg<ir::Value>(0).Def() == &read;
    const bool store = uses.front().count == 1 && op == ir::OpCode::StoreMemory &&
            consumer->GetArg<ir::Value>(1).Def() == &read;
    if (!(direct_alu || direct_or || caller_alu || adjacent_flags || callee_sub ||
          zero_extend || sign_extend || widen_extend || store)) return fail(5);
    if (census) {
        ++g_clause[side][0];
        if (side == 0) {
            g_early_record[&read] = {0, read.Id(), home, width};
        } else {
            const auto found = g_early_record.find(&read);
            if (found == g_early_record.end()) ++g_lateok_early[6];
            else if (found->second.id != read.Id() || found->second.home != home ||
                     found->second.width != width) ++g_lateok_early[7];
            else ++g_lateok_early[found->second.clause];
        }
    }
    return std::optional<u16>{static_cast<u16>(home)};
}

namespace {
// Env-gated probe (SVM_BINDCENSUS): for reads the late matcher accepts
// but early preparation did not declare, attribute the difference to
// the GuestStateMap seeding (block-local Analyze vs the planner map)
// or to IR/use changes between the phases.
struct NarrowProbe {
    unsigned long long late_only{};
    unsigned long long seeding_diff{};
    unsigned long long other{};
};
NarrowProbe g_narrow_probe;
bool g_narrow_probe_registered{};
bool NarrowProbeEnabled() {
    static const bool enabled = std::getenv("SVM_BINDCENSUS") != nullptr;
    if (enabled && !g_narrow_probe_registered) {
        g_narrow_probe_registered = true;
        std::atexit([] {
            const auto& c = g_narrow_probe;
            std::fprintf(stderr, "[svm-narrowprobe] late_only=%llu seeding_diff=%llu other=%llu\n",
                         c.late_only, c.seeding_diff, c.other);
        });
    }
    return enabled;
}
}  // namespace

void GPRPlanBuilder::PrepareNarrowReads(
        const std::function<ir::Flags(ir::Inst*)>& set_flags) {
    ASSERT(!finished);
    const bool probe = NarrowProbeEnabled();
    for (auto& read : block->GetInstList()) {
        const auto* capture = allocation.PreservedGPRDefinitionAt(read.Id());
        if (read.GetOp() != ir::OpCode::GetHostGPR ||
            (allocation.IsFixedGPRDefinitionElided(read.Id()) && !capture) ||
            allocation.IsHostReadCoalesced(read.Id()) || HasLowView(&read) ||
            (ValueFullyResident(&read) && !capture)) continue;
        if (const auto home = MatchNarrowFixedGPRRead(
                    read, block_analysis, guest_state_map, set_flags)) {
            if (probe && !allocation.HasFixedGPRDefinition(read.Id())) {
                ++g_narrow_probe.late_only;
                GuestStateMap local;
                local.Analyze(block, features);
                g_clause_suppress = true;
                const bool local_ok = MatchNarrowFixedGPRRead(
                        read, block_analysis, local, set_flags, true).has_value();
                g_clause_suppress = false;
                if (local_ok)
                    ++g_narrow_probe.other;
                else
                    ++g_narrow_probe.seeding_diff;
            }
            AssignLowView(&read, *home);
        }
    }
}

}  // namespace swift::runtime::backend::arm64
