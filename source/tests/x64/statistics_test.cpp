#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/x64/translator.h"
#include "runtime/backend/function_plan.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;
using R = x64::TransferRegion;

namespace swift::runtime::backend::x64 {
// Private observation entry points let the test compare the same native
// instruction sequence with a bare Xbyak generator, without executing it.
struct TransferStatisticsProbe {
    template<class Generator> static void Moves(Generator& g) {
        g.mov(g.rax, g.qword[g.r13]);
        g.movzx(g.rax, g.byte[g.r13]);
        g.movsx(g.rax, g.word[g.r13]);
        g.movsxd(g.rax, g.dword[g.r13]);
        g.mov(g.qword[g.r13 + 8], g.rax);
        g.movd(g.xmm0, g.dword[g.r13]); g.movd(g.dword[g.r13], g.xmm0);
        g.movq(g.xmm0, g.qword[g.r13]); g.movq(g.qword[g.r13], g.xmm0);
        g.movdqu(g.xmm0, g.ptr[g.r13]); g.movdqu(g.ptr[g.r13], g.xmm0);
        g.movdqa(g.xmm0, g.ptr[g.r13]); g.movdqa(g.ptr[g.r13], g.xmm0);
        g.movss(g.xmm0, g.dword[g.r13]); g.movss(g.dword[g.r13], g.xmm0);
        g.movsd(g.xmm0, g.qword[g.r13]); g.movsd(g.qword[g.r13], g.xmm0);
        g.pop(g.qword[g.r13 + 16]);
        g.call(g.r10); g.call(g.r11);
    }
    static R Region(const Translator& t, int base, s64 displacement, u32 bytes, bool indexed = false) {
        const Xbyak::Reg64 reg(base);
        const auto expr = indexed ? reg + t.rax * 2 + displacement : reg + displacement;
        return t.ClassifyTransfer(t.ptr[expr], bytes);
    }
    static u32 Home(const Translator& t, ir::Value value) { return t.Home(value.Def()); }
    static u32 LocalBegin(const Translator& t) { return t.local_begin; }
    static u32 PhiBegin(const Translator& t) { return t.phi_scratch; }
    static void Emitting(Translator& t, ir::Inst* inst) { t.emitting_inst = inst; }
};
}

namespace {
IntrusivePtr<ir::Block> NewBlock() { return new ir::Block(ir::Location{0x1000}); }
Config TestConfig() { return {.uniform_buffer_size = 256}; }
u64 Identity(u64 value) { return value; }
}

TEST_CASE("x64 transfer observations preserve Xbyak bytes and exact access widths", "[x64][statistics]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    (void)as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    x64::Translator observed{block.get(), {}};
    Xbyak::CodeGenerator bare;
    x64::TransferStatisticsProbe::Moves(observed);
    x64::TransferStatisticsProbe::Moves(bare);
    REQUIRE(observed.getSize() == bare.getSize());
    REQUIRE(std::memcmp(observed.getCode(), bare.getCode(), bare.getSize()) == 0);
    const auto& stats = observed.Statistics();
    const auto& homes = stats.At(R::CanonicalSSA);
    REQUIRE(homes.scalar_load_sites == 4); REQUIRE(homes.load_bytes == 71);
    REQUIRE(homes.scalar_store_sites == 1); REQUIRE(homes.store_bytes == 64);
    REQUIRE(homes.vector_load_sites == 6); REQUIRE(homes.vector_store_sites == 6);
    REQUIRE(stats.At(R::ProducerMetadata).scalar_store_sites == 1);
    REQUIRE(stats.At(R::ProducerMetadata).store_bytes == 8);
    REQUIRE(stats.flags_snapshot_sites == 1); REQUIRE(stats.helper_call_sites == 2);
    REQUIRE(stats.unresolved_transfer_width_sites == 0);
    REQUIRE_FALSE(x64::EmitStatistics::complete_memory_census);
}

TEST_CASE("x64 transfer regions require complete bounds and fixed base addresses", "[x64][statistics]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    ir::Params inputs; inputs.Push(ir::Imm{3});
    auto phi = as.AddPhi(inputs).SetType(T::U64);
    as.DefineLocal(ir::Local{1, T::U64});
    block->SetTerminal(ir::terminal::ReturnToHost{});
    auto config = TestConfig(); x64::Translator translator{block.get(), {}, &config};
    using P = x64::TransferStatisticsProbe;
    const auto home = P::Home(translator, phi);
    REQUIRE(P::Region(translator, 13, home, 16) == R::CanonicalSSA);
    REQUIRE(P::Region(translator, 13, home + 15, 1) == R::CanonicalSSA);
    REQUIRE(P::Region(translator, 13, home + 15, 2) == R::FrameOther);
    REQUIRE(P::Region(translator, 13, home + 16, 8) == R::ProducerMetadata);
    REQUIRE(P::Region(translator, 13, home + 23, 2) == R::FrameOther);
    REQUIRE(P::Region(translator, 13, home + 24, 8) == R::AddressScratch);
    REQUIRE(P::Region(translator, 13, home + 31, 2) == R::FrameOther);
    REQUIRE(P::Region(translator, 13, -8, 8) == R::FrameOther);
    REQUIRE(P::Region(translator, 13, home, 8, true) == R::IndexedFrame);
    REQUIRE(P::Region(translator, 14, home, 8) == R::OtherAddress);
    REQUIRE(P::Region(translator, 13, P::LocalBegin(translator), 16) == R::Local);
    REQUIRE(P::Region(translator, 13, P::PhiBegin(translator), 16) == R::PhiScratch);
    REQUIRE(P::Region(translator, 13, P::PhiBegin(translator) + 15, 2) == R::FrameOther);
    REQUIRE(P::Region(translator, 12, backend::state_offset_host_flags, 8) == R::StateFlags);
    REQUIRE(P::Region(translator, 12, backend::state_offset_host_flags + 7, 2) == R::StateOther);
    REQUIRE(P::Region(translator, 12, -8, 8) == R::StateOther);
    const auto uniform = backend::state_offset_uniform_buffer;
    REQUIRE(P::Region(translator, 12, uniform, 8) == R::Uniform);
    REQUIRE(P::Region(translator, 12, uniform - 1, 2) == R::StateOther);
    REQUIRE(P::Region(translator, 12, uniform + 248, 8) == R::Uniform);
    REQUIRE(P::Region(translator, 12, uniform + 249, 8) == R::StateOther);
    REQUIRE(P::Region(translator, 12, uniform + 256, 1) == R::StateOther);
    REQUIRE(P::Region(translator, 12, uniform, 8, true) == R::OtherAddress);
    translator.Translate(); REQUIRE(translator.Statistics().uniform_extent_known);
    x64::Translator unbounded{block.get(), {}}; unbounded.Translate();
    REQUIRE_FALSE(unbounded.Statistics().uniform_extent_known);
    REQUIRE(P::Region(unbounded, 12, uniform + 256, 8) == R::Uniform);
}

TEST_CASE("x64 scalar and flag statistics count emitted sites rather than executions", "[x64][statistics]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{0, type}).SetType(type);
        auto result = as.Add(input, ir::Operand{ir::Imm{1}}).SetType(type);
        as.SaveFlags(result, ir::Flags::All);
        as.StoreUniform(ir::Uniform{16, type}, result);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}}; translator.Translate();
        const auto& stats = translator.Statistics();
        REQUIRE(stats.At(R::Uniform).scalar_load_sites == 1);
        REQUIRE(stats.At(R::Uniform).scalar_store_sites == 1);
        REQUIRE(stats.At(R::Uniform).load_bytes == ir::GetValueSizeByte(type));
        REQUIRE(stats.At(R::Uniform).store_bytes == ir::GetValueSizeByte(type));
        REQUIRE(stats.At(R::CanonicalSSA).scalar_load_sites == 3);
        REQUIRE(stats.At(R::CanonicalSSA).scalar_store_sites == 4);
        REQUIRE(stats.At(R::ProducerMetadata).scalar_load_sites == 1);
        REQUIRE(stats.At(R::ProducerMetadata).scalar_store_sites == 1);
        REQUIRE(stats.At(R::StateFlags).scalar_load_sites == 1);
        REQUIRE(stats.At(R::StateFlags).scalar_store_sites == 1);
        REQUIRE(stats.flags_snapshot_sites == 1); REQUIRE(stats.flags_materialization_ir_sites == 1);
        REQUIRE(stats.helper_call_sites == 1); // unconditional recovery epilogue EndMemory
        REQUIRE(stats.emitted_code_bytes == translator.getSize());
        REQUIRE(stats.frame_bytes == translator.FrameSize());
        REQUIRE(stats.unresolved_transfer_width_sites == 0);
    }
}

TEST_CASE("x64 vector forwarding statistics distinguish home and uniform transfers", "[x64][statistics]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto a = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
    auto b = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
    auto result = as.VecAdd(a, b, ir::Imm{32}).SetType(T::V128);
    as.StoreUniform(ir::Uniform{32, T::V128}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    x64::Translator translator{block.get(), {}}; translator.Translate();
    const auto& stats = translator.Statistics();
    REQUIRE(stats.At(R::Uniform).vector_load_sites == 2);
    REQUIRE(stats.At(R::Uniform).vector_store_sites == 1);
    REQUIRE(stats.At(R::Uniform).load_bytes == 32); REQUIRE(stats.At(R::Uniform).store_bytes == 16);
    REQUIRE(stats.At(R::CanonicalSSA).vector_load_sites == 2);
    REQUIRE(stats.At(R::CanonicalSSA).vector_store_sites == 2);
    REQUIRE(stats.vector_forward_write_sites == 1); REQUIRE(stats.vector_forward_read_sites == 1);
}

TEST_CASE("x64 phi statistics count each emitted edge copy and scratch access", "[x64][statistics]") {
    for (auto type : {T::U64, T::V128}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        ir::Params entry; entry.Push(ir::Imm{9});
        auto first = as.AddPhi(entry).SetType(type);
        auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        auto branch = as.Goto(ir::BOOL{condition});
        auto other = as.LoadUniform(ir::Uniform{16, type}).SetType(type);
        as.BindLabel(branch);
        ir::Params incoming; incoming.Push(first); incoming.Push(other);
        auto phi = as.AddPhi(incoming).SetType(type);
        as.StoreUniform(ir::Uniform{64, type}, phi);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        x64::Translator translator{block.get(), {}}; translator.Translate();
        const auto& stats = translator.Statistics();
        REQUIRE(stats.phi_immediate_copy_sites == 1); REQUIRE(stats.phi_value_copy_sites == 2);
        REQUIRE(stats.At(R::PhiScratch).vector_load_sites == 3);
        REQUIRE(stats.At(R::PhiScratch).scalar_store_sites == (type == T::U64 ? 6 : 2));
        REQUIRE(stats.At(R::PhiScratch).vector_store_sites == (type == T::U64 ? 0 : 2));
        REQUIRE(stats.At(R::PhiScratch).load_bytes == 48); REQUIRE(stats.At(R::PhiScratch).store_bytes == 48);
    }
}

TEST_CASE("x64 statistics include direct callback calls and MemoryCopy address temporaries", "[x64][statistics]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto result = as.CallHost(&Identity, value).SetType(T::U64);
    as.StoreUniform(ir::Uniform{16, T::U64}, result);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    x64::Translator translator{block.get(), {}}; translator.Translate();
    REQUIRE(translator.Statistics().helper_call_sites == 2);

    auto copy_block = NewBlock(); ir::Assembler copy_as{copy_block.get()};
    auto* copy = copy_as.AppendInst(ir::OpCode::MemoryCopy, ir::Lambda{ir::Imm{1}}, ir::Lambda{ir::Imm{2}}, ir::Imm{8});
    x64::Translator copy_translator{copy_block.get(), {}};
    using P = x64::TransferStatisticsProbe;
    const auto offset = P::Home(copy_translator, ir::Value{copy});
    REQUIRE(P::Region(copy_translator, 13, offset, 8) == R::CanonicalSSA);
    P::Emitting(copy_translator, copy);
    REQUIRE(P::Region(copy_translator, 13, offset, 8) == R::AddressScratch);
    REQUIRE(P::Region(copy_translator, 13, offset + 8, 8) == R::AddressScratch);
    REQUIRE(P::Region(copy_translator, 13, offset + 15, 2) == R::FrameOther);
}

TEST_CASE("x64 phi statistics exclude edges replaced by FunctionPlan exits", "[x64][statistics]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto first = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto branch = as.Goto(ir::BOOL{first});
    auto other = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    as.BindLabel(branch);
    // The exit replaces the branch completely. Only the unreachable lexical
    // predecessor retains an edge to this Phi; it still has one emitted copy.
    branch.Def()->SetReturn(T::VOID);
    ir::Params inputs; inputs.Push(other);
    auto phi = as.AddPhi(inputs).SetType(T::U64);
    as.StoreUniform(ir::Uniform{64, T::U64}, phi);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    backend::FunctionPlan plan;
    plan.stops.insert(branch.Def());
    plan.exits.emplace(branch.Def(), ir::terminal::ReturnToHost{});
    x64::Translator translator{block.get(), {}, nullptr, &plan}; translator.Translate();
    REQUIRE(translator.Statistics().phi_value_copy_sites == 1);
    REQUIRE(translator.Statistics().phi_immediate_copy_sites == 0);
    REQUIRE(translator.Statistics().At(R::PhiScratch).load_bytes == 16);
    REQUIRE(translator.Statistics().At(R::PhiScratch).store_bytes == 16);
}

TEST_CASE("x64 flag request sites count lowered read modify write transfers", "[x64][statistics]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
    as.ClearFlags(ir::Flags::All);
    as.SetCarry(value); as.SetOverflow(value); as.InvertCarry();
    block->SetTerminal(ir::terminal::ReturnToHost{});
    x64::Translator translator{block.get(), {}}; translator.Translate();
    REQUIRE(translator.Statistics().flags_materialization_ir_sites == 4);
    REQUIRE(translator.Statistics().flags_snapshot_sites == 0);
    // Mutations now use the shared flags-access layer. Each lowers to an
    // explicit load/store pair and is visible to the transfer census.
    REQUIRE(translator.Statistics().At(R::StateFlags).scalar_load_sites == 4);
    REQUIRE(translator.Statistics().At(R::StateFlags).scalar_store_sites == 4);
    REQUIRE(translator.Statistics().flags_state_load_sites == 4);
    REQUIRE(translator.Statistics().flags_state_store_sites == 4);
    REQUIRE(translator.Statistics().helper_call_sites == 1);
}
