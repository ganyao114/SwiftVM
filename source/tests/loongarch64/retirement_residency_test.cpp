#include "test_support.h"

#include <atomic>
#include <bit>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/loongarch64/jit/compile.h"
#include "runtime/backend/loongarch64/jit/fault_scope.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/smc_tracker.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
using Factory = std::function<IntrusivePtr<ir::Block>()>;
constexpr u32 state_bytes = 4096;

void Put(backend::State* state, u32 offset, u64 value) {
    std::memcpy(state->uniform_buffer_begin + offset, &value, sizeof(value));
}
u64 Get(const backend::State* state, u32 offset) {
    u64 value{};
    std::memcpy(&value, state->uniform_buffer_begin + offset, sizeof(value));
    return value;
}
void Seed(backend::State* state, u32 sample, u64 pc) {
    u64 random = 0x5170a6e32bd1948fULL ^ (u64{sample} * 0x9e3779b97f4a7c15ULL);
    for (u32 offset = 0; offset < state_bytes; offset += 8) {
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        Put(state, offset, sample == 0 ? 0 : sample == 1 ? UINT64_MAX : random);
    }
    state->current_loc = ir::Location{pc};
    state->prev_loc = ir::Location{pc - 4};
    state->host_cpu_flags = std::rotl(random, 17);
    state->exit_request = 0;
    state->halt_reason = HaltReason::None;
}
void Equal(const backend::State* actual, const backend::State* expected, const std::string& detail) {
    const bool equal = std::memcmp(actual->uniform_buffer_begin, expected->uniform_buffer_begin, state_bytes) == 0 &&
          actual->host_cpu_flags == expected->host_cpu_flags &&
          actual->current_loc == expected->current_loc && actual->prev_loc == expected->prev_loc &&
          actual->halt_reason == expected->halt_reason && actual->exit_request == expected->exit_request;
    if (!equal) {
        u32 first = state_bytes;
        for (u32 offset = 0; offset < state_bytes; ++offset)
            if (actual->uniform_buffer_begin[offset] != expected->uniform_buffer_begin[offset]) {
                first = offset; break;
            }
        std::cerr << "[retirement-state-difference] " << detail << " actual/expected"
                  << " flags=" << std::hex << actual->host_cpu_flags << '/' << expected->host_cpu_flags
                  << " pc=" << actual->current_loc.Value() << '/' << expected->current_loc.Value()
                  << " prev=" << actual->prev_loc.Value() << '/' << expected->prev_loc.Value()
                  << " halt=" << static_cast<u32>(actual->halt_reason) << '/'
                  << static_cast<u32>(expected->halt_reason)
                  << " request=" << actual->exit_request << '/' << expected->exit_request << std::dec
                  << " first-uniform-byte=" << first;
        if (first < state_bytes) {
            const auto word = first & ~u32{7};
            std::cerr << " word-offset=" << word << " word=" << std::hex
                      << Get(actual, word) << '/' << Get(expected, word) << std::dec;
        }
        std::cerr << '\n';
    }
    Check(equal, detail + " full 4096-byte uniform/flags/current/previous PC/halt/request");
}

struct Owner {
    std::vector<UniformRangeDesc> ranges{{0, state_bytes}};
    Config config;
    backend::AddressSpace space;
    std::shared_ptr<backend::Module> module;

    static Config Configure(Config config, std::span<UniformRangeDesc> ranges) {
        config.store_sink_uniform_ranges = ranges;
        config.global_opts = Optimizations::UniformElimination | Optimizations::XmmFaultSink |
                             Optimizations::DeadCodeRemove;
        return config;
    }
    explicit Owner(bool residency, Config requested = TestConfig(), bool read_only = true)
            : config(Configure(requested, ranges)), space(config) {
        backend::ModuleConfig policy;
        policy.read_only = read_only;
        policy.optimizations = config.global_opts;
        policy.feature_overrides.Set(FeatureId::uniform_elim, true);
        policy.feature_overrides.Set(FeatureId::uniform_dse, true);
        policy.feature_overrides.Set(FeatureId::xmm_fault_sink, true);
        policy.feature_overrides.Set(FeatureId::loongarch_uniform_forwarding, false);
        policy.feature_overrides.Set(FeatureId::loongarch_uniform_residency, residency);
        // Map the module so ordinary Runtime signal recovery can resolve its
        // fault table; an unattached synthetic Module would test only calls.
        module = space.MapModule(config.loc_start, config.loc_end, policy);
        Check(module && ResolveFeatureSet(module->GetModuleConfig()).loongarch_uniform_residency == residency,
              "production module selects the requested residency ON/OFF policy");
    }
    BlockFn Compile(ir::Block* block) {
        Check(module->Push(block), "residency fixture publishes a fresh production source");
        void* entry{};
        { auto lock = block->LockWrite(); entry = la::CompileBlock(module, block); }
        Check(entry && block->GetJitCache().jit_state == backend::JitState::Cached,
              "residency fixture executes published CompileBlock code");
        return reinterpret_cast<BlockFn>(entry);
    }
};

struct Unit {
    Owner owner;
    IntrusivePtr<ir::Block> source;
    BlockFn fn;
    la::UniformValueStats traffic{};
    la::EmissionStats emission{};
    u32 code_bytes{}, checkpoint_values{};

    Unit(const Factory& make, bool residency)
            : owner(residency), source(make()), fn(owner.Compile(source.get())) {
        code_bytes = source->GetJitCache().cache_size.get<u32>();
        la::JitContext context;
        context.ConfigureModule(owner.module.get());
        la::JitTranslator translator{context};
        translator.Translate(source.get());
        traffic = context.UniformTraffic(); emission = translator.Stats();
        for (auto& inst : translator.EmittedIR()->GetInstList())
            if (inst.GetOp() == O::Checkpoint)
                for (const auto& param : inst.GetArg<ir::Params>(0))
                    checkpoint_values += param.data.IsValue();
        Check(context.CurrentBufferSize() == code_bytes && !context.UsesUniformForwarding(),
              "residency metrics describe the production code with backend forwarding disabled");
    }
};

void Compare(const Factory& make, const std::string& detail, u64 final_pc, u64 previous_pc,
             u32 samples = 32) {
    Unit on{make, true}, off{make, false};
    auto raw = make();
    for (u32 sample = 0; sample < samples; ++sample) {
        StateStorage actual, control, reference;
        for (auto* storage : {&actual, &control, &reference})
            Seed(storage->state, sample, raw->GetStartLocation().Value());
        backend::interp::Interpreter interpreter{*reference.state, raw.get()};
        const auto reason = interpreter.Run();
        // The generic interpreter treats AdvancePC as source metadata. Its
        // byte/flag oracle is independent; expected guest locations are derived
        // from the fixture's decoded instruction positions, not native State.
        reference.state->current_loc = ir::Location{final_pc};
        reference.state->prev_loc = ir::Location{previous_pc};
        Check(on.fn(actual.state) == reason && off.fn(control.state) == reason,
              detail + " native production ON/OFF match the interpreter reason");
        Equal(actual.state, reference.state, detail + " ON");
        Equal(control.state, reference.state, detail + " OFF");
        if (sample < 2) {
            StateStorage abi_on, abi_off;
            Seed(abi_on.state, sample, raw->GetStartLocation().Value());
            Seed(abi_off.state, sample, raw->GetStartLocation().Value());
            Check(CheckABI(on.fn, abi_on.state) == 1 && CheckABI(off.fn, abi_off.state) == 1,
                  detail + " retirement residency preserves LP64D");
            Equal(abi_on.state, reference.state, detail + " ABI ON");
            Equal(abi_off.state, reference.state, detail + " ABI OFF");
        }
    }
}

IntrusivePtr<ir::Block> ManyGuests() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc100})};
    ir::Assembler as{block.get()};
    auto vector = as.LoadUniform(ir::Uniform{512, T::V128}).SetType(T::V128);
    for (u64 i = 0; i < 16; ++i) {
        auto before = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto next = as.Add(before, ir::Operand{ir::Imm{i + 1}}).SetType(T::U64);
        as.SaveFlags(next, ir::Flags::All);
        as.StoreUniform(ir::Uniform{0, T::U64}, next);
        as.StoreUniform(ir::Uniform{32, T::V128}, vector);
        auto low = as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128);
        as.StoreUniform(ir::Uniform{64, T::V128}, low);
        as.AdvancePC(ir::Imm{u32{4}});
    }
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

void StraightLineAndCost() {
    Unit on{ManyGuests, true}, off{ManyGuests, false};
    Check(on.emission.bytes[size_t(O::AdvancePC)] == 0 &&
          off.emission.bytes[size_t(O::AdvancePC)] == 0,
          "AdvancePC is a zero-byte compile-time cursor in both residency policies");
    Check(on.emission.checkpoints == 16 && off.emission.checkpoints == 16 &&
          on.checkpoint_values == 16 * 3 && off.checkpoint_values == 0,
          "explicit checkpoints retain side-exit SSA snapshots without eager OFF publication");
    Check(on.emission.hot_pc_stores == 2 && off.emission.hot_pc_stores == 2,
          "sixteen retirements publish current/previous PC only at the ordinary unit exit");
    Check(on.traffic.stores < off.traffic.stores && on.traffic.loads < off.traffic.loads &&
          on.emission.cold_uniform_stores > 0,
          "residency reduces actual hot uniform loads/stores while accounting cold stores separately");
    Compare(ManyGuests, "sixteen scalar/vector guest retirements", 0xc140, 0xc13c, 48);

    const auto timed = [](Unit& unit) {
        StateStorage storage; Seed(storage.state, 3, 0xc100);
        const auto start = std::chrono::steady_clock::now();
        for (u32 iteration = 0; iteration < 256; ++iteration) {
            storage.state->halt_reason = HaltReason::None;
            storage.state->current_loc = ir::Location{0xc100};
            storage.state->prev_loc = ir::Location{0xc0fc};
            (void)unit.fn(storage.state);
        }
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    };
    const auto on_ns = timed(on), off_ns = timed(off);
    std::cout << "[loongarch-retirement-residency] hot-loads=" << on.traffic.loads << '/' << off.traffic.loads
              << " hot-stores=" << on.traffic.stores << '/' << off.traffic.stores
              << " checkpoint-stores=" << on.emission.cold_uniform_stores
              << " pc-stores-static=" << on.emission.pc_stores << '/' << off.emission.pc_stores
              << " hot-pc-stores=" << on.emission.hot_pc_stores << '/' << off.emission.hot_pc_stores
              << " marker-pc-stores=0/legacy32 code-bytes=" << on.code_bytes << '/' << off.code_bytes
              << " execution-ns=" << on_ns << '/' << off_ns << '\n';
}

backend::State* FromUniform(u64 address) {
    return reinterpret_cast<backend::State*>(address - backend::state_offset_uniform_buffer);
}
u64 RequestAndMutate(u64 address, u64 replacement, u64 request) {
    auto* state = FromUniform(address);
    const auto old = Get(state, 0);
    Put(state, 384, old);
    Put(state, 392, state->current_loc.Value());
    Put(state, 400, state->prev_loc.Value());
    Put(state, 0, replacement);
    Put(state, 32, 0x0123456789abcdefULL);
    Put(state, 40, 0xfedcba9876543210ULL);
    std::atomic_ref{state->exit_request}.store(request, std::memory_order_release);
    return old;
}

Factory RequestingGuest(u64 request) {
    return [=] {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc200})};
        ir::Assembler as{block.get()};
        auto address = as.GetUniformAddress(ir::Imm{u32{0}}).SetType(T::U64);
        auto input = as.LoadUniform(ir::Uniform{512, T::U64}).SetType(T::U64);
        auto replacement = as.LoadUniform(ir::Uniform{520, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, as.Add(input, ir::Operand{ir::Imm{u64{3}}}).SetType(T::U64));
        as.AdvancePC(ir::Imm{u32{4}});
        auto next = as.Add(as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64),
                           ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, next);
        as.AdvancePC(ir::Imm{u32{4}});
        auto result = as.CallHost(&RequestAndMutate, address, replacement,
                                 as.LoadImm(ir::Imm{request}).SetType(T::U64)).SetType(T::U64);
        as.StoreUniform(ir::Uniform{8, T::U64}, result);
        auto modified = as.Add(as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64),
                               ir::Operand{ir::Imm{u64{9}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, modified);
        as.StoreUniform(ir::Uniform{16, T::U64}, modified);
        as.AdvancePC(ir::Imm{u32{4}});
        as.StoreUniform(ir::Uniform{0, T::U64}, as.LoadImm(ir::Imm{u64{0xbad}}).SetType(T::U64));
        as.StoreUniform(ir::Uniform{32, T::U64}, next);
        as.AdvancePC(ir::Imm{u32{4}});
        block->SetTerminal(ir::terminal::ReturnToHost{});
        return block;
    };
}

void RequestedSideExits() {
    for (u64 request : {u64{1}, u64{1} << 63, (u64{1} << 63) | 3}) {
        const auto make = RequestingGuest(request);
        Unit on{make, true}, off{make, false};
        for (u32 sample = 0; sample < 32; ++sample) {
            StateStorage actual, control, expected;
            for (auto* storage : {&actual, &control, &expected}) Seed(storage->state, sample, 0xc200);
            const auto original = expected.Get(512) + 10;
            const auto modified = expected.Get(520) + 9;
            expected.Put(0, modified); expected.Put(8, original); expected.Put(16, modified);
            expected.Put(32, 0x0123456789abcdefULL); expected.Put(40, 0xfedcba9876543210ULL);
            expected.Put(384, original); expected.Put(392, 0xc208); expected.Put(400, 0xc204);
            expected.state->current_loc = ir::Location{0xc20c};
            expected.state->prev_loc = ir::Location{0xc208};
            expected.state->exit_request = request;
            const auto reason = request >> 63 ? HaltReason::Signal : HaltReason::CodeMiss;
            expected.state->halt_reason = reason;
            Check(on.fn(actual.state) == reason && off.fn(control.state) == reason,
                  "helper request stops at the following checkpoint after all retiring guest effects");
            Equal(actual.state, expected.state, "requested snapshot ON");
            Equal(control.state, expected.state, "requested snapshot OFF");
            if (sample < 2) {
                StateStorage abi_on, abi_off;
                Seed(abi_on.state, sample, 0xc200); Seed(abi_off.state, sample, 0xc200);
                Check(CheckABI(on.fn, abi_on.state) == 1 && CheckABI(off.fn, abi_off.state) == 1,
                      "snapshot Signal/SMC exits preserve LP64D");
                Equal(abi_on.state, expected.state, "requested snapshot ABI ON");
                Equal(abi_off.state, expected.state, "requested snapshot ABI OFF");
            }
        }
    }
}

IntrusivePtr<ir::Block> WideRequestedGuest() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc280})};
    ir::Assembler as{block.get()};
    auto address = as.GetUniformAddress(ir::Imm{u32{0}}).SetType(T::U64);
    auto wide = as.LoadUniform(ir::Uniform{512, T::V256}).SetType(T::V256);
    auto high = as.LoadUniform(ir::Uniform{560, T::V128}).SetType(T::V128);
    auto replacement = as.LoadUniform(ir::Uniform{520, T::U64}).SetType(T::U64);
    as.AdvancePC(ir::Imm{u32{4}});
    (void)as.CallHost(&RequestAndMutate, address, replacement,
                     as.LoadImm(ir::Imm{u64{1} << 63}).SetType(T::U64)).SetType(T::U64);
    as.StoreUniform(ir::Uniform{96, T::V256}, wide);
    as.StoreUniform(ir::Uniform{104, T::U64},
                    as.LoadImm(ir::Imm{u64{0x5a1032b4c6d8e9f0}}).SetType(T::U64));
    as.StoreUniform(ir::Uniform{112, T::V128}, high);
    as.AdvancePC(ir::Imm{u32{4}});
    as.StoreUniform(ir::Uniform{96, T::U64}, as.LoadImm(ir::Imm{u64{0xbad}}).SetType(T::U64));
    as.AdvancePC(ir::Imm{u32{4}});
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

void WideRequestedSnapshots() {
    Unit on{WideRequestedGuest, true}, off{WideRequestedGuest, false};
    bool source_wide{};
    for (auto& inst : on.source->GetInstList()) if (inst.GetOp() == O::Checkpoint)
        for (const auto& param : inst.GetArg<ir::Params>(0))
            source_wide |= param.data.IsValue() && param.data.value.Type() == T::V256;
    Check(source_wide, "retirement snapshot pins the original partially overwritten V256 store");
    for (u32 sample = 0; sample < 32; ++sample) {
        StateStorage actual, control, expected;
        for (auto* storage : {&actual, &control, &expected}) Seed(storage->state, sample, 0xc280);
        expected.Put(384, expected.Get(0));
        expected.Put(392, 0xc284); expected.Put(400, 0xc280);
        expected.Put(0, expected.Get(520));
        expected.Put(32, 0x0123456789abcdefULL); expected.Put(40, 0xfedcba9876543210ULL);
        std::memcpy(expected.state->uniform_buffer_begin + 96,
                    expected.state->uniform_buffer_begin + 512, 32);
        expected.Put(104, 0x5a1032b4c6d8e9f0);
        std::memcpy(expected.state->uniform_buffer_begin + 112,
                    expected.state->uniform_buffer_begin + 560, 16);
        expected.state->current_loc = ir::Location{0xc288};
        expected.state->prev_loc = ir::Location{0xc284};
        expected.state->exit_request = u64{1} << 63;
        expected.state->halt_reason = HaltReason::Signal;
        Check(on.fn(actual.state) == HaltReason::Signal && off.fn(control.state) == HaltReason::Signal,
              "V256/V128/scalar overlap retires before the requested side exit");
        Equal(actual.state, expected.state, "wide snapshot ON");
        Equal(control.state, expected.state, "wide snapshot OFF");
        if (sample < 2) {
            StateStorage abi_on, abi_off;
            Seed(abi_on.state, sample, 0xc280); Seed(abi_off.state, sample, 0xc280);
            Check(CheckABI(on.fn, abi_on.state) == 1 && CheckABI(off.fn, abi_off.state) == 1,
                  "wide overlapping retirement snapshots preserve LP64D");
            Equal(abi_on.state, expected.state, "wide snapshot ABI ON");
            Equal(abi_off.state, expected.state, "wide snapshot ABI OFF");
        }
    }
}

bool ChangingOracle(void* opaque, u64, u64) {
    auto* state = static_cast<backend::State*>(opaque);
    Put(state, 416, Get(state, 0));
    Put(state, 424, state->current_loc.Value());
    Put(state, 432, state->prev_loc.Value());
    Put(state, 0, Get(state, 520));
    Put(state, 32, 0x9182736455463728ULL);
    return true;
}
bool RejectingOracle(void* opaque, u64 guest, u64 size) {
    (void)ChangingOracle(opaque, guest, size);
    return false;
}

Factory ObservingMemory(bool after_fault = false) {
    return [=] {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xc300})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{512, T::U64}).SetType(T::U64);
        auto vector = as.LoadUniform(ir::Uniform{528, T::V128}).SetType(T::V128);
        as.StoreUniform(ir::Uniform{0, T::U64}, as.Add(input, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64));
        as.StoreUniform(ir::Uniform{32, T::V128}, vector);
        as.AdvancePC(ir::Imm{u32{4}});
        auto next = as.Add(as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64),
                           ir::Operand{ir::Imm{u64{2}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, next);
        as.AdvancePC(ir::Imm{u32{4}});
        auto loaded = as.LoadMemory(ir::Operand{ir::Imm{u64{0}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{16, T::U64}, loaded);
        auto changed = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{8, T::U64}, changed);
        as.AdvancePC(ir::Imm{u32{4}});
        as.StoreUniform(ir::Uniform{0, T::U64}, as.Add(changed, ir::Operand{ir::Imm{u64{5}}}).SetType(T::U64));
        if (after_fault) as.StoreUniform(ir::Uniform{48, T::V128}, vector);
        as.AdvancePC(ir::Imm{u32{4}});
        block->SetTerminal(ir::terminal::ReturnToHost{});
        return block;
    };
}

void OracleAndBoundedFaults() {
    std::array<u64, 4> memory{0xfeedabcddcba1234ULL, 0, 0, 0};
    for (u32 mode = 0; mode < 3; ++mode) {
        const auto make = ObservingMemory(mode != 0);
        Unit on{make, true}, off{make, false};
        auto raw = make();
        for (u32 sample = 0; sample < 24; ++sample) {
            StateStorage actual, control, reference;
            for (auto* storage : {&actual, &control, &reference}) {
                Seed(storage->state, sample, 0xc300);
                storage->state->pt = reinterpret_cast<u8*>(memory.data());
                storage->state->guest_addr_mask = UINT64_MAX;
                storage->state->guest_addr_limit = mode == 1 ? 0 : sizeof(memory);
                storage->state->interp_range_check = mode == 0 ? ChangingOracle : mode == 2 ? RejectingOracle : nullptr;
                storage->state->interp_range_check_ctx = storage->state;
            }
            backend::interp::Interpreter interpreter{*reference.state, raw.get()};
            const auto reason = interpreter.Run();
            if (mode != 1) {
                reference.Put(424, 0xc308); reference.Put(432, 0xc304);
            }
            reference.state->current_loc = ir::Location{mode ? 0xc308u : 0xc310u};
            reference.state->prev_loc = ir::Location{mode ? 0xc304u : 0xc30cu};
            Check(on.fn(actual.state) == reason && off.fn(control.state) == reason,
                  "returning/rejecting oracle and bounded fault retain exact guest effects");
            Equal(actual.state, reference.state, "oracle/fault ON");
            Equal(control.state, reference.state, "oracle/fault OFF");
            if (sample < 2) {
                StateStorage abi_on, abi_off;
                for (auto* storage : {&abi_on, &abi_off}) {
                    Seed(storage->state, sample, 0xc300);
                    storage->state->pt = reinterpret_cast<u8*>(memory.data());
                    storage->state->guest_addr_mask = UINT64_MAX;
                    storage->state->guest_addr_limit = mode == 1 ? 0 : sizeof(memory);
                    storage->state->interp_range_check = mode == 0 ? ChangingOracle : mode == 2 ? RejectingOracle : nullptr;
                    storage->state->interp_range_check_ctx = storage->state;
                }
                Check(CheckABI(on.fn, abi_on.state) == 1 && CheckABI(off.fn, abi_off.state) == 1,
                      "oracle continuation and synchronous faults preserve LP64D");
                Equal(abi_on.state, reference.state, "oracle/fault ABI ON");
                Equal(abi_off.state, reference.state, "oracle/fault ABI OFF");
            }
        }
    }
}

struct Mapping {
    const size_t page{static_cast<size_t>(getpagesize())}, size{page * 4};
    u8* bytes{static_cast<u8*>(mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))};
    Mapping() { Check(bytes != MAP_FAILED, "allocate residency guest memory mapping"); }
    ~Mapping() { if (bytes != MAP_FAILED) (void)munmap(bytes, size); }
    Config Configuration(bool dynamic = false) const {
        auto config = TestConfig();
        config.loc_end = size;
        config.memory_base = bytes;
        config.guest_addr_mask = size - 1;
        config.static_program = !dynamic;
        return config;
    }
};
thread_local Runtime* runtime_for_abi{};
thread_local HaltReason runtime_reason{};
HaltReason RunRuntimeForABI(backend::State*) { return runtime_reason = runtime_for_abi->Run(); }

void Cleanup(backend::State* state) {
    Check(state->spill_area[backend::kRiscvRecoveryPcSlot] == 0 &&
          state->spill_area[backend::kRiscvRecoveryFrameSlot] == 0 &&
          state->spill_area[backend::kRiscvMemoryOwnedSlot] == 0 &&
          la::GetGuestFaultKind(state) == la::GuestFaultKind::None,
          "residency side-exit clears recovery anchors, guest scope and reader lease");
}

u64 InterruptRuntime(u64 address, u64 replacement) {
    auto* state = FromUniform(address);
    const auto old = Get(state, 0);
    Put(state, 800, old);
    Put(state, 808, state->current_loc.Value());
    Put(state, 816, state->prev_loc.Value());
    Put(state, 0, replacement);
    Put(state, 32, 0x1948576a3b2c0dfeULL);
    runtime_for_abi->SignalInterrupt();
    return old;
}

void ActualSignalRequests() {
    for (bool residency : {false, true}) for (u32 sample = 0; sample < 8; ++sample) {
        Mapping mapping;
        Owner owner{residency, mapping.Configuration()};
        Runtime runtime{&owner.space};
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x100})};
        ir::Assembler as{block.get()};
        auto address = as.GetUniformAddress(ir::Imm{u32{0}}).SetType(T::U64);
        auto input = as.LoadUniform(ir::Uniform{512, T::U64}).SetType(T::U64);
        auto replacement = as.LoadUniform(ir::Uniform{520, T::U64}).SetType(T::U64);
        auto first = as.Add(input, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, first);
        as.AdvancePC(ir::Imm{u32{4}});
        auto prior = as.CallHost(&InterruptRuntime, address, replacement).SetType(T::U64);
        auto final = as.Add(as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64),
                            ir::Operand{ir::Imm{u64{2}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, final);
        as.StoreUniform(ir::Uniform{8, T::U64}, prior);
        as.AdvancePC(ir::Imm{u32{4}});
        as.StoreUniform(ir::Uniform{96, T::U64}, as.LoadImm(ir::Imm{u64{0xbad}}).SetType(T::U64));
        as.AdvancePC(ir::Imm{u32{4}});
        block->SetTerminal(ir::terminal::ReturnToHost{});
        (void)owner.Compile(block.get());
        StateStorage expected;
        Seed(runtime.GetState(), sample, 0x100); Seed(expected.state, sample, 0x100);
        const auto retired = expected.Get(512) + 1;
        expected.Put(0, expected.Get(520) + 2); expected.Put(8, retired);
        expected.Put(32, 0x1948576a3b2c0dfeULL);
        expected.Put(800, retired); expected.Put(808, 0x104); expected.Put(816, 0x100);
        expected.state->current_loc = ir::Location{0x108};
        expected.state->prev_loc = ir::Location{0x104};
        expected.state->exit_request = u64{1} << 63;
        runtime_for_abi = &runtime;
        Check(CheckABI(RunRuntimeForABI, nullptr) == 1 && runtime_reason == HaltReason::Signal,
              "Runtime SignalInterrupt retires the requesting guest before stopping with LP64D intact");
        Equal(runtime.GetState(), expected.state, "real Runtime signal retirement");
        Cleanup(runtime.GetState());
        Check(CheckABI(RunRuntimeForABI, nullptr) == 1 && runtime_reason == HaltReason::Signal,
              "Runtime signal stays sticky across Run without executing another guest");
        Equal(runtime.GetState(), expected.state, "sticky Runtime signal");
        runtime.ClearInterrupt();
        expected.state->exit_request = 0;
        Equal(runtime.GetState(), expected.state, "ClearInterrupt acknowledges the sticky request");
        Cleanup(runtime.GetState());
        runtime_for_abi = nullptr;
    }
}

void ProtectedNativeAndTypedFaults() {
    for (bool residency : {false, true}) for (bool typed : {false, true}) {
        Mapping mapping;
        Check(mprotect(mapping.bytes + mapping.page, mapping.page, PROT_NONE) == 0,
              "protect the residency fault data page");
        Owner owner{residency, mapping.Configuration()};
        Runtime runtime{&owner.space};
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x100})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{512, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, as.Add(input, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64));
        as.AdvancePC(ir::Imm{u32{4}});
        auto latest = as.Add(as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64),
                             ir::Operand{ir::Imm{u64{2}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, latest);
        as.AdvancePC(ir::Imm{u32{4}});
        auto address = as.LoadImm(ir::Imm{u64(mapping.page)}).SetType(T::U64);
        auto loaded = typed ? as.AtomicFetchAdd(address, as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)).SetType(T::U8) :
                              as.LoadMemory(ir::Operand{address}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{16, loaded.Type()}, loaded);
        as.StoreUniform(ir::Uniform{0, T::U64}, as.LoadImm(ir::Imm{u64{0xbad}}).SetType(T::U64));
        as.AdvancePC(ir::Imm{u32{4}});
        block->SetTerminal(ir::terminal::ReturnToHost{});
        (void)owner.Compile(block.get());
        for (u32 sample = 0; sample < 3; ++sample) {
            StateStorage expected;
            Seed(runtime.GetState(), sample, 0x100); Seed(expected.state, sample, 0x100);
            expected.Put(0, expected.Get(512) + 3);
            expected.state->current_loc = ir::Location{0x108};
            expected.state->prev_loc = ir::Location{0x104};
            runtime_for_abi = &runtime;
            Check(CheckABI(RunRuntimeForABI, nullptr) == 1 && runtime_reason == HaltReason::PageFatal,
                  typed ? "typed helper fault restores LP64D after deferred retirements" :
                          "native access fault restores LP64D after deferred retirements");
            runtime_for_abi = nullptr;
            Equal(runtime.GetState(), expected.state, typed ? "protected typed fault" : "protected native fault");
            Cleanup(runtime.GetState());
        }
    }
}

BlockFn Diamond(Owner& owner) {
    BlockFn fn{};
    {
        ir::HIRBuilder builder{1, true};
        auto* function = builder.AppendFunction(ir::Location{0xc400});
        auto input = function->LoadUniform<ir::U64>(ir::Uniform{512, T::U64});
        function->StoreUniform(ir::Uniform{0, T::U64},
                              function->Add<ir::U64>(input, ir::Operand{ir::Imm{u64{1}}}));
        function->AdvancePC(ir::Imm{u32{4}});
        function->StoreUniform(ir::Uniform{0, T::U64},
                              function->Add<ir::U64>(input, ir::Operand{ir::Imm{u64{3}}}));
        function->AdvancePC(ir::Imm{u32{4}});
        auto condition = function->TestZero<ir::U8>(input);
        builder.If(ir::terminal::If{ir::BOOL{condition}, ir::terminal::LinkBlock{ir::Location{0xc500}},
                                   ir::terminal::LinkBlock{ir::Location{0xc600}}});
        builder.SetCurBlock(ir::Location{0xc500});
        auto left = function->Add<ir::U64>(input, ir::Operand{ir::Imm{u64{5}}});
        function->StoreUniform(ir::Uniform{0, T::U64}, left);
        function->AdvancePC(ir::Imm{u32{4}});
        auto left_result = function->Add<ir::U64>(function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64}),
                                                ir::Operand{ir::Imm{u64{7}}});
        function->StoreUniform(ir::Uniform{8, T::U64}, left_result);
        function->AdvancePC(ir::Imm{u32{4}});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xc700}});
        builder.SetCurBlock(ir::Location{0xc600});
        auto right = function->Sub<ir::U64>(input, ir::Operand{ir::Imm{u64{11}}});
        function->StoreUniform(ir::Uniform{0, T::U64}, right);
        function->AdvancePC(ir::Imm{u32{4}});
        auto right_result = function->Mul<ir::U64>(function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64}),
                                                 ir::Operand{ir::Imm{u64{3}}});
        function->StoreUniform(ir::Uniform{8, T::U64}, right_result);
        function->AdvancePC(ir::Imm{u32{4}});
        builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xc700}});
        builder.SetCurBlock(ir::Location{0xc700});
        ir::Params inputs; inputs.Push(left_result); inputs.Push(right_result);
        auto phi = function->AddPhi<ir::U64>(inputs);
        function->StoreUniform(ir::Uniform{16, T::U64}, phi);
        function->AdvancePC(ir::Imm{u32{4}});
        auto selected = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
        function->StoreUniform(ir::Uniform{24, T::U64}, selected);
        function->StoreUniform(ir::Uniform{0, T::U64},
                              function->Add<ir::U64>(selected, ir::Operand{ir::Imm{u64{17}}}));
        function->AdvancePC(ir::Imm{u32{4}});
        function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
        fn = reinterpret_cast<BlockFn>(la::CompileFunctions(owner.module, std::span{&function, 1}));
        Check(fn != nullptr, "production residency compiles whole-function diamond/phi");
    }
    return fn;
}

BlockFn Loop(Owner& owner) {
    ir::HIRBuilder builder{1, true};
    auto* function = builder.AppendFunction(ir::Location{0xc800});
    auto initial = function->LoadUniform<ir::U64>(ir::Uniform{512, T::U64});
    builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xc900}});
    builder.SetCurBlock(ir::Location{0xc900});
    ir::Params inputs; inputs.Push(initial); inputs.Push(initial);
    auto count = function->AddPhi<ir::U64>(inputs);
    auto next = function->Sub<ir::U64>(count, ir::Operand{ir::Imm{u64{1}}});
    ir::Params feedback; feedback.Push(initial); feedback.Push(next);
    count.Def()->SetArg(0, feedback);
    function->StoreUniform(ir::Uniform{0, T::U64}, next);
    function->AdvancePC(ir::Imm{u32{4}});
    function->StoreUniform(ir::Uniform{8, T::U64}, function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64}));
    function->AdvancePC(ir::Imm{u32{4}});
    auto remaining = function->TestNotZero<ir::U8>(next);
    builder.If(ir::terminal::If{ir::BOOL{remaining}, ir::terminal::LinkBlock{ir::Location{0xc900}},
                               ir::terminal::LinkBlock{ir::Location{0xca00}}});
    builder.SetCurBlock(ir::Location{0xca00});
    function->StoreUniform(ir::Uniform{16, T::U64}, count);
    function->AdvancePC(ir::Imm{u32{4}});
    function->EndBlock(ir::terminal::ReturnToHost{}); function->EndFunction();
    auto fn = reinterpret_cast<BlockFn>(la::CompileFunctions(owner.module, std::span{&function, 1}));
    Check(fn != nullptr, "production residency compiles loop-carried SSA with repeated static checkpoints");
    return fn;
}

void FunctionPaths() {
    Owner on{true}, off{false};
    const auto on_diamond = Diamond(on), off_diamond = Diamond(off);
    const auto on_loop = Loop(on), off_loop = Loop(off);
    for (u32 sample = 0; sample < 32; ++sample) {
        StateStorage actual, control, expected;
        for (auto* storage : {&actual, &control, &expected}) Seed(storage->state, sample, 0xc400);
        const auto input = expected.Get(512), selected = input == 0 ? input + 5 : input - 11;
        const auto result = input == 0 ? selected + 7 : selected * 3;
        expected.Put(0, selected + 17); expected.Put(8, result);
        expected.Put(16, result); expected.Put(24, selected);
        expected.state->current_loc = ir::Location{0xc708};
        expected.state->prev_loc = ir::Location{0xc704};
        expected.state->halt_reason = HaltReason::CallHost;
        Check(on_diamond(actual.state) == HaltReason::CallHost && off_diamond(control.state) == HaltReason::CallHost,
              "CFG labels/phi select the correct path through several guest retirements");
        Equal(actual.state, expected.state, "residency diamond ON");
        Equal(control.state, expected.state, "residency diamond OFF");
        if (sample < 2) {
            StateStorage abi_on, abi_off;
            Seed(abi_on.state, sample, 0xc400); Seed(abi_off.state, sample, 0xc400);
            Check(CheckABI(on_diamond, abi_on.state) == 1 && CheckABI(off_diamond, abi_off.state) == 1,
                  "CFG snapshots survive builder destruction and preserve LP64D");
            Equal(abi_on.state, expected.state, "residency diamond ABI ON");
            Equal(abi_off.state, expected.state, "residency diamond ABI OFF");
        }
    }
    for (u64 count : {u64{1}, u64{2}, u64{17}, u64{129}}) {
        StateStorage actual, control, expected;
        for (auto* storage : {&actual, &control, &expected}) {
            Seed(storage->state, u32(count), 0xc800); storage->Put(512, count);
        }
        expected.Put(0, 0); expected.Put(8, 0); expected.Put(16, 1);
        expected.state->current_loc = ir::Location{0xca04};
        expected.state->prev_loc = ir::Location{0xca00};
        expected.state->halt_reason = HaltReason::CallHost;
        Check(on_loop(actual.state) == HaltReason::CallHost && off_loop(control.state) == HaltReason::CallHost,
              "backedge resets the real guest cursor and retains fresh loop-carried snapshots");
        Equal(actual.state, expected.state, "residency backedge ON");
        Equal(control.state, expected.state, "residency backedge OFF");
    }
}

void RealSMC() {
    struct Restore {
        bool prior{backend::SmcTracker::IsEnabled()};
        ~Restore() { backend::SmcTracker::SetEnabled(prior); }
    } restore;
    backend::SmcTracker::SetEnabled(true);
    for (bool residency : {false, true}) for (u32 sample : {0u, 2u}) {
        Mapping mapping;
        const u64 target = mapping.page + 0x100;
        Owner owner{residency, mapping.Configuration(true), false};
        Runtime runtime{&owner.space};
        IntrusivePtr<ir::Block> old{new ir::Block(ir::Location{target})};
        old->SetTerminal(ir::terminal::ReturnToHost{});
        (void)owner.Compile(old.get());
        Check(owner.space.GetSmcTracker().HasProtectedPages(), "SMC residency target page is write protected");
        IntrusivePtr<ir::Block> writer{new ir::Block(ir::Location{0x100})};
        ir::Assembler as{writer.get()};
        auto input = as.LoadUniform(ir::Uniform{512, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, as.Add(input, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64));
        as.AdvancePC(ir::Imm{u32{4}});
        auto prior = as.Add(as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64),
                            ir::Operand{ir::Imm{u64{2}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, prior);
        as.AdvancePC(ir::Imm{u32{4}});
        auto word = as.LoadUniform(ir::Uniform{800, T::U32}).SetType(T::U32);
        as.StoreMemory(ir::Operand{ir::Imm{target}}, word);
        auto latest = as.Add(prior, ir::Operand{ir::Imm{u64{4}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, latest);
        as.StoreUniform(ir::Uniform{16, T::U64}, latest);
        as.AdvancePC(ir::Imm{u32{4}});
        as.StoreUniform(ir::Uniform{0, T::U64}, as.LoadImm(ir::Imm{u64{0xbad}}).SetType(T::U64));
        writer->SetTerminal(ir::terminal::ReturnToHost{});
        (void)owner.Compile(writer.get());
        StateStorage expected;
        Seed(runtime.GetState(), sample, 0x100); Seed(expected.state, sample, 0x100);
        Put(runtime.GetState(), 800, 0xaabbccdd); expected.Put(800, 0xaabbccdd);
        expected.Put(0, expected.Get(512) + 7); expected.Put(16, expected.Get(0));
        expected.state->current_loc = ir::Location{0x10c}; expected.state->prev_loc = ir::Location{0x108};
        // One target owns the dirty page. HandleWriteFault clears its slots
        // and publishes generation 1, then TakeDirtyNodes clears that same
        // target while closing the write window and publishes generation 2.
        // The Runtime captured generation 1 before closing; its exact-token
        // CAS cannot acknowledge the newer generation. This independent
        // model follows the two known publications, never native State.
        constexpr u64 dirty_targets = 1;
        expected.state->exit_request = dirty_targets * 2;
        runtime_for_abi = &runtime;
        Check(CheckABI(RunRuntimeForABI, nullptr) == 1 && runtime_reason == HaltReason::CodeMiss,
              "real SMC write retries its access, retires once, and exits through the checkpoint preserving LP64D");
        runtime_for_abi = nullptr;
        Equal(runtime.GetState(), expected.state, "real SMC deferred retirement " +
              std::string(residency ? "ON" : "OFF") + " sample=" + std::to_string(sample));
        Cleanup(runtime.GetState());
        u32 stored{}; std::memcpy(&stored, mapping.bytes + target, sizeof(stored));
        Check(stored == 0xaabbccdd && owner.space.GetCodeCache(target) == nullptr &&
              backend::IsEmpty(owner.module->GetNode(ir::Location{target})),
              "SMC invalidation closes the real write window and removes the stale target");
        runtime_for_abi = &runtime;
        Check(CheckABI(RunRuntimeForABI, nullptr) == 1 && runtime_reason == HaltReason::CodeMiss,
              "next Runtime entry acknowledges the newer SMC generation before cache lookup");
        runtime_for_abi = nullptr;
        expected.state->exit_request = 0;
        Equal(runtime.GetState(), expected.state, "real SMC generation acknowledgement");
        Cleanup(runtime.GetState());
    }
}

}  // namespace

void RetirementResidency() {
    const auto initial_checks = checks;
    StraightLineAndCost();
    RequestedSideExits();
    WideRequestedSnapshots();
    OracleAndBoundedFaults();
    ActualSignalRequests();
    ProtectedNativeAndTypedFaults();
    FunctionPaths();
    RealSMC();
    std::cout << "PASS cross-retirement residency/checkpoint/full-state "
              << checks - initial_checks << " checks\n";
}

}  // namespace swift::tests::loongarch64
