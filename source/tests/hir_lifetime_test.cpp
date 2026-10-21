#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>

#include "runtime/ir/hir_builder.h"

namespace {
thread_local bool fail_next_allocation{};
unsigned checks{};
unsigned injected_failures{};
}

// The failure hook is confined to this standalone test executable.
void* operator new(std::size_t size) {
    if (std::exchange(fail_next_allocation, false)) {
        ++injected_failures;
        throw std::bad_alloc{};
    }
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::ir;

namespace {
constexpr std::size_t block_capacity = 256;
constexpr std::size_t function_capacity = 128;

void Check(bool condition, const char* detail) {
    ++checks;
    if (!condition) throw std::runtime_error(detail);
}

template <typename T, std::size_t Capacity>
void CheckFreeObjects(std::size_t expected, const char* detail) {
    std::array<T*, Capacity> objects{};
    std::size_t count{};
    while (auto* object = T::Allocate()) {
        Check(count < Capacity, "slab free list exceeds its original capacity");
        for (std::size_t i = 0; i < count; ++i) {
            Check(objects[i] != object, "slab free list contains a duplicate node");
        }
        objects[count++] = object;
    }
    for (std::size_t i = 0; i < count; ++i) T::Free(objects[i]);
    Check(count == expected, detail);
}

void CheckAllObjectsFree() {
    CheckFreeObjects<Block, block_capacity>(block_capacity, "raw Block ownership leaks");
    CheckFreeObjects<Function, function_capacity>(function_capacity, "raw Function ownership leaks");
}

struct InstructionSet {
    std::array<Inst*, 8> addresses{};
    std::size_t count{};
};

InstructionSet CaptureInstructions(HIRFunction* function) {
    InstructionSet result;
    for (auto& block : function->GetHIRBlockList()) {
        for (auto& inst : block.GetInstList()) {
            Check(result.count < result.addresses.size(), "instruction capture capacity exceeded");
            result.addresses[result.count++] = &inst;
        }
    }
    return result;
}

void CheckReusedInstructions(const InstructionSet& expected) {
    Check(expected.count != 0, "instruction-reuse oracle has no captured objects");
    std::array<std::unique_ptr<Inst>, 8> probes;
    for (std::size_t i = 0; i < expected.count; ++i) {
        probes[i] = std::make_unique<Inst>(OpCode::AdvancePC);
        Check(std::find(expected.addresses.begin(), expected.addresses.begin() + expected.count,
                        probes[i].get()) != expected.addresses.begin() + expected.count,
              "Block teardown did not return a captured instruction to the actual TLS free list");
        for (std::size_t j = 0; j < i; ++j) {
            Check(probes[j].get() != probes[i].get(), "instruction free list contains duplicate slots");
        }
    }
}

template <typename F> void ExpectAllocationFailure(F&& operation) {
    const auto before = injected_failures;
    bool failed{};
    fail_next_allocation = true;
    try {
        operation();
    } catch (const std::bad_alloc&) {
        failed = true;
    } catch (...) {
        fail_next_allocation = false;
        throw;
    }
    fail_next_allocation = false;
    Check(failed && injected_failures == before + 1, "expected allocation failure was not observed");
}

HIRFunction* CreateFunction(HIRPools& pools, bool fast, u64 start = 0x100) {
    return pools.functions.Create(std::make_unique<Function>(Location{start}),
                                  Location{start}, Location{start + 0x20}, pools, fast);
}

void AddLiveInstructions(HIRFunction* function) {
    const auto input = function->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
    auto* next = function->AppendBlock(Location{0x110});
    function->AddEdge(function->GetCurrentBlock(), next);
    function->SetCurBlock(next);
    const auto sum = function->Add(input, Operand{Imm{u64{7}}}).SetType(ValueType::U64);
    function->StoreUniform(Uniform{8, ValueType::U64}, sum);
}

void Unfinished(bool fast) {
    {
        HIRPools pools{1};
        auto* function = CreateFunction(pools, fast);
        AddLiveInstructions(function);
        function->GetEntryBlock()->AppendInst<TypedValue<ValueType::U64>>(
                OpCode::LoadUniform, Uniform{16, ValueType::U64});
        const auto instructions = CaptureInstructions(function);
        pools.Reset();
        CheckReusedInstructions(instructions);
        CheckAllObjectsFree();
    }
    CheckAllObjectsFree();
    InstructionSet outer_instructions;
    {
        HIRBuilder outer{1, true, fast};
        auto* first = outer.AppendFunction(Location{0x100});
        AddLiveInstructions(first);
        outer_instructions = CaptureInstructions(first);
        InstructionSet inner_instructions;
        {
            HIRBuilder inner{1, true, fast};
            auto* second = inner.AppendFunction(Location{0x200});
            second->LoadUniform(Uniform{0, ValueType::U64}).SetType(ValueType::U64);
            inner_instructions = CaptureInstructions(second);
        }
        CheckReusedInstructions(inner_instructions);
        CheckFreeObjects<Block, block_capacity>(block_capacity - 3,
                                                "nested builder did not release only its own blocks");
        CheckFreeObjects<Function, function_capacity>(function_capacity - 1,
                                                      "nested builder did not release its Function");
    }
    CheckReusedInstructions(outer_instructions);
    CheckAllObjectsFree();
}

void Published(bool fast) {
    std::unique_ptr<Function> retained;
    InstructionSet instructions;
    {
        HIRPools pools{1};
        auto* function = CreateFunction(pools, fast);
        AddLiveInstructions(function);
        instructions = CaptureInstructions(function);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        retained.reset(function->GetFunction());
        function->ReleaseFunctionOwnership();
        pools.Reset();
        Check(retained->GetBlocks().size() == 2, "published Function lost real blocks");
        auto* first = retained->FindBlock(Location{0x100});
        auto* second = retained->FindBlock(Location{0x110});
        Check(first && second, "published raw blocks were released with HIR wrappers");
        Check(first->GetInstList().size() == 1 && second->GetInstList().size() == 2,
              "published instructions did not survive HIR cleanup");
        Check(second->GetInstList().begin()->GetArg<Value>(0).Def() ==
                      first->GetInstList().begin().operator->(),
              "published cross-block SSA input changed");
        CheckFreeObjects<Block, block_capacity>(block_capacity - 2,
                                                "synthetic entry was not released independently");
        CheckFreeObjects<Function, function_capacity>(function_capacity - 1,
                                                      "published Function owner was released early");
    }
    retained.reset();
    CheckReusedInstructions(instructions);
    CheckAllObjectsFree();
}

void Construction(bool fast) {
    {
        HIRPools pools{1};
        ExpectAllocationFailure([&] { CreateFunction(pools, true); });
        pools.Reset();
        CheckAllObjectsFree();
    }
    {
        HIRPools pools{1};
        CreateFunction(pools, fast);
        ExpectAllocationFailure([&] { CreateFunction(pools, fast, 0x200); });
        pools.Reset();
        CheckAllObjectsFree();
    }
    {
        HIRValueMap values;
        HIRPools pools{1};
        for (unsigned i = 0; i < 8; ++i) {
            pools.blocks.Create(std::make_unique<Block>(Location{u64{i}}), values, pools);
        }
        ExpectAllocationFailure([&] {
            pools.blocks.Create(std::make_unique<Block>(Location{u64{8}}), values, pools);
        });
        pools.Reset();
        CheckAllObjectsFree();
    }
    {
        HIRValueMap values;
        HIRPools pools{1};
        for (unsigned i = 0; i < 7; ++i) {
            pools.blocks.Create(std::make_unique<Block>(Location{u64{i}}), values, pools);
        }
        // The synthetic entry fits; allocation of the first real block fails.
        ExpectAllocationFailure([&] { CreateFunction(pools, false); });
        pools.Reset();
        CheckAllObjectsFree();
    }
    {
        HIRPools pools{1};
        for (unsigned i = 0; i < 16; ++i) pools.edges.Create(nullptr, nullptr);
        // Both wrappers exist before the failed parent's first edge allocation.
        ExpectAllocationFailure([&] { CreateFunction(pools, false); });
        pools.Reset();
        CheckAllObjectsFree();
    }
}

void PartialFinish(bool fast, bool retry) {
    {
        HIRPools pools{1};
        auto* function = CreateFunction(pools, fast);
        AddLiveInstructions(function);
        const auto instructions = CaptureInstructions(function);
        auto* last = function->AppendBlock(Location{0x120});
        for (unsigned i = 0; i < 128; ++i) function->AddEdge(last, last);
        ExpectAllocationFailure([&] { function->EndFunction(); });
        Check(function->GetFunction()->GetBlocks().size() == 2,
              "failure did not occur after the first two ownership transfers");
        Check(function->GetFunction()->FindBlock(Location{0x120}) == nullptr,
              "failing block was transferred before its graph storage was prepared");
        if (retry) {
            function->EndFunction();
            Check(function->GetFunction()->GetBlocks().size() == 3,
                  "retry duplicated or lost a canonical block");
            Check(function->GetFunction()->FindBlock(Location{0x120}) == last->GetBlock(),
                  "retry did not transfer the exact remaining Block");
        }
        pools.Reset();
        CheckReusedInstructions(instructions);
        CheckAllObjectsFree();
    }
    CheckAllObjectsFree();
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) return 2;
        Block::InitializeSlabHeap(block_capacity);
        Function::InitializeSlabHeap(function_capacity);
        CheckAllObjectsFree();
        const bool fast = IRBuildFastEnabled();
        const std::string_view mode{argv[1]};
        if (mode == "unfinished") Unfinished(fast);
        else if (mode == "published") Published(fast);
        else if (mode == "construction") Construction(fast);
        else if (mode == "finish-retry") {
            PartialFinish(fast, false);
            PartialFinish(fast, true);
        } else return 2;
        CheckAllObjectsFree();
        std::cout << "mode=" << mode << " fast=" << fast << " checks=" << checks
                  << " injected_failures=" << injected_failures << " passed\n";
        return 0;
    } catch (const std::exception& error) {
        fail_next_allocation = false;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
