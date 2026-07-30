#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include "aarch64/disasm-aarch64.h"
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/ir_meta.h"
#include "runtime/ir/opts/cfg_analysis_pass.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/flags_elimination_pass.h"
#include "runtime/ir/opts/reid_instr_pass.h"
#include "runtime/ir/opts/register_alloc_pass.h"
#include "runtime/ir/opts/uniform_elimination_pass.h"
#include "runtime/backend/mem_map.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/backend/arm64/jit/jit_context.h"
#include "runtime/backend/arm64/jit/translator.h"
#include "runtime/frontend/x86/decoder.h"
#include "compiler/slang/slang.h"
#include "assembler_riscv64.h"
#include "fmt/format.h"
#include "translator/x86/translator.h"

TEST_CASE("Test compiler") {
    using namespace swift::slang;
    Context context{};
    CompileFile("/Users/swift/CLionProjects/SwiftVM/source/tests/test.slang", context);
}

TEST_CASE("Test runtime ir") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    Inst::InitializeSlabHeap(0x100000);
    Block::InitializeSlabHeap(0x10000);
    Function::InitializeSlabHeap(0x2000);
    HIRBuilder hir_builder{1};
    auto function = hir_builder.AppendFunction(Location{0}, Location{0x10});
    Local local_arg1{
            .id = 0,
            .type = ValueType::U32,
    };
    Local local_arg2 {
            .id = 1,
            .type = ValueType::U32,
    };
    Local local_arg3 {
            .id = 2,
            .type = ValueType::U32,
    };
    function->DefineLocal(local_arg1);
    function->DefineLocal(local_arg2);
    function->DefineLocal(local_arg3);
    auto const1 = function->LoadImm(Imm(UINT32_MAX));
    auto const2 = function->LoadImm(Imm(UINT32_MAX-1));
    function->StoreLocal(local_arg1, const1);
    auto local1 = function->LoadLocal(local_arg1);
    function->StoreLocal(local_arg2, local1);
    auto local2 = function->LoadLocal(local_arg2);
    auto [else_, then_] = hir_builder.If(terminal::If{local2, terminal::LinkBlock{1}, terminal::LinkBlock{2}});
    hir_builder.SetCurBlock(then_);
    function->StoreLocal(local_arg3, const1);
    hir_builder.LinkBlock(terminal::LinkBlock{3});
    hir_builder.SetCurBlock(else_);
    function->StoreLocal(local_arg3, const2);
    hir_builder.LinkBlock(terminal::LinkBlock{3});
    hir_builder.SetCurBlock(3);
    function->StoreUniform(Uniform{0, ValueType::U32}, function->LoadLocal(local_arg3));
    Params params{};
    params.Push(local1);
    params.Push(local2);
    hir_builder.CallDynamic(Lambda(Imm(uint64_t(1))), params);

    hir_builder.Return();
    CFGAnalysisPass::Run(&hir_builder);
    ReIdInstrPass::Run(&hir_builder);
    RegAlloc reg_alloc{function->MaxInstrCount(), GPRSMask{0}, FPRSMask{0}};
    RegisterAllocPass::Run(&hir_builder, &reg_alloc);

    MemMap mem_arena{0x100000, true};

    auto res = mem_arena.Map(0x100000, 0, MemMap::ReadExe, false);
    ASSERT(res);
}

TEST_CASE("Test runtime ir cfg") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    Inst::InitializeSlabHeap(0x100000);
    Block::InitializeSlabHeap(0x10000);
    Function::InitializeSlabHeap(0x2000);
    HIRBuilder hir_builder{1};
    auto function = hir_builder.AppendFunction(Location{0}, Location{0x10});
    Local local_arg1{
            .id = 0,
            .type = ValueType::U32,
    };
    Local local_arg2{
            .id = 1,
            .type = ValueType::U32,
    };
    Local local_arg3{
            .id = 2,
            .type = ValueType::U32,
    };
    function->DefineLocal(local_arg1);
    function->DefineLocal(local_arg2);
    function->DefineLocal(local_arg3);
    auto const1 = function->LoadImm(Imm(UINT32_MAX));
    auto const2 = function->LoadImm(Imm(UINT32_MAX-1));
    function->StoreLocal(local_arg1, const1);
    auto local1 = function->LoadLocal(local_arg1);
    function->StoreLocal(local_arg2, local1);
    auto local2 = function->LoadLocal(local_arg2);
    hir_builder.SetCurBlock(hir_builder.LinkBlock(terminal::LinkBlock{2}));
    function->StoreLocal(local_arg3, const1);
    hir_builder.SetCurBlock(hir_builder.LinkBlock(terminal::LinkBlock{3}));
    function->StoreLocal(local_arg3, const2);
    function->StoreUniform(Uniform{0, ValueType::U32}, function->LoadLocal(local_arg3));
    hir_builder.SetCurBlock(hir_builder.LinkBlock(terminal::LinkBlock{4}));
    Params params{};
    params.Push(local1);
    params.Push(local2);
    hir_builder.CallDynamic(Lambda(Imm(uint64_t(1))), params);

    hir_builder.Return();
    CFGAnalysisPass::Run(&hir_builder);
    ReIdInstrPass::Run(&hir_builder);
#define ARM64_X_REGS_MASK 0b1111111111111111111
    swift::runtime::backend::GPRSMask gprs{ARM64_X_REGS_MASK};
    swift::runtime::backend::FPRSMask fprs{ARM64_X_REGS_MASK};
    RegAlloc reg_alloc{0x100, gprs, fprs};
    RegisterAllocPass::Run(&hir_builder, &reg_alloc);

    assert(local2.Defined());

    MemMap mem_arena{0x100000, true};

    auto res = mem_arena.Map(0x100000, 0, MemMap::ReadExe, false);
    ASSERT(res);

}

TEST_CASE("Test runtime ir loop") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    Inst::InitializeSlabHeap(0x100000);
    Block::InitializeSlabHeap(0x10000);
    Function::InitializeSlabHeap(0x2000);
    HIRBuilder hir_builder{1};
    auto function = hir_builder.AppendFunction(Location{0}, Location{0x10});
    Local local1{
            .id = 0,
            .type = ValueType::U32,
    };
    auto value = function->LoadLocal(local1);
    auto [else_, then_] = hir_builder.If(terminal::If{value, terminal::LinkBlock{1}, terminal::LinkBlock{2}});
    else_->StoreLocal(local1, else_->LoadImm(Imm(UINT32_MAX)));
    then_->StoreLocal(local1, else_->LoadImm(Imm(UINT32_MAX)));
    hir_builder.SetCurBlock(else_);
    hir_builder.LinkBlock(terminal::LinkBlock{0});
    hir_builder.SetCurBlock(then_);
    hir_builder.Return();

    CFGAnalysisPass::Run(&hir_builder);
    ReIdInstrPass::Run(&hir_builder);
}

TEST_CASE("CFG analysis terminates and computes dominators for an irreducible loop") {
    using namespace swift::runtime::ir;

    Inst::InitializeSlabHeap(0x100000);
    Block::InitializeSlabHeap(0x10000);
    Function::InitializeSlabHeap(0x2000);

    HIRBuilder hir_builder{1};
    auto* function = hir_builder.AppendFunction(Location{0}, Location{0x10});
    auto condition = function->LoadImm<BOOL>(Imm{1u});

    // Minimal trigger (five real blocks plus the synthetic entry):
    //
    //            +----------------> exit
    //            |                   ^
    //   root --> left --> loop -------+
    //     |               |  ^
    //     +-----> right ---+  |
    //              ^          |
    //              +----------+
    //
    // `right <-> loop` is an irreducible cycle with entries from root and
    // left. The old one-pass dominator walk first made left dominate exit,
    // then revised loop's dominator after seeing the second cycle entry without
    // propagating that revision to exit. Dominance-frontier construction then
    // chased the stale chain to entry, whose dominator is itself, forever.
    auto [left, right] =
            hir_builder.If(terminal::If{condition,
                                       terminal::LinkBlock{2},
                                       terminal::LinkBlock{1}});
    hir_builder.SetCurBlock(left);
    auto [loop, exit] =
            hir_builder.If(terminal::If{condition,
                                       terminal::LinkBlock{4},
                                       terminal::LinkBlock{3}});
    hir_builder.SetCurBlock(right);
    hir_builder.LinkBlock(terminal::LinkBlock{3});
    hir_builder.SetCurBlock(loop);
    hir_builder.If(terminal::If{condition,
                               terminal::LinkBlock{4},
                               terminal::LinkBlock{2}});
    hir_builder.SetCurBlock(exit);
    hir_builder.Return();

    // Run the analysis in a subprocess so a future termination regression
    // fails this case after two seconds instead of wedging the whole test job.
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        alarm(2);
        CFGAnalysisPass::Run(function);
        alarm(0);

        auto* entry = function->GetEntryBlock();
        const bool dominators_are_correct =
                entry->GetDominator() == entry &&
                function->GetHIRBlocks()[1]->GetDominator() == entry &&
                left->GetDominator() == function->GetHIRBlocks()[1] &&
                right->GetDominator() == function->GetHIRBlocks()[1] &&
                loop->GetDominator() == function->GetHIRBlocks()[1] &&
                exit->GetDominator() == function->GetHIRBlocks()[1];
        _exit(dominators_are_correct ? 0 : 1);
    }

    int status = 0;
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);
}

TEST_CASE("Test riscv64 asm") {
    using namespace swift;
    riscv64::Riscv64Label label{};
    riscv64::ArenaAllocator allocator{};
    riscv64::Riscv64Assembler assembler{&allocator};
    assembler.Add(riscv64::A1, riscv64::A1, riscv64::A1);
    assembler.Bind(&label);
    assembler.Add(riscv64::A1, riscv64::A1, riscv64::A1);
    assembler.Add(riscv64::A1, riscv64::A1, riscv64::A1);
    assembler.Bne(riscv64::A1, riscv64::A2, &label);
    assembler.FinalizeCode();
}

TEST_CASE("Test runtime") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    swift::runtime::Config config {
            .loc_start = 0,
            // Must be a realistic guest range, matching the arm64 translator
            // (translator/arm64/translator.cpp). Module's AddressHashMap
            // reserves one pointer per 1 MB of [loc_start, loc_end): 2^48
            // costs a 2 MB reservation, while UINT64_MAX would ask mmap for
            // 128 TB and abort in AllocateMemoryPages.
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    // Module::Push takes ownership: it calls IntrusivePtrAddRef, and the
    // matching release eventually runs Block::operator delete -> SlabObject::
    // TryFree, which hands a non-slab pointer to libc free(). A stack-allocated
    // Block therefore ends the test with free() on a stack address (SIGABRT).
    // Heap-allocate, matching how TranslateIR feeds Push in runtime.cpp.
    auto* block1 = new Block(0, Location{1});
    auto* block2 = new Block(1, Location{2});
    REQUIRE(module->Push(block1));
    REQUIRE(module->Push(block2));
}

TEST_CASE("Test block ir print") {
    using namespace swift::runtime::backend;
    using namespace swift::runtime::ir;
    Block block{0, Location{0x1000}};
    auto imm32 = block.LoadImm(Imm{8u}).SetType(ValueType::U32);
    auto imm8 = block.LoadImm<BOOL>(Imm{8u}).SetType(ValueType::U8);
    block.StoreUniform(Uniform{32, ValueType::U32}, imm8);
    Params params{};
    params.Push(imm8);
    params.Push(imm8);
    block.CallDynamic(Lambda(Imm(uint64_t(1))), params);
    block.SaveFlags(imm8, Flags{Flags::NZCV});
    block.SetTerminal(terminal::If(terminal::If{imm8, terminal::LinkBlock{0x1000}, terminal::LinkBlock{0x2000}}));
    std::cout << block.ToString() << std::endl;
}

TEST_CASE("Uniform elimination does not propagate conditional stores past labels") {
    using namespace swift::runtime::ir;

    UniformInfo info{.uniform_size = 64};

    Block conditional{0, Location{0x1000}};
    auto condition = conditional.LoadImm<BOOL>(Imm{1u});
    auto skip_store = conditional.NotGoto(condition);
    auto stored = conditional.LoadImm(Imm{0u}).SetType(ValueType::U8);
    conditional.StoreUniform(Uniform{32, ValueType::U8}, stored);
    conditional.BindLabel(skip_store);
    auto merged_load = conditional.LoadUniform(Uniform{32, ValueType::U8});

    UniformEliminationPass::Run(&conditional, info);
    REQUIRE(merged_load.Def()->GetOp() == OpCode::LoadUniform);

    Block straight_line{1, Location{0x2000}};
    auto straight_value = straight_line.LoadImm(Imm{0u}).SetType(ValueType::U8);
    straight_line.StoreUniform(Uniform{32, ValueType::U8}, straight_value);
    auto straight_load = straight_line.LoadUniform(Uniform{32, ValueType::U8});

    UniformEliminationPass::Run(&straight_line, info);
    REQUIRE(straight_load.Def()->GetOp() == OpCode::BitExtract);
}

TEST_CASE("Uniform fast path is instruction-identical to the legacy pass") {
    using namespace swift::runtime::ir;

    UniformInfo info{.uniform_size = 64};
    auto make_uniform_block = [] {
        auto block = std::make_unique<Block>(0, Location{0x3000});

        // Full overwrite: the first store is dead.
        auto first = block->LoadImm(Imm{0x1111111111111111ull}).SetType(ValueType::U64);
        auto latest = block->LoadImm(Imm{0x2222222222222222ull}).SetType(ValueType::U64);
        block->StoreUniform(Uniform{0, ValueType::U64}, first);
        block->StoreUniform(Uniform{0, ValueType::U64}, latest);

        // Narrow forwarding must stay a BitExtract with the same source/type.
        block->LoadUniform(Uniform{2, ValueType::U16});

        // The opaque call invalidates forwarding facts. The following load
        // must remain a LoadUniform in both implementations.
        Params params{};
        block->CallDynamic(Lambda(Imm{1u}), params);
        block->LoadUniform(Uniform{2, ValueType::U16});

        // Two overlapping later stores jointly cover the earlier U64 store.
        // This pins byte-range aliasing rather than just exact-offset DSE.
        auto wide = block->LoadImm(Imm{0x3333333333333333ull}).SetType(ValueType::U64);
        auto low = block->LoadImm(Imm{0x44444444u}).SetType(ValueType::U32);
        auto high = block->LoadImm(Imm{0x55555555u}).SetType(ValueType::U32);
        block->StoreUniform(Uniform{16, ValueType::U64}, wide);
        block->StoreUniform(Uniform{16, ValueType::U32}, low);
        block->StoreUniform(Uniform{20, ValueType::U32}, high);
        // Spans two different stored values, so it must not forward.
        block->LoadUniform(Uniform{18, ValueType::U32});
        return block;
    };

    auto legacy = make_uniform_block();
    auto fast = make_uniform_block();
    UniformEliminationPass::Run(legacy.get(), info, false);
    UniformEliminationPass::Run(fast.get(), info, true);

    // ToString includes every opcode, result type, argument and defining id;
    // equality is therefore a per-instruction IR comparison, not just counts.
    REQUIRE(fast->ToString() == legacy->ToString());
    size_t stores = 0;
    size_t loads = 0;
    size_t extracts = 0;
    for (const auto& inst : fast->GetInstList()) {
        stores += inst.GetOp() == OpCode::StoreUniform;
        loads += inst.GetOp() == OpCode::LoadUniform;
        extracts += inst.GetOp() == OpCode::BitExtract;
    }
    REQUIRE(stores == 3);
    REQUIRE(loads == 2);
    REQUIRE(extracts == 1);

    // No-uniform early return must preserve even an otherwise trivial block.
    Block legacy_plain{1, Location{0x4000}};
    Block fast_plain{1, Location{0x4000}};
    legacy_plain.LoadImm(Imm{7u}).SetType(ValueType::U32);
    fast_plain.LoadImm(Imm{7u}).SetType(ValueType::U32);
    UniformEliminationPass::Run(&legacy_plain, info, false);
    UniformEliminationPass::Run(&fast_plain, info, true);
    REQUIRE(fast_plain.ToString() == legacy_plain.ToString());
}

namespace {

struct IRBuildFixture {
    explicit IRBuildFixture(bool fast)
            : builder(1, true, fast) {
        using namespace swift::runtime::ir;

        function = builder.AppendFunction(Location{0x1000}, Location{0x1200});
        const Local local{.id = 7, .type = ValueType::U64};
        function->DefineLocal(local);

        auto imm = function->LoadImm(Imm{0x1122334455667788ull})
                           .SetType(ValueType::U64);
        first_inst = imm.Def();
        auto uniform =
                function->LoadUniform(Uniform{24, ValueType::U64}).SetType(ValueType::U64);
        auto operand =
                function->Add(imm, Operand{uniform, imm, OperandLsl}).SetType(ValueType::U64);
        auto selected =
                function->CondSelect(Cond::EQ, operand, uniform).SetType(ValueType::U64);
        function->SaveFlags(selected, Flags::All);
        function->StoreLocal(local, selected);
        auto local_value = function->LoadLocal(local).SetType(ValueType::U64);

        // Lambda-as-Value plus three ordinary Value slots.
        [[maybe_unused]] auto lambda_result =
                function->CallLambda(Lambda{selected}, imm, uniform, operand)
                        .SetType(ValueType::U64);

        // Params deliberately repeats `imm`: order and duplicates are part of
        // the HIR use-list contract, not a set.
        Params params{};
        params.Push(imm);
        params.Push(Imm{0xa5a5u});
        params.Push(uniform);
        params.Push(imm);
        [[maybe_unused]] auto dynamic_result =
                function->CallDynamic(Lambda{Imm{0x1234ull}}, params)
                        .SetType(ValueType::U64);
        function->StoreUniform(Uniform{40, ValueType::U64}, local_value);

        // More than one 64 KiB Inst arena chunk even at the minimum possible
        // slot size. All instructions stay live until both fixtures have been
        // compared, pinning pointer stability across growth.
        for (unsigned i = 0; i < 1536; ++i) {
            function->Nop();
        }

        auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x1100}});
        builder.SetCurBlock(next);
        auto tail = function->Add(selected, Operand{imm}).SetType(ValueType::U64);
        function->StoreUniform(Uniform{48, ValueType::U64}, tail);
        function->EndBlock(terminal::ReturnToDispatch{});
        function->EndFunction();
        function->ComputeRPO();
        function->IdByRPO();
    }

    swift::runtime::ir::HIRBuilder builder;
    swift::runtime::ir::HIRFunction* function{};
    swift::runtime::ir::Inst* first_inst{};
};

std::string IRSnapshotArg(swift::runtime::ir::Arg& arg) {
    using namespace swift::runtime::ir;
    switch (arg.GetType()) {
        case ArgType::Void:
            return "void";
        case ArgType::Value: {
            const auto value = arg.Get<Value>();
            return fmt::format("value:{}:{}", value.Id(), static_cast<unsigned>(value.Type()));
        }
        case ArgType::Imm: {
            const auto imm = arg.Get<Imm>();
            return fmt::format("imm:{}:{}", static_cast<unsigned>(imm.GetType()), imm.Get());
        }
        case ArgType::Cond:
            return fmt::format("cond:{}", static_cast<unsigned>(arg.Get<Cond>()));
        case ArgType::Flags:
            return fmt::format("flags:{}", static_cast<std::uint64_t>(arg.Get<Flags>()));
        case ArgType::Operand: {
            const auto op = arg.Get<Operand::Op>();
            return fmt::format(
                    "operand:{}:{}", static_cast<unsigned>(op.type), op.shift_ext);
        }
        case ArgType::Local: {
            const auto local = arg.Get<Local>();
            return fmt::format(
                    "local:{}:{}", local.id, static_cast<unsigned>(local.type));
        }
        case ArgType::Uniform: {
            const auto uniform = arg.Get<Uniform>();
            return fmt::format("uniform:{}:{}",
                               uniform.GetOffset(),
                               static_cast<unsigned>(uniform.GetType()));
        }
        case ArgType::Lambda: {
            const auto lambda = arg.Get<Lambda>();
            if (lambda.IsValue()) {
                const auto value = lambda.GetValue();
                return fmt::format(
                        "lambda-value:{}:{}", value.Id(), static_cast<unsigned>(value.Type()));
            }
            const auto imm = lambda.GetImm();
            return fmt::format(
                    "lambda-imm:{}:{}", static_cast<unsigned>(imm.GetType()), imm.Get());
        }
        case ArgType::Params: {
            std::string out{"params"};
            for (const auto& param : arg.Get<Params>()) {
                if (param.data.IsValue()) {
                    out += fmt::format(
                            ":v{}:{}", param.data.value.Id(),
                            static_cast<unsigned>(param.data.value.Type()));
                } else {
                    out += fmt::format(
                            ":i{}:{}", static_cast<unsigned>(param.data.imm.GetType()),
                            param.data.imm.Get());
                }
            }
            return out;
        }
    }
    return "invalid";
}

}  // namespace

TEST_CASE("IR build fast path is field-identical across all argument shapes") {
    using namespace swift::runtime::ir;

    IRBuildFixture legacy{false};
    IRBuildFixture fast{true};
    auto* lhs = legacy.function;
    auto* rhs = fast.function;

    REQUIRE(lhs->MaxBlockCount() == rhs->MaxBlockCount());
    REQUIRE(lhs->MaxInstrCount() == rhs->MaxInstrCount());
    REQUIRE(lhs->MaxLocalCount() == rhs->MaxLocalCount());
    REQUIRE(lhs->GetHIRBlocks().size() == rhs->GetHIRBlocks().size());
    REQUIRE(lhs->GetHIRBlocksRPO().size() == rhs->GetHIRBlocksRPO().size());

    // The first instruction was allocated before >1536 later live objects.
    // Its address and initialized fields must remain valid after arena growth.
    REQUIRE(legacy.first_inst->GetOp() == OpCode::LoadImm);
    REQUIRE(fast.first_inst->GetOp() == OpCode::LoadImm);
    REQUIRE(legacy.first_inst->GetArg<Imm>(0).Get() == 0x1122334455667788ull);
    REQUIRE(fast.first_inst->GetArg<Imm>(0).Get() == 0x1122334455667788ull);

    for (std::size_t block_index = 0; block_index < lhs->GetHIRBlocks().size();
         ++block_index) {
        auto* left_hir = lhs->GetHIRBlocks()[block_index];
        auto* right_hir = rhs->GetHIRBlocks()[block_index];
        INFO("block " << block_index);
        REQUIRE(left_hir->GetOrderId() == right_hir->GetOrderId());
        REQUIRE(left_hir->GetPredecessors().size() == right_hir->GetPredecessors().size());
        REQUIRE(left_hir->GetSuccessors().size() == right_hir->GetSuccessors().size());
        for (std::size_t i = 0; i < left_hir->GetPredecessors().size(); ++i) {
            REQUIRE(left_hir->GetPredecessors()[i]->GetOrderId() ==
                    right_hir->GetPredecessors()[i]->GetOrderId());
        }
        for (std::size_t i = 0; i < left_hir->GetSuccessors().size(); ++i) {
            REQUIRE(left_hir->GetSuccessors()[i]->GetOrderId() ==
                    right_hir->GetSuccessors()[i]->GetOrderId());
        }

        auto* left_block = left_hir->GetBlock();
        auto* right_block = right_hir->GetBlock();
        REQUIRE(left_block->GetStartLocation() == right_block->GetStartLocation());
        REQUIRE(left_block->GetEndLocation() == right_block->GetEndLocation());
        REQUIRE(fmt::format("{}", left_block->GetTerminal()) ==
                fmt::format("{}", right_block->GetTerminal()));

        auto left_it = left_block->GetInstList().begin();
        auto right_it = right_block->GetInstList().begin();
        for (; left_it != left_block->GetInstList().end() &&
               right_it != right_block->GetInstList().end();
             ++left_it, ++right_it) {
            const auto& left_inst = *left_it;
            const auto& right_inst = *right_it;
            INFO("instruction id " << left_inst.Id());
            REQUIRE(left_inst.GetOp() == right_inst.GetOp());
            REQUIRE(left_inst.Id() == right_inst.Id());
            REQUIRE(left_inst.ReturnType() == right_inst.ReturnType());
            REQUIRE(left_inst.VirRegID() == right_inst.VirRegID());
            REQUIRE(const_cast<Inst&>(left_inst).GetUses(false) ==
                    const_cast<Inst&>(right_inst).GetUses(false));
            for (unsigned slot = 0; slot < Inst::max_args; ++slot) {
                INFO("physical argument slot " << slot);
                REQUIRE(IRSnapshotArg(left_inst.ArgAt(slot)) ==
                        IRSnapshotArg(right_inst.ArgAt(slot)));
            }
            auto left_pseudo = const_cast<Inst&>(left_inst).GetPseudoOperations();
            auto right_pseudo = const_cast<Inst&>(right_inst).GetPseudoOperations();
            REQUIRE(left_pseudo.size() == right_pseudo.size());
            for (std::size_t i = 0; i < left_pseudo.size(); ++i) {
                REQUIRE(left_pseudo[i]->Id() == right_pseudo[i]->Id());
                REQUIRE(left_pseudo[i]->GetOp() == right_pseudo[i]->GetOp());
            }
        }
        REQUIRE(left_it == left_block->GetInstList().end());
        REQUIRE(right_it == right_block->GetInstList().end());
    }

    const auto& left_values = lhs->GetHIRValues();
    const auto& right_values = rhs->GetHIRValues();
    REQUIRE(left_values.size() == right_values.size());
    for (std::size_t id = 0; id < left_values.size(); ++id) {
        auto* left_value = left_values[id];
        auto* right_value = right_values[id];
        INFO("HIR value id " << id);
        REQUIRE((left_value == nullptr) == (right_value == nullptr));
        if (!left_value) {
            continue;
        }
        REQUIRE(left_value->value.Id() == right_value->value.Id());
        REQUIRE(left_value->value.Type() == right_value->value.Type());
        REQUIRE(left_value->block->GetOrderId() == right_value->block->GetOrderId());
        REQUIRE(left_value->allocated.type == right_value->allocated.type);

        auto left_use = left_value->uses.begin();
        auto right_use = right_value->uses.begin();
        for (; left_use != left_value->uses.end() && right_use != right_value->uses.end();
             ++left_use, ++right_use) {
            REQUIRE(left_use->inst->Id() == right_use->inst->Id());
            REQUIRE(left_use->arg_idx == right_use->arg_idx);
        }
        REQUIRE(left_use == left_value->uses.end());
        REQUIRE(right_use == right_value->uses.end());
    }
}

TEST_CASE("Uniform elimination preserves rotate-by-zero carry polarity load") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    // cmp si,sp; rol r15b,cl; lahf; seto r15b; hlt
    std::array<swift::u8, 13> code{
            0x66, 0x44, 0x39, 0xe6, 0x41, 0xd2, 0xc7,
            0x9f, 0x41, 0x0f, 0x90, 0xc7, 0xf4,
    };
    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    const auto address = reinterpret_cast<VAddr>(code.data());
    Block block{0, Location{address}};
    Assembler assembler{&block};
    X64Decoder decoder{address, &memory, &assembler, true};
    decoder.Decode();

    UniformInfo info{.uniform_size = sizeof(ThreadContext64)};
    UniformEliminationPass::Run(&block, info);

    const auto polarity_offset = offsetof(ThreadContext64, carry_inverted);
    size_t polarity_loads = 0;
    for (auto& inst : block.GetInstList()) {
        if (inst.GetOp() == OpCode::LoadUniform &&
            inst.GetArg<Uniform>(0).GetOffset() == polarity_offset) {
            polarity_loads++;
        }
    }
    REQUIRE(polarity_loads == 1);
}

TEST_CASE("Constant CSE does not reuse a constant materialized under a branch") {
    using namespace swift::runtime::ir;

    // ConstFoldingPass deduplicates LoadImm within a sliding window, and resets
    // its table at Goto / NotGoto / BindLabel because the x86 front end builds
    // intra-block control flow. BindLabel is the reset that carries the
    // correctness: a constant materialized between a forward branch and its
    // label has NOT executed on the path that took the branch, so rewriting a
    // use after the label to that definition reads a register the block never
    // wrote. (The resets at the branch itself are redundant with it -- code
    // before a forward branch dominates everything after -- and are kept only
    // to scope facts to one straight-line region; see the comment in the pass.)
    //
    // The shape is real: DecodeShift (decoder_alu.cc) materializes LoadImm(0)
    // for SAR's OF inside exactly such a NotGoto/BindLabel region, a handful of
    // IR instructions before the label.
    Block conditional{0, Location{0x1000}};
    auto cond = conditional.LoadImm<BOOL>(Imm{1u});
    auto skip = conditional.NotGoto(cond);
    auto guarded = conditional.LoadImm(Imm{0x1234u});
    conditional.StoreUniform(Uniform{0, ValueType::U32}, guarded);
    conditional.BindLabel(skip);
    auto merged = conditional.LoadImm(Imm{0x1234u});
    conditional.StoreUniform(Uniform{8, ValueType::U32}, merged);
    conditional.SetTerminal(terminal::ReturnToDispatch{});

    ConstFoldingPass::Run(&conditional);

    auto arg_of_store = [](Block& block, std::uint32_t offset) -> Inst* {
        Inst* found = nullptr;
        for (auto& inst : block.GetInstList()) {
            if (inst.GetOp() == OpCode::StoreUniform &&
                inst.GetArg<Uniform>(0).GetOffset() == offset) {
                found = inst.GetArg<Value>(1).Def();
            }
        }
        return found;
    };

    // The store after the label must still read the constant materialized
    // after the label. Removing the BindLabel reset rewrites it to `guarded`.
    REQUIRE(arg_of_store(conditional, 8) == merged.Def());
    REQUIRE(arg_of_store(conditional, 8) != guarded.Def());

    // Positive control. Without it the case above would also pass if the pass
    // did no deduplication at all -- which is exactly how a barrier ends up
    // "covered" by a test that proves nothing. Same two constants, same
    // distance, no branch between them: here the second one MUST be folded away.
    Block straight_line{1, Location{0x2000}};
    auto first = straight_line.LoadImm(Imm{0x1234u});
    straight_line.StoreUniform(Uniform{0, ValueType::U32}, first);
    auto second = straight_line.LoadImm(Imm{0x1234u});
    straight_line.StoreUniform(Uniform{8, ValueType::U32}, second);
    straight_line.SetTerminal(terminal::ReturnToDispatch{});

    ConstFoldingPass::Run(&straight_line);

    REQUIRE(arg_of_store(straight_line, 8) == first.Def());
    REQUIRE(arg_of_store(straight_line, 8) != second.Def());
}

TEST_CASE("MMX-form shared opcodes are refused instead of run on the XMM file") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    // MMX and SSE share opcode numbers: distorm reports the same insn.opcode
    // for `paddb mm0,mm1` (0F FC C1) and `paddb xmm0,xmm1` (66 0F FC C1), and
    // only the operand register class distinguishes them. x86_regs_table maps
    // R_MM0..R_MM7 onto X86RegInfo::Xmm0..Xmm7, so before the guard in
    // DecodeSwitch an MMX-form instruction did not fail -- it read and wrote
    // the guest's XMM0-XMM7 and produced silently wrong data. This runtime
    // implements no MMX register file and does not advertise MMX (CPUID leaf 1
    // EDX bit 23 is clear), so the only correct answer is to refuse the
    // instruction: InterruptReason::FALLBACK, which the runtime turns into
    // IllegalCode.
    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    constexpr auto kInterruptOffset = offsetof(ThreadContext64, interrupt);
    constexpr auto kXmmBase = offsetof(ThreadContext64, xmms);
    constexpr auto kXmmEnd = kXmmBase + sizeof(ThreadContext64::xmms);

    struct Outcome {
        bool refused;      // a FALLBACK interrupt was raised
        bool touched_xmm;  // the XMM register file was read or written
    };
    auto decode = [&](std::vector<swift::u8> bytes) {
        bytes.push_back(0xF4);  // hlt, so Decode() terminates
        const auto address = reinterpret_cast<VAddr>(bytes.data());
        Block block{0, Location{address}};
        Assembler assembler{&block};
        X64Decoder decoder{address, &memory, &assembler, true};
        decoder.Decode();

        Outcome out{false, false};
        for (auto& inst : block.GetInstList()) {
            const bool is_store = inst.GetOp() == OpCode::StoreUniform;
            const bool is_load = inst.GetOp() == OpCode::LoadUniform;
            if (!is_store && !is_load) {
                continue;
            }
            const auto offset = inst.GetArg<Uniform>(0).GetOffset();
            if (offset >= kXmmBase && offset < kXmmEnd) {
                out.touched_xmm = true;
            }
            if (is_store && offset == kInterruptOffset) {
                auto reason = inst.GetArg<Value>(1);
                if (reason.Defined() && reason.Def()->GetOp() == OpCode::LoadImm &&
                    reason.Def()->GetArg<Imm>(0).Get() ==
                            static_cast<swift::u32>(InterruptReason::FALLBACK)) {
                    out.refused = true;
                }
            }
        }
        return out;
    };

    struct Case {
        const char* name;
        std::vector<swift::u8> mmx;  // no 66 prefix: MM operands
        std::vector<swift::u8> sse;  // 66-prefixed: XMM operands
    };
    // The seven named in the original report, plus a spread across the rest of
    // the shared space (the plain MMX/SSE2 integer set, the SSSE3 additions and
    // the GPR<->vector movers) -- the defect was never specific to seven
    // opcodes, it was a property of the dispatch.
    const std::vector<Case> cases = {
            {"pshufb", {0x0F, 0x38, 0x00, 0xC1}, {0x66, 0x0F, 0x38, 0x00, 0xC1}},
            {"palignr", {0x0F, 0x3A, 0x0F, 0xC1, 0x03}, {0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x03}},
            {"pextrw", {0x0F, 0xC5, 0xC1, 0x02}, {0x66, 0x0F, 0xC5, 0xC1, 0x02}},
            {"pmovmskb", {0x0F, 0xD7, 0xC1}, {0x66, 0x0F, 0xD7, 0xC1}},
            {"pmuludq", {0x0F, 0xF4, 0xC1}, {0x66, 0x0F, 0xF4, 0xC1}},
            {"psadbw", {0x0F, 0xF6, 0xC1}, {0x66, 0x0F, 0xF6, 0xC1}},
            {"pminub", {0x0F, 0xDA, 0xC1}, {0x66, 0x0F, 0xDA, 0xC1}},
            {"paddb", {0x0F, 0xFC, 0xC1}, {0x66, 0x0F, 0xFC, 0xC1}},
            {"psubb", {0x0F, 0xF8, 0xC1}, {0x66, 0x0F, 0xF8, 0xC1}},
            {"pand", {0x0F, 0xDB, 0xC1}, {0x66, 0x0F, 0xDB, 0xC1}},
            {"pxor", {0x0F, 0xEF, 0xC1}, {0x66, 0x0F, 0xEF, 0xC1}},
            {"pcmpeqb", {0x0F, 0x74, 0xC1}, {0x66, 0x0F, 0x74, 0xC1}},
            {"punpcklbw", {0x0F, 0x60, 0xC1}, {0x66, 0x0F, 0x60, 0xC1}},
            {"packsswb", {0x0F, 0x63, 0xC1}, {0x66, 0x0F, 0x63, 0xC1}},
            {"pmullw", {0x0F, 0xD5, 0xC1}, {0x66, 0x0F, 0xD5, 0xC1}},
            {"pmaddwd", {0x0F, 0xF5, 0xC1}, {0x66, 0x0F, 0xF5, 0xC1}},
            {"pavgb", {0x0F, 0xE0, 0xC1}, {0x66, 0x0F, 0xE0, 0xC1}},
            {"pmaxsw", {0x0F, 0xEE, 0xC1}, {0x66, 0x0F, 0xEE, 0xC1}},
            {"paddusb", {0x0F, 0xDC, 0xC1}, {0x66, 0x0F, 0xDC, 0xC1}},
            {"psllw imm", {0x0F, 0x71, 0xF0, 0x03}, {0x66, 0x0F, 0x71, 0xF0, 0x03}},
            {"psllw reg", {0x0F, 0xF1, 0xC1}, {0x66, 0x0F, 0xF1, 0xC1}},
            {"pinsrw", {0x0F, 0xC4, 0xC0, 0x02}, {0x66, 0x0F, 0xC4, 0xC0, 0x02}},
            {"movd", {0x0F, 0x6E, 0xC0}, {0x66, 0x0F, 0x6E, 0xC0}},
            {"pabsb", {0x0F, 0x38, 0x1C, 0xC1}, {0x66, 0x0F, 0x38, 0x1C, 0xC1}},
            {"pmulhrsw", {0x0F, 0x38, 0x0B, 0xC1}, {0x66, 0x0F, 0x38, 0x0B, 0xC1}},
            // paddq is the one shared opcode distorm reports with XMM operand
            // indices in BOTH forms, so the register-class test cannot see it
            // and DecodeSwitch reads the 0x66 prefix instead. It is here as a
            // register form and as a memory form, because the prefix scan walks
            // the encoding and the memory form has a different byte layout.
            {"paddq", {0x0F, 0xD4, 0xC1}, {0x66, 0x0F, 0xD4, 0xC1}},
            {"paddq mem", {0x0F, 0xD4, 0x01}, {0x66, 0x0F, 0xD4, 0x01}},
            {"paddq rex", {0x0F, 0xD4, 0xC1}, {0x66, 0x44, 0x0F, 0xD4, 0xC1}},
            // paddq mm0,[rcx+0x66]: the displacement byte IS 0x66. A prefix
            // scan that does not stop at the 0x0F escape reads it as an
            // operand-size prefix and lets this MMX instruction through.
            {"paddq disp=0x66",
             {0x0F, 0xD4, 0x41, 0x66},
             {0x66, 0x0F, 0xD4, 0x41, 0x66}},
    };

    for (auto& c : cases) {
        INFO("MMX form of " << c.name);
        auto mmx = decode(c.mmx);
        // Refusing is the point; not touching the XMM file is what refusing
        // BUYS, and is the assertion that fails if the guard is removed.
        REQUIRE_FALSE(mmx.touched_xmm);
        REQUIRE(mmx.refused);
    }
    for (auto& c : cases) {
        // The 66-prefixed twin must be unaffected: a guard that rejected both
        // forms would pass every assertion above while breaking SSE2.
        INFO("SSE form of " << c.name);
        auto sse = decode(c.sse);
        REQUIRE_FALSE(sse.refused);
        REQUIRE(sse.touched_xmm);
    }
}

TEST_CASE("Uniform elimination uses the latest full GPR store for a narrow load") {
    using namespace swift::runtime;
    using namespace swift::runtime::ir;
    using namespace swift::x86;

    // sub rbx,rdx; popcnt r10w,bx; lahf; seto r15b; hlt
    std::array<swift::u8, 15> code{
            0x48, 0x29, 0xd3, 0xf3, 0x66, 0x44, 0x0f, 0xb8,
            0xd3, 0x9f, 0x41, 0x0f, 0x90, 0xc7, 0xf4,
    };
    struct MemIf final : MemoryInterface {
        bool Read(void* dest, size_t addr, size_t size) override {
            return std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
        }
        bool Write(void* src, size_t addr, size_t size) override {
            return std::memcpy(reinterpret_cast<void*>(addr), src, size);
        }
        void* GetPointer(void* src) override { return src; }
    } memory;

    const auto address = reinterpret_cast<VAddr>(code.data());
    Block block{0, Location{address}};
    Assembler assembler{&block};
    X64Decoder decoder{address, &memory, &assembler, true};
    decoder.Decode();

    const auto rbx_offset = offsetof(ThreadContext64, rbx);
    Value latest_rbx_store{};
    Inst* narrow_rbx_load = nullptr;
    for (auto& inst : block.GetInstList()) {
        if (inst.GetOp() == OpCode::StoreUniform &&
            inst.GetArg<Uniform>(0).GetOffset() == rbx_offset) {
            latest_rbx_store = inst.GetArg<Value>(1);
        } else if (inst.GetOp() == OpCode::LoadUniform) {
            auto uniform = inst.GetArg<Uniform>(0);
            if (uniform.GetOffset() == rbx_offset &&
                uniform.GetType() == ValueType::U16) {
                narrow_rbx_load = &inst;
            }
        }
    }
    REQUIRE(latest_rbx_store.Defined());
    REQUIRE(narrow_rbx_load != nullptr);

    UniformInfo info{.uniform_size = sizeof(ThreadContext64)};
    UniformEliminationPass::Run(&block, info);

    REQUIRE(narrow_rbx_load->GetOp() == OpCode::BitExtract);
    REQUIRE(narrow_rbx_load->GetArg<Value>(0) == latest_rbx_store);

    // Execute the same sequence without Unicorn. The old bug passed the full
    // post-sub RBX value to Popcnt64 after the narrow load was folded.
    void* guest_code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    std::memcpy(guest_code, code.data(), code.size());
    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();
    auto* core = swift::translator::x86::X86Core::Make(instance);
    auto& context = core->GetContext();
    context.rip.qword = reinterpret_cast<VAddr>(guest_code);
    context.rbx.qword = UINT64_MAX;
    // RBX - RDX = 0xffffffffffff0003. Its full-width popcount is 50,
    // while popcnt BX must see only 0x0003 and return 2.
    context.rdx.qword = 0xFFFC;
    context.r10.qword = 0xFFFF0000;
    core->Run();
    const auto popcount = context.r10.qword & 0xFFFF;
    swift::translator::x86::X86Core::Destroy(core);
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(guest_code, 4096);
    REQUIRE(popcount == 2);
}

TEST_CASE("JIT BitExtract feeds an exact U16 value to CallLambda") {
    using namespace swift::runtime;
    using namespace swift::x86;

    // add cx,r8w; popcnt bx,cx; lahf; seto r15b; hlt
    std::array<swift::u8, 15> code{
            0x66, 0x44, 0x01, 0xc1, 0xf3, 0x66, 0x0f, 0xb8,
            0xd9, 0x9f, 0x41, 0x0f, 0x90, 0xc7, 0xf4,
    };
    void* guest_code = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE(guest_code != MAP_FAILED);
    std::memcpy(guest_code, code.data(), code.size());
    backend::SmcTracker::SetEnabled(false);
    auto* instance = swift::translator::x86::X86Instance::Make();

    bool matched = true;
    swift::u64 failed_rcx{};
    swift::u64 failed_r8{};
    swift::u16 expected{};
    swift::u16 actual{};
    swift::u64 random = 0x9E3779B97F4A7C15ull;
    for (int iteration = 0; iteration < 1024; ++iteration) {
        random ^= random << 7;
        random ^= random >> 9;
        auto rcx = random;
        random ^= random << 8;
        auto r8 = random;

        auto* core = swift::translator::x86::X86Core::Make(instance);
        auto& context = core->GetContext();
        context.rip.qword = reinterpret_cast<VAddr>(guest_code);
        context.rcx.qword = rcx;
        context.r8.qword = r8;
        context.rbx.qword = 0xA4297CCD266D0000ull;
        core->Run();
        expected = static_cast<swift::u16>(
                std::popcount(static_cast<swift::u16>(rcx + r8)));
        actual = static_cast<swift::u16>(context.rbx.qword);
        swift::translator::x86::X86Core::Destroy(core);
        if (actual != expected) {
            matched = false;
            failed_rcx = rcx;
            failed_r8 = r8;
            break;
        }
    }
    swift::translator::x86::X86Instance::Destroy(instance);
    backend::SmcTracker::SetEnabled(true);
    munmap(guest_code, 4096);

    INFO(fmt::format("rcx={:#x}, r8={:#x}, expected={}, actual={}",
                     failed_rcx, failed_r8, expected, actual));
    REQUIRE(matched);
}

TEST_CASE("Test x86 translator") {
    using namespace swift::x86;
    using namespace swift::translator::x86;
    auto instance = X86Instance::Make();
    auto core1 = X86Core::Make(instance);

    core1->Run();
}

TEST_CASE("IR meta consistency") {
    using namespace swift::runtime::ir;

    // (1) The constexpr projection (ir.inc via meta::kPhys) must agree with the
    // runtime table summed through the single slot rule, PhysicalSlots.
    constexpr int kPhysByOp[] = {
            0,  // OpCode::Void
#define INST(name, ret, ...) meta::kPhys_##name,
#include "runtime/ir/ir.inc"
#undef INST
    };
    for (int op = 1; op < (int)OpCode::BASE_COUNT; op++) {
        auto& info = GetIRMetaInfo((OpCode)op);
        int runtime_slots = 0;
        for (auto t : info.arg_types) {
            runtime_slots += PhysicalSlots(t);
        }
        REQUIRE(runtime_slots == kPhysByOp[op]);
        REQUIRE(runtime_slots <= Inst::max_args);
    }

    // (2) StoreMemory(Operand, Value): GetArg<Value>(1) must resolve to the right
    // physical slot without the old PublicIndex out-of-bounds read.
    Block block{0, Location{0x1000}};
    auto base = block.LoadImm(Imm{0x1000u});
    auto idx = block.LoadImm(Imm{0x8u});
    auto stored = block.LoadImm(Imm{42u});
    Operand operand{base, idx};
    auto* store = block.AppendInst(OpCode::StoreMemory, operand, stored);
    REQUIRE(store->GetArg<Value>(1) == stored);

    // (3) Operand round-trip: GetArg<Operand>(0) rebuilds op/left/right.
    Operand got = store->GetArg<Operand>(0);
    REQUIRE(got.GetOp() == operand.GetOp());
    REQUIRE(got.GetLeft().IsValue());
    REQUIRE(got.GetLeft().value == base);
    REQUIRE(got.GetRight().IsValue());
    REQUIRE(got.GetRight().value == idx);
}

TEST_CASE("Flag elimination keeps live pseudo masks and unlinks dead pseudos") {
    using namespace swift::runtime::ir;

    // Dead-pseudo removal, exercised on a carry-free mask. A SaveFlags whose
    // bits are all rewritten before any read is dropped, and the surviving
    // write keeps its original (un-narrowed) mask.
    Block overwritten{0, Location{0x1000}};
    auto lhs = overwritten.LoadImm(Imm{1u});
    auto rhs = overwritten.LoadImm(Imm{2u});
    auto old_result = overwritten.Add(lhs, Operand{rhs});
    overwritten.AppendInst(OpCode::SaveFlags, old_result, Flags::NZ);
    auto new_result = overwritten.Add(lhs, Operand{rhs});
    auto* new_save = overwritten.AppendInst(OpCode::SaveFlags, new_result, Flags::NZ);

    FlagsEliminationPass::Run(&overwritten);

    REQUIRE(old_result.Def()->GetPseudoOperations(OpCode::SaveFlags).empty());
    auto new_pseudos = new_result.Def()->GetPseudoOperations(OpCode::SaveFlags);
    REQUIRE(new_pseudos.size() == 1);
    REQUIRE(new_pseudos[0] == new_save);
    REQUIRE(new_pseudos[0]->GetArg<Flags>(1) == Flags::NZ);
    size_t save_count = 0;
    for (auto& inst : overwritten.GetInstList()) {
        save_count += inst.GetOp() == OpCode::SaveFlags;
    }
    REQUIRE(save_count == 1);

    // A carry write covered by a later carry write in the same block is dead.
    // SVM_FLAG_CARRY_ELIM=0 retains the old Gate B behavior for bisection.
    Block carry_overwritten{0, Location{0x1800}};
    auto c_lhs = carry_overwritten.LoadImm(Imm{1u});
    auto c_rhs = carry_overwritten.LoadImm(Imm{2u});
    auto c_old = carry_overwritten.Add(c_lhs, Operand{c_rhs});
    carry_overwritten.AppendInst(OpCode::SaveFlags, c_old, Flags::All);
    auto c_new = carry_overwritten.Add(c_lhs, Operand{c_rhs});
    carry_overwritten.AppendInst(OpCode::SaveFlags, c_new, Flags::All);

    FlagsEliminationPass::Run(&carry_overwritten);

    const bool carry_elim_off = std::getenv("SVM_FLAG_CARRY_ELIM") &&
                                std::strcmp(std::getenv("SVM_FLAG_CARRY_ELIM"), "0") == 0;
    REQUIRE(c_old.Def()->GetPseudoOperations(OpCode::SaveFlags).size() ==
            (carry_elim_off ? 1 : 0));
    REQUIRE(c_new.Def()->GetPseudoOperations(OpCode::SaveFlags).size() == 1);
    size_t carry_save_count = 0;
    for (auto& inst : carry_overwritten.GetInstList()) {
        carry_save_count += inst.GetOp() == OpCode::SaveFlags;
    }
    REQUIRE(carry_save_count == (carry_elim_off ? 2 : 1));

    Block carry_read{0, Location{0x2000}};
    auto carry_lhs = carry_read.LoadImm(Imm{5u});
    auto carry_rhs = carry_read.LoadImm(Imm{3u});
    auto carry_source = carry_read.Add(carry_lhs, Operand{carry_rhs});
    auto* carry_save = carry_read.AppendInst(OpCode::SaveFlags, carry_source, Flags::All);
    auto sbb_result = carry_read.Sbb(carry_lhs, Operand{carry_rhs});
    carry_read.AppendInst(OpCode::SaveFlags, sbb_result, Flags::All);

    FlagsEliminationPass::Run(&carry_read);

    // Sbb reads the preceding carry, so its producer stays live. Its mask must
    // remain whole: the JIT cannot safely turn this into a C-only pseudo.
    auto carry_pseudos = carry_source.Def()->GetPseudoOperations(OpCode::SaveFlags);
    REQUIRE(carry_pseudos.size() == 1);
    REQUIRE(carry_pseudos[0] == carry_save);
    REQUIRE(carry_pseudos[0]->GetArg<Flags>(1) == Flags::All);
}

TEST_CASE("Flag elimination removes only overwritten in-block carry writes") {
    using namespace swift::runtime::ir;

    auto count_op = [](Block& block, OpCode op) {
        size_t count = 0;
        for (auto& inst : block.GetInstList()) {
            count += inst.GetOp() == op;
        }
        return count;
    };
    auto contains = [](Block& block, Inst* wanted) {
        for (auto& inst : block.GetInstList()) {
            if (&inst == wanted) {
                return true;
            }
        }
        return false;
    };
    auto append_carry_save = [](Block& block, Value lhs, Value rhs) {
        auto result = block.Add(lhs, Operand{rhs});
        return block.AppendInst(OpCode::SaveFlags, result, Flags::Carry);
    };

    const bool carry_elim_off = std::getenv("SVM_FLAG_CARRY_ELIM") &&
                                std::strcmp(std::getenv("SVM_FLAG_CARRY_ELIM"), "0") == 0;
    if (carry_elim_off) {
        // The bisect switch covers every deletion introduced by this change.
        // Carry writes of all three forms survive exactly as under old Gate B;
        // unrelated non-carry DSE remains enabled.
        Block disabled{0, Location{0x3000}};
        auto lhs = disabled.LoadImm(Imm{1u});
        auto rhs = disabled.LoadImm(Imm{2u});
        auto* old_save = append_carry_save(disabled, lhs, rhs);
        auto* old_clear = disabled.AppendInst(OpCode::ClearFlags, Flags::Carry);
        auto carry_value = disabled.LoadImm<BOOL>(Imm{1u});
        auto* old_set = disabled.AppendInst(OpCode::SetCarry, carry_value);
        auto* old_noncarry =
                disabled.AppendInst(OpCode::ClearFlags, Flags::Overflow);
        auto* last_save = append_carry_save(disabled, lhs, rhs);
        auto* last_noncarry =
                disabled.AppendInst(OpCode::ClearFlags, Flags::Overflow);

        FlagsEliminationPass::Run(&disabled);

        REQUIRE(contains(disabled, old_save));
        REQUIRE(contains(disabled, old_clear));
        REQUIRE(contains(disabled, old_set));
        REQUIRE_FALSE(contains(disabled, old_noncarry));
        REQUIRE(contains(disabled, last_save));
        REQUIRE(contains(disabled, last_noncarry));
        REQUIRE(count_op(disabled, OpCode::SaveFlags) == 2);
        REQUIRE(count_op(disabled, OpCode::ClearFlags) == 2);
        REQUIRE(count_op(disabled, OpCode::SetCarry) == 1);
        return;
    }

    // (1) Straight line, no intervening reader: the earlier C write is dead.
    Block overwritten{1, Location{0x3100}};
    auto lhs = overwritten.LoadImm(Imm{1u});
    auto rhs = overwritten.LoadImm(Imm{2u});
    auto* first = append_carry_save(overwritten, lhs, rhs);
    auto* last = append_carry_save(overwritten, lhs, rhs);

    FlagsEliminationPass::Run(&overwritten);

    REQUIRE_FALSE(contains(overwritten, first));
    REQUIRE(contains(overwritten, last));
    REQUIRE(count_op(overwritten, OpCode::SaveFlags) == 1);

    // (2) A C reader between two writers keeps both writers live.
    Block read_between{2, Location{0x3200}};
    lhs = read_between.LoadImm(Imm{3u});
    rhs = read_between.LoadImm(Imm{4u});
    first = append_carry_save(read_between, lhs, rhs);
    read_between.AppendInst(OpCode::TestFlags, Flags::Carry);
    last = append_carry_save(read_between, lhs, rhs);

    FlagsEliminationPass::Run(&read_between);

    REQUIRE(contains(read_between, first));
    REQUIRE(contains(read_between, last));
    REQUIRE(count_op(read_between, OpCode::SaveFlags) == 2);

    // (3) Flags::All at block exit makes a lone final C write live.
    Block live_out{3, Location{0x3300}};
    lhs = live_out.LoadImm(Imm{5u});
    rhs = live_out.LoadImm(Imm{6u});
    auto* only = append_carry_save(live_out, lhs, rhs);

    FlagsEliminationPass::Run(&live_out);

    REQUIRE(contains(live_out, only));
    REQUIRE(count_op(live_out, OpCode::SaveFlags) == 1);

    // (4) On one path C is read before the later covering write; on the other
    // path the branch skips directly to that write. The earlier writer is live
    // on the union of both backward paths.
    Block branched{4, Location{0x3400}};
    lhs = branched.LoadImm(Imm{7u});
    rhs = branched.LoadImm(Imm{8u});
    first = append_carry_save(branched, lhs, rhs);
    auto condition = branched.LoadImm<BOOL>(Imm{1u});
    auto skip_read = branched.NotGoto(condition);
    branched.AppendInst(OpCode::TestFlags, Flags::Carry);
    branched.BindLabel(skip_read);
    last = append_carry_save(branched, lhs, rhs);

    FlagsEliminationPass::Run(&branched);

    REQUIRE(contains(branched, first));
    REQUIRE(contains(branched, last));
    REQUIRE(count_op(branched, OpCode::SaveFlags) == 2);

    // (5) ClearFlags(C) and SetCarry are ordinary C writers for liveness.
    Block clear_covered{5, Location{0x3500}};
    lhs = clear_covered.LoadImm(Imm{9u});
    rhs = clear_covered.LoadImm(Imm{10u});
    auto* clear = clear_covered.AppendInst(OpCode::ClearFlags, Flags::Carry);
    last = append_carry_save(clear_covered, lhs, rhs);

    FlagsEliminationPass::Run(&clear_covered);

    REQUIRE_FALSE(contains(clear_covered, clear));
    REQUIRE(contains(clear_covered, last));
    REQUIRE(count_op(clear_covered, OpCode::ClearFlags) == 0);

    Block set_covered{6, Location{0x3600}};
    lhs = set_covered.LoadImm(Imm{11u});
    rhs = set_covered.LoadImm(Imm{12u});
    auto carry_value = set_covered.LoadImm<BOOL>(Imm{1u});
    auto* set = set_covered.AppendInst(OpCode::SetCarry, carry_value);
    last = append_carry_save(set_covered, lhs, rhs);

    FlagsEliminationPass::Run(&set_covered);

    REQUIRE_FALSE(contains(set_covered, set));
    REQUIRE(contains(set_covered, last));
    REQUIRE(count_op(set_covered, OpCode::SetCarry) == 0);

    // (7) Gate A remains block-wide: any Adc/Sbb leaves every instruction
    // untouched, including otherwise-covered C writers.
    Block gate_a{7, Location{0x3700}};
    lhs = gate_a.LoadImm(Imm{13u});
    rhs = gate_a.LoadImm(Imm{14u});
    first = append_carry_save(gate_a, lhs, rhs);
    clear = gate_a.AppendInst(OpCode::ClearFlags, Flags::Carry);
    carry_value = gate_a.LoadImm<BOOL>(Imm{1u});
    set = gate_a.AppendInst(OpCode::SetCarry, carry_value);
    gate_a.Sbb(lhs, Operand{rhs});
    last = append_carry_save(gate_a, lhs, rhs);

    FlagsEliminationPass::Run(&gate_a);

    REQUIRE(contains(gate_a, first));
    REQUIRE(contains(gate_a, clear));
    REQUIRE(contains(gate_a, set));
    REQUIRE(contains(gate_a, last));
    REQUIRE(count_op(gate_a, OpCode::SaveFlags) == 2);
    REQUIRE(count_op(gate_a, OpCode::ClearFlags) == 1);
    REQUIRE(count_op(gate_a, OpCode::SetCarry) == 1);
}

TEST_CASE("Register allocation gives every spilled value a private slot") {
    using namespace swift::runtime::ir;
    using swift::runtime::backend::RegAlloc;
    using swift::runtime::backend::GPRSMask;
    using swift::runtime::backend::FPRSMask;

    // Two live values sharing one State::spill_area slot is a silent
    // miscompile: both Str/Ldr target the same address, so each reload
    // observes the other's value. Squeeze the linear scan onto its spill path
    // and assert the slots are disjoint, counting a SIMD spill as the two u64
    // slots its 16-byte access actually covers.
    auto check_disjoint_slots = [](int n_scalar,
                                   int n_vector,
                                   std::uint32_t free_gprs,
                                   std::uint32_t free_fprs) {
        Block block{0, Location{0x1000}};
        std::vector<Value> scalars;
        for (int i = 0; i < n_scalar; i++) {
            scalars.push_back(block.LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
        }
        std::vector<Value> vectors;
        for (int i = 0; i < n_vector; i++) {
            vectors.push_back(block.LoadImm<TypedValue<ValueType::V128>>(
                    Imm{static_cast<std::uint64_t>(i + 0x100)}));
        }
        // Every value needs a use to own a live interval, and consuming them
        // in reverse definition order keeps them all live at once.
        for (int i = n_vector - 1; i >= 0; i--) {
            block.VecAdd<TypedValue<ValueType::V128>>(vectors[i], vectors[i], Imm{32u});
        }
        if (!scalars.empty()) {
            Value acc = scalars.back();
            for (int i = n_scalar - 2; i >= 0; i--) {
                acc = block.Add(acc, Operand{scalars[i]});
            }
        }
        block.ReIdInstr();  // AppendInst does not assign ids; the pass indexes by id

        // bit SET = unavailable; leave only the lowest `free_*` clear.
        GPRSMask gprs{~((1u << free_gprs) - 1u)};
        FPRSMask fprs{~((1u << free_fprs) - 1u)};
        RegAlloc reg_alloc{0x200, gprs, fprs};
        RegisterAllocPass::Run(&block, &reg_alloc);

        std::map<std::uint32_t, int> occupancy;
        int spilled = 0;
        auto record = [&](const std::vector<Value>& values, int slot_width) {
            for (auto& value : values) {
                if (reg_alloc.ValueType(value) != RegAlloc::MEM) {
                    continue;
                }
                spilled++;
                const std::uint32_t slot = reg_alloc.ValueMem(value).offset;
                for (int k = 0; k < slot_width; k++) {
                    occupancy[slot + k]++;
                }
            }
        };
        record(scalars, 1);
        record(vectors, 2);

        REQUIRE(spilled >= 2);  // the case must actually reach the spill path
        for (auto& [slot, owners] : occupancy) {
            INFO("spill slot " << slot << " claimed by " << owners << " live values");
            REQUIRE(owners == 1);
        }
    };

    check_disjoint_slots(12, 0, 2, 8);  // scalar-only pressure
    check_disjoint_slots(0, 10, 8, 2);  // vector-only pressure
    check_disjoint_slots(10, 6, 2, 2);  // mixed
    check_disjoint_slots(5, 5, 1, 1);   // odd-sized stack before a SIMD pair
}

TEST_CASE("Single-block register allocation is map-identical to the general path") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    // One real block containing all high-risk shapes: fixed host-register
    // aliases (both crossing and non-crossing writes), a host call, a terminal-
    // only value, a BitCast alias, and enough simultaneous scalar liveness to
    // force spills in the deliberately small register pool below.
    HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(Location{0x1000}, Location{0x1100});

    auto pinned =
            function->GetHostGPR(HostRegIndex(0), Imm{0u}).SetType(ValueType::U64);
    auto snapshot =
            function->GetHostGPR(HostRegIndex(1), Imm{0u}).SetType(ValueType::U64);
    auto fixed_fpr =
            function->GetHostFPR(HostRegIndex(2), Imm{0u}).SetType(ValueType::V128);
    auto fpr_snapshot =
            function->GetHostFPR(HostRegIndex(7), Imm{0u}).SetType(ValueType::V128);

    std::vector<Value> live;
    for (std::uint64_t i = 0; i < 14; ++i) {
        live.push_back(function->LoadImm(Imm{0x100u + i}).SetType(ValueType::U64));
    }
    auto alias = function->BitCast(live[3]).SetType(ValueType::U64);

    // The SetHostGPR crosses snapshot's lifetime, so snapshot must receive a
    // normal allocation instead of aliasing host register 1.
    function->SetHostGPR(live[0], HostRegIndex(1), Imm{0u});
    // Same snapshot rule for pinned SIMD state: this write crosses
    // fpr_snapshot's lifetime, so the old v7 value must first be copied to a
    // dynamic FPR (or a spill slot).
    function->SetHostFPR(fixed_fpr, HostRegIndex(7), Imm{0u});
    auto pinned_use = function->Add(pinned, Operand{live[1]}).SetType(ValueType::U64);
    auto snapshot_use =
            function->Add(snapshot, Operand{alias}).SetType(ValueType::U64);

    Params params{};
    params.Push(live[2]);
    params.Push(snapshot_use);
    params.Push(pinned_use);
    function->CallDynamic(Lambda{Imm{1ull}}, params);

    // Consume the pressure values after the call so all of them cross it.
    Value sum = live.back();
    for (int i = static_cast<int>(live.size()) - 2; i >= 0; --i) {
        sum = function->Add(sum, Operand{live[i]}).SetType(ValueType::U64);
    }
    sum = function->Add(sum, Operand{snapshot_use}).SetType(ValueType::U64);
    function->StoreUniform(Uniform{0, ValueType::U64}, sum);
    function->StoreUniform(Uniform{16, ValueType::V128}, fixed_fpr);
    function->StoreUniform(Uniform{32, ValueType::V128}, fpr_snapshot);

    auto terminal_cond = function->TestNotZero(live[4]);
    function->EndBlock(terminal::If{
            terminal_cond, terminal::ReturnToDispatch{}, terminal::ReturnToHost{}});
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
    REQUIRE(function->GetHIRBlocksRPO().size() == 1);

    // Bits set are unavailable. Leave six allocatable GPRs/FPRs, less the
    // default scratch reserve, so the block must take the spill path.
    constexpr std::uint32_t available_gprs = 0x000003f0u;  // x4..x9
    constexpr std::uint32_t available_fprs = 0x00000f78u;  // v3..v6, v8..v11
    GPRSMask gprs{~available_gprs};
    FPRSMask fprs{~available_fprs};
    RegAlloc general{function->MaxInstrCount(), gprs, fprs};
    RegAlloc fast{function->MaxInstrCount(), gprs, fprs};
    RegAlloc selected{function->MaxInstrCount(), gprs, fprs};

    RegisterAllocPass::Run(function, &general, false);
    RegisterAllocPass::Run(function, &fast, true);
    RegisterAllocPass::Run(function, &selected);

    REQUIRE(general.MapCount() == fast.MapCount());
    bool saw_spill = false;
    for (std::uint32_t id = 0; id < general.MapCount(); ++id) {
        INFO("allocation map id " << id);
        REQUIRE(general.Mapping(id) == fast.Mapping(id));
        saw_spill |= general.Mapping(id).type == RegAlloc::MEM;
    }
    REQUIRE(saw_spill);
    REQUIRE(general.ValueType(pinned) == RegAlloc::GPR);
    REQUIRE(general.ValueGPR(pinned).id == 0);
    REQUIRE(general.ValueType(snapshot) != RegAlloc::REF);
    REQUIRE(general.ValueType(fixed_fpr) == RegAlloc::FPR);
    REQUIRE(general.ValueFPR(fixed_fpr).id == 2);
    REQUIRE(general.ValueType(fpr_snapshot) != RegAlloc::REF);
    if (general.ValueType(fpr_snapshot) == RegAlloc::FPR) {
        REQUIRE(general.ValueFPR(fpr_snapshot).id != 7);
    }

    const char* env = std::getenv("SVM_RA_1BLK");
    const auto& expected = env && std::strcmp(env, "0") == 0 ? general : fast;
    for (std::uint32_t id = 0; id < selected.MapCount(); ++id) {
        INFO("production selector map id " << id);
        REQUIRE(selected.Mapping(id) == expected.Mapping(id));
    }
}

// Builds a block that keeps `live` scalar values simultaneously live across a
// VecFAdd -- the emitter with the largest scratch appetite in the backend
// (JitTranslator::EmitVecFloatNaNFixup holds eight GPRs at once). Consuming
// the scalars only after the VecFAdd is what makes them live *across* it, so
// the linear scan is asked to fill the register file at exactly the
// instruction that needs the most scratch.
static swift::runtime::ir::Block* BuildScratchPressureBlock(unsigned live) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x1000});
    std::vector<Value> scalars;
    scalars.reserve(live);
    for (unsigned i = 0; i < live; i++) {
        scalars.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    // Vector values come from uniforms, the shape the x86 frontend produces
    // for XMM registers (a V128 LoadImm is not a form the backend emits).
    auto lhs = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{16, ValueType::V128});
    auto rhs = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{32, ValueType::V128});
    auto sum = block->VecFAdd<TypedValue<ValueType::V128>>(lhs, rhs, Imm{32u});
    block->StoreUniform(Uniform{48, ValueType::V128}, sum);
    // Consume the scalars *after* the float op, in reverse definition order.
    Value acc = scalars.back();
    for (int i = static_cast<int>(live) - 2; i >= 0; i--) {
        acc = block->Add(acc, Operand{scalars[i]});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, acc);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Companion to the block above, aimed at the *reload* half of the budget:
// every value is read twice, far apart, so the linear scan spills most of them
// and the second read of each becomes a reload into a scratch register on top
// of the reading opcode's own budget. The final Add names one spilled value
// twice, which costs one register only because JitContext memoizes reloads per
// (instruction, value).
static swift::runtime::ir::Block* BuildReloadPressureBlock(unsigned live) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x2000});
    std::vector<Value> values;
    values.reserve(live);
    for (unsigned i = 0; i < live; i++) {
        values.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    // First read in reverse order: nothing dies until the very end, so the
    // register file is saturated for the whole block.
    Value first = values.back();
    for (int i = static_cast<int>(live) - 2; i >= 0; i--) {
        first = block->Add(first, Operand{values[i]});
    }
    // Second read of every value, by which point most of them are spilled.
    Value second = values.front();
    for (unsigned i = 1; i < live; i++) {
        second = block->Add(second, Operand{values[i]});
    }
    auto both = block->Add(first, Operand{second});
    // One instruction naming the same value twice, and deliberately the value
    // defined first: it is live across the whole block, so the scan spills it,
    // and both reads become reloads of the same slot. JitContext must serve
    // them from one scratch register -- that memoisation is what the reload
    // accounting above is entitled to assume.
    // And, not Add: its emitter is one of the few that actually spends its
    // whole three-register budget, leaving no slack to absorb a second reload.
    auto doubled = block->And(values.front(), Operand{values.front()});
    auto total = block->Add(both, Operand{doubled});
    block->StoreUniform(Uniform{0, ValueType::U32}, total);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

TEST_CASE("Scratch pool survives a register file saturated across a VecFAdd") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    // The regression this guards: JitContext::GetTmpX hands out registers the
    // allocation pass left free, and never releases them before the next
    // instruction. EmitVecFAdd needs eight at once. With a fixed reserve
    // (4 before this change) the scan was free to fill the register file down
    // to four, so a VecFAdd in a saturated block either panicked with
    // "No free temporary GPR" or -- worse, when the pool happened to have room
    // -- silently handed the NaN fixup a register holding a live value.
    //
    // Saturating the file is the whole point, so `live` is deliberately larger
    // than any ARM64 GPR pool.
    swift::runtime::Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    const auto gprs = address_space.GetTrampolines().GetGPRRegs();
    const auto fprs = address_space.GetTrampolines().GetFPRRegs();

    auto check_and_emit = [&](Block* raw) {
        swift::runtime::IntrusivePtr<Block> block{raw};
        RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs};
        RegisterAllocPass::Run(block.get(), &reg_alloc);

        // 1. The allocation must leave every instruction the scratch its
        //    emitter is declared to need, plus a reload register for each
        //    DISTINCT spilled value it names (JitContext reloads a value once
        //    per instruction however often the instruction names it). This is
        //    the contract GetTmpX relies on.
        for (auto& inst : block->GetInstList()) {
            auto need = ScratchBudget(inst.GetOp());
            unsigned reloads_gpr = 0, reloads_fpr = 0;
            std::vector<std::uint32_t> counted;
            auto count = [&](const Value& value) {
                if (!value.Defined() || reg_alloc.ValueType(value) != RegAlloc::MEM) {
                    return;
                }
                if (std::find(counted.begin(), counted.end(), value.Id()) != counted.end()) {
                    return;
                }
                counted.push_back(value.Id());
                auto type = value.Type();
                (type >= ValueType::V8 && type <= ValueType::V256 ? reloads_fpr : reloads_gpr)++;
            };
            for (auto& value : inst.GetValues()) {
                count(value);
            }
            if (inst.HasValue()) {
                count(Value{&inst});
            }
            INFO("opcode " << static_cast<unsigned>(inst.GetOp()) << " at id " << inst.Id());
            REQUIRE(static_cast<unsigned>(reg_alloc.DirtyGPR(inst.Id()).GetClearCount()) >=
                    need.gpr + reloads_gpr);
            REQUIRE(static_cast<unsigned>(reg_alloc.DirtyFPR(inst.Id()).GetClearCount()) >=
                    need.fpr + reloads_fpr);
        }

        // 2. And the JIT must actually emit it. GetTmpX panics through
        //    AssertFailed, so reaching the end of Translate is the assertion:
        //    a scratch shortfall aborts here rather than returning.
        arm64::JitContext context{module, reg_alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
    };

    for (unsigned live : {8u, 16u, 24u, 40u}) {
        INFO("live scalar values across the VecFAdd: " << live);
        check_and_emit(BuildScratchPressureBlock(live));
    }
    // Reload pressure: values read twice across a saturated file, and one
    // instruction naming the same spilled value twice. Capped below the point
    // where the 64-slot State::spill_area runs out -- an unrelated limit
    // (backend::kMaxSpillSlots) with its own loud assert.
    for (unsigned live : {12u, 20u, 28u}) {
        INFO("values read twice across a saturated file: " << live);
        check_and_emit(BuildReloadPressureBlock(live));
    }
}

// --- spill-slot recycling ----------------------------------------------------
//
// The blocks below pin the register file full with long-lived "anchor" values
// and then push a long stream of SHORT-lived values past it. Every short value
// spills (the file is full) but at most two are live at any instant, so the
// number of stack slots the unit needs is a small constant while the number of
// spill *events* is proportional to the churn. That difference is the whole
// point: a linear scan that never returns a slot consumes one per event and
// dies on backend::kMaxSpillSlots; one that recycles stays flat.

static swift::runtime::ir::Block* BuildSpillChurnBlock(unsigned anchors, unsigned churn) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x3000});
    std::vector<Value> held;
    held.reserve(anchors);
    for (unsigned i = 0; i < anchors; i++) {
        held.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    // The churn. `acc` is a chain: acc(i) dies at exactly the instruction that
    // defines acc(i+1), which is the boundary case an off-by-one in the expiry
    // test (`end < start` vs `end <= start`) gets wrong -- both are live at
    // that instruction, so they must not share a slot.
    Value acc = block->LoadImm(Imm{0u});
    for (unsigned i = 0; i < churn; i++) {
        auto tmp = block->LoadImm(Imm{static_cast<std::uint32_t>(0x1000 + i)});
        acc = block->Add(acc, Operand{tmp});
    }
    // Anchors are consumed only here, so they are live across the whole churn.
    Value sum = held.back();
    for (int i = static_cast<int>(anchors) - 2; i >= 0; i--) {
        sum = block->Add(sum, Operand{held[i]});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, block->Add(sum, Operand{acc}));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Same shape in the vector file. A spilled V128 occupies TWO consecutive u64
// slots and is accessed with a 16-byte Ldr/Str, so recycling it has to return
// both halves and keep the surviving pairs even-aligned (the scaled offset form
// cannot encode an odd one).
static swift::runtime::ir::Block* BuildVecSpillChurnBlock(unsigned anchors, unsigned churn) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x4000});
    std::vector<Value> held;
    held.reserve(anchors);
    for (unsigned i = 0; i < anchors; i++) {
        held.push_back(block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i + 1)), ValueType::V128}));
    }
    Value acc = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{0, ValueType::V128});
    for (unsigned i = 0; i < churn; i++) {
        auto tmp = block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i % 8)), ValueType::V128});
        acc = block->VecAdd<TypedValue<ValueType::V128>>(acc, tmp, Imm{32u});
    }
    Value sum = held.back();
    for (int i = static_cast<int>(anchors) - 2; i >= 0; i--) {
        sum = block->VecAdd<TypedValue<ValueType::V128>>(sum, held[i], Imm{32u});
    }
    block->StoreUniform(Uniform{0, ValueType::V128},
                        block->VecAdd<TypedValue<ValueType::V128>>(sum, acc, Imm{32u}));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Both files churning at once, so scalar and SIMD spills interleave over the
// same recycled stack: this is the case where returning only one half of a
// SIMD pair, or losing the even alignment, shows up.
static swift::runtime::ir::Block* BuildMixedSpillChurnBlock(unsigned anchors, unsigned churn) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x5000});
    std::vector<Value> gpr_held;
    std::vector<Value> vec_held;
    for (unsigned i = 0; i < anchors; i++) {
        gpr_held.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
        vec_held.push_back(block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i + 1)), ValueType::V128}));
    }
    Value gpr_acc = block->LoadImm(Imm{0u});
    Value vec_acc = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{0, ValueType::V128});
    for (unsigned i = 0; i < churn; i++) {
        auto scalar = block->LoadImm(Imm{static_cast<std::uint32_t>(0x2000 + i)});
        gpr_acc = block->Add(gpr_acc, Operand{scalar});
        auto vec = block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i % 8)), ValueType::V128});
        vec_acc = block->VecAdd<TypedValue<ValueType::V128>>(vec_acc, vec, Imm{32u});
    }
    Value gpr_sum = gpr_held.back();
    Value vec_sum = vec_held.back();
    for (int i = static_cast<int>(anchors) - 2; i >= 0; i--) {
        gpr_sum = block->Add(gpr_sum, Operand{gpr_held[i]});
        vec_sum = block->VecAdd<TypedValue<ValueType::V128>>(vec_sum, vec_held[i], Imm{32u});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, block->Add(gpr_sum, Operand{gpr_acc}));
    block->StoreUniform(Uniform{16, ValueType::V128},
                        block->VecAdd<TypedValue<ValueType::V128>>(vec_sum, vec_acc, Imm{32u}));
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

// Manufactures the one stack layout that tells an even-aligned pair search from
// a naive one: a scalar sitting on the low half of a freed SIMD pair, with the
// high half and the pair above it free. The free slots are then {odd, even,
// even+1} and the first *adjacent free pair* starts on an ODD index -- which a
// 16-byte Ldr/Str cannot address with the scaled offset form. Only a search
// that steps by two rejects it.
static swift::runtime::ir::Block* BuildPairFragmentBlock(unsigned gpr_anchors,
                                                         unsigned vec_anchors,
                                                         unsigned rounds) {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x6000});
    std::vector<Value> gpr_held;
    std::vector<Value> vec_held;
    for (unsigned i = 0; i < gpr_anchors; i++) {
        gpr_held.push_back(block->LoadImm(Imm{static_cast<std::uint32_t>(i + 1)}));
    }
    for (unsigned i = 0; i < vec_anchors; i++) {
        vec_held.push_back(block->LoadUniform<TypedValue<ValueType::V128>>(
                Uniform{static_cast<std::uint32_t>(16 * (i + 1)), ValueType::V128}));
    }
    for (unsigned r = 0; r < rounds; r++) {
        auto va = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{16, ValueType::V128});
        auto vb = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{32, ValueType::V128});
        block->StoreUniform(Uniform{64, ValueType::V128}, va);  // va dies
        auto sx = block->LoadImm(Imm{static_cast<std::uint32_t>(0x30 + r)});
        block->StoreUniform(Uniform{80, ValueType::V128}, vb);  // vb dies; sx now holds va's low slot
        auto vc = block->LoadUniform<TypedValue<ValueType::V128>>(Uniform{48, ValueType::V128});
        block->StoreUniform(Uniform{96, ValueType::V128}, vc);
        block->StoreUniform(Uniform{0, ValueType::U32}, sx);
    }
    Value gpr_sum = gpr_held.back();
    for (int i = static_cast<int>(gpr_anchors) - 2; i >= 0; i--) {
        gpr_sum = block->Add(gpr_sum, Operand{gpr_held[i]});
    }
    Value vec_sum = vec_held.back();
    for (int i = static_cast<int>(vec_anchors) - 2; i >= 0; i--) {
        vec_sum = block->VecAdd<TypedValue<ValueType::V128>>(vec_sum, vec_held[i], Imm{32u});
    }
    block->StoreUniform(Uniform{0, ValueType::U32}, gpr_sum);
    block->StoreUniform(Uniform{112, ValueType::V128}, vec_sum);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

TEST_CASE("Spill slots are recycled, not merely handed out") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    // The regression this guards: SpillAtInterval never recorded its interval
    // in the allocator's active set, so the MEM arm of ExpireOldIntervals --
    // and FreeSpill with it -- was unreachable. Slots were handed out and never
    // returned, making the 64-slot State::spill_area a budget for the TOTAL
    // number of spills in a compilation unit instead of the number live at
    // once. Any long enough block of short-lived spills aborted the guest with
    // "spill area exhausted".
    swift::runtime::Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    const auto gprs = address_space.GetTrampolines().GetGPRRegs();
    const auto fprs = address_space.GetTrampolines().GetFPRRegs();
    // Filling the file exactly means the anchors take every allocatable
    // register and only the scratch reserve's worth spills permanently, so the
    // slot high-water mark is dominated by the churn -- which is what is under
    // test.
    const auto gpr_pool = static_cast<unsigned>(GPRSMask{gprs}.GetClearCount());
    const auto fpr_pool = static_cast<unsigned>(FPRSMask{fprs}.GetClearCount());

    // Live range of a value as the linear scan models it in block mode: from
    // its defining instruction to the last instruction that names it, extended
    // to the end of the block when the terminal reads it.
    auto live_ranges = [](Block* block) {
        std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> range;
        for (auto& inst : block->GetInstList()) {
            if (inst.HasValue()) {
                range[inst.Id()] = {inst.Id(), inst.Id()};
            }
        }
        for (auto& inst : block->GetInstList()) {
            for (auto& value : inst.GetValues()) {
                if (!value.Defined()) continue;
                auto it = range.find(value.Id());
                if (it != range.end()) {
                    it->second.second = std::max<std::uint32_t>(it->second.second, inst.Id());
                }
            }
        }
        return range;
    };

    auto check_recycling = [&](Block* raw, unsigned expect_min_spills) {
        swift::runtime::IntrusivePtr<Block> block{raw};
        RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs};
        // Pre-fix this call itself threw ("spill area exhausted: slot 64 (+1)
        // >= 64 reserved slots") long before any assertion below could run.
        RegisterAllocPass::Run(block.get(), &reg_alloc);

        auto ranges = live_ranges(block.get());
        struct Occupancy {
            std::uint32_t id;
            std::uint32_t slot;
            std::uint32_t width;
            std::uint32_t start;
            std::uint32_t end;
        };
        std::vector<Occupancy> spilled;
        for (auto& inst : block->GetInstList()) {
            // A bitcast is an alias of its source, not an allocation of its
            // own; counting it would report one slot with two owners.
            if (!inst.HasValue() || inst.IsBitCastOperation()) {
                continue;
            }
            Value value{&inst};
            if (reg_alloc.ValueType(value) != RegAlloc::MEM) {
                continue;
            }
            const auto type = inst.ReturnType();
            const bool is_vector = type >= ValueType::V8 && type <= ValueType::V256;
            const std::uint32_t slot = reg_alloc.ValueMem(value).offset;
            auto& r = ranges[inst.Id()];
            spilled.push_back({inst.Id(), slot, is_vector ? 2u : 1u, r.first, r.second});
        }

        // 1. The unit must actually have gone down the spill path, hard.
        INFO("spilled values: " << spilled.size());
        REQUIRE(spilled.size() >= expect_min_spills);

        std::uint32_t high_water = 0;
        for (auto& s : spilled) {
            high_water = std::max(high_water, s.slot + s.width);
            // 2. A 16-byte Ldr/Str encodes only a multiple-of-16 offset, so a
            //    SIMD pair must stay even-aligned however the stack is reused.
            if (s.width == 2) {
                INFO("SIMD value " << s.id << " at slot " << s.slot);
                REQUIRE(s.slot % 2 == 0);
            }
            // 3. Never past the reservation: beyond it the Str walks into the
            //    uniform buffer that follows State::spill_area.
            REQUIRE(s.slot + s.width <= kMaxSpillSlots);
        }

        // 4. Two values that are live at the same time must not share a slot.
        //    This is what makes recycling safe rather than merely cheap: a slot
        //    returned one instruction too early is handed to an overlapping
        //    value and both Ldr/Str target the same address.
        for (std::size_t i = 0; i < spilled.size(); i++) {
            for (std::size_t j = i + 1; j < spilled.size(); j++) {
                const auto& a = spilled[i];
                const auto& b = spilled[j];
                const bool ranges_overlap = a.start <= b.end && b.start <= a.end;
                if (!ranges_overlap) {
                    continue;
                }
                const bool slots_overlap =
                        a.slot < b.slot + b.width && b.slot < a.slot + a.width;
                INFO("values " << a.id << " [" << a.start << "," << a.end << "] slot " << a.slot
                               << "+" << a.width << " and " << b.id << " [" << b.start << ","
                               << b.end << "] slot " << b.slot << "+" << b.width);
                REQUIRE_FALSE(slots_overlap);
            }
        }

        // 5. And the reuse must be real: far more spills than slots. Without
        //    recycling these are equal by construction.
        INFO("high water " << high_water << " slots for " << spilled.size() << " spills");
        REQUIRE(high_water * 2 < spilled.size());

        // 6. Finally the JIT has to emit it. GetTmpX/GetTmpV and
        //    SpillGPR/SpillFPR assert through AssertFailed, so reaching the end
        //    of Translate is itself the assertion.
        arm64::JitContext context{module, reg_alloc};
        arm64::JitTranslator translator{context};
        translator.Translate(block.get());
        context.Finish();
        REQUIRE(context.CurrentBufferSize() > 0);
        return high_water;
    };

    SECTION("scalar churn") {
        // 200 rounds of (LoadImm, Add) past a full file: >=400 spill events
        // against a 64-slot area. Pre-fix this aborts at the 64th.
        check_recycling(BuildSpillChurnBlock(gpr_pool, 200), 300);
    }
    SECTION("vector churn") {
        check_recycling(BuildVecSpillChurnBlock(fpr_pool, 200), 300);
    }
    SECTION("mixed churn") {
        // Scalar and SIMD spills interleaved over one recycled stack.
        check_recycling(BuildMixedSpillChurnBlock(std::min(gpr_pool, fpr_pool), 200), 500);
    }
    SECTION("fragmented pairs stay 16-byte aligned") {
        // Deliberately leaves the first adjacent free pair on an ODD index.
        check_recycling(BuildPairFragmentBlock(gpr_pool, fpr_pool, 120), 400);
    }
}

// --- x86 mul CF/OF must reach the flags register -----------------------------
//
// JitTranslator::SaveCV/SaveOF used to write their C/V bits into the HOST NZCV
// register (Msr) and then clear nzcv_dirty, which is exactly the state that
// makes MergeNZCV() do nothing: the bits were produced and then dropped. The
// x86 frontend cannot reach either function today -- MulWithFlags materialises
// CF/OF through a separate `SaveFlags(t + t, C|V)` producer rather than hanging
// the pseudo on the Mul, and the only frontend ir::Div is the flagless RCL/RCR
// modulus -- so no guest program and no exit code can catch this. The IR the
// backend accepts is wider than the IR the frontend currently emits, and this
// builds the missing shape directly.
//
// The check is on emitted code rather than on a running guest because
// executing a bare Block needs the trampoline/State machinery a unit test does
// not have. That is still a real check: whether the two C/V bits are OR-ed into
// the flags register (x26) or parked in host NZCV is precisely the difference
// between the broken and the fixed lowering.
static swift::runtime::ir::Block* BuildMulCarryOverflowBlock() {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x7000});
    auto a = block->LoadUniform<TypedValue<ValueType::U32>>(Uniform{0, ValueType::U32});
    auto b = block->LoadUniform<TypedValue<ValueType::U32>>(Uniform{4, ValueType::U32});
    // 32x32 unsigned multiply: EmitMul widens with Umull and asks SaveCV
    // whether the upper half is nonzero, i.e. the x86 `mul` CF=OF rule.
    auto product = block->Mul(a, Operand{b});
    block->SaveFlags(product, Flags::Carry | Flags::Overflow);
    block->StoreUniform(Uniform{8, ValueType::U32}, product);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

TEST_CASE("SaveCV commits x86 CF/OF into the flags register") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    swift::runtime::Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    const auto gprs = address_space.GetTrampolines().GetGPRRegs();
    const auto fprs = address_space.GetTrampolines().GetFPRRegs();

    swift::runtime::IntrusivePtr<Block> block{BuildMulCarryOverflowBlock()};
    RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs};
    RegisterAllocPass::Run(block.get(), &reg_alloc);

    arm64::JitContext context{module, reg_alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);

    auto& masm = context.GetMasm();
    auto* buffer = masm.GetBuffer();
    auto* first = buffer->GetStartAddress<const vixl::aarch64::Instruction*>();
    auto* last = buffer->GetEndAddress<const vixl::aarch64::Instruction*>();

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::string text;
    for (const auto* instr = first; instr < last; instr = instr->GetNextInstruction()) {
        decoder.Decode(instr);
        text += disassembler.GetOutput();
        text += '\n';
    }
    INFO(text);

    // HostFlagsBit::C = 29, HostFlagsBit::V = 28 -> (3u << 28) = 0x30000000,
    // and `flags` is x26 (backend/arm64/defines.h).
    REQUIRE(text.find("orr x26, x26, #0x30000000") != std::string::npos);
    // ...and it must NOT be stashed in host NZCV, where nothing collects it.
    // Nothing else in this block has any reason to write NZCV: the only guest
    // flag producer here is the multiply.
    REQUIRE(text.find("msr nzcv") == std::string::npos);
    REQUIRE(text.find("msr NZCV") == std::string::npos);
}

// --- CondSet is an arithmetic 0/1 value, not merely truthy ------------------
//
// Guest consumers historically fed CondSet only into truthiness operations,
// so replacing CSET (0/1) with CSETM (0/-1) survived the entire guest corpus.
// Build the missing IR shape directly: materialize EQ and add it to an integer
// before storing it. The disassembly check is the direct backend contract,
// following the SaveCV structural test above; a CSETM mutation fails here even
// though branches would continue to behave the same way.
static swift::runtime::ir::Block* BuildCondSetArithmeticBlock() {
    using namespace swift::runtime::ir;
    auto* block = new Block(0, Location{0x7100});
    auto flags_value = block->LoadImm(Imm{0ull}).SetType(ValueType::U64);
    block->SaveFlags(flags_value, Flags::NZ);
    auto one = block->CondSet(Cond::EQ).SetType(ValueType::U64);
    auto base = block->LoadImm(Imm{41ull}).SetType(ValueType::U64);
    auto sum = block->Add(base, Operand{one});
    block->StoreUniform(Uniform{0, ValueType::U64}, sum);
    block->SetTerminal(terminal::ReturnToDispatch{});
    block->ReIdInstr();
    return block;
}

TEST_CASE("CondSet materializes exactly one for arithmetic consumers") {
    using namespace swift::runtime::ir;
    using namespace swift::runtime::backend;

    swift::runtime::Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = swift::runtime::kArm64,
    };
    AddressSpace address_space{config};
    auto module = address_space.GetDefaultModule();
    const auto gprs = address_space.GetTrampolines().GetGPRRegs();
    const auto fprs = address_space.GetTrampolines().GetFPRRegs();

    swift::runtime::IntrusivePtr<Block> block{BuildCondSetArithmeticBlock()};
    RegAlloc reg_alloc{block->MaxInstrId(), gprs, fprs};
    RegisterAllocPass::Run(block.get(), &reg_alloc);

    arm64::JitContext context{module, reg_alloc};
    arm64::JitTranslator translator{context};
    translator.Translate(block.get());
    context.Finish();
    REQUIRE(context.CurrentBufferSize() > 0);

    auto& masm = context.GetMasm();
    auto* buffer = masm.GetBuffer();
    auto* first = buffer->GetStartAddress<const vixl::aarch64::Instruction*>();
    auto* last = buffer->GetEndAddress<const vixl::aarch64::Instruction*>();

    vixl::aarch64::Decoder decoder;
    vixl::aarch64::Disassembler disassembler;
    decoder.AppendVisitor(&disassembler);
    std::string text;
    for (const auto* instr = first; instr < last; instr = instr->GetNextInstruction()) {
        decoder.Decode(instr);
        text += disassembler.GetOutput();
        text += '\n';
    }
    INFO(text);

    REQUIRE(text.find("cset ") != std::string::npos);
    REQUIRE(text.find("csetm ") == std::string::npos);
    REQUIRE(text.find("add ") != std::string::npos);
}
