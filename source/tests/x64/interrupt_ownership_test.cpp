#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/interrupt_request.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/ir_assembler.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;
namespace {
using BlockFn = HaltReason (*)(backend::State*);
Config Configuration(bool native, bool cache = false) {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = native,
            .backend_isa = kX86_64, .uniform_buffer_size = 256, .static_program = true,
            .global_opts = cache ? Optimizations::ScalarRegisterCache : Optimizations::None,
            .stack_alignment = 16};
}
IntrusivePtr<ir::Block> Block(u64 location = 0x1000) { return new ir::Block(ir::Location{location}); }
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + 256> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    ~Storage() { state->~State(); }
    u64 Get(u32 offset) const { u64 value; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
};
void Store(ir::Assembler& as, u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, T::U64}, value); }
struct Request {
    backend::State* state{};
    Runtime* runtime{};
    std::atomic_bool entered{false}, release{false};
    bool rendezvous{};
    u64 bits{};
    HaltReason synchronous{HaltReason::None};
};
constexpr u64 result = 0x8123456789abcdefULL;
u64 RequestHelper(u64 pointer, u64, u64) {
    auto& request = *reinterpret_cast<Request*>(pointer);
    request.entered.store(true, std::memory_order_release);
    if (request.rendezvous) while (!request.release.load(std::memory_order_acquire)) std::this_thread::yield();
    if (request.bits) std::atomic_ref<u64>(request.state->exit_request).fetch_or(request.bits, std::memory_order_release);
    if (request.synchronous != HaltReason::None) request.state->halt_reason = request.synchronous;
    return result;
}
ir::Value Call(ir::Assembler& as, u32 shape) {
    auto pointer = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    const auto lambda = ir::Lambda{ir::Imm{reinterpret_cast<u64>(&RequestHelper)}};
    if (!shape) return as.CallLambda(lambda, pointer, pointer, pointer).SetType(T::U64);
    ir::Params args; args.Push(pointer); args.Push(pointer); args.Push(pointer);
    return (shape == 1 ? as.CallDynamic(lambda, args) : as.CallLocation(lambda, args)).SetType(T::U64);
}
void Put(backend::State* state, u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
u64 Get(backend::State* state, u32 offset) { u64 value; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
void MoveLabel(ir::Block* block, ir::Inst* before) {
    auto* label = &block->GetInstList().back(); block->RemoveInst(label); block->InsertBefore(label, before); block->ReIdInstr();
}
}

TEST_CASE("interrupt requester leaves owner halt state unchanged and clear reuses stopped Runtime", "[x64][interrupt][ownership]") {
    for (bool native : {false, true}) {
        backend::AddressSpace space{Configuration(native)}; Runtime runtime{&space};
        for (u32 i = 0; i < 64; ++i) {
            runtime.GetState()->halt_reason = HaltReason::PageFatal;
            std::atomic_ref<u64>(runtime.GetState()->exit_request).store(17, std::memory_order_release);
            std::array<std::thread, 4> requesters;
            for (auto& requester : requesters) requester = std::thread{[&] { for (u32 attempt = 0; attempt < 3; ++attempt) runtime.SignalInterrupt(); }};
            for (auto& requester : requesters) requester.join();
            REQUIRE(runtime.GetState()->halt_reason == HaltReason::PageFatal);
            REQUIRE((std::atomic_ref<u64>(runtime.GetState()->exit_request).load(std::memory_order_acquire) & kBackedgeSignalRequest) != 0);
            runtime.ClearInterrupt(); REQUIRE(runtime.GetState()->halt_reason == HaltReason::None);
            REQUIRE(std::atomic_ref<u64>(runtime.GetState()->exit_request).load(std::memory_order_acquire) == 17);
            std::atomic_ref<u64>(runtime.GetState()->exit_request).store(0, std::memory_order_release);
            runtime.SignalInterrupt(); REQUIRE(runtime.GetState()->halt_reason == HaltReason::None);
            REQUIRE(runtime.Run() == HaltReason::Signal);
            runtime.ClearInterrupt(); runtime.SetLocation(0x1000); REQUIRE(runtime.Run() == HaltReason::CodeMiss);
        }
    }
}

TEST_CASE("interpreter atomically observes Signal before empty blocks and local branch continue paths", "[x64][interrupt][interpreter]") {
    for (bool invert : {false, true}) {
        auto block = Block(); ir::Assembler as{block.get()};
        auto control = as.LoadImm(ir::Imm{u8(invert ? 0 : 1)}).SetType(T::U8);
        auto edge = invert ? as.NotGoto(ir::BOOL{control}) : as.Goto(ir::BOOL{control}); as.BindLabel(edge); MoveLabel(block.get(), control.Def());
        block->SetTerminal(ir::terminal::ReturnToHost{});
        for (u32 iteration = 0; iteration < 8; ++iteration) {
            Storage storage;
            std::thread requester{[&] { std::this_thread::sleep_for(std::chrono::milliseconds{2}); std::atomic_ref<u64>(storage.state->exit_request).fetch_or(kBackedgeSignalRequest, std::memory_order_release); }};
            backend::interp::Interpreter interpreter{*storage.state, block.get()}; const auto reason = interpreter.Run(); requester.join();
            REQUIRE(reason == HaltReason::Signal); REQUIRE(storage.state->halt_reason == HaltReason::Signal);
        }
    }
    auto empty = Block(); empty->SetTerminal(ir::terminal::ReturnToHost{});
    for (auto synchronous : {HaltReason::None, HaltReason::PageFatal, HaltReason::IllegalCode, HaltReason::CallHost, HaltReason::Step}) {
        Storage storage; storage.state->halt_reason = synchronous; storage.state->exit_request = kBackedgeSignalRequest | 17;
        backend::interp::Interpreter interpreter{*storage.state, empty.get()};
        REQUIRE(interpreter.Run() == (synchronous == HaltReason::None ? HaltReason::Signal : synchronous));
    }
}

TEST_CASE("Runtime interpreter empty block chains stop through atomic Signal and support clear reentry", "[x64][interrupt][interpreter]") {
    backend::AddressSpace space{Configuration(false)}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
    auto first = Block(0x1000), second = Block(0x1100);
    first->SetTerminal(ir::terminal::LinkBlock{ir::Location{0x1100}}); second->SetTerminal(ir::terminal::LinkBlock{ir::Location{0x1000}});
    REQUIRE(module->Push(first.get())); REQUIRE(module->Push(second.get()));
    for (u32 i = 0; i < 8; ++i) {
        runtime.SetLocation(0x1000);
        std::thread requester{[&] { std::this_thread::sleep_for(std::chrono::milliseconds{2}); runtime.SignalInterrupt(); }};
        const auto reason = runtime.Run(); requester.join(); REQUIRE(reason == HaltReason::Signal);
        REQUIRE((runtime.GetLocation() == 0x1000 || runtime.GetLocation() == 0x1100));
        REQUIRE(runtime.GetState()->halt_reason == HaltReason::None);
        runtime.ClearInterrupt(); runtime.SetLocation(0x1200); REQUIRE(runtime.Run() == HaltReason::CodeMiss);
    }
}

TEST_CASE("native and interpreter helper stop checks ignore low SMC preserve results and prioritize synchronous faults", "[x64][interrupt][helper]") {
    for (bool native : {false, true}) for (bool cache : {false, true}) for (u32 shape = 0; shape < 3; ++shape)
    for (u64 bits : {u64{0}, u64{17}, kBackedgeSignalRequest, kBackedgeSignalRequest | 17})
    for (auto synchronous : {HaltReason::None, HaltReason::PageFatal, HaltReason::IllegalCode, HaltReason::CallHost, HaltReason::Step}) {
        auto block = Block(); ir::Assembler as{block.get()};
        auto returned = Call(as, shape); Store(as, 8, returned);
        auto suffix = as.LoadImm(ir::Imm{u64{0x72134}}).SetType(T::U64); Store(as, 16, suffix);
        block->SetTerminal(ir::terminal::CheckHalt{ir::terminal::ReturnToHost{}});
        Storage storage; Request request{.state = storage.state, .bits = bits, .synchronous = synchronous}; Put(storage.state, 0, reinterpret_cast<u64>(&request));
        HaltReason reason;
        const auto original = _mm_getcsr(); const auto changed = (original & ~0x6000u) | 0x4000u; _mm_setcsr(changed);
        if (native) { x64::Translator translator{block.get(), {}, nullptr, nullptr, cache}; translator.Translate(); reason = translator.getCode<BlockFn>()(storage.state); }
        else { backend::interp::Interpreter interpreter{*storage.state, block.get()}; reason = interpreter.Run(); }
        const auto restored = _mm_getcsr(); _mm_setcsr(original); REQUIRE(restored == changed);
        const auto expected = synchronous != HaltReason::None ? synchronous : ((bits & kBackedgeSignalRequest) ? HaltReason::Signal : HaltReason::CallHost);
        const bool completed = synchronous == HaltReason::None && !(bits & kBackedgeSignalRequest);
        REQUIRE(reason == expected); REQUIRE(storage.Get(8) == (completed ? result : 0));
        REQUIRE(storage.Get(16) == (completed ? 0x72134 : 0));
        REQUIRE(storage.state->exit_request == bits);
    }
}

TEST_CASE("cross thread Runtime helper requests stop guest local loops and preserve synchronous priority", "[x64][interrupt][runtime]") {
    for (bool native : {false, true}) for (bool cache : {false, true}) for (bool fault : {false, true}) {
        backend::AddressSpace space{Configuration(native, cache)}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
        auto block = Block(); ir::Assembler as{block.get()};
        auto returned = Call(as, false); as.AdvancePC(ir::Imm{u32{4}}); Store(as, 8, returned);
        auto condition = as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8); auto edge = as.Goto(ir::BOOL{condition}); as.BindLabel(edge); MoveLabel(block.get(), returned.Def());
        block->SetTerminal(ir::terminal::ReturnToHost{});
        REQUIRE(module->Push(block.get())); if (native) REQUIRE(x64::CompileBlock(module, block.get()) != nullptr);
        Request request{.state = runtime.GetState(), .runtime = &runtime, .rendezvous = true,
                        .synchronous = fault ? HaltReason::PageFatal : HaltReason::None};
        Put(runtime.GetState(), 0, reinterpret_cast<u64>(&request)); runtime.SetLocation(0x1000);
        std::thread requester{[&] {
            while (!request.entered.load(std::memory_order_acquire)) std::this_thread::yield();
            runtime.SignalInterrupt(); request.release.store(true, std::memory_order_release);
        }};
        const auto reason = runtime.Run(); requester.join();
        REQUIRE(reason == (fault ? HaltReason::PageFatal : HaltReason::Signal));
        REQUIRE(runtime.GetLocation() == 0x1000); REQUIRE(Get(runtime.GetState(), 8) == 0);
        REQUIRE(runtime.GetState()->halt_reason == HaltReason::None);
        runtime.ClearInterrupt(); runtime.SetLocation(0x1200); REQUIRE(runtime.Run() == HaltReason::CodeMiss);
    }
}

TEST_CASE("native FunctionPlan Nop and terminal CheckHalt stop after committed memory writes without SMC replay", "[x64][interrupt][metadata]") {
    for (bool metadata : {false, true}) for (bool cache : {false, true}) for (u64 bits : {u64{17}, kBackedgeSignalRequest, kBackedgeSignalRequest | 17}) {
        auto block = Block(); ir::Assembler as{block.get()};
        auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64); auto value = as.LoadImm(ir::Imm{bits}).SetType(T::U64);
        as.StoreMemory(ir::Operand{address}, value);
        auto committed = as.LoadImm(ir::Imm{u64{73}}).SetType(T::U64); Store(as, 8, committed);
        as.AdvancePC(ir::Imm{u32{4}});
        backend::FunctionPlan plan; plan.body = block;
        if (metadata) { auto* marker = as.AppendInst(ir::OpCode::Nop); plan.halt_checks.insert(marker); }
        if (metadata) { auto suffix = as.LoadImm(ir::Imm{u64{31}}).SetType(T::U64); Store(as, 16, suffix); }
        block->SetTerminal(ir::terminal::CheckHalt{ir::terminal::ReturnToHost{}});
        Storage storage; Put(storage.state, 0, reinterpret_cast<u64>(&storage.state->exit_request));
        x64::Translator translator{block.get(), {}, nullptr, metadata ? &plan : nullptr, cache}; translator.Translate();
        const auto reason = translator.getCode<BlockFn>()(storage.state);
        REQUIRE(reason == ((bits & kBackedgeSignalRequest) ? HaltReason::Signal : HaltReason::CallHost));
        REQUIRE(storage.Get(8) == 73); REQUIRE(storage.state->exit_request == bits);
        if (bits & kBackedgeSignalRequest) REQUIRE(storage.state->current_loc == ir::Location{0x1004});
        REQUIRE(storage.Get(16) == (metadata && !(bits & kBackedgeSignalRequest) ? 31 : 0));
    }
}
