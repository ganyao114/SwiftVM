#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/context.h"
#include "runtime/backend/execution_owner.h"
#include "runtime/backend/guest_memory_scope.h"
#include "runtime/backend/riscv64/memory_protocol.h"
#include "runtime/backend/runtime.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "runtime/frontend/arm64/single_instruction_admission.h"
#include "translator/arm64/translator.h"

using namespace swift;
using namespace swift::runtime;
namespace a64 = swift::translator::arm64;
namespace {
using Context = swift::arm64::ThreadContext64;
using T = ir::ValueType;
using Trap = swift::arm64::InterruptReason;
using Exit = swift::translator::ExitReason;
constexpr u64 pc = 0x1000, other_flags = 0xA5A5000002345678ULL;
constexpr std::array<u64, 3> recovery_sentinels{0x12345678, 0x87654321, 0};

struct Provider final : MemoryInterface {
    std::array<u8, 0x10000> memory{};
    std::atomic<unsigned> fetches{}, reads{}, writes{}, pointers{};
    std::vector<std::pair<u64, size_t>> requests;
    std::function<void()> before_fetch;
    CodeFetchResult status{CodeFetchResult::Success};
    void Word(u64 address, u32 word) { std::memcpy(memory.data() + address, &word, 4); }
    CodeFetchResult ReadCode(void* output, size_t address, size_t size) override {
        ++fetches; requests.emplace_back(address, size); if (before_fetch) before_fetch();
        if (status != CodeFetchResult::Success) return status;
        if (size != 4 || address > memory.size() - size) return CodeFetchResult::Fault;
        std::memcpy(output, memory.data() + address, size); return CodeFetchResult::Success;
    }
    bool MatchesCodeAddressSpace(uintptr_t bias, u64 mask) const override {
        return bias == reinterpret_cast<uintptr_t>(memory.data()) && mask == memory.size() - 1;
    }
    bool Read(void*, size_t, size_t) override { ++reads; return false; }
    bool Write(void*, size_t, size_t) override { ++writes; return false; }
    void* GetPointer(void*) override { ++pointers; return nullptr; }
    static SingleDecodeResult Decode(const void*, LocationDescriptor location, MemoryInterface& memory, ir::Block& block) {
        ir::Assembler assembler{&block}; swift::arm64::A64Decoder decoder{location, &memory, &assembler};
        switch (decoder.DecodeSingle()) {
        case CodeFetchResult::Success: return SingleDecodeResult::Ready;
        case CodeFetchResult::Fault: return SingleDecodeResult::FaultIR;
        case CodeFetchResult::Unsupported: return SingleDecodeResult::Unsupported;
        }
        return SingleDecodeResult::Unsupported;
    }
    static bool Admit(const void*, const ir::Block& block) { return swift::arm64::Arm64SingleInstructionAdmitted(block); }
};

Context Initial() {
    Context context{};
    for (unsigned index = 0; index < context.r.size(); ++index) context.r[index] = 0x8123456789abcdefULL ^ u64(index) * 0x0101010101010101ULL;
    context.fp = 0x123456789abcdef0; context.lr = 0x9988776655443322; context.sp = 0x8123000045670000;
    context.pc = pc; context.pstate = 0x13579bdf; context.padding = 0x2468ace0;
    context.fpcr = 0; context.fpsr = 0x08000000; context.tpidr = 0xfedcba9876543210;
    for (unsigned index = 0; index < 32; ++index) {
        const std::array<u64, 2> lanes{0x1122334455667788 + index, 0x8877665544332211 - index};
        std::memcpy(&context.v[index], lanes.data(), 16);
    }
    return context;
}
Config Configuration(Provider& provider, SingleDecodeService service, bool registered) {
    return {.loc_start = 0, .loc_end = provider.memory.size(), .enable_jit = false,
            .backend_isa = kX86_64, .uniform_buffer_size = sizeof(Context), .static_program = true,
            .global_opts = Optimizations::None, .stack_alignment = 16,
            .memory_base = provider.memory.data(), .guest_addr_mask = provider.memory.size() - 1,
            .memory = registered ? &provider : nullptr, .single_decode = service};
}
struct Fixture {
    Provider provider;
    backend::AddressSpace space;
    Runtime runtime;
    explicit Fixture(SingleDecodeService service = {&Provider::Decode, nullptr, &Provider::Admit}, bool registered = true)
        : space(Configuration(provider, service, registered)), runtime(&space) { Seed(Initial()); }
    Context& Guest() { return *reinterpret_cast<Context*>(runtime.GetUniformBuffer().data()); }
    void Seed(const Context& context, u64 flags = other_flags) {
        Guest() = context; runtime.SetLocation(context.pc); runtime.GetState()->host_cpu_flags = flags;
        runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] = recovery_sentinels[0];
        runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] = recovery_sentinels[1];
        runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] = recovery_sentinels[2];
    }
    void NoPublication(u64 address = pc) {
        REQUIRE(backend::IsEmpty(space.GetDefaultModule()->GetNode(ir::Location{address})));
        REQUIRE(space.GetCodeCache(address) == nullptr);
        REQUIRE(space.GetCallCodeCacheTable().Lookup(address) == 0);
        REQUIRE(space.GetPendingCallCodeCacheTable().Lookup(address) == 0);
    }
};
void Equal(const Context& expected, const Context& actual) { REQUIRE(std::memcmp(&expected, &actual, sizeof(Context)) == 0); }
std::vector<u8> Capture(Runtime& runtime) {
    std::vector<u8> bytes(sizeof(backend::State) + sizeof(Context));
    std::memcpy(bytes.data(), runtime.GetState(), bytes.size()); return bytes;
}
HaltReason Step(Fixture& fixture) {
    REQUIRE_FALSE(fixture.space.GetConfig().enable_jit);
    const auto fp = _mm_getcsr(), desired = (fp & ~0x6000u) | 0x4000u | 0x1f80u | 0x15u;
    struct Restore { u32 original; ~Restore() { _mm_setcsr(original); } } restore{fp};
    _mm_setcsr(desired);
    const auto mapping = backend::GuestMemoryScope::Current();
    const auto readers = backend::riscv64::anonymous_memory_readers.load();
    const auto kernels = backend::riscv64::memory_kernel_depth.load();
    const auto reason = fixture.runtime.Step();
    REQUIRE(_mm_getcsr() == desired);
    REQUIRE(fixture.runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == recovery_sentinels[0]);
    REQUIRE(fixture.runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == recovery_sentinels[1]);
    REQUIRE(fixture.runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == recovery_sentinels[2]);
    REQUIRE(backend::riscv64::anonymous_memory_readers.load() == readers);
    REQUIRE(backend::riscv64::memory_kernel_depth.load() == kernels);
    REQUIRE_FALSE(backend::RuntimeExecutionActiveOnCurrentThread());
    REQUIRE(backend::GuestMemoryScope::Current().bias == mapping.bias);
    REQUIRE(backend::GuestMemoryScope::Current().mask == mapping.mask);
    REQUIRE(fixture.provider.reads.load() == 0); REQUIRE(fixture.provider.writes.load() == 0); REQUIRE(fixture.provider.pointers.load() == 0);
    return reason;
}
void Check(Fixture& fixture, const Context& context, u64 flags, u64 location) {
    Equal(context, fixture.Guest()); REQUIRE(fixture.runtime.GetLocation() == location);
    REQUIRE(fixture.runtime.GetState()->host_cpu_flags == flags); fixture.NoPublication();
    for (const auto& request : fixture.provider.requests) REQUIRE(request.second == 4);
}
u64 Arithmetic(bool wide, bool subtract, u64 lhs, u64 rhs, bool carry, u64* value = nullptr) {
    const unsigned width = wide ? 64 : 32; const u64 mask = wide ? UINT64_MAX : UINT32_MAX;
    lhs &= mask; rhs &= mask;
    const unsigned __int128 a = lhs, b = rhs, extra = subtract ? !carry : carry;
    const auto full = subtract ? a - b - extra : a + b + extra; const auto result = u64(full) & mask;
    const auto signed_value = [=](u64 input) -> __int128 { return input & (u64{1} << (width - 1)) ? __int128(input) - (__int128{1} << width) : __int128(input); };
    const auto total = subtract ? signed_value(lhs) - signed_value(rhs) - __int128(extra) : signed_value(lhs) + signed_value(rhs) + __int128(extra);
    if (value) *value = result;
    return (u64(result >> (width - 1)) << 31) | (u64(result == 0) << 30) |
           (u64(subtract ? a >= b + extra : full >> width) << 29) |
           (u64(total < -(__int128{1} << (width - 1)) || total >= (__int128{1} << (width - 1))) << 28);
}
using ArmInstance = std::unique_ptr<a64::Arm64Instance, decltype(&a64::Arm64Instance::Destroy)>;
using ArmCore = std::unique_ptr<a64::Arm64Core, decltype(&a64::Arm64Core::Destroy)>;
struct CoreFixture {
    Provider provider;
    ArmInstance instance{a64::Arm64Instance::Make(provider.memory.data(), provider.memory.size() - 1, &provider), &a64::Arm64Instance::Destroy};
    ArmCore core{a64::Arm64Core::Make(instance.get()), &a64::Arm64Core::Destroy};
    CoreFixture() { core->GetContext() = Initial(); }
};
template<class Predicate> bool Poll(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!predicate()) { if (std::chrono::steady_clock::now() > deadline) return false; std::this_thread::yield(); }
    return true;
}
struct Rejected {
    unsigned shape{}, calls{}, helpers{};
    static u64 Helper(u64 pointer, u64, u64) { ++reinterpret_cast<Rejected*>(pointer)->helpers; return 5; }
    static SingleDecodeResult Decode(const void* pointer, LocationDescriptor, MemoryInterface&, ir::Block& block) {
        auto& self = *static_cast<Rejected*>(const_cast<void*>(pointer)); ++self.calls;
        ir::Assembler as{&block}; auto value = as.LoadImm(ir::Imm{u64{0x1234}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{u32(offsetof(Context, r)), T::U64}, value);
        auto address = as.LoadImm(ir::Imm{u64{0x8000}}).SetType(T::U64);
        switch (self.shape) {
        case 0: as.LoadMemory(ir::Operand{address}).SetType(T::U64); break;
        case 1: as.StoreMemory(ir::Operand{address}, value); break;
        case 2: as.LoadMemoryTSO(ir::Operand{address}).SetType(T::U64); break;
        case 3: as.StoreMemoryTSO(ir::Operand{address}, value); break;
        case 4: as.MemoryCopy(ir::Lambda{address}, ir::Lambda{address}, ir::Imm{u32{8}}); break;
        case 5: as.MemoryCopyTSO(ir::Lambda{address}, ir::Lambda{address}, ir::Imm{u32{8}}); break;
        case 6: as.CompareAndSwap(address, value, value).SetType(T::U64); break;
        case 7: as.CompareAndSwap128(address, value, value, value, value).SetType(T::V128); break;
        case 8: as.AtomicExchange(address, value).SetType(T::U64); break;
        case 9: as.AtomicFetchAdd(address, value).SetType(T::U64); break;
        case 10: as.AtomicRMW(address, ir::Imm{u32{0}}, value, value, value).SetType(T::U64); break;
        case 11: as.CheckMemoryAlignment(address, ir::Imm{u32{8}}); break;
        case 12: as.AdvancePC(ir::Imm{u32{4}}); break;
        case 13: return SingleDecodeResult::Ready; // no terminal
        case 14: {
            auto jump = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)}); as.BindLabel(jump);
            auto* label = &block.GetInstList().back(); block.RemoveInst(label); block.InsertBefore(label, jump.Def()); block.ReIdInstr(); break;
        }
        case 15: {
            Vector<ir::terminal::Switch::Case> cases;
            block.SetTerminal(ir::terminal::Switch{value, cases}); return SingleDecodeResult::Ready;
        }
        case 16: case 17: case 18: case 19: {
            auto argument = as.LoadImm(ir::Imm{reinterpret_cast<u64>(&self)}).SetType(T::U64);
            ir::Lambda lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}};
            if (self.shape == 16) as.CallLambda(lambda, argument, argument, argument);
            else if (self.shape == 19) as.Cpuid(argument, value, ir::Imm{u64{0}}, ir::Imm{reinterpret_cast<u64>(&Helper)});
            else { ir::Params parameters; parameters.Push(argument); parameters.Push(argument); parameters.Push(argument);
                if (self.shape == 17) as.CallDynamic(lambda, parameters); else as.CallLocation(lambda, parameters); }
            break;
        }
        case 20: {
            Vector<ir::terminal::Switch::Case> cases;
            block.SetTerminal(ir::terminal::CheckHalt{ir::terminal::Condition{
                ir::Cond::EQ, ir::terminal::LinkBlock{ir::Location{pc + 4}}, ir::terminal::Switch{value, cases}}});
            return SingleDecodeResult::Ready;
        }
        default: break;
        }
        block.SetTerminal(ir::terminal::LinkBlock{ir::Location{pc + 4}}); return SingleDecodeResult::Ready;
    }
};
std::atomic<unsigned> native_calls{};
HaltReason NativeTripwire(backend::State*) { ++native_calls; return HaltReason::IllegalCode; }
} // namespace

TEST_CASE("Interpreter Runtime Step retires fresh arithmetic and complete conditional flags", "[runtime][interp-step][gate-e1]") {
    using namespace vixl::aarch64;
    for (bool wide : {false, true}) for (bool subtract : {false, true}) for (bool carry : {false, true}) {
        Fixture fixture; const u32 base = wide ? (subtract ? SBCS_x : ADCS_x) : (subtract ? SBCS_w : ADCS_w);
        fixture.provider.Word(pc, base | (2 << 16) | (1 << 5)); auto initial = Initial();
        initial.r[1] = u64{1} << (wide ? 63 : 31); initial.r[2] = 1; fixture.Seed(initial, other_flags | (u64(carry) << 29));
        auto expected = initial; const auto flags = Arithmetic(wide, subtract, initial.r[1], initial.r[2], carry, &expected.r[0]);
        REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, expected, other_flags | flags, pc + 4);
    }
    for (bool wide : {false, true}) for (bool subtract : {false, true}) for (bool immediate : {false, true}) for (bool taken : {false, true}) {
        Fixture fixture; const u32 base = immediate ? u32(wide ? (subtract ? CCMP_x_imm : CCMN_x_imm) : (subtract ? CCMP_w_imm : CCMN_w_imm))
                                                   : u32(wide ? (subtract ? CCMP_x : CCMN_x) : (subtract ? CCMP_w : CCMN_w));
        fixture.provider.Word(pc, base | ((immediate ? 3u : 2u) << 16) | (1u << 5) | 0xb);
        auto initial = Initial(); initial.r[1] = (u64{1} << (wide ? 63 : 31)) - 1; initial.r[2] = 1;
        fixture.Seed(initial, other_flags | (u64(taken) << 30));
        const auto flags = taken ? Arithmetic(wide, subtract, initial.r[1], immediate ? 3 : initial.r[2], subtract) : u64{0xb} << 28;
        REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, initial, other_flags | flags, pc + 4);
    }
    Fixture fresh; fresh.provider.Word(pc, 0x91000400); fresh.provider.Word(pc + 4, 0x91000800);
    auto expected = fresh.Guest(); ++expected.r[0]; REQUIRE(Step(fresh) == HaltReason::Step); Check(fresh, expected, other_flags, pc + 4);
    expected.r[0] += 2; REQUIRE(Step(fresh) == HaltReason::Step); Check(fresh, expected, other_flags, pc + 8);
    REQUIRE(fresh.provider.fetches.load() == 2);
}

TEST_CASE("Interpreter Runtime Step uses one architectural branch or PC relative result", "[runtime][interp-step][gate-e1]") {
    for (u32 word : {0x14000003u, 0x97fffffeu, 0xD61F03C0u, 0xD63F03C0u, 0xD65F03C0u}) {
        Fixture fixture; fixture.provider.Word(pc, word); auto initial = Initial(); initial.lr = 0x87654320; fixture.Seed(initial);
        auto expected = initial; if (word == 0x97fffffe || word == 0xD63F03C0) expected.lr = pc + 4;
        const u64 target = word == 0x14000003 ? pc + 12 : word == 0x97fffffe ? pc - 8 : initial.lr;
        REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, expected, other_flags, target);
    }
    for (bool page : {false, true}) for (s32 delta : {-3, 5}) {
        Fixture fixture; const u32 field = u32(delta) & 0x1fffff;
        fixture.provider.Word(pc, (page ? 0x90000000u : 0x10000000u) | ((field & 3) << 29) | ((field >> 2) << 5));
        auto expected = fixture.Guest(); expected.r[0] = page ? pc + s64(delta) * 4096 : pc + s64(delta);
        REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, expected, other_flags, pc + 4);
    }
}

TEST_CASE("Interpreter Runtime Step audited FP and SIMD helpers preserve full guest and host state", "[runtime][interp-step][gate-e1][fp]") {
    struct Case { u32 word; u64 lhs, rhs, answer; bool half; };
    for (auto test : {Case{0x1e222820, 0x3f800000, 0x40000000, 0x40400000, false},
                      Case{0x1ee22820, 0x3c00, 0x4000, 0x4200, true}}) { // exactly 1+2=3 in S/H
        Fixture fixture; fixture.provider.Word(pc, test.word); auto initial = Initial();
        initial.v[1] = test.lhs; initial.v[2] = test.rhs; fixture.Seed(initial);
        auto expected = initial; expected.v[0] = test.answer;
        REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, expected, other_flags, pc + 4);
    }
    Fixture vector; vector.provider.Word(pc, 0x4e228420); // ADD V0.16B,V1.16B,V2.16B
    auto initial = Initial(), expected = initial;
    std::array<u8, 16> left{}, right{}, answer{};
    for (unsigned i = 0; i < 16; ++i) { left[i] = u8(0xf0 + i); right[i] = u8(3 * i); answer[i] = u8(left[i] + right[i]); }
    std::memcpy(&initial.v[1], left.data(), 16); std::memcpy(&initial.v[2], right.data(), 16); expected = initial;
    std::memcpy(&expected.v[0], answer.data(), 16); vector.Seed(initial);
    REQUIRE(Step(vector) == HaltReason::Step); Check(vector, expected, other_flags, pc + 4);
}

TEST_CASE("Interpreter Runtime Step preserves synchronous trap categories", "[runtime][interp-step][gate-e1][exit]") {
    for (auto test : {std::pair{0xd4000001u, Trap::SVC}, std::pair{0xd4200000u, Trap::BRK},
                      std::pair{0xd4400000u, Trap::HLT}, std::pair{0u, Trap::ILL_CODE}}) {
        Fixture fixture; fixture.provider.Word(pc, test.first); auto expected = fixture.Guest(); expected.padding = u32(test.second);
        REQUIRE(Step(fixture) == HaltReason::CallHost); Check(fixture, expected, other_flags, pc + (test.second == Trap::SVC ? 4 : 0));
    }
    Fixture fault; fault.provider.status = CodeFetchResult::Fault; auto expected = fault.Guest(); expected.padding = u32(Trap::PAGE_FATAL);
    REQUIRE(Step(fault) == HaltReason::CallHost); Check(fault, expected, other_flags, pc);
    Fixture unaligned; unaligned.runtime.SetLocation(pc + 1); expected = unaligned.Guest(); expected.padding = u32(Trap::ILL_CODE);
    REQUIRE(Step(unaligned) == HaltReason::CallHost); Check(unaligned, expected, other_flags, pc + 1); REQUIRE(unaligned.provider.fetches.load() == 0);
}

TEST_CASE("Interpreter Core Step commits context PC and maps all exits", "[runtime][interp-step][gate-e1][core]") {
    CoreFixture fixture; REQUIRE_FALSE(fixture.instance->GetAddressSpace()->GetConfig().enable_jit);
    fixture.provider.Word(pc, 0x91000400); auto expected = fixture.core->GetContext(); ++expected.r[0]; expected.pc += 4;
    REQUIRE(fixture.core->Step() == Exit::Step); Equal(expected, fixture.core->GetContext());
    for (auto test : {std::pair{0xd4000001u, Trap::SVC}, std::pair{0xd4200000u, Trap::BRK},
                      std::pair{0xd4400000u, Trap::HLT}, std::pair{0u, Trap::ILL_CODE}}) {
        fixture.provider.Word(pc, test.first); fixture.core->GetContext() = Initial(); fixture.core->GetContext().r[8] = 173;
        expected = fixture.core->GetContext(); expected.padding = u32(test.second); if (test.second == Trap::SVC) expected.pc += 4;
        const auto syscall = fixture.core->GetSyscallNumber();
        REQUIRE(fixture.core->Step() == (test.second == Trap::SVC ? Exit::Syscall : Exit::IllegalCode)); Equal(expected, fixture.core->GetContext());
        REQUIRE(fixture.core->GetSyscallNumber() == (test.second == Trap::SVC ? 173 : syscall));
    }
    fixture.core->GetContext() = Initial(); expected = fixture.core->GetContext(); const auto count = fixture.provider.fetches.load();
    fixture.core->SignalInterrupt(); REQUIRE(fixture.core->Step() == Exit::Signal); Equal(expected, fixture.core->GetContext());
    REQUIRE(fixture.provider.fetches.load() == count); fixture.core->ClearInterrupt();
    fixture.provider.status = CodeFetchResult::Unsupported; REQUIRE(fixture.core->Step() == Exit::Unsupported); Equal(expected, fixture.core->GetContext());
    fixture.provider.status = CodeFetchResult::Fault; expected.padding = u32(Trap::PAGE_FATAL);
    REQUIRE(fixture.core->Step() == Exit::PageFatal); Equal(expected, fixture.core->GetContext());
}

TEST_CASE("Interpreter Core Step branch and audited FP commit complete context", "[runtime][interp-step][gate-e1][core]") {
    CoreFixture fixture;
    for (u32 word : {0xd63f03c0u, 0x90000020u, 0x1e222820u, 0x1ee22820u}) {
        fixture.provider.Word(pc, word); auto initial = Initial(); initial.lr = 0x81234560;
        const bool half = word == 0x1ee22820;
        initial.v[1] = half ? 0x3c00 : 0x3f800000; initial.v[2] = half ? 0x4000 : 0x40000000;
        fixture.core->GetContext() = initial; auto expected = initial; expected.pc += 4;
        if (word == 0xd63f03c0) { expected.lr = pc + 4; expected.pc = initial.lr; }
        else if (word == 0x90000020) expected.r[0] = pc + 4 * 4096; // ADRP #4 pages
        else expected.v[0] = half ? 0x4200 : 0x40400000;
        REQUIRE(fixture.core->Step() == Exit::Step); Equal(expected, fixture.core->GetContext());
        REQUIRE(fixture.provider.requests.back().first == pc); REQUIRE(fixture.provider.requests.back().second == 4);
    }
    fixture.provider.Word(pc, 0xc85f7c20); fixture.core->GetContext() = Initial(); const auto expected = fixture.core->GetContext();
    REQUIRE(fixture.core->Step() == Exit::Unsupported); Equal(expected, fixture.core->GetContext());
    REQUIRE(fixture.provider.reads.load() == 0); REQUIRE(fixture.provider.writes.load() == 0); REQUIRE(fixture.provider.pointers.load() == 0);
}

TEST_CASE("Interpreter Step pending or decode callback Signal starts no guest effects", "[runtime][interp-step][gate-e1][interrupt]") {
    for (u32 word : {0x91000400u, 0x1e222820u, 0x7a42002bu, 0xd4000001u}) {
        Fixture fixture; fixture.provider.Word(pc, word); const auto expected = fixture.Guest();
        fixture.runtime.SignalInterrupt(); const auto requested = Capture(fixture.runtime);
        REQUIRE(Step(fixture) == HaltReason::Signal); REQUIRE(Capture(fixture.runtime) == requested); REQUIRE(fixture.provider.fetches.load() == 0);
        fixture.runtime.ClearInterrupt(); fixture.provider.before_fetch = [&] { fixture.runtime.SignalInterrupt(); };
        REQUIRE(Step(fixture) == HaltReason::Signal); Check(fixture, expected, other_flags, pc); REQUIRE(fixture.provider.fetches.load() == 1);
        fixture.provider.before_fetch = {}; fixture.runtime.ClearInterrupt(); fixture.provider.Word(pc, 0x91000400);
        auto retired = expected; ++retired.r[0]; REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, retired, other_flags, pc + 4);
    }
    CoreFixture core; core.provider.Word(pc, 0x1e222820); const auto expected = core.core->GetContext();
    core.provider.before_fetch = [&] { core.core->SignalInterrupt(); };
    REQUIRE(core.core->Step() == Exit::Signal); Equal(expected, core.core->GetContext()); REQUIRE(core.provider.fetches.load() == 1);
    core.core->ClearInterrupt(); core.provider.before_fetch = {}; core.provider.Word(pc, 0x91000400);
    auto retired = expected; ++retired.r[0]; retired.pc += 4;
    REQUIRE(core.core->Step() == Exit::Step); Equal(retired, core.core->GetContext());
}

TEST_CASE("Interpreter Step rejects unsupported data or host tails before register prefix", "[runtime][interp-step][gate-e1][admission]") {
    for (unsigned shape = 0; shape < 21; ++shape) {
        CAPTURE(shape); Rejected rejected{.shape = shape}; Fixture fixture{{&Rejected::Decode, &rejected}};
        fixture.provider.memory.fill(0xa7); const auto before = Capture(fixture.runtime); const auto memory = fixture.provider.memory;
        REQUIRE(Step(fixture) == HaltReason::Unsupported); REQUIRE(Capture(fixture.runtime) == before); REQUIRE(fixture.provider.memory == memory);
        REQUIRE(rejected.calls == 1); REQUIRE(rejected.helpers == 0); fixture.NoPublication();
    }
    Fixture missing({}, true); const auto before = Capture(missing.runtime);
    REQUIRE(Step(missing) == HaltReason::Unsupported); REQUIRE(Capture(missing.runtime) == before); REQUIRE(missing.provider.fetches.load() == 0);
    Fixture unregistered({&Provider::Decode, nullptr, &Provider::Admit}, false); const auto absent = Capture(unregistered.runtime);
    REQUIRE(Step(unregistered) == HaltReason::Unsupported); REQUIRE(Capture(unregistered.runtime) == absent); REQUIRE(unregistered.provider.fetches.load() == 0);
    Fixture data; data.provider.Word(pc, 0xf9400020); const auto data_before = Capture(data.runtime);
    REQUIRE(Step(data) == HaltReason::Unsupported); REQUIRE(Capture(data.runtime) == data_before); REQUIRE(data.provider.fetches.load() == 1);
}

TEST_CASE("Interpreter Step never executes an existing ordinary native code pointer", "[runtime][interp-step][gate-e1][native]") {
    Fixture fixture; fixture.provider.Word(pc, 0x91000400);
    fixture.space.PushCodeCache(ir::Location{pc}, reinterpret_cast<void*>(&NativeTripwire));
    const auto calls = native_calls.load(); auto expected = fixture.Guest(); ++expected.r[0];
    REQUIRE(Step(fixture) == HaltReason::Step); Equal(expected, fixture.Guest()); REQUIRE(fixture.runtime.GetLocation() == pc + 4);
    REQUIRE(native_calls.load() == calls); REQUIRE(fixture.space.GetCodeCache(pc) == reinterpret_cast<void*>(&NativeTripwire));
    REQUIRE(backend::IsEmpty(fixture.space.GetDefaultModule()->GetNode(ir::Location{pc})));
}

TEST_CASE("Interpreter Step decode owner rejects recursive and concurrent entries", "[runtime][interp-step][gate-e1][ownership]") {
    Fixture fixture, other; fixture.provider.Word(pc, 0x91000400); fixture.provider.Word(pc + 4, 0x91000400);
    CoreFixture core; std::array<HaltReason, 4> rejected{}; std::array<Exit, 2> core_rejected{}; bool unchanged = false;
    fixture.provider.before_fetch = [&] {
        const auto before = Capture(fixture.runtime), other_before = Capture(other.runtime); const auto core_before = core.core->GetContext();
        rejected = {fixture.runtime.Step(), fixture.runtime.Run(), other.runtime.Step(), other.runtime.Run()};
        core_rejected = {core.core->Step(), core.core->Run()};
        unchanged = before == Capture(fixture.runtime) && other_before == Capture(other.runtime) && std::memcmp(&core_before, &core.core->GetContext(), sizeof(Context)) == 0;
    };
    REQUIRE(Step(fixture) == HaltReason::Step); REQUIRE(unchanged);
    for (auto reason : rejected) REQUIRE(reason == HaltReason::EntryRejected);
    for (auto reason : core_rejected) REQUIRE(reason == Exit::EntryRejected);
    REQUIRE(other.provider.fetches.load() == 0); REQUIRE(core.provider.fetches.load() == 0);
    std::atomic_bool entered{}, done{}, timed_out{}; bool owner_unchanged = false;
    fixture.provider.before_fetch = [&] {
        const auto before = Capture(fixture.runtime); entered.store(true, std::memory_order_release);
        if (!Poll([&] { return done.load(std::memory_order_acquire); })) timed_out.store(true);
        owner_unchanged = before == Capture(fixture.runtime);
    };
    std::jthread contender([&](std::stop_token stop) {
        if (!Poll([&] { return entered.load(std::memory_order_acquire) || stop.stop_requested(); })) timed_out.store(true);
        if (entered.load(std::memory_order_acquire)) { rejected[0] = fixture.runtime.Step(); rejected[1] = fixture.runtime.Run(); }
        done.store(true, std::memory_order_release);
    });
    const auto reason = Step(fixture); contender.request_stop(); contender.join();
    REQUIRE(reason == HaltReason::Step); REQUIRE(owner_unchanged); REQUIRE_FALSE(timed_out.load());
    REQUIRE(rejected[0] == HaltReason::EntryRejected); REQUIRE(rejected[1] == HaltReason::EntryRejected);
}

TEST_CASE("Interpreter Core Step concurrent decode rejection preserves owner context", "[runtime][interp-step][gate-e1][core][ownership]") {
    CoreFixture fixture; fixture.provider.Word(pc, 0x91000400);
    std::atomic_bool entered{}, done{}, timed_out{}; bool unchanged = false;
    std::array<Exit, 2> rejected{};
    fixture.provider.before_fetch = [&] {
        const auto before = fixture.core->GetContext(); const auto syscall = fixture.core->GetSyscallNumber();
        entered.store(true, std::memory_order_release);
        if (!Poll([&] { return done.load(std::memory_order_acquire); })) timed_out.store(true);
        unchanged = std::memcmp(&before, &fixture.core->GetContext(), sizeof(Context)) == 0 && syscall == fixture.core->GetSyscallNumber();
    };
    std::jthread contender([&](std::stop_token stop) {
        if (!Poll([&] { return entered.load(std::memory_order_acquire) || stop.stop_requested(); })) timed_out.store(true);
        if (entered.load(std::memory_order_acquire)) rejected = {fixture.core->Step(), fixture.core->Run()};
        done.store(true, std::memory_order_release);
    });
    auto expected = fixture.core->GetContext(); ++expected.r[0]; expected.pc += 4;
    const auto reason = fixture.core->Step(); contender.request_stop(); contender.join();
    REQUIRE(reason == Exit::Step); Equal(expected, fixture.core->GetContext()); REQUIRE(unchanged); REQUIRE_FALSE(timed_out.load());
    for (auto result : rejected) REQUIRE(result == Exit::EntryRejected);
    REQUIRE(fixture.provider.fetches.load() == 1); REQUIRE_FALSE(backend::RuntimeExecutionActiveOnCurrentThread());
}
