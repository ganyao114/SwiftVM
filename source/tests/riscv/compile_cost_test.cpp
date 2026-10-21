#include "test_support.h"

#include <algorithm>
#include <chrono>
#include <sys/resource.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/riscv64/jit/compile.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::tests::riscv;
namespace be = swift::runtime::backend;

namespace {
void ReportLateBarrier(const char* kind, const ir::Block* block) {
    unsigned instructions{}, returns{}, values{}, flags{}, uniforms{}, saves{};
    for (const auto& inst : block->GetInstList()) {
        if (inst.GetOp() == ir::OpCode::MemoryBarrierTSO) break;
        ++instructions;
        returns += inst.ReturnType() != ir::ValueType::VOID;
        saves += inst.GetOp() == ir::OpCode::SaveFlags;
        for (unsigned slot = 0; slot < ir::Inst::max_args; ++slot) {
            const auto& arg = inst.ArgAt(slot);
            values += arg.IsValue();
            flags += arg.GetType() == ir::ArgType::Flags;
            uniforms += arg.GetType() == ir::ArgType::Uniform;
        }
    }
    std::cout << "admission_fixture " << kind << " instructions_before_barrier=" << instructions
              << " return_width=" << returns << " arg_slots=" << instructions * ir::Inst::max_args
              << " value_width=" << values << " flag_masks=" << flags
              << " uniform_ranges=" << uniforms << " definite_producers=" << saves << '\n';
}

template <typename Assembler>
void Body(Assembler& as, unsigned operations) {
    auto value = as.LoadUniform(ir::Uniform{0, ir::ValueType::U64}).SetType(ir::ValueType::U64);
    auto right = as.LoadUniform(ir::Uniform{8, ir::ValueType::U64}).SetType(ir::ValueType::U64);
    for (unsigned i = 0; i < operations; ++i) {
        value = as.Add(value, ir::Operand{right}).SetType(ir::ValueType::U64);
        as.SaveFlags(value, ir::Flags::All);
    }
    as.StoreUniform(ir::Uniform{16, ir::ValueType::U64}, value);
}

void Measure(const char* kind, bool whole_function, bool rejected = false,
             unsigned operations = 24, bool fold = false, bool flags = true) {
    Config config{TestConfig()};
    be::AddressSpace space{config};
    auto module = space.MapModule(0x10000, 0x20000,
            {.read_only = true, .optimizations = (flags ? Optimizations::FlagElimination : Optimizations::None) |
                    (fold ? Optimizations::ConstantFolding : Optimizations::None)});
    constexpr unsigned warmup = 32, count = 768;
    std::vector<double> times;
    times.reserve(count);
    unsigned saves{};
    for (unsigned i = 0; i < warmup + count; ++i) {
        const auto guest = ir::Location{0x10000 + u64{i} * 16};
        void* entry{};
        double elapsed{};
        if (whole_function) {
            ir::HIRBuilder builder{1, true};
            auto* function = builder.AppendFunction(guest);
            Body(*function, operations);
            function->EndBlock(ir::terminal::ReturnToHost{});
            function->EndFunction();
            for (auto* hir : function->GetHIRBlocks()) {
                if (rejected && hir->GetBlock()->GetStartLocation() == guest) {
                    hir->GetBlock()->AppendInst(ir::OpCode::MemoryBarrierTSO);
                    if (i == 0) ReportLateBarrier(kind, hir->GetBlock());
                }
            }
            const auto start = std::chrono::steady_clock::now();
            entry = be::riscv64::CompileFunctions(module, std::span{&function, 1});
            elapsed = std::chrono::duration<double, std::nano>(
                    std::chrono::steady_clock::now() - start).count();
            for (auto* hir : function->GetHIRBlocks())
                for (const auto& inst : hir->GetBlock()->GetInstList())
                    saves += inst.GetOp() == ir::OpCode::SaveFlags;
        } else {
            IntrusivePtr<ir::Block> block{new ir::Block(guest)};
            ir::Assembler as{block.get()};
            Body(as, operations);
            if (rejected) block->AppendInst(ir::OpCode::MemoryBarrierTSO);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            if (rejected && i == 0) ReportLateBarrier(kind, block.get());
            const auto start = std::chrono::steady_clock::now();
            entry = be::riscv64::CompileBlock(module, block.get());
            elapsed = std::chrono::duration<double, std::nano>(
                    std::chrono::steady_clock::now() - start).count();
            for (const auto& inst : block->GetInstList())
                saves += inst.GetOp() == ir::OpCode::SaveFlags;
        }
        Check(entry != nullptr, "whole-entry compile produced native code");
        StateStorage storage;
        storage.Put(0, 3); storage.Put(8, 5);
        reinterpret_cast<BlockFn>(entry)(storage.state);
        Check(storage.Get(16) == 3 + 5 * operations, "compile-cost fixture native checksum");
        if (i >= warmup) times.push_back(elapsed);
    }
    double total{};
    for (double elapsed : times) total += elapsed;
    std::sort(times.begin(), times.end());
    Check(saves == (warmup + count) * ((rejected || !flags) ? operations : 1),
          "compile fixture admitted/rejected flags shape");
    std::cout << "compile_cost " << kind << " count=" << count << " total_ns=" << total
              << " median_ns=" << times[count / 2] << " saves_after=" << saves << '\n';
}
} // namespace

int main(int argc, char** argv) {
    try {
        rlimit limit{}; getrlimit(RLIMIT_NOFILE, &limit);
        std::cout << "RLIMIT_NOFILE=" << limit.rlim_cur << '\n';
        if (argc == 2 && std::string{argv[1]} == "--rejected-only") {
            Measure("CompileBlockRejected", false, true);
            Measure("CompileBlockRejectedFold", false, true, 24, true);
            std::cout << "OK " << checks << " checks\n";
            return 0;
        }
        Measure("CompileBlock", false);
        Measure("CompileFunctions", true);
        Measure("CompileBlockRejected", false, true);
        Measure("CompileFunctionsRejected", true, true);
        Measure("CompileBlockShort", false, false, 2);
        Measure("CompileFunctionsShort", true, false, 2);
        Measure("CompileBlockFold", false, false, 24, true);
        Measure("CompileFunctionsFold", true, false, 24, true);
        Measure("CompileBlockFoldShort", false, false, 2, true);
        Measure("CompileFunctionsFoldShort", true, false, 2, true);
        Measure("CompileBlockRejectedFold", false, true, 24, true);
        Measure("CompileFunctionsRejectedFold", true, true, 24, true);
        Measure("CompileBlockDisabled", false, false, 24, false, false);
        Measure("CompileFunctionsDisabled", true, false, 24, false, false);
        std::cout << "OK " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
