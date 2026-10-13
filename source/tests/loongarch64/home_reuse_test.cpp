#include "test_support.h"
#include "runtime/backend/loongarch64/jit/sparse_homes_plan.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace swift::tests::loongarch64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
using I = ir::Inst;
using V = ir::Value;
using P = la::SparseHomePayload;
using Set = std::unordered_set<I*>;

// Independent path-search oracle: ask whether a read is reachable before the
// next definition of ONE value. No RegisterPlan, allocation interference, or
// home-plan graph is used to decide expected lifetimes. Edge reads/writes are
// supplied by the fixture, including the actual parallel-phi protocol.
struct Oracle {
    struct Event { Set reads, writes; };
    struct Edge { I* to{}; Set reads, writes; };
    std::vector<I*> instructions;
    std::unordered_map<I*, Event> events;
    std::unordered_map<I*, std::vector<Edge>> outgoing;
    std::vector<std::pair<I*, I*>> coupled;

    bool Before(I* value, I* at) const {
        Set visited;
        const auto walk = [&](const auto& recurse, I* next) -> bool {
            if (!next || !visited.insert(next).second) return false;
            const auto& event = events.at(next);
            if (event.reads.contains(value)) return true;
            if (event.writes.contains(value)) return false;
            for (const auto& edge : outgoing.at(next)) {
                if (edge.reads.contains(value)) return true;
                if (!edge.writes.contains(value) && recurse(recurse, edge.to)) return true;
            }
            return false;
        };
        return walk(walk, at);
    }

    bool After(I* value, I* at) const {
        for (const auto& edge : outgoing.at(at)) {
            if (edge.reads.contains(value)) return true;
            if (!edge.writes.contains(value) && Before(value, edge.to)) return true;
        }
        return false;
    }

    void Record(I* inst) {
        instructions.push_back(inst);
        auto& event = events[inst];
        const auto op = inst->GetOp();
        const bool early = op == O::Div128Remainder || op == O::CpuidUpper ||
                           op == O::VecSatMulQC || op == O::VecSatShiftQC;
        if (early) { events[inst->GetArg<V>(0).Def()].writes.insert(inst); return; }
        if (op == O::AddPhi || op == O::BindLabel) return;
        if (inst->ReturnType() != T::VOID) event.writes.insert(inst);
        for (u32 slot = 0; slot < I::max_args; ++slot) {
            auto& arg = inst->ArgAt(slot);
            if (arg.IsValue()) event.reads.insert(arg.Get<V>().Def());
            else if (arg.IsLambda() && arg.Get<ir::Lambda>().IsValue()) event.reads.insert(arg.Get<ir::Lambda>().GetValue().Def());
            else if (arg.IsParams()) for (const auto& parameter : arg.Get<ir::Params>())
                if (parameter.data.IsValue()) event.reads.insert(parameter.data.value.Def());
        }
    }

    void Linear(const la::RegisterPlan::Stops& stops = {}) {
        for (u32 i = 0; i < instructions.size(); ++i) {
            outgoing.try_emplace(instructions[i]);
            if (i + 1 < instructions.size() && !stops.contains(instructions[i]))
                outgoing.at(instructions[i]).push_back({instructions[i + 1], {}, {}});
        }
    }

    void Branch(I* from, I* to) { outgoing.at(from).push_back({to, {}, {}}); }
    void Phi(I* entry, I* destination, const std::vector<std::pair<I*, I*>>& sources) {
        events.at(entry).writes.insert(destination);
        for (const auto& [from, source] : sources) {
            for (auto& edge : outgoing.at(from)) if (edge.to == entry) {
                if (source) edge.reads.insert(source);
                edge.writes.insert(destination);
            }
        }
    }

    void CheckPlan(const la::SparseHomesPlan& plan) const {
        std::vector<I*> values(plan.RequiredSet().begin(), plan.RequiredSet().end());
        const auto distinct = [&](Set simultaneous) {
            bool changed;
            do {
                changed = false;
                for (const auto& [left, right] : coupled) {
                    if (simultaneous.contains(left)) changed |= simultaneous.insert(right).second;
                    if (simultaneous.contains(right)) changed |= simultaneous.insert(left).second;
                }
            } while (changed);
            std::unordered_set<u16> slots;
            for (auto* value : simultaneous) if (auto slot = plan.HomeId(value))
                Check(slots.insert(*slot).second, "independent path oracle prohibits a live/write/edge home alias");
        };
        for (auto* value : values)
            Check(plan.HomeId(value) && *plan.HomeId(value) > 0 && *plan.HomeId(value) != I::invalid_id,
                  "reuse homes exclude recovery slot and invalid sentinel");
        for (auto* inst : instructions) {
            Set simultaneous = events.at(inst).writes;
            for (auto* value : values) if (Before(value, inst) || After(value, inst)) simultaneous.insert(value);
            distinct(std::move(simultaneous));
            for (const auto& edge : outgoing.at(inst)) {
                Set parallel = edge.reads;
                parallel.insert(edge.writes.begin(), edge.writes.end());
                for (auto* value : values) if (!edge.writes.contains(value) && Before(value, edge.to)) parallel.insert(value);
                distinct(std::move(parallel));
            }
        }
    }
};

struct Builder {
    ir::Block block{ir::Location{0x1000}};
    Oracle oracle;
    Builder() { block.SetTerminal(ir::terminal::ReturnToHost{}); }
    template<class... Args> I* Add(O op, T type, const Args&... args) {
        auto* inst = I::Create(op, args...);
        inst->SetReturn(type); block.AppendInstUnchecked(inst); oracle.Record(inst); return inst;
    }
    I* Label() {
        auto* label = new I(O::BindLabel);
        label->SetReturn(T::VOID); label->SetArgs(V{label});
        block.AppendInstUnchecked(label); oracle.Record(label); return label;
    }
    I* Phi() {
        auto* phi = new I(O::AddPhi); phi->SetReturn(T::U64);
        block.AppendInstUnchecked(phi); oracle.Record(phi); return phi;
    }
    I* Load(u32 offset, T type = T::U64) { return Add(O::LoadUniform, type, ir::Uniform{offset, type}); }
    I* Constant(u64 value) { return Add(O::LoadImm, T::U64, ir::Imm{value}); }
    I* Store(I* value, u32 offset = 0) { return Add(O::StoreUniform, T::VOID, ir::Uniform{offset, value->ReturnType()}, V{value}); }
    void Finish(const la::RegisterPlan::Stops& stops = {}) { block.ReIdInstr(); oracle.Linear(stops); }
    std::vector<u16> IDs() const {
        std::vector<u16> ids;
        for (const auto& inst : block.GetInstList()) ids.push_back(inst.Id());
        return ids;
    }
};

template<class F> void Reject(F&& action, const char* detail) {
    bool rejected{};
    try { action(); } catch (const std::exception&) { rejected = true; }
    Check(rejected, detail);
}

la::SparseHomesPlan Reuse(Builder& b, const la::RegisterPlan::Targets& targets = {},
                           const la::RegisterPlan::Stops& stops = {},
                           const la::RegisterPlan::Exits& exits = {},
                           std::span<const la::WideVectorPair> wide = {}) {
    return la::SparseHomesPlan::Build(&b.block, {}, exits, nullptr, true, targets, stops, wide);
}

void LinearLifetimeOracle() {
    Builder b;
    auto* keep = b.Load(64);
    std::vector<I*> dead;
    for (u32 i = 0; i < 32; ++i) {
        auto* input = b.Load(128 + i * 8);
        auto* result = b.Add(O::Add, T::U64, V{input}, ir::Operand{V{keep}});
        b.Store(result, 1024 + i * 8);
        dead.push_back(b.Load(512 + i * 8));
    }
    b.Store(keep); b.Finish();
    for (auto* inst : b.oracle.instructions) inst->SetId(static_cast<u16>(200 + inst->Id() * 7));
    const auto ids = b.IDs();
    const auto baseline = la::SparseHomesPlan::Build(&b.block, {});
    const auto plan = Reuse(b);
    b.oracle.CheckPlan(plan);
    Check(plan.Stats().reuse_active && plan.HomeCount() == 4, "long scalar waves color to three homes plus recovery slot");
    Check(plan.Stats().saved_bytes > 1024 && plan.HomeCount() < baseline.HomeCount(), "home reuse has a substantial static byte positive control");
    for (auto* value : dead) Check(plan.HomeId(value) != plan.HomeId(keep), "unused definition cannot overwrite retained live value");
    Check(b.IDs() == ids, "offline reuse never rewrites sparse public IDs");
    Reject([&] { (void)plan.HomeOffset(nullptr); }, "missing home cannot become recovery slot");
    Reject([&] { (void)plan.HomeOffset(keep, 2); }, "invalid raw half rejected");
    const auto repeat = Reuse(b);
    for (auto* value : plan.RequiredSet()) Check(plan.HomeId(value) == repeat.HomeId(value), "home coloring is deterministic");
}

void CFGAndPhiOracle() {
    Builder diamond;
    auto* keep = diamond.Load(64);
    auto* condition = diamond.Load(72, T::U8);
    auto* choose = diamond.Add(O::Goto, T::U8, ir::BOOL{condition});
    auto* left = diamond.Load(80);
    auto* to_join = diamond.Add(O::Goto, T::U8, ir::BOOL{condition});
    auto* right_label = diamond.Label();
    auto* right = diamond.Load(88);
    auto* join = diamond.Label();
    auto* phi = diamond.Phi();
    ir::Params inputs; inputs.Push(V{left}); inputs.Push(V{right}); phi->SetArgs(inputs);
    auto* sum = diamond.Add(O::Add, T::U64, V{phi}, ir::Operand{V{keep}}); diamond.Store(sum);
    diamond.Finish({to_join}); diamond.oracle.Branch(choose, right_label); diamond.oracle.Branch(to_join, join);
    diamond.oracle.Phi(join, phi, {{to_join, left}, {right, right}});
    const auto plan = Reuse(diamond, {{choose, right_label}, {to_join, join}}, {to_join});
    diamond.oracle.CheckPlan(plan);
    Check(plan.Stats().cfg_nodes == 4 && plan.HomeId(phi) != plan.HomeId(keep), "diamond retains cross-join value beside all phi writes");

    Builder loop;
    auto* entry = loop.Label();
    auto* x = loop.Phi(), *y = loop.Phi(), *n = loop.Phi();
    loop.Store(x); loop.Store(y, 8);
    auto* scratch = loop.Load(64);
    loop.Store(scratch, 24);
    auto* next = loop.Add(O::Sub, T::U64, V{n}, ir::Operand{ir::Imm{u64{1}}});
    auto* nonzero = loop.Add(O::TestNotZero, T::U8, V{next});
    auto* back = loop.Add(O::Goto, T::U8, ir::BOOL{nonzero});
    loop.Store(next, 16);
    ir::Params xp; xp.Push(ir::Imm{u64{31}}); xp.Push(V{y}); x->SetArgs(xp);
    ir::Params yp; yp.Push(ir::Imm{u64{47}}); yp.Push(V{x}); y->SetArgs(yp);
    ir::Params np; np.Push(ir::Imm{u64{6}}); np.Push(V{next}); n->SetArgs(np);
    loop.Finish(); loop.oracle.Branch(back, entry);
    loop.oracle.Phi(entry, x, {{back, y}}); loop.oracle.Phi(entry, y, {{back, x}}); loop.oracle.Phi(entry, n, {{back, next}});
    const auto cyclic = Reuse(loop, {{back, entry}});
    loop.oracle.CheckPlan(cyclic);
    std::unordered_set<u16> parallel;
    for (auto* value : {x, y, n, next}) Check(parallel.insert(*cyclic.HomeId(value)).second, "cyclic phi sources and simultaneous destinations have separate homes");

    // Two entries to A/B and a backedge B->A make a non-natural loop. The
    // path oracle follows all alternatives; a lexical interval is insufficient.
    Builder irreducible;
    auto* retained = irreducible.Load(64), *predicate = irreducible.Load(72, T::U8);
    auto* enter_b = irreducible.Add(O::Goto, T::U8, ir::BOOL{predicate});
    auto* a = irreducible.Label(); auto* tmp = irreducible.Load(80); irreducible.Store(tmp, 32);
    auto* a_to_b = irreducible.Add(O::Goto, T::U8, ir::BOOL{predicate});
    auto* bb = irreducible.Label(); auto* other = irreducible.Load(88); irreducible.Store(other, 40);
    auto* b_to_a = irreducible.Add(O::Goto, T::U8, ir::BOOL{predicate}); irreducible.Store(retained);
    irreducible.Finish(); irreducible.oracle.Branch(enter_b, bb); irreducible.oracle.Branch(a_to_b, bb); irreducible.oracle.Branch(b_to_a, a);
    const auto nonnatural = Reuse(irreducible, {{enter_b, bb}, {a_to_b, bb}, {b_to_a, a}});
    irreducible.oracle.CheckPlan(nonnatural);
    Check(nonnatural.HomeId(retained) != nonnatural.HomeId(tmp) && nonnatural.HomeId(retained) != nonnatural.HomeId(other),
          "retained home survives both irreducible-loop entries");
}

void PayloadPseudoAndWideOracle() {
    Builder b;
    auto* raw = b.Load(64, T::V128); b.Store(raw, 512);
    auto* scalar = b.Load(80);
    auto* copy = b.Add(O::BitCast, T::U8, V{scalar}); b.Store(copy, 528);
    auto* narrow_vector = b.Load(88, T::V8);
    auto* packed = b.Add(O::BitCast, T::V128, V{narrow_vector}); b.Store(packed, 544);
    auto* second_raw = b.Load(96, T::V128);
    auto* carrier = b.Add(O::BitCast, T::U8, V{second_raw});
    auto* restored = b.Add(O::BitCast, T::V128, V{carrier}); b.Store(restored, 560);
    auto* numerator = b.Load(112), *high = b.Load(120), *divisor = b.Load(128);
    auto* quotient = b.Add(O::Div128, T::U64, V{high}, V{numerator}, V{divisor}, ir::Imm{u32{0}});
    auto* remainder = b.Add(O::Div128Remainder, T::U64, V{quotient});
    // Move the no-op marker after intervening dead writes and its consumer.
    b.block.RemoveInst(remainder);
    b.oracle.instructions.erase(std::find(b.oracle.instructions.begin(), b.oracle.instructions.end(), remainder));
    auto* overwrite = b.Load(136);
    b.Store(remainder, 576); b.Store(quotient, 584);
    b.block.AppendInstUnchecked(remainder); b.oracle.instructions.push_back(remainder);
    b.Finish();
    const auto plan = Reuse(b); b.oracle.CheckPlan(plan);
    Check(plan.Payload(raw) == P::Full128 && plan.Payload(carrier) == P::Full128 && plan.Payload(copy) == P::Full128,
          "narrow raw-copy annotations retain full-payload pool");
    Check(plan.Payload(scalar) == P::ZeroHigh && plan.Payload(narrow_vector) == P::ZeroHigh,
          "low-only scalar and narrow vector loads retain zero-high pool");
    for (auto* zero_high : {scalar, narrow_vector}) for (auto* full : {raw, copy, carrier, restored})
        Check(plan.HomeId(zero_high) != plan.HomeId(full), "zero-high slots never inherit a full raw payload");
    Check(plan.HomeId(remainder) != plan.HomeId(overwrite) && plan.HomeId(remainder) != plan.HomeId(quotient),
          "producer-written delayed pseudo survives intervening writes");

    Builder wide;
    auto* ll = wide.Load(64, T::V128), *lh = wide.Load(80, T::V128);
    auto* rl = wide.Load(96, T::V128), *rh = wide.Load(112, T::V128);
    auto* low = wide.Add(O::VecXor, T::V128, V{ll}, V{rl});
    auto* upper = wide.Add(O::VecXor, T::V128, V{lh}, V{rh});
    auto* alive = wide.Load(128, T::V128); wide.Store(alive, 1024); wide.Store(low, 1040);
    wide.Finish();
    const std::array pairs{la::WideVectorPair{low, upper, {ll, lh}, {rl, rh}}};
    wide.oracle.coupled = {{low, upper}, {ll, lh}, {rl, rh}};
    wide.oracle.events.at(low).writes.insert(upper);
    const auto paired = Reuse(wide, {}, {}, {}, pairs); wide.oracle.CheckPlan(paired);
    Check(paired.Stats().paired_homes == 6 && paired.HomeId(upper) != paired.HomeId(alive),
          "dead LASX high half cannot alias another live container while its low half survives");
}

void SplitStagingOracle() {
    Builder b;
    std::vector<I*> input;
    for (u32 i = 0; i < 12; ++i) input.push_back(b.Load(64 + i * 8));
    auto* result = input.front();
    for (u32 phase = 0; phase < 3; ++phase) for (u32 round = 0; round < 24; ++round) {
        result = b.Add(O::Add, T::U64, V{result}, ir::Operand{V{input[phase * 4 + round % 4]}});
        result = b.Add(O::Xor, T::U64, V{result}, ir::Operand{V{input[phase * 4 + (round + 1) % 4]}});
    }
    for (u32 i = 0; i < input.size(); ++i) b.Store(input[i], 512 + i * 8);
    b.Store(result, 1024); b.Finish();
    const auto live = la::RegisterPlan::Build(&b.block);
    const auto split = la::ScalarAllocationPlan::Build(&b.block, live, {}, la::ScalarAllocationPlan::kRegisterCount, false, true);
    const auto plan = la::SparseHomesPlan::Build(&b.block, split, {}, nullptr, true);
    b.oracle.CheckPlan(plan);
    Check(split.Stats().split_transfer_stores + split.Stats().split_transfer_loads > 0 && plan.Stats().split_homes > 0,
          "rotating pressure creates actual live-range split staging homes");
    for (auto* at : b.oracle.instructions) {
        std::unordered_set<u16> staging;
        for (const auto& move : split.MovesBefore(at)) {
            Check(plan.HasHome(move.value) && bool(plan.Reasons(move.value) & u32(la::SparseHomeReason::Split)),
                  "every split transfer has an inventoried home");
            Check(staging.insert(*plan.HomeId(move.value)).second, "parallel split transfer stores use distinct staging homes");
            for (auto* retained : plan.RequiredSet())
                if (retained != move.value && b.oracle.Before(retained, at))
                    Check(plan.HomeId(retained) != plan.HomeId(move.value), "split staging cannot overwrite another incoming live home");
        }
    }
}

void TerminalsIsolationAndBounds() {
    Builder b;
    auto* value = b.Load(64), *predicate = b.Load(72, T::U8), *selector = b.Load(80);
    auto* dead = b.Load(88);
    auto* exit = b.Add(O::Nop, T::VOID);
    auto* after_exit = b.Load(96); b.Store(after_exit);
    const ir::Terminal terminal = ir::terminal::If{ir::BOOL{predicate},
            ir::terminal::Switch{V{selector}, {{ir::Imm{u64{3}}, ir::terminal::ReturnToHost{}}}},
            ir::terminal::ReturnToHost{}};
    b.Finish({exit});
    b.oracle.events.at(exit).reads.insert(predicate); b.oracle.events.at(exit).reads.insert(selector);
    const auto plan = Reuse(b, {}, {exit}, {{exit, terminal}}); b.oracle.CheckPlan(plan);
    Check(plan.HomeId(dead) != plan.HomeId(predicate) && plan.HomeId(dead) != plan.HomeId(selector),
          "dead home definitions preserve nested exit selectors");
    (void)value;

    Builder isolated;
    auto* a = isolated.Add(O::CallLambda, T::VOID, ir::Lambda{ir::Imm{u64{1}}});
    auto* c = isolated.Add(O::CallLambda, T::VOID, ir::Lambda{ir::Imm{u64{1}}});
    auto* zero = isolated.Add(O::Zero, T::V128); isolated.Store(zero); isolated.Finish();
    const auto private_homes = Reuse(isolated);
    Check(private_homes.Payload(a) == P::Isolated && private_homes.HomeId(a) != private_homes.HomeId(c) &&
          private_homes.HomeId(a) != private_homes.HomeId(zero), "VOID helper ownhomes stay private");

    auto foreign = std::unique_ptr<I>(I::Create(O::Zero)); foreign->SetReturn(T::U64);
    Reject([&] { (void)Reuse(b, {{exit, foreign.get()}}); }, "foreign target rejected before reuse analysis");
    Reject([&] { (void)Reuse(b, {}, {foreign.get()}); }, "foreign stop rejected before reuse analysis");
    Reject([&] { (void)Reuse(b, {{exit, exit}}); }, "nonbranch target source rejected");
    const std::array bad_pair{la::WideVectorPair{zero, zero, {zero, zero}, {zero, zero}}};
    Reject([&] { (void)Reuse(isolated, {}, {}, {}, bad_pair); }, "invalid LASX pairing rejected");

    Builder budget;
    for (u32 i = 0; i < 4097; ++i) budget.Add(O::Zero, T::V128);
    budget.Finish(); const auto ids = budget.IDs();
    const auto fallback = Reuse(budget);
    Check(fallback.Stats().budget_rejected && !fallback.Stats().reuse_active && fallback.HomeCount() == 4098,
          "analysis budget has a bounded nonzero unique-slot fallback");
    std::unordered_set<u16> slots;
    for (auto* inst : fallback.RequiredSet())
        Check(*fallback.HomeId(inst) > 0 && slots.insert(*fallback.HomeId(inst)).second, "budget fallback uses distinct nonzero homes");
    Check(budget.IDs() == ids && fallback.Stats().saved_bytes == -16, "fallback preserves IR and reports recovery-slot cost");

    Builder checkpoint;
    auto* scalar = checkpoint.Load(64), *vector = checkpoint.Load(80, T::V128);
    ir::Params snapshot; snapshot.Push(ir::Imm{u64{0}}); snapshot.Push(V{scalar});
    snapshot.Push(ir::Imm{u64{16}}); snapshot.Push(V{vector});
    auto* marker = checkpoint.Add(O::Checkpoint, T::VOID, snapshot); checkpoint.Finish();
    const auto allocation = la::ScalarAllocationPlan::Build(&checkpoint.block, la::RegisterPlan::Build(&checkpoint.block));
    const auto checkpoint_homes = la::SparseHomesPlan::Build(&checkpoint.block, allocation, {}, nullptr, true);
    Check(!la::SparseHomesPlan::MayCallTypedHelper(marker) && !checkpoint_homes.HasHome(scalar) &&
          checkpoint_homes.HasHome(vector), "GPR checkpoint snapshots use mapped scalars without forcing helper homes");
}

u64 callback_calls{}, callback_value{};
u64 Capture(u64 value) { ++callback_calls; callback_value = value; return ~value; }
ir::Lambda Probe() { return ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Capture)}}; }

struct ReuseCode {
    Config config{TestConfig()};
    la::JitContext context;
    la::JitTranslator translator{context};
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    BlockFn fn{};
    ReuseCode(Builder& b, bool reuse, bool split = false,
              la::HostFeatures features = la::HostFeatures::Detect(), bool vector_cfg = true,
              const la::RegisterPlan::Targets& targets = {})
            : context(true, features, {}, la::ScalarAllocationMode::GlobalCFG) {
        context.SetSparseHomes(true); context.SetHomeReuse(reuse);
        context.SetScalarSplitting(split); context.SetScalarRematerialization(false);
        context.SetUniformForwarding(false); context.SetVectorCFGAllocation(vector_cfg);
        context.ConfigureFlow(targets, {}, {});
        translator.Translate(&b.block);
        const auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate home-reuse executable"); context.Flush(*buffer);
        fn = reinterpret_cast<BlockFn>(buffer->exec_data);
        Check(context.HomeStats().reuse_requested == reuse, "requested home-reuse mode reaches emitted private plan");
    }
    void Run(StateStorage& state) { Check(CheckABI(fn, state.state) == 1, "home-reuse executable preserves host ABI"); }
};

void Initialize(StateStorage& state, u64 seed) {
    for (u32 offset = 0; offset < 4096; offset += 8)
        state.Put(offset, seed * 0x9e3779b97f4a7c15ULL ^ (u64(offset) * 0x18357a2b6c9de4f1ULL));
}

void SameUniform(const StateStorage& actual, const StateStorage& expected, const char* detail) {
    Check(std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 4096) == 0, detail);
}

void PressureWavesExecution() {
    Builder b;
    constexpr u32 waves = 12, width = 9;
    for (u32 wave = 0; wave < waves; ++wave) {
        std::vector<I*> inputs;
        for (u32 i = 0; i < width; ++i) inputs.push_back(b.Load(64 + (wave * width + i) * 8));
        auto* helper = b.Add(O::CallLambda, T::U64, Probe(), V{inputs.back()});
        b.Store(helper, 1536 + wave * 8);
        auto* sum = inputs.front();
        for (u32 i = 1; i < width; ++i) sum = b.Add(O::Add, T::U64, V{sum}, ir::Operand{V{inputs[i]}});
        b.Store(sum, 1024 + wave * 8);
    }
    b.Finish(); const auto ids = b.IDs();
    for (bool split : {false, true}) {
        ReuseCode baseline(b, false, split), colored(b, true, split);
        Check(colored.translator.HomeBytes() < baseline.translator.HomeBytes() && colored.context.HomeStats().reused_homes >= waves,
              "multiple real spill/helper waves substantially reuse homes");
        Check(colored.context.HomeStats().spill_homes > 0 || colored.context.HomeStats().split_homes > 0,
              "pressure positive control includes actual spill or split homes");
        for (u64 seed : {u64{1}, u64{17}, u64{0xfeedbeef}}) for (auto* code : {&baseline, &colored}) {
            StateStorage state, expected; Initialize(state, seed); Initialize(expected, seed);
            for (u32 wave = 0; wave < waves; ++wave) {
                u64 sum{};
                for (u32 i = 0; i < width; ++i) sum += expected.Get(64 + (wave * width + i) * 8);
                expected.Put(1024 + wave * 8, sum);
                expected.Put(1536 + wave * 8, ~expected.Get(64 + (wave * width + width - 1) * 8));
            }
            callback_calls = 0; code->Run(state);
            Check(callback_calls == waves && callback_value == state.Get(64 + (waves * width - 1) * 8),
                  "all pressure-wave helper inputs survive publication");
            SameUniform(state, expected, "pressure-wave home reuse and splitting equal independent C model including uniform sentinels");
        }
        std::cout << "HOME-REUSE split=" << split << " unique=" << baseline.translator.HomeBytes()
                  << " reused=" << colored.translator.HomeBytes() << " required=" << colored.context.HomeStats().required_homes
                  << " slots=" << colored.context.HomeStats().home_count << " edges=" << colored.context.HomeStats().interference_edges << '\n';
    }
    Check(b.IDs() == ids, "execution compilation preserves source IDs");
}

void RawPayloadExecution() {
    Builder b;
    constexpr u32 waves = 8;
    for (u32 wave = 0; wave < waves; ++wave) {
        auto* source = b.Load(64 + wave * 16, T::V128);
        auto* carrier = b.Add(O::BitCast, T::U8, V{source});
        auto* helper = b.Add(O::CallLambda, T::U64, Probe(), ir::U8{carrier});
        auto* restored = b.Add(O::BitCast, T::V128, V{carrier});
        b.Store(restored, 1024 + wave * 16); b.Store(helper, 1536 + wave * 8);
        auto* scalar = b.Load(2048 + wave * 8);
        auto* widened = b.Add(O::BitCast, T::V128, V{scalar}); b.Store(widened, 3072 + wave * 16);
        auto* narrow = b.Load(2304 + wave * 8, T::V8);
        auto* narrow_wide = b.Add(O::BitCast, T::V128, V{narrow}); b.Store(narrow_wide, 3328 + wave * 16);
    }
    b.Finish();
    const auto host = la::HostFeatures::Detect();
    for (auto features : {host.WithPolicy(false, false, false), host.WithPolicy(true, false, false), host})
        for (bool vector_cfg : {false, true}) for (bool split : {false, true}) for (bool reuse : {false, true}) {
            ReuseCode code(b, reuse, split, features, vector_cfg);
            StateStorage state, expected; Initialize(state, 0x12345); Initialize(expected, 0x12345);
            for (u32 wave = 0; wave < waves; ++wave) {
                expected.Put(1024 + wave * 16, expected.Get(64 + wave * 16));
                expected.Put(1032 + wave * 16, expected.Get(72 + wave * 16));
                expected.Put(1536 + wave * 8, ~(expected.Get(64 + wave * 16) & 255));
                expected.Put(3072 + wave * 16, expected.Get(2048 + wave * 8)); expected.Put(3080 + wave * 16, 0);
                expected.Put(3328 + wave * 16, expected.Get(2304 + wave * 8) & 255); expected.Put(3336 + wave * 16, 0);
            }
            callback_calls = 0; code.Run(state);
            Check(callback_calls == waves, "raw payload fixture reaches every helper");
            SameUniform(state, expected, "raw carriers preserve high payload while low-only scalar/narrow-vector homes stay zero-high");
        }
}

void PhiAndDelayedPseudoExecution() {
    Builder b;
    auto* label = b.Label(); auto* x = b.Phi(), *y = b.Phi(), *n = b.Phi();
    b.Store(x); b.Store(y, 8);
    auto* next = b.Add(O::Sub, T::U64, V{n}, ir::Operand{ir::Imm{u64{1}}});
    auto* condition = b.Add(O::TestNotZero, T::U8, V{next});
    auto* back = b.Add(O::Goto, T::U8, ir::BOOL{condition}); b.Store(next, 16);
    ir::Params xp; xp.Push(ir::Imm{u64{31}}); xp.Push(V{y}); x->SetArgs(xp);
    ir::Params yp; yp.Push(ir::Imm{u64{47}}); yp.Push(V{x}); y->SetArgs(yp);
    ir::Params np; np.Push(ir::Imm{u64{6}}); np.Push(V{next}); n->SetArgs(np); b.Finish();
    for (bool split : {false, true}) for (bool reuse : {false, true}) {
        ReuseCode code(b, reuse, split, la::HostFeatures::Detect(), true, {{back, label}});
        StateStorage state, expected; Initialize(state, 7); Initialize(expected, 7);
        expected.Put(0, 47); expected.Put(8, 31); expected.Put(16, 0); code.Run(state);
        SameUniform(state, expected, "cyclic phi copies survive reused staging homes and splitting");
    }

    Builder pseudo;
    auto* lo = pseudo.Load(64), *hi = pseudo.Load(72), *divisor = pseudo.Load(80);
    // Div128's operands are {high, low, divisor}; check both zero and nonzero
    // high halves against an independent 128-bit C quotient/remainder model.
    auto* quotient = pseudo.Add(O::Div128, T::U64, V{hi}, V{lo}, V{divisor}, ir::Imm{u32{0}});
    auto* remainder = pseudo.Add(O::Div128Remainder, T::U64, V{quotient});
    pseudo.block.RemoveInst(remainder);
    for (u32 i = 0; i < 10; ++i) { auto* dead = pseudo.Load(128 + i * 8); pseudo.Store(dead, 512 + i * 8); }
    pseudo.Store(remainder, 16); pseudo.Store(quotient, 24); pseudo.block.AppendInstUnchecked(remainder); pseudo.Finish();
    for (bool split : {false, true}) for (bool reuse : {false, true}) {
        ReuseCode code(pseudo, reuse, split);
        for (u64 high : {u64{0}, u64{2}}) {
            StateStorage state, expected; Initialize(state, 19); Initialize(expected, 19);
            for (auto* item : {&state, &expected}) { item->Put(64, 1000); item->Put(72, high); item->Put(80, 7); }
            const unsigned __int128 numerator = (static_cast<unsigned __int128>(high) << 64) | 1000;
            expected.Put(16, u64(numerator % 7)); expected.Put(24, u64(numerator / 7));
            for (u32 i = 0; i < 10; ++i) expected.Put(512 + i * 8, expected.Get(128 + i * 8));
            code.Run(state); SameUniform(state, expected, "delayed producer-written remainder survives later home reuse");
        }
    }
}

void WideLifetimeExecution() {
    Builder b;
    for (u32 wave = 0; wave < 8; ++wave) {
        auto* left = b.Load(64 + wave * 64, T::V256), *right = b.Load(96 + wave * 64, T::V256);
        auto* result = b.Add(O::VecXor, T::V256, V{left}, V{right});
        // Keep only low128 alive after defining unrelated vectors. A LASX
        // cache may still publish the entire dirty pair at the helper barrier.
        auto* unrelated = b.Load(640 + wave * 16, T::V128);
        auto* caller_arg = b.Load(896 + wave * 8);
        auto* helper = b.Add(O::CallLambda, T::U64, Probe(), V{caller_arg});
        auto* low = b.Add(O::BitCast, T::V128, V{result});
        b.Store(helper, 1536 + wave * 8); b.Store(unrelated, 2048 + wave * 16); b.Store(low, 1024 + wave * 16);
    }
    b.Finish();
    const auto host = la::HostFeatures::Detect();
    for (auto features : {host.WithPolicy(false, false, false), host.WithPolicy(true, false, false), host})
        for (bool vector_cfg : {false, true}) for (bool reuse : {false, true}) {
            ReuseCode code(b, reuse, true, features, vector_cfg);
            StateStorage state, expected; Initialize(state, 997); Initialize(expected, 997);
            for (u32 wave = 0; wave < 8; ++wave) {
                expected.Put(1024 + wave * 16, expected.Get(64 + wave * 64) ^ expected.Get(96 + wave * 64));
                expected.Put(1032 + wave * 16, expected.Get(72 + wave * 64) ^ expected.Get(104 + wave * 64));
                expected.Put(2048 + wave * 16, expected.Get(640 + wave * 16));
                expected.Put(2056 + wave * 16, expected.Get(648 + wave * 16));
                expected.Put(1536 + wave * 8, ~expected.Get(896 + wave * 8));
            }
            callback_calls = 0; code.Run(state);
            Check(callback_calls == 8, "wide asymmetric-lifetime fixture reaches helper barriers");
            SameUniform(state, expected, "wide low-half and unrelated live vector survive full-pair delayed publication");
            if (reuse) Check(code.context.HomeStats().paired_homes > 0 && code.context.HomeStats().reused_homes > 0,
                             "wide execution has paired provenance and actual home reuse");
        }
}
}  // namespace

void HomeReuse() {
    LinearLifetimeOracle(); CFGAndPhiOracle(); PayloadPseudoAndWideOracle(); SplitStagingOracle(); TerminalsIsolationAndBounds();
    PressureWavesExecution(); RawPayloadExecution(); PhiAndDelayedPseudoExecution(); WideLifetimeExecution();
}
}  // namespace swift::tests::loongarch64
