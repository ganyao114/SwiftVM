#include "test_support.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <functional>
#include <thread>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/ir/atomic_rmw.h"

namespace swift::tests::loongarch64 {

using T = ir::ValueType;

namespace {
Runtime* abi_runtime{};
HaltReason abi_halt{};
HaltReason RunForABI(backend::State*) { return abi_halt = abi_runtime->Run(); }

void ExpectHostFault(const std::string& detail, const std::function<void()>& probe) {
    const auto child = fork();
    Check(child >= 0, "fork isolated host-fault probe");
    if (child == 0) {
        const rlimit no_core{0, 0};
        (void)setrlimit(RLIMIT_CORE, &no_core);
        probe();
        _exit(77);
    }
    int status{};
    pid_t waited{};
    do { waited = waitpid(child, &status, 0); } while (waited == -1 && errno == EINTR);
    Check(waited == child && WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS),
          detail + " status=" + std::to_string(status));
}

struct RangeFault {
    void* page{};
    u32 calls{}, fail_on{};
};
bool FaultingRangeCheck(void* context, u64, u64) {
    auto& fault = *static_cast<RangeFault*>(context);
    if (++fault.calls == fault.fail_on) return *static_cast<volatile u8*>(fault.page) != 0;
    return true;
}
}

void GuestMemory() {
    for (auto type : {T::U8, T::U16, T::U32, T::U64, T::S8, T::S16, T::S32, T::S64})
    for (bool ordered : {false, true}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x5000})};
        ir::Assembler as{block.get()};
        auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto value = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
        if (ordered) as.StoreMemoryTSO(ir::Operand{address}, value);
        else as.StoreMemory(ir::Operand{address}, value);
        auto loaded = ordered ? as.LoadMemoryTSO(ir::Operand{address}) : as.LoadMemory(ir::Operand{address});
        loaded = loaded.SetType(type);
        as.StoreUniform(ir::Uniform{16, type}, loaded);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled cached{block.get()}, uncached{block.get(), false};
        for (u64 address : {u64{0}, u64{1}, u64{7}, u64{31}, u64{4093}, u64{4095},
                            u64{8191}, UINT64_MAX, u64{8192}, u64{16384}}) {
            StateStorage actual, control, reference;
            alignas(16) std::array<u8, 8192> actual_memory, control_memory, expected_memory;
            actual_memory.fill(0xa5); control_memory.fill(0xa5); expected_memory.fill(0xa5);
            actual.state->pt = actual_memory.data(); control.state->pt = control_memory.data();
            reference.state->pt = expected_memory.data();
            for (auto* storage : {&actual, &control, &reference}) {
                storage->state->guest_addr_limit = 8192; storage->state->guest_addr_mask = 8191;
                storage->Put(0, address); storage->Put(8, 0x923456789abcdef0ULL);
            }
            backend::interp::Interpreter interpreter{*reference.state, block.get()};
            const auto halt = interpreter.Run();
            Check(cached.fn(actual.state) == halt && uncached.fn(control.state) == halt, "guest memory halt");
            Check(actual_memory == expected_memory && control_memory == expected_memory,
                  "guest mask, bounded access, cross-page, unaligned bytes and untouched neighbors");
            Check(actual.Get(16) == reference.Get(16) && control.Get(16) == reference.Get(16), "guest scalar memory value");
        }
    }
    for (u64 alignment : {u64{2}, u64{4}, u64{8}, u64{16}}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x5100})};
        ir::Assembler as{block.get()};
        auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        as.SetLocation(ir::Lambda{ir::Imm{u64{0x5120}}});
        as.CheckMemoryAlignment(address, ir::Imm{alignment - 1});
        auto value = as.LoadImm(ir::Imm{u64{42}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{8, T::U64}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled compiled{block.get()};
        for (u64 offset = 0; offset < alignment; ++offset) {
            StateStorage state; state.Put(0, 0x1000 + offset);
            const auto halt = compiled.fn(state.state);
            Check(halt == (offset ? HaltReason::PageFatal : HaltReason::CallHost),
                  "architectural alignment check halt alignment=" + std::to_string(alignment) +
                  " offset=" + std::to_string(offset) + " halt=" + std::to_string(u32(halt)) +
                  " pc=" + std::to_string(state.state->current_loc.Value()) + " value=" + std::to_string(state.Get(8)));
            Check(state.Get(8) == (offset ? 0 : 42) && state.state->current_loc.Value() == 0x5120,
                  "alignment trap keeps precise PC and prevents later effects");
        }
    }
    for (u64 mask : {u64{0}, u64{5}}) {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x5140})};
        ir::Assembler as{block.get()};
        auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        as.SetLocation(ir::Lambda{ir::Imm{u64{0x5144}}});
        as.CheckMemoryAlignment(address, ir::Imm{mask});
        auto value = as.LoadImm(ir::Imm{u64{42}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{8, T::U64}, value);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        Compiled cached{block.get()}, uncached{block.get(), false};
        for (u64 offset = 0; offset < 16; ++offset) {
            StateStorage actual, control;
            actual.Put(0, 0x1000 + offset); control.Put(0, 0x1000 + offset);
            const bool trap = (offset & mask) != 0;
            const auto actual_halt = cached.fn(actual.state), control_halt = uncached.fn(control.state);
            Check(actual_halt == (trap ? HaltReason::PageFatal : HaltReason::CallHost) && control_halt == actual_halt,
                  "zero and sparse alignment masks preserve exact bitmask semantics");
            Check(actual.Get(8) == (trap ? 0 : 42) && control.Get(8) == actual.Get(8) &&
                  actual.state->current_loc.Value() == 0x5144 && control.state->current_loc == actual.state->current_loc,
                  "zero/sparse alignment mask traps publish precise PC and suppress subsequent effects");
        }
    }
    std::cout << "PASS all memory widths, bounded windows, page crossings, TSO and architectural alignment\n";
}

void GuestAtomics() {
    for (auto type : {T::U8, T::U16, T::U32, T::U64}) {
        const u32 size = ir::GetValueSizeByte(type), bits = size * 8;
        const u64 mask = bits == 64 ? UINT64_MAX : (u64{1} << bits) - 1;
        for (u32 operation = 0; operation < 15; ++operation) {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x5200})};
            ir::Assembler as{block.get()};
            auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
            auto input = as.LoadUniform(ir::Uniform{8, type}).SetType(type);
            auto desired = as.LoadUniform(ir::Uniform{16, type}).SetType(type);
            auto carry = as.LoadUniform(ir::Uniform{24, T::U8}).SetType(T::U8);
            ir::Value result;
            if (operation == 0) result = as.AtomicExchange(address, input);
            else if (operation == 1) result = as.AtomicFetchAdd(address, input);
            else if (operation == 2) result = as.CompareAndSwap(address, input, desired);
            else result = as.AtomicRMW(ir::Imm{u64(operation - 3)}, address, input, carry);
            result = result.SetType(type);
            as.StoreUniform(ir::Uniform{32, type}, result);
            block->SetTerminal(ir::terminal::ReturnToHost{});
            Compiled cached{block.get()}, uncached{block.get(), false};
            for (u32 offset = 0; offset < 8; ++offset)
            for (u64 initial : {u64{0}, u64{1}, mask, mask >> 1, mask ^ (mask >> 1)})
            for (u32 cin = 0; cin < 2; ++cin) {
                const u64 input_value = offset & 1 ? initial : 0xf234fb67525b8395ULL & mask;
                const u64 desired_value = 0x89318a565283764bULL & mask;
                u64 wanted{};
                switch (operation) {
                    case 0: wanted = input_value; break;
                    case 1: wanted = initial + input_value; break;
                    case 2: wanted = initial == input_value ? desired_value : initial; break;
                    case 3: wanted = initial + input_value; break;
                    case 4: wanted = initial - input_value; break;
                    case 5: wanted = initial & input_value; break;
                    case 6: wanted = initial | input_value; break;
                    case 7: wanted = initial ^ input_value; break;
                    case 8: wanted = u64{0} - initial; break;
                    case 9: wanted = initial + input_value + cin; break;
                    case 10: wanted = initial - input_value - cin; break;
                    case 11: case 12: {
                        const u64 sign = u64{1} << (bits - 1);
                        const bool greater = (initial ^ sign) > (input_value ^ sign);
                        wanted = (operation == 11 ? greater : !greater) ? initial : input_value;
                        break;
                    }
                    case 13: wanted = std::max(initial, input_value); break;
                    case 14: wanted = std::min(initial, input_value); break;
                }
                wanted &= mask;
                for (auto* compiled : {&cached, &uncached}) {
                    alignas(16) std::array<u8, 32> memory;
                    memory.fill(0xa5);
                    std::memcpy(memory.data() + offset + 8, &initial, size);
                    auto expected = memory;
                    std::memcpy(expected.data() + offset + 8, &wanted, size);
                    StateStorage state;
                    state.Put(0, reinterpret_cast<u64>(memory.data() + offset + 8));
                    state.Put(8, input_value); state.Put(16, desired_value); state.Put(24, cin);
                    Check(compiled->fn(state.state) == HaltReason::CallHost, "atomic operation halt");
                    Check(state.Get(32) == initial && memory == expected,
                          "independent atomic result and neighbors: width=" + std::to_string(bits) +
                          " operation=" + std::to_string(operation) + " offset=" + std::to_string(offset));
                }
            }
        }
    }
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x5300})};
    ir::Assembler as{block.get()};
    auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto one = as.LoadImm(ir::Imm{u64{1}}).SetType(T::U64);
    (void)as.AtomicFetchAdd(address, one).SetType(T::U64);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled compiled{block.get()};
    alignas(16) u64 memory{};
    std::atomic<bool> valid{true};
    std::array<std::thread, 4> workers;
    for (auto& worker : workers) worker = std::thread{[&] {
        StateStorage state; state.Put(0, reinterpret_cast<u64>(&memory));
        for (u32 i = 0; i < 1000; ++i) {
            state.state->halt_reason = HaltReason::None;
            if (compiled.fn(state.state) != HaltReason::CallHost) valid.store(false);
        }
    }};
    for (auto& worker : workers) worker.join();
    Check(valid.load() && memory == 4000, "concurrent atomic fetch-add preserves every update actual=" +
          std::to_string(memory) + " halts_valid=" + std::to_string(valid.load()));
    // All updates fit in the low byte, so a native U64 AMO and a locked U8
    // helper must produce exactly the same mathematical count when racing.
    IntrusivePtr<ir::Block> byte_block{new ir::Block(ir::Location{0x5380})};
    ir::Assembler byte_as{byte_block.get()};
    auto byte_address = byte_as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto byte_one = byte_as.LoadImm(ir::Imm{u64{1}}).SetType(T::U8);
    (void)byte_as.AtomicFetchAdd(byte_address, byte_one).SetType(T::U8);
    byte_block->SetTerminal(ir::terminal::ReturnToHost{});
    Compiled byte_compiled{byte_block.get()};
    for (u32 round = 0; round < 16; ++round) {
        memory = 0; valid.store(true);
        for (u32 worker = 0; worker < workers.size(); ++worker) workers[worker] = std::thread{[&, worker] {
            StateStorage state; state.Put(0, reinterpret_cast<u64>(&memory));
            const auto fn = worker & 1 ? byte_compiled.fn : compiled.fn;
            for (u32 i = 0; i < 40; ++i) {
                state.state->halt_reason = HaltReason::None;
                if (fn(state.state) != HaltReason::CallHost) valid.store(false);
            }
        }};
        for (auto& worker : workers) worker.join();
        Check(valid.load() && memory == 160, "mixed native/helper atomic reader protocol serializes every update round=" +
              std::to_string(round) + " actual=" + std::to_string(memory) + " halts_valid=" + std::to_string(valid.load()));
    }
    // A guest-aligned address can have an unaligned physical mapping bias.
    alignas(16) std::array<u8, 32> biased_memory{};
    const u64 biased_initial = 0x92345678abcdef01ULL;
    std::memcpy(biased_memory.data() + 1, &biased_initial, 8);
    StateStorage biased;
    biased.state->pt = biased_memory.data() + 1; biased.state->guest_addr_limit = 16;
    biased.Put(0, 0);
    Check(compiled.fn(biased.state) == HaltReason::CallHost, "host-bias misalignment takes the atomic helper path");
    u64 biased_result{}; std::memcpy(&biased_result, biased_memory.data() + 1, 8);
    Check(biased_result == biased_initial + 1 && biased_memory[0] == 0 && biased_memory[9] == 0,
          "atomic helper uses the translated physical address and preserves neighbors");
    std::cout << "PASS atomics: all widths, 15 operations, unaligned paths, neighbors and contention\n";
}

void RuntimeFaults() {
    const auto page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto* inaccessible = mmap(nullptr, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(inaccessible != MAP_FAILED, "allocate protected guest page");
    auto config = TestConfig();
    config.loc_end = u64{1} << 48;
    backend::AddressSpace space{config};
    auto module = space.GetDefaultModule();
    Runtime runtime{&space};
    for (bool helper : {false, true}) {
        const u64 location = helper ? 0x5500 : 0x5400;
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{location})};
        ir::Assembler as{block.get()};
        auto zero = as.Zero().SetType(T::U8);
        as.SaveFlags(zero, ir::Flags::All);
        auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        as.SetLocation(ir::Lambda{ir::Imm{location + 4}});
        ir::Value result;
        if (helper) {
            auto one = as.LoadImm(ir::Imm{u64{1}}).SetType(T::U8);
            result = as.AtomicFetchAdd(address, one).SetType(T::U8);
        } else result = as.LoadMemory(ir::Operand{address}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{8, T::U64}, result);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        if (helper) {
            Compiled qualified{block.get()};
            Check(qualified.translator.Stats().helpers > 0, "protected U8 atomic uses an explicit typed semantic fallback");
        }
        Check(module->Push(block.get()) && backend::TranslateIR(module, block) != nullptr, "compile precise protected-page fault fixture");
        const u64 pointer = reinterpret_cast<u64>(inaccessible), sentinel = 0x123456789abcdef0ULL;
        std::memcpy(runtime.GetUniformBuffer().data(), &pointer, 8);
        std::memcpy(runtime.GetUniformBuffer().data() + 8, &sentinel, 8);
        const auto initial_flags = (u64{1} << 50) | (u64{1} << 29) | (u64{1} << 26);
        runtime.GetState()->host_cpu_flags = initial_flags;
        runtime.SetLocation(location); abi_runtime = &runtime;
        Check(CheckABI(&RunForABI, nullptr) == 1 && abi_halt == HaltReason::PageFatal,
              helper ? "abandoned typed atomic helper restores the full LP64D frame" : "native fault restores the full LP64D frame");
        abi_runtime = nullptr;
        u64 output{}; std::memcpy(&output, runtime.GetUniformBuffer().data() + 8, 8);
        Check(output == sentinel && runtime.GetLocation() == location + 4, "fault prevents later effects and preserves exact guest PC");
        Check(runtime.GetState()->host_cpu_flags == (initial_flags | (u64{1} << 30)), "fault preserves dirty resident flags");
        runtime.SetLocation(0xdead);
        Check(runtime.Run() == HaltReason::CodeMiss, "Runtime remains usable after native/helper fault recovery");
        u64 valid_value = 37;
        const u64 valid_pointer = reinterpret_cast<u64>(&valid_value);
        std::memcpy(runtime.GetUniformBuffer().data(), &valid_pointer, 8);
        runtime.SetLocation(location);
        Check(runtime.Run() == HaltReason::CallHost,
              "a fresh memory access reacquires the protocol after native/typed helper fault recovery");
        std::memcpy(&output, runtime.GetUniformBuffer().data() + 8, 8);
        const u64 expected_output = helper ? (sentinel & ~u64{0xff}) | 37 : 37;
        Check(output == expected_output && valid_value == (helper ? 38u : 37u),
              "fault recovery releases abandoned typed atomic locks and preserves subsequent memory effects helper=" +
              std::to_string(helper) + " output=" + std::to_string(output) +
              " expected=" + std::to_string(expected_output) + " memory=" + std::to_string(valid_value));
    }
    // An arbitrary callback fault belongs to the host. It must propagate in
    // the child instead of being misclassified as a recoverable guest access.
    IntrusivePtr<ir::Block> unknown{new ir::Block(ir::Location{0x5600})};
    ir::Assembler as{unknown.get()};
    auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    (void)as.CallHost(&SwiftLoongArchFaultHelper, address).SetType(T::U64);
    unknown->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(unknown.get()) && backend::TranslateIR(module, unknown) != nullptr,
          "compile unknown callback host-fault classification fixture");
    ExpectHostFault("unknown CallHost fault propagates to the host process instead of guest PageFatal", [&] {
        const u64 pointer = reinterpret_cast<u64>(inaccessible);
        std::memcpy(runtime.GetUniformBuffer().data(), &pointer, 8);
        runtime.SetLocation(0x5600);
        (void)runtime.Run();
    });
    ExpectHostFault("typed atomic range-validation callback fault belongs to the host", [&] {
        u64 valid = 37;
        const u64 pointer = reinterpret_cast<u64>(&valid);
        std::memcpy(runtime.GetUniformBuffer().data(), &pointer, 8);
        RangeFault fault{inaccessible, 0, 1};
        runtime.GetState()->interp_range_check = &FaultingRangeCheck;
        runtime.GetState()->interp_range_check_ctx = &fault;
        runtime.SetLocation(0x5500);
        (void)runtime.Run();
    });
    IntrusivePtr<ir::Block> copy{new ir::Block(ir::Location{0x5800})};
    ir::Assembler copying{copy.get()};
    auto destination = copying.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto source = copying.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    copying.SetLocation(ir::Lambda{ir::Imm{u64{0x5804}}});
    copying.MemoryCopy(ir::Lambda{destination}, ir::Lambda{source}, ir::Imm{u64{16}});
    auto done = copying.LoadImm(ir::Imm{u64{42}}).SetType(T::U64);
    copying.StoreUniform(ir::Uniform{64, T::U64}, done);
    copy->SetTerminal(ir::terminal::ReturnToHost{});
    Check(module->Push(copy.get()) && backend::TranslateIR(module, copy) != nullptr,
          "compile two-range typed memory-copy recovery fixture");
    for (bool destination_fault : {false, true}) {
        alignas(16) std::array<u8, 32> source_bytes{}, destination_bytes{};
        source_bytes.fill(0x6b); destination_bytes.fill(0x34);
        const u64 target = reinterpret_cast<u64>(destination_fault ? inaccessible : destination_bytes.data());
        const u64 input = reinterpret_cast<u64>(destination_fault ? source_bytes.data() : inaccessible);
        std::memcpy(runtime.GetUniformBuffer().data(), &target, 8);
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &input, 8);
        const u64 sentinel = 0x123456789abcdef0ULL;
        std::memcpy(runtime.GetUniformBuffer().data() + 64, &sentinel, 8);
        runtime.SetLocation(0x5800); abi_runtime = &runtime;
        Check(CheckABI(&RunForABI, nullptr) == 1 && abi_halt == HaltReason::PageFatal,
              "typed memory copy recovers either protected range and preserves LP64D/FCSR");
        abi_runtime = nullptr;
        u64 output{}; std::memcpy(&output, runtime.GetUniformBuffer().data() + 64, 8);
        Check(output == sentinel && runtime.GetLocation() == 0x5804,
              "typed memory-copy fault publishes precise PC and prevents later effects");
        const u64 valid_target = reinterpret_cast<u64>(destination_bytes.data());
        const u64 valid_source = reinterpret_cast<u64>(source_bytes.data());
        std::memcpy(runtime.GetUniformBuffer().data(), &valid_target, 8);
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &valid_source, 8);
        runtime.SetLocation(0x5800);
        Check(runtime.Run() == HaltReason::CallHost && std::memcmp(destination_bytes.data(), source_bytes.data(), 16) == 0 &&
              destination_bytes[16] == 0x34 && destination_bytes[31] == 0x34,
              "memory copy reacquires the protocol after recovery and preserves neighboring bytes");
    }
    for (u32 fail_on : {1u, 2u}) ExpectHostFault("memory-copy range-validation callback propagates host fault", [&] {
        alignas(16) std::array<u8, 16> target{}, source{};
        const u64 target_pointer = reinterpret_cast<u64>(target.data()), source_pointer = reinterpret_cast<u64>(source.data());
        std::memcpy(runtime.GetUniformBuffer().data(), &target_pointer, 8);
        std::memcpy(runtime.GetUniformBuffer().data() + 16, &source_pointer, 8);
        RangeFault fault{inaccessible, 0, fail_on};
        runtime.GetState()->interp_range_check = &FaultingRangeCheck;
        runtime.GetState()->interp_range_check_ctx = &fault;
        runtime.SetLocation(0x5800);
        (void)runtime.Run();
    });
    munmap(inaccessible, page_size);
    std::cout << "PASS native and typed atomic protected-page faults, LP64D/FCSR, flags, Runtime reuse and host-fault classification\n";
}

}  // namespace swift::tests::loongarch64
