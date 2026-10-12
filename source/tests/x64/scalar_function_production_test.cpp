#include <array>
#include <atomic>
#include <cstring>
#include <span>
#include <vector>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/function_plan.h"
#include "runtime/backend/riscv64/memory_protocol.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/function.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;

namespace {
constexpr u32 uniform_extent = 256;
constexpr u64 initial_flags = 0xabcde98765432109ULL;
constexpr u64 chain_xor = 0x9e3779b97f4a7c15ULL;
using BlockFn = HaltReason (*)(backend::State*);

Config ProductionConfig(bool cache) {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
            .backend_isa = kX86_64, .uniform_buffer_size = uniform_extent,
            .static_program = true,
            .global_opts = cache ? Optimizations::ScalarRegisterCache : Optimizations::None,
            .stack_alignment = 16};
}
void Put(std::span<u8> bytes, u32 offset, u64 value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}
void Initialize(backend::State* state) {
    for (u32 i = 0; i < uniform_extent; ++i)
        state->uniform_buffer_begin[i] = static_cast<u8>(i * 37 + 0xa5);
    state->host_cpu_flags = initial_flags;
}
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + uniform_extent> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    Storage() { Initialize(state); }
    ~Storage() { state->~State(); }
    std::span<u8> Uniforms() { return {state->uniform_buffer_begin, uniform_extent}; }
};
// Compare every declared uniform byte, the architectural PC/flags/stop state,
// and the complete spill area. Runtime dispatch pointers are host identities.
struct Snapshot {
    std::array<u8, uniform_extent> uniforms;
    u64 flags, pc, previous, request;
    HaltReason halt;
    std::array<u64, 64> spills;
    bool operator==(const Snapshot&) const = default;
};
Snapshot Capture(const backend::State* state) {
    Snapshot result;
    std::memcpy(result.uniforms.data(), state->uniform_buffer_begin, uniform_extent);
    result.flags = state->host_cpu_flags;
    result.pc = state->current_loc.Value();
    result.previous = state->prev_loc.Value();
    result.request = state->exit_request;
    result.halt = state->halt_reason;
    result.spills = state->spill_area;
    return result;
}
u64 MathChain(u64 value, u64 addend) {
    // Independent unsigned arithmetic; every operation wraps modulo 2^64.
    return ((((value + addend) ^ chain_xor) * 13) << 3) - 7;
}
u64 MathAddFlags(u64 value, u64 addend) {
    const u64 result = value + addend;
    const bool overflow = ((~(value ^ addend) & (value ^ result)) >> 63) != 0;
    return (initial_flags & ~(u64{15} << 28)) | (result & (u64{1} << 63) ? u64{1} << 31 : 0) |
            (result == 0 ? u64{1} << 30 : 0) | (result < value ? u64{1} << 29 : 0) |
            (overflow ? u64{1} << 28 : 0);
}
u64 ClobberReturningHelper(u64 a, u64 b, u64 c) {
    asm volatile("xor %%r8, %%r8\n\txor %%r9, %%r9\n\txor %%r10, %%r10\n\txor %%r11, %%r11\n\t"
                 "pxor %%xmm0, %%xmm0\n\tpxor %%xmm1, %%xmm1"
                 ::: "r8", "r9", "r10", "r11", "xmm0", "xmm1", "cc");
    return a * 3 + b * 5 + c * 7;
}
constexpr u64 uniform_replacement = 0x1029384756abcdefULL;
u64 MutateUniformHelper(u64 pointer, u64 value, u64) {
    std::memcpy(reinterpret_cast<void*>(pointer), &uniform_replacement, sizeof(uniform_replacement));
    return ClobberReturningHelper(value, value, value);
}
struct ChainValues { ir::Value result, addition; };
ChainValues EmitChain(ir::HIRFunction* function, ir::Value input, u64 addend) {
    const auto a = function->Add<ir::U64>(input, ir::Operand{ir::Imm{addend}});
    const auto b = function->Xor<ir::U64>(a, ir::Operand{ir::Imm{chain_xor}});
    const auto c = function->Mul<ir::U64>(b, ir::Operand{ir::Imm{u64{13}}});
    const auto d = function->LslImm<ir::U64>(c, ir::Imm{u8{3}});
    const auto e = function->Sub<ir::U64>(d, ir::Operand{ir::Imm{u64{7}}});
    return {e, a};
}
std::vector<ir::HIRBlock*> OrderedBlocks(ir::HIRFunction* function) {
    std::vector<ir::HIRBlock*> result;
    const auto start = function->GetFunction()->GetStartLocation();
    for (auto* block : function->GetHIRBlocks())
        if (block && block->GetBlock()->GetStartLocation() == start &&
            (block->GetBlock()->HasTerminal() || !block->GetInstList().empty())) result.push_back(block);
    for (auto* block : function->GetHIRBlocks())
        if (block && block->GetBlock()->GetStartLocation() != start &&
            block->GetBlock()->GetStartLocation() != ir::Location::INVALID && block->GetBlock()->HasTerminal())
            result.push_back(block);
    return result;
}
struct Publication {
    BlockFn entry;
    std::vector<u8> bytes;
};
Publication CompileAndProve(const std::shared_ptr<backend::Module>& module,
                            ir::HIRFunction* function, bool cache) {
    auto blocks = OrderedBlocks(function);
    REQUIRE(x64::NeedsFunctionFrame(blocks));
    auto plan = backend::FunctionPlan::Build(function, blocks);
    const auto& config = module->GetAddressSpace().GetConfig();
    REQUIRE(backend::ResolveFeatureSet(module->GetModuleConfig()).x64_scalar_cache == cache);
    x64::Translator off{plan.body.get(), x64::HostFeatures::Detect(), &config, &plan, false};
    x64::Translator on{plan.body.get(), x64::HostFeatures::Detect(), &config, &plan, true};
    off.Translate(); on.Translate();
    REQUIRE(on.ScalarPlan().Enabled());
    REQUIRE(on.ScalarPlan().ValidateNonOverlapping());
    REQUIRE(on.Statistics().scalar_cache_read_sites > 0);
    REQUIRE(on.Statistics().scalar_cache_write_sites > 0);
    REQUIRE(on.Statistics().scalar_coalesced_binary_sites > 0);
    REQUIRE(off.Statistics().scalar_cache_read_sites == 0);
    REQUIRE(off.Statistics().scalar_cache_write_sites == 0);
    REQUIRE(off.FrameSize() == on.FrameSize());
    REQUIRE(off.Statistics().phi_value_copy_sites == on.Statistics().phi_value_copy_sites);
    REQUIRE(off.Statistics().phi_immediate_copy_sites == on.Statistics().phi_immediate_copy_sites);
    for (auto& inst : plan.body->GetInstList()) if (inst.GetOp() == ir::OpCode::AddPhi) {
        REQUIRE_FALSE(on.ScalarPlan().Slot(&inst));
        REQUIRE(on.ScalarPlan().RequiresCanonicalHome(&inst));
        for (const auto& param : inst.GetArg<ir::Params>(0)) if (param.data.IsValue()) {
            REQUIRE_FALSE(on.ScalarPlan().Slot(param.data.value.Def()));
            REQUIRE(on.ScalarPlan().RequiresCanonicalHome(param.data.value.Def()));
        }
    }
    if (function->GetFunction()->GetStartLocation() == ir::Location{0x1000}) {
        u32 cached_callback_survivors{};
        for (auto& inst : plan.body->GetInstList()) if (inst.GetOp() == ir::OpCode::StoreUniform) {
            const auto offset = inst.GetArg<ir::Uniform>(0).GetOffset();
            if (offset == 72 || offset == 80) {
                REQUIRE(on.ScalarPlan().Slot(inst.GetArg<ir::Value>(1).Def()));
                ++cached_callback_survivors;
            }
        }
        REQUIRE(cached_callback_survivors == 2);
    }
    const std::vector<u8> off_bytes{off.getCode(), off.getCode() + off.getSize()};
    const std::vector<u8> on_bytes{on.getCode(), on.getCode() + on.getSize()};
    REQUIRE(off_bytes != on_bytes);
    const auto& expected = cache ? on_bytes : off_bytes;
    auto* published = x64::CompileFunctions(module, std::span{&function, 1});
    REQUIRE(published != nullptr);
    REQUIRE(*function->GetFunction()->GetJitCache().cache_size >= expected.size());
    // These kernels embed only stable helper addresses and relative labels.
    // Compare all emitted bytes, not a size heuristic or an independent probe
    // alone: this proves the production entry is the selected cache engine.
    REQUIRE(std::memcmp(published, expected.data(), expected.size()) == 0);
    REQUIRE(module->GetAddressSpace().GetCodeCache(function->GetFunction()->GetStartLocation()) == published);
    for (size_t i = 1; i < blocks.size(); ++i)
        REQUIRE(module->GetAddressSpace().GetCodeCache(blocks[i]->GetBlock()->GetStartLocation()) == nullptr);
    return {reinterpret_cast<BlockFn>(published), expected};
}
HaltReason Execute(BlockFn entry, backend::State* state) {
    const auto original = _mm_getcsr();
    const auto changed = (original & ~0x6000u) | 0x2000u;
    _mm_setcsr(changed);
    const auto result = entry(state);
    const auto restored = _mm_getcsr();
    _mm_setcsr(original);
    REQUIRE(restored == changed);
    REQUIRE(backend::riscv64::anonymous_memory_readers.load() == 0);
    return result;
}
}

TEST_CASE("x64 production shared cache diamond survives HIR release and matches complete state", "[x64][scalar-function-production]") {
    std::vector<Snapshot> baseline;
    std::vector<u8> off_code;
    for (bool cache : {false, true}) {
        backend::AddressSpace space{ProductionConfig(cache)}; auto module = space.GetDefaultModule();
        Publication compiled;
        {
            ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0x1000});
            const auto input = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
            const auto selector = function->LoadUniform<ir::U8>(ir::Uniform{8, T::U8});
            const auto root = EmitChain(function, input, 17);
            function->StoreUniform(ir::Uniform{16, T::U64}, root.result);
            builder.If(ir::terminal::If{ir::BOOL{selector}, ir::terminal::LinkBlock{ir::Location{0x1100}}, ir::terminal::LinkBlock{ir::Location{0x1200}}});
            builder.SetCurBlock(ir::Location{0x1100}); const auto left = EmitChain(function, root.result, 31).result;
            function->StoreUniform(ir::Uniform{24, T::U64}, left);
            builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0x1300}});
            builder.SetCurBlock(ir::Location{0x1200}); const auto right = EmitChain(function, root.result, 47).result;
            function->StoreUniform(ir::Uniform{24, T::U64}, right);
            builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0x1300}});
            builder.SetCurBlock(ir::Location{0x1300}); ir::Params inputs; inputs.Push(left); inputs.Push(right);
            const auto phi = function->AddPhi<ir::U64>(inputs);
            const auto result = EmitChain(function, phi, 29).result;
            const auto helper = function->CallLambda<ir::U64>(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&ClobberReturningHelper)}},
                                                              phi, root.result, phi);
            const auto old_uniform = function->LoadUniform<ir::U64>(ir::Uniform{64, T::U64});
            const auto old_chain = EmitChain(function, old_uniform, 19).result;
            const auto address = function->GetUniformAddress<ir::U64>(ir::Imm{u32{64}});
            const auto mutation_return = function->CallLambda<ir::U64>(
                    ir::Lambda{ir::Imm{reinterpret_cast<u64>(&MutateUniformHelper)}}, address, old_uniform, old_uniform);
            function->StoreUniform(ir::Uniform{72, T::U64}, old_uniform);
            function->StoreUniform(ir::Uniform{80, T::U64}, old_chain);
            function->StoreUniform(ir::Uniform{88, T::U64}, function->LoadUniform<ir::U64>(ir::Uniform{64, T::U64}));
            function->StoreUniform(ir::Uniform{96, T::U64}, mutation_return);
            // Publish a root producer's snapshot after the cross-block Phi,
            // arithmetic and a caller-saved/host-flags clobbering helper.
            function->SaveFlags(root.addition, ir::Flags::NZCV);
            function->StoreUniform(ir::Uniform{32, T::U64}, result);
            function->StoreUniform(ir::Uniform{40, T::U64}, phi);
            function->StoreUniform(ir::Uniform{48, T::U64}, function->GetLocation<ir::U64>());
            function->StoreUniform(ir::Uniform{56, T::U64}, helper);
            function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
            compiled = CompileAndProve(module, function, cache);
        }
        if (!cache) off_code = compiled.bytes; else REQUIRE(compiled.bytes != off_code);
        size_t case_index{};
        for (u64 input : {u64{0}, u64{1}, u64{0x7fffffffffffffff}, UINT64_MAX}) for (u64 selector : {u64{0}, u64{1}}) {
            INFO("cache=" << cache << " input=" << input << " selector=" << selector);
            Storage actual; Put(actual.Uniforms(), 0, input); Put(actual.Uniforms(), 8, selector);
            auto expected = Capture(actual.state);
            u64 original_uniform; std::memcpy(&original_uniform, expected.uniforms.data() + 64, 8);
            const auto root = MathChain(input, 17); const auto selected = MathChain(root, selector ? 31 : 47);
            Put(expected.uniforms, 16, root); Put(expected.uniforms, 24, selected);
            Put(expected.uniforms, 32, MathChain(selected, 29)); Put(expected.uniforms, 40, selected); Put(expected.uniforms, 48, 0x1300);
            Put(expected.uniforms, 56, selected * 10 + root * 5);
            Put(expected.uniforms, 64, uniform_replacement); Put(expected.uniforms, 72, original_uniform);
            Put(expected.uniforms, 80, MathChain(original_uniform, 19)); Put(expected.uniforms, 88, uniform_replacement);
            Put(expected.uniforms, 96, original_uniform * 15);
            expected.flags = MathAddFlags(input, 17); expected.pc = 0x1300;
            expected.previous = selector ? 0x1100 : 0x1200; expected.halt = HaltReason::CallHost;
            REQUIRE(Execute(compiled.entry, actual.state) == HaltReason::CallHost);
            const auto result = Capture(actual.state); REQUIRE(result == expected);
            if (!cache) baseline.push_back(result); else REQUIRE(result == baseline.at(case_index));
            ++case_index;
        }
    }
}

TEST_CASE("x64 production shared cache loop Phi matches an independent recurrence after HIR release", "[x64][scalar-function-production]") {
    std::vector<Snapshot> baseline;
    for (bool cache : {false, true}) {
        backend::AddressSpace space{ProductionConfig(cache)}; auto module = space.GetDefaultModule();
        Publication compiled;
        {
            ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0x2000});
            const auto seed = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
            const auto iterations = function->LoadUniform<ir::U64>(ir::Uniform{8, T::U64});
            const auto root = EmitChain(function, seed, 13);
            function->SaveFlags(root.addition, ir::Flags::NZCV);
            function->StoreUniform(ir::Uniform{16, T::U64}, root.result);
            builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0x2100}});
            builder.SetCurBlock(ir::Location{0x2100});
            ir::Params initial_count; initial_count.Push(iterations); initial_count.Push(iterations);
            ir::Params initial_value; initial_value.Push(root.result); initial_value.Push(root.result);
            const auto count = function->AddPhi<ir::U64>(initial_count);
            const auto value = function->AddPhi<ir::U64>(initial_value);
            const auto next_value = EmitChain(function, value, 7).result;
            const auto next_count = function->Sub<ir::U64>(count, ir::Operand{ir::Imm{u64{1}}});
            ir::Params counts; counts.Push(iterations); counts.Push(next_count); count.Def()->SetArg(0, counts);
            ir::Params values; values.Push(root.result); values.Push(next_value); value.Def()->SetArg(0, values);
            function->StoreUniform(ir::Uniform{24, T::U64}, next_value);
            const auto complete = function->TestZero<ir::U8>(next_count);
            builder.If(ir::terminal::If{ir::BOOL{complete}, ir::terminal::LinkBlock{ir::Location{0x2200}}, ir::terminal::LinkBlock{ir::Location{0x2100}}});
            builder.SetCurBlock(ir::Location{0x2200});
            function->StoreUniform(ir::Uniform{32, T::U64}, EmitChain(function, next_value, 29).result);
            function->StoreUniform(ir::Uniform{40, T::U64}, value);
            function->StoreUniform(ir::Uniform{48, T::U64}, next_count);
            function->StoreUniform(ir::Uniform{56, T::U64}, function->GetLocation<ir::U64>());
            function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
            compiled = CompileAndProve(module, function, cache);
        }
        size_t case_index{};
        for (u64 seed : {u64{0}, u64{0x8000000000000000}, UINT64_MAX}) for (u64 iterations : {u64{1}, u64{2}, u64{7}, u64{63}}) {
            INFO("cache=" << cache << " seed=" << seed << " iterations=" << iterations);
            Storage actual; Put(actual.Uniforms(), 0, seed); Put(actual.Uniforms(), 8, iterations);
            auto expected = Capture(actual.state); const auto root = MathChain(seed, 13);
            u64 previous = root, value = root;
            for (u64 i = 0; i < iterations; ++i) { previous = value; value = MathChain(value, 7); }
            Put(expected.uniforms, 16, root); Put(expected.uniforms, 24, value);
            Put(expected.uniforms, 32, MathChain(value, 29)); Put(expected.uniforms, 40, previous);
            Put(expected.uniforms, 48, 0); Put(expected.uniforms, 56, 0x2200);
            expected.flags = MathAddFlags(seed, 13); expected.pc = 0x2200; expected.previous = 0x2100; expected.halt = HaltReason::CallHost;
            REQUIRE(Execute(compiled.entry, actual.state) == HaltReason::CallHost);
            const auto result = Capture(actual.state); REQUIRE(result == expected);
            if (!cache) baseline.push_back(result); else REQUIRE(result == baseline.at(case_index));
            ++case_index;
        }
    }
}

TEST_CASE("x64 production shared cache Switch preserves distinct duplicate edge Phi values", "[x64][scalar-function-production]") {
    std::vector<Snapshot> baseline;
    for (bool cache : {false, true}) {
        backend::AddressSpace space{ProductionConfig(cache)}; auto module = space.GetDefaultModule();
        Publication compiled;
        {
            ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0x3000});
            const auto seed = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
            const auto selector = function->LoadUniform<ir::U64>(ir::Uniform{8, T::U64});
            const auto root = EmitChain(function, seed, 23);
            function->SaveFlags(root.addition, ir::Flags::NZCV);
            function->StoreUniform(ir::Uniform{16, T::U64}, root.result);
            builder.Switch(ir::terminal::Switch{selector, {
                    {ir::Imm{u64{0}}, ir::terminal::LinkBlock{ir::Location{0x3100}}},
                    {ir::Imm{u64{7}}, ir::terminal::LinkBlock{ir::Location{0x3100}}}}});
            builder.SetCurBlock(ir::Location{0x3100}); ir::Params inputs; inputs.Push(ir::Imm{u64{73}}); inputs.Push(root.result);
            const auto phi = function->AddPhi<ir::U64>(inputs);
            function->StoreUniform(ir::Uniform{24, T::U64}, EmitChain(function, phi, 37).result);
            function->StoreUniform(ir::Uniform{32, T::U64}, phi);
            function->StoreUniform(ir::Uniform{40, T::U64}, function->GetLocation<ir::U64>());
            function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
            compiled = CompileAndProve(module, function, cache);
        }
        size_t case_index{};
        for (u64 seed : {u64{0}, u64{41}, UINT64_MAX}) for (u64 selector : {u64{0}, u64{7}, u64{9}, UINT64_MAX}) {
            INFO("cache=" << cache << " seed=" << seed << " selector=" << selector);
            Storage actual; Put(actual.Uniforms(), 0, seed); Put(actual.Uniforms(), 8, selector);
            auto expected = Capture(actual.state); const auto root = MathChain(seed, 23); const bool matched = selector == 0 || selector == 7;
            Put(expected.uniforms, 16, root); expected.flags = MathAddFlags(seed, 23);
            expected.pc = matched ? 0x3100 : 0x3000; expected.previous = 0x3000;
            expected.halt = matched ? HaltReason::CallHost : HaltReason::None;
            if (matched) {
                const auto selected = selector == 0 ? u64{73} : root;
                Put(expected.uniforms, 24, MathChain(selected, 37)); Put(expected.uniforms, 32, selected); Put(expected.uniforms, 40, 0x3100);
            }
            REQUIRE(Execute(compiled.entry, actual.state) == expected.halt);
            const auto result = Capture(actual.state); REQUIRE(result == expected);
            if (!cache) baseline.push_back(result); else REQUIRE(result == baseline.at(case_index));
            ++case_index;
        }
    }
}

TEST_CASE("x64 production shared cache native faults preserve committed arithmetic and recovery state", "[x64][scalar-function-production][fault]") {
    struct Mapping {
        void* address = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        ~Mapping() { if (address != MAP_FAILED) munmap(address, 4096); }
    } page;
    REQUIRE(page.address != MAP_FAILED);
    std::vector<Snapshot> baseline;
    for (bool cache : {false, true}) {
        REQUIRE(mprotect(page.address, 4096, PROT_NONE) == 0);
        backend::AddressSpace space{ProductionConfig(cache)}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
        {
            ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0x4000});
            const auto address = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
            const auto seed = function->LoadUniform<ir::U64>(ir::Uniform{8, T::U64});
            const auto root = EmitChain(function, seed, 41).result;
            function->StoreUniform(ir::Uniform{16, T::U64}, root);
            builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0x4100}});
            builder.SetCurBlock(ir::Location{0x4100}); ir::Params inputs; inputs.Push(root);
            const auto phi = function->AddPhi<ir::U64>(inputs);
            const auto committed = EmitChain(function, phi, 43);
            function->SaveFlags(committed.addition, ir::Flags::NZCV);
            function->StoreUniform(ir::Uniform{24, T::U64}, committed.result);
            function->AdvancePC(ir::Imm{u32{4}});
            const auto loaded = function->LoadMemory<ir::U64>(ir::Operand{address});
            function->StoreUniform(ir::Uniform{32, T::U64}, loaded);
            function->StoreUniform(ir::Uniform{40, T::U64}, EmitChain(function, committed.result, 47).result);
            function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
            (void)CompileAndProve(module, function, cache);
        }
        Initialize(runtime.GetState());
        constexpr u64 seed = 0xfffffffffffffff1ULL;
        Put(runtime.GetUniformBuffer(), 0, reinterpret_cast<u64>(page.address)); Put(runtime.GetUniformBuffer(), 8, seed);
        runtime.GetState()->guest_addr_limit = UINT64_MAX;
        runtime.SetLocation(0x4000);
        const auto before = Capture(runtime.GetState());
        const auto root = MathChain(seed, 41); const auto committed = MathChain(root, 43);
        for (bool accessible : {false, true}) {
            if (accessible) {
                REQUIRE(mprotect(page.address, 4096, PROT_READ | PROT_WRITE) == 0);
                const u64 loaded = 0x123456789abcdef0ULL; std::memcpy(page.address, &loaded, 8);
                runtime.SetLocation(0x4000);
            }
            auto expected = before;
            Put(expected.uniforms, 16, root); Put(expected.uniforms, 24, committed);
            if (accessible) {
                Put(expected.uniforms, 32, 0x123456789abcdef0ULL); Put(expected.uniforms, 40, MathChain(committed, 47));
            }
            expected.flags = MathAddFlags(root, 43); expected.pc = 0x4104; expected.previous = 0x4000; expected.halt = HaltReason::None;
            const auto original = _mm_getcsr(); const auto changed = (original & ~0x6000u) | 0x6000u;
            _mm_setcsr(changed); const auto reason = runtime.Run(); const auto restored = _mm_getcsr(); _mm_setcsr(original);
            REQUIRE(reason == (accessible ? HaltReason::CallHost : HaltReason::PageFatal));
            REQUIRE(restored == changed); REQUIRE(runtime.GetLocation() == 0x4104);
            REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
            REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
            REQUIRE(runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == 0);
            REQUIRE(backend::riscv64::anonymous_memory_readers.load() == 0);
            const auto result = Capture(runtime.GetState()); REQUIRE(result == expected);
            if (!cache) baseline.push_back(result); else REQUIRE(result == baseline.at(accessible ? 1 : 0));
        }
    }
}
