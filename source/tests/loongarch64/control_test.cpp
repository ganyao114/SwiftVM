#include "test_support.h"

#include <atomic>
#include <chrono>
#include <thread>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {

using T = ir::ValueType;

void ControlFlow() {
    // Malformed CFG must fail before any executable prefix is produced.
    for (bool duplicate : {false, true}) {
        IntrusivePtr<ir::Block> invalid{new ir::Block(ir::Location{0x2f00})};
        ir::Assembler malformed{invalid.get()};
        auto yes = malformed.LoadImm(ir::Imm{u64{1}}).SetType(T::U8);
        auto edge = malformed.Goto(ir::BOOL{yes});
        if (duplicate) { malformed.BindLabel(edge); malformed.BindLabel(edge); }
        invalid->SetTerminal(ir::terminal::ReturnToHost{});
        la::JitContext context; la::JitTranslator translator{context};
        bool rejected{};
        try { translator.Translate(invalid.get()); } catch (const std::runtime_error&) { rejected = true; }
        Check(rejected && context.CurrentBufferSize() == 0, "unbound or duplicate CFG labels reject before emission");
    }
    for (bool invert : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x3000})};
        ir::Assembler as{block.get()};
        auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        auto anchor = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        auto edge = invert ? as.NotGoto(ir::BOOL{condition}) : as.Goto(ir::BOOL{condition});
        for (u64 i = 0; i < 24; ++i) {
            auto temporary = as.Add(anchor, ir::Operand{ir::Imm{i + 1}}).SetType(T::U64);
            as.StoreUniform(ir::Uniform{16, T::U64}, temporary);
        }
        as.InvertCarry();
        as.BindLabel(edge);
        auto value = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        auto sum = as.Add(anchor, ir::Operand{value}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{24, T::U64}, sum);
        auto flags = as.GetFlags(sum, ir::Flags::All).SetType(T::U64);
        as.StoreUniform(ir::Uniform{32, T::U64}, flags);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled cached{block.get()}, uncached{block.get(), false};
        Check(cached.translator.Stats().helpers == 0, "forward CFG and flags use native lowering");
        for (u64 input : {u64{0}, u64{1}}) {
            StateStorage a, b;
            for (auto* storage : {&a, &b}) {
                storage->Put(0, input); storage->Put(8, 71); storage->Put(16, 11);
                storage->state->host_cpu_flags = u64{1} << 53;
            }
            const bool taken = invert ? input == 0 : input != 0;
            Check(cached.fn(a.state) == HaltReason::CallHost && uncached.fn(b.state) == HaltReason::CallHost,
                  "forward join halt");
            Check(a.Get(24) == (taken ? 82 : 166) && b.Get(24) == a.Get(24),
                  "join reads uniform state from the selected path");
            Check(a.Get(32) == ((u64{1} << 53) | (taken ? 0 : u64{1} << 29)) && b.Get(32) == a.Get(32),
                  "join observes flags from the selected path");
        }
    }
    IntrusivePtr<ir::Block> loop{new ir::Block(ir::Location{0x3100})};
    ir::Assembler as{loop.get()};
    auto count = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto accumulated = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto sum = as.Add(accumulated, ir::Operand{count}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{8, T::U64}, sum);
    auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{0, T::U64}, next);
    auto running = as.TestNotZero(next);
    auto edge = as.Goto(running);
    as.BindLabel(edge);
    auto* label = &loop->GetInstList().back();
    loop->RemoveInst(label);
    loop->InsertBefore(label, count.Def());
    as.StoreUniform(ir::Uniform{16, T::U64}, sum);
    loop->SetTerminal(ir::terminal::ReturnToHost{});
    loop->ReIdInstr();
    Compiled cached{loop.get()}, uncached{loop.get(), false};
    for (u64 initial : {u64{1}, u64{2}, u64{9}, u64{31}, u64{1024}}) {
        StateStorage a, b;
        a.Put(0, initial); b.Put(0, initial); a.Put(8, 17); b.Put(8, 17);
        Check(cached.fn(a.state) == HaltReason::CallHost && uncached.fn(b.state) == HaltReason::CallHost,
              "finite backward loop halt");
        Check(a.Get(0) == 0 && b.Get(0) == 0 && a.Get(8) == 17 + initial * (initial + 1) / 2 &&
              b.Get(8) == a.Get(8) && a.Get(16) == a.Get(8), "loop carries state and exits with the final iteration values");
    }
    for (bool taken : {false, true}) {
        StateStorage state;
        IntrusivePtr<ir::Block> retirement{new ir::Block(ir::Location{0x3200})};
        ir::Assembler retire{retirement.get()};
        auto pointer = retire.LoadImm(ir::Imm{reinterpret_cast<u64>(state.state)}).SetType(T::U64);
        auto request = +[](u64 pointer, u64, u64) -> u64 {
            auto* state = reinterpret_cast<backend::State*>(pointer);
            std::atomic_ref<u64>(state->exit_request).store(1, std::memory_order_release);
            return 0;
        };
        (void)retire.CallHost(request, pointer).SetType(T::U64);
        auto condition = retire.LoadImm(ir::Imm{u64(taken)}).SetType(T::U8);
        auto edge = retire.Goto(ir::BOOL{condition});
        auto skipped = retire.LoadImm(ir::Imm{u64{23}}).SetType(T::U64);
        retire.StoreUniform(ir::Uniform{0, T::U64}, skipped);
        retire.BindLabel(edge);
        auto retired = retire.LoadImm(ir::Imm{u64{42}}).SetType(T::U64);
        retire.StoreUniform(ir::Uniform{8, T::U64}, retired);
        retire.AdvancePC(ir::Imm{u64{4}});
        auto forbidden = retire.LoadImm(ir::Imm{u64{99}}).SetType(T::U64);
        retire.StoreUniform(ir::Uniform{16, T::U64}, forbidden);
        retirement->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{retirement.get()};
        state.state->current_loc = ir::Location{0x3200};
        const auto halt = compiled.fn(state.state);
        Check(halt == HaltReason::CodeMiss && state.state->current_loc.Value() == 0x3204,
              "SMC request exits at instruction retirement after the local CFG taken=" + std::to_string(taken) +
              " halt=" + std::to_string(u32(halt)) + " pc=" + std::to_string(state.state->current_loc.Value()) +
              " request=" + std::to_string(state.state->exit_request) + " values=" + std::to_string(state.Get(0)) +
              "/" + std::to_string(state.Get(8)) + "/" + std::to_string(state.Get(16)));
        Check(state.Get(0) == (taken ? 0 : 23) && state.Get(8) == 42 && state.Get(16) == 0,
              "SMC polling completes selected instruction effects exactly once before the next instruction");
    }
    std::cout << "PASS native forward joins, flags joins, NotGoto and finite local loops\n";
}

void FunctionCFG() {
    auto config = TestConfig();
    backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    void* entry{};
    {
        ir::HIRBuilder builder{1, true};
        auto* function = builder.AppendFunction(ir::Location{0x4000});
        auto input = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
        auto condition = function->TestZero<ir::U8>(input);
        builder.If(ir::terminal::If{ir::BOOL{condition},
            ir::terminal::LinkBlock{ir::Location{0x4100}}, ir::terminal::LinkBlock{ir::Location{0x4200}}});
        builder.SetCurBlock(ir::Location{0x4100});
        auto left = function->Add<ir::U64>(input, ir::Operand{ir::Imm{u64{31}}});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0x4300}});
        builder.SetCurBlock(ir::Location{0x4200});
        auto right = function->Sub<ir::U64>(input, ir::Operand{ir::Imm{u64{7}}});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0x4300}});
        builder.SetCurBlock(ir::Location{0x4300});
        ir::Params params; params.Push(left); params.Push(right);
        auto result = function->AddPhi<ir::U64>(params);
        function->StoreUniform(ir::Uniform{8, T::U64}, result);
        function->EndBlock(ir::terminal::CheckHalt{ir::terminal::ReturnToHost{}});
        function->EndFunction();
        entry = backend::TranslateIR(module, function);
        Check(entry != nullptr, "compile cross-block SSA diamond with phi");
    }
    const auto fn = reinterpret_cast<BlockFn>(entry);
    for (u64 input : {u64{0}, u64{19}, UINT64_MAX}) {
        StateStorage state; state.Put(0, input);
        Check(fn(state.state) == HaltReason::CallHost && state.Get(8) == (input == 0 ? 31 : input - 7),
              "diamond phi remains correct after HIR builder destruction");
        Check(state.state->current_loc == ir::Location{0x4300}, "function publishes the executing guest block");
        Check(CheckABI(fn, state.state) == 1, "whole CFG preserves LP64D");
    }
    {
        ir::HIRBuilder builder{1, true};
        auto* function = builder.AppendFunction(ir::Location{0x4400});
        auto initial = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0x4500}});
        builder.SetCurBlock(ir::Location{0x4500});
        ir::Params inputs; inputs.Push(initial); inputs.Push(initial);
        auto count = function->AddPhi<ir::U64>(inputs);
        auto next = function->Sub<ir::U64>(count, ir::Operand{ir::Imm{u64{1}}});
        ir::Params loop_inputs; loop_inputs.Push(initial); loop_inputs.Push(next);
        count.Def()->SetArg(0, loop_inputs);
        auto complete = function->TestZero<ir::U8>(next);
        builder.If(ir::terminal::If{ir::BOOL{complete},
            ir::terminal::LinkBlock{ir::Location{0x4600}}, ir::terminal::LinkBlock{ir::Location{0x4500}}});
        builder.SetCurBlock(ir::Location{0x4600});
        function->StoreUniform(ir::Uniform{8, T::U64}, count);
        function->EndBlock(ir::terminal::ReturnToHost{});
        function->EndFunction();
        entry = backend::TranslateIR(module, function);
        Check(entry != nullptr, "compile loop-carried phi");
    }
    for (u64 input : {u64{1}, u64{2}, u64{127}, u64{10000}}) {
        StateStorage state; state.Put(0, input);
        Check(reinterpret_cast<BlockFn>(entry)(state.state) == HaltReason::CallHost && state.Get(8) == 1,
              "whole CFG backedge chooses and moves the loop-carried phi");
    }
    IntrusivePtr<ir::Block> first{new ir::Block(ir::Location{0x4700})};
    first->SetTerminal(ir::terminal::LinkBlock{ir::Location{0x4800}});
    IntrusivePtr<ir::Block> last{new ir::Block(ir::Location{0x4800})};
    ir::Assembler last_as{last.get()};
    auto answer = last_as.LoadImm(ir::Imm{u64{42}}).SetType(T::U64);
    last_as.StoreUniform(ir::Uniform{0, T::U64}, answer);
    last->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(first.get()) && module->Push(last.get()), "publish two Runtime blocks");
    Check(backend::TranslateIR(module, first) && backend::TranslateIR(module, last), "Runtime selects LoongArch compiler");
    Runtime runtime{&space}; runtime.SetLocation(0x4700);
    Check(runtime.Run() == HaltReason::CallHost, "LoongArch dispatcher executes multiple blocks");
    u64 output{}; std::memcpy(&output, runtime.GetUniformBuffer().data(), 8);
    Check(output == 42 && runtime.GetLocation() == 0x4800, "dispatcher state and exit location");
    runtime.SignalInterrupt();
    Check(runtime.Run() == HaltReason::Signal, "sticky interrupt is observed before guest entry");
    runtime.ClearInterrupt();
    runtime.SetLocation(0xdead);
    Check(runtime.Run() == HaltReason::CodeMiss, "dispatcher returns a cache miss after interrupt reuse");
    std::cout << "PASS cross-block diamonds, loop-carried phi, Runtime dispatch and sticky signals\n";
}

}  // namespace swift::tests::loongarch64
