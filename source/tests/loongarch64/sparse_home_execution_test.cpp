#include "test_support.h"

#include <memory>
#include <vector>

namespace swift::tests::loongarch64 {
namespace {
using O = ir::OpCode;
using T = ir::ValueType;
using I = ir::Inst;
using V = ir::Value;

struct Builder {
    ir::Block block{ir::Location{0x1000}};
    Builder() { block.SetTerminal(ir::terminal::ReturnToHost{}); }
    template<class... Args> I* Add(O op, T type, const Args&... args) {
        auto* inst = I::Create(op, args...);
        inst->SetReturn(type); block.AppendInstUnchecked(inst); return inst;
    }
    I* Constant(u64 value, T type = T::U64) { return Add(O::LoadImm, type, ir::Imm{value}); }
    I* Store(I* value, u32 offset = 0) {
        return Add(O::StoreUniform, T::VOID, ir::Uniform{offset, value->ReturnType()}, V{value});
    }
    void Finish() { block.ReIdInstr(); }
    std::vector<u16> IDs() {
        std::vector<u16> result;
        for (auto& inst : block.GetInstList()) result.push_back(inst.Id());
        return result;
    }
};

struct HomesCode {
    Config config{TestConfig()};
    la::JitContext context;
    la::JitTranslator translator{context};
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    BlockFn fn{};
    HomesCode(Builder& b, bool sparse,
              la::ScalarAllocationMode mode = la::ScalarAllocationMode::GlobalCFG,
              const la::RegisterPlan::Targets& targets = {},
              const la::RegisterPlan::Stops& stops = {},
              const la::RegisterPlan::Exits& exits = {})
            : context(true, la::HostFeatures::Detect().WithPolicy(false, false, false), {}, mode) {
        context.SetSparseHomes(sparse);
        context.ConfigureFlow(targets, stops, {});
        translator.SetFunctionExits(exits, {});
        translator.Translate(&b.block);
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate compact-home executable");
        context.Flush(*buffer);
        fn = reinterpret_cast<BlockFn>(buffer->exec_data);
        Check(context.SparseHomesActive() == (sparse && mode == la::ScalarAllocationMode::GlobalCFG),
              "sparse request only affects GlobalCFG mode");
    }
    void Run(StateStorage& storage) { Check(CheckABI(fn, storage.state) == 1, "compact-home code preserves host ABI"); }
};

u64 observed{};
u64 Capture(u64 value) { observed = value; return ~value; }
ir::Lambda Probe() { return ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Capture)}}; }

void ChainAndModes() {
    Builder b;
    auto* result = b.Constant(17);
    for (u32 i = 0; i < 64; ++i) result = b.Add(O::Add, T::U64, V{result}, ir::Operand{ir::Imm{u64{3}}});
    b.Store(result); b.Finish(); const auto source_ids = b.IDs();
    HomesCode dense(b, false), sparse(b, true);
    StateStorage baseline, actual; dense.Run(baseline); sparse.Run(actual);
    Check(baseline.Get(0) == 209 && actual.Get(0) == baseline.Get(0), "dense and compact chain results agree");
    Check(sparse.context.SlotCount() == 1 && sparse.context.HomeStats().required_homes == 0, "chain retains only minimum frame anchor");
    Check(dense.translator.HomeBytes() == 1056 && sparse.translator.HomeBytes() == 16, "chain reserves 1040 fewer home bytes");
    Check(dense.translator.Stats().frame_size + dense.translator.HomeBytes() -
                  sparse.translator.Stats().frame_size - sparse.translator.HomeBytes() == 1040,
          "total generated stack reservation shrinks by home savings");
    // The prologue clears homes with a loop. Compaction reduces its dynamic
    // iterations, while the static loop can have the same instruction count.
    Check(dense.context.SlotCount() * 2 == 132 && sparse.context.SlotCount() * 2 == 2,
          "compact chain reduces dynamic prologue zero stores from 132 to 2");
    Check(sparse.context.CurrentBufferSize() <= dense.context.CurrentBufferSize(),
          "compact chain does not increase emitted code size");
    Check(sparse.context.ValueStats().loads == 0 && sparse.context.ValueStats().stores == 0, "compact chain avoids SSA home accesses");
    Check(b.IDs() == source_ids, "compilation preserves public chain IDs");
    for (auto mode : {la::ScalarAllocationMode::LocalCache, la::ScalarAllocationMode::CanonicalHomes}) {
        HomesCode code(b, true, mode); StateStorage state; code.Run(state);
        Check(state.Get(0) == 209 && code.translator.HomeBytes() == 1056, "baseline allocation modes retain dense homes");
    }
}

void NonContiguousSourceIDs() {
    Builder b;
    auto* value = b.Constant(0x123456789abcdef0ULL);
    auto* store = b.Store(value);
    b.Finish();
    value->SetId(50000);
    store->SetId(50001);
    const auto source_ids = b.IDs();
    HomesCode compact(b, true);
    StateStorage actual;
    compact.Run(actual);
    Check(actual.Get(0) == 0x123456789abcdef0ULL,
          "private compact layout executes non-contiguous public instruction IDs");
    Check(compact.context.SlotCount() == 1 && compact.context.HomeStats().required_homes == 0,
          "non-contiguous source IDs do not allocate dense sparse homes");
    Check(b.IDs() == source_ids && value->Id() == 50000 && store->Id() == 50001,
          "compact compilation preserves non-contiguous public instruction IDs");
}

void ForeignProducerPseudo() {
    Builder b;
    auto* producer = b.Add(O::Zero, T::U64);
    b.Finish();
    producer->SetId(50000);
    const auto source_ids = b.IDs();
    // Linked producer outputs outside the source body must be rejected before
    // private cloning, rather than silently disappearing from its inventory.
    auto outside = std::unique_ptr<I>(I::Create(O::Div128Remainder, V{producer}));
    outside->SetReturn(T::U64);
    outside->SetId(50001);
    la::JitContext context;
    la::JitTranslator translator{context};
    bool rejected{};
    try { translator.Translate(&b.block); }
    catch (const std::exception&) { rejected = true; }
    Check(rejected && context.CurrentBufferSize() == 0,
          "compact translator rejects foreign producer pseudo before emitting code");
    Check(b.IDs() == source_ids && producer->Id() == 50000 && outside->Id() == 50001,
          "rejected compact cloning preserves source and external pseudo IDs");
    bool retained{};
    for (auto* pseudo : producer->GetPseudoOperations()) retained |= pseudo == outside.get();
    Check(retained, "rejected private cloning retains the source producer's external pseudo link");
}

void HelperAndPressure() {
    Builder b;
    std::vector<I*> values;
    // Dynamic inputs keep all nine values live across the helper even when
    // scalar LoadImm rematerialization is enabled by default.
    for (u32 i = 0; i < 9; ++i)
        values.push_back(b.Add(O::LoadUniform, T::U64, ir::Uniform{64 + i * 8, T::U64}));
    b.Add(O::CallLambda, T::VOID, Probe(), V{values.back()});
    auto* sum = values.front();
    for (u32 i = 1; i < values.size(); ++i) sum = b.Add(O::Add, T::U64, V{sum}, ir::Operand{V{values[i]}});
    b.Store(sum); b.Finish(); const auto source_ids = b.IDs();
    for (bool compact : {false, true}) {
        HomesCode code(b, compact); StateStorage state;
        for (u32 i = 0; i < values.size(); ++i) state.Put(64 + i * 8, i + 1);
        observed = 0; code.Run(state);
        Check(state.Get(0) == 45 && observed == 9, "values survive spills and VOID typed helper");
        Check(code.context.ValueStats().spills > 0, "helper-pressure fixture executes allocated spill sites");
        if (compact) {
            Check(code.context.HomeStats().spill_homes > 0 && code.context.HomeStats().helper_homes >= 2,
                  "compact layout retains spills and VOID helper own/input homes");
            for (auto& inst : code.translator.EmittedIR()->GetInstList()) if (inst.GetOp() == O::CallLambda)
                Check(code.context.HomeOffset(&inst) < code.translator.HomeBytes(), "VOID helper ownhome is valid in emitted clone");
        }
    }
    Check(b.IDs() == source_ids, "helper compilation preserves public IDs");
}

void RawThroughHelper() {
    constexpr u64 low = 0x0123456789abcdefULL, high = 0xfedcba9876543210ULL;
    Builder b;
    auto* vector = b.Add(O::LoadUniform, T::V128, ir::Uniform{64, T::V128});
    auto* carrier = b.Add(O::BitCast, T::U8, V{vector});
    auto* helper = b.Add(O::CallLambda, T::U64, Probe(), ir::U8{carrier});
    auto* restored = b.Add(O::BitCast, T::V128, V{carrier});
    b.Store(helper, 32); b.Store(restored, 96); b.Finish();
    for (bool compact : {false, true}) {
        HomesCode code(b, compact); StateStorage state;
        state.Put(64, low); state.Put(72, high); observed = 0; code.Run(state);
        Check(observed == (low & 255), "typed helper receives normalized narrow input");
        Check(state.Get(96) == low && state.Get(104) == high, "helper publication preserves raw carrier's complete payload");
        Check(state.Get(32) == ~(low & 255), "typed helper result reload uses its own home");
    }
}

void PhiCycle() {
    Builder b;
    auto* label = new I(O::BindLabel); label->SetArgs(V{label}); label->SetReturn(T::VOID); b.block.AppendInstUnchecked(label);
    auto* x = new I(O::AddPhi), *y = new I(O::AddPhi), *n = new I(O::AddPhi);
    for (auto* phi : {x, y, n}) { phi->SetReturn(T::U64); b.block.AppendInstUnchecked(phi); }
    b.Store(x); b.Store(y, 8);
    auto* next = b.Add(O::Sub, T::U64, V{n}, ir::Operand{ir::Imm{u64{1}}});
    auto* condition = b.Add(O::TestNotZero, T::U8, V{next});
    auto* back = b.Add(O::Goto, T::U8, ir::BOOL{condition});
    b.Store(next, 16);
    ir::Params xp; xp.Push(ir::Imm{u64{31}}); xp.Push(V{y}); x->SetArgs(xp);
    ir::Params yp; yp.Push(ir::Imm{u64{47}}); yp.Push(V{x}); y->SetArgs(yp);
    ir::Params np; np.Push(ir::Imm{u64{6}}); np.Push(V{next}); n->SetArgs(np);
    b.Finish(); const auto source_ids = b.IDs();
    for (bool compact : {false, true}) {
        HomesCode code(b, compact, la::ScalarAllocationMode::GlobalCFG, {{back, label}});
        StateStorage state; code.Run(state);
        Check(state.Get(0) == 47 && state.Get(8) == 31 && state.Get(16) == 0, "compact CFG executes cyclic parallel Phi copies");
        if (compact) Check(code.context.HomeStats().phi_homes == 3, "three Phi destinations retain high-half homes");
    }
    Check(b.IDs() == source_ids, "cyclic Phi source IDs unchanged");
}

void NativeAndTypedGuestMemory() {
    for (bool atomic : {false, true}) {
        Builder b;
        auto* address = b.Constant(3), *replacement = b.Constant(121);
        auto* before = atomic ? b.Add(O::AtomicExchange, T::U64, V{address}, V{replacement}) :
                                b.Add(O::LoadMemory, T::U64, ir::Operand{V{address}});
        if (!atomic) b.Add(O::StoreMemory, T::VOID, ir::Operand{V{address}}, V{replacement});
        b.Store(before); b.Finish();
        for (bool compact : {false, true}) {
            HomesCode code(b, compact); StateStorage state; alignas(16) std::array<u8, 32> memory{};
            const u64 initial = 101; std::memcpy(memory.data() + 3, &initial, 8);
            state.state->pt = memory.data(); state.state->guest_addr_limit = memory.size(); state.state->guest_addr_mask = memory.size() - 1;
            code.Run(state); u64 written{}; std::memcpy(&written, memory.data() + 3, 8);
            Check(state.Get(0) == initial && written == 121, "compact unaligned guest access agrees with dense result");
            if (compact) {
                if (atomic) Check(code.context.HomeStats().helper_homes >= 3, "unaligned atomic typed kernel has input/result homes");
                else Check(code.context.HomeStats().required_homes == 0, "native guest-address calls preserve register-only SSA values");
            }
        }
    }
}

void NestedExitsAndWide() {
    Builder exit;
    auto* predicate = exit.Constant(1, T::U8), *selector = exit.Constant(7);
    auto* marker = exit.Add(O::Nop, T::VOID); exit.Finish(); const auto source_ids = exit.IDs();
    const ir::Terminal terminal = ir::terminal::If{ir::BOOL{predicate},
            ir::terminal::CheckHalt{ir::terminal::Switch{V{selector}, {{ir::Imm{u64{7}}, ir::terminal::ReturnToHost{}}}}},
            ir::terminal::ReturnToDispatch{}};
    for (bool compact : {false, true}) {
        HomesCode code(exit, compact, la::ScalarAllocationMode::GlobalCFG, {}, {marker}, {{marker, terminal}});
        StateStorage state; code.Run(state);
        Check(state.state->halt_reason == HaltReason::CallHost, "private function terminal recursively remaps dynamic selectors");
        if (compact) Check(code.context.HomeStats().required_homes == 0, "nested terminal scalars remain in fixed registers");
    }
    Check(exit.IDs() == source_ids, "function exit source IDs preserved");

    Builder wide;
    auto* payload = wide.Add(O::LoadUniform, T::V256, ir::Uniform{0, T::V256}); wide.Store(payload, 64); wide.Finish();
    const auto wide_ids = wide.IDs();
    for (bool compact : {false, true}) {
        HomesCode code(wide, compact); StateStorage state;
        for (u32 part = 0; part < 4; ++part) state.Put(part * 8, 0xabcdef0000000000ULL + part);
        code.Run(state);
        for (u32 part = 0; part < 4; ++part) Check(state.Get(64 + part * 8) == state.Get(part * 8), "compact legalized V256 halves preserve payload");
        if (compact) Check(code.context.HomeStats().vector_homes == 2, "V256 lowering retains two vector homes");
    }
    Check(wide.IDs() == wide_ids, "V256 source IDs preserved");
}
}  // namespace

void SparseHomesExecution() {
    ChainAndModes(); NonContiguousSourceIDs(); ForeignProducerPseudo();
    HelperAndPressure(); RawThroughHelper(); PhiCycle();
    NativeAndTypedGuestMemory(); NestedExitsAndWide();
}
}  // namespace swift::tests::loongarch64
