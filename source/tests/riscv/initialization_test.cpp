#include "test_support.h"
#include "runtime/frontend/ir_assembler.h"
#include <initializer_list>
#include <sys/resource.h>

namespace swift::tests::riscv {
namespace {
using T = ir::ValueType;

void ExpectHomes(ir::Block* block, std::initializer_list<ir::Value> homes,
                 bool unreachable_tail = false, bool function_contract = false, bool terminal_stop = false) {
    block->ReIdInstr();
    rv::JitContext actual, expected;
    if (unreachable_tail) actual.ConfigureFlow({}, {&block->GetInstList().front()});
    if (terminal_stop) actual.ConfigureFlow({}, {&block->GetInstList().back()});
    if (function_contract) {
        auto* last = &block->GetInstList().back();
        actual.ConfigureFlow({{last, last}}, {});
    }
    actual.ConfigureInitialization(block);
    actual.InitializeValues();
    u32 stores{};
    for (auto value : homes) {
        expected.Store(biscuit::x0, rv::values, s64(value.Def()->Id()) * rv::kValueStride);
        ++stores;
        if (value.Type() == T::V128) {
            expected.Store(biscuit::x0, rv::values, s64(value.Def()->Id()) * rv::kValueStride + 8);
            ++stores;
        }
    }
    Check(actual.ValueStats().initializations == stores, "exact initialization store count");
    Check(actual.CurrentBufferSize() == expected.CurrentBufferSize(), "exact initialization byte count");
    if (actual.CurrentBufferSize())
        Check(std::memcmp(actual.GetMasm().GetCodeBuffer().GetOffsetPointer(0),
                          expected.GetMasm().GetCodeBuffer().GetOffsetPointer(0),
                          actual.CurrentBufferSize()) == 0, "initialization homes and physical order");
}

void OrdinaryAndForward() {
    for (bool forward : {false, true}) for (T type : {T::U8, T::U64, T::V128}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2000})};
        ir::Assembler as{block.get()};
        auto source = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        as.StoreUniform(ir::Uniform{32, type}, source);
        auto* store = &block->GetInstList().back();
        // Move the consumer in front of its definition, preserving both nodes.
        if (forward) { block->RemoveInst(store); block->InsertBefore(store, source.Def()); }
        block->SetTerminal(ir::terminal::ReturnToHost{});
        if (forward) ExpectHomes(block.get(), {source});
        else ExpectHomes(block.get(), {});
        Compiled compiled{block.get()};
        StateStorage state; state.Put(0, 71); state.Put(8, 89); state.Put(32, 0);
        Check(compiled.fn(state.state) == HaltReason::CallHost, "straight-line initialization native halt");
        Check(state.Get(32) == (forward ? 0 : 71), "forward reads see initialized homes");
        if (type == T::V128) Check(state.Get(40) == (forward ? 0 : 89), "forward vector high home");
    }
}

void ImplicitFlagsAndPairPseudo() {
    {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2100})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto producer = as.Add(input, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        auto flags = as.GetFlags(producer, ir::Flags::Carry).SetType(T::U64);
        block->RemoveInst(flags.Def()); block->InsertBefore(flags.Def(), input.Def());
        block->SetTerminal(ir::terminal::ReturnToHost{});
        ExpectHomes(block.get(), {input, producer});
    }
    for (bool cpuid : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2200})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto producer = cpuid ? as.Cpuid(input, input, ir::Imm{u64{0}}, ir::Imm{u64{0}})
                              : as.Div128(input, input, input, ir::Imm{u64{0}});
        auto pseudo = cpuid ? as.CpuidUpper(producer) : as.Div128Remainder(producer);
        as.StoreUniform(ir::Uniform{8, T::U64}, pseudo);
        auto* consumer = &block->GetInstList().back();
        block->RemoveInst(consumer); block->InsertBefore(consumer, pseudo.Def());
        block->SetTerminal(ir::terminal::ReturnToHost{});
        ExpectHomes(block.get(), {});
    }
}

void NestedTerminalAndCFGContract() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2300})};
    ir::Assembler as{block.get()};
    auto first = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto later = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto result = as.Add(later, ir::Operand{first}).SetType(T::U64);
    block->RemoveInst(result.Def()); block->InsertBefore(result.Def(), later.Def());
    block->SetTerminal(ir::terminal::CheckHalt{ir::terminal::If{ir::BOOL{result},
        ir::terminal::ReturnToHost{}, ir::terminal::Condition{ir::Cond::EQ,
            ir::terminal::ReturnToHost{}, ir::terminal::ReturnToHost{}}}});
    ExpectHomes(block.get(), {later});
    ExpectHomes(block.get(), {later}, false, false, true);
    ExpectHomes(block.get(), {}, true);
    bool rejected{};
    try { ExpectHomes(block.get(), {}, false, true); }
    catch (const std::runtime_error& error) {
        rejected = std::string(error.what()) == "RV64 function SSA input does not dominate its use";
    }
    Check(rejected, "function flow metadata preserves invalid SSA rejection");
}

void SelfLambdaParamsAndForeign() {
    {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2400})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto value = as.Add(input, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        value.Def()->SetArg(0, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        ExpectHomes(block.get(), {value});
    }
    {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2500})};
        ir::Assembler as{block.get()};
        auto target = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto argument = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        ir::Params params; params.Push(argument);
        auto* call = block->AppendInst(ir::OpCode::CallDynamic, ir::Lambda{target}, params);
        block->RemoveInst(call); block->InsertBefore(call, target.Def());
        block->SetTerminal(ir::terminal::ReturnToHost{});
        ExpectHomes(block.get(), {target, argument});
    }
    {
        IntrusivePtr<ir::Block> foreign{new ir::Block(ir::Location{0x2600})};
        ir::Assembler other{foreign.get()};
        auto value = other.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2700})};
        ir::Assembler as{block.get()};
        as.StoreUniform(ir::Uniform{8, T::U64}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        // Initialization keeps its historical foreign-SSA policy. Public
        // compilation performs a separate verification before translation.
        ExpectHomes(block.get(), {});
    }
}

void EntryPhiAndLocalJoin() {
    for (T type : {T::U8, T::U16, T::U32, T::U64}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2800})};
        ir::Assembler as{block.get()};
        ir::Params incoming; incoming.Push(ir::Imm{u64{37}});
        auto phi = as.AddPhi(incoming).SetType(type);
        as.StoreUniform(ir::Uniform{32, type}, phi);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        Check(compiled.context.ValueStats().initializations == 0, "entry immediate phi does not need a zero home");
        StateStorage state;
        Check(compiled.fn(state.state) == HaltReason::CallHost, "entry immediate phi native halt");
        Check(state.Get(32) == 37, "entry phi move follows initialization and publishes its immediate");
    }
    for (bool invert : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2900})};
        ir::Assembler as{block.get()};
        auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        auto anchor = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        auto jump = invert ? as.NotGoto(ir::BOOL{condition}) : as.Goto(ir::BOOL{condition});
        auto skipped = as.Add(anchor, ir::Operand{ir::Imm{u64{11}}}).SetType(T::U64);
        as.BindLabel(jump);
        auto sum = as.Add(anchor, ir::Operand{skipped}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{16, T::U64}, sum);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        ExpectHomes(block.get(), {skipped});
        Compiled compiled{block.get()};
        for (u64 condition_value : {u64{0}, u64{1}}) {
            StateStorage state; state.Put(0, condition_value); state.Put(8, 71);
            Check(compiled.fn(state.state) == HaltReason::CallHost, "local CFG join native halt");
            const bool taken = invert ? condition_value == 0 : condition_value != 0;
            Check(state.Get(16) == (taken ? 71 : 153), "local CFG retains skipped-definition initialization");
        }
    }
}

void SingletonSelfEdgePhi() {
    for (T type : {T::U8, T::U64}) for (u64 count : {u64{1}, u64{2}, u64{9}}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3000})};
        ir::Assembler as{block.get()};
        auto remaining = as.AddPhi(ir::Params{}).SetType(type);
        auto sum = as.AddPhi(ir::Params{}).SetType(T::U64);
        auto next = as.Sub(remaining, ir::Operand{ir::Imm{u64{1}}}).SetType(type);
        auto updated = as.Add(sum, ir::Operand{remaining}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{16, T::U64}, updated);
        as.StoreUniform(ir::Uniform{24, type}, next);
        auto again = as.TestNotZero(next);
        auto jump = as.Goto(again);
        as.BindLabel(jump);
        auto* label = &block->GetInstList().back();
        block->RemoveInst(label); block->InsertBefore(label, remaining.Def());
        ir::Params remaining_inputs; remaining_inputs.Push(ir::Imm{count}); remaining_inputs.Push(next);
        remaining.Def()->SetArg(0, remaining_inputs);
        ir::Params sum_inputs; sum_inputs.Push(ir::Imm{u64{0}}); sum_inputs.Push(updated);
        sum.Def()->SetArg(0, sum_inputs);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Check(block->GetInstList().front().GetOp() == ir::OpCode::BindLabel &&
              block->GetInstList().back().GetOp() == ir::OpCode::Goto,
              "singleton fixture has entry label and last-instruction self edge");
        Compiled compiled{block.get()};
        Check(compiled.context.ValueStats().initializations == 0,
              "singleton backedge phi inputs dominate edge, entry phi moves initialize values");
        StateStorage state;
        Check(compiled.fn(state.state) == HaltReason::CallHost, "finite singleton phi loop native halt");
        Check(state.Get(16) == count * (count + 1) / 2 && state.Get(24) == 0,
              "singleton parallel phi moves preserve loop values and termination");
    }
}

void NoPhiTranslateFlowQualification() {
    // Exercise ConfigurePhis through Translate, not ConfigureInitialization alone.
    for (bool scalar_cache : {false, true}) for (u32 stop_mode = 0; stop_mode < 4; ++stop_mode) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2b00})};
        ir::Assembler as{block.get()};
        auto source = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto first = as.Add(source, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{8, T::U64}, first);
        auto middle = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        auto tail = as.Add(source, ir::Operand{ir::Imm{u64{13}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{24, T::U64}, tail);
        auto last = as.LoadUniform(ir::Uniform{32, T::U64}).SetType(T::U64);
        block->SetTerminal(ir::terminal::CheckHalt{ir::terminal::If{ir::BOOL{source},
            ir::terminal::ReturnToHost{}, ir::terminal::Condition{ir::Cond::EQ,
                ir::terminal::ReturnToHost{}, ir::terminal::ReturnToHost{}}}});
        IntrusivePtr<ir::Block> other{new ir::Block(ir::Location{0x2b10})};
        ir::Assembler foreign{other.get()};
        auto unknown = foreign.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        rv::JitContext context{scalar_cache};
        rv::JitTranslator translator{context};
        if (stop_mode) {
            auto* stop = stop_mode == 1 ? last.Def() : stop_mode == 2 ? middle.Def() : unknown.Def();
            context.ConfigureFlow({}, {stop});
            if (stop_mode != 3) translator.SetFunctionExits({{stop, ir::terminal::ReturnToHost{}}}, {});
        }
        translator.Translate(block.get());
        Check(context.ValueStats().initializations == 0, "no-phi Translate stop metadata preserves initialized SSA");
        Config config{TestConfig()};
        backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
        QuiescentRawCache release_cache{cache};
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate no-phi Translate flow fixture");
        context.Flush(*buffer);
        auto fn = reinterpret_cast<BlockFn>(buffer->exec_data);
        for (u64 input : {u64{0}, u64{9}}) {
            StateStorage state; state.Put(0, input); state.Put(24, 0xc0ffee);
            Check(fn(state.state) == HaltReason::CallHost, "no-phi nested terminal/flowstop native halt");
            Check(state.Get(8) == input + 7, "no-phi value live across arithmetic and uniform store");
            Check(state.Get(24) == (stop_mode == 2 ? 0xc0ffee : input + 13),
                  "midstop exits before tail, final/unknown stop preserve scalar liveness");
            Check(state.memory_participant.active.load() == 0, "no-phi exit restores registered lease");
            Check(state.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0, "no-phi exit restores ownership");
        }
    }
}

void PhiTranslateExceptionQualification() {
    const char* expected[] = {
        "RV64 phi CFG has an unbound edge",
        "RV64 phi must precede non-phi instructions at a CFG entry",
        "RV64 entry phi requires an immediate for the host-entry edge",
        "RV64 phi input must have the result width and a local definition",
        "RV64 phi input must have the result width and a local definition",
        "RV64 phi input count differs from incoming edge count",
    };
    for (u32 kind = 0; kind < std::size(expected); ++kind) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x2c00})};
        IntrusivePtr<ir::Block> other{new ir::Block(ir::Location{0x2c10})};
        ir::Assembler as{block.get()}, foreign{other.get()};
        if (kind == 0) {
            auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
            as.Goto(ir::BOOL{condition}); // Deliberately no BindLabel.
        } else {
            ir::Params incoming;
            ir::Value local{};
            if (kind == 1 || kind == 2 || kind == 4)
                local = as.LoadUniform(ir::Uniform{0, kind == 4 ? T::U8 : T::U64}).SetType(kind == 4 ? T::U8 : T::U64);
            if (kind == 2 || kind == 4) incoming.Push(local);
            else if (kind == 3) incoming.Push(foreign.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64));
            else incoming.Push(ir::Imm{u64{17}});
            if (kind == 5) incoming.Push(ir::Imm{u64{23}});
            auto phi = as.AddPhi(incoming).SetType(T::U64);
            if (kind == 2 || kind == 4) { block->RemoveInst(phi.Def()); block->InsertBefore(phi.Def(), local.Def()); }
        }
        block->SetTerminal(ir::terminal::ReturnToHost{});
        bool rejected{};
        try {
            rv::JitContext context;
            rv::JitTranslator translator{context};
            translator.Translate(block.get());
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()) == expected[kind];
            std::cout << "phi_exception kind=" << kind << " observed=" << error.what() << '\n';
        }
        Check(rejected, "real Translate preserves exact phi/CFG exception kind " + std::to_string(kind));
    }
}
} // namespace
} // namespace swift::tests::riscv

int main() {
    using namespace swift::tests::riscv;
    try {
        struct rlimit limit{}; getrlimit(RLIMIT_NOFILE, &limit);
        std::cout << "RLIMIT_NOFILE=" << limit.rlim_cur << '\n';
        OrdinaryAndForward(); ImplicitFlagsAndPairPseudo(); NestedTerminalAndCFGContract();
        SelfLambdaParamsAndForeign();
        EntryPhiAndLocalJoin();
        SingletonSelfEdgePhi();
        NoPhiTranslateFlowQualification();
        PhiTranslateExceptionQualification();
        std::cout << "OK " << checks << " checks\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
