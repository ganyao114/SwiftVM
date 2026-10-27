#include "runtime/backend/arm64/compile/inputs.h"
#include "tests/support/allocation_fixture.h"
#include "tests/support/register_alloc_test_support.h"
using swift::runtime::test::AllocationFixture;
#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

#include "runtime/backend/address_space.h"
#include "runtime/backend/arm64/compile/compiler.h"
#include "runtime/backend/arm64/compile/register_state.h"
#include "runtime/backend/arm64/compile/target.h"
#include "runtime/backend/arm64/jit/function_code_object_emitter.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/compile/allocate/allocation.h"
#include "runtime/compile/analysis/uses.h"
#include "runtime/compile/analysis/guest_state.h"
#include "runtime/compile/analysis/semantic_facts.h"
#include "runtime/compile/builder/ir_builder.h"
#include "runtime/compile/passes/value_worklist.h"
#include "runtime/compile/pipeline.h"
#include "runtime/compile/unit.h"
#include "runtime/compile/workspace.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/hir_pools.h"

namespace {
thread_local bool fail_next_allocation{};
thread_local size_t allocation_requests{};
thread_local size_t allocation_bytes_requested{};
thread_local size_t fail_allocation_countdown{};
thread_local size_t failed_allocation_request{};
unsigned checks{};
unsigned injected_failures{};
}

// The failure hook is confined to this standalone test executable.
void* operator new(std::size_t size) {
    ++allocation_requests;
    allocation_bytes_requested += size;
    if (std::exchange(fail_next_allocation, false) ||
        (fail_allocation_countdown && --fail_allocation_countdown == 0)) {
        failed_allocation_request = allocation_requests;
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
    std::array<decltype(T::Allocate()), Capacity> objects{};
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
    std::array<const Inst*, 8> addresses{};
    std::size_t count{};
};

InstructionSet CaptureInstructions(const HIRFunction* function) {
    InstructionSet result;
    const auto capture = [&](const HIRBlock& block) {
        for (auto& inst : block.GetInstList()) {
            Check(result.count < result.addresses.size(), "instruction capture capacity exceeded");
            result.addresses[result.count++] = &inst;
        }
    };
    if (function->GetHIRBlockList().empty()) {
        for (const auto* block : function->GetHIRBlocks())
            if (block) capture(*block);
    } else {
        for (const auto& block : function->GetHIRBlockList()) capture(block);
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
    const auto input = function->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
    auto* next = function->AppendBlock(Location{0x110});
    function->AddEdge(function->GetCurrentBlock(), next);
    function->SetCurBlock(next);
    const auto sum = function->Add(ValueType::U64, input, Operand{Imm{u64{7}}});
    function->StoreUniform(Uniform{8, ValueType::U64}, sum);
}

void Unfinished(bool fast) {
    {
        HIRPools pools{1};
        auto* function = CreateFunction(pools, fast);
        AddLiveInstructions(function);
        function->EditBlock(function->GetEntryBlock())
                .AppendInst<TypedValue<ValueType::U64>>(OpCode::LoadUniform,
                                                        Uniform{16, ValueType::U64});
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
            second->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
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
    IntrusivePtr<Function> retained;
    InstructionSet instructions;
    {
        HIRPools pools{1};
        auto* function = CreateFunction(pools, fast);
        AddLiveInstructions(function);
        instructions = CaptureInstructions(function);
        function->EndBlock(terminal::ReturnToHost{});
        function->EndFunction();
        retained.reset(function->GetFunction());
        function->MarkFunctionPublished();
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

void ParamsOwnership(bool fast) {
    constexpr std::size_t capacity = 8;
    struct NodeSlots {
        static void* Allocate() { return HIRTestSupport::AllocateParamsNodeSlot(); }
        static void Free(void* slot) { HIRTestSupport::FreeParamsNodeSlot(slot); }
    };
    HIRTestSupport::InitializeParamsNodeSlab(capacity);
    const auto free_nodes = [&](std::size_t expected) {
        CheckFreeObjects<NodeSlots, capacity>(expected, "Params nodes were leaked or freed twice");
    };
    const auto reject = [&](auto&& operation) {
        bool rejected{};
        try { operation(); }
        catch (const std::logic_error&) { rejected = true; }
        Check(rejected, "invalid Params installation did not reject its null definition");
    };
    free_nodes(capacity);
    Inst definition{OpCode::LoadImm, ValueType::U64};
    definition.SetArgs(Imm{u64{7}});
    const Value source{&definition};

    {
        Params unused;
        unused.Push(source);
        unused.Push(Imm{u64{9}});
        free_nodes(capacity - 2);
        Check(definition.GetUses(false) == 0, "an uninstalled Params owner registered uses");
    }
    free_nodes(capacity);

    // This tests actual scope unwinding, not Node allocation failure: the
    // executable's global operator new hook does not intercept slab/malloc.
    struct LeaveScope {};
    for (const bool adopt : {false, true}) {
        bool unwound{};
        try {
            Params owner;
            owner.Push(source);
            owner.Push(Imm{u64{11}});
            Inst pending{OpCode::CallDynamic, ValueType::VOID};
            if (adopt) {
                if (fast) pending.SetArgsFresh(Lambda{Imm{u64{1}}}, std::move(owner));
                else pending.SetArgs(Lambda{Imm{u64{1}}}, std::move(owner));
            }
            free_nodes(capacity - 2);
            Check(definition.GetUses(false) == (adopt ? 1u : 0u),
                  "Params adoption lost or duplicated a registered occurrence");
            throw LeaveScope{};
        } catch (const LeaveScope&) { unwound = true; }
        Check(unwound, "Params scope did not exercise exception unwinding");
        free_nodes(capacity);
        Check(definition.GetUses(false) == 0, "unwinding retained installed Params uses");
    }

    {
        Params original;
        original.Push(Value{source}.SetCastType(ValueType::U16));
        original.Push(Imm{u64{13}});
        const auto retained = original.begin();
        auto snapshot = *retained;
        Params moved{std::move(original)};
        Check(original.begin() == original.end(), "moving Params did not empty its source");
        snapshot.data = DataClass{Imm{u64{99}}};
        Check((*retained).data.IsValue() && (*retained).data.value.Type() == ValueType::U16,
              "a snapshot retained before moving Params changed the owned node");
        original.Push(Imm{u64{17}});
        Params destination;
        destination.Push(source);
        free_nodes(capacity - 4);
        destination = std::move(moved);
        Check(moved.begin() == moved.end(), "move assignment retained a second Params owner");
        free_nodes(capacity - 3);
        auto* self = &destination;
        destination = std::move(*self);
        free_nodes(capacity - 3);
        Check((*destination.begin()).data.value.Type() == ValueType::U16,
              "Params self move destroyed its original contents");
        moved.Push(Imm{u64{19}});
        free_nodes(capacity - 4);
        moved.Destroy();
        moved.Destroy();
        free_nodes(capacity - 3);
    }
    free_nodes(capacity);
    Check(definition.GetUses(false) == 0, "moving uninstalled Params changed use counts");

    {
        auto call = std::make_unique<Inst>(OpCode::CallDynamic, ValueType::VOID);
        Params owner;
        owner.Push(source);
        owner.Push(Imm{u64{23}});
        owner.Push(source);
        if (fast) call->SetArgsFresh(Lambda{Imm{u64{1}}}, std::move(owner));
        else call->SetArgs(Lambda{Imm{u64{1}}}, std::move(owner));
        Check(owner.begin() == owner.end(), "installation left Params owned by its caller");
        Check(definition.GetUses(false) == 2, "installation lost repeated Params occurrences");
        free_nodes(capacity - 3);
        owner.Push(Imm{u64{29}});
        free_nodes(capacity - 4);
        owner.Destroy();
        free_nodes(capacity - 3);
        Params replacement;
        replacement.Push(source);
        call->SetArg(1, std::move(replacement));
        Check(replacement.begin() == replacement.end(), "replacement did not adopt its owner");
        Check(definition.GetUses(false) == 1, "replacement did not unregister old Params uses");
        free_nodes(capacity - 1);
        call.reset();
    }
    free_nodes(capacity);
    Check(definition.GetUses(false) == 0, "destroying an Inst retained Params uses");

    {
        Inst call{OpCode::CallDynamic, ValueType::VOID};
        Params installed;
        installed.Push(source);
        call.SetArgs(Lambda{Imm{u64{1}}}, std::move(installed));
        {
            Params invalid;
            invalid.Push(source);
            invalid.Push(Value{});
            reject([&] { call.SetArg(1, std::move(invalid)); });
            Check(invalid.begin() != invalid.end(), "failed replacement consumed its owner");
            Check(definition.GetUses(false) == 1,
                  "failed replacement changed old or partially registered new Params uses");
            Check((*call.GetArg<Params>(1).begin()).data.value.Def() == &definition,
                  "failed replacement changed the previously installed Params");
            free_nodes(capacity - 3);
        }
        free_nodes(capacity - 1);
    }
    free_nodes(capacity);
    Check(definition.GetUses(false) == 0, "failed replacement leaked the old Params uses");

    {
        Inst fresh{OpCode::CallDynamic, ValueType::VOID};
        Params invalid;
        invalid.Push(source);
        invalid.Push(Value{});
        reject([&] { fresh.SetArgFresh(1, std::move(invalid)); });
        Check(fresh.ArgAt(1).IsVoid(), "failed fresh adoption wrote the argument slot");
        Check(invalid.begin() != invalid.end(), "failed fresh adoption consumed its owner");
        Check(definition.GetUses(false) == 0, "failed fresh adoption partially registered uses");
        free_nodes(capacity - 2);
    }
    free_nodes(capacity);

    {
        IntrusivePtr<Block> block{new Block{Location{0x118}}};
        {
            Params invalid;
            invalid.Push(source);
            invalid.Push(Value{});
            reject([&] {
                block->AppendInst(OpCode::CallDynamic, ValueType::VOID, Lambda{source},
                                  std::move(invalid));
            });
            Check(block->GetInstList().empty(), "failed Params construction linked an instruction");
            Check(invalid.begin() != invalid.end(), "failed construction consumed uninstalled Params");
            Check(definition.GetUses(false) == 0,
                  "failed construction leaked the earlier Lambda use from its pending Inst");
            free_nodes(capacity - 2);
        }
        free_nodes(capacity);
        Params owner;
        owner.Push(source);
        owner.Push(source);
        const auto call = block->CallDynamic(ValueType::VOID, Lambda{Imm{u64{1}}}, std::move(owner));
        free_nodes(capacity - 2);
        Check(definition.GetUses(false) == 2, "Block construction lost Params uses");
        block->DestroyInst(call.Def());
        Check(block->GetInstList().empty(), "ordinary Params erase did not unlink its instruction");
        Check(definition.GetUses(false) == 0, "ordinary erase did not unregister Params uses");
        free_nodes(capacity);
    }

    {
        auto foreign = std::make_unique<Inst>(OpCode::LoadImm, ValueType::U64);
        foreign->SetArgs(Imm{u64{31}});
        IntrusivePtr<Block> block{new Block{Location{0x119}}};
        Params owner;
        owner.Push(Value{foreign.get()});
        owner.Push(Value{foreign.get()});
        block->CallDynamic(ValueType::VOID, Lambda{Imm{u64{1}}}, std::move(owner));
        Check(foreign->GetUses(false) == 2, "foreign Params fixture did not register both uses");
        free_nodes(capacity - 2);
        const auto* released = foreign.get();
        foreign.reset();
        // Inst storage is reused immediately. Keep a fresh zero-use object at
        // the destroyed definition's address: an accidental UnUse during
        // whole-Block teardown must fail instead of silently touching dead IR.
        auto reused = std::make_unique<Inst>(OpCode::Nop);
        Check(reused.get() == released, "foreign teardown fixture did not reuse the released slot");
        Check(reused->GetUses(false) == 0, "foreign teardown sentinel already has uses");
        block.reset();
        Check(reused->GetUses(false) == 0, "whole teardown changed an unrelated reused Inst");
        free_nodes(capacity);
    }
    free_nodes(capacity);
    CheckAllObjectsFree();
}

void Construction(bool fast) {
    ParamsOwnership(fast);
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
        last->GetBlock()->SetTerminal(terminal::ReturnToHost{});
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

void PrepareConstant(HIRFunction* function, u64 value) {
    const auto input = function->LoadImm(ValueType::U64, Imm{value});
    function->StoreUniform(Uniform{0, ValueType::U64}, input);
    function->EndBlock(terminal::ReturnToHost{});
}

void Retained(bool fast, bool thread_exit, bool defer_seal = false) {
    std::optional<compile::CompileUnit> prepared;
    HIRFunction* function{};
    InstructionSet instructions;
    const auto produce = [&] {
        HIRBuilder builder{1, true, fast};
        function = builder.AppendFunction(Location{0x100}, Location{0x120});
        PrepareConstant(function, 37);
        instructions = CaptureInstructions(function);
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
        function->GetFunction()->SetEndLocation(Location{0x120});
        compile::CompileUnit unit{function};
        if (!defer_seal) unit.SealIR();
        prepared.emplace(std::move(unit));
    };
    if (thread_exit) {
        std::exception_ptr failure;
        std::thread producer([&] {
            try {
                produce();
            } catch (...) {
                failure = std::current_exception();
            }
        });
        producer.join();
        if (failure) std::rethrow_exception(failure);
    } else {
        produce();
    }
    {
        HIRBuilder next{1, true, fast};
        auto* other = next.AppendFunction(Location{0x200});
        PrepareConstant(other, 99);
        other->EndFunction();
        const auto* view = prepared->GetFunction();
        Check(view == function && view->GetFunction()->IsIRReadOnly() == !defer_seal,
              "prepared Function lost its owner or sealing state after the builder ended");
        const auto* root = view->GetHIRBlocks()[1]->GetBlock();
        Check(root->GetInstList().front().GetArg<Imm>(0).Get() == 37,
              "another builder reused live prepared storage");
    }
    if (defer_seal) {
        Check(!prepared->IsSealed(), "preparing input unexpectedly froze its semantic IR");
        prepared->EditFunction()->IdByRPO();
        prepared->SealIR();
    }
    if (thread_exit) {
        Config config{.loc_start = 0,
                      .loc_end = 0x10000,
                      .enable_jit = true,
                      .has_local_operation = false,
                      .backend_isa = kArm64,
                      .uniform_buffer_size = 64};
        backend::AddressSpace space{config};
        auto module = space.GetDefaultModule();
        const auto features = backend::ResolveFeatureSet(module->GetModuleConfig());
        AllocationFixture allocation{function->MaxInstrCount(),
                                     space.GetTrampolines().GetGPRRegs(),
                                     space.GetTrampolines().GetFPRRegs(),
                                     features};
        compile::AllocateFunction(function,
                                  &allocation,
                                  ::swift::runtime::backend::arm64::PrepareAllocationInputs(
                                          function, allocation, features));
        Check(backend::arm64::CompleteARM64UnitForEmission(module, allocation, nullptr, function),
              "prepared Function admission failed after its creating thread exited");
        backend::arm64::JitContext context{module, allocation, allocation.Actions()};
        backend::arm64::JitTranslator translator{context};
        Check(translator.Translate(prepared->GetFunction()) && context.CurrentBufferSize() != 0,
              "prepared Function emission failed after its creating thread exited");
    }
    CheckFreeObjects<Function, function_capacity>(function_capacity - 1,
                                                  "prepared Function was reclaimed early");
    if (!thread_exit) {
        // A full root pool tries to coalesce its capacity on reset. Failure of
        // that optional allocation must safely discard storage, not terminate.
        const auto before = injected_failures;
        fail_next_allocation = true;
        prepared.reset();
        fail_next_allocation = false;
        Check(injected_failures == before + 1, "cache-maintenance failure was not exercised");
    } else {
        prepared.reset();
    }
    CheckReusedInstructions(instructions);
    CheckAllObjectsFree();
    std::optional<compile::CompileUnit> independent;
    {
        IntrusivePtr<Block> block{new Block{Location{0x300}}};
        block->LoadImm(ValueType::U64, Imm{u64{51}});
        block->SetTerminal(terminal::ReturnToHost{});
        block->ReIdInstr();
        compile::CompileUnit unit{block};
        unit.SealIR();
        independent.emplace(std::move(unit));
    }
    Check(independent->GetBlock()->IsIRReadOnly() &&
                  independent->GetBlock()->GetInstList().front().GetArg<Imm>(0).Get() == 51,
          "independent Block did not survive its original owning handle");
    independent.reset();
    CheckAllObjectsFree();
}

void Prepared(bool fast) {
    Config config{.loc_start = 0,
                  .loc_end = 0x10000,
                  .enable_jit = true,
                  .has_local_operation = false,
                  .backend_isa = kArm64,
                  .uniform_buffer_size = 64};
    backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    std::optional<backend::arm64::Arm64PreparedUnit> prepared;
    InstructionSet instructions;
    std::exception_ptr failure;
    std::thread producer([&] {
        try {
            HIRBuilder builder{1, true, fast};
            auto* function = builder.AppendFunction(Location{0x100}, Location{0x120});
            PrepareConstant(function, 37);
            function->EndFunction();
            auto result = backend::arm64::PrepareFunction(module, function);
            Check(result.has_value(), "compiler did not return a prepared result");
            instructions = CaptureInstructions(function);
            prepared.emplace(std::move(*result));
        } catch (...) {
            failure = std::current_exception();
        }
    });
    producer.join();
    if (failure) std::rethrow_exception(failure);
    {
        backend::arm64::Arm64PreparedUnit moved{std::move(*prepared)};
        prepared.reset();
        Check(moved.ReadyForEmission(module) && moved.Unit().IsSealed(),
              "prepared result lost its final admission or IR storage after a move");
        CheckFreeObjects<Function, function_capacity>(
                function_capacity - 1,
                "prepared compiler result released its actual Function early");
        backend::arm64::FunctionCodeObjectEmitter emitter{module, std::span{&moved, 1}};
        Check(emitter.Emit(false) && emitter.CurrentBufferSize() != 0,
              "prepared result could not emit after its builder and producer thread ended");
    }
    CheckReusedInstructions(instructions);
    CheckAllObjectsFree();
}

std::vector<u8> EmitPreparedBytes(const std::shared_ptr<backend::Module>& module,
                                  std::span<const backend::arm64::Arm64PreparedUnit> units) {
    Check(!units.empty(), "delayed emission has no prepared input");
    for (const auto& unit : units)
        Check(unit.ReadyForEmission(module), "delayed prepared input lost its admission");
    if (units.front().Function()) {
        backend::arm64::FunctionCodeObjectEmitter emitter{module, units};
        Check(emitter.Emit(false) && emitter.CurrentBufferSize() != 0,
              "delayed regions failed actual emission");
        const auto* bytes = emitter.Context().GetMasm().GetBuffer()->GetStartAddress<const u8*>();
        return {bytes, bytes + emitter.CurrentBufferSize()};
    }
    Check(units.size() == 1, "independent Blocks must keep separate emission entries");
    const auto& unit = units.front();
    backend::arm64::JitContext context{module, unit.Allocation(), unit.Actions(), false};
    backend::arm64::JitTranslator translator{context};
    Check(translator.Translate(unit.Block()) && context.CurrentBufferSize() != 0,
          "delayed independent Block failed actual emission");
    const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
    return {bytes, bytes + context.CurrentBufferSize()};
}

void PreparedHandoff(bool fast, bool regions) {
    using backend::arm64::Arm64PreparedUnit;
    Config config{.loc_start = 0,
                  .loc_end = 0x10000,
                  .enable_jit = true,
                  .has_local_operation = false,
                  .backend_isa = kArm64,
                  .uniform_buffer_size = 64};
    config.region_edges = true;
    backend::AddressSpace space{config};
    backend::ModuleConfig options;
    options.optimizations = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove |
                            Optimizations::FlagElimination;
    options.feature_overrides.Set(FeatureId::int_imm_fold, true);
    const auto module = space.MapModule(0, 0x10000, options);
    Check(module != nullptr, "could not map the optimized handoff module");
    const auto features = backend::ResolveFeatureSet(module->GetModuleConfig());
    std::optional<std::vector<Arm64PreparedUnit>> pending;
    std::array<std::vector<u8>, 2> individual;
    std::vector<u8> combined;
    std::exception_ptr failure;
    std::thread producer([&] {
        try {
            compile::CompileWorkspace* original;
            {
                compile::CompileWorkspaceScope observe;
                original = &observe.Get();
            }
            pending.emplace();
            if (regions) {
                HIRBuilder builder{2, true, fast};
                std::array<HIRFunction*, 2> functions{};
                for (size_t i = 0; i < functions.size(); ++i) {
                    const auto start = u64{0x100} + i * 0x100;
                    auto* function =
                            builder.AppendFunction(Location{start}, Location{start + 0x20});
                    const auto value = function->Xor(
                            ValueType::U64,
                            function->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64}),
                            Operand{Imm{u64{37} + i * 36}});
                    if (i == 1)
                        builder.SetCurBlock(
                                builder.LinkBlock(terminal::LinkBlock{Location{start + 0x10}}));
                    function->StoreUniform(Uniform{8, ValueType::U64}, value);
                    function->AdvancePC(Imm{u64{1}});
                    function->EndBlock(terminal::ReturnToHost{});
                    function->EndFunction();
                    functions[i] = function;
                }
                auto prepared = backend::arm64::PrepareFunctions(module, functions);
                Check(prepared && prepared->size() == 2, "multi-region preparation failed");
                pending = std::move(prepared);
            } else {
                for (u64 i = 0; i < 2; ++i) {
                    IntrusivePtr<Block> block{new Block{Location{0x100 + i * 0x100}}};
                    const auto value = block->Xor(
                            ValueType::U64,
                            block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64}),
                            Operand{Imm{u64{37} + i * 36}});
                    block->StoreUniform(Uniform{8, ValueType::U64}, value);
                    block->AdvancePC(Imm{u64{1}});
                    block->SetTerminal(terminal::ReturnToHost{});
                    auto prepared = backend::arm64::PrepareBlock(module, block);
                    Check(prepared.has_value(), "independent Block preparation failed");
                    pending->push_back(std::move(*prepared));
                }
            }
            for (size_t i = 0; i < pending->size(); ++i)
                individual[i] = EmitPreparedBytes(module, std::span{&(*pending)[i], 1});
            Check(individual[0] != individual[1], "distinct delayed inputs emitted identical code");
            if (regions) combined = EmitPreparedBytes(module, *pending);
            {
                compile::CompileWorkspaceScope overwrite;
                Check(&overwrite.Get() == original,
                      "sequential preparations did not reuse TLS storage");
                compile::InstructionRowScope rows{overwrite.Get(), 65537};
                for (size_t i = 0; i < 65537; ++i) rows[i] = 0x12345678;
                compile::LoweringCandidateScope candidates{overwrite.Get()};
                candidates.Entries().assign(65537, {17, 0xffff});
            }
            // An actual third compilation reuses the value roster, deletion
            // queues, spill cursor and target preparation after both results
            // have detached. Its larger IR also forces capacity growth.
            const auto before_recompile = SnapshotWorkspaceStorage();
            {
                IntrusivePtr<Block> churn{new Block{Location{0x800}}};
                auto value = churn->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
                for (unsigned i = 0; i < 256; ++i)
                    value = churn->Add(ValueType::U64, value, Operand{Imm{u64{1}}});
                churn->StoreUniform(Uniform{8, ValueType::U64}, value);
                churn->SetTerminal(terminal::ReturnToHost{});
                auto other = backend::arm64::PrepareBlock(module, churn);
                Check(other.has_value(), "workspace reuse compilation failed");
                Check(!EmitPreparedBytes(module, std::span{&*other, 1}).empty(),
                      "workspace reuse result did not emit");
            }
            if (before_recompile.enabled) {
                const auto after_recompile = SnapshotWorkspaceStorage();
                for (auto kind : {WorkspaceBufferKind::ValueRows,
                                  WorkspaceBufferKind::ValueLookup,
                                  WorkspaceBufferKind::ValueQueue}) {
                    const auto index = static_cast<size_t>(kind);
                    Check(after_recompile.buffers[index].allocated_bytes >
                                  before_recompile.buffers[index].allocated_bytes,
                          "third compilation did not grow the actual value workspace");
                }
            }
            for (size_t i = 0; i < pending->size(); ++i)
                Check(EmitPreparedBytes(module, std::span{&(*pending)[i], 1}) == individual[i],
                      "workspace reuse changed an earlier result's machine code");
        } catch (...) {
            failure = std::current_exception();
        }
    });
    producer.join();
    if (failure) std::rethrow_exception(failure);
    {
        std::vector<Arm64PreparedUnit> received;
        // Moving each unit also relocates the vector's outer objects. Internal
        // allocation/actions views must remain tied to their owning storage.
        for (auto& unit : *pending) received.push_back(std::move(unit));
        pending.reset();
        for (size_t i = 0; i < received.size(); ++i)
            Check(EmitPreparedBytes(module, std::span{&received[i], 1}) == individual[i],
                  "producer exit or a result move changed delayed machine code");
        if (regions) {
            Check(received[1].Function()->GetFunction()->GetBlocks().empty(),
                  "secondary region unexpectedly owns the merged raw Blocks");
            Check(EmitPreparedBytes(module, received) == combined,
                  "delayed code-object emission changed after workspace destruction");
        }
        auto survivor = std::move(received.back());
        received.pop_back();
        received.clear();
        Check(EmitPreparedBytes(module, std::span{&survivor, 1}) == individual[1],
              "releasing the first result invalidated the independent/secondary survivor");
    }
    CheckAllObjectsFree();
}

void ConstantWindow() {
    for (bool enabled : {false, true}) {
        IntrusivePtr<Block> block{new Block{Location{0x400}}};
        for (u64 i = 0; i < 256; ++i) block->LoadImm(ValueType::U64, Imm{i});
        block->SetTerminal(terminal::ReturnToHost{});
        compile::CompileWorkspace workspace;
        compile::RewriteContext rewrite{block.get(), workspace, nullptr, false};
        FeatureSet features{};
        features.const_cse = enabled;
        bool allocated{};
        fail_next_allocation = true;
        try {
            compile::ShareConstants(rewrite, features);
        } catch (const std::bad_alloc&) {
            allocated = true;
        }
        const bool unused_failure = std::exchange(fail_next_allocation, false);
        Check(!allocated && unused_failure,
              "unique scalar constants allocated temporary CSE heap storage");
        Check(block->GetInstList().size() == 256,
              "constant reuse unexpectedly deleted a unique definition");
    }
    for (bool scoped : {false, true}) {
        IntrusivePtr<Block> block{new Block{Location{0x500}}};
        compile::IrBuilder builder{block.get()};
        FeatureSet features{};
        features.int_imm_fold = true;
        builder.ConfigureGuestState(nullptr, features, backend::arm64::BuilderTarget());
        for (u64 i = 0; i < 8; ++i) builder.LoadImm(ValueType::U64, Imm{i});
        const auto append_constants = [&] {
            for (u64 i = 8; i < 264; ++i) builder.LoadImm(ValueType::U64, Imm{i});
        };
        bool allocated{};
        fail_next_allocation = true;
        try {
            if (scoped) {
                compile::IrBuilder::GuestInstructionScope instruction{builder};
                append_constants();
            } else {
                append_constants();
            }
        } catch (const std::bad_alloc&) {
            allocated = true;
        }
        const bool unused_failure = std::exchange(fail_next_allocation, false);
        Check(!allocated && unused_failure,
              "builder immediate history kept allocating after its query window was warm");
        Check(block->GetInstList().size() == 264,
              "bounded immediate history lost an appended constant");
    }
    {
        HIRBuilder builder;
        auto* function = builder.AppendFunction(Location{0x600});
        const auto input = builder.LoadImm(ValueType::U8, Imm{1u});
        for (u64 i = 1; i < 128; ++i) builder.LoadImm(ValueType::U8, Imm{i});
        builder.SignExtend(ValueType::S64, input);
        builder.ReturnToHost();
        function->ComputeRPO();
        function->IdByRPO();
        compile::CompileWorkspace workspace;
        {
            compile::RewriteQueueScope warm{workspace};
            warm.Entries().reserve(512);
        }
        compile::RewriteContext rewrite{function, workspace};
        FeatureSet features{};
        features.int_imm_fold = true;
        const compile::BuilderCapabilities target{};
        compile::ValueWorklist work{rewrite, features, target, compile::ValueCleanup::All};
        bool allocated{};
        fail_next_allocation = true;
        try {
            rewrite.CollectDead();
        } catch (const std::bad_alloc&) {
            allocated = true;
        }
        const bool unused_failure = std::exchange(fail_next_allocation, false);
        Check(!allocated && unused_failure,
              "Function DCE allocated a duplicate owner table during a value epoch");
        Check(std::all_of(
                      function->GetHIRBlocks().begin(),
                      function->GetHIRBlocks().end(),
                      [](const HIRBlock* block) { return !block || block->GetInstList().empty(); }),
              "indexed Function DCE failed to collect its dead dependency chain");
    }
    {
        IntrusivePtr<Block> block{new Block{Location{0x700}}};
        const auto condition = block->LoadImm(ValueType::U8, Imm{1u});
        const auto selector = block->LoadImm(ValueType::U64, Imm{u64{2}});
        Vector<terminal::Switch::Case> cases{{Imm{2u}, terminal::ReturnToHost{}}};
        block->SetTerminal(terminal::If{BOOL{condition},
                                        terminal::Switch{selector, cases},
                                        terminal::CheckHalt{terminal::ReturnToDispatch{}}});
        std::array<const Inst*, 2> values{};
        size_t count{};
        bool allocated{};
        fail_next_allocation = true;
        try {
            compile::VisitTerminalValues(block->GetTerminal(), [&](Value value) {
                if (count < values.size()) values[count] = value.Def();
                ++count;
            });
        } catch (const std::bad_alloc&) {
            allocated = true;
        }
        const bool unused_failure = std::exchange(fail_next_allocation, false);
        Check(!allocated && unused_failure,
              "read-only terminal traversal cloned recursive storage");
        Check(count == 2 && values[0] == condition.Def() && values[1] == selector.Def(),
              "borrowed terminal traversal lost a nested operand");
        Terminal snapshot = block->GetTerminal();
        block->SetTerminal(terminal::ReturnToHost{});
        Check(boost::get<terminal::If>(&snapshot) &&
                      boost::get<terminal::ReturnToHost>(&block->GetTerminal()),
              "explicit terminal snapshot changed with the owning Block");
    }
    {
        IntrusivePtr<Block> block{new Block{Location{0x800}}};
        const auto input = block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64});
        const auto result = block->And(ValueType::U64, input, Operand{Imm{u64{0xff}}});
        block->SaveFlags(result, Flags::NZCV);
        block->ClearFlags(Flags::Parity | Flags::AuxiliaryCarry);
        const auto polarity = block->LoadImm(ValueType::U8, Imm{0u});
        block->StoreUniform(Uniform{16, ValueType::U8}, polarity);
        block->AdvancePC(Imm{4u});
        const auto condition = block->LocalCondSet(ValueType::U8, Cond::NE);
        block->BranchOnlyEdges();
        block->SetTerminal(terminal::If{BOOL{condition},
                                        terminal::LinkBlock{Location{0x900}},
                                        terminal::LinkBlock{Location{0xa00}}});
        compile::CompileWorkspace workspace;
        compile::RewriteContext rewrite{block.get(), workspace, nullptr, false};
        FeatureSet features{};
        features.flags_branch_only = true;
        bool allocated{};
        fail_next_allocation = true;
        try {
            compile::EliminateFlags(rewrite, features);
        } catch (const std::bad_alloc&) {
            allocated = true;
        }
        const bool unused_failure = std::exchange(fail_next_allocation, false);
        Check(!allocated && unused_failure,
              "branch-only preparation allocated temporary containers");
        Check(std::count_if(block->GetInstList().begin(),
                            block->GetInstList().end(),
                            [](const Inst& inst) {
                                return inst.GetOp() == OpCode::BranchOnlyFlags;
                            }) == 1,
              "allocation-free branch preparation did not preserve its accepted transform");
    }
    // Equal peak pending storage must not allocate once per publication.
    // This measures C++ new requests/bytes only in the existing test hook.
    const auto sink_requests = [](unsigned flushes, u32 uniform_size = 64, u32 offset = 0) {
        struct Result { size_t requests, bytes; unsigned stores; };
        Block block{Location{0xb00}};
        for (unsigned i = 0; i < flushes; ++i) {
            const auto value = block.LoadImm(ValueType::V128, Imm{u64{i + 1}});
            block.StoreUniform(Uniform{offset, ValueType::V128}, value);
            block.AppendInst(OpCode::UniformBarrier);
        }
        block.AppendInst(OpCode::UniformBarrier);
        block.SetTerminal(terminal::ReturnToHost{});
        compile::GuestStateLayout layout{.uniform_size = uniform_size};
        layout.xmm_uniform_ranges.push_back({offset, offset + 16});
        compile::CompileWorkspace workspace;
        compile::RewriteContext rewrite{&block, workspace, nullptr, false};
        const auto before = allocation_requests;
        const auto bytes_before = allocation_bytes_requested;
        compile::SinkStateStores(rewrite, layout);
        const auto requests = allocation_requests - before;
        const auto bytes = allocation_bytes_requested - bytes_before;
        const auto stores = std::count_if(block.GetInstList().begin(), block.GetInstList().end(),
                                          [](const Inst& inst) {
                                              return inst.GetOp() == OpCode::StoreUniform;
                                          });
        return Result{requests, bytes, static_cast<unsigned>(stores)};
    };
    const auto empty_sink = sink_requests(0);
    const auto small_sink = sink_requests(8), large_sink = sink_requests(64);
    Check(empty_sink.requests == 0 && empty_sink.bytes == 0 && small_sink.requests > 0 &&
                  small_sink.requests == large_sink.requests && small_sink.bytes == large_sink.bytes,
          "state sink heap requests/bytes grew without stores or with flush count at equal peak");
    Check(empty_sink.stores == 0 && small_sink.stores == 8 && large_sink.stores == 64,
          "state sink allocation comparison lost required publications");
    const auto wide_sink = sink_requests(8, 65536);
    const auto shifted_sink = sink_requests(8, 65536, 64000);
    Check(small_sink.requests == wide_sink.requests && small_sink.bytes == wide_sink.bytes &&
                  wide_sink.stores == 8,
          "pending coverage storage scaled with unrelated uniform-buffer bytes");
    Check(small_sink.requests == shifted_sink.requests && small_sink.bytes == shifted_sink.bytes &&
                  shifted_sink.stores == 8,
          "pending coverage retained an unused uniform-buffer prefix");
    // A warmed physical version view must scale with actual guest-state
    // definitions, not the count of unrelated IR instructions.
    const auto version_requests = [](unsigned padding) {
        struct Result { size_t requests, bytes; bool location; };
        Block block{Location{0xb20}};
        const auto value = block.LoadImm(ValueType::U64, Imm{u64{7}});
        block.SetHostGPR(value, HostRegIndex(20), Imm{0u});
        for (unsigned i = 0; i < padding; ++i) block.Nop();
        const auto use = block.Add(ValueType::U64, value, Operand{Imm{u64{1}}});
        block.SetTerminal(terminal::ReturnToHost{});
        block.ReIdInstr();
        compile::SemanticFacts facts{&block};
        backend::arm64::RegisterStatePreparation state;
        state.BeginBlock(&block, FeatureSet{}, facts);
        state.PrepareValueVersions({}, {}, true);
        const auto requests_before = allocation_requests;
        const auto bytes_before = allocation_bytes_requested;
        state.PrepareValueVersions({}, {}, true);
        const auto requests = allocation_requests - requests_before;
        const auto bytes = allocation_bytes_requested - bytes_before;
        const auto location = state.FixedHomeForUse(value, use.Def());
        return Result{requests, bytes, location && location->home == 20 &&
                                      state.ResidentUseCount(value.Def()) == 1};
    };
    const auto short_versions = version_requests(8);
    const auto long_versions = version_requests(2048);
    Check(short_versions.requests > 0 && short_versions.requests == long_versions.requests &&
                  short_versions.bytes == long_versions.bytes,
          "physical version storage grew with unrelated IR after semantic catalog preparation");
    Check(short_versions.location && long_versions.location,
          "physical version allocation comparison lost the actual resident use");
    // Exercise every allocation boundary of the shared version family and
    // the target clobber projection. A failed prepare must publish no answer;
    // retry and warm read-only queries must still use the same sealed input.
    for (bool logical : {false, true}) {
        bool completed{};
        for (size_t fail_at = 1; fail_at <= 128 && !completed; ++fail_at) {
            IntrusivePtr<Block> block{new Block{Location{0xb40}}};
            const auto value = block->GetHostGPR(ValueType::U64, HostRegIndex(20), Imm{0u});
            const auto helper_value = block->CallLambda(ValueType::VOID,
                    Lambda{DataClass{Imm{u64{1}}},
                           HelperCallTraits{.guest_state = HelperGuestStateEffect::ReadOnly,
                                            .reentry = HelperReentryEffect::NoReentry}});
            const auto* helper = helper_value.Def();
            block->Nop();
            const auto* point = &block->GetInstList().back();
            block->SetTerminal(terminal::ReturnToHost{});
            block->ReIdInstr();
            compile::CompileUnit unit{block}; unit.SealIR();
            compile::SemanticFacts facts{block.get()};
            Check(facts.PrepareSSAValidation(), "version failure fixture has invalid SSA");
            backend::arm64::RegisterStatePreparation state;
            state.BeginBlock(block.get(), FeatureSet{}, facts);
            const auto prepare = [&] {
                return logical ? facts.PrepareGuestStateGPRMatch(block.get(), 20, 0, *point,
                                               value, compile::GuestStateEntryPolicy::Block)
                               : state.PrepareFirstClobber(0, value.Id()) == helper->Id();
            };
            bool failed{};
            fail_allocation_countdown = fail_at;
            try { Check(prepare(), "cold guest-state prepare lost its expected result"); }
            catch (const std::bad_alloc&) { failed = true; }
            catch (...) { fail_allocation_countdown = 0; throw; }
            fail_allocation_countdown = 0;
            if (failed && logical)
                Check(!facts.FindGuestStateGPRMatch(block.get(), 20, 0, *point, value,
                                                   compile::GuestStateEntryPolicy::Block),
                      "failed logical prepare published a partial cached answer");
            Check(prepare(), "guest-state preparation failed to recover after allocation failure");
            fail_next_allocation = true;
            bool allocated{};
            try {
                Check(prepare(), "warm guest-state prepare lost its answer");
                if (logical)
                    Check(facts.FindGuestStateGPRMatch(block.get(), 20, 0, *point, value,
                                                      compile::GuestStateEntryPolicy::Block) == true,
                          "const logical lookup lost its prepared answer");
            } catch (const std::bad_alloc&) { allocated = true; }
            const bool unused = std::exchange(fail_next_allocation, false);
            Check(!allocated && unused, "warm guest-state lookup allocated hidden storage");
            completed = !failed;
        }
        Check(completed, "guest-state allocation-failure traversal did not reach completion");
    }
    for (bool primed : {false, true}) {
        bool completed{};
        unsigned failures{};
        for (size_t fail_at = 1; fail_at <= 256 && !completed; ++fail_at) {
            HIRBuilder builder{1, false, FeatureSet{}};
            auto* function = builder.AppendFunction(Location{0xb60});
            auto* entry = function->GetCurrentBlock()->GetBlock();
            const auto wanted = function->LoadImm(ValueType::U64, Imm{u64{7}});
            function->SetHostGPR(wanted, HostRegIndex(20), Imm{0u});
            function->Nop(); const auto* entry_point = &entry->GetInstList().back();
            const auto selector = function->GetHostGPR(ValueType::U64, HostRegIndex(21), Imm{0u});
            Vector<terminal::Switch::Case> cases;
            for (u32 i = 0; i < 18; ++i)
                cases.push_back({Imm{i}, terminal::LinkBlock{Location{0xc00 + i * 16}}});
            const auto arms = builder.Switch(terminal::Switch{selector, cases});
            HIRBlock* join{};
            for (const auto& arm : arms) {
                builder.SetCurBlock(arm.then_);
                const auto arm_condition = function->GetHostGPR(ValueType::U8, HostRegIndex(22), Imm{0u});
                const auto arm_branch = function->Goto(BOOL{arm_condition});
                function->Nop(); function->BindLabel(arm_branch);
                join = builder.LinkBlock(terminal::LinkBlock{Location{0xe00}});
            }
            builder.SetCurBlock(join);
            auto* block = join->GetBlock();
            const auto condition = function->GetHostGPR(ValueType::U8, HostRegIndex(23), Imm{0u});
            for (unsigned i = 0; i < 48; ++i) {
                const auto branch = function->Goto(BOOL{condition});
                function->Nop(); function->BindLabel(branch);
            }
            function->Nop(); const auto* point = &block->GetInstList().back();
            builder.ReturnToHost(); function->ComputeRPO(); function->IdByRPO();
            compile::CompileUnit unit{function}; unit.SealIR();
            compile::SemanticFacts facts{function};
            Check(facts.PrepareSSAValidation(), "wide version fixture has invalid SSA");
            if (primed)
                Check(facts.PrepareGuestStateGPRMatch(entry, 20, 0, *entry_point, wanted,
                                                     compile::GuestStateEntryPolicy::Function),
                      "local Function version could not prime its answer");
            Check(!facts.FindFunctionCFG(), "a local version proof prepared a Function CFG");
            const auto prepare = [&] {
                return facts.PrepareGuestStateGPRMatch(block, 20, 0, *point, wanted,
                                                       compile::GuestStateEntryPolicy::Function);
            };
            const auto before = allocation_requests;
            bool failed{};
            fail_allocation_countdown = fail_at;
            try { Check(prepare(), "wide Function version proof lost its incoming value"); }
            catch (const std::bad_alloc&) { failed = true; ++failures; }
            catch (...) { fail_allocation_countdown = 0; throw; }
            fail_allocation_countdown = 0;
            Check(!failed || (failed_allocation_request == allocation_requests &&
                             allocation_requests - before == fail_at),
                  "Function version recovery allocated while unwinding failure");
            if (failed)
                Check(!facts.FindGuestStateGPRMatch(block, 20, 0, *point, wanted,
                                                   compile::GuestStateEntryPolicy::Function),
                      "failed Function version proof published a partial answer");
            if (primed)
                Check(facts.FindGuestStateGPRMatch(entry, 20, 0, *entry_point, wanted,
                                                  compile::GuestStateEntryPolicy::Function) == true,
                      "failed Function version preparation invalidated an earlier answer");
            Check(prepare(), "Function version retry did not recover its complete state");
            Check(facts.FindFunctionCFG(), "incoming version proof did not prepare shared CFG");
            fail_next_allocation = true;
            bool allocated{};
            try {
                Check(prepare(), "warm Function version proof lost its answer");
                Check(facts.FindGuestStateGPRMatch(block, 20, 0, *point, wanted,
                                                  compile::GuestStateEntryPolicy::Function) == true,
                      "const Function version lookup lost its answer");
            } catch (const std::bad_alloc&) { allocated = true; }
            const bool unused = std::exchange(fail_next_allocation, false);
            Check(!allocated && unused, "warm Function version query allocated storage");
            completed = !failed;
        }
        Check(completed && failures > 0,
              "Function version allocation-failure traversal did not complete its boundaries");
    }
    CheckAllObjectsFree();
}

void StateSinkFailure(bool fast) {
    bool saw_partial_victim_failure{};
    for (unsigned shape : {0u, 1u, 2u})
        for (bool extra_flush : {false, true}) {
            if (shape == 2 && extra_flush) continue;
            unsigned failures{};
            bool completed{};
            for (size_t fail_at = 1; fail_at <= 64 && !completed; ++fail_at) {
                {
                    HIRBuilder builder{2, false, fast, FeatureSet{}};
                    auto* function = shape ? builder.AppendFunction(Location{0xc00}) : nullptr;
                    IntrusivePtr<Block> standalone;
                    if (!function) standalone.reset(new Block{Location{0xc00}});
                    auto* block = function
                            ? function->EditBlock(function->GetCurrentBlock()).GetBlock()
                            : standalone.get();
                    const auto append = [&]<class... Args>(OpCode op, ValueType type,
                                                           const Args&... args) {
                        return function ? function->AppendInst(op, type, args...)
                                        : block->AppendInst(op, type, args...);
                    };
                    std::vector<Value> sources;
                    const auto constant = [&](ValueType type, u64 bits) {
                        const auto value = Value{append(OpCode::LoadImm, type, Imm{bits})};
                        sources.push_back(value);
                        return value;
                    };
                    const auto store = [&](u32 offset, Value value) {
                        append(OpCode::StoreUniform, ValueType::VOID,
                               Uniform{offset, value.Type()}, value);
                    };
                    Value first_victim;
                    const Inst* boundary{};
                    if (shape == 2) {
                        // The second block contains only stores of foreign
                        // definitions. Coverage allocation sees an empty list.
                        for (u64 i = 0; i < 25; ++i) constant(ValueType::U32, i + 1);
                        auto* next = function->AppendBlock(Location{0xc10});
                        function->AddEdge(function->GetCurrentBlock(), next);
                        block->SetTerminal(terminal::LinkBlock{Location{0xc10}});
                        function->SetCurBlock(next);
                        block = function->EditBlock(next).GetBlock();
                        for (auto value : sources) store(0, value);
                        first_victim = sources.front();
                    } else {
                        store(0, constant(ValueType::U32, 1));
                        store(16, constant(ValueType::U32, 2));
                        first_victim = constant(ValueType::U32, 3);
                        store(4, first_victim);
                        store(24, constant(ValueType::U32, 4));
                        store(4, constant(ValueType::U32, 5));
                        for (u64 i = 0; i < 20; ++i)
                            store(32, constant(ValueType::U32, 100 + i));
                        if (extra_flush) store(48, constant(ValueType::V128, 0x1234));
                        const auto observed = Value{append(OpCode::LoadUniform, ValueType::U64,
                                                           Uniform{0, ValueType::U64})};
                        sources.push_back(observed);
                        store(64, observed);
                        boundary = append(OpCode::UniformBarrier, ValueType::VOID);
                    }
                    compile::GuestStateLayout layout{.uniform_size = 96};
                    layout.xmm_uniform_ranges.push_back({0, 64});
                    if (extra_flush) {
                        // Exercise an additional full-width write and whole
                        // flush after the earlier partial flush can erase victims.
                        const auto* early = append(OpCode::UniformBarrier, ValueType::VOID);
                        block->RemoveInst(early);
                        block->InsertBefore(early, boundary);
                    }
                    if (function) builder.ReturnToHost();
                    else block->SetTerminal(terminal::ReturnToHost{});
                    auto unit = function ? compile::CompileUnit{function}
                                         : compile::CompileUnit{standalone};
                    if (function) {
                        function->ComputeRPO();
                        function->IdByRPO();
                    } else block->ReIdInstr();
                    compile::CompileWorkspace workspace;
                    auto rewrite = function ? compile::RewriteContext{function, workspace}
                                            : compile::RewriteContext{block, workspace};
                    // Make deletion notifications use their real Function
                    // queue; it may need to grow after a victim was deleted.
                    rewrite.CollectDead();
                    const auto requests_before = allocation_requests;
                    const auto failures_before = injected_failures;
                    bool failed{};
                    fail_allocation_countdown = fail_at;
                    try { compile::SinkStateStores(rewrite, layout); }
                    catch (const std::bad_alloc&) { failed = true; }
                    catch (...) { fail_allocation_countdown = 0; throw; }
                    fail_allocation_countdown = 0;
                    const auto requests = allocation_requests - requests_before;
                    Check(!failed || (injected_failures == failures_before + 1 &&
                                      failed_allocation_request == allocation_requests &&
                                      requests == fail_at),
                          "sink recovery allocated or failed to preserve the injected exception");
                    std::vector<const Inst*> actual;
                    bool past_boundary{}, restored_after_boundary{};
                    for (const auto* member : rewrite.Blocks())
                        for (const auto& inst : member->GetInstList()) {
                            Check(inst.GetBlock() == member,
                                  "sink left an instruction under the wrong Block owner");
                            actual.push_back(&inst);
                            if (&inst == boundary) past_boundary = true;
                            else if (past_boundary && inst.GetOp() == OpCode::StoreUniform)
                                restored_after_boundary = true;
                        }
                    for (auto source : sources) {
                        size_t uses{};
                        for (const auto* inst : actual)
                            inst->ForEachValue([&](Value value) { uses += value == source; });
                        Check(source.Def()->GetUses(false) == uses,
                              "sink lost a detached store and its raw operand uses");
                        if (function)
                            Check(function->GetHIRValue(source)->GetUses().size() == uses,
                                  "sink recovery changed registered HIR uses");
                    }
                    if (failed) {
                        ++failures;
                        saw_partial_victim_failure |= function && shape == 1 &&
                                first_victim.Def()->GetUses(false) == 0 && restored_after_boundary;
                    } else {
                        Check(requests + 1 == fail_at,
                              "sink failure sweep did not visit every allocation of its successful run");
                        completed = true;
                    }
                    // Never retry or emit failed optimizer input. Unit and
                    // builder teardown must reclaim every recovered node.
                }
                CheckAllObjectsFree();
            }
            Check(completed && failures >= 3,
                  "sink failure sweep missed pending growth or coverage preparation");
        }
    Check(saw_partial_victim_failure,
          "sink sweep missed failure after a victim deletion and pending ownership transfer");
    CheckAllObjectsFree();
}

void OptimizedHandoff(bool fast, bool function_input) {
    using backend::arm64::AllocationPreparation;
    using backend::arm64::JitContext;
    using backend::arm64::JitTranslator;
    using backend::arm64::UnitActions;
    Config config{.loc_start = 0,
                  .loc_end = 0x10000,
                  .enable_jit = true,
                  .has_local_operation = false,
                  .backend_isa = kArm64,
                  .uniform_buffer_size = 64};
    backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    compile::CompileOptions options;
    options.enabled = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove |
                      Optimizations::FlagElimination;
    options.features = backend::ResolveFeatureSet(module->GetModuleConfig());
    options.target = backend::arm64::BuilderTarget();
    options.hoist = backend::arm64::LoopHoistPolicy(space.GetTrampolines().GetGPRRegs());
    std::optional<compile::OptimizedUnit> pending;
    const compile::SemanticFacts* original_facts{};
    const compile::ValueLiveness* original_liveness{};
    const compile::FunctionCFG* original_cfg{};
    std::vector<std::pair<const Block*, const compile::BlockAnalysisIndex*>> original_uses;
    InstructionSet instructions;
    std::exception_ptr failure;
    std::thread producer([&] {
        try {
            compile::CompileWorkspaceScope workspace;
            if (function_input) {
                HIRBuilder builder{2, true, fast};
                auto* function = builder.AppendFunction(Location{0x500}, Location{0x520});
                const auto value = function->Xor(
                        ValueType::U64,
                        function->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64}),
                        Operand{Imm{u64{37}}});
                builder.SetCurBlock(builder.LinkBlock(terminal::LinkBlock{Location{0x510}}));
                function->StoreUniform(Uniform{8, ValueType::U64}, value);
                function->AdvancePC(Imm{u64{1}});
                function->EndBlock(terminal::ReturnToHost{});
                function->EndFunction();
                pending.emplace(compile::OptimizeFunction(
                        compile::CompileUnit{function}, workspace.Get(), options));
                instructions = CaptureInstructions(pending->Function());
            } else {
                IntrusivePtr<Block> block{new Block{Location{0x500}}};
                const auto value =
                        block->Xor(ValueType::U64,
                                   block->LoadUniform(ValueType::U64, Uniform{0, ValueType::U64}),
                                   Operand{Imm{u64{37}}});
                block->StoreUniform(Uniform{8, ValueType::U64}, value);
                block->AdvancePC(Imm{u64{1}});
                block->SetTerminal(terminal::ReturnToHost{});
                pending.emplace(compile::OptimizeBlock(
                        compile::CompileUnit{block}, workspace.Get(), options));
                for (const auto& inst : pending->Block()->GetInstList()) {
                    Check(instructions.count < instructions.addresses.size(),
                          "optimized Block capture exceeded its capacity");
                    instructions.addresses[instructions.count++] = &inst;
                }
            }
            auto& facts = pending->PrepareFacts();
            original_facts = &facts;
            original_liveness = &facts.PrepareValueLiveness();
            if (const auto* function = pending->Function()) {
                original_cfg = &facts.PrepareFunctionCFG();
                Check(!facts.CanUseBlockIntervals(), "handoff Function lost its cross-block flow");
                for (const auto& member : function->GetHIRBlocksRPO())
                    original_uses.emplace_back(member.GetBlock(),
                                               &facts.PrepareUses(member.GetBlock()));
            } else
                original_uses.emplace_back(pending->Block(), &facts.PrepareUses(pending->Block()));
            Check(facts.PrepareSSAValidation(), "optimized producer built invalid SSA");
        } catch (...) {
            failure = std::current_exception();
        }
    });
    producer.join();
    if (failure) std::rethrow_exception(failure);
    std::optional<compile::Allocation> allocation;
    std::optional<UnitActions> actions;
    std::optional<compile::CompileUnit> retained;
    std::vector<u8> original_bytes;
    const auto emit = [&](const Block* block, const HIRFunction* function) {
        JitContext context{module, allocation->View(), *actions, false};
        JitTranslator translator{context};
        Check(function ? translator.Translate(function) : translator.Translate(block),
              "optimized handoff could not emit its allocated input");
        Check(context.CurrentBufferSize() != 0, "optimized handoff emitted no machine code");
        const auto* bytes = context.GetMasm().GetBuffer()->GetStartAddress<const u8*>();
        return std::vector<u8>{bytes, bytes + context.CurrentBufferSize()};
    };
    std::thread consumer([&] {
        try {
            auto input = std::move(*pending);
            pending.reset();
            Check(&input.Facts() == original_facts, "worker handoff replaced the semantic owner");
            Check(input.Facts().FindValueLiveness() == original_liveness &&
                          input.Facts().FindFunctionCFG() == original_cfg,
                  "worker handoff lost cached liveness or CFG");
            const auto* block = input.Block();
            const auto* function = input.Function();
            allocation.emplace(function ? function->MaxInstrCount() : block->MaxInstrId(),
                               space.GetTrampolines().GetGPRRegs(),
                               space.GetTrampolines().GetFPRRegs(),
                               input.Features());
            actions.emplace(allocation->View());
            compile::CompileWorkspaceScope workspace;
            {
                AllocationPreparation preparation{
                        *allocation, input.PrepareFacts(), workspace.Get(), *actions};
                if (function)
                    compile::AllocateFunction(function,
                                              &preparation,
                                              backend::arm64::PrepareAllocationInputs(
                                                      function, preparation, input.Features()));
                else
                    compile::AllocateBlock(block,
                                           &preparation,
                                           backend::arm64::PrepareAllocationInputs(
                                                   block, preparation, input.Features()));
                Check(backend::arm64::CompleteARM64UnitForEmission(
                              module, preparation, block, function),
                      "optimized input failed target admission on the receiving worker");
                Check(input.Facts().FindValueLiveness() == original_liveness &&
                              input.Facts().FindFunctionCFG() == original_cfg,
                      "allocation replaced the retained semantic facts");
                for (const auto& [member, uses] : original_uses)
                    Check(input.Facts().FindUses(member) == uses,
                          "allocation replaced a retained direct-use index");
            }
            original_bytes = emit(block, function);
            retained.emplace(std::move(input).TakeIR());
            Check(emit(retained->GetBlock(), retained->GetFunction()) == original_bytes,
                  "releasing active facts changed the allocated emission");
        } catch (...) {
            failure = std::current_exception();
        }
    });
    consumer.join();
    if (failure) std::rethrow_exception(failure);
    Check(backend::arm64::ARM64UnitReadyForEmission(module,
                                                    allocation->View(),
                                                    *actions,
                                                    retained->GetBlock(),
                                                    retained->GetFunction()),
          "handoff output lost admission after both worker workspaces ended");
    Check(emit(retained->GetBlock(), retained->GetFunction()) == original_bytes,
          "handoff output changed after both workers exited");
    actions.reset();
    allocation.reset();
    retained.reset();
    CheckReusedInstructions(instructions);
    CheckAllObjectsFree();
}

void RegionOwner(bool fast) {
    std::optional<compile::CompileUnit> first_unit, second_unit;
    Function* actual_owner{};
    InstructionSet instructions;
    {
        HIRBuilder first_builder{1, true, fast}, second_builder{1, true, fast};
        auto* first = first_builder.AppendFunction(Location{0x100});
        PrepareConstant(first, 11);
        first->CreateOrGetBlock(Location{0x200});
        instructions = CaptureInstructions(first);
        first->EndFunction();
        first->ComputeRPO();
        first->IdByRPO();
        auto* second = second_builder.AppendFunction(Location{0x200});
        PrepareConstant(second, 22);
        const auto more = CaptureInstructions(second);
        for (size_t i = 0; i < more.count; ++i)
            instructions.addresses[instructions.count++] = more.addresses[i];
        second->EndFunction();
        second->ComputeRPO();
        second->IdByRPO();
        actual_owner = first->GetFunction();
        compile::CompileUnit a{first}, b{second};
        const std::array<compile::CompileUnit*, 2> units{&a, &b};
        Check(compile::CompileUnit::MergeFunctions(units),
              "decoded region ownership did not merge");
        a.SealIR();
        b.SealIR();
        Check(actual_owner->IsIRReadOnly() && second->GetFunction()->IsIRReadOnly(),
              "prepared regions did not seal their header and actual parent");
        first_unit.emplace(std::move(a));
        second_unit.emplace(std::move(b));
    }
    std::thread releaser{[unit = std::move(*first_unit)] { (void)unit.GetFunction(); }};
    first_unit.reset();
    releaser.join();
    const auto* view = second_unit->GetFunction();
    Check(actual_owner->IsIRReadOnly() && view->GetFunction()->IsIRReadOnly(),
          "releasing another region removed the actual parent structure seal");
    Check(view->GetFunction()->GetBlocks().empty(),
          "secondary header unexpectedly owns canonical blocks");
    const auto* root = view->GetHIRBlocks()[1]->GetBlock();
    Check(root->IsIRReadOnly() && root->GetInstList().front().GetArg<Imm>(0).Get() == 22,
          "releasing another region freed the actual persistent Block owner");
    CheckFreeObjects<Function, function_capacity>(
            function_capacity - 2,
            "the prepared region did not retain both its header and canonical owner");
    second_unit.reset();
    CheckReusedInstructions(instructions);
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
        else if (mode == "retained")
            Retained(fast, false);
        else if (mode == "thread-exit")
            Retained(fast, true);
        else if (mode == "preparing")
            Retained(fast, true, true);
        else if (mode == "prepared")
            Prepared(fast);
        else if (mode == "prepared-blocks")
            PreparedHandoff(fast, false);
        else if (mode == "prepared-regions")
            PreparedHandoff(fast, true);
        else if (mode == "constant-window")
            ConstantWindow();
        else if (mode == "state-sink-failure")
            StateSinkFailure(fast);
        else if (mode == "optimized-block")
            OptimizedHandoff(fast, false);
        else if (mode == "optimized-function")
            OptimizedHandoff(fast, true);
        else if (mode == "region-owner")
            RegionOwner(fast);
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
        fail_allocation_countdown = 0;
        std::cerr << error.what() << '\n';
        return 1;
    }
}
