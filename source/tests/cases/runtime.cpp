#include "../support/case_support.h"
#include "../support/fp_environment.h"

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
    HIRBuilder hir_builder{1, false, FeatureSet{}};
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
    RegAlloc reg_alloc{function->MaxInstrCount(), GPRSMask{0}, FPRSMask{0},
                       FeatureSet{}};
    RegisterAllocPass::Run(&hir_builder, &reg_alloc, FeatureSet{});

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
    HIRBuilder hir_builder{1, false, FeatureSet{}};
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
    RegAlloc reg_alloc{0x100, gprs, fprs, FeatureSet{}};
    RegisterAllocPass::Run(&hir_builder, &reg_alloc, FeatureSet{});

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
    HIRBuilder hir_builder{1, false, FeatureSet{}};
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

    HIRBuilder hir_builder{1, false, FeatureSet{}};
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

TEST_CASE("Runtime preserves an interrupt between Run calls") {
    using namespace swift::runtime;
    using namespace swift::runtime::backend;
    Config config{
            .loc_start = 0,
            .loc_end = 1ull << 48,
            .enable_jit = true,
            .has_local_operation = false,
            .backend_isa = kArm64,
            .global_opts = Optimizations::ReturnStackBuffer,
    };
    AddressSpace address_space{config};
    Runtime runtime{&address_space};

    bool initial_miss = true;
    bool interrupt_seen = true;
    bool resumed_miss = true;
    bool request_published = true;
    bool request_cleared = true;
    bool pending_call_redirected = true;
    bool pending_call_restored = true;
    auto* const empty_rsb = runtime.GetState()->rsb_pointer;
    bool continuation_reset = empty_rsb != nullptr;
    auto* pending_call_table =
            address_space.GetPendingCallCodeCacheTable().Data();
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
        initial_miss &= runtime.Run() == HaltReason::CodeMiss;
        // Deliberately publish after Run has returned and before the next Run:
        // this is the W64 empty-loop window, repeated without multiplying the
        // suite's assertion count.
        runtime.SignalInterrupt();
        request_published &=
                (runtime.GetState()->exit_request & kBackedgeSignalRequest) != 0;
        pending_call_redirected &=
                runtime.GetState()->pending_call_l1_code_cache != pending_call_table;
        interrupt_seen &= runtime.Run() == HaltReason::Signal;
        runtime.GetState()->rsb_pointer = empty_rsb - 1;
        runtime.ClearInterrupt();
        continuation_reset &= runtime.GetState()->rsb_pointer == empty_rsb;
        request_cleared &=
                (runtime.GetState()->exit_request & kBackedgeSignalRequest) == 0;
        pending_call_restored &=
                runtime.GetState()->pending_call_l1_code_cache == pending_call_table;
        resumed_miss &= runtime.Run() == HaltReason::CodeMiss;
    }
    REQUIRE(initial_miss);
    REQUIRE(interrupt_seen);
    REQUIRE(resumed_miss);
    REQUIRE(request_published);
    REQUIRE(request_cleared);
    REQUIRE(pending_call_redirected);
    REQUIRE(pending_call_restored);
    REQUIRE(continuation_reset);
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
