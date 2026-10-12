#include <array>
#include <cstring>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "runtime/backend/function_plan.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;
using O = ir::OpCode;
namespace {
using BlockFn = HaltReason (*)(backend::State*);
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 4096> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    ~Storage() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const { u64 value; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
};
IntrusivePtr<ir::Block> NewBlock() { return new ir::Block(ir::Location{0x1000}); }
u64 Mask(T type) { const auto bits = ir::GetValueSizeByte(type) * 8; return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
void Store(ir::Assembler& as, u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
ir::Value Binary(ir::Assembler& as, O op, ir::Value value, u64 immediate, T type) {
    return ir::Value{as.AppendInst(op, value, ir::Operand{ir::Imm{immediate}})}.SetType(type);
}
void MoveBefore(ir::Block* block, ir::Inst* moved, ir::Inst* before) {
    block->RemoveInst(moved); block->InsertBefore(moved, before); block->ReIdInstr();
}
struct Compiled {
    x64::Translator translator;
    Compiled(backend::FunctionPlan& plan, bool cache) : translator(plan.body.get(), {}, nullptr, &plan, cache) { translator.Translate(); }
    HaltReason Run(Storage& storage) { return translator.getCode<BlockFn>()(storage.state); }
};
void CheckPlans(const Compiled& off, const Compiled& on) {
    REQUIRE(on.translator.ScalarPlan().Enabled());
    REQUIRE(on.translator.ScalarPlan().ValidateNonOverlapping());
    REQUIRE(on.translator.FrameSize() == off.translator.FrameSize());
    REQUIRE(off.translator.Statistics().scalar_cache_read_sites == 0);
    REQUIRE(on.translator.Statistics().scalar_cache_read_sites > 0);
    REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites > 0);
}
void HomeOnly(const x64::Translator& translator, ir::Value value) {
    REQUIRE_FALSE(translator.ScalarPlan().Slot(value.Def()));
    REQUIRE(translator.ScalarPlan().RequiresCanonicalHome(value.Def()));
}
u64 Clobber(u64 a, u64 b, u64 c) {
    asm volatile("xor %%r8, %%r8\n\txor %%r9, %%r9\n\txor %%r10, %%r10\n\txor %%r11, %%r11\n\t"
                 "pxor %%xmm0, %%xmm0\n\tpxor %%xmm1, %%xmm1" ::: "r8", "r9", "r10", "r11", "xmm0", "xmm1", "cc");
    return a * 3 + b * 5 + c * 7;
}
}

TEST_CASE("x64 validated function cache excludes critical edge Phi copies and retains local chains", "[x64][scalar-function][phi]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool invert : {false, true}) {
        backend::FunctionPlan plan; plan.body = NewBlock(); ir::Assembler as{plan.body.get()};
        auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        auto input = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        auto first = Binary(as, O::Add, input, 17, type);
        auto initial = Binary(as, O::Xor, first, 0x39, type);
        auto initial_use = Binary(as, O::Add, initial, 3, type); Store(as, 64, initial_use);
        auto conditional = invert ? as.NotGoto(ir::BOOL{condition}) : as.Goto(ir::BOOL{condition});
        auto other = as.LoadUniform(ir::Uniform{16, type}).SetType(type);
        auto added = Binary(as, O::Sub, other, 31, type);
        auto updated = Binary(as, O::Xor, added, 0x85, type);
        auto updated_use = Binary(as, O::Add, updated, 5, type); Store(as, 72, updated_use);
        auto jump = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
        as.BindLabel(jump); auto* label = &plan.body->GetInstList().back();
        ir::Params inputs; inputs.Push(initial); inputs.Push(updated);
        auto phi = as.AddPhi(inputs).SetType(type);
        auto joined = Binary(as, O::Add, phi, 11, type);
        auto result = Binary(as, O::Xor, joined, 0xa9, type); Store(as, 80, result);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        plan.targets[conditional.Def()] = label; plan.targets[jump.Def()] = label; plan.stops.insert(jump.Def());
        std::vector<ir::Inst*> identities;
        for (auto& inst : plan.body->GetInstList()) identities.push_back(&inst);
        Compiled off{plan, false}, on{plan, true}; CheckPlans(off, on);
        for (auto value : {condition, initial, updated, phi}) HomeOnly(on.translator, value);
        for (auto value : {input, first, other, added, joined, result}) REQUIRE(on.translator.ScalarPlan().Slot(value.Def()));
        REQUIRE(on.translator.ScalarPlan().Find(input.Def())->segment != on.translator.ScalarPlan().Find(other.Def())->segment);
        REQUIRE(on.translator.ScalarPlan().Find(other.Def())->segment != on.translator.ScalarPlan().Find(joined.Def())->segment);
        u32 position{}; for (auto& inst : plan.body->GetInstList()) REQUIRE(&inst == identities.at(position++));
        REQUIRE(plan.targets.size() == 2); REQUIRE(plan.stops.size() == 1); REQUIRE(plan.exits.empty());
        REQUIRE_FALSE(x64::ScalarRegisterPlan::Build(plan.body.get(), true).Enabled());
        REQUIRE_FALSE(x64::ScalarRegisterPlan::Build(plan.body.get()).Enabled());
        const auto mask = Mask(type);
        for (u64 seed : {u64{0}, u64{1}, mask, u64{0x8123456789abcdef}}) for (u64 control : {u64{0}, u64{1}})
        for (auto* code : {&off, &on}) {
            Storage storage; storage.Put(0, control); storage.Put(8, seed); storage.Put(16, ~seed);
            constexpr u64 flags = 0xdeadbeef87654321ULL; storage.state->host_cpu_flags = flags;
            const auto left = (((seed & mask) + 17) & mask) ^ 0x39;
            const auto right = ((((~seed) & mask) - 31) & mask) ^ 0x85;
            const bool taken = invert ? control == 0 : control != 0;
            REQUIRE(code->Run(storage) == HaltReason::CallHost);
            REQUIRE(storage.Get(64) == ((left + 3) & mask));
            REQUIRE(storage.Get(72) == (taken ? 0 : ((right + 5) & mask)));
            REQUIRE(storage.Get(80) == ((((taken ? left : right) + 11) & mask) ^ 0xa9));
            REQUIRE(storage.state->host_cpu_flags == flags);
        }
    }
}

TEST_CASE("x64 validated function cache preserves parallel Phi cycles and local writers across helpers", "[x64][scalar-function][loop]") {
    for (auto type : {T::U64, T::V8, T::V16, T::V32, T::V64, T::V128}) for (bool helper : {false, true}) for (bool fanout : {false, true}) {
        backend::FunctionPlan plan; plan.body = NewBlock(); ir::Assembler as{plan.body.get()};
        std::array<ir::Value, 3> initial, phis;
        for (u32 lane = 0; lane < 3; ++lane) {
            auto full = as.LoadUniform(ir::Uniform{lane * 16, T::V128}).SetType(T::V128);
            initial[lane] = as.BitCast(full).SetType(type);
        }
        auto count = as.LoadUniform(ir::Uniform{64, T::U64}).SetType(T::U64);
        for (auto& phi : phis) phi = as.AddPhi(ir::Params{}).SetType(type);
        auto n = as.AddPhi(ir::Params{}).SetType(T::U64);
        auto local = as.LoadUniform(ir::Uniform{80, T::U64}).SetType(T::U64);
        auto added = Binary(as, O::Add, local, 13, T::U64);
        if (helper) { auto returned = as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Clobber)}}, n, n, n).SetType(T::U64); Store(as, 96, returned); }
        auto result = Binary(as, O::Xor, added, 0x5a, T::U64); Store(as, 80, result);
        auto next = Binary(as, O::Sub, n, 1, T::U64);
        auto edge = as.Goto(as.TestNotZero(next)); as.BindLabel(edge);
        auto* header = &plan.body->GetInstList().back(); MoveBefore(plan.body.get(), header, phis.front().Def());
        for (u32 lane = 0; lane < 3; ++lane) {
            const u32 source = fanout ? (lane == 2 ? 0 : 1) : (lane + 1) % 3;
            ir::Params inputs; inputs.Push(initial[lane]); inputs.Push(phis[source]); phis[lane].Def()->SetArg(0, inputs);
            auto full = as.BitCast(phis[lane]).SetType(type == T::U64 ? T::U64 : T::V128); Store(as, 128 + lane * 16, full);
        }
        ir::Params inputs; inputs.Push(count); inputs.Push(next); n.Def()->SetArg(0, inputs); Store(as, 192, next);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled off{plan, false}, on{plan, true}; CheckPlans(off, on);
        for (auto value : initial) HomeOnly(on.translator, value);
        for (auto value : phis) HomeOnly(on.translator, value);
        for (auto value : {count, n, next}) HomeOnly(on.translator, value);
        REQUIRE(on.translator.ScalarPlan().Slot(local.Def())); REQUIRE(on.translator.ScalarPlan().Slot(added.Def()));
        for (u64 iterations : {u64{1}, u64{2}, u64{5}, u64{17}}) for (auto* code : {&off, &on}) {
            Storage storage; storage.Put(64, iterations); storage.Put(80, UINT64_MAX - 3);
            for (u32 lane = 0; lane < 3; ++lane) for (u32 byte = 0; byte < 16; ++byte)
                storage.state->uniform_buffer_begin[lane * 16 + byte] = u8(0x93 + lane * 41 + byte * 7);
            u64 expected = UINT64_MAX - 3; for (u64 i = 0; i < iterations; ++i) expected = (expected + 13) ^ 0x5a;
            REQUIRE(code->Run(storage) == HaltReason::CallHost); REQUIRE(storage.Get(80) == expected); REQUIRE(storage.Get(192) == 0);
            REQUIRE(storage.Get(96) == (helper ? 15 : 0));
            std::array<u32, 3> sources{0, 1, 2};
            for (u64 i = 1; i < iterations; ++i) {
                const auto old = sources;
                sources = fanout ? std::array<u32, 3>{old[1], old[1], old[0]} : std::array<u32, 3>{old[1], old[2], old[0]};
            }
            for (u32 lane = 0; lane < 3; ++lane) for (u32 byte = 0; byte < (type == T::U64 ? 8u : 16u); ++byte) {
                const auto source = sources[lane];
                REQUIRE(storage.state->uniform_buffer_begin[128 + lane * 16 + byte] == u8(0x93 + source * 41 + byte * 7));
            }
        }
    }
}

TEST_CASE("x64 validated function cache isolates Nop halt and recursive metadata exits", "[x64][scalar-function][metadata]") {
    for (u32 shape = 0; shape < 4; ++shape) {
        backend::FunctionPlan plan; plan.body = NewBlock(); ir::Assembler as{plan.body.get()};
        auto cross = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto a = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        auto b = Binary(as, O::Add, a, 7, T::U64); Store(as, 64, b);
        // Raise the synchronous reason at the marker being tested, rather
        // than on entry (which now correctly stops before the prefix).
        // This single-owner store avoids an earlier post-helper stop.
        auto reason_address = as.LoadUniform(ir::Uniform{112, T::U64}).SetType(T::U64);
        auto owner_reason = as.LoadUniform(ir::Uniform{120, T::U32}).SetType(T::U32);
        as.StoreMemory(ir::Operand{reason_address}, owner_reason);
        auto* check = as.AppendInst(O::Nop); plan.halt_checks.insert(check);
        auto c = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        auto d = Binary(as, O::Add, c, 9, T::U64); Store(as, 72, d); Store(as, 80, cross);
        auto selector = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
        auto condition = as.TestNotZero(selector).SetType(T::U8);
        // Give both hidden selectors local uses too, to expose accidental allocation.
        auto local_use = Binary(as, O::Add, selector, 3, T::U64); Store(as, 88, local_use); Store(as, 96, condition);
        auto* exit = as.AppendInst(O::Nop); plan.stops.insert(exit);
        ir::Terminal terminal = ir::terminal::If{ir::BOOL{condition}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}};
        if (shape >= 1) terminal = ir::terminal::Switch{selector, {{ir::Imm{u64{1}}, terminal}, {ir::Imm{u64{7}}, ir::terminal::ReturnToHost{}}}};
        if (shape >= 2) terminal = ir::terminal::Condition{ir::Cond::AL, terminal, ir::terminal::ReturnToDispatch{}};
        if (shape >= 3) terminal = ir::terminal::CheckHalt{terminal};
        plan.exits.emplace(exit, terminal);
        auto suffix = as.LoadImm(ir::Imm{u64{0xdead}}).SetType(T::U64); Store(as, 104, suffix);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled off{plan, false}, on{plan, true}; CheckPlans(off, on);
        HomeOnly(on.translator, cross); HomeOnly(on.translator, condition);
        if (shape >= 1) HomeOnly(on.translator, selector);
        REQUIRE(on.translator.ScalarPlan().Find(a.Def())->segment != on.translator.ScalarPlan().Find(c.Def())->segment);
        for (u64 control : {u64{0}, u64{1}, u64{7}}) for (bool halt : {false, true}) for (auto* code : {&off, &on}) {
            Storage storage; storage.Put(0, 0x12345678); storage.Put(8, UINT64_MAX); storage.Put(16, 100); storage.Put(24, control);
            storage.Put(112, reinterpret_cast<u64>(&storage.state->halt_reason));
            storage.Put(120, static_cast<u32>(halt ? HaltReason::PageFatal : HaltReason::None));
            const auto reason = code->Run(storage);
            REQUIRE(reason == (halt ? HaltReason::PageFatal : (control ? HaltReason::CallHost : HaltReason::None)));
            REQUIRE(storage.Get(64) == 6); REQUIRE(storage.Get(72) == (halt ? 0 : 109));
            REQUIRE(storage.Get(80) == (halt ? 0 : 0x12345678)); REQUIRE(storage.Get(104) == 0);
        }
    }
}

TEST_CASE("x64 validated function cache retains strict validation of opaque label and Phi arguments", "[x64][scalar-function][validation]") {
    for (bool phi_extra : {false, true}) for (bool foreign : {false, true}) for (u32 shape = 0; shape < 3; ++shape) {
        backend::FunctionPlan plan; plan.body = NewBlock(); ir::Assembler as{plan.body.get()};
        auto other = NewBlock(); ir::Assembler other_as{other.get()}; auto external = other_as.LoadImm(ir::Imm{u64{7}}).SetType(T::U64);
        ir::Params inputs; inputs.Push(ir::Imm{u64{3}}); auto phi = as.AddPhi(inputs).SetType(T::U64);
        auto local = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64); auto result = Binary(as, O::Add, local, 9, T::U64); Store(as, 8, result);
        auto edge = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)}); as.BindLabel(edge); auto* label = &plan.body->GetInstList().back();
        auto later = as.LoadImm(ir::Imm{u64{17}}).SetType(T::U64); Store(as, 16, later); Store(as, 24, phi);
        auto* consumer = phi_extra ? phi.Def() : label;
        const auto opaque = foreign ? external : later;
        if (shape == 0) consumer->SetArg(4, opaque);
        else if (shape == 1) consumer->SetArg(4, ir::Lambda{opaque});
        else { ir::Params params; params.Push(opaque); consumer->SetArg(4, params); }
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        // The extra slot is not emitted, but it must not authorize a cache plan.
        x64::Translator translator{plan.body.get(), {}, nullptr, &plan, true};
        REQUIRE_FALSE(translator.ScalarPlan().Enabled());
        REQUIRE(translator.ScalarPlan().Rejection() == x64::ScalarPlanRejection::InvalidSSA);
    }
}

TEST_CASE("x64 validated function cache accepts physically later dominating producers only as homes", "[x64][scalar-function][dominance]") {
    backend::FunctionPlan plan; plan.body = NewBlock(); ir::Assembler as{plan.body.get()};
    auto entry = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)}); plan.stops.insert(entry.Def());
    auto local = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto added = Binary(as, O::Add, local, 17, T::U64);
    auto placeholder = as.LoadImm(ir::Imm{u64{0}}).SetType(T::U64);
    auto result = as.Add(added, ir::Operand{placeholder}).SetType(T::U64); Store(as, 64, result);
    auto* exit = as.AppendInst(O::Nop); plan.stops.insert(exit); plan.exits.emplace(exit, ir::terminal::ReturnToHost{});
    as.BindLabel(entry);
    auto source = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto source_add = Binary(as, O::Add, source, 13, T::U64);
    auto producer = Binary(as, O::Xor, source_add, 0x91, T::U64);
    auto back = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)}); plan.stops.insert(back.Def());
    as.BindLabel(back); auto* label = &plan.body->GetInstList().back(); MoveBefore(plan.body.get(), label, local.Def());
    result.Def()->SetArg(1, ir::Operand{producer});
    plan.body->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled off{plan, false}, on{plan, true}; CheckPlans(off, on);
    HomeOnly(on.translator, producer); REQUIRE(on.translator.ScalarPlan().Slot(source.Def()));
    REQUIRE(on.translator.ScalarPlan().Slot(local.Def()));
    REQUIRE_FALSE(x64::ScalarRegisterPlan::Build(plan.body.get()).Enabled());
    for (u64 input : {u64{0}, u64{1}, UINT64_MAX, u64{0x123456789abcdef0}}) for (auto* code : {&off, &on}) {
        Storage storage; storage.Put(0, input); storage.Put(8, ~input);
        REQUIRE(code->Run(storage) == HaltReason::CallHost);
        REQUIRE(storage.Get(64) == ((~input + 17) + ((input + 13) ^ 0x91)));
    }
}

TEST_CASE("x64 validated function cache handles immediate entry and duplicate edges without merging Phi inputs", "[x64][scalar-function][phi]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool entry_phi : {false, true}) for (bool invert : {false, true}) {
        backend::FunctionPlan plan; plan.body = NewBlock(); ir::Assembler as{plan.body.get()};
        ir::Params inputs;
        if (entry_phi) inputs.Push(ir::Imm{UINT64_MAX});
        else {
            auto control = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
            auto edge = invert ? as.NotGoto(ir::BOOL{control}) : as.Goto(ir::BOOL{control}); as.BindLabel(edge);
            // Distinct fallthrough and taken edges reach the same header.
            inputs.Push(ir::Imm{u64{73}}); inputs.Push(ir::Imm{u64{31}});
        }
        auto phi = as.AddPhi(inputs).SetType(type);
        auto local = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        auto first = Binary(as, O::Add, local, 7, type);
        auto last = as.Add(first, ir::Operand{phi}).SetType(type); Store(as, 64, last);
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled off{plan, false}, on{plan, true}; CheckPlans(off, on); HomeOnly(on.translator, phi);
        for (u64 control : {u64{0}, u64{1}}) for (u64 input : {u64{0}, Mask(type), u64{0x123456789abcdef0}}) for (auto* code : {&off, &on}) {
            Storage storage; storage.Put(0, control); storage.Put(8, input);
            const u64 selected = entry_phi ? Mask(type) : ((invert ? control == 0 : control != 0) ? 31 : 73);
            REQUIRE(code->Run(storage) == HaltReason::CallHost); REQUIRE(storage.Get(64) == ((input + 7 + selected) & Mask(type)));
        }
    }
}

TEST_CASE("x64 validated function cache never authorizes malformed Phi or foreign metadata", "[x64][scalar-function][validation]") {
    for (u32 malformed = 0; malformed < 5; ++malformed) {
        backend::FunctionPlan plan; plan.body = NewBlock(); ir::Assembler as{plan.body.get()};
        auto other = NewBlock(); ir::Assembler other_as{other.get()}; auto external = other_as.LoadImm(ir::Imm{u64{7}}).SetType(T::U64);
        auto value = as.LoadImm(ir::Imm{u64{13}}).SetType(T::U64);
        auto edge = as.Goto(ir::BOOL{as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8)}); as.BindLabel(edge);
        ir::Params inputs; if (malformed != 3) inputs.Push(malformed == 1 ? external : value); if (malformed != 0 && malformed != 3) inputs.Push(value);
        auto phi = as.AddPhi(inputs).SetType(T::U64); Store(as, 64, phi);
        if (malformed == 2) { plan.body->RemoveInst(value.Def()); plan.body->InsertAfter(value.Def(), phi.Def()); plan.body->ReIdInstr(); }
        if (malformed == 3) { edge.Def()->SetReturn(T::VOID); plan.exits.emplace(edge.Def(), ir::terminal::If{ir::BOOL{external}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToHost{}}); plan.stops.insert(edge.Def()); }
        if (malformed == 4) plan.halt_checks.insert(external.Def());
        plan.body->SetTerminal(ir::terminal::ReturnToHost{});
        if (malformed == 3) REQUIRE_THROWS_WITH(Compiled(plan, true), Catch::Matchers::ContainsSubstring("foreign SSA input"));
        else REQUIRE_THROWS_AS(Compiled(plan, true), std::runtime_error);
    }
}
