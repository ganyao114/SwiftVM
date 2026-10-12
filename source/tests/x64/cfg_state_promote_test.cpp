#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <sys/mman.h>
#include <vector>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/cfg_state_promote.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/common/variant_util.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/frontend/arm64/state_ranges.h"
#include "translator/arm64/cpu.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
constexpr u32 extent = 4096;
constexpr u64 initial_flags = 0xabcde98776543219ULL;
constexpr u64 nzcv_mask = u64{15} << 28;
constexpr u32 Lane(u32 index) { return offsetof(swift::arm64::ThreadContext64, r) + index * 8; }
static_assert(sizeof(swift::arm64::ThreadContext64) < extent);

struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new(bytes.data()) backend::State{};
    Machine() {
        for (u32 i = 0; i < extent; ++i) state->uniform_buffer_begin[i] = u8(i * 37 + 13);
        state->host_cpu_flags = initial_flags;
    }
    ~Machine() { state->~State(); }
    void Put(u32 offset, u64 value, u32 size = 8) { std::memcpy(state->uniform_buffer_begin + offset, &value, size); }
    u64 Get(u32 offset) const { u64 value; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
};

backend::FunctionPlan Plan(bool root = true) {
    backend::FunctionPlan plan; plan.body = new ir::Block(ir::Location{0x1000});
    if (root) {
        ir::Assembler as{plan.body.get()}; auto* label = as.AppendInst(O::BindLabel);
        label->SetArg(0, ir::Value{label});
    }
    return plan;
}
ir::Inst* Label(ir::Assembler& as) {
    auto* label = as.AppendInst(O::BindLabel); label->SetArg(0, ir::Value{label}); return label;
}
ir::Value Imm(ir::Assembler& as, u64 value, T type = T::U64) { return as.LoadImm(ir::Imm{value}).SetType(type); }
ir::Value Load(ir::Assembler& as, u32 offset, T type = T::U64) { return as.LoadUniform(ir::Uniform{offset, type}).SetType(type); }
void Store(ir::Assembler& as, u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
ir::Value Binary(ir::Assembler& as, O op, ir::Value lhs, ir::Value rhs, T type = T::U64) {
    return ir::Value{as.AppendInst(op, lhs, ir::Operand{rhs})}.SetType(type);
}
void Finish(backend::FunctionPlan& plan, ir::Assembler& as) {
    as.SetLocation(ir::Lambda{ir::Imm{u64{0x1204}}});
    plan.body->SetTerminal(ir::terminal::ReturnToHost{});
}
auto Graph(backend::FunctionPlan& plan) { return x64::CfgRegisterPlan::Build(plan.body.get(), plan, {.physical_registers = {}}); }

// Exact instruction/argument identity checks also observe parameter-list
// contents. They deliberately omit use counts, which change after success.
std::vector<u64> InstructionImage(const ir::Inst& inst) {
    std::vector<u64> image{reinterpret_cast<u64>(&inst), u64(inst.GetOp()), u64(inst.ReturnType()), inst.Id()};
    for (u32 slot = 0; slot < ir::Inst::max_args; ++slot) {
        const auto& arg = inst.ArgAt(slot);
        const auto* bytes = reinterpret_cast<const u8*>(&arg);
        for (u32 byte = 0; byte < sizeof(ir::Arg); ++byte) image.push_back(bytes[byte]);
        auto argument = arg;
        if (argument.IsParams()) for (const auto& param : argument.Get<ir::Params>()) {
            image.push_back(param.data.IsValue() ? 1 : param.data.IsImm() ? 2 : 0);
            if (param.data.IsValue()) { image.push_back(reinterpret_cast<u64>(param.data.value.Def())); image.push_back(u64(param.data.value.Type())); }
            else if (param.data.IsImm()) { image.push_back(param.data.imm.Get()); image.push_back(u64(param.data.imm.GetType())); }
        }
        image.push_back(UINT64_MAX);
    }
    return image;
}
struct StoreImage { ir::Inst* inst; std::vector<u64> image; };
std::vector<StoreImage> Stores(backend::FunctionPlan& plan) {
    std::vector<StoreImage> stores;
    for (auto& inst : plan.body->GetInstList()) if (inst.GetOp() == O::StoreUniform) stores.push_back({&inst, InstructionImage(inst)});
    return stores;
}
void StoresUnchanged(backend::FunctionPlan& plan, const std::vector<StoreImage>& before) {
    const auto after = Stores(plan); REQUIRE(after.size() == before.size());
    for (u32 i = 0; i < before.size(); ++i) {
        REQUIRE(after[i].inst == before[i].inst);
        auto old_image = before[i].image, new_image = after[i].image;
        // Insertion/re-ID may change IDs but never the store or its operands.
        old_image[3] = new_image[3] = 0;
        REQUIRE(old_image == new_image);
    }
}
std::vector<u64> BodyImage(backend::FunctionPlan& plan) {
    std::vector<u64> image;
    for (const auto& inst : plan.body->GetInstList()) {
        const auto item = InstructionImage(inst); image.insert(image.end(), item.begin(), item.end());
    }
    std::vector<std::pair<u64, u64>> targets, locations;
    for (const auto& [source, target] : plan.targets) targets.emplace_back(reinterpret_cast<u64>(source), reinterpret_cast<u64>(target));
    for (const auto& [label, location] : plan.locations) locations.emplace_back(reinterpret_cast<u64>(label), location.Value());
    for (auto* map : {&targets, &locations}) {
        std::sort(map->begin(), map->end()); image.push_back(map->size());
        for (const auto& [first, second] : *map) { image.push_back(first); image.push_back(second); }
    }
    for (auto* set : {&plan.stops, &plan.halt_checks, &plan.synthetic_labels}) {
        std::vector<u64> keys; for (auto* inst : *set) keys.push_back(reinterpret_cast<u64>(inst));
        std::sort(keys.begin(), keys.end()); image.push_back(keys.size()); image.insert(image.end(), keys.begin(), keys.end());
    }
    image.push_back(plan.exits.size());
    return image;
}

thread_local backend::State* helper_state{};
thread_local u32 helper_calls{};
constexpr u64 helper_flags = 0xfedcba9812345679ULL;
u64 MutateState(u64 first, u64 second, u64) {
    ++helper_calls;
    std::memcpy(helper_state->uniform_buffer_begin + Lane(1), &first, 8);
    std::memcpy(helper_state->uniform_buffer_begin + Lane(2), &second, 8);
    helper_state->host_cpu_flags = helper_flags;
    asm volatile("xor %%r11d, %%r11d\n\tsub $1, %%r11d" ::: "r11", "cc");
    return first ^ second;
}
u64 SignalNoArguments() { ++helper_calls; helper_state->exit_request |= u64{1} << 63; return 0; }
thread_local const volatile u64* fault_address{};
u64 FaultNoArguments() { return *fault_address; }
void Run(x64::Translator& code, Machine& machine, HaltReason wanted = HaltReason::CallHost) {
    helper_state = machine.state; helper_calls = 0; const auto host_fp = _mm_getcsr();
    const auto reason = code.getCode<HaltReason (*)(backend::State*)>()(machine.state);
    helper_state = nullptr;
    REQUIRE(reason == wanted); REQUIRE(_mm_getcsr() == host_fp);
}
void FinalModel(Machine& model) {
    model.state->current_loc = ir::Location{0x1204}; model.state->prev_loc = ir::Location{0x1000};
    model.state->halt_reason = HaltReason::CallHost;
}
void Equal(const Machine& actual, const Machine& expected) {
    // Includes all architectural GPR/FP/vector/context bytes and untouched
    // uniform tail, PC/previous PC, packed flags and native recovery slots.
    REQUIRE(std::memcmp(actual.bytes.data(), expected.bytes.data(), actual.bytes.size()) == 0);
}
struct Compiled {
    Config config{.loc_start = 0, .loc_end = 0x10000, .uniform_buffer_size = extent, .stack_alignment = 16};
    x64::Translator baseline;
    x64::CfgStatePromoteStats stats;
    std::unique_ptr<x64::Translator> promoted;
    Compiled(backend::FunctionPlan& plan, std::span<const u32> lanes, bool flags,
             x64::CfgStatePromoteOptions options = {})
        : baseline(plan.body.get(), {}, &config, &plan, true, x64::ExecutionMode::Ordinary, true, flags) {
        baseline.Translate(); const auto stores = Stores(plan); const auto graph = Graph(plan); REQUIRE(graph.Enabled());
        stats = x64::PromoteCfgState(plan, extent, lanes, graph, options); REQUIRE(stats.accepted);
        StoresUnchanged(plan, stores); const auto rebuilt = Graph(plan); REQUIRE(rebuilt.Enabled()); REQUIRE(rebuilt.Validate());
        promoted = std::make_unique<x64::Translator>(plan.body.get(), x64::HostFeatures{}, &config, &plan, true, x64::ExecutionMode::Ordinary, true, flags);
        promoted->Translate();
    }
};
} // namespace

TEST_CASE("state loop Phi policy preserves readonly lanes while rejecting dirty recurrent state", "[x64][cfg][state-promotion][policy]") {
    for (bool policy : {false, true}) for (bool flags : {false, true}) {
        auto plan = Plan(); auto* root = &plan.body->GetInstList().front(); ir::Assembler as{plan.body.get()};
        Store(as, Lane(3), Load(as, Lane(1)));
        auto dirty_load = Load(as, Lane(2));
        Store(as, Lane(2), Binary(as, O::Add, dirty_load, Imm(as, 7)));
        auto next = Binary(as, O::Sub, Load(as, Lane(0)), Imm(as, 1)); Store(as, Lane(0), next);
        auto back = as.Goto(as.TestNotZero(next).SetType(T::U8)); plan.targets[back.Def()] = root;
        Label(as); Finish(plan, as);
        const std::array lanes{Lane(1), Lane(2)};
        Compiled code{plan, lanes, flags, {.reject_dirty_loop_phis = policy}};
        REQUIRE(code.stats.candidate_lanes == 2);
        REQUIRE(code.stats.rejected_dirty_loop_lanes == (policy ? 1 : 0));
        REQUIRE(code.stats.entry_seeds == (policy ? 1 : 2));
        REQUIRE(code.stats.phis_inserted == (policy ? 0 : 1));
        REQUIRE(dirty_load.Def()->GetOp() == (policy ? O::LoadUniform : O::BitExtract));
        for (u64 trips : {1, 2, 7}) {
            Machine before, actual, model;
            for (auto* machine : {&before, &actual, &model}) {
                machine->Put(Lane(0), trips); machine->Put(Lane(1), 0x8899aabbccddeeffULL); machine->Put(Lane(2), UINT64_MAX - 11);
            }
            model.Put(Lane(0), 0); model.Put(Lane(2), UINT64_MAX - 11 + trips * 7);
            model.Put(Lane(3), model.Get(Lane(1))); FinalModel(model);
            Run(code.baseline, before); Run(*code.promoted, actual); Equal(before, model); Equal(actual, model);
        }
    }
}

TEST_CASE("state loop Phi policy keeps dirty store-before-load forwarding without retained Phi", "[x64][cfg][state-promotion][policy]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); auto* root = &plan.body->GetInstList().front(); ir::Assembler as{plan.body.get()};
        Store(as, Lane(1), Imm(as, 0x123456789abcdef0ULL));
        Store(as, Lane(2), Load(as, Lane(1)));
        auto next = Binary(as, O::Sub, Load(as, Lane(0)), Imm(as, 1)); Store(as, Lane(0), next);
        auto back = as.Goto(as.TestNotZero(next).SetType(T::U8)); plan.targets[back.Def()] = root;
        Label(as); Finish(plan, as);
        const std::array lanes{Lane(1)};
        Compiled code{plan, lanes, flags, {.reject_dirty_loop_phis = true}};
        REQUIRE(code.stats.candidate_lanes == 1); REQUIRE(code.stats.rejected_dirty_loop_lanes == 0);
        REQUIRE(code.stats.entry_seeds == 0); REQUIRE(code.stats.phis_inserted == 0); REQUIRE(code.stats.loads_replaced == 1);
        for (u64 trips : {1, 2, 7}) {
            Machine before, actual, model;
            for (auto* machine : {&before, &actual, &model}) machine->Put(Lane(0), trips);
            model.Put(Lane(0), 0); model.Put(Lane(1), 0x123456789abcdef0ULL); model.Put(Lane(2), model.Get(Lane(1))); FinalModel(model);
            Run(code.baseline, before); Run(*code.promoted, actual); Equal(before, model); Equal(actual, model);
        }
    }
}

TEST_CASE("state loop Phi policy excludes irreducible and duplicate-edge dirty lanes transactionally", "[x64][cfg][state-promotion][policy][scc]") {
    for (bool irreducible : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()};
        auto entry = as.Goto(as.TestNotZero(Load(as, Lane(0))).SetType(T::U8));
        auto* first = Label(as);
        Store(as, Lane(1), Binary(as, O::Add, Load(as, Lane(1)), Imm(as, 7)));
        auto forward = as.Goto(as.TestNotZero(Load(as, Lane(2))).SetType(T::U8));
        if (irreducible) plan.stops.insert(forward.Def());
        auto* second = Label(as); Store(as, Lane(3), Load(as, Lane(1)));
        auto back = as.Goto(as.TestNotZero(Load(as, Lane(4))).SetType(T::U8));
        Label(as); Finish(plan, as);
        plan.targets[entry.Def()] = irreducible ? second : first;
        plan.targets[forward.Def()] = second; plan.targets[back.Def()] = first;
        const auto graph = Graph(plan); REQUIRE(graph.Enabled());
        u32 forward_edges{};
        for (const auto& edge : graph.Edges()) if (edge.source == forward.Def()) ++forward_edges;
        REQUIRE(forward_edges == (irreducible ? 1 : 2));
        const auto image = BodyImage(plan); const std::array lanes{Lane(1)};
        const auto stats = x64::PromoteCfgState(plan, extent, lanes, graph, {.reject_dirty_loop_phis = true});
        REQUIRE(stats.accepted); REQUIRE(stats.candidate_lanes == 1); REQUIRE(stats.rejected_dirty_loop_lanes == 1);
        REQUIRE(stats.entry_seeds == 0); REQUIRE(stats.phis_inserted == 0); REQUIRE(stats.loads_replaced == 0);
        REQUIRE(BodyImage(plan) == image); REQUIRE(graph.MatchesBody(plan.body.get())); REQUIRE(graph.Validate());
        REQUIRE(Graph(plan).Enabled());
    }
}

TEST_CASE("state loop Phi policy ignores host-entry pseudo-edge and unreachable dirty cycles", "[x64][cfg][state-promotion][policy][scc]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()};
    auto value = Load(as, Lane(1)); Store(as, Lane(2), value);
    auto exit = as.Goto(ir::BOOL{Imm(as, 1, T::U8)}); plan.stops.insert(exit.Def());
    auto* dead = Label(as); Store(as, Lane(1), Binary(as, O::Add, Load(as, Lane(1)), Imm(as, 7)));
    auto back = as.Goto(ir::BOOL{Imm(as, 1, T::U8)}); plan.targets[back.Def()] = dead; plan.stops.insert(back.Def());
    auto* end = Label(as); plan.targets[exit.Def()] = end; Finish(plan, as);
    const auto graph = Graph(plan); REQUIRE(graph.Enabled()); const std::array lanes{Lane(1)};
    const auto stats = x64::PromoteCfgState(plan, extent, lanes, graph, {.reject_dirty_loop_phis = true});
    REQUIRE(stats.accepted); REQUIRE(stats.candidate_lanes == 1); REQUIRE(stats.rejected_dirty_loop_lanes == 0);
    REQUIRE(stats.entry_seeds == 1); REQUIRE(stats.phis_inserted == 0); REQUIRE(stats.loads_replaced == 1);
    REQUIRE(Graph(plan).Enabled());
}

TEST_CASE("state promotion seeds real host entry and carries swapped lanes through a loop", "[x64][cfg][state-promotion][loop]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); auto* root = &plan.body->GetInstList().front(); ir::Assembler as{plan.body.get()};
        auto count = Load(as, Lane(0)), a = Load(as, Lane(1)), b = Load(as, Lane(2));
        Store(as, Lane(1), b); Store(as, Lane(2), a);
        auto next = Binary(as, O::Sub, count, Imm(as, 1)); Store(as, Lane(0), next); as.SaveFlags(next, ir::Flags::NZCV);
        auto back = as.Goto(as.TestNotZero(Load(as, Lane(0))).SetType(T::U8)); plan.targets[back.Def()] = root;
        Label(as); Store(as, Lane(3), Binary(as, O::Xor, Load(as, Lane(1)), Load(as, Lane(2)))); Finish(plan, as);
        const std::array lanes{Lane(0), Lane(1), Lane(2)}; Compiled code{plan, lanes, flags};
        REQUIRE(code.stats.entry_seeds > 0); REQUIRE(code.stats.phis_inserted >= 3); REQUIRE(code.stats.loads_replaced >= 3);
        REQUIRE(plan.targets.at(back.Def()) == root); REQUIRE(&plan.body->GetInstList().front() != root);
        for (u64 count_value : {1, 2, 3, 8, 31}) for (u64 seed : std::array<u64, 3>{0, UINT64_MAX, 0x8123456789abcdefULL}) {
            Machine before, actual, model;
            for (auto* machine : {&before, &actual, &model}) { machine->Put(Lane(0), count_value); machine->Put(Lane(1), seed); machine->Put(Lane(2), seed ^ 0x123456789abcdef0ULL); }
            const u64 first = count_value & 1 ? seed ^ 0x123456789abcdef0ULL : seed;
            const u64 second = count_value & 1 ? seed : seed ^ 0x123456789abcdef0ULL;
            model.Put(Lane(0), 0); model.Put(Lane(1), first); model.Put(Lane(2), second); model.Put(Lane(3), first ^ second);
            model.state->host_cpu_flags = (initial_flags & ~nzcv_mask) | 0x60000000; FinalModel(model);
            Run(code.baseline, before); Run(*code.promoted, actual); Equal(before, model); Equal(actual, model);
        }
    }
}

TEST_CASE("state promotion creates merge Phi for differing diamond state updates", "[x64][cfg][state-promotion][diamond]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto selector = Load(as, Lane(0));
        auto branch = as.Goto(as.TestNotZero(selector).SetType(T::U8));
        Store(as, Lane(1), Binary(as, O::Add, Load(as, Lane(1)), Imm(as, 17)));
        auto jump = as.Goto(ir::BOOL{Imm(as, 1, T::U8)}); plan.stops.insert(jump.Def());
        auto* alternative = Label(as); Store(as, Lane(1), Binary(as, O::Xor, Load(as, Lane(1)), Imm(as, 0x85)));
        auto* join = Label(as); plan.targets[branch.Def()] = alternative; plan.targets[jump.Def()] = join;
        Store(as, Lane(2), Binary(as, O::Add, Load(as, Lane(1)), Imm(as, 11))); Finish(plan, as);
        const std::array lanes{Lane(1)}; Compiled code{plan, lanes, flags};
        REQUIRE(code.stats.entry_seeds == 1); REQUIRE(code.stats.phis_inserted > 0); REQUIRE(code.stats.loads_replaced >= 3);
        for (u64 selector_value : {0, 1, 255}) for (u64 seed : std::array<u64, 3>{0, UINT64_MAX, 0x8123456789abcdefULL}) {
            Machine before, actual, model; for (auto* machine : {&before, &actual, &model}) { machine->Put(Lane(0), selector_value); machine->Put(Lane(1), seed); }
            const u64 chosen = selector_value ? seed ^ 0x85 : seed + 17;
            model.Put(Lane(1), chosen); model.Put(Lane(2), chosen + 11); FinalModel(model);
            Run(code.baseline, before); Run(*code.promoted, actual); Equal(before, model); Equal(actual, model);
        }
    }
}

TEST_CASE("state promotion preserves duplicate conditional and fallthrough edge occurrences", "[x64][cfg][state-promotion][edges]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()}; auto branch = as.Goto(as.TestNotZero(Load(as, Lane(0))).SetType(T::U8));
    auto* join = Label(as); plan.targets[branch.Def()] = join;
    Store(as, Lane(2), Binary(as, O::Add, Load(as, Lane(1)), Imm(as, 7))); Finish(plan, as);
    const std::array lanes{Lane(1)}; Compiled code{plan, lanes, true}; REQUIRE(code.stats.loads_replaced > 0);
    const auto graph = Graph(plan); u32 duplicate_edges{};
    for (const auto& edge : graph.Edges()) if (edge.source == branch.Def()) ++duplicate_edges;
    REQUIRE(duplicate_edges == 2);
    for (u64 condition : {0, 1}) {
        Machine before, actual, model; for (auto* machine : {&before, &actual, &model}) { machine->Put(Lane(0), condition); machine->Put(Lane(1), UINT64_MAX); }
        model.Put(Lane(2), 6); FinalModel(model); Run(code.baseline, before); Run(*code.promoted, actual); Equal(before, model); Equal(actual, model);
    }
}

TEST_CASE("state promotion respects ARM64 W zero-high writes and partial uniform stores", "[x64][cfg][state-promotion][width]") {
    for (bool frontend_w : {false, true}) for (u32 byte : {0u, 2u, 4u}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()};
        auto low = Load(as, Lane(1), T::U32); auto next = Binary(as, O::Add, low, Imm(as, 7, T::U32), T::U32);
        if (frontend_w) Store(as, Lane(1), as.ZeroExtend64(next).SetType(T::U64));
        else Store(as, Lane(1) + byte, next.SetCastType(T::U16));
        auto whole = Load(as, Lane(1)); Store(as, Lane(2), whole);
        Store(as, Lane(3), Load(as, Lane(1), T::U32).SetCastType(T::U64)); Finish(plan, as);
        const std::array lanes{Lane(1)}; Compiled code{plan, lanes, true};
        if (!frontend_w) REQUIRE(code.stats.partial_kills > 0);
        for (u64 seed : std::array<u64, 3>{0x88776655ffffffffULL, UINT64_MAX, 0x8123456789abcdefULL}) {
            Machine before, actual, model; for (auto* machine : {&before, &actual, &model}) machine->Put(Lane(1), seed);
            const u64 next_value = u32(seed) + u32{7};
            if (frontend_w) model.Put(Lane(1), u32(next_value)); else model.Put(Lane(1) + byte, next_value, 2);
            model.Put(Lane(2), model.Get(Lane(1))); model.Put(Lane(3), u32(model.Get(Lane(1)))); FinalModel(model);
            Run(code.baseline, before); Run(*code.promoted, actual); Equal(before, model); Equal(actual, model);
        }
    }
}

TEST_CASE("state promotion kills partial overlap facts across a join and preserves full vector context", "[x64][cfg][state-promotion][overlap]") {
    auto plan = Plan(); ir::Assembler as{plan.body.get()};
    auto skip = as.Goto(as.TestNotZero(Load(as, Lane(0))).SetType(T::U8));
    Store(as, Lane(1) + 4, Imm(as, 0xaabbccdd, T::U32));
    auto* join = Label(as); plan.targets[skip.Def()] = join;
    auto wide = Load(as, Lane(1)); Store(as, Lane(2), wide); Store(as, Lane(3), Load(as, Lane(1), T::U16).SetCastType(T::U64));
    const u32 vectors = offsetof(swift::arm64::ThreadContext64, v);
    Store(as, vectors + 32, Load(as, vectors, T::V128)); Finish(plan, as);
    const std::array lanes{Lane(1)}; Compiled code{plan, lanes, true}; REQUIRE(code.stats.partial_kills > 0);
    // A full reload after this join establishes availability; an unknown
    // predecessor cannot be replaced by a fabricated old entry snapshot.
    REQUIRE(wide.Def()->GetOp() == O::LoadUniform);
    for (u64 condition : {0, 1}) {
        Machine before, actual, model; for (auto* machine : {&before, &actual, &model}) { machine->Put(Lane(0), condition); machine->Put(Lane(1), 0x8877665544332211ULL); }
        if (!condition) model.Put(Lane(1) + 4, 0xaabbccdd, 4);
        model.Put(Lane(2), model.Get(Lane(1))); model.Put(Lane(3), model.Get(Lane(1)) & 65535);
        std::memcpy(model.state->uniform_buffer_begin + vectors + 32, model.state->uniform_buffer_begin + vectors, 16); FinalModel(model);
        Run(code.baseline, before); Run(*code.promoted, actual); Equal(before, model); Equal(actual, model);
    }
}

TEST_CASE("state promotion observes opaque helpers and a barrier on only one predecessor", "[x64][cfg][state-promotion][helper]") {
    for (bool conditional : {false, true}) for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()}; ir::Value skip;
        if (conditional) skip = as.Goto(as.TestNotZero(Load(as, Lane(0))).SetType(T::U8));
        as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&MutateState)}}, Imm(as, 0xabcdef9876543210ULL), Imm(as, 0x123456789abcdef0ULL), Imm(as, 0)).SetType(T::U64);
        if (conditional) plan.targets[skip.Def()] = Label(as);
        auto a = Load(as, Lane(1)), b = Load(as, Lane(2));
        Store(as, Lane(3), Binary(as, O::Xor, a, b)); Store(as, Lane(4), Load(as, Lane(1)));
        if (flags) Store(as, Lane(5), as.GetFlags(a, ir::Flags::NZCV).SetType(T::U64));
        Finish(plan, as); const std::array lanes{Lane(1), Lane(2)}; Compiled code{plan, lanes, flags};
        REQUIRE(code.stats.barriers > 0); REQUIRE(a.Def()->GetOp() == O::LoadUniform); REQUIRE(b.Def()->GetOp() == O::LoadUniform);
        for (u64 condition : {0, 1}) {
            Machine before, actual, model; for (auto* machine : {&before, &actual, &model}) { machine->Put(Lane(0), condition); machine->Put(Lane(1), 11); machine->Put(Lane(2), 22); }
            const bool called = !conditional || !condition;
            if (called) { model.Put(Lane(1), 0xabcdef9876543210ULL); model.Put(Lane(2), 0x123456789abcdef0ULL); model.state->host_cpu_flags = helper_flags; }
            model.Put(Lane(3), model.Get(Lane(1)) ^ model.Get(Lane(2))); model.Put(Lane(4), model.Get(Lane(1)));
            if (flags) model.Put(Lane(5), model.state->host_cpu_flags); FinalModel(model);
            Run(code.baseline, before); REQUIRE(helper_calls == u32(called)); Run(*code.promoted, actual); REQUIRE(helper_calls == u32(called));
            Equal(before, model); Equal(actual, model);
        }
    }
}

TEST_CASE("state promotion rejects invalid inputs without mutating instructions or metadata", "[x64][cfg][state-promotion][rejection]") {
    for (u32 shape = 0; shape < 7; ++shape) {
        auto plan = Plan(shape != 0); ir::Assembler as{plan.body.get()}; Store(as, Lane(2), Load(as, Lane(1))); Finish(plan, as);
        auto graph = Graph(plan); REQUIRE(graph.Enabled()); std::vector<u32> lanes{Lane(1)}; u32 size = extent;
        x64::CfgStatePromoteRejection wanted;
        if (shape == 0) wanted = x64::CfgStatePromoteRejection::InvalidRoot;
        else if (shape == 1) { lanes = {Lane(1), Lane(1)}; wanted = x64::CfgStatePromoteRejection::InvalidLanes; }
        else if (shape == 2) { lanes = {Lane(1) + 1}; wanted = x64::CfgStatePromoteRejection::InvalidLanes; }
        else if (shape == 3) { lanes = {extent}; wanted = x64::CfgStatePromoteRejection::InvalidLanes; }
        else if (shape == 4) { for (u32 i = 0; i < 65; ++i) lanes.push_back(i * 8); lanes.erase(lanes.begin()); wanted = x64::CfgStatePromoteRejection::Budget; }
        else if (shape == 5) { as.AppendInst(O::Nop); wanted = x64::CfgStatePromoteRejection::InvalidGraph; }
        else {
            auto other = Plan(); ir::Assembler other_as{other.body.get()}; Finish(other, other_as); graph = Graph(other);
            wanted = x64::CfgStatePromoteRejection::InvalidGraph;
        }
        const auto image = BodyImage(plan); const auto stores = Stores(plan);
        const auto stats = x64::PromoteCfgState(plan, size, lanes, graph);
        REQUIRE_FALSE(stats.accepted); REQUIRE(stats.rejection == wanted); REQUIRE(BodyImage(plan) == image); StoresUnchanged(plan, stores);
    }
}

TEST_CASE("state promotion retains committed stores when a no-argument helper requests a signal", "[x64][cfg][state-promotion][interrupt]") {
    for (bool flags : {false, true}) {
        auto plan = Plan(); ir::Assembler as{plan.body.get()};
        auto next = Binary(as, O::Add, Load(as, Lane(1)), Imm(as, 7)); Store(as, Lane(1), next);
        Store(as, Lane(2), Load(as, Lane(1))); as.SaveFlags(next, ir::Flags::NZCV);
        (void)as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&SignalNoArguments)}}).SetType(T::U64);
        Store(as, Lane(3), Load(as, Lane(1))); Finish(plan, as);
        const std::array lanes{Lane(1)}; Compiled code{plan, lanes, flags}; REQUIRE(code.stats.loads_replaced > 0);
        Machine before, actual, model;
        for (auto* machine : {&before, &actual, &model}) machine->Put(Lane(1), UINT64_MAX);
        model.Put(Lane(1), 6); model.Put(Lane(2), 6);
        model.state->current_loc = model.state->prev_loc = ir::Location{0x1000};
        model.state->exit_request = u64{1} << 63; model.state->halt_reason = HaltReason::Signal;
        model.state->host_cpu_flags = (initial_flags & ~nzcv_mask) | 0x20000000;
        Run(code.baseline, before, HaltReason::Signal); REQUIRE(helper_calls == 1);
        Run(*code.promoted, actual, HaltReason::Signal); REQUIRE(helper_calls == 1);
        Equal(before, model); Equal(actual, model);
    }
}

TEST_CASE("production HIR counter uses frontend state lanes through CompileFunctions and Runtime", "[x64][cfg][state-promotion][production]") {
    for (bool cfg : {false, true}) for (bool flags : {false, true}) {
        Config config{.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64,
                      .uniform_buffer_size = extent, .static_program = true,
                      .global_opts = Optimizations::UniformElimination | Optimizations::ScalarRegisterCache, .stack_alignment = 16};
        config.loop_gpr_uniform_ranges = swift::arm64::Arm64GprUniformRanges();
        backend::AddressSpace space{config}; backend::ModuleConfig module_config; module_config.read_only = true;
        module_config.feature_overrides.Set(FeatureId::x64_cfg_allocation, cfg);
        module_config.feature_overrides.Set(FeatureId::x64_flags_residency, flags);
        auto module = space.MapModule(0x2000, 0x3000, module_config); Runtime runtime{&space};
        {
            ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0x2000});
            auto a = function->LoadUniform<ir::U64>(ir::Uniform{Lane(0), T::U64});
            auto b = function->LoadUniform<ir::U64>(ir::Uniform{Lane(1), T::U64});
            auto sum = function->Add<ir::U64>(a, ir::Operand{ir::Imm{u64{1}}});
            auto remaining = function->Sub<ir::U64>(b, ir::Operand{ir::Imm{u64{1}}});
            function->StoreUniform(ir::Uniform{Lane(0), T::U64}, sum); function->StoreUniform(ir::Uniform{Lane(1), T::U64}, remaining);
            function->SaveFlags(remaining, ir::Flags::NZCV); function->AdvancePC(ir::Imm{u32{12}});
            builder.If(ir::terminal::If{ir::BOOL{function->TestNotZero(remaining)},
                ir::terminal::LinkBlock{ir::Location{0x2000}}, ir::terminal::LinkBlock{ir::Location{0x2100}}});
            builder.SetCurBlock(ir::Location{0x2100});
            function->StoreUniform(ir::Uniform{Lane(2), T::U64}, function->LoadUniform<ir::U64>(ir::Uniform{Lane(0), T::U64}));
            function->StoreUniform(ir::Uniform{offsetof(swift::arm64::ThreadContext64, pc), T::U64},
                                   function->LoadImm<ir::U64>(ir::Imm{u64{0x2104}}));
            function->AdvancePC(ir::Imm{u32{4}});
            // A real retiring frontend instruction publishes its successor.
            // AdvancePC alone is lexical metadata and the separate-block
            // ReturnToHost path does not materialize that pending location.
            function->SetLocation(ir::Lambda{ir::Imm{u64{0x2104}}});
            function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
            const auto entry = x64::CompileFunctions(module, std::span{&function, 1}); REQUIRE(entry != nullptr);
            REQUIRE(space.GetCodeCache(ir::Location{0x2000}) == entry);
        }
        for (u64 trips : {1, 2, 3, 17}) for (u64 seed : std::array<u64, 3>{0, UINT64_MAX, 0x8123456789abcdefULL}) {
            CAPTURE(cfg, flags, trips, seed);
            Machine model; model.Put(Lane(0), seed); model.Put(Lane(1), trips);
            auto uniforms = runtime.GetUniformBuffer(); std::memcpy(uniforms.data(), model.state->uniform_buffer_begin, extent);
            runtime.GetState()->host_cpu_flags = initial_flags; runtime.SetLocation(0x2000);
            model.Put(Lane(0), seed + trips); model.Put(Lane(1), 0); model.Put(Lane(2), seed + trips);
            model.Put(offsetof(swift::arm64::ThreadContext64, pc), 0x2104);
            const auto host_fp = _mm_getcsr(); REQUIRE(runtime.Run() == HaltReason::CallHost); REQUIRE(_mm_getcsr() == host_fp);
            REQUIRE(std::memcmp(uniforms.data(), model.state->uniform_buffer_begin, extent) == 0);
            REQUIRE(runtime.GetLocation() == 0x2104);
            REQUIRE(runtime.GetState()->prev_loc.Value() == (cfg ? 0x200c : 0x2100));
            REQUIRE(runtime.GetState()->host_cpu_flags == ((initial_flags & ~nzcv_mask) | 0x60000000));
            REQUIRE(runtime.GetState()->halt_reason == HaltReason::None); REQUIRE(runtime.GetState()->exit_request == 0);
            REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
            REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
        }
    }
}

TEST_CASE("production state promotion retains stores before a no-argument helper native fault", "[x64][cfg][state-promotion][production][fault]") {
    struct Mapping { void* address = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ~Mapping() { if (address != MAP_FAILED) munmap(address, 4096); } } mapping;
    REQUIRE(mapping.address != MAP_FAILED); fault_address = static_cast<const volatile u64*>(mapping.address);
    for (bool flags : {false, true}) {
        Config config{.loc_start = 0, .loc_end = 0x10000, .enable_jit = true, .backend_isa = kX86_64,
                      .uniform_buffer_size = extent, .static_program = true,
                      .global_opts = Optimizations::UniformElimination | Optimizations::ScalarRegisterCache, .stack_alignment = 16};
        config.loop_gpr_uniform_ranges = swift::arm64::Arm64GprUniformRanges();
        backend::AddressSpace space{config}; backend::ModuleConfig module_config; module_config.read_only = true;
        module_config.feature_overrides.Set(FeatureId::x64_cfg_allocation, true); module_config.feature_overrides.Set(FeatureId::x64_flags_residency, flags);
        auto module = space.MapModule(0x2000, 0x3000, module_config); Runtime runtime{&space};
        {
            ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0x2000});
            const auto loaded = function->LoadUniform<ir::U64>(ir::Uniform{Lane(1), T::U64});
            const auto next = function->Add<ir::U64>(loaded, ir::Operand{ir::Imm{u64{7}}});
            function->StoreUniform(ir::Uniform{Lane(1), T::U64}, next);
            function->StoreUniform(ir::Uniform{Lane(2), T::U64}, function->LoadUniform<ir::U64>(ir::Uniform{Lane(1), T::U64}));
            function->SaveFlags(next, ir::Flags::NZCV); function->AdvancePC(ir::Imm{u32{4}});
            (void)function->CallLambda<ir::U64>(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&FaultNoArguments)}});
            function->StoreUniform(ir::Uniform{Lane(3), T::U64}, next);
            function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
            REQUIRE(x64::CompileFunctions(module, std::span{&function, 1}) != nullptr);
        }
        Machine model; model.Put(Lane(1), UINT64_MAX); auto uniforms = runtime.GetUniformBuffer();
        std::memcpy(uniforms.data(), model.state->uniform_buffer_begin, extent);
        runtime.GetState()->host_cpu_flags = initial_flags; runtime.SetLocation(0x2000);
        model.Put(Lane(1), 6); model.Put(Lane(2), 6);
        const auto host_fp = _mm_getcsr(); REQUIRE(runtime.Run() == HaltReason::PageFatal); REQUIRE(_mm_getcsr() == host_fp);
        REQUIRE(std::memcmp(uniforms.data(), model.state->uniform_buffer_begin, extent) == 0);
        REQUIRE(runtime.GetLocation() == 0x2004); REQUIRE(runtime.GetState()->prev_loc.Value() == 0x2000);
        REQUIRE(runtime.GetState()->host_cpu_flags == ((initial_flags & ~nzcv_mask) | 0x20000000));
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0); REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
    }
    fault_address = nullptr;
}
