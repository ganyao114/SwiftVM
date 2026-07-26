#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <sys/mman.h>
#include <vector>
#include "runtime/ir/hir_builder.h"
#include "runtime/ir/ir_meta.h"
#include "runtime/ir/opts/cfg_analysis_pass.h"
#include "runtime/ir/opts/local_elimination_pass.h"
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
    LocalEliminationPass::Run(&hir_builder);
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
    LocalEliminationPass::Run(&hir_builder);
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
    LocalEliminationPass::Run(&hir_builder);
    ReIdInstrPass::Run(&hir_builder);
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

    // Carry is deliberately exempt from removal even when a later write in the
    // same block covers it: C persists across blocks and a following block's
    // Adc/Sbb may read it, which per-block liveness cannot model (see the
    // guard in FlagsEliminationPass::Run). Both writes must therefore survive.
    Block carry_overwritten{0, Location{0x1800}};
    auto c_lhs = carry_overwritten.LoadImm(Imm{1u});
    auto c_rhs = carry_overwritten.LoadImm(Imm{2u});
    auto c_old = carry_overwritten.Add(c_lhs, Operand{c_rhs});
    carry_overwritten.AppendInst(OpCode::SaveFlags, c_old, Flags::All);
    auto c_new = carry_overwritten.Add(c_lhs, Operand{c_rhs});
    carry_overwritten.AppendInst(OpCode::SaveFlags, c_new, Flags::All);

    FlagsEliminationPass::Run(&carry_overwritten);

    REQUIRE(c_old.Def()->GetPseudoOperations(OpCode::SaveFlags).size() == 1);
    REQUIRE(c_new.Def()->GetPseudoOperations(OpCode::SaveFlags).size() == 1);
    size_t carry_save_count = 0;
    for (auto& inst : carry_overwritten.GetInstList()) {
        carry_save_count += inst.GetOp() == OpCode::SaveFlags;
    }
    REQUIRE(carry_save_count == 2);

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
