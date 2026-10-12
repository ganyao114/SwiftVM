#include <catch2/catch_test_macros.hpp>
#include <vector>
#include "runtime/backend/x64/scalar_register_plan.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
using Plan = backend::x64::ScalarRegisterPlan;
using Rejection = backend::x64::ScalarPlanRejection;
using Phase = backend::x64::ScalarCachePhase;
using T = ir::ValueType;
namespace {
IntrusivePtr<ir::Block> NewBlock() { return new ir::Block(ir::Location{0x1000}); }
void Store(ir::Assembler& as, ir::Value value, u32 offset = 0) {
    as.StoreUniform(ir::Uniform{offset, value.Type()}, value);
}
void CheckFinalAssignments(const Plan& plan) {
    REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
    std::vector<ir::Inst*> seen;
    for (const auto& interval : plan.Intervals()) {
        REQUIRE(plan.Find(interval.producer) == &interval);
        REQUIRE(plan.Slot(interval.producer) == interval.slot);
        REQUIRE(plan.RequiresCanonicalHome(interval.producer) == interval.requires_canonical_home);
        REQUIRE(interval.slot < Plan::host_register_indices.size());
        for (auto* producer : seen) REQUIRE(producer != interval.producer);
        if (!seen.empty()) REQUIRE(plan.Find(seen.back())->definition_position < interval.definition_position);
        seen.push_back(interval.producer);
    }
}
}

TEST_CASE("x64 scalar replacement removes a complete interval from every slot", "[x64][scalar-plan][replacement]") {
    for (u32 victim = 0; victim < 3; ++victim) {
        CAPTURE(victim);
        auto block = NewBlock(); ir::Assembler as{block.get()};
        std::array<ir::Value, 3> inputs;
        for (u32 i = 0; i < 3; ++i) inputs[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
        Store(as, inputs[victim], 64);
        auto short_lived = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
        Store(as, short_lived, 72); Store(as, inputs[victim], 80);
        for (u32 i = 0; i < 3; ++i) if (i != victim) Store(as, inputs[i], 88 + i * 8);
        Store(as, inputs[victim], 112);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        std::vector<ir::Inst*> original;
        for (auto& inst : block->GetInstList()) original.push_back(&inst);
        const auto plan = Plan::Build(block.get());
        CheckFinalAssignments(plan);
        REQUIRE(plan.Intervals().size() == 3);
        REQUIRE_FALSE(plan.Slot(inputs[victim].Def()));
        REQUIRE(plan.Find(inputs[victim].Def()) == nullptr);
        REQUIRE(plan.RequiresCanonicalHome(inputs[victim].Def()));
        REQUIRE(plan.Slot(short_lived.Def()) == victim);
        REQUIRE(plan.Find(short_lived.Def())->definition_position == 4);
        REQUIRE(plan.Find(short_lived.Def())->last_use == 5);
        REQUIRE(plan.Find(short_lived.Def())->use_positions == std::vector<u32>{5});
        for (u32 i = 0; i < 3; ++i) if (i != victim) REQUIRE(plan.Slot(inputs[i].Def()) == i);
        const auto again = Plan::Build(block.get()); CheckFinalAssignments(again);
        REQUIRE(again.Intervals().size() == plan.Intervals().size());
        for (auto* inst : original) {
            REQUIRE(plan.Slot(inst) == again.Slot(inst));
            REQUIRE(plan.RequiresCanonicalHome(inst) == again.RequiresCanonicalHome(inst));
        }
        u32 position{};
        for (auto& inst : block->GetInstList()) REQUIRE(&inst == original.at(position++));
        REQUIRE(position == original.size());
        for (auto value : inputs) REQUIRE(value.Type() == T::U64);
        REQUIRE(short_lived.Type() == T::U64);
        REQUIRE(block->GetInstList().size() == original.size());
    }
}

TEST_CASE("x64 scalar replacement compacts consecutive victims including a replacement", "[x64][scalar-plan][replacement]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    std::array<ir::Value, 7> values;
    constexpr std::array<u32, 7> definitions{0, 1, 2, 4, 6, 8, 10};
    constexpr std::array<u32, 7> endpoints{33, 31, 29, 25, 21, 17, 13};
    for (u32 position = 0; position <= 33; ++position) {
        bool emitted{};
        for (u32 i = 0; i < values.size(); ++i) {
            if (position == definitions[i]) {
                values[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64); emitted = true;
            } else if (position == endpoints[i]) { Store(as, values[i], 128 + i * 8); emitted = true; }
        }
        if (position == 3) { Store(as, values[0], 64); emitted = true; }
        for (u32 i = 3; i < values.size(); ++i)
            if (position == definitions[i] + 1) { Store(as, values[i], 64 + i * 8); emitted = true; }
        if (!emitted) as.AppendInst(ir::OpCode::Nop);
    }
    const auto plan = Plan::Build(block.get()); CheckFinalAssignments(plan);
    REQUIRE(plan.Intervals().size() == 3);
    for (u32 i = 0; i < 4; ++i) {
        REQUIRE_FALSE(plan.Slot(values[i].Def())); REQUIRE(plan.Find(values[i].Def()) == nullptr);
        REQUIRE(plan.RequiresCanonicalHome(values[i].Def()));
    }
    constexpr std::array<u32, 3> final_slots{1, 2, 0};
    for (u32 i = 4; i < values.size(); ++i) {
        const auto* interval = plan.Find(values[i].Def()); REQUIRE(interval);
        REQUIRE(interval->definition_position == definitions[i]); REQUIRE(interval->last_use == endpoints[i]);
        REQUIRE(interval->slot == final_slots[i - 4]);
        REQUIRE(interval->use_positions == std::vector<u32>{definitions[i] + 1, endpoints[i]});
    }
}

TEST_CASE("x64 scalar replacement never uses phase to replace equal-position ends", "[x64][scalar-plan][replacement][phases]") {
    for (bool candidate_opaque : {false, true}) {
        CAPTURE(candidate_opaque);
        auto block = NewBlock(); ir::Assembler as{block.get()};
        std::array<ir::Value, 3> inputs;
        for (u32 i = 0; i < 3; ++i) inputs[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
        auto candidate = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
        if (candidate_opaque) {
            auto sum = as.Add(inputs[0], ir::Operand{inputs[1], inputs[2], {ir::OperandOp::Plus, 0}}).SetType(T::U64);
            sum.Def()->SetArg(4, ir::Lambda{candidate});
        } else {
            Store(as, candidate, 64);
            auto& consumer = block->GetInstList().back();
            consumer.SetArg(2, inputs[0]); consumer.SetArg(3, ir::Lambda{inputs[1]}); consumer.SetArg(4, inputs[2]);
        }
        const auto plan = Plan::Build(block.get()); CheckFinalAssignments(plan);
        REQUIRE_FALSE(plan.Slot(candidate.Def())); REQUIRE(plan.RequiresCanonicalHome(candidate.Def()));
        REQUIRE(plan.Intervals().size() == 3);
        for (u32 i = 0; i < 3; ++i) {
            REQUIRE(plan.Slot(inputs[i].Def()) == i);
            REQUIRE(plan.Find(inputs[i].Def())->last_use == 4);
            REQUIRE(plan.Find(inputs[i].Def())->last_use_phase == (candidate_opaque ? Phase::Read : Phase::Write));
        }
    }
}

TEST_CASE("x64 scalar replacement chooses first farthest slot and prefers expiration", "[x64][scalar-plan][replacement]") {
    for (bool expired : {false, true}) {
        CAPTURE(expired);
        auto block = NewBlock(); ir::Assembler as{block.get()};
        std::array<ir::Value, 3> inputs;
        for (u32 i = 0; i < 3; ++i) inputs[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
        if (expired) Store(as, inputs[2], 64);
        auto candidate = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
        Store(as, candidate, 72);
        if (expired) { Store(as, inputs[0], 80); Store(as, inputs[1], 88); }
        else {
            auto* consumer = as.AppendInst(ir::OpCode::Nop);
            for (u32 i = 0; i < 3; ++i) consumer->SetArg(i, inputs[i]);
        }
        const auto plan = Plan::Build(block.get()); CheckFinalAssignments(plan);
        REQUIRE(plan.Slot(candidate.Def()) == (expired ? 2 : 0));
        REQUIRE(plan.Intervals().size() == (expired ? 4 : 3));
        REQUIRE(bool(plan.Slot(inputs[0].Def())) == expired);
        REQUIRE(plan.Slot(inputs[1].Def()) == 1); REQUIRE(plan.Slot(inputs[2].Def()) == 2);
        if (!expired) REQUIRE(plan.RequiresCanonicalHome(inputs[0].Def()));
    }
}

TEST_CASE("x64 scalar plan uses fixed SysV registers and skips unused producers", "[x64][scalar-plan]") {
    REQUIRE(Plan::host_register_indices == std::array<unsigned, 3>{3, 5, 15});
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto unused = as.LoadImm(ir::Imm{4}).SetType(T::U64);
    auto used = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    Store(as, used);
    const auto plan = Plan::Build(block.get());
    REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
    REQUIRE_FALSE(plan.Slot(unused.Def()));
    REQUIRE(plan.RequiresCanonicalHome(unused.Def()));
    REQUIRE(plan.RequiresCanonicalHome(nullptr));
    REQUIRE(plan.Slot(used.Def()) == 0);
    REQUIRE_FALSE(plan.RequiresCanonicalHome(used.Def()));
    REQUIRE_FALSE(plan.Find(used.Def())->requires_canonical_home);
    REQUIRE(plan.Intervals().size() == 1);
    REQUIRE(plan.Find(used.Def())->definition_position == 1);
    REQUIRE(plan.Find(used.Def())->last_use == 2);
    REQUIRE(plan.Find(used.Def())->use_positions == std::vector<u32>{2});
}

TEST_CASE("x64 scalar plan retains three live values and falls back under pressure", "[x64][scalar-plan]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    std::array<ir::Value, 5> values;
    for (u32 index = 0; index < values.size(); ++index)
        values[index] = as.LoadUniform(ir::Uniform{index * 8, T::U64}).SetType(T::U64);
    for (auto value : values) Store(as, value);
    auto after = as.LoadImm(ir::Imm{7}).SetType(T::U64); Store(as, after);
    const auto plan = Plan::Build(block.get());
    REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
    REQUIRE(plan.Slot(values[0].Def()) == 0); REQUIRE(plan.Slot(values[1].Def()) == 1);
    REQUIRE(plan.Slot(values[2].Def()) == 2);
    REQUIRE_FALSE(plan.Slot(values[3].Def())); REQUIRE_FALSE(plan.Slot(values[4].Def()));
    REQUIRE(plan.RequiresCanonicalHome(values[3].Def()));
    REQUIRE(plan.RequiresCanonicalHome(values[4].Def()));
    REQUIRE(plan.Slot(after.Def()) == 0); REQUIRE(plan.Intervals().size() == 4);
}

TEST_CASE("x64 scalar plan reuses a dead input after all reads at its endpoint", "[x64][scalar-plan]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto a = as.LoadImm(ir::Imm{1}).SetType(T::U64);
    auto b = as.Add(a, ir::Operand{ir::Imm{2}}).SetType(T::U64);
    auto c = as.Add(b, ir::Operand{ir::Imm{3}}).SetType(T::U64);
    Store(as, c);
    const auto plan = Plan::Build(block.get());
    REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
    REQUIRE(plan.Slot(a.Def()) == 0); REQUIRE(plan.Slot(b.Def()) == 0);
    REQUIRE(plan.Slot(c.Def()) == 0);
    REQUIRE(plan.Find(a.Def())->last_use == plan.Find(b.Def())->definition_position);
    REQUIRE(plan.Find(b.Def())->last_use == plan.Find(c.Def())->definition_position);
    REQUIRE(plan.Find(a.Def())->last_use_phase == Phase::Read);
    REQUIRE(plan.Find(b.Def())->last_use_phase == Phase::Read);
}

TEST_CASE("x64 scalar plan waits for every physical final occurrence before reuse", "[x64][scalar-plan][phases]") {
    for (u32 shape = 0; shape < 4; ++shape) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto b = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        auto c = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        ir::Value result;
        if (shape == 0) result = as.Add(a, ir::Operand{c, c, {ir::OperandOp::Plus, 0}});
        else if (shape == 1) result = as.MulSub(a, b, c);
        else if (shape == 2) result = as.BitInsert(a, c, ir::Imm{8}, ir::Imm{16});
        else result = as.LslValue(a, c);
        result.SetType(T::U64);
        Store(as, a); Store(as, b); Store(as, result);
        const auto plan = Plan::Build(block.get());
        REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
        REQUIRE(plan.Slot(c.Def()) == 2); REQUIRE(plan.Slot(result.Def()) == 2);
        REQUIRE(plan.Find(c.Def())->last_use_phase == Phase::Read);
        REQUIRE(plan.Find(c.Def())->last_use == plan.Find(result.Def())->definition_position);
        REQUIRE(plan.Find(c.Def())->use_positions.size() == (shape == 0 ? 2 : 1));
        REQUIRE_FALSE(plan.RequiresCanonicalHome(result.Def()));
    }
}

TEST_CASE("x64 scalar plan does not reuse an opaque occurrence at the same boundary", "[x64][scalar-plan][phases]") {
    for (u32 shape = 0; shape < 5; ++shape) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto b = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        auto c = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        auto view = c;
        if (shape == 3) view.SetCastType(T::V64);
        auto result = shape == 4 ? ir::Value{as.AppendInst(ir::OpCode::Not, c)} :
                                  as.Add(a, ir::Operand{view});
        result.SetType(T::U64);
        if (shape == 0) result.Def()->SetArg(4, c);
        else if (shape == 1) result.Def()->SetArg(4, ir::Lambda{c});
        else if (shape == 2) {
            ir::Params params; params.Push(c); params.Push(c); result.Def()->SetArg(4, params);
        }
        Store(as, a); Store(as, b); Store(as, result);
        const auto plan = Plan::Build(block.get());
        REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
        REQUIRE(plan.Slot(c.Def()) == 2);
        REQUIRE(plan.Find(c.Def())->last_use_phase == Phase::Write);
        REQUIRE_FALSE(plan.Slot(result.Def())); REQUIRE(plan.RequiresCanonicalHome(result.Def()));
    }
}

TEST_CASE("x64 scalar plan keeps inputs with later stores or metadata publications live", "[x64][scalar-plan][phases]") {
    for (bool delayed_flags : {false, true}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto b = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        auto c = as.Add(a, ir::Operand{b}).SetType(T::U64);
        // Keep both original inputs live, filling all three registers.
        auto result = as.Add(c, ir::Operand{a}).SetType(T::U64);
        if (delayed_flags) as.SaveFlags(c, ir::Flags::NZCV);
        else Store(as, c);
        Store(as, a); Store(as, b); Store(as, result);
        const auto plan = Plan::Build(block.get());
        REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
        REQUIRE(plan.Slot(c.Def()) == 2); REQUIRE_FALSE(plan.Slot(result.Def()));
        REQUIRE(plan.Find(c.Def())->last_use > plan.Find(c.Def())->definition_position + 1);
    }
}

TEST_CASE("x64 scalar plan keeps SSA snapshots live across callback and raw consumers", "[x64][scalar-plan]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto b = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto operand = as.GetOperand(ir::Operand{a, b, {ir::OperandOp::Plus, 0}}).SetType(T::U64);
    ir::Params params; params.Push(b); params.Push(b);
    auto callback = as.CallDynamic(ir::Lambda{a}, params).SetType(T::U64);
    auto raw = as.BitCast(a).SetType(T::V128); // This consumer reads the canonical 16-byte home.
    Store(as, raw); Store(as, callback); Store(as, operand); Store(as, a);
    const auto plan = Plan::Build(block.get());
    REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
    REQUIRE(plan.Slot(a.Def()) == 0); REQUIRE(plan.Slot(b.Def()) == 1);
    REQUIRE(plan.Find(a.Def())->use_positions == std::vector<u32>{2, 3, 4, 8});
    REQUIRE(plan.Find(b.Def())->use_positions == std::vector<u32>{2, 3, 3});
    REQUIRE_FALSE(plan.Slot(callback.Def())); REQUIRE_FALSE(plan.Slot(raw.Def()));
    REQUIRE(plan.RequiresCanonicalHome(a.Def()));
    REQUIRE(plan.RequiresCanonicalHome(b.Def()));
    REQUIRE_FALSE(plan.RequiresCanonicalHome(operand.Def()));
}

TEST_CASE("x64 scalar plan visits nested terminal values after all instructions", "[x64][scalar-plan]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
    auto key = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    (void)as.LoadImm(ir::Imm{9}).SetType(T::U64);
    Vector<ir::terminal::Switch::Case> cases;
    cases.push_back({ir::Imm{2}, ir::terminal::ReturnToHost{}});
    block->SetTerminal(ir::terminal::CheckHalt{ir::terminal::If{
        ir::BOOL{condition}, ir::terminal::Switch{key, cases},
        ir::terminal::Condition{ir::Cond::EQ, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToHost{}}}});
    const auto plan = Plan::Build(block.get());
    REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
    REQUIRE(plan.Find(condition.Def())->last_use == 3);
    REQUIRE(plan.Find(key.Def())->last_use == 3);
    REQUIRE(plan.Intervals().size() == 2);
    REQUIRE_FALSE(plan.RequiresCanonicalHome(condition.Def()));
    REQUIRE_FALSE(plan.RequiresCanonicalHome(key.Def()));
}

TEST_CASE("x64 scalar plan excludes raw containers and pair results with scalar annotations", "[x64][scalar-plan]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto raw_input = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::U64);
    auto bitcast = as.BitCast(raw_input).SetType(T::U64);
    auto alias = as.GetResult(bitcast, bitcast).SetType(T::U64);
    auto selected = as.Select(ir::BOOL{alias}, alias, bitcast).SetType(T::U64);
    auto pair = as.Cpuid(alias, bitcast, ir::Imm{0}, ir::Imm{1}).SetType(T::U64);
    auto upper = as.CpuidUpper(pair).SetType(T::U64);
    auto div_pair = as.Div128(alias, bitcast, selected, ir::Imm{0}).SetType(T::U64);
    auto remainder = as.Div128Remainder(div_pair).SetType(T::U64);
    for (auto value : {raw_input, bitcast, alias, selected, pair, upper, div_pair, remainder}) Store(as, value);
    const auto plan = Plan::Build(block.get());
    REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping()); REQUIRE(plan.Intervals().empty());
}

TEST_CASE("x64 scalar plan rejects Phi edge uses and FunctionPlan conservatively", "[x64][scalar-plan]") {
    for (auto op : {ir::OpCode::AddPhi}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto input = as.LoadImm(ir::Imm{1}).SetType(T::U64); Store(as, input);
        as.AppendInst(op);
        const auto plan = Plan::Build(block.get());
        REQUIRE_FALSE(plan.Enabled()); REQUIRE(plan.Rejection() == Rejection::ControlFlow);
        REQUIRE(plan.Intervals().empty()); REQUIRE_FALSE(plan.Slot(input.Def()));
        REQUIRE(plan.RequiresCanonicalHome(input.Def()));
    }
    auto block = NewBlock();
    const auto plan = Plan::Build(block.get(), true);
    REQUIRE_FALSE(plan.Enabled()); REQUIRE(plan.Rejection() == Rejection::FunctionPlan);
    REQUIRE(plan.Intervals().empty());
    REQUIRE(Plan::Build(nullptr).Rejection() == Rejection::MissingBlock);
}

TEST_CASE("x64 scalar plan rejects undefined foreign and non-dominating SSA", "[x64][scalar-plan]") {
    auto external = NewBlock(); ir::Assembler external_as{external.get()};
    auto foreign = external_as.LoadImm(ir::Imm{4}).SetType(T::U64);
    for (auto invalid : {ir::Value{}, foreign}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto input = as.LoadImm(ir::Imm{1}).SetType(T::U64); Store(as, input);
        auto value = as.Add(input, ir::Operand{ir::Imm{2}}).SetType(T::U64); Store(as, value);
        // The assembler rejects null inputs before the planner can see them.
        // Temporarily inject the malformed operand without changing use counts.
        const auto saved_arg = value.Def()->ArgAt(0);
        value.Def()->ArgAt(0) = invalid;
        const auto plan = Plan::Build(block.get());
        value.Def()->ArgAt(0) = saved_arg;
        REQUIRE_FALSE(plan.Enabled()); REQUIRE(plan.Rejection() == Rejection::InvalidSSA);
        REQUIRE(plan.Intervals().empty());
    }
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto initial = as.LoadImm(ir::Imm{1}).SetType(T::U64);
    auto before = as.Add(initial, ir::Operand{ir::Imm{2}}).SetType(T::U64);
    auto later = as.LoadImm(ir::Imm{4}).SetType(T::U64);
    before.Def()->SetArg(0, later); Store(as, before);
    const auto plan = Plan::Build(block.get());
    REQUIRE_FALSE(plan.Enabled()); REQUIRE(plan.Rejection() == Rejection::InvalidSSA);
    REQUIRE(plan.Intervals().empty());
}

TEST_CASE("x64 scalar plan does not mutate IR or its terminal references", "[x64][scalar-plan]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto a = as.LoadImm(ir::Imm{1}).SetType(T::U64);
    auto b = as.Add(a, ir::Operand{a}).SetType(T::U64); Store(as, b);
    const auto opcode = b.Def()->GetOp(); const auto type = b.Def()->ReturnType();
    const auto first = b.Def()->GetArg<ir::Value>(0).Def();
    const auto plan = Plan::Build(block.get());
    REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
    REQUIRE(b.Def()->GetOp() == opcode); REQUIRE(b.Def()->ReturnType() == type);
    REQUIRE(b.Def()->GetArg<ir::Value>(0).Def() == first);
    REQUIRE(plan.Find(a.Def())->use_positions == std::vector<u32>{1, 1});
}

TEST_CASE("x64 scalar home plan audits both physical Operand values and delayed flag uses", "[x64][scalar-plan][home]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto left = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto right = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto operand = as.GetOperand(ir::Operand{left, right, {ir::OperandOp::Plus, 0}}).SetType(T::U64);
    auto sum = as.Add(left, ir::Operand{operand, right, {ir::OperandOp::Plus, 0}}).SetType(T::U64);
    as.SaveFlags(sum, ir::Flags::NZ);
    as.SaveFlags(sum, ir::Flags::CV | ir::Flags::AuxiliaryCarry);
    Store(as, sum);
    const auto plan = Plan::Build(block.get());
    REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
    for (auto value : {left, right, operand}) {
        REQUIRE(plan.Slot(value.Def()));
        REQUIRE_FALSE(plan.RequiresCanonicalHome(value.Def()));
    }
    // All three final input occurrences finish before the sum is published.
    REQUIRE(plan.Slot(sum.Def()) == 0);
    REQUIRE_FALSE(plan.RequiresCanonicalHome(sum.Def()));
    REQUIRE(plan.Find(left.Def())->use_positions == std::vector<u32>{2, 3});
    REQUIRE(plan.Find(right.Def())->use_positions == std::vector<u32>{2, 3});
    REQUIRE(plan.Find(operand.Def())->use_positions == std::vector<u32>{3});
}

TEST_CASE("x64 scalar home plan retains any raw or opaque consumer alongside scalar uses", "[x64][scalar-plan][home]") {
    using O = ir::OpCode;
    for (auto op : {O::BitCast, O::Select, O::GetResult, O::VecXor, O::Div}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        Store(as, input);
        if (op == O::BitCast) (void)as.BitCast(input).SetType(T::U64);
        else if (op == O::Select) (void)as.Select(ir::BOOL{input}, input, input).SetType(T::U64);
        else if (op == O::GetResult) (void)as.GetResult(input, input).SetType(T::U64);
        else if (op == O::VecXor) (void)as.VecXor(input, input).SetType(T::U64);
        else (void)as.Div(input, ir::Operand{ir::Imm{2}}).SetType(T::U64);
        const auto plan = Plan::Build(block.get());
        REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
        REQUIRE(plan.Slot(input.Def()));
        REQUIRE(plan.RequiresCanonicalHome(input.Def()));
    }
}

TEST_CASE("x64 scalar home plan vetoes unexpected physical values Lambda and Params", "[x64][scalar-plan][home]") {
    for (u32 decoration = 0; decoration < 3; ++decoration) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto result = as.Add(input, ir::Operand{ir::Imm{2}}).SetType(T::U64);
        // A valid scalar use in slot 0 does not license the extra physical
        // slot, even if this emitter currently ignores that extra argument.
        if (decoration == 0) result.Def()->SetArg(4, input);
        else if (decoration == 1) result.Def()->SetArg(4, ir::Lambda{input});
        else {
            ir::Params params; params.Push(input); params.Push(input);
            result.Def()->SetArg(4, params);
        }
        Store(as, result);
        const auto plan = Plan::Build(block.get());
        REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
        REQUIRE(plan.Slot(input.Def())); REQUIRE(plan.RequiresCanonicalHome(input.Def()));
        REQUIRE_FALSE(plan.RequiresCanonicalHome(result.Def()));
        REQUIRE(plan.Find(input.Def())->use_positions.size() == (decoration == 2 ? 3 : 2));
    }
}

TEST_CASE("x64 scalar home plan requires strict scalar views and uniform descriptors", "[x64][scalar-plan][home]") {
    for (auto view_type : {T::U8, T::S16, T::U32, T::S64, T::V8, T::V16, T::V32, T::V64, T::V128}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto view = input; (void)view.SetCastType(view_type);
        as.StoreUniform(ir::Uniform{8, T::U64}, view);
        const auto plan = Plan::Build(block.get());
        REQUIRE(plan.Enabled()); REQUIRE(plan.Slot(input.Def()));
        REQUIRE(input.Type() == T::U64);
        REQUIRE(plan.RequiresCanonicalHome(input.Def()) == (view_type >= T::V8));
    }
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    // The emitter follows the value width. Still retain a home for a vector
    // descriptor until that mismatched annotation is explicitly audited.
    as.StoreUniform(ir::Uniform{8, T::V128}, input);
    const auto plan = Plan::Build(block.get());
    REQUIRE(plan.Enabled()); REQUIRE(plan.Slot(input.Def()));
    REQUIRE(plan.RequiresCanonicalHome(input.Def()));
}

TEST_CASE("x64 scalar home plan preserves homes for raw terminal views", "[x64][scalar-plan][home]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto raw_view = input; (void)raw_view.SetCastType(T::V64);
    Vector<ir::terminal::Switch::Case> cases;
    cases.push_back({ir::Imm{1}, ir::terminal::ReturnToHost{}});
    block->SetTerminal(ir::terminal::Switch{raw_view, cases});
    const auto plan = Plan::Build(block.get());
    REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
    REQUIRE(plan.Slot(input.Def())); REQUIRE(plan.RequiresCanonicalHome(input.Def()));
    REQUIRE(plan.Find(input.Def())->last_use == 1);
}

TEST_CASE("x64 scalar home plan allows scalar producer chains and branch-only publications", "[x64][scalar-plan][home]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto narrow_view = input; (void)narrow_view.SetCastType(T::U32);
    auto sum = as.Add(narrow_view, ir::Operand{ir::Imm{1}}).SetType(T::U32);
    auto widened = as.ZeroExtend64(sum).SetType(T::U64);
    auto negated = as.Neg(widened).SetType(T::U64);
    as.SaveFlags(negated, ir::Flags::NZ);
    Store(as, negated);
    as.BranchOnlyFlags(negated, ir::Flags::CV);
    block->SetTerminal(ir::terminal::Condition{ir::Cond::CS,
        ir::terminal::ReturnToHost{}, ir::terminal::ReturnToHost{}});
    const auto plan = Plan::Build(block.get());
    REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
    for (auto value : {input, sum, widened, negated}) {
        REQUIRE(plan.Slot(value.Def())); REQUIRE_FALSE(plan.RequiresCanonicalHome(value.Def()));
    }
    REQUIRE(input.Type() == T::U64);
    REQUIRE(plan.Find(negated.Def())->use_positions == std::vector<u32>{4, 5, 6});
}
