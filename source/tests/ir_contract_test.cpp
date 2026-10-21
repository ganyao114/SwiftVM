#include <catch2/catch_test_macros.hpp>
#include <array>
#include <algorithm>
#include <chrono>
#include <iostream>
#include "runtime/ir/block.h"
#include "runtime/ir/opts/const_folding_pass.h"
#include "runtime/ir/opts/deadcode_elimination_pass.h"
#include "runtime/ir/opts/integer_width_elimination_pass.h"
#include "runtime/ir/consumer_index.h"
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::runtime::ir;

TEST_CASE("IR use counts retain high fanout definitions", "[ir][regression]") {
    if constexpr (sizeof(void*) == 8)
        REQUIRE(((sizeof(Inst) + 15u) & ~size_t{15u}) == 96);
    for (u32 uses : {255u, 256u, 1024u}) {
        INFO("uses=" << uses);
        Block block{0, Location{0x1000}};
        auto value = block.LoadImm(Imm{u64{42}});
        for (u32 i = 0; i < uses; ++i)
            block.StoreUniform(Uniform{0, ValueType::U64}, value);
        block.SetTerminal(terminal::ReturnToHost{});
        REQUIRE(value.Def()->GetUses(false) == uses);
        DeadCodeEliminationPass::Run(&block);
        REQUIRE(value.Def()->GetUses(false) == uses);
        REQUIRE(block.GetInstList().size() == uses + 1);
    }
}

TEST_CASE("IR use counts include more than 255 pseudo edges", "[ir][regression]") {
    Block block{0, Location{0x1000}};
    auto left = block.LoadImm(Imm{u64{1}});
    auto result = block.Add(left, Operand{Imm{u64{1}}});
    for (u32 i = 0; i < 300; ++i) block.GetFlags(result, Flags::Carry);
    REQUIRE(result.Def()->GetUses(false) == 300);
    REQUIRE(result.Def()->GetUses() == 0);
    block.SetTerminal(terminal::ReturnToHost{});
    DeadCodeEliminationPass::Run(&block);
    REQUIRE(block.GetInstList().empty());
}

TEST_CASE("IR constant CSE preserves each operand width view", "[ir][regression]") {
    for (auto view : {ValueType::U8, ValueType::U16, ValueType::U32, ValueType::U64}) {
        INFO("view=" << unsigned(view));
        Block block{0, Location{0x1000}};
        auto canonical = block.LoadImm(Imm{u64{0x1122334455667788}});
        block.StoreUniform(Uniform{0, ValueType::U64}, canonical);
        auto duplicate = block.LoadImm(Imm{u64{0x1122334455667788}});
        block.StoreUniform(Uniform{8, view}, Value{duplicate}.SetCastType(view));
        block.SetTerminal(terminal::ReturnToHost{});
        ConstFoldingPass::Run(&block, FeatureSet{});
        auto& store = block.GetInstList().back();
        REQUIRE(store.GetArg<Value>(1).Def() == canonical.Def());
        REQUIRE(store.GetArg<Value>(1).Type() == view);
        DeadCodeEliminationPass::Run(&block);
        REQUIRE(block.GetInstList().size() == 3);
    }
}

#if defined(__aarch64__)
TEST_CASE("IR CSE narrow stores retain surrounding bytes in ARM64 JIT", "[ir][regression][jit]") {
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = 0x2000;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
    config.global_opts = Optimizations::ConstantFolding | Optimizations::DeadCodeRemove;
    backend::AddressSpace space{config};
    Runtime runtime{&space};
    auto state = runtime.GetUniformBuffer();
    std::fill(state.begin(), state.end(), 0xa5);
    IntrusivePtr<Block> block{new Block(0, Location{0x1000})};
    auto first = block->LoadImm(Imm{u64{0x1122334455667788}});
    block->StoreUniform(Uniform{0, ValueType::U64}, first);
    auto second = block->LoadImm(Imm{u64{0x1122334455667788}});
    block->StoreUniform(Uniform{8, ValueType::U8}, Value{second}.SetCastType(ValueType::U8));
    block->SetTerminal(terminal::ReturnToHost{});
    auto module = space.GetModule(0x1000);
    REQUIRE(module->Push(block.get()));
    REQUIRE(TranslateIR(module, block) != nullptr);
    runtime.SetLocation(0x1000);
    REQUIRE(runtime.Run() == HaltReason::CallHost);
    REQUIRE(state[8] == 0x88);
    for (size_t i = 9; i < state.size(); ++i) REQUIRE(state[i] == 0xa5);
}
#endif

TEST_CASE("IR effects separate liveness from observable behavior", "[ir][effects]") {
    Block block{0, Location{0x1000}};
    auto address = block.LoadImm(Imm{u64{0x1000}});
    auto sum = block.Add(address, Operand{Imm{u64{1}}});
    block.StoreUniform(Uniform{0, ValueType::U64}, sum);
    REQUIRE(address.Def()->GetUses(false) != 0);
    REQUIRE_FALSE(address.Def()->HasSideEffects());
    auto load = block.LoadMemory(Operand{address}).SetType(ValueType::U64);
    REQUIRE(load.Def()->HasSideEffects());
    block.SetTerminal(terminal::ReturnToHost{});
    DeadCodeEliminationPass::Run(&block);
    REQUIRE(load.Def()->GetUses(false) == 0);
    REQUIRE(load.Def()->GetOp() == OpCode::LoadMemory);
    for (u32 op = 1; op < static_cast<u32>(OpCode::BASE_COUNT); ++op)
        REQUIRE(GetIRMetaInfo(static_cast<OpCode>(op)).effects != IREffect::Unknown);
}

TEST_CASE("IR helper effects retain conservative memory contracts", "[ir][effects]") {
    Block block{0, Location{0x1000}};
    auto helper = block.CallLambda(Lambda{DataClass{Imm{u64{1}}},
        HelperCallTraits{.guest_state = HelperGuestStateEffect::None,
                         .fault = HelperFaultEffect::NoDirectFault,
                         .reentry = HelperReentryEffect::NoReentry}});
    auto effects = helper.Def()->GetEffects();
    CHECK((effects & IREffect::ReadGuestState) == IREffect::None);
    CHECK((effects & IREffect::WriteGuestState) == IREffect::None);
    CHECK((effects & IREffect::MayFault) == IREffect::None);
    CHECK((effects & IREffect::WriteMemory) != IREffect::None);
    CHECK(helper.Def()->HasSideEffects());
}

TEST_CASE("IR consumer index retains multiplicity and tracks replacements", "[ir][analysis]") {
    Block block{0, Location{0x1000}};
    auto first = block.LoadImm(Imm{u64{1}});
    auto twice = block.Add(first, Operand{first});
    auto single = block.LoadImm(Imm{u64{2}});
    auto user = block.Add(single, Operand{Imm{u64{1}}});
    SingleConsumerIndex index{&block};
    REQUIRE(index.Find(first.Def()) == nullptr);
    REQUIRE(index.Find(single.Def()) == user.Def());
    user.Def()->SetArg(0, single.WithDefinition(twice.Def()));
    index.Transfer(single.Def(), twice.Def(), user.Def());
    REQUIRE(index.Find(single.Def()) == nullptr);
    REQUIRE(index.Find(twice.Def()) == user.Def());
}

TEST_CASE("IR type validation rejects inconsistent uniform store widths", "[ir][types]") {
    auto* constant = Inst::Create(OpCode::LoadImm, Imm{u64{7}});
    auto* store = Inst::Create(OpCode::StoreUniform,
                             Uniform{0, ValueType::U8}, Value{constant});
    CHECK_FALSE(store->HasValidTypeViews());
    store->SetArg(1, Value{constant}.SetCastType(ValueType::U8));
    CHECK(store->HasValidTypeViews());
    delete store;
    delete constant;
}

TEST_CASE("IR DCE removes a long unused definition chain", "[ir][regression]") {
    Block block{0, Location{0x1000}};
    auto value = block.LoadImm(Imm{u64{1}});
    for (u32 i = 0; i < 8192; ++i)
        value = block.Add(value, Operand{Imm{u64{1}}});
    block.SetTerminal(terminal::ReturnToHost{});
    DeadCodeEliminationPass::Run(&block);
    REQUIRE(block.GetInstList().empty());
}

TEST_CASE("IR DCE reaches predecessor definitions across blocks", "[ir][analysis]") {
    HIRBuilder builder;
    auto* function = builder.AppendFunction(Location{0x1000});
    auto* predecessor = function->GetCurrentBlock()->GetBlock();
    auto root = builder.LoadImm(Imm{u64{1}});
    auto middle = builder.Add(root, Operand{Imm{u64{1}}});
    auto* next = builder.LinkBlock(terminal::LinkBlock{Location{0x2000}});
    function->SetCurBlock(next);
    builder.Add(middle, Operand{Imm{u64{1}}});
    builder.ReturnToHost();
    DeadCodeEliminationPass::Run(function);
    REQUIRE(predecessor->GetInstList().empty());
    REQUIRE(next->GetBlock()->GetInstList().empty());
}

#if defined(__aarch64__)
TEST_CASE("IR ARM64 compilation rejects Phi instead of emitting an undefined result", "[ir][cfg]") {
    Config config{};
    config.loc_start = 0x1000;
    config.loc_end = 0x2000;
    config.enable_jit = true;
    config.backend_isa = kArm64;
    config.uniform_buffer_size = 32;
    config.stack_alignment = 16;
    backend::AddressSpace space{config};
    IntrusivePtr<Block> block{new Block(0, Location{0x1000})};
    auto input = block->LoadImm(Imm{u64{1}});
    Params incoming;
    incoming.Push(input);
    auto phi = block->AddPhi(incoming).SetType(ValueType::U64);
    block->StoreUniform(Uniform{0, ValueType::U64}, phi);
    block->SetTerminal(terminal::ReturnToHost{});
    REQUIRE(TranslateIR(space.GetModule(0x1000), block) == nullptr);
    REQUIRE(space.GetCodeCache(Location{0x1000}) == nullptr);
}
#endif

TEST_CASE("IR DCE chain scaling probe", "[.ir-benchmark]") {
    std::cout << "inst_size=" << sizeof(Inst) << " arena_slot="
              << ((sizeof(Inst) + 15u) & ~size_t{15u}) << '\n';
    for (u32 length : {256u, 2048u, 8192u}) {
        std::array<double, 11> elapsed;
        for (auto& time : elapsed) {
            Block block{0, Location{0x1000}};
            auto value = block.LoadImm(Imm{u64{1}});
            for (u32 i = 0; i < length; ++i) value = block.Add(value, Operand{Imm{u64{1}}});
            block.SetTerminal(terminal::ReturnToHost{});
            const auto start = std::chrono::steady_clock::now();
            DeadCodeEliminationPass::Run(&block);
            time = std::chrono::duration<double, std::micro>(
                std::chrono::steady_clock::now() - start).count();
            REQUIRE(block.GetInstList().empty());
        }
        std::sort(elapsed.begin(), elapsed.end());
        std::cout << "dce_chain length=" << length << " median_us=" << elapsed[5] << '\n';
    }
}
