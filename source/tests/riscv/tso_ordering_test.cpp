#include "test_support.h"
#include "runtime/compile/builder/ir_builder.h"
#include "runtime/backend/atomic_fallback.h"
#include <barrier>
#include <bit>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <sched.h>
#include <sys/resource.h>

namespace swift::tests::riscv {
namespace {
using T = ir::ValueType;
bool inject_forbidden{};
bool inject_cas_tear{};
enum class Event { Read, Write, Fence };
struct Step { Event event; u32 address{}; u32 output{}; };
struct Program {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xf900})};
    compile::IrBuilder as{block.get()};
    std::unique_ptr<Compiled> code;
    Program(std::initializer_list<Step> steps, T type, bool ordered = true) {
        for (auto step : steps) {
            if (step.event == Event::Fence) {
                as.MemoryBarrierTSO();
                continue;
            }
            auto address = as.LoadImm(T::U64, ir::Imm{u64(step.address)});
            if (step.event == Event::Write) {
                auto value = as.LoadImm(type, ir::Imm{u64{1}});
                if (ordered)
                    as.StoreMemoryTSO(ir::Operand{address}, value);
                else as.StoreMemory(ir::Operand{address}, value);
            } else {
                auto value = (ordered ? as.LoadMemoryTSO(type, ir::Operand{address})
                                      : as.LoadMemory(type, ir::Operand{address}));
                as.StoreUniform(ir::Uniform{step.output * 8, type}, value);
            }
        }
        block->SetTerminal(ir::terminal::ReturnToHost{});
        code = std::make_unique<Compiled>(block.get());
    }
};
struct Fences { u32 full{}, read{}, write{}; };
Fences Encodings(Compiled& code) {
    Fences result;
    auto& buffer = code.context.GetMasm().GetCodeBuffer();
    for (u32 offset = 0; offset < code.context.CurrentBufferSize(); offset += 4) {
        u32 word; std::memcpy(&word, buffer.GetOffsetPointer(offset), 4);
        result.full += word == 0x0ff0000f; // Existing Biscuit FENCE(): IORW,IORW.
        result.read += word == 0x0230000f; // R,RW.
        result.write += word == 0x0310000f; // RW,W.
    }
    return result;
}
void EncodingChecks(bool narrow) {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool store : {false, true}) {
        const auto event = store ? Event::Write : Event::Read;
        Program ordered{{{event, 64}}, type}, plain{{{event, 64}}, type, false};
        const auto a = Encodings(*ordered.code), b = Encodings(*plain.code);
        const u32 fallback = type == T::U8 ? 0 : 2;
        Check(a.full == b.full + (narrow ? fallback : fallback + 2), "exact full-fence delta includes unchanged unaligned fallback");
        Check(a.read == b.read + (narrow && !store ? 1 : 0), "aligned load fence R,RW");
        Check(a.write == b.write + (narrow && store ? 1 : 0), "aligned store fence RW,W");
        std::cout << "encoding width=" << u32(ir::GetValueSizeByte(type)) << " store=" << store
                  << " full_delta=" << a.full - b.full << " read_delta=" << a.read - b.read
                  << " write_delta=" << a.write - b.write << '\n';
    }
    Program barrier{{{Event::Fence}}, T::U64}, empty{{}, T::U64};
    Check(Encodings(*barrier.code).full == Encodings(*empty.code).full + 1,
          "explicit MemoryBarrierTSO retains full IORW,IORW fence");
}
bool Pin(int cpu) {
    cpu_set_t mask; CPU_ZERO(&mask); CPU_SET(cpu, &mask);
    return sched_setaffinity(0, sizeof(mask), &mask) == 0;
}
struct alignas(64) Cell { u64 value{}; u8 padding[56]{}; };
using Results = std::array<std::array<u64, 2>, 3>;
void Litmus(const char* name, T type, std::initializer_list<Program*> units,
            const std::function<bool(const Results&)>& forbidden, u32 rounds) {
    std::array<Cell, 3> memory{};
    Results results{};
    const std::vector<Program*> programs{units};
    std::barrier sync{static_cast<std::ptrdiff_t>(programs.size() + 1)};
    std::atomic<bool> failed{};
    std::vector<std::thread> workers;
    for (u32 index = 0; index < programs.size(); ++index) workers.emplace_back([&, index] {
        StateStorage state; state.state->pt = memory.data();
        state.state->interp_range_check = nullptr; // No opaque callback fences in litmus.
        if (!Pin(index + 1)) failed = true;
        for (u32 round = 0; round < rounds; ++round) {
            sync.arrive_and_wait();
            state.state->halt_reason = HaltReason::None;
            state.Put(0, 0); state.Put(8, 0);
            if (programs[index]->code->fn(state.state) != HaltReason::CallHost ||
                state.memory_participant.active.load() != 0 ||
                state.state->spill_area[backend::kRiscvMemoryOwnedSlot] != 0) failed = true;
            results[index] = {state.Get(0), state.Get(8)};
            sync.arrive_and_wait();
        }
    });
    u32 forbidden_count{}, zero_zero{}, invalid_values{};
    for (u32 round = 0; round < rounds; ++round) {
        for (auto& cell : memory) cell.value = 0;
        sync.arrive_and_wait(); sync.arrive_and_wait();
        if (inject_forbidden && round == 0 && std::string_view{name} == "MP") {
            results[1] = {1, 0};
            std::cout << "negative_control synthetic MP result=(1,0); not a hardware observation\n";
        }
        forbidden_count += forbidden(results);
        zero_zero += results[0][0] == 0 && results[1][0] == 0;
        for (u32 index = 0; index < programs.size(); ++index) {
            ++checks;
            invalid_values += results[index][0] > 1 || results[index][1] > 1;
        }
    }
    for (auto& worker : workers) worker.join();
    Check(invalid_values == 0, "litmus native reads are written values");
    Check(!failed, "each native invocation halts correctly, releases lease, and uses distinct CPU");
    Check(forbidden_count == 0, std::string{name} + " forbidden TSO outcome");
    std::cout << "litmus " << name << " width=" << u32(ir::GetValueSizeByte(type)) << " rounds=" << rounds
              << " forbidden=" << forbidden_count << " observed_zero_zero=" << zero_zero << '\n';
}
void Concurrency(u32 rounds) {
    Check(Pin(0), "main coordinator pinned to CPU0");
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) {
        Program mpw{{{Event::Write, 64}, {Event::Write, 128}}, type};
        Program mpr{{{Event::Read, 128, 0}, {Event::Read, 64, 1}}, type};
        Litmus("MP", type, {&mpw, &mpr}, [](const Results& r) { return r[1][0] == 1 && r[1][1] == 0; }, rounds);
        Program lba{{{Event::Read, 64, 0}, {Event::Write, 128}}, type};
        Program lbb{{{Event::Read, 128, 0}, {Event::Write, 64}}, type};
        Litmus("LB", type, {&lba, &lbb}, [](const Results& r) { return r[0][0] == 1 && r[1][0] == 1; }, rounds);
        Program wrca{{{Event::Write, 64}}, type};
        Program wrcb{{{Event::Read, 64, 0}, {Event::Write, 128}}, type};
        Program wrcc{{{Event::Read, 128, 0}, {Event::Read, 64, 1}}, type};
        Litmus("WRC", type, {&wrca, &wrcb, &wrcc}, [](const Results& r) {
            return r[1][0] == 1 && r[2][0] == 1 && r[2][1] == 0;
        }, rounds);
        Program sba{{{Event::Write, 64}, {Event::Fence}, {Event::Read, 128, 0}}, type};
        Program sbb{{{Event::Write, 128}, {Event::Fence}, {Event::Read, 64, 0}}, type};
        Litmus("SB-fence", type, {&sba, &sbb}, [](const Results& r) { return r[0][0] == 0 && r[1][0] == 0; }, rounds);
        Program sboa{{{Event::Write, 64}, {Event::Read, 128, 0}}, type};
        Program sbob{{{Event::Write, 128}, {Event::Read, 64, 0}}, type};
        // Store->load reordering is permitted by TSO, never required to occur.
        Litmus("SB-allowed", type, {&sboa, &sbob}, [](const Results&) { return false; }, rounds);
    }
}
struct Oracle {
    u64 calls{}, fail_at{}; bool throws{}; u8* memory{}; bool saw_store{};
};
bool Range(void* opaque, u64 address, u64 size) {
    auto& oracle = *static_cast<Oracle*>(opaque);
    ++oracle.calls;
    if (oracle.calls == 2) oracle.saw_store = oracle.memory[64] == 1;
    if (oracle.calls == oracle.fail_at) {
        if (oracle.throws) throw std::runtime_error("test range oracle exception");
        return false;
    }
    return address <= 192 && size <= 192 - address;
}
void MixedObservers() {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xfa00})};
        compile::IrBuilder as{block.get()};
        auto address = as.LoadImm(T::U64, ir::Imm{u64{64}});
        auto value = as.LoadImm(type, ir::Imm{u64{1}});
        as.StoreMemoryTSO(ir::Operand{address}, value);
        auto vector = as.LoadMemoryTSO(T::V128, ir::Operand{address});
        as.StoreUniform(ir::Uniform{32, T::V128}, vector);
        auto scalar = as.LoadMemoryTSO(type, ir::Operand{address});
        as.StoreUniform(ir::Uniform{8, type}, scalar);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled code{block.get()};
        for (bool anonymous : {false, true}) for (u64 fail : {u64{0}, u64{2}}) for (bool throws : {false, true}) {
            alignas(64) std::array<u8, 192> memory{};
            StateStorage state; state.state->pt = memory.data();
            if (anonymous) state.state->spill_area[backend::kRiscvMemoryParticipantSlot] = 0;
            Oracle oracle{0, fail, throws, memory.data()};
            state.state->interp_range_check = &Range; state.state->interp_range_check_ctx = &oracle;
            const auto expected = fail == 0 ? HaltReason::CallHost : throws ? HaltReason::IllegalCode : HaltReason::PageFatal;
            Check(code.fn(state.state) == expected, "scalar/vector mixed block retains helper halt semantics");
            Check(oracle.calls == (fail ? 2 : 3) && oracle.saw_store && memory[64] == 1,
                  "callback observes preceding scalar store and exact access count");
            if (!fail) Check(state.Get(8) == 1 && state.Get(32) == 1 && state.Get(40) == 0, "mixed vector/scalar values");
            Check(state.memory_participant.active.load() == 0 &&
                  state.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 && rv::anonymous_memory_readers.load() == 0,
                  "success, fault and exception release registered/anonymous lease");
        }
    }
}
u64 PairHigh(u64 ticket) { return std::rotl(ticket, 17) ^ 0xdeadc0de12345678ULL; }
bool LegalTicket(u64 ticket, u32 rounds) {
    return ticket == 0 || ((ticket & 3) >= 1 && (ticket & 3) <= 2 &&
                           (ticket >> 2) >= 1 && (ticket >> 2) <= 2 * rounds);
}
bool LegalPair(u64 low, u64 high, u32 rounds) { return LegalTicket(low, rounds) && high == PairHigh(low); }
void SoftwareCAS128(T reader_type, bool anonymous) {
    constexpr u32 rounds = 5000, read_count = rounds * 4;
    auto features = rv::HostFeatures::Detect(); features.zacas = false;
    IntrusivePtr<ir::Block> writer_block{new ir::Block(ir::Location{0xfb00})};
    compile::IrBuilder writer_as{writer_block.get()};
    auto address = writer_as.LoadUniform(T::U64, ir::Uniform{0, T::U64});
    auto before = writer_as.LoadMemoryTSO(T::U64, ir::Operand{address});
    writer_as.StoreUniform(ir::Uniform{56, T::U64}, before);
    std::array<ir::Value, 4> arguments;
    for (u32 index = 0; index < arguments.size(); ++index)
        arguments[index] = writer_as.LoadUniform(T::U64, ir::Uniform{8 + index * 8, T::U64});
    auto old = writer_as.CompareAndSwap128(
            T::V128, address, arguments[0], arguments[1], arguments[2], arguments[3]);
    writer_as.StoreUniform(ir::Uniform{40, T::V128}, old);
    auto after_low = writer_as.LoadMemoryTSO(T::U64, ir::Operand{address});
    auto high_address = writer_as.Add(T::U64, address, ir::Operand{ir::Imm{u64{8}}});
    auto after_high = writer_as.LoadMemoryTSO(T::U64, ir::Operand{high_address});
    writer_as.StoreUniform(ir::Uniform{64, T::U64}, after_low);
    writer_as.StoreUniform(ir::Uniform{72, T::U64}, after_high);
    writer_block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled writer{writer_block.get(), true, features};

    IntrusivePtr<ir::Block> reader_block{new ir::Block(ir::Location{0xfb10})};
    compile::IrBuilder reader_as{reader_block.get()};
    auto base = reader_as.LoadImm(T::U64, ir::Imm{u64{0}});
    auto scalar_low = reader_as.LoadMemoryTSO(reader_type, ir::Operand{base});
    auto vector = reader_as.LoadMemoryTSO(T::V128, ir::Operand{base});
    auto high = reader_as.LoadImm(T::U64, ir::Imm{u64{8}});
    auto scalar_high = reader_as.LoadMemoryTSO(reader_type, ir::Operand{high});
    reader_as.StoreUniform(ir::Uniform{64, reader_type}, scalar_low);
    reader_as.StoreUniform(ir::Uniform{80, T::V128}, vector);
    reader_as.StoreUniform(ir::Uniform{104, reader_type}, scalar_high);
    reader_block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled reader{reader_block.get(), true, features};
    alignas(16) std::array<u64, 2> memory{0, PairHigh(0)};
    std::barrier start{4};
    std::atomic<bool> failed{};
    std::array<u32, 2> wins{}, attempts{};
    std::atomic<u32> reads{}, seen_writers{};
    const auto restored = [](StateStorage& state) {
        return state.memory_participant.active.load() == 0 &&
               state.state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 && rv::memory_kernel_depth.load() == 0;
    };
    const auto run_writer = [&](u32 index) {
        StateStorage state; state.state->pt = memory.data(); state.Put(0, 0);
        if (anonymous) state.state->spill_area[backend::kRiscvMemoryParticipantSlot] = 0;
        if (!Pin(index + 1)) failed = true;
        start.arrive_and_wait();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
        u64 expected{};
        while (wins[index] < rounds && attempts[index] < rounds * 64) {
            if (std::chrono::steady_clock::now() > deadline) { failed = true; break; }
            ++attempts[index];
            const u64 desired = (((expected >> 2) + 1) << 2) | (index + 1);
            state.Put(8, expected); state.Put(16, PairHigh(expected));
            state.Put(24, desired); state.Put(32, PairHigh(desired));
            state.state->halt_reason = HaltReason::None;
            const auto halt = writer.fn(state.state);
            const auto old_low = state.Get(40), old_high = state.Get(48);
            // The CAS helper releases/reacquires the lease. Never equate its
            // old pair with the preceding or following scalar snapshots.
            if (halt != HaltReason::CallHost || !LegalTicket(state.Get(56), rounds) ||
                !LegalPair(old_low, old_high, rounds) || !LegalPair(state.Get(64), state.Get(72), rounds) || !restored(state)) {
                failed = true; break;
            }
            if (old_low == expected && old_high == PairHigh(expected)) { ++wins[index]; expected = desired; }
            else expected = old_low;
        }
        if (!restored(state)) failed = true;
    };
    std::thread first{run_writer, 0}, second{run_writer, 1};
    std::thread observer{[&] {
        StateStorage state; state.state->pt = memory.data();
        if (anonymous) state.state->spill_area[backend::kRiscvMemoryParticipantSlot] = 0;
        if (!Pin(3)) failed = true;
        start.arrive_and_wait();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
        const u32 width = ir::GetValueSizeByte(reader_type);
        const u64 mask = width == 8 ? UINT64_MAX : (u64{1} << (width * 8)) - 1;
        for (u32 index = 0; index < read_count; ++index) {
            if (std::chrono::steady_clock::now() > deadline) { failed = true; break; }
            state.state->halt_reason = HaltReason::None;
            const auto halt = reader.fn(state.state);
            const auto low = state.Get(80); auto high_value = state.Get(88);
            if (inject_cas_tear && index == 0) {
                high_value ^= 1;
                std::cout << "negative_control synthetic CAS reader mismatch; not a hardware observation\n";
            }
            // No oracle or opaque helper interrupts these same-block reads.
            if (halt != HaltReason::CallHost || !LegalPair(low, high_value, rounds) ||
                state.Get(64) != (low & mask) || state.Get(104) != (high_value & mask) || !restored(state)) failed = true;
            if (low && LegalTicket(low, rounds)) seen_writers.fetch_or(1u << ((low & 3) - 1));
            ++reads;
        }
        if (!restored(state)) failed = true;
    }};
    start.arrive_and_wait(); first.join(); second.join(); observer.join();
    std::cout << "software_cas128 width=" << u32(ir::GetValueSizeByte(reader_type)) << " anonymous=" << anonymous
              << " zacas=0 wins=" << wins[0] << ',' << wins[1] << " attempts=" << attempts[0] << ',' << attempts[1]
              << " reads=" << reads.load() << " seen_writer_mask=" << seen_writers.load() << '\n';
    Check(!failed, "same-lease scalar/V128 reader and softwareCAS128 pairs never tear");
    Check(wins[0] == rounds && wins[1] == rounds && reads == read_count, "both CAS writers and reader make bounded progress");
    Check(seen_writers.load() == 3, "reader validates complete values from both CAS writers");
    Check(LegalPair(memory[0], memory[1], rounds) && (memory[0] >> 2) == 2 * rounds, "both writers have no lost updates");
    Check(rv::anonymous_memory_readers.load() == 0 && backend::unaligned_atomic_lock.load() == 0,
          "all anonymous readers and software atomic gate restored after join");
}
} // namespace
} // namespace swift::tests::riscv

int main(int argc, char** argv) {
    using namespace swift::tests::riscv;
    try {
        bool narrow{}, encoding_only{}, cas_only{};
        for (int index = 1; index < argc; ++index) {
            const std::string_view arg{argv[index]};
            if (arg == "--expect-narrow") narrow = true;
            else if (arg == "--expect-strong") narrow = false;
            else if (arg == "--encoding-only") encoding_only = true;
            else if (arg == "--inject-forbidden") inject_forbidden = true;
            else if (arg == "--inject-cas-tear") inject_cas_tear = true;
            else if (arg == "--cas-only") cas_only = true;
            else throw std::runtime_error("unknown ordering test argument");
        }
        struct rlimit limit{}; getrlimit(RLIMIT_NOFILE, &limit);
        std::cout << "RLIMIT_NOFILE=" << limit.rlim_cur << '\n';
        EncodingChecks(narrow);
        if (!encoding_only) {
            if (!cas_only) { MixedObservers(); Concurrency(5000); }
            Check(Pin(0), "CAS coordinator pinned to CPU0");
            for (auto type : {T::U8, T::U16, T::U32, T::U64}) for (bool anonymous : {false, true})
                SoftwareCAS128(type, anonymous);
        }
        std::cout << "OK " << checks << " checks; finite litmus regression is not a formal memory-model proof\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
