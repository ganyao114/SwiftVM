#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>
#include <xmmintrin.h>
#include <catch2/catch_test_macros.hpp>
#include "runtime/backend/interrupt_request.h"
#include "runtime/backend/riscv64/memory_protocol.h"
#include "runtime/backend/x64/translator.h"
#include "runtime/frontend/arm64/arm64_frontend.h"

using namespace swift;
using namespace swift::runtime;
namespace x64 = backend::x64;
namespace {
using Context = swift::arm64::ThreadContext64;
using Reason = swift::arm64::InterruptReason;
using T = ir::ValueType;
using Bytes = std::array<u8, 32>;
using Entry = HaltReason (*)(backend::State*);
constexpr VAddr pc = 0x1000;
constexpr u64 other_flags = 0xA5A5000002345678;
constexpr u64 helper_value = 0x8123456789ABCDEFULL;
constexpr std::array<u64, 6> nonvolatile{
    0x1111222233334444ULL, 0x2222333344445555ULL, 0x3333444455556666ULL,
    0x4444555566667777ULL, 0x5555666677778888ULL, 0x6666777788889999ULL};

class Fetch final : public MemoryInterface {
public:
    explicit Fetch(u32 word, VAddr start, size_t available) : start(start), available(available) {
        const u32 successor = 0xD2800034; // MOV X20,#1: must never execute.
        std::memcpy(storage.data() + 1, &word, 4);
        std::memcpy(storage.data() + 5, &successor, 4);
    }
    CodeFetchResult ReadCode(void* destination, size_t address, size_t size) override {
        ++fetches; last_address = address; last_size = size;
        if (address != start || size != 4 || available < 4) return CodeFetchResult::Fault;
        std::memcpy(destination, storage.data() + 1, 4);
        return CodeFetchResult::Success;
    }
    bool Read(void*, size_t, size_t) override { ++unsafe_reads; return false; }
    bool Write(void*, size_t, size_t) override { return false; }
    void* GetPointer(void*) override { ++unsafe_reads; return nullptr; }
    size_t fetches{}, unsafe_reads{}, last_address{}, last_size{};
private:
    std::array<u8, 9> storage{};
    VAddr start;
    size_t available;
};

Context Initial() {
    Context value{};
    for (u32 index = 0; index < value.r.size(); ++index)
        value.r[index] = 0x8123456789ABCDEFULL ^ (u64(index) * 0x0101010101010101ULL);
    value.fp = 0xFEDCBA9876543210ULL; value.lr = 0x9988776655443322ULL;
    value.sp = 0x1234567887654321ULL; value.pc = 0x777788889999AAAAULL;
    value.pstate = 0x13579BDF; value.padding = 0x2468ACE0;
    value.fpcr = 0x00800000; value.fpsr = 0x08000015; value.tpidr = 0x1020304050607080ULL;
    for (u32 index = 0; index < 32; ++index) {
        const std::array<u64, 2> lanes{0x1122334455667788ULL + index, 0x8877665544332211ULL - index};
        std::memcpy(&value.v[index], lanes.data(), sizeof(lanes));
    }
    return value;
}

struct Machine {
    alignas(backend::State) std::array<u8, sizeof(backend::State) + sizeof(Context) + 32> storage{};
    backend::State* state = new(storage.data()) backend::State{};
    alignas(16) Bytes memory{};
    ~Machine() { state->~State(); }
    Context& Guest() { return *reinterpret_cast<Context*>(state->uniform_buffer_begin); }
};

struct AbiResult {
    std::array<u64, 6> registers{};
    u64 before{}, after{}, flags{};
    u32 reason{};
};

// A real caller tests every SysV nonvolatile, including the JIT's anchors.
// Its output pointer lives on the stack rather than in a register under test.
class AbiCaller final : public Xbyak::CodeGenerator {
public:
    AbiCaller() {
        push(rbx); push(rbp); push(r12); push(r13); push(r14); push(r15);
        sub(rsp, 40); // entry RSP=8 mod16; 6 pushes +40 => aligned for CALL
        mov(qword[rsp], rdi); mov(qword[rsp + 8], rsi); mov(qword[rsp + 16], rdx);
        mov(qword[rdx + offsetof(AbiResult, before)], rsp);
        const Xbyak::Reg64 registers[]{rbx, rbp, r12, r13, r14, r15};
        for (u32 index = 0; index < 6; ++index) mov(registers[index], nonvolatile[index]);
        mov(rdi, qword[rsp]); mov(rax, qword[rsp + 8]); call(rax);
        mov(rcx, qword[rsp + 16]); mov(dword[rcx + offsetof(AbiResult, reason)], eax);
        for (u32 index = 0; index < 6; ++index) mov(qword[rcx + index * 8], registers[index]);
        mov(qword[rcx + offsetof(AbiResult, after)], rsp);
        pushfq(); pop(rax); mov(qword[rcx + offsetof(AbiResult, flags)], rax);
        add(rsp, 40); pop(r15); pop(r14); pop(r13); pop(r12); pop(rbp); pop(rbx); ret(); ready();
    }
    HaltReason Run(backend::State* state, Entry entry, AbiResult& result) {
        getCode<void (*)(backend::State*, Entry, AbiResult*)>()(state, entry, &result);
        return static_cast<HaltReason>(result.reason);
    }
};

struct HostFP {
    u32 original = _mm_getcsr();
    u32 changed = (original & ~0x6000u) | 0x4000u | 0x1F80u | 0x15u;
    HostFP() { _mm_setcsr(changed); }
    ~HostFP() { _mm_setcsr(original); }
};

struct Request {
    backend::State* state{};
    std::atomic_bool entered{false}, released{false}, timed_out{false};
    std::atomic<u32> calls{0};
    u64 bits{kBackedgeSignalRequest};
    HaltReason synchronous{HaltReason::None};
    bool reject{};
    u64 address{}, size{};
};

void Rendezvous(Request& request) {
    request.calls.fetch_add(1, std::memory_order_relaxed);
    request.entered.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!request.released.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) {
            request.timed_out.store(true, std::memory_order_relaxed); break;
        }
        std::this_thread::yield();
    }
    if (request.synchronous != HaltReason::None) request.state->halt_reason = request.synchronous;
}

u64 ReturnHelper(u64 pointer, u64, u64) {
    auto& request = *reinterpret_cast<Request*>(pointer);
    Rendezvous(request);
    // Deliberately perturb the helper's host FP state; the native epilogue
    // must restore its caller's complete MXCSR on every exit route.
    _mm_setcsr(0x1F80u | 0x6000u | 0x21u);
    return helper_value;
}

bool RangeHelper(void* pointer, u64 address, u64 size) {
    auto& request = *static_cast<Request*>(pointer);
    request.address = address; request.size = size;
    Rendezvous(request);
    return !request.reject;
}

struct Requester {
    Request* request;
    std::jthread thread;
    explicit Requester(Request& value) : request(&value), thread([&value](std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (!value.entered.load(std::memory_order_acquire)) {
            if (stop.stop_requested()) { value.released.store(true, std::memory_order_release); return; }
            if (std::chrono::steady_clock::now() > deadline) {
                value.timed_out.store(true, std::memory_order_relaxed);
                value.released.store(true, std::memory_order_release); return;
            }
            std::this_thread::yield();
        }
        std::atomic_ref<u64>(value.state->exit_request).fetch_or(value.bits, std::memory_order_release);
        value.released.store(true, std::memory_order_release);
    }) {}
    ~Requester() { thread.request_stop(); request->released.store(true, std::memory_order_release); }
};

struct Program {
    Fetch fetch;
    ir::Block block;
    ir::Assembler assembler{&block};
    swift::arm64::A64Decoder decoder;
    CodeFetchResult status;
    explicit Program(u32 word, VAddr start = pc, size_t available = 8)
        : fetch(word, start, available), block(0, start), decoder(start, &fetch, &assembler),
          status(decoder.DecodeSingle()) {}
    void CheckFetch(bool success = true, bool attempted = true) {
        REQUIRE(status == (success ? CodeFetchResult::Success : CodeFetchResult::Fault));
        REQUIRE(fetch.unsafe_reads == 0); REQUIRE(fetch.fetches == size_t(attempted));
        if (attempted) { REQUIRE(fetch.last_address == block.GetStartLocation().Value()); REQUIRE(fetch.last_size == 4); }
        const auto& dependencies = block.GetGuestCodeDependencies();
        REQUIRE(dependencies.size() == size_t(success));
        if (success) {
            REQUIRE(dependencies[0].start == block.GetStartLocation());
            REQUIRE(dependencies[0].end.Value() == block.GetStartLocation().Value() + 4);
        }
        for (auto& instruction : block.GetInstList()) REQUIRE(instruction.GetOp() != ir::OpCode::AdvancePC);
    }
};

void Check(Program& program, bool cache, const Context& initial, const Context& expected,
           u64 flags, u64 wanted_flags, VAddr wanted_pc, HaltReason wanted_reason = HaltReason::Step,
           Bytes input = {}, Bytes wanted = {}, Request* request = nullptr, bool range = false,
           u64 pending = 0, HaltReason synchronous = HaltReason::None,
           x64::ExecutionMode mode = x64::ExecutionMode::SingleInstruction) {
  for (bool detected : {false, true}) {
    CAPTURE(cache, detected, wanted_pc, wanted_reason);
    program.block.ReIdInstr();
    x64::Translator translator{&program.block, detected ? x64::HostFeatures::Detect() : x64::HostFeatures{},
                               nullptr, nullptr, cache, mode};
    translator.Translate();
    Machine machine; machine.Guest() = initial; machine.memory = input;
    machine.state->pt = machine.memory.data(); machine.state->guest_addr_limit = machine.memory.size();
    machine.state->host_cpu_flags = flags; machine.state->current_loc = ir::Location{0xDEAD0000};
    machine.state->halt_reason = synchronous;
    std::atomic_ref<u64>(machine.state->exit_request).store(pending, std::memory_order_release);
    const bool entry_stopped = mode == x64::ExecutionMode::SingleInstruction &&
        (synchronous != HaltReason::None || (pending & kBackedgeSignalRequest));
    constexpr u64 recovery_pc_sentinel = 0xABCDEF123400ULL, recovery_frame_sentinel = 0x987654321000ULL;
    if (entry_stopped) {
        machine.state->spill_area[backend::kRiscvRecoveryPcSlot] = recovery_pc_sentinel;
        machine.state->spill_area[backend::kRiscvRecoveryFrameSlot] = recovery_frame_sentinel;
    }
    machine.state->prev_loc = ir::Location{0xBEEF0000};
    if (request) {
        // The probe IR contains this stable pointer, but each feature route
        // must perform a fresh real rendezvous rather than reuse its latch.
        request->entered.store(false); request->released.store(false); request->timed_out.store(false);
        request->calls.store(0); request->address = request->size = 0;
        request->state = machine.state;
        if (range) { machine.state->interp_range_check = RangeHelper; machine.state->interp_range_check_ctx = request; }
    }
    const auto readers = backend::riscv64::anonymous_memory_readers.load();
    AbiCaller caller; AbiResult abi;
    HaltReason reason;
    u32 observed_mxcsr;
    {
        HostFP fp;
        {
            std::unique_ptr<Requester> requester;
            if (request && !entry_stopped) requester = std::make_unique<Requester>(*request);
            reason = caller.Run(machine.state, translator.getCode<Entry>(), abi);
        }
        observed_mxcsr = _mm_getcsr();
        REQUIRE(observed_mxcsr == fp.changed);
    }
    REQUIRE(reason == wanted_reason); REQUIRE(machine.state->halt_reason == wanted_reason);
    REQUIRE(std::memcmp(&machine.Guest(), &expected, sizeof(expected)) == 0);
    REQUIRE(machine.state->host_cpu_flags == wanted_flags);
    REQUIRE(machine.state->current_loc.Value() == wanted_pc); REQUIRE(machine.memory == wanted);
    REQUIRE(abi.registers == nonvolatile); REQUIRE(abi.before == abi.after); REQUIRE((abi.flags & (1u << 10)) == 0);
    REQUIRE(machine.state->spill_area[backend::kRiscvRecoveryPcSlot] == (entry_stopped ? recovery_pc_sentinel : 0));
    REQUIRE(machine.state->spill_area[backend::kRiscvRecoveryFrameSlot] == (entry_stopped ? recovery_frame_sentinel : 0));
    if (entry_stopped) REQUIRE(machine.state->prev_loc == ir::Location{0xBEEF0000});
    REQUIRE(machine.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0);
    REQUIRE(backend::riscv64::anonymous_memory_readers.load() == readers);
    REQUIRE(std::atomic_ref<u64>(machine.state->exit_request).load(std::memory_order_acquire) ==
            (pending | (request && request->entered.load(std::memory_order_acquire) ? request->bits : 0)));
    if (request) { REQUIRE_FALSE(request->timed_out.load()); REQUIRE(request->calls.load() == u32(!entry_stopped)); }
  }
}

// Test-only HIR rendezvous. It adds no guest code word/dependency. Splice
// before emission so the register plan sees the real helper/local CFG.
void Inject(Program& program, Request& request, ir::Inst* before, bool store, u32 shape = 0) {
    ir::Assembler as{&program.block};
    auto pointer = as.LoadImm(ir::Imm{reinterpret_cast<u64>(&request)}).SetType(T::U64);
    ir::Lambda lambda{ir::Imm{reinterpret_cast<u64>(&ReturnHelper)}};
    ir::Value result;
    if (shape == 0) result = as.CallLambda(lambda, pointer, pointer, pointer).SetType(T::U64);
    else {
        ir::Params parameters; parameters.Push(pointer); parameters.Push(pointer); parameters.Push(pointer);
        result = (shape == 1 ? as.CallDynamic(lambda, parameters) : as.CallLocation(lambda, parameters)).SetType(T::U64);
    }
    std::vector<ir::Inst*> inserted{pointer.Def(), result.Def()};
    if (store) {
        as.StoreUniform(ir::Uniform{u32(offsetof(Context, r) + 7 * 8), T::U64}, result);
        inserted.push_back(&program.block.GetInstList().back());
    }
    if (before) for (auto* instruction : inserted) {
        program.block.RemoveInst(instruction); program.block.InsertBefore(instruction, before);
    }
    program.block.ReIdInstr();
}

struct Arithmetic { u64 value, flags; };
Arithmetic Calculate(bool wide, bool subtract, u64 lhs, u64 rhs, bool carry) {
    const u32 width = wide ? 64 : 32;
    const u64 mask = wide ? UINT64_MAX : UINT32_MAX;
    lhs &= mask; rhs &= mask;
    const unsigned __int128 a = lhs, b = rhs, extra = subtract ? !carry : carry;
    const auto raw = subtract ? a - b - extra : a + b + extra;
    const u64 result = u64(raw) & mask;
    const auto signed_value = [=](u64 value) -> __int128 {
        return value & (u64{1} << (width - 1)) ? __int128(value) - (__int128{1} << width) : __int128(value);
    };
    const auto total = subtract ? signed_value(lhs) - signed_value(rhs) - __int128(extra)
                               : signed_value(lhs) + signed_value(rhs) + __int128(extra);
    return {result, (u64(result >> (width - 1)) << 31) | (u64(result == 0) << 30) |
            (u64(subtract ? a >= b + extra : (raw >> width) != 0) << 29) |
            (u64(total < -(__int128{1} << (width - 1)) || total >= (__int128{1} << (width - 1))) << 28)};
}
} // namespace

TEST_CASE("single native retires only one word and preserves host ABI", "[x64][single-native][gate-c]") {
    for (bool cache : {false, true}) for (size_t available : {size_t{4}, size_t{8}})
    for (u32 word : {0x91001420u, 0xD503201Fu}) {
        Program program{word, pc, available}; program.CheckFetch();
        auto initial = Initial(), expected = initial;
        if (word == 0x91001420) expected.r[0] = initial.r[1] + 5;
        Check(program, cache, initial, expected, other_flags | 0xF0000000, other_flags | 0xF0000000, pc + 4);
    }
}

TEST_CASE("single native entry preserves synchronous priority and sticky requests", "[x64][single-native][gate-c]") {
    for (bool cache : {false, true})
    for (u64 bits : {u64{0}, u64{17}, kBackedgeSignalRequest, kBackedgeSignalRequest | 17})
    for (auto synchronous : {HaltReason::None, HaltReason::PageFatal, HaltReason::IllegalCode, HaltReason::CallHost}) {
        Program program{0x91001420}; program.CheckFetch();
        auto initial = Initial(), expected = initial;
        const bool completed = synchronous == HaltReason::None && !(bits & kBackedgeSignalRequest);
        if (completed) expected.r[0] = initial.r[1] + 5;
        const auto reason = synchronous != HaltReason::None ? synchronous :
                            (completed ? HaltReason::Step : HaltReason::Signal);
        Check(program, cache, initial, expected, other_flags, other_flags,
              completed ? pc + 4 : 0xDEAD0000, reason, {}, {}, nullptr, false, bits, synchronous);
    }
    // Entry admission must not enter even a checked memory callback.
    for (bool cache : {false, true}) for (auto synchronous : {HaltReason::None, HaltReason::PageFatal}) {
        Program program{0xF9400020}; auto initial = Initial(); initial.r[1] = 8; Request request;
        Check(program, cache, initial, initial, other_flags, other_flags, 0xDEAD0000,
              synchronous == HaltReason::None ? HaltReason::Signal : synchronous,
              {}, {}, &request, true, kBackedgeSignalRequest, synchronous);
    }
    // Ordinary dispatch still treats low SMC bits as a restartable CodeMiss.
    for (bool cache : {false, true}) {
        Program program{0x91001420};
        Check(program, cache, Initial(), Initial(), other_flags, other_flags, pc,
              HaltReason::CodeMiss, {}, {}, nullptr, false, 17, HaltReason::None, x64::ExecutionMode::Ordinary);
    }
}

TEST_CASE("single native PC terminals preserve BLR X30 input before LR output", "[x64][single-native][gate-c]") {
    for (bool cache : {false, true}) {
        for (u32 word : {0x14000003u, 0x97FFFFFEu, 0xD61F03C0u, 0xD63F03C0u, 0xD65F03C0u}) {
            Program program{word}; program.CheckFetch();
            auto initial = Initial(); initial.lr = 0x87654320; auto expected = initial;
            const bool call = word == 0x97FFFFFE || word == 0xD63F03C0;
            if (call) expected.lr = pc + 4;
            const VAddr target = word == 0x14000003 ? pc + 12 : word == 0x97FFFFFE ? pc - 8 : initial.lr;
            Check(program, cache, initial, expected, other_flags, other_flags, target);
        }
        for (u32 word : {0x54000060u, 0xB4000061u, 0x36380061u}) for (bool taken : {false, true}) {
            Program program{word}; program.CheckFetch(); auto initial = Initial(); initial.r[1] = taken ? 0 : 0x80;
            const u64 flags = other_flags | (u64(taken) << 30);
            Check(program, cache, initial, initial, flags, flags, taken ? pc + 12 : pc + 4);
        }
        for (bool page : {false, true}) for (s32 immediate : {-3, 5}) {
            const u32 field = u32(immediate) & 0x1FFFFF;
            const u32 word = (page ? u32(vixl::aarch64::ADRP) : u32(vixl::aarch64::ADR)) |
                             ((field & 3) << 29) | ((field >> 2) << 5);
            Program program{word, 0x8FFC}; program.CheckFetch(); auto initial = Initial(), expected = initial;
            expected.r[0] = page ? 0x8000 + s64(immediate) * 4096 : 0x8FFC + s64(immediate);
            Check(program, cache, initial, expected, other_flags, other_flags, 0x9000);
        }
    }
}

TEST_CASE("single native carry arithmetic publishes all NZCV", "[x64][single-native][gate-c]") {
    using namespace vixl::aarch64;
    for (bool cache : {false, true}) for (bool wide : {false, true}) for (bool subtract : {false, true}) {
        const u64 sign = u64{1} << (wide ? 63 : 31), mask = wide ? UINT64_MAX : UINT32_MAX;
        const std::array<std::array<u64, 2>, 4> pairs{{{sign - 1, 0}, {0, 0}, {mask, 1}, {sign, 1}}};
        Program program{u32(wide ? (subtract ? SBCS_x : ADCS_x) : (subtract ? SBCS_w : ADCS_w)) | (2u << 16) | (1u << 5)};
        program.CheckFetch();
        for (bool carry : {false, true}) for (const auto pair : pairs) {
            auto initial = Initial(); initial.r[1] = pair[0]; initial.r[2] = pair[1]; auto expected = initial;
            const auto result = Calculate(wide, subtract, pair[0], pair[1], carry); expected.r[0] = result.value;
            Check(program, cache, initial, expected, other_flags | (u64(carry) << 29) | (1u << 30),
                  other_flags | result.flags, pc + 4);
        }
    }
}

TEST_CASE("single native conditional compare retires both local paths under rendezvous Signal", "[x64][single-native][gate-c]") {
    using namespace vixl::aarch64;
    for (bool cache : {false, true}) for (bool wide : {false, true}) for (bool subtract : {false, true})
    for (bool immediate : {false, true}) for (bool condition : {false, true}) for (bool interrupt : {false, true}) {
        const u32 base = immediate ? u32(wide ? (subtract ? CCMP_x_imm : CCMN_x_imm) : (subtract ? CCMP_w_imm : CCMN_w_imm))
                                   : u32(wide ? (subtract ? CCMP_x : CCMN_x) : (subtract ? CCMP_w : CCMN_w));
        Program program{base | ((immediate ? 3u : 2u) << 16) | (1u << 5) | 0xBu}; program.CheckFetch();
        ir::Inst* conditional{}; u32 gotos{}, labels{};
        for (auto& instruction : program.block.GetInstList()) {
            if (instruction.GetOp() == ir::OpCode::Goto) { ++gotos; if (!conditional) conditional = &instruction; }
            labels += instruction.GetOp() == ir::OpCode::BindLabel;
        }
        REQUIRE(gotos == 2); REQUIRE(labels == 2); REQUIRE(conditional != nullptr);
        Request request;
        if (interrupt) Inject(program, request, conditional, false);
        auto initial = Initial(); initial.r[1] = (u64{1} << (wide ? 63 : 31)) - 1; initial.r[2] = 1;
        const auto result = Calculate(wide, subtract, initial.r[1], immediate ? 3 : initial.r[2], subtract);
        Check(program, cache, initial, initial, other_flags | (u64(condition) << 30),
              other_flags | (condition ? result.flags : u64{0xB} << 28), pc + 4,
              interrupt ? HaltReason::Signal : HaltReason::Step, {}, {}, interrupt ? &request : nullptr);
    }
}

TEST_CASE("single native returning helpers commit RAX suffix before Signal but abort synchronous faults", "[x64][single-native][gate-c]") {
    for (bool cache : {false, true}) for (u32 shape = 0; shape < 3; ++shape)
    for (u64 bits : {u64{17}, kBackedgeSignalRequest, kBackedgeSignalRequest | 17})
    for (auto synchronous : {HaltReason::None, HaltReason::PageFatal, HaltReason::IllegalCode, HaltReason::CallHost}) {
        Program program{0xD503201F}; program.CheckFetch(); Request request; request.bits = bits; request.synchronous = synchronous;
        Inject(program, request, nullptr, true, shape);
        auto initial = Initial(), expected = initial;
        if (synchronous == HaltReason::None) expected.r[7] = helper_value;
        const auto reason = synchronous != HaltReason::None ? synchronous :
                            (bits & kBackedgeSignalRequest ? HaltReason::Signal : HaltReason::Step);
        Check(program, cache, initial, expected, other_flags, other_flags,
              synchronous == HaltReason::None ? pc + 4 : pc, reason, {}, {}, &request);
    }
    // Ordinary mode keeps its existing instruction-internal stop contract.
    for (bool cache : {false, true}) {
        Program program{0xD503201F}; Request request;
        Inject(program, request, nullptr, true);
        Check(program, cache, Initial(), Initial(), other_flags, other_flags, pc, HaltReason::Signal,
              {}, {}, &request, false, 0, HaltReason::None, x64::ExecutionMode::Ordinary);
    }
}

TEST_CASE("single native checked data helpers retire memory or prioritize rejected range", "[x64][single-native][gate-c]") {
    for (bool cache : {false, true}) for (bool store : {false, true}) for (bool fault : {false, true}) {
        Program program{store ? 0xF9000020u : 0xF9400020u}; program.CheckFetch();
        Bytes input; input.fill(0xA7); const u64 value = 0xDEADBEEF12345678ULL; std::memcpy(input.data() + 8, &value, 8);
        auto wanted = input; auto initial = Initial(); initial.r[1] = 8; auto expected = initial;
        if (!fault) {
            if (store) std::memcpy(wanted.data() + 8, &initial.r[0], 8);
            else expected.r[0] = value;
        }
        Request request; request.reject = fault;
        Check(program, cache, initial, expected, other_flags, other_flags, fault ? pc : pc + 4,
              fault ? HaltReason::PageFatal : HaltReason::Signal, input, wanted, &request, true);
        REQUIRE(request.address == 8); REQUIRE(request.size == 8);
    }
    for (bool cache : {false, true}) for (u32 size = 0; size < 4; ++size) {
        Program program{0x38201000u | (size << 30) | (2u << 5)}; program.CheckFetch();
        Bytes input; input.fill(0xA7); const u64 old = 0xFEDCBA9876543210ULL, operand = 0x0F0FF0F05555AAAAULL;
        const size_t bytes = size_t{1} << size; std::memcpy(input.data() + 8, &old, bytes);
        auto wanted = input; const u64 replacement = old & ~operand; std::memcpy(wanted.data() + 8, &replacement, bytes);
        auto initial = Initial(); initial.r[0] = operand; initial.r[2] = 8; auto expected = initial;
        expected.r[0] = old & (size == 3 ? UINT64_MAX : (u64{1} << (8u << size)) - 1);
        Request request;
        Check(program, cache, initial, expected, other_flags, other_flags, pc + 4, HaltReason::Signal, input, wanted, &request, true);
        REQUIRE(request.address == 8); REQUIRE(request.size == bytes);
    }
}

TEST_CASE("single native host exits and fetch fault IR outrank retirement requests", "[x64][single-native][gate-c]") {
    struct Exit { u32 word; Reason reason; bool next; };
    for (bool cache : {false, true}) for (const auto item : {
        Exit{0xD4000001, Reason::SVC, true}, Exit{0xD4200000, Reason::BRK, false},
        Exit{0xD4400000, Reason::HLT, false}, Exit{0, Reason::ILL_CODE, false}}) {
        Program program{item.word}; program.CheckFetch(); Request request;
        Inject(program, request, &program.block.GetInstList().front(), false);
        auto initial = Initial(), expected = initial; expected.padding = u32(item.reason);
        Check(program, cache, initial, expected, other_flags, other_flags,
              item.next ? pc + 4 : pc, HaltReason::CallHost, {}, {}, &request);
    }
    for (bool cache : {false, true}) for (size_t available = 0; available < 4; ++available) {
        Program program{0xD503201F, pc, available}; program.CheckFetch(false);
        auto initial = Initial(), expected = initial; expected.padding = u32(Reason::PAGE_FATAL);
        Check(program, cache, initial, expected, other_flags, other_flags, pc, HaltReason::CallHost);
    }
}

TEST_CASE("single native constructor rejects multi instruction and hidden dispatch shapes", "[x64][single-native][gate-c]") {
    for (bool cache : {false, true}) {
        Program program{0xD503201F}; ir::Assembler as{&program.block}; as.AdvancePC(ir::Imm{u32{4}});
        REQUIRE_THROWS_AS((x64::Translator{&program.block, {}, nullptr, nullptr, cache, x64::ExecutionMode::SingleInstruction}), std::runtime_error);
        Program planned{0xD503201F}; backend::FunctionPlan plan;
        REQUIRE_THROWS_AS((x64::Translator{&planned.block, {}, nullptr, &plan, cache, x64::ExecutionMode::SingleInstruction}), std::runtime_error);
        Program hidden{0xD503201F}; ir::Assembler hs{&hidden.block};
        auto value = hs.LoadImm(ir::Imm{u64{0}}).SetType(T::U64);
        Vector<ir::terminal::Switch::Case> cases;
        ir::terminal::Switch selection{value, cases};
        hidden.block.SetTerminal(ir::terminal::CheckHalt{ir::terminal::Condition{
            ir::Cond::EQ, ir::terminal::LinkBlock{ir::Location{pc + 4}}, selection}});
        REQUIRE_THROWS_AS((x64::Translator{&hidden.block, {}, nullptr, nullptr, cache, x64::ExecutionMode::SingleInstruction}), std::runtime_error);
    }
}

TEST_CASE("single native state reuses its cleaned frame after owner clears retirement", "[x64][single-native][gate-c]") {
    for (bool cache : {false, true}) for (bool detected : {false, true}) {
        Program program{0x91000400}; program.CheckFetch(); // ADD X0,X0,#1
        x64::Translator translator{&program.block, detected ? x64::HostFeatures::Detect() : x64::HostFeatures{},
                                   nullptr, nullptr, cache, x64::ExecutionMode::SingleInstruction};
        translator.Translate(); Machine machine; machine.Guest() = Initial();
        machine.state->host_cpu_flags = other_flags; AbiCaller caller;
        const auto readers = backend::riscv64::anonymous_memory_readers.load();
        auto expected = machine.Guest();
        for (u32 attempt = 0; attempt < 2; ++attempt) {
            ++expected.r[0]; AbiResult abi;
            machine.state->halt_reason = HaltReason::None;
            std::atomic_ref<u64>(machine.state->exit_request).store(0, std::memory_order_release);
            { HostFP fp; REQUIRE(caller.Run(machine.state, translator.getCode<Entry>(), abi) == HaltReason::Step);
              REQUIRE(_mm_getcsr() == fp.changed); }
            REQUIRE(machine.state->halt_reason == HaltReason::Step);
            REQUIRE(std::memcmp(&machine.Guest(), &expected, sizeof(expected)) == 0);
            REQUIRE(machine.state->host_cpu_flags == other_flags); REQUIRE(machine.state->current_loc.Value() == pc + 4);
            REQUIRE(abi.registers == nonvolatile); REQUIRE(abi.before == abi.after); REQUIRE((abi.flags & (1u << 10)) == 0);
            REQUIRE(machine.state->spill_area[backend::kRiscvRecoveryPcSlot] == 0);
            REQUIRE(machine.state->spill_area[backend::kRiscvRecoveryFrameSlot] == 0);
            REQUIRE(machine.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0);
            REQUIRE(backend::riscv64::anonymous_memory_readers.load() == readers);
        }
    }
}
