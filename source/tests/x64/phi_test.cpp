#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <sys/mman.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/function_plan.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/riscv64/memory_protocol.h"
#include "runtime/common/backedge_control.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;

namespace {
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 4096> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    ~Storage() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const { u64 value; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
};
using BlockFn = HaltReason (*)(backend::State*);
struct Compiled {
    x64::Translator translator;
    BlockFn fn;
    explicit Compiled(ir::Block* block) : translator(block, {}) {
        translator.Translate(); fn = translator.getCode<BlockFn>();
    }
};
IntrusivePtr<ir::Block> NewBlock() { return new ir::Block(ir::Location{0x1000}); }
void MoveBefore(ir::Block* block, ir::Inst* moved, ir::Inst* before) {
    block->RemoveInst(moved); block->InsertBefore(moved, before); block->ReIdInstr();
}
u64 Helper(u64 a, u64 b, u64 c) { return a * 3 + b * 5 + c * 7; }
Config TestConfig() {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
            .backend_isa = kX86_64, .uniform_buffer_size = 4096,
            .static_program = true, .global_opts = Optimizations::None,
            .stack_alignment = 16};
}
}

TEST_CASE("x64 phi selects taken and fallthrough edges", "[x64][phi]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64, T::V128})
    for (bool invert : {false, true}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        auto original = as.LoadUniform(ir::Uniform{16, type}).SetType(type);
        auto edge = invert ? as.NotGoto(ir::BOOL{condition}) : as.Goto(ir::BOOL{condition});
        auto updated = as.LoadUniform(ir::Uniform{32, type}).SetType(type);
        as.BindLabel(edge);
        ir::Params inputs; inputs.Push(original); inputs.Push(updated);
        auto selected = as.AddPhi(inputs).SetType(type);
        as.StoreUniform(ir::Uniform{64, type}, selected);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u64 value : {u64{0}, u64{1}}) {
            Storage storage; storage.Put(0, value);
            storage.Put(16, 0x123456789abcdef0); storage.Put(24, 0x1122334455667788);
            storage.Put(32, 0xfedcba9876543210); storage.Put(40, 0x8877665544332211);
            const bool taken = invert ? value == 0 : value != 0;
            const u32 size = ir::GetValueSizeByte(type);
            const u64 mask = size >= 8 ? UINT64_MAX : (u64{1} << (size * 8)) - 1;
            REQUIRE(compiled.fn(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(64) == ((taken ? 0x123456789abcdef0ULL : 0xfedcba9876543210ULL) & mask));
            if (size == 16) REQUIRE(storage.Get(72) == (taken ? 0x1122334455667788ULL : 0x8877665544332211ULL));
        }
    }
}

TEST_CASE("x64 entry phi normalizes immediate widths", "[x64][phi]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64, T::V128}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        ir::Params inputs; inputs.Push(ir::Imm{UINT64_MAX});
        auto phi = as.AddPhi(inputs).SetType(type);
        as.StoreUniform(ir::Uniform{0, type}, phi);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()}; Storage storage;
        REQUIRE(compiled.fn(storage.state) == HaltReason::CallHost);
        const auto size = ir::GetValueSizeByte(type);
        REQUIRE(storage.Get(0) == (size >= 8 ? UINT64_MAX : (u64{1} << (size * 8)) - 1));
        REQUIRE(storage.Get(8) == 0);
    }
}

TEST_CASE("x64 vector phi preserves complete containers across actual and input views", "[x64][phi][container]") {
    constexpr std::array vector_types{T::V8, T::V16, T::V32, T::V64, T::V128};
    constexpr std::array actual_types{T::V8, T::V16, T::V32, T::V64, T::V128, T::U8, T::U16, T::U32, T::U64};
    constexpr std::array scalar_views{T::U8, T::U16, T::U32, T::U64};
    std::array<u8, 16> original_bytes{}, updated_bytes{};
    for (u32 byte = 0; byte < 16; ++byte) {
        original_bytes[byte] = u8(0x91 ^ (byte * 13));
        updated_bytes[byte] = u8(0xe7 ^ (byte * 17));
    }
    for (auto actual : actual_types) for (u32 width = 0; width < vector_types.size(); ++width)
    for (bool scalar_view : {false, true}) for (bool invert : {false, true}) {
        const auto type = vector_types[width];
        if (scalar_view && type == T::V128) continue;
        const auto view = scalar_view ? scalar_views[width] : type;
        INFO("actual=" << unsigned(actual) << " view=" << unsigned(view) << " phi=" << unsigned(type) << " invert=" << invert);
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        auto original = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
        if (actual != T::V128) original = as.BitCast(original).SetType(actual);
        auto edge = invert ? as.NotGoto(ir::BOOL{condition}) : as.Goto(ir::BOOL{condition});
        auto updated = as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128);
        if (actual != T::V128) updated = as.BitCast(updated).SetType(actual);
        as.BindLabel(edge);
        ir::Params inputs; inputs.Push(original.SetCastType(view)); inputs.Push(updated.SetCastType(view));
        auto phi = as.AddPhi(inputs).SetType(type);
        auto widened = as.BitCast(phi).SetType(T::V128);
        as.StoreUniform(ir::Uniform{64, T::V128}, widened);
        as.StoreUniform(ir::Uniform{96, T::V128}, phi.SetCastType(T::V128));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u64 condition_value : {u64{0}, u64{1}}) {
            Storage storage; storage.Put(0, condition_value);
            std::memcpy(storage.state->uniform_buffer_begin + 16, original_bytes.data(), 16);
            std::memcpy(storage.state->uniform_buffer_begin + 32, updated_bytes.data(), 16);
            std::memset(storage.state->uniform_buffer_begin + 64, 0x5a, 16);
            std::memset(storage.state->uniform_buffer_begin + 96, 0xa5, 16);
            const bool taken = invert ? condition_value == 0 : condition_value != 0;
            const auto& expected = taken ? original_bytes : updated_bytes;
            REQUIRE(compiled.fn(storage.state) == HaltReason::CallHost);
            for (u32 byte = 0; byte < 16; ++byte) {
                INFO("byte=" << byte << " taken=" << taken);
                REQUIRE(storage.state->uniform_buffer_begin[64 + byte] == expected[byte]);
                REQUIRE(storage.state->uniform_buffer_begin[96 + byte] == expected[byte]);
            }
        }
    }
}

TEST_CASE("x64 narrow vector phi parallel cycles retain poison bytes across callbacks", "[x64][phi][container]") {
    constexpr u32 lanes = 3;
    std::array<std::array<u8, 16>, lanes> bytes{};
    for (u32 lane = 0; lane < lanes; ++lane) for (u32 byte = 0; byte < 16; ++byte)
        bytes[lane][byte] = u8(0x83 + lane * 41 + byte * 7);
    for (auto type : {T::V8, T::V16, T::V32, T::V64}) for (bool callback : {false, true}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        std::array<ir::Value, lanes> initial, phis;
        for (u32 lane = 0; lane < lanes; ++lane) {
            auto full = as.LoadUniform(ir::Uniform{lane * 16, T::V128}).SetType(T::V128);
            initial[lane] = as.BitCast(full).SetType(type);
        }
        auto count = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
        for (auto& phi : phis) phi = as.AddPhi(ir::Params{}).SetType(type);
        auto n = as.AddPhi(ir::Params{}).SetType(T::U64);
        if (callback) (void)as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, n, n, n).SetType(T::U64);
        auto next = as.Sub(n, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        auto edge = as.Goto(as.TestNotZero(next));
        as.BindLabel(edge); MoveBefore(block.get(), &block->GetInstList().back(), phis.front().Def());
        for (u32 lane = 0; lane < lanes; ++lane) {
            ir::Params inputs; inputs.Push(initial[lane]); inputs.Push(phis[(lane + 1) % lanes]);
            phis[lane].Def()->SetArg(0, inputs);
            auto widened = as.BitCast(phis[lane]).SetType(T::V128);
            as.StoreUniform(ir::Uniform{512 + lane * 16, T::V128}, widened);
        }
        ir::Params inputs; inputs.Push(count); inputs.Push(next); n.Def()->SetArg(0, inputs);
        as.StoreUniform(ir::Uniform{768, T::U64}, next);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u64 iterations : {u64{1}, u64{2}, u64{3}, u64{4}, u64{11}}) {
            INFO("phi=" << unsigned(type) << " callback=" << callback << " iterations=" << iterations);
            Storage storage; storage.Put(256, iterations);
            for (u32 lane = 0; lane < lanes; ++lane)
                std::memcpy(storage.state->uniform_buffer_begin + lane * 16, bytes[lane].data(), 16);
            std::memset(storage.state->uniform_buffer_begin + 512, 0x5a, lanes * 16);
            REQUIRE(compiled.fn(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(768) == 0);
            for (u32 lane = 0; lane < lanes; ++lane) {
                const u32 source = (lane + iterations - 1) % lanes;
                for (u32 byte = 0; byte < 16; ++byte) {
                    INFO("lane=" << lane << " byte=" << byte);
                    REQUIRE(storage.state->uniform_buffer_begin[512 + lane * 16 + byte] == bytes[source][byte]);
                }
            }
        }
    }
}

TEST_CASE("x64 vector phi immediates initialize all re-widened bytes", "[x64][phi][container]") {
    for (auto type : {T::V8, T::V16, T::V32, T::V64, T::V128})
    for (u64 immediate : {UINT64_MAX, u64{0xfedcba9876543210}}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        ir::Params inputs; inputs.Push(ir::Imm{immediate});
        auto phi = as.AddPhi(inputs).SetType(type);
        auto widened = as.BitCast(phi).SetType(T::V128);
        as.StoreUniform(ir::Uniform{64, T::V128}, widened);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()}; Storage storage;
        std::memset(storage.state->uniform_buffer_begin + 64, 0x5a, 16);
        REQUIRE(compiled.fn(storage.state) == HaltReason::CallHost);
        const u32 size = ir::GetValueSizeByte(type);
        for (u32 byte = 0; byte < 16; ++byte) {
            const u8 expected = byte < size && byte < 8 ? u8(immediate >> (byte * 8)) : 0;
            REQUIRE(storage.state->uniform_buffer_begin[64 + byte] == expected);
        }
    }
}

TEST_CASE("x64 scalar phi canonicalizes vector views for signed and unsigned destinations", "[x64][phi][container]") {
    constexpr std::array scalar_types{T::U8, T::U16, T::U32, T::U64, T::S8, T::S16, T::S32, T::S64};
    constexpr std::array vector_views{T::V8, T::V16, T::V32, T::V64};
    std::array<u8, 16> original_bytes{}, updated_bytes{};
    for (u32 byte = 0; byte < 16; ++byte) {
        original_bytes[byte] = u8(0xf1 - byte * 5);
        updated_bytes[byte] = u8(0x83 + byte * 7);
    }
    for (u32 index = 0; index < scalar_types.size(); ++index) for (bool invert : {false, true}) {
        const auto type = scalar_types[index], view = vector_views[index % vector_views.size()];
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        auto original = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
        auto edge = invert ? as.NotGoto(ir::BOOL{condition}) : as.Goto(ir::BOOL{condition});
        auto updated = as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128);
        as.BindLabel(edge);
        ir::Params inputs; inputs.Push(original.SetCastType(view)); inputs.Push(updated.SetCastType(view));
        auto phi = as.AddPhi(inputs).SetType(type);
        auto widened = as.BitCast(phi).SetType(T::V128);
        as.StoreUniform(ir::Uniform{64, T::V128}, widened);
        as.StoreUniform(ir::Uniform{96, T::V128}, phi.SetCastType(T::V128));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u64 condition_value : {u64{0}, u64{1}}) {
            Storage storage; storage.Put(0, condition_value);
            std::memcpy(storage.state->uniform_buffer_begin + 16, original_bytes.data(), 16);
            std::memcpy(storage.state->uniform_buffer_begin + 32, updated_bytes.data(), 16);
            const bool taken = invert ? condition_value == 0 : condition_value != 0;
            const auto& selected = taken ? original_bytes : updated_bytes;
            const u32 size = ir::GetValueSizeByte(type);
            REQUIRE(compiled.fn(storage.state) == HaltReason::CallHost);
            for (u32 byte = 0; byte < 16; ++byte) {
                const u8 expected = byte < size ? selected[byte] : 0;
                REQUIRE(storage.state->uniform_buffer_begin[64 + byte] == expected);
                REQUIRE(storage.state->uniform_buffer_begin[96 + byte] == expected);
            }
        }
    }
}

TEST_CASE("x64 duplicate phi edges retain distinct inputs", "[x64][phi]") {
    for (bool invert : {false, true}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        auto edge = invert ? as.NotGoto(ir::BOOL{condition}) : as.Goto(ir::BOOL{condition});
        as.BindLabel(edge);
        // Both outgoing edges have one predecessor. The contract orders its
        // fallthrough edge before its taken edge, without merging the inputs.
        ir::Params inputs; inputs.Push(ir::Imm{u64{73}}); inputs.Push(ir::Imm{u64{31}});
        auto phi = as.AddPhi(inputs).SetType(T::U64);
        as.StoreUniform(ir::Uniform{16, T::U64}, phi);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u64 value : {u64{0}, u64{1}}) {
            Storage storage; storage.Put(0, value);
            REQUIRE(compiled.fn(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(16) == ((invert ? value == 0 : value != 0) ? 31 : 73));
        }
    }
}

TEST_CASE("x64 explicit flow targets lower critical edge phi copies", "[x64][phi]") {
    auto block = NewBlock(); ir::Assembler as{block.get()};
    auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
    auto initial = as.LoadImm(ir::Imm{u64{17}}).SetType(T::U64);
    auto conditional = as.Goto(ir::BOOL{condition});
    auto updated = as.LoadImm(ir::Imm{u64{31}}).SetType(T::U64);
    auto jump = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
    as.BindLabel(jump); auto* label = &block->GetInstList().back();
    ir::Params inputs; inputs.Push(initial); inputs.Push(updated);
    auto phi = as.AddPhi(inputs).SetType(T::U64);
    as.StoreUniform(ir::Uniform{16, T::U64}, phi);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    backend::FunctionPlan plan;
    plan.body = block; plan.targets[conditional.Def()] = label; plan.targets[jump.Def()] = label;
    plan.stops.insert(jump.Def());
    x64::Translator translator{block.get(), {}, nullptr, &plan}; translator.Translate();
    auto fn = translator.getCode<BlockFn>();
    for (u64 value : {u64{0}, u64{1}}) {
        Storage storage; storage.Put(0, value);
        REQUIRE(fn(storage.state) == HaltReason::CallHost);
        REQUIRE(storage.Get(16) == (value ? 17 : 31));
    }
}

TEST_CASE("x64 loop phi cycles preserve scalar and vector values across callbacks", "[x64][phi]") {
    constexpr u32 lanes = 13;
    for (auto type : {T::U64, T::V128}) for (bool callback : {false, true}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        std::array<ir::Value, lanes> initial, phis;
        for (u32 i = 0; i < lanes; ++i) initial[i] = as.LoadUniform(ir::Uniform{i * 16, type}).SetType(type);
        auto count = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
        for (auto& phi : phis) phi = as.AddPhi(ir::Params{}).SetType(type);
        auto n = as.AddPhi(ir::Params{}).SetType(T::U64);
        if (callback) (void)as.CallLambda(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, n, n, n).SetType(T::U64);
        auto next = as.Sub(n, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        auto edge = as.Goto(as.TestNotZero(next));
        as.BindLabel(edge); MoveBefore(block.get(), &block->GetInstList().back(), phis.front().Def());
        for (u32 i = 0; i < lanes; ++i) {
            ir::Params inputs; inputs.Push(initial[i]); inputs.Push(phis[(i + 1) % lanes]);
            phis[i].Def()->SetArg(0, inputs);
            as.StoreUniform(ir::Uniform{512 + i * 16, type}, phis[i]);
        }
        ir::Params inputs; inputs.Push(count); inputs.Push(next); n.Def()->SetArg(0, inputs);
        as.StoreUniform(ir::Uniform{768, T::U64}, next);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u64 iterations : {u64{1}, u64{2}, u64{14}, u64{29}}) {
            Storage storage; storage.Put(256, iterations);
            for (u32 i = 0; i < lanes; ++i) { storage.Put(i * 16, 1001 + i); storage.Put(i * 16 + 8, 0x9876543210ULL + i); }
            REQUIRE(compiled.fn(storage.state) == HaltReason::CallHost);
            REQUIRE(storage.Get(768) == 0);
            for (u32 i = 0; i < lanes; ++i) {
                const u32 source = (i + iterations - 1) % lanes;
                REQUIRE(storage.Get(512 + i * 16) == 1001 + source);
                if (type == T::V128) REQUIRE(storage.Get(520 + i * 16) == 0x9876543210ULL + source);
            }
        }
    }
}

TEST_CASE("x64 malformed phi graphs fail before emission", "[x64][phi]") {
    for (u32 malformed = 0; malformed < 6; ++malformed) {
        INFO(malformed);
        auto block = NewBlock(); ir::Assembler as{block.get()};
        if (malformed == 0) {
            auto edge = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)}); as.BindLabel(edge);
            ir::Params inputs; inputs.Push(ir::Imm{u64{7}}); as.AddPhi(inputs).SetType(T::U64);
        } else if (malformed == 1) {
            auto later = as.LoadImm(ir::Imm{u64{7}}).SetType(T::U64);
            auto edge = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)}); as.BindLabel(edge);
            ir::Params inputs; inputs.Push(later); inputs.Push(later); auto phi = as.AddPhi(inputs).SetType(T::U64);
            block->RemoveInst(later.Def()); block->InsertAfter(later.Def(), phi.Def()); block->ReIdInstr();
        } else if (malformed == 2) {
            auto value = as.LoadImm(ir::Imm{u32{7}}).SetType(T::U32);
            auto edge = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)}); as.BindLabel(edge);
            ir::Params inputs; inputs.Push(value); inputs.Push(value); as.AddPhi(inputs).SetType(T::U64);
        } else if (malformed == 3) {
            as.LoadImm(ir::Imm{u64{7}}).SetType(T::U64);
            ir::Params inputs; inputs.Push(ir::Imm{u64{11}}); as.AddPhi(inputs).SetType(T::U64);
        } else if (malformed == 4) {
            ir::Params inputs; inputs.Push(ir::Imm{u64{11}}); as.AddPhi(inputs).SetType(T::VOID);
        } else {
            ir::Params inputs; inputs.Push(ir::Imm{u64{11}}); as.AddPhi(inputs).SetType(T::V256);
        }
        block->SetTerminal(ir::terminal::ReturnToHost{});
        REQUIRE_THROWS_AS(Compiled{block.get()}, std::runtime_error);
    }
}

TEST_CASE("x64 whole function phi survives HIR release", "[x64][phi][function]") {
    for (auto opts : {Optimizations::None, Optimizations::ConstantFolding | Optimizations::DeadCodeRemove}) {
    auto config = TestConfig(); config.global_opts = opts;
    backend::AddressSpace space{config}; auto module = space.GetDefaultModule();
    void* entry;
    {
        ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0xa000});
        auto input = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
        auto condition = function->TestZero<ir::U8>(input);
        builder.If(ir::terminal::If{ir::BOOL{condition}, ir::terminal::LinkBlock{ir::Location{0xa100}}, ir::terminal::LinkBlock{ir::Location{0xa200}}});
        builder.SetCurBlock(ir::Location{0xa100}); auto left = function->Add<ir::U64>(input, ir::Operand{ir::Imm{u64{31}}});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xa300}});
        builder.SetCurBlock(ir::Location{0xa200}); auto right = function->Sub<ir::U64>(input, ir::Operand{ir::Imm{u64{7}}});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xa300}});
        builder.SetCurBlock(ir::Location{0xa300}); ir::Params inputs; inputs.Push(left); inputs.Push(right);
        auto phi = function->AddPhi<ir::U64>(inputs);
        auto result = function->CallLambda<ir::U64>(ir::Lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}}, phi, input, phi);
        function->StoreUniform(ir::Uniform{8, T::U64}, result);
        function->EndBlock(ir::terminal::CheckHalt{ir::terminal::ReturnToHost{}}); function->EndFunction();
        entry = x64::CompileFunctions(module, std::span{&function, 1}); REQUIRE(entry != nullptr);
    }
    for (u64 input : {u64{0}, u64{19}, UINT64_MAX}) {
        Storage storage; storage.Put(0, input); const auto value = input == 0 ? 31 : input - 7;
        REQUIRE(reinterpret_cast<BlockFn>(entry)(storage.state) == HaltReason::CallHost);
        REQUIRE(storage.Get(8) == Helper(value, input, value));
        REQUIRE(storage.state->current_loc == ir::Location{0xa300});
    }
    REQUIRE(space.GetCodeCache(ir::Location{0xa300}) == nullptr);
    }
}

TEST_CASE("x64 whole function backedges update loop phi and guest PC", "[x64][phi][function]") {
    backend::AddressSpace space{TestConfig()}; auto module = space.GetDefaultModule(); void* entry;
    {
        ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0xb000});
        auto initial = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
        function->AdvancePC(ir::Imm{u32{4}});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xb100}});
        builder.SetCurBlock(ir::Location{0xb100});
        ir::Params inputs; inputs.Push(initial); inputs.Push(initial);
        auto count = function->AddPhi<ir::U64>(inputs);
        auto next = function->Sub<ir::U64>(count, ir::Operand{ir::Imm{u64{1}}});
        ir::Params loop; loop.Push(initial); loop.Push(next); count.Def()->SetArg(0, loop);
        function->AdvancePC(ir::Imm{u32{4}});
        auto complete = function->TestZero<ir::U8>(next);
        builder.If(ir::terminal::If{ir::BOOL{complete}, ir::terminal::LinkBlock{ir::Location{0xb200}}, ir::terminal::LinkBlock{ir::Location{0xb100}}});
        builder.SetCurBlock(ir::Location{0xb200});
        function->StoreUniform(ir::Uniform{8, T::U64}, count);
        auto pc = function->GetLocation<ir::U64>(); function->StoreUniform(ir::Uniform{16, T::U64}, pc);
        function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        entry = x64::CompileFunctions(module, std::span{&function, 1}); REQUIRE(entry != nullptr);
    }
    for (u64 input : {u64{1}, u64{2}, u64{127}, u64{10000}}) {
        Storage storage; storage.Put(0, input);
        REQUIRE(reinterpret_cast<BlockFn>(entry)(storage.state) == HaltReason::CallHost);
        REQUIRE(storage.Get(8) == 1);
        REQUIRE(storage.Get(16) == 0xb200);
        REQUIRE(storage.state->current_loc == ir::Location{0xb200});
    }
}

TEST_CASE("x64 function switch expands duplicate terminal edges", "[x64][phi][function]") {
    backend::AddressSpace space{TestConfig()}; auto module = space.GetDefaultModule(); void* entry;
    {
        ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0xe000});
        auto input = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
        auto saved = function->Add<ir::U64>(input, ir::Operand{ir::Imm{u64{101}}});
        builder.Switch(ir::terminal::Switch{input, {
                {ir::Imm{u64{0}}, ir::terminal::LinkBlock{ir::Location{0xe100}}},
                {ir::Imm{u64{7}}, ir::terminal::LinkBlock{ir::Location{0xe100}}}}});
        builder.SetCurBlock(ir::Location{0xe100});
        ir::Params inputs; inputs.Push(ir::Imm{u64{73}}); inputs.Push(saved);
        auto phi = function->AddPhi<ir::U64>(inputs);
        function->StoreUniform(ir::Uniform{8, T::U64}, phi);
        function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        entry = x64::CompileFunctions(module, std::span{&function, 1}); REQUIRE(entry != nullptr);
    }
    for (u64 input : {u64{0}, u64{7}, u64{19}}) {
        Storage storage; storage.Put(0, input);
        const auto reason = reinterpret_cast<BlockFn>(entry)(storage.state);
        REQUIRE(reason == (input == 19 ? HaltReason::None : HaltReason::CallHost));
        REQUIRE(storage.Get(8) == (input == 19 ? 0 : input == 0 ? 73 : input + 101));
        if (input != 19) REQUIRE(storage.state->current_loc == ir::Location{0xe100});
    }
}

TEST_CASE("x64 function duplicate conditional edges preserve each phi input", "[x64][phi][function]") {
    backend::AddressSpace space{TestConfig()}; auto module = space.GetDefaultModule(); void* entry;
    {
        ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0xf000});
        auto input = function->LoadUniform<ir::U8>(ir::Uniform{0, T::U8});
        builder.If(ir::terminal::If{ir::BOOL{input}, ir::terminal::LinkBlock{ir::Location{0xf100}}, ir::terminal::LinkBlock{ir::Location{0xf100}}});
        builder.SetCurBlock(ir::Location{0xf100});
        // HIR records the false edge first, then the true edge.
        ir::Params inputs; inputs.Push(ir::Imm{u64{73}}); inputs.Push(ir::Imm{u64{31}});
        auto phi = function->AddPhi<ir::U64>(inputs);
        function->StoreUniform(ir::Uniform{8, T::U64}, phi);
        function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        entry = x64::CompileFunctions(module, std::span{&function, 1}); REQUIRE(entry != nullptr);
    }
    for (u64 input : {u64{0}, u64{1}}) {
        Storage storage; storage.Put(0, input);
        REQUIRE(reinterpret_cast<BlockFn>(entry)(storage.state) == HaltReason::CallHost);
        REQUIRE(storage.Get(8) == (input ? 31 : 73));
    }
}

TEST_CASE("x64 function narrow switches compare full case constants", "[x64][phi][function]") {
    for (bool valid_case : {false, true}) {
    backend::AddressSpace space{TestConfig()}; auto module = space.GetDefaultModule(); void* entry;
    {
        ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0xf200});
        auto input = function->LoadUniform<ir::U8>(ir::Uniform{0, T::U8});
        auto saved = function->LoadImm<ir::U64>(ir::Imm{u64{101}});
        Vector<ir::terminal::Switch::Case> cases{{ir::Imm{u64{300}}, ir::terminal::LinkBlock{ir::Location{0xf300}}}};
        if (valid_case) cases.push_back({ir::Imm{u64{44}}, ir::terminal::LinkBlock{ir::Location{0xf300}}});
        builder.Switch(ir::terminal::Switch{input, cases});
        builder.SetCurBlock(ir::Location{0xf300});
        ir::Params inputs; inputs.Push(saved);
        if (valid_case) inputs.Push(ir::Imm{u64{31}});
        auto phi = function->AddPhi<ir::U64>(inputs);
        function->StoreUniform(ir::Uniform{8, T::U64}, phi);
        function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        entry = x64::CompileFunctions(module, std::span{&function, 1}); REQUIRE(entry != nullptr);
    }
    for (u64 input : {u64{44}, u64{0}, u64{255}}) {
        Storage storage; storage.Put(0, input);
        const bool match = valid_case && input == 44;
        REQUIRE(reinterpret_cast<BlockFn>(entry)(storage.state) == (match ? HaltReason::CallHost : HaltReason::None));
        REQUIRE(storage.Get(8) == (match ? 31 : 0));
    }
    }
}

TEST_CASE("x64 failed function publication preserves HIR ownership and existing entry", "[x64][phi][function]") {
    backend::AddressSpace space{TestConfig()}; auto module = space.GetDefaultModule(); void* entry;
    const auto build = [](ir::HIRBuilder& builder, u64 number) {
        auto* function = builder.AppendFunction(ir::Location{0xa600});
        auto value = function->LoadImm<ir::U64>(ir::Imm{number});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xa700}});
        builder.SetCurBlock(ir::Location{0xa700});
        function->StoreUniform(ir::Uniform{8, T::U64}, value);
        function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        return function;
    };
    {
        ir::HIRBuilder builder{1, true}; auto* function = build(builder, 73);
        entry = x64::CompileFunctions(module, std::span{&function, 1}); REQUIRE(entry != nullptr);
    }
    {
        ir::HIRBuilder builder{1, true}; auto* function = build(builder, 31);
        REQUIRE_THROWS_AS(x64::CompileFunctions(module, std::span{&function, 1}), std::runtime_error);
        // Builder still owns its function after failed publication. Leaving
        // this scope checks that failure did not delete it prematurely.
    }
    REQUIRE(space.GetCodeCache(ir::Location{0xa600}) == entry);
    Storage storage;
    REQUIRE(reinterpret_cast<BlockFn>(entry)(storage.state) == HaltReason::CallHost);
    REQUIRE(storage.Get(8) == 73);
}

TEST_CASE("x64 function flags and early pair results survive cross-block phi", "[x64][phi][function]") {
    backend::AddressSpace space{TestConfig()}; auto module = space.GetDefaultModule(); void* entry;
    {
        ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0xa800});
        auto dividend = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
        auto high = function->LoadImm<ir::U64>(ir::Imm{u64{0}});
        auto divisor = function->LoadImm<ir::U64>(ir::Imm{u64{7}});
        auto quotient = function->Div128<ir::U64>(high, dividend, divisor, ir::Imm{u8{0}});
        auto flag = function->Sub<ir::U64>(dividend, ir::Operand{ir::Imm{u64{0}}});
        function->SaveFlags(flag, ir::Flags::All);
        auto condition = function->CondSet<ir::U8>(ir::Cond::EQ);
        auto* root = function->GetCurrentBlock()->GetBlock();
        builder.If(ir::terminal::If{ir::BOOL{condition}, ir::terminal::LinkBlock{ir::Location{0xa900}}, ir::terminal::LinkBlock{ir::Location{0xaa00}}});
        root->SetTerminal(ir::terminal::Condition{ir::Cond::EQ, ir::terminal::LinkBlock{ir::Location{0xa900}}, ir::terminal::LinkBlock{ir::Location{0xaa00}}});
        builder.SetCurBlock(ir::Location{0xa900});
        auto left = function->Add<ir::U64>(quotient, ir::Operand{ir::Imm{u64{101}}});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xab00}});
        builder.SetCurBlock(ir::Location{0xaa00});
        auto right = function->Add<ir::U64>(quotient, ir::Operand{ir::Imm{u64{203}}});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xab00}});
        builder.SetCurBlock(ir::Location{0xab00});
        ir::Params inputs; inputs.Push(left); inputs.Push(right); auto phi = function->AddPhi<ir::U64>(inputs);
        auto remainder = function->Div128Remainder<ir::U64>(quotient);
        function->StoreUniform(ir::Uniform{8, T::U64}, phi);
        function->StoreUniform(ir::Uniform{16, T::U64}, remainder);
        auto equal = function->CondSet<ir::U8>(ir::Cond::EQ);
        function->StoreUniform(ir::Uniform{24, T::U8}, equal);
        function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        entry = x64::CompileFunctions(module, std::span{&function, 1}); REQUIRE(entry != nullptr);
    }
    for (u64 input : {u64{0}, u64{1}, u64{19}, UINT64_MAX}) {
        Storage storage; storage.Put(0, input);
        REQUIRE(reinterpret_cast<BlockFn>(entry)(storage.state) == HaltReason::CallHost);
        REQUIRE(storage.Get(8) == input / 7 + (input == 0 ? 101 : 203));
        REQUIRE(storage.Get(16) == input % 7);
        REQUIRE(storage.Get(24) == (input == 0));
    }
}

TEST_CASE("x64 function edge publishes target before interrupt polling", "[x64][phi][function]") {
    for (u64 choice : {u64{0}, u64{1}}) for (bool markers : {false, true}) {
        backend::AddressSpace space{TestConfig()}; auto module = space.GetDefaultModule();
        Runtime runtime{&space};
        // This fixture writes the owner's request word through a guest store;
        // allow that host-backed address, rather than the ordinary code-only
        // 64 KiB guest window used by the other function fixtures.
        runtime.GetState()->guest_addr_limit = UINT64_MAX;
        ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0xc000});
        auto value = function->LoadImm<ir::U64>(ir::Imm{u64{73}});
        function->StoreUniform(ir::Uniform{8, T::U64}, value);
        auto pointer = function->LoadImm<ir::U64>(ir::Imm{reinterpret_cast<u64>(&runtime.GetState()->exit_request)});
        auto selection = function->LoadImm<ir::U64>(ir::Imm{choice});
        auto interrupt = +[](u64, u64 choice, u64) -> u64 { return choice; };
        auto condition = function->CallLambda<ir::U64>(ir::Lambda{ir::Imm{reinterpret_cast<u64>(interrupt)}}, pointer, selection, selection);
        // The helper's new post-call check must see no request. Publish the
        // request with a single-owner store afterwards so this still tests
        // target-PC publication at the function edge itself.
        auto request = function->LoadImm<ir::U64>(ir::Imm{kBackedgeSignalRequest});
        function->StoreMemory(ir::Operand{pointer}, request);
        if (markers) function->AdvancePC(ir::Imm{u32{4}});
        builder.If(ir::terminal::If{ir::BOOL{condition}, ir::terminal::LinkBlock{ir::Location{0xc100}}, ir::terminal::LinkBlock{ir::Location{0xc200}}});
        builder.SetCurBlock(ir::Location{0xc100});
        function->StoreUniform(ir::Uniform{16, T::U64}, value); function->EndBlock(ir::terminal::ReturnToHost{});
        builder.SetCurBlock(ir::Location{0xc200});
        function->StoreUniform(ir::Uniform{16, T::U64}, value); function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        REQUIRE(x64::CompileFunctions(module, std::span{&function, 1}) != nullptr);
        runtime.SetLocation(0xc000);
        REQUIRE(runtime.Run() == HaltReason::Signal);
        REQUIRE(runtime.GetLocation() == (choice ? 0xc100 : 0xc200));
        u64 before{}, after{};
        std::memcpy(&before, runtime.GetUniformBuffer().data() + 8, 8);
        std::memcpy(&after, runtime.GetUniformBuffer().data() + 16, 8);
        REQUIRE(before == 73); REQUIRE(after == 0);
    }
}

TEST_CASE("x64 function shared frame recovers precise native memory faults", "[x64][phi][function]") {
    backend::AddressSpace space{TestConfig()}; auto module = space.GetDefaultModule();
    Runtime runtime{&space};
    auto inaccessible = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    REQUIRE(inaccessible != MAP_FAILED);
    {
        ir::HIRBuilder builder{1, true}; auto* function = builder.AppendFunction(ir::Location{0xd000});
        auto address = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
        auto value = function->LoadImm<ir::U64>(ir::Imm{u64{0x72134}});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xd100}});
        builder.SetCurBlock(ir::Location{0xd100});
        function->StoreUniform(ir::Uniform{8, T::U64}, value);
        function->AdvancePC(ir::Imm{u32{4}});
        auto loaded = function->LoadMemory<ir::U64>(ir::Operand{address});
        function->StoreUniform(ir::Uniform{16, T::U64}, loaded);
        function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        REQUIRE(x64::CompileFunctions(module, std::span{&function, 1}) != nullptr);
    }
    const u64 pointer = reinterpret_cast<u64>(inaccessible);
    std::memcpy(runtime.GetUniformBuffer().data(), &pointer, 8); runtime.SetLocation(0xd000);
    const auto original = _mm_getcsr(); const auto changed = (original & ~0x6000u) | 0x6000u;
    _mm_setcsr(changed); const auto reason = runtime.Run(); const auto recovered = _mm_getcsr(); _mm_setcsr(original);
    // Release the fixture before assertions, including when recovery is wrong.
    REQUIRE(munmap(inaccessible, 4096) == 0);
    REQUIRE(reason == HaltReason::PageFatal);
    REQUIRE(runtime.GetLocation() == 0xd104);
    REQUIRE(recovered == changed);
    u64 before{}, after{};
    std::memcpy(&before, runtime.GetUniformBuffer().data() + 8, 8);
    std::memcpy(&after, runtime.GetUniformBuffer().data() + 16, 8);
    REQUIRE(before == 0x72134); REQUIRE(after == 0);
    REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
    REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
    REQUIRE(runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == 0);
    REQUIRE(backend::riscv64::anonymous_memory_readers.load() == 0);
}

namespace {
constexpr u64 exit_phi_flags = 0xfedcba9876543219ULL;
struct ExitPhiProgram {
    backend::FunctionPlan function;
    ir::Assembler as;
    ir::Value source, replaced, phi;
    ExitPhiProgram(T type, bool reachable, bool invert)
        : as((function.body = NewBlock()).get()) {
        const auto storage_type = type <= T::S64 ? T::U64 : T::V128;
        source = as.LoadUniform(ir::Uniform{16, storage_type}).SetType(type);
        as.SetCarry(as.LoadImm(ir::Imm{u8{0}}).SetType(T::U8));
        ir::Value choice, live;
        if (reachable) {
            choice = as.Goto(ir::BOOL{as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8)});
            live = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
            function.stops.insert(live.Def());
            function.targets[choice.Def()] = Label();
        }
        const auto condition = ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)};
        replaced = invert ? as.NotGoto(condition) : as.Goto(condition);
        replaced.Def()->SetReturn(T::VOID);
        function.stops.insert(replaced.Def());
        function.exits.emplace(replaced.Def(), ir::terminal::ReturnToHost{});
        auto* join = Label();
        function.targets[replaced.Def()] = join;
        if (reachable) function.targets[live.Def()] = join;
        ir::Params inputs;
        if (reachable) inputs.Push(source);
        phi = as.AddPhi(inputs).SetType(type);
        as.StoreUniform(ir::Uniform{128, storage_type}, phi.SetCastType(storage_type));
        function.body->SetTerminal(ir::terminal::ReturnToHost{});
    }
    ir::Inst* Label() {
        auto* label = new ir::Inst(ir::OpCode::BindLabel);
        label->SetArgs(ir::Value{label}); function.body->AppendInst(label); return label;
    }
};
void InitializeExitPhiStorage(Storage& storage, u64 choice) {
    for (u32 i = 0; i < 4096; ++i) storage.state->uniform_buffer_begin[i] = u8(i * 37 + 13);
    storage.state->host_cpu_flags = exit_phi_flags;
    storage.Put(0, choice);
}
void ExpectExitPhiState(Storage& expected, bool writes, u32 bytes) {
    if (writes) std::memcpy(expected.state->uniform_buffer_begin + 128,
                            expected.state->uniform_buffer_begin + 16, bytes);
    expected.state->host_cpu_flags = exit_phi_flags & ~(u64{1} << 29);
    expected.state->current_loc = ir::Location{0x1000};
    expected.state->prev_loc = ir::Location{0x1000};
    expected.state->halt_reason = HaltReason::CallHost;
}
void RunExitPhi(x64::Translator& code, Storage& actual) {
    const auto mxcsr = _mm_getcsr();
    REQUIRE(code.getCode<BlockFn>()(actual.state) == HaltReason::CallHost);
    REQUIRE(_mm_getcsr() == mxcsr);
}
} // namespace

TEST_CASE("x64 terminal replaced branches contribute no scalar or vector Phi edge", "[x64][phi][exit][native]") {
    const auto config = TestConfig();
    for (auto type : {T::U64, T::V8, T::V128}) for (bool reachable : {false, true})
    for (bool invert : {false, true}) for (bool cfg : {false, true})
    for (bool flags : {false, true}) for (bool vector : {false, true}) {
        CAPTURE(type, reachable, invert, cfg, flags, vector);
        ExitPhiProgram program{type, reachable, invert};
        const auto graph = x64::CfgRegisterPlan::ValidateGraph(program.function.body.get(), program.function);
        REQUIRE(graph.Enabled());
        u32 phi_copies{};
        for (const auto& edge : graph.Edges()) {
            REQUIRE(edge.source != program.replaced.Def());
            for (const auto& copy : edge.copies) phi_copies += copy.destination == program.phi.Def();
        }
        REQUIRE(phi_copies == u32(reachable));
        auto code = x64::Translator::PrepareFunction(program.function, {}, &config, true, cfg, flags,
                                                    {.vector_cfg_allocation = vector});
        code->Translate();
        REQUIRE(code->FlagsResident() == flags);
        if (vector) REQUIRE(code->VectorPlan().Validate());
        for (u64 choice : {u64{0}, u64{1}}) {
            Storage actual, expected;
            InitializeExitPhiStorage(actual, choice); InitializeExitPhiStorage(expected, choice);
            ExpectExitPhiState(expected, reachable && !choice, type == T::U64 ? 8 : 16);
            RunExitPhi(*code, actual); REQUIRE(actual.bytes == expected.bytes);
        }
    }
}

TEST_CASE("x64 replaced branch condition is structural rather than an executable SSA use", "[x64][phi][exit][ssa][native]") {
    const auto config = TestConfig();
    for (bool cfg : {false, true}) for (bool flags : {false, true}) for (bool vector : {false, true}) {
        CAPTURE(cfg, flags, vector);
        ExitPhiProgram program{T::U64, false, false};
        // This local scalar definition follows the reachable exit and belongs
        // to an unreachable block. The replaced branch never reads it.
        auto dead = program.as.LoadUniform(ir::Uniform{32, T::U8}).SetType(T::U8);
        program.replaced.Def()->SetArg(0, dead);
        // SetArg infers a value return for Goto; this is still a replaced marker.
        program.replaced.Def()->SetReturn(T::VOID);
        REQUIRE(program.replaced.Def()->ReturnType() == T::VOID);
        REQUIRE(program.replaced.Def()->GetArg<ir::Value>(0).Def() == dead.Def());
        const auto graph = x64::CfgRegisterPlan::ValidateGraph(program.function.body.get(), program.function);
        INFO("rejection=" << unsigned(graph.Rejection()) << " IR=" << program.function.body->ToString());
        REQUIRE(graph.Enabled());
        auto code = x64::Translator::PrepareFunction(program.function, {}, &config, true, cfg, flags,
                                                    {.vector_cfg_allocation = vector});
        code->Translate();
        Storage actual, expected; InitializeExitPhiStorage(actual, 1); InitializeExitPhiStorage(expected, 1);
        ExpectExitPhiState(expected, false, 8); RunExitPhi(*code, actual); REQUIRE(actual.bytes == expected.bytes);
        // A selector in the replacing terminal really executes at the marker
        // and must still dominate it. The same dead value is invalid there.
        program.function.exits.at(program.replaced.Def()) = ir::terminal::If{
            ir::BOOL{dead}, ir::terminal::ReturnToHost{}, ir::terminal::ReturnToHost{}};
        REQUIRE(x64::CfgRegisterPlan::ValidateGraph(program.function.body.get(), program.function).Rejection() ==
                x64::CfgPlanRejection::InvalidSSA);
        REQUIRE_THROWS_AS(x64::Translator::PrepareFunction(program.function, {}, &config, true, cfg, flags,
                            {.vector_cfg_allocation = vector}), std::runtime_error);
    }
}

TEST_CASE("x64 terminal replacement preserves malformed branch metadata rejection", "[x64][phi][exit][invalid]") {
    const auto config = TestConfig();
    for (u32 malformed = 0; malformed < 8; ++malformed)
    for (bool cfg : {false, true}) for (bool flags : {false, true}) for (bool vector : {false, true}) {
        CAPTURE(malformed, cfg, flags, vector);
        ExitPhiProgram program{T::U64, false, false};
        auto foreign = NewBlock(); ir::Assembler foreign_as{foreign.get()};
        if (malformed == 0) program.function.targets.clear();
        if (malformed == 1) program.replaced.Def()->SetArg(0, foreign_as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8));
        if (malformed == 2) program.replaced.Def()->SetArg(0,
            program.as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128));
        if (malformed == 3) program.function.stops.erase(program.replaced.Def());
        if (malformed == 4) program.replaced.Def()->SetReturn(T::U64);
        if (malformed == 5) program.replaced.Def()->SetArg(0, ir::Imm{u8{1}});
        // Keep each operand negative isolated from the separate non-VOID test.
        if (malformed == 1 || malformed == 2 || malformed == 5)
            program.replaced.Def()->SetReturn(T::VOID);
        if (malformed == 6) {
            // No executable incoming edge authorizes this extra Phi input.
            ir::Params phantom; phantom.Push(program.source); program.phi.Def()->SetArg(0, phantom);
        }
        if (malformed == 7) {
            auto* store = program.as.AppendInst(ir::OpCode::StoreUniform, ir::Uniform{256, T::U64}, program.source);
            program.function.stops.insert(store); program.function.exits.emplace(store, ir::terminal::ReturnToHost{});
        }
        const auto rejection = x64::CfgRegisterPlan::ValidateGraph(program.function.body.get(), program.function).Rejection();
        const auto expected_rejection = malformed == 1 || malformed == 2 ? x64::CfgPlanRejection::InvalidSSA :
                                        malformed == 6 ? x64::CfgPlanRejection::InvalidPhi : x64::CfgPlanRejection::InvalidMetadata;
        REQUIRE(rejection == expected_rejection);
        REQUIRE_THROWS_AS(x64::Translator::PrepareFunction(program.function, {}, &config, true, cfg, flags,
                            {.vector_cfg_allocation = vector}), std::runtime_error);
    }
}
