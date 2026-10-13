#include "test_support.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <vector>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
using V = ir::Value;
using M = la::ScalarAllocationMode;
constexpr u32 kSources = 12, kPreserved = 1024, kResults = 1152;
constexpr u32 kCondition = 256, kCount = 264, kAccumulator = 272;

u64 Callback(u64 a, u64 b, u64 c) { return std::rotl(a + b, 17) ^ c; }

struct Fixture {
    std::string name;
    IntrusivePtr<ir::Block> block;
    std::function<void(StateStorage&)> model;
    bool interpreter{true}, improves{}, no_gain{};
    Fixture(std::string name, u64 location) : name(std::move(name)), block(new ir::Block(ir::Location{location})) {}
    void Finish() { block->SetTerminal(ir::terminal::ReturnToHost{}); block->ReIdInstr(); }
};

std::array<V, kSources> Sources(ir::Assembler& as) {
    std::array<V, kSources> result;
    for (u32 i = 0; i < result.size(); ++i)
        result[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
    return result;
}

std::array<u64, kSources> Sources(const StateStorage& state) {
    std::array<u64, kSources> result;
    for (u32 i = 0; i < result.size(); ++i) result[i] = state.Get(i * 8);
    return result;
}

void Preserve(ir::Assembler& as, const std::array<V, kSources>& input) {
    for (u32 i = 0; i < input.size(); ++i)
        as.StoreUniform(ir::Uniform{kPreserved + i * 8, T::U64}, input[i]);
}

void Preserve(StateStorage& state, const std::array<u64, kSources>& input) {
    for (u32 i = 0; i < input.size(); ++i) state.Put(kPreserved + i * 8, input[i]);
}

V Phase(ir::Assembler& as, const std::array<V, kSources>& input, V value, u32 phase, u32 rounds) {
    for (u32 i = 0; i < rounds; ++i) {
        value = as.Add(value, ir::Operand{input[phase * 4 + i % 4]}).SetType(T::U64);
        value = as.Xor(value, ir::Operand{input[phase * 4 + (i + 1) % 4]}).SetType(T::U64);
    }
    return value;
}

u64 Phase(const std::array<u64, kSources>& input, u64 value, u32 phase, u32 rounds) {
    for (u32 i = 0; i < rounds; ++i)
        value = (value + input[phase * 4 + i % 4]) ^ input[phase * 4 + (i + 1) % 4];
    return value;
}

Fixture RotatingPressure(bool helper) {
    Fixture fixture{helper ? "rotating-helper" : "rotating-pressure", helper ? 0x7a200 : 0x7a100};
    ir::Assembler as{fixture.block.get()};
    const auto input = Sources(as);
    std::array<V, 3> result;
    auto value = input[0];
    for (u32 phase = 0; phase < result.size(); ++phase) {
        value = Phase(as, input, value, phase, 48);
        if (helper) value = as.CallHost(&Callback, value, input[phase * 4], input[phase * 4 + 3]).SetType(T::U64);
        result[phase] = value;
    }
    Preserve(as, input);
    for (u32 phase = 0; phase < result.size(); ++phase)
        as.StoreUniform(ir::Uniform{kResults + phase * 8, T::U64}, result[phase]);
    fixture.Finish(); fixture.improves = true;
    fixture.model = [helper](StateStorage& state) {
        const auto input = Sources(state);
        auto value = input[0];
        for (u32 phase = 0; phase < 3; ++phase) {
            value = Phase(input, value, phase, 48);
            if (helper) value = std::rotl(value + input[phase * 4], 17) ^ input[phase * 4 + 3];
            state.Put(kResults + phase * 8, value);
        }
        Preserve(state, input);
    };
    return fixture;
}

Fixture DiamondPhi() {
    Fixture fixture{"pressure-diamond-phi", 0x7a300};
    ir::Assembler as{fixture.block.get()};
    const auto input = Sources(as);
    const auto condition = as.LoadUniform(ir::Uniform{kCondition, T::U64}).SetType(T::U64);
    const auto initial = Phase(as, input, input[0], 0, 16);
    const auto edge = as.Goto(ir::BOOL{condition});
    const auto alternate = Phase(as, input, initial, 1, 16);
    as.BindLabel(edge);
    // Incoming edges follow source-block order: the initial block's taken
    // edge precedes the later alternate block's fallthrough edge.
    ir::Params incoming; incoming.Push(initial); incoming.Push(alternate);
    const auto joined = as.AddPhi(incoming).SetType(T::U64);
    const auto result = Phase(as, input, joined, 2, 16);
    Preserve(as, input);
    as.StoreUniform(ir::Uniform{kResults, T::U64}, joined);
    as.StoreUniform(ir::Uniform{kResults + 8, T::U64}, result);
    fixture.Finish();
    // The portable interpreter has no AddPhi execution semantics. Its other
    // fixtures independently qualify arithmetic; this join has a C oracle.
    fixture.interpreter = false;
    fixture.model = [](StateStorage& state) {
        const auto input = Sources(state);
        auto value = Phase(input, input[0], 0, 16);
        if (!state.Get(kCondition)) value = Phase(input, value, 1, 16);
        state.Put(kResults, value);
        state.Put(kResults + 8, Phase(input, value, 2, 16));
        Preserve(state, input);
    };
    return fixture;
}

Fixture Backedge() {
    Fixture fixture{"pressure-backedge", 0x7a400};
    ir::Assembler as{fixture.block.get()};
    const auto input = Sources(as);
    const auto count = as.LoadUniform(ir::Uniform{kCount, T::U64}).SetType(T::U64);
    const auto value = as.LoadUniform(ir::Uniform{kAccumulator, T::U64}).SetType(T::U64);
    const auto sum = Phase(as, input, value, 1, 8);
    as.StoreUniform(ir::Uniform{kAccumulator, T::U64}, sum);
    const auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{kCount, T::U64}, next);
    const auto edge = as.Goto(as.TestNotZero(next));
    as.BindLabel(edge);
    auto* label = &fixture.block->GetInstList().back();
    fixture.block->RemoveInst(label); fixture.block->InsertBefore(label, count.Def());
    Preserve(as, input);
    as.StoreUniform(ir::Uniform{kResults, T::U64}, sum);
    fixture.Finish();
    fixture.model = [](StateStorage& state) {
        const auto input = Sources(state);
        auto value = state.Get(kAccumulator);
        for (u64 count = state.Get(kCount); count; --count) value = Phase(input, value, 1, 8);
        state.Put(kCount, 0); state.Put(kAccumulator, value); state.Put(kResults, value);
        Preserve(state, input);
    };
    return fixture;
}

Fixture RawAndFlags() {
    Fixture fixture{"raw-container-helper-delayed-flags", 0x7a500};
    ir::Assembler as{fixture.block.get()};
    const auto input = Sources(as);
    const auto raw = as.LoadUniform(ir::Uniform{320, T::V128}).SetType(T::V128);
    const auto narrow = as.BitCast(raw).SetType(T::U8);
    const auto producer = as.Add(input[0], ir::Operand{input[11]}).SetType(T::U64);
    const auto saved = as.GetFlags(producer, ir::Flags::All).SetType(T::U64);
    auto value = Phase(as, input, producer, 1, 16);
    value = as.CallHost(&Callback, value, input[3], input[8]).SetType(T::U64);
    value = Phase(as, input, value, 2, 16);
    const auto incremented = as.Add(narrow, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U8);
    as.StoreUniform(ir::Uniform{1280, T::V128}, as.BitCast(narrow).SetType(T::V128));
    as.StoreUniform(ir::Uniform{1296, T::U8}, incremented);
    as.StoreUniform(ir::Uniform{1304, T::U64}, saved);
    as.SaveFlags(producer, ir::Flags::Carry | ir::Flags::Overflow);
    as.StoreUniform(ir::Uniform{kResults, T::U64}, value);
    Preserve(as, input);
    fixture.Finish();
    // The independent interpreter checks flags and the complete raw payload.
    // The arithmetic oracle additionally checks all pressure-chain results.
    fixture.model = [](StateStorage& state) {
        const auto input = Sources(state);
        auto value = Phase(input, input[0] + input[11], 1, 16);
        value = std::rotl(value + input[3], 17) ^ input[8];
        state.Put(kResults, Phase(input, value, 2, 16));
        Preserve(state, input);
    };
    return fixture;
}

Fixture SingleUsePressure() {
    Fixture fixture{"single-use-pressure-baseline", 0x7a600};
    ir::Assembler as{fixture.block.get()};
    std::array<V, 7> input;
    for (u32 i = 0; i < input.size(); ++i)
        input[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
    for (u32 i = 0; i < input.size(); ++i)
        as.StoreUniform(ir::Uniform{kPreserved + i * 8, T::U64}, input[i]);
    fixture.Finish(); fixture.no_gain = true;
    fixture.model = [](StateStorage& state) {
        for (u32 i = 0; i < 7; ++i) state.Put(kPreserved + i * 8, state.Get(i * 8));
    };
    return fixture;
}

struct SplitCompiled {
    Config config{TestConfig()};
    la::JitContext context{true, la::HostFeatures::Detect(), {}, M::GlobalCFG};
    la::JitTranslator translator{context};
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    BlockFn fn{};
    SplitCompiled(Fixture& fixture, bool split, bool sparse) {
        context.SetScalarSplitting(split);
        context.SetSparseHomes(sparse);
        context.SetUniformForwarding(false);
        context.SetScalarRematerialization(false);
        translator.Translate(fixture.block.get());
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate scalar live-range splitting fixture");
        context.Flush(*buffer); fn = reinterpret_cast<BlockFn>(buffer->exec_data);
    }
};

void Initialize(StateStorage& state, u32 sample) {
    u64 seed = 0xb3861732a519cd47ULL ^ (u64(sample + 1) * 0x9e3779b97f4a7c15ULL);
    for (u32 offset = 0; offset < 4096; offset += 8) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        const u64 value = sample == 0 ? u64(offset + 1) * 0x0123456789abcdefULL :
                          sample == 1 ? UINT64_MAX ^ u64(offset) :
                          sample == 2 ? 0x8000800080008000ULL ^ (u64(offset) << 24) :
                          sample == 3 ? 0x7fff7fff7fff7fffULL ^ (u64(offset) << 32) : seed;
        state.Put(offset, value);
    }
    state.Put(kCondition, sample & 1); state.Put(kCount, 1 + sample % 17);
    state.state->host_cpu_flags = std::rotl(seed, 23);
}

void Equal(const StateStorage& actual, const StateStorage& expected, const std::string& detail) {
    u32 first = actual.bytes.size();
    for (u32 i = 0; i < actual.bytes.size(); ++i) if (actual.bytes[i] != expected.bytes[i]) { first = i; break; }
    Check(first == actual.bytes.size(), detail + " first_state_byte=" + std::to_string(first));
}

void Compare(Fixture& fixture, SplitCompiled& compiled, bool split, bool sparse) {
    for (u32 sample = 0; sample < 24; ++sample) {
        const auto detail = fixture.name + " split=" + std::to_string(split) +
            " sparse=" + std::to_string(sparse) + " sample=" + std::to_string(sample);
        StateStorage expected, actual, model;
        Initialize(expected, sample); Initialize(actual, sample); Initialize(model, sample);
        fixture.model(model);
        if (fixture.interpreter) {
            backend::interp::Interpreter interpreter{*expected.state, fixture.block.get()};
            Check(interpreter.Run() == HaltReason::CallHost, "scalar split independent interpreter halt " + detail);
            for (u32 offset = kPreserved; offset < kPreserved + kSources * 8; offset += 8)
                Check(expected.Get(offset) == model.Get(offset), "preserved source C oracle " + detail);
            Check(expected.Get(kResults) == model.Get(kResults), "pressure-chain C oracle " + detail);
            if (fixture.improves) for (u32 phase = 1; phase < 3; ++phase)
                Check(expected.Get(kResults + phase * 8) == model.Get(kResults + phase * 8), "rotating phase C oracle " + detail);
        } else {
            expected.bytes = model.bytes;
            expected.state->halt_reason = HaltReason::CallHost;
        }
        Check(compiled.fn(actual.state) == HaltReason::CallHost, "scalar split native halt " + detail);
        Equal(actual, expected, "scalar split complete State differential " + detail);
        if (sample % 12 == 0) {
            StateStorage abi; Initialize(abi, sample);
            Check(CheckABI(compiled.fn, abi.state) == 1, "scalar split LP64D GPR/FPR/FCSR " + detail);
            Equal(abi, expected, "scalar split ABI-wrapped complete State " + detail);
        }
    }
}

void PlanContracts(Fixture& fixture) {
    const auto live = la::RegisterPlan::Build(fixture.block.get());
    const auto baseline = la::ScalarAllocationPlan::Build(fixture.block.get(), live);
    const auto split = la::ScalarAllocationPlan::Build(fixture.block.get(), live, {}, 6, false, true);
    Check(!baseline.SplittingEnabled() && split.SplittingEnabled(), "explicit scalar split analysis rollback");
    if (fixture.improves)
        Check(split.Stats().split_values > 0 && split.Stats().split_transfer_stores > 0 && split.Stats().split_transfer_loads > 0,
              "actual register/home fragments and transfers exist " + fixture.name);
    std::vector<ir::Inst*> instructions;
    for (auto& inst : fixture.block->GetInstList()) instructions.push_back(&inst);
    for (u32 i = 0; i < instructions.size(); ++i) {
        auto* inst = instructions[i];
        la::LiveSet values = live.At(inst);
        values.insert(live.After(inst).begin(), live.After(inst).end());
        values.insert(split.Definitions(inst).begin(), split.Definitions(inst).end());
        std::array<ir::Inst*, la::ScalarAllocationPlan::kRegisterCount> occupants{};
        for (auto* value : values) if (la::ScalarAllocationPlan::Eligible(value)) {
            if (auto reg = split.RegisterAt(value, inst)) {
                Check(*reg < occupants.size() && !occupants[*reg], "split point operands/results never alias " + fixture.name);
                occupants[*reg] = value;
            } else Check(baseline.Spilled(value) || split.NeedsSplitHome(value),
                         "every home fragment has canonical storage " + fixture.name);
            const bool edge = i == 0 || i + 1 == instructions.size() || inst->GetOp() == O::Goto ||
                inst->GetOp() == O::NotGoto || inst->GetOp() == O::BindLabel || inst->GetOp() == O::AddPhi ||
                (i && (instructions[i - 1]->GetOp() == O::Goto || instructions[i - 1]->GetOp() == O::NotGoto)) ||
                (i + 1 < instructions.size() && instructions[i + 1]->GetOp() == O::BindLabel);
            if (edge) Check(split.RegisterAt(value, inst) == baseline.Register(value),
                            "CFG edge agrees with global baseline placement " + fixture.name);
        }
        Check(baseline.MovesBefore(inst).empty(), "split OFF emits no placement transfers");
        for (const auto& move : split.MovesBefore(inst)) {
            Check(i && live.At(inst).contains(move.value) && move.from != move.to && split.NeedsSplitHome(move.value),
                  "split transfer moves a real incoming SSA value " + fixture.name);
            Check(move.from == split.RegisterAt(move.value, instructions[i - 1]) &&
                  move.to == split.RegisterAt(move.value, inst), "parallel split transfer placement contract " + fixture.name);
        }
    }
}
} // namespace

void ScalarSplittingExecution() {
    std::vector<Fixture> fixtures;
    fixtures.push_back(RotatingPressure(false)); fixtures.push_back(RotatingPressure(true));
    fixtures.push_back(DiamondPhi()); fixtures.push_back(Backedge()); fixtures.push_back(RawAndFlags());
    fixtures.push_back(SingleUsePressure());
    for (auto& fixture : fixtures) {
        PlanContracts(fixture);
        for (bool sparse : {false, true}) {
            SplitCompiled off{fixture, false, sparse}, on{fixture, true, sparse};
            Compare(fixture, off, false, sparse); Compare(fixture, on, true, sparse);
            const auto& plain = off.context.ValueStats(); const auto& optimized = on.context.ValueStats();
            const auto& plan = on.context.AllocationStats();
            std::cout << "SCALAR-SPLIT fixture=" << fixture.name << " sparse=" << sparse
                      << " loads_off=" << plain.loads << " loads_on=" << optimized.loads
                      << " stores_off=" << plain.stores << " stores_on=" << optimized.stores
                      << " bytes_off=" << off.context.CurrentBufferSize() << " bytes_on=" << on.context.CurrentBufferSize()
                      << " split_values=" << plan.split_values << " fragments=" << plan.fragments
                      << " transfer_loads=" << plan.split_transfer_loads
                      << " transfer_stores=" << plan.split_transfer_stores << std::endl;
            if (fixture.improves) {
                Check(plain.loads > 96 && plain.spills > 0, "positive control actually spills frequently read values");
                Check(optimized.loads + optimized.stores < plain.loads + plain.stores,
                      "real fragment splitting reduces total emitted SSA home traffic including transfers");
            }
            if (fixture.no_gain) {
                Check(plain.spills > 0, "single-use control really exceeds the global register bank");
                Check(plan.split_values == 0 && optimized.split_loads == 0 && optimized.split_stores == 0 &&
                      optimized.loads == plain.loads && optimized.stores == plain.stores &&
                      on.context.CurrentBufferSize() == off.context.CurrentBufferSize(),
                      "splitting without a net saving preserves the actual baseline code and home traffic");
            }
        }
    }
    std::cout << "PASS scalar live-range fragments, pressure/helper transfers, stable CFG edges, raw payload and ABI\n";
}
} // namespace swift::tests::loongarch64
