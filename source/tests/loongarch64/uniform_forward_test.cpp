#include "test_support.h"

#include <bit>
#include <chrono>
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
namespace {
using T = ir::ValueType;
using O = ir::OpCode;

struct ForwardCompiled {
    Config config{TestConfig()};
    la::JitContext context;
    la::JitTranslator translator;
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    BlockFn fn{};
    u64 translate_ns{};
    ForwardCompiled(ir::Block* block, bool forward, bool rematerialize = false,
                    la::ExecutionMode mode = la::ExecutionMode::Ordinary, backend::Module* owner = nullptr)
        : translator(context, mode) {
        context.SetUniformForwarding(forward); context.SetScalarRematerialization(rematerialize);
        context.SetFlagsDemand(false); context.SetVectorAllocation(false);
        if (owner) context.ConfigureModule(owner);
        const auto start = std::chrono::steady_clock::now();
        translator.Translate(block);
        translate_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate uniform-forward/rematerialization code");
        context.Flush(*buffer); fn = reinterpret_cast<BlockFn>(buffer->exec_data);
    }
};

void Compare(ir::Block* block, u32 samples, bool loop = false) {
    const auto source = block->ToString();
    ForwardCompiled optimized{block, true}, baseline{block, false};
    Check(!baseline.context.UniformStats().enabled && baseline.context.UniformStats().loads_forwarded == 0,
          "uniform OFF baseline actually disables the pass");
    Check(block->ToString() == source, "uniform forwarding keeps source public IDs and arguments intact");
    u64 seed = 0x91465594630f1327ULL;
    for (u32 sample = 0; sample < samples; ++sample) {
        StateStorage actual, control, reference;
        for (u32 offset = 0; offset < 256; offset += 8) {
            seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
            const auto value = sample == 0 ? u64{0} : sample == 1 ? UINT64_MAX : seed;
            actual.Put(offset, value); control.Put(offset, value); reference.Put(offset, value);
        }
        if (loop) { actual.Put(16, 1 + sample % 31); control.Put(16, 1 + sample % 31); reference.Put(16, 1 + sample % 31); }
        actual.state->host_cpu_flags = control.state->host_cpu_flags = reference.state->host_cpu_flags = std::rotl(seed, 21);
        backend::interp::Interpreter interpreter{*reference.state, block};
        const auto halt = interpreter.Run();
        Check(optimized.fn(actual.state) == halt && baseline.fn(control.state) == halt &&
              std::memcmp(actual.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 256) == 0 &&
              std::memcmp(control.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 256) == 0 &&
              actual.state->host_cpu_flags == reference.state->host_cpu_flags,
              "uniform ON/OFF matches interpreter across aliases, joins and backedges sample=" + std::to_string(sample));
    }
    Check(optimized.context.UniformStats().loads_forwarded > 0 &&
          optimized.context.UniformTraffic().loads < baseline.context.UniformTraffic().loads &&
          optimized.context.UniformTraffic().stores == baseline.context.UniformTraffic().stores,
          "forwarding reduces state reads while retaining every precise architectural store");
    std::cout << "[loongarch-uniform-case] pc=" << block->GetStartLocation().Value()
              << " forwarded=" << optimized.context.UniformStats().loads_forwarded
              << " state-loads=" << optimized.context.UniformTraffic().loads << '/' << baseline.context.UniformTraffic().loads
              << " spills=" << optimized.context.AllocationStats().spilled_values << '/' << baseline.context.AllocationStats().spilled_values
              << " translate-ns=" << optimized.translate_ns << '/' << baseline.translate_ns << '\n';
}

IntrusivePtr<ir::Block> Aliases() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x8100})}; ir::Assembler as{block.get()};
    // The current emitter follows result/store view widths, not descriptor widths.
    auto source = as.LoadUniform(ir::Uniform{0, T::U8}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{8, T::U8}, source);
    as.AdvancePC(ir::Imm{u32{4}});
    auto complete = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto subrange = as.LoadUniform(ir::Uniform{9, T::U16}).SetType(T::U16);
    as.StoreUniform(ir::Uniform{32, T::U64}, complete);
    as.StoreUniform(ir::Uniform{40, T::U16}, subrange);
    as.StoreUniform(ir::Uniform{8, T::U64}, source.SetCastType(T::U8));
    as.StoreUniform(ir::Uniform{48, T::U8}, as.LoadUniform(ir::Uniform{8, T::U8}).SetType(T::U8));
    // A mixed-width store cannot reconstruct a wider load from one scalar view.
    as.StoreUniform(ir::Uniform{56, T::U64}, as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64));
    auto vector = as.LoadUniform(ir::Uniform{64, T::V128}).SetType(T::V128);
    as.StoreUniform(ir::Uniform{8, T::V128}, vector);
    as.StoreUniform(ir::Uniform{80, T::U64}, as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64));
    block->SetTerminal(ir::terminal::ReturnToHost{}); return block;
}

IntrusivePtr<ir::Block> Join(bool dirty) {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x8200})}; ir::Assembler as{block.get()};
    auto source = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto condition = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{8, T::U64}, source);
    auto branch = as.Goto(ir::BOOL{as.TestNotZero(condition)});
    auto snapshot = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{32, T::U64}, snapshot);
    if (dirty) as.StoreUniform(ir::Uniform{8, T::U64}, as.Add(source, ir::Operand{ir::Imm{u64{7}}}).SetType(T::U64));
    as.BindLabel(branch);
    auto joined = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{40, T::U64}, joined);
    block->SetTerminal(ir::terminal::ReturnToHost{}); return block;
}

IntrusivePtr<ir::Block> Loop() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x8300})}; ir::Assembler as{block.get()};
    auto source = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{8, T::U64}, source);
    auto count = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
    auto repeated = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto sum = as.Add(count, ir::Operand{repeated}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{32, T::U64}, sum);
    auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{16, T::U64}, next); as.AdvancePC(ir::Imm{u32{4}});
    auto branch = as.Goto(ir::BOOL{as.TestNotZero(next)}); as.BindLabel(branch);
    auto* label = &block->GetInstList().back(); block->RemoveInst(label); block->InsertBefore(label, count.Def());
    as.StoreUniform(ir::Uniform{40, T::U64}, as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64));
    block->SetTerminal(ir::terminal::ReturnToHost{}); block->ReIdInstr(); return block;
}

u64 MutateUniform(u64 address, u64 value, u64 request) {
    auto* state = reinterpret_cast<backend::State*>(address);
    std::memcpy(state->uniform_buffer_begin, &value, 8); state->exit_request = request;
    return value;
}
bool MutateRange(void* opaque, u64, u64) {
    auto* state = static_cast<backend::State*>(opaque); const u64 value = 0x123456789abcdef0ULL;
    std::memcpy(state->uniform_buffer_begin, &value, 8); return true;
}
void HelpersAndFaults() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x8400})}; ir::Assembler as{block.get()};
    auto address = as.LoadUniform(ir::Uniform{480, T::U64}).SetType(T::U64);
    auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{8, T::U64}, input);
    auto replacement = as.LoadImm(ir::Imm{u64{0x456789}}).SetType(T::U64);
    as.CallHost(&MutateUniform, address, replacement, as.LoadImm(ir::Imm{u64{0}}));
    auto after = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{16, T::U64}, after);
    // Native memory calls a range oracle; a returning oracle may mutate State.
    auto memory = as.LoadMemory(ir::Operand{as.LoadImm(ir::Imm{u64{0}})}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{24, T::U64}, memory);
    after = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{32, T::U64}, after);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    for (bool forward : {false, true}) {
        ForwardCompiled compiled{block.get(), forward}; StateStorage actual;
        std::array<u64, 4> data{0xabba, 0, 0, 0}; actual.Put(0, 17); actual.Put(480, reinterpret_cast<u64>(actual.state));
        actual.state->pt = reinterpret_cast<u8*>(data.data()); actual.state->guest_addr_mask = UINT64_MAX;
        actual.state->guest_addr_limit = sizeof(data); actual.state->interp_range_check = &MutateRange;
        actual.state->interp_range_check_ctx = actual.state;
        Check(compiled.fn(actual.state) == HaltReason::CallHost && actual.Get(8) == 17 &&
              actual.Get(16) == 0x456789 && actual.Get(24) == 0xabba && actual.Get(32) == 0x123456789abcdef0ULL,
              "opaque helper and range oracle invalidate promoted uniform facts");
    }
    IntrusivePtr<ir::Block> fault{new ir::Block(ir::Location{0x8500})}; ir::Assembler f{fault.get()};
    f.StoreUniform(ir::Uniform{0, T::U64}, f.LoadImm(ir::Imm{u64{0xfeed}})); f.AdvancePC(ir::Imm{u32{4}});
    auto bad = f.LoadMemory(ir::Operand{f.LoadImm(ir::Imm{u64{64}})}).SetType(T::U64);
    f.StoreUniform(ir::Uniform{0, T::U64}, bad); fault->SetTerminal(ir::terminal::ReturnToHost{});
    for (bool forward : {false, true}) {
        ForwardCompiled compiled{fault.get(), forward}; StateStorage actual; std::array<u8, 16> bytes{};
        actual.state->pt = bytes.data(); actual.state->guest_addr_mask = UINT64_MAX; actual.state->guest_addr_limit = bytes.size();
        actual.state->current_loc = 0x8500;
        Check(compiled.fn(actual.state) == HaltReason::PageFatal && actual.Get(0) == 0xfeed && actual.state->current_loc == 0x8504,
              "fault observes committed uniforms at the exact retired guest boundary");
    }
}

void RetirementAndStep() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x8600})}; ir::Assembler as{block.get()};
    auto value = as.LoadImm(ir::Imm{u64{0x1234}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{0, T::U64}, value); as.AdvancePC(ir::Imm{u32{4}});
    auto forwarded = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{8, T::U64}, forwarded);
    auto address = as.LoadUniform(ir::Uniform{480, T::U64}).SetType(T::U64);
    auto request = as.LoadUniform(ir::Uniform{488, T::U64}).SetType(T::U64);
    as.CallHost(&MutateUniform, address, forwarded, request); as.AdvancePC(ir::Imm{u32{4}});
    as.StoreUniform(ir::Uniform{0, T::U64}, as.LoadImm(ir::Imm{u64{0xbad}}));
    block->SetTerminal(ir::terminal::ReturnToHost{});
    for (bool forward : {false, true}) for (u64 request : {u64{1}, u64{1} << 63}) {
        ForwardCompiled compiled{block.get(), forward}; StateStorage actual;
        actual.Put(480, reinterpret_cast<u64>(actual.state)); actual.Put(488, request); actual.state->current_loc = 0x8600;
        Check(compiled.fn(actual.state) == (request >> 63 ? HaltReason::Signal : HaltReason::CodeMiss) &&
              actual.Get(0) == 0x1234 && actual.Get(8) == 0x1234 && actual.state->current_loc == 0x8608,
              "Signal/SMC Poll exits retain precise stores and do not execute the successor");
        if (forward) Check(compiled.context.UniformStats().loads_forwarded > 0,
                           "a continuing retirement marker permits load forwarding before a later Poll exit");
    }
    IntrusivePtr<ir::Block> single{new ir::Block(ir::Location{0x8700})}; ir::Assembler s{single.get()};
    auto input = s.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    s.StoreUniform(ir::Uniform{8, T::U64}, input);
    s.StoreUniform(ir::Uniform{16, T::U64}, s.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64));
    single->SetTerminal(ir::terminal::LinkBlock{ir::Location{0x8704}});
    for (bool forward : {false, true}) {
        ForwardCompiled compiled{single.get(), forward, true, la::ExecutionMode::SingleInstruction}; StateStorage actual;
        actual.Put(0, 0x8877665544332211ULL);
        Check(compiled.fn(actual.state) == HaltReason::Step && actual.Get(8) == actual.Get(0) && actual.Get(16) == actual.Get(0) &&
              actual.state->current_loc == 0x8704, "Step atomically retires forwarded IR with precise state commits");
    }
}

void DominanceAndPolicy() {
    IntrusivePtr<ir::Block> invalid{new ir::Block(ir::Location{0x8800})}; ir::Assembler as{invalid.get()};
    auto condition = as.LoadImm(ir::Imm{u8{1}}); auto edge = as.Goto(ir::BOOL{condition});
    auto path_only = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64); as.BindLabel(edge);
    as.StoreUniform(ir::Uniform{8, T::U64}, path_only); (void)as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    invalid->SetTerminal(ir::terminal::ReturnToHost{});
    const auto rejected = la::UniformForwardPlan::Build(invalid.get());
    Check(rejected.Stats().candidates == 0 && rejected.Stats().rejected_sources > 0,
          "uniform source must dominate, even for malformed conditional SSA definitions");

    // Force every nonconstant source to spill so pruning is deterministic,
    // independent of the production allocator's six-register preferences.
    IntrusivePtr<ir::Block> pressure{new ir::Block(ir::Location{0x8840})}; ir::Assembler p{pressure.get()};
    auto input = p.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    p.StoreUniform(ir::Uniform{8, T::U8}, input);
    p.StoreUniform(ir::Uniform{16, T::U64}, p.LoadUniform(ir::Uniform{8, T::U8}).SetType(T::U64));
    pressure->SetTerminal(ir::terminal::ReturnToHost{});
    const auto original = pressure->ToString();
    auto forced = la::UniformForwardPlan::Build(pressure.get()); forced.Apply();
    auto live = la::RegisterPlan::Build(pressure.get());
    auto spilled = la::ScalarAllocationPlan::Build(pressure.get(), live, {}, 0, false);
    Check(forced.Stats().loads_forwarded == 1 && forced.RejectSpilledSources(spilled) &&
          forced.Stats().allocation_rejections == 1 && forced.Stats().loads_forwarded == 0 &&
          pressure->ToString() == original,
          "spilled promoted source restores the exact descriptor and architectural load");
    forced.Apply(); forced.RejectAll();
    Check(pressure->ToString() == original && forced.Stats().loads_forwarded == 0,
          "bounded allocation fallback restores every active promotion");

    IntrusivePtr<ir::Block> constant{new ir::Block(ir::Location{0x8880})}; ir::Assembler c{constant.get()};
    auto immutable = c.LoadImm(ir::Imm{u64{0x1122334455667788}}).SetType(T::U64);
    c.StoreUniform(ir::Uniform{0, T::U64}, immutable);
    c.StoreUniform(ir::Uniform{8, T::U64}, c.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64));
    constant->SetTerminal(ir::terminal::ReturnToHost{});
    auto reconstructible = la::UniformForwardPlan::Build(constant.get()); reconstructible.Apply();
    live = la::RegisterPlan::Build(constant.get());
    auto rematerialized = la::ScalarAllocationPlan::Build(constant.get(), live, {}, 0, true);
    Check(reconstructible.Stats().loads_forwarded == 1 && !reconstructible.RejectSpilledSources(rematerialized) &&
          rematerialized.Rematerialized(immutable.Def()) && !rematerialized.Spilled(immutable.Def()),
          "rematerialized promoted source needs no register or spill home");

    IntrusivePtr<ir::Block> evicted{new ir::Block(ir::Location{0x88c0})}; ir::Assembler e{evicted.get()};
    auto anchor = e.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    e.StoreUniform(ir::Uniform{8, T::U64}, anchor);
    std::array<ir::Value, la::ScalarAllocationPlan::kRegisterCount - 1> others;
    for (u32 i = 0; i < others.size(); ++i)
        others[i] = e.LoadUniform(ir::Uniform{64 + i * 8, T::U64}).SetType(T::U64);
    for (u32 i = 0; i < others.size(); ++i) e.StoreUniform(ir::Uniform{128 + i * 8, T::U64}, others[i]);
    e.StoreUniform(ir::Uniform{200, T::U64}, e.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64));
    for (u32 i = 0; i < others.size(); ++i) e.StoreUniform(ir::Uniform{256 + i * 8, T::U64}, others[i]);
    evicted->SetTerminal(ir::terminal::ReturnToHost{});
    const auto public_ir = evicted->ToString();
    ForwardCompiled retained{evicted.get(), true}, off{evicted.get(), false};
    const auto& pressure_stats = retained.context.UniformStats();
    Check(pressure_stats.candidates == 1 && pressure_stats.source_spill_rejections == 0 &&
          pressure_stats.spill_growth_rejections == 1 && pressure_stats.loads_forwarded == 0 &&
          pressure_stats.baseline_spilled_values == 0 && pressure_stats.peak_spilled_values == 1 &&
          pressure_stats.final_spilled_values == 0 && retained.context.AllocationStats().spilled_values == 0 &&
          off.context.AllocationStats().spilled_values == 0 && public_ir == evicted->ToString(),
          "register-resident promoted source must not force another value into a new spill");
    for (u32 sample = 0; sample < 16; ++sample) {
        StateStorage actual, control, reference;
        for (u32 offset = 0; offset < 320; offset += 8) {
            const auto value = (u64{sample} << 56) ^ (u64{offset} * 0x11223344556677ULL);
            actual.Put(offset, value); control.Put(offset, value); reference.Put(offset, value);
        }
        backend::interp::Interpreter interpreter{*reference.state, evicted.get()}; const auto halt = interpreter.Run();
        Check(retained.fn(actual.state) == halt && off.fn(control.state) == halt &&
              std::memcmp(actual.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 320) == 0 &&
              std::memcmp(control.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 320) == 0,
              "aggregate spill fallback executes the exact OFF architectural loads and stores");
    }

    auto block = Aliases(); backend::AddressSpace space{TestConfig()};
    for (bool enabled : {false, true}) {
        backend::ModuleConfig policy; policy.feature_overrides.Set(FeatureId::loongarch_uniform_forwarding, enabled);
        auto module = std::make_shared<backend::Module>(space, 0, TestConfig().loc_end, policy);
        ForwardCompiled compiled{block.get(), !enabled, false, la::ExecutionMode::Ordinary, module.get()};
        Check(compiled.context.UsesUniformForwarding() == enabled && compiled.context.UniformStats().enabled == enabled,
              "module uniform policy overrides the preconfigured local switch");
    }
}

u64 ConstantHelper(u64 a, u64 b, u64 c) { return std::rotl(a + b, 17) ^ c; }
void RematerializeConstants() {
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x8900})}; ir::Assembler as{block.get()};
    std::array<ir::Value, 24> constants;
    for (u32 i = 0; i < constants.size(); ++i)
        constants[i] = as.LoadImm(ir::Imm{u64{0x1122334455667700ULL} + i}).SetType(i == 0 ? T::U8 : T::U64);
    auto input = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto total = as.CallHost(&ConstantHelper, constants[0], constants[7], constants[23]).SetType(T::U64);
    for (auto value : constants) total = as.Add(total, ir::Operand{value}).SetType(T::U64);
    total = as.Xor(total, ir::Operand{input}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{32, T::U64}, total);
    // A wider view cannot recover masked bits of a canonical U8 LoadImm.
    as.StoreUniform(ir::Uniform{40, T::U64}, constants[0].SetCastType(T::U64));
    // Narrow raw BitCast/GetResult aliases retain all 128 bits from containers.
    auto raw = as.LoadUniform(ir::Uniform{64, T::V128}).SetType(T::V128);
    auto narrow = as.BitCast(raw).SetType(T::U8);
    as.StoreUniform(ir::Uniform{80, T::V128}, as.BitCast(narrow).SetType(T::V128));
    as.StoreUniform(ir::Uniform{96, T::V128}, as.BitCast(constants[0]).SetType(T::V128));
    block->SetTerminal(ir::terminal::ReturnToHost{});
    const auto source = block->ToString();
    ForwardCompiled optimized{block.get(), false, true}, baseline{block.get(), false, false};
    Check(optimized.context.AllocationStats().rematerialized_values == constants.size() &&
          baseline.context.AllocationStats().rematerialized_values == 0 &&
          optimized.context.AllocationStats().spilled_values < baseline.context.AllocationStats().spilled_values &&
          optimized.context.HomeStats().compact_bytes < baseline.context.HomeStats().compact_bytes,
          "rematerialization removes constant pressure, spills and compact homes");
    Check(optimized.context.ValueStats().stores < baseline.context.ValueStats().stores,
          "rematerialization reduces static SSA stores even with helper/raw canonical publication");
    for (u64 input : {u64{0}, u64{1}, UINT64_MAX, u64{1} << 63}) {
        StateStorage actual, control, reference;
        for (auto* storage : {&actual, &control, &reference}) {
            storage->Put(0, input); storage->Put(64, 0xfedcba9876543210ULL); storage->Put(72, 0x9988776655443322ULL);
        }
        backend::interp::Interpreter interpreter{*reference.state, block.get()}; const auto halt = interpreter.Run();
        Check(optimized.fn(actual.state) == halt && baseline.fn(control.state) == halt &&
              std::memcmp(actual.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 128) == 0 &&
              std::memcmp(control.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 128) == 0,
              "LoadImm helper homes, scalar widening and narrow raw128 aliases retain canonical bits");
        Check(actual.Get(104) == 0 && actual.Get(88) == 0x9988776655443322ULL,
              "constant raw high half is zero while raw narrow alias preserves its nonzero high half");
    }
    Check(source == block->ToString(), "rematerialization preserves source IR after multiple compiled snapshots");
    std::cout << "[loongarch-remat] values=" << optimized.context.AllocationStats().rematerialized_values
              << " spills=" << optimized.context.AllocationStats().spilled_values << '/' << baseline.context.AllocationStats().spilled_values
              << " homes=" << optimized.context.HomeStats().compact_bytes << '/' << baseline.context.HomeStats().compact_bytes
              << " SSA-stores=" << optimized.context.ValueStats().stores << '/' << baseline.context.ValueStats().stores
              << " code-bytes=" << optimized.context.CurrentBufferSize() << '/' << baseline.context.CurrentBufferSize()
              << " translate-ns=" << optimized.translate_ns << '/' << baseline.translate_ns << '\n';
}
}  // namespace

void UniformForwarding() {
    Compare(Aliases().get(), 128); Compare(Join(false).get(), 128); Compare(Join(true).get(), 128); Compare(Loop().get(), 64, true);
    HelpersAndFaults(); RetirementAndStep(); DominanceAndPolicy();
    auto block = Aliases(); ForwardCompiled optimized{block.get(), true}, baseline{block.get(), false};
    std::cout << "[loongarch-uniform] forwarded=" << optimized.context.UniformStats().loads_forwarded
              << " state-loads=" << optimized.context.UniformTraffic().loads << '/' << baseline.context.UniformTraffic().loads
              << " stores=" << optimized.context.UniformTraffic().stores << '/' << baseline.context.UniformTraffic().stores
              << " SSA-loads=" << optimized.context.ValueStats().loads << '/' << baseline.context.ValueStats().loads
              << " spills=" << optimized.context.AllocationStats().spilled_values << '/' << baseline.context.AllocationStats().spilled_values << '\n';
    std::cout << "PASS uniform promotion across CFG joins/backedges, byte aliases, helpers, retirement, fault, Signal/SMC and Step\n";
}
void ScalarRematerialization() {
    RematerializeConstants();
    std::cout << "PASS scalar constant rematerialization with pressure, helpers and raw128 canonical views\n";
}
}  // namespace swift::tests::loongarch64
