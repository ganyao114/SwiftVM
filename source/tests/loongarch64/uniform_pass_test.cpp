#include "test_support.h"

#include <algorithm>
#include <bit>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>
#include "runtime/backend/address_space.h"
#include "runtime/backend/code_serial.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/backend/loongarch64/jit/compile.h"
#include "runtime/frontend/ir_assembler.h"
#include "runtime/ir/opts/uniform_elimination_pass.h"
#include "runtime/ir/opts/uniform_store_sink_pass.h"

namespace swift::tests::loongarch64 {
namespace {
using T = ir::ValueType;
using O = ir::OpCode;
using Factory = std::function<IntrusivePtr<ir::Block>()>;
using Setup = std::function<void(StateStorage&)>;
constexpr u32 kUniformBytes = 4096;

struct PassPolicy {
    bool elimination{}, dse{}, sink{};
};

// Every switch combination owns a fresh module and fresh source IR: public
// Prepare mutates its source and a cached entry must never stand in for OFF.
struct ProductionModule {
    Config config{TestConfig()};
    std::vector<UniformRangeDesc> ranges;
    std::vector<UniformMapDesc> mappings;
    std::unique_ptr<backend::AddressSpace> space;
    std::shared_ptr<backend::Module> module;

    ProductionModule(PassPolicy policy, std::vector<UniformRangeDesc> sink_ranges = {},
                     std::vector<UniformMapDesc> static_mappings = {})
            : ranges(std::move(sink_ranges)), mappings(std::move(static_mappings)) {
        config.store_sink_uniform_ranges = ranges;
        config.buffers_static_alloc = mappings;
        space = std::make_unique<backend::AddressSpace>(config);
        backend::ModuleConfig options;
        options.read_only = true;
        options.optimizations = Optimizations::DeadCodeRemove;
        if (policy.elimination) options.optimizations |= Optimizations::UniformElimination;
        if (policy.sink) options.optimizations |= Optimizations::XmmFaultSink;
        options.feature_overrides.Set(FeatureId::uniform_elim, policy.elimination);
        options.feature_overrides.Set(FeatureId::uniform_dse, policy.dse);
        options.feature_overrides.Set(FeatureId::xmm_fault_sink, policy.sink);
        // Attribute traffic changes to the shared IR passes, independently of
        // the backend's separate private-clone forwarding optimization.
        options.feature_overrides.Set(FeatureId::loongarch_uniform_forwarding, false);
        options.feature_overrides.Set(FeatureId::loongarch_uniform_residency, false);
        module = std::make_shared<backend::Module>(*space, 0, config.loc_end, options);
    }
};

u32 Count(ir::Block* block, O opcode) {
    u32 count{};
    for (auto& inst : block->GetInstList()) count += inst.GetOp() == opcode;
    return count;
}

struct ProductionBlock {
    ProductionModule owner;
    IntrusivePtr<ir::Block> source;
    BlockFn fn{};
    la::UniformValueStats traffic{};
    u32 code_bytes{};

    ProductionBlock(const Factory& make, PassPolicy policy,
                    std::vector<UniformRangeDesc> ranges = {},
                    std::vector<UniformMapDesc> mappings = {})
            : owner(policy, std::move(ranges), std::move(mappings)), source(make()) {
        Check(owner.module->Push(source.get()), "uniform pass publishes fresh source owner");
        void* entry{};
        { auto lock = source->LockWrite(); entry = la::CompileBlock(owner.module, source.get()); }
        Check(entry && source->GetJitCache().jit_state == backend::JitState::Cached,
              "uniform pass uses production CompileBlock publication");
        fn = reinterpret_cast<BlockFn>(entry);
        code_bytes = source->GetJitCache().cache_size.get<u32>();
        // The executable under test is the published production entry. This
        // emission-only probe exposes traffic for that same prepared source.
        la::JitContext probe;
        probe.ConfigureModule(owner.module.get());
        la::JitTranslator translator{probe};
        translator.Translate(source.get());
        traffic = probe.UniformTraffic();
        Check(!probe.UsesUniformForwarding() && probe.CurrentBufferSize() == code_bytes,
              "public pass traffic probe has production policy and code size");
    }
};

u64 Random(u64& seed) {
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    return seed;
}

void Seed(StateStorage& storage, u32 sample, u64 pc) {
    u64 seed = 0x425fd23813ba06e7ULL ^ (u64{sample} * 0x9e3779b97f4a7c15ULL);
    for (u32 offset = 0; offset < kUniformBytes; offset += 8)
        storage.Put(offset, sample == 0 ? 0 : sample == 1 ? UINT64_MAX : Random(seed));
    storage.state->host_cpu_flags = std::rotl(Random(seed), 19);
    storage.state->current_loc = ir::Location{pc};
    storage.state->prev_loc = ir::Location{pc - 4};
}

void EqualState(const StateStorage& actual, const StateStorage& expected, const std::string& detail) {
    Check(std::memcmp(actual.state->uniform_buffer_begin, expected.state->uniform_buffer_begin,
                      kUniformBytes) == 0 &&
          actual.state->host_cpu_flags == expected.state->host_cpu_flags &&
          actual.state->current_loc == expected.state->current_loc &&
          actual.state->prev_loc == expected.state->prev_loc &&
          actual.state->exit_request == expected.state->exit_request &&
          actual.state->halt_reason == expected.state->halt_reason,
          detail + " complete uniform/flags/PC/halt state");
}

void Differential(const Factory& make, PassPolicy policy, const std::string& detail,
                  u32 samples = 24, const Setup& setup = {},
                  std::vector<UniformRangeDesc> ranges = {},
                  std::vector<UniformMapDesc> mappings = {}) {
    ProductionBlock on{make, policy, ranges, mappings};
    ProductionBlock off{make, {}, ranges, mappings};
    auto reference_block = make();
    for (u32 sample = 0; sample < samples; ++sample) {
        StateStorage actual, control, reference;
        for (auto* storage : {&actual, &control, &reference}) {
            Seed(*storage, sample, reference_block->GetStartLocation().Value());
            if (setup) setup(*storage);
        }
        backend::interp::Interpreter interpreter{*reference.state, reference_block.get()};
        const auto halt = interpreter.Run();
        Check(on.fn(actual.state) == halt && off.fn(control.state) == halt,
              detail + " production ON/OFF interpreter halt sample=" + std::to_string(sample));
        EqualState(actual, reference, detail + " ON");
        EqualState(control, reference, detail + " OFF");
        if (sample < 2) {
            StateStorage abi_on, abi_off;
            for (auto* storage : {&abi_on, &abi_off}) {
                Seed(*storage, sample, reference_block->GetStartLocation().Value());
                if (setup) setup(*storage);
            }
            Check(CheckABI(on.fn, abi_on.state) == 1 && CheckABI(off.fn, abi_off.state) == 1,
                  detail + " production ON/OFF preserve LP64D");
            EqualState(abi_on, reference, detail + " ABI ON");
            EqualState(abi_off, reference, detail + " ABI OFF");
        }
    }
}

IntrusivePtr<ir::Block> SameInstructionStores() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa100})};
    ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
    for (u64 i = 1; i <= 16; ++i)
        as.StoreUniform(ir::Uniform{8, T::U64}, as.Add(input, ir::Operand{ir::Imm{i}}).SetType(T::U64));
    as.StoreUniform(ir::Uniform{16, T::U64}, as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64));
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

void PositiveControls() {
    const Factory make = SameInstructionStores;
    ProductionBlock off{make, {}}, forwarding{make, {true, false, false}}, dse{make, {true, true, false}};
    Check(Count(off.source.get(), O::StoreUniform) == 17 &&
          Count(forwarding.source.get(), O::StoreUniform) == 17 &&
          Count(dse.source.get(), O::StoreUniform) == 2,
          "public DSE independently removes overwritten stores within one guest instruction");
    Check(dse.traffic.stores < off.traffic.stores && dse.traffic.loads < off.traffic.loads &&
          dse.code_bytes < off.code_bytes,
          "public forwarding plus DSE reduces actual uniform traffic and published bytes");
    Differential(make, {true, true, false}, "same instruction public DSE", 48);
    const std::vector<UniformRangeDesc> ranges{{0, 128}};
    ProductionBlock sink_off{make, {}, ranges}, sink{make, {false, false, true}, ranges};
    Check(!sink.owner.module->GetModuleConfig().HasOpt(Optimizations::UniformElimination) &&
          !ResolveFeatureSet(sink.owner.module->GetModuleConfig()).uniform_dse &&
          Count(sink.source.get(), O::StoreUniform) == 2 &&
          sink.traffic.stores < sink_off.traffic.stores && sink.code_bytes < sink_off.code_bytes,
          "configured public sink wins with uniform elimination and DSE disabled");
    Differential(make, {false, false, true}, "independent public sink", 48, {}, ranges);
    std::cout << "[loongarch-uniform-pass] dse-stores=" << dse.traffic.stores << '/' << off.traffic.stores
              << " dse-loads=" << dse.traffic.loads << '/' << off.traffic.loads
              << " bytes=" << dse.code_bytes << '/' << off.code_bytes
              << " sink-stores=" << sink.traffic.stores << '/' << sink_off.traffic.stores
              << " sink-bytes=" << sink.code_bytes << '/' << sink_off.code_bytes << '\n';
}

backend::State* StateFromUniform(u64 address) {
    return reinterpret_cast<backend::State*>(address - backend::state_offset_uniform_buffer);
}

u64 ObserveAndMutate(u64 address, u64 replacement, u64) {
    auto* state = StateFromUniform(address);
    u64 observed{};
    std::memcpy(&observed, state->uniform_buffer_begin, 8);
    std::memcpy(state->uniform_buffer_begin, &replacement, 8);
    return observed;
}

bool ReturningOracle(void* opaque, u64, u64) {
    auto* state = static_cast<backend::State*>(opaque);
    u64 observed{};
    std::memcpy(&observed, state->uniform_buffer_begin, 8);
    std::memcpy(state->uniform_buffer_begin + 40, &observed, 8);
    const u64 replacement = 0x639485a6b7c8d9eaULL;
    std::memcpy(state->uniform_buffer_begin, &replacement, 8);
    return true;
}

void FaultsAndObservers() {
    const Factory fault = [] {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa200})};
        ir::Assembler as{block.get()};
        as.StoreUniform(ir::Uniform{0, T::U64}, as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64));
        // No AdvancePC between the two stores: the memory operation itself
        // must make the earlier guest State observable on its failing path.
        auto bad = as.LoadMemory(ir::Operand{ir::Imm{u64{64}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{24, T::U64}, bad);
        as.StoreUniform(ir::Uniform{0, T::U64}, as.LoadImm(ir::Imm{u64{0xbad}}).SetType(T::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        return block;
    };
    std::array<u64, 4> memory{0x9182736455ULL, 0, 0, 0};
    const Setup bounded = [&](StateStorage& storage) {
        storage.state->pt = reinterpret_cast<u8*>(memory.data());
        storage.state->guest_addr_mask = UINT64_MAX;
        storage.state->guest_addr_limit = sizeof(memory);
    };
    for (PassPolicy policy : {PassPolicy{true, true, false}, PassPolicy{false, false, true}})
        Differential(fault, policy, "store A/fault/store B without AdvancePC", 16, bounded, {{0, 64}});

    const Factory calls = [] {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa300})};
        ir::Assembler as{block.get()};
        auto address = as.GetUniformAddress(ir::Imm{u32{0}}).SetType(T::U64);
        auto input = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
        auto replacement = as.LoadUniform(ir::Uniform{264, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, input);
        auto observed = as.CallHost(&ObserveAndMutate, address, replacement,
                                   as.LoadImm(ir::Imm{u64{0}}).SetType(T::U64)).SetType(T::U64);
        as.StoreUniform(ir::Uniform{8, T::U64}, observed);
        as.StoreUniform(ir::Uniform{16, T::U64}, as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64));
        as.StoreUniform(ir::Uniform{0, T::U64}, input);
        auto loaded = as.LoadMemory(ir::Operand{ir::Imm{u64{0}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{24, T::U64}, loaded);
        as.StoreUniform(ir::Uniform{32, T::U64}, as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64));
        as.StoreUniform(ir::Uniform{0, T::U64}, replacement);
        block->SetTerminal(ir::terminal::ReturnToHost{});
        return block;
    };
    const Setup oracle = [&](StateStorage& storage) {
        bounded(storage);
        storage.state->interp_range_check = ReturningOracle;
        storage.state->interp_range_check_ctx = storage.state;
    };
    Differential(calls, {true, true, true}, "CallHost and returning memory oracle", 32, oracle, {{0, 64}});
}

void ConfiguredOverlap() {
    for (bool store : {false, true}) {
        const Factory make = [=] {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{store ? 0xa400u : 0xa500u})};
            ir::Assembler as{block.get()};
            auto first = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
            auto second = as.LoadUniform(ir::Uniform{264, T::U64}).SetType(T::U64);
            as.StoreUniform(ir::Uniform{0, T::U64}, first); // eligible [0,8)
            if (store) as.StoreUniform(ir::Uniform{4, T::U64}, second); // noneligible [4,12)
            auto overlapping = as.LoadUniform(ir::Uniform{4, T::U64}).SetType(T::U64);
            as.StoreUniform(ir::Uniform{32, T::U64}, overlapping);
            if (!store) as.StoreUniform(ir::Uniform{0, T::U64}, second);
            as.StoreUniform(ir::Uniform{48, T::U64}, as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64));
            block->SetTerminal(ir::terminal::ReturnToHost{});
            return block;
        };
        Differential(make, {false, false, true}, store ? "noneligible overlapping store" :
                     "noneligible overlapping load", 48, {}, {{0, 8}});
    }

    // A partial flush must carry the entire connected component of pending
    // byte overlaps. Otherwise an older, still-pending store can later move
    // after a newer overlapping store that was already committed.
    const Factory connected = [] {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa580})};
        ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
        auto b = as.LoadUniform(ir::Uniform{288, T::V128}).SetType(T::V128);
        as.StoreUniform(ir::Uniform{8, T::U64}, a);
        as.StoreUniform(ir::Uniform{0, T::V128}, b);
        // Mixed FPR/GPR views cannot forward, so this forces a partial flush.
        as.StoreUniform(ir::Uniform{64, T::U32}, as.LoadUniform(ir::Uniform{0, T::U32}).SetType(T::U32));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        return block;
    };
    Differential(connected, {false, false, true}, "overlapping pending flush component", 32, {}, {{0, 16}});
    const Factory transitive = [] {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa5c0})};
        ir::Assembler as{block.get()};
        auto a = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
        auto b = as.LoadUniform(ir::Uniform{288, T::V128}).SetType(T::V128);
        auto c = as.LoadUniform(ir::Uniform{264, T::U64}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, a);
        as.StoreUniform(ir::Uniform{4, T::V128}, b);
        as.StoreUniform(ir::Uniform{12, T::U64}, c);
        as.StoreUniform(ir::Uniform{64, T::U32}, as.LoadUniform(ir::Uniform{16, T::U32}).SetType(T::U32));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        return block;
    };
    Differential(transitive, {false, false, true}, "transitive pending overlap closure", 32, {}, {{0, 20}});
}

IntrusivePtr<ir::Block> PartialViews() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa600})};
    ir::Assembler as{block.get()};
    auto source = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
    // A descriptor never widens or narrows the value actually being stored.
    as.StoreUniform(ir::Uniform{0, T::U8}, source);
    as.StoreUniform(ir::Uniform{64, T::U64}, as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64));
    for (const auto [offset, type] : {std::pair{1u, T::U8}, std::pair{2u, T::U16}, std::pair{4u, T::U32}}) {
        auto narrow = as.LoadUniform(ir::Uniform{offset, type}).SetType(type);
        as.StoreUniform(ir::Uniform{72 + offset * 8, type}, narrow);
    }
    as.StoreUniform(ir::Uniform{8, T::U64}, source.SetCastType(T::U16));
    as.StoreUniform(ir::Uniform{128, T::U64}, as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64));
    auto vector = as.LoadUniform(ir::Uniform{288, T::V128}).SetType(T::V128);
    as.StoreUniform(ir::Uniform{32, T::V128}, vector);
    as.StoreUniform(ir::Uniform{144, T::V64}, as.LoadUniform(ir::Uniform{32, T::V64}).SetType(T::V64));
    as.StoreUniform(ir::Uniform{160, T::U64}, as.LoadUniform(ir::Uniform{40, T::U64}).SetType(T::U64));
    as.StoreUniform(ir::Uniform{33, T::U16}, source.SetCastType(T::U16));
    as.StoreUniform(ir::Uniform{176, T::V128}, as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128));
    as.StoreUniform(ir::Uniform{4079, T::V128}, vector);
    as.StoreUniform(ir::Uniform{4087, T::U64}, source);
    as.StoreUniform(ir::Uniform{512, T::V128}, as.LoadUniform(ir::Uniform{4079, T::V128}).SetType(T::V128));
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

void RandomAndPartialViews() {
    Differential(PartialViews, {true, true, true}, "scalar/vector partial uniform views", 48, {}, {{0, 4096}});
    for (u32 topology = 0; topology < 4; ++topology) {
        const Factory make = [=] {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xa700 + topology * 0x100})};
            ir::Assembler as{block.get()};
            constexpr std::array types{T::U8, T::U16, T::U32, T::U64, T::V64, T::V128};
            u64 seed = 0x593816c7b05f2e4aULL + topology;
            for (u32 i = 0; i < 32; ++i) {
                const auto type = types[Random(seed) % types.size()];
                const u32 width = ir::GetValueSizeByte(type);
                const u32 target = i % 7 == 0 ? 4096 - width : Random(seed) % 192;
                auto input = as.LoadUniform(ir::Uniform{1024 + i * 16, type}).SetType(type);
                as.StoreUniform(ir::Uniform{target, type}, input);
                const u32 delta = Random(seed) % width;
                const u32 max_width = width - delta;
                const T read_type = max_width >= 8 ? T::U64 : max_width >= 4 ? T::U32 :
                                    max_width >= 2 ? T::U16 : T::U8;
                auto read = as.LoadUniform(ir::Uniform{target + delta, read_type}).SetType(read_type);
                as.StoreUniform(ir::Uniform{2048 + i * 16, read_type}, read);
                if (i % 3 == 0)
                    as.StoreUniform(ir::Uniform{3072 + i * 16, type},
                                    as.LoadUniform(ir::Uniform{target, type}).SetType(type));
            }
            block->SetTerminal(ir::terminal::ReturnToHost{});
            return block;
        };
        Differential(make, {true, true, true}, "random byte aliases topology=" + std::to_string(topology),
                     24, {}, {{0, 4096}});
    }
}

u64 RequestExit(u64 address, u64 request, u64) {
    auto* state = StateFromUniform(address);
    state->exit_request = request;
    u64 observed{};
    std::memcpy(&observed, state->uniform_buffer_begin, 8);
    return observed;
}

void PreciseRetirement() {
    for (u64 request : {u64{1}, u64{1} << 63}) {
        const Factory make = [=] {
            IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xac00})};
            ir::Assembler as{block.get()};
            auto address = as.GetUniformAddress(ir::Imm{u32{0}}).SetType(T::U64);
            auto input = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
            as.StoreUniform(ir::Uniform{0, T::U64}, input);
            as.AdvancePC(ir::Imm{u32{4}});
            as.StoreUniform(ir::Uniform{8, T::U64}, input);
            auto observed = as.CallHost(&RequestExit, address,
                    as.LoadImm(ir::Imm{request}).SetType(T::U64),
                    as.LoadImm(ir::Imm{u64{0}}).SetType(T::U64)).SetType(T::U64);
            as.StoreUniform(ir::Uniform{16, T::U64}, observed);
            as.AdvancePC(ir::Imm{u32{4}});
            as.StoreUniform(ir::Uniform{0, T::U64}, as.LoadImm(ir::Imm{u64{0xbad}}).SetType(T::U64));
            block->SetTerminal(ir::terminal::ReturnToHost{});
            return block;
        };
        ProductionBlock on{make, {true, true, true}, {{0, 32}}}, off{make, {}, {{0, 32}}};
        for (u32 sample = 0; sample < 16; ++sample) {
            StateStorage actual, control, expected;
            for (auto* storage : {&actual, &control, &expected}) Seed(*storage, sample, 0xac00);
            for (u32 offset : {0u, 8u, 16u}) expected.Put(offset, expected.Get(256));
            expected.state->current_loc = ir::Location{0xac08};
            expected.state->prev_loc = ir::Location{0xac04};
            expected.state->exit_request = request;
            const auto halt = request >> 63 ? HaltReason::Signal : HaltReason::CodeMiss;
            expected.state->halt_reason = halt;
            // The ordinary Interpreter samples requests at every IR opcode and
            // has no AdvancePC retirement semantics. Use this independent
            // architectural model for the exact guest-boundary expectation.
            Check(on.fn(actual.state) == halt && off.fn(control.state) == halt,
                  "AdvancePC observes Signal/SMC after completing the retiring instruction");
            EqualState(actual, expected, "precise retirement ON");
            EqualState(control, expected, "precise retirement OFF");
            if (sample < 2) {
                StateStorage abi_on, abi_off;
                Seed(abi_on, sample, 0xac00); Seed(abi_off, sample, 0xac00);
                Check(CheckABI(on.fn, abi_on.state) == 1 && CheckABI(off.fn, abi_off.state) == 1,
                      "retirement exits preserve LP64D");
                EqualState(abi_on, expected, "precise retirement ABI ON");
                EqualState(abi_off, expected, "precise retirement ABI OFF");
            }
        }
    }
}

void StaticMappingIsolation() {
    const Factory make = [] {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xad00})};
        ir::Assembler as{block.get()};
        auto input = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
        auto vector = as.LoadUniform(ir::Uniform{288, T::V128}).SetType(T::V128);
        as.StoreUniform(ir::Uniform{0, T::U64}, input);
        as.UniformBarrier();
        as.StoreUniform(ir::Uniform{16, T::U64}, as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64));
        as.StoreUniform(ir::Uniform{32, T::V128}, vector);
        as.UniformBarrier();
        as.StoreUniform(ir::Uniform{96, T::V128}, as.LoadUniform(ir::Uniform{32, T::V128}).SetType(T::V128));
        as.StoreUniform(ir::Uniform{128, T::U64}, as.LoadUniform(ir::Uniform{40, T::U64}).SetType(T::U64));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        return block;
    };
    const std::vector<UniformMapDesc> mappings{{0, 8, 25, false}, {32, 16, 24, true}};
    ProductionBlock on{make, {true, true, true}, {{0, 144}}, mappings};
    for (auto op : {O::GetHostGPR, O::SetHostGPR, O::GetHostFPR, O::SetHostFPR})
        Check(Count(on.source.get(), op) == 0, "LoongArch public passes never introduce unimplemented static Host ops");
    const auto& original = on.owner.space->GetUniformInfo();
    Check(!original.uniform_regs_map.GetValueAt(0).Null() &&
          !original.uniform_regs_map.GetValueAt(32).Null() &&
          original.uni_gprs.Get(25) && original.uni_fprs.Get(24),
          "LoongArch sanitizes a local UniformInfo copy and preserves AddressSpace mappings");
    Differential(make, {true, true, true}, "static GPR/FPR descriptor isolation", 32, {}, {{0, 144}}, mappings);
}

IntrusivePtr<ir::Block> DiamondReference() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xae00})};
    ir::Assembler as{block.get()};
    auto input = as.LoadUniform(ir::Uniform{256, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{0, T::U64}, input);
    auto right = as.Goto(ir::BOOL{as.TestNotZero(input)});
    as.StoreUniform(ir::Uniform{8, T::U64}, as.Add(input, ir::Operand{ir::Imm{u64{31}}}).SetType(T::U64));
    auto join = as.Goto(ir::BOOL{as.LoadImm(ir::Imm{u8{1}}).SetType(T::U8)});
    as.BindLabel(right);
    as.StoreUniform(ir::Uniform{8, T::U64}, as.Sub(input, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64));
    as.BindLabel(join);
    as.StoreUniform(ir::Uniform{16, T::U64}, input);
    as.SetLocation(ir::Lambda{ir::Imm{u64{0xb100}}});
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

void HIRLifetimeAndIndices() {
    ProductionModule on{{true, true, true}, {{0, 32}}}, off{{}, {{0, 32}}};
    const auto build = [](ProductionModule& owner, bool optimized) {
        BlockFn fn{};
        {
            ir::HIRBuilder builder{1, true};
            auto* function = builder.AppendFunction(ir::Location{0xae00});
            auto input = function->LoadUniform<ir::U64>(ir::Uniform{256, T::U64});
            auto dead = function->Add<ir::U64>(input, ir::Operand{ir::Imm{u64{3}}});
            function->StoreUniform(ir::Uniform{0, T::U64}, dead);
            function->StoreUniform(ir::Uniform{0, T::U64}, input);
            auto folded = function->LoadUniform<ir::U64>(ir::Uniform{0, T::U64});
            auto condition = function->TestZero<ir::U8>(folded);
            builder.If(ir::terminal::If{ir::BOOL{condition},
                    ir::terminal::LinkBlock{ir::Location{0xaf00}},
                    ir::terminal::LinkBlock{ir::Location{0xb000}}});
            builder.SetCurBlock(ir::Location{0xaf00});
            auto left = function->Add<ir::U64>(folded, ir::Operand{ir::Imm{u64{31}}});
            builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xb100}});
            builder.SetCurBlock(ir::Location{0xb000});
            auto right = function->Sub<ir::U64>(folded, ir::Operand{ir::Imm{u64{7}}});
            builder.LinkBlock(ir::terminal::LinkBlock{ir::Location{0xb100}});
            builder.SetCurBlock(ir::Location{0xb100});
            ir::Params inputs; inputs.Push(left); inputs.Push(right);
            auto phi = function->AddPhi<ir::U64>(inputs);
            function->StoreUniform(ir::Uniform{8, T::U64}, phi);
            function->StoreUniform(ir::Uniform{16, T::U64}, folded);
            function->EndBlock(ir::terminal::ReturnToHost{});
            function->EndFunction();
            const auto input_id = input.Def()->Id(), folded_id = folded.Def()->Id();
            const auto dead_id = dead.Def()->Id();
            auto* folded_identity = folded.Def();
            void* entry = la::CompileFunctions(owner.module, std::span{&function, 1});
            Check(entry && function->GetFunction()->GetJitCache().jit_state == backend::JitState::Cached,
                  "uniform HIR test uses production CompileFunctions publication");
            fn = reinterpret_cast<BlockFn>(entry);
            Check(input.Def()->Id() == input_id && folded.Def()->Id() == folded_id &&
                  folded.Def() == folded_identity,
                  "per-block preparation preserves function-wide IDs and cross-block SSA identity");
            Check(folded.Def()->GetOp() == (optimized ? O::BitCast : O::LoadUniform),
                  "forwarded HIR load keeps its definition while cross-block users survive");
            if (optimized)
                Check(dead_id < function->GetHIRValues().size() &&
                      function->GetHIRValues()[dead_id] == nullptr,
                      "DSE and DCE erase obsolete HIR producer from its original value index");
            std::unordered_set<u16> ids;
            for (auto* hir : function->GetHIRBlocks()) {
                if (!hir) continue;
                for (auto& inst : hir->GetInstList()) {
                    Check(ids.insert(inst.Id()).second, "surviving HIR instruction IDs remain unique across blocks");
                    if (inst.ReturnType() != T::VOID) {
                        const auto* value = function->GetHIRValue(ir::Value{&inst});
                        Check(value && value->value.Def() == &inst,
                              "surviving HIR value index names the original definition");
                    }
                }
            }
        } // Execution deliberately starts after HIR builder and use pools die.
        return fn;
    };
    const auto on_fn = build(on, true), off_fn = build(off, false);
    auto reference_block = DiamondReference();
    for (u32 sample = 0; sample < 32; ++sample) {
        StateStorage actual, control, reference;
        for (auto* storage : {&actual, &control, &reference}) Seed(*storage, sample, 0xae00);
        const auto input = reference.Get(256);
        backend::interp::Interpreter interpreter{*reference.state, reference_block.get()};
        const auto halt = interpreter.Run();
        // Function entry/path labels also publish block history. The ordinary
        // flat interpreter has no FunctionPlan location table, so supply its
        // independently selected predecessor while retaining its byte oracle.
        reference.state->prev_loc = ir::Location{input == 0 ? 0xaf00u : 0xb000u};
        Check(on_fn(actual.state) == halt && off_fn(control.state) == halt,
              "HIR diamond executes both phi predecessors after builder destruction");
        EqualState(actual, reference, "cross-block SSA ON");
        EqualState(control, reference, "cross-block SSA OFF");
        if (sample < 2) {
            StateStorage abi_on, abi_off;
            Seed(abi_on, sample, 0xae00); Seed(abi_off, sample, 0xae00);
            Check(CheckABI(on_fn, abi_on.state) == 1 && CheckABI(off_fn, abi_off.state) == 1,
                  "optimized whole-function phi path preserves LP64D");
            EqualState(abi_on, reference, "HIR ABI ON");
            EqualState(abi_off, reference, "HIR ABI OFF");
        }
    }
}

void ObservationLayouts() {
    constexpr std::array observations{
        O::LoadMemory, O::StoreMemory, O::LoadMemoryTSO, O::StoreMemoryTSO,
        O::MemoryCopy, O::MemoryCopyTSO, O::CompareAndSwap, O::CompareAndSwap128,
        O::CheckMemoryAlignment, O::AtomicExchange, O::AtomicFetchAdd, O::AtomicRMW,
        O::CallLambda, O::CallLocation, O::CallDynamic, O::X87Op, O::GetUniformAddress,
        O::UniformBarrier, O::GetHostGPR, O::SetHostGPR, O::GetHostFPR, O::SetHostFPR,
        O::SetLocation, O::GetLocation, O::Goto, O::NotGoto, O::BindLabel,
        O::PushRSB, O::CallReturn, O::PopRSB, O::AdvancePC};
    ir::UniformInfo info;
    info.uniform_size = kUniformBytes;
    info.store_sink_uniform_ranges = {{0, 16}};
    info.precise_observation_points = true;
    FeatureSet features{};
    features.uniform_dse = true;
    features.xmm_fault_sink = true;
    for (auto op : observations) {
        Check(ir::UniformStoreSinkPass::IsObservationPoint(op, info),
              "public observation inventory covers opcode=" + std::to_string(u32(op)));
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0xb200})};
        ir::Assembler as{block.get()};
        auto a = as.LoadImm(ir::Imm{u64{17}}).SetType(T::U64);
        auto b = as.LoadImm(ir::Imm{u64{29}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, a);
        auto* first = &block->GetInstList().back();
        // This is a pass-layout fixture, not executable IR: only the opcode
        // class is relevant here. Native fault/call/retirement cases above
        // independently verify the production code and observation semantics.
        auto* observation = new ir::Inst(op);
        block->AppendInstUnchecked(observation);
        as.StoreUniform(ir::Uniform{0, T::U64}, b);
        auto* last = &block->GetInstList().back();
        block->SetTerminal(ir::terminal::ReturnToHost{});
        ir::UniformEliminationPass::Run(block.get(), info, true, features);
        ir::UniformStoreSinkPass::Run(block.get(), info, features);
        size_t position{}, first_position = SIZE_MAX, boundary_position = SIZE_MAX, last_position = SIZE_MAX;
        for (auto& inst : block->GetInstList()) {
            if (&inst == first) first_position = position;
            if (&inst == observation) boundary_position = position;
            if (&inst == last) last_position = position;
            ++position;
        }
        Check(Count(block.get(), O::StoreUniform) == 2 && first_position < boundary_position &&
              boundary_position < last_position,
              "DSE preserves and sink flushes stores on both sides of observer=" + std::to_string(u32(op)));
    }
    Check(!ir::UniformStoreSinkPass::IsObservationPoint(O::Add, info), "pure arithmetic permits local store sinking");
    info.precise_observation_points = false;
    Check(!ir::UniformStoreSinkPass::IsObservationPoint(O::AdvancePC, info),
          "precise retirement policy is opt-in for legacy backends");
}

void RangeHashAndBounds() {
    using backend::ComputeConfigHash;
    auto base = TestConfig();
    // Frozen pre-generic-sink key for this exact TestConfig, including both
    // existing empty XMM/GPR range counts. Empty new ranges must retain it.
    constexpr u64 legacy_key = 0x95497ad9c97e71f2ULL;
    Check(ComputeConfigHash(base) == legacy_key, "empty generic sink ranges preserve legacy ConfigHash");
    std::vector<UniformRangeDesc> first{{0, 8}}, copied{{0, 8}}, shifted{{4, 8}}, widened{{0, 16}};
    auto configured = base, copy = base, offset = base, size = base, gpr = base, xmm = base;
    configured.store_sink_uniform_ranges = first;
    copy.store_sink_uniform_ranges = copied;
    offset.store_sink_uniform_ranges = shifted;
    size.store_sink_uniform_ranges = widened;
    gpr.loop_gpr_uniform_ranges = first;
    xmm.xmm_uniform_ranges = first;
    auto expected = backend::HashU64(0x554e4953494e4b01ULL, legacy_key);
    for (u64 word : {u64{1}, u64{0}, u64{8}}) expected = backend::HashU64(word, expected);
    Check(ComputeConfigHash(configured) == expected && ComputeConfigHash(configured) == ComputeConfigHash(copy),
          "configured sink identity hashes domain/count/offset/size rather than span address");
    std::unordered_set<u64> keys{legacy_key, ComputeConfigHash(configured), ComputeConfigHash(offset),
                               ComputeConfigHash(size), ComputeConfigHash(gpr), ComputeConfigHash(xmm)};
    Check(keys.size() == 6, "range offset/size and sink/GPR/XMM domains have distinct cache identities");
    for (const auto range : {UniformRangeDesc{4096, 1}, UniformRangeDesc{4097, 0},
                             UniformRangeDesc{4090, 16}, UniformRangeDesc{UINT32_MAX, 16}}) {
        auto config = base;
        std::array ranges{range};
        config.store_sink_uniform_ranges = ranges;
        bool rejected{};
        try { backend::AddressSpace space{config}; }
        catch (const std::invalid_argument&) { rejected = true; }
        Check(rejected, "invalid/overflowing sink range is rejected before uniform analysis");
    }
    std::array nonempty{UniformRangeDesc{0, 1}};
    auto zero = base;
    zero.uniform_buffer_size = 0;
    zero.store_sink_uniform_ranges = nonempty;
    bool rejected{};
    try { backend::AddressSpace space{zero}; }
    catch (const std::invalid_argument&) { rejected = true; }
    Check(rejected, "nonempty configured range is rejected for zero-sized uniform state");
    std::array valid{UniformRangeDesc{4080, 16}, UniformRangeDesc{4096, 0}};
    auto exact = base;
    exact.store_sink_uniform_ranges = valid;
    backend::AddressSpace accepted{exact};
    Check(accepted.GetUniformInfo().IsStoreSinkUniformRange(4080, 16) &&
          !accepted.GetUniformInfo().IsStoreSinkUniformRange(4080, 17),
          "end-aligned range and empty end sentinel have exact byte bounds");
}

}  // namespace

void UniformPasses() {
    const auto initial_checks = checks;
    PositiveControls();
    FaultsAndObservers();
    ConfiguredOverlap();
    RandomAndPartialViews();
    PreciseRetirement();
    StaticMappingIsolation();
    HIRLifetimeAndIndices();
    ObservationLayouts();
    RangeHashAndBounds();
    std::cout << "PASS production uniform elimination/DSE/sink/full-state "
              << checks - initial_checks << " checks\n";
}

}  // namespace swift::tests::loongarch64
