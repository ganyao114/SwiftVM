#include "test_support.h"
#include "runtime/backend/loongarch64/jit/sparse_homes_plan.h"
#include "runtime/backend/loongarch64/jit/wide.h"

#include <algorithm>
#include <memory>
#include <unordered_set>

namespace swift::tests::loongarch64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
using I = ir::Inst;
using V = ir::Value;
using R = la::SparseHomeReason;

struct Builder {
    ir::Block block{ir::Location{0x1000}};
    Builder() { block.SetTerminal(ir::terminal::ReturnToHost{}); }
    template<class... Args> I* Add(O op, T type, const Args&... args) {
        auto* inst = I::Create(op, args...);
        inst->SetReturn(type); block.AppendInstUnchecked(inst); return inst;
    }
    I* Constant(u64 value = 17, T type = T::U64) { return Add(O::LoadImm, type, ir::Imm{value}); }
    I* Store(I* value) { return Add(O::StoreUniform, T::VOID, ir::Uniform{0, value->ReturnType()}, V{value}); }
    void Finish() { block.ReIdInstr(); }
};

template<class F> void Reject(F&& action, const char* detail) {
    bool rejected{};
    try { action(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, detail);
}

la::ScalarAllocationPlan Allocate(Builder& b, const la::RegisterPlan::Targets& targets = {},
                                   const la::RegisterPlan::Stops& stops = {},
                                   const la::RegisterPlan::Exits& exits = {},
                                   const ir::Terminal* terminal = nullptr) {
    return la::ScalarAllocationPlan::Build(&b.block, la::RegisterPlan::Build(&b.block, targets, stops, {}, exits, terminal), targets);
}

void Exact(const la::SparseHomesPlan& plan, const std::vector<I*>& expected) {
    const la::LiveSet oracle{expected.begin(), expected.end()};
    Check(plan.RequiredSet() == oracle, "required homes equal independent emitter-access inventory");
    std::unordered_set<u16> ids;
    for (auto* inst : expected) {
        Check(plan.HasHome(inst), "audited access has a home");
        const auto id = plan.HomeId(inst);
        Check(id && *id != I::invalid_id && ids.insert(*id).second, "homes have unique non-sentinel IDs");
        Check(plan.HomeOffset(inst, 1) == u32(*id) * 16 + 8, "high-half home address uses compact layout");
    }
    Check(plan.HomeCount() == std::max(u32{1}, u32(oracle.size())), "compact layout retains one-home minimum");
}

void EmptyChainAndSpills() {
    Builder empty;
    const auto empty_plan = la::SparseHomesPlan::Build(&empty.block, {});
    Exact(empty_plan, {});
    Check(empty_plan.Stats().instructions == 0 && empty_plan.Stats().compact_bytes == 16, "empty frame still has recovery anchor");
    Reject([&] { (void)empty_plan.HomeOffset(nullptr); }, "empty/null home lookup cannot alias zero");
    Reject([&] { (void)la::SparseHomesPlan::Build(nullptr, {}); }, "null block rejected");

    Builder chain;
    auto* value = chain.Constant();
    for (u32 i = 0; i < 64; ++i) value = chain.Add(O::Add, T::U64, V{value}, ir::Operand{ir::Imm{u64{3}}});
    chain.Store(value); chain.Finish();
    // Non-dense public IDs are deliberately preserved by the offline plan.
    std::vector<std::pair<I*, u16>> original;
    for (auto& inst : chain.block.GetInstList()) {
        inst.SetId(static_cast<u16>(inst.Id() * 11 + 100)); original.emplace_back(&inst, inst.Id());
    }
    const auto allocation = Allocate(chain);
    Check(allocation.Stats().spilled_values == 0, "chain uses only fixed registers");
    const auto sparse = la::SparseHomesPlan::Build(&chain.block, allocation);
    Exact(sparse, {});
    Check(sparse.Stats().dense_bytes == 1056 && sparse.Stats().compact_bytes == 16, "64-add chain saves 1040 home bytes");
    for (const auto& [inst, id] : original) Check(inst->Id() == id, "public source IDs unchanged");
    Check(!sparse.HomeId(value), "register-only value has no compact ID");
    Reject([&] { (void)sparse.HomeOffset(value); }, "missing register-only home fails closed");

    Builder pressure;
    std::vector<I*> constants;
    for (u32 i = 0; i < 9; ++i) constants.push_back(pressure.Constant(i));
    for (auto* inst : constants) pressure.Store(inst);
    pressure.Finish();
    const auto colored = Allocate(pressure);
    std::vector<I*> spills;
    for (auto* inst : constants) if (!colored.Register(inst)) spills.push_back(inst);
    Check(spills.size() == 3, "pressure fixture has three physical spills");
    const auto homes = la::SparseHomesPlan::Build(&pressure.block, colored);
    Exact(homes, spills);
    Check(homes.Stats().spill_homes == 3, "only allocated spills need native scalar homes");
    Exact(la::SparseHomesPlan::Build(&pressure.block, {}), constants);
}

void NativeMemoryAndColdHelpers() {
    Builder memory;
    auto* address = memory.Constant(128);
    auto* loaded = memory.Add(O::LoadMemory, T::U64, ir::Operand{V{address}});
    auto* stored = memory.Add(O::StoreMemory, T::VOID, ir::Operand{V{address}}, V{loaded});
    auto* ordered = memory.Add(O::LoadMemoryTSO, T::U64, ir::Operand{ir::Imm{u64{160}}});
    auto* ordered_store = memory.Add(O::StoreMemoryTSO, T::VOID, ir::Operand{ir::Imm{u64{192}}}, V{ordered});
    memory.Finish();
    Check(!la::SparseHomesPlan::MayCallTypedHelper(loaded) && !la::SparseHomesPlan::MayCallTypedHelper(stored) &&
          !la::SparseHomesPlan::MayCallTypedHelper(ordered_store), "native address callbacks do not need SSA homes");
    Exact(la::SparseHomesPlan::Build(&memory.block, Allocate(memory)), {});

    for (O op : {O::AtomicExchange, O::AtomicFetchAdd, O::AtomicRMW, O::CompareAndSwap}) {
        Builder b;
        auto* pointer = b.Constant(128), *left = b.Constant(3), *right = b.Constant(5);
        I* result;
        if (op == O::AtomicRMW) result = b.Add(op, T::U64, ir::Imm{u32{0}}, V{pointer}, V{left}, V{right});
        else if (op == O::CompareAndSwap) result = b.Add(op, T::U64, V{pointer}, V{left}, V{right});
        else result = b.Add(op, T::U64, V{pointer}, V{left});
        b.Store(result); b.Finish();
        const auto plan = la::SparseHomesPlan::Build(&b.block, Allocate(b));
        Exact(plan, op == O::AtomicRMW || op == O::CompareAndSwap ? std::vector<I*>{pointer, left, right, result} :
                                                                std::vector<I*>{pointer, left, result});
        Check(la::SparseHomesPlan::MayCallTypedHelper(result), "alignment/feature atomic fallback retains typed homes");
    }

    Builder fp;
    auto* lhs = fp.Constant(0x3ff0000000000000ULL), *rhs = fp.Constant(0x4000000000000000ULL);
    auto* sum = fp.Add(O::VecFAddScalar64, T::U64, V{lhs}, V{rhs}); fp.Store(sum); fp.Finish();
    Exact(la::SparseHomesPlan::Build(&fp.block, Allocate(fp)), {lhs, rhs, sum});

    Builder call;
    auto* target = call.Constant(0x2000), *argument = call.Constant(71);
    auto* invoke = call.Add(O::CallLambda, T::VOID, ir::Lambda{V{target}}, V{argument});
    ir::Params args; args.Push(V{argument}); args.Push(ir::Imm{u64{7}});
    auto* location = call.Add(O::CallLocation, T::U64, ir::Lambda{V{target}}, args);
    call.Store(location); call.Finish();
    const auto call_plan = la::SparseHomesPlan::Build(&call.block, Allocate(call));
    Exact(call_plan, {target, argument, invoke, location});
    Check(call_plan.Reasons(invoke) & u32(R::HelperInstruction), "VOID typed helper retains its own home");

    Builder unknown;
    auto* unaudited = unknown.Add(O::Void, T::VOID); unknown.Finish();
    Check(la::SparseHomesPlan::MayCallTypedHelper(unaudited), "unaudited opcode defaults to typed helper");
    Exact(la::SparseHomesPlan::Build(&unknown.block, {}), {unaudited});
    Reject([&] { (void)la::SparseHomesPlan::MayCallTypedHelper(nullptr); }, "null helper classifier rejected");
}

void EarlyPseudos() {
    for (O pseudo_op : {O::VecSatMulQC, O::VecSatShiftQC, O::Div128Remainder, O::CpuidUpper}) {
        Builder b;
        const bool qc = pseudo_op == O::VecSatMulQC || pseudo_op == O::VecSatShiftQC;
        auto* input = qc ? b.Add(O::Zero, T::V128) : b.Constant();
        I* producer;
        if (pseudo_op == O::VecSatMulQC)
            producer = b.Add(O::VecSatMulHigh, T::V128, V{input}, V{input}, V{input}, ir::Imm{u32{32}}, ir::Imm{u32{0}});
        else if (pseudo_op == O::VecSatShiftQC)
            producer = b.Add(O::VecSatShiftImm, T::V128, V{input}, ir::Imm{u32{1}}, ir::Imm{u32{32}}, ir::Imm{u32{0}});
        else if (pseudo_op == O::Div128Remainder)
            producer = b.Add(O::Div128, T::U64, V{input}, V{input}, V{input}, ir::Imm{u32{0}});
        else producer = b.Add(O::Cpuid, T::U64, V{input}, V{input}, ir::Imm{u32{0}}, ir::Imm{u32{0}});
        auto* pseudo = I::Create(pseudo_op, V{producer}); pseudo->SetReturn(qc ? T::U8 : T::U64);
        b.Add(O::Nop, T::VOID); b.block.AppendInstUnchecked(pseudo); b.Store(pseudo); b.Finish();
        const auto allocation = Allocate(b);
        Check(allocation.Definitions(producer).contains(pseudo), "producer defines delayed pseudo before its marker");
        const auto plan = la::SparseHomesPlan::Build(&b.block, allocation);
        Exact(plan, {input, producer, pseudo});
        Check(plan.Reasons(pseudo) & u32(R::ProducerPseudo), "producer-written QC/pair output has compact home");
        Check(plan.HomeOffset(pseudo) != plan.HomeOffset(producer), "early pair result cannot alias producer");
    }
}

void RawContainersAndPhi() {
    Builder raw;
    auto* vector = raw.Add(O::LoadUniform, T::V128, ir::Uniform{0, T::V128});
    auto* narrow = raw.Add(O::BitCast, T::U8, V{vector});
    auto* copy = raw.Add(O::GetResult, T::U32, V{narrow});
    auto* predicate = raw.Constant(1, T::U8), *other = raw.Constant(0xabcdef1234567890ULL);
    auto* selected = raw.Add(O::Select, T::V8, ir::BOOL{predicate}, V{copy}, V{other});
    raw.Store(selected); raw.Finish();
    const auto allocation = Allocate(raw);
    const auto plan = la::SparseHomesPlan::Build(&raw.block, allocation);
    Exact(plan, {vector, narrow, copy, other, selected});
    Check(!plan.HasHome(predicate), "raw select predicate remains purely in a register");
    std::vector<u64> storage(plan.HomeCount() * 2);
    std::array<u64, la::ScalarAllocationPlan::kRegisterCount> registers{};
    const auto read = [&](I* inst, u32 part) {
        if (!part && allocation.Register(inst)) return registers[*allocation.Register(inst)];
        return storage.at(plan.HomeOffset(inst, part) / 8);
    };
    const auto write = [&](I* inst, u32 part, u64 data) {
        if (!part && allocation.Register(inst)) registers[*allocation.Register(inst)] = data;
        else storage.at(plan.HomeOffset(inst, part) / 8) = data;
    };
    constexpr u64 lo = 0x0123456789abcdefULL, hi = 0xfedcba9876543210ULL;
    write(vector, 0, lo); write(vector, 1, hi);
    for (auto* carrier : {narrow, copy, selected}) {
        auto* source = carrier == narrow ? vector : carrier == copy ? narrow : copy;
        const auto low = read(source, 0), high = read(source, 1);
        write(carrier, 0, low); write(carrier, 1, high);
    }
    Check(read(selected, 0) == lo && read(selected, 1) == hi, "raw narrow carrier retains complete 128-bit payload");
    Reject([&] { (void)plan.HomeOffset(narrow, 2); }, "third raw half rejected");

    Builder phi;
    auto* condition = phi.Constant(1, T::U8);
    auto* label = new I(O::BindLabel); label->SetArgs(V{label}); label->SetReturn(T::VOID); phi.block.AppendInstUnchecked(label);
    auto* x = new I(O::AddPhi), *y = new I(O::AddPhi); x->SetReturn(T::U64); y->SetReturn(T::U64);
    ir::Params xp; xp.Push(ir::Imm{u64{31}}); xp.Push(V{y}); x->SetArgs(xp);
    ir::Params yp; yp.Push(ir::Imm{u64{47}}); yp.Push(V{x}); y->SetArgs(yp);
    phi.block.AppendInstUnchecked(x); phi.block.AppendInstUnchecked(y); phi.Store(x); phi.Store(y);
    auto* back = phi.Add(O::Goto, T::VOID, V{condition}); phi.Finish();
    const la::RegisterPlan::Targets targets{{back, label}};
    const auto phi_allocation = Allocate(phi, targets, {back});
    const auto phi_plan = la::SparseHomesPlan::Build(&phi.block, phi_allocation);
    Exact(phi_plan, {x, y});
    Check(phi_plan.Stats().phi_homes == 2 && !phi_plan.HasHome(condition), "Phi destinations retain raw high stores without homes for condition");
    std::array<u64, la::ScalarAllocationPlan::kRegisterCount> physical{};
    physical[*phi_allocation.Register(x)] = 31; physical[*phi_allocation.Register(y)] = 47;
    std::vector<u64> phi_storage(phi_plan.HomeCount() * 2, UINT64_MAX);
    for (u32 iteration = 0; iteration < 20; ++iteration) {
        const auto staged_x = physical[*phi_allocation.Register(y)], staged_y = physical[*phi_allocation.Register(x)];
        physical[*phi_allocation.Register(x)] = staged_x; physical[*phi_allocation.Register(y)] = staged_y;
        phi_storage.at(phi_plan.HomeOffset(x, 1) / 8) = 0; phi_storage.at(phi_plan.HomeOffset(y, 1) / 8) = 0;
        Check(physical[*phi_allocation.Register(x)] == (iteration & 1 ? 31 : 47), "compact Phi parallel copy preserves cycle");
        Check(phi_storage.at(phi_plan.HomeOffset(x, 1) / 8) == 0 && phi_storage.at(phi_plan.HomeOffset(y, 1) / 8) == 0,
              "compact scalar Phi retains both required high-half stores");
    }
}

void TerminalsAndRejects() {
    Builder b;
    auto* predicate = b.Constant(1, T::U8), *selector = b.Constant(7);
    auto* marker = b.Add(O::Nop, T::VOID); b.Finish();
    const ir::Terminal terminal = ir::terminal::If{ir::BOOL{predicate},
            ir::terminal::CheckHalt{ir::terminal::Switch{V{selector}, {{ir::Imm{u64{7}}, ir::terminal::ReturnToHost{}}}}},
            ir::terminal::Condition{ir::Cond::EQ, ir::terminal::ReturnToDispatch{}, ir::terminal::ReturnToHost{}}};
    const la::RegisterPlan::Exits exits{{marker, terminal}};
    const auto allocated = Allocate(b, {}, {}, exits, &terminal);
    Exact(la::SparseHomesPlan::Build(&b.block, allocated, exits, &terminal), {});

    Builder foreign;
    auto* outside = foreign.Constant(); foreign.Finish();
    const ir::Terminal bad = ir::terminal::Switch{V{outside}, {}};
    Reject([&] { (void)la::SparseHomesPlan::Build(&b.block, allocated, {}, &bad); }, "foreign terminal operand rejected");
    Reject([&] { (void)la::SparseHomesPlan::Build(&b.block, allocated, {{outside, terminal}}); }, "foreign function exit anchor rejected");
    Builder operand;
    operand.Add(O::BitCast, T::U8, V{outside}); operand.Finish();
    Reject([&] { (void)la::SparseHomesPlan::Build(&operand.block, {}); }, "foreign raw/helper operand rejected");
    Builder producer;
    auto* primary = producer.Add(O::Zero, T::U64);
    // Only a producer-linked pseudo outside the lexical body remains to
    // detect. Its owner outlives the inventory call.
    auto secondary = std::unique_ptr<I>(I::Create(O::Div128Remainder, V{primary})); secondary->SetReturn(T::U64);
    Reject([&] { (void)la::SparseHomesPlan::Build(&producer.block, {}); }, "non-lexical producer pseudo rejected");

    Builder wide;
    auto* input = wide.Add(O::LoadUniform, T::V256, ir::Uniform{0, T::V256}); wide.Store(input); wide.Finish();
    Reject([&] { (void)la::SparseHomesPlan::Build(&wide.block, {}); }, "unlegalized V256 homes rejected");
    Builder local;
    local.Add(O::DefineLocal, T::VOID, ir::Local{0, T::V256});
    Reject([&] { (void)la::SparseHomesPlan::Build(&local.block, {}); }, "V256 local metadata rejected before compact layout");
    const auto lowered = la::WidePlan::Build(&wide.block);
    const auto sparse = la::SparseHomesPlan::Build(lowered.body.get(), {});
    Check(sparse.Stats().required_homes >= 2 && sparse.Stats().vector_homes >= 2, "legalized V256 body has independent 128-bit homes");
    for (auto& inst : lowered.body->GetInstList()) if (ir::IsFloatValueType(inst.ReturnType()))
        Check(sparse.HasHome(&inst) && inst.ReturnType() != T::V256, "all legalized vector halves retain a home");
}

void IDBoundary() {
    Builder b;
    I* last{};
    for (u32 i = 0; i < I::invalid_id; ++i) {
        last = b.Add(O::Zero, T::V128);
        last->SetId(static_cast<u16>(i ^ 0x3456));
    }
    const auto plan = la::SparseHomesPlan::Build(&b.block, {});
    Check(plan.HomeCount() == I::invalid_id && plan.HomeId(last) == I::invalid_id - 1, "maximum 16-bit home layout excludes sentinel");
    Check(plan.HomeOffset(last, 1) == (u32(I::invalid_id) - 1) * 16 + 8, "maximum home offset does not wrap");
    u32 index{};
    for (auto& inst : b.block.GetInstList()) {
        Check(plan.HomeId(&inst) == index, "maximum layout IDs remain consecutive");
        Check(inst.Id() == static_cast<u16>(index ^ 0x3456), "maximum layout does not rewrite source IDs");
        ++index;
    }
    b.Add(O::Zero, T::V128);
    Reject([&] { (void)la::SparseHomesPlan::Build(&b.block, {}); }, "65536th required home fails closed");
}
}  // namespace

void SparseHomesPlan() {
    const auto run = [](const char* fixture, auto test) {
        try { test(); }
        catch (const std::exception& error) { throw std::runtime_error(std::string(fixture) + ": " + error.what()); }
    };
    run("empty/chain/spills", EmptyChainAndSpills);
    run("native memory/cold helpers", NativeMemoryAndColdHelpers);
    run("early pseudos", EarlyPseudos);
    run("raw containers/Phi", RawContainersAndPhi);
    run("terminals/rejects/wide", TerminalsAndRejects);
    run("16-bit ID boundary", IDBoundary);
}
}  // namespace swift::tests::loongarch64
