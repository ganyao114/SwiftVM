#include "test_support.h"

#include <bit>
#include "runtime/backend/address_space.h"
#include "runtime/backend/module.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
namespace {
using T = ir::ValueType;

// Callbacks observe canonical memory and may replace it. The resident r31
// must be published before the first observation and reloaded after mutation.
u64 ObserveFlags(u64 address) { return reinterpret_cast<backend::State*>(address)->host_cpu_flags; }
u64 MutateFlags(u64 address) {
    auto& flags = reinterpret_cast<backend::State*>(address)->host_cpu_flags;
    flags ^= u64{1} << 28;
    return flags;
}
struct RangeObservation {
    backend::State* state;
    u64 observed{};
    u64 calls{};
    bool allow{true};
};
bool ObserveAndMutateRange(void* context, u64, u64) {
    auto& observation = *static_cast<RangeObservation*>(context);
    observation.observed = observation.state->host_cpu_flags;
    ++observation.calls;
    observation.state->host_cpu_flags ^= u64{1} << 28;
    return observation.allow;
}

struct FlagsCompiled {
    Config config{TestConfig()};
    la::JitContext context;
    la::JitTranslator translator{context};
    backend::CodeCache cache{config, 1u << 20, FeatureSet{}};
    BlockFn fn{};
    FlagsCompiled(ir::Block* block, bool optimize, bool cache_values, backend::Module* owner = nullptr)
        : context(cache_values) {
        context.SetFlagsPublication(optimize);
        if (owner) context.ConfigureModule(owner);
        translator.Translate(block);
        auto buffer = cache.AllocCode(context.CurrentBufferSize());
        Check(buffer.has_value(), "allocate flags publication differential code");
        context.Flush(*buffer); fn = reinterpret_cast<BlockFn>(buffer->exec_data);
    }
};

IntrusivePtr<ir::Block> Straight() {
    IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xb000}}};
    ir::Assembler as{block.get()};
    auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto carry = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    const auto observe = [&](u32 offset, bool mutate = false) {
        auto value = as.CallHost(mutate ? &MutateFlags : &ObserveFlags, address).SetType(T::U64);
        as.StoreUniform(ir::Uniform{offset, T::U64}, value);
    };
    observe(32);
    as.SetCarry(carry); observe(40);
    observe(48, true); observe(56);
    as.ClearFlags(ir::Flags::NZCV); observe(64); observe(72);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

IntrusivePtr<ir::Block> Joined() {
    IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xb100}}};
    ir::Assembler as{block.get()};
    auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto condition = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto branch = as.Goto(ir::BOOL{condition});
    as.SetCarry(condition); as.InvertCarry();
    as.BindLabel(branch);
    auto snapshot = as.CallHost(&ObserveFlags, address).SetType(T::U64);
    as.StoreUniform(ir::Uniform{32, T::U64}, snapshot);
    snapshot = as.CallHost(&ObserveFlags, address).SetType(T::U64);
    as.StoreUniform(ir::Uniform{40, T::U64}, snapshot);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

IntrusivePtr<ir::Block> Loop() {
    IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xb200}}};
    ir::Assembler as{block.get()};
    auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto count = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto snapshot = as.CallHost(&ObserveFlags, address).SetType(T::U64);
    as.StoreUniform(ir::Uniform{32, T::U64}, snapshot);
    // The first callback synchronizes resident flags even when the backedge
    // reaches it dirty. A second observer tests a genuinely clean boundary.
    auto synchronized = as.CallHost(&ObserveFlags, address).SetType(T::U64);
    as.StoreUniform(ir::Uniform{48, T::U64}, synchronized);
    as.InvertCarry();
    auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{8, T::U64}, next);
    auto branch = as.Goto(ir::BOOL{as.TestNotZero(next)});
    as.BindLabel(branch);
    auto* label = &block->GetInstList().back();
    block->RemoveInst(label); block->InsertBefore(label, address.Def());
    snapshot = as.CallHost(&ObserveFlags, address).SetType(T::U64);
    as.StoreUniform(ir::Uniform{40, T::U64}, snapshot);
    block->SetTerminal(ir::terminal::ReturnToHost{}); block->ReIdInstr();
    return block;
}

IntrusivePtr<ir::Block> GuardedFP() {
    IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xb300}}};
    ir::Assembler as{block.get()};
    auto address = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto carry = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    auto left = as.LoadUniform(ir::Uniform{16, T::V128}).SetType(T::V128);
    auto right = as.LoadUniform(ir::Uniform{80, T::V128}).SetType(T::V128);
    as.SetCarry(carry);
    auto result = as.VecFAddScalar64(left, right).SetType(T::V128);
    as.StoreUniform(ir::Uniform{96, T::V128}, result);
    auto snapshot = as.CallHost(&ObserveFlags, address).SetType(T::U64);
    as.StoreUniform(ir::Uniform{32, T::U64}, snapshot);
    snapshot = as.CallHost(&ObserveFlags, address).SetType(T::U64);
    as.StoreUniform(ir::Uniform{40, T::U64}, snapshot);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    return block;
}

void Initialize(StateStorage& state, u64 seed, u64 input, bool special_fp) {
    state.Put(0, reinterpret_cast<u64>(state.state)); state.Put(8, input);
    state.Put(16, special_fp ? 0x7ff8000000000031ULL : std::bit_cast<u64>(1.25));
    state.Put(24, std::rotl(seed, 19));
    state.Put(80, std::bit_cast<u64>(2.5)); state.Put(88, std::rotl(seed, 37));
    state.state->host_cpu_flags = seed;
}

void ModulePolicy() {
    Check(FeatureIdFromName("loongarch_flags_publication") == FeatureId::loongarch_flags_publication,
          "flags publication feature is addressable by its stable configuration name");
    auto on_features = GetSvmConfig().GetFeatureSet(), off_features = on_features;
    on_features.loongarch_flags_publication = true; off_features.loongarch_flags_publication = false;
    Check(HashFeatureSet(on_features) != HashFeatureSet(off_features),
          "flags publication ON/OFF changes the feature fingerprint");
    auto block = Straight();
    auto config = TestConfig();
    backend::AddressSpace space{config};
    std::array<std::shared_ptr<backend::Module>, 2> modules;
    for (bool enabled : {false, true}) {
        backend::ModuleConfig policy;
        policy.feature_overrides.Set(FeatureId::loongarch_flags_publication, enabled);
        modules[enabled] = std::make_shared<backend::Module>(space, 0, config.loc_end, policy);
    }
    FlagsCompiled enabled{block.get(), false, true, modules[1].get()};
    FlagsCompiled disabled{block.get(), true, true, modules[0].get()};
    Check(enabled.context.FlagStats().skipped_stores > 0 && disabled.context.FlagStats().skipped_stores == 0 &&
          enabled.context.FlagStats().stores < disabled.context.FlagStats().stores,
          "module feature policy overrides the preconfigured publication mode");
    for (u32 sample = 0; sample < 16; ++sample) {
        StateStorage a, b;
        const u64 flags = std::rotl(0x123456789abcdef0ULL, sample);
        Initialize(a, flags, sample & 1, false); Initialize(b, flags, sample & 1, false);
        Check(enabled.fn(a.state) == disabled.fn(b.state) && a.state->host_cpu_flags == b.state->host_cpu_flags &&
              std::memcmp(a.state->uniform_buffer_begin + 8, b.state->uniform_buffer_begin + 8, 104) == 0,
              "module publication ON/OFF preserve callback and complete state snapshots");
    }
}

void NativeMemoryObserver() {
    IntrusivePtr<ir::Block> block{new ir::Block{ir::Location{0xb400}}};
    ir::Assembler as{block.get()};
    auto state_pointer = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    auto input = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
    as.SetCarry(input);
    auto loaded = as.LoadMemory(ir::Operand{ir::Imm{u64{16}}}).SetType(T::U64);
    as.StoreUniform(ir::Uniform{32, T::U64}, loaded);
    auto snapshot = as.CallHost(&ObserveFlags, state_pointer).SetType(T::U64);
    as.StoreUniform(ir::Uniform{40, T::U64}, snapshot);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    for (bool cached : {false, true}) {
        FlagsCompiled optimized{block.get(), true, cached}, baseline{block.get(), false, cached};
        Check(optimized.context.FlagStats().skipped_stores > 0 &&
              optimized.context.FlagStats().stores < baseline.context.FlagStats().stores,
              "native memory reload makes the next read-only callback publication redundant");
        for (bool allowed : {false, true})
        for (u32 sample = 0; sample < 16; ++sample) {
            std::array<u64, 16> guest{}; guest[2] = 0x4d656d6f7279 + sample;
            StateStorage a, b;
            const u64 original = std::rotl(0x123456789abcdef0ULL, sample);
            RangeObservation oa{a.state}, ob{b.state}; oa.allow = ob.allow = allowed;
            for (auto* state : {&a, &b}) {
                Initialize(*state, original, sample & 1, false);
                state->state->pt = guest.data();
                state->state->guest_addr_mask = guest.size() * 8 - 1;
                state->state->guest_addr_limit = guest.size() * 8;
                state->state->interp_range_check = &ObserveAndMutateRange;
            }
            a.state->interp_range_check_ctx = &oa; b.state->interp_range_check_ctx = &ob;
            const auto published = (original & ~(u64{1} << 29)) | (u64(sample & 1) << 29);
            const auto after_callback = published ^ (u64{1} << 28);
            const auto halt = allowed ? HaltReason::CallHost : HaltReason::PageFatal;
            Check(optimized.fn(a.state) == halt && baseline.fn(b.state) == halt,
                  "native range callback preserves approved and rejected memory halt");
            Check(oa.calls == 1 && ob.calls == 1 && oa.observed == published && ob.observed == published,
                  "native range callback observes dirty flags published before guest validation");
            Check(a.state->host_cpu_flags == after_callback && b.state->host_cpu_flags == after_callback,
                  "native range callback flag mutation survives access and rejected-access early return");
            if (allowed) Check(a.Get(32) == guest[2] && b.Get(32) == guest[2] &&
                               a.Get(40) == after_callback && b.Get(40) == after_callback,
                               "native memory callback reload is visible to the following host observer");
        }
    }
}
}  // namespace

void FlagsPublications() {
    const auto initial_checks = checks;
    ModulePolicy();
    NativeMemoryObserver();
    u64 seed = 0x5075626c697368ULL;
    const std::array<const char*, 4> names{"straight_helpers", "dirty_join", "dirty_backedge", "guarded_fp"};
    for (u32 workload = 0; workload < names.size(); ++workload) {
        auto block = workload == 0 ? Straight() : workload == 1 ? Joined() : workload == 2 ? Loop() : GuardedFP();
        for (bool cached : {false, true}) {
            FlagsCompiled optimized{block.get(), true, cached}, baseline{block.get(), false, cached};
            const auto& on = optimized.context.FlagStats();
            const auto& off = baseline.context.FlagStats();
            Check(off.skipped_stores == 0 && on.skipped_stores > 0 && on.stores < off.stores,
                  std::string{names[workload]} + " omits synchronized publications and preserves the forced epilogue store");
            Check(on.loads == off.loads && optimized.context.CurrentBufferSize() < baseline.context.CurrentBufferSize(),
                  std::string{names[workload]} + " removes flag stores without hiding loads or increasing code size");
            std::cout << "FLAGS-PUBLICATION {\"workload\":\"" << names[workload] << "\",\"cached\":"
                      << (cached ? "true" : "false") << ",\"on_stores\":" << on.stores
                      << ",\"off_stores\":" << off.stores << ",\"skipped_stores\":" << on.skipped_stores
                      << ",\"on_bytes\":" << optimized.context.CurrentBufferSize()
                      << ",\"off_bytes\":" << baseline.context.CurrentBufferSize() << "}\n";
            for (u32 sample = 0; sample < 64; ++sample) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                StateStorage a, b, reference;
                const auto input = workload == 2 ? 1 + sample % 31 : sample & 1;
                for (auto* state : {&a, &b, &reference}) Initialize(*state, seed, input, (sample & 1) != 0);
                backend::interp::Interpreter interpreter{*reference.state, block.get()};
                const auto wanted = interpreter.Run();
                Check(optimized.fn(a.state) == wanted && baseline.fn(b.state) == wanted,
                      "flags publication ON/OFF preserve interpreter halt");
                Check(a.state->host_cpu_flags == reference.state->host_cpu_flags &&
                      b.state->host_cpu_flags == reference.state->host_cpu_flags &&
                      std::memcmp(a.state->uniform_buffer_begin + 8, reference.state->uniform_buffer_begin + 8, 104) == 0 &&
                      std::memcmp(b.state->uniform_buffer_begin + 8, reference.state->uniform_buffer_begin + 8, 104) == 0,
                      "flags publication ON/OFF match callbacks, canonical flags and uniform snapshots");
                if (sample == 0) {
                    Initialize(a, seed, input, false); Initialize(b, seed, input, false);
                    Check(CheckABI(optimized.fn, a.state) == 1 && CheckABI(baseline.fn, b.state) == 1,
                          "flags publication ON/OFF preserve the complete LoongArch C ABI");
                }
            }
        }
    }
    std::cout << "PASS software flags publication ON/OFF " << checks - initial_checks << " checks\n";
}

}  // namespace swift::tests::loongarch64
