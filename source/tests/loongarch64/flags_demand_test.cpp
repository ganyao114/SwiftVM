#include "test_support.h"

#include <bit>
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/module.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
namespace {
using T = ir::ValueType;
using F = ir::Flags;

struct DemandCompiled {
    Config config{TestConfig()};
    la::JitContext context;
    la::JitTranslator translator{context};
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    BlockFn fn{};
    DemandCompiled(ir::Block* block, bool enabled, backend::Module* owner = nullptr) {
        context.SetUniformForwarding(false);
        context.SetScalarRematerialization(false);
        context.SetFlagsDemand(enabled);
        if (owner) context.ConfigureModule(owner);
        translator.Translate(block);
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate flags demand comparison");
        context.Flush(*buffer);
        fn = reinterpret_cast<BlockFn>(buffer->exec_data);
    }
};

void Inputs(StateStorage& state, u64 left, u64 right, u64 flags) {
    state.Put(0, left); state.Put(8, right); state.Put(16, reinterpret_cast<u64>(state.state));
    state.state->host_cpu_flags = flags;
    state.state->current_loc = ir::Location{0xd000};
    state.state->prev_loc = ir::Location{0xcffc};
}

u64 Observe(u64 pointer) {
    return reinterpret_cast<backend::State*>(pointer)->host_cpu_flags;
}
u64 RequestSignal(u64 pointer) {
    reinterpret_cast<backend::State*>(pointer)->exit_request = u64{1} << 63;
    return 0;
}

void Compare(ir::Block* block, bool expect_reduction) {
    DemandCompiled optimized{block, true}, baseline{block, false};
    const auto& on = optimized.translator.Stats();
    const auto& off = baseline.translator.Stats();
    Check(off.flag_bits_requested == off.flag_bits_computed, "disabled demand computes the complete requested mask");
    Check(on.flag_bits_requested == off.flag_bits_requested, "demand preserves the original flag requests");
    if (expect_reduction) {
        Check(on.flag_bits_computed < off.flag_bits_computed &&
              optimized.context.CurrentBufferSize() < baseline.context.CurrentBufferSize(),
              "dead flag computations reduce actual emitted code");
    } else {
        Check(on.flag_bits_computed == off.flag_bits_computed,
              "observable flags remain computed at their original position");
    }
    u64 seed = 0x89f446c21ce14ec9ULL;
    for (u32 sample = 0; sample < 128; ++sample) {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
        const auto left = sample < 4 ? std::array<u64, 4>{0, UINT64_MAX, u64{1} << 63, 0x7fffffff}[sample] : seed;
        const auto right = sample < 4 ? std::array<u64, 4>{UINT64_MAX, 1, u64{1} << 63, 1}[sample] : std::rotl(seed, 29);
        const auto flags = std::rotl(seed, 11);
        StateStorage actual, control, reference;
        Inputs(actual, left, right, flags); Inputs(control, left, right, flags); Inputs(reference, left, right, flags);
        backend::interp::Interpreter interpreter{*reference.state, block};
        const auto halt = interpreter.Run();
        Check(optimized.fn(actual.state) == halt && baseline.fn(control.state) == halt &&
              actual.state->host_cpu_flags == reference.state->host_cpu_flags &&
              control.state->host_cpu_flags == reference.state->host_cpu_flags &&
              std::memcmp(actual.state->uniform_buffer_begin + 24, reference.state->uniform_buffer_begin + 24, 232) == 0 &&
              std::memcmp(control.state->uniform_buffer_begin + 24, reference.state->uniform_buffer_begin + 24, 232) == 0,
              "flags demand agrees with typed interpreter and disabled code, including unused canonical bits");
    }
    StateStorage abi;
    Inputs(abi, 17, 29, 0xf0000083);
    Check(CheckABI(optimized.fn, abi.state) == 1, "flags demand preserves LP64D ABI");
}

IntrusivePtr<ir::Block> Chain(T type, F final_mask, bool carry) {
    IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xd000}}};
    ir::Assembler as{block.get()};
    auto left = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
    auto right = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
    auto first = as.Add(left, ir::Operand{right}).SetType(type);
    as.SaveFlags(first, F::All);
    auto last = (carry ? as.Adc(first, ir::Operand{right}) : as.Sub(first, ir::Operand{left})).SetType(type);
    as.SaveFlags(last, final_mask);
    as.StoreUniform(ir::Uniform{32, type}, first);
    as.StoreUniform(ir::Uniform{40, type}, last);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

void ObservationBoundaries() {
    for (const auto* boundary : {"get", "helper", "retirement", "branch"}) {
        IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xd000}}};
        ir::Assembler as{block.get()};
        auto pointer = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        auto left = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto right = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        auto first = as.Add(left, ir::Operand{right}).SetType(T::U64);
        as.SaveFlags(first, F::All);
        if (std::string_view{boundary} == "get") {
            // GetFlags returns the whole canonical word despite this annotation.
            auto observed = as.GetFlags(first, F::Carry).SetType(T::U64);
            as.StoreUniform(ir::Uniform{24, T::U64}, observed);
        } else if (std::string_view{boundary} == "helper") {
            auto observed = as.CallHost(&Observe, pointer).SetType(T::U64);
            as.StoreUniform(ir::Uniform{24, T::U64}, observed);
        } else if (std::string_view{boundary} == "retirement") {
            as.AdvancePC(ir::Imm{u64{4}});
        } else {
            auto edge = as.Goto(as.TestZero(first));
            as.Nop(); as.BindLabel(edge);
        }
        auto last = as.Sub(left, ir::Operand{right}).SetType(T::U64);
        as.SaveFlags(last, F::All);
        as.StoreUniform(ir::Uniform{32, T::U64}, last);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compare(block.get(), false);
    }
}

void SignalAtRetirement() {
    IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xd000}}};
    ir::Assembler as{block.get()};
    auto pointer = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    auto left = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto right = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    as.CallHost(&RequestSignal, pointer);
    auto first = as.Add(left, ir::Operand{right}).SetType(T::U64);
    as.SaveFlags(first, F::All);
    as.AdvancePC(ir::Imm{u64{4}});
    auto last = as.Sub(left, ir::Operand{right}).SetType(T::U64);
    as.SaveFlags(last, F::All);
    as.StoreUniform(ir::Uniform{32, T::U64}, last);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    IntrusivePtr<ir::Block> prefix{new ir::Block{ir::Location{0xd000}}};
    ir::Assembler reference_as{prefix.get()};
    auto a = reference_as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto b = reference_as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    reference_as.SaveFlags(reference_as.Add(a, ir::Operand{b}).SetType(T::U64), F::All);
    prefix->SetTerminal(ir::terminal::ReturnToHost{});
    for (bool enabled : {false, true}) {
        DemandCompiled compiled{block.get(), enabled};
        for (u64 value : {u64{0}, UINT64_MAX, u64{1} << 63, u64{0x7654321}}) {
            StateStorage actual, reference;
            Inputs(actual, value, 1, 0x39abcdef); Inputs(reference, value, 1, 0x39abcdef);
            backend::interp::Interpreter interpreter{*reference.state, prefix.get()};
            interpreter.Run();
            Check(compiled.fn(actual.state) == HaltReason::Signal &&
                  actual.state->host_cpu_flags == reference.state->host_cpu_flags && actual.Get(32) == 0 &&
                  actual.state->current_loc.Value() == 0xd004 && actual.state->prev_loc.Value() == 0xd000,
                  "Signal observes full flags and exact PC of the retired instruction before an overwrite");
        }
    }
}

void LoopAndCarryRewrites() {
    IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xd000}}};
    ir::Assembler as{block.get()};
    auto left = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto right = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto count = as.LoadImm(ir::Imm{u64{7}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{64, T::U64}, count);
    auto current = as.LoadUniform(ir::Uniform{64, T::U64}).SetType(T::U64);
    auto* first_inst = current.Def();
    auto sum = as.Adc(left, ir::Operand{right}).SetType(T::U64);
    // SaveFlags reads the carry at its own position, not the ADC's old carry.
    as.SetCarry(right);
    as.SaveFlags(sum, F::All);
    auto next = as.Sbb(sum, ir::Operand{left}).SetType(T::U64);
    as.SaveFlags(next, F::All);
    as.StoreUniform(ir::Uniform{32, T::U64}, next);
    auto remaining = as.Sub(current, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{64, T::U64}, remaining);
    auto back = as.Goto(as.TestNotZero(remaining)); as.BindLabel(back);
    auto* label = &block->GetInstList().back();
    block->RemoveInst(label); block->InsertBefore(label, first_inst);
    block->SetTerminal(ir::terminal::ReturnToHost{}); block->ReIdInstr();
    Compare(block.get(), true);
}

void ReplacedFlagUpdates() {
    IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xd000}}};
    ir::Assembler as{block.get()};
    auto left = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto right = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    as.SetCarry(left); as.InvertCarry(); as.SetOverflow(right); as.ClearFlags(F::All);
    as.PublishFCmpFlags(left, ir::Imm{u64{0}});
    as.PublishSse42StrFlags(right, F::All);
    auto last = as.Add(left, ir::Operand{right}).SetType(T::U64);
    as.SaveFlags(last, F::All);
    as.StoreUniform(ir::Uniform{32, T::U64}, last);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compare(block.get(), true);
    DemandCompiled compiled{block.get(), true};
    Check(compiled.translator.Stats().flag_writes_elided >= 6,
          "replaced carry, overflow, clear and compare updates emit no flag write");
    for (bool inverted : {false, true}) {
        IntrusivePtr<ir::Block> partial{new ir::Block{ir::Location{0xd000}}};
        ir::Assembler update{partial.get()};
        auto input = update.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        update.PublishFCmpFlags(input, ir::Imm{u64(inverted)});
        auto snapshot = update.GetFlags(input, F::Carry).SetType(T::U64);
        update.StoreUniform(ir::Uniform{24, T::U64}, snapshot);
        update.ClearFlags(F::NZCV);
        partial->SetTerminal(ir::terminal::ReturnToHost{});
        Compare(partial.get(), false);
    }
}

void PolicyAndEvidence() {
    Check(FeatureIdFromName("loongarch_flags_demand") == FeatureId::loongarch_flags_demand,
          "flags demand has a named module feature");
    auto on = GetSvmConfig().GetFeatureSet(), off = on;
    on.loongarch_flags_demand = true; off.loongarch_flags_demand = false;
    Check(HashFeatureSet(on) != HashFeatureSet(off), "demand ON/OFF changes the cache fingerprint");
    auto block = Chain(T::U64, F::All, true);
    backend::AddressSpace space{TestConfig()};
    for (bool enabled : {false, true}) {
        backend::ModuleConfig policy;
        policy.feature_overrides.Set(FeatureId::loongarch_flags_demand, enabled);
        auto module = std::make_shared<backend::Module>(space, 0, 1u << 20, policy);
        DemandCompiled compiled{block.get(), !enabled, module.get()};
        Check(compiled.context.FlagsDemandEnabled() == enabled, "module policy overrides preconfigured demand");
        const auto& stats = compiled.translator.Stats();
        Check((stats.flag_bits_computed < stats.flag_bits_requested) == enabled,
              "module policy selects the actual emitted flag computations");
        std::cout << "FLAGS_DEMAND {\"enabled\":" << (enabled ? "true" : "false")
                  << ",\"emitted_bytes\":" << compiled.context.CurrentBufferSize()
                  << ",\"requested_bits\":" << stats.flag_bits_requested
                  << ",\"computed_bits\":" << stats.flag_bits_computed
                  << ",\"elided_writes\":" << stats.flag_writes_elided << "}\n";
    }
}
}  // namespace

void FlagsDemand() {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) {
        for (bool carry : {false, true}) {
            auto block = Chain(type, F::All, carry); Compare(block.get(), true);
            block = Chain(type, F::NZ, carry); Compare(block.get(), true);
        }
    }
    ObservationBoundaries(); SignalAtRetirement(); LoopAndCarryRewrites(); ReplacedFlagUpdates(); PolicyAndEvidence();
    std::cout << "PASS flags demand, carry inputs, partial replacement, callbacks, CFG loops and Signal retirement\n";
}
}  // namespace swift::tests::loongarch64
