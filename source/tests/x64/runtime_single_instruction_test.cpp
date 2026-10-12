#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/address_space.h"
#include "runtime/backend/execution_owner.h"
#include "runtime/backend/guest_memory_scope.h"
#include "runtime/backend/interrupt_request.h"
#include "runtime/backend/riscv64/memory_protocol.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/arm64/arm64_frontend.h"
#include "translator/arm64/translator.h"

using namespace swift;
using namespace swift::runtime;
namespace a64 = swift::translator::arm64;
namespace {
using Context = swift::arm64::ThreadContext64;
using Reason = swift::arm64::InterruptReason;
using Exit = swift::translator::ExitReason;
constexpr u64 code_pc = 0x1000, flags_other = 0xA5A5000002345678;

struct Mapping {
    size_t size = 0x10000;
    u8* bytes = static_cast<u8*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    Mapping() { if (bytes == MAP_FAILED) throw std::runtime_error("test mmap failed"); }
    ~Mapping() { munmap(bytes, size); }
    void Word(u64 address, u32 word) { std::memcpy(bytes + address, &word, 4); }
    void Protect(u64 address, int protection) {
        REQUIRE(mprotect(bytes + address, size_t(getpagesize()), protection) == 0);
    }
};

struct Provider final : MemoryInterface {
    Mapping& mapping;
    std::atomic<u32> fetches{}, data_reads{}, pointers{};
    std::vector<std::pair<u64, size_t>> requests;
    CodeFetchResult status{CodeFetchResult::Success};
    std::function<void()> before_fetch;
    explicit Provider(Mapping& mapping) : mapping(mapping) {}
    CodeFetchResult ReadCode(void* destination, size_t address, size_t size) override {
        ++fetches; requests.emplace_back(address, size);
        if (before_fetch) before_fetch();
        if (status != CodeFetchResult::Success) return status;
        if (size != 4 || address > mapping.size - size) return CodeFetchResult::Fault;
        std::memcpy(destination, mapping.bytes + address, size); return CodeFetchResult::Success;
    }
    bool MatchesCodeAddressSpace(uintptr_t bias, u64 mask) const override {
        return bias == reinterpret_cast<uintptr_t>(mapping.bytes) && mask == mapping.size - 1;
    }
    bool Read(void*, size_t, size_t) override { ++data_reads; return false; }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void* address) override {
        ++pointers; const auto guest = reinterpret_cast<u64>(address);
        return guest < mapping.size ? mapping.bytes + guest : nullptr;
    }
    static SingleDecodeResult Decode(const void*, LocationDescriptor address, MemoryInterface& memory, ir::Block& block) {
        ir::Assembler assembler{&block}; swift::arm64::A64Decoder decoder{address, &memory, &assembler};
        const auto result = decoder.DecodeSingle();
        return result == CodeFetchResult::Success ? SingleDecodeResult::Ready :
               result == CodeFetchResult::Fault ? SingleDecodeResult::FaultIR : SingleDecodeResult::Unsupported;
    }
};

Config Configuration(Provider& provider, bool cache, bool native = true, SingleDecodeService service = {}) {
    return {.loc_start = 0, .loc_end = provider.mapping.size, .enable_jit = native,
            .backend_isa = kX86_64, .uniform_buffer_size = sizeof(Context), .static_program = true,
            .global_opts = cache ? Optimizations::ScalarRegisterCache : Optimizations::None,
            .stack_alignment = 16, .memory_base = provider.mapping.bytes,
            .guest_addr_mask = provider.mapping.size - 1, .memory = &provider,
            .single_decode = service.callback ? service : SingleDecodeService{&Provider::Decode, nullptr}};
}

Context Initial(u64 pc = code_pc) {
    Context context{};
    for (u32 index = 0; index < context.r.size(); ++index) context.r[index] = 0x8123456789ABCDEFULL ^ (u64(index) * 0x0101010101010101ULL);
    context.fp = 0xFEDCBA9876543210ULL; context.lr = 0x9988776655443322ULL; context.sp = 0x1234567887654321ULL;
    context.pc = pc; context.pstate = 0x13579BDF; context.padding = 0x2468ACE0; context.fpcr = 0; context.fpsr = 0x08000015;
    context.tpidr = 0x1020304050607080ULL;
    for (u32 index = 0; index < 32; ++index) {
        const std::array<u64, 2> lanes{0x1122334455667788ULL + index, 0x8877665544332211ULL - index};
        std::memcpy(&context.v[index], lanes.data(), sizeof(lanes));
    }
    return context;
}

struct Fixture {
    Mapping mapping;
    Provider provider{mapping};
    backend::AddressSpace space;
    Runtime runtime;
    explicit Fixture(bool cache = false, bool native = true, SingleDecodeService service = {})
        : space(Configuration(provider, cache, native, service)), runtime(&space) {
        Seed(Initial());
    }
    Context& Guest() { return *reinterpret_cast<Context*>(runtime.GetUniformBuffer().data()); }
    void Seed(const Context& context, u64 flags = flags_other) {
        Guest() = context; runtime.GetState()->host_cpu_flags = flags; runtime.SetLocation(context.pc);
    }
    void NoPublication(u64 address = code_pc) {
        REQUIRE(backend::IsEmpty(space.GetDefaultModule()->GetNode(ir::Location{address})));
        REQUIRE(space.GetCodeCache(address) == nullptr);
        REQUIRE(space.GetCallCodeCacheTable().Lookup(address) == 0);
        REQUIRE(space.GetPendingCallCodeCacheTable().Lookup(address) == 0);
    }
};

// ABI wrapper around the public C++ Runtime call; none of the six tested
// nonvolatiles carry output pointers. Real native fault recovery returns here.
constexpr std::array<u64, 6> abi_values{0x1111222233334444ULL, 0x2222333344445555ULL, 0x3333444455556666ULL,
                                      0x4444555566667777ULL, 0x5555666677778888ULL, 0x6666777788889999ULL};
struct AbiResult { std::array<u64, 6> registers{}; u64 before{}, after{}, flags{}; u32 reason{}; };
HaltReason StepCall(Runtime* runtime) { return runtime->Step(); }
class AbiCaller final : public Xbyak::CodeGenerator {
public:
    AbiCaller() {
        push(rbx); push(rbp); push(r12); push(r13); push(r14); push(r15); sub(rsp, 40);
        mov(qword[rsp], rdi); mov(qword[rsp + 8], rsi); mov(qword[rsi + offsetof(AbiResult, before)], rsp);
        const Xbyak::Reg64 registers[]{rbx, rbp, r12, r13, r14, r15};
        for (u32 index = 0; index < 6; ++index) mov(registers[index], abi_values[index]);
        mov(rdi, qword[rsp]); mov(rax, reinterpret_cast<u64>(&StepCall)); call(rax);
        mov(rcx, qword[rsp + 8]); mov(dword[rcx + offsetof(AbiResult, reason)], eax);
        for (u32 index = 0; index < 6; ++index) mov(qword[rcx + index * 8], registers[index]);
        mov(qword[rcx + offsetof(AbiResult, after)], rsp); pushfq(); pop(rax); mov(qword[rcx + offsetof(AbiResult, flags)], rax);
        add(rsp, 40); pop(r15); pop(r14); pop(r13); pop(r12); pop(rbp); pop(rbx); ret(); ready();
    }
    HaltReason Run(Runtime& runtime, AbiResult& result) {
        getCode<void (*)(Runtime*, AbiResult*)>()(&runtime, &result); return static_cast<HaltReason>(result.reason);
    }
};
struct HostFP {
    u32 original = _mm_getcsr(); u32 desired = (original & ~0x6000u) | 0x4000u | 0x1F80u | 0x15u;
    HostFP() { _mm_setcsr(desired); } ~HostFP() { _mm_setcsr(original); }
};
HaltReason Step(Fixture& fixture) {
    const auto readers = backend::riscv64::anonymous_memory_readers.load();
    const auto kernels = backend::riscv64::memory_kernel_depth.load();
    const auto mapping = backend::GuestMemoryScope::Current();
    AbiCaller caller; AbiResult result; HaltReason reason;
    { HostFP fp; reason = caller.Run(fixture.runtime, result); REQUIRE(_mm_getcsr() == fp.desired); }
    REQUIRE(result.registers == abi_values); REQUIRE(result.before == result.after); REQUIRE((result.flags & (1u << 10)) == 0);
    REQUIRE(fixture.runtime.GetState()->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
    REQUIRE(fixture.runtime.GetState()->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
    REQUIRE(fixture.runtime.GetState()->spill_area[backend::kRiscvMemoryOwnedSlot] == 0);
    REQUIRE(backend::riscv64::anonymous_memory_readers.load() == readers);
    REQUIRE(backend::riscv64::memory_kernel_depth.load() == kernels);
    REQUIRE_FALSE(backend::RuntimeExecutionActiveOnCurrentThread());
    REQUIRE(backend::GuestMemoryScope::Current().bias == mapping.bias);
    REQUIRE(backend::GuestMemoryScope::Current().mask == mapping.mask);
    REQUIRE(fixture.provider.data_reads.load() == 0); REQUIRE(fixture.provider.pointers.load() == 0);
    return reason;
}
void Equal(const Context& expected, const Context& actual) { REQUIRE(std::memcmp(&expected, &actual, sizeof(expected)) == 0); }
void Check(Fixture& fixture, const Context& expected, u64 flags, u64 pc) {
    Equal(expected, fixture.Guest()); REQUIRE(fixture.runtime.GetState()->host_cpu_flags == flags);
    REQUIRE(fixture.runtime.GetLocation() == pc); fixture.NoPublication();
    REQUIRE(fixture.provider.requests.back().second == 4);
}

u64 ArithmeticFlags(bool wide, bool subtract, u64 lhs, u64 rhs, bool carry, u64* output = nullptr) {
    const unsigned width = wide ? 64 : 32; const u64 mask = wide ? UINT64_MAX : UINT32_MAX;
    lhs &= mask; rhs &= mask; const unsigned __int128 a = lhs, b = rhs, extra = subtract ? !carry : carry;
    const auto full = subtract ? a - b - extra : a + b + extra; const u64 value = u64(full) & mask;
    const auto signed_value = [=](u64 input) -> __int128 { return input & (u64{1} << (width - 1)) ? __int128(input) - (__int128{1} << width) : __int128(input); };
    const auto mathematical = subtract ? signed_value(lhs) - signed_value(rhs) - __int128(extra) : signed_value(lhs) + signed_value(rhs) + __int128(extra);
    if (output) *output = value;
    return (u64(value >> (width - 1)) << 31) | (u64(value == 0) << 30) |
           (u64(subtract ? a >= b + extra : (full >> width) != 0) << 29) |
           (u64(mathematical < -(__int128{1} << (width - 1)) || mathematical >= (__int128{1} << (width - 1))) << 28);
}

using ArmInstance = std::unique_ptr<a64::Arm64Instance, decltype(&a64::Arm64Instance::Destroy)>;
using ArmCore = std::unique_ptr<a64::Arm64Core, decltype(&a64::Arm64Core::Destroy)>;
struct CoreFixture {
    Mapping mapping; Provider provider{mapping};
    ArmInstance instance{a64::Arm64Instance::Make(mapping.bytes, mapping.size - 1, &provider), &a64::Arm64Instance::Destroy};
    ArmCore core{a64::Arm64Core::Make(instance.get()), &a64::Arm64Core::Destroy};
    CoreFixture() { core->GetContext() = Initial(); }
};

struct Rendezvous {
    Runtime* runtime{}; backend::State* state{};
    std::atomic_bool entered{}, release{}, timeout{};
    std::atomic<u32> calls{};
    bool reject{}; void* bad_pointer{};
    u64 recovery{};
    static bool Range(void* pointer, u64, u64) {
        auto& self = *static_cast<Rendezvous*>(pointer); ++self.calls;
        if (self.bad_pointer) *static_cast<volatile u8*>(self.bad_pointer) = 7;
        self.recovery = self.state->spill_area[backend::kRiscvRecoveryPcSlot];
        self.entered.store(true, std::memory_order_release);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!self.release.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) { self.timeout.store(true); break; }
            std::this_thread::yield();
        }
        return !self.reject;
    }
};
template<class Predicate> bool Poll(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!predicate()) { if (std::chrono::steady_clock::now() > deadline) return false; std::this_thread::yield(); }
    return true;
}

struct RejectedUnit {
    u32 shape{}; unsigned decodes{}, helper_calls{};
    static u64 Helper(u64 pointer, u64, u64) {
        ++reinterpret_cast<RejectedUnit*>(pointer)->helper_calls; return 7;
    }
    static SingleDecodeResult Decode(const void* pointer, LocationDescriptor,
                                     MemoryInterface&, ir::Block& block) {
        auto& self = *static_cast<RejectedUnit*>(const_cast<void*>(pointer)); ++self.decodes;
        using T = ir::ValueType;
        ir::Assembler as{&block};
        auto value = as.LoadImm(ir::Imm{u64{0x1234}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{u32(offsetof(Context, r)), T::U64}, value);
        auto address = as.LoadImm(ir::Imm{u64{0x8000}}).SetType(T::U64);
        as.StoreMemory(ir::Operand{address}, value);
        if (self.shape == 0) {
            // Valid effects preceding a constructor-invalid multi-word marker.
            as.AdvancePC(ir::Imm{u32{4}});
        } else if (self.shape == 1) {
            // Currently emitted memory-copy code has no precise guest sites.
            as.MemoryCopy(ir::Lambda{ir::Imm{u64{0x8008}}}, ir::Lambda{ir::Imm{u64{0x8000}}}, ir::Imm{u32{8}});
        } else if (self.shape >= 2 && self.shape <= 4) {
            auto argument = as.LoadImm(ir::Imm{reinterpret_cast<u64>(&self)}).SetType(T::U64);
            ir::Lambda lambda{ir::Imm{reinterpret_cast<u64>(&Helper)}};
            if (self.shape == 2) as.CallLambda(lambda, argument, argument, argument);
            else {
                ir::Params params; params.Push(argument); params.Push(argument); params.Push(argument);
                if (self.shape == 3) as.CallDynamic(lambda, params);
                else as.CallLocation(lambda, params);
            }
        } else if (self.shape == 5) {
            auto jump = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
            as.BindLabel(jump);
            auto* label = &block.GetInstList().back();
            block.RemoveInst(label); block.InsertBefore(label, jump.Def()); block.ReIdInstr();
        } else if (self.shape == 6) {
            // Cpuid also carries an arbitrary host target despite its opcode.
            auto argument = as.LoadImm(ir::Imm{reinterpret_cast<u64>(&self)}).SetType(T::U64);
            as.Cpuid(argument, value, ir::Imm{u64{0}}, ir::Imm{reinterpret_cast<u64>(&Helper)});
        } else {
            return SingleDecodeResult::Ready; // Missing terminal also rejects the complete unit.
        }
        block.SetTerminal(ir::terminal::LinkBlock{ir::Location{code_pc + 4}});
        return SingleDecodeResult::Ready;
    }
};
} // namespace

TEST_CASE("Runtime Step decodes fresh words without publishing cached instruction entries", "[x64][runtime-step][gate-d]") {
    for (bool cache : {false, true}) {
        Fixture fixture{cache}; fixture.mapping.Word(code_pc, 0x91000400); fixture.mapping.Word(code_pc + 4, 0x91000800);
        auto expected = fixture.Guest(); ++expected.r[0];
        REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, expected, flags_other, code_pc + 4);
        expected.r[0] += 2;
        REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, expected, flags_other, code_pc + 8);
        REQUIRE(fixture.provider.fetches.load() == 2);
        REQUIRE(fixture.provider.requests[0].first == code_pc); REQUIRE(fixture.provider.requests[1].first == code_pc + 4);
    }
}

TEST_CASE("Runtime Step arithmetic and conditional compare retire complete NZCV", "[x64][runtime-step][gate-d]") {
    using namespace vixl::aarch64;
    for (bool cache : {false, true}) for (bool wide : {false, true}) for (bool subtract : {false, true}) {
        for (bool carry : {false, true}) {
            Fixture fixture{cache}; const u32 word = u32(wide ? (subtract ? SBCS_x : ADCS_x) : (subtract ? SBCS_w : ADCS_w)) | (2u << 16) | (1u << 5);
            fixture.mapping.Word(code_pc, word); auto initial = Initial(); initial.r[1] = u64{1} << (wide ? 63 : 31); initial.r[2] = 1;
            fixture.Seed(initial, flags_other | (u64(carry) << 29)); auto expected = initial;
            const u64 flags = ArithmeticFlags(wide, subtract, initial.r[1], initial.r[2], carry, &expected.r[0]);
            REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, expected, flags_other | flags, code_pc + 4);
        }
        for (bool immediate : {false, true}) for (bool condition : {false, true}) {
            Fixture fixture{cache}; const u32 base = immediate ? u32(wide ? (subtract ? CCMP_x_imm : CCMN_x_imm) : (subtract ? CCMP_w_imm : CCMN_w_imm))
                                                           : u32(wide ? (subtract ? CCMP_x : CCMN_x) : (subtract ? CCMP_w : CCMN_w));
            fixture.mapping.Word(code_pc, base | ((immediate ? 3u : 2u) << 16) | (1u << 5) | 0xBu);
            auto initial = Initial(); initial.r[1] = (u64{1} << (wide ? 63 : 31)) - 1; initial.r[2] = 1;
            fixture.Seed(initial, flags_other | (u64(condition) << 30));
            const u64 flags = condition ? ArithmeticFlags(wide, subtract, initial.r[1], immediate ? 3 : initial.r[2], subtract) : u64{0xB} << 28;
            REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, initial, flags_other | flags, code_pc + 4);
        }
    }
}

TEST_CASE("Runtime Step uses architectural branch targets and PC relative addresses", "[x64][runtime-step][gate-d]") {
    for (bool cache : {false, true}) for (u32 word : {0x14000003u, 0x97FFFFFEu, 0xD61F03C0u, 0xD63F03C0u, 0xD65F03C0u}) {
        Fixture fixture{cache}; fixture.mapping.Word(code_pc, word); auto initial = Initial(); initial.lr = 0x87654320; fixture.Seed(initial);
        auto expected = initial; if (word == 0x97FFFFFE || word == 0xD63F03C0) expected.lr = code_pc + 4;
        const u64 target = word == 0x14000003 ? code_pc + 12 : word == 0x97FFFFFE ? code_pc - 8 : initial.lr;
        REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, expected, flags_other, target);
    }
    for (bool cache : {false, true}) for (bool page : {false, true}) for (s32 offset : {-3, 5}) {
        Fixture fixture{cache}; const u32 field = u32(offset) & 0x1FFFFF;
        fixture.mapping.Word(code_pc, (page ? u32(vixl::aarch64::ADRP) : u32(vixl::aarch64::ADR)) | ((field & 3) << 29) | ((field >> 2) << 5));
        auto expected = fixture.Guest(); expected.r[0] = page ? code_pc + s64(offset) * 4096 : code_pc + s64(offset);
        REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, expected, flags_other, code_pc + 4);
    }
}

TEST_CASE("Runtime Step entry refusals do not fetch or mutate architectural state", "[x64][runtime-step][gate-d]") {
    for (bool cache : {false, true}) {
        Fixture fixture{cache}; fixture.mapping.Word(code_pc, 0x91000400); auto expected = fixture.Guest();
        fixture.runtime.SignalInterrupt(); REQUIRE(Step(fixture) == HaltReason::Signal); REQUIRE(Step(fixture) == HaltReason::Signal);
        Equal(expected, fixture.Guest()); REQUIRE(fixture.runtime.GetLocation() == code_pc); REQUIRE(fixture.provider.fetches.load() == 0);
        fixture.runtime.ClearInterrupt(); REQUIRE(Step(fixture) == HaltReason::Step); ++expected.r[0]; Check(fixture, expected, flags_other, code_pc + 4);
        Fixture interpreted{cache, false}; interpreted.mapping.Word(code_pc, 0x91000400); auto retired = interpreted.Guest();
        REQUIRE(Step(interpreted) == HaltReason::Step); ++retired.r[0];
        Check(interpreted, retired, flags_other, code_pc + 4); REQUIRE(interpreted.provider.fetches.load() == 1);
        Fixture unsupported{cache}; unsupported.provider.status = CodeFetchResult::Unsupported; const auto unchanged = unsupported.Guest();
        REQUIRE(Step(unsupported) == HaltReason::Unsupported); Equal(unchanged, unsupported.Guest()); REQUIRE(unsupported.runtime.GetLocation() == code_pc);
        unsupported.NoPublication(); REQUIRE(unsupported.provider.fetches.load() == 1);
    }
}

TEST_CASE("Runtime Step fault IR and host traps retain frontend distinctions", "[x64][runtime-step][gate-d]") {
    struct Trap { u32 word; Reason reason; bool next; };
    for (bool cache : {false, true}) for (auto trap : {Trap{0xD4000001, Reason::SVC, true}, Trap{0xD4200000, Reason::BRK, false},
                                                      Trap{0xD4400000, Reason::HLT, false}, Trap{0, Reason::ILL_CODE, false}}) {
        Fixture fixture{cache}; fixture.mapping.Word(code_pc, trap.word); auto expected = fixture.Guest(); expected.padding = u32(trap.reason);
        REQUIRE(Step(fixture) == HaltReason::CallHost); Check(fixture, expected, flags_other, code_pc + (trap.next ? 4 : 0));
    }
    for (bool cache : {false, true}) {
        Fixture fixture{cache}; fixture.provider.status = CodeFetchResult::Fault; auto expected = fixture.Guest(); expected.padding = u32(Reason::PAGE_FATAL);
        REQUIRE(Step(fixture) == HaltReason::CallHost); Check(fixture, expected, flags_other, code_pc);
        Fixture unaligned{cache}; unaligned.runtime.SetLocation(code_pc + 1); expected = unaligned.Guest(); expected.padding = u32(Reason::ILL_CODE);
        REQUIRE(Step(unaligned) == HaltReason::CallHost); Equal(expected, unaligned.Guest()); REQUIRE(unaligned.runtime.GetLocation() == code_pc + 1);
        REQUIRE(unaligned.provider.fetches.load() == 0);
    }
}

TEST_CASE("Runtime Step rejects invalid or unaudited tails before any prefix effect", "[x64][runtime-step][gate-d][admission]") {
    for (bool cache : {false, true}) for (u32 shape = 0; shape < 8; ++shape) {
        CAPTURE(cache, shape);
        RejectedUnit unit{.shape = shape}; Fixture fixture{cache, true, {&RejectedUnit::Decode, &unit}};
        std::array<u8, 16> memory; memory.fill(0xA7); std::memcpy(fixture.mapping.bytes + 0x8000, memory.data(), memory.size());
        std::vector<u8> state(sizeof(backend::State) + sizeof(Context));
        std::memcpy(state.data(), fixture.runtime.GetState(), state.size());
        REQUIRE(Step(fixture) == HaltReason::Unsupported);
        REQUIRE(std::memcmp(state.data(), fixture.runtime.GetState(), state.size()) == 0);
        REQUIRE(std::memcmp(memory.data(), fixture.mapping.bytes + 0x8000, memory.size()) == 0);
        REQUIRE(unit.decodes == 1); REQUIRE(unit.helper_calls == 0);
        REQUIRE(fixture.provider.fetches.load() == 0); fixture.NoPublication();
    }
}

TEST_CASE("ARM64 Core Step commits PC and maps syscall and non syscall exits", "[x64][runtime-step][gate-d][core]") {
    CoreFixture fixture; REQUIRE(fixture.instance->GetAddressSpace()->GetConfig().enable_jit);
    fixture.mapping.Word(code_pc, 0x91000400); fixture.mapping.Word(code_pc + 4, 0x91000800);
    auto expected = fixture.core->GetContext(); ++expected.r[0]; expected.pc = code_pc + 4;
    REQUIRE(fixture.core->Step() == Exit::Step); Equal(expected, fixture.core->GetContext());
    expected.r[0] += 2; expected.pc += 4; REQUIRE(fixture.core->Step() == Exit::Step); Equal(expected, fixture.core->GetContext());
    REQUIRE(fixture.provider.fetches.load() == 2); REQUIRE(fixture.provider.pointers.load() == 0);
    struct Trap { u32 word; Reason reason; Exit exit; bool next; };
    for (auto trap : {Trap{0xD4000001, Reason::SVC, Exit::Syscall, true}, Trap{0xD4200000, Reason::BRK, Exit::IllegalCode, false},
                      Trap{0xD4400000, Reason::HLT, Exit::IllegalCode, false}, Trap{0, Reason::ILL_CODE, Exit::IllegalCode, false}}) {
        fixture.mapping.Word(code_pc, trap.word); fixture.core->GetContext() = Initial(); fixture.core->GetContext().r[8] = 173;
        expected = fixture.core->GetContext(); expected.padding = u32(trap.reason); expected.pc += trap.next ? 4 : 0;
        const auto previous_syscall = fixture.core->GetSyscallNumber(); REQUIRE(fixture.core->Step() == trap.exit);
        Equal(expected, fixture.core->GetContext()); REQUIRE(fixture.core->GetSyscallNumber() == (trap.exit == Exit::Syscall ? 173 : previous_syscall));
    }
    fixture.core->GetContext() = Initial(); expected = fixture.core->GetContext(); const auto count = fixture.provider.fetches.load();
    fixture.core->SignalInterrupt(); REQUIRE(fixture.core->Step() == Exit::Signal); Equal(expected, fixture.core->GetContext());
    REQUIRE(fixture.provider.fetches.load() == count); fixture.core->ClearInterrupt();
    fixture.provider.status = CodeFetchResult::Unsupported; REQUIRE(fixture.core->Step() == Exit::Unsupported); Equal(expected, fixture.core->GetContext());
    fixture.provider.status = CodeFetchResult::Fault; expected.padding = u32(Reason::PAGE_FATAL);
    REQUIRE(fixture.core->Step() == Exit::PageFatal); Equal(expected, fixture.core->GetContext());
}

TEST_CASE("ARM64 Core Step refuses static frontend fallback before committing effects", "[x64][runtime-step][gate-d][core][admission]") {
    CoreFixture fixture; fixture.mapping.Word(code_pc, 0xC85F7C20); // LDXR X0,[X1], presently unsupported.
    fixture.mapping.Word(code_pc + 4, 0xD2800034); // successor must not execute
    const auto expected = fixture.core->GetContext(); const auto syscall = fixture.core->GetSyscallNumber();
    REQUIRE(fixture.core->Step() == Exit::Unsupported); Equal(expected, fixture.core->GetContext());
    REQUIRE(fixture.core->GetSyscallNumber() == syscall); REQUIRE(fixture.provider.fetches.load() == 1);
    REQUIRE(fixture.provider.requests.back().first == code_pc); REQUIRE(fixture.provider.requests.back().second == 4);
    REQUIRE(fixture.provider.data_reads.load() == 0); REQUIRE(fixture.provider.pointers.load() == 0);
    auto& space = *fixture.instance->GetAddressSpace();
    REQUIRE(backend::IsEmpty(space.GetDefaultModule()->GetNode(ir::Location{code_pc})));
    REQUIRE(space.GetCodeCache(code_pc) == nullptr);
}

TEST_CASE("Runtime Step native data faults recover exact PC and suppress load store writeback", "[x64][runtime-step][gate-d][fault]") {
    for (bool cache : {false, true}) for (u32 shape = 0; shape < 4; ++shape) for (u32 protection = 0; protection < 3; ++protection) {
        const bool store = shape & 1, writeback = shape >= 2;
        if (protection == 1 && !store) continue; // Read-only reads are valid.
        Fixture fixture{cache}; const u64 page = getpagesize(), guarded = page * 3;
        const u32 word = writeback ? (store ? 0xF8008C20u : 0xF8408C20u) : (store ? 0xF9000020u : 0xF9400020u);
        fixture.mapping.Word(code_pc, word); fixture.mapping.Word(code_pc + 4, 0xD2800034); // sentinel successor
        std::array<u8, 16> before; before.fill(0xA7); std::memcpy(fixture.mapping.bytes + guarded - 16, before.data(), before.size());
        fixture.mapping.Protect(guarded, protection == 1 ? PROT_READ : PROT_NONE);
        auto initial = Initial(); const u64 access = protection == 2 ? guarded - 4 : guarded;
        initial.r[1] = access - (writeback ? 8 : 0); fixture.Seed(initial);
        REQUIRE(Step(fixture) == HaltReason::PageFatal); Check(fixture, initial, flags_other, code_pc);
        std::array<u8, 16> after; std::memcpy(after.data(), fixture.mapping.bytes + guarded - 16, after.size());
        REQUIRE(after == before); // A single x86 scalar store faults before committing its bytes.
        fixture.mapping.Protect(guarded, PROT_READ | PROT_WRITE);
        fixture.mapping.Word(code_pc, 0x91000400); fixture.runtime.SetLocation(code_pc); auto expected = initial; ++expected.r[0];
        REQUIRE(Step(fixture) == HaltReason::Step); Check(fixture, expected, flags_other, code_pc + 4);
    }
}

TEST_CASE("Runtime Step rendezvous Signal waits for complete memory and synchronous range faults win", "[x64][runtime-step][gate-d][interrupt]") {
    for (bool cache : {false, true}) for (bool store : {false, true}) for (u32 fault = 0; fault < 3; ++fault) {
        const bool reject = fault == 1, physical_fault = fault == 2, fails = fault != 0;
        Fixture fixture{cache}; fixture.mapping.Word(code_pc, store ? 0xF9000020 : 0xF9400020);
        const u64 address = 0x8000, old = 0xFEDCBA9876543210ULL; std::memcpy(fixture.mapping.bytes + address, &old, 8);
        auto initial = Initial(); initial.r[1] = address; fixture.Seed(initial); auto expected = initial;
        if (!fails && !store) expected.r[0] = old;
        if (physical_fault) fixture.mapping.Protect(address, PROT_NONE);
        Rendezvous request{.runtime = &fixture.runtime, .state = fixture.runtime.GetState(), .reject = reject};
        fixture.runtime.GetState()->interp_range_check = &Rendezvous::Range; fixture.runtime.GetState()->interp_range_check_ctx = &request;
        std::jthread requester([&](std::stop_token stop) {
            if (!Poll([&] { return request.entered.load(std::memory_order_acquire) || stop.stop_requested(); })) request.timeout.store(true);
            if (request.entered.load(std::memory_order_acquire)) fixture.runtime.SignalInterrupt();
            request.release.store(true, std::memory_order_release);
        });
        const auto reason = Step(fixture); requester.request_stop(); requester.join();
        REQUIRE(reason == (fails ? HaltReason::PageFatal : HaltReason::Signal)); Check(fixture, expected, flags_other, fails ? code_pc : code_pc + 4);
        REQUIRE_FALSE(request.timeout.load()); REQUIRE(request.calls.load() == 1); REQUIRE(request.recovery != 0);
        if (physical_fault) fixture.mapping.Protect(address, PROT_READ | PROT_WRITE);
        u64 actual; std::memcpy(&actual, fixture.mapping.bytes + address, 8); REQUIRE(actual == (!fails && store ? initial.r[0] : old));
        fixture.runtime.GetState()->interp_range_check = nullptr; fixture.runtime.ClearInterrupt(); fixture.runtime.SetLocation(code_pc);
        REQUIRE(Step(fixture) == HaltReason::Step);
    }
}

TEST_CASE("Runtime Step rejects recursive and concurrent entry before touching owner state", "[x64][runtime-step][gate-d][ownership]") {
    Fixture fixture; fixture.mapping.Word(code_pc, 0x91000400); Fixture other; other.mapping.Word(code_pc, 0xD503201F);
    std::array<HaltReason, 4> reasons{}; bool same = false;
    fixture.provider.before_fetch = [&] {
        const auto before = fixture.Guest(), other_before = other.Guest(); const auto location = fixture.runtime.GetLocation();
        reasons = {fixture.runtime.Step(), fixture.runtime.Run(), other.runtime.Step(), other.runtime.Run()};
        same = std::memcmp(&before, &fixture.Guest(), sizeof(before)) == 0 &&
               std::memcmp(&other_before, &other.Guest(), sizeof(before)) == 0 && fixture.runtime.GetLocation() == location;
    };
    REQUIRE(Step(fixture) == HaltReason::Step); for (auto reason : reasons) REQUIRE(reason == HaltReason::EntryRejected);
    REQUIRE(same); REQUIRE(other.provider.fetches.load() == 0); REQUIRE(fixture.provider.fetches.load() == 1);
    fixture.provider.before_fetch = {}; fixture.mapping.Word(code_pc + 4, 0x91000400);
    std::atomic_bool entered{}, checked{}, release{}, timed_out{}; bool owner_unchanged = false;
    fixture.provider.before_fetch = [&] {
        const auto before = fixture.Guest(); const auto location = fixture.runtime.GetLocation();
        entered.store(true, std::memory_order_release);
        if (!Poll([&] { return checked.load(std::memory_order_acquire); })) timed_out.store(true);
        owner_unchanged = std::memcmp(&before, &fixture.Guest(), sizeof(before)) == 0 && fixture.runtime.GetLocation() == location;
        release.store(true, std::memory_order_release);
    };
    std::jthread contender([&](std::stop_token stop) {
        if (!Poll([&] { return entered.load(std::memory_order_acquire) || stop.stop_requested(); })) timed_out.store(true);
        if (entered.load(std::memory_order_acquire)) reasons = {fixture.runtime.Step(), fixture.runtime.Run(), HaltReason::EntryRejected, HaltReason::EntryRejected};
        checked.store(true, std::memory_order_release);
    });
    REQUIRE(Step(fixture) == HaltReason::Step); contender.request_stop(); contender.join();
    REQUIRE(owner_unchanged); REQUIRE_FALSE(timed_out.load()); for (auto reason : reasons) REQUIRE(reason == HaltReason::EntryRejected);
}

TEST_CASE("Runtime Step self modifying stores invalidate old ordinary cache before subsequent Step and Run", "[x64][runtime-step][gate-d][smc]") {
    for (bool cache : {false, true}) {
        Fixture fixture{cache}; constexpr u64 target = code_pc + 0x100;
        fixture.mapping.Word(code_pc, 0xB9000020); fixture.mapping.Word(target, 0x91000462); // STR W0,[X1], ADD X2,X3,#1
        IntrusivePtr<ir::Block> old{new ir::Block(ir::Location{target})};
        REQUIRE(Provider::Decode(nullptr, target, fixture.provider, *old) == SingleDecodeResult::Ready);
        REQUIRE(fixture.space.GetDefaultModule()->Push(old.get())); REQUIRE(backend::TranslateIR(fixture.space.GetDefaultModule(), old) != nullptr);
        fixture.space.GetSmcTracker().RegisterNode(fixture.space.GetDefaultModule(), old.get(), target, target + 4);
        auto initial = Initial(); initial.r[0] = 0x91001462; initial.r[1] = target; fixture.Seed(initial);
        REQUIRE(Step(fixture) == HaltReason::Step); fixture.NoPublication();
        REQUIRE(backend::IsEmpty(fixture.space.GetDefaultModule()->GetNode(ir::Location{target})));
        REQUIRE(fixture.space.GetCodeCache(target) == nullptr);
        u32 word; std::memcpy(&word, fixture.mapping.bytes + target, 4); REQUIRE(word == 0x91001462);
        fixture.runtime.SetLocation(target); auto expected = initial; expected.r[2] = initial.r[3] + 5;
        REQUIRE(Step(fixture) == HaltReason::Step); Equal(expected, fixture.Guest()); REQUIRE(fixture.runtime.GetLocation() == target + 4);
        fixture.NoPublication(target);
        fixture.runtime.SetLocation(target); REQUIRE(fixture.runtime.Run() == HaltReason::CodeMiss);
        IntrusivePtr<ir::Block> fresh{new ir::Block(ir::Location{target})};
        REQUIRE(Provider::Decode(nullptr, target, fixture.provider, *fresh) == SingleDecodeResult::Ready);
        fresh->SetTerminal(ir::terminal::ReturnToHost{});
        REQUIRE(fixture.space.GetDefaultModule()->Push(fresh.get())); REQUIRE(backend::TranslateIR(fixture.space.GetDefaultModule(), fresh) != nullptr);
        fixture.runtime.SetLocation(target); REQUIRE(fixture.runtime.Run() == HaltReason::CallHost); Equal(expected, fixture.Guest());
    }
}

TEST_CASE("ARM64 Core Step owns fresh decode across recursive and concurrent calls", "[x64][runtime-step][gate-d][core][ownership]") {
    CoreFixture fixture, other;
    ArmCore sibling{a64::Arm64Core::Make(fixture.instance.get()), &a64::Arm64Core::Destroy};
    sibling->GetContext() = Initial();
    fixture.mapping.Word(code_pc, 0x91000400); fixture.mapping.Word(code_pc + 4, 0x91000400);
    std::array<Exit, 6> rejected{}; bool unchanged = false;
    fixture.provider.before_fetch = [&] {
        const auto before = fixture.core->GetContext(), other_before = other.core->GetContext(), sibling_before = sibling->GetContext();
        const auto syscall = fixture.core->GetSyscallNumber(), other_syscall = other.core->GetSyscallNumber(), sibling_syscall = sibling->GetSyscallNumber();
        rejected = {fixture.core->Step(), fixture.core->Run(), sibling->Step(), sibling->Run(), other.core->Step(), other.core->Run()};
        unchanged = std::memcmp(&before, &fixture.core->GetContext(), sizeof(before)) == 0 &&
                    std::memcmp(&other_before, &other.core->GetContext(), sizeof(before)) == 0 &&
                    std::memcmp(&sibling_before, &sibling->GetContext(), sizeof(before)) == 0 &&
                    syscall == fixture.core->GetSyscallNumber() && other_syscall == other.core->GetSyscallNumber() && sibling_syscall == sibling->GetSyscallNumber();
    };
    auto expected = fixture.core->GetContext(); ++expected.r[0]; expected.pc += 4;
    REQUIRE(fixture.core->Step() == Exit::Step); Equal(expected, fixture.core->GetContext());
    REQUIRE(unchanged); for (auto reason : rejected) REQUIRE(reason == Exit::EntryRejected);
    REQUIRE(fixture.provider.fetches.load() == 1); REQUIRE(other.provider.fetches.load() == 0);
    std::atomic_bool entered{}, completed{}, timed_out{}; bool owner_unchanged = false;
    fixture.provider.before_fetch = [&] {
        const auto before = fixture.core->GetContext(); const auto syscall = fixture.core->GetSyscallNumber();
        entered.store(true, std::memory_order_release);
        if (!Poll([&] { return completed.load(std::memory_order_acquire); })) timed_out.store(true);
        owner_unchanged = std::memcmp(&before, &fixture.core->GetContext(), sizeof(before)) == 0 && syscall == fixture.core->GetSyscallNumber();
    };
    std::jthread contender([&](std::stop_token stop) {
        if (!Poll([&] { return entered.load(std::memory_order_acquire) || stop.stop_requested(); })) timed_out.store(true);
        if (entered.load(std::memory_order_acquire)) {
            rejected[0] = fixture.core->Step(); rejected[1] = fixture.core->Run();
        }
        completed.store(true, std::memory_order_release);
    });
    const auto reason = fixture.core->Step(); contender.request_stop(); contender.join();
    REQUIRE(reason == Exit::Step); ++expected.r[0]; expected.pc += 4; Equal(expected, fixture.core->GetContext());
    REQUIRE(owner_unchanged); REQUIRE_FALSE(timed_out.load());
    REQUIRE(rejected[0] == Exit::EntryRejected); REQUIRE(rejected[1] == Exit::EntryRejected);
    REQUIRE(fixture.provider.fetches.load() == 2); REQUIRE(fixture.provider.pointers.load() == 0);
    REQUIRE_FALSE(backend::RuntimeExecutionActiveOnCurrentThread());
}

TEST_CASE("Runtime Step does not classify a host range callback fault as guest data fault", "[x64][runtime-step][gate-d][death]") {
    const pid_t child = fork(); REQUIRE(child >= 0);
    if (child == 0) {
        alarm(10);
        rlimit limit{0, 0}; setrlimit(RLIMIT_CORE, &limit);
        Fixture fixture; fixture.mapping.Word(code_pc, 0xF9400020); auto initial = Initial(); initial.r[1] = 0x8000; fixture.Seed(initial);
        void* inaccessible = mmap(nullptr, size_t(getpagesize()), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (inaccessible == MAP_FAILED) _exit(90);
        Rendezvous request{.runtime = &fixture.runtime, .state = fixture.runtime.GetState(), .bad_pointer = inaccessible};
        fixture.runtime.GetState()->interp_range_check = &Rendezvous::Range; fixture.runtime.GetState()->interp_range_check_ctx = &request;
        const auto reason = fixture.runtime.Step(); _exit(reason == HaltReason::PageFatal ? 91 : 92);
    }
    int status{}; REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFSIGNALED(status)); REQUIRE((WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS));
}
