#include "test_support.h"

#include <bit>
#include "runtime/backend/address_space.h"
#include "runtime/backend/interp/interpreter.h"
#include "runtime/frontend/ir_assembler.h"

namespace swift::tests::loongarch64 {
namespace {
using T = ir::ValueType;
using M = la::ScalarAllocationMode;

u64 Callback(u64 a, u64 b, u64 c) { return std::rotl(a + b, 17) ^ c; }

void CompareModes(ir::Block* block, bool loop = false) {
    u64 seed = 0x157676396eff9eb5ULL;
    for (auto mode : {M::CanonicalHomes, M::LocalCache, M::GlobalCFG}) {
        Compiled compiled{block, true, la::HostFeatures::Detect(), nullptr, mode};
        Check(compiled.context.AllocationMode() == mode, "explicit allocation mode is preserved without module policy");
        if (mode == M::GlobalCFG)
            Check(compiled.context.AllocationStats().register_values > 0,
                  "execution fixture actually exercises globally allocated scalars");
        for (u32 sample = 0; sample < 128; ++sample) {
            StateStorage actual, reference;
            for (u32 offset = 0; offset < 192; offset += 8) {
                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                const u64 value = sample == 0 ? 0 : sample == 1 ? UINT64_MAX : sample == 2 ? u64{1} << 63 : seed;
                actual.Put(offset, value); reference.Put(offset, value);
            }
            if (loop) { actual.Put(0, 1 + sample % 63); reference.Put(0, 1 + sample % 63); }
            actual.state->host_cpu_flags = reference.state->host_cpu_flags = std::rotl(seed, 21);
            backend::interp::Interpreter interpreter{*reference.state, block};
            const auto halt = interpreter.Run();
            Check(compiled.fn(actual.state) == halt &&
                  std::memcmp(actual.state->uniform_buffer_begin, reference.state->uniform_buffer_begin, 512) == 0 &&
                  actual.state->host_cpu_flags == reference.state->host_cpu_flags,
                  "scalar allocation execution matches interpreter mode=" + std::to_string(u32(mode)) +
                  " sample=" + std::to_string(sample) + " loop=" + std::to_string(loop));
            if (!loop && sample % 16 == 0)
                Check(CheckABI(compiled.fn, actual.state) == 1,
                      "all allocation modes preserve the complete LP64D ABI across pressure and helpers");
        }
    }
}

void ModulePolicy() {
    backend::AddressSpace space{TestConfig()};
    IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7500})};
    ir::Assembler as{block.get()};
    auto value = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
    for (u64 i = 0; i < 32; ++i) value = as.Add(value, ir::Operand{ir::Imm{i + 1}}).SetType(T::U64);
    as.SaveFlags(value, ir::Flags::All);
    as.StoreUniform(ir::Uniform{192, T::U64}, value);
    block->SetTerminal(ir::terminal::ReturnToHost{});
    for (bool allow : {false, true}) {
        backend::ModuleConfig policy;
        policy.feature_overrides.Set(FeatureId::loongarch_cfg_allocation, allow);
        auto module = std::make_shared<backend::Module>(space, 0, TestConfig().loc_end, policy);
        Compiled compiled{block.get(), true, la::HostFeatures::Detect(), module.get(), M::GlobalCFG};
        Check(compiled.context.AllocationMode() == (allow ? M::GlobalCFG : M::LocalCache),
              "module CFG allocation override selects global allocation or local-cache rollback");
        for (u64 input : {u64{0}, u64{1}, UINT64_MAX, u64{1} << 63}) {
            StateStorage actual, reference;
            actual.Put(0, input); reference.Put(0, input);
            actual.state->host_cpu_flags = reference.state->host_cpu_flags = (u64{1} << 53) | 0xa5;
            backend::interp::Interpreter interpreter{*reference.state, block.get()};
            const auto halt = interpreter.Run();
            Check(compiled.fn(actual.state) == halt && actual.Get(192) == reference.Get(192) &&
                  actual.state->host_cpu_flags == reference.state->host_cpu_flags,
                  "module CFG allocation policy preserves the result and flags");
        }
    }
    const auto named = FeatureIdFromName("loongarch_cfg_allocation");
    Check(named && *named == FeatureId::loongarch_cfg_allocation, "CFG allocation feature has a stable serialized name");
    FeatureSet disabled, enabled;
    disabled.Set(FeatureId::loongarch_cfg_allocation, false);
    enabled = disabled; enabled.Set(FeatureId::loongarch_cfg_allocation, true);
    Check(HashFeatureSet(disabled) != HashFeatureSet(enabled), "CFG allocation feature is included in cache compatibility hash");
}

}  // namespace

void ScalarAllocationExecution() {
    {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7300})};
        ir::Assembler as{block.get()};
        std::array<ir::Value, 24> values;
        for (u32 i = 0; i < values.size(); ++i)
            values[i] = as.LoadUniform(ir::Uniform{i * 8, T::U64}).SetType(T::U64);
        auto sum = as.Add(values[0], ir::Operand{values[23]}).SetType(T::U64);
        // The pseudo reads the producer's old flags after unrelated updates,
        // so its implicit scalar producer inputs must remain available.
        auto early_flags = as.GetFlags(sum, ir::Flags::All).SetType(T::U64);
        auto called = as.CallHost(&Callback, sum, values[11], values[23]).SetType(T::U64);
        auto joined = called;
        for (auto value : values) joined = as.Xor(joined, ir::Operand{value}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{192, T::U64}, joined);
        as.StoreUniform(ir::Uniform{200, T::U64}, early_flags);
        as.SaveFlags(sum, ir::Flags::Carry | ir::Flags::Overflow);
        auto carried = as.Adc(joined, ir::Operand{values[7]}).SetType(T::U64);
        as.SaveFlags(carried, ir::Flags::All);
        as.StoreUniform(ir::Uniform{208, T::U64}, carried);
        auto vector = as.LoadUniform(ir::Uniform{0, T::V128}).SetType(T::V128);
        auto narrow = as.BitCast(vector).SetType(T::U8);
        auto narrow_arithmetic = as.Add(narrow, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U8);
        as.StoreUniform(ir::Uniform{224, T::U8}, narrow_arithmetic);
        as.StoreUniform(ir::Uniform{240, T::V128}, as.BitCast(narrow).SetType(T::V128));
        block->SetTerminal(ir::terminal::ReturnToHost{});
        CompareModes(block.get());
    }
    {
        IntrusivePtr<ir::Block> block{new ir::Block(ir::Location{0x7400})};
        ir::Assembler as{block.get()};
        auto anchor = as.LoadUniform(ir::Uniform{16, T::U64}).SetType(T::U64);
        auto count = as.LoadUniform(ir::Uniform{0, T::U64}).SetType(T::U64);
        auto accumulated = as.LoadUniform(ir::Uniform{8, T::U64}).SetType(T::U64);
        auto sum = as.Add(accumulated, ir::Operand{anchor}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{8, T::U64}, sum);
        auto next = as.Sub(count, ir::Operand{ir::Imm{u64{1}}}).SetType(T::U64);
        as.StoreUniform(ir::Uniform{0, T::U64}, next);
        as.SaveFlags(next, ir::Flags::All);
        auto edge = as.Goto(as.TestNotZero(next));
        as.BindLabel(edge);
        auto* label = &block->GetInstList().back();
        block->RemoveInst(label); block->InsertBefore(label, count.Def());
        as.StoreUniform(ir::Uniform{192, T::U64}, sum);
        as.StoreUniform(ir::Uniform{200, T::U64}, anchor);
        block->SetTerminal(ir::terminal::ReturnToHost{}); block->ReIdInstr();
        CompareModes(block.get(), true);
    }
    ModulePolicy();
    std::cout << "PASS executed global/local/home scalar allocation, pressure, flags, raw bits, loops and module rollback\n";
}

}  // namespace swift::tests::loongarch64
