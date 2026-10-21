#pragma once
#include <map>
#include <functional>
#include <optional>
#include <unordered_set>
#include "pstate_facts.h"
#include "runtime/backend/arm64/jit/block_analysis_index.h"

namespace swift::runtime::backend::arm64 {
struct BooleanConsumerAction {
    ir::Inst* logical_user{};
    bool later_local_control{}, last_instruction{};
};
struct AluSplitFacts {
    u32 value_uses{}, external_users{};
    u8 family_count{};
    bool family_capped{}, saw_address{}, saw_value{};
};
struct ConsumerPlan {
    struct PairResultAction {
        ir::OpCode secondary_opcode;
        ir::Inst* secondary{};
    };
    bool valid{true};
    std::map<const ir::Inst*, PairResultAction> pair_results;
    std::map<const ir::Inst*, AluSplitFacts> alu_split;
    std::map<const ir::Inst*, BooleanConsumerAction> booleans;
    std::map<const ir::Inst*, ir::Flags> string_flags;
    struct FCmpAction {
        bool compact_carrier{}, packed_relation{}, raw_all_conditions{};
        ir::Inst* raw_condition{};
    };
    std::map<const ir::Inst*, FCmpAction> comparisons;
    std::unordered_set<const ir::Inst*> raw_fcmp_conditions;
    std::map<const ir::Inst*, std::vector<ir::Inst*>> carry_inversions;
    std::optional<PstateFacts> pstate;
    std::unordered_set<const ir::Inst*> local_conditions;
    std::unordered_set<const ir::Inst*> later_pstate;
    std::map<const ir::Inst*, ir::Inst*> sole_users;
    std::map<const ir::Inst*, ir::Inst*> compound_clears;
    std::unordered_set<const ir::Inst*> incoming_zeros;
};
ConsumerPlan BuildConsumerPlan(const BlockAnalysisIndex& analysis,
        const std::function<bool(const ir::Inst&)>& retains_nzcv,
        const std::function<bool(const ir::Inst&)>& observes,
        const ir::Terminal* terminal = nullptr, bool alu_split_audit = false);
}
