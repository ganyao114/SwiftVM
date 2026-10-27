#include "runtime/compile/workspace.h"
#include "test_support.h"

#include <bit>
#include <sys/resource.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/riscv64/jit/compile.h"
#include "runtime/compile/builder/ir_builder.h"
#include "runtime/compile/passes/passes.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::tests::riscv;
namespace be = swift::runtime::backend;
using T = ir::ValueType;
using O = ir::OpCode;

namespace {
u64 Helper() { return 7; }

unsigned SaveCount(ir::Block* block) {
    unsigned count{};
    for (const auto& inst : block->GetInstList()) count += inst.GetOp() == O::SaveFlags;
    return count;
}

IntrusivePtr<ir::Block> Program(u64 guest, T type, ir::Flags mask, ir::Cond condition,
                              const std::string& boundary = {}) {
    ::swift::runtime::compile::CompileWorkspace compilation_workspace;
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{guest})};
    compile::IrBuilder as{block.get()};
    auto left = as.LoadUniform(type, ir::Uniform{0, type});
    auto right = as.LoadUniform(type, ir::Uniform{8, type});
    auto dead = as.Add(type, left, ir::Operand{right});
    as.SaveFlags(dead, ir::Flags::All);
    auto dead_again = as.Add(type, left, ir::Operand{ir::Imm{u64{3}}});
    as.SaveFlags(dead_again, ir::Flags::All);
    // Any inserted boundary must prevent elimination even of earlier writes
    // that would be dead in the corresponding straight-line integer unit.
    if (boundary == "memory") {
        auto value = as.LoadMemory(type, ir::Operand{left});
        as.StoreUniform(ir::Uniform{112, type}, value);
    } else if (boundary == "helper") {
        auto value = ir::Value{block->AppendInst(
                O::CallLambda, T::U64, ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}})};
        as.StoreUniform(ir::Uniform{112, T::U64}, value);
    } else if (boundary == "label") {
        auto edge = as.Goto(ir::BOOL{as.TestNotZero(T::U8, left)});
        as.BindLabel(edge);
    } else if (boundary == "div" || boundary == "adc" || boundary == "sbb") {
        const auto op = boundary == "div" ? O::Div : boundary == "adc" ? O::Adc : O::Sbb;
        auto value = ir::Value{block->AppendInst(op, type, left, ir::Operand{right})};
        as.StoreUniform(ir::Uniform{112, type}, value);
    } else if (boundary == "fault") {
        block->AppendInst(O::CheckMemoryAlignment, left, ir::Imm{u64{8}});
    } else if (boundary == "barrier") {
        block->AppendInst(O::MemoryBarrierTSO);
    } else if (boundary == "vector") {
        auto value = as.LoadUniform(T::V128, ir::Uniform{128, T::V128});
        as.StoreUniform(ir::Uniform{144, T::V128}, value);
    } else if (boundary == "uniform-range") {
        auto value = as.LoadUniform(T::U64, ir::Uniform{4095, T::U64});
        as.StoreUniform(ir::Uniform{112, T::U64}, value);
    } else if (boundary == "uniform-load-width") {
        auto value = as.LoadUniform(T::U64, ir::Uniform{4095, T::U64});
        // An imported declaration mismatch bypasses typed construction, just
        // as the store-width case below; admission must inspect native width.
        compile::RewriteContext{block.get(), compilation_workspace, nullptr, false}.SetArg(
                block.get(), value.Def(), 0, ir::Uniform{4095, T::U8});
        as.StoreUniform(ir::Uniform{112, T::U64}, value);
    } else if (boundary == "uniform-store-width") {
        as.StoreUniform(ir::Uniform{4095, T::U64}, left);
        // Imported IR can carry inconsistent declarations. The shared IR
        // contract must reject this before native span checks or emission.
        compile::RewriteContext{block.get(), compilation_workspace, nullptr, false}.SetArg(
                block.get(), &block->GetInstList().back(), 0, ir::Uniform{4095, T::U8});
    } else if (boundary == "undefined-logical-flags" || boundary == "undefined-mul-flags") {
        const auto op = boundary == "undefined-logical-flags" ? O::Xor : O::Mul;
        auto value = ir::Value{block->AppendInst(op, type, left, ir::Operand{right})};
        as.SaveFlags(value, ir::Flags::All);
    }
    auto sum = as.Add(type, left, ir::Operand{right});
    as.SaveFlags(sum, ir::Flags::All);
    as.StoreUniform(ir::Uniform{24, T::U8}, as.TestFlags(mask));
    // GetFlags returns the complete word even when its IR argument is narrow.
    as.StoreUniform(ir::Uniform{32, T::U64}, as.GetFlags(T::U64, sum, mask));
    auto decrement = as.Sub(type, sum, ir::Operand{ir::Imm{u64{1}}});
    as.SaveFlags(decrement, ir::Flags::All & ~ir::Flags::Carry);
    as.StoreUniform(ir::Uniform{40, T::U64}, as.GetFlags(T::U64, decrement, ir::Flags::All));
    as.StoreUniform(ir::Uniform{48, T::U8}, as.TestFlags(mask));
    as.StoreUniform(ir::Uniform{56, T::U8}, as.TestNotFlags(mask));
    as.StoreUniform(ir::Uniform{64, T::U8}, as.CondSet(T::U8, condition));
    as.StoreUniform(ir::Uniform{72, T::U8}, as.LocalCondSet(T::U8, condition));
    as.StoreUniform(ir::Uniform{80, type}, as.CondSelect(type, condition, left, right));
    as.StoreUniform(ir::Uniform{88, type}, decrement);
    if (boundary == "poll") block->SetTerminal(ir::terminal::CheckHalt{ir::terminal::ReturnToHost{}});
    else block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

struct Environment {
    Config config{TestConfig()};
    be::AddressSpace space{config};
    std::shared_ptr<be::Module> module;
    explicit Environment(Optimizations opts) {
        module = space.MapModule(0x10000, 0x20000,
                                 {.read_only = true, .optimizations = opts});
    }
};

void PositiveReferences(Environment& original, Environment& optimized) {
    u64 seed = 0xba5afcf0836174c9ULL;
    unsigned programs{};
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) {
        const u32 bits = ir::GetValueSizeByte(type) * 8;
        const u64 low_mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        for (u32 subset = 0; subset < 64; ++subset) {
            const auto mask = static_cast<ir::Flags>((subset & 31) | ((subset & 32) << 1));
            const auto cond = static_cast<ir::Cond>(subset % 16);
            const auto guest = 0x10000 + 16 * programs++;
            auto reference_ir = Program(guest, type, mask, cond);
            auto candidate_ir = Program(guest, type, mask, cond);
            if (programs == 1) std::cout << "first reference program prepared\n";
            auto reference = reinterpret_cast<BlockFn>(rv::CompileBlock(original.module, reference_ir.get()));
            if (programs == 1) std::cout << "first original program compiled\n";
            auto candidate = reinterpret_cast<BlockFn>(rv::CompileBlock(optimized.module, candidate_ir.get()));
            if (programs == 1) std::cout << "first candidate program compiled\n";
            Check(reference && candidate, "compile both pure scalar reference variants");
            Check(SaveCount(reference_ir.get()) == 4 && SaveCount(candidate_ir.get()) == 2,
                  "admit pure block but retain intermediate complete snapshot and final carry inheritance");
            for (u32 sample = 0; sample < 32; ++sample) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                u64 left = seed, right = std::rotl(seed, 19);
                if (sample == 0) { left = 0; right = 0; }
                if (sample == 1) { left = low_mask; right = 1; }
                if (sample == 2) { left = low_mask >> 1; right = 1; }
                if (sample == 3) { left = u64{1} << (bits - 1); right = left; }
                StateStorage a, b, expected;
                for (auto* storage : {&a, &b, &expected}) {
                    storage->Put(0, left); storage->Put(8, right);
                    storage->state->host_cpu_flags = seed;
                }
                be::interp::Interpreter interpreter{*expected.state, reference_ir.get()};
                const auto halt = interpreter.Run();
                Check(reference(a.state) == halt && candidate(b.state) == halt,
                      "optimized and original flags halt match independent interpreter");
                Check(std::memcmp(a.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 96) == 0 &&
                      std::memcmp(b.state->uniform_buffer_begin, expected.state->uniform_buffer_begin, 96) == 0,
                      "all masks, all conditions, whole-word GetFlags and DEC outputs match reference");
                Check(a.state->host_cpu_flags == expected.state->host_cpu_flags &&
                      b.state->host_cpu_flags == expected.state->host_cpu_flags,
                      "full final flags including non-arithmetic bits match reference");
                Check((b.Get(40) & (u64{1} << 29)) == (b.Get(32) & (u64{1} << 29)),
                      "DEC preserves CF from preceding ADD across both carry outcomes");
                const auto lhs = left & low_mask, rhs = right & low_mask;
                const auto sum = (lhs + rhs) & low_mask;
                const auto result = (sum - 1) & low_mask;
                const bool carry = bits == 64 ? (lhs + rhs) < lhs : ((lhs + rhs) >> bits) != 0;
                const bool overflow = (((sum ^ 1) & (sum ^ result)) >> (bits - 1)) & 1;
                constexpr u64 arithmetic_bits = 255 | (u64{15} << 28) | (u64{1} << 26);
                const auto expected_flags = (seed & ~arithmetic_bits) | u8(result) |
                        (u64(carry) << 29) | (u64(overflow) << 28) |
                        (u64(result == 0) << 30) | (((result >> (bits - 1)) & 1) << 31) |
                        (u64(((sum ^ 1 ^ result) & 16) != 0) << 26);
                Check(b.state->host_cpu_flags == expected_flags && b.Get(88) == result,
                      "independent C++ full flags oracle including overflow, AF, raw parity and preserved bits");
            }
        }
    }
    std::cout << "PASS 256 admitted pure blocks, 8192 full-state independent references\n";
}

void MalformedScalarALU() {
    Environment environment{Optimizations::FlagElimination | Optimizations::ConstantFolding |
                             Optimizations::DeadCodeRemove};
    unsigned cases{};
    for (const bool function_mode : {false, true})
    for (const auto op : {O::Add, O::Sub, O::Adc, O::Sbb, O::Mul, O::Div, O::And, O::Or,
                          O::Xor, O::AndNot, O::Not, O::Neg, O::MulSub, O::SignedDiv64,
                          O::MulHigh, O::Div128})
    for (const auto type : {T::V64, T::V128})
    for (int slot = -1; slot < ir::Inst::max_args; ++slot) {
        ir::HIRBuilder hir{1, false, FeatureSet{}};
        auto* function = function_mode ? hir.AppendFunction(ir::Location{0x13000},
                                                             ir::Location{0x13001}) : nullptr;
        IntrusivePtr<ir::Block> block;
        if (!function) block = new ir::Block{ir::Location{0x13000}};
        compile::IrBuilder as = function ? compile::IrBuilder{&hir} : compile::IrBuilder{block.get()};
        const auto value = as.LoadUniform(ir::Uniform{0, T::U64});
        const ir::Inst* invalid{};
        switch (op) {
            case O::Neg: invalid = as.AppendInst(op, T::U64, value); break;
            case O::MulSub: invalid = as.AppendInst(op, T::U64, value, value, value); break;
            case O::SignedDiv64: invalid = as.AppendInst(op, T::U64, value, value); break;
            case O::MulHigh: invalid = as.AppendInst(op, T::U64, value, value, ir::Imm{u64{1}}); break;
            case O::Div128:
                invalid = as.AppendInst(op, T::U64, value, value, value, ir::Imm{u64{1}}); break;
            default:
                invalid = as.AppendInst(op, T::U64, value,
                                       ir::Operand{ir::DataClass{ir::Imm{u64{1}}},
                                                   ir::DataClass{value}});
        }
        auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock() : block.get();
        as.ReturnToHost();
        if (slot >= 0 && !invalid->ArgAt(slot).IsValue() && !invalid->ArgAt(slot).IsImm()) continue;
        {
            compile::CompileWorkspace workspace;
            compile::RewriteContext rewrite{body, workspace};
            if (slot < 0) rewrite.Change(body, invalid, [&](ir::Inst& inst) { inst.SetReturn(type); });
            else if (invalid->ArgAt(slot).IsValue())
                rewrite.SetArg(body, invalid, slot, ir::Value{value}.SetCastType(type));
            else rewrite.SetArg(body, invalid, slot, ir::Imm{u64{1}, type});
        }
        if (function) { function->ComputeRPO(); function->IdByRPO(); }
        else body->ReIdInstr();
        rv::JitContext context{false, rv::HostFeatures{}};
        rv::JitTranslator translator{context};
        bool direct_rejected{};
        try { translator.Translate(body); }
        catch (const std::runtime_error& error) {
            direct_rejected = std::string{error.what()} == "RV64 IR has an invalid result or arguments";
        }
        Check(direct_rejected && context.CurrentBufferSize() == 0,
              "scalar ALU domain rejected before direct RV code/frame emission");
        bool module_rejected{};
        try {
            if (function) (void)rv::CompileFunctions(environment.module, std::span{&function, 1});
            else (void)rv::CompileBlock(environment.module, block.get());
        } catch (const std::runtime_error& error) {
            module_rejected = std::string{error.what()} == "RV64 IR has invalid arguments";
        }
        Check(module_rejected && environment.space.GetCodeCache(ir::Location{0x13000}) == nullptr,
              "Block/Function rejects unused malformed scalar ALU before optimization/publication");
        ++cases;
    }
    std::cout << "PASS scalar ALU malformed domains cases=" << cases
              << " Block/Function direct/module before emission/optimization\n";
}

void MalformedTerminals() {
    Environment environment{Optimizations::FlagElimination | Optimizations::ConstantFolding |
                             Optimizations::DeadCodeRemove};
    const std::array<const char*, 7> names{
            "condition-16", "nested-unselected-condition-255", "empty-vector-switch",
            "fp-storage-switch-key-v32", "vector-case", "fp-storage-case-v32",
            "fp-storage-if-view-v32"};
    unsigned programs{};
    for (unsigned unit = 0; unit < 3; ++unit) {
        for (unsigned shape = 0; shape < names.size(); ++shape) {
            const u64 guest = 0x14000 + unit * 0x200 + shape * 0x10;
            const auto detail = std::string{names[shape]} + " unit=" + std::to_string(unit);
            ir::HIRBuilder hir{1, false, FeatureSet{}};
            auto* function = unit ? hir.AppendFunction(ir::Location{guest},
                                                         ir::Location{guest + 2}) : nullptr;
            IntrusivePtr<ir::Block> block;
            if (!function) block = new ir::Block{ir::Location{guest}};
            compile::IrBuilder as = function ? compile::IrBuilder{&hir}
                                             : compile::IrBuilder{block.get()};
            const auto scalar = as.LoadUniform(ir::Uniform{0, T::U64});
            ir::Value domain = scalar;
            if (shape == 2) domain = as.LoadUniform(ir::Uniform{16, T::V128});
            else if (shape == 3 || shape == 6)
                // FP scalar payloads use V32 storage, distinct from V128.
                domain = as.LoadUniform(ir::Uniform{16, T::V32});
            if (unit == 2) {
                // Keep a genuine cross-block SSA input independent of the
                // malformed terminal. FunctionPlan would erase an empty
                // Switch's selector while lowering it to a dispatcher exit.
                auto* root = function->GetCurrentBlock();
                auto* tail = function->AppendBlock(ir::Location{guest + 1});
                function->EndBlock(ir::terminal::LinkBlock{ir::Location{guest + 1}});
                function->AddEdge(root, tail);
                function->SetCurBlock(tail);
                as.StoreUniform(ir::Uniform{48, T::U64}, scalar);
            }
            auto* body = function ? function->EditBlock(function->GetCurrentBlock()).GetBlock()
                                  : block.get();
            const auto* dead = as.AppendInst(O::Add, T::U64, scalar,
                                            ir::Operand{ir::Imm{u64{1}}});
            ir::Terminal terminal = ir::terminal::ReturnToHost{};
            switch (shape) {
                case 0:
                    terminal = ir::terminal::Condition{static_cast<ir::Cond>(16),
                            ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}};
                    break;
                case 1: {
                    const auto false_value = as.LoadImm(T::U8, ir::Imm{u8{0}});
                    const auto zero = as.LoadImm(T::U64, ir::Imm{u64{0}});
                    terminal = ir::terminal::CheckHalt{ir::terminal::If{
                            ir::BOOL{false_value}, ir::terminal::ReturnToHost{},
                            ir::terminal::Condition{ir::Cond::AL,
                                    ir::terminal::Switch{zero,
                                            {{ir::Imm{u8{0}}, ir::terminal::ReturnToHost{}},
                                             {ir::Imm{u16{1}}, ir::terminal::Condition{
                                                     static_cast<ir::Cond>(255),
                                                     ir::terminal::ReturnToHost{},
                                                     ir::terminal::ReturnToDispatch{}}}}},
                                    ir::terminal::ReturnToDispatch{}}}};
                    break;
                }
                case 2:
                    terminal = ir::terminal::Switch{domain, {}};
                    break;
                case 3:
                    terminal = ir::terminal::Switch{domain,
                            {{ir::Imm{u64{0}}, ir::terminal::ReturnToHost{}}}};
                    break;
                case 4:
                case 5:
                    terminal = ir::terminal::Switch{scalar,
                            {{ir::Imm{u8{0}}, ir::terminal::ReturnToHost{}},
                             {ir::Imm{u64{1}, shape == 4 ? T::V64 : T::V32},
                                     ir::terminal::ReturnToDispatch{}}}};
                    break;
                case 6: {
                    ir::BOOL condition{domain};
                    (void)condition.SetCastType(T::V32);
                    terminal = ir::terminal::If{condition, ir::terminal::ReturnToHost{},
                                                ir::terminal::ReturnToDispatch{}};
                    break;
                }
            }
            if (function) {
                function->EndBlock(std::move(terminal));
                function->EndFunction(); function->ComputeRPO(); function->IdByRPO();
            } else {
                body->SetTerminal(std::move(terminal)); body->ReIdInstr();
            }
            const auto count = body->GetInstList().size();
            rv::JitContext context{false, rv::HostFeatures{}};
            rv::JitTranslator translator{context};
            bool direct_rejected{};
            try { translator.Translate(body); }
            catch (const std::runtime_error& error) {
                direct_rejected = std::string{error.what()} == "RV64 terminal has invalid arguments";
            }
            Check(direct_rejected && context.CurrentBufferSize() == 0,
                  "terminal rejected before direct RV code/frame emission: " + detail);
            bool module_rejected{};
            try {
                if (function) (void)rv::CompileFunctions(environment.module, std::span{&function, 1});
                else (void)rv::CompileBlock(environment.module, block.get());
            } catch (const std::runtime_error& error) {
                module_rejected = std::string{error.what()} == "RV64 terminal has invalid arguments";
            }
            Check(module_rejected && environment.space.GetCodeCache(ir::Location{guest}) == nullptr &&
                          environment.space.GetCodeCache(body->GetStartLocation()) == nullptr &&
                          body->GetJitCache().jit_state != be::JitState::Cached,
                  "terminal rejected before Block/Function publication: " + detail);
            bool retained{};
            for (const auto& inst : body->GetInstList())
                if (&inst == dead) retained = inst.GetOp() == O::Add && inst.GetUses(false) == 0;
            Check(body->GetInstList().size() == count && retained,
                  "terminal rejection precedes folding/dead-instruction removal: " + detail);
            ++programs;
        }
    }
    std::cout << "PASS " << programs
              << " malformed terminal programs, direct/Block/single/cross-block preflight\n";
}

void TerminalReferences() {
    Environment environment{Optimizations::FlagElimination | Optimizations::ConstantFolding |
                             Optimizations::DeadCodeRemove};
    unsigned executions{};
    for (unsigned program = 0; program < 6; ++program) {
        const u64 guest = 0x16000 + program * 0x10;
        IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{guest}}};
        compile::IrBuilder as{block.get()};
        const auto marker = as.LoadImm(T::U64, ir::Imm{u64{0x5a17}});
        as.StoreUniform(ir::Uniform{64, T::U64}, marker);
        if (program == 0) {
            as.SetLocation(ir::Lambda{ir::Imm{guest + 1}});
            block->SetTerminal(ir::terminal::Invalid{});
        } else if (program == 1 || program == 2) {
            const auto selector = as.LoadUniform(ir::Uniform{0, T::U64});
            if (program == 1) block->SetTerminal(ir::terminal::Switch{selector, {}});
            else block->SetTerminal(ir::terminal::Switch{selector,
                    {{ir::Imm{u16{7}}, ir::terminal::ReturnToHost{}},
                     {ir::Imm{s32{7}}, ir::terminal::ReturnToDispatch{}},
                     {ir::Imm{s8{-1}}, ir::terminal::ReturnToHost{}},
                     {ir::Imm{s64{-1}}, ir::terminal::ReturnToHost{}}}});
        } else if (program == 3) {
            const auto value = as.LoadUniform(ir::Uniform{0, T::U64});
            ir::BOOL condition{value};
            (void)condition.SetCastType(T::S64);
            block->SetTerminal(ir::terminal::If{condition, ir::terminal::ReturnToHost{},
                                               ir::terminal::ReturnToDispatch{}});
        } else {
            block->SetTerminal(ir::terminal::Condition{
                    program == 4 ? ir::Cond::AL : ir::Cond::NV,
                    ir::terminal::ReturnToHost{}, ir::terminal::ReturnToDispatch{}});
        }
        auto fn = reinterpret_cast<BlockFn>(rv::CompileBlock(environment.module, block.get()));
        Check(fn && environment.space.GetCodeCache(ir::Location{guest}) == reinterpret_cast<void*>(fn),
              "publish legal terminal program " + std::to_string(program));
        const std::array<u64, 4> inputs = program == 2
                ? std::array<u64, 4>{7, 255, UINT64_MAX, 99}
                : program == 3 ? std::array<u64, 4>{0, u64{1} << 32, u64{1} << 63, 0}
                               : std::array<u64, 4>{0, u64{15} << 28, 0, 0};
        const unsigned samples = program == 2 ? 4 : program == 3 ? 3 : program >= 4 ? 2 : 1;
        for (unsigned sample = 0; sample < samples; ++sample) {
            StateStorage storage;
            storage.Put(0, inputs[sample]);
            storage.state->host_cpu_flags = inputs[sample];
            storage.state->current_loc = ir::Location{guest};
            const bool host = program == 2 ? inputs[sample] != 99
                    : program == 3 ? inputs[sample] != 0 : program >= 4;
            Check(fn(storage.state) == (host ? HaltReason::CallHost : HaltReason::None) &&
                          storage.Get(64) == 0x5a17,
                  "native terminal result follows independent expected branch " +
                          std::to_string(program) + "/" + std::to_string(sample));
            if (program == 0)
                Check(storage.state->current_loc == ir::Location{guest + 1},
                      "ordinary Invalid terminal retains the explicit next location");
            ++executions;
        }
    }
    std::cout << "PASS 6 legal terminal programs, " << executions
              << " native Invalid/Switch/wide-If/AL/NV references\n";
}

void RejectedBoundaries(Environment& environment) {
    unsigned index{};
    for (const auto* boundary : {"memory", "helper", "poll", "label", "div", "adc", "sbb",
                                 "fault", "barrier", "vector", "uniform-range",
                                 "undefined-logical-flags", "undefined-mul-flags",
                                 "uniform-load-width", "uniform-store-width"}) {
        std::cout << "admission boundary " << boundary << '\n';
        auto block = Program(0x12000 + 16 * index++, T::U64, ir::Flags::All, ir::Cond::CS, boundary);
        const auto before = SaveCount(block.get());
        const bool invalid_span = std::string{boundary} == "uniform-range" ||
                std::string{boundary} == "uniform-load-width" || std::string{boundary} == "uniform-store-width";
        if (invalid_span) {
            bool specific{};
            try { (void)rv::CompileBlock(environment.module, block.get()); }
            catch (const std::runtime_error& error) {
                const auto expected = std::string{boundary} == "uniform-store-width"
                        ? "RV64 IR has invalid arguments"
                        : "RV64 scalar Uniform access exceeds configured state span";
                specific = std::string{error.what()} == expected;
            }
            Check(specific && block->GetJitCache().jit_state != be::JitState::Cached,
                  std::string{"explicit scalar admission rejection: "} + boundary);
        } else {
            Check(rv::CompileBlock(environment.module, block.get()) != nullptr,
                  std::string{"compile rejected-boundary fallback: "} + boundary);
        }
        Check(SaveCount(block.get()) == before,
              std::string{"preserve all writes at observation boundary: "} + boundary);
    }
    std::cout << "PASS rejected memory/helper/poll/label/div/carry/fault/barrier/vector/range boundaries\n";
}
}  // namespace

int main(int argc, char** argv) {
    try {
        std::cout << std::unitbuf;
        rlimit limit{}; getrlimit(RLIMIT_NOFILE, &limit);
        std::cout << "RLIMIT_NOFILE=" << limit.rlim_cur << '\n';
        const bool fold = argc == 2 && std::string{argv[1]} == "--fold";
        Environment original{Optimizations::None}, optimized{Optimizations::FlagElimination |
                (fold ? Optimizations::ConstantFolding : Optimizations::None)};
        std::cout << "reference environments prepared\n";
        PositiveReferences(original, optimized);
        if (!fold) {
            MalformedScalarALU(); MalformedTerminals(); TerminalReferences();
            RejectedBoundaries(optimized);
        }
        else std::cout << "PASS constant-folding flags references; default mode covers negative admission\n";
        std::cout << "OK " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
