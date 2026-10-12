#include <array>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/x64/compile.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/include/sruntime.h"
#include "runtime/frontend/arm64/arm64_frontend.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
using T = ir::ValueType;
using O = ir::OpCode;
using Plan = x64::ScalarRegisterPlan;
namespace {
constexpr u32 extent = 4096;
using BlockFn = HaltReason (*)(backend::State*);
struct Storage {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + extent> bytes{};
    backend::State* state = new (bytes.data()) backend::State{};
    ~Storage() { state->~State(); }
    void Put(u32 offset, u64 value) { std::memcpy(state->uniform_buffer_begin + offset, &value, 8); }
    u64 Get(u32 offset) const { u64 value; std::memcpy(&value, state->uniform_buffer_begin + offset, 8); return value; }
};
Config NativeConfig(bool cache) {
    return {.loc_start = 0, .loc_end = 0x10000, .enable_jit = true,
            .backend_isa = kX86_64, .uniform_buffer_size = extent, .static_program = true,
            .global_opts = cache ? Optimizations::ScalarRegisterCache : Optimizations::None,
            .stack_alignment = 16};
}
IntrusivePtr<ir::Block> NewBlock() { return new ir::Block(ir::Location{0x1000}); }
void Store(ir::Assembler& as, u32 offset, ir::Value value) { as.StoreUniform(ir::Uniform{offset, value.Type()}, value); }
u64 Mask(T type) { const auto bits = ir::GetValueSizeByte(type) * 8; return bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1; }
ir::Value ImmediateBinary(ir::Assembler& as, O op, ir::Value input, u64 immediate, T type) {
    return ir::Value{as.AppendInst(op, input, ir::Operand{ir::Imm{immediate}})}.SetType(type);
}
void MoveBefore(ir::Block* block, ir::Inst* moved, ir::Inst* before) {
    block->RemoveInst(moved); block->InsertBefore(moved, before); block->ReIdInstr();
}
struct Compiled {
    Config config;
    x64::Translator translator;
    Compiled(ir::Block* block, bool cache) : config(NativeConfig(cache)), translator(block, {}, &config, nullptr, cache) { translator.Translate(); }
    HaltReason Run(Storage& storage) { return translator.getCode<BlockFn>()(storage.state); }
};
u64 ClobberAndChangeUniform(u64 pointer, u64 value, u64) {
    auto* state = reinterpret_cast<backend::State*>(pointer);
    const u64 replacement = 0xaaaa5555bbbb6666ULL;
    std::memcpy(state->uniform_buffer_begin + 24, &replacement, 8);
    asm volatile("xor %%r8, %%r8\n\txor %%r9, %%r9\n\txor %%r10, %%r10\n\txor %%r11, %%r11\n\t"
                 "pxor %%xmm0, %%xmm0\n\tpxor %%xmm1, %%xmm1"
                 ::: "r8", "r9", "r10", "r11", "xmm0", "xmm1", "cc");
    return value * 7 + 5;
}
class CodeMemory final : public MemoryInterface {
public:
    explicit CodeMemory(std::vector<u32> instructions) : words(std::move(instructions)) { words.push_back(0xd4200000); }
    bool Read(void* destination, size_t address, size_t size) override {
        if (address < 0x1000 || address - 0x1000 > words.size() * 4 || size > words.size() * 4 - (address - 0x1000)) return false;
        std::memcpy(destination, reinterpret_cast<u8*>(words.data()) + address - 0x1000, size); return true;
    }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* pointer) override {
        const auto address = reinterpret_cast<size_t>(pointer);
        return address >= 0x1000 && address - 0x1000 < words.size() * 4 ? reinterpret_cast<u8*>(words.data()) + address - 0x1000 : nullptr;
    }
    std::vector<u32> words;
};
u64 CompareFlags(u64 old, u64 a, u64 b, bool subtract, u32 bits) {
    const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
    const auto sign = u64{1} << (bits - 1); a &= mask; b &= mask;
    const auto wide = subtract ? static_cast<unsigned __int128>(a) - b : static_cast<unsigned __int128>(a) + b;
    const auto result = u64(wide) & mask;
    const bool carry = subtract ? a >= b : wide > mask;
    const bool overflow = subtract ? ((a ^ b) & (a ^ result) & sign) != 0 : ((~(a ^ b)) & (a ^ result) & sign) != 0;
    return (old & ~(u64{15} << 28)) | (u64(result >> (bits - 1)) << 31) |
        (u64(result == 0) << 30) | (u64(carry) << 29) | (u64(overflow) << 28);
}
}

TEST_CASE("x64 CFG cache splits diamond branches and join while retaining all cross segment values in homes", "[x64][scalar-cfg][execution]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64})
    for (u32 slot = 0; slot < 3; ++slot) for (bool invert : {false, true}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        const auto cross = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        const auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8);
        const auto taken_edge = invert ? as.NotGoto(ir::BOOL{condition}) : as.Goto(ir::BOOL{condition});
        std::array<ir::Value, 2> fallback_prefix, taken_prefix;
        for (u32 i = 0; i < slot; ++i) fallback_prefix[i] = as.LoadUniform(ir::Uniform{128 + i * 8, T::U64}).SetType(T::U64);
        const auto fallback = as.LoadUniform(ir::Uniform{24, type}).SetType(type);
        const auto fallback_add = ImmediateBinary(as, O::Add, fallback, 7, type);
        const auto fallback_xor = ImmediateBinary(as, O::Xor, fallback_add, 0x55, type); Store(as, 96, fallback_xor);
        for (u32 i = 0; i < slot; ++i) Store(as, 192 + i * 8, fallback_prefix[i]);
        const auto always = as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8); const auto done = as.Goto(ir::BOOL{always});
        as.BindLabel(taken_edge);
        for (u32 i = 0; i < slot; ++i) taken_prefix[i] = as.LoadUniform(ir::Uniform{128 + i * 8, T::U64}).SetType(T::U64);
        const auto taken = as.LoadUniform(ir::Uniform{24, type}).SetType(type);
        const auto taken_sub = ImmediateBinary(as, O::Sub, taken, 9, type);
        const auto taken_xor = ImmediateBinary(as, O::Xor, taken_sub, 0xaa, type);
        const auto taken_add = ImmediateBinary(as, O::Add, taken_xor, 11, type); Store(as, 96, taken_add);
        for (u32 i = 0; i < slot; ++i) Store(as, 208 + i * 8, taken_prefix[i]);
        as.BindLabel(done);
        const auto join = as.LoadUniform(ir::Uniform{96, type}).SetType(type);
        const auto joined = as.Add(join, ir::Operand{cross}).SetType(type);
        const auto final = ImmediateBinary(as, O::Xor, joined, 0x19, type); Store(as, 104, final); Store(as, 112, cross);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = Plan::Build(block.get()); REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
        for (auto value : {cross, condition, always}) { REQUIRE_FALSE(plan.Slot(value.Def())); REQUIRE(plan.RequiresCanonicalHome(value.Def())); }
        REQUIRE(plan.Slot(fallback.Def()) == slot); REQUIRE(plan.Slot(taken.Def()) == slot); REQUIRE(plan.Slot(join.Def()) == 0);
        REQUIRE(plan.Find(fallback.Def())->segment != plan.Find(taken.Def())->segment);
        REQUIRE(plan.Find(taken.Def())->segment != plan.Find(join.Def())->segment);
        for (auto value : {fallback, fallback_add, fallback_xor, taken, taken_sub, taken_xor, taken_add, join, joined, final})
            REQUIRE_FALSE(plan.RequiresCanonicalHome(value.Def()));
        Compiled off{block.get(), false}, on{block.get(), true};
        REQUIRE(off.translator.Statistics().scalar_coalesced_binary_sites == 0);
        REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites == 7);
        REQUIRE(on.translator.Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_store_sites < off.translator.Statistics().At(x64::TransferRegion::CanonicalSSA).scalar_store_sites);
        const auto mask = Mask(type);
        for (u64 value : {u64{0}, u64{1}, mask, u64{0x123456789abcdef0}})
        for (u64 control : {u64{0}, u64{1}}) for (auto* code : {&off, &on}) {
            Storage storage; storage.Put(0, control); storage.Put(8, 0xfedcba9876543210ULL); storage.Put(24, value);
            for (u32 i = 0; i < slot; ++i) storage.Put(128 + i * 8, 0x1122334455667788ULL + i);
            const u64 flags = 0xfedcba9876abcdefULL; storage.state->host_cpu_flags = flags;
            const bool choose_taken = invert ? control == 0 : control != 0;
            const auto selected = choose_taken ? ((((value & mask) - 9) & mask) ^ 0xaa) + 11 : (((value & mask) + 7) & mask) ^ 0x55;
            const auto result = (((selected & mask) + (0xfedcba9876543210ULL & mask)) & mask) ^ 0x19;
            REQUIRE(code->Run(storage) == HaltReason::CallHost);
            REQUIRE(storage.Get(96) == (selected & mask)); REQUIRE(storage.Get(104) == (result & mask));
            REQUIRE(storage.Get(112) == 0xfedcba9876543210ULL); REQUIRE(storage.state->host_cpu_flags == flags);
            for (u32 i = 0; i < slot; ++i) {
                REQUIRE(storage.Get((choose_taken ? 208 : 192) + i * 8) == 0x1122334455667788ULL + i);
                REQUIRE(storage.Get((choose_taken ? 192 : 208) + i * 8) == 0);
            }
        }
    }
}

TEST_CASE("x64 CFG cache reexecutes local writers on every backward label entry", "[x64][scalar-cfg][loop]") {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool void_identity : {false, true}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        const auto counter = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        const auto accumulator = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        const auto sum = as.Add(accumulator, ir::Operand{counter}).SetType(type); Store(as, 8, sum);
        const auto next = as.Sub(counter, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64); Store(as, 0, next);
        const auto condition = as.TestNotZero(next).SetType(T::U8); const auto back = as.Goto(ir::BOOL{condition});
        // Goto normally infers U8 from its BOOL condition. Its result is a
        // label identity rather than runtime data, so also exercise the
        // supported VOID identity without changing the actual condition.
        if (void_identity) back.Def()->SetReturn(T::VOID);
        as.BindLabel(back); auto* header = &block->GetInstList().back(); MoveBefore(block.get(), header, counter.Def());
        const auto after = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        const auto final = ImmediateBinary(as, O::Add, after, 7, type); Store(as, 48, final);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = Plan::Build(block.get()); REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
        REQUIRE(header->GetArg<ir::Value>(0).Def() == back.Def());
        REQUIRE(back.Type() == (void_identity ? T::VOID : T::U8));
        REQUIRE(header->GetArg<ir::Value>(0).Type() == back.Type());
        REQUIRE(back.Def()->GetArg<ir::Value>(0).Def() == condition.Def());
        REQUIRE(back.Def()->GetArg<ir::Value>(0).Type() == T::U8);
        REQUIRE(&block->GetInstList().front() == header); // Real target precedes every loop writer and the backedge.
        REQUIRE(plan.Slot(counter.Def())); REQUIRE(plan.Slot(accumulator.Def())); REQUIRE(plan.Slot(sum.Def()));
        REQUIRE_FALSE(plan.Slot(condition.Def())); REQUIRE(plan.RequiresCanonicalHome(condition.Def()));
        REQUIRE(plan.Find(counter.Def())->segment != plan.Find(after.Def())->segment);
        Compiled off{block.get(), false}, on{block.get(), true}; REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites > 0);
        const auto mask = Mask(type);
        for (u64 count : {u64{1}, u64{2}, u64{5}, u64{17}})
        for (u64 initial : {u64{0}, mask, mask / 2}) for (auto* code : {&off, &on}) {
            Storage storage; storage.Put(0, count); storage.Put(8, initial); const u64 flags = 0xa5a55a5adeadbeefULL; storage.state->host_cpu_flags = flags;
            const auto expected = (initial + count * (count + 1) / 2) & mask;
            REQUIRE(code->Run(storage) == HaltReason::CallHost); REQUIRE(storage.Get(0) == 0);
            REQUIRE(storage.Get(8) == expected); REQUIRE(storage.Get(48) == ((expected + 7) & mask));
            REQUIRE(storage.state->host_cpu_flags == flags);
        }
    }
}

TEST_CASE("x64 CFG cache excludes every raw physical Lambda Params and metadata cross segment use", "[x64][scalar-cfg][analysis]") {
    for (u32 shape = 0; shape < 6; ++shape) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        const auto cross = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        const auto local = as.Add(cross, ir::Operand{ir::Imm{u64{3}}}).SetType(T::U64); Store(as, 32, local);
        const auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8); const auto edge = as.Goto(ir::BOOL{condition});
        as.BindLabel(edge);
        const auto after = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
        const auto consumer = as.Add(after, ir::Operand{ir::Imm{u64{9}}}).SetType(T::U64);
        if (shape == 0) consumer.Def()->SetArg(4, cross);
        else if (shape == 1) consumer.Def()->SetArg(4, ir::Lambda{cross});
        else if (shape == 2) { ir::Params params; params.Push(cross); params.Push(cross); consumer.Def()->SetArg(4, params); }
        else if (shape == 3) { auto raw = cross; raw = raw.SetCastType(T::V128); (void)as.BitCast(raw).SetType(T::V128); }
        else if (shape == 4) (void)as.GetOperand(ir::Operand{cross, cross, {ir::OperandOp::PlusExt, 3}}).SetType(T::U64);
        else as.SaveFlags(local, ir::Flags::NZCV);
        Store(as, 48, consumer); block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = Plan::Build(block.get()); REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
        if (shape == 5) { REQUIRE_FALSE(plan.Slot(local.Def())); REQUIRE(plan.RequiresCanonicalHome(local.Def())); }
        else { REQUIRE_FALSE(plan.Slot(cross.Def())); REQUIRE(plan.RequiresCanonicalHome(cross.Def())); }
        REQUIRE_FALSE(plan.Slot(condition.Def())); REQUIRE(plan.Slot(after.Def())); REQUIRE(plan.Slot(consumer.Def()));
    }
}

TEST_CASE("x64 CFG cache keeps prebranch terminal inputs home while caching final segment terminal inputs", "[x64][scalar-cfg][terminal]") {
    for (bool switch_terminal : {false, true}) for (bool local_terminal : {false, true}) {
        auto block = NewBlock(); ir::Assembler as{block.get()};
        const auto before = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        const auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8); const auto edge = as.Goto(ir::BOOL{condition});
        const auto skipped = as.LoadImm(ir::Imm{u64{0x99}}).SetType(T::U64); Store(as, 32, skipped);
        as.BindLabel(edge);
        const auto after = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        Store(as, 40, before); const auto key = local_terminal ? after : before;
        if (switch_terminal) {
            Vector<ir::terminal::Switch::Case> cases;
            cases.push_back({ir::Imm{u64{0}}, ir::terminal::LinkBlock{ir::Location{0x2000}}});
            cases.push_back({ir::Imm{u64{1}}, ir::terminal::LinkBlock{ir::Location{0x3000}}});
            cases.push_back({ir::Imm{UINT64_MAX}, ir::terminal::LinkBlock{ir::Location{0x4000}}});
            block->SetTerminal(ir::terminal::Switch{key, cases});
        } else block->SetTerminal(ir::terminal::CheckHalt{ir::terminal::If{ir::BOOL{key},
                    ir::terminal::LinkBlock{ir::Location{0x2000}}, ir::terminal::LinkBlock{ir::Location{0x3000}}}});
        const auto plan = Plan::Build(block.get()); REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
        REQUIRE_FALSE(plan.Slot(before.Def())); REQUIRE(plan.RequiresCanonicalHome(before.Def()));
        if (local_terminal) { REQUIRE(plan.Slot(after.Def())); REQUIRE(plan.Find(after.Def())->last_use_phase == x64::ScalarCachePhase::Read); }
        else REQUIRE_FALSE(plan.Slot(after.Def())); // Unused producer, no manufactured cache slot.
        Compiled off{block.get(), false}, on{block.get(), true};
        for (u64 value : {u64{0}, u64{1}, UINT64_MAX, u64{7}})
        for (u64 control : {u64{0}, u64{1}}) for (auto* code : {&off, &on}) {
            Storage storage; storage.Put(0, control); storage.Put(8, value);
            REQUIRE(code->Run(storage) == HaltReason::None);
            const u64 expected_pc = switch_terminal ? value == 0 ? 0x2000 : value == 1 ? 0x3000 : value == UINT64_MAX ? 0x4000 : 0x1000 : value ? 0x2000 : 0x3000;
            REQUIRE(storage.state->current_loc.Value() == expected_pc); REQUIRE(storage.Get(40) == value);
            REQUIRE(storage.Get(32) == (control ? 0 : 0x99));
        }
    }
}

TEST_CASE("x64 CFG cache survives returning helpers inside a segment and skipped writers", "[x64][scalar-cfg][helper]") {
    for (bool cache : {false, true}) {
        Storage storage; auto block = NewBlock(); ir::Assembler as{block.get()};
        const auto cross = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        const auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8); const auto edge = as.Goto(ir::BOOL{condition});
        const auto dirty = as.LoadImm(ir::Imm{u64{0xfedcba9876543210}}).SetType(T::U64);
        const auto dirtied = ImmediateBinary(as, O::Xor, dirty, 0xff, T::U64); Store(as, 64, dirtied);
        as.BindLabel(edge);
        const auto original = as.LoadUniform(ir::Uniform{24, T::U64}).SetType(T::U64);
        const auto pointer = as.LoadImm(ir::Imm{reinterpret_cast<u64>(storage.state)}).SetType(T::U64);
        const auto called = as.CallHost(&ClobberAndChangeUniform, pointer, original, original).SetType(T::U64);
        const auto after = as.Add(original, ir::Operand{cross}).SetType(T::U64);
        Store(as, 96, original); Store(as, 104, called); Store(as, 112, after);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = Plan::Build(block.get()); REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping());
        REQUIRE_FALSE(plan.Slot(cross.Def())); REQUIRE(plan.Slot(original.Def())); REQUIRE(plan.RequiresCanonicalHome(original.Def()));
        REQUIRE(plan.Slot(pointer.Def())); REQUIRE(plan.RequiresCanonicalHome(pointer.Def())); REQUIRE(plan.Slot(after.Def()));
        Compiled code{block.get(), cache};
        for (u64 value : {u64{0}, u64{19}, UINT64_MAX}) for (u64 control : {u64{0}, u64{1}}) {
            storage.Put(0, control); storage.Put(8, 0x123456789abcdef0ULL); storage.Put(24, value); storage.Put(64, 0);
            const u64 flags = 0xfedcba9876abcdefULL; storage.state->host_cpu_flags = flags; storage.state->halt_reason = HaltReason::None;
            REQUIRE(code.Run(storage) == HaltReason::CallHost);
            REQUIRE(storage.Get(24) == 0xaaaa5555bbbb6666ULL); REQUIRE(storage.Get(96) == value);
            REQUIRE(storage.Get(104) == value * 7 + 5); REQUIRE(storage.Get(112) == value + 0x123456789abcdef0ULL);
            REQUIRE(storage.Get(64) == (control ? 0 : (0xfedcba9876543210ULL ^ 0xff))); REQUIRE(storage.state->host_cpu_flags == flags);
        }
    }
}

TEST_CASE("x64 CFG cache native faults preserve already committed state from a cached segment", "[x64][scalar-cfg][fault]") {
    for (bool cache : {false, true}) for (u64 control : {u64{0}, u64{1}}) {
        backend::AddressSpace space{NativeConfig(cache)}; auto module = space.GetDefaultModule(); Runtime runtime{&space};
        auto* inaccessible = mmap(nullptr, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0); REQUIRE(inaccessible != MAP_FAILED);
        auto block = NewBlock(); ir::Assembler as{block.get()};
        const auto condition = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U8); const auto edge = as.Goto(ir::BOOL{condition});
        const auto skipped = as.LoadImm(ir::Imm{u64{13}}).SetType(T::U64); Store(as, 32, skipped); as.BindLabel(edge);
        const auto input = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        const auto committed = ImmediateBinary(as, O::Add, input, 31, T::U64); Store(as, 40, committed);
        as.AdvancePC(ir::Imm{u32{4}});
        const auto address = as.LoadImm(ir::Imm{reinterpret_cast<u64>(inaccessible)}).SetType(T::U64);
        const auto loaded = as.LoadMemory(ir::Operand{address}).SetType(T::U64); Store(as, 48, loaded); Store(as, 56, committed);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        const auto plan = Plan::Build(block.get()); REQUIRE(plan.Enabled()); REQUIRE(plan.Slot(input.Def())); REQUIRE(plan.Slot(committed.Def()));
        REQUIRE(x64::CompileBlock(module, block.get()) != nullptr);
        const u64 value = 0xfffffffffffffff0ULL, flags = 0xfedcba9876abcdefULL;
        std::memcpy(runtime.GetUniformBuffer().data(), &control, 8); std::memcpy(runtime.GetUniformBuffer().data() + 8, &value, 8);
        runtime.GetState()->host_cpu_flags = flags; runtime.GetState()->guest_addr_limit = UINT64_MAX; runtime.GetState()->guest_addr_mask = UINT64_MAX;
        runtime.SetLocation(0x1000); const auto reason = runtime.Run(); REQUIRE(munmap(inaccessible, 4096) == 0);
        REQUIRE(reason == HaltReason::PageFatal); REQUIRE(runtime.GetLocation() == 0x1004); REQUIRE(runtime.GetState()->host_cpu_flags == flags);
        u64 before{}, selected{}, after{}, tail{};
        std::memcpy(&before, runtime.GetUniformBuffer().data() + 32, 8); std::memcpy(&selected, runtime.GetUniformBuffer().data() + 40, 8);
        std::memcpy(&after, runtime.GetUniformBuffer().data() + 48, 8); std::memcpy(&tail, runtime.GetUniformBuffer().data() + 56, 8);
        REQUIRE(before == (control ? 0 : 13)); REQUIRE(selected == value + 31); REQUIRE(after == 0); REQUIRE(tail == 0);
        REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0); REQUIRE(runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
    }
}

TEST_CASE("x64 CFG cache activates before and after decoded ARM64 conditional compares", "[x64][scalar-cfg][arm64]") {
    using namespace vixl::aarch64;
    for (bool wide : {false, true}) for (bool subtract : {false, true}) {
        // ADD r0,r1,#7; ADD r0,r0,#3; CCMP/CCMN r0,r2,#NZCV,EQ;
        // ADD r3,r0,#11; EOR r4,r3,r1; BRK.
        const u32 size = wide ? 0x80000000u : 0;
        const u32 fallback_nzcv = 0xb;
        CodeMemory memory{{size | 0x11000000u | (7u << 10) | (1u << 5),
                           size | 0x11000000u | (3u << 10),
                           u32(wide ? (subtract ? CCMP_x : CCMN_x) : (subtract ? CCMP_w : CCMN_w)) | (2u << 16) | fallback_nzcv,
                           size | 0x11000000u | (11u << 10) | 3u,
                           size | 0x4a000000u | (1u << 16) | (3u << 5) | 4u}};
        auto block = NewBlock(); ir::Assembler as{block.get()}; swift::arm64::A64Decoder{0x1000, &memory, &as}.Decode();
        const auto plan = Plan::Build(block.get()); REQUIRE(plan.Enabled()); REQUIRE(plan.ValidateNonOverlapping()); REQUIRE_FALSE(plan.Intervals().empty());
        bool before{}, after{}; u32 first_control = UINT32_MAX, last_control{}, position{};
        for (auto& inst : block->GetInstList()) {
            if (inst.GetOp() == O::Goto || inst.GetOp() == O::NotGoto || inst.GetOp() == O::BindLabel) { first_control = std::min(first_control, position); last_control = position; }
            ++position;
        }
        REQUIRE(first_control != UINT32_MAX);
        for (const auto& interval : plan.Intervals()) { before |= interval.definition_position < first_control; after |= interval.definition_position > last_control; }
        REQUIRE(before); REQUIRE(after);
        Compiled off{block.get(), false}, on{block.get(), true};
        REQUIRE(on.translator.Statistics().scalar_cache_read_sites > 0); REQUIRE(on.translator.Statistics().scalar_cache_write_sites > 0);
        REQUIRE(on.translator.Statistics().scalar_coalesced_binary_sites > 0);
        const u32 bits = wide ? 64 : 32; const auto mask = wide ? UINT64_MAX : u64{UINT32_MAX};
        for (u64 input : {u64{0}, mask, u64{0x7fffffffffffffff}, u64{0x8000000000000000}})
        for (bool equal : {false, true}) for (auto* code : {&off, &on}) {
            Storage storage; auto& context = *reinterpret_cast<swift::arm64::ThreadContext64*>(storage.state->uniform_buffer_begin);
            context.r[1] = input; context.r[2] = 0x123456789abcdef0ULL;
            const auto flags = (0x123456787abcdef0ULL & ~(u64{1} << 30)) | (u64(equal) << 30); storage.state->host_cpu_flags = flags;
            const auto r0 = ((input & mask) + 10) & mask;
            const auto expected_flags = equal ? CompareFlags(flags, r0, context.r[2], subtract, bits) :
                (flags & ~(u64{15} << 28)) | (u64(fallback_nzcv) << 28);
            REQUIRE(code->Run(storage) == HaltReason::CallHost);
            REQUIRE(context.r[0] == r0); REQUIRE(context.r[3] == ((r0 + 11) & mask));
            REQUIRE(context.r[4] == (((r0 + 11) & mask) ^ (input & mask)));
            REQUIRE(storage.state->host_cpu_flags == expected_flags); REQUIRE(context.padding == u32(swift::arm64::InterruptReason::BRK));
        }
    }
}
