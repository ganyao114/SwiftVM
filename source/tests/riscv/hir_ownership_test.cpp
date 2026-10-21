#include "test_support.h"
#include <algorithm>
#include <span>
#include <vector>
#include "runtime/backend/address_space.h"
#include "runtime/backend/runtime.h"
#include "runtime/backend/riscv64/jit/compile.h"
#include "runtime/ir/hir_builder.h"

using namespace swift;
using namespace swift::runtime;
using namespace swift::tests::riscv;
namespace be = swift::runtime::backend;
namespace ir = swift::runtime::ir;

static Config ConfigForFixture() {
    auto config = TestConfig();
    config.uniform_buffer_size = 256;
    return config;
}

static void Complete(ir::HIRFunction* function) {
    function->EndFunction();
    function->ComputeRPO();
    function->IdByRPO();
}

static std::vector<u16> IDs(ir::Block* block) {
    std::vector<u16> ids;
    for (auto& inst : block->GetInstList()) ids.push_back(inst.Id());
    return ids;
}

static void RunOracle(be::AddressSpace& space, u64 location, u64 input, bool body) {
    Runtime runtime{&space};
    auto storage = runtime.GetUniformBuffer();
    std::vector<u8> oracle(storage.size());
    for (size_t i = 0; i < oracle.size(); ++i) oracle[i] = u8(0x80 + (i * 13) % 113);
    std::memcpy(oracle.data(), &input, sizeof(input));
    std::copy(oracle.begin(), oracle.end(), storage.begin());
    if (body) {
        const auto next = input + 7;
        std::memcpy(oracle.data() + 8, &next, sizeof(next));
        std::memcpy(oracle.data() + 16, &input, sizeof(input));
    }
    runtime.SetLocation(location);
    Check(runtime.Run() == HaltReason::CallHost, "native RV standalone entry returns CallHost");
    Check(std::equal(oracle.begin(), oracle.end(), storage.begin()), "native RV complete Uniform oracle after publication");
}

static void BodyAndEmptyPublication() {
    be::AddressSpace space{ConfigForFixture()};
    auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    ir::Block* body_raw{};
    ir::Block* empty_raw{};
    {
        ir::HIRBuilder builder{1, true, features};
        auto* function = builder.AppendFunction(ir::Location{0x200}, ir::Location{0x210});
        auto* hir = function->GetCurrentBlock();
        body_raw = hir->GetBlock();
        auto old = function->LoadUniform(ir::Uniform{0, ir::ValueType::U64}).SetType(ir::ValueType::U64);
        auto next = function->Add(old, ir::Operand{ir::Imm{u64{7}}}).SetType(ir::ValueType::U64);
        function->StoreUniform(ir::Uniform{8, ir::ValueType::U64}, next);
        function->StoreUniform(ir::Uniform{16, ir::ValueType::U64}, old);
        function->EndBlock(ir::terminal::ReturnToHost{});
        auto* retained_hir = function->AppendBlock(ir::Location{0x220}, ir::Location{0x230});
        function->SetCurBlock(retained_hir);
        function->EndBlock(ir::terminal::ReturnToHost{});
        Complete(function);
        auto* retained = retained_hir->GetBlock();
        Check(function->GetFunction()->FindBlock(ir::Location{0x200}) == body_raw,
              "completed Function initially owns the exact body block");
        Check(function->GetFunction()->FindBlock(ir::Location{0x220}) == retained,
              "completed Function initially owns a second decoded block");
        const auto ids = IDs(body_raw);
        auto* code = be::TranslateIR(module, hir);
        Check(code != nullptr, "public TranslateIR publishes and compiles a native RV body");
        Check(function->GetFunction()->FindBlock(ir::Location{0x200}) == nullptr && function->HasStandaloneBlocks(),
              "successful standalone body publication detaches its canonical hook");
        Check(function->GetFunction()->FindBlock(ir::Location{0x220}) == retained,
              "standalone handoff preserves ownership of the other canonical block");
        Check(be::GetBlock(module->GetNode(ir::Location{0x200})).get() == body_raw && IDs(body_raw) == ids,
              "Module owns exact body and publication preserves instruction IDs");
        space.PushCodeCache(ir::Location{0x200}, code);
        RunOracle(space, 0x200, 41, true);
        {
            ir::HIRBuilder nested{1, true, features};
            auto* empty_function = nested.AppendFunction(ir::Location{0x240}, ir::Location{0x250});
            auto* empty_hir = empty_function->GetCurrentBlock();
            empty_raw = empty_hir->GetBlock();
            empty_function->EndBlock(ir::terminal::ReturnToHost{});
            Complete(empty_function);
            Check(empty_raw->GetInstList().empty(), "empty standalone block has no body instructions");
            auto* empty_code = be::TranslateIR(module, empty_hir);
            Check(empty_code != nullptr, "public TranslateIR publishes and compiles an empty native RV body");
            Check(empty_function->GetFunction()->FindBlock(ir::Location{0x240}) == nullptr && empty_function->HasStandaloneBlocks(),
                  "empty standalone handoff commits its canonical ownership detach");
            space.PushCodeCache(ir::Location{0x240}, empty_code);
            RunOracle(space, 0x240, 91, false);
        }
        Check(be::GetBlock(module->GetNode(ir::Location{0x240})).get() == empty_raw,
              "Module retains exact empty block after the nested builder is destroyed");
        RunOracle(space, 0x240, 101, false);
    }
    Check(be::GetBlock(module->GetNode(ir::Location{0x200})).get() == body_raw &&
          be::GetBlock(module->GetNode(ir::Location{0x240})).get() == empty_raw,
          "Module retains both exact standalone blocks after all HIR builders are destroyed");
    Check(be::IsEmpty(module->GetNode(ir::Location{0x220})), "unpublished remaining block was never inserted in Module");
    RunOracle(space, 0x200, 51, true);
    RunOracle(space, 0x240, 111, false);
    std::cout << "PASS RV native body and empty HIR publication after nested builder destruction\n";
}

static void RollbackRetryAndPartialPreflight() {
    be::AddressSpace space{ConfigForFixture()};
    auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    ir::Block* raw{};
    {
        ir::HIRBuilder builder{1, true, features};
        auto* function = builder.AppendFunction(ir::Location{0x300}, ir::Location{0x310});
        auto* hir = function->GetCurrentBlock();
        raw = hir->GetBlock();
        function->EndBlock(ir::terminal::ReturnToHost{});
        Complete(function);
        IntrusivePtr<ir::Block> occupied{new ir::Block(0, ir::Location{0x300})};
        occupied->SetTerminal(ir::terminal::ReturnToHost{});
        Check(!function->GetFunction()->DetachBlock(occupied.get()), "same-location foreign pointer cannot detach the canonical exact node");
        Check(module->Push(occupied.get()), "Module accepts the same-location occupied placeholder");
        Check(be::TranslateIR(module, hir) == nullptr, "occupied address causes standalone Push rejection");
        Check(function->GetFunction()->FindBlock(ir::Location{0x300}) == raw && !function->HasStandaloneBlocks(),
              "failed Push restores exact canonical hook and ownership state");
        Check(be::GetBlock(module->GetNode(ir::Location{0x300})).get() == occupied.get() &&
              module->GetJitCache(ir::Location{0x300}) == nullptr,
              "failed publication preserves placeholder and publishes no JIT entry");
        module->Remove(occupied.get());
        auto* code = be::TranslateIR(module, hir);
        Check(code != nullptr, "standalone retry succeeds after removing only the placeholder");
        space.PushCodeCache(ir::Location{0x300}, code);
        Check(function->GetFunction()->FindBlock(ir::Location{0x300}) == nullptr && function->HasStandaloneBlocks(),
              "retry commits canonical detach exactly once");
        const auto cache = module->GetJitCache(ir::Location{0x300});
        Check(cache != nullptr && be::GetBlock(module->GetNode(ir::Location{0x300})).get() == raw,
              "retry publishes an entry owned by the exact standalone block");
        Check(be::TranslateIR(module, hir) == nullptr, "duplicate standalone HIR wrapper publication is rejected");
        Check(be::TranslateIR(module, function) == nullptr, "common single partial-Function compile rejects before emission");
        Check(!be::PublishIRFunction(module, function), "partial-Function publication is rejected");
        const std::array<ir::HIRFunction*, 1> single{{function}};
        Check(be::TranslateIR(module, std::span<ir::HIRFunction* const>{single}) == nullptr,
              "common Function span rejects a partial Function");
        Check(be::riscv64::CompileFunctions(module, std::span<ir::HIRFunction* const>{single}) == nullptr,
              "direct RV Function span rejects a partial Function");
        ir::HIRBuilder valid_builder{1, true, features};
        auto* valid = valid_builder.AppendFunction(ir::Location{0x320}, ir::Location{0x330});
        auto* valid_raw = valid->GetCurrentBlock()->GetBlock();
        valid->EndBlock(ir::terminal::ReturnToHost{});
        Complete(valid);
        const auto preserved = [&] {
            Check(be::IsEmpty(module->GetNode(ir::Location{0x320})) && module->GetJitCache(ir::Location{0x320}) == nullptr &&
                  valid->GetFunction()->FindBlock(ir::Location{0x320}) == valid_raw && !valid->HasStandaloneBlocks(),
                  "all-input preflight preserves the preceding valid Function owner and publishes no entry");
            Check(be::GetBlock(module->GetNode(ir::Location{0x300})).get() == raw &&
                  module->GetJitCache(ir::Location{0x300}) == cache && function->HasStandaloneBlocks(),
                  "preflight preserves the previously committed standalone node and JIT entry");
        };
        for (const auto& regions : {std::array<ir::HIRFunction*, 2>{{valid, function}},
                                   std::array<ir::HIRFunction*, 2>{{function, valid}},
                                   std::array<ir::HIRFunction*, 2>{{valid, nullptr}}}) {
            Check(be::TranslateIR(module, std::span<ir::HIRFunction* const>{regions}) == nullptr,
                  "common multi-Function span preflights every partial/null input");
            preserved();
            Check(be::riscv64::CompileFunctions(module, std::span<ir::HIRFunction* const>{regions}) == nullptr,
                  "direct RV multi-Function span preflights every partial/null input before recursion");
            preserved();
        }
    }
    Check(be::GetBlock(module->GetNode(ir::Location{0x300})).get() == raw,
          "rollback/retry block survives destruction of both canonical HIR owners");
    RunOracle(space, 0x300, 121, false);
    std::cout << "PASS RV exact-node failed Push retry duplicate and all-input partial preflight\n";
}

static void UnfinishedAndPublishedOwners() {
    be::AddressSpace space{ConfigForFixture()};
    auto module = space.GetDefaultModule();
    const auto features = ResolveFeatureSet(module->GetModuleConfig());
    ir::Block* raw{};
    ir::Function* canonical{};
    {
        ir::HIRBuilder builder{1, true, features};
        auto* function = builder.AppendFunction(ir::Location{0x340}, ir::Location{0x350});
        auto* hir = function->GetCurrentBlock();
        raw = hir->GetBlock();
        canonical = function->GetFunction();
        function->EndBlock(ir::terminal::ReturnToHost{});
        Check(be::TranslateIR(module, hir) == nullptr, "unfinished Function rejects standalone publication");
        Check(be::IsEmpty(module->GetNode(ir::Location{0x340})) && !function->HasStandaloneBlocks() &&
              function->GetFunction() == canonical && hir->GetBlock() == raw && raw->HasTerminal(),
              "unfinished rejection preserves HIR block identity terminal and owner state");
        Complete(function);
        Check(canonical->FindBlock(ir::Location{0x340}) == raw, "completion transfers the same retained block to the canonical Function");
        Check(be::PublishIRFunction(module, function), "public Function publication succeeds before standalone admission attempt");
        Check(be::TranslateIR(module, hir) == nullptr, "already published Function owner rejects standalone admission");
        Check(!function->HasStandaloneBlocks() && canonical->FindBlock(ir::Location{0x340}) == raw &&
              be::GetFunction(module->GetNode(ir::Location{0x340})).get() == canonical &&
              module->GetJitCache(ir::Location{0x340}) == nullptr,
              "published rejection preserves exact Function and Block ownership without emitting code");
    }
    Check(be::GetFunction(module->GetNode(ir::Location{0x340})).get() == canonical &&
          canonical->FindBlock(ir::Location{0x340}) == raw && raw->HasTerminal(),
          "published Function and canonical Block survive the HIR builder destructor");
    std::cout << "PASS RV unfinished and published Function ownership unchanged\n";
}

int main() {
    try {
        BodyAndEmptyPublication();
        RollbackRetryAndPartialPreflight();
        UnfinishedAndPublishedOwners();
        std::cout << "PASS RV R89 standalone HIR ownership 3 cases " << checks << " checks\n";
    } catch (const std::exception& error) {
        std::cerr << "FAIL RV R89 HIR ownership: " << error.what() << '\n';
        return 1;
    }
}
