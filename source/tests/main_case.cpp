#include <catch2/catch_test_macros.hpp>
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/opts/cfg_analysis_pass.h"
#include "runtime/ir/opts/local_elimination_pass.h"
#include "runtime/backend/mem_map.h"
#include "compiler/slang/slang.h"

TEST_CASE("Test compiler") {
    using namespace swift::slang;
    Context context{};
    CompileFile("/Users/swift/CLionProjects/SwiftVM/source/tests/test.slang", context);
}

TEST_CASE("Test runtime-ir") {
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
    LocalEliminationPass::Run(&hir_builder);

    assert(local2.Defined());

    MemMap mem_arena{0x100000, true};

    auto res = mem_arena.Map(0x100000, 0, MemMap::ReadExe, false);
    ASSERT(res);

}

TEST_CASE("Test runtime-ir-cfg") {
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
    LocalEliminationPass::Run(&hir_builder);

    assert(local2.Defined());

    MemMap mem_arena{0x100000, true};

    auto res = mem_arena.Map(0x100000, 0, MemMap::ReadExe, false);
    ASSERT(res);

}