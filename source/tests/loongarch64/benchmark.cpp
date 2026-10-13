#include "test_support.h"

#include <bit>
#include <chrono>
#include <string_view>
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
namespace {
using T = ir::ValueType;

u64 PressureHelper(u64 a, u64 b, u64 c) { return (a ^ std::rotl(b, 13)) + c; }

IntrusivePtr<ir::Block> Chain() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7000})};
    ir::Assembler as{block.get()};
    auto result = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    const auto rhs = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    for (u64 i = 0; i < 128; ++i) {
        result = as.Add(result, ir::Operand{rhs}).SetType(T::U64);
        if (i % 8 == 0) as.SaveFlags(result, ir::Flags::All);
        result = as.Xor(result, ir::Operand{ir::Imm{0x35ab + i}}).SetType(T::U64);
        result = as.RorImm(result, ir::Imm{u64{13}}).SetType(T::U64);
    }
    as.StoreUniform(ir::Uniform{256, T::U64}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

IntrusivePtr<ir::Block> Pressure() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7100})};
    ir::Assembler as{block.get()};
    std::array<ir::Value, 24> values;
    for (u32 i = 0; i < values.size(); ++i)
        values[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
    auto result = as.CallHost(&PressureHelper, values[0], values[11], values[23]).SetType(T::U64);
    for (const auto& value : values) result = as.Add(result, ir::Operand{value}).SetType(T::U64);
    as.SaveFlags(result, ir::Flags::All);
    as.StoreUniform(ir::Uniform{256, T::U64}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

IntrusivePtr<ir::Block> Loop() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7200})};
    ir::Assembler as{block.get()};
    auto count = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto accumulated = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto sum = as.Add(accumulated, ir::Operand{count}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{8, T::U64}, sum);
    auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{0, T::U64}, next);
    auto edge = as.Goto(as.TestNotZero(next));
    as.BindLabel(edge);
    auto* label = &block->GetInstList().back();
    block->RemoveInst(label);
    block->InsertBefore(label, count.Def());
    as.StoreUniform(ir::Uniform{256, T::U64}, sum);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    block->ReIdInstr();
    return block;
}

void Inputs(StateStorage& state, bool loop) {
    for (u32 i = 0; i < 24; ++i) state.Put(i * 8, 0x123456789ab0 + i);
    if (loop) { state.Put(0, 31); state.Put(8, 17); }
    state.state->host_cpu_flags = (u64{1} << 55) | 0x83;
}
}

void Benchmark() {
    constexpr u32 iterations = 4096;
    for (const auto* workload : {"chain", "pressure", "local_cfg_loop"}) {
        const bool loop = std::string_view{workload} == "local_cfg_loop";
        auto block = loop ? Loop() : std::string_view{workload} == "pressure" ? Pressure() : Chain();
        StateStorage reference;
        Inputs(reference, loop);
        backend::interp::Interpreter interpreter{*reference.state, block.get()};
        const auto wanted_halt = interpreter.Run();
        for (auto allocation : {la::ScalarAllocationMode::CanonicalHomes,
                                la::ScalarAllocationMode::LocalCache, la::ScalarAllocationMode::GlobalCFG})
        for (bool flags_publication : {false, true}) {
            Compiled compiled{block.get(), true, la::HostFeatures::Detect(), nullptr, allocation, flags_publication};
            StateStorage actual;
            Inputs(actual, loop);
            Check(compiled.fn(actual.state) == wanted_halt && actual.Get(256) == reference.Get(256) &&
                  actual.state->host_cpu_flags == reference.state->host_cpu_flags,
                  std::string{workload} + " benchmark warmup preserves value and flags");
            bool halts_match = true;
            const auto start = std::chrono::steady_clock::now();
            for (u32 call = 0; call < iterations; ++call) {
                if (loop) { actual.Put(0, 31); actual.Put(8, 17); }
                actual.state->halt_reason = HaltReason::None;
                halts_match &= compiled.fn(actual.state) == wanted_halt;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count();
            Check(halts_match && actual.Get(256) == reference.Get(256) &&
                  actual.state->host_cpu_flags == reference.state->host_cpu_flags,
                  std::string{workload} + " benchmark repeated execution preserves value and flags");
            const auto& values = compiled.context.ValueStats();
            const auto& registers = compiled.context.RegisterStats();
            const auto& coloring = compiled.context.AllocationStats();
            const auto& flag_stats = compiled.context.FlagStats();
            const auto& homes = compiled.context.HomeStats();
            const auto& emit = compiled.translator.Stats();
            const auto mode = compiled.context.AllocationMode();
            const auto* mode_name = mode == la::ScalarAllocationMode::CanonicalHomes ? "canonical_homes" :
                                    mode == la::ScalarAllocationMode::LocalCache ? "local_cache" : "global_cfg";
            std::cout << "BENCH {\"workload\":\"" << workload << "\",\"allocation\":\"" << mode_name << '"'
                      << ",\"cached\":" << (mode == la::ScalarAllocationMode::CanonicalHomes ? "false" : "true")
                      << ",\"flags_publication\":" << (flags_publication ? "true" : "false")
                      << ",\"iterations\":" << iterations << ",\"emitted_bytes\":" << compiled.context.CurrentBufferSize()
                      << ",\"instruction_words\":" << compiled.context.CurrentBufferSize() / 4
                      << ",\"ssa_loads\":" << values.loads << ",\"ssa_stores\":" << values.stores
                      << ",\"cache_hits\":" << values.cache_hits << ",\"spills\":" << values.spills
                      << ",\"register_reads\":" << values.register_reads
                      << ",\"dead_values\":" << values.dead_values << ",\"cfg_nodes\":" << registers.nodes
                      << ",\"max_live\":" << registers.max_live << ",\"liveness_iterations\":" << registers.iterations
                      << ",\"native_emitters\":" << emit.direct << ",\"helper_sites\":" << emit.helpers
                      << ",\"allocated_values\":" << coloring.register_values << ",\"spilled_values\":" << coloring.spilled_values
                      << ",\"interference_edges\":" << coloring.interference_edges << ",\"phi_coalesced\":" << coloring.phi_coalesced
                      << ",\"estimated_input_sites\":" << coloring.input_sites
                      << ",\"estimated_definition_sites\":" << coloring.definition_sites
                      << ",\"estimated_spill_input_sites\":" << coloring.spill_input_sites
                      << ",\"estimated_spill_definition_sites\":" << coloring.spill_definition_sites
                      << ",\"flags_loads\":" << flag_stats.loads << ",\"flags_stores\":" << flag_stats.stores
                      << ",\"flags_skipped_stores\":" << flag_stats.skipped_stores
                      << ",\"sparse_homes\":" << (compiled.context.SparseHomesActive() ? "true" : "false")
                      << ",\"home_slots\":" << compiled.context.SlotCount()
                      << ",\"home_bytes\":" << compiled.translator.HomeBytes()
                      << ",\"required_homes\":" << homes.required_homes
                      << ",\"spill_homes\":" << homes.spill_homes
                      << ",\"vector_homes\":" << homes.vector_homes
                      << ",\"helper_homes\":" << homes.helper_homes
                      << ",\"pseudo_homes\":" << homes.pseudo_homes
                      << ",\"raw_homes\":" << homes.raw_homes
                      << ",\"phi_homes\":" << homes.phi_homes
                      << ",\"frame_bytes\":" << emit.frame_size
                      << ",\"saved_frame_bytes\":" << emit.frame_size
                      << ",\"total_stack_bytes\":" << emit.frame_size + compiled.translator.HomeBytes()
                      << ",\"prologue_zero_stores\":" << compiled.context.SlotCount() * 2
                      << ",\"simulator_wall_ns\":" << elapsed
                      << ",\"performance_scope\":\"qemu_simulation_not_native_hardware\"}\n";
        }
    }
}
}  // namespace swift::tests::loongarch64
