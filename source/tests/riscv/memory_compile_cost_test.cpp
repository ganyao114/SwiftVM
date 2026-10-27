#include "test_support.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/riscv64/jit/compile.h"
#include "runtime/compile/builder/ir_builder.h"
#include "runtime/ir/hir_builder.h"
#include <algorithm>
#include <chrono>
#include <sys/resource.h>
#include <vector>

namespace swift::tests::riscv {
namespace {
namespace be = runtime::backend;
using T = ir::ValueType;
template <class Assembler> void Body(Assembler& as) {
    auto address = as.LoadUniform(T::U64, ir::Uniform{0, T::U64});
    auto increment = as.LoadUniform(T::U64, ir::Uniform{8, T::U64});
    auto value = as.LoadMemoryTSO(T::U64, ir::Operand{address});
    for (u32 index = 0; index < 4; ++index) {
        value = as.Add(T::U64, value, ir::Operand{increment});
        as.StoreMemoryTSO(ir::Operand{address}, value);
        value = as.LoadMemoryTSO(T::U64, ir::Operand{address});
    }
    as.StoreUniform(ir::Uniform{16, T::U64}, value);
}
void Measure(const char* kind, bool function) {
    Config config{TestConfig()};
    be::AddressSpace space{config};
    auto module = space.MapModule(0x10000, 0x20000,
                                 {.read_only = true, .optimizations = Optimizations::FlagElimination});
    constexpr u32 warmup = 32, count = 768;
    std::vector<double> times;
    u32 saves{};
    for (u32 index = 0; index < warmup + count; ++index) {
        const auto guest = ir::Location{0x10000 + u64(index) * 16};
        void* entry{}; double elapsed{};
        if (function) {
            ir::HIRBuilder builder{1, true};
            auto* unit = builder.AppendFunction(guest);
            Body(*unit); unit->EndBlock(ir::terminal::ReturnToHost{}); unit->EndFunction();
            const auto start = std::chrono::steady_clock::now();
            entry = be::riscv64::CompileFunctions(module, std::span{&unit, 1});
            elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            for (auto* block : unit->GetHIRBlocks()) for (auto& inst : block->GetBlock()->GetInstList())
                saves += inst.GetOp() == ir::OpCode::SaveFlags;
        } else {
            IntrusivePtr<ir::Block> block{new ir::Block(guest)};
            compile::IrBuilder as{block.get()}; Body(as);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            const auto start = std::chrono::steady_clock::now();
            entry = be::riscv64::CompileBlock(module, block.get());
            elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
            for (auto& inst : block->GetInstList()) saves += inst.GetOp() == ir::OpCode::SaveFlags;
        }
        Check(entry != nullptr, "whole memory compile entry produced code");
        alignas(64) std::array<u64, 16> memory{}; memory[8] = 3;
        StateStorage state; state.state->pt = memory.data(); state.Put(0, 64); state.Put(8, 5);
        const auto halt = reinterpret_cast<BlockFn>(entry)(state.state);
        Check(halt == HaltReason::CallHost && memory[8] == 23 && state.Get(16) == 23, "memory compile fixture native checksum");
        Check(state.memory_participant.active.load() == 0 && state.state->spill_area[be::kRiscvMemoryOwnedSlot] == 0,
              "compiled memory fixture releases its lease");
        if (index >= warmup) times.push_back(elapsed);
    }
    Check(saves == 0, "memory fixture has no introduced flags writes");
    double total{}; for (auto time : times) total += time;
    std::sort(times.begin(), times.end());
    std::cout << "compile_cost " << kind << " count=" << count << " total_ns=" << total
              << " median_ns=" << times[count / 2] << " saves_after=" << saves << '\n';
}
} // namespace
} // namespace swift::tests::riscv
int main() {
    using namespace swift::tests::riscv;
    try {
        struct rlimit limit{}; getrlimit(RLIMIT_NOFILE, &limit);
        std::cout << "RLIMIT_NOFILE=" << limit.rlim_cur << '\n';
        Measure("CompileBlockMemory", false); Measure("CompileFunctionsMemory", true);
        std::cout << "OK " << checks << " checks\n";
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
